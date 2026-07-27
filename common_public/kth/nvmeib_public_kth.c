#include "nvmeib_public_kth.h"
#include "nvmeib_public.h"
#define nvmeib_debug_level nvmeib_public_debug_level
#include "nvmeib_utils.h"
#include "nvmeib_kth_events.h"
#include "../nvmeibp_trace.h"

#define _NFIN
#define _NFOUT

/* 56-bit virtual addresses */
#define KYH_SIG_SHIFT 56
#define KTH_MASK ((u64)0xFF << KYH_SIG_SHIFT)
#define KTH_NMASK (~KTH_MASK)
#define KTH_SIG ((u64)0xDC << KYH_SIG_SHIFT)
static const char KTH_NAME_PREFIX[] = "X";

#define IS_KTH(p) (((u64)p & KTH_MASK) == KTH_SIG)
#define ENC_KTH(p) (((u64)p & KTH_NMASK) | KTH_SIG)
#ifdef __KERNEL__
	#define DEC_KTH(p) (struct nvmesh_kth *)(((u64)p & KTH_NMASK) | KTH_MASK)
#else
	#define DEC_KTH(p) (struct nvmesh_kth *)(((u64)p & KTH_NMASK))
#endif

//static typeof(&kthread_data) _kthread_data __attribute__((unused));
static void *(*_kthread_data)(struct task_struct *k) __attribute__((unused));

static inline struct nvmesh_kth * _get_current(void);
static struct nvmesh_kth * _get_current_(void);

static struct nvmesh_kth * _get_current_kth(void)
{
	struct nvmesh_kth *kth;
	kth = (current->flags & PF_KTHREAD) ? _kthread_data(current) : NULL;
	if (kth && IS_KTH(kth)) {
		kth = DEC_KTH(kth);
	}
	else
		kth = NULL;
	return kth;
}

static typeof(&_get_current) __get_current __attribute__((unused));
static typeof(&_get_current_) __get_current_ __attribute__((unused));

void nvmeib_public_kth_init_lib(void)
{
#if KS_KALLSYMS_LOOKUP
	_kthread_data = (typeof(_kthread_data))kallsyms_lookup_name("kthread_data");
#else
	printk(KERN_ERR "kallsyms_lookup_name() is NOT available!\n");
#endif
	if (false && _kthread_data) {
		__get_current = _get_current_kth;
		__get_current_ = _get_current_kth;
	}
	else {
		__get_current = _get_current;
		__get_current_ = _get_current_;
	}
}

static bool no_scheds(void);
static bool no_threads(void);
static void dump_scheds(void);
static void dump_threads(void);
void nvmeib_public_kth_end_lib(void)
{
	static const int maxn = 1000;
	int n = 0;

	while (n++ < maxn && !(no_scheds() && no_threads()))
		msleep(10);
	if (!(no_scheds() && no_threads())) {
		_NE(error_nvmeib_public_kth_nvmeib_public_kth_end_lib, "CATASTROPHIC: going down while threads are still running - "
		   "please reboot");
		dump_scheds();
		dump_threads();
	}
}

/* global thread_id hash */
static DEFINE_HASHTABLE(kth_hash, 8);
static DEFINE_HASHTABLE(kth_hash_rl, 8);
static DEFINE_SPINLOCK(kth_hash_guard);

static inline unsigned long hash_lock_guard(void)
{
	unsigned long flags;
	spin_lock_irqsave(&kth_hash_guard, flags);
	return flags;
}

static inline void hash_unlock_guard(unsigned long flags)
{
	spin_unlock_irqrestore(&kth_hash_guard, flags);
}

/* the event queue */
static DEFINE_HASHTABLE(sched_hash, 8);
static DEFINE_SPINLOCK(sched_hash_guard);

/* global unique id */
static atomic_t global_uid = ATOMIC_INIT(0);

static inline u32 _get_uid(atomic_t *uid)
{
	return atomic_inc_return(uid);
}

static inline u32 _get_guid(void)
{
	return _get_uid(&global_uid);
}

struct kth_done_event {
	struct nvmeib_public_kth_event event;
	u32 done_kth;
};

static void free_done_event(struct nvmeib_public_kth_event *event)
{
	struct kth_done_event *e =
		container_of(event, struct kth_done_event, event);

	NFIN;
	kfree(e);
	NFOUT;
}

struct timeout_descr {
	u32 kth_guid;
	u64 timeout;
	struct nvmeib_public_kth_event *timeout_event;
	struct rb_node node;
	struct hlist_node hlist;
	void (*free)(struct timeout_descr *);
};

static inline void print_to(
	const char *pre, struct timeout_descr *to, const char *post)
{
	_NF(trace_nvmeib_public_kth_print_to, "@PREFIX kth=@KTH_GUID, timeout=@TIMEOUT_LLONG, now=@NOW@PREFIX",
	   pre ?: "",
	   to->kth_guid, to->timeout, jiffies,
	   post ?: "");
}

#define MAX_SCHED_BATCH 2

struct scheduler {
	u32 guid;
	/* thread name */
	char *name;
	struct nvmesh_kth *ower;
	struct nvmesh_kth *cur;
	struct rb_root timeouts;
	DECLARE_HASHTABLE(rl_timeouts, 8);
	int n_before_timeout;
	struct list_head events;
	wait_queue_head_t wqh;
	spinlock_t events_guard;
	struct hlist_node hlist;
	struct list_head cur_events;
	spinlock_t switch_to_guard;
	unsigned long idle_version;
	bool idle;

	struct list_head link_pool;
	int n_link_allocated;
	int n_link_pool;
};

enum kth_state {
	kth_none,
	kth_launched,
	kth_suspended,
	kth_ready,
	kth_running,
	kth_done
};

static const char * __attribute__((used)) state_to_str(enum kth_state s)
{
	switch (s) {
	case kth_none: return "NONE";
	case kth_launched: return "LAUNCHED";
	case kth_suspended: return "SUSPENDED";
	case kth_ready: return "READY";
	case kth_running: return "RUNNING";
	case kth_done: return "done";
	default: return "???";
	}
}

struct nvmesh_kth {
	/* thread name */
	char *name;
	/* thread CPU - the thread will be binded to that CPU */
	int cpu;
	/* thread GUID */
	u32 guid;
	/* cache the sched guid */
	u32 sguid;
	/* true if we are a main thread - main thread ownes the scheduler */
	bool is_main;
	/* the current state */
	enum kth_state state;
	/* the thread */
	struct task_struct *t;
	/* thread event list */
	struct list_head events;
	/* thread wait for events on this wait_queue */
	/* the thread scheduler */
	wait_queue_head_t wqh;
	/* the thread scheduler */
	struct scheduler *sched;
	/* the run function */
	void *run_arg;
	int (*run)(void *arg);
	void *done_arg;
	int (*done)(void *arg);

	/* thread in the global thread list*/
	struct hlist_node hlist;
	struct hlist_node hlist_rl;
	/* the thread that created me */
	struct nvmesh_kth *creator;

	/* some info for /proc/.../kth */
	u32 creator_guid;
	u32 creator_sguid;

	/* my link in other thread done list */
	struct list_head done_link;
	/* my list of done threads */
	struct list_head done_list;

	/* current event queue */
	struct list_head *current_events;

	/* count the number of times stop on the thread was called */
	int stop_was_called;
};

static void erase_timeout(struct timeout_descr *to)
{
	NFIN;
	if (to) {
		kfree(to->timeout_event);
		if (to->free)
			to->free(to);
		else
			kfree(to);
	}

	NFOUT;
}

