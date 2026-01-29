/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef _NVMEIBC_BLOCK_DP_EC_GF_DEFS_H_
#define _NVMEIBC_BLOCK_DP_EC_GF_DEFS_H_

#include "nvmeibc_block_dp_ec_gf_arm_defs.h"

enum nvmeibc_gf_optimization_type {
    NVMEIBC_GF_DISPLAY_CURRENT  = -2,   // Instruction to display the current optimization value
    NVMEIBC_GF_AUTO_INIT        = -1,   // Default value, and request for auto initialization
    NVMEIBC_GF_UNOPTIMIZED      =  0,   // All valid values must be >= 0
    NVMEIBC_GF_64_BIT           =  1,
    NVMEIBC_GF_SSE2             =  2,
    NVMEIBC_GF_AVX2             =  3,
    NVMEIBC_GF_EC_CALC          =  4,
    NVMEIBC_GF_ARM_INTRINSICS   =  5,   //optional; available on some platforms
    NVMEIBC_GF_TOTAL
};

enum gf_return_val {
	GF_ERROR = -1,
	GF_SUCCESS = 0,
	GF_DEFERRED = 1,
};

typedef void (gf_callback)(void *ctx, int rv);				// Callback for future hardware offloading

#define GF_MAX_P        2			// Support up to EC 14+2
#define GF_MAX_D        14
#define GF_BUFFER_QUANT (4096)		// Deprecated, todo, remove from Kernel and UM repositories

#endif /*_NVMEIBC_BLOCK_DP_EC_GF_DEFS_H_ */

