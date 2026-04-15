/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_io.c — TPV IO dispatch: zero-read, write-allocate, DISCARD.
 *
 * Entry points:
 *   nvmeibc_tpv_make_request()     — block-layer make_request/submit_bio hook.
 *   nvmeibc_tpv_retry_pending_bios() — drains write-blocked bios after pool
 *                                      replenishment (called from work context).
 *
 * IO dispatch per operation:
 *
 *   READ/WRITE bios are split to single-extent granularity at the top of
 *   tpv_handle_one_bio.  On modern kernels (>= 5.9) with fops->submit_bio,
 *   the generic block layer does NOT enforce chunk_sectors — the driver must
 *   split bios itself.  DISCARD bios may span multiple extents and are
 *   handled by the DISCARD loop inside tpv_handle_one_bio.
 *
 *   READ  + unmapped → zero-fill pages and complete immediately.
 *   WRITE + unmapped → nvmeibc_tpv_alloc_extent():
 *                        0       → slot acquired; fall through to mapped path.
 *                       -EAGAIN  → park bio on pending_bios; CDV_extent
 *                                  pre-fetch is already scheduled.
 *                       other   → fail bio with the error code.
 *   READ/WRITE + mapped → forward to CDV at physical offset.
 *   DISCARD → free all extents covered by the bio; complete immediately.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_block.h"			/* KERNEL_SECTOR_SHIFT */
#include "common/nvmeib_common_os_block_api.h"	/* REQ_RET, REQ_RET_ZERO */

/* ── Bio-split bioset for extent-boundary splitting ─────────────────────── */

static struct bio_set tpv_split_bio_set;

int nvmeibc_tpv_io_init(void)
{
	return bioset_init(&tpv_split_bio_set, BIO_POOL_SIZE, 0,
			   BIOSET_NEED_BVECS);
}
EXPORT_SYMBOL(nvmeibc_tpv_io_init);

void nvmeibc_tpv_io_exit(void)
{
	bioset_exit(&tpv_split_bio_set);
}
EXPORT_SYMBOL(nvmeibc_tpv_io_exit);

/* ── Forward declarations ────────────────────────────────────────────────── */

/* Satisfy -Werror=missing-prototypes: REQ_RET is defined in the include above. */
REQ_RET nvmeibc_tpv_make_request(struct request_queue *q, struct bio *bio);

/*
 * nvmeibc_tpv_cdv_submit_bio — forward a mapped bio to the CDV IB transport.
 *
 * cdv_phys_offset is the CDV byte offset of the first byte of the bio.
 * Takes ownership of bio; bio is completed via bio_endio() when CDV IO done.
 *
 * Implemented in nvmeibc_tpv_cdv.c (CDV block-layer integration step).
 */
extern void nvmeibc_tpv_cdv_submit_bio(struct nvmeibc_tpv *tpv,
					struct bio *bio,
					u64 cdv_phys_offset);

/* ── Sector accessor (kernel-version-aware) ─────────────────────────────── */

/*
 * KS_BVEC_ITER — bio uses bi_iter.bi_sector (new kernels); else bi_sector.
 * The same flag is used by nvmeibc_block_dp_submit_bio_part.h for __GET_BI_SECTOR.
 */
static inline u64 tpv_bio_start_bytes(const struct bio *bio)
{
#if KS_BVEC_ITER
	return (u64)bio->bi_iter.bi_sector << KERNEL_SECTOR_SHIFT;
#else
	return (u64)bio->bi_sector << KERNEL_SECTOR_SHIFT;
#endif
}

/* ── DISCARD detection (kernel-version-aware) ───────────────────────────── */

/*
 * Matches the pattern in nvmeibc_block_api_os.c (lines 1745-1751) for
 * consistent DISCARD handling across all supported kernel versions.
 */
static inline bool tpv_bio_is_discard(const struct bio *bio)
{
#ifdef REQ_OP_BITS
	return bio_op(bio) == REQ_OP_DISCARD;
#elif defined(BIO_DISCARD)
	return !!(bio->bi_rw & BIO_DISCARD);
#else
	return !!(bio->bi_rw & REQ_DISCARD);
#endif
}

/* ── tpv_handle_one_bio — dispatch one extent-aligned bio ──────────────── */

