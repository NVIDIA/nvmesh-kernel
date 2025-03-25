/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block.h"					// Must be first for simulator
#include "block/nvmeibc_block_common.h"
#include "nvmeibc_icore_ops.h"
#include "nvmeibc_block_dp_sync_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.h"
#include "block/recovery/nvmeibc_block_sync_profiling_stages.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec.h"
#include "block/nvmeibc_topology.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeibc_io_pet.h"
#include "common/nvmeib_measured_work.h"
#include "clnt/nvmeibc_wq_metrics.h"
#include "block/datapath_utils_generic/binfo/nvmeibc_block_dp_binfo.h"

// a module parameter to set limit of concurrent sync operations
uint nvmeibc_sync_max_operations_per_dev = NVMEIBC_MAX_ALLOWED_SYNC_OPS_DEFAULT;
module_param(nvmeibc_sync_max_operations_per_dev, uint, 0644);
MODULE_PARM_DESC(nvmeibc_sync_max_operations_per_dev, "Maximum number of outstanding sync (recovery) operations per volume. Maximum is 6144.");

uint nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_DEFAULT;
module_param(nvmeibc_sync_full_lockset_probability_factor, uint, 0644);
MODULE_PARM_DESC(nvmeibc_sync_full_lockset_probability_factor, "Defines the probability of sync'ing full 128K blocks instead of the current IO requested. It is used to avoid very slow IO during recovery, while avoiding wasteful repeat synchronizations. If the entire block is not synchronized, this will still need to be done by the regular recovery mechanism. The probability is computed by multiplying the number of blocks in the IO x param value / 3200. Range is 0 to 3200. 0 = never synchronize the full block. 3200 = always.");


NVMEIBC_MEMMGR_METRIC(dp_recov_operation, "component=raid.io_ctrl.recovery");
NVMEIBC_WQ_METRIC(nvmeibc_rso_execute_wq_latency, "reason=rso_execute");

/****************************** Internal API **********************************/
static void __cmd_fill_rldr_only(struct recovery_sync_op *so)
{
	struct nvmeibc_block_command *c = so->cmds;
	c->locksets = so->locks;				// Without this dummy cmd, locks cannot find 'o'
	c->o        = so->o;
	c->ncmds	= 1;
	c->ds		= so->locks->ds;			// Just an arbitrary command
	c->do_not_send = true;					// Dont even think to send them
}

int dp_sync_cmd_alloc_fill_rldr_only(struct recovery_sync_op *so)
{
	if (unlikely(!(so->cmds = dp_cmds_kvzalloc(1))))
		return -ENOMEM;
	__cmd_fill_rldr_only(so);
	return 0;
}

void dp_sync_cmd_init(struct recovery_sync_op *so, int i)
{
	struct nvmeibc_block_command *c = so->cmds;
	c[i].iocmd->comp.cmd = &c[i];
	c[i].cmdarr = c;
	c[i].locksets = so->locks;
	c[i].o = so->o;
	if (unlikely(c[i].ds->toma_acm == NVMEIBTC_DS_MODE_DEAD && !c[i].do_not_send)) {
		c[i].do_not_send = true; 			// So command will not be executed
		WARN(true, "nvmeibc bug");	// Daniel-Ronen 12/03/17, decision: N-replica, in any degraded mode. Client would write dirty instead of sync. Toma will sync dirty bits
		goto _out;
	}
	c[i].nlbas = so->n_slices;
	c[i].iocmd->reqs = &c[i].iocmd->reqs1;
	if (so->orig_rldr)
		c[i].iocmd->reqs->cpu_mask_info = &so->orig_rldr->o->cpu_mask_info;
	DEBUG_TRANSFERS_init_cb_counter(&c[i]);
_out:;
}

/************** Sync rescheduling on resubmitter thread ***********************/
static int __handle_locks_o(      struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag);	// take release locks state machine
static int __jour_garbg_collect_o(struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag);

#ifndef DP_LIB
void nvmeibc_mark_sync_wants_to_inject_caller_sgl(const struct nvmeibc_block_command *cmd)
{
	(void)cmd;	// In dplib canot inject to caller so have to explicitly mark this as todo. In kernel we inject to caller so no more work todo
}
#endif
#ifndef BLKCMP_SO_COMPLETION_PRESERVE_STACK
NVMEIBC_WQ_METRIC(nvmeibc_rso_resched_wq_latency, "reason=rso_resched");

static void set_callback_as_locks_state_machine(struct nvmeibc_d_rdma_comp *dc, int (*callback)(struct nvmeibc_d_rdma_comp*, struct nvmeibc_d_rdma_comp_tag))
{
	dc->callback = callback;	// Continue state machine from lock callback context
}

static inline void __verify_unlock_callback_is_correct(struct nvmeibc_d_rdma_comp *dc)
{
	BUG_ON((dc->callback != __handle_locks_o) && ( dc->callback != __jour_garbg_collect_o));
}

static void __resume_sync_operation_work(struct work_struct *work)
{
	struct measured_work *mw = measured_work_from(work);
	struct operation *o = container_of(mw, struct operation, work_rso_resched);
	struct recovery_sync_op *so = o->rso;
	struct nvmeibc_d_rdma_comp* dc = &so->locks[0].comp;
	nvmeib_wq_metrics_update(nvmeibc_rso_resched_wq_latency, measured_work_wait_ticks(mw));

	__handle_locks_o(dc, nvmeibc_d_rdma_comp_tag_make());	// == dc->callback(dc), Only locks SM rescheduling is supported for now. TODO: Support other sync SMs, including nested.
}

void nvmeibcbdp_sync_reschedule(struct recovery_sync_op *so)
{
	MEASURED_INIT_WORK(&so->o->work_rso_resched, __resume_sync_operation_work);
	dp_block_schedule_operation_work(so->o, &so->o->work_rso_resched.work);
}

#else
static int __um_completion_unblock_waiting_stack(struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l =  lock_of_bcomp(dc);		// Any lock from all siblings
	//struct recovery_sync_op *so = l->cmds->o->rso;
	(void)tag;
	SO_CMP_COMP(l->cmds->o);
	return 0;
}

static void set_callback_as_locks_state_machine(struct nvmeibc_d_rdma_comp *dc, int (*callback)(struct nvmeibc_d_rdma_comp*, struct nvmeibc_d_rdma_comp_tag))
{
	dc->callback = __um_completion_unblock_waiting_stack; // Callback to unblock (wakeup caller). Dont continue state machine in completion context
	(void)callback;
}

static inline void __verify_unlock_callback_is_correct(struct nvmeibc_d_rdma_comp *dc) { BUG_ON(dc->callback != __um_completion_unblock_waiting_stack); }

void nvmeibcbdp_sync_reschedule(struct recovery_sync_op *so) { (void)so; } 				// No rescheduling is needed
#endif

/******************* API for nesting callign of so ***************************/
void recovery_sync_stack_to_string(const struct recovery_sync_stack *st, char buf[32])
{
	int i, pos = 0, len = 32-3;	// Reserve 3 chars to "{}\0"
	buf[pos++] = '{';
	for (i = 0; i < (int)st->sp; i++) {
		pos += scnprintf(buf + pos, len - pos,
						 "{%2x,%2x}", st->op[i], st->stage[i]);
	}
	buf[pos++] = '}';
	buf[pos++] = '\0';
}

void nvmeibcbdpec_push_sm_to_stack(struct recovery_sync_op *so,
								   void (*ret)(struct recovery_sync_op *))
{
	struct recovery_sync_stack *st = &so->stack;
	if (st->sp == ARRAY_SIZE(st->ret)){	// Stack is full
		char buf[32];
		recovery_sync_stack_to_string(st, buf);
		WARN(true, "SO={%2x,%2x}, Stack=%s\n", so->o->op, so->stage, buf);
		BUG();
	}
	st->stage[   st->sp] = so->stage;
	st->ret[     st->sp] = ret;
	st->op      [st->sp] = so->o->op;
	st->cleanup[ st->sp] = NULL;
	st->sp++;
}

void nvmeibcbdpec_push_clean_to_stack(struct recovery_sync_op *so,
									  void (*cln)(struct recovery_sync_op *))
{
	struct recovery_sync_stack *st = &so->stack;
	BUG_ON(st->sp == 0 || st->cleanup[st->sp-1] != NULL);	// Must be called after nvmeibcbdpec_push_sm_to_stack()
	st->cleanup[st->sp-1] = cln;
}

static void __return_to_locks_unlock_sm(struct recovery_sync_op *so)
{
	struct nvmeibc_d_rdma_comp *dc = &so->locks[0].comp;
	if (should_blockset_info_commit(so)) { /* Commit blockset info if needed, prior to releasing locks */
		so->stage = sync_stage_recov_write_binfo;
	} else if (so->should_send_msg_blckst_recovrd) { 	  /* Send blockset recovered if needed, prior to releasing locks */
		so->stage = sync_stage_recov_send_recovered;
	} else {
		so->stage = sync_stage_recov_owner_ulock_sm;
	}
	__verify_unlock_callback_is_correct(dc);
	BLKCMP_SO_ASYNC_RESUME_CAL(dc->callback(dc, nvmeibc_d_rdma_comp_tag_make()));		// Transition back to locks state machine
}

void nvmeibcbdpec_return_to_caller_sm(struct recovery_sync_op *so)
{
	struct recovery_sync_stack *st = &so->stack;
	int stack_pos = st->sp;
	if (stack_pos) {			// Return to previous state machine if exists
		void (*f)(struct recovery_sync_op *);
		stack_pos = --(st->sp);
		f = st->ret[stack_pos]; st->ret[   stack_pos] = NULL;
		so->stage = st->stage[stack_pos];                          st->stage[ stack_pos] = 0;
		so->o->op  = st->op   [stack_pos];	                       st->op   [ stack_pos] = 0;
		if (st->cleanup[stack_pos]) {
			st->cleanup[stack_pos](so);	   /* Cleanup if needed*/ st->cleanup[stack_pos] = NULL;
		}
		BUG_ON(f == __return_to_locks_unlock_sm);		// Should be never pushed directly to stack!
		if (f == nvmeibcbdpec_return_to_caller_sm) {
			f(so);	// Keep unrolling the stack, and cleaning up transition between state machines
		} else {
			BLKCMP_SO_ASYNC_RESUME_CAL(f(so));	// Actually transition back to caller state machine
		}
	} else {				// No nested sync caller, Return to default 'unlocks' state machines
		__return_to_locks_unlock_sm(so);
	}
}

bool nvmeibcbd_sync_stack_is_htr_asking_for_extern_sm(const struct recovery_sync_op *so)
{
		return (so->stack.sp > 0) && 	// HTR ask assistance from already existing sync via the stack (nested so)
			(so->stack.op[so->stack.sp-1] == NVMEIB_BLOCK_IO_OP_RECOVER_STALE) &&	// Asker is indeed HTR
			(so->o->op != NVMEIB_BLOCK_IO_OP_RECOVER_STALE);					 	// HTR cant ask for another HTR only for no writhole stuff, like dbits turn off/on etc.
}

/*********************** Resource reuse between syncs ************************/
static void _resource_reuse_put_locked(struct nvmeibc_datapath_syncs_resources *sr, struct recovery_sync_op *so)
{
	if (so->rr_container.should_trasnfer_resources_for_another_sync) {
		recovery_sync_op_resources_reuse* rr = (void*)so;	// Use 'so' as container for resource.
		rr->rr_container.tcp_mode = get_tcp_mode_of_operation(so->o);
		so->o = NULL;	// Currently we pass only pages, not, 'o'. 'o' will be freed.
		sr->stats.num_el_in_resources_reuse_list++;
		list_add(&rr->sync_request_link, &sr->resources_reuse_list); // treat as stack, not list!
	}
}

static recovery_sync_op_resources_reuse* _resource_reuse_get_locked(struct nvmeibc_datapath_syncs_resources *sr)
{	// Atempt to take element resources from free list.
	recovery_sync_op_resources_reuse* rr = list_first_entry_or_null(&sr->resources_reuse_list, recovery_sync_op_resources_reuse, sync_request_link);
	if (rr) {
		list_del(&rr->sync_request_link);
		sr->stats.num_el_in_resources_reuse_list--;
		sr->stats.n_resources_reused++;
	}
	return rr;
}

static struct recovery_sync_op *__alloc_so(void)
{
	struct recovery_sync_op *so = my_kzalloc(sizeof(*so), GFP_ATOMIC);

	nvmesh_memmgr_metric_on_alloc_update(dp_recov_operation, sizeof(*so), so);
	return so;
}

