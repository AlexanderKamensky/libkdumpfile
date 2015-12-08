/* Routines for parsing ELF notes.
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

#include <stdlib.h>
#include <string.h>
#include <elf.h>

/* System information exported through crash notes. */
#define XEN_ELFNOTE_CRASH_INFO 0x1000001

/* .Xen.note types */
#define XEN_ELFNOTE_DUMPCORE_NONE            0x2000000
#define XEN_ELFNOTE_DUMPCORE_HEADER          0x2000001
#define XEN_ELFNOTE_DUMPCORE_XEN_VERSION     0x2000002
#define XEN_ELFNOTE_DUMPCORE_FORMAT_VERSION  0x2000003

struct xen_elfnote_header {
	uint64_t xch_magic;
	uint64_t xch_nr_vcpus;
	uint64_t xch_nr_pages;
	uint64_t xch_page_size;
}; 

struct xen_crash_info_32 {
	uint32_t xen_major_version;
	uint32_t xen_minor_version;
	uint32_t xen_extra_version;
	uint32_t xen_changeset;
	uint32_t xen_compiler;
	uint32_t xen_compile_date;
	uint32_t xen_compile_time;
	uint32_t tainted;
	/* Additional arch-dependent and version-dependent fields  */
};

struct xen_crash_info_64 {
	uint64_t xen_major_version;
	uint64_t xen_minor_version;
	uint64_t xen_extra_version;
	uint64_t xen_changeset;
	uint64_t xen_compiler;
	uint64_t xen_compile_date;
	uint64_t xen_compile_time;
	uint64_t tainted;
	/* Additional arch-dependent and version-dependent fields  */
};

#define XEN_EXTRA_VERSION_SZ	16
#define XEN_COMPILER_SZ		64
#define XEN_COMPILE_BY_SZ	16
#define XEN_COMPLE_DOMAIN_SZ	32
#define XEN_COMPILE_DATE_SZ	32
#define XEN_CAPABILITIES_SZ	1024
#define XEN_CHANGESET_SZ	64

struct xen_dumpcore_elfnote_xen_version_32 {
	uint64_t major_version;
	uint64_t minor_version;
	char     extra_version[XEN_EXTRA_VERSION_SZ];
	struct {
		char compiler[XEN_COMPILER_SZ];
		char compile_by[XEN_COMPILE_BY_SZ];
		char compile_domain[XEN_COMPLE_DOMAIN_SZ];
		char compile_date[XEN_COMPILE_DATE_SZ];
	} compile_info;
	char capabilities[XEN_CAPABILITIES_SZ];
	char changeset[XEN_CHANGESET_SZ];
	struct   {
		uint32_t virt_start;
	} platform_parameters;
	uint64_t pagesize;
};

struct xen_dumpcore_elfnote_xen_version_64 {
	uint64_t major_version;
	uint64_t minor_version;
	char     extra_version[XEN_EXTRA_VERSION_SZ];
	struct {
		char compiler[XEN_COMPILER_SZ];
		char compile_by[XEN_COMPILE_BY_SZ];
		char compile_domain[XEN_COMPLE_DOMAIN_SZ];
		char compile_date[XEN_COMPILE_DATE_SZ];
	} compile_info;
	char capabilities[XEN_CAPABILITIES_SZ];
	char changeset[XEN_CHANGESET_SZ];
	struct   {
		uint64_t virt_start;
	} platform_parameters;
	uint64_t pagesize;
};

typedef kdump_status do_note_fn(kdump_ctx *ctx, Elf32_Word type,
				const char *name, size_t namesz,
				void *desc, size_t descsz);

/* These fields in kdump_ctx must be initialised:
 *
 *   arch_ops
 */
static kdump_status
process_core_note(kdump_ctx *ctx, uint32_t type,
		  void *desc, size_t descsz)
{
	if (type == NT_PRSTATUS) {
		if (ctx->arch_ops && ctx->arch_ops->process_prstatus)
			return ctx->arch_ops->process_prstatus(
				ctx, desc, descsz);
	}

	return kdump_ok;
}

/* These fields in kdump_ctx must be initialised:
 *
 *   endian
 *   ptr_size
 */
