#ifndef NVMEIBC_DP_OPERATION_PER_CPU_INFRA_H
#define NVMEIBC_DP_OPERATION_PER_CPU_INFRA_H

#include "common/compat/kr_incs_compiler_types.h"
#include "common/nvmeib_measured_work.h"
#include "clnt/nvmeibc_wq_metrics.h"

/********************** Per cpu counters **************************/
struct nvmeibc_blk_op_globals {		// Global counters to support block operations. Currently this is a single copy for all clnt instances, because this holds only per_cpu counters
	struct t_support_operation_cpu {
		struct {
			u64 req;				// Debug generated counter for io_cmd req. Due to scalability & performance reasons, use per-cpu counters & split the 64b range to: 2B (msb) as the cpu-id and 6B (lsb) as counter
			u64 op;					// Debug generated counter for operation
		} dbg_cntr;
		struct {					// Plug related stuff
			struct blk_plug plugp;
			bool in_plug;
		};
	} ____cacheline_aligned *o_cpu;	// Array, 1 element for each cpu
};
static struct nvmeibc_blk_op_globals *og = NULL;		//	In future, consider allocating this for each instance
static struct t_support_operation_cpu *o_cpu = NULL;

struct nvmeibc_blk_op_globals *nvmeibc_operation_all_create(void);
struct nvmeibc_blk_op_globals* nvmeibc_operation_all_create(void)
{
	//struct nvmeibc_blk_op_globals *og;
	int i;
	o_cpu = kzalloc(sizeof(*og) + sizeof(*o_cpu) * MAX_NUM_ACTIVE_CPUS, GFP_KERNEL);	// Allocate pointer and array together.
	if (!o_cpu)
		return NULL;
	og = (typeof(og))&o_cpu[MAX_NUM_ACTIVE_CPUS];
	og->o_cpu = o_cpu;
	for_each_allocated_cpu(i) {
		o_cpu[i].dbg_cntr.req = ((u64)i << 48);
	}
	return og;
}

void nvmeibc_operation_all_destroy(void /*struct nvmeibc_blk_op_globals *og*/);
void nvmeibc_operation_all_destroy(void /*struct nvmeibc_blk_op_globals *og*/)
{
	if (og) {
		void *mem = og->o_cpu;
		if (og->o_cpu) {
			int i;
			for_each_allocated_cpu(i) {
				WARN(og->o_cpu[i].in_plug, "nvmeibc: cpu=%d, still in plug", i);
			}
			og->o_cpu = NULL;
		}
		og = NULL;
		kfree(mem);
	}
}

/********************** Operation debug ID allocator **************************/
#define NVMEIBC_O_DBG_ID_BITS			(sizeof(o_dbg_id_t) * BITS_PER_BYTE)
#define NVMEIBC_O_DBG_ID_CPU_BITS		(ilog2(NR_CPUS) + 1)
#define NVMEIBC_O_DBG_ID_COUNTER_BITS	(NVMEIBC_O_DBG_ID_BITS - NVMEIBC_O_DBG_ID_CPU_BITS)

static o_dbg_id_t __alloc_dbg_id(void)
{
	const unsigned i = raw_smp_processor_id();
	const u64 dbg_id = (o_cpu[i].dbg_cntr.op++);
	BUILD_BUG_ON(NR_CPUS > (1 << NVMEIBC_O_DBG_ID_CPU_BITS));
	BUILD_BUG_ON(NVMEIBC_O_DBG_ID_CPU_BITS >= NVMEIBC_O_DBG_ID_BITS);
	return (dbg_id & GENMASK(NVMEIBC_O_DBG_ID_COUNTER_BITS - 1, 0)) | (i << NVMEIBC_O_DBG_ID_COUNTER_BITS);
}

u64 dp_cmds_req_alloc_unique_id(void)
{
	return (o_cpu[raw_smp_processor_id()].dbg_cntr.req++);
}

void nvmeibc_operation_alloc_dbg_id(struct operation *o)
{
	o->dbg_id = __alloc_dbg_id();
}

/********************** Plug + mini elev **************************/
// Todo: Move to separate file (Plug + Mini Elevator), create .h file API for elevator with this api
static void __mini_elevator_start_plug(struct nvmeibc_block_device *nd);
void nvmeibc_dp_operation_mi_clear_plug(struct task_struct *tsk);
static int __mini_elevator_try_unify_op(struct operation *o, ulong merge_lbas /* Typically ec slice_size */, struct operation **ro);
static void __mini_elevator_flush_if_needed(struct nvmeibc_block_device *nd, int cpu_id);

