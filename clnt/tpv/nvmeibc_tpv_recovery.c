/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_recovery.c - TPV cold recovery: orphaned CDV_extent detection.
 *
 * Background
 * ----------
 * The L1/L2 tree in the per-TPV tree extent records only virtual extents
 * that have been mapped AND flushed.  TOMA's cdv_extent_md records every CDV_extent
 * allocated to a TPV regardless of whether any virtual extent has been
 * mapped within it.  A crash between CDV_ALLOC_OK and the first flush that
 * writes a leaf for a slot in the new CDV_extent therefore leaves an
 * "orphaned" CDV_extent: TOMA believes it belongs to this TPV, but the
 * flat-L1 tree has no reference to it, so load_state() never adds it to the
 * allocator's cdv_extent_list or free pool.
 *
 * Recovery
 * --------
 * nvmeibc_tpv_recovery() is called unconditionally from
 * nvmeibc_tpv_attach() after nvmeibc_tpv_load_state() succeeds.  We
 * do not know what happened while the TPV was offline, so the TOMA
 * extent list must always be cross-checked - even for a volume whose
 * tree is empty (all CDV_extents may have been allocated after the
 * last flush, or the volume may have been fully DISCARDed).
 *
 * Algorithm:
 *   1. Snapshot the CDV allocator TOMA identity (toma_id, generation).
 *   2. Query TOMA for all data CDV_extents assigned to this TPV UUID via
 *      nvmeibc_ib_admin_cdv_list_extents().
 *   3. Walk the TOMA list.  For each extent_index that does NOT appear in
 *      the allocator's in-memory cdv_extent_list:
 *        a. Allocate a nvmeibc_cdv_extent_ref with allocated_count = 0.
 *        b. Allocate n_slots nvmeibc_tpv_free_slot entries, one per
 *           TPV_extent slot within the CDV_extent.
 *        c. Append both to the allocator lists (no lock: single-threaded).
 *   4. Log a summary and return 0.
 *
 * Failure policy
 * --------------
 * Recovery errors are non-fatal.  If the TOMA query fails or slot
 * allocations fail, recovery logs the problem and continues.  The allocator
 * runs with a reduced free pool; NVCK will reclaim any unrecovered orphans
 * on the next maintenance scan.
 *
 * Locking
 * -------
 * No allocator.lock is taken: recovery runs at attach time, before IO gates
 * open and before work structs are scheduled, so the allocator is single-
 * threaded.  allocator_id_lock is taken only for the TOMA ID snapshot.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"	/* nvmeibc_volume, hdr.uuid */

/* -- Forward declaration --------------------------------------------------- */

/*
 * nvmeibc_ib_admin_cdv_list_extents - query TOMA for the set of data
 * CDV_extents whose cdv_extent_md is DATA / tpv_uuid.
 *
 * On success, *out_indices is set to a kvmalloc'd array of *out_count u64
 * extent indices; the caller must kvfree(*out_indices).  On failure,
 * *out_indices is NULL and *out_count is 0.
 *
 * Returns 0 on success, negative errno on failure.
 * Blocks; called only from process context (attach / recovery path).
 *
 * Implemented in nvmeibc_ib_admin_channel.c (S.2.8).
 */
extern int nvmeibc_ib_admin_cdv_list_extents(struct nvmeibc_volume *cdv,
					     const char *toma_id,
					     const char *tpv_uuid,
					     u64 **out_indices,
					     u64 *out_count);

/* -- Local geometry helpers ----------------------------------------------- */

/*
 * Mirror the geometry helpers in nvmeibc_tpv_allocator.c.  Kept local to
 * avoid coupling recovery to the allocator's static functions.
 */

static inline u64 recov_alloc_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return a->allocator_size_gib << 30;		/* A in bytes */
}

static inline u64 recov_extent_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->cdv_extent_size_mib << 20;	/* E in bytes */
}

static inline u64 recov_slot_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->tpv_extent_size_kb << 10;	/* T in bytes */
}

static inline u64 recov_slots_per_extent(const struct nvmeibc_tpv_allocator *a)
{
	return recov_extent_bytes(a) / recov_slot_bytes(a);
}

