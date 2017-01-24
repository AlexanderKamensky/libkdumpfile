/** @internal @file src/addrxlat/s390x.c
 * @brief Routines specific to IBM z/Architecture.
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

#include "addrxlat-priv.h"

/* Use IBM's official bit numbering to match spec... */
#define PTE_MASK(bits)		(((uint64_t)1<<bits) - 1)
#define PTE_VAL(x, shift, bits)	(((x) >> (64-(shift)-(bits))) & PTE_MASK(bits))

#define PTE_FC(x)	PTE_VAL(x, 53, 1)
#define PTE_I(x)	PTE_VAL(x, 58, 1)
#define PTE_TF(x)	PTE_VAL(x, 56, 2)
#define PTE_TT(x)	PTE_VAL(x, 60, 2)
#define PTE_TL(x)	PTE_VAL(x, 62, 2)

/* Page-Table Origin has 2K granularity in hardware */
#define PTO_MASK	(~(((uint64_t)1 << 11) - 1))

/* Maximum pointers in the root page table */
#define ROOT_PGT_LEN	2048

/** IBM z/Architecture page table step function.
 * @param state  Page table walk state.
 * @returns      Error status.
 */
addrxlat_status
pgt_s390x(addrxlat_walk_t *state)
{
	static const char pgt_full_name[][16] = {
		"Page",
		"Page table",
		"Segment table",
		"Region 3 table",
		"Region 2 table",
		"Region 1 table"
	};
	static const char pte_name[][4] = {
		"pte",
		"pmd",
		"pud",
		"pgd",
		"rg1",		/* Invented; does not exist in the wild. */
	};
	const addrxlat_paging_form_t *pf = &state->meth->def.param.pgt.pf;
	const struct pgt_extra_def *pgt = &state->meth->extra.pgt;

	if (PTE_I(state->raw_pte))
		return set_error(state->ctx, addrxlat_notpresent,
				 "%s not present: %s[%u] = 0x%" ADDRXLAT_PRIxPTE,
				 pgt_full_name[state->level - 1],
				 pte_name[state->level - 1],
				 (unsigned) state->idx[state->level],
				 state->raw_pte);

	if (state->level >= 2 && PTE_TT(state->raw_pte) != state->level - 2)
		return set_error(state->ctx, addrxlat_invalid,
				 "Table type field %u in %s",
				 (unsigned) PTE_TT(state->raw_pte),
				 pgt_full_name[state->level]);

	state->base.as = ADDRXLAT_MACHPHYSADDR;
	state->base.addr = state->raw_pte;

	if (state->level >= 2 && state->level <= 3 &&
	    PTE_FC(state->raw_pte)) {
		state->base.addr &= pgt->pgt_mask[state->level - 1];
		return pgt_huge_page(state);
	}

	if (state->level >= 3) {
		unsigned pgidx = state->idx[state->level - 1] >>
			(pf->bits[state->level - 1] - pf->bits[0]);
		if (pgidx < PTE_TF(state->raw_pte) ||
		    pgidx > PTE_TL(state->raw_pte))
			return set_error(state->ctx, addrxlat_notpresent,
					 "%s index %u not within %u and %u",
					 pgt_full_name[state->level-1],
					 (unsigned) state->idx[state->level-1],
					 (unsigned) PTE_TF(state->raw_pte),
					 (unsigned) PTE_TL(state->raw_pte));
	}

	state->base.addr &= (state->level == 2 ? PTO_MASK : pgt->pgt_mask[0]);
	return addrxlat_continue;
}

/** Determine OS-specific page table root.
 * @param ctl        Initialization data.
 * @param[out] root  Page table root address (set on successful return).
 * @returns          Error status.
 */
static addrxlat_status
get_pgtroot(struct sys_init_data *ctl, addrxlat_fulladdr_t *root)
{
	addrxlat_status status;

	switch (ctl->osdesc->type) {
	case addrxlat_os_linux:
		status = get_symval(ctl->ctx, "swapper_pg_dir", &root->addr);
		if (status == addrxlat_ok) {
			root->as = ADDRXLAT_KPHYSADDR;
			return addrxlat_ok;
		}
		break;

	default:
		break;
	}

	return set_error(ctl->ctx, addrxlat_notimpl,
			 "Cannot determine page table root address");
}

/* Try to guess the page table type from its content.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
determine_pgttype(struct sys_init_data *ctl)
{
	addrxlat_meth_t *pgtmeth;
	addrxlat_def_t def;
	addrxlat_fulladdr_t ptr;
	uint64_t entry;
	unsigned i;
	addrxlat_status status;

	pgtmeth = ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	def_choose_pgtroot(&def, pgtmeth);
	if (def.param.pgt.root.as == ADDRXLAT_NOADDR) {
		status = get_pgtroot(ctl, &pgtmeth->def.param.pgt.root);
		if (status != addrxlat_ok)
			return status;
	}

	ptr = pgtmeth->def.param.pgt.root;
	for (i = 0; i < ROOT_PGT_LEN; ++i) {
		status = read64(ctl->ctx, &ptr, &entry, "page table");
		if (status != addrxlat_ok)
			return status;
		if (!PTE_I(entry)) {
			addrxlat_paging_form_t pf = {
				.pte_format = addrxlat_pte_s390x,
				.bits = { 12, 8, 11, 11, 11, 11 }
			};

			def.kind = ADDRXLAT_PGT;
			def.param.pgt.pf = pf;
			def.param.pgt.pf.levels = PTE_TT(entry) + 3;
			return internal_meth_set_def(pgtmeth, &def);
		}
		ptr.addr += sizeof(uint64_t);
	}

	return set_error(ctl->ctx, addrxlat_notpresent,
			 "Empty top-level page table");
}

/** Initialize a translation map for a s390x OS.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
addrxlat_status
sys_s390x(struct sys_init_data *ctl)
{
	addrxlat_map_t *newmap;
	addrxlat_range_t range;
	addrxlat_status status;

	if (!ctl->sys->meth[ADDRXLAT_SYS_METH_PGT])
		ctl->sys->meth[ADDRXLAT_SYS_METH_PGT] = internal_meth_new();
	if (!ctl->sys->meth[ADDRXLAT_SYS_METH_PGT])
		return addrxlat_nomem;

	status = determine_pgttype(ctl);
	if (status != addrxlat_ok)
		return status;

	range.meth = ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	range.endoff = paging_max_index(&range.meth->def.param.pgt.pf);
	newmap = internal_map_set(ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS],
				  0, &range);
	if (!newmap)
		return set_error(ctl->ctx, addrxlat_nomem,
				 "Cannot set up default mapping");
	ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS] = newmap;

	return addrxlat_ok;
}
