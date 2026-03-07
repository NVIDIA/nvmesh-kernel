#include "nvmeibc_raid_recovery.h"
#include "block/nvmeibc_block_common.h"
#include "nvmeibc_icore_ops.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "common/compat/kr_incs_compiler_types.h"
#include "nvmeib_event.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"
#include "nvmeibc_io_pet.h"
#include "common/nvmeib_measured_work.h"
#include "clnt/nvmeibc_wq_metrics.h"

struct __fake_recovery{ struct{ u64 task_id; const struct nvmeibc_subscription_ctx *tr; } args; enum NVMEIBT_RECOVERY_TYPE type; };
#define DECL_FAKE_RECOVERY(id, type, tr) struct __fake_recovery __fake_rcvr = {{(id), (tr)}, (type)}, *recov = &__fake_rcvr;	// For prints/traces, when recovery context does not exist

struct nvmeibc_recovery_hooks* rcvr_hooks = NULL;									//cannot make per recovery - the recoveries are managed by topology attach/detach tests destroy topology;

NVMEIBC_WQ_METRIC(nvmeibc_skip_recov_wq_latency, "reason=skip_recov");

/****************** Update stats on batch request **************************/
int recovery_on_batch_request(struct nvmeibc_recovery *recov)
{
	ulong flags;
	int rv = 0;
	spin_lock_irqsave(&recov->guard, flags);
	if (recov->status != RCVR_CANCELING)
		recov->status = RCVR_PROCESS;	// {INIT (on first batch) / PROCESS (on next batch)}->PROCESS
	else
		rv = -10043;	// Rare race condition: Canceled recovery between prev batch completed OK and next batch not requested yet
	recov->b_start += recov->b_length;			// Skip the prev batch
	recov->b_length =  0;
	if (recov->itr.on_batch_done)
		recov->itr.on_batch_done(&recov->itr);
	spin_unlock_irqrestore(&recov->guard, flags);
	return rv;
}

void recovery_on_batch_req_comp_calc_upper_bound(struct nvmeibc_recovery *recov, u64 n_elem, u64 size)
{
	unsigned long flags;														// Must hold spinlock, to prevent probing partially updated iterator
	_NTRR(trace_3_raid_recovery_blocksets_problems_read_cb, "n_elem=@LLU, b_length=@B_LENGTH", n_elem, size);
	spin_lock_irqsave(&recov->guard, flags);
	recov->itr.set_upper_bound(&recov->itr, n_elem);
	recov->b_length = size;
	spin_unlock_irqrestore(&recov->guard, flags);
}

/************************* nvmeibc_recov_sync_worker **************************/
typedef int (* _sync_cb_fn)(struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t done_cb, void *context);	// recovery call that runs the actual recovery
struct nvmeibc_recov_sync_worker {		// Recovery can fix a few blocksets in parallel
	struct operation o; 				// operation of the 'sync'. Possible to view sync worker as inerittance (extention) of operation
	struct nvmeibc_recovery_itr_job job;// Which job (blkset) was assigned to worker by the iterator
	_sync_cb_fn fn;						// recovery 'type' does not define uniquely how to fix single blockset, so we need virutal func
	struct nvmeibc_raid1 *pr;			// Just caching for short writing pr == __get_r1_by_tr(o->topo, tr)
	struct delayed_work dwork;			// Delayed work to wait on in case is_delayed
};										// Array of workers
#ifndef BLKCMP_RC_COMPLETION_PRESERVE_STACK
	#define RECOV_MAX_PARALLEL_SYNCS (64)	/* Max amount of parallel workers */
#else
	#define RECOV_MAX_PARALLEL_SYNCS (1)	/* Syncs are blocking, no benefit of launching parallel syncs */
#endif
#define RECOV_DFL_PARALLEL_SYNCS RECOV_MAX_PARALLEL_SYNCS	// Default amount of parallel workers is the maximum
#if NVMEIBC_MAX_ALLOWED_SYNC_OPS_LIMIT < RECOV_MAX_PARALLEL_SYNCS
	#error Wrong Constants. Total amount of syncs per volume < amount of worker of single recovery?
#endif
#if NVMEIBC_MAX_ALLOWED_SYNC_OPS_DEFAULT < 2*RECOV_MAX_PARALLEL_SYNCS
	#error Wrong Constants. Default amount of syncs per volume is too small... Not enoguh for 2 recoveries. What if volume is extended and has many praids? Or multiple recoveries for each praid?
#endif
// Number of syncs that require progress message
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	#define N_SYNCS_FOR_PROGRESS(pr) (1 + (tr->length/LOCKSET_SLICES)/800)			// More frequent reports to test race conditions and support small volumes
	#define DELAY_RECOVERY_START_BY_N_SEC  (-1)			// Avoid rescheduling to thread context, to make tests faster, can use (0) to allocate recovery on system work queue
	#define SELF_BATCHES_OF_BLOCKSETS	   (2)			/* Work in small batches */
#else
	#define N_SYNCS_FOR_PROGRESS(pr) ((((1<<30)/BYTES_IN_LOCKSET)/((pr)->slice_size*2)) * RECOV_DFL_PARALLEL_SYNCS)		// Report Every 1/2[Gbyte] of rebuilt rlba, per worker.
	#define DELAY_RECOVERY_START_BY_N_SEC  (HZ)			// Ronen Hod: Brute-force hack. Allow 1 sec for the registrations to all praid segments, since recovery fails when 1 toma requests recovery but other segs are not all registered
	#define SELF_BATCHES_OF_BLOCKSETS	   (256)		/* Work in batches of 256 blksets */
#endif

#define __sync_worker_of_o(o_ptr) container_of(o_ptr, struct nvmeibc_recov_sync_worker, o)
#define __recov_worker0(recov)  ((recov)->sw[0])	// When syncs are not running, use the first worker context to perform work and hold topo for recovery

static struct nvmeibc_recov_sync_worker *nvmeibc_recov_sync_worker_create_array(void)
{
	struct nvmeibc_recov_sync_worker *sw = kzalloc(sizeof(*sw)*RECOV_MAX_PARALLEL_SYNCS, GFP_KERNEL);
	return sw;
}

static void __recover_next_blockset_delayed_handler(struct work_struct *work);

static int nvmeibc_recov_sync_worker_init(struct nvmeibc_recov_sync_worker *sw,
		/* Params used as context void* */	struct nvmeibc_block_device *nd, struct nvmeibc_recovery *recov)
{
	struct operation *o = &sw->o;
	int rv = 0, i, n_locks = N_MAX_RAID_LOCKS, n_cmds = 1;	// Todo: Use topology value instead of N_MAX_RAID_LOCKS
	if ((nvmeibc_clmat_allocate(o, false, n_locks, n_cmds, 0) < 0) || (!(o->cmds = dp_cmds_kvzalloc(n_cmds)))) {
		rv = -ENOMEM;
		goto _out;
	}
	o->nd = nd;
	o->op = NVMEIB_BLOCK_IO_OP_RECOV_PROBLEM;
	o->locks->cmds = o->cmds;
	o->cmds->ncmds = n_cmds;				// Ficticious commands
	o->cmds->o = o;
	o->cmds->cmdarr = o->cmds;
	o->cmds->locksets = o->locks;
	o->vro = recov;
	o->topo = NULL;
	o->cpu_id = NR_CPUS;
	INIT_DELAYED_WORK(&sw->dwork, __recover_next_blockset_delayed_handler);
	for (i = 0; i < n_locks; ++i) {
		nvmeibc_b_rdma_comp_init(&o->locks[i].comp, i, o->locks);
		nvmeibc_clmat_set_link(o->CLmat, 0, i, o->locks);
	}
	nvmeibc_recovery_itr_job_init(&sw->job);
_out:
	if (rv) {
		_NT(tr_1_recov_resource_alloc, "alloc failed. rv=@RV", rv);
	}
	return rv;
}

static void nvmeibc_recov_sync_worker_free(struct nvmeibc_recov_sync_worker *sw, int i)
{
	struct operation *o = &sw->o;
	sw->fn = NULL;
	dp_cmds_free_all(o->cmds);
	nvmeibc_operation_move_mem_to_locks(o);
	nvmeibc_clmat_free_dangling_locks(o->locks);
	WARN(o->topo, "nvmeibc bug, topo=%p, held by sw=%d\n", o->topo, i);
}

#define __sync_worker_clean_topo(sw) ({ (sw)->o.topo = NULL; (sw)->pr = NULL; })
static void nvmeibc_recov_sync_worker_put_topo(struct nvmeibc_recov_sync_worker *sw)
{
	if (likely(sw->o.topo))
		_ND(trace_3_sync_worker_put_topo, "o=@OPERATION @TOPOLOGY @TOPO_DBG_ID", &sw->o, sw->o.topo, sw->o.topo->debug_unique_index);
	nvmeibc_topology_put(sw->o.topo);
	__sync_worker_clean_topo(sw);
}

#define __effort_percents_to___nsw(effrt) (((u32)(effrt)*RECOV_MAX_PARALLEL_SYNCS)/100)
#define __effort_percents_from_nsw(recov) (((recov)->n_sw_to_use*100)/RECOV_MAX_PARALLEL_SYNCS)
static void  __update_effort_from_toma_msg(struct nvmeibc_recovery *recov, const struct nvmeibt_client_recovery_generic_header *task)
{
	const struct nvmeibc_subscription_ctx *tr = recov->args.tr;
	const struct nvmeibc_block_device *nd = nvmeibc_block_nt_to_b(tr->nt);
	const u32 nsw = __effort_percents_to___nsw(task->effort_percents);
	const bool do_update = !nd->ignore_all_recov_toma_speed_req;
	_NTRR(t_tt_recov_08, "Setting=@BOOL_YN: effort @EFFORT_PERCENTS[nsw=@UINT], batch_size(@UINT->@UINT)", do_update, task->effort_percents, nsw, recov->max_batch_size, task->max_batch_size);
	if (do_update && (nsw != NVMEIBT_CLIENT_PROTOCOL_EFFORT_PERCENTS_DONT_CARE))
		nvmeibc_recovery_set_num_sw(recov, nsw);
	if (do_update && (task->max_batch_size != NVMEIBT_CLIENT_PROTOCOL_BATCH_SIZE_DONT_CARE))
		nvmeibc_recovery_set_max_batch_size(recov, task->max_batch_size);
}

