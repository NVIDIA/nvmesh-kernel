/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_sync_no_write_hole.h"
#include "block/nvmeibc_topology.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_reed_solomon.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec.h"
#include "block/datapath_mirror/nvmeibc_block_dp_mirror.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_maintenance.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"

/* Link to spec (no writehole sync algorithm):
   https://docs.google.com/document/d/1v1uk-8fW8y8aEqZXmOrTj1cKvfYwAp_8NtwCe25RI8w/*/
/* NO WRITE HOLE SYNC:

   Mirror:
   NVMEIB_BLOCK_IO_OP_RECOVER_DB
	* Called by TOMA or any IO that sees a DB in the lock that is in TURN_OFF mode
		1. Send all possible reads (to all readable segments)
		2. Analyze return code and see that we have at least Slice size valid sources if so:
			a. Restore data from any valid source
		else: start slice by slice mode
			a. for each slice send all reads for a single slice
			b. if we do not have enough valid sources -> mark the slice uncorrectable
			c. TODO: When MD is enabled we can use it for Dirty bits per slice
		3. If all writes succeeded (do_not_send are considered successful) - Remove DB that are TURN OFF mode / Add DB that are in Turn ON (per slice DBs are correct)

   NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL:
	1. Triggered by any readfail IO
	2. Same as DB recovery from here

   NVMEIB_BLOCK_IO_OP_RECOVER_STALE:
	1. Triggered by any stale lock found
	2. Same as DB recovery since there is no write hole for mirror

   EC:
   NVMEIB_BLOCK_IO_OP_RECOVER_DB
	* Called by TOMA or any IO that sees a DB in the lock that is in TURN_OFF mode
	 Send all possible reads (to all RW segments)
		2. Analyze return code and see that we have at least Slice size valid sources if so:
			a. Restore data/parities and write back all slices
		else: start slice by slice mode
			a. for each slice send all reads for a single slice
			b. if we do not have enough valid sources -> mark the slice uncorrectable
			c. TODO: Add stage for each slice read P/Q and if a W seg is not degraded in P/Q MD read it as well (might overcome # of valid sources)
		3. If all writes succeeded (do_not_send are considered successful) - Remove DB that are TURN OFF mode / Add DB that are in Turn ON (per slice DBs are correct)

   NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL:
	1. Triggered by any readfail IO
	2. Same as DB recovery from here

   NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK:
	1. Triggered by cold recov
	2. Same as DB recovery from here.
	3. TODO: Rollback needs to regen also W segs with no dbits on them, Today DB recovery regens all W segs so it's ok. Needs to be changed in the future
*/

/******************************* PET trace helpers ****************************/

/* should_blockset_info_commit() is set at a stage where it must be clear. */
void pet_trace_binfo_commit_unexpected_at(const struct recovery_sync_op *so, u16 line) {
	if (!so || !so->o || !so->cmds)
		return;
	NVMEIBC_IO_PET_MSG_ERROR(&so->o->journal,
		"binfo_commit_unexpected(op=%hhu<enum nvmeib_block_io_op>, pre=0x%x<union nvmeib_blkset_info>, post=0x%x<union nvmeib_blkset_info>, stage=%hhu<enum sync_op_stage_e>, n_slices=%hhu, line=%hu)",
		numeric_downcast(u8, so->o->op), so->cmds->rld.pre.all, so->cmds->rld.post.all,
		numeric_downcast(u8, so->stage), numeric_downcast(u8, so->n_slices), line);
}

/* should_blockset_info_commit() is clear at a stage where it must be set. */
void pet_trace_binfo_commit_missing_at(const struct recovery_sync_op *so, u16 line) {
	if (!so || !so->o || !so->cmds)
		return;
	NVMEIBC_IO_PET_MSG_ERROR(&so->o->journal,
		"binfo_commit_missing(op=%hhu<enum nvmeib_block_io_op>, pre=0x%x<union nvmeib_blkset_info>, post=0x%x<union nvmeib_blkset_info>, stage=%hhu<enum sync_op_stage_e>, n_slices=%hhu, line=%hu)",
		numeric_downcast(u8, so->o->op), so->cmds->rld.pre.all, so->cmds->rld.post.all,
		numeric_downcast(u8, so->stage), numeric_downcast(u8, so->n_slices), line);
}

/* post != pre at EC SM entry — caller must not have modified post before entry. */
void pet_trace_binfo_entry_mismatch_at(const struct recovery_sync_op *so, u16 line) {
	if (!so || !so->o || !so->cmds)
		return;
	NVMEIBC_IO_PET_MSG_ERROR(&so->o->journal,
		"binfo_entry_mismatch(op=%hhu<enum nvmeib_block_io_op>, pre=0x%x<union nvmeib_blkset_info>, post=0x%x<union nvmeib_blkset_info>, n_slices=%hhu, line=%hu)",
		numeric_downcast(u8, so->o->op), so->cmds->rld.pre.all, so->cmds->rld.post.all,
		numeric_downcast(u8, so->n_slices), line);
}

/* nwhole params or exec-plan invariant violated — captures params and stage for post-mortem. */
void pet_trace_nwhole_param_err_at(const struct recovery_sync_op *so, u16 line) {
	if (!so || !so->o || !so->cmds)
		return;
	NVMEIBC_IO_PET_MSG_ERROR(&so->o->journal,
		"nwhole_param_err(op=%hhu<enum nvmeib_block_io_op>, pre=0x%x<union nvmeib_blkset_info>, stage=%hhu<enum sync_op_stage_e>, params=0x%llx<union no_writehole_params>, line=%hu)",
		numeric_downcast(u8, so->o->op), so->cmds->rld.pre.all,
		numeric_downcast(u8, so->stage), so->nwhole_params.raw, line);
}

/******************************* SBS *****************************************/
u32 nvmeibc_sync_sl_by_sl_is_get_current_slice_index(const struct recovery_sync_op *so)
{
	const u32 sl_idx = ((so->start_slice + so->slice_by_slice_index) * 32) / LOCKSET_SLICES;	// For 512[b] blocks saves results of 8 slices into a single bit
	BUG_ON(sl_idx >= 32);	// All sync bitmaps are 32 bits long. Designed for slices 0..32 or 0..511 in groups of 8.
	return sl_idx;
}

#define nvmeibc_sync_sl_by_sl_get_bit_index(so) \
	(1U << nvmeibc_sync_sl_by_sl_is_get_current_slice_index(so))

bool nvmeibc_sync_sl_by_sl_is_current_slice_destroyed(const struct recovery_sync_op *so)
{
	const u32 slice_bit = nvmeibc_sync_sl_by_sl_get_bit_index(so);
	return (so->write_unco_mask & slice_bit) != 0;
}

static inline void __dp_sync_no_write_hole_clean_bad_sector_and_do_not_send_comp_code_and_rv_from_read_cmds(struct recovery_sync_op *so) {
	struct nvmeibc_block_command *rldr = so->cmds;
	int c, n_reads = n_read_cmds(so);
	for (c = 0; c < n_reads; c++) {
		if ((!is_transient_disk_error(rldr[c].o_rv)) || rldr[c].do_not_send) {	// Reset all read fail or do_not_send errors
			__cmd_set_comp_err(&rldr[c], 0);
		}
	}
}

