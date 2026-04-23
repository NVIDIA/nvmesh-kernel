/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
/**
 * wire_buf_test.c - Wire buffer unit tests
 * Tests persist_and_wire_buf operations:
 * - Per-section merge via TEST_raft_merge_data_to_section
 * - Follower realloc_and_upd orchestration via TEST_realloc_and_upd_follower_persist_and_wire_bufs
 * - Incremental selection logic via TEST_compute_is_configs_incremental
 */

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_persistency_info.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_topo_bin.h"
#include "nvmeibt_disk_segment.h"
#include "unitest/00_framework/toma_test_framework.h"
#include "unitest/00_framework/toma_test_helpers.h"
#include "wire_buf_test.h"
#include <stdlib.h>
#include <string.h>

#define TOPO_HDR_NAME	"BIN_TOPO"

static void make_test_uuid(union nvmeib_uuid *uuid, int id)
{
	uuid->ll[0] = (unsigned long long)(0xAAAA000000000000ULL + (unsigned)id);
	uuid->ll[1] = (unsigned long long)(0xBBBB000000000000ULL + (unsigned)id);
}

//
// Initialize a test_praid_spec AND register it in the praids hash.
// Combines make_test_uuid + field init + TEST_add_praid_to_hash.
//
static void setup_test_praid(struct test_praid_spec *spec, int uuid_id,
							 int segs_num, int64_t topo_idx_updated,
							 int praid_version_major, int praid_version_minor)
{
	make_test_uuid(&spec->uuid, uuid_id);
	spec->segs_num = segs_num;
	spec->topo_idx_updated = topo_idx_updated;
	spec->praid_version_major = praid_version_major;
	spec->praid_version_minor = praid_version_minor;
	TEST_add_praid_to_hash(&spec->uuid, topo_idx_updated, praid_version_major, praid_version_minor);
}

static void fill_tlv(struct nvmeibt_wire_type_len_value *tlv, int8_t type,
		int data_len, int64_t idx)
{
	int64_t	neg_one = -1LL;

	memset(tlv, 0, sizeof(*tlv));
	tlv->tlv_type = LE_SWAP8(type);
	tlv->tlv_len = LE_SWAP32(data_len);
	tlv->tlv_idx = LE_SWAP64(idx);
	tlv->seq_no = LE_SWAP64(neg_one);
	tlv->tlv_crc = 0;
}

//
// Build a topo wire buffer with N praids, each with M segments.
// tlv_type should be TLV_TYPE_TOPO_COMPLETE or TLV_TYPE_TOPO_INCREMENTAL.
// Returns data length on success, -1 on error.
//
static int craft_topo_buf(char *buf, int buf_size,
		struct nvmeibt_wire_type_len_value *tlv_out,
		int8_t tlv_type, int64_t topo_idx,
		int n_praids, const struct test_praid_spec *praids)
{
	struct nvmeibt_topology_serialized_topo_header	*header;
	char											*ptr;
	int												data_len;

	data_len = (int)sizeof(struct nvmeibt_topology_serialized_topo_header);
	for (int i = 0; i < n_praids; i++) {
		data_len += (int)sizeof(struct nvmeibt_praid_serialized_topo) +
				praids[i].segs_num * (int)sizeof(struct nvmeibt_serialized_seg_leader_topo);
	}
	if (data_len > buf_size) {
		return -1;
	}

	memset(buf, 0, data_len);
	header = (struct nvmeibt_topology_serialized_topo_header *)buf;
	memcpy(header->topo_name, TOPO_HDR_NAME, NVMEIBT_TOPOLOGY_BIN_NAME_LEN);
	header->encoding_ver = LE_SWAP32(TOMA_ENCODING_VER);
	header->topo_len = LE_SWAP32(data_len);
	header->praids_num = LE_SWAP32(n_praids);

	ptr = buf + sizeof(*header);
	for (int i = 0; i < n_praids; i++) {
		struct nvmeibt_praid_serialized_topo	host_praid;
		struct nvmeibt_praid_serialized_topo	*p = (struct nvmeibt_praid_serialized_topo *)ptr;

		// Build praid in host byte order, then convert to wire format using
		// the production conversion function for correct UUID/field encoding.
		memset(&host_praid, 0, sizeof(host_praid));
		memcpy(host_praid.eyecatcher, "PRAD", 4);
		host_praid.uuid = praids[i].uuid;
		host_praid.segs_num = (int8_t)praids[i].segs_num;
		nvmeibt_praid_serialized_set_topo_idx_updated(&host_praid, praids[i].topo_idx_updated);
		host_praid.praid_version_major = praids[i].praid_version_major;
		host_praid.praid_version_minor = praids[i].praid_version_minor;
		host_praid.is_activated = 1;
		nvmeibt_praid_convert_topo_le_be(&host_praid, p);
		ptr += sizeof(*p);

		for (int j = 0; j < praids[i].segs_num; j++) {
			struct nvmeibt_serialized_seg_leader_topo	host_seg;
			struct nvmeibt_serialized_seg_leader_topo	*seg = (struct nvmeibt_serialized_seg_leader_topo *)ptr;

			memset(&host_seg, 0, sizeof(host_seg));
			memcpy(host_seg.eyecatcher, "DSEG", 4);
			make_test_uuid(&host_seg.uuid, i * 100 + j);
			host_seg.seg_idx = (int8_t)j;
			host_seg.praid_version_major = praids[i].praid_version_major;
			host_seg.praid_version_minor = praids[i].praid_version_minor;
			nvmeibt_disk_segment_convert_topo_le_be(&host_seg, seg);
			ptr += sizeof(*seg);
		}
	}

	fill_tlv(tlv_out, tlv_type, data_len, topo_idx);
	return data_len;
}

//
// Build a generic (non-topo) section buffer with recognizable fill pattern.
// Returns data length written.
//
static int craft_section_buf(char *buf, int buf_size,
		struct nvmeibt_wire_type_len_value *tlv_out,
		int8_t tlv_type, int64_t idx, int data_len, unsigned char fill_byte)
{
	if (data_len > buf_size) {
		return -1;
	}
	memset(buf, fill_byte, data_len);
	fill_tlv(tlv_out, tlv_type, data_len, idx);
	return data_len;
}

//
// Find a praid by UUID in a topo data buffer. Returns pointer to the wire praid, or NULL.
//
static struct nvmeibt_praid_serialized_topo *find_praid_in_topo(char *topo_data,
		const union nvmeib_uuid *target_uuid)
{
	struct nvmeibt_topology_serialized_topo_header	header;
	struct nvmeibt_praid_serialized_topo			*p;

	nvmeibt_topology_convert_header_le_be((struct nvmeibt_topology_serialized_topo_header *)topo_data, &header);
	p = (struct nvmeibt_praid_serialized_topo *)(topo_data + sizeof(struct nvmeibt_topology_serialized_topo_header));

	for (int i = 0; i < header.praids_num; i++) {
		struct nvmeibt_praid_serialized_topo host_praid;

		nvmeibt_praid_convert_topo_le_be(p, &host_praid);
		if (ARE_UUID_EQ(&host_praid.uuid, target_uuid)) {
			return p;
		}
		p = (struct nvmeibt_praid_serialized_topo *)((char *)p +
				sizeof(*p) + nvmeibt_praid_wire_get_n_segs(p) * (int)sizeof(struct nvmeibt_serialized_seg_leader_topo));
	}
	return NULL;
}

//
// Get praid count from a topo data buffer (host byte order).
//
static int get_topo_praid_count(const char *topo_data)
{
	struct nvmeibt_topology_serialized_topo_header	header;

	nvmeibt_topology_convert_header_le_be((struct nvmeibt_topology_serialized_topo_header *)topo_data, &header);
	return header.praids_num;
}

/*******************    Serialized struct helpers    ***************************/

DEFINE_TEST(topo_idx_updated_getter_setter)
{
	struct nvmeibt_praid_serialized_topo	p;
	int64_t									val;
	int										rv = -1;

	(void)_ctx;

	// Zero
	memset(&p, 0, sizeof(p));
	nvmeibt_praid_serialized_set_topo_idx_updated(&p, 0);
	val = nvmeibt_praid_serialized_get_topo_idx_updated(&p);
	TEST_ASSERT_EQ(val, 0);

	// Small positive
	nvmeibt_praid_serialized_set_topo_idx_updated(&p, 42);
	val = nvmeibt_praid_serialized_get_topo_idx_updated(&p);
	TEST_ASSERT_EQ(val, 42);

	// -1 (uninitialized sentinel)
	nvmeibt_praid_serialized_set_topo_idx_updated(&p, -1);
	val = nvmeibt_praid_serialized_get_topo_idx_updated(&p);
	TEST_ASSERT_EQ(val, -1);

	// Negative sentinels used in incremental merge
	nvmeibt_praid_serialized_set_topo_idx_updated(&p, -2);
	val = nvmeibt_praid_serialized_get_topo_idx_updated(&p);
	TEST_ASSERT_EQ(val, -2);

	nvmeibt_praid_serialized_set_topo_idx_updated(&p, -3);
	val = nvmeibt_praid_serialized_get_topo_idx_updated(&p);
	TEST_ASSERT_EQ(val, -3);

	// Packed value: raft_term=1 in upper 32, counter=100 in lower 32
	nvmeibt_praid_serialized_set_topo_idx_updated(&p, (1LL << 32) | 100);
	val = nvmeibt_praid_serialized_get_topo_idx_updated(&p);
	TEST_ASSERT_EQ(val, (1LL << 32) | 100);

	// Large raft_term in upper 32 bits
	nvmeibt_praid_serialized_set_topo_idx_updated(&p, (0x7FFFFFFFLL << 32) | 0xFFFFFFFF);
	val = nvmeibt_praid_serialized_get_topo_idx_updated(&p);
	TEST_ASSERT_EQ(val, (0x7FFFFFFFLL << 32) | 0xFFFFFFFF);

	// Verify byte-swap round-trip through convert function
	{
		struct nvmeibt_praid_serialized_topo	host, wire, back;
		int64_t								test_val = (5LL << 32) | 999;

		memset(&host, 0, sizeof(host));
		nvmeibt_praid_serialized_set_topo_idx_updated(&host, test_val);
		nvmeibt_praid_convert_topo_le_be(&host, &wire);
		nvmeibt_praid_convert_topo_le_be(&wire, &back);
		val = nvmeibt_praid_serialized_get_topo_idx_updated(&back);
		TEST_ASSERT_EQ(val, test_val);
	}

	rv = 0;
out:
	return rv;
}

/*************************    Complete merges    *******************************/

// -------------------------- Complete: topo ---------------------------

DEFINE_TEST(complete_topo_replaces)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], new_praids[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len = 0;
	int									upd_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 0;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;

	make_test_uuid(&new_praids[0].uuid, 2);
	new_praids[0].segs_num = 0;
	new_praids[0].topo_idx_updated = 20;
	new_praids[0].praid_version_major = 2;
	new_praids[0].praid_version_minor = 0;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 1LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_COMPLETE, 2LL, 1, new_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, upd_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 2LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->upd_buf, (size_t)upd_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), upd_len);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(complete_topo_same_idx_keeps_old)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				praids[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	make_test_uuid(&praids[0].uuid, 1);
	praids[0].segs_num = 0;
	praids[0].topo_idx_updated = 10;
	praids[0].praid_version_major = 1;
	praids[0].praid_version_minor = 0;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 5LL, 1, praids);
	TEST_ASSERT_TRUE(old_len > 0);

	// upd has same idx as old and zero data => keep old
	fill_tlv(&upd_tlv, TLV_TYPE_TOPO_COMPLETE, 0, 5LL);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 5LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->old_buf, (size_t)old_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), old_len);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(complete_topo_empty_old)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				praids[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									upd_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	make_test_uuid(&praids[0].uuid, 1);
	praids[0].segs_num = 0;
	praids[0].topo_idx_updated = 10;
	praids[0].praid_version_major = 1;
	praids[0].praid_version_minor = 0;

	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_COMPLETE, 1LL, 1, praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	// Empty but valid old: TLV header with zero data length
	fill_tlv(&old_tlv, TLV_TYPE_TOPO_COMPLETE, 0, 0LL);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, upd_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 1LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->upd_buf, (size_t)upd_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), 0);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), upd_len);
	rv = 0;
