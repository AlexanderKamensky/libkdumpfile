/** @internal @file src/addrxlat/sys.c
 * @brief Translation system routines.
 */
/* Copyright (C) 2016-2017 Petr Tesarik <ptesarik@suse.com>

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
#include <string.h>

#include "addrxlat-priv.h"

addrxlat_sys_t *
addrxlat_sys_new(void)
{
	addrxlat_sys_t *ret;

	ret = calloc(1, sizeof(addrxlat_sys_t));
	if (ret) {
		ret->refcnt = 1;
	}
	return ret;
}

unsigned long
addrxlat_sys_incref(addrxlat_sys_t *sys)
{
	return ++sys->refcnt;
}

/** Clean up all translation system maps and methods.
 * @param sys  Translation system.
 */
static void
sys_cleanup(addrxlat_sys_t *sys)
{
	unsigned i;

	for (i = 0; i < ADDRXLAT_SYS_MAP_NUM; ++i)
		if (sys->map[i]) {
			internal_map_clear(sys->map[i]);
			free(sys->map[i]);
			sys->map[i] = NULL;
		}


	for (i = 0; i < ADDRXLAT_SYS_METH_NUM; ++i)
		if (sys->meth[i]) {
			internal_meth_decref(sys->meth[i]);
			sys->meth[i] = NULL;
		}
}

unsigned long
addrxlat_sys_decref(addrxlat_sys_t *sys)
{
	unsigned long refcnt = --sys->refcnt;
	if (!refcnt) {
		sys_cleanup(sys);
		free(sys);
	}
	return refcnt;
}

addrxlat_status
addrxlat_sys_init(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx,
		  const addrxlat_osdesc_t *osdesc)
{
	struct sys_init_data ctl;
	sys_arch_fn *arch_fn;
	addrxlat_status status;

	clear_error(ctx);

	if (!strcmp(osdesc->arch, "x86_64"))
		arch_fn = sys_x86_64;
	else if ((osdesc->arch[0] == 'i' &&
		  (osdesc->arch[1] >= '3' && osdesc->arch[1] <= '6') &&
		  !strcmp(osdesc->arch + 2, "86")) ||
		 !strcmp(osdesc->arch, "ia32"))
		arch_fn = sys_ia32;
	else if (!strcmp(osdesc->arch, "s390x"))
		arch_fn = sys_s390x;
	else if (!strcmp(osdesc->arch, "ppc64"))
		arch_fn = sys_ppc64;
	else
		return set_error(ctx, addrxlat_notimpl,
				"Unsupported architecture");

	sys_cleanup(sys);

	ctl.sys = sys;
	ctl.ctx = ctx;
	ctl.osdesc = osdesc;

	status = parse_opts(&ctl.popt, ctx, osdesc->opts);
	if (status != addrxlat_ok)
		return status;

	return arch_fn(&ctl);
}

void
addrxlat_sys_set_map(addrxlat_sys_t *sys, addrxlat_sys_map_t idx,
		      addrxlat_map_t *map)
{
	if (sys->map[idx]) {
		internal_map_clear(sys->map[idx]);
		free(sys->map[idx]);
	}
	sys->map[idx] = map;
}

addrxlat_map_t *
addrxlat_sys_get_map(const addrxlat_sys_t *sys, addrxlat_sys_map_t idx)
{
	return sys->map[idx];
}

void
addrxlat_sys_set_meth(addrxlat_sys_t *sys,
		      addrxlat_sys_meth_t idx, addrxlat_meth_t *meth)
{
	if (sys->meth[idx])
		internal_meth_decref(sys->meth[idx]);
	sys->meth[idx] = meth;
	if (meth)
		internal_meth_incref(meth);
}

addrxlat_meth_t *
addrxlat_sys_get_meth(const addrxlat_sys_t *sys, addrxlat_sys_meth_t idx)
{
	if (sys->meth[idx])
		internal_meth_incref(sys->meth[idx]);
	return sys->meth[idx];
}

