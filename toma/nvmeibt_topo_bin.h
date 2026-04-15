/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_TOPO_BIN
#define NVMEIBT_TOPO_BIN

void nvmeibt_disk_segment_print_leader_wire_topo(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_serialized_seg_leader_topo *seg_wire_topo_ptr);
void nvmeibt_seg_serialized_active_topo(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_serialized_seg_active_topo *serialized_seg_topo_ptr);
void nvmeibt_disk_segment_convert_topo_le_be(struct nvmeibt_serialized_seg_leader_topo *src_ptr, struct nvmeibt_serialized_seg_leader_topo *dst_ptr);
void nvmeibt_disk_segment_convert_active_bin_topo_le_be(struct nvmeibt_serialized_seg_active_topo *seg_ptr);

void nvmeibt_praid_print_leader_wire_topo(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_praid_serialized_topo *praid_wire_topo);
void nvmeibt_praid_convert_topo_le_be(const struct nvmeibt_praid_serialized_topo *src_ptr, struct nvmeibt_praid_serialized_topo *dst_ptr);

void nvmeibt_topology_print_topo_header(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_topology_serialized_topo_header *header_ptr);
void nvmeibt_topology_print(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, const struct nvmeibt_Buf *wire_topo_buf, bool check_max_len);
void nvmeibt_topology_follower_print(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, const void *serialized_topo_ptr);
void nvmeibt_topology_convert_header_le_be(const struct nvmeibt_topology_serialized_topo_header *src_ptr, struct nvmeibt_topology_serialized_topo_header *dst_ptr);
void nvmeibt_topology_convert_follower_header_le_be(struct nvmeibt_active_topo_header *header_ptr);
void nvmeibt_convert_topo_le_be(void *src_topo, void* dst_topo, BOOL is_src_the_usable, struct nvmeibt_Str *JSON_output);
bool nvmeibt_topology_is_global_bin_topo(const void *topo_ptr);
bool nvmeibt_topology_is_active_bin_topo(const void *topo_ptr);

// Expose: Builder for constructing an ACT_TOPO wire buffer. To be exeternally used by test environment
// Usage: init -> append (repeated) -> to_wire. Caller fills each appended segment's fields via the returned pointer.
 struct nvmeibt_act_topo_builder {
	char											*buf;
	int												buf_size;
	int												n_segs;
	int												topo_len;
	struct nvmeibt_serialized_seg_active_topo		*next_seg;
};
void nvmeibt_act_topo_builder_init(struct nvmeibt_act_topo_builder *b, char *output_buf, int buf_size);		// Non null output buffer.
struct nvmeibt_serialized_seg_active_topo *nvmeibt_act_topo_builder_append(struct nvmeibt_act_topo_builder *b);	// Reserve the next segment slot. or NULL if no space
void nvmeibt_act_topo_builder_to_wire(const struct nvmeibt_act_topo_builder *b);							// Serialize built topology (buffer of size b->topo_len)

#endif
