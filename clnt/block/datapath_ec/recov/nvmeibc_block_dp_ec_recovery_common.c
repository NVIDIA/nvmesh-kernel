/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_ec_recovery_common.h"
#include "nvmeibc_block_dp_ec_recov_maintenance.h"
#include "nvmeibc_block_dp_ec_recov_hot.h"
#include "nvmeibc_block_dp_ec_recov_cold.h"
#include "../nvmeibc_block_dp_ec_reed_solomon.h"
#include "../nvmeibc_block_dp_ec.h"
#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole.h"
#include "nvmeibc_block_dp_ec_sync_txid_wraparound.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"

static int dp_ec_sync_read_write_prepare_op(struct recovery_sync_op *so);
static int dp_ec_sync_md_read_prepare_op(   struct recovery_sync_op *so); // Called from thread context

int nvmeibcbdpec_get_rv_cur_stage_cmds(struct recovery_sync_op *so)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	int i, last_cmd  = so->last_cmd, rv = 0;
	for (i = (last_cmd + 1 - so->n_cmds); (i <= last_cmd)&&(!rv); i++) {
		if (!rldr[i].do_not_send) {								// Dont care about dead disks
			rv = rldr[i].o_rv;
			if (unlikely(rv))
				_NTSO(t_02_ecdpgtrvcmds, "stage=@SYNC_STAGE, cmds[@INT].o_rv=@RV", so->stage, i, rv);
		}
	}
	return rv;
}

static int dp_ec_prepare_full_sync_mssa(struct operation *o, struct dp_io_topo_iterator_res *it)
{
	const struct nvmeibc_raid1 *pr = it->r;
	int rv = -ENOMEM;
	dp_ec_set_allocated_mssa_into_operation(o);
	{
		struct multi_snake_slice_analyzer *mssa = o->mssa;
		const int replicas = pr->replicas;
		mssa->owner_seg = get_owner_seg_slice_start(pr, it->rlba);
		mssa->map_size = replicas * LOCKSET_SLICES; // TODO - once all rests upon mssa, reduce to single slice with single strat
		mssa->snake_size = o->nd->dp.p.snake_size;
		mssa->slice_size = pr->slice_size;
		mssa->replicas = pr->replicas;
		mssa->no_jour = true;
		mssa->single_strat = true;
		mssa->strategies[0] = COMPLEMENT_STRATEGY; // For all reacoveries it's complement strategy (even though it's actually restore before write)
		if (is_op_sync_read_md_only(o->op)) {
			mssa->n_writes = replicas;	// We only read MD (consider setting reads 0 and writes instead, depending on choice)
		} else {
			bitmap_set(mssa->pre_read, 0, mssa->map_size);
			mssa->n_reads = mssa->n_writes = replicas;
			mssa->gf_blocks.bio = kzalloc(sizeof(*mssa->gf_blocks.bio)*mssa->map_size, GFP_NOFS);
			mssa->blocks_md = kzalloc(sizeof(*mssa->blocks_md)*mssa->map_size, GFP_NOFS);
		}
		memset(mssa->column_to_cmd.read, -1, replicas);
		memset(mssa->column_to_cmd.write, -1, replicas);
		bitmap_set(mssa->bio_map, 0, mssa->map_size);
		mssa->tx_bms[0] = ((1 << mssa->slice_size) - 1); // We write them all
		mssa->row_masks.bio[                 0] = (1 << replicas) - 1;
		mssa->row_masks.bio[LOCKSET_SLICES - 1] = (1 << replicas) - 1;

		rv = mssa->n_reads + mssa->n_writes;
	}

	return rv;
}

int dp_ec_sync_prepare_op(struct recovery_sync_op *so)
{
	struct operation *o = so->o;
	struct nvmeibc_cmd_lock *l = so->locks;
	int ncmds_alloc, rv = -ENOMEM;
	struct dp_io_topo_iterator_res res;
	if (is_op_sync_commandless(o->op)) {
		rv = dp_sync_cmd_alloc_fill_rldr_only(so);
		goto _out;
	}
	dp_io_topo_iterator_conv_phys_lock_addr_to_raid_ofst(&res, l);
	ncmds_alloc = dp_ec_prepare_full_sync_mssa(o, &res);
	if (ncmds_alloc < 0) {
		rv = ncmds_alloc;
		goto _out;
	}

	if (!(so->cmds = dp_cmds_kvzalloc(ncmds_alloc)))
		goto _out;

	o->cmds = so->cmds;
	if (dp_ec_cmds_add_for_raid(o, &res, 0) != ncmds_alloc)
		goto _out;
	o->cmds->ncmds = ncmds_alloc;

	// Dispatch the prepare according to sync type. Typical preparation: D+P read commands and D+P writes to entire blockset.
	if (is_op_sync_stale(o->op) || is_op_sync_hot_jgc(o->op) || is_op_sync_no_wr_ho(o->op) || is_op_sync_cold(o->op) || is_op_sync_txid_wrap(o->op)) {
		rv = dp_ec_sync_read_write_prepare_op(so);
	} else if (is_op_sync_read_md_only(o->op)) {
		rv = dp_ec_sync_md_read_prepare_op(so);
	} else { BUG();}

_out:
	return rv;
}

/*************************** Sync mutation mechanism **************************/
/*
* ----------------------- Sync caller ---------------------------
* 1. IO : Encounters stale lock, wrong binfo or dirt-bit
* 2. Recovery: Has initial guess of binfo problem from server.
* 3.1. Sync-REC-SO (nested call)
* 3.2. Sync-Maintenance-SO (nested call). Under callers lock, fix binfo problems
* ----------------------- Handled problems ---------------------------
* 0. IO can encounter:
* 0.0. All maintenance problems.
* 0.0. REC SO only of types write-hole, no-write-hole.
* 0. Recovery can encounter:
* 0.0. Maintenance: only Broken {TxID,DBIT}, no TxID wraparound coz does not generate new data
* 0.0. All REC SO's
* 0. Running maintenance of Broken {TxID or DBIT}, encounter
* 0.0. Broken {TxID or DBIT}
* 0.0. read-fails - unsupported yet
* 0. Running REC SO
* 0.0. Maintenance: only Broken {TxID,DBIT}, no TxID wraparound coz does not generate new data
* ----------------------- So Steps (Maintenance and Rec) --------------
* 0. Take locks (missing locks if needed) and read blockset info
* 0. Check if binfo is broken (precondition for sync is not met)
* 0.0. We are sure it cannot be broken
* 0.0.0. NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP - It calls txid_wraparound sync before proceeding
* 0.0. Else, have problem but don't care
* 0.0.0  NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO - don't care, will commit broken binfo
* 0.0.0  NVMEIB_BLOCK_IO_OP_REC_COLD - binfo is irrelevant (was initialized by Toma to Dirty suspect and unknown TxID)
* 0.0.0  Maintance which solve unknowns in TxID/Dbits - they expect a problem and intend to solve it
* 0.0. Else need to fix binfo (rest of syncs)
* 0.0.0. Problems: Broken {TxID,DBIT}. Cannot encounter TX_WRAP_AROUND. Only caller IO can, not SO
* 0.0.0. Call nested 'SO' that will solve each of the problems
* 0. Now all binfo problems were solved or we don't care about them.
* 0. If SO cannot mutate at all:
* 0.0. ALL MAINTAIN cannot mutate, they are designed to only solve binfo, and are executed under callers locks
* 0.0. COLD cannot mutate, it has nothing to mutate to.
* 0. Else (SO can mutate to a different SO or can change its params)
* 0.0. Calc if write hole can exist
* 0.0. If (no_write_hole or write_hole) SO:
* 0.0.0. Mutate no_write_hole <---> write_hole according to the result above
* 0.0. Else (SO which does not fix problem on disk but can change its params)
* 0.0.0. Clarification: Relevant for SO DCONVICT_TURN_ON, COMMIT_BINFO
* 0.0.0. If write hole can exist
* 0.0.0.0. After 'SO' finish release locks back to primary owners val regardless of so->error
* 0. Else BUG()
* 0.0. R1 SO's are irrelevant
*
* ------------------------- Autonomous syncs --------------
* Daniel: Todo, maybe remove them in future. They save execution time but
*  introduce more code. Autonomous syncs dont acquire locks nor write anything
*  to disk. Theit flow is not shared with other syncs. So they dont mutate
*  dont deal with binfo and are not encapsulated by request-release locks state
*  machine
*/

bool dp_sync_common_are_all_binfo_equal(const struct recovery_sync_op *so)
{
	/* Daniel: Important!!! If seconday owner exists, must take its value coz
	   it might be the sole owner in next topology and have higher TxID */
	const struct nvmeibc_cmd_lock *lo = &so->locks[0];
	const union nvmeib_blkset_info ow_rv = {.all = nvmeibc_get_binfo_of_lock(lo) };
	bool rv = true;
	int i, n_sibs = lo->n_siblings;
	for (i = 1; i < n_sibs; i++) {  // merge all copy-of-owner to owner
		const union nvmeib_blkset_info so_rv = {.all = nvmeibc_get_binfo_of_lock(&lo[i]) };
		if (unlikely(so_rv.all != ow_rv.all)) {
			_NTSO(t_04_binfoeq, "binfos aren't equal: owner_@BINFO, seg[@SI]_@BINFO", ow_rv.all, get_si_of_lock(&lo[i], so->r1), so_rv.all);
			rv = false;
		}
	}
	if (likely(rv)) {
		_NTSO(t_05_binfoeq, "all binfos are equal: @BINFO", ow_rv.all);
	}
	return rv;
}

bool dp_sync_common_has_dbits_anywhere(const struct recovery_sync_op *so)
{
	const struct nvmeibc_cmd_lock *lo = &so->locks[0];
	int i, n_sibs = lo->n_siblings;
	for (i = 0; i < n_sibs; i++) {
		const union nvmeib_blkset_info so_rv = {.all = nvmeibc_get_binfo_of_lock(&lo[i]) };
		if (so_rv.bits.dirty) {
			_NTSO(t_06_binfoeq, "Lock @INT has dbits, seg[@SI]_@BINFO", i, get_si_of_lock(&lo[i], so->r1), so_rv.all);
			return true;
		}
	}
	return false;
}

static bool __is_write_hole_possible(const struct recovery_sync_op *so)
{
	const struct nvmeibc_cmd_lock *l = &so->locks[0];
	const union nvmeib_lock_id holder = {.all = (u32)get_contending_id(&l->comp)}; // == dc->compare (coz its cmpxchng result
	if ((holder.all == 0ULL) || did_caller_of_so_took_this_lock(l))
		return false; // Caller holds the Primary owner, so even if we encountered stale lock it is not real
	if (so->cmds->rld.pre.bits.txid == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS)
		return false; // If stale lock -> no transaction was made
	if (holder.bits.is_read)
		return false; // If stale lock -> left by read. No write hole possible
	BUG_ON(holder.bits.is_stale == 0);
	return true;
}

