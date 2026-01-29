/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "kr_incs.h"
#include "../../../common/nvmeib_types.h"
#include "../../../clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_blk.h"
#include "../../../clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_print.h"

#define EXIT_ON(condition) do { const int hit = !!(condition); if(hit) {  fprintf(stderr, "Run time error at %s() line %d, val=%d, condition=%s\n", __FUNCTION__, __LINE__, hit, #condition); return 255; } } while(0)
#define VERSION "1.4" /* Please make sure to update this in case breaking changes are made */
#ifndef COMMIT_ID
	#define COMMIT_ID 0xdeadbeef
#endif
#define OUT_SIZE (4*NVMEIBC_SECTOR_SIZE)
int main(int argc, char *argv[]) {
	char binary_block[NVMEIBC_SECTOR_SIZE], out[OUT_SIZE];
	data_blk *data = (void*)binary_block;
	int size, i;
	char *fn = argv[1], *outf = argv[2];
	FILE *f, *f2;
	if (argc < 3) {
		fprintf(stderr, "Version: " VERSION "\n");
		fprintf(stderr, "commit 0x%lx. Args <binary_block_file> <output_txt_file>\n", (unsigned long)COMMIT_ID);
		return 255;
	}
	f = fopen(fn, "rb");
	if (!f) {
		fprintf(stderr, "could not open file %s for read\n", fn);
		return 255;
	}
	f2 = fopen(outf, "ab+");
	if (!f2) {
		fprintf(stderr, "cannot open output file for write (%s)\n", outf);
		return 255;
	}

	EXIT_ON(fread(data, sizeof(binary_block), 1, f) != 1);
	{
		extern void init_roles_pair_compression_tables(void);
		init_roles_pair_compression_tables();
	}
	for (i = 0; !feof(f); i++) {
		size = (int)data_blk_to_string(data, out, OUT_SIZE);
		fprintf(stderr, "parsed block: %d, len %d\n", i, size);
		fprintf(f2, "Block: %d, len %d, \n%s", i, size, out);
		EXIT_ON(fread(data, sizeof(binary_block), 1, f) > 1);
	}
	fclose(f);
	fclose(f2);
	return 0;
}
