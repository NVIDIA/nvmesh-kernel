/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_icore_ops.h"
#include "nvmeibc_block_dp_ec_recovery_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "nvmeibc_block_dp_ec_recov_cold.h"
#include "nvmeibc_block_dp_ec_recov_hot.h"
#include "../../controlpath/nvmeibc_b_cp_blkset_topo.h"
#include "../nvmeibc_block_dp_ec.h"

extern void cldr_kfree(void *ptr);

void nvmibc_blockset_candidates_kfree_deleted(struct nvmibc_blockset_candidates *jcl)
{
	struct nvmibc_tx_candidate *cur, *tmp;
	list_for_each_entry_safe(cur, tmp, &jcl->can_list, next) {
		if (nvmibc_is_tx_candidate_invalid(cur)) {
			list_del(&cur->next);
			cldr_kfree(cur);
		}
	}
}

/********************** DP sync cold virtal functions ************************/
int  dp_ec_sync_cold_prepare_op(struct recovery_sync_op *so);			// Todo: Implement! Dont trust preparation for no_writehole()

#define CAND_PRINT_FMT     "cand{j2slba=@J2D, txid=@TXID, txbm=@TXBM, len=@HEX_1B}"
#define CAND_PRINT_ARG(c)  (c)->b.j2slba, (c)->b.tx_id, (c)->b.tx_bmp[0], (c)->b.len
static void __dump_cold_state(const struct recovery_sync_op *so)
{
	int i;
	const u64 slba =  so->rlba / so->r1->slice_size;
	const struct nvmeibc_roles_bmps topo_bm = nvmeibc_roles_bmps_get_by_slba(so->r1, slba);
	struct nvmibc_blockset_candidates *jcl = so->o->user_data;
	struct nvmibc_tx_candidate *cur;
	const u32 max_data_txid = so->cmds->rld.post.bits.txid;
	const u32 ram_dbits = so->cmds->rld.post.bits.dirty;
	_NTSO(t_01_rcdcs, "Cold Blockset State Dump: max_txid=@TXID, dbits=@DBITS, topos: rw=@X, w=@X, d=@X",  max_data_txid, ram_dbits, topo_bm.rw, topo_bm.w, topo_bm.dead);

	list_for_each_entry(cur , &jcl->can_list, next)
		_NT(trace_1_dp_ec_recov_cold_dump_cold_state, CAND_PRINT_FMT " cuuid=@CLIENT_UUID",  CAND_PRINT_ARG(cur), &cur->cuuid);

	for (i = (so->last_cmd + 1 - so->n_cmds); i <= so->last_cmd; i++) {
		const struct nvmeibc_block_command *c = &so->cmds[i];
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(c->ds);
		if ((c->do_not_send) /*||(c->iocmd.comp.comp_code != 0)*/)		// Todo: Handle errors of reads
			continue;
		nbdpec_md_to_string(c->iocmd->reqs1.md, md_size, c->nlbas, 'P');
	}
}

