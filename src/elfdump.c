/** @internal @file src/elfdump.c
 * @brief Routines to work with ELF kdump files.
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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <elf.h>

/* This definition is missing from older version of <elf.h> */
#ifndef EM_AARCH64
# define EM_AARCH64      183
#endif

static const struct format_ops xc_core_elf_ops;

/** Invalid Xen page index.
 * This constant is used to denote that there is no valid index value,
 * e.g. to indicate search failure.
 */
#define IDX_NONE	(~(uint_fast64_t)0)

struct xen_p2m {
	uint64_t pfn;
	uint64_t gmfn;
};

struct load_segment {
	off_t file_offset;
	off_t filesz;
	kdump_paddr_t phys;
	kdump_addr_t memsz;
	kdump_vaddr_t virt;
};

struct section {
	off_t file_offset;
	uint64_t size;
	int name_index;
};

/** Translation of a single PFN to an index.
 */
struct pfn2idx {
	kdump_pfn_t pfn;	/**< PFN to be translated. */
	uint_fast64_t idx;	/**< Page index in .xen_pages.  */
};

/** Translation of a PFN range to an index.
 */
struct pfn2idx_range {
	kdump_pfn_t pfn;	/**< PFN to be translated. */
	uint_fast64_t idx;	/**< Page index in .xen_pages.  */

	/** Length of the range.
	 * If the number is negative, then PFNs are mapped in descending
	 * order (i.e. while PFN increases, the index decreases),
	 * otherwise in ascending order.
	 */
	int_fast64_t len;
};

/** Complete mapping of all PFNs to indices.
 */
struct pfn2idx_map {
	size_t nranges;		      /**< Number of ranges. */
	struct pfn2idx_range *ranges; /**< Page ranges (longer than 1). */

	size_t nsingles;	 /**< Number of single pages. */
	struct pfn2idx *singles; /**< Single pages outside of any range. */
};

/** PFN-to-index vector allocation increment.
 * For optimal performance, this should be a power of two.
 */
#define PFN2IDX_ALLOC_INC    16

struct elfdump_priv {
	int num_load_segments;
	struct load_segment *load_segments;
	int num_note_segments;
	struct load_segment *note_segments;

	int num_sections;
	struct section *sections;

	size_t strtab_size;
	char *strtab;

	off_t xen_pages_offset;

	int elfclass;

	/** Map PFN to page index in .xen_pages. */
	struct pfn2idx_map xen_pfnmap;

	/** Map GMFN to page index in .xen_pages. */
	struct pfn2idx_map xen_mfnmap;

	/** File offset of Xen page map (xc_core) */
	off_t xen_map_offset;
};

static void elf_cleanup(struct kdump_shared *shared);

static const char *
mach2arch(unsigned mach, int elfclass)
{
	switch(mach) {
	case EM_AARCH64:
			return KDUMP_ARCH_AARCH64;
	case EM_ARM:	return KDUMP_ARCH_ARM;
	case EM_ALPHA:
	case EM_FAKE_ALPHA:
			return KDUMP_ARCH_ALPHA;
	case EM_IA_64:	return KDUMP_ARCH_IA64;
	case EM_MIPS:	return KDUMP_ARCH_MIPS;
	case EM_PPC:	return KDUMP_ARCH_PPC;
	case EM_PPC64:	return KDUMP_ARCH_PPC64;
	case EM_S390:	return (elfclass == ELFCLASS64
				? KDUMP_ARCH_S390X
				: KDUMP_ARCH_S390);
	case EM_386:	return KDUMP_ARCH_IA32;
	case EM_X86_64:	return KDUMP_ARCH_X86_64;
	default:	return NULL;
	}
}

/**  Find the LOAD segment that is closest to a physical address.
 * @param edp	 ELF dump private data.
 * @param paddr	 Requested physical address.
 * @param dist	 Maximum allowed distance from @ref paddr.
 * @returns	 Pointer to the closest LOAD segment, or @c NULL if none.
 */
static struct load_segment *
find_closest_load(struct elfdump_priv *edp, kdump_paddr_t paddr,
		  unsigned long dist)
{
	unsigned long bestdist;
	struct load_segment *bestload;
	int i;

	bestdist = dist;
	bestload = NULL;
	for (i = 0; i < edp->num_load_segments; i++) {
		struct load_segment *pls = &edp->load_segments[i];
		if (pls->phys == ADDRXLAT_ADDR_MAX)
			continue;
		if (paddr >= pls->phys + pls->memsz)
			continue;
		if (paddr >= pls->phys)
			return pls;	/* Exact match */
		if (bestdist > pls->phys - paddr) {
			bestdist = pls->phys - paddr;
			bestload = pls;
		}
	}
	return bestload;
}

