/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
/* Implements other Tomas in raft quorum of the alive Toma */
#include "../sandbox_util.h"
#include "nvmeibt_disk_segment_basics.h"	// enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE, NVMEIBT_MEM_TBL_INIT_MODE

#define PEER_TOMA_SIMU_MAX_SEGS       16	// >= D+P+1 across all peer-owned segs in any test praid
#define PEER_TOMA_SIMU_BIN_TOPO_MAX 4096	// Bound on the leader's BIN_TOPO blob we cache for ACT_TOPO emission

struct peer_toma_simu_seg_override {					// Specific actions to apply to the segment according to unitest scenario, A scenario-set per-seg dirty_bits override that ACT_TOPO emission applies on top of the leader's committed view. Used to inject state divergences that a real follower would produce from local work (e.g. OWNER_RECOVERER_DONE from recovery completion); the simulator does not model that work, so the test names the result explicitly. */
	uint32_t                              uuid;			// 32-bit shorthand matching mongodb_simu / pr->segs[i].uuid throughout the sandbox
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dirty_bits_state;
};

struct peer_toma_simu {
	struct sb_node_conf *node;							// Reference to node configuration in mongo-db
	uint32_t my_sw_version;
	int n_replies_to_leader;							// Count how many replies this peer sent to the leader, used for unit-test assertions
	bool ignore_append_entries;							// Emulates infinitely slow local disk response time, does not commit raft leaders topo, Much like real Toma 'enum raft_pause_mode_enm'
	unsigned long long ser_ver_per_seg_counter;			// Incrementing ACT_TOPO serialization version (per-seg wire field)
	unsigned long long running_local_serialization_version;	// Mirrors real nvmeibt_topology::running_local_serialization_version. Bumped on BIN_TOPO ingest and override changes; compared against the leader's echoed "known_to_leader" to gate ACT_TOPO attachment on REPs.
	char latest_bin_topo[PEER_TOMA_SIMU_BIN_TOPO_MAX];	// Last BIN_TOPO from the leader, stored in little endian; ACT_TOPO emission walks this on demand
	int  latest_bin_topo_len;

	struct peer_toma_simu_seg_override overrides[PEER_TOMA_SIMU_MAX_SEGS];
	int n_overrides;
};

struct peer_toma_simu *peer_toma_simu_create( struct sb_node_conf *node);
void                   peer_toma_simu_destroy(struct peer_toma_simu *);
void                   peer_toma_simu_ignore_append_entries_by_node(int node_idx);
void                   peer_toma_simu_resume_append_entries_by_node(int node_idx);

void peer_toma_simu_upd_committed_from_bin_topo(struct peer_toma_simu *peer, const void *bin_topo, int bin_topo_len);		// Cache the leader's latest BIN_TOPO blob. ACT_TOPO emission walks the cached buffer on demand instead of maintaining a parallel typed copy
int  peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *peer, char *out_buf, int out_buf_size);

void peer_toma_simu_set_seg_override(       struct peer_toma_simu *peer, uint32_t uuid, enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE state);
void peer_toma_simu_clear_seg_override(     struct peer_toma_simu *peer, uint32_t uuid);
void peer_toma_simu_clear_all_overrides(    struct peer_toma_simu *peer);
int  peer_toma_simu_complete_all_recoveries(struct peer_toma_simu *peer);
