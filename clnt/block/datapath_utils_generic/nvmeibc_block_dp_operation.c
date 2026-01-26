#include "nvmeib_types.h"
#include "nvmeibc_block_dp_common.h"
#include "nvmeibc_pausable.h"
#include "block/nvmeibc_topology.h"
#include "block/nvmeibc_block_common.h"
#include "nvmeibc_block_dp_dbg_tools.h"
#include "../datapath_ec/nvmeibc_block_dp_ec.h"
#include "../datapath_mirror/nvmeibc_block_dp_mirror.h"
#include "nvmeibc_block_dp_io_req_rel_locks.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeib_io_stats.h"

#include "operation/nvmeibc_block_dp_operation_per_cpu.h"
#include "operation/nvmeibc_block_dp_operation_throttling.h"
#include "nvmeibc_io_pet.h"

NVMEIBC_MEMMGR_METRIC(dp_io_operation, "component=raid.io.user");
NVMEIBC_MEMMGR_METRIC(dp_sync_operation, "component=raid.io.sync");

/******************************** Operation ***********************************/
static void __on_complete_update_operation_stats(struct operation *o)
{
	struct nvmeibc_block_device *nd = o->nd;
	const u64 io_duration_nsecs = ((u64)(jiffies - o->jiffies1) * NSEC_PER_SEC) / HZ;
	u64 latency, io_exec;
	struct nvmeib_io_counters io_counters;
	#ifdef DEBUG
		WARN_ON(!nvmeib_stop_watch_is_start(&o->time.exec));
	#endif
	#ifndef DP_LIB
	latency = nvmeib_stop_watch_measureq(&o->time.total);
	io_exec = nvmeib_stop_watch_measureq(&o->time.exec);

	// instead of calling nvmeib_io_stats_operation_end, we set inflight_ops to -1
	// to indicate that this operation is not inflight anymore. this approach is used to avoid
	// needless locking at nv
	io_counters = (struct nvmeib_io_counters) {
		.total_ops = 1,
		.total_executions = o->time.exec.num_measures,
		.inflight_ops = 0,      // inflight op is decremented at nvmeibc_operation_destroy
		.total_size = ((u64)o->io_stat_length << NVMEIBC_SECTOR_SHIFT),
		.total_latency = latency,
		.total_latency_sqr = nvmeib_square_latency(latency),
		.total_io_exec = io_exec,
		.total_e2e_exec = io_duration_nsecs,
		.worst_latency = latency,
		.worst_io_exec = io_exec,
		.worst_e2e_exec = io_duration_nsecs,
		.total_sub_block = 0
	};
	nvmeib_io_stats_update_one(nd->os->stats, NULL, io_op_to_verb(o->op, false), io_counters);
	#else
	(void)latency;	// Avoid warning in lib
	(void)io_exec;	// Avoid warning in lib
	(void)io_counters;	// Avoid warning in lib
	#endif
	if (unlikely(io_duration_nsecs > 30*NSEC_PER_SEC)) {
		if (nvmeibc_operation_is_bio(o)) {
			const u64 st_B = get_op_start_lba(o), n_B = get_op_nlbas(o);
			_NW(t_01_ioop, DMESG_PREFIX("@DEV_NAME") ": slow IO, op=@BLOCK_IO_OP, vlba=[@VLBA..@VLBA)[blks] flags=@LLX {@O_DBG_ID} took @MILISECONDS", nd->name, o->op, st_B, st_B + n_B, o->dbg_cntrs.raw, o->dbg_id, (u32)(io_duration_nsecs/USEC_PER_SEC));
		}
	}
}

