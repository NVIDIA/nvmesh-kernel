/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_ec_recov_maintenance.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "nvmeibc_block_dp_ec_recovery_common.h"
#include "../nvmeibc_block_dp_ec.h"
#include "nvmeibc_pausable.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "nvmeibc_block_dp_ec_sync_txid_wraparound.h"
#include "nvmeibc_block_dp_ec_recov_stats.h"

bool dp_ec_mainten_has_txid_unreslvd(const struct nvmeibc_block_command *rldr)
{
	return (rldr->rld.pre.bits.txid == INITIAL_LAZY_READ_TXID);
}

/* We have at least 1 unknown dbits, and cannot resolve them from disk, assume
   worst case (by topo) */
static union nvmeibc_dbits_entry
	__calc_worst_case_dbits(const struct recovery_sync_op *so)
{
	struct nvmeibc_dbits_tx tx;
	union nvmeibc_dbits_entry pre = {.all_bits = so->cmds->rld.pre.bits.dirty};
	sgmnts_bmp_t turn_on_dbit_bmp, turn_on_conv_bmp;

	turn_on_dbit_bmp = nvmeibc_raid1_get_sgmnts_bmp(so->r1, dbits_off_mask) | nvmeibc_raid1_get_sgmnts_bmp(so->r1, dbits_on_mask);
	turn_on_conv_bmp = nvmeibc_raid1_get_sgmnts_bmp(so->r1, wm);

	nvmeibc_dbits_tx_init_by_bmp(&tx, nvmeibc_raid1_get_protect_lvl(so->r1), turn_on_dbit_bmp, 0 /* turn_off_dbit_bmp */, turn_on_conv_bmp);
	nvmeibc_dbits_tx_apply(&pre, &tx);

	return tx.post;
}

static void __append_convicts_of_topo(const struct recovery_sync_op *so, union nvmeibc_dbits_entry *pre)
{
	struct nvmeibc_dbits_tx tx;
	nvmeibc_dbits_tx_init_only_dconv(&tx, nvmeibc_raid1_get_protect_lvl(so->r1), nvmeibc_raid1_get_sgmnts_bmp(so->r1, wm));
	nvmeibc_dbits_tx_apply(pre, &tx);
	*pre = tx.post;
}

/*********************** DP maintain virtal functions *************************/
union nvmeibc_dbits_entry nvmeibcbdpec_calc_max_dbit_in_ram_md(const struct recovery_sync_op *so)
{
	struct nvmeibc_block_command *c = so->cmds;
	union nvmeibc_dbits_entry res = {.all_bits = so->cmds->rld.pre.bits.dirty};
	int i, first_p = so->r1->slice_size;
	const int slice_start = so_get_owner_seg(so);
	const int num_parities = nvmeibc_raid1_get_protect_lvl(so->r1);
	const roles_bmp_t non_readable = nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, readable);
	const roles_bmp_t pari_bmp =     nvmeibc_raid1_get_roles_bmp(        so->r1, slice_start, raid.pari);
	const roles_bmp_t non_readable_pari = pari_bmp & non_readable;
	const bool all_pari_degraded = (non_readable_pari == pari_bmp);
	const bool double_deg_and_deg_parity = (hweight32(non_readable) > 1) && (non_readable_pari);

	WARN((all_pari_degraded && (num_parities > 1)) && (!double_deg_and_deg_parity), "nvmeibc bug, so=%p incorrect topo!, nrp=0%x, ss=%d\n", so, non_readable, slice_start); // sanity
	if (unlikely(double_deg_and_deg_parity || all_pari_degraded)) { // Exact condition for when on-disk-dbits cannot be used (inaccessible or cannot be trusted). Example: no-whole in the middle of turning-off dbits on D0 in slice -> cold recovery + Q becomes dead, D0 still W and no-whole only turned off the dbits in P (Q has dbits for D0) -> cold recovery needs to turn-on dbits on Q since it's dbits/txid might be invalid.
		res = __calc_worst_case_dbits(so); /* nowhere to read dbits from */
		goto _resolved;
	}
	// Find the first valid parity metadata with with dirty bits
	for (; (first_p < so->r1->replicas)&&(c[first_p].do_not_send); first_p++);
	if (first_p >= so->r1->replicas) {
		WARN(true, "nvmeibc bug, first_p=0x%x, so=%p must exist because readable_pari(0x%x) != all_pari(0x%x)!\n", first_p, so, non_readable_pari, pari_bmp); // sanity
		res = __calc_worst_case_dbits(so); /* nowhere to read dbits from */
		goto _resolved;
	}

	for (i = first_p, c = &so->cmds[i]; i < so->r1->replicas; i++, c++) {
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(c->ds);
		void *md = c->iocmd->reqs1.md, *md_end = md + (md_size*c->nlbas);
		BUG_ON(((!!c->do_not_send) != ((non_readable_pari >> i)&0x1)) || (c->nlbas != LOCKSET_SLICES));		// Just sanity verification.
		if (!c->do_not_send)
			for (; md < md_end; md += md_size) {
				const union nvmeibc_dbits_entry cur = fill_nvmeibc_dbits_entry_from_md(md);
				if (cur.all_bits) res.all_bits = nvmeibc_dbits_merge_owners(&res, &cur, num_parities);
			}
	}
	__append_convicts_of_topo(so, &res);