out:
	return rv;
}

// ----------------------- Complete: topo config -----------------------

DEFINE_TEST(complete_topo_config_replaces)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	old_len = craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 1LL, 64, 0xAA);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 2LL, 128, 0xBB);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, upd_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 2LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->upd_buf, (size_t)upd_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), upd_len);
	rv = 0;
out:
	return rv;
}

// -------------------- Complete: kafka mgmt config --------------------

DEFINE_TEST(complete_kafka_config_replaces)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	old_len = craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 1LL, 96, 0xCC);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 2LL, 200, 0xDD);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, upd_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 2LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->upd_buf, (size_t)upd_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), upd_len);
	rv = 0;
out:
	return rv;
}

// ---------------------- Complete: raft members -----------------------

DEFINE_TEST(complete_raft_members_replaces)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	old_len = craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 1LL, 48, 0x11);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 2LL, 80, 0x22);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, upd_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_RAFT_MEMBERS_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 2LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->upd_buf, (size_t)upd_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), upd_len);
	rv = 0;
out:
	return rv;
}

// ---------------------- Complete: multi-section ----------------------

DEFINE_TEST(complete_all_sections_mixed)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	char								old_data[64], upd_data[128];
	int									rv = -1;
	int									merge_size;

	// topo_config: update changes it
	memset(old_data, 0xAA, 64);
	memset(upd_data, 0xBB, 128);
	fill_tlv(&old_tlv, TLV_TYPE_TOPO_CONFIG_COMPLETE, 64, 1LL);
	fill_tlv(&upd_tlv, TLV_TYPE_TOPO_CONFIG_COMPLETE, 128, 2LL);
	memcpy(ctx->old_buf, old_data, 64);
	memcpy(ctx->upd_buf, upd_data, 128);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, 128);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), 128);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 2LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, upd_data, 128);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), 64);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), 128);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), 128);

	// kafka_config: update has different idx => replaces old
	memset(old_data, 0xCC, 48);
	memset(upd_data, 0xDD, 32);
	fill_tlv(&old_tlv, TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 48, 5LL);
	fill_tlv(&upd_tlv, TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 32, 6LL);
	memcpy(ctx->old_buf, old_data, 48);
	memcpy(ctx->upd_buf, upd_data, 32);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, 32);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), 32);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 6LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, upd_data, 32);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), 48);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), 32);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), 32);

	// raft_members: same idx + zero upd_len => keep old
	memset(old_data, 0x55, 40);
	fill_tlv(&old_tlv, TLV_TYPE_RAFT_MEMBERS_COMPLETE, 40, 3LL);
	fill_tlv(&upd_tlv, TLV_TYPE_RAFT_MEMBERS_COMPLETE, 0, 3LL);
	memcpy(ctx->old_buf, old_data, 40);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, 40);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_RAFT_MEMBERS_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), 40);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 3LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, old_data, 40);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), 40);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), 40);
	rv = 0;
out:
	return rv;
}

/************************    Incremental merges    *****************************/

// ------------------------ Incremental: topo --------------------------

DEFINE_TEST(incremental_topo_empty_keeps_old)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				praids[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	TEST_init_praids_hash();
	make_test_uuid(&praids[0].uuid, 1);
	praids[0].segs_num = 0;
	praids[0].topo_idx_updated = 10;
	praids[0].praid_version_major = 1;
	praids[0].praid_version_minor = 0;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, praids);
	TEST_ASSERT_TRUE(old_len > 0);

	fill_tlv(&upd_tlv, TLV_TYPE_TOPO_INCREMENTAL, 0, 10LL);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), old_len);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->old_buf, (size_t)old_len);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 1);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), old_len);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_single_praid_updated)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 0;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;

	incr_praids[0] = old_praids[0];
	incr_praids[0].topo_idx_updated = 20;
	incr_praids[0].praid_version_major = 2;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 20LL, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);

	// Verify merged TLV metadata -- result must be COMPLETE, len must match
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);

	// Verify pointer advancement
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 1);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 20);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_single_praid_not_updated)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	setup_test_praid(&old_praids[0], 1, 0, 20, 2, 0);

	incr_praids[0] = old_praids[0];
	incr_praids[0].topo_idx_updated = 10;
	incr_praids[0].praid_version_major = 1;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 20LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 20LL, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 1);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 20);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_multi_praid_partial_update)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[3], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	for (int i = 0; i < 3; i++)
		setup_test_praid(&old_praids[i], i + 1, 0, 10, 1, 0);

	incr_praids[0] = old_praids[1];
	incr_praids[0].topo_idx_updated = 30;
	incr_praids[0].praid_version_major = 3;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 3, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 30LL, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 3);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 10);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[1].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 3);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 30);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[2].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 10);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_multi_praid_all_updated)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[3], incr_praids[3];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	for (int i = 0; i < 3; i++) {
		make_test_uuid(&old_praids[i].uuid, i + 1);
		old_praids[i].segs_num = 0;
		old_praids[i].topo_idx_updated = 10;
		old_praids[i].praid_version_major = 1;
		old_praids[i].praid_version_minor = 0;

		incr_praids[i] = old_praids[i];
		incr_praids[i].topo_idx_updated = 50;
		incr_praids[i].praid_version_major = 5;
	}

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 3, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 50LL, 3, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 3);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	for (int i = 0; i < 3; i++) {
		found = find_praid_in_topo(ctx->dst_buf, &old_praids[i].uuid);
		TEST_ASSERT_NOT_NULL(found);
		nvmeibt_praid_convert_topo_le_be(found, &host_praid);
		TEST_ASSERT_EQ(host_praid.praid_version_major, 5);
		TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 50);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_praid_with_segments)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[2], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;
	int									expected_size;

	TEST_init_praids_hash();

	// Hash has committed state with 0 segs (minimal test praid).
	// The kept praid will be re-serialized from hash, not copied from old wire buf.
	// Wire old has 2 segs per praid for the incremental wire buffer parsing.
	setup_test_praid(&old_praids[0], 1, 2, 10, 1, 0);
	setup_test_praid(&old_praids[1], 2, 2, 10, 1, 0);

	incr_praids[0] = old_praids[0];
	incr_praids[0].topo_idx_updated = 20;
	incr_praids[0].praid_version_major = 2;
	incr_praids[0].segs_num = 2;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 2, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 20LL, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	// Updated praid (from incremental wire) has 2 segs, kept praid (from hash) has 0 segs
	expected_size = (int)sizeof(struct nvmeibt_topology_serialized_topo_header) +
			((int)sizeof(struct nvmeibt_praid_serialized_topo) +
				2 * (int)sizeof(struct nvmeibt_serialized_seg_leader_topo)) +
			(int)sizeof(struct nvmeibt_praid_serialized_topo);
	TEST_ASSERT_EQ(merge_size, expected_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 2);

	// Updated praid: from incremental wire, has 2 segs
	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 20);
	TEST_ASSERT_EQ((int)nvmeibt_praid_wire_get_n_segs(found), 2);

	// Kept praid: re-serialized from hash committed state (0 segs in test helper)
	found = find_praid_in_topo(ctx->dst_buf, &old_praids[1].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 10);
	TEST_ASSERT_EQ((int)nvmeibt_praid_wire_get_n_segs(found), 0);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_praid_seg_count_changes)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;
	int									expected_size;

	TEST_init_praids_hash();
	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 2;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;

	incr_praids[0] = old_praids[0];
	incr_praids[0].segs_num = 3;
	incr_praids[0].topo_idx_updated = 20;
	incr_praids[0].praid_version_major = 2;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 20LL, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 1);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	expected_size = (int)sizeof(struct nvmeibt_topology_serialized_topo_header) +
			(int)sizeof(struct nvmeibt_praid_serialized_topo) +
			3 * (int)sizeof(struct nvmeibt_serialized_seg_leader_topo);
	TEST_ASSERT_EQ(merge_size, expected_size);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ((int)nvmeibt_praid_wire_get_n_segs(found), 3);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 20);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_extra_uuid_ignored)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	setup_test_praid(&old_praids[0], 1, 0, 10, 1, 0);

	make_test_uuid(&incr_praids[0].uuid, 99);
	incr_praids[0].segs_num = 0;
	incr_praids[0].topo_idx_updated = 50;
	incr_praids[0].praid_version_major = 9;
	incr_praids[0].praid_version_minor = 0;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 50LL, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	// Hash-based merge: new UUID from incremental is accepted (leader added it),
	// old UUID from hash is appended in phase 2. Result: 2 praids.
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 2);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	// New praid from incremental is included
	found = find_praid_in_topo(ctx->dst_buf, &incr_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 9);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 50);

	// Old praid from hash is also present (appended in phase 2)
	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 10);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_ordering_differs)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[3], incr_praids[2];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	for (int i = 0; i < 3; i++)
		setup_test_praid(&old_praids[i], i + 1, 0, 10, 1, 0);

	// Incremental in reverse order: [C, A] instead of [A, B, C]
	incr_praids[0] = old_praids[2];	// C
	incr_praids[0].topo_idx_updated = 40;
	incr_praids[0].praid_version_major = 4;

	incr_praids[1] = old_praids[0];	// A
	incr_praids[1].topo_idx_updated = 40;
	incr_praids[1].praid_version_major = 4;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 3, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 40LL, 2, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 3);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	// A and C updated, B unchanged
	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 4);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 40);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[1].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 10);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[2].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 4);
	TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 40);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_same_idx_keeps_old)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	setup_test_praid(&old_praids[0], 1, 0, 10, 1, 0);

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);

	// Incremental with 1 praid but same topo_idx => keep old
	incr_praids[0] = old_praids[0];
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 10LL, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 1);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	{
		struct nvmeibt_praid_serialized_topo	*found;
		struct nvmeibt_praid_serialized_topo	host_praid;

		found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
		TEST_ASSERT_NOT_NULL(found);
		nvmeibt_praid_convert_topo_le_be(found, &host_praid);
		TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
		TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 10);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_large_praid_count)
{
	const int							n_total_praids = 50;
	const int							n_updated_praids = 10;
	const int							update_stride = n_total_praids / n_updated_praids;
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[WIRE_BUF_TEST_MAX_PRAIDS];
	struct test_praid_spec				incr_praids[WIRE_BUF_TEST_MAX_PRAIDS];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;
	int									i;

	TEST_init_praids_hash();
	for (i = 0; i < n_total_praids; i++)
		setup_test_praid(&old_praids[i], i + 1, 0, 100, 1, 0);

	for (i = 0; i < n_updated_praids; i++) {
		incr_praids[i] = old_praids[i * update_stride];
		incr_praids[i].topo_idx_updated = 200;
		incr_praids[i].praid_version_major = 7;
	}

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 100LL, n_total_praids, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 200LL, n_updated_praids, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), merge_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), n_total_praids);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	for (i = 0; i < n_total_praids; i++) {
		int		expected_ver = ((i % update_stride) == 0) ? 7 : 1;
		int64_t	expected_idx = ((i % update_stride) == 0) ? 200 : 100;

		found = find_praid_in_topo(ctx->dst_buf, &old_praids[i].uuid);
		TEST_ASSERT_NOT_NULL(found);
		nvmeibt_praid_convert_topo_le_be(found, &host_praid);
		TEST_ASSERT_EQ(host_praid.praid_version_major, expected_ver);
		TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), expected_idx);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_unknown_type_fails)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 1LL, 64, 0xAA);
	// Use a bogus TLV type that doesn't match any known incremental type
	craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			(int8_t)127, 2LL, 32, 0xBB);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, &upd_ptr);
	TEST_ASSERT_EQ(merge_size, -1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_size_only_no_dst)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	struct test_praid_spec				praids[1];
	char								*old_ptr, *upd_ptr;
	int									old_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	make_test_uuid(&praids[0].uuid, 1);
	praids[0].segs_num = 0;
	praids[0].topo_idx_updated = 10;
	praids[0].praid_version_major = 1;
	praids[0].praid_version_minor = 0;

	old_len = craft_topo_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, praids);
	TEST_ASSERT_TRUE(old_len > 0);

	fill_tlv(&upd_tlv, TLV_TYPE_TOPO_INCREMENTAL, 0, 10LL);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, old_len);
	rv = 0;
out:
	return rv;
}

// ---- Incremental: empty upd keeps old for topo_config, kafka, raft ------