static bool __any_stale(const struct recovery_sync_op *so)
{
	int i;
	for (i = 0; i < so->locks->nlocks; i++) {
		const struct nvmeibc_cmd_lock *l = &so->locks[i];
		const union nvmeib_lock_id holder = {.all = (u32)get_contending_id(&l->comp)};
		if (holder.bits.is_stale)
			return true;
	}
	return false;
}

void nvmeibcbdpec_fill_blockset_recovered_info(struct nvmeibc_block_command *cmd,
		const int timeout, void *ctx, const u64 lock_entry,
		struct recovery_sync_op *so, const u32 rng_id, const u32 ent_id, const bool pass2toma)
{
	const u64 slice_size = so->r1->slice_size;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd->gen_cmd;
	struct nvmeibc_disk_segment *ds = cmd->ds;
	NFIN;

	/* init gen-cmd common part */
	gen_cmd->jiffies_start = jiffies;
	gen_cmd->timeout = timeout; // Needed for HTR
	gen_cmd->ctx = ctx;			// Needed for HTR
	/* init gen-cmd specific part */
	gen_cmd->opcode = NVMEIB_GEN_OP_BLKSET_RECOVERED;

	if (so->orig_rldr)
		gen_cmd->cpu_mask_info = so->o->cpu_mask_info; // STRUCT ASSIGNMENT

	gen_cmd->param.br.uuid = so->recoveree_cuuid;
	strlcpy(gen_cmd->param.br.ds_uuid, ds->uuid, NVMEIB_GID_STR_MAX);
	gen_cmd->param.br.lock_ent = lock_entry;
	gen_cmd->param.br.blkset_num = (so->rlba / (LOCKSET_SLICES * slice_size));
	gen_cmd->param.br.blkset_slba = ds->first_lba + (so->rlba / slice_size);
	gen_cmd->param.br.rng_id = rng_id;
	gen_cmd->param.br.ent_id = ent_id;
	gen_cmd->param.br.pass2toma = pass2toma;

	WARN_ON((ds->toma_acm == NVMEIBTC_DS_MODE_DEAD) || cmd->do_not_send);	// Daniel: Over caucious, todo remove

	NFOUT;
}

static void __mutate_op_according_to_binfo(struct recovery_sync_op *so)
{
	const enum nvmeib_block_io_op op = so->o->op;
	bool is_write_hole;

	if (unlikely(is_so_nested(so))) {	// Trap for mutation of nested 'so', impossible
		char buf[32];
		recovery_sync_stack_to_string(&so->stack, buf);
		WARN(true, "SO={0x%2x,0x%2x}, Stack=%s, Wrong usage, original so already mutated\n", so->o->op, so->stage, buf);
		BUG();
	}

	/* Skip immutable ops */
	if (is_op_sync_maintain(op))
		return;						// maintanace never mutate coz it is executed under callers locks
	if (is_op_sync_cold(op))
		return;						// Nothing to mutate to
	if (is_op_sync_hot_jgc(op))
		return;						// Executed under callers locks, no write hole.
	if (is_op_sync_txid_wrap(op))
		return;						// Executed under callers locks and when being called all binfo already resolved.

	is_write_hole = __is_write_hole_possible(so);
	if (is_op_sync_stale(op)) {
		if (!is_write_hole)
			so->o->op = NVMEIB_BLOCK_IO_OP_RECOVER_DB;		// Downgrade to no_write_hole op
	} else if (is_op_sync_no_wr_ho(op)) {
		if (is_write_hole)
			so->o->op = NVMEIB_BLOCK_IO_OP_RECOVER_STALE;	// Upgrade to write_hole op
		else if (is_op_sync_scrubbing(op) && dp_ec_can_fix_dbits(so->cmds)) {
			so->o->op = NVMEIB_BLOCK_IO_OP_RECOVER_STALE;	// Upgrade to write_hole op
		}
	} else if (is_op_sync_commandless(op) || is_op_sync_commit_binfo(op) || (op == NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO)) {
		// If there is a write hole problem, cannot fix it. But even if there isn't,
		// and if there is a non-owner stale lock, not able to send blockset recovered for it,
		// so not allowed to unlock to 0. In both cases, don't mutate but prevent unlocking to 0.
		if (is_write_hole || __any_stale(so)) {
			if (is_op_sync_commit_binfo(op)) {
				// Note1: Regardless of whether we have stale locks or not, owner_binfo is valid
				// Note2: Even if other copy has a more recent binfo than primary owner, it is legal to take the lesser problem, not the maximal problem. HTR should be able to take care of that
				// Note3: so->o.commit_only_owner_binfo = can be True/False. and mathematically this is valid
			}
			if (op == NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO) {
				// Mathematically was not implemented as no-writehole, but same as commit-binfo-sync.
				// We did not perform any work on disk, and this sync, regardless of implementation, has nothing to do
				BUG_ON(!is_op_sync_read_md_only(NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO));	// If developers did not implement it as no write hole -then why using anything except metadata???
			}
			so->n_slices = 0;	// Unlock back to stale where needed, otherwise no effect on those sync types
		}
	} else {
		WARN(true, "nvmeibc di bug! op=%d, incorrect mutation\n", op);
	}
	if (op != so->o->op) {
		WARN(is_op_sync_commit_binfo(so->o->op), "commit binfo is never mutated to, if it's the first time don't forget to call mark_blockset_info_not_written(so)\n");
		_NTSO(trace_dp_ec_recovery_common_mutate_op_according_to_binfo, "SO mutated type=@BLOCK_IO_OP-->@BLOCK_IO_OP", op, so->o->op);
	}
}

static void __execute_op_post_maint_and_mutation(struct recovery_sync_op *so)
{	/* Note: so->error == 0 or else those calls would be skipped */
	const enum nvmeib_block_io_op op = so->o->op;
	if ((op == NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO) || (op == NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON)) {
		return dp_maintenance_execute_op(so);
	} else if (is_op_sync_txid_wrap(op)) {
		WARN_ON(so->stage != sync_stage_recov_lo_all_taken);
		return dp_ec_sync_txid_wraparound_execute_op(so);
	} else if (is_op_sync_stale(op) || is_op_sync_hot_jgc(op)) {
		return dp_ec_sync_stale_execute_op(so);
	} else if (is_op_sync_no_wr_ho(op)) {
		return dp_ec_sync_no_write_hole_execute_op(so);
	} else if (is_op_sync_cold(op)) {
		return dp_ec_sync_cold_execute_op(so);
	} else if (is_op_sync_commit_binfo(op)) {
		if (!should_blockset_info_commit(so))
			return nvmeibcbdpec_return_to_caller_sm(so);	// Nothing to do: Example: must commit binfo, but all binfo copies on all locks are identical
		nvmeibcbdpec_push_sm_to_stack(so, nvmeibcbdpec_return_to_caller_sm);
		return dp_sync_write_all_blocksets_info_op(so);
	} else if (op == NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_GC) {
		return dp_ec_sync_j_gc_execute_op(so);
	} else { BUG(); }
}

static inline bool dp_ec_no_write_hole_has_pre_sync_work(const struct recovery_sync_op *so)
{
	const struct nvmeibc_block_command* rldr = so->cmds;
	if (unlikely(dp_ec_mainten_has_txid_unreslvd(rldr)))
		return true;
	if (unlikely(dp_sync_has_unknown_dbits(rldr, nvmeibc_raid1_get_protect_lvl(so->r1))))
		return true;
	if (unlikely(!dp_sync_common_are_all_binfo_equal(so)))
		return true;
	return false;
}

/* The sm below is the equivalent of __analyze_binfo_sm_cb_b4w()
   (Sync/IO both might needs help from other sync.
   The state machine includes 3 steps:
   0. Implicit steps: Precondition of locks is met (every lock that is needed is indeed taken)
   1. Check preconditions for the sync (broken binfo). If broken, launch nested
      syncs to solve them (NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO/all_binfo_are_NOT_equal)
   2. Once preconditions are met, analyze the severncess of the problem, if it
      is less/more severe than we thought, sync might need to mutate to other
      syncs, or change properties of existing sync.
   3. Now, execute post mutation sync */
void dp_ec_sync_execute_op(struct recovery_sync_op *so)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	__ndump_operation(ec_sync_execute, &so->o);

_func_start:
	if (so->error) { 				// Fast fail, during error path
		_NTSO(trace_dp_ec_recovery_common_dp_ec_sync_execute_op, "stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
		so->stage = sync_stage_mutate_done;
	}
	_ND(trace_1_dp_ec_recovery_common_dp_ec_sync_execute_op, "-->op=@BLOCK_IO_OP stage=@SYNC_STAGE err=@ERR", so->o->op, so->stage, so->error);
	//__dump_operation(&so->o);

	switch (so->stage) {
	/* Before doing the sync: Fixup Broken Ram (via maintanance ops) */
	case sync_stage_recov_lo_all_taken: {		// Start state machine
		bool take_only_owner_binfo = is_op_sync_commit_binfo(so->o->op) && so->o->commit_only_owner_binfo;
		const union nvmeib_blkset_info binfo = dp_locks_get_TxID_dbits(so->locks, 0, take_only_owner_binfo);
		if (!dp_sync_verify_binfo_is_legal(so, binfo))
			goto _func_start;
		if (unlikely(so->is_autonomous)) {
			BUG_ON(so->o->op != NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_GC);
			so->stage = sync_stage_mutate_done;
			goto _func_start;
		}

		rldr->rld.post.all = rldr->rld.pre.all = binfo.all;	// Commit possibly broken binfo to rldr
		if (nvmeib_do_i_care_about_broken_binfo(so->o->op)) {
			so->stage = sync_stage_recov_analyze_binfo;
		} else {
			so->stage = sync_stage_recov_write_binfo_done;
		}
		goto _func_start;
	}

	case sync_stage_recov_analyze_binfo: {	// Check all preconditions
		if (dp_ec_mainten_has_txid_unreslvd(rldr) || dp_sync_has_unknown_dbits(rldr, nvmeibc_raid1_get_protect_lvl(so->r1))) { // Step 1 part 1
			so->stage = sync_stage_recov_analyze_binfo;	// Continue analyzing for next problems implicit mark_blockset_info_not_written
			nvmeibcbdpec_push_sm_to_stack(so, dp_ec_sync_execute_op);
			dp_ec_mainten_reinit(so, NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO);
			ASYNC_AWAIT_AND_RESUME(dp_maintenance_execute_op(so));
		}

		// If any of the above were needed, all locks are now equal in the sync, and mark not written is set
		// Otherwise binfo might need to be commited if they are not all equal (precondition for mutation)
		if (!should_blockset_info_commit(so) && !dp_sync_common_are_all_binfo_equal(so)) { // Step 1 part 2
			mark_blockset_info_not_written(so); // Explicit mark_blockset_info_not_written
		}

		if (is_op_sync_commit_binfo(so->o->op) || !should_blockset_info_commit(so)) { // No (additional) resolutions required - can mutate
			so->stage = sync_stage_recov_write_binfo_done;      // In commit_binfo in the last stage we commit the binfo anyway.
		} else {	// All binfo fixes have been analyzed and completed, we need to commit binfo
			so->stage = sync_stage_recov_write_binfo;			// After fixing done commit blockset info
		}
		goto _func_start;
	}

	case sync_stage_recov_write_binfo: { 						// Commit fixed binfo to servers RAM
		if (!dp_sync_verify_binfo_is_legal(so, rldr->rld.post))
			goto _func_start;
		so->stage = sync_stage_recov_write_binfo_done;
		nvmeibcbdpec_push_sm_to_stack(so, dp_ec_sync_execute_op);
		ASYNC_AWAIT_AND_RESUME(dp_sync_write_all_blocksets_info_op(so));
	}

	case sync_stage_recov_write_binfo_done: {					// Precondition (RAM = locks + binfo) to sync op is OK
		if (!dp_sync_verify_binfo_is_legal(so, rldr->rld.post))
			goto _func_start;
		__mutate_op_according_to_binfo(so);
		so->stage = sync_stage_mutate_done;
		goto _func_start;
	}

	case sync_stage_mutate_done: {
		/* Finished fixing pre conditions (broken binfo, mutations), now execute 'so' */
		so->stage = sync_stage_recov_lo_all_taken;	// As if no maintanace was done
		if (unlikely(so->error)) {
			return nvmeibcbdpec_return_to_caller_sm(so);
		} else if (so->o->op == NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO) {	// Could not or no need to mutate. Nothing to do, fixed binfo was already commited
			BUG_ON(dp_ec_no_write_hole_has_pre_sync_work(so)); // Just for debug, verify that no problems remains
			return nvmeibcbdpec_return_to_caller_sm(so);
		}
		return __execute_op_post_maint_and_mutation(so);
	}

	default:;
	} // switch (->stage)
	WARN(true, "nvmeibc bug! Illegal sync state. IO can stuck\n!");
}