// Future optimization - When MSSA is in SBS mode, retain all existing buffers and reuse them
// If is_valid_for_reuse set, this way we only read the readfailed segs of each slice

/* Update all commands to start from last slice, and start loop over slices
	Slices loop: do { i = n_slices; i--; process[i]; } while (i != 0);
   each iteration (including the first) will do addr--
   reasoning is that we know we are done in the last write phase when
   slice_by_slice index is 0. Happens in 2 cases:
   1. Full blockset fixed (index remains 0)
   2. Slice by slice finished the last iteration (we reduce index to 0 after writing slice 1)
      read slice 0 and after writing it we complete the sync. */
static void nvmeibc_sync_sl_by_sl_start(struct recovery_sync_op *so)
{
	struct multi_snake_slice_analyzer *mssa = so->o->mssa;
	int i;
	struct nps_block_iter nbi = NPS_BLOCK_ITER_INIT(so->pages);

	if (unlikely(so->is_sbs_mode)) {
		pet_trace_nwhole_param_err(so);
		WARN(1, "nvmeibc bug, wrong flow=%d, already in sbs mode\n", so->slice_by_slice_index);
	}
	_NTSO(trace_dp_sync_no_write_hole_sl_by_sl_start, "Starting slice by slice mode");
	so->slice_by_slice_index = so->n_slices;	// Start from last slice towards first slice. +1, becasue we call next() after this function.
	so->n_cmds = n_read_cmds(so);
	so->last_cmd = so->n_cmds - 1;
	// Set map check to 1 slice only
	if (mssa) {
		mssa->map_size = mssa->snake_size * mssa->replicas;
	}
	for (i = 0; i < so->cmds->ncmds; i++) {
		struct nvmeibc_block_command *cmd = &so->cmds[i];
		struct nvmeib_data_buffer *ndb = cmd->iocmd->reqs1.ndb;
		cmd->iocmd->reqs1.disk_address += so->slice_by_slice_index; // We overshoot the block address and always call next after start so we decrement later
		if (so->r1->slice_size == 1) { // Simplify for mirror (each slice is --)
			cmd->first_rlba += so->slice_by_slice_index;
		}
		cmd->nlbas = 1;
		if (i < n_read_cmds(so)) {
			// From the entire 128K, we use only the first block (Single slice)
			if (mssa)
				nvmeibc_fill_ndb_for_ec(mssa, i*mssa->snake_size, ndb, 1, &nbi);
			else
				nvmeibc_fill_ndb(								  ndb, 1, &nbi);
		} else if (ndb) {
			// Write NDB should be shared with read and already updated
			BUG_ON(!cmd->is_not_ndb_owner);
			BUG_ON(ndb->length != NVMEIBC_SECTOR_SIZE);
		}
		__cmd_clean_comp_val(cmd);					// Reset all cmds rv
	}
	so->is_sbs_mode = true;
}

bool nvmeibc_notify_toma_on_slice_by_slice_destruction_in_sync = true;				// Default true
static void __log_corruption(const struct recovery_sync_op *so, const char* str)
{
	const struct nvmeibc_subscription_ctx *tr = so->locks->ds->toma_reg;
	_NI_to_user(t01_bdsect_fix, DMESG_PREFIX("@DEV_NAME"), "@TOPO_DBG_ID(@CHUNK_IDX,@PRAID_IDX), @RLBA, op=@X, bmp{fixed=@BITMAP, uncor=@BITMAP} , Error code: 1068: @STR", so->o->nd->name, so->o->topo->debug_unique_index, tr->ch, tr->r1, so->rlba, so->o->op, so->write_fix_mask, so->write_unco_mask, str);
}

static void __notify_toma_on_slice_by_slice_comp(const struct recovery_sync_op *so, int err)
{
	int i, n_reads = n_read_cmds(so);
	if (unlikely(n_reads != so->r1->replicas)) {
		pet_trace_nwhole_param_err(so);
		WARN_ON(1);
	}
	if (!nvmeibc_notify_toma_on_slice_by_slice_destruction_in_sync)
		return;
	for (i = 0; i < n_reads; i++) {
		struct nvmeibc_d_iocmd_comp *comp = &so->cmds[i].iocmd->comp;
		const int tmp = comp->comp_code;
		if (so->cmds[i].ds->toma_acm == NVMEIBTC_DS_MODE_DEAD) {
			WARN((!so->cmds[i].do_not_send), "nvmeibc bug, dead segment is not do_not_send\n");      // Just a trap
			continue;
		}
		comp->comp_code = err;
		__send_toma_cmd_help(comp, so->write_unco_mask, so->write_fix_mask);
		comp->comp_code = tmp;
	}
}

/* We completed slice by slice return original values to cmds */
static inline void __transition_from_slice_back_to_sync_mode(struct recovery_sync_op *so)
{
	int i;
	struct multi_snake_slice_analyzer *mssa = so->o->mssa;
	struct nps_block_iter nbi = NPS_BLOCK_ITER_INIT(so->pages);
	for (i = 0; i < so->cmds->ncmds;i++) {
		struct nvmeibc_block_command *cmd = &so->cmds[i];
		// Note: cmd->first_rlba is automatically back to normal
		cmd->nlbas = so->n_slices;
		if (i < n_read_cmds(so)) {
			// Restore NDB length to n_slices blocks
			if (mssa) { // Is this required or just busy work?
				nvmeibc_fill_ndb_for_ec(mssa, i*mssa->snake_size, cmd->iocmd->reqs1.ndb, so->n_slices /* nlbas */, &nbi);
			} else nvmeibc_fill_ndb(cmd->iocmd->reqs1.ndb, so->n_slices /* nlbas */, &nbi);
		}
		else if (cmd->iocmd->reqs1.ndb) {
			// Write NDB should be shared with read and already updated
			BUG_ON(!cmd->is_not_ndb_owner);
			BUG_ON(cmd->iocmd->reqs1.ndb->length != (u32)NVMEIBC_SECTOR_SIZE * so->n_slices);
		}
	}
	so->is_sbs_mode = false;
	so->nwhole_exec_plan.was_in_sbs_mode = true;
}

