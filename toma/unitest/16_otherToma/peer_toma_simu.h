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
	bool ignore_append_entries;			                // Emulates infinitely slow local disk response time, does not commit raft leaders topo
	unsigned long long ser_ver_counter;					// Incrementing ACT_TOPO serialization version
	struct {
		uint32_t uuid;
		uint32_t state;
	} dirty_bits_overrides[PEER_TOMA_SIMU_MAX_DIRTY_BITS_OVERRIDES];
	int n_dirty_bits_overrides;
};

struct peer_toma_simu *peer_toma_simu_create( struct sb_node_conf *node);
void                   peer_toma_simu_destroy(struct peer_toma_simu *);
void                   peer_toma_simu_ignore_append_entries_by_node(int node_idx);
void                   peer_toma_simu_resume_append_entries_by_node(int node_idx);

/** Build a per-peer ACT_TOPO reply from the leader's BIN_TOPO.
 *  Decodes the leader's topology, filters to segments on this peer's disks,
 *  maps fields, applies dirty_bits overrides, and encodes as ACT_TOPO.
 *  Returns the ACT_TOPO byte length written to out_buf, or 0 on error. */
int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *peer,
	const char *leader_topo_data, int leader_topo_len,
	char *out_buf, int out_buf_size);

/** Set a dirty_bits_state override for a segment. When building the ACT_TOPO
 *  reply, if a segment matches this UUID, the override state is used instead
 *  of the leader's value. Used by the eviction test to fake recovery completion. */
void peer_toma_simu_set_seg_dirty_bits(struct peer_toma_simu *peer, uint32_t seg_uuid, uint32_t dirty_bits_state);
