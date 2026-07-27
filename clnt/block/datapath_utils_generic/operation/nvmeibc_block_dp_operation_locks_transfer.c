#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.h"	// Stale special value
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_operation.h"
#include "block/nvmeibc_block_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_profiling_lock_stages.h"
#include "nvmeibc_block_dp_operation_locks_transfer.h"

#define do_ow_locks_match(l1, l2) 	\
			(((l1)->ds == (l2)->ds) && ((l1)->address == (l2)->address))

/******************* Locks transferring between IO's **************************/
#define IO_LT_TRANSFERRED ((void *)0x1)
#define IO_LT_DONE        ((void *)0x2)
#define __IO_LT_still_unhandled(ptr) (((u64)(ptr)&(0x3)) == 0)

#define get_first_owner_lock_of(ls) ((ls)                                )
#define get_last_owner_lock_of( ls) ((ls) + (ls)[(ls)->nlocks-1].owner_id)

static uint per_cpu_lock_transfer_num = NVMEIBC_NUM_PREV_OWNERS;
module_param(per_cpu_lock_transfer_num, uint, 0444);
MODULE_PARM_DESC(per_cpu_lock_transfer_num, "Number of lock transfer candidates per CPU");


//why using hdr->good_path_profile? It was used before, before moving here.
//what we actually measure?
//1. start point - the operation requested lock transfer and
//2. end point:
//	 2.1 lock were transfered via nvmeibc_d_rdma_comp::callback call (sunny day)
//	 2.2. asker(me) or giver(other, from) decided that the locks cannot be transfered or received.
static inline struct nvmeibc_profiler* __IO_LT_get_profiler(struct nvmeibc_cmd_lock *locksets){
	return nvmeibc_get_raid_good_path_profile_for_rwt_op(locksets->ds, locksets->cmds->o->op);
}

static inline void __IO_LT_start_take_stats(struct nvmeibc_cmd_lock *locksets){
	struct nvmeibc_profiler* prof = __IO_LT_get_profiler(locksets);
	struct operation* obj = locksets->cmds ? locksets->cmds->o : NULL;

	nvmeibc_profiling_start_take_stats_for_stage(prof, obj, E_CMDS_STAGE_BLOCKSET_FIXUP);

	//if( nvmeibc_profiling_start_take_stats(prof, locksets) ){
	//  never get here - make sense - if profiler is free the operation takes it, thus lockest is out of lack
	//	nvmeibc_profiling_start_take_stats_for_stage(prof, locksets, E_CMDS_STAGE_BLOCKSET_FIXUP);
	//}
}

static inline void __IO_LT_end_take_stats(struct nvmeibc_cmd_lock *locksets){
	struct nvmeibc_profiler* prof = __IO_LT_get_profiler(locksets);
	struct operation* obj = locksets->cmds ? locksets->cmds->o : NULL;
	nvmeibc_profiling_end_take_stats_for_stage(prof, obj, E_CMDS_STAGE_BLOCKSET_FIXUP, 0);

	//nvmeibc_profiling_end_take_stats_for_stage(prof, locksets, E_CMDS_STAGE_BLOCKSET_FIXUP, 0);
	//nvmeibc_profiling_end_take_stats(prof, locksets, 0);
}