/*
 * Pre-conditions:
 *   • READ/WRITE bios are fully contained within a single TPV_extent
 *     (enforced by blk_queue_chunk_sectors in the registration path).
 *   • DISCARD bios may span multiple extents (chunk_sectors does not
 *     constrain DISCARDs; the kernel uses max_discard_sectors instead).
 *   • tpv->state == TPV_ATTACHED (checked by caller).
 *
 * Returns  0        — bio dispatched or completed.
 * Returns -EAGAIN   — bio has been added to pending_bios; caller must not
 *                     touch bio after this return.
 * Returns other <0  — bio has been completed with the error code.
 */
static int tpv_handle_one_bio(struct nvmeibc_tpv *tpv, struct bio *bio)
{
	struct nvmeibc_tpv_allocator    *alloc = &tpv->allocator;
	u64 extent_bytes = (u64)alloc->tpv_extent_size_kb << 10;
	u64 virt_offset  = tpv_bio_start_bytes(bio);
	u64 virt_idx     = virt_offset / extent_bytes;
	u64 intra_offset = virt_offset % extent_bytes;
	struct nvmeibc_tpv_extent_entry *entry;
	bool is_write;
	u64  phys_off;
	int  rv;

	/*
	 * ── Extent-boundary split ────────────────────────────────────────
	 *
	 * On modern kernels (>= 5.9) the generic block layer does not
	 * enforce chunk_sectors for devices that provide fops->submit_bio,
	 * so a single READ/WRITE bio may span multiple TPV extents.  Split
	 * it here: carve off the head (up to the next extent boundary),
	 * resubmit the tail back to make_request (which re-enters this
	 * function for the next chunk), and fall through to single-extent
	 * handling for the head.
	 *
	 * DISCARDs are excluded — they have their own multi-extent loop.
	 */
	if (!tpv_bio_is_discard(bio)) {
		u64 extent_sectors = (u64)alloc->tpv_extent_size_kb << 1;
		unsigned int to_boundary = (unsigned int)
			(extent_sectors - (tpv_bio_start_bytes(bio) >>
					   KERNEL_SECTOR_SHIFT) % extent_sectors);

		if (bio_sectors(bio) > to_boundary) {
			struct bio *first;

			first = bio_split(bio, to_boundary,
					  GFP_NOIO, &tpv_split_bio_set);
			bio_chain(first, bio);

			/*
			 * Resubmit the remainder (bio) to our own
			 * make_request via the generic block layer.
			 * submit_bio_noacct / generic_make_request
			 * handles the recursion safely via per-task
			 * bio lists.
			 */
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
			generic_make_request(bio);
#else
			submit_bio_noacct(bio);
#endif
			bio = first;

			/* Recompute offsets for the (now trimmed) bio. */
			virt_offset  = tpv_bio_start_bytes(bio);
			virt_idx     = virt_offset / extent_bytes;
			intra_offset = virt_offset % extent_bytes;
		}
	}

	/* ── DISCARD — may span multiple extents ──────────────────────────── */
	if (tpv_bio_is_discard(bio)) {
		u64 discard_bytes = (u64)bio_sectors(bio) << KERNEL_SECTOR_SHIFT;
		u64 end_byte      = virt_offset + discard_bytes;
		u64 idx;

		for (idx = virt_idx; idx * extent_bytes < end_byte; idx++)
			nvmeibc_tpv_free_extent(tpv, idx);
		bio_endio(bio, 0);
		return 0;
	}

	is_write = (bio_data_dir(bio) == WRITE);

	/*
	 * Extent map lookup under RCU.  xa_load requires either xa_lock or
	 * rcu_read_lock; the entry is freed via kfree_rcu in free_extent,
	 * so dereferencing the returned pointer is safe within the RCU
	 * read-side critical section.
	 */
	rcu_read_lock();
	entry = xa_load(&alloc->extent_map, virt_idx);

	if (!entry) {
		rcu_read_unlock();

		/* ── READ on unmapped extent → zero-fill and complete ────── */
		if (!is_write) {
			zero_fill_bio(bio);
			bio_endio(bio, 0);
			return 0;
		}

		/* ── WRITE on unmapped extent → allocate physical slot ────── */
		rv = nvmeibc_tpv_alloc_extent(tpv, virt_idx, &entry);
		if (rv == -EAGAIN) {
			/*
			 * Free pool exhausted.  Park bio on pending_bios; it
			 * will be retried by nvmeibc_tpv_retry_pending_bios()
			 * once the CDV allocator work function replenishes the
			 * pool.  cdv_alloc_work is already scheduled by
			 * alloc_extent().
			 *
			 * Arm the timeout sweep if this is the first parked bio
			 * so bios don't wait forever when the CDV is full or
			 * the allocator TOMA is unreachable.
			 */
			unsigned long flags;

			spin_lock_irqsave(&tpv->pending_bio_lock, flags);
			if (bio_list_empty(&tpv->pending_bios))
				schedule_delayed_work(&tpv->timeout_work,
						      tpv->max_retry_jiffies);
			bio_list_add(&tpv->pending_bios, bio);
			spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);
			return -EAGAIN;
		}
		if (rv < 0) {
			bio_endio(bio, rv);
			return rv;
		}

		/*
		 * Freshly allocated entry — not yet visible to concurrent
		 * erasers, so direct access is safe without RCU.
		 *
		 * sync_flush mode: park the bio until persist_work has
		 * flushed the new L1 entry to CDV_extent[0].  persist_work
		 * was already scheduled by alloc_extent (dirty → true).
		 * The parked bio is re-dispatched by
		 * nvmeibc_tpv_forward_l1_flush_bios() after flush succeeds;
		 * at that point the extent_map lookup finds the mapping and
		 * the bio takes the normal mapped-IO path below.
		 */
		if (tpv->sync_flush) {
			unsigned long sflags;

			spin_lock_irqsave(&tpv->pending_bio_lock, sflags);
			bio_list_add(&tpv->pending_l1_flush_bios, bio);
			spin_unlock_irqrestore(&tpv->pending_bio_lock, sflags);
			return 0;
		}

		nvmeibc_tpv_cdv_submit_bio(tpv, bio,
					   entry->phys_offset + intra_offset);
		return 0;
	}

	/* ── Mapped READ or WRITE — snapshot offset under RCU ─────────────── */
	phys_off = entry->phys_offset + intra_offset;

	/*
	 * sync_flush: if this entry hasn't been persisted yet, park the bio
	 * so it doesn't reach CDV before flush_state writes the L1/L2 tree.
	 * Without this, a second thread hitting a just-allocated (but
	 * unpersisted) entry would bypass the sync_flush gate entirely.
	 */
	if (tpv->sync_flush && is_write && !READ_ONCE(entry->persisted)) {
		unsigned long sflags;

		rcu_read_unlock();
		spin_lock_irqsave(&tpv->pending_bio_lock, sflags);
		bio_list_add(&tpv->pending_l1_flush_bios, bio);
		spin_unlock_irqrestore(&tpv->pending_bio_lock, sflags);
		return 0;
	}

	rcu_read_unlock();

	nvmeibc_tpv_cdv_submit_bio(tpv, bio, phys_off);
	return 0;
}

