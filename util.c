/* Utility functions.
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

/* utsname strings are 65 characters long.
 * Final NUL may be missing (i.e. corrupted dump data)
 */
void
kdump_copy_uts_string(char *dest, const char *src)
{
	if (!*dest) {
		memcpy(dest, src, 65);
		dest[65] = 0;
	}
}

int
kdump_uts_looks_sane(struct new_utsname *uts)
{
	return uts->sysname[0] && uts->nodename[0] && uts->release[0] &&
		uts->version[0] && uts->machine[0];
}
