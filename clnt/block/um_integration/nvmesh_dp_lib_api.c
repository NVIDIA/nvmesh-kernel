/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "common/kr_incs.h"
#include "common/nvmeib_shared.h"
#include "clnt/block/controlpath/nvmeibc_b_cp_lost_srv_resources.h"
#include "nvmesh_dp_lib_api.h"
#include "block/recovery/nvmeibc_block_dp_sync_api.h"
#include "block/nvmeibc_block_common.h"
#include "block/recovery/nvmeibc_raid_recovery.h"
#include "nvmeibc_icore_ops.h"
#ifdef BLKDEV_SIMULATOR
	#error This file should not include anything from kernel simulator!
#endif

/***************************** Client instance layer **************************/
struct nvmeibc_cinst {		// Client instance. Only 1 exists
	struct nvmeibc_cinst_params_blk pb;
	void *ppb;
	struct nvmesh_dp_lib_virtual_table vtable;
} cinst = { .pb = {NULL, 0}, .ppb = NULL, {} };
static inline bool _is_library_already_initialized(void) { return (cinst.ppb != NULL); }
bool profiling_enabled = false;	// nvmeibc_main: module param

/************************ Kernel compatibility layer **************************/
#include "common/compat/kr_incs_malloc.inc.c"
#include "common/compat/kr_incs_sgl.inc.c"
#include "common/compat/kr_incs_data_structs.inc.c"
#include "common/compat/kr_incs_nvmeib_common.inc.c"
#ifdef DISABLE_ALL_TRACING		// Framework for jiffies/cycles_khz is defined by tracing
	#include "common/compat/kr_incs_time_jiff.inc.c"
#endif

#include <stdarg.h>
int printk(const char *fmt,...) {
	int res = 0;
	va_list ap;
	va_start(ap, fmt);
	res = cinst.vtable.run_printk(fmt+2, ap);	/* Skip prefix */
	va_end(ap);
	return res;
}

/***************************** Pausable layer - glue to vfunc **************************/
static int __um_run_cmpxchg(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *dc) {
	(void)self;(void)handle;
	cinst.vtable.run_cmpxchg(disk, addr, dc);
	return 0;
}

static int __um_write_blkset_info(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *dc) {
	(void)self;(void)handle;
	cinst.vtable.run_write_binfo(disk, addr, dc);
	return 0;
}

static int __um_execute_gen(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_gen_cmd *cmd) {
	(void)self;
	cinst.vtable.run_gen(disk, cmd);
	return 0;
}

static int __um_free_jrnl_ents(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp) {
	comp->gen_cmd->opcode = NVMEIB_GEN_OP_FREE_JRNL_ENTS;
	memcpy(comp->gen_cmd->param.free_ents.serjio_boot_id, comp->serjio_boot_id, NVMEIB_GID_STR_MAX);
	memcpy(comp->gen_cmd->param.free_ents.seg_uuid, comp->seg_uuid, NVMEIB_GID_STR_MAX);
	comp->gen_cmd->param.free_ents.src = comp->recov_src;
	comp->gen_cmd->param.free_ents.blkset_num = comp->blkset_num;
	comp->gen_cmd->param.free_ents.blkset_slba = comp->blkset_slba;
	comp->gen_cmd->param.free_ents.lock_ent = comp->lock_ent;
	comp->gen_cmd->param.free_ents.num_ents = comp->num_ents;
	comp->gen_cmd->param.free_ents.pass2toma = comp->pass2toma;
	comp->gen_cmd->param.free_ents.ents = comp->ents;
	comp->gen_cmd->ctx = comp;
	return __um_execute_gen(self, disk, comp->gen_cmd);
}

static int __um_execute_io_blocks(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *dcmd) {
	(void)self;
	cinst.vtable.run_io_blocks(disk, dcmd);
	return 0;
}

//the following 3 function update the amount of requests that entered the gate
static void __um_cb_called_comp(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_d_rdma_comp *c) {
	(void)self;(void)disk; (void)c; 
}

static void __um_cb_called_free_jrnl_ents(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *c) { 
	(void)self;(void)disk; (void)c;
}

static void __um_cb_called_jmdc(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp *c) {
	(void)self;(void)disk; (void)c;
}

static int __um_run_read_lock(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *dc) {
	(void)self;(void)handle;
	cinst.vtable.run_viewlock(disk, addr, dc);
	return 0;
}

static int __um_execute_io_jour_blocks(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *cmd) {
	return __um_execute_io_blocks(self, disk, cmd);
}

static int __um_get_blkset_problems(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void *handle, u64 start, u64 length, struct nvmeibc_d_rdma_comp *dc) {
	(void)self;(void)handle;
	cinst.vtable.recov.get_problems(disk, start, length, dc);
	return 0;
}

static int __um_jmdc_read(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp      *dc) {
	(void)self;
	cinst.vtable.recov.read_jmdc(disk, dc);
	return 0;
}

static int __um_reused_bb_release(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk,struct nvmeib_data_reuse_buf_params *p) { 
	(void)self;(void)disk; (void)p; 
	BUG();
	return 0;
}

static int __um_dbg_please_kill_yourself(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, void (*cb)(void*), void *ctx, int rsc_id, u64 dlba)
{
	(void)self;(void)disk; (void)cb; (void)ctx; (void)rsc_id; (void)dlba;
	BUG();
	return -EOPNOTSUPP;
}

static void __um_cb_called_cmd(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, struct nvmeibc_disk_command *disk_cmd)
{
	(void)self;(void)disk; (void)disk_cmd;
	BUG();
}

static void __um_dump_transfers(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk)
{
	(void)self;(void)disk;
	BUG();
}

static int __um_tostring(struct nvmeibc_icore_ops const* self, const struct nvmeibc_disk *disk, char *buf, int buf_len)
{
	(void)self;(void)disk; (void)buf; (void)buf_len;
	BUG();
	return 0;
}

static int __um_jam_get(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk)
{
	(void)self;(void)disk;
	BUG();
	return 0;
}

static void __um_jam_put(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk)
{
	(void)self;(void)disk;
	BUG();
}

static int __um_jam_get_all(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_disk *disks[])
{
	(void)self;(void)n_disks; (void)disks;
	BUG();
	return 0;
}