/* ── nvmeibc_tpv_make_request — block-layer IO entry point ─────────────── */

/*
 * Called by the block layer for every bio targeting the TPV gendisk.
 *
 * blk_queue_chunk_sectors() ensures READ/WRITE bios are single-extent.
 * DISCARD bios may span multiple extents and are handled accordingly.
 *
 * On old kernels (KS_REQUEST_QUEUE_HAS_REQUEST_FN): registered via
 *   blk_queue_make_request(queue, nvmeibc_tpv_make_request).
 * On new kernels: called via nvmeibc_tpv_submit_bio_wrapper() in fops.
 */
REQ_RET nvmeibc_tpv_make_request(struct request_queue *q, struct bio *bio)
{
	struct nvmeibc_tpv *tpv = q->queuedata;

	if (unlikely(atomic_read(&tpv->state) != TPV_ATTACHED)) {
		bio_endio(bio, -EIO);
		return REQ_RET_ZERO;
	}

	/*
	 * Allocator state may still be loading from CDV_extent[0] in the
	 * background.  Park the bio until load_state_work completes.
	 *
	 * Double-checked locking: READ_ONCE avoids the lock in steady state
	 * (state_loaded is set once and never cleared).  The re-check under
	 * pending_bio_lock synchronises with load_state_work_fn which sets
	 * state_loaded under the same lock.
	 */
	if (unlikely(!READ_ONCE(tpv->state_loaded))) {
		unsigned long flags;

		spin_lock_irqsave(&tpv->pending_bio_lock, flags);
		if (!tpv->state_loaded) {
			if (bio_list_empty(&tpv->pending_bios))
				schedule_delayed_work(&tpv->timeout_work,
						      tpv->max_retry_jiffies);
			bio_list_add(&tpv->pending_bios, bio);
			spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);
			return REQ_RET_ZERO;
		}
		spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);
	}

	tpv_handle_one_bio(tpv, bio);
	return REQ_RET_ZERO;
}
EXPORT_SYMBOL(nvmeibc_tpv_make_request);

