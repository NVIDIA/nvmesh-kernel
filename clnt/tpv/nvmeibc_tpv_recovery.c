/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_recovery.c — TPV cold recovery: orphaned CDV_extent detection.
 *
 * Background
 * ──────────
 * The flat-L1 tree in CDV_extent[0] records only virtual extents that have
 * been mapped AND flushed.  TOMA's cdv_extent_md records every CDV_extent
 * allocated to a TPV regardless of whether any virtual extent has been
 * mapped within it.  A crash between CDV_ALLOC_OK and the first flush that
 * writes a leaf for a slot in the new CDV_extent therefore leaves an
 * "orphaned" CDV_extent: TOMA believes it belongs to this TPV, but the
 * flat-L1 tree has no reference to it, so load_state() never adds it to the
 * allocator's cdv_extent_list or free pool.
 *
 * Recovery
 * ────────
 * nvmeibc_tpv_recovery() is called unconditionally from
 * nvmeibc_tpv_attach() after nvmeibc_tpv_load_state() succeeds.  We
 * do not know what happened while the TPV was offline, so the TOMA
 * extent list must always be cross-checked — even for a volume whose
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
 * ──────────────
 * Recovery errors are non-fatal.  If the TOMA query fails or slot
 * allocations fail, recovery logs the problem and continues.  The allocator
 * runs with a reduced free pool; NVCK will reclaim any unrecovered orphans
 * on the next maintenance scan.
 *
 * Locking
 * ───────
 * No allocator.lock is taken: recovery runs at attach time, before IO gates
 * open and before work structs are scheduled, so the allocator is single-
 * threaded.  allocator_id_lock is taken only for the TOMA ID snapshot.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"	/* nvmeibc_volume, hdr.uuid */

/* ── Forward declaration ─────────────────────────────────────────────────── */

/*
 * nvmeibc_ib_admin_cdv_list_extents — query TOMA for the set of data
 * CDV_extents whose cdv_extent_md is DATA / tpv_uuid.
 *
 * On success, *out_indices is set to a vmalloc'd array of *out_count u64
 * extent indices; the caller must vfree(*out_indices).  On failure,
 * *out_indices is NULL and *out_count is 0.
 *
 * Returns 0 on success, negative errno on failure.
 * Blocks; called only from process context (attach / recovery path).
 *
 * Implemented in nvmeibc_ib_admin_channel.c (§2.8).
 */
extern int nvmeibc_ib_admin_cdv_list_extents(struct nvmeibc_volume *cdv,
					     const char *toma_id,
					     const char *tpv_uuid,
					     u64 **out_indices,
					     u64 *out_count);

/* ── Local geometry helpers ─────────────────────────────────────────────── */

/*
 * Mirror the geometry helpers in nvmeibc_tpv_allocator.c.  Kept local to
 * avoid coupling recovery to the allocator's static functions.
 */

static inline u64 recov_alloc_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return a->allocator_size_gb << 30;		/* A in bytes */
}

static inline u64 recov_extent_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->cdv_extent_size_mb << 20;	/* E in bytes */
}

static inline u64 recov_slot_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->tpv_extent_size_kb << 10;	/* T in bytes */
}

static inline u64 recov_slots_per_extent(const struct nvmeibc_tpv_allocator *a)
{
	return recov_extent_bytes(a) / recov_slot_bytes(a);
}

static inline u64 recov_slot_phys(const struct nvmeibc_tpv_allocator *a,
				   u64 extent_index, u64 slot)
{
	return recov_alloc_bytes(a) +
	       extent_index * recov_extent_bytes(a) +
	       slot * recov_slot_bytes(a);
}

/* ── tpv_recovery_is_known ──────────────────────────────────────────────────
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

/* ── tpv_recovery_adopt_orphan ──────────────────────────────────────────────
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
static int tpv_recovery_adopt_orphan(struct nvmeibc_tpv *tpv, u64 extent_index)
{
	struct nvmeibc_tpv_allocator  *alloc = &tpv->allocator;
	struct nvmeibc_cdv_extent_ref *ref;
	struct nvmeibc_tpv_free_slot  *fs, *fstmp;
	u64   n_slots = recov_slots_per_extent(alloc);
	u64   s;
	LIST_HEAD(batch);

	ref = kzalloc(sizeof(*ref), GFP_NOIO);
	if (!ref)
		return -ENOMEM;

	ref->extent_index    = extent_index;
	ref->allocated_count = 0;
	INIT_LIST_HEAD(&ref->node);

	for (s = 0; s < n_slots; s++) {
		fs = kzalloc(sizeof(*fs), GFP_NOIO);
		if (!fs) {
			list_for_each_entry_safe(fs, fstmp, &batch, node) {
				list_del(&fs->node);
				kfree(fs);
			}
			kfree(ref);
			return -ENOMEM;
		}
		fs->phys_offset      = recov_slot_phys(alloc, extent_index, s);
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
	alloc->free_tpv_extent_count += n_slots;

	return 0;
}

/* ── nvmeibc_tpv_recovery ───────────────────────────────────────────────────
 *
 * Cold recovery entry point.  See file header for full description.
 *
 * Returns 0 always.  Individual adoption failures are logged but do not
 * abort recovery of the remaining extents, and do not fail the attach.
 */
