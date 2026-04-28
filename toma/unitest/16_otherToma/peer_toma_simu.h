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
 *   committed: leader-authored directive, written verbatim from each BIN_TOPO ingest
 *              (analog of seg_follower.committed_seg_lot.seg_topo).
 *   applied:   follower-authored live state that ACT_TOPO emission reads verbatim
 *              (analog of seg_follower.applied_seg_lot.seg_topo). Mutates via
 *              apply_committed_to_active() on each ingest (state-machine acceptance plus
 *              zero-latency auto-progress for X_ZERO/INIT/FIRST_USE_EVER) and via the
 *              scenario event peer_toma_simu_complete_recovery(). The OWNER_RECOVERER
 *              -> OWNER_RECOVERER_DONE transition the scenario injects is preserved across
 *              re-ingest by an inline never-demote check inside apply_committed_to_active.
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

struct peer_toma_simu {
	struct sb_node_conf *node;							// Reference to node configuration in mongo-db
	uint32_t my_sw_version;
	int n_replies_to_leader;							// Count how many replies this peer sent to the leader, used for unit-test assertions
	bool ignore_append_entries;							// Emulates infinitely slow local disk response time, does not commit raft leaders topo, Much like real Toma 'enum raft_pause_mode_enm'
	bool ignore_segs_initialization;					// Emulates as if Toma cannot initialize any local segment (global pin equivalent for INIT)
	unsigned long long ser_ver_per_seg_counter;			// Incrementing ACT_TOPO serialization version (per-seg wire field)
	unsigned long long running_local_serialization_version;	// Mirrors real nvmeibt_topology::running_local_serialization_version. Bumps when applied_segs[] content may have changed (BIN_TOPO ingest, scenario events). Compared against the leader's echoed "known_to_leader" (read from incoming AE's local_serialization_version) to gate ACT_TOPO attachment on REPs.
	struct peer_toma_simu_seg_topo committed_segs[PEER_TOMA_SIMU_MAX_SEGS];	// Leader-authored state (filtered to this peer's owned segs) from the latest BIN_TOPO.
	struct peer_toma_simu_seg_topo applied_segs[PEER_TOMA_SIMU_MAX_SEGS];		// Follower-authored live state. ACT_TOPO emission reads this verbatim.
	int n_segs;											// Count for committed_segs[] and applied_segs[]. Invariant: committed_segs[i].uuid == applied_segs[i].uuid for i in [0, n_segs).
};

struct peer_toma_simu *peer_toma_simu_create( struct sb_node_conf *node);
void                   peer_toma_simu_destroy(struct peer_toma_simu *);
void                   peer_toma_simu_ignore_append_entries_by_node(int node_idx);
void                   peer_toma_simu_resume_append_entries_by_node(int node_idx);

/* Ingest a BIN_TOPO blob from the leader. Updates committed_segs[] verbatim for segs
 * owned by this peer, mirroring nvmeibt_seg_follower_upd_committed_seg_topo().
 * For each seg also runs apply_committed_to_active()
 * which updates applied_segs[i] per the auto-rules: state-machine acceptance,
 * zero-latency completion of unpinned work transitions, never demote a previously
 * scenario-injected OWNER_RECOVERER_DONE back to OWNER_RECOVERER. Segs no longer in
 * BIN_TOPO are dropped from both arrays. */
void peer_toma_simu_upd_committed_from_bin_topo(struct peer_toma_simu *peer, const void *bin_topo, int bin_topo_len);

/* Build an ACT_TOPO reply body by walking applied_segs[]. */
int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *peer, char *out_buf, int out_buf_size);

/* Scenario-driven event: recovery client signaled completion. Valid only when applied
 * dirty_bits_state is OWNER_RECOVERER. Writes OWNER_RECOVERER_DONE on the applied
 * layer and bumps running_local_serialization_version. Subsequent BIN_TOPO ingests
 * with committed still at OWNER_RECOVERER will not demote it. BUG_ONs if seg uuid
 * is not present in applied_segs[] or applied state is wrong. */
void peer_toma_simu_complete_recovery(struct peer_toma_simu *peer, uint32_t seg_uuid);
