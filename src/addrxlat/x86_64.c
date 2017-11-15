/** @internal @file src/addrxlat/x86_64.c
 * @brief Routines specific to AMD64 and Intel 64.
 */
/* Copyright (C) 2016 Petr Tesarik <ptesarik@suse.com>

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

#include <stdlib.h>

#include "addrxlat-priv.h"

/* Maximum physical address bits (architectural limit) */
#define PHYSADDR_BITS_MAX	52
#define PHYSADDR_MASK		ADDR_MASK(PHYSADDR_BITS_MAX)

#define _PAGE_BIT_PRESENT	0
#define _PAGE_BIT_PSE		7

#define _PAGE_PRESENT	(1UL << _PAGE_BIT_PRESENT)
#define _PAGE_PSE	(1UL << _PAGE_BIT_PSE)

/* Maximum virtual address bits (architecture limit) */
#define VIRTADDR_BITS_MAX	48

/** Page shift (log2 4K). */
#define PAGE_SHIFT		12

/** Page mask. */
#define PAGE_MASK		ADDR_MASK(PAGE_SHIFT)

/** 2M page shift (log2 2M). */
#define PAGE_SHIFT_2M		21

/** 2M page mask. */
#define PAGE_MASK_2M		ADDR_MASK(PAGE_SHIFT_2M)

/** 1G page shift (log2 1G). */
#define PAGE_SHIFT_1G		30

/** 1G page mask. */
#define PAGE_MASK_1G		ADDR_MASK(PAGE_SHIFT_1G)

#define NONCANONICAL_START	((uint64_t)1<<(VIRTADDR_BITS_MAX-1))
#define NONCANONICAL_END	(~NONCANONICAL_START)
#define VIRTADDR_MAX		UINT64_MAX

/** Virtual address of the Xen machine-to-physical map. */
#define XEN_MACH2PHYS_ADDR	0xffff800000000000

/** Kernel text mapping (virtual address).
 * Note that the start address of this mapping has never changed, so this
 * constant applies to all kernel versions.
 */
#define LINUX_KTEXT_START	0xffffffff80000000

/** Possible ends of Linux kernel text mapping, in ascending order. */
static const addrxlat_addr_t linux_ktext_ends[] = {
	0xffffffff827fffff, /* 40M mapping (original) */
	0xffffffff87ffffff, /* 128M mapping (2.6.25+) */
	0xffffffff9fffffff, /* 512M mapping (2.6.26+) */
	0xffffffffbfffffff, /* 1G mapping with kASLR */
};

/* Original Linux layout (before 2.6.11) */
static const struct sys_region linux_layout_2_6_0[] = {
	/* 0x0000000000000000 - 0x0000007fffffffff     user space       */
	/* 0x0000008000000000 - 0x000000ffffffffff     guard hole       */
	{  0x0000010000000000,  0x000001ffffffffff, /* direct mapping   */
	   ADDRXLAT_SYS_METH_DIRECT, SYS_ACT_DIRECT },
	/* 0x0000020000000000 - 0x00007fffffffffff     unused hole      */
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xfffffeffffffffff     unused hole      */
	/* 0xffffff0000000000 - 0xffffff7fffffffff     vmalloc/ioremap  */
	/* 0xffffff8000000000 - 0xffffffff7fffffff     unused hole      */
	/* 0xffffffff80000000 - 0xffffffff827fffff     kernel text      */
	/* 0xffffffff82800000 - 0xffffffff9fffffff     unused hole      */
	/* 0xffffffffa0000000 - 0xffffffffafffffff     modules          */
	/* 0xffffffffb0000000 - 0xffffffffff5exxxx     unused hole      */
	/* 0xffffffffff5ed000 - 0xffffffffffdfffff     fixmap/vsyscalls */
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
	SYS_REGION_END
};

/* Linux layout introduced in 2.6.11 */
static const struct sys_region linux_layout_2_6_11[] = {
	/* 0x0000000000000000 - 0x00007fffffffffff     user space       */
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xffff80ffffffffff     guard hole       */
	{  0xffff810000000000,  0xffffc0ffffffffff, /* direct mapping   */
	   ADDRXLAT_SYS_METH_DIRECT, SYS_ACT_DIRECT },
	/* 0xffffc10000000000 - 0xffffc1ffffffffff     guard hole       */
	/* 0xffffc20000000000 - 0xffffe1ffffffffff     vmalloc/ioremap  */
	/* 0xffffe20000000000 - 0xffffe2ffffffffff     VMEMMAP          */
	/*					         (2.6.24+ only) */
	/* 0xffffe30000000000 - 0xffffffff7fffffff     unused hole      */
	/* 0xffffffff80000000 - 0xffffffff827fffff     kernel text      */
	/* 0xffffffff82800000 - 0xffffffff87ffffff     unused hole      */
	/* 0xffffffff88000000 - 0xffffffffffdfffff     modules and      */
	/*					        fixmap/vsyscall */
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
	SYS_REGION_END
};

