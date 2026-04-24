/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

/*
 * nvmeibc_tpv_compaction.c - offline TPV compaction.
 *
 * See TPV_Trimming.md Step 4 for the full design.  High-level:
 *
 *   run(tpv, aggressiveness)
 *     pre-flight-check(free_tpv_extent_count <= high_watermark) -> no-op
 *     spawn L2 writer thread
 *     build planner (sort cdv_extent_refs by allocated_count)
 *     spawn N = aggressiveness worker threads
 *     workers loop: plan -> relocate_one -> report progress
 *     termination: watermark met or planner exhausted
 *     join workers, stop L2 writer thread
 *
 * Durability: each relocation's single synchronous L2 leaf write is the
 * sole commit point.  No journal, no orphan sweep.  See protocol sketch
 * in TPV_Trimming.md Step 4.
 *
 * Concurrency:
 *   - allocator->lock serialises ref list + free-pool mutations; held
 *     for short intervals only (pick slot, return slot).
 *   - plan_mutex serialises planner progression.
 *   - L2 writer thread handles all L2 page RMW; per-page serialisation
 *     via an in-flight list prevents lost updates when two relocations
 *     target the same L2 page.
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/xarray.h>

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "nvmeibc_tpv_compaction.h"

/* ==========================================================================
 * External hooks (declared in peer files).  See nvmeibc_tpv_persist.c /
 * nvmeibc_tpv_cdv.c / nvmeibc_tpv_allocator.c for definitions.
 * ==========================================================================
 */

/* Metadata CDV sync helpers (for L2 leaf RMW). */
extern int nvmeibc_tpv_cdv_sync_read(struct nvmeibc_tpv *tpv,
				     u64 cdv_offset, void *buf, u64 len);
extern int nvmeibc_tpv_cdv_sync_write(struct nvmeibc_tpv *tpv,
				      u64 cdv_offset, const void *buf, u64 len);
/* Data CDV sync helpers (for relocation source-read / dest-write). */
extern int nvmeibc_tpv_cdv_sync_read_data(struct nvmeibc_tpv *tpv,
					   u64 cdv_offset, void *buf, u64 len);
extern int nvmeibc_tpv_cdv_sync_write_data(struct nvmeibc_tpv *tpv,
					    u64 cdv_offset, const void *buf, u64 len);

/* The CDV byte offset of an L2 page for a given virt_idx.  Computed from
 * the TPV's L1 table (already loaded in memory via l1_to_l2_ctx).  The
 * persist code uses equivalent math; if the integration work moves this
 * into a shared helper, this stub should call it instead. */
static int compaction_l2_page_for_virt(struct nvmeibc_tpv *tpv,
				       u64 virt_idx,
				       u64 *out_page_phys,
				       u64 *out_page_offset_in_l2,
				       u64 *out_l2_page_idx,
				       u64 *out_l1_idx);

/* ==========================================================================
 * L2 writer thread.
 *
 * Requests arrive on job->l2_req_queue.  The thread pops a request, then
 * serialises against any in-flight request for the same L2 page by
 * waiting for a matching entry on job->l2_inflight_pages to clear.
 * This keeps different L2 pages fully parallel while ensuring at most
 * one RMW per page at a time.
 * ==========================================================================
 */

/*
 * Claim the page for this request.  Returns true once no other request
 * for the same page is in flight; the request is added to
 * l2_inflight_pages and the caller proceeds.  May sleep.
 */
static bool l2_writer_claim_page(struct tpv_compaction_job *job,
				 struct tpv_l2_write_req *req)
{
	DEFINE_WAIT(wait);
	unsigned long flags;
	bool claimed = false;

	for (;;) {
		struct tpv_l2_write_req *other;
		bool conflict = false;

		spin_lock_irqsave(&job->l2_req_lock, flags);
		list_for_each_entry(other, &job->l2_inflight_pages, inflight_node) {
			if (other->l2_page == req->l2_page &&
			    other != req) {
				conflict = true;
				break;
			}
		}
		if (!conflict) {
			list_add_tail(&req->inflight_node, &job->l2_inflight_pages);
			claimed = true;
		}
		spin_unlock_irqrestore(&job->l2_req_lock, flags);
		if (claimed)
			break;
		/* Another writer is busy on the same page; wait on the
		 * general req wait queue (re-signalled whenever a req
		 * completes).  A blocking mutex could also be used; we
		 * deliberately keep this spinlock + wait-queue pattern
		 * to stay uniform with the existing TPV code style. */
		prepare_to_wait(&job->l2_req_wq, &wait, TASK_UNINTERRUPTIBLE);
		/* Re-check under lock before sleeping */
		spin_lock_irqsave(&job->l2_req_lock, flags);
		conflict = false;
		list_for_each_entry(other, &job->l2_inflight_pages, inflight_node) {
			if (other->l2_page == req->l2_page &&
			    other != req) {
				conflict = true;
				break;
			}
		}
		spin_unlock_irqrestore(&job->l2_req_lock, flags);
		if (conflict)
			schedule();
		finish_wait(&job->l2_req_wq, &wait);
	}
	return claimed;
}

static void l2_writer_release_page(struct tpv_compaction_job *job,
				   struct tpv_l2_write_req *req)
{
	unsigned long flags;
	spin_lock_irqsave(&job->l2_req_lock, flags);
	list_del_init(&req->inflight_node);
	spin_unlock_irqrestore(&job->l2_req_lock, flags);
	wake_up_all(&job->l2_req_wq);
}

/*
 * Execute one leaf-change request.  Read-modify-write on the 4 KB L2
 * page via cdv_sync_read + cdv_sync_write.  Returns 0 on durable write,
 * <0 on I/O failure.
 */
static int l2_writer_do_one(struct nvmeibc_tpv *tpv,
			    struct tpv_l2_write_req *req)
{
	u64 page_phys = 0;
	u64 page_off_in_l2 = 0;
	u64 l2_page_idx = 0;
	u64 l1_idx = 0;
	u8 *page = NULL;
	int rv;
	u64 *leaves;
	u64 leaf_offset_in_page;

	rv = compaction_l2_page_for_virt(tpv, req->virt_idx,
					 &page_phys,
					 &page_off_in_l2,
					 &l2_page_idx,
					 &l1_idx);
	if (rv)
		return rv;
	req->l1_idx = l1_idx;
	req->l2_page = (l1_idx << 32) | l2_page_idx;

