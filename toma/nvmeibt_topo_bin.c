/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_common.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_praid.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_topo_bin.h"

#define TOPOLOGY_MAX_PRAIDS_TO_PRINT				50
#define TOPOLOGY_MAX_SEGS_TO_PRINT					300
#define SHORT_UUID_STR_LEN							9

	/* -------------------- SEGMENT --------------------*/

void nvmeibt_disk_segment_print_leader_wire_topo(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_serialized_seg_leader_topo *seg_wire_topo_ptr)
{
	struct nvmeibt_urn_uuid							seg_uuid;
	struct nvmeibt_serialized_seg_leader_topo		serialized;

	nvmeibt_disk_segment_convert_topo_le_be(seg_wire_topo_ptr, &serialized);
	seg_uuid = nvmeibt_union_uuid_to_urn_uuid(&(serialized.uuid));

	(*printf_fn)(printf_ctx, "seg=%s ver=(%x,%x) dirty=%s inits=(d=%s s=%s t=%s) ow1=%x is_s=%d flg=%x\n",
		         seg_uuid.str, serialized.praid_version_major, serialized.praid_version_minor,
		         dirty_bits_state_str(serialized.dirty_bits_state),
		         mem_tbl_init_mode_str(serialized.dirty_bits_init_mode), mem_tbl_init_mode_str(serialized.stale_locks_init_mode),
				 mem_tbl_init_mode_str(serialized.txid_init_mode), serialized.owner_idx,
		         serialized.is_registrants_synchronizer, serialized.leader_seg_flags);
}

void nvmeibt_seg_serialized_active_topo(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_serialized_seg_active_topo *serialized_seg_topo_ptr)
{
	struct nvmeibt_urn_uuid					seg_uuid;

	seg_uuid = nvmeibt_union_uuid_to_urn_uuid(&(serialized_seg_topo_ptr->uuid));

	(*printf_fn)(printf_ctx, "id=%s ver=(%x,%x) ser_v=%x dirty=%s inits=(d=%s s=%s) flg=%x\n",
		         seg_uuid.str, serialized_seg_topo_ptr->active_praid_version_major, serialized_seg_topo_ptr->active_praid_version_minor,
		         serialized_seg_topo_ptr->active_seg_ser_ver, dirty_bits_state_str(serialized_seg_topo_ptr->dirty_bits_state),
		         mem_tbl_init_mode_str(serialized_seg_topo_ptr->dirty_bits_init_mode), mem_tbl_init_mode_str(serialized_seg_topo_ptr->stale_locks_init_mode),
				 *(int *)&(serialized_seg_topo_ptr->active_seg_flags));
}

void nvmeibt_disk_segment_convert_topo_le_be(struct nvmeibt_serialized_seg_leader_topo *src_ptr, struct nvmeibt_serialized_seg_leader_topo *dst_ptr)
{
	nvmeibt_strlcpy(dst_ptr->eyecatcher, src_ptr->eyecatcher, sizeof(dst_ptr->eyecatcher));
	COPY_SWAP_UUID_STR_FIELD(src_ptr, dst_ptr, uuid);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, praid_version_major);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, praid_version_minor);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, leader_seg_flags_int);
	COPY_SWAP32_STR_BITFIELD(src_ptr, dst_ptr, dirty_bits_state);
	COPY_SWAP32_STR_BITFIELD(src_ptr, dst_ptr, dirty_bits_init_mode);
	COPY_SWAP32_STR_BITFIELD(src_ptr, dst_ptr, stale_locks_init_mode);
	COPY_SWAP32_STR_BITFIELD(src_ptr, dst_ptr, txid_init_mode);
	COPY_SWAP8_STR_FIELD(src_ptr, dst_ptr, is_registrants_synchronizer);
	COPY_SWAP8_STR_FIELD(src_ptr, dst_ptr, seg_idx);
	COPY_SWAP8_STR_FIELD(src_ptr, dst_ptr, owner_idx);
	COPY_SWAP8_STR_FIELD(src_ptr, dst_ptr, secondary_owner_idx);
	dst_ptr->res_1 = 0;
	dst_ptr->res_2 = 0;
}