/** Linux layout with hypervisor area, introduced in 2.6.27 */
static const struct sys_region linux_layout_2_6_27[] = {
	/* 0x0000000000000000 - 0x00007fffffffffff     user space       */
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xffff80ffffffffff     guard hole       */
	/* 0xffff810000000000 - 0xffff87ffffffffff     hypervisor area  */
	{  0xffff880000000000,  0xffffc0ffffffffff, /* direct mapping   */
	   ADDRXLAT_SYS_METH_DIRECT, SYS_ACT_DIRECT },
	/* 0xffffc10000000000 - 0xffffc1ffffffffff     guard hole       */
	/* 0xffffc20000000000 - 0xffffe1ffffffffff     vmalloc/ioremap  */
	/* 0xffffe20000000000 - 0xffffe2ffffffffff     VMEMMAP          */
	/* 0xffffe30000000000 - 0xffffffff7fffffff     unused hole      */
	/* 0xffffffff80000000 - 0xffffffff87ffffff     kernel text      */
	/* 0xffffffff88000000 - 0xffffffffffdfffff     modules and      */
	/*					        fixmap/vsyscall */
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
	SYS_REGION_END
};

/** Linux layout with 64T direct mapping, introduced in 2.6.31 */
static const struct sys_region linux_layout_2_6_31[] = {
	/* 0x0000000000000000 - 0x00007fffffffffff     user space       */
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xffff80ffffffffff     guard hole       */
	/* 0xffff810000000000 - 0xffff87ffffffffff     hypervisor area  */
	{  0xffff880000000000,  0xffffc7ffffffffff, /* direct mapping   */
	   ADDRXLAT_SYS_METH_DIRECT, SYS_ACT_DIRECT },
	/* 0xffffc80000000000 - 0xffffc8ffffffffff     guard hole       */
	/* 0xffffc90000000000 - 0xffffe8ffffffffff     vmalloc/ioremap  */
	/* 0xffffe90000000000 - 0xffffe9ffffffffff     guard hole       */
	/* 0xffffea0000000000 - 0xffffeaffffffffff     VMEMMAP          */
	/* 0xffffeb0000000000 - 0xffffffeeffffffff     unused hole      */
	/* 0xffffff0000000000 - 0xffffff7fffffffff     %esp fixup stack */
	/* 0xffffff8000000000 - 0xffffffeeffffffff     unused hole      */
	/* 0xffffffef00000000 - 0xfffffffeffffffff     EFI runtime      */
	/*						   (3.14+ only) */
	/* 0xffffffff00000000 - 0xffffffff7fffffff     guard hole       */
	/* 0xffffffff80000000 - 0xffffffff9fffffff     kernel text      */
	/* 0xffffffffa0000000 - 0xffffffffffdfffff     modules and      */
	/*					        fixmap/vsyscall */
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
	SYS_REGION_END
};

/** AMD64 (Intel 64) page table step function.
 * @param step  Current step state.
 * @returns     Error status.
 */
addrxlat_status
pgt_x86_64(addrxlat_step_t *step)
{
	static const char pgt_full_name[][16] = {
		"Page",
		"Page table",
		"Page directory",
		"PDPT table",
	};
	static const char pte_name[][4] = {
		"pte",
		"pmd",
		"pud",
		"pgd",
	};
	addrxlat_status status;

	status = read_pte(step);
	if (status != ADDRXLAT_OK)
		return status;

	if (!(step->raw.pte & _PAGE_PRESENT))
		return set_error(step->ctx, ADDRXLAT_ERR_NOTPRESENT,
				 "%s not present: %s[%u] = 0x%" ADDRXLAT_PRIxPTE,
				 pgt_full_name[step->remain - 1],
				 pte_name[step->remain - 1],
				 (unsigned) step->idx[step->remain],
				 step->raw.pte);

	step->base.addr = step->raw.pte & PHYSADDR_MASK;
	step->base.as = step->meth->target_as;

	if (step->remain == 3 && (step->raw.pte & _PAGE_PSE)) {
		step->base.addr &= ~PAGE_MASK_1G;
		return pgt_huge_page(step);
	}

	if (step->remain == 2 && (step->raw.pte & _PAGE_PSE)) {
		step->base.addr &= ~PAGE_MASK_2M;
		return pgt_huge_page(step);
	}

	step->base.addr &= ~PAGE_MASK;
	if (step->remain == 1)
		step->elemsz = 1;

	return ADDRXLAT_OK;
}

