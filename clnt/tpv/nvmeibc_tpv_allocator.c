/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_allocator.c - TPV.allocator: xarray extent map, CDV_extent
 * alloc/free, and low-watermark CDV_extent pre-fetch from TOMA.
 *
 * Responsibilities:
 *   - nvmeibc_tpv_alloc_extent()      - pop one free physical TPV_extent slot,
 *     install it in the xarray, trigger CDV_extent pre-fetch if below watermark.
 *   - nvmeibc_tpv_free_extent()       - erase from xarray, return slot to free
 *     list; when a CDV_extent becomes empty move it to pending_return_list and
 *     schedule cdv_alloc_work (which drains the return list before allocating).
 *   - nvmeibc_tpv_cdv_alloc_work_fn() - background work: return any pending
 *     empty CDV_extents via NVMEIBC_MA_CDV_FREE_EXTENT, then (if still below
 *     watermark) send NVMEIBC_MA_CDV_ALLOC_EXTENT to the TOMA allocator.
 *   - nvmeibc_tpv_free_slots_list()   - called by nvmeibc_tpv.c at detach to
 *     free all free_tpv_extents list entries.
 *
 * Locking order:
 *   allocator_id_lock  (irqsave)   - read toma_id / generation snapshot
 *   allocator.lock     (spin)      - protect extent_map, cdv_extent_list,
 *                                    free_tpv_extents, pending_return_list
 *   persist_lock       (spin)      - set dirty, schedule persist_work;
 *                                    always taken AFTER releasing allocator.lock
 *
 * CDV geometry (from nvmeibc_tpv_allocator fields):
 *   A = allocator_size_gib x 1 GiB - byte offset of first data CDV_extent
 *   E = cdv_extent_size_mib x 1 MiB - size of one data CDV_extent
 *   T = tpv_extent_size_kb x 1 KiB - size of one TPV_extent (= one slot)
 *   n_slots = E / T                  - TPV_extents per CDV_extent
 *
 * Data CDV_extent[i] occupies CDV bytes [A + ixE, A + (i+1)xE).
 * Slot s within CDV_extent[i] starts at CDV byte A + ixE + sxT.
 */

#include "common/kr_incs.h"		/* kernel headers, u64, spinlock, kzalloc, etc. */
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"	/* nvmeibc_volume, hdr.uuid */
#include "clnt/nvmeibc_msgs_shared.h"	/* nvmeibc_cdv_alloc_req/resp, nvmeibc_cdv_free_req */

/* struct nvmeibc_tpv_free_slot is now in nvmeibc_tpv.h (shared with persist) */

/* -- Geometry helpers ----------------------------------------------------- */

static inline u64 tpv_alloc_area_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return a->allocator_size_gib << 30;		/* A in bytes */
}

static inline u64 tpv_cdv_extent_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->cdv_extent_size_mib << 20;	/* E in bytes */
}

static inline u64 tpv_extent_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->tpv_extent_size_kb << 10;	/* T in bytes */
}

/* Number of TPV_extent slots per data CDV_extent. */
static inline u64 tpv_slots_per_cdv_extent(const struct nvmeibc_tpv_allocator *a)
{
	return tpv_cdv_extent_bytes(a) / tpv_extent_bytes(a);
}

/*
 * Physical CDV byte offset of slot s within data extent extent_index.
 * Extent indices are 1-based: extent 1 starts at byte offset A.
 */
static inline u64 tpv_slot_phys_offset(const struct nvmeibc_tpv_allocator *a,
					u64 extent_index, u64 slot)
{
	return tpv_alloc_area_bytes(a) +
	       (extent_index - 1) * tpv_cdv_extent_bytes(a) +
	       slot * tpv_extent_bytes(a);
}

/* -- Forward declarations ---------------------------------------------------
 *
 * nvmeibc_ib_admin_cdv_alloc_extent() and nvmeibc_ib_admin_cdv_free_extent()
 * are implemented in nvmeibc_ib_admin_channel.c as part of the IB admin
 * channel additions for thin provisioning (S.2.8).  They may block; both are
 * called only from process context (work_struct handlers).
 */
extern int nvmeibc_ib_admin_cdv_alloc_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_alloc_req   *req,
	struct nvmeibc_cdv_alloc_resp        *resp);

extern int nvmeibc_ib_admin_cdv_free_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_free_req    *req);

/*
 * nvmeibc_tpv_install_data_extent() - install a newly allocated data
 * CDV_extent into the L2/L3 mapping tree (tree extent) and flush the
 * modified pages to the CDV.  Called before slots are added to the free list
 * to maintain crash-consistency: if the client crashes after the tree write,
 * re-attach will re-populate free_tpv_extents from the tree; if it crashes
 * before, the extent remains orphaned (NVCK-detectable).
 *
 * Implemented in nvmeibc_tpv_persist.c (step 11d).  Returns 0 on success.
 * Prototype in nvmeibc_tpv.h.
 */

/*
 * Monotonically increasing request ID across all CDV_ALLOC_EXTENT requests
 * from all TPVs on this client node.  Provides idempotency on the TOMA side
 * if the same request is retried after allocator failover.
 */
static atomic64_t nvmeibc_tpv_req_id_counter = ATOMIC64_INIT(0);

/* -- nvmeibc_tpv_free_slots_list --------------------------------------------
 *
 * Walk and free all nvmeibc_tpv_free_slot entries on a list.  Called from
 * nvmeibc_tpv.c:nvmeibc_tpv_allocator_free() at detach time after all IO
 * is quiesced.
 */
void nvmeibc_tpv_free_slots_list(struct list_head *free_tpv_extents)
{
	struct nvmeibc_tpv_free_slot *slot, *tmp;

	list_for_each_entry_safe(slot, tmp, free_tpv_extents, node) {
		list_del(&slot->node);
		/*
		 * Detach release: at this point IO is quiesced (called from
		 * nvmeibc_tpv_allocator_free at detach), so the drain ref is
		 * the only ref left on the entry.  Drop it directly rather
		 * than going through tpv_inflight_release to avoid a tpv*
		 * dependency in this list-only helper - no waiter is parked
		 * on inflight_drain_wq once IO has quiesced.
		 */
		if (slot->draining_entry &&
		    atomic_dec_and_test(&slot->draining_entry->inflight))
			kfree_rcu(slot->draining_entry, rcu);
		kfree(slot);
	}
}
EXPORT_SYMBOL(nvmeibc_tpv_free_slots_list);

/* -- sorted cdv_extent_list invariant --------------------------------------
 *
 * Density-aware allocator (TPV_Trimming.md follow-up).  cdv_extent_list is
 * maintained in ASCENDING order of allocated_count: head = sparsest, tail =
 * densest.  The allocator picks slots from the densest non-full extent so
 * writes pack tightly; sparse extents drain naturally to pending_return
 * and get reclaimed without a compaction pass.  Compaction itself also
 * walks this list directly, skipping its own sort.
 *
 * Maintenance is O(1) per ref->allocated_count +/-1 change: after the
 * mutation, call tpv_reposition_ref_locked(alloc, ref) while still
 * holding alloc->lock.  The ref swaps at most once per direction in
 * practice (the count changed by +/- 1, so at most one neighbor can be
 * out of order), but the helper loops defensively to tolerate callers
 * that batch multiple mutations before repositioning (e.g., the
 * flush-and-promote decrement by flushing_free_count).
 *
 * Refs that sit on pending_return_list are exempt (not in
 * cdv_extent_list); the caller should skip reposition in that case, or
 * set on_pending_return_list=true before the mutation.
 */
void nvmeibc_tpv_reposition_ref_locked(
	struct nvmeibc_tpv_allocator *alloc,
	struct nvmeibc_cdv_extent_ref *ref)
{
	if (ref->on_pending_return_list)
		return;

	/* Bubble forward (toward tail) while the next entry is LESS dense. */
	while (ref->node.next != &alloc->cdv_extent_list) {
		struct nvmeibc_cdv_extent_ref *next = list_entry(
			ref->node.next, struct nvmeibc_cdv_extent_ref, node);
		if (next->allocated_count >= ref->allocated_count)
			break;
		list_move(&ref->node, &next->node);
	}
	/* Bubble backward (toward head) while the prev entry is MORE dense. */
	while (ref->node.prev != &alloc->cdv_extent_list) {
		struct nvmeibc_cdv_extent_ref *prev = list_entry(
			ref->node.prev, struct nvmeibc_cdv_extent_ref, node);
		if (prev->allocated_count <= ref->allocated_count)
			break;
		list_move_tail(&ref->node, &prev->node);
	}
}
EXPORT_SYMBOL(nvmeibc_tpv_reposition_ref_locked);

/*
 * Insert a newly-created ref into cdv_extent_list at the position that
 * preserves the ascending-by-allocated_count invariant.  Used by
 * load_state / recovery / cdv_alloc_ok which build fresh refs with a
 * specific initial count.  For the count=0 (common) case, insertion
 * lands at the head.  For load_state's count=data_cnt[+l2_cnt], we
 * scan to find the right slot.  O(N) worst case; N = CDV extents held,
 * typically a handful.
 */
void nvmeibc_tpv_insert_ref_sorted_locked(
	struct nvmeibc_tpv_allocator *alloc,
	struct nvmeibc_cdv_extent_ref *ref)
{
	struct nvmeibc_cdv_extent_ref *cur;

	list_for_each_entry(cur, &alloc->cdv_extent_list, node) {
		if (cur->allocated_count > ref->allocated_count) {
			list_add_tail(&ref->node, &cur->node);
			return;
		}
	}
	list_add_tail(&ref->node, &alloc->cdv_extent_list);
}
EXPORT_SYMBOL(nvmeibc_tpv_insert_ref_sorted_locked);