static void __compressed_op_trace_end(const struct operation *o, int rv) {
	if (nvmeibc_operation_is_bio(o)){
		if (o->op == NVMEIB_BLOCK_IO_OP_READ) {
			NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: Operation end: RV: @RV", _T, goodpath_nvmeibc, compressed_op_trace_end_bio_r , o->dbg_id, rv);
		} else {
			NVMEIB_LOG_GOODPATH("{@O_DBG_ID}: Operation end: RV: @RV", _I, goodpath_nvmeibc, compressed_op_trace_end_bio_wt, o->dbg_id, rv);
		}
		NVMEIBC_IO_PET_MSG_NORM(&o->journal, "operation.end(rv=%d<errno>)", rv);
	}
}

/************** Relationship between bio_part and operation *******************/
#define BPART_LDR_NREFS (0x20000000)								// Leader give completion twice. First time like the rest with -1. Second time with this value to free the operation
#define _NBP(fmt, ...) //_Emerg(fmt, ##__VA_ARGS__)

void nvmeibc_operation_start_bio_part(struct bio_part *ldr, int n_refs)
{
	nvmeibc_atomic_set(&ldr->n_ref, n_refs + BPART_LDR_NREFS);
	_NBP("O$ set, %p, r=0x%x, o=%p\n", ldr, n_refs + BPART_LDR_NREFS, nvmeibc_operation_of_bio_part(ldr));
}

void nvmeibc_operation_add_bio_part(struct bio_part *ldr)
{
	nvmeibc_atomic_inc(&ldr->n_ref);		// Just a single reference for all parts
	nvmeibc_operation_of_bio_part(ldr)->flags.was_bio_part_split = true;
	_NBP("O$ ++, %p\n", ldr);
}

void nvmeibc_operation_free_bio_part(struct bio_part *b)
{
	if (b) {
		nvmesh_memmgr_metric_on_free_update(dp_io_operation, PAGE_SIZE);
	}

	kfree(b);
}

static void __free_bio_part(struct bio_part *b)
{
	struct operation *o = nvmeibc_operation_of_bio_part(b);
	if (!o->flags.is_clmat_embedded) {
		nvmeibc_operation_free_bio_part(b);
	}	// Else: locks will call nvmeibc_operation_free_bio_part when the complete
}

// Called at most twice by each bio part: 1. Optional 'can_free == false', 2. Mandatory 'can_free==true'
static inline void __complete_bio_part_possibly_free(struct bio_part *cur, unsigned long start_time, int rv, bool can_free)
{
	struct bio_part *ldr = cur->ref;
	const bool is_leader = (ldr == cur);
	int r;

	// ------     Fast path implementation for bio that was in fact never split, avoid atomic calculations
	if (is_leader) {
		struct operation *o_ldr = nvmeibc_operation_of_bio_part(ldr);
		if (!o_ldr->flags.was_bio_part_split) {
			_NBP("O$ FEnd, %p, last=%d\n", ldr, can_free);
			if (!can_free)
				block_api_os_end_io(cur, start_time, rv);
			else
				__free_bio_part(ldr);
			return;
		}
	}

	// -----      Slow path, handle split of rldr
	if (can_free) {
		if (!is_leader) {
			__free_bio_part(cur);
			return;
		}
		r = nvmeibc_atomic_sub_return(BPART_LDR_NREFS, &ldr->n_ref);
		_NBP("O$ End, %p, r=0x%x\n", ldr, r);
	} else {						// Real completion
		if (rv)
			ldr->rv = rv;			// Leader stores the last error
		r = nvmeibc_atomic_dec_return(&ldr->n_ref);
		_NBP("O%c End, %p, r=0x%x\n", (is_leader ? '$' : '^' ), ldr, r);
	}
	if ((r == BPART_LDR_NREFS) ||			// All real completions finished, ldr did not give its last callback yet.
	   (!is_leader && (r == 0))) {	// All real completions finished, non ldr gave real completion after ldr finished last callback
		block_api_os_end_io(cur, start_time, ldr->rv);
	}
	if (r == 0) {
		_NBP("O Free, %p, o=%p\n", ldr, (void*)&ldr[1]);
		__free_bio_part(ldr);					// Only now it is safe to fre the leader
	}
}