static void __free_so(struct recovery_sync_op *so)
{
	if (so) {
		nvmesh_memmgr_metric_on_free_update(dp_recov_operation, sizeof(*so));
	}

	my_kfree(so);
}

bool nvmeibc_should_sync_reuse_memory = true;
module_param(nvmeibc_should_sync_reuse_memory, bool, 0644);
MODULE_PARM_DESC(nvmeibc_should_sync_reuse_memory, "Reuse pages across syncs (internal storage recovery operations) to reduce the number of page allocations and deallocations.");
static bool _resource_reuse_should_resue_if_not_free(struct recovery_sync_op *so)
{
	if (nvmeibc_should_sync_reuse_memory && !nvmeibc_pages_is_empty(&so->pages) && (so->n_slices == LOCKSET_SLICES)) {
		nvmeibc_pages_clean_for_reuse(&so->pages);
		_NTSO(t06csamr, "Add my resources @PTR to reuse list", so);
		return so->rr_container.should_trasnfer_resources_for_another_sync = true;
	} else {
		nvmeibc_pages_free(&so->pages, get_tcp_mode_of_operation(so->o));
		return so->rr_container.should_trasnfer_resources_for_another_sync = false;
	}
}

static void nvmeibc_sync_mem_resources_reuse_free(recovery_sync_op_resources_reuse* rr) {
	nvmeibc_pages_free(&rr->pages, rr->rr_container.tcp_mode);
	__free_so(rr);
}

static void _resource_reuse_merge_to_new_so(struct recovery_sync_op *so, recovery_sync_op_resources_reuse *rr)
{
	if (rr) {
		so->pages = rr->pages;	// Move pages from 'rr' into 'so'
		rr->pages = nvmeibc_pages_set_empty();
		nvmeibc_sync_mem_resources_reuse_free(rr);
		_NTSO(t07csamr, "Got my resources @PTR, from reuse list", rr);
	}
}

static bool __sync_mem_resources_reuse_should_free_unsafe(const struct nvmeibc_datapath_syncs_resources *sr)
{
	return (!list_empty(&sr->resources_reuse_list) && ((sr->stats.num_pending == 0) && (sr->stats.num_running == 0)));	// Syncs have stopped, free buffers for kernel/IO to use
}

bool nvmeibc_sync_mem_resources_reuse_should_free_unsafe(const struct nvmeibc_block_device *nd)
{
	return __sync_mem_resources_reuse_should_free_unsafe(&nd->dp.sync_rsrcs);
}

void nvmeibc_sync_mem_resources_reuse_free_all(struct nvmeibc_block_device *nd)
{
	unsigned long flags;
	struct nvmeibc_datapath_syncs_resources *sr = &nd->dp.sync_rsrcs;
	struct list_head temp_resources_list;
	INIT_LIST_HEAD(&temp_resources_list);
	spin_lock_irqsave(&nd->dp.resub.lock, flags);
	if (__sync_mem_resources_reuse_should_free_unsafe(sr)) {
		list_splice_init(&sr->resources_reuse_list, &temp_resources_list); // Free entire list, not under spinlock
		sr->stats.num_el_in_resources_reuse_list = 0;
	}
	spin_unlock_irqrestore(&nd->dp.resub.lock, flags);

	{	// Free the entire list, not under spinlock
		recovery_sync_op_resources_reuse *rr = NULL;
		while ((rr = list_first_entry_or_null(&temp_resources_list, recovery_sync_op_resources_reuse, sync_request_link)) != NULL) {
			list_del(&rr->sync_request_link);
			nvmeibc_sync_mem_resources_reuse_free(rr);
		}
	}
}

int nvmeibc_sync_alloc_mem_resources(struct recovery_sync_op* so)
{
	const u32 nblocks_needed = so->r1->replicas * so->n_slices;
	const int tcp_mode = get_tcp_mode_of_operation(so->o);
	if (!nvmeibc_pages_is_empty(&so->pages)) {
		if (so->pages.n_blks_total >= nblocks_needed)
			return 0;			// Can reuse the resource, nothing to do
		_NTSO(t05csamr, "Reused @INT[blks] < @INT, reallocating. tcp=@INT", so->pages.n_blks_total, nblocks_needed, tcp_mode);
		nvmeibc_pages_free(&so->pages, tcp_mode);
	}
	return nvmeibc_pages_alloc(&so->pages, nblocks_needed, tcp_mode);
}

/**************************** Throttling of Syncs *****************************/
static void __syncs_list_op(const int action, struct recovery_sync_op *so)
{
	unsigned long flags;
	struct nvmeibc_datapath *dp = &so->o->nd->dp;
	struct nvmeibc_sync_stats *ss = &dp->sync_rsrcs.stats;
	spin_lock_irqsave(&dp->resub.lock, flags);
	__ndump_operation(syncs_list_op, &so->o);
	if (action == '+') {	// When new sync op is added to the listy
		list_add_tail(&so->sync_request_link, &dp->sync_rsrcs.request_list);
		ss->num_pending++;
	} else {// == '-'		// When running sync op finished
		ss->num_running--;
		ss->num_total++;
		if (is_op_sync_maintain(so->o->op))
			ss->num_total_maintainance++;
		else if (is_op_sync_db(so->o->op)) {
			if (__is_raid1_mirror(so) && (so->R1.is_dirty_suspect))
				ss->num_dirty_bit_suspect++;
		} else if (so->o->op == NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE) {
			ss->num_commit_stale++;
		} else if (so->o->op == NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO) {
			ss->num_commit_binfo++;
		}
		if (so->error == 0) {
			const u32 msecs_comp = ((u64)(jiffies - so->o->jiffies1) * 1000)/HZ;
			if (ss->longest_sync_time < msecs_comp)
				ss->longest_sync_time = msecs_comp;
			if (so->n_slices == LOCKSET_SLICES)
				ss->num_full_blockset_ok++;
			else
				ss->num_part_blockset_ok++;
		}
		_resource_reuse_put_locked(&dp->sync_rsrcs, so);
	}
	nvmeibc_io_resubmitter_wakeup(&dp->resub);
	spin_unlock_irqrestore(&dp->resub.lock, flags);
	#ifdef BLKCMP_SO_LAUNCH_ON_CALLER_STACK
		if (action == '+')
			nvmeibc_sync_submit(so->o->nd);
	#endif
}

bool nvmeibc_datapath_syncs_can_launch(const struct nvmeibc_datapath_syncs_resources *sr)
{
	return (sr->stats.num_running < nvmeibc_sync_get_max_dev_sync_ops());
}

bool nvmeibc_datapath_syncs_any_pending_unsafe(const struct nvmeibc_datapath_syncs_resources *sr)
{
	return !list_empty(&sr->request_list);
}

static struct recovery_sync_op * __syncs_list_get_so(struct nvmeibc_block_device *nd, const bool should_autofail)
{
	unsigned long flags;
	struct recovery_sync_op *so = NULL;
	struct nvmeibc_datapath_syncs_resources *sr = &nd->dp.sync_rsrcs;
	struct nvmeibc_sync_stats *ss = &sr->stats;
	recovery_sync_op_resources_reuse *rr = NULL;
	spin_lock_irqsave(&nd->dp.resub.lock, flags);
	if (nvmeibc_datapath_syncs_can_launch(sr) || should_autofail) {
		if ((so = list_first_entry_or_null(&sr->request_list, struct recovery_sync_op, sync_request_link))) {
			list_del(&so->sync_request_link);
			--ss->num_pending;
			++ss->num_running;
			rr = _resource_reuse_get_locked(sr);
		}
	}
	spin_unlock_irqrestore(&nd->dp.resub.lock, flags);
	_resource_reuse_merge_to_new_so(so, rr);
	return so;
}

static void __free_sync_op(struct recovery_sync_op *so, bool release_slot)
{
	struct operation *o;
	bool should_trasnfer_resources = false;
	if (!so)
		return;
	if (!so->o) {
		__free_so(so);
		return;
	}

	_NDSO(t01fso, " free ");
	o = so->o;
	if (so->cmds) { /* Free commands and return cb as fast as possible */
		dp_cmds_free_all(so->cmds);
	}
	should_trasnfer_resources = _resource_reuse_should_resue_if_not_free(so);
	nvmeibc_profiling_end_take_stats(so->r1->hdr->sync_profile, so, so->error);
	if (so->when_done_cb)
		so->when_done_cb(so->when_done_context, so->error);
	DEBUG_TOPO_CNTRS_del_elem_from_topo(o);

	/* Slot released before releasing topo to prevent crash in shutdown. */
	if (release_slot)
		__syncs_list_op('-', so);
	nvmeibc_topology_put(o->topo);
	if (!should_trasnfer_resources)	// Cached on stack. Else, 'so' was already reused. Do not access it
		__free_so(so);

	// Free remaining 'o', without 'so'
	if (o->mssa) {
		my_kfree(o->mssa->gf_blocks.bio);
		my_kfree(o->mssa->gf_blocks.prd);
		my_kfree(o->mssa->blocks_md);
	}
	nvmeib_pet_journal_commit(&o->journal);
	nvmeibc_operation_free(o);
}

/************************** Handling Locks for Sync ***************************/
/* Todo: Unify with __set_cmpxchg_for_release() */

static void __prepare_lock_for_release(struct nvmeibc_cmd_lock *l, u64 exch)
{
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	l->comp.code = NVMEIBC_CMD_LOCK_UNLOCK;
	dc->compare = dc->exchange;
	dc->exchange = exch;
}

#define LARGE_SYNC_DEBUG_VALUE (0x013100) // We have many counters that go up and down (like amount of uncompleted locks. Add this big value to the counter and remove at the end to debug, whether the counters don't go negative)
static void __verify_sync_locks_all_done(struct recovery_sync_op *so)
{
	int i, val;
	if (so->error != 0)
		return;		// Todo: Test also when error occurs
	for (i = 0; i < so->locks->nlocks; i++) {
		val = nvmeibc_atomic_read(&so->locks[i].n_uncompleted_locks);
		WARN_ON(val != ((i == 0) ? LARGE_SYNC_DEBUG_VALUE : 0));
		WARN_ON(so->locks[i].status != NCL_STATUS_DONE);
	}
	BUILD_BUG_ON(LARGE_SYNC_DEBUG_VALUE == LARGE_DEBUG_VALUE);		// They must be different, for debug reasons
}

static void __compressed_sync_op_trace_start(const struct recovery_sync_op *so) {
	const u32 vol_id = nvmeibc_volume_short_id(so->o->nd);
	const struct nvmeibc_subscription_ctx *tr = so->locks->ds->toma_reg;
	// Attention: This compressed bitfield trace is active in good path. It must stay fast and compact.  Never add dynamically sized values, only known sizes.
	NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: Sync start: ORIG_O: {@O_DBG_ID}, @SYNC_START_DUMP", _I, goodpath_nvmeibc_syncs, compressed_sync_op_trace_start,
			so->o->dbg_id, so->orig_rldr ? so->orig_rldr->o->dbg_id : 0,
			so->o->op, vol_id, (u64)so->o->topo->debug_unique_index,
			tr->ch, tr->r1, so->rlba,
			so->start_slice, so->n_slices);
	NVMEIBC_IO_PET_MSG_NORM(
		&so->o->journal,
		"sync_start(origr_o_dbg_id=%u op=%hhu<enum nvmeib_block_io_op> vol_id=%u topo=%llu rlba=%llu slices=%hhu-%hhu)",
		so->orig_rldr ? so->orig_rldr->o->dbg_id : 0, numeric_downcast(u8, so->o->op), vol_id,
		(u64)so->o->topo->debug_unique_index, so->rlba, numeric_downcast(u8, so->start_slice), numeric_downcast(u8, so->n_slices));
}

static void __compressed_sync_op_trace_write_binfo(const struct recovery_sync_op *so) {
	const u32 bi_post = so->cmds->rld.post.all, bi_pre = so->cmds->rld.pre.all;
	NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: Sync write binfo: PRE: @BINFO POST: @BINFO", _T, goodpath_nvmeibc_syncs, compressed_sync_op_write_binfo, so->o->dbg_id, bi_pre, bi_post);
}

static void __compressed_sync_op_trace_end(const struct recovery_sync_op *so) {
	NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: Sync end: RV: @RV", _I, goodpath_nvmeibc_syncs, compressed_sync_op_trace_end, so->o->dbg_id, so->error);
	NVMEIBC_IO_PET_MSG(&so->o->journal, "sync_end(orig_o_dbg_id=%u) = %d",
			   so->error ? NVMEIB_PET_SEVERITY_WARNING : NVMEIB_PET_SEVERITY_NORMAL, so->o->dbg_id,
			   so->error);
}

