/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include <linux/hrtimer.h>

#include "nvmeib_public_poll.h"
#include "nvmeib_public.h"
#define nvmeib_debug_level nvmeib_public_debug_level
#include "nvmeib_utils.h"
#include "../nvmeibp_trace.h"

struct nvmeib_public_per_core {
	struct nvmeib_public_poller *poller;
	int cpu_id;
	int node_id;
	int core_id;
	/* thread name */
	char name[16];
	/* the thread */
	struct task_struct *t;
	/* thread event list */
	struct list_head ctxs;
	/* thread wait for ctxs here */
	wait_queue_head_t wqh;
	/* new ctxs wait here */
	struct list_head new_ctxs;
	/* guard new ctxs */
	spinlock_t guard;
	volatile bool done;
	volatile bool fetch;
	bool started;
	u64 iterations;
};

#ifdef COMPILE_NANO_DELAY
static void __attribute__ ((unused)) nano_delay(unsigned int nsecs)
{
	struct hrtimer_sleeper hs;
	enum hrtimer_mode mode;
	ktime_t kt;

	kt = ktime_set(0, nsecs);
	mode = HRTIMER_MODE_REL;
	hrtimer_init_on_stack(&hs.timer, CLOCK_MONOTONIC, mode);
	hrtimer_set_expires(&hs.timer, kt);
	hrtimer_init_sleeper(&hs, current);
	set_current_state(TASK_UNINTERRUPTIBLE);
	hrtimer_start_expires(&hs.timer, mode);
	if (hs.task)
		io_schedule();
	hrtimer_cancel(&hs.timer);
}
#endif // COMPILE_NANO_DELAY

static void core_free(struct nvmeib_public_per_core *core)
{
	unsigned long flags;

	NFIN;
	_NT(core_free_t1,
		"Going to terminate kth @STR (@PTR)\n", core->name, core->t);
	if (current != core->t && core->started)
		kthread_stop(core->t);
	/* allow task_struct to be freed only after it cannot be accessed anymore
	 * the get was just after the thread creation */
	put_task_struct(core->t);
	if (!list_empty(&core->ctxs))
		_NW(core_free_w1, "Free core @STR with live ctxs", core->name);
	spin_lock_irqsave(&core->guard, flags);
	if (!list_empty(&core->new_ctxs))
		_NW(core_free_w2, "Free core @STR with nex_ctxs", core->name);
	spin_unlock_irqrestore(&core->guard, flags);
	core->poller->nodes[core->node_id].cores[core->core_id] = NULL;
	kfree(core);
	NFOUT;
}

static int poller_run(void *arg)
{
	struct nvmeib_public_per_core *core = arg;
	struct nvmeib_public_poll_ctx *e, *t;
	unsigned long flags;

	NFIN;
	while (!kthread_should_stop() && !core->done) {
		_NT(poller_run_t1, "waiting for work or stop...");
		spin_lock_irqsave(&core->guard, flags);
		wait_event_interruptible_lock_irq(core->wqh,
			!list_empty(&core->new_ctxs) || core->done, core->guard);
		list_splice_tail_init(&core->new_ctxs, &core->ctxs);
		spin_unlock_irqrestore(&core->guard, flags);
		/* the main and only real loop */
		while (!core->done && !list_empty(&core->ctxs)) {
			if (((++core->iterations) % 100000000ULL) == 0)
				_NI(poller_run_i1, "core @STR(@PTR): iterations @INT64",
					core->name, core, core->iterations);
			list_for_each_entry_safe(e, t, &core->ctxs, link)
				if (likely(!e->done))
					e->poll_f(e->poll_ctx);
				else {
					list_del(&e->link);
					e->done_f(e->done_ctx);
				}
			/* get new contexes */
			if (unlikely(core->fetch)) {
				spin_lock_irqsave(&core->guard, flags);
				list_splice_tail_init(&core->new_ctxs, &core->ctxs);
				core->fetch = false;
				spin_unlock_irqrestore(&core->guard, flags);
			}
			/* give the kernel some air... */
			//rep_nop();
			//nano_delay(1);
			cond_resched();
		}
	}
	_NT(poller_run_t2, "Poller core @STR is terminating", core->name);
	NFOUT;
	return 0;
}