static inline void __complete_all_bparts_possibly_free(struct operation *o, int rv, bool can_free)
{
	if (o->flags.is_allocated_with_bio_part) {
		int i;
		// Note: if (o->num_bios == 0) this means that operation was merged by elevator. It will not free itself but rather keep the buffer for the bio_part
		for (i = o->num_bios-1; i >= 0; --i) {				// Reverse order because completion of bios[0] can free the operation itself
			__complete_bio_part_possibly_free(o->bios[i], o->jiffies1, rv, can_free);
		}
	} else {
		WARN_ON(!can_free);
		nvmesh_memmgr_metric_on_free_update(dp_sync_operation, ksize(o));
		kfree(o);
	}
}

static inline void __complete_all_bparts(struct operation *o, int rv)
{
	__complete_all_bparts_possibly_free(o, rv, false);
}

static inline void __operation_free(struct operation *o)
{
	__complete_all_bparts_possibly_free(o, ~0 /* meaningless value*/, true);
}

/**************************** vv_bio_inter ************************************/
void vv_bio_inter_init_thick(union vv_bio_inter *vbi, const struct operation *o)
{
	vbi->bi = __BI_INIT(o->bios[0]);
	vbi->nb = 0;
	vbi->nbi = NPS_BLOCK_ITER_INIT(o->pages);
	vbi->on_op_read_use_private_blocks = nvmeibc_operation_is_bio_copy_needed_for_read(o);
	if (vbi->on_op_read_use_private_blocks){
		vbi->private_read_blocks_iter = NPS_BLOCK_ITER_INIT(o->pages);
		nps_block_iter_advance(&vbi->nbi, get_op_nlbas(o));
	}
}

/******************************************************************************/
#ifdef DEBUG_NON_DIRECT_IO
static int __was_bio_changed_in_air(const u64* p0, const u64* p1, const int n_u64, const char* h0, const char* h1, const void *o)
{
	int i;
	if (memcmp(p0, p1, n_u64*8) != 0) {
		for (i = 0; i < n_u64; i++) {
			if (p0[i] != p1[i])
				_NE(t_xb_dp_dbg_tools, "BIO_v={@INT) @STR(@LLX) != @STR(@LLX)}, o=@OPERATION", i, h0, p0[i], h1, p1[i], o);
		}
		return true;
	}
	return false;
}
static void __verify_non_direct_bug(const struct operation *o)
{
	_NI(t_xc_dp_dbg_tools, "BIO_e={page=@PTR} o=@OPERATION", o->cp.buf, o);
	if (o->cp.buf) {
		const int n_u64 = ARRAY_SIZE(o->cp.bio0);
		__was_bio_changed_in_air(o->cp.bio0, o->cp.bio1, n_u64, "b0", "b1" , o);
		__was_bio_changed_in_air(o->cp.bio0, o->cp.buf , n_u64, "b0", "bio", o);
		__was_bio_changed_in_air(o->cp.bio1, o->cp.buf , n_u64, "b1", "bio", o);
	}
}

void __non_direct_init_verification(struct operation *o)
{
	const enum nvmeib_block_io_op op = o->op;
	if ((nvmeib_block_io_op_is_write(op))||(op == NVMEIB_BLOCK_IO_OP_READ)) {
		bio_iter_t bi = __BI_INIT(o->bios[0]);	// Todo: Use vv_bio_inter_init_thick() instead. Currently dont care coz we use only first bio anyways
		struct bio_vec bv = __get_bio_vec(o->bios[0], &bi, NVMEIBC_SECTOR_SIZE);
		const void *buf = page_address(bv.bv_page);
		const u8 *first_bytes = &((const u8*)buf)[bv.bv_offset];
		_NI(t_xd_dp_dbg_tools, "BIO_s={page=@PTR,op=@OP}, offset=@X, o=@OPERATION", buf, op, (u32)bv.bv_offset, o);
		if ((nvmeib_block_io_op_is_write(op))) {
			memcpy(o->cp.bio0, first_bytes, sizeof(o->cp.bio0));
			memcpy(o->cp.bio1, first_bytes, sizeof(o->cp.bio1));
			o->cp.buf = (void*)first_bytes;
		}
	}
}
#else
	#define __verify_non_direct_bug(o)
