/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

#include "common/kr_incs.h"
#include "nvmeibc_block_dp_dbgdi_tests.h"

#ifndef DBGDI_REMOVED_IN_PRODUCTION

#include "block/datapath_mirror/nvmeibc_block_dp_mirror.h"
#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_blk.h"
#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_stages.h"
#include "block/nvmeibc_block_common.h"
#include "block/nvmeibc_topology.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "nvmeibc_block.h"

#include <string.h>

/* dbgdi_log capacity is log.buf[]; sizeof(struct dbgdi_log) is header + buf + any padding — never
 * substitute DBG_DI_INJ_SPACE or sizeof(log) when meaning "buffer bytes" only. */
static inline void ut_dbgdi_log_clear(struct dbgdi_log *log)
{
	memset(log, 0, sizeof(*log));
}

static inline void ut_dbgdi_log_init_clean(struct dbgdi_log *log)
{
	ut_dbgdi_log_clear(log);
	dbgdi_log_init(log, (int)sizeof(log->buf));
}

static void ut_setup_cmd_data(struct nvmeibc_block_command *bcmd,
			      struct nvmeibc_disk_io_command *ioc,
			      struct nvmeib_data_buffer *ndb,
			      struct scatterlist *sg,
			      void *buf,
			      enum nvmeib_block_io_op op)
{
	memset(bcmd, 0, sizeof(*bcmd));
	memset(ioc, 0, sizeof(*ioc));
	memset(ndb, 0, sizeof(*ndb));

	sg_init_one(sg, buf, NVMEIBC_SECTOR_SIZE);
	ndb->table.sgl = sg;
	ndb->table.nents = 1;
	ndb->length = NVMEIBC_SECTOR_SIZE;

	ioc->reqs1.ndb = ndb;
	ioc->reqs1.op = op;
	ioc->reqs1.do_512b_sub_block_x = 0;

	bcmd->iocmd = ioc;
	bcmd->nlbas = 1;
}

static void test_dbgdi_log_roundtrip_and_corrupt_headers(void)
{
	struct dbgdi_log log;
	struct t_db_who_mark mark = {.dbg_di_magic = 0};
	char out[256];
	int rc;

	ut_dbgdi_log_init_clean(&log);
	rc = dbgdi_log_add_rec(&log, (void *)&mark, DBG_DI_SYNC_OVERWRITTEN_CLEARED, DBG_DI_MARK_REC_SIZE);
	BUG_ON(rc != 0);
	rc = dbgdi_log_get_record_by_type(&log, DBG_DI_SYNC_OVERWRITTEN_CLEARED, out, sizeof(out));
	BUG_ON(rc != 0);

	rc = dbgdi_log_get_record_by_type(&log, DBG_DI_WRITE, out, sizeof(out));
	BUG_ON(rc != -1);

	ut_dbgdi_log_init_clean(&log);
	log.header.head = 5000;
	rc = dbgdi_log_get_record_by_type(&log, DBG_DI_WRITE, out, sizeof(out));
	BUG_ON(rc != -1);

	ut_dbgdi_log_init_clean(&log);
	log.header.size = (u32)sizeof(log.buf) + 1;
	rc = dbgdi_log_get_record_by_type(&log, DBG_DI_WRITE, out, sizeof(out));
	BUG_ON(rc != -1);

	ut_dbgdi_log_clear(&log);
	log.header.magic = 0;
	rc = dbgdi_log_get_record_by_type(&log, DBG_DI_WRITE, out, sizeof(out));
	BUG_ON(rc != -1);

	ut_dbgdi_log_init_clean(&log);
	BUG_ON(dbgdi_log_iter_init(&log) != -1);

	ut_dbgdi_log_init_clean(&log);
	dbgdi_log_add_rec(&log, (void *)&mark, DBG_DI_SYNC_OVERWRITTEN_CLEARED, DBG_DI_MARK_REC_SIZE);
	BUG_ON(dbgdi_log_iter_init(&log) == -1);
}

