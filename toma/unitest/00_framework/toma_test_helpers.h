/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/**
 * toma_test_helpers.h - Shared TEST_ function declarations for component tests
 *
 * Collects extern declarations for all TEST_ wrappers used by test suites
 * in 91_tests_components/. Each TEST_ function is defined in the production
 * source file that owns the state it accesses.
 */
#include "nvmeibt_persistency_info.h"
#include "nvmeibt_kafka.h"		// For rd_kafka_resp_err_t

/* Test environment init — 00_framework/toma_test_helpers.c */
extern void TEST_init(void);

/* Sandbox controls — unitest/toma_in_sandbox.c and unitest/kafka/sandbox_kafka.c */
extern void TEST_set_nm_remote_nodes_connected(bool is_connected);

/* Kafka real-time error visibility — nvmeibt_kafka.c */
extern void check_if_kafka_init_preserve_state_vars_required(rd_kafka_resp_err_t err);
extern volatile int64_t kafka_last_transient_err_boot_sec;
#include "../kafka/sandbox_kafka_internal.h"

/* RPC command dispatch — nvmeibt_rpc.c */
extern int TEST_nvmeibt_rpc_handle_command(char *in, struct nvmeibt_Str *out);

/* Praid hash — nvmeibt_praid.c */
extern void TEST_init_praids_hash(void);
extern void TEST_add_praid_to_hash(const union nvmeib_uuid *uuid, int64_t topo_idx_updated,
								   int praid_version_major, int praid_version_minor);
extern void TEST_set_praid_conf_corrupted(const union nvmeib_uuid *uuid);

/* Raft members hash — nvmeibt_raft.c */
extern void TEST_init_raft_members_hash(void);
extern void TEST_add_raft_member_to_hash(const union nvmeib_uuid *uuid, const char *hostname,
										 int64_t seq_no_updated, int64_t kafka_offset);
extern void TEST_remove_raft_member_from_hash(const union nvmeib_uuid *uuid);

/* Blkdev hash — nvmeibt_global.c */
extern void TEST_init_blkdevs_hash(void);
extern void TEST_add_blkdev_to_hash(const union nvmeib_uuid *uuid, int version,
									const void *wire_buf, int wire_len,
									bool is_being_deleted);

/* Chunk hash — nvmeibt_praid.c */
extern void TEST_init_chunks_hash(void);
extern void TEST_add_chunk_to_hash(const union nvmeib_uuid *chunk_uuid,
	int n_praid_uuids, const union nvmeib_uuid *praid_uuids);

/* Disk hash — nvmeibt_disk.c */
extern void TEST_add_disk_to_hash(const union nvmeib_uuid *uuid, bool is_drive_write_error);
extern void TEST_remove_disk_from_hash(const union nvmeib_uuid *uuid);

/* Local disk hash — nvmeibt_local_disk.c */
extern void TEST_add_local_disk_to_hash(const char *ldisk_id_str, bool is_excluded, bool is_drive_write_error);
extern void TEST_add_not_ready_local_disk_to_hash(const char *ldisk_id_str, int n_segments);
extern void TEST_remove_local_disk_from_hash(const char *ldisk_id_str);

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

/* Compare wire bufs (excl raft_ctx) — nvmeibt_raft.c */
extern int TEST_compare_persist_and_wire_bufs(const struct nvmeibt_persist_and_wire_buf *b1,
											  const struct nvmeibt_persist_and_wire_buf *b2);

/* CRC and length validation — nvmeibt_raft.c */
extern bool TEST_is_persist_and_wire_buf_crc_and_len_ok(struct nvmeibt_persist_and_wire_buf *buf,
														int data_len);

/* Validation — nvmeibt_raft.c (non-static, no header decl) */
extern void persist_and_wire_buf_validate_len(const struct nvmeibt_persist_and_wire_buf *b);
