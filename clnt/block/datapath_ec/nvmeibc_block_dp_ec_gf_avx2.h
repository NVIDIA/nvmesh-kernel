/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 *
 * Declarations for GF(EC) paths implemented in nvmeibc_block_dp_ec_gf_avx2.c.
 * Include after common/kr_incs.h (u32) and nvmeibc_block_dp_ec_gf_defs.h / gf.h as needed.
 */

#ifndef NVMEIBC_BLOCK_DP_EC_GF_AVX2_H
#define NVMEIBC_BLOCK_DP_EC_GF_AVX2_H

#include "common/kr_incs.h"
#include "nvmeibc_block_dp_ec_gf_defs.h"

void ec_encode_data_p_avx2(int len, int rows, unsigned char **data, unsigned char **coding,
			   u32 *crcp, unsigned char **data_copy);
void ec_encode_data_q_avx2(int len, int rows, unsigned char **data, unsigned char **coding,
			   u32 *crcp, unsigned char **data_copy);
void ec_encode_data_pq_avx2(int len, int rows, unsigned char **data, unsigned char **coding,
			    u32 *crcp, unsigned char **data_copy);
enum gf_return_val ec_encode_data_update_avx2(int len, int k, int vec_i, unsigned char **data,
					      unsigned char **coding, u32 *crcp,
					      unsigned char *data_copy);
void ec_decode_data_p_avx2(int len, int rows, int d0, unsigned char **data,
			   unsigned char **new_data, u32 *crcp);
void ec_decode_data_q_avx2(int len, int rows, int d0, unsigned char **data,
			   unsigned char **new_data, u32 *crcp);
void ec_decode_data_pq_avx2_asm(int len, int rows, int d0, int d1, unsigned char **data,
				unsigned char **new_data, u32 *crcp, unsigned char factor);

#if defined(__KERNEL__) && defined(__x86_64__)
void nvmeib_save_avx256(void *area);
void nvmeib_restore_avx256(void *area);
#endif

#endif /* NVMEIBC_BLOCK_DP_EC_GF_AVX2_H */
