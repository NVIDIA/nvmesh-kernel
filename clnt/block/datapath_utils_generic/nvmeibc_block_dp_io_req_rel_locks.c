#include "nvmeibc_block_dp_io_req_rel_locks.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeibc_block_dp_dbg_tools.h"
#include "nvmeibc_block_dp_operation.h"
#include "nvmeibc_pausable.h"
#include "block/recovery/nvmeibc_block_dp_sync_api.h"
#include "block/nvmeibc_block_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_profiling_lock_stages.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.h"
#include "nvmeibc_io_pet.h"

#define __SUSPICIOUS_LOCK_REQ_TIME 500 /* 0.5[sec], If lock request takes more time, print that to log */
#define __SUSPICIONS_LOCK_TIME   20000 /*  20[sec]  , If lock finally acquired but it took a lot of time, print that to log */
#define __COMPLAIN_LOCK_TIME     12000 /*  12[sec]  , If lock was not acquired for more then X[sec] from last complain, complain to toma */
unsigned warn_if_lock_took_more_than_n_msec = __SUSPICIONS_LOCK_TIME;
module_param(warn_if_lock_took_more_than_n_msec, uint, 0644);
MODULE_PARM_DESC(warn_if_lock_took_more_than_n_msec, "If lock acquisition takes more than X[msec], issue warning to log");

#ifdef DEBUG_CONTENDED_LOCKS
unsigned warn_if_lock_held_more_than_n_msec = __SUSPICIONS_LOCK_TIME / 4;
module_param(warn_if_lock_held_more_than_n_msec, uint, 0644);
MODULE_PARM_DESC(warn_if_lock_held_more_than_n_msec, "If lock held more than X[msec], issue warning to log");
#endif

/* YR: TODO: should be parameters received via some management system */
#define MAX_RETRY_DELAY 	(unsigned)(100*1000)	/* in microseconds */
#define DEFAULT_RETRY_MULT			  (5000)		/* in microseconds */
unsigned lock_retry_delay_multiplier = DEFAULT_RETRY_MULT;
module_param(lock_retry_delay_multiplier, uint, 0644);
MODULE_PARM_DESC(lock_retry_delay_multiplier, "Quadratic backoff retry with this factor [usec]");

/* Formula : delay_time  = func(retry_count) * multiplier + random
 * note that the code arithmetic is in [usec] but the actual delay is based on
 * the kernel timers which use a granularity of HZ
 * since HZ is usually no more than 1000, the delay is in [msec]
 * in the future, consider having our own retry mechanism  to allow a more
 * fine grained retry time */
ulong dp_locks_get_retry_time(struct nvmeibc_cmd_lock *l)
{
	const u32* retries = &l->retries;
	ulong random_value, delay, lul, delay_jiffies;
	/* Generate random value [1..63], from address and value of 'retries' */
	lul = (unsigned long)retries;
	random_value = 1 + ((0x3f) & (lul >> (8 + (*retries & 0xf))));
	delay = (((*retries + 1) * (*retries + 2))/2); // quadratic backoff
	delay = delay * lock_retry_delay_multiplier + random_value;
	if (delay > MAX_RETRY_DELAY) {
		delay = MAX_RETRY_DELAY;
	}
	nvmeibc_profiling_add_retry_count_and_delay(l->ds->lock_operation_profiler, delay, l->type, *retries);
	delay_jiffies = max((unsigned)usecs_to_jiffies(delay), (unsigned)1);
	#if defined(BLKDEV_SIMULATOR)
		delay_jiffies = random_value;	// Everything is local, very short retries
	#endif
	return jiffies + delay_jiffies;
}

const char* ncl_status_str(const enum nvmeibc_block_lock_status s)
{
	switch (s) {
	case NCL_STATUS_INVALID: 			return "INVALID";
	case NCL_STATUS_NOTISSUED:			return "NOT_ISSUED";
	case NCL_STATUS_ISSUED:				return "ISSUED";
	case NCL_STATUS_TAKEN:				return "TAKEN";
	case NCL_STATUS_CONTENDED:			return "CONTENDED";
	case NCL_STATUS_DONE:				return "DONE";
	case NCL_STATUS_FAIL_COMP:			return "FAIL_COMP";
	case NCL_STATUS_FAIL_NO_COMP:		return "FAIL_NCOMP";
	case NCL_STATUS_DISKDEAD:			return "DISK_DEAD";
	case NCL_STATUS_DISKDEAD_NO_RETRY:	return "DISK_DEAD_DNR";
	case NCL_STATUS_TAKEN_DISKDEAD:	    return "DISK_DEAD_TAKEN";
	case NCL_STATUS_ABANDONED:	    	return "ABANDONED";
	case NCL_STATUS_TRANSFERRED:	    return "TRANSFERRED";
	default:							return "???";
	}
}

static inline void __enforce_enums_compatibility(void)
{	// Only for debug purposes: verify enum nvmeibc_rdma_intent extends nvmeibc_disk_locks_opr
	BUILD_BUG_ON((int)NVMEIBC_LOCK_CMP_AND_SWAP      != (int)NVMEIBC_CMD_LOCK_UNLOCK);
	BUILD_BUG_ON((int)NVMEIBC_LOCK_FORCE_WRITE       != (int)NVMEIBC_CMD_LOCK_COPY_OWNER);
	BUILD_BUG_ON((int)NVMEIBC_LOCK_READ              != (int)NVMEIBC_CMD_LOCK_READ_DR);
	BUILD_BUG_ON((int)NVMEIBC_LOCK_BLKSET_INFO_WRITE != (int)NVMEIBC_CMD_BLKSET_INFO_WR_DR);
}

const char* nvmeibc_rdma_intent_to_string(const enum nvmeibc_rdma_intent i)
{
	__enforce_enums_compatibility();
	switch (i) {
	case NVMEIBC_CMD_LOCK_OWNER  :		return "L-Ow";
	case NVMEIBC_CMD_LOCK_COPY_OWNER:	return "L-Cp";
	case NVMEIBC_CMD_LOCK_READ_PB:		return "R-PB";
	case NVMEIBC_CMD_LOCK_READ_DR:		return "R-DR";
	case NVMEIBC_CMD_PREDISCARD  :		return "P-Ow";
	case NVMEIBC_CMD_BLKSET_INFO_WR_DR:	return "W-DR";
	case NVMEIBC_CMD_BLKSET_INFO_WR_PB:	return "W-PB";
	case NVMEIBC_CMD_LOCK_UNLOCK:	    return "U-Ow";
	default :
		WARN(true, "nvmeibc bug - unknown lock intent %d\n", i);
		return "????";
	}
}

static inline struct nvmeibc_profiler *__raid_gp_profile_for_rwt_op_locks(const struct nvmeibc_cmd_lock *l, const struct nvmeibc_cmd_lock *locks)
{
	return nvmeibc_get_raid_good_path_profile_for_rwt_op(l->ds, locks->cmds->o->op);
}

//The real reasons to receive the reference to operation instance are:
//1. the operation reference is initialized only on the first lock within the lockset array;
//   but this can be solved, since nvmeibc_cmd_lock has lockset_idx
//2. the sync operations do some ugly tricks, by replacing the lock commands pointer with something else;
//   thus getting the operation via replaced commands will not gave us the desired result.

__attribute__((nonnull(2)))
static void nvmeibc_cmd_lock_request_io_pet_describe(struct operation const* o, struct nvmeibc_cmd_lock const* lock)
{
	struct nvmeibc_d_rdma_comp const *rdma_comp = &lock->comp;
	if (!o){
		return;
	}

	if (rdma_comp->opr == NVMEIBC_LOCK_READ){
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
								"lock.request(sgmnt=%hhu, address=0x%llx, opr=READ, type=%hhu<enum nvmeibc_rdma_intent>, rdma_comp(code=%hhu<enum nvmeibc_rdma_intent>)",
								numeric_downcast(u8, dp_locks_get_sgmnt_idx_of_lock(lock)),
								lock->address,
								numeric_downcast(u8, lock->type),
								numeric_downcast(u8, rdma_comp->code));

	} else {
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
								"lock.request(sgmnt=%hhu, address=0x%llx, opr=%hhu<enum nvmeibc_disk_locks_opr>, type=%hhu<enum nvmeibc_rdma_intent>, rdma_comp(code=%hhu<enum nvmeibc_rdma_intent>, compare=0x%x<union nvmeib_lock_id>, exchange=0x%x<union nvmeib_lock_id>))",
								numeric_downcast(u8, dp_locks_get_sgmnt_idx_of_lock(lock)),
								lock->address,
								numeric_downcast(u8, rdma_comp->opr),
								numeric_downcast(u8, lock->type),
								numeric_downcast(u8, rdma_comp->code),
								//casting, since there is no promises about the upper bits content
								(u32)(rdma_comp->compare),
								(u32)(rdma_comp->exchange));
	}
}

__attribute__((nonnull(2)))
static void nvmeibc_cmd_lock_response_io_pet_describe(struct operation const* o, struct nvmeibc_cmd_lock const* lock)
{
	struct nvmeibc_d_rdma_comp const *rdma_comp = &lock->comp;
	enum nvmeib_pet_severity const severity = NCL_is_request_failed(rdma_comp->lock_status)
									   	      ? NVMEIB_PET_SEVERITY_WARNING : NVMEIB_PET_SEVERITY_NORMAL;
	if (!o){
		return;
	}

	//we don't call this function on lock release - mainly because the operation already does not exist
	//so, rdma_comp->lock.bi should contain a legal value
	NVMEIBC_IO_PET_MSG(&o->journal,
						"lock.response(sgmnt=%hhu, rdma_comp(lock_status=%hhu<enum nvmeibc_block_lock_status>, blkset_info=0x%x<union nvmeib_blkset_info>, contending=0x%x<union nvmeib_lock_id>))",
						severity,
						numeric_downcast(u8, dp_locks_get_sgmnt_idx_of_lock(lock)),
						numeric_downcast(u8, rdma_comp->lock_status),
						//casting, since there is no promises about the upper bits content
						(u32)(rdma_comp->lock.bi),
						(u32)(get_contending_id(rdma_comp)));
}