/** Allocate a translation method if needed.
 * @param ctl  Initialization data.
 * @parma idx  Method index
 * @returns    Error status.
 */
addrxlat_status
sys_ensure_meth(struct sys_init_data *ctl, addrxlat_sys_meth_t idx)
{
	if (ctl->sys->meth[idx])
		return addrxlat_ok;

	if ( (ctl->sys->meth[idx] = internal_meth_new()) )
		return addrxlat_ok;

	return set_error(ctl->ctx, addrxlat_nomem,
			 "Cannot allocate translation method %u",
			 (unsigned) idx);
}

/** Action function for @ref SYS_ACT_DIRECT.
 * @param ctl     Initialization data.
 * @param meth    Current directmap translation method.
 * @param region  Directmap region definition.
 *
 * This action sets up the direct mapping as a linear mapping that
 * maps the current region to kernel physical addresses starting at 0.
 */
static addrxlat_status
act_direct(struct sys_init_data *ctl,
	   addrxlat_meth_t *meth, const struct sys_region *region)
{
	struct sys_region layout[2] = {
		{ 0, region->last - region->first,
		  ADDRXLAT_SYS_METH_RDIRECT },
		SYS_REGION_END
	};
	addrxlat_def_t def;
	addrxlat_status status;

	def.kind = ADDRXLAT_LINEAR;
	def.target_as = ADDRXLAT_KPHYSADDR;
	def.param.linear.off = -region->first;
	internal_meth_set_def(meth, &def);

	status = sys_ensure_meth(ctl, ADDRXLAT_SYS_METH_RDIRECT);
	if (status != addrxlat_ok)
		return status;

	def.target_as = ADDRXLAT_KVADDR;
	def.param.linear.off = region->first;
	internal_meth_set_def(ctl->sys->meth[ADDRXLAT_SYS_METH_RDIRECT], &def);

	return sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KPHYS_DIRECT, layout);
}

/** Action function for @ref SYS_ACT_IDENT_KPHYS.
 * @param meth  Current translation method.
 *
 * If the current method is @c ADDRXLAT_NONE, this action sets it up
 * as identity mapping to kernel physical addresses.
 * If the current method is not @c ADDRXLAT_NONE, nothing is done.
 */
static void
act_ident_kphys(addrxlat_meth_t *meth)
{
	addrxlat_def_t def;
	def.kind = ADDRXLAT_LINEAR;
	def.target_as = ADDRXLAT_KPHYSADDR;
	def.param.linear.off = 0;
	internal_meth_set_def(meth, &def);
}

/** Action function for @ref SYS_ACT_IDENT_MACHPHYS.
 * @param meth  Current translation method.
 *
 * If the current method is @c ADDRXLAT_NONE, this action sets it up
 * as identity mapping to machine physical addresses.
 * If the current method is not @c ADDRXLAT_NONE, nothing is done.
 */
static void
act_ident_machphys(addrxlat_meth_t *meth)
{
	addrxlat_def_t def;
	def.kind = ADDRXLAT_LINEAR;
	def.target_as = ADDRXLAT_MACHPHYSADDR;
	def.param.linear.off = 0;
	internal_meth_set_def(meth, &def);
}

/** Set memory map layout.
 * @param ctl     Initialization data.
 * @param idx     Map index.
 * @param layout  Layout definition table.
 * @returns       Error status.
 */