#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
void dp_ec_sync_cmd_cb(struct nvmeibc_block_command *cmd)
{	/* This state machine starts when all locks are taken and terminates in request to release locks */
	struct recovery_sync_op *so;
	dp_dbgdi_do_rdr_info(cmd);
	if (dp_sync_cmd_generic_cb(cmd) != 0)
		return;	// Waiting for at least 1 remaining read or write
	on_disk_hook(dp_ec_sync_cmd_cb, cmd->ds->disk, on_sync_cb_stage_end, cmd);	// In simulator only, must be after atomic decs to verify dbits barrier
	so = cmd->cmdarr->o->rso;
	switch (so->o->op) {
		case NVMEIB_BLOCK_IO_OP_RECOVER_STALE:
		case NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_HOT_GC:
			return dp_ec_sync_stale_cb_stg_end(cmd); //EC-1453: htr hack, use 'so'
		default: break;
	};
	BLKCMP_SO_ASYNC_RESUME_CMP(dp_ec_sync_resume_op(so));
}

void dp_ec_sync_resume_op(struct recovery_sync_op *so)
{	/* This state machine starts when all locks are taken and terminates in request to release locks */
	//__dump_operation(so->o);
	switch (so->o->op) {
		case NVMEIB_BLOCK_IO_OP_REC_COLD:
			return dp_ec_sync_cold_cb_stg_end(so);
		case NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL:
		case NVMEIB_BLOCK_IO_OP_RECOVER_DB:
		case NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK:
		case NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING:
			return dp_sync_no_write_hole_cb_stg_end(so);
		case NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP:
			return dp_ec_sync_txid_wraparound_cb_stg_end(so);
		case NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO:
			return dp_ec_mainten_cb_stg_end(so);
		case NVMEIB_BLOCK_IO_OP_MAINTAIN_BLKSET_RECOV :
			return dp_ec_mainten_blkset_recov_cb_stg_end(so);
		default:
			WARN(1, "nvmeibc bug! so=" PRI_SO_NAME ", stage=%d, op=0x%x\n", PRI_SO_NAME_ARGS(so), so->stage, so->o->op);
	}
}

/***************** EC common for READ+WRITE sync's (i.e: Np whole, HTR, cold, txid-wraparound)  *********************/

/* Verify only readfail or do_not_send return codes exist */
int dp_sync_get_any_non_readfail_errors(struct recovery_sync_op *so)
// TODO(EC-2518) - rename and unite both of these
{ /* Daniel: I would unite this fucntion with previous and call it __generate_slice_plan_from_read_cmds_rv(). It analyzes rv and creates decision plan:
		{ .worst_software_error, .badsectors_bmp, .first_valid_read (for R1), .enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE }
		Execution plan is created by this function and __analyze_no_write_hole_read(),  both should be wrappened in a single creatino of plan, upon which main state machine acts */
	int i = so->last_cmd - so->n_cmds + 1, rv = 0;
	for (; (i <= so->last_cmd) && (!rv); i++) {
		if ((so->cmds[i].do_not_send) || (!is_transient_disk_error(so->cmds[i].o_rv))) {
			continue;
		}
		rv = so->cmds[i].o_rv;
		// if we are looking for any disk error we stop here; otherwise some errors merge strategy should be defined (at least in comments)
		// If we are looking for the Worst Case RV (WCRV) we can call a function that clasifies errors and returns the worst of 2, can be part of EC-2518
	}
	if (rv)
		_ND(tr_06_sync_check_read_rv, "Read cmd[@RV].error=@RV", i, rv);
	return rv;
}

static bool dp_sync_cmd_is_valid_read_source(const struct recovery_sync_op *so, const int src)
{	// Note If this segment is not readable (for any reason: topo / dirtybit) then __mark_read_to_dirty_w_seg_as_do_not_send() would make it do not send.
	const struct nvmeibc_block_command *c = &so->cmds[src];
	const bool cmd_succeeded = ((c->o_rv == 0) && (!c->do_not_send));
	//const int slice = (so->is_sbs_mode ? so->slice_by_slice_index : -1);	// When dbit in metadata is implemented, use it to get per slice dbit and decide on validity of this read
	if (!cmd_succeeded)
		return false;						// Read cmd failed.
	if (nvmeibc_is_readable(c->ds))
		return true;
	if (c->ds->toma_acm == NVMEIBTC_DS_MODE_W) {
		// If dirty suspect or real dirtybit, we read this segment to compare to RW and potentially avoid write if they are identical, but this cannot be source of truth, only compared to source of truth
		return dp_sync_does_see_clean_ram_dbits(so);			// Todo, for N-mirror, test if this specifc segment has dbits (not dbits in general) || so->R1.is_dirty_suspect
	}
	return false;
}

// TODO: make virtual and merge with EC-2518
static void __find_valid_source_for_r1(struct recovery_sync_op *so)
{
	int ir;
	so->R1.valid_read_index = -1;
	for (ir = 0; ir < n_read_cmds(so); ir++) {
		if (dp_sync_cmd_is_valid_read_source(so, ir)) {
			so->R1.valid_read_index = ir;
			break;
		}
	}
}

/* Generates the readfail bitmask for all reads for EC
   Sets valid read index for Mirror */
u32 dp_sync_gen_read_fail_bit_mask(struct recovery_sync_op *so)
{
	//TODO(EC-2518): the function name is misleading; I propose to split it to two with and without side effect , Daniel: Or unite it with function below to form executino plan
	//               also it looks like this function should be virtual
	struct nvmeibc_block_command *cmds = so->cmds;
	int c;
	u32 bit_mask = 0, bit;
	if (__is_raid1_mirror(so)) { //TODO Follow integration in EC-2518
		__find_valid_source_for_r1(so);
	}
	for (c = so->last_cmd + 1 - so->n_cmds, bit = 1; c <= so->last_cmd; c++, bit <<= 1) { // Search for read fail seg
		if (!is_transient_disk_error(cmds[c].o_rv)) { // Found one bad sector
			bit_mask |= bit;
		} else if (unlikely(cmds[c].o_rv == -ENXIO) && cmds[c].do_not_send) { // Do Not Send
			continue;
		} else if (unlikely(cmds[c].o_rv)) { // Some other error
			_NTSO(error_dp_ec_recovery_readfail_gen_read_fail_bit_mask, "error: @COMMAND_IDX, operation_rv=@OPERATION_RV", c, cmds[c].o_rv);
		}
	}
	return bit_mask;
}

/* Write failures must notify TOMA */
void dp_sync_notify_toma_on_write_failure(struct recovery_sync_op *so)
{
	const int start = n_read_cmds(so);
	int i;
	for (i = 0; i < n_write_cmds(so); i++) {
		if (so->cmds[start+i].o_rv && should_notify_toma(so->cmds[start+i].o_rv))
			__send_toma_cmd_help(&so->cmds[start+i].iocmd->comp, U32_MAX, 0);
	}
}

bool dp_sync_verify_binfo_is_legal(struct recovery_sync_op *so, const union nvmeib_blkset_info binfo)
{
	if (verify_binfo_is_legal(so->locks->ds, binfo, so->locks->address, 'r'))
		return true;
	nvmeibc_block_suspend(so->o->nd, NULL, NULL); /* Critical error. Any further action will cause corruption. Fail recovery, suspend the device. We require user intervention to exit this state. Should not happen normally. */
	so->error = -10022;
	return false;
}

/********************* General Cold/Hot Commont functions *******************/
sgmnts_bmp_t nvmeibcbdpec_bmp_ss2fs_with_pari(struct nvmeibc_raid1 *r1, u64 slba, roles_bmp_t tx_bmp)
{
	const int shift = get_owner_seg_slice_start(r1, slba * (u64)r1->slice_size);
	tx_bmp |= nvmeibc_raid1_get_parities_bmp(r1);
	return rol32_width(tx_bmp, shift, r1->replicas);
}

/********************* Journal candidate for IO transaction *******************/
const struct nvmibc_tx_candidate*
     nvmeibcbdpec_get_rollfwd_jour_candidate(const struct recovery_sync_op *so)
{
	struct nvmibc_blockset_candidates *jcl = so->o->user_data;
	const struct nvmibc_tx_candidate* rv = NULL;
	struct nvmibc_tx_candidate *can;

	if ((!jcl) || list_empty(&jcl->can_list))
		goto _out;

	list_for_each_entry(can, &jcl->can_list, next) {
		if (can->type == ROLL_FWD) {
			rv = can;
			break;
		}
	}

_out:
	return rv;
}

bool nvmeibcbdpec_has_rollfwd_jour_candidate(const struct recovery_sync_op *so)
{
	struct nvmibc_blockset_candidates *jcl = so->o->user_data;
	struct nvmibc_tx_candidate *can;
	bool res = false;
	if ((!jcl) || list_empty(&jcl->can_list))
		goto _out;

	list_for_each_entry(can, &jcl->can_list, next) {
		if (can->type == ROLL_FWD) {
			res = true;
			break;
		}
	}

_out:
	return res;
}

bool __attribute__((__unused__)) nvmeibcbdpec_has_jour_candidate(const struct recovery_sync_op *so)
{
	struct nvmibc_blockset_candidates *jcl = so->o->user_data;
	return ((jcl) && (!list_empty(&jcl->can_list)));
}