static void __um_jam_put_all(struct nvmeibc_icore_ops const* self, int n_disks, struct nvmeibc_disk *disks[])
{
	(void)self;(void)n_disks; (void)disks;
	BUG();
}

static int __um_toma_send(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, u64 handle, struct nvmeibc_disk_toma_send_params *params)
{
	(void)self;(void)disk; (void)handle; (void)params;
	BUG();
	return -EOPNOTSUPP;
}

static int __um_toma_unsubscribe(struct nvmeibc_icore_ops const* self, struct nvmeibc_disk *disk, u64 handle)
{
	(void)self;(void)disk; (void)handle;
	BUG();
	return -EOPNOTSUPP;
}


struct nvmeibc_icore_ops const um_core_ops = {
	.run_cmpxchg = __um_run_cmpxchg,
	.run_read_lock = __um_run_read_lock,
	.jmdc_read = __um_jmdc_read,
	.free_jrnl_ents = __um_free_jrnl_ents,
	.dbg_please_kill_yourself = __um_dbg_please_kill_yourself,
	.write_blkset_info = __um_write_blkset_info,
	.get_blkset_problems = __um_get_blkset_problems,
	.execute_io_blocks = __um_execute_io_blocks,
	.execute_io_jour_blocks = __um_execute_io_jour_blocks,
	.execute_gen = __um_execute_gen,
	.cb_called_comp = __um_cb_called_comp,
	.cb_called_cmd = __um_cb_called_cmd,
	.cb_called_jmdc = __um_cb_called_jmdc,
	.cb_called_free_jrnl_ents = __um_cb_called_free_jrnl_ents,
	.dump_transfers = __um_dump_transfers,
	.tostring = __um_tostring,
	.jam_get = __um_jam_get,
	.jam_put = __um_jam_put,
	.jam_get_all = __um_jam_get_all,
	.jam_put_all = __um_jam_put_all,
	.toma_send = __um_toma_send,
	.toma_unsubscribe = __um_toma_unsubscribe,
	.reused_bb_release = __um_reused_bb_release,
};

struct nvmeibc_icore_ops const* nvmeibc_core_ops_get(void){
	return &um_core_ops;
}


/***************************** JAM - glue to vfunc **************************/
#include "nvmeibc_jam.h"
struct nvmeib_cpu_mask_info;
int  nvmeibc_jam_lbas_alloc(int n_disks, struct nvmeibc_disk *disks[], u32 txid, u64 dlbas[], u64 res_jlbas[], bool w, const struct nvmeib_cpu_mask_info *cpu_mask_info, unsigned long deadline_jiffies, unsigned long priority, void *ctx) { (void)n_disks; (void)disks; (void)txid; (void)dlbas; (void)res_jlbas; (void)w; (void)cpu_mask_info; (void)deadline_jiffies; (void)priority; (void)ctx; BUG(); return 0; }
void nvmeibc_jam_lbas_free(int n_disks, struct nvmeibc_disk *disks[], u64 jlbas[], u32 wr_sts_bm) { (void)n_disks; (void)disks; (void)jlbas; (void)wr_sts_bm; BUG(); return; }
int  nvmeibc_jam_abandon_lba(struct nvmeibc_disk *disk, u64 jlba, u8 *gen_id) { (void)disk; (void)jlba; (void)gen_id; BUG(); return 0; }
// Loser: Todo, make vritual function
void nvmeibc_b_cp_loser_aband_jour(struct nvmeibc_b_cp_loser *l, const struct nvmeibc_subscription_ctx * tr, int jent, u8 jid) { (void)l; (void)tr; (void)jent; (void)jid; BUG(); }

/************************* glue to nvmeib, cdisk, resolver *******************/
void nvmeib_set_block_dp_ec_funcs(void (*read_mod_wr_dmd)(void*, u64)) { (void)read_mod_wr_dmd; }

bool nvmeibc_disk_do_512b_sub_block_x_supported(const struct nvmeibc_disk* disk) {
	return nvmeibc_disk_get_sector_shift(disk) == 9 && disk->md_size == 0;
}

#include "clnt/block/recovery/nvmeibc_decentralized_unreg.h"
enum stale_lock_resolve_status stale_lock_resolver_get_status(struct stale_lock_resolver_t *slr, u32 lock_id, const struct nvmeibc_cmd_lock *cmd_lock) {
	const int rv = cinst.vtable.get_slr_status(slr, lock_id);
	(void)cmd_lock;
	return (rv == 0) ? stale_lock_resolve_safe_to_use : stale_lock_resolve_broken;
}

int stale_lock_resolver_fill_cuuid_by_lockid(struct stale_lock_resolver_t *slr, u32 lock_id, uuid_be *cuuid) {
	const int rv = cinst.vtable.get_slr_cuuid_by_lockid(slr, lock_id, cuuid);
	return (rv == 0) ? 0 : -ENOENT;
}

uuid_be *nvmeibc_get_uuid(const struct nvmeibc_cinst_params_core *p)
{
	return cinst.vtable.get_cuuid();
}

/*********************************** Implementation ************************************/
struct dplib_caller {		// Ideas take from recovery worker, that also behaves as sync caller
	struct operation o;
	struct nvmeibc_block_device nd;
	struct nvmeibc_topology t;
	struct nvmeibc_chunk chunks[2];		// 1 real, 1 dummy
	struct nvmeibc_raid1 pr;
	struct nvmeibc_raid_topo_persistent hdr;
	struct nvmeibc_disk_segment segs[N_MAX_RAID_SLICE_LEN];
	struct nvmeibc_subscription_ctx trs[N_MAX_RAID_SLICE_LEN];
	union {											// Input params, C++ polymorphism
		struct lib_call_api_params_generic *gp;		// Generic params (base class)
		struct lib_call_api_sync*           sync;
		struct lib_call_api_io*             iorw;
		struct lib_call_api_recov*          recv;
	};
};

// For back reference to 'dplib_caller' struct, store it in topology and block device
#define HACK_SW_FOR_BACKREF_PUT(sw) ((void*)sw)
#define HACK_SW_FOR_BACKREF_GET(p)  ((struct dplib_caller *)p)