static kdump_status
elf_read_page(kdump_ctx_t *ctx, struct page_io *pio, cache_key_t addr)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	struct load_segment *pls;
	void *p, *endp;
	off_t pos;
	ssize_t size, rd;

	p = pio->chunk.data;
	endp = p + get_page_size(ctx);
	while (p < endp) {
		pls = find_closest_load(edp, addr, endp - p);
		if (!pls) {
			memset(p, 0, endp - p);
			break;
		}

		if (pls->phys > addr) {
			memset(p, 0, pls->phys - addr);
			p += pls->phys - addr;
			addr = pls->phys;
		}

		pos = pls->file_offset + addr - pls->phys;
		if (pls->phys + pls->filesz > addr) {
			size = endp - p;
			if (size > pls->phys + pls->filesz - addr)
				size = pls->phys + pls->filesz - addr;

			rd = pread(get_file_fd(ctx), p, size, pos);
			if (rd != size)
				return set_error(
					ctx, read_error(rd),
					"Cannot read page data at %llu",
					(unsigned long long) pos);
			p += size;
			addr += size;
		}
		if (p < endp) {
			size = endp - p;
			if (size > pls->phys + pls->memsz - addr)
				size = pls->phys + pls->memsz - addr;
			memset(p, 0, size);
			p += size;
			addr += size;
		}
	}

	return KDUMP_OK;
}

static kdump_status
elf_get_page(kdump_ctx_t *ctx, struct page_io *pio)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	struct load_segment *pls;
	kdump_paddr_t addr;
	size_t sz;

	pls = find_closest_load(edp, pio->addr.addr, get_page_size(ctx));
	if (!pls)
		return set_error(ctx, KDUMP_ERR_NODATA, "Page not found");

	addr = pio->addr.addr;
	sz = get_page_size(ctx);
	return (pls->phys <= addr && pls->filesz >= addr - pls->phys + sz)
		? fcache_get_chunk(ctx->shared->fcache, &pio->chunk, sz,
				   pls->file_offset + addr - pls->phys)
		: cache_get_page(ctx, pio, elf_read_page, addr);
}

static void
pfn2idx_map_start(struct pfn2idx_map *map, struct pfn2idx_range *cur)
{
	map->nranges = 0;
	map->ranges = NULL;
	map->nsingles = 0;
	map->singles = NULL;

	cur->idx = 0;
	cur->len = 0;
}

static void
pfn2idx_map_free(struct pfn2idx_map *map)
{
	free(map->ranges);
	free(map->singles);
}

static kdump_status
pfn2idx_map_addrange(struct pfn2idx_map *map, struct pfn2idx_range *range)
{
	if (range->len > 1 || range->len < -1) {
		if (map->nranges % PFN2IDX_ALLOC_INC == 0) {
			struct pfn2idx_range *newranges;
			size_t newsz = (map->nranges + PFN2IDX_ALLOC_INC) *
				sizeof *newranges;
			newranges = realloc(map->ranges, newsz);
			if (!newranges)
				return KDUMP_ERR_SYSTEM;
			map->ranges = newranges;
		}

		map->ranges[map->nranges].pfn = range->pfn;
		map->ranges[map->nranges].idx = range->idx - 1;
		map->ranges[map->nranges].len = range->len;
		map->nranges++;
	} else if (range->len) {
		if (map->nsingles % PFN2IDX_ALLOC_INC == 0) {
			struct pfn2idx *newsingles;
			size_t newsz = (map->nsingles + PFN2IDX_ALLOC_INC) *
				sizeof *newsingles;
			newsingles = realloc(map->singles, newsz);
			if (!newsingles)
				return KDUMP_ERR_SYSTEM;
			map->singles = newsingles;
		}
		map->singles[map->nsingles].pfn = range->pfn;
		map->singles[map->nsingles].idx = range->idx - 1;
		map->nsingles++;
	}

	return KDUMP_OK;
}

static kdump_status
pfn2idx_map_add(struct pfn2idx_map *map, struct pfn2idx_range *range,
		kdump_pfn_t pfn)
{
	kdump_status status;

	if (range->len > 0 && pfn == range->pfn + 1)
		++range->len;
	else if (range->len < 0 && pfn == range->pfn - 1)
		--range->len;
	else if (range->len == 1 && pfn == range->pfn - 1)
		range->len = -2;
	else {
		status = pfn2idx_map_addrange(map, range);
		if (status != KDUMP_OK)
			return status;
		range->len = 1;
	}
	range->pfn = pfn;
	++range->idx;
	return KDUMP_OK;
}

static int
pfn2idx_range_cmp(const void *a, const void *b)
{
	const struct pfn2idx_range *ra = a, *rb = b;
	return ra->pfn != rb->pfn ? (ra->pfn > rb->pfn ? 1 : -1) : 0;
}