/******************************* Plug *****************************************/
static bool mini_elevator = false;
module_param(mini_elevator, bool, 0644);
MODULE_PARM_DESC(mini_elevator, "Enable mini-elevator which combines writes to erasure coded volumes to fill stripes. This functionality was experimental (and promising), but did not reach production quality.");
#define _NPLUGLOG _ND

// YR: We should have a linked list of all operations put into the cache this way... If the plug works well, we may not need the cache.
struct __mini_elevator_plug_cb {
	struct blk_plug_cb cb;
};

static int __plug_find_of_which_cpu(const struct blk_plug *ptr)
{
	if ((ptr >= &o_cpu[0].plugp) && (ptr < &o_cpu[MAX_NUM_ACTIVE_CPUS].plugp)) {
		const struct t_support_operation_cpu *c = container_of(ptr, struct t_support_operation_cpu, plugp);
		return (int)(c-o_cpu);
	} else if (ptr == NULL) {
		return -1;		// For debug:
	} else {
		return -2;		// For debug: External plug
	}
}

void nvmeibc_dp_operation_mi_clear_plug(struct task_struct *tsk)
{
	const int cpu_i = __plug_find_of_which_cpu(tsk->plug);
	if (cpu_i >= 0) {
		_NPLUGLOG(mini_elevatorf, "task=@PTR, name=@STR, used_@CPU, same_cpu=@BOOL_YN", tsk, tsk->comm, cpu_i, (cpu_i == raw_smp_processor_id()));
		tsk->plug = NULL;			// DHS: 'blk_finish_plug' was not called. Indicate that a batch of I/O submissions is complete
	}
}

static void __mini_elevator_unplug_cb(struct blk_plug_cb *cb, bool from_schedule)
{	// Called each time process is scheduled out,	Flush elevator immediately dont wait for timers
	struct __mini_elevator_plug_cb *plug_cb = container_of(cb, struct __mini_elevator_plug_cb, cb);
	struct nvmeibc_block_device *nd = cb->data;
	const int i = raw_smp_processor_id();
	_NPLUGLOG(mini_elevatorg, "Got called from_schedule=@BOOL_YN", from_schedule);
	o_cpu[i].in_plug = false;
	__mini_elevator_flush_if_needed(nd, i);
	kfree(plug_cb);
	nvmeibc_dp_operation_mi_clear_plug(current);
}

extern bool nvmeibc_prof_evt_registered;	// Do not include nvmeibc_main.h for a single module param in main
static void __mini_elevator_start_plug(struct nvmeibc_block_device *nd)
{																				// DHS: Examples in: https://code.woboq.org/linux/linux/drivers/md/raid10.c.html
	struct __mini_elevator_plug_cb *micb_unused;
	struct blk_plug_cb *cb;
	if (mini_elevator && nvmeibc_prof_evt_registered) {
		const int i = raw_smp_processor_id();
		if (!current->plug) {
			_NPLUGLOG(mini_elevatorh, "Adding plug");
			blk_start_plug(&o_cpu[i].plugp);									// Indicates to the block layer an intent by the caller cpu to submit multiple I/O requests in a batch
		} else {
			_NPLUGLOG(mini_elevatori, "Exist  plug on @CPU", __plug_find_of_which_cpu(current->plug));
		}
		// current->plug exists now
		cb = blk_check_plugged(__mini_elevator_unplug_cb, nd, sizeof(*micb_unused));	// Add unplug callback to current->plug->cb_list if doesn't exist
		if (cb) {	// Always true
			_NPLUGLOG(mini_elevatorj, "Adding unplug: cb=@PTR", cb);
			micb_unused = container_of(cb, struct __mini_elevator_plug_cb, cb);
			(void)micb_unused;															// Todo: Here initialize additional future fields
			o_cpu[i].in_plug = true;
		}
	}
}

/******************************* Mini Elevator ********************************/
#define _NMILOG _ND					// _NMILOG is for mini-elevator

