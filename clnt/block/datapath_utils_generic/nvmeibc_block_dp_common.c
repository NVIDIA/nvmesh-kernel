/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_icore_ops.h"
#include "block/nvmeibc_topology.h"
#include "nvmeibc_block_dp_dbg_tools.h"
#include "../datapath_ec/nvmeibc_block_dp_ec.h"
#include "../datapath_mirror/nvmeibc_block_dp_mirror.h"
#include "../datapath_mirror/nvmeibc_block_dp_mirror_cmpxchng.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_profiling_lock_stages.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_profiling_disk_stages.h"

bool nvmeibc_dp_alloc_allow_io = false;		// By default is disabled (default is to use GFP_NOIO)
module_param(nvmeibc_dp_alloc_allow_io, bool, 0644);
MODULE_PARM_DESC(nvmeibc_dp_alloc_allow_io, "Do not use GFP_NOIO for data path memory allocations");

/************************* Configuration related methods **********************/
int __check_layout(u64 nlbas, u64 vlba, const struct nvmeibc_topology *t, bool check_max_write)
{
	int nchunks = t->nchunks, valid = -EIO;
	const struct nvmeibc_chunk *chunks = t->chunks;
	const u64 last_addr = chunks[nchunks-1].first_vlba;
	struct nvmeibc_block_device *nd = nvmeibc_block_t_to_b(t);
	struct nvmeibc_api_of_auto_extend *autoext = &nd->autoext;
	bool allow_thin_write = (check_max_write && autoext->is_api_enabled);

	if (unlikely(nchunks < 2)) {
		_NW(warn_dp_common_check_layout, DMESG_PREFIX("@DEV_NAME") ": Missing disk layout, cannot handle block I/O requests", nd->name);
	} else if (unlikely(nlbas==0)) {
		_NW(warn_1_dp_common_check_layout, DMESG_PREFIX("@DEV_NAME") ": Requested empty I/O operation", nd->name);
	} else if (unlikely(vlba > (1ULL << 63) || last_addr < vlba + nlbas)) {
		if (!allow_thin_write) {
			_NW(warn_2_dp_common_check_layout, DMESG_PREFIX("@DEV_NAME") ": Requested blocks [@VLBA-@VLBA] exceed disk size @VLBA", nd->name, vlba, (vlba + nlbas - 1), last_addr);
		}
	} else {
		valid = 0;
	}
	if (allow_thin_write) {
		const unsigned long last_lba = vlba + nlbas - 1;
		unsigned long current_last_lba = autoext->max_write_lba;

		if (last_lba < nd->size) { // Otherwise, it is still beyond the boundary
			while (last_lba > current_last_lba) {  // Atomically enlarge last addr
				cmpxchg(&autoext->max_write_lba, current_last_lba, last_lba);
				current_last_lba = autoext->max_write_lba;
			}
			if (valid) { // msleep(10);
				valid = -EAGAIN;
			}
		}
	}
	return valid;
}

/* Given [vlba.. vlba+nlbas) in chunk 'c' of topo 't', truncate it to the first protection raid (in stripe). Returns the rlba (raid 'offset') raid and length in the raid (nlba) */
static inline struct dp_io_topo_iterator_res __trunc_nlbas_to_stripe(u64 vlba, u64 nlbas, const struct nvmeibc_topology *t, int c)
{
	const struct nvmeibc_chunk *chunk = &t->chunks[c];
	const int stripe_size = chunk->stripe_size;
	int stripe_index = 0;
	struct dp_io_topo_iterator_res rv;
	rv.nlbas = nlbas;
	rv.rlba = vlba - chunk->first_vlba;	/* Without striping rlba = clba */

	if (t->stripe_width > 1) {
		const u64 nlbas_in_stripe = stripe_size - rv.rlba % stripe_size;
		stripe_index = (rv.rlba / stripe_size) % t->stripe_width;
		rv.rlba = (rv.rlba / (t->stripe_width * stripe_size)) * stripe_size + rv.rlba % stripe_size;
		if (rv.nlbas > nlbas_in_stripe) {
			rv.nlbas = nlbas_in_stripe;
		}
	}
	rv.r = &chunk->raid1s[stripe_index];
	return rv;
}