int __IO_LT_try_request_transfer(struct nvmeibc_cmd_lock *locksets)
{
	int lsi_start = 0;
	unsigned long flags;
	struct nvmeibc_cmd_lock *first_owner = get_first_owner_lock_of(locksets);
	struct nvmeibc_cmd_lock *last_owner =  get_last_owner_lock_of( locksets);
	struct nvmeibc_cmd_lock *cand;	// I will ask from 'get_locks_arr_of(cand)' to give me its last ow 'cand' to server as my first_owner
	struct nvmeibc_topo_percpu *last_ls = locksets->cmds->o->topo->percpu + get_cpu();		// Todo: use raw_smp_processor_id()
	unsigned i, n = MIN(per_cpu_lock_transfer_num, NVMEIBC_NUM_PREV_OWNERS); //
	struct nvmeibc_topo_percpu_entry *best = 0;

	put_cpu();
	locksets->last_ls = last_ls;
	spin_lock_irqsave(&last_ls->lock, flags);

	for (i = 0; i < n; ++i) {
		struct nvmeibc_topo_percpu_entry *e = &last_ls->array[i];
		cand = e->prev_owner;			// Under spinlock, so will not change
		if (!cand) {
			best = e;					// No candidate, may use this slot
		} else if (do_ow_locks_match(cand, first_owner)) {
			if (get_locks_arr_of(cand)->asker != IO_LT_TRANSFERRED && (++e->counter & 0xff)) {
				//BUG_ON((get_locks_arr_of(cand)->asker == IO_LT_DONE));
				BUG_ON((get_locks_arr_of(cand)->asker != NULL));
				_ND(trace_1_IO_LT, "locksets=@LOCKSETS requests transfer from locksets=@LOCKSETS", locksets, get_locks_arr_of(cand));
				lsi_start = first_owner->n_siblings;/* Skip the first owner lock+siblings*/
				cand->asker = locksets;
				locksets->giver = cand;
				__IO_LT_start_take_stats(locksets);
			} else {
				e->counter = 0;
				__IO_LT_refuse_transfer(get_locks_arr_of(cand));
			}
			best = e;					// Definitely use this slot, regardless if transferred or did not
			break;
		} else {
			if (!best || best->jiffies1 > e->jiffies1)
				best = e;
		}
	}

	if (i == n) {
		best->counter = 0;
		cand = best->prev_owner;
		if (cand) { /* Last lockset (IO) has no asker, It will never get one because I (locksets) am becoming the last lockset now. */
			__IO_LT_refuse_transfer(get_locks_arr_of(cand));
		}
	}

	_ND(trace_2_IO_LT, "locksets=@LOCKSETS setting prev_owner @LAST_OWNER", locksets, last_owner);
	best->prev_owner = last_owner; /* I am the last IO/lockset on this topo*/
	spin_unlock_irqrestore(&last_ls->lock, flags);
	return lsi_start;
}

static void __IO_LT_schedule(struct nvmeibc_cmd_lock *lock, void (*work_fn)(struct workqe_struct *))
{
	WQ_INIT_WORK(&lock->comp.transfer_work, work_fn);
	dp_block_schedule_operation_work(lock->cmds->o, &lock->comp.transfer_work);
}

static void __IO_LT_no_transfer_on_wq(struct workqe_struct *work)
{
	struct nvmeibc_cmd_lock *lock = container_of(work, struct nvmeibc_cmd_lock, comp.transfer_work);
	dp_locks_resend_raid_locks(lock);
};

static void __IO_LT_schedule_no_transfer(struct nvmeibc_cmd_lock *lock)
{
	IO_STATS_INCR(&lock->cmds->o->nd->dp.io_stats, DP_IO_STATS_LOCK_TRANSFER_REJECTED_COUNT);
	__IO_LT_schedule(lock, __IO_LT_no_transfer_on_wq);
}

static void __IO_LT_transfer_on_wq(struct workqe_struct *work)
{
	struct nvmeibc_cmd_lock *lock = container_of(work, struct nvmeibc_cmd_lock, comp.transfer_work);
	__IO_LT_complete_transfer_transaction(lock);
};

static void __IO_LT_schedule_transfer(struct nvmeibc_cmd_lock *lock)
{
	IO_STATS_INCR(&lock->cmds->o->nd->dp.io_stats, DP_IO_STATS_LOCK_TRANSFER_ACCEPTED_COUNT);
	__IO_LT_schedule(lock, __IO_LT_transfer_on_wq);
}

