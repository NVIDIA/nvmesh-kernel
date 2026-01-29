/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_SYNC_MANAGER_EXTERNAL_API_H
#define NVMEIBC_SYNC_MANAGER_EXTERNAL_API_H
/* Blockdevice/datapath should must implement this api to be able to use syncs */
struct nvmeibc_datapath_syncs_resources {	// Resources / stats for running syncs for block device
	struct list_head request_list;			// Linked list of sync operations which are performed async by the resubmit thread
	struct list_head resources_reuse_list;	// Resources which syncs completed syncs havent freed to be reused by future syncs
	struct nvmeibc_sync_stats {				// Statistics of sync per volume (todo: extend to be per protection raid)
		// Real time stats
		uint num_running;					// Amount of currently executing sync operations. Not atomic_t because it is protected by spinlock
		int num_pending;					// Amount of elements in 'sync.request_list' (amount of syncs waiting to be submitted). May be large if num_running_syncs is capped at low number
		uint num_el_in_resources_reuse_list;
		// Cumulative stats
		u64 num_total;						// For debug only: Amount of attempts to sync locks, this block device did since attach
		u64 num_total_maintainance;			// Amount of maintanance ops
		u64	num_dirty_bit_suspect;			// Amount of DB suspect syncs for R1 and EC DB suspect that turned into a DB sync
		u64 num_full_blockset_ok;			// Amount of successfull syncs operations (regardless of whether data was identical or was actually copied)
		u64 num_part_blockset_ok;			// Amount of successfull syncs operations (regardless of whether data was identical or was actually copied)
		u64 num_commit_stale;				//
		u64 num_commit_binfo;
		#ifdef AUTONOMOUS_SYNCS_STATS
			atomic64_t num_autonomous_ok;	// Count of successfull autonomous syncs
			atomic64_t num_autonomous_err;	// Count of failed autonomous syncs
		#endif
		uint longest_sync_time;				// In units of [msecs]
		uint n_resources_reused;			// For debugging the resource transfer mechanism between syncs
	} stats;
	struct nvmeibc_flow_counters* fcntr;	// Preparation for per block device flow counters, currently only points to global structure
};
void nvmeibc_datapath_syncs_resources_init(    struct nvmeibc_datapath_syncs_resources *);
bool nvmeibc_datapath_syncs_can_launch(  const struct nvmeibc_datapath_syncs_resources *);
bool nvmeibc_datapath_syncs_any_pending_unsafe(const struct nvmeibc_datapath_syncs_resources *);
void nvmeibc_datapath_syncs_resources_destroy( struct nvmeibc_datapath_syncs_resources *);
void nvmeibc_datapath_syncs_zero_stats(        struct nvmeibc_datapath_syncs_resources *);

/************************** Global flow counters *****************************/
/* Various counters which fire on specific flows of syncs. Global in the system, not per block device.
	In future we might want to move some/all of those counters to be per block-device or per protection raid.
*/
#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole_stats.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_stats.h"

struct nvmeibc_flow_counters {
	struct nvmeibc_nowhole_stats       nowh;
	struct nvmeibc_cold_stats          jour;
	struct nvmeibc_maintain_sync_stats main;
	struct htr_stats                   htrs;
};
struct nvmeibc_flow_counters* nvmeibc_flow_counters_ref(void); // Reference for syncs to fill in

static inline void nvmeibc_flow_counters_reset(void)
{
	nvmeibc_htr_stats_reset();
	nvmeibc_cold_stats_reset();
	nvmeibc_maintain_sync_stats_reset();
	nvmeibc_nowhole_stats_reset();
}

#endif // NVMEIBC_SYNC_MANAGER_EXTERNAL_API_H