static inline u64 __trunc_nlabs_to_blockset(const struct nvmeibc_raid1* pr, u64 rlba, u64 nlbas)
{
	const u64 n_blks_in_blkset = LOCKSET_SLICES * pr->slice_size;
	const u64 n_remaining = (n_blks_in_blkset - (rlba % n_blks_in_blkset));
	return min(nlbas, n_remaining);
}

u64 dp_io_topo_iterator_conv_rlba_to_phys_lock_addr(const struct nvmeibc_raid1* r1, int si, u64 rlba)
{   /* Daniel: The calculation below is not accurate but it has to be aligned
       to LOCKSET_MASK, because first_lba is aligned the inaccuracy of
	   (rlba % r1->slice_size) cancels out. This is optimization */
	const u64 phys_offset_in_seg =  (rlba / r1->slice_size);
	return (r1->segments[si].first_lba + phys_offset_in_seg) & LOCKSET_MASK;
}

void dp_io_topo_iterator_conv_phys_lock_addr_to_raid_ofst(struct dp_io_topo_iterator_res *res, const struct nvmeibc_cmd_lock *l)
{
	const u64 phys_offset_in_seg = __lock_start(*l) - l->ds->first_lba;
	dp_io_topo_iterator_conv_seg_lock_addr_to_raid_ofst(res, l->ds, phys_offset_in_seg);
}

void dp_io_topo_iterator_conv_seg_lock_addr_to_raid_ofst(struct dp_io_topo_iterator_res *res, const struct nvmeibc_disk_segment *ds, const u64 phys_offset_in_seg)
{
	res->r =    nvmeibc_disk_segment_get_praid(ds);
	res->rlba = phys_offset_in_seg * res->r->slice_size;
	res->nlbas = LOCKSET_SLICES     * res->r->slice_size;
}

void dp_io_topo_iterator_init(struct dp_io_topo_iterator *it,
				u64 vlba, u64 nlbas, const struct nvmeibc_topology *t, int ci)
{
	it->t =         t;
	it->nlbas =     nlbas;
	it->vlba =      vlba;
	it->ci =        ci - 1;	// -1 coz iterator advances to start chunk
	it->nlbas_chunk = 0;
	it->res.r = NULL;
	it->res.rlba = 0;
	it->res.nlbas = 0;
}

static void dp_io_topo_iterator_consume(struct dp_io_topo_iterator *it)
{
	it->nlbas_chunk -= it->res.nlbas;
	it->vlba +=        it->res.nlbas;
	_ND(trace_dp_common_dp_io_topo_iterator_consume, "nlbas: chunk=@NLBAS_CHUNK, raid=@NLBAS_RAID", it->nlbas_chunk, it->res.nlbas);
}

bool dp_io_topo_iterator_next(struct dp_io_topo_iterator *it, const char what)
{
	dp_io_topo_iterator_consume(it); // Consume prev iteration
_start:
	if (it->nlbas_chunk) { 					// Process remaining chunk, by raids
		it->res = __trunc_nlbas_to_stripe(it->vlba, it->nlbas_chunk, it->t, it->ci);
		if (what == 'l')
			it->res.nlbas = __trunc_nlabs_to_blockset(it->res.r, it->res.rlba, it->res.nlbas);
		//dp_io_topo_iterator_consume(it); - Daniel: Not here, to let the user change 'res' values.
	} else if (it->nlbas) { 				// Process new (next) chunk
		it->ci++;
		it->nlbas_chunk = min(it->nlbas, it->t->chunks[it->ci + 1].first_vlba - it->vlba);
		it->nlbas -= it->nlbas_chunk;
		goto _start;						// Now we have a chunk to process
	} else {
		return false;						// Done processing IO
	}
	return true;							// Have remaining
}

static inline u64 __ec_calc_column_within_slice(const struct nvmeibc_raid1* r1, const int si, const int slice_start_seg_index) {
	const int seg_offset_within_slice = (r1->replicas + si - slice_start_seg_index) % r1->replicas;
	return ((seg_offset_within_slice < r1->slice_size) ? seg_offset_within_slice : 0);
}