_resolved:
	nvmeibc_dbits_del_unk(&res, num_parities);

	if (unlikely(!verify_binfo_is_legal(so->locks->ds, (const union nvmeib_blkset_info){.bits.txid = so->cmds->rld.post.bits.txid, .bits.dirty = res.all_bits}, so->locks->address, 'r'))) { // merge failure, Todo: Check it, dump all metadatas. Probably data corruption
		for (i = first_p, c = &so->cmds[i]; i < so->r1->replicas; i++, c++) {
			const u32 md_size = nvmeibc_sgmnt_sw_md_size(c->ds);
			_NTSO(t_02_ecmint, "seg=@SEG, @DLBA, not_sent=@BOOL_YN, role=@ROLE_INT", c->ds->uuid, c->iocmd->reqs1.disk_address, c->do_not_send, i);
			nbdpec_md_to_string(c->iocmd->reqs1.md, md_size, c->nlbas, 'P');
		}
		WARN(true, "nvmeibc bug, dumping mds!\n");
	}
	return res;
}

union nvmeibc_dbits_entry nvmeibcbdpec_calc_worst_case_dbits(const struct recovery_sync_op *so)
{
	union nvmeibc_dbits_entry res = __calc_worst_case_dbits(so);
	const int num_parities = nvmeibc_raid1_get_protect_lvl(so->r1);

	nvmeibc_dbits_del_unk(&res, num_parities);

	return res;
}

void nvmeibcbdpec_inject_binfo_back_to_caller(struct recovery_sync_op *so)
{
	const union nvmeib_blkset_info *fix = &so->cmds->rld.post;
	struct nvmeibc_block_command *orig_ldr = (void*)so->orig_rldr;
	const int ow_i = nvmeibc_cllink_find_lock_by_cmd(orig_ldr);
	orig_ldr->rld.pre.all = fix->all;
	dp_locks_put_TxID_dbits(orig_ldr->cmdarr->locksets, ow_i, *fix, false);	// Commit binfo to IO locks struct
	so->cmds->rld.pre.all = fix->all;				// My 'post' is callers 'pre'
	dp_locks_put_TxID_dbits(so->locks,                     0, *fix, false);	// Commit binfo to SO locks struct
	// Note: txid may not be NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS. Even though we fixed entire blockset, we may not have overwrriten all blocks metadata with lowset txid, so cannot reset it in ram!
}

