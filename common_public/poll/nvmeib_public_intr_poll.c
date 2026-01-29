/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_public_intr_poll.h"
#include "nvmeib_public.h"
#define nvmeib_debug_level nvmeib_public_debug_level
#include "nvmeib_utils.h"
#include "../nvmeibp_trace.h"

#define IPOLL_T_NAME "ipoll"

#define IPOLLER_SCHED_FROM_ANY_CPU 1

struct intr_poller {
	/* thread name */
	char name[16];
	/* the thread */
	struct task_struct *t;
	/* thread event list */
	struct list_head ctxs;
	/* thread wait for ctxs here */
	wait_queue_head_t wqh;
	/* true when we are done */
	volatile bool done;
	/* true when has new work */
	volatile int fetch;
#if defined (IPOLLER_SCHED_FROM_ANY_CPU) && (IPOLLER_SCHED_FROM_ANY_CPU==1)
	/* for sched from other cpu */
	spinlock_t spinlock;
#endif
};

static DEFINE_PER_CPU(struct intr_poller, intr_pollers);

unsigned nvmeib_public_ipoller_poll_duration_jif = 0;
module_param_named(ipoller_poll_duration_jif, nvmeib_public_ipoller_poll_duration_jif, uint, 0644);
MODULE_PARM_DESC(ipoller_poll_duration_jif, "ipoller poll duration till reschedule, 0=default");

#if defined (IPOLLER_SCHED_FROM_ANY_CPU) && (IPOLLER_SCHED_FROM_ANY_CPU==1)
#define ipoller_irq_disable(__p, flags)	spin_lock_irqsave(&(__p)->spinlock, flags);
#define ipoller_irq_enable(__p, flags)	spin_unlock_irqrestore(&(__p)->spinlock, flags);
#define ipoller_irq_save(__p, flags)   	spin_lock_irqsave(&(__p)->spinlock, flags);
#define ipoller_irq_restore(__p, flags) spin_unlock_irqrestore(&(__p)->spinlock, flags);

#define nviop_is_disabled(__nviop) 		atomic_read(&(__nviop)->disabled)
#define nviop_set_disabled(__nviop) 		atomic_set(&(__nviop)->disabled, 1)
#define nviop_clear_disabled(__nviop) 		atomic_set(&(__nviop)->disabled, 0)
#define nviop_test_and_set_sched(__nviop) 	atomic_xchg(&(__nviop)->sched, 1)
#define nviop_is_sched(__nviop) 		atomic_read(&(__nviop)->sched)
#define nviop_clear_sched(__nviop) 		atomic_set(&(__nviop)->sched, 0)

#define nviop_sched_list(__nviop) 		((__nviop)->iop.list)
#define nviop_in_sched_list(__nviop) 		(!list_empty(&(__nviop)->iop.list))

#else
#define ipoller_irq_disable(__p, flags)	local_irq_disable()
#define ipoller_irq_enable(__p, flags)	local_irq_enable()
#define ipoller_irq_save(__p, flags)	local_irq_save(flags)
#define ipoller_irq_restore(__p, flags)	local_irq_restore(flags)

#define nviop_is_disabled(__nviop) 		test_bit(IRQ_POLL_F_DISABLE, &(__nviop)->iop.state)
#define nviop_set_disabled(__nviop) 		set_bit(IRQ_POLL_F_DISABLE, &(__nviop)->iop.state)
#define nviop_clear_disabled(__nviop) 		clear_bit(IRQ_POLL_F_DISABLE, &(__nviop)->iop.state)
#define nviop_test_and_set_sched(__nviop) 	test_and_set_bit(IRQ_POLL_F_SCHED, &(__nviop)->iop.state)
#define nviop_is_sched(__nviop) 		test_bit(IRQ_POLL_F_SCHED, &(__nviop)->iop.state)
#define nviop_clear_sched(__nviop) 		clear_bit_unlock(IRQ_POLL_F_SCHED, &(__nviop)->iop.state)

#define nviop_sched_list(__nviop) 		((__nviop)->iop.list)
#define nviop_in_sched_list(__nviop) 		(!list_empty(&(__nviop)->iop.list))

#endif

static void intr_poll_complete_locked(struct nvmeib_irq_poll *iop);