static struct nvmesh_kth* find_kth_(u32 t)
{
	struct nvmesh_kth *kth = NULL, *tkth;

	hash_for_each_possible(kth_hash, tkth, hlist, t) {
		if (tkth->guid == t) {
			kth = tkth;
			break;
		}
	}
	return kth;
}

static inline struct nvmesh_kth* find_kth(u32 t)
{
	struct nvmesh_kth *kth;
	unsigned long flags;
	flags = hash_lock_guard();
	kth = find_kth_(t);
	hash_unlock_guard(flags);
	return kth;
}

static struct nvmesh_kth* _get_current_(void)
{
	struct task_struct *tsk = current;
	struct nvmesh_kth *kth = NULL, *tkth;

	hash_for_each_possible(kth_hash_rl, tkth, hlist_rl, ((u64)tsk)) {
		if (tkth->t == tsk) {
			kth = tkth;
			break;
		}
	}
#if 0
	if (kth == NULL)
		pr_debug("current is not a kth thread\n");
#endif
	return kth;
}

static inline struct nvmesh_kth* _get_current(void)
{
	struct nvmesh_kth *kth;
	unsigned long flags;

	flags = hash_lock_guard();
	kth = _get_current_();
	hash_unlock_guard(flags);
	return kth;
}

static struct scheduler* find_sched_(u32 t)
{
	struct scheduler *sched = NULL, *tsched;

	//NFIN;
	hash_for_each_possible(sched_hash, tsched, hlist, t) {
		if (tsched->guid == t) {
			sched = tsched;
			break;
		}
	}
	//NFOUT;
	return sched;
}

static void sched_add_event(struct scheduler *sched,
	struct nvmeib_public_kth_event *event, bool add_tail)
{
	unsigned long flags;

	_NFIN;
	spin_lock_irqsave(&sched->events_guard, flags);
	if (add_tail)
		list_add_tail(&event->link, &sched->events);
	else
		list_add(&event->link, &sched->events);
	sched->idle = false;
	wake_up(&sched->wqh);
	spin_unlock_irqrestore(&sched->events_guard, flags);
	_NFOUT;
}

static int sched_find_add_event(
	struct nvmeib_public_kth_event *event, bool add_tail)
{
	struct scheduler *sched;
	unsigned long flags;
	int rv = 0;

	_NFIN;
	spin_lock_irqsave(&sched_hash_guard, flags);
	if ((sched = find_sched_(event->id.s)))
		sched_add_event(sched, event, add_tail);
	else
		rv = -1;
	spin_unlock_irqrestore(&sched_hash_guard, flags);
	_NFOUT;
	return rv;
}

static struct timeout_descr* sched_find_timeout(
	struct scheduler *sched, u32 kth_guid)
{
	struct timeout_descr *to = NULL, *tto;

	NFIN;
	hash_for_each_possible(sched->rl_timeouts, tto, hlist, kth_guid) {
		if (tto->kth_guid == kth_guid) {
			to = tto;
			break;
		}
	}
	NFOUT;
	return to;
}

static void rb_insert_timeout(
	struct scheduler *sched, struct timeout_descr *to)
{
	struct rb_node **link = &sched->timeouts.rb_node;
	struct rb_node *parent = NULL;
	struct timeout_descr *cur;

	NFIN;
	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct timeout_descr, node);

		if (cur->timeout > to->timeout)
			link = &(*link)->rb_left;
		else if (cur->timeout < to->timeout)
			link = &(*link)->rb_right;
		else
			link = &(*link)->rb_left;
	}
	rb_link_node(&to->node, parent, link);
	rb_insert_color(&to->node, &sched->timeouts);
	NFOUT;
}

static void hash_insert_timeout(
	struct scheduler *sched, struct timeout_descr *to)
{
	NFIN;
	hash_add(sched->rl_timeouts, &to->hlist, to->kth_guid);
	NFOUT;
}

static struct timeout_descr* insert_timeout(
	struct scheduler *sched, struct timeout_descr *to)
{
	struct timeout_descr *rv = NULL;

	NFIN;
	if (!(rv = sched_find_timeout(sched, to->kth_guid))) {
		rb_insert_timeout(sched, to);
		hash_insert_timeout(sched, to);
	}
	NFOUT;
	return rv;
}

static void remove_timeout(struct scheduler *sched, struct timeout_descr *to)
{
	NFIN;
	if (to) {
		hash_del(&to->hlist);
		rb_erase(&to->node, &sched->timeouts);
		erase_timeout(to);
	}
	NFOUT;
}

static int remove_kth_timeout(struct nvmesh_kth *kth)
{
	struct scheduler *sched = kth->sched;
	struct timeout_descr *to = NULL;

	NFIN;
	if ((to = sched_find_timeout(sched, kth->guid)))
		remove_timeout(sched, to);
	NFOUT;
	return to ? 0 : -1;
}

static void add_sched(struct scheduler *sched)
{
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&sched_hash_guard, flags);
	hash_add(sched_hash, &sched->hlist, sched->guid);
	spin_unlock_irqrestore(&sched_hash_guard, flags);
	NFOUT;
}

static void remove_sched(struct scheduler *sched)
{
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&sched_hash_guard, flags);
	hash_del(&sched->hlist);
	spin_unlock_irqrestore(&sched_hash_guard, flags);
	NFOUT;
}

static bool no_scheds(void)
{
	unsigned long flags;
	bool rv;

	NFIN;
	spin_lock_irqsave(&sched_hash_guard, flags);
	rv = hash_empty(sched_hash);
	spin_unlock_irqrestore(&sched_hash_guard, flags);
	NFOUT;
	return rv;
}

/*BEWARE: This function hangs !!!
 * calling a trace will callback this library to get kth name & try to acquire the same lock
 */
static void dump_scheds(void)
{
	struct scheduler *sched;
	int t;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&sched_hash_guard, flags);
	hash_for_each(sched_hash, t, sched, hlist)
		_NW(warn_nvmeib_public_kth_dump_scheds, "Found sched @GUID_INT (@SCHED_NAME)", sched->guid, sched->name);
	spin_unlock_irqrestore(&sched_hash_guard, flags);
	NFOUT;
}

static void sched_fetch_events(struct scheduler *sched)
{
	unsigned long flags;

	_NFIN;
	spin_lock_irqsave(&sched->events_guard, flags);
	list_splice_init(&sched->events, &sched->cur_events);
	spin_unlock_irqrestore(&sched->events_guard, flags);
	_NFOUT;
}

static struct scheduler* create_sched(struct nvmeib_public_kth_params *p)
{
	struct scheduler *sched = NULL;
	int len;

	NFIN;
	if (!(sched = kzalloc(sizeof(*sched), GFP_KERNEL))) {
		_NE(error_nvmeib_public_kth_create_sched, "OOM!!! Failed to create scheduler for main kth (@P_NAME)", p->name);
		goto out;
	}
	len = strlen(p->name) + sizeof(KTH_NAME_PREFIX) + 1;
	if (!(sched->name = kzalloc(len, GFP_KERNEL))) {
		_NE(error_1_nvmeib_public_kth_create_sched, "OOM!!! Failed to create kth name (@P_NAME)", p->name);
		goto free_sched;
	}
	else
		snprintf(sched->name, len, "%s%s", KTH_NAME_PREFIX, p->name);
	sched->guid = _get_guid();
	sched->timeouts = RB_ROOT;
	INIT_LIST_HEAD(&sched->events);
	init_waitqueue_head(&sched->wqh);
	spin_lock_init(&sched->events_guard);
	INIT_HLIST_NODE(&sched->hlist);
	INIT_LIST_HEAD(&sched->cur_events);
	spin_lock_init(&sched->switch_to_guard);
	hash_init(sched->rl_timeouts);
	INIT_LIST_HEAD(&sched->link_pool);
	add_sched(sched);
	goto out;

free_sched:
	kfree(sched);
	sched = NULL;

out:
	NFOUT;
	return sched;
}

