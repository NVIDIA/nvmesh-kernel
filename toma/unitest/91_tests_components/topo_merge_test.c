/**
 * topo_merge_test.c - Comprehensive topology merge unit tests
 *
 * Tests persist_and_wire_buf_calculate_and_merge_data_to_section (incremental
 * topo merge) via TEST_raft_merge_data_to_section.
 *
 * Validates:
 * - Topo incremental merge with various praid/segment configurations
 * - Complete-replaces-complete for all four TLV sections
 * - Edge cases (missing old, large praid counts)
 */

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_persistency_info.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_topo_bin.h"
#include "nvmeibt_disk_segment.h"
#include "unitest/00_framework/toma_test_framework.h"
#include "topo_merge_test.h"
#include <stdlib.h>
#include <string.h>

#define TOPO_HDR_NAME	"BIN_TOPO"

static void make_test_uuid(union nvmeib_uuid *uuid, int id)
{
	uuid->ll[0] = (unsigned long long)(0xAAAA000000000000ULL + (unsigned)id);
	uuid->ll[1] = (unsigned long long)(0xBBBB000000000000ULL + (unsigned)id);
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
	header->sw_ver = LE_SWAP32(TOMA_SW_COMPATIBILITY_VER);
	header->topo_len = LE_SWAP32(data_len);
	header->praids_num = LE_SWAP32(n_praids);

	ptr = buf + sizeof(*header);
	for (int i = 0; i < n_praids; i++) {
		struct nvmeibt_praid_serialized_topo	*p = (struct nvmeibt_praid_serialized_topo *)ptr;

		memcpy(p->eyecatcher, "PRAD", 4);
		p->uuid = praids[i].uuid;
		p->segs_num = LE_SWAP8((int8_t)praids[i].segs_num);
		p->topo_idx_updated = LE_SWAP64(praids[i].topo_idx_updated);
		p->praid_version_major = LE_SWAP32(praids[i].praid_version_major);
		p->praid_version_minor = LE_SWAP32(praids[i].praid_version_minor);
		p->is_activated = 1;
		ptr += sizeof(*p);

		for (int j = 0; j < praids[i].segs_num; j++) {
			struct nvmeibt_serialized_seg_leader_topo	*seg = (struct nvmeibt_serialized_seg_leader_topo *)ptr;

			memcpy(seg->eyecatcher, "DSEG", 4);
			make_test_uuid(&seg->uuid, i * 100 + j);
			seg->seg_idx = LE_SWAP8((int8_t)j);
			seg->praid_version_major = p->praid_version_major;
			seg->praid_version_minor = p->praid_version_minor;
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
		// Compare wire-format UUIDs directly (ll[0]/ll[1] match regardless of byte order)
		if (ARE_UUID_EQ(&p->uuid, target_uuid)) {
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

/*************************    Complete merges    *******************************/

// -------------------------- Complete: topo ---------------------------

DEFINE_TEST(complete_topo_replaces)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

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
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 20);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_single_praid_not_updated)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 0;
	old_praids[0].topo_idx_updated = 20;
	old_praids[0].praid_version_major = 2;
	old_praids[0].praid_version_minor = 0;

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
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 20);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_multi_praid_partial_update)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[3], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	for (int i = 0; i < 3; i++) {
		make_test_uuid(&old_praids[i].uuid, i + 1);
		old_praids[i].segs_num = 0;
		old_praids[i].topo_idx_updated = 10;
		old_praids[i].praid_version_major = 1;
		old_praids[i].praid_version_minor = 0;
	}

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
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 10);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[1].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 3);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 30);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[2].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 10);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_multi_praid_all_updated)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[3], incr_praids[3];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

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
		nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
		TEST_ASSERT_EQ(host_praid.praid_version_major, 5);
		TEST_ASSERT_EQ(host_praid.topo_idx_updated, 50);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_praid_with_segments)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[2], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;
	int									expected_size;
	union nvmeib_uuid					expected_uuid;
	struct nvmeibt_serialized_seg_leader_topo	*seg;

	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 2;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;

	make_test_uuid(&old_praids[1].uuid, 2);
	old_praids[1].segs_num = 2;
	old_praids[1].topo_idx_updated = 10;
	old_praids[1].praid_version_major = 1;
	old_praids[1].praid_version_minor = 0;

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

	expected_size = (int)sizeof(struct nvmeibt_topology_serialized_topo_header) +
			2 * ((int)sizeof(struct nvmeibt_praid_serialized_topo) +
				2 * (int)sizeof(struct nvmeibt_serialized_seg_leader_topo));
	TEST_ASSERT_EQ(merge_size, expected_size);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 2);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 20);
	TEST_ASSERT_EQ((int)nvmeibt_praid_wire_get_n_segs(found), 2);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[1].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 10);
	TEST_ASSERT_EQ((int)nvmeibt_praid_wire_get_n_segs(found), 2);

	// Verify segment data bytes survived the merge on kept praid
	seg = (struct nvmeibt_serialized_seg_leader_topo *)(found + 1);
	for (int s = 0; s < 2; s++) {
		make_test_uuid(&expected_uuid, 1 * 100 + s);
		TEST_ASSERT_TRUE(ARE_UUID_EQ(&seg[s].uuid, &expected_uuid));
		TEST_ASSERT_EQ(LE_SWAP8(seg[s].seg_idx), s);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_praid_seg_count_changes)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;
	int									expected_size;

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
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ((int)nvmeibt_praid_wire_get_n_segs(found), 3);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 2);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 20);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_extra_uuid_ignored)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 0;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;

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
	TEST_ASSERT_EQ(merge_size, old_len);
	TEST_ASSERT_EQ(get_topo_praid_count(ctx->dst_buf), 1);
	TEST_ASSERT_EQ((int)(old_ptr - ctx->old_buf), old_len);
	TEST_ASSERT_EQ((int)(upd_ptr - ctx->upd_buf), upd_len);
	TEST_ASSERT_EQ((int)(dst_ptr - ctx->dst_buf), merge_size);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[0].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 10);

	found = find_praid_in_topo(ctx->dst_buf, &incr_praids[0].uuid);
	TEST_ASSERT_NULL(found);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_ordering_differs)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[3], incr_praids[2];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	for (int i = 0; i < 3; i++) {
		make_test_uuid(&old_praids[i].uuid, i + 1);
		old_praids[i].segs_num = 0;
		old_praids[i].topo_idx_updated = 10;
		old_praids[i].praid_version_major = 1;
		old_praids[i].praid_version_minor = 0;
	}

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
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 4);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 40);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[1].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 10);

	found = find_praid_in_topo(ctx->dst_buf, &old_praids[2].uuid);
	TEST_ASSERT_NOT_NULL(found);
	nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
	TEST_ASSERT_EQ(host_praid.praid_version_major, 4);
	TEST_ASSERT_EQ(host_praid.topo_idx_updated, 40);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_same_idx_keeps_old)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[1], incr_praids[1];
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;

	// The merge function calls nvmeibt_abort when old_wire_ctx is NULL with
	// incremental data, so we cannot test that path directly.
	// Instead, test incremental with empty upd_len (upd_len==0) and verify
	// the function gracefully returns old_len (keep-old path).
	make_test_uuid(&old_praids[0].uuid, 1);
	old_praids[0].segs_num = 0;
	old_praids[0].topo_idx_updated = 10;
	old_praids[0].praid_version_major = 1;
	old_praids[0].praid_version_minor = 0;

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
		nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
		TEST_ASSERT_EQ(host_praid.praid_version_major, 1);
		TEST_ASSERT_EQ(host_praid.topo_idx_updated, 10);
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv, dst_tlv;
	struct test_praid_spec				old_praids[TOPO_MERGE_MAX_PRAIDS];
	struct test_praid_spec				incr_praids[TOPO_MERGE_MAX_PRAIDS];
	struct nvmeibt_praid_serialized_topo	*found;
	struct nvmeibt_praid_serialized_topo	host_praid;
	char								*old_ptr, *upd_ptr, *dst_ptr;
	int									old_len, upd_len;
	int									rv = -1;
	int									merge_size;
	int									i;

	for (i = 0; i < n_total_praids; i++) {
		make_test_uuid(&old_praids[i].uuid, i + 1);
		old_praids[i].segs_num = 0;
		old_praids[i].topo_idx_updated = 100;
		old_praids[i].praid_version_major = 1;
		old_praids[i].praid_version_minor = 0;
	}

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
		nvmeibt_praid_convert_topo_le_be(found, &host_praid, TOMA_SW_COMPATIBILITY_VER);
		TEST_ASSERT_EQ(host_praid.praid_version_major, expected_ver);
		TEST_ASSERT_EQ(host_praid.topo_idx_updated, expected_idx);
	}
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(incremental_topo_unknown_type_fails)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
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

