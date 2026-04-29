/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
/* Implements other Tomas in raft quorum of the alive Toma */
#include "../sandbox_util.h"

#define PEER_TOMA_SIMU_MAX_DIRTY_BITS_OVERRIDES 8

struct peer_toma_simu {
	struct sb_node_conf *node;							// Reference to node configuration in mongo-db
	uint32_t my_sw_version;
	int n_replies_to_leader;							// Count how many replies this peer sent to the leader, used for unit-test assertions
	bool ignore_append_entries;							// Emulates infinitely slow local disk response time, does not commit raft leaders topo, Much like real Toma 'enum raft_pause_mode_enm'
	bool ignore_segs_initialization;					// Emulates as if Toma cannot initialize any local segment
	unsigned long long ser_ver_per_seg_counter;			// Incrementing ACT_TOPO serialization version (per-seg wire field)
	unsigned long long running_local_serialization_version;	// Mirrors real nvmeibt_topology::running_local_serialization_version. Bumps when simulated follower state changes (seg inject or BIN_TOPO receipt). Compared against the leader's echoed "known_to_leader" (read from incoming AE's local_serialization_version) to gate ACT_TOPO attachment on REPs -- is_applied_topo_ready_and_different, nvmeibt_raft.c:2864.
	struct toma_simu_inject_seg_state_t {				// Specific actions to apply to the segment according to unitest scenario
		uint32_t uuid;
		uint32_t dbits_state;
		bool     disk_error;
	} segs_overrides[PEER_TOMA_SIMU_MAX_DIRTY_BITS_OVERRIDES];
	int n_seg_overrides;
};

struct peer_toma_simu *peer_toma_simu_create( struct sb_node_conf *node);
void                   peer_toma_simu_destroy(struct peer_toma_simu *);
void                   peer_toma_simu_ignore_append_entries_by_node(int node_idx);
void                   peer_toma_simu_resume_append_entries_by_node(int node_idx);

struct nvmeibt_topology_serialized_topo_header;
int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *peer, const struct nvmeibt_topology_serialized_topo_header *leader_topo_data, int leader_topo_len, char *out_buf, int out_buf_size);

void peer_toma_simu_set_seg_inject(struct peer_toma_simu *peer, const struct toma_simu_inject_seg_state_t *inj);
void peer_toma_simu_clear_seg_injects(struct peer_toma_simu *peer);