/* ── nvmeibc_tpv_retry_pending_bios — drain write-blocked bios ─────────── */

/*
 * Called from nvmeibc_tpv_cdv_alloc_work_fn() after tpv_on_cdv_alloc_ok()
 * succeeds — new TPV_extent slots are now in free_tpv_extents.
 *
 * Atomically swaps out the pending_bios list and re-dispatches each bio
 * through tpv_handle_one_bio().  Bios that return -EAGAIN again (rare: pool
 * refill was partial) are re-added to pending_bios by tpv_handle_one_bio().
 *
 * Runs in process context (work_struct); may sleep in tpv_handle_one_bio
 * via nvmeibc_tpv_cdv_submit_bio().
 */
void nvmeibc_tpv_retry_pending_bios(struct nvmeibc_tpv *tpv)
{
	struct bio_list  local;
	struct bio      *bio;
	unsigned long    flags;

	bio_list_init(&local);

	spin_lock_irqsave(&tpv->pending_bio_lock, flags);
	bio_list_merge(&local, &tpv->pending_bios);
	bio_list_init(&tpv->pending_bios);
	spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

	/*
	 * Cancel the timeout sweep — bios are being processed now.
	 * If tpv_handle_one_bio re-parks any of them (partial pool refill),
	 * the parking code re-schedules timeout_work.
	 */
	cancel_delayed_work(&tpv->timeout_work);

	while ((bio = bio_list_pop(&local)) != NULL)
		tpv_handle_one_bio(tpv, bio);
}
EXPORT_SYMBOL(nvmeibc_tpv_retry_pending_bios);

/* ── nvmeibc_tpv_forward_l1_flush_bios ─────────────────────────────────── */

/*
 * Drain bios parked on pending_l1_flush_bios after a successful L1 flush.
 * Each bio already has its virtual extent mapped in the xarray — the
 * re-dispatch through tpv_handle_one_bio() hits the "mapped" path and
 * forwards the bio to the CDV at the physical offset recorded earlier.
 *
 * Called from persist_work context (process context, may sleep).
 */
void nvmeibc_tpv_forward_l1_flush_bios(struct nvmeibc_tpv *tpv)
{
	struct bio_list  local;
	struct bio      *bio;
	unsigned long    flags;

	bio_list_init(&local);

	spin_lock_irqsave(&tpv->pending_bio_lock, flags);
	bio_list_merge(&local, &tpv->pending_l1_flush_bios);
	bio_list_init(&tpv->pending_l1_flush_bios);
	spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

	cancel_delayed_work(&tpv->timeout_work);

	while ((bio = bio_list_pop(&local)) != NULL)
		tpv_handle_one_bio(tpv, bio);
}
EXPORT_SYMBOL(nvmeibc_tpv_forward_l1_flush_bios);

/* ── nvmeibc_tpv_timeout_work_fn — fail parked bios after timeout ─────── */

/*
 * Fires after max_retry_jiffies from the moment the first bio was parked.
 * Fails all bios on pending_bios and pending_l1_flush_bios with -EIO.
 *
 * The timeout value depends on TPV state:
 *   - Attaching (state_loaded == false): 30 s  — CDV tree didn't load in time.
 *   - Attached (normal):        virtually infinite — CDV extent pool exhaustion
 *                                is transient; pool refill cancels this work.
 *   - Detaching:                10 ms — fast drain for graceful shutdown.
 */
void nvmeibc_tpv_timeout_work_fn(struct work_struct *work)
{
	struct nvmeibc_tpv *tpv = container_of(to_delayed_work(work),
					       struct nvmeibc_tpv, timeout_work);
	struct bio_list  expired;
	struct bio      *bio;
	unsigned long    flags;
	int              n = 0;

	bio_list_init(&expired);

	spin_lock_irqsave(&tpv->pending_bio_lock, flags);
	bio_list_merge(&expired, &tpv->pending_bios);
	bio_list_init(&tpv->pending_bios);
	bio_list_merge(&expired, &tpv->pending_l1_flush_bios);
	bio_list_init(&tpv->pending_l1_flush_bios);
	spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

	while ((bio = bio_list_pop(&expired)) != NULL) {
		bio_endio(bio, -EIO);
		n++;
	}

	if (n)
		_NW(tpv_bio_timeout, "TPV @STR: timed out @INT parked bios after @LLU ms",
		    tpv->tpv_name, n,
		    (u64)tpv->max_retry_jiffies * 1000 / HZ);
}
EXPORT_SYMBOL(nvmeibc_tpv_timeout_work_fn);