/** Get Linux virtual memory layout by kernel version.
 * @param ver  Version code.
 * @returns    Layout definition, or @c NULL.
 */
static const struct sys_region *
linux_layout_by_ver(unsigned version_code)
{
#define LINUX_LAYOUT_BY_VER(a, b, c)			\
	if (version_code >= ADDRXLAT_VER_LINUX(a, b, c))	\
		return linux_layout_ ## a ## _ ## b ## _ ## c

	LINUX_LAYOUT_BY_VER(2, 6, 31);
	LINUX_LAYOUT_BY_VER(2, 6, 27);
	LINUX_LAYOUT_BY_VER(2, 6, 11);
	LINUX_LAYOUT_BY_VER(2, 6, 0);

	return NULL;
}

/** Check whether a virtual address is mapped to a physical address.
 * @param sys    Translation system object.
 * @param ctx    Address translation context.
 * @param addr   Address to be checked.
 * @returns      Non-zero if the address can be translated.
 */
static int
is_mapped(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx,
	  addrxlat_addr_t addr)
{
	addrxlat_step_t step;
	addrxlat_status status;

	step.ctx = ctx;
	step.sys = sys;
	step.meth = &sys->meth[ADDRXLAT_SYS_METH_PGT];
	status = internal_launch(&step, addr);

	if (status == ADDRXLAT_OK)
		status = internal_walk(&step);

	clear_error(ctx);
	return status == ADDRXLAT_OK;
}

/** Check whether an address looks like start of direct mapping.
 * @param sys    Translation system.
 * @param ctx    Address translation context.
 * @param addr   Address to be checked.
 * @returns      Non-zero if the address maps to physical address 0.
 */
static int
is_directmap(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx,
	     addrxlat_addr_t addr)
{
	addrxlat_fulladdr_t faddr;
	addrxlat_status status;

	faddr.addr = addr;
	faddr.as = ADDRXLAT_KVADDR;
	status = internal_fulladdr_conv(&faddr, ADDRXLAT_KPHYSADDR, ctx, sys);
	clear_error(ctx);
	return status == ADDRXLAT_OK && faddr.addr == 0;
}

/** Get virtual memory layout by walking page tables.
 * @param sys    Translation system object.
 * @param ctx    Address translation context.
 * @returns      Memory layout, or @c NULL if undetermined.
 */
static const struct sys_region *
linux_layout_by_pgt(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx)
{
	/* Only pre-2.6.11 kernels had this direct mapping */
	if (is_directmap(sys, ctx, 0x0000010000000000))
		return linux_layout_2_6_0;

	/* Only kernels between 2.6.11 and 2.6.27 had this direct mapping */
	if (is_directmap(sys, ctx, 0xffff810000000000))
		return linux_layout_2_6_11;

	/* Only 2.6.31+ kernels map VMEMMAP at this address */
	if (is_mapped(sys, ctx, 0xffffea0000000000))
		return linux_layout_2_6_31;

	/* Sanity check for 2.6.27+ direct mapping */
	if (is_directmap(sys, ctx, 0xffff880000000000))
		return linux_layout_2_6_27;

	return NULL;
}

/** Translate a virtual address using page tables.
 * @param sys    Translation system object.
 * @param ctx    Address translation object.
 * @param addr   Address to be translated.
 * @returns      Error status.
 */
static addrxlat_status
vtop_pgt(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx, addrxlat_addr_t *addr)
{
	addrxlat_step_t step;
	addrxlat_status status;

	step.ctx = ctx;
	step.sys = sys;
	step.meth = &sys->meth[ADDRXLAT_SYS_METH_PGT];
	status = internal_launch(&step, *addr);
	if (status != ADDRXLAT_OK)
		return status;

	status = internal_walk(&step);
	if (status != ADDRXLAT_OK)
		return status;

	status = internal_fulladdr_conv(&step.base, ADDRXLAT_KPHYSADDR,
					ctx, sys);
	if (status == ADDRXLAT_OK)
		*addr = step.base.addr;

	return status;
}

/** Set Linux kernel text mapping offset.
 * @param sys    Translation system object.
 * @param ctx    Address translation object.
 * @param vaddr  Any valid kernel text virtual address.
 * @returns      Error status.
 */
static addrxlat_status
set_ktext_offset(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx,
		 addrxlat_addr_t vaddr)
{
	addrxlat_addr_t paddr;
	addrxlat_meth_t *meth;
	addrxlat_status status;

	paddr = vaddr;
	status = vtop_pgt(sys, ctx, &paddr);
	if (status != ADDRXLAT_OK)
		return status;

	meth = &sys->meth[ADDRXLAT_SYS_METH_KTEXT];
	meth->kind = ADDRXLAT_LINEAR;
	meth->target_as = ADDRXLAT_KPHYSADDR;
	meth->param.linear.off = paddr - vaddr;
	return ADDRXLAT_OK;
}

