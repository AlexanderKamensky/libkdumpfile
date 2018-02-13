/** @internal @file src/addrxlat/ctx.c
 * @brief Address translation context routines.
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
#include <string.h>
#include <stdarg.h>

#include "addrxlat-priv.h"

/** Maximum length of the static error message. */
#define ERRBUF	64

addrxlat_ctx_t *
addrxlat_ctx_new(void)
{
	addrxlat_ctx_t *ctx = calloc(1, sizeof(addrxlat_ctx_t) + ERRBUF);
	if (ctx) {
		ctx->refcnt = 1;
		err_init(&ctx->err, ERRBUF);
	}
	return ctx;
}

unsigned long
addrxlat_ctx_incref(addrxlat_ctx_t *ctx)
{
	return ++ctx->refcnt;
}

unsigned long
addrxlat_ctx_decref(addrxlat_ctx_t *ctx)
{
	unsigned long refcnt = --ctx->refcnt;
	if (!refcnt) {
		err_cleanup(&ctx->err);
		free(ctx);
	}
	return refcnt;
}

void addrxlat_ctx_clear_err(addrxlat_ctx_t *ctx)
{
	clear_error(ctx);
}

const char *
addrxlat_ctx_get_err(const addrxlat_ctx_t *ctx)
{
	return err_str(&ctx->err);
}

kdump_errmsg_t *
addrxlat_ctx_get_errmsg(addrxlat_ctx_t *ctx)
{
	return &ctx->err;
}

void
addrxlat_ctx_set_cb(addrxlat_ctx_t *ctx, const addrxlat_cb_t *cb)
{
	addrxlat_cb_hook_fn *hook = ctx->cb.cb_hook;
	void *data = ctx->cb.data;
	ctx->cb = ctx->orig_cb = *cb;
	if (hook)
		hook(data, &ctx->cb);
}

const addrxlat_cb_t *
addrxlat_ctx_get_cb(const addrxlat_ctx_t *ctx)
{
	return &ctx->orig_cb;
}

addrxlat_cb_t *
addrxlat_ctx_get_ecb(addrxlat_ctx_t *ctx)
{
	return &ctx->cb;
}

/** Get the (string) name of an address space.
 * @param as  Address space.
 * @returns   The (human-readable) name of the address space.
 */
static const char *
addrspace_name(addrxlat_addrspace_t as)
{
	switch (as) {
	case ADDRXLAT_KPHYSADDR:	return "KPHYSADDR";
	case ADDRXLAT_MACHPHYSADDR:	return "MACHPHYSADDR";
	case ADDRXLAT_KVADDR:		return "KVADDR";
	case ADDRXLAT_NOADDR:		return "NOADDR";
	default:			return "invalid addrspace_t";
	}
}

/** Common format string for missing read callback. */
static const char read_nocb_fmt[] =
	"No read callback for %d-bit %s";

/** Common format string for read callback failures. */
static const char read_err_fmt[] =
	"Cannot read %d-bit %s at %s:0x%"ADDRXLAT_PRIxADDR;

struct read_param {
	addrxlat_ctx_t *ctx;
	void *val;
};

static addrxlat_status
read32_op(void *data, const addrxlat_fulladdr_t *addr)
{
	const struct read_param *param = data;
	return param->ctx->cb.read32(param->ctx->cb.data, addr, param->val);
}

/** Read a 32-bit value, making an error message if needed.
 * @param     step  Current step state.
 * @param[in] addr  Full address of the data.
 * @param[out] val  32-bit data (on successful return).
 * @param     what  Descriptive object name.
 * @returns         Error status.
 */
addrxlat_status
read32(addrxlat_step_t *step, const addrxlat_fulladdr_t *addr, uint32_t *val,
       const char *what)
{
	addrxlat_ctx_t *ctx = step->ctx;
	addrxlat_op_ctl_t ctl;
	struct read_param param;
	addrxlat_status status;

	if (!ctx->cb.read32)
		return set_error(ctx, ADDRXLAT_ERR_NODATA, read_nocb_fmt, 32,
				 addrspace_name(addr->as));

	param.ctx = ctx;
	param.val = val;
	ctl.ctx = ctx;
	ctl.sys = step->sys;
	ctl.op = read32_op;
	ctl.data = &param;
	ctl.caps = step->ctx->cb.read_caps;
	status = internal_op(&ctl, addr);
	if (status != ADDRXLAT_OK)
		return set_error(ctx, status, read_err_fmt, 32, what,
				 addrspace_name(addr->as), addr->addr);

	return ADDRXLAT_OK;
}