static int __do_on_stage_done(struct recovery_sync_op *so) {
	if (!so->assume_caller_holds_locks)
		__verify_sync_locks_all_done(so);
	if (so->is_autonomous) {
		#ifdef AUTONOMOUS_SYNCS_STATS
			struct nvmeibc_sync_stats *ss = &so->o->nd->dp.sync_rsrcs.stats;
			if (likely(so->error == 0))
				atomic64_inc(&ss->num_autonomous_ok);
			else
				atomic64_inc(&ss->num_autonomous_err);
		#endif
	}
	__compressed_sync_op_trace_end(so);
	__ndump_operation(sync_stage_done, &so->o);
	__free_sync_op(so, !so->is_autonomous);
	return 0;
}

/* Much like: like dp_locks_release_cb() but simpler, does not rereg topo. Used for garbage collection */
static void __sync_dp_locks_release_cb(struct nvmeibc_cmd_lock *l, struct nvmeibc_d_rdma_comp *dc)
{
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	_ND(trace_dp_sync_common_sync_dp_locks_release_cb, "release_active=@RELEASE_ACTIVE", l->status);
	__invoke_crash_on_lock_corruption(l, 0, "release", 1);
	if (NCL_had_release_callback(dc->lock_status))
		icore_ops->cb_called_comp(icore_ops, l->ds->disk, dc);
	l->status = NCL_STATUS_DONE;
	__invoke_crash_on_lock_corruption(l, 0, "free", 0);
	nvmeibc_atomic_dec(&dp_locks_get_locks_header(l)->n_uncompleted_locks);
}

/* Return 0 if callback will arrive, otherwise error code without callback */
static int __release_lock_of_sync(struct nvmeibc_cmd_lock *l, struct recovery_sync_op *so)
{
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	int rv;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	dp_locks_trace_lock_release(so->o, l);
	dc->opr = NVMEIBC_LOCK_CMP_AND_SWAP;
	nvmeibc_cmd_lock_request_io_pet_describe(so->o, l);
	rv = icore_ops->run_cmpxchg(icore_ops, l->ds->disk, handle_of(l->ds), l->address, dc);
	if (rv) {
		__change_lock_status_to(l, NCL_STATUS_FAIL_NO_COMP);
		so->error = -10016;
	}
	return rv;
}

/* Set context variable of lock to point to a given lock */
#define __set_lock_ptr_to(lock) ({l = (lock); lock_comp = &l->comp; })

static struct stale_lock_resolver_t *__get_slr_of_so(struct recovery_sync_op *so)
{
	return nvmeibc_raid_topo_persistent_get_slr(so->r1->hdr);
}

/* Retry retaking owner lock, if contended */
static inline void __retry_aquire_lock(struct nvmeibc_d_rdma_comp *lock_comp, struct nvmeibc_cmd_lock *l, struct recovery_sync_op *so)
{
	const union nvmeib_lock_id holder = nvmeibc_d_rdma_comp_get_contending_id(lock_comp);
	if (unlikely(so->o->topo->phased_out)) {
		so->error = -10011;	/* Immediate failure to speed up topo-free */
	}

	if (holder.all == LS_UNLOCKED) {
		if (is_op_sync_stale(so->o->op)){
			/* Owner is 0. If we hold at least 1 lock than should proceed
			   (secondary is zero), otherwise abort sync operation, probably other
			   client already synced */
			if ((l->lockset_idx > 0) && NCL_do_i_have_lock(l[-1].status))
				goto _retry;	// Try unlocked
			else {
				// IMPORTANT: Other client did the sync but this might not be good enough for me. The direction of the sync is crucial for some callers (roll-fwd/foll-backwards).
				// If the caller needed to commit his read data to the entire slice and other client in different topo did roll-bkwrds then caller will do data corruption.
				// Caller which does not care about direction: Recovery/Write bio. Read bio in some topologies, does.
				// In order not to define the exact condition on caller we just make a broad test.
				if (nvmeibc_raid_is_ec(so->r1)){
					// EC: supports only roll-fwd so other client cannot sync in opposite direction
					const struct nvmeibc_subscription_ctx *tr = l->ds->toma_reg;
					int lock_idx;
					_NTSO(t_01_sral, "Primary owner lock is clean. Lock was cleared by other client? so=@SO raid=(@CH,@R1_INT,@SI) @RLBA", so, tr->ch, tr->r1, tr->seg, so->rlba);
					so->error = 0;
					so->stage = sync_stage_done;		// No need to take locks not release. Finish the sync
					for (lock_idx = 0; lock_idx < so->locks->nlocks; lock_idx++) {
						nvmeibc_atomic_set(&so->locks[lock_idx].n_uncompleted_locks, (lock_idx == 0)? LARGE_SYNC_DEBUG_VALUE : 0);
						__change_lock_status_to(&so->locks[lock_idx], NCL_STATUS_DONE);
					}
					so->stats.other_client_did = true;
				} else {
					const struct operation *caller = so->orig_rldr->o;
					if (caller->op == NVMEIB_BLOCK_IO_OP_RECOV_PROBLEM) {
						// No IO is waiting so we can continue fixing stale lock even though other client could already fixed it in revere direction
						_NTSO(t_02_sral, "Overcome bug EC-6870. Sync retries to acquire unlocked primary owner, Just to clean other copies");
						goto _retry;	// Try unlocked
					} else {
						_NTSO(t_03_sral, "Cancel sync. Otherwise might do data corurption of caller io {@O_DBG_ID} op=@BLOCK_IO_OP", caller->dbg_id, caller->op);
						so->error = -10012;	// Cancel sync . Primary owner is 0, secondary might remain stale becasue secondary owner stale special.
					}
				}
				goto _out;
			}
		} else {
			goto _retry;	// Try unlocked. Perfectly valid if stale is not expected
		}
	}

	if (!holder.bits.is_stale) {
		_NTSO(t_04_sral, "Canceling sync, not stale, contending @LOCKID", holder.all);
		so->error = -10013;	/* Cancel sync (other clnt already cleaning lock) */
		goto _out;
	}
	{
		struct stale_lock_resolver_t *slr = __get_slr_of_so(so);
		const enum stale_lock_resolve_status ss = stale_lock_resolver_get_status(slr, holder.all, l);
		if (holder.bits.is_read) { // EC: lock is stale but slice was not corrupted, Must query stale locks resolver, to get recoveree UUID for blockset recovered message
			goto _retry;
		}
		if (ss != stale_lock_resolve_safe_to_use) {
			_NTSO(t_05_sral, "Canceling sync, lock resolve status, contending @LOCKID", holder.all);
			so->error = -10014;	/* Cancel sync (Toma, didnt clean-up yet). */
			goto _out;
		}
	}
_retry:
	__invoke_crash_on_lock_corruption(l, 0, "take", -1);
	lock_comp->compare = holder.all;
	so->stage = sync_stage_recov_lo_try_lock;
	if (unlikely(so->o->topo->phased_out)) {				// Cancel 'sync'. dont hold topology too long
		so->error = -10019;
	}
_out:
	return;
}
/*  Get UUID of the client under recovery - Do it once, coz all Tomas should
	   hold identical UUID. Can also do it once for first taken lock */
static inline void __fill_recoveree_uuid(struct nvmeibc_d_rdma_comp *dc, struct recovery_sync_op *so)
{
	const union nvmeib_lock_id holder = nvmeibc_d_rdma_comp_get_contending_id(dc);

	if (holder.bits.is_stale) {
		so->should_send_msg_blckst_recovrd = true;		// Assuming we are going to solve the problem (sync mutation will make a decision)
		if (!nvmeib_uuid_cmp(NULL_UUID_BE, so->recoveree_cuuid)) {
			struct stale_lock_resolver_t *slr = __get_slr_of_so(so);
			if (stale_lock_resolver_fill_cuuid_by_lockid(slr, holder.all, &so->recoveree_cuuid) != 0) {
				/* Rare race condition, lock taken but slr was flushed */
				so->error = -10015;
			} else if (!nvmeib_uuid_cmp(NULL_UUID_BE, so->recoveree_cuuid)) {			// Very bad, sync will stuck
				#if KS_SECTION_GET_STR_AS_PARAM
				static u32 __section(".data.unlikely") __n_warnings = 0;
				#else
				static u32 __section(.data.unlikely) __n_warnings = 0;
				#endif
				__n_warnings++;
				if (hweight32(__n_warnings) == 1) {										// Exponential backoff on binary tracing
					const struct nvmeibc_cmd_lock *l =  lock_of_bcomp(dc);
					const enum nvmeib_io_type_permission io_perm = so->o->topo->io_perm;
					NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: Bug in Toma? no UUID on seg @SEG_DBG_UUID for lock=@C_LID sync may stuck, io_perm=@IO_PERM, n_retries=@X!", _T, goodpath_nvmeibc_syncs, t_01_binfo_write, so->o->dbg_id, l->ds->dbg_uuid, holder.all, io_perm, __n_warnings);
					WARN((__n_warnings == 1), "Bug in Toma? no UUID for lock 0x%x, is_cold_recovery=%d sync may stuck!\n", holder.all, !nvmeibc_topo_is_partially_ok(so->o->topo));	// Only once, dmesg warning
				}
			} else { /* All good */}
		}
	}
}

static union nvmeib_lock_id __get_first_ow_val_that_so_had_to_take(struct recovery_sync_op *so)
{
	union nvmeib_lock_id rv =  {0};
	int i;
	for (i = 0; i < so->locks->nlocks; i++) {
		if (!did_caller_of_so_took_this_lock(&so->locks[i])) {	// Note: Dont care if lock was actually taken by 'so' or not
			rv = nvmeibc_d_rdma_comp_get_compare_lock_id(&(so->locks[i].comp));
			break;	// Usually locks[0]. In topology with dual lock / copy owner (instead of active in raid1), this can be lock[1]
		}
	}
	return rv;
}

#define does_so_cleans_stale_lock(so) \
	(((so)->n_slices == LOCKSET_SLICES)&&(so->error == 0))		// If not all slices fixed, this is partial/empty sync and unlock will be to stale values. Example: (NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE)

static union nvmeib_lock_id __calc_lock_non_clean_release_to_value_mirror(struct recovery_sync_op *so)
{
	const union nvmeib_lock_id rv = __get_first_ow_val_that_so_had_to_take(so);
	if (rv.bits.is_stale){
		return nvmeib_stale_special_raid1.lock_id;	// Convert from stale to stale special to boost future sync. This is cosher: If lock was taken --> it was alreay resolved by decentralized unreg. If could not be acquired then there is no meaning to release value as well
	}
	else {
		return rv;
	}
}

static void __prepare_locks_for_release_to(struct recovery_sync_op *so, u64 exchange)
{
	int i;
	for (i = 0; i < so->locks->n_siblings; i++)
		__prepare_lock_for_release(&so->locks[i], exchange);
}

static void __prepare_locks_for_release_to_prev(struct recovery_sync_op *so)
{
	int i;
	for (i = 0; i < so->locks->n_siblings; i++) {
		const union nvmeib_lock_id exchange = nvmeibc_d_rdma_comp_get_compare_lock_id(&so->locks[i].comp);
		WARN((exchange.all && !exchange.bits.is_stale), "nvmeibc di bug, unlock to 0x%u\n", exchange.all);
		__prepare_lock_for_release(&so->locks[i], exchange.all);
	}
}

static void __prepare_locks_for_release(struct recovery_sync_op *so)
{
	if (!does_so_cleans_stale_lock(so)) {
		// Partial R1 LOCKSET sync or NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE or failure

		// NOTE: Mirror and EC have conflicting behavior requirements here:
		// Mirror sync (either failed or commit stale) must commit stale (special)
		// to all locks if the owner was stale (EC-463).
		// EC sync must not write/copy stale lock to RAM that potentially contradicts
		// its TOMA's stale lock hash value (EC-3788), and cannot write stale special, so all locks
		// are just reverted to their previous values.
		if (!nvmeibc_raid_is_ec(so->r1)) { // Mirror
			const union nvmeib_lock_id exchange = __calc_lock_non_clean_release_to_value_mirror(so);

			// NOTE: 'exchange' can be 0 in the case this is a commit stale sync but the owner lock was not stale
			WARN(exchange.all && !exchange.bits.is_stale, "nvmeibc di bug, unlock to 0x%u\n", exchange.all);

			__prepare_locks_for_release_to(so, exchange.all);
		} else { // EC
			__prepare_locks_for_release_to_prev(so);
		}
	} else {
		// Sync's success and not commit stale sync, unlock to 0
		__prepare_locks_for_release_to(so, LS_UNLOCKED);
	}
}