static inline bool __does_caller_holds_primary_lock(const struct dplib_caller* sw) { return sw->gp->pr.locks[0].is_already_taken; }
static inline bool __is_sync( struct dplib_caller* sw) { return (sw->gp->op > NVMEIB_BLOCK_IO_OP_DISCARD); } // !nvmeib_block_io_op_is_bio_op(op)
static inline bool __is_recov(struct dplib_caller* sw) { return sw->gp->op == NVMEIB_BLOCK_IO_OP_NOP; }
static inline bool __is_io(   struct dplib_caller* sw) { return !__is_sync(sw) && !__is_recov(sw); }
static inline struct output_generic_t* _get_out_struct(struct dplib_caller* sw) { return &sw->sync->out;   }
static inline struct lib_op_stats_t  * _get_stt_struct(struct dplib_caller* sw) { return &sw->sync->stats; }
static inline void _set_out_binfo(struct output_generic_t* out, u32 val) {
	out->binfo.all = val;
	out->is_binfo_field_valid = true;
}

/***************************** Replacing Operation.c **************************/
void nvmeibc_operation_alloc_dbg_id(struct operation *o) {
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(o->nd->volume);
	o->dbg_id = sw->gp->dbg_id;
}
bool nvmeibc_operation_throttling_check_should_execute(struct operation *o) { (void)o; return true; }	// No throttling, execute immediately
u64 dp_cmds_req_alloc_unique_id(void) { return 0x114115; }		// Todo: Make predictable and unique

int nvmeibc_io_resubmitter_retry_op(struct operation *o) {
	nvmeibc_operation_destroy(o, -ENOEXEC);			// No retries, returns asap to caller
	return 0;
}
int nvmeibc_io_resubmitter_submit_op(struct operation *o) { (void)o; BUG(); return -1; }	// Should never be used
struct nvmeibc_blk_op_globals *nvmeibc_operation_all_create(void);
struct nvmeibc_blk_op_globals *nvmeibc_operation_all_create(void) { return NULL;}
void nvmeibc_operation_all_destroy(void);
void nvmeibc_operation_all_destroy(void) {}

/***************************** Locks transfer **************************/
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.h"
bool __IO_LT_is_allowed_to_transfer_locks(const enum nvmeib_block_io_op op) {
	(void)op; return true;						/* Caller of DP lib may already hold the locks for any type of IO */
}

int  __IO_LT_try_request_transfer(struct nvmeibc_cmd_lock *locks) {	// Caller gives its locks to IO
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(locks->cmds->o->nd->volume);
	int i, n_locks = locks->nlocks;
	const bool are_locks_taken_by_caller = __does_caller_holds_primary_lock(sw);
	if (are_locks_taken_by_caller) {
		const struct locks_params_t *pls = sw->gp->pr.locks;
		for (i = 0; i < n_locks; i++) {				// Transfer locks from caller to IO
			struct nvmeibc_cmd_lock *l = &locks[i];
			struct nvmeibc_d_rdma_comp *dc = &l->comp;
			BUG_ON(pls[i].is_already_taken != are_locks_taken_by_caller);	// Sanity: Verify input params: all locks are taken by the caller or none
			nvmeibc_cmd_lock_set_bi(l, pls[i].binfo);
			__change_lock_status_to(l, NCL_STATUS_TRANSFERRED);
			dc->lock.id = dc->compare;
		}
		for (i = 0; i < n_locks; i++) {
			struct nvmeibc_d_rdma_comp *dc = &locks[i].comp;
			dc->callback(dc, nvmeibc_d_rdma_comp_tag_make()); /* Simulate success callback */
		}
		return n_locks;
	}
	return 0;
}

void __IO_LT_try_transfer_give(struct nvmeibc_cmd_lock *locks, int lsi) {	// IO returns locks back to caller
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(locks->cmds->o->nd->volume);
	int i, n_locks = locks->nlocks;
	if (lsi == 0) {
		struct output_generic_t *out = _get_out_struct(sw);
		_set_out_binfo(out, nvmeibc_cmd_lock_get_bi(locks).all);
	}
	if (__does_caller_holds_primary_lock(sw)) {
		for (i = 0; i < n_locks; i++)				// Transfer locks back to caller
			__change_lock_status_to(&locks[i], NCL_STATUS_TRANSFERRED);
	}
}
void __IO_LT_refuse_transfer(     struct nvmeibc_cmd_lock *l) { (void)l; }
void __IO_LT_complete_locks(      struct nvmeibc_cmd_lock *l) { (void)l; }	// IO: finished with its locks, do nothing

// trs
void nvmeibc_trs_hash_verify_empty_unsafe(void) {}

/***************************** Replacing Topology.c **************************/
#include "block/nvmeibc_topology.h"
static struct nvmeibc_topology* __veirfy_this_is_dummy_topo(struct nvmeibc_topology *t) {
	struct nvmeibc_block_device *dev = nvmeibc_block_t_to_b(t);
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(dev->volume);
	BUG_ON(t != &sw->t);
	return t;
}

struct nvmeibc_topology *nvmeibc_topology_get(struct nvmeibc_topologies *nt) {
	struct nvmeibc_topology *t = list_first_entry(&nt->topologies, struct nvmeibc_topology, list_n);
	return __veirfy_this_is_dummy_topo(t);
}

struct nvmeibc_topology *nvmeibc_topology_addref(struct nvmeibc_topology *t) {
	return __veirfy_this_is_dummy_topo(t);
}

void nvmeibc_topology_put(struct nvmeibc_topology *t) {
	if (t) __veirfy_this_is_dummy_topo(t);
}

static void __autofail_rest_of_execution(struct dplib_caller *sw) {
	sw->t.phased_out = true;				// Will autofail as fast as possible entire execution
}

int nvmeibc_topologies_rereg_seg(struct nvmeibc_disk_segment *seg) {
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(seg->lock_operation_profiler);
	struct output_generic_t *out = _get_out_struct(sw);
	out->err_info.must_topo_rereg = true;
	__autofail_rest_of_execution(sw);
	return 0;
}

int nvmeibc_get_chunk_ind_of_lba(u64 lba, const struct nvmeibc_topology *t) {
	(void)t; (void)lba; return 0;	// Dummy topo with 1 chunk only
}

