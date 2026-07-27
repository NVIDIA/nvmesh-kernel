#ifndef KR_INC_UNITEST_INCS_H
#define KR_INC_UNITEST_INCS_H
/*
 * 1. Unitests of kernel simulator
 */
#include "kr_incs.h"

/* Execute various infrastructure code tests (a function, class, etc.)
 * These tests require the kernel to be up (i.e.: fully initialized) */
void test_kernel_infra(struct kernel_sim *kernel);
#endif

// For documentation, see Header in H file
#include "nvmeibc_simu_disk.h" // Ugly



/***************** testing kth signaling **********************/
/*
 * create multiple thread that will be awakend after some time & then they will
 * signal & wait for each other, as if a token as passed among them all (as in
 * kth library scheduler)
 */
#define TEST_KTH_LIB_SIG_NUM_THREADS	5	// number of threads in test
#define TEST_KTH_LIB_SIG_RUN_TIME		100	// number of seconds for the toekn passing game to be executed

struct kth_signaling_test {
	struct spinlock		switch_to_guard;
	struct task_struct	*creator_thread;
	wait_queue_head_t 	creator_wq;
	struct ut_kth {
		struct task_struct	*task;
		wait_queue_head_t 	wqh;
		enum my_state {
			ut_kth_init,
			ut_kth_suspended,
			ut_kth_running,
			ut_kth_exiting,
		} state;
	} kth[TEST_KTH_LIB_SIG_NUM_THREADS];
} kth_signaling_test;


static int kth_signaling_test__thread_fn(void *data)
{
	const u32		my_task_ind = (u32)(u64)data;
	struct ut_kth	*my_kth = &kth_signaling_test.kth[my_task_ind];
	const u32		next_task_ind = (my_task_ind + 1) % TEST_KTH_LIB_SIG_NUM_THREADS;
	struct ut_kth	*next_kth = &kth_signaling_test.kth[next_task_ind];
	int		i;

	_ND(trace_kr_incs_unitest_kth_signaling_test__thread_fn, "Task # @MY_TASK_IND starts running", my_task_ind);
	BUG_ON(my_task_ind >= TEST_KTH_LIB_SIG_NUM_THREADS);
	BUG_ON(my_kth->state != ut_kth_init);

	my_kth->state = ut_kth_suspended;
	wake_up(&kth_signaling_test.creator_wq);

	spin_lock(&kth_signaling_test.switch_to_guard);
	for (i=0; !kthread_should_stop() ; i++) {
		_ND(trace_1_kr_incs_unitest_kth_signaling_test__thread_fn, "Task # @MY_TASK_IND waiting for signal", my_task_ind);
		if (wait_event_interruptible_lock_irq(my_kth->wqh, my_kth->state != ut_kth_suspended, kth_signaling_test.switch_to_guard) == -1/*exit*/) {
			continue;	// we are told to exit loop
		}
		_ND(trace_2_kr_incs_unitest_kth_signaling_test__thread_fn, "Task # @MY_TASK_IND got signal", my_task_ind);
		spin_unlock(&kth_signaling_test.switch_to_guard);
		BUG_ON(my_kth->state != ut_kth_running);
		my_kth->state = ut_kth_suspended;
		msleep(1);	// working ...
		_ND(trace_3_kr_incs_unitest_kth_signaling_test__thread_fn, "Task # @MY_TASK_IND waking # @NEXT_TASK_IND", my_task_ind, next_task_ind);
		// acquire lock *before* waking up the other bcz otherwise, all threads might be awakened yet blocked on lock to call wait_*() & that would make the state a mess
		spin_lock(&kth_signaling_test.switch_to_guard);
		next_kth->state = ut_kth_running;
		wake_up(&next_kth->wqh);
	}
	spin_unlock(&kth_signaling_test.switch_to_guard);
	_ND(trace_4_kr_incs_unitest_kth_signaling_test__thread_fn, "Task # @MY_TASK_IND exiting", my_task_ind);
	my_kth->state = ut_kth_exiting;
	return 0;
}

static void test_kthread_signaling(void)
{
	struct ut_kth	*kth;
	int	i;

	spin_lock_init(&kth_signaling_test.switch_to_guard);
	kth_signaling_test.creator_thread = kthread_self_task;
	init_waitqueue_head(&kth_signaling_test.creator_wq);

	// create all threads in suspended state
	for (i=0; i < TEST_KTH_LIB_SIG_NUM_THREADS; i++) {
		kth = &kth_signaling_test.kth[i];
		kth->state = ut_kth_init;
		init_waitqueue_head(&kth->wqh);
		kth->task = kthread_create(kth_signaling_test__thread_fn, (void*)(u64)i, "ut:kth signaling/%d", i);
		spin_lock(&kth_signaling_test.switch_to_guard);
		wake_up_process(kth->task);
		_ND(trace_kr_incs_unitest_test_kthread_signaling, "Wait for Task # @TASK_IDX to initialize", i);
		wait_event_interruptible_lock_irq(kth_signaling_test.creator_wq, kth->state != ut_kth_init, kth_signaling_test.switch_to_guard);
		_ND(trace_1_kr_incs_unitest_test_kthread_signaling, "Task # @TASK_IDX signaled that it is ready", i);
		spin_unlock(&kth_signaling_test.switch_to_guard);
	}
	msleep(5);	// let all thread arrive at the starting position
	// wake up one thread to start the ping poing
	_ND(trace_2_kr_incs_unitest_test_kthread_signaling, "waking Task # 0 to start token game");
	kth = &kth_signaling_test.kth[0];
	kth->state = ut_kth_running;
	wake_up(&kth->wqh);
	sleep(TEST_KTH_LIB_SIG_RUN_TIME);	// let the token game play ...
	for (i=0; i < TEST_KTH_LIB_SIG_NUM_THREADS; i++) {
		kth = &kth_signaling_test.kth[i];
		kthread_stop(kth->task);
		_ND(trace_3_kr_incs_unitest_test_kthread_signaling, "Task # @TASK_IDX terminated", i);
	}
	spin_lock_destroy(&kth_signaling_test.switch_to_guard);
}


