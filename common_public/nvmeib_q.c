/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"

#include "nvmeib.h"
#include "nvmeib_public.h"
#include "nvmeib_q.h"
#include "nvmeib_utils.h"
#include "kth/nvmeib_public_kth_sync.h"
#include "nvmeibp_trace.h"

#define MAX_WQ_PROCESSING_TIME_JIF (3 * HZ)
ulong nvmeib_wq_max_processing_time = MAX_WQ_PROCESSING_TIME_JIF;
module_param_named(wq_max_processing_time, nvmeib_wq_max_processing_time, ulong, 0644);
MODULE_PARM_DESC(wq_max_processing_time, "Maximum wq processing time before doing rescheduling self in jiffies"); // YR: TODO: this should be moved to msec

int nvmeib_wq_trace_debug_level = NVMEIB_Q_LOG_LEVEL_VERBOSE; /* TRACE */
module_param_named(tracer_wq_debug_level, nvmeib_wq_trace_debug_level, int, 0644);
MODULE_PARM_DESC(tracer_wq_debug_level, "Set trace level for nvmib_q");

#define nvmeib_debug_level nvmeib_public_debug_level

#define NVMEIB_Q_LOG(q, level, level_num, name, fmt, ...) \
	do { \
		if (!q || level_num <= q->log_level) \
			_N##level##_SCOPE(name, nvmeib_wq_trace, "wq=@PID: " fmt, nvmeib_qpid(q), ##__VA_ARGS__); \
	} while(0);

#define NVMEIB_Q_ERROR(q, name, fmt, ...) NVMEIB_Q_LOG(q, E, 1, name, fmt, ##__VA_ARGS__)
#define NVMEIB_Q_WARN(q, name, fmt, ...) NVMEIB_Q_LOG(q, W, 2, name, fmt, ##__VA_ARGS__)
#define NVMEIB_Q_INFO(q, name, fmt, ...) NVMEIB_Q_LOG(q, I, 3, name, fmt, ##__VA_ARGS__)
#define NVMEIB_Q_TRACE(q, name, fmt, ...) NVMEIB_Q_LOG(q, T, 4, name, fmt, ##__VA_ARGS__)
#define NVMEIB_Q_DEBUG(q, name, fmt, ...) NVMEIB_Q_LOG(q, D, 5, name, fmt, ##__VA_ARGS__)
#define NVMEIB_Q_FINE(q, name, fmt, ...) NVMEIB_Q_LOG(q, F, 6, name, fmt, ##__VA_ARGS__)

struct nvmeib_q {
	spinlock_t lock;
	struct list_head list;
	struct nvmeib_qent *current_entry;
	struct task_struct *thread;
	wait_queue_head_t waitq;
	struct completion flush_complete;
	bool busy;
	int pid;
	unsigned int log_level;
#ifdef DEBUG_NVMEIB_Q
	unsigned long wrk_idx;
#endif
	unsigned long size;
};

inline static void _del_entry_list(struct nvmeib_q *q, struct nvmeib_qent *entry) {
	list_del_init(&entry->link);
	--(q->size);
}


inline static void _add_entry_list(struct nvmeib_q *q, struct nvmeib_qent *entry) {
	list_add_tail(&entry->link, &q->list);
	++(q->size);
}

inline static void _add_entry_list_head(struct nvmeib_q *q, struct nvmeib_qent *entry) {
	list_add(&entry->link, &q->list);
	++(q->size);
}

inline static void _add_entry_after(struct nvmeib_q *q, struct nvmeib_qent *after, struct nvmeib_qent *entry) {
	list_add(&entry->link, &after->link);
	++(q->size);
}

static int qthread_func(void *arg)
{
	struct nvmeib_q *q = arg;
	struct nvmeib_qent *entry;
	long work_start_jif, work_dt;
	long loop_start_jif, loop_dt, now, work_idx;
	unsigned long flags;
	void (*entry_fn)(struct nvmeib_qent *);

	NFIN;
	while (!kthread_should_stop()) {
		wait_event_interruptible(q->waitq,
			!list_empty(&q->list) || kthread_should_stop());
		loop_start_jif = jiffies; /* we dont catch the corner case where right
		before we decided to wait/sleep (list was empty) new work was added */
		spin_lock_irqsave(&q->lock, flags);
		q->busy = true;
		while (!list_empty(&q->list)) {
			entry = container_of(q->list.next, struct nvmeib_qent,
					     link);
			_del_entry_list(q, entry);
			q->current_entry = entry;
			spin_unlock_irqrestore(&q->lock, flags);
			work_start_jif = jiffies;
			entry_fn = entry->f;

			#ifdef DEBUG_NVMEIB_Q
				work_idx = entry->idx;
			#else
				work_idx = 0;
			#endif

			NVMEIB_Q_TRACE(q, trace_nvmeib_q_qthread_func_start,
						   "--> work-idx=@LONG, work-func='@CALLBACK'",
						   work_idx, entry_fn);
			(*entry->f)(entry);
			now = jiffies;
			work_dt = now - work_start_jif;
			loop_dt = now - loop_start_jif;
			if (work_dt >= MAX_WQ_PROCESSING_TIME_JIF ||
				loop_dt >= MAX_WQ_PROCESSING_TIME_JIF ) {
				NVMEIB_Q_WARN(q, trace_1_nvmeib_q_qthread_func,
					"wq processsing time exceeded, loop_dt=@DURATION ms, "
					"work @ENTRY_FN (@ENTRY), work_dt=@DURATION ms",
				   loop_dt, entry_fn, entry, work_dt);
				cond_resched();
				loop_start_jif = jiffies;
			}
			NVMEIB_Q_TRACE(q, trace_nvmeib_q_qthread_func_end,
						   "<-- work-idx=@LONG, work-func='@CALLBACK' "
						   "(work_dt=@DURATION ms, loop_dt=@DURATION ms)",
						   work_idx, entry_fn, work_dt, loop_dt);
			spin_lock_irqsave(&q->lock, flags);
			q->current_entry = NULL;
		}
		q->busy = false;
		if (!completion_done(&q->flush_complete))
			complete_all(&q->flush_complete);
		spin_unlock_irqrestore(&q->lock, flags);
	}
	NFOUT;
	return 0;
}