/* Analyze entire slice by slice run, notify tomas regarding fixed/failed/uncorrectable slices and restore recovery cmds to original values */
static void nvmeibc_sync_sl_by_sl_finish(struct recovery_sync_op *so)
{
	_NTSO(t_01_sbs_fin, "Finished sbs: slice=@SBS_INDEX, bmp: fixed=@BITMAP, uncor=@BITMAP, err=@ERR", so->slice_by_slice_index, so->write_fix_mask, so->write_unco_mask, so->error);
	if (!so->error) { // Sync has no error see if we fixed/wrote uncorrectable
		if (so->write_unco_mask) { 	// 1 or more broken slices, possibly some slices were fixed
			const struct nvmeibc_subscription_ctx *tr = so->locks->ds->toma_reg;
			NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: Blockset Destroy! vol=@DEV_NAME<@TOPO_DBG_ID>(@CHUNK_IDX,@PRAID_IDX), RLBA: @RLBA, bmp: fixed=@BITMAP, uncor=@BITMAP,", _T, goodpath_nvmeibc_syncs, t_02_sbs_fin,
				so->o->dbg_id, so->o->nd->name, so->o->topo->debug_unique_index, tr->ch, tr->r1, so->rlba, so->write_fix_mask, so->write_unco_mask);
			__log_corruption(so, "multiple disasters data loss");			// Or else we would not enter slice by slice mode
			__notify_toma_on_slice_by_slice_comp(so, EPERM_READ_FAIL_NO_RETRY);
			so->error = EPERM_READ_FAIL_NO_RETRY; // No!!!! This is a bug! Caller should verify if the requested slice is uncorrectable
			// TODO for above line, add another counterpart with the error, i.e. Non-sync error, rather data error,
			// where the caller must analyze both values, (if failed-> handle failure, if write_uncor -> is requested data affected (use the mask)
			// mark original IO as EPERM_READ_FAIL_NO_RETRY (not from the so->error, rather a parallel mechanizm)
			nvmeibcb_dp_io_fail_mgr_inspect(so->cmds, 0);        // LKJ: First command, might not have any error, just using it as api to function
		} else if (so->write_fix_mask) { // Some slices fixed, no slices are broken, We fixed some read fail error, use EPERM_READ_FAIL to notify TOMA
			__notify_toma_on_slice_by_slice_comp(so, EPERM_READ_FAIL);
		}
	}
	__transition_from_slice_back_to_sync_mode(so);	// transition back to orig sync op
}

/* Update all existing commands to be on the next slice */
static void nvmeibc_sync_sl_by_sl_next(struct recovery_sync_op *so, const enum sync_op_stage_e next_stage)
{
	const struct nvmeibc_datapath *dp = &so->o->nd->dp;
	const int snake_size = dp->p.snake_size, replicas = so->r1->replicas, slice_size = so->r1->slice_size;
	int i;
	nvmeibc_sync_prepare_so_for_read(so, NULL, next_stage); // prepare so for general read stage
	BUG_ON(((u32)so->start_slice + (u32)so->slice_by_slice_index) > LOCKSET_SLICES);
	BUG_ON(so->slice_by_slice_index == 0);	// [1..LOCKSET_SLICES]
	for (i = 0; i < so->cmds->ncmds; i++) {
		struct nvmeibc_block_command *cmd = &so->cmds[i];
		cmd->iocmd->reqs1.disk_address--;	// Addr--, once initialization to 32.
		if (!i) {
			if (slice_size == 1) {
				cmd->first_rlba--;
			} else { // Non mirror has multiple considerations
				// Calculate from disk address once (if we store the original blockset start we can add the mssa rlba index (parity for D0, instead of dlba_to_rlba)
				// This can be optimized becuase after knowing snake_start_rlba of cmd[0] we automatically know cmd[i].first_rlba = cmd[0].first_rlba + snake_size*i:
				// nvmeibc_datapath_dlba_to_rlba(dp, so->r1, 0, so->cmds[0].iocmd->reqs1.disk_address) + i*dp->p.snake_size); for Data, i=0 for parity
				cmd->first_rlba = nvmeibc_datapath_dlba_to_rlba(dp, so->r1, cmd->ds->toma_reg->seg, cmd->iocmd->reqs1.disk_address);
			}
		} else {
			if (slice_size == 1) { // If slice_size == 1 we can optimize to just copy from 0
				cmd->first_rlba = so->cmds[0].first_rlba;
			} else { // Non mirror
				if (i < replicas) { // From slice start add snake size by column
					if (i < slice_size) {
						cmd->first_rlba = so->cmds[0].first_rlba + (i * snake_size);
					} else { // Parity rlba is the same as D0 (works for mirror to)
						cmd->first_rlba = so->cmds[0].first_rlba;
					}
				} else { // Writes can copy from reads
					cmd->first_rlba = so->cmds[i - replicas].first_rlba;
				}
			}
			__cmd_clean_comp_val(cmd);
		}
		DEBUG_TRANSFERS_init_cb_counter(cmd);
	}

	if (dp->nwhole_funcs.sbs_cleanup) // Only mirror has SBS cleanup - Sets write buffers to NULL TODO(EC-2522) for EC
		dp->nwhole_funcs.sbs_cleanup(so);
	so->slice_by_slice_index--;			// Must be at the end of function, because cleanup may rely on this value
}

/******************************* Generic funcs *******************************/
static inline struct dp_ec_restore_plan prepare_plan_for_parity_md_dbits_change(const struct recovery_sync_op *so, const enum dp_ec_dirty_bits_operation db_op) {
	const int n_parities = nvmeibc_raid1_get_protect_lvl(so->r1);
	const u16 slice_size = so->r1->slice_size;
	const roles_bmp_t write_bmp = (db_op == DBITS_OP_TURN_ON) ? so->nwhole_exec_plan.first_write_bmp : so->nwhole_exec_plan.second_write_bmp;
	return (struct dp_ec_restore_plan){ .parity_rebuild_required = true, .invalid_sources = so->nwhole_exec_plan.invalid_sources, .parity_target_mask = (GENMASK(n_parities-1,0) & (write_bmp >> slice_size)), .dbits_op = db_op };
}

static inline void __parity_md_dbits_turnon(struct recovery_sync_op *so) {
	prepare_parity_md(so, prepare_plan_for_parity_md_dbits_change(so, DBITS_OP_TURN_ON));
}

static inline void __parity_md_dbits_turnoff(struct recovery_sync_op *so) {
	prepare_parity_md(so, prepare_plan_for_parity_md_dbits_change(so, DBITS_OP_TURN_OFF));
}

/* Prepares lock DB values for dbits turnoff */
static void __calc_new_binfo_dbits_turnoff(struct recovery_sync_op *so) {
	BUG_ON(__is_raid1_mirror(so));  // Currently not being used in mirror, needed for dbits in metadata
	BUG_ON(!so->nwhole_params.must_turn_off_dbits); // Today no_whole sync never turn off dbits unless must_turn_off_dbits is ture, if changed in the future this bug_on() can be removed.
	if (dp_ec_can_fix_dbits(so->cmds)) {
		struct nvmeibc_raid_leader_cmd_ctx *rld = &so->cmds->rld;
		struct nvmeibc_dbits_tx tx;
		struct nvmeibc_dbits_tx tx_turnon;
		union nvmeibc_dbits_entry pre = {.all_bits = rld->pre.bits.dirty};

		/* EC-4697: Dead parities become dirty, because parities contain dirty which is essential data, must be updated */
		const sgmnts_bmp_t turn_on_dbit_bmp = nvmeibc_calc_db_on_parities_segs(so->cmds);
		const sgmnts_bmp_t turn_off_dbit_bmp = (nvmeibc_mssa_calc_full_blockset_write_bmp(so->o->mssa) & nvmeibc_raid1_get_sgmnts_bmp(so->r1, dbits_off_mask));

		nvmeibc_dbits_tx_init_by_bmp(&tx_turnon, &so->r1->calculated_data.topo_traits, turn_on_dbit_bmp, 0 /* turn_off_dbit_bmp */, 0 /* turn_on_conv_bmp */);
		nvmeibc_dbits_tx_init_by_bmp(&tx, &so->r1->calculated_data.topo_traits, turn_on_dbit_bmp, turn_off_dbit_bmp, 0 /* turn_on_conv_bmp */);

		so->nwhole_exec_plan.ram_dbits_after_turnoff.all_bits = nvmeibc_dbits_tx_apply(&pre, &tx);
		so->nwhole_exec_plan.ram_dbits_after_first_turnon.all_bits = nvmeibc_dbits_tx_apply(&pre, &tx_turnon);
		so->nwhole_exec_plan.should_turnoff_dbits = true;
		so->nwhole_exec_plan.should_write_ram_dbits_first = !!tx_turnon.action.db_turn_on_bmp && (so->nwhole_exec_plan.ram_dbits_after_first_turnon.all_bits != rld->pre.bits.dirty);
		if (!!tx_turnon.action.db_turn_on_bmp) {
			/* EC-4697: If we turn off on some parity, and another parity is dead - turn on on dead parity */
			so->nwhole_params.dbits_turnon_bmp |= nvmeibc_calc_db_on_parities_roles(so->cmds);
			dp_sync_calc_new_binfo_dbits_turnon(so, tx_turnon.action.db_turn_on_bmp);  /* Prepares lock DB values for dbits turnon due to rollback */
		}

		if (unlikely((pre.all_bits == 0)||(so->nwhole_exec_plan.ram_dbits_after_turnoff.all_bits == rld->pre.bits.dirty))) {
			pet_trace_nwhole_param_err(so);
			WARN(1,
			  "nvmeibc bug: pre=%d, post=%d\n", rld->pre.bits.dirty, rld->post.bits.dirty);	// Just a sanity check..
		}
	} else { /* We are in read-fail fixup and no dbits can be turned off */
		if (unlikely(so->nwhole_params.raw == no_writehole_params_default.raw)) {
			pet_trace_nwhole_param_err(so);
			WARN(1, "No write hole sync called with only turnoff dbits params but there are no dbits to turnoff\n");
		}
	}
}

