/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_debug.h"
#include <pthread.h>
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_wq.h"

/*
 * Generic WorkQueues:
 *
 * The workqueue provides a generic mechanism to execute work in a separate
 * thread, so that such work will not block/delay the main TOMA thread flow.
 * Most commonly it is used for operations involving the filesystem.
 *
 * See nvmeibt_wq.h for details on how workqueues operate, APIs and semantics.
 */

#define N__D(__name__, fmt, ...) N_Df(__name__, "(wq=@WQ_NAME) " fmt, ((wq && wq->name) ? wq->name : "?"), ## __VA_ARGS__)
#define N__W(__name__, fmt, ...) N_Wf(__name__, "(wq=@WQ_NAME) " fmt, ((wq && wq->name) ? wq->name : "?"), ## __VA_ARGS__)
#define N__E(__name__, fmt, ...) N_Ef(__name__, "(wq=@WQ_NAME) " fmt, ((wq && wq->name) ? wq->name : "?"), ## __VA_ARGS__)

#define __NFIN  N__D(__AUTOID__, "-->")
#define __NFOUT N__D(__AUTOID__, "<--")

/* wait for wakeup timeout in millisecod */
#define WAIT_WAKEUP_TIMEOUT 5000

typedef XDLIST_DECLARE(wq_entries_list, struct nvmeibt_wq_entry, link) wq_entries_list_t;

struct nvmeibt_wq {
	char *name;
	pthread_t thr;
	pthread_mutex_t guard;
	/* aligned to prevent cache-line split lock in pthread_cond_wait/signal */
	pthread_cond_t wakeup __attribute__((aligned(64)));
	wq_entries_list_t entries1;
	wq_entries_list_t entries2;
	wq_entries_list_t *entries;
	int cont;
	BOOL	is_destroying_wq;
	BOOL	is_one_time;
};

static int lock(struct nvmeibt_wq *wq)
{
	int rv;

	if ((rv = pthread_mutex_lock(&wq->guard)) != 0) {
		N__E(error_wq_lock, "Failed to lock wq guard @AUTO_ERRNO");
		rv = -1;
	}
	return rv;
}

static int wakeup(struct nvmeibt_wq *wq)
{
	int rv;

	if ((rv = pthread_cond_signal(&wq->wakeup)) != 0) {
		N__E(error_wq_wakeup, "Failed to signal wq cond @AUTO_ERRNO");
		rv = -1;
	}
	return rv;
}

static int unlock(struct nvmeibt_wq *wq)
{
	int rv;

	if ((rv = pthread_mutex_unlock(&wq->guard)) < 0) {
		N__E(error_wq_unlock, "Failed to unlock wq guard @AUTO_ERRNO");
		rv = -1;
	}
	return rv;
}

void nvmeibt_wq_cancel_entry(struct nvmeibt_wq_entry *e)
{
	if (e) {
		e->is_canceled = true;
		if (e->abort) {
			e->abort(e);
		} else if (e->free) {
			e->free(e);
		}
	}
}

static int timed_wait(struct nvmeibt_wq *wq, int ms)
{
	pthread_cond_t *cond = &wq->wakeup;
	pthread_mutex_t *m = &wq->guard;
	struct timespec ts;
	long ns = MSEC_TO_NSEC(ms);
	int rv;

	if (ms) {
		getnstimeofday(&ts);
		timespec_update_by_a_few_nsec(&ts, ns);
		if ((rv = pthread_cond_timedwait(cond, m, &ts)) != 0 &&
			rv != ETIMEDOUT) {
			N__E(error_wq_timed_wait, "Failed to wait for wq cond @RV", rv);
			rv = -1;
		}
		else {
			rv = 0;
		}
	}
	else {
		if ((rv = pthread_cond_wait(cond, m)) != 0) {
			N__E(error_1_wq_timed_wait, "Failed to wait for wq cond @AUTO_ERRNO");
			rv = -1;
		}
	}
	return rv;
}