DEFINE_TEST(incremental_topo_config_empty_keeps_old)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	old_len = craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 5LL, 64, 0xAA);
	TEST_ASSERT_TRUE(old_len > 0);

	fill_tlv(&upd_tlv, TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 0, 10LL);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 10LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->old_buf, (size_t)old_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), old_len);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_kafka_config_empty_keeps_old)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	old_len = craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 3LL, 96, 0xCC);
	TEST_ASSERT_TRUE(old_len > 0);

	fill_tlv(&upd_tlv, TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 0, 7LL);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 7LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->old_buf, (size_t)old_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), old_len);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_raft_members_empty_keeps_old)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len = 0;
	int									rv = -1;
	int									merge_size = -1;

	old_len = craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 2LL, 48, 0x11);
	TEST_ASSERT_TRUE(old_len > 0);

	fill_tlv(&upd_tlv, TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 0, 5LL);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_EQ(merge_size, old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_RAFT_MEMBERS_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst_tlv), old_len);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst_tlv), 5LL);
	TEST_ASSERT_MEM_EQ(ctx->dst_buf, ctx->old_buf, (size_t)old_len);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), old_len);
	rv = 0;
out:
	return rv;
}

/*************** Incremental raft_members merge (non-empty) ********************/

struct test_raft_member_spec {
	union nvmeib_uuid	uuid;
	char				hostname[NVMEIB_HOST_NAME_LEN];
	int64_t				seq_no_updated;
	int64_t				kafka_offset;
};

static void init_raft_member_spec(struct test_raft_member_spec *spec, int id,
								  int64_t seq_no, int64_t kafka_offset)
{
	memset(spec, 0, sizeof(*spec));
	make_test_uuid(&spec->uuid, id);
	snprintf(spec->hostname, sizeof(spec->hostname), "node-%d", id);
	spec->seq_no_updated = seq_no;
	spec->kafka_offset = kafka_offset;
}

//
// Build a raft_members wire buffer: header + N members + eyecatcher.
// Members are serialized in wire byte order via nvmeibt_raft_member_conf_convert_le_be.
//
static int craft_raft_members_buf(char *buf, int buf_size,
		struct nvmeibt_wire_type_len_value *tlv_out,
		int8_t tlv_type, int64_t idx,
		int n_members, const struct test_raft_member_spec *members)
{
	char	*ptr;
	int		data_len;

	data_len = (int)(sizeof(int) * 2) +
			n_members * (int)sizeof(struct mm_raft_member_conf) +
			(int)sizeof(EYECATCHER_CNF_END);
	if (data_len > buf_size)
		return -1;

	memset(buf, 0, data_len);
	ptr = buf;

	// Header: n_raft_members + filler
	*(int *)ptr = LE_SWAP32(n_members);
	ptr += sizeof(int) * 2;

	// Members — convert via aligned temp then memcpy (wire buffer may not be 16-byte aligned)
	for (int i = 0; i < n_members; i++) {
		struct mm_raft_member_conf host_conf __attribute__((aligned(16)));
		struct mm_raft_member_conf wire_tmp __attribute__((aligned(16)));

		memset(&host_conf, 0, sizeof(host_conf));
		memcpy(host_conf.eyecatcher, "MMB", 4);
		host_conf.uuid = members[i].uuid;
		host_conf.kafka_offset = members[i].kafka_offset;
		host_conf.raft_members_seq_no_updated = members[i].seq_no_updated;
		nvmeibt_strlcpy(host_conf.hostname, members[i].hostname, sizeof(host_conf.hostname));
		nvmeibt_raft_member_conf_convert_le_be(&wire_tmp, &host_conf);
		memcpy(ptr, &wire_tmp, sizeof(wire_tmp));
		ptr += sizeof(struct mm_raft_member_conf);
	}

	// Eyecatcher
	nvmeibt_strlcpy(ptr, EYECATCHER_CNF_END, sizeof(EYECATCHER_CNF_END));
	ptr += sizeof(EYECATCHER_CNF_END);

	fill_tlv(tlv_out, tlv_type, data_len, idx);
	return data_len;
}

//
// Register a test_raft_member_spec in the hash (combines spec init + hash add).
//
static void setup_test_raft_member(struct test_raft_member_spec *spec, int id,
								   int64_t seq_no, int64_t kafka_offset)
{
	init_raft_member_spec(spec, id, seq_no, kafka_offset);
	TEST_add_raft_member_to_hash(&spec->uuid, spec->hostname, seq_no, kafka_offset);
}

DEFINE_TEST(incremental_raft_members_partial_update)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_raft_member_spec		old_members[3], incr_members[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_raft_members_hash();
	for (int i = 0; i < 3; i++)
		setup_test_raft_member(&old_members[i], i + 1, 10, 100 + i);

	// Incremental: only member[1] updated with higher seq_no
	init_raft_member_spec(&incr_members[0], 2, 20, 200);

	old_len = craft_raft_members_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 10LL, 3, old_members);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_raft_members_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 20LL, 1, incr_members);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_RAFT_MEMBERS_COMPLETE);

	// Result: 1 from incremental + 2 non-visited from hash = 3 total
	{
		int *n_out = (int *)ctx->dst_buf;
		TEST_ASSERT_EQ(LE_SWAP32(*n_out), 3);
	}

	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_raft_members_new_member_accepted)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_raft_member_spec		old_members[2], incr_members[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_raft_members_hash();
	for (int i = 0; i < 2; i++)
		setup_test_raft_member(&old_members[i], i + 1, 10, 100 + i);

	// Incremental: new member (id=99) not in hash
	init_raft_member_spec(&incr_members[0], 99, 30, 300);

	old_len = craft_raft_members_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 10LL, 2, old_members);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_raft_members_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 30LL, 1, incr_members);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_RAFT_MEMBERS_COMPLETE);

	// Result: 1 new + 2 non-visited from hash = 3 total
	{
		int *n_out = (int *)ctx->dst_buf;
		TEST_ASSERT_EQ(LE_SWAP32(*n_out), 3);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_raft_members_old_seq_keeps_hash)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_raft_member_spec		old_members[1], incr_members[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;
	struct mm_raft_member_conf			out_member __attribute__((aligned(16)));
	struct mm_raft_member_conf			host_member __attribute__((aligned(16)));

	TEST_init_raft_members_hash();
	setup_test_raft_member(&old_members[0], 1, 20, 100);

	// Incremental: same member but OLDER seq_no => keep hash version
	init_raft_member_spec(&incr_members[0], 1, 5, 50);

	old_len = craft_raft_members_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 20LL, 1, old_members);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_raft_members_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 20LL, 1, incr_members);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);

	// 1 member total (from hash, since incremental is older)
	{
		int *n_out = (int *)ctx->dst_buf;
		TEST_ASSERT_EQ(LE_SWAP32(*n_out), 1);
	}

	// Verify the member has seq_no=20 (from hash), not 5 (from incremental)
	memcpy(&out_member, ctx->dst_buf + sizeof(int) * 2, sizeof(out_member));
	nvmeibt_raft_member_conf_convert_le_be(&host_member, &out_member);
	TEST_ASSERT_EQ(host_member.raft_members_seq_no_updated, 20);
	TEST_ASSERT_EQ(host_member.kafka_offset, 100);
	rv = 0;
out:
	return rv;
}

/*
 * Scenario: 2 raft members exist in hash, leader removes one.
 * Incremental carries only 1 member. Non-visited hash member is appended.
 * Result: 2 members (raft members are not deleted via incremental merge).
 */
DEFINE_TEST(incremental_raft_members_removed_member_kept)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_raft_member_spec		old_members[2], incr_members[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_raft_members_hash();
	for (int i = 0; i < 2; i++)
		setup_test_raft_member(&old_members[i], i + 1, 10, 100 + i);

	// Incremental: only member[0] with updated seq_no — member[1] absent
	init_raft_member_spec(&incr_members[0], 1, 20, 200);

	old_len = craft_raft_members_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 10LL, 2, old_members);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_raft_members_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 20LL, 1, incr_members);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_RAFT_MEMBERS_COMPLETE);

	// Non-visited member[1] appended from hash => 2 total
	{
		int *n_out = (int *)ctx->dst_buf;
		TEST_ASSERT_EQ(LE_SWAP32(*n_out), 2);
	}
	rv = 0;
out:
	return rv;
}

/************ Incremental kafka_mgmt_config merge (non-empty) *****************/

struct test_vol_spec {
	union nvmeib_uuid	uuid;
	uint32_t			version;
	int64_t				kafka_offset_or_idx;
};

//
// Build a minimal kafka_mgmt_config wire buffer with N volumes, each having
// 1 chunk with 1 praid with 0 segments. Uses production conversion functions.
// Returns data length on success, -1 on error.
//
static int craft_kafka_mgmt_config_buf(char *buf, int buf_size,
		struct nvmeibt_wire_type_len_value *tlv_out,
		int8_t tlv_type, int64_t idx,
		int n_vols, const struct test_vol_spec *vols)
{
	struct mm_mgmt_conf			mgmt = {0};
	struct mm_vol_conf			vol = {0};
	struct mm_chunk_conf		chunk = {0};
	struct mm_praid_conf		praid = {0};
	char						*ptr;
	int							data_len;
	int							per_vol_size;

	per_vol_size = (int)(nvmeibt_packed_vol_config_size() +
				nvmeibt_packed_chunk_config_size() +
				nvmeibt_packed_praid_config_size());
	data_len = (int)nvmeibt_packed_mm_mgmt_config_size() +
			n_vols * per_vol_size +
			(int)sizeof(EYECATCHER_CNF_END);
	if (data_len > buf_size)
		return -1;

	memset(buf, 0, data_len);
	ptr = buf;

	// Header
	memcpy(mgmt.eyecatcher, "CNF", 4);
	mgmt.structVersion = 2067;
	mgmt.protocolVersion = 1;
	mgmt.configurationVersion = 1;
	mgmt.idx = idx;
	mgmt.num_vols = n_vols;
	ptr += nvmeibt_mm_mgmt_convert_to_wire_via_aligned_tmp(ptr, &mgmt);

	// Volumes
	for (int i = 0; i < n_vols; i++) {
		memset(&vol, 0, sizeof(vol));
		memcpy(vol.eyecatcher, "VOL", 4);
		vol.uuid = vols[i].uuid;
		vol.version = vols[i].version;
		vol.kafka_offset_or_idx = vols[i].kafka_offset_or_idx;
		vol.num_chunks = 1;
		vol.blockSize = 4096;
		ptr += nvmeibt_vol_convert_to_wire_via_aligned_tmp(ptr, &vol);

		memset(&chunk, 0, sizeof(chunk));
		memcpy(chunk.eyecatcher, "CHK", 4);
		make_test_uuid(&chunk.uuid, 1000 + i);
		chunk.num_praids = 1;
		ptr += nvmeibt_chunk_convert_to_wire_via_aligned_tmp(ptr, &chunk);

		memset(&praid, 0, sizeof(praid));
		memcpy(praid.eyecatcher, "PRD", 4);
		make_test_uuid(&praid.uuid, 2000 + i);
		praid.num_segments = 0;
		praid.version = vols[i].version;
		ptr += nvmeibt_praid_convert_to_wire_via_aligned_tmp(ptr, &praid);
	}

	// Eyecatcher
	nvmeibt_strlcpy(ptr, EYECATCHER_CNF_END, sizeof(EYECATCHER_CNF_END));

	fill_tlv(tlv_out, tlv_type, data_len, idx);
	return data_len;
}

//
// Serialize a single volume's wire data for use as a blkdev's pre-serialized buffer.
// Returns the wire data length.
//
static int serialize_vol_wire_data(char *buf, int buf_size __attribute__((unused)), const struct test_vol_spec *vol_spec, int vol_idx)
{
	struct mm_vol_conf		vol = {0};
	struct mm_chunk_conf	chunk = {0};
	struct mm_praid_conf	praid = {0};
	char					*ptr = buf;

	memcpy(vol.eyecatcher, "VOL", 4);
	vol.uuid = vol_spec->uuid;
	vol.version = vol_spec->version;
	vol.kafka_offset_or_idx = vol_spec->kafka_offset_or_idx;
	vol.num_chunks = 1;
	vol.blockSize = 4096;
	ptr += nvmeibt_vol_convert_to_wire_via_aligned_tmp(ptr, &vol);

	memcpy(chunk.eyecatcher, "CHK", 4);
	make_test_uuid(&chunk.uuid, 1000 + vol_idx);
	chunk.num_praids = 1;
	ptr += nvmeibt_chunk_convert_to_wire_via_aligned_tmp(ptr, &chunk);

	memcpy(praid.eyecatcher, "PRD", 4);
	make_test_uuid(&praid.uuid, 2000 + vol_idx);
	praid.num_segments = 0;
	praid.version = vol_spec->version;
	ptr += nvmeibt_praid_convert_to_wire_via_aligned_tmp(ptr, &praid);

	return (int)(ptr - buf);
}