void dp_locks_free_all(struct nvmeibc_cmd_lock *locks)
{
	struct nvmeibc_topology *t = locks->topo;
	dp_cmds_free_all(locks->new_cmds); /* Used only in trim on mirrored segs */
	DEBUG_TOPO_CNTRS_del_elem_from_topo(locks);
	__IO_LT_complete_locks(locks);
	#ifndef DP_LIB
	if (t->on_free.paused_disk && t->on_free.paused_disk->n_cont_prevents_waited_too_long) {
		_NW(t_01_cmplk, DMESG_PREFIX("@DEV_NAME") ": lock held ref too long. @TOPOLOGY(@TOPO_DBG_ID) paused_disk @DISK_NAME (@DISK) locks @LOCKS comp @COMP",
			t->nt->device_name, t, t->debug_unique_index, t->on_free.paused_disk->name, t->on_free.paused_disk, locks, &locks->comp);
		#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
		do {
			struct nvmeibc_disk_command_probes_try_data *current_try = nvmeibc_disk_command_probes_current_try(&locks->comp.probes);
			unsigned long jif = jiffies;
			_NW(t_05_cmplk, DMESG_PREFIX("@DEV_NAME") ": total-RTT @LD, n_retries=@RETRY_CNT, current-RTT = {pending=@LD, wire-rtt=@LD }",
				t->nt->device_name,
				(jif - locks->comp.probes.ulp_first_try_jif),
				(locks->comp.probes.n_tries - 1),
				current_try->llp_post_jif - current_try->ulp_post_jif,
				current_try->llp_comp_jif - current_try->llp_post_jif);
		} while(0);
		#endif
	}
	#endif
	nvmeibc_topology_put(t);
	nvmeibc_clmat_free_dangling_locks(locks);
}

void dp_locks_complete_lock(struct nvmeibc_cmd_lock *locks, int nrefs, int lock_i, const bool release)
{
	int value;
	_ND(t_dpcl0, "locksets=@LOCKSETS[@LSI] nrefs=@X, cmds=@CMDS, new_cmds=@CMDS", locks, lock_i, nrefs, locks->cmds, locks->new_cmds);
	if (lock_i >= 0) {		// Finish all side-effects of this lock and assert
		struct nvmeibc_cmd_lock *l = &locks[lock_i];
		const bool rv = NCL_is_failed_to_acquire(l->status) || NCL_is_failed_to_release(l->status);
		if (!release) {
			nvmeibc_profiling_end_take_cmd_stats_for_op(__raid_gp_profile_for_rwt_op_locks(l, locks), l->ds->lock_operation_profiler, locks->cmds->o, l->type, l, rv);
		}
		__invoke_crash_on_lock_corruption(locks, lock_i, "free", 0);
		if (NCL_is_failed_to_acquire(l->status)) {
			l->status = NCL_STATUS_DONE; /* IO failed. Will retry unless time-out */
		} else if (NCL_is_failed_to_release(l->status)) {
			l->status = NCL_STATUS_DONE; /* Notify Toma to convert this lock to stale ... :-( */
			nvmeibc_topologies_rereg_seg(l->ds); // Performed only if needed
		} else { /* Good path, release/trasnfer. Do nothing */ }
		WARN_ON(l->status != NCL_STATUS_DONE);
	}
	value = nvmeibc_atomic_sub_return(nrefs, &locks->n_uncompleted_locks);
	_ND(t_dpcl1, "locksets=@LOCKSETS[@LSI] waiting for others: uncomp=@RV", locks, lock_i, value);
	if (likely(value != 0)) {	// NOTE: locks can be already kfree. Dont touch them
		WARN((value < 0), "nvmeibc bug! locksets=%p[lsi%d].uncomp=0x%x\n", locks, lock_i, value);
	} else { // value == 0
		BLKCMP_IO_ONLY_IF_PRESERVE_STACK(struct operation *o = locks->cmds->o);	// Otherwise, operation was already free
		BLKCMP_IO_ASYNC_RESUME_CMP(dp_locks_free_all(locks));
	}
}

static inline void DEBUG_LOCKS_CONTENTION(__attribute__((__unused__)) struct nvmeibc_d_rdma_comp *dc)
{
#if 0									// Set to 1 to hack rdma cmpxcng to succees. Causes data corruption, so use only for measurmenet
	dc->lock_status = NCL_STATUS_TAKEN;
	get_contending_id(dc) = dc->compare;
#endif
}

/*
 * dp_locks_release_cb - Callback invoked when a lock release operation completes
 *
 * IMPORTANT: The operation (nvmeibc_operation) and associated command structures
 * may already be dead and deleted by the time this lock release callback is invoked.
 * Lock release happens asynchronously via RDMA, and the operation completion may
 * trigger operation cleanup/destruction before the RDMA lock release completes.
 * Therefore, DO NOT access operation or cmd structures within this callback.
 * Only access the lock structures themselves, which are kept alive until the
 * release completes.
 */
int dp_locks_release_cb(struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l = lock_of_bcomp(dc), *locksets = dp_locks_get_locks_header(l);
	const int lock_i = l->lockset_idx, ow_id = l->owner_idx;			// Just for short writing
	// Warning: operation/cmds might already be free() dont access them!!!
	(void)tag;
	__invoke_crash_on_lock_corruption(locksets, lock_i, "release", 1);
	WARN_ON(NCL_is_failed_to_acquire(dc->lock_status));			// Cant release if we havent acquired lock
	if (NCL_had_release_callback(dc->lock_status)) {
		nvmeibc_pd_cb_called_comp(l->ds->disk, dc);
	}

	DEBUG_LOCKS_CONTENTION(dc);
	switch (dc->lock_status) {
	case NCL_STATUS_CONTENDED:			// remote cmpxchng() failed
		#ifndef BLKDEV_SIMULATOR		// Simulator, Toma missbehaves with stalelocks
		WARN(true, "nvmeibc bug: Release failed: %p[%d|ow=%d]=%s cmp=0x%llx lock_id=0x%llx, seg=%s, addr=0x%llx, dc=%px\n",
		   locksets, lock_i, ow_id, ncl_status_str(dc->lock_status),
		   dc->compare, get_contending_id(dc), l->ds->uuid, l->address, dc);
		#endif
		FALLTHRU;						//
	case NCL_STATUS_FAIL_COMP:			// Transport could not send request
	case NCL_STATUS_FAIL_NO_COMP:
		_NT(trace_1_locks_rel_cb, "Release failed: locksets=@LOCKSETS[@LSI|ow=@OW_ID]=@NCL_STATUS_STR cmp=@CMP rv=@LOCK_ENT_U64",
		   locksets, lock_i, ow_id, ncl_status_str(dc->lock_status),
		   dc->compare, get_contending_id(dc));
		__change_lock_status_to(l, NCL_STATUS_TAKEN_DISKDEAD);
		break;

	case NCL_STATUS_TAKEN:				// Released OK
	case NCL_STATUS_TRANSFERRED:		// No need to release, gave to other IO
		_ND(trace_2_locks_rel_cb, "Release succeed: locksets=@LOCKSETS[@LSI|ow=@OW_ID]=@NCL_STATUS_STR", locksets, lock_i, ow_id, ncl_status_str(dc->lock_status));
		l->status = NCL_STATUS_DONE;
		#ifdef DEBUG_CONTENDED_LOCKS
		{
			const u32 msecs = jiffies_to_msecs(jiffies - l->lock_taken_jif);
			if (msecs >= warn_if_lock_held_more_than_n_msec) {
				const struct operation *o = locksets->cmds->o;
				if (o) {
					const u32 vol_id = nvmeibc_volume_short_id(o->nd);
					_NW_dmesg(warn_1_check_lock_actions_pre, DMESG_PREFIX("@DEV_NAME") ": slow I/O: Lock held too long. locksets=@LOCKSETS[@LSI|ow=@OWNER_ID] time=@MILISECONDS @DLBA my_id=@LOCKID - {@O_DBG_ID} OPCODE: @OP_CODE, VOL_MINOR: @VOL_ID, TOPOLOGY: <@TOPOLOGY_VERSION>",
					o->nd->name, locksets, lock_i, l->owner_id, msecs, l->address,
					(u32)l->comp.compare, o->dbg_id, o->op, vol_id, o->topo ? (u64)o->topo->debug_unique_index : 0);
					//BUG();
				}
			}
		}
		#endif
		break;

	default:
		WARN(1, "nvmeibc bug! locksets=%p[lsi=%d|ow=%d]=%s, alock_id=0x%llx\n", locksets, lock_i, ow_id, ncl_status_str(dc->lock_status), dc->lockset_id);
		break;
	}
	dp_locks_complete_lock(locksets, 1, lock_i, true);
	return 0;
}

/* Decide how to release (compare-exchange) the lock which was taken as
 * STALE-SPECIAL (or STALE + safe to treat as special, by Decentralized-Unreg).
 * 1. Write IO of a full LOCKSET of R1:
 * 1.1 When we currupted the data (at least one write cmd OK & one failed) or
 *     failure during release we ABANDONE the lock.
 * 1.2 When this write completes successfully, it effectively synchronized the
 *     mirrors, so we can release back to UNLOCKED (i.e. free).
 * 1.3 When all cmds have failed without writing anything then we didnt change
 *     any data & so the lock is released to STALE-SPECIAL (regardless of
 *     what its previous state was).
 * 2   Write IO to partial LOCKSET of R1:
 * 2.1 Same rule of 1.1 for ABANDONing lock as above, applies
 * 2.2 When this write completes successfully (all mirrirs), it synchronized
 *     only part of the mirrors, so we unlock back to STALE-SPECIAL.
 * 2.3 Same rule as 1.3
 * 3.  Read IO
 * 3.1 If lock was STALE-SPECIAL, TODO(EBA): Read IO is still triggering a
 *     SYNC job (full LOCKSET). Partial sync is not supported yet.
 * 3.2 If lock was STALE, sync must be executed. Sync will take it, and use its
 *     internal logic, regardless of the original IO being a read. If sync
 *     succeeded it releases to 0, if failed release to STALE-SPECIAL, or it may
 *     abandon lock.
 * 4.  Sync operation encountering STALE not special.
 * 4.1 If lock is safe to be used, treates it as STALE-SPECIAL. Otherwise Sync
 *     aborts.
 */
static void __set_cmpxchg_for_release(struct nvmeibc_cmd_lock*l, struct nvmeibc_disk_segment *seg)
{
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	const union nvmeib_lock_id holder = { .all = dc->exchange };
	const union nvmeib_lock_id toma_id = nvmeibc_disk_segment_get_praid(seg)->lid;
	dc->code = NVMEIBC_CMD_LOCK_UNLOCK;
	WARN(!nvmeib_lockid_are_purified_eq(holder, toma_id), "nvmeibc bug: 0x%x != 0x%x", holder.all, toma_id.all);
	if (unlikely(l->unlock_val)) {
		WARN_ON(l->unlock_val != RELEASE_LOCK__STALE_SPECIAL);
		dc->exchange = R1_STALE_SPECIAL_BINFO_VAL;				// Convert stale lock to stale special (stale unlocked)
		_ND(tr_1_set_cmpxchng_release, " locksets=@LOCKSETS release lock to stale-special: reason=@RV\n", l, l->unlock_reason);
	} else {	/* Unlock lock (keeping the non-lock bits intact) */
		dc->exchange = dc->lock_cnsts->unlocked_val;
	}
	dc->compare = holder.all;
}

