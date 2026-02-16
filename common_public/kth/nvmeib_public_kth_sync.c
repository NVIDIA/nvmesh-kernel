#include "kr_incs.h"
#define nvmeib_debug_level nvmeib_public_debug_level
#include "nvmeib_utils.h"
#include "nvmeib_public_kth_sync.h"
#include "nvmeib_public_kth.h"
#include "../nvmeibp_trace.h"

static void init_wq(struct nvmeib_public_kth_wq *wq,
	int (*t)(void *, void *), void *targ, int (*k)(void *, void *), void *karg)
{
	NFIN;
	INIT_LIST_HEAD(&wq->q);
	spin_lock_init(&wq->q_guard);
	wq->under_lock_wait = t;
	wq->under_lock_wait_arg = targ;
	wq->under_lock_wake = k;
	wq->under_lock_wake_arg = karg;
	NFOUT;
}

struct nvmeib_public_kth_wq * nvmeib_public_kth_wq_create(void)
{
	struct nvmeib_public_kth_wq *wq;

	NFIN;
	if ((wq = kzalloc(sizeof(*wq), GFP_KERNEL)))
		init_wq(wq, NULL, NULL, NULL, NULL);
	else
		_NE(error_nvmeib_public_kth_sync_nvmeib_public_kth_wq_create, "Fail to allocate wait_queue");
	NFOUT;
	return wq;
}
EXPORT_SYMBOL(nvmeib_public_kth_wq_create);

static void free_wq(struct nvmeib_public_kth_wq *wq)
{
	(void)wq;
}

void nvmeib_public_kth_wq_free(struct nvmeib_public_kth_wq *wq)
{
	NFIN;
	free_wq(wq);
	kfree(wq);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_kth_wq_free);

static int wq_wait(struct nvmeib_public_kth_wq *wq, void *arg, bool tail)
{
	unsigned long flags;
	struct kth_generic_link *link;
	int rv;

	NFIN;
	spin_lock_irqsave(&wq->q_guard, flags);
	/* check if we have a need_to_wait test function and if we do wait if
	   the return > 0
	*/
	if (wq->under_lock_wait)
		rv = wq->under_lock_wait(wq->under_lock_wait_arg, arg);
	else
		rv = 1;
	/* if we need to wait we queue the link under the lock */
	if (rv > 0) {
		if ((link = nvmeib_public_kth_get_link())) {
			if (tail)
				list_add_tail(&link->link, &wq->q);
			else
				list_add(&link->link, &wq->q);
			++wq->n_waiting;
		}
		else
			rv = -1;
	}
	spin_unlock_irqrestore(&wq->q_guard, flags);
	/* check if we need to wait */
	if (rv > 0)
		rv = nvmeib_public_kth_wait_resume();
	NFOUT;
	return rv;
}

static int wq_wait_head(struct nvmeib_public_kth_wq *wq, void *arg)
{
	return wq_wait(wq, arg, false);
}

int nvmeib_public_kth_wq_wait(struct nvmeib_public_kth_wq *wq, void *arg)
{
	return wq_wait(wq, arg, true);
}
EXPORT_SYMBOL(nvmeib_public_kth_wq_wait);

int nvmeib_public_kth_wq_wakeup(struct nvmeib_public_kth_wq *wq, void *arg)
{
	unsigned long flags;
	struct kth_generic_link *link;
	int rv;

	NFIN;
	spin_lock_irqsave(&wq->q_guard, flags);
	/* check if we have a need_to_wake test function and if we do not wake if
	   return > 0.  if return is zero we need to possible wake kth and
	   if return is negative we have an error
	*/
	if (wq->under_lock_wake)
		rv = wq->under_lock_wake(wq->under_lock_wake_arg, arg);
	else
		rv = 0;
	if (!rv && (link = list_first_entry_or_null(
		&wq->q, struct kth_generic_link, link))) {
		list_del(&link->link);
		--wq->n_waiting;
		nvmeib_public_kth_resume(link->id);
		spin_unlock_irqrestore(&wq->q_guard, flags);
		nvmeib_public_kth_put_link(link);
		goto out;
	}
	rv = -ENOENT;
	spin_unlock_irqrestore(&wq->q_guard, flags);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_wq_wakeup);

int nvmeib_public_kth_wq_wakeup_all(struct nvmeib_public_kth_wq *wq, void *arg)
{
	NFIN;
	while (!nvmeib_public_kth_wq_wakeup(wq, arg));
	NFOUT;
	return 0;
}
EXPORT_SYMBOL(nvmeib_public_kth_wq_wakeup_all);

