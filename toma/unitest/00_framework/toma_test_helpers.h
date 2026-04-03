/**
 * toma_test_helpers.h - Shared TEST_ function declarations for component tests
 *
 * Collects extern declarations for all TEST_ wrappers used by test suites
 * in 91_tests_components/. Each TEST_ function is defined in the production
 * source file that owns the state it accesses.
 */

#ifndef TOMA_TEST_HELPERS_H
#define TOMA_TEST_HELPERS_H

#include "nvmeibt_persistency_info.h"

/* Test environment init — 00_framework/toma_test_helpers.c */
extern void TEST_init(void);

/* Praid hash — nvmeibt_praid.c */
extern void TEST_add_praid_to_hash(const union nvmeib_uuid *uuid, int64_t topo_idx_updated,
								   int praid_version_major, int praid_version_minor);
extern void TEST_clear_praids_hash(void);

/* Raft members hash — nvmeibt_raft.c */
extern void TEST_init_raft_members_hash(void);
extern void TEST_add_raft_member_to_hash(const union nvmeib_uuid *uuid, const char *hostname,
										 int64_t seq_no_updated, int64_t kafka_offset);
extern void TEST_clear_raft_members_hash(void);

/* Blkdev hash — nvmeibt_global.c */
extern void TEST_add_blkdev_to_hash(const union nvmeib_uuid *uuid, int version,
									const void *wire_buf, int wire_len);
extern void TEST_clear_blkdevs_hash(void);

/* Incremental selection — nvmeibt_raft.c */
extern int TEST_is_peer_incremental_wire_buf_supported(uint32_t peer_sw_ver);
extern int TEST_is_configs_incremental_allowed_for_peer_sw_ver(
	uint32_t peer_sw_ver,
	int64_t peer_topo_idx, int64_t peer_topo_config_idx,
	int64_t peer_kafka_mgmt_config_offset, int64_t peer_raft_members_seq_no,
	int64_t peer_raft_members_kafka_offset,
	int64_t leader_topo_to_commit, int64_t leader_topo_config_to_commit,
	int64_t leader_kafka_mgmt_config_to_commit, int64_t leader_raft_members_seq_no_to_commit,
	int64_t last_delete_kafka_mgmt_config_offset, int64_t last_delete_raft_members_kafka_offset);
extern int TEST_compute_is_configs_incremental(
	int64_t peer_topo_idx, int64_t peer_topo_config_idx,
	int64_t peer_kafka_mgmt_config_offset, int64_t peer_raft_members_seq_no,
	int64_t peer_raft_members_kafka_offset,
	int64_t leader_topo_to_commit, int64_t leader_topo_config_to_commit,
	int64_t leader_kafka_mgmt_config_to_commit, int64_t leader_raft_members_seq_no_to_commit,
	int64_t last_delete_kafka_mgmt_config_offset, int64_t last_delete_raft_members_kafka_offset);

/* Per-section merge — nvmeibt_raft.c */
extern int TEST_raft_merge_data_to_section(struct nvmeibt_wire_type_len_value *dst_wire_ctx,
	const struct nvmeibt_wire_type_len_value *old_wire_ctx,
	const struct nvmeibt_wire_type_len_value *upd_wire_ctx,
	char **dst_data_ptr, char **old_data_ptr, const char **upd_data_ptr);

/* Realloc and update — nvmeibt_raft.c */
extern struct nvmeibt_persist_and_wire_buf *TEST_realloc_and_upd_follower_persist_and_wire_bufs(
	struct nvmeibt_persist_and_wire_buf *old,
	const struct nvmeibt_persist_and_wire_buf *upd,
	bool is_with_raft_log);

/* Validation — nvmeibt_raft.c (non-static, no header decl) */
extern void persist_and_wire_buf_validate_len(const struct nvmeibt_persist_and_wire_buf *b);

#endif /* TOMA_TEST_HELPERS_H */
