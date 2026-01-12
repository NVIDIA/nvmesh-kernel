#ifndef NVMEIB_PUBLIC_KTH_H
#define NVMEIB_PUBLIC_KTH_H

#if !(defined(__KERNEL__) || (defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR == 1)))
#error("Kernel KTH include attempt from non-kernel build")
#endif

#include "common/kr_incs.h"
#include "nvmeib_utils.h"

#define KTH_CONTEXT(cond) do { \
	if (!(cond)) { \
		_NW(KTH_CONTEXT, "Bad context\n"); \
		dump_stack(); \
	} \
} while (0)

struct nvmeib_public_kth_params {
	const char *name;
    const char *short_name;
	int cpu;
	bool started;
	bool is_main;
	int (*run)(void *arg);
	int (*done)(void *arg);
	void *run_arg;
	void *done_arg;
};

struct nvmeib_public_kth_id {
	union {
		struct {
			u32 t;
			u32 s;
		};
		u64 tns;
		void *ptr;
	};
};

struct nvmeib_public_kth_event {
	struct list_head link;
	struct nvmeib_public_kth_id id;
	int type;
	int (*call)(void *, struct nvmeib_public_kth_event *);
	void (*free)(struct nvmeib_public_kth_event *);
	/* if return value is false we MUST_NOT delete the message
	   after this call
	 */
	bool (*no_recipient)(struct nvmeib_public_kth_event *);

	bool sync;
	struct nvmeib_public_kth_id pending_kth_id;
	struct completion *comp;
	int rc;
};

/**
 * Resume (start) a kth thread
 *
 * @author ofer (6/25/17)
 *
 * @param id
 *
 * @return int
 */
int nvmeib_public_kth_resume(struct nvmeib_public_kth_id id);

void __nvmeib_public_kth_event_free(struct nvmeib_public_kth_event *e);
void nvmeib_public_kth_event_free(struct nvmeib_public_kth_event *e);

const char * nvmeib_public_kth_e2s(int i);

struct nvmeib_public_kth_start_stop_event {
	struct nvmeib_public_kth_event event; /* Must be first */
	u64 sender;
};

static inline void nvmeib_public_kth_free_start_stop_event(
	struct nvmeib_public_kth_event *e)
{
	kfree(container_of(e, struct nvmeib_public_kth_start_stop_event, event));
}

struct nvmeib_public_kth_obj {
	struct nvmeib_public_kth_id kth;
	struct list_head events;
};

void nvmeib_public_kth_obj_init(struct nvmeib_public_kth_obj *obj);
void nvmeib_public_kth_obj_free(struct nvmeib_public_kth_obj *obj);
int nvmeib_public_kth_obj_wait_events(struct nvmeib_public_kth_obj *obj,
	long timeout, struct nvmeib_public_kth_event *e);

#define kth_event_pop(events) ({ \
	struct nvmeib_public_kth_event *e; \
	\
	e = list_first_entry_or_null((events), struct nvmeib_public_kth_event, link); \
	if (e) \
		list_del(&e->link); \
	e; \
})

#define for_each_kth_event_pop(events, e) \
	while (((e) = kth_event_pop((events))))

/* The following API can be called in any context */
/**
 * Start the kth library
 *
 * @author ofer (6/25/17)
 *
 * @param void
 */
void nvmeib_public_kth_init_lib(void);
/**
 * End the kth library
 *
 * @author ofer (6/25/17)
 *
 * @param void
 */
void nvmeib_public_kth_end_lib(void);
/**
 * Create a new kth thread
 *
 * @author ofer (6/25/17)
 *
 * @param p
 *
 * @return struct nvmeib_public_kth_id
 */
struct nvmeib_public_kth_id nvmeib_public_kth_create(
	struct nvmeib_public_kth_params *p);
/**
 * Stop remove_from_hash and free kth thread
 *
 * @author ofer (6/25/17)
 *
 * @param id
 *
 * @return int
 */
int nvmeib_public_kth_free(struct nvmeib_public_kth_id id);
/**
 * Check kth creation error condition
 *
 * @author ofer (6/25/17)
 *
 * @param id
 *
 * @return bool
 */