/** Fall back to page table mapping if needed.
 * @param sys    Translation system object.
 * @param idx    Translation method index.
 *
 * If the corresponding translation method is undefined, fall back
 * to hardware page table mapping.
 */
static void
set_pgt_fallback(addrxlat_sys_t *sys, addrxlat_sys_meth_t idx)
{
	addrxlat_meth_t *meth = &sys->meth[idx];
	if (meth->kind == ADDRXLAT_NOMETH)
		*meth = sys->meth[ADDRXLAT_SYS_METH_PGT];
}

/** Set up Linux kernel reverse direct mapping on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
addrxlat_status
linux_rdirect_map(struct os_init_data *ctl)
{
	static const struct {
		addrxlat_addr_t first;
		addrxlat_addr_t last;
	} ranges[] = {
		{ 0x0000010000000000, 0x000001ffffffffff },
		{ 0xffff810000000000, 0xffffc0ffffffffff },
		{ 0xffff880000000000, 0xffffc0ffffffffff },
		{ 0xffff880000000000, 0xffffc7ffffffffff },
	};

	int i;

	if (!ctl->ctx->cb.read64 ||
	    !(ctl->ctx->cb.read_caps & ADDRXLAT_CAPS(ADDRXLAT_KVADDR)))
		return ADDRXLAT_ERR_NOMETH;

	for (i = 0; i < ARRAY_SIZE(ranges); ++i) {
		struct sys_region layout[2];
		addrxlat_status status;

		ctl->sys->meth[ADDRXLAT_SYS_METH_DIRECT].param.linear.off =
			-ranges[i].first;

		layout[0].first = 0;
		layout[0].last = ranges[i].last - ranges[i].first;
		layout[0].meth = ADDRXLAT_SYS_METH_RDIRECT;
		layout[0].act = SYS_ACT_RDIRECT;
		layout[1].meth = ADDRXLAT_SYS_METH_NUM;
		status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KPHYS_DIRECT,
					layout);
		if (status != ADDRXLAT_OK)
			return set_error(ctl->ctx, status,
					 "Cannot set up Linux kernel direct mapping");

		if (is_directmap(ctl->sys, ctl->ctx, ranges[i].first))
			return ADDRXLAT_OK;

		/* rollback */
		ctl->sys->meth[ADDRXLAT_SYS_METH_RDIRECT].kind =
			ADDRXLAT_NOMETH;
		internal_map_decref(
			ctl->sys->map[ADDRXLAT_SYS_MAP_KPHYS_DIRECT]);
		ctl->sys->map[ADDRXLAT_SYS_MAP_KPHYS_DIRECT] = NULL;
	}

	return ADDRXLAT_NOMETH;
}

/** The beginning of the kernel text virtual mapping may not be mapped
 * for various reasons. Let's use an offset of 16M to be safe.
 */
#define LINUX_KTEXT_SKIP		(16ULL << 20)

/** Xen kernels are loaded low in memory. The ktext mapping may not go up
 * to 16M then. Let's use 1M, because Xen kernel should take up at least
 * 1M of RAM, and this value also covers kernels loaded at 1M (so this code
 * may be potentially reused for ia32).
 */
#define LINUX_KTEXT_SKIP_alt		(1ULL << 20)

/** Set up Linux kernel text translation method.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
linux_ktext_meth(struct os_init_data *ctl)
{
	addrxlat_addr_t stext;
	addrxlat_status status;

	if (ctl->popt.val[OPT_physbase].set) {
		addrxlat_meth_t *meth =
			&ctl->sys->meth[ADDRXLAT_SYS_METH_KTEXT];

		meth->kind = ADDRXLAT_LINEAR;
		meth->target_as = ADDRXLAT_KPHYSADDR;
		meth->param.linear.off = ctl->popt.val[OPT_physbase].addr -
			LINUX_KTEXT_START;
		return ADDRXLAT_OK;
	}

	status = get_symval(ctl->ctx, "_stext", &stext);
	if (status == ADDRXLAT_ERR_NODATA)
		stext = LINUX_KTEXT_START + LINUX_KTEXT_SKIP;
	else if (status != ADDRXLAT_OK)
		return status;

	status = set_ktext_offset(ctl->sys, ctl->ctx, stext);
	if (status == ADDRXLAT_ERR_NOTPRESENT ||
	    status == ADDRXLAT_ERR_NODATA) {
		clear_error(ctl->ctx);
		stext = LINUX_KTEXT_START + LINUX_KTEXT_SKIP_alt;
		status = set_ktext_offset(ctl->sys, ctl->ctx, stext);
	}
	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status, "Cannot translate ktext");
	return status;
}

/** Find the kernel text mapping extents.
 * @param ctl   Initialization data.
 * @param low   Lowest ktext address (set on successful return).
 * @param high  Highest ktext address (set on successful return).
 * @returns     Error status.
 */