static void free_sched(struct scheduler *sched)
{
	struct nvmeib_public_kth_event *e;
	struct kth_generic_link *link;

	NFIN;
	remove_sched(sched);
	while ((e = list_first_entry_or_null(
		&sched->events, struct nvmeib_public_kth_event, link))) {
		list_del(&e->link);
		nvmeib_public_kth_event_free(e);
	}
	while ((e = list_first_entry_or_null(
		&sched->cur_events, struct nvmeib_public_kth_event, link))) {
		list_del(&e->link);
		nvmeib_public_kth_event_free(e);
	}
	while ((link = list_first_entry_or_null(
		&sched->link_pool, struct kth_generic_link, link))) {
		list_del(&link->link);
		kfree(link);
	}
	kfree(sched->name);
	kfree(sched);
	NFOUT;
}

static struct kth_generic_link * sched_get_link(struct scheduler *sched)
{
	struct kth_generic_link *link;

	NFIN;
	if ((link = list_first_entry_or_null(
		&sched->link_pool, struct kth_generic_link, link))) {
		list_del(&link->link);
		--sched->n_link_pool;
	}
	else if ((link = kzalloc(sizeof(*link), GFP_ATOMIC)))
		++sched->n_link_allocated;
	else
		_NE(error_nvmeib_public_kth_sched_get_link, "OOPS: failed to allocate kth_link - already allocated @N_LINK_ALLOCATED",
			sched->n_link_allocated);
	NFOUT;
	return link;
}

static void sched_put_link(struct scheduler *sched, struct kth_generic_link *link)
{
	NFIN;
	list_add_tail(&link->link, &sched->link_pool);
	++sched->n_link_pool;
	NFOUT;
}

static void free_kth_events(struct list_head *events)
{
	struct nvmeib_public_kth_event *e;

	while ((e = list_first_entry_or_null(
		events, struct nvmeib_public_kth_event, link))) {
		list_del(&e->link);
		if ((e->no_recipient ? e->no_recipient(e) : true))
			nvmeib_public_kth_event_free(e);
	}
}

static void kth_free(struct nvmesh_kth *kth)
{
	unsigned long flags;

	NFIN;
	_NT(trace_nvmeib_public_kth_kth_free, "Going to terminate kth @KTH_NAME (@PTR)", kth->name, kth->t);
	if (current != kth->t)
		kthread_stop(kth->t);
	_NF(trace_1_nvmeib_public_kth_kth_free, "kth @KTH_NAME (@PTR) is now terminated", kth->name, kth->t);
	flags = hash_lock_guard();
	hash_del(&kth->hlist);
	hash_del(&kth->hlist_rl);
	hash_unlock_guard(flags);
	/* allow task_struct to be freed only after it cannot be accessed anymore
	 * the get was just after the thread creation */
	put_task_struct(kth->t);
	free_kth_events(&kth->events);
	kfree(kth->name);
	kfree(kth);
	NFOUT;
}

static void add_to_done(struct nvmesh_kth *cur,
	struct nvmesh_kth *to)
{
	NFIN;
	list_add_tail(&cur->done_link, &to->done_list);
	NFOUT;
}

static void remove_done_threads(struct nvmesh_kth *kth)
{
	struct nvmesh_kth *t;

	NFIN;
	while ((t = list_first_entry_or_null(
		&kth->done_list, struct nvmesh_kth, done_link))) {
		list_del(&t->done_link);
		BUG_ON(t == kth);
		BUG_ON(t->state != kth_done);
		remove_done_threads(t);
		kth_free(t);
	}
	NFOUT;
}

static void kth_switch_to(struct nvmesh_kth *kth)
{
	struct nvmesh_kth *cur;
	struct scheduler *sched;
	u32 from_guid, to_guid;

	_NFIN;
	cur = __get_current();
	sched = cur->sched;
	BUG_ON(sched != kth->sched);
	from_guid = cur->guid;
	to_guid = kth->guid;
	remove_kth_timeout(kth);
	_NF(trace_nvmeib_public_kth_kth_switch_to, "Switching from kth @FROM_GUID to kth @TO_GUID", from_guid, to_guid);
	if (cur != kth) {
		unsigned long flags;

		kth->state = kth_ready;
		/* if we are done do not change state since we are here just to wake
		   up the next ready thread
		*/
		if (cur->state != kth_done)
			cur->state = kth_suspended;
		else
			add_to_done(cur, kth);
		_NF(trace_1_nvmeib_public_kth_kth_switch_to, "my state - @STATE_TO_STR", state_to_str(cur->state));
		spin_lock_irqsave(&sched->switch_to_guard, flags);
		wake_up(&kth->wqh);
		/* if we are done there will be no more wake up for use */
		if (cur->state != kth_done)
			wait_event_interruptible_lock_irq(
				cur->wqh, cur->state != kth_suspended, sched->switch_to_guard);
		spin_unlock_irqrestore(&sched->switch_to_guard, flags);
	}
	/* after the wait make us the current running thread */
	if (cur->state != kth_done) {
		sched->cur = cur;
		cur->state = kth_running;
	}
	/* firstly remove done threads */
	remove_done_threads(cur);
	_NFOUT;
}

static int fetch_new_events(struct scheduler *sched)
{
	struct rb_node *node;
	struct timeout_descr *to;
	struct nvmesh_kth *kth;
	struct nvmeib_public_kth_event *timeout_event;
	u64 now;
	s64 timeout = 0;
	int resume = 0;
	int rv;

	_NFIN;
retry:
	_NF(trace_nvmeib_public_kth_fetch_new_events, "Try to fetch threads with expired timeouts");
	while ((node = rb_first(&sched->timeouts))) {
		to = rb_entry(node, struct timeout_descr, node);
		print_to("top waiter: ", to, NULL);
		if ((kth = find_kth(to->kth_guid))) {
			_NF(trace_1_nvmeib_public_kth_fetch_new_events, "Found candidate kth @KTH_NAME", kth->name);
			now = jiffies;
			if (to->timeout <= now) {
				timeout_event = to->timeout_event;
				to->timeout_event = NULL;
				remove_timeout(sched, to);
				if (timeout_event) {
					_NF(trace_2_nvmeib_public_kth_fetch_new_events, "Launched timeout event for kth @KTH_NAME", kth->name);
					sched_add_event(sched, timeout_event, false);
				}
				++resume;
			}
			else
				timeout = to->timeout - now;
			break;
		}
		else
			remove_timeout(sched, to);
	}
	sched->n_before_timeout = 0;
	if (resume == 0 && list_empty(&sched->cur_events)) {
		unsigned long flags;

		_NF(trace_3_nvmeib_public_kth_fetch_new_events, "Waiting for new events timeout @TIMEOUT_LLONG (Hz=@HZ)...", timeout, HZ);
		spin_lock_irqsave(&sched->events_guard, flags);
		if (list_empty(&sched->events)) {
			// only now indicate were idle, after we acquired the lock.
			++sched->idle_version;
			sched->idle = true;
		}
		rv = timeout ?
			wait_event_interruptible_lock_irq_timeout(sched->wqh,
			!list_empty(&sched->events), sched->events_guard, timeout) :
			wait_event_interruptible_lock_irq(sched->wqh,
			!list_empty(&sched->events), sched->events_guard);
		++sched->idle_version;
		sched->idle = false;
		spin_unlock_irqrestore(&sched->events_guard, flags);
		if (rv < 0) {
			_NE(error_nvmeib_public_kth_fetch_new_events, "Interrupted timed wait - try again");
			goto retry;
		}
		if ((timeout && rv > 0) || (timeout == 0 && rv == 0)) {
			_NF(trace_4_nvmeib_public_kth_fetch_new_events, "New event arrived - process it");
			++resume;
			goto out;
		}
		_NF(trace_5_nvmeib_public_kth_fetch_new_events, "Timeout waiting for event - fetch the waiting event");
		goto retry;
	}
	else if (resume == 0)
		++resume;

out:
	_NFOUT;
	return resume;
}

