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
	/************************* Serialized struct helpers ****************/ \
	X(topo_idx_updated_getter_setter,				"topo_idx_updated getter/setter round-trip",	"Verifies split-field get/set for zero, positive, negative, and high-bit values") \
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
	X(incremental_topo_extra_uuid_ignored,			"New UUID in incremental accepted",			"Incremental has UUID not in hash => accepted as new praid") \
	X(incremental_topo_ordering_differs,			"Incremental ordering differs from old",	"Old=[A,B,C], incr=[C,A] => merge works (O(n*m) scan)") \
	X(incremental_topo_same_idx_keeps_old,			"Incremental same idx keeps old",			"Incremental with same topo_idx_updated => keeps old praid") \
	X(incremental_topo_large_praid_count,			"Large praid count (50 praids)",			"50 praids, 10 updated => correct merge, no overruns") \
	X(incremental_topo_unknown_type_fails,			"Unknown TLV type returns error",			"Bogus incremental type hits default branch, returns -1") \
	X(incremental_topo_size_only_no_dst,			"Incremental topo size-only (no dst)",		"dst_wire_ctx NULL returns correct size without copying") \
	/* -- Incremental: empty upd keeps old for all section types -- */ \
	X(incremental_topo_config_empty_keeps_old,		"Empty topo_config incremental keeps old",	"TOPO_CONFIG_INCREMENTAL with upd_len==0 => keeps old data") \
	X(incremental_kafka_config_empty_keeps_old,		"Empty kafka_config incremental keeps old",	"KAFKA_MGMT_CONFIG_INCREMENTAL with upd_len==0 => keeps old data") \
	X(incremental_raft_members_empty_keeps_old,		"Empty raft_members incremental keeps old",	"RAFT_MEMBERS_INCREMENTAL with upd_len==0 => keeps old data") \
	/************** Incremental raft_members merge (non-empty) *****************/ \
	X(incremental_raft_members_partial_update,		"Raft members partial update",				"3 members, 1 updated => merged has 3 with correct seq_no") \
	X(incremental_raft_members_new_member_accepted,	"Raft members new member accepted",			"New member in incremental not in hash => accepted") \
	X(incremental_raft_members_old_seq_keeps_hash,	"Raft members old seq keeps hash",			"Incremental with lower seq_no => keeps hash member data") \
	X(incremental_raft_members_removed_member_kept,	"Raft members removed member kept",			"2 members, incremental has 1 => non-visited member appended from hash") \
	/*********** Incremental kafka_mgmt_config merge (non-empty) ****************/ \
	X(incremental_kafka_config_partial_update,		"Kafka config partial vol update",			"2 vols, 1 updated => merged has both with correct version") \
	X(incremental_kafka_config_old_version_keeps_hash, "Kafka config old version keeps hash",	"Incremental with lower version => uses hash blkdev wire buf") \
	X(incremental_kafka_config_vol_deleted,			"Kafka config vol deleted",					"2 vols, 1 being deleted => incremental has 1, merge skips deleted") \
	X(incremental_kafka_config_vol_added,			"Kafka config vol added",					"1 vol exists, incremental has 2 => merge produces 2") \
	/*********** Incremental topo_config merge (non-empty) **********************/ \
	X(incremental_topo_config_partial_praid_update,	"Topo config partial praid update",			"1 vol, 1 praid updated via chunk hash => merged correctly") \
	X(incremental_topo_config_mixed_keep_and_update, "Topo config mixed keep and update",		"2 praids, 1 updated + 1 kept from hash via follower re-serialize") \
	X(incremental_topo_config_vol_deleted,			"Topo config vol deleted",					"2 vols, 1 being deleted => incremental has 1 vol, merge succeeds") \
	X(incremental_topo_config_vol_added,			"Topo config vol added",					"1 vol exists, incremental has 2 vols => merge succeeds with 2") \
	/********** Leader incremental selection: deletion forces complete ***********/ \
	X(deletion_guard_forces_complete_configs,		"Vol deletion forces complete configs",		"Peer in window but last_delete_kafka > peer offset => complete") \
	X(deletion_guard_no_effect_when_peer_caught_up,	"Caught-up peer still gets incremental",	"Peer kafka offset >= last_delete => incremental allowed") \
	X(selection_topo_window_boundary,				"Topo window boundary",						"Peer just below topo window => complete, at boundary => incremental") \
	X(selection_topo_config_window_boundary,		"Topo config window boundary",				"Peer just below topo_config window => complete, at boundary => incremental") \
	X(selection_kafka_window_boundary,				"Kafka window boundary",					"Peer just below kafka window => complete, at boundary => incremental") \
	X(selection_raft_members_seq_window_boundary,	"Raft members seq window boundary",			"Peer just below members seq window => complete, at boundary => incremental") \
	X(selection_old_peer_version_forces_complete,	"Old peer version forces complete",			"Old peer stays complete even when indices qualify for incremental") \
	X(selection_supported_peer_version_allows_incremental, "Supported peer version allows incremental", "Peer at support threshold may use incremental when indices qualify") \
	/****** compare_persist_and_wire_bufs_tlvs_excl_raft_ctx direct tests *******/ \
	X(compare_both_null_returns_equal,				"Compare: both NULL => EQUAL",				"Both NULL inputs return PERSIST_AND_WIRE_BUF_DIFF_EQUAL (0)") \
	X(compare_one_null_returns_topo_and_configs,	"Compare: one NULL => TOPO_AND_CONFIGS",	"One NULL input returns TOPO_AND_CONFIGS (2)") \
	X(compare_equal_bufs_returns_equal,				"Compare: equal bufs => EQUAL",				"All TLV indices match => returns EQUAL (0)") \
	X(compare_topo_only_diff,						"Compare: topo only diff => TOPO_ONLY",		"Only topo_idx differs => returns TOPO_ONLY (1)") \
	X(compare_topo_config_only_diff,				"Compare: topo_config diff => TOPO_AND_CONFIGS", "Only topo_config_idx differs => returns TOPO_AND_CONFIGS (2)") \
	X(compare_kafka_config_only_diff,				"Compare: kafka_config diff => TOPO_AND_CONFIGS", "Only kafka_config_idx differs => returns TOPO_AND_CONFIGS (2)") \
	X(compare_raft_members_only_diff,				"Compare: raft_members diff => TOPO_AND_CONFIGS", "Only raft_members_idx differs => returns TOPO_AND_CONFIGS (2)") \
	X(compare_topo_and_config_diff,					"Compare: topo+config diff => TOPO_AND_CONFIGS", "Both topo and config differ => returns TOPO_AND_CONFIGS (2)") \
	/****** CRC and length validation tests ************************************/ \
	X(crc_corrupted_topo_data_fails,				"CRC: corrupted topo data fails",			"Flipping a byte in topo section data fails CRC check") \
	X(crc_corrupted_raft_ctx_fails,					"CRC: corrupted raft_ctx fails",			"Corrupting raft_ctx CRC field fails validation") \
	X(crc_length_mismatch_fails,					"CRC: length mismatch fails",				"Passing wrong data_len fails length check") \
	X(generate_buf_crc_valid,						"Generate: CRC valid",						"Generated complete buf passes CRC and length validation") \
	X(follower_merge_produces_valid_crc,			"Follower merge: valid CRC after merge",	"Merged follower buf passes CRC and length validation") \
	/****** Merge error paths: old is incremental (not complete) ****************/ \
	X(merge_topo_old_incremental_fails,				"Merge: topo old incremental => -1",		"Old TOPO_INCREMENTAL + upd TOPO_INCREMENTAL => merge returns -1") \
	X(merge_topo_config_old_incremental_fails,		"Merge: topo_config old incremental => -1",	"Old TOPO_CONFIG_INCREMENTAL + upd => merge returns -1") \
	X(merge_kafka_config_old_incremental_fails,		"Merge: kafka old incremental => -1",		"Old KAFKA_MGMT_CONFIG_INCREMENTAL + upd => merge returns -1") \
	X(merge_raft_members_old_incremental_fails,		"Merge: raft_members old incremental => -1", "Old RAFT_MEMBERS_INCREMENTAL + upd => merge returns -1") \
	/********** Follower realloc_and_upd orchestration **************************/ \
	X(topo_incremental_configs_complete_inplace,	"Topo incremental + complete configs",		"Leader mixed buffer shape updates topo in place while configs stay complete") \
	X(all_sections_incremental_full_merge,			"All sections incremental full merge",		"Incoming all-incremental wire buf merges all sections into complete follower state") \
	X(first_update_with_raft_log,        "First update with raft log",        "old=NULL, raft_log=true => full memcpy of upd") \
	X(first_update_without_raft_log,     "First update without raft log",     "old=NULL, raft_log=false => only raft_ctx copied") \
	X(equal_bufs_only_raft_ctx_updated,  "Equal bufs updates raft_ctx only",  "All idx match => dst==old, raft_ctx+sw_ver updated") \
	X(no_raft_log_keeps_old,             "No raft log keeps old",             "is_with_raft_log=false => dst==old regardless of idx diff") \
	X(topo_only_same_size_inplace,       "Topo-only same size in-place",      "Only topo idx differs, same size => in-place merge") \
	X(topo_only_diff_size_realloc,       "Topo-only diff size realloc",       "Only topo idx differs, different size => full alloc") \
	X(full_alloc_topo_and_configs,       "Full alloc topo and configs",       "Multiple sections differ => new alloc, old freed") \
	X(error_in_pass1_keeps_old,          "Error in pass1 keeps old buf",      "Bogus TLV type => merge returns -1 => dst==old") \
	/********** Leader wire buf generation verification ************************/ \
	X(generate_complete_buf_types_correct,			"Generate: complete types correct",			"All-complete generation sets correct TLV types and lengths") \
	X(generate_incremental_buf_types_correct,		"Generate: incremental types correct",		"All-incremental generation sets correct TLV types and lengths") \
	X(generate_mixed_buf_types_correct,				"Generate: mixed types correct",			"Topo incremental + configs complete sets correct TLV types")

int wire_buf_test_main(int argc, char *argv[]);

#endif // #ifndef WIRE_BUF_TEST_H