/***************************** Intercept: Block.c API **************************/
void nvmeibc_mark_sync_wants_to_inject_caller_sgl(const struct nvmeibc_block_command *cmd) {
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(cmd->ds->lock_operation_profiler);
	struct output_generic_t *out = _get_out_struct(sw);
	BUG_ON(!__is_sync(sw));			// If IO called a sync, then sync should inject data directly, and this function should not be called
	out->please_reread_data = true;
}

int __send_toma_lock_help(const struct nvmeibc_cmd_lock *u1, struct nvmeibc_disk_segment *u2) { (void)u1; (void)u2; BUG(); return 0; }

int __send_toma_cmd_help(const struct nvmeibc_d_iocmd_comp *dc, const u32 failed, const u32 fixed) {
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(dc);
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(cmd->ds->lock_operation_profiler);
	struct output_generic_t *out = _get_out_struct(sw);
	if (dc->comp_code == EPERM_READ_FAIL || dc->comp_code == EPERM_READ_FAIL_NO_RETRY) {
		out->err_info.slice_masks.destroyed = failed;
		out->err_info.slice_masks.fixed = fixed;
	}
	return 0;
}

int nvmeibc_block_suspend(struct nvmeibc_block_device *dev, void *ctx, blk2blk_gen_work_t cb) {
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(dev->volume);
	struct output_generic_t *out = _get_out_struct(sw);
	(void)ctx; (void)cb;
	out->err_info.should_suspend_bdev = true;
	__autofail_rest_of_execution(sw);
	return 0;
}

int nvmeibc_block_revive(struct nvmeibc_block_device *dev) { (void)dev; BUG(); return 0; }	// Sync/IO never calls it !
void __copy_sub_block_by_map(unsigned char *dst, unsigned char *src, const ulong map) { (void)src; (void)dst; (void)map; BUG(); }

void nvmeibc_block_completion(struct nvmeibc_d_iocmd_comp *comp) {	// Remove from here!!!!!!!!!!!!!!!!!!!
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(comp);
	if (unlikely(comp->comp_code)) {
		if (comp->comp_code > 0) {							// NVME error code
			comp->comp_code = nvmeib_error_code_refine(comp->comp_code);
		}
	}
	cmd->o->nd->dp.cmd_comp_cb(comp, nvmeibc_d_iocmd_comp_tag_make());
}

void nvmeibc_block_comp_gencmd(struct nvmeibc_d_iocmd_comp *comp) {
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(comp);
	cmd->o->nd->dp.cmd_comp_cb(comp, nvmeibc_d_iocmd_comp_tag_make());
}

/***************************** Intercept: OS API **************************/
bool nvmeibc_block_is_kernel_sector_io_allowed(const struct nvmeibc_block_device *dev) { (void)dev; return false; }	// Sub block is not supported
u64 block_api_os_get_max_supported_trim_blks(struct nvmeibc_os_api *os);
u64 block_api_os_get_max_supported_trim_blks(struct nvmeibc_os_api *os) { (void)os; return ~0UL; }	// Infinite, Caller already split trims if needed
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_part.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_part.inc.c"
enum nvmeib_block_io_op __get_bio_op(const struct nvmeibc_os_api*os, const struct bio *bio) {
	(void)os;
	if (bio->bi_rw == READ)  return NVMEIB_BLOCK_IO_OP_READ;
	if (bio->bi_rw == WRITE) return NVMEIB_BLOCK_IO_OP_WRITE;
	return NVMEIB_BLOCK_IO_OP_DISCARD;
}
const struct nvmeibc_os_api* block_api_os_get_os(const struct bio *bio);
const struct nvmeibc_os_api* block_api_os_get_os(const struct bio *bio) { return (void*)bio->bi_private; }
const struct nvmeibc_block_device* get_bdev_of_bio(const struct bio *bio);
const struct nvmeibc_block_device* get_bdev_of_bio(const struct bio *bio){ return (const struct nvmeibc_block_device*)block_api_os_get_os(bio); }
struct nvmeibc_block_device* block_api_os_get_base_bdev(const struct nvmeibc_os_api* os, ulong *s, ulong *l);
struct nvmeibc_block_device* block_api_os_get_base_bdev(const struct nvmeibc_os_api* os, ulong *s, ulong *l) { (void)s; (void)l; return (void*)os; }
struct nvmeibc_os_apis_container * nvmeibc_os_api_layer_init(const struct nvmeibc_cinst_params_blk *p);
struct nvmeibc_os_apis_container * nvmeibc_os_api_layer_init(const struct nvmeibc_cinst_params_blk *p) { (void)p; return (struct nvmeibc_os_apis_container *)0xFFFF; }
struct nvmeibc_b_cp_cpu_masks *nvmeibc_b_cp_cpu_masks_create(void);
struct nvmeibc_b_cp_cpu_masks *nvmeibc_b_cp_cpu_masks_create(void) { return (struct nvmeibc_b_cp_cpu_masks *)0xFFFF; }
void nvmeibc_b_cp_cpu_masks_destroy(struct nvmeibc_b_cp_cpu_masks *cpu_masks);
void nvmeibc_b_cp_cpu_masks_destroy(struct nvmeibc_b_cp_cpu_masks *cpu_masks) { (void)cpu_masks; }

void nvmeibc_os_api_layer_destroy(const struct nvmeibc_cinst_params_blk *p);
void nvmeibc_os_api_layer_destroy(const struct nvmeibc_cinst_params_blk *p) { (void)p; }

int block_api_os_verify_bio_geometry(const struct bio *bio);
int block_api_os_verify_bio_geometry(const struct bio *bio)
{
	ulong sub_len = ~0UL;					// len - Irrelevant for testing of size (volume can be auto extandable)
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	const ulong lba_bio_start_s = (ulong)__GET_BI_SECTOR(bio);
	const long total_size_b = __GET_BI_SIZE(bio);
	const enum nvmeib_block_io_op op = __get_bio_op(os, bio);	//Even if sub-read is done, there will be no change to the operation	

	if ((int)op < 0){// enum is uint so convert to signed error code
		return -EINVAL;
	}

	/* Subset of out of bound tests taken from operation code. Needed to avoid wrong split */
	if (unlikely((lba_bio_start_s > (1ULL << 63)) || (total_size_b == 0) ||
				 (lba_bio_start_s + (total_size_b>>KERNEL_SECTOR_SHIFT) > sub_len))){
		return -EINVAL;
	}

	return 0;
}