static void add_kth_event(
	struct nvmesh_kth *kth, struct nvmeib_public_kth_event *event)
{
	_NFIN;
	if (event->type == nke_stop)
		++kth->stop_was_called;
	list_add_tail(&event->link, &kth->events);
	_NFOUT;
}

static void scheduler_sched(struct scheduler *sched)
{
	struct nvmeib_public_kth_event *event;
	struct nvmesh_kth *kth;

	_NFIN;
retry:
	if (list_empty(&sched->cur_events))
		sched_fetch_events(sched);
	_NF(trace_nvmeib_public_kth_scheduler_sched, "Found events? @YES_NO_STATUS",
		list_empty(&sched->cur_events) ? "N" : "Y");
	while (sched->n_before_timeout < MAX_SCHED_BATCH &&
		   (event = list_first_entry_or_null(
				&sched->cur_events, struct nvmeib_public_kth_event, link))) {
		++sched->n_before_timeout;
		list_del(&event->link);
		_NF(scheduler_sched_f1, "Handling event @STR (@INT)",
			nvmeib_kth_event_to_str(event->type),(int)event->type);
		if ((kth = find_kth(event->id.t)) && kth->state != kth_done) {
			_NF(scheduler_sched_f2, "Found thread @STR", kth->name);
			add_kth_event(kth, event);
			kth_switch_to(kth);
			goto out;
		}
		else {
			_NF(scheduler_sched_f3, "e @PTR (@INT) has no thread...",
				event, (int)event->type);
			if ((event->no_recipient ? event->no_recipient(event) : true))
				nvmeib_public_kth_event_free(event);
		}
	}
	if (fetch_new_events(sched))
		goto retry;

out:
	_NFOUT;
}

static void default_on_done_kth(struct nvmesh_kth *kth)
{
	struct kth_done_event *event;

	NFIN;
	if ((event = kzalloc(sizeof(*event), GFP_ATOMIC))) {
		event->event.id.t = kth->sched->ower->guid;
		event->event.id.s = kth->sched->guid;
		event->event.type = nke_kth_done;
		event->event.free = free_done_event;
		event->done_kth = kth->guid;
		sched_add_event(kth->sched, &event->event, true);
	}
	else
		_NE(error_nvmeib_public_kth_default_on_done_kth, "OOM!!! kth @KTH_NAME annot allocate doen event - may lead a system hang",
			kth->name);
	NFOUT;
}

static int can_start_new_kth(struct nvmesh_kth *kth)
{
	struct scheduler *sched = kth->sched;
	struct nvmeib_public_kth_event *e;
	unsigned long flags;
	int rv = -1;

	_NFIN;
	spin_lock_irqsave(&sched->switch_to_guard, flags);
	kth->state = kth_suspended;
	kth->creator->state = kth_running;
	wake_up(&kth->creator->wqh);
	wait_event_interruptible_lock_irq(
		kth->wqh, kth->state != kth_suspended, sched->switch_to_guard);
	spin_unlock_irqrestore(&sched->switch_to_guard, flags);
	while ((e = list_first_entry_or_null(
				&kth->events, struct nvmeib_public_kth_event, link))) {
		if (e->type != nke_stop)
			rv = 0;
		if (e->type == nke_start ||
			e->type == nke_resume ||
			e->type == nke_stop) {
			list_del(&e->link);
			nvmeib_public_kth_event_free(e);
		}
		break;
	}
	_NFOUT;
	return rv;
}

static int kth_run(void *arg)
{
	struct nvmesh_kth *kth = DEC_KTH(arg);
	struct scheduler *sched = kth->sched;
	int rv = 0;

	_NFIN;
	sched->cur = kth;
	kth->state = kth_launched;
	while (!kthread_should_stop() && kth->run && rv == 0) {
		kth->state = kth_running;
		if (kth->is_main || !(rv = can_start_new_kth(kth)))
			rv = kth->run(kth->run_arg);
	}
	/* if we are here the thread is done */
	kth->state = kth_done;
	BUG_ON(kth->done == NULL);
	if (kth->done)
		kth->done(kth->done_arg);
	else if (!kth->is_main)
		default_on_done_kth(kth);
	if (!kth->is_main) {
		if (kth->state != kth_launched)
			scheduler_sched(kth->sched);
		else
			wake_up(&kth->creator->wqh);
	}
	else {
		free_sched(kth->sched);
		remove_done_threads(kth);
	}
	_NF(trace_nvmeib_public_kth_kth_run, "kth @KTH_NAME main is out", kth->name);
	_NFOUT;
	return 0;
}

static void add_to_thread_list(struct nvmesh_kth *kth)
{
	unsigned long flags;

	NFIN;
	flags = hash_lock_guard();
	hash_add(kth_hash, &kth->hlist, kth->guid);
	hash_add(kth_hash_rl, &kth->hlist_rl, ((u64)kth->t));
	hash_unlock_guard(flags);
	NFOUT;
}

static bool no_threads(void)
{
	unsigned long flags;
	bool rv;

	NFIN;
	flags = hash_lock_guard();
	rv = hash_empty(kth_hash);
	hash_unlock_guard(flags);
	NFOUT;
	return rv;
}

/*BEWARE: This function hangs !!!
 * calling a trace will callback this library to get kth name & try to acquire the same lock
 */
static void dump_threads(void)
{
	struct nvmesh_kth *kth = NULL;
	int t;
	unsigned long flags;

	NFIN;
	flags = hash_lock_guard();
	hash_for_each(kth_hash, t, kth, hlist)
		_NW(warn_nvmeib_public_kth_dump_threads, "Found thread @GUID_INT (@KTH_NAME)", kth->guid, kth->name);
	hash_unlock_guard(flags);
	NFOUT;
}

static int wake_up_new(struct nvmesh_kth *kth)
{
	struct nvmesh_kth *cur = __get_current();
	struct scheduler *sched = cur->sched;
	unsigned long flags;

	NFIN;
	kth->creator = cur;
	kth->creator_sguid = cur->sguid;
	kth->creator_guid = cur->guid;
	spin_lock_irqsave(&sched->switch_to_guard, flags);
	cur->state = kth_suspended;
	wake_up_process(kth->t);
	wait_event_interruptible_lock_irq(
		cur->wqh, cur->state != kth_suspended, sched->switch_to_guard);
	spin_unlock_irqrestore(&sched->switch_to_guard, flags);
	sched->cur = cur;
	NFOUT;

	return 0;
}

struct nvmeib_public_kth_id nvmeib_public_kth_create(
	struct nvmeib_public_kth_params *p)
{
	struct nvmesh_kth *kth;
	char *name;
	struct task_struct *t;
	u32 guid;
	int len;
	struct nvmeib_public_kth_id id = {{}};
	struct scheduler *sched;