/* -- nvmeibc_tpv_wastage_pct ------------------------------------------------
 *
 * Compute the online-compaction wastage signal for this TPV's data
 * allocator (design: TPV_Trimming.md Step 5 / Trigger and planner).
 *
 *   wastage = 1 - live_slots / (cdv_extents_count * n_slots_per_cdv_extent)
 *
 * live_slots is the sum of allocated_count across every ref on
 * cdv_extent_list.  Refs on pending_return_list are NOT counted on the
 * live side (they have no live mappings) but their cdv_extents are
 * also not counted in cdv_extents_count... wait, check:
 * cdv_extents_count tracks "CDV extents owned by this TPV from TOMA's
 * perspective" - incremented at CDV_ALLOC_OK, decremented only after a
 * successful CDV_FREE_EXTENT.  That is the number of extents the TPV
 * is paying the capacity cost for; compaction can reduce it by
 * emptying sparse extents and letting the drain fire.  So
 * cdv_extents_count is the right denominator.
 *
 * The metadata allocator (split-mode TPV) deliberately does not
 * contribute - online compaction is data-side only per S. Split-mode
 * scope.
 *
 * Walks cdv_extent_list under alloc->lock; N is small (tens to low
 * hundreds per TPV in practice), so this is cheap enough to call from
 * operator-driven /proc read paths and from the re-check trigger hooks
 * in free_extent.
 *
 * Returns 0..100.  If the TPV has no CDV extents yet (fresh attach,
 * no allocations), returns 0.
 */
u32 nvmeibc_tpv_wastage_pct(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_cdv_extent_ref *ref;
	u64 data_total = 0;
	u64 data_used  = 0;
	u64 n_slots;
	u32 pct;

	/*
	 * Wastage measures fragmentation that the *online compactor* can
	 * act on - and only that.  The compactor's plan_pick scans
	 * cdv_extent_list looking for:
	 *
	 *   SOURCE: a non-L1 ref whose data slots (allocated_count -
	 *           l2_slots) exceed pending_free_count - that is, a ref
	 *           that holds at least one live data slot the compactor
	 *           could move.
	 *   DEST:   a non-full ref that is not the L1 extent and has any
	 *           data-side capacity.
	 *
	 * Refs the compactor cannot use - L1 (always pinned), pending-
	 * return (already on their way back to TOMA), and refs whose
	 * entire allocated_count is L2 metadata - contribute neither
	 * to the unused-capacity numerator nor to the denominator.  This
	 * gives operationally useful semantics:
	 *
	 *   - Freshly-emptied TPV (only L1 + L2 metadata slots remain):
	 *     no data-bearing refs -> data_total == 0 -> wastage = 0.
	 *     The worker stays disarmed; nothing for it to compact.
	 *
	 *   - Workload churns slots evenly across owned extents:
	 *     wastage is the average per-extent slack, capturing the
	 *     real fragmentation the compactor reduces by relocating
	 *     sparse-extent slots onto denser extents.
	 *
	 *   - Drain-only steady state (e.g. post-blkdiscard):
	 *     all refs migrated to pending_return, no data-bearing
	 *     refs left -> wastage = 0.  Drain (separate from
	 *     compaction) handles those refs.
	 *
	 * Pending_free_count is subtracted from data_used because those
	 * slots have been logically freed (DISCARD path) but not yet
	 * promoted; they no longer hold live data and the compactor
	 * cannot move them either.
	 */
	spin_lock(&alloc->lock);
	n_slots = tpv_slots_per_cdv_extent(alloc);
	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		u64 ref_data_used;

		if (ref->is_l1_extent)
			continue;
		if (ref->allocated_count <= ref->l2_slots + ref->pending_free_count)
			continue;	/* no live data slots; not a compaction candidate */

		ref_data_used = ref->allocated_count - ref->l2_slots
			      - ref->pending_free_count;
		data_total += n_slots - ref->l2_slots;
		data_used  += ref_data_used;
	}
	spin_unlock(&alloc->lock);

	if (data_total == 0 || data_used >= data_total)
		return 0;

	pct = (u32)((100ULL * (data_total - data_used)) / data_total);
	if (pct > 100)
		pct = 100;
	return pct;
}
EXPORT_SYMBOL(nvmeibc_tpv_wastage_pct);

/* -- nvmeibc_tpv_alloc_extent -----------------------------------------------
 *
 * Pop one physical TPV_extent slot from free_tpv_extents, install an
 * extent_entry in the xarray at virt_idx, and return it to the caller.
 *
 * Returns  0         - *out is valid; caller maps the bio to entry->phys_offset.
 * Returns -EAGAIN    - free pool empty; cdv_alloc_work has been scheduled;
 *                      caller must queue the bio and retry when the work fn
 *                      replenishes free_tpv_extents.
 * Returns -ENOMEM    - kzalloc failed under GFP_ATOMIC; caller fails the bio.
 *
 * Called from IO context (nvmeibc_tpv_make_request); must not sleep.
 * Uses GFP_ATOMIC for all allocations.
 */
int nvmeibc_tpv_alloc_extent(struct nvmeibc_tpv *tpv, u64 virt_idx,
			     struct nvmeibc_tpv_extent_entry **out)
{
	struct nvmeibc_tpv_allocator    *alloc = &tpv->allocator;
	struct nvmeibc_tpv_free_slot    *slot;
	struct nvmeibc_cdv_extent_ref   *ref;
	struct nvmeibc_tpv_extent_entry *entry;
	bool schedule_alloc = false;
	int rv;

	spin_lock(&alloc->lock);

	/*
	 * Double-check: another bio for the same virt_idx may have raced
	 * past the caller's unlocked xa_load()==NULL and already allocated
	 * a slot.  This happens under iodepth>1 with overlapping random
	 * writes - two bios target the same unmapped extent concurrently.
	 *
	 * Without this check, xa_store below silently overwrites the first
	 * entry, leaking its physical slot and causing the first bio's data
	 * to become unreachable while the xarray points to a different slot.
	 */
	{
		struct nvmeibc_tpv_extent_entry *existing;

		existing = xa_load(&alloc->extent_map, virt_idx);
		if (existing) {
			spin_unlock(&alloc->lock);
			*out = existing;
			return 0;
		}
	}

	if (list_empty(&alloc->free_tpv_extents)) {
		/* Pool empty - arm CDV_extent pre-fetch if not already pending. */
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
		spin_unlock(&alloc->lock);
		atomic64_inc(&alloc->stat_tpv_alloc_eagain);
		return -EAGAIN;
	}

	/*
	 * Density-aware pick (TPV_Trimming.md follow-up).  Walk
	 * cdv_extent_list from tail (densest first); pick the first
	 * non-full ref that has a free slot in alloc->free_tpv_extents.
	 * This packs writes tightly so sparse extents drain naturally.
	 *
	 * Fallback (when no cdv_extent_list entry matches, e.g., the only
	 * free slots belong to a ref on pending_return_list whose return
	 * we're about to cancel): pop head of free_tpv_extents.  The
	 * ref-finding dance below handles both paths.
	 */
	slot = NULL;
	{
		struct nvmeibc_cdv_extent_ref *r;
		const u64 n_slots = tpv_slots_per_cdv_extent(alloc);

		list_for_each_entry_reverse(r, &alloc->cdv_extent_list, node) {
			struct nvmeibc_tpv_free_slot *s, *tmp;

			if (r->allocated_count >= n_slots)
				continue;
			list_for_each_entry_safe(s, tmp,
						 &alloc->free_tpv_extents, node) {
				if (s->cdv_extent_index != r->extent_index)
					continue;
				list_del(&s->node);
				alloc->free_tpv_extent_count--;
				slot = s;
				break;
			}
			if (slot)
				break;
		}
	}
	if (!slot) {
		/* Fallback: first slot in pool, regardless of density. */
		slot = list_first_entry(&alloc->free_tpv_extents,
					struct nvmeibc_tpv_free_slot, node);
		list_del(&slot->node);
		alloc->free_tpv_extent_count--;
	}

	/*
	 * Find the owning CDV_extent_ref.  It may live on either
	 * cdv_extent_list (the common case, including the one the
	 * density walk just picked) or pending_return_list (if a prior
	 * promote moved it there because its allocated_count hit zero).
	 * In the latter case, this new allocation "cancels" the pending
	 * return: flip the ref back to cdv_extent_list so the drain
	 * skips it.  Without this, the drain would send CDV_FREE_EXTENT
	 * and TOMA could reassign the extent to another TPV while a
	 * slot inside it is now live-mapped for this TPV --- a cross-TPV
	 * data-corruption bug.
	 */
	{
		struct nvmeibc_cdv_extent_ref *r;
		bool found = false;

		list_for_each_entry(r, &alloc->cdv_extent_list, node) {
			if (r->extent_index == slot->cdv_extent_index) {
				r->allocated_count++;
				nvmeibc_tpv_reposition_ref_locked(alloc, r);
				found = true;
				break;
			}
		}
		if (!found) {
			list_for_each_entry(r, &alloc->pending_return_list,
					    node) {
				if (r->extent_index !=
				    slot->cdv_extent_index)
					continue;
				/*
				 * Cancel the pending return.  cdv_extents_count
				 * is NOT incremented: the count was never
				 * decremented when this ref was moved to
				 * pending_return_list (see
				 * tpv_promote_flushing_frees_locked).  The
				 * count tracks "CDV extents owned by this TPV
				 * from TOMA's perspective" and decrements only
				 * in tpv_drain_pending_returns after a
				 * successful CDV_FREE_EXTENT send.
				 */
				r->on_pending_return_list = false;
				r->allocated_count++;
				/*
				 * Insert in sorted position - ref is fresh to
				 * cdv_extent_list; list_move_tail to end
				 * would break the density invariant.
				 */
				list_del(&r->node);
				nvmeibc_tpv_insert_ref_sorted_locked(alloc, r);
				atomic64_inc(&alloc->stat_cdv_returns_cancelled);
				found = true;
				break;
			}
		}
		WARN_ON(!found);
	}

