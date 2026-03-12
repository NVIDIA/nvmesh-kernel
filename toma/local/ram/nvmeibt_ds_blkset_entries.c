/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_ds_blkset_entries.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_praid.h"
#include "nvmeibt_local_disk.h"

void nvmeibt_ds_blkset_entries_sanitize_packed(
	BOOL is_EC, char *disk_blk_buf, uint64_t n_blksets, uint64_t *n_stale_locks, uint64_t *n_dirty_bits)
{
	const union nvmeib_lock_id		lockid_is_read = { .bits = { .is_read = 1, } };

	union nvmeib_lock_blkset_entry	*tbl = (union nvmeib_lock_blkset_entry *)disk_blk_buf;
	uint64_t						n_stale = 0;
	uint64_t						n_dirty = 0;
	uint64_t						ii;

	for (ii = 0; ii < n_blksets; ii++) {
		/* lock_id.bits.reserved should _always_ be zero */
		NTOMA_ASSERT(trace_70_toma_dsmd, (tbl[ii].lock_id.bits.reserved == 0),
			"lock_id=@T_LID.reserved!=0 (blkset=@ZU)", tbl[ii].lock_id.all, ii);

		/* ignore lock_id.bits.id_read bit when considering the locks */
		if (tbl[ii].lock_id.all & ~lockid_is_read.all) {
			if (!is_EC) { /* RAID1: convert all locks to generic stale_special */
				tbl[ii].lock_id = nvmeib_stale_special_raid1.lock_id;
			}
			n_stale++;
		}
		if (tbl[ii].blkset_info.bits.dirty != 0) {
			n_dirty++;
		}
	}

	N_Tf(trace_71_toma_dsmd, "total lock ents in memory: is_stale=@ZU dirty=@N_DBITS", n_stale, n_dirty);
	*n_stale_locks = n_stale;
	*n_dirty_bits = n_dirty;
}

/* Prepare dirty in dirty_plus_txid_init_val in case needed for init
 * Side effect on @disk_segment: if init mode is INIT_IRRELEVANT will convert it to INIT_DONE.
 */
static int ds_metadata_init_EC_prepare_dirty_and_txid_bits_init_val(struct nvmeibt_seg_active *seg_active, union nvmeib_blkset_info *dirty_plus_txid_init_val)
{
	const enum NVMEIBT_MEM_TBL_INIT_MODE mode = seg_active->active_seg_topo.dirty_bits_init_mode;
	int rv = 0;

	switch (mode) {
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER:
		dirty_plus_txid_init_val->bits.dirty = 0;
		dirty_plus_txid_init_val->bits.txid = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF:
		dirty_plus_txid_init_val->bits.dirty = 0;
		dirty_plus_txid_init_val->bits.txid = INITIAL_LAZY_READ_TXID;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON:
		TODO(make optimization: 2 unknowns is an overkill, put convicts and allow single degraded. Not madatory because client will resolve the unknowns anyways)
		dirty_plus_txid_init_val->bits.dirty = nvmeib_dbits_entry_build_unk(-1, -1).all_bits;
		dirty_plus_txid_init_val->bits.txid = INITIAL_LAZY_READ_TXID;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN:
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED:
	default:
		N_Ef(dgy76gr, "EC: illegal init_mode=@INIT_MODE_STR", mem_tbl_init_mode_str(mode));
		rv = -1;
	}

	return rv;
}

/*
 * Prepare lock_id in lock_id_init_val in case needed for init
 * Side effect on @disk_segment:
 * if init mode is INIT_IRRELEVANT will convert it to INIT_DONE.
 */
static int ds_metadata_init_EC_prepare_stale_locks_init_val(struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id *lock_id_init_val, bool *is_stale_rebuild_required)
{
	const enum NVMEIBT_MEM_TBL_INIT_MODE mode =	seg_active->active_seg_topo.stale_locks_init_mode;
	int rv = 0;

	// - Handle the cases that only modify the init_mode (no TBL writes)
	// - Prepare lock_id_init_val (lockid+is_stale) in case we do need to init

	*is_stale_rebuild_required = 0;
	switch (mode) {
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER:
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF:
		(*lock_id_init_val).all = 0;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST:
		// For now: Stale locks always handled by cold-recovery from the journal.
		// So (in EC) this init state is illegal for stale locks!
		N_Ef(dgfhryq, "seg=@UUID_8 stale=@INIT_MODE_STR illegal", nvmeibt_seg_active_UUID_8(seg_active), mem_tbl_init_mode_str(mode));
		nvmeibt_abort(ES_FATAL);
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON:
		*is_stale_rebuild_required = 1;
		FALLTHRU;
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED:
	case NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN:
	default:
		N_Ef(cni874c, "seg=@UUID_8 stale=@INIT_MODE_STR illegal", nvmeibt_seg_active_UUID_8(seg_active), mem_tbl_init_mode_str(mode));
		rv = -1;
	}

	return rv;
}

