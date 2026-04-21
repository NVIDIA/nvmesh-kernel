/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_allocator.c — TPV.allocator: xarray extent map, CDV_extent
 * alloc/free, and low-watermark CDV_extent pre-fetch from TOMA.
 *
 * Responsibilities:
 *   • nvmeibc_tpv_alloc_extent()      — pop one free physical TPV_extent slot,
 *     install it in the xarray, trigger CDV_extent pre-fetch if below watermark.
 *   • nvmeibc_tpv_free_extent()       — erase from xarray, return slot to free
 *     list; when a CDV_extent becomes empty move it to pending_return_list and
 *     schedule cdv_alloc_work (which drains the return list before allocating).
 *   • nvmeibc_tpv_cdv_alloc_work_fn() — background work: return any pending
 *     empty CDV_extents via NVMEIBC_MA_CDV_FREE_EXTENT, then (if still below
 *     watermark) send NVMEIBC_MA_CDV_ALLOC_EXTENT to the TOMA allocator.
 *   • nvmeibc_tpv_free_slots_list()   — called by nvmeibc_tpv.c at detach to
 *     free all free_tpv_extents list entries.
 *
 * Locking order:
 *   allocator_id_lock  (irqsave)   — read toma_id / generation snapshot
 *   allocator.lock     (spin)      — protect extent_map, cdv_extent_list,
 *                                    free_tpv_extents, pending_return_list
 *   persist_lock       (spin)      — set dirty, schedule persist_work;
 *                                    always taken AFTER releasing allocator.lock
 *
 * CDV geometry (from nvmeibc_tpv_allocator fields):
 *   A = allocator_size_gib × 1 GiB — byte offset of first data CDV_extent
 *   E = cdv_extent_size_mib × 1 MiB — size of one data CDV_extent
 *   T = tpv_extent_size_kb × 1 KiB — size of one TPV_extent (= one slot)
 *   n_slots = E / T                  — TPV_extents per CDV_extent
 *
 * Data CDV_extent[i] occupies CDV bytes [A + i×E, A + (i+1)×E).
 * Slot s within CDV_extent[i] starts at CDV byte A + i×E + s×T.
 */

#include "common/kr_incs.h"		/* kernel headers, u64, spinlock, kzalloc, etc. */
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"	/* nvmeibc_volume, hdr.uuid */
#include "clnt/nvmeibc_msgs_shared.h"	/* nvmeibc_cdv_alloc_req/resp, nvmeibc_cdv_free_req */

/* struct nvmeibc_tpv_free_slot is now in nvmeibc_tpv.h (shared with persist) */

/* ── Geometry helpers ───────────────────────────────────────────────────── */

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