	/* Allocate the xarray value (GFP_ATOMIC; called from IO context). */
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		/* Restore state and return -ENOMEM. */
		list_add(&slot->node, &alloc->free_tpv_extents);
		alloc->free_tpv_extent_count++;
		list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
			if (ref->extent_index == slot->cdv_extent_index) {
				ref->allocated_count--;
				nvmeibc_tpv_reposition_ref_locked(alloc, ref);
				break;
			}
		}
		spin_unlock(&alloc->lock);
		atomic64_inc(&alloc->stat_tpv_alloc_enomem);
		return -ENOMEM;
	}

	entry->phys_offset      = slot->phys_offset;
	entry->cdv_extent_index = slot->cdv_extent_index;
	entry->state            = NVMEIBC_TPV_ENTRY_NORMAL;
	/* inflight is a refcount.  Pre-charge 2:
	 *   +1  xarray link (dropped by xa_erase / xa_swap sites)
	 *   +1  caller's ref (the IO-path caller will either pass entry
	 *       to tpv_inflight_submit_bio, which consumes the ref, or
	 *       explicitly drop it via nvmeibc_tpv_drop_alloc_ref() on
	 *       the sync_flush park path).
	 *
	 * Why the caller ref must be taken here, before dropping
	 * alloc->lock: the alternative is to have the caller do
	 * atomic_add_unless(entry->inflight, 1, 0) after return, but
	 * that is a UAF.  Between our unlock and the caller's add_unless,
	 * a concurrent DISCARD can xa_erase the fresh entry, drop the
	 * xarray ref to 0, and kfree_rcu.  The caller is not under
	 * rcu_read_lock at that point so the grace period can elapse
	 * freely and the caller's add_unless dereferences freed memory.
	 * Taking the caller ref here, while we still hold alloc->lock
	 * (which any would-be xa_erase also needs), closes the window.
	 *
	 * If xa_store fails below we just kfree() - the two refs are
	 * atomic fields on memory that nothing else holds a reference
	 * to; no release cycle is needed.
	 */
	atomic_set(&entry->inflight, 2);

	/*
	 * xa_store before kfree(slot) so that on failure we can restore state.
	 * xa_store with GFP_ATOMIC may fail under memory pressure; treat as
	 * transient - the IO path will retry.
	 */
	rv = xa_err(xa_store(&alloc->extent_map, virt_idx, entry, GFP_ATOMIC));
	if (rv) {
		kfree(entry);
		/* Restore slot and counts. */
		list_add(&slot->node, &alloc->free_tpv_extents);
		alloc->free_tpv_extent_count++;
		list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
			if (ref->extent_index == slot->cdv_extent_index) {
				ref->allocated_count--;
				nvmeibc_tpv_reposition_ref_locked(alloc, ref);
				break;
			}
		}
		spin_unlock(&alloc->lock);
		atomic64_inc(&alloc->stat_tpv_alloc_enomem);
		return rv;
	}

	kfree(slot);	/* xa_store succeeded; slot info is now in the entry */

	/* Schedule CDV_extent pre-fetch if below watermark. */
	if (alloc->free_tpv_extent_count < alloc->low_watermark &&
	    !atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_alloc = true;

	spin_unlock(&alloc->lock);

	if (schedule_alloc)
		schedule_work(&tpv->cdv_alloc_work);

	/*
	 * Partial-page flush hook (S.3.4.3): mark the 4 KB page of the owning
	 * L2 table that holds this leaf as dirty.  No-op when the L2 ctx
	 * does not exist yet (first-ever leaf under this L1 idx - flush_state
	 * will create the ctx with all pages dirty).
	 */
	nvmeibc_tpv_mark_l2_leaf_dirty(tpv, virt_idx);

	/* Mark dirty and schedule persistence outside allocator lock. */
	spin_lock(&tpv->persist_lock);
	if (!tpv->dirty) {
		tpv->dirty = true;
		schedule_work(&tpv->persist_work);
	}
	spin_unlock(&tpv->persist_lock);

	atomic64_inc(&alloc->stat_tpv_alloc_ok);

	/*
	 * Online-compaction arm re-check on the alloc path (TPV_Trimming.md
	 * Step 5).  Wastage can rise across pure-write workloads (every alloc
	 * adds capacity to the denominator without commensurate live slots),
	 * so a TPV that disarmed at low wastage and then sees only writes
	 * would never re-arm without this hook - free_extent's recheck only
	 * fires on TRIMs and persist_work's recheck only fires when L1/L2
	 * are dirtied.  Gated by the same ONLINE_RECHECK_STRIDE counter as
	 * the free path so an alloc storm doesn't hammer maybe_arm's O(N)
	 * wastage walk.
	 */
	if (++tpv->online_wastage_recheck_counter >= ONLINE_RECHECK_STRIDE) {
		tpv->online_wastage_recheck_counter = 0;
		nvmeibc_tpv_online_maybe_arm(tpv);
	}

	if (out)
		*out = entry;
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_alloc_extent);

/* -- nvmeibc_tpv_alloc_l2_slot ---------------------------------------------
 *
 * Reserve one slot from the free pool for use as an L2 table.  Used by
 * flush_state when a new L1 index becomes non-null.  The slot is removed
 * from free_tpv_extents and its owning cdv_extent_ref's bookkeeping is
 * updated to reflect the L2 reservation (allocated_count + l2_slots both
 * increment, so the owning extent is pinned until the L2 is relocated or
 * the TPV is deleted).
 *
 * Returns 0 on success (*phys_offset_out set), -EAGAIN when the pool is
 * empty.  -EAGAIN is recoverable: flush_state rearms itself on the next
 * dirty mark once new free slots arrive from tpv_on_cdv_alloc_ok.
 */
int nvmeibc_tpv_alloc_l2_slot(struct nvmeibc_tpv *tpv, u64 *phys_offset_out)
{
	/*
	 * L2 tables live on the side that hosts the tree. In split mode
	 * that's the metadata CDV; in single-CDV mode nvmeibc_tpv_meta_alloc
	 * returns the data allocator so this degrades to the pre-split path.
	 * On -EAGAIN we re-arm the allocator whose free pool was empty -
	 * data-side cdv_alloc_work in single mode, meta-side in split mode.
	 */
	struct nvmeibc_tpv_allocator  *alloc = nvmeibc_tpv_meta_alloc(tpv);
	const bool is_split = nvmeibc_tpv_is_split(tpv);
	struct nvmeibc_tpv_free_slot  *slot;
	struct nvmeibc_cdv_extent_ref *ref;

	spin_lock(&alloc->lock);

	if (list_empty(&alloc->free_tpv_extents)) {
		spin_unlock(&alloc->lock);
		if (is_split) {
			if (!atomic_xchg(&tpv->meta_cdv_alloc_pending, 1))
				schedule_work(&tpv->meta_cdv_alloc_work);
		} else {
			if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
				schedule_work(&tpv->cdv_alloc_work);
		}
		return -EAGAIN;
	}

	/*
	 * L2-slot density-aware pick (TPV_Trimming.md follow-up).  Prefer
	 * the L1 extent when it still has room: co-locating L2 tables with
	 * the L1 table keeps metadata in one CDV_extent as long as possible,
	 * which in turn means fewer extents have l2_slots > 0 and more
	 * extents become reclaimable after data compaction.  If L1 is full
	 * (or in split mode, not applicable here), fall back to the densest
	 * non-full extent, same walk as nvmeibc_tpv_alloc_extent.
	 */
	slot = NULL;
	{
		struct nvmeibc_cdv_extent_ref *r;
		const u64 n_slots = (u64)((u64)alloc->cdv_extent_size_mib << 20) /
				    ((u64)alloc->tpv_extent_size_kb << 10);
		struct nvmeibc_tpv_free_slot *s, *tmp;

		/* Pass 1: L1 extent.  Only one per allocator (if any). */
		list_for_each_entry(r, &alloc->cdv_extent_list, node) {
			if (!r->is_l1_extent)
				continue;
			if (r->allocated_count >= n_slots)
				break;
			list_for_each_entry_safe(s, tmp,
						 &alloc->free_tpv_extents, node) {
				if (s->cdv_extent_index != r->extent_index)
					continue;
				list_del(&s->node);
				alloc->free_tpv_extent_count--;
				slot = s;
				break;
			}
			break;
		}
		/* Pass 2: densest non-full, any extent.  Same walk as
		 * alloc_extent. */
		if (!slot) {
			list_for_each_entry_reverse(r, &alloc->cdv_extent_list,
						    node) {
				if (r->allocated_count >= n_slots)
					continue;
				list_for_each_entry_safe(s, tmp,
							 &alloc->free_tpv_extents,
							 node) {
					if (s->cdv_extent_index != r->extent_index)
						continue;
					list_del(&s->node);
					alloc->free_tpv_extent_count--;
					slot = s;
					break;
				}
				if (slot)
					break;
			}
		}
	}
	if (!slot) {
		/* Fallback: first slot in pool. */
		slot = list_first_entry(&alloc->free_tpv_extents,
					struct nvmeibc_tpv_free_slot, node);
		list_del(&slot->node);
		alloc->free_tpv_extent_count--;
	}

	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->extent_index == slot->cdv_extent_index) {
			ref->allocated_count++;
			ref->l2_slots++;
			nvmeibc_tpv_reposition_ref_locked(alloc, ref);
			break;
		}
	}

	*phys_offset_out = slot->phys_offset;
	spin_unlock(&alloc->lock);

	kfree(slot);
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_alloc_l2_slot);