/********* State machine which writes blockset info to all owner locks ********/
static void debug_transfer_transport_so(__attribute__ ((unused)) const struct recovery_sync_op *so)
{
	#if defined(DEBUG_TRANSFERS) && defined(DEBUG_TRANSFERS_DETECT_FAIL_TO_UNMAP)
		int i;
		for (i = 0; i < so->cmds->nraid_siblings; i++) {
			if (so->cmds[i].iocmd && so->cmds[i].iocmd->disk_cmd.in_flight) {
				_NE_dmesg(t_0n_dp_dbg_tools, "cmd %px iocmd %px - still in flight!", &so->cmds[i], so->cmds[i].iocmd);
				BUG();
			}
			#ifdef DEBUG_TRANSFERS_CHECK_NDB_MAPPED
			if (so->cmds[i].iocmd && so->cmds[i].iocmd->disk_cmd.cmd_type == NVMEIBC_DISK_CMD_IO && so->cmds[i].iocmd->reqs->ndb_mapped) {
				_NE_dmesg(t_0o_dp_dbg_tools, "cmd %px iocmd %px - still mapped!", &so->cmds[i], so->cmds[i].iocmd);
				BUG();
			}
			#endif
		}
	#endif
}

// Used to mark all blkset info writes. Much like __send_blkset_info_to_data_lock_cb()
static int __complete_bs_info_write(struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l =  lock_of_bcomp(dc);
	struct recovery_sync_op *so = l->cmds->o->rso;
	int i, rv = 0;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();

	(void)tag;
	if (NCL_had_acquire_callback(dc->lock_status))
		icore_ops->cb_called_comp(icore_ops, l->ds->disk, dc);
	if (!NCL_do_i_have_lock(dc->lock_status)) { // Handle errors
		so->error = -10026;						// Todo: Race condition here (3 locks attempt to update same integer), first wait for all locks to return, then test their result and set so->error
		_NTSO(t_00_binfo_write, "Failed: seg=@SEG_DBG_UUID lock=@LSI, err=@ERR, status=@STATUS", l->ds->dbg_uuid, l->lockset_idx, so->error, dc->lock_status);
	}
	rv = nvmeibc_atomic_dec_return(&so->cmds->n_uncompleted_cmds);
	if (rv > 0)
		return 0;
	WARN(rv < 0, "nvmeibc bug rv=%d\n", rv);
	for (i = 0; i < so->locks->n_siblings; ++i) {
		nvmeibc_cmd_lock_response_io_pet_describe(so->o, &so->locks[i]);
	}

	debug_transfer_transport_so(so);

	//atomic_inc(&get_so_fctr(so)->main.n_commit_binfo); // Todo: Daniel: do really add the counter here?
	nvmeibcbdpec_return_to_caller_sm(so);
	BLKCMP_SO_ASYNC_RESUME_CMP();	// Wakes up caller or continue to kernel continues. Much like completion of disk/gen commands
	return 0;
}

#define __restore_lock_status_after_rdma_op(l, dc)  ({ (dc)->lock_status = (l)->status; dc->opr = NVMEIBC_LOCK_CMP_AND_SWAP; }) // Restore original status of lock (need for unlock)
static void __cleanup_after_all_binfo_writes(struct recovery_sync_op *so)
{
	int i, nlocks = so->locks->n_siblings;
	for (i = 0; i < nlocks; i++) {			// Cleanup tmp data
		struct nvmeibc_cmd_lock *l = &so->locks[i];
		struct nvmeibc_d_rdma_comp *dc = &l->comp;
		__restore_lock_status_after_rdma_op(l, dc);
		set_callback_as_locks_state_machine(dc, __handle_locks_o); 	// Restore original locks cb
	}
	if (so->error == 0)
		mark_blockset_info_written(so);
}

bool dp_sync_does_see_clean_ram_dbits(const struct recovery_sync_op *so)
{
	const union nvmeibc_dbits_entry pre = {.all_bits = so->cmds->rld.pre.bits.dirty};
	return (pre.all_bits == zero_dbits.all_bits);
}

bool dp_sync_verify_binfo_is_legal(struct recovery_sync_op *so, const union nvmeib_blkset_info binfo);
void dp_sync_write_all_blocksets_info_op(struct recovery_sync_op *so) {
	struct nvmeibc_cmd_lock *ow_l = &so->locks[0];
	const bool expect_taken_lock = (NCL_do_i_have_lock(ow_l->status) || did_caller_of_so_took_this_lock(ow_l));
	__compressed_sync_op_trace_write_binfo(so);
	WARN(!expect_taken_lock || !should_blockset_info_commit(so), "nvmeibc bug, op=%p, calling commit binfo from wrong context: tkn=%d, cmmit=%d\n", so, expect_taken_lock, should_blockset_info_commit(so));
	// Trap, incorrect binfo!
	if (nvmeibc_raid_is_ec(so->r1)) {
		const union nvmeib_blkset_info binfo = so->cmds->rld.post;
		const bool is_origininal_sync_should_resolved_binfo = nvmeib_do_i_care_about_broken_binfo(so->o->op);
		const bool should_post_txid_be_correct = (so->o->op != NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON);			// This is the only sync which will not resolve TxID. It just does not care
		const bool should_post_dbit_be_correct = ((so->o->op != NVMEIB_BLOCK_IO_OP_REC_COLD) && !is_so_nested(so));	// EC has multiple stages of dbit manipulation, including turning on dbits, only last step guranteed to be without DBITs on 'W'
		if (is_origininal_sync_should_resolved_binfo) {		// Verify the precondition to launching this state machine. rldr.pre, not post!
			WARN_ON(nvmeibcbdp_binfo_has_unknown_dbits(so->cmds, so->r1));		// Was already resolved, and cannot appear, test unknowns in pre-binfo
			if ((so->o->op == NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO) && (!is_so_nested(so)) && (dp_ec_can_fix_dbits(so->cmds))) {
				WARN(true, "Data corruption: so=" PRI_SO_NAME ", o=%p, binfo=0x%x copied instead of turning dbits off. n_slices=%u\n", PRI_SO_NAME_ARGS(so), &so->o, binfo.all, so->n_slices);
				nvmeibcb_dp_io_fail_mgr_binfo_err(&so->o->nd->dp.io_stats.mgr);
			}
		}
		if (should_post_dbit_be_correct) {
			if (!verify_binfo_is_legal(so->locks->ds, binfo, so->locks->address, 's')) {
				WARN(true, "Data corruption: so=" PRI_SO_NAME ", o=%p, binfo=0x%x committing wrong dbits! n_slices=%u\n", PRI_SO_NAME_ARGS(so), &so->o, binfo.all, so->n_slices);
				nvmeibcb_dp_io_fail_mgr_binfo_err(&so->o->nd->dp.io_stats.mgr);
			}
		}
		if (should_post_txid_be_correct) {
			WARN(binfo.bits.txid == INITIAL_LAZY_READ_TXID, "Data corruption: so=" PRI_SO_NAME ", o=%p, binfo=0x%x committing unknown TxID! n_slices=%u\n", PRI_SO_NAME_ARGS(so), &so->o, binfo.all, so->n_slices);
		}
	} else if (so->cmds->rld.post.bits.dirty != 0) {		// R1, verify dirty bits
		const union nvmeib_blkset_info binfo = so->cmds->rld.post;
		const union nvmeibc_dbits_entry dbits = { .all_bits = binfo.bits.dirty };
		const u32 num_unknown = nvmeibc_dbits_get_n_unk(&dbits, &so->r1->calculated_data.topo_traits);	// test unknowns in post-binfo
		const bool special_2mirror_case = (so->r1->replicas <= 2);
		const bool no_dbits_for_w_segs = (special_2mirror_case || (so->n_slices == LOCKSET_SLICES));
		const char ver_action = (so->o->op == NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON) ? 's' :		// Dont check W-
								no_dbits_for_w_segs ? 'w' :											// W- and W must not include any dbits
								'r';																// 3+ Mirror, W seg in partial syncs can have dbits
		if (!verify_binfo_is_legal(so->locks->ds, binfo, so->locks->address, ver_action)) {
			WARN(true, "Data corruption: so=" PRI_SO_NAME ", o=%p, binfo=0x%x committing wrong dbits! n_slices=%u\n", PRI_SO_NAME_ARGS(so), &so->o, binfo.all, so->n_slices);
			nvmeibcb_dp_io_fail_mgr_binfo_err(&so->o->nd->dp.io_stats.mgr);
		}
		if ((num_unknown != 0) && no_dbits_for_w_segs) {		// R1 uses still preserves unknown dbits to prefer reads from W seg over writes + in future we might used metadata dbits so unknowns can be resolved
			WARN(true, "Data corruption: so=" PRI_SO_NAME ", o=%p, binfo=0x%x committing unknown dbits! n_slices=%u\n", PRI_SO_NAME_ARGS(so), &so->o, binfo.all, so->n_slices);
			nvmeibcb_dp_io_fail_mgr_binfo_err(&so->o->nd->dp.io_stats.mgr);
		}
		if (special_2mirror_case) {
			// R1 2-mirror, special case where writing/turning-on dbits is allowed only by 2 syncs:
			// 	Stale2dirty in {RW,D} - not using this function
			// 	Dirty_convict_turn_on in {RW, W-}
			// 	Note: in {RW, W}, This is a plain bug, must fix dirtybit.
			// 	When more than 2 replicas: commit_stale_lock and other syncs can write dbits. Especially in {RW,W,D} topo
			WARN(so->o->op != NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON, "Data corruption: so=" PRI_SO_NAME ", o=%p, binfo=0x%x committing dbits!\n", PRI_SO_NAME_ARGS(so), &so->o, binfo.all);
		}
	}
	so->o->op = NVMEIB_BLOCK_IO_OP_MAINTAIN_COMMIT_BINFO;
	nvmeibcbdpec_push_clean_to_stack(so, __cleanup_after_all_binfo_writes);
	nvmeibc_sync_set_uncompleted_cmds(so, ow_l->n_siblings);		// Cleaner to use so->locks->n_uncompleted_locks
	dp_sync_verify_binfo_is_legal(so, so->cmds->rld.post);
	BLKCMP_SO_ASYNC_AWAIT(dp_locks_write_all_blocksets_info_op(ow_l, &so->cmds->rld.post, &__complete_bs_info_write, 0));
	/* Watch out from here so might be freed. */
}

/**************** Mapping operations to sync functions ************************/
void* nvmeibcbdpec_sync_get_fn_by_rtype(enum NVMEIBT_RECOVERY_TYPE rtype)
{
	switch (rtype) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:      return nvmeibc_sync_recover_dirty;
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:   return nvmeibc_sync_unknown_binfo_recov;
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:          return nvmeibc_sync_scrubbing;
	case NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON: return nvmeibc_sync_turn_on_dirty_convict;
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:      return nvmeibc_sync_fix_stale;
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:            return nvmeibc_sync_fix_cold;
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:         return nvmeibc_sync_gc_unlocked_lock;
	default:                                       return NULL; /* Dummy recovery */
	}
}