/* ── Forward declarations ───────────────────────────────────────────────────
 *
 * nvmeibc_ib_admin_cdv_alloc_extent() and nvmeibc_ib_admin_cdv_free_extent()
 * are implemented in nvmeibc_ib_admin_channel.c as part of the IB admin
 * channel additions for thin provisioning (§2.8).  They may block; both are
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
 * nvmeibc_tpv_install_data_extent() — install a newly allocated data
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

/* ── nvmeibc_tpv_free_slots_list ────────────────────────────────────────────
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
		kfree(slot);
	}
}
EXPORT_SYMBOL(nvmeibc_tpv_free_slots_list);

/* ── nvmeibc_tpv_alloc_extent ───────────────────────────────────────────────
 *
 * Pop one physical TPV_extent slot from free_tpv_extents, install an
 * extent_entry in the xarray at virt_idx, and return it to the caller.
 *
 * Returns  0         — *out is valid; caller maps the bio to entry->phys_offset.
 * Returns -EAGAIN    — free pool empty; cdv_alloc_work has been scheduled;
 *                      caller must queue the bio and retry when the work fn
 *                      replenishes free_tpv_extents.
 * Returns -ENOMEM    — kzalloc failed under GFP_ATOMIC; caller fails the bio.
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
	 * writes — two bios target the same unmapped extent concurrently.
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
		/* Pool empty — arm CDV_extent pre-fetch if not already pending. */
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
		spin_unlock(&alloc->lock);
		atomic64_inc(&alloc->stat_tpv_alloc_eagain);
		return -EAGAIN;
	}

	/* Pop the first available physical slot. */
	slot = list_first_entry(&alloc->free_tpv_extents,
				struct nvmeibc_tpv_free_slot, node);
	list_del(&slot->node);
	alloc->free_tpv_extent_count--;

	/* Increment allocated_count in the owning CDV_extent_ref. */
	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->extent_index == slot->cdv_extent_index) {
			ref->allocated_count++;
			break;
		}
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
				break;
			}
		}
		spin_unlock(&alloc->lock);
		atomic64_inc(&alloc->stat_tpv_alloc_enomem);
		return -ENOMEM;
	}

	entry->phys_offset      = slot->phys_offset;
	entry->cdv_extent_index = slot->cdv_extent_index;

	/*
	 * xa_store before kfree(slot) so that on failure we can restore state.
	 * xa_store with GFP_ATOMIC may fail under memory pressure; treat as
	 * transient — the IO path will retry.
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
	 * Partial-page flush hook (§3.4.3): mark the 4 KB page of the owning
	 * L2 table that holds this leaf as dirty.  No-op when the L2 ctx
	 * does not exist yet (first-ever leaf under this L1 idx — flush_state
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
	*out = entry;
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_alloc_extent);

/* ── nvmeibc_tpv_alloc_l2_slot ─────────────────────────────────────────────
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
	struct nvmeibc_tpv_allocator  *alloc = &tpv->allocator;
	struct nvmeibc_tpv_free_slot  *slot;
	struct nvmeibc_cdv_extent_ref *ref;

	spin_lock(&alloc->lock);

	if (list_empty(&alloc->free_tpv_extents)) {
		spin_unlock(&alloc->lock);
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
		return -EAGAIN;
	}

	slot = list_first_entry(&alloc->free_tpv_extents,
				struct nvmeibc_tpv_free_slot, node);
	list_del(&slot->node);
	alloc->free_tpv_extent_count--;

	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->extent_index == slot->cdv_extent_index) {
			ref->allocated_count++;
			ref->l2_slots++;
			break;
		}
	}

	*phys_offset_out = slot->phys_offset;
	spin_unlock(&alloc->lock);

	kfree(slot);
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_alloc_l2_slot);

/* ── nvmeibc_tpv_free_extent ────────────────────────────────────────────────
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
	struct nvmeibc_tpv_free_slot    *s, *stmp;
	bool schedule_work_flag = false;

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
		kfree_rcu(entry, rcu);

		/* Still update allocated_count so the CDV_extent isn't leaked. */
		spin_lock(&alloc->lock);
		list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
			if (ref->extent_index == lost_idx) {
				ref->allocated_count--;
				break;
			}
		}
		spin_unlock(&alloc->lock);
		return 0;	/* bio still completes; slot loss is non-fatal */
	}

	slot->phys_offset      = entry->phys_offset;
	slot->cdv_extent_index = entry->cdv_extent_index;
	INIT_LIST_HEAD(&slot->node);
	kfree_rcu(entry, rcu);

	spin_lock(&alloc->lock);

	list_add_tail(&slot->node, &alloc->free_tpv_extents);
	alloc->free_tpv_extent_count++;

	/* Decrement allocated_count of the owning CDV_extent_ref. */
	found_ref = NULL;
	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (ref->extent_index == slot->cdv_extent_index) {
			found_ref = ref;
			break;
		}
	}

	if (WARN_ON(!found_ref))
		goto out_unlock;

	found_ref->allocated_count--;

	/*
	 * Return the CDV_extent to TOMA when it holds no data or L2 slots
	 * and is not the L1 extent.  The L1 extent is pinned for the TPV
	 * lifetime because its slot 0 holds the L1 table.  An extent that
	 * still hosts L2 tables keeps allocated_count > 0 via the L2 count
	 * and is therefore implicitly retained.
	 */
	if (found_ref->allocated_count == 0 && !found_ref->is_l1_extent) {
		/*
		 * All TPV_extents within this CDV_extent are free.
		 * Remove all its slots from free_tpv_extents (they
		 * cannot be reused once the extent is returned) and
		 * move the ref to pending_return_list for deferred
		 * CDV_FREE_EXTENT processing.
		 */
		list_del(&found_ref->node);
		alloc->cdv_extents_count--;

		list_for_each_entry_safe(s, stmp,
					 &alloc->free_tpv_extents, node) {
			if (s->cdv_extent_index == found_ref->extent_index) {
				list_del(&s->node);
				alloc->free_tpv_extent_count--;
				kfree(s);
			}
		}

		list_add_tail(&found_ref->node, &alloc->pending_return_list);
		schedule_work_flag = true;
	}

