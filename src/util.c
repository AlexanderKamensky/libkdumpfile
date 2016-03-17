/** @internal @file src/util.c
 * @brief Utility functions.
 */
/* Copyright (C) 2014 Petr Tesarik <ptesarik@suse.cz>

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   libkdumpfile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE

#include "kdumpfile-priv.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#if USE_ZLIB
# include <zlib.h>
#endif

kdump_status
set_error(kdump_ctx *ctx, kdump_status ret, const char *msgfmt, ...)
{
	static const char failure[] = "(set_error failed)";
	static const char delim[] = { ':', ' ' };
	static const char ellipsis[] = { '.', '.', '.' };

	va_list ap;
	char msgbuf[ERRBUF];
	const char *msg, *err;
	int msglen;
	size_t remain;

	if (ret == kdump_ok)
		return ret;

	va_start(ap, msgfmt);
	msglen = vsnprintf(msgbuf, sizeof(msgbuf), msgfmt, ap);
	va_end(ap);

	if (msglen < 0) {
		msg = failure;
		msglen = sizeof(failure) - 1;
	} else {
		msg = msgbuf;
		if (msglen >= sizeof(msgbuf))
			msglen = sizeof(msgbuf) - 1;
		if (ret == kdump_syserr && !ctx->err_str &&
		    msglen + 3 < sizeof(msgbuf)) {
			memcpy(msgbuf + msglen, delim, sizeof(delim));
			msglen += sizeof(delim);
			remain = sizeof(msgbuf) - msglen;
			err = strerror_r(errno, msgbuf + msglen, remain);
			remain = sizeof(msgbuf) - msglen - 1;
			strncpy(msgbuf + msglen, err, remain);
			msgbuf[msglen + remain] = '\0';
			msglen += strlen(msgbuf + msglen);
		}
	}

	if (!ctx->err_str) {
		ctx->err_str = ctx->err_buf + sizeof(ctx->err_buf) - 1;
		*ctx->err_str = '\0';
		remain = sizeof(ctx->err_buf) - 1;
	} else {
		remain = ctx->err_str - ctx->err_buf;
		if (remain >= sizeof(delim)) {
			ctx->err_str -= sizeof(delim);
			memcpy(ctx->err_str, delim, sizeof(delim));
			remain -= sizeof(delim);
		}
	}

	if (remain >= msglen) {
		ctx->err_str -= msglen;
		memcpy(ctx->err_str, msg, msglen);
	} else {
		ctx->err_str = ctx->err_buf;
		memcpy(ctx->err_str, msg + msglen - remain, remain);
		memcpy(ctx->err_str, ellipsis, sizeof(ellipsis));
	}

	return ret;
}

void *
ctx_malloc(size_t size, kdump_ctx *ctx, const char *desc)
{
	void *ret = malloc(size);
	if (!ret)
		set_error(ctx, kdump_syserr,
			  "Cannot allocate %s (%zu bytes)", desc, size);
	return ret;
}

static inline void
add_to_hash(unsigned long *hash, unsigned long x)
{
	*hash += x;
	*hash *= 9;
}

unsigned long
mem_hash(const char *s, size_t len)
{
	unsigned long hash = 0;

	while (len >= sizeof(unsigned long)) {
		add_to_hash(&hash, *(unsigned long*)s);
		s += sizeof(unsigned long);
		len -= sizeof(unsigned long);
	}
	while (len--)
		hash += (unsigned long)*s++ << (8 * len);
	return hash;
}

unsigned long
string_hash(const char *s)
{
	return mem_hash(s, strlen(s));
}

/**  Update a partial hash with a memory area.
 * @param[in,out] phash  Partial hash state.
 * @param[in]     s      Start of memory area with new data to be hashed.
 * @param[in]     len    Number of bytes at @ref s to be hashed.
 */
void
phash_update(struct phash *ph, const char *s, size_t len)
{
	if (ph->idx) {
		while (len && ph->idx < sizeof(unsigned long)) {
			ph->part.bytes[ph->idx] = *s++;
			--len;
			++ph->idx;
		}
		if (ph->idx >= sizeof(unsigned long)) {
			add_to_hash(&ph->val, ph->part.num);
			ph->idx = 0;
		}
	}

	while (len >= sizeof(unsigned long)) {
		add_to_hash(&ph->val, *(unsigned long*)s);
		s += sizeof(unsigned long);
		len -= sizeof(unsigned long);
	}
	while (len--)
		ph->part.bytes[ph->idx++] = *s++;
}