/***************** testing kthread sync using wait_queue **********************/
struct kthread_sync_test {
	enum kthread_sync_test_case {
		kthread_sync_test_case___condition_fail_a_few_times,
		kthread_sync_test_case___fast_signal_to_service,
	} test_case;
	struct task_struct 		*bg_thread;	// the bg thread
	wait_queue_head_t 	 	wq;			// sync object for signaling
	spinlock_t 				lock;		// consistency lock
	int						c2s;		// the data transfered from client to server (bg)
	int						s2c;		// the data transfered from server (bg) to client
#define TEST_KTHREAD_SYNC_THRESHOLD		1000
} kill_before_entering_loop;

static int kthread_wait_and_kill_bg(void *param)
{
	struct kthread_sync_test	*test_param = (struct kthread_sync_test*)param;
	unsigned long 				flags;

	_NT(trace_kr_incs_unitest_kthread_wait_and_kill_bg, "bg thread starting");
	while (!kthread_should_stop()) {
		spin_lock_irqsave(&test_param->lock, flags);
		if (test_param->c2s > TEST_KTHREAD_SYNC_THRESHOLD) {
			_NT(trace_1_kr_incs_unitest_kthread_wait_and_kill_bg, "processing event: c2s=@C2S", test_param->c2s);
			switch (test_param->test_case) {
			case kthread_sync_test_case___condition_fail_a_few_times:
				test_param->c2s = 0;    // consume msg
				test_param->s2c = 1;	// send reply
				break;
			case kthread_sync_test_case___fast_signal_to_service:
				test_param->s2c = test_param->c2s;		// send reply
				test_param->c2s = 0;					// consume msg
				break;
			default:
				BUG_ON(1);
			}
		}
		_NT(trace_2_kr_incs_unitest_kthread_wait_and_kill_bg, "waiting for event: c2s=@C2S", test_param->c2s);
		spin_unlock_irqrestore(&test_param->lock, flags);
		wait_event_interruptible(test_param->wq,
								 (test_param->c2s > TEST_KTHREAD_SYNC_THRESHOLD) || kthread_should_stop());
	}
	_NT(trace_3_kr_incs_unitest_kthread_wait_and_kill_bg, "bg thread terminating");
	return 0;
}

__attribute__ ((unused)) static void test_kthread_wait_and_kill(void) {
#define KTHREAD_START() kthread_run(kthread_wait_and_kill_bg, &test_param, bg_thread_name);
	static const char bg_thread_name[] = "kthread sync test bg thread";
	struct kthread_sync_test	test_param;
	unsigned long 		flags;
	int					step, wait_cnt;

	// init
	memset(&test_param, 0, sizeof(test_param));
	init_waitqueue_head(&test_param.wq);
	spin_lock_init(&test_param.lock);
	test_param.bg_thread = KTHREAD_START();
	BUG_ON(test_param.bg_thread == NULL);
	// sleep to let bg get into wait loop.
	usleep(1000);
	// test 1: change only c2s until it crosses threshold to wake bg thread
	test_param.test_case = kthread_sync_test_case___condition_fail_a_few_times;
#define NUM_STEPS	5
	BUG_ON(test_param.s2c != 0);	// no reply for now
	for (step=0; step <= NUM_STEPS; step++) {
		spin_lock_irqsave(&test_param.lock, flags);
		test_param.c2s += TEST_KTHREAD_SYNC_THRESHOLD / NUM_STEPS;
		_NT(trace_kr_incs_unitest_test_kthread_wait_and_kill, "update c2s: c2s=@C2S", test_param.c2s);
		spin_unlock_irqrestore(&test_param.lock, flags);
		usleep(1000);
	}
	usleep(HZ/3);// sleep more than wait() API, to give it a chance to wake, consume msg & reply
	spin_lock_irqsave(&test_param.lock, flags);
	BUG_ON(test_param.s2c != 1);
	spin_unlock_irqrestore(&test_param.lock, flags);

	// test 2: now lest do a fast msg/reply loop to see it wakes up immediately when we signal to it
	// 			msg is *below* threshold so the condition wont wake the bg thread
	_NT(trace_1_kr_incs_unitest_test_kthread_wait_and_kill, "fast request -> service latency test");
	test_param.test_case = kthread_sync_test_case___fast_signal_to_service;
	spin_lock_irqsave(&test_param.lock, flags);
	test_param.s2c = 0;
	spin_unlock_irqrestore(&test_param.lock, flags);
	for (step=TEST_KTHREAD_SYNC_THRESHOLD+1; step < TEST_KTHREAD_SYNC_THRESHOLD + 100; step++) {
		spin_lock_irqsave(&test_param.lock, flags);
		test_param.c2s = step;
		BUG_ON(test_param.c2s < TEST_KTHREAD_SYNC_THRESHOLD);
		_NT(trace_2_kr_incs_unitest_test_kthread_wait_and_kill, "update c2s: c2s=@C2S", test_param.c2s);
		spin_unlock_irqrestore(&test_param.lock, flags);
		wake_up(&test_param.wq);	// wake up bg
		// now wait for reply
		spin_lock_irqsave(&test_param.lock, flags);
		wait_cnt = 0;
		while (test_param.s2c != step) {
			spin_unlock_irqrestore(&test_param.lock, flags);
			usleep(10);
			wait_cnt++;
			spin_lock_irqsave(&test_param.lock, flags);
		}
		test_param.s2c = 0; // consume reply under lock
		spin_unlock_irqrestore(&test_param.lock, flags);
		_NT(trace_3_kr_incs_unitest_test_kthread_wait_and_kill, "waited @WAIT_CNT rounds for service by bg thread. reply=@REPLY", wait_cnt, test_param.s2c);
	}

	// test 3:
	_NT(trace_4_kr_incs_unitest_test_kthread_wait_and_kill, "sleep some time to ensure bg doesnt do anything until we ask it to stop");
	sleep(1);

	// now kill
	_NT(trace_5_kr_incs_unitest_test_kthread_wait_and_kill, "ask bg thread to terminate");
	kthread_stop(test_param.bg_thread);
	// cleanup
	spin_lock_destroy(&test_param.lock);
}