	page = kmalloc(4096, GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	rv = nvmeibc_tpv_cdv_sync_read(tpv, page_phys, page, 4096);
	if (rv)
		goto out;

	/* Each leaf is 8 bytes (struct tpv_tree_entry == u64 cdv_offset).
	 * page_off_in_l2 is the byte offset inside THIS 4 KB page. */
	leaves = (u64 *)page;
	leaf_offset_in_page = page_off_in_l2;
	leaves[leaf_offset_in_page / 8] = req->new_cdv_offset;

	rv = nvmeibc_tpv_cdv_sync_write(tpv, page_phys, page, 4096);

out:
	kfree(page);
	return rv;
}

static int l2_writer_thread_fn(void *arg)
{
	struct nvmeibc_tpv *tpv = arg;
	struct tpv_compaction_job *job = &tpv->compaction_job;
	unsigned long flags;

	while (!kthread_should_stop() && !READ_ONCE(job->l2_writer_should_stop)) {
		struct tpv_l2_write_req *req = NULL;

		spin_lock_irqsave(&job->l2_req_lock, flags);
		if (!list_empty(&job->l2_req_queue)) {
			req = list_first_entry(&job->l2_req_queue,
					       struct tpv_l2_write_req, q_node);
			list_del_init(&req->q_node);
		}
		spin_unlock_irqrestore(&job->l2_req_lock, flags);

		if (!req) {
			wait_event_interruptible_timeout(
				job->l2_req_wq,
				!list_empty(&job->l2_req_queue) ||
				READ_ONCE(job->l2_writer_should_stop),
				HZ);
			continue;
		}

		/* Compute the L2 page index (fills req->l2_page) first so
		 * conflict detection is meaningful.  Looking up the same
		 * twice is fine since the math is deterministic per
		 * virt_idx. */
		{
			u64 dummy_phys, dummy_off, pidx, l1i;
			int rv = compaction_l2_page_for_virt(tpv, req->virt_idx,
							     &dummy_phys, &dummy_off,
							     &pidx, &l1i);
			if (rv) {
				req->result = rv;
				complete(&req->done);
				continue;
			}
			req->l1_idx = l1i;
			req->l2_page = (l1i << 32) | pidx;
		}

		l2_writer_claim_page(job, req);
		req->result = l2_writer_do_one(tpv, req);
		l2_writer_release_page(job, req);
		complete(&req->done);
	}

	/* On shutdown, fail any pending requests so their waiters don't
	 * hang forever. */
	for (;;) {
		struct tpv_l2_write_req *req;
		spin_lock_irqsave(&job->l2_req_lock, flags);
		if (list_empty(&job->l2_req_queue)) {
			spin_unlock_irqrestore(&job->l2_req_lock, flags);
			break;
		}
		req = list_first_entry(&job->l2_req_queue,
				       struct tpv_l2_write_req, q_node);
		list_del_init(&req->q_node);
		spin_unlock_irqrestore(&job->l2_req_lock, flags);
		req->result = -EINTR;
		complete(&req->done);
	}
	return 0;
}

/* ==========================================================================
 * Public submit-and-wait helper.
 * ==========================================================================
 */

int tpv_l2_writer_submit_and_wait(struct nvmeibc_tpv *tpv,
				  u64 virt_idx,
				  u64 new_cdv_offset)
{
	struct tpv_compaction_job *job = &tpv->compaction_job;
	struct tpv_l2_write_req req;
	unsigned long flags;

	if (!job->l2_writer)
		return -ENOTSUPP;

	memset(&req, 0, sizeof(req));
	req.virt_idx = virt_idx;
	req.new_cdv_offset = new_cdv_offset;
	init_completion(&req.done);
	INIT_LIST_HEAD(&req.q_node);
	INIT_LIST_HEAD(&req.inflight_node);

	spin_lock_irqsave(&job->l2_req_lock, flags);
	list_add_tail(&req.q_node, &job->l2_req_queue);
	spin_unlock_irqrestore(&job->l2_req_lock, flags);
	wake_up(&job->l2_req_wq);

	wait_for_completion(&req.done);
	return req.result;
}
EXPORT_SYMBOL(tpv_l2_writer_submit_and_wait);

/* ==========================================================================
 * Relocation primitive.
 * ==========================================================================
 */

/*
 * Pick a free slot from dest_ref's in-memory free pool under
 * allocator->lock.  Returns the slot's physical offset and removes
 * it from the free pool.  Returns -ENOSPC if nothing free.
 *
 * INTEGRATION NOTE: this walks alloc->free_tpv_extents to find a slot
 * tagged with dest_ref's cdv_extent_index.  The existing allocator
 * keeps all free slots on a single list keyed by cdv_extent_index
 * in the struct nvmeibc_tpv_free_slot; we reuse that indexing.
 */
static int reloc_take_dest_slot(struct nvmeibc_tpv *tpv,
				struct nvmeibc_cdv_extent_ref *dest_ref,
				u64 *out_phys);

/*
 * Return the source slot to source_ref.  Decrements source_ref's
 * allocated_count and appends a free-slot entry for source_phys.
 * If allocated_count hits zero, the Step 2 machinery (existing
 * promote-to-pending_return_list path) picks it up.
 */
static void reloc_return_source_slot(struct nvmeibc_tpv *tpv,
				     struct nvmeibc_cdv_extent_ref *source_ref,
				     u64 source_phys);

/*
 * Find the xarray entry and source phys offset for virt_idx.  Returns
 * the entry (RCU-protected) or NULL if not mapped to source_ref.
 * Caller holds an RCU read lock.
 */
static struct nvmeibc_tpv_extent_entry *
reloc_lookup_in_source(struct nvmeibc_tpv *tpv,
		       u64 virt_idx,
		       struct nvmeibc_cdv_extent_ref *source_ref);

/*
 * Swap the xarray entry for virt_idx with a new entry pointing at
 * dest_phys.  Old entry is kfree_rcu'd.
 */
static int reloc_swap_xarray_entry(struct nvmeibc_tpv *tpv,
				   u64 virt_idx,
				   u64 dest_phys,
				   u64 dest_cdv_extent_index);

int tpv_reloc_one(struct nvmeibc_tpv *tpv,
		  u64 virt_idx,
		  struct nvmeibc_cdv_extent_ref *source_ref,
		  struct nvmeibc_cdv_extent_ref *dest_ref)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_extent_entry *entry;
	u64 source_phys = 0;
	u64 dest_phys = 0;
	u64 tpv_extent_bytes = (u64)alloc->tpv_extent_size_kb << 10;
	u8 *buf = NULL;
	int rv;

	/* -- 1. Pick dest slot -- */
	rv = reloc_take_dest_slot(tpv, dest_ref, &dest_phys);
	if (rv)
		return rv;

	/* -- 2. Look up virt_idx; must still be mapped to source_ref -- */
	rcu_read_lock();
	entry = reloc_lookup_in_source(tpv, virt_idx, source_ref);
	if (!entry) {
		rcu_read_unlock();
		/* Race with concurrent discard - return dest slot untouched. */
		reloc_return_source_slot(tpv, dest_ref, dest_phys);
		return -EAGAIN;
	}
	source_phys = entry->phys_offset;
	rcu_read_unlock();

	/* -- 3. Copy data source -> dest -- */
	buf = kvmalloc(tpv_extent_bytes, GFP_KERNEL);
	if (!buf) {
		reloc_return_source_slot(tpv, dest_ref, dest_phys);
		return -ENOMEM;
	}
	rv = nvmeibc_tpv_cdv_sync_read_data(tpv, source_phys, buf, tpv_extent_bytes);
	if (rv)
		goto out_err;
	rv = nvmeibc_tpv_cdv_sync_write_data(tpv, dest_phys, buf, tpv_extent_bytes);
	if (rv)
		goto out_err;

	/* -- 4. Rewrite L2 leaf via the L2 writer thread -- */
	rv = tpv_l2_writer_submit_and_wait(tpv, virt_idx, dest_phys);
	if (rv)
		goto out_err;

	/* L2 leaf durably points at dest; commit point crossed. */

	/* -- 5. Swap xarray entry (consumers now see dest) -- */
	rv = reloc_swap_xarray_entry(tpv, virt_idx, dest_phys,
				     dest_ref->extent_index);
	if (rv) {
		/* The mapping is already durable on disk; an xarray swap
		 * failure here is catastrophic.  Log and fall through;
		 * the next load_state() will recover from the on-disk
		 * (authoritative) view. */
		pr_err("tpv compaction: xarray swap failed virt_idx=%llu rv=%d\n",
		       virt_idx, rv);
	}

	/* -- 6. Return source slot to source_ref's free pool -- */
	reloc_return_source_slot(tpv, source_ref, source_phys);

	kvfree(buf);
	return 0;

out_err:
	reloc_return_source_slot(tpv, dest_ref, dest_phys);
	kvfree(buf);
	return rv;
}
EXPORT_SYMBOL(tpv_reloc_one);

/* ==========================================================================
 * init / destroy
 * ==========================================================================
 */

void tpv_compaction_init(struct nvmeibc_tpv *tpv)
{
	struct tpv_compaction_job *job = &tpv->compaction_job;

	atomic_set(&job->state, TPV_COMPACTION_IDLE);
	atomic_set(&job->abort_flag, 0);
	WRITE_ONCE(job->shutdown, false);
	atomic_set(&job->pending_starts, 0);
	init_waitqueue_head(&job->pending_starts_wq);
	WRITE_ONCE(job->defer_drain, false);
	atomic64_set(&job->relocated, 0);
	atomic64_set(&job->planned_relocations, 0);
	atomic64_set(&job->reclaimed_extents, 0);
	atomic64_set(&job->l2_relocated, 0);

	mutex_init(&job->plan_mutex);
	INIT_LIST_HEAD(&job->plan_sources);
	job->head_pn = NULL;
	job->tail_pn = NULL;

	atomic_set(&job->n_workers_running, 0);
	init_completion(&job->workers_done);

	job->l2_writer = NULL;
	INIT_LIST_HEAD(&job->l2_req_queue);
	spin_lock_init(&job->l2_req_lock);
	init_waitqueue_head(&job->l2_req_wq);
	job->l2_writer_should_stop = false;
	INIT_LIST_HEAD(&job->l2_inflight_pages);
}

