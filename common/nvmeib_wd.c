/*
    Watchdog
    Use case:
    1. Non responsive hardware (a command that doesn't return)
    2. The exact correct timeout is unknown, we take large margins, and
       check it quite unfrequently (once a second)
    3. Thousands of in-work commands. Very high frequency of work.
       This implies that we also keep the watchdog thread alive.
       (Every WD object had a thread of its own)
    4. Rarely happens, hence the add/remove should be cheap
    Design:
    1. Counting the pending IOs
	- Every CPU counts its own (submit-completion) in wd_per_cpu
	  per time slices.
	- Since every CPU only updates its own counter, it is lockless.
	- The sum of all CPUs on all the old entries should be 0
    2. Every pending IO is registered in , including the submission time
       If an old sum-of-counters is not zero then something is broken and all
       the pending IOs are scanned, looking for stuck entries
*/

#include <linux/percpu.h>

#include "nvmeib_wd.h"
#include "nvmeib_utils.h"
#include "nvmeibm_trace.h"

struct wd_obj {
	struct list_head entries;
	spinlock_t spinlock;
	struct task_struct *thread;
	unsigned long interval_ms;
//	int *__percpu percpu_buckets;
	int *percpu_buckets;
	long last_jiffies;
	int timeout_sec;
	int timeout_hz;
	int n_buckets;
	int hz_per_bucket;
	/*an iterator to the global wd's list*/
	struct list_head entry;
	/* and we would like to know which process locked it */
	int locking_pid;
	/* CPU for per-cpu WDs */
	int pcpu_cpu;
	/* debug */
	unsigned long last_wakeup;
};

#define wd_is_pcpu(wd) ((wd)->pcpu_cpu >= 0 && (wd)->pcpu_cpu < NVMEIB_DFLT_MAX_CPUS && (wd)->pcpu_cpu != NVMEIB_CPU_INVALID)
#define wd_pcpu_get_cpu(wd) ((wd->pcpu_cpu))

bool nvmeib_wd_is_pcpu(struct wd_obj* wd)
{
	return wd_is_pcpu(wd);
}
EXPORT_SYMBOL(nvmeib_wd_is_pcpu);

int nvmeib_wd_pcpu_get_cpu(struct wd_obj *wd)
{
	return wd_pcpu_get_cpu(wd);
}
EXPORT_SYMBOL(nvmeib_wd_pcpu_get_cpu);

static inline bool wd_already_locked(struct wd_obj *wd);
static inline void wd_lock_irq_disable(struct wd_obj *wd, unsigned long *pflags);
static inline void wd_unlock_irq_restore(struct wd_obj *wd, unsigned long flags);

/**
 * list of all wd's
 *
 * @param
 */
struct wd_globals {
	struct mutex guard;
	/*remeber all the active watchdogs*/
	struct list_head wd_list;
} all_wd_globals;

/**
 * add the wd to the general list of wd's
 *
 * @param wd - newwd to add
 */
static void wd_remember_wd(struct wd_obj *wd) {
	NFIN;

	mutex_lock(&all_wd_globals.guard);
	_ND(trace_nvmeib_wd_wd_remember_wd, "Adding new wd into the working threads");
	list_add_tail(&wd->entry, &all_wd_globals.wd_list);
	_ND(trace_1_nvmeib_wd_wd_remember_wd, "OK did that");
	mutex_unlock(&all_wd_globals.guard);
	NFOUT;
}

/**
 * remove the wd from the gloabal list
 *
 * @param wd - object to remove
 */
static void wd_forget_wd(struct wd_obj *wd)
{
	NFIN;
	mutex_lock(&all_wd_globals.guard);
	list_del_init(&wd->entry);
	mutex_unlock(&all_wd_globals.guard);
	NFOUT;
}

static inline int get_bucket_no(struct wd_obj *wd, unsigned long time_jiffies) {
	/* todo - we need to restrict n_buckets and the hz_per_bucket to be power
	   of 2 and use bit operation
	*/
	int ret = (time_jiffies / wd->hz_per_bucket) % wd->n_buckets;
	return ret;
}

static void nvmeib_wd_del_entry_(struct nvmeib_wd_entry *e)
{
	NFIN;
	if (e->wd_registered) {
		if (list_empty(&e->entry)) {
			_NE(error_nvmeib_wd_nvmeib_wd_del_entry_, "e=@ENTRY NOT linked", e);
			BUG();
		}

		list_del_init(&e->entry);
		e->wd_registered = false;
	}
	else {
		_NW(warn_nvmeib_wd_nvmeib_wd_del_entry_, "attempt to unregister entry which is not registered");
		BUG();
	}
	NFOUT;
}

