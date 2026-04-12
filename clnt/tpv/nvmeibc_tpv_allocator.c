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
 *   A = allocator_size_gb × 1 GiB — byte offset of first data CDV_extent
 *   E = cdv_extent_size_mb × 1 MiB — size of one data CDV_extent
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

/* ── Private type ───────────────────────────────────────────────────────────
 *
 * Tracks one available physical TPV_extent slot within an already-allocated
 * CDV_extent.  Lives on nvmeibc_tpv_allocator.free_tpv_extents.
 */
struct nvmeibc_tpv_free_slot {
	u64              phys_offset;		/* CDV byte offset of this slot */
	u64              cdv_extent_index;	/* CDV_extent containing this slot */
	struct list_head node;
};

/* ── Geometry helpers ───────────────────────────────────────────────────── */

static inline u64 tpv_alloc_area_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return a->allocator_size_gb << 30;		/* A in bytes */
}

static inline u64 tpv_cdv_extent_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->cdv_extent_size_mb << 20;	/* E in bytes */
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

/* Physical CDV byte offset of slot s within data CDV_extent[extent_index]. */
static inline u64 tpv_slot_phys_offset(const struct nvmeibc_tpv_allocator *a,
					u64 extent_index, u64 slot)
{
	return tpv_alloc_area_bytes(a) +
	       extent_index * tpv_cdv_extent_bytes(a) +
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
 * CDV_extent into the L2/L3 mapping tree (CDV_extent[0]) and flush the
 * modified pages to the CDV.  Called before slots are added to the free list
 * to maintain crash-consistency: if the client crashes after the tree write,
 * re-attach will re-populate free_tpv_extents from the tree; if it crashes
 * before, the extent remains orphaned (NVCK-detectable).
 *
 * Implemented in nvmeibc_tpv_persist.c (step 11d).  Returns 0 on success.
 */
extern int nvmeibc_tpv_install_data_extent(struct nvmeibc_tpv *tpv,
					   u64 extent_index);

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

	if (list_empty(&alloc->free_tpv_extents)) {
		/* Pool empty — arm CDV_extent pre-fetch if not already pending. */
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
		spin_unlock(&alloc->lock);
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
		return rv;
	}

	kfree(slot);	/* xa_store succeeded; slot info is now in the entry */

	/* Schedule CDV_extent pre-fetch if below watermark. */
	if (alloc->free_tpv_extent_count < alloc->low_watermark)
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_alloc = true;

	spin_unlock(&alloc->lock);

	if (schedule_alloc)
		schedule_work(&tpv->cdv_alloc_work);

	/* Mark dirty and schedule persistence outside allocator lock. */
	spin_lock(&tpv->persist_lock);
	if (!tpv->dirty) {
		tpv->dirty = true;
		schedule_work(&tpv->persist_work);
	}
	spin_unlock(&tpv->persist_lock);

	*out = entry;
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_alloc_extent);

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

		pr_warn_ratelimited("nvmeibc_tpv: %s: kzalloc failed in free_extent virt_idx=%llu; slot lost until recovery\n",
				    tpv->tpv_name, virt_idx);
		kfree(entry);

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
	kfree(entry);

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

	if (found_ref->allocated_count == 0) {
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

	/* Mark dirty: the tree leaf for this virt_idx must be cleared. */
	spin_lock(&tpv->persist_lock);
	if (!tpv->dirty) {
		tpv->dirty = true;
		schedule_work(&tpv->persist_work);
	}
	spin_unlock(&tpv->persist_lock);

	/* Defer CDV_FREE_EXTENT send to the work function. */
	if (schedule_work_flag)
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);

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
			pr_warn("nvmeibc_tpv: %s: CDV_FREE_EXTENT extent=%llu failed (%d); will retry\n",
				tpv->tpv_name, ref->extent_index, rv);
			spin_lock(&alloc->lock);
			list_add_tail(&ref->node, &alloc->pending_return_list);
			spin_unlock(&alloc->lock);
		} else {
			pr_debug("nvmeibc_tpv: %s: CDV_extent[%llu] returned to TOMA\n",
				 tpv->tpv_name, ref->extent_index);
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
static int tpv_on_cdv_alloc_ok(struct nvmeibc_tpv *tpv, u64 extent_index)
{
	struct nvmeibc_tpv_allocator  *alloc = &tpv->allocator;
	struct nvmeibc_cdv_extent_ref *ref;
	struct nvmeibc_tpv_free_slot  *fs, *fstmp;
	LIST_HEAD(batch);
	u64 n_slots;
	u64 s;
	int rv;

	n_slots = tpv_slots_per_cdv_extent(alloc);

	/*
	 * Install tree leaf and flush before making slots visible.
	 * Implemented in nvmeibc_tpv_persist.c (step 11d).
	 */
	rv = nvmeibc_tpv_install_data_extent(tpv, extent_index);
	if (rv) {
		pr_err("nvmeibc_tpv: %s: install_data_extent(%llu) failed (%d); CDV_extent orphaned until NVCK\n",
		       tpv->tpv_name, extent_index, rv);
		return rv;
	}

	ref = kzalloc(sizeof(*ref), GFP_NOIO);
	if (!ref)
		return -ENOMEM;

	ref->extent_index    = extent_index;
	ref->allocated_count = 0;
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
			list_for_each_entry_safe(fs, fstmp, &batch, node) {
				list_del(&fs->node);
				kfree(fs);
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

	pr_info("nvmeibc_tpv: %s: CDV_extent[%llu] ready, %llu slots added (pool total: %llu)\n",
		tpv->tpv_name, extent_index, n_slots,
		alloc->free_tpv_extent_count);
	return 0;
}

/* ── nvmeibc_tpv_cdv_alloc_work_fn ─────────────────────────────────────────
 *
 * Background work item (process context, may sleep).
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
void nvmeibc_tpv_cdv_alloc_work_fn(struct work_struct *work)
{
	struct nvmeibc_tpv           *tpv = container_of(work, struct nvmeibc_tpv,
							 cdv_alloc_work);
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_cdv_alloc_req  req;
	struct nvmeibc_cdv_alloc_resp resp;
	char     toma_id[NVMEIB_HOST_NAME_LEN];
	u64      client_gen;
	unsigned long flags;
	int      rv;

	if (atomic_read(&tpv->state) == TPV_DETACHING)
		goto out_clear_pending;

	/* Snapshot (toma_id, generation) atomically. */
	spin_lock_irqsave(&tpv->allocator_id_lock, flags);
	strncpy(toma_id, tpv->allocator_toma_id, sizeof(toma_id) - 1);
	toma_id[sizeof(toma_id) - 1] = '\0';
	client_gen = tpv->allocator_generation;
	spin_unlock_irqrestore(&tpv->allocator_id_lock, flags);

	if (toma_id[0] == '\0') {
		/*
		 * Allocator TOMA identity not yet known (topology push has not
		 * arrived).  Clear pending so alloc_extent() can re-arm later.
		 */
		pr_warn_ratelimited("nvmeibc_tpv: %s: no allocator TOMA ID yet; deferring CDV work\n",
				    tpv->tpv_name);
		goto out_clear_pending;
	}

	/* ── Phase 1: Return any empty CDV_extents ── */
	if (!list_empty(&alloc->pending_return_list))
		tpv_drain_pending_returns(tpv, toma_id);

	/* ── Phase 2: Request a new CDV_extent if still below watermark ── */
	if (alloc->free_tpv_extent_count >= alloc->low_watermark)
		goto out_clear_pending;

	memset(&req, 0, sizeof(req));
	strncpy(req.tpv_uuid, tpv->tpv_uuid, sizeof(req.tpv_uuid) - 1);
	strncpy(req.cdv_uuid, tpv->cdv_vol->hdr.uuid, sizeof(req.cdv_uuid) - 1);
	req.req_id            = (u64)atomic64_inc_return(&nvmeibc_tpv_req_id_counter);
	req.client_generation = client_gen;

	pr_debug("nvmeibc_tpv: %s: CDV_ALLOC_EXTENT → %s gen=%llu req_id=%llu\n",
		 tpv->tpv_name, toma_id, client_gen, req.req_id);

	memset(&resp, 0, sizeof(resp));
	rv = nvmeibc_ib_admin_cdv_alloc_extent(tpv->cdv_vol, toma_id, &req,
					       &resp);
	if (rv) {
		pr_err("nvmeibc_tpv: %s: CDV_ALLOC_EXTENT send error (%d)\n",
		       tpv->tpv_name, rv);
		goto out_clear_pending;
	}

	switch ((enum nvmeibc_cdv_alloc_status)resp.status) {
	case NVMEIBC_CDV_ALLOC_OK:
		rv = tpv_on_cdv_alloc_ok(tpv, resp.extent_index);
		if (rv)
			pr_err("nvmeibc_tpv: %s: tpv_on_cdv_alloc_ok(%llu) failed (%d)\n",
			       tpv->tpv_name, resp.extent_index, rv);
		break;

	case NVMEIBC_CDV_ALLOC_CDV_FULL:
		/*
		 * CDV is at capacity.  TOMA sends CDVCapacityWarning to
		 * management.  IOs needing allocation remain queued.
		 */
		pr_warn_ratelimited("nvmeibc_tpv: %s: CDV full; IOs blocked until CDV is extended\n",
				    tpv->tpv_name);
		break;

	case NVMEIBC_CDV_ALLOC_WRONG_GEN:
		/*
		 * Stale generation: a new allocator has been elected.  The
		 * updated CDV topology push will call
		 * nvmeibc_tpv_update_allocator_id(); the next alloc_extent()
		 * below-watermark event will re-schedule this work with the
		 * new generation.
		 */
		pr_info("nvmeibc_tpv: %s: WRONG_GEN (ours=%llu TOMA=%llu); awaiting topology update\n",
			tpv->tpv_name, client_gen, resp.allocator_generation);
		break;

	default:
		pr_err("nvmeibc_tpv: %s: CDV_ALLOC_EXTENT unexpected status=%u\n",
		       tpv->tpv_name, resp.status);
		break;
	}

out_clear_pending:
	atomic_set(&tpv->cdv_alloc_pending, 0);
}
EXPORT_SYMBOL(nvmeibc_tpv_cdv_alloc_work_fn);