static void __data_slice_d2j_info_init(const struct recovery_sync_op *so, struct data_slice_d2j_info *d2j_info)
{
	const union nvmeibc_block_dp_ec_data_block_md *md;
	int i, b, max_txid_slice = 0;
	u32 max_txid_jri = JRI_MARK_NO_JOURNAL, mask, max_txid = 0;
	bool is_all_mds_never_written = true;

	for (i = (so->last_cmd + 1 - so->n_cmds), mask = 1; i <= so->last_cmd; i++, mask <<= 1) {
		const struct nvmeibc_block_command *c = &so->cmds[i];
		const int n_slices = (int)c->nlbas;
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(c->ds);
		if ((c->do_not_send) /*||(c->iocmd.comp.comp_code != 0)*/)		// Todo: Handle errors of reads
			continue;
		for (b = 0; b < n_slices; b++) {
			u32 max_txid_jri_cur_seg = JRI_MARK_NO_JOURNAL;
			md = (c->iocmd->reqs1.md + (b*md_size));
			if (nbdpec_md_was_data_never_written(md)) {
				continue;
			} else {
				is_all_mds_never_written = false;
			}
			BUG_ON(md->D.version != NVMEIBC_DATA_MD_VERSION); // Trap for reading only correct version
			if (md->tx_id > max_txid) {             // Important: If tx_id is 0 then this data does not have a journal and the if () will be false
				max_txid = md->tx_id;
				max_txid_jri_cur_seg = max_txid_jri = md->jri;
				memset(d2j_info->txbm, 0, sizeof(d2j_info->txbm));
				d2j_info->txbm[b] = mask;
			} else if ((!nvmeib_txid_no_journal(max_txid)) && (md->tx_id == max_txid)) {
				if (unlikely((max_txid_jri_cur_seg != JRI_MARK_NO_JOURNAL) && (md->jri != JRI_MARK_NO_JOURNAL) && (max_txid_jri_cur_seg != md->jri))) { // not all maxTxID blocks are on the same slice.
					WARN(true, "Invalid data state, same txid in different jris on the same seg: txid=0x%x, one dmd with jri=%u, slice=%u and second with jri=%u, slice=%u\n", max_txid, max_txid_jri_cur_seg, max_txid_slice, md->jri, b);
					if (is_op_sync_cold(so->o->op))
						__dump_cold_state(so);
					// Dont abort as it is likely that this TxID is not the maximal value in the blockset (during the rest of the scan we will find a higher txid) and bug will be avoided, even though this is a bug
				}
				d2j_info->txbm[b] |= mask;
			}
		}
	}

	d2j_info->is_txid_wraparound_or_write_called_it_failed = false;
	if (is_all_mds_never_written) {  // If all readable segs are neverwritten that means that also the non readable segs are neverwritten -> so max_txid in blockset is txid_no_journal.
		for (b = 0; b < LOCKSET_SLICES; b++) {
			d2j_info->txbm[b] = (roles_bmp_t)~0U; // All datas match in any slice.
		}
		max_txid = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS; // Make sure we start txid on disk from 1 and not 0
	} else if (unlikely(max_txid == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS)) {
		const int slice_start = so_get_owner_seg(so);
		const roles_bmp_t non_readable = nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, readable);
		d2j_info->is_txid_wraparound_or_write_called_it_failed = true;  // If wraparound and the write tx who called it succeeded max_txid in data should have been (NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS + 1).
		if (non_readable) { // means all readable mds are neverwritten or with txid == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS -> we are potentially failed in the middle of wraparound
			max_txid = NVMEIBC_DP_EC_MD_TX_ID_MAX; // we are potentially failed in the middle of wraparound lazily make new wraparound excute.
		} // else: Wraparound failed but finished wrapping the txid because all txid are NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS or neverwritte, max_txid = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS
	} else if (unlikely(max_txid == NVMEIBC_DP_EC_MD_TX_ID_UNSET)) {  // Sanity, should never happen!
		WARN(true, "nvmeibc bug, " PRI_SO_NAME ": (max_txid == 0) and not all mds are neverwritten\n", PRI_SO_NAME_ARGS(so));
	} else if (unlikely(max_txid > NVMEIBC_DP_EC_MD_TX_ID_MAX)) { // Sanity, should never happen!
		WARN(true, "nvmeibc bug, " PRI_SO_NAME ": txid > max id is never directly written\n", PRI_SO_NAME_ARGS(so));
		max_txid = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS; // Treat the above WARN(), should never happen!
	}

	d2j_info->max_txid = max_txid;
	d2j_info->jri = max_txid_jri;
	return;
}

u32 nvmeibcbdpec_calc_max_txid_in_data_md(const struct recovery_sync_op *so)
{
	struct data_slice_d2j_info d2j_info;
	__data_slice_d2j_info_init(so, &d2j_info);
	return d2j_info.max_txid;
}

static bool __does_dbits_need_reconstruction(const struct recovery_sync_op *so)
{
	//in case cold recovery restarts, after we made some progress, dirty bits may be resolved (not unknown)
	const union nvmeib_blkset_info *bi = &so->cmds->rld.post;
	union nvmeibc_dbits_entry db = {.all_bits = bi->bits.dirty};
	return (nvmeibc_dbits_get_n_unk(&db, &so->r1->calculated_data.topo_traits) != 0);
}

