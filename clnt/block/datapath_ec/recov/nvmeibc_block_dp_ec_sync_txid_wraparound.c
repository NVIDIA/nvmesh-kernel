/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_ec_sync_txid_wraparound.h"
#include "kr_incs.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "block/nvmeibc_topology.h"
#include "nvmeibc_block_dp_ec_recovery_common.h"
#include "nvmeibc_block_dp_ec_recov_maintenance.h"
#include "nvmeibc_block_dp_ec_recov_stats.h"
#include "nvmeib_shared.h"
#include "nvmeib_msgs_shared.h"

void dp_ec_sync_txid_wraparound_execute_op(struct recovery_sync_op *so) {
	dp_ec_sync_txid_wraparound_cb_stg_end(so);
}

static void __set_cmds_new_md(struct recovery_sync_op *so, const u32 txid, const roles_bmp_t writable_bmp) {
	struct nvmeibc_block_command *rldr = so->cmds;
	const int io_cmds_ind = mssa_get_data_start_index(rldr->o->mssa);
	const struct nvmeibc_raid1 *pr = so->r1;
	struct nvmeibc_block_command *io_cmds = &rldr[io_cmds_ind];
	const ulong writable_roles = writable_bmp;
	u32 slice_i;
	raid_role_t cmd_i;
	for_each_set_bit(cmd_i, &writable_roles, pr->replicas) {
		const struct nvmeibc_block_command *cmd = io_cmds+cmd_i;
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
		union nvmeibc_block_dp_ec_data_block_md *md = cmd->iocmd->reqs1.md;
		for (slice_i = 0; slice_i < cmd->nlbas; slice_i++) {	// Slice iterator
			enum nvmeibc_data_written_state written_state = nbdpec_md_get_data_written_state(md);
			const int slice_start = so_get_owner_seg(so);
			//WTF? when we sent those commands we sent them to readable_sync roles
			//When we update TxId we should update on all non dead (writable) segments including W-
			//readable_sync does not include D & W- segments; thus asking a question about the metadata on such segments produces false warnings
			const roles_bmp_t readable_bmp = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, readable_sync);
			const bool cmd_i_was_read = readable_bmp & (1u << cmd_i);
			if (dp_sync_is_slice_neverwritten(so, slice_i, readable_bmp, 'W')) {
				WARN_ON(cmd_i_was_read && !nbdpec_md_was_data_never_written(md)); // Slice is neverwritten
				if (cmd->is_parity) { // No turnning on dbits on neverwritten slice (As pre condition that there is no whole)
					union nvmeibc_dbits_entry dbits = fill_nvmeibc_dbits_entry_from_md(md);
					nbdpec_md_mark_data_never_written_with_dbits(md, cmd->is_parity, dbits);
				} else {
					nbdpec_md_mark_data_never_written_no_dbits(md, false);
				}
			} else {
				if (cmd->is_parity) {
					const struct nvmeibc_raid_leader_cmd_ctx *rld = &so->cmds->rld;
					const union nvmeibc_dbits_entry binfo_dbits = { .all_bits = rld->post.bits.dirty};
					WARN_ON(cmd_i_was_read && nbdpec_md_was_data_never_written(md));  // Slice is not neverwritten -> all readable parities must be written.
					nvmeibc_block_dp_ec_md_fill_p_with_dbits(md, binfo_dbits);
					nbdpec_md_mark_no_journal(md, txid, cmd->is_parity);
				} else if (written_state == DATA_WRITTEN) {
					nbdpec_md_mark_no_journal(md, txid, cmd->is_parity);
				} else if (written_state == DATA_EXPLICITLY_MARKED_NEVERWRITTEN) {
					nbdpec_md_mark_data_never_written_no_dbits(md, false);
				} else if (written_state == DATA_EXPLICITLY_MARKED_INVALID) {
					WARN_ON(!(is_data_invalid_for_read(md)));
				}
			}
			md = (void*)((u8*)md + md_size);
		}
	}
}

static inline void __inject_txid_back_to_caller(struct recovery_sync_op *so, const u32 txid) {
	so->cmds->rld.post.bits.txid = txid;
	// mark_blockset_info_not_written(so); - no need to mark it because the caller write tx will commit it for us when sending it's new txid.
	nvmeibcbdpec_inject_binfo_back_to_caller(so);
}

