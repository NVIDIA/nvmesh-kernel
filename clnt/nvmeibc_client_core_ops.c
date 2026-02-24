/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_client_core_ops.h"

#if defined(__KERNEL__) || (defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR)

#include "nvmeibc_pausable.h"

static int __run_cmpxchg(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	return nvmeibc_pd_cmpxchg(nvmeibc_disk_from_base(disk), handle, addr, comp);
}

static int __run_read_lock(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	return nvmeibc_pd_read_lock(nvmeibc_disk_from_base(disk), handle, addr, comp);
}

static int __jmdc_read(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_jmdc_read_comp *comp)
{
	(void)self;
	return nvmeibc_pd_jmdc_read(nvmeibc_disk_from_base(disk), comp);
}

static int __free_jrnl_ents(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	(void)self;
	return nvmeibc_pd_free_jrnl_ents(nvmeibc_disk_from_base(disk), comp);
}

static int __dbg_please_kill_yourself(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void (*cb)(void*), void *ctx, int rsc_id, u64 dlba)
{
	(void)self;
	return nvmeibc_pd_dbg_please_kill_yourself(nvmeibc_disk_from_base(disk), cb, ctx, rsc_id, dlba);
}

static int __write_blkset_info(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	return nvmeibc_pd_write_blkset_info(nvmeibc_disk_from_base(disk), handle, addr, comp);
}

static int __get_blkset_problems(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, void *handle, u64 start, u64 length, struct nvmeibc_d_rdma_comp *dc)
{
	(void)self;
	return nvmeibc_pd_get_blkset_problems(nvmeibc_disk_from_base(disk), handle, start, length, dc);
}

static int __execute_io_blocks(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_io_command *cmd)
{
	(void)self;
	return nvmeibc_pd_execute_io_blocks(nvmeibc_disk_from_base(disk), cmd);
}

static int __execute_io_jour_blocks(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_io_command *cmd)
{
	(void)self;
	return nvmeibc_pd_execute_io_jour_blocks(nvmeibc_disk_from_base(disk), cmd);
}

static int __execute_gen(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_gen_cmd *cmd)
{
	(void)self;
	return nvmeibc_pd_execute_gen(nvmeibc_disk_from_base(disk), cmd);
}

static void __cb_called_comp(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_d_rdma_comp *comp)
{
	(void)self;
	nvmeibc_pd_cb_called_comp(nvmeibc_disk_from_base(disk), comp);
}

static void __cb_called_cmd(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_command *disk_cmd)
{
	(void)self;
	nvmeibc_pd_cb_called_cmd(nvmeibc_disk_from_base(disk), disk_cmd);
}

static void __cb_called_jmdc(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_jmdc_read_comp *comp)
{
	(void)self;
	nvmeibc_pd_cb_called_jmdc(nvmeibc_disk_from_base(disk), comp);
}

static void __cb_called_free_jrnl_ents(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	(void)self;
	nvmeibc_pd_cb_called_free_jrnl_ents(nvmeibc_disk_from_base(disk), comp);
}

static void __dump_transfers(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk)
{
	(void)self;
	nvmeibc_pd_dump_transfers(nvmeibc_disk_from_base(disk));
}

static int __tostring(struct nvmeibc_icore_ops const* self, const struct nvmeibc_idisk *disk, char *buf, int buf_len)
{
	(void)self;
	return nvmeibc_pd_tostring(nvmeibc_disk_from_base(disk), buf, buf_len);
}

static int __jam_get(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk)
{
	(void)self;
	return nvmeibc_pd_jam_get(nvmeibc_disk_from_base(disk));
}

static void __jam_put(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk)
{
	(void)self;
	nvmeibc_pd_jam_put(nvmeibc_disk_from_base(disk));
}

static int __jam_get_all(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_idisk *disks[])
{
	(void)self;
	BUILD_BUG_ON(offsetof(struct nvmeibc_disk, base) != 0);
	BUILD_BUG_ON_MSG(0 == __builtin_types_compatible_p(typeof(((struct nvmeibc_disk*)(NULL))->base), struct nvmeibc_idisk), "base should be the first member variable");
	return nvmeibc_pd_jam_get_all(n_disks, (struct nvmeibc_disk **)disks);
}

static void __jam_put_all(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_idisk *disks[])
{
	(void)self;
	BUILD_BUG_ON(offsetof(struct nvmeibc_disk, base) != 0);
	BUILD_BUG_ON_MSG(0 == __builtin_types_compatible_p(typeof(((struct nvmeibc_disk*)(NULL))->base), struct nvmeibc_idisk), "base should be the first member variable");
	nvmeibc_pd_jam_put_all(n_disks, (struct nvmeibc_disk **)disks);
}

static int __toma_send(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, u64 handle, struct nvmeibc_disk_toma_send_params *params)
{
	(void)self;
	return nvmeibc_pd_toma_send(nvmeibc_disk_from_base(disk), handle, params);
}

static int __toma_subscribe(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, u64 handle, struct nvmeibc_disk_subscription_params *params)
{
	(void)self;
	return nvmeibc_disk_subscribe_toma_service(nvmeibc_disk_from_base(disk), handle, params);
}

static int __toma_unsubscribe(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, u64 handle)
{
	(void)self;
	return nvmeibc_pd_toma_unsubscribe(nvmeibc_disk_from_base(disk), handle);
}

static int __reused_bb_release(struct nvmeibc_icore_ops const* self, struct nvmeibc_idisk *disk, struct nvmeib_data_reuse_buf_params *p)
{
	(void)self;
	return nvmeibc_pd_reused_bb_release(nvmeibc_disk_from_base(disk), p);
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
	.toma_subscribe = __toma_subscribe,
	.toma_unsubscribe = __toma_unsubscribe,
	.reused_bb_release = __reused_bb_release,
};

struct nvmeibc_icore_ops const* nvmeibc_core_ops_get(void){
	return &core_ops;
}

#else

	static_assert(0, "client core operations are not defined for this platform")

#endif
