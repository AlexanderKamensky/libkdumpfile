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
 * @param step  Current step state.
 * @returns     Error status.
 */
addrxlat_status
pgt_s390x(addrxlat_step_t *step)
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
	const addrxlat_paging_form_t *pf = &step->meth->desc.param.pgt.pf;
	const struct pgt_extra_def *pgt = &step->meth->extra.pgt;
	addrxlat_status status;

	status = read_pte(step);
	if (status != ADDRXLAT_OK)
		return status;

	if (PTE_I(step->raw_pte))
		return set_error(step->ctx, ADDRXLAT_NOTPRESENT,
				 "%s not present: %s[%u] = 0x%" ADDRXLAT_PRIxPTE,
				 pgt_full_name[step->remain - 1],
				 pte_name[step->remain - 1],
				 (unsigned) step->idx[step->remain],
				 step->raw_pte);

	if (step->remain >= 2 && PTE_TT(step->raw_pte) != step->remain - 2)
		return set_error(step->ctx, ADDRXLAT_INVALID,
				 "Table type field %u in %s",
				 (unsigned) PTE_TT(step->raw_pte),
				 pgt_full_name[step->remain]);

	step->base.addr = step->raw_pte;
	step->base.as = step->meth->desc.target_as;

	if (step->remain >= 2 && step->remain <= 3 &&
	    PTE_FC(step->raw_pte)) {
		step->base.addr &= pgt->pgt_mask[step->remain - 1];
		return pgt_huge_page(step);
	}

	if (step->remain >= 3) {
		unsigned pgidx = step->idx[step->remain - 1] >>
			(pf->fieldsz[step->remain - 1] - pf->fieldsz[0]);
		if (pgidx < PTE_TF(step->raw_pte) ||
		    pgidx > PTE_TL(step->raw_pte))
			return set_error(step->ctx, ADDRXLAT_NOTPRESENT,
					 "%s index %u not within %u and %u",
					 pgt_full_name[step->remain-1],
					 (unsigned) step->idx[step->remain-1],
					 (unsigned) PTE_TF(step->raw_pte),
					 (unsigned) PTE_TL(step->raw_pte));
	}

	step->base.addr &= (step->remain == 2 ? PTO_MASK : pgt->pgt_mask[0]);
	return ADDRXLAT_OK;
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
		if (status == ADDRXLAT_OK) {
			root->as = ADDRXLAT_KPHYSADDR;
			return ADDRXLAT_OK;
		}
		break;

	default:
		break;
	}

	clear_error(ctl->ctx);
	return set_error(ctl->ctx, ADDRXLAT_NOTIMPL,
			 "Cannot determine page table root address");
}

/* Try to guess the page table type from its content.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
determine_pgttype(struct sys_init_data *ctl)
{
	addrxlat_step_t step =	/* step state surrogate */
		{ .ctx = ctl->ctx, .sys = ctl->sys };
	addrxlat_desc_t desc;
	addrxlat_fulladdr_t ptr;
	uint64_t entry;
	unsigned i;
	addrxlat_status status;

	if (ctl->popt.val[OPT_rootpgt].set)
		desc.param.pgt.root = ctl->popt.val[OPT_rootpgt].fulladdr;
	else
		desc.param.pgt.root.as = ADDRXLAT_NOADDR;

	if (desc.param.pgt.root.as == ADDRXLAT_NOADDR) {
		status = get_pgtroot(ctl, &desc.param.pgt.root);
		if (status != ADDRXLAT_OK)
			return status;
	}

	ptr = desc.param.pgt.root;
	for (i = 0; i < ROOT_PGT_LEN; ++i) {
		status = read64(&step, &ptr, &entry, "page table");
		if (status != ADDRXLAT_OK)
			return status;
		if (!PTE_I(entry)) {
			static const addrxlat_paging_form_t pf = {
				.pte_format = ADDRXLAT_PTE_S390X,
				.fieldsz = { 12, 8, 11, 11, 11, 11 }
			};
			addrxlat_meth_t *pgtmeth =
				ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];

			desc.kind = ADDRXLAT_PGT;
			desc.target_as = ADDRXLAT_MACHPHYSADDR;
			desc.param.pgt.pf = pf;
			desc.param.pgt.pf.nfields = PTE_TT(entry) + 3;
			return internal_meth_set_desc(pgtmeth, &desc);
		}
		ptr.addr += sizeof(uint64_t);
	}

	return set_error(ctl->ctx, ADDRXLAT_NOTPRESENT,
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

	status = sys_set_physmaps(ctl, ~(uint64_t)0);
	if (status != ADDRXLAT_OK)
		return status;

	status = sys_ensure_meth(ctl, ADDRXLAT_SYS_METH_PGT);
	if (status != ADDRXLAT_OK)
		return status;

	status = determine_pgttype(ctl);
	if (status != ADDRXLAT_OK)
		return status;

	range.meth = ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	range.endoff = paging_max_index(&range.meth->desc.param.pgt.pf);
	newmap = internal_map_new();
	if (!newmap)
		return set_error(ctl->ctx, ADDRXLAT_NOMEM,
				 "Cannot set up hardware mapping");
	ctl->sys->map[ADDRXLAT_SYS_MAP_HW] = newmap;
	status = internal_map_set(newmap, 0, &range);
	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status,
				 "Cannot set up hardware mapping");

	newmap = internal_map_dup(ctl->sys->map[ADDRXLAT_SYS_MAP_HW]);
	if (!newmap)
		return set_error(ctl->ctx, ADDRXLAT_NOMEM,
				 "Cannot duplicate hardware mapping");
	ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS] = newmap;

	return ADDRXLAT_OK;
}