/* test the per-cpu emulation
 * the test creates a number of threads, 3 times the number of processors so we have about 3 threads contenting for every processor.
 * the threads acquire the percpu & then yield a few times to allow another one to try to take the same cpu.
 * a global state allows them all to verify that only one has managed to take every cpu at any given time.
 * NOTE: modify the macro which sets the number of emulated processors to increase/decrease contention.
 */
// with about 2 threads per cpu, we ensure contention on every emulated processot
#define TEST_PERCPU_THREADS			(3 * CONFIG_NR_CPUS)
#define TEST_PERCPU_ITERATIONS		1000	// test iterations
#define TEST_PERCPU_MAX_YIELD		5		// max number of yields while holding the cpu

struct test_percpu_param {
	atomic_t			*is_cpu_in_use;	// shared state for all threads
	int					thread_idx;
	struct task_struct	*task;	// BEWARE: dont use this in worker thread, as it might start running before this member is set
};

static int test_percpu_thread(void		*param)
{
	struct test_percpu_param		*cpu_ctx = (struct test_percpu_param*)param;
	int		iter, my_cpu, num_yield;

	my_cpu = get_cpu();
	_NT(trace_kr_incs_unitest_test_percpu_thread, "task @THREAD_ID running on @CPU", cpu_ctx->thread_idx, my_cpu); // trace once bcz threasd are bound to cpu upon creation
	BUG_ON(atomic_read(&cpu_ctx->is_cpu_in_use[my_cpu]) != false);
	put_cpu();
	for (iter=0; iter < TEST_PERCPU_ITERATIONS; iter ++) {
		my_cpu = get_cpu();
		_NT(trace_1_kr_incs_unitest_test_percpu_thread, "Thread @THREAD_ID has @CPU", cpu_ctx->thread_idx, my_cpu);
		BUG_ON(atomic_read(&cpu_ctx->is_cpu_in_use[my_cpu]) != false);
		atomic_set(&cpu_ctx->is_cpu_in_use[my_cpu], true);
		// yield to allow other to try to acquire the same cpu while we are bound to it
		for (num_yield = rand() % (TEST_PERCPU_MAX_YIELD+1); num_yield >= 0; num_yield--) {
			_NT(trace_2_kr_incs_unitest_test_percpu_thread, "Thread @THREAD_ID yielding...", cpu_ctx->thread_idx);
			BUG_ON(sched_yield() != 0);
		}
		_NT(trace_3_kr_incs_unitest_test_percpu_thread, "Thread @THREAD_ID releasing @CPU", cpu_ctx->thread_idx, my_cpu);
		atomic_set(&cpu_ctx->is_cpu_in_use[my_cpu], false);
		put_cpu();
		BUG_ON(sched_yield() != 0);	// let another thread have a chance
	}
	return 0;
}
static __attribute__ ((unused)) void test_percpu(void)
{
	struct test_percpu_param		percpu_ctx[TEST_PERCPU_THREADS];
	atomic_t						is_cpu_in_use[CONFIG_NR_CPUS];
	struct test_percpu_param		*cpu_ctx_iter;
	int		thread_idx;

#ifndef SCHED_NEW_KTHREAD_ROUND_ROBIN
	// ***WATCHOUT*** : with kthread assignment to unused cpu's, we cannot create more thareads than cpus's - use RR assignment to use this test bcz its the the whole point here is having multiple threads per cpu
	BUG_ON(true);
#endif

	for (thread_idx=0, cpu_ctx_iter = percpu_ctx; thread_idx < TEST_PERCPU_THREADS; cpu_ctx_iter++, thread_idx++) {
		atomic_set(&is_cpu_in_use[thread_idx], false);
	}
	_NT(trace_kr_incs_unitest_test_percpu, "test percpu: num_cpu=@N_CPU, num_threads=@N_THREADS", CONFIG_NR_CPUS, TEST_PERCPU_THREADS);
	for (thread_idx=0, cpu_ctx_iter = percpu_ctx; thread_idx < TEST_PERCPU_THREADS; cpu_ctx_iter++, thread_idx++) {
		cpu_ctx_iter->is_cpu_in_use = is_cpu_in_use;
		cpu_ctx_iter->thread_idx = thread_idx;
		cpu_ctx_iter->task = kthread_run(test_percpu_thread, cpu_ctx_iter, "percpu ut");
	}
	// let workers run & fight over cpu's. wait until they are done
	for (thread_idx=0, cpu_ctx_iter = percpu_ctx; thread_idx < TEST_PERCPU_THREADS; cpu_ctx_iter++, thread_idx++) {
		kthread_stop(cpu_ctx_iter->task);
	}
}