static struct task_struct * create_thread(struct nvmeib_public_per_core *core,
	int cpu, const char *name)
{
	struct task_struct *t;

#if KS_KALLSYMS_LOOKUP
	struct task_struct * (*create_on_cpu)(int (*threadfn)(void *data),
										  void *data, unsigned int cpu,
										  const char *namefmt);
	unsigned long addr = kallsyms_lookup_name("kthread_create_on_cpu");
	/* this function does not exist in kernels - leave it for now */
	if (false && addr) {
		_NI(create_thread_t1, "Using kthread_create_on_cpu()");
		create_on_cpu = (typeof(create_on_cpu))addr;
		t = create_on_cpu(poller_run, core, cpu, name);
		goto out;
	}
	else
		goto bind;
#else
	goto bind;
#endif

bind:
	_NI(create_thread_t2, "Using kthread_create_on_node() and bind...");
	t = kthread_create_on_node(
			poller_run, core,  cpu_to_node(cpu), "%s", name);
	if (!IS_ERR(t))
		kthread_bind(t, cpu);

out:
	return t;
}

static struct nvmeib_public_per_core * create_core(
	struct nvmeib_public_poller *poller, int cpu_id, int node_id, int core_id)
{
	struct nvmeib_public_per_core *core;

	NFIN;
	if (!(core = kzalloc(sizeof(*core), GFP_KERNEL))) {
		_NE(create_core_e1, "OOM: Fail to allocate a per_core object");
		goto out;
	}
	snprintf(core->name, sizeof(core->name), "%s", "poller");

	spin_lock_init(&core->guard);

	core->t = create_thread(core, cpu_id, core->name);
	if (IS_ERR(core->t)) {
		_NE(create_core_e2, "Fail to create thread @STR - error @LONG",
			core->name, PTR_ERR(core->t));
		goto free_core;
	}
	_NI(create_core_t1, "Created CORE thread @STR (pid=@INT), "
						"numa=@INT, cpu=@INT, core=@INT",
		core->name, core->t->pid, node_id, cpu_id, core_id);

	/* increments usage counter so the thread function can exit without
	   waiting for kthread_stop.  Otherwise if the thread leaves before
	   the call to kthread_stop we will crash when we eventually
	   call to kthread_stop.
	*/
	//omril: why do we need this ability?
	//cant the thread wait on wqh till we call kthread_stop
	get_task_struct(core->t);
	/* beyond this point core_free() MUST be called  */
	core->poller = poller;
	core->cpu_id = cpu_id;
	core->node_id = node_id;
	core->core_id = core_id; //this is just the running index of num cpus this numa has...
	INIT_LIST_HEAD(&core->ctxs);
	INIT_LIST_HEAD(&core->new_ctxs);
	init_waitqueue_head(&core->wqh);
	spin_lock_init(&core->guard);
	core->poller->nodes[core->node_id].cores[core->core_id] = core;
	goto out;

free_core:
	kfree(core);
	core = NULL;

out:
	NFOUT;
	return core;
}

static void stop_core(struct nvmeib_public_per_core *core)
{
	NFIN;
	core->done = true;
	wake_up(&core->wqh);
	core_free(core);
	NFOUT;
}

static void free_node(struct nvmeib_public_per_node *node)
{
	int i;

	NFIN;
	_NI(free_node, "Stopping node @PTR", node);
	for (i = 0; i < node->n_cores; ++i)
		stop_core(node->cores[i]);
	kfree(node->cores);
	node->n_cores = 0;
	NFOUT;
}

static void start_core(struct nvmeib_public_poller *poller)
{
	int i, j;

	NFIN;
	for (i = 0; i < poller->n_nodes; ++i)
		for (j = 0; j < poller->nodes[i].n_cores; ++j)
			if (poller->nodes[i].cores[j]) {
				_ND(start_core_d1,
					"Waking up process @PTR on node @INT and core @INT",
					poller->nodes[i].cores[j]->t, i, j);
				wake_up_process(poller->nodes[i].cores[j]->t);	//omril: check rv
				poller->nodes[i].cores[j]->started = true;		//omril: set start before calling wake-up? incase fee can be called here
			}
	NFOUT;
}

static int * find_n_cores_numa_node(int node, int *cores)
{
	int cpu;
	int i = 0;
	int *cpus = NULL;

	NFIN;
	for_each_online_cpu(cpu)
		i += cpu_to_node(cpu) == node ? 1 : 0;
	if (i && (cpus = kmalloc(sizeof(*cpus) * i, GFP_KERNEL))) {
		*cores = i;
		i = 0;
		for_each_online_cpu(cpu)
			if (cpu_to_node(cpu) == node) 
				cpus[i++] = cpu;
	}
	NFOUT;
	return cpus;

}