#if ELEVATOR_TIMERS_IMPLEMENTATION
	static ulong mini_elevator_jiffies = 100;
	module_param(mini_elevator_jiffies, ulong, 0644);
	MODULE_PARM_DESC(mini_elevator_jiffies, "Defines the maximum number of jiffies an IO may reside in the mini-elevator.");
	#if USE_1_ELEV_HASH_TO_ALL_BDEVS
		static struct nvmeibc_elevator_operation_hash hash_elev;							// Instead of each block device using its hash, use a single hash, because timer gets only a single uint (key), It cannot get as param both key and pointer to hash
		static bool is_hash_elev_initiliaized = false;
		#define __elev_hash_ptr(mo) (&hash_elev)											// struct nvmeibc_blk_op_elevator *mo
	#endif
#else
	#define __elev_hash_ptr(mo) (&(mo)->hash)
#endif

static void __remove_from_mini_elevator_cache_unsafe(struct nvmeibc_blk_op_elevator *mo, int i)
{
	struct nvmeibc_merge_op_cache_t *nmoct = &mo->cache[i];
	#if ELEVATOR_TIMERS_IMPLEMENTATION
		struct nvmeibc_elevator_operation_hash *h = __elev_hash_ptr(mo);
		radix_tree_delete(&h->tree, nmoct->id);
		del_timer(&nmoct->timer);					// No need for del_timer_sync() because even if timer will fire it will not find the id in hash
	#endif
	mo->n_elem -= (nmoct->op != NULL);
	nmoct->op = NULL;
}

NVMEIBC_WQ_METRIC(nvmeibc_elevator_wq_latency, "reason=elevator");

static void __operation_execute_no_cache(struct operation *o, int rv);
static void __wq_execute_elevator(struct workqe_struct *work)
{
	struct measured_work *mw = measured_work_from(work);
	struct operation *o = container_of(mw, struct operation, work_elev);
	nvmeib_wq_metrics_update(nvmeibc_elevator_wq_latency, measured_work_wait_ticks(mw));
	__operation_execute_no_cache(o, 0);
};

static void __operation_schedule_execute_no_cache(struct operation *o)
{
	MEASURED_INIT_WORK(&o->work_elev, __wq_execute_elevator);
	dp_block_schedule_work(o->cpu_id, &o->work_elev.work);
}

#if ELEVATOR_TIMERS_IMPLEMENTATION
/* We have no lock on the block device, but nvmeibc_blk_op_elevator_destroy() guarantess that no timers will run after festruction fo block device */
TIMER_CALLBACK(__execute_no_cache, struct nvmeibc_merge_op_cache_t, timer, void, v)
	struct nvmeibc_elevator_operation_hash *h = __elev_hash_ptr(mo); // Todo: get the h from timer itself, change 'v' to represent hash and timers
	ulong id = (unsigned long)v;
	struct operation *o = 0;
	unsigned long flags;
	struct nvmeibc_merge_op_cache_t *nmoct;

	spin_lock_irqsave(&h->lock, flags);
	nmoct = radix_tree_delete(&h->tree, id);
	if (nmoct) {
		o = nmoct->op;
		nmoct->n_elem -= (nmoct->op != NULL);
		nmoct->op = NULL;
	} else { /* Most common case: Elevetor was flushed / op merged and executed directly etc */ }
	spin_unlock_irqrestore(&h->lock, flags);

	if (o) {
		_NMILOG(execute_no_cache1, "Executing from cache o=@OPERATION", o);
		__operation_schedule_execute_no_cache(o);
	} else {
		_NMILOG(execute_no_cache2, "Timer with no operation");
	}
}
#endif

static unsigned long __first_lba(const struct operation *o)
{
	return get_start_lba(o->bios[0]);
}

static unsigned long __last_lba1(const struct operation *o)
{
	struct bio_part *b = o->bios[o->num_bios-1];
	return get_start_lba(b) + get_nlbas(b);
}

static void __mini_elevator_force_flush_cpu(struct nvmeibc_block_device *nd, int cpu_id)
{
	struct nvmeibc_elevator_operation_hash *h = __elev_hash_ptr(&nd->merge_op);
	unsigned long flags;
	int i;
	struct nvmeibc_merge_op_cache_t *nmoct = nd->merge_op.cache;
	spin_lock_irqsave(&h->lock, flags);
	for (i = 0; i < ND_OP_CACHE_LEN; ++i, ++nmoct) {
		struct operation *o = nmoct->op;
		if (!o)
			continue;

		_ND(t_s2_cop, "i=@INT nmoct->op->@CPU cur@CPU", i, o->cpu_id, cpu_id);
		if (o->cpu_id == cpu_id) {
			__remove_from_mini_elevator_cache_unsafe(&nd->merge_op, i);
			__operation_schedule_execute_no_cache(o);
		}
	}
	spin_unlock_irqrestore(&h->lock, flags);
}