const struct nvmibc_tx_candidate* __attribute__((__unused__))
     nvmeibcbdpec_get_jour_candidate(const struct recovery_sync_op *so)
{
	struct nvmibc_blockset_candidates *jcl = so->o->user_data;
	const struct nvmibc_tx_candidate* rv = NULL;
	if (jcl)
		rv = list_first_entry_or_null(&jcl->can_list, struct nvmibc_tx_candidate, next);
	return rv;
}

u32 nvmeibc_tx_get_dblk_ofset_in_cmd(const struct recovery_sync_op *so,
				int si, const struct nvmibc_tx_candidate *cand, int i, u32* ofst_rv)
{
	struct nvmeibc_block_command *rldr = so->cmds;		// First set of read commands, relative to slice start
	const int ci = ((si + so->r1->replicas) - so_get_owner_seg(so)) % so->r1->replicas;	// Find Command to segment si. Todo: Simplify this
	struct nvmeibc_block_command *cmd = &rldr[ci];
	const u64 data_dlba = cand->b.j2slba + (u64)i + so->r1->segments[si].first_lba;
	BUG_ON(cmd->ds->toma_reg->seg != si);
	*ofst_rv = (u32)(data_dlba - cmd->iocmd->reqs1.disk_address);
	return ci;
}

void nvmeibc_tx_get_ptrs_to_blk_in_cmd(const struct recovery_sync_op *so, u32 ci, u32 ofst, union nvmeibc_block_dp_ec_data_block_md **dmd, void**buf)
{
	struct nvmeibc_block_command *cmd = &so->cmds[ci];
	struct sgl_block_iter sbi = SGL_BLOCK_ITER_INIT(nvmeibc_block_command_get_sgl(cmd));
	const u32 md_ssize = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	(*dmd) = (void*)((u8*)cmd->iocmd->reqs1.md + (ofst*md_ssize));
	sgl_block_iter_advance(&sbi, ofst);
	(*buf) = sgl_block_iter_virt(&sbi);
}

// EC-1475: Unify with HTR: does_dmd_match()
static bool nvmeibc_tx_does_seg_d2j_match(const struct recovery_sync_op *so, int si,
								   struct nvmibc_tx_candidate *cand, int i)
{
	u32 ofset_in_blckset, md_ssize;
	u32 ci = nvmeibc_tx_get_dblk_ofset_in_cmd(so, si, cand, i, &ofset_in_blckset);	// Note: Here ofset_in_blckset == use data.slice
	struct nvmeibc_block_command *cmd = &so->cmds[ci];
	union nvmeibc_block_dp_ec_data_block_md *dmd;
	u32 ver;
	bool match = false;
	md_ssize = nvmeibc_sgmnt_sw_md_size(cmd->ds);

	if (cmd->o_rv) {		// Non RW or failed RW (For now shouldn't get inside this because all readable segs cmds are validated before)
		_ND(trace_dp_ec_recovery_common_nvmeibc_tx_does_seg_d2j_match, "si @SI: c=@CI, !send=@SEND, rv=@O_RV", si, ci, cmd->do_not_send, cmd->o_rv);
		goto _out;
	}

	dmd = (cmd->iocmd->reqs1.md + (ofset_in_blckset*md_ssize));

	if (dmd->tx_id != cand->b.tx_id) {
		_ND(trace_1_dp_ec_recovery_common_nvmeibc_tx_does_seg_d2j_match, "si @SI: diff TxID (@TXID,@TXID)", si, dmd->tx_id, cand->b.tx_id);
		goto _out;
	}

	if (dmd->jri != cand->locations[si].jri) {
		_ND(trace_2_dp_ec_recovery_common_nvmeibc_tx_does_seg_d2j_match, "segment_idx=@SI: diff JRI (jri=@JRI,jri=@JRI)", si, dmd->jri, cand->locations[si].jri);
		goto _out;
	}

	#ifdef DEBUG_SAVE_JENTRY
	{
		const u32 d2j_rng = dmd->D.d2j_rng; //(cmd->is_parity) ? dmd->P.d2j_rng : dmd->D.d2j_rng;
		/* d2j_rng should either be the rblk of the journal entry (pre 2.7) or the jentry (2.7+) */
		WARN(d2j_rng != cand->locations[si].jentry &&
			d2j_rng != cand->locations[si].jentry*cand->locations[si].rng_binje + i - cand->locations[si].binje_offset, 
			"Candidate for seg %d jentry is not the same as the used one (cand=%d, md=%d)\n",si, cand->locations[si].jentry, d2j_rng);
	}
	#endif

	ver = dmd->D.version; //(cmd->is_parity ? dmd->P.version: dmd->D.version);
	if (ver != NVMEIBC_DATA_MD_VERSION) {
		_ND(trace_3_dp_ec_recovery_common_nvmeibc_tx_does_seg_d2j_match, "segment_idx=@SI: diff VER (@MD_VER,@MD_VER)",si, ver, NVMEIBC_DATA_MD_VERSION);
		goto _out;
	}

	match = true;
_out:
	return match;
}

#include "nvmeib_shared_ec.inc.c"

static bool __is_journal_committed_snake_extra_validations(struct jent_md_decompressed *pivot_ent) {
	// TODO: Can have more tight validations
	bool has_zero_txbm_part = false, prev_txbm_zero = false, has_next;
	struct jblock_md_decompressed_for_seg *pivot_md = pivot_ent->md_arr;
	WARN(!(pivot_md->tx_bmp), "nvmeibc bug!, the first txbm in the entry can never be 0.\n"); // sanity
	for (has_next = true; has_next; pivot_md++) {
		if (pivot_md->tx_bmp == 0) {
			if ((!prev_txbm_zero) && has_zero_txbm_part)
				return false;  // There are more than 1 part in which txbm == 0 can't be a snake - probably 2 ios with same txid which sumed up to one candidate.
			prev_txbm_zero = has_zero_txbm_part = true;
		} else {
			prev_txbm_zero = false;
		}
		has_next = pivot_md->has_next;
	}

	return true;
}

/* __is_journal_committed == true doesn't ensure that the journal commited (There might be false positives) - this is ok because the d2j will tell for certain if the journal is really commited and is part of an io. */
bool __is_journal_committed(struct nvmeibc_raid1 *r1, struct jent_md_decompressed jent_mds[], sgmnts_bmp_t analyzed_segs /* suppose to be readable segs*/)
{
	struct jblock_md_decompressed_for_seg *jent_mds_iter[N_MAX_RAID_SLICE_LEN];
	struct jblock_md_decompressed_for_seg *pivot_md;
	int i, j, pivot = 0, slice_start;
	u64 slba;
	const int replicas = r1->replicas;
	sgmnts_bmp_t rw_parities, readable, txbm_conu;
	struct jent_md_decompressed *pivot_ent;
	bool is_jgc = hweight16(analyzed_segs) == 1;
	bool has_next;
	ulong rw_parities_ul;
	WARN(is_jgc, "nvmeibc_bug!\n");
	i = 0;
	while (!jent_mds[i].is_valid && i < replicas) {
		i++;
	}
	if (i==replicas)
		return false;
	slba = jent_mds[i].md_arr[0].j2slba;
	slice_start = get_owner_seg_slice_start(r1, slba * (u64)r1->slice_size);
	readable = nvmeibc_raid1_get_sgmnts_bmp(r1, readable);
	rw_parities = readable & nvmeibc_raid1_get_roles_bmp(r1, slice_start, pari_sgmnts);
	rw_parities_ul = rw_parities;
	WARN(readable != analyzed_segs, "nvmeibc bug! readable=%u, analyzed_segs=%u\n", readable, analyzed_segs);
	if (!rw_parities) {
		return false;	/* We get here only on cold and in this case we can never be sure 100% that the candidate is real Either way there will be a resolve dbits on this blockset which will turnon dbits on all parities so we can ignore this candidate. */
	}

	pivot = find_first_bit(&rw_parities_ul, replicas);
	pivot_ent = &jent_mds[pivot];
	pivot_md = pivot_ent->md_arr;
	_NT(t_01_bcrcjc, "pivot: seg=@HEX_1B, tx_len=@HEX_1B, valid=@BOOL_YN, txid=@TXID, rw_parities=@TXBM", (u8)pivot, (u8)pivot_ent->len, pivot_ent->is_valid, pivot_md->tx_id, (u32)rw_parities_ul);
	if (!pivot_ent->is_valid)
		return false;       // Parity journal must exist for IO to take place.

	for (j = 0; j < replicas; j++)
		jent_mds_iter[j] = jent_mds[j].is_valid ? jent_mds[j].md_arr : NULL;

	do { // Check that parities md is equal
		for_each_set_bit(j, &rw_parities_ul, replicas) {
			if (!jent_mds_iter[j])
				return false; // Parity journal must exist for IO to take place.
			if (j == pivot)
				continue;

			if (memcmp(jent_mds_iter[j], pivot_md, sizeof(*pivot_md))) {
				WARN(jent_mds_iter[j]->version != pivot_md->version || jent_mds_iter[j]->tx_id != pivot_md->tx_id, "nvmeibc bug!, to jblocks with different version for same client: j2slba=0x%llx, txid=%u, txbm=%u, version=%u, version_pivot=%u, pivot_txid=%u\n", jent_mds_iter[j]->j2slba, jent_mds_iter[j]->tx_id, jent_mds_iter[j]->tx_bmp, jent_mds_iter[j]->version, pivot_md->version, pivot_md->tx_id);
				return false;
			}

			jent_mds_iter[j]++;
		}
		has_next = pivot_md->has_next;  // caching
		pivot_md++;
	} while (has_next);

	pivot_md = pivot_ent->md_arr;  // reset pivot pointer
	txbm_conu = rw_parities; // must have parities
	do { // Check that data md doesn't conflict with parities
		const u64 slba_pivot = pivot_md->j2slba;
		roles_bmp_t txbm_pivot = pivot_md->tx_bmp;
		roles_bmp_t txbm_pivot_with_pari = txbm_pivot | nvmeibc_raid1_get_parities_bmp(r1);
		const u32 txbm_data = nvmeibcbdpec_bmp_ss2fs_with_pari(r1, slba_pivot, txbm_pivot) & nvmeibc_raid1_get_roles_bmp(r1, slice_start, data_sgmnts); // relative to praid
		const u32 ok_txbm_data = txbm_data & analyzed_segs;
		txbm_conu |= (sgmnts_bmp_t)ok_txbm_data;
		_NT(t_02_bcrcjc, "txbm=@TXBM=>@TXBM ok_txbm=@TXBM, slba=@J2D", txbm_pivot_with_pari, txbm_data, (u32)ok_txbm_data, slba_pivot);
		rw_parities_ul = ok_txbm_data;
		for_each_set_bit(j, &rw_parities_ul, replicas) {  // Check that parities md is equal
			if (!jent_mds_iter[j])
				return false;

			if (memcmp(jent_mds_iter[j], pivot_md, sizeof(*pivot_md) - sizeof(pivot_md->has_next) /*has next might not be equal */ )) {
				WARN(jent_mds_iter[j]->version != pivot_md->version || jent_mds_iter[j]->tx_id != pivot_md->tx_id, "nvmeibc bug!, to jblocks with different version for same client: j2slba=0x%llx, txid=%u, txbm=%u, version=%u, version_pivot=%u, pivot_txid=%u\n", jent_mds_iter[j]->j2slba, jent_mds_iter[j]->tx_id, jent_mds_iter[j]->tx_bmp, jent_mds_iter[j]->version, pivot_md->version, pivot_md->tx_id);
				return false;
			}

			/* ---- mds match ----- */
			if (jent_mds_iter[j]->has_next) {  // mds match inc iterator
				jent_mds_iter[j]++;
			} else {
				jent_mds_iter[j] = NULL;  // Finish checking this entry and it's ok.
			}
		}
		has_next = pivot_md->has_next;  // caching
		pivot_md++;
	} while (has_next);

	// check that the jouranl has the form of a snake (Not a mandatory check)
	if (!__is_journal_committed_snake_extra_validations(pivot_ent)) {
		return false;
	}

	// Needs no to invalidate the entries which are outside of all the txbms of pivot
	rw_parities_ul = ~txbm_conu;
	for_each_set_bit(j, &rw_parities_ul, replicas) {  // Check that parities md is equal
		jent_mds[j].is_valid = false; // invalidate_jent
	}
	return true;
}