int nvmeib_public_kth_wq_n_waiting(struct nvmeib_public_kth_wq *wq)
{
	return wq->n_waiting;
}
EXPORT_SYMBOL(nvmeib_public_kth_wq_n_waiting);

struct nvmeib_public_kth_mutex {
	const char *name;
	struct nvmeib_public_kth_wq wq;
	struct nvmeib_public_kth_id owner;
	int recursive;
};

#define ANONYMOUS_MUTEX "anon_mutex"
#define MUTEX_NAME_MAX_LEN 256

static int mutex_wait(void *targ, void *p __attribute__((unused)))
{
	struct nvmeib_public_kth_mutex *m = targ;
	struct nvmeib_public_kth_id cur = nvmeib_public_kth_current();
	int rv;

	NFIN;
	/* we only need to wait if the mutex is taken and not by out kth */
	if (m->owner.ptr && m->owner.ptr != cur.ptr)
		rv = 1;
	else {
		/* it is either free or taken by us */
		m->owner = cur;
		++m->recursive;
		rv = 0;
	}
	NFOUT;
	return rv;
}

static int mutex_wake(void *karg, void *p __attribute__((unused)))
{
	struct nvmeib_public_kth_mutex *m = karg;
	struct nvmeib_public_kth_id releaser = nvmeib_public_kth_current();
	 int rv;

	NFIN;
	/* check that the releasing thread is the owner thread */
	if (m->owner.ptr != releaser.ptr) {
		_NE(error_nvmeib_public_kth_sync_mutex_wake, "CATASTROPHIC: locker=@LOCKER, releaser=@RELEASER",
			m->owner.ptr, releaser.ptr);
		rv = -1;
	}
	else if (--m->recursive == 0) { /* we supporet recursive mutex */
		/* our kth does not hold the mutex anymore */
		m->owner.ptr = 0;
		rv = 0;
	}
	else
		/* this is a recursive lock so cur kth can marsh on */
		rv = 1;
	NFOUT;
	return rv;
}

static struct nvmeib_public_kth_mutex * mutex_create(
	struct nvmeib_public_kth_mutex *m, const char *name)
{
	char *mutex_name = NULL;
	int len;

	NFIN;
	if (name == NULL)
		name = ANONYMOUS_MUTEX;
	if ((len = strnlen(name, MUTEX_NAME_MAX_LEN)) &&
		(mutex_name = kzalloc(len + 1, GFP_KERNEL)) &&
		(m || (m = kzalloc(sizeof(*m), GFP_KERNEL)))) {
		memcpy(mutex_name, name, len);
		m->name = mutex_name;
		init_wq(&m->wq, mutex_wait, m, mutex_wake, m);
	}
	else {
		if (mutex_name)
			kfree(mutex_name);
		_NE(error_nvmeib_public_kth_sync_mutex_create, "Fail to allocate mutex @NAME", name);
		m = NULL;
	}
	NFOUT;
	return m;
}

struct nvmeib_public_kth_mutex * nvmeib_public_kth_mutex_create(
	const char *name)
{
	return mutex_create(NULL, name);
}
EXPORT_SYMBOL(nvmeib_public_kth_mutex_create);

static void mutex_free(struct nvmeib_public_kth_mutex *m)
{
	NFIN;
	free_wq(&m->wq);
	kfree(m->name);
	NFOUT;
}

void nvmeib_public_kth_mutex_free(struct nvmeib_public_kth_mutex *m)
{
	NFIN;
	mutex_free(m);
	kfree(m);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_kth_mutex_free);

int nvmeib_public_kth_mutex_lock(struct nvmeib_public_kth_mutex *m)
{
	struct nvmeib_public_kth_id cur = nvmeib_public_kth_current();
	int rv = 0;

	NFIN;
	while (!rv && m->owner.ptr && m->owner.ptr != cur.ptr)
		rv = nvmeib_public_kth_wq_wait(&m->wq, NULL);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_mutex_lock);

int nvmeib_public_kth_mutex_unlock(struct nvmeib_public_kth_mutex *m)
{
	return nvmeib_public_kth_wq_wakeup(&m->wq, NULL) < 0 ? -1 : 0;
}
EXPORT_SYMBOL(nvmeib_public_kth_mutex_unlock);