static int exec_entries(struct nvmeibt_wq *wq, wq_entries_list_t *entries)
{
	struct nvmeibt_wq_entry *p;
	int rv = 0;

	__NFIN;
	while (!rv && !XDLIST_EMPTY(entries)) {
		p = XDLIST_FIRST(entries);
		XDLIST_DEL(&p->link);
		p->wq = wq;
		if (wq->is_destroying_wq) {
			nvmeibt_wq_cancel_entry(p);
			continue;
		}
		NTOMA_ASSERT(error_wq_exec_entries, p != NULL, "wq entry is NULL, list corruption!");
		p->execute(p);
	}
	__NFOUT;
	return rv;
}

static void delete_wq_thread(struct nvmeibt_wq *wq)
{
	__NFIN;
	if (pthread_cond_destroy(&wq->wakeup)) {
		N_Ef(xx_40, "pthread_cond_destroy failed @AUTO_ERRNO");
	}
	if (pthread_mutex_destroy(&wq->guard)) {
		N_Ef(xx_41, "pthread_mutex_destroy failed @AUTO_ERRNO");
	}
	NNVMEIBT_TOMA_FREE(trace_wq_delete_wq_thread, wq->name);
	NNVMEIBT_TOMA_FREE(trace_1_wq_delete_wq_thread, wq);
	__NFOUT;
}

static void *wq_func(void *arg)
{
	struct nvmeibt_wq *wq = arg;
	wq_entries_list_t *entries;

	__NFIN;
	while (wq->cont) {
		if (lock(wq) < 0) {
			goto out;
		}
		while (wq->cont && XDLIST_EMPTY(wq->entries)) {
			if (timed_wait(wq, WAIT_WAKEUP_TIMEOUT) < 0) {
				unlock(wq);
				goto out;
			}
		}
		entries = wq->entries;
		wq->entries = entries == &wq->entries1 ? &wq->entries2 : &wq->entries1;
		if (unlock(wq) < 0) {
			goto out;
		}
		if (entries && (exec_entries(wq, entries) < 0)) {
			N__E(error_wq_wq_func, "Failed to process wq requests");
			goto out;
		}
	}
	if (!lock(wq)) {
		if (wq->entries) {
			exec_entries(wq, wq->entries);
		}
		unlock(wq);
	}


out:
	__NFOUT;
	return NULL;
}

/*
 * nvmeibt_wq_create(): create a new workqueue.
 * returns: pointer to new wq
 */
struct nvmeibt_wq *nvmeibt_wq_create(const char *name)
{
	struct nvmeibt_wq *wq = NULL;
	int len;
	pthread_condattr_t cattr;
	pthread_attr_t tattr;

	NNVMEIBT_TOMA_POSIX_MEMALIGN(trace_wq_nvmeibt_wq_create, (void **)&wq, 64, sizeof(*wq));
	if (wq)
		memset(wq, 0, sizeof(*wq));
	if ((len = strlen(name)) > 0) {
		wq->name = NNVMEIBT_TOMA_CALLOC(trace_1_wq_nvmeibt_wq_create, len + 1, 1);
		nvmeibt_strlcpy(wq->name, name, len + 1);
	}
	__NFIN;
	wq->cont = true;
	if (pthread_mutex_init(&wq->guard, NULL) != 0) {
		N__E(error_wq_nvmeibt_wq_create, "wq=@WQ_NAME failed to create wq guard @AUTO_ERRNO", wq->name);
		goto free_exec;
	}
	if (pthread_condattr_init(&cattr) != 0) {
		N__E(error_1_wq_nvmeibt_wq_create, "wq=@WQ_NAME failed to create wq cond var attr @AUTO_ERRNO", wq->name);
		goto free_guard;
	}
	if (pthread_cond_init(&wq->wakeup, &cattr) != 0) {
		N__E(error_2_wq_nvmeibt_wq_create, "wq=@WQ_NAME failed to create wq cond var @AUTO_ERRNO", wq->name);
		goto free_guard;
	}
	XDLIST_HEAD_INIT(&wq->entries1);
	XDLIST_HEAD_INIT(&wq->entries2);
	wq->entries = &wq->entries1;