// ------------- Incremental: topo config (not yet implemented) ------------

DEFINE_TEST(incremental_topo_config_not_implemented)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_TOPO_CONFIG_COMPLETE, 1LL, 64, 0xAA);
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

// ---------- Incremental: kafka mgmt config (not yet implemented) ---------

DEFINE_TEST(incremental_kafka_config_not_implemented)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, 1LL, 96, 0xCC);
	craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL, 2LL, 48, 0xDD);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, &upd_ptr);
	TEST_ASSERT_EQ(merge_size, -1);
	rv = 0;
out:
	return rv;
}

// ----------- Incremental: raft members (not yet implemented) -------------

DEFINE_TEST(incremental_raft_members_not_implemented)
{
	struct topo_merge_test_ctx			*ctx = (struct topo_merge_test_ctx *)_ctx;
	struct nvmeibt_wire_type_len_value	old_tlv, upd_tlv;
	char								*old_ptr;
	const char							*upd_ptr;
	int									rv = -1;
	int									merge_size;

	craft_section_buf(ctx->old_buf, ctx->buf_size, &old_tlv,
			TLV_TYPE_RAFT_MEMBERS_COMPLETE, 1LL, 48, 0x11);
	craft_section_buf(ctx->upd_buf, ctx->buf_size, &upd_tlv,
			TLV_TYPE_RAFT_MEMBERS_INCREMENTAL, 2LL, 24, 0x22);

	old_ptr = ctx->old_buf;
	upd_ptr = ctx->upd_buf;

	merge_size = TEST_raft_merge_data_to_section(NULL, &old_tlv, &upd_tlv,
			NULL, &old_ptr, &upd_ptr);
	TEST_ASSERT_EQ(merge_size, -1);
	rv = 0;
out:
	return rv;
}

/*******************************    Main    ************************************/

int topo_merge_test_main(int argc, char *argv[])
{
	struct topo_merge_test_ctx	ctx;
	const char					*selection = NULL;
	int							rv = 1;

	#define X(func, name, desc) {name, desc, test_##func},
	struct toma_test_entry tests[] = { TOPO_MERGE_TEST_LIST };
	#undef X

	ctx.buf_size = TOPO_MERGE_BUF_SIZE;
	ctx.old_buf = (char *)malloc((size_t)ctx.buf_size);
	ctx.upd_buf = (char *)malloc((size_t)ctx.buf_size);
	ctx.dst_buf = (char *)malloc((size_t)ctx.buf_size);

	if (ctx.old_buf == NULL || ctx.upd_buf == NULL || ctx.dst_buf == NULL) {
		fprintf(stderr, "topo_merge_test: malloc failed\n");
		goto out;
	}

	if (argc > 1) {
		selection = argv[1];
	}

	rv = test_run_suite("Topo Merge",
			   tests, (int)(sizeof(tests) / sizeof(tests[0])),
			   &ctx, selection, 0);

out:
	free(ctx.old_buf);
	free(ctx.upd_buf);
	free(ctx.dst_buf);
	return rv;
}