void nvmeibt_disk_segment_convert_active_bin_topo_le_be(struct nvmeibt_serialized_seg_active_topo *seg_ptr)
{
	SWAP_UUID_STR_FIELD(seg_ptr, uuid);
	SWAP32_STR_FIELD(seg_ptr,active_praid_version_major);
	SWAP32_STR_FIELD(seg_ptr,active_praid_version_minor);
	SWAP64_STR_FIELD(seg_ptr,active_seg_ser_ver);
	SWAP32_STR_FIELD(seg_ptr,active_seg_flags_int);
	SWAP32_STR_FIELD(seg_ptr,dirty_bits_state);
	SWAP32_STR_FIELD(seg_ptr,dirty_bits_init_mode);
	SWAP32_STR_FIELD(seg_ptr,stale_locks_init_mode);
}

	/* -------------------- PRAID --------------------*/

void nvmeibt_praid_print_leader_wire_topo(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_praid_serialized_topo *praid_wire_topo)
{
	struct nvmeibt_urn_uuid					praid_uuid;
	struct nvmeibt_praid_serialized_topo 	serialized;

	nvmeibt_praid_convert_topo_le_be(praid_wire_topo, &serialized);
	praid_uuid = nvmeibt_union_uuid_to_urn_uuid(&(serialized.uuid));
	(*printf_fn)(printf_ctx, "praid=%s ver=(%x,%x) sync_cmd=%s is_sync=%d act=%d n_seg=%d\n",
				 praid_uuid.str, serialized.praid_version_major, serialized.praid_version_minor,
				 praid_registrants_sync_cmd_str(serialized.registrants_sync_cmd), serialized.leader_did_all_segs_sync_registrants,
				 serialized.is_activated, serialized.segs_num);
}

void nvmeibt_praid_convert_topo_le_be(struct nvmeibt_praid_serialized_topo *src_ptr, struct nvmeibt_praid_serialized_topo *dst_ptr)
{
	nvmeibt_strlcpy(dst_ptr->eyecatcher, src_ptr->eyecatcher, sizeof(dst_ptr->eyecatcher));
	COPY_SWAP_UUID_STR_FIELD(src_ptr, dst_ptr, uuid);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, praid_version_major);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, praid_version_minor);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, leader_did_all_segs_sync_registrants);
	COPY_SWAP32_STR_BITFIELD(src_ptr, dst_ptr, registrants_sync_cmd);
	COPY_SWAP8_STR_FIELD(src_ptr, dst_ptr, is_activated);
	COPY_SWAP8_STR_FIELD(src_ptr, dst_ptr, segs_num);
	dst_ptr->res_1 = 0;
	dst_ptr->res_2 = 0;
	dst_ptr->res_3 = 0;
}

	/* -------------------- TOPO --------------------*/

void nvmeibt_topology_print_topo_header(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_topology_serialized_topo_header *header_ptr)
{
	(*printf_fn)(printf_ctx, "ver=0x%x len=%u n_praid=%d\n",
				 header_ptr->sw_ver, header_ptr->topo_len, header_ptr->praids_num);
}