static kdump_status
process_xen_crash_info(kdump_ctx *ctx, void *data, size_t len)
{
	size_t ptr_size = get_attr_ptr_size(ctx);
	unsigned words = len / ptr_size;

	if (ptr_size == 8 &&
	    len >= sizeof(struct xen_crash_info_64)) {
		struct xen_crash_info_64 *info = data;
		ctx->xen_ver.major = dump64toh(ctx, info->xen_major_version);
		ctx->xen_ver.minor = dump64toh(ctx, info->xen_minor_version);
		ctx->xen_extra_ver = dump64toh(ctx, info->xen_extra_version);
		ctx->xen_p2m_mfn = dump64toh(ctx, ((uint64_t*)data)[words-1]);
	} else if (ptr_size == 4 &&
		   len >= sizeof(struct xen_crash_info_32)){
		struct xen_crash_info_32 *info = data;
		ctx->xen_ver.major = dump32toh(ctx, info->xen_major_version);
		ctx->xen_ver.minor = dump32toh(ctx, info->xen_minor_version);
		ctx->xen_extra_ver = dump32toh(ctx, info->xen_extra_version);
		ctx->xen_p2m_mfn = dump32toh(ctx, ((uint32_t*)data)[words-1]);
	}

	return kdump_ok;
}

static kdump_status
process_xen_dumpcore_version(kdump_ctx *ctx, void *data, size_t len)
{
	size_t ptr_size = get_attr_ptr_size(ctx);
	const char *ver_extra = NULL;

	if (ptr_size == 8 &&
	    len >= sizeof(struct xen_dumpcore_elfnote_xen_version_64)) {
		struct xen_dumpcore_elfnote_xen_version_64 *ver = data;
		ctx->xen_ver.major = dump64toh(ctx, ver->major_version);
		ctx->xen_ver.minor = dump64toh(ctx, ver->minor_version);
		ver_extra = ver->extra_version;
	} else if(ptr_size == 4 &&
		  len >= sizeof(struct xen_dumpcore_elfnote_xen_version_32)) {
		struct xen_dumpcore_elfnote_xen_version_32 *ver = data;
		ctx->xen_ver.major = dump64toh(ctx, ver->major_version);
		ctx->xen_ver.minor = dump64toh(ctx, ver->minor_version);
		ver_extra = ver->extra_version;
	}

	if (ver_extra) {
		char *newextra;
		newextra = ctx_malloc(XEN_EXTRA_VERSION_SZ + 1,
				      ctx, "Xen extra version");
		if (!newextra)
			return kdump_syserr;
		memcpy(newextra, ver_extra, XEN_EXTRA_VERSION_SZ);
		newextra[XEN_EXTRA_VERSION_SZ] = '\0';
		ctx->xen_ver.extra = newextra;
	}

	return kdump_ok;
}

/* These fields in kdump_ctx must be initialised:
 *
 *   endian
 *   ptr_size
 */
static kdump_status
process_xen_note(kdump_ctx *ctx, uint32_t type,
		 void *desc, size_t descsz)
{
	kdump_status ret = kdump_ok;

	if (type == XEN_ELFNOTE_CRASH_INFO)
		ret = process_xen_crash_info(ctx, desc, descsz);
	else if (type == XEN_ELFNOTE_DUMPCORE_XEN_VERSION)
		process_xen_dumpcore_version(ctx, desc, descsz);

	ctx->flags |= DIF_XEN;
	return ret;
}

/* These fields in kdump_ctx must be initialised:
 *
 *   endian
 */
static kdump_status
process_xc_xen_note(kdump_ctx *ctx, uint32_t type,
		    void *desc, size_t descsz)
{
	if (type == XEN_ELFNOTE_DUMPCORE_HEADER) {
		struct xen_elfnote_header *header = desc;
		uint64_t page_size = dump64toh(ctx, header->xch_page_size);

		return set_page_size(ctx, page_size);
	} else if (type == XEN_ELFNOTE_DUMPCORE_FORMAT_VERSION) {
		uint64_t version = dump64toh(ctx, *(uint64_t*)desc);

		if (version != 1)
			return set_error(ctx, kdump_unsupported,
					 "Unsupported Xen dumpcore format version: %llu",
					 (unsigned long long) version);
	}

	return kdump_ok;
}