static void dp_ec_mainten_sl_by_sl_start(struct recovery_sync_op *so)
{
	int i;

	WARN(so->is_sbs_mode, "nvmeibc bug, wrong flow=%d, already in sbs mode\n", so->slice_by_slice_index);
	_NTSO(trace_dp_ec_mainten_sl_by_sl_start, "Starting slice by slice mode");
	so->slice_by_slice_index = so->n_slices;	// Start from last slice towards first slice, position beyond the last slice for _next() to be called immediately

	for (i = (so->last_cmd + 1 - so->n_cmds); i <= so->last_cmd; i++) {
		struct nvmeibc_block_command *cmd = &so->cmds[i];
		const u32 md_ssize = nvmeibc_sgmnt_sw_md_size(cmd->ds);
		cmd->iocmd->reqs1.disk_address += so->slice_by_slice_index;
		cmd->iocmd->reqs1.md += (so->slice_by_slice_index * md_ssize);
		// No need to fill cmd->first_rlba, since for MD read we won't need it for EDIC check (we can't, no data)
		BUG_ON(cmd->iocmd->reqs1.ndb->length != NVMEIBC_SECTOR2BYTE(cmd->nlbas));
		BUG_ON(cmd->nlbas != so->n_slices);
		cmd->nlbas = 1;
		cmd->iocmd->reqs1.ndb->length = NVMEIBC_SECTOR_SIZE;
	}
	so->is_sbs_mode = true;
}

static void dp_ec_mainten_sl_by_sl_next(struct recovery_sync_op *so)
{
	int i;

	BUG_ON(((u32)so->start_slice + (u32)so->slice_by_slice_index) > LOCKSET_SLICES);
	BUG_ON(so->slice_by_slice_index == 0);

	for (i = (so->last_cmd + 1 - so->n_cmds); i <= so->last_cmd; i++) {
		struct nvmeibc_block_command *cmd = &so->cmds[i];
		const u32 md_ssize = nvmeibc_sgmnt_sw_md_size(cmd->ds);
		cmd->iocmd->reqs1.disk_address--;
		cmd->iocmd->reqs1.md -= md_ssize;
		__cmd_clean_comp_val(cmd);
		DEBUG_TRANSFERS_init_cb_counter(cmd);
	}

	so->slice_by_slice_index--;
}

static void dp_ec_mainten_sl_by_sl_finish(struct recovery_sync_op *so)
{
	int i;

	_NTSO(t_01_mainten_sbs_fin, "Finished sbs: slice=@SBS_INDEX, err=@ERR", so->slice_by_slice_index, so->error);

	while (unlikely(so->slice_by_slice_index)) // on error, cleanup (complete the loop)
		dp_ec_mainten_sl_by_sl_next(so);

	for (i = (so->last_cmd + 1 - so->n_cmds); i <= so->last_cmd; i++) {
		struct nvmeibc_block_command *cmd = &so->cmds[i];
		cmd->nlbas = so->n_slices;
		cmd->iocmd->reqs1.ndb->length = NVMEIBC_SECTOR2BYTE(cmd->nlbas);
		__cmd_clean_comp_val(cmd);
	}

	so->is_sbs_mode = false;
}