void tpv_compaction_destroy(struct nvmeibc_tpv *tpv)
{
	struct tpv_compaction_job *job = &tpv->compaction_job;

	/*
	 * Set shutdown before anything else.  Subsequent tpv_compaction_run
	 * invocations will short-circuit; in-flight start-works will see the
	 * flag and skip; the /proc write path consults it before incrementing
	 * pending_starts.
	 *
	 * The Dekker-style handshake with the /proc write path
	 * (INC pending_starts -> smp_mb__after_atomic -> LOAD shutdown)
	 * requires a matching StoreLoad barrier on this side between the
	 * shutdown store and the pending_starts load.  WRITE_ONCE is relaxed
	 * and wait_event's outer optimistic check has no full barrier, so
	 * add smp_mb() explicitly here.  Without this, on weakly-ordered
	 * architectures a /proc writer could observe shutdown=0 after it has
	 * incremented pending_starts, yet we could read pending_starts=0
	 * and proceed to tear down state the writer is about to use.
	 */
	WRITE_ONCE(job->shutdown, true);
	smp_mb();

	/*
	 * Drain any /proc-triggered start-works that were queued but have
	 * not yet begun running.  Any already-running start-work either
	 * skipped due to shutdown, or is executing tpv_compaction_run and
	 * will be cancelled by the abort path below.
	 */
	wait_event(job->pending_starts_wq,
		   atomic_read(&job->pending_starts) == 0);

	/* If a run is in flight, request abort and wait. */
	if (atomic_read(&job->state) == TPV_COMPACTION_RUNNING ||
	    atomic_read(&job->state) == TPV_COMPACTION_STOPPING) {
		tpv_compaction_abort(tpv);
		wait_for_completion(&job->workers_done);
	}
	if (job->l2_writer) {
		WRITE_ONCE(job->l2_writer_should_stop, true);
		wake_up_all(&job->l2_req_wq);
		kthread_stop(job->l2_writer);
		job->l2_writer = NULL;
	}
	mutex_destroy(&job->plan_mutex);
}

/* ==========================================================================
 * Abort + progress snapshot
 * ==========================================================================
 */

void tpv_compaction_abort(struct nvmeibc_tpv *tpv)
{
	atomic_set(&tpv->compaction_job.abort_flag, 1);
	wake_up_all(&tpv->compaction_job.l2_req_wq);
}
EXPORT_SYMBOL(tpv_compaction_abort);

/*
 * Called from the TPV detach path before tpv_compaction_destroy.
 * Signals any in-flight run to stop; returns immediately (non-blocking).
 * tpv_compaction_destroy, called shortly after, waits for threads to exit.
 */
void nvmeibc_tpv_abort_any_compaction_for(struct nvmeibc_tpv *tpv)
{
	if (atomic_read(&tpv->compaction_job.state) == TPV_COMPACTION_RUNNING ||
	    atomic_read(&tpv->compaction_job.state) == TPV_COMPACTION_STOPPING) {
		_NI(tpv_compaction_detach_abort,
		    "TPV: @STR: detach requested; aborting in-flight compaction",
		    tpv->tpv_name);
		tpv_compaction_abort(tpv);
	}
}
EXPORT_SYMBOL(nvmeibc_tpv_abort_any_compaction_for);

bool tpv_compaction_drain_deferred(struct nvmeibc_tpv *tpv)
{
	return READ_ONCE(tpv->compaction_job.defer_drain);
}
EXPORT_SYMBOL(tpv_compaction_drain_deferred);

void tpv_compaction_get_progress(struct nvmeibc_tpv *tpv,
				 struct tpv_compaction_progress *out)
{
	struct tpv_compaction_job *job = &tpv->compaction_job;
	out->relocated           = atomic64_read(&job->relocated);
	out->planned_relocations = atomic64_read(&job->planned_relocations);
	out->reclaimed_extents   = atomic64_read(&job->reclaimed_extents);
	out->l2_relocated        = atomic64_read(&job->l2_relocated);
}
EXPORT_SYMBOL(tpv_compaction_get_progress);

/* ==========================================================================
 * Auto-kick path - invoked from the TPV attach handler when the attach
 * payload carries isCompaction=true.  tpv_compaction_run is blocking
 * (spawns workers, waits for them, does phase-2 L2 reloc), so we must
 * not call it from the MCS handler thread directly - schedule on the
 * system workqueue.  Uses the same shutdown/pending_starts handshake as
 * the /proc-write path so it composes with tpv_compaction_destroy.
 * ==========================================================================
 */

struct tpv_compaction_kick_work {
	struct work_struct  w;
	struct nvmeibc_tpv *tpv;
	u32                 aggressiveness;
};

static void tpv_compaction_kick_work_fn(struct work_struct *w)
{
	struct tpv_compaction_kick_work *kw =
		container_of(w, struct tpv_compaction_kick_work, w);
	struct nvmeibc_tpv        *tpv = kw->tpv;
	struct tpv_compaction_job *job = &tpv->compaction_job;

	if (atomic_read(&tpv->state) == TPV_ATTACHED &&
	    !READ_ONCE(job->shutdown)) {
		struct tpv_compaction_params p = {
			.aggressiveness = kw->aggressiveness
		};
		(void)tpv_compaction_run(tpv, &p);
	}
	kfree(kw);
	if (atomic_dec_and_test(&job->pending_starts))
		wake_up_all(&job->pending_starts_wq);
}

int tpv_compaction_kick(struct nvmeibc_tpv *tpv,
			const struct tpv_compaction_params *p)
{
	struct tpv_compaction_job *job = &tpv->compaction_job;
	struct tpv_compaction_kick_work *kw;
	u32 aggressiveness = (p && p->aggressiveness) ? p->aggressiveness : 4;

	if (READ_ONCE(job->shutdown))
		return -ESHUTDOWN;

	kw = kzalloc(sizeof(*kw), GFP_KERNEL);
	if (!kw)
		return -ENOMEM;

	atomic_inc(&job->pending_starts);
	smp_mb__after_atomic();
	if (READ_ONCE(job->shutdown)) {
		if (atomic_dec_and_test(&job->pending_starts))
			wake_up_all(&job->pending_starts_wq);
		kfree(kw);
		return -ESHUTDOWN;
	}

	INIT_WORK(&kw->w, tpv_compaction_kick_work_fn);
	kw->tpv            = tpv;
	kw->aggressiveness = aggressiveness;
	schedule_work(&kw->w);

	_NI(tpv_compaction_kick,
	    "TPV: @STR: compaction kicked on attach aggressiveness=@UINT",
	    tpv->tpv_name, aggressiveness);
	return 0;
}
EXPORT_SYMBOL(tpv_compaction_kick);

/* ==========================================================================
 * Planner + run loop.
 *
 * Planner state machine under job->plan_mutex:
 *   1. Build plan_sources: list of (non-full, non-L1) refs sorted
 *      ascending by allocated_count.
 *   2. plan_curr_source = head of plan_sources.
 *   3. plan_curr_dest = tail of plan_sources (most-used non-full).
 *      Never the same ref as plan_curr_source.
 *   4. On each plan_next() call, advance cursors as refs empty out
 *      (source) or fill up (dest).
 *
 * Workers call plan_next() to get their next (virt_idx, src, dst).
 * The planner iterates live slots in the current source by walking
 * the xarray for entries whose cdv_extent_index matches.
 * ==========================================================================
 */

static int  plan_build(struct nvmeibc_tpv *tpv);
static void plan_free(struct nvmeibc_tpv *tpv);
static int  plan_next(struct nvmeibc_tpv *tpv,
		      u64 *out_virt_idx,
		      struct nvmeibc_cdv_extent_ref **out_src,
		      struct nvmeibc_cdv_extent_ref **out_dst);
static bool plan_termination_met(struct nvmeibc_tpv *tpv);

static int worker_thread_fn(void *arg);

static u64  tpv_compact_l2_phase(struct nvmeibc_tpv *tpv);