static int
pfn2idx_single_cmp(const void *a, const void *b)
{
	const struct pfn2idx *sa = a, *sb = b;
	return sa->pfn != sb->pfn ? (sa->pfn > sb->pfn ? 1 : -1) : 0;
}

static kdump_status
pfn2idx_map_end(struct pfn2idx_map *map, struct pfn2idx_range *range)
{
	kdump_status status;

	status = pfn2idx_map_addrange(map, range);
	if (status != KDUMP_OK)
		return status;

	qsort(map->ranges, map->nranges, sizeof *map->ranges,
	      pfn2idx_range_cmp);
	qsort(map->singles, map->nsingles, sizeof *map->singles,
	      pfn2idx_single_cmp);

	return KDUMP_OK;
}

static uint_fast64_t
pfn2idx_map_search(struct pfn2idx_map *map, kdump_pfn_t pfn)
{
	size_t i;
	for (i = 0; i < map->nranges; ++i) {
		struct pfn2idx_range *r = &map->ranges[i];
		if (r->len >= 0) {
			if (pfn < r->pfn - r->len + 1)
				break;
			if (pfn <= r->pfn)
				return r->idx + pfn - r->pfn;
		} else {
			if (pfn < r->pfn)
				break;
			if (pfn <= r->pfn - r->len - 1)
				return r->idx + r->pfn - pfn;
		}
	}

	for (i = 0; i < map->nsingles && pfn <= map->singles[i].pfn; ++i)
		if (map->singles[i].pfn == pfn)
			return map->singles[i].idx;

	return IDX_NONE;
}

static kdump_status
make_xen_pfn_map_auto(kdump_ctx_t *ctx, const struct section *sect)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	kdump_pfn_t max_pfn = 0;
	uint64_t pfn, *p;
	struct pfn2idx_range range;
	off_t pos, endpos;
	struct fcache_entry fce;
	kdump_status status;

	pfn2idx_map_start(&edp->xen_pfnmap, &range);

	pos = edp->xen_map_offset = sect->file_offset;
	endpos = pos + sect->size - sizeof *p;
	fce.len = 0;
	fce.cache = NULL;
	while (pos <= endpos) {
		if (fce.len < sizeof *p) {
			fcache_put(&fce);
			status = fcache_get_fb(ctx->shared->fcache, &fce,
					       pos, &pfn, sizeof pfn);
			if (status != KDUMP_OK)
				goto err_read;
		}
		p = fce.data;

		if (*p >= max_pfn)
			max_pfn = *p + 1;

		status = pfn2idx_map_add(&edp->xen_pfnmap, &range, *p);
		if (status != KDUMP_OK)
			goto err_pfn;

		fce.data += sizeof *p;
		fce.len -= sizeof *p;
		pos += sizeof *p;
	}
	status = pfn2idx_map_end(&edp->xen_pfnmap, &range);
	if (status != KDUMP_OK)
		goto err_pfn;

	/* TODO: Warn if endpos - pos < sizeof *p */

	fcache_put(&fce);

	set_max_pfn(ctx, max_pfn);
	return status;

 err_pfn:
	p = fce.data;
	set_error(ctx, status, "Cannot map %s 0x%"PRIx64" -> 0x%"PRIxFAST64,
		  "PFN", *p, range.idx);
	fcache_put(&fce);
	return status;

 err_read:
	return set_error(ctx, status, "Cannot read Xen map at %llu",
			 (unsigned long long)pos);
}

static kdump_status
make_xen_pfn_map_nonauto(kdump_ctx_t *ctx, const struct section *sect)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	kdump_pfn_t max_pfn = 0;
	struct xen_p2m p2m, *p;
	struct pfn2idx_range pfnrange, mfnrange;
	off_t pos, endpos;
	struct fcache_entry fce;
	kdump_status status;

	pfn2idx_map_start(&edp->xen_pfnmap, &pfnrange);
	pfn2idx_map_start(&edp->xen_mfnmap, &mfnrange);

	pos = edp->xen_map_offset = sect->file_offset;
	endpos = pos + sect->size - sizeof *p;
	fce.len = 0;
	fce.cache = NULL;
	while (pos <= endpos) {
		if (fce.len < sizeof *p) {
			fcache_put(&fce);
			status = fcache_get_fb(ctx->shared->fcache, &fce,
					       pos, &p2m, sizeof p2m);
			if (status != KDUMP_OK)
				goto err_read;
		}
		p = fce.data;

		if (p->pfn >= max_pfn)
			max_pfn = p->pfn + 1;

		status = pfn2idx_map_add(&edp->xen_pfnmap, &pfnrange, p->pfn);
		if (status != KDUMP_OK)
			goto err_pfn;
		status = pfn2idx_map_add(&edp->xen_mfnmap, &mfnrange, p->gmfn);
		if (status != KDUMP_OK)
			goto err_mfn;

		fce.data += sizeof *p;
		fce.len -= sizeof *p;
		pos += sizeof *p;
	}
	status = pfn2idx_map_end(&edp->xen_pfnmap, &pfnrange);
	if (status != KDUMP_OK)
			goto err_pfn;
	status = pfn2idx_map_end(&edp->xen_mfnmap, &mfnrange);
	if (status != KDUMP_OK)
			goto err_mfn;

	/* TODO: Warn if endpos - pos < sizeof *p */

	fcache_put(&fce);

	set_max_pfn(ctx, max_pfn);
	return status;

 err_pfn:
	p = fce.data;
	set_error(ctx, status, "Cannot map %s 0x%"PRIx64" -> 0x%"PRIxFAST64,
		  "PFN", p->pfn, pfnrange.idx);
	fcache_put(&fce);
	return status;

