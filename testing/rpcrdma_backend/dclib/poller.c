#include "poller.h"
#include "xtrace.h"

#define IPOLL_T_NAME "ipoll"

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
};

static DEFINE_PER_CPU(struct intr_poller, intr_pollers);

static int ipoller_run(void *arg)
{
	struct intr_poller *p = arg;
	struct list_head *l = &p->ctxs;
	struct irq_poll *iop;
	int work;
	int weight;
	unsigned long start_time;

	FIN;
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
			local_irq_disable();
			while (!list_empty(l)) {
				if (time_after(jiffies, start_time))
					goto let_others;
				local_irq_enable();
				/* interrupts have been re-enabled but we are safe
				 because interrupts can only add new
				 entries to the tail of this list, and only ->poll()
				 calls can remove this head entry from the list.
				 */
				iop = list_entry(l->next, struct irq_poll, list);
				weight = iop->weight;
				work = iop->poll(iop, weight);
				local_irq_disable();
				if (work >= weight)
					list_move_tail(&iop->list, l);
				/* else:
				   In case iop->poll (ib_poll_handler) polled less than
				   iop->weight, it removes iop from this list and rearms
				   interrupts with MISSED-EVENTS. If did missed events,
				   it adds the iop back to list and wake-up this thread.
				*/
				
			}
			p->fetch = 0;

let_others:
			local_irq_enable();
			cond_resched();
		}
	}

out:
	FOUT;
	return 0;
}

static struct task_struct * create_thread(struct intr_poller *p, int cpu)
{
	struct task_struct *t;

	FIN;
	t = kthread_create_on_node(
			ipoller_run, p,  cpu_to_node(cpu), "%s", p->name);
	if (!IS_ERR(t))
		kthread_bind(t, cpu);
	else
		t = NULL;
	FOUT;
	return t;
}

static int init_intr_poller(struct intr_poller *p, int cpu)
{
	int rv;
	int node_id;

	FIN;
	node_id = cpu_to_node(cpu);
	p->done = false;
	p->fetch = false;
	snprintf(p->name, sizeof(p->name), IPOLL_T_NAME "/%d/%d", node_id, cpu);
	if ((p->t = create_thread(p, cpu))) {
		xttrace("created POLLER thread %s (pid=%d)\n", p->name, p->t->pid);
		get_task_struct(p->t);
		INIT_LIST_HEAD(&p->ctxs);
		init_waitqueue_head(&p->wqh);
		wake_up_process(p->t);
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

int intr_pollers_start(void)
{
	int i;
	int rv = 0;

	FIN;
	for_each_online_cpu(i) {
		if ((rv = init_intr_poller(&per_cpu(intr_pollers, i), i)))
			break;
	}
	if (rv)
		intr_pollers_stop();
	FOUT;
	return rv;
}

void intr_pollers_stop(void)
{
	int i;
	struct intr_poller *p;

	FIN;
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
	FOUT;
}

void intr_poll_init(struct irq_poll *iop, int weight, irq_poll_fn *poll_fn)
{
	FIN;
	memset(iop, 0, sizeof(*iop));
	INIT_LIST_HEAD(&iop->list);
	iop->weight = weight;
	iop->poll = poll_fn;
	FOUT;
}

#define iop_is_sched(__iop) (!list_empty(&__iop->list))

void intr_poll_sched(struct irq_poll *iop)
{
	struct intr_poller *p;
	unsigned long flags;

	FIN;
	local_irq_save(flags);
	p = this_cpu_ptr(&intr_pollers);
	if (iop_is_sched(iop)) {
		xetrace("OOPS, iop=%p already sched\n", iop);
		BUG();
	}
	list_add_tail(&iop->list, &p->ctxs);
	++p->fetch;
	wake_up_process(p->t);
	local_irq_restore(flags);

	FOUT;
}

void intr_poll_complete(struct irq_poll *iop)
{
	unsigned long flags;

	FOUT;
	local_irq_save(flags);
	if (!iop_is_sched(iop)) {
		xetrace("OOPS, iop=%p NOT sched\n", iop);
		BUG();
	}
	list_del_init(&iop->list);
	/* the following provides a full memory barrier before the immediately
	   following non-value-returning atomic operation
	*/
	smp_mb__before_atomic();
	local_irq_restore(flags);
	FOUT;
}

bool intr_poll_is_sched(struct irq_poll *iop)
{
	return iop_is_sched(iop);
}