static void __mini_elevator_flush_if_needed(struct nvmeibc_block_device *nd, int cpu_id)
{
	const u32 n_active_ios = (u32)topo_get_cpu_ios(&nd->topologies, cpu_id);	// Illegal call: Will not return apper_bound coz irq is not disabled
	_ND(t_s1_cop, "@CPU.in_plug=@BOOL_YN nd->merge_op.n_elem=@INT n_active_ios=@N_IOS", cpu_id, o_cpu[cpu_id].in_plug, nd->merge_op.n_elem, n_active_ios);

	// If this core is far from full, release it's cache entries, Why? If user space is not issuing more IO's, there is no need to wait for timers to fire, in hope for future IO to merge with. Future IO will not come
	if (nd->merge_op.n_elem &&
		!o_cpu[cpu_id].in_plug    &&			// We were scheduled out, user space is not issuing more IO's
		(n_active_ios == 0 ||		// Even if the numbers do not sum up, when empty - flush
		 nd->merge_op.n_elem + n_active_ios <= max_ios_per_cpu)) {	// If not many are in work, Do not defer the decision. Do not wait for future arrivals for merge
		__mini_elevator_force_flush_cpu(nd, cpu_id);
	}
}

void __mini_elevator_force_flush_all_ioctl(struct nvmeibc_block_device *nd);
void __mini_elevator_force_flush_all_ioctl(struct nvmeibc_block_device *nd)
{
	int i;
	for_each_allocated_cpu(i) {
		__mini_elevator_force_flush_cpu(nd, i);
		o_cpu[i].in_plug = false;				// Now this is a very unsafe action!!!!
	}
}

static bool __check_can_merge_ops(const struct operation *o1, const struct operation *o2)
{
	return  (__last_lba1(o1) == __first_lba(o2)) &&
			(o1->num_bios < MAX_BIOS_PER_OP) &&
			(o1->topo == o2->topo);					// Or else we may create data corruption during switch topology if prev topo io is merged into new topo io
}

struct t_destroy_merged_op {
	struct nvmeibc_topology *topo;
	int cpu_id;
	bool was_chained;
	bool should_destroy;
};
#define t_destroy_merged_op_init(o) (struct t_destroy_merged_op){.topo = (o)->topo, .cpu_id = (o)->cpu_id, .was_chained = ((o)->chained_op != NULL), .should_destroy = true };

static struct t_destroy_merged_op __merge_ops(struct operation *o1, struct operation *o2)
{
	BUG_ON(o2->num_bios != 1);
	BUG_ON(o1->num_bios >= MAX_BIOS_PER_OP);
	o1->bios[o1->num_bios++] = o2->bios[0];
	o2->bios[0] = 0;									// Not mandatory: Just for debug
	o2->num_bios = 0;									// Not mandatory: Just for debug
	DEBUG_TOPO_CNTRS_del_elem_from_topo(o2);
	return t_destroy_merged_op_init(o2);
}

