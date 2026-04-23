/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
/* Implements other Tomas in raft quorum of the alive Toma */
#include "../sandbox_util.h"
#include "nvmeibt_disk_segment_basics.h"	// enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE, NVMEIBT_MEM_TBL_INIT_MODE

#define PEER_TOMA_SIMU_MAX_SEGS 16	// >= D+P+1 across all peer-owned segs in any test praid

/* Per-seg topology. Used in two roles on each peer_toma_simu, mirroring the real follower's
 * committed and applied segment lots:
 *   committed: leader-authored directive, updated from BIN_TOPO (analog of
 *              seg_follower.committed_seg_lot.seg_topo).
 *   applied:   follower-authored live state that ACT_TOPO emission reads verbatim
 *              (analog of seg_follower.applied_seg_lot.seg_topo). Seeded from committed
 *              with simulated-apply transformations and then mutated by scenario injections.
 *
 * Real toma also has a third in-RAM layer (seg_active->active_seg_topo_ctx) for live I/O
 * state; the sandbox does not model it. Field layout matches what ACT_TOPO emission needs;
 * praid_version_{major,minor} types match the wire struct nvmeibt_serialized_seg_leader_topo. */
struct peer_toma_simu_seg_topo {
	union nvmeib_uuid						uuid;
	uint64_t								praid_version_major;
	uint64_t								praid_version_minor;
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	dirty_bits_state;
	enum NVMEIBT_MEM_TBL_INIT_MODE			dirty_bits_init_mode;
	enum NVMEIBT_MEM_TBL_INIT_MODE			stale_locks_init_mode;
};

/* Scenario-authored injection (written from unit tests into the peer's applied layer). */
struct toma_simu_inject_seg_state_t {
	uint32_t uuid;			// Low 32 bits of the seg uuid
	uint32_t dbits_state;
};

struct peer_toma_simu {
	struct sb_node_conf *node;							// Reference to node configuration in mongo-db
	uint32_t my_sw_version;
	int n_replies_to_leader;							// Count how many replies this peer sent to the leader, used for unit-test assertions
	bool ignore_append_entries;							// Emulates infinitely slow local disk response time, does not commit raft leaders topo, Much like real Toma 'enum raft_pause_mode_enm'
	bool ignore_segs_initialization;					// Emulates as if Toma cannot initialize any local segment
	unsigned long long ser_ver_per_seg_counter;			// Incrementing ACT_TOPO serialization version (per-seg wire field)
	unsigned long long running_local_serialization_version;	// Mirrors real nvmeibt_topology::running_local_serialization_version. Bumps when applied_segs[] content may have changed (BIN_TOPO ingest, seg inject, clear injects). Compared against the leader's echoed "known_to_leader" (read from incoming AE's local_serialization_version) to gate ACT_TOPO attachment on REPs.
	struct peer_toma_simu_seg_topo committed_segs[PEER_TOMA_SIMU_MAX_SEGS];	// Leader-authored state (filtered to this peer's owned segs) from the latest BIN_TOPO.
	struct peer_toma_simu_seg_topo applied_segs[PEER_TOMA_SIMU_MAX_SEGS];		// Follower-authored live state. Seeded from committed with simulated-apply transforms on every BIN_TOPO ingest; scenario injections mutate it; ACT_TOPO emission reads it verbatim.
	int n_segs;											// Count for both committed_segs[] and applied_segs[]. Invariant: committed_segs[i].uuid == applied_segs[i].uuid for i in [0, n_segs).
};

struct peer_toma_simu *peer_toma_simu_create( struct sb_node_conf *node);
void                   peer_toma_simu_destroy(struct peer_toma_simu *);
void                   peer_toma_simu_ignore_append_entries_by_node(int node_idx);
void                   peer_toma_simu_resume_append_entries_by_node(int node_idx);

/* Ingest a BIN_TOPO blob from the leader. Refreshes committed_segs[] (replacing prior content)
 * for segs owned by this peer, then seeds applied_segs[] from committed with zero-latency
 * simulated-apply transforms. Named to parallel real
 * nvmeibt_seg_follower_upd_committed_seg_topo(). */
void peer_toma_simu_upd_committed_from_bin_topo(struct peer_toma_simu *peer, const void *bin_topo, int bin_topo_len);

/* Build an ACT_TOPO reply body by walking applied_segs[]. */
int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *peer, char *out_buf, int out_buf_size);

/* Scenario-authored mutation of the applied layer. Requires the uuid to already exist in
 * applied_segs[] (i.e., covered by a prior BIN_TOPO); BUG_ONs otherwise. */
void peer_toma_simu_set_seg_inject(struct peer_toma_simu *peer, const struct toma_simu_inject_seg_state_t *inj);

/* Wipe any scenario-written applied state by re-seeding applied_segs[] from committed_segs[]. */
void peer_toma_simu_clear_seg_injects(struct peer_toma_simu *peer);