	if (pthread_attr_init(&tattr) != 0) {
		N__E(error_3_wq_nvmeibt_wq_create, "wq=@WQ_NAME failed to init wq thread attr @AUTO_ERRNO", wq->name);
		goto free_cond;
	}
	if (pthread_create(&wq->thr, &tattr, wq_func, wq) != 0) {
		N__E(error_4_wq_nvmeibt_wq_create, "wq=@WQ_NAME failed to start the wq thread @AUTO_ERRNO", wq->name);
		goto free_cond;
	}
	{
		char t_name[24];	// Linux allows thread names of at most 16[b]
		snprintf(t_name, sizeof(t_name), "wq_%.12s", wq->name);
		pthread_setname_np(wq->thr, t_name);
		N_Tf(trace_2_wq_nvmeibt_wq_create, "created wq=@WQ_NAME with thr=@PTHREAD", wq->name,  wq->thr);
		goto out;
	}

free_cond:
	pthread_cond_destroy(&wq->wakeup);

free_guard:
	pthread_mutex_destroy(&wq->guard);

free_exec:
	NNVMEIBT_TOMA_FREE(trace_3_wq_nvmeibt_wq_create, wq->name);
	NNVMEIBT_TOMA_FREE(trace_4_wq_nvmeibt_wq_create, wq);

out:
	__NFOUT;
	return wq;
}


// Stuck WQ thread handling: we don't want pthread_join to block the main thread.
struct stuck_pthread_t {
	struct xdlist link;
	struct nvmeibt_wq *wq;
};
static XDLIST_DECLARE(, struct stuck_pthread_t, link) stuck_pthread_list = XDLIST_INIT(stuck_pthread_list);

void nvmeibt_wq_stuck_pthread_add(struct nvmeibt_wq *wq)
{
	struct stuck_pthread_t *x = NNVMEIBT_TOMA_MALLOC(trace_stuck_pthread_1, sizeof(struct stuck_pthread_t));
	x->wq = wq;
	XDLIST_ADD_TAIL(&stuck_pthread_list, x);
	N_Tf(trace_stuck_pthread_4, "Added stuck pthread thr=@PTHREAD", x->wq->thr);
}

void nvmeibt_wq_stuck_pthread_check(void)
{
	struct stuck_pthread_t *x;
	XDLIST_FOREACH_SAFE(x, &stuck_pthread_list) {
		void *th_rv;
		int rc = pthread_tryjoin_np(x->wq->thr, &th_rv);

		if (rc == 0) {
			N_Tf(trace_stuck_pthread_3, "Finally joined pthread thr=@PTHREAD", x->wq->thr);
			XDLIST_DEL(&x->link);
			delete_wq_thread(x->wq);
			x->wq = NULL;
			NNVMEIBT_TOMA_FREE(trace_stuck_pthread_2, x);
		}
	}
}

/*
 * nvmeibt_wq_destory(): destroy a workqueue.
 * returns: none
 */
void nvmeibt_wq_destroy(struct nvmeibt_wq *wq)
{
	void	*th_rv;
	int		jrv;

	__NFIN;
	if (!wq) {
		goto out;
	}
	wq->cont = false;
	wakeup(wq);

	if (wq->is_one_time)
		jrv = pthread_tryjoin_np(wq->thr, &th_rv);
	else
		jrv = pthread_join(wq->thr, &th_rv);

	if (jrv != 0) {
		N_Tf(error_wq_nvmeibt_wq_destroy, "Fail to join thr=@PTHREAD name=@NAME '@AUTO_ERRNO' jrv=@JRV",
			wq->thr, wq->name, strerror(jrv));
		nvmeibt_wq_stuck_pthread_add(wq);
		goto out;
	}
	else {
		N_Tf(trace_wq_nvmeibt_wq_destroy, "Joined wq thr=@PTHREAD name=@NAME", wq->thr, wq->name);
	}

	// Now we can free the WQ thread resources.
	delete_wq_thread(wq);
	wq = NULL;
out:
	__NFOUT; // wq==NULL - cannot use __FOUT
}

/*
 * nvmeibt_wq_addw(): add a wq_entry to a workqueue.
 * returns: 0 on success, -1 otherwise
 */
