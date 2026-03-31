/**
 * wire_buf_test.c - Wire buffer unit tests
 *
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
		struct nvmeibt_praid_serialized_topo	host_praid;
		struct nvmeibt_praid_serialized_topo	*p = (struct nvmeibt_praid_serialized_topo *)ptr;

		// Build praid in host byte order, then convert to wire format using
		// the production conversion function for correct UUID/field encoding.
		memset(&host_praid, 0, sizeof(host_praid));
		memcpy(host_praid.eyecatcher, "PRAD", 4);
		host_praid.uuid = praids[i].uuid;
		host_praid.segs_num = (int8_t)praids[i].segs_num;
		host_praid.topo_idx_updated = praids[i].topo_idx_updated;
		host_praid.praid_version_major = praids[i].praid_version_major;
		host_praid.praid_version_minor = praids[i].praid_version_minor;
		host_praid.is_activated = 1;
		nvmeibt_praid_convert_topo_le_be(&host_praid, p, TOMA_SW_COMPATIBILITY_VER);
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

		nvmeibt_praid_convert_topo_le_be(p, &host_praid, TOMA_SW_COMPATIBILITY_VER);
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
	(void)_ctx;
	return 0;
}

DEFINE_TEST(incremental_topo_multi_praid_partial_update)
{
	(void)_ctx;
	return 0;
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
	(void)_ctx;
	return 0;
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
	(void)_ctx;
	return 0;
}

DEFINE_TEST(incremental_topo_ordering_differs)
{
	(void)_ctx;
	return 0;
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
	(void)_ctx;
	return 0;
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

// ------------- Incremental: topo config (not yet implemented) ------------

DEFINE_TEST(incremental_topo_config_not_implemented)
{
	(void)_ctx;
	return 0;
}

// ---------- Incremental: kafka mgmt config (not yet implemented) ---------

DEFINE_TEST(incremental_kafka_config_not_implemented)
{
	(void)_ctx;
	return 0;
}

// ----------- Incremental: raft members (not yet implemented) -------------

DEFINE_TEST(incremental_raft_members_not_implemented)
{
	(void)_ctx;
	return 0;
}

/***********************    Realloc & Update tests    *************************/