//---------------------------------------------------------------------------------------------------------------------
#include "../../../common/nvmeib_scalabale_refcount.h"
/* test scenario: create multiple threads that get/put reference while the main thread (controling the test) transition state to stop allowing them to acquire a reference.
 * it also puts them on hold to verify the aggregated counter value & eventually asks them to terminate
 * code assumes that the kthreads are assigend to multiple cpu cores, to have contention & races
 */
#define SCALABALE_REFCOUNT_TEST_CPU_NUM				5 // more than my core count, more context switches :)
#define SCALABALE_REFCOUNT_TEST_NUM_ITERATION		10000	// number of tests to do
#define SCALABALE_REFCOUNT_TEST_MAX_WORKER_USLEEP	2 // maximun number of used the worker will sleep

struct scalabale_refcount_test_param {
	atomic_t	state;
#define SCALABALE_REFCOUNT_TEST_STATE_GET_REF_ALLOWED		1 // get_ref is allowed
#define SCALABALE_REFCOUNT_TEST_STATE_GET_REF_DENIED		2 // get_ref is NOT allowed
#define SCALABALE_REFCOUNT_TEST_STATE_WORKER_DONT_BOTHER	3 // workers stop attempting to acquire, so we can assert invariants !!!
#define SCALABALE_REFCOUNT_TEST_STATE_TERM					4 // terminating worker threads -> get_ref is not allowed
	struct scalabale_refcount	refcount;
};

static bool test_scalabale_refcount_cond_is_terminating(struct scalabale_refcount	*s,
														void						*ctx)
{
	struct scalabale_refcount_test_param	*test_param = (struct scalabale_refcount_test_param*)ctx;
	int  state = atomic_read(&test_param->state);
	(void)s;
	switch (state) {
	case SCALABALE_REFCOUNT_TEST_STATE_GET_REF_ALLOWED:		// get_ref is ok
		return false;
	case SCALABALE_REFCOUNT_TEST_STATE_GET_REF_DENIED:		// get_ref is not allowed
	case SCALABALE_REFCOUNT_TEST_STATE_WORKER_DONT_BOTHER:
	case SCALABALE_REFCOUNT_TEST_STATE_TERM:
		return true;
	default:
		BUG_ON(true);
		return true;
	}
}
static int test_scalabale_refcount_worker(void *p)
{
	struct scalabale_refcount_test_param	*test_param = (struct scalabale_refcount_test_param	*)p;
	int		ref_taken, ref_denied, ref_count_val;
	bool	is_ref_taken;

	_NT(trace_kr_incs_unitest_test_scalabale_refcount_worker, "pthread using @CPU starts...", kthread_self_task->cpu_idx);
	ref_taken = 0;
	ref_denied = 0;
	do {
		is_ref_taken = scalabale_refcount_get_ref(&test_param->refcount);
		usleep(rand() % SCALABALE_REFCOUNT_TEST_MAX_WORKER_USLEEP);
		if (is_ref_taken) {
			ref_taken++;
			scalabale_refcount_put_ref(&test_param->refcount);
		} else {
			ref_denied++;
		}
		ref_count_val = scalabale_refcount_get_count(&test_param->refcount);
		BUG_ON(ref_count_val < 0);
		while (atomic_read(&test_param->state) == SCALABALE_REFCOUNT_TEST_STATE_WORKER_DONT_BOTHER) {
			usleep(10);	// dont bother, when we dont hold the reference
		}
	} while (atomic_read(&test_param->state) != SCALABALE_REFCOUNT_TEST_STATE_TERM);
	_NT(trace_1_kr_incs_unitest_test_scalabale_refcount_worker, "pthread using cpu core @CPU terminates: ref_taken=@REF_TAKEN, ref_denied=@REF_DENIED",
	   kthread_self_task->cpu_idx, ref_taken, ref_denied);
	return 0;
}
__attribute__ ((unused)) static void test_scalabale_refcount(void)
{
	struct scalabale_refcount_test_param	test_param;
	struct task_struct		*tasks[SCALABALE_REFCOUNT_TEST_CPU_NUM];
	int	i, iter;

	_NT(trace_kr_incs_unitest_test_scalabale_refcount, "Test scalabale_refcount...");
	for (iter=0; iter < SCALABALE_REFCOUNT_TEST_NUM_ITERATION ; iter++) {
		atomic_set(&test_param.state, SCALABALE_REFCOUNT_TEST_STATE_GET_REF_ALLOWED);
		scalabale_refcount_init(&test_param.refcount, test_scalabale_refcount_cond_is_terminating, &test_param);
		for (i=0; i < SCALABALE_REFCOUNT_TEST_CPU_NUM; i++) {
			tasks[i] = kthread_run(test_scalabale_refcount_worker, &test_param, "ut:scalabale counter worker");
		}
		msleep(10);
		_NT(trace_1_kr_incs_unitest_test_scalabale_refcount, "Asking scalabale_refcount to deby new ref requests");
		atomic_set(&test_param.state, SCALABALE_REFCOUNT_TEST_STATE_GET_REF_DENIED);
		scalabale_refcount_drain(&test_param.refcount);// wait for all workers to return their references
		_NT(trace_2_kr_incs_unitest_test_scalabale_refcount, "All refs were returned (workers still attempt to acquire)!!!");
		// TODO(EBA): bcz of drain, we need not wait at all !!!
		usleep(10);	/// let them fail to get the reference for a while
		_NT(trace_3_kr_incs_unitest_test_scalabale_refcount, "ask workers to stop bothering");
		atomic_set(&test_param.state, SCALABALE_REFCOUNT_TEST_STATE_WORKER_DONT_BOTHER);
		usleep(SCALABALE_REFCOUNT_TEST_MAX_WORKER_USLEEP * 10); // wait till workers stop attempting to acquire (minor chance they it'll take longer diw to context switch on loaded machine)
		if (scalabale_refcount_get_count(&test_param.refcount) != 0) {
			// for the few cases the minor sleep wasnt enough, give it a little more time before we complain.
			int		j;
			_NT(trace_4_kr_incs_unitest_test_scalabale_refcount, "minor wait for workers seems to be a little to short - let them have another chance");
			for (j=0; (j < 1000) && (scalabale_refcount_get_count(&test_param.refcount) != 0); j++) {
				sched_yield();
			}
			BUG_ON(scalabale_refcount_get_count(&test_param.refcount) != 0);
			_NT(trace_5_kr_incs_unitest_test_scalabale_refcount, "had to wait @N_ITERATIONS iterations for workers to let go", j);
		}

		atomic_set(&test_param.state, SCALABALE_REFCOUNT_TEST_STATE_TERM);
		for (i=0; i < SCALABALE_REFCOUNT_TEST_CPU_NUM; i++) {
			kthread_stop(tasks[i]);
		}
		scalabale_refcount_destroy(&test_param.refcount);
	}
}