err_mfn:
	p = fce.data;
	set_error(ctx, status, "Cannot map %s 0x%"PRIx64" -> 0x%"PRIxFAST64,
			 "MFN", p->gmfn, mfnrange.idx);
	fcache_put(&fce);
	return status;

 err_read:
	return set_error(ctx, status, "Cannot read Xen map at %llu",
			 (unsigned long long)pos);
}

/** xc_core physical-to-machine first step function.
 * @param step  Step state.
 * @param addr  Address to be translated.
 * @returns     Error status.
 */
static addrxlat_status
xc_p2m_first_step(addrxlat_step_t *step, addrxlat_addr_t addr)
{
	const addrxlat_meth_t *meth = step->meth;
	struct kdump_shared *shared = meth->param.custom.data;
	struct elfdump_priv *edp = shared->fmtdata;
	struct xen_p2m p2m;
	uint_fast64_t idx;
	off_t pos;
	kdump_status status;

	idx = pfn2idx_map_search(&edp->xen_pfnmap,
				 addr >> shared->page_shift.number);
	if (idx == IDX_NONE)
		return addrxlat_ctx_err(step->ctx, ADDRXLAT_ERR_NODATA,
					"PFN not found");

	pos = edp->xen_map_offset + idx * sizeof(struct xen_p2m);
	status = fcache_pread(shared->fcache, &p2m, sizeof p2m, pos);
	if (status != KDUMP_OK)
		return addrxlat_ctx_err(step->ctx, ADDRXLAT_ERR_NODATA,
					"Cannot read p2m entry at %llu",
					(unsigned long long)pos);

	step->base.addr = p2m.gmfn << shared->page_shift.number;
	step->idx[0] = addr & (shared->page_size.number - 1);
	step->remain = 1;
	step->elemsz = 1;
	return ADDRXLAT_OK;
}

/** xc_core machine-to-physical first step function.
 * @param step  Step state.
 * @param addr  Address to be translated.
 * @returns     Error status.
 */
static addrxlat_status
xc_m2p_first_step(addrxlat_step_t *step, addrxlat_addr_t addr)
{
	const addrxlat_meth_t *meth = step->meth;
	struct kdump_shared *shared = meth->param.custom.data;
	struct elfdump_priv *edp = shared->fmtdata;
	struct xen_p2m p2m;
	uint_fast64_t idx;
	off_t pos;
	kdump_status status;

	idx = pfn2idx_map_search(&edp->xen_mfnmap,
				 addr >> shared->page_shift.number);
	if (idx == IDX_NONE)
		return addrxlat_ctx_err(step->ctx, ADDRXLAT_ERR_NODATA,
					"MFN not found");

	pos = edp->xen_map_offset + idx * sizeof(struct xen_p2m);
	status = fcache_pread(shared->fcache, &p2m, sizeof p2m, pos);
	if (status != KDUMP_OK)
		return addrxlat_ctx_err(step->ctx, ADDRXLAT_ERR_NODATA,
					"Cannot read p2m entry at %llu",
					(unsigned long long)pos);

	step->base.addr = p2m.pfn << shared->page_shift.number;
	step->idx[0] = addr & (shared->page_size.number - 1);
	step->remain = 1;
	step->elemsz = 1;
	return ADDRXLAT_OK;
}

/** Identity next step function.
 * @param walk  Current step state.
 * @returns     Error status.
 *
 * This method does not modify anything and always succeeds.
 */
static addrxlat_status
next_step_ident(addrxlat_step_t *state)
{
	return ADDRXLAT_OK;
}