int nvmeibc_tpv_recovery(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	char   toma_id[NVMEIB_HOST_NAME_LEN];
	u64   *toma_indices = NULL;
	u64    toma_count   = 0;
	u64    n_orphans    = 0;
	unsigned long flags;
	u64    i;
	int    rv;

	/* ── 1. Snapshot the allocator TOMA identity ────────────────────────── */
	spin_lock_irqsave(&tpv->allocator_id_lock, flags);
	strncpy(toma_id, tpv->allocator_toma_id, sizeof(toma_id) - 1);
	toma_id[sizeof(toma_id) - 1] = '\0';
	spin_unlock_irqrestore(&tpv->allocator_id_lock, flags);

	if (toma_id[0] == '\0') {
		/*
		 * CDV topology has not yet pushed the allocator identity.
		 * The normal below-watermark CDV_extent request will eventually
		 * replenish the free pool.  Orphaned extents, if any, are left
		 * for NVCK to reclaim.
		 */
		_NW(tpv_recovery_no_toma,
		    "TPV: @STR: no allocator TOMA ID at recovery time; orphan check skipped",
		    tpv->tpv_name);
		return 0;
	}

	/* ── 2. Get TOMA extent list (prefer cached from load_state) ───────── */
	if (alloc->toma_extent_list && alloc->toma_extent_count > 0) {
		toma_indices = alloc->toma_extent_list;
		toma_count   = alloc->toma_extent_count;
		/* Transfer ownership: recovery will vfree. */
		alloc->toma_extent_list  = NULL;
		alloc->toma_extent_count = 0;
	} else {
		rv = nvmeibc_ib_admin_cdv_list_extents(tpv->cdv_vol, toma_id,
						       tpv->tpv_uuid,
						       &toma_indices,
						       &toma_count);
		if (rv) {
			_NE(tpv_recovery_list_fail,
			    "TPV: @STR: CDV_LIST_EXTENTS to @STR failed rv=@INT; orphan check skipped",
			    tpv->tpv_name, toma_id, rv);
			return 0;
		}
	}

	_NT(tpv_recovery_list_ok,
	    "TPV: @STR: TOMA reports @LLU data CDV_extents; tree has @LLU",
	    tpv->tpv_name, toma_count, alloc->cdv_extents_count);

	/* ── 3. Cross-reference and adopt orphans ───────────────────────────── */
	for (i = 0; i < toma_count; i++) {
		u64 eidx = toma_indices[i];
		bool known;

		/* Skip the tree extent — it is metadata, not a data orphan. */
		if (eidx == alloc->tree_extent_index) {
			_NT(tpv_recovery_skip_tree,
			    "TPV: @STR: skipping tree extent[@LLU]",
			    tpv->tpv_name, eidx);
			continue;
		}

		known = tpv_recovery_is_known(alloc, eidx);

		_NT(tpv_recovery_check_ext,
		    "TPV: @STR: recovery TOMA extent[@LLU] known=@INT",
		    tpv->tpv_name, eidx, (int)known);

		if (known)
			continue;

		/*
		 * Orphan: TOMA allocated this CDV_extent to us but there are
		 * no leaves for it in the L1/L2 tree.  Adopt it so its
		 * physical slots enter the free pool.
		 */
		_NI(tpv_recovery_orphan_found,
		    "TPV: @STR: CDV_extent[@LLU] orphaned; adopting",
		    tpv->tpv_name, eidx);

		rv = tpv_recovery_adopt_orphan(tpv, eidx);
		if (rv) {
			_NE(tpv_recovery_adopt_fail,
			    "TPV: @STR: failed to adopt CDV_extent[@LLU] rv=@INT; slots unavailable until NVCK",
			    tpv->tpv_name, eidx, rv);
			/* Non-fatal: try remaining extents. */
			continue;
		}
		n_orphans++;
	}

	vfree(toma_indices);

	/* ── 4. Summary ─────────────────────────────────────────────────────── */
	if (n_orphans > 0)
		_NI(tpv_recovery_done,
		    "TPV: @STR: adopted @LLU orphaned CDV_extents; free pool now @LLU slots",
		    tpv->tpv_name, n_orphans, alloc->free_tpv_extent_count);
	else
		_ND(tpv_recovery_clean,
		    "TPV: @STR: no orphans (TOMA: @LLU tree: @LLU CDV_extents)",
		    tpv->tpv_name, toma_count, alloc->cdv_extents_count);

	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_recovery);