//
// Build a topo_config wire buffer. Same layout as kafka_mgmt_config but
// with topo_config_idx_updated set on praids (used for merge comparison).
//
static int craft_topo_config_buf(char *buf, int buf_size,
		struct nvmeibt_wire_type_len_value *tlv_out,
		int8_t tlv_type, int64_t idx,
		int n_vols, const struct test_vol_spec *vols)
{
	struct mm_mgmt_conf			mgmt = {0};
	struct mm_vol_conf			vol = {0};
	struct mm_chunk_conf		chunk = {0};
	struct mm_praid_conf		praid = {0};
	char						*ptr;
	int							data_len;
	int							per_vol_size;

	per_vol_size = (int)(nvmeibt_packed_vol_config_size() +
				nvmeibt_packed_chunk_config_size() +
				nvmeibt_packed_praid_config_size());
	data_len = (int)nvmeibt_packed_mm_mgmt_config_size() +
			n_vols * per_vol_size +
			(int)sizeof(EYECATCHER_CNF_END);
	if (data_len > buf_size)
		return -1;

	memset(buf, 0, data_len);
	ptr = buf;

	memcpy(mgmt.eyecatcher, "CNF", 4);
	mgmt.structVersion = 2067;
	mgmt.protocolVersion = 1;
	mgmt.configurationVersion = 1;
	mgmt.idx = idx;
	mgmt.num_vols = n_vols;
	ptr += nvmeibt_mm_mgmt_convert_to_wire_via_aligned_tmp(ptr, &mgmt);

	for (int i = 0; i < n_vols; i++) {
		memset(&vol, 0, sizeof(vol));
		memcpy(vol.eyecatcher, "VOL", 4);
		vol.uuid = vols[i].uuid;
		vol.version = vols[i].version;
		vol.kafka_offset_or_idx = vols[i].kafka_offset_or_idx;
		vol.num_chunks = 1;
		vol.blockSize = 4096;
		ptr += nvmeibt_vol_convert_to_wire_via_aligned_tmp(ptr, &vol);

		memset(&chunk, 0, sizeof(chunk));
		memcpy(chunk.eyecatcher, "CHK", 4);
		make_test_uuid(&chunk.uuid, 1000 + i);
		chunk.num_praids = 1;
		ptr += nvmeibt_chunk_convert_to_wire_via_aligned_tmp(ptr, &chunk);

		memset(&praid, 0, sizeof(praid));
		memcpy(praid.eyecatcher, "PRD", 4);
		make_test_uuid(&praid.uuid, 2000 + i);
		praid.num_segments = 0;
		praid.version = vols[i].version;
		praid.topo_config_idx_updated = vols[i].kafka_offset_or_idx;
		ptr += nvmeibt_praid_convert_to_wire_via_aligned_tmp(ptr, &praid);
	}

	nvmeibt_strlcpy(ptr, EYECATCHER_CNF_END, sizeof(EYECATCHER_CNF_END));
	fill_tlv(tlv_out, tlv_type, data_len, idx);
	return data_len;
}

DEFINE_TEST(incremental_kafka_config_partial_update)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_vol_spec				old_vols[2], incr_vols[1];
	char								wire_data[1024];
	int									wire_len;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_blkdevs_hash();

	// Set up 2 volumes in hash
	for (int i = 0; i < 2; i++) {
		make_test_uuid(&old_vols[i].uuid, i + 1);
		old_vols[i].version = 10;
		old_vols[i].kafka_offset_or_idx = 100 + i;
		wire_len = serialize_vol_wire_data(wire_data, (int)sizeof(wire_data), &old_vols[i], i);
		TEST_add_blkdev_to_hash(&old_vols[i].uuid, 10, wire_data, wire_len, false);
	}

	// Incremental: only vol[0] updated with higher version
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 20;
	incr_vols[0].kafka_offset_or_idx = 200;

	old_len = craft_kafka_mgmt_config_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 100LL, 2, old_vols);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_kafka_mgmt_config_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 200LL, 1, incr_vols);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	// Decode merged output and verify content
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, false, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 2);
		// vol[0] from incremental: version 20
		TEST_ASSERT_EQ((int)merged->volumes[0].version, 20);
		// vol[1] from hash: version 10
		TEST_ASSERT_EQ((int)merged->volumes[1].version, 10);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_kafka_config_old_version_keeps_hash)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_vol_spec				old_vols[1], incr_vols[1];
	char								wire_data[1024];
	int									wire_len;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_blkdevs_hash();

	make_test_uuid(&old_vols[0].uuid, 1);
	old_vols[0].version = 20;
	old_vols[0].kafka_offset_or_idx = 200;
	wire_len = serialize_vol_wire_data(wire_data, (int)sizeof(wire_data), &old_vols[0], 0);
	TEST_add_blkdev_to_hash(&old_vols[0].uuid, 20, wire_data, wire_len, false);

	// Incremental: same vol but OLDER version => skip from incremental, use hash
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 5;
	incr_vols[0].kafka_offset_or_idx = 50;

	old_len = craft_kafka_mgmt_config_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 200LL, 1, old_vols);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_kafka_mgmt_config_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 200LL, 1, incr_vols);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);

	// Decode merged output: vol should have version 20 (from hash), not 5 (from incremental)
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, false, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].version, 20);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

/*
 * Scenario: 2 volumes exist, 1 is being deleted. Leader serializes incremental
 * with only 1 vol. Follower merge skips being-deleted blkdev from hash.
 */
DEFINE_TEST(incremental_kafka_config_vol_deleted)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_vol_spec				old_vols[2], incr_vols[1];
	char								wire_data[1024];
	int									wire_len;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_blkdevs_hash();

	// Set up 2 volumes: vol[0] active with wire data, vol[1] being deleted
	for (int i = 0; i < 2; i++) {
		make_test_uuid(&old_vols[i].uuid, i + 1);
		old_vols[i].version = 10;
		old_vols[i].kafka_offset_or_idx = 100 + i;
	}
	wire_len = serialize_vol_wire_data(wire_data, (int)sizeof(wire_data), &old_vols[0], 0);
	TEST_add_blkdev_to_hash(&old_vols[0].uuid, 10, wire_data, wire_len, false);
	TEST_add_blkdev_to_hash(&old_vols[1].uuid, 10, NULL, 0, true);

	// Old: complete with 2 volumes
	old_len = craft_kafka_mgmt_config_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 100LL, 2, old_vols);
	TEST_ASSERT_TRUE(old_len > 0);

	// Incremental: only 1 volume (leader skipped being-deleted vol)
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 20;
	incr_vols[0].kafka_offset_or_idx = 200;
	upd_len = craft_kafka_mgmt_config_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 200LL, 1, incr_vols);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);

	// Merged: 1 vol (active), being-deleted vol excluded
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, false, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].version, 20);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

/*
 * Scenario: 1 volume exists, a new volume is added. Leader serializes
 * incremental with 2 volumes. Follower merge produces 2 volumes.
 */
DEFINE_TEST(incremental_kafka_config_vol_added)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_vol_spec				old_vols[1], incr_vols[2];
	char								wire_data[1024];
	int									wire_len;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_blkdevs_hash();

	// Set up 1 volume in hash
	make_test_uuid(&old_vols[0].uuid, 1);
	old_vols[0].version = 10;
	old_vols[0].kafka_offset_or_idx = 100;
	wire_len = serialize_vol_wire_data(wire_data, (int)sizeof(wire_data), &old_vols[0], 0);
	TEST_add_blkdev_to_hash(&old_vols[0].uuid, 10, wire_data, wire_len, false);

	// Old: complete with 1 volume
	old_len = craft_kafka_mgmt_config_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 100LL, 1, old_vols);
	TEST_ASSERT_TRUE(old_len > 0);

	// Incremental: 2 volumes (existing + new)
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 20;
	incr_vols[0].kafka_offset_or_idx = 200;
	make_test_uuid(&incr_vols[1].uuid, 2);
	incr_vols[1].version = 20;
	incr_vols[1].kafka_offset_or_idx = 200;

	upd_len = craft_kafka_mgmt_config_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 200LL, 2, incr_vols);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);

	// Merged: 2 vols — vol[0] updated from incremental, vol[1] new from incremental
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, false, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 2);
		TEST_ASSERT_EQ((int)merged->volumes[0].version, 20);
		TEST_ASSERT_EQ((int)merged->volumes[1].version, 20);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

/************* Incremental topo_config merge (non-empty) **********************/

DEFINE_TEST(incremental_topo_config_partial_praid_update)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_vol_spec				old_vols[1], incr_vols[1];
	union nvmeib_uuid					chunk_uuid, praid_uuid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	TEST_init_chunks_hash();

	// Set up 1 volume with 1 chunk with 1 praid in hash
	make_test_uuid(&old_vols[0].uuid, 1);
	old_vols[0].version = 10;
	old_vols[0].kafka_offset_or_idx = 100;

	make_test_uuid(&praid_uuid, 2000);
	TEST_add_praid_to_hash(&praid_uuid, 100, 10, 0);  // topo_config_idx = 100

	make_test_uuid(&chunk_uuid, 1000);
	TEST_add_chunk_to_hash(&chunk_uuid, 1, &praid_uuid);

	// Incremental: same volume but praid has higher topo_config_idx
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 20;
	incr_vols[0].kafka_offset_or_idx = 200;  // topo_config_idx for the praid

	old_len = craft_topo_config_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 100LL, 1, old_vols);
	TEST_ASSERT_TRUE(old_len > 0);
	upd_len = craft_topo_config_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 200LL, 1, incr_vols);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	// Decode merged output and verify praid was updated
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, true, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].chunks[0].num_praids, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].chunks[0].praids[0].version, 20);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

struct test_tc_praid_spec {
	union nvmeib_uuid	uuid;
	int64_t				topo_config_idx;
	uint32_t			version;
};

//
// Build a topo_config wire buffer with 1 vol, 1 chunk, N praids.
// Each praid has its own uuid and topo_config_idx_updated.
//
static int craft_topo_config_multi_praid_buf(char *buf, int buf_size,
		struct nvmeibt_wire_type_len_value *tlv_out,
		int8_t tlv_type, int64_t idx,
		const union nvmeib_uuid *vol_uuid, uint32_t vol_version,
		const union nvmeib_uuid *chunk_uuid,
		int n_praids, const struct test_tc_praid_spec *praids)
{
	struct mm_mgmt_conf		mgmt = {0};
	struct mm_vol_conf		vol = {0};
	struct mm_chunk_conf	chunk = {0};
	struct mm_praid_conf	praid = {0};
	char					*ptr;
	int						data_len;

	data_len = (int)(nvmeibt_packed_mm_mgmt_config_size() +
			nvmeibt_packed_vol_config_size() +
			nvmeibt_packed_chunk_config_size() +
			n_praids * (int)nvmeibt_packed_praid_config_size() +
			sizeof(EYECATCHER_CNF_END));
	if (data_len > buf_size)
		return -1;

	memset(buf, 0, data_len);
	ptr = buf;

	memcpy(mgmt.eyecatcher, "CNF", 4);
	mgmt.structVersion = 2067;
	mgmt.protocolVersion = 1;
	mgmt.configurationVersion = 1;
	mgmt.idx = idx;
	mgmt.num_vols = 1;
	ptr += nvmeibt_mm_mgmt_convert_to_wire_via_aligned_tmp(ptr, &mgmt);

	memset(&vol, 0, sizeof(vol));
	memcpy(vol.eyecatcher, "VOL", 4);
	vol.uuid = *vol_uuid;
	vol.version = vol_version;
	vol.kafka_offset_or_idx = idx;
	vol.num_chunks = 1;
	vol.blockSize = 4096;
	ptr += nvmeibt_vol_convert_to_wire_via_aligned_tmp(ptr, &vol);

	memset(&chunk, 0, sizeof(chunk));
	memcpy(chunk.eyecatcher, "CHK", 4);
	chunk.uuid = *chunk_uuid;
	chunk.num_praids = (uint8_t)n_praids;
	ptr += nvmeibt_chunk_convert_to_wire_via_aligned_tmp(ptr, &chunk);

	for (int i = 0; i < n_praids; i++) {
		memset(&praid, 0, sizeof(praid));
		memcpy(praid.eyecatcher, "PRD", 4);
		praid.uuid = praids[i].uuid;
		praid.version = praids[i].version;
		praid.topo_config_idx_updated = praids[i].topo_config_idx;
		praid.num_segments = 0;
		ptr += nvmeibt_praid_convert_to_wire_via_aligned_tmp(ptr, &praid);
	}

	nvmeibt_strlcpy(ptr, EYECATCHER_CNF_END, sizeof(EYECATCHER_CNF_END));
	fill_tlv(tlv_out, tlv_type, data_len, idx);
	return data_len;
}

