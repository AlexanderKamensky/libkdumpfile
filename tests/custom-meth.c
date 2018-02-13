/* Custom translations method
   Copyright (C) 2017 Petr Tesarik <ptesarik@suse.com>

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

#include <stdio.h>

#include <libkdumpfile/addrxlat.h>

#include "testutil.h"

#define STEPS		2
#define XOR_VALUE	0xabcd
#define OFFSET		0x1111

#define TEST_ADDR	0x123456
#define EXPECT_ADDR	(((TEST_ADDR ^ XOR_VALUE) << 1) + OFFSET)

static addrxlat_status
first_step(addrxlat_step_t *step, addrxlat_addr_t addr)
{
	printf("First step: 0x%"ADDRXLAT_PRIxADDR"\n", addr);
	step->base.addr = addr ^ XOR_VALUE;
	step->remain = STEPS;
	step->elemsz = 1;
	step->idx[0] = OFFSET;
	step->idx[1] = 0;
	return ADDRXLAT_OK;
}

static addrxlat_status
next_step(addrxlat_step_t *step)
{
	printf("Next step #%u: 0x%"ADDRXLAT_PRIxADDR"\n",
	       STEPS - step->remain, step->base.addr);
	step->base.addr <<= 1;
	return ADDRXLAT_OK;
}

int
main(int argc, char **argv)
{
	addrxlat_meth_t meth;
	addrxlat_ctx_t *ctx;
	addrxlat_step_t step;
	addrxlat_status status;
	int result;

	ctx = addrxlat_ctx_new();
	if (!ctx) {
		perror("Cannot allocate context");
		return TEST_ERR;
	}

	meth.kind = ADDRXLAT_CUSTOM;
	meth.target_as = ADDRXLAT_NOADDR;
	meth.param.custom.first_step = first_step;
	meth.param.custom.next_step = next_step;

	step.ctx = ctx;
	step.sys = NULL;
	step.meth = &meth;
	status = addrxlat_launch(&step, 0x123456);
	if (status != ADDRXLAT_OK) {
		fprintf(stderr, "Cannot launch translation: %s\n",
			addrxlat_ctx_get_err(ctx));
		goto err;
	}

	while (step.remain) {
		status = addrxlat_step(&step);
		if (status != ADDRXLAT_OK) {
			fprintf(stderr, "Cannot step translation: %s\n",
				addrxlat_ctx_get_err(ctx));
			goto err;
		}
	}

	result = TEST_OK;
	printf("Result: 0x%"ADDRXLAT_PRIxADDR"\n", step.base.addr);
	if (step.base.addr != EXPECT_ADDR) {
		printf("-> does not match expectation (0x%lx)!\n",
		       (unsigned long)EXPECT_ADDR);
		result = TEST_FAIL;
	}
	addrxlat_ctx_decref(ctx);
	return result;

 err:
	addrxlat_ctx_decref(ctx);
	return TEST_ERR;
}
