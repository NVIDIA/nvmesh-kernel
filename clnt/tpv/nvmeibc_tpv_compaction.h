/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

/*
 * nvmeibc_tpv_compaction.h - offline TPV compaction (TPV_Trimming.md Step 4).
 *
 * Compaction relocates live TPV slots out of sparse CDV extents into
 * denser ones already owned by the same TPV, then returns the emptied
 * source extents via the existing Step 2 drain.
 *
 * Concurrency model:
 *   - Multiple relocation worker threads run in parallel, bounded by
 *     the caller-supplied aggressiveness.
 *   - A dedicated L2 writer thread owns all L2 leaf writes.  Requests
 *     targeting distinct L2 pages run in parallel inside the writer;
 *     requests targeting the same page are serialized.  No RMW race.
 *   - Relocation workers hand out (virt_idx, source_ref, dest_ref)
 *     triples under a per-TPV planner lock.
 *
 * Durability contract: the single synchronous L2 leaf write is the sole
 * commit point per relocation.  See TPV_Trimming.md Step 4 Protocol sketch.
 */

#ifndef NVMEIBC_TPV_COMPACTION_H
#define NVMEIBC_TPV_COMPACTION_H

#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>

struct nvmeibc_tpv;
struct nvmeibc_cdv_extent_ref;
struct plan_node;		/* defined privately in nvmeibc_tpv_compaction.c */

/* -- Public parameters passed in from RECOVER_START ---------------------- */

struct tpv_compaction_params {
	u32 aggressiveness;	/* max parallel slot relocations; default 4 */
};

/* -- Public progress snapshot (consumed by RECOVER_PROGRESS) ------------- */

struct tpv_compaction_progress {
	u64 relocated;			/* data slot relocations (phase 1) */
	u64 planned_relocations;	/* phase-1 planner target */
	u64 reclaimed_extents;		/* CDV extents returned to TOMA */
	u64 l2_relocated;		/* L2-table relocations (phase 2) */
};

/* -- Job lifecycle ------------------------------------------------------- */

enum tpv_compaction_state {
	TPV_COMPACTION_IDLE = 0,
	TPV_COMPACTION_RUNNING,
	TPV_COMPACTION_STOPPING,	/* abort flag set; workers draining */
	TPV_COMPACTION_DONE,
};

/*
 * One per attached TPV.  Embedded directly in struct nvmeibc_tpv so
 * there is no separate allocation to manage.  The entire struct is
 * zero-initialized at tpv attach; the L2 writer thread and workers
 * are spawned lazily on the first tpv_compaction_run.
 */
struct tpv_compaction_job {
	/* Bookkeeping */
	atomic_t			state;		/* enum tpv_compaction_state */
	atomic_t			abort_flag;	/* 1 = worker should stop */

	/*
	 * Shutdown flag set by tpv_compaction_destroy BEFORE any teardown.
	 * tpv_compaction_run and the /proc-write-triggered start work both
	 * check this under pending_starts accounting to avoid use-after-
	 * free when detach races with an async start.
	 */
	bool				shutdown;
	atomic_t			pending_starts;	/* queued-but-not-yet-run start works */
	wait_queue_head_t		pending_starts_wq;

	/*
	 * While a compaction run is active, the planner caches raw
	 * nvmeibc_cdv_extent_ref pointers in plan_sources.  A concurrent
	 * cdv_alloc_work draining pending_return_list could kfree one of
	 * those refs after a successful CDV_FREE_EXTENT send, leaving the
	 * planner with a dangling pointer.  defer_drain, set by
	 * tpv_compaction_run for the duration of the run, tells the
	 * allocator work to skip tpv_drain_pending_returns; emptied refs
	 * accumulate on pending_return_list and are drained once after the
	 * run ends.  The accumulation is bounded by the TPV's CDV extent
	 * count, so worst case is small.  Alloc phase 2 (request new
	 * CDV_extents from TOMA) is unaffected.
	 */
	bool				defer_drain;

	/* Progress counters - atomics so the /proc + RECOVER_PROGRESS paths
	 * can snapshot without grabbing the job mutex. */
	atomic64_t			relocated;		/* data-slot relocations */
	atomic64_t			planned_relocations;
	atomic64_t			reclaimed_extents;
	atomic64_t			l2_relocated;		/* L2-table relocations (phase 2) */

	/*
	 * Planner state - all protected by plan_mutex.
	 *
	 * plan_sources holds struct plan_node entries (defined in
	 * nvmeibc_tpv_compaction.c), sorted ascending by allocated_count
	 * (sparsest first, densest last).  head_pn / tail_pn walk the list:
	 *   head_pn advances forward  past ineligible sources     (is_l1,
	 *            full, or data_slots == 0).
	 *   tail_pn retreats backward past ineligible destinations (full,
	 *            or crossed past head_pn).
	 * Data flows head->tail only, so no extent ever serves as both src
	 * and dst within the same run -> no ping-pong.  Termination: head_pn
	 * == tail_pn, or either is NULL.
	 */
	struct mutex			plan_mutex;
	struct list_head		plan_sources;
	struct plan_node		*head_pn;
	struct plan_node		*tail_pn;

	/* Worker thread pool.  n_workers_running is atomic so the main
	 * thread (on spawn failure) and worker threads (on exit) can
	 * both decrement without a data race. */
	atomic_t			n_workers_running;
	struct completion		workers_done;