#define __print_release_lock_status(trace_name, msg, l) _ND(trace_name, "@STR: locksets=@LOCKSETS[@LSI] @DLBA disk=@DISK_NAME", msg, dp_locks_get_locks_header(l), l->lockset_idx, l->address, l->ds->disk->name);

static void dp_locks_release_lock(struct nvmeibc_cmd_lock *locksets, int lsi)
{
	struct nvmeibc_cmd_lock *l = &locksets[lsi], *lo = &locksets[l->owner_idx];
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	struct nvmeibc_disk_segment *seg = l->ds;
	struct nvmeibc_disk *disk = seg->disk;

	if (unlikely(lo->unlock_val == RELEASE_LOCK__FORCE_ABANDON)) {
		WARN(NCL_is_failed_to_acquire(l->status), "Bug in nvmeibc! locksets=%p[lsi=%d] %d\n", locksets, lsi, l->status);	// Did we just sent disk cmds, without acquiring locks???
		__print_release_lock_status(t2_rel_lock, "Abandoning lock", l);
		__change_lock_status_to(l, NCL_STATUS_ABANDONED);
		return dp_locks_complete_lock(locksets, 1, lsi, true);
	}

	if (!NCL_do_i_have_lock(l->status)) {
		WARN(!NCL_is_failed_to_acquire(l->status), "Bug in nvmeibc! locksets=%p[lsi=%d] %d l=%p 0x%llx disk=%s\n", locksets, lsi, l->status, l, l->address, disk->full_name); // Dont have lock and didnt fail to take it. So what was I trying to do???
		__print_release_lock_status(t4_rel_lock, "Not releasing untaken lock", l);
		return dp_locks_complete_lock(locksets, 1, lsi, true);
	}

	/* Update the completion struct (which is still set for taking the lock)*/
	dc->callback = &dp_locks_release_cb;
	__IO_LT_try_transfer_give(locksets, lsi);

	if (l->status != NCL_STATUS_TRANSFERRED) {	// OWNER || PREDISCARD || COPY_OWNER
		int rv = 0;
		__set_cmpxchg_for_release(l, seg);
		nvmeibc_cmd_lock_request_io_pet_describe(locksets->cmds ? locksets->cmds->o : NULL, l);
		dp_locks_trace_lock_release(locksets->cmds ? locksets->cmds->o : NULL, l);
		rv = nvmeibc_pd_cmpxchg(disk, handle_of(seg), l->address, dc);
		if (rv < 0) { // Simulate failed release completion
			dc->lock_status = NCL_STATUS_FAIL_NO_COMP;
			dc->callback(dc, nvmeibc_d_rdma_comp_tag_make());
		}
	} else {
		dc->callback(dc, nvmeibc_d_rdma_comp_tag_make());		// Callback on successful transfer
	}
}

void dp_locks_release_locks_sibs(struct nvmeibc_cmd_lock *locksets, int owner_i)
{
	struct nvmeibc_cmd_lock *lo = &locksets[owner_i];
	int i, end = owner_i + lo->n_siblings;
	if (lo->secondary_id < 0) {		// Backwards release order
		for (i = end-1  ; i >= owner_i; i--)
			dp_locks_release_lock(locksets, i);
	} else {
		for (i = owner_i; i  < end    ; i++)
			dp_locks_release_lock(locksets, i);
	}
}

#define __give_failed_lock_cb(dc) ({ (dc)->lock_status = NCL_STATUS_DISKDEAD; (dc)->callback(dc, nvmeibc_d_rdma_comp_tag_make()); })
#ifdef BLKCMP_IO_COMPLETION_PRESERVE_STACK
	static int __um_completion_unblock_waiting_stack(struct nvmeibc_d_rdma_comp *read_comp, struct nvmeibc_d_rdma_comp_tag tag)
	{
		struct operation *o = get_d_comp_of_pg(read_comp)->cmd->o;

		(void)tag;
		read_comp->callback = &dp_locks_view_lock_sm;
		BLKCMP_IO_ASYNC_RESUME_CMP(dp_locks_view_lock_sm(read_comp));
		return 0;
	}
#endif


__attribute__((nonnull(1)))
static void dp_locks_send_read_lock(struct nvmeibc_d_iocmd_comp *cmp) {
	struct nvmeibc_cmd_lock *l = cmp->pigbck_lock, *locksets = dp_locks_get_locks_header(l);
	struct nvmeibc_disk_io_command *iocmd = container_of(cmp, struct nvmeibc_disk_io_command, comp);
	struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(iocmd);
	ulong times[3] = {jiffies, 0, 0}, duration;

	struct nvmeibc_block_command const *cmd = dp_cmds_get_cmd_from_comp(cmp);
	u64 const lock_addr = iocmd->lpb.addr;
	int rv;

	#ifdef BLKCMP_IO_COMPLETION_PRESERVE_STACK
		locksets->cmds->should_check_view_lock = 1;	// == cmp->cmd, relevant for fiber mode only
		cmp->pigbck_comp.callback = &__um_completion_unblock_waiting_stack;
	#endif

	nvmeibc_cmd_lock_request_io_pet_describe(cmd->o, l);
	rv = nvmeibc_pd_read_lock(l->ds->disk, iocmd->lpb.handle, lock_addr, dc);
	times[1] = jiffies;
	if (rv) {
		_ND(t_1srl, "locksets=@LOCKSETS[@LSI] rv=@RV o=@OPERATION c=@CMD_PTR", locksets, l->lockset_idx, rv, cmd->o, cmd);
		__give_failed_lock_cb(dc);
	}
	times[2] = jiffies;
	duration = jiffies_to_msecs(times[2] - times[0]);
	if (__SUSPICIOUS_LOCK_REQ_TIME < duration) {
		_NT(t_2srl, "Retry, locksets=@LOCKSETS[@LSI] read=@MILISECONDS cb=@MILISECONDS total=@MILISECONDS", locksets, l->lockset_idx, jiffies_to_msecs(times[1] - times[0]), jiffies_to_msecs(times[2] - times[1]), (u32)duration);
	}
}

static void __retry_read_lock(struct nvmeibc_d_iocmd_comp *cmp)
{
	struct nvmeibc_cmd_lock *l = cmp->pigbck_lock, *locksets = dp_locks_get_locks_header(l);
	struct nvmeibc_disk_io_command *iocmd = container_of(cmp, struct nvmeibc_disk_io_command, comp);
	_ND(trace_1_retry_rdlock, "Retry cmp=@CMP_PTR, locksets=@LOCKSETS[@LSI]", cmp, locksets, l->lockset_idx);
	l->retries++;
	slow_io_stats_t_log_over_retry(&locksets->cmds->o->nd->dp.io_slow, l, get_contending_id(dp_cmds_get_pigbck_comp_dc(iocmd)));
	dp_locks_send_read_lock(cmp);
}

/* In EC operations one lock can protect a few cmds. Read lock is probed by
   raid-leader-cmd only (after all its sibling complete) and not by each cmd */
void dp_locks_read_complete(struct nvmeibc_cmd_lock *locksets, int lsi,
		enum nvmeibc_block_lock_status status)
{
	// Daniel: Optimization: Skip atomic_sub(locksets[lsi].ncmds)
	__invoke_crash_on_lock_corruption(locksets, lsi, "view", 1);
	locksets[lsi].status = status;
	dp_locks_complete_lock(locksets, 1, lsi, false);
}

static void __read_lock_and_cmd_complete(struct nvmeibc_d_iocmd_comp *cmp,
	struct nvmeibc_cmd_lock *locks, int lsi, enum nvmeibc_block_lock_status s)
{
	// Daniel: Optimization: skip atomic_dec_return(&cmp->cmd->nlocks)
	dp_locks_read_complete(locks, lsi, s);
	dp_cmds_analyze_rv_and_complete(cmp);
}

static void __fail_cmds_of_broken_read_lock(enum nvmeibc_block_lock_status l_status,
			struct nvmeibc_d_iocmd_comp *cmp, int cmd_err)
{
	struct nvmeibc_cmd_lock *l = cmp->pigbck_lock, *locksets = dp_locks_get_locks_header(l);
	const int lsi = l->lockset_idx;
	const u64 holder = get_contending_id(&cmp->pigbck_comp);
	_NT(t_1fblr, "locksets=@LOCKSETS[@LSI].status=@STATUS_STR-->@STATUS_STR seg=@SEG, @DLBA, lock=@LOCK_ENT_U64, retries=@RETRIES, comp=@COMP, err=@ERR", locksets, lsi, ncl_status_str(l->status), ncl_status_str(l_status), l->ds->uuid, l->address, holder, l->retries, cmp, cmd_err);
	cmp->comp_code = cmd_err;
	__read_lock_and_cmd_complete(cmp, locksets, lsi, l_status);
}

#ifdef BLKCMP_IO_COMPLETION_NO_LOCKS_TIMER_RETRY
static void __schedule_retry_read_lock(struct nvmeibc_cmd_lock *u1, struct nvmeibc_d_iocmd_comp *cmp, ulong u2)
{
	(void)u1; (void)u2;
	__fail_cmds_of_broken_read_lock(NCL_STATUS_DONE, cmp, -EBUSY);	// No need to call full calback via: __give_failed_lock_cb(dc)
}
#else

static void __retry_read_lock_cb_work(struct workqe_struct *work) {
	struct nvmeibc_disk_io_command *io_cmd = container_of(work, struct nvmeibc_disk_io_command, disk_cmd.view_lock_work);
	slow_io_stats_t_rdl_dec(&io_cmd->comp->cmd->o->nd->dp.io_slow);
	__retry_read_lock(&io_cmd->comp);
}

TIMER_CALLBACK(__retry_read_lock_cb_timer, struct nvmeibc_d_rdma_comp, retry_timer, struct nvmeibc_d_iocmd_comp, cmp)
	struct nvmeibc_disk_io_command *io_cmd = container_of(cmp, struct nvmeibc_disk_io_command, comp);
	WQ_INIT_WORK(&io_cmd->disk_cmd.view_lock_work, __retry_read_lock_cb_work);
	dp_block_schedule_work(WORK_CPU_UNBOUND, &io_cmd->disk_cmd.view_lock_work);
}

static void __schedule_retry_read_lock(struct nvmeibc_cmd_lock *l, struct nvmeibc_d_iocmd_comp *cmp, ulong retry_time)
{
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	const ulong cur_time = jiffies;
	if (jiffies_to_msecs(cur_time - l->last_retry_report_time) > __COMPLAIN_LOCK_TIME) {
		get_contending_id(dc) = get_contending_id(&cmp->pigbck_comp); // == get_contending_id(cmp->cmd->iocmd->lpb.comp); /* Copy the holder for correct toma help invocation */
		l->last_retry_report_time = cur_time;
		__send_toma_lock_help(l, l->ds);
	}
	slow_io_stats_t_rdl_inc(&cmp->cmd->o->nd->dp.io_slow);
	dp_locks_activate_timer(dc, __retry_read_lock_cb_timer, cmp, retry_time);
}
#endif

