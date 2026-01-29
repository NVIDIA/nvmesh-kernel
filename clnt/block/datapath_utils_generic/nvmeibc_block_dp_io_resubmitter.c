/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_io_resubmitter.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "../nvmeibc_block_common.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "block/recovery/nvmeibc_block_dp_sync_api.h"
#include "nvmeibc_block_dp_io_req_rel_locks.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.h"

static void __execute_resubmitted_on_wq(struct workqe_struct *work)
{
	struct operation *o = container_of(work, struct operation, work_resubmitted);
	nvmeibc_operation_execute(o, false);
};

static void __repeat_op_execution(struct operation* o, bool by_resubmitter)
{
	IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_RESUBMITTED);
	if (by_resubmitter){
		nflog(flog_repeat_op_execution1, "resubmit paused execute IO: o=@OPERATION", o);
	} else {
		nflog(flog_repeat_op_execution2, "repeat execution IO: o=@OPERATION", o);
	}
	#ifdef DEBUG
		WARN_ON(!nvmeib_stop_watch_is_start(&o->time.exec));
	#endif

	if (!by_resubmitter || !NVMEIB_CPU_MASK_INFO_IS_EMPTY(o->cpu_mask_info)) {
		WQ_INIT_WORK(&o->work_resubmitted, __execute_resubmitted_on_wq);
		dp_block_schedule_operation_work(o, &o->work_resubmitted);
	} else {
		nvmeibc_operation_execute(o, false);
	}
}

int nvmeibc_io_resubmitter_retry_op(struct operation *o)
{
	struct nvmeibc_block_device *nd = o->nd;
	bool const is_phased_out = o->topo ? o->topo->phased_out : true;

	{//cleanup
		DEBUG_TOPO_CNTRS_del_elem_from_topo(o);
		nvmeibc_topology_put(o->topo);
		nvmeibc_clmat_free(o);
		o->topo  = NULL;	// Disconnect 'o' from locks and cmds for debug only. Those lines are needed only for better undestanding, And debugging. Code will reinitialize those fields when 'o' is resubmitted in future.
		o->cmds  = NULL;
		o->locks = NULL;
		nvmeibc_atomic_set(&o->n_uncomp_raids, 0);
	}

	if (jiffies - o->jiffies1 > nd->max_retry_jiffies) { 		// Fail due to timeout
		nflog(flog_retry_op1, "avoid repeating execution - timeout; IO: o=@OPERATION", o);
		nvmeibc_operation_destroy(o, -EIO);
		return 0;
	}

	if (is_phased_out == false && nvmeibc_operation_can_execute_in_topo(&nd->topologies, o)) {
		//the operation may perfectly wait in the per-cpu queue, no need to bother the resubmitter
		__repeat_op_execution(o, false);
		return 0;
	}

	return nvmeibc_io_resubmitter_submit_op(o);
}

int nvmeibc_io_resubmitter_submit_op(struct operation *o)
{
	unsigned long flags;
	int retval = 0;
	struct nvmeibc_io_resubmitter *resub = &o->nd->dp.resub;
	OPERATION_DBG_CNTR_INC(o, n_resubmissions);
	spin_lock_irqsave(&resub->lock, flags);
	IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_RESUBMITTED_STARTED);
	list_add_tail(&o->list_paused, &resub->list_paused_ops);
	if (o->op == NVMEIB_BLOCK_IO_OP_READ)
		resub->n_reads_ops += 1;
	nvmeibc_io_resubmitter_wakeup(resub);
	spin_unlock_irqrestore(&resub->lock, flags);
	return retval;
}

int nvmeibc_io_resubmitter_init(struct nvmeibc_io_resubmitter *resub)
{
	spin_lock_init(&resub->lock);
	init_waitqueue_head(&resub->wait_queue);
	INIT_LIST_HEAD(&resub->list_paused_ops);
	resub->n_reads_ops = 0;
	resub->thread = NULL;
	return 0;
}

void nvmeibc_io_resubmitter_destroy(struct nvmeibc_io_resubmitter* resub)
{
	if (resub->thread) {
		kthread_stop(resub->thread);
		resub->thread = NULL;
	}
}

/* Assist thread for scheduling resubmittion of IO and other non urgent tasks (Sync operation, locks resubmition, etc). 1 copy for each block device. */
static int __resubmitter_func(void *p);