/* -- nvmeibc_tpv_free_extent ------------------------------------------------
 *
 * Erase the mapping for virt_idx from the xarray and return the physical slot
 * to free_tpv_extents.  If the owning CDV_extent has no more allocated
 * TPV_extents, it is moved to pending_return_list and cdv_alloc_work is
 * scheduled to return it to TOMA (avoiding a blocking send from IO context).
 *
 * Returns 0 on success, -ENOENT if virt_idx was not mapped.
 *
 * Called from IO context (DISCARD handler in nvmeibc_tpv_make_request);
 * must not block.  The NVMEIBC_MA_CDV_FREE_EXTENT send is deferred to
 * nvmeibc_tpv_cdv_alloc_work_fn().
 */
int nvmeibc_tpv_free_extent(struct nvmeibc_tpv *tpv, u64 virt_idx)
{
	struct nvmeibc_tpv_allocator    *alloc = &tpv->allocator;
	struct nvmeibc_tpv_extent_entry *entry;
	struct nvmeibc_tpv_free_slot    *slot;
	struct nvmeibc_cdv_extent_ref   *ref;
	struct nvmeibc_cdv_extent_ref   *found_ref;

	/*
	 * Online-compaction cancellation (TPV_Trimming.md Step 5 S. Correctness
	 * "Concurrent DISCARD"):
	 *
	 * If a relocation is in flight on virt_idx, we must cancel it before
	 * erasing the entry; otherwise the worker's post-CDV-write xa_store
	 * would install new_entry on a virt_idx we are about to erase, and
	 * the source slot's state would diverge.  cmpxchg the entry state
	 * from RELOCATING to RELOC_CANCELLED; the worker's step-8 commit
	 * cmpxchg will then fail and the worker will abort.  If the cmpxchg
	 * loses (state raced to COMMITTED), re-xa_load and cancel the NEW
	 * entry's relocation if any (there will not be one - the freshly
	 * swapped entry starts at NORMAL).
	 *
	 * Guest writes cancel identically via the IO-path cmpxchg in
	 * tpv_handle_one_bio; DISCARD here is the same mechanism on a
	 * different entry point.
	 */
retry_cancel:
	rcu_read_lock();
	entry = xa_load(&alloc->extent_map, virt_idx);
	if (entry) {
		u8 state = READ_ONCE(entry->state);

		if (state == NVMEIBC_TPV_ENTRY_COMMITTED) {
			rcu_read_unlock();
			cpu_relax();
			goto retry_cancel;
		}
		if (state == NVMEIBC_TPV_ENTRY_RELOCATING) {
			if (cmpxchg(&entry->state,
				    NVMEIBC_TPV_ENTRY_RELOCATING,
				    NVMEIBC_TPV_ENTRY_RELOC_CANCELLED) !=
			    NVMEIBC_TPV_ENTRY_RELOCATING) {
				rcu_read_unlock();
				goto retry_cancel;
			}
		}
	}
	rcu_read_unlock();

	entry = xa_erase(&alloc->extent_map, virt_idx);
	if (!entry)
		return -ENOENT;

	/*
	 * Allocate a free_slot to re-insert the physical region.  GFP_ATOMIC
	 * because we may be in IO context.  On failure the physical slot is
	 * not returned to the free pool, but we still decrement allocated_count
	 * so the owning CDV_extent can eventually be returned to TOMA.  The
	 * "dark" slot persists until the next TPV re-attach / recovery.
	 */
	slot = kzalloc(sizeof(*slot), GFP_ATOMIC);
	if (!slot) {
		u64 lost_idx = entry->cdv_extent_index;

		_NW(tpv_free_ext_alloc_fail,
		    "TPV: @STR: kzalloc failed in free_extent virt_idx=@LLU; slot lost until recovery",
		    tpv->tpv_name, virt_idx);
		/* Release the xarray's ref on entry.  If no bios are in flight
		 * (common case), inflight drops from 1 to 0 and we kfree_rcu
		 * now; otherwise the last in-flight bio's end_io will do it. */
		if (atomic_dec_and_test(&entry->inflight)) {
			wake_up_all(&tpv->inflight_drain_wq);
			kfree_rcu(entry, rcu);
		}

		/* Still update allocated_count so the CDV_extent isn't leaked. */
		spin_lock(&alloc->lock);
		list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
			if (ref->extent_index == lost_idx) {
				ref->allocated_count--;
				nvmeibc_tpv_reposition_ref_locked(alloc, ref);
				break;
			}
		}
		spin_unlock(&alloc->lock);
		return 0;	/* bio still completes; slot loss is non-fatal */
	}

	slot->phys_offset      = entry->phys_offset;
	slot->cdv_extent_index = entry->cdv_extent_index;
	INIT_LIST_HEAD(&slot->node);
	/*
	 * Transfer the xarray's inflight ref onto the parked slot as a "drain
	 * ref".  tpv_drain_flushing_entries (run from flush_and_promote after
	 * the L2 leaf is durable-null) waits for entry->inflight to reach
	 * this single ref before releasing it - blocking promotion until any
	 * guest bio that snapshotted entry->phys_offset before xa_erase has
	 * completed.  Closes the cross-slot-return window where a slow
	 * in-flight read could land on the slot's data after a concurrent
	 * alloc had reused it for a different virt_idx.
	 */
	slot->draining_entry = entry;

	spin_lock(&alloc->lock);

	/* Find the owning CDV_extent_ref. */
	found_ref = NULL;
	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->extent_index == slot->cdv_extent_index) {
			found_ref = ref;
			break;
		}
	}

	if (WARN_ON(!found_ref)) {
		/* Orphan slot - shouldn't happen.  Release the drain ref we
		 * just transferred onto it and free the slot. */
		spin_unlock(&alloc->lock);
		tpv_inflight_release(tpv, slot->draining_entry);
		slot->draining_entry = NULL;
		kfree(slot);
		spin_lock(&alloc->lock);
		goto out_unlock;
	}

	/*
	 * Park the slot on the owning ref's pending_free_slots list.
	 * Do NOT push to alloc->free_tpv_extents yet, and do NOT
	 * decrement allocated_count: this slot is still logically
	 * allocated from the CDV's perspective because the on-disk L2
	 * still references it.  A concurrent allocator must not be able
	 * to hand this slot out to a different virt_idx until the L2
	 * leaf is durable-null on CDV - otherwise a crash between the
	 * reuse and the flush leaves two virt_idx entries pointing at
	 * the same physical slot.
	 *
	 * The flush-completion path (nvmeibc_tpv_flush_and_promote)
	 * snapshots pending_free_slots, flushes L2, and then promotes
	 * the snapshotted slots to free_tpv_extents under alloc->lock.
	 * allocated_count is decremented and the ref moved to
	 * pending_return_list there, not here.
	 *
	 * See TPV_Trimming.md Step 2 "Persistence ordering" and Step 5
	 * "Deferred slot visibility" for the correctness argument.
	 */
	list_add_tail(&slot->node, &found_ref->pending_free_slots);
	found_ref->pending_free_count++;

out_unlock:
	spin_unlock(&alloc->lock);

	/*
	 * Partial-page flush hook (S.3.4.3): the L2 leaf for this virt_idx
	 * must be zeroed on disk, so mark its page dirty.  No-op when the
	 * L2 ctx doesn't exist yet (nothing to flush).
	 */
	nvmeibc_tpv_mark_l2_leaf_dirty(tpv, virt_idx);

	/*
	 * Mark dirty: persist_work_fn will flush the L2 and then promote
	 * this (and any other parked) slot to free_tpv_extents via
	 * nvmeibc_tpv_flush_and_promote(), which also handles the
	 * eventual CDV_FREE_EXTENT when a ref's allocated_count reaches
	 * zero.
	 */
	spin_lock(&tpv->persist_lock);
	if (!tpv->dirty) {
		tpv->dirty = true;
		schedule_work(&tpv->persist_work);
	}
	spin_unlock(&tpv->persist_lock);

	atomic64_inc(&alloc->stat_tpv_free_ok);

	/*
	 * Online-compaction arm re-check (TPV_Trimming.md Step 5).  Every
	 * ONLINE_RECHECK_STRIDE free_extent calls, re-read wastage and arm
	 * the worker if it crossed arm_high_pct.  Cheap O(N) walk inside
	 * maybe_arm; gated on the counter so a DISCARD storm doesn't
	 * hammer the allocator lock every 4 KB.
	 */
	if (++tpv->online_wastage_recheck_counter >= ONLINE_RECHECK_STRIDE) {
		tpv->online_wastage_recheck_counter = 0;
		nvmeibc_tpv_online_maybe_arm(tpv);
	}
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_free_extent);

/* -- nvmeibc_tpv_flush_and_promote ------------------------------------------
 *
 * Deferred-visibility commit path.  Implements the snapshot-before-
 * flush discipline described in Step 2 / Step 5 of TPV_Trimming.md:
 *
 *   1. Under alloc->lock, splice every ref's pending_free_slots into
 *      its flushing_free_slots.  Slots that land on pending_free_slots
 *      during the flush (step 3) stay parked for the next cycle.
 *   2. Release the lock.  flush_state writes any dirty L2 pages to
 *      CDV (may sleep).  The pages covering the snapshotted slots are
 *      dirty at entry (free_extent marked them) and, on success, are
 *      clean on disk at exit.
 *   3. On flush success: under alloc->lock, splice each ref's
 *      flushing_free_slots into alloc->free_tpv_extents and decrement
 *      allocated_count.  A ref whose count reaches zero (and is not
 *      the L1 extent) is moved to pending_return_list so cdv_alloc_work
 *      can send CDV_FREE_EXTENT.  Its slots are freed outright rather
 *      than spliced into the pool - the extent is going away.
 *   4. On flush failure: splice flushing_free_slots back onto
 *      pending_free_slots.  The next successful flush retries them.
 *
 * Returns the flush_state result (0 on success).  Called from the
 * persist_work_fn async path; exposed in the header for kernel
 * self-tests that need synchronous behavior.
 */