static void __update_new_binfo_dbits_on_destroy(struct recovery_sync_op *so, const sgmnts_bmp_t dead_bm) {
	/* Turnon dbits on dead segs */
	struct nvmeibc_dbits_tx tx;
	struct nvmeibc_raid_leader_cmd_ctx *rld = &so->cmds->rld;
	union nvmeibc_dbits_entry pre = {.all_bits = rld->pre.bits.dirty};
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t dead_roles = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, dbits_on_mask);
	BUG_ON(__is_raid1_mirror(so));  // Currently not being used in mirror, needed for dbits in metadata

	nvmeibc_dbits_tx_init_empty(&tx, &so->r1->calculated_data.topo_traits);
	tx.action.db_turn_on_bmp = dead_bm;
	so->nwhole_params.dbits_turnon_bmp |= dead_roles;
	if (so->nwhole_params.must_turn_off_dbits && so->nwhole_exec_plan.should_turnoff_dbits) {
		so->nwhole_exec_plan.ram_dbits_after_turnoff.all_bits = nvmeibc_dbits_tx_apply(&so->nwhole_exec_plan.ram_dbits_after_turnoff, &tx);
	}
	so->nwhole_exec_plan.ram_dbits_after_first_turnon.all_bits = nvmeibc_dbits_tx_apply(&pre, &tx);  // We either way turning on on all dead segs so recalculate
	so->nwhole_exec_plan.should_write_ram_dbits_first = !!dead_bm && (pre.all_bits != so->nwhole_exec_plan.ram_dbits_after_first_turnon.all_bits);
}

/********************** Mirror DB Inspection - TBD ***************************/
static roles_bmp_t __get_writable_parities_bmp_for_dbits_metadata(const struct recovery_sync_op *so)
{	// We need that only if parities are actually storing dbits on disk. Update those parities when dbits are changed (turn on / off)
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t writable_bmp = nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, dead);
	return (roles_bmp_t)(nvmeibc_raid1_get_parities_bmp(so->r1) & writable_bmp);
}

static roles_bmp_t __get_dirty_roles_bmp_pre_sync(const struct recovery_sync_op *so, int slice_start)
{
	const union nvmeibc_dbits_entry pre_db = { .all_bits = so->cmds->rld.pre.bits.dirty };
	const sgmnts_bmp_t pre_db_sgmnts_bmp = nvmeibc_dbits_get_turn_on_bmp(&pre_db, &so->r1->calculated_data.topo_traits);
	return (roles_bmp_t)ror32_width(pre_db_sgmnts_bmp, slice_start, so->r1->replicas);
	// const int slice = (so->is_sbs_mode ? so->slice_by_slice_index : -1);	// When dbit in metadata is implemented, use it to get per slice dbit and decide on validity of this read
}

static inline void __calc_execution_plan_for_dbits_turnoff(struct recovery_sync_op *so) {
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t pre_db_bmp = __get_dirty_roles_bmp_pre_sync(so, slice_start);
	const roles_bmp_t fixable_binfo_dbits_bmp = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, dbits_off_mask) & pre_db_bmp;
	BUG_ON(!so->nwhole_params.must_turn_off_dbits);			// This function assumes we are turning off possible dbits in ram.
	BUG_ON(!nvmeibc_raid_is_ec(so->r1)); // No unknowns dbits should exist, EC resolves them in advance, but R1 does not (calls __mark_read_to_dirty_w_seg_as_do_not_send). This will screw up the fixable bitmap
	if (fixable_binfo_dbits_bmp) {  // Add writable parities.
		so->nwhole_exec_plan.first_write_bmp |= fixable_binfo_dbits_bmp; // In fix dbits is possible and needed, need to change the data itself in the first write TOODO(LKJ): optimization: need to change the non readable parities md. //(fixable_binfo_dbits_bmp | (nvmeibc_raid1_get_parities_bmp(so->r1) & writable_non_readable));
		so->nwhole_exec_plan.second_write_bmp = __get_writable_parities_bmp_for_dbits_metadata(so);  // Write the parities with dbits turned off
	}
	BUG_ON((bool)(fixable_binfo_dbits_bmp != 0) != dp_ec_can_fix_dbits(so->cmds));	// Sanity, 2 calcualtions yield the same result
}

static inline void __dump_nwhole_exec_plan(struct recovery_sync_op *so, bool should_restore, bool should_only_scrub, bool destroy_slice, bool start_slice_by_slice) {
	const struct no_writehole_execution_plan *p = &so->nwhole_exec_plan;
	_NTSO(trace_06_no_whole_sm, "sbs{start=@BOOL_YN, destroy=@BOOL_YN, is_sbs=@BOOL_YN, index=@SBS_INDEX}, first_write_bmp=@BITMAP, second_write_bmp=@BITMAP, invalid_sources=@BITMAP, ram_dbits_before_turnoff=[@DBITS], should_turnoff_dbits=@BOOL, ram_dbits_after_turnoff=[@DBITS], binfo_txid=@TXID, should_restore=@BOOL, should_scrub=@BOOL",
		  start_slice_by_slice, destroy_slice, so->is_sbs_mode, so->slice_by_slice_index, p->first_write_bmp, p->second_write_bmp, p->invalid_sources,
		  so->cmds->rld.pre.bits.dirty, p->should_turnoff_dbits, p->ram_dbits_after_turnoff.all_bits, so->cmds->rld.post.bits.txid, should_restore, should_only_scrub);
}