static void __reconstruct_blockset_info_from_md(const struct recovery_sync_op *so, struct data_slice_d2j_info *d2j_info)
{
	union nvmeib_blkset_info *fix = &so->cmds->rld.post;
	const union nvmeib_blkset_info *bi = &so->cmds->rld.post;
	__data_slice_d2j_info_init(so, d2j_info);

	_NTSO(trace_dp_ec_recov_cold_reconstruct_blockset_info_from_md_2, "Initial binfo values: {TxID=@TXID, dbits=@DBITS}", bi->bits.txid, bi->bits.dirty);

	if ((bi->bits.txid != NVMEIBC_DP_EC_MD_TX_ID_MAX + 1) && (bi->bits.txid != INITIAL_LAZY_READ_TXID)) {
		WARN(d2j_info->max_txid > bi->bits.txid, "nvmeibc bug, " PRI_SO_NAME ": max_txid in data is greater than ram {data_TxID=%d, ram_TxID=%d} \n", PRI_SO_NAME_ARGS(so), d2j_info->max_txid, bi->bits.txid);
	}

	fix->bits.txid = d2j_info->max_txid; // Resolve & Set txid for blockset metadata reconstruction

	if (__does_dbits_need_reconstruction(so)) {// We already read 'md' so resolve dirty suspect ont he way
		union nvmeibc_dbits_entry db = nvmeibcbdpec_calc_max_dbit_in_ram_md(so);
		fix->bits.dirty = db.all_bits;
	}
	_NTSO(trace_dp_ec_recov_cold_reconstruct_blockset_info_from_md, "Reconstructed binfo {TxID=@TXID, dbits=@DBITS}", fix->bits.txid, fix->bits.dirty);
	return;
}

static inline void __set_rollback_action(struct recovery_sync_op *so, roles_bmp_t rollback_dead_segs_txbm, roles_bmp_t rollback_w_segs_txbm)
{
	so->nwhole_params = no_writehole_params_default;
	so->nwhole_params.dbits_turnon_bmp = rollback_dead_segs_txbm;
	so->nwhole_params.force_rebuild_bmp = rollback_w_segs_txbm;
}

static inline bool __should_rollback(const struct recovery_sync_op *so) {
	/* In cold recovery dbtis turnon or force_rbuild bmp are setted iff there is neded for rollback. */
	return (so->nwhole_params.dbits_turnon_bmp || so->nwhole_params.force_rebuild_bmp);
}

