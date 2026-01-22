/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_SYNC_PROFILING_STAGES_H
#define NVMEIBC_BLOCK_SYNC_PROFILING_STAGES_H

#include "block/datapath_utils_generic/profiling/nvmeibc_block_dp_profiling_generic.h"

enum nvmeibc_profiling_sync_stages{
	NVMEIBC_PROFILING_SYNC_STAGE_SYNC_SUBMIT_UP_TO_SYNC_START,
	NVMEIBC_PROFILING_SYNC_STAGE_SYNC_START_UP_TO_ALL_LOCKS_TAKEN,
	NVMEIBC_PROFILING_SYNC_STAGE_LOCKS_TAKEN_UP_TO_UNLOCK_SM,
	NVMEIBC_PROFILING_SYNC_STAGE_UNLOCK_SM_TP_TO_SYNC_FREE,
	NVMEIBC_PROFILING_SYNC_STAGE_ERROR,
	NVMEIBC_PROFILING_SYNC_N_STAGES //the last valid stage
};

static inline u8 nvmeibc_profiling_translate_sync_stages(int /*enum nvmeibc_rdma_intent*/ sync_stage){
	if (unlikely(sync_stage == PROFILING_GET_N_STAGES)){
		return NVMEIBC_PROFILING_SYNC_N_STAGES;
	}
	return (u8)sync_stage;
}

static inline const char *nvmeibc_profiling_sync_stage_stage2name(u8 /*enum nvmeibc_profiling_sync_stages*/ stage_index)
{
	switch ((enum nvmeibc_profiling_sync_stages)stage_index) {
	case NVMEIBC_PROFILING_SYNC_STAGE_SYNC_SUBMIT_UP_TO_SYNC_START:	return "SYNC_SUBMIT_UP_TO_SYNC_START";
	case NVMEIBC_PROFILING_SYNC_STAGE_SYNC_START_UP_TO_ALL_LOCKS_TAKEN: return "SYNC_START_UP_TO_ALL_LOCKS_TAKEN";
	case NVMEIBC_PROFILING_SYNC_STAGE_LOCKS_TAKEN_UP_TO_UNLOCK_SM: return "LOCKS_TAKEN_UP_TO_UNLOCK_SM";
	case NVMEIBC_PROFILING_SYNC_STAGE_UNLOCK_SM_TP_TO_SYNC_FREE: return "UNLOCK_SM_TP_TO_SYNC_FREE";
	case NVMEIBC_PROFILING_SYNC_STAGE_ERROR:
			FALLTHRU;
	default: return "ERROR";
	}
}

#endif // H beginning