//---------------------------------------------------------------------------------------------------------------------
/* test the implementation of the kernel timers.
 * test scenario:
 * create N timers separated by 10msec each. timer in index K is delayed for k*10 [msec]
 * randomly shuffle their creation & then verify they expire at the correct order. for simplicity, initiate timers from both ends towards the middle.
 * a second test hand-crafts a timer that hangs for long * as we wait for its deletion, the timer is allowed to return.
 * a third test stresses the races between timers that might expire with a thread that attempts to cancel them.
 */

// the number of timers we'll fire.
#define TIMERS_TEST_MAX_TIMERS	1002
#if (TIMERS_TEST_MAX_TIMERS % MAX_NUM_TIMERS_ENGINE == 0)
#  error "make sure they dont align too nicely when round-robin"
#endif

// initial delay of timers, so they dont contend with the thread that starts them
#define TIMERS_TEST_INITIAL_INTERVAL	msecs_to_jiffies(100)
// the time between expiration of consecutive timers
#define TIMERS_TEST_SEP_INTERVAL		msecs_to_jiffies(10/*msec*/)
// maximum tolerable delay of expiration callback vs. the desired time
#define TIMERS_TEST_MAX_DRIFT			msecs_to_jiffies(1000/*msec*/)

struct test_timer_param {
	// the timer we fire
	struct timer_list	timer;
	// before/after call to initiate the timer. expiration should be
	unsigned long 		pre_init, post_init;
	// the index of this timer within the vector
	int					index;
	union {	// per test-case param
		struct cancel_test_param {
			volatile bool is_canceled;		// after being canceled we mark it & the expiration callback verifies this is never set
		} cancel;
	} u;
};
struct kernel_sim_timers_test_param {
	struct test_timer_param		timer_param[TIMERS_TEST_MAX_TIMERS];
};
static atomic_t		kernel_sim_timers_test_completed;	// count the completed counters.

static void timers_test_accuracy_timer_cb(unsigned long	data)
{
	struct test_timer_param	*t = (struct test_timer_param	*)data;
	unsigned long	curr_jiffies = jiffies;
	unsigned long	exp_delay = TIMERS_TEST_INITIAL_INTERVAL + t->index * TIMERS_TEST_SEP_INTERVAL;
	unsigned long	max_valid = t->post_init + exp_delay + TIMERS_TEST_MAX_DRIFT;

	/* BEWARE: when this runs in SlickEdit, its much slower & drift is huge !!!!!! */
	_NT(trace_kr_incs_unitest_timers_test_accuracy_timer_cb, "timer index @INDEX expired: expected=@EXPECTED, now=@NOW, drift=@DRIFT", t->index, t->timer.expires, curr_jiffies, curr_jiffies - t->timer.expires);
#if 1
	BUG_ON((t->index < 0) || (t->index >= TIMERS_TEST_MAX_TIMERS));
	BUG_ON(curr_jiffies < t->pre_init + exp_delay); // not too soon
	BUG_ON(curr_jiffies > max_valid);				// not too late
#else
	// use this when we dont want to crash
	(void)max_valid;
	msleep(1);
#endif
	atomic_inc(&kernel_sim_timers_test_completed);
	BUG_ON(atomic_read(&kernel_sim_timers_test_completed) > TIMERS_TEST_MAX_TIMERS);
}

static void timers_test_accuracy_init_timer(struct test_timer_param	*t, int	index)
{
	t->index = index;
	__setup_timer(&t->timer, timers_test_accuracy_timer_cb, (unsigned long)t, 0);
	t->pre_init = jiffies;
	t->timer.expires = t->pre_init + TIMERS_TEST_INITIAL_INTERVAL + index * TIMERS_TEST_SEP_INTERVAL;
	_NT(trace_kr_incs_unitest_timers_test_accuracy_init_timer, "initiate timer @TIMER index @INDEX to expire at @EXPIRES", &t->timer, index, t->timer.expires);
	add_timer(&t->timer);
	t->post_init = jiffies;
	BUG_ON(t->pre_init > t->post_init);
}