bool nvmeib_public_kth_is_err(struct nvmeib_public_kth_id id);
/**
 * Stop a kth thread
 *
 * @author ofer (6/25/17)
 *
 * @param id
 *
 * @return int
 */
int nvmeib_public_kth_stop(struct nvmeib_public_kth_id id);
int nvmeib_public_kth_stop_sync(struct nvmeib_public_kth_id id);
/**
 * Get kth name - no guarantee that the kth will be deleted in
 * the middle
 *
 * @author ofer (6/25/17)
 *
 * @param void
 *
 * @return const char*
 */
const char * nvmeib_public_kth_get_name(void);
/**
 * Send event to kth threads
 *
 * @author ofer (6/25/17)
 *
 * @param event
 *
 * @return int
 */
int nvmeib_public_kth_add_event(struct nvmeib_public_kth_event *event);
int nvmeib_public_kth_add_event_sync(struct nvmeib_public_kth_event *e);

/* The following API can be called only in kth context */
/**
 * Get the current thread ID
 */
struct nvmeib_public_kth_id nvmeib_public_kth_current(void);
/**
 * Kth wait for events.
 * events: where the put the new events
 * timeout: timeout in jiffies
 * e: if timeout_ms is not -1 e will be emitted on timeout. If e
 * is NULL the default event will be emitted
 *
 * @author ofer (6/25/17)
 *
 * @param events
 * @param timeout_jiffies
 * @param e
 *
 * @return int
 */
int nvmeib_public_kth_wait(struct list_head *events,
	long timeout, struct nvmeib_public_kth_event *e);


static inline long nvmeib_public_kth_msleep(unsigned int msecs)
{
	long j = nvmeib_public_kth_wait(NULL, msecs_to_jiffies(msecs), NULL);

	return j > 0 ? jiffies_to_msecs(j) : j;
}

/**
 * Just wait for new events - no timeout.  Event will be fetched
 * later
 *
 * @author ofer (6/25/17)
 *
 * @param void
 *
 * @return int
 */
int nvmeib_public_kth_jwait(void);
/**
 * Get all queued events
 *
 * @author ofer (6/25/17)
 *
 * @param events
 *
 * @return int
 */
int nvmeib_public_kth_get_events(struct list_head *events);
/**
 * Wait for the resume message
 */
int nvmeib_public_kth_wait_resume(void);

int nvmeib_public_kth_wait_async_fin(void);

/**
 * Wait for either a timeout message or a resume message
 */
int nvmeib_public_kth_wait_timeout_resume(
	long timeout, struct nvmeib_public_kth_event **e);
/**
 * Wait for the stop message
 */
int nvmeib_public_kth_wait_stop(void);
/**
 * Wait for either a timeout message or a stop message
 */
int nvmeib_public_kth_wait_timeout_stop(
	long timeout, struct nvmeib_public_kth_event **e);
/**
 * Wait for either a resume message or a stop message
 */
int nvmeib_public_kth_wait_resume_stop(struct nvmeib_public_kth_event **e);

int nvmeib_public_kth_wait_async_fin(void);

/**
 * Reschedule self
 *
 * @author ofer (6/25/17)
 *
 * @param void
 *
 * @return int
 */
int nvmeib_public_kth_resched(void);
/**
 * Rename your given name when created
 *
 * @author ofer (6/25/17)
 *
 * @param new_name
 *
 * @return int
 */
int nvmeib_public_kth_rename(const char *new_name);
/**
 * Switch current event queue
 */
struct list_head * nvmeib_public_kth_switch_current_eq(
	struct list_head *current_events);
/**
 * get current event queue
 */
struct list_head * nvmeib_public_kth_switch_get_eq(void);
/**
 * Kth wait for spesific events.
 * The filter function will end the wait when it returns true.
 * All unmatched events will queued in the current_events queue.
 *
 * @author ofer (6/25/17)
 *
 * @param f: the filter dunction
 * @param delete_e: if true will delete the matched event
 * @param e: if the matched event is not deleted it will be
 *  		returned in e
 *
 * @return int: the function failes if current event queue is
 *  	   not set
 */
typedef bool (*filter_f)(struct nvmeib_public_kth_event *);
int nvmeib_public_kth_wait_events(
	filter_f f, bool delete_e, struct nvmeib_public_kth_event **pe);