/***************************** Init library **************************/
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf.h"
struct nvmeibc_recovery_hooks dp_lib_recov_hook = {NULL};		// Put hooks on recovery to trigger on sync finish callback
void __recov_yield_after_1_sync(struct nvmeibc_recovery_hooks* unused, struct nvmeibc_recovery* recov);

void nvmesh_dp_lib_create(struct nvmesh_dp_lib_virtual_table vt) {
	extern bool nvmeibc_should_sync_reuse_memory;	// Module param
	extern bool nvmeibc_copy_bio_buffers;	// Module param
	WARN(_is_library_already_initialized(), "library was already initialized");
	int_cpu_freq_tsc_offset_jiffies();
	cinst.vtable = vt;
	cinst.pb._private = &cinst.ppb;
	t_blok_clnt_globals_init(&cinst.pb);
	nvmeibc_should_sync_reuse_memory = false;	// Not supported yet
	nvmeibc_copy_bio_buffers = false;			// Caller takes care of In flight write buffers change???
	BUILD_BUG_ON(offsetof(struct lib_call_api_sync, gen_params) != 0ULL); // Must be first field, due to union between general_params and those structs
	BUILD_BUG_ON(offsetof(struct lib_call_api_io, gen_params) != 0ULL); // Must be first field
	BUILD_BUG_ON(offsetof(struct lib_call_api_recov, gen_params) != 0ULL); // Must be first field
	BUILD_BUG_ON(offsetof(struct lib_call_api_sync,exec) != offsetof(struct lib_call_api_recov, exec)); // Same offset
	BUILD_BUG_ON(offsetof(struct lib_call_api_io  ,exec) != offsetof(struct lib_call_api_recov, exec)); // Same offset
	BUILD_BUG_ON(offsetof(struct lib_call_api_sync,out ) != offsetof(struct lib_call_api_recov, out )); // Same offset
	BUILD_BUG_ON(offsetof(struct lib_call_api_io  ,out ) != offsetof(struct lib_call_api_recov, out )); // Same offset
	if (vt.recov.yield_after_1_sync) { /* Recovery hook */
		extern struct nvmeibc_recovery_hooks* rcvr_hooks;
		dp_lib_recov_hook.on_finish_one_sync = __recov_yield_after_1_sync;
		rcvr_hooks = &dp_lib_recov_hook;
	}
}

void nvmesh_dp_lib_destroy(void) {
	WARN(!_is_library_already_initialized(), "library was not initialized properly");
	t_blok_clnt_globals_destroy(&cinst.pb);
}

/***************************** Replacing Topology.c **************************/
static void __rec_dummy_drainer(void* _dev) { (void)_dev; BUG_ON(1); }

static void __create_dummy_block_device(const struct dplib_caller *sw, struct nvmeibc_block_device* dev)
{	// Simulation of nvmeibc_block_init()
	dev->size = 0;
	strlcpy(dev->name, "slib_bdev_name", sizeof(dev->name));
	strlcpy(dev->uuid, "slib_bdev_uuid", sizeof(dev->uuid));
	nvmeibc_volume_short_id(dev) = sw->gp->dbg_id;
	dev->status = NCBD_ATTACHED;
	dev->max_retry_jiffies = 300 * HZ;
	dev->type = NORMAL_VOLUME;
	dev->volume = HACK_SW_FOR_BACKREF_PUT(sw); // Not mandatory! better use container_of()
	{ // nvmeibc_topologies_init()
		struct nvmeibc_topologies *nt = &dev->topologies;
		INIT_LIST_HEAD(&nt->topologies);
		nt->device_name = dev->name;
		nt->io_perm = NVMEIB_IO_TYPE_PERMIT_ALL;
	}
	{	// __bdev_datapath_init()
		const struct praid_params_t* pr = &sw->gp->pr;
		const struct bdev_params_t*  pdev = &sw->gp->bdev;
		const enum nvmeibc_data_path_type dp_type = (pr->slice_size > 1 ) ? NVMEIBC_DATA_PATH_EC_R6 : NVMEIBC_DATA_PATH_MIR_BIO; // __select_datapath_type()
		const bool optimize_local_reads = false;
		const struct nvmeibc_dp_params dp_par = { .snake_size = 1, .binje = pdev->binje, .slice_size = pr->slice_size, .protect_level = (pr->n_segments - pr->slice_size) };
		nvmeibc_datapath_init(&dev->dp, dev->name, dp_type, pdev->enable_crc_check, pdev->use_debug_di, optimize_local_reads,
				      0 /*read_has_mutable_bio_buffers */, pr->slice_size, dp_par);
		nvmeibc_recovs_drainer_init(&dev->dp.running_recovs, __rec_dummy_drainer, dev);
	}
}

static void __create_dummy_segment(struct dplib_caller *sw, int i) {
	struct nvmeibc_disk_segment *seg = &sw->segs[i];
	struct nvmeibc_subscription_ctx *tr = &sw->trs[i];
	const struct seg_params_t *p = &sw->gp->pr.segs[i];
	seg->disk = p->disk;

	// __digest_segment_layout()
	strlcpy(seg->uuid, p->uuid, sizeof(seg->uuid));
	seg->dbg_uuid = i;

	// __subscribe_seg();
	seg->toma_reg = tr;
	tr->nt = &sw->nd.topologies;
	tr->ch =  0;			// Topology has 1 chunk, 1 praid
	tr->r1 =  0;
	tr->seg = i;
	tr->hdr = &sw->hdr;
	seg->first_lba = p->dlba_start;
	tr->length = seg->length = p->dlba_length;

	// __toma_segment_register_succeed()
	seg->disk = p->disk;
	seg->registration_status = SEG_REGSTATUS_TOMA_OK;
	seg->max_dma_size = (seg->disk->max_request_size_bytes >> NVMEIBC_SECTOR_SHIFT);
	seg->sw_md_size = __nvmeibc_disk_sw_md_size(seg->disk);
	seg->lmap = p->lmap;
	seg->sync_safety = p->sync_safety;
	seg->toma_acm = p->toma_acm;
	seg->lock_operation_profiler = HACK_SW_FOR_BACKREF_PUT(sw);
	seg->chunk = &sw->chunks[0];
};

