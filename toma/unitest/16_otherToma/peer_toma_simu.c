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
	peer->does_support_incremental_topo = (node->uuid&0x1);		// 1 Toma In old sw version, another one in new
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

static struct toma_simu_inject_seg_state_t *__find_seg_inject(struct peer_toma_simu *T, uint32_t seg_uuid) {
	for (int i = 0; i < T->n_seg_overrides; i++) {
		if (T->segs_overrides[i].uuid == seg_uuid)
			return &T->segs_overrides[i];
	}
	return NULL;
}

static void __gen_seg_reply_to_leader(struct peer_toma_simu *T, const struct sb_seg_conf *sb_seg, const struct nvmeibt_serialized_seg_leader_topo *ld_seg, struct nvmeibt_serialized_seg_active_topo *act_seg) {
	struct toma_simu_inject_seg_state_t *inj = __find_seg_inject(T, sb_seg->uuid);
	BUG_ON(!act_seg);
	nvmeibt_strlcpy(act_seg->eyecatcher, "SFW", sizeof(act_seg->eyecatcher));					// Map BIN_TOPO seg fields -> ACT_TOPO seg fields: Todo unify with nvmeibt_topology_serialize_active_topology()
	act_seg->uuid = ld_seg->uuid;																// Copy everything from the leader
	act_seg->active_praid_version_major = ld_seg->praid_version_major;
	act_seg->active_praid_version_minor = ld_seg->praid_version_minor;
	act_seg->dirty_bits_state =           ld_seg->dirty_bits_state;
	act_seg->dirty_bits_init_mode =       ld_seg->dirty_bits_init_mode;
	act_seg->stale_locks_init_mode =      ld_seg->stale_locks_init_mode;
	act_seg->active_seg_ser_ver = ++T->ser_ver_per_seg_counter;
	act_seg->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = 1;

	// Now Apply the injected changes according to unitest scenario
	if (!T->ignore_segs_initialization) {
		/* The leader issues mem-tbl init commands (FIRST_USE_EVER for brand-new
		 * segments, TURN_ALL_ON/OFF and FROM_PERSIST during recovery) that the
		 * real peer executes locally and then reports as INIT_DONE. Simulate
		 * that completion by collapsing every actionable init_mode to
		 * INIT_DONE in the reply. Without this the leader stays in
		 * leader_is_waiting_for_any_remote_seg_to_apply_topo() because it sees
		 * the init command still outstanding. */
		const unsigned actionable = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER |
			NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON  |
			NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF |
			NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST;
		const bool was_fresh = (act_seg->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER);
		if (act_seg->dirty_bits_init_mode  & actionable) act_seg->dirty_bits_init_mode  = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE;
		if (act_seg->stale_locks_init_mode & actionable) act_seg->stale_locks_init_mode = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE;
		/* For segments the peer is seeing for the first time (init_mode was
		 * FIRST_USE_EVER) with the leader still at UNKNOWN, simulate the real
		 * peer finishing its local format+GPT by reporting OWNER_IDLE. Without
		 * this, the leader's nvmeibt_seg_lot_leader_convert_unusable_to_dead
		 * trap marks the segment DEAD during eviction's replacement flow
		 * (leader_switch_to_replacement_seg) and recovery never starts. */
		if (was_fresh && act_seg->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN)
			act_seg->dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE;
	}
	if (ld_seg->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO)
		act_seg->dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE;						// Toma done zeroing this disk segment
	if (inj) {
		act_seg->active_seg_flags.is_drive_write_error = inj->disk_error;
		act_seg->dirty_bits_state = inj->dbits_state;											// Here, inject degraded mode instead of owner idle, etc
		return;
	}
}

void peer_toma_simu_set_seg_inject(struct peer_toma_simu *T, const struct toma_simu_inject_seg_state_t *inj) {
	struct toma_simu_inject_seg_state_t *dst = __find_seg_inject(T, inj->uuid);	// Update existing override if present, or create a new one
	if (!dst) {
		dst = &T->segs_overrides[T->n_seg_overrides++];
		BUG_ON(T->n_seg_overrides > PEER_TOMA_SIMU_MAX_DIRTY_BITS_OVERRIDES);
	}
	*dst = *inj;
}

void peer_toma_simu_clear_seg_injects(struct peer_toma_simu *T) {
	T->n_seg_overrides = 0;
}

int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *T, const struct nvmeibt_topology_serialized_topo_header *leader_topo_data, int leader_topo_len, char *out_buf, int out_buf_size) {
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	const int my_node_idx = (int)(T->node - cfg->nodes);
	struct nvmeibt_act_topo_builder builder;
	char tmp[4096];
	struct nvmeibt_topology_serialized_topo_header hdr;
	struct nvmeibt_praid_serialized_topo *wire_praid = (typeof(wire_praid))&tmp[sizeof(hdr)];

	BUG_ON(!nvmeibt_topology_is_global_bin_topo(leader_topo_data));		// Only complete BIN_TOPO supported (sandbox never enables incremental)
	BUG_ON(leader_topo_len > (int)sizeof(tmp));
	memcpy(tmp, leader_topo_data, (size_t)leader_topo_len);				// Copy to temp buffer since byte-swap is in-place
	nvmeibt_topology_convert_header_le_be((typeof(&hdr))tmp, &hdr);		// Decode BIN_TOPO header
	nvmeibt_act_topo_builder_init(&builder, out_buf, out_buf_size);

	for (int i = 0; i < hdr.praids_num; i++) {							// Iterate praids and their segments (same pattern as wire_buf_test.c:find_praid_in_topo)
		struct nvmeibt_praid_serialized_topo ld_praid;					// ld = leader decoded
		struct nvmeibt_serialized_seg_leader_topo *wire_seg = (typeof(wire_seg))(&wire_praid[1]);
		nvmeibt_praid_convert_topo_le_be(wire_praid, &ld_praid);
		for (int j = 0; j < ld_praid.segs_num; j++) {
			struct nvmeibt_serialized_seg_leader_topo ld_seg;
			const struct sb_seg_conf *sb_seg;
			nvmeibt_disk_segment_convert_topo_le_be(&wire_seg[j], &ld_seg);
			sb_seg = sb_cluster_get_seg_ptr_from_uuid(cfg, (uint32_t)ld_seg.uuid.ll[0]);
			if (sb_cluster_get_node_idx_from_disk_uuid(cfg, sb_seg->disk_uuid) != my_node_idx)			// Filter: does this segment belong to this peer?
				continue;
			__gen_seg_reply_to_leader(T, sb_seg, &ld_seg, nvmeibt_act_topo_builder_append(&builder));
		}
		wire_praid = (typeof(wire_praid))&wire_seg[ld_praid.segs_num];
	}
	nvmeibt_act_topo_builder_to_wire(&builder);
	return builder.topo_len;
}