#endif // DEBUG_NON_DIRECT_IO

static inline void __operation_extended_end_bio(struct operation *o, int rv)
{
	int i;
	if ((rv) && (rv != -EXDEV)) {									// If error occured and not autofailed by rider
		const u64 slba = get_op_start_lba(o), nlbas = get_op_nlbas(o);
		BUILD_BUG_ON(sizeof(o->dbg_cntrs.raw) < sizeof(o->dbg_cntrs));
		for (i = 0; i < o->num_bios; ++i)
			_NW(warn_dp_operation_extended_end_bio, DMESG_PREFIX("@DEV_NAME") ": Failed IO, operation_type=@BLOCK_IO_OP, vlba=[@VLBA..@VLBA)[blks]. operation=@OPERATION flags=@LLX bio=@BIO, rv=@RV", o->nd->name, o->op, slba, slba + nlbas, o, o->dbg_cntrs.raw, o->bios[i], rv);
	}
	__verify_non_direct_bug(o);
	// __operation_extended_rider_io_end_bio(o, rv);
	__complete_all_bparts(o, rv);
}

static void __operation_extended_resubmit(struct operation *o)
{
 	//__operation_extended_rider_io_resubmit(o);
	//__dump_operation(o);
	if (o->mssa) {	// Topology can change we need to reset previous MSSA values
		memset(o->mssa, 0, sizeof(*o->mssa));
		o->mssa = NULL;
	}

	if (o->flags.is_clmat_embedded) {
		if (o->locks) {	// Wait for locks to complete before resubmitions
			o->locks->resubmit_operation_dont_free = true;
		} else {	// Ensure that we do not free embeded jbod operation rather we resubmit it
			o->flags.resubmit_operation_no_free = true;
		}
		// After resubmittion, in new topology locks may not be embedded
		o->flags.is_clmat_embedded = false;
	} else {
		nvmeibc_io_resubmitter_retry_op(o);
	}
}

void nvmeibc_operation_move_mem_to_locks(struct operation *o)
{
	if (o->locks) { 					// If there are locks, they will refer to the topo during the release process, so they should "put" it upon their release
		o->locks->topo = o->topo;
		DEBUG_TOPO_CNTRS_move_elem_on_topo(o, o->locks);
		o->locks->resubmit_operation_dont_free = false;			// Default initialization
		o->locks->pg = NULL;									// Default initialization
		if (o->flags.is_clmat_embedded)
			o->locks->pg = o;									// Upon locks completion, either resubmit operation or free its memory. Anyways, pointer to 'o' is needed
		o->topo = NULL;
	}
}

static bool __check_rider_error(struct dp_io_stats *io_stats, int o_rv)
{
	if (o_rv == -EXDEV) {

		IO_STATS_INCR(io_stats, DP_IO_STATS_CANCELED_BY_RIDER);
		return true;
	}
	return false;
}

static void __on_complete_io_stats(struct dp_io_stats *io_stats, struct operation *o, int o_rv)
{
	if (!o_rv)
		return;

	IO_STATS_ADD(io_stats, DP_IO_STATS_LOCK_OP_FAILED, o->dbg_cntrs.n_lcmd_failed);

	if (!__check_rider_error(io_stats, o_rv)) {
		IO_STATS_INCR(io_stats, DP_IO_STATS_OTHER);
	}
}