static size_t
arch_ptr_size(enum kdump_arch arch)
{
	switch (arch) {
	case ARCH_ALPHA:
	case ARCH_IA64:
	case ARCH_PPC64:
	case ARCH_S390X:
	case ARCH_X86_64:
		return 8;	/* 64 bits */

	case ARCH_ARM:
	case ARCH_PPC:
	case ARCH_MIPS:
	case ARCH_S390:
	case ARCH_X86:
	default:
		return 4;	/* 32 bits */
	}

}

static enum kdump_arch
machine_arch(const char *machine)
{
	if (!strcmp(machine, "alpha"))
		return ARCH_ALPHA;
	else if (!strcmp(machine, "ia64"))
		return ARCH_IA64;
	else if (!strcmp(machine, "mips"))
		return ARCH_MIPS;
	else if (!strcmp(machine, "ppc"))
		return ARCH_PPC;
	else if (!strcmp(machine, "ppc64") ||
		 !strcmp(machine, "ppc64le"))
		return ARCH_PPC64;
	else if (!strcmp(machine, "s390"))
		return ARCH_S390;
	else if (!strcmp(machine, "s390x"))
		return ARCH_S390X;
	else if (!strcmp(machine, "i386") ||
		 !strcmp(machine, "i586") ||
		 !strcmp(machine, "i686"))
		return ARCH_X86;
	else if (!strcmp(machine, "x86_64"))
		return ARCH_X86_64;
	else if (!strcmp(machine, "arm64") ||
		 !strcmp(machine, "aarch64"))
		return ARCH_AARCH64;
	else if (!strncmp(machine, "arm", 3))
		return ARCH_ARM;
	else
		return ARCH_UNKNOWN;
}

static size_t
default_page_shift(enum kdump_arch arch)
{
	static const int page_shifts[] = {
		[ARCH_AARCH64] = 0,
		[ARCH_ALPHA]= 13,
		[ARCH_ARM] = 12,
		[ARCH_IA64] = 0,
		[ARCH_MIPS] = 12,
		[ARCH_PPC] = 0,
		[ARCH_PPC64] = 0,
		[ARCH_S390] = 12,
		[ARCH_S390X] = 12,
		[ARCH_X86] = 12,
		[ARCH_X86_64] = 12,
	};

	if (arch < ARRAY_SIZE(page_shifts))
		return page_shifts[arch];
	return 0;
}

static const struct arch_ops*
arch_ops(enum kdump_arch arch)
{
	switch (arch) {
	case ARCH_AARCH64:
	case ARCH_ALPHA:
	case ARCH_ARM:
	case ARCH_IA64:
	case ARCH_MIPS:
	case ARCH_PPC:
	case ARCH_S390:
		/* TODO */
		break;

	case ARCH_PPC64:	return &ppc64_ops;
	case ARCH_S390X:	return &s390x_ops;
	case ARCH_X86:		return &ia32_ops;
	case ARCH_X86_64:	return &x86_64_ops;

	default:
		break;
	}

	return NULL;
}

static const char *
arch_name(enum kdump_arch arch)
{
	static const char *const names[] = {
		[ARCH_AARCH64] = "aarch64",
		[ARCH_ALPHA] = "alpha",
		[ARCH_ARM] = "arm",
		[ARCH_IA64] = "ia64",
		[ARCH_MIPS] = "mips",
		[ARCH_PPC] = "ppc",
		[ARCH_PPC64] = "ppc64",
		[ARCH_S390] = "s390",
		[ARCH_S390X] = "s390x",
		[ARCH_X86] = "i386",
		[ARCH_X86_64] = "x86_64",
	};
	if (arch < ARRAY_SIZE(names))
		return names[arch];
	return NULL;
}

kdump_status
set_arch(kdump_ctx *ctx, enum kdump_arch arch)
{
	if (!isset_page_size(ctx)) {
		int page_shift = default_page_shift(arch);
		if (!page_shift)
			return set_error(ctx, kdump_unsupported,
					 "No default page size");
		set_page_shift(ctx, page_shift);
	}

	ctx->arch_ops = arch_ops(arch);

	set_ptr_size(ctx, arch_ptr_size(arch));
	set_arch_name(ctx, arch_name(arch));

	if (ctx->arch_ops && ctx->arch_ops->init)
		return ctx->arch_ops->init(ctx);

	return kdump_ok;
}

