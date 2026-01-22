/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CMP_BLOCKS_IMPL_H
#define CMP_BLOCKS_IMPL_H

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	#include "nvmeib_common_all.h"
	#define NVMEIBC_SECTOR_SIZE	(1 << NVMEIBC_SECTOR_SHIFT)
#else
	#include "kr_incs.h"
#endif

#include "nvmeib_types.h"
#include "../../../clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_blk.h"
#include "../../../clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "../../../clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_print.h"

#define EXIT_ON(condition) do { const int hit = !!(condition); if(hit) { fprintf(stderr, "Run time error at %s() line %d, val=%d, condition=%s\n", __FUNCTION__, __LINE__, hit, #condition); return CB_RUNTIME_ERROR; } } while(0)

typedef struct cmp_blocks_file_data {
	u64 segment[NVMEIBC_SECTOR_SIZE/8] __attribute__((aligned(32))); 	// 4K aligned to 32 bytes for AVX
	union {
		union nvmeibc_block_dp_ec_data_block_md md;
		u32 edic_qlc;
	};
} __attribute__((packed)) cmp_blocks_file_data;

struct cmp_blks_archive {
	char* buf;
	size_t count;
	size_t len;
};

typedef enum {
	CB_OK = 0,
	CB_CRC_MISMATCH = 1,
	CB_PARITY_MISMATCH = 2,
	CB_CRC_AND_PARITY_MISMATCH = CB_PARITY_MISMATCH|CB_CRC_MISMATCH,
	CB_DATA_LOSS = 4,				// Enough bad sectors to cause partial/full data loss
	CB_RUNTIME_ERROR = 1024
} cmp_blocks_retval; 				//Bitfield enum

cmp_blocks_retval cmp_blocks_mirror(
	cmp_blocks_file_data f_data[N_MAX_RAID_SLICE_LEN],
	const bool dbg_di,
	const uint64_t rlba,
	const int verbose_level,
	const bool has_metadata);		// R1 can be with/without metadata

cmp_blocks_retval cmp_blocks_ec(
	const u32 data_size,			/*The number of data block*/
	const u32 parity_size,			/*The number of parity block*/
	cmp_blocks_file_data f_data[N_MAX_RAID_SLICE_LEN],	/*The slice data and parity*/
	const bool check_crc,			/*If needs to be checked*/
	const bool dbg_di,				/*If the blocks contain dbg_di data*/
	const u32 snake_size,			/*The snake size for rlba update*/
	const uint64_t rlba,			/*The rlba of D0*/
	const bool fix_data,			/*if true f_data will be fixed*/
	const bool fix_crc,				/*if true the crc will be fixed*/
	const int verbose_level,		/*Enable printings*/
	const u32 reconst_bmp           /*the bmp of the valid sources in case of need to reconstruct*/
	);

struct _gen_md_params {
	union nvmeibc_dbits_entry dbits;
	bool is_parity;
	bool is_r1;
};

cmp_blocks_retval gen_md(
	cmp_blocks_file_data *f_data,
	const struct _gen_md_params *par,
	const bool dbg_di,
	const uint64_t rlba,
	const bool fix_md,				// if true f_data->md will be fixed
	const int _verbose);

#endif //CMP_BLOCKS_IMPL_H