/* Callback typeof nvmeibc_sync_cb_t */
static int __retry_read_lock_cb_sync_done(void* context, int err)
{
	struct nvmeibc_d_iocmd_comp *cmp = context;
	struct nvmeibc_cmd_lock *l = cmp->pigbck_lock, *locksets = dp_locks_get_locks_header(l);
	const int lsi = l->lockset_idx;
	switch (err) {
		case EPERM_READ_FAIL_NO_RETRY: {/* sync failed in double read error. */
			cmp->comp_code = err;
			__read_lock_and_cmd_complete(cmp, locksets, lsi, NCL_STATUS_DISKDEAD_NO_RETRY);
			break;
		}
		case 0: { /* Sync succeeded, the data we originally read (before
			encountering stale lock) was commited to the entire slice,
			(via data sync or dirty bit turn on).
			R1: Sync force overwrote IO buffers with latest data, because IO could read not from owner
			EC: Assume IO read from owners. Will cause read 'uncommitted' DI, if suddently block had new data, became bad sector and HTR rolled it backwards from parities
				Todo: Solution - in EC, retry the entire read (disk cmds) not jsut view lock */
			__read_lock_and_cmd_complete(cmp, locksets, lsi, NCL_STATUS_DONE);
			break;
		}
		default: /* Daniel: Todo, analyze sync more precisely, TODO(EC-2584) */
			l->last_retry_report_time = jiffies;	// Don't include sync time in lock timeout, start anew
			__retry_read_lock(cmp); // Retry reading the lock
			break;
	}
	return 0;
}

static bool must_do_full_blkset_sync(const struct nvmeibc_block_command *c) {
	return (nvmeibc_raid_is_ec(nvmeibc_disk_segment_get_praid(c->ds))||
		nvmeibc_sync_is_trigger_full_blkset_sync(c->nlbas));
}

bool nvmeibc_debug_ram_binfo = true;	// By default check binfo
module_param(nvmeibc_debug_ram_binfo, bool, 0644);
MODULE_PARM_DESC(nvmeibc_debug_ram_binfo, "Enforce detection of topological data corruptions in RAM");

bool verify_binfo_is_legal(struct nvmeibc_disk_segment *seg, const union nvmeib_blkset_info binfo, const u64 dlba, const char action)
{
	if (nvmeibc_debug_ram_binfo) {
		if (unlikely(binfo.bits.dirty != 0)) {		// Todo: Extend this test, currently detects wrong dbits
			struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(seg);
			sgmnts_bmp_t clean_bm = nvmeibc_raid1_get_sgmnts_bmp(pr, readable);
			const union nvmeibc_dbits_entry dbits_ent = { .all_bits = binfo.bits.dirty };
			const sgmnts_bmp_t dbits_bm = nvmeibc_dbits_get_bm(&dbits_ent, nvmeibc_raid1_get_protect_lvl(pr));
			if (action == 'w') {			// Write IO/Sync, Strongest verification, Ensure the dbits that we are turning off cannot be set
				clean_bm |= nvmeibc_raid1_get_sgmnts_bmp(pr, w);
			} else if (action == 's') {		// Write-by-Sync, Weaker verification, same as above but can turn on convicts for W- topology
				clean_bm |= (nvmeibc_raid1_get_sgmnts_bmp(pr, w) & nvmeibc_raid1_get_inverse_sgmnts_bmp(pr, wm));
			} else if (action == 'r') {		// Read IO/Sync. Weakest verification, only readable segments, are tested dbits may exist on 'W' segs
			}
			if (unlikely(dbits_bm & clean_bm)) {                // Dbit on segment which cannot be turned on
				const u64 slba_blksets = (dlba - seg->first_lba) / LOCKSET_SLICES;
				char buf_print[32];
				nvmeibc_dbits_entry_to_str(buf_print, sizeof(buf_print), binfo.bits.dirty);
				WARN(true, "Possible Data corruption: seg=%-.8s, dlba=0x%llx, slba=0x%llx[blksets] binfo=0x%x=%s, dbits on clean seg. Disabling IO. Manual intervention is required to continue.\n", seg->uuid, dlba, slba_blksets, binfo.all, buf_print);
				nvmeibcb_dp_io_fail_mgr_binfo_err(&nvmeibc_disk_seg_to_bdev(seg)->dp.io_stats.mgr);
				return false;
			}
			/*if (nvmeibc_praid_are_all_readable(pr)) { // Unknonw dbits in perfect topology. This might indicatet a bug in toma, but not a corruption: Enable and fix unitest, now we dont test UNKNOWNS
				const u64 slba_blksets = (dlba - seg->first_lba) / LOCKSET_SLICES;
				char buf_print[32];
				nvmeibc_dbits_entry_to_str(buf_print, sizeof(buf_print), binfo.bits.dirty);
				WARN(true, "Possible Data corruption: seg=%-.8s, dlba=0x%llx, slba=0x%llx[blksets] binfo=0x%x=%s, unknown dbits in normal mode. Disabling IO. Manual intervention is required to continue.\n", seg->uuid, dlba, slba_blksets, binfo.all, buf_print);
			}*/
		}
	}
	return true;
}

static void __squash_transport_lock_status(struct nvmeibc_cmd_lock *l, enum nvmeibc_block_lock_status s)
{
	switch (s) {
	case NCL_STATUS_TRANSFERRED:
		__change_lock_status_to(l, NCL_STATUS_TAKEN);
		break;
	case NCL_STATUS_FAIL_NO_COMP:
	case NCL_STATUS_FAIL_COMP:
		__change_lock_status_to(l, NCL_STATUS_DISKDEAD);
	default:
		break;
	}
}

int dp_locks_view_lock_sm(struct nvmeibc_d_rdma_comp *read_comp, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_d_iocmd_comp *cmp = get_d_comp_of_pg(read_comp);
	struct nvmeibc_cmd_lock *l = cmp->pigbck_lock, *locksets = dp_locks_get_locks_header(l);
	struct nvmeibc_disk_io_command *iocmd = container_of(cmp, struct nvmeibc_disk_io_command, comp);
	unsigned long retry_time;
	struct operation *o = locksets->cmds->o;
	const u64 holder = get_contending_id(read_comp);
	const int lsi = l->lockset_idx;

	(void)tag;
	nvmeibc_cmd_lock_response_io_pet_describe(o, l);
	l->status = read_comp->lock_status;
	_ND(t_rlsm0, "locksets=@LOCKSETS[@LSI] cmp=@PTR, val=@LOCK_ENT_U64, lock_status=@STATUS_STR" , locksets, lsi, cmp, holder, ncl_status_str(l->status));
	dp_locks_trace_lock_comp(o, l, read_comp);

	// ----- Step1: Handle piggyback view-lock send-if-needed
	if (unlikely(!dp_cmds_pigbck_has_any(iocmd))) { 			// Disk-cmd completion, without piggyback. View lock uninitialized (not piggibacked)
		WARN(l->status != NCL_STATUS_INVALID, "nvmeibc bug: l->status=%d, it was unused!", l->status);
		dp_cmds_add_generic_piggyback(iocmd);		  // read now, only after data was read!. Note: read_comp == dp_cmds_get_pigbck_comp_dc(iocmd)
		dp_locks_send_read_lock(cmp); /*EC-4937 if we do not have lock id yet, do not call for toma help. Maybe we don't need it.*/
		return 0;												// Will get a future callback from view read lock
	} else if (l->status == NCL_STATUS_INVALID) {				// Disk-cmd completion with piggybacked view lock. Lock was not requested directly
		const enum nvmeibc_block_lock_status new_status = ((holder == read_comp->lock_cnsts->unlocked_val) ? NCL_STATUS_TAKEN : NCL_STATUS_CONTENDED);
		__change_lock_status_to(l, new_status);					// Simulate as happens in transport layer via explicit view lock
	} else {													// Explicit Read-lock view operation via pausable layer
		if (NCL_had_acquire_callback(l->status))
			nvmeibc_pd_cb_called_comp(l->ds->disk, read_comp);
		__squash_transport_lock_status(l, l->status);
		if (l->status == NCL_STATUS_DISKDEAD) {
			OPERATION_DBG_CNTR_INC(o, n_lcmd_failed);
		}
		if (NCL_is_failed_to_acquire(l->status)) {				// Failed to send (failed locally on client)
			__fail_cmds_of_broken_read_lock(NCL_STATUS_DISKDEAD, cmp, -EIO);
			return 0;	// Beware, everything could get free
		}
	}

	// ----- Step2: Handle piggyback view-lock success
	if (likely(NCL_do_i_have_lock(l->status)) ||										// Lock is empty
		(nvmeibc_sync_is_stale(l, holder) && nvmeibc_sync_is_read_only(l, holder))) {	// Or, EC: lock is stale but slice was not corrupted
		const union nvmeib_blkset_info binfo = { .all = (u32)nvmeibc_get_binfo_of_comp(read_comp) };
		if (!verify_binfo_is_legal(l->ds, binfo, l->address, 'r')) {
			_NE(tr6_dplrcb, DMESG_PREFIX("@DEV_NAME") ": Additional_info: locksets=@LOCKSETS[@LSI], lock_val=@LOCK_ENT_U64", o->nd->name, locksets, lsi, holder);
			nvmeibc_block_suspend(o->nd, NULL, NULL);
			__fail_cmds_of_broken_read_lock(NCL_STATUS_DONE, cmp, -EIO);
		} else																			// _successful_unlocked
			__read_lock_and_cmd_complete(cmp, locksets, lsi, NCL_STATUS_DONE);
		return 0;		// Beware, everything could get free
	}

	// ----- Step3: Handle piggyback view-lock retry
	WARN_ON(l->status != NCL_STATUS_CONTENDED);					// Assumes that every problem with lock can be overcome with retry
	retry_time = dp_locks_get_retry_time(l);
	_ND(t_rlsm1, "locksets=@LOCKSETS[@LSI] retries=@RETRIES retry_time=@TIME @DLBA ptr=@PTR" , locksets, lsi, l->retries, retry_time, l->address, l);

	__ndump_operation(locks_read_cb, o);
	if (nvmeibc_operation_does_expire_at(o, retry_time)) {
		__fail_cmds_of_broken_read_lock(NCL_STATUS_DONE, cmp, -EBUSY);
	} else if (o->topo->phased_out) {	// Our data is outdated (new topo may have different read owner).
		OPERATION_DBG_CNTR_INC(o, n_topo_phased_out);
		__fail_cmds_of_broken_read_lock(NCL_STATUS_DONE, cmp, -EAGAIN);
	} else {
		if (nvmeibc_sync_is_stale(l, holder)) {
			enum stale_lock_resolve_status ss;
			/* Daniel: Read cannot complete without sync operation or else it is
			   data corruption. So it does not matter if lock is stale or stale
			   special, just let sync-op to solve the problem. Issue a sync op
			   & wait for its completion callback to retry reading locks */
			const struct nvmeibc_block_command *c = dp_cmds_get_cmd_from_comp(cmp);
			u16 start_block = 0, n_slices = LOCKSET_SLICES;
			l->comp.lock = read_comp->lock;			// Copy the holder+binfo, for sync to know how to compare exchange and treat unknown dbits
			l->comp.opr =  read_comp->opr;			// Copy the type of operation into the lock
			ss = stale_lock_resolver_get_status(&l->ds->toma_reg->hdr->slr, holder, l);
			if (ss == stale_lock_resolve_safe_to_use) { // Treat as stale special
				if (!must_do_full_blkset_sync(c)) {
					start_block = (u16)(iocmd->reqs1.disk_address % LOCKSET_SLICES);
					n_slices    = (u16)c->nlbas; // Partial sync (NOT clear the stale-special but ensures consistency of the IO extent).
				}
				if (nvmeibc_sync_fix_some_slices_in_stale(l, start_block, n_slices, __retry_read_lock_cb_sync_done, cmp) == 0)
					return 0;								// Will get a future callback from sync completion
			} else { } // Just retry after delay and hope toma will answer us. Same flow as typical contended lock
		} else if (holder != read_comp->exchange) { /* Just contended lock, retry */
		} else { /* Lock is already mine but another thread locked it */ }
		__schedule_retry_read_lock(l, cmp, retry_time);
	}
	return 0;								// Will get a future callback from retry-send
}