/*
 * Encode (1-based extent_index, slot) back to a CDV byte offset.  Must mirror
 * tpv_slot_phys_offset() in nvmeibc_tpv_allocator.c and persist_decode_phys()
 * in nvmeibc_tpv_persist.c, both of which treat extent_index as 1-based
 * (persist_decode_phys adds 1 to the quotient; tpv_slot_phys_offset subtracts
 * 1 before multiplying).  An earlier version of this helper used
 * extent_index * E, which places slot 0 of extent 1 at A + E - one full
 * CDV_extent past the correct location - causing orphan adoption to install
 * free_slots with phys_offsets pointing at the wrong extent.
 */
static inline u64 recov_slot_phys(const struct nvmeibc_tpv_allocator *a,
				   u64 extent_index, u64 slot)
{
	return recov_alloc_bytes(a) +
	       (extent_index - 1) * recov_extent_bytes(a) +
	       slot * recov_slot_bytes(a);
}

/* -- tpv_recovery_is_known --------------------------------------------------
 *
 * Returns true if extent_index is already in the allocator's cdv_extent_list.
 * Called from single-threaded recovery context; no lock held.
 */
static bool tpv_recovery_is_known(const struct nvmeibc_tpv_allocator *alloc,
				  u64 extent_index)
{
	const struct nvmeibc_cdv_extent_ref *ref;

	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->extent_index == extent_index)
			return true;
	}
	return false;
}

/* -- tpv_recovery_adopt_orphan ----------------------------------------------
 *
 * Integrate an orphaned CDV_extent into the allocator.
 *
 * An orphan is a CDV_extent that TOMA allocated to this TPV (cdv_extent_md
 * == DATA / tpv_uuid) but that has no leaves in the flat-L1 tree and was
 * therefore missed by load_state().
 *
 * Adoption procedure:
 *   1. Allocate a nvmeibc_cdv_extent_ref with allocated_count = 0 (all
 *      slots are free: no virtual extent has ever been mapped into this
 *      CDV_extent, or all mappings were lost in the crash).
 *   2. Build n_slots nvmeibc_tpv_free_slot entries into a local batch.
 *   3. Append the ref to cdv_extent_list and splice the batch into
 *      free_tpv_extents.
 *
 * On -ENOMEM the local batch is cleaned up and the orphaned CDV_extent
 * remains invisible to the allocator until the next recovery run.
 *
 * Must be called from single-threaded context (attach / recovery path).
 */