u64 nvmeibc_datapath_dlba_to_rlba(const struct nvmeibc_datapath *dp, const struct nvmeibc_raid1* r1, const int si, const u64 dlba) {
	const u64 snake_size = dp->p.snake_size;
	const u64 slba = dlba - r1->segments[si].first_lba;
	if (r1->slice_size == 1) {
		return slba;			// R1, JBOD: slba == rlba
	} else {
		const u64 snake_index = (slba / snake_size);
		const u64 n_blks_in_full_snakes = snake_index * (u64)r1->slice_size * snake_size; 			// Num blocks in full snakes (snake column on all D+P segs)
		const int d0_si = get_owner_seg_slice_start(r1, slba * (u64)r1->slice_size);
		const u64 n_blks_in_full_cols_of_partial_snake = (snake_size * __ec_calc_column_within_slice(r1, si, d0_si));
		const u64 n_blks_in_partial_column = (slba % snake_size);
		return n_blks_in_full_snakes + n_blks_in_full_cols_of_partial_snake + n_blks_in_partial_column;
	}
}

/* For Disk lba to chunk lba translation, Description via variables
   Example: Raid 60, stripeing of 4 praids. Each praid of (6+2), snake_size = 2. full_snake=6*2 blocks, each stripe holds 2 snakes.
Disks 01234567  89..... ->
DLBA  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
0     AAAAAAPQ  AAAAAAPQ  BBBBBBPQ  CCCCCCPQ
1     AAAAAAPQ  AAAAAAPQ  BBBBBBPQ  CCCCCCPQ
2     AAAAAAPQ  AAAAAAPQ  BBBBBBPQ  CCCCCCPQ
3     AAAAAAPQ  AAAAAAPQ  BBBBBBPQ  CCCCCCPQ
.     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
.     DDDDDDPQ  DDDDDDPQ  EEEEEEPQ
.     DDDDDDPQ  DDDDDDPQ  EEEEEEPQ
|     DDDDDDPQ  DDDDDDPQ  FFG   PQ
V     DDDDDDPQ  DDDDDDPQ  FF    PQ
Definitions:
	A+B+C = Full Stripes
	D = Full blocksets of partial stripe
	E = Full snakes
	F = Full columns of partial snake
    G = Partial column of partial snake
    (B+E)/6+G = DLBA
    (B+E+F+G) = RLBA
    (A+B+C+D+E+F+G) = VLBA
Dlba->Rlba:
 1. Calc num blocks in full snakes = B + E
 2. Calc num blocks in full columns of partial snake = F
 3. Calc num blocks in last partial column G
 4. Return sum of above (B + E + F + G)
Dlba->Clba:
 1. Calc num blocks in full stripes without current praid = A + C
 2. Calc num blocks in prev praids in partial stripe = D
 3. Return sum of the above + Dlba->Rlba conversion = (A + C + D) + (B + E + F + G)
*/
static u64 dp_generic_trans_dlba_to_clba(const struct nvmeibc_datapath *dp, const struct nvmeibc_raid1* r1, int si, u64 dlba)
{
	const struct nvmeibc_chunk *chunk =           r1->segments->chunk;
	const u64 stripe_width =                      nvmeibc_chunk_get_num_raids(chunk->topology, chunk);
	const u64 slba =                              dlba - r1->segments[si].first_lba;
	const u64 number_of_full_stripes =            (slba * r1->slice_size) / chunk->stripe_size;
	const u64 n_blks_in_full_stripes =            number_of_full_stripes * (chunk->stripe_size * (stripe_width-1));	// Num blocks in full stripes (Raid 60) exclusding this praid
	const u64 n_blks_prev_praids_in_parital_stripe =  (r1 - (chunk)->raid1s) * chunk->stripe_size;	// Num blocks in previous praids in last partial stripe
	const u64 n_blks_in_cur_praid = nvmeibc_datapath_dlba_to_rlba(dp, r1, si, dlba);
	return n_blks_in_full_stripes + n_blks_prev_praids_in_parital_stripe + n_blks_in_cur_praid;
}