/***************** State machine which Takes/Releases all locks ***************/
static int __handle_locks_o(struct nvmeibc_d_rdma_comp *lock_comp, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l =  lock_of_bcomp(lock_comp);		// Any lock from all siblings
	struct recovery_sync_op *so = l->cmds->o->rso;
	int err = 0;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
_func_start:

	(void)tag;
	__ndump_operation(handle_locks, &so->o);
	if (so->error != 0) {
		if (so->should_abandon_on_error && (so->stage < sync_stage_done)) {
			_NTSO(trace_should_abandon_on_error, "Post sync error stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
			so->stage = sync_stage_recov_rereg_on_error;
		} else if (so->stage < sync_stage_recov_owner_ulock_sm) {
			/* Any error during taking of locks or IO fails to lock release*/
			_NTSO(trace_dp_sync_common_handle_locks_o, "stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
			so->stage = sync_stage_recov_owner_ulock_sm;
		} else {
			/* Error during release is not treated */
		}
	}
	_ND(trace_1_dp_sync_common_handle_locks_o, "-->op=@BLOCK_IO_OP, @DLBA, stage=@SYNC_STAGE err=@ERR", so->o->op, l->address, so->stage, so->error);
	switch (so->stage) {
	/*************** Acquire all needed locks (i, i+1, i+2..)*****************/
	case sync_stage_start:
		nvmeibc_profiling_take_stats_for_next_stage(so->r1->hdr->sync_profile, so, NVMEIBC_PROFILING_SYNC_STAGE_SYNC_SUBMIT_UP_TO_SYNC_START, so->error);
		if (!so->assume_caller_holds_locks) {
			so->stage = sync_stage_recov_lo_try_lock;
		} else {
			so->stage = sync_stage_recov_lo_all_taken;
		}
		goto _func_start;

	case sync_stage_recov_lo_try_lock:{
		if (l->status == NCL_STATUS_DONE) {			// If sync should not take it (IO already holds it or other reason)
			so->stage = sync_stage_recov_lo_next;
			goto _func_start;
		}
		so->stage = sync_stage_recov_lo_try_lock_cb;
		lock_comp->opr = NVMEIBC_LOCK_CMP_AND_SWAP;
		nvmeibc_cmd_lock_request_io_pet_describe(so->o, l);
		err = BLKCMP_SO_ASYNC_AWAIT_RV(icore_ops->run_cmpxchg(icore_ops, l->ds->disk, handle_of(l->ds), l->address, lock_comp));
		if (!err)
			BLKCMP_SO_ASYNC_RESUME_CUR(0);
		so->error = -10010;	  // Abort, could not take owner lock
		goto _func_start;
	}

	case sync_stage_recov_lo_try_lock_cb:{
		l->status = lock_comp->lock_status;
		icore_ops->cb_called_comp(icore_ops, l->ds->disk, lock_comp);
		nvmeibc_cmd_lock_response_io_pet_describe(so->o, l);
		dp_locks_trace_lock_comp(so->o, l, lock_comp);
		__invoke_crash_on_lock_corruption(l, 0, "take", 1);
		if (NCL_do_i_have_lock(lock_comp->lock_status)) {
			so->stage = sync_stage_recov_lo_next;
			if (nvmeibc_raid_is_ec(so->r1))
				__fill_recoveree_uuid(lock_comp, so);
		} else {
			__retry_aquire_lock(lock_comp, l, so);
		}
		goto _func_start;
	}

	case sync_stage_recov_lo_next:{
		/* This lock is taken, check if any others are needed to be taken:*/
		const int next_lsi = l->lockset_idx+1;
		if (so->locks->n_siblings == next_lsi) { // I was the last lock
			so->stage = sync_stage_recov_lo_all_taken;
		} else {
			__set_lock_ptr_to(&so->locks[next_lsi]);
			so->stage = sync_stage_recov_lo_try_lock;
		}
		goto _func_start;
	}

	case sync_stage_recov_lo_all_taken: {
		nvmeibc_profiling_take_stats_for_next_stage(so->r1->hdr->sync_profile, so, NVMEIBC_PROFILING_SYNC_STAGE_SYNC_START_UP_TO_ALL_LOCKS_TAKEN, so->error);
		ASYNC_AWAIT_AND_RESUME(so->o->nd->dp.sync_execute_op(so), 0);
	}

	/*************** Blockset Recovered *****************/
	/* Stale lock sync (HTR) sends blockset recovered for each
	   TOMA/Lock to ensure Serjio/TOMA can reset journal/stale lock history
	   Should be triggered outside of HTR when only a lock copy is stale
	   */
	case sync_stage_recov_send_recovered: {
		if (so->should_send_msg_blckst_recovrd) { /* 3 cases:
			1. Owner is stale -> send blkset recovered only if we actually
				fixed the problem.
			2. Owner OK, Copy is stale -> no problem with data, just send
				blkset recovered to clean the abandoned journal.
			3. Autonomous-Sync: Should never happen! We dont hold locks so must not send any message!
			Todo: differentiate between cases for clearance of condition below */
			WARN(so->error != 0, "nvmeibc bug. Incorrect flow\n");	// We actually did not fix the blockset
			if (so->is_autonomous) {						// Case 3:
				WARN(1, "nvmeibc bug. Incorrect flow\n");	// BUG, what the hell???
				so->should_send_msg_blckst_recovrd = false;	// Must not or cant send msg
			}
			if ((!does_so_cleans_stale_lock(so))||			// Assumes worst: Case 1, if true, must not send msg or else data corruption (Did not clean lock)
				is_op_sync_commandless(so->o->op) || is_op_sync_commit_binfo(so->o->op))			// Case 2: should send msg, but dont have cmds to send msg. Will leak journal, Todo: Solve in future
				so->should_send_msg_blckst_recovrd = false;	// Must not or cant send msg
		}
		if (so->should_send_msg_blckst_recovrd) {
			so->stage = sync_stage_recov_send_recovered_thread;
			ASYNC_AWAIT_AND_RESUME(nvmeibcbdp_sync_reschedule(so), 0);
		} else {
			ASYNC_AWAIT_AND_RESUME(__return_to_locks_unlock_sm(so), 0); // Just continue to unlock state machine
		}
	}

	case sync_stage_recov_send_recovered_thread: {
		extern void dp_ec_mainten_blkset_recov_cb_stg_end(struct recovery_sync_op *so);
		nvmeibcbdpec_push_sm_to_stack(so, nvmeibcbdpec_return_to_caller_sm);
		so->o->op = NVMEIB_BLOCK_IO_OP_MAINTAIN_BLKSET_RECOV;
		ASYNC_AWAIT_AND_RESUME(dp_ec_mainten_blkset_recov_cb_stg_end(so), 0);
	}

	/*************** Dbit/binfo clean up ofter blockset fixup *****************/
	case sync_stage_recov_write_binfo: { // write binfo directly to locks to ensure we update fixed DBs/etc
		so->stage = sync_stage_recov_owner_ulock_sm;				// Sub-State machine which writes blockset info
		WARN_ON(so->error != 0);
		nvmeibcbdpec_push_sm_to_stack(so, nvmeibcbdpec_return_to_caller_sm);
		ASYNC_AWAIT_AND_RESUME(dp_sync_write_all_blocksets_info_op(so), 0);
	}

	/************** Unlocking state machine (in reverse order) ****************/
	case sync_stage_recov_owner_ulock_sm:{
		/* Locks acquisition failed or commands state machine terminated */
		nvmeibc_profiling_take_stats_for_next_stage(so->r1->hdr->sync_profile, so, NVMEIBC_PROFILING_SYNC_STAGE_LOCKS_TAKEN_UP_TO_UNLOCK_SM, so->error);
		if ((so->error == 0) && (so->orig_rldr->o->op == NVMEIB_BLOCK_IO_OP_DPLIB_PROBLEM))
			nvmeibcbdpec_inject_binfo_back_to_caller(so);
		if (unlikely(so->assume_caller_holds_locks)) {
			so->stage = sync_stage_done;
			goto _func_start;
		}

		__prepare_locks_for_release(so);
		__set_lock_ptr_to(&so->locks[so->locks->n_siblings-1]);		// Go from end to start, to release primary owner last
		so->stage = sync_stage_recov_un_lock_release;
		goto _func_start;
	}

	case sync_stage_recov_un_lock_release: {
		if (!so->error) /* This is a point of no return, if we fail here we have to reregister to recover ec-4271 */
			so->should_abandon_on_error = true;
		so->stage = sync_stage_recov_un_lock_release_cb;
		if (NCL_do_i_have_lock(l->status)) {            // If sync took it, release it
			if ((err = BLKCMP_SO_ASYNC_AWAIT_RV(__release_lock_of_sync(l, so))) == 0)
				BLKCMP_SO_ASYNC_RESUME_CUR(0);
		} else {
			so->stage = sync_stage_recov_un_lock_next_lock;
		}
		goto _func_start;
	}

	case sync_stage_recov_un_lock_release_cb: {
		l->status = lock_comp->lock_status;
		if (!did_caller_of_so_took_this_lock(l)) { /* Owner/Dual/Copy-Ow handled by sync, released or broken */
			nvmeibc_cmd_lock_response_io_pet_describe(so->o, l);
			dp_locks_release_cb(lock_comp, nvmeibc_d_rdma_comp_tag_make());
		}
		so->stage = sync_stage_recov_un_lock_next_lock;
		goto _func_start;
	}

	case sync_stage_recov_un_lock_next_lock: {
		const int prev_lsi = l->lockset_idx-1;
		if (l->lockset_idx == 0) { 	// All locks handled, last to first
			so->stage = sync_stage_done;  /* All locks release or broken*/
			goto _func_start;
		}
		__set_lock_ptr_to(&so->locks[prev_lsi]);
		so->stage = sync_stage_recov_un_lock_release;
		goto _func_start;
	}

	case sync_stage_recov_rereg_on_error: {
		nvmeibc_topologies_rereg_seg(so->locks[l->owner_idx].ds);
		BUG_ON(so->error == 0);			// Why then are we doing rereg?
		so->stage = sync_stage_done;
		goto _func_start;
	}

	case sync_stage_done:{ // Free memory
		nvmeibc_profiling_end_take_stats_for_stage(so->r1->hdr->sync_profile, so, NVMEIBC_PROFILING_SYNC_STAGE_UNLOCK_SM_TP_TO_SYNC_FREE, so->error);
		return __do_on_stage_done(so);
	}
	default:;
	} // switch (->stage)
	BUG();	// Illegal state
	return 0;
}

/* Only copying the needed information from owner_lock of original IO, used when
   Sync does not intend to take locks (either all locks are taken or sync is
   stale-to-dirty / journal-garbage-collection) */
static void __copy_only_owner_lock(struct nvmeibc_cmd_lock *l,
						const struct nvmeibc_cmd_lock *lock)
{
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	BUG_ON(dp_locks_get_blockset_owner_lock(lock) != lock);	// Sanity: Verify we are copying owner lock
	l->address = lock->address;
	l->ds =      lock->ds;
	nvmeibc_b_rdma_comp_init(&l->comp, 0, l);	// Lockset of 1 lock: owner_id = comp->lock_i = 0
	__change_lock_status_to(l, NCL_STATUS_INVALID);
	nvmeibc_copy_blockset_info(l, lock);
	l->n_siblings = l->nlocks = 1;
	l->comp.code = NVMEIBC_CMD_LOCK_OWNER;	// Daniel: Not sure this line is needed
	set_callback_as_locks_state_machine(dc, __handle_locks_o);
	dc->cpu_mask_info = lock->comp.cpu_mask_info;
}

/* Sync copies the state of locks from IO to append the needed ones, It servese
   as IO's dp_fill_locks_for_raid() + dp_locks_send_all() */
static void __copy_all_locks(struct recovery_sync_op *so, const struct nvmeibc_cmd_lock *lock)
{
	const struct nvmeibc_cmd_lock *ow = dp_locks_get_blockset_owner_lock(lock);			// In rare case lock might be the dual owner.
	const union nvmeib_lock_id holder = nvmeibc_d_rdma_comp_get_contending_id(&lock->comp);	// Stale lock we are trying to solve
	struct nvmeibc_cmd_lock *l = so->locks;
	int i, n_missing_olocks = 0;
	if (is_op_sync_stale(so->o->op))
		WARN_ON(!holder.bits.is_stale);				// Sanity check. How can caller complain on non stale lock?
	so->locks->nlocks = dp_fill_locks_for_raid(so->r1, so->o->op, so->rlba, so->locks);
	BUG_ON(ow->n_siblings > so->locks->n_siblings);					// Debug only: Can be less (view read lock only) or equal
	for (i = 0; i < so->locks->n_siblings; i++, l++) {				// Copy the locks. Sync has to fill only the locks IO could not take
		struct nvmeibc_d_rdma_comp *dc = &l->comp;
		if ((l->type == NVMEIBC_CMD_LOCK_OWNER)||
			(l->type == NVMEIBC_CMD_LOCK_COPY_OWNER)) {
			if ((i < ow->n_siblings)&&(NCL_do_i_have_lock(ow[i].status))) {
				BUG_ON(ow[i].ds != l->ds);
				nvmeibc_copy_blockset_info(l, &ow[i]);
				__change_lock_status_to(l, NCL_STATUS_DONE);		// IO already holds this lock. Daniel: deliberatly not using NCL_STATUS_TRANSFERRED, but DONE. To mark the sync should not do anything with this lock
			} else {
				__change_lock_status_to(l, NCL_STATUS_NOTISSUED);	// Sync has to take this lock
				dc->compare  = holder.all;
				dc->exchange = get_lockid_for_cmpxchg(so->r1, so->o->op);
				n_missing_olocks++;
			}
			l->comp.code = NVMEIBC_CMD_LOCK_OWNER;
			l->comp.cpu_mask_info = ow->comp.cpu_mask_info;
		} else { WARN(true, "nvmeibc bug!\n"); }
		set_callback_as_locks_state_machine(dc, __handle_locks_o);
	} // Note: (ow->ds != lock->ds): Is when IO holds owner lock but secondary/copy was stale
	nvmeibc_atomic_set(&so->locks->n_uncompleted_locks, LARGE_SYNC_DEBUG_VALUE + n_missing_olocks);	// Active has to be unlocked, owner has to be taken and unlocked.
	so->assume_caller_holds_locks = (n_missing_olocks == 0);
}

static inline void nvmeibc_no_write_hole_params_init(struct recovery_sync_op *so) {
	const enum nvmeib_block_io_op op = so->o->op;
	union no_writehole_params *params = &so->nwhole_params;
	*params = no_writehole_params_default;
	if (is_op_sync_readfail(op)) {
		params->must_fix_bad_sectors = true;
	} else if (is_op_sync_db(op) || is_op_sync_stale(op)) {
		params->must_fix_bad_sectors = true;
	} else if (is_op_sync_rollback(op) || is_op_sync_cold(op)) { // Cold can only call no_whole for rollback reasons.
		params->must_fix_bad_sectors = false;
		params->must_turn_off_dbits = false;
	} else if (is_op_sync_scrubbing(op)) {
		params->must_fix_bad_sectors = true;
		params->must_scrub = true;
	} else {
		*params = no_writehole_params_empty;
	}
}

static int __init_so(enum nvmeib_block_io_op op, struct recovery_sync_op *so,
					  u16 start_block, u16 n_slices,
   struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t done_cb, void* context)
{
	struct nvmeibc_cmd_lock *locksets = dp_locks_get_locks_header(lock);
	const struct nvmeibc_block_command *cmds = locksets->cmds;
	const struct operation *orig_o = cmds->o;
	int rv = 0;
	struct operation *o;
	if (!(so->o = nvmeibc_operation_create_atomic(orig_o))) {
		_NW(trace_3_dp_sync_common_init_so, DMESG_PREFIX("@DEV_NAME") ": No memory for sync's operation", orig_o->nd->name);
		rv = -ENOMEM; 		/* Critical failure */
		goto _out;
	}
	o = so->o;
	o->rso = so;
	o->journal = nvmeibc_io_pet_journal_make(orig_o->nd->io_pet_controller);
	o->jiffies1 = jiffies; // Just for debug
	BLKCMP_SO_BLOCKING_CONTEXT_ALLOC(o);
	o->nd = orig_o->nd; // Just for debug
	o->topo = nvmeibc_topology_addref(orig_o->topo);
	o->cpu_mask_info = orig_o->cpu_mask_info;
	o->cpu_id = orig_o->cpu_id;
	DEBUG_TOPO_CNTRS_op_constructor(o, NULL, NULL);
	DEBUG_TOPO_CNTRS_add_elem_to_topo(o, o->topo);
	so->o->op = op;
	so->start_slice = start_block;
	so->r1 = nvmeibc_disk_segment_get_praid(lock->ds);
	so->n_slices = n_slices;
	so->orig_rldr = nvmeibc_cllink_find_cmd_by_lock(locksets, lock->lockset_idx);
	BUG_ON(!so->orig_rldr);	// Sanity. Caller (IO / Recovery / DpLib) Must have at least 1 disk command
	BUG_ON((start_block + n_slices) > LOCKSET_SLICES);		// Sanity, invalid blocksets request
	so->stage = sync_stage_start;
	so->when_done_cb =     done_cb;
	so->when_done_context = context;
	so->rlba =  (__lock_start(*lock) - lock->ds->first_lba) * so->r1->slice_size;	// simplified version of dp_io_topo_iterator_conv_seg_lock_addr_to_raid_ofst()
	if (0) {				// Debugging sync operations
		const struct nvmeibc_subscription_ctx *tr = lock->ds->toma_reg;
		_NDSO(trace_5_dp_sync_common_init_so, " init o=@ORIG_O, op=@BLOCK_IO_OP @TOPOLOGY l_seg=" SEGMENT_FMT " l=@LOCKSETS, cmds=@CMDS, so=@SO", orig_o, orig_o->op, orig_o->topo, tr->ch, tr->r1, tr->seg, locksets, cmds, so);
	}

	nvmeibc_no_write_hole_params_init(so);
	//so->fctr = so->o->nd->dp.sync.fcntr;	// Caching the pointer
	__ndump_operation(init_so, o);
_out:
	return rv;
}

#define should_abort_sync_on_detach(l) nvmeibc_block_status_is_detaching(dp_locks_get_locks_header(l)->cmds->o->nd->status) // can also use: nvmeibc_disk_seg_to_bdev(l->ds)
static int __trigger_sync(struct nvmeibc_cmd_lock *lock, u16 start_block, u16 n_slices,
	enum nvmeib_block_io_op op, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	struct recovery_sync_op *so = NULL;
	int rv = 0;
	if (unlikely(should_abort_sync_on_detach(lock)))
		return -10031;
	if (!(so = __alloc_so())) {
		rv = -ENOMEM;
		goto error;
	}

	if ((rv = __init_so(op, so, start_block, n_slices, lock, when_done_cb, context))) {
		goto error;
	}
	nvmeibc_profiling_start_take_stats(so->r1->hdr->sync_profile, so);
	if (nvmeibc_raid_is_jbod(so->r1))
		goto __checks_done;

	{ /* Anaylize amount of Non readable segs */
		const int max_unavail_srcs = nvmeibc_raid1_get_protect_lvl( so->r1);
		const int cur_deadseg_srcs = nvmeibc_raid1_get_num_dead_seg(so->r1);
		const int num_nonread_srcs = nvmeibc_praid_get_num_deg_segs(so->r1);
		const int cur_unavail_srcs = nvmeibc_raid_is_ec(so->r1) ? num_nonread_srcs : (int)nvmeibc_raid1_count_inverse_bmp(so->r1, readable_sync); // EC does not use 'W' for read, R1 cant use it if dbit exists
		if (cur_unavail_srcs > max_unavail_srcs) {	// Test if there are too many preblematic segs, wrong topology
			WARN(true, DMESG_PREFIX("%s: ") "nvmeibc bug. How? op=%d, %d > %d \n", so->o->nd->name, op, cur_unavail_srcs, max_unavail_srcs);
			rv = -10001; /* Not supported, cannot read */
		} else if (is_op_sync_scrubbing(op)) {	// Must be perfect topo
			if (num_nonread_srcs != 0) {
				WARN_ONCE(true, DMESG_PREFIX("%s: ") "nvmeibc bug. scrubbing can be launched only in non degraded mode\n", so->o->nd->name);
				rv = -10004; /* Not supported, cannot read */
			}
		} else if (cur_deadseg_srcs == max_unavail_srcs) {	// Slice has no protection at all
			if (is_op_sync_stale(op) || is_op_sync_cold(op) || is_op_sync_commandless(op) || is_op_sync_commit_binfo(op) || is_op_sync_hot_jgc(op) || is_op_sync_txid_wrap(op)) {
				if (__is_raid1_mirror(so)) {
					WARN(true, DMESG_PREFIX("%s: ") "Serious problem. op(%d) must stale2dbits(%d)\n", so->o->nd->name, op, cur_deadseg_srcs);
					rv = -10002;
				} else { // In EC: If 'max_num_dead_segs_reached' stale lock recovery will just clean journals, Else W- topology will be fixed
				}
			} else if (is_op_sync_readfail(op)) {	// Goto destroy slice
				_ND(t0dpscts, "Skip meaningless readfail sync. Will busyloop");
				rv = -10002;
			} else if (op == NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO) {
				// Note: Unknown binfo can appear in any topology, reproduction of NVMESH-2628, in this flow
			} else {
				WARN(true, DMESG_PREFIX("%s: ") "nvmeibc bug. op=%d not supported\n", so->o->nd->name, op);
				rv = -10003; /* Not supported, cannot read */
			}
		}
	}
	if (rv == 0) { /* Analize Correct locks of caller */
		if (lock->ds->toma_acm != NVMEIBTC_DS_MODE_RW) {	// Primary owner lock is taken, secondary/copy lock is contended
			struct nvmeibc_cmd_lock *ow = dp_locks_get_blockset_owner_lock(lock);
			if (!NCL_do_i_have_lock(ow->status)) {
				WARN(true, DMESG_PREFIX("%s: ") "nvmeibc bug. No reason for sync, op=%u\n", so->o->nd->name, op);
				rv = -10005;
			}
		}
	}
	if (unlikely(rv))
		goto error;

__checks_done:
	if (is_op_sync_cold(op))
		so->o->user_data = lock->user_data;		// Add reference to payload of cold recovery (sync will not free it)
	if (unlikely(rv))
		goto error;

	__copy_all_locks(so, lock);
	nvmeibc_profiling_start_take_stats_for_stage(so->r1->hdr->sync_profile, so, NVMEIBC_PROFILING_SYNC_STAGE_SYNC_SUBMIT_UP_TO_SYNC_START);
	__syncs_list_op('+', so);
	so = NULL;									// Dont touch 'so' it might be freed
	return 0;
error:
	if (so)
		so->when_done_cb = NULL; // Error returned without callback
	__free_sync_op(so, false /* slot was not reserved yet */);
	return rv;
}

// EC-1477: Hide this entire state machine in _mirror_ files. It should not be in common!
static int __convert_stale_special_2_dirty_o(struct nvmeibc_d_rdma_comp *lock_comp, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l =  lock_of_bcomp(lock_comp); // Always first lock
	struct recovery_sync_op *so = (void*)l->cmds;
	int err = 0;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();

	(void)tag;
	__ndump_operation(t_00_ss2dbit, &so->o);

_func_start:
	if (so->error != 0) {
		_NTSO(t_01_ss2dbit, "ss2dbit stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
		so->stage = sync_stage_done;
	}
	switch (so->stage) {
		case sync_stage_start:{
			_NDSO(t_02_ss2dbit, "ss2dbit stage=sync_stage_start, Holder lock id: @LOCKID", nvmeibc_d_rdma_comp_get_contending_id(lock_comp).all);
			nvmeibc_atomic_set(&l->n_uncompleted_locks, 1 + LARGE_SYNC_DEBUG_VALUE);
			l->comp.code = NVMEIBC_CMD_LOCK_UNLOCK;						// Important, we are going to unlock it
			lock_comp->compare = nvmeibc_d_rdma_comp_get_lock_id(lock_comp).all;			//Use the original lock id as the "locking" value (This may be R1_STALE_SPECIAL_BINFO_VAL or a specific lock value with stale bits)
			lock_comp->exchange = LS_UNLOCKED;
			so->stage = sync_stage_st_to_db_written_db;
			{
				extern void __mirror_sync_calc_post_binfo(struct recovery_sync_op*, struct nvmeibc_raid_leader_cmd_ctx *, bool has_stale_lock);		// EC-1477: Remove after solving this issue
				u8 const sgmnt = numeric_downcast(u8, dp_locks_get_sgmnt_idx_of_lock(l));
				struct nvmeibc_raid_leader_cmd_ctx rld;
				memset(&rld, 0, sizeof(rld));
				rld.pre.all = lock_comp->lock.bi;						// Check binfo for unknown
				__mirror_sync_calc_post_binfo(so, &rld, true);			// Here we will commit binfo much like should_blockset_info_commit() but with state machine to only primary owner lock
				lock_comp->lock.bi  = (u32)rld.post.all;
				nvmeibc_blkset_info_write_pet_describe(so->o, sgmnt, l->address, lock_comp);
				// Multiple writes of dirty bits to bi are fine, unlocking will still only be done once
				err = BLKCMP_SO_ASYNC_AWAIT_RV(icore_ops->write_blkset_info(icore_ops, l->ds->disk, handle_of(l->ds), l->address, lock_comp));
			}
			if (!err)
				BLKCMP_SO_ASYNC_RESUME_CUR(0);

			_NT(t_ss2dbit_error_1, "ss2dbit error 1 - Couldn't write dbit to binfo, stage=sync_stage_start, err=@ERR", err);
			so->error = err;	  // Abort, could not write dirty bit
			goto _func_start;
		}

		case sync_stage_st_to_db_written_db:{
			icore_ops->cb_called_comp(icore_ops, l->ds->disk, lock_comp);
			nvmeibc_cmd_lock_response_io_pet_describe(so->o, l);
			__invoke_crash_on_lock_corruption(l, 0, "take", 1);
			so->stage = sync_stage_st_to_db_stale_released;
			if (!NCL_do_i_have_lock(lock_comp->lock_status)) {
				so->error = -10007;	/* Could not write dirty bit. aborting */
				_NTSO(t_ss2dbit_error_2, "ss2dbit error 2: seg=@SEG_DBG_UUID err=@ERR, status=@STATUS",
						l->ds->dbg_uuid, so->error, lock_comp->lock_status);
				goto _func_start;
			}
			__change_lock_status_to(l, NCL_STATUS_INVALID);
			dp_locks_trace_lock_release(so->o, l);
			lock_comp->opr = NVMEIBC_LOCK_CMP_AND_SWAP;
			nvmeibc_cmd_lock_request_io_pet_describe(so->o, l);
			err = BLKCMP_SO_ASYNC_AWAIT_RV(icore_ops->run_cmpxchg(icore_ops, l->ds->disk, handle_of(l->ds), l->address, lock_comp));
			if (!err)
				BLKCMP_SO_ASYNC_RESUME_CUR(0);
			_NT(t_ss2dbit_error_3, "ss2dbit error 3 - cmpxchg failed, stage=sync_stage_st_to_db_written_db, err=@ERR", err);
			lock_comp->lock_status = NCL_STATUS_FAIL_NO_COMP;	// Wrote db, could not unlock the lock.
			goto _func_start;
		}

		case sync_stage_st_to_db_stale_released:{
			nvmeibc_cmd_lock_response_io_pet_describe(so->o, l);
			if (NCL_had_release_callback(lock_comp->lock_status))   // Do this only if actuall callback returned, or else changing to TAKEN implies a callback and we would corrupt the count of in_transfers io requests
				lock_comp->lock_status = NCL_STATUS_TAKEN;			// Daniel: Even if stale-special release failed, sync operation is a success, coz dirty bit was written (no data corruption). dp_locks_release_cb() - does reregisters failed release. We don't want this!
			dp_locks_release_cb(lock_comp, nvmeibc_d_rdma_comp_tag_make());
			so->stage = sync_stage_done;
			goto _func_start;
		}

		case sync_stage_done:{ // Free memory
			return __do_on_stage_done(so);
		}
	default:;
	} // switch (->stage)
	BUG();	// Illegal state
	return 0;
}

/* Construct for syncs that run without holding locks */
static struct recovery_sync_op *__create_autonomous_sync(enum nvmeib_block_io_op op, struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t done_cb, void* ctx)
{
	struct recovery_sync_op *so = __alloc_so();
	if (so) {
		struct nvmeibc_cmd_lock *l = &so->locks[0];
		if (unlikely(__init_so(op, so, 0, LOCKSET_SLICES, lock, done_cb, ctx))) {
			nvmeibc_operation_free(so->o);
			__free_so(so);
			return NULL;
		}
		so->is_autonomous = true;
		nvmeibc_operation_alloc_dbg_id(so->o);
		__copy_only_owner_lock(l, lock);
		BUG_ON(NCL_do_i_have_lock(lock->status));		// Lock must be unlocked
		_NTSO(t_1_sync_autonomous, "seg:@SEG_DBG_UUID, @DLBA_BLKSETS", l->ds->dbg_uuid, (l->address/LOCKSET_SLICES));
		__compressed_sync_op_trace_start(so);
		l->cmds = (void*)so;	//TODO XXX Daniel: Hack, lock cannot point to operation without commands, can allocate dummy command
	}
	return so;
}

static int __convert_stale_special_2_dirty_bit(struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t done_cb, void* ctx)
{
	if (unlikely(should_abort_sync_on_detach(lock)))
		return -10031;
	else {
		struct recovery_sync_op *so = __create_autonomous_sync(NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB, lock, done_cb, ctx);
		if (so) {
			struct nvmeibc_d_rdma_comp *dc = &so->locks[0].comp;
			dc->lock.id = lock->comp.lock.id;	// Propegate the original holder lock id for handling later
			set_callback_as_locks_state_machine(dc ,&__convert_stale_special_2_dirty_o);
			__convert_stale_special_2_dirty_o(dc, nvmeibc_d_rdma_comp_tag_make());
			return 0;
		} else
			return -ENOMEM;
	}
}

/******************************* External API *********************************/

static inline bool __can_r1_convert_stale_lock_2_diry_bit(const struct nvmeibc_raid1* pr)
{
	return (!nvmeibc_raid_is_ec(pr) && (nvmeibc_raid1_get_num_dead_seg(pr) == (pr->replicas-1)));
}

bool nvmeibc_sync_is_io_implicit_sync(const struct nvmeibc_cmd_lock *l,
									 const struct nvmeibc_block_command *c)
{
	const struct nvmeibc_raid1* pr = nvmeibc_disk_segment_get_praid(l->ds);
	if (!nvmeibc_raid_is_ec(pr)) {
		if (unlikely(__can_r1_convert_stale_lock_2_diry_bit(pr)))
			return true;	/* In degraded mode of raid-1, 2 mirror, sync stale-to-dirty is legal */
		return ((c->nlbas == LOCKSET_SLICES) && (nvmeib_block_io_op_is_write(c->o->op)));
		/* Daniel todo: Trim is not supported because even if command's length is
		   1 lock or longer - there is no guarantee that entire lock is covered.
		   Maybe trim has offset. To do, analyze the scatter gather first entry */
	}
	return false; // On EC even IO is never an implicit sync as need to send blockset recovered to TOMA and free journals of recoveree.
}

void nvmeibc_datapath_syncs_zero_stats(struct nvmeibc_datapath_syncs_resources *sr)
{
	struct nvmeibc_sync_stats *ss = &sr->stats;
	ss->num_total = ss->num_full_blockset_ok =
		ss->num_part_blockset_ok = ss->num_total_maintainance =
		ss->num_dirty_bit_suspect = 0ULL;
	ss->num_commit_stale = ss->num_commit_binfo = 0;
	ss->longest_sync_time = 0;
	ss->n_resources_reused = 0;
	#ifdef DEBUG_TOPO_CNTRS
		atomic64_set(&ss->num_autonomous_ok , 0);
		atomic64_set(&ss->num_autonomous_err, 0);
	#endif
}

void block_api_sync_clear_stats(struct nvmeibc_block_device *nd)
{
	unsigned long flags;
	spin_lock_irqsave(&nd->dp.resub.lock, flags);
	nvmeibc_datapath_syncs_zero_stats(&nd->dp.sync_rsrcs);
	spin_unlock_irqrestore(&nd->dp.resub.lock, flags);
}

int nvmeibc_sync_txid_wrap(struct nvmeibc_cmd_lock *l,
	nvmeibc_sync_cb_t done_cb, void* context)
{
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP, done_cb, context);
}

int nvmeibc_sync_recover_dirty(struct nvmeibc_cmd_lock *l,
	nvmeibc_sync_cb_t done_cb, void* context)
{
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_RECOVER_DB, done_cb, context);
}

int nvmeibc_sync_unknown_binfo_recov(struct nvmeibc_cmd_lock *l,
	nvmeibc_sync_cb_t done_cb, void* context)
{
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO, done_cb, context);
}