	/* L2 writer thread */
	struct task_struct		*l2_writer;
	struct list_head		l2_req_queue;	/* pending requests */
	spinlock_t			l2_req_lock;
	wait_queue_head_t		l2_req_wq;
	bool				l2_writer_should_stop;
	/* Per-page lock queue: list of "currently-servicing" page indices. */
	struct list_head		l2_inflight_pages;
};

/* -- Public API ---------------------------------------------------------- */

/*
 * Initialize the embedded job struct.  Called from the TPV attach path
 * exactly once, after the allocator is populated.
 */
void tpv_compaction_init(struct nvmeibc_tpv *tpv);

/*
 * Tear down the job, cancelling any in-flight run.  Called from TPV
 * detach.  Synchronous: returns only after all worker threads and the
 * L2 writer thread have exited.
 */
void tpv_compaction_destroy(struct nvmeibc_tpv *tpv);

/*
 * Run a compaction job to completion.  Blocks until the worker pool
 * drains and the L2 writer has committed every queued leaf write.
 * Returns 0 on normal completion or RECOVER_FINISH; -EINTR if aborted;
 * -EBUSY if a job is already running on this TPV.
 *
 * Progress is reported via the atomic counters in struct
 * tpv_compaction_job; callers (the IB admin RECOVER_PROGRESS path)
 * snapshot them periodically.
 */
int  tpv_compaction_run(struct nvmeibc_tpv *tpv,
			const struct tpv_compaction_params *p);

/*
 * Request abort.  Sets the abort flag; workers check it at every
 * relocation boundary.  Returns immediately; caller uses
 * tpv_compaction_run's return to confirm termination.
 */
void tpv_compaction_abort(struct nvmeibc_tpv *tpv);

/*
 * Kick a compaction run on the system workqueue.  Non-blocking; used by
 * the attach path when the incoming attach carries isCompaction=true.
 * Returns 0 on queue, -ESHUTDOWN if the TPV is tearing down, -ENOMEM on
 * alloc failure.  Composes with tpv_compaction_destroy via the same
 * shutdown / pending_starts handshake as the /proc-write start path.
 */
int  tpv_compaction_kick(struct nvmeibc_tpv *tpv,
			 const struct tpv_compaction_params *p);

/*
 * Called from the TPV detach path.  Signals any in-flight run to stop;
 * non-blocking.  tpv_compaction_destroy (called shortly after) waits
 * for all threads to exit.
 */
void nvmeibc_tpv_abort_any_compaction_for(struct nvmeibc_tpv *tpv);

/*
 * Snapshot the progress counters.  Safe to call from any context.
 */
void tpv_compaction_get_progress(struct nvmeibc_tpv *tpv,
				 struct tpv_compaction_progress *out);

/*
 * Returns true iff cdv_alloc_work should skip draining pending_return_list
 * because a compaction run is actively caching ref pointers in its planner.
 * Callable with no locks held.
 */
bool tpv_compaction_drain_deferred(struct nvmeibc_tpv *tpv);

/* -- Relocation primitive (exported for unit tests) --------------------- */

/*
 * Relocate virt_idx from a slot in source_ref to a free slot in
 * dest_ref.  Returns 0 on commit (L2 leaf write durable), -EAGAIN if
 * the virt_idx was not mapped to source_ref after all (racey discard),
 * -EIO on CDV I/O failure, -EINTR if the abort flag is set before
 * commit.
 *
 * Preconditions:
 *   - source_ref and dest_ref are both on tpv->allocator.cdv_extent_list.
 *   - dest_ref has at least one free slot (caller checked).
 *   - virt_idx is currently mapped to a slot in source_ref.
 *
 * Postconditions on success:
 *   - L2 leaf for virt_idx on CDV points at the dest slot (durable).
 *   - xarray entry for virt_idx has phys_offset / cdv_extent_index of
 *     the dest slot.
 *   - source_ref->allocated_count decremented; source slot back in
 *     source_ref's free pool.
 */
int tpv_reloc_one(struct nvmeibc_tpv *tpv,
		  u64 virt_idx,
		  struct nvmeibc_cdv_extent_ref *source_ref,
		  struct nvmeibc_cdv_extent_ref *dest_ref);

/* -- L2 writer API (internal but declared for selftests) ----------------- */

/*
 * A single leaf-change request handed to the L2 writer thread.  Lives on
 * tpv_compaction_job.l2_req_queue.  The thread serializes requests
 * sharing an L2 page via tpv_compaction_job.l2_inflight_pages.
 */
struct tpv_l2_write_req {
	u64 virt_idx;			/* which TPV slot is being remapped */
	u64 new_cdv_offset;		/* TPV_TREE_NULL if clearing */
	struct completion done;
	int result;			/* 0 on success, <0 on CDV I/O failure */
	struct list_head q_node;	/* on l2_req_queue */
	/* Bookkeeping used inside the writer thread: */
	u64 l1_idx;			/* covering L1 slot */
	u64 l2_page;			/* covering L2 page index */
	struct list_head inflight_node;	/* on l2_inflight_pages when active */
};

/*
 * Submit a leaf-change request.  Blocks until the L2 writer has issued
 * a durable CDV write for the covering L2 page.
 *
 * Exposed so kernel self-tests can exercise same-page serialization
 * and cross-page parallelism directly, without going through the
 * full relocation pipeline.
 */
int tpv_l2_writer_submit_and_wait(struct nvmeibc_tpv *tpv,
				  u64 virt_idx,
				  u64 new_cdv_offset);

#endif /* NVMEIBC_TPV_COMPACTION_H */