struct stale_lock_resolver_t *nvmeibc_raid_topo_persistent_get_slr(struct nvmeibc_raid_topo_persistent *p) {
	return (struct stale_lock_resolver_t *)p->slr.self;
}

static void __create_dummy_protection_raid(struct dplib_caller *sw) {
	struct nvmeibc_raid1* pr = &sw->pr;
	struct praid_params_t* ppr = &sw->gp->pr;
	int si;
	{ // nvmeibc_raid_topo_persistent_create()
		struct nvmeibc_raid_topo_persistent *hdr = pr->hdr = &sw->hdr;
		if (ppr->slr) {
			hdr->slr = *ppr->slr;
			hdr->slr.self = (void *)ppr->slr;
		}
		hdr->sync_profile = NULL;
		if (__is_recov(sw)) {
			BUG_ON(nvmeibc_recoveries_create(hdr) != 0);
		}
	}

	// __toma_update_raid1()
	pr->version = ppr->praid_version;
	pr->replicas = ppr->n_segments;
	pr->slice_size = ppr->slice_size;
	pr->lid.all = ppr->lid.all;
	pr->toma.io_perm = NVMEIB_IO_TYPE_PERMIT_ALL;
	for (si = 0; si < pr->replicas; si++) {
		__create_dummy_segment(sw, si);
	}

	pr->segments = sw->segs;
	pr->lock_scheme.type = OWNER_SCHEME_SL_START_DEC_C;
	pr->lock_scheme.lockset_shift = LOCKSET_SHIFT;
	pr->lock_scheme.max_n_owners = 1 + nvmeibc_raid1_get_protect_lvl(pr);
	pr->use_rdma_locks = true;
};

static void __create_dummy_chunk(struct dplib_caller *sw) {
	struct nvmeibc_chunk *c = &sw->chunks[0];				// Default chunk[0]
	__create_dummy_protection_raid(sw);

	c->raid1s = &sw->pr;
	c->topology = &sw->t;
	c->first_vlba = 0;
	c->stripe_size = LOCKSET_SLICES * sw->pr.slice_size;	// Not important

	c++;	// Dummy chunk[1]
	c->first_vlba = nvmeibc_raid_get_rlba_len(&sw->pr);
	c->topology = &sw->t;
}

static void __create_dummy_topology(struct dplib_caller *sw) {
	// Much like nvmeibc_topology_update_configuration()
	struct nvmeibc_topology *t = &sw->t;
	__create_dummy_chunk(sw);
	t->nt = &sw->nd.topologies;
	t->configuration_version = t->topology_version = 1;
	t->debug_unique_index = sw->gp->pr.topology_id;
	INIT_LIST_HEAD(&t->list_n);
	spin_lock_init(&t->delete_lock);
	t->chunks = sw->chunks;
	t->nchunks = ARRAY_SIZE(sw->chunks);
	t->io_perm = sw->pr.toma.io_perm;
	t->highest_pow2_in_chunk_ind = 0;			// __topo_calc_chunk_binary_search_start()
	t->stripe_width = 1;

	// Much like __set_active_topology()
	nvmeibc_topology_fill_all_topo_raids_calculated_data(t);
	list_add(&t->list_n, &t->nt->topologies);
}

static int __create_dummy_operation_locks_cmds(struct dplib_caller *sw) {
	struct operation *o = &sw->o;
	int i, n_locks = N_MAX_RAID_LOCKS, n_cmds = 1;	// Todo: Use topology value instead of N_MAX_RAID_LOCKS
	if ((nvmeibc_clmat_allocate(o, false, n_locks, n_cmds, 0) < 0) || (!(o->cmds = dp_cmds_kvzalloc(n_cmds)))) {
		_NT(t_01_slbac, "alloc failed.");
		return -ENOMEM;
	}

	// Below code is almost identical to nvmeibc_recov_sync_worker_init()
	o->nd = &sw->nd;
	o->op = NVMEIB_BLOCK_IO_OP_DPLIB_PROBLEM;
	o->locks->cmds = o->cmds;
	o->cmds->ncmds = n_cmds;				// Ficticious commands
	o->cmds->o = o;
	o->cmds->cmdarr = o->cmds;
	o->cmds->locksets = o->locks;
	o->dbg_id = 2020202;
	o->slo = sw;
	o->topo = NULL;
	o->cpu_id = NR_CPUS;
	for (i = 0; i < n_locks; ++i) {
		nvmeibc_b_rdma_comp_init(&o->locks[i].comp, i, o->locks);
		nvmeibc_clmat_set_link(o->CLmat, 0, i, o->locks);
	}

	// Roughly logic of __recover_next_blockset()
	/* Add locks to protect given raid */
	o->locks->nlocks = dp_fill_locks_for_raid(&sw->pr, o->op, sw->sync->sync_params.rlba, o->locks);
	o->cmds->ds = o->locks[0].ds;
	{	// Simulate as if tried to acquire lock but maybe failed, or already took all locks
		const struct praid_params_t* ppr = &sw->gp->pr;
		const struct locks_params_t *pls = ppr->locks;
		const u64 holder = ppr->offending_stale_lock.all;
		struct nvmeibc_cmd_lock *l = o->locks;
		bool all_locks_taken = true;
		for (i = 0; i < o->locks->nlocks; i++, l++) {				// Copy the locks. Sync has to fill only the locks IO could not take
			struct nvmeibc_d_rdma_comp *dc = &l->comp;
			dc->code = NVMEIBC_CMD_LOCK_OWNER;
			dc->compare = 0;
			dc->exchange = sw->pr.lid.all;
			if (pls[i].is_already_taken) {
				nvmeibc_cmd_lock_set_bi(l, pls[i].binfo);
				__change_lock_status_to(l, NCL_STATUS_TAKEN);
				dc->lock.id = dc->compare;
			} else {
				__change_lock_status_to(l, NCL_STATUS_NOTISSUED);	// Sync has to take this lock
				dc->lock.id = holder;
				all_locks_taken = false;
			}
		}

		// Fill the dummy rld.pre binfo like and when a kernel caller does - some syncs need it
		if (all_locks_taken) {
			const union nvmeib_blkset_info binfo = dp_locks_get_TxID_dbits(o->locks, 0  /* owner_i */, false);
			o->cmds[0].rld.pre.all = binfo.all;
		}
	}
	o->topo = &sw->t;		// Equivalent to __recovery_get_topo()
	return 0;
}