static inline u32 __scrub_blockset(struct recovery_sync_op *so) {
	// All edics already match their respective D/P blocks, or else we would not call scrubbing but read-fail fixup
	BUG_ON(!is_op_sync_scrubbing(so->o->op));
	BUG_ON(so->nwhole_exec_plan.invalid_sources != 0);
	BUG_ON(so->error);
	if (__is_raid1_mirror(so)) {
		return dp_mirror_calc_scrub_writes(so);
	} else { // Todo: Encapsulate this block of code as dp_ec_calc_scrub_writes()
		u32 wrong_slices_mask, b;
		so->o->mssa->target_bmp = nvmeibc_raid1_get_parities_bmp(so->r1);
		nvmeibc_sync_set_uncompleted_cmds(so, nvmeibc_raid1_get_protect_lvl(so->r1)+1); // Allow reed solomon "async" completion to end, then set value accordingly
		if (!so->is_sbs_mode) {
			for (b = 0; b < so->cmds[0].nlbas; b++) {	// Slice iterator just for comparing that both parities have identical never-written status
				(void)dp_sync_is_slice_neverwritten(so, b, ~0, 'R');
			}
			if (so->nwhole_exec_plan.is_blkset_corrupted) {
				// We copied the parity so edic is now incorrect (with very high probability) and scrubbing should detect and fix it
			}
		}
		wrong_slices_mask = apply_gf_calculation_for_operation(so->o, 0, 0/*so->error*/);
		return (so->is_sbs_mode) ? (u32)(wrong_slices_mask << nvmeibc_sync_sl_by_sl_is_get_current_slice_index(so)) : wrong_slices_mask; // In slice by slice mode, shift the single bit result for current slice to its place
	}
}

static roles_bmp_t __calc_invalid_source(const struct recovery_sync_op *so, const roles_bmp_t readfail_bmp, const int slice_start)
{	// Todo: Here use per slice info in metadata / dbits-binfo if there is no dirty convict
	const roles_bmp_t pre_db_bmp = __get_dirty_roles_bmp_pre_sync(so, slice_start);
	const roles_bmp_t invalid_by_topology = nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, readable_sync);
	if (nvmeibc_raid_is_ec(so->r1) || (so->R1.is_dirty_suspect))		// Todo, remove this test for EC. Historically, EC still does not use 'W' for read, even if there is no dbit for it
		return readfail_bmp | nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, readable);
	else // In R1 we do not resolve unknowns as optimization to favour reads over writes.
		return readfail_bmp | invalid_by_topology | pre_db_bmp;	// This is the correct calculation
}

bool ec_8005_enable_warning = true; //will be used by um simulator to disable warning
/* Read phase analysis:
   1. Create bit masks of all required fixes (for dbits turnoff - readfailed reads and DB turn off, for dbits turnon - if (writable non readable parities): [eadfailed reads and DB turn off] else: [0 and goto turnon stage]).
   2. Create bit mask of invalid sources (to determine if fix is possible)
   3. If there is a target mask or stale lock sync (Mirror only)
      a. If we have enough sources fix entire blockset / slice (depending on mode)
      b. If we do not have enough sources:
            1. Start SBS mode
         2. If already in SBS mode destroy this slice
   4. If in SBS mode and no target mask continue to next slice. (Assert if in turnon dbits stage)
   5. If no restore_bmp if in parity_md dbits turon stage turnon dbits on parities.  (else if in dbits turnon stage - assert, probably something odd happened). */
static enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE __analyze_no_write_hole_read(struct recovery_sync_op *so, const roles_bmp_t readfail_bmp)
{
	enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE rv = GOTO_NEXT_STAGE;
	const int slice_start = so_get_owner_seg(so);
	bool should_restore = false, should_only_scrub = false;
	bool is_first_call_to_analayze = (!so->is_sbs_mode); // At the first loop iteration always so->is_sbs_mode = false if calling twice will always be true
	bool destroy_slice = false;
	bool start_slice_by_slice = false;
	const bool enable_store_dirty_bits_in_peristent_md = so->o->nd->dp.enable_store_dirty_bits_in_peristent_md;
	// After using for checking readfails, clean the bad_sectors and do_not_send comp_codes and_rv
	__dp_sync_no_write_hole_clean_bad_sector_and_do_not_send_comp_code_and_rv_from_read_cmds(so); // Clean up for SBS next stage (read RVs already analyzed) clean it also before return
	so->nwhole_exec_plan.encountered_bad_sectors |= (readfail_bmp != 0);
	so->nwhole_exec_plan.invalid_sources = __calc_invalid_source(so, readfail_bmp, slice_start);
	so->nwhole_exec_plan.first_write_bmp = so->nwhole_params.force_rebuild_bmp;  // Starting value - empty or specifically requested by caller
	so->nwhole_exec_plan.second_write_bmp = 0;  // Starting value - empty, needed only if dbits exist in metadata
	if (enable_store_dirty_bits_in_peristent_md) {
		if (so->nwhole_params.must_turn_off_dbits && is_first_call_to_analayze)
			__calc_new_binfo_dbits_turnoff(so);
		if (so->nwhole_params.dbits_turnon_bmp)
			so->nwhole_exec_plan.first_write_bmp |= __get_writable_parities_bmp_for_dbits_metadata(so);
		if (so->nwhole_params.must_turn_off_dbits)
			__calc_execution_plan_for_dbits_turnoff(so);
	}
	if (so->nwhole_params.must_fix_bad_sectors)
		so->nwhole_exec_plan.first_write_bmp |= readfail_bmp;

	{	// Sanity check that we are not using dead segments.
		const roles_bmp_t dead_bmp = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, dead);
		const roles_bmp_t any_write = (so->nwhole_exec_plan.first_write_bmp | so->nwhole_exec_plan.second_write_bmp);
		const bool wrong_usage_of_dead_seg = ((dead_bmp & (readfail_bmp | any_write | so->nwhole_params.force_rebuild_bmp)) != 0);
		if (unlikely(wrong_usage_of_dead_seg)) {
			pet_trace_nwhole_param_err(so);
			WARN(1, "nvmeibc bug using data from dead seg! bmp{d=0x%x, w1=0x%x, w2=0x%x, rf=0x%x, force=0x%x}\n", dead_bmp, so->nwhole_exec_plan.first_write_bmp, so->nwhole_exec_plan.second_write_bmp, readfail_bmp, so->nwhole_params.force_rebuild_bmp);
		}
		if (__is_raid1_mirror(so)) {	// Check if we need to restore blocks (need to write something we could not read)
			extern void __find_best_valid_source_for_data(struct recovery_sync_op *so);
			__find_best_valid_source_for_data(so);	// Finding 1 seg with source of data is the restore step and a single algorithm for all r1 problems
			should_restore = true; // R1: comparing the read blocks must always be done and write what is not identical to the source
		} else {		// EC
			should_restore = (any_write & so->nwhole_exec_plan.invalid_sources);	// EC: Missing data that we need to write. Must resotre it
		}
	}

	// Check if not enough sources for restore
	if (should_restore && ((int)hweight32(so->nwhole_exec_plan.invalid_sources) > nvmeibc_raid1_get_protect_lvl(so->r1))) {
		if (so->is_sbs_mode) { // Not enough sources destroy slice
			const u32 slice_bit = nvmeibc_sync_sl_by_sl_get_bit_index(so);
			so->write_unco_mask |= slice_bit;
			destroy_slice = true;
		} else {          // Not enough sources try to fix in slice granularity.
			start_slice_by_slice = true;
		}
	}

	if (destroy_slice && nvmeibc_raid_is_ec(so->r1)) {	// Turn on dbits for dead segments in destroyed slice
		const sgmnts_bmp_t dead_bm = nvmeibc_raid1_get_sgmnts_bmp(so->r1, dbits_on_mask);
		if (!!dead_bm) { // on destroy need to turnon dbits on dead segs.
			__update_new_binfo_dbits_on_destroy(so, dead_bm);
			dp_sync_calc_new_binfo_dbits_turnon(so, dead_bm);  /* Prepares lock DB values for dbits turnon due to rollback */
		}
	}

	if ((so->nwhole_params.dbits_turnon_bmp) && (enable_store_dirty_bits_in_peristent_md)) {
		__parity_md_dbits_turnon(so);	// prepare MD before first write
	}
	if (so->nwhole_params.must_scrub) {
		WARN_ONCE((!!nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, readable)), "nvmeibt bug!, scrub isn't supported on degraded topo.\n");
		should_only_scrub = (so->nwhole_exec_plan.invalid_sources == 0);	// Scrub only in no bad sectors. Otherwise, fix bad sector
	}

	__dump_nwhole_exec_plan(so, should_restore, should_only_scrub, destroy_slice, start_slice_by_slice);
	if (should_only_scrub) { // Currently we only verify data if there are no degaraded segs at all.
		// If we can fix DBs from scrub context we should have mutated into DB sync (which is stronger than scrubbing, since we assume Ps are valid for restoration)
		const u32 scrub_result = __scrub_blockset(so);
		nvmeibc_sync_set_uncompleted_cmds(so, 0); // All async completions finished
		if (scrub_result) { // Scrubber found a problem, we need to write all calculated parities
			so->nwhole_exec_plan.first_write_bmp = nvmeibc_raid1_get_parities_bmp(so->r1);
			so->stage = sync_stage_recov_no_write_hole_restore_complete;	// Need to write the fixed stuff
			so->write_fix_mask |= scrub_result;
			return GOTO_NEXT_STAGE;
		}	// No write whole scrub complete no errors found in (slice/blockset)
		if (so->is_sbs_mode) {
			so->stage = sync_stage_recov_no_write_hole_sbs_loop_end;
			return GOTO_NEXT_STAGE;
		}
		return NO_WRITE_HOLE_DONE;
	}

	// Prepare data for first write (restore and change dbits)
	if (should_restore) { 				// Target mask means either RF that needs restore/copy, or W seg with dirty bits
		const u32 slice_bit = nvmeibc_sync_sl_by_sl_get_bit_index(so);
		if (destroy_slice) { // Mark slice destroyed rv == GOTO_NEXT_STAGE
			so->o->nd->dp.nwhole_funcs.destroy_function(so);	// Write unrecoverable ready for sending.
		} else if (start_slice_by_slice) {
			rv = START_SLICE_BY_SLICE_MODE; 			// Start SBS
			goto out; // Stage will be set outside this function.
		// TODO: W segments that are not dirty in the slice are valid sources. Need to send P/Q first and unset do_not_send for W segs that are not dirty (or any other solution)
		} else { // Fix slice/blockset from valid sources
			if (so->is_sbs_mode && readfail_bmp) {
				so->write_fix_mask |= slice_bit;
			}
			so->stage = sync_stage_recov_no_write_hole_restore_complete;
			rv = so->o->nd->dp.nwhole_funcs.restore_function(so); // EC runs asynchronously / Mirror is Sync (for now)
											//^^^^^^ Assumption restore_function will turn on dbits
			so = NULL;
			goto out;
		}
	}

	// Calc next stage
	if (__is_raid1_mirror(so)) {
		so->stage = sync_stage_recov_no_write_hole_restore_complete;
	} else if (so->nwhole_exec_plan.first_write_bmp) {
		so->stage = sync_stage_recov_no_write_hole_restore_complete;
	} else if (so->nwhole_exec_plan.second_write_bmp) {
		pet_trace_nwhole_param_err(so);
		WARN(true, "If first_write_bmp=0 it's impossible to have second_write_bmp cause we havne't restored nothing.\n");
		so->stage = sync_stage_recov_no_write_hole_sent_restore_data_and_turnon_parity_md_dbits_done;
	} else if (so->is_sbs_mode) {
		so->stage = sync_stage_recov_no_write_hole_sbs_loop_end;
	} else if (so->nwhole_params.must_scrub) {
		so->stage = sync_stage_recov_write_cmds_done;  // This blockset only needed scurbbing, Nothing should be written
	} else {  // !so->is_sbs_mode and nothing todo
		const bool is_turnon_with_no_writable_pari = so->nwhole_params.dbits_turnon_bmp && !(nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, has_writable_pari));
		if (unlikely(!is_turnon_with_no_writable_pari && ec_8005_enable_warning)) {
			pet_trace_nwhole_param_err(so);
			WARN(1, "EC-8005 - EC No write hole sync with nothing to do - with very low chances can be a bad sector fixed by itself before current read.\n");
		}
		so->stage = sync_stage_recov_write_cmds_done;
	}
out:
	return rv;
}

static inline void __validate_nwhole_params(struct recovery_sync_op *so)
{
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t dead = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, dead);        // Todo: Here use per slice info in metadata if there is no convictness!
	if (unlikely(so->nwhole_params.force_rebuild_bmp & dead)) {
		pet_trace_nwhole_param_err(so);
		WARN(1, "nvmeibc bug! no write hole was called with rebuild_bmp on dead segs.\n");
	}
	if (unlikely(so->nwhole_params.must_scrub && so->nwhole_params.dbits_turnon_bmp)) {
		pet_trace_nwhole_param_err(so);
		WARN(1, "nvmeibc bug! should never get here since there is no impl for scrub and dbits turnon.\n");
	}
	BUG_ON(so->nwhole_params.reserved);
	BUG_ON(so->nwhole_params.must_fix_bad_sectors && (!so->nwhole_params.must_turn_off_dbits)); //nvmeibc bug! no whole sync expects that must_fix_bad_sector==true => must_turn_off_dbits=true for the simplicity of implementation.
}

// In order to make sure that the restore function will do it work properly, needs to verify the sm starting vals.
static inline void __validate_write_cmds_do_not_send_vals(struct recovery_sync_op *so) {
	int bit, i;
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t dead = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, dead);

	so->n_cmds =   n_write_cmds(so);
	so->last_cmd = last_cmd(so) - 1;	// Last write cmd

	for (i = (so->last_cmd + 1 - so->n_cmds), bit = 1; i <= so->last_cmd; i++, bit <<= 1) {
		 struct nvmeibc_block_command *c = &so->cmds[i];
		 if (dead & bit) {
			BUG_ON(!c->do_not_send);
		 } else {
			BUG_ON(c->do_not_send);
		 }
	}
}

static inline void __dump_nwhole_params(const struct recovery_sync_op *so) {
	const union no_writehole_params *params = &so->nwhole_params;
	_NTSO(trace_01_no_whole_sm, "ram_pre_dbits=[@DBITS], db_turnon_bmp=@BITMAP, force_rebuild_bmp=@BITMAP, destroy=@BOOL_YN, fix_badsect=@BOOL_YN, must_turn_off=@BOOL_YN, must_scrub=@BOOL_YN",
		  so->cmds->rld.pre.bits.dirty, params->dbits_turnon_bmp, params->force_rebuild_bmp, params->destroy_full_slice, params->must_fix_bad_sectors, params->must_turn_off_dbits, params->must_scrub);
}

