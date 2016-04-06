/* Check VMCOREINFO post-set hooks.
   Copyright (C) 2016 Petr Tesarik <ptesarik@suse.com>

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

#include <stdio.h>
#include <string.h>
#include <kdumpfile.h>

#include "testutil.h"

#define xstr(s)	#s
#define str(s)	xstr(s)

#define OSRELEASE	"3.4.5-test"
#define ATTR_OSRELEASE	"linux.uts.release"

#define PAGESIZE	2048	/* unlikely to match host page size. */
#define ATTR_PAGESIZE	"arch.page_size"

#define SYM_NAME	"test_symbol"
#define SYM_VALUE	0x123456

#define ATTR_LINES	"linux.vmcoreinfo.lines"

static const char vmcore[] =
	"OSRELEASE=" OSRELEASE			"\n"
	"PAGESIZE=" str(PAGESIZE)		"\n"
	"SYMBOL(" SYM_NAME ")=" str(SYM_VALUE)	"\n"
	"";

static int
check_string(kdump_ctx *ctx, const char *attrpath, const char *expect)
{
	kdump_attr_t attr;
	kdump_status status;

	status = kdump_get_attr(ctx, attrpath, &attr);
	if (status != kdump_ok) {
		fprintf(stderr, "%s: Cannot get value: %s\n",
			attrpath, kdump_err_str(ctx));
		return TEST_ERR;
	}
	if (attr.type != kdump_string) {
		fprintf(stderr, "%s: Wrong attribute type: %d\n",
			attrpath, (int) attr.type);
		return TEST_FAIL;
	}
	if (strcmp(attr.val.string, expect)) {
		fprintf(stderr, "%s: Invalid attribute value: '%s' != '%s'\n",
			attrpath, attr.val.string, expect);
		return TEST_FAIL;
	}

	printf("%s: %s\n", attrpath, attr.val.string);
	return TEST_OK;
}

static int
check(kdump_ctx *ctx)
{
	kdump_attr_t attr;
	kdump_addr_t symval;
	kdump_status status;
	int rc, tmprc;

	attr.type = kdump_string;
	attr.val.string = vmcore;
	status = kdump_set_attr(ctx, "linux.vmcoreinfo.raw", &attr);
	if (status != kdump_ok) {
		fprintf(stderr, "Cannot set vmcoreinfo: %s\n",
			kdump_err_str(ctx));
		return TEST_ERR;
	}

	rc = TEST_OK;

	tmprc = check_string(ctx, ATTR_LINES ".OSRELEASE", OSRELEASE);
	if (tmprc == TEST_FAIL)
		return tmprc;
	if (tmprc != TEST_OK)
		rc = tmprc;

	tmprc = check_string(ctx, ATTR_LINES ".PAGESIZE", str(PAGESIZE));
	if (tmprc == TEST_FAIL)
		return tmprc;
	if (tmprc != TEST_OK)
		rc = tmprc;


	tmprc = check_string(ctx, ATTR_LINES ".SYMBOL(" SYM_NAME ")",
			     str(SYM_VALUE));
	if (tmprc == TEST_FAIL)
		return tmprc;
	if (tmprc != TEST_OK)
		rc = tmprc;

	tmprc = check_string(ctx, ATTR_OSRELEASE, OSRELEASE);
	if (tmprc == TEST_FAIL)
		return tmprc;
	if (tmprc != TEST_OK)
		rc = tmprc;

	status = kdump_get_attr(ctx, ATTR_PAGESIZE, &attr);
	if (status != kdump_ok) {
		fprintf(stderr, "%s: Cannot get value: %s\n",
			ATTR_PAGESIZE, kdump_err_str(ctx));
		return TEST_ERR;
	}
	if (attr.type != kdump_number) {
		fprintf(stderr, "%s: Wrong attribute type: %d\n",
			ATTR_PAGESIZE, (int) attr.type);
		rc = TEST_FAIL;
	} else if (attr.val.number != PAGESIZE) {
		fprintf(stderr, "%s: Invalid attribute value: %lld != %lld\n",
			ATTR_PAGESIZE, (long long) attr.val.number,
			(long long) PAGESIZE);
		rc = TEST_FAIL;
	} else
		printf("%s: %lld\n", ATTR_PAGESIZE,
		       (long long) attr.val.number);

	status = kdump_vmcoreinfo_symbol(ctx, SYM_NAME, &symval);
	if (status != kdump_ok) {
		fprintf(stderr, "%s: Cannot get value: %s\n",
			SYM_NAME, kdump_err_str(ctx));
		return TEST_ERR;
	}
	if (symval != SYM_VALUE) {
		fprintf(stderr, "%s: Invalid attribute value: %llx != %llx\n",
			SYM_NAME, (long long) symval, (long long) SYM_VALUE);
		rc = TEST_FAIL;
	} else
		printf("%s = %llx\n", SYM_NAME, (long long) symval);

	return rc;
}

int
main(int argc, char **argv)
{
	kdump_ctx *ctx;
	int rc;

	ctx = kdump_new();
	if (!ctx) {
		perror("Cannot initialize dump context");
		return TEST_ERR;
	}

	rc = check(ctx);

	kdump_free(ctx);
	return rc;
}