static void __attribute__ ((unused)) blkset_LUT_debug(int n_topo_segs, const bool l[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID])
{
	uint64_t i, n_blksets = (n_topo_segs << LOCK_CHANGE_STRIDE_SHIFT);		// Full rotation on praid
	N_Tf(t_01_ds_bse, "|@INT|@INT|@INT|@INT|@INT|@INT|@INT|@INT|@INT|@INT|", l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[9]);
	for (i = 0; i < n_blksets; i++) {
		N_Tf(t_02_ds_bse, "@ZU)@INT", i, nvmeibt_ds_blkset_entries_check_condition(i, n_topo_segs, l));
	}
}

static void init_is_blkset_used_LUT(const struct nvmeibt_praid_lot *praid_lot, int8_t seg_ind, bool is_blkset_used_LUT[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID])
{
	const struct nvmeibt_praid_config *pr_cfg = &praid_lot->from_config;
	const int n_segs = praid_lot->n_topo_seg_lots;
	const int max_n_used = min((pr_cfg->redundancy + 1), n_segs);
	int i;

	// Build bitmap of used entries for blockset according to config only
	// Explanation: lock scheme gives for seg_ind where it lock 'i' reside, by using '-i' we get the inverse query, which seg holds lock on me.
	memset(is_blkset_used_LUT, 0, sizeof(*is_blkset_used_LUT) * NVMEIBT_MAX_N_SEGMENTS_IN_PRAID);
	for (i = 0; i < max_n_used; i++) {
		const int8_t which_seg_holds_locks_on_me = get_owner_idx_by_owner_scheme_type(-i, pr_cfg->lock_scheme_type, seg_ind, n_segs);
		is_blkset_used_LUT[which_seg_holds_locks_on_me] = true;
	}

	// Daniel: Todo, Can use variant of nvmeibt_client_topo_disk_segment_upd_owners() function to take into account the topology as well
	// Example: Dont save blockset entries of dead segments.
	N_Tf(t_03_ds_bse, "lock_scheme=@INT, seg_ind=@INT, n_segs=@INT", pr_cfg->lock_scheme_type, seg_ind, n_segs);
	if (0) blkset_LUT_debug(n_segs, is_blkset_used_LUT);
}