static kdump_status
setup_custom_method(kdump_ctx_t *ctx, addrxlat_sys_meth_t methidx,
		    addrxlat_sys_map_t mapidx, const addrxlat_meth_t *meth)
{
	addrxlat_range_t range;
	addrxlat_map_t *map;
	addrxlat_status axstatus;

	map = addrxlat_map_new();
	if (!map)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot allocate translation map");

	range.endoff = ADDRXLAT_ADDR_MAX;
	range.meth = methidx;
	axstatus = addrxlat_map_set(map, 0, &range);
	if (axstatus != ADDRXLAT_OK) {
		addrxlat_map_decref(map);
		return addrxlat2kdump(ctx, axstatus);
	}
	addrxlat_sys_set_map(ctx->shared->xlatsys, mapidx, map);
	addrxlat_sys_set_meth(ctx->shared->xlatsys, methidx, meth);

	return KDUMP_OK;
}

static kdump_status
xc_post_addrxlat(kdump_ctx_t *ctx)
{
	addrxlat_meth_t meth;
	kdump_status status;

	if (get_xen_xlat(ctx) != KDUMP_XEN_NONAUTO)
		return KDUMP_OK;

	/* common fields */
	meth.kind = ADDRXLAT_CUSTOM;
	meth.param.custom.next_step = next_step_ident;
	meth.param.custom.data = ctx->shared;

	/* p2m translation */
	meth.target_as = ADDRXLAT_MACHPHYSADDR;
	meth.param.custom.first_step = xc_p2m_first_step;
	status = setup_custom_method(ctx, ADDRXLAT_SYS_METH_KPHYS_MACHPHYS,
				     ADDRXLAT_SYS_MAP_KPHYS_MACHPHYS, &meth);
	if (status != KDUMP_OK)
		return set_error(ctx, status, "Failed p2m setup");

	/* m2p translation */
	meth.target_as = ADDRXLAT_KPHYSADDR;
	meth.param.custom.first_step = xc_m2p_first_step;
	status = setup_custom_method(ctx, ADDRXLAT_SYS_METH_MACHPHYS_KPHYS,
				     ADDRXLAT_SYS_MAP_MACHPHYS_KPHYS, &meth);
	if (status != KDUMP_OK)
		return set_error(ctx, status, "Failed m2p setup");

	return KDUMP_OK;
}

static kdump_status
xc_get_page(kdump_ctx_t *ctx, struct page_io *pio)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	kdump_pfn_t pfn = pio->addr.addr >> get_page_shift(ctx);
	uint_fast64_t idx;
	off_t offset;

	idx = (pio->addr.as == ADDRXLAT_KPHYSADDR
	       ? pfn2idx_map_search(&edp->xen_pfnmap, pfn)
	       : pfn2idx_map_search(&edp->xen_mfnmap, pfn));
	if (idx == IDX_NONE)
		return set_error(ctx, KDUMP_ERR_NODATA, "Page not found");

	offset = edp->xen_pages_offset + ((off_t)idx << get_page_shift(ctx));
	return fcache_get_chunk(ctx->shared->fcache, &pio->chunk,
				get_page_size(ctx), offset);
}

static kdump_status
init_segments(kdump_ctx_t *ctx, unsigned phnum)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;

	if (!phnum)
		return KDUMP_OK;

	edp->load_segments =
		ctx_malloc(2 * phnum * sizeof(struct load_segment),
			   ctx, "program headers");
	if (!edp->load_segments)
		return KDUMP_ERR_SYSTEM;
	edp->num_load_segments = 0;

	edp->note_segments = edp->load_segments + phnum;
	edp->num_note_segments = 0;
	return KDUMP_OK;
}

static kdump_status
init_sections(kdump_ctx_t *ctx, unsigned snum)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;

	if (!snum)
		return KDUMP_OK;

	edp->sections =
		ctx_malloc(snum * sizeof(struct section),
			   ctx, "section headers");
	if (!edp->sections)
		return KDUMP_ERR_SYSTEM;
	edp->num_sections = 0;
	return KDUMP_OK;
}

static struct load_segment *
next_phdr(struct elfdump_priv *edp, unsigned type)
{
	struct load_segment *pls;

	if (type == PT_LOAD) {
		pls = edp->load_segments + edp->num_load_segments;
		++edp->num_load_segments;
	} else if (type == PT_NOTE) {
		pls = edp->note_segments + edp->num_note_segments;
		++edp->num_note_segments;
	} else
		pls = NULL;

	return pls;
}

static void
store_sect(struct elfdump_priv *edp, off_t offset,
	   uint64_t size, unsigned name_index)
{
	struct section *ps;

	ps = edp->sections + edp->num_sections;
	ps->file_offset = offset;
	ps->size = size;
	ps->name_index = name_index;
	++edp->num_sections;
}