addrxlat_status
linux_ktext_extents(struct os_init_data *ctl,
		    addrxlat_addr_t *low, addrxlat_addr_t *high)
{
	addrxlat_addr_t linearoff;
	addrxlat_step_t step;
	unsigned i;
	addrxlat_status status;

	step.ctx = ctl->ctx;
	step.sys = ctl->sys;
	step.meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	*low = LINUX_KTEXT_START;
	status = lowest_mapped(
		&step, low,
		linux_ktext_ends[ARRAY_SIZE(linux_ktext_ends) - 1]);
	if (status != ADDRXLAT_OK)
		return status;
	status = internal_fulladdr_conv(&step.base, ADDRXLAT_KPHYSADDR,
					step.ctx, step.sys);
	if (status != ADDRXLAT_OK)
		return status;

	linearoff = step.base.addr - *low;
	if (ctl->popt.val[OPT_physbase].set &&
	    ctl->popt.val[OPT_physbase].addr !=
	    linearoff + LINUX_KTEXT_START)
			return set_error(ctl->ctx, ADDRXLAT_ERR_INVALID,
					 "physbase=0x%"ADDRXLAT_PRIxADDR
					 " actual=0x%"ADDRXLAT_PRIxADDR,
					 ctl->popt.val[OPT_physbase].addr,
					 linearoff + LINUX_KTEXT_START);

	for (i = 0; i < ARRAY_SIZE(linux_ktext_ends); ++i) {
		if (linux_ktext_ends[i] < *low)
			continue;
		*high = linux_ktext_ends[i];
		status = highest_mapped(&step, high, *low);
		if (status != ADDRXLAT_OK)
			return status;
		if (i) {
			status = internal_fulladdr_conv(
				&step.base, ADDRXLAT_KPHYSADDR,
				step.ctx, step.sys);
			if (status != ADDRXLAT_OK)
				return status;
			if (step.base.addr - *high != linearoff)
				*high = linux_ktext_ends[i - 1];
		}
		if (*high < linux_ktext_ends[i])
			break;
	}
	return ADDRXLAT_OK;
}

/** Set up Linux kernel text mapping on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
linux_ktext_map(struct os_init_data *ctl)
{
	addrxlat_range_t range;
	addrxlat_meth_t *meth;
	addrxlat_addr_t low, high;
	addrxlat_status status;

	status = linux_ktext_meth(ctl);
	if (status != ADDRXLAT_OK &&
	    status != ADDRXLAT_ERR_NOMETH &&
	    status != ADDRXLAT_ERR_NODATA &&
	    status != ADDRXLAT_ERR_NOTPRESENT)
		return status;
	clear_error(ctl->ctx);

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	if (meth->kind == ADDRXLAT_PGT &&
	    meth->param.pgt.root.as == ADDRXLAT_KVADDR) {
		/* minimal ktext mapping for the root page table */
		range.endoff = PAGE_MASK;
		range.meth = ADDRXLAT_SYS_METH_KTEXT;
		status = internal_map_set(
			ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS],
			meth->param.pgt.root.addr, &range);
		if (status != ADDRXLAT_OK)
			return set_error(ctl->ctx, status, "Cannot set up %s",
					 "minimal Linux kernel text mapping");
	}

	status = linux_ktext_extents(ctl, &low, &high);
	if (status == ADDRXLAT_ERR_NOMETH ||
	    status == ADDRXLAT_ERR_NODATA ||
	    status == ADDRXLAT_ERR_NOTPRESENT)
		return ADDRXLAT_OK; /* Non-fatal here. */
	else if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status,
				 "Linux kernel text search failed");

	range.meth = ADDRXLAT_SYS_METH_KTEXT;
	range.endoff = high - low;
	status = internal_map_set(
		ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS], low, &range);
	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status, "Cannot set up %s",
				 "Linux kernel text mapping");

	return ADDRXLAT_OK;
}

/** Initialize a translation map for Linux on x86_64.
 * @param ctl  Initialization data.
 * @param m2p  Virtual address of the machine-to-physical array.
 */
