/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "peer_toma_simu.h"
#include "nvmeibt_debug.h"	// Binary tracing
#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_praid_basics.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_topo_bin.h"
#include "../13_mgmt/mongodb_simu.h"

struct peer_toma_simu *peer_toma_simu_create(struct sb_node_conf *node) {
	struct peer_toma_simu *peer = calloc(1, sizeof(*peer));
	peer->node = node;
	peer->my_sw_version = ((node->uuid&0x1) ? TOMA_SW_VER : TOMA_SW_VER_MIN_FOR_INCREMENTAL-0x20);		// 1 Toma In old sw version, another one in new
	return peer;
}

void peer_toma_simu_destroy(struct peer_toma_simu *peer) {
	peer->node->peer = NULL;
	free(peer);
}

void peer_toma_simu_ignore_append_entries_by_node(int node_idx) {
	struct sb_cluster_conf *cfg = (struct sb_cluster_conf *)sb_cluster_get_const_conf();
	BUG_ON(node_idx != 2);			// Our volumes configuration, currently supports only ignore by node 2
	cfg->nodes[node_idx].peer->ignore_append_entries = true;
}

void peer_toma_simu_resume_append_entries_by_node(int node_idx) {
	struct sb_cluster_conf *cfg = (struct sb_cluster_conf *)sb_cluster_get_const_conf();
	cfg->nodes[node_idx].peer->ignore_append_entries = false;
}

/* Collapse actionable init_mode values to INIT_DONE. The leader issues mem-tbl init commands
 * (FIRST_USE_EVER for brand-new segments, TURN_ALL_ON/OFF and FROM_PERSIST during recovery)
 * that the real peer executes locally and then reports as INIT_DONE. The sandbox skips the
 * "execute" step and collapses straight to DONE. */
static inline void __progress_seg_init_mode(enum NVMEIBT_MEM_TBL_INIT_MODE *s, enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE seg_state) {
	const enum NVMEIBT_MEM_TBL_INIT_MODE actionable = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER |
		NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON | NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF |
		NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO | NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST;
	const enum NVMEIBT_MEM_TBL_INIT_MODE do_nothing = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE | NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT;
	if (*s & actionable) {
		*s = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE;
	} else if (seg_state == NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD) {	// For Dead seg, just clean the RAM, no instruction how.
		BUG_ON(*s != NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);		// Do nothing, Leader has to give proper instruction
	} else {
		BUG_ON((*s & do_nothing) == 0);		// Invalid enum value sent
	}
}

/* Apply the committed -> applied transition rules for seg index i.
 *
 *   1. praid_version always tracks committed (pure value sync).
 *   2. Never-demote: if applied is OWNER_RECOVERER_DONE (the only "_DONE" the scenario
 *      ever injects via complete_all_recoveries), preserve it. The leader hasn't yet
 *      processed our previous report so committed still says OWNER_RECOVERER; demoting
 *      back would lose Phase 5's inject between AE rounds. Once the leader catches up,
 *      committed moves past OWNER_RECOVERER and acceptance resumes naturally.
 *   3. Otherwise state-machine acceptance: applied takes committed's dbits + init modes.
 *      Then zero-latency auto-progress for instant follower work:
 *        - X_ZERO -> X_DONE
 *        - INIT modes actionable -> INIT_DONE  (unless ignore_segs_initialization)
 *        - FIRST_USE_EVER + UNKNOWN -> OWNER_IDLE
 */
