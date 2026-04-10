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
#include <string.h>

struct peer_toma_simu *peer_toma_simu_create(struct sb_node_conf *node) {
	struct peer_toma_simu *peer = calloc(1, sizeof(*peer));
	peer->node = node;
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

static unsigned int peer_toma_simu_lookup_dirty_bits_override(
	struct peer_toma_simu *peer, const union nvmeib_uuid *seg_uuid,
	unsigned int default_state)
{
	for (int i = 0; i < peer->n_dirty_bits_overrides; i++)
		if (ARE_UUID_EQ(&peer->dirty_bits_overrides[i].uuid, seg_uuid))
			return peer->dirty_bits_overrides[i].state;
	return default_state;
}

void peer_toma_simu_set_seg_dirty_bits(struct peer_toma_simu *peer,
	union nvmeib_uuid seg_uuid, unsigned int dirty_bits_state)
{
	// Update existing override if present
	for (int i = 0; i < peer->n_dirty_bits_overrides; i++) {
		if (ARE_UUID_EQ(&peer->dirty_bits_overrides[i].uuid, &seg_uuid)) {
			peer->dirty_bits_overrides[i].state = dirty_bits_state;
			return;
		}
	}
	// Add new override
	BUG_ON(peer->n_dirty_bits_overrides >= PEER_TOMA_SIMU_MAX_DIRTY_BITS_OVERRIDES);
	peer->dirty_bits_overrides[peer->n_dirty_bits_overrides].uuid = seg_uuid;
	peer->dirty_bits_overrides[peer->n_dirty_bits_overrides].state = dirty_bits_state;
	peer->n_dirty_bits_overrides++;
}

int peer_toma_simu_build_act_topo_reply(struct peer_toma_simu *peer,
	const char *leader_topo_data, int leader_topo_len,
	char *out_buf, int out_buf_size)
{
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	int my_node_idx = (int)(peer->node - cfg->nodes);
	struct nvmeibt_act_topo_builder builder;
	char tmp[4096];
	struct nvmeibt_topology_serialized_topo_header hdr;
	struct nvmeibt_praid_serialized_topo *wire_praid;

	// Only complete BIN_TOPO supported (sandbox never enables incremental)
	BUG_ON(!nvmeibt_topology_is_global_bin_topo(leader_topo_data));
	BUG_ON(leader_topo_len > (int)sizeof(tmp));

	// Copy to temp buffer since byte-swap is in-place
	memcpy(tmp, leader_topo_data, (size_t)leader_topo_len);

	// Decode BIN_TOPO header
	nvmeibt_topology_convert_header_le_be(
		(struct nvmeibt_topology_serialized_topo_header *)tmp, &hdr);

	nvmeibt_act_topo_builder_init(&builder, out_buf, out_buf_size);

	// Iterate praids and their segments (same pattern as wire_buf_test.c:find_praid_in_topo)
	wire_praid = (struct nvmeibt_praid_serialized_topo *)
		(tmp + sizeof(struct nvmeibt_topology_serialized_topo_header));

	for (int i = 0; i < hdr.praids_num; i++) {
		struct nvmeibt_praid_serialized_topo host_praid;
		struct nvmeibt_serialized_seg_leader_topo *wire_seg;

		nvmeibt_praid_convert_topo_le_be(wire_praid, &host_praid, TOMA_SW_COMPATIBILITY_VER);
		wire_seg = (struct nvmeibt_serialized_seg_leader_topo *)
			((char *)wire_praid + sizeof(*wire_praid));

		for (int j = 0; j < host_praid.segs_num; j++) {
			struct nvmeibt_serialized_seg_leader_topo host_seg;
			struct nvmeibt_serialized_seg_active_topo *act_seg;
			const struct sb_seg_conf *sb_seg;
			int seg_node_idx;

			nvmeibt_disk_segment_convert_topo_le_be(&wire_seg[j], &host_seg);

			// Filter: does this segment belong to this peer?
			sb_seg = sb_cluster_get_seg_ptr_from_uuid(cfg, (uint32_t)host_seg.uuid.ll[0]);
			if (!sb_seg)
				continue;
			seg_node_idx = sb_cluster_get_node_idx_from_disk_uuid(cfg, sb_seg->disk_uuid);
			if (seg_node_idx != my_node_idx)
				continue;

			// Map BIN_TOPO seg fields -> ACT_TOPO seg fields
			act_seg = nvmeibt_act_topo_builder_append(&builder);
			BUG_ON(!act_seg);
			nvmeibt_strlcpy(act_seg->eyecatcher, "SFW", sizeof(act_seg->eyecatcher));
			act_seg->uuid = host_seg.uuid;
			act_seg->active_praid_version_major = host_seg.praid_version_major;
			act_seg->active_praid_version_minor = host_seg.praid_version_minor;
			act_seg->dirty_bits_state = peer_toma_simu_lookup_dirty_bits_override(
				peer, &host_seg.uuid, host_seg.dirty_bits_state);
			act_seg->dirty_bits_init_mode = host_seg.dirty_bits_init_mode;
			act_seg->stale_locks_init_mode = host_seg.stale_locks_init_mode;
			act_seg->active_seg_ser_ver = ++peer->ser_ver_counter;
			act_seg->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = 1;
		}

		// Advance past this praid's segments to the next praid
		wire_praid = (struct nvmeibt_praid_serialized_topo *)
			((char *)wire_praid + sizeof(*wire_praid)
				+ nvmeibt_praid_wire_get_n_segs(wire_praid) * (int)sizeof(struct nvmeibt_serialized_seg_leader_topo));
	}

	nvmeibt_act_topo_builder_to_wire(&builder);
	return builder.topo_len;
}