DEFINE_TEST(incremental_topo_config_mixed_keep_and_update)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	union nvmeib_uuid					vol_uuid, chunk_uuid;
	union nvmeib_uuid					praid_uuids[2];
	struct test_tc_praid_spec			old_praids[2], incr_praids[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	TEST_init_chunks_hash();

	make_test_uuid(&vol_uuid, 1);
	make_test_uuid(&chunk_uuid, 1000);

	// 2 praids in hash, both at topo_config_idx = 100
	for (int i = 0; i < 2; i++) {
		make_test_uuid(&praid_uuids[i], 2000 + i);
		TEST_add_praid_to_hash(&praid_uuids[i], 100, 10, 0);
		old_praids[i].uuid = praid_uuids[i];
		old_praids[i].topo_config_idx = 100;
		old_praids[i].version = 10;
	}
	TEST_add_chunk_to_hash(&chunk_uuid, 2, praid_uuids);

	// Old: complete with 2 praids
	old_len = craft_topo_config_multi_praid_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 100LL, &vol_uuid, 10, &chunk_uuid, 2, old_praids);
	TEST_ASSERT_TRUE(old_len > 0);

	// Incremental: only praid[0] updated with higher topo_config_idx.
	// praid[1] is NOT in the incremental — must be re-serialized from hash.
	incr_praids[0].uuid = praid_uuids[0];
	incr_praids[0].topo_config_idx = 200;
	incr_praids[0].version = 20;

	upd_len = craft_topo_config_multi_praid_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 200LL, &vol_uuid, 20, &chunk_uuid, 1, incr_praids);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	// Merged result should be larger than incremental (1 praid) because
	// the old praid[1] was appended from hash. Expected: header + vol + chunk + 2 praids + eyecatcher + padding.
	TEST_ASSERT_TRUE(merge_size > upd_len);

	// Decode merged output and verify both praids are present with correct versions
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, true, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].num_chunks, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].chunks[0].num_praids, 2);
		// praid[0] from incremental: version 20
		TEST_ASSERT_EQ((int)merged->volumes[0].chunks[0].praids[0].version, 20);
		// praid[1] from hash (re-serialized): version 10
		TEST_ASSERT_EQ((int)merged->volumes[0].chunks[0].praids[1].version, 10);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

/*
 * Scenario: 2 volumes exist, 1 is being deleted. Leader serializes incremental
 * with only 1 vol (skipping the being-deleted vol). Follower merge must handle
 * num_new_vols < n_old_vols (because being-deleted vols are excluded).
 */
DEFINE_TEST(incremental_topo_config_vol_deleted)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_vol_spec				old_vols[2], incr_vols[1];
	union nvmeib_uuid					chunk_uuids[2], praid_uuids[2];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	TEST_init_chunks_hash();
	TEST_init_blkdevs_hash();

	// Set up 2 volumes: vol[0] active, vol[1] being deleted
	for (int i = 0; i < 2; i++) {
		make_test_uuid(&old_vols[i].uuid, i + 1);
		old_vols[i].version = 10;
		old_vols[i].kafka_offset_or_idx = 100;

		make_test_uuid(&praid_uuids[i], 2000 + i);
		TEST_add_praid_to_hash(&praid_uuids[i], 100, 10, 0);

		make_test_uuid(&chunk_uuids[i], 1000 + i);
		TEST_add_chunk_to_hash(&chunk_uuids[i], 1, &praid_uuids[i]);
	}

	// vol[0] is active, vol[1] is being deleted
	TEST_add_blkdev_to_hash(&old_vols[0].uuid, 10, NULL, 0, false);
	TEST_add_blkdev_to_hash(&old_vols[1].uuid, 10, NULL, 0, true);

	// Old: complete with 2 volumes
	old_len = craft_topo_config_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 100LL, 2, old_vols);
	TEST_ASSERT_TRUE(old_len > 0);

	// Incremental: only 1 volume (leader skipped the being-deleted vol)
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 20;
	incr_vols[0].kafka_offset_or_idx = 200;
	upd_len = craft_topo_config_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 200LL, 1, incr_vols);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_CONFIG_COMPLETE);

	// Merged output should have 1 volume (the active one)
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, true, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].version, 20);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

/*
 * Scenario: 1 volume exists, a new volume is added. Leader serializes
 * incremental with 2 volumes. Follower merge must handle
 * num_new_vols > n_old_vols (new volume added).
 */
DEFINE_TEST(incremental_topo_config_vol_added)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_vol_spec				old_vols[1], incr_vols[2];
	union nvmeib_uuid					chunk_uuids[2], praid_uuids[2];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	TEST_init_praids_hash();
	TEST_init_chunks_hash();
	TEST_init_blkdevs_hash();

	// Set up 1 existing volume in hash
	make_test_uuid(&old_vols[0].uuid, 1);
	old_vols[0].version = 10;
	old_vols[0].kafka_offset_or_idx = 100;

	make_test_uuid(&praid_uuids[0], 2000);
	TEST_add_praid_to_hash(&praid_uuids[0], 100, 10, 0);

	make_test_uuid(&chunk_uuids[0], 1000);
	TEST_add_chunk_to_hash(&chunk_uuids[0], 1, &praid_uuids[0]);

	TEST_add_blkdev_to_hash(&old_vols[0].uuid, 10, NULL, 0, false);

	// Old: complete with 1 volume
	old_len = craft_topo_config_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 100LL, 1, old_vols);
	TEST_ASSERT_TRUE(old_len > 0);

	// Incremental: 2 volumes (existing vol + newly added vol)
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 20;
	incr_vols[0].kafka_offset_or_idx = 200;

	make_test_uuid(&incr_vols[1].uuid, 2);
	incr_vols[1].version = 20;
	incr_vols[1].kafka_offset_or_idx = 200;

	// Add chunk/praid for the new vol so merge can find them
	make_test_uuid(&praid_uuids[1], 2001);
	make_test_uuid(&chunk_uuids[1], 1001);

	upd_len = craft_topo_config_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 200LL, 2, incr_vols);
	TEST_ASSERT_TRUE(upd_len > 0);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;
	dst_ptr = ctx->dst_buf;

	merge_size = TEST_raft_merge_data_to_section(&dst_tlv, &old_tlv, &upd_tlv,
			&dst_ptr, &old_ptr, (const char **)&upd_ptr);
	TEST_ASSERT_TRUE(merge_size > 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst_tlv), TLV_TYPE_TOPO_CONFIG_COMPLETE);

	// Merged output should have 2 volumes
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(ctx->dst_buf, true, NULL);
		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 2);
		TEST_ASSERT_EQ((int)merged->volumes[0].version, 20);
		TEST_ASSERT_EQ((int)merged->volumes[1].version, 20);
		mm_conf_free_tree(merged);
	}
	rv = 0;
out:
	return rv;
}

/***** Leader incremental selection: deletion guard forces complete **********/

/*
 * Scenario: peer is within all incremental windows, then a volume deletion
 * sets last_delete_kafka_mgmt_config_offset ahead of the peer's kafka offset.
 * Expected: is_configs_and_raft_members_incremental == false (complete forced).
 */
DEFINE_TEST(deletion_guard_forces_complete_configs)
{
	int		rv = -1;
	int		result;

	(void)_ctx;

	/* Leader state: topo=100, topo_config=50, kafka=50, raft_members_seq_no=20 */
	/* Peer is within all windows (close to leader values) */
	/* No deletion yet => should be incremental */
	result = TEST_compute_is_configs_incremental(
		/* peer */    98, 49, 49, 19, 50,
		/* leader */  100, 50, 50, 20,
		/* deletes */ 0, 0);
	TEST_ASSERT_EQ(result, 1);

	/* Volume deletion: last_delete_kafka_mgmt_config_offset = 50 (current offset) */
	/* Peer's kafka offset (49) < deletion offset (50) => forced complete */
	result = TEST_compute_is_configs_incremental(
		/* peer */    98, 49, 49, 19, 50,
		/* leader */  100, 50, 50, 20,
		/* deletes */ 50, 0);
	TEST_ASSERT_EQ(result, 0);

	rv = 0;
out:
	return rv;
}

/*
 * Scenario: peer has caught up past the deletion offset.
 * Expected: is_configs_and_raft_members_incremental == true (incremental OK).
 */
DEFINE_TEST(deletion_guard_no_effect_when_peer_caught_up)
{
	int		rv = -1;
	int		result;

	(void)_ctx;

	/* Deletion happened at kafka offset 40, but peer is at 49 (past it) */
	/* Leader is at 50, peer is within window */
	result = TEST_compute_is_configs_incremental(
		/* peer */    98, 49, 49, 19, 50,
		/* leader */  100, 50, 50, 20,
		/* deletes */ 40, 0);
	TEST_ASSERT_EQ(result, 1);

	/* Same but with raft_members deletion guard too */
	result = TEST_compute_is_configs_incremental(
		/* peer */    98, 49, 49, 19, 50,
		/* leader */  100, 50, 50, 20,
		/* deletes */ 40, 40);
	TEST_ASSERT_EQ(result, 1);

	/* Peer's raft_members_kafka_offset (30) < last_delete (40) => forced complete */
	result = TEST_compute_is_configs_incremental(
		/* peer */    98, 49, 49, 19, 30,
		/* leader */  100, 50, 50, 20,
		/* deletes */ 40, 40);
	TEST_ASSERT_EQ(result, 0);

	rv = 0;
out:
	return rv;
}