int nvmeibc_sync_scrubbing(struct nvmeibc_cmd_lock *l,
	nvmeibc_sync_cb_t done_cb, void* context)
{
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING, done_cb, context);
}

int nvmeibc_sync_unknown_binfo_by_io(struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	struct recovery_sync_op *so = NULL;
	int rv = -EPERM;
	if (!(so = __alloc_so())) {
		rv = -ENOMEM;
		goto error;
	}
	if ((rv = __init_so(NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO, so, 0, LOCKSET_SLICES, lock, when_done_cb, NULL))) {
		goto error;
	}
	so->when_done_context = context;
	__copy_only_owner_lock(so->locks, lock);
	nvmeibc_cmd_lock_set_bi(&so->locks[0], so->orig_rldr->rld.pre);		// binfo is merged from all available lock copies and is set into the sync lock, the original locks retain their value
	so->assume_caller_holds_locks = true;
	__syncs_list_op('+', so);
	rv = 0;
	__ndump_operation(sync_maintain_job, &so->o);
	goto out;
error:
	if (so)
		so->when_done_cb = NULL; // Error returned without callback
	__free_sync_op(so, false /* slot was not reserved yet */);
out:
	return rv;
}

int nvmeibc_sync_fix_some_slices_in_stale(struct nvmeibc_cmd_lock *l,
									   u16 start_block, u16 n_slices,
									   nvmeibc_sync_cb_t done_cb, void* context)
{
	BUG_ON(start_block >= LOCKSET_SLICES);
	BUG_ON(start_block + n_slices > LOCKSET_SLICES);
	if (__can_r1_convert_stale_lock_2_diry_bit(nvmeibc_disk_segment_get_praid(l->ds))) {
		start_block = 0; n_slices = LOCKSET_SLICES;					// ignore the blk range here bcz we set dbit for the whole LOCKSET extent
		return __convert_stale_special_2_dirty_bit(l, done_cb, context); // {RW,DEAD} degraded mode
	} else {
		return __trigger_sync(l, start_block, n_slices, NVMEIB_BLOCK_IO_OP_RECOVER_STALE, done_cb, context);
	}
}

