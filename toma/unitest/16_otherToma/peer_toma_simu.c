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
 * that the real peer executes locally and then reports as INIT_DONE. We skip the "execute"
 * step and collapse straight to DONE. */
static inline void __progress_seg_init_mode(enum NVMEIBT_MEM_TBL_INIT_MODE *s, enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE seg_state) {
	const enum NVMEIBT_MEM_TBL_INIT_MODE actionable = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER |
		NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON | NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF |
		NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO | NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST;
	const enum NVMEIBT_MEM_TBL_INIT_MODE do_nothing = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE | NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT;
	if (*s & actionable) {
		*s = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE;
	} else if (seg_state == NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD) {	// For Dead seg, just clean the RAM, no instruction how.
		BUG_ON(*s != NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);		// Do nothing, Leader has to give proper instruction
		// *s = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE;
	} else {
		BUG_ON((*s & do_nothing) == 0);		// Invalid enum value sent
	}
}

/* Seed applied_segs[i] from committed_segs[i] with "simulated-apply" transformations: whatever
 * local work the real peer would have had to do (memory-table init, zeroing), we assume
 * instantly done. This is where a future sandbox work-queue simulation would plug in to
 * model latency between leader's committed directive and follower's applied completion. */
static void __seed_applied_from_committed(struct peer_toma_simu *T, int i) {
	const struct peer_toma_simu_seg_topo *c = &T->committed_segs[i];
	struct peer_toma_simu_seg_topo *a = &T->applied_segs[i];
	*a = *c;
	if (!T->ignore_segs_initialization) {	// The leader issues mem-tbl init commands that the real peer executes and reports as INIT_DONE. Otherwise leader stays in leader_is_waiting_for_any_remote_seg_to_apply_topo().
		__progress_seg_init_mode(&a->dirty_bits_init_mode,  c->dirty_bits_state);
		__progress_seg_init_mode(&a->stale_locks_init_mode, c->dirty_bits_state);
		if ((c->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER) && (c->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN))
			a->dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE;	// For first-seen segs, simulate the peer finishing format+GPT by reporting OWNER_IDLE. Without this, nvmeibt_seg_lot_leader_convert_unusable_to_dead marks the segment DEAD during eviction's replacement flow (leader_switch_to_replacement_seg).
	}
	if (c->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO)
		a->dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE;			// Toma done zeroing this disk segment
}

/* Linear search applied_segs[] by low-32-bits uuid (the injection API uses a 32-bit handle). */
static int __find_seg_by_uuid32(const struct peer_toma_simu *T, uint32_t uuid32) {
	for (int i = 0; i < T->n_segs; i++) {
		if ((uint32_t)T->applied_segs[i].uuid.ll[0] == uuid32)
			return i;
	}
	return -1;
}

void peer_toma_simu_upd_committed_from_bin_topo(struct peer_toma_simu *T, const void *bin_topo_buf, int bin_topo_len) {
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	const int my_node_idx = (int)(T->node - cfg->nodes);
	char tmp[4096];
	struct nvmeibt_topology_serialized_topo_header hdr;
	struct nvmeibt_praid_serialized_topo *wire_praid = (typeof(wire_praid))&tmp[sizeof(hdr)];

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
			int i;
			nvmeibt_disk_segment_convert_topo_le_be(&wire_seg[s], &ld_seg);
			sb_seg = sb_cluster_get_seg_ptr_from_uuid(cfg, (uint32_t)ld_seg.uuid.ll[0]);
			if (sb_cluster_get_node_idx_from_disk_uuid(sb_seg->disk_uuid) != my_node_idx)
				continue;												// Skip segs this peer doesn't own
			BUG_ON(T->n_segs >= PEER_TOMA_SIMU_MAX_SEGS);
			i = T->n_segs++;
			T->committed_segs[i] = (struct peer_toma_simu_seg_topo){
				.uuid                   = ld_seg.uuid,
				.praid_version_major    = ld_seg.praid_version_major,
				.praid_version_minor    = ld_seg.praid_version_minor,
				.dirty_bits_state       = ld_seg.dirty_bits_state,
				.dirty_bits_init_mode   = ld_seg.dirty_bits_init_mode,
				.stale_locks_init_mode  = ld_seg.stale_locks_init_mode,
			};
			__seed_applied_from_committed(T, i);
		}
		wire_praid = (typeof(wire_praid))&wire_seg[ld_praid.segs_num];
	}
	T->running_local_serialization_version++;	// Applied state may have changed; next AE handler's is_applied_topo_ready_and_different gate will see running != leader_echoed.
}

void peer_toma_simu_set_seg_inject(struct peer_toma_simu *T, const struct toma_simu_inject_seg_state_t *inj) {
	const int i = __find_seg_by_uuid32(T, inj->uuid);
	BUG_ON(i < 0);	// Scenario injected state for a seg the peer hasn't received in BIN_TOPO yet
	T->applied_segs[i].dirty_bits_state = inj->dbits_state;
	T->running_local_serialization_version++;	// Mirrors the serializer bump at nvmeibt_topology.c:1179.
}

void peer_toma_simu_clear_seg_injects(struct peer_toma_simu *T) {
	for (int i = 0; i < T->n_segs; i++)
		__seed_applied_from_committed(T, i);
	T->running_local_serialization_version++;
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
	}
	nvmeibt_act_topo_builder_to_wire(&builder);
	return builder.topo_len;
}
