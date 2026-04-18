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
	bool does_support_incremental_topo;					// Todo: Extend this mechanism to test toma software upgrade
	bool ignore_append_entries;							// Emulates infinitely slow local disk response time, does not commit raft leaders topo, Much like real Toma 'enum raft_pause_mode_enm'
	bool ignore_segs_initialization;					// Emulates as if Toma cannot initialize any local segment
	unsigned long long ser_ver_per_seg_counter;			// Incrementing ACT_TOPO serialization version
	unsigned long long append_entries_rep_ser_ver;		// Monotonic counter for raft-follower-msg.local_serialization_version on each APPEND_ENTRIES_REP. Increased so leader will take this reply
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

/** Drop every dirty_bits override previously installed via set_seg_inject().
 *  Use at the end of a scenario phase that injected a transient state so the
 *  peer resumes echoing the leader's normal topology. */
void peer_toma_simu_clear_seg_injects(struct peer_toma_simu *peer);