/********************* Dirty bits/convict reinition logic *****************************/
/* Without prior knowledge: Every blockset potentially, may have uncommited binfo. We do the inject for to cover 2 cases:
   1. Binfo on copy lock is for 100% uncommited: Owner-lock on RW seg, copy on W seg. Must copy the binfo from owner to copy
   2. Binfo on copy lock might have wrong dbits: All locks are on RW segs. Owner lock has not dirty markers, but on copy locks there is dirty bit for W seg because dbits turn-off-sync failed towards the end. We dont leave a stale lock if binfo is incorrect. A more correct future impl will be to leave stale lock on binfo operation errors.
   3. Combination of the above: Example: EC 8+2: {S0=D0-RW, S1=D1-Dead,S8=P-W, S9=Q-RW}. Rebuild from S0. For 10% of praid lock map is {S0-owner-RW,S9-copy1-RW,S8-copy2-W}. This is reason 1. For another 10% S0 took ownership from dead S1 so lock maps is {S0-owner-RW,S9-copy2-RW}. This is reason 2 */
static void __uncommited_binfo_inject_to_blkset_problem(union nvmeib_blkset_problem_report *arr, const u64 n_elem)
{
	u64 i;
	for (i = 0; i < n_elem; i++)
		arr[i].binfo_not_commited = (u16)1;
}

static __attribute__((unused)) bool __uncommited_binfo_detect_case_1(struct nvmeibc_cmd_lock *l)
{
	int i, all_ok = true;
	for (i = 0; (i < l->n_siblings) && all_ok; i++) {
		all_ok = nvmeibc_is_readable(l[i].ds); // RW/W+ has correct binfo, W - uncommited, Dead - Impossible, lock would not have this sibling
	}
	return !all_ok;
}

#include "block/controlpath/nvmeibc_b_cp_topo_common.h"
#define dlba_blksets_ofst(tr) ((tr)->first_lba / LOCKSET_SLICES)
#define n_bits_blkset_problem (sizeof(union nvmeib_blkset_problem_report)*8)
#define n_bits_blkset_stale_s (sizeof(union nvmeib_blkset_sparse_report )*8)
#define nvmeibc_is_request_sparse(el_size) ((el_size) == n_bits_blkset_stale_s)

static int __blocksets_problems_read_cb(struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag)
{
	int rv = 0;
	struct nvmeibc_cmd_lock *db_req = lock_of_bcomp(dc);
	struct operation *o = db_req->cmds->o;
	struct nvmeibc_recovery *recov = o->vro;
	struct nvmeibc_topology *topo  = o->topo;	// Possibly NULL
	struct nvmeibc_subscription_ctx *tr = recov->args.tr;
	const u64 n_elem = dc->dbits_arr.size;
	const struct nvmeibc_disk_segment *seg = db_req->ds;
	u64 b_length;								// Exact lenght of the batch (in units of blocksets)
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();

	u8 *arr = dc->dbits_arr.arr;
	(void)tag;
	__sync_worker_clean_topo(__sync_worker_of_o(o));			// Disconnect topology from worker to be able to free it even if worker finishes
	if (NCL_had_acquire_callback(dc->lock_status))
		icore_ops->cb_called_comp(icore_ops, seg->disk, dc);

	if (unlikely(!NCL_do_i_have_lock(dc->lock_status))) {
		_NTRR(trace_raid_recovery_blocksets_problems_read_cb, "Data cannot be obtained: @NCL_STATUS_STR", ncl_status_str(dc->lock_status));
		BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_finish(recov, -10040), recov, true);
		goto out;
	}

	if (nvmeibc_is_request_sparse(db_req->n_siblings)){
		if (n_elem == 0){
			b_length = (recov->args.r_end - recov->b_start);	// Exactly the remaining range
			arr = NULL;
		} else {
			const u64 dlba_start = recov->b_start + dlba_blksets_ofst(tr);	// slba -> dlba
			const union nvmeib_blkset_sparse_report *el = (void*)arr;
			const u64 last_dlba = (u64)el[n_elem-1].ind;		// Todo: Encapsualte the logic of those few lines into iterator
			WARN_RR(last_dlba < dlba_start, "Wrong dlba range: start=0x%llx, last=0x%llx\n", dlba_start, last_dlba);
			b_length = (last_dlba - dlba_start + 1ULL);
		}
	} else {
		if (n_elem == 0) {
			_NTRR(trace_2_raid_recovery_blocksets_problems_read_cb, "non-sparse request with n_elem = 0, failing recovery");
			BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_finish(recov, -10042), recov, true);
			goto out;
		}
		if ((recov->type == NVMEIBT_RECOVERY_TYPE_SCRUBBING) ||
			(recov->is_ec_raid && (recov->type == NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD))) {
			__uncommited_binfo_inject_to_blkset_problem((void*)arr, n_elem);
		}
		b_length = n_elem;		// Bitmap worst case is it is full (need to process all blocksets)
	}

	recovery_on_batch_req_comp_calc_upper_bound(recov, n_elem, b_length);
	rv = recov->itr.reinit(&recov->itr, dlba_blksets_ofst(tr), recov->b_start, n_elem, db_req->n_siblings, arr);
	if (unlikely(rv)){
		_NTRR(trace_4_raid_recovery_blocksets_problems_read_cb, "Failed to reinit recovery @RV", rv);
		BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_finish(recov, -10044), recov, true);
		goto out;
	}
	BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_start(recov), recov, false);
out:
	nvmeibc_topology_put(topo);
	return 0;
}

/* Simulate answer from transport that all blocksets need fixing */
static int __simulate_get_problems_array_from_server(struct nvmeibc_d_rdma_comp *dc, u64 dlba_length)
{
	union nvmeib_blkset_problem_report _arr[SELF_BATCHES_OF_BLOCKSETS];
	memset(_arr, 0xD, sizeof(_arr));  				// Arbitrary 0xD0D value written into dbits
	dc->dbits_arr.arr = (void*)&_arr[0];
	dc->dbits_arr.size = min(dlba_length, (u64)ARRAY_SIZE(_arr));
	dc->lock_status = NCL_STATUS_TRANSFERRED; 		// Transfer from stack to callback
	dc->callback(dc, nvmeibc_d_rdma_comp_tag_make());
	return 0;		// Always succeeds
}

/* 11/04/2018, Important! Agreed with Ronen. Daniel implemented only a
   simplified logic of do_only_owners==true, for Raid1/5/6
   It makes stuff really easy coz, we have to read only 1 dbit map.
   This is good enough coz each Toma launches its local client to take care
   of only owner blocksets. In R1 2-mirror this is the general case */
static inline bool __recovery_are_wrong_owners(const struct nvmeibc_recovery *recov)
{
	if (!recov->args.silent_mode)
		return (!recov->args.do_only_owners && recov->is_ec_raid);
	else
		return false; // We can trigger manual blockset recovery via ioctl anyway we want
}

static int __recovery_get_topo(struct nvmeibc_recovery *recov, struct nvmeibc_recov_sync_worker *sw);
static void __get_blksets_info_next_work_batch(struct nvmeibc_recovery *recov)
{
	struct operation *o = &__recov_worker0(recov).o;						// Use Default first worker to hold topo
	struct nvmeibc_disk_segment *seg;
	struct nvmeibc_subscription_ctx *tr = recov->args.tr;
	int rv = 0;
	bool need_info_from_server = true;
	struct nvmeibc_cmd_lock *db_req = &o->locks[0];	// Request dbits via 'comp' of first lock
	struct nvmeibc_d_rdma_comp *dc = &db_req->comp;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	struct nvmeibc_blkst_arr_req *req = &dc->dbits_arr_req;

	dc->callback = __blocksets_problems_read_cb;
	if ((rv = recovery_on_batch_request(recov)) != 0)
		goto __treat_error_via_cb;
	if ((rv = __recovery_get_topo(recov, __sync_worker_of_o(o))) != 0)			// Lock topology here and release when result arrive from core.
		goto __treat_error_via_cb;

	db_req->ds = seg = &__sync_worker_of_o(o)->pr->segments[tr->seg];
	if (__recovery_are_wrong_owners(recov) || (seg->toma_acm != NVMEIBTC_DS_MODE_RW)) {
		WARN_RR(true, "Toma bug: %d, acm=%s\n", recov->args.do_only_owners, nvmeibt_client_topo_seg_access_mode_to_str(seg->toma_acm));
		rv = -10046;
	}
	// Pack the requested problems in each blockset
	req->get_full_val = true;
	req->get_dbits = 1; req->get_stales = 1; db_req->n_siblings = n_bits_blkset_problem;		// Default value. n_bits_blkset_problem is a must because by current implementation dbits rebuild recovery needs to check all blocksets in raid because a dbit might apear on copy_lock and not on owner. (BUG: EC-3914). Also In EC dbits rebuild, must clean dbits & stale locks, Note: For R1-2mirrored Toma launches NVMEIBT_RECOVERY_TYPE_STALE_REBUILD only in topologies where dbits dont exist
	if (recov->type == NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON || recov->type == NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO ) {
		need_info_from_server = false;	// No info needed, we have to fix each and every blockset. In future optimize for unknown binfo to get this list from server
	} else if ((recov->is_ec_raid) && (recov->type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD)) {
		req->get_dbits = 0; db_req->n_siblings = n_bits_blkset_stale_s;	// Need exact value of stale lock to access recoveree clinets journal
	}
	if (!rv) {
		const u64 dlba_start = recov->b_start + dlba_blksets_ofst(tr);
		const u64 blocksets_length = (recov->args.r_end - recov->b_start);
		_NTRR(tr_1_get_next_batch, "requesting blksets: start_@DLBA_BLKSETS length_@DLBA_BLKSETS disk @DISK_NAME",
			 dlba_start, blocksets_length, seg->disk->name);
		if (need_info_from_server)
			rv = icore_ops->get_blkset_problems(icore_ops, seg->disk, handle_of(seg), dlba_start, blocksets_length, dc);
		else
			rv = __simulate_get_problems_array_from_server(dc, blocksets_length);
	}
__treat_error_via_cb:
	if (rv) {
		_NTRR(tr_2_get_next_batch, "autofail rv=@RV", rv);
		dc->lock_status = NCL_STATUS_DISKDEAD;
		dc->callback(dc, nvmeibc_d_rdma_comp_tag_make());	// Every error is handled via call to callback
	}
}