static void test_dbgdi_log_zero_length_record_returns_error(void)
{
	struct dbgdi_log log;
	struct dbgdi_log_entry *e;
	char out[256];
	int rc;

	ut_dbgdi_log_init_clean(&log);
	e = (void *)&log.buf[0];
	e->type = 0xff;
	e->size = 0;
	log.header.tail = 0;
	log.header.head = sizeof(*e);
	log.header.full = 0;

	rc = dbgdi_log_get_record_by_type(&log, DBG_DI_WRITE, out, sizeof(out));
	BUG_ON(rc != -1);
}

static void test_dbgdi_log_stride_hop_bound(void)
{
	struct dbgdi_log log;
	struct dbgdi_log_entry *e;
	char out[256];
	int rc;

	ut_dbgdi_log_init_clean(&log);
	/* tail..head orbit steps by sizeof(*e); head=1 is unreachable — hop bound must fire */
	e = (void *)&log.buf[0];
	e->type = DBG_DI_READ;
	e->size = sizeof(*e);
	log.header.tail = 0;
	log.header.head = 1;
	log.header.full = 0;

	rc = dbgdi_log_get_record_by_type(&log, DBG_DI_WRITE, out, sizeof(out));
	BUG_ON(rc != -1);
}

static void test_predicate_3way_mirror(void)
{
	static struct nvmeibc_raid1 r1 = {
		.slice_size = 1,
		.replicas = 3,
	};
	static struct nvmeibc_chunk ch = {.raid1s = &r1};
	static struct nvmeibc_subscription_ctx tr = {.r1 = 0};
	static struct nvmeibc_disk_segment ds = {.chunk = &ch, .toma_reg = &tr};
	static struct nvmeibc_block_command cmds[3];
	int i;

	memset(cmds, 0, sizeof(cmds));
	for (i = 0; i < 3; i++) {
		cmds[i].ds = &ds;
		cmds[i].cmdarr = cmds;
		cmds[i].my_leader = 0;
		cmds[i].my_stage = E_CMDS_STAGE_DO_IO_AND_PAR;
	}
	cmds[0].nraid_siblings = 3;

	/* New semantics: every leg of a multi-leg mirror is unsafe to mutate. */
	BUG_ON(dp_dbgdi_can_mutate_shared_buf(&cmds[0]));
	BUG_ON(dp_dbgdi_can_mutate_shared_buf(&cmds[1]));
	BUG_ON(dp_dbgdi_can_mutate_shared_buf(&cmds[2]));
}

static void test_predicate_2way_mirror_and_core_disabled_for_writes(void)
{
	static struct nvmeibc_raid1 r1 = {.slice_size = 1, .replicas = 2};
	static struct nvmeibc_chunk ch = {.raid1s = &r1};
	static struct nvmeibc_subscription_ctx tr = {.r1 = 0};
	static struct nvmeibc_disk_segment ds = {.chunk = &ch, .toma_reg = &tr};
	static struct nvmeibc_block_command cmds[2];
	static struct nvmeibc_block_device nd;
	static struct operation o;
	static struct nvmeibc_disk_io_command ioc[2];
	int i;

	memset(cmds, 0, sizeof(cmds));
	memset(&nd, 0, sizeof(nd));
	memset(&o, 0, sizeof(o));
	memset(ioc, 0, sizeof(ioc));

	nd.dp.enable_di_debug_mode = true;
	o.nd = &nd;

	for (i = 0; i < 2; i++) {
		cmds[i].o = &o;
		cmds[i].ds = &ds;
		cmds[i].cmdarr = cmds;
		cmds[i].my_leader = 0;
		cmds[i].my_stage = E_CMDS_STAGE_DO_IO_AND_PAR;
	}
	cmds[0].nraid_siblings = 2;

	ioc[0].comp.cmd = &cmds[0];
	ioc[0].reqs1.op = NVMEIB_BLOCK_IO_OP_WRITE;
	ioc[1].comp.cmd = &cmds[1];
	ioc[1].reqs1.op = NVMEIB_BLOCK_IO_OP_WRITE;

	/* New semantics: 2-way mirror writes are also unsafe to mutate (same race surface). */
	BUG_ON(dp_dbgdi_can_mutate_shared_buf(&cmds[0]));
	BUG_ON(dp_dbgdi_can_mutate_shared_buf(&cmds[1]));

	/* core_pre/post is disabled for all mirror write legs. */
	BUG_ON(dp_dbgdi_should_add_info_core(&ioc[0]));
	BUG_ON(dp_dbgdi_should_add_info_core(&ioc[1]));
}