/**************************** Sync call implementation ***********************/
#define mark_negative_rv(s) ({ memset(&(s)->out, 0, sizeof((s)->out)); (s)->out.error = -ENOEXEC; ((void*)s); })
#define mark_not_started(s) ({ (s)->out.err_info.not_even_started = true;  })

// Update stats upon IO / Sync finish. Note: When IO finishes, update IO and syncs stats, it may have called
static void __update_stats_on_finish(struct dplib_caller *sw) {
	struct nvmeibc_datapath_syncs_resources *dsr = &sw->nd.dp.sync_rsrcs;
	const struct nvmeibcb_dp_io_fail_mgr *ifm = &sw->nd.dp.io_stats.mgr;
	struct output_generic_t *out = _get_out_struct(sw);
	struct lib_op_stats_t *stats = _get_stt_struct(sw);
	if (stats->sync)
		*stats->sync = dsr->stats;
	out->err_info.binfo_corrupted = (ifm->n_binfo_errors > 0);
	out->err_info.htr_null_uuid =   (ifm->n_htr_null_uuids > 0);

	if (out->err_info.not_even_started) {
	} else {	// Fill stats of autonomous syncs
		#if defined(AUTONOMOUS_SYNCS_STATS)
			const u64 was_autonomous_sync = atomic64_read(&dsr->stats.num_autonomous_ok) + atomic64_read(&dsr->stats.num_autonomous_err);
			if (was_autonomous_sync)
				out->sync_was_autonomous = true;
		#endif
	}
}

static void dplib_caller_free(struct dplib_caller *sw) {
	sw->sync->exec.cancellation_context = NULL;		// It does not matter which type of operation is that, all have the same 'exec' struct in the same offset
	dp_cmds_free_all(sw->o.cmds);
	nvmeibc_operation_move_mem_to_locks(&sw->o);
	nvmeibc_clmat_free_dangling_locks(sw->o.locks);
	__update_stats_on_finish(sw);
	if (__is_recov(sw)) {
		nvmeibc_recoveries_destroy(&sw->hdr);
	}
	kfree(sw);
}

static struct dplib_caller *dplib_caller_create(void *s) {
	struct dplib_caller *sw = (struct dplib_caller *)my_kzalloc(sizeof(*sw), GFP_KERNEL);
	struct lib_op_stats_t *stats;
	if (unlikely(!sw))
		return sw;
	WARN(!_is_library_already_initialized(), "library was not initialized properly");
	sw->gp = s;
	__create_dummy_block_device(sw, &sw->nd);
	__create_dummy_topology(sw);
	sw->sync->exec.cancellation_context = sw;
	if (__is_sync(sw)) {
		if (unlikely(__create_dummy_operation_locks_cmds(sw) < 0)) {
			dplib_caller_free(sw);
			sw = NULL;
		}
	} else if (__is_recov(sw)) {
	} else {
		struct bio *bio = sw->iorw->io_params.bio;
		bio->bi_private = &sw->nd;
		sw->nd.dp.enable_local_read_optimization = sw->iorw->io_params.enable_local_read_optimization;
	}
	stats = _get_stt_struct(sw);
	if (stats->fctr)
		sw->nd.dp.sync_rsrcs.fcntr = stats->fctr;	// Running syncs will fill this struct directly
	return sw;
}

static int sync_done_return_to_caler(void* ctx, int err) {
	struct dplib_caller *sw = (struct dplib_caller *)ctx;
	struct output_generic_t *out = _get_out_struct(sw);
	BUG_ON(!__is_sync(sw));							// Sanity
	out->error = err;
	_set_out_binfo(out, sw->o.cmds[0].rld.pre.all);
	return 0;
}

static struct nvmeibc_cmd_lock * _first_missing_lock_or_primary_owner(struct dplib_caller *sw) {
	struct nvmeibc_cmd_lock *locks = sw->o.locks;
	int i; // Find First missing (not taken) lock, or if all taken, use primary owner
	for (i = 0; i < locks->nlocks; ++i) {
		if (!sw->gp->pr.locks[i].is_already_taken)
			return &locks[i];
	}
	return &locks[0];
}

void nvmesh_dp_lib_do_sync_op(struct lib_call_api_sync *s) {
	struct dplib_caller *sw = dplib_caller_create(mark_negative_rv(s));
	const struct lib_call_api_params_sync* sp = &s->sync_params;
	struct nvmeibc_cmd_lock *work_lock = _first_missing_lock_or_primary_owner(sw);
	int rv;
	BUG_ON(!__is_sync(sw));		// Sanity
	if (!sw) {
		mark_not_started(s);
		return;
	}

	rv = nvmeibc_sync_generic_by_op(work_lock, sp->start_slice, sp->n_slices, sw->gp->op, sync_done_return_to_caler, sw);
	if (rv < 0) {
		mark_not_started(s);
		sync_done_return_to_caler(sw, rv);
	} else { /* Do nothing, sync already completed! */ }
	dplib_caller_free(sw);
}

static void __on_op_canceled_by_caller(struct dplib_caller *sw) {
	struct output_generic_t *out = _get_out_struct(sw);
	out->err_info.was_canceled_by_caller = true;
	__autofail_rest_of_execution(sw);
}

void nvmesh_dp_lib_do_sync_abort(struct lib_call_api_sync*s) {
	__on_op_canceled_by_caller((struct dplib_caller *)s->exec.cancellation_context);
}

/**************************** IO call implementation ***********************/
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_func.h"