static void apply_committed_to_active(struct peer_toma_simu *T, int i) {
	const struct peer_toma_simu_seg_topo *c = &T->committed_segs[i];
	struct peer_toma_simu_seg_topo *a = &T->applied_segs[i];
	const enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE prev_applied_dbits = a->dirty_bits_state;

	a->praid_version_major = c->praid_version_major;
	a->praid_version_minor = c->praid_version_minor;

	if (c->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER &&
		a->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE) {
		if (!T->ignore_segs_initialization) {
			__progress_seg_init_mode(&a->dirty_bits_init_mode,  c->dirty_bits_state);
			__progress_seg_init_mode(&a->stale_locks_init_mode, c->dirty_bits_state);
		}
		N_Tf(__AUTOID__, "seg=@UUID_8 applied ahead of committed: keep applied=@DIRTY_BITS_STATE_STR (committed=@DIRTY_BITS_STATE_STR)",
			(uint32_t)a->uuid.ll[0], dirty_bits_state_str(a->dirty_bits_state), dirty_bits_state_str(c->dirty_bits_state));
		return;
	}

	a->dirty_bits_state      = c->dirty_bits_state;
	a->dirty_bits_init_mode  = c->dirty_bits_init_mode;
	a->stale_locks_init_mode = c->stale_locks_init_mode;

	if (c->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO)
		a->dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE;

	if (!T->ignore_segs_initialization) {
		__progress_seg_init_mode(&a->dirty_bits_init_mode,  c->dirty_bits_state);
		__progress_seg_init_mode(&a->stale_locks_init_mode, c->dirty_bits_state);
	}

	if (c->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER &&
		c->dirty_bits_state     == NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN) {
		a->dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE;
	}

	if (a->dirty_bits_state != prev_applied_dbits) {
		N_Tf(__AUTOID__, "seg=@UUID_8 applied=@DIRTY_BITS_STATE_STR->@DIRTY_BITS_STATE_STR (committed=@DIRTY_BITS_STATE_STR)",
			(uint32_t)a->uuid.ll[0], dirty_bits_state_str(prev_applied_dbits), dirty_bits_state_str(a->dirty_bits_state),
			dirty_bits_state_str(c->dirty_bits_state));
	}
}

void peer_toma_simu_upd_committed_from_bin_topo(struct peer_toma_simu *T, const void *bin_topo_buf, int bin_topo_len) {
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	const int my_node_idx = (int)(T->node - cfg->nodes);
	char tmp[4096];
	struct nvmeibt_topology_serialized_topo_header hdr;
	struct nvmeibt_praid_serialized_topo *wire_praid = (typeof(wire_praid))&tmp[sizeof(hdr)];

	/* Snapshot prior applied so we can match incoming uuids against existing applied state.
	 * Without this, a re-ingest would lose any scenario-injected applied progression
	 * (complete_all_recoveries) written between BIN_TOPO arrivals. */
	struct peer_toma_simu_seg_topo old_applied[PEER_TOMA_SIMU_MAX_SEGS];
	const int n_old = T->n_segs;
	memcpy(old_applied, T->applied_segs, (size_t)n_old * sizeof(*old_applied));

	BUG_ON(!nvmeibt_topology_is_global_bin_topo((const struct nvmeibt_topology_serialized_topo_header *)bin_topo_buf));	// Only complete BIN_TOPO supported (sandbox never enables incremental)
	BUG_ON(bin_topo_len > (int)sizeof(tmp));
	memcpy(tmp, bin_topo_buf, (size_t)bin_topo_len);					// Copy to temp buffer since byte-swap is in-place
	nvmeibt_topology_convert_header_le_be((typeof(&hdr))tmp, &hdr);		// Decode BIN_TOPO header

	T->n_segs = 0;														// BIN_TOPO is the leader's *full* committed topology; a seg dropped in a new BIN_TOPO must not linger. See scenario_evict_rebuild_r1 Phase 3 which drops the deprecated seg.
	for (int p = 0; p < hdr.praids_num; p++) {							// Iterate praids and their segments (same pattern as wire_buf_test.c:find_praid_in_topo)
		struct nvmeibt_praid_serialized_topo ld_praid;					// ld = leader decoded
		struct nvmeibt_serialized_seg_leader_topo *wire_seg = (typeof(wire_seg))(&wire_praid[1]);
		nvmeibt_praid_convert_topo_le_be(wire_praid, &ld_praid);
		for (int s = 0; s < ld_praid.segs_num; s++) {
			struct nvmeibt_serialized_seg_leader_topo ld_seg;
			const struct sb_seg_conf *sb_seg;
			int i, old_idx;
			nvmeibt_disk_segment_convert_topo_le_be(&wire_seg[s], &ld_seg);
			sb_seg = sb_cluster_get_seg_ptr_from_uuid(cfg, (uint32_t)ld_seg.uuid.ll[0]);
			if (sb_cluster_get_node_idx_from_disk_uuid(sb_seg->disk_uuid) != my_node_idx)
				continue;												// Skip segs this peer doesn't own
			BUG_ON(T->n_segs >= PEER_TOMA_SIMU_MAX_SEGS);
			i = T->n_segs++;

			/* Existing seg: carry over applied so apply_committed_to_active can preserve
			 * an injected OWNER_RECOVERER_DONE. Fresh seg: seed applied=committed; the
			 * auto-progress rules will fire FIRST_USE_EVER + INIT progression. */
			old_idx = -1;
			for (int k = 0; k < n_old; k++) {
				if ((uint32_t)old_applied[k].uuid.ll[0] == (uint32_t)ld_seg.uuid.ll[0]) {
					old_idx = k;
					break;
				}
			}

			T->committed_segs[i] = (struct peer_toma_simu_seg_topo){
				.uuid                   = ld_seg.uuid,
				.praid_version_major    = ld_seg.praid_version_major,
				.praid_version_minor    = ld_seg.praid_version_minor,
				.dirty_bits_state       = ld_seg.dirty_bits_state,
				.dirty_bits_init_mode   = ld_seg.dirty_bits_init_mode,
				.stale_locks_init_mode  = ld_seg.stale_locks_init_mode,
			};

			T->applied_segs[i] = (old_idx >= 0) ? old_applied[old_idx] : T->committed_segs[i];
			apply_committed_to_active(T, i);
		}
		wire_praid = (typeof(wire_praid))&wire_seg[ld_praid.segs_num];
	}
	T->running_local_serialization_version++;	// Applied state may have changed; next AE handler's is_applied_topo_ready_and_different gate will see running != leader_echoed.
}