static void
set_xen_mach2phys(struct os_init_data *ctl, addrxlat_addr_t m2p)
{
	addrxlat_meth_t *meth =
		&ctl->sys->meth[ADDRXLAT_SYS_METH_MACHPHYS_KPHYS];

	meth->kind = ADDRXLAT_MEMARR;
	meth->target_as = ADDRXLAT_KPHYSADDR;
	meth->param.memarr.base.as = ADDRXLAT_KVADDR;
	meth->param.memarr.base.addr = m2p;
	meth->param.memarr.shift = PAGE_SHIFT;
	meth->param.memarr.elemsz = sizeof(uint64_t);
	meth->param.memarr.valsz = sizeof(uint64_t);
}

/** Initialize Xen p2m translation.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
set_xen_p2m(struct os_init_data *ctl)
{
	static const addrxlat_paging_form_t xen_p2m_pf = {
		.pte_format = ADDRXLAT_PTE_PFN64,
		.nfields = 4,
		.fieldsz = { 12, 9, 9, 9 }
	};

	addrxlat_addr_t p2m_maddr;
	addrxlat_map_t *map;
	addrxlat_meth_t *meth;
	addrxlat_range_t range;
	addrxlat_status status;

	map = ctl->sys->map[ADDRXLAT_SYS_MAP_KPHYS_MACHPHYS];
	map_clear(map);
	if (!ctl->popt.val[OPT_xen_p2m_mfn].set)
		return ADDRXLAT_OK; /* leave undefined */
	p2m_maddr = ctl->popt.val[OPT_xen_p2m_mfn].num << PAGE_SHIFT;

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_KPHYS_MACHPHYS];
	meth->kind = ADDRXLAT_PGT;
	meth->target_as = ADDRXLAT_MACHPHYSADDR;
	meth->param.pgt.root.addr = p2m_maddr;
	meth->param.pgt.root.as = ADDRXLAT_MACHPHYSADDR;
	meth->param.pgt.pf = xen_p2m_pf;

	range.endoff = paging_max_index(&xen_p2m_pf);
	range.meth = ADDRXLAT_SYS_METH_KPHYS_MACHPHYS;
	status = internal_map_set(map, 0, &range);
	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status,
				 "Cannot allocate Xen p2m map");

	return ADDRXLAT_OK;
}

/** Initialize a translation map for Linux on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
map_linux_x86_64(struct os_init_data *ctl)
{
	static const struct sym_spec pgtspec[] = {
		{ ADDRXLAT_SYM_REG, ADDRXLAT_MACHPHYSADDR, "cr3" },
		{ ADDRXLAT_SYM_VALUE, ADDRXLAT_KVADDR, "init_top_pgt" },
		{ ADDRXLAT_SYM_VALUE, ADDRXLAT_KVADDR, "init_level4_pgt" },
		{ ADDRXLAT_SYM_NONE }
	};

	const struct sys_region *layout;
	addrxlat_status status;

	sys_sym_pgtroot(ctl, pgtspec);

	if (ctl->popt.val[OPT_xen_xlat].set &&
	    ctl->popt.val[OPT_xen_xlat].num) {
		status = set_xen_p2m(ctl);
		if (status != ADDRXLAT_OK)
			return status;

		set_xen_mach2phys(ctl, XEN_MACH2PHYS_ADDR);
	}

	if (!(ctl->ctx->cb.read_caps & ADDRXLAT_CAPS(ADDRXLAT_MACHPHYSADDR)) &&
	    !(ctl->ctx->cb.read_caps & ADDRXLAT_CAPS(ADDRXLAT_KPHYSADDR))) {
		status = linux_rdirect_map(ctl);
		if (status != ADDRXLAT_OK &&
		    status != ADDRXLAT_ERR_NOMETH &&
		    status != ADDRXLAT_ERR_NODATA &&
		    status != ADDRXLAT_ERR_NOTPRESENT)
			return status;

		clear_error(ctl->ctx);
	}

	status = linux_ktext_map(ctl);
	if (status != ADDRXLAT_OK)
		return status;

	layout = linux_layout_by_pgt(ctl->sys, ctl->ctx);

	if (!layout && ctl->osdesc->ver)
		layout = linux_layout_by_ver(ctl->osdesc->ver);
	if (layout) {
		status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KV_PHYS, layout);
		if (status != ADDRXLAT_OK)
			return status;
	}

	set_pgt_fallback(ctl->sys, ADDRXLAT_SYS_METH_KTEXT);

	return ADDRXLAT_OK;
}

/** Xen direct mapping virtual address. */
#define XEN_DIRECTMAP	0xffff830000000000

/** Xen direct mapping virtual address with Xen 4.6+ BIGMEM. */
#define XEN_DIRECTMAP_BIGMEM	0xffff848000000000

/** Xen 1TB directmap size. */
#define XEN_DIRECTMAP_SIZE_1T	(1ULL << 40)