static int ipoller_run(void *arg)
{
	struct intr_poller *p = arg;
	struct list_head *l = &p->ctxs;
	struct nvmeib_irq_poll *iop;
	int work;
	int weight;
	unsigned long start_time;
	unsigned long flags;
	bool all_poll_linger;

	NFIN;
	BUG_ON(p != this_cpu_ptr(&intr_pollers));
	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		preempt_disable();
		if (kthread_should_stop() || p->done) {
			set_current_state(TASK_RUNNING);
			preempt_enable();
			goto out;
		}
		if (!p->fetch) {
#ifdef preempt_enable_no_resched
			preempt_enable_no_resched();
#else
			preempt_enable();
#endif
			schedule();
		} else {
			set_current_state(TASK_RUNNING);
			preempt_enable();
			start_time = jiffies;
			ipoller_irq_disable(p, flags);
			while (!list_empty(l)) {
				if (time_after(jiffies, start_time + nvmeib_public_ipoller_poll_duration_jif))
					goto let_others;
				ipoller_irq_enable(p, flags);
				/* interrupts have been re-enabled but we are safe
				 because interrupts can only add new
				 entries to the tail of this list, and only ->poll()
				 calls can remove this head entry from the list.
				 */
				iop = list_entry(l->next, struct nvmeib_irq_poll, iop.list);
				weight = iop->iop.weight;

				/* Check disabled flag first to speed up any context waiting for disable to complete */
				if (!nviop_is_disabled(iop)) {
					/* ib_poll_handler */
					work = iop->iop.poll(&iop->iop, weight);
				} else {
					/* Removes iop from the list (takes lock) */
					nvmeib_public_intr_poll_complete(iop);
					work = 0;
				}
				ipoller_irq_disable(p, flags);

				if (weight != iop->iop.weight) {
					/* ib_poll_handler has modified weight (nvmeib_pcpu_cq_poll_budget changed)
					 * log the change */ 
					_NI_dmesg(trace_ipoller_run, "iop=@PTR weight: @INT -> @INT",
							  iop, weight, iop->iop.weight);
					weight = iop->iop.weight;
				}

				/* Question: Should this not be the old weight? 
				 * Otherwise if weight is increased, it could look like we emptied the CQ, when we actually did not */
				if (work >= weight || iop->poll_linger) {
					if (nviop_is_disabled(iop)) {
						/* Disabled flag was set while we were polling, remove from list (Remember we already hold lock) */
						intr_poll_complete_locked(iop);
					} else
						list_move_tail(&nviop_sched_list(iop), l);
				}
				/* else:
				   In case iop->poll (ib_poll_handler) polled less than
				   iop->weight, it removes iop from this list and rearms
				   interrupts with MISSED-EVENTS. If did missed events,
				   it adds the iop back to list and wake-up this thread.

				   NOTE: We do need to check disabled flag in this case as ib_poll_handler will have either:
					- Called nvmeib_public_intr_poll_complete (so redundant)
					- Called nvmeib_public_intr_poll_sched (which checks the disabled flag itself)
				*/

				/* Check if all entries in l have poll_linger set - means we need to poll but may let other tasks to run*/
				if (!list_empty(l)) {
					all_poll_linger = true;
					list_for_each_entry(iop, l, iop.list) {
						if (!iop->poll_linger) {
							all_poll_linger = false;
							break;
						}
					}
					if (all_poll_linger) {
						goto let_others;
					}
				}
			}
			p->fetch = 0;
let_others:
			ipoller_irq_enable(p, flags);
			cond_resched();
		}
	}

out:
	NFOUT;
	return 0;
}

static struct task_struct * create_thread(struct intr_poller *p, int cpu)
{
	struct task_struct *t;

	NFIN;
	t = kthread_create_on_node(
			ipoller_run, p,  cpu_to_node(cpu), "%s", p->name);
	if (!IS_ERR(t))
		kthread_bind(t, cpu);
	else
		t = NULL;
	NFOUT;
	return t;
}

static int init_intr_poller(struct intr_poller *p, int cpu)
{
	int rv;
	int node_id;

	NFIN;
	node_id = cpu_to_node(cpu);
	p->done = false;
	p->fetch = false;
	snprintf(p->name, sizeof(p->name), IPOLL_T_NAME "/%d/%d", node_id, cpu);
	if ((p->t = create_thread(p, cpu))) {
		_NT(init_intr_poller_t1,
			"created POLLER thread @STR (pid=@INT)\n", p->name, p->t->pid);
		get_task_struct(p->t);
		INIT_LIST_HEAD(&p->ctxs);
		init_waitqueue_head(&p->wqh);
		spin_lock_init(&p->spinlock);
		wake_up_process(p->t);
		rv = 0;
	}
	else
		rv = -1;
	NFOUT;
	return rv;
}

int nvmeib_public_intr_pollers_start(void)
{
	int i;
	int rv = 0;

	NFIN;
	for_each_online_cpu(i) {
		if ((rv = init_intr_poller(&per_cpu(intr_pollers, i), i)))
			break;
	}
	if (rv)
		nvmeib_public_intr_pollers_stop();
	NFOUT;
	return rv;
}