int nvmeibt_wq_addw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *entry)
{
	int rv = -1;

	NTOMA_ASSERT(error_wq_nvmeibt_wq_addw, entry != NULL, "wq entry is NULL");

	if (lock(wq)) {
		goto out;
	}

	if (wq->is_destroying_wq) {
		N_Tf(t1_wq_addw, "Not adding to wq=@WQ_NAME, ptr=@PTR (dead)", (wq && wq->name) ? wq->name : "?", entry);
		goto unlock;
	}

	N_Tf(t2_wq_addw, "Adding to wq=@WQ_NAME, ptr=@PTR", (wq && wq->name) ? wq->name : "?", entry);
	XDLIST_ADD_TAIL(wq->entries, entry);

	rv = 0;

unlock:
	if (unlock(wq)) {
		goto out;
	}
	// (no harm done in wakeu when destroying, so let this be)
	if (wakeup(wq)) {
		goto out;
	}

out:
	return rv;
}

/*
 * nvmeibt_wq_setw(): replace all wq_entry in a workqueue with a new one.
 * returns: 0 on success, -1 otherwise
 */
int nvmeibt_wq_setw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *entry)
{
	struct nvmeibt_wq_entry *e;
	int rv = -1;

	__NFIN;
	if (lock(wq)) {
		goto out;
	}

	if (wq->is_destroying_wq) {
		N_Tf(t1_wq_setw, "Not adding to wq=@WQ_NAME, ptr=@PTR (dead)", (wq && wq->name) ? wq->name : "?", entry);
		goto unlock;
	}

	while ((e = XDLIST_FIRST(wq->entries))) {
		XDLIST_DEL(&e->link);
		nvmeibt_wq_cancel_entry(e);
	}

	N_Tf(t2_wq_setw, "Adding to wq=@WQ_NAME, ptr=@PTR", (wq && wq->name) ? wq->name : "?", entry);
	XDLIST_ADD_TAIL(wq->entries, entry);

	rv = 0;

unlock:
	if (unlock(wq)) {
		goto out;
	}
	// (no harm done in wakeu when destroying, so let this be)
	if (wakeup(wq)) {
		goto out;
	}

out:
	__NFOUT;
	return rv;
}

/*
 * nvmeibt_wq_delw(): delete a wq_entry from the workqueue.
 * returns: 0 on success, -1 otherwise (not found)
 */
int nvmeibt_wq_delw(struct nvmeibt_wq *wq, struct nvmeibt_wq_entry *entry)
{
	struct nvmeibt_wq_entry *e;
	int rv = -1;

	__NFIN;
	if (lock(wq)) {
		goto out;
	}
	XDLIST_FOREACH(e, wq->entries) {
		if (e == entry) {
			rv = 0;
			break;
		}
	}
	if (rv == 0) {
		XDLIST_DEL(&entry->link);
	}
	if (unlock(wq)) {
		goto out;
	}

out:
	__NFOUT;
	return rv;
}

/*
 * nvmeibt_wq_flush(): force execution of all wq_entry in a workqueue.
 * returns: none
 */
void nvmeibt_wq_flush(struct nvmeibt_wq *wq)
{
	wq_entries_list_t *l;

	__NFIN;
	if (lock(wq)) {
		goto out;
	}
	l = wq->entries;
	wq->entries = wq->entries == &wq->entries1 ? &wq->entries2 : &wq->entries1;
	if (unlock(wq)) {
		goto out;
	}
	exec_entries(wq, l);

out:
	__NFOUT;
}

static int is_wq_empty(struct nvmeibt_wq *wq)
{
	int rv;

	__NFIN;
	if (!lock(wq)) {
		rv = XDLIST_EMPTY(wq->entries) ? 1 : 0;
		unlock(wq);
	}
	else
		rv = 1;
	__NFOUT;
	return rv;
}

/*
 * nvmeibt_wq_drain(): force cancel of all wq_entry in a workqueue.
 * returns: none
 */
void nvmeibt_wq_drain(struct nvmeibt_wq *wq)
{
	__NFIN;
	do {
		wq->is_destroying_wq = true;
		nvmeibt_wq_flush(wq);
	} while (!is_wq_empty(wq));
	__NFOUT;
}

/*
 * struct once_wq_entry is used to wrap wq_entry that is intended to be
 * run once using an ad-hoc dedicated workqueue (see below).
 */
struct once_wq_entry {
	char						type[32];
	struct nvmeibt_wq			*once_wq;
	struct nvmeibt_wq_entry		*real_wq_entry;
	struct nvmeibt_wq_entry		wq_entry;
};