int nvmeibc_sync_fix_stale(struct nvmeibc_cmd_lock *lock,
									   nvmeibc_sync_cb_t done_cb, void* context)
{
	return nvmeibc_sync_fix_some_slices_in_stale(lock, 0, LOCKSET_SLICES, done_cb, context);
}

int nvmeibc_sync_jour_hot_gc(struct nvmeibc_cmd_lock *l, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_HOT_GC, when_done_cb, context);
}

int nvmeibc_sync_commit_stale_lock(struct nvmeibc_cmd_lock *l, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	return __trigger_sync(l, 0, 0, NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE, when_done_cb, context);
}

int nvmeibc_sync_commit_binfo(struct nvmeibc_cmd_lock *l, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	/* Note: this sync does not alter data but it is still marked as fixing all
	   slices coz it does fix the problem for entire blockset. If however a
	   more accute problem is discovered (like write hole) then mutation will
	   mark that this sync does not fix all slice */
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO, when_done_cb, context);
}

int nvmeibc_sync_turn_on_dirty_convict(struct nvmeibc_cmd_lock *l, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON, when_done_cb, context);
}

int nvmeibc_sync_read_failure(struct nvmeibc_cmd_lock *l, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	/* Daniel: Precicely: Read-Fail recovery is cosher as long as it does not
	   encounter stale lock (On stales it acts as implicit stales sync).
	   I did a quick solution. Todo: Launch the read-fail sync and test the
	   safety conition only if/when stale lock is encountered (Not here) */
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL, when_done_cb, context);
}

int nvmeibc_sync_fix_cold(struct nvmeibc_cmd_lock *l, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	return __trigger_sync(l, 0, LOCKSET_SLICES, NVMEIB_BLOCK_IO_OP_REC_COLD, when_done_cb, context);
}

int nvmeibc_sync_generic_by_op(struct nvmeibc_cmd_lock *lock, u16 start_block, u16 n_slices,
	enum nvmeib_block_io_op op, nvmeibc_sync_cb_t when_done_cb, void* context)
{
	const int valid_op_types = NVMEIB_BLOCK_IO_OP_RECOVER_STALE | NVMEIB_BLOCK_IO_OP_REC_SPARE | NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO;
	if (!(op & valid_op_types)) {								return -EINVAL;	// Not a sync
	} else if (op == NVMEIB_BLOCK_IO_OP_REC_COLD) {				return -EINVAL;	// Sync lib does not support recoveries
	} else if (op == NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_GC) {		return -EINVAL;	// Sync lib does not support recoveries
	} else if (op == NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO) {	return -EINVAL;	// Sync lib does not support recoveries
	} else if (op == NVMEIB_BLOCK_IO_OP_MAINTAIN_BLKSET_RECOV) {return -EINVAL; // Internal decision of sync lib, cannot be called externally
    } else if (op == NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB) {	return -EINVAL;  // Internal decision of sync lib, cannot be called externally
	} else if (op == NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE) {
		BUG_ON((start_block | n_slices) != 0);	// Wrong argument, this sync does not fix any slice, all should be 0
	} else if (op == NVMEIB_BLOCK_IO_OP_RECOVER_STALE) {
		return nvmeibc_sync_fix_some_slices_in_stale(lock, start_block, n_slices, when_done_cb, context);
	} else if (op == NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO) {
		return nvmeibc_sync_unknown_binfo_by_io(lock, when_done_cb, context);
	} else {
		BUG_ON((start_block != 0) | (n_slices != LOCKSET_SLICES));	// Wrong argument, this sync always fixes entire blockset
	}
	return __trigger_sync(lock, start_block, n_slices, op, when_done_cb, context);
}