int tpv_compaction_run(struct nvmeibc_tpv *tpv,
		       const struct tpv_compaction_params *p)
{
	struct tpv_compaction_job *job = &tpv->compaction_job;
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u32 aggressiveness = p ? p->aggressiveness : 4;
	int rv = 0;
	u32 i;
	int prev_state;

	if (aggressiveness == 0)
		aggressiveness = 1;

	/* Refuse new runs during teardown. */
	if (READ_ONCE(job->shutdown))
		return -ESHUTDOWN;

	/*
	 * Accept a start from either IDLE (fresh attach) or DONE (previous
	 * run completed).  Atomic cmpxchg-from-either implemented as two
	 * tries so the result is unambiguous: if neither transitions
	 * succeeded, someone else is already running.
	 */
	prev_state = atomic_cmpxchg(&job->state, TPV_COMPACTION_IDLE,
				    TPV_COMPACTION_RUNNING);
	if (prev_state != TPV_COMPACTION_IDLE) {
		prev_state = atomic_cmpxchg(&job->state, TPV_COMPACTION_DONE,
					    TPV_COMPACTION_RUNNING);
		if (prev_state != TPV_COMPACTION_DONE)
			return -EBUSY;
	}

	/*
	 * Drain any pre-existing dirty allocator state via persist_work.
	 * Compaction's L2 writer performs direct read-modify-write on L2
	 * pages; we must not race with persist_work flushing leaves we are
	 * about to rewrite.  cancel_work_sync ensures the current
	 * persist_work run (if any) completes before we proceed, and a
	 * subsequent flush_and_promote snapshot drains any parked frees.
	 * Compaction does not mark L2 leaves dirty itself, so persist_work
	 * has nothing else to do until the worker exits.
	 */
	cancel_work_sync(&tpv->persist_work);
	(void)nvmeibc_tpv_flush_and_promote(tpv);

	/*
	 * Freeze the allocator's drain-of-pending-returns for the duration
	 * of the run.  plan_sources caches raw struct nvmeibc_cdv_extent_ref
	 * pointers (see plan_build); a concurrent tpv_drain_pending_returns
	 * can kfree a ref after CDV_FREE_EXTENT succeeds, which would turn
	 * plan_next's head->ref dereferences into UAF.  Set defer_drain
	 * before cancel_work_sync so no new drain starts once the in-flight
	 * one (if any) completes.  The accumulated pending_return_list is
	 * drained once below after plan_free().
	 */
	WRITE_ONCE(job->defer_drain, true);
	cancel_work_sync(&tpv->cdv_alloc_work);

	atomic_set(&job->abort_flag, 0);
	atomic64_set(&job->relocated, 0);
	atomic64_set(&job->planned_relocations, 0);
	atomic64_set(&job->reclaimed_extents, 0);
	reinit_completion(&job->workers_done);

	/* -- Pre-flight no-op: already under watermark? -- */
	if (alloc->free_tpv_extent_count <= alloc->high_watermark) {
		complete_all(&job->workers_done);
		rv = 0;
		goto out_done;
	}

	/* -- Spawn L2 writer thread -- */
	WRITE_ONCE(job->l2_writer_should_stop, false);
	job->l2_writer = kthread_run(l2_writer_thread_fn, tpv,
				     "tpv_l2w/%s", tpv->tpv_name);
	if (IS_ERR(job->l2_writer)) {
		rv = PTR_ERR(job->l2_writer);
		job->l2_writer = NULL;
		goto out_done;
	}

	/* -- Build planner -- */
	rv = plan_build(tpv);
	if (rv)
		goto out_stop_l2;

	/*
	 * Spawn workers.  n_workers_running is pre-incremented to the
	 * target and decremented on every spawn failure; the last worker
	 * out signals workers_done via atomic_dec_and_test.  If every
	 * kthread_run fails (n_workers_running ends at 0) the main thread
	 * fires complete_all itself so wait_for_completion below does not
	 * hang.
	 */
	atomic_set(&job->n_workers_running, aggressiveness);
	_NI(tpv_compaction_run_start,
	    "TPV: @STR: compaction run start aggressiveness=@UINT planned=@LLU free_count=@LLU high_wm=@LLU",
	    tpv->tpv_name, aggressiveness,
	    (unsigned long long)atomic64_read(&job->planned_relocations),
	    alloc->free_tpv_extent_count, alloc->high_watermark);
	for (i = 0; i < aggressiveness; i++) {
		struct task_struct *t;
		t = kthread_run(worker_thread_fn, tpv,
				"tpv_compact/%s/%u", tpv->tpv_name, i);
		if (IS_ERR(t)) {
			_NE(tpv_compaction_spawn_fail,
			    "TPV: @STR: kthread_run worker @UINT failed rv=@INT",
			    tpv->tpv_name, i, (int)PTR_ERR(t));
			/* Match the worker-exit decrement pattern. */
			if (atomic_dec_and_test(&job->n_workers_running))
				complete_all(&job->workers_done);
		}
	}
	if (atomic_read(&job->n_workers_running) == 0) {
		/*
		 * All spawn attempts failed AND complete_all already fired
		 * from the last failure above.  Wait returns immediately;
		 * signal no progress as a spawn error.
		 */
		rv = -EAGAIN;
	}

	/* -- Wait for all workers -- */
	wait_for_completion(&job->workers_done);

	if (atomic_read(&job->abort_flag))
		rv = -EINTR;

	/*
	 * -- Phase 2: L2-table relocation --
	 *
	 * Workers exited; safe to migrate L2 tables single-threaded.  The
	 * L2 writer thread is still alive (we'll stop it below), but no
	 * one is submitting to it anymore.  tpv_compact_l2_phase issues
	 * its own sync reads/writes for L2 bodies and L1 entries, not
	 * through the writer thread.
	 *
	 * Skipped on abort: the abort path wants a fast exit, not a cleanup
	 * pass.
	 */
	if (rv == 0 && !atomic_read(&job->abort_flag))
		(void)tpv_compact_l2_phase(tpv);

	plan_free(tpv);

out_stop_l2:
	WRITE_ONCE(job->l2_writer_should_stop, true);
	wake_up_all(&job->l2_req_wq);
	if (job->l2_writer) {
		kthread_stop(job->l2_writer);
		job->l2_writer = NULL;
	}

out_done:
	/*
	 * Re-arm the normal allocator drain path.  plan_sources is freed
	 * by plan_free above (or never built on the error path), so raw
	 * ref pointers are no longer cached anywhere; cdv_alloc_work is
	 * safe to kfree emptied refs again.  Schedule once to drain any
	 * returns that accumulated on pending_return_list during the run.
	 */
	WRITE_ONCE(job->defer_drain, false);
	if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_work(&tpv->cdv_alloc_work);
	atomic_set(&job->state, TPV_COMPACTION_DONE);

	_NI(tpv_compaction_run_done,
	    "TPV: @STR: compaction run done rv=@INT data_reloc=@LLU l2_reloc=@LLU reclaimed=@LLU",
	    tpv->tpv_name, rv,
	    (unsigned long long)atomic64_read(&job->relocated),
	    (unsigned long long)atomic64_read(&job->l2_relocated),
	    (unsigned long long)atomic64_read(&job->reclaimed_extents));

	return rv;
}
EXPORT_SYMBOL(tpv_compaction_run);

static int worker_thread_fn(void *arg)
{
	struct nvmeibc_tpv *tpv = arg;
	struct tpv_compaction_job *job = &tpv->compaction_job;
	u64 iters = 0;
	u64 eagain = 0;

	_ND(tpv_compact_worker_enter,
	    "TPV: @STR: compaction worker entered",
	    tpv->tpv_name);

	while (!kthread_should_stop() &&
	       !atomic_read(&job->abort_flag) &&
	       !plan_termination_met(tpv)) {
		u64 virt_idx;
		struct nvmeibc_cdv_extent_ref *src = NULL, *dst = NULL;
		int rv;

		iters++;
		rv = plan_next(tpv, &virt_idx, &src, &dst);
		if (rv == -ENOENT) {
			_ND(tpv_compact_plan_exhausted,
			    "TPV: @STR: plan_next -ENOENT iters=@LLU eagain=@LLU",
			    tpv->tpv_name, iters, eagain);
			break;			/* planner exhausted */
		}
		if (rv < 0) {
			eagain++;
			/*
			 * Bail out if we spin on -EAGAIN too long.  Only
			 * happens if a src ref has data_slots > 0 but the
			 * xarray has no matching entry - a transient race
			 * with persist_work's promote step.  Caps wasted
			 * CPU.
			 */
			if (eagain > 1000) {
				_NE(tpv_compact_eagain_storm,
				    "TPV: @STR: plan_next -EAGAIN x@LLU iters=@LLU; bailing",
				    tpv->tpv_name, eagain, iters);
				break;
			}
			continue;
		}

		_ND(tpv_compact_reloc_start,
		    "TPV: @STR: reloc virt_idx=@LLU src_ext=@LLU dst_ext=@LLU",
		    tpv->tpv_name, virt_idx,
		    src->extent_index, dst->extent_index);

		rv = tpv_reloc_one(tpv, virt_idx, src, dst);
		if (rv == 0) {
			atomic64_inc(&job->relocated);
			_ND(tpv_compact_reloc_ok,
			    "TPV: @STR: reloc ok virt_idx=@LLU",
			    tpv->tpv_name, virt_idx);
		} else {
			_NW(tpv_compact_reloc_err,
			    "TPV: @STR: reloc virt_idx=@LLU rv=@INT",
			    tpv->tpv_name, virt_idx, rv);
		}
	}

	_ND(tpv_compact_worker_exit,
	    "TPV: @STR: compaction worker exit iters=@LLU eagain=@LLU relocated=@LLU",
	    tpv->tpv_name, iters, eagain,
	    (unsigned long long)atomic64_read(&job->relocated));

	/* Last worker out wakes the run loop. */
	if (atomic_dec_and_test(&job->n_workers_running))
		complete_all(&job->workers_done);
	return 0;
}