void __IO_LT_complete_locks(struct nvmeibc_cmd_lock *locks)
{
	unsigned long flags;
	struct nvmeibc_topo_percpu *last_ls = locks->last_ls;
	unsigned i, n;
	_ND(trace_3_IO_LT, "locksets=@LOCKSETS locks->asker=@ASKER", locks, locks->asker);
	if (last_ls && (locks->asker != IO_LT_DONE || locks->giver)) {
		struct nvmeibc_cmd_lock *last_owner = get_last_owner_lock_of(locks);
		_ND(trace_4_IO_LT, "locks: locksets=@LOCKSETS asker=@ASKER trigger=@TRIGGER last_ls=@LAST_LS @TOPOLOGY", locks, locks->asker, locks->giver, last_ls, locks->topo);
		spin_lock_irqsave(&last_ls->lock, flags);
		n = MIN(per_cpu_lock_transfer_num, NVMEIBC_NUM_PREV_OWNERS);
		for (i = 0; i < n; ++i) {
			struct nvmeibc_topo_percpu_entry *e = &last_ls->array[i];
			_ND(trace_5_IO_LT, "Comparing prev_owner @PREV_OWNER to @LAST_OWNER [locksets=@LOCKSETS]", e->prev_owner, last_owner, locks);
			if (e->prev_owner == last_owner) {
				/* I am the last IO on this topo/CPU no other IO could possibly request my last lock, so I will never transfer it */
				_ND(trace_6_IO_LT, "Setting last_ls prev_owner to 0");
				e->prev_owner = 0;	// Disconecting from topology
				if (locks->asker == IO_LT_TRANSFERRED)
					__IO_LT_refuse_transfer(locks); /* For debug only */
				break;
			}
		}
		if (locks->giver) {
			/* I requested a transfer from other IO, but due to my internal
			   error, I probably aborted the operation and don't want this
			   lock anymore. Don't allow the transfer to me. This check has
			   to made regardless of locks->asker value */
			locks->giver->asker = 0;
			__IO_LT_end_take_stats(locks);
		}
		if (__IO_LT_still_unhandled(locks->asker) && last_owner->asker) {
			/* Had to transfer the lock but I already abandoned or
			   could not take it (skipped release_lock()). Sorry */
			struct nvmeibc_cmd_lock *l = last_owner->asker;
			_ND(trace_7_IO_LT, "last_owner=@LAST_OWNER asker=@ASKER", last_owner, l);
			l->giver = 0; /* I (locks) can't transfer my lock to l */
			__IO_LT_refuse_transfer(locks); /* For debug only */
			spin_unlock_irqrestore(&last_ls->lock, flags);

			__IO_LT_end_take_stats(l);
			__IO_LT_schedule_no_transfer(l);
		} else
			spin_unlock_irqrestore(&last_ls->lock, flags);
	}
}

/* Make the transfer of all 'locksets[ow_i]' siblings to 'to[0]' siblings */
static int __IO_LT_transfer_locks_of_raid(struct nvmeibc_cmd_lock *locksets,
									int ow_i, struct nvmeibc_cmd_lock *to)
{
	struct nvmeibc_cmd_lock *fr = &locksets[ow_i];	// Copy from ow
	int i, c = fr->n_siblings, rv = -EIO;
	bool siblings_ok = true;
	to->giver = 0;
	if (unlikely((!to->cmds)||(to->cmds->locksets != to)||(c != to->n_siblings))) {
		WARN(true, "nvmeibc bug! (%d?=%d),%p,%p\n", c, to->n_siblings, to, to->cmds);
		goto _out;
	}

	/* Verify all siblings are taken & can be transferred */
	for (i = 0; i < c; i++)
		siblings_ok &= (fr[i].status == NCL_STATUS_TAKEN);
	if (unlikely(!siblings_ok))
		goto _out; /* At least 1 sibling is broken (dead disk, abandoned, etc)*/

	for (i = 0; i < c; i++) {			// Transfer fr[i] --> to[i]
		struct nvmeibc_cmd_lock *lf = &fr[i];
		struct nvmeibc_cmd_lock *lt = &to[i];
		__change_lock_status_to(lf, NCL_STATUS_TRANSFERRED);
		__invoke_crash_on_lock_corruption(locksets, ow_i+i, "mstarttransfer", 0);
		switch (lf->type) {
		case NVMEIBC_CMD_LOCK_OWNER:
		case NVMEIBC_CMD_LOCK_COPY_OWNER: {		// Copy owner[i]
			NVMEIBC_LOCK_SET_UNLOCK(lt, lf->unlock_val, lf->unlock_reason);
			lt->comp.compare = lf->comp.compare;
			__copy_blockset_info(lt, lf);
			lt->last_retry_report_time = lt->first_try_time = jiffies;
			break;
		}
		default:
			WARN(true, "Unhandled lock type: %d\n", lf->type);
			break;
		}
	}
	rv = c;
	_ND(trace_8_IO_LT, "@RV locks transfered from locksets=@LOCKSETS[@LSI] to locksets=@LOCKSETS[0]",c, locksets, ow_i, to);
_out:
	__IO_LT_refuse_transfer(locksets);	// Regardless whether transfer went OK or not, the action is done, nothing left to transfer.
	return rv;
}