int nvmeibc_io_resubmitter_start(struct nvmeibc_io_resubmitter* resub, void *_dev)
{
	struct nvmeibc_block_device *dev = _dev;
	proc_name_t pname;
	clnt_proc_name_format(pname, 'C', "RB", dev->name, nvmeibc_cinst_get_blok_inst_num(nvmeibc_cinst_get_blok_p(dev)));
	if (unlikely(resub->thread)) {
		_NT(trace_dp_io_resubmitter_nvmeibc_io_resubmitter_start, "@DEV_NAME: Resubmitter already running", dev->name);
		goto _out;
	}
	resub->thread = kthread_run(__resubmitter_func, dev, "%s", pname);
	if (IS_ERR(resub->thread)) {
		_NT(error_dp_io_resubmitter_nvmeibc_io_resubmitter_start, "@DEV_NAME: Resubmitter failed: @PTR_ERR", dev->name, PTR_ERR(resub->thread));
		resub->thread = NULL;
		return -ENOMEM;
	}
_out:
	return 0;
}

static int __resubmitter_should_launch_syncs_unsafe(struct nvmeibc_block_device *nd)
{
	return (nvmeibc_datapath_syncs_any_pending_unsafe(&nd->dp.sync_rsrcs) && nvmeibc_datapath_syncs_can_launch(&nd->dp.sync_rsrcs));
}

static int __resubmitter_should_free_resources_unsafe(struct nvmeibc_block_device *nd)
{
	// If nvmeibc_datapath_syncs_can_launch() is false, resubmitter won't free the resources
	if (!nvmeibc_datapath_syncs_can_launch(&nd->dp.sync_rsrcs))
		return false;

	// The exact condition under which resubmitter will free all resources
	return nvmeibc_sync_mem_resources_reuse_should_free_unsafe(nd);
}

static int __resubmitter_should_launch_syncs_or_free_resources_unsafe(struct nvmeibc_block_device *nd)
{
	return (__resubmitter_should_launch_syncs_unsafe(nd) || __resubmitter_should_free_resources_unsafe(nd));
}

static int __resubmitter_should_launch_syncs_or_free_resources(struct nvmeibc_block_device *nd)
{
	bool result = false;
	unsigned long flags;

	spin_lock_irqsave(&nd->dp.resub.lock, flags);
	result = __resubmitter_should_launch_syncs_or_free_resources_unsafe(nd);
	spin_unlock_irqrestore(&nd->dp.resub.lock, flags);

	return result;
}

#define carrier_d_wakeup_cond(nd) \
	(nvmeibc_block_is_d_carrier(nd) && (nd->c_d_api.reconf_msg.all_bits))

#define basic_wakeup_cond(nd) \
	__resubmitter_should_launch_syncs_or_free_resources(nd) || \
	carrier_d_wakeup_cond(nd) || \
	kthread_should_stop()

#define DEFAULT_RESUB_AWAKE_THROTTLE_THRESHOLD_MS	(1000)
#define DEFAULT_RESUB_AWAKE_THROTTLE_SLEEP_MS		(10)

ulong nvmeibc_resub_awake_throttle_threshold_ms = DEFAULT_RESUB_AWAKE_THROTTLE_THRESHOLD_MS;
module_param_named(resub_awake_throttle_threshold_ms, nvmeibc_resub_awake_throttle_threshold_ms, ulong, 0644);
MODULE_PARM_DESC(resub_awake_throttle_threshold_ms, "Resubmit thread awake throttle threshold [millis]. 0 - throttle disabled.");

ulong nvmeibc_resub_awake_throttle_sleep_ms = DEFAULT_RESUB_AWAKE_THROTTLE_SLEEP_MS;
module_param_named(resub_awake_throttle_sleep_ms, nvmeibc_resub_awake_throttle_sleep_ms, ulong, 0644);
MODULE_PARM_DESC(resub_awake_throttle_sleep_ms, "Resubmit thread awake throttle sleep time [millis].");

static int __resubmitter_has_paused_ops(struct nvmeibc_block_device *nd)
{
	bool empty = false;
	unsigned long flags;

	spin_lock_irqsave(&nd->dp.resub.lock, flags);
	empty = list_empty(&nd->dp.resub.list_paused_ops);
	spin_unlock_irqrestore(&nd->dp.resub.lock, flags);

	return !empty;
}