int nvmeib_public_kth_wait_events_timeout(filter_f f, bool delete_e,
	struct nvmeib_public_kth_event **pe, long timeout);

/**
 * Wait for a specific event type. The event is free'd.
 *
 * @param evtype: event type to wait for
 * @param timeout: timeout or -1 to wait forever
 *
 * @return int: -ETIMEDOUT if timedout, 0 on success or error code if failed
 */
int nvmeib_public_kth_wait_evtype(int evtype, long timeout);

/**
 * Mark that stop was called for the thread
 */
void nvmeib_public_kth_set_stop(void);
/**
 * Read the stop counter for the thread
 */
int nvmeib_public_kth_get_stop(void);

/**
 * Check if current context is kth thread
 */
bool nvmeib_public_is_kth(void);

/**
 * Check is object is the current running thread
 */
bool nvmeib_public_kth_is_current_obj(struct nvmeib_public_kth_obj *obj);
bool nvmeib_public_kth_is_current(struct nvmeib_public_kth_id id);

/**
 * Check is current thread is on the same scheduler as input
 * object
 */
bool nvmeib_public_kth_is_same_sched(struct nvmeib_public_kth_obj *obj);

const char * nvmeib_public_kth_get_obj_name(struct nvmeib_public_kth_obj *obj);

struct nvmeib_public_kth_ft {
	struct nvmeib_public_kth_id (*f_current)(void);
	const char * (*f_get_name)(void);
	const char * (*f_e2s)(int);
	int (*f_add_event)(struct nvmeib_public_kth_event *);
	int (*f_wait_events)(filter_f, bool, struct nvmeib_public_kth_event **);
	int (*f_wait_events_timeout)(
		filter_f, bool, struct nvmeib_public_kth_event **, long);
	void (*f_event_free)(struct nvmeib_public_kth_event *e);

	int (*f_wait_resume)(void);
	int (*f_resume)(struct nvmeib_public_kth_id id);
};

bool nvmeib_public_kth_is_idle(
	struct nvmeib_public_kth_obj *obj, unsigned long *version);

void nvmeib_public_kth_barrier(struct nvmeib_public_kth_obj *obj);

ssize_t nvmeib_public_kth_format_proc_table(void *dummy, char *buffer,
											size_t len);

/* simple_kth_event is a utility facility to be used with simple kth events
 * that have only a single pointer as an argument
 */
struct simple_kth_event {
	struct nvmeib_public_kth_event e; /* Must be first member */
	void *priv;
};

#define SKE_PRIV(__e) container_of(__e, struct simple_kth_event, e)->priv

int nvmeib_fire_simple_event(struct nvmeib_public_kth_id id,
			     int evtype, void *priv);
int nvmeib_fire_simple_event_wait(struct nvmeib_public_kth_id id,
				  int evtype, void *priv);

#ifdef NO_KTH_CHECKS
#ifdef KTH_CHECKS
#undef KTH_CHECKS
#pragma message("Disabling KTH_CHECKS in common module")
#endif
#endif

#ifdef KTH_CHECKS
static inline void nvmeib_public_kth_sleep_ctxt_check(const char *f, int l, const char *what, const char *who)
{
	if (!nvmeib_public_is_kth())
	  return;
  _NE(t_01_kthcsc, "%s:%d - It is forbiden to wait for %s (%s) inside kth context!!!", f, l, what, who);
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define _mutex_lock(_l) mutex_lock_nested(_l, 0)
#else
#define _mutex_lock(_l) mutex_lock_nested(_l, 0)
#endif

#undef mutex_lock
#define mutex_lock(_l) ({ \
  nvmeib_public_kth_sleep_ctxt_check(__func__, __LINE__, "mutex", #_l); \
  _mutex_lock(_l); \
})

#define wait_for_completion(_c) ({ \
  nvmeib_public_kth_sleep_ctxt_check(__func__, __LINE__, "completion", #_c); \
  wait_for_completion(_c); \
})
#endif

struct kth_generic_link {
	struct list_head link;
	struct nvmeib_public_kth_id id;
};

struct kth_generic_link * nvmeib_public_kth_get_link(void);
int nvmeib_public_kth_put_link(struct kth_generic_link *link);

#endif