bool nvmeibt_ds_metadata_init_EC_locks_table(struct nvmeibt_seg_active *seg_active)
{
	bool									is_stale_rebuild_required = 0;
	union nvmeib_lock_blkset_entry			init_val = {.all = 0};
	bool									is_init_dirty_required;
	bool									is_init_stale_required;
	union nvmeib_lock_blkset_entry			*mmapped_locks_table;
	const struct nvmeibt_local_disk			*local_disk;
	struct nvmeibt_disk_segment				*disk_segment;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx;
	struct nvmeibt_praid_lot				*applied_praid_lot;
	struct nvmeibt_praid_topo_ctx			*praid_topo_ctx;

	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	praid_topo_ctx = nvmeibt_seg_active_get_praid_applied_topo(seg_active);
	N_Tf(gtyu765, "seg=@UUID_8 lock_init_mode=@LOCK_INIT_MODE dirty_init_mode=@DIRTY_INIT_MODE",
		nvmeibt_seg_active_UUID_8(seg_active),
		mem_tbl_init_mode_str(seg_topo_ctx->stale_locks_init_mode),
		mem_tbl_init_mode_str(seg_topo_ctx->dirty_bits_init_mode));

	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(asdt65m, "seg=@UUID_8 is not local.", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	mmapped_locks_table = nvmeibt_seg_active_get_locks_tbl_ptr(seg_active);
	if (!mmapped_locks_table) {
		N_Wf(lpo09iy, "seg=@UUID_8 locks table is NOT mmapped, disk was probably removed",
			nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	// We get here only if not fully init_done. I.e. there shouldn't be any registered registrants

	// For cold recovery init, we first require that serjio finished reading from its partitions (serjio and journal)
	if (praid_topo_ctx->registrants_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I) {
		if (!nvmeibt_local_disk_is_serjio_ready(local_disk)) {
			goto out;	// Cannot make any progress towards INIT_DONE
		}
	}

	nvmeibt_register_eliminate_all_active_registrants_and_stales_of_seg_due_to_locks_table_reset(seg_active);

	// First see if we need to read the dirty_bits from persistency
	if (seg_topo_ctx->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST) {
		/* pesistent storage contains valid/clean data: try to restore */
		if (nvmeibt_ds_metadata_locks_table_restore(seg_active, &is_stale_rebuild_required) == 0) {	/* restore from persistent storage succeeded: all done */
			goto _out_success;
		} else {
			/* restore from persistent storage failed: fallback (see above) */
			seg_active->is_last_shutdown_clean = 0;
			seg_active->is_locktable_on_disk_corrupted = 1;			// Daniel: We can mark worst possible problems in RAM but be causios
			N_Ef(hji98fe, "seg=@UUID_8 restore failed (@AUTO_ERRNO), fallback to TURN_ALL_ON",nvmeibt_seg_active_UUID_8(seg_active));
			NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(dhu7875, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON);
		}
	}

	// Dirty_bits,  (see function comment for side-effects)
	if (ds_metadata_init_EC_prepare_dirty_and_txid_bits_init_val(seg_active, &init_val.blkset_info) < 0)
		goto out;

	// Stale_locks (see function comment for side-effects)
	if (ds_metadata_init_EC_prepare_stale_locks_init_val(seg_active, &init_val.lock_id, &is_stale_rebuild_required) < 0)
		goto out;

	// Now we are left with one of TURN_ALL_OFF or TURN_ALL_ON or INIT_DONE
	// Since one u64 holds both the lock and the dirty, we prepare a value that is based
	// on a combination of the two, and try to write it at once
	// If any of the two is already initialized, then we keep its old value

	is_init_dirty_required = (seg_topo_ctx->dirty_bits_init_mode !=  NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
	is_init_stale_required = (seg_topo_ctx->stale_locks_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);

	// Write over all the locks in a loop
	if (1) { // Todo: Move to separate function
		const uint64_t n_blksets = num_blksets_in_disk_segment(disk_segment);
		uint64_t i;
		bool is_init_all = (is_init_stale_required && is_init_dirty_required);
		bool is_blkset_used_LUT[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];

		applied_praid_lot = nvmeibt_seg_active_get_applied_praid_lot(seg_active);
		init_is_blkset_used_LUT(applied_praid_lot, nvmeibt_disk_segment_idx_in_praid(disk_segment), is_blkset_used_LUT);
		N_Tf(sdrt6b2, "Looping over n_blksets=@UINT64_TX init_dirty_required=@BOOL is_init_stale_required=@BOOL init_val=@LLX",
			 n_blksets, is_init_dirty_required, is_init_stale_required, init_val.all);

		for (i = 0; i < n_blksets; i++) {
			if (!nvmeibt_ds_blkset_entries_check_condition(i, applied_praid_lot->n_topo_seg_lots, is_blkset_used_LUT))
				continue; //Exactly ((D-1)/D+P) >= 50% for Raid 4+2 and higher.
			if (is_init_all) {
				mmapped_locks_table[i] = init_val;
			} else {	// Daniel: Pretty sure this 'else' is unreachable. But be cauchious
				if (is_init_stale_required) {
					mmapped_locks_table[i].lock_id = init_val.lock_id;
				}
				if (is_init_dirty_required) {
					mmapped_locks_table[i].blkset_info = init_val.blkset_info;
				}
			}
		}

		if (init_val.blkset_info.bits.txid == INITIAL_LAZY_READ_TXID)
			nvmeibt_seg_active_mark_txid_rebuild_required_if_needed(seg_active);
	}

_out_success:
	NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(fhu82ws, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
	NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(bnki98e, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
out:
	NFOUT;
	return is_stale_rebuild_required;
}

static int ds_metadata_prepare_non_EC_dirty_and_txid_bits_init_val(struct nvmeibt_seg_active *seg_active, union nvmeib_blkset_info *blkset_info_init_val)
{
	int										rv = 0;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx;
	union nvmeibc_dbits_entry				dbits_entry;
	const union nvmeibc_dbits_entry			dbits_entry_zero = { .all_bits = 0 };
	const union nvmeibc_dbits_entry			dbits_entry_unknown = nvmeib_dbits_entry_single_unk();	// Todo: EC-5969, Coz this is R1, onyl 1 unknown possible. Revisit completely for 3 replica

	// NFIN;
	seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	N_Tf(trace_ds_metadata_ds_metadata_init_non_EC_dirty_bits, "seg=@UUID_8 init_mode=@INIT_MODE_STR",
		nvmeibt_seg_active_UUID_8(seg_active),
		mem_tbl_init_mode_str(seg_topo_ctx->dirty_bits_init_mode));

	dbits_entry = dbits_entry_zero;
	switch (seg_topo_ctx->dirty_bits_init_mode) {
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST:
		/* Init with FROM_PERSIST always applies to both stale locks and dirty
		 * bits, and should have been already applied before we are called -
		 * see nvmeibt_ds_metadata_init_non_EC_locks_table().
		 * However, if we did reach here, it means that restore from persistent
		 * storage has failed; so here we assume worse case and turn all dirty
		 * bits on.
		 */
		// Daniel: This is a trap, here should decide upon topology and either set Y or N
		dbits_entry = dbits_entry_unknown;
		rv = 1;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON:
		 // If no convict given client will read data and compare before syncing - previously was marked dirty nvmeib_dbits_entry_build_for_seg(idx_in_praid ^ 1)
		dbits_entry = dbits_entry_unknown;
		rv = 1;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER:
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF:
		rv = 1;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN:
	default:
		N_Ef(error_ds_metadata_ds_metadata_init_non_EC_dirty_bits, "init_mode=@INIT_MODE", seg_topo_ctx->dirty_bits_init_mode);
		rv = -1;
		break;
	}
	blkset_info_init_val->bits.txid = INITIAL_LAZY_READ_TXID;
	blkset_info_init_val->bits.dirty = dbits_entry.dirty.bits;

	NFOUT;
	return rv;
}

static int ds_metadata_prepare_non_EC_stale_locks_init_val(struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id *lock_id_init_val, bool *is_stale_rebuild_required)
{
	int	 									rv = 0;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx;
	union nvmeib_lock_id					stale_lock_zero = { .all = 0 };

	NFIN;
	seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);

	N_Tf(u876gt2, "Received @MEM_CTL_INIT_MODE_STR", mem_tbl_init_mode_str(seg_topo_ctx->stale_locks_init_mode));

	*is_stale_rebuild_required = 0;
	*lock_id_init_val = stale_lock_zero;
	switch (seg_topo_ctx->stale_locks_init_mode) {
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE:
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST:
		/*
		 * Init with FROM_PERSIST always applies to both stale locks and dirty
		 * bits, and should have been already applied before we are called -
		 * see nvmeibt_ds_metadata_init_non_EC_locks_table().
		 * However, if we did reach here, it means that restore from persistent
		 * storage has failed; so here we assume worse case and turn all dirty
		 * bits on.
		 */
		FALLTHRU;
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON:
		*lock_id_init_val = nvmeib_stale_special_raid1.lock_id;
		*is_stale_rebuild_required = 1;
		rv = 1;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER:
	case NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF:
		nvmeibt_register_eliminate_all_active_registrants_and_stales_of_seg_due_to_locks_table_reset(seg_active);
		rv = 1;
		break;
	case NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN:
	default:
		N_Ef(gy76x29, "init_mode=@INIT_MODE", seg_topo_ctx->stale_locks_init_mode);
		rv = -1;
		break;
	}

	NFOUT;
	return rv;
}

bool nvmeibt_ds_metadata_init_non_EC_locks_table(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment				*disk_segment;
	enum NVMEIBT_MEM_TBL_INIT_MODE			dirty_bits_init_mode;
	enum NVMEIBT_MEM_TBL_INIT_MODE			stale_locks_init_mode;
	int										rv;
	bool									is_stale_rebuild_required = 0;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx;
	union nvmeib_lock_blkset_entry			lock_blkset_entry_init_val;
	union nvmeib_lock_blkset_entry			*mmapped_locks_tbl = nvmeibt_seg_active_get_locks_tbl_ptr(seg_active);
	int8_t									idx_in_praid;
	uint64_t								n_blksets, ii;
	bool									is_init_dirty_required;
	bool									is_init_stale_required;

	NFIN;

	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	dirty_bits_init_mode = seg_topo_ctx->dirty_bits_init_mode;
	stale_locks_init_mode = seg_topo_ctx->stale_locks_init_mode;

	N_Tf(fr65w2q, "seg=@UUID_8 lock_init_mode=@LOCK_INIT_MODE dirty_init_mode=@DIRTY_INIT_MODE",
		nvmeibt_seg_active_UUID_8(seg_active), mem_tbl_init_mode_str(stale_locks_init_mode), mem_tbl_init_mode_str(dirty_bits_init_mode));

	if (nvmeibt_local_disk_is_being_deleted(nvmeibt_seg_active_get_local_disk(seg_active))) {
		N_Tf(doo09da, "seg=@UUID_8 is being deleted (so not local)", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	if (!mmapped_locks_tbl) {
		N_Wf(kkii98s, "Disk seg=@UUID_8 locks table is NOT mmapped, disk was probably removed", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	/*
	 * If initialization mode is FROM_PERSIST, _and_ if persistent storage hold
	 * trustworthy data (i.e. is_last_shutdown_clean), then we initialize by
	 * restoring the data from peristent storage; However if that fails, we
	 * fallback to call the specific initialization functions - and they will
	 * set all the stale-locks/dirty-bits.
	 */

	if (dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST ||
		stale_locks_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST) {

		/* we expect init mode of both stale locks and dirty bits to match */
		if (dirty_bits_init_mode != stale_locks_init_mode) {
			N_Ef(error_ds_metadata_nvmeibt_ds_metadata_init_non_EC_locks_table, "seg=@UUID_8 init modes mismatch", nvmeibt_seg_active_UUID_8(seg_active));
		}

		if (seg_active->is_last_shutdown_clean) {		// note: this is the value read when the segment was discovered, not the "current" value
			/* pesistent storage contains valid/clean data: try to restore */
			if (nvmeibt_ds_metadata_locks_table_restore(seg_active, &is_stale_rebuild_required) == 0) {
				NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(fhuy7t5, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
				NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(sj98476, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
				/* restore from persistent storage succeeded: all done */
				goto out;
			}
			/* restore from persistent storage failed: fallback (see above) */
			seg_active->is_last_shutdown_clean = 0;
			seg_active->is_locktable_on_disk_corrupted = 1;			// Daniel: We can mark worst possible problems in RAM but be causios
			N_Ef(gfdu878, "seg=@UUID_8 restore failed (@AUTO_ERRNO), fallback to turn-on-all",nvmeibt_seg_active_UUID_8(seg_active));
		} else {
			/* pesistent storage does not contain valid/clean data: fallback */
			N_Tf(wuu811n, "seg=@UUID_8 last shutdown not clean, fallback to turn-on-all", nvmeibt_seg_active_UUID_8(seg_active));
		}

		/*
		 * Fallback to standard init: in mode FROM_PERSIST the two functions
		 * below will force turn-on-all for both stale locks and dirty bits.
		 */
	}

	rv = ds_metadata_prepare_non_EC_dirty_and_txid_bits_init_val(seg_active, &lock_blkset_entry_init_val.blkset_info);
	if (rv < 0)
		goto out;
	is_init_dirty_required = rv;

	rv = ds_metadata_prepare_non_EC_stale_locks_init_val(seg_active, &lock_blkset_entry_init_val.lock_id, &is_stale_rebuild_required);
	if (rv < 0)
		goto out;
	is_init_stale_required = rv;
	rv = 0;
	
	if (!(is_init_stale_required || is_init_dirty_required)) {
		is_stale_rebuild_required = 0;
		goto out;
	}

	NTOMA_ASSERT(hq123xc, (is_init_stale_required && is_init_dirty_required), "Init modes mismatch");

	idx_in_praid = nvmeibt_disk_segment_idx_in_praid(disk_segment);
	n_blksets = num_blksets_in_disk_segment(disk_segment);
	N_Tf(by65tf3, "seg=@UUID_8 idx_in_praid=@IDX_IN_PRAID locks_tbl=@LOCKS_TBL n_blksets=@UINT64_TX",
		 nvmeibt_seg_active_UUID_8(seg_active), idx_in_praid, mmapped_locks_tbl, n_blksets);

	for (ii = 0; ii < n_blksets; ii++) {
		mmapped_locks_tbl[ii] = lock_blkset_entry_init_val;
	}

	NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(ryf8xms, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
	NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(u876fr4, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);

out:
	NFOUT;
	return is_stale_rebuild_required;
}

uint64_t nvmeibt_ds_blkset_entries___pack(union nvmeib_lock_blkset_entry *src, char *dst_buf, struct nvmeibt_disk_segment *ds)
{
	struct nvmeibt_praid			*praid = ds->seg_mgmt.its_praid;
	struct nvmeibt_praid_lot		*applied_praid_lot = &praid->praid_follower.applied_praid_lot;
	const uint64_t					n_blocksets = num_blksets_in_disk_segment(ds);
	const bool						do_pack = nvmeibt_praid_is_type_EC(praid);
	uint64_t packed_n_bytes;
	if (do_pack) {
		bool is_blkset_used_LUT[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];
		union nvmeib_lock_blkset_entry *dst = (typeof(dst))dst_buf;
		uint64_t i;
		init_is_blkset_used_LUT(applied_praid_lot, nvmeibt_disk_segment_idx_in_praid(ds), is_blkset_used_LUT);
		for (i = 0; i < n_blocksets; i++) {
			if (nvmeibt_ds_blkset_entries_check_condition(i, applied_praid_lot->n_topo_seg_lots, is_blkset_used_LUT)) {
				(*dst++) = src[i];
			}
		}
		packed_n_bytes = ((char*)dst - dst_buf);
	} else if (nvmeibt_praid_is_jbod(praid)) {
		packed_n_bytes = 0;						// Currently: Blockset entry is not used at all
	} else {
		packed_n_bytes = NVMEIB_LOCK_BLKSET_ENTRY_SIZE * n_blocksets;
		memcpy(dst_buf, src, packed_n_bytes);
	}
	N_Tf(t_0x_ds_bse, "@UUID_8:n_blocksets=@ZU -> n_bytes=@ZU", nvmeibt_seg_UUID_8(ds), n_blocksets, packed_n_bytes);
	return packed_n_bytes;
}

uint64_t nvmeibt_ds_blkset_entries_unpack(union nvmeib_lock_blkset_entry *dst, char *src_buf, struct nvmeibt_disk_segment *ds)
{
	struct nvmeibt_praid			*praid = ds->seg_mgmt.its_praid;
	struct nvmeibt_praid_lot		*applied_praid_lot = &praid->praid_follower.applied_praid_lot;
	const uint64_t					n_blocksets = num_blksets_in_disk_segment(ds);
	const bool						do_pack = nvmeibt_praid_is_type_EC(praid);
	uint64_t packed_n_bytes;
	if (do_pack) {
		bool is_blkset_used_LUT[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];
		const union nvmeib_lock_blkset_entry *src = (typeof(dst))src_buf;
		uint64_t i;
		init_is_blkset_used_LUT(applied_praid_lot, nvmeibt_disk_segment_idx_in_praid(ds), is_blkset_used_LUT);
		for (i = 0; i < n_blocksets; i++) {
			if (nvmeibt_ds_blkset_entries_check_condition(i, applied_praid_lot->n_topo_seg_lots, is_blkset_used_LUT)) {
				dst[i] = (*src++);
				dst[i].lock_id.all = 0; // The old lock id cannot be used because we are unable to find the corresponding client
			}
		}
		packed_n_bytes = ((char*)src - src_buf);
	} else if (nvmeibt_praid_is_jbod(praid)) {
		packed_n_bytes = 0;						// Currently: Blockset entry is not used at all
	} else {
		packed_n_bytes = NVMEIB_LOCK_BLKSET_ENTRY_SIZE * n_blocksets;
		memcpy(dst, src_buf, packed_n_bytes);
	}
	N_Tf(t_0y_ds_bse, "@UUID_8:n_blocksets=@ZU <- n_bytes=@ZU", nvmeibt_seg_UUID_8(ds), n_blocksets, packed_n_bytes);
	return packed_n_bytes;
}