struct nvmeib_public_poller * nvmeib_public_poller_create(void)
{
	struct nvmeib_public_poller *poller;
	int nodes;
	int *cores;
	int n_cores;
	int i, j;
	int free_cores = nvmeib_get_free_cores();

	NFIN;
	if (!(poller = kzalloc(sizeof(*poller), GFP_KERNEL))) {
		_NE(nvmeib_public_poller_create_e1,
			"OOM: Fail to allocate poller object");
		goto out;
	}
	if (!(nodes = num_online_nodes())) {
		_NE(nvmeib_public_poller_create_e2,
			"OOPS: failed to find NUMA nodes on the machine");
		goto free_poller;
	}
	else
		_ND(nvmeib_public_poller_create_d1,
			"Machines has @INT NUMA nodes\n", nodes);
	if (!(poller->nodes =
		  kzalloc(sizeof(*poller->nodes) * nodes, GFP_KERNEL))) {
		_NE(nvmeib_public_poller_create_e3,
			"OOM: Fail to allocate poller node objects");
		goto free_poller;
	}
	else
		poller->n_nodes = nodes;
	for (i = 0; i < poller->n_nodes; ++i) {
		if (!(cores = find_n_cores_numa_node(i, &n_cores))) {
			_NE(nvmeib_public_poller_create_e4,
				"OOPS: failed to find cores for NUMA node @INT", i);
			goto free_nodes;
		}
		else if (n_cores <= free_cores) {
			_NE(nvmeib_public_poller_create_e5,
				"OOPS: NUMA node @INT has @INT but free cores is @INT",
				i, n_cores, free_cores);
			kfree(cores);
			goto free_nodes;
		}
		else
			_NI(nvmeib_public_poller_create_d2,
				"NUMA node @INT has @INT cores: ", i, n_cores);

		n_cores -= free_cores;
		if (!(poller->nodes[i].cores = kzalloc(
			sizeof(*poller->nodes[i].cores) * n_cores, GFP_KERNEL))) {
			_NE(nvmeib_public_poller_create_e6,
				"OOM: Fail to allocate poller node cores");
			kfree(cores);
			goto free_nodes;
		}
		else {
			poller->nodes[i].n_cores = n_cores;
			poller->nodes[i].max_cores = n_cores + free_cores;
			for (j = 0; j < poller->nodes[i].n_cores; ++j) {
				if (!create_core(poller, cores[j], i, j)) /* cpu_id, int node_id, int core_id */
					_NE(nvmeib_public_poller_create_e7,
						"OOM: Fail to allocate node[@INT],core[@INT]", i, j);
			}
			kfree(cores);
		}
	}
	start_core(poller);
	poller->add_ctx = nvmeib_public_poller_add_ctx;
	goto out;

free_nodes:
	for (i = 0; i < poller->n_nodes; ++i)
		free_node(&poller->nodes[i]);
	kfree(poller->nodes);

free_poller:
	kfree(poller);
	poller = NULL;

out:
	NFOUT;
	return poller;
}
EXPORT_SYMBOL(nvmeib_public_poller_create);

void nvmeib_public_poller_free(struct nvmeib_public_poller *poller)
{
	int i;

	NFIN;
	if (poller) {
		_NI(nvmeib_public_poller_free_e1, "Freeing poller @PTR", poller);
		for (i = 0; i < poller->n_nodes; ++i)
			free_node(&poller->nodes[i]);
		kfree(poller->nodes);
		kfree(poller);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_poller_free);

void nvmeib_public_poller_add_ctx(
	struct nvmeib_public_per_core *core, struct nvmeib_public_poll_ctx *ctx)
{
	unsigned long flags;

	NFIN;
	_NI(nvmeib_public_poller_add_ctx_i1,
		"Add ctx @PTR to core @PTR", ctx, core);
	spin_lock_irqsave(&core->guard, flags);
	list_add_tail(&ctx->link, &core->new_ctxs);
	wake_up(&core->wqh);
	core->fetch = true;
	spin_unlock_irqrestore(&core->guard, flags);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_poller_add_ctx);