static int tpv_recovery_adopt_orphan(struct nvmeibc_tpv *tpv, u64 extent_index,
				      bool is_meta_side)
{
	struct nvmeibc_tpv_allocator  *alloc = is_meta_side ? tpv->meta_allocator
							    : &tpv->allocator;
	struct nvmeibc_cdv_extent_ref *ref;
	struct nvmeibc_tpv_free_slot  *fs;
	u64   n_slots = recov_slots_per_extent(alloc);
	u64   s, first_s = 0;
	bool  promote_to_l1 = false;
	/*
	 * The L1 extent lives on the side that owns the tree. In single-CDV
	 * mode that's the data allocator; in split mode it's the metadata
	 * allocator. Only that side may promote an orphan to the L1 host.
	 */
	bool  this_side_hosts_l1 = !nvmeibc_tpv_is_split(tpv) || is_meta_side;
	LIST_HEAD(batch);

	ref = kzalloc(sizeof(*ref), GFP_NOIO);
	if (!ref)
		return -ENOMEM;

	/*
	 * If the TPV has no L1 extent yet (client crashed between a CDV_ALLOC
	 * success and the first flush_state), promote the first orphan we
	 * adopt.  Without this the next flush_state has nowhere to write L1.
	 * Mirrors the tpv_on_cdv_alloc_ok bootstrap path: reserve slot 0 for
	 * the forthcoming L1 write and keep the rest as free slots.
	 */
	if (this_side_hosts_l1 && alloc->l1_extent_index == 0) {
		alloc->l1_extent_index = extent_index;
		promote_to_l1          = true;
		first_s                = 1;	/* skip slot 0 in free-pool splice */
		/*
		 * Fresh L1 extent (promoted from orphan): on-disk slot 0 is
		 * garbage, so force the first flush to rewrite the full L1.
		 */
		nvmeibc_tpv_mark_l1_full_dirty(tpv);
	}

	ref->extent_index    = extent_index;
	ref->allocated_count = 0;
	ref->l2_slots        = 0;
	ref->is_l1_extent    = promote_to_l1;
	nvmeibc_cdv_extent_ref_init_lists(ref);

	for (s = first_s; s < n_slots; s++) {
		u64 phys = recov_slot_phys(alloc, extent_index, s);

		/*
		 * Skip slots whose offset is 0; collides with TPV_TREE_NULL.
		 * See nvmeibc_tpv_allocator.c tpv_on_cdv_alloc_ok_for_side for
		 * the canonical rationale (split mode, data CDV, A = 0).
		 */
		if (phys == 0)
			continue;

		fs = kzalloc(sizeof(*fs), GFP_NOIO);
		if (!fs) {
			struct nvmeibc_tpv_free_slot *tmp;

			while (!list_empty(&batch)) {
				tmp = list_first_entry(&batch, struct nvmeibc_tpv_free_slot, node);
				list_del(&tmp->node);
				kfree(tmp);
			}
			kfree(ref);
			return -ENOMEM;
		}
		fs->phys_offset      = phys;
		fs->cdv_extent_index = extent_index;
		INIT_LIST_HEAD(&fs->node);
		list_add_tail(&fs->node, &batch);
	}

	/*
	 * Append without lock: single-threaded at attach time.  IO gates
	 * are not yet open; no work struct is running.
	 */
	list_add_tail(&ref->node, &alloc->cdv_extent_list);
	alloc->cdv_extents_count++;
	list_splice_tail(&batch, &alloc->free_tpv_extents);
	alloc->free_tpv_extent_count += (n_slots - first_s);

	/*
	 * Step 2 Commit 2 extension: if this adopted orphan has no data
	 * or L2 slots (allocated_count == 0) and is not the L1 extent,
	 * it is immediately returnable.  Move it onto pending_return_list
	 * so the next cdv_alloc_work drains it.  Without this, an
	 * adopted-but-empty extent would sit on cdv_extent_list
	 * indefinitely because nothing else triggers flush_and_promote's
	 * pending_return_list transition -- the extent has no parked
	 * frees to process.
	 *
	 * The extent's slots stay in alloc->free_tpv_extents (we just
	 * spliced them).  The drain's watermark gate may or may not let
	 * them go right away; if not, they remain allocatable until the
	 * pool is deep enough to allow the return.
	 */
	if (ref->allocated_count == 0 && !ref->is_l1_extent) {
		/*
		 * Keep cdv_extents_count at its post-increment value: the
		 * extent is still owned by this TPV from TOMA's perspective
		 * until the drain sends CDV_FREE_EXTENT.  The count decrements
		 * only on successful IB admin send in
		 * tpv_drain_pending_returns.
		 */
		list_move_tail(&ref->node, &alloc->pending_return_list);
		ref->on_pending_return_list = true;
		atomic64_inc(&alloc->stat_cdv_returns_queued);
	}

	return 0;
}

/* -- nvmeibc_tpv_recovery ---------------------------------------------------
 *
 * Cold recovery entry point.  See file header for full description.
 *
 * Returns 0 always.  Individual adoption failures are logged but do not
 * abort recovery of the remaining extents, and do not fail the attach.
 */
/*
 * tpv_recovery_one_side - run orphan reconciliation for a single CDV side.
 *
 * Returns 0 always; adoption failures are logged but non-fatal.
 */