static void __sw_do_next_sync(struct nvmeibc_recovery *recov, struct nvmeibc_recov_sync_worker *sw);
static void __extract_worst_error_from_sync_workers_and_finish_batch(struct nvmeibc_recovery *recov);
static void __do_full_recovery_state_machine(struct nvmeibc_recovery *recov)
{
	#ifdef BLKCMP_RC_COMPLETION_PRESERVE_STACK
		const u64 task_id = recov->args.task_id;
		#define __recovery_already_finished(recov) ((recov->args.task_id != task_id) || (recov->status == RCVR_READY)) // Note: check for (recov->status == RCVR_READY) is not good as recovery could already finish and restart due to new toma message
		BLKCMP_SO_BLOCKING_CONTEXT_ALLOC(recov);
		while (!__recovery_already_finished(recov)) {
			BLKCMP_RC_ASYNC_AWAIT(recov->do_next_work_batch_cb(recov), recov);		// Request work for next batch (get dbits/stale-locks/etc)
			if (__recovery_already_finished(recov))
				break; 													// Batch request failed or nothing to do (example: no candidates in cold/jgc recov)
			recovery_on_batch_start(recov);
			if (__recovery_already_finished(recov))
				break; 													// Batch start failed, Recovery terminated with error and is ready to run again
			BUILD_BUG_ON(RECOV_MAX_PARALLEL_SYNCS != 1);				// Not supported yet
			while (atomic_read(&recov->n_sw_running) > 0) {				// Launch all syncs in the current batch
				struct nvmeibc_recov_sync_worker *sw = &recov->sw[0];
				__sw_do_next_sync(recov, sw); 							// Sync itself is a syncronous function that preserves stack. No need to await it
			}
			BUG_ON(atomic_read(&recov->n_sw_running) != 0);
			__extract_worst_error_from_sync_workers_and_finish_batch(recov);
		}
	#else
		recov->do_next_work_batch_cb(recov);		// Wake up to first batch
	#endif

}

/*************************** Dummy recovery ***********************************/
static void __dummy_empty_recov_batch(struct nvmeibc_recovery *recov)
{
	struct nvmeibc_subscription_ctx *tr = recov->args.tr;
	u64 n_elem = 0ULL, b_length_ub;
	const u8 *arr = NULL;		// Default: No bad blockset in batch==entire range

	recovery_on_batch_request(recov);
	if (unlikely(rcvr_hooks && rcvr_hooks->on_dummy_get_next_batch))
		rcvr_hooks->on_dummy_get_next_batch(rcvr_hooks, recov, &arr, &n_elem);

	b_length_ub = ((n_elem > 0) ? n_elem : recov->args.r_end);	// Single empty batch covering all blocksets or partial nonempty batch
	recovery_on_batch_req_comp_calc_upper_bound(recov, n_elem, b_length_ub);
	recov->itr.reinit(&recov->itr, dlba_blksets_ofst(tr), recov->b_start, n_elem, n_bits_blkset_problem, arr);
	BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_start(recov), recov, false);
}

static void __schedule_skip_blockset(struct operation *o, int err);
static int __dummy_sync_cb_fn(struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t done_cb, void *ctx)
{
	const int r = (get_random_u32()&0xF);		// 16 optional
	const int err = (r<4) ? -EIO : 0;			// 25% error (0..3)
	if (r < 2) return -EIO;						// 12.5% Syncronous failure (0..1)
	__schedule_skip_blockset(&((struct nvmeibc_recov_sync_worker *)ctx)->o, err); // 75% async success, (3..15) 12.5% async failure (2..3)
	(void)lock; (void)done_cb;
	return 0;
}

/*************************** Assist structs ***********************************/
/* Overtime: Done is always going up. Total can decrease (if next batch is partial)
   1. done/total monotonically increases from 0 to 1
   2. total-done monotonically decreases from (r_end-r_start) to 0 */
static void __recov_get_global_progress(const struct nvmeibc_recovery *recov, u64 *done, u64 *total)
{
	if (recov->itr.ctx) {
		const u64 prev_batches = (recov->b_start - recov->args.r_start);
		const u64 next_batches = (recov->args.r_end - (recov->b_start + recov->b_length));
		recov->itr.get_progress(&recov->itr, done, total);		// Get info of cur batch
		*done  += prev_batches;					// All previous batches were done
		*total += prev_batches + next_batches;
		WARN((*total < *done), "nvmeibc bug! total=%lld < done=%lld\n", *total, *done);
	} else {					// Error, launching recovery, iterators not created
		*done = *total = 0ULL;
	}
}

static inline void __recov_args_destroy(struct nvmeibc_recovery_args *args)
{
	memset(args, 0, sizeof(*args));		// task_id = 0 illegal, pointers=NULL
}

static inline void __recov_work_batch_destroy(struct nvmeibc_recovery *recov)
{
	recov->b_start = recov->b_length = 0;
}

static inline void __recov_task_destroy(struct nvmeibc_recovery *recov)
{
	recov->do_next_work_batch_cb = NULL;
	recov->has_appendix_task = false;
}

/************************** Sending messages to Toma **************************/
#include "nvmeib_types.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.h"

struct nvmeibc_raid_recovery_pl {
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type;
	struct nvmeibt_client_recovery_status_pl rs;
};
#define RECOVERY_RLBA_BLKSTS_RANGE_4_u64(recov) \
	(recov)->args.r_start, (recov)->b_start, (recov)->b_start + (recov)->b_length, (recov)->args.r_end

/* Build update message to Toma for valid recovery process */
static void __recovery_build_msg(struct nvmeibc_raid_recovery_pl *pl,
								 const struct nvmeibc_recovery *recov, int err)
{
	u64 done = 0, total = 0;
	s64 num_left;
	__recov_get_global_progress(recov, &done, &total);
	num_left = (s64)(total - done);
	pl->rs.task.id =   recov->args.task_id;
	pl->rs.task.type = recov->type;
	pl->rs.task.effort_percents = __effort_percents_from_nsw(recov);
	pl->rs.task.max_batch_size = recov->max_batch_size;
	pl->rs.ret_code = err;
	pl->rs.praid_version = recov->args.praid_version;
	_NTRR(t_01_brbm, "@PROTOCOL_CLIENT_MSG_STR, [@R_START..{[@B_START..@B_LENGTH)}...@R_END), err=@ERR, num_left=@NUM_LEFT, effort @EFFORT_PERCENTS, max_batch_size=@UINT",
		nvmeibt_protocol_client_msg_str(pl->msg_type), RECOVERY_RLBA_BLKSTS_RANGE_4_u64(recov), err, num_left, pl->rs.task.effort_percents, pl->rs.task.max_batch_size); /* Full job and current batch */
	WARN_RR(num_left < 0, "%llu<%llu\n", total, done);
	pl->rs.next_unfixed_lock = recov->b_start;	// All previous batches were successfull, otherwise this batch would not start
	pl->rs.num_locks_left = num_left;
	if (pl->msg_type == NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH) {
		if (err == 0) {		// Some debug: Todo Fix & remove. n_locks_to_do stores the range of locks to fix, while n_fixed stores the amount of fixed locks (sparse)
			WARN_RR(num_left != 0, "num_left=%lld\n", num_left);	// Todo: What about non mandatory recovery? for them this warn is irrelevant
			pl->rs.next_unfixed_lock = recov->args.r_end;
		}
	}
}

/* Send already built msg to originator Toma */
static int __recovery_send_reply(const struct nvmeibc_subscription_ctx *tr,
					const struct nvmeibc_raid_recovery_pl *pl, bool should_send)
{
	struct nvmeibc_topology *topo;
	struct nvmeibc_disk_segment *seg;
	struct nvmeibt_client_msg_pl msg_pl;
	int rv;
	DECL_FAKE_RECOVERY(pl->rs.task.id, pl->rs.task.type, tr);
// TODO(Remove the if (!should_send), now checking before the call)
	if (unlikely(!should_send)) {
		rv = 0;
		goto _out;
	}
	msg_pl.recov_status = pl->rs;
	topo = nvmeibc_topology_get(tr->nt);
	if (!topo) {			// Daniel: Not sure can happen, protected by caller
		rv = -10047;
		goto _out;
	}
	seg = __get_seg_by_tr(topo, tr);
	_NTRR(trace_raid_recovery_recovery_reply, "msg(@MSG_TYPE) num_left=@NUM_LEFT", pl->msg_type, pl->rs.num_locks_left);
	rv = nvmeibc_toma_send_direct_msg(seg, pl->msg_type, &msg_pl);
	nvmeibc_topology_put(topo);
_out:
	return rv;
}

/* Reply that client rejects Toma's request (Wrong request, Busy, etc...).
   Important: Func() deliberately does not get recovery as param, so while
   rejection is sent to prev recovery, a new one could be already started and
   possibly finished */
static void __recovery_reject_reply(const struct nvmeibc_subscription_ctx *tr,
				u64 task_id, enum NVMEIBT_RECOVERY_TYPE type, int err, int praid_version, bool should_send)
{
	struct nvmeibc_raid_recovery_pl pl = {0};
	DECL_FAKE_RECOVERY(task_id, type, tr);
	pl.msg_type = NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH;
	pl.rs.task.id = task_id;
	pl.rs.task.type = type;
	pl.rs.task.effort_percents = NVMEIBT_CLIENT_PROTOCOL_EFFORT_PERCENTS_DONT_CARE;
	pl.rs.task.max_batch_size = NVMEIBT_CLIENT_PROTOCOL_BATCH_SIZE_DONT_CARE;
	pl.rs.ret_code = err;
	pl.rs.praid_version = praid_version; // Typically != 0, when real recovery cant start, 0 when quering wrong recovery.
	pl.rs.next_unfixed_lock = 0;	// Nothing was fixed, because recovery was rejected (not started).
	pl.rs.num_locks_left = 0;		// Typically != 0, when real recovery cant start, 0 when quering wrong recovery.
	WARN_ON(err == 0);				// Reject is always failure, Incorrect usage of the function
	_NIRR(trace_raid_recovery_recovery_reject_reply, "@EVENT_TAG recovery REJECTED err=@ERR", EV_RAID_RECOV_REJECTED(), err);
	if (should_send) {
		__recovery_send_reply(tr, &pl, should_send);
	}
}

static void __recovery_query_send_reject(const struct nvmeibc_subscription_ctx *tr, const struct nvmeibt_client_recovery_taskid_pl *ri, int err)
{
	__recovery_reject_reply(tr, ri->task.id, ri->task.type, err, 0            , true);
}
static void __recovery_start_send_reject(const struct nvmeibc_subscription_ctx *tr, u64 task_id, enum NVMEIBT_RECOVERY_TYPE type, int err, int praid_version, bool should_send)
{
	__recovery_reject_reply(tr,     task_id,          type, err, praid_version, should_send);
}