static kdump_status
uts_machine_post_hook(kdump_ctx *ctx, struct attr_data *attr)
{
	enum kdump_arch arch;

	if (isset_arch_name(ctx))
		return kdump_ok;

	arch = machine_arch(attr_value(attr)->string);
	return arch != ARCH_UNKNOWN
		? set_arch(ctx, arch)
		: kdump_ok;
}

const struct attr_ops uts_machine_ops = {
	.post_set = uts_machine_post_hook,
};

static kdump_status
page_size_pre_hook(kdump_ctx *ctx, struct attr_data *attr,
		   union kdump_attr_value *newval)
{
	size_t page_size = newval->number;

	/* It must be a power of 2 */
	if (page_size != (page_size & ~(page_size - 1)))
		return set_error(ctx, kdump_dataerr,
				 "Invalid page size: %zu", page_size);

	return set_page_shift(ctx, ffsl((unsigned long)page_size) - 1);
}

static kdump_status
page_size_post_hook(kdump_ctx *ctx, struct attr_data *attr)
{
	return ctx->ops && ctx->ops->realloc_caches
		? ctx->ops->realloc_caches(ctx)
		: kdump_ok;
}

const struct attr_ops page_size_ops = {
	.pre_set = page_size_pre_hook,
	.post_set = page_size_post_hook,
};

static kdump_status
page_shift_post_hook(kdump_ctx *ctx, struct attr_data *attr)
{
	return set_page_size(ctx, (size_t)1 << attr_value(attr)->number);
}

const struct attr_ops page_shift_ops = {
	.post_set = page_shift_post_hook,
};

/* Final NUL may be missing in the source (i.e. corrupted dump data),
 * but let's make sure that it is present in the destination.
 */
static kdump_status
set_uts_string(kdump_ctx *ctx, struct attr_data *attr, const char *src)
{
	char str[NEW_UTS_LEN + 1];

	memcpy(str, src, NEW_UTS_LEN);
	str[NEW_UTS_LEN] = 0;
	return set_error(ctx, set_raw_attr_string(ctx, attr, str),
			 "Cannot set attribute %s", attr->template->key);
}