#include "block/controlpath/nvmeibc_b_cp_blkset_topo.h"
enum cand_state nvmeibc_tx_does_d2j_j2d_match(const struct recovery_sync_op *so,
		struct nvmibc_tx_candidate *cand, const struct data_slice_d2j_info *data)
{
	struct nvmeibc_raid1 *pr = so->r1;
	int si, n_match = 0, n_analyzed = 0, i;
	enum cand_state rv;
	ulong txbm_with_pari_roles;
	ulong txbm_with_pari_segs = 0;
	u32 n_needed = 0;
	bool checked_entire_txbm;
	const u64 blockset_slba = cand->b.j2slba & (~(((1UL << LOCKSET_SLICES_SHIFT) - 1UL)));

	// Validation
	WARN(!cand->b.tx_bmp[0], "Candidate with first txbm only on parities: cand{j2slba=0x%llx, txid=0x%x}\n", cand->b.j2slba, cand->b.tx_id);

	for (i = 0; i < cand->b.len; ++i) {
		const int slice = (int)((cand->b.j2slba + i) % LOCKSET_SLICES);
		txbm_with_pari_roles = cand->b.tx_bmp[i] | nvmeibc_raid1_get_parities_bmp(pr);
		nvmeibcbdpec_bmp_ss2fs_with_pari(pr, cand->b.j2slba + i, cand->b.tx_bmp[i]);
		if (!__is_bmp_included_in(data->txbm[slice], txbm_with_pari_roles))  // DataTxBmp contradicts journal txbm. journal candidate is invalid
			return NOT_A_CANDIDATE;
	}

	{ // not madatory validation: validation that there is no data committed with the same txid outside the journal candidate scope, (not mandatory cause the umbiguity and jri check must ensure this anyway).
		const u64 first_slba = cand->b.j2slba, last_slba = cand->b.j2slba + cand->b.len - 1;
		for (i = 0; i < LOCKSET_SLICES; ++i) {
			const u64 slba = blockset_slba + i;
			if (data->txbm[i] && (slba < first_slba || slba > last_slba))
				return NOT_A_CANDIDATE;
		}
	}

	/* Per RW seg we managed to read, check if Data/Parity MD match
	   Lockset-Entry (TxID) && Journal-MD (JRI) and has valid EDIC */
	for (i = 0; i < cand->b.len; ++i) {
		txbm_with_pari_segs = nvmeibcbdpec_bmp_ss2fs_with_pari(pr, cand->b.j2slba + i, cand->b.tx_bmp[i]);
		n_needed += hweight32(txbm_with_pari_segs);
		for_each_set_bit(si, &txbm_with_pari_segs, pr->replicas) {
			if (nvmeibc_is_readable(&pr->segments[si])) {
				if (nvmeibc_tx_does_seg_d2j_match(so, si, cand, i)) {
					set_bit(i, cand->locations[si].is_data_commited);
					n_match++;
				}
				n_analyzed++;
			}
		}
	}
	checked_entire_txbm = (n_analyzed == (int)n_needed);

	/* Decision: If at least one match found, roll forward */
	if (n_match == 0) {  // In this case Candidate is not the last one (it's a tx before wraparound) - if it was the last one than max_data_txid would have been smaller in one
		rv = NOT_A_CANDIDATE; /* For sure not the last TX, discard J candidate*/
	} else if (n_match == n_analyzed) { /* Data is fully commited to RW segs */
		if (checked_entire_txbm)
			rv = FULLY_COMMITED; /* Transaction was completed, Journal candidate not needed*/
		else
			rv = MIGHT_BE_FULLY_COMMITED; /* Could not fully analyze TXBM (may need to update dbit)*/
	} else {  /* Data partially commited, Roll forward required */
		rv = PARTIALLY_COMMITED;
	}
	_NTSO(trace_dp_ec_recovery_common_nvmeibc_tx_does_d2j_j2d_match, "ss_txbmp=@TXBM, Disk_txbmp=@TXBM, match(@N_MATCH) <= anal(@N_ANALYZED) <= need(@N_NEEDED), has_d2j=@HAS_D2J",
		   cand->b.tx_bmp[0], (u32)txbm_with_pari_segs, n_match, n_analyzed, n_needed, rv);
	return rv;
}

/**************** No Write Hole Virtual Funcs *********************************/

// Init recovery plan and set n_uncompleted_cmds accordingly
static struct dp_ec_restore_plan __init_recovery_plan(struct recovery_sync_op *so)
{
	const u32 invalid_data_mask = so->nwhole_exec_plan.invalid_sources & nvmeibc_raid1_get_data_bmp(so->r1);
	const u16 slice_size = so->r1->slice_size;
	const int n_parities = nvmeibc_raid1_get_protect_lvl(so->r1);
	const roles_bmp_t parities_to_write = (so->nwhole_exec_plan.first_write_bmp | so->nwhole_exec_plan.second_write_bmp);
	const struct dp_ec_restore_plan plan = {
		.parity_target_mask = GENMASK(n_parities-1,0) & (parities_to_write >> slice_size), // IF we are going to write this parity then it is target.
		.invalid_sources = so->nwhole_exec_plan.invalid_sources,
		.md_to_calc = so->nwhole_exec_plan.first_write_bmp,
		.parity_rebuild_required = ((parities_to_write & so->nwhole_exec_plan.invalid_sources) >= (roles_bmp_t)(1 << slice_size)), // The second_write_bmp is only for dbits and not for restoration.
		.invalid_data_rebuild_required = (invalid_data_mask),
		.dbits_op = so->nwhole_params.dbits_turnon_bmp ? DBITS_OP_TURN_ON : DBITS_OP_TURN_OFF,
	};
	// Each restored data and calculated parity will have a single completion and an additional one after both are complete
	int n_callbacks = hweight32(invalid_data_mask) + 1;
	if (plan.parity_rebuild_required) { // DEAD parities are not calculated - count how many parities will be calculated
		const u32 dead_parity_mask = (nvmeibc_raid1_get_roles_bmp(so->r1, so_get_owner_seg(so), dead) >> slice_size);
		const int n_calculated_parities = n_parities - hweight32(dead_parity_mask);
		n_callbacks += n_calculated_parities;
	}
	nvmeibc_sync_set_uncompleted_cmds(so, n_callbacks);
	return plan;
}

static inline union nvmeibc_block_dp_ec_data_block_md *__get_valid_pari_md_in_slice(const struct recovery_sync_op *so, u64 lba, const roles_bmp_t readable_segs)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	const struct nvmeibc_raid1 *pr = so->r1;
	const struct multi_snake_slice_analyzer *mssa = so->o->mssa;
	const ulong valid_pari_bmp = nvmeibc_raid1_get_parities_bmp(pr) & readable_segs;
	const raid_role_t valid_pari = find_first_bit(&valid_pari_bmp, pr->replicas);
	const struct nvmeibc_block_command *valid_pari_cmd = &rldr[column_to_cmd(mssa, valid_pari, true)];
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(valid_pari_cmd->ds);
	union nvmeibc_block_dp_ec_data_block_md *valid_pari_md = (void*)((u8*)valid_pari_cmd->iocmd->reqs1.md + md_size*lba);
	if (unlikely(!valid_pari_bmp)) {
		NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: readable_segs=@BITMAP slba_in_blockset=@SLICE", _T, goodpath_nvmeibc_syncs, t04_cmtfp, so->o->dbg_id, readable_segs, lba);
		WARN(true, "Trying to get a valid parity while there are no valid parities.\n");
	}
	return valid_pari_md;
}

bool nvmeibc_warn_on_parities_sync_missmatch = true;	// Designed to handle cases where 1 parity is written manually by 'dd' and its metadata is lost (marked as never written)
static void __parities_sync_never_written_missmatch_fix(const struct recovery_sync_op *so, union nvmeibc_block_dp_ec_data_block_md *p0, union nvmeibc_block_dp_ec_data_block_md *p1)
{
	((struct recovery_sync_op *)so)->nwhole_exec_plan.is_blkset_corrupted = true;
	if (nbdpec_md_was_data_never_written(p0))		// Take the correct parity metadata (simulate as if it was read this way). Edic becomes incorrect!
		p0->raw = p1->raw;
	else
		p1->raw = p0->raw;
}

static inline bool __is_slice_neverwritten_by_pari(const struct recovery_sync_op *so, u64 lba, const roles_bmp_t readable_segs)
{
	union nvmeibc_block_dp_ec_data_block_md *valid_pari_md = __get_valid_pari_md_in_slice(so, lba, readable_segs);
	const bool res = nbdpec_md_was_data_never_written(valid_pari_md);

	{	// Sanity - redo with mssa, verify the written state of all parities is identical
		struct nvmeibc_block_command *rldr = so->cmds;
		struct multi_snake_slice_analyzer *mssa = so->o->mssa;
		const struct nvmeibc_raid1 *pr = so->r1;
		const ulong valid_pari_bmp = nvmeibc_raid1_get_parities_bmp(pr) & readable_segs;
		int bit;
		for_each_set_bit(bit, &valid_pari_bmp, pr->replicas) {
			const struct nvmeibc_block_command *valid_pari_cmd_i = &rldr[column_to_cmd(mssa, bit, true)];
			const u32 md_size_i = nvmeibc_sgmnt_sw_md_size(valid_pari_cmd_i->ds);
			union nvmeibc_block_dp_ec_data_block_md *valid_pari_md_i = (void*)((u8*)valid_pari_cmd_i->iocmd->reqs1.md + md_size_i*lba);
			if (unlikely(res != nbdpec_md_was_data_never_written(valid_pari_md_i))) {
				NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: orig_md=@MD_PTR other_pari_md=@MD_PTR other_pari_role=@ROLE slba_in_blockset=@SLICE", _T, goodpath_nvmeibc_syncs, t03_cmtfp,
				so->o->dbg_id, valid_pari_md, valid_pari_md_i, bit, lba);
				WARN(nvmeibc_warn_on_parities_sync_missmatch,
					"nvmeibc bug: volume %s, rlba=0x%llx, slice=0x%llx, One parity is written, other is not! md={0x%llx,0x%llx}\n",
					so->o->nd->name, so->rlba, lba, valid_pari_md->raw, valid_pari_md_i->raw);
				__parities_sync_never_written_missmatch_fix(so, valid_pari_md, valid_pari_md_i);
				*((bool*)&res) = false;	// This slice is written, but with corrupted parity!
			}
		}
	}
	return res;
}