out_unlock:
	spin_unlock(&alloc->lock);

	/*
	 * Partial-page flush hook (§3.4.3): the L2 leaf for this virt_idx
	 * must be zeroed on disk, so mark its page dirty.  No-op when the
	 * L2 ctx doesn't exist yet (nothing to flush).
	 */
	nvmeibc_tpv_mark_l2_leaf_dirty(tpv, virt_idx);

	/* Mark dirty: the tree leaf for this virt_idx must be cleared. */
	spin_lock(&tpv->persist_lock);
	if (!tpv->dirty) {
		tpv->dirty = true;
		schedule_work(&tpv->persist_work);
	}
	spin_unlock(&tpv->persist_lock);

	/* Defer CDV_FREE_EXTENT send to the work function. */
	if (schedule_work_flag && !atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_work(&tpv->cdv_alloc_work);

	atomic64_inc(&alloc->stat_tpv_free_ok);
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_free_extent);

/* ── tpv_drain_pending_returns ──────────────────────────────────────────────
 *
 * Called from nvmeibc_tpv_cdv_alloc_work_fn() (process context, may sleep).
 * Drains pending_return_list: for each empty CDV_extent, sends
 * NVMEIBC_MA_CDV_FREE_EXTENT to TOMA.  On failure, the extent stays in the
 * list for the next work invocation.
 */
static void tpv_drain_pending_returns(struct nvmeibc_tpv *tpv,
				      const char *toma_id)
{
	struct nvmeibc_tpv_allocator  *alloc = &tpv->allocator;
	struct nvmeibc_cdv_extent_ref *ref, *tmp;
	struct nvmeibc_cdv_free_req    freq;
	LIST_HEAD(to_send);
	int rv;

	/* Snapshot the list under lock; process outside. */
	spin_lock(&alloc->lock);
	list_splice_init(&alloc->pending_return_list, &to_send);
	spin_unlock(&alloc->lock);

	list_for_each_entry_safe(ref, tmp, &to_send, node) {
		memset(&freq, 0, sizeof(freq));
		strncpy(freq.tpv_uuid, tpv->tpv_uuid, sizeof(freq.tpv_uuid) - 1);
		strncpy(freq.cdv_uuid, tpv->cdv_vol->hdr.uuid,
			sizeof(freq.cdv_uuid) - 1);
		freq.extent_index = ref->extent_index;

		rv = nvmeibc_ib_admin_cdv_free_extent(tpv->cdv_vol, toma_id,
						      &freq);
		if (rv) {
			/*
			 * Send failed: put back in pending_return_list for retry.
			 * The extent is already not in the active cdv_extent_list
			 * and its slots are removed from free_tpv_extents, so it
			 * is effectively dead until it is successfully returned.
			 */
			_NW(tpv_cdv_free_fail,
			    "TPV: @STR: CDV_FREE_EXTENT extent=@LLU failed rv=@INT; will retry",
			    tpv->tpv_name, ref->extent_index, rv);
			spin_lock(&alloc->lock);
			list_add_tail(&ref->node, &alloc->pending_return_list);
			spin_unlock(&alloc->lock);
		} else {
			_ND(tpv_cdv_ext_returned, "TPV: @STR: CDV_extent[@LLU] returned to TOMA",
			    tpv->tpv_name, ref->extent_index);
			atomic64_inc(&tpv->allocator.stat_cdv_free_ok);
			list_del(&ref->node);
			kfree(ref);
		}
	}
}