static void test_predicate_jbod_short_circuit(void)
{
	static struct nvmeibc_raid1 r1 = {.slice_size = 1, .replicas = 1};
	static struct nvmeibc_chunk ch = {.raid1s = &r1};
	static struct nvmeibc_subscription_ctx tr = {.r1 = 0};
	static struct nvmeibc_disk_segment ds = {.chunk = &ch, .toma_reg = &tr};
	static struct nvmeibc_block_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.ds = &ds;
	/* JBOD has a single leg — no shared buffer, mutation is safe. */
	BUG_ON(!dp_dbgdi_can_mutate_shared_buf(&cmd));
}

static void test_predicate_ec_short_circuit(void)
{
	static struct nvmeibc_raid1 r1 = {.slice_size = 2, .replicas = 5};
	static struct nvmeibc_chunk ch = {.raid1s = &r1};
	static struct nvmeibc_subscription_ctx tr = {.r1 = 0};
	static struct nvmeibc_disk_segment ds = {.chunk = &ch, .toma_reg = &tr};
	static struct nvmeibc_block_command cmds[2];
	static struct nvmeibc_block_device nd;
	static struct operation o;
	static struct nvmeibc_disk_io_command ioc[1];
	int i;

	memset(cmds, 0, sizeof(cmds));
	memset(&nd, 0, sizeof(nd));
	memset(&o, 0, sizeof(o));
	memset(ioc, 0, sizeof(ioc));

	nd.dp.enable_di_debug_mode = true;
	o.nd = &nd;

	for (i = 0; i < 2; i++) {
		cmds[i].o = &o;
		cmds[i].ds = &ds;
		cmds[i].cmdarr = cmds;
		cmds[i].my_leader = 0;
		cmds[i].my_stage = E_CMDS_STAGE_DO_IO_AND_PAR;
	}
	cmds[0].nraid_siblings = 2;

	/* EC has per-leg buffers — mutation is safe on every leg. */
	BUG_ON(!dp_dbgdi_can_mutate_shared_buf(&cmds[1]));

	ioc[0].comp.cmd = &cmds[1];
	ioc[0].reqs1.op = NVMEIB_BLOCK_IO_OP_WRITE;
	BUG_ON(!dp_dbgdi_should_add_info_core(&ioc[0]));
}

static void test_should_add_info_core_read_not_gated_by_leg(void)
{
	static struct nvmeibc_raid1 r1 = {.slice_size = 1, .replicas = 3};
	static struct nvmeibc_chunk ch = {.raid1s = &r1};
	static struct nvmeibc_subscription_ctx tr = {.r1 = 0};
	static struct nvmeibc_disk_segment ds = {.chunk = &ch, .toma_reg = &tr};
	static struct nvmeibc_block_command cmds[3];
	static struct nvmeibc_block_device nd;
	static struct operation o;
	static struct nvmeibc_disk_io_command ioc;
	int i;

	memset(cmds, 0, sizeof(cmds));
	memset(&nd, 0, sizeof(nd));
	memset(&o, 0, sizeof(o));
	memset(&ioc, 0, sizeof(ioc));

	nd.dp.enable_di_debug_mode = true;
	o.nd = &nd;

	for (i = 0; i < 3; i++) {
		cmds[i].o = &o;
		cmds[i].ds = &ds;
		cmds[i].cmdarr = cmds;
		cmds[i].my_leader = 0;
		cmds[i].my_stage = E_CMDS_STAGE_DO_IO_AND_PAR;
	}
	cmds[0].nraid_siblings = 3;

	ioc.comp.cmd = &cmds[2];
	ioc.reqs1.op = NVMEIB_BLOCK_IO_OP_READ;
	BUG_ON(!dp_dbgdi_should_add_info_core(&ioc));
}

