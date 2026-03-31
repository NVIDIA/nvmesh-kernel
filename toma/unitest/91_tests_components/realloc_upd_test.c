/**
 * realloc_upd_test.c - Unit tests for realloc_and_upd two-pass merge
 *
 * Tests the orchestration logic in
 * realloc_and_upd_follower_persist_and_wire_bufs_with_incoming_data:
 * - Two-pass size calculation + allocation + merge
 * - Topo-only in-place optimization
 * - Error fallback when merge returns -1
 * - Early-return paths (NULL old, EQUAL, no raft log)
 */

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_persistency_info.h"
#include "nvmeibt_raft.h"
#include "unitest/00_framework/toma_test_framework.h"
#include "unitest/00_framework/toma_test_helpers.h"
#include "realloc_upd_test.h"
#include <stdlib.h>
#include <string.h>

/*
 * Simplified builder around nvmeibt_raft_generate_persist_and_wire_buf.
 * Returns a TOMA-allocated persist_and_wire_buf with complete TLV types.
 * Pass NULL/0 for sections that should be empty (upd_len==0 triggers "keep old" in merge).
 */
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

/***********************    old=NULL paths    **********************************/

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

/**********************    Early-return paths    *******************************/

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

/**********************    Two-pass merge paths    *****************************/

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
	TEST_ASSERT_EQ(persist_and_wire_buf_get_total_len(dst),
				   (int)sizeof(*dst) + 200 + 50 + 30 + 40);
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
	TEST_ASSERT_EQ(persist_and_wire_buf_get_total_len(dst),
				   (int)sizeof(*dst) + 120 + 60 + 30 + 40);
	persist_and_wire_buf_validate_len(dst);

	TEST_ASSERT_EQ(dst->buf_sw_ver, upd->buf_sw_ver);
	TEST_ASSERT_EQ((long long)persist_and_wire_buf_get_current_raft_TERM(dst), 5LL);
	rv = 0;
out:
	NNVMEIBT_TOMA_FREE(test7_dst, dst);
	NNVMEIBT_TOMA_FREE(test7_upd, upd);
	return rv;
}

/**************************    Error path    **********************************/

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

int realloc_upd_test_main(int argc, char *argv[])
{
	const char					*selection = NULL;

	#define X(func, name, desc) {name, desc, test_##func},
	struct toma_test_entry tests[] = { REALLOC_UPD_TEST_LIST };
	#undef X

	TEST_init();

	if (argc > 1) {
		selection = argv[1];
	}

	return test_run_suite("Realloc & Update",
			   tests, (int)(sizeof(tests) / sizeof(tests[0])),
			   NULL, selection, 0);
}