static inline u32 __calc_max_txid_from_pari(const struct recovery_sync_op *so, u64 lba)
{
	union nvmeibc_block_dp_ec_data_block_md *valid_pari_md = __get_valid_pari_md_in_slice(so, lba, ~so->nwhole_exec_plan.invalid_sources);
	const u32 res = valid_pari_md->tx_id;
	if (unlikely(nbdpec_md_was_data_never_written(valid_pari_md))) {
		NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: slba_in_blockset=@SLICE", _T, goodpath_nvmeibc_syncs, t01cmtxfp, so->o->dbg_id, lba);
		WARN(nvmeibc_warn_on_parities_sync_missmatch, "Trying to find max_txid in a neverwritten slice!\n");	// This can actually happen if we have a never written slice with 3 bad sectors which causes a destruction of a never written slice.
	}
	{	// Sanity, verify txid on all valid parities is identical
		struct nvmeibc_block_command *rldr = so->cmds;
		struct multi_snake_slice_analyzer *mssa = so->o->mssa;
		const struct nvmeibc_raid1 *pr = so->r1;
		const ulong valid_pari_bmp = nvmeibc_raid1_get_parities_bmp(pr) & (~so->nwhole_exec_plan.invalid_sources);
		int bit;
		for_each_set_bit(bit, &valid_pari_bmp, pr->replicas) {
			const struct nvmeibc_block_command *valid_pari_cmd_i = &rldr[column_to_cmd(mssa, bit, true)];
			const u32 md_size_i = nvmeibc_sgmnt_sw_md_size(valid_pari_cmd_i->ds);
			union nvmeibc_block_dp_ec_data_block_md *valid_pari_md_i = (void*)((u8*)valid_pari_cmd_i->iocmd->reqs1.md + md_size_i*lba);
			const u32 txid = valid_pari_md_i->tx_id;
			if (res != txid) {
				NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: orig_md=@MD_PTR other_pari_md=@MD_PTR other_pari_role=@ROLE slba_in_blockset=@SLICE", _T, goodpath_nvmeibc_syncs, t01_cmtfp,
						so->o->dbg_id, valid_pari_md, valid_pari_md_i, bit, lba);
				WARN(nvmeibc_warn_on_parities_sync_missmatch,
					"nvmeibc bug: volume %s, rlba=0x%llx, slice=0x%llx, Readable parities with different txids: 0x%x != 0x%x\n",
					so->o->nd->name, so->rlba, lba, res, txid);
				__parities_sync_never_written_missmatch_fix(so, valid_pari_md, valid_pari_md_i);
				*((u32*)&res) = valid_pari_md->tx_id;
			}
		}
	}
	return res;
}

struct md_visitor {
	//if returns true the "for" will break
	bool (*visit)(struct md_visitor *base, const union nvmeibc_block_dp_ec_data_block_md *md);
};

struct is_slice_neverwritten_visitor {
	struct md_visitor md_visitor;
	bool is_slice_neverwritten;
};

struct max_txid_visitor {
	struct md_visitor md_visitor;
	u32 max_txid;
};

static inline bool block_calc_max_txid(struct md_visitor *base, const union nvmeibc_block_dp_ec_data_block_md *md) {
	struct max_txid_visitor* self = container_of(base, struct max_txid_visitor, md_visitor);
	self->max_txid = max(self->max_txid, (typeof(self->max_txid))md->tx_id);
	return false;
}

static inline bool is_block_neverwritten(struct md_visitor *base, const union nvmeibc_block_dp_ec_data_block_md *md) {
	struct is_slice_neverwritten_visitor* self = container_of(base, struct is_slice_neverwritten_visitor, md_visitor);
	self->is_slice_neverwritten = nbdpec_md_was_data_never_written(md);
	return !self->is_slice_neverwritten; //if the block was written - break the loop, we have the answer
}

static inline void for_each_md(const struct recovery_sync_op *so, const roles_bmp_t md_to_visit_bmp, u64 lba, struct md_visitor* visitor){
	struct nvmeibc_block_command *rldr = so->cmds;
	struct multi_snake_slice_analyzer *mssa = so->o->mssa;
	const struct nvmeibc_raid1 *pr = so->r1;
	const ulong md_bmp = md_to_visit_bmp;
	raid_role_t role;

	for_each_set_bit(role, &md_bmp, pr->replicas) {
		const struct nvmeibc_block_command *data_cmd = &rldr[column_to_cmd(mssa, role, false)];
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(data_cmd->ds);
		union nvmeibc_block_dp_ec_data_block_md *data_md = (void*)((u8*)data_cmd->iocmd->reqs1.md + md_size*lba);
		if (visitor->visit(visitor, data_md))
			break;
	}
}

// debug_reason = 'D' - destroy slice, 'R' - Reconstruct, 'W' - txid wraparound
static inline bool __is_slice_neverwritten_by_data_blocks(const struct recovery_sync_op *so, u64 lba, const roles_bmp_t readable_segs, char debug_reason) {
	const struct nvmeibc_raid1 *pr = so->r1;
	const roles_bmp_t readable_data_md_bmp = nvmeibc_raid1_get_data_bmp(pr) & readable_segs;
	struct is_slice_neverwritten_visitor visitor = { .md_visitor.visit = is_block_neverwritten, .is_slice_neverwritten = false };
    if (unlikely(readable_data_md_bmp != nvmeibc_raid1_get_data_bmp(pr))) {
		NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: slba_in_blockset=@SLICE", _T, goodpath_nvmeibc_syncs, t01isnbdb, so->o->dbg_id, lba);
		WARN((debug_reason != 'D'), "Both parities are invalid and not all data blocks are valid, should have destroyed slice instead of: %c.\n", debug_reason);
	}
	for_each_md(so, readable_data_md_bmp, lba, &visitor.md_visitor);
	return visitor.is_slice_neverwritten;
}

static inline u32 __calc_max_txid_by_data_blocks(const struct recovery_sync_op *so, u64 lba) {
	const struct nvmeibc_raid1 *pr = so->r1;
	const roles_bmp_t data_md_bmp = nvmeibc_raid1_get_data_bmp(pr);
	const roles_bmp_t readable_data = data_md_bmp & (~so->nwhole_exec_plan.invalid_sources); // at this point if there are invalid sources we are in destroy slice flow.
	struct max_txid_visitor visitor = { .md_visitor.visit = block_calc_max_txid, .max_txid = 0};
	for_each_md(so, readable_data, lba, &visitor.md_visitor);
	return visitor.max_txid;
}

bool dp_sync_is_slice_neverwritten(const struct recovery_sync_op *so, u64 lba, const roles_bmp_t readable_segs, char debug_reason)
{
	const bool has_valid_pari = ((nvmeibc_raid1_get_parities_bmp(so->r1) & readable_segs) != 0);
	if (has_valid_pari) {
		return __is_slice_neverwritten_by_pari(so, lba, readable_segs);
	} else {
		return __is_slice_neverwritten_by_data_blocks(so, lba, readable_segs, debug_reason);
	}
}

static inline u32 __calc_max_txid_in_slice(const struct recovery_sync_op *so, u64 lba)
{
	const bool has_valid_pari = ((nvmeibc_raid1_get_parities_bmp(so->r1) & (~so->nwhole_exec_plan.invalid_sources)) != 0);
	if (has_valid_pari) {
		return __calc_max_txid_from_pari(so, lba);
	} else {
		return __calc_max_txid_by_data_blocks(so, lba);
	}
}

// TODO consider refactor for mssa (not just start _n_cmds)
static void __prepare_data_wr_cmds_md(struct recovery_sync_op *so, const struct dp_ec_restore_plan *plan)
{
	const struct multi_snake_slice_analyzer *mssa = so->o->mssa;
	struct nvmeibc_block_command *rldr = so->cmds;
	int bit;
	const int wri = mssa_get_data_start_index(mssa);
	const u32 restored_bit_mask = plan->invalid_sources;
	const unsigned long test = (unsigned long)plan->md_to_calc;
	u32 b;
	BUG_ON(wri < mssa->n_reads);
	BUG_ON((restored_bit_mask & (u32)plan->md_to_calc) == 0);

	for_each_set_bit(bit, &test, mssa->slice_size) {   // Parities handled separetly
		const struct nvmeibc_block_command *cmd = &rldr[column_to_cmd(mssa, bit, false)];
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
		union nvmeibc_block_dp_ec_data_block_md *md = cmd->iocmd->reqs1.md;	// We don't know/care about Jentry when restoring data
		for (b = 0; b < cmd->nlbas; b++) {	// Slice iterator
			if (dp_sync_is_slice_neverwritten(so, b, (~so->nwhole_exec_plan.invalid_sources), 'R')) {  // Data never written to this slice, restore data blocks also to neverwritten.
				nbdpec_md_mark_data_never_written_no_dbits(md, cmd->is_parity);
			} else if ((1 << bit) & restored_bit_mask) {	// No pre-read MD, only EDIC was set via reed solmon, need to add version/TXID/journal info (mark no journal)
				const u32 max_txid_in_slice = __calc_max_txid_in_slice(so, b);
				nbdpec_md_mark_no_journal(md, max_txid_in_slice, cmd->is_parity);
			} else {
				BUG();
			} /* else was written and read correctly nothing to do */
			md = (void*)((u8*)md + md_size);
		}
	}
}

static void __debug_verify_writing_to_parity_md_correct_dbits(const struct recovery_sync_op *so, const union nvmeibc_dbits_entry *dbits, enum dp_ec_dirty_bits_operation dbits_op)
{
	const union nvmeib_blkset_info binfo = {.bits.txid = so->cmds->rld.post.bits.txid, .bits.dirty = dbits->all_bits};
	const char act = (dbits_op == DBITS_OP_TURN_OFF) ? 'w' : 'r';
	if (!verify_binfo_is_legal(so->locks->ds, binfo, so->locks->address, act))	// ? 'w' : 'r'
		nvmeibc_block_suspend(so->o->nd, NULL, NULL);
}

