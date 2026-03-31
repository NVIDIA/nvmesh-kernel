/**
 * wire_buf_test.h - Wire buffer unit tests
 *
 * Tests for persist_and_wire_buf operations: per-section merge, follower
 * realloc_and_upd orchestration, and incremental selection logic.
 *
 * Invoked via: ./nvmeibt_toma wire_buf_test [selection]
 */

#ifndef WIRE_BUF_TEST_H
#define WIRE_BUF_TEST_H

#include "nvmeibt_common.h"

#define WIRE_BUF_TEST_MAX_PRAIDS		64
#define WIRE_BUF_TEST_MAX_SEGS			8
#define WIRE_BUF_TEST_BUF_SIZE			(32 * 1024)

//
// Spec for a single praid used by the test helpers.
//
struct test_praid_spec {
	union nvmeib_uuid	uuid;
	int					segs_num;
	int64_t				topo_idx_updated;
	int					praid_version_major;
	int					praid_version_minor;
};

//
// Test context for section merge tests (wire buffers, optional hash state).
//
struct section_merge_test_ctx {
	char	*old_buf;
	char	*upd_buf;
	char	*dst_buf;
	int		buf_size;
};

//
// X-macro test list: X(func_name, "Test Name", "Description")
//

#define WIRE_BUF_TEST_LIST \
	/************************* Complete merges *************************/ \
	/* -- Complete: topo -- */ \
	X(complete_topo_replaces,						"Complete topo replaces old",				"Old complete + new complete => output == new") \
	X(complete_topo_same_idx_keeps_old,				"Complete topo same idx keeps old",			"Same idx + zero upd_len => keeps old data") \
	X(complete_topo_empty_old,						"Complete topo with empty old",				"First-ever update (old empty, zero-len) => copies upd data") \
	/* -- Complete: topo config -- */ \
	X(complete_topo_config_replaces,				"Complete topo_config replaces old",			"Old + new topo_config (both complete) => output == new") \
	/* -- Complete: kafka mgmt config -- */ \
	X(complete_kafka_config_replaces,				"Complete kafka_config replaces old",		"Old + new kafka_config (both complete) => output == new") \
	/* -- Complete: raft members -- */ \
	X(complete_raft_members_replaces,				"Complete raft_members replaces old",		"Old + new raft_members (both complete) => output == new") \
	/* -- Complete: multi-section -- */ \
	X(complete_all_sections_mixed,					"Mixed update: some sections change",		"Update topo+config, keep kafka+raft => only changed sections update") \
	/************************ Incremental merges ************************/ \
	/* -- Incremental: topo -- */ \
	X(incremental_topo_empty_keeps_old,				"Empty incremental keeps old topo",			"Old complete + empty incremental => output == old") \
	X(incremental_topo_single_praid_updated,		"Single praid updated",						"1 praid, incremental has higher topo_idx_updated => uses new") \
	X(incremental_topo_single_praid_not_updated,	"Single praid not updated (lower idx)",		"1 praid, incremental has lower topo_idx_updated => keeps old") \
	X(incremental_topo_multi_praid_partial_update,	"Multi praid partial update",				"3 praids, 1 updated in incremental => 2 old + 1 new") \
	X(incremental_topo_multi_praid_all_updated,		"Multi praid all updated",					"3 praids, all in incremental => all replaced") \
	X(incremental_topo_praid_with_segments,			"Praid with segments preserved",			"Praids with 2 segs each => segment data survives merge") \
	X(incremental_topo_praid_seg_count_changes,		"Praid segment count changes",				"Old has 2 segs, incremental has 3 => merged has 3, size correct") \
	X(incremental_topo_extra_uuid_ignored,			"Extra UUID in incremental ignored",		"Incremental has UUID not in old => ignored, old praids survive") \
	X(incremental_topo_ordering_differs,			"Incremental ordering differs from old",	"Old=[A,B,C], incr=[C,A] => merge works (O(n*m) scan)") \
	X(incremental_topo_same_idx_keeps_old,			"Incremental same idx keeps old",			"Incremental with same topo_idx_updated => keeps old praid") \
	X(incremental_topo_large_praid_count,			"Large praid count (50 praids)",			"50 praids, 10 updated => correct merge, no overruns") \
	X(incremental_topo_unknown_type_fails,			"Unknown TLV type returns error",			"Bogus incremental type hits default branch, returns -1") \
	X(incremental_topo_size_only_no_dst,			"Incremental topo size-only (no dst)",		"dst_wire_ctx NULL returns correct size without copying") \
	/* -- Incremental: topo config (not yet implemented) -- */ \
	X(incremental_topo_config_not_implemented,		"Topo_config incremental => error",			"TOPO_CONFIG_INCREMENTAL not implemented, returns -1") \
	/* -- Incremental: kafka mgmt config (not yet implemented) -- */ \
	X(incremental_kafka_config_not_implemented,		"Kafka_config incremental => error",		"KAFKA_MGMT_CONFIG_INCREMENTAL not implemented, returns -1") \
	/* -- Incremental: raft members (not yet implemented) -- */ \
	X(incremental_raft_members_not_implemented,		"Raft_members incremental => error",		"RAFT_MEMBERS_INCREMENTAL not implemented, returns -1") \
	/********** Follower realloc_and_upd orchestration **************************/ \
	X(first_update_with_raft_log,        "First update with raft log",        "old=NULL, raft_log=true => full memcpy of upd") \
	X(first_update_without_raft_log,     "First update without raft log",     "old=NULL, raft_log=false => only raft_ctx copied") \
	X(equal_bufs_only_raft_ctx_updated,  "Equal bufs updates raft_ctx only",  "All idx match => dst==old, raft_ctx+sw_ver updated") \
	X(no_raft_log_keeps_old,             "No raft log keeps old",             "is_with_raft_log=false => dst==old regardless of idx diff") \
	X(topo_only_same_size_inplace,       "Topo-only same size in-place",      "Only topo idx differs, same size => in-place merge") \
	X(topo_only_diff_size_realloc,       "Topo-only diff size realloc",       "Only topo idx differs, different size => full alloc") \
	X(full_alloc_topo_and_configs,       "Full alloc topo and configs",       "Multiple sections differ => new alloc, old freed") \
	X(error_in_pass1_keeps_old,          "Error in pass1 keeps old buf",      "Bogus TLV type => merge returns -1 => dst==old")

int wire_buf_test_main(int argc, char *argv[]);

#endif // #ifndef WIRE_BUF_TEST_H