/* ==========================================================================
 * Integration stubs.  These require read-only integration knowledge of
 * the existing allocator + L2 layout.  Each is a skeleton that the
 * integrating engineer fills in from the existing helpers in
 * nvmeibc_tpv_allocator.c and nvmeibc_tpv_persist.c.  The rest of the
 * file above is self-contained and compiles against these stubs.
 * ==========================================================================
 */

/*
 * Pop a free slot from the TPV-wide free pool that belongs to
 * dest_ref (i.e. whose cdv_extent_index matches).  Returns -ENOSPC
 * if dest_ref has no slot currently on alloc->free_tpv_extents.
 *
 * dest_ref->allocated_count is incremented here to reserve the slot.
 * alloc->free_tpv_extent_count is decremented.  Matches the internal
 * accounting rules established by nvmeibc_tpv_alloc_extent.
 */
static int reloc_take_dest_slot(struct nvmeibc_tpv *tpv,
				struct nvmeibc_cdv_extent_ref *dest_ref,
				u64 *out_phys)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_free_slot *slot, *tmp;
	int rv = -ENOSPC;

	spin_lock(&alloc->lock);
	list_for_each_entry_safe(slot, tmp, &alloc->free_tpv_extents, node) {
		if (slot->cdv_extent_index != dest_ref->extent_index)
			continue;
		list_del(&slot->node);
		alloc->free_tpv_extent_count--;
		dest_ref->allocated_count++;
		nvmeibc_tpv_reposition_ref_locked(alloc, dest_ref);
		*out_phys = slot->phys_offset;
		kfree(slot);
		rv = 0;
		break;
	}
	spin_unlock(&alloc->lock);
	return rv;
}

/*
 * Return the source slot to the TPV-wide free pool and decrement
 * source_ref->allocated_count.  If the ref transitions to empty (and
 * isn't the L1 extent), move it to pending_return_list and kick
 * cdv_alloc_work to send CDV_FREE_EXTENT.  Bumps the job's
 * reclaimed_extents counter on empty-transition.
 *
 * Must be called only after the L2 leaf write has committed (the
 * durability invariant in TPV_Trimming.md Step 4).
 */
static void reloc_return_source_slot(struct nvmeibc_tpv *tpv,
				     struct nvmeibc_cdv_extent_ref *source_ref,
				     u64 source_phys)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_free_slot *slot;
	bool schedule_drain = false;

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot) {
		/* Memory pressure: dark slot, same handling as
		 * nvmeibc_tpv_free_extent.  Still account allocated_count
		 * so the ref can eventually be returned. */
		spin_lock(&alloc->lock);
		source_ref->allocated_count--;
		if (source_ref->allocated_count == 0 && !source_ref->is_l1_extent) {
			list_move_tail(&source_ref->node, &alloc->pending_return_list);
			source_ref->on_pending_return_list = true;
			schedule_drain = true;
			atomic64_inc(&tpv->compaction_job.reclaimed_extents);
		} else {
			nvmeibc_tpv_reposition_ref_locked(alloc, source_ref);
		}
		spin_unlock(&alloc->lock);
		goto out;
	}
	slot->phys_offset      = source_phys;
	slot->cdv_extent_index = source_ref->extent_index;
	INIT_LIST_HEAD(&slot->node);

	spin_lock(&alloc->lock);
	list_add(&slot->node, &alloc->free_tpv_extents);
	alloc->free_tpv_extent_count++;
	source_ref->allocated_count--;
	if (source_ref->allocated_count == 0 && !source_ref->is_l1_extent) {
		list_move_tail(&source_ref->node, &alloc->pending_return_list);
		source_ref->on_pending_return_list = true;
		atomic64_inc(&alloc->stat_cdv_returns_queued);
		schedule_drain = true;
		atomic64_inc(&tpv->compaction_job.reclaimed_extents);
	} else {
		nvmeibc_tpv_reposition_ref_locked(alloc, source_ref);
	}
	spin_unlock(&alloc->lock);

out:
	if (schedule_drain && !atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_work(&tpv->cdv_alloc_work);
}

/*
 * Look up the xarray entry for virt_idx; return it iff it currently
 * resolves to a slot inside source_ref.  Caller holds rcu_read_lock.
 */
static struct nvmeibc_tpv_extent_entry *
reloc_lookup_in_source(struct nvmeibc_tpv *tpv,
		       u64 virt_idx,
		       struct nvmeibc_cdv_extent_ref *source_ref)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_extent_entry *entry;

	entry = xa_load(&alloc->extent_map, virt_idx);
	if (!entry)
		return NULL;
	if (entry->cdv_extent_index != source_ref->extent_index)
		return NULL;
	return entry;
}

/*
 * Swap the xarray entry for virt_idx with a fresh entry pointing at
 * dest_phys in dest_cdv_extent_index.  The old entry is kfree_rcu'd.
 * Called after the L2 leaf write is durable.
 */
static int reloc_swap_xarray_entry(struct nvmeibc_tpv *tpv,
				   u64 virt_idx,
				   u64 dest_phys,
				   u64 dest_cdv_extent_index)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_extent_entry *neu;
	void *old_void;

	neu = kzalloc(sizeof(*neu), GFP_KERNEL);
	if (!neu)
		return -ENOMEM;
	neu->phys_offset      = dest_phys;
	neu->cdv_extent_index = dest_cdv_extent_index;
	neu->persisted        = true;	/* L2 leaf already durable */

	/* xa_store returns the previous value (or an encoded error).
	 * On success with a prior entry present, that entry is ours to
	 * kfree_rcu.  Compaction is offline, so there is no concurrent
	 * allocator racing on this virt_idx. */
	old_void = xa_store(&alloc->extent_map, virt_idx, neu, GFP_KERNEL);
	if (xa_is_err(old_void)) {
		kfree(neu);
		return xa_err(old_void);
	}
	if (old_void) {
		struct nvmeibc_tpv_extent_entry *old = old_void;
		kfree_rcu(old, rcu);
	}
	return 0;
}