struct flush_qent {
	struct nvmeib_qent entry;
	struct completion *comp;
};

static void __flush_qent_work_fn(struct nvmeib_qent *entry)
{
	struct flush_qent *flush_qent = container_of(entry, struct flush_qent, entry);
	complete(flush_qent->comp);
}

static bool __cancel_and_flush_qent(struct nvmeib_q *q, struct nvmeib_qent *entry, bool try_cancel)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	struct flush_qent flush_qent = {
		.entry = {
			.f = __flush_qent_work_fn,
		},
		.comp = &comp,
	};
	bool wait_comp = false;
	bool ret = false;
	unsigned long flags;
	
	NFIN;
	spin_lock_irqsave(&q->lock, flags);
	if (q->current_entry == entry) {
		/* Currently running our entry. Put the flush work at the head of the list */
		NVMEIB_Q_DEBUG(q, trace_cancel_and_flush_qent_current_wait,
			       "Entry @ENTRY with FN @ENTRY_FN currently running. Waiting for it to finish",
				entry, entry->f);
		_add_entry_list_head(q, &flush_qent.entry);
		wait_comp = true;
		ret = true;
	} else if (!list_empty(&entry->link)) {
		/* Our entry is in the list. Either remove it (try_cancel = true) or put the flush work after it */
		if (try_cancel) {
			NVMEIB_Q_DEBUG(q, trace_cancel_and_flush_qent_sched_cancel,
				       "Entry @ENTRY with FN @ENTRY_FN scheduled. Removing from the list",
					entry, entry->f);
			_del_entry_list(q, entry);
		} else {
			NVMEIB_Q_DEBUG(q, trace_cancel_and_flush_qent_sched_wait,
				       "Entry @ENTRY with FN @ENTRY_FN scheduled. Waiting for it to run",
					entry, entry->f);
			_add_entry_after(q, entry, &flush_qent.entry);
			wait_comp = true;
		}
		ret = true;
	} else {
		NVMEIB_Q_DEBUG(q, trace_cancel_and_flush_qent_not_sched,
			       "Entry @ENTRY with FN @ENTRY_FN not scheduled",
				entry, entry->f);
	}
	spin_unlock_irqrestore(&q->lock, flags);
	if (wait_comp) {
		int i, wait_rv;
		for (i = 0;; i++) {
			if ((wait_rv = wait_for_completion_timeout(&comp, 5 * HZ)) > 0)
				break;
			NVMEIB_Q_ERROR(q, error_cancel_and_flush_qent_wait_fail,
				       "Failed (rvw @RVW) waiting for flush completion. Attempt #@NUM_RETRY_ATTEMPTS",
				       wait_rv, i);
			WARN_ON_ONCE(1);
		}
	}
	NFOUT;
	return ret;
}

/* 
 * Cancels a single entry from the workqueue (or waits for it to finish) 
 * Returns: True if entry was scheduled
 */
bool nvmeib_cancel_qent(struct nvmeib_q *q, struct nvmeib_qent *entry)
{
	NVMEIB_Q_TRACE(q, trace_nvmeib_cancel_qent,
		       "Cancelling Entry @ENTRY with FN @ENTRY_FN",
		entry, entry->f);
	return __cancel_and_flush_qent(q, entry, true);
}
EXPORT_SYMBOL(nvmeib_cancel_qent);

/* 
 * Flushes a single entry from the workqueue (rather than flushing the whole queue) 
 * Returns: True if entry was scheduled
 */
bool nvmeib_flush_qent(struct nvmeib_q *q, struct nvmeib_qent *entry)
{
	NVMEIB_Q_TRACE(q, trace_nvmeib_flush_qent,
		       "Flushing Entry @ENTRY with FN @ENTRY_FN",
		entry, entry->f);
	return __cancel_and_flush_qent(q, entry, false);
}
EXPORT_SYMBOL(nvmeib_flush_qent);