static void __construct_parity_md(struct recovery_sync_op *so, int di, const enum dp_ec_dirty_bits_operation dbits_op)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	struct nvmeibc_block_command *dst = &rldr[di];
	void *dmd = dst->iocmd->reqs1.md;
	u32 b, md_dsize = nvmeibc_sgmnt_sw_md_size(dst->ds);
	const union nvmeibc_dbits_entry binfo_dbits = {.all_bits = rldr->rld.post.bits.dirty};
	const bool is_slice_destroyed = nvmeibc_sync_sl_by_sl_is_current_slice_destroyed(so);
	for (b = 0; b < dst->nlbas; b++, dmd += md_dsize) {
		const union nvmeibc_dbits_entry *dbits = (dbits_op == DBITS_OP_TURN_OFF) ? &so->nwhole_exec_plan.ram_dbits_after_turnoff : &binfo_dbits;
		__debug_verify_writing_to_parity_md_correct_dbits(so, dbits, dbits_op);
		if (unlikely(is_slice_destroyed)) {  // we are in sbs_mode
			if (unlikely(!so->is_sbs_mode)) {
				NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: slba_in_blockset=@SLICE", _T, goodpath_nvmeibc_syncs, t_01_cpme, so->o->dbg_id, b);
				WARN(true, "nvmeibc bug, destroy and not on slice by slice mode\n");
			}
			nvmeibc_block_dp_ec_md_fill_p_with_dbits(dmd, *dbits);
		} else if (dp_sync_is_slice_neverwritten(so, b, (~so->nwhole_exec_plan.invalid_sources), 'R')) {
			nbdpec_md_mark_data_never_written_with_dbits(dmd, true, *dbits); // Mark never written with correct version
		} else {
			const u32 max_txid_in_slice = __calc_max_txid_in_slice(so, b);
			nvmeibc_block_dp_ec_md_set_txid_and_dirty_bits_value_mark_no_journal(dmd, max_txid_in_slice, *dbits);
		}
	}
}

static void __build_target_parity_from_source(struct recovery_sync_op *so,
											  int si, int di)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	struct nvmeibc_block_command *src = &rldr[si];
	const u32 md_ssize = nvmeibc_sgmnt_sw_md_size(src->ds);
	void *smd = src->iocmd->reqs1.md;
	u32 b;
	const bool is_slice_destroyed = nvmeibc_sync_sl_by_sl_is_current_slice_destroyed(so);

	for (b = 0; b < src->nlbas; b++, smd += md_ssize) {
		struct nvmeibc_block_command *dst = &rldr[di];
		const u32 md_dsize = nvmeibc_sgmnt_sw_md_size(dst->ds);
		void *dmd = dst->iocmd->reqs1.md + md_dsize*b;
		if (unlikely(is_slice_destroyed)) {  // we are in sbs_mode
			WARN(!so->is_sbs_mode, "nvmeibc bug, destroy and not on slice by slice mode\n");
			nvmeibc_block_dp_ec_copy_md_dbits(smd, dmd); // Copy only dbits the rest already written in destroy function.
		} else {
			nvmeibc_block_dp_ec_copy_md(smd, dmd);  // preserve max_txid in blkset
		}
	}
}

static void __update_parity_metadata(struct recovery_sync_op *so, int pi, const enum dp_ec_dirty_bits_operation dbits_op)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	struct nvmeibc_block_command *parity_cmd = &rldr[pi];
	const u32 md_ssize = nvmeibc_sgmnt_sw_md_size(parity_cmd->ds);
	void *pmd = parity_cmd->iocmd->reqs1.md;
	u32 b;
	struct nvmeibc_dbits_tx tx;
	const int slice_start = so_get_owner_seg(so);
	const int num_parities = nvmeibc_raid1_get_protect_lvl(so->r1);
	const bool is_slice_destroyed = nvmeibc_sync_sl_by_sl_is_current_slice_destroyed(so);
	sgmnts_bmp_t dbits_turnon_bmp_pr = rol32_width(so->nwhole_params.dbits_turnon_bmp, slice_start, so->r1->replicas);

    WARN(parity_cmd->do_not_send, "nvmeibc bug, using unread parity MD as valid source src_acm=%d\n", parity_cmd->ds->toma_acm);

	if (dbits_op == DBITS_OP_TURN_OFF) {
		nvmeibc_dbits_tx_init_by_bmp(&tx, num_parities, 0, nvmeibc_raid1_get_sgmnts_bmp(so->r1, dbits_off_mask), 0);
	} else if (dbits_op == DBITS_OP_TURN_ON) {
		nvmeibc_dbits_tx_init_by_bmp(&tx, num_parities, dbits_turnon_bmp_pr, 0, 0);
	}

	// TODO: Ensure we update MD binfo and RAM binfo correctly
	for (b = 0; b < parity_cmd->nlbas; b++) {
		union nvmeibc_block_dp_ec_data_block_md *_pmd = pmd;
		enum nvmeibc_data_written_state written_state = nbdpec_md_get_data_written_state(pmd);
		if (unlikely(is_slice_destroyed)) {  // we are in sbs_mode
			WARN(!so->is_sbs_mode, "nvmeibc bug, destroy and not on slice by slice mode\n");
		} else if (written_state == DATA_WRITTEN) {
			nbdpec_md_mark_no_journal(pmd, _pmd->tx_id, true); // preserve max_txid in blkset dmd by taking the max_txid in slice (which always apears on readable parity).
		} else if (written_state == DATA_VIRGIN) {
			nbdpec_md_mark_data_never_written_no_dbits(pmd, true);
		} // Else: DATA_EXPLICITLY_MARKED_NEVERWRITTEN no need to touch txid.

		if (dbits_op != DBITS_OP_NOP) { // Update current DB with required change (Turn Off or On)
			union nvmeibc_dbits_entry pre_slice_dbits;
			union nvmeibc_dbits_entry post_slice_dbits;
			pre_slice_dbits.all_bits = fill_nvmeibc_dbits_entry_from_md(pmd).all_bits;
			post_slice_dbits.all_bits = nvmeibc_dbits_tx_apply(&pre_slice_dbits, &tx);
			__debug_verify_writing_to_parity_md_correct_dbits(so, &post_slice_dbits, dbits_op);
			nvmeibc_block_dp_ec_md_fill_p_with_dbits(pmd, post_slice_dbits);
		}

		pmd += md_ssize;
	}
}

void prepare_parity_md(struct recovery_sync_op *so, const struct dp_ec_restore_plan plan)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	const struct nvmeibc_raid1 *pr = so->r1;
	s32 pari_idx = 0;
	int P_src = pr->slice_size;				// First parity Read  cmd
	const int P_dst = P_src + pr->replicas;	// First parity Write cmd
	const s16 protect_lvl = nvmeibc_raid1_get_protect_lvl(pr);
	bool override_dbits_op_on_readfail = false;	// Used in case read-failed for parities with per-slice info of DBits, we need to set all slices with DBits from ram
	WARN_ON(2 < protect_lvl);
	WARN_ON(rldr[P_src].iocmd->reqs1.md != rldr[P_dst].iocmd->reqs1.md);		// Read/Write cmds share same metadata buffers

	for (; pari_idx < protect_lvl; pari_idx++) { // Find Valid source parity
		if (!((1 << (P_src + pari_idx)) & so->nwhole_exec_plan.invalid_sources)) {
			P_src += pari_idx;
			break;
		}
	}

	if (pari_idx == protect_lvl) {	// No valid source found - all parities will be reconsturcted
		P_src = -1;
		if (so->o->op == NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL) {	// ensure dbits are taken from binfo, they are the current worst case dbits
			if (plan.dbits_op != DBITS_OP_TURN_ON) {	// If we are turning on we take it from resolved (post) binfo
				if (!so->nwhole_exec_plan.should_turnoff_dbits) { // Do not take pre dbits
					override_dbits_op_on_readfail = true;
				}// else Readfail + Dbit sync, we do clear DBits and post is calculated correcly in this case
			}
		}
	} else 						 	// Found source, update it, then copy
		__update_parity_metadata(so, P_src, plan.dbits_op);
	for_each_set_bit(pari_idx, &plan.parity_target_mask, protect_lvl) { // Go over all targets (if DB requires update all writeable segments will be targets)
		if (P_src < 0) { // No sources so just set all values (TX/DB) in MD
			__construct_parity_md(so, P_dst + pari_idx, (override_dbits_op_on_readfail) ? DBITS_OP_NOP : plan.dbits_op);
		} else {
			if ((P_dst + pari_idx - pr->replicas) != P_src) // Source already ready
				__build_target_parity_from_source(so, P_src, P_dst + pari_idx);
		}
	}
}

static void __restore_invalid_data_and_md(struct recovery_sync_op *so, const struct dp_ec_restore_plan *plan) {
	extern void restore_degraded_data_for_read(const struct operation *o, u32 dgrd_segment_bmp, int prev_rv, const bool is_crc_required);
	restore_degraded_data_for_read(so->o, plan->invalid_sources, 0/*so->error*/, true); // 0 is so->error (which is 0 if we get here)
	__prepare_data_wr_cmds_md(so, plan);
}

static void __mark_is_valid_for_reuse(struct recovery_sync_op *so, const ulong bitmap, const int size)
{
	int column;
	for_each_set_bit(column, &bitmap, size) {
		int ci = so->o->mssa->column_to_cmd.read[column];
		BUG_ON(ci < 0);
		so->o->cmds[ci].is_valid_for_reuse = true;
	}
}


static void __restore_invalid_parity_and_md(struct recovery_sync_op *so, const struct dp_ec_restore_plan plan)
{	// Asumption: All non-degraded data segs have been restored
	if (plan.parity_rebuild_required) {
		extern u32 apply_gf_calculation_for_operation(const struct operation *o, u32 dgrd_sgmnts_bmp, int prev_rv);
		const u16 target_bmp = nvmeibc_raid1_get_parities_bmp(so->r1) & nvmeibc_raid1_get_inverse_roles_bmp(so->r1, so->o->mssa->owner_seg, dead); // All non-dead parities are targets
		so->o->mssa->target_bmp = target_bmp;
		apply_gf_calculation_for_operation(so->o, 0, 0/*so->error*/); // 0 is so->error (which is 0 if we get here)
		__mark_is_valid_for_reuse(so, target_bmp, so->o->mssa->replicas); // Not for SBS mode - since the next slice might not have been valid and this is per command
	}
	if (plan.parity_rebuild_required || (plan.dbits_op != DBITS_OP_NOP)) {	// If parity calucalted or DB are changed update Party MD
		prepare_parity_md(so, plan);
	}
}

/* EC ONLY:
   1. Restore data if in target bit map (readfailed or DB turn off)
   2. Calculate Parities if required */
static void __rebuild_full_slices(struct recovery_sync_op *so, const struct dp_ec_restore_plan plan)
{
	if (plan.invalid_data_rebuild_required) {       // Restore data, before calc P/Q. (D0 might be dead, so it's not a target, but it is used for P/Q)
		const u16 target_bmp = plan.invalid_sources & nvmeibc_raid1_get_data_bmp(so->r1);
		so->o->mssa->target_bmp = target_bmp;
		__restore_invalid_data_and_md(so, &plan);
		__mark_is_valid_for_reuse(so, target_bmp, so->o->mssa->slice_size); // Not for SBS mode - since the next slice might not have been valid and this is per command
	}
	__restore_invalid_parity_and_md(so, plan);
	dp_ec_sync_cmd_cb(so->cmds);						// called last to ensure we completed to mark all MD before preparing for write stage
}

