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

static inline enum NVMEIBT_MEM_TBL_INIT_MODE __init_mode_normalized(enum NVMEIBT_MEM_TBL_INIT_MODE m, enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE seg_state) {
	const enum NVMEIBT_MEM_TBL_INIT_MODE actionable = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER |
		NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON | NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF |
		NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO | NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST;
	const enum NVMEIBT_MEM_TBL_INIT_MODE do_nothing = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE | NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT;
	if (m & actionable) {
		return NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE;
	}
	if (seg_state == NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD) {	// For Dead seg, just clean the RAM, no instruction how.
		BUG_ON(m != NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);		// Do nothing, Leader has to give proper instruction
		return m;
	}
	BUG_ON((m & do_nothing) == 0);		// Invalid enum value sent
	return m;
}

void peer_toma_simu_upd_committed_from_bin_topo(struct peer_toma_simu *peer, const void *bin_topo_buf, int bin_topo_len) {
	BUG_ON(!nvmeibt_topology_is_global_bin_topo(bin_topo_buf));	// Only complete BIN_TOPO supported (sandbox never enables incremental)
	BUG_ON(bin_topo_len > (int)sizeof(peer->latest_bin_topo));
	memcpy(peer->latest_bin_topo, bin_topo_buf, (size_t)bin_topo_len);
	peer->latest_bin_topo_len = bin_topo_len;
	peer->running_local_serialization_version++;	// Next AE handler's is_applied_topo_ready_and_different gate will see running != leader_echoed.
}

typedef void (*peer_seg_visit_fn)(void *ctx, const struct nvmeibt_serialized_seg_leader_topo *seg, const struct sb_seg_conf *sb_seg);

/* Walk the peer's owned segs in the cached BIN_TOPO and call visit() for each one. */
static void peer_for_each_local_seg(struct peer_toma_simu *peer, peer_seg_visit_fn visit, void *ctx) {
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	const int my_node_idx = (int)(peer->node - cfg->nodes);
	char tmp[PEER_TOMA_SIMU_BIN_TOPO_MAX];
	struct nvmeibt_topology_serialized_topo_header hdr;
	struct nvmeibt_praid_serialized_topo *wire_praid = (typeof(wire_praid))&tmp[sizeof(hdr)];

	if (peer->latest_bin_topo_len == 0) return;
	memcpy(tmp, peer->latest_bin_topo, (size_t)peer->latest_bin_topo_len);	// *_convert_*_le_be helpers swap in place; walk the throwaway copy so the cached buffer stays pristine
	nvmeibt_topology_convert_header_le_be((typeof(&hdr))tmp, &hdr);

	for (int p = 0; p < hdr.praids_num; p++) {
		struct nvmeibt_praid_serialized_topo ld_praid;
		struct nvmeibt_serialized_seg_leader_topo *wire_seg = (typeof(wire_seg))(&wire_praid[1]);
		nvmeibt_praid_convert_topo_le_be(wire_praid, &ld_praid);
		for (int s = 0; s < ld_praid.segs_num; s++) {
			struct nvmeibt_serialized_seg_leader_topo ld_seg;
			const struct sb_seg_conf *sb_seg;
			nvmeibt_disk_segment_convert_topo_le_be(&wire_seg[s], &ld_seg);
			sb_seg = sb_cluster_get_seg_ptr_from_uuid(cfg, (uint32_t)ld_seg.uuid.ll[0]);
			if (sb_cluster_get_node_idx_from_disk_uuid(sb_seg->disk_uuid) == my_node_idx)
				visit(ctx, &ld_seg, sb_seg);
		}
		wire_praid = (typeof(wire_praid))&wire_seg[ld_praid.segs_num];
	}
}

static struct peer_toma_simu_seg_override *__find_override(struct peer_toma_simu *peer, uint32_t uuid) {
	for (int i = 0; i < peer->n_overrides; i++)
		if (peer->overrides[i].uuid == uuid) return &peer->overrides[i];
	return NULL;
}

void peer_toma_simu_set_seg_override(struct peer_toma_simu *peer, uint32_t uuid, enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE state) {
	struct peer_toma_simu_seg_override *o = __find_override(peer, uuid);
	if (!o) {
		BUG_ON(peer->n_overrides >= PEER_TOMA_SIMU_MAX_SEGS);
		o = &peer->overrides[peer->n_overrides++];
		o->uuid = uuid;
	}
	o->dirty_bits_state = state;
	peer->running_local_serialization_version++;
	N_Tf(__AUTOID__, "peer override seg=@UUID_8 -> @DIRTY_BITS_STATE_STR", uuid, dirty_bits_state_str(state));
}