static void __update_no_whole_counters(const struct recovery_sync_op *so)
{
	struct nvmeibc_nowhole_stats *nowh = &get_so_fctr(so)->nowh;
	if (!so->error) {  // No writehole succeeded.
		if (so->nwhole_exec_plan.encountered_bad_sectors) {	// Regardless of so->o->op
			atomic_inc(&nowh->n_bdsec_fix);
			__log_corruption(so, "found and fixed bad sector"); // Otherwise, already printed this
		}
		if (       is_op_sync_db(so->o->op)) {
			atomic_inc(&nowh->n_dbits_fix);
		} else if (is_op_sync_rollback(so->o->op)) {
			atomic_inc(&nowh->n_rlbck_fix);
		} else if (is_op_sync_scrubbing(so->o->op)) {
			if (so->write_fix_mask) {// Scrub had to fix a blockset
				atomic_inc(&nowh->n_scrub_fix);
				__log_corruption(so, "scrubbing found and fixed a problem"); // Otherwise, already printed this
				// TODO: notify TOMA that we fixed a scrub issue
			} else if (!so->nwhole_exec_plan.encountered_bad_sectors) {
				atomic_inc(&nowh->n_other_fix);	// Scrub just read and everything was ok. Nothing fixed
			}
		} else {
			atomic_inc(&nowh->n_other_fix);
		}
		if (so->nwhole_exec_plan.is_blkset_corrupted) {
			atomic_inc(&nowh->n_di_fix);
		}
	} else if (so->write_unco_mask) {
		atomic_inc(&nowh->n_destoyed);			// Some data loss occured
		if (so->write_fix_mask)
			atomic_inc(&nowh->n_bdsec_fix);		// But at least some slices were fixed
	}
}

static void __cleanup_no_write_hole_sync_upon_finish(struct recovery_sync_op *so)
{	// Clean cause nwhole might be called again in the same sync.
	so->n_cmds =   n_write_cmds(so);
	so->last_cmd = last_cmd(so) - 1;	// Last write cmd
	nvmeibc_restore_write_cmds_do_not_send_vals(so);
	if (so->is_sbs_mode) {	// Only on Error could abort slice by slice and cause sync finish
		BUG_ON(!so->error);
		nvmeibc_sync_sl_by_sl_finish(so);
	}
	so->nwhole_params = no_writehole_params_empty;
	memset(&so->nwhole_exec_plan, 0, sizeof(so->nwhole_exec_plan)); // starting with empty execution plan
	so->write_unco_mask = so->write_fix_mask = 0;
}

/********************* NO WRITE HOLE SM **************************************/
/* This state machine starts when all locks are taken */
/* Pre coditions:
   1. RAM's TXID and dbits are resolved.
   2. must_fix_bad_sector==true => must_turn_off_dbits=true for the simplicity of implementation. */