// Use dlba to clba and add chunk start lba
u64 nvmeibc_datapath_dlba_to_vlba(const struct nvmeibc_datapath *dp, const struct nvmeibc_raid1* r1, int si, u64 dlba)
{
	const u64 chunk_start_lba = r1->segments->chunk->first_vlba;
	const u64 clba = dp->dbg_trans_dlba_to_clba(dp, r1, si, dlba);
	return chunk_start_lba + clba;
}

/*********************** Recoveries draining component ************************/
void nvmeibc_recovs_drainer_reinit(struct nvmeibc_recoveries_drainer *d)
{
	d->num_running = 1; // kref_init(&d->num_running_recoveries);
	d->num_finished = 0;
	d->last_finished_jiff = jiffies;	// Avoid corner cases, as if recovery finished right now
}

void nvmeibc_recovs_drainer_init(struct nvmeibc_recoveries_drainer *d,
								  void (*fn)(void *_ctx), void *_ctx)
{
	spin_lock_init(&d->lock);
	d->__on_last_recovery_finish_cb = fn;
	d->_ctx = _ctx;
	nvmeibc_recovs_drainer_reinit(d);
}

static void __nvmeibc_recovs_drainer_inc_common( struct nvmeibc_recoveries_drainer *d, bool is_weak)
{
	ulong flags;
	spin_lock_irqsave(&d->lock, flags);
	d->num_running++;
	WARN_ON_ONCE(d->num_running < 2);				// kref_get(&(nd)->num_running_recoveries)		// Deliberatly not using kref_get_unless_zero() to detect flow bugs
	if (is_weak)
		d->num_running_weak++;
	spin_unlock_irqrestore(&d->lock, flags);
}

void nvmeibc_recovs_drainer_inc( struct nvmeibc_recoveries_drainer *d)
{
	__nvmeibc_recovs_drainer_inc_common(d, false /* is_weak */);
}

void nvmeibc_recovs_drainer_inc_weak( struct nvmeibc_recoveries_drainer *d)
{
	__nvmeibc_recovs_drainer_inc_common(d, true /* is_weak */);
}

static int __nvmeibc_recovs_drainer_dec_common(struct nvmeibc_recoveries_drainer *d, bool is_weak)
{
	int should_release;
	ulong flags;
	spin_lock_irqsave(&d->lock, flags);
	should_release = ((--d->num_running) == 0);			// kref_put(&(nd)->num_running_recoveries, __on_last_recovery_finish_upon_detach)
	if (!is_weak) {
		d->last_finished_jiff = jiffies;
		d->num_finished++;							// Eventually the last put will add +1 during detach
	} else {
		d->num_running_weak--;
	}
	spin_unlock_irqrestore(&d->lock, flags);
	if (should_release)
		d->__on_last_recovery_finish_cb(d->_ctx);
	return should_release;
}

int nvmeibc_recovs_drainer_dec( struct nvmeibc_recoveries_drainer *d)
{
	return __nvmeibc_recovs_drainer_dec_common(d, false /* is_weak */);
}

int nvmeibc_recovs_drainer_dec_weak( struct nvmeibc_recoveries_drainer *d)
{
	return __nvmeibc_recovs_drainer_dec_common(d, true /* is_weak */);
}

int nvmeibc_recovs_drainer_get_num( struct nvmeibc_recoveries_drainer *d,
									ulong *get_age, uint *num_finished, uint *num_running_weak)
{
	int rv;
	ulong flags;
	spin_lock_irqsave(&d->lock, flags);
	rv = (int)d->num_running; //kref_read(&(nd)->num_running_recoveries)
	if (get_age)
		*get_age = (((jiffies - d->last_finished_jiff) * 1000) / HZ);		// X[msecs] ago
	if (num_finished)
		*num_finished = d->num_finished;
	if (num_running_weak)
		*num_running_weak = d->num_running_weak;
	spin_unlock_irqrestore(&d->lock, flags);
	return rv - 1;			// -1 because it is kref
}

/************************** Datapath Virtual functions ************************/
static int __dp_default_ignore_all_operations(const struct operation *o)
{
	(void)o; return -EIO; // Ignore all ops with failure
}

bool nvmeibc_default_debug_di = false;		// By default is disabled (corrupts user data)
module_param(nvmeibc_default_debug_di, bool, 0644);
MODULE_PARM_DESC(nvmeibc_default_debug_di, "Upon volume attach, enable \"debug di\" mode.");