static void nvmeibc_operation_throttling_pull_next(struct nvmeibc_block_device *nd, const int cpu_id, bool is_chained);
static int __mini_elevator_try_unify_op(struct operation *o, ulong merge_lbas, struct operation **ro)
{
	struct nvmeibc_block_device *nd = o->nd;
	struct nvmeibc_elevator_operation_hash *h = __elev_hash_ptr(&nd->merge_op);
	const u64 o_first = (u64)__first_lba(o), o_last1 = (u64)__last_lba1(o);
	const bool is_at_start = !(o_first % merge_lbas), is_at_end = !(o_last1 % merge_lbas);				// Is aligned to [start..end] of merge_lba's range?
	const bool is_at_col_start = !(o_first % nd->dp.p.snake_size); // Is aligned to snake column start (relevant for EC only)
	int rv = 0, i, insert_cand = -1;					// Candidate for index in cache for insertion, empty place or oldest operation.
	ulong flags, min_timeout = 0;
	struct operation *execute_o = NULL;
	struct t_destroy_merged_op dmo = {0, 0, 0, 0};

	_NMILOG(mini_elevator0, "... vlba=[@VLBA..@VLBA), aligned [@BOOL_YN..@BOOL_YN]", o_first, o_last1, is_at_start, is_at_end);
	if (is_at_start && is_at_end) // We have the whole merge_lbas or multiple of it
		goto out;

	spin_lock_irqsave(&h->lock, flags);				// Let's check if we can merge
	for (i = 0; i < ND_OP_CACHE_LEN; ++i) {
		struct operation *o1 = nd->merge_op.cache[i].op;
		if (o1) {
			_NMILOG(mini_elevator1,       "... compare cache[@RV].op=@OPERATION (@VLBA) (@VLBA) o=@OPERATION", i, o1, (u64)__last_lba1(o1), o_first, o);
			if (__check_can_merge_ops(o1, o)) {
				_NMILOG(mini_elevator2,     "... merge   cache[@RV].op=@OPERATION <- o=@OPERATION", i, o1, o);
				dmo = __merge_ops(o1, o);
				if (is_at_end || o1->num_bios == MAX_BIOS_PER_OP) { // Cannot add more bios must execute
					_NMILOG(mini_elevator3, "... done    cache[@RV].op=@OPERATION <- o=@OPERATION", i, o1, o);
					*ro = o1;
					__remove_from_mini_elevator_cache_unsafe(&nd->merge_op, i);
					rv = 0;
				} else
					rv = 1; // Merged, but not executing yet
				goto unlock;
			}
			if ((!min_timeout && insert_cand < 0) || o1->jiffies1 < min_timeout) {
				min_timeout = o1->jiffies1;
				insert_cand = i;
			}
		} else {
			insert_cand = i;
			min_timeout = 0;
		}
	}

	// We did not find a merge
	#if !ELEVATOR_TIMERS_IMPLEMENTATION
		if (!o_cpu[raw_smp_processor_id()].in_plug) {				// If we put it into cache, no one is going to remove this from cache
			_NMILOG(mini_elevator4, "... executing as is o=@OPERATION, not in plug", o);
		} else
	#endif
	if ((merge_lbas == LOCKSET_SLICES) ||									// R1: Temp hack for mirror unification
		((nd->dp.p.snake_size > 1) ? is_at_col_start : is_at_start)) {		// EC: Single slice - only from slice start, with snake - only from column start
		struct nvmeibc_merge_op_cache_t *nmoct = &nd->merge_op.cache[insert_cand]; // Put in cache in the most appropriate place
		i = insert_cand;					// Just for short writing
		if (nmoct->op) { // We are replacing an op in the cache, so we will go ahead and execute it
			execute_o = nmoct->op;			// Note: If timer executes for this operation right now, it will wait on spinlock and will not find the 'op' in hash
			_NMILOG(mini_elevator5,     "... replace cache[@RV].op=@OPERATION <- o=@OPERATION", i, execute_o, o);
			#if ELEVATOR_TIMERS_IMPLEMENTATION
				radix_tree_delete(&h->tree, nmoct->id); // Optimization: Dont call __remove_from_mini_elevator_cache_unsafe() because most fields will be reinitialized
			#endif
		} else {						// Nothing to replace in the cache, so just adding us in
			_NMILOG(mini_elevator6,     "... add to cache[@RV].op=@OPERATION", i, o);
		}
		nd->merge_op.n_elem += (nmoct->op == NULL);
		nmoct->op = o;
		#if ELEVATOR_TIMERS_IMPLEMENTATION
			nmoct->id = ++h->id;
			radix_tree_insert(&h->tree, h->id, nmoct);
			TIMER_SET_DATA(nd, merge_op.cache[i].timer, h->id);
			mod_timer(&nmoct->timer, (o->jiffies1 + mini_elevator_jiffies));
		#endif
		rv = 2;
	} else {
		_NMILOG(mini_elevator7, "... executing as is o=@OPERATION, not at start", o);
	}

unlock:
	spin_unlock_irqrestore(&h->lock, flags);

out:
	if (dmo.should_destroy) { // original 'o' was merged and maybe already executed and its memory was freed (wen bio_part was freed). Simulate remains of nvmeibc_operation_destroy() on it
		nvmeibc_operation_throttling_pull_next(nd, dmo.cpu_id, dmo.was_chained);
		nvmeibc_topology_put(dmo.topo);
	}

	if (execute_o) {
		int cpu_id = raw_smp_processor_id();
		if (execute_o->cpu_id != cpu_id) {
			__operation_schedule_execute_no_cache(execute_o);
		} else {
			*ro = execute_o;
			rv = 0;
		}
	}

	return rv;
}