static void __recovery_report_progress(struct nvmeibc_recovery *recov)
{
	//it does not matter who asks for progress, TOMA recovery owner will receive the progress
	struct nvmeibc_raid_recovery_pl pl = {0};
	pl.msg_type = NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS;
	__recovery_build_msg(&pl, recov, 0 /*no err, else recov would finish*/);
	if (recov->args.silent_mode || (__recovery_send_reply(recov->args.tr, &pl, (!recov->args.silent_mode)) >= 0))
			recov->report.n_attempts_since_msg = 0;		// Else, periodic update failed, retry it
}

static bool __recovery_cancel_locked(struct nvmeibc_recovery *recov, const struct nvmeibc_subscription_ctx *tr)
{
	const bool is_explicit_req_by_tr = (tr != NULL);
	if (recov->status != RCVR_READY) {
		recov->status = RCVR_CANCELING;		// When recovery finishes it will send a message
		if (!is_explicit_req_by_tr)
			tr = recov->args.tr;			// Need 'tr' for print. Use the tr on which recovery was launched
		_NTRR(tr_1_recov_cancel, "cancel task_id=@TASK_ID, explicit=@BOOL_YN", recov->args.task_id, is_explicit_req_by_tr);
		return true;
	}
	return false;	// Could not be canceled.
}

static inline void __print_incomming_msg_req(const struct nvmeibc_subscription_ctx *tr, enum NVMEIBT_CLIENT_MSG_TYPES msg_type, const struct nvmeibt_client_recovery_taskid_pl *ri) {
	DECL_FAKE_RECOVERY(ri->task.id, ri->task.type, tr);
	_NTRR(t_b1_crhr, "toma rqst msg=@MSG_TYPE", (u32)msg_type);
}

void nvmeibc_recovery_handle_request(const struct nvmeibc_subscription_ctx *tr, enum NVMEIBT_CLIENT_MSG_TYPES msg_type, const struct nvmeibt_client_recovery_taskid_pl *ri)
{
	unsigned long flags;
	struct nvmeibc_recovery *recov = NULL;
	const enum NVMEIBT_RECOVERY_TYPE rtype = nvmeibt_recovery_type_cast(ri->task.type);
	__print_incomming_msg_req(tr, msg_type, ri);								// Todo: Just for debug, all info exists in reply messages and the rest of the flow
	if (rtype == NVMEIBT_RECOVERY_TYPE_INVALID) {
		__recovery_query_send_reject(tr, ri, -10035); //unknown recovery type
		return;
	}

	recov = tr->hdr->recoveries[rtype];
	if (!recov){
		__recovery_query_send_reject(tr, ri, -10036); //recovery does not exist
		return;
	}

	spin_lock_irqsave(&recov->guard, flags);
	if (recov->status == RCVR_READY) {
		__recovery_query_send_reject(tr, ri, -10032); // No recovery is running
	} else if (recov->args.task_id != ri->task.id) {
		__recovery_query_send_reject(tr, ri, -10033); // Different recovery is running
	} else if (recov->type != ri->task.type){
		__recovery_query_send_reject(tr, ri, -10025); // Different recovery is running
	} else {
		if (msg_type == NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT) {
			if (!__recovery_cancel_locked(recov, tr /* explicit request */))
				__recovery_query_send_reject(tr, ri, -10034); // No recovery is running. Ack with cancel rejection msg
		} else { // Safe fallback to progress
			__update_effort_from_toma_msg(recov, &ri->task);
			WARN_ON(msg_type != NVMEIBT_CLIENT_MSG_TR_RECOVER_PING);
			__recovery_report_progress(recov);
		}
	}
	spin_unlock_irqrestore(&recov->guard, flags);
}

void nvmeibc_recoveries_handle_ioctl_request(const struct nvmeibc_subscription_ctx *tr, enum NVMEIBT_CLIENT_MSG_TYPES msg_type, const struct nvmeibt_client_recovery_taskid_pl *ri)
{
	if (likely(ri)) { // This is the main scenario,
		nvmeibc_recovery_handle_request(tr, msg_type, ri);
	} else {
		struct nvmeibc_recovery **recovs = tr->hdr->recoveries;
		struct nvmeibt_client_recovery_taskid_pl _ri = {{0}};
		u32 i, is_running;
		for (i = 0; i < NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES; ++i) {	// Find All non idle recoveries
			struct nvmeibc_recovery *recov = recovs[i];
			if (recov) {
				unsigned long flags;
				_ri.task.type = i;
				spin_lock_irqsave(&recov->guard, flags);
				_ri.task.id = recov->args.task_id;
				is_running = (recov->status != RCVR_READY);
				spin_unlock_irqrestore(&recov->guard, flags);
				if (is_running)
					nvmeibc_recovery_handle_request(tr, msg_type, &_ri);
			}
		}
	}
}

void nvmeibc_recoveries_cancel(struct nvmeibc_raid_topo_persistent *hdr)
{
	u32 i, n_recoveries = ARRAY_SIZE(hdr->recoveries);
	for (i = 0; i < n_recoveries; ++i){
		struct nvmeibc_recovery *recov = hdr->recoveries[i];
		if (recov){
			unsigned long flags;
			spin_lock_irqsave(&recov->guard, flags);
			__recovery_cancel_locked(recov, NULL /* implicit request, dont care about rv */);
			spin_unlock_irqrestore(&recov->guard, flags);
		}
	}
}

static const char *recov_status_to_string(struct nvmeibc_recovery *recov)
{
	const char *rv;
	switch (recov->status) {
		case RCVR_READY:	rv = "IDLE"; 	break;
		case RCVR_INIT:		rv = "INIT";	break;
		case RCVR_PROCESS:	rv = "RUN ";	break;
		case RCVR_CANCELING:rv = "CNCL";	break;
		default:			rv = "??? ";	break;
	}
	return rv;
}

//helper fuction to print recovery information to buffer
void nvmeibc_recovery_info_to_str(struct nvmeibc_recovery *recov, bool with_stats, struct nvmeib_txt *txt)
{
	u64 done = 0, total = 0;
	ulong flags;

	spin_lock_irqsave(&recov->guard, flags);
	if (recov->status != RCVR_READY) {
		nvmeib_txt_append(txt, "recov info: {status=%s, n_max_sw=%u}", recov_status_to_string(recov), recov->n_sw_to_use);
		nvmeib_txt_append(txt, "{task_id=0x%llx type=%d ver=%d}, ", recov->args.task_id,
				recov->type, recov->args.praid_version);
		nvmeib_txt_append(txt, "{Rlba=[%lld..[%lld..%lld)...%lld), apdx=%d, n_sw=%d, batch_size=%u, recov_ptr=%p}, ",
				RECOVERY_RLBA_BLKSTS_RANGE_4_u64(recov), recov->has_appendix_task, atomic_read(&recov->n_sw_running), recov->max_batch_size, recov);
		__recov_get_global_progress(recov, &done, &total);
		nvmeib_txt_append(txt, "{Progress: %lld/%lld}, ", done, total);
		if (with_stats){
			nvmeib_txt_append(txt, "{Job Stats: %lld/%lld}", recov->stats.n_success, recov->stats.n_total_tasks);
			nvmeib_txt_append(txt, "{Delayed jobs: %lld}", recov->stats.n_delayed_jobs);
		}
		nvmeib_txt_append(txt, "\n");
	}
	spin_unlock_irqrestore(&recov->guard, flags);
}

void nvmeibc_recovery_stats_to_str(struct nvmeibc_recovery *recov, struct nvmeib_txt *txt)
{
	ulong flags;
	spin_lock_irqsave(&recov->guard, flags);
	nvmeib_txt_append(txt, "{%s:%lld/%lld|n_sw=%d|n_dj=%lld}", nvmeibt_recov_type_to_3str(recov->type), recov->stats.n_success, recov->stats.n_total_tasks, recov->n_sw_to_use, recov->stats.n_delayed_jobs);
	spin_unlock_irqrestore(&recov->guard, flags);
}

void nvmeibc_recovery_stats_clear(struct nvmeibc_recovery *recov)
{
	ulong flags;
	spin_lock_irqsave(&recov->guard, flags);
	recov->stats.n_delayed_jobs = recov->stats.n_success = recov->stats.n_total_tasks = 0;
	spin_unlock_irqrestore(&recov->guard, flags);
}

static struct nvmeibc_recovery* __recovery_create(enum NVMEIBT_RECOVERY_TYPE rtype){
	struct nvmeibc_recovery *recov = kzalloc(sizeof(struct nvmeibc_recovery), GFP_KERNEL);
	if (recov){
		recov->type = rtype;
		spin_lock_init(&recov->guard);
		recov->status = RCVR_READY;
		recov->n_sw_to_use = RECOV_DFL_PARALLEL_SYNCS;
		//recov->stats - is initialized to zero - good enough
	} else {
		_NT(error_raid_recovery_recovery_create, "-ENOMEM");
	}
	return recov;
}

void nvmeibc_recovery_set_num_sw(struct nvmeibc_recovery *recov, u32 n)
{
	if (n == 0)
		recov->n_sw_to_use = RECOV_DFL_PARALLEL_SYNCS;
	else if (n <= RECOV_MAX_PARALLEL_SYNCS)
		recov->n_sw_to_use = n;
}

void nvmeibc_recovery_set_max_batch_size(struct nvmeibc_recovery *recov, u32 n)
{
	recov->max_batch_size = n;	// Todo: Add logic here
}

/*************************** Recovery implementation **************************/
int nvmeibc_recoveries_create(struct nvmeibc_raid_topo_persistent *hdr)
{
	int rv = 0, rcvr, num_rcvr = (int)ARRAY_SIZE(hdr->recoveries);
	for (rcvr = 0; rcvr < num_rcvr; ++rcvr){
		WARN_ON(hdr->recoveries[rcvr]);
		hdr->recoveries[rcvr] = __recovery_create((enum NVMEIBT_RECOVERY_TYPE)rcvr);
		if (!hdr->recoveries[rcvr]){
			rv = -ENOMEM;
			break;
		}
	}

	if (rv)
		nvmeibc_recoveries_destroy(hdr);
	return rv;
}