void nvmeibc_operation_complete(struct operation *o, bool do_retry, int o_rv)
{
	struct nvmeibc_block_device *nd = o->nd;
	nvmeibc_operation_move_mem_to_locks(o);
	__compressed_op_trace_end(o, (do_retry ? -EAGAIN : o_rv));

	/* account this execution attempt */
	nvmeib_stop_watch_stop(&o->time.exec);

	if (!do_retry) {
		__on_complete_io_stats(&nd->dp.io_stats, o, o_rv);
		if (block_api_os_is_io_api_enabled(nd->os)) {
			nvmeib_stop_watch_stop(&o->time.total);
			__on_complete_update_operation_stats(o);
		}
		nvmeibc_operation_destroy(o, o_rv);
	} else {
		__operation_extended_resubmit(o);
	}
}

static void operation_print_wrong_input(const struct operation *o, int rv)
{
	const u64 st_B = get_op_start_lba(o), n_B = get_op_nlbas(o);
	const u64 st_b = get_start_b(o->bios[0]), n_b = get_op_length(o);
	_NW(op_wrong_input, DMESG_PREFIX("@DEV_NAME") ": wrong IO, rv=@RV op=@BLOCK_IO_OP, vlba=[@VLBA..@VLBA)[blks], [@VLBA_BYTES..@VLBA_BYTES)[bytes]", o->nd->name, rv, o->op, st_B, st_B + n_B, st_b, st_b + n_b);
}

static void operation_abort_on_error_b4_execution_started(struct operation *o, int rv)
{	/* Assume rv < 0, Cleanup memory allocs, Errors are returned solely via bio_endio() */
	operation_print_wrong_input(o, rv);
	if (rv == -EPIPE)
		IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_SUSPED_FAIL);
	else if (rv == -EACCES)
		IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_IGNORED_ERR);
	else
		IO_STATS_INCR(&o->nd->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL);
	nvmeibc_operation_destroy(o, rv);
}

// Function below, somewhat equivalent to nvmeibc_operation_put() at the end of IO execution
static void nvmeibc_operation_put_upon_prepare_op_failure(struct operation *o, const int rv)
{
	_NW_to_user(t_02_oppof, DMESG_PREFIX("@DEV_NAME"), "Internal error in IO execution that is not expected to happen, contact Excelero support. Error code: 1006. Return code: @RV. Volume Type: @HDR_TYPE. Operation: @OP", o->nd->name, rv, o->nd->type, o->op);
	dp_cmds_free_all(o->cmds);
	if (o->locks) {
		dp_cmds_free_all(o->locks->new_cmds);
		if (!o->flags.is_clmat_embedded)
			nvmeibc_clmat_free_dangling_locks(o->locks);
		o->flags.is_clmat_embedded = false;				// Locks will not be acquired and not released (not take control of the operation memory). Regardless whether operation is going to resubmition or returns failure to user space
	}
	nvmeibc_pages_free(&o->pages, get_tcp_mode_of_operation(o));

	if (rv == -EAGAIN) {
		__operation_extended_resubmit(o);
	} else {
		operation_abort_on_error_b4_execution_started(o, rv);
	}
}

static void __operation_execute_no_cache(struct operation *o, int rv)
{
	struct nvmeibc_datapath *dp = &o->nd->dp;

	nvmeib_stop_watch_start(&o->time.exec);

	if (!rv)
		rv = dp->prepare_op(o);

	if (unlikely(rv < 0)) {
		nvmeibc_operation_put_upon_prepare_op_failure(o, rv);
	} else {
		struct nvmeibc_block_command *cmds = o->cmds;
		DEBUG_TRANSFERS_init_cb_counters(cmds->ncmds, cmds);
		__uncompleted_cmds_list_add(&cmds->iocmd);
		BLKCMP_IO_ONLY_IF_PRESERVE_STACK(BUG_ON(o->locks && (o->locks->n_siblings != o->locks->nlocks)));	// Trap, IO with multiple state machines (more than 1 blockset length)
		nvmeibc_operation_compressed_op_pet_dump_bio(o);
		dp->execute_op(o);
	}
}