static void full_scan(struct wd_obj *wd, unsigned long now)
{
	struct nvmeib_wd_entry *n, *tn;
	unsigned long start, fn_start, duration;
	unsigned long flags;

	NFIN;
	start = jiffies;

	wd_lock_irq_disable(wd, &flags);
	list_for_each_entry_safe(n, tn, &wd->entries, entry) {
		if (n->function) {
			fn_start = jiffies;
			n->function(n->data, now); /* handle_watchdog_event */
			duration = jiffies - fn_start;
			if (duration >= HZ)
				_NE(error_nvmeib_wd_full_scan, "WD handler '@FUNCTION_PTR' took too long: @DURATION, @DURATION "
				   "[sec, jiffies]", n->function, duration, duration/HZ);
		}
	}
	wd_unlock_irq_restore(wd, flags);

	duration = jiffies - start;
	if (duration >= 2*HZ)
		_NE(error_1_nvmeib_wd_full_scan, "Full scan took too long:"
		   "@DURATION, @DURATION [sec, jiffies]", duration, duration/HZ);
	NFOUT;
	/* TODO - we need to restrict n_buckets to be power of 2 and use bit operation*/
}

/**
 * debug function to list all active threads
 *
 * @param
 */
static void print_all_threads(void)
{
	struct wd_obj *wd;
	NFIN;
	// [Needed at exit?] mutex_lock(&all_wd_globals.guard);
	list_for_each_entry(wd, &all_wd_globals.wd_list, entry) {
		_ND(trace_nvmeib_wd_print_all_threads, "a thread in @THREAD", wd->thread);
	}
	NFOUT;
}

/**
 * the main watch dog loop thread function
 * each watch dog obj have a thread that loops in this function
 *
 * @param arg
 *
 * @return int
 */
static int watchdog_loop_thread_function(void *arg)
{
	struct wd_obj *wd = arg;
	unsigned long now, end_of_scan_jiffies;
	int i, j, end_of_scan;
	unsigned long tot;
	unsigned long time_from_last, timeout_hz;
	int sleep_time = HZ * max(wd->hz_per_bucket / HZ, 1);
	int tot_per_bucket;

	NFIN;
	/* Hibernation / freezing of the WD kernel thread is not supported. */
	current->flags |= PF_NOFREEZE;
	_ND(trace_nvmeib_wd_watchdog_loop_thread_function, "WD: kernel thread (PID @PID) started",
	   current->pid);

	wd->last_wakeup = jiffies;
	while (!kthread_should_stop()) {
		now = jiffies;
		if (now - wd->last_wakeup > 2*HZ) {
			_NE(error_nvmeib_wd_watchdog_loop_thread_function, "WD: last wakeup (@LAST_WAKEUP), delta=@DELTA > 2 sec",
				wd->last_wakeup, (now - wd->last_wakeup) / HZ);
		}
		end_of_scan_jiffies = now - wd->timeout_hz;
		end_of_scan = get_bucket_no(wd, end_of_scan_jiffies);
		tot = 0;
		tot_per_bucket = 0;
		for (i = get_bucket_no(wd, wd->last_jiffies); i != end_of_scan;) {
			for_each_possible_cpu(j) {
				int *buckets_of_cpu_j = wd->percpu_buckets + j*wd->n_buckets;
				tot_per_bucket += buckets_of_cpu_j[i];
			}

			tot += tot_per_bucket;
			if (tot_per_bucket)
				_NT(trace_1_nvmeib_wd_watchdog_loop_thread_function, "Found @TOT_PER_BUCKET WDs in bucket @BUCKET_NO (tot @TOT_LONG)",
					tot_per_bucket, i, tot);
			tot_per_bucket = 0;

			if (++i == wd->n_buckets) /*buckets array is cyclic so we might need to wrap around*/
				i = 0;
		}
		//_ND(watchdog_loop_thread_function_d1, "so, tot is now @LU", tot);
		if (tot) {
			_NT(trace_2_nvmeib_wd_watchdog_loop_thread_function, "WD: Total of @TOTAL, scanned slots [@BUCKET_NO,@END_OF_SCAN) ", *((int *)(&tot)),
				get_bucket_no(wd, wd->last_jiffies), end_of_scan);
			// Run full_scan only if tot does not include recent counters
			// The problem occurs if jiffies wrapped oround the bucket of
			// last_jiffies
			time_from_last = jiffies - wd->last_jiffies;
			timeout_hz = (wd->n_buckets - 2) * wd->hz_per_bucket;
			if (true || time_from_last < timeout_hz) {
				full_scan(wd, now);
			}
			wd->last_jiffies = jiffies - wd->timeout_hz;
		}
		else {
			wd->last_jiffies = end_of_scan_jiffies;
		}
		wd->last_wakeup = jiffies;
		set_current_state(TASK_INTERRUPTIBLE);
		// match the timeout to the bucket "size"
		// schedule_timeout(round_jiffies_relative(wd->hz_per_bucket));
		schedule_timeout(sleep_time - (jiffies + 3*raw_smp_processor_id())%HZ);
	}
	_ND(trace_3_nvmeib_wd_watchdog_loop_thread_function, "Kernel thread (PID @PID) stopped", current->pid);
	NFOUT;
	return 0;
}