// EC-1477: Hide this entire state machine in _ec_ files. It should not be in common!
static int __jour_garbg_collect_o(struct nvmeibc_d_rdma_comp *lock_comp, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l =  lock_of_bcomp(lock_comp); // Always first lock
	struct recovery_sync_op *so = (void*)l->cmds;
	int err = 0;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();

	(void)tag;
	__ndump_operation(jour_garbg_collect, &so->o);

_func_start:
	if (so->error != 0) {
		_NTSO(t_01_jgco, "stage=@SYNC_STAGE, err=@ERR", so->stage, so->error);
		so->stage = sync_stage_done;
	}
	switch (so->stage) {
		case sync_stage_start:{
			so->stage = sync_stage_recov_read_cmds_sent;
			nvmeibc_atomic_set(&l->n_uncompleted_locks, 1 + LARGE_SYNC_DEBUG_VALUE);
			TODO(JGC, Add support viewing lock without command and use the read instead of cpxchng from 0->0)
			#if 1
			lock_comp->compare  = lock_comp->exchange = 0ULL;
			l->comp.code = NVMEIBC_CMD_LOCK_UNLOCK;	// Compare exchange to zero
			__invoke_crash_on_lock_corruption(l, 0, "take", 1);	// As if was taken before release
			lock_comp->opr = NVMEIBC_LOCK_CMP_AND_SWAP;
			nvmeibc_cmd_lock_request_io_pet_describe(so->o, l);
			err = BLKCMP_SO_ASYNC_AWAIT_RV(icore_ops->run_cmpxchg(icore_ops, l->ds->disk, handle_of(l->ds), l->address, lock_comp));
			#else
			__change_lock_status_to(l, NCL_STATUS_INVALID);
			l->comp.code = NVMEIBC_CMD_LOCK_READ_DR;	// Important, we are going to only read the lock
			err = BLKCMP_SO_ASYNC_AWAIT_RV(icore_ops->run_read_lock(icore_ops, l->ds->disk, handle_of(l->ds), l->address, lock_comp));
			#endif
			if (!err)
				BLKCMP_SO_ASYNC_RESUME_CUR(0);
			so->error = -10018;	  // Abort, could read lock value
			goto _func_start;
		}

		case sync_stage_recov_read_cmds_sent:{
			const bool is_lock_zero = NCL_do_i_have_lock(lock_comp->lock_status);
			const union nvmeib_lock_id holder = nvmeibc_d_rdma_comp_get_contending_id(lock_comp);
			nvmeibc_cmd_lock_response_io_pet_describe(so->o, l);
			dp_locks_trace_lock_comp(so->o, l, lock_comp);
			__sync_dp_locks_release_cb(l, lock_comp);

			if (is_lock_zero) {		// Can free garbage jounrlas
				so->stage = sync_stage_recov_lo_all_taken;
				ASYNC_AWAIT_AND_RESUME(so->o->nd->dp.sync_execute_op(so), 0);	// Will return to state machine in state sync_stage_recov_owner_ulock_sm
			} else if (holder.bits.is_stale) {
				_NTSO(t_02_jgco, "Not garbage: journal needed. lock=@LOCK_ENT_U64", holder.all);
			} else { /* Error while reading lock or lock is held by other clnt. We can do here analysis like cold recovery and free all except the actual candidate, but simpler solution is just to ignore. On the next garbage collection request, this lock will be unlocked */
				_NTSO(t_03_jgco, "Not garbage: dc->status=@NCL_STATUS_STR, lock=@LOCK_ENT_U64", ncl_status_str(lock_comp->lock_status), holder.all);
			}
			so->stage = sync_stage_done;
			goto _func_start;
		}

		case sync_stage_recov_owner_ulock_sm:
		case sync_stage_done:{ // Free memory
			return __do_on_stage_done(so);
		}
	default:;
	} // switch (->stage)
	BUG();	// Illegal state
	return 0;
}


int nvmeibc_sync_gc_unlocked_lock(struct nvmeibc_cmd_lock *l, nvmeibc_sync_cb_t done_cb, void* ctx)
{
	if (unlikely(should_abort_sync_on_detach(l)))
		return -10031;
	else {
		struct recovery_sync_op *so = __create_autonomous_sync(NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_GC, l, done_cb, ctx);
		if (so) {
			struct nvmeibc_d_rdma_comp *dc = &so->locks[0].comp;
			so->o->user_data = l->user_data;
			set_callback_as_locks_state_machine(dc, __jour_garbg_collect_o);
			__jour_garbg_collect_o(dc, nvmeibc_d_rdma_comp_tag_make());
			return 0;
		} else
			return -ENOMEM;
	}
}

static void __execute_sync_operation(struct recovery_sync_op *so)
{
	if (so->o->nd->dp.sync_prepare_op(so) < 0) {
		_NTSO(error_dp_sync_common_nvmeibc_sync_submit, "Error in prepare");
		__free_sync_op(so, true);
	} else {
		struct operation *o = so->o;
		struct nvmeibc_cmd_lock *l = &so->locks[0];
		int i;
		//accumulate and counts the total syncs operations along the block lifetime.
		IO_STATS_SYNC_INCR(&o->nd->dp.io_stats, o->op);
		for (i = 0; i < l->n_siblings; i++ ) {	// Update locks with result of prepare_op(so)
			l[i].cmds = so->cmds;
		}
		so->error = 0;					// If no error occurs, this field is not updated
		o->cmds = so->cmds;				// Copy fields into 'o' for debug/prints
		o->locks= so->locks;
		__compressed_sync_op_trace_start(so);
		__handle_locks_o(&l->comp, nvmeibc_d_rdma_comp_tag_make());	// Start on owner
	}
};

static void __execute_sync_operation_work(struct work_struct *work)
{
	struct measured_work *mw = measured_work_from(work);
	struct operation *o = container_of(mw, struct operation, work_rso_execute);
	nvmeib_wq_metrics_update(nvmeibc_rso_execute_wq_latency, measured_work_wait_ticks(mw));
	__execute_sync_operation(o->rso);
}

int nvmeibc_sync_submit(struct nvmeibc_block_device *nd)
{
	struct recovery_sync_op *so;
	const bool should_autofail = nvmeibc_block_status_is_detaching(nd->status);

	if (!(so = __syncs_list_get_so(nd, should_autofail)))
		return 0;				// No syncs ops processed
	so->error = -ENOMEM;		// Every alloc problem is sync failure
	nvmeibc_operation_alloc_dbg_id(so->o);
	if (should_autofail) {
		__free_sync_op(so, true);
	} else if (!NVMEIB_CPU_MASK_INFO_IS_EMPTY(so->o->cpu_mask_info)) {
		MEASURED_INIT_WORK(&so->o->work_rso_execute, __execute_sync_operation_work);
		dp_block_schedule_work(so->o->cpu_id, &so->o->work_rso_execute.work);
	} else {
		__execute_sync_operation(so);
	}

	return 1;						// 1 sync operation processed (failed or OK)
}

static void __debug_verify_no_access_to_wrong_topo(const struct nvmeibc_block_command *c)
{
	const enum nvmeib_block_io_op op = c->iocmd->reqs1.op;
	const enum NVMEIBTC_DS_MODE acm = c->ds->toma_acm;		// Deliberatly not testing via bitmaps to add another layer of debugging
	const bool readable_dp_topo = ((acm != NVMEIBTC_DS_MODE_DEAD) && (acm != NVMEIBTC_DS_MODE_W_IS_DIRTY));	// Data can be read from 'W' seg without dbit, just an optimization to avoid writes.
	const bool is_write = ((nvmeib_block_io_op_is_write(op))||(op == NVMEIB_BLOCK_IO_OP_WRITE_UNCOR));
	if (op & NVMEIB_BLOCK_IO_OP_MD_READ)
		WARN(!nvmeibc_is_readable_acm(acm), "nvmeibc bug, read untrustable metadata %u", acm);
	else if (is_write) {
		WARN_ON(acm == NVMEIBTC_DS_MODE_DEAD);
	} else if (op == NVMEIB_BLOCK_IO_OP_READ) {
		WARN_ON(!readable_dp_topo);
	} else {
		WARN(op, "nvmeibc bug: unsupported op=%d, acm=%d\n", op, acm);
	}
}

static void __nvmeibc_sync_send_cur_stage_cmds(struct recovery_sync_op *so)
{
	const int last_cmd  = so->last_cmd;	/* copy to stack coz 'so' may free() */
	int i, err = -ENXIO;
	for (i = (so->last_cmd + 1 - so->n_cmds); i <= last_cmd; i++) {
		struct nvmeibc_block_command *c = &so->cmds[i];
		DEBUG_TRANSFERS_is_init_cb_counter(&c->iocmd->comp);
		if (c->do_not_send) {
			c->iocmd->comp.comp_code = err; // Simulate completion
		} else {
			__debug_verify_no_access_to_wrong_topo(c);				// Just a sanity, todo: remove
		}
		dp_cmds_tryexec_cmd(so->cmds, i, err);
	}
	so = NULL; /* BEWARE! sync operation already complete and free() here */
}

void nvmeibc_sync_send_cur_stage_cmds(struct recovery_sync_op *so);
void nvmeibc_sync_send_cur_stage_cmds(struct recovery_sync_op *so)
{
	BLKCMP_SO_ASYNC_AWAIT(__nvmeibc_sync_send_cur_stage_cmds(so));
}

void nvmeibc_sync_prepare_so_for_read(struct recovery_sync_op *so, const roles_bmp_t *read_bmp, const enum sync_op_stage_e next_stage)
{
	int n_cmds_to_wait_for = so->n_cmds = n_read_cmds(so);
	so->last_cmd = so->n_cmds - 1;
	so->stage = next_stage;
	if (read_bmp)	// Else send all commands as prepared
		nvmeibc_sync_set_cmds_do_not_send_by_bmp(so, (so->last_cmd + 1 - so->n_cmds), so->last_cmd, (*read_bmp));
	nvmeibc_sync_set_uncompleted_cmds(so, n_cmds_to_wait_for);
}

// read_bmp == NULL means don't change don't send vals.
void nvmeibc_sync_send_all_read_cmds(struct recovery_sync_op *so, const roles_bmp_t *read_bmp, const enum sync_op_stage_e next_stage)
{
	nvmeibc_sync_prepare_so_for_read(so, read_bmp, next_stage);
	nvmeibc_sync_send_cur_stage_cmds(so);
}

void nvmeibc_sync_prepare_so_for_write(struct recovery_sync_op *so, const roles_bmp_t write_bmp, const enum sync_op_stage_e next_stage)
{
	int n_cmds_to_wait_for = so->n_cmds = n_write_cmds(so);
	so->last_cmd = last_cmd(so) - 1;	// Last write cmd
	so->stage = next_stage;
	if (nvmeibc_raid_is_ec(so->r1))
		nvmeibc_sync_set_cmds_do_not_send_by_bmp(so, (so->last_cmd + 1 - so->n_cmds), so->last_cmd, write_bmp);
	nvmeibc_sync_set_uncompleted_cmds(so, n_cmds_to_wait_for);
}

void nvmeibc_sync_send_all_write_cmds(struct recovery_sync_op *so, const roles_bmp_t write_bmp, const enum sync_op_stage_e next_stage)
{
	nvmeibc_sync_prepare_so_for_write(so, write_bmp, next_stage);
	nvmeibc_sync_send_cur_stage_cmds(so);
}

void nvmeibc_restore_read_cmds_do_not_send_vals(struct recovery_sync_op *so) {
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t send_bmp = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, readable);
	nvmeibc_sync_set_cmds_do_not_send_by_bmp(so, 0, n_read_cmds(so) - 1, send_bmp);
}

void nvmeibc_restore_write_cmds_do_not_send_vals(struct recovery_sync_op *so) {
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t send_bmp = nvmeibc_raid1_get_inverse_roles_bmp(so->r1, slice_start, dead);
	if (nvmeibc_raid_is_ec(so->r1))
		nvmeibc_sync_set_cmds_do_not_send_by_bmp(so, (so->last_cmd + 1 - so->n_cmds), so->last_cmd, send_bmp);
}
