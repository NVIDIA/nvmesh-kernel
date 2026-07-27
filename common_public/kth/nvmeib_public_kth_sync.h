#ifndef NVMEIB_PUBLIC_KTH_SYNC_H
#define NVMEIB_PUBLIC_KTH_SYNC_H

#include <linux/completion.h>

struct nvmeib_public_kth_wq {
	struct list_head q;
	spinlock_t q_guard;
	int n_waiting;
	int (*under_lock_wait)(void *, void *);
	void *under_lock_wait_arg;
	int (*under_lock_wake)(void *, void *);
	void *under_lock_wake_arg;
};

struct nvmeib_public_kth_completion {
	struct nvmeib_public_kth_wq wq;
	struct completion comp;
};

struct nvmeib_public_kth_wq * nvmeib_public_kth_wq_create(void);
void nvmeib_public_kth_wq_free(struct nvmeib_public_kth_wq *wq);
int nvmeib_public_kth_wq_wait(struct nvmeib_public_kth_wq *wq, void *arg);
int nvmeib_public_kth_wq_wakeup(struct nvmeib_public_kth_wq *wq, void *arg);
int nvmeib_public_kth_wq_wakeup_all(struct nvmeib_public_kth_wq *wq, void *arg);
int nvmeib_public_kth_wq_n_waiting(struct nvmeib_public_kth_wq *wq);

struct nvmeib_public_kth_mutex;
struct nvmeib_public_kth_mutex * nvmeib_public_kth_mutex_create(
	const char *name);
void nvmeib_public_kth_mutex_free(struct nvmeib_public_kth_mutex *m);
int nvmeib_public_kth_mutex_lock(struct nvmeib_public_kth_mutex *m);
int nvmeib_public_kth_mutex_unlock(struct nvmeib_public_kth_mutex *m);

struct nvmeib_public_kth_semaphore;
struct nvmeib_public_kth_semaphore * nvmeib_public_kth_semap_create(
	const char *name, int count);
void nvmeib_public_kth_semap_free(struct nvmeib_public_kth_semaphore *s);
/* if we need n resources always go for the whole n at once otherwise
   a deadlock is almost guaranteed...
*/
int nvmeib_public_kth_semap_down(struct nvmeib_public_kth_semaphore *s, int n);
int nvmeib_public_kth_semap_up(struct nvmeib_public_kth_semaphore *s, int n);

/* this one is a simple implementation - it can be improved if we mark
   the writelock and do not allow any more reads - maybe in next iteration
*/
struct nvmeib_public_kth_rwmutex;
struct nvmeib_public_kth_rwmutex * nvmeib_public_kth_rwmutex_create(
	const char *name, int count);
void nvmeib_public_kth_rwmutex_free(struct nvmeib_public_kth_rwmutex *m);
int nvmeib_public_kth_rwmutex_rlock(struct nvmeib_public_kth_rwmutex *m);
int nvmeib_public_kth_rwmutex_runlock(struct nvmeib_public_kth_rwmutex *m);
int nvmeib_public_kth_rwmutex_wlock(struct nvmeib_public_kth_rwmutex *m);
int nvmeib_public_kth_rwmutex_wunlock(struct nvmeib_public_kth_rwmutex *m);

void nvmeib_public_kth_init_completion(struct nvmeib_public_kth_completion *comp);
void nvmeib_public_kth_complete_all(struct nvmeib_public_kth_completion *comp);
void nvmeib_public_kth_complete(struct nvmeib_public_kth_completion *comp);
void nvmeib_public_kth_wait_for_completion(struct nvmeib_public_kth_completion *comp);

static inline bool nvmeib_public_kth_completion_done(struct nvmeib_public_kth_completion *comp)
{
	return completion_done(&comp->comp);
}

#endif