/* ── tpv_on_cdv_alloc_ok ────────────────────────────────────────────────────
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
	 * Split-mode (TPV_MetadataCDV.md §6.1): in split mode the metadata
	 * allocator hosts the L1 extent and L2 tables — never data. The data
	 * allocator hosts only user-data slots, no L1/L2. So:
	 *   - Single-CDV mode: this allocator IS the L1 host → honour the
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
	 * the same physical location — silent data corruption.
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
		INIT_LIST_HEAD(&ref->node);

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
		list_add_tail(&ref->node, &alloc->cdv_extent_list);
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
	INIT_LIST_HEAD(&ref->node);

	/*
	 * Allocate all slot structs into a local batch list (no pointer array).
	 * This avoids a large contiguous allocation that would fail at extreme
	 * n_slots (e.g. 1M when CDV extent = 64 GB, TPV extent = 64 KB).
	 *
	 * GFP_NOIO: we are a storage driver — GFP_KERNEL can trigger writeback
	 * that re-enters this driver, causing deadlock.
	 */
	for (s = 0; s < n_slots; s++) {
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

	/* Splice everything into the allocator under the lock. */
	spin_lock(&alloc->lock);
	list_add_tail(&ref->node, &alloc->cdv_extent_list);
	alloc->cdv_extents_count++;
	list_splice_tail(&batch, &alloc->free_tpv_extents);
	alloc->free_tpv_extent_count += n_slots;
	spin_unlock(&alloc->lock);

	_NI(tpv_cdv_ext_ready,
	    "TPV: @STR: CDV_extent[@LLU] ready @LLU slots added (pool total: @LLU)",
	    tpv->tpv_name, extent_index, n_slots, alloc->free_tpv_extent_count);
	return 0;
}

/* ── nvmeibc_tpv_cdv_alloc_work_fn ─────────────────────────────────────────
 *
 * Background work item (process context, may sleep).
 *
 * Services one side (data CDV or — in split mode — metadata CDV) of the TPV.
 * The side is identified by comparing the work pointer against the two
 * embedded work structs. Split-mode TPVs have two parallel work items with
 * independent allocators, TOMA identities, and free pools; single-CDV TPVs
 * use only the data-side work item.
 *
 * Phase 1: Drain pending_return_list — return empty CDV_extents to TOMA via
 *   NVMEIBC_MA_CDV_FREE_EXTENT.
 *
 * Phase 2: If free_tpv_extent_count is still below low_watermark, send
 *   NVMEIBC_MA_CDV_ALLOC_EXTENT to request one new CDV_extent from TOMA.
 *
 * Error cases:
 *   WRONG_GEN — our allocator_generation is stale; a new allocator was
 *               elected.  The updated topology push will call
 *               nvmeibc_tpv_update_allocator_id(); the next alloc_extent()
 *               shortfall will re-arm this work.
 *   CDV_FULL  — TOMA will send CDVCapacityWarning to management; IOs blocked
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

	if (!list_empty(&alloc->pending_return_list))
		tpv_drain_pending_returns(tpv, toma_id);

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
		if (is_meta_side) {
			unsigned long iflags;

			spin_lock_irqsave(id_lock, iflags);
			*gen_src = resp.allocator_generation;
			spin_unlock_irqrestore(id_lock, iflags);
		} else {
			nvmeibc_tpv_update_allocator_id(tpv, toma_id, resp.allocator_generation);
		}
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
 * relative to the container_of-derived tpv) — only the function pointer
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
