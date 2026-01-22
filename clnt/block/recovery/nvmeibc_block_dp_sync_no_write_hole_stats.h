/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_ALL_RECOV_NO_WHOLE_STATS_H
#define NVMEIBC_DP_ALL_RECOV_NO_WHOLE_STATS_H

struct jdr;

/********************* Nowritehole syncs stats counter ************************/
struct nvmeibc_nowhole_stats {
	atomic_t n_dbits_fix;	// Number of dbits fix syncs
	atomic_t n_bdsec_fix;	// Number of bad sector fixes
	atomic_t n_rlbck_fix;	// Number of roll backwards syncs
	atomic_t n_scrub_fix;	// Number of scrubbed blocksets (found error ands fixed)
	atomic_t n_destoyed;	// Number of blocksets where at least 1 slice was destroyed (by sync or was already destroyed before-hand)
	atomic_t n_di_fix;		// Detected data corruptions (data state that should not exist, issued warning, and managed to fix this)
	atomic_t n_other_fix;	// In progress, other, non documented syncs
	atomic_t n_resets;		// Number of times this structure was reset. Without this field one cannot know of counters are 0 because they were cleaned or nothing happened in the past
};

void nvmeibc_nowhole_stats_tojson(struct jdr *jdr);               // Convert interal stats to JSON
void nvmeibc_nowhole_stats_reset(void);                           // Reset internal statrs
void nvmeibc_nowhole_stats_get(struct nvmeibc_nowhole_stats *rv); // Take a snapshot (copy) internal stats to 'rv'

#endif  // H beginning