void dp_ec_mainten_cb_stg_end(struct recovery_sync_op *so)
{
	union nvmeib_blkset_info *fix = &so->cmds->rld.post;				// Fixed binfo owritten to post
	struct nvmeibc_block_command *rldr = so->cmds;
_func_start:
	if (so->error != 0) {  /* On error terminate IO commands */
		_NTSO(t_01_ecmint, "stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
		so->stage = sync_stage_recov_write_cmds_done;
	}

	switch (so->stage) {
		case sync_stage_recov_read_cmds_sent: {
			const int non_read_failure_error = dp_sync_get_any_non_readfail_errors(so);
			const roles_bmp_t readfail_bmp = dp_sync_gen_read_fail_bit_mask(so);

			if (unlikely(so->is_sbs_mode)) {
				if (unlikely(non_read_failure_error)) {
					dp_ec_mainten_sl_by_sl_finish(so);
					so->error = -10045;
					goto _func_start;
				}

				if (readfail_bmp) {
					// Mark (fake) never-written in MD, so that it will not affect the choice of max. TxID after SBS end
					int i, bit;
					for (i = (so->last_cmd + 1 - so->n_cmds), bit = 1; i <= so->last_cmd; i++, bit <<= 1) {
						if (bit & readfail_bmp) {
							struct nvmeibc_block_command *c = &so->cmds[i];
							nbdpec_md_mark_data_never_written_no_dbits(c->iocmd->reqs1.md, c->is_parity);
						}
					}

					so->write_unco_mask |= (1U << (so->start_slice + so->slice_by_slice_index)); // Remember this readfail for the unknown dbits resolving step
				}

				so->stage = sync_stage_recov_mainten_sbs_loop_end;
				goto _func_start;
			}

			if (unlikely(non_read_failure_error)) {
				so->error = -10045;
				goto _func_start;
			}

			if (unlikely(readfail_bmp)) {
				dp_ec_mainten_sl_by_sl_start(so);
				so->stage = sync_stage_recov_mainten_sbs_loop_end;
				goto _func_start;
			}

			so->stage = sync_stage_recov_mainten_resolve_binfo;
			goto _func_start;
		}

		case sync_stage_recov_mainten_sbs_loop_end: {
			BUG_ON(!so->is_sbs_mode);

			if (so->slice_by_slice_index == 0) { // Done SBS
				dp_ec_mainten_sl_by_sl_finish(so);
				so->stage = sync_stage_recov_mainten_resolve_binfo;
				goto _func_start;
			}

			so->stage = sync_stage_recov_read_cmds_sent;
			dp_ec_mainten_sl_by_sl_next(so);
			nvmeibc_sync_set_uncompleted_cmds(so, so->n_cmds);
			ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_cur_stage_cmds(so));
			goto _func_start;
		}

		case sync_stage_recov_mainten_resolve_binfo: {

			if (unlikely(so->write_unco_mask)) {
				if (dp_ec_mainten_has_txid_unreslvd(rldr) && !nvmeibc_praid_are_all_readable(so->r1) && !dp_sync_has_unknown_dbits(rldr, nvmeibc_raid1_get_protect_lvl(so->r1))) {
					// This should never happen, as there is no flow that resolves dbits without TxID or that sets unknown dbits after TxID was already resolved
					WARN_ONCE(1, "nvmeibc bug! Cannot resolve dbits to worst case when resolving TxID in the presence of a readfail");
					_NTSO(trace_2_mainten_cb_stg, "Cannot resolve dbits to worst case when resolving TxID in the presence of a readfail, aborting");
					so->error = -10049;
					goto _func_start;
				}
			}

			so->stage = sync_stage_recov_write_binfo;

			if (dp_sync_has_unknown_dbits(rldr, nvmeibc_raid1_get_protect_lvl(so->r1))) {
				union nvmeibc_dbits_entry db;
				db = so->write_unco_mask ? nvmeibcbdpec_calc_worst_case_dbits(so) : nvmeibcbdpec_calc_max_dbit_in_ram_md(so);
				fix->bits.dirty = db.all_bits;
				atomic_inc(&get_so_fctr(so)->main.n_dbits_resolve);
				if (nvmeibc_dbits_get_n_unk(&db, nvmeibc_raid1_get_protect_lvl(so->r1))) {
					so->error = -10046;	// Daniel: I think this will stuck caller IO in a loop. Todo: Solve this!!!
				}
			}

			if (dp_ec_mainten_has_txid_unreslvd(rldr)) {
				fix->bits.txid = nvmeibcbdpec_calc_max_txid_in_data_md(so);
				atomic_inc(&get_so_fctr(so)->main.n_txid_resolve);
			}

			goto _func_start;
		}

		case sync_stage_recov_write_binfo: {
			const bool all_locks_exists = (NCL_do_i_have_lock(so->locks->status) || did_caller_of_so_took_this_lock(so->locks));
			so->stage = sync_stage_recov_write_cmds_done;
			if (all_locks_exists) {
				mark_blockset_info_not_written(so);
				/* Don't commit resolved binfo with RDMA, coz the caller will do that anyways.
				nvmeibcbdpec_push_sm_to_stack(so, dp_ec_mainten_cb_stg_end);
				ASYNC_AWAIT_AND_RESUME(dp_sync_write_all_blocksets_info_op(so)); */
			} else { /* Maintanance is not able to commit binfo anyways.*/ }
			goto _func_start;
		}

		case sync_stage_recov_write_cmds_done: {
			if (!so->error) {			// Simulate for caller as if problem never existed
				nvmeibcbdpec_inject_binfo_back_to_caller(so);
				if (unlikely(so->o->topo->phased_out)) {
					_NTSO(trace_1_mainten_cb_stg, "aborting, fail topo");		// Avoid prevention of topo free by a too-long nested 'so'
					so->error = -10047;
				}
			}
			DEBUG_TRANSFERS_init_cb_counters(so->n_cmds, &so->cmds[so->last_cmd + 1 - so->n_cmds]);		// Cleanup counters
			so->write_unco_mask = 0;	// Clean up this NoWH field which we might have used here
			return nvmeibcbdpec_return_to_caller_sm(so);
		}
	default:;
	} // switch (->stage)
	WARN(true, "nvmeibc bug! Illegal sync state. IO can stuck\n!");
}