static void __print_lock_to_log(struct nvmeibc_cmd_lock* locksets, int lsi)
{	// Daniel: Todo, unite with code of __dump_operation_unsafe()
	const struct nvmeibc_cmd_lock* l = &locksets[lsi]; (void)l;
	_ND(trace_lock_to_log, "locksets=@LOCKSETS[@LSI|ow=@OWNER_ID] type=@TYPE @DISK_NAME:@DLBA, n_sibs=@N_SIBS", locksets, lsi, l->owner_idx,
	   l->type, l->ds->disk->name, l->address, l->n_siblings);
}

/* Add locks to protect given raid. Returns the amount of locks added */
static int __add_locks_for_raid(const struct nvmeibc_raid1 *r1, const enum nvmeib_block_io_op op, u64 rlba, struct nvmeibc_cmd_lock *locksets, const int owner_i, const struct nvmeib_cpu_mask_info *cpu_mask_info)
{
	struct lock_ownership_map rlmap;
	int li;				// In a loop, adding lock 'li' on segment 'si'
	lock_ownership_build_raid_map(r1, rlba, op, &rlmap);
	for (li = 0; li < rlmap.n_locks; li++) {
		const int si = rlmap.si[li];
		const int my_ind = owner_i + li;
		const u64 dlba = dp_io_topo_iterator_conv_rlba_to_phys_lock_addr(r1, si, rlba);
		struct nvmeibc_cmd_lock *l = &locksets[my_ind];

		l->owner_idx = owner_i;		/* Init generic lock header */
		l->n_siblings = rlmap.n_locks;
		l->ds = &r1->segments[si];
		l->address = __to4K(dlba);
		l->comp.lock_cnsts = nvmeibc_raid1_get_lock_consts(r1);
		if (cpu_mask_info)
			l->comp.cpu_mask_info = *cpu_mask_info;
		nvmeibc_b_rdma_comp_init(&l->comp, my_ind, locksets);

		switch (rlmap.type[li]) {
		case NVMEIBTC_DS_OWNER_MODE_SECONDARY: {		// Dual locks topology
			l->type = NVMEIBC_CMD_LOCK_OWNER;
			locksets[owner_i].secondary_id = my_ind + 1; //+1 so 0 means no sec
			l->status = NCL_STATUS_NOTISSUED;
			break;
		}
		case NVMEIBTC_DS_OWNER_MODE_COPY_OWNER: {
			l->type = NVMEIBC_CMD_LOCK_COPY_OWNER;
			locksets[owner_i].secondary_id = -1; 	// Means, copy-of-owners
			l->status = NCL_STATUS_NOTISSUED;
			break;
		}
		case NVMEIBTC_DS_OWNER_MODE_PRIMARY: {
			l->type = NVMEIBC_CMD_LOCK_OWNER;
			nvmeibc_atomic_set(&l->pending, l->n_siblings);
			l->status = (rlmap.n_locks == 0) ? NCL_STATUS_INVALID : NCL_STATUS_NOTISSUED;
			if (unlikely(op == NVMEIB_BLOCK_IO_OP_DISCARD)) {
				l->type = NVMEIBC_CMD_PREDISCARD; /* Split long trims */
				nvmeibc_atomic_inc(&locksets[0].prediscards);
			}
			break;
		}
		default: BUG();	// Unsupported.
		}
		__print_lock_to_log(locksets, my_ind);
	} /* for (all siblings; */
	return li;
}

int dp_fill_locks_for_io(enum nvmeib_block_io_op op, u64 nlbas, u64 vlba, int c_i, struct nvmeibc_topology *t, struct nvmeibc_cmd_lock *locksets, const int locksets_len, const struct nvmeib_cpu_mask_info *cpu_mask_info)
{
	struct dp_io_topo_iterator it;
	int lock_i = 0;
	if (locksets_len <= 0) {
		return locksets_len; /* Error or don't need locks at all */
	}
	BUG_ON(locksets == NULL);		// Sanity: Why is this function being called?
	BUG_ON(locksets->nlocks != 0);	// Sanity: Did you already call this function and calling it again? adding more locks
	dp_io_topo_iterator_init(&it, vlba, nlbas, t, c_i);
	while (dp_io_topo_iterator_next(&it, 'l')) {
		lock_i += __add_locks_for_raid(it.res.r, op, it.res.rlba, locksets, lock_i, cpu_mask_info);
		WARN(lock_i > locksets_len, "nvmeibc bug %d > %d\n", lock_i, locksets_len);
	}
	_ND(trace_1_fill_locks, "locksets=@LOCKSETS->nlocks=@N_LOCKS", locksets, lock_i);
	locksets->nlocks = lock_i;
	__invoke_crash_on_lock_corruption(locksets, 0, "init", 0);
	return lock_i;
}

raid_sgmnt_t dp_locks_get_sgmnt_idx_of_lock(const struct nvmeibc_cmd_lock *self) 
{
	struct nvmeibc_raid1* raid = nvmeibc_disk_segment_get_praid(self->ds);
	return self->ds - raid->segments;
}

int dp_fill_locks_for_raid(const struct nvmeibc_raid1 *r1, enum nvmeib_block_io_op op, u64 rlba, struct nvmeibc_cmd_lock *locksets)
{
	return __add_locks_for_raid(r1, op, rlba, locksets, 0, NULL);
}

void dp_block_translation_unit_calc_locks(struct dp_block_translation_unit *tu, struct dp_io_topo_iterator *it)
{
	struct t_dp_block_trans_output *_out = &tu->output;
	struct operation o;			// Dummy operation
	int i;
	memset(&o, 0, sizeof(o));
	o.nd = tu->input.nd;
	nvmeibc_clmat_allocate(&o, true, N_MAX_RAID_LOCKS, 0, 0);
	_out->n_locks = dp_fill_locks_for_raid(it->res.r, tu->input.op, it->res.rlba, o.locks);
	for (i = 0; i < _out->n_locks; i++) {
		 _out->ldisks[i] = o.locks[i].ds->disk;
		 _out->ldescr[i] = nvmeibc_rdma_intent_to_string(o.locks[i].type);
	}
	nvmeibc_operation_move_mem_to_locks(&o);			// Simulate as if operation completed
	nvmeibc_clmat_free_dangling_locks(o.locks);
}

static void __request_lock(struct nvmeibc_cmd_lock *locksets, int lsi);
static void __retry_owner_lock(struct nvmeibc_cmd_lock *l, bool autofail)
{
	const int lsi = l->lockset_idx;
	struct nvmeibc_cmd_lock *locksets = dp_locks_get_locks_header(l);
	struct nvmeibc_d_rdma_comp* dc = &l->comp;
	const struct operation *o = locksets->cmds->o;
	ulong start = jiffies, diff;
	_ND(t1_rol, "retry owner locksets=@LOCKSETS[@LSI].retries=@RETRIES time=@MILISECONDS",locksets, lsi, l->retries, jiffies_to_msecs(jiffies - l->first_try_time));
	if (likely(!autofail)) {
		l->retries++;
		slow_io_stats_t_log_over_retry(&o->nd->dp.io_slow, l, get_contending_id(dc));
		l->status = NCL_STATUS_NOTISSUED;
		__request_lock(locksets, lsi);
		diff = (jiffies - start);
		if (__SUSPICIOUS_LOCK_REQ_TIME < jiffies_to_msecs(diff)) {
			_NT(trace_1_retry_owner_lock, "Retry locksets=@LOCKSETS[@LSI] rqst=@MILISECONDS", locksets, lsi, jiffies_to_msecs(diff));
		}
	} else {
		_NT(t2_rol, "locksets=@LOCKSETS[@LSI] Error with lock for @DLBA disk=@DISK_NAME", locksets, lsi, l->address, l->ds->disk->name);
		dc->lock_status = NCL_STATUS_DISKDEAD_NO_RETRY;
		dc->callback(dc, nvmeibc_d_rdma_comp_tag_make()); /* Simulate failure callback */
		diff = (jiffies - start);
		if (__SUSPICIOUS_LOCK_REQ_TIME < jiffies_to_msecs(diff)) {
			_NT(t3_rol, "Retry locksets=@LOCKSETS[@LSI] callback=@MILISECONDS", locksets, lsi, jiffies_to_msecs(diff));
		}
	}
}

#ifdef BLKCMP_IO_COMPLETION_NO_LOCKS_TIMER_RETRY
static void __schedule_retry_owner_lock(struct nvmeibc_cmd_lock *l, ulong u1)
{
	(void)u1;
	__give_failed_lock_cb(&l->comp);
}
#else
static void __retry_owner_lock_cb_work(struct workqe_struct *work) {
	struct nvmeibc_cmd_lock *lock = container_of(work, struct nvmeibc_cmd_lock, comp.retry_work_post_timer);
	#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
		struct nvmeibc_disk_command_probes_try_data *current_try = nvmeibc_disk_command_probes_prev_try(&lock->comp.probes);
		current_try->retry_work_jif = jiffies;
	#endif
	slow_io_stats_t_wrl_dec(&lock->cmds->o->nd->dp.io_slow);
	__retry_owner_lock(lock, false);
}