// Virtual function called from so->restore_function: Implemenation support A-Sync state machine
enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE dp_ec_no_write_hole_fix(struct recovery_sync_op *so)
{
	const struct dp_ec_restore_plan plan = __init_recovery_plan(so);
	BLKCMP_SO_ASYNC_AWAIT(__rebuild_full_slices(so, plan));
	return NEXT_STAGE_SENT; // EC return code - All commands already sent to reed solomon
}

#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
// TODO: Ofir add turnon dbits stage on dead blocks (use Yuris commit of turnon dbits after turnoff.)
// Mirror (with MD enabled TBD) and EC can destroy slice in MD only
bool nvmeibc_warn_on_no_readable_segs = true;
static void __prep_write_perm_read_fail(struct recovery_sync_op *so)
{
	int i, n_total = last_cmd(so);
	u32 max_txid_in_slice, src_bit;
	const int slice_start = so_get_owner_seg(so);
	const bool has_readable_blocks = so->nwhole_exec_plan.invalid_sources != nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, raid.all);
	bool is_never_written_slice = false;
	if (has_readable_blocks) {
		const u64 lba = 0; // lba = 0 cause we read only one slice
		is_never_written_slice = dp_sync_is_slice_neverwritten(so, lba, (~so->nwhole_exec_plan.invalid_sources), 'D');
		max_txid_in_slice = is_never_written_slice ? NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS : __calc_max_txid_in_slice(so, lba);
	} else {
		/* The reason we preserving max_txid in slice is because if max_txid in blockset is
		   on the destroyed slice we might destroy all but one block which has max_txid on it and fall to cold recovery + seg turns to dead
		   Now resolve txid will find smaller txid and start working with that. later the block will turn rw (without regen - it doesn't have dbit).
		   With another cold now we will get the previous max_txid and might miss roll-fwd of the last TX. */
		max_txid_in_slice = so->cmds->rld.pre.bits.txid;
		WARN(nvmeibc_warn_on_no_readable_segs, "No readable segs, destroying slice with ram's txid=%u", max_txid_in_slice);
	}
	_NTSO(t_01_pwprf, "destroying slice with txid=@TXID, never_written=@BOOL_YN", max_txid_in_slice, is_never_written_slice);

	for (i = n_read_cmds(so), src_bit = 1; i < n_total; i++, src_bit <<= 1) {
		struct nvmeibc_block_command *cmd = &so->cmds[i];
		if (!(src_bit & (so->nwhole_exec_plan.first_write_bmp | so->nwhole_exec_plan.second_write_bmp)))
			continue;
		if (src_bit & so->nwhole_exec_plan.invalid_sources) { // Daniel: I added the required optimization from below (we only destroy the invalid sources) the tests pass so it's a win win, but we should test that if a DEAD segment returns we CAN fix the slice
			nbdpec_md_mark_data_invalid_for_read(cmd->iocmd->reqs1.md, cmd->is_parity, max_txid_in_slice);
			dp_dbgdi_do_add_restore_info(cmd, true);	// The failed read blocks are already marked as such since we set poison into the writer magic before submitting, but the degraded (W) ones have not been sent to read and if we destroy them we must invalidate first (when restoring them we do mark it in reed solomon)
		}
		// TODO(EC-395) AND TODO(EC-400)
		// This will be implemented as part of EC-395/EC-400(future) - in our Gantt at line - 55/173(future) (probably will need to be updated)
		// Before writing data (probably missing data or just the empty buffer we would have wanted to
		// Fill by reed solomon) we mark the issue within our MD, since the first thing that we do
		// When we read this block the next time is to understand that the data is invalid for read
		// Thus it allows us a second chance of fixing it in the future if more sources become available
		// We need to optimize to destroy only the ((non-readable || non-restoreable) && writeable) segments
	}
}

// Virtual function called from so->destroy_function
/* 	sm:
	1. (Optimisation: not implemented) If there is a readable parity and it is neverwritten: mark the destroyed blocks as neverwritten.
	2. Else:
    	2.1. Need to turn-on dbits on dead segs.
		2.2. Destroy all writable blocks.
	The txid which will be assigned to destroyed blocks will be the max_txid in slice.
*/
void dp_ec_no_write_hole_destroy(struct recovery_sync_op *so) {
	__prep_write_perm_read_fail(so);
}

// Check rldr (so->cmds[0]) for result of reed solomon, set stage of pre-read
int dp_ec_no_write_hole_get_restore_rv(struct recovery_sync_op *so)
{
	return so->cmds[0].o_rv;
}

/***************** EC ONLY *****************/
static int dp_ec_sync_md_read_prepare_op(struct recovery_sync_op *so)
{
	struct nvmeibc_block_command *c = so->cmds;
	int i, rv = -ENOMEM;

	for (i = 0; i < c->ncmds; i++) {
		const u32 md_ssize = nvmeibc_sgmnt_sw_md_size(c[i].ds);
		struct nvmeibc_block_io_req *req = &c[i].iocmd->reqs1;
		dp_sync_cmd_init(so, i);

		if (!nvmeib_get_ndb(&c[i], 0, GFP_NOFS))		// MD ops need an NDB with correct length. SG table can be empty (0 entries).
			goto _out;

		req->ndb->length = NVMEIBC_SECTOR2BYTE(c[i].nlbas);
		if (md_ssize > 0) {
			if (!(req->md = nvmeibc_alloc_md(c[i].nlbas, md_ssize)))
				goto _out;
		} else {
			WARN(1, "nvmeibc bug! md_ssize=%d\n", md_ssize); // sanity
			req->md = NULL;
		}
	}
	rv = 0;
_out:
	return rv;
}

// EC-5585: Todo: Why do we need this function below, why not use nvmeibc_fill_ndb()
// Please review
// We use this to set pages into each commands ndb
// Instead of going over this again to populate gf_blocks anew, we can set each block directly in it's place in the 2D Matrix
void nvmeibc_fill_ndb_for_ec(struct multi_snake_slice_analyzer *mssa, int map_start, struct nvmeib_data_buffer *ndb, unsigned int nlbas, struct nps_block_iter *nbi)
{
	struct scatterlist *sg = ndb->table.sgl;
	const int snake_size = mssa->snake_size, replicas = mssa->replicas;
	ndb->table.nents = 0;
	ndb->length = NVMEIBC_SECTOR2BYTE(nlbas);
	BUG_ON(map_start % mssa->snake_size);
	while (nlbas) {
		unsigned int n_sg_blocks = 1;
		sg_set_page(sg, nps_block_iter_page(nbi), NVMEIBC_SECTOR2BYTE(n_sg_blocks), nps_block_iter_offset(nbi));
		nps_block_iter_advance(nbi, n_sg_blocks);
		ndb->table.nents++;
		nlbas -= n_sg_blocks;

		// MSSA fill pointer map - advance map entry index accordingly
		{	// This is why we call nvmeibc_fill_ndb_for_ec and not nvmeibc_fill_ndb
			mssa->gf_blocks.bio[map_start] = sg;
			advance_rlba_to_next_slice(map_start, snake_size, replicas);
		}
		if (!nlbas)
			sg_mark_end(sg);
		else
			sg_unmark_end(sg);
		sg = sg_next(sg);
	}
}

static void sync_set_metadata_in_mssa(struct multi_snake_slice_analyzer *mssa, int map_start, struct nvmeibc_block_command *cmd)
{
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	const int snake_size = mssa->snake_size, replicas = mssa->replicas, nlbas = (int)cmd->nlbas;
	int i;
	for (i=0;i<nlbas;i++) {
		mssa->blocks_md[map_start] = cmd->iocmd->reqs1.md + md_size * i;
		advance_rlba_to_next_slice(map_start, snake_size, replicas);
	}
}

static int dp_ec_sync_operation_page_manager(struct recovery_sync_op *so)
{
	struct multi_snake_slice_analyzer *mssa = so->o->mssa;
	struct nvmeibc_block_command *c = so->cmds;
	const int snake_size = mssa->snake_size;
	int i;
	struct nps_block_iter nbi = NPS_BLOCK_ITER_INIT(so->pages);

	for (i = 0; i < mssa->n_reads; i++) {
		struct nvmeibc_block_command *cr = &c[i];
		struct nvmeibc_block_command *cw = &c[i + mssa->n_reads];
		struct nvmeib_data_buffer *ndb;
		const u32 md_ssize = nvmeibc_sgmnt_sw_md_size(cr->ds);
		void *md;

		if (md_ssize > 0) {
			if (!(md = nvmeibc_alloc_md(so->n_slices, md_ssize)))		// Todo: Replication of code in dp_ec_sync_md_read_prepare_op(), unify both functions
				return -ENOMEM;
		} else {
			WARN(1, "nvmeibc bug! md_ssize=%d\n", md_ssize); // sanity
			md = NULL;
		}

		ndb = nvmeib_get_ndb(cr, so->n_slices /* nentries */, GFP_NOFS);
		if (!ndb) {
			nvmeibc_free_md(md);
			return -ENOMEM;
		}

		dp_sync_cmd_init(so, i);

		nvmeibc_fill_ndb_for_ec(mssa, i*snake_size, ndb, so->n_slices /* nlbas */, &nbi);
		cw->iocmd->reqs1.ndb =   cr->iocmd->reqs1.ndb = ndb;	// Copy ndb  of 'read' into write cmd
		cw->iocmd->reqs1.md =    cr->iocmd->reqs1.md =  md;		// Copy 'md' of 'read' into write cmd
		cr->do_not_realease_md = cr->is_not_ndb_owner = false;	// Already set to 0, just for clarity. read cmd will release all memory
		cw->do_not_realease_md = cw->is_not_ndb_owner = true;	// Write will not release memory
		sync_set_metadata_in_mssa(mssa, i, cw);
	}
	WARN(!nps_block_iter_empty(&nbi), "nvmeibc bug! unused extra blocks\n");
	return 0;
}

static int dp_ec_sync_read_write_prepare_op(struct recovery_sync_op *so)
{
	int rv = -ENOMEM;
	NFIN;
	so->is_sbs_mode = false; // Always start with full-blockset mode.
	//	TODO(DORON): At this stage we should have: Sync parity calculation will be the following -> make writes point to read sgls, calculate new parity into parity location, compare read parity with calculated, this first difference will trigger all writes, otherwise nothing to do
	if ((rv = nvmeibc_sync_alloc_mem_resources(so)) < 0)
		goto _out;
	if ((rv = dp_ec_sync_operation_page_manager(so)) < 0)
		goto _out;
	rv = 0;
_out:
	if (unlikely(rv<0))
		_NTSO(t01desrwpo, "failed! err=@ERR", rv);
	NFOUT;
	return rv;
}

void dp_ec_sync_no_write_hole_execute_op(struct recovery_sync_op *so)
{
	BUG_ON(dp_ec_no_write_hole_has_pre_sync_work(so)); // Just for debug
	if ( is_op_sync_db(so->o->op) && (!dp_ec_can_fix_dbits(so->cmds))) {
		return nvmeibcbdpec_return_to_caller_sm(so);
	} else {
		so->stage = sync_stage_recov_lo_all_taken;		// Daniel: This is redundant, should already be set.
		return dp_sync_no_write_hole_cb_stg_end(so);
	}
}