static void __execute_chain_noplug(struct operation *o)
{
	while (o) {
		struct operation *next = o->chained_op; /*After execute, operation may be already destroyed, take next now*/
		nvmeibc_operation_execute(o, true);
		o = next;
	}
}

void nvmeibc_operation_execute_chain(struct operation *o)
{
	__mini_elevator_start_plug(o->nd);								// Plug is started only when user space submits IO
	__execute_chain_noplug(o);
}

struct operation *nvmeibc_operation_create_with_biopart(u32 op_size, struct nvmeib_pet_base_controller* io_pet_controller)
{
	const u32 bp_size = sizeof(struct bio_part);
	void *buf = kmalloc(PAGE_SIZE, GFP_NOFS);
	BUILD_BUG_ON((((sizeof(struct bio_part)/8)*8)) != sizeof(struct bio_part));
	nvmesh_memmgr_metric_on_alloc_update(dp_io_operation, PAGE_SIZE, buf);
	if (buf) {
		struct operation *o = (buf + bp_size);
		memset(buf, 0, bp_size + op_size);
		o->bios[0] = buf;
		o->flags.is_allocated_with_bio_part = true;
		o->journal = nvmeibc_io_pet_journal_make(io_pet_controller);
		return o;
	}
	return NULL;
}

void* nvmeibc_operation_alloc_from_sufix(const struct operation *o, u32 alloc_size)
{
	if ((o->flags.is_allocated_with_bio_part)&&(!o->flags.was_bio_part_split)) {
		// When bio is split - leader cannot share memory with locks or else memory corruption will occur
		const u32 o_size = o->nd->dp.sizeof_operation;
		const u32 remaining_size = (u32)PAGE_SIZE - (o_size + (u32)sizeof(struct bio_part));
		if (remaining_size >= alloc_size)
			return (void*)o + o_size;
		// Here: in 1 page the memory stores the following structs {bio_part, operation, locks[n_locks], clmat[ncmds][nlocks], bcmds[ncmds], iocmds[ncmds], ndbs[ncmds], sgls[ncmds][nlba]}
	}
	return NULL;						// Else, Illegal to combine locks and oepration in the same memory
}

struct operation * nvmeibc_operation_create_atomic(const struct operation *o)
{
	const u32 op_size = o->nd->dp.sizeof_operation;
	struct operation *_o = kzalloc(op_size, GFP_ATOMIC);
	nvmesh_memmgr_metric_on_alloc_update(dp_sync_operation, _o? ksize(_o):op_size, _o);
	return _o;
}

/* to be used by callers of nvmeibc_operation_create_atomic (sync) */
void nvmeibc_operation_free(struct operation *o)
{
	if (!o)
		goto out;

	nvmesh_memmgr_metric_on_free_update(dp_sync_operation, ksize(o));
	kfree(o);

out:
	return;
}