void nvmeibc_recoveries_destroy(struct nvmeibc_raid_topo_persistent *hdr)
{
	int rcvr = 0, num_rcvr = (int)ARRAY_SIZE(hdr->recoveries);
	for (rcvr = 0; rcvr < num_rcvr; ++rcvr){
		struct nvmeibc_recovery *recov = hdr->recoveries[rcvr];
		if (recov) {
			ulong flags;
			spin_lock_irqsave(&recov->guard, flags);
			if (recov->status != RCVR_READY) {
				WARN(true, "nvmeibc bug: %d, recovery still running\n", recov->status);
				/* Caller did not wait for recoveries to finish before this call,
				   expecting crash with NULL deref when recovery continues to run */
			}
			spin_unlock_irqrestore(&recov->guard, flags);
			kfree(recov);
		}
		hdr->recoveries[rcvr] = NULL;
	}
}

static void __recov_task_resources_free(struct nvmeibc_recovery *recov)
{
	int i;
	if (recov->sw){
		const char *name = __recov_worker0(recov).o.nd->name;
		for (i = 0; i < RECOV_MAX_PARALLEL_SYNCS; i++)
			nvmeibc_recov_sync_worker_free(&recov->sw[i], i);
		_NT(tr_1_recov_resource_free, "@DEV_NAME: free of recovery @RECOV num_sw=@RV", name, recov, RECOV_MAX_PARALLEL_SYNCS);
		kfree(recov->sw);
		recov->sw = NULL;
	}
}

static int __recov_task_resources_alloc(struct nvmeibc_recovery *recov,
										struct nvmeibc_subscription_ctx *tr)
{
	struct nvmeibc_block_device *nd = nvmeibc_block_nt_to_b(tr->nt);
	int rv = 0, i;
	if ((recov->sw = nvmeibc_recov_sync_worker_create_array()) == NULL) {
		rv = -ENOMEM;
		goto _out;
	}
	for (i = 0; i < RECOV_MAX_PARALLEL_SYNCS; i++) {
		if (nvmeibc_recov_sync_worker_init(&recov->sw[i], nd, recov) < 0) {
			rv = -ENOMEM;
			goto _out;
		}
	}
_out:
	if (rv) { /* Todo: Add cleanup here */}
	return rv;
}

/* Do we have next batch (regardless of whether we will execute it or not) */
static inline bool __has_next_batch(struct nvmeibc_recovery *recov)
{
	if (!recov->do_next_work_batch_cb)
		return false; // Batches are irrelevant
	if ((recov->b_start + recov->b_length) < recov->args.r_end)
		return true;  // Definitely has next batch
	if (recov->has_appendix_task) {
		recov->has_appendix_task = false;
		return true;
	}
	return false;
}

/* Must hold spinlock while calling this function and it will be released from this function! */
static void __recov_cleanup_locked_unlock(struct nvmeibc_recovery *recov, int err, ulong flags)
{
	struct nvmeibc_block_device *nd = nvmeibc_block_nt_to_b(recov->args.tr->nt);
	struct nvmeibc_recovery_chaining on_stack = recov->args.notify_caller;

	nvmeibc_recovery_itr_t_destroy(&recov->itr);
	__recov_args_destroy(&recov->args);
	__recov_work_batch_destroy(recov);
	__recov_task_destroy(recov);
	__recov_task_resources_free(recov);
	recov->report.n_attempts_since_msg = 0;
	recov->status = RCVR_READY;

	if (unlikely(rcvr_hooks && rcvr_hooks->on_finish))
		rcvr_hooks->on_finish(rcvr_hooks, recov);
	spin_unlock_irqrestore(&recov->guard, flags);
	if (on_stack.on_finish) {
		on_stack.on_finish(on_stack.ctx, err);	// Here 'recov' could already be relaunched by other toma msg
	}
	nvmeibc_recovs_drainer_dec(&nd->dp.running_recovs);	// now, when the recovery completely gone, we may update the device, Guratanteed tr/bdev exists
}

/* Called when recovery finished with error code 'err' */
void recovery_on_batch_finish(struct nvmeibc_recovery *recov, int err)
{
	struct nvmeibc_raid_recovery_pl pl;
	struct nvmeibc_recovery_args *args = &recov->args;
	struct nvmeibc_subscription_ctx *tr = args->tr;
	ulong flags;
	const bool has_next_batch = __has_next_batch(recov);
	const int n_proc = atomic_read(&recov->n_sw_running);

	WARN_RR(n_proc != 0, "n_proc=%d, status=%d\n", n_proc, recov->status);
	spin_lock_irqsave(&recov->guard, flags);
	if (has_next_batch) {
		if (!err) {							// Need next batch
			if (recov->status == RCVR_PROCESS /* Not cancelling */) {
				spin_unlock_irqrestore(&recov->guard, flags);
				BLKCMP_RC_ASYNC_RESUME_SYN(recov->do_next_work_batch_cb(recov));
				return;	// will resume recovery on next batch
			} else {	// Cur batch finished OK, But Cancel forces to skip next batch.
				err = -10010;
			}
		} else {}		// NOP: Cur batch finished with error, just abort recovery
	} else {}			// NOP: Already finished last batch (It is irrelecant if recovery was canceled or no, coz it already finished)
	WARN_RR((recov->status == RCVR_READY), "tid=0x%llx, ver=%d\n", args->task_id, args->praid_version);	// RCVR_INIT - Req first batch failed, RCVR_PROCESS/RCVR_CANCELING is valid
	recov->stats.n_total_tasks++;
	if (!err)
		recov->stats.n_success++;
	pl.msg_type = NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH;
	__recovery_build_msg(&pl, recov, err);
	if (!args->silent_mode) {
		__recovery_send_reply(tr, &pl, (!args->silent_mode));
	}
	_NIRR(tr_0_recov_batch_finish, "terminating recovery @EVENT_TAG ch=@INT, r1=@INT, set=@INT rv=@ERR",
	      EV_RAID_RECOV_FINISHED(), tr->ch, tr->r1, tr->seg, err);
	if ((recov->priv)&&(recov->priv_destructor))
		recov->priv_destructor(recov->priv);
	WARN_RR(recov->priv != NULL, "recovery memory leak of inherited %p, err=%d!", recov->priv, err);
	__recov_cleanup_locked_unlock(recov, err, flags);
}

static void __extract_worst_error_from_sync_workers_and_finish_batch(struct nvmeibc_recovery *recov)
{
	int i, rv = 0;
	for (i = 0; i < RECOV_MAX_PARALLEL_SYNCS; i++) {
		if (recov->sw[i].job.err < rv)			// Find the worst error of workers on batch.
			rv = recov->sw[i].job.err;			// Example: 1 worker succeded, one could not acquire topo, and 1 sow that recoveyr is being canceled
	}
	recovery_on_batch_finish(recov, rv);
}

static void __stop_sw(struct nvmeibc_recovery *recov, struct nvmeibc_recov_sync_worker *sw, int rv)
{
	int n_sw_running, i = (int)(sw-recov->sw);
	sw->job.err = rv;
	_NDRR(tr_1_sttop_sw, "sw=@INT stop, still running=@INT", i, atomic_read(&recov->n_sw_running) - 1);
	n_sw_running = atomic_dec_return(&recov->n_sw_running);		// here if (n_sw_running != 0) everything can get free. So we set rv before dec, otherwise this is data corruption
	WARN(n_sw_running < 0, "Recov, Wrong counting of workers, sw=%d stop, still running=%d!\n", i, n_sw_running);
	if (n_sw_running == 0) {
		BLKCMP_RC_ASYNC_RESUME_SYN(__extract_worst_error_from_sync_workers_and_finish_batch(recov));
	} // Else: dont touch, workers can get kfree(). The last sync that will finish do the above line
}

static void __sw_do_next_sync(struct nvmeibc_recovery *recov, struct nvmeibc_recov_sync_worker *sw)
{
	if (nvmeibc_recovery_itr_job_has_job(&sw->job)) {
		if (sw->job.jif_delay_before_work) {
			unsigned long flags;
			_NTRR(sw_delayed_blockset_hander, "Delaying next recovery job because of inefficiency for @JIFFIES[jif], ctx_ptr=@PTR", (unsigned long)sw->job.jif_delay_before_work, recov->itr.ctx);
			spin_lock_irqsave(&recov->guard, flags);
			recov->stats.n_delayed_jobs++;
			spin_unlock_irqrestore(&recov->guard, flags);
			schedule_delayed_work(&sw->dwork, sw->job.jif_delay_before_work); /* Do the same thing but later */
		} else {
			schedule_work(&sw->dwork.work); /* Reschedule for immediate execution */
		}
	} else {
		__stop_sw(recov, sw, 0); // This worker has no more work to do, it can go to sleep
	}
}

void recovery_on_batch_start(struct nvmeibc_recovery *recov)
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&recov->guard, flags);
	_NTRR(tr_1_on_batch_start, "Starting batch [@R_START..{[@B_START..@B_LENGTH)}...@R_END) only_ow=@BOOL, apdx=@BOOL",
		RECOVERY_RLBA_BLKSTS_RANGE_4_u64(recov), recov->args.do_only_owners, recov->has_appendix_task);
	WARN_ON(recov->b_start+recov->b_length > recov->args.r_end);
	WARN_RR((recov->status != RCVR_CANCELING)&&(recov->status != RCVR_PROCESS), "wrong status=%d", recov->status);	// (READY/INIT) Impossible, coz we already got batch for processing. Recovery wills stuck.
	if (recov->status == RCVR_PROCESS) {
		const int n_sw_to_use = (int)recov->n_sw_to_use;						// This amount will be launched for current batch. ioctl can change num of sw but this will apply only for next batch
		recov->itr.begin_cb(&recov->itr, &__recov_worker0(recov).job);
		spin_unlock_irqrestore(&recov->guard, flags);
		atomic_set(&recov->n_sw_running, n_sw_to_use);
		for (i = 0; i < n_sw_to_use; i++) {
			struct nvmeibc_recov_sync_worker *sw = &recov->sw[i];
			if (i>0) {
				WARN_ON(nvmeibc_recovery_itr_job_has_job(&sw->job));
				spin_lock_irqsave(&recov->guard, flags);
				recov->itr.next(&recov->itr, &sw->job);		// As if dummy work finished and next one is requested
				spin_unlock_irqrestore(&recov->guard, flags);
				_NDRR(tr_2_on_batch_start, "sw=@RV el=@INDEX_LLONG, @SLBA_BLKSETS", i, sw->job.cookie, sw->job.blkset_lba);
			}
			__sw_do_next_sync(recov, sw);
		}
	} else {
		spin_unlock_irqrestore(&recov->guard, flags);				// Note: here possibly: status != recov->status
		recovery_on_batch_finish(recov, -10036);
	}
}