static int compaction_l2_page_for_virt(struct nvmeibc_tpv *tpv,
				       u64 virt_idx,
				       u64 *out_page_phys,
				       u64 *out_page_offset_in_l2,
				       u64 *out_l2_page_idx,
				       u64 *out_l1_idx)
{
	/*
	 * Mirrors nvmeibc_tpv_mark_l2_leaf_dirty in nvmeibc_tpv_persist.c:
	 *   T      = tpv_extent_size_kb * 1024  (slot size)
	 *   N_L2   = T / sizeof(tpv_tree_entry) = T / 8
	 *   L1_idx = virt_idx / N_L2
	 *   L2_idx = virt_idx % N_L2
	 *
	 * The covering L2 table is located at ctx->phys; the 4 KB page
	 * within that table holding the leaf starts at
	 * ctx->phys + ((L2_idx * 8) / 4096) * 4096.
	 *
	 * Uses the META allocator's L1/L2 tree (which equals the data
	 * allocator in single-CDV mode; split-mode routes to the meta
	 * CDV via nvmeibc_tpv_meta_cdv()).
	 */
	struct nvmeibc_tpv_allocator *malloc_ = nvmeibc_tpv_meta_alloc(tpv);
	const u32 T = malloc_->tpv_extent_size_kb << 10;
	const u64 N_L2 = T / sizeof(struct tpv_tree_entry);
	const u64 l1_idx = virt_idx / N_L2;
	const u64 l2_idx = virt_idx % N_L2;
	const u64 leaf_byte = l2_idx * sizeof(struct tpv_tree_entry);
	const u64 page_base = (leaf_byte / 4096ULL) * 4096ULL;
	struct tpv_l2_ctx *ctx;

	ctx = xa_load(&malloc_->l1_to_l2_ctx, l1_idx);
	if (!ctx) {
		/* No L2 table yet means nothing to rewrite; compaction
		 * should never be asked to relocate an unmapped virt_idx. */
		return -ENOENT;
	}

	*out_page_phys         = ctx->phys + page_base;
	*out_page_offset_in_l2 = leaf_byte - page_base;
	*out_l2_page_idx       = leaf_byte / 4096ULL;
	*out_l1_idx            = l1_idx;
	return 0;
}

/*
 * Helper: slots per CDV extent.
 */
static inline u64 n_slots_per_cdv_extent(const struct nvmeibc_tpv_allocator *alloc)
{
	const u64 T = (u64)alloc->tpv_extent_size_kb << 10;
	const u64 E = (u64)alloc->cdv_extent_size_mib << 20;
	return E / T;
}

/*
 * Build the initial planner state.  Snapshots the cdv_extent_list into
 * job->plan_sources under alloc->lock, sorted ascending by
 * allocated_count (so the head is the sparsest source).  Records
 * planned_relocations = sum of allocated_count across all eligible refs
 * above the dense-floor.
 *
 * plan_sources uses a small helper node (struct plan_node) so we don't
 * perturb the real ref->node linkage.
 */
struct plan_node {
	struct nvmeibc_cdv_extent_ref *ref;
	struct list_head node;
};

static int plan_build(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct tpv_compaction_job    *job = &tpv->compaction_job;
	struct nvmeibc_cdv_extent_ref *ref;
	const u64 n_slots = n_slots_per_cdv_extent(alloc);
	u64 live_total = 0;
	u64 planned;

	mutex_lock(&job->plan_mutex);
	INIT_LIST_HEAD(&job->plan_sources);
	job->head_pn = NULL;
	job->tail_pn = NULL;

	spin_lock(&alloc->lock);
	_ND(tpv_plan_build_enter,
	    "TPV: @STR: plan_build walking cdv_extent_list",
	    tpv->tpv_name);
	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		struct plan_node *pn;
		u64 data_slots;

		/*
		 * data_slots = live DATA leaves = allocated_count - l2_slots.
		 * L2-table slots live in allocated_count but are not in the
		 * xarray, so only data_slots is movable by phase 1 (data
		 * compaction).  Phase 2 (L2-table compaction, below) handles
		 * l2_slots separately.
		 */
		data_slots = ref->allocated_count > ref->l2_slots ?
			     ref->allocated_count - ref->l2_slots : 0;

		_ND(tpv_plan_build_ext,
		    "TPV: @STR: plan_build ext=@LLU alloc=@LLU l2=@LLU data=@LLU is_l1=@INT",
		    tpv->tpv_name, ref->extent_index,
		    ref->allocated_count, ref->l2_slots, data_slots,
		    (int)ref->is_l1_extent);

		/*
		 * Include every non-full extent.  head_pn/tail_pn walking
		 * decides src/dst eligibility at hand-out time:
		 *   - SRC: !is_l1_extent && data_slots > 0
		 *   - DST: any non-full (L1 included)
		 * Extents with allocated_count == 0 and !is_l1_extent are
		 * already on pending_return_list (will be drained after the
		 * run) so there's no need to include them - they can't serve
		 * either role.
		 */
		if (ref->allocated_count >= n_slots)
			continue;	/* full: no room to give as dst */
		if (ref->allocated_count == 0 && !ref->is_l1_extent)
			continue;	/* empty non-L1: already returnable */

		/* kmalloc under spinlock - use GFP_ATOMIC.  The plan list
		 * is bounded by the number of CDV extents this TPV owns,
		 * typically a handful. */
		pn = kmalloc(sizeof(*pn), GFP_ATOMIC);
		if (!pn)
			continue;
		pn->ref = ref;
		INIT_LIST_HEAD(&pn->node);

		/*
		 * cdv_extent_list is maintained sorted ascending by
		 * allocated_count (the sort invariant), so a straight append
		 * here produces plan_sources in the same sparsest-first order.
		 */
		list_add_tail(&pn->node, &job->plan_sources);
		if (!ref->is_l1_extent)
			live_total += data_slots;
	}
	spin_unlock(&alloc->lock);

	if (!list_empty(&job->plan_sources)) {
		job->head_pn = list_first_entry(&job->plan_sources,
						struct plan_node, node);
		job->tail_pn = list_last_entry(&job->plan_sources,
					       struct plan_node, node);
	}

	planned = live_total;
	atomic64_set(&job->planned_relocations, planned);

	_ND(tpv_plan_build_done,
	    "TPV: @STR: plan_build done planned=@LLU head_ext=@LLU tail_ext=@LLU",
	    tpv->tpv_name, planned,
	    job->head_pn ? job->head_pn->ref->extent_index : 0,
	    job->tail_pn ? job->tail_pn->ref->extent_index : 0);

	mutex_unlock(&job->plan_mutex);
	return 0;
}

static void plan_free(struct nvmeibc_tpv *tpv)
{
	struct tpv_compaction_job *job = &tpv->compaction_job;
	struct plan_node *pn, *tmp;

	mutex_lock(&job->plan_mutex);
	list_for_each_entry_safe(pn, tmp, &job->plan_sources, node) {
		list_del(&pn->node);
		kfree(pn);
	}
	mutex_unlock(&job->plan_mutex);
}

/*
 * Hand out the next (virt_idx, source_ref, dest_ref) triple.
 *
 * Source is the head of plan_sources (sparsest).  Dest is the tail of
 * plan_sources whose allocated_count < n_slots AND which is not the
 * source itself.  virt_idx is obtained by walking the xarray for any
 * entry whose cdv_extent_index matches source_ref->extent_index.
 *
 * Returns:
 *   0      - *out_* populated; caller proceeds to tpv_reloc_one.
 *   -EAGAIN - source exhausted between lookups; caller retries.
 *   -ENOENT - no (src, dst) pair remains; caller emits RECOVER_FINISH.
 */
/*
 * Advance head_pn forward past any node that cannot serve as src:
 *   - L1 extent (never a src)
 *   - full (no useful work - also never a dst)
 *   - data_slots == 0 (nothing to move)
 * Returns the (possibly-advanced) head, or NULL if the head has
 * walked off the end.  Caller holds plan_mutex.
 */
static struct plan_node *plan_advance_head(struct tpv_compaction_job *job,
					   u64 n_slots)
{
	while (job->head_pn) {
		struct plan_node *h = job->head_pn;
		u64 data_slots;

		data_slots = h->ref->allocated_count > h->ref->l2_slots ?
			     h->ref->allocated_count - h->ref->l2_slots : 0;
		if (!h->ref->is_l1_extent &&
		    h->ref->allocated_count < n_slots &&
		    data_slots > 0)
			return h;	/* eligible as src */
		/* Step forward. */
		if (h->node.next == &job->plan_sources)
			job->head_pn = NULL;
		else
			job->head_pn = list_next_entry(h, node);
	}
	return NULL;
}

/*
 * Retreat tail_pn backward past any node that cannot serve as dst:
 *   - full (no room)
 *   - same as head (would relocate into self)
 * Stops when crossed past head (tail_pn precedes head_pn in list order).
 * Returns the eligible tail, or NULL if no dst remains.  Caller holds
 * plan_mutex.
 */
static struct plan_node *plan_retreat_tail(struct tpv_compaction_job *job,
					   u64 n_slots)
{
	while (job->tail_pn) {
		struct plan_node *t = job->tail_pn;

		if (t == job->head_pn) {
			/* Crossed - no separate dst remains. */
			job->tail_pn = NULL;
			return NULL;
		}
		if (t->ref->allocated_count < n_slots)
			return t;	/* has room */
		if (t->node.prev == &job->plan_sources)
			job->tail_pn = NULL;
		else
			job->tail_pn = list_prev_entry(t, node);
	}
	return NULL;
}