static struct nvmeibt_persist_and_wire_buf *build_test_buf(
	unsigned long long raft_term,
	char *topo_data, int topo_len, int64_t topo_idx,
	char *tc_data, int tc_len, int64_t tc_idx,
	char *kmc_data, int kmc_len, int64_t kmc_idx,
	char *rm_data, int rm_len, int64_t rm_idx)
{
	union nvmeib_uuid null_uuid;

	memset(&null_uuid, 0, sizeof(null_uuid));
	return nvmeibt_raft_generate_persist_and_wire_buf(
		raft_term, 0, 0, &null_uuid, &null_uuid, 0, 0, 0,
		false, topo_idx, -1LL, topo_data, topo_len,
		false, tc_idx, -1LL, tc_data, tc_len,
		false, kmc_idx, -1LL, kmc_data, kmc_len,
		false, rm_idx, -1LL, rm_data, rm_len);
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
	// Only raft_ctx copied, not TLV data
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 7LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->kafka_mgmt_config_ctx), 0);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->raft_members_ctx), 0);
	TEST_ASSERT_EQ(dst->buf_sw_ver, upd->buf_sw_ver);
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
	// Same idx values for all 4 sections => compare returns EQUAL
	old = build_test_buf(1, topo, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	upd = build_test_buf(5, topo, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst == old);	// Buffer reused
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 5LL);
	TEST_ASSERT_EQ(dst->buf_sw_ver, upd->buf_sw_ver);
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
	TEST_ASSERT_TRUE(dst == old);	// Buffer reused despite idx diffs
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 9LL);
	// TLV data unchanged from old
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
	// old: all 4 sections with data
	old = build_test_buf(1, topo_old, 100, 10, tc, 50, 20, kmc, 30, 30, rm, 40, 40);
	// upd: topo differs (idx=15 vs 10, same size), other sections same idx with len=0
	upd = build_test_buf(3, topo_upd, 100, 15, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst == old);	// In-place optimization
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 15LL);	// Topo updated
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 100);
	// Topo data should be upd's data
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_ctx), &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	TEST_ASSERT_MEM_EQ(topo_data_out, topo_upd, 100);
	// Other sections untouched
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 20LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 50);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_config_ctx), &tc_data_out);
	TEST_ASSERT_TRUE(tc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(tc_data_out, tc, 50);
	persist_and_wire_buf_validate_len(dst);
	TEST_ASSERT_EQ(dst->buf_sw_ver, upd->buf_sw_ver);
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
	// upd: larger topo (200 vs 100), different idx, other sections same idx with len=0
	upd = build_test_buf(3, topo_upd, 200, 15, NULL, 0, 20, NULL, 0, 30, NULL, 0, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst != old_saved);	// New allocation (old was freed by function)
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 15LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 200);
	// Topo data should be upd's data
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_ctx), &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	TEST_ASSERT_MEM_EQ(topo_data_out, topo_upd, 200);
	// Other sections preserved from old
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 20LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 50);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_config_ctx), &tc_data_out);
	TEST_ASSERT_TRUE(tc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(tc_data_out, tc, 50);
	// Total length consistent
	TEST_ASSERT_EQ(persist_and_wire_buf_get_total_len(dst), (int)sizeof(*dst) + 200 + 50 + 30 + 40);
	persist_and_wire_buf_validate_len(dst);
	TEST_ASSERT_EQ(dst->buf_sw_ver, upd->buf_sw_ver);
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
	// topo AND topo_config idx differ => TOPO_AND_CONFIGS. kmc/rm same idx with len=0.
	upd = build_test_buf(5, topo_upd, 120, 15, tc_upd, 60, 25, NULL, 0, 30, NULL, 0, 40);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst != old_saved);	// New allocation
	// Topo: uses upd (higher idx)
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 15LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 120);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_ctx), &topo_data_out);
	TEST_ASSERT_TRUE(topo_data_out != NULL);
	TEST_ASSERT_MEM_EQ(topo_data_out, topo_upd, 120);
	// Topo config: uses upd (higher idx)
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_config_ctx), 25LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_config_ctx), 60);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->topo_config_ctx), &tc_data_out);
	TEST_ASSERT_TRUE(tc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(tc_data_out, tc_upd, 60);
	// Kafka mgmt config: uses old (same idx, upd_len=0)
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->kafka_mgmt_config_ctx), 30LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->kafka_mgmt_config_ctx), 30);
	nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(dst, nvmeibt_tlv_get_type(&dst->kafka_mgmt_config_ctx), &kmc_data_out);
	TEST_ASSERT_TRUE(kmc_data_out != NULL);
	TEST_ASSERT_MEM_EQ(kmc_data_out, kmc, 30);
	// Total length
	TEST_ASSERT_EQ(persist_and_wire_buf_get_total_len(dst), (int)sizeof(*dst) + 120 + 60 + 30 + 40);
	persist_and_wire_buf_validate_len(dst);
	TEST_ASSERT_EQ(dst->buf_sw_ver, upd->buf_sw_ver);
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
	// Build upd with different topo+tc idx so compare returns TOPO_AND_CONFIGS
	upd = build_test_buf(8, topo_upd, 120, 15, tc_upd, 60, 25, NULL, 0, 30, NULL, 0, 40);
	// Corrupt topo TLV type to invalid value. The merge function will return -1.
	upd->topo_ctx.tlv_type = LE_SWAP8((int8_t)99);
	dst = TEST_realloc_and_upd_follower_persist_and_wire_bufs(old, upd, true);
	TEST_ASSERT_TRUE(dst == old);	// Error => keeps old
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 8LL);	// raft_ctx still updated
	TEST_ASSERT_EQ(dst->buf_sw_ver, upd->buf_sw_ver);
	// TLV data unchanged from old
	TEST_ASSERT_EQ(nvmeibt_tlv_get_idx(&dst->topo_ctx), 10LL);
	TEST_ASSERT_EQ(nvmeibt_tlv_get_len(&dst->topo_ctx), 100);
	persist_and_wire_buf_validate_len(dst);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test8_dst, dst);
	NNVMEIBT_TOMA_FREE(test8_upd, upd);
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
