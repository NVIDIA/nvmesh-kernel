/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "kr_incs.h"
#include "../common/compat/kr_incs_malloc.inc.c"
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf.h"
int util_initialize_gf_layer(void) {
	extern void init_roles_pair_compression_tables(void);
	int rv, capability =__gf_choose_functions(NVMEIBC_GF_AUTO_INIT);
	(void)capability; //fprintf(stderr, "Crc/GF type = %s(%d)\n", nvmeibc_gf_optimization_to_string(capability), capability);
	init_roles_pair_compression_tables();
	rv = nvmeib_allocate_xsave_bufs();
	return rv;
}