	_NFIN;
	len = strlen(p->name) + sizeof(KTH_NAME_PREFIX) + 1;
	if (!(name = kzalloc(len, GFP_KERNEL))) {
		_NE(error_nvmeib_public_kth_nvmeib_public_kth_create, "OOM!!! Failed to create kth name (@P_NAME)", p->name);
		goto out;
	}
	if (p->is_main) {
		if (!(sched = create_sched(p))) {
			_NE(error_1_nvmeib_public_kth_nvmeib_public_kth_create, "OOM!!! Failed to create scheduler for kth name (@P_NAME)",
				p->name);
			goto free_kth_name;
		}
	}
	else
		sched = __get_current()->sched;

	_NT(trace_nvmeib_public_kth_nvmeib_public_kth_create, "short: @SHORT_NAME name: @P_NAME is_main: @IS_MAIN current sched: @GUID_INT",
	   p->short_name, p->name, p->is_main, sched->guid);

	if (!(kth = kzalloc(sizeof(*kth), GFP_KERNEL))) {
		_NE(error_2_nvmeib_public_kth_nvmeib_public_kth_create, "OOM!!! Failed to create kth obj (@P_NAME)", p->name);
		goto free_kth_sched;
	}
	t = kthread_create(kth_run, (void *)ENC_KTH(kth), "%s", p->short_name);
	if (IS_ERR(t)) {
		_NE(error_3_nvmeib_public_kth_nvmeib_public_kth_create, "Fail to create thread @P_NAME - error @PTR_ERR", p->name, PTR_ERR(t));
		goto free_kth;
	}
	else {
		/* increments usage counter so the thread can exit without
		   waiting for kthread_stop.  Otherwise if the thread leaves before
		   the call to kthread_stop we will crash when we eventually
		   call to kthread_stop.
		*/
		get_task_struct(t);
		/* beyond this point nvmeib_public_kth_free() MUST be called  */
	}

	if (p->cpu != -1)
		kthread_bind(t, p->cpu);

	/* get the thread GUID */
	guid = _get_guid();
	snprintf(name, len, "%s%s", KTH_NAME_PREFIX, p->name);
	kth->t = t;
	kth->name = name;
	kth->guid = guid;
	kth->sguid = sched->guid;
	kth->run = p->run;
	kth->run_arg = p->run_arg;
	kth->done = p->done;
	kth->done_arg = p->done_arg;
	kth->state = kth_none;
	kth->is_main = p->is_main;
	kth->sched = sched;
	if (kth->is_main)
		kth->sched->ower = kth;
	INIT_LIST_HEAD(&kth->events);
	INIT_LIST_HEAD(&kth->done_list);
	INIT_HLIST_NODE(&kth->hlist);
	INIT_HLIST_NODE(&kth->hlist_rl);
	init_waitqueue_head(&kth->wqh);
	add_to_thread_list(kth);
	/* check if we are a main thread and if we are start it now */
	if (kth->is_main) {
		wake_up_process(t);
		p->started = true;
	}
	else
		p->started = wake_up_new(kth) == 0;
	id.t = guid;
	id.s = kth->sched->guid;
	goto out;

free_kth:
	kfree(kth);
	kth = NULL;

free_kth_sched:
	if (p->is_main)
		kfree(sched);

free_kth_name:
	kfree(name);

out:
	_NFOUT;
	return id;
}
EXPORT_SYMBOL(nvmeib_public_kth_create);

int nvmeib_public_kth_free(struct nvmeib_public_kth_id id)
{
	struct nvmesh_kth *kth;
	int rv;

	NFIN;
	kth = find_kth(id.t);
	if (kth && kth->is_main) {
		kth_free(kth);
		rv = 0;
	}
	else
		rv = -1;
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_free);

bool nvmeib_public_kth_is_err(struct nvmeib_public_kth_id id)
{
	return !(id.s && id.t);
}
EXPORT_SYMBOL(nvmeib_public_kth_is_err);

const char* nvmeib_public_kth_get_name(void)
{
	struct nvmesh_kth *kth;
	const char *name = NULL;

	//NFIN;
	kth = __get_current();
	name = kth ? kth->name : current->comm;
	//NFOUT;
	return name;
}
EXPORT_SYMBOL(nvmeib_public_kth_get_name);

struct list_head * nvmeib_public_kth_switch_current_eq(
	struct list_head *current_events)
{
	struct nvmesh_kth *kth;
	struct list_head *l;

	NFIN;
	if ((kth = __get_current())) {
		l = kth->current_events;
		kth->current_events = current_events;
		list_splice_tail_init(&kth->events, current_events);
	}
	else
		l = NULL;

	NFOUT;
	return l;
}
EXPORT_SYMBOL(nvmeib_public_kth_switch_current_eq);

struct list_head * nvmeib_public_kth_switch_get_eq(void)
{
	struct nvmesh_kth *kth;
	struct list_head *l;

	NFIN;
	kth = __get_current();
	l = kth ? kth->current_events : NULL;
	NFOUT;
	return l;
}
EXPORT_SYMBOL(nvmeib_public_kth_switch_get_eq);

int nvmeib_public_kth_rename(const char *new_name)
{
	struct nvmesh_kth *kth;
	char *name;
	int l1, l2, rv = -1;

	_NFIN;
	kth = __get_current();
	l1 = strlen(new_name) + sizeof(KTH_NAME_PREFIX) + 1;
	l2 = strlen(kth->name);
	if (l1 < l2) {
		memset(kth->name, 0, l2);
	}
	else {
		if (!(name = kzalloc(l1, GFP_KERNEL))) {
			_NE(error_nvmeib_public_kth_nvmeib_public_kth_rename, "OOM!!! Failed to create kth (@KTH_NAME) new name (@NEW_NAME)",
				kth->name, new_name);
			goto out;
		}
		else {
			kfree(kth->name);
			kth->name = name;
			l2 = l1;
		}
	}
	snprintf(kth->name, l2, "%s%s", KTH_NAME_PREFIX, new_name);

out:
	_NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_rename);

static int _nvmeib_public_kth_stop(struct nvmeib_public_kth_id id, bool sync)
{
	struct nvmesh_kth *kth;
	struct nvmeib_public_kth_start_stop_event *e;
	int rv;

	_NFIN;
	kth = _get_current();
	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e) {
		_NE(error_nvmeib_public_kth_nvmeib_public_kth_stop, "kth @KTH_NAME failed to allocate stop event for kth @TOPOLOGY_INT",
		   kth ? kth->name : "other", id.t);
		rv = -ENOMEM;
		goto out;
	}

	e->sender = kth ? kth->guid : 0;
	e->event.type = nke_stop;
	e->event.id = id;

	rv = sync ?
		nvmeib_public_kth_add_event_sync(&e->event) :
		nvmeib_public_kth_add_event(&e->event);
	if (rv) {
		_NE(error_1_nvmeib_public_kth_nvmeib_public_kth_stop, "No recipient for stop message to thread @TOPOLOGY_INT", id.t);
		goto err_free;
	}

	goto out;

err_free:
	kfree(e);
out:
	_NFOUT;
	return rv;
}

int nvmeib_public_kth_stop(struct nvmeib_public_kth_id id)
{
	return _nvmeib_public_kth_stop(id, false);
}
EXPORT_SYMBOL(nvmeib_public_kth_stop);

int nvmeib_public_kth_stop_sync(struct nvmeib_public_kth_id id)
{
	return _nvmeib_public_kth_stop(id, true);
}
EXPORT_SYMBOL(nvmeib_public_kth_stop_sync);