static void tpv_snapshot_pending_frees_locked(struct nvmeibc_tpv_allocator *alloc)
{
	struct nvmeibc_cdv_extent_ref *ref;

	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->pending_free_count == 0)
			continue;
		list_splice_tail_init(&ref->pending_free_slots,
				      &ref->flushing_free_slots);
		ref->flushing_free_count += ref->pending_free_count;
		ref->pending_free_count = 0;
	}
}

static void tpv_revert_flushing_frees_locked(struct nvmeibc_tpv_allocator *alloc)
{
	struct nvmeibc_cdv_extent_ref *ref;

	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->flushing_free_count == 0)
			continue;
		/*
		 * Prepend: the snapshotted slots were freed before any
		 * that may have been added during the failed flush.
		 */
		list_splice_init(&ref->flushing_free_slots,
				 &ref->pending_free_slots);
		ref->pending_free_count += ref->flushing_free_count;
		ref->flushing_free_count = 0;
	}
}

/*
 * Promote flushing_free_slots to the TPV-wide free pool.  Returns the
 * number of refs that transitioned to pending_return_list; caller
 * schedules cdv_alloc_work if > 0.
 */
static u32 tpv_promote_flushing_frees_locked(struct nvmeibc_tpv_allocator *alloc)
{
	struct nvmeibc_cdv_extent_ref *ref, *rtmp;
	u32 refs_returnable = 0;

	list_for_each_entry_safe(ref, rtmp, &alloc->cdv_extent_list, node) {
		if (ref->flushing_free_count == 0)
			continue;

		/*
		 * Splice snapshotted slots into the TPV-wide free pool
		 * unconditionally.  Even if this ref is about to be moved
		 * to pending_return_list (allocated_count about to hit 0),
		 * we leave its slots in the pool so they stay allocatable
		 * until the drain runs.  This gives the high_watermark
		 * gate something to work with (Step 2 Commit 2), and lets
		 * the alloc-cancels-return path reclaim the extent for a
		 * concurrent write without needing to re-allocate a whole
		 * CDV_extent from TOMA.  The drain path purges the slots
		 * at the moment CDV_FREE_EXTENT is actually about to be
		 * sent.
		 */
		list_splice_tail_init(&ref->flushing_free_slots,
				      &alloc->free_tpv_extents);
		alloc->free_tpv_extent_count += ref->flushing_free_count;
		ref->allocated_count         -= ref->flushing_free_count;
		ref->flushing_free_count      = 0;

		if (ref->allocated_count == 0 && !ref->is_l1_extent) {
			/*
			 * Move ref to pending_return_list but KEEP
			 * cdv_extents_count unchanged: the extent is still
			 * owned by this TPV from TOMA's perspective (no
			 * CDV_FREE_EXTENT sent yet).  The count decrements
			 * only in tpv_drain_pending_returns after a
			 * successful IB admin send, or if alloc-cancels-
			 * return leaves the ref on cdv_extent_list forever
			 * (in which case it never decrements, which is
			 * correct: the extent stays ours).
			 */
			list_move_tail(&ref->node, &alloc->pending_return_list);
			ref->on_pending_return_list = true;
			atomic64_inc(&alloc->stat_cdv_returns_queued);
			refs_returnable++;
		} else {
			/*
			 * Ref stays on cdv_extent_list; its allocated_count
			 * dropped by flushing_free_count, so its sort
			 * position almost certainly moved backward (toward
			 * sparser).  Reposition maintains the density-aware
			 * invariant.
			 */
			nvmeibc_tpv_reposition_ref_locked(alloc, ref);
		}
	}

	return refs_returnable;
}

/*
 * Drain bios that still hold refs on entries unlinked by free_extent
 * but whose slots are sitting on flushing_free_slots awaiting promotion.
 *
 * A guest read can hit tpv_handle_one_bio's mapped path, snapshot the
 * entry's phys_offset under rcu_read_lock, take an inflight ref, and
 * dispatch a bio to the CDV transport.  If the entry is then xa_erased
 * by free_extent and the slot promoted to free_tpv_extents before that
 * bio's CDV roundtrip completes, a concurrent alloc + write on a
 * different virt_idx can land on the same physical offset before the
 * read fires - producing a cross-slot data return.  See TPV_Trimming.md
 * Step 2 "Per-slot in-flight drain".
 *
 * free_extent transfers the xarray's inflight ref onto the parked slot
 * (slot->draining_entry).  This function waits for each such entry's
 * inflight to reach 1 (the drain ref alone), then releases that ref.
 * After return, every slot in any ref's flushing_free_slots has
 * draining_entry == NULL and can be safely promoted.
 *
 * Sleeps in synchronize_rcu and wait_event; must be called with no
 * locks held.
 */
static void tpv_drain_flushing_entries(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator    *alloc = &tpv->allocator;
	struct nvmeibc_cdv_extent_ref   *ref;
	struct nvmeibc_tpv_free_slot    *slot;
	struct nvmeibc_tpv_extent_entry *e;
	bool found;

	/*
	 * Pair with the rcu_read_lock around xa_load + atomic_add_unless
	 * in tpv_handle_one_bio.  After this returns, no CPU is still in an
	 * rcu critical section that observed the entry through the xarray
	 * (xa_erase happened in free_extent before the slot landed on the
	 * pending list, which itself happened before this flush cycle's
	 * snapshot).  Any reader that successfully bumped inflight has its
	 * bio in flight; end_io will dec the ref.  No new bumps can occur.
	 */
	synchronize_rcu();

	/*
	 * Snapshot-and-restart over flushing_free_slots: spin_lock only
	 * long enough to detach one (slot, entry) pair, drop the lock, wait
	 * for the entry to drain, release its drain ref, repeat.  Restarting
	 * from the head of cdv_extent_list each time keeps the iteration
	 * trivially safe against concurrent list mutation; the lists are
	 * short (a flush cycle covers slots for a handful of L2 pages) and
	 * the wait dominates wall time anyway.
	 */
	do {
		found = false;
		spin_lock(&alloc->lock);
		list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
			list_for_each_entry(slot, &ref->flushing_free_slots, node) {
				if (slot->draining_entry) {
					e = slot->draining_entry;
					slot->draining_entry = NULL;
					found = true;
					break;
				}
			}
			if (found)
				break;
		}
		spin_unlock(&alloc->lock);

		if (found) {
			wait_event(tpv->inflight_drain_wq,
				   atomic_read(&e->inflight) == 1);
			tpv_inflight_release(tpv, e);
		}
	} while (found);
}