static void *
read_elf_sect(kdump_ctx_t *ctx, struct section *sect)
{
	void *buf;

	buf = ctx_malloc(sect->size, ctx, "ELF section buffer");
	if (!buf)
		return NULL;

	if (pread(get_file_fd(ctx), buf, sect->size,
		  sect->file_offset) == sect->size)
		return buf;

	free(buf);
	return NULL;
}

static kdump_status
init_strtab(kdump_ctx_t *ctx, unsigned strtabidx)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	struct section *ps;

	if (!strtabidx || strtabidx >= edp->num_sections)
		return KDUMP_OK;	/* no string table */

	ps = edp->sections + strtabidx;
	edp->strtab_size = ps->size;
	edp->strtab = read_elf_sect(ctx, ps);
	if (!edp->strtab)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot allocate string table (%zu bytes)",
				 edp->strtab_size);

	return KDUMP_OK;
}

static const char *
strtab_entry(struct elfdump_priv *edp, unsigned index)
{
	return index < edp->strtab_size
		? edp->strtab + index
		: NULL;
}

static kdump_status
init_elf32(kdump_ctx_t *ctx, Elf32_Ehdr *ehdr)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	Elf32_Phdr prog;
	Elf32_Shdr sect;
	off_t offset;
	kdump_status ret;
	int i;

	set_arch_machine(ctx, dump16toh(ctx, ehdr->e_machine));

	ret = init_segments(ctx, dump16toh(ctx, ehdr->e_phnum));
	if (ret != KDUMP_OK)
		return ret;

	ret = init_sections(ctx, dump16toh(ctx, ehdr->e_shnum));
	if (ret != KDUMP_OK)
		return ret;

	offset = dump32toh(ctx, ehdr->e_phoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to program headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_phnum); ++i) {
		struct load_segment *pls;
		ssize_t rd;

		rd = read(get_file_fd(ctx), &prog, sizeof prog);
		if (rd != sizeof prog)
			return set_error(ctx, read_error(rd),
					 "Cannot read program header #%d", i);

		pls = next_phdr(edp, dump32toh(ctx, prog.p_type));
		if (pls) {
			pls->file_offset = dump32toh(ctx, prog.p_offset);
			pls->filesz = dump32toh(ctx, prog.p_filesz);
			pls->phys = dump32toh(ctx, prog.p_paddr);
			if (pls->phys == UINT32_MAX)
				pls->phys = ADDRXLAT_ADDR_MAX;
			pls->memsz = dump32toh(ctx, prog.p_memsz);
			pls->virt = dump32toh(ctx, prog.p_vaddr);
		}
	}

	offset = dump32toh(ctx, ehdr->e_shoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to section headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_shnum); ++i) {
		ssize_t rd;

		rd = read(get_file_fd(ctx), &sect, sizeof sect);
		if (rd != sizeof sect)
			return set_error(ctx, read_error(rd),
					 "Cannot read section header #%d", i);
		store_sect(edp,
			   dump32toh(ctx, sect.sh_offset),
			   dump32toh(ctx, sect.sh_size),
			   dump32toh(ctx, sect.sh_name));
	}

	ret = init_strtab(ctx, dump16toh(ctx, ehdr->e_shstrndx));
	if (ret != KDUMP_OK)
		return ret;

	return ret;
}

static kdump_status
init_elf64(kdump_ctx_t *ctx, Elf64_Ehdr *ehdr)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	Elf64_Phdr prog;
	Elf64_Shdr sect;
	off_t offset;
	kdump_status ret;
	int i;

	set_arch_machine(ctx, dump16toh(ctx, ehdr->e_machine));

	ret = init_segments(ctx, dump16toh(ctx, ehdr->e_phnum));
	if (ret != KDUMP_OK)
		return ret;

	ret = init_sections(ctx, dump16toh(ctx, ehdr->e_shnum));
	if (ret != KDUMP_OK)
		return ret;


	offset = dump64toh(ctx, ehdr->e_phoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to program headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_phnum); ++i) {
		struct load_segment *pls;
		ssize_t rd;

		rd = read(get_file_fd(ctx), &prog, sizeof prog);
		if (rd != sizeof prog)
			return set_error(ctx, read_error(rd),
					 "Cannot read program header #%d", i);

		pls = next_phdr(edp, dump32toh(ctx, prog.p_type));
		if (pls) {
			pls->file_offset = dump64toh(ctx, prog.p_offset);
			pls->filesz = dump64toh(ctx, prog.p_filesz);
			pls->phys = dump64toh(ctx, prog.p_paddr);
			if (pls->phys == UINT64_MAX)
				pls->phys = ADDRXLAT_ADDR_MAX;
			pls->memsz = dump64toh(ctx, prog.p_memsz);
			pls->virt = dump64toh(ctx, prog.p_vaddr);
		}
	}

	offset = dump32toh(ctx, ehdr->e_shoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to section headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_shnum); ++i) {
		ssize_t rd;

		rd = read(get_file_fd(ctx), &sect, sizeof sect);
		if (rd != sizeof sect)
			return set_error(ctx, read_error(rd),
					 "Cannot read section header #%d", i);
		store_sect(edp,
			   dump64toh(ctx, sect.sh_offset),
			   dump64toh(ctx, sect.sh_size),
			   dump32toh(ctx, sect.sh_name));
	}

	ret = init_strtab(ctx, dump16toh(ctx, ehdr->e_shstrndx));
	if (ret != KDUMP_OK)
		return ret;

	return ret;
}