void nvmeibt_topology_print(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, const struct nvmeibt_Buf *wire_topo_buf, bool check_max_len)
{
	struct nvmeibt_Buf								serialized_topo_buf = {(size_t)0, (void *)0};
	struct nvmeibt_topology_serialized_topo_header	serialized_header;
	struct nvmeibt_topology_serialized_topo_header	*wire_header_ptr;
	struct nvmeibt_praid_serialized_topo			*praid_wire_topo_ptr;
	struct nvmeibt_serialized_seg_leader_topo		*seg_wire_topo_ptr;
	int												i, j;

	if (wire_topo_buf->buf_len == 0) {
		(*printf_fn)(printf_ctx, "Topology is empty\n");
		goto out;
	}
	if (nvmeibt_topology_is_global_bin_topo(wire_topo_buf->data_buf)) {
		wire_header_ptr = (struct nvmeibt_topology_serialized_topo_header *)(wire_topo_buf->data_buf);
		nvmeibt_topology_convert_header_le_be(wire_header_ptr, &serialized_header);
		nvmeibt_topology_print_topo_header(printf_fn, printf_ctx, &serialized_header);
		if (check_max_len && (serialized_header.praids_num > TOPOLOGY_MAX_PRAIDS_TO_PRINT)) {
			(*printf_fn)(printf_ctx, "Topology is too long\n");
			goto out;
		}
		(*printf_fn)(printf_ctx, "PRAIDS:\n");
		praid_wire_topo_ptr = (struct nvmeibt_praid_serialized_topo *)(wire_header_ptr + 1);
		for (i = 0; i < serialized_header.praids_num; i++) {
			nvmeibt_praid_print_leader_wire_topo(printf_fn, printf_ctx, praid_wire_topo_ptr);
			seg_wire_topo_ptr = (struct nvmeibt_serialized_seg_leader_topo *)(praid_wire_topo_ptr + 1);

			for (j = 0; j < LE_SWAP8(praid_wire_topo_ptr->segs_num); j++) {
				(*printf_fn)(printf_ctx, "   ");
				nvmeibt_disk_segment_print_leader_wire_topo(printf_fn, printf_ctx, seg_wire_topo_ptr);
				seg_wire_topo_ptr++;
			}

			praid_wire_topo_ptr = (struct nvmeibt_praid_serialized_topo *)seg_wire_topo_ptr;	// Next praid. right after this praid's last seg
		}
	}
	else { // text, just print it
		(*printf_fn)(printf_ctx, wire_topo_buf->data_buf);
	}
out:
	NNVMEIBT_BM_FREE(4cvbwk4, serialized_topo_buf.data_buf);
}

void nvmeibt_topology_follower_print(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, const void *serialized_topo_ptr)
{
	struct nvmeibt_active_topo_header				*header_ptr;
	struct nvmeibt_serialized_seg_active_topo		*seg_topo_ptr;
	int												i;

	if (!serialized_topo_ptr) {
		(*printf_fn)(printf_ctx, "Topology is empty\n");
		return;
	}

	if (nvmeibt_topology_is_active_bin_topo(serialized_topo_ptr)) {

		header_ptr = (struct nvmeibt_active_topo_header *)serialized_topo_ptr;
		(*printf_fn)(printf_ctx, "ver=%x len=%u n_seg=%d\n", header_ptr->sw_ver, header_ptr->topo_len, header_ptr->segs_num);

		if (header_ptr->segs_num > TOPOLOGY_MAX_SEGS_TO_PRINT) {
			(*printf_fn)(printf_ctx, "Topology is too long\n");
			return;
		}

		seg_topo_ptr = (struct nvmeibt_serialized_seg_active_topo *)(header_ptr + 1);
		for (i = 0; i < header_ptr->segs_num; i++) {
			(*printf_fn)(printf_ctx, "   ");
			nvmeibt_seg_serialized_active_topo(printf_fn, printf_ctx, seg_topo_ptr);
			seg_topo_ptr++;
		}
	}
	else { // text, just print it
		(*printf_fn)(printf_ctx, serialized_topo_ptr);
	}
}

void nvmeibt_topology_convert_header_le_be(struct nvmeibt_topology_serialized_topo_header *src_ptr, struct nvmeibt_topology_serialized_topo_header *dst_ptr)
{
	memcpy(dst_ptr, src_ptr, NVMEIBT_TOPOLOGY_BIN_NAME_LEN);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, sw_ver);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, topo_len);
	COPY_SWAP32_STR_FIELD(src_ptr, dst_ptr, praids_num);
	dst_ptr->res_1 = 0;
	dst_ptr->res_2 = 0;
}