int nvmeibc_tpv_flush_and_promote(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u32 refs_returnable = 0;
	int rv;

	spin_lock(&alloc->lock);
	tpv_snapshot_pending_frees_locked(alloc);
	spin_unlock(&alloc->lock);

	rv = nvmeibc_tpv_flush_state(tpv);

	if (rv == 0) {
		/*
		 * Drain in-flight reads/writes against the entries unlinked
		 * when these slots were freed.  Must run before promote to
		 * prevent cross-slot returns; see tpv_drain_flushing_entries.
		 */
		tpv_drain_flushing_entries(tpv);
	}

	spin_lock(&alloc->lock);
	if (rv == 0) {
		refs_returnable = tpv_promote_flushing_frees_locked(alloc);
		/*
		 * Also wake cdv_alloc_work if pending_return_list is
		 * non-empty, even when we didn't queue a new ref this
		 * cycle.  A previous drain may have parked refs at the
		 * high_watermark; this promotion grew the pool, so the
		 * gate may now be satisfied.  Without this re-arm, parked
		 * refs can sit indefinitely while activity flows through
		 * the free pool around them.
		 */
		if (!list_empty(&alloc->pending_return_list))
			refs_returnable = refs_returnable ? refs_returnable : 1;
	} else {
		tpv_revert_flushing_frees_locked(alloc);
	}
	spin_unlock(&alloc->lock);

	if (refs_returnable > 0 &&
	    !atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_work(&tpv->cdv_alloc_work);

	/*
	 * Re-evaluate the online-compaction arm condition.  This is the only
	 * point in the allocator where wastage can rise without a corresponding
	 * free_extent call: DISCARDs park slots on pending_free_slots without
	 * decrementing allocated_count, so wastage stays flat through the
	 * discard storm and the free_extent recheck counter never sees the
	 * post-discard wastage.  flush_and_promote is what materializes the
	 * change - promote moves slots to free_tpv_extents and drops
	 * allocated_count, which is where wastage_pct actually rises.  Without
	 * this maybe_arm here, a blkdiscard of a previously-full TPV leaves
	 * wastage at ~99% but the worker stays disarmed (state idle_below_arm)
	 * until something else - typically the next guest alloc_extent's own
	 * recheck - happens to fire.
	 *
	 * Cheap call: maybe_arm reads wastage and either bails (already armed,
	 * disabled, below threshold) or schedules a single delayed_work.
	 */
	if (rv == 0)
		nvmeibc_tpv_online_maybe_arm(tpv);

	return rv;
}
EXPORT_SYMBOL(nvmeibc_tpv_flush_and_promote);

/* -- nvmeibc_tpv_discard_range ---------------------------------------------
 *
 * Guest-issued DISCARD dispatch.  Releases every TPV_extent fully covered
 * by [start_byte, start_byte + len_bytes); skips any extent that the
 * range only partially overlaps at the head or tail.  See
 * design/TPV_Trimming.md Step 1 item 2 ("Reject / split misaligned
 * discards").
 *
 * Per-bio counting:
 *   stat_discard_ok: incremented per successful full-extent free.
 *   stat_discard_misaligned_skipped: incremented once if either endpoint
 *     is not extent-aligned, regardless of how many extents were dropped.
 *
 * Returns 0 unconditionally; individual free failures surface through
 * stat_tpv_free_ok (unchanged) but do not fail the caller.  The caller
 * completes the DISCARD bio with success either way - discard is
 * advisory at the block layer.
 */
int nvmeibc_tpv_discard_range(struct nvmeibc_tpv *tpv,
			      u64 start_byte, u64 len_bytes)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u64 extent_bytes = (u64)alloc->tpv_extent_size_kb << 10;
	u64 end_byte    = start_byte + len_bytes;
	u64 head_idx    = start_byte / extent_bytes;
	u64 end_idx     = end_byte   / extent_bytes;
	bool head_partial = (start_byte % extent_bytes) != 0;
	bool tail_partial = (end_byte   % extent_bytes) != 0;
	u64 first_full  = head_partial ? (head_idx + 1) : head_idx;
	u64 stop_full   = end_idx;  /* integer floor: partial tail already excluded */
	u64 idx;
	u64 freed = 0;

	if (len_bytes == 0)
		return 0;

	if (head_partial || tail_partial)
		atomic64_inc(&alloc->stat_discard_misaligned_skipped);

	for (idx = first_full; idx < stop_full; idx++) {
		if (nvmeibc_tpv_free_extent(tpv, idx) == 0) {
			atomic64_inc(&alloc->stat_discard_ok);
			freed++;
		}
		/* -ENOENT (already unmapped) is common and silent. */

		/*
		 * Yield every 256 iterations.  A whole-volume discard on a
		 * large virtual size produces millions of iterations and
		 * each free_extent takes alloc->lock + RCU work; without a
		 * yield the bio-submission task hogs the CPU long enough to
		 * trip the soft-lockup watchdog.  We are in process context
		 * (block submit_bio path) holding no locks at this point, so
		 * cond_resched() is safe here.  The 0xff mask amortises the
		 * already-cheap TIF_NEED_RESCHED check across iterations.
		 */
		if ((idx & 0xff) == 0xff)
			cond_resched();
	}

	_ND(tpv_discard_range,
	    "TPV: @STR: DISCARD start=@LLU len=@LLU freed=@LLU partial=@INT",
	    tpv->tpv_name, (unsigned long long)start_byte,
	    (unsigned long long)len_bytes, (unsigned long long)freed,
	    (head_partial || tail_partial) ? 1 : 0);

	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_discard_range);

/* -- tpv_drain_pending_returns ----------------------------------------------
 *
 * Called from nvmeibc_tpv_cdv_alloc_work_fn() (process context, may sleep).
 * Drains pending_return_list for the specified side: for each empty
 * CDV_extent, sends NVMEIBC_MA_CDV_FREE_EXTENT to TOMA.  On failure, the
 * extent stays in the list for the next work invocation.
 *
 * The side must match the caller's is_meta_side branch in
 * nvmeibc_tpv_cdv_alloc_work_run: each side has its own allocator,
 * pending_return_list, cdv_vol, and TOMA identity.
 */
static void tpv_drain_pending_returns(struct nvmeibc_tpv *tpv,
				      bool is_meta_side,
				      const char *toma_id)
{
	struct nvmeibc_tpv_allocator  *alloc;
	struct nvmeibc_volume         *cdv_vol;
	struct nvmeibc_cdv_extent_ref *ref, *tmp;
	struct nvmeibc_tpv_free_slot  *s, *stmp;
	struct nvmeibc_cdv_free_req    freq;
	u64 n_slots;
	LIST_HEAD(to_send);
	int rv;

	if (is_meta_side) {
		alloc   = tpv->meta_allocator;
		cdv_vol = tpv->meta_cdv_vol;
	} else {
		alloc   = &tpv->allocator;
		cdv_vol = tpv->cdv_vol;
	}
	n_slots = tpv_slots_per_cdv_extent(alloc);

	/*
	 * Under alloc->lock, walk pending_return_list head-to-tail.  For
	 * each ref, apply the Step 2 Commit 2 gates and prep:
	 *
	 *   1. Watermark gate.  Returning an extent drops the free pool by
	 *      its n_slots.  Refuse the return if the post-return pool
	 *      would fall below high_watermark.  All refs on this list own
	 *      the same-sized extents, so once one is refused the rest
	 *      are too - break.
	 *   2. Slot purge.  Until now the ref's slots have been sitting
	 *      in alloc->free_tpv_extents (kept there by
	 *      tpv_promote_flushing_frees_locked so the alloc-cancels-
	 *      return path could reclaim them).  Remove them now so that
	 *      once CDV_FREE_EXTENT lands and TOMA reassigns the extent,
	 *      we never hand one of those slots to a new guest write.
	 *      Also defensively drain ref->pending_free_slots (should be
	 *      empty since allocated_count is 0, but free-memory cost is
	 *      trivial and the invariant is clearer).
	 *   3. Clear on_pending_return_list and transfer to a local
	 *      to_send list.  Lock released, IB admin sends below.
	 */
	spin_lock(&alloc->lock);
	list_for_each_entry_safe(ref, tmp, &alloc->pending_return_list, node) {
		if (alloc->free_tpv_extent_count <
		    n_slots + alloc->low_watermark) {
			/*
			 * Watermark gate.  Stop returning when pool would fall
			 * below low_watermark - that's the "trigger pre-fetch"
			 * floor; subsequent guest writes that drain the pool
			 * further will fire cdv_alloc_work which fetches a
			 * fresh extent from TOMA on demand.
			 *
			 * The original design used high_watermark (2 *
			 * low_watermark) here to preserve a comfortable buffer
			 * for guest writes.  That floor is too conservative for
			 * post-blkdiscard scenarios: every owned extent ends up
			 * on pending_return_list, the first drain returns
			 * extents until the pool drops to high_watermark +
			 * n_slots, then breaks - and the remaining refs sit
			 * forever because no further persist activity grows the
			 * pool past the gate.  Using low_watermark lets drain
			 * fully reclaim a discarded TPV in one or two persist
			 * cycles; mixed workloads pay only an occasional extra
			 * CDV_ALLOC_EXTENT round-trip when allocs immediately
			 * follow a heavy free burst.
			 *
			 * No re-schedule here: same rationale as before - the
			 * pool only changes via flush_and_promote, which already
			 * schedules cdv_alloc_work when pending_return_list is
			 * non-empty (see nvmeibc_tpv_flush_and_promote).
			 */
			atomic64_inc(&alloc->stat_cdv_returns_parked);
			break;
		}

		/* Purge this extent's slots from the TPV-wide free pool. */
		list_for_each_entry_safe(s, stmp,
					 &alloc->free_tpv_extents, node) {
			if (s->cdv_extent_index != ref->extent_index)
				continue;
			list_del(&s->node);
			alloc->free_tpv_extent_count--;
			kfree(s);
		}
		/* Defensive: drain pending_free_slots on the ref. */
		list_for_each_entry_safe(s, stmp,
					 &ref->pending_free_slots, node) {
			list_del(&s->node);
			kfree(s);
		}
		ref->pending_free_count = 0;

		ref->on_pending_return_list = false;
		list_move_tail(&ref->node, &to_send);
	}
	spin_unlock(&alloc->lock);

	list_for_each_entry_safe(ref, tmp, &to_send, node) {
		memset(&freq, 0, sizeof(freq));
		strncpy(freq.tpv_uuid, tpv->tpv_uuid, sizeof(freq.tpv_uuid) - 1);
		strncpy(freq.cdv_uuid, cdv_vol->hdr.uuid,
			sizeof(freq.cdv_uuid) - 1);
		freq.extent_index = ref->extent_index;

		rv = nvmeibc_ib_admin_cdv_free_extent(cdv_vol, toma_id, &freq);
		if (rv) {
			/*
			 * Send failed.  The extent's slots have already been
			 * purged from alloc->free_tpv_extents above (so no
			 * stale slot can be re-allocated), which makes this
			 * extent effectively "owned by TOMA but invisible to
			 * the allocator" until the retry succeeds.  Put the
			 * ref back on pending_return_list (flag set again) so
			 * the next drain picks it up.
			 */
			_NW(tpv_cdv_free_fail,
			    "TPV: @STR: CDV_FREE_EXTENT extent=@LLU failed rv=@INT; will retry",
			    tpv->tpv_name, ref->extent_index, rv);
			spin_lock(&alloc->lock);
			ref->on_pending_return_list = true;
			list_add_tail(&ref->node, &alloc->pending_return_list);
			spin_unlock(&alloc->lock);
		} else {
			_ND(tpv_cdv_ext_returned,
			    "TPV: @STR: CDV_extent[@LLU] returned to TOMA",
			    tpv->tpv_name, ref->extent_index);
			atomic64_inc(&alloc->stat_cdv_free_ok);
			/*
			 * Decrement cdv_extents_count only now, on successful
			 * CDV_FREE_EXTENT send.  The count tracks TOMA's view
			 * of ownership, which changes only when TOMA
			 * acknowledges the return (by accepting the send).
			 */
			spin_lock(&alloc->lock);
			alloc->cdv_extents_count--;
			spin_unlock(&alloc->lock);
			list_del(&ref->node);
			kfree(ref);
		}
	}
}