void block_api_os_end_io(struct bio_part *cur, ulong start_time, int rv) {
	const struct nvmeibc_block_device* nd = get_bdev_of_bio(cur->bio);
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(nd->volume);
	struct lib_call_api_io *io = sw->iorw;
	BUG_ON(!__is_io(sw) || (io->io_params.bio != cur->bio));			// Sanity
	io->out.error = rv; (void)start_time;
}
void nvmesh_dp_lib_do_rwt_op(struct lib_call_api_io* io) {
	struct dplib_caller *sw = dplib_caller_create(mark_negative_rv(io));
	int rv = 0;
	BUG_ON(!__is_io(sw));		// Sanity
	BUG_ON(__get_bio_op(NULL, io->io_params.bio) != io->gen_params.op);		// Sanity
	if (!sw) {
		mark_not_started(io);
		return;
	}
	if (io->gen_params.op == NVMEIB_BLOCK_IO_OP_DISCARD)
		rv = -EPERM;
	if (rv == 0)
		rv = execute_bio(io->io_params.bio, jiffies);
	if (rv < 0) {
		struct bio_part bp = {.bio = io->io_params.bio};
		mark_not_started(io);
		block_api_os_end_io(&bp, 0, rv);
	}
	dplib_caller_free(sw);
}

void nvmesh_dp_lib_do_trim_op( struct lib_call_api_io* io) {
	struct dplib_caller *sw = dplib_caller_create(mark_negative_rv(io));
	struct bio_part bp = {.bio = io->io_params.bio};
	mark_not_started(io);	// Not supported yet
	block_api_os_end_io(&bp, 0, -EPERM);
	dplib_caller_free(sw);
}

void nvmesh_dp_lib_do_rwt_abort(struct lib_call_api_io* io) {
	__on_op_canceled_by_caller((struct dplib_caller *)io->exec.cancellation_context);	// Can also get sw via: HACK_SW_FOR_BACKREF_GET(get_bdev_of_bio(io->io_params.bio)->volume)
}

/**************************** Recovery call implementation ***********************/
void __recov_yield_after_1_sync(struct nvmeibc_recovery_hooks* unused, struct nvmeibc_recovery* recov) {
	const struct nvmeibc_block_device *nd = nvmeibc_block_nt_to_b(recov->args.tr->nt);
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(nd->volume);
	cinst.vtable.recov.yield_after_1_sync(sw->recv);
	(void)unused;
}

static void __recov_done_return_to_caler(struct dplib_caller *sw, int err) {
	struct output_generic_t *out = _get_out_struct(sw);
	BUG_ON(!__is_recov(sw));							// Sanity
	out->error = err;
}

int nvmeibc_toma_send_direct_msg(struct nvmeibc_disk_segment *seg, enum NVMEIBT_CLIENT_MSG_TYPES msg_type, struct nvmeibt_client_msg_pl *pl) {
	struct dplib_caller *sw = HACK_SW_FOR_BACKREF_GET(seg->lock_operation_profiler);
	if (msg_type == NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH) {
		struct nvmeibt_client_recovery_status_pl *rpl = &pl->recov_status;
		__recov_done_return_to_caler(sw, rpl->ret_code);
		_NT(t_02_slbac, "recov finish, task_id=@TASK_ID, err=@ERR, num_left=@NUM_LEFT", sw->recv->recov_params.id, rpl->ret_code, rpl->num_locks_left);
	} else {
		BUG_ON(msg_type != NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS);		// Only finish and progress are allowed
	}
	cinst.vtable.recov.reply_for_toma(msg_type, &pl->recov_status);
	return 0;
}

static inline struct nvmeibt_client_recovery_generic_header __gen_header_from(const struct lib_call_api_recov* rr) {
	return (struct nvmeibt_client_recovery_generic_header) {
		.id = rr->recov_params.id,
		.type = rr->recov_params.type,
		.max_batch_size = NVMEIBT_CLIENT_PROTOCOL_BATCH_SIZE_DONT_CARE,
		.effort_percents = rr->recov_params.effort_percents,
	};
}

void nvmesh_dp_lib_do_recovery_op(struct lib_call_api_recov* rr) {
	struct dplib_caller *sw = dplib_caller_create(mark_negative_rv(rr));
	if (!sw) {
		mark_not_started(rr);
		return;
	} else {
		struct lib_call_api_params_recov *rrp = &rr->recov_params;
		struct nvmeibc_subscription_ctx* tr = &sw->trs[rrp->seg_id];
		struct nvmeibt_client_recovery_start_pl rpl = {
			.task = __gen_header_from(rr),
			.is_mandatory = rrp->is_mandatory,
			.do_only_owners = rrp->do_only_owners,
			.praid_id = {0},
			.start_lock = rrp->start_lock,
			.num_locks = rrp->num_locks,
			.cold.surviving_ram_bmp = rrp->surviving_ram_bmp_cold_recov,	//.reserved = {0},
		};
		const int rv = nvmeibc_recovery_start_ext(tr, &sw->pr, &rpl);
		if (rv < 0){
			mark_not_started(rr);
			return;
		}
	}
	dplib_caller_free(sw);
}

void nvmesh_dp_lib_do_recovery_abort(struct lib_call_api_recov* rr) {
	struct dplib_caller *sw = (struct dplib_caller *)rr->exec.cancellation_context;
	sw->t.io_perm = NVMEIB_IO_TYPE_PERMIT_NONE_SUS;	// Emulate as if detach arrived, fastes possible cancel, better than option below
	// nvmeibc_recoveries_cancel(&sw->hdr);  // Sub optimal, for multiple recov workers, after cancel, 1 sync can be still executed per worker.
	__on_op_canceled_by_caller(sw);
}

void nvmesh_dp_lib_do_recovery_ping(struct lib_call_api_recov* rr) {
	struct dplib_caller *sw = (struct dplib_caller *)rr->exec.cancellation_context;
	struct lib_call_api_params_recov *rrp = &rr->recov_params;
	struct nvmeibc_subscription_ctx* tr = &sw->trs[rrp->seg_id];
	const struct nvmeibt_client_recovery_taskid_pl rpl = { .task =  __gen_header_from(rr) };
	nvmeibc_recovery_handle_request(tr , NVMEIBT_CLIENT_MSG_TR_RECOVER_PING, &rpl);
}