int nvmeib_public_kth_wait(struct list_head *events,
	long timeout, struct nvmeib_public_kth_event *e)
{
	struct nvmesh_kth *kth;
	struct timeout_descr *to;
	struct nvmeib_public_kth_event *t = NULL;
	struct nvmeib_public_kth_id id;
	int rv = 0;

	_NFIN;
	kth = __get_current();
	if (timeout > 0) {
		if ((to = kzalloc(sizeof(*to), GFP_ATOMIC)) &&
			(e || (t = kzalloc(sizeof(*t), GFP_ATOMIC)))) {
			to->kth_guid = kth->guid;
			if (!e && (t != NULL)) {
				t->id.t = kth->guid;
				t->id.s = kth->sched->guid;
				t->type = nke_timeout;
				to->timeout_event = t;
			}
			else {
				if (e) {
					to->timeout_event = e;
				}
			}
			to->timeout = jiffies + timeout;
			insert_timeout(kth->sched, to);
		}
		else {
			_NE(error_nvmeib_public_kth_nvmeib_public_kth_wait, "OOM!!! Failed to allocate timeout descriptor "
				"memory for kth @KTH_NAME", kth->name);
			rv = -ENOMEM;
			if (to != NULL) {
				kfree(to);
			}
			if (t != NULL) { // improbable
				kfree(t);
			}
			goto out;
		}
	}
	else if (timeout == 0) {
		/* re-schedule */
		id.t = kth->guid;
		id.s = kth->sched->guid;
		rv = nvmeib_public_kth_resume(id);
	}
	scheduler_sched(kth->sched);
	_NF(trace_nvmeib_public_kth_nvmeib_public_kth_wait, "We are out of the scheduler");
	if (events) {
		_NF(trace_1_nvmeib_public_kth_nvmeib_public_kth_wait, "Adding events to object event list (@EVENTS)", events);
		list_splice_tail_init(&kth->events, events);
	}

out:
	_NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_wait);

int nvmeib_public_kth_jwait(void)
{
	return nvmeib_public_kth_wait(NULL, -1, NULL);
}
EXPORT_SYMBOL(nvmeib_public_kth_jwait);

int nvmeib_public_kth_get_events(struct list_head *events)
{
	struct nvmesh_kth *kth;

	NFIN;
	if (events) {
		kth = __get_current();
		list_splice_init(&kth->events, events);
	}
	NFOUT;
	return 0;
}
EXPORT_SYMBOL(nvmeib_public_kth_get_events);

int nvmeib_public_kth_resume(struct nvmeib_public_kth_id id)
{
	struct nvmeib_public_kth_event *e;
	int rv;

	NFIN;
	if ((e = kzalloc(sizeof(*e), GFP_ATOMIC))) {
		e->type = nke_resume;
		e->id = id;
		if ((rv = nvmeib_public_kth_add_event(e))) {
			_NE(error_nvmeib_public_kth_nvmeib_public_kth_resume, "No recipient for resume message to thread @TOPOLOGY_INT", id.t);
			kfree(e);
		}
	}
	else {
		_NE(error_1_nvmeib_public_kth_nvmeib_public_kth_resume, "OOM!!! Failed to allocate resumed event for kth @TOPOLOGY_INT", id.t);
		rv = -1;
	}
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_resume);

static int nvmeib_public_kth_async_fin(struct nvmeib_public_kth_id id)
{
	struct nvmeib_public_kth_event *e;
	int rv;

	NFIN;
	if ((e = kzalloc(sizeof(*e), GFP_ATOMIC))) {
		e->type = nke_async_fin;
		e->id = id;
		if ((rv = nvmeib_public_kth_add_event(e))) {
			_NE(error_nvmeib_public_kth_nvmeib_public_kth_async_fin, "No recipient for async_fin message to thread @TOPOLOGY_INT", id.t);
			kfree(e);
		}
	}
	else {
		_NE(error_1_nvmeib_public_kth_nvmeib_public_kth_async_fin, "OOM!!! Failed to allocate async_fin event for kth @TOPOLOGY_INT", id.t);
		rv = -1;
	}
	NFOUT;
	return rv;
}

void nvmeib_public_kth_event_free(struct nvmeib_public_kth_event *e)
{
	if (e->sync) {
		int rc;

		if (e->comp) {
			_NF(trace_nvmeib_public_kth_nvmeib_public_kth_event_free, "Sending completion");

			complete(e->comp);
		} else {
			_NF(trace_1_nvmeib_public_kth_nvmeib_public_kth_event_free, "Sending kth resume event");
			rc = nvmeib_public_kth_async_fin(e->pending_kth_id);
			if (rc)
				_NE(error_nvmeib_public_kth_nvmeib_public_kth_event_free, "Error resuming pending kth");
		}

		return;
	}

	__nvmeib_public_kth_event_free(e);
}
EXPORT_SYMBOL(nvmeib_public_kth_event_free);

void __nvmeib_public_kth_event_free(struct nvmeib_public_kth_event *e)
{
	if (e->sync) {
		_NE(__nvmeib_public_kth_event_free,
			"Must use nvmeib_public_kth_event_free() for sync events\n");
		WARN_ON(1);
		return;
	}

	if (e->free)
		e->free(e);
	else
		kfree(e);
}
EXPORT_SYMBOL(__nvmeib_public_kth_event_free);

int nvmeib_public_kth_resched(void)
{
	struct nvmesh_kth *kth;
	struct nvmeib_public_kth_id id = {{}};
	int rv;

	NFIN;
	kth = _get_current();
	id.t = kth->guid;
	id.s = kth->sched->guid;
	nvmeib_public_kth_resume(id);
	rv = nvmeib_public_kth_wait_resume();
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_resched);

int nvmeib_public_kth_add_event(struct nvmeib_public_kth_event *e)
{
	int rv;

	NFIN;

	/* Try to hunt down calling to add_event() instead of add_event_sync()
	 * when using event allocated on stack
	 */
	//WARN_ON(!e->sync && !virt_addr_valid(e));

	_NF(trace_nvmeib_public_kth_nvmeib_public_kth_add_event, "add_event @NVMEIB_KTH_EVENT_TO_STR (@E_TYPE_INT)", nvmeib_kth_event_to_str(e->type), e->type);

	rv = sched_find_add_event(e, true);

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_add_event);

int nvmeib_public_kth_add_event_sync(struct nvmeib_public_kth_event *e)
{
	bool is_kth = nvmeib_public_is_kth();
	DECLARE_COMPLETION_ONSTACK(comp);
	int evtype = e->type;
	int rc;

	e->sync = true;

	/* e->free should not be used in sync calls - in such calls the
	 * convention is to allocate the event struct on stack
	 */
	WARN_ON(e->free);

	if (is_kth)
		e->pending_kth_id = nvmeib_public_kth_current();
	else
		e->comp = e->comp ?: &comp;

	rc = nvmeib_public_kth_add_event(e);
	if (rc) {
		_NE(error_nvmeib_public_kth_nvmeib_public_kth_add_event_sync, "Error adding event");
		return rc;
	}

	_NF(trace_nvmeib_public_kth_nvmeib_public_kth_add_event_sync, "Waiting for synchronic event @NVMEIB_KTH_EVENT_TO_STR (@EVTYPE) to finish",
	   nvmeib_kth_event_to_str(evtype), evtype);
	if (is_kth) {
		rc = nvmeib_public_kth_wait_async_fin();
		_NF(trace_1_nvmeib_public_kth_nvmeib_public_kth_add_event_sync, "Finished waiting.");
	} else {
		wait_for_completion(&comp);
		_NF(trace_2_nvmeib_public_kth_nvmeib_public_kth_add_event_sync, "Got completion. Finished waiting");
	}

	return rc ?: e->rc;
}
EXPORT_SYMBOL(nvmeib_public_kth_add_event_sync);