void nvmeibc_operation_execute(struct operation *o, bool is_from_user_space)
{
	// IO from kernel take topo. IO from per-cpu wait-list already took topo
	struct nvmeibc_topology *t = o->topo =
		o->topo ? : nvmeibc_topology_get(&o->nd->topologies);
	struct nvmeibc_datapath *dp = &o->nd->dp;
	int rv = 0;

	if (is_from_user_space) { /* 1st execution, not-resubmission */
		nvmeib_stop_watch_init(&o->time.total);
		nvmeib_stop_watch_init(&o->time.exec);
		nvmeib_stop_watch_start(&o->time.total);
	}

	if (o->dbg_id == 0)
		nvmeibc_operation_alloc_dbg_id(o);
	else {} 			// Exists, coz this IO failed and was resubmitted. Keep previous debug id

	if (unlikely(t == NULL)) { /* Daniel: IO arriving during device reboot */
		nvmeibc_io_resubmitter_submit_op(o);
		goto _out;
	}
	DEBUG_TOPO_CNTRS_add_elem_to_topo(o, t);
	rv = dp->should_ignore_op(o);
	if (unlikely(rv)) { // operation failes/succeeds syncronously
		NVMEIBC_IO_PET_MSG_NORM(&o->journal, "operation.should_ignore_op(rv=%d<errno>)", rv);
		if (rv > 0)
			nvmeibc_operation_destroy(o, 0);
		else
			operation_abort_on_error_b4_execution_started(o, -EACCES);	// Should never get here! Autofail IO
		goto _out;
	}

	if (!nvmeibc_operation_can_execute_in_topo(t, o)) { /* Use t, not t->nt! */
		__operation_extended_resubmit(o);
		goto _out;
	}

	if (mini_elevator && is_from_user_space && (nvmeib_block_io_op_is_write(o->op)) && (dp->elevator.max_elev_write > 0))	// Otherwise merges are not allowed
		rv = __mini_elevator_try_unify_op(o, dp->elevator.max_elev_write, &o);	// Note: here value of 'o' might change
	else { /* rv remains 0. Includes ops like destage, read, trim  */}
	if (rv <= 0) // Elevator is not delaying execution or has merged it
		__operation_execute_no_cache(o, rv);
_out:;
}

void nvmeibc_operation_destroy(struct operation *o, int rv)
{
	const ulong now_jiffies = jiffies;
    struct nvmeibc_block_device *nd = o->nd;
	enum nvmeib_pet_severity const severity = rv ? NVMEIB_PET_SEVERITY_WARNING : NVMEIB_PET_SEVERITY_NORMAL;
	if (o->op < NVMEIB_BLOCK_IO_OP_DISCARD) {
		const u64 nlbas = get_op_nlbas(o);
		nvmeib_io_stats_operation_end(o->nd->os->stats, io_op_to_verb(o->op, false),
					      nlbas << NVMEIBC_SECTOR_SHIFT);
	}
	if (rv < 0 && (now_jiffies - o->jiffies1 > nd->max_retry_jiffies)) {
		IO_STATS_INCR(&nd->dp.io_stats, DP_IO_STATS_TIMED_OUT);
	}
	__ndump_operation(operation_destroy, o);
	nvmeibc_clmat_free(o);
	nflog(flog_dp_operation_nvmeibc_operation_destroy, "IO completed: o=@OPERATION now[jiffies]=@NOW_JIFFIES, diff[jiffies]=@DIFF_JIFFIES",o, now_jiffies, now_jiffies - o->jiffies1);
	__operation_extended_end_bio(o, rv);
	DEBUG_TOPO_CNTRS_del_elem_from_topo(o);
	nvmeibc_operation_throttling_pull_next(o->nd, o->cpu_id, (o->chained_op != NULL));
	nvmeibc_topology_put(o->topo);
	NVMEIBC_IO_PET_MSG(&o->journal, "operation.destroy(rv=%d<errno>)", severity, rv);
	nvmeib_pet_journal_commit(&o->journal);
	__operation_free(o);
}

bool nvmeibc_operation_does_expire_at(struct operation *o, ulong future)
{
	const ulong io_timeout = o->nd->max_retry_jiffies;
	if (future - o->jiffies1 > io_timeout) {
		/* TODO: 
		 * this code does not look right. io_timeout may be 2**30; 
		 * jiffies (current time) is ~2**10 thus we get underflow; very hard to reason about correctness in multiple places
		 * */
		o->jiffies1 = jiffies - io_timeout - 1; // Ensure no resubmission now because in future this will timeout
		return true;
	}
	return false;
}

unsigned long nvmeibc_operation_get_expiry_jiffies(const struct operation *o)
{
	return o->jiffies1 + o->nd->max_retry_jiffies;
}

int nvmeibc_operation_get_cinst_params_core_tcp_mode(const struct operation *o) {
	return nvmeibc_cinst_params_blk_get_cinst_params_core_tcp_mode(o->nd->cips);
}
