/* Functions that provide access to kdump_ctx contents.
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

#include <string.h>

#include "kdumpfile-priv.h"

const char *
kdump_format(kdump_ctx *ctx)
{
	return ctx->format;
}

int
kdump_is_xen(kdump_ctx *ctx)
{
	return !!(ctx->flags & DIF_XEN);
}

size_t
kdump_pagesize(kdump_ctx *ctx)
{
	return ctx->page_size;
}

size_t
kdump_phys_base(kdump_ctx *ctx)
{
	return ctx->phys_base;
}

const char *
kdump_sysname(kdump_ctx *ctx)
{
	return ctx->utsname.sysname;
}

const char *
kdump_nodename(kdump_ctx *ctx)
{
	return ctx->utsname.nodename;
}

const char *
kdump_release(kdump_ctx *ctx)
{
	return ctx->utsname.release;
}

const char *
kdump_version(kdump_ctx *ctx)
{
	return ctx->utsname.version;
}

const char *
kdump_machine(kdump_ctx *ctx)
{
	return ctx->utsname.machine;
}

const char *
kdump_domainname(kdump_ctx *ctx)
{
	return ctx->utsname.domainname;
}

unsigned
kdump_num_cpus(kdump_ctx *ctx)
{
	return ctx->num_cpus;
}

kdump_status
kdump_read_reg(kdump_ctx *ctx, unsigned cpu, unsigned index,
	       kdump_reg_t *value)
{
	if (!ctx->arch_ops || !ctx->arch_ops->read_reg)
		return kdump_nodata;

	return ctx->arch_ops->read_reg(ctx, cpu, index, value);
}

const char *
kdump_vmcoreinfo(kdump_ctx *ctx)
{
	return ctx->vmcoreinfo ? ctx->vmcoreinfo->raw : NULL;
}

const char *
kdump_vmcoreinfo_xen(kdump_ctx *ctx)
{
	return ctx->vmcoreinfo_xen ? ctx->vmcoreinfo_xen->raw : NULL;
}

static const char*
vmcoreinfo_row(struct vmcoreinfo *info, const char *key)
{
	unsigned i;
	if (!info)
		return NULL;
	for (i = 0; i < info->n; ++i)
		if (!strcmp(key, info->row[i].key))
			return info->row[i].val;
	return NULL;
}

const char *
kdump_vmcoreinfo_row(kdump_ctx *ctx, const char *key)
{
	return vmcoreinfo_row(ctx->vmcoreinfo, key);
}

const char *
kdump_vmcoreinfo_row_xen(kdump_ctx *ctx, const char *key)
{
	return vmcoreinfo_row(ctx->vmcoreinfo_xen, key);
}

void
kdump_xen_version(kdump_ctx *ctx, kdump_xen_version_t *version)
{
	memcpy(version, &ctx->xen_ver, sizeof(kdump_xen_version_t));
}