typedef kdump_status walk_notes_fn(kdump_ctx_t *, void *, size_t);

static kdump_status
walk_elf_notes(kdump_ctx_t *ctx, walk_notes_fn *fn)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	unsigned i;
	struct fcache_chunk fch;
	kdump_status ret;

	for (i = 0; i < edp->num_note_segments; ++i) {
		struct load_segment *seg = edp->note_segments + i;

		ret = fcache_get_chunk(ctx->shared->fcache, &fch,
				       seg->filesz, seg->file_offset);
		if (ret != KDUMP_OK)
			return set_error(ctx, ret,
					 "Cannot read ELF notes at %llu",
					 (unsigned long long) seg->file_offset);

		ret = fn(ctx, fch.data, seg->filesz);
		fcache_put_chunk(&fch);
		if (ret != KDUMP_OK)
			return ret;
	}

	return KDUMP_OK;
}

static kdump_status
open_common(kdump_ctx_t *ctx)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	kdump_status ret;
	int i;

	if (!edp->num_load_segments && !edp->num_sections)
		return set_error(ctx, KDUMP_ERR_NOTIMPL, "No content found");

	/* process NOTE segments */
	ret = walk_elf_notes(ctx, process_noarch_notes);
	if (ret != KDUMP_OK)
		return ret;

	if (!isset_arch_name(ctx)) {
		uint_fast16_t mach = get_arch_machine(ctx);
		const char *arch = mach2arch(mach, edp->elfclass);
		if (arch) {
			ret = set_arch_name(ctx, arch);
			if (ret != KDUMP_OK)
				return ret;
		}
	}

	ret = walk_elf_notes(ctx, process_arch_notes);
	if (ret != KDUMP_OK)
		return ret;

	/* Check that physical addresses are usable */
	for (i = 0; i < edp->num_load_segments; ++i)
		if (edp->load_segments[i].phys)
			break;
	if (i >= edp->num_load_segments && i > 1)
		while (i--)
			edp->load_segments[i].phys = ADDRXLAT_ADDR_MAX;

	/* process LOAD segments */
	for (i = 0; i < edp->num_load_segments; ++i) {
		struct load_segment *seg = edp->load_segments + i;
		unsigned long pfn =
			(seg->phys + seg->filesz) >> get_page_shift(ctx);
		if (pfn > get_max_pfn(ctx))
			set_max_pfn(ctx, pfn);

		if (ctx->shared->arch_ops && ctx->shared->arch_ops->process_load) {
			ret = ctx->shared->arch_ops->process_load(
				ctx, seg->virt, seg->phys);
			if (ret != KDUMP_OK)
				return ret;
		}
	}

	for (i = 0; i < edp->num_sections; ++i) {
		struct section *sect = edp->sections + i;
		const char *name = strtab_entry(edp, sect->name_index);
		if (!strcmp(name, ".xen_pages"))
			edp->xen_pages_offset = sect->file_offset;
		else if (!strcmp(name, ".xen_p2m")) {
			set_xen_xlat(ctx, KDUMP_XEN_NONAUTO);
			ret = make_xen_pfn_map_nonauto(ctx, sect);
			if (ret != KDUMP_OK)
				return set_error(ctx, ret,
						 "Cannot create Xen P2M map");
		} else if (!strcmp(name, ".xen_pfn")) {
			set_xen_xlat(ctx, KDUMP_XEN_AUTO);
			ret = make_xen_pfn_map_auto(ctx, sect);
			if (ret != KDUMP_OK)
				return set_error(ctx, ret,
						 "Cannot create Xen PFN map");
		} else if (!strcmp(name, ".note.Xen")) {
			void *notes = read_elf_sect(ctx, sect);
			if (!notes)
				return KDUMP_ERR_SYSTEM;
			ret = process_notes(ctx, notes, sect->size);
			free(notes);
			if (ret != KDUMP_OK)
				return set_error(ctx, ret,
						 "Cannot process Xen notes");
		} else if (!strcmp(name, ".xen_prstatus")) {
			void *data = read_elf_sect(ctx, sect);
			if (!data)
				return KDUMP_ERR_SYSTEM;
			ret = ctx->shared->arch_ops->process_xen_prstatus(
				ctx, data, sect->size);
			free(data);
			if (ret != KDUMP_OK)
				return set_error(ctx, ret,
						 "Cannot process Xen prstatus");
		}
	}

	if (edp->xen_pages_offset) {
		set_xen_type(ctx, KDUMP_XEN_DOMAIN);
		if (!edp->xen_map_offset)
			return set_error(ctx, KDUMP_ERR_NOTIMPL,
					 "Missing Xen P2M mapping");
		ctx->shared->ops = &xc_core_elf_ops;
		set_addrspace_caps(ctx->shared,
				   ADDRXLAT_CAPS(ADDRXLAT_KPHYSADDR) |
				   ADDRXLAT_CAPS(ADDRXLAT_MACHPHYSADDR));
	}

	return KDUMP_OK;
}

