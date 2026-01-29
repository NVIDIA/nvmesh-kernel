/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_DP_PROFILING_DISK_STAGES_H
#define NVMEIBC_BLOCK_DP_PROFILING_DISK_STAGES_H

#include "block/datapath_utils_generic/profiling/nvmeibc_block_dp_profiling_generic.h"

// Overloading operations as an indication that a PB was used
enum nvmeib_block_io_op_for_profiling {
	NVMEIB_BLOCK_IO_OP_ADD_PB = 0x30,
	NVMEIB_BLOCK_IO_OP_READ_PB,
	NVMEIB_BLOCK_IO_OP_WRITE_PB,
	NVMEIB_BLOCK_IO_OP_DISCARD_PB
};

// Use the operation and add to it if lpb is allocated
static inline int nvmeibc_profiling_get_cmd_stage(struct nvmeibc_disk_io_command *cmd)
{
	int is_piggy = dp_cmds_pigbck_has_any(cmd) ? NVMEIB_BLOCK_IO_OP_ADD_PB : 0;
	return cmd->reqs1.op + is_piggy;
}

static inline u8 nvmeibc_profiling_translate_block_io_op(int /*enum nvmeib_block_io_op*/ op) {
	if (op == PROFILING_GET_N_STAGES) {
		return 7;
	}
	switch(op){
	case NVMEIB_BLOCK_IO_OP_READ: return 0;
	case NVMEIB_BLOCK_IO_OP_WRITE: return 1;
	case NVMEIB_BLOCK_IO_OP_DISCARD: return 2;
	case NVMEIB_BLOCK_IO_OP_READ_PB: return 3;
	case NVMEIB_BLOCK_IO_OP_WRITE_PB: return 4;
	case NVMEIB_BLOCK_IO_OP_DISCARD_PB: return 5;
	default: return 6;
	}
	return op;
}

static inline const char *nvmeibc_profiling_block_io_op_stage2name(u8 stage)
{
	switch(stage){
	case 0: return "READ";
	case 1: return "WRITE";
	case 2: return "TRIM";
	case 3: return "READ+PB";
	case 4: return "WRITE+PB";
	case 5: return "TRIM+PB";
	default: return "OTHER";
	}
}

#endif  // H beginning