kdump_status
set_uts(kdump_ctx *ctx, const struct new_utsname *src)
{
	static const struct {
		unsigned idx, off;
	} defs[] = {
#define DEF(name) \
		{ GKI_linux_uts_ ## name, offsetof(struct new_utsname, name) }
		DEF(sysname),
		DEF(nodename),
		DEF(release),
		DEF(version),
		DEF(machine),
		DEF(domainname)
#undef DEF
	};

	unsigned i;
	kdump_status res;

	for (i = 0; i < ARRAY_SIZE(defs); ++i) {
		res = set_uts_string(ctx, ctx->global_attrs[defs[i].idx],
				     (const char*)src + defs[i].off);
		if (res != kdump_ok)
			return res;
	}

	return kdump_ok;
}

int
uts_looks_sane(struct new_utsname *uts)
{
	/* Since all strings are NUL-terminated, the last byte in
	 * the array must be always zero; domainname may be missing.
	 */
	if (uts->sysname[NEW_UTS_LEN] || uts->nodename[NEW_UTS_LEN] ||
	    uts->release[NEW_UTS_LEN] || uts->version[NEW_UTS_LEN] ||
	    uts->machine[NEW_UTS_LEN])
		return 0;

	/* release, version and machine cannot be empty */
	if (!uts->release[0] || !uts->version[0] || !uts->machine[0])
		return 0;

	/* sysname is kind of a magic signature */
	return !strcmp(uts->sysname, UTS_SYSNAME);
}

int
uncompress_rle(unsigned char *dst, size_t *pdstlen,
	       const unsigned char *src, size_t srclen)
{
	const unsigned char *srcend = src + srclen;
	size_t remain = *pdstlen;

	while (src < srcend) {
		unsigned char byte, cnt;

		if (! (byte = *src++)) {
			if (src >= srcend)
				return -1;
			if ( (cnt = *src++) ) {
				if (remain < cnt)
					return -1;
				if (src >= srcend)
					return -1;
				memset(dst, *src++, cnt);
				dst += cnt;
				remain -= cnt;
				continue;
			}
		}

		if (!remain)
			return -1;
		*dst++ = byte;
		--remain;
	}

	*pdstlen -= remain;
	return 0;
}

#if USE_ZLIB
static kdump_status
set_zlib_error(kdump_ctx *ctx, const char *what,
	       const z_stream *zstream, int err)
{
	if (err == Z_ERRNO)
		return set_error(ctx, kdump_syserr, "%s", what);

	if (!zstream->msg)
		return set_error(ctx, kdump_dataerr,
				 "%s: error %d", what, err);

	return set_error(ctx, kdump_dataerr,
			 "%s: %s", what, zstream->msg);
}
#endif

/**  Uncompress a gzipp'ed page.
 * @param ctx     Dump file object.
 * @param dst     Destination buffer.
 * @param src     Source (compressed) data.
 * @param srclen  Length of source data.
 */
kdump_status
uncompress_page_gzip(kdump_ctx *ctx, unsigned char *dst,
		     unsigned char *src, size_t srclen)
{
#if USE_ZLIB
	z_stream zstream;
	int res;

	memset(&zstream, 0, sizeof zstream);
	zstream.next_in = (z_const Bytef *)src;
	zstream.avail_in = srclen;
	zstream.next_out = dst;
	zstream.avail_out = get_page_size(ctx);

	res = inflateInit(&zstream);
	if (res != Z_OK)
		return set_zlib_error(ctx, "Cannot init zlib", &zstream, res);

	res = inflate(&zstream, Z_FINISH);
	if (res != Z_STREAM_END) {
		inflateEnd(&zstream);
		if (res == Z_NEED_DICT ||
		    (res == Z_BUF_ERROR && zstream.avail_in == 0))
			res = Z_DATA_ERROR;
		return set_zlib_error(ctx, "Decompresion failed",
				      &zstream, res);
	}

	res = inflateEnd(&zstream);
	if (res != Z_OK)
		return set_zlib_error(ctx, "Decompression failed",
				      &zstream, res);

	if (zstream.avail_out)
		return set_error(ctx, kdump_dataerr,
				 "Wrong uncompressed size: %lu",
				 (unsigned long) zstream.total_out);

	return kdump_ok;

#else
	return set_error(ctx, kdump_unsupported,
			 "Unsupported compression method: %s", "zlib");
#endif
}

static kdump_status
create_attr_path(kdump_ctx *ctx, char *path, enum kdump_attr_type type)
{
	char *p;
	kdump_status res;

	for (p = path; (p = strchr(p, '.')); ++p) {
		*p = '\0';
		res = add_attr_template(ctx, path, kdump_directory);
		if (res != kdump_ok)
			return set_error(ctx, res,
					 "Cannot add attribute '%s'", path);
		*p = '.';
	}
	return set_error(ctx, add_attr_template(ctx, path, type),
			 "Cannot add attribute '%s'", path);
}

static kdump_status
add_parsed_row(kdump_ctx *ctx, const char *path,
	       const char *key, const char *val)
{
	char *attrkey, *suff, *type, *sym;
	char *p, *q;
	size_t len;
	unsigned long long num;
	enum kdump_attr_type attr_type;
	kdump_status res;

	attrkey = alloca(strlen(path) + strlen(key) + sizeof(".lines"));
	suff = stpcpy(attrkey, path);

	p = stpcpy(suff, ".lines.");
	stpcpy(p, key);

	/* FIXME: Invent a better way to store lines with dots
	 * in the key name
	 */
	res = create_attr_path(ctx, attrkey, kdump_string);
	if (res != kdump_ok)
		return res;
	res = set_attr_string(ctx, attrkey, val);
	if (res != kdump_ok)
		return set_error(ctx, res,
				 "Cannot set vmcoreinfo '%s'", key);

	p = strchr(key, '(');
	if (!p)
		return kdump_ok;
	q = strchr(p, ')');
	if (!q || q[1])
		return kdump_ok;

	type = suff;
	*type++ = '.';
	len = p - key;
	memcpy(type, key, len);
	type[len] = '\0';

	sym = type + len + 1;
	len = q - p - 1;
	memcpy(sym, p + 1, len);
	sym[len] = '\0';

	if (!strcmp(type, "SYMBOL")) {
		num = strtoull(val, &p, 16);
		if (*p)
			/* invalid format -> ignore */
			return kdump_ok;
		attr_type = kdump_address;
	} else if (!strcmp(type, "LENGTH") ||
		   !strcmp(type, "NUMBER") ||
		   !strcmp(type, "OFFSET") ||
		   !strcmp(type, "SIZE")) {
		num = strtoull(val, &p, 10);
		if (*p)
			/* invalid format -> ignore */
			return kdump_ok;
		attr_type = kdump_number;
	} else
		return kdump_ok;

	sym[-1] = '.';
	res = create_attr_path(ctx, attrkey, attr_type);
	if (res != kdump_ok)
		return res;
	return set_error(ctx,
			 (attr_type == kdump_number)
			 ? set_attr_number(ctx, attrkey, num)
			 : set_attr_address(ctx, attrkey, num),
			 "Cannot set %s", attrkey);
}

kdump_status
store_vmcoreinfo(kdump_ctx *ctx, const char *path, void *data, size_t len)
{
	char key[strlen(path) + sizeof(".lines")];
	char *raw, *p, *endp, *val;
	kdump_status res;

	raw = ctx_malloc(len + 1, ctx, "VMCOREINFO");
	if (!raw)
		return kdump_syserr;
	memcpy(raw, data, len);
	raw[len] = '\0';

	stpcpy(stpcpy(key, path), ".raw");
	res = set_attr_string(ctx, key, raw);
	if (res != kdump_ok) {
		free(raw);
		return set_error(ctx, res, "Cannot set '%s'", key);
	}

	stpcpy(stpcpy(key, path), ".lines");
	for (p = raw; *p; p = endp) {
		endp = strchrnul(p, '\n');
		if (*endp)
			*endp++ = '\0';

		val = strchr(p, '=');
		if (val)
			*val++ = '\0';

		res = add_parsed_row(ctx, path, p, val);
		if (res != kdump_ok)
			break;
	}

	free(raw);
	return res;
}

/* /dev/crash cannot handle reads larger than page size */
ssize_t
paged_read(int fd, void *buffer, size_t size)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t todo = size;
	while (todo) {
		size_t chunksize = (todo > page_size)
			? page_size
			: todo;
		ssize_t rd = read(fd, buffer, chunksize);
		if (rd < 0)
			return rd;

		buffer += rd;
		todo -= rd;
		if (rd != chunksize)
			break;
	}
	return size - todo;
}