static int plan_next(struct nvmeibc_tpv *tpv,
		     u64 *out_virt_idx,
		     struct nvmeibc_cdv_extent_ref **out_src,
		     struct nvmeibc_cdv_extent_ref **out_dst)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct tpv_compaction_job    *job = &tpv->compaction_job;
	const u64 n_slots = n_slots_per_cdv_extent(alloc);
	struct plan_node *head, *tail;
	struct nvmeibc_cdv_extent_ref *src_ref, *dst_ref;
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;
	int rv = -ENOENT;

	mutex_lock(&job->plan_mutex);

	head = plan_advance_head(job, n_slots);
	if (!head)
		goto out;

	tail = plan_retreat_tail(job, n_slots);
	if (!tail)
		goto out;

	src_ref = head->ref;
	dst_ref = tail->ref;

	/*
	 * Find any live virt_idx whose xarray entry points at src_ref.
	 * data_slots > 0 was asserted by plan_advance_head, so the xarray
	 * MUST contain at least one matching entry - if we don't find one,
	 * it is a transient race with persist_work's promote step (the
	 * allocated_count decrement trails the xa_erase) and the caller
	 * should retry.
	 */
	rcu_read_lock();
	xa_for_each(&alloc->extent_map, idx, entry) {
		if (entry->cdv_extent_index == src_ref->extent_index) {
			*out_virt_idx = (u64)idx;
			*out_src = src_ref;
			*out_dst = dst_ref;
			rv = 0;
			break;
		}
	}
	rcu_read_unlock();
	if (rv != 0)
		rv = -EAGAIN;

out:
	mutex_unlock(&job->plan_mutex);
	return rv;
}

static bool plan_termination_met(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	/* Primary termination: watermark satisfied. */
	return alloc->free_tpv_extent_count <= alloc->high_watermark;
}

/* ==========================================================================
 * Phase 2: L2-table relocation.
 *
 * After phase 1 (data relocation) completes, extents still holding L2
 * tables cannot be returned to TOMA even if they have no data.  Phase 2
 * walks those extents and relocates each L2 table to the L1 extent (or
 * any extent with room), so the source extent drops to allocated_count
 * == 0 and gets reclaimed by the subsequent drain.
 *
 * Protocol:
 *   1. Allocate a free slot in the destination extent.
 *   2. Read the old L2 page (T bytes), write it to the new slot.
 *   3. Update the L1 entry at index l1_idx to point at the new slot
 *      (single 8-byte leaf write inside the L1 page).  THIS IS THE
 *      COMMIT POINT: after it lands on CDV, load_state on next attach
 *      will read L2 from the new location.
 *   4. Update the in-memory tpv_l2_ctx->phys so the L2 writer (and
 *      future leaf lookups) go to the new location.
 *   5. Free the old slot - decrement source ref's allocated_count and
 *      l2_slots; if the ref transitions to allocated_count == 0 and is
 *      not the L1 extent, move it to pending_return_list.
 *
 * Crash safety:
 *   - Crash before step 3 -> L1 still points at old slot, old L2 page
 *     is intact -> attach reads valid state, new slot is an orphan
 *     reclaimed at free-pool rebuild time.
 *   - Crash after step 3, before step 5 -> L1 points at new slot,
 *     old slot is an orphan -> reclaimed at attach.
 *
 * Concurrency:
 *   - Runs single-threaded in tpv_compaction_run AFTER all data workers
 *     exited and the L2 writer thread is stopped.  No other path can
 *     read/write L2 tables concurrently.
 * ==========================================================================
 */

/*
 * CDV byte offset of slot @slot within data extent @extent_index.
 * Duplicates persist.c's persist_tree_slot_offset logic (static-inline
 * there).  Extent indices are 1-based: extent 1 starts at byte offset A.
 */
static u64 compact_slot_phys(const struct nvmeibc_tpv_allocator *a,
			     u64 extent_index, u64 slot)
{
	u64 A = (u64)a->allocator_size_gib << 30;
	u64 E = (u64)a->cdv_extent_size_mib << 20;
	u64 T = (u64)a->tpv_extent_size_kb  << 10;

	return A + (extent_index - 1) * E + slot * T;
}

/* Inverse of compact_slot_phys: extract (extent_index, slot) from phys. */
static void compact_decode_phys(const struct nvmeibc_tpv_allocator *a,
				u64 phys, u64 *extent_index, u64 *slot)
{
	u64 A = (u64)a->allocator_size_gib << 30;
	u64 E = (u64)a->cdv_extent_size_mib << 20;
	u64 T = (u64)a->tpv_extent_size_kb  << 10;
	u64 rel = phys - A;

	*extent_index = rel / E + 1;
	*slot         = (rel - (*extent_index - 1) * E) / T;
}

/*
 * Commit an L1 leaf update by doing a synchronous RMW on the 4 KB page
 * that covers L1 entry @l1_idx inside the L1 table (slot 0 of the L1
 * extent).  Returns 0 on success.
 */
static int compact_l1_commit_entry(struct nvmeibc_tpv *tpv,
				   u64 l1_idx, u64 new_cdv_offset)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u64 l1_slot_phys = compact_slot_phys(alloc, alloc->l1_extent_index, 0);
	u64 entry_byte   = sizeof(struct tpv_l1_header) +
			   l1_idx * sizeof(struct tpv_tree_entry);
	u64 page_base    = (entry_byte / 4096ULL) * 4096ULL;
	u64 page_phys    = l1_slot_phys + page_base;
	u64 off_in_page  = entry_byte - page_base;
	u8 *page;
	int rv;

	page = kmalloc(4096, GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	rv = nvmeibc_tpv_cdv_sync_read(tpv, page_phys, page, 4096);
	if (rv)
		goto out;

	*((u64 *)(page + off_in_page)) = new_cdv_offset;

	rv = nvmeibc_tpv_cdv_sync_write(tpv, page_phys, page, 4096);
out:
	kfree(page);
	return rv;
}

/*
 * Take one free slot from dst_ref's share of alloc->free_tpv_extents.
 * Increments dst_ref->allocated_count AND l2_slots (this slot is going
 * to host an L2 table, not data).  Returns -ENOSPC if dst_ref has no
 * free slot in the pool.
 */
static int compact_take_dest_l2_slot(struct nvmeibc_tpv *tpv,
				     struct nvmeibc_cdv_extent_ref *dst_ref,
				     u64 *out_phys)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_free_slot *slot, *tmp;
	int rv = -ENOSPC;

	spin_lock(&alloc->lock);
	list_for_each_entry_safe(slot, tmp, &alloc->free_tpv_extents, node) {
		if (slot->cdv_extent_index != dst_ref->extent_index)
			continue;
		list_del(&slot->node);
		alloc->free_tpv_extent_count--;
		dst_ref->allocated_count++;
		dst_ref->l2_slots++;
		nvmeibc_tpv_reposition_ref_locked(alloc, dst_ref);
		*out_phys = slot->phys_offset;
		kfree(slot);
		rv = 0;
		break;
	}
	spin_unlock(&alloc->lock);
	return rv;
}

/*
 * Return an L2 slot to the free pool and decrement the owning ref's
 * allocated_count + l2_slots.  If the ref transitions to
 * allocated_count == 0 and isn't the L1 extent, move it to
 * pending_return_list and arm cdv_alloc_work.
 */
static void compact_free_src_l2_slot(struct nvmeibc_tpv *tpv,
				     struct nvmeibc_cdv_extent_ref *src_ref,
				     u64 src_phys)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_free_slot *slot;
	bool schedule_drain = false;

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);

	spin_lock(&alloc->lock);
	if (slot) {
		slot->phys_offset      = src_phys;
		slot->cdv_extent_index = src_ref->extent_index;
		INIT_LIST_HEAD(&slot->node);
		list_add(&slot->node, &alloc->free_tpv_extents);
		alloc->free_tpv_extent_count++;
	}
	src_ref->allocated_count--;
	if (src_ref->l2_slots > 0)
		src_ref->l2_slots--;
	if (src_ref->allocated_count == 0 && !src_ref->is_l1_extent) {
		list_move_tail(&src_ref->node, &alloc->pending_return_list);
		src_ref->on_pending_return_list = true;
		atomic64_inc(&alloc->stat_cdv_returns_queued);
		schedule_drain = true;
		atomic64_inc(&tpv->compaction_job.reclaimed_extents);
	} else {
		nvmeibc_tpv_reposition_ref_locked(alloc, src_ref);
	}
	spin_unlock(&alloc->lock);

	if (schedule_drain && !atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_work(&tpv->cdv_alloc_work);
}

