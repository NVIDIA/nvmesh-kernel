/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_VOLUME_TYPE_H
#define NVMEIB_VOLUME_TYPE_H

#include "nvmeib_math.h"
enum nvmeibc_config_volume_type {					// List of flags
	NORMAL_VOLUME =            0x0,					// Regular (thick) block device as implemented since nvmesh 2.0
	// -------------- generic flags, can combine with any other  -------------
	RECOVERER_VOLUME =         0x1,					// Recoverer volume, ignores all reservation info, only for recovery purposes, will be auto-detach if no recoveries are running,Usually is also hidden but not a must
	HIDDEN_VOLUME =		       0x2,					// Hidden Volume. No kernel IO allowed, unlike recoverer hidden volume is not auto-detached
	AUTO_EXTEND_VOLUME =       0x4,					// Block device which acts as thin provisioned auto growing volume. Allow writes to tange > max_vlba
	SHADOW_VOLUME =            0x10,				// Shadow(encrypted) volume, created by Toma for a short period of time; should behave like normal volume
													// Except: status.* files should report it as SHADOW & should NOT send keep alive report to management
	UNKNOWN_ILLEGAL =     0xBADFAC,					// Illegal type of volume, can be inserted to func's which search volumes to search all volume types
};

static inline bool __is_vol_type_valid(enum nvmeibc_config_volume_type t)
{
	const unsigned long valid_types[] = {
		NORMAL_VOLUME,
		NORMAL_VOLUME | RECOVERER_VOLUME,
		NORMAL_VOLUME | HIDDEN_VOLUME,
		NORMAL_VOLUME | AUTO_EXTEND_VOLUME,
		NORMAL_VOLUME | SHADOW_VOLUME
	}; 
	size_t idx = 0;
	for(idx = 0; idx < ARRAY_SIZE(valid_types); ++idx){
		if (valid_types[idx] == t)
			return true;
	}	
	return false;
}

#define nvmeibc_block_is_recoverer(v) (                    RECOVERER_VOLUME & (v)->type)		// 1 Bit flag
#define nvmeibc_block_is_hidden(v) (         	           HIDDEN_VOLUME    & (v)->type)		// 1 Bit flag
#define nvmeibc_block_is_shadow(v) (         	           SHADOW_VOLUME    & (v)->type)		// 1 Bit flag
#define nvmeibc_block_is_recoverer_or_hidden(v) ((HIDDEN_VOLUME|RECOVERER_VOLUME) & (v)->type)	// Todo: Remove in future. Back-compatible for 2.2.0 and below + Old managements. Mgmt cares about this condition.

#endif /* NVMEIB_VOLUME_TYPE_H */