void __IO_LT_try_transfer_give(struct nvmeibc_cmd_lock *locksets, int lsi)
{
	struct nvmeibc_cmd_lock *l = locksets + lsi, *lo = locksets + l->owner_id;
	if (lo != get_last_owner_lock_of(locksets))
		goto _out;									// Only transfer of last owner with siblings is supported

	/* We will release the lock, unless other IO requested to transfer it */
	_ND(trace_9_IO_LT, "locksets=@LOCKSETS[@LSI] l->asker=@LOCKSETS", locksets, lsi, lo->asker);
	if (__IO_LT_still_unhandled(locksets->asker)) {
		struct nvmeibc_cmd_lock *rcv_ow;	// Receving owner lock
		unsigned long flags;
		bool transferred = false;
		/* asker could be changed while we wait for spin_lock, so
		   will have to retest this value again below */
		spin_lock_irqsave(&locksets->last_ls->lock, flags);
		if (!__IO_LT_still_unhandled(locksets->asker)){
			/* Another sibling already transfered me while I was waiting for
			   the spinlock, or a newer IO decided it will not ask for locks
			   transfer for me. Anyways, I should skip transfer attempt*/
			spin_unlock_irqrestore(&locksets->last_ls->lock, flags);
			goto _out;
		} else if (!lo->asker) {
			/* No one requested transfer yet. Prevent future transfer requests
			   between this statement and successfull release of my lock */
			locksets->asker = IO_LT_TRANSFERRED;
			spin_unlock_irqrestore(&locksets->last_ls->lock, flags);
			goto _out;
		}
		rcv_ow = lo->asker;	/* Going to try to tansfer my locks */
		transferred = (__IO_LT_transfer_locks_of_raid(locksets, l->owner_id, rcv_ow) > 0);
		spin_unlock_irqrestore(&locksets->last_ls->lock, flags);
		if (transferred)
			__IO_LT_schedule_transfer(rcv_ow);
		else
			__IO_LT_schedule_no_transfer(rcv_ow);
	} /* else, non-transferrable lock or was already transfrered. release it */
_out:;
}

void __IO_LT_refuse_transfer(struct nvmeibc_cmd_lock *locksets)
{
	locksets->asker = IO_LT_DONE;	// No one can ask from me
}

void __IO_LT_complete_transfer_transaction(struct nvmeibc_cmd_lock *lo)
{
	const int n_copies = lo->n_siblings;
	int lsi;
	for (lsi = 0; lsi < n_copies; lsi++) {	// Daniel, Important! First mark all the copies as transferred to avoid double requesting: example, transferring owner which auto requests secondary owner while we transfer secondary owner
		lo[lsi].comp.lock_status = NCL_STATUS_TRANSFERRED;
	}
	
	__IO_LT_end_take_stats(lo);

	for (lsi = 0; lsi < n_copies; lsi++) {  // lo might free() in the loop, cacheded n_copies stops the loop
		struct nvmeibc_d_rdma_comp* cmp = &lo[lsi].comp;
		__invoke_crash_on_lock_corruption(lo, lsi, "mendtransfer", 0);
		_ND(trace_10_IO_LT, "TRANSFER locksets=@LOCKSETS callback cmp=@CMP_PTR", &lo[lsi], cmp);
		cmp->callback(cmp); /* Simulate success callback */
	}
}

bool __IO_LT_is_allowed_to_transfer_locks(const enum nvmeib_block_io_op op)
{
	return (op == NVMEIB_BLOCK_IO_OP_WRITE);	/* Skip optimization: TRIM -> Not helpful, READ - may only views locks - not going to release it. */
}
