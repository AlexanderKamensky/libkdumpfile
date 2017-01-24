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

static void
free_map(addrxlat_map_t *map)
{
	if (map) {
		internal_map_clear(map);
		free(map);
	}
}

unsigned long
addrxlat_sys_decref(addrxlat_sys_t *sys)
{
	unsigned long refcnt = --sys->refcnt;
	if (!refcnt) {
		unsigned i;

		free_map(sys->map);
		for (i = 0; i < ADDRXLAT_SYS_METH_NUM; ++i)
			if (sys->meth[i])
				internal_meth_decref(sys->meth[i]);
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

	ctl.sys = sys;
	ctl.ctx = ctx;
	ctl.osdesc = osdesc;

	status = parse_opts(&ctl.popt, ctx, osdesc->opts);
	if (status != addrxlat_ok)
		return status;

	return arch_fn(&ctl);
}

void
addrxlat_sys_set_map(addrxlat_sys_t *sys, addrxlat_map_t *map)
{
	free_map(sys->map);
	sys->map = map;
}

const addrxlat_map_t *
addrxlat_sys_get_map(const addrxlat_sys_t *sys)
{
	return sys->map;
}

void
addrxlat_sys_set_xlat(addrxlat_sys_t *sys,
		      addrxlat_sys_meth_t idx, addrxlat_meth_t *meth)
{
	if (sys->meth[idx])
		internal_meth_decref(sys->meth[idx]);
	sys->meth[idx] = meth;
	if (meth)
		internal_meth_incref(meth);
}

addrxlat_meth_t *
addrxlat_sys_get_xlat(addrxlat_sys_t *sys, addrxlat_sys_meth_t idx)
{
	if (sys->meth[idx])
		internal_meth_incref(sys->meth[idx]);
	return sys->meth[idx];
}

/** Action function for @ref SYS_ACT_DIRECT.
 * @parma ctl  Initialization data.
 */
static void
direct_hook(struct sys_init_data *ctl, const struct sys_region *region)
{
	addrxlat_def_t def;
	def.kind = ADDRXLAT_LINEAR;
	def.param.linear.off = region->first;
	internal_meth_set_def(ctl->sys->meth[region->meth], &def);
}

/** Set memory map layout.
 * @parma ctl     Initialization data.
 * @param layout  Layout definition table.
 * @returns       Error status.
 */
addrxlat_status
sys_set_layout(struct sys_init_data *ctl,
	       const struct sys_region layout[])
{
	static sys_action_fn *const actions[] = {
		[SYS_ACT_DIRECT] = direct_hook,
	};

	const struct sys_region *region;
	addrxlat_map_t *newmap;

	for (region = layout; region->meth != ADDRXLAT_SYS_METH_NUM;
	     ++region) {
		addrxlat_range_t range;

		if (!ctl->sys->meth[region->meth])
			ctl->sys->meth[region->meth] = internal_meth_new();
		if (!ctl->sys->meth[region->meth])
			return set_error(ctl->ctx, addrxlat_nomem,
					 "Cannot allocate translation"
					 " method %u",
					 (unsigned) region->meth);

		if (region->act != SYS_ACT_NONE)
			actions[region->act](ctl, region);

		range.endoff = region->last - region->first;
		range.meth = ctl->sys->meth[region->meth];
		newmap = internal_map_set(ctl->sys->map,
					  region->first, &range);
		if (!newmap)
			return set_error(ctl->ctx, addrxlat_nomem,
					 "Cannot set up mapping for"
					 " 0x%"ADDRXLAT_PRIxADDR
					 "-0x%"ADDRXLAT_PRIxADDR,
					 region->first,
					 region->last);
		ctl->sys->map = newmap;
	}

	return addrxlat_ok;
}