void nvmeib_public_intr_pollers_stop(void)
{
	int i;
	struct intr_poller *p;

	NFIN;
	for_each_online_cpu(i) {
		p = &per_cpu(intr_pollers, i);
		if (p->t) {
			p->done = true;
			wake_up(&p->wqh);
			kthread_stop(p->t);
			put_task_struct(p->t);
			p->t = NULL;
			p->done = false;
		}
	}
	NFOUT;
}

void nvmeib_public_intr_poll_init(
	struct nvmeib_irq_poll *iop, int weight, irq_poll_fn *poll_fn)
{
	NFIN;
	memset(iop, 0, sizeof(*iop));
	INIT_LIST_HEAD(&iop->iop.list);
	iop->iop.weight = weight;
	iop->iop.poll = poll_fn;
	atomic_set(&iop->disabled, 0);
	atomic_set(&iop->sched, 0);
	iop->last_nonempty_comp_jif = 0;
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_intr_poll_init);

//omril: must be add-tail!!!
void nvmeib_public_intr_poll_sched(struct nvmeib_irq_poll *iop, int cpu)
{
	struct intr_poller *p = per_cpu_ptr(&intr_pollers, cpu);
	unsigned long flags;

	NFIN;

#if !defined (IPOLLER_SCHED_FROM_ANY_CPU) || (IPOLLER_SCHED_FROM_ANY_CPU!=1)
	BUG_ON(cpu != smp_processor_id());
#endif
	
	/* Check pollee has not been disabled by another context */
	if (nviop_is_disabled(iop))
		return;
	/* Checks that this is the only context about to schedule the pollee */
	if (nviop_test_and_set_sched(iop))
		return;

	ipoller_irq_save(p, flags);
	//p = this_cpu_ptr(&intr_pollers);
	if (nviop_in_sched_list(iop)) {
		_NE_dmesg(nvmeib_public_intr_poll_sched_e1, "common public module crashing the system to prevent data corruption. Error code: 1063. iop=@PTR already sched\n", iop);
		BUG();
	}
	list_add_tail(&nviop_sched_list(iop), &p->ctxs);
	++p->fetch;
	//wake_up(&p->wqh);
	wake_up_process(p->t);
	ipoller_irq_restore(p, flags);

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_intr_poll_sched);

static void intr_poll_complete_locked(struct nvmeib_irq_poll *iop)
{
	if (!nviop_in_sched_list(iop)) {
		_NE(nvmeib_public_intr_poll_complete_e1,
		    "OOPS, iop=@PTR NOT sched\n", iop);
		BUG();
	}
	list_del_init(&nviop_sched_list(iop));
	smp_mb__before_atomic();
	/* Let other contexts know the pollee is no longer scheduled and is free to be rescheduled (or that disabled is complete) */
	nviop_clear_sched(iop);
}

void nvmeib_public_intr_poll_complete(struct nvmeib_irq_poll *iop)
{
	struct intr_poller *p = this_cpu_ptr(&intr_pollers);
	unsigned long flags;

	NFIN;
	ipoller_irq_save(p, flags);
	intr_poll_complete_locked(iop);
	ipoller_irq_restore(p, flags);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_intr_poll_complete);

bool nvmeib_public_intr_poll_is_sched(struct nvmeib_irq_poll *iop)
{
	return nviop_is_sched(iop);
}
EXPORT_SYMBOL(nvmeib_public_intr_poll_is_sched);

/* Barrier to ensure pollee is not queued, running or about to be queued on any poller context.
 * Based on logic of irq_poll_disable. Works as follows:
 * - Sets the disabled atomic to prevent any current context from deciding to schedule
 * - Waits until this context sets sched from 0 -> 1 
 * 	(ie any current poller has returned sched to 0 and no other context has set it back to 1 before adding to another poller list)
 * - Clears sched back to 0 (but leaves disabled set to 1)
 */
void nvmeib_public_intr_poll_disable(struct nvmeib_irq_poll *iop)
{
	nviop_set_disabled(iop);
	while (nviop_test_and_set_sched(iop))
		msleep(1);
	nviop_clear_sched(iop);
}
EXPORT_SYMBOL(nvmeib_public_intr_poll_disable);

/* Clears the disabled atomic, so is free to be scheduled by other context
 * Based on logic of irq_poll_enable */
void nvmeib_public_intr_poll_enable(struct nvmeib_irq_poll *iop)
{
	if (!nviop_is_disabled(iop)) {
		_NE_dmesg(err_intr_poll_enable, "iop @PTR, enable without disable", iop);
		BUG();
	}
	smp_mb__before_atomic();
	nviop_clear_disabled(iop);
}
EXPORT_SYMBOL(nvmeib_public_intr_poll_enable);