// use_debug_di from mgmt configuration, will be set once and will override local nvmeibc_default_debug_di parameter value
static inline bool __can_enable_debug_di_for_volume(enum nvmeibc_data_path_type e, const bool use_debug_di)
{
	if (e == NVMEIBC_DATA_PATH_MIR_MDBLK_BIO)
		return false;			// Disable for Metadata volumes
	return (use_debug_di) ? use_debug_di : nvmeibc_default_debug_di;
}

static void __init_no_write_hole_functions(struct no_writehole_functions *funcs, const bool is_raid1) {
	funcs->restore_function = (is_raid1) ? &dp_mirror_no_write_hole_fix         : &dp_ec_no_write_hole_fix;
	funcs->destroy_function = (is_raid1) ? &dp_mirror_no_write_hole_destroy     : &dp_ec_no_write_hole_destroy;
	funcs->sbs_cleanup =      (is_raid1) ? &dp_mirror_no_write_hole_sbs_cleanup : NULL;	// EC-2522 - why EC does not cleanup?
	funcs->get_restore_rv =   (is_raid1) ? NULL                                 : &dp_ec_no_write_hole_get_restore_rv;
}

static void __init_no_write_hole_func_null(struct no_writehole_functions *funcs) {
	memset(funcs, 0, sizeof(*funcs));	// All NULL;
}
void nvmeibc_datapath_syncs_resources_init(struct nvmeibc_datapath_syncs_resources *sr)
{
	INIT_LIST_HEAD(&sr->request_list);
	INIT_LIST_HEAD(&sr->resources_reuse_list);
	sr->fcntr = nvmeibc_flow_counters_ref();	// Global ptr, not per block device
}

void nvmeibc_datapath_syncs_resources_destroy( struct nvmeibc_datapath_syncs_resources *sr)
{
	WARN_ON(!list_empty(&sr->request_list));			// Impossible, recoviries drained, no new syncs can arrive
	WARN(!list_empty(&sr->resources_reuse_list) || sr->stats.num_el_in_resources_reuse_list, "nvmeibc: block device mem leaked %u, sync resources", sr->stats.num_el_in_resources_reuse_list);
}