void nvmeib_public_kth_obj_init(struct nvmeib_public_kth_obj *obj)
{
	INIT_LIST_HEAD(&obj->events);
}
EXPORT_SYMBOL(nvmeib_public_kth_obj_init);

void nvmeib_public_kth_obj_free(struct nvmeib_public_kth_obj *obj)
{
	free_kth_events(&obj->events);
}
EXPORT_SYMBOL(nvmeib_public_kth_obj_free);

int nvmeib_public_kth_obj_wait_events(struct nvmeib_public_kth_obj *obj,
	long timeout, struct nvmeib_public_kth_event *e)
{
	return list_empty(&obj->events) ?
		nvmeib_public_kth_wait(&obj->events, timeout, e) : 0;
}
EXPORT_SYMBOL(nvmeib_public_kth_obj_wait_events);

const char *nvmeib_public_kth_e2s(int i)
{
	switch (i) {
	case nke_start: return "START";
	case nke_stop: return "STOP";
	case nke_timeout: return "TIMEOUT";
	case nke_resume: return "RESUME";
	case nke_private: return "PRIVATE";
	case nke_kth_done: return "DONE";
	default: return "UNKNOWN";
	}
}
EXPORT_SYMBOL(nvmeib_public_kth_e2s);

static int _kth_wait_events(filter_f f, int evtype, bool delete_e,
	struct nvmeib_public_kth_event **pe, long timeout)
{
	struct nvmesh_kth *kth;
	struct list_head *eq = NULL;
	LIST_HEAD(next_events);
	struct nvmeib_public_kth_event *e;
	int rv;
	bool cont = true;
	long wakeup_time;

	_NFIN;

	WARN_ON(f && evtype >= 0);
	WARN_ON(evtype == -1 && !f);

	kth = __get_current();
	if (!(kth && (eq = kth->current_events))) {
		_NE(error_nvmeib_public_kth_kth_wait_events, "Invalid kth for wait event - kth=@KTH, eq=@EQ", kth, eq);
		rv = -1;
		goto out;
	}
	/* if we were waiting for multiple events and left after
	   the first one but the caller still waiting for more filtered events
	   we must check that we do not have them already in the queue
	*/
	list_splice_tail_init(eq, &next_events);
	if (timeout >= 0)
		wakeup_time = jiffies + timeout;
	else
		wakeup_time = -1;
	do {
		rv = 0;
		if (!list_empty(&next_events) ||
			!(rv = nvmeib_public_kth_wait(&next_events, timeout, NULL))) {
			while (cont && (e = list_first_entry_or_null(
				&next_events, struct nvmeib_public_kth_event, link))) {
				bool match;

				list_del(&e->link);

				match = f ? f(e) : evtype == e->type;

				if (match || (wakeup_time >= 0 && e->type == nke_timeout)) {
					cont = false;
					if (delete_e)
						nvmeib_public_kth_event_free(e);
					else if (pe)
						*pe = e;
					else
						_NE(_kth_wait_events_e1,
							"Memory leak we got a matched filter but it is not "
							"mark for deletion and not returned e=@PTR", e);
				}
				else
					list_add_tail(&e->link, eq);
			}
		}
		if (wakeup_time != -1) {
			timeout = wakeup_time - (long)jiffies;
			if (timeout < 0)
				timeout = 0;
		}
	} while (cont && rv == 0);
	/* push back the remaining of the event in the next_events
	   into the user queue
	*/
	while ((e = list_first_entry_or_null(
		&next_events, struct nvmeib_public_kth_event, link))) {
		list_del(&e->link);
		list_add_tail(&e->link, eq);
	}

out:
	_NFOUT;
	return rv;
}

int nvmeib_public_kth_wait_events(
	filter_f f, bool delete_e, struct nvmeib_public_kth_event **pe)
{
	return _kth_wait_events(f, -1, delete_e, pe, -1);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_events);

int nvmeib_public_kth_wait_events_timeout(filter_f f, bool delete_e,
	struct nvmeib_public_kth_event **pe, long timeout)
{
	return _kth_wait_events(f, -1, delete_e, pe, timeout);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_events_timeout);

int nvmeib_public_kth_wait_evtype(int evtype, long timeout)
{
	struct nvmeib_public_kth_event *e = NULL;
	int rc;

	_NT(trace_nvmeib_public_kth_nvmeib_public_kth_wait_evtype, "About to wait for event @NVMEIB_KTH_EVENT_TO_STR(@EVTYPE)",
	   nvmeib_kth_event_to_str(evtype), evtype);
	rc = _kth_wait_events(NULL, evtype, false, &e, timeout);
	if (rc)
		goto out;

	if (e->type == nke_timeout)
		rc = -ETIMEDOUT;

	nvmeib_public_kth_event_free(e);

out:
	_NT(trace_1_nvmeib_public_kth_nvmeib_public_kth_wait_evtype, "Finished waiting for event @NVMEIB_KTH_EVENT_TO_STR(@EVTYPE). rc = @RC",
	   nvmeib_kth_event_to_str(evtype), evtype, rc);

	return rc;
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_evtype);

struct nvmeib_public_kth_id nvmeib_public_kth_current(void)
{
	struct nvmeib_public_kth_id id = {{}};
	struct nvmesh_kth *kth;

	NFIN;
	kth = __get_current();
	if (kth) {
		id.t = kth->guid;
		id.s = kth->sched->guid;
	}
	NFOUT;
	return id;
}
EXPORT_SYMBOL(nvmeib_public_kth_current);

static bool filter_resume(struct nvmeib_public_kth_event *e)
{
	return e->type == nke_resume;
}

int nvmeib_public_kth_wait_resume(void)
{
	return nvmeib_public_kth_wait_events(filter_resume, true, NULL);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_resume);

static bool filter_async_fin(struct nvmeib_public_kth_event *e)
{
	return e->type == nke_async_fin;
}

int nvmeib_public_kth_wait_async_fin(void)
{
	return nvmeib_public_kth_wait_events(filter_async_fin, true, NULL);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_async_fin);

static bool filter_timeout(struct nvmeib_public_kth_event *e)
{
	return e->type == nke_timeout;
}

static bool filter_timeout_resume(struct nvmeib_public_kth_event *e)
{
	return filter_timeout(e) || filter_resume(e);
}

int nvmeib_public_kth_wait_timeout_resume(
	long timeout, struct nvmeib_public_kth_event **e)
{
	return nvmeib_public_kth_wait_events_timeout(filter_timeout_resume, false, e, timeout);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_timeout_resume);

static bool filter_stop(struct nvmeib_public_kth_event *e)
{
	return e->type == nke_stop;
}

int nvmeib_public_kth_wait_stop(void)
{
	return nvmeib_public_kth_wait_events(filter_stop, true, NULL);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_stop);

static bool filter_timeout_stop(struct nvmeib_public_kth_event *e)
{
	return filter_timeout(e) || filter_stop(e);
}

int nvmeib_public_kth_wait_timeout_stop(
	long timeout, struct nvmeib_public_kth_event **e)
{
	return nvmeib_public_kth_wait_events_timeout(filter_timeout_stop, false, e, timeout);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_timeout_stop);

static bool filter_resume_stop(struct nvmeib_public_kth_event *e)
{
	return filter_resume(e) || filter_stop(e);
}

int nvmeib_public_kth_wait_resume_stop(struct nvmeib_public_kth_event **e)
{
	return nvmeib_public_kth_wait_events(filter_resume_stop, false, e);
}
EXPORT_SYMBOL(nvmeib_public_kth_wait_resume_stop);

void nvmeib_public_kth_set_stop(void)
{
	struct nvmesh_kth *kth;

	NFIN;
	kth = __get_current();
	if (kth)
		++kth->stop_was_called;
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_kth_set_stop);