static void __nvmeibc_elevator_operation_hash_init(struct nvmeibc_elevator_operation_hash *h)
{
	#if USE_1_ELEV_HASH_TO_ALL_BDEVS
		if (is_hash_elev_initiliaized)
			return;
		is_hash_elev_initiliaized = true;
	#endif
	#if ELEVATOR_TIMERS_IMPLEMENTATION
		INIT_RADIX_TREE(&h->tree, GFP_ATOMIC);
		h->id = 117;
	#endif
	spin_lock_init(&h->lock);
}

static void __nvmeibc_elevator_operation_hash_destroy(__attribute__ ((unused)) struct nvmeibc_blk_op_elevator *mo)
{
	#if ELEVATOR_TIMERS_IMPLEMENTATION
		#if !USE_1_ELEV_HASH_TO_ALL_BDEVS
			struct nvmeibc_elevator_operation_hash *h = __elev_hash_ptr(mo);
			WARN_ON(!radix_tree_empty(&h->tree));
		#endif
	#endif
}


void nvmeibc_blk_op_elevator_init(struct nvmeibc_blk_op_elevator *mo)
{
	struct nvmeibc_elevator_operation_hash *h = __elev_hash_ptr(mo);
	#if ELEVATOR_TIMERS_IMPLEMENTATION
		int i;
		for (i = 0; i < ND_OP_CACHE_LEN; ++i) {
			struct nvmeibc_merge_op_cache_t *nmoct = &mo->cache[i];
			INIT_TIMER(&nmoct->timer);
			nmoct->timer.function = __execute_no_cache;
		}
	#endif
	__nvmeibc_elevator_operation_hash_init(h);
}

void nvmeibc_blk_op_elevator_destroy(__attribute__ ((unused)) struct nvmeibc_blk_op_elevator *mo)
{
	#if ELEVATOR_TIMERS_IMPLEMENTATION
		int i;
		for (i = 0; i < ND_OP_CACHE_LEN; ++i) {
			struct nvmeibc_merge_op_cache_t *nmoct = &mo->cache[i];
			del_timer_sync(&nmoct->timer);
		}
	#endif
	__nvmeibc_elevator_operation_hash_destroy(mo);
}

void nvmeibc_blk_op_elevator_tostring(struct nvmeibc_blk_op_elevator *mo, struct nvmeib_txt *txt)
{
	struct nvmeibc_elevator_operation_hash *h = __elev_hash_ptr(mo);
	int i, last_inplug_cpu = -1;
	unsigned long flags;

	nvmeib_txt_append(txt, "mini_elevator: in_plug={");

	// Find the last CPU with in_plug set
	for_each_allocated_cpu(i) {
		if (o_cpu[i].in_plug)
			last_inplug_cpu = i;
	}

	// Print CPUs with in_plug, omitting comma after the last one
	for_each_allocated_cpu(i) {
		if (!o_cpu[i].in_plug) continue;
		if (i == last_inplug_cpu)
			nvmeib_txt_append(txt, "%d", i);
		else
			nvmeib_txt_append(txt, "%d,", i);
	}

	nvmeib_txt_append(txt, "} cache={");

	spin_lock_irqsave(&h->lock, flags);
	for (i = 0; i < ND_OP_CACHE_LEN; ++i) {
		struct operation *o = mo->cache[i].op;
		if (o) {
			nvmeib_txt_append(txt, "{%d) op=%p, %ld[msec], cpu=%d},", i, o, (long)(1000 * (jiffies - o->jiffies1) / HZ), o->cpu_id);
		}
	}
	spin_unlock_irqrestore(&h->lock, flags);
	nvmeib_txt_append(txt, "}\n");
}

void nvmeibc_blk_op_elevator_tojson(struct nvmeibc_blk_op_elevator *mo, struct jdr *jdr)
{
	(void)mo;
	jdr->ops.ascii(jdr, "mini_elevator", "unsupported yet");
}

#endif	// H file