struct nvmeib_public_kth_semaphore {
	const char *name;
	struct nvmeib_public_kth_wq wq;
	int max_count;
	int count;

};

#define ANONYMOUS_SEMAPHORE "anon_sema"

static int sema_wait(void *targ, void *p)
{
	struct nvmeib_public_kth_semaphore *s = targ;
	int *n = p;
	int rv;

	NFIN;
	if (s->count < *n)
		rv = 1;
	else {
		s->count -= *n;
		*n = 0;
		rv = 0;
	}
	NFOUT;
	return rv;
}

static int sema_wake(void *karg, void *p)
{
	struct nvmeib_public_kth_semaphore *s = karg;
	int *n = p;
	int rv;

	NFIN;
	s->count += *n;
	/* if count is positive wakeup the next one waiting */
	if (s->count > 0)
		rv = 0;
	else
		rv = 1;
	NFOUT;
	return rv;
}

static struct nvmeib_public_kth_semaphore * sema_create(
	struct nvmeib_public_kth_semaphore *s, const char *name, int count)
{
	char *sema_name = NULL;
	int len;

	NFIN;
	if (name == NULL)
		name = ANONYMOUS_SEMAPHORE;
	if ((len = strnlen(name, MUTEX_NAME_MAX_LEN)) &&
		(sema_name = kzalloc(len + 1, GFP_KERNEL)) &&
		(s || (s = kzalloc(sizeof(*s), GFP_KERNEL)))) {
		memcpy(sema_name, name, len);
		s->name = sema_name;
		init_wq(&s->wq, sema_wait, s, sema_wake, s);
		s->max_count = s->count = count; 
	}
	else {
		if (sema_name)
			kfree(sema_name);
		_NE(error_nvmeib_public_kth_sync_sema_create, "Fail to allocate semaphore @NAME", name);
		s = NULL;
	}
	NFOUT;
	return s;
}

struct nvmeib_public_kth_semaphore * nvmeib_public_kth_semap_create(
	const char *name, int count)
{
	return sema_create(NULL, name, count);
}
EXPORT_SYMBOL(nvmeib_public_kth_semap_create);

static void sema_free(struct nvmeib_public_kth_semaphore *s)
{
	NFIN;
	free_wq(&s->wq);
	kfree(s->name);
	NFOUT;
}

void nvmeib_public_kth_semap_free(struct nvmeib_public_kth_semaphore *s)
{
	NFIN;
	sema_free(s);
	kfree(s);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_kth_semap_free);

int nvmeib_public_kth_semap_down(struct nvmeib_public_kth_semaphore *s, int n)
{
	int rv = 0;
	int m = n;

	NFIN;
	if (!rv && m) {
		/* start like the rest and (possibly )wait in line */
		rv = nvmeib_public_kth_wq_wait(&s->wq, &m);
		while (!rv && m)
			/* well we woke up but still we did not have enough in resource
			   in queue so to give it a chance make it the head of the waiters
			*/
			rv = wq_wait_head(&s->wq, &m);
	}
	if (!rv && s->count)
		nvmeib_public_kth_wq_wakeup(&s->wq, 0);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_semap_down);

int nvmeib_public_kth_semap_up(struct nvmeib_public_kth_semaphore *s, int n)
{
	return nvmeib_public_kth_wq_wakeup(&s->wq, &n) < 0 ? -1 : 0;
}
EXPORT_SYMBOL(nvmeib_public_kth_semap_up);

struct nvmeib_public_kth_rwmutex {
	struct nvmeib_public_kth_mutex m;
	struct nvmeib_public_kth_semaphore s;
};

#define ANONYMOUS_RWMUTEX "anon_rwmutex"

struct nvmeib_public_kth_rwmutex * nvmeib_public_kth_rwmutex_create(
	const char *name, int count)
{
	struct nvmeib_public_kth_rwmutex *m;

	NFIN;
	if ((m = kzalloc(sizeof(*m), GFP_KERNEL))) {
		if ((&m->m != mutex_create(&m->m, name)) ||
			(&m->s != sema_create(&m->s, name, count))) {
			_NE(error_nvmeib_public_kth_sync_nvmeib_public_kth_rwmutex_create, "Fail to initialize remutex @NAME", name);
			nvmeib_public_kth_rwmutex_free(m);
			m = NULL;
		}
	}
	else
		_NE(error_1_nvmeib_public_kth_sync_nvmeib_public_kth_rwmutex_create, "Fail to allocate rwmutex @NAME", name);
	NFOUT;
	return m;
}
EXPORT_SYMBOL(nvmeib_public_kth_rwmutex_create);