static void __cleanup_func(struct recovery_sync_op *so, enum nvmeib_block_io_op iocmd_op)
{
	int i, n_max_cmds = n_write_cmds(so);	// == so->cmds->nraid_siblings
	for (i = 0; i < n_max_cmds; i++) {
		struct nvmeibc_block_command *c = &so->cmds[i];
		c->iocmd->reqs1.op = iocmd_op;
		DEBUG_TRANSFERS_init_cb_counter(c);
		__cmd_clean_comp_val(c);	 // Reset cmds comp codes
	}
}

static void __cleanup_func_read(struct recovery_sync_op *so) {
	__cleanup_func(so, NVMEIB_BLOCK_IO_OP_READ);
}

static void __cleanup_func_md_read(struct recovery_sync_op *so) {
	__cleanup_func(so, NVMEIB_BLOCK_IO_OP_MD_READ);
}

void dp_ec_mainten_reinit(struct recovery_sync_op *so, enum nvmeib_block_io_op new_op)
{
	int i = 0, n_max_cmds = n_write_cmds(so);		// Maybe we need to read only parities, or all data + parity, we dont know yet
	const enum nvmeib_block_io_op prev_cmd_op = so->cmds[so->r1->slice_size].iocmd->reqs1.op;
	BUG_ON(new_op != NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO);	// Just for debug, incorrect new_op
	so->o->op = new_op;
	if (prev_cmd_op == NVMEIB_BLOCK_IO_OP_READ) {
		nvmeibcbdpec_push_clean_to_stack(so, __cleanup_func_read);
	} else if (prev_cmd_op == NVMEIB_BLOCK_IO_OP_MD_READ) {
		nvmeibcbdpec_push_clean_to_stack(so, __cleanup_func_md_read);
	} else { 			// That is a plain bug. Should never happen!
		goto _bug_corrupted_sync_call_stack;
	}
	for (i = 0; i < n_max_cmds; i++) {
		int *cmd_op = &so->cmds[i].iocmd->reqs1.op;
		if (unlikely(*cmd_op != (int)prev_cmd_op))
			goto _bug_corrupted_sync_call_stack;
		*cmd_op = NVMEIB_BLOCK_IO_OP_MD_READ;	// Change READ to MD-READ
	}
	return;
_bug_corrupted_sync_call_stack:
	{
		char buf[32];
		recovery_sync_stack_to_string(&so->stack, buf);
		WARN(true, "SO={0x%2x,0x%2x}, Stack=%s, cmd[%d], cmd_op=%d\n", so->o->op, so->stage, buf, i, prev_cmd_op);
		BUG();
	}
}