DEFINE_TEST(selection_topo_window_boundary)
{
	int			rv = -1;
	int			result;
	int64_t		topo_window_start = 100 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_IDX;
	int64_t		config_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_CONFIG_IDX;
	int64_t		kafka_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_KAFKA_MGMT_CONFIG_OFFSET;
	int64_t		raft_members_seq_window_start = 20 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_RAFT_MEMBERS_SEQ_NO;

	(void)_ctx;

	result = TEST_compute_is_configs_incremental(
		topo_window_start - 1, config_window_start, kafka_window_start, raft_members_seq_window_start, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 0);

	result = TEST_compute_is_configs_incremental(
		topo_window_start, config_window_start, kafka_window_start, raft_members_seq_window_start, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(selection_topo_config_window_boundary)
{
	int			rv = -1;
	int			result;
	int64_t		topo_window_start = 100 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_IDX;
	int64_t		config_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_CONFIG_IDX;
	int64_t		kafka_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_KAFKA_MGMT_CONFIG_OFFSET;
	int64_t		raft_members_seq_window_start = 20 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_RAFT_MEMBERS_SEQ_NO;

	(void)_ctx;

	result = TEST_compute_is_configs_incremental(
		topo_window_start, config_window_start - 1, kafka_window_start, raft_members_seq_window_start, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 0);

	result = TEST_compute_is_configs_incremental(
		topo_window_start, config_window_start, kafka_window_start, raft_members_seq_window_start, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(selection_kafka_window_boundary)
{
	int			rv = -1;
	int			result;
	int64_t		topo_window_start = 100 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_IDX;
	int64_t		config_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_CONFIG_IDX;
	int64_t		kafka_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_KAFKA_MGMT_CONFIG_OFFSET;
	int64_t		raft_members_seq_window_start = 20 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_RAFT_MEMBERS_SEQ_NO;

	(void)_ctx;

	result = TEST_compute_is_configs_incremental(
		topo_window_start, config_window_start, kafka_window_start - 1, raft_members_seq_window_start, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 0);

	result = TEST_compute_is_configs_incremental(
		topo_window_start, config_window_start, kafka_window_start, raft_members_seq_window_start, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(selection_raft_members_seq_window_boundary)
{
	int			rv = -1;
	int			result;
	int64_t		topo_window_start = 100 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_IDX;
	int64_t		config_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_TOPO_CONFIG_IDX;
	int64_t		kafka_window_start = 50 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_KAFKA_MGMT_CONFIG_OFFSET;
	int64_t		raft_members_seq_window_start = 20 - NVMEIBT_INCREMENTAL_WINDOW_SIZE_RAFT_MEMBERS_SEQ_NO;

	(void)_ctx;

	result = TEST_compute_is_configs_incremental(
		topo_window_start, config_window_start, kafka_window_start, raft_members_seq_window_start - 1, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 0);

	result = TEST_compute_is_configs_incremental(
		topo_window_start, config_window_start, kafka_window_start, raft_members_seq_window_start, 50,
		100, 50, 50, 20,
		0, 0);
	TEST_ASSERT_EQ(result, 1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(selection_old_peer_version_forces_complete)
{
	int		rv = -1;
	int		is_incremental_allowed;

	(void)_ctx;

	is_incremental_allowed = TEST_is_configs_incremental_allowed_for_peer_sw_ver(
		TOMA_SW_VER_MIN_FOR_INCREMENTAL - 1,
		/* peer */    98, 49, 49, 19, 50,
		/* leader */  100, 50, 50, 20,
		/* deletes */ 0, 0);
	TEST_ASSERT_EQ(is_incremental_allowed, 0);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(selection_supported_peer_version_allows_incremental)
{
	int		rv = -1;
	int		is_incremental_allowed;

	(void)_ctx;

	is_incremental_allowed = TEST_is_configs_incremental_allowed_for_peer_sw_ver(
		TOMA_SW_VER_MIN_FOR_INCREMENTAL,
		/* peer */    98, 49, 49, 19, 50,
		/* leader */  100, 50, 50, 20,
		/* deletes */ 0, 0);
	TEST_ASSERT_EQ(is_incremental_allowed, 1);
	rv = 0;
out:
	return rv;
}

/* Forward declarations for helpers defined in Realloc section */
static struct nvmeibt_persist_and_wire_buf *build_test_buf(
	unsigned long long raft_term,
	char *topo_data, int topo_len, int64_t topo_idx,
	char *tc_data, int tc_len, int64_t tc_idx,
	char *kmc_data, int kmc_len, int64_t kmc_idx,
	char *rm_data, int rm_len, int64_t rm_idx);
static struct nvmeibt_persist_and_wire_buf *build_test_buf_ex(
	unsigned long long raft_term,
	bool is_topo_incremental, char *topo_data, int topo_len, int64_t topo_idx,
	bool is_topo_config_incremental, char *tc_data, int tc_len, int64_t tc_idx,
	bool is_kafka_mgmt_config_incremental, char *kmc_data, int kmc_len, int64_t kmc_idx,
	bool is_raft_members_incremental, char *rm_data, int rm_len, int64_t rm_idx, int64_t rm_seq_no);

/*****  compare_persist_and_wire_bufs_tlvs_excl_raft_ctx direct tests  *******/

DEFINE_TEST(compare_both_null_returns_equal)
{
	int		rv = -1;

	(void)_ctx;
	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(NULL, NULL), 0);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(compare_one_null_returns_topo_and_configs)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	buf = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(buf);

	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(buf, NULL), 2);
	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(NULL, buf), 2);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_cmp1, buf);
	return rv;
}

DEFINE_TEST(compare_equal_bufs_returns_equal)
{
	struct nvmeibt_persist_and_wire_buf		*b1 = NULL, *b2 = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	b1 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	b2 = build_test_buf(5, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(b1);
	TEST_ASSERT_NOT_NULL(b2);

	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(b1, b2), 0);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_cmp2a, b1);
	NNVMEIBT_TOMA_FREE(test_cmp2b, b2);
	return rv;
}

DEFINE_TEST(compare_topo_only_diff)
{
	struct nvmeibt_persist_and_wire_buf		*b1 = NULL, *b2 = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	b1 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	b2 = build_test_buf(1, topo, 64, 99, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(b1);
	TEST_ASSERT_NOT_NULL(b2);

	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(b1, b2), 1);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_cmp3a, b1);
	NNVMEIBT_TOMA_FREE(test_cmp3b, b2);
	return rv;
}

DEFINE_TEST(compare_topo_config_only_diff)
{
	struct nvmeibt_persist_and_wire_buf		*b1 = NULL, *b2 = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	b1 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	b2 = build_test_buf(1, topo, 64, 10, NULL, 0, 99, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(b1);
	TEST_ASSERT_NOT_NULL(b2);

	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(b1, b2), 2);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_cmp4a, b1);
	NNVMEIBT_TOMA_FREE(test_cmp4b, b2);
	return rv;
}

DEFINE_TEST(compare_kafka_config_only_diff)
{
	struct nvmeibt_persist_and_wire_buf		*b1 = NULL, *b2 = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	b1 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	b2 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 99, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(b1);
	TEST_ASSERT_NOT_NULL(b2);

	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(b1, b2), 2);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_cmp5a, b1);
	NNVMEIBT_TOMA_FREE(test_cmp5b, b2);
	return rv;
}

DEFINE_TEST(compare_raft_members_only_diff)
{
	struct nvmeibt_persist_and_wire_buf		*b1 = NULL, *b2 = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	b1 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	b2 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 99);
	TEST_ASSERT_NOT_NULL(b1);
	TEST_ASSERT_NOT_NULL(b2);

	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(b1, b2), 2);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_cmp6a, b1);
	NNVMEIBT_TOMA_FREE(test_cmp6b, b2);
	return rv;
}

DEFINE_TEST(compare_topo_and_config_diff)
{
	struct nvmeibt_persist_and_wire_buf		*b1 = NULL, *b2 = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	b1 = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	b2 = build_test_buf(1, topo, 64, 99, NULL, 0, 88, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(b1);
	TEST_ASSERT_NOT_NULL(b2);

	TEST_ASSERT_EQ(TEST_compare_persist_and_wire_bufs(b1, b2), 2);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_cmp7a, b1);
	NNVMEIBT_TOMA_FREE(test_cmp7b, b2);
	return rv;
}

/***********  CRC and length validation tests  **********/

DEFINE_TEST(crc_corrupted_topo_data_fails)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64], tc[32];
	char									*topo_data_out;
	int										data_len;
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	buf = build_test_buf(1, topo, 64, 10, tc, 32, 20, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(buf);
	data_len = 64 + 32;

	// Corrupt topo section data
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(buf, TLV_TYPE_TOPO_COMPLETE, &topo_data_out);
	TEST_ASSERT_NOT_NULL(topo_data_out);
	topo_data_out[0] ^= 0xFF;

	TEST_ASSERT_TRUE(!TEST_is_persist_and_wire_buf_crc_and_len_ok(buf, data_len));
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_crc2, buf);
	return rv;
}

DEFINE_TEST(crc_corrupted_raft_ctx_fails)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64];
	int										data_len;
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	buf = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(buf);
	data_len = 64;

	// Corrupt raft_ctx CRC field
	buf->raft_ctx.raft_ctx_crc ^= 0xDEADBEEF;

	TEST_ASSERT_TRUE(!TEST_is_persist_and_wire_buf_crc_and_len_ok(buf, data_len));
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_crc3, buf);
	return rv;
}

DEFINE_TEST(crc_length_mismatch_fails)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	buf = build_test_buf(1, topo, 64, 10, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	TEST_ASSERT_NOT_NULL(buf);

	// Pass wrong data_len (actual is 64, pass 128)
	TEST_ASSERT_TRUE(!TEST_is_persist_and_wire_buf_crc_and_len_ok(buf, 128));
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_crc4, buf);
	return rv;
}

DEFINE_TEST(generate_buf_crc_valid)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64], tc[32], kmc[16], rm[16];
	int										data_len;
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	buf = build_test_buf(1, topo, 64, 10, tc, 32, 20, kmc, 16, 30, rm, 16, 40);
	TEST_ASSERT_NOT_NULL(buf);
	data_len = 64 + 32 + 16 + 16;

	TEST_ASSERT_TRUE(TEST_is_persist_and_wire_buf_crc_and_len_ok(buf, data_len));
	persist_and_wire_buf_validate_len(buf);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_gen4, buf);
	return rv;
}

DEFINE_TEST(follower_merge_produces_valid_crc)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo_old[100], topo_upd[200];
	char									tc[50], kmc[30], rm[40];
	int										data_len;
	int										rv = -1;

	(void)_ctx;
	memset(topo_old, 0xAA, sizeof(topo_old));
	memset(topo_upd, 0xEE, sizeof(topo_upd));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	old = build_test_buf(1, topo_old, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	upd = build_test_buf(3, topo_upd, 200, 15, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_NOT_NULL(dst);

	data_len = nvmeibt_tlv_get_len(&dst->topo_ctx) +
			   nvmeibt_tlv_get_len(&dst->topo_config_ctx) +
			   nvmeibt_tlv_get_len(&dst->kafka_mgmt_config_ctx) +
			   nvmeibt_tlv_get_len(&dst->raft_members_ctx);
	TEST_ASSERT_TRUE(TEST_is_persist_and_wire_buf_crc_and_len_ok(dst, data_len));
	persist_and_wire_buf_validate_len(dst);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_fcrc_dst, dst);
	NNVMEIBT_TOMA_FREE(test_fcrc_upd, upd);
	return rv;
}

/***********  Merge error paths: old is incremental (not complete)  **********/

DEFINE_TEST(merge_topo_old_incremental_fails)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 1LL, 64, 0xAA);
	craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_INCREMENTAL, 2LL, 32, 0xBB);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, &upd_ptr);
	TEST_ASSERT_EQ(merge_size, -1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(merge_topo_config_old_incremental_fails)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 1LL, 64, 0xAA);
	craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 2LL, 32, 0xBB);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, &upd_ptr);
	TEST_ASSERT_EQ(merge_size, -1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(merge_kafka_config_old_incremental_fails)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 1LL, 64, 0xAA);
	craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 2LL, 32, 0xBB);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, &upd_ptr);
	TEST_ASSERT_EQ(merge_size, -1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(merge_raft_members_old_incremental_fails)
{
	struct section_merge_test_ctx		*ctx = (struct section_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 1LL, 64, 0xAA);
	craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 2LL, 32, 0xBB);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, &upd_ptr);
	TEST_ASSERT_EQ(merge_size, -1);
	rv = 0;
out:
	return rv;
}

/***********************    Realloc & Update tests    *************************/

static struct nvmeibt_persist_and_wire_buf *build_test_buf_ex(
	unsigned long long raft_term,
	bool is_topo_incremental, char *topo_data, int topo_len, int64_t topo_idx,
	bool is_topo_config_incremental, char *tc_data, int tc_len, int64_t tc_idx,
	bool is_kafka_mgmt_config_incremental, char *kmc_data, int kmc_len, int64_t kmc_idx,
	bool is_raft_members_incremental, char *rm_data, int rm_len, int64_t rm_idx, int64_t rm_seq_no)
{
	union nvmeib_uuid null_uuid;

	memset(&null_uuid, 0, sizeof(null_uuid));
	return nvmeibt_raft_generate_persist_and_wire_buf(
		raft_term, 0, 0, &null_uuid, &null_uuid, 0, 0, 0,
		is_topo_incremental, topo_idx, -1LL, topo_data, topo_len,
		is_topo_config_incremental, tc_idx, -1LL, tc_data, tc_len,
		is_kafka_mgmt_config_incremental, kmc_idx, -1LL, kmc_data, kmc_len,
		is_raft_members_incremental, rm_idx, rm_seq_no, rm_data, rm_len);
}