/*
 * Relocate the L2 table covering L1 index @l1_idx from wherever it
 * currently lives to @dst_ref.  Returns 0 on success.
 */
static int tpv_reloc_l2_table(struct nvmeibc_tpv *tpv,
			      u64 l1_idx,
			      struct nvmeibc_cdv_extent_ref *dst_ref)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct tpv_l2_ctx *ctx;
	struct nvmeibc_cdv_extent_ref *src_ref = NULL, *r;
	u64 old_phys, new_phys = 0;
	u64 src_extent_idx, src_slot;
	u64 T = (u64)alloc->tpv_extent_size_kb << 10;
	u8 *buf = NULL;
	int rv;

	ctx = xa_load(&alloc->l1_to_l2_ctx, l1_idx);
	if (!ctx)
		return -ENOENT;
	old_phys = ctx->phys;

	/* Identify the source ref that currently owns old_phys. */
	compact_decode_phys(alloc, old_phys, &src_extent_idx, &src_slot);
	spin_lock(&alloc->lock);
	list_for_each_entry(r, &alloc->cdv_extent_list, node) {
		if (r->extent_index == src_extent_idx) {
			src_ref = r;
			break;
		}
	}
	spin_unlock(&alloc->lock);
	if (!src_ref)
		return -ENOENT;
	if (src_ref == dst_ref)
		return 0;	/* already where we want it */

	rv = compact_take_dest_l2_slot(tpv, dst_ref, &new_phys);
	if (rv)
		return rv;

	buf = kvmalloc(T, GFP_KERNEL);
	if (!buf) {
		rv = -ENOMEM;
		goto out_return_dst;
	}

	rv = nvmeibc_tpv_cdv_sync_read(tpv, old_phys, buf, T);
	if (rv)
		goto out_return_dst;
	rv = nvmeibc_tpv_cdv_sync_write(tpv, new_phys, buf, T);
	if (rv)
		goto out_return_dst;

	/* Commit point: L1 entry now points at new_phys. */
	rv = compact_l1_commit_entry(tpv, l1_idx, new_phys);
	if (rv)
		goto out_return_dst;

	/* Post-commit: redirect in-memory ctx, free the old slot. */
	ctx->phys = new_phys;
	compact_free_src_l2_slot(tpv, src_ref, old_phys);

	kvfree(buf);
	return 0;

out_return_dst:
	/* Rollback: the dst slot was taken (allocated_count + l2_slots
	 * bumped).  Decrement both and return the phys to the free pool. */
	if (new_phys) {
		struct nvmeibc_tpv_free_slot *s =
			kzalloc(sizeof(*s), GFP_KERNEL);
		spin_lock(&alloc->lock);
		dst_ref->allocated_count--;
		if (dst_ref->l2_slots > 0)
			dst_ref->l2_slots--;
		nvmeibc_tpv_reposition_ref_locked(alloc, dst_ref);
		if (s) {
			s->phys_offset      = new_phys;
			s->cdv_extent_index = dst_ref->extent_index;
			INIT_LIST_HEAD(&s->node);
			list_add(&s->node, &alloc->free_tpv_extents);
			alloc->free_tpv_extent_count++;
		}
		spin_unlock(&alloc->lock);
	}
	kvfree(buf);
	return rv;
}

/*
 * Phase 2 entry point.  Walks plan_sources, identifies every L2 table
 * that lives in a non-L1 extent, and relocates each one to the L1
 * extent.  Single-threaded; called from tpv_compaction_run after data
 * workers have exited.
 *
 * Returns the number of L2 tables successfully relocated.  Errors on
 * individual relocations are logged and skipped (best-effort).
 */
static u64 tpv_compact_l2_phase(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct tpv_compaction_job    *job = &tpv->compaction_job;
	const u64 n_slots = n_slots_per_cdv_extent(alloc);
	struct nvmeibc_cdv_extent_ref *l1_ref = NULL, *r;
	struct plan_node *pn;
	u64 done = 0;
	u64 planned = 0;

	/* Find the L1 extent ref (our destination). */
	spin_lock(&alloc->lock);
	list_for_each_entry(r, &alloc->cdv_extent_list, node) {
		if (r->is_l1_extent) {
			l1_ref = r;
			break;
		}
	}
	spin_unlock(&alloc->lock);
	if (!l1_ref) {
		_NW(tpv_compact_l2_no_l1,
		    "TPV: @STR: phase 2 skipped - no L1 extent found",
		    tpv->tpv_name);
		return 0;
	}

	/* Count L2 tables in non-L1 extents to give progress a planned value. */
	mutex_lock(&job->plan_mutex);
	list_for_each_entry(pn, &job->plan_sources, node) {
		if (pn->ref->is_l1_extent)
			continue;
		planned += pn->ref->l2_slots;
	}
	mutex_unlock(&job->plan_mutex);

	_ND(tpv_compact_l2_phase_start,
	    "TPV: @STR: phase 2 start planned_l2=@LLU dst_ext=@LLU dst_free=@LLU",
	    tpv->tpv_name, planned, l1_ref->extent_index,
	    (unsigned long long)(n_slots - l1_ref->allocated_count));

	/*
	 * Iterate l1_to_l2_ctx; for each ctx pointing into a non-L1 extent,
	 * relocate it to l1_ref.  We can't hold a lock across
	 * tpv_reloc_l2_table (it does CDV I/O), so snapshot the L1 indices
	 * we want to move first, then migrate them.
	 *
	 * Two-pass: count under rcu, allocate exact-size buffer, then fill
	 * under rcu.  Single-threaded at this point (workers already
	 * exited), so no concurrent xarray mutations between the passes.
	 */
	{
		unsigned long idx;
		struct tpv_l2_ctx *ctx;
		u64 *todo = NULL;
		u64 n_todo = 0, n_expected = 0, i;

		rcu_read_lock();
		xa_for_each(&alloc->l1_to_l2_ctx, idx, ctx) {
			u64 ext_idx, slot;

			if (!ctx)
				continue;
			compact_decode_phys(alloc, ctx->phys, &ext_idx, &slot);
			if (ext_idx == alloc->l1_extent_index)
				continue;
			n_expected++;
		}
		rcu_read_unlock();

		if (n_expected == 0)
			return 0;

		todo = kvmalloc_array(n_expected, sizeof(u64), GFP_KERNEL);
		if (!todo)
			return 0;

		rcu_read_lock();
		xa_for_each(&alloc->l1_to_l2_ctx, idx, ctx) {
			u64 ext_idx, slot;

			if (!ctx)
				continue;
			compact_decode_phys(alloc, ctx->phys, &ext_idx, &slot);
			if (ext_idx == alloc->l1_extent_index)
				continue;
			if (n_todo < n_expected)
				todo[n_todo++] = (u64)idx;
		}
		rcu_read_unlock();

		for (i = 0; i < n_todo; i++) {
			int rv;

			if (atomic_read(&job->abort_flag))
				break;
			if (l1_ref->allocated_count >= n_slots) {
				_NW(tpv_compact_l2_dst_full,
				    "TPV: @STR: phase 2 L1 dst full; @LLU L2 tables remain",
				    tpv->tpv_name,
				    (unsigned long long)(n_todo - i));
				break;
			}
			rv = tpv_reloc_l2_table(tpv, todo[i], l1_ref);
			if (rv == 0) {
				done++;
				atomic64_inc(&job->l2_relocated);
				_ND(tpv_compact_l2_reloc_ok,
				    "TPV: @STR: phase 2 reloc L1_idx=@LLU to ext=@LLU",
				    tpv->tpv_name, todo[i], l1_ref->extent_index);
			} else {
				_NW(tpv_compact_l2_reloc_err,
				    "TPV: @STR: phase 2 reloc L1_idx=@LLU rv=@INT",
				    tpv->tpv_name, todo[i], rv);
			}
		}
		kvfree(todo);
	}

	return done;
}
