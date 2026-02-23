/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_ICORE_OPS_H
#define NVMEIBC_ICORE_OPS_H

#include "kr_incs.h"
#include "nvmeibc_idisk.h"

struct nvmeibc_d_rdma_comp;
struct nvmeibc_disk_jmdc_read_comp;
struct nvmeibc_disk_free_jrnl_ents_comp;
struct nvmeibc_disk_io_command;
struct nvmeibc_disk_gen_cmd;
struct nvmeibc_disk_command;
struct nvmeibc_disk_toma_send_params;
struct nvmeib_data_reuse_buf_params;
struct nvmeibc_icore_ops;

//the following struct defines "core" layer functionality towards the block layer
struct nvmeibc_icore_ops {
	__attribute__((nonnull (1,2,5)))
	int (*run_cmpxchg)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp);
	__attribute__((nonnull (1,2,5)))
	int (*run_read_lock)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp);
	__attribute__((nonnull (1,2,3)))
	int (*jmdc_read)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_jmdc_read_comp *comp);
	__attribute__((nonnull (1,2,3)))
	int (*free_jrnl_ents)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp);
	__attribute__((nonnull (1,2,3)))
	int (*dbg_please_kill_yourself)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void (*cb)(void*), void *ctx, int rsc_id, u64 dlba);
	__attribute__((nonnull (1,2,5)))
	int (*write_blkset_info)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp);
	__attribute__((nonnull (1,2,6)))
	int (*get_blkset_problems)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 start, u64 length, struct nvmeibc_d_rdma_comp *dc);
	__attribute__((nonnull (1,2,3)))
	int (*execute_io_blocks)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_io_command *cmd);
	__attribute__((nonnull (1,2,3)))
	int (*execute_io_jour_blocks)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_io_command *cmd);
	__attribute__((nonnull (1,2,3)))
	int (*execute_gen)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_gen_cmd *cmd);
	__attribute__((nonnull (1,2,3)))
	void (*cb_called_comp)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_d_rdma_comp *comp);
	__attribute__((nonnull (1,2,3)))
	void (*cb_called_cmd)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_command *disk_cmd);
	__attribute__((nonnull (1,2,3)))
	void (*cb_called_jmdc)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_jmdc_read_comp *comp);
	__attribute__((nonnull (1,2,3)))
	void (*cb_called_free_jrnl_ents)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp);
	__attribute__((nonnull (1,2)))
	void (*dump_transfers)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk);
	__attribute__((nonnull (1,2,3)))
	int (*tostring)(struct nvmeibc_icore_ops const* self, const struct nvmeibc_idisk *disk, char *buf, int buf_len);
	__attribute__((nonnull (1,2)))
	int (*jam_get)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk);
	__attribute__((nonnull (1,2)))
	void (*jam_put)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk);
	__attribute__((nonnull (1,3)))
	int (*jam_get_all)(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_idisk *disks[]);
	__attribute__((nonnull (1,3)))
	void (*jam_put_all)(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_idisk *disks[]);
	__attribute__((nonnull (1,2,4)))
	int (*toma_send)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, u64 handle, struct nvmeibc_disk_toma_send_params *params);
	__attribute__((nonnull (1,2)))
	int (*toma_unsubscribe)(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, u64 handle);
	__attribute__((nonnull (1,2,3)))
	int (*reused_bb_release)(struct nvmeibc_icore_ops const *self, struct nvmeibc_idisk *disk, struct nvmeib_data_reuse_buf_params *p);
};

//the function is just a placeholder, every platform will have to implement it:wa
struct nvmeibc_icore_ops const* nvmeibc_core_ops_get(void);

#endif // NVMEIBC_ICORE_OPS_H