/******************************* Recovery chaning *****************************/
static void __aux_recovery_finished_wakes_me_up(void *ctx, int err)
{
	struct nvmeibc_recovery *recov = ctx;
	if (err) {
		unsigned long flags;
		_NTRR(tr_1_aux_recov_wake, "skipped. Aux recov failed rv=@RV", err);
		spin_lock_irqsave(&recov->guard, flags);
		WARN_RR((recov->status != RCVR_INIT)&&(recov->status != RCVR_CANCELING),
			"Recovery chain bug status=%d\n", recov->status);
		recov->status = RCVR_CANCELING;
		spin_unlock_irqrestore(&recov->guard, flags);
	}
	__do_full_recovery_state_machine(recov);
}

/* Todo: The 2 functions below should be probably extended and exported via .h
   file, to allow chaining not only recoveries but other components + changing
   contexts.
   Chaining recoveries can be done in 2 ways:
   1. Current implementation: Recov simulates toma message from aux and accesses
    	Aux during external API (same as Toma request)
		Note: ioctls use the same approach even though it might not be optimal
   2. Internal API: Recov/ioctls use directly __try_setup_recovery_check_busy()
	    And bypass creation of toma message and conversion back.
   */
static struct nvmeibt_client_recovery_start_pl __simulate_toma_msg_for_aux_recovery(struct nvmeibc_recovery *recov)
{
	const struct nvmeibt_client_recovery_start_pl ri = {
		.task = {
			.id =   (recov->args.task_id ^ (1ULL<<63)),				// Same as my task id but with special bit marking this is auxilliary recovery
			.type = NVMEIBT_RECOVERY_TYPE_INVALID,					// Will be filled later
			.effort_percents = __effort_percents_from_nsw(recov),	// Same effort as me
			.max_batch_size = recov->max_batch_size,
		},
		.is_mandatory =		recov->args.is_mandatory,
		.do_only_owners =	recov->args.do_only_owners,
		.start_lock =		recov->args.r_start,
		.num_locks =		(recov->args.r_end - recov->args.r_start),
	};
	return ri;
}

static int __nvmeibc_recovery_start(struct nvmeibc_subscription_ctx *tr, struct nvmeibc_raid1 *r1, struct nvmeibt_client_recovery_start_pl *ri, int praid_version, bool silent_mode, const struct nvmeibc_recovery_chaining *ch);
static void __aux_recovery_call_if_needed_or_do_first_batch(struct nvmeibc_recovery *recov)
{
	int rv = 0, was_aux_recov_lanched = false, aux_recov_needed = false;// By default no need to run anything
	const struct nvmeibc_recovery_chaining ch = {
			.ctx = recov, .on_finish = __aux_recovery_finished_wakes_me_up, };
	struct nvmeibc_topology *topo = NULL;

	if (recov->type != NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD)
		goto _out;
	if (recov->args.dconv.w_is_dirty_segs_bmp == 0)
		goto _out;														// Cases where aux recovery is not needed

	if (1) {															// Supported in R1 and EC
		struct nvmeibt_client_recovery_start_pl ri = __simulate_toma_msg_for_aux_recovery(recov);
		struct nvmeibc_raid1 *pr;

		aux_recov_needed = true;
		ri.task.type = NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON;
		rv = recovery_get_topo(recov);
		if (rv)
			goto _out;
		topo  = __recov_worker0(recov).o.topo;
		pr = recovery_topo_pr_ptr(recov);
		__sync_worker_clean_topo(&__recov_worker0(recov));				// Disconnect topology from 'recov' to be able to free it even if aux recov gives callback very fast
		rv = __nvmeibc_recovery_start(recov->args.tr, pr, &ri, recov->args.praid_version, true /*silent*/, &ch);
		if (rv)
			goto _out;
		was_aux_recov_lanched = true;
	}

_out:
	nvmeibc_topology_put(topo);						// Put if needed
	if (!was_aux_recov_lanched) {
		if (aux_recov_needed)
			rv = -10050;							// For debug: Special mark for launch failure
		ch.on_finish(ch.ctx, rv);          			// As if auxilirary recovery finished
	} else { /* NOP: Async callback will arrive */}
}

/******************************************************************************/
#define try_rv(expression) if ((rv = (expression)) < 0) goto _out
static void __recovery_launch(struct nvmeibc_recovery *recov)
{
	int rv = 0;
	const bool is_mandatory = recov->args.is_mandatory;
	const bool only_owners = recov->args.do_only_owners;
	NFIN;
	if (unlikely(rcvr_hooks && rcvr_hooks->on_launch))
		rcvr_hooks->on_launch(rcvr_hooks, recov);

	switch (recov->type) { // Todo: Convert this switch to virtual functions
	case NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON:
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:
		try_rv(nvmeibc_recovery_itr_t_init_blocksets(&recov->itr, is_mandatory));
		recov->do_next_work_batch_cb = __get_blksets_info_next_work_batch;
		goto _switch_success;
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
		WARN((only_owners || is_mandatory), "GC - must do all segs and not be mandatory");
		FALLTHRU;
	case NVMEIBT_RECOVERY_TYPE_EC_COLD :
		try_rv(nvmeibc_recovery_itr_init_cold_candidates(&recov->itr, is_mandatory));
		recov->do_next_work_batch_cb = nvmeibc_block_dp_ec_recov_cold_start;
		goto _switch_success;
	case NVMEIBT_RECOVERY_TYPE_VOID_DUMMY:
		try_rv(nvmeibc_recovery_itr_t_init_blocksets(&recov->itr, is_mandatory));
		recov->do_next_work_batch_cb = __dummy_empty_recov_batch;
		goto _switch_success;
	case NVMEIBT_RECOVERY_TYPE_INVALID:
	case NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES:
	case NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE:
		// Failure
		break;
	}
	WARN(1, "nvmeibc bug! unknown recovery %d\n", recov->type);
	try_rv(-EINVAL);
_switch_success:
	__aux_recovery_call_if_needed_or_do_first_batch(recov);
	recov = NULL;		// Dont touch it here. It is running asyncronously
_out:
	if (rv)
		recovery_on_batch_finish(recov, -ENOEXEC); // Recovery was not executed
	NFOUT;
}

static __attr_no_alignment_sanity int __find_cold_surviving_ram_bmp(struct nvmeibc_raid1 *r1, struct nvmeibt_client_recovery_start_pl *ri, u16 *surviving_ram_bmp)
{
	int rv = 0;
	*surviving_ram_bmp = ri->cold.surviving_ram_bmp;
	(void)r1;
	if (0)	/* Todo: Here verify correctnes vs topology and amount of bits */
		rv = -EINVAL;
	return rv;
}

static void __recov_thread_context_alloc_launch(struct work_struct *w)
{
	struct delayed_work *dwork = container_of(w, struct delayed_work, work);
	struct nvmeibc_recovery *recov = container_of(dwork, struct nvmeibc_recovery, dwork);
	struct nvmeibc_subscription_ctx *tr = recov->args.tr;
	int rv = __recov_task_resources_alloc(recov, tr);
	if (unlikely(rv)) {
		ulong flags;
		struct nvmeibc_recovery_args t = recov->args;	// Cache on stack!
		const enum NVMEIBT_RECOVERY_TYPE rtype = recov->type;
		spin_lock_irqsave(&recov->guard, flags);
		WARN((recov->status != RCVR_INIT)&&(recov->status != RCVR_CANCELING), "nvmeibc bug! st=%d ti=0x%llx\n", recov->status, t.task_id);
		rv = -10003;
		__recov_cleanup_locked_unlock(recov, rv, flags);
		__recovery_start_send_reject(t.tr, t.task_id, rtype, rv, t.praid_version, !t.silent_mode);
	} else {
		recov->priv = NULL;			// Ready for inheritance
		__recovery_launch(recov);	// Treats failures internally
	}
}

/* If not busy start recovery, otherwise, fail */
static int __try_setup_recovery_check_busy(struct nvmeibc_recovery *recov, struct nvmeibc_recovery **recovs, const struct nvmeibc_recovery_args *args)
{
	struct nvmeibc_subscription_ctx *tr = args->tr;
	int rv = 0;
	ulong flags;
	spin_lock_irqsave(&recov->guard, flags);
	if (unlikely(recov->status != RCVR_READY)) {		// Verify self not running
		const struct nvmeibc_recovery_args *i = &recov->args;
		_NTRR_ARGS(recov_check_busy_self, tr, recov->type, args->task_id, "Cannot start recov before previous finished (ti=@TASK_ID, seg=@SI)", i->task_id, i->tr->seg);
		if (args->task_id==i->task_id) {
			rv = -EALREADY;		// Toma probably did not hear our pings for long time and restared the recovery?
			goto _out;
		} else {
			goto _busy;		// Must reject
		}
	}
	if (recov->type == NVMEIBT_RECOVERY_TYPE_EC_COLD) {	// Verify no other recovery is running when cold is launched. JGC recovery would be disastrous becuase it will lead to data corruption
		enum NVMEIBT_RECOVERY_TYPE t;
		for (t = NVMEIBT_RECOVERY_TYPE_INVALID+1; t < NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES; t++) { // Daniel: Todo: Correct solution, do atomic counter on amount of recoveries and existance of cold? Maybe there are more exclusive recoveries?
			const volatile enum e_recov_status *other_status = &recovs[t]->status;
			if (*other_status!= RCVR_READY) {
				_NTRR_ARGS(recov_check_busy_cold_vs, tr, recov->type, args->task_id, "Attempt to start recovery during @TR_RECOV_TYPE", recovs[t]->type);
				if (*other_status == RCVR_CANCELING)
					goto _busy;
				goto _busy_corruption;
			}
		}
	} else {											// Verify no cold recovery is running right now
		const enum NVMEIBT_RECOVERY_TYPE t = NVMEIBT_RECOVERY_TYPE_EC_COLD;
		const volatile enum e_recov_status *cold_status = &recovs[t]->status;
		if (*cold_status!= RCVR_READY) {
			_NTRR_ARGS(recov_check_busy_vs_cold, tr, recov->type, args->task_id, "Attempt to start recovery during @TR_RECOV_TYPE", recovs[t]->type);
			if (recov->type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC){
				//In order to verify that JGC may be executed, the simulator should scan all TOMAs,
				//which participate in the RAID, and decided whether it could be run or not; right now this functionality does not exist
				goto _busy;
			} else {
				if (*cold_status == RCVR_CANCELING)
					goto _busy;
				goto _busy_corruption;
			}
		}
	}
	recov->args = *args;
	nvmeibc_recovery_itr_t_init_null(&recov->itr);
	recov->status = RCVR_INIT;
_out:
	spin_unlock_irqrestore(&recov->guard, flags);
	return rv;

_busy:						// Cant start recovery
	rv = -EBUSY;
	goto _out;
_busy_corruption:			// Cant start recovery, bug in the system
	WARN(true, "nvmeibc bug. Wrong recovery %d timining", recov->type);
	goto _busy;
}