int nvmeib_public_kth_get_stop(void)
{
	struct nvmesh_kth *kth;
	int n;

	NFIN;
	kth = __get_current();
	if (kth)
		n = kth->stop_was_called;
	else
		n = -1;
	NFOUT;
	return n;
}
EXPORT_SYMBOL(nvmeib_public_kth_get_stop);

bool nvmeib_public_is_kth(void)
{
	return !!_get_current();
}
EXPORT_SYMBOL(nvmeib_public_is_kth);

bool nvmeib_public_kth_is_current(struct nvmeib_public_kth_id id)
{
	struct nvmesh_kth *kth;
	bool rv;

	kth = find_kth(id.t);
	if (kth)
		rv = kth->t == current;
	else
		rv = false;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kth_is_current);

bool nvmeib_public_kth_is_current_obj(struct nvmeib_public_kth_obj *obj)
{
	return nvmeib_public_kth_is_current(obj->kth);
}
EXPORT_SYMBOL(nvmeib_public_kth_is_current_obj);

bool nvmeib_public_kth_is_same_sched(struct nvmeib_public_kth_obj *obj)
{
	struct nvmesh_kth *kth;
	struct nvmesh_kth *obj_kth;
	bool same = false;

	NFIN;
	if (!(kth = __get_current()))
		goto out;
	if (!(obj_kth = find_kth(obj->kth.t)))
		goto out;
	same = kth->sched == obj_kth->sched;

out:
	NFOUT;
	return same;
}
EXPORT_SYMBOL(nvmeib_public_kth_is_same_sched);

const char * nvmeib_public_kth_get_obj_name(struct nvmeib_public_kth_obj *obj)
{
	const char *s;
	struct nvmesh_kth *obj_kth;
	if ((obj_kth = find_kth(obj->kth.t)))
		s = obj_kth->name;
	else
		s = NULL;
	return s;
}
EXPORT_SYMBOL(nvmeib_public_kth_get_obj_name);


bool nvmeib_public_kth_is_idle(
	struct nvmeib_public_kth_obj *obj, unsigned long *version)
{
	struct scheduler *sched;
	unsigned long flags;
	bool idle;

	_NFIN;
	spin_lock_irqsave(&sched_hash_guard, flags);
	if ((sched = find_sched_(obj->kth.s))) {
		unsigned long flags1;

		spin_lock_irqsave(&sched->events_guard, flags1);
		idle = sched->idle;
		if (version)
			*version = sched->idle_version;
		spin_unlock_irqrestore(&sched->events_guard, flags1);
	}
	else
		idle = true;
	spin_unlock_irqrestore(&sched_hash_guard, flags);
	_NFOUT;
	return idle;
}
EXPORT_SYMBOL(nvmeib_public_kth_is_idle);

void nvmeib_public_kth_barrier(struct nvmeib_public_kth_obj *obj)
{
	struct nvmesh_kth *kth;
	struct nvmeib_public_kth_event *e, *t;
	unsigned long flags;

	_NFIN;
	kth = __get_current();
	spin_lock_irqsave(&kth->sched->events_guard, flags);
	list_for_each_entry_safe(e, t, &kth->sched->events, link)
		if (e->id.tns == obj->kth.tns && e->type != nke_stop) {
			list_del(&e->link);
			list_add_tail(&e->link, &kth->events);
		}
	spin_unlock_irqrestore(&kth->sched->events_guard, flags);
	free_kth_events(&obj->events);
	free_kth_events(&kth->events);
	_NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_kth_barrier);

/*
 * format output for proc file
 * OS-pid		 kth{sched,thread}		OS-name		kth-name
 *
 * TODO(EBA):
 * 1) designate each kth as owned by nvmeib{c,s} & dump only kth of the module to its proc file
 */
ssize_t nvmeib_public_kth_format_proc_table(void *dummy, char *buffer, size_t len)
{
	#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	int count = 0;
	struct nvmesh_kth *kth = NULL;
	int t;
	unsigned long flags;

	(void)dummy;

	NFIN;
	BUF_ADD("OS-pid\t\tkth{sched,thread}\t\tcreator{sched,thread}\t\tOS-name\t\tkth-name\n");
	flags = hash_lock_guard();
	hash_for_each(kth_hash, t, kth, hlist) {
		BUF_ADD("%5u\t\t%8x:%8x\t\t%8x:%8x\t\t%s\t\t%s\n",
			kth->t->pid, kth->sguid, kth->guid,
			kth->creator_sguid, kth->creator_guid,
			kth->t->comm, kth->name);
	}
	hash_unlock_guard(flags);
	NFOUT;

	return count;
	#undef BUF_ADD
}
EXPORT_SYMBOL(nvmeib_public_kth_format_proc_table);

int nvmeib_fire_simple_event(struct nvmeib_public_kth_id id,
			     int evtype, void *priv)
{
	struct simple_kth_event *ske;
	int rv = 0;

	ske = kzalloc(sizeof(*ske), GFP_ATOMIC);
	if (!ske) {
		_NE(error_nvmeib_public_kth_nvmeib_fire_simple_event, "Failed to allocate kth event for @NAME",
		   nvmeib_public_kth_get_name());
		return -ENOMEM;
	}

	ske->priv = priv;
	ske->e.type = evtype;
	ske->e.id = id;

	rv = nvmeib_public_kth_add_event(&ske->e);
	if (rv) {
		_NE(error_1_nvmeib_public_kth_nvmeib_fire_simple_event, "No recipient for kth event message @TOPOLOGY_INT", ske->e.id.t);
		kfree(ske);
	}

	return rv;
}
EXPORT_SYMBOL(nvmeib_fire_simple_event);

int nvmeib_fire_simple_event_wait(struct nvmeib_public_kth_id id,
				  int evtype, void *priv)
{
	struct simple_kth_event ske = {
		.priv = priv,
		.e.type = evtype,
		.e.id = id,
	};
	int rv = 0;

	rv = nvmeib_public_kth_add_event_sync(&ske.e);
	if (rv)
		_NE(error_nvmeib_public_kth_nvmeib_fire_simple_event_wait, "No recipient for kth event message @NVMEIB_KTH_EVENT_TO_STR (@EVTYPE) to @TOPOLOGY_INT",
				   nvmeib_kth_event_to_str(evtype), evtype, ske.e.id.t);

	return rv;
}
EXPORT_SYMBOL(nvmeib_fire_simple_event_wait);

struct kth_generic_link * nvmeib_public_kth_get_link(void)
{
	struct nvmesh_kth *kth;
	struct kth_generic_link *link;

	_NFIN;
	if ((kth = __get_current()) && (link = sched_get_link(kth->sched))) {
		link->id.t = kth->guid;
		link->id.s = kth->sched->guid;
	}
	else
		link= NULL;
	_NFOUT;
	return link;
}
EXPORT_SYMBOL(nvmeib_public_kth_get_link);

int nvmeib_public_kth_put_link(struct kth_generic_link *link)
{
	struct nvmesh_kth *kth;

	_NFIN;
	if ((kth = __get_current())) {
		link->id.tns = 0;
		sched_put_link(kth->sched, link);
	}
	else {
		/* This could happend in a valid flow, where the waiter is in a kth
		 * context and the wakeup is called from a non kth context
		 */
		_NF(trace_nvmeib_public_kth_nvmeib_public_kth_put_link, "Current is not a KTH - deleting link");
		kfree(link);
	}
	_NFOUT;
	return 0;
}
EXPORT_SYMBOL(nvmeib_public_kth_put_link);