static void test_timers_expiration_accuracy(struct kernel_sim *kernel)
{
	struct kernel_sim_timers_test_param 	test_param;
	struct test_timer_param	*tl, *th;
	int		i;

	atomic_set(&kernel_sim_timers_test_completed, 0);
	BUG_ON(TIMERS_TEST_MAX_TIMERS % 2);	// # of timers must be even, due to the loop that initiates them
	for (i = 0, tl=test_param.timer_param, th = &test_param.timer_param[TIMERS_TEST_MAX_TIMERS - 1];
		 i < TIMERS_TEST_MAX_TIMERS/2;
		 i++, tl++, th--) {
		BUG_ON(tl >= th); // make sure we dont use a timer with other iterator
		timers_test_accuracy_init_timer(tl, i);
		timers_test_accuracy_init_timer(th, TIMERS_TEST_MAX_TIMERS-i-1);
#if 0	// debug
		{
			char		buf[100*1024];
			kernel_timers_dump(&kernel_sim.timers, buf, sizeof(buf));
			puts(buf);
		}
#endif
	}
	printf("Done initiating all timers - waiting for their expiration\n");
	kernel_timers_drain(&kernel->timers, false);
	BUG_ON(atomic_read(&kernel_sim_timers_test_completed) != TIMERS_TEST_MAX_TIMERS);
	printf("Kernel Simulator Timers test completed\n");
}


//------------------------------- test of basic timer add & deletion while its expiration processing takes place--------------------------------

struct timers_test_cancel_paced {
	volatile bool		is_ret_from_timer_cb;
	volatile bool		is_canceled;
	volatile bool		is_cb_invoked;
	unsigned int		sleep_time;				// the time to let the timer cb hand until its allowed to return.
	struct timer_list	timer_hang;				// the timer we fire to execute for log
	struct timer_list	timer_let_go;			// the timer we fire to async instruct the first timer to return from executin (allow del_sync to complete)
};

// a timer callback that will hang for long to allow us to cancel it while its active
static void timers_test_cancel_timer_with_return_control_cb(unsigned long	data)
{
	struct timers_test_cancel_paced	*t = (struct timers_test_cancel_paced *)data;
	t->is_cb_invoked = true;
	BUG_ON(t->is_canceled);
	while (!t->is_ret_from_timer_cb) {
		_ND(trace_kr_incs_unitest_timers_test_cancel_timer_with_return_control_cb, "wait for permission to complete...");
		sched_yield();
	}
	_ND(trace_1_kr_incs_unitest_timers_test_cancel_timer_with_return_control_cb, "got permission to complete");
	BUG_ON(t->is_canceled);
}

// a timer callback that will hang for long to allow us to cancel it while its active
static void timers_test_cancel_timer_after_some_msec(unsigned long	data)
{
	struct timers_test_cancel_paced	*t = (struct timers_test_cancel_paced *)data;
	_ND(trace_kr_incs_unitest_timers_test_cancel_timer_after_some_msec, "sleep for @SLEEP_TIME msec", t->sleep_time);
	msleep(t->sleep_time);
	_ND(trace_1_kr_incs_unitest_timers_test_cancel_timer_after_some_msec, "let hanging timer return");
	t->is_ret_from_timer_cb = true;
}

static void test_timers_cancelation_basic(void /*struct kernel_sim *kernel*/)
{
#define TEST_TIMER_CANCEL_BASIC_REPEAT	100
	struct timers_test_cancel_paced		t;
	struct timer_list       timer;
	unsigned long	start, stop;
	int	i;

	for (i=0; i < TEST_TIMER_CANCEL_BASIC_REPEAT; i++) {
		// test the time it takes to cancel when canceling before expiration - should be immediate
		__setup_timer(&timer, timers_test_cancel_timer_after_some_msec/*wont be invoked*/, (unsigned long)&timer, 0);
		timer.expires = jiffies + msecs_to_jiffies(100*10000);
		add_timer(&timer);
		start = jiffies;
		del_timer_sync(&timer);
		stop = jiffies;
		//_ND_dmesg(t_simuav30, "del_timer_sync() took @LONG usec", jiffies_to_usecs(stop-start));
		BUG_ON(jiffies_to_usecs(stop-start) > 100);
		t.is_ret_from_timer_cb = false;
		t.is_canceled = false;
		t.is_cb_invoked = false;
		t.sleep_time = 10/*msec*/;
		__setup_timer(&t.timer_hang, timers_test_cancel_timer_with_return_control_cb, (unsigned long)&t, 0);
		t.timer_hang.expires = jiffies;
		add_timer(&t.timer_hang);
		// wait for the timer to be invoked.
		while (!t.is_cb_invoked) {
			sched_yield();
		}
		// now fire another timer to instruct the first to return after some time.
		__setup_timer(&t.timer_let_go, timers_test_cancel_timer_after_some_msec, (unsigned long)&t, 0);
		t.timer_let_go.expires = jiffies + jiffies_to_msecs(10);
		add_timer(&t.timer_let_go);
		// now see that we are delayed until timer completes execution.
		del_timer_sync(&t.timer_hang);
		t.is_canceled = true;
		BUG_ON(t.is_cb_invoked == false);
		del_timer_sync(&t.timer_let_go);
		BUG_ON(get_kernel_num_active_timers() != 0);
	}
}


//------------------------------- test of race between timer canceling & expiration processing --------------------------------
// NOTE: Since this test attempts to create races between threads, it might fail with BUG_ON() on sanity checks. if it only happens occasionaly then its still OK as long as most runs yield the desired test.

