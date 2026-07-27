#ifndef NVMEIBT_WQ_H_INCLUDED
#define NVMEIBT_WQ_H_INCLUDED

#include "nvmeibt_ds.h"

/*
 * Generic WorkQueues
 * ==================
 *
 * The workqueue provides a generic mechanism to execute work in a separate
 * thread, so that such work will not block/delay the main TOMA thread flow.
 * Most commonly it is used for operations involving the filesystem.
 *
 * Work is described by "struct nvmeibt_wq_entry" (aka: wq_entry, or entry).
 *  - Workqueues can be created and destroyed.
 *  - Entries are added to a specific workqueue.
 *  - The entries in a workqueue are executed in-order of arrival.
 *
 * API:
 *  - nvmeibt_wq_create(name): create a new workqueue
 *  - nvmeibt_wq_destroy(wq): destory a given workqueue
 *  - nvmeibt_wq_addw(wq, entry): add entry to a workqueue
 *  - nvmeibt_wq_setw(wq, entry): replace (abort) all entries with one new
 *  - nvmeibt_wq_delw(wq, entry): remove an entry from a workqueue
 *  - nvmeibt_wq_flush(wq): force execute of all entries in workqueue
 *  - nvmeibt_wq_drain(wq): abort all entries currently in the workqueue
 *  - nvmeibt_wq_run_once(entry): run entry once with temporary adhoc workqueue
 *
 * Entries have the following callbacks:
 *  - wq_entry->execute():
 *      called to execute the work.
 *      runs in the wq thread context.
 *      the callee is responsible to free the wq_entry if needed.
 *  - wq_entry->abort():
 *      called for entires not executed (e.g. due to drain request)
 *      runs in the wq thread context.
 *      the callee is repsonsible to free the wq_entry if needed.
 *  - wq_entry->finalize():
 *      user-defined post-exec function (SEE NOTE BELOW)
 *      runs in the main toma thread context.
 *  - wq_entry->free():
 *      user-defined free function (SEE NOTE BELOW)
 *      runs in the either context (toma main thread, or wq thread)
 *
 * How to use:
 *  Typicailly workqueue users create a container struct with state, e.g.:
 *     struct struct dumper_wq_entry {
 *         struct nvmeibt_wq_entry  wq_entry;
 *         struct nvmeibt_Str       *csv_ctx;
 *     };
 *  and then use a macro to get the container struct from the wq_entry:
 *    de = container_of(wq_entry, struct dumper_wq_entry, wq_entry);
 *
 *
 * IMPORTANT NOTE ABOUT toma_wakeup:
 *
 * Disclaimer: this note should move to nvmeibt_toma.[ch] next to toma_wakuep
 * code. The code will change soon to hide/wrap the 'finalize' logic.
 *
 * The toma_wakeup extends workqueues to execute extra-work in the main thread
 * after the entry was executed in the workqueue thread. Put another way: once
 * the entry executes, it notifies TOMA mainthread to run some finalize work.
 *
 * Users of toma_wakeup must arrange that:
 *  - wq_entry->execute() will invoke nvmeibt_toma_trigger_wakeup() - with an explicit
 *    call at the end of the callback.
 *  - wq_entry->abort() will invoke nvmeibt_toma_trigger_wakeup() too - by setting the
 *    callback to generic nvmeibt_toma_wakeup_wq_abort_func().
 * This ensures that ->finalize() is always called, and in turn:
 *  - tests for wq_entry->is_canceled (i.e. ->execute() did not run)
 *  - performs whatever work needed in the context of toma main thread
 *  - frees the wq_entry
 *
 * All entries (executed or not) will thus run ->finalize() and ->free() in
 * toma main thread. The test for wq_entry->is_canceled indicates whether the
 * wq_entry was not executed.
 */

struct nvmeibt_wq;
struct nvmeibt_wq_entry;

typedef void (*nvmeibt_wq_entry_cb_t)(struct nvmeibt_wq_entry *);

struct nvmeibt_wq_entry {
	const char				*type;
	nvmeibt_wq_entry_cb_t	execute;		/* callback: primary execution */
	nvmeibt_wq_entry_cb_t	finalize;		/* callback: finalize execution */
	nvmeibt_wq_entry_cb_t	abort;			/* callback: abort execution */
	nvmeibt_wq_entry_cb_t	free;			/* callback: cleanup */
	BOOL 					is_canceled;	/* entry not executed */
	struct nvmeibt_wq 		*wq;
	struct nvmeibt_wq_entry	*once_wq_entry;	/* if submitted via run_once, stores the pointer to the once_wq_entry */
	unsigned int			last_CHANGE_no; /* Needed for WQ's that chain as a result other WQ's to continue their logical execution*/
	struct xdlist 			link;
};

void nvmeibt_wq_cancel_entry(struct nvmeibt_wq_entry *e);
struct nvmeibt_wq *nvmeibt_wq_create(const char *name);
void nvmeibt_wq_destroy(struct nvmeibt_wq *wq);
int nvmeibt_wq_addw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *entry);
int nvmeibt_wq_setw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *entry);
int nvmeibt_wq_delw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *entry);
void nvmeibt_wq_flush(struct nvmeibt_wq *wq);
void nvmeibt_wq_drain(struct nvmeibt_wq *wq);
struct nvmeibt_wq *nvmeibt_wq_run_once(struct nvmeibt_wq_entry *entry);
void nvmeibt_wq_stuck_pthread_check(void);

#endif