static void __call_no_whole_sync_and_restart_sm(struct recovery_sync_op *so, enum nvmeib_block_io_op op) {
	so->stage = sync_stage_recov_lo_all_taken;
	so->nwhole_params = no_writehole_params_default;
	so->nwhole_params.must_fix_bad_sectors = true;  // set nwhole params, because we want it to fix dbits and if there is a bad sector turn it off too.
	nvmeibcbdpec_push_sm_to_stack(so, dp_ec_sync_txid_wraparound_cb_stg_end);
	so->o->op = op;
	return dp_ec_sync_no_write_hole_execute_op(so);
}

/********************* TXID WRAP AROUND SM **************************************/
/* This state machine starts when all locks are taken */
/* Pre coditions (taken from design spec):
1. All needed locks are taken and no stale-locks.
2. Know the current binfo (blockset-info == TxID/Dbits) and none of them is unknown.
3. ram_txid == max_possible_txid.
4. There is no write-hole on the blockset.
5. For each slice if all readable block's md is never-written all other blocks are never-written (It is necessary because when cold recovery sees that all readable mds in blockset are neverwritten it would guess that all the degraded segs are also never-written and there won?t be a call to txid-wraparound). If this assumption is not satisfied it can lead to a corruption but with very very low chances. We can satisfy this requirement by a bit of code change in no-whole sync and HTR (If we regen a block and one it?s mds are neverwritten set it to neverwritten).
6. The current max_txid in blset is written in at least n_parity+1 blocks in at least one slice (non readable segs are considered as max_txid is written on them as when nwhole sync will pass them it will assign max_txid to them).
7. (Optional): No dbits on write segs (calling nwhole sync before starting wraparound). In this case no need to turn on dbits on W segs only on D.
*/
void dp_ec_sync_txid_wraparound_cb_stg_end(struct recovery_sync_op *so)
{
_func_start:
	if (so->error != 0) {  /* On error terminate sync flow */
		_NTSO(trace_02_txid_wrap_sm, "stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
		so->stage = sync_stage_recov_write_cmds_done;	// On error we will terminate sync.
	}
	_ND(trace_03_txid_wrap_sm, "-->op=@BLOCK_IO_OP, stage=@SYNC_STAGE err=@ERR", so->o->op, so->stage, so->error);
	switch (so->stage) {
		case sync_stage_recov_lo_all_taken: {
			const int slice_start = so_get_owner_seg(so);
			const roles_bmp_t read_bmp = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, readable_sync);
			WARN_ON(so->cmds->rld.post.all != so->cmds->rld.pre.all); // the sync assumes pre binfo is the most updated version, so making sure that no one setted post before.
			WARN_ON(!nvmeibc_raid_is_ec(so->r1)); // No wraparound on mirror
			WARN(so->cmds->rld.pre.bits.txid != NVMEIBC_DP_EC_MD_TX_ID_MAX,
				 "nvmeibc bug! txid_wraparound sync is callled but txid in binfo isn't NVMEIBC_DP_EC_MD_TX_ID_MAX, txid=%u\n",
				 so->cmds->rld.pre.bits.txid);
			WARN(!so->orig_rldr, "Impossible, TXID Wraparound sm counts on the caller write IO to commit the binfo.");
			ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_all_read_cmds(so, &read_bmp, sync_stage_recov_txid_wrap_read_done));
		}

		case sync_stage_recov_txid_wrap_read_done: {
			const int non_read_failure_error = dp_sync_get_any_non_readfail_errors(so);
			if (unlikely(non_read_failure_error)) {  // If any read failed that we cannot fix, end sync with error
				so->error = non_read_failure_error;
				goto _func_start;
			} else {
				roles_bmp_t readfail_bmp = dp_sync_gen_read_fail_bit_mask(so);
				nvmeibc_restore_read_cmds_do_not_send_vals(so);
				nvmeibc_erase_rv_and_comp_codes_of_cur_stage_cmds(so, false);
				if (unlikely(readfail_bmp)) {  // call nwhole sync and start from the beginning.
					_NTSO(trace_04_txid_wrap_sm, "Calling nwhole sync inorder to fix bad sectors.\n");
					ASYNC_AWAIT_AND_RESUME(__call_no_whole_sync_and_restart_sm(so, NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL));
				} else {
					const u32 max_txid = nvmeibcbdpec_calc_max_txid_in_data_md(so);
					if (unlikely(max_txid < NVMEIBC_DP_EC_MD_TX_ID_MAX)) {
						_NTSO(trace_05_txid_wrap_sm, "max_txid in blockset's mds: @TXID which is smaller than max_possible so setting it to binfo and finishing.\n", max_txid);
						__inject_txid_back_to_caller(so, max_txid); // Need to set binfo's txid to the max in blockset because we can't start wraparound until max_txid in blockset equals max_possible.
						ASYNC_AWAIT_AND_RESUME(nvmeibcbdpec_return_to_caller_sm(so));
					} else { // Start doing wraparound

						// Turnoff dbits if possible for assumption: 7 to be valid.
						if (unlikely(dp_ec_can_fix_dbits(so->cmds))) {
							_NTSO(trace_06_txid_wrap_sm, "Calling nwhole sync inorder to turnoff dbits, dbits=@DBITS\n", so->cmds->rld.pre.bits.dirty);
							ASYNC_AWAIT_AND_RESUME(__call_no_whole_sync_and_restart_sm(so, NVMEIB_BLOCK_IO_OP_RECOVER_DB));
						}
						so->stage = sync_stage_recov_txid_wrap_turnon_ram_dbits;
						goto _func_start;
					}
				}
			}
		}

		case sync_stage_recov_txid_wrap_turnon_ram_dbits: {
			const sgmnts_bmp_t dbits_turnon_bmp = nvmeibc_raid1_get_sgmnts_bmp(so->r1, dead);  // Only the dead segs are non readable cause the pre_cond is that there are no dbits on W segs.
			dp_sync_calc_new_binfo_dbits_turnon(so, dbits_turnon_bmp);
			so->stage = sync_stage_recov_txid_wrap_turnon_ram_dbits_done;
			if (should_blockset_info_commit(so)) {
				nvmeibcbdpec_push_sm_to_stack(so, dp_ec_sync_txid_wraparound_cb_stg_end);
				ASYNC_AWAIT_AND_RESUME(dp_sync_write_all_blocksets_info_op(so));
			} else {
				goto _func_start;
			}
		}

		case sync_stage_recov_txid_wrap_turnon_ram_dbits_done: {
			const int slice_start = so_get_owner_seg(so);
			const roles_bmp_t writable_bmp = nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, dead);
			__set_cmds_new_md(so, NVMEIBC_DP_EC_MD_TX_ID_MAX, writable_bmp);
			ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_all_write_cmds(so, writable_bmp, sync_stage_recov_txid_wrap_sent_maxtxid_and_turnon_dbits_done));
		}

		case sync_stage_recov_txid_wrap_sent_maxtxid_and_turnon_dbits_done: {
			const int write_rv =  nvmeibcbdpec_get_rv_cur_stage_cmds(so); // Get write errors
			const int slice_start = so_get_owner_seg(so);
			const roles_bmp_t writable_bmp = nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, dead);
			if (unlikely(write_rv)) {
				BUG_ON(so->error);
				so->error = write_rv;
				dp_sync_notify_toma_on_write_failure(so);
				goto _func_start;
			}
			nvmeibcbdpec_inject_binfo_back_to_caller(so);
			__set_cmds_new_md(so, NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS, writable_bmp);
			ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_all_write_cmds(so, writable_bmp, sync_stage_recov_txid_wrap_sent_txid_no_journal_done));
		}

		case sync_stage_recov_txid_wrap_sent_txid_no_journal_done: {
			const int write_rv =  nvmeibcbdpec_get_rv_cur_stage_cmds(so); // Get write errors
			if (unlikely(write_rv)) {
				BUG_ON(so->error);
				so->error = write_rv;
				dp_sync_notify_toma_on_write_failure(so);
			} else {
				so->stage = sync_stage_recov_write_cmds_done;
			}
			goto _func_start;
		}

		case sync_stage_recov_write_cmds_done: {
			if (!so->error) {  // wraparound finished.
				atomic_inc(&get_so_fctr(so)->main.n_txid_wrap);
				// Need to set binfo's txid to NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS
				__inject_txid_back_to_caller(so, NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS);
				mark_blockset_info_not_written(so);
			}
			return nvmeibcbdpec_return_to_caller_sm(so);
		}
	default:;
	} // switch (->stage)
	WARN(true, "nvmeibc bug! Illegal sync state. IO can stuck\n!");
}