/**
 * destroy a given wd
 *
 * @param wd - wd to unregister and destroy
 */
void nvmeib_wd_remove(struct wd_obj *wd)
{
	NFIN;
	if (wd) {
//		free_wd_percpu_buckets(wd);
		if (wd->thread) {
			_ND(trace_nvmeib_wd_nvmeib_wd_remove, "stopping thread @THREAD", wd->thread);
			kthread_stop(wd->thread);
			_ND(trace_1_nvmeib_wd_nvmeib_wd_remove, "OK thread was stopped");
		}
		kfree(wd->percpu_buckets);
		wd_forget_wd(wd);
	}
	// TODO: Make certain that the wd->entry is deleted
	kfree(wd);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_wd_remove);

/**
 * create a new watch dog
 *
 *
 * @param timeout_sec
 * @param n_buckets
 * @param hz_per_bucket
 *
 * @return struct wd_obj*
 */
struct wd_obj* nvmeib_wd_create_on_cpu(unsigned int timeout_sec,
	unsigned int n_buckets, unsigned int hz_per_bucket, int cpu) {
	struct wd_obj *wd = NULL;
	int min_n_buckets;

	NFIN;
	_ND(trace_nvmeib_wd_nvmeib_wd_create, "Allocating wd_obj");
	wd = kzalloc(sizeof *wd, GFP_KERNEL);
	if (!wd) {
		_NE(error_nvmeib_wd_nvmeib_wd_create, "Failed to allocate wd_obj");
		goto out;
	}

	wd->timeout_sec = timeout_sec ?: WD_DEFAULT_TIMEOUT_SEC;
	wd->timeout_hz = wd->timeout_sec * HZ;
	wd->hz_per_bucket = hz_per_bucket ?: WD_DEFAULT_HZ_PER_BUCKET;
	min_n_buckets = (wd->timeout_hz * 11 / 10) / wd->hz_per_bucket + 2;
	if (n_buckets < min_n_buckets)
		n_buckets = min_n_buckets;
	n_buckets = round_up(n_buckets, cache_line_size()/sizeof(int));
	wd->n_buckets = (n_buckets == 0 ? min_n_buckets : n_buckets);
	// alloc_percpu_buckets(wd, n_buckets);
	wd->percpu_buckets = kzalloc(
		n_buckets * nr_cpu_ids * sizeof(int), GFP_KERNEL);
	if (!wd->percpu_buckets) {
		_NE(error_1_nvmeib_wd_nvmeib_wd_create, "Failed to allocate wd->percpu_buckets");
		goto free_wd;
	}
	spin_lock_init(&wd->spinlock);
	INIT_LIST_HEAD(&(wd->entries));
	INIT_LIST_HEAD(&wd->entry);
	wd->locking_pid = -1;

	_ND(trace_1_nvmeib_wd_nvmeib_wd_create, "Creating thread for WD. It will run forever.");
	
	if (cpu < 0) {
		wd->thread = kthread_run(watchdog_loop_thread_function, wd, "%s", proc_name_format("M", "WD", "nvmeibWD"));
	} else {
		proc_name_t pname;
		proc_name_format_extd(pname, 'M', "WD", "nvmeibWD", cpu);
		if (!IS_ERR(wd->thread = nvmeib_kthread_create_on_cpu(watchdog_loop_thread_function, wd, cpu, pname))) {
			wake_up_process(wd->thread);
		}
	}
	_ND(trace_2_nvmeib_wd_nvmeib_wd_create, "thread created this is the pointer value: @THREAD", wd->thread);

	print_all_threads();
	if (IS_ERR(wd->thread)) {
		_NE(error_2_nvmeib_wd_nvmeib_wd_create, "Failed to create WD kernel thread @PTR_ERR", PTR_ERR(wd->thread));
		wd->thread = NULL;
		goto free_buckets;
	}
	wd->pcpu_cpu = cpu;
	wd_remember_wd(wd);
	goto out;

free_buckets:
	kfree(wd->percpu_buckets);

free_wd:
	kfree(wd);
	wd = NULL;

out:
	NFOUT;
	return wd;
}
EXPORT_SYMBOL(nvmeib_wd_create_on_cpu);

