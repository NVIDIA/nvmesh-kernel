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
/* Test environment init — 00_framework/toma_test_helpers.c */
extern void TEST_init(void);

/* Sandbox controls — unitest/toma_in_sandbox.c and unitest/kafka/sandbox_kafka.c */
extern void TEST_set_nm_remote_nodes_connected(bool is_connected);

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
