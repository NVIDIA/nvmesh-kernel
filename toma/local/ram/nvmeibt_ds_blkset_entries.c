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

static u16 calc_dirty_bits_by_topo(struct nvmeibt_seg_active *seg_active)
{
	const struct nvmeibt_praid_lot			*praid_lot = nvmeibt_seg_active_get_applied_praid_lot(seg_active);
	int8_t									n_parity;
	int8_t									n_non_owners = 0;
	bool									is_seg_degraded;

	if (!praid_lot) {
		N_Ef(t01dstidb, "No praid for seg_active, cant set unkown dbits properly!");
		nvmeibt_abort(ES_FATAL);
	}

	n_parity = praid_lot->from_config.redundancy;
	is_seg_degraded = !nvmeibt_disk_segment_is_competent_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active));
	for (int i = 0; i < praid_lot->n_topo_seg_lots; i++)
		n_non_owners += !nvmeibt_disk_segment_is_competent_owner(&(praid_lot->topo_seg_lots[i]->seg_topo));

	N_Tf(tyeund1, "seg=@UUID_8: n_non_owners=@INT n_replicas=@INT n_parity=@INT is_degraded=@INT",
		 nvmeibt_seg_active_UUID_8(seg_active), (int)n_non_owners, (int)(praid_lot->n_topo_seg_lots), (int)n_parity, (int)is_seg_degraded);
	return nvmeib_dbits_entry_build_unknowns_generic(n_non_owners, praid_lot->n_topo_seg_lots, n_parity, is_seg_degraded).all_bits;
}

