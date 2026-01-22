/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_random.h"
#include "../nvmeibc_block_common.h"
#include "./uni_framework/bunitest_conf.h"
#include "../uni_framework/unitest_defs.h"
#include "../uni_framework/range_algorithms.h"
#include "block/controlpath/nvmeibc_b_cp_blkset_topo.h"


u32 __rand_n_bits_bitmap(int n_bits, int n_segs) {
	int r = rand()%(n_segs*n_segs);
	u32 bit_1 = (1 << (r%n_segs));
	if (n_bits == 1) return bit_1;
	if (n_bits == 2) return bit_1 | (1 << (r/n_segs));
	BUG(); return 0;			// Not supported yet
}

roles_bmp_t rand_valid_txbm(const struct TstPRaid sraid, const bool with_parities){
	const u16 slice_size = sraid.cpr->slice_size;
	roles_bmp_t data = 0;
	if (slice_size == 1){
		data = 1;
	} else if (slice_size == 2){
		data = (rand() % 2);
	} else {
		const u8 offset = rand() % slice_size;
		const u8 length = max(1, rand() % (slice_size - offset));
		data = GENMASK(offset+length-1, offset);
	}
	if (with_parities)
		data |= nvmeibc_raid1_get_parities_bmp(sraid.cpr);
	return data;
}


void rand_valid_txbm_multi_slice(const struct TstPRaid sraid, const bool with_parities, u32 tx_bm[], const u32 tx_height, bool with_snake) {
	u32 h;
	for (h = 0; h < tx_height; h++) {
		const u16 slice_size = sraid.cpr->slice_size;
		roles_bmp_t data = 0;
		if (slice_size == 1){
			data = 1;
		} else {
			u8 length;
			bool is_first_slice = h == 0;
			bool is_last_slice = h == (tx_height - 1);
			bool is_fisrt_or_last_slice = (h == 0 || h == (tx_height - 1));
			const u8 offset = (with_snake || is_first_slice) ? rand() % slice_size : 0; // without snake last slice should start at data 0
			if (with_snake)
				length = is_fisrt_or_last_slice ? max(1, rand() % (slice_size - offset + 1)) : rand() % (slice_size - offset + 1); // First and last slices should always contain data seg
			else if (is_last_slice)
				length = max(1, rand() % (slice_size - offset + 1));
			else
				length = (slice_size - offset + 1);  // without snake all slices but the last should be full.

			if (length) // else data = 0
				data = GENMASK(offset+length-1, offset);
		}
		if (with_parities)
			data |= nvmeibc_raid1_get_parities_bmp(sraid.cpr);

		tx_bm[h] = data;
	}

	return;
}
