/* Functions for the x86-64 architecture.
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

#include <stdint.h>
#include <stdlib.h>
#include <linux/version.h>

#define ELF_NGREG 27

/* Maximum virtual address bits (architecture limit) */
#define VIRTADDR_BITS_MAX	48

#define NONCANONICAL_START	((uint64_t)1<<(VIRTADDR_BITS_MAX-1))
#define NONCANONICAL_END	(~NONCANONICAL_START)
#define VIRTADDR_MAX		UINT64_MAX

#define __START_KERNEL_map	0xffffffff80000000ULL

/* This constant is not the maximum physical load offset. This is the
 * maximum expected value of the PHYSICAL_START config option, which
 * defaults to 0x1000000. A relocatable kernel can be loaded anywhere
 * regardless of this config option. It is useful only for non-relocatable
 * kernels, and it moves the kernel text both in physical and virtual
 * address spaces. That means, the kernel must never overlap with the
 * following area in virtual address space (kernel modules). The virtual
 * memory layout has changed several times, but the minimum distance from
 * kernel modules has been 128M (the following constants). On kernel
 * versions where the distance is 512M, PHYSICAL_START can be higher than
 * this value. The check in process_load() will fail in such configurations.
 *
 * In other words, this constant is a safe value that will prevent
 * mistaking a kernel module LOAD for kernel text even on kernels
 * where the gap is only 128M.
 */
#define MAX_PHYSICAL_START	0x0000000008000000ULL

struct region_def {
	kdump_vaddr_t first, last;
	kdump_xlat_t xlat;
	kdump_vaddr_t phys_off;
};