TIMER_CALLBACK(__retry_owner_lock_cb_timer, struct nvmeibc_d_rdma_comp, retry_timer, struct nvmeibc_d_rdma_comp, dc)
	#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
		struct nvmeibc_disk_command_probes_try_data *current_try = nvmeibc_disk_command_probes_prev_try(&dc->probes);
		current_try->retry_timer_jif = jiffies;
	#endif
	WQ_INIT_WORK(&dc->retry_work_post_timer, __retry_owner_lock_cb_work);
	dp_block_schedule_work(WORK_CPU_UNBOUND, &dc->retry_work_post_timer);
}

static void __schedule_retry_owner_lock(struct nvmeibc_cmd_lock *l, ulong retry_time)
{
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	const ulong cur_time = jiffies;
	_ND(t_1_srol, "setting timer");
	dc->compare = dc->lock_cnsts->unlocked_val; /* Revert previous attempt to lock a stale or stale special */
	NVMEIBC_LOCK_SET_UNLOCK(l, RELEASE_LOCK__UNLOCKED, RELEASE_LOCK_REASON__NONE);
	if (jiffies_to_msecs(cur_time - l->last_retry_report_time) > __COMPLAIN_LOCK_TIME) {
		l->last_retry_report_time = cur_time;
		__send_toma_lock_help(l, l->ds);
	}
	slow_io_stats_t_wrl_inc(&lock->cmds->o->nd->dp.io_slow);
	dp_locks_activate_timer(dc, __retry_owner_lock_cb_timer, dc, retry_time);
}
#endif

static void __fix_release_val_after_full_sync(struct nvmeibc_cmd_lock *l)
{
	/* Problematic case: Owner lock was stale (IO took it with intent to release
	as stale special), but secondary was stale special (and sync op was called
	on it). Sync took only secondary owner (coz owner is taken by IO).
	Solution: if sync succeeded, original IO has to mark its owner to be
	released as 0, not stale special */
	struct nvmeibc_cmd_lock *lo = dp_locks_get_blockset_owner_lock(l);
	if (unlikely(lo->secondary_id > 0)) {
		if ((lo->secondary_id == (l->lockset_idx + 1)) &&
		    (lo->unlock_val   == RELEASE_LOCK__STALE_SPECIAL)) {
			NVMEIBC_LOCK_SET_UNLOCK(lo, RELEASE_LOCK__UNLOCKED, RELEASE_LOCK_REASON__NONE);
		}
	} /* Else, no secondary or have copy owners in which case there is no unlocking to any value except zero*/
}

/* Callback typeof nvmeibc_sync_cb_t */
static int __retry_owner_lock_cb_sync_done(void* context, int err)
{
	struct nvmeibc_cmd_lock *l = (struct nvmeibc_cmd_lock *)context;
	int autofail = false;
	switch (err) {
	case EPERM_READ_FAIL_NO_RETRY: /* Read error in sync op on owner */
		autofail = true;
		break;
	case 0:
		__fix_release_val_after_full_sync(l);

	default:
		break;  /* Daniel: Todo, analyze read failure error more precisely, TODO(EC-2584) */
	}
	// we'll now retry the cmd locking attempt although the sync has already placed the data in the cmd buffer.
	l->last_retry_report_time = jiffies;	// Don't include sync time in lock timeout, start anew
	__retry_owner_lock(l, autofail);
	return 0;
}

#ifdef BLKCMP_IO_COMPLETION_PRESERVE_STACK
	static int __um_completion_unblock_waiting_stack_sync(void* context, int err)
	{
		struct operation *o = context;
		struct nvmeibc_block_command *rldr = o->cmds;
		BUG_ON(rldr->o_rv != 0);	// If has error, we should have autofailed the io, not launching syncs
		rv_storage_of_sync_error(rldr) = err;
		BLKCMP_IO_ASYNC_RESUME_CMP(o);
		return 0;
	}

	static void dp_locks_stale_launc_sync(struct operation *o, int lock_i)
	{
		nvmeibc_sync_cb_t done_cb = __um_completion_unblock_waiting_stack_sync;
		int rv = nvmeibc_sync_fix_stale(&o->locks[lock_i], done_cb, o);
		if (rv < 0)
			done_cb(o, -ENOEXEC);
	}

	void dp_locks_stale_after_sync_retry_acquire(struct nvmeibc_cmd_lock *owl, int sync_rv, sgmnts_bmp_t retry_locks_bmp)
	{
		int i, n_locks = owl->n_siblings;
		if (retry_locks_bmp == 1) {
			nvmeibc_atomic_set(&owl->pending, n_locks);	// Already set, meaningless action
			__retry_owner_lock_cb_sync_done(owl, sync_rv);
		} else {
			const int n_stale_locks = hweight16(retry_locks_bmp);
			nvmeibc_atomic_set(&owl->pending, n_stale_locks);	// was drained to 0, reset it
			for (i = 0; i < n_locks; i++) {
				if (retry_locks_bmp & (1 << i)) {
					__retry_owner_lock_cb_sync_done(&owl[i], sync_rv);
				}
			}
		}
	}
	sgmnts_bmp_t dp_locks_stale_get_locks_bitmap_in_blockset(struct nvmeibc_cmd_lock *owl)
	{
		int i, n_locks = owl->n_siblings;
		sgmnts_bmp_t retry_locks_bmp = 0;
		for (i = n_locks - 1; i >= 0; --i) {
			if (owl[i].need_stale_sync) {
				retry_locks_bmp |= (1 << i);
				owl[i].need_stale_sync = 0;
			}
		}
		return retry_locks_bmp;
	}

	int dp_locks_stale_call_sync_blockset(struct nvmeibc_cmd_lock *owl, ulong retry_locks_bmp)
	{
		struct nvmeibc_block_command *rldr = owl->cmds;
		struct operation *o = rldr->o;
		const int pb_index = find_first_bit(&retry_locks_bmp, owl->n_siblings);
		int sync_rv;
		BLKCMP_IO_ASYNC_AWAIT(dp_locks_stale_launc_sync(o, pb_index));
		sync_rv = rv_storage_of_sync_error(rldr);	// Wakup after sync and return the error
		rv_storage_of_sync_error(rldr) = 0;
		return sync_rv;
	}

	static int __call_assist_sync(struct nvmeibc_cmd_lock *l)
	{
		struct operation *o = l->cmds->o;
		struct nvmeibc_cmd_lock *owner_lock = dp_locks_get_blockset_owner_lock(l);
		l->need_stale_sync = true;
		if (owner_lock == l) {
			BLKCMP_IO_ASYNC_RESUME_CMP();	// Waiting only for owner, wakeup fiber now
		} else {	 // Cant wakeup, wait for all in air requests of copy locks to finish
			const int pending = nvmeibc_atomic_dec_return(&owner_lock->pending);
			if (pending == 0) {
				BLKCMP_IO_ASYNC_RESUME_CMP();
			}
		}
		return 0;
	}
#else
	#define __call_assist_sync(lock)    ((nvmeibc_sync_fix_stale(l, __retry_owner_lock_cb_sync_done, l) == 0) ? 0 : -EBUSY)

#endif

/* Contended lock with stale value which Toma said is safe to take over*/
static int __on_stale_resolved_val(struct nvmeibc_cmd_lock *locksets, int lock_i, struct operation *o, const u64 holder)
{
	struct nvmeibc_cmd_lock *l = &locksets[lock_i];
	const struct nvmeibc_block_command *c = nvmeibc_cllink_find_cmd_by_lock(locksets, lock_i);
	if (nvmeibc_sync_is_io_implicit_sync(l, c)) {
		/* IO write entire blockset so it will syncronize it. no need for sync*/
	} else if ((o->op == NVMEIB_BLOCK_IO_OP_READ) || must_do_full_blkset_sync(c)) { 	// must use external sync to fix lock
		return __call_assist_sync(l);		// Todo: On sync fail, call __give_failed_lock_cb(dc); asap
	} else { // Write/Trim on Raid1 & unlock back to stale-special (without sync)
		_ND(trace_1_on_st_resolv, "locksets=@LOCKSETS[@LSI], skip sync. op=@BLOCK_IO_OP", locksets, lock_i, o->op);
		NVMEIBC_LOCK_SET_UNLOCK(l, RELEASE_LOCK__STALE_SPECIAL, RELEASE_LOCK_REASON__IO_WITHOUT_SYNC);
	}
	l->comp.compare = holder;
	__retry_owner_lock(l, false);	// Immediately (no timer)
	return 0;
}

