/* Utility functions.
   Copyright (C) 2014 Petr Tesarik <ptesarik@suse.cz>

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
#include <errno.h>

static size_t
arch_ptr_size(enum kdump_arch arch)
{
	switch (arch) {
	case ARCH_ALPHA:
	case ARCH_IA64:
	case ARCH_PPC64:
	case ARCH_PPC64LE:
	case ARCH_S390X:
	case ARCH_X86_64:
		return 8;	/* 64 bits */

	case ARCH_ARM:
	case ARCH_PPC:
	case ARCH_S390:
	case ARCH_X86:
	default:
		return 4;	/* 32 bits */
	}

}

enum kdump_arch
machine_arch(const char *machine)
{
	if (!strcmp(machine, "alpha"))
		return ARCH_ALPHA;
	else if (!strcmp(machine, "ia64"))
		return ARCH_IA64;
	else if (!strcmp(machine, "ppc"))
		return ARCH_PPC;
	else if (!strcmp(machine, "ppc64"))
		return ARCH_PPC64;
	else if (!strcmp(machine, "ppc64le"))
		return ARCH_PPC64LE;
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
		[ARCH_PPC] = 0,
		[ARCH_PPC64] = 0,
		[ARCH_PPC64LE] = 0,
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
	case ARCH_PPC:
	case ARCH_PPC64:
	case ARCH_PPC64LE:
	case ARCH_S390:
		/* TODO */
		break;

	case ARCH_S390X:	return &s390x_ops;
	case ARCH_X86:		return &ia32_ops;
	case ARCH_X86_64:	return &x86_64_ops;

	default:
		break;
	}

	return NULL;
}

static kdump_status
set_page_size_and_shift(kdump_ctx *ctx, size_t page_size, unsigned page_shift)
{
	void *page = realloc(ctx->page, page_size);
	if (!page)
		return set_error(ctx, kdump_syserr, strerror(errno));
	ctx->page = page;
	ctx->page_size = page_size;
	ctx->page_shift = page_shift;
	return kdump_ok;
}

kdump_status
set_arch(kdump_ctx *ctx, enum kdump_arch arch)
{
	if (!ctx->page_size) {
		int page_shift = default_page_shift(arch);
		if (!page_shift)
			return set_error(ctx, kdump_unsupported,
					 "No default page size");
		set_page_size_and_shift(ctx, 1UL << page_shift, page_shift);
	}

	ctx->arch = arch;
	ctx->ptr_size = arch_ptr_size(arch);
	ctx->arch_ops = arch_ops(arch);

	if (ctx->arch_ops && ctx->arch_ops->init)
		return ctx->arch_ops->init(ctx);

	return kdump_ok;
}

kdump_status
set_page_size(kdump_ctx *ctx, size_t page_size)
{
	unsigned page_shift;

	/* It must be a power of 2 */
	if (page_size != (page_size & ~(page_size - 1)))
		return set_error(ctx, kdump_dataerr,
				 "Invalid page size");

	page_shift = ffsl((unsigned long)page_size) - 1;
	return set_page_size_and_shift(ctx, page_size, page_shift);
}

/* Final NUL may be missing in the source (i.e. corrupted dump data),
 * but let's make sure that it is present in the destination.
 */
void
copy_uts_string(char *dest, const char *src)
{
	if (!*dest) {
		memcpy(dest, src, NEW_UTS_LEN);
		dest[NEW_UTS_LEN] = 0;
	}
}

void
set_uts(kdump_ctx *ctx, const struct new_utsname *src)
{
	copy_uts_string(ctx->utsname.sysname, src->sysname);
	copy_uts_string(ctx->utsname.nodename, src->nodename);
	copy_uts_string(ctx->utsname.release, src->release);
	copy_uts_string(ctx->utsname.version, src->version);
	copy_uts_string(ctx->utsname.machine, src->machine);
	copy_uts_string(ctx->utsname.domainname, src->domainname);
	ctx->flags |= DIF_UTSNAME;
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

static unsigned
count_lines(char *buf, size_t len)
{
	size_t remain;
	unsigned ret = 0;

	for (remain = len; *buf && remain > 0; ++buf, --remain)
		if (*buf == '\n')
			++ret;

	/* Possibly incomplete last line */
	if (len && buf[-1] != '\n')
		++ret;
	return ret;
}

kdump_status
store_vmcoreinfo(kdump_ctx *ctx, struct vmcoreinfo **pinfo,
		 void *data, size_t len)
{
	struct vmcoreinfo *info;
	struct vmcoreinfo_row *row;
	char *p, *q;
	unsigned n;

	n = count_lines(data, len);
	info = malloc(sizeof(struct vmcoreinfo) +
		      n * sizeof(struct vmcoreinfo_row) +
		      2 * (len + 1));
	if (!info)
		return set_error(ctx, kdump_syserr, strerror(errno));

	info->raw = (char*)info->row + n * sizeof(struct vmcoreinfo_row);
	memcpy(info->raw, data, len);
	info->raw[len] = '\0';

	p = info->raw;
	q = p + len + 1;
	row = info->row;
	info->n = 0;
	while (*p) {
		char *endp, *eq;

		endp = strchr(p, '\n');
		if (!endp)
			endp = p + strlen(p);

		memcpy(q, p, endp - p);

		row->key = q;

		eq = memchr(q, '=', endp - p);
		if (eq) {
			*eq = 0;
			row->val = eq + 1;
		} else
			row->val = NULL;
		++row;
		++info->n;

		q += endp - p;
		*q++ = '\0';
		p = endp;
		if (*p)
			++p;
	}

	*pinfo = info;
	return kdump_ok;
}

/* /dev/crash cannot handle reads larger than page size */
size_t
paged_cpin(int fd, void *buffer, size_t size)
{
	long page_size = sysconf(_SC_PAGESIZE);
	while (size) {
		size_t chunksize = (size > page_size)
			? page_size
			: size;
		if (read(fd, buffer, chunksize) != chunksize)
			return size;

		buffer += chunksize;
		size -= chunksize;
	}
	return 0;
}