#define TIMERS_TEST_CACNEL__MAX_PER_BATCH		200				// many timers that expire at the same time on multiple (4 as of now) timer threads increase probability of having some being canceld & do the expiration processing
#if TIMERS_TEST_CACNEL__MAX_PER_BATCH > TIMERS_TEST_MAX_TIMERS
#  error "cancel test requires too many timers"
#endif
#define TIMERS_TEST_CACNEL__INTERVAL_TO_EXPIRE	 40/*msec*/		// enough time to fire many timers & than wait for that time to arrive & race with the timers expiration.
// a callback of timers that we cancel, to count whether we managed to cancel & at what phase.
static void timers_test_cancel_timer_cb(unsigned long	data)
{
	struct test_timer_param	*t = (struct test_timer_param *)data;
	int n_completed;

	BUG_ON((t->index < 0) || (t->index >= TIMERS_TEST_CACNEL__MAX_PER_BATCH));
	atomic_inc(&kernel_sim_timers_test_completed);
	sched_yield();	// delay execution a little to give chance to canceling thread
	n_completed = atomic_read(&kernel_sim_timers_test_completed);
	// if so many timers expire, wait a little to allow the canceling thread to achieve something
	if (n_completed > TIMERS_TEST_CACNEL__MAX_PER_BATCH / 2) {
		usleep(5);
	} else if (n_completed > TIMERS_TEST_CACNEL__MAX_PER_BATCH / 4) {
		usleep(2);
	} else if (n_completed > TIMERS_TEST_CACNEL__MAX_PER_BATCH / 8) {
		usleep(1);
	}

	BUG_ON(t->u.cancel.is_canceled == true);
}

static void timers_test_cancel_init_timer(struct test_timer_param	*t, int	index, const unsigned long expiration)
{
	t->index = index;
	__setup_timer(&t->timer, timers_test_cancel_timer_cb, (unsigned long)t, 0);
	t->pre_init = jiffies;
	t->timer.expires = expiration;
	_NT(trace_kr_incs_unitest_timers_test_cancel_init_timer, "initiate timer @TIMER index @INDEX to expire at @EXPIRES", &t->timer, index, t->timer.expires);
	add_timer(&t->timer);
	t->post_init = jiffies;
	BUG_ON(t->pre_init > t->post_init);
}

/* test the race between cancel & expiration execution.
 * create a large (100) batch pf timers that all expire at the same time. wait for that time & then cancel all of them.
 * This is a race between cancel & expiration execution.
 * repeat the above many times
 * Note: since its a design for race, different system might execute differently & fail bcz ALL timers would be on one side (either expired or canceled).
 *       so it might require some tuning with minor delays in either of the canceling thread or the timer callback.
 */
static void test_timers_cancelation(void /*struct kernel_sim *kernel*/)
{
	const int num_batches = 100;	// repeat many times to ensure high probability of covering races.
	struct kernel_sim_timers_test_param 	test_param;
	struct test_timer_param	*t;
	unsigned long expiration;
	int		batch_ind, timer_ind;
	int		n_waited, n_canceled, n_expired;	// count number of wait/canceled/expired timers.
	int		rv;

	n_waited = 0;		// since we expect just a few of these, we count them for the whole test rather than per batch
	for (batch_ind=0; batch_ind < num_batches; batch_ind++) {
		n_canceled = n_expired = 0;
		atomic_set(&kernel_sim_timers_test_completed, 0);
		_NT(trace_kr_incs_unitest_test_timers_cancelation, "------------- batch_ind=@BATCH_IND begins -------------", batch_ind);
		expiration = jiffies + msecs_to_jiffies(TIMERS_TEST_CACNEL__INTERVAL_TO_EXPIRE);	// all timers are set for the exact same time
		for (timer_ind=0, t=test_param.timer_param; timer_ind < TIMERS_TEST_CACNEL__MAX_PER_BATCH; timer_ind++, t++) {
			t->u.cancel.is_canceled = false;
			timers_test_cancel_init_timer(t, timer_ind, expiration);
		}
		_NT(trace_1_kr_incs_unitest_test_timers_cancelation, "------------- batch_ind=@BATCH_IND ends (jiffies=@JIFFIES, expiration=@EXPIRATION)-------------", batch_ind, jiffies, expiration);
		BUG_ON(jiffies_to_msecs(jiffies - expiration) < msecs_to_jiffies(1/*msec*/));	// ensure we added all timers & have some idle time before they start to expire.
		// wait for timers expiration time, so we'll cancel them as they expire
		do {
			sched_yield();
		} while (jiffies < expiration);
		// wait until expiration processing really starts (by at least one timer thread)
		while (atomic_read(&kernel_sim_timers_test_completed) == 0) {
			sched_yield();
		}
		// cancel times & ensure that some are canceled & some dont.
		for (timer_ind=0, t=test_param.timer_param; timer_ind < TIMERS_TEST_CACNEL__MAX_PER_BATCH; timer_ind++, t++) {
			rv = del_timer_sync(&t->timer);
			t->u.cancel.is_canceled = true;
			switch (rv) {
			case 0: n_expired  ++;	break;	// timer has already expired
			case 1: n_canceled ++;	break;	// timer was canceled before being executed
			case 2: n_waited   ++;	break;	// timer was executing while canceled & we had to wait for its execution to complete.
			default:
				BUG();
			}
		}
		pr_crit("n_waited=%d, n_canceled=%d, n_expired=%d\n", n_waited, n_canceled, n_expired);
		BUG_ON((n_expired == 0) || (n_canceled == 0));		// a proper run must yield both canceled & expired timers.
		BUG_ON(atomic_read(&kernel_sim_timers_test_completed) == TIMERS_TEST_CACNEL__MAX_PER_BATCH);
		// verify we drained it all
		BUG_ON(get_kernel_num_active_timers() != 0);
	}
	BUG_ON(n_waited == 0);	// we have to catch some of these bcz that's what we wanted to test here
}

static void test_timers(struct kernel_sim *kernel)
{
	test_timers_expiration_accuracy(kernel);
	test_timers_cancelation_basic(/*kernel*/);
	test_timers_cancelation(/*kernel*/);
}