/* -- tpv_on_cdv_alloc_ok ----------------------------------------------------
 *
 * Called from nvmeibc_tpv_cdv_alloc_work_fn() after a successful
 * NVMEIBC_MA_CDV_ALLOC_EXTENT response.
 *
 * 1. Install extent_index into the L2/L3 tree and flush (crash-safety: if we
 *    crash after this write, re-attach re-populates the free pool from the
 *    tree; if before, the extent is orphaned and NVCK-detectable).
 * 2. Build CDV_extent_ref and n_slots free_slot entries.
 * 3. Under lock, add ref to cdv_extent_list and slots to free_tpv_extents.
 *
 * Returns 0 on success.
 */
static int tpv_on_cdv_alloc_ok_for_side(struct nvmeibc_tpv *tpv, u64 extent_index,
					 bool is_meta_side)
{
	struct nvmeibc_tpv_allocator  *alloc = is_meta_side ? tpv->meta_allocator
							    : &tpv->allocator;
	struct nvmeibc_cdv_extent_ref *ref;
	struct nvmeibc_tpv_free_slot  *fs;
	LIST_HEAD(batch);
	u64 n_slots;
	u64 s;
	int rv;
	/*
	 * Split-mode (TPV_MetadataCDV.md S.6.1): in split mode the metadata
	 * allocator hosts the L1 extent and L2 tables - never data. The data
	 * allocator hosts only user-data slots, no L1/L2. So:
	 *   - Single-CDV mode: this allocator IS the L1 host -> honour the
	 *     "first extent becomes L1 extent" rule.
	 *   - Split mode, meta side: this allocator IS the L1 host.
	 *   - Split mode, data side: this allocator NEVER becomes the L1 host.
	 */
	bool this_side_hosts_l1 = !nvmeibc_tpv_is_split(tpv) || is_meta_side;

	_NT(tpv_cdv_alloc_ok_enter,
	    "TPV: @STR: tpv_on_cdv_alloc_ok extent_index=@LLU cdv_extents_count=@LLU free_slots=@LLU",
	    tpv->tpv_name, extent_index, alloc->cdv_extents_count,
	    alloc->free_tpv_extent_count);

	/*
	 * Guard against duplicate CDV_extent index.  After a TOMA restart
	 * where TOMA's persisted state lost track of some extents, TOMA may
	 * re-allocate an index that the client already holds from load_state.
	 * Adding duplicate slots would cause two virtual extents to map to
	 * the same physical location - silent data corruption.
	 */
	spin_lock(&alloc->lock);
	{
		struct nvmeibc_cdv_extent_ref *existing;

		list_for_each_entry(existing, &alloc->cdv_extent_list, node) {
			if (existing->extent_index == extent_index) {
				spin_unlock(&alloc->lock);
				_NE(tpv_cdv_ext_dup,
				    "TPV: @STR: CDV_extent[@LLU] already in allocator (allocated_count=@LLU); ignoring duplicate ALLOC_OK",
				    tpv->tpv_name, extent_index, existing->allocated_count);
				return 0;
			}
		}
	}
	spin_unlock(&alloc->lock);

	n_slots = tpv_slots_per_cdv_extent(alloc);

	/*
	 * If no L1 extent yet, this CDV_extent hosts the L1 table in slot 0.
	 * Its remaining slots (1..n_slots-1) enter the free pool as ordinary
	 * data/L2 candidates.  is_l1_extent pins the extent for the TPV
	 * lifetime (see free_extent logic) so slot 0 is never reclaimed.
	 *
	 * We skip install_data_extent here: the L1 table does not exist yet,
	 * so there is nothing to record.  flush_state will write the initial
	 * L1 on the first persist cycle.
	 */
	if (this_side_hosts_l1 && alloc->l1_extent_index == 0) {
		u64 first_free_slot = 1;	/* slot 0 reserved for L1 */

		alloc->l1_extent_index  = extent_index;
		alloc->n_l2_tables_used = 0;
		/*
		 * Fresh L1 extent: on-disk slot 0 is garbage.  Force the first
		 * flush to write the full L1 slot (header + all-null entries).
		 */
		nvmeibc_tpv_mark_l1_full_dirty(tpv);

		ref = kzalloc(sizeof(*ref), GFP_NOIO);
		if (!ref)
			return -ENOMEM;
		ref->extent_index    = extent_index;
		ref->allocated_count = 0;	/* slot 0 is pinned via is_l1_extent */
		ref->l2_slots        = 0;
		ref->is_l1_extent    = true;
		nvmeibc_cdv_extent_ref_init_lists(ref);

		for (s = first_free_slot; s < n_slots; s++) {
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
			fs->phys_offset      = tpv_slot_phys_offset(alloc, extent_index, s);
			fs->cdv_extent_index = extent_index;
			list_add_tail(&fs->node, &batch);
		}

		spin_lock(&alloc->lock);
		nvmeibc_tpv_insert_ref_sorted_locked(alloc, ref);
		alloc->cdv_extents_count++;
		list_splice_tail(&batch, &alloc->free_tpv_extents);
		alloc->free_tpv_extent_count += (n_slots - first_free_slot);
		spin_unlock(&alloc->lock);

		_NI(tpv_l1_extent_set,
		    "TPV: @STR: CDV_extent[@LLU] is the L1 extent (slot 0 pinned; @LLU data/L2 slots added)",
		    tpv->tpv_name, extent_index, n_slots - first_free_slot);
		return 0;
	}

	/*
	 * Install tree leaf and flush before making slots visible.
	 * Implemented in nvmeibc_tpv_persist.c (step 11d).
	 */
	rv = nvmeibc_tpv_install_data_extent(tpv, extent_index);
	if (rv) {
		_NE(tpv_install_ext_fail,
		    "TPV: @STR: install_data_extent(@LLU) failed rv=@INT; CDV_extent orphaned until NVCK",
		    tpv->tpv_name, extent_index, rv);
		return rv;
	}

	ref = kzalloc(sizeof(*ref), GFP_NOIO);
	if (!ref)
		return -ENOMEM;

	ref->extent_index    = extent_index;
	ref->allocated_count = 0;
	ref->l2_slots        = 0;
	ref->is_l1_extent    = false;
	nvmeibc_cdv_extent_ref_init_lists(ref);

	/*
	 * Allocate all slot structs into a local batch list (no pointer array).
	 * This avoids a large contiguous allocation that would fail at extreme
	 * n_slots (e.g. 1M when CDV extent = 64 GB, TPV extent = 64 KB).
	 *
	 * GFP_NOIO: we are a storage driver - GFP_KERNEL can trigger writeback
	 * that re-enters this driver, causing deadlock.
	 */
	/*
	 * Skip any slot whose physical byte offset would be 0.  That value is
	 * reserved as TPV_TREE_NULL ("unmapped") in on-disk L2 leaves; a data
	 * slot at phys_offset 0 would be silently dropped by load_state.
	 *
	 * When does phys_offset == 0 happen?  It happens iff the allocator
	 * area is empty (A = allocator_size_gib << 30 == 0) AND this is
	 * extent 1 AND slot 0.  In single-CDV mode A > 0 (the allocator
	 * region holds CDV metadata), so the collision cannot occur.  In
	 * split mode the data CDV carries no allocator region (A = 0), so
	 * slot 0 of data extent 1 computes to offset 0 -- triggered by the
	 * smoke test's virt_idx 0 cross-reboot loss.  Reserving that one slot
	 * costs at most a single TPV_extent (64 KiB default) per TPV, which
	 * is negligible.
	 *
	 * We generalise the check to "skip if phys_offset == 0" rather than
	 * hard-coding "split mode && extent 1 && slot 0" so the invariant is
	 * enforced structurally regardless of future layout changes.
	 */
	for (s = 0; s < n_slots; s++) {
		u64 phys = tpv_slot_phys_offset(alloc, extent_index, s);

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
		list_add_tail(&fs->node, &batch);
	}

	/* Splice everything into the allocator under the lock. */
	spin_lock(&alloc->lock);
	nvmeibc_tpv_insert_ref_sorted_locked(alloc, ref);
	alloc->cdv_extents_count++;
	list_splice_tail(&batch, &alloc->free_tpv_extents);
	alloc->free_tpv_extent_count += n_slots;
	spin_unlock(&alloc->lock);

	_NI(tpv_cdv_ext_ready,
	    "TPV: @STR: CDV_extent[@LLU] ready @LLU slots added (pool total: @LLU)",
	    tpv->tpv_name, extent_index, n_slots, alloc->free_tpv_extent_count);
	return 0;
}

/* -- nvmeibc_tpv_cdv_alloc_work_fn -----------------------------------------
 *
 * Background work item (process context, may sleep).
 *
 * Services one side (data CDV or - in split mode - metadata CDV) of the TPV.
 * The side is identified by comparing the work pointer against the two
 * embedded work structs. Split-mode TPVs have two parallel work items with
 * independent allocators, TOMA identities, and free pools; single-CDV TPVs
 * use only the data-side work item.
 *
 * Phase 1: Drain pending_return_list - return empty CDV_extents to TOMA via
 *   NVMEIBC_MA_CDV_FREE_EXTENT.
 *
 * Phase 2: If free_tpv_extent_count is still below low_watermark, send
 *   NVMEIBC_MA_CDV_ALLOC_EXTENT to request one new CDV_extent from TOMA.
 *
 * Error cases:
 *   WRONG_GEN - our allocator_generation is stale; a new allocator was
 *               elected.  The updated topology push will call
 *               nvmeibc_tpv_update_allocator_id(); the next alloc_extent()
 *               shortfall will re-arm this work.
 *   CDV_FULL  - TOMA will send CDVCapacityWarning to management; IOs blocked
 *               on allocation remain queued until a topology push signals
 *               available capacity.
 */