static void wq_run_once_execute(struct nvmeibt_wq_entry *wq_entry)
{
	struct once_wq_entry		*once_wq_entry;
	struct nvmeibt_wq_entry		*real_wq_entry;

	once_wq_entry = container_of(wq_entry, struct once_wq_entry, wq_entry);
	real_wq_entry = once_wq_entry->real_wq_entry;

	if (real_wq_entry->execute != NULL)
		real_wq_entry->execute(real_wq_entry);
}

static void wq_run_once_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct once_wq_entry		*once_wq_entry;
	struct nvmeibt_wq_entry		*real_wq_entry;

	NFIN;
	once_wq_entry = container_of(wq_entry, struct once_wq_entry, wq_entry);
	real_wq_entry = once_wq_entry->real_wq_entry;

	if (real_wq_entry->finalize != NULL)
		real_wq_entry->finalize(real_wq_entry);

	/* real_wq_entry->free() will be called by wq_run_once_free() callback */

	/*
	 * finalize() always runs in main toma thread, so we can now safely destroy
	 * the run_once workqueue we had created - it cannot be running now.
	 */

	nvmeibt_wq_destroy(once_wq_entry->once_wq);
	NFOUT;
}

extern void nvmeibt_toma_wakeup_wq_abort_func(struct nvmeibt_wq_entry *wq_entry);

static void wq_run_once_free(struct nvmeibt_wq_entry *wq_entry)
{
	struct once_wq_entry		*once_wq_entry;
	struct nvmeibt_wq_entry		*real_wq_entry;

	once_wq_entry = container_of(wq_entry, struct once_wq_entry, wq_entry);
	real_wq_entry = once_wq_entry->real_wq_entry;

	if (real_wq_entry->free != NULL)
		real_wq_entry->free(real_wq_entry);

	NNVMEIBT_TOMA_FREE(t2_wq_run_once_free, once_wq_entry);
}

/*
 * nemviebt_wq_run_once(): create a new workqueue to run just a given wq_entry.
 * the new workqueue created will be freed automatically when the entry is done
 * or when the workqueue is explicity destroyed.
 * returns: pointer to new wq
 */
struct nvmeibt_wq *nvmeibt_wq_run_once(struct nvmeibt_wq_entry *wq_entry)
{
	struct nvmeibt_wq			*once_wq;
	struct once_wq_entry		*once_wq_entry;

	static unsigned long		counter;

	if (wq_entry->abort != NULL) {
		N_Ef(e1_wq_run_once, "invalid abort func in entry type=@TYPE_STR PTR=@PTR", wq_entry->type, wq_entry);
		nvmeibt_abort(ES_FATAL);
	}

	once_wq_entry = NNVMEIBT_TOMA_CALLOC(t1_wq_run_once, 1, sizeof(*once_wq_entry));
	snprintf(once_wq_entry->type, 32, "RUN_ONCE(%lu): %s", counter++, wq_entry->type);

	once_wq_entry->wq_entry.type = once_wq_entry->type;
	once_wq_entry->wq_entry.execute = wq_run_once_execute;
	once_wq_entry->wq_entry.finalize = wq_run_once_finalize;
	once_wq_entry->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	once_wq_entry->wq_entry.free = wq_run_once_free;

	/*
	 * NOTE: we use the wq entry's type for the workqueue name, therefore the
	 * wq entry must outlive (as in: not freed before) the worqueue!
	 */
	once_wq = nvmeibt_wq_create(once_wq_entry->type);
	if (once_wq == NULL) {
		NNVMEIBT_TOMA_FREE(t2_wq_run_once, once_wq_entry);
		goto out;
	}

	once_wq->is_one_time = true;
	once_wq_entry->once_wq = once_wq;
	once_wq_entry->real_wq_entry = wq_entry;
	wq_entry->once_wq_entry = &once_wq_entry->wq_entry;

	if (nvmeibt_wq_addw(once_wq, &once_wq_entry->wq_entry) < 0) {
		N_Ef(e2_wq_run_once, "failed to add entry type=@TYPE_STR PTR=@PTR", wq_entry->type, wq_entry);
		nvmeibt_abort(ES_FATAL);
	}

out:
	return once_wq;
}

