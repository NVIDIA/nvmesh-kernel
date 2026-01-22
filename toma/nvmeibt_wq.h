/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_WQ_H_INCLUDED
#define NVMEIBT_WQ_H_INCLUDED

#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
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
 * How to use:
 *  Typically workqueue users create a container struct with state, e.g.:
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
 * All entries (executed or not) will thus run ->finalize() and ->free() in
 */
struct nvmeibt_wq;
struct nvmeibt_wq_entry;

typedef void (*nvmeibt_wq_entry_cb_t)(struct nvmeibt_wq_entry *);	// Callback for code execution of work

struct nvmeibt_wq_entry {
	const char				*type;			/* Params: name */
	nvmeibt_wq_entry_cb_t	execute;		/* Params: primary execution, runs in the wq thread context, at the end should wakeup Toma main thread */
	nvmeibt_wq_entry_cb_t	finalize;		/* Params: finalize execution, called on Toma main thread */
	nvmeibt_wq_entry_cb_t	abort;			/* Params: abort execution, runs in the wq thread context, at the end should wakeup Toma main thread. Called for entires not executed (e.g. due to drain request) */
	nvmeibt_wq_entry_cb_t	free;			/* Params: cleanup and mem-free. runs in the either context (toma main thread, or wq thread) */
	bool 					is_canceled;	/* Return: was entry not executed, should be tested by caller during finalize() */

	struct nvmeibt_wq 		*wq;			/* Temp: During exec, Points to work queue where task is executed */
	struct xdlist 			link;			/* Temp: During exec, Attach to work queue linked list */

	// User extension (extra params, below). Not part of basic work-queue design
	struct nvmeibt_wq_entry	*chained;		/* Allow work to depend on another work. (Used when work submitted via run_once, stores the pointer to the once_wq_entry) */
	unsigned int			last_CHANGE_no; /* Needed for WQ's that chain as a result other WQ's to continue their logical execution*/
};

const char* nvmeibt_wq_get_name(const struct nvmeibt_wq *wq);

/* create a new workqueue */
struct nvmeibt_wq *nvmeibt_wq_create(const char *name);

/* destroy a given workqueue */
void nvmeibt_wq_destroy(struct nvmeibt_wq *wq);

/* add entry to a workqueue */
int nvmeibt_wq_addw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *);

/* replace (abort) all (non executed) entries with one new */
int nvmeibt_wq_setw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *);

//int nvmeibt_wq_delw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *);
//void nvmeibt_wq_cancel_entry(struct nvmeibt_wq_entry *e);

/* force execute of all entries in workqueue */
void nvmeibt_wq_flush(struct nvmeibt_wq *wq);

/* abort all entries currently in the workqueue */
void nvmeibt_wq_drain(struct nvmeibt_wq *wq);

/* run entry once with temporary newly created adhoc workqueue */
struct nvmeibt_wq *nvmeibt_wq_run_once(struct nvmeibt_wq_entry *);

/* Attempt to join expired run once work queues*/
void nvmeibt_wq_stuck_pthread_check(void);

#endif