//---------------------------------------------------------------------------------------------------------------------
/* test dispatch & cancel of a delayed work item
 * first case is a simple sanity of dispatch & cancel with a timer that is sure to NOT expire.
 * second is more complicated & assumes the separate test for canceling a timer is working. we dispatch 2 delayed work items.
 *  	both have a very short timeout so we cancel them when they are in the workqueue.
 *  	first will sleep for long & so the second is queued behind.
 *  	we cancel the second which finds it in the queue *before* execution. so we verify it is NOT invoking its callback.
 *  	we then cancel the first but is executing. so we verify its callback was invoked & that the cancel call wait for the execution to complete.
 */
struct test_delayed_work_param {
	struct delayed_work 	dwork;				// the delayed work item that we cancel
	struct delayed_work 	dwork_long_time;	// the delayed work item that will run for long
	volatile bool	is_cb_invoked;
	volatile bool	is_long_invoked;
	volatile bool	is_start_sleep;
	volatile bool	is_long_returned;
};

static void test_delayed_work_cancel_cb(struct work_struct *work)
{
	struct test_delayed_work_param	*p = container_of(work, struct test_delayed_work_param, dwork.work);
	p->is_cb_invoked = true;
	_ND(trace_kr_incs_unitest_test_delayed_work_cancel_cb, "Invoked");
}

static void test_delayed_work__long_work_cb(struct work_struct *work)
{
	struct test_delayed_work_param	*p = container_of(work, struct test_delayed_work_param, dwork_long_time.work);
	_ND(trace_kr_incs_unitest_test_delayed_work__long_work_cb, "Invoked - wait for permission to start");
	p->is_long_invoked = true;
	while (!p->is_start_sleep) usleep(10);	// wait for signal. bcz the dispatch of 2 timers sometime causes the first to complete before the second is invoked.
	_ND(trace_1_kr_incs_unitest_test_delayed_work__long_work_cb, "sleeping...");
	msleep(10);								// delay to ensure the second delayed work item is not executing
	p->is_long_returned = true;
	_ND(trace_2_kr_incs_unitest_test_delayed_work__long_work_cb, "Returning");
}
static void test_workqueue(void /*struct kernel_sim *kernel*/)
{
	const int 	max_loops = 200;
	struct test_delayed_work_param 	test_param;
	int		repeat;
	for (repeat = 0; repeat < max_loops; repeat ++) {
		const int wq_canceled = system_wq->num_canceled;
		// dispatch a dwork far into future & cancel it, before timer expires.
		_ND(trace_kr_incs_unitest_test_workqueue, "Case 1:");
		test_param.is_cb_invoked = false;
		INIT_DELAYED_WORK(&test_param.dwork, test_delayed_work_cancel_cb);
		BUG_ON(!schedule_delayed_work(&test_param.dwork, jiffies + msecs_to_jiffies(100000/*large so timer wont expire*/)));
		BUG_ON(!cancel_delayed_work_sync(&test_param.dwork));	// dwork was pending.
		BUG_ON(test_param.is_cb_invoked);						// canceled before expired

		// dispatch 2 dwork objects. first will hold the thread & so prevent the second from executing, this will allow the cancel to find the second in the queue.
		_ND(trace_1_kr_incs_unitest_test_workqueue, "Case 2:");
		test_param.is_long_invoked = false;
		test_param.is_long_returned = false;
		test_param.is_start_sleep = false;
		BUG_ON(system_wq->num_canceled != wq_canceled);
		INIT_DELAYED_WORK(&test_param.dwork_long_time, test_delayed_work__long_work_cb);
		INIT_DELAYED_WORK(&test_param.dwork, 		   test_delayed_work_cancel_cb);
		BUG_ON(!schedule_delayed_work(&test_param.dwork_long_time, 0/*let this one start now*/));
		while (system_wq->num_pending_works != 1)						// wait for first timer to expire & place the first work on the work-queue
			rmb();
		BUG_ON(!schedule_delayed_work(&test_param.dwork, 		   0/*let it run immediately after bcz we want to find it in the workqueue*/));
		while (test_param.is_long_invoked != true) {					// wait for second timers to expire, place its work on the work-queue & for it to execute
			usleep(1);
		}
		test_param.is_start_sleep = true;
		_ND(trace_2_kr_incs_unitest_test_workqueue, "Cancel queued dwork");
		BUG_ON(!cancel_delayed_work_sync(&test_param.dwork));           // cancel the queued one that hasnt started to execute yet.
		_ND(trace_3_kr_incs_unitest_test_workqueue, "Queued dwork canceled");
		BUG_ON(system_wq->num_pending_works != 1);
		BUG_ON(test_param.is_cb_invoked);								// canceled before expired
		BUG_ON(system_wq->num_canceled != wq_canceled + 1);				// it was canceled
		_ND(trace_4_kr_incs_unitest_test_workqueue, "Cancel Executing dwork");
		BUG_ON(cancel_delayed_work_sync(&test_param.dwork_long_time));	// cancel the executing work & make sure we block, waiting for it to complete.
		_ND(trace_5_kr_incs_unitest_test_workqueue, "Executing dwork completed");
		BUG_ON(test_param.is_long_returned == false);
		BUG_ON(system_wq->num_canceled != wq_canceled + 1);				// it was NOT canceled
		BUG_ON(system_wq->num_pending_works != 0);						// all should be done
		BUG_ON(test_param.is_cb_invoked);								// canceled before expired
	}
}

//---------------------------------------------------------------------------------------------------------------------

void test_kernel_infra(struct kernel_sim *kernel)
{
	pr_crit("Start Simulator infra tests ...\n");
	test_kthread_signaling();
	test_kthread_wait_and_kill();
	//test_percpu();
	test_scalabale_refcount();
	test_timers(kernel);
	test_workqueue();
	pr_crit("Done Simulator infra tests ...\n");
}

/*****************************************************************************/
// EOF.