uint32_t
cksum32(void *buffer, size_t size, uint32_t csum)
{
	uint32_t *p, prevsum;

	for (p = buffer; size >= 4; ++p, size -= 4) {
		prevsum = csum;
		csum += be32toh(*p);
		if (csum < prevsum)
			++csum;
	}

	if (size) {
		unsigned char *pbyte = (unsigned char*)p;
		uint32_t val = 0;
		while (size--)
			val = (val >> 8) | (*pbyte++ << 24);

		prevsum = csum;
		csum += val;
		if (csum < prevsum)
			++csum;
	}

	return csum;
}

kdump_status
get_symbol_val(kdump_ctx *ctx, const char *name, kdump_addr_t *val)
{
	kdump_status ret = ctx->cb_get_symbol_val(ctx, name, val);
	return set_error(ctx, ret, "Cannot resolve \"%s\"", name);
}

kdump_status
get_symbol_val_xen(kdump_ctx *ctx, const char *name, kdump_addr_t *val)
{
	kdump_status ret = ctx->cb_get_symbol_val_xen(ctx, name, val);
	return set_error(ctx, ret, "Cannot resolve \"%s\"", name);
}

kdump_status
set_cpu_regs64(kdump_ctx *ctx, unsigned cpu,
	       const struct attr_template *tmpl, uint64_t *regs, unsigned num)
{
	char cpukey[sizeof("cpu.") + 20 + sizeof(".reg")];
	unsigned i;
	kdump_status res;

	sprintf(cpukey, "cpu.%u.reg", cpu);
	res = create_attr_path(ctx, cpukey, kdump_directory);
	if (res != kdump_ok)
		return res;

	for (i = 0; i < num; ++i) {
		res = add_attr_number(ctx, cpukey, tmpl + i,
				      dump64toh(ctx, regs[i]));
		if (res != kdump_ok)
			return res;
	}

	return kdump_ok;
}

kdump_status
set_cpu_regs32(kdump_ctx *ctx, unsigned cpu,
	       const struct attr_template *tmpl, uint32_t *regs, unsigned num)
{
	char cpukey[sizeof("cpu.") + 20 + sizeof(".reg")];
	unsigned i;
	kdump_status res;

	sprintf(cpukey, "cpu.%u.reg", cpu);
	res = create_attr_path(ctx, cpukey, kdump_directory);
	if (res != kdump_ok)
		return res;

	for (i = 0; i < num; ++i) {
		res = add_attr_number(ctx, cpukey, tmpl + i,
				      dump32toh(ctx, regs[i]));
		if (res != kdump_ok)
			return res;
	}

	return kdump_ok;
}