static void __filter_candidates_by_txid_j2d_d2j(struct recovery_sync_op *so, const struct data_slice_d2j_info *d2j_info)
{
	struct nvmeibc_raid1 *pr = so->r1;
	struct nvmibc_blockset_candidates *jcl = so->o->user_data;
	struct nvmibc_tx_candidate *cur;
	u32 max_data_txid = so->cmds->rld.post.bits.txid;
	enum cand_state c_state;
	roles_bmp_t rollback_txbm = 0;
	u32 n_max_txid_cand = 0;
	bool rollback_possible = true;
	const u64 slba =  so->rlba / so->r1->slice_size;
	const struct nvmeibc_roles_bmps topo_bm = nvmeibc_roles_bmps_get_by_slba(so->r1, slba);
	unsigned si;

	// Note: the case when (max_data_txid == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS) is private case and handled (rollback_possible = true but no max_txid candidate).
	list_for_each_entry(cur , &jcl->can_list, next) {	// Find valid candidate with highest TxID and check possible rollback
		WARN(cur->b.tx_id > NVMEIBC_DP_EC_MD_TX_ID_MAX, "Found a candidate with txid > NVMEIBC_DP_EC_MD_TX_ID_MAX, txid=%u\n", cur->b.tx_id);
		if (unlikely(d2j_info->is_txid_wraparound_or_write_called_it_failed)) {
			// If the write after txid_wraparound potentially failed we need to roll-back all txs with txid greater than NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS which are all valid txid
			// binfo_txid == NVMEIBC_DP_EC_MD_TX_ID_MAX cause we would like to rerun wraparound again. In this situtation the TX after the wraparound might only written to non-readable segs so need to roll-back them.
			if (NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS < cur->b.tx_id) {
				for (si=0; si<NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY; si++) {
					const roles_bmp_t txbm_with_pari = cur->b.tx_bmp[si] | nvmeibc_raid1_get_parities_bmp(pr);
					rollback_txbm |= txbm_with_pari & nvmeibc_get_nonrw_roles(topo_bm);
				}
				cur->type = ROLL_BWD;
				rollback_possible = true;
			}
		} else if (cur->b.tx_id == max_data_txid) {
			if (d2j_info->jri == JRI_MARK_NO_JOURNAL) {
				_NTSO(__filter_cands_j2d_d2j_no_j,"max_txid with no_journal, " CAND_PRINT_FMT, CAND_PRINT_ARG(cur));	// Means we are in no_w_hole discarding max_txid candidate
				nvmibc_tx_candidate_invalidate(cur);
				continue;
			}

			c_state = nvmeibc_tx_does_d2j_j2d_match(so, cur, d2j_info);

			if (c_state != NOT_A_CANDIDATE)
				n_max_txid_cand += 1;

			if (c_state == PARTIALLY_COMMITED)
				rollback_possible = false;

			// Remove all candidates where d2j and j2d does not point back (j2d and d2j do not close the loop) or candidate is fully committed
			if (c_state == FULLY_COMMITED || c_state == NOT_A_CANDIDATE) {
				// Todo: Use does_dmd_match() of HTR to find that Data and Journal match
				_NTSO(__filter_cands_j2d_d2j_inv, "invalid cand, reason=@E_CAND_STATE, " CAND_PRINT_FMT, c_state, CAND_PRINT_ARG(cur));
				nvmibc_tx_candidate_invalidate(cur);
			} else {
				cur->type = ROLL_FWD;
			}
		} else if (max_data_txid < cur->b.tx_id) {
			for (si=0; si<NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY; si++) {
				const roles_bmp_t txbm_with_pari = cur->b.tx_bmp[si] | nvmeibc_raid1_get_parities_bmp(pr);
				rollback_txbm |= txbm_with_pari & nvmeibc_get_nonrw_roles(topo_bm);
			}
			cur->type = ROLL_BWD;
		} else { // cur->b.tx_id < max_data_txid means old Tx after last wraparound
			_NTSO(__filter_cands_j2d_d2j_old, "del old " CAND_PRINT_FMT ", max_txid=@TXID", CAND_PRINT_ARG(cur), max_data_txid);
			nvmibc_tx_candidate_invalidate(cur);
		}
	}

	if (unlikely(n_max_txid_cand > 1)) {
		WARN(true, "Found %d max_txid candidates - at most one possible\n", n_max_txid_cand);
		__dump_cold_state(so);
	}

	nvmibc_blockset_candidates_kfree_deleted(jcl);		// Remove all the candidates from the list except for 'best'

	WARN(so->nwhole_params.dbits_turnon_bmp, "Found turnon bmp in nwhole params\n"); //Sanity
	WARN(so->nwhole_params.force_rebuild_bmp, "Found force_rebuild_bmp in nwhole params\n"); //Sanity
	if (rollback_possible && rollback_txbm)
		__set_rollback_action(so, rollback_txbm & topo_bm.dead, rollback_txbm & topo_bm.w); // TODO: for now no_wrtehole regen all w segs so it sufficient to pass only rollback_txbm of dead segs

	return;
}