/** Xen 3.5TB directmap size (BIGMEM). */
#define XEN_DIRECTMAP_SIZE_3_5T	(3584ULL << 30)

/** Xen 5TB directmap size. */
#define XEN_DIRECTMAP_SIZE_5T	(5ULL << 40)

/** Xen 3.2-4.0 text virtual address. */
#define XEN_TEXT_3_2	0xffff828c80000000

/** Xen text virtual address (only during 4.0 development). */
#define XEN_TEXT_4_0dev	0xffff828880000000

/** Xen 4.0-4.3 text virtual address. */
#define XEN_TEXT_4_0	0xffff82c480000000

/** Xen 4.3-4.4 text virtual address. */
#define XEN_TEXT_4_3	0xffff82c4c0000000

/** Xen 4.4+ text virtual address. */
#define XEN_TEXT_4_4	0xffff82d080000000

/** Xen text mapping size. Always 1GB. */
#define XEN_TEXT_SIZE	(1ULL << 30)

/** Check whether an address looks like Xen text mapping.
 * @param ctl    Initialization data.
 * @param addr   Address to be checked.
 * @returns      Non-zero if the address maps to a 2M page.
 */
static int
is_xen_ktext(struct os_init_data *ctl, addrxlat_addr_t addr)
{
	addrxlat_step_t step;
	addrxlat_status status;
	unsigned steps = 0;

	step.ctx = ctl->ctx;
	step.sys = ctl->sys;
	step.meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	status = internal_launch(&step, addr);
	while (status == ADDRXLAT_OK && step.remain) {
		++steps;
		status = internal_step(&step);
	}

	clear_error(ctl->ctx);

	return status == ADDRXLAT_OK && steps == 4;
}

/** Initialize temporary mapping to make the page table usable.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
setup_xen_pgt(struct os_init_data *ctl)
{
	static const struct sym_spec pgtspec[] = {
		{ ADDRXLAT_SYM_REG, ADDRXLAT_MACHPHYSADDR, "cr3" },
		{ ADDRXLAT_SYM_VALUE, ADDRXLAT_KVADDR, "pgd_l4" },
		{ ADDRXLAT_SYM_NONE }
	};

	struct sys_region layout[2];
	addrxlat_meth_t *meth;
	addrxlat_addr_t pgt;
	addrxlat_off_t off;
	addrxlat_status status;

	status = sys_sym_pgtroot(ctl, pgtspec);
	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	if (meth->param.pgt.root.as != ADDRXLAT_KVADDR)
		return status;	/* either unset or physical */

	pgt = meth->param.pgt.root.addr;
	if (pgt >= XEN_DIRECTMAP) {
		off = -XEN_DIRECTMAP;
	} else if (ctl->popt.val[OPT_physbase].set) {
		addrxlat_addr_t xen_virt_start = pgt & ~(XEN_TEXT_SIZE - 1);
		off = ctl->popt.val[OPT_physbase].addr - xen_virt_start;
	} else
		return ADDRXLAT_ERR_NODATA;

	/* Temporary linear mapping just for the page table */
	layout[0].first = pgt;
	layout[0].last = pgt + (1ULL << PAGE_SHIFT) - 1;
	layout[0].meth = ADDRXLAT_SYS_METH_KTEXT;
	layout[0].act = SYS_ACT_NONE;

	layout[1].meth = ADDRXLAT_SYS_METH_NUM;

	status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KV_PHYS, layout);
	if (status != ADDRXLAT_OK)
		return status;

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_KTEXT];
	meth->kind = ADDRXLAT_LINEAR;
	meth->target_as = ADDRXLAT_KPHYSADDR;
	meth->param.linear.off = off;
	return ADDRXLAT_OK;
}