static struct nvmeibt_persist_and_wire_buf *build_test_buf(
	unsigned long long raft_term,
	char *topo_data, int topo_len, int64_t topo_idx,
	char *tc_data, int tc_len, int64_t tc_idx,
	char *kmc_data, int kmc_len, int64_t kmc_idx,
	char *rm_data, int rm_len, int64_t rm_idx)
{
	return build_test_buf_ex(raft_term,
		false, topo_data, topo_len, topo_idx,
		false, tc_data, tc_len, tc_idx,
		false, kmc_data, kmc_len, kmc_idx,
		false, rm_data, rm_len, rm_idx, -1LL);
}

DEFINE_TEST(topo_incremental_configs_complete_inplace)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*old_saved = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	struct nvmeibt_wire_type_len_value		tlv_ctx;
	struct test_praid_spec					old_praids[1], incr_praids[1];
	char									old_topo_wire[1024], incr_topo_wire[1024];
	char									tc_wire[128], kmc_wire[128], rm_wire[128];
	char									*topo_data_out = NULL;
	int										old_topo_len;
	int										incr_topo_len;
	int										tc_len;
	int										kmc_len;
	int										rm_len;
	int										rv = -1;

	(void)_ctx;
	TEST_init_praids_hash();
	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 0;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;
	incr_praids[0] = old_praids[0];
	incr_praids[0].topo_idx_updated = 20;
	incr_praids[0].praid_version_major = 2;

	old_topo_len = craft_topo_buf(old_topo_wire, (int)sizeof(old_topo_wire), &tlv_ctx,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_topo_len > 0);
	incr_topo_len = craft_topo_buf(incr_topo_wire, (int)sizeof(incr_topo_wire), &tlv_ctx,
			TLV_TYPE_TOPO_INCREMENTAL, 20LL, 1, incr_praids);
	TEST_ASSERT_TRUE(incr_topo_len > 0);
	tc_len = craft_section_buf(tc_wire, (int)sizeof(tc_wire), &tlv_ctx,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 20LL, 50, 0xBB);
	TEST_ASSERT_TRUE(tc_len > 0);
	kmc_len = craft_section_buf(kmc_wire, (int)sizeof(kmc_wire), &tlv_ctx,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 30LL, 30, 0xCC);
	TEST_ASSERT_TRUE(kmc_len > 0);
	rm_len = craft_section_buf(rm_wire, (int)sizeof(rm_wire), &tlv_ctx,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 40LL, 40, 0xDD);
	TEST_ASSERT_TRUE(rm_len > 0);

	old = build_test_buf_ex(1, false, old_topo_wire, old_topo_len, 10LL,
			false, tc_wire, tc_len, 20LL,
			false, kmc_wire, kmc_len, 30LL,
			false, rm_wire, rm_len, 40LL, -1LL);
	old_saved = old;
	upd = build_test_buf_ex(3, true, incr_topo_wire, incr_topo_len, 20LL,
			false, tc_wire, tc_len, 20LL,
			false, kmc_wire, kmc_len, 30LL,
			false, rm_wire, rm_len, 40LL, -1LL);

	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst == old_saved);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst->topo_ctx), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 20LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst->topo_config_ctx), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 20LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->kafka_mgmt_config_ctx), 30LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->raft_members_ctx), 40LL);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, TLV_TYPE_TOPO_COMPLETE, &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	TEST_ASSERT_EQ(get_topo_praid_count(topo_data_out), 1);
	{
		struct nvmeibt_praid_serialized_topo	*found;
		struct nvmeibt_praid_serialized_topo	host_praid;

		found = find_praid_in_topo(topo_data_out, &old_praids[0].uuid);
		TEST_ASSERT_NOT_NULL(found);
		nvmeibt_praid_convert_topo_le_be(found, &host_praid);
		TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 20);
		TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	}
	persist_and_wire_buf_validate_len(dst);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_mixed_dst, dst);
	NNVMEIBT_TOMA_FREE(test_mixed_upd, upd);
	return rv;
}

DEFINE_TEST(all_sections_incremental_full_merge)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*old_saved = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	struct nvmeibt_wire_type_len_value		tlv_ctx;
	struct test_praid_spec					old_praids[1], incr_praids[1];
	struct test_vol_spec					old_vols[1], incr_vols[1];
	struct test_raft_member_spec			old_members[1], incr_members[1];
	char									old_topo_wire[1024], incr_topo_wire[1024];
	char									old_tc_wire[1024], incr_tc_wire[1024];
	char									old_kmc_wire[1024], incr_kmc_wire[1024];
	char									old_rm_wire[1024], incr_rm_wire[1024];
	char									blkdev_wire[1024];
	union nvmeib_uuid						chunk_uuid;
	union nvmeib_uuid						praid_uuid;
	char									*topo_data_out = NULL;
	char									*tc_data_out = NULL;
	char									*kmc_data_out = NULL;
	char									*rm_data_out = NULL;
	int										old_topo_len;
	int										incr_topo_len;
	int										old_tc_len;
	int										incr_tc_len;
	int										old_kmc_len;
	int										incr_kmc_len;
	int										old_rm_len;
	int										incr_rm_len;
	int										blkdev_wire_len;
	int										rv = -1;

	(void)_ctx;
	TEST_init_raft_members_hash();
	TEST_init_blkdevs_hash();
	TEST_init_chunks_hash();
	TEST_init_praids_hash();

	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 0;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;
	incr_praids[0] = old_praids[0];
	incr_praids[0].topo_idx_updated = 20;
	incr_praids[0].praid_version_major = 2;
	old_topo_len = craft_topo_buf(old_topo_wire, (int)sizeof(old_topo_wire), &tlv_ctx,
			TLV_TYPE_TOPO_COMPLETE, 10LL, 1, old_praids);
	TEST_ASSERT_TRUE(old_topo_len > 0);
	incr_topo_len = craft_topo_buf(incr_topo_wire, (int)sizeof(incr_topo_wire), &tlv_ctx,
			TLV_TYPE_TOPO_INCREMENTAL, 20LL, 1, incr_praids);
	TEST_ASSERT_TRUE(incr_topo_len > 0);

	make_test_uuid(&old_vols[0].uuid, 1);
	old_vols[0].version = 10;
	old_vols[0].kafka_offset_or_idx = 100;
	incr_vols[0] = old_vols[0];
	incr_vols[0].version = 20;
	incr_vols[0].kafka_offset_or_idx = 200;
	old_tc_len = craft_topo_config_buf(old_tc_wire, (int)sizeof(old_tc_wire), &tlv_ctx,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 100LL, 1, old_vols);
	TEST_ASSERT_TRUE(old_tc_len > 0);
	incr_tc_len = craft_topo_config_buf(incr_tc_wire, (int)sizeof(incr_tc_wire), &tlv_ctx,
			TLV_TYPE_TOPO_CONFIG_INCREMENTAL, 200LL, 1, incr_vols);
	TEST_ASSERT_TRUE(incr_tc_len > 0);
	old_kmc_len = craft_kafka_mgmt_config_buf(old_kmc_wire, (int)sizeof(old_kmc_wire), &tlv_ctx,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 100LL, 1, old_vols);
	TEST_ASSERT_TRUE(old_kmc_len > 0);
	incr_kmc_len = craft_kafka_mgmt_config_buf(incr_kmc_wire, (int)sizeof(incr_kmc_wire), &tlv_ctx,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 200LL, 1, incr_vols);
	TEST_ASSERT_TRUE(incr_kmc_len > 0);

	init_raft_member_spec(&old_members[0], 1, 10, 100);
	init_raft_member_spec(&incr_members[0], 1, 20, 200);
	setup_test_raft_member(&old_members[0], 1, 10, 100);
	old_rm_len = craft_raft_members_buf(old_rm_wire, (int)sizeof(old_rm_wire), &tlv_ctx,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 100LL, 1, old_members);
	TEST_ASSERT_TRUE(old_rm_len > 0);
	incr_rm_len = craft_raft_members_buf(incr_rm_wire, (int)sizeof(incr_rm_wire), &tlv_ctx,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 200LL, 1, incr_members);
	TEST_ASSERT_TRUE(incr_rm_len > 0);

	make_test_uuid(&praid_uuid, 2000);
	TEST_add_praid_to_hash(&praid_uuid, 100, 10, 0);
	make_test_uuid(&chunk_uuid, 1000);
	TEST_add_chunk_to_hash(&chunk_uuid, 1, &praid_uuid);
	blkdev_wire_len = serialize_vol_wire_data(blkdev_wire, (int)sizeof(blkdev_wire), &old_vols[0], 0);
	TEST_add_blkdev_to_hash(&old_vols[0].uuid, 10, blkdev_wire, blkdev_wire_len, false);

	old = build_test_buf_ex(1, false, old_topo_wire, old_topo_len, 10LL,
			false, old_tc_wire, old_tc_len, 100LL,
			false, old_kmc_wire, old_kmc_len, 100LL,
			false, old_rm_wire, old_rm_len, 100LL, 10LL);
	old_saved = old;
	upd = build_test_buf_ex(5, true, incr_topo_wire, incr_topo_len, 20LL,
			true, incr_tc_wire, incr_tc_len, 200LL,
			true, incr_kmc_wire, incr_kmc_len, 200LL,
			true, incr_rm_wire, incr_rm_len, 200LL, 20LL);

	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst != old_saved);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst->topo_ctx), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst->topo_config_ctx), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst->kafka_mgmt_config_ctx), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&dst->raft_members_ctx), TLV_TYPE_RAFT_MEMBERS_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 20LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 200LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->kafka_mgmt_config_ctx), 200LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->raft_members_ctx), 200LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_seq_no(&dst->raft_members_ctx), 20LL);

	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, TLV_TYPE_TOPO_COMPLETE, &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	{
		struct nvmeibt_praid_serialized_topo	*found;
		struct nvmeibt_praid_serialized_topo	host_praid;

		found = find_praid_in_topo(topo_data_out, &old_praids[0].uuid);
		TEST_ASSERT_NOT_NULL(found);
		nvmeibt_praid_convert_topo_le_be(found, &host_praid);
		TEST_ASSERT_EQ(nvmeibt_praid_serialized_get_topo_idx_updated(&host_praid), 20);
		TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	}

	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, TLV_TYPE_TOPO_CONFIG_COMPLETE, &tc_data_out);
	TEST_ASSERT_TRUE(tc_data_out != NULL);
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(tc_data_out, true, NULL);

		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].chunks[0].praids[0].version, 20);
		mm_conf_free_tree(merged);
	}

	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, &kmc_data_out);
	TEST_ASSERT_TRUE(kmc_data_out != NULL);
	{
		struct mm_mgmt_conf *merged = mm_wire_buf_to_mm_mgmt_conf(kmc_data_out, false, NULL);

		TEST_ASSERT_NOT_NULL(merged);
		TEST_ASSERT_EQ(merged->num_vols, 1);
		TEST_ASSERT_EQ((int)merged->volumes[0].version, 20);
		mm_conf_free_tree(merged);
	}

	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, TLV_TYPE_RAFT_MEMBERS_COMPLETE, &rm_data_out);
	TEST_ASSERT_TRUE(rm_data_out != NULL);
	{
		struct mm_raft_member_conf	out_member __attribute__((aligned(16)));
		struct mm_raft_member_conf	host_member __attribute__((aligned(16)));

		memcpy(&out_member, rm_data_out + sizeof(int) * 2, sizeof(out_member));
		nvmeibt_raft_member_conf_convert_le_be(&host_member, &out_member);
		TEST_ASSERT_EQ(host_member.raft_members_seq_no_updated, 20);
		TEST_ASSERT_EQ(host_member.kafka_offset, 200);
	}

	persist_and_wire_buf_validate_len(dst);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_inc_full_dst, dst);
	NNVMEIBT_TOMA_FREE(test_inc_full_upd, upd);
	return rv;
}

DEFINE_TEST(first_update_with_raft_log)
{
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo[100], tc[50], kmc[30], rm[40];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	upd = build_test_buf(5, topo, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(NULL, upd, true);
	TEST_ASSERT_TRUE(dst != NULL);
	TEST_ASSERT_TRUE(dst != upd);
	TEST_ASSERT_EQ(persist_and_wire_buf_get_total_len(dst), persist_and_wire_buf_get_total_len(upd));
	TEST_ASSERT_MEM_EQ(dst, upd, persist_and_wire_buf_get_total_len(upd));
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test1_dst, dst);
	NNVMEIBT_TOMA_FREE(test1_upd, upd);
	return rv;
}