static addrxlat_status
read64_op(void *data, const addrxlat_fulladdr_t *addr)
{
	const struct read_param *param = data;
	return param->ctx->cb.read64(param->ctx->cb.data, addr, param->val);
}

/** Read a 64-bit value, making an error message if needed.
 * @param     step  Current step state.
 * @param[in] addr  Full address of the data.
 * @param[out] val  64-bit data (on successful return).
 * @param     what  Descriptive object name.
 * @returns         Error status.
 */
addrxlat_status
read64(addrxlat_step_t *step, const addrxlat_fulladdr_t *addr, uint64_t *val,
       const char *what)
{
	addrxlat_ctx_t *ctx = step->ctx;
	addrxlat_op_ctl_t ctl;
	struct read_param param;
	addrxlat_status status;

	if (!ctx->cb.read64)
		return set_error(ctx, ADDRXLAT_ERR_NODATA, read_nocb_fmt, 64,
				 addrspace_name(addr->as));

	param.ctx = ctx;
	param.val = val;
	ctl.ctx = ctx;
	ctl.sys = step->sys;
	ctl.op = read64_op;
	ctl.data = &param;
	ctl.caps = step->ctx->cb.read_caps;
	status = internal_op(&ctl, addr);
	if (status != ADDRXLAT_OK)
		return set_error(ctx, status, read_err_fmt, 64, what,
				 addrspace_name(addr->as), addr->addr);

	return ADDRXLAT_OK;
}

/** Get register value.
 * @param      ctx   Address translation context.
 * @param      name  Register name.
 * @param[out] val   Register value, returned on sucess.
 * @returns          Error status.
 *
 * The register value is obtained using a user-supplied callback.
 */
addrxlat_status
get_reg(addrxlat_ctx_t *ctx, const char *name, addrxlat_addr_t *val)
{
	struct {
		addrxlat_sym_t sym;
		const char *name;
	} info;
	addrxlat_status status;

	if (!ctx->cb.sym)
		return set_error(ctx, ADDRXLAT_ERR_NODATA,
				 "No symbolic information callback");

	info.sym.type = ADDRXLAT_SYM_REG;
	info.name = name;
	status = ctx->cb.sym(ctx->cb.data, (addrxlat_sym_t*)&info);
	if (status != ADDRXLAT_OK)
		return set_error(ctx, status,
				 "Cannot read register \"%s\"", info.name);

	*val = info.sym.val;
	return status;
}

/** Resolve a symbol value.
 * @param      ctx   Address translation context.
 * @param      name  Symbol name.
 * @param[out] val   Symbol value, returned on sucess.
 * @returns          Error status.
 *
 * The symbol is resolved using a user-supplied callback.
 */
addrxlat_status
get_symval(addrxlat_ctx_t *ctx, const char *name, addrxlat_addr_t *val)
{
	struct {
		addrxlat_sym_t sym;
		const char *name;
	} info;
	addrxlat_status status;

	if (!ctx->cb.sym)
		return set_error(ctx, ADDRXLAT_ERR_NODATA,
				 "No symbolic information callback");

	info.sym.type = ADDRXLAT_SYM_VALUE;
	info.name = name;
	status = ctx->cb.sym(ctx->cb.data, (addrxlat_sym_t*)&info);
	if (status != ADDRXLAT_OK)
		return set_error(ctx, status,
				 "Cannot resolve \"%s\"", info.name);

	*val = info.sym.val;
	return status;
}

/** Get the size of a symbol or type.
 * @param      ctx   Address translation context.
 * @param      name  Symbol name or type name.
 * @param[out] sz    Size in bytes, returned on sucess.
 * @returns          Error status.
 *
 * The size is determined using a user-supplied callback.
 */