static inline void __verify_maintanance_op_never_sends_data_over_the_network(const struct recovery_sync_op *so) {
	int i;
	for (i = (so->last_cmd + 1 - so->n_cmds); i <= so->last_cmd; i++) {
		 WARN_ON(so->cmds[i].iocmd->reqs1.op != NVMEIB_BLOCK_IO_OP_MD_READ);
	}
}

void dp_maintenance_execute_op(struct recovery_sync_op *so)
{
	const enum nvmeib_block_io_op op = so->o->op;
	if (op == NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON) {
		// This op finishes syncronously, no callbacks, no cmds. It is the caller responcibility to pedal futher
		struct nvmeibc_raid_leader_cmd_ctx* rld = &so->cmds->rld;
		union nvmeibc_dbits_entry rv = {.all_bits = rld->pre.bits.dirty};
		nvmeibc_dbits_turn_on_convict(&rv, so->r1);
		rld->post.bits.dirty = rv.all_bits;
		atomic_inc(&get_so_fctr(so)->main.n_dconvict_turnon);
		mark_blockset_info_not_written(so);	// Must always commit blockset info
		nvmeibcbdpec_push_sm_to_stack(so, nvmeibcbdpec_return_to_caller_sm);
		return dp_sync_write_all_blocksets_info_op(so);
	} else if (op == NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO) {
		struct nvmeibc_block_command *rldr = so->cmds;
		BUG_ON(!nvmeibc_raid_is_ec(so->r1));	// Raid1 has nothing to do with this sync
		so->n_cmds =   dp_ec_mainten_has_txid_unreslvd(rldr) ? n_write_cmds(so) : (u8)nvmeibc_raid1_count_bmp(so->r1, raid.pari);  // Dbits are not writen in data md blks, only in parity
		so->last_cmd = (n_read_cmds(so) ? n_read_cmds(so) : last_cmd(so)) -1;	// If 'so' is not nested, use its allocated commands, otherwise use pre-reads of caller 'so'
		so->stage =    sync_stage_recov_read_cmds_sent;
		nvmeibc_sync_set_uncompleted_cmds(so, so->n_cmds);
		__verify_maintanance_op_never_sends_data_over_the_network(so);
		nvmeibc_sync_send_cur_stage_cmds(so);
		BLKCMP_SO_ASYNC_RESUME_SND(dp_ec_sync_resume_op(so));	// Todo: if want to support dbits in R1 md and resolve binfo, just call here appropriate callback
	} else {
		char buf[32];
		recovery_sync_stack_to_string(&so->stack, buf);
		WARN(true, "Wrong flow! SO={%2x,%2x}, Stack=%s\n", so->o->op, so->stage, buf);
		BUG();
	}
}

/********* State machine which sends blockset recovered to all owner locks ****/
static void __cleanup_after_blockset_recovered(struct recovery_sync_op *so)
{
	(void)so; // Todo: We already did cleanup when analyzing rv, so no need here
}