void serialize_topo_hdr_to_JSON(struct nvmeibt_topology_serialized_topo_header *hdr, struct nvmeibt_Str *JSON_output)
{
	if (!JSON_output) {
		goto out;
	}
	nvmeibt_Str_sprintf(JSON_output, "\n\"FULL_TOPO\":{\n\"topo_hdr\":{\"sw_ver\":%u, \"topo_len\":%u, \"praids_num\":%d},\n"
						"\"praids\":[%s",
						hdr->sw_ver, hdr->topo_len, hdr->praids_num, (hdr->praids_num <= 0 ? "]" : ""));
out:;
}

void nvmeibt_topology_convert_follower_header_le_be(struct nvmeibt_active_topo_header *header_ptr)
{
	SWAP32_STR_FIELD(header_ptr, sw_ver);
	SWAP32_STR_FIELD(header_ptr, topo_len);
	SWAP32_STR_FIELD(header_ptr, segs_num);
}

void serialize_topo_hdr_to_persist_and_wire(struct nvmeibt_topology_serialized_topo_header *hdr, struct nvmeibt_Str *JSON_output)
{
	N_Ef(crgvsdfygfwegbhy, "@PTR @PTR", hdr, JSON_output);
}

void serialize_praid_topo_to_JSON(struct nvmeibt_praid_serialized_topo *t, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_urn_uuid			urn_uuid;

	if (!JSON_output) {
		goto out;
	}
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(t->uuid));
	nvmeibt_Str_sprintf(JSON_output,
						"\n\t{\"eyecatcher\":\"%.4s\", \"uuid\":\"%s\", \"praid_version_major\":%d, \"praid_version_minor\":%d, "
						"\"leader_did_all_segs_sync_registrants\":%d, \"registrants_sync_cmd\":\"%s\", \"is_activated\":%d, \"segs_num\":%d, \"segments\":[",
						t->eyecatcher, urn_uuid.str, t->praid_version_major, t->praid_version_minor, t->leader_did_all_segs_sync_registrants,
						praid_registrants_sync_cmd_str(t->registrants_sync_cmd), t->is_activated, t->segs_num);
out:;
}

void serialize_praid_topo_to_persist_and_wire(struct nvmeibt_praid_serialized_topo *t, struct nvmeibt_Str *JSON_output)
{
	N_Ef(794k3k0dbiod, "@PTR @PTR", t, JSON_output);
}

void serialize_seg_topo_to_JSON(struct nvmeibt_serialized_seg_leader_topo *t, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_urn_uuid			urn_uuid;

	if (!JSON_output) {
		goto out;
	}
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(t->uuid));
	nvmeibt_Str_sprintf(JSON_output,
						"\n\t\t{\"eyecatcher\":\"%.4s\", \"uuid\":\"%s\", \"praid_version_major\":%d, \"praid_version_minor\":%d, "
						"\"has_ram_survived\":%d, \"is_newly_added_seg\":%d, \"is_owner_ram_recoverable_stable\":%d, \"is_drive_write_error\":%d, "
						"\"dirty_bits_state\":\"%s\", "
						"\"dirty_bits_init_mode\":\"%s\", \"stale_locks_init_mode\":\"%s\", \"txid_init_mode\":\"%s\", "
						"\"seg_idx\":%d, \"owner_idx\":%d, \"secondary_owner_idx\":%d, \"is_registrants_synchronizer\":%d}",
						t->eyecatcher, urn_uuid.str, t->praid_version_major, t->praid_version_minor,
						t->leader_seg_flags.has_ram_survived, t->leader_seg_flags.is_newly_added_seg, t->leader_seg_flags.is_owner_ram_recoverable_stable, t->leader_seg_flags.is_drive_write_error,
						dirty_bits_state_str(t->dirty_bits_state),
						mem_tbl_init_mode_str(t->dirty_bits_init_mode), mem_tbl_init_mode_str(t->stale_locks_init_mode), mem_tbl_init_mode_str(t->txid_init_mode),
						t->seg_idx, t->owner_idx, t->secondary_owner_idx, t->is_registrants_synchronizer);
out:;
}

