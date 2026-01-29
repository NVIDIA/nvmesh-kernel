/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_common.h"
#include "nvmeibt_disk_segment_basics.h"

const char *mem_tbl_init_mode_str(enum NVMEIBT_MEM_TBL_INIT_MODE m)
{
	switch (m) {
	case NVMEIBT_MEM_TBL_INIT_MODE_UNUSED_0: return "UNUSED_0";
	case NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN: return "UNKNOWN";
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED: return "INIT_REQUIRED";
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE: return "INIT_DONE";
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT: return "INIT_IRRELEVANT";
	case NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST: return "FROM_PERSIST";
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON: return "TURN_ALL_ON";
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF: return "TURN_ALL_OFF";
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER: return "FIRST_USE_EVER";
	default : {
		static char	unexpected_val_str[] = "unexpected value               ";
		snprintf(unexpected_val_str, sizeof(unexpected_val_str), "unexpected value %x", m);
		return unexpected_val_str;
	}
	}
}

const char *dirty_bits_state_str(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE s)
{
	switch (s) {
	case NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN: return "UNKNOWN";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE: return "ALIVE_STABLE";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE: return "ALIVE_UNSTABLE";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD: return "DEAD";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO: return "X_ZERO";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE: return "X_DONE";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I: return "UNDER_RECOVERY_I";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_R: return "UNDER_RECOVERY_R";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER: return "OWNER_RECOVERER";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE: return "OWNER_RECOVERER_DONE";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE: return "OWNER_IDLE";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERED: return "OWNER_RECOVERED";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER: return "EC_COLD_RECOVERER";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE: return "EC_COLD_RECOVERER_DONE";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_UNUSED_0: return "0";
	default : {
		static char	unexpected_val_str[] = "unexpected value               ";
		snprintf(unexpected_val_str, sizeof(unexpected_val_str), "unexpected value %x", s);
		return unexpected_val_str;
	}
	}
}