static  __attr_no_alignment_sanity int __nvmeibc_recovery_start(struct nvmeibc_subscription_ctx *tr, struct nvmeibc_raid1 *r1, struct nvmeibt_client_recovery_start_pl *ri, int praid_version, bool silent_mode, const struct nvmeibc_recovery_chaining *ch)
{
	int rv = -ENOEXEC, err_to_toma = -10001;					// Default: Cant start value
	struct nvmeibc_recovery **recovs = tr->hdr->recoveries;
	struct nvmeibc_recovery *recov = NULL;
	const bool is_ec = nvmeibc_raid_is_ec(r1);

	struct nvmeibc_recovery_args rcvr_args = {
		.task_id = ri->task.id,
		.tr = tr,
		.praid_version = praid_version,
		.silent_mode = silent_mode,
		.do_only_owners = (!!ri->do_only_owners),
		.is_mandatory = (!!ri->is_mandatory),
		.r_start = ri->start_lock,
		.r_end = ri->start_lock + ri->num_locks,
	};
	rcvr_args.dconv.w_is_dirty_segs_bmp = nvmeibc_raid1_get_sgmnts_bmp(r1, wm);

	/* Verify parameters */
	if ((ri->task.type <= NVMEIBT_RECOVERY_TYPE_INVALID) || (ri->task.type >= NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES))
		goto _reject_reply_to_toma;

	recov = recovs[ri->task.type];

	_NIRR_ARGS(tr_0_recov_start_agrs, tr, recov->type, ri->task.id,
		"@EVENT_TAG tr=@TR, [start_@SLBA_BLKSETS length_@SLBA_BLKSETS], @T_PRV recov=@RECOV, is_mandatory=@BOOL, do_only_owners=@BOOL, conv=@BITMAP",
		EV_RAID_RECOV_STARTED(), tr, ri->start_lock, ri->num_locks, praid_version, recov, ri->is_mandatory,
		ri->do_only_owners, rcvr_args.dconv.w_is_dirty_segs_bmp);

	if ((ri->num_locks == 0) || (ri->start_lock + ri->num_locks > (tr->length/LOCKSET_SLICES)))	// Note: tr->length != nvmeibc_raid_get_slba_len(r1), because unitest plays with 'tr' length in some tests
		goto _reject_reply_to_toma;

	if ((!recov) ||
		(tr->nt->io_perm == NVMEIB_IO_TYPE_PERMIT_NONE_SUS) ||
		((ri->task.type == NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON) && (rcvr_args.dconv.w_is_dirty_segs_bmp == 0)) ||
		((ri->task.type == NVMEIBT_RECOVERY_TYPE_EC_COLD)            && __find_cold_surviving_ram_bmp(r1, ri, &rcvr_args.cold.surviving_ram_bmp)) ||
		((ri->task.type == NVMEIBT_RECOVERY_TYPE_SCRUBBING)          && (nvmeibc_raid1_get_inverse_sgmnts_bmp(r1, readable) != 0)) ||
		((ri->task.type == NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD)      && (nvmeibc_raid1_get_num_dead_seg(r1) >= nvmeibc_raid1_get_protect_lvl(r1)))) {
		goto _reject_reply_to_toma;
	}

	if (nvmeibc_raid_is_jbod(r1)) {
		//if (ri->task.type != NVMEIBT_RECOVERY_TYPE_SCRUBBING) {
		err_to_toma = -10008; // all sorts of recoveries are irrelevant for jbods
		goto _reject_reply_to_toma;
		//}
	}

	if ((is_ec)&&(ri->task.type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD)) {
		rcvr_args.is_mandatory = true;									// For Toma this is not mandatory but for client it is because this stale lock holds journals that are needed for other volumes
	}
	rv = __try_setup_recovery_check_busy(recov, recovs, &rcvr_args);
	if (rv) {
		if (rv == -EALREADY) {
			return rv; // __recovery_report_progress(recov); // Do nothing. We are already running, reports will reach toma anyways
		} else {
			err_to_toma = -10051;    // Already running, cannot start
			goto _reject_reply_to_toma;
		}
	}
	if (ch)
		recov->args.notify_caller = *ch;
	recov->b_start = recov->args.r_start;
	recov->is_ec_raid = is_ec;
	recov->report.threshold = N_SYNCS_FOR_PROGRESS(r1);			// Daniel, consider adding ioctl which controls this in run-time and define precisely by recovery type. RAM recovery is much faster

	__update_effort_from_toma_msg(recov, &ri->task);
	nvmeibc_recovs_drainer_inc(&nvmeibc_block_nt_to_b(tr->nt)->dp.running_recovs);	// Guratanteed that bdev did not start draining recoveries, because we hold a valid topology so it is safe to ++
	INIT_DELAYED_WORK(&(recov->dwork), __recov_thread_context_alloc_launch);
	BLKCMP_ANY_schedule_delayed_work(&recov->dwork, DELAY_RECOVERY_START_BY_N_SEC);
	return 0;

_reject_reply_to_toma:
	__recovery_start_send_reject(tr, ri->task.id, ri->task.type, err_to_toma, praid_version, (!silent_mode));
	return rv;
}

int nvmeibc_recovery_start(struct nvmeibc_subscription_ctx *tr, struct nvmeibc_raid1 *pr, struct nvmeibt_client_recovery_start_pl *ri, int praid_version, bool silent_mode)
{
	return __nvmeibc_recovery_start(tr, pr, ri, praid_version, silent_mode, NULL);
}

int nvmeibc_recovery_start_ioctl(struct nvmeibc_subscription_ctx *tr, struct nvmeibc_raid1 *pr, struct nvmeibt_client_recovery_start_pl *ri)
{
	return __nvmeibc_recovery_start(tr, pr, ri, pr->version, true /*silent*/, NULL);
}

int nvmeibc_recovery_start_ext(struct nvmeibc_subscription_ctx *tr, struct nvmeibc_raid1 *pr, struct nvmeibt_client_recovery_start_pl *ri)
{
	return __nvmeibc_recovery_start(tr, pr, ri, pr->version, false /*!silent*/, NULL);
}

// Called when recovery of sigle lock set block has finished. Type: nvmeibc_sync_cb_t
static int __on_finish_one_sync_cb(void *sw_context, int err)
{
	struct nvmeibc_recov_sync_worker *sw = sw_context;
	struct operation *o = &sw->o;
	struct nvmeibc_recovery *recov = o->vro;
	unsigned long flags;

	WARN_RR(!o->topo, "topo must be taken for sync, sw=%d\n", (int)(sw-recov->sw));
	nvmeibc_recov_sync_worker_put_topo(sw);
	// Here: We dont hold topology. So volume might become suspended on detach
	// True, but attach/detach still need to wait for recovery cancel complete
	// otherwise we should not use recov pointer - it is managed by topology
	if (unlikely(rcvr_hooks && rcvr_hooks->on_finish_one_sync))
		rcvr_hooks->on_finish_one_sync(rcvr_hooks, recov);

	sw->job.err = err;
	spin_lock_irqsave(&recov->guard, flags);
	if (recov->status == RCVR_CANCELING) {
		spin_unlock_irqrestore(&recov->guard, flags);
		__stop_sw(recov, sw, -ENOEXEC);
	} else {
		const bool should_send = ((++recov->report.n_attempts_since_msg) >= recov->report.threshold);
		WARN_RR(recov->status != RCVR_PROCESS, "status=%d\n", recov->status);
		_NDRR(tr_1_sync_finish_cb, "sw=@RV, el=@INDEX_LLONG err=@ERR", (int)(sw-recov->sw), sw->job.cookie, err);
		recov->itr.next(&recov->itr, &sw->job);
		spin_unlock_irqrestore(&recov->guard, flags);
		if ((should_send)&&(sw == &__recov_worker0(recov)))	// Only first worker will report (to not clog toma)
			__recovery_report_progress(recov);
		BLKCMP_RC_ASYNC_RESUME_SYN(__sw_do_next_sync(recov, sw));
	}
	return 0;
}

/* Asyncrously skip blockset and mark it as success if (o_rv==0) or failure*/
static void __skip_blockset_cb(struct workqe_struct *w)
{
	struct measured_work *mw = measured_work_from(w);
	struct operation *o = container_of(mw, struct operation, work_skip_recov);	// Can use: struct nvmeibc_disk_io_command *iocmd = container_of(w, struct nvmeibc_disk_io_command, auto_fail_work); struct nvmeibc_block_command *cmd = iocmd->disk_cmd.owner; o = cmd->o;
	nvmeib_wq_metrics_update(nvmeibc_skip_recov_wq_latency, measured_work_wait_ticks(mw));
	__on_finish_one_sync_cb(__sync_worker_of_o(o), o->cmds->o_rv);
}

static void __schedule_skip_blockset(struct operation *o, int err)
{
	struct measured_work *mw = &o->work_skip_recov;
	o->cmds->o_rv = err;
	MEASURED_INIT_WORK(mw, __skip_blockset_cb);
	BLKCMP_ANY_dp_block_schedule_operation_work(o, &mw->work);
}

static void __call_sync_vfunc_internal(struct nvmeibc_recov_sync_worker *sw);

TIMER_CALLBACK(__call_sync_vfunc, struct nvmeibc_d_rdma_comp, retry_timer, struct nvmeibc_recov_sync_worker, sw)
	__call_sync_vfunc_internal(sw);
}

