/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * recv_comp_work_ctr_test.c - Test module for recv_comp_work_ctr vs cancel_work_sync
 *
 * Tests whether recv_comp_work_ctr in s_nordda.c is needed or if just
 * cancel_work_sync suffices. Simulates the nordda pattern:
 * - Interrupt (hr-timer) schedules work on a WQ
 * - Work checks global atomic "run_allowed"; prints error if it is 0
 * - Main thread races shutdown (run_allowed=0) against work execution
 *
 * If cancel_work_sync alone is sufficient, work should never see run_allowed=0.
 * If recv_comp_work_ctr is needed, we may observe work running during shutdown.
 *
 * Use use_ctr=1 to test WITH the recv_comp_work_ctr pattern (like nordda).
 * Use use_ctr=0 to test with cancel_work_sync only.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/smp.h>
#include <linux/cpumask.h>

/* Default ~100us simulates RDMA completion interrupt rate under high load (~10k/sec) */
#define TIMER_FIRE_US_DEFAULT  100
#define WAIT_AFTER_DEC_MS 20

static struct workqueue_struct *test_wq;
static struct work_struct test_work;
static struct hrtimer test_timer;
static atomic_t run_allowed;
static atomic_t hazard_count;

/* Simulates recv_comp_work_ctr: when 0, "interrupt" must not queue work */
static atomic_t work_ctr;

static int use_ctr = 0;
module_param(use_ctr, int, 0444);
MODULE_PARM_DESC(use_ctr, "1=use recv_comp_work_ctr pattern, 0=cancel_work_sync only");

static uint iterations = 50000;
module_param(iterations, uint, 0644);
MODULE_PARM_DESC(iterations, "Number of test loop iterations");

/* Timer fire interval in microseconds (RDMA high-load: ~50-200us) */
static uint timer_fire_us = TIMER_FIRE_US_DEFAULT;
module_param(timer_fire_us, uint, 0644);
MODULE_PARM_DESC(timer_fire_us, "Timer fire interval in us (RDMA high-load: 50-200)");

/* CPU for timer (interrupt sim); -1 = same as main. Use different CPU for cross-CPU stress. */
static int timer_cpu = 1;
module_param(timer_cpu, int, 0644);
MODULE_PARM_DESC(timer_cpu, "CPU to run timer on (-1=same as main, >=0 explicit)");

/* CPU for main thread (shutdown loop) */
static int main_cpu = 0;
module_param(main_cpu, int, 0644);
MODULE_PARM_DESC(main_cpu, "CPU to pin main thread to");

struct timer_start_ctx {
	struct hrtimer *timer;
	ktime_t kt;
};

static void start_timer_on_cpu(void *info)
{
	struct timer_start_ctx *ctx = info;
	hrtimer_start(ctx->timer, ctx->kt, HRTIMER_MODE_REL);
}

static void test_work_fn(struct work_struct *work)
{
	if (atomic_read(&run_allowed) == 0) {
		atomic_inc(&hazard_count);
		pr_err("recv_comp_work_ctr_test: HAZARD - work ran with run_allowed=0\n");
	}
	if (use_ctr)
		atomic_dec(&work_ctr);
}

static enum hrtimer_restart test_timer_fn(struct hrtimer *timer)
{
	/* Simulate interrupt: schedule work on WQ */
	if (use_ctr) {
		if (atomic_inc_not_zero(&work_ctr)) {
			if (!queue_work(test_wq, &test_work))
				atomic_dec(&work_ctr);
		}
	} else {
		queue_work(test_wq, &test_work);
	}
	return HRTIMER_NORESTART;
}

