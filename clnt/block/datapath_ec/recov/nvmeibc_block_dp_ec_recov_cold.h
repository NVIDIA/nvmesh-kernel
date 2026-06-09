/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_EC_RECOV_COLD_H
#define NVMEIBC_DP_EC_RECOV_COLD_H
/* Journal recoveries:
   1. Cold:
	Recovery Component:
		Analyze all journal entries (RAM from 4MB journal metadata) on each
		drive in protection raid, find candidates blocksets. Launch sync
		for each of them. Analysis stage is called 'crac' algorithm.
		Goal: After cold recovery journals are not needed.
	Sync Component:
		HTR sync logic which rolls-fwd/backward for each problematic blockset
		and fix it. Very much like regular HTR with stale lock but with more than
		1 possible candidate

   2. Journal garbage collection:
	Recovery Component:
		Subset of cold recovery, construct candidates but instead of launching
		HTR sync uses different sync
	Sync Component:
		Just test if lock is unlocked. If so clean the journals.
		Garbage collector can only free unneded journals and never changes data
 */
#include "block/recovery/nvmeibc_raid_recovery.h"
#include "nvmeibc_block.h"		/* external API of the block */
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"

/* JMDC read and create complete candidate list per block set */
void nvmeibc_block_dp_ec_recov_cold_start(struct nvmeibc_recovery *recov);

struct nvmibc_blockset_candidates;
void cold_iterator_free(struct nvmibc_blockset_candidates *candidates, u64 num_items);

/********************** DP sync cold virtal functions ************************/
// Cold virtal functions
int  dp_ec_sync_cold_prepare_op(struct recovery_sync_op *so);
void dp_ec_sync_cold_execute_op(struct recovery_sync_op *so);
void dp_ec_sync_cold_cb_stg_end(struct recovery_sync_op *so);

// Journal garbage collection. autonoums sync. No prepare, no stage callback
void dp_ec_sync_j_gc_execute_op(struct recovery_sync_op *so);

#include "nvmeibc_block_dp_ec_recov_stats.h"


/* PET trace declarations */
void pet_trace_cold_sync_err(const struct recovery_sync_op *so);

#endif  // H beginning