DEFINE_TEST(first_update_without_raft_log)
{
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo[100], tc[50], kmc[30], rm[40];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	upd = build_test_buf(7, topo, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(NULL, upd, false);
	TEST_ASSERT_TRUE(dst != NULL);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 7LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->kafka_mgmt_config_ctx), 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->raft_members_ctx), 0);
	TEST_ASSERT_EQ(dst->buf_encoding_ver, upd->buf_encoding_ver);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test2_dst, dst);
	NNVMEIBT_TOMA_FREE(test2_upd, upd);
	return rv;
}

DEFINE_TEST(equal_bufs_only_raft_ctx_updated)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo[100], tc[50], kmc[30], rm[40];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	old = build_test_buf(1, topo, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	upd = build_test_buf(5, topo, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst == old);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 5LL);
	TEST_ASSERT_EQ(dst->buf_encoding_ver, upd->buf_encoding_ver);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 100);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test3_dst, dst);
	NNVMEIBT_TOMA_FREE(test3_upd, upd);
	return rv;
}

DEFINE_TEST(no_raft_log_keeps_old)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo_old[100], tc_old[50], kmc_old[30], rm_old[40];
	char									topo_upd[200], tc_upd[80];
	int										rv = -1;

	(void)_ctx;
	memset(topo_old, 0xAA, sizeof(topo_old));
	memset(tc_old, 0xBB, sizeof(tc_old));
	memset(kmc_old, 0xCC, sizeof(kmc_old));
	memset(rm_old, 0xDD, sizeof(rm_old));
	memset(topo_upd, 0xEE, sizeof(topo_upd));
	memset(tc_upd, 0xFF, sizeof(tc_upd));
	old = build_test_buf(1, topo_old, 100, 10, tc_old, 50, 20, kmc_old, 30, 30, rm_old, 40, 40);
	upd = build_test_buf(9, topo_upd, 200, 99, tc_upd, 80, 88, NULL, 0, 77, NULL, 0, 66);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, false);
	TEST_ASSERT_TRUE(dst == old);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 9LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 10LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 100);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test4_dst, dst);
	NNVMEIBT_TOMA_FREE(test4_upd, upd);
	return rv;
}

DEFINE_TEST(topo_only_same_size_inplace)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo_old[100], topo_upd[100];
	char									tc[50], kmc[30], rm[40];
	char									*topo_data_out = NULL;
	char									*tc_data_out = NULL;
	int										rv = -1;

	(void)_ctx;
	memset(topo_old, 0xAA, sizeof(topo_old));
	memset(topo_upd, 0xEE, sizeof(topo_upd));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	old = build_test_buf(1, topo_old, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	upd = build_test_buf(3, topo_upd, 100, 15, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst == old);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 15LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 100);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_ctx), &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	TEST_ASSERT_MEM_EQ(topo_data_out, topo_upd, 100);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 20LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 50);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_config_ctx), &tc_data_out);
	TEST_ASSERT_TRUE(tc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(tc_data_out, tc, 50);
	persist_and_wire_buf_validate_len(dst);
	TEST_ASSERT_EQ(dst->buf_encoding_ver, upd->buf_encoding_ver);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 3LL);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test5_dst, dst);
	NNVMEIBT_TOMA_FREE(test5_upd, upd);
	return rv;
}

DEFINE_TEST(topo_only_diff_size_realloc)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*old_saved = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo_old[100], topo_upd[200];
	char									tc[50], kmc[30], rm[40];
	char									*topo_data_out = NULL;
	char									*tc_data_out = NULL;
	int										rv = -1;

	(void)_ctx;
	memset(topo_old, 0xAA, sizeof(topo_old));
	memset(topo_upd, 0xEE, sizeof(topo_upd));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	old = build_test_buf(1, topo_old, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	old_saved = old;
	upd = build_test_buf(3, topo_upd, 200, 15, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst != old_saved);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 15LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 200);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_ctx), &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	TEST_ASSERT_MEM_EQ(topo_data_out, topo_upd, 200);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 20LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 50);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_config_ctx), &tc_data_out);
	TEST_ASSERT_TRUE(tc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(tc_data_out, tc, 50);
	TEST_ASSERT_EQ(persist_and_wire_buf_get_total_len(dst), (int)sizeof(*dst) + 200 + 50 + 30 + 40);
	persist_and_wire_buf_validate_len(dst);
	TEST_ASSERT_EQ(dst->buf_encoding_ver, upd->buf_encoding_ver);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 3LL);
	rv = 0;
out:
	// old was freed by the function. Only free dst and upd.
	NNVMEIBT_TOMA_FREE(test6_dst, dst);
	NNVMEIBT_TOMA_FREE(test6_upd, upd);
	return rv;
}

DEFINE_TEST(full_alloc_topo_and_configs)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*old_saved = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo_old[100], topo_upd[120];
	char									tc_old[50], tc_upd[60];
	char									kmc[30], rm[40];
	char									*topo_data_out = NULL;
	char									*tc_data_out = NULL;
	char									*kmc_data_out = NULL;
	int										rv = -1;

	(void)_ctx;
	memset(topo_old, 0xAA, sizeof(topo_old));
	memset(topo_upd, 0xEE, sizeof(topo_upd));
	memset(tc_old, 0xBB, sizeof(tc_old));
	memset(tc_upd, 0xFF, sizeof(tc_upd));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	old = build_test_buf(1, topo_old, 100, 10, tc_old, 50, 20, kmc, 30, 30, rm, 40, 40);
	old_saved = old;
	upd = build_test_buf(5, topo_upd, 120, 15, tc_upd, 60, 25, NULL, 0, 30, NULL, 0, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst != old_saved);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 15LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 120);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_ctx), &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	TEST_ASSERT_MEM_EQ(topo_data_out, topo_upd, 120);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 25LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 60);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_config_ctx), &tc_data_out);
	TEST_ASSERT_TRUE(tc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(tc_data_out, tc_upd, 60);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->kafka_mgmt_config_ctx), 30LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->kafka_mgmt_config_ctx), 30);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->kafka_mgmt_config_ctx), &kmc_data_out);
	TEST_ASSERT_TRUE(kmc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(kmc_data_out, kmc, 30);
	TEST_ASSERT_EQ(persist_and_wire_buf_get_total_len(dst), (int)sizeof(*dst) + 120 + 60 + 30 + 40);
	persist_and_wire_buf_validate_len(dst);
	TEST_ASSERT_EQ(dst->buf_encoding_ver, upd->buf_encoding_ver);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 5LL);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test7_dst, dst);
	NNVMEIBT_TOMA_FREE(test7_upd, upd);
	return rv;
}

DEFINE_TEST(error_in_pass1_keeps_old)
{
	struct nvmeibt_persist_and_wire_buf		*old = NULL;
	struct nvmeibt_persist_and_wire_buf		*upd = NULL;
	struct nvmeibt_persist_and_wire_buf		*dst = NULL;
	char									topo_old[100], tc_old[50], kmc[30], rm[40];
	char									topo_upd[120], tc_upd[60];
	int										rv = -1;

	(void)_ctx;
	memset(topo_old, 0xAA, sizeof(topo_old));
	memset(tc_old, 0xBB, sizeof(tc_old));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	memset(topo_upd, 0xEE, sizeof(topo_upd));
	memset(tc_upd, 0xFF, sizeof(tc_upd));
	old = build_test_buf(1, topo_old, 100, 10, tc_old, 50, 20, kmc, 30, 30, rm, 40, 40);
	// Build upd with different topo+tc idx so compare returns TOPO_AND_CONFIGS.
	upd = build_test_buf(8, topo_upd, 120, 15, tc_upd, 60, 25, NULL, 0, 30, NULL, 0, 40);
	// Corrupt topo TLV type to invalid value. The merge function will return -1.
	upd->topo_ctx.tlv_type = LE_SWAP8((int8_t)99);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst == old);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 8LL);
	TEST_ASSERT_EQ(dst->buf_encoding_ver, upd->buf_encoding_ver);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 10LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 100);
	persist_and_wire_buf_validate_len(dst);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test8_dst, dst);
	NNVMEIBT_TOMA_FREE(test8_upd, upd);
	return rv;
}

/**********  Leader wire buf generation verification  ************/

DEFINE_TEST(generate_complete_buf_types_correct)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64], tc[32], kmc[16], rm[16];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	buf = build_test_buf(1, topo, 64, 10, tc, 32, 20, kmc, 16, 30, rm, 16, 40);
	TEST_ASSERT_NOT_NULL(buf);

	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->topo_ctx), TLV_TYPE_TOPO_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->topo_config_ctx), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->kafka_mgmt_config_ctx), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->raft_members_ctx), TLV_TYPE_RAFT_MEMBERS_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->topo_ctx), 64);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->topo_config_ctx), 32);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->kafka_mgmt_config_ctx), 16);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->raft_members_ctx), 16);
	persist_and_wire_buf_validate_len(buf);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_gen1, buf);
	return rv;
}

DEFINE_TEST(generate_incremental_buf_types_correct)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64], tc[32], kmc[16], rm[16];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	buf = build_test_buf_ex(1, true, topo, 64, 10,
			true, tc, 32, 20,
			true, kmc, 16, 30,
			true, rm, 16, 40, -1LL);
	TEST_ASSERT_NOT_NULL(buf);

	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->topo_ctx), TLV_TYPE_TOPO_INCREMENTAL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->topo_config_ctx), TLV_TYPE_TOPO_CONFIG_INCREMENTAL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->kafka_mgmt_config_ctx), TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->raft_members_ctx), TLV_TYPE_RAFT_MEMBERS_INCREMENTAL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->topo_ctx), 64);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->topo_config_ctx), 32);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->kafka_mgmt_config_ctx), 16);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&buf->raft_members_ctx), 16);
	persist_and_wire_buf_validate_len(buf);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_gen2, buf);
	return rv;
}

DEFINE_TEST(generate_mixed_buf_types_correct)
{
	struct nvmeibt_persist_and_wire_buf		*buf = NULL;
	char									topo[64], tc[32], kmc[16], rm[16];
	int										rv = -1;

	(void)_ctx;
	memset(topo, 0xAA, sizeof(topo));
	memset(tc, 0xBB, sizeof(tc));
	memset(kmc, 0xCC, sizeof(kmc));
	memset(rm, 0xDD, sizeof(rm));
	buf = build_test_buf_ex(1, true, topo, 64, 10,
			false, tc, 32, 20,
			false, kmc, 16, 30,
			false, rm, 16, 40, -1LL);
	TEST_ASSERT_NOT_NULL(buf);

	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->topo_ctx), TLV_TYPE_TOPO_INCREMENTAL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->topo_config_ctx), TLV_TYPE_TOPO_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->kafka_mgmt_config_ctx), TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_type(&buf->raft_members_ctx), TLV_TYPE_RAFT_MEMBERS_COMPLETE);
	persist_and_wire_buf_validate_len(buf);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test_gen3, buf);
	return rv;
}

/*******************************    Main    ************************************/

int wire_buf_test_main(int argc, char *argv[])
{
	struct section_merge_test_ctx	ctx;
	const char						*selection = NULL;
	int								rv = 1;

	#define X(func, name, desc) {name, desc, test_##func},
	struct toma_test_entry tests[] = { WIRE_BUF_TEST_LIST };
	#undef X

	TEST_init();

	ctx.buf_size = WIRE_BUF_TEST_BUF_SIZE;
	ctx.old_buf = (char *)malloc((size_t)ctx.buf_size);
	ctx.upd_buf = (char *)malloc((size_t)ctx.buf_size);
	ctx.dst_buf = (char *)malloc((size_t)ctx.buf_size);

	if (ctx.old_buf == NULL || ctx.upd_buf == NULL || ctx.dst_buf == NULL) {
		fprintf(stderr, "wire_buf_test: malloc failed\n");
		goto out;
	}

	if (argc > 1) {
		selection = argv[1];
	}

	rv = test_run_suite("Wire Buffer",
			   tests, (int)(sizeof(tests) / sizeof(tests[0])),
			   &ctx, selection, 0);

out:
	free(ctx.old_buf);
	free(ctx.upd_buf);
	free(ctx.dst_buf);
	return rv;
}