static int __init recv_comp_work_ctr_test_init(void)
{
	int i;
	ktime_t kt;
	bool cancelled;
	int ctr_val = 0;
	int tcpu = timer_cpu;
	bool cross_cpu = false;

	if (iterations == 0) {
		pr_err("recv_comp_work_ctr_test: iterations must be > 0\n");
		return -EINVAL;
	}

	if (main_cpu >= 0 && main_cpu < nr_cpu_ids)
		set_cpus_allowed_ptr(current, cpumask_of(main_cpu));

	{
		int cur = get_cpu();
		if (tcpu >= 0 && tcpu < nr_cpu_ids && tcpu != cur)
			cross_cpu = true;
		else if (tcpu < 0 || tcpu >= nr_cpu_ids)
			tcpu = -1;
		put_cpu();
	}

	pr_info("recv_comp_work_ctr_test: Module loading, %u iterations (use_ctr=%d, timer_fire_us=%u, cross_cpu=%d)\n",
		iterations, use_ctr, timer_fire_us, cross_cpu);

	test_wq = alloc_workqueue("recv_comp_wctr_test_wq",
				 WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!test_wq) {
		pr_err("recv_comp_work_ctr_test: Failed to create workqueue\n");
		return -ENOMEM;
	}

	atomic_set(&run_allowed, 1);
	atomic_set(&hazard_count, 0);
	atomic_set(&work_ctr, 1);
	hrtimer_init(&test_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	test_timer.function = test_timer_fn;

	for (i = 0; i < iterations; i++) {
		/* 1) Set run_allowed to 1 */
		atomic_set(&run_allowed, 1);
		if (use_ctr)
			atomic_set(&work_ctr, 1);

		/* 2) Init and start timer (simulates interrupt), optionally on different CPU */
		INIT_WORK(&test_work, test_work_fn);
		kt = ns_to_ktime((u64)timer_fire_us * 1000);
		if (cross_cpu) {
			struct timer_start_ctx ctx = { &test_timer, kt };
			smp_call_function_single(tcpu, start_timer_on_cpu, &ctx, 1);
		} else {
			hrtimer_start(&test_timer, kt, HRTIMER_MODE_REL);
		}

		/* 3) Wait for timer to fire and possibly queue work */
		usleep_range(timer_fire_us + 50, timer_fire_us + 200);

		/* 4) Decrement counter (run_allowed -> 0) */
		atomic_dec(&run_allowed);

		if (use_ctr) {
			/* nordda pattern: dec ctr first to block new scheduling */
			ctr_val = atomic_dec_return(&work_ctr);
		}

		/* 5) Cancel work sync */
		cancelled = cancel_work_sync(&test_work);

		if (use_ctr) {
			if (cancelled)
				ctr_val = atomic_dec_return(&work_ctr);
			else
				ctr_val = atomic_read(&work_ctr);
			if (ctr_val > 0)
				pr_err("recv_comp_work_ctr_test: ctr_val=%d after cancel\n", ctr_val);
		}

		/* 6) run_allowed already 0 */
		/* 7) Wait a bit to see if work finds a hazard */
		msleep(WAIT_AFTER_DEC_MS);

		/* 8) Set run_allowed to 1 */
		atomic_set(&run_allowed, 1);

		/* 9) Re-init work for next iteration (enables work) */
		INIT_WORK(&test_work, test_work_fn);

		if (i > 0 && (i % 100) == 0)
			pr_info("recv_comp_work_ctr_test: iteration %d (cancelled=%d)\n",
				i, cancelled);
	}

	pr_info("recv_comp_work_ctr_test: Completed %u iterations, hazard_count=%d\n",
		iterations, atomic_read(&hazard_count));

	if (atomic_read(&hazard_count) > 0)
		pr_warn("recv_comp_work_ctr_test: HAZARDS DETECTED - recv_comp_work_ctr may be needed\n");
	else
		pr_info("recv_comp_work_ctr_test: No hazards - cancel_work_sync alone may suffice\n");

	destroy_workqueue(test_wq);
	return 0;
}

static void __exit recv_comp_work_ctr_test_exit(void)
{
	pr_info("recv_comp_work_ctr_test: Module unloaded\n");
}

module_init(recv_comp_work_ctr_test_init);
module_exit(recv_comp_work_ctr_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NVMesh");
MODULE_DESCRIPTION("Test recv_comp_work_ctr vs cancel_work_sync for nordda shutdown");