static void __all_blockset_recovered_completed_analyze_rv(struct recovery_sync_op *so)
{
	const int nlocks = so->locks->nlocks, n_segs = so->r1->replicas;
	const int n_read = n_read_cmds(so);
	int l, seg_offset = so_get_owner_seg(so);			// Alternatively so->cmds[0].ds->tom_reg->seg
	for (l = 0; l < nlocks; l++) {
		const struct nvmeibc_cmd_lock *lock = &so->locks[l];
		const int si = (lock->ds - so->r1->segments);
		struct nvmeibc_block_command *cmd = &so->cmds[(si-seg_offset+n_segs)%n_segs+n_read]; 	// Todo: Make a macro which finds command of lock
		struct nvmeibc_disk_gen_cmd *gen_cmd = cmd->gen_cmd;
		const int cmd_rv = (gen_cmd ? gen_cmd->comp_code : -ENOMEM);
		if (unlikely(cmd_rv)) {
			_NTSO(tr_2_blkset_rec_cb, "Couldnt send to serjio @SI, err=@RV", si, cmd_rv);
			so->error = -10050;		// Could not send to Serjio/Toma!
		} else if (gen_cmd->rsp.br.status != 0) {		// Serjio reported failure, but we dont care? Is this correct?
			_NTSO(tr_3_blkset_rec_cb, "Ignorring Serjio @SI, srj_err=@ERROR, so_err=@SO_ERR", si, gen_cmd->rsp.br.status, so->error);
		}
		dp_cmds_gencmd_del(cmd);
	}
	so->should_send_msg_blckst_recovrd = false;
}

/* Prepare & execute sub state machine which sends all blockset recovered */
static void __send_all_blockset_recovered(struct recovery_sync_op *so)
{
	const int nlocks = so->locks->nlocks, n_segs = so->r1->replicas;
	const int n_read = n_read_cmds(so);
	int l, err, seg_offset = so_get_owner_seg(so);			// Alternatively so->cmds[0].ds->tom_reg->seg
	nvmeibc_sync_set_uncompleted_cmds(so, nlocks);
	for (l = 0; l < nlocks; l++) {
		const struct nvmeibc_cmd_lock *lock = &so->locks[l];
		const u64 holder = get_contending_id(&lock->comp);	// Stale lock we are trying to solve
		const int si = (lock->ds - so->r1->segments);
		struct nvmeibc_block_command *cmd = &so->cmds[(si-seg_offset+n_segs)%n_segs+n_read];	// Todo: Make a macro which finds command of lock
		err = dp_cmds_gencmd_add(cmd);
		if (!err) {
			nvmeibcbdpec_fill_blockset_recovered_info(cmd, HZ, so, holder, so,
				NVMEIB_EC_INVALID_JOURNAL_RANGE, NVMEIB_EC_INVALID_JOURNAL_ENTRY, true);
			err = nvmeibc_pd_execute_gen(cmd->ds->disk, cmd->gen_cmd);
		}
		if (err) {
			if (cmd->gen_cmd)
				cmd->gen_cmd->comp_code = err; // Simulate completion
			dp_ec_sync_cmd_cb(cmd);
		}
	}
	so = NULL; /* BEWARE! sync operation already complete and free() here */
}

static void send_all_blockset_recovered(struct recovery_sync_op *so)
{
	BLKCMP_SO_ASYNC_AWAIT(__send_all_blockset_recovered(so));
}

void dp_ec_mainten_blkset_recov_cb_stg_end(struct recovery_sync_op *so)
{
_func_start:
	if (so->error != 0) {  /* On error terminate IO commands */
		_NTSO(tr_1_blkset_recov_stg, "stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
		so->stage = sync_stage_recov_write_cmds_done;
	}

	switch (so->stage) {
		case sync_stage_recov_send_recovered_thread: {
			WARN_ON_ONCE(in_interrupt());	// Going to use sleeping alloc of gen cmds
			so->stage = sync_stage_recov_write_cmds_done;
			nvmeibcbdpec_push_clean_to_stack(so, __cleanup_after_blockset_recovered);
			so->should_abandon_on_error = true;
			ASYNC_AWAIT_AND_RESUME(send_all_blockset_recovered(so));
		}

		case sync_stage_recov_write_cmds_done: {
			__all_blockset_recovered_completed_analyze_rv(so);
			return nvmeibcbdpec_return_to_caller_sm(so);
		}
	default:;
	} // switch (->stage)
	WARN(true, "nvmeibc bug! Illegal sync state. IO can stuck\n!");
	if (0) goto _func_start;	// Never executed. Just to avoid unused label warning
}

