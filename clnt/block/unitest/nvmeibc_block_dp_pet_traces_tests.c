/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 *
 * Unit tests for PET trace functions added in NVMESH-8223.
 *
 * Each test verifies that calling the trace function causes the per-operation
 * journal's worst_severity to advance to at least ERROR (or CRITICAL where
 * applicable), proving that nvmeib_pet_journal_add_msg was invoked with the
 * correct severity.  The journal is committed at the end of each test so the
 * messages appear in the test's binlog for post-mortem inspection.
 */

#include "nvmeibc_block_dp_pet_traces_tests.h"
#include "kr_incs.h"
#include "io_pet_traces_controller.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole.h"
#include "block/datapath_mirror/nvmeibc_block_dp_mirror.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_hot.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_maintenance.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_sync_txid_wraparound.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.h"
#include "common/pet/nvmeib_pet_specification.h"

/* ---- helpers ------------------------------------------------------------ */

static void __setup_so(struct recovery_sync_op *so,
		       struct operation *o,
		       struct nvmeibc_block_command *cmd)
{
	memset(so,  0, sizeof(*so));
	memset(o,   0, sizeof(*o));
	memset(cmd, 0, sizeof(*cmd));
	o->journal          = nvmeibc_io_pet_journal_make(sim_get_io_pet_controller());
	o->op               = NVMEIB_BLOCK_IO_OP_RECOVER_DB;
	cmd->rld.pre.all    = 0xAABBCCDD;
	cmd->rld.post.all   = 0x11223344;
	so->o               = o;
	so->cmds            = cmd;
	so->stage           = sync_stage_recov_lo_all_taken;
	so->n_slices        = 32;
}

/* Verify journal received at least one ERROR-severity (or worse) message. */
#define ASSERT_PET_ERROR(journal) \
	BUG_ON((journal).worst_severity < NVMEIB_PET_SEVERITY_ERROR)

#define ASSERT_PET_CRITICAL(journal) \
	BUG_ON((journal).worst_severity < NVMEIB_PET_SEVERITY_CRITICAL)

/* Commit and reset the journal so each test is independent. */
#define COMMIT_JOURNAL(o) nvmeib_pet_journal_commit(&(o).journal)

/* ---- sync_common --------------------------------------------------------- */


void test_pet_traces_sync_common(void)
{
	struct operation           o;
	struct nvmeibc_block_command cmd;
	struct recovery_sync_op    so;
	union nvmeib_blkset_info   binfo = { .all = 0xDEADBEEF };

	/* pet_trace_binfo_commit_rejected_nover — CRIT */
	__setup_so(&so, &o, &cmd);
	pet_trace_binfo_commit_rejected_nover(&so, binfo);
	ASSERT_PET_CRITICAL(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_binfo_commit_rejected — CRIT */
	__setup_so(&so, &o, &cmd);
	pet_trace_binfo_commit_rejected(&so, binfo, 's');
	ASSERT_PET_CRITICAL(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_binfo_wrong_call_context — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_binfo_wrong_call_context(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_binfo_unknown_txid — CRIT */
	__setup_so(&so, &o, &cmd);
	pet_trace_binfo_unknown_txid(&so, binfo);
	ASSERT_PET_CRITICAL(o.journal);
	COMMIT_JOURNAL(o);
}

void test_pet_traces_sync_no_write_hole(void)
{
	struct operation           o;
	struct nvmeibc_block_command cmd;
	struct recovery_sync_op    so;

	/* pet_trace_binfo_commit_unexpected — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_binfo_commit_unexpected(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_binfo_commit_missing — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_binfo_commit_missing(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_binfo_entry_mismatch — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_binfo_entry_mismatch(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_nwhole_param_err — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_nwhole_param_err(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);
}

void test_pet_traces_mirror(void)
{
	struct operation           o;
	struct nvmeibc_block_command cmd;
	struct recovery_sync_op    so;

	/* pet_trace_mirror_sync_wrong_state — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_mirror_sync_wrong_state(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_mirror_cmd_state_err — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_mirror_cmd_state_err(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_mirror_binfo_write_err — ERROR */
	__setup_so(&so, &o, &cmd);
	pet_trace_mirror_binfo_write_err(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);

	/* pet_trace_mirror_lock_state_err — ERROR */
	__setup_so(&so, &o, &cmd);
	so.locks[0].n_siblings = 1;
	pet_trace_mirror_lock_state_err(&so);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);
}

void test_pet_traces_io_req_rel_locks(void)
{
	struct operation        o;
	struct nvmeibc_cmd_lock l;

	memset(&o, 0, sizeof(o));
	memset(&l, 0, sizeof(l));
	o.journal  = nvmeibc_io_pet_journal_make(sim_get_io_pet_controller());
	o.op       = NVMEIB_BLOCK_IO_OP_WRITE;
	l.status   = NCL_STATUS_TAKEN;
	l.type     = NVMEIBC_CMD_LOCK_OWNER;

	/* pet_trace_lock_state_err — ERROR */
	pet_trace_lock_state_err(&o, &l);
	ASSERT_PET_ERROR(o.journal);
	COMMIT_JOURNAL(o);
}