void peer_toma_simu_clear_seg_override(struct peer_toma_simu *peer, uint32_t uuid) {
	struct peer_toma_simu_seg_override *o = __find_override(peer, uuid);
	if (!o) return;
	*o = peer->overrides[--peer->n_overrides];	// Swap-with-last is fine; ordering is not observable
	peer->running_local_serialization_version++;
	N_Tf(__AUTOID__, "peer override seg=@UUID_8 cleared", uuid);
}

void peer_toma_simu_clear_all_overrides(struct peer_toma_simu *peer) {
	if (peer->n_overrides == 0) return;
	peer->n_overrides = 0;
	peer->running_local_serialization_version++;
	N_Tf(__AUTOID__, "peer overrides cleared (all)");
}

struct __recovery_visit_ctx { struct peer_toma_simu *peer; int n; };
static void __recovery_visit(void *ctxp, const struct nvmeibt_serialized_seg_leader_topo *seg, const struct sb_seg_conf *sb_seg) {
	struct __recovery_visit_ctx *ctx = ctxp;
	(void)sb_seg;
	if (seg->dirty_bits_state != NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER) return;
	peer_toma_simu_set_seg_override(ctx->peer, (uint32_t)seg->uuid.ll[0], NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE);
	ctx->n++;
}

int peer_toma_simu_complete_all_recoveries(struct peer_toma_simu *peer) {
	struct __recovery_visit_ctx ctx = { .peer = peer, .n = 0 };
	peer_for_each_local_seg(peer, __recovery_visit, &ctx);
	return ctx.n;
}

struct __act_topo_ctx { struct peer_toma_simu *peer; struct nvmeibt_act_topo_builder *builder; };
static void __act_topo_visit(void *ctxp, const struct nvmeibt_serialized_seg_leader_topo *seg, const struct sb_seg_conf *sb_seg) {
	struct __act_topo_ctx *ctx = ctxp;
	struct peer_toma_simu *peer = ctx->peer;
	struct nvmeibt_serialized_seg_active_topo *act_seg = nvmeibt_act_topo_builder_append(ctx->builder);
	const struct peer_toma_simu_seg_override *o = __find_override(peer, (uint32_t)seg->uuid.ll[0]);
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dbits;
	if (o) {
		dbits = o->dirty_bits_state;					// Override is authoritative; no wire normalization on top
	} else {
		dbits = seg->dirty_bits_state;
		if (dbits == NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO) dbits = NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE;	// Wire-normalize: leader expects acknowledgment that zeroing finished, simulator does not model the work
	}
	(void)sb_seg;
	BUG_ON(!act_seg);
	nvmeibt_strlcpy(act_seg->eyecatcher, "SFW", sizeof(act_seg->eyecatcher));	// S-segment, F-follower, W-wire
	act_seg->uuid                        = seg->uuid;
	act_seg->active_praid_version_major  = seg->praid_version_major;
	act_seg->active_praid_version_minor  = seg->praid_version_minor;
	act_seg->dirty_bits_state            = dbits;
	act_seg->dirty_bits_init_mode        = __init_mode_normalized(seg->dirty_bits_init_mode,  dbits);
	act_seg->stale_locks_init_mode       = __init_mode_normalized(seg->stale_locks_init_mode, dbits);
	act_seg->active_seg_ser_ver          = ++peer->ser_ver_per_seg_counter;
	act_seg->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = 1;
	act_seg->active_seg_flags.is_drive_write_error = 0;
	N_Tf(__AUTOID__, "ACT_TOPO reply seg=@UUID_8 dbits=@DIRTY_BITS_STATE_STR ser_ver=@INT",
		(uint32_t)seg->uuid.ll[0], dirty_bits_state_str(dbits), (int)act_seg->active_seg_ser_ver);
}

int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *peer, char *out_buf, int out_buf_size) {
	struct nvmeibt_act_topo_builder builder;
	struct __act_topo_ctx ctx = { .peer = peer, .builder = &builder };
	nvmeibt_act_topo_builder_init(&builder, out_buf, out_buf_size);
	peer_for_each_local_seg(peer, __act_topo_visit, &ctx);
	nvmeibt_act_topo_builder_to_wire(&builder);
	return builder.topo_len;
}