/* Original layout (before 2.6.11) */
static const struct region_def mm_layout_2_6_0[] = {
	{  0x0000000000000000,  0x0000007fffffffff, /* user space       */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0x0000008000000000 - 0x000000ffffffffff     guard hole       */
	{  0x0000010000000000,  0x000001ffffffffff, /* direct mapping   */
	   KDUMP_XLAT_DIRECT,   0x0000010000000000 },
	/* 0x0000020000000000 - 0x00007fffffffffff     unused hole      */
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xfffffeffffffffff     unused hole      */
	{  0xffffff0000000000,  0xffffff7fffffffff, /* vmalloc/ioremap  */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0xffffff8000000000 - 0xffffffff7fffffff     unused hole      */
	{  0xffffffff80000000,  0xffffffff827fffff, /* kernel text      */
	   KDUMP_XLAT_KTEXT,    0xffffffff80000000 },
	/* 0xffffffff82800000 - 0xffffffff9fffffff     unused hole      */
	{  0xffffffffa0000000,  0xffffffffafffffff, /* modules          */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0xffffffffb0000000 - 0xffffffffff5exxxx     unused hole      */
	{  0xffffffffff5ed000,  0xffffffffffdfffff, /* fixmap/vsyscalls */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
};

/* New layout introduced in 2.6.11 */
static const struct region_def mm_layout_2_6_11[] = {
	{  0x0000000000000000,  0x00007fffffffffff, /* user space       */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xffff80ffffffffff     guard hole       */
	{  0xffff810000000000,  0xffffc0ffffffffff, /* direct mapping   */
	   KDUMP_XLAT_DIRECT,   0xffff810000000000 },
	/* 0xffffc10000000000 - 0xffffc1ffffffffff     guard hole       */
	{  0xffffc20000000000,  0xffffe1ffffffffff, /* vmalloc/ioremap  */
	   KDUMP_XLAT_VTOP,     0 },
	{  0xffffe20000000000,  0xffffe2ffffffffff, /* VMEMMAP          */
	   KDUMP_XLAT_VTOP,     0 },		    /*   (2.6.24+ only) */
	/* 0xffffe30000000000 - 0xffffffff7fffffff     unused hole      */
	{  0xffffffff80000000,  0xffffffff827fffff, /* kernel text      */
	   KDUMP_XLAT_KTEXT,    0xffffffff80000000 },
	/* 0xffffffff82800000 - 0xffffffff87ffffff     unused hole      */
	{  0xffffffff88000000,  0xffffffffffdfffff, /* modules and      */
	   KDUMP_XLAT_VTOP,     0 },		    /*  fixmap/vsyscall */
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
};

static const struct region_def mm_layout_2_6_27[] = {
	{  0x0000000000000000,  0x00007fffffffffff, /* user space       */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xffff80ffffffffff     guard hole       */
	/* 0xffff810000000000 - 0xffff87ffffffffff     hypervisor area  */
	{  0xffff880000000000,  0xffffc0ffffffffff, /* direct mapping   */
	   KDUMP_XLAT_DIRECT,   0xffff880000000000 },
	/* 0xffffc10000000000 - 0xffffc1ffffffffff     guard hole       */
	{  0xffffc20000000000,  0xffffe1ffffffffff, /* vmalloc/ioremap  */
	   KDUMP_XLAT_VTOP,     0 },
	{  0xffffe20000000000,  0xffffe2ffffffffff, /* VMEMMAP          */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0xffffe30000000000 - 0xffffffff7fffffff     unused hole      */
	{  0xffffffff80000000,  0xffffffff827fffff, /* kernel text      */
	   KDUMP_XLAT_KTEXT,    0xffffffff80000000 },
	/* 0xffffffff82800000 - 0xffffffff87ffffff     unused hole      */
	{  0xffffffff88000000,  0xffffffffffdfffff, /* modules and      */
	   KDUMP_XLAT_VTOP,     0 },		    /*  fixmap/vsyscall */
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
};

static const struct region_def mm_layout_2_6_31[] = {
	{  0x0000000000000000,  0x00007fffffffffff, /* user space       */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0x0000800000000000 - 0xffff7fffffffffff     non-canonical    */
	/* 0xffff800000000000 - 0xffff80ffffffffff     guard hole       */
	/* 0xffff810000000000 - 0xffff87ffffffffff     hypervisor area  */
	{  0xffff880000000000,  0xffffc7ffffffffff, /* direct mapping   */
	   KDUMP_XLAT_DIRECT,   0xffff880000000000 },
	/* 0xffffc80000000000 - 0xffffc8ffffffffff     guard hole       */
	{  0xffffc90000000000,  0xffffe8ffffffffff, /* vmalloc/ioremap  */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0xffffe90000000000 - 0xffffe9ffffffffff     guard hole       */
	{  0xffffea0000000000,  0xffffeaffffffffff, /* VMEMMAP          */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0xffffeb0000000000 - 0xffffffeeffffffff     unused hole      */
	{  0xffffff0000000000,  0xffffff7fffffffff, /* %esp fixup stack */
	   KDUMP_XLAT_VTOP,     0 },
	/* 0xffffff8000000000 - 0xffffffeeffffffff     unused hole      */
	{  0xffffffef00000000,  0xfffffffeffffffff, /* EFI runtime      */
	   KDUMP_XLAT_VTOP,     0 },		    /*     (3.14+ only) */
	/* 0xffffffff00000000 - 0xffffffff7fffffff     guard hole       */
	{  0xffffffff80000000,  0xffffffff827fffff, /* kernel text      */
	   KDUMP_XLAT_KTEXT,    0xffffffff80000000 },
	/* 0xffffffff82800000 - 0xffffffff87ffffff     unused hole      */
	{  0xffffffff88000000,  0xffffffffffdfffff, /* modules and      */
	   KDUMP_XLAT_VTOP,     0 },		    /*  fixmap/vsyscall */
	/* 0xffffffffffe00000 - 0xffffffffffffffff     guard hole       */
};

#define LAYOUT_NAME(a, b, c)	mm_layout_ ## a ## _ ## b ## _ ## c
#define DEF_LAYOUT(a, b, c) \
	{ KERNEL_VERSION(a, b, c), LAYOUT_NAME(a, b, c),	\
			ARRAY_SIZE(LAYOUT_NAME(a, b, c)) }

struct layout_def {
	unsigned ver;
	const struct region_def *regions;
	unsigned nregions;
} mm_layouts[] = {
	DEF_LAYOUT(2, 6, 0),
	DEF_LAYOUT(2, 6, 11),
	DEF_LAYOUT(2, 6, 27),
	DEF_LAYOUT(2, 6, 31),
};

struct elf_siginfo
{
	int32_t si_signo;	/* signal number */
	int32_t si_code;	/* extra code */
	int32_t si_errno;	/* errno */
} __attribute__((packed));

struct elf_prstatus
{
	struct elf_siginfo pr_info;	/* UNUSED in kernel cores */
	int16_t	pr_cursig;		/* UNUSED in kernel cores */
	char	_pad1[2];		/* alignment */
	uint64_t pr_sigpend;		/* UNUSED in kernel cores */
	uint64_t pr_sighold;		/* UNUSED in kernel cores */
	int32_t	pr_pid;			/* PID of crashing task */
	int32_t	pr_ppid;		/* UNUSED in kernel cores */
	int32_t	pr_pgrp;		/* UNUSED in kernel cores */
	int32_t	pr_sid;			/* UNUSED in kernel cores */
	struct timeval_64 pr_utime;	/* UNUSED in kernel cores */
	struct timeval_64 pr_stime;	/* UNUSED in kernel cores */
	struct timeval_64 pr_cutime;	/* UNUSED in kernel cores */
	struct timeval_64 pr_cstime;	/* UNUSED in kernel cores */
	uint64_t pr_reg[ELF_NGREG];	/* GP registers */
	/* optional UNUSED fields may follow */
} __attribute__((packed));

/* Internal CPU state, as seen by libkdumpfile */
struct cpu_state {
	int32_t pid;
	uint64_t reg[ELF_NGREG];
	struct cpu_state *next;
};

struct x86_64_data {
	struct cpu_state *cpu_state;
	uint64_t *pgt;
};

static kdump_status
add_noncanonical_region(kdump_ctx *ctx)
{
	return kdump_set_region(ctx, NONCANONICAL_START, NONCANONICAL_END,
				KDUMP_XLAT_INVALID, 0);
}

static kdump_status
x86_64_init(kdump_ctx *ctx)
{
	kdump_status ret;

	ctx->archdata = calloc(1, sizeof(struct x86_64_data));
	if (!ctx->archdata)
		return kdump_syserr;

	ret = add_noncanonical_region(ctx);
	if (ret != kdump_ok)
		return ret;

	ret = kdump_set_region(ctx, __START_KERNEL_map, VIRTADDR_MAX,
			       KDUMP_XLAT_KTEXT, __START_KERNEL_map);
	if (ret != kdump_ok)
		return ret;

	return kdump_ok;
}

static kdump_status
read_pgt(kdump_ctx *ctx)
{
	struct x86_64_data *archdata = ctx->archdata;
	kdump_vaddr_t pgtaddr;
	uint64_t *pgt;
	kdump_status ret;
	size_t sz;

	ret = kdump_vmcoreinfo_symbol(ctx, "init_level4_pgt", &pgtaddr);
	if (ret != kdump_ok)
		return ret;

	pgt = malloc(ctx->page_size);
	if (!pgt)
		return kdump_syserr;

	sz = ctx->page_size;
	ret = kdump_readp(ctx, pgtaddr, pgt, &sz, KDUMP_KVADDR);
	if (ret == kdump_ok)
		archdata->pgt = pgt;
	else
		free(pgt);

	return ret;
}

static struct layout_def*
layout_by_version(kdump_ctx *ctx)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(mm_layouts); ++i)
		if (mm_layouts[i].ver > ctx->version_code)
			break;
	if (!i)
		return NULL;
	return &mm_layouts[i-1];
}

static kdump_status
x86_64_vtop_init(kdump_ctx *ctx)
{
	struct layout_def *layout;
	unsigned i;
	kdump_status ret;

	read_pgt(ctx);

	layout = layout_by_version(ctx);
	if (!layout) {
		/* Keep the temporary mapping from x86_64_init */
		return kdump_ok;
	}

	kdump_flush_regions(ctx);
	ret = add_noncanonical_region(ctx);
	if (ret != kdump_ok)
		return ret;

	for (i = 0; i < layout->nregions; ++i) {
		const struct region_def *def = &layout->regions[i];
		ret = kdump_set_region(ctx, def->first, def->last,
				       def->xlat, def->phys_off);
		if (ret != kdump_ok)
			return ret;
	}

	return kdump_ok;
}

static kdump_status
process_x86_64_prstatus(kdump_ctx *ctx, void *data, size_t size)
{
	struct x86_64_data *archdata = ctx->archdata;
	struct elf_prstatus *status = data;
	struct cpu_state *cs;
	int i;

	if (size < sizeof(struct elf_prstatus))
		return kdump_dataerr;

	++ctx->num_cpus;

	cs = malloc(sizeof *cs);
	if (!cs)
		return kdump_syserr;

	cs->pid = dump32toh(ctx, status->pr_pid);
	for (i = 0; i < ELF_NGREG; ++i)
		cs->reg[i] = dump64toh(ctx, status->pr_reg[i]);

	cs->next = archdata->cpu_state;
	archdata->cpu_state = cs;

	return kdump_ok;
}

static kdump_status
x86_64_read_reg(kdump_ctx *ctx, unsigned cpu, unsigned index,
		kdump_reg_t *value)
{
	struct x86_64_data *archdata = ctx->archdata;
	struct cpu_state *cs;
	int i;

	if (index >= ELF_NGREG)
		return kdump_nodata;

	for (i = 0, cs = archdata->cpu_state; i < cpu && cs; ++i)
		cs = cs->next;
	if (!cs)
		return kdump_nodata;

	*value = cs->reg[index];
	return kdump_ok;
}

static kdump_status
x86_64_process_load(kdump_ctx *ctx, kdump_vaddr_t vaddr, kdump_paddr_t paddr)
{
	if (!(ctx->flags & DIF_PHYS_BASE) &&
	    vaddr >= __START_KERNEL_map &&
	    vaddr < __START_KERNEL_map + MAX_PHYSICAL_START)
		kdump_set_phys_base(ctx, paddr - (vaddr - __START_KERNEL_map));
	return kdump_ok;
}

static void
x86_64_cleanup(kdump_ctx *ctx)
{
	struct x86_64_data *archdata = ctx->archdata;
	struct cpu_state *cs, *oldcs;

	cs = archdata->cpu_state;
	while (cs) {
		oldcs = cs;
		cs = cs->next;
		free(oldcs);
	}

	if (archdata->pgt)
		free(archdata->pgt);

	free(archdata);
	ctx->archdata = NULL;
}

static kdump_status
x86_64_vtop(kdump_ctx *ctx, kdump_vaddr_t vaddr, kdump_paddr_t *paddr)
{
	return kdump_unsupported;
}

const struct arch_ops kdump_x86_64_ops = {
	.init = x86_64_init,
	.vtop_init = x86_64_vtop_init,
	.process_prstatus = process_x86_64_prstatus,
	.read_reg = x86_64_read_reg,
	.process_load = x86_64_process_load,
	.vtop = x86_64_vtop,
	.cleanup = x86_64_cleanup,
};