/* Lock acquisition completion callback analysis */
static void __check_lock_actions(struct nvmeibc_cmd_lock *locksets, int lock_i)
{
	struct nvmeibc_cmd_lock *l = &locksets[lock_i];
	const int owner_id = l->owner_idx;
	const u32 msecs = jiffies_to_msecs(jiffies - l->first_try_time);
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	struct nvmeibc_cmd_lock *owner_lock = &locksets[owner_id];
	struct operation *o = locksets->cmds->o;

	if (l == owner_lock){
		nvmeibc_cmd_lock_response_io_pet_describe(o,l);
	}
	__squash_transport_lock_status(l, dc->lock_status);
	BUG_ON((l->status != dc->lock_status) || (l->type == NVMEIBC_CMD_PREDISCARD));		// Just sanity
	WARN(!(NCL_is_failed_to_acquire(l->status) || (l->status == NCL_STATUS_CONTENDED) || (l->status == NCL_STATUS_TAKEN)), "nvmeibc bug: locks=%p[%d].status=%d", locksets, lock_i, l->status);
	#ifdef DEBUG_CONTENDED_LOCKS
		l->curr_txid = (dc->lock.bi >> dc->lock_cnsts->blkset_info_txid_shift) & dc->lock_cnsts->blkset_info_txid_mask;
		if (dc->lock_status == NCL_STATUS_CONTENDED) {
			l->curr_contender_id = get_contending_id(dc);
			if (l->retries == 0) {
				l->first_try_contender_id = l->curr_contender_id;
				l->first_try_txid = l->curr_txid;
			}
		} else if (dc->lock_status == NCL_STATUS_TAKEN) {
			if (l->type != NVMEIBC_CMD_LOCK_UNLOCK)
				l->lock_taken_jif = jiffies;
		}
	#endif
	if (l->status == NCL_STATUS_TAKEN) {
		if (msecs >= warn_if_lock_took_more_than_n_msec) {
			#ifdef DEBUG_CONTENDED_LOCKS
			_NW(warn_1_check_lock_actions, DMESG_PREFIX("@DEV_NAME") ": slow I/O: Long lock acqusition. locksets=@LOCKSETS[@LSI|ow=@OWNER_ID] retries=@RETRIES time=@MILISECONDS @DLBA my_id=@LOCKID last_contended=@LOCKID first_contended=@LOCKID last_contended_txid=@TXID first_contended_txid=@TXID",
			o->nd->name, locksets, lock_i, owner_id, l->retries, msecs, l->address,
			l->comp.exchange, l->curr_contender_id, l->first_try_contender_id, l->curr_txid, l->first_try_txid);
			#else
			_NW(warn_1_check_lock_actions, DMESG_PREFIX("@DEV_NAME") ": slow I/O: Long lock acqusition. locksets=@LOCKSETS[@LSI|ow=@OWNER_ID] retries=@RETRIES time=@MILISECONDS @DLBA my_id=@LOCKID",
			o->nd->name, locksets, lock_i, owner_id, l->retries, msecs, l->address, l->comp.exchange);
			#endif
		}
	} else if (l->status == NCL_STATUS_DISKDEAD) {
		OPERATION_DBG_CNTR_INC(o, n_lcmd_failed);
	}
	_ND(t_1clap , "locksets=@LOCKSETS[@LSI|ow=@OWNER_ID].status=@STATUS_STR retries=@RETRIES @MILISECONDS @DLBA", locksets, lock_i,owner_id, ncl_status_str(l->status), l->retries, msecs, l->address);

	if (unlikely(l->status == NCL_STATUS_CONTENDED)) {
		const u64 holder = get_contending_id(dc);
		const ulong retry_time = dp_locks_get_retry_time(l);
		const bool bad_topo = (o->topo->phased_out);
		IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_LOCK_CONTENDED_COUNT);
		if (nvmeibc_sync_is_stale(l, holder)) {
			IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_LOCK_STALE_COUNT);
		}
		_ND(tr_2_check_lock_actions, "Contention owner: locksets=@LOCKSETS[@LSI]=@LOCK_ENT_U64", locksets, lock_i, holder);
		if (bad_topo || nvmeibc_operation_does_expire_at(o, retry_time)) {	// Abort: Due to primary owner contention
			_ND(tr_3_check_lock_actions, "Giveup contented owner: retries=@RETRIES topo_fo=@RV", l->retries, bad_topo);
			IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_WAIT_FOR_LOCKSET_CANCELED_COUNT);
			__change_lock_status_to(l, NCL_STATUS_DISKDEAD);
			if (bad_topo) {
				OPERATION_DBG_CNTR_INC(o, n_topo_phased_out);
			}
		} else {	// Retry primary owner / copy / dual-lock
			l->cmds = locksets->cmds; /* critical for __retry_owner_lock */
			if (unlikely(nvmeibc_sync_is_stale(l, holder))) {
				enum stale_lock_resolve_status ss;
				if (nvmeibc_sync_is_read_only(l, holder)) { 	// EC: lock is stale but slice was not corrupted. No need to sync
					dc->compare = holder;
					return __retry_owner_lock(l, false);	// Immediately (no timer).
				}

				ss = stale_lock_resolver_get_status(&l->ds->toma_reg->hdr->slr, holder, l);
				if (ss == stale_lock_resolve_safe_to_use) { // Treat as stale special
					if (__on_stale_resolved_val(locksets, lock_i, o, holder) == 0)
						return;
				} else {
					/* Just retry after delay and hope toma will answer us. Same flow as typical contended lock */
				}
			} else if (holder == dc->lock_cnsts->unlocked_val) {		/* Previously stale, now unlocked. Retry immediately. */
				WARN_ON(dc->compare == dc->lock_cnsts->unlocked_val);	// Status should not be "contended" in this case
				dc->compare = dc->lock_cnsts->unlocked_val;
				return __retry_owner_lock(l, false);
			} else if (holder != dc->exchange) {
				/* Just contended lock, retry */
			} else {
				/* Daniel Todo: Lock is already mine but another thread locked it */
			}
			__schedule_retry_owner_lock(l, retry_time);
			return;
		}
	}

	if (l == owner_lock) { // Done testing sec/copy of owners, Analyze only primary owner loc
		if (likely(NCL_do_i_have_owner_lock(l->status))) {
			if (owner_lock->n_siblings > 1) {		// Take care of dual lock / copy locks siblings
				if (owner_lock[1].comp.lock_status != NCL_STATUS_TRANSFERRED) {
					int i, n_sibs = owner_lock->n_siblings;
					for (i = 1; i < n_sibs; i++)  // request all siblings in parallel dual/copy of owner
						__request_lock(locksets, owner_id + i);
				} /* else, don't request lock. It will be transferred very soon */
			}
		} else if (NCL_is_failed_to_acquire(l->status)) {	// giveup siblings of failed primary owner (struct nvmeibc_cmd_lock *locksets, int failed)
			int i, n_sibs = owner_lock->n_siblings;
			_ND(t_8clap, "Giveup siblings. locksets=@LOCKSETS[@LSI]", locksets, owner_id);
			for (i = 1; i < n_sibs; i++) {
				__change_lock_status_to(&owner_lock[i], NCL_STATUS_DISKDEAD);
				__invoke_crash_on_lock_corruption(locksets, owner_id + i, "take", +1);
			}
			nvmeibc_atomic_sub_return((n_sibs - 1), &owner_lock->pending);
		}
	}
	if (1) {						// __notify_cmds_state_machine: Todo: Move this code block to aux function
		int pending;
		__invoke_crash_on_lock_corruption(locksets, lock_i, "take", +1);
		pending = nvmeibc_atomic_dec_return(&owner_lock->pending);			// WARN: If pending != 0, Locks can get kfree(), trust only stack variables
		_ND(tr_6_check_lock_actions, "locksets=@LOCKSETS[@LSI|ow=@OWNER_ID] pending=@PENDING_INT", locksets, lock_i, owner_id, pending);
		WARN(pending < 0, "nvmeibc bug: locks=%p[%d|ow=%d], pend=%d", locksets, lock_i, owner_id, pending);
		if (pending == 0) {
			int sibling_idx = 0;
			for (sibling_idx = 1; sibling_idx < owner_lock->n_siblings; ++sibling_idx) {
				nvmeibc_cmd_lock_response_io_pet_describe(o, &locksets[owner_id + sibling_idx]);
			}

			BLKCMP_IO_ASYNC_RESUME_CMP(dp_transition_to_locked_cmds_sm(locksets, owner_id));
		}
	}
}

int dp_locks_calc_blockset_rv(const struct nvmeibc_cmd_lock *ow)
{
	int i, n_sibs = ow->n_siblings;
	for (i = 0; i < n_sibs; i++) {  // If at least 1 sibling failed, blockset is not protected
		if (NCL_is_failed_to_acquire(ow[i].status))
			return (NCL_is_failed_no_retry(ow[i].status) ? -ENOEXEC : -ENXIO);
		BUG_ON(!NCL_do_i_have_lock(ow[i].status));
	}
	return 0;
}

int nvmeibc_do_trim_split_if_needed(struct nvmeibc_cmd_lock *ls);

// Wait for all primary owner locks. If at least one is contended, need to split cmds
static void prediscard_proc(struct nvmeibc_cmd_lock *ls, int lsi)
{
	int rv, i, left, last_prediscard;
	if ((left = nvmeibc_atomic_dec_return(&ls[0].prediscards)) != 0) {
		_ND(trace_1_prediscard, "locksets=@LOCKSETS[@LSI] Not all PREDISCARDs returned, left=@LEFT", ls, lsi, left);
		return;
	}

	_ND(trace_2_prediscard, "locksets=@LOCKSETS[@LSI] All PREDISCARDs returned", ls, lsi);
	rv = nvmeibc_do_trim_split_if_needed(ls);
	relink_trim_split_cmds(ls, rv);

	/* Simulate callbacks on all the prediscard locks (with old/new cmds). */
	last_prediscard = ls[ls->nlocks-1].owner_idx;	// Once last ow gets cb(), kfree() occurs, so cache it on stack
	for (i = 0; i <= last_prediscard; i+= left) {	// Traverse all owners
		struct nvmeibc_cmd_lock *lo = &ls[i];
		left = lo->n_siblings;
		lo->type = NVMEIBC_CMD_LOCK_OWNER;	// Change from NVMEIBC_CMD_PREDISCARD
		_ND(trace_3_prediscard, "locksets=@LOCKSETS[@LSI] DISCARD OWNR.pending=@PENDING_INT", ls, i, nvmeibc_atomic_read(&(lo->pending)));
		__check_lock_actions(ls, i);
	}
}

static int __lock_response_cb(struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l = lock_of_bcomp(dc), *locksets = dp_locks_get_locks_header(l);
	const int lsi = l->lockset_idx;
	const bool rv1 = NCL_is_failed_to_acquire(dc->lock_status) || (dc->lock_status == NCL_STATUS_CONTENDED);

	(void)tag;
	_ND(trace_1_lock_cb, "locksets=@LOCKSETS[@LSI] @DLBA", locksets, lsi, l->address);
	dp_locks_trace_lock_comp(locksets->cmds->o, l, dc);
	nvmeibc_profiling_end_take_cmd_stats_for_op(__raid_gp_profile_for_rwt_op_locks(l, locksets), l->ds->lock_operation_profiler, locksets->cmds->o, l->type, l, rv1);

	if (NCL_had_acquire_callback(dc->lock_status)) {	// Decrease the transferring counter, to allow PAUSE arrive safely
		nvmeibc_pd_cb_called_comp(l->ds->disk, dc);	// No callback issued -> immediate error -> decreased trasnsferring counter. If callback was issued, we have to decrease it now.
	}
	DEBUG_LOCKS_CONTENTION(dc);
	l->status = dc->lock_status;		// Copy transport layer 'rv' into locks status
	if (unlikely(l->type == NVMEIBC_CMD_PREDISCARD)) {
		prediscard_proc(locksets, lsi);
	} else {
		__check_lock_actions(locksets, lsi);
	}
	return 0;
}

static void __request_lock(struct nvmeibc_cmd_lock *locksets, int lsi)
{
	struct nvmeibc_cmd_lock *l = &locksets[lsi];
	struct nvmeibc_d_rdma_comp *dc = &l->comp;
	struct nvmeibc_disk_segment *seg = l->ds;
	int const l_type = l->type; //lock may be free, when we decide to trace it
	unsigned long started = 0, lock_rqsted = 0, callback = 0;
	int rv = 0;

	nvmeibc_profiling_start_take_cmd_stats_for_op(__raid_gp_profile_for_rwt_op_locks(l, locksets), seg->lock_operation_profiler, locksets->cmds->o, l->type, l);
	WARN(l->status != NCL_STATUS_NOTISSUED, "nvmeibc bug: locks=%p[%d].status=%d", locksets, lsi, l->status); // Incorrect flow initialized
	_ND(trace_req_lock, "locks=@LOCKSETS[@LSI] status=@STATUS_STR @DLBA", locksets, lsi, ncl_status_str(l->status), l->address);
	started = jiffies;
	if (!l->retries) { // Owner (primary/second/copy)
		l->last_retry_report_time = l->first_try_time = jiffies;
	}
	l->status = NCL_STATUS_ISSUED;						// Issue owner request
	nvmeibc_cmd_lock_request_io_pet_describe(locksets->cmds? locksets->cmds->o : NULL, l);
	rv = nvmeibc_pd_cmpxchg(seg->disk, handle_of(seg), l->address, dc);
	lock_rqsted = jiffies;
	if (unlikely(rv)) { // handle pausable/transport layer immediate errors
		__give_failed_lock_cb(dc);
	} /* Note: Here locksets may be already free(), Do not access it */
	callback = jiffies;
	if (__SUSPICIOUS_LOCK_REQ_TIME < jiffies_to_msecs(callback - started)) {
		_NT(trace_1_request_lock_too_long, "Retry locksets=@LOCKSETS[@LSI] type=@LOCK_TYPE_INT lock_rqsted=@MILISECONDS callback=@MILISECONDS total_duration=@MILISECONDS", locksets, lsi, l_type, jiffies_to_msecs(lock_rqsted - started), jiffies_to_msecs(callback - lock_rqsted), jiffies_to_msecs(callback - started)); (void)lock_rqsted;
	}
}