void nvmeib_public_kth_rwmutex_free(struct nvmeib_public_kth_rwmutex *m)
{
	NFIN;
	mutex_free(&m->m);
	sema_free(&m->s);
	kfree(m);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_kth_rwmutex_free);

int nvmeib_public_kth_rwmutex_rlock(struct nvmeib_public_kth_rwmutex *m)
{
	return nvmeib_public_kth_semap_down(&m->s, 1);
}
EXPORT_SYMBOL(nvmeib_public_kth_rwmutex_rlock);

int nvmeib_public_kth_rwmutex_runlock(struct nvmeib_public_kth_rwmutex *m)
{
	return nvmeib_public_kth_semap_up(&m->s, 1);
}
EXPORT_SYMBOL(nvmeib_public_kth_rwmutex_runlock);

int nvmeib_public_kth_rwmutex_wlock(struct nvmeib_public_kth_rwmutex *m)
{
	int rv;
	int i;

	NFIN;
	/* for write lock we need to firstly get the guard */
	if (!(rv = nvmeib_public_kth_mutex_lock(&m->m))) {
		/* now we do it one by one assuming that is a caller needs more than one
		   int will call it with the exact sixze that it needs
		*/
		for (i = 0; rv == 0 && i < m->s.max_count; ++i) {
			/* try to get the resource */
			if ((rv = nvmeib_public_kth_semap_down(&m->s, 1))) {
				/* we failed so return the already taken */
				while (i--)
					nvmeib_public_kth_semap_up(&m->s, 1);
			}
		}
		/* no need for the lock */
		nvmeib_public_kth_mutex_unlock(&m->m);
	}
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_rwmutex_wlock);

int nvmeib_public_kth_rwmutex_wunlock(struct nvmeib_public_kth_rwmutex *m)
{
	return nvmeib_public_kth_semap_up(&m->s, m->s.max_count);
}
EXPORT_SYMBOL(nvmeib_public_kth_rwmutex_wunlock);

void nvmeib_public_kth_init_completion(struct nvmeib_public_kth_completion *comp)
{
	init_completion(&comp->comp);
	init_wq(&comp->wq, NULL, NULL, NULL, NULL);
}
EXPORT_SYMBOL(nvmeib_public_kth_init_completion);

void nvmeib_public_kth_complete_all(struct nvmeib_public_kth_completion *comp)
{
	int rc;

	if (nvmeib_public_kth_completion_done(comp))
		return;

	complete_all(&comp->comp);
	rc = nvmeib_public_kth_wq_wakeup_all(&comp->wq, NULL);
	if (rc)
		_NE(error_nvmeib_public_kth_sync_nvmeib_public_kth_complete_all, "Failed to wakeup kth wq waiters (@RC)", rc);
}
EXPORT_SYMBOL(nvmeib_public_kth_complete_all);

void nvmeib_public_kth_complete(struct nvmeib_public_kth_completion *comp)
{
	int rc;

	if (nvmeib_public_kth_completion_done(comp))
		return;

	complete(&comp->comp);
	rc = nvmeib_public_kth_wq_wakeup(&comp->wq, NULL);
	if (rc)
		_NE(error_nvmeib_public_kth_sync_nvmeib_public_kth_complete, "Failed to wakeup kth wq waiter (@RC)", rc);
}
EXPORT_SYMBOL(nvmeib_public_kth_complete);

void nvmeib_public_kth_wait_for_completion(struct nvmeib_public_kth_completion *comp)
{
	bool is_kth;
	int rc;

	is_kth = nvmeib_public_is_kth();

	_ND(trace_nvmeib_public_kth_sync_nvmeib_public_kth_wait_for_completion, "Waiting for completion (@TYPE_STR)", is_kth ? "kth" : "non-kth");
	if (is_kth) {
		rc = nvmeib_public_kth_wq_wait(&comp->wq, NULL);
		if (rc)
			_NE(error_nvmeib_public_kth_sync_nvmeib_public_kth_wait_for_completion, "Error waiting for kth completion (@RC)", rc);
	} else {
		wait_for_completion(&comp->comp);
	}
	_ND(trace_1_nvmeib_public_kth_sync_nvmeib_public_kth_wait_for_completion, "Got completion (@TYPE_STR)", is_kth ? "kth" : "non-kth");
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_for_completion);