int peer_toma_simu_complete_all_recoveries(struct peer_toma_simu *T) {
	int n = 0;
	for (int i = 0; i < T->n_segs; i++) {
		struct peer_toma_simu_seg_topo *a = &T->applied_segs[i];
		if (a->dirty_bits_state != NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER)
			continue;
		N_Tf(__AUTOID__, "seg=@UUID_8 complete_all_recoveries: applied OWNER_RECOVERER->OWNER_RECOVERER_DONE",
			(uint32_t)a->uuid.ll[0]);
		a->dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE;
		T->running_local_serialization_version++;
		n++;
	}
	return n;
}
int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *T, char *out_buf, int out_buf_size) {
	struct nvmeibt_act_topo_builder builder;
	nvmeibt_act_topo_builder_init(&builder, out_buf, out_buf_size);
	for (int i = 0; i < T->n_segs; i++) {
		const struct peer_toma_simu_seg_topo *a = &T->applied_segs[i];
		struct nvmeibt_serialized_seg_active_topo *act_seg = nvmeibt_act_topo_builder_append(&builder);
		BUG_ON(!act_seg);
		nvmeibt_strlcpy(act_seg->eyecatcher, "SFW", sizeof(act_seg->eyecatcher));	// S-segment, F-follower, W-wire
		act_seg->uuid                        = a->uuid;
		act_seg->active_praid_version_major  = a->praid_version_major;
		act_seg->active_praid_version_minor  = a->praid_version_minor;
		act_seg->dirty_bits_state            = a->dirty_bits_state;
		act_seg->dirty_bits_init_mode        = a->dirty_bits_init_mode;
		act_seg->stale_locks_init_mode       = a->stale_locks_init_mode;
		act_seg->active_seg_ser_ver          = ++T->ser_ver_per_seg_counter;
		act_seg->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = 1;
		act_seg->active_seg_flags.is_drive_write_error = 0;
		N_Tf(__AUTOID__, "ACT_TOPO reply seg=@UUID_8 dbits=@DIRTY_BITS_STATE_STR ser_ver=@INT",
			(uint32_t)a->uuid.ll[0], dirty_bits_state_str(a->dirty_bits_state), (int)act_seg->active_seg_ser_ver);
	}
	nvmeibt_act_topo_builder_to_wire(&builder);
	return builder.topo_len;
}