void dp_ec_sync_cold_cb_stg_end(struct recovery_sync_op *so)
{
_func_start:
	if (so->error) { 				// Fast fail, during error path
		_NTSO(error_dp_ec_recov_cold_dp_ec_sync_cold_cb_stg_end, "sync_stage=@SYNC_STAGE, error=@ERROR", so->stage, so->error);
		so->stage = sync_stage_recov_write_cmds_done;
	}
	_ND(error_1_dp_ec_recov_cold_dp_ec_sync_cold_cb_stg_end, "-->op=@BLOCK_IO_OP, stage=@SYNC_STAGE error=@ERROR", so->o->op, so->stage, so->error);

	switch (so->stage) {
	case sync_stage_recov_lo_all_taken: {		// Start state machine
		atomic_inc(&get_so_fctr(so)->jour.n_syncs);
		so->n_cmds =   n_write_cmds(so);				// Read entire metadata of the blockset (using pre-read commands)
		so->last_cmd = n_read_cmds(so) - 1;				// Last commands are always writes
		so->stage =    sync_stage_recov_read_cmds_sent;
		nvmeibc_sync_set_uncompleted_cmds(so, so->n_cmds);
		ASYNC_AWAIT_AND_RESUME(nvmeibc_sync_send_cur_stage_cmds(so));
	}

	case sync_stage_recov_read_cmds_sent: {
		struct data_slice_d2j_info d2j_info;
		const int rv = nvmeibcbdpec_get_rv_cur_stage_cmds(so);
		if (rv != 0) { /* Todo: handle errors (can still recover using parities)? */
			so->error = -10023;
			goto _func_start;
		}

		nvmeibc_restore_read_cmds_do_not_send_vals(so);
		nvmeibc_erase_rv_and_comp_codes_of_cur_stage_cmds(so, false);
		// Reconstruct blockset info in RAM (TxID, Dbits)
		__reconstruct_blockset_info_from_md(so, &d2j_info);
		// We just resolved the binfo, mark it not written
		mark_blockset_info_not_written(so);
		nvmeibcbdpec_inject_binfo_back_to_caller(so);			// Caller is always recovery
		__filter_candidates_by_txid_j2d_d2j(so, &d2j_info);
		if (nvmeibcbdpec_has_rollfwd_jour_candidate(so)) {
			struct nvmibc_blockset_candidates *jcl = so->o->user_data;
			struct nvmibc_tx_candidate *can;
			list_for_each_entry(can, &jcl->can_list, next)
				_NTSO(trace_dp_ec_recov_cold_dp_ec_sync_cold_cb_stg_end, "Found a valid " CAND_PRINT_FMT, CAND_PRINT_ARG(can));
			so->recoveree_cuuid = nvmeibcbdpec_get_rollfwd_jour_candidate(so)->cuuid;
			so->stage = sync_stage_recov_do_sync_stale;
		} else {
			atomic_inc(&get_so_fctr(so)->jour.n_no_htr_call);
			so->stage = __should_rollback(so) ? sync_stage_recov_commit_binfo_before_rollback : sync_stage_recov_write_cmds_done;
		}
		goto _func_start;
	}

	case sync_stage_recov_do_sync_stale: {								// Goto stale locks sync state machine to solve write hole and its consequences
		so->stage = __should_rollback(so) ? sync_stage_recov_commit_binfo_before_rollback : sync_stage_recov_write_cmds_done;
		atomic_inc(&get_so_fctr(so)->jour.n_htr_call);
		nvmeibcbdpec_push_sm_to_stack(so, dp_ec_sync_cold_cb_stg_end);
		so->o->op = NVMEIB_BLOCK_IO_OP_RECOVER_STALE;
		ASYNC_AWAIT_AND_RESUME(dp_ec_sync_stale_execute_op(so));
	}

	/* It's not a must to commit binfo before calling no_whole but it is for safety cause no_whole might assume in the future that the binfo commited already. */
	case sync_stage_recov_commit_binfo_before_rollback: {
		so->stage = sync_stage_recov_do_rollback;
		if (!should_blockset_info_commit(so)) {
			goto _func_start;
		}
		nvmeibcbdpec_push_sm_to_stack(so, dp_ec_sync_cold_cb_stg_end);
		ASYNC_AWAIT_AND_RESUME(dp_sync_write_all_blocksets_info_op(so));
	}

	case sync_stage_recov_do_rollback: {
		so->stage = sync_stage_recov_write_cmds_done;
		WARN(!__should_rollback(so), "Cold recovery - rollback stage with no rollback params, something went wrong\n");
		nvmeibcbdpec_push_sm_to_stack(so, dp_ec_sync_cold_cb_stg_end);
		so->o->op = NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK;
		so->stage = sync_stage_recov_lo_all_taken;
		ASYNC_AWAIT_AND_RESUME(dp_ec_sync_no_write_hole_execute_op(so));
	}

	case sync_stage_recov_write_cmds_done: {
		return nvmeibcbdpec_return_to_caller_sm(so);
	}
	default:;
	} // switch (->stage)
	WARN(true, "nvmeibc bug! Illegal sync state. IO can stuck\n!");
}

void dp_ec_sync_cold_execute_op(struct recovery_sync_op *so)
{
	dp_ec_sync_cold_cb_stg_end(so);
}

/********************** DP sync JGC virtal functions ************************/
void dp_ec_sync_j_gc_execute_op(struct recovery_sync_op *so)
{
	struct nvmibc_blockset_candidates *jcl = so->o->user_data;
	struct nvmibc_tx_candidate *cur;
	atomic_inc(&get_so_fctr(so)->jour.n_jgc_blksets);
	_NTSO(jgc_1_mark_garbage, "@SLBA_BLKSETS", (u64)jcl->blkset_lba);
	list_for_each_entry(cur , &jcl->can_list, next) {	// Actually it is enough to mark only the first element
		struct candidate_location *loc = &cur->locations[0];	// Enough to mark the first location
		_NTSO(jgc_mark_garbage, "jgc: mark " CAND_PRINT_FMT "{jri=@JRI, jent=@JENT}", CAND_PRINT_ARG(cur), loc->jri, loc->jentry);
		loc->is_garbage = true;
	}
	return nvmeibcbdpec_return_to_caller_sm(so);
}