addrxlat_status
sys_set_layout(struct sys_init_data *ctl, addrxlat_sys_map_t idx,
	       const struct sys_region layout[])
{
	const struct sys_region *region;
	addrxlat_map_t *newmap;

	for (region = layout; region->meth != ADDRXLAT_SYS_METH_NUM;
	     ++region) {
		addrxlat_range_t range;
		addrxlat_status status;

		status = sys_ensure_meth(ctl, region->meth);
		if (status != addrxlat_ok)
			return status;

		range.endoff = region->last - region->first;
		range.meth = ctl->sys->meth[region->meth];

		switch (region->act) {
		case SYS_ACT_DIRECT:
			status = act_direct(ctl, range.meth, region);
			if (status != addrxlat_ok)
				return status;
			break;

		case SYS_ACT_IDENT_KPHYS:
			act_ident_kphys(range.meth);
			break;

		case SYS_ACT_IDENT_MACHPHYS:
			act_ident_machphys(range.meth);
			break;

		default:
			break;
		}

		newmap = internal_map_set(ctl->sys->map[idx],
					  region->first, &range);
		if (!newmap)
			return set_error(ctl->ctx, addrxlat_nomem,
					 "Cannot set up mapping for"
					 " 0x%"ADDRXLAT_PRIxADDR
					 "-0x%"ADDRXLAT_PRIxADDR,
					 region->first,
					 region->last);
		ctl->sys->map[idx] = newmap;
	}

	return addrxlat_ok;
}

/** Set default (identity) physical mappings.
 * @param ctl     Initialization data.
 * @param maxaddr Maximum physical address.
 * @returns       Error status.
 */
addrxlat_status
sys_set_physmaps(struct sys_init_data *ctl, addrxlat_addr_t maxaddr)
{
	struct sys_region layout[2];
	addrxlat_status status;

	layout[1].meth = ADDRXLAT_SYS_METH_NUM;
	layout[0].first = 0;
	layout[0].last = maxaddr;

	layout[0].meth = ADDRXLAT_SYS_METH_MACHPHYS_KPHYS;
	layout[0].act = SYS_ACT_IDENT_KPHYS;
	status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_MACHPHYS_KPHYS, layout);
	if (status != addrxlat_ok)
		return status;

	layout[0].meth = ADDRXLAT_SYS_METH_KPHYS_MACHPHYS;
	layout[0].act = SYS_ACT_IDENT_MACHPHYS;
	return sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KPHYS_MACHPHYS, layout);
}

#define DSTMAP(dst, mapidx) \
	[(ADDRXLAT_ ## dst)] = (ADDRXLAT_SYS_MAP_ ## mapidx) + 1

static const addrxlat_sys_map_t
map_trans[][3] = {
	[ADDRXLAT_KPHYSADDR] = {
		DSTMAP(MACHPHYSADDR, KPHYS_MACHPHYS),
		DSTMAP(KVADDR, KPHYS_DIRECT),
	},
	[ADDRXLAT_MACHPHYSADDR] = {
		DSTMAP(KPHYSADDR, MACHPHYS_KPHYS),
		DSTMAP(KVADDR, MACHPHYS_KPHYS),
	},
	[ADDRXLAT_KVADDR] = {
		DSTMAP(KPHYSADDR, KV_PHYS),
		DSTMAP(MACHPHYSADDR, KV_PHYS),
	},
};

DEFINE_ALIAS(by_sys);

addrxlat_status
addrxlat_by_sys(addrxlat_ctx_t *ctx, addrxlat_fulladdr_t *paddr,
		addrxlat_addrspace_t goal, const addrxlat_sys_t *sys)
{
	addrxlat_sys_map_t mapidx;
	addrxlat_map_t *map;
	addrxlat_step_t step;
	addrxlat_status status;

	clear_error(ctx);

	if (paddr->as == goal)
		return addrxlat_ok;

	if (paddr->as >= ARRAY_SIZE(map_trans) ||
	    goal >= ARRAY_SIZE(map_trans[0]))
		return set_error(ctx, addrxlat_notimpl,
				 "Unrecognized address space");

	mapidx = map_trans[paddr->as][goal] - 1;
	if (mapidx < 0 || !(map = sys->map[mapidx]))
		return set_error(ctx, addrxlat_nometh, "No way to translate");

	step.ctx = ctx;
	step.sys = sys;
	status = internal_launch_map(&step, paddr->addr, map);
	if (status != addrxlat_ok)
		return status;

	status = internal_walk(&step);
	if (status == addrxlat_ok) {
		*paddr = step.base;
		if (step.base.as != goal)
			return addrxlat_by_sys(ctx, paddr, goal, sys);
	}

	return status;
}
