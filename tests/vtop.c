/* Virtual to physical translation.
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

#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <kdumpfile.h>
#include <addrxlat.h>

#include "testutil.h"

static int
vtop(kdump_ctx_t *ctx, unsigned long long vaddr)
{
	addrxlat_fulladdr_t faddr;
	addrxlat_ctx_t *axctx;
	addrxlat_sys_t *axsys;
	addrxlat_status axstatus;

	axctx = kdump_get_addrxlat_ctx(ctx);
	axsys = kdump_get_addrxlat_sys(ctx);
	faddr.addr = vaddr;
	faddr.as = ADDRXLAT_KVADDR;
	axstatus = addrxlat_by_sys(axctx, axsys, &faddr, ADDRXLAT_KPHYSADDR);
	addrxlat_sys_decref(axsys);
	if (axstatus != ADDRXLAT_OK) {
		fprintf(stderr, "VTOP translation failed: %s\n",
			addrxlat_ctx_get_err(axctx));
		addrxlat_ctx_decref(axctx);
		return TEST_FAIL;
	}
	addrxlat_ctx_decref(axctx);

	printf("0x%"ADDRXLAT_PRIxADDR"\n", faddr.addr);

	return TEST_OK;
}

static int
vtop_fd(int fd, unsigned long long vaddr)
{
	kdump_ctx_t *ctx;
	kdump_status res;
	int rc;

	ctx = kdump_new();
	if (!ctx) {
		perror("Cannot initialize dump context");
		return TEST_ERR;
	}

	res = kdump_set_number_attr(ctx, KDUMP_ATTR_FILE_FD, fd);
	if (res != KDUMP_OK) {
		fprintf(stderr, "Cannot open dump: %s\n", kdump_get_err(ctx));
		rc = TEST_ERR;
	} else
		rc = vtop(ctx, vaddr);

	kdump_free(ctx);
	return rc;
}

static void
usage(FILE *f, const char *prog)
{
	fprintf(f,
		"Usage: %s <dump> <vaddr>\n\n"
		"Options:\n"
		"  --help     Print this help and exit\n"
		"  --pgt      Force pagetable translation\n",
		prog);
}

static const struct option opts[] = {
	{ "help", no_argument, NULL, 'h' },
	{ NULL, 0, NULL, 0 }
};

int
main(int argc, char **argv)
{
	unsigned long long addr;
	FILE *fhelp;
	char *endp;
	int fd;
	int rc;
	int c;

	fhelp = stdout;
	while ( (c = getopt_long(argc, argv, "h", opts, NULL)) != -1)
		switch (c) {
		case '?':
			fhelp = stderr;
		case 'h':
			usage(fhelp, argv[0]);
			if (fhelp == stderr)
				return TEST_ERR;
			return TEST_OK;
		}

	if (argc -optind != 2) {
		usage(stderr, argv[0]);
		return TEST_ERR;
	}

	addr = strtoull(argv[optind+1], &endp, 0);
	if (*endp) {
		fprintf(stderr, "Invalid address: %s", argv[2]);
		return TEST_ERR;
	}

	fd = open(argv[optind], O_RDONLY);
	if (fd < 0) {
		perror("open dump");
		return TEST_ERR;
	}

	rc = vtop_fd(fd, addr);

	if (close(fd) < 0) {
		perror("close dump");
		rc = TEST_ERR;
	}

	return rc;
}