static void nvmeibc_tpv_cdv_alloc_work_run(struct nvmeibc_tpv *tpv,
					    bool is_meta_side)
{
	struct nvmeibc_tpv_allocator *alloc;
	struct nvmeibc_volume        *cdv_vol;
	spinlock_t                   *id_lock;
	const char                   *id_src;
	u64                          *gen_src;
	atomic_t                     *pending;
	struct work_struct           *self_work;
	struct nvmeibc_cdv_alloc_req  req;
	struct nvmeibc_cdv_alloc_resp resp;
	char     toma_id[NVMEIB_HOST_NAME_LEN];
	u64      client_gen;
	unsigned long flags;
	ktime_t  t_start;
	int      rv;
	bool     retry_pending = false;

	if (is_meta_side) {
		alloc    = tpv->meta_allocator;
		cdv_vol  = tpv->meta_cdv_vol;
		id_lock  = &tpv->meta_allocator_id_lock;
		id_src   = tpv->meta_allocator_toma_id;
		gen_src  = &tpv->meta_allocator_generation;
		pending  = &tpv->meta_cdv_alloc_pending;
		self_work = &tpv->meta_cdv_alloc_work;
	} else {
		alloc    = &tpv->allocator;
		cdv_vol  = tpv->cdv_vol;
		id_lock  = &tpv->allocator_id_lock;
		id_src   = tpv->allocator_toma_id;
		gen_src  = &tpv->allocator_generation;
		pending  = &tpv->cdv_alloc_pending;
		self_work = &tpv->cdv_alloc_work;
	}

	_NI(tpv_alloc_work_enter,
	    "TPV @STR: cdv_alloc_work[meta=@INT]: free=@LLU wm=@LLU cdv_extents=@LLU",
	    tpv->tpv_name, (int)is_meta_side, alloc->free_tpv_extent_count,
	    alloc->low_watermark, alloc->cdv_extents_count);

	if (atomic_read(&tpv->state) == TPV_DETACHING)
		goto out_clear_pending;

	if (!READ_ONCE(tpv->state_loaded))
		goto out_clear_pending;

	spin_lock_irqsave(id_lock, flags);
	strncpy(toma_id, id_src, sizeof(toma_id) - 1);
	toma_id[sizeof(toma_id) - 1] = '\0';
	client_gen = *gen_src;
	spin_unlock_irqrestore(id_lock, flags);

	if (toma_id[0] == '\0') {
		_NW(tpv_no_toma_id, "TPV: @STR: no allocator TOMA ID yet (meta=@INT); deferring CDV work",
		    tpv->tpv_name, (int)is_meta_side);
		goto out_clear_pending;
	}

	/*
	 * Skip the drain while a compaction run is in flight.  Compaction's
	 * planner (plan_sources in nvmeibc_tpv_compaction.c) caches raw
	 * nvmeibc_cdv_extent_ref pointers; tpv_drain_pending_returns kfrees
	 * refs after a successful CDV_FREE_EXTENT, which would turn those
	 * cached pointers into UAFs.  Compaction re-schedules this work
	 * once when it clears defer_drain, so the accumulated returns get
	 * processed promptly.
	 */
	if (!list_empty(&alloc->pending_return_list) &&
	    !tpv_compaction_drain_deferred(tpv))
		tpv_drain_pending_returns(tpv, is_meta_side, toma_id);

	if (alloc->free_tpv_extent_count >= alloc->low_watermark)
		goto out_clear_pending;

	memset(&req, 0, sizeof(req));
	strncpy(req.tpv_uuid, tpv->tpv_uuid, sizeof(req.tpv_uuid) - 1);
	strncpy(req.cdv_uuid, cdv_vol->hdr.uuid, sizeof(req.cdv_uuid) - 1);
	req.req_id            = (u64)atomic64_inc_return(&nvmeibc_tpv_req_id_counter);
	req.client_generation = client_gen;

	if (alloc->cdv_extent_size_mib > 0) {
		u64 cdv_bytes   = (u64)nvmeibc_volume_get_size(cdv_vol)
				  << NVMEIBC_SECTOR_SHIFT;
		req.total_data_extents = cdv_bytes / ((u64)alloc->cdv_extent_size_mib << 20);
	}

	_NT(tpv_cdv_alloc_req, "TPV: @STR: CDV_ALLOC_EXTENT[meta=@INT] to @STR gen=@LLU req_id=@LLU total_extents=@LLU free=@LLU wm=@LLU cdv_extents=@LLU",
	    tpv->tpv_name, (int)is_meta_side, toma_id, client_gen, req.req_id, req.total_data_extents,
	    alloc->free_tpv_extent_count, alloc->low_watermark, alloc->cdv_extents_count);

	memset(&resp, 0, sizeof(resp));
	t_start = ktime_get();
	rv = nvmeibc_ib_admin_cdv_alloc_extent(cdv_vol, toma_id, &req, &resp);
	if (rv) {
		atomic64_inc(&alloc->stat_cdv_alloc_err);
		_NE(tpv_cdv_alloc_send_fail, "TPV: @STR: CDV_ALLOC_EXTENT[meta=@INT] send error rv=@INT",
		    tpv->tpv_name, (int)is_meta_side, rv);
		goto out_clear_pending;
	}

	_NT(tpv_cdv_alloc_resp, "TPV: @STR: CDV_ALLOC_EXTENT[meta=@INT] resp status=@UINT extent_index=@LLU resp_gen=@LLU",
	    tpv->tpv_name, (int)is_meta_side, resp.status, resp.extent_index, resp.allocator_generation);

	switch ((enum nvmeibc_cdv_alloc_status)resp.status) {
	case NVMEIBC_CDV_ALLOC_OK:
		rv = tpv_on_cdv_alloc_ok_for_side(tpv, resp.extent_index, is_meta_side);
		if (rv)
			_NE(tpv_cdv_alloc_ok_fail,
			    "TPV: @STR: tpv_on_cdv_alloc_ok[meta=@INT](@LLU) failed rv=@INT",
			    tpv->tpv_name, (int)is_meta_side, resp.extent_index, rv);
		else {
			atomic64_add(ktime_to_ns(ktime_sub(ktime_get(), t_start)),
				     &alloc->stat_cdv_alloc_ns);
			atomic64_inc(&alloc->stat_cdv_alloc_ok);
			retry_pending = true;
		}
		break;

	case NVMEIBC_CDV_ALLOC_CDV_FULL:
		atomic64_inc(&alloc->stat_cdv_alloc_full);
		_NW(tpv_cdv_full, "TPV: @STR: CDV full[meta=@INT]; IOs blocked until CDV is extended",
		    tpv->tpv_name, (int)is_meta_side);
		break;

	case NVMEIBC_CDV_ALLOC_WRONG_GEN:
		atomic64_inc(&alloc->stat_cdv_alloc_wgen);
		_NI(tpv_cdv_wrong_gen,
		    "TPV: @STR: WRONG_GEN[meta=@INT] ours=@LLU TOMA=@LLU; updating generation and re-arming",
		    tpv->tpv_name, (int)is_meta_side, client_gen, resp.allocator_generation);
		/* toma_id is unchanged - the RAFT leader may move to a different
		 * TOMA via a management topology push, but WRONG_GEN only bumps
		 * the generation. Use the side-appropriate helper to update the
		 * cached generation under the right lock. */
		if (is_meta_side)
			nvmeibc_tpv_update_meta_allocator_id(tpv, toma_id, resp.allocator_generation);
		else
			nvmeibc_tpv_update_allocator_id(tpv, toma_id, resp.allocator_generation);
		if (atomic_cmpxchg(pending, 0, 1) == 0)
			schedule_work(self_work);
		break;

	default:
		_NE(tpv_cdv_alloc_bad_status,
		    "TPV: @STR: CDV_ALLOC_EXTENT unexpected status=@UINT (meta=@INT)",
		    tpv->tpv_name, resp.status, (int)is_meta_side);
		break;
	}

out_clear_pending:
	atomic_set(pending, 0);

	/*
	 * Retry pending bios AFTER clearing *_pending. Only the data side has
	 * parked bios waiting on data-extent allocation; metadata-side pool
	 * refills do not directly unblock IO (they unblock the persist path,
	 * which reschedules itself).
	 */
	if (retry_pending && !is_meta_side)
		nvmeibc_tpv_retry_pending_bios(tpv);
}

/*
 * Two distinct work entry points: the side is encoded in the function
 * pointer, not in runtime inspection of the work_struct address. Using a
 * single function for both work embeds would produce a tautological
 * container_of comparison (work always equals &tpv->cdv_alloc_work
 * relative to the container_of-derived tpv) - only the function pointer
 * distinguishes the two embedded work_structs reliably.
 */
void nvmeibc_tpv_cdv_alloc_work_fn(struct work_struct *work)
{
	struct nvmeibc_tpv *tpv = container_of(work, struct nvmeibc_tpv, cdv_alloc_work);

	nvmeibc_tpv_cdv_alloc_work_run(tpv, /*is_meta_side=*/false);
}
EXPORT_SYMBOL(nvmeibc_tpv_cdv_alloc_work_fn);

void nvmeibc_tpv_meta_cdv_alloc_work_fn(struct work_struct *work)
{
	struct nvmeibc_tpv *tpv = container_of(work, struct nvmeibc_tpv, meta_cdv_alloc_work);

	nvmeibc_tpv_cdv_alloc_work_run(tpv, /*is_meta_side=*/true);
}
EXPORT_SYMBOL(nvmeibc_tpv_meta_cdv_alloc_work_fn);