void nvmeib_flushq(struct nvmeib_q *q)
{
	unsigned long flags;

	NFIN;
	NVMEIB_Q_TRACE(q, trace_nvmeib_q_nvmeib_flushq, "Flushing");
	spin_lock_irqsave(&q->lock, flags);
	if (!list_empty(&q->list) || q->busy) {
		reinit_completion(&q->flush_complete);
		spin_unlock_irqrestore(&q->lock, flags);
		wait_for_completion(&q->flush_complete);
	}
	else
		spin_unlock_irqrestore(&q->lock, flags);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_flushq);

void nvmeib_drainq(struct nvmeib_q *q)
{
	NFIN;
	do {
		nvmeib_flushq(q);
	} while (!list_empty(&q->list) || q->busy);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_drainq);

struct nvmeib_q *nvmeib_startq(const char *name, unsigned int cpu)
{
	struct nvmeib_q *q = kzalloc(sizeof *q, GFP_KERNEL);

	NFIN;
	if (IS_ERR_OR_NULL(q)) {
		NVMEIB_Q_ERROR(q, error_nvmeib_q_nvmeib_startq, "Failed to allocate queue: @NAME", name);
	} else if (IS_ERR(q->thread = kthread_create(
				qthread_func, q, "%s", name))) {
		kfree(q);
		q = NULL;
		NVMEIB_Q_ERROR(q, error_1_nvmeib_q_nvmeib_startq, "Failed to create thread for q: @NAME", name);
	} else {
		spin_lock_init(&q->lock);
		INIT_LIST_HEAD(&q->list);
		init_completion(&q->flush_complete);
		init_waitqueue_head(&q->waitq);
		q->busy = false;
		q->pid = q->thread->pid;
		q->log_level = NVMEIB_Q_LOG_LEVEL_DEFAULT; /* WARN */
		NVMEIB_Q_TRACE(q, trace_1_nvmeib_q_nvmeib_startq, "Created wq: @NAME (@QUEUE)", name, q);
		if (cpu < NR_CPUS)
			kthread_bind(q->thread, cpu);
		wake_up_process(q->thread);
	}

	NFOUT;
	return q;
}
EXPORT_SYMBOL(nvmeib_startq);

void nvmeib_endq(struct nvmeib_q *q)
{
	NFIN;
	if (q) {
		NVMEIB_Q_TRACE(q, trace_nvmeib_q_nvmeib_endq, "Ending queue");
		kthread_stop(q->thread);
		kfree(q);
	} else {
		NVMEIB_Q_WARN(q, trace_nvmeib_q_nvmeib_endq_empty, "Ending a non-existent queue");
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_endq);

bool nvmeib_addq(struct nvmeib_q *q, struct nvmeib_qent *entry
#ifdef DEBUG_NVMEIB_Q
		, void *add_fn, void *add_fn_prev, void *add_fn_prev2
#endif
		)
{
	ulong flags;
	bool empty;
	long wrk_idx = 0;

	if (!list_empty(&entry->link)) {
		NFOUT;
		return false;
	}

#ifdef DEBUG_NVMEIB_Q
	/* This is used for debugging so we can determine which function added
	this entry to the Work Queue */
	entry->add_fn = add_fn;
	entry->add_fn_prev = add_fn_prev;
	entry->add_fn_prev2 = add_fn_prev2;
#endif

	spin_lock_irqsave(&q->lock, flags);
	empty = list_empty(&q->list);

#ifdef DEBUG_NVMEIB_Q
	entry->idx = wrk_idx = q->wrk_idx++;
#endif

	_add_entry_list(q, entry);
	NVMEIB_Q_TRACE(q, trace_nvmeib_q_nvmeib_add,
				   "Added work-func=@CALLBACK, work-idx=@QUEUE_E_IDX, q_size=@LONG "
#ifdef DEBUG_NVMEIB_Q
				   "(called from @ADD_FN_PTR<-@ADD_FN_PREV_PTR<-@ADD_FN_PREV2_PTR)"
#endif
				   , (*entry->f), wrk_idx, q->size
#ifdef DEBUG_NVMEIB_Q
				   , add_fn, add_fn_prev, add_fn_prev2
#endif
				   );
	spin_unlock_irqrestore(&q->lock, flags);
	if (empty)
		wake_up(&q->waitq);
	return true;
}
EXPORT_SYMBOL(nvmeib_addq);

bool nvmeib_delq(struct nvmeib_q *q, struct nvmeib_qent *entry)
{
	ulong flags;
	bool found = false;

	NFIN;
	spin_lock_irqsave(&q->lock, flags);
	if ((found = !list_empty(&entry->link)))
		_del_entry_list(q, entry);
	spin_unlock_irqrestore(&q->lock, flags);
	NFOUT;
	return found;
}
EXPORT_SYMBOL(nvmeib_delq);

int nvmeib_qpid(struct nvmeib_q *q)
{
	return q ? q->pid : -1;
}
EXPORT_SYMBOL(nvmeib_qpid);

void nvmeib_q_set_log_level(struct nvmeib_q *q, unsigned int level) {
	q->log_level = level;
}
EXPORT_SYMBOL(nvmeib_q_set_log_level);