void dp_sync_no_write_hole_cb_stg_end(struct recovery_sync_op *so)
{
_func_start:
	if (so->error != 0) {  /* On error terminate sync flow */
		_NTSO(trace_02_no_whole_sm, "stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
		so->stage = sync_stage_recov_write_cmds_done;	// On error we will terminate both SBS and full slice sync
	} else if (unlikely(so->o->topo->phased_out)) {
		so->error = -EAGAIN;
		goto _func_start;
	}
	_ND(trace_03_no_whole_sm, "-->op=@BLOCK_IO_OP, stage=@SYNC_STAGE err=@ERR sbs_index=@SBS_INDEX", so->o->op, so->stage, so->error, so->slice_by_slice_index);
	switch (so->stage) {
		case sync_stage_recov_lo_all_taken: {	// EC Entry point (mirror starts at read cmds sent)
			// Equivallent to All locks taken - SBS index is 32 (more than possible)
			__dump_nwhole_params(so);
			if (unlikely(so->cmds->rld.post.all != so->cmds->rld.pre.all)) {
				pet_trace_binfo_entry_mismatch(so);
				WARN_ON(1); // the sync assumes pre binfo is the most updated version, so making sure that no one setted post before.
			}
			if (unlikely(__is_raid1_mirror(so))) {
				pet_trace_binfo_entry_mismatch(so);
				WARN_ON(1); // mirror starts from stage: sync_stage_recov_no_write_hole_read_done, needed for dbits in metadata
			}
			if (unlikely(should_blockset_info_commit(so))) {
				pet_trace_binfo_commit_unexpected(so);
				WARN_ON(1);
			}
			__validate_nwhole_params(so);
			__validate_write_cmds_do_not_send_vals(so);
			if (unlikely(so->is_sbs_mode)) {
				pet_trace_nwhole_param_err(so);
				WARN_ON(1);
			}
			memset(&so->nwhole_exec_plan, 0, sizeof(so->nwhole_exec_plan)); // starting with empty execution plan

			if (so->nwhole_params.dbits_turnon_bmp) {
				const int slice_start = so_get_owner_seg(so);
				const sgmnts_bmp_t dbits_turnon_bmp_pr = rol32_width(so->nwhole_params.dbits_turnon_bmp, slice_start, so->r1->replicas);
				BUG_ON(!nvmeibc_raid_is_ec(so->r1));
				dp_sync_calc_new_binfo_dbits_turnon(so, dbits_turnon_bmp_pr);  /* Prepares lock DB values for dbits turnon due to rollback */
				if (should_blockset_info_commit(so)) // If the dbits are not set we need to commit binfo
					so->stage = sync_stage_recov_no_write_hole_turnon_ram_dbits;
				else 								// We can skip commit binfo and start no write hole sync
					so->stage = sync_stage_recov_no_write_hole_turnon_ram_dbits_done;
			} else if (so->nwhole_params.must_turn_off_dbits || so->nwhole_params.force_rebuild_bmp || so->nwhole_params.must_scrub) {
				so->stage = sync_stage_recov_no_write_hole_turnon_ram_dbits_done;
			} else {  // Sanity
				WARN(true, "nvmeibc bug! no_whole sync called and has nothing to do, op=0x%x, params=0x%llx\n", so->o->op, so->nwhole_params.raw);
				so->stage = sync_stage_recov_write_cmds_done;
			}
			goto _func_start;
		}

		case sync_stage_recov_no_write_hole_turnon_ram_dbits: {
			if (unlikely(!should_blockset_info_commit(so))) {
				pet_trace_binfo_commit_missing(so);
				WARN_ON(1);
			}
			so->stage = sync_stage_recov_no_write_hole_turnon_ram_dbits_done;
			nvmeibcbdpec_push_sm_to_stack(so, dp_sync_no_write_hole_cb_stg_end);
			ASYNC_AWAIT_AND_RESUME(dp_sync_write_all_blocksets_info_op(so));
		}

		case sync_stage_recov_no_write_hole_turnon_ram_dbits_done: {
			if (unlikely(should_blockset_info_commit(so))) {
				pet_trace_binfo_commit_unexpected(so);
				WARN_ON(1);
			}
			nvmeibcbdpec_inject_binfo_back_to_caller(so);
			ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_all_read_cmds(so, NULL, sync_stage_recov_no_write_hole_read_done));
		}

		case sync_stage_recov_no_write_hole_read_done: {	// If any read failed that we cannot fix, end sync with error
			const union dp_sync_reads_rv_bmp rv_bmp = dp_sync_reads_rv_bmp_init(so);
			WARN((so->o->op == NVMEIB_BLOCK_IO_OP_RECOVER_STALE) && nvmeibc_raid_is_ec(so->r1), "Stale lock recovery is a no write hole solution for mirror only, however this raid has %d\n", so->r1->slice_size);
			if (unlikely(rv_bmp.worst_software_error)) {  // If any read error (transport / detach / etc...), end sync with error
				so->error = rv_bmp.worst_software_error;
			} else {// If only readfailure errors exist try to fix
				const enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE rv = __analyze_no_write_hole_read(so, rv_bmp.readfail_bmp);
				switch(rv){
					case NO_WRITE_HOLE_DONE: { // EC-QLC: Can commit is writing to binfo here but a write will come soon anyway. Rider will have to solve this.
						so->stage = sync_stage_recov_write_cmds_done;
						break;
					}
					case START_SLICE_BY_SLICE_MODE: {	// Start SBS mode - when not enough sources exist
						nvmeibc_sync_sl_by_sl_start(so);
						so->stage = sync_stage_recov_no_write_hole_sbs_loop_end;
						break;
					}
					case NEXT_STAGE_SENT: // Async calculations are running (only for EC - reed solomon stage), just return
						BLKCMP_SO_ASYNC_RESUME_CUR();
					case GOTO_NEXT_STAGE: //Ready for next stage (All Done (no required fix)/Uncorrectable Ready/Write Ready (source were valid)/Switch to SBS) - Next stage is set
						break;
				}
			}
			goto _func_start;
		}

		case sync_stage_recov_no_write_hole_turnon_ram_dbits_before_turnoff: {
			if (unlikely(!should_blockset_info_commit(so))) {
				pet_trace_binfo_commit_missing(so);
				WARN_ON(1);
			}
			so->stage = sync_stage_recov_no_write_hole_restore_complete;
			so->cmds->rld.post.bits.dirty = so->nwhole_exec_plan.ram_dbits_after_first_turnon.all_bits;
			nvmeibcbdpec_push_sm_to_stack(so, dp_sync_no_write_hole_cb_stg_end);
			ASYNC_AWAIT_AND_RESUME(dp_sync_write_all_blocksets_info_op(so));
		}

		case sync_stage_recov_no_write_hole_restore_complete: {
			if (so->o->nd->dp.nwhole_funcs.get_restore_rv) { // EC only: verify restore successful
				int restore_rv = so->o->nd->dp.nwhole_funcs.get_restore_rv(so);
				if (restore_rv) {
					_NTSO(trace_04_no_whole_sm, "Restore failure: slice=@SBS_INDEX, rv=@RV\n", so->slice_by_slice_index, restore_rv);
					BUG(); // There should never be restore_rv cause it never produce errors. And errors before it have already been handleds.
				}
			}

			/* Just before turning dbits off: it is possible we have another step here - to commit ram dirty bits first */
			if (so->nwhole_exec_plan.should_write_ram_dbits_first) { // Commit binfo before sync
				if (unlikely(!should_blockset_info_commit(so))) {
					pet_trace_binfo_commit_missing(so);
					WARN_ON(1);
				}
				so->stage = sync_stage_recov_no_write_hole_turnon_ram_dbits_before_turnoff;
				so->nwhole_exec_plan.should_write_ram_dbits_first = false; /* Make sure we get here only once */
				goto _func_start;
			}

			/*
			 * Do not inject binfo to the caller here. In SBS mode the caller is
			 * the sync itself, so publishing cleaned post binfo as pre would let
			 * later slices trust a stale W copy. Inject once all writes complete.
			 */

			ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_all_write_cmds(so, so->nwhole_exec_plan.first_write_bmp, sync_stage_recov_no_write_hole_sent_restore_data_and_turnon_parity_md_dbits_done));
		}

		case sync_stage_recov_no_write_hole_sent_restore_data_and_turnon_parity_md_dbits_done: {
			const int write_rv = nvmeibcbdpec_get_rv_cur_stage_cmds(so); // Get write errors
			if (unlikely(write_rv)) {
				BUG_ON(so->error);
				so->error = write_rv;
				dp_sync_notify_toma_on_write_failure(so);
				goto _func_start;
			}
			nvmeibc_erase_rv_and_comp_codes_of_cur_stage_cmds(so, true);
			if (so->nwhole_exec_plan.second_write_bmp) { // If Dbits have been fixed we need to update MD (parities)
				__parity_md_dbits_turnoff(so);
				ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_all_write_cmds(so, so->nwhole_exec_plan.second_write_bmp, sync_stage_recov_no_write_hole_turoff_parity_md_dbits_done));
			} else {
				so->stage = sync_stage_recov_no_write_hole_sbs_loop_end;
				goto _func_start;
			}
		}

		case sync_stage_recov_no_write_hole_turoff_parity_md_dbits_done: {
			const int write_rv =  nvmeibcbdpec_get_rv_cur_stage_cmds(so); // Get write errors
			if (unlikely(should_blockset_info_commit(so))) {
				pet_trace_binfo_commit_unexpected(so);
				WARN_ON(1);
			}
			if (unlikely(write_rv)) {
				BUG_ON(so->error);
				so->error = write_rv;
				dp_sync_notify_toma_on_write_failure(so);
				goto _func_start;
			}
			nvmeibc_erase_rv_and_comp_codes_of_cur_stage_cmds(so, true);
			so->stage = sync_stage_recov_no_write_hole_sbs_loop_end;
			goto _func_start;
		}

		case sync_stage_recov_no_write_hole_sbs_loop_end: {
			if (so->is_sbs_mode) {
				if (so->slice_by_slice_index != 0) { // Not Done SBS
					nvmeibc_sync_sl_by_sl_next(so, sync_stage_recov_no_write_hole_read_done);	// prepare next slice read stage
					ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_cur_stage_cmds(so));
				} else {
					nvmeibc_sync_sl_by_sl_finish(so);  // Last slice writes have completed
				}
			}
			so->stage = sync_stage_recov_write_cmds_done;
			goto _func_start;
		}

		case sync_stage_recov_write_cmds_done: {
			if (!so->error) {  // No writehole succeeded.
				if (so->nwhole_params.must_turn_off_dbits && nvmeibc_raid_is_ec(so->r1) && so->nwhole_exec_plan.should_turnoff_dbits) {
					so->cmds->rld.post.bits.dirty = so->nwhole_exec_plan.ram_dbits_after_turnoff.all_bits;
					mark_blockset_info_not_written(so);	// Used correctly
				}
				nvmeibcbdpec_inject_binfo_back_to_caller(so);
			}
			__update_no_whole_counters(so);
			__cleanup_no_write_hole_sync_upon_finish(so);
			return nvmeibcbdpec_return_to_caller_sm(so);
		}

	default:;
	} // switch (->stage)
	WARN(true, "nvmeibc bug! Illegal sync state. IO can stuck\n!");
}