/* The thread typically sleeps until it is awoken by incomming task */
static int __resubmitter_func(void *p)
{
	unsigned long flags;
	struct nvmeibc_block_device *nd = (struct nvmeibc_block_device *)p;
	struct operation *o = NULL;
	int count = 0;
	unsigned long jiffies_woke_up = jiffies;

	while (!kthread_should_stop()) {
		if (++count >= 32) {
			if (nvmeibc_resub_awake_throttle_threshold_ms &&
					unlikely(jiffies - jiffies_woke_up > msecs_to_jiffies(nvmeibc_resub_awake_throttle_threshold_ms))) {
				msleep(nvmeibc_resub_awake_throttle_sleep_ms);	/* Just be nice (EC-4803). */
				jiffies_woke_up = jiffies;
			} else
				cond_resched(); /* Prevent soft lockup */
			count = 0;
		}

		if (nvmeibc_block_is_d_carrier(nd) && (nd->c_d_api.reconf_msg.all_bits) && nd->c_d_api.on_reconf_cb) {
			nd->c_d_api.on_reconf_cb(&nd->c_d_api);
			nd->c_d_api.reconf_msg.all_bits = 0;
		}

		#ifndef BLKCMP_SO_LAUNCH_ON_CALLER_STACK
		if (nvmeibc_datapath_syncs_can_launch(&nd->dp.sync_rsrcs)) {
			uint i1, max_syncs = nvmeibc_sync_get_max_dev_sync_ops();
			for (i1 = 0; i1 < max_syncs; ++i1) {	// Throttle launching, to prevent soft lockup
				if (!nvmeibc_sync_submit(nd))
					break;
			}
			nvmeibc_sync_mem_resources_reuse_free_all(nd); // No need to cond_resched each k iterations here because number of resources < num running syncs
		}
		#endif

		spin_lock_irqsave(&nd->dp.resub.lock, flags);

		o = list_first_entry_or_null(&nd->dp.resub.list_paused_ops, struct operation, list_paused);	// The next operation to execute in round-robin manner
		if (o) {
			if (jiffies - o->jiffies1 >= nd->max_retry_jiffies) { 		// Fail due to timeout
				if (o->op == NVMEIB_BLOCK_IO_OP_READ)
					o->nd->dp.resub.n_reads_ops -= 1;
				list_del(&o->list_paused);
				spin_unlock_irqrestore(&nd->dp.resub.lock, flags);
				nflog(flog_2_dp_io_resubmitter_resubmitter_func, "resubmit paused fail IO: o=@OPERATION", o);
				nvmeibc_operation_destroy(o, -EIO);
			} else if (nvmeibc_operation_can_execute_in_topo(&nd->topologies, o)) {  // Try to re-excute the operation
				if (o->op == NVMEIB_BLOCK_IO_OP_READ)
					o->nd->dp.resub.n_reads_ops -= 1;
				list_del(&o->list_paused);
				spin_unlock_irqrestore(&nd->dp.resub.lock, flags);
				__repeat_op_execution(o, true);
			} else if (nd->topologies.io_perm == NVMEIB_IO_TYPE_PERMIT_RDONLY && nd->dp.resub.n_reads_ops) {
				list_del(&o->list_paused);
				list_add_tail(&o->list_paused, &o->nd->dp.resub.list_paused_ops);
				spin_unlock_irqrestore(&nd->dp.resub.lock, flags);
			} else { // nd->topologies.io_perm <= NVMEIB_IO_TYPE_PERMIT_NO_IO - Block until topolgy fixes or 1 sec passes
				spin_unlock_irqrestore(&nd->dp.resub.lock, flags);
				nflog(flog_4_dp_io_resubmitter_resubmitter_func, "resubmit skip operation: o=@OPERATION waiting_jiffies=@JIFFIES", o, (jiffies - o->jiffies1));
				wait_event_interruptible_timeout(nd->dp.resub.wait_queue,
					nvmeibc_topo_is_partially_ok(&nd->topologies) ||
					(jiffies - o->jiffies1 >= nd->max_retry_jiffies) ||
					basic_wakeup_cond(nd), HZ);
				jiffies_woke_up = jiffies;
			}
			continue;
		}
		// The list is empty - wait for new data
		spin_unlock_irqrestore(&nd->dp.resub.lock, flags);
		wait_event_interruptible(nd->dp.resub.wait_queue,
			__resubmitter_has_paused_ops(nd) || basic_wakeup_cond(nd));
		jiffies_woke_up = jiffies;
	}

	nvmeibc_sync_mem_resources_reuse_free_all(nd);

	nd->dp.resub.thread = NULL;
	return 0;
}