static kdump_status
do_probe(kdump_ctx_t *ctx, void *hdr)
{
	unsigned char *eheader = hdr;
	Elf32_Ehdr *elf32 = hdr;
	Elf64_Ehdr *elf64 = hdr;
	struct elfdump_priv *edp;

	if (memcmp(eheader, ELFMAG, SELFMAG))
		return set_error(ctx, KDUMP_NOPROBE,
				 "Invalid ELF signature");

	edp = calloc(1, sizeof *edp);
	if (!edp)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot allocate ELF dump private data");
	ctx->shared->fmtdata = edp;

	set_addrspace_caps(ctx->shared,
			   ADDRXLAT_CAPS(ADDRXLAT_MACHPHYSADDR));

	switch (eheader[EI_DATA]) {
	case ELFDATA2LSB:
		set_byte_order(ctx, KDUMP_LITTLE_ENDIAN);
		break;
	case ELFDATA2MSB:
		set_byte_order(ctx, KDUMP_BIG_ENDIAN);
		break;
	default:
		return set_error(ctx, KDUMP_ERR_NOTIMPL,
				 "Unsupported ELF data format: %u",
				 eheader[EI_DATA]);
	}

        if ((elf32->e_ident[EI_CLASS] == ELFCLASS32) &&
	    (dump16toh(ctx, elf32->e_type) == ET_CORE) &&
	    (dump32toh(ctx, elf32->e_version) == EV_CURRENT)) {
		edp->elfclass = ELFCLASS32;
		set_file_description(ctx, "ELF32 dump");
		return init_elf32(ctx, elf32);
	} else if ((elf64->e_ident[EI_CLASS] == ELFCLASS64) &&
		   (dump16toh(ctx, elf64->e_type) == ET_CORE) &&
		   (dump32toh(ctx, elf64->e_version) == EV_CURRENT)) {
		edp->elfclass = ELFCLASS64;
		set_file_description(ctx, "ELF64 dump");
		return init_elf64(ctx, elf64);
	}

	return set_error(ctx, KDUMP_ERR_NOTIMPL,
			 "Unsupported ELF class: %u", elf32->e_ident[EI_CLASS]);
}

static kdump_status
elf_probe(kdump_ctx_t *ctx)
{
	struct fcache_chunk fch;
	kdump_status ret;

	ret = fcache_get_chunk(ctx->shared->fcache, &fch,
			       sizeof(Elf64_Ehdr), 0);
	if (ret != KDUMP_OK)
		return set_error(ctx, ret, "Cannot read dump header");

	ret = do_probe(ctx, fch.data);
	fcache_put_chunk(&fch);

	if (ret == KDUMP_OK)
		ret = open_common(ctx);

	if (ret != KDUMP_OK)
		elf_cleanup(ctx->shared);

	return ret;
}

static void
elf_cleanup(struct kdump_shared *shared)
{
	struct elfdump_priv *edp = shared->fmtdata;

	if (edp) {
		if (edp->load_segments)
			free(edp->load_segments);
		if (edp->sections)
			free(edp->sections);
		if (edp->strtab)
			free(edp->strtab);
		pfn2idx_map_free(&edp->xen_pfnmap);
		pfn2idx_map_free(&edp->xen_mfnmap);
		free(edp);
		shared->fmtdata = NULL;
	}
};

const struct format_ops elfdump_ops = {
	.name = "elf",
	.probe = elf_probe,
	.get_page = elf_get_page,
	.put_page = cache_put_page,
	.realloc_caches = def_realloc_caches,
	.cleanup = elf_cleanup,
};

static const struct format_ops xc_core_elf_ops = {
	.name = "xc_core_elf",
	.get_page = xc_get_page,
	.put_page = cache_put_page,
	.post_addrxlat = xc_post_addrxlat,
	.realloc_caches = def_realloc_caches,
	.cleanup = elf_cleanup,
};