#if defined(BLKDEV_SIMULATOR)
static void test_mirror_sync_inject_skipped_for_mirror(void)
{
	static struct nvmeibc_raid1 r1 = {.slice_size = 1, .replicas = 3};
	static struct nvmeibc_chunk ch = {.raid1s = &r1};
	static struct nvmeibc_subscription_ctx tr = {.r1 = 0};
	static struct nvmeibc_disk_segment ds = {.chunk = &ch, .toma_reg = &tr};
	static struct nvmeibc_block_command cmds[6];
	static struct nvmeibc_disk_io_command iocs[6];
	static struct nvmeib_data_buffer ndbs[6];
	static struct scatterlist sgs[6];
	static struct nvmeibc_block_device nd;
	static struct operation o;
	static struct recovery_sync_op so;
	uint8_t *bufs[6] = {0};
	unsigned int i;

	memset(&so, 0, sizeof(so));
	memset(cmds, 0, sizeof(cmds));
	memset(&nd, 0, sizeof(nd));
	memset(&o, 0, sizeof(o));

	nd.dp.enable_di_debug_mode = true;
	o.nd = &nd;
	o.op = NVMEIB_BLOCK_IO_OP_RECOVER_STALE;

	so.o = &o;
	so.cmds = cmds;
	so.r1 = &r1;
	so.nwhole_exec_plan.invalid_sources = (1u << 0) | (1u << 1) | (1u << 2);

	for (i = 0; i < 6; i++) {
		/* sg_init_one -> virt_to_page reads a kmem canary prefix; only sim_kmalloc'd
		 * buffers have one. Static/global buffers trip ASan global-buffer-overflow. */
		bufs[i] = sim_kmalloc(NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
		ut_setup_cmd_data(&cmds[i], &iocs[i], &ndbs[i], &sgs[i], bufs[i],
				  i < 3 ? NVMEIB_BLOCK_IO_OP_READ : NVMEIB_BLOCK_IO_OP_WRITE);
		cmds[i].o = &o;
		cmds[i].ds = &ds;
		cmds[i].cmdarr = cmds;
		cmds[i].ncmds = 6;
	}

	cmds[3].do_not_send = true;
	cmds[4].do_not_send = false;
	cmds[5].do_not_send = false;

	dp_ut_dbgdi_sync_copy_calls = 0;
	dp_ut_dbgdi_sync_clear_calls = 0;

	dp_mirror_ut_inject_debug_di_sync_info(&so, 2);

	/* New semantics: mirror sync writes share the NDB, so dbgdi must not mutate at all.
	 * Both counters stay at zero — no copy_sync_overwritten and no clear_sync_overwritten. */
	BUG_ON(dp_ut_dbgdi_sync_copy_calls != 0);
	BUG_ON(dp_ut_dbgdi_sync_clear_calls != 0);

	for (i = 0; i < 6; i++)
		sim_kfree(bufs[i]);
}
#endif /* BLKDEV_SIMULATOR */

#endif /* DBGDI_REMOVED_IN_PRODUCTION */

void test_nvmeibc_block_dp_dbgdi(void)
{
#ifndef DBGDI_REMOVED_IN_PRODUCTION
	test_dbgdi_log_roundtrip_and_corrupt_headers();
	test_dbgdi_log_zero_length_record_returns_error();
	test_dbgdi_log_stride_hop_bound();
	test_predicate_3way_mirror();
	test_predicate_2way_mirror_and_core_disabled_for_writes();
	test_predicate_jbod_short_circuit();
	test_predicate_ec_short_circuit();
	test_should_add_info_core_read_not_gated_by_leg();
#if defined(BLKDEV_SIMULATOR)
	test_mirror_sync_inject_skipped_for_mirror();
#endif
#endif
}