void serialize_seg_topo_to_persist_and_wire(struct nvmeibt_serialized_seg_leader_topo *t, struct nvmeibt_Str *JSON_output)
{
	N_Ef(8aj3mcnsuakl, "@PTR @PTR", t, JSON_output);
}

void nvmeibt_convert_topo_le_be(void *src_topo, void* dst_topo, BOOL is_src_the_usable, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_topology_serialized_topo_header	*src_header_ptr;
	struct nvmeibt_topology_serialized_topo_header	*dst_header_ptr;
	struct nvmeibt_praid_serialized_topo			*src_praid_topo_ptr;
	struct nvmeibt_praid_serialized_topo			*dst_praid_topo_ptr;
	struct nvmeibt_serialized_seg_leader_topo		*src_seg_topo_ptr;
	struct nvmeibt_serialized_seg_leader_topo		*dst_seg_topo_ptr;
	int												i, j, praids_num, segs_num;

	src_header_ptr = (struct nvmeibt_topology_serialized_topo_header *)src_topo;
	dst_header_ptr = (struct nvmeibt_topology_serialized_topo_header *)dst_topo;
	nvmeibt_topology_convert_header_le_be(src_header_ptr, dst_header_ptr);
	if (is_src_the_usable) {
		serialize_topo_hdr_to_persist_and_wire(src_header_ptr, JSON_output);
	} else {
		serialize_topo_hdr_to_JSON(dst_header_ptr, JSON_output);
	}

	src_praid_topo_ptr = (struct nvmeibt_praid_serialized_topo *)(src_header_ptr + 1);
	dst_praid_topo_ptr = (struct nvmeibt_praid_serialized_topo *)(dst_header_ptr + 1);
	praids_num = is_src_the_usable ? src_header_ptr->praids_num : dst_header_ptr->praids_num;
	
	for (i = 0; i < praids_num; i++) {
		nvmeibt_praid_convert_topo_le_be(src_praid_topo_ptr, dst_praid_topo_ptr);
		if (is_src_the_usable) {
			serialize_praid_topo_to_persist_and_wire(src_praid_topo_ptr, JSON_output);
		} else {
			serialize_praid_topo_to_JSON(dst_praid_topo_ptr, JSON_output);
		}
		src_seg_topo_ptr = (struct nvmeibt_serialized_seg_leader_topo *)(src_praid_topo_ptr + 1);
		dst_seg_topo_ptr = (struct nvmeibt_serialized_seg_leader_topo *)(dst_praid_topo_ptr + 1);
		segs_num = is_src_the_usable ? src_praid_topo_ptr->segs_num : dst_praid_topo_ptr->segs_num;

		for (j = 0; j < segs_num; j++) {
			nvmeibt_disk_segment_convert_topo_le_be(src_seg_topo_ptr, dst_seg_topo_ptr);
			if (is_src_the_usable) {
				serialize_seg_topo_to_persist_and_wire(src_seg_topo_ptr, JSON_output);
			} else {
				serialize_seg_topo_to_JSON(dst_seg_topo_ptr, JSON_output);
			}
			serialize_end_of_array_obj_to_JSON(j, segs_num, 1, JSON_output);
			src_seg_topo_ptr++;
			dst_seg_topo_ptr++;
		}
		serialize_end_of_array_obj_to_JSON(i, praids_num, 1, JSON_output);
		src_praid_topo_ptr = (struct nvmeibt_praid_serialized_topo *)src_seg_topo_ptr;
		dst_praid_topo_ptr = (struct nvmeibt_praid_serialized_topo *)dst_seg_topo_ptr;
	}
}

bool nvmeibt_topology_is_global_bin_topo(const void *topo_ptr)
{
	return !memcmp(topo_ptr, nvmeibt_topology_binary_topo_header, NVMEIBT_TOPOLOGY_BIN_NAME_LEN);
}

bool nvmeibt_topology_is_active_bin_topo(const void *topo_ptr)
{
	return !memcmp(topo_ptr, nvmeibt_topology_binary_active_topo_header, NVMEIBT_TOPOLOGY_BIN_NAME_LEN);
}