kdump_status
process_vmcoreinfo(kdump_ctx *ctx, void *desc, size_t descsz)
{
	kdump_status ret;
	const char *val;

	ret = store_vmcoreinfo(ctx, &ctx->vmcoreinfo, desc, descsz);
	if (ret != kdump_ok)
		return ret;

	val = kdump_vmcoreinfo_row(ctx, "PAGESIZE");
	if (val) {
		char *endp;
		unsigned long page_size = strtoul(val, &endp, 10);
		if (*endp)
			return set_error(ctx, kdump_dataerr,
					 "Invalid PAGESIZE: %s", val);

		ret = set_page_size(ctx, page_size);
		if (ret != kdump_ok)
			return ret;
	}

	val = kdump_vmcoreinfo_row(ctx, "OSRELEASE");
	if (val) {
		ret = set_attr_string(ctx, GATTR(GKI_linux_uts_release), val);
		if (ret != kdump_ok)
			return set_error(ctx, ret,
					 "Cannot set UTS release");
	}

	return kdump_ok;
}

static int
note_equal(const char *name, const char *notename, size_t notenamesz)
{
	size_t namelen = strlen(name);
	if (notenamesz >= namelen && notenamesz <= namelen + 1)
		return !memcmp(name, notename, notenamesz);
	return 0;
}

static kdump_status
do_noarch_note(kdump_ctx *ctx, Elf32_Word type,
	       const char *name, size_t namesz, void *desc, size_t descsz)
{
	if (note_equal("VMCOREINFO", name, namesz))
		return process_vmcoreinfo(ctx, desc, descsz);
	else if (note_equal("VMCOREINFO_XEN", name, namesz))
		return store_vmcoreinfo(ctx, &ctx->vmcoreinfo_xen,
					desc, descsz);

	return kdump_ok;
}

/* These fields from kdump_ctx must be initialised:
 *
 *   endian
 *   ptr_size
 *   arch_ops
 *
 */
static kdump_status
do_arch_note(kdump_ctx *ctx, Elf32_Word type,
	     const char *name, size_t namesz, void *desc, size_t descsz)
{
	if (note_equal("CORE", name, namesz))
		return process_core_note(ctx, type, desc, descsz);
	else if (note_equal("Xen", name, namesz))
		return process_xen_note(ctx, type, desc, descsz);
	else if (note_equal(".note.Xen", name, namesz))
		return process_xc_xen_note(ctx, type, desc, descsz);

	return kdump_ok;
}

static kdump_status
do_any_note(kdump_ctx *ctx, Elf32_Word type,
	    const char *name, size_t namesz, void *desc, size_t descsz)
{
	kdump_status ret;

	ret = do_noarch_note(ctx, type, name, namesz, desc, descsz);
	if (ret != kdump_ok)
		return ret;
	return do_arch_note(ctx, type, name, namesz, desc, descsz);
}

#define roundup_size(sz)	(((size_t)(sz)+3) & ~(size_t)3)

static kdump_status
do_notes(kdump_ctx *ctx, void *data, size_t size, do_note_fn *do_note)
{
	Elf32_Nhdr *hdr = data;
	kdump_status ret = kdump_ok;

	while (ret == kdump_ok && size >= sizeof(Elf32_Nhdr)) {
		char *name, *desc;
		Elf32_Word namesz = dump32toh(ctx, hdr->n_namesz);
		Elf32_Word descsz = dump32toh(ctx, hdr->n_descsz);
		Elf32_Word type = dump32toh(ctx, hdr->n_type);
		size_t descoff = sizeof(Elf32_Nhdr) + roundup_size(namesz);

		if (size < descoff + descsz)
			break;

		name = (char*) (hdr + 1);
		desc = (char*)hdr + descoff;
		size -= descoff;

		hdr = (Elf32_Nhdr*) (desc + roundup_size(descsz));
		size = (size >= roundup_size(descsz))
			? size - roundup_size(descsz)
			: 0;

		ret = do_note(ctx, type, name, namesz, desc, descsz);
	}

	return ret;
}

kdump_status
process_noarch_notes(kdump_ctx *ctx, void *data, size_t size)
{
	return do_notes(ctx, data, size, do_noarch_note);
}

kdump_status
process_arch_notes(kdump_ctx *ctx, void *data, size_t size)
{
	return do_notes(ctx, data, size, do_arch_note);
}

kdump_status
process_notes(kdump_ctx *ctx, void *data, size_t size)
{
	return do_notes(ctx, data, size, do_any_note);
}