void nvmeibc_datapath_init(struct nvmeibc_datapath *dp, const char* name, enum nvmeibc_data_path_type e, bool enable_edic_check, bool use_debug_di, bool enable_local_read_optimization, int read_has_mutable_bio_buffers, int slice_size_split, struct nvmeibc_dp_params par)
{
	dp_io_stats_init(&dp->io_stats);
	slow_io_stats_t_init(  &dp->io_slow);
	nvmeibc_datapath_syncs_resources_init(&dp->sync_rsrcs);
	dp->p = par;
	dp->alignment_sectors.read = 0;										// Default No limit for reads
	dp->elevator.max_elev_write = 0;									// Default, No limitation on size of writes
	dp->sizeof_operation = sizeof(struct operation);
	dp->enable_edic_check = enable_edic_check;							// Management supplies this value
	dp->read_has_mutable_bio_buffers = read_has_mutable_bio_buffers;
	dp->allow_locks_rel_debug =  &dp_mirror_complete_locks_debug_val;	// Default value
	dp->dbg_trans_dlba_to_clba = &dp_generic_trans_dlba_to_clba;
	dp->enable_local_read_optimization = false;							// Default value
	dp->enable_multi_degraded_writes = (par.protect_level > 1);
	dp->enable_care_about_txid = false;									// Default

	// Consider refactoring into DP types (EC has ReadModifyWrite in syncs, and EC has write journal which should be the same as normal write)
	dp->disk_profiling_defs =			(struct nvmeibc_profiler_configuration){.type = NVMEIBC_PROFILER_TYPE_CMD,  .stage2name=&nvmeibc_profiling_block_io_op_stage2name,			.translate=&nvmeibc_profiling_translate_block_io_op};
	// Consider refactoring into DP types (JBOD has no locks what so ever, EC has owner and copies, Mirror Has owner/secondary/active)
	dp->lock_profiling_defs = 			(struct nvmeibc_profiler_configuration){.type = NVMEIBC_PROFILER_TYPE_CMD,  .stage2name=&nvmeibc_profiling_lock_stage2name,					.translate=&nvmeibc_profiling_translate_rdma_intent};
	// Consider refactoring into DP types (JBOD being the unique one with much less stages)
	dp->preparation_profiling_defs =	(struct nvmeibc_profiler_configuration){.type = NVMEIBC_PROFILER_TYPE_FLOW, .stage2name=nvmeibc_profiling_e_preparation_stage_stage2name,	.translate=&nvmeibc_profiling_translate_e_preparation_stage};
	// Generic sync profiling in handle_locks_o function
	dp->sync_profiling_defs =			(struct nvmeibc_profiler_configuration){.type = NVMEIBC_PROFILER_TYPE_FLOW, .stage2name=&nvmeibc_profiling_sync_stage_stage2name,			.translate=&nvmeibc_profiling_translate_sync_stages};

	switch (e) {
	case NVMEIBC_DATA_PATH_JBODS:
		dp->should_ignore_op =       &dp_mirror_should_ignore_op;
		dp->prepare_op =             &dp_mirror_prepare_op;
		dp->execute_op =             &dp_mirror_execute_op;
		dp->cmd_comp_cb =            &dp_mirror_block_completion;
		dp->exec_func_on_locks_tkn = NULL;
		dp->exec_func_on_stage_end = &dp_mirror_exec_func_on_stage_end;
		dp->calc_should_abandon =    NULL;
		dp->calc_comp_state =        &dp_mirror_calc_comp_state;
		dp->allow_locks_rel_debug =  NULL;
		dp->dbg_trans_addr =         &dp_mirror_translate_addr;
		dp->sync_prepare_op =        NULL;
		dp->sync_execute_op =        NULL;
		dp->turn_off_dbits_before_io = dp->enable_store_dirty_bits_in_peristent_md = false;	// Dbits dont exist
		__init_no_write_hole_func_null(&dp->nwhole_funcs);
		dp->good_path_io_profiling_defs = (struct nvmeibc_profiler_configuration){ .type = NVMEIBC_PROFILER_TYPE_FLOW, .stage2name=&nvmeibc_profiling_e_cmds_stage_stage2name_mirror, .translate=&nvmeibc_profiling_translate_e_cmds_stage_mirror};
		break;

	case NVMEIBC_DATA_PATH_MIR_BIO:
		dp->should_ignore_op =       &dp_mirror_should_ignore_op;
		dp->prepare_op =             &dp_mirror_prepare_op;
		dp->execute_op =             &dp_mirror_execute_op;
		dp->cmd_comp_cb =            &dp_mirror_block_completion;
		dp->exec_func_on_locks_tkn = &dp_mirror_exec_func_on_locks_tkn;
		dp->exec_func_on_stage_end = &dp_mirror_exec_func_on_stage_end;
		dp->calc_should_abandon =    &dp_mirror_calc_should_abandon;
		dp->calc_comp_state =        &dp_mirror_calc_comp_state;
		dp->dbg_trans_addr =         &dp_mirror_translate_addr;
		dp->sync_prepare_op =        &dp_mirror_sync_prepare_op;
		dp->sync_execute_op =        &dp_mirror_sync_execute_op;
		dp->enable_local_read_optimization = enable_local_read_optimization;	// From management + reservation mode
		dp->enable_store_dirty_bits_in_peristent_md = false;	// Currently not supported as R1 blockset fixup is fast, so even if disks are formatted with md, we dont store dbits in them
		dp->turn_off_dbits_before_io = false;					// No need for such an optimization as goodpath already support this.
		__init_no_write_hole_functions(&dp->nwhole_funcs, true);
		dp->elevator.max_elev_write = LOCKSET_SLICES;							// Elevator will not unify IO's from different blocksets
		dp->alignment_sectors.write = ((128)*(LOCKSET_SLICES << KERNEL_SECTOR_TO_SECTOR_SHIFT));		// Forbid IO longer than 128[blksets] = 16[mb], else memory allocations of arrays may become too large
		dp->good_path_io_profiling_defs = (struct nvmeibc_profiler_configuration){ .type = NVMEIBC_PROFILER_TYPE_FLOW, .stage2name=&nvmeibc_profiling_e_cmds_stage_stage2name_mirror, .translate=&nvmeibc_profiling_translate_e_cmds_stage_mirror};
		break;

	case NVMEIBC_DATA_PATH_EC_R6:
		dp->should_ignore_op =       &dp_ec_should_ignore_op;
		dp->prepare_op =             &dp_ec_prepare_op;
		dp->execute_op =             &dp_ec_execute_op;
		dp->cmd_comp_cb =            &dp_ec_block_completion;
		dp->exec_func_on_locks_tkn = &dp_ec_exec_func_on_locks_tkn;
		dp->exec_func_on_stage_end = &dp_ec_exec_func_on_stage_end;
		dp->calc_should_abandon =    &dp_ec_calc_should_abandon;
		dp->calc_comp_state =        &dp_ec_calc_comp_state;
		dp->dbg_trans_addr =         &dp_ec_translate_addr;
		dp->sync_prepare_op =        &dp_ec_sync_prepare_op;
		dp->sync_execute_op =        &dp_ec_sync_execute_op;
		dp->enable_store_dirty_bits_in_peristent_md = true;	// EC blockset fixup is expensive and due to {d2j,j2d} always require metadata so we use it for dbits
		dp->turn_off_dbits_before_io = true;				// Todo: Explain why we use this optimization
		dp->enable_care_about_txid = true;
		dp->sizeof_operation += sizeof(struct multi_snake_slice_analyzer);
		__init_no_write_hole_functions(&dp->nwhole_funcs, false);
		dp->alignment_sectors.write = ((slice_size_split * dp->p.binje       ) << KERNEL_SECTOR_TO_SECTOR_SHIFT);    // Slice size * consecutive blocks (multi-slice/snake) (N >= S && N%S == 0)
		dp->alignment_sectors.read =   (slice_size_split * LOCKSET_SLICES)     << KERNEL_SECTOR_TO_SECTOR_SHIFT;			// Split EC reads to locksets
		dp->elevator.max_elev_write =  (slice_size_split * dp->p.snake_size  );									     // Elevator will not unify IO's more than then a single snake
		dp->good_path_io_profiling_defs = (struct nvmeibc_profiler_configuration){.type = NVMEIBC_PROFILER_TYPE_FLOW, .stage2name=&nvmeibc_profiling_e_cmds_stage_stage2name_ec, .translate=&nvmeibc_profiling_translate_e_cmds_stage_ec};
		WARN(((dp->alignment_sectors.write >> KERNEL_SECTOR_TO_SECTOR_SHIFT) % dp->p.snake_size), DMESG_PREFIX("%s: ") "EC configuration error, alignment_sectors=%u, snake_size=%u\n", name, dp->alignment_sectors.write, dp->p.snake_size);
		break;

	default:
		_NE(t_01_dp_init, DMESG_PREFIX("@DEV_NAME") ": unsupported @DATAPATH_TYPE. IO ignorred", name, e);
		dp->should_ignore_op      = &__dp_default_ignore_all_operations;
		/* Other functions are NULL, and never used */
		break;
	}

	if (__can_enable_debug_di_for_volume(e, use_debug_di)) {
		_NT(trace_dp_common_nvmeibc_datapath_init, "@DEV_NAME: Enabling DI Debug Mode", name);
		dp->enable_di_debug_mode = true;
	}
	_NT(t_02_dp_init, "@DEV_NAME: Alignment[512b]={W=@X, R=@X}, Elev_merge=@X[blks], snake=@X[blks], Jentry=@BINJE[blks]", name, dp->alignment_sectors.write, dp->alignment_sectors.read, dp->elevator.max_elev_write, dp->p.snake_size, dp->p.binje);
}

void nvmeibc_datapath_destroy(struct nvmeibc_datapath *dp)
{
	nvmeibc_datapath_syncs_resources_destroy(&dp->sync_rsrcs);
}

raid_sgmnt_t nvmeibc_dp_get_sgmnt_idx_from_ds(const struct nvmeibc_disk_segment *ds)
{
	return ds->toma_reg->seg;
}