static inline bool wd_already_locked(struct wd_obj *wd)
{
	if (wd_is_pcpu(wd)) {
		if (preemptible())
			return false;
		BUG_ON(wd_pcpu_get_cpu(wd) != smp_processor_id());
		return wd->locking_pid == current->pid;
	}

	return irqs_disabled() && wd->locking_pid == current->pid;
}

static inline void wd_lock_irq_disable(struct wd_obj *wd, unsigned long *pflags)
{
	if (wd_is_pcpu(wd)) {
		BUG_ON(get_cpu() != wd_pcpu_get_cpu(wd));
		local_irq_save(*pflags);
	} else {
		spin_lock_irqsave(&wd->spinlock, *pflags);
	}
	wd->locking_pid = current->pid;
}

static inline void wd_unlock_irq_restore(struct wd_obj *wd, unsigned long flags)
{
	wd->locking_pid = -1;
	if (wd_is_pcpu(wd)) {
		BUG_ON(smp_processor_id() != wd_pcpu_get_cpu(wd));
		local_irq_restore(flags);
		put_cpu();
	} else {
		spin_unlock_irqrestore(&wd->spinlock, flags);
	}
}

void nvmeib_wd_add_entry(struct wd_obj *wd, struct nvmeib_wd_entry *e)
{
	unsigned long flags = 0;
	bool already_locked;

	NFIN;
	if (!(already_locked = wd_already_locked(wd))) {
		wd_lock_irq_disable(wd, &flags);
	}

	if (e->wd_registered) {
		_NE(error_nvmeib_wd_nvmeib_wd_add_entry, "e=@ENTRY already registered", e);
		BUG();
	}
	if (!list_empty(&e->entry)) {
		_NE(error_1_nvmeib_wd_nvmeib_wd_add_entry, "e=@ENTRY already linked", e);
		BUG();
	}

	list_add_tail(&e->entry, &wd->entries);
	e->wd_registered = true;
	if (!already_locked) {
		wd_unlock_irq_restore(wd, flags);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_wd_add_entry);

void nvmeib_wd_del_entry(struct wd_obj *wd, struct nvmeib_wd_entry *e)
{
	unsigned long flags = 0;
	bool already_locked;

	NFIN;
	if (wd) {
		if (!(already_locked = wd_already_locked(wd))) {
			wd_lock_irq_disable(wd, &flags);
		}
		nvmeib_wd_del_entry_(e);
		if (!already_locked) {
			wd_unlock_irq_restore(wd, flags);
		}
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_wd_del_entry);

void nvmeib_wd_mod_thiscpu_bucket(struct wd_obj *wd, unsigned long jif, int n)
{
	int *buckets_of_cpu;
	int cpu;
	cpu = get_cpu();
	buckets_of_cpu = wd->percpu_buckets + cpu*wd->n_buckets;
	/*
	   For the case the += does not compiled into a single instruction
	*/
	/* asm volatile ("addl %1,%0" : "+m"(buckets_of_cpu[get_bucket_no(wd, jif)]) : "r"(n))  */
	buckets_of_cpu[get_bucket_no(wd, jif)] += n;
	put_cpu();
}
EXPORT_SYMBOL(nvmeib_wd_mod_thiscpu_bucket);

int nvmeib_wd_init(void) {
    NFIN;
	mutex_init(&all_wd_globals.guard);
	INIT_LIST_HEAD(&all_wd_globals.wd_list);
	NFOUT;
	return 0;	// Assuming that the two inits cannot fail
}
EXPORT_SYMBOL(nvmeib_wd_init);

/*
    Release the memory of all the active watchdogs
*/
void nvmeib_wd_exit(void) {
	struct wd_obj *n, *tn;
	NFIN;
	print_all_threads();
	// [Needed at exit?] mutex_lock(&all_wd_globals.guard);
	list_for_each_entry_safe(n, tn, &all_wd_globals.wd_list, entry) {
		nvmeib_wd_remove(n);
	}
	// mutex_unlock(&wd->guard);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_wd_exit);

static inline bool wdc_is_pcpu(struct wd_info_common *c)
{
	return c->pcpu_cpu >= 0 && c->pcpu_cpu < NVMEIB_DFLT_MAX_CPUS && c->pcpu_cpu != NVMEIB_CPU_INVALID;
}

static inline int wdc_pcpu_get_cpu(struct wd_info_common *c)
{
	return c->pcpu_cpu;
}

static inline bool wdc_already_locked(struct wd_info_common *c)
{
	if (wdc_is_pcpu(c)) {
		if (preemptible())
			return false;
		BUG_ON(smp_processor_id() != wd_pcpu_get_cpu(c));
		return c->locking_pid == current->pid;
	}
	/* work assumption: wdc is always locked with irqsave */
	return irqs_disabled() && c->locking_pid == current->pid;
}

static inline void wdc_spin_lock_irqsave(struct wd_info_common *c,
	unsigned long *pflags)
{
	if (wdc_is_pcpu(c)) {
		BUG_ON(get_cpu() != wdc_pcpu_get_cpu(c));
		local_irq_save(*pflags);
	} else {
		spin_lock_irqsave(&c->wdc_guard, *pflags);
	}
	c->locking_pid = current->pid;
}

static inline void wdc_spin_unlock_irqrestore(struct wd_info_common *c,
	unsigned long flags)
{
	c->locking_pid = -1;
	if (wdc_is_pcpu(c)) {
		BUG_ON(smp_processor_id() != wdc_pcpu_get_cpu(c));
		local_irq_restore(flags);
		put_cpu();
	} else {
		spin_unlock_irqrestore(&c->wdc_guard, flags);
	}
}

static int handle_watchdog_event(void *cntx, unsigned long now)
{
	struct wd_info_common *c = cntx;
	int time_passed;
	unsigned long flags = 0;
	bool rv;

	//NFIN;
	time_passed = now - c->called_on;
	if (time_passed <= 0) {
		_ND(trace_nvmeib_wd_handle_watchdog_event, "Too early watchdog call - timepassed was @INT", time_passed);
		goto out;
	}
	rv = c->on_start(c->cntx);
	wdc_spin_lock_irqsave(c, &flags);
	if (c->watchdog_armed == 1) {
		if (rv) {
			/* handle_watchdog_event_locks_ch
			   handle_watchdog_event_nordda
			   handle_watchdog_event_rdda
			*/
			if (c->process(c->cntx, time_passed) > 0) {
				nvmeib_wd_bucket_dec(c->wd, c->last_used);
				c->last_used = now;
				nvmeib_wd_bucket_inc(c->wd, c->last_used);
				c->n_events++;
			}
			else {
				if (c->watchdog_armed) {
					_ND(trace_1_nvmeib_wd_handle_watchdog_event, "WD @CONTEXT, user implicit disarm request", c);
					nvmeib_wd_bucket_dec(c->wd, c->last_used);
					c->watchdog_armed = 0;
				}
				else {
					/* This is not used by any channel anymore ... */
					_NE(trace_2_nvmeib_wd_handle_watchdog_event, "WD @CONTEXT, user disarmed (and dec bucket)", c);
					WARN_ON_ONCE(1);
				}
			}
		}
	}
	else if (c->watchdog_armed != 0) {
		_NW(warn_nvmeib_wd_handle_watchdog_event, "WD @CONTEXT, armed=@ARMED", c, c->watchdog_armed);
		WARN_ON(1);
	}
	wdc_spin_unlock_irqrestore(c, flags);
	c->on_end(c->cntx);

out:
	//OUT;
	return 0;
}

void nvmeib_wd_init_wdc(struct wd_info_common *c)
{
	struct nvmeib_wd_entry *e;
	NFIN;
	c->watchdog_entry.function = handle_watchdog_event;
	c->watchdog_entry.data = c;
	c->last_used = 0;
	spin_lock_init(&c->wdc_guard);
	c->locking_pid = -1;
	c->pcpu_cpu = wd_pcpu_get_cpu(c->wd);

	/* init for catching list corruption */
	e = &c->watchdog_entry;
	INIT_LIST_HEAD(&e->entry);
	e->wd_registered = false;
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_wd_init_wdc);

static void wd_add_wdc_fn(void *ctx)
{
	struct wd_info_common *c = ctx;
	NFIN;
	nvmeib_wd_stop_wdc(c);
	if (!c->watchdog_entry.wd_registered)
		nvmeib_wd_add_entry(c->wd, &c->watchdog_entry);
	NFOUT;
}

void nvmeib_wd_add_wdc(struct wd_info_common *c)
{
	if (wdc_is_pcpu(c)) {
		/* Schedule IPI on to run wd_add_wdc_fn on correct CPU */
		BUG_ON(irqs_disabled() || in_interrupt());
		smp_call_function_single(wdc_pcpu_get_cpu(c), wd_add_wdc_fn, c, true);
	} else {
		wd_add_wdc_fn(c);
	}
}
EXPORT_SYMBOL(nvmeib_wd_add_wdc);

static void wd_remove_wdc_fn(void *ctx)
{
	struct wd_info_common *c = ctx;
	struct nvmeib_wd_entry *entry;
	
	NFIN;
	nvmeib_wd_stop_wdc(c);
	entry = &c->watchdog_entry;
	if (entry->wd_registered)
		nvmeib_wd_del_entry(c->wd, entry);
	NFOUT;
}

void nvmeib_wd_remove_wdc(struct wd_info_common *c)
{
	if (wdc_is_pcpu(c) && smp_processor_id() != wdc_pcpu_get_cpu(c)) {
		/* Schedule IPI on to run wd_remove_wdc_fn on correct CPU */
		BUG_ON(irqs_disabled() || in_interrupt());
		smp_call_function_single(wdc_pcpu_get_cpu(c), wd_remove_wdc_fn, c, true);
	} else {
		wd_remove_wdc_fn(c);
	}
}
EXPORT_SYMBOL(nvmeib_wd_remove_wdc);

/* Start and Stop APIs should not be called directly by WD but
   may be called indirectly from WD thread through WD's client
   event handler function where wdc-guard is already taken */
void nvmeib_wd_start_wdc(struct wd_info_common *c)
{
	unsigned long flags = 0;
	bool already_lock = wdc_already_locked(c);
	NFIN;

	if (!already_lock)
		wdc_spin_lock_irqsave(c, &flags);

	c->called_on = jiffies;
	c->last_used = c->called_on;
	c->n_events = 0;
	if (c->watchdog_armed) {
		_NW(warn_nvmeib_wd_nvmeib_wd_start_wdc, "WD @CONTEXT, armed=@ARMED", c, c->watchdog_armed);
		WARN_ON(1);
	}
	nvmeib_wd_bucket_inc(c->wd, c->last_used);
	c->watchdog_armed = 1;

	if (!already_lock)
		wdc_spin_unlock_irqrestore(c, flags);

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_wd_start_wdc);

void nvmeib_wd_stop_wdc(struct wd_info_common *c)
{
	unsigned long flags = 0;
	bool already_lock = wdc_already_locked(c);
	NFIN;

	if (!already_lock)
		wdc_spin_lock_irqsave(c, &flags);

	if (c->watchdog_armed == 1) {
		if (c->wd) {
			nvmeib_wd_bucket_dec(c->wd, c->last_used);
		}
		_ND(trace_nvmeib_wd_nvmeib_wd_stop_wdc, "WD @CONTEXT, explicit disarm", c);
		c->watchdog_armed = 0;
	}
	else if (c->watchdog_armed != 0) {
		_NW(warn_nvmeib_wd_nvmeib_wd_stop_wdc, "WD @CONTEXT, armed=@ARMED", c, c->watchdog_armed);
		WARN_ON(1);
	}

	if (!already_lock)
		wdc_spin_unlock_irqrestore(c, flags);

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_wd_stop_wdc);

bool nvmeib_wd_is_armed_wdc(struct wd_info_common *c)
{
	return !!c->watchdog_armed;
}
EXPORT_SYMBOL(nvmeib_wd_is_armed_wdc);