addrxlat_status
get_sizeof(addrxlat_ctx_t *ctx, const char *name, addrxlat_addr_t *sz)
{
	struct {
		addrxlat_sym_t sym;
		const char *name;
	} info;
	addrxlat_status status;

	if (!ctx->cb.sym)
		return set_error(ctx, ADDRXLAT_ERR_NODATA,
				 "No symbolic information callback");

	info.sym.type = ADDRXLAT_SYM_SIZEOF;
	info.name = name;
	status = ctx->cb.sym(ctx->cb.data, (addrxlat_sym_t*)&info);
	if (status != ADDRXLAT_OK)
		return set_error(ctx, status, "Cannot get sizeof(%s)",
				 info.name);

	*sz = info.sym.val;
	return status;
}

/** Get the relative offset of a member inside a type.
 * @param      ctx   Address translation context.
 * @param      type  Container type name.
 * @param      memb  Member name.
 * @param[out] val   Symbol value, returned on sucess.
 * @returns          Error status.
 *
 * The symbol is resolved using a user-supplied callback.
 */
addrxlat_status
get_offsetof(addrxlat_ctx_t *ctx, const char *type, const char *memb,
	     addrxlat_addr_t *off)
{
	struct {
		addrxlat_sym_t sym;
		const char *type;
		const char *memb;
	} info;
	addrxlat_status status;

	if (!ctx->cb.sym)
		return set_error(ctx, ADDRXLAT_ERR_NODATA,
				 "No symbolic information callback");

	info.sym.type = ADDRXLAT_SYM_OFFSETOF;
	info.type = type;
	info.memb = memb;
	status = ctx->cb.sym(ctx->cb.data, (addrxlat_sym_t*)&info);
	if (status != ADDRXLAT_OK)
		return set_error(ctx, status, "Cannot get offsetof(%s, %s)",
				 info.type, info.memb);

	*off = info.sym.val;
	return status;
}

/** Get the first successfuly resolved value from a specifier list.
 * @param      ctx   Address translation context.
 * @param      spec  Vector of specifiers.
 * @param[out] addr  Symbol full address, returned on sucess.
 * @returns          Error status.
 *
 * The symbol is resolved using a user-supplied callback.
 */
addrxlat_status
get_first_sym(addrxlat_ctx_t *ctx, const struct sym_spec *spec,
	      addrxlat_fulladdr_t *addr)
{
	struct {
		addrxlat_sym_t sym;
		const char *name;
	} info;
	addrxlat_status status = ADDRXLAT_ERR_NODATA;

	if (!ctx->cb.sym)
		return set_error(ctx, status,
				 "No symbolic information callback");

	while (spec->type != ADDRXLAT_SYM_NONE) {
		info.sym.type = spec->type;
		info.name = spec->name;
		status = ctx->cb.sym(ctx->cb.data, (addrxlat_sym_t*)&info);
		if (status == ADDRXLAT_OK) {
			addr->addr = info.sym.val;
			addr->as = spec->as;
			return status;
		} else if (status != ADDRXLAT_ERR_NODATA)
			break;

		clear_error(ctx);
		++spec;
	}

	return set_error(ctx, status,
			 "Cannot resolve \"%s\"", info.name);
}

DEFINE_ALIAS(ctx_err);

addrxlat_status
addrxlat_ctx_err(addrxlat_ctx_t *ctx, addrxlat_status status,
		 const char *msgfmt, ...)
{
	if (status != ADDRXLAT_OK) {
		va_list ap;

		va_start(ap, msgfmt);
		err_vadd(&ctx->err, msgfmt, ap);
		va_end(ap);
	}

	return status;
}

const char *
addrxlat_strerror(addrxlat_status status)
{
	switch (status) {
	case ADDRXLAT_OK:		return "Success";
	case ADDRXLAT_ERR_NOTIMPL:	return "Unimplemented feature";
	case ADDRXLAT_ERR_NOTPRESENT:	return "Page not present";
	case ADDRXLAT_ERR_INVALID:	return "Invalid address";
	case ADDRXLAT_ERR_NOMEM:	return "Memory allocation failure";
	case ADDRXLAT_ERR_NODATA:	return "Data not available";
	case ADDRXLAT_ERR_NOMETH:	return "No translation method";
	default:			return "Unknown error";
	}
}
