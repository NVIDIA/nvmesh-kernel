/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_client_core_ops.h"

#if defined(__KERNEL__) || (defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR)

#include "nvmeibc_pausable.h"

static int __run_cmpxchg(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	return nvmeibc_pd_cmpxchg(disk, handle, addr, comp);
}

static int __run_read_lock(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	return nvmeibc_pd_read_lock(disk, handle, addr, comp);
}

static int __jmdc_read(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp *comp)
{
	(void)self;
	return nvmeibc_pd_jmdc_read(disk, comp);
}

static int __free_jrnl_ents(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	(void)self;
	return nvmeibc_pd_free_jrnl_ents(disk, comp);
}

static int __dbg_please_kill_yourself(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void (*cb)(void*), void *ctx, int rsc_id, u64 dlba)
{
	(void)self;
	return nvmeibc_pd_dbg_please_kill_yourself(disk, cb, ctx, rsc_id, dlba);
}

static int __write_blkset_info(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	return nvmeibc_pd_write_blkset_info(disk, handle, addr, comp);
}

static int __get_blkset_problems(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 start, u64 length, struct nvmeibc_d_rdma_comp *dc)
{
	(void)self;
	return nvmeibc_pd_get_blkset_problems(disk, handle, start, length, dc);
}

static int __execute_io_blocks(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *cmd)
{
	(void)self;
	return nvmeibc_pd_execute_io_blocks(disk, cmd);
}

static int __execute_io_jour_blocks(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *cmd)
{
	(void)self;
	return nvmeibc_pd_execute_io_jour_blocks(disk, cmd);
}

static int __execute_gen(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_gen_cmd *cmd)
{
	(void)self;
	return nvmeibc_pd_execute_gen(disk, cmd);
}

static void __cb_called_comp(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	nvmeibc_pd_cb_called_comp(disk, comp);
}

static void __cb_called_cmd(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_command *disk_cmd)
{
	(void)self;
	nvmeibc_pd_cb_called_cmd(disk, disk_cmd);
}

static void __cb_called_jmdc(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp *comp)
{
	(void)self;
	nvmeibc_pd_cb_called_jmdc(disk, comp);
}

static void __cb_called_free_jrnl_ents(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	(void)self;
	nvmeibc_pd_cb_called_free_jrnl_ents(disk, comp);
}

static void __dump_transfers(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk)
{
	(void)self;
	nvmeibc_pd_dump_transfers(disk);
}

static int __tostring(struct nvmeibc_icore_ops const* self, const struct nvmeibc_disk *disk, char *buf, int buf_len)
{
	(void)self;
	return nvmeibc_pd_tostring(disk, buf, buf_len);
}

static int __jam_get(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk)
{
	(void)self;
	return nvmeibc_pd_jam_get(disk);
}

static void __jam_put(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk)
{
	(void)self;
	nvmeibc_pd_jam_put(disk);
}

static int __jam_get_all(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_disk *disks[])
{
	(void)self;
	return nvmeibc_pd_jam_get_all(n_disks, disks);
}

static void __jam_put_all(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_disk *disks[])
{
	(void)self;
	nvmeibc_pd_jam_put_all(n_disks, disks);
}

static int __toma_send(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, u64 handle, struct nvmeibc_disk_toma_send_params *params)
{
	(void)self;
	return nvmeibc_pd_toma_send(disk, handle, params);
}

static int __toma_unsubscribe(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, u64 handle)
{
	(void)self;
	return nvmeibc_pd_toma_unsubscribe(disk, handle);
}

static int __reused_bb_release(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeib_data_reuse_buf_params *p)
{
	(void)self;
	return nvmeibc_pd_reused_bb_release(disk, p);
}

struct nvmeibc_icore_ops const core_ops = {
	.run_cmpxchg = __run_cmpxchg,
	.run_read_lock = __run_read_lock,
	.jmdc_read = __jmdc_read,
	.free_jrnl_ents = __free_jrnl_ents,
	.dbg_please_kill_yourself = __dbg_please_kill_yourself,
	.write_blkset_info = __write_blkset_info,
	.get_blkset_problems = __get_blkset_problems,
	.execute_io_blocks = __execute_io_blocks,
	.execute_io_jour_blocks = __execute_io_jour_blocks,
	.execute_gen = __execute_gen,
	.cb_called_comp = __cb_called_comp,
	.cb_called_cmd = __cb_called_cmd,
	.cb_called_jmdc = __cb_called_jmdc,
	.cb_called_free_jrnl_ents = __cb_called_free_jrnl_ents,
	.dump_transfers = __dump_transfers,
	.tostring = __tostring,
	.jam_get = __jam_get,
	.jam_put = __jam_put,
	.jam_get_all = __jam_get_all,
	.jam_put_all = __jam_put_all,
	.toma_send = __toma_send,
	.toma_unsubscribe = __toma_unsubscribe,
	.reused_bb_release = __reused_bb_release,
};

struct nvmeibc_icore_ops const* nvmeibc_core_ops_get(void){
	return &core_ops;
}

#else

	static_assert(0, "client core operations are not defined for this platform")

#endif