#ifndef NVMEIBC_DP_OPERATION_THROTTLING_H
#define NVMEIBC_DP_OPERATION_THROTTLING_H

#include "common/nvmeib_measured_work.h"
#include "clnt/nvmeibc_wq_metrics.h"

NVMEIBC_WQ_METRIC(nvmeibc_throttle_wq_latency, "reason=throttle");

/**************************** IO throttleing **********************************/
unsigned max_ios_per_cpu = 64;
module_param(max_ios_per_cpu, uint, 0644);
MODULE_PARM_DESC(max_ios_per_cpu, "Maximum number of concurrent IO operations handled per core. Can be used to prevent IO flooding. In other words, the upper limit on the number of outstanding IOs to issue via the block driver per CPU core. Some file systems and applications queue or perform read-ahead very aggressively, likely to overcome problems with legacy storage solutions. With NVMesh, large numbers of outstanding read requests may lead to network congestion especially when target bandwidth exceeds client bandwidth. Throttling the number of outstanding requests using this parameter can reduce this congestion and improve overall quality of service. Limiting this value often ends up improving performance for the Client and others on the network. If in doubt, start with a value of 8. This setting can be applied dynamically to the kernel module without restarting services.");

static void __execute_chain_noplug(struct operation *o);
static void wq_execute_throttled_operation_chain(struct workqe_struct *work)
{
	struct measured_work *mw = measured_work_from(work);
	struct operation *o = container_of(mw, struct operation, work_throttled);
	nvmeib_wq_metrics_update(nvmeibc_throttle_wq_latency, measured_work_wait_ticks(mw));
	_ND(t_s3_cop, "o=@OPERATION", o);
	#if ELEVATOR_TIMERS_IMPLEMENTATION
		__execute_chain_noplug(o);					// Yaniv, Timers will take care of it
	#else
		nvmeibc_operation_execute_chain(o);			// Add plug to system-wq thread of current cpu, which submits the IO, when thread will be scheduled out, elevator will be flushed
	#endif
};

bool nvmeibc_operation_throttling_check_should_execute(struct operation *o)
{
/* the counting of IO's in-flight is achieved with 2 counters: the ios we   																																 .
 * started execution minus the IO's we completed.
 * since both are monotonically increasing & were the only one issuing (bcz 																																 .
 * its percpu), sampling the completed counter of another core is *safe* in 																																 .
 * the sense that the # of in-flight IO's can only get lower 																																				 .
 * since the issued counter is percpu, were the only one who can issue a new
 * IO, so as we acquire the core, the number of in-flight IO's can only
 * decrease. hence, we can safely test whether we can issue a new IO without
 * taking a lock !!!
 * the result is that the critical IO flow need not acquire the spinlock as
 * long as we dont need to push IO's into the wait-list, thereby shortening
 * the single-io latency.
 * NOTE: the wait-list spinlock is taken when an IO completes & since
 *  	 completion can be execution on any core, it constantly changes core
 *  	 ownership, thereby slowing down execution.
 *  	 that's why we try to refrain from acquiring the spinlock for every
 *  	 IO.
 */
	bool should_execute = true;
	int cpu_id = get_cpu();
	struct nvmeibc_topologies *nt = &o->nd->topologies;
	unsigned active_io = (unsigned)topo_get_cpu_ios(nt, cpu_id);		// this is slow (reads volatile) so make it early.
	struct topos_percpu *tp= nt->percpu + cpu_id;
	struct topo_percore_shared *tps = nt->percore_shared + cpu_id;
	unsigned long flags;
	{
		struct operation *iter;
		for_each_op_in_chain(iter, o)
			iter->cpu_id = cpu_id;
	}
	if (unlikely(active_io >= max_ios_per_cpu)) {
		/* Here active_io can decrese, so reread it with spinlock */
		spin_lock_irqsave(&tps->list_access, flags);
		if (likely((unsigned)topo_get_cpu_ios(nt, cpu_id) >= max_ios_per_cpu)) {
			list_add_tail(&o->per_cpu_wait_list, &tps->io_wait_list);
			should_execute = false;
			tps->n_wait_list++;
		}
		spin_unlock_irqrestore(&tps->list_access, flags);
	}
	if (should_execute) {
		tp->ios_issued++;
		#ifdef DEBUG_PERCPU_ISSUED_IO_CNTRS
			spin_lock_irqsave(&tps->list_access, flags);
			tps->n_ios++;
			WARN_ON(tps->n_ios != topo_get_cpu_ios(topologies,cpu_id));
			spin_unlock_irqrestore(&tps->list_access, flags);
		#endif
		__ndump_operation(should_execute, o);
	}
	put_cpu(); tp=NULL;
	return should_execute;
}

static void nvmeibc_operation_throttling_pull_next(struct nvmeibc_block_device *nd, const int cpu_id, bool is_chained)
{ /* If IO's wait in per cpu list, launch the head of the list */
	unsigned long flags;
	struct nvmeibc_topologies *nt = &nd->topologies;
	struct topo_percore_shared *tps = &nt->percore_shared[cpu_id];
	struct operation *next_o = NULL;
	if (is_chained)					// Operation chain is done only when the last member of chain is done. All others do not count. note: It is possible (low prob) the last member will be done before all others are done. This is not crucial. At worst - we have 1-2 extra ops in the air, who cares.
		return;
	spin_lock_irqsave(&tps->list_access, flags);
	if (!list_empty(&tps->io_wait_list)) {
		next_o = list_first_entry(&tps->io_wait_list, struct operation,
								  per_cpu_wait_list);
		list_del(&next_o->per_cpu_wait_list);
		tps->n_wait_list--;
		/* Note: Number of in air io's havent changed ('o' completed, but
		   'next_o' started). tp->ios_issued - tps->ios_completed remains
		   unchnaged. As optimization we don't do tps->ios_completed++ here
		   for 'o' and in execute_bio() did not do tp->ios_issued++ for
		   'next_o' so the diff between them is correct. */
		/* Note we havent decreased # of active IO's bcz the new IO we now
		 * dispacth wont increase the count either when it begin execution. 																																												,
		 * had we decreaseed the count, a newly arrived IO would see it 																																												    ,
		 * could start executing. and when this IO we dispatch would start  																																													    ,
		 * execution, we'll have more IO's executing then the max allowed */
	} else {
		tps->ios_completed++;
		#ifdef DEBUG_PERCPU_ISSUED_IO_CNTRS
			tps->n_ios--;
		#endif
		nflog(t_s0_cop, "IO completion with empty wait-list: @CPU, n_ios=@N_IOS, n_wait_list=@N_WAIT_LIST", cpu_id, topo_get_cpu_ios(nt, cpu_id), tps->n_wait_list);
	}
	spin_unlock_irqrestore(&tps->list_access, flags);
	__mini_elevator_flush_if_needed(nd, cpu_id);
	if (next_o) {
		// take topo now so it wont be freed while were on the queue,
		// *before* we put the topo !!!
		next_o->topo = nvmeibc_topology_get(nt);
		MEASURED_INIT_WORK(&next_o->work_throttled, wq_execute_throttled_operation_chain);
		nvmeib_schedule_work_on(cpu_id, &next_o->work_throttled.work);
	}
}

#endif	// H file