static int tpv_recovery_one_side(struct nvmeibc_tpv *tpv, bool is_meta_side)
{
	struct nvmeibc_tpv_allocator *alloc;
	struct nvmeibc_volume        *cdv_vol;
	spinlock_t                   *id_lock;
	const char                   *id_src;
	char   toma_id[NVMEIB_HOST_NAME_LEN];
	u64   *toma_indices = NULL;
	u64    toma_count   = 0;
	u64    n_orphans    = 0;
	unsigned long flags;
	u64    i;
	int    rv;

	if (is_meta_side) {
		alloc   = tpv->meta_allocator;
		cdv_vol = tpv->meta_cdv_vol;
		id_lock = &tpv->meta_allocator_id_lock;
		id_src  = tpv->meta_allocator_toma_id;
	} else {
		alloc   = &tpv->allocator;
		cdv_vol = tpv->cdv_vol;
		id_lock = &tpv->allocator_id_lock;
		id_src  = tpv->allocator_toma_id;
	}

	if (!alloc || !cdv_vol)
		return 0;

	/* -- 1. Snapshot the allocator TOMA identity ----------- */
	spin_lock_irqsave(id_lock, flags);
	strncpy(toma_id, id_src, sizeof(toma_id) - 1);
	toma_id[sizeof(toma_id) - 1] = '\0';
	spin_unlock_irqrestore(id_lock, flags);

	if (toma_id[0] == '\0') {
		_NW(tpv_recovery_no_toma,
		    "TPV: @STR: no allocator TOMA ID at recovery time (meta=@INT); orphan check skipped",
		    tpv->tpv_name, (int)is_meta_side);
		return 0;
	}

	/* -- 2. Get TOMA extent list (prefer cached from load_state) -- */
	if (alloc->toma_extent_list && alloc->toma_extent_count > 0) {
		toma_indices = alloc->toma_extent_list;
		toma_count   = alloc->toma_extent_count;
		alloc->toma_extent_list  = NULL;
		alloc->toma_extent_count = 0;
	} else {
		rv = nvmeibc_ib_admin_cdv_list_extents(cdv_vol, toma_id,
						       tpv->tpv_uuid,
						       &toma_indices,
						       &toma_count);
		if (rv) {
			_NE(tpv_recovery_list_fail,
			    "TPV: @STR: CDV_LIST_EXTENTS to @STR failed rv=@INT (meta=@INT); orphan check skipped",
			    tpv->tpv_name, toma_id, rv, (int)is_meta_side);
			return 0;
		}
	}

	_NT(tpv_recovery_list_ok,
	    "TPV: @STR: TOMA reports @LLU CDV_extents (meta=@INT); tree has @LLU",
	    tpv->tpv_name, toma_count, (int)is_meta_side, alloc->cdv_extents_count);

	/* -- 3. Cross-reference and adopt orphans --------------- */
	for (i = 0; i < toma_count; i++) {
		u64 eidx = toma_indices[i];
		bool known;

		known = tpv_recovery_is_known(alloc, eidx);
		if (known)
			continue;

		_NI(tpv_recovery_orphan_found,
		    "TPV: @STR: CDV_extent[@LLU] orphaned (meta=@INT); adopting",
		    tpv->tpv_name, eidx, (int)is_meta_side);

		rv = tpv_recovery_adopt_orphan(tpv, eidx, is_meta_side);
		if (rv) {
			_NE(tpv_recovery_adopt_fail,
			    "TPV: @STR: failed to adopt CDV_extent[@LLU] (meta=@INT) rv=@INT",
			    tpv->tpv_name, eidx, (int)is_meta_side, rv);
			continue;
		}
		n_orphans++;
	}

	kvfree(toma_indices);

	if (n_orphans > 0)
		_NI(tpv_recovery_done,
		    "TPV: @STR: adopted @LLU orphaned CDV_extents (meta=@INT); free pool now @LLU slots",
		    tpv->tpv_name, n_orphans, (int)is_meta_side, alloc->free_tpv_extent_count);
	else
		_ND(tpv_recovery_clean,
		    "TPV: @STR: no orphans (meta=@INT)", tpv->tpv_name, (int)is_meta_side);

	return 0;
}

int nvmeibc_tpv_recovery(struct nvmeibc_tpv *tpv)
{
	/*
	 * Recover data side (always). In split mode, also recover the
	 * metadata side - its extents host L1/L2 only but orphans still
	 * need adopting so the tree-write pool is replenished.
	 */
	(void)tpv_recovery_one_side(tpv, /*is_meta_side=*/false);
	if (nvmeibc_tpv_is_split(tpv))
		(void)tpv_recovery_one_side(tpv, /*is_meta_side=*/true);
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_recovery);