static void __send_locks_of_raid_write(struct nvmeibc_cmd_lock *locks, int lsi)
{
	__request_lock(locks, lsi);	// request only the owner, it will request the rest
}

static void __send_locks_of_raid_read(struct nvmeibc_cmd_lock *locks, int lsi)
{
	struct nvmeibc_cmd_lock *lo = &locks[lsi];
	if ((lo->type == NVMEIBC_CMD_LOCK_READ_PB) || (lo->type == NVMEIBC_CMD_LOCK_READ_DR)){
		/* Will be piggibacked or read after command, request nothing */
	} else { /* Take locks on read, much like write */
		return __send_locks_of_raid_write(locks, lsi);
	}
}

u64 get_lockid_for_cmpxchg(const struct nvmeibc_raid1 *r1, enum nvmeib_block_io_op op) {
	union nvmeib_lock_blkset_entry rv;
	rv.all = r1->lid.all;
	if (nvmeibc_raid_is_ec(r1)) { // Only writes use journal, read/sync does not
		rv.lock_id.bits.is_read = (!nvmeib_block_io_op_is_write(op));
	} else {
		// We don't have is_read bit in mirror (currently)
	}
	return rv.all;
}

void dp_locks_send_all(struct nvmeibc_cmd_lock *locksets)
{
	int lsi, lsi_start = 0, n_locks = locksets->nlocks, end_req_locks = 0;
	const enum nvmeib_block_io_op op = locksets->cmds->o->op;
	/* Prepare the locks for request. Must prepare all in advance */
	for (lsi = 0; lsi < n_locks; lsi++) {
		struct nvmeibc_cmd_lock *l = &locksets[lsi];
		struct nvmeibc_d_rdma_comp *dc = &l->comp;
		__print_lock_to_log(locksets,lsi);
		switch (l->type) {
		case NVMEIBC_CMD_PREDISCARD:
		case NVMEIBC_CMD_LOCK_OWNER:
		case NVMEIBC_CMD_LOCK_COPY_OWNER:{
			struct nvmeibc_raid1* r1 = nvmeibc_disk_segment_get_praid(l->ds);
			dc->code = NVMEIBC_CMD_LOCK_OWNER;		// Daniel: always do cmpxchg (even for copy owners). For debug!
			dc->callback = &__lock_response_cb;
			dc->compare = nvmeibc_raid1_get_lock_consts(r1)->unlocked_val;
			dc->exchange = get_lockid_for_cmpxchg(r1, op);
			end_req_locks = lsi + 1;	// We will loop until last lock to request. After it the operation can finish within the loop
			break;
		}
		case NVMEIBC_CMD_LOCK_READ_PB:
		case NVMEIBC_CMD_LOCK_READ_DR:
			WARN(l->n_siblings != 1, "nvmeibc bug. n_sibs=%d\n", l->n_siblings);
			break;		/* Just skip it, it was initialized/piggibacked */
		default:
			WARN_ON_ONCE(true);	/* Other types of locks should not be sent */
			break;
		}
	}

	if (!__IO_LT_is_allowed_to_transfer_locks(op)) {
		__IO_LT_refuse_transfer(locksets);
	} else {
		lsi_start = __IO_LT_try_request_transfer(locksets);
	}

	for_each_primary_owner_safe_loop_start(lsi, locksets, lsi_start, end_req_locks)
		switch (op) {
			case NVMEIB_BLOCK_IO_OP_WRITE:  __send_locks_of_raid_write(locksets,lsi); break;
			case NVMEIB_BLOCK_IO_OP_READ:	__send_locks_of_raid_read( locksets,lsi); break;
			case NVMEIB_BLOCK_IO_OP_DISCARD:__send_locks_of_raid_write(locksets,lsi); break;
			default: BUG();
		}
	for_each_primary_owner_safe_loop_end();
}

void dp_locks_resend_raid_locks(struct nvmeibc_cmd_lock *locks)
{
	WARN_ON(!nvmeib_block_io_op_is_write(locks->cmds->o->op));
	__send_locks_of_raid_write(locks, 0);
}

/*********************** Locks and Binfo *******************************/
union nvmeib_blkset_info dp_locks_get_TxID_dbits(const struct nvmeibc_cmd_lock *locksets, int owner_i, bool only_owner)
{
	/* Daniel: Important!!! If seconday owner exists, must take its value coz
	   it might be the sole owner in next topology and have higher TxID */
	const struct nvmeibc_cmd_lock *lo = &locksets[owner_i];
	union nvmeib_blkset_info ow_rv, so_rv;
	struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(lo->ds);
	ow_rv.all = nvmeibc_get_binfo_of_lock(lo);
	if (lo->secondary_id && (!only_owner)) {
		union nvmeibc_dbits_entry ow_dbits, so_dbits;
		int i, n_sibs = lo->n_siblings;
		for (i = 1; i < n_sibs; i++) {  // merge all copy-of-owner to owner
			if (unlikely(!nvmeibc_is_readable(lo[i].ds)))			// Non readble segments - cant trust their binfo
				continue;
			so_rv.all = nvmeibc_get_binfo_of_lock(&lo[i]);
			ow_dbits.all_bits = ow_rv.bits.dirty;
			so_dbits.all_bits = so_rv.bits.dirty;
			/*TOOD: (Ofir) uncomment, this warn_on if failing because when destroying slice no_Whole sync doesn't commit binfo, instead it returns so->error.
			  WARN_ON((ow_rv.bits.txid == INITIAL_LAZY_READ_TXID) != (so_rv.bits.txid == INITIAL_LAZY_READ_TXID)); // Should never happen: If one lock has unknown txid_id and the other has valid txid than taking the max of them will result in possibly inaccurate txid (in the dmds it might be greater).
			*/
			ow_rv.bits.txid = max((u32)ow_rv.bits.txid, (u32)so_rv.bits.txid);
			ow_rv.bits.dirty = nvmeibc_dbits_intersect_owners(&ow_dbits, &so_dbits, nvmeibc_raid1_get_protect_lvl(pr));
		}
		// nvmeibc_get_binfo_of_lock(lo) = ow_rv.val;	// Finally re-inject the merged back to owner. Daniel: for debug reasons dont do that yet
		// When owner and copies could cmpxchg from different 0/stale values. Only the stale of primary owner counts
	}
	return ow_rv;
}

void dp_locks_put_TxID_dbits(struct nvmeibc_cmd_lock *locksets, int owner_i, union nvmeib_blkset_info binfo, bool can_put_unknown_txid)
{
	struct nvmeibc_cmd_lock *lo = &locksets[owner_i];
	const struct operation *o = locksets->cmds->o;		// Only for prints
	int i, n_sibs = lo->n_siblings;
	for (i = 0; i < n_sibs; i++) {
		const bool wrong_txid = (binfo.bits.txid > NVMEIBC_DP_EC_MD_TX_ID_MAX) ||
						(!can_put_unknown_txid && (binfo.bits.txid == INITIAL_LAZY_READ_TXID));
		WARN(wrong_txid, "NVMesh Bug: volume %s: o{%u32}.op=%u, Attempt to inject invalid txid=0x%x to locks\n", o->nd->name, o->dbg_id, o->op, binfo.bits.txid);
		nvmeibc_get_binfo_of_lock(&lo[i]) = binfo.all;
	}
}

/* Prepare & execute sub state machine which writes all blockset infos */
void dp_locks_write_all_blocksets_info_op(struct nvmeibc_cmd_lock *ow_l, const union nvmeib_blkset_info *binfo, int (*callback)(struct nvmeibc_d_rdma_comp*, struct nvmeibc_d_rdma_comp_tag), int prev_rv)
{
	int i, err, nlocks = ow_l->n_siblings;
	for (i = 0; i < nlocks; i++) { // Daniel: Note, we traverse all locks, so we do commit binfo to W- segment. This is not mandatory when turning dbits on, but mandatory when turning off. We refrain from optimizations and always commit to W-
		struct nvmeibc_cmd_lock *l = &ow_l[i];
		struct nvmeibc_d_rdma_comp *dc = &l->comp;
		dc->lock.bi =  binfo->all;
		dc->callback = callback;
		if (likely(prev_rv == 0)) {	/* Send the lock info */
			dc->lock_status = NCL_STATUS_NOTISSUED;	// Lock is taken but we use its comp for binfo
			nvmeibc_blkset_info_write_pet_describe(ow_l->cmds, l->address, dc);
			err = nvmeibc_pd_write_blkset_info(l->ds->disk, handle_of(l->ds), l->address, dc);
		} else {
			err = prev_rv;
		}
		if (unlikely(err)) {
			__give_failed_lock_cb(dc);
		}
	}
}

/***************************** Locks Tracing **********************************/
// Lock to blockset number in segment
#define __lock_blockset(l_) (((l_).address - __to4K((l_).ds->first_lba)) >> LOCKSET_4KS_SHIFT)
void dp_locks_trace_lock_comp(const struct operation *o, const struct nvmeibc_cmd_lock *l, const struct nvmeibc_d_rdma_comp *dc)
{
	NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: @LOCK_COMPLETION_DUMP", _T, goodpath_nvmeibc_locks, lock_comp,        o->dbg_id    , l->ds->dbg_uuid, __lock_blockset(*l), l->type, dc->lock_status, dc->compare, dc->exchange, get_contending_id(dc), l->retries, (jiffies - l->first_try_time));
	//do not call PET here - this function is called from multiple contexts
}

void dp_locks_trace_lock_release(const struct operation *o, const struct nvmeibc_cmd_lock *l)
{
	struct nvmeibc_d_rdma_comp const* dc = &l->comp;
	NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: @LOCK_RELEASE_DUMP",    _T, goodpath_nvmeibc_locks, lock_release, o ? o->dbg_id : 0, l->ds->dbg_uuid, __lock_blockset(*l),                                        dc->exchange);
	//do not call PET here - this function is called from multiple contexts
}