static void __call_sync_vfunc_internal(struct nvmeibc_recov_sync_worker *sw)
{
	struct operation *o = &sw->o;
	struct nvmeibc_recovery *recov = o->vro;
	const struct nvmeibc_subscription_ctx* tr = recov->args.tr;
	struct nvmeibc_cmd_lock *l = &o->locks[0];
	enum stale_lock_resolve_status ss = stale_lock_resolve_safe_to_use;
	const u64 holder = nvmeibc_d_rdma_comp_get_contending_id(&l->comp).all;

	if (( nvmeibc_sync_is_stale(    l, holder)) &&
		(!nvmeibc_sync_is_read_only(l, holder))) {
		ss = stale_lock_resolver_get_status(&tr->hdr->slr, holder, l);
	}
	if (ss == stale_lock_resolve_safe_to_use) { // For stale lock start only if resolved, otherwise ss is initially safe
		int err = sw->fn(l, __on_finish_one_sync_cb, sw);
		if (err) {
			_NTRR(trace_0_sync_vfunc, "Failed launching sync: seg=@SEG, @DLBA", l->ds->uuid, l->address);
			__schedule_skip_blockset(o, err);
		}
	} else { // Handle unresolved stale blockset
		const bool random_50per_chance = ((get_random_u32()&0x1) == 0);
		_NTRR(trace_1_sync_vfunc,"Delayed stale: act=@ACT, lid=@LOCK_ENT_U64, retries=@RETRIES, seg=@SEG, @DLBA_BLKSETS",
			random_50per_chance, holder, l->retries, l->ds->uuid, l->address/LOCKSET_SLICES);
		if ((random_50per_chance)||(o->topo->phased_out)) {
			__schedule_skip_blockset(o, -10015 /* no recoveree uuid */);
		} else {					// Retry this funcion a bit later
			const ulong retry_time __attribute__((unused)) = dp_locks_get_retry_time(l);	// Profiler add retry to sync profiler
			struct nvmeibc_d_rdma_comp *dc __attribute__((unused)) = &l->comp;
			dp_locks_activate_timer(dc, __call_sync_vfunc, sw, retry_time);
		}
	}
}

static int __recovery_get_topo(struct nvmeibc_recovery *recov, struct nvmeibc_recov_sync_worker *sw)
{
	struct operation *o = &sw->o;
	const struct nvmeibc_subscription_ctx* tr = recov->args.tr;
	int rv = 0;

	WARN((o->topo || !tr), "nvmeibc bug. %p, %p", o->topo, tr);
	o->topo = nvmeibc_topology_get(tr->nt);
	if (unlikely(o->topo == NULL)) {
		rv = -10037;
		_NTRR(trace_0_acquire_topo, "Detaching. Cancelling");
		goto _out;
	}
	if (nvmeibc_topo_is_in_err_state(o->topo)) {
		if ((o->topo->io_perm == NVMEIB_IO_TYPE_PERMIT_NONE_SUS) ||
			(nvmeibc_raid1_get_io_perm(o->topo, tr->ch, tr->r1) <= NVMEIB_IO_TYPE_PERMIT_NONE_ERR)) {
			rv = -10038;
			_NTRR(trace_1_acquire_topo, "Abort, io_perm=@IO_PERM", o->topo->io_perm);
			goto _out;
		} else {} // NOP: topology in error state, but the raid is ioable
	}
	if (1) {	// Verify that topology is not newer than recovery task, or else we might not be able to complete the task
		sw->pr = __get_r1_by_tr(o->topo, tr);
		if (unlikely(sw->pr->version != recov->args.praid_version)) {
			_NTRR(trace_2_acquire_topo, "Topo-changed. Cancelling: cur_@C_PRV != recov_@C_PRV",
				sw->pr->version, recov->args.praid_version);
			rv = -10045;
			WARN_RR(sw->pr->version < recov->args.praid_version, "bug: praid_version went backwards, was=%u now %u", recov->args.praid_version, sw->pr->version);
			// Version went backwards??? Daniel: Todo, remove, just sanity
		}
	}
	if (likely(rv == 0 && o->topo)){
		_NDRR(trace_3_acquire_topo, "o=@OPERATION @TOPOLOGY @TOPO_DBG_ID", o, o->topo, o->topo->debug_unique_index);
	}
_out:
	if (unlikely((rv != 0)&&(o->topo)))
		nvmeibc_recov_sync_worker_put_topo(sw);	// coz caller will not do that
	return rv;
}

struct nvmeibc_raid1 *recovery_topo_pr_ptr(struct nvmeibc_recovery *recov){          return __recov_worker0(recov).pr;}
int  recovery_get_topo(struct nvmeibc_recovery *recov){ return __recovery_get_topo( recov, &__recov_worker0(recov));}
void recovery_put_topo(struct nvmeibc_recovery *recov){ nvmeibc_recov_sync_worker_put_topo(&__recov_worker0(recov));}

/* process next blockset by worker. */
static void __recover_next_blockset(struct nvmeibc_recov_sync_worker *sw)
{
	int rv = 0, ow_seg;
	struct operation *o = &sw->o;
	struct nvmeibc_recovery_itr_job *job = &sw->job;
	struct nvmeibc_recovery *recov = o->vro;
	struct nvmeibc_d_rdma_comp *dc;
	struct nvmeibc_raid1 *r1;
	const struct nvmeibc_subscription_ctx* tr = recov->args.tr;
	u64 rlba, lock_initial_val = 0; // Default lock is expected to be 0

	//__dump_operation(o);
	if ((rv = __recovery_get_topo(recov, sw)) != 0 )
		goto _out;

	/* Calculate the next rlba of blockset to process and verify it */
	r1 = sw->pr;
	rlba = job->blkset_lba * LOCKSET_SLICES * r1->slice_size;
	ow_seg = get_owner_seg_of_lock(r1, rlba);
	if (unlikely(rlba >= (tr->length*r1->slice_size))) {		// Note: tr->length != seg->length, because unitest plays with 'tr' length in some tests
		rv = -10039;	// How ??? sync outside of raid. Should never happen!!!
		WARN_RR(true, "nvmeibc bug: Abort, rlba=%llu\n", rlba);
		goto _out;
	}
	if (recov->args.do_only_owners && (ow_seg != tr->seg)) { // Skip this blockset
		__schedule_skip_blockset(o, 0 /* No error */);
		return;
	}

	/* Add locks to protect given raid */
	o->locks->nlocks = dp_fill_locks_for_raid(r1, o->op, rlba, o->locks);
	o->cmds->ds = o->locks[0].ds;
	o->locks[0].comp.code = NVMEIBC_CMD_LOCK_OWNER;
	dc = &o->locks[0].comp;
	dc->exchange = r1->lid.all;	// Simulate as if tried to acquire lock but failed

	/* Fine tune the input arguments to the sync, Todo, unify the if's below*/
	sw->fn = nvmeibcbdpec_sync_get_fn_by_rtype(recov->type);	// Set sync func to default

	switch (recov->type) {
		case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD: {
			if (!recov->is_ec_raid) {
				const union nvmeib_blkset_problem_report problem = job->aux.blckset_problem;
				WARN_RR(!problem.is_stale, "nvmeibc bug, slba[blksets]=0x%llx, wrong problem=0x%x\n", job->blkset_lba, problem.all);
				lock_initial_val = R1_STALE_SPECIAL_BINFO_VAL; // Reduce priobability to retry lock
			} else {
				lock_initial_val = job->aux.stale_lock; // EC: reduce priobability to retry lock by supplying the expected stale lock
			}
			_NDRR(tr_0_recov_next, "Fix @SLBA_BLKSETS, ST lock=@LOCK_ENT_U64", job->blkset_lba, lock_initial_val);
		} break;
		case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD: {
			const union nvmeib_blkset_problem_report problem = job->aux.blckset_problem;
			if (!recov->is_ec_raid) {
				if (problem.is_stale) {
					lock_initial_val = R1_STALE_SPECIAL_BINFO_VAL; // Reduce priobability to retry lock
					if (problem.dbits == 0) {
						sw->fn = nvmeibc_sync_commit_stale_lock;
					}
				}
			} else {
				if (problem.is_stale) {
					// sw->fn = nvmeibc_sync_fix_stale;		// Todo: Covnert to stale sync - today we call dbits_rebuild sync which will mutate to stale lock recovery.
				} else if (problem.dbits) {
					// Just launch regular dbits sync
				} else if (problem.binfo_not_commited) { // Verify if it is indeed not commited. For more info, see documentation of __uncommited_binfo_inject_to_blkset_problem()
					sw->fn = nvmeibc_sync_commit_binfo;
					//o->locks->commit_only_owner_binfo = true; // The sync will take the best case of all of the binfos, although it's enough to take the owner binfo.
					/* if (__uncommited_binfo_detect_case_1(o->locks)) {
						sw->fn = nvmeibc_sync_commit_binfo; // Just commit blockset info
					} else {		// Case 2. with current implementation also need to commit binfo so this code is commented out
						__schedule_skip_blockset(o, 0 / * No error * /); // Blockset has a 'W' segment but all locks segs are RW.
						return;
					} */
				} else {
					WARN_RR(true, "unsuported code, wrong problem=0x%x\n", problem.all);
					BUG();
				}
			}
			_NDRR(tr_1_recov_next, "Fix @SLBA_BLKSETS, DB {db=@DBITS, is_st=@BOOL_YN, not_commited=@BOOL_YN}", job->blkset_lba, problem.dbits, problem.is_stale, problem.binfo_not_commited);
		} break;
		case NVMEIBT_RECOVERY_TYPE_EC_COLD:	//on cold recovery restart, Toma should clean the locks once again. Consider use case: the block was fixed, but some error prevented cold recovery to free the lock; Tests should be added
		case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC: // Locks dont exist (all zero), but has an aux cadidates
 			o->locks->user_data = (void *)job->aux.blckset_candidates_ptr;
			_NDRR(tr_2_recov_next, "Fix @SLBA_BLKSETS, {@USER_DATA}", job->blkset_lba, o->locks->user_data);
			break;
		case NVMEIBT_RECOVERY_TYPE_VOID_DUMMY:
			sw->fn = __dummy_sync_cb_fn;
			break;
		case NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON:
		case NVMEIBT_RECOVERY_TYPE_SCRUBBING: // Nothing to do in here on case of a problem mutate.
		case NVMEIBT_RECOVERY_TYPE_INVALID:
		case NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES:
		case NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE:
		case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:
			break;
	}
	nvmeibc_d_rdma_comp_set_lock_id(dc, (union nvmeib_lock_id){ .all = lock_initial_val });
	__call_sync_vfunc_internal(sw);
_out:
	if (unlikely(rv)) {
		nvmeibc_recov_sync_worker_put_topo(sw);	// coz sync cb will not do it.
		__stop_sw(recov, sw, rv);
	}
}

static void __recover_next_blockset_delayed_handler(struct work_struct *work) {
	struct nvmeibc_recov_sync_worker *sw = container_of(work, struct nvmeibc_recov_sync_worker, dwork.work);
	__recover_next_blockset(sw);
}