/** Initialize a translation map for Xen on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
map_xen_x86_64(struct os_init_data *ctl)
{
	struct sys_region layout[4];
	addrxlat_status status;

	layout[0].first = XEN_DIRECTMAP;
	layout[0].last = XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_5T - 1;
	layout[0].meth = ADDRXLAT_SYS_METH_DIRECT;
	layout[0].act = SYS_ACT_DIRECT;

	layout[1].meth = ADDRXLAT_SYS_METH_KTEXT;
	layout[1].act = SYS_ACT_NONE;

	layout[2].meth = ADDRXLAT_SYS_METH_NUM;

	setup_xen_pgt(ctl);

	if (is_directmap(ctl->sys, ctl->ctx, XEN_DIRECTMAP)) {
		if (is_xen_ktext(ctl, XEN_TEXT_4_4))
			layout[1].first = XEN_TEXT_4_4;
		else if (is_xen_ktext(ctl, XEN_TEXT_4_3))
			layout[1].first = XEN_TEXT_4_3;
		else if (is_xen_ktext(ctl, XEN_TEXT_4_0))
			layout[1].first = XEN_TEXT_4_0;
		else if (is_xen_ktext(ctl, XEN_TEXT_3_2)) {
			layout[0].last =
				XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_1T - 1;
			layout[1].first = XEN_TEXT_3_2;
		} else if (is_xen_ktext(ctl, XEN_TEXT_4_0dev))
			layout[1].first = XEN_TEXT_4_0dev;
		else {
			layout[0].last =
				XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_1T - 1;
			layout[1].meth = ADDRXLAT_SYS_METH_NUM;
		}
	} else if (is_directmap(ctl->sys, ctl->ctx, XEN_DIRECTMAP_BIGMEM)) {
		layout[0].first = XEN_DIRECTMAP_BIGMEM;
		layout[0].last =
			XEN_DIRECTMAP_BIGMEM + XEN_DIRECTMAP_SIZE_3_5T - 1;
		layout[1].first = XEN_TEXT_4_4;
	} else if (ctl->osdesc->ver >= ADDRXLAT_VER_XEN(4, 0)) {
		/* !BIGMEM is assumed for Xen 4.6+. Can we do better? */

		if (ctl->osdesc->ver >= ADDRXLAT_VER_XEN(4, 4))
			layout[1].first = XEN_TEXT_4_4;
		else if (ctl->osdesc->ver >= ADDRXLAT_VER_XEN(4, 3))
			layout[1].first = XEN_TEXT_4_3;
		else
			layout[1].first = XEN_TEXT_4_0;
	} else if (ctl->osdesc->ver) {
		layout[0].last =
			XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_1T - 1;

		if (ctl->osdesc->ver >= ADDRXLAT_VER_XEN(3, 2))
			layout[1].first = XEN_TEXT_3_2;
		else
			/* Prior to Xen 3.2, text was in direct mapping. */
			layout[1].meth = ADDRXLAT_SYS_METH_NUM;
	} else
		return ADDRXLAT_OK;

	layout[1].last = layout[1].first + XEN_TEXT_SIZE - 1;

	status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KV_PHYS, layout);
	if (status != ADDRXLAT_OK)
		return status;

	if (layout[1].meth == ADDRXLAT_SYS_METH_KTEXT) {
		set_ktext_offset(ctl->sys, ctl->ctx, layout[1].first);
		clear_error(ctl->ctx);
		set_pgt_fallback(ctl->sys, ADDRXLAT_SYS_METH_KTEXT);
	}

	return ADDRXLAT_OK;
}

/** Generic x86_64 layout */
static const struct sys_region layout_generic[] = {
	{  0,  NONCANONICAL_START - 1,		/* lower half       */
	   ADDRXLAT_SYS_METH_PGT },
	/* NONCANONICAL_START - NONCANONICAL_END   non-canonical    */
	{  NONCANONICAL_END + 1,  VIRTADDR_MAX,	/* higher half      */
	   ADDRXLAT_SYS_METH_PGT },
	SYS_REGION_END
};

/** Initialize a translation map for an x86_64 OS.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
addrxlat_status
sys_x86_64(struct os_init_data *ctl)
{
	static const addrxlat_paging_form_t x86_64_pf = {
		.pte_format = ADDRXLAT_PTE_X86_64,
		.nfields = 5,
		.fieldsz = { 12, 9, 9, 9, 9 }
	};
	addrxlat_map_t *map;
	addrxlat_meth_t *meth;
	addrxlat_status status;

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	meth->kind = ADDRXLAT_PGT;
	meth->target_as = ADDRXLAT_MACHPHYSADDR;
	if (ctl->popt.val[OPT_rootpgt].set)
		meth->param.pgt.root = ctl->popt.val[OPT_rootpgt].fulladdr;
	else
		meth->param.pgt.root.as = ADDRXLAT_NOADDR;
	meth->param.pgt.pf = x86_64_pf;

	status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_HW, layout_generic);
	if (status != ADDRXLAT_OK)
		return status;

	map = internal_map_copy(ctl->sys->map[ADDRXLAT_SYS_MAP_HW]);
	if (!map)
		return set_error(ctl->ctx, ADDRXLAT_ERR_NOMEM,
				 "Cannot duplicate hardware mapping");
	ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS] = map;

	status = sys_set_physmaps(ctl, PHYSADDR_MASK);
	if (status != ADDRXLAT_OK)
		return status;

	switch (ctl->osdesc->type) {
	case ADDRXLAT_OS_LINUX:
		return map_linux_x86_64(ctl);

	case ADDRXLAT_OS_XEN:
		return map_xen_x86_64(ctl);

	default:
		return ADDRXLAT_OK;
	}
}