static int ds_metadata_init_EC_prepare_dirty_and_txid_bits_init_val(struct nvmeibt_seg_active *seg_active, union nvmeib_blkset_info *binfo)
{
	const enum NVMEIBT_MEM_TBL_INIT_MODE mode = seg_active->active_seg_topo.dirty_bits_init_mode;
	switch (mode) {
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER:
		binfo->bits.dirty = 0;
		binfo->bits.txid = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS;
		return 0;
	case NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO:
		binfo->bits.dirty = calc_dirty_bits_by_topo(seg_active);
		binfo->bits.txid = INITIAL_LAZY_READ_TXID;
		return 0;
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT:
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE:
		return 0;
	default:
		N_Ef(dgy76gr, "seg=@UUID_8, illegal init_mode=@INIT_MODE_STR", nvmeibt_seg_active_UUID_8(seg_active), mem_tbl_init_mode_str(mode));
		return -1;
	}
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

static inline bool is_init_mode_turn_all(enum NVMEIBT_MEM_TBL_INIT_MODE init_mode)
{
	return (init_mode & (NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON | NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF));
}

static void convert_set_all_to_by_topo(struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	if (is_init_mode_turn_all(topo_ctx->dirty_bits_init_mode))
		topo_ctx->dirty_bits_init_mode = NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO;
	if (is_init_mode_turn_all(topo_ctx->stale_locks_init_mode))
		topo_ctx->stale_locks_init_mode = NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO;
}

bool nvmeibt_ds_metadata_init_EC_locks_table(struct nvmeibt_seg_active *seg_active)
{
	bool									is_stale_rebuild_required = 0;
	union nvmeib_lock_blkset_entry			init_val = {.all = 0};
	union nvmeib_lock_blkset_entry			*mmapped_locks_table;
	const struct nvmeibt_local_disk			*local_disk;
	struct nvmeibt_disk_segment				*disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	struct nvmeibt_praid_topo_ctx			*praid_topo_ctx = nvmeibt_seg_active_get_praid_applied_topo(seg_active);
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

	convert_set_all_to_by_topo(seg_topo_ctx);
	nvmeibt_register_eliminate_all_active_registrants_and_stales_of_seg_due_to_locks_table_reset(seg_active);

	// First see if we need to read the dirty_bits from persistency
	if (seg_topo_ctx->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST) {
		/* pesistent storage contains valid/clean data: try to restore */
		if (nvmeibt_ds_metadata_locks_table_restore(seg_active, &is_stale_rebuild_required) == 0)
			goto _out_success;
		/* restore from persistent storage failed: fallback (see above) */
		seg_active->is_last_shutdown_clean = 0;
		seg_active->is_locktable_on_disk_corrupted = 1;			// Daniel: We can mark worst possible problems in RAM but be causios
		N_Ef(hji98fe, "seg=@UUID_8 restore failed (@AUTO_ERRNO), fallback to BY_TOPO",nvmeibt_seg_active_UUID_8(seg_active));
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(dhu7875, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO);
	}

	// Dirty_bits,  (see function comment for side-effects)
	if (ds_metadata_init_EC_prepare_dirty_and_txid_bits_init_val(seg_active, &init_val.blkset_info) < 0)
		goto out;

	// Stale_locks are always OFF for EC, nothing to init
	// Write over all the locks in a loop
	if (1) { // Todo: Move to separate function
		const uint64_t n_blksets = num_blksets_in_disk_segment(disk_segment);
		uint64_t i;
		const bool is_init_dirty_required = (seg_topo_ctx->dirty_bits_init_mode !=  NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
		const bool is_init_stale_required = (seg_topo_ctx->stale_locks_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
		bool is_init_all = (is_init_stale_required && is_init_dirty_required);
		bool is_blkset_used_LUT[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];
		const struct nvmeibt_praid_lot *applied_praid_lot = nvmeibt_seg_active_get_applied_praid_lot(seg_active);
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

static int ds_metadata_prepare_non_EC_dirty_and_txid_bits_init_val(struct nvmeibt_seg_active *seg_active, union nvmeib_blkset_info *binfo)
{
	const enum NVMEIBT_MEM_TBL_INIT_MODE mode = seg_active->active_seg_topo.dirty_bits_init_mode;
	binfo->bits.txid = INITIAL_LAZY_READ_TXID;		// TxID not used in mirror
	switch (mode) {
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT:
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE:
		return 0;
	case NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO:
		binfo->bits.dirty = calc_dirty_bits_by_topo(seg_active);
		return 1;
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER:
		return 1;			// Init to zero dbit
	default:
		N_Ef(dgy76gs, "seg=@UUID_8, illegal init_mode=@INIT_MODE_STR", nvmeibt_seg_active_UUID_8(seg_active), mem_tbl_init_mode_str(mode));
		return -1;
	}
}

static int ds_metadata_prepare_non_EC_stale_locks_init_val(struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id *lock_id_init_val, bool *is_stale_rebuild_required)
{
	const struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	const enum NVMEIBT_MEM_TBL_INIT_MODE mode = seg_topo_ctx->stale_locks_init_mode;
	*lock_id_init_val = (union nvmeib_lock_id){ .all = 0 };
	switch (mode) {
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT:
	case NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE:
		return 0;
	case NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO:
		if (nvmeibt_disk_segment_is_competent_owner(seg_topo_ctx)) {
			*lock_id_init_val = nvmeib_stale_special_raid1.lock_id;
			*is_stale_rebuild_required = true;
		} else {
			nvmeibt_register_eliminate_all_active_registrants_and_stales_of_seg_due_to_locks_table_reset(seg_active);
		}
		return 1;
	case NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER:
		nvmeibt_register_eliminate_all_active_registrants_and_stales_of_seg_due_to_locks_table_reset(seg_active);
		return 1;
	default:
		N_Ef(dgy76gt, "seg=@UUID_8, illegal init_mode=@INIT_MODE_STR", nvmeibt_seg_active_UUID_8(seg_active), mem_tbl_init_mode_str(mode));
		return -1;
	}
}

bool nvmeibt_ds_metadata_init_non_EC_locks_table(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment				*disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	enum NVMEIBT_MEM_TBL_INIT_MODE			dirty_bits_init_mode;
	enum NVMEIBT_MEM_TBL_INIT_MODE			stale_locks_init_mode;
	int										rv;
	bool									is_stale_rebuild_required = 0;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	union nvmeib_lock_blkset_entry			lock_blkset_entry_init_val = nvmeib_lock_init_value;
	union nvmeib_lock_blkset_entry			*mmapped_locks_tbl = nvmeibt_seg_active_get_locks_tbl_ptr(seg_active);
	bool									is_init_dirty_required;
	bool									is_init_stale_required;

	NFIN;
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

	convert_set_all_to_by_topo(seg_topo_ctx);
	if (dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST ||
		stale_locks_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST) {

		/* we expect init mode of both stale locks and dirty bits to match */
		if (dirty_bits_init_mode != stale_locks_init_mode) {
			N_Ef(error_ds_metadata_nvmeibt_ds_metadata_init_non_EC_locks_table, "seg=@UUID_8 init modes mismatch", nvmeibt_seg_active_UUID_8(seg_active));
		}

		/* pesistent storage contains valid/clean data: try to restore */
		if (nvmeibt_ds_metadata_locks_table_restore(seg_active, &is_stale_rebuild_required) == 0)
			goto _out_success;
		/* restore from persistent storage failed: fallback (see above) */
		seg_active->is_last_shutdown_clean = 0;
		seg_active->is_locktable_on_disk_corrupted = 1;			// Daniel: We can mark worst possible problems in RAM but be causios
		N_Ef(gfdu878, "seg=@UUID_8 restore failed (@AUTO_ERRNO), fallback to BY_TOPO",nvmeibt_seg_active_UUID_8(seg_active));
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(fhu8st5, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO);
		NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(sm18476, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_BY_TOPO);
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

	NTOMA_ASSERT(hq123xc, (is_init_stale_required ^ is_init_dirty_required), "Init modes mismatch");
	{
		const int8_t idx_in_praid = nvmeibt_disk_segment_idx_in_praid(disk_segment);
		uint64_t ii, n_blksets = num_blksets_in_disk_segment(disk_segment);
		N_Tf(by65tf3, "seg=@UUID_8 idx_in_praid=@IDX_IN_PRAID locks_tbl=@LOCKS_TBL n_blksets=@UINT64_TX",	nvmeibt_seg_active_UUID_8(seg_active), idx_in_praid, mmapped_locks_tbl, n_blksets);
		for (ii = 0; ii < n_blksets; ii++)
			mmapped_locks_tbl[ii] = lock_blkset_entry_init_val;
	}
_out_success:
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

