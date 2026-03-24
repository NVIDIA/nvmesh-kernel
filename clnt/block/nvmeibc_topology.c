/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_txt.h"
#include "nvmeibc_block.h"					// Must be first for simulator
#include "nvmeib_public.h"
#include "nvmeib_types.h"
#include "nvmeib_event.h"
#include "block/nvmeibc_block_common.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_topology.h"
#include "nvmeib_utils.h"
#include "recovery/nvmeibc_raid_recovery.h"
#include "block/controlpath/nvmeibc_b_cp_trs_hash.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeibc_main.h"					// Schedule req-conf messages to mgmt
#include "block/controlpath/nvmeibc_b_cp_topo_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "main/cc_api/nvmeibc_main_capi_manipulate_vols.h"	// For attach_t
#include "management_utils_common/nvmeibc_management_volume_conf_checks.h"
#include "common/compat/kr_incs_compiler_types.h"
#include "common/proc_epilog.h"
#include <linux/module.h>
#include "compat/kr_incs_time_rdtsc.h"

NVMEIBC_MEMMGR_METRIC(io_ctrl_topologies, "component=raid.io_ctrl.topologies");

#define NVMEIBC_SGMNT_DEFAULT_MD_SIZE sizeof(union nvmeibc_block_dp_ec_data_block_md)

// Todo: rearrange methods and remove the declarations below
static int __unregister_all_segments_io_is_possible(struct nvmeibc_topologies *nt);
static int __unregister_all_segments_on_cleanup_no_io(struct nvmeibc_topologies *nt);

static int   __register_all_segments(struct nvmeibc_topologies *nt);
static int __all_segments_apply_reconf(struct nvmeibc_topologies *nt, bool force_unrequested, struct nvmeibc_subscription_ctx *optional_tr, struct nvmeibc_idisk *disk);
static int __raid1_try_to_apply_RUD(struct nvmeibc_subscription_ctx *tr, struct nvmeibt_client_msg *pl, int len);
static int __segment_register(struct nvmeibc_disk_segment *seg);
static int nvmeibc_warm_raid1_apply_conf_diffs(struct nvmeibc_topologies *nt, struct nvmeibc_subscription_ctx *tr);
static inline int __toma_disconnect_segment(struct nvmeibc_disk_segment *seg);
extern void nvmeibc_volume_update_volume_single_segment(void *context, const struct nvmeibc_cinst_params_main *p);
static void __topo_status_tostring(const struct nvmeibc_topology *t, struct nvmeib_txt *txt);

static void *topo_kzalloc(size_t size, gfp_t flags)
{
	void *ptr = kzalloc(size, flags);
	nvmesh_memmgr_metric_on_alloc_update(io_ctrl_topologies, ptr? ksize(ptr): size, ptr);
	return ptr;
}

static void *topo_kmalloc(size_t size, gfp_t flags)
{
	void *ptr = kmalloc(size, flags);
	nvmesh_memmgr_metric_on_alloc_update(io_ctrl_topologies, ptr? ksize(ptr): size, ptr);
	return ptr;
}

static void topo_kfree(void *ptr)
{
	if (ptr)
		nvmesh_memmgr_metric_on_free_update(io_ctrl_topologies, ksize(ptr));
	kfree(ptr);
}

/********************** Protection Raid persistency ***************************/
struct nvmeibc_raid_topo_persistent* nvmeibc_raid_topo_persistent_create(void)
{
	struct nvmeibc_raid_topo_persistent* rv = topo_kzalloc(sizeof(*rv), GFP_KERNEL);
	if (unlikely(!rv))
		return NULL;
	if (unlikely(stale_lock_resolver_get_create(&rv->slr) < 0)) {
		topo_kfree(rv);
		return NULL;
	}
	//profile objects initialized to 0 and this is good enough
	nvmeibc_recoveries_create(rv);
	nvmeibc_b_cp_loser_init(&rv->loser);
	atomic_set(&rv->refcount, 1);
	return rv;
}

void nvmeibc_seg_topo_persistent_init_profilers(struct nvmeibc_disk_segment *seg, struct nvmeibc_block_device* bdev, const int chunk, const int raid, const int segment)
{
	const struct nvmeibc_datapath *dp = &bdev->dp;
	char desc[ARRAY_SIZE(seg->lock_operation_profiler->desc)];

	//lets verify the function is not called twice
	BUG_ON(nvmeibc_profiling_is_initialized(seg->lock_operation_profiler));
	BUG_ON(nvmeibc_profiling_is_initialized(seg->disk_operation_profiler));

	snprintf(desc, ARRAY_MEM_SIZE(desc), "%s Lock OP (%d/%d/%d)", bdev->name, chunk, raid, segment);
	seg->lock_operation_profiler = nvmeibc_profiling_create(dp->lock_profiling_defs, desc);

	snprintf(desc, ARRAY_MEM_SIZE(desc), "%s Disk OP (%d/%d/%d)", bdev->name, chunk, raid, segment);
	seg->disk_operation_profiler = nvmeibc_profiling_create(dp->disk_profiling_defs, desc);
}

void nvmeibc_seg_topo_persistent_clear_profilers(struct nvmeibc_disk_segment *seg)
{
	if (nvmeibc_profiling_is_initialized(seg->lock_operation_profiler)) {
		nvmeibc_profiling_clear(seg->lock_operation_profiler);
	}
	if (nvmeibc_profiling_is_initialized(seg->disk_operation_profiler)) {
		nvmeibc_profiling_clear(seg->disk_operation_profiler);
	}
}

void nvmeibc_raid_topo_persistent_init_profilers(struct nvmeibc_raid_topo_persistent* self, struct nvmeibc_block_device* bdev, int chunk, int raid){
	char desc[ARRAY_SIZE(self->good_path_profile[0]->desc)];
	const struct nvmeibc_datapath *dp = &bdev->dp;
	unsigned verb;

	//lets verify the function is not called twice
	for (verb = 0; verb < VERB_RW_NUM; verb++)
		BUG_ON(nvmeibc_profiling_is_initialized(self->good_path_profile[verb]));
	BUG_ON(nvmeibc_profiling_is_initialized(self->sync_profile));

	snprintf(desc, ARRAY_MEM_SIZE(desc), "%s Sync (%d/%d)", bdev->name, chunk, raid);
	self->sync_profile = nvmeibc_profiling_create(dp->sync_profiling_defs, desc);

	for (verb = 0; verb < VERB_RW_NUM; verb++) {
		snprintf(desc, ARRAY_MEM_SIZE(desc), "%s Good Path Execution %s (%d/%d)", bdev->name, verb_to_string(verb, false), chunk, raid);
		self->good_path_profile[verb] = nvmeibc_profiling_create(dp->good_path_io_profiling_defs, desc);
	}

	if (!nvmeibc_profiling_is_initialized(bdev->preparation_profiler)) { // Only once pre bdev
		snprintf(desc, ARRAY_MEM_SIZE(desc), "%s Preparation", bdev->name);
		bdev->preparation_profiler = nvmeibc_profiling_create(bdev->dp.preparation_profiling_defs, desc);
	}
}

void nvmeibc_raid_topo_persistent_clear_profilers(struct nvmeibc_raid_topo_persistent* self){
	unsigned verb;
	for (verb = 0; verb < VERB_RW_NUM; verb++) {
		if (nvmeibc_profiling_is_initialized(self->good_path_profile[verb])) {
			nvmeibc_profiling_clear(self->good_path_profile[verb]);
		}
	}
	if (nvmeibc_profiling_is_initialized(self->sync_profile)) {
		nvmeibc_profiling_clear(self->sync_profile);
	}
}

void nvmeibc_raid_topo_persistent_clear_preparation_profiler(struct nvmeibc_block_device* bdev){
	if (nvmeibc_profiling_is_initialized(bdev->preparation_profiler)) // Only once pre bdev
		nvmeibc_profiling_clear(bdev->preparation_profiler);
}

void nvmeibc_raid_topo_persistent_add_ref(struct nvmeibc_raid_topo_persistent*p)
{
	atomic_inc(&p->refcount);
}

void nvmeibc_raid_topo_persistent_put_ref(struct nvmeibc_raid_topo_persistent*p)
{
	const int ref = atomic_dec_return(&p->refcount);
	if (ref == 0) {
		unsigned verb;
		stale_lock_resolver_get_destroy(&p->slr);
		nvmeibc_recoveries_destroy(p);
		nvmeibc_b_cp_loser_destroy(&p->loser);
		BUG_ON(!nvmeibc_profiling_put(p->sync_profile));
		for (verb = 0; verb < VERB_RW_NUM; verb++)
			BUG_ON(!nvmeibc_profiling_put(p->good_path_profile[verb]));
		topo_kfree(p);
	}
}

struct stale_lock_resolver_t * nvmeibc_raid_topo_persistent_get_slr(struct nvmeibc_raid_topo_persistent*p)
{
	return &p->slr;
}

/***************************** Segment API ***********************************/
/* Each segment has a pointer to replacement (for segment relocation).
   Also it can be deleted (raid1 downgraded), upgraded in 2 ways and there must
   be a boolean, whether toma already requested to the action (above) or not.
   To reduce the mess, all 5 fields are packed into one pointer, using flags.
   Normal case: Seg->Rep==NULL	(Segment has no action on it)
   Client gets segment relocation reconf before toma:
    	Seg->Rep->Rep==NULL		(allocated replacement, waiting for toma)
    	Seg->Rep->Rep==Toma 	(Toma instructed to apply replacement)
    	Apply replacement. Resulting in normal case
   Client gets segment relocation reconf after toma:
    	Seg->Rep==Toma 			(Toma instructed to act, but we dont know how)
    	Seg->Rep->Rep==Toma 	(Getting reconf, inserting replacement)
    	Apply replacement. Resulting in normal case
   Client gets volume downgrade reconf before toma:
    	Seg->Rep==DEL			(marked for deletion, waiting for toma)
    	Seg->Rep==DEL|Toma 		(Toma instructed to delete)
    	Make downgrade. Affects entire Raid1 not just this segment
   Client gets volume downgrade reconf after toma:
    	Seg->Rep==Toma 			(Toma instructed to act, but we dont know how)
    	Seg->Rep==DEL|Toma 		(Getting reconf, mark deletion)
    	Make downgrade. Affects entire Raid1 not just this segment
   Client gets volume upgrade reconf before toma:
    	Seg->Rep->Rep==UP		(alloc second seg, mark upgrade, wait for toma)
    	Seg->Rep->Rep==UP|TOMA	(Toma instructed to upgrade)
    	Make upgrade. Affects entire Raid1 not just this segment
   Client gets volume upgrade reconf after toma:
    	Seg->Rep==Toma 			(Toma instructed to act, but we dont know how)
    	Seg->Rep->Rep==UP|Toma 	(Getting reconf, inserting second seg)
    	Make upgrade. Affects entire Raid1 not just this segment
    	Note: Raid1 upgrade can be done in 2 ways {seg,seg->rep} or
    	{seg->rep,seg}. There is a 1-bit flag 'o' for that
		o - index where replacement will be if 0 -> {rep,seg}, 1 -> {seg,rep}
   */
const u64 REP_POISON1   =(0x0000000A);	/* Toma instructed to act */
const u64 REP_POISON2   =(0x000000D0);	/* Segment marked for deletion */
const u64 REP_POISON3   =(0x00000020);	/* Replacement is actually second seg */
/* Note: Every pointer > 0xFF is a real pointer, and not just our flags */
#define __r(seg) (seg)->replacement
#define __has_attached(seg)       (((u64)    __r(seg) &  (~0x00FF)) !=0)
#define __did_toma_said_apply(seg)(((u64)    __r(seg) &REP_POISON1) !=0)
#define __is_marked_down(seg)     (((u64)    __r(seg) &REP_POISON2) !=0)
#define __is_marked_upgr(seg)     (((u64)__r(__r(seg))&REP_POISON3) !=0)

static inline bool seg_has_replacement(const struct nvmeibc_disk_segment *seg)
{
    return __has_attached(seg) && !__is_marked_upgr(seg);
}

static inline bool seg_has_upgrade(const struct nvmeibc_disk_segment *seg)
{
    return __has_attached(seg) && __is_marked_upgr(seg);
}

static inline bool seg_has_downgrade(const struct nvmeibc_disk_segment *seg)
{
    return !__has_attached(seg) && __is_marked_down(seg);
}

static inline bool seg_has_any_reconf(const struct nvmeibc_disk_segment *seg)
{
    return __has_attached(seg) || __is_marked_down(seg);
}

static inline u64 seg_get_upgrade_order(const struct nvmeibc_disk_segment *seg)
{
    return (((u64)__r(__r(seg)))>>4) & 0x1;
}

#define __mark_toma_said_apply(seg)({ \
	__r(seg)      = (void*)((u64)    __r(seg)  | REP_POISON1); })
#define __un_mark_toma_said_apply(seg)({ \
	__r(seg)      = (void*)((u64)    __r(seg)  & (~REP_POISON1)); })
#define seg_mark_for_down(seg)({ \
	__r(seg)      = (void*)((u64)    __r(seg)  | REP_POISON2); })
#define seg_mark_for_upgr(seg, o)({ \
	__r(__r(seg)) = (void*)((u64)__r(__r(seg)) | REP_POISON3 | ((o&0x1)<<4)); })

static inline bool seg_has_toma_said_apply(const struct nvmeibc_disk_segment *seg)
{
	return __has_attached(seg) ? __did_toma_said_apply(__r(seg)) : __did_toma_said_apply(seg);
}

static inline void seg_mark_toma_said_apply(struct nvmeibc_disk_segment *seg)
{
	__has_attached(seg) ? __mark_toma_said_apply(__r(seg)) : __mark_toma_said_apply(seg);
}

static inline void seg_unmark_toma_said_apply(struct nvmeibc_disk_segment *seg)
{
	__has_attached(seg) ? __un_mark_toma_said_apply(__r(seg)) : __un_mark_toma_said_apply(seg);
}

// A place holder per registration information, currently empty
#define __per_reg_free(...)
#define __per_reg_init(...)
#define __per_reg_clear(...)		//(seg)->per_reg = NULL

/* Before freeing the segment we must stop unneeded toma messages handler */
static void __seg_prepare_for_free(struct nvmeibc_disk_segment *seg,
			bool is_detached_topo /* Free bcz detach or segment relocation*/)
{
	unsigned long flags;
	struct nvmeibc_subscription_ctx *tr = seg->toma_reg;	// seg still holds ref to tr.
	tr->disk = seg->disk;
	if (is_detached_topo) {
		// Daniel Todo: EXC-1920, This WARN_ON should always be tested, not only on detach.
	    WARN(seg->registration_status, "nvmeibc bug, segment was not unregistered tr=%p", tr);
		/* Dont change 'seg->shared_own' In detach, last topo (not this) takes ownership */
	} else {
		seg->shared_own = SEG_SHARED_MEMO_ALL; /* On segment relocation, current topo is the last which uses this segment */
	}
	_NTTR(t_01_topo_seg_pfor_free, "segment left corpse. handle=@HANDLE", tr->handle);
	WARN(tr->status != NVMEIBC_SUBSCRIPTION_STATUS_NORMAL, "nvmeibc bug, double free of tr=%p", tr);
	/* Daniel: Don't do tr->nt = NULL; And don't change any field of it except status! Because other refs may use tr now */
	spin_lock_irqsave(&tr->death_lock, flags);
	tr->status = NVMEIBC_SUBSCRIPTION_STATUS_DEAD;
	spin_unlock_irqrestore(&tr->death_lock, flags);
	nvmeibc_trs_hash_remove(tr);
}

static void __seg_get_profilers(struct nvmeibc_disk_segment *seg)
{
	nvmeibc_profiling_get(seg->disk_operation_profiler);
	nvmeibc_profiling_get(seg->lock_operation_profiler);
}


static void __seg_put_profilers(struct nvmeibc_disk_segment *seg)
{
	if (nvmeibc_profiling_put(seg->disk_operation_profiler))
		seg->disk_operation_profiler = NULL;
	if (nvmeibc_profiling_put(seg->lock_operation_profiler))
		seg->lock_operation_profiler = NULL;
}

/* Remove segment from the system entirely */
static void __seg_free(struct nvmeibc_disk_segment *seg)
{
	if (seg_has_replacement(seg)||seg_has_upgrade(seg)) {
		if (seg->replacement->toma_reg)
			__put_tr(seg->replacement->toma_reg); /* replacements ref */
		__seg_put_profilers(seg->replacement);
		topo_kfree(seg->replacement);	/* 1 or 2 segs */
		seg->replacement = NULL;
	}
	__seg_put_profilers(seg);

	if (seg->shared_own & SEG_SHARED_MEMO_REGISTRAT) {
		__per_reg_free(seg);
	}
	if (seg->shared_own & SEG_SHARED_MEMO_SUBSCRIPT) {
		__toma_disconnect_segment(seg); /* If failed, too bad... */
		nvmeibc_seg_on_active_free(seg);
		__seg_put_profilers(seg);
	} else {
		if (seg->toma_reg)
			__put_tr(seg->toma_reg);	/* Segments ref, definitly not last */
		/* Note: seg->on_active != NULL, meaning on this topo we will not apply
		   the delayed message, but rather will do it on the next topo.
		   That is why we don't free on_active. */
	}

}

/* Copy only the data of segment without all its refs */
static void __seg_shallow_copy_unsafe(struct nvmeibc_disk_segment *dst,
		  const struct nvmeibc_disk_segment *src)
{
	WARN((src == dst) , "nvmeibc bug in seg deep copy\n");	// Incorrect usage
	WARN(dst->toma_reg, "nvmeibc bug, old seg has tr\n");	// Incorrect usage
	BUG_ON(dst->lock_operation_profiler);
	*dst = *src;
	__seg_get_profilers(dst);		// Add refcount after copying the profilers
	if (dst->toma_reg) 				__add_ref_to_tr(dst->toma_reg);
}

/* Move replacement into raid->segments array (kind of realloc()) */
static void __seg_shallow_move_unsafe(struct nvmeibc_disk_segment *dst,
		  const struct nvmeibc_disk_segment *src)
{
	*dst = *src; //assuming src is going to be release right after calling this function.
}

/* Copy the entire segment's data + replacements */
static int __seg_deep_copy(struct nvmeibc_disk_segment *dst, struct nvmeibc_chunk *dst_chunk, const struct nvmeibc_disk_segment *src)
{
	struct nvmeibc_disk_segment *replacement = NULL;
	int rv = -ENOMEM, n_segs_to_alloc;
	__seg_shallow_copy_unsafe(dst, src);
	dst->chunk = dst_chunk;
	if      (seg_has_replacement(src)) n_segs_to_alloc = 1; // EC raid may have only replacement
	else if (seg_has_upgrade(    src)) n_segs_to_alloc = 2;
	else                               n_segs_to_alloc = 0;

	if (n_segs_to_alloc) {
		if (!(replacement = topo_kzalloc(sizeof(*dst) * n_segs_to_alloc, GFP_ATOMIC)))
			goto _out;
		dst->replacement = replacement;
		__seg_shallow_copy_unsafe(dst->replacement, src->replacement); /* Copy the first seg */
	}
	rv = 0;
_out:
	return rv;
}

/* Exactly like __get_seg_by_t() but must verify input */
struct nvmeibc_disk_segment *nvmeibc_topology_get_seg(struct nvmeibc_topology *t,
							int chunk, int r, int seg)
{
	struct nvmeibc_raid1 *r1;
	bool is_valid = (chunk < __get_topo_num_chunks(t));
	is_valid = (is_valid && (r < __get_num_r1s(t)));
	if (!is_valid)
		return NULL;
	r1 = __get_r1_by_t(t, chunk,r);
	return (seg < r1->replicas) ? r1->segments+seg : NULL;
}

/******************************* raid1 API ***********************************/
void nvmeibc_segment_clear_new_topo_upon_unreg(struct nvmeibc_disk_segment *seg)
{
	#ifdef DEBUG_TOPO_CNTRS
		if ((seg->toma_reg)&&(seg->registration_status))
			BUG_ON(atomic_dec_return(&seg->toma_reg->n_registers) != 0);
	#endif
	seg->registration_status = SEG_REGSTATUS_EMPTY;
	__per_reg_clear(seg);
	nvmeibc_seg_on_active_free(seg);	// If exists (unlikely)
}

void nvmeibc_segment_clear_old_topo_upon_unreg(struct nvmeibc_disk_segment *seg)
{
	seg->shared_own = SEG_SHARED_MEMO_REGISTRAT;
}

void nvmeibc_raid1_clear_new_topo_upon_unreg(struct nvmeibc_raid1 *r1)
{
	int i;
	r1->version = 0;
	r1->toma.topo_checksum = 0;
	r1->lid.all = LS_UNLOCKED;
	nvmeibc_topo_init_io_perm(&r1->toma);
	for (i=0; i<r1->replicas; i++)
		nvmeibc_segment_clear_new_topo_upon_unreg(&r1->segments[i]);
}

void nvmeibc_raid1_clear_old_topo(struct nvmeibc_raid1 *r1)
{
	struct nvmeibc_raid_topo_persistent *hdr = r1->hdr;
	nvmeibc_recoveries_cancel(hdr);
	stale_lock_resolver_clear_all(   &hdr->slr);
}

static void __nvmeibc_raid1_clear_old_topo_1st_tr(struct nvmeibc_topology *old_t, const struct nvmeibc_raid1 *new_r1)
{
	nvmeibc_raid1_clear_old_topo(__get_r1_by_tr(old_t, new_r1->segments->toma_reg));
}

static bool nvmeibc_praid_has_valid_primary_owner_lock(const struct nvmeibc_raid1 *r1)
{
	int i;
	for (i=0; i<r1->replicas; i++) {
		const struct nvmeibc_disk_segment *seg = &r1->segments[i];
		if (seg->lmap.si[0] == seg->toma_reg->seg)
			return true;
	}
	return false;
}

/* Switch locations of seg0 and seg1, like chess castling move */
static inline void __nvmeibc_raid1_castle_segs(struct nvmeibc_raid1 *r1)
{
	struct nvmeibc_disk_segment *s = r1->segments;
	struct nvmeibc_disk_segment tmp;
	BUG_ON(r1->replicas!=2);
	tmp = s[0]; s[0] = s[1]; s[1] = tmp;						/*EC-1473: Fix for 3 mirrored R1*/
	s[0].toma_reg->seg = 0;
	s[1].toma_reg->seg = 1;
	BUG();	// Not supported, lock map gets scrambled
}

static inline bool __does_raid1_uses_disk( struct nvmeibc_raid1 *r1,
										   const struct nvmeibc_idisk *disk)
{
	int i;
	for (i=0; i<r1->replicas; i++) {
		if (r1->segments[i].disk == disk)
			return true;
	}
	return false;
}

/******************************* chunk API ***********************************/
/* Note: Every pointer > 0xFF is a real pointer, and not just our flags.
	We use same idea as with segments reconfiguration by marking the chunk's
	pointer. Special mark means (a few last) chunks have to be removed.*/
#define chunk_has_removal(     t) ((t)->resize.chunks == (void*)REP_POISON2)
#define chunk_mark_for_removal(t)  (t)->resize.chunks =  (void*)REP_POISON2
#define chunk_has_addition(    t) (((u64)(t)->resize.chunks & ~0xFF) != 0)

/* Specific functionality for volume grow in hot fashion. Algorithm:
	1. We have a valid topology of N chunks.
	2. Append K chunks. If warm, update the size of block device immediately
	3. Else if hot, Mark the last K chunks as disabled. Topo where segments of
		first N chunks are OK is good for IO even if segments of last K chunk
		are unregistered.
	4. Once toma approves registration of all the segments of last K chunks it
		becomes enabled and we update block size, like we did in warm.
	5. We store the amount of disabled last chunks in a dedicated variable
*/
#define __topo_num_last_chunks_disabled(t)	((t)->resize.n_last_disabled_chunks)
#define __topo_set_resizing_to(t, _size) do {t->resize.size = _size; }while(0)

static void __topo_calc_chunk_binary_search_start(struct nvmeibc_topology *t)
{
	int n;
	for (n = 1; n < __get_topo_num_chunks(t); n <<= 1);
	t->highest_pow2_in_chunk_ind = (n >> 1);				// Example: if config has 6 chunks then index of last one is 5 and highest pow2 is 4
}

void nvmeibc_topo_update_size_of_bdev(struct nvmeibc_topology *t)
{
	struct nvmeibc_block_device *dev = nvmeibc_block_nt_to_b(t->nt);
	dev->autoext.allocated_size = t->chunks[__get_topo_num_chunks(t)].first_vlba;
	if (!dev->autoext.is_api_enabled)
		dev->size = dev->autoext.allocated_size;	// Regular volume
	__topo_calc_chunk_binary_search_start(t);
	block_api_os_change_size(dev, false);
}

int nvmeibc_get_chunk_ind_of_lba(u64 lba, const struct nvmeibc_topology *t)
{
	const int nchunks = __get_topo_num_chunks(t);
	const struct nvmeibc_chunk *c = t->chunks;
	int i = 0, bit;
	for (bit = t->highest_pow2_in_chunk_ind; bit; bit >>= 1) {	// Binary search implementation, constructing 'i' from highest bit to lowest
		const int next_i = i|bit;
		if ((next_i < nchunks) && (c[next_i].first_vlba <= lba)) {
			i = next_i;											// turn on this bit
			if (lba < c[i+1].first_vlba)
				break;
		}
	}
	return i;
}

/******************************* resize API ***********************************/
void nvmeibc_topology_resize_clear(struct nvmeibc_topology_resize *r)
{
	memset(r, 0, sizeof(*r));
}

/******************************************************************************/
#define NVMEIBC_TOPOLOGY_BEING_FREED	(33)		// Todo: Remove, this is used only in dead VV code
bool nvmeibc_topology_is_being_freed_ID(struct nvmeibc_topology *t)
{
	return (t->phased_out>=NVMEIBC_TOPOLOGY_BEING_FREED);
}

/* Get the total number of users of this topology */
static inline int nvmeibc_topo_get_topo_users(const struct nvmeibc_topology	*t)
{
	int	i, sum = 0;
	for_each_allocated_cpu(i)
		sum += t->percpu[i].t_users;
	return sum;
}

/* Release topology memory. If is_last_topo==true force
   release the shared memory of all segments*/
static struct nvmeibc_topology *__free_topology(struct nvmeibc_topology *t,
	bool is_last_topo)
{
	struct nvmeibc_topology *tnewer = NULL;
	int c, r, si;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;

	NFIN;
	if (unlikely(!t))
		goto out;
	_NI_TOPO(trace_topology_free_topology, t, "- (@TOPOLOGY)", t);
	on_topo_free_send_shceduled(t);
	tnewer = t->newer;

	if (t->percpu) {
		topo_kfree(t->percpu);
		t->percpu = NULL;
	}
	if (!t->chunks)
		goto out;
	WARN_TOPO(chunk_has_addition(t), t, "releasing a topology with pending resize!\n"); // Removal is OK in warm, it is only a marker

	topo_for_each_chunk(t, chunk, c){
		chunk_for_each_raid1(chunk, r1, r){
			raid1_for_each_seg(r1, seg, si){
				if (is_last_topo)
					seg->shared_own = SEG_SHARED_MEMO_ALL;
				__seg_free(seg);
			}
			topo_kfree(r1->segments);
			nvmeibc_raid_topo_persistent_put_ref(r1->hdr);
		}
		topo_kfree(chunk->raid1s);
	}
	topo_kfree(t->chunks);

	if (tnewer && (tnewer->io_perm == NVMEIB_IO_TYPE_PERMIT_NONE_SUS) &&
		tnewer->nt->on_suspend_finish_cb) { /* Schedule this call on main wq*/
		struct nvmeibc_topologies *nt = tnewer->nt;
		const struct nvmeibc_block_device *dev = nt->nd;
		blk2blk_gen_work_t cb = nt->on_suspend_finish_cb;
		void *param = nt->susped_context;
		nt->on_suspend_finish_cb = NULL; /* Prevent multiple callbacks*/
		nt->susped_context = NULL;
		nvmeibc_block_set_generic_work_to_self(nvmeibc_cinst_get_blok_p(dev), dev->uuid, cb, param, false);
	}

out:
	topo_kfree(t);
	NFOUT;
	return tnewer;
}

/* Mutual exclusion mechanism between updating list of topos (inserting head)
	and getting/putting reference to the head topo of the list. This is a bit
	like multiple readers single writer lock. Multiple CPU's can take/put ref
	to the head topology (readers) but only 1 CPU can insert new head (writer) :
	1. READ-LOCK: get_topo_noupdate() .. Critical Section .. put_topo_noupdate()
		Enusres During critical section:
		1. New head will not be inserted into the list, thus you can take ref
		to head and be sure that your ref is indeed to the latest topology.
		2. If you hold ref to topo, this topo will not become phased_out so
		   so when refcount on topologies reaches zero readers knows whether
		   to free topo or not without any race conditions.
	2. WRITER-READER-LOCK: (Prevents readers from entering critical section)
		topos_list_update_begin() .. Critical Section .. topos_list_update_end()
		Ensure that in critical section you can list_add() the new head, and set
		ex-head as phased out. This is writers lock
	3. WRITER-WRITER-LOCK: Writers mutual exclusion is implemented using
		spin_lock that wraps writers critical section and much more code.
		because writers not only exclude critical sections but also must prevent
		the list from forking (becoming a tree) so writers lock protects the
		entire area of of generating a new topo from previous and insertion to
		list.
	Important: Readers lock must be implemented without asm "lock;" operations
	(no spinlocks, no atomics, etc) because they are in data path. Writer lock
	can use spinlock because it is not in data path (communication with toma)
	Implementation:
	1. Reader lock: get_topo_noupdate() - Wait until nt->need_update==false,
		do nt->nt_users++ (per_cpu). Each CPU marks the amount of ref's it
		holds to the head topology.
	2. Writer lock: topos_list_update_begin() - make sure we hold nt->lock
		spinlock to prevent other writers. Set nt->need_update==true and
		wait for nt_users of all cpu's to drop to 0 (drain readers) */
static void get_topo_noupdate(struct nvmeibc_topologies *nt)
{
	unsigned long flags;
	struct topos_percpu *tp;
	bool done = false;
	int count = 0;

	/* TODO: limit this to some reasonable period of time using jiffies. */
	while (!done) {
		while (atomic_read(&nt->need_update)) {
			if (++count == 100000) {
				_NT_SCOPE(trace_topology_get_topo_noupdate, topology, "Seems to be stuck, count=@COUNT", count);
				count = 0;
				WARN_ONCE(1, "#NVMESH-525: Apparently stuck waiting for topology read access\n");
			}
			udelay(10);
		}

		local_irq_save(flags);
		tp = &nt->percpu[get_cpu()];

		atomic_inc_per_cpu_volatile_int(tp->nt_users);
		mb();	// Force writer (which may turn need_update to true) to see our counter before we test need_update flag
		/* Alternatively to above can use boolean: tp->nt_users = 1 */
		tp->flags = flags;

		/* Prevent race condition error, write already started and draining readers, quit */
		if (atomic_read(&nt->need_update)) {
			atomic_dec_per_cpu_volatile_int(tp->nt_users);
			local_irq_restore(flags);
		} else {
			done = true;
		}
		put_cpu();
	}
}

static void put_topo_noupdate(struct nvmeibc_topologies *nt)
{
	struct topos_percpu *tp = &nt->percpu[get_cpu()];
	atomic_dec_per_cpu_volatile_int(tp->nt_users);
	/* Alternatively to above can use boolean: tp->nt_users = 0 */
	local_irq_restore(tp->flags);
	put_cpu();
}

static void inc_topo_cntr(struct nvmeibc_topology *t)
{
	if (t) {
		volatile int *p = &(t->percpu[get_cpu()].t_users);
		atomic_inc_per_cpu_volatile_int(*p);
		put_cpu();
	}
}

/* check whether the given topology can be deleted (i.e.: no users)
   NOTE: t->delete_lock must be taken, phase_out member must be checked !!! */
static bool topo_is_del_allowed(struct nvmeibc_topology *t)
{
	const int sum = nvmeibc_topo_get_topo_users(t);
	_ND_TOPO(trace_topology_topo_is_del_allowed, t, "sum=@SUM", sum);
	WARN_TOPO(sum < 0, t, "io can stuck. sum=%d\n", sum);
	return (sum == 0);
}

static struct nvmeibc_topology *dec_topo_cntr(struct nvmeibc_topology *t)
{
	volatile int *p;
	unsigned long flags = 0;
	struct nvmeibc_topology *tret = NULL;
	struct nvmeibc_topologies *nt = t->nt;
	bool phased_out;
	NFIN;
	get_topo_noupdate(nt); /* t->phased_out cannot change while this function
	runs, coz we are in critical section. Writer which sets it to true,
	guarantees at least one more dec_topo_cntr() call that will free the topo */
	phased_out = t->phased_out;
	if (phased_out)
		spin_lock_irqsave(&t->delete_lock, flags);

	p = &(t->percpu[get_cpu()].t_users);
	atomic_dec_per_cpu_volatile_int(*p);	// No mb() in good path, as if not phased_out no one is doing the summation. If phased out then spinlock is used
	put_cpu();
	if (phased_out) {
		/* If there are no more users, topology can be released. */
		const bool going_to_delete = topo_is_del_allowed(t);
		if (going_to_delete){
			BUG_ON(t->phased_out>=NVMEIBC_TOPOLOGY_BEING_FREED);		// Todo: Remove, this is used only in dead VV code
			t->phased_out = NVMEIBC_TOPOLOGY_BEING_FREED;
		}
		spin_unlock_irqrestore(&t->delete_lock, flags);
		if (going_to_delete) {
			const u64 last_freed_version = t->debug_unique_index;
			list_del(&t->list_n);
			put_topo_noupdate(nt);
			#ifdef DEBUG_TOPO_DELAY_FREE_VERY_DANGEROUS
				if (sum>0)
					msleep(get_random_u32()&0x3);/* Easier reproduction of Toma protocol bugs */
			#endif
			tret = __free_topology(t, false);
			nt->topo_debug_last_freed_version = last_freed_version;	// when testing/debuging the system, once we see the last unique version we know that it was already freed !!!
			goto _out;
		}
	}
	put_topo_noupdate(nt);	// Topo's list was not changed
_out:
	NFOUT;
	return tret;
}

struct nvmeibc_topology *nvmeibc_topology_get(struct nvmeibc_topologies *nt)
{
	struct nvmeibc_topology *t;
	get_topo_noupdate(nt);
	if (likely(nt->boot_state != NVMEIBC_TOPO_BOOT_NO_GET_TOPO) && (nt->boot_state != NVMEIBC_TOPO_BOOT_LIST_CLEANUP)) {
		t = list_first_entry_or_null(&nt->topologies, struct nvmeibc_topology, list_n);
		inc_topo_cntr(t);
	} else {
		_NT_SCOPE(warn_nvmeibc_topology_no_topo_to_get, topology, DMESG_PREFIX("@DEV_NAME: ") "error no topology for IO", nt->device_name);
		t = NULL;
	}
	put_topo_noupdate(nt);
	return t;
}

static struct nvmeibc_topology *__nvmeibc_topology_get_regardless_boot_state(struct nvmeibc_topologies *nt)
{
	struct nvmeibc_topology *t;
	get_topo_noupdate(nt);

	t = list_first_entry_or_null(&nt->topologies, struct nvmeibc_topology, list_n);
	inc_topo_cntr(t);

	put_topo_noupdate(nt);
	return t;
}

void nvmeibc_topology_put(struct nvmeibc_topology *t)
{
	while (t) {
		t = dec_topo_cntr(t); /* 't' may be freed inside dec_topo_ctr */
	}
}

struct nvmeibc_topology *nvmeibc_topology_addref(struct nvmeibc_topology *t)
{
	NFIN;
	get_topo_noupdate(t->nt);
	inc_topo_cntr(t);	// If t->phased_out no need to get_noupdate/put_noupdate
	put_topo_noupdate(t->nt);
	NFOUT;
	return t;
}

u64 topo_get_cpu_io_completed(const struct nvmeibc_topologies *nt, int cpu_id) {
	return *((volatile u64*)(&nt->percore_shared[cpu_id].ios_completed));
}

u64 topo_get_cpu_ios(const struct nvmeibc_topologies *nt, int cpu_id) {
	u64 issued = nt->percpu[cpu_id].ios_issued; 			// Cannot rise because current cpu is in this function, so cannot accept new io's
	u64 completed = topo_get_cpu_io_completed(nt, cpu_id);	// Can    rise when other cpu's complete io's that originated in this cpu.
	return (issued - completed); // While we return, completed can be increased so we get an over estimation
}

bool nvmeibc_topology_is_reconfiguring_now(const struct nvmeibc_topology *t)
{
	return (t)&&((t->num_reconfing_segs>0) || (t->resize.size!=0));
}

/* Unsafe function. copy old_t->chunks[c] into dst_chunk. We assume both chunks
 have identical structure (stripe_width) and also not all the pointers of dst
 can be updated. dst becomes a broken chunk! */
static int deep_copy_topo_chunk_unsafe(struct nvmeibc_chunk* dst_chunk, const struct nvmeibc_topology *old_t, int c)
{
	int r, s, rv = -ENOMEM, num_r1s = __get_num_r1s(old_t);
	const struct nvmeibc_chunk* src_chunk = &old_t->chunks[c];
	*dst_chunk = *src_chunk;
	dst_chunk->topology = NULL;
	if (!src_chunk->raid1s) {
		rv = 0; /* dummy chunk */
		goto _out;
	}

	dst_chunk->raid1s = topo_kzalloc(sizeof(struct nvmeibc_raid1) * num_r1s, GFP_ATOMIC);
	if (!dst_chunk->raid1s)
		goto _out;
	for (r = 0; r<num_r1s; r++) {
		struct nvmeibc_raid1* dst_r1 = &dst_chunk->raid1s[r];
		*dst_r1 = *__get_r1_by_t(old_t, c, r);
		nvmeibc_raid_topo_persistent_add_ref(dst_r1->hdr);
		dst_r1->segments = topo_kzalloc(sizeof(struct nvmeibc_disk_segment) * dst_r1->replicas, GFP_ATOMIC);
		if (!dst_r1->segments)
			goto _out;
		for (s = 0; s < dst_r1->replicas; s++) {
			const struct nvmeibc_disk_segment *src = __get_seg_by_t(old_t, c, r, s);
			if (__seg_deep_copy(&dst_r1->segments[s], dst_chunk, src) < 0)
				goto _out;
		}
	}
	rv = 0;
_out:
	return rv;
}

/* Copy a single chunk 'c' from old to new topology. Topo's must be valid */
static int deep_copy_topo_chunk(struct nvmeibc_topology *new_t, const struct nvmeibc_topology *old_t, int c)
{
    struct nvmeibc_chunk* dst_chunk = &new_t->chunks[c];
	int rv = deep_copy_topo_chunk_unsafe(dst_chunk, old_t, c);
	dst_chunk->topology = new_t;
	return rv;
}

static inline struct nvmeibc_topo_percpu* nvmeibc_topo_percpu_create(void)
{
	struct nvmeibc_topo_percpu *tpcpu;
	tpcpu = topo_kzalloc(round_up(sizeof(*tpcpu), cache_line_size()) * MAX_NUM_ACTIVE_CPUS, GFP_ATOMIC);	// Todo, use a better way to allocate percpu instead of paddinf
	if (tpcpu) {
		int n;
		for_each_allocated_cpu(n) {
			spin_lock_init(&tpcpu[n].lock);
		}
	}
	return tpcpu;
}

static struct nvmeibc_topology *deep_copy_topology(struct nvmeibc_topology *t)
{
	struct nvmeibc_topology *t1;
	int c;

	NFIN;

	t1 = topo_kzalloc(sizeof(*t), GFP_ATOMIC);
	if (!t1) {
		_NE_TOPO(error_topology_deep_copy_topology, t, "No memory for a dup topology");
		goto _out;
	}
	INIT_LIST_HEAD(&t1->list_n);
	spin_lock_init(&t1->delete_lock);
#ifdef DEBUG_TOPO_CNTRS
	INIT_LIST_HEAD(&t1->dbg_tcntrs);
	spin_lock_init(&t1->dbg_tcntrs_lck);
#endif
	t1->configuration_version = t->configuration_version;
	t1->topology_version = t->topology_version; /* Defined by TOMA not here. */
	nvmeibc_topo_init_io_perm(t1);
	t1->nt = t->nt;
	t1->debug_unique_index = (++t1->nt->topo_debug_unique_index_generator);
	t1->nchunks = t->nchunks;
	t1->highest_pow2_in_chunk_ind = t->highest_pow2_in_chunk_ind;
	t1->stripe_width = t->stripe_width;
	t1->num_reconfing_segs = t->num_reconfing_segs;
	t1->resize	= t->resize;	// Don't deep copy the whole struct on purpose
	nvmeibc_topology_resize_clear(&t->resize);	// Moved ownership of resize to 't1'

	t1->chunks = topo_kzalloc(sizeof(struct nvmeibc_chunk) * t->nchunks, GFP_ATOMIC);
	if (!t1->chunks){
		t1->nchunks = 0;
		goto nomem;
	}

	t1->percpu = nvmeibc_topo_percpu_create();
	if (!t1->percpu) {
		_NE_TOPO(error_1_topology_deep_copy_topology, t, "No memory for percpu for topology copy");
		goto nomem;
	}
	for (c = 0; c < t1->nchunks; ++c) { /* Copy including dummy chunk */
		if (deep_copy_topo_chunk(t1, t, c))
			goto nomem;
	}

	goto _out;
nomem:
	_NE_TOPO(error_2_topology_deep_copy_topology, t, "No memory for topology copy");
	__free_topology(t1, false);
	t1 = 0;

_out:
	NFOUT;
	return t1;
}

/* Performs a deep copy of a topology and makes the new one be dependent on the previous one*/
static struct nvmeibc_topology *dup_topology(struct nvmeibc_topology *t)
{
	struct nvmeibc_topology *tnew = deep_copy_topology(t);
	if (likely(tnew)) {
		t->newer = tnew;
		inc_topo_cntr(tnew);
		_NI_TOPO(trace_topology_dup_topology, tnew, "+ (@TOPOLOGY) <- @TOPO_DBG_ID vol_short_id(minor)=@VOL_ID", tnew, t->debug_unique_index, nvmeibc_volume_short_id(t->nt->nd));
	}
	return tnew;
}

/* wait until all users of the topologies are out */
static void topo_drain_users(struct nvmeibc_topologies *nt)
{
	int i, count;
	bool done;

	for (done=false, count=0; !done;) {
		done = true;
		for_each_allocated_cpu(i) {
			const int n_readers = nt->percpu[i].nt_users;
			if (n_readers) {
				if (++count == 100000) {
					_NT_SCOPE(trace_topology_topo_drain_users, topology, "@DEV_NAME: Seems to be stuck, @CPU.nt_users=@TOPO_USERS count=@COUNT", nt->device_name, i, n_readers, count);
					count = 0;
					WARN_ON_ONCE(1);
				}
				/* n_readers for each cpu go 0->1, 1->0 in readers critical
					section. Any other value is illegal */
				WARN((n_readers&(~1)), "nvmeibc bug, reader on cpu=%d interrupted=%d", i, n_readers);
				done = false;
			}
		}
		if (!done) {
			udelay(10);
		}
	}
}

/* Start critical section of manipulating the head of topologies list.
   Daniel Todo: EXC-1230, move rw-lock mechanism to nvmeibc_common or use kernel
   rwlock_t instead */
static void topos_list_update_begin(struct nvmeibc_topologies *nt)
{
	BUG_ON(!spin_is_locked(&nt->lock));	// Not preventing other writers!
	atomic_set(&nt->need_update, 1);	// Starting to drain
	/* Important: nt->need_update = true; barrier(); is not good enough because
		barrier does not exist in the other function. */
	/* Now wait until none is getting their topology right now */
	mb();
	topo_drain_users(nt);
}
static inline void topos_list_update_end(struct nvmeibc_topologies *nt)
{
	atomic_set(&nt->need_update, 0);
}

static inline void __debug_simulate_io_aband_resource(__attribute__ ((unused)) struct nvmeibc_topology *t)
{
	#ifdef DEBUG_LOSER_CONDITIONS
	if (nvmeibc_topo_is_partially_ok(t)) {
		int c, r, si;
		struct nvmeibc_chunk *chunk;
		struct nvmeibc_raid1 *r1;
		struct nvmeibc_disk_segment *seg;
		topo_for_each_seg(t, chunk, c, r1, r, seg, si) {
			if (seg->toma_acm != NVMEIBTC_DS_MODE_DEAD) {
				/* Could have IO on this seg so simulate abandons */
				nvmeibc_b_cp_loser_aband_dummy(&r1->hdr->loser, seg->toma_reg, 0);
			}
		}
	}
	#endif
}

#include "controlpath/nvmeibc_b_cp_topo_algorithms.c"
// When activating a new topology we might have a new segment that was replaced or added
// after block device init, so in that case we initialize the profiler before activiating the topology
static void __verify_all_profilers_are_initialized(struct nvmeibc_topology *t)
{
#if defined(BLKDEV_PROFILING)
	struct nvmeibc_block_device *nd = t->nt->nd;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	int c, r, si;
	topo_for_each_raid1(t, chunk, c, r1, r) {
		struct nvmeibc_disk_segment *seg;
		raid1_for_each_seg(r1, seg, si) {
			if (!nvmeibc_profiling_is_initialized(seg->disk_operation_profiler)) {
				nvmeibc_seg_topo_persistent_init_profilers(seg, nd, c, r, si);
			} else { /*nvmeibc_seg_topo_persisten_reinit_profilers(seg); */}
		}
		if (!nvmeibc_profiling_is_initialized(r1->hdr->good_path_profile[0])) { /* FIXME */
			nvmeibc_raid_topo_persistent_init_profilers(r1->hdr, nd, c, r);
		}
		if (!nvmeibc_profiling_is_initialized(r1->hdr->sync_profile)) {
			nvmeibc_raid_topo_persistent_init_profilers(r1->hdr, nd, c, r);
		}
	}
#else
	(void)t;
#endif
}

static inline void __update_topo_stats(struct nvmeibc_topology *t) {
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	int c, r;
	struct nvmeibc_topologies *nt = t->nt;
	struct topo_stats_t *ts = &nt->nd->topo_stats;
	if (nvmeibc_topo_is_partially_ok(nt)) {	// Incrementing counters only for ioable topologies.
		topo_for_each_raid1(t, chunk, c, r1, r) {
			sgmnts_bmp_t deg = nvmeibc_raid1_get_inverse_sgmnts_bmp(r1, readable);
			const int n_deg = hweight16(deg);
			if (n_deg == 1) {
				ts->n_single_deg++;
			} else if (n_deg >= 2) {
				ts->n_multiple_deg++;
			}
		}
	}
}

static inline void __set_phased_out_topo(struct nvmeibc_topology *t /* != NULL */)
{
	t->phased_out = true;
	mb(); 				// Force Future topology_put() on different CPU, to actually see this value
}

static void __set_error_state(struct nvmeibc_topology *t /* != NULL */);
static void __set_active_topology(struct nvmeibc_topologies *nt, struct nvmeibc_topology *new_t, struct nvmeibc_topology *old_t)
{
	nvmeibc_topology_fill_all_topo_raids_calculated_data(new_t); // Calculated data is recomputed before taking lock
	__update_topo_stats(new_t);
	__verify_all_profilers_are_initialized(new_t);
	topos_list_update_begin(nt);
	/* Here no other CPU executes inc_topo_cntr() nor dec_topo_cntr() via
		nvmeibc_topology_get(), put() or add_ref() */
	// Best time to initialize new segments profilers and unset in_use/stats_object after copying from older segment
	list_add(&new_t->list_n, &nt->topologies);
	__set_error_state(new_t);
	__set_phased_out_topo(old_t);
	topos_list_update_end(nt);	 	/* Crucial: must after phased_out = true. */
	__debug_simulate_io_aband_resource(new_t);	// Here: IO may start on new_t
	_NI_TOPO(trace_topology_set_active_topology, new_t, "old @TOPO_DBG_ID", old_t->debug_unique_index);
	/* Now we allow dec_topo_cntr() run, ensuring it checks phased_out flag */
}

/* 0 - OK, 1 - pausing, 2 - never discovered, 3 - paused */

__attribute__((nonnull(1)))
static int __disk_p_state2num(const struct nvmeibc_idisk *disk)
{
	return (disk->ops.should_pause(disk) ? 1 : 0) +
		   ((disk->ops.get_status(disk) == d_offline) ? 2 : 0);
}

static inline void io_perm_merge(enum nvmeib_io_type_permission *res, enum nvmeib_io_type_permission val)
{
	if (val < *res) // Take minimal permissions
		*res = val;
}

static inline bool __is_ec_journal_ok(const struct nvmeibc_disk_client_journal *jour, struct nvmeibc_block_device *nd)
{
	return (jour->rng_nlba > 0) && (jour->rng_binje == nvmeibc_cinst_get_blok_p(nd)->binje) &&
			(jour->rng_id >= NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES) && (jour->rng_id < NVMEIB_EC_MAX_JOURNAL_RANGES);
}

struct t_praid_io_disable_reason {
	u32 si;						// First problematic segment
	u32 code;					// Error code
};
#define store_seg_error_goto(jump_to, name, fmt, ...) ({ \
	_NI_TOPO(name, t, "seg=" SEGMENT_FMT ": " fmt, c, r, si, ##__VA_ARGS__); \
	if (info) { \
		info->si = (u32)si; \
		info->code = __LINE__; \
		return nvmeibc_raid1_get_io_perm_2(r1); /* When extracting reason, raid was already updated */ \
	} \
	goto jump_to; })



static inline bool __seg_has_problem(struct nvmeibc_disk_segment const *s){
	return ((bool)(s->disk->ops.should_pause(s->disk) || !s->registration_status));
}

static enum nvmeib_io_type_permission nvmeibc_raid1_calc_io_perm(const struct nvmeibc_topology *t, int c, int r, struct t_praid_io_disable_reason *info)
{
	struct nvmeibc_raid1 *r1 = __get_r1_by_t(t, c, r);
	struct nvmeibc_topologies *nt = t->nt;
	struct nvmeibc_block_device *nd = nt->nd;
	struct nvmeibc_disk_segment *seg;
	int si = 0, num_rw = hweight32(nvmeibc_raid1_get_sgmnts_bmp(r1, rw));
	bool have_journal_area = true;

	/* Step 0: Basic optional Config check and print of segs */
	raid1_for_each_seg(r1, seg, si) {
		const struct nvmeibc_idisk *disk = seg->disk;
		if (unlikely(!seg->toma_reg || !disk)) {
			store_seg_error_goto(_out_seg_err, t_01_prioperm, "tr=@TOMA_REG, disk=@DISK", seg->toma_reg, disk); /* Daniel: happens only if code has a BUG */
		}
		if (!info) {	// Dont print second time when we are just caluclating the reason
			const char *s_acm = nvmeibt_client_topo_seg_access_mode_to_str(seg->toma_acm);
			_NT_TOPO(t_02_prioperm, t, "seg=" SEGMENT_FMT " disk=@DISK_NAME acm=@ACM act=@ACT p=@RV, lid=@LID @C_PRV uuid=@SEG_DBG_UUID",
				c, r, si, disk->ops.get_full_name(disk), s_acm, seg->registration_status,
				__disk_p_state2num(disk), r1->lid.all, r1->version, seg->dbg_uuid);
		}
	}

	/* Step 1: Check global r1 topology validity */
	if (unlikely(!nvmeibc_praid_has_valid_primary_owner_lock(r1))) {	// Topology has 0 owner locks
		store_seg_error_goto(_out_topo_inv, t_03_prioperm, "No owner lock");
	}
	if (unlikely(num_rw < r1->slice_size)) {
		store_seg_error_goto(_out_topo_inv, t_04_prioperm, "Not enough RW segs");
	}
	if (unlikely(hweight32(nvmeibc_raid1_get_sgmnts_bmp(r1, dead)) == (u32)r1->replicas)) { // Dangerous toma topology bug. If all dead, no toma to request topology from so IO will stuck
		store_seg_error_goto(_out_topo_inv, t_05_prioperm, "All segments are dead. IO will stuck");
	}

	if (info) { /* Optional, Check transport layer connectivity according to topology. Covered anyways by later tests, crucial to test first if we are interested in reason */
		raid1_for_each_seg(r1, seg, si) {
			if (unlikely(seg->toma_acm != NVMEIBTC_DS_MODE_DEAD) && seg->disk->ops.should_pause(seg->disk))
				store_seg_error_goto(_out_seg_unreged, t_06_prioperm, "No connection to needed disk");
		}
	}

	if (1) {/* Step 2: Check each seg for IOability */
		bool does_lock_protect_all = true;
		raid1_for_each_seg(r1, seg, si) {
			does_lock_protect_all &= (seg->lmap.si[0] >= 0);			// The only mandatory test for dead segments
			if (seg->toma_acm == NVMEIBTC_DS_MODE_DEAD)
				continue;
			if (__seg_has_problem(seg)) {
				store_seg_error_goto(_out_seg_unreged, t_07_prioperm, "needed, not ready");
			}
			if (nvmeibc_raid_is_ec(r1)) {
				if (unlikely(seg->max_dma_size < LOCKSET_SLICES)) {			// Daniel: EC works in blocksets, todo add support to splitting commands if needed.
					store_seg_error_goto(_out_topo_inv, t_08_prioperm, "max_dma_size too short @MAX_DMA_SIZE[blks]", seg->max_dma_size);
				}
				{ // Test journal area and metadata size.
					const int md_size = nvmeibc_sgmnt_sw_md_size(seg);
					const struct nvmeibc_disk_client_journal *jour = seg->disk->ops.get_journal(seg->disk);
					if (unlikely((md_size < DISK_MIN_MD_SIZE_BYTE) || (md_size > DISK_MAX_MD_SIZE_BYTE))) {
						store_seg_error_goto(_out_topo_inv, t_09_prioperm, "Wrong metadata size @MD_SIZE[bytes]", md_size);
					}
					if (!__is_ec_journal_ok(jour, nd)) {
						_NI_TOPO(t_0a_prioperm, t, "seg=" SEGMENT_FMT " no valid journal! disk_binje=@BINJE, bdev_binje=@BINJE, jri=@JRI, blocks=@INT", c, r, si, jour->rng_binje, nvmeibc_cinst_get_blok_p(nd)->binje, jour->rng_id, jour->rng_nblk);
						have_journal_area = false;
					}
				}
			}
		}
		/* Step 3: Check praid cumulative info for IOability, Client might be registered correctly but topology itself is invalid. */
		if (unlikely(!does_lock_protect_all)) {
			store_seg_error_goto(_out_topo_inv, t_0c_prioperm, "Primary owner does not protect all locks");
		}
	}

	if (1) {/* Step 4: IO is enabled, Apply limitations and io permissions */
		enum nvmeib_io_type_permission rv = NVMEIB_IO_TYPE_PERMIT_NONE_ERR;
		if (unlikely(nvmeibc_raid_is_ec(r1))) {
			if (have_journal_area) {
				if (num_rw == r1->slice_size) {
					const bool no_protection_should_be_read_only = nvmeibc_io_perm_alert_should_create_readonly_topo(&nd->dp.io_perm_alert, jiffies);
					if (no_protection_should_be_read_only) {
						rv = NVMEIB_IO_TYPE_PERMIT_RDONLY; // No Writes'
					} else {
						rv = NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION;
					}
				} else {
					rv = NVMEIB_IO_TYPE_PERMIT_ALL;
				}
			} else {
				rv = NVMEIB_IO_TYPE_PERMIT_RDONLY; // No Writes'
			}
		} else
			rv = NVMEIB_IO_TYPE_PERMIT_ALL;
		io_perm_merge(&rv, r1->toma.io_perm); 		// Permissions cant exceed Toma instruction (each praid has verified reservation version or NVMEIB_IO_TYPE_PERMIT_NO_IO)
		return (nvmeibc_raid1_get_io_perm_2(r1) = rv);		// Cache this value for future use
	}

_out_seg_err:     return (nvmeibc_raid1_get_io_perm_2(r1) = NVMEIB_IO_TYPE_PERMIT_NONE_ERR);
_out_seg_unreged: return (nvmeibc_raid1_get_io_perm_2(r1) = NVMEIB_IO_TYPE_PERMIT_NONE_ERR);
_out_topo_inv:    return (nvmeibc_raid1_get_io_perm_2(r1) = NVMEIB_IO_TYPE_PERMIT_NONE_INV);
}

static enum nvmeib_io_type_permission __is_chunk_ioable(struct nvmeibc_topology *t, int c)
{
	struct nvmeibc_chunk *chunk = &t->chunks[c];
	struct nvmeibc_raid1 *r1;
	int r;
	enum nvmeib_io_type_permission err = NVMEIB_IO_TYPE_PERMIT_ALL;
	chunk_for_each_raid1(chunk, r1, r) {
		io_perm_merge(&err, nvmeibc_raid1_calc_io_perm(t, c, r, NULL));
		// Don't break on error coz we will miss prints of next raids
	}
	return err;
}

void nvmeibc_topologies_error_state_reason(struct nvmeibc_topologies *nt, void *ctx)
{
	const struct nvmeibc_topology *t = ctx;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct t_praid_io_disable_reason reason = {.code = 0};
	int c, r;

	nt->io_disabled_reason[0] = 0;
	topo_for_each_raid1(t, chunk, c, r1, r) {
		const struct { enum nvmeib_io_type_permission io_perm; } cur_pr = {.io_perm = nvmeibc_raid1_get_io_perm_2(r1)};
		if (nvmeibc_topo_is_partially_ok(&cur_pr))
			continue;											// Just optimization, do not scan praids which dont have any problem
		if (cur_pr.io_perm == NVMEIB_IO_TYPE_PERMIT_NO_IO) {
			scnprintf(nt->io_disabled_reason, sizeof(nt->io_disabled_reason), "raid %d,%d undergoing cold recovery (permission=%d)", c, r, r1->toma.io_perm);
			return;
		}
		nvmeibc_raid1_calc_io_perm(t, c, r, &reason);
		if (reason.code) {										// We are interested in first problematic praid
			struct nvmeibc_disk_segment const* sgmnt = ((reason.si < (u32)r1->replicas) ? &(r1->segments[reason.si]) : NULL);
			const struct nvmeibc_idisk *disk = sgmnt ? sgmnt->disk : NULL;
			scnprintf(nt->io_disabled_reason, sizeof(nt->io_disabled_reason), "segment %d,%d,%d disconnected, disk %s, error_code: %d", c, r, reason.si, (disk ? disk->ops.get_full_name(disk) : "?"), reason.code);
			return;												// First problematic praid is enough, no need to scan them all
		}
	}
}

#define __get_num_enabled_chunks(t)  (__get_topo_num_chunks(t) - __topo_num_last_chunks_disabled(t))

enum nvmeib_io_perm_class {
	IO_PERM_NO_IO = 0,
	IO_PERM_RDONLY,
	IO_PERM_ENABLED
};

static enum nvmeib_io_perm_class nvmeibc_io_perm_to_class(const enum nvmeib_io_type_permission perm)
{
	switch (perm) {
		case NVMEIB_IO_TYPE_PERMIT_NEVER:
		case NVMEIB_IO_TYPE_PERMIT_NONE_SUS:
		case NVMEIB_IO_TYPE_PERMIT_NONE_INV:
		case NVMEIB_IO_TYPE_PERMIT_NONE_ERR:
		case NVMEIB_IO_TYPE_PERMIT_NO_IO:
		case NVMEIB_IO_TYPE_PERMIT_NO_RM_IO:
			return IO_PERM_NO_IO;
		case NVMEIB_IO_TYPE_PERMIT_RDONLY:
			return IO_PERM_RDONLY;
		case NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION:
		case NVMEIB_IO_TYPE_PERMIT_ALL:
			return IO_PERM_ENABLED;
		default:
			BUG();
	}
	return IO_PERM_NO_IO; /* Not reached */
}

bool nvmeibc_has_io_permission_changed(const enum nvmeib_io_type_permission old_perm,
				       const enum nvmeib_io_type_permission new_perm)
{
	return (nvmeibc_io_perm_to_class(old_perm) != nvmeibc_io_perm_to_class(new_perm));
}

static void __set_error_state(struct nvmeibc_topology *t)
{
	struct nvmeibc_topologies *nt = t->nt;
	const struct nvmeibc_block_device *nd = nvmeibc_block_nt_to_b(nt);
	int c;
	const int n_enabl_chunks = __get_num_enabled_chunks(t);
	const int n_total_chunks = __get_topo_num_chunks(t);
	enum nvmeib_io_type_permission errE = NVMEIB_IO_TYPE_PERMIT_ALL, errD = errE;
	enum nvmeib_io_type_permission old_perm;
	NFIN;
	for (c = 0; c < n_enabl_chunks; c++)
		io_perm_merge(&errE, __is_chunk_ioable(t, c));	// Enabled chunks
	for (     ; c < n_total_chunks; c++)
		io_perm_merge(&errD, __is_chunk_ioable(t, c));	// Disabled chunks
	if (unlikely((n_enabl_chunks != n_total_chunks)&&(errE == errD))) {
		if (errE > NVMEIB_IO_TYPE_PERMIT_NONE_ERR){ /* Disabled chunks are registered, enable them all */
		} else { /* Safe to enable, because IO is stopped anyways (other chunk has error) */
		}
		nvmeibc_topo_update_size_of_bdev(t);
		t->resize.n_last_disabled_chunks = 0;
	}
	if (nvmeibc_block_is_hidden(nd))	// Hidden attach
		io_perm_merge(&errE, NVMEIB_IO_TYPE_PERMIT_NO_IO);
	if (is_suspended(*nt))
		errE = NVMEIB_IO_TYPE_PERMIT_NONE_SUS; /* Regardless of prev topo errors */
	old_perm = t->io_perm;
	t->io_perm = errE;
	if (nvmeibc_has_io_permission_changed(old_perm, t->io_perm)) {
		_NI_TOPO(trace_topology_set_error_state0, t, "@EVENT_TAG io_perm changed @STR->@STR",
			 EV_TOPOLOGY_PERMISSION_CHANGE(),
			 nvmeib_io_type_permission_to_string_short(old_perm),
			 nvmeib_io_type_permission_to_string_short(t->io_perm));
	} else {
		_NT_TOPO(trace_topology_set_error_state, t, "io_perm=@IO_PERM", t->io_perm);
	}
	nt->error_state_update_cb(nt, t->io_perm, t);
	NFOUT;
}

bool nvmeibc_topologies_are_reads_enabled(struct nvmeibc_topologies *nt) {
	unsigned long flags;
	bool rv;
	spin_lock_irqsave(&nt->lock, flags);
	rv = (nt->io_perm >= NVMEIB_IO_TYPE_PERMIT_RDONLY);
	spin_unlock_irqrestore(&nt->lock, flags);
	return rv;
}

void nvmeibc_topology_cont(struct nvmeibc_topologies *nt, 
	struct nvmeibc_idisk *disk)
{
	int c, r, si;
	struct nvmeibc_topology *t = NULL, *t1 = NULL;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&nt->lock, flags);
	if (unlikely(list_empty(&nt->topologies))) {
		_NT_SCOPE(trace_topology_nvmeibc_topology_cont, topology, "@DEV_NAME: ignore cont, has no topologies nt=@NT", nt->device_name, nt);
		goto _out;
	}
	if (__all_segments_apply_reconf(nt, true /*force*/, NULL, disk) < 0){
		goto _out;	/* Daniel: Not sure this is correct. Ignoring cont */
	}
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) goto _out;

	for (c = 0; t && (c < __get_topo_num_chunks(t)); c++) {
		for (r = 0; r < __get_num_r1s(t); r++) {
			struct nvmeibc_raid1 *r1 = &t->chunks[c].raid1s[r]; /* No cashing*/
			for (si = 0; si < r1->replicas; si++) {
				struct nvmeibc_disk_segment *seg = &r1->segments[si];
				if (seg->disk != disk)
					continue;
				t1 = dup_topology(t);
				if (!t1) {
					_NE_TOPO(error_topology_nvmeibc_topology_cont, t, "No memory for topology dup");
					goto _out;
				}
				on_topo_free_schedule_praid_ack(r1->segments[0].toma_reg, t1, t, NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT, NVMEIBT_CLIENT_RT_REASON_REG_DISK_CONT);
				__set_active_topology(nt, t1, t);
				nvmeibc_topology_put(t);
				t = nvmeibc_topology_get(nt); /* t got the head == t1*/
				r1 = &t->chunks[c].raid1s[r]; /* continue the loop on new topo*/
			}
		}
	}
	if (nvmeibc_topo_is_in_err_state(nt)&&(!t1))
		__set_error_state(t);
_out:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
	NFOUT;
}

#define __in_range(L, ME, H) 		(((ME) >= (L)) && ((ME) <= (H)))
/* Update segment with info received in toma message */
static int __toma_update_mirrored_segment(struct nvmeibc_disk_segment *seg,
	struct nvmeibt_client_topo_disk_segment *toma_si)
{
	struct nvmeibc_subscription_ctx * tr = seg->toma_reg;
	const char* toma_seg_uuid = toma_si->uuid;
	if (strncmp(toma_seg_uuid, seg->uuid, UUID_LEN)) {
		_NT_SCOPE(trace_topology_toma_update_mirrored_segment, topology, "Reconf is not supported here: @SEGMENT_UUID->@SEG, has_reconf=@BOOL_YN",
		   seg->uuid, toma_seg_uuid, seg_has_any_reconf(seg));
		seg_mark_toma_said_apply(seg);
		if (seg_has_any_reconf(seg))
			return -EAGAIN;/*Have latest configuration, Toma requested it now*/
		else
			return -EINVAL;/* Don't have the latest configuration. */
	}

	seg->toma_acm= toma_si->access_mode;
	if (!__in_range(NVMEIBTC_DS_MODE_RW  , seg->toma_acm, NVMEIBTC_DS_MODE_W_IS_DIRTY)) {
		WARN(true, "device_name=%s (chunk_idx=%d,praid_idx=%d,segment_idx=%d) acm=%d\n", tr->nt->device_name,
			 tr->ch, tr->r1, tr->seg, seg->toma_acm);
		return -EINVAL;
	}
	return 0;
}

/* Disk is inactive. Don't wait for exponential backoff. Request cont now */
#define __should_spur_disk_discovery(seg) ((seg)->disk->ops.should_pause((seg)->disk))

/* After segment was updated by toma message request registration */
static int __toma_after_update_send_seg_reg(struct nvmeibc_disk_segment *seg, struct nvmeibc_topology *old_t)
{
	NFIN;
	if (!seg->registration_status && seg->toma_acm != NVMEIBTC_DS_MODE_DEAD) {
		on_topo_free_schedule_seg_msg(seg, old_t, NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT, NVMEIBT_CLIENT_RT_REASON_REG_ON_PR_UPDATE);
		if (__should_spur_disk_discovery(seg)) {
			seg->disk->ops.call_discover(seg->disk);
		}
	} else if (seg->registration_status && (seg->toma_acm == NVMEIBTC_DS_MODE_DEAD)) {
		/* Must send unreg explicitly! (Toma is considered dead by leader but,
		   it might be alive and communicating with us).Daniel: 21/03/2017 */
		on_topo_free_schedule_seg_msg(seg, old_t, NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT, NVMEIBT_CLIENT_RT_REASON_INSTRUCTED_DEAD);
	}
	NFOUT;
	return 0;
}

static void __toma_after_update_send_raid_reg(struct nvmeibc_raid1* r1,
										 struct nvmeibc_topology *old_t)
{
	struct nvmeibc_disk_segment *seg;
	int si;
	raid1_for_each_seg(r1, seg, si)
		__toma_after_update_send_seg_reg(seg, old_t);
}

static int __toma_update_lock_id(struct nvmeibc_raid1* r1, u32 lock_id)
{
	u32 rv;
	union nvmeib_lock_id max_lock = { .lock_id_bits = ~0 };
	rv = (lock_id & (~max_lock.all));	// Find invalid bits in lock (Inverse of nvmeib_lockid_purify())
	if (rv == 0) {
		if (unlikely(lock_id == LS_UNLOCKED))
			rv = 0xDEAD;		// Toma cannot give 0 lock id
	}
	if (rv == 0) {
		r1->lid.all = lock_id;
		_NI_SCOPE(trace_topology_toma_update_lock_id, topology, "Updated c_lid=@C_LID", lock_id);
		return 0;
	} else {
		_NW_SCOPE(error_topology_toma_update_lock_id, topology, "Rejecting invalid t_lid=@T_LID (wrong bits=@RV)", lock_id, rv);
		return -EINVAL;
	}
}

static void __debug_print_updated_seg(const struct nvmeibc_disk_segment *seg)
{
	char lmstr[LOCK_OWNERSHIP_MAP_STRING_LEN];
	const struct nvmeibc_subscription_ctx *tr = seg->toma_reg;
	const char *acm = nvmeibt_client_topo_seg_access_mode_to_str(seg->toma_acm);
	lock_ownership_map_to_string(&seg->lmap, lmstr);
	_NITR(trace_topology_debug_print_updated_seg, "acm=@ACM sy=@SYNC_SAFETY lm(@STR)", acm, seg->sync_safety, lmstr);
}

static enum nvmeib_io_type_permission __parse_toma_io_perm(union io_perms_bitfield ip)
{
	if (ip.bits.is_io_W)
		return NVMEIB_IO_TYPE_PERMIT_ALL;
	_NT_SCOPE(trace_topology_parse_toma_io_perm, topology, "Toma assigned partial io_perm=@IO_PERM", ip.all);
	if (ip.bits.is_io_R)
		return NVMEIB_IO_TYPE_PERMIT_RDONLY;
	if (ip.all)
		return NVMEIB_IO_TYPE_PERMIT_NO_IO;	// Only recoveries
	WARN(true, "nvmeibc, raid wrong io_perm=0x%x\n", ip.all);
	return NVMEIB_IO_TYPE_PERMIT_ALL; //NVMEIB_IO_TYPE_PERMIT_NO_IO;		// This is a bug in Toma which always sends 0, Todo: Remove
}

static void __verify_sync_safety_validity(struct nvmeibc_disk_segment*seg)
{
	if (!__in_range(NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO, seg->sync_safety, NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_YES)) {
		const struct nvmeibc_subscription_ctx *tr = seg->toma_reg;
		WARN(true, "nvmeibc, device_name=%s (%d,%d,%d) acm=%d sy=%d! IO will be slow!\n", tr->nt->device_name, tr->ch, tr->r1, tr->seg, seg->toma_acm, seg->sync_safety);
		seg->sync_safety = NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_YES;
	}
}

// We store the max reservation_version so we can update all TOMAs with this value. (can be > nt->reservation_version)
static inline void __update_reservation_version_max_seen(struct nvmeibc_topologies *nt, const struct nvmeibt_client_msg *pl, struct nvmeibc_raid1* r1, const struct nvmeibc_subscription_ctx *tr)
{
	const u64 toma_resrv_ver = pl->thick.reservation_mode_version;
	const u64 clnt_resrv_ver = nt->reservation_version;
	nt->reservation_version_max_seen = max(nt->reservation_version_max_seen, toma_resrv_ver);

	if ((clnt_resrv_ver == RESERVATION_MODE_IRRELEVANT) || (!r1))
		return;								// Nothing to do in this case

	// Daniel: According to protocol the code below should run only in register ACK. However I put it here, just to protects against data corruption caused by possible toma bugs
	if (toma_resrv_ver > clnt_resrv_ver) {
		#define IO_PREEMPT_FMT "IO Preempted (toma_@RES_MOD_VER > clnt_@RES_MOD_VER)"
		io_perm_merge(&r1->toma.io_perm, NVMEIB_IO_TYPE_PERMIT_NO_RM_IO);
		if (tr)		// If preempt is a direct result of toma meesage on 'tr' then also log it
			_NTTR(t_rvms_0, ": " IO_PREEMPT_FMT, toma_resrv_ver, clnt_resrv_ver);
		else		// This is not mandatory. Anyaways register ACK will print it
			_NT_SCOPE(t_rvms_1, topology, "@DEV_NAME  tr==NULL"   ": " IO_PREEMPT_FMT, nt->device_name,                          toma_resrv_ver, clnt_resrv_ver);
	} else if (toma_resrv_ver < clnt_resrv_ver) {
		const bool is_register_ack_msg = (tr != NULL);		// Should be changed to 'true' when function is called only for register ack
		if (is_register_ack_msg) {
			// EC-5372, Todo: This can happen when volume goes from hidden to visible, todo, solve this correctly, and reenable the warn-on
			//WARN(true, "Bug in Toma: allows IO with wrong RMV. Preempting self, toma_RMV=0x%llx != clnt_RMV=0x%llx\n", toma_resrv_ver, clnt_resrv_ver);	// Trap to catch illegal toma register ack without saving the reserversion version to persistency.			io_perm_merge(&r1->toma.io_perm, NVMEIB_IO_TYPE_PERMIT_NO_RM_IO);
		}
	} else {		// Reserverion versions match
		WARN((r1->toma.io_perm == NVMEIB_IO_TYPE_PERMIT_NO_RM_IO),"Bug in Toma: Cannot un-preempt clnt, toma_RMV=0x%llx != clnt_RMV=0x%llx\n", toma_resrv_ver, clnt_resrv_ver);	// Trap to catch illegal transitions when reservation mode decreases. Should Never happen (clnt == toma after toma > clnt) if we are preempted it should be forever, until detach.
	}
}

static inline int __naive_select_worst_error(int err1, int err2){
	return min(err1, err2);
}

/* Upon arrival of toma message with a newer topology, update the entire raid
   and imemdiately request register on the needed segments */
static int __toma_update_raid1(struct nvmeibc_raid1* r1, struct nvmeibt_client_msg *pl,
						struct nvmeibc_topology *old_t)
{
	struct nvmeibc_disk_segment            *seg       = r1->segments;
	struct nvmeibt_client_topo_praid       *raid1_info= (void*)&pl->thick.data;
	struct nvmeibt_client_topo_disk_segment*seg_infos = (void*)raid1_info->segs;
	const int tomas_replicas = raid1_info->n_segments;
	int si, rv = 0;
	NFIN;

	__nvmeibc_raid1_clear_old_topo_1st_tr(old_t, r1);	// We dont care if this is switch topo or nack. Sane behavior
	if (r1->replicas != tomas_replicas) {
		/* Toma probably holds a newer configuration of up/downgraded volume*/
		_NI_TOPO(error_topology_toma_update_raid1, old_t, "error: nsegs clnt(@N_SEGMENTS)!=toma(@N_SEGMENTS)", r1->replicas, tomas_replicas);
		rv = -EAGAIN;
		goto out;
	}
	if (1) {		/* Verify volume version */
		const int clnt_ver = (int)__get_topo_of_r1(r1)->configuration_version;
		const int toma_ver = (int)pl->hdr.volume_config_version;
		WARN_TOPO(toma_ver <= 0, old_t, "wrong toma version = %d\n", toma_ver);
		if (clnt_ver != toma_ver) {
			_NI_TOPO(trace_topology_toma_update_raid1, old_t, "Volume versions: c_@C_VOL_VER != t_@C_VOL_VER", clnt_ver, toma_ver);
			/* Daniel: Try to update protection raid, because, likely, volume
			   change did not affect this raid1, so IO can still be enabled */
			/*rv = -EAGAIN; goto out;*/
		}
	}

	BUG_ON(r1->lid.all == LS_UNLOCKED);
	r1->version = pl->thick.praid_version;
	r1->toma.topo_checksum = raid1_info->topo_checksum;
	r1->toma.io_perm =__parse_toma_io_perm((union io_perms_bitfield)raid1_info->io_perms);
	__update_reservation_version_max_seen(old_t->nt, pl, r1, NULL);

	raid1_for_each_seg(r1, seg, si){	// First, update segment data
		int rv_sgmnt = 0;
		seg->sync_safety = raid1_info->blkset_sync_safety;
		__verify_sync_safety_validity(seg);
		rv_sgmnt = __toma_update_mirrored_segment(seg, &seg_infos[si]);
		rv = __naive_select_worst_error(rv, rv_sgmnt);
	}
	if (unlikely(rv < 0))
	   goto out;

	raid1_for_each_seg(r1, seg, si){	// Second, update lock ownership
		lock_ownership_update_seg_map(r1, si, &seg_infos[si]);
		__debug_print_updated_seg(seg); // Daniel: Enabled to debug N-replicas
	}
	/* Here 'rv==0' so update was successfull */
	__toma_after_update_send_raid_reg(r1, old_t);
out:
	NFOUT;
	return rv;
}

/* Use when we cannot update raid1, since the topology is illegal (client or
   toma have outdated configuration). We cannot do IO. Unregister from
   registered segments and poison raid1, to force NACK in the future */
static void __poison_raid1_by_seg(const struct nvmeibc_subscription_ctx *tr,
	struct nvmeibc_topology *t_new, struct nvmeibc_topology *t_old)
{
	int si;
	struct nvmeibc_disk_segment *seg;
	struct nvmeibc_raid1 *r1;

	on_topo_free_schedule_praid_ack(tr, t_new, t_old, NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT, NVMEIBT_CLIENT_RT_REASON_POISON_RAID);
	r1 = __get_r1_by_tr(t_new, tr); // Note: nvmeibc_raid1_clear..() was already called on new and old topos. Now Posion segs of new_topo
	raid1_for_each_seg(r1, seg, si) {
		seg->toma_acm =    NVMEIBTC_DS_MODE_INVALID;
		seg->sync_safety = NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_UNKNOWN;
		lock_ownership_map_poison(&seg->lmap);
	}
	r1->toma.conversation_ind++;				// Todo: Disrespect previous REG-ACK messages
	_ND_TOPO(trace_topology_poison_raid1_by_seg, t_new, SEGMENT_FMT ": Poison, conv=@CLNT_TOMA_PR_CONVER_IND", tr->ch, tr->r1, tr->seg, r1->toma.conversation_ind);
}

// Common TOMA message handling [double-]checks after tr->nt->lock is locked
#define __if_cant_handle_goto(name, tr, jump_to) ({ \
	if (is_toma_reg_already_dead(tr)) { \
		_NWTR(name##_1, "Recovering from EXC-2433 bug"); /*WARN_ON(true);*/ \
		goto jump_to; \
	} \
	if (nvmeibc_block_nt_to_b(tr->nt)->ignore_all_toma_msgs) { \
		_NTTR(name##_2, "Ignoring toma message"); \
		goto jump_to; \
	}})


static void __do_on_toma_not_ready(struct nvmeibc_subscription_ctx* tr, struct nvmeibt_client_msg *pl)
{
	unsigned long flags;
	struct nvmeibc_disk_segment *seg;
	struct nvmeibc_topology *t = NULL;
	struct nvmeibc_topologies *nt = tr->nt;

	spin_lock_irqsave(&nt->lock, flags);
	__if_cant_handle_goto(t_01_2433, tr, ignore_msg);
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) goto ignore_msg;
	seg = __get_seg_by_tr(t, tr);
	if (pl->hdr.reason == NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH) {

		if (pl->hdr.protocol_version != NVMEIBT_CLIENT_PROTO_VERSION_PREV && pl->hdr.protocol_version != NVMEIBT_CLIENT_PROTO_VERSION) {
			_NWTR(w_on_toma_not_ready_unsupported_version, "Unsupported TOMA protocol version=@TOMA_CLIENT_PROTOCOL_VERSION, ignoring message",
					pl->hdr.protocol_version);
			goto ignore_msg;
		}

		if (seg->registration_status) {
			// Todo: Here, analyze which message was not understand, currently
			// Dont care: Todo, what if decentralized unreg message was lost.
			goto ignore_msg;
		}

		// Downgrade from NVMEIBT_CLIENT_PROTO_VERSION or upgrade back from NVMEIBT_CLIENT_PROTO_VERSION_PREV
		tr->protocol_version = pl->hdr.protocol_version;
		__segment_register(seg);

	} else if (pl->hdr.reason == NVMEIBT_CLIENT_TR_REASON_LOCKID_MESS) {
		/* Very Bad, What todo here??? */
		_NTTR(trace_topology_do_on_toma_not_ready, "got LOCKID_MESS");
	}
ignore_msg:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
}

#define _NT_IR(name, format, ...) _NT_SCOPE(name, topology, "Ignoring REG_ACK, @T_PRV, @C_PRV, @T_LID, @C_LID, " format "", toma_raid_version, r1->version, pl_lock_id, r1->lid.all, ##__VA_ARGS__)

static int __toma_segment_register_succeed(struct nvmeibc_subscription_ctx *tr, struct nvmeibt_client_msg *pl, int len)
{
	unsigned long flags;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	struct nvmeibc_topology *t = NULL, *t1;
	struct nvmeibc_topologies *nt = tr->nt;
	const int toma_raid_version = pl->thick.praid_version;
	const u32 pl_lock_id = pl->thick.lock_id;
	int oa_ver;		/* On segment activation version */
	int client_toma_out_of_sync = false;	// Type of critical error

#define goto_ignore_msg_treat_errors ({ \
	if (unlikely(client_toma_out_of_sync)) goto input_checks_done; \
	else                                   goto ignore_msg; })

	BUILD_BUG_ON(sizeof(union jblock_md) != sizeof(union nvmeibc_block_dp_ec_data_block_md));
	BUILD_BUG_ON(sizeof(union nvmeibc_block_dp_ec_data_block_md) < NVMEIBC_SGMNT_DEFAULT_MD_SIZE);

	NFIN;
	(void)len;		/* Todo: Verify mesage length */
	spin_lock_irqsave(&nt->lock, flags);			// I must lock the spinlock before the get_topology to ensure that I am working against the latest and to serialize the updates
	__if_cant_handle_goto(t_02_2433, tr, ignore_msg);
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) goto ignore_msg;
	r1  = __get_r1_by_tr( t, tr);
	seg = __get_seg_by_tr(t, tr);
	oa_ver = nvmeibc_seg_on_active_get_version_calc(seg);

	if (seg->disk->ops.should_pause(seg->disk)) {
		_NT_IR(tr_07_topo_seg_reg_ack, "disk pause");
		goto ignore_msg;	/* Valid Transport layer race: msg slipped in after pause. toma thinks that we are unregistered. Illegal to accept this msg */
	}

	if (r1->toma.conversation_ind != pl->thick.conversation_ind) {
		const u64 c_conv = r1->toma.conversation_ind, t_conv = pl->thick.conversation_ind;
		_NTTR(tr_00_topo_seg_reg_ack, "Ignoring REGISTER_SUCCEED, @T_PRV, @C_PRV, t_cnv=@CLNT_TOMA_PR_CONVER_IND != c_cnv=@CLNT_TOMA_PR_CONVER_IND", toma_raid_version, r1->version, t_conv, c_conv);
		if (t_conv == 0ULL) {			// Illegal value. Bug in client-toma protocol. toma does not respond properly to conversation id
			_NTTR(tr_01_topo_seg_reg_ack, "Bug in the TOMA protocol! seg=@SEG c_lid=@C_LID t_conv=0x0!", seg->uuid, r1->lid.all);
		}
		if (c_conv < t_conv) {
			// WTF? Toma invented conversation ID?
		} else {
			// Discard old reg-ack, we already unregistered and are waiting for a new ack
		}
		goto ignore_msg;	/* Always ignore this message */

	}

	if (seg->registration_status) {
		if (toma_raid_version < r1->version) {			// Old register ack.
		} else if (toma_raid_version > r1->version){
			/* What??? We should have got a NACK before and update version*/
			client_toma_out_of_sync = 1;
		} else {							// Double ack
		}
		_NT_IR(tr_02_topo_seg_reg_ack, "double");
		goto_ignore_msg_treat_errors;	/* Always ignore this message */
	}

	if (toma_raid_version != r1->version){
		if (oa_ver == 0) { /* Indeed versions do not match, no on_active */
			if (toma_raid_version < r1->version) { /* Sent REG, got NACK, Send REG again
					with updated pr, but got ACK on prev REG */
				if (pl_lock_id == r1->lid.all) {	// BUG EC-2823 solved by this clause
					/* Previous and cur REG's were on different sequentinal
						topos, like: {RW,W} and {RW,RW}, this is very bad.
						Toma thinks we are registered to old topo, but we dont
						even know it, we might prevent ourselves from doing io
						becasue toma will give TOMA_NOT_READY (AWAITING_CLIENTS_SYNC)*/
					_NT_IR(trace_03_topology_toma_segment_register_succeed, "self blocking");
					client_toma_out_of_sync = 6;
				} else {
					/* Can safely ignore coz already sent new REQ request
						to new proto with new lock-id */
					_NT_IR(trace_04_topology_toma_segment_register_succeed, "Old");
				}
			} else { // (toma_raid_version > r1->version)
				if (r1->version == 0) { /* Other seg got PAUSE/Bad lock ID
					and posioned/unregisterd r1, while the ACK for this seg was
					on the way. Can ignore coz already sent new REQ request */
					_NT_IR(trace_4_topology_toma_segment_register_succeed, "evil sibling"); 	// Todo: may cause Lock-ID mess!!!
				} else {
					_NT_IR(trace_5_topology_toma_segment_register_succeed, "New???"); /* What??? Impossible! We should've got
					NACK earlier + update version*/
					client_toma_out_of_sync = 2;
				}
			}
			goto_ignore_msg_treat_errors;
		} else if (toma_raid_version != oa_ver) { /* After updated r1, still no match */
			/* What??? We haven't updated Raid1 yet, nor sent registration
			request. How on earth did we get ACK? */
			_NT_IR(trace_6_topology_toma_segment_register_succeed, "Wierd???");
			client_toma_out_of_sync = 3;
			goto_ignore_msg_treat_errors;
		} /* else (toma_raid_version == oa_ver) is normal situation. Valid ACK*/
	}
	if (r1->lid.all != pl_lock_id) {
		_NTTR(error_1_topology_toma_segment_register_succeed, "Serious problem. REGISTER_SUCCEED, c_lid=@C_LID, t_lid=@T_LID", r1->lid.all, pl_lock_id);
		client_toma_out_of_sync = 4;
	}
	if (unlikely(pl_lock_id == LS_UNLOCKED)) {
		/* What??? Got ACK without lock-id. That's just illegal*/
		_NTTR(error_2_topology_toma_segment_register_succeed, "Bug in the TOMA protocol! seg=@SEG c_lid=@C_LID", seg->uuid, r1->lid.all);
		client_toma_out_of_sync = 5;
	}
	if (unlikely(seg->toma_acm == NVMEIBTC_DS_MODE_DEAD)) {
		bool dead_toma_reg_ack_bug;
		if (!oa_ver) {
			dead_toma_reg_ack_bug = true;	// Latest topology is wrong
		} else {
			const void* data = (void*)nvmeibc_seg_on_active_get_msg(seg)->thick.data;
			const struct nvmeibt_client_topo_praid *r1_msg = data;
			const struct nvmeibt_client_topo_disk_segment *s_msg = (void*)r1_msg->segs;
			dead_toma_reg_ack_bug = (s_msg[tr->seg].access_mode == NVMEIBTC_DS_MODE_DEAD);
		}
		if (dead_toma_reg_ack_bug)
			_NTTR(error_3_topology_toma_segment_register_succeed, "Bug in the TOMA (dead ack)! seg=@SEG c_lid=@C_LID", seg->uuid, r1->lid.all);
	}

input_checks_done:
	t1 = dup_topology(t);
	if (!t1) {
		_NE_TOPO(error_4_topology_toma_segment_register_succeed, t, "No memory for a topology dup");
		goto ignore_msg;
	}
	r1  = __get_r1_by_tr( t1, tr);
	seg = __get_seg_by_tr(t1, tr);

	if (unlikely(client_toma_out_of_sync)){
		_NI_TOPO(trace_7_topology_toma_segment_register_succeed, t, "Serious problem. REG_ACK rejected, @T_PRV @C_PRV(@OA_PRV), toma_lid=@T_LID, client_lid=@C_LID." SEGMENT_FMT ": seg=@SEG err=@RV",
				toma_raid_version, r1->version, oa_ver, pl_lock_id, r1->lid.all, tr->ch, tr->r1, tr->seg, seg->uuid, client_toma_out_of_sync);
		__poison_raid1_by_seg(tr, t1, t); /* Do not update. Just ignore this message and on active msgs*/
		__set_active_topology(nt, t1, t);
		nvmeibc_topology_put(t);
		t = nvmeibc_topology_get(nt); /* t got the head == t1*/
		WARN_ON(!t); /* Some topology must exist here */
		t1 = dup_topology(t);
		if (!t1) {
			_NE_TOPO(error_6_topology_toma_segment_register_succeed, t, "No memory for a topology dup");
			goto ignore_msg;
		}
		on_topo_free_schedule_praid_ack(tr, t1, t, NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT, NVMEIBT_CLIENT_RT_REASON_REJECT_REG_ACK);
		goto done_handling_ack;
	}

	seg->registration_status = SEG_REGSTATUS_TOMA_OK;
	seg->max_dma_size = (seg->disk->ops.get_max_request_size_bytes(seg->disk) >> NVMEIBC_SECTOR_SHIFT);
	seg->sw_md_size = nvmeibc_idisk_get_sw_md_size(seg->disk);
	if (nvmeibc_raid_is_ec(r1)) {
		if (seg->sw_md_size < NVMEIBC_SGMNT_DEFAULT_MD_SIZE) {
			WARN(true, "%s: ec cannot operate with metadata-size %u < %u. Crashing to prevent data corruption due to config error...", nt->device_name, seg->sw_md_size, (u32)NVMEIBC_SGMNT_DEFAULT_MD_SIZE);
			BUG();
		}
	}
	#ifdef DEBUG_TOPO_CNTRS
		BUG_ON(atomic_inc_return(&seg->toma_reg->n_registers)-1); /*Must==1*/
	#endif
	_NI_TOPO(trace_9_topology_toma_segment_register_succeed, t, "seg=@SEG registered with TOMA, c_lid=@C_LID, dma_size=@DMA_SIZE", seg->uuid,
	   r1->lid.all, seg->max_dma_size);
	__update_reservation_version_max_seen(nt, pl, r1, tr);
	__per_reg_init(seg, pl);
	if (oa_ver) { /* If we have scheduled acks - send them now */
		int update_rv = 0;
		_NT_TOPO(trace_10_topology_toma_segment_register_succeed, t, "Seg has on_active, @OA_PRV ? @C_PRV", oa_ver, r1->version);
		if (oa_ver > r1->version) { /* Update raid to send ACK with latest ver*/
			/* Explanation: Previously we send to toma of 'seg' register request
			   with oa_ver even though we hold it aside in on_active and not in
			   the topology. Now we got register ack, so it is time to update
			   the topology and answer with ACK on the on_active message */
			update_rv =	__toma_update_raid1(r1, nvmeibc_seg_on_active_get_msg(seg), t);
			if (update_rv<0){
				_NE_TOPO(error_8_topology_toma_segment_register_succeed, t, "Serious problem. on_active failed, rv=@RV", update_rv);
				__poison_raid1_by_seg(tr, t1, t);
			} else {
				_NT_TOPO(trace_on_active_applied, t, "Seg on_active, @OA_PRV applied", oa_ver);
				update_rv = 0; /* No error proceed to sending ACK */
			}
		} else if (oa_ver == r1->version) {
			/* raid already update without the need for on_active msg.
			Example: NACK from another toma brought same info. Do nothing */
			_NT_TOPO(trace_on_active_skipped, t, "Seg on_active, @OA_PRV skipped", oa_ver);
		} else {
			/* Only possible if TOMAs which sent us SWITCH TOPO already cut us off,
			 * and we didn't get the PAUSEs yet.
			 * No matter what we reply as it won't pass through. */
			WARN_TOPO(true, t, NVMESH_BUG_PREFIX_FMT " Seg on_active, 2 prv behind, oa_prv=0x%x c_prv=0x%x\n", 810, oa_ver, r1->version);
		}
		if (update_rv == 0) {
			nvmeibc_seg_on_active_sched_ack(tr, r1, seg, t);
		}
		nvmeibc_seg_on_active_free(seg);		// Regardless if was used or not, because it is not needed anymore
	}
done_handling_ack:
	__set_active_topology(nt, t1, t);

ignore_msg:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
	NFOUT;
	return 0;
}

/* When in degraded mode & getting new topo which returns us to full mirroring,
   do not update raid1 immediately. Now we have a valid topo (can do IO), but if
   we update it will become invalid until the new segment registers. We will not
   be able to make IO. To avoid this, First request registration of new segment,
   and only upon success, transition to the new topology (update raid1) */
static void __toma_delayed_update_topo(const struct nvmeibc_subscription_ctx *tr,
  struct nvmeibt_client_msg *pl, int len, struct nvmeibc_disk_segment *inactive)
{
	nvmeibc_seg_on_active_schedule(tr, pl, len, inactive);
	if (__should_spur_disk_discovery(inactive)) {
			inactive->disk->ops.call_discover(inactive->disk);
	} else {
		__segment_register(inactive);
	}
}

static bool __toma_topology_msg_is_valid(const struct nvmeibc_subscription_ctx *tr,
	const struct nvmeibt_client_msg *pl, int len)
{
	/* Minimal topology includes at least one disk segment*/
	int minimal_len  = sizeof(struct nvmeibt_client_msg) +
					   sizeof(struct nvmeibt_client_topo_praid) +
					   sizeof(struct nvmeibt_client_topo_disk_segment);
	const int pl_ver = pl->thick.praid_version;
	int n_segs;
	bool rv = false;
	const struct nvmeibt_client_topo_praid *raid1_info = ((const struct nvmeibt_client_topo_praid*)(pl+1));

	if (unlikely(len < minimal_len))
		goto _err;

	/*If topology includes more than 1 segments, update min length accordingly*/
	n_segs = raid1_info->n_segments;
	minimal_len += (n_segs-1) * sizeof(struct nvmeibt_client_topo_disk_segment);
	if (unlikely(len != minimal_len))
		goto _err;
	if (pl_ver==0) {
		WARN(true, "nvmeibt bug. t_prv=0"); // IO may corrupt volume data
		goto _err;	/* Deliberately ignore the message as invalid */
	}
	_NTTR(trace_01_topo_toma_msg_is_valid, "TOMA @T_PRV, checksum=@T_PRCHKSUM", pl_ver, raid1_info->topo_checksum);
	rv = true;
	goto _out;
_err:
	_NTTR(trace_topology_toma_topology_msg_is_valid, "TOMA msg invalid: buf=@BUF len=@LEN min=@MIN", pl, len, minimal_len);
	_Dbuf(pl, len);
_out:
	return rv;
}

/* If all segs in protection raid, except one,  match the state described in
	toma msg (switch topo / registrable / NACK), this one requires a special
	treatment (possibly delayed response). If such seg exists, return its index,
	otherwise return 0 */
static int __find_single_seg_diff_from_toma_msg(
	const struct nvmeibt_client_msg *pl, const struct nvmeibc_raid1 *r1)
{
	int si, rv = -1;
	const struct nvmeibt_client_topo_praid *r1_msg;
	const struct nvmeibt_client_topo_disk_segment *s_msg;
	bool msg_active_seg[N_MAX_RAID_SLICE_LEN]; // Toma says seg[i] is IOable
	bool   seg_like_msg[N_MAX_RAID_SLICE_LEN]; // Our seg[i] matches the msg
	int  num_seg_like_msg = 0;				   // Ammount of matching segs
	r1_msg = (const struct nvmeibt_client_topo_praid        *)&pl->thick.data;
	s_msg  = (const struct nvmeibt_client_topo_disk_segment *)r1_msg->segs;
	WARN_ON(r1->replicas != r1_msg->n_segments); // Daniel: Todo, handle this
	for (si = 0; si < r1_msg->n_segments; si++) {
		msg_active_seg[si] = (s_msg[si].access_mode != NVMEIBTC_DS_MODE_DEAD);
		seg_like_msg[  si] = (msg_active_seg[si] == is_seg_active(r1->segments[si]));
		num_seg_like_msg  += seg_like_msg[si];
	}
	if (num_seg_like_msg != r1->replicas-1)
		goto _out;	// Daniel: Nothing to do, no special segment

	for (si = 0; (si < r1_msg->n_segments) && (seg_like_msg[si]); si++);
	rv = (si < r1_msg->n_segments) ? si : -1;
_out:
	return rv;
}

static void __verify_toma_correct_registrable(const struct nvmeibc_subscription_ctx *tr,
											  const struct nvmeibc_topology *t)
{
	if (unlikely(!nvmeibc_topo_is_in_err_state(t))) {
		struct nvmeibc_raid1 *r1 = __get_r1_by_tr( t, tr);
		struct nvmeibc_disk_segment *me = &r1->segments[tr->seg];
		if (me->toma_acm == NVMEIBTC_DS_MODE_DEAD) {
			/* Daniel: This is possibly ok (race condition of leader putting us
			   to degraded mode and dead toma sending registrable. But high
			   probability, that this is a bug*/
			_NE_TOPO(error_topology_verify_toma_correct_registrable, t, "nvmeibc Toma possible bug! Got registrable to wrong seg");
		} else {
			if (tr->ch >= __get_num_enabled_chunks(t)) {
				/* We are extending the volume and a segment in the new chunk
				   is not registered but not needed for IO enabling */
			} else {
				/* We are data corrupting now, coz segment is needed for IO
				   but we had IO enabled without it! Crash asap! */
				BUG();
			}
		}
	}
}

/***************************** Client side Recovery ***************************/
static void __toma_purge_stalocks_cache(struct nvmeibc_subscription_ctx *tr,
					struct nvmeibt_client_msg *pl, struct nvmeibc_topology *t)
{
	struct nvmeibt_client_msg_pl msg_pl;	// response
	const struct nvmeibt_lockid_cache_purge_pl *ri = (void*) &pl->thick.data;
	      struct nvmeibt_lockid_cache_purge_pl *rs = &msg_pl.lockid_cache_purge;
	struct nvmeibc_raid1 *r1 = __get_r1_by_tr( t, tr);
	struct nvmeibc_disk_segment *seg = &r1->segments[tr->seg];
	int rv;

	stale_lock_resolver_clear_all(&r1->hdr->slr);
	*rs = *ri;
	rv = nvmeibc_toma_send_direct_msg(seg,
					NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK, &msg_pl);
	_NITR(trace_topology_toma_purge_stalocks_cache, "purged taskid=@TASKID, start=@START length=@LENGTH, rv=@RV",
		rs->purge_seqno, rs->start_counter, rs->length, rv);
}

static void __toma_handle_running_recovery(struct nvmeibc_subscription_ctx *tr, enum NVMEIBT_CLIENT_MSG_TYPES msg_type
										 ,struct nvmeibt_client_msg *pl, int len)
{
	struct nvmeibt_client_recovery_taskid_pl *ri = (void *)&pl->thick.data;
	const int expected_len  = sizeof(struct nvmeibt_client_msg) + sizeof(*ri);
	struct nvmeibc_topologies *nt = tr->nt;
	struct nvmeibc_topology *t = NULL;
	ulong flags;
	if (unlikely(expected_len != len)) {
		_NTTR(trace_topology_toma_handle_running_recovery, "TOMA msg invalid: buf=@BUF len=@LEN exp=@EXP", pl, len, expected_len);
		_Dbuf(pl, len);
		goto _out;
	}
	spin_lock_irqsave(&nt->lock, flags);
	__if_cant_handle_goto(t_03_2433, tr, ignore_msg);
	/* Hold Topo, not coz it is needed, but to serialize with detach */
	t = nvmeibc_topology_get(tr->nt);
	if (unlikely(!t))
		goto ignore_msg;

	nvmeibc_recovery_handle_request(tr, msg_type, ri);

ignore_msg:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
_out:
	return;
}

static void __attr_no_alignment_sanity __toma_recover_start(struct nvmeibc_subscription_ctx *tr,
								 struct nvmeibt_client_msg *pl, int len)
{
	struct nvmeibt_client_recovery_start_pl *ri = (void *)&pl->thick.data;
	const int expected_len  = sizeof(struct nvmeibt_client_msg) + sizeof(*ri);
	struct nvmeibc_topologies *nt = tr->nt;
	struct nvmeibc_topology *t = NULL;
	struct nvmeibc_raid1  *pr = NULL;
	ulong flags;
	/* Regarding the BUILD_BUG_ON() below, which is a result of a twisted design where Stale lock purge
	 * is not a standard recovery on the TOMA side.
	 * Stale lock purge payload can be initiated by toma message.
	 * Toma does not treat this as a recovery but the client does.
	 * So Toma does not send a typical recovery start message.
	 * But clients flow treat this as a recovery so the tests for payload validity (like payload size are unified).
	 * Regardless of the above, please dont change sizes of structs unless this is unavoidable.
	 * Use the reserved fields instead, Or use a different message type.
	 * If you do change the sizes. Please increase the client toma protocol version and make sure that backwards
	 * compatibility is supported, or else we will break the upgrade process.
	 * Please see the code of nvmeibt_client_decode() and encode, to see how backwards compatibility is achieved,
	 * by translating messages in place between different protocol versions.
	 * We did it once between 1.3 and 2.0 versions and it wasn't so nice. See function __v13_vs_v19_recov_taskid_pl() Prioir to version 3.2.0 as an example.
	 */
	BUILD_BUG_ON(sizeof(struct nvmeibt_lockid_cache_purge_pl) != sizeof(struct nvmeibt_client_recovery_start_pl));
	if (unlikely(expected_len != len)) {
		_NTTR(t_tt_recov_00, "TOMA msg invalid: buf=@BUF len=@LEN exp=@EXP", pl, len, expected_len);
		_Dbuf(pl, len);
		goto _out;
	}
	spin_lock_irqsave(&nt->lock, flags);
	__if_cant_handle_goto(t_04_2433, tr, ignore_msg);
	/* Hold Topo, not coz it is needed, but to serialize with detach */
	t = nvmeibc_topology_get(tr->nt);
	if (unlikely(!t))
		goto ignore_msg;
	if ((u32)pl->hdr.msg_type == (u32)NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE)
		goto _purge_recov;

	pr = __get_r1_by_tr(t,tr);
	if (1) {	// Check if recovery should be auto-rejected, Todo: Move to separate function
		const struct nvmeibc_disk_segment *owner_seg = &pr->segments[tr->seg];	// Only owner seg can request recovery for self
		const bool is_owner_requesting = ARE_UUIDS_EQ(pl->thick.disk_segment_uuid, owner_seg->uuid);
		bool should_reject_recovery = false;
		if (unlikely(!is_owner_requesting)) {
			WARN(true, "Bug in Toma, only owner seg can request self recovery");
			should_reject_recovery = true;
		}

		if (unlikely(pr->version != pl->thick.praid_version)) {
			_NTTR(t_tt_recov_01, "Cannot comply to recovery, @C_PRV != @T_PRV", pr->version, pl->thick.praid_version);
			should_reject_recovery = true;
		}
		if (nt->nd->ignore_all_recov_requests) {
			_NTTR(t_tt_recov_02, QA_BLOCK_PREFIX "Debug: Rejecting Toma recoveries");
			should_reject_recovery = true;
		}
		if (should_reject_recovery)
			ri->num_locks = 0;				// Daniel: A quick hack to force recovery reject. Think how to do it without adding 'auto_fail' param to function
	}
	if (ri->task.type == NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE) {
		_purge_recov:	// This is a special type of task (not really recovery)
		__toma_purge_stalocks_cache(tr, pl, t);
	} else {
		(void)nvmeibc_recovery_start(tr, pr, ri, pl->thick.praid_version, false);
	}
ignore_msg:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
_out:
	return;
}

/* When Toma sends registrable with outdated raid version, client should ignore
   its payload but cannot ignore the message or else it will be stuck
   unregistered. So client ignore the message, resend register request */
static void __toma_solve_outdated_registrable(struct nvmeibc_topology *t,
										const struct nvmeibc_subscription_ctx *tr)
{
	struct nvmeibc_topology *t1 = dup_topology(t);
	if (t1) {
		__toma_after_update_send_raid_reg(__get_r1_by_tr(t1, tr), t);
		__set_active_topology(t->nt, t1, t);
	} else {
		_NE_TOPO(error_topology_toma_solve_outdated_registrable, t, "No memory for a topology dup");
	}
}

static int __verify_topology_crc(const struct nvmeibc_raid1 *r1, const struct nvmeibt_client_msg *pl)
{
	if (pl->thick.praid_version == r1->version) {
		const struct nvmeibt_client_topo_praid *msg = (void*)&pl->thick.data;
		if (msg->topo_checksum != r1->toma.topo_checksum) {
			_NT_SCOPE(trace_0_verify_topology_crc, topology, "topo checksum failed: {Clnt=@T_PRCHKSUM, Toma=@T_PRCHKSUM, @T_PRV}", r1->toma.topo_checksum, msg->topo_checksum, r1->version);
			return -EINVAL;
		}
	}
	return 0;
}

static void __verify_topology_msg_header_integrity(const struct nvmeibt_client_msg *pl)
{
	const struct nvmeibt_client_topo_praid *msg = (void*)&pl->thick.data;
	if (unlikely(pl->thick.praid_version != msg->praid_version))
		_NT_SCOPE(trace_1_verify_topology_crc, topology, "nvmeibc: toma bug, @T_PRV != @T_PRV", pl->thick.praid_version, msg->praid_version);
}

struct register_seg_outcome {
	int rv;
	bool activated; // at least 1 needed segment was not registered in this topo so we scheduled registration request to needed segments in the entire praid
};

static struct register_seg_outcome __toma_register_raid1_by_seg(const struct nvmeibc_subscription_ctx *tr, enum NVMEIBT_CLIENT_TR_REASON reason);

/* Assumed that len(pl) >= sizeof(struct nvmeibt_client_msg) already verified. */
static int __toma_update_topology_with_new(const struct nvmeibc_subscription_ctx *tr, struct nvmeibt_client_msg *pl, int len)
{
	unsigned long flags;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_topology *t = NULL, *t1;
	struct nvmeibc_topologies *nt;
	int pl_ver, rv, should_apply_reconf = false;
	const u32 pl_lock_id = pl->thick.lock_id;
	bool should_update_lock_id = true; /* By default, take tomas proposed lock*/
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type = pl->hdr.msg_type;
	bool should_register_on_registrable_error = false;
	NFIN;
	nt = tr->nt;
	__verify_topology_msg_header_integrity(pl);
	spin_lock_irqsave(&nt->lock, flags);
	__if_cant_handle_goto(t_05_2433, tr, ignore_msg);
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) goto ignore_msg;
	r1  = __get_r1_by_tr( t, tr);
	if (unlikely(!__toma_topology_msg_is_valid(tr, pl, len)))
		goto ignore_msg;

	if (__verify_topology_crc(r1, pl) < 0) {
		WARN_TOPO(true, t, "wrong topology crc\n");
		should_apply_reconf = true; // Must be enabled to cause a rereg
		goto ignore_msg;
	}

	pl_ver = pl->thick.praid_version;
	if (r1->lid.all != LS_UNLOCKED){
		switch (msg_type) {
		case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT:
			if (pl_ver < r1->version) {
				_NI_TOPO(trace_topology_toma_update_topology_with_new, t, "Ignoring registrable: raid is ahead(@T_PRV < @C_PRV), c_lid=@C_LID", pl_ver, r1->version, r1->lid.all);
				__toma_solve_outdated_registrable(t, tr);
				goto ignore_msg;
			} else if (pl_ver == r1->version) {
				/* We should register, probably coz toma previously answered
				with TOMA_NOT_READY and now it is ready. Latest topology needs
				this segment to enable IO */
				__verify_toma_correct_registrable(tr, t);
			}
			break;

		case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK:
			if (unlikely(pl->hdr.reason ==
				NVMEIBT_CLIENT_TR_REASON_LOCKID_ALREADY_TAKEN)) {
				/* Toma gave NACK coz it can't support the proposed lock-id, but we
				  might already be registered with other Tomas. Warm re-reg (as if
				  NACK was broken). Daniel: We don't care about raid version's here */
				_NT_TOPO(trace_1_topology_toma_update_topology_with_new, t, "NACK treated as UNREG (lock contention). Warming...");
				should_apply_reconf = true; // Must be enabled to cause a rereg
				goto ignore_msg;
			} else if (pl_ver <= r1->version) {
				_NT_TOPO(trace_2_topology_toma_update_topology_with_new, t, "Ignoring nack: raid already updated(@T_PRV <= @C_PRV), c_lid=@C_LID",
				   pl_ver, r1->version, r1->lid.all);
				goto ignore_msg;
			}
			break;

		default:
			WARN_TOPO(true, t, "unknown msg type=0x%x\n", msg_type);
			break;
		}
	}

	if (((!nvmeibc_topo_is_in_err_state(t))) && (r1->replicas >= 2)) { // IO enabled. Must be causious because if we except new topology IO can stop
		const int seg_id = __find_single_seg_diff_from_toma_msg(pl, r1);
		const bool has_ureged_seg = ((seg_id>=0) && (!is_seg_active(r1->segments[seg_id])));
		if (has_ureged_seg && (r1->segments[seg_id].toma_acm == NVMEIBTC_DS_MODE_DEAD)) {
			/* {RW,D} -> {RW,W} - Delayed (hot) switch topo must be invoked,
			so dont let this NACK/Registrable drop it to warm. Ignore msg */
			struct nvmeibc_disk_segment *dead_seg = &r1->segments[seg_id];
			const int oa_ver = nvmeibc_seg_on_active_get_version_calc(dead_seg);
			if (oa_ver >= pl_ver) {
				/* We already have a delayed switch_topo msg so hot transition
				   is on the way (started by the owner RW Toma)! */
				__segment_register(dead_seg);
			} else {
				/* RW Toma's SWITCH TOPO has not arrived yet (or NACK has newer
				   version). SWITCH TOPO might never arrive, if that Toma Dies,
				   so let NACK effectively start hot switch topology */
				WARN_TOPO((pl_ver == r1->version), t, "Toma left client in degraded mode, seg=%d, t_prv=0x%x\n", seg_id, r1->version);
				__toma_delayed_update_topo(tr, pl, len, &r1->segments[seg_id]);
			}
			goto ignore_msg; // Same as for delayed switch topo RW,D->RW,W
		} else if (has_ureged_seg) {
			/* During volume extend, last chunk is irrelevant for io. So unregistered segment can be even RW */
		}
	}

	/* If already have lock id -> registration request was sent to segments.
	   However it might failed (Toma not ready) or we haven't got answer yet.
	   Anyways, do not take the newly proposed lock but do request registration
	   again (if it is needed) */
	if ((r1->lid.all != LS_UNLOCKED)&& (r1->lid.all != pl_lock_id )) {
		_NT_TOPO(trace_3_topology_toma_update_topology_with_new, t, "msg (@MSG_TYPE): waiting for reg with c_lid=@C_LID. "
		   "Ignorring proposed t_lid=@T_LID", pl->hdr.msg_type,
		   r1->lid.all, pl_lock_id);
		should_update_lock_id = false;
	}

	t1 = dup_topology(t);
	if (!t1) {
		_NE_TOPO(error_1_topology_toma_update_topology_with_new, t, "No memory for a topology dup");
		goto ignore_msg;
	}

	r1 = __get_r1_by_tr(t1, tr);
	rv = 0;
	if (should_update_lock_id)
		rv = __toma_update_lock_id(r1, pl_lock_id);
	if (rv == 0)
		rv = __toma_update_raid1(r1, pl, t);
	if (rv < 0) {
		if (rv == -EAGAIN) {
			_NI_TOPO(trace_4_topology_toma_update_topology_with_new, t, "NACK/Registrable failed. Warm applying new config for praid");
			should_apply_reconf = true;
		} else {
			const int clnt_ver = (int)__get_topo_of_r1(r1)->configuration_version;
			const int toma_ver = (int)pl->hdr.volume_config_version;
			if (clnt_ver < toma_ver) {
				_NI_TOPO(trace_5_topology_toma_update_topology_with_new, t, "NACK/Registrable failed. Dont have latest config? rv=@RV", rv);
			} else if (msg_type == NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT) {
				_NI_TOPO(trace_6_topology_toma_update_topology_with_new, t, "Registrable failed, rv=@RV. Volume versions: c_@C_VOL_VER >= t_@C_VOL_VER, set to register", rv, clnt_ver, toma_ver);
				should_register_on_registrable_error = true;
			}
		}
		__poison_raid1_by_seg(tr, t1, t);
	}
	__set_active_topology(nt, t1, t);

ignore_msg:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
	if (should_apply_reconf) {
		nvmeibc_warm_raid1_apply_conf_diffs(nt, (struct nvmeibc_subscription_ctx *)tr);
	} else if (should_register_on_registrable_error) {
		__toma_register_raid1_by_seg(tr, NVMEIBT_CLIENT_RT_REASON_POISON_RAID);
	}
	NFOUT;
	return 0;
}

static int __toma_switch_topology(struct nvmeibc_subscription_ctx *tr, struct nvmeibt_client_msg *pl, int len)
{
	unsigned long flags;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_topology *t = NULL, *t1;
	struct nvmeibc_topologies *nt;
	int pl_ver, update_rv = 0;
	const u32 pl_lock_id = pl->thick.lock_id;
	bool update = true;

	if (unlikely(!__toma_topology_msg_is_valid(tr, pl, len))){
		_NTTR(trace_topology_toma_switch_topology, "toma topo is invalid");
		goto _out;
	}

	nt = tr->nt;
	spin_lock_irqsave(&nt->lock, flags);
	__if_cant_handle_goto(t_06_2433, tr, ignore_msg);

	update_rv = __raid1_try_to_apply_RUD(tr, pl, len);
	if (update_rv < 0) {
		_NTTR(trace_1_topology_toma_switch_topology, "Failed to apply RUD rv=@UPDATE_RV", update_rv);
		if ((update_rv == -EAGAIN)||(update_rv == -EBUSY)) {
			spin_unlock_irqrestore(&nt->lock, flags);
			goto _out;	/* We don't have new conf yet, or cant apply. Wait */
		}
		_NTTR(warn_topology_toma_switch_topology, "Serious problem. Cannot apply seg & switch-topo. Warming praid");
		spin_unlock_irqrestore(&nt->lock, flags);
		/* Do not warm the entire device, because we possibly hold an old volume
		   version. We will not be able to register other segments once
		   unregistered, because toma will answer with invalid volume version */
		nvmeibc_warm_raid1_apply_conf_diffs(nt, tr);
		goto _out;
	}

	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) goto ignore_msg;
	r1  = __get_r1_by_tr( t, tr);

	if (__verify_topology_crc(r1, pl) < 0) {
		WARN_TOPO(true, t, "wrong switch_topo msg crc\n");
		goto ignore_msg;
	}

	pl_ver = pl->thick.praid_version;
	if (pl_ver <= r1->version) {
		_NTTR(trace_2_topology_toma_switch_topology, "Ignoring msg: raid1 already updated (@T_PRV <= @C_PRV)", pl_ver,
		   r1->version);
		update = false;
		goto do_switch_topo_without_delay; /* Do send ack on switch topo */
	}

	if ((r1->lid.all == LS_UNLOCKED) || // Can't comply to switch topo
		(pl_lock_id  == LS_UNLOCKED) || // WTF??? Toma bug, Illegal switch topo
		(r1->lid.all != pl_lock_id)) {  // WTF?? Lock-id dissagreement...
		if (r1->lid.all == LS_UNLOCKED){
			_NTTR(trace_3_topology_toma_switch_topology, "Client already unregistered. Will not ACK, Warming...");
			/* This can occur when client unregisters and toma switches topo at the same time. Perform warm recovery. Unregister acts as ACK */
		} else {
			_NTTR(trace_4_topology_toma_switch_topology, "Serious problem. Out of sync. c_lid=@C_LID, t_lid=@T_LID. Warming...", r1->lid.all, pl_lock_id);
		}
		spin_unlock_irqrestore(&nt->lock, flags);
		nvmeibc_topology_put(t);
		nvmeibc_warm_apply_conf_diffs(nt); /* Acts as ack on switch topo */
		goto _out;
	}

	WARN_ON(update == false);		// Set a trap. The code below will definitely update the topo either in delayed or inline fashion. All tests had to be before
	if (nt->reservation_version != RESERVATION_MODE_IRRELEVANT) {
		WARN(nvmeibc_topo_is_partially_ok(&r1->toma) &&
			 (pl->thick.reservation_mode_version != nt->reservation_version),
			 "Bug in Toma: Wrong SW-topo, toma_RMV=0x%llx != clnt_RMV=0x%llx\n",
			 pl->thick.reservation_mode_version, nt->reservation_version);	// Trap to catch illegal switch topo transitions when reservation mode increases. Should be unregister transition. Changing reservation mode version while IO is enabled causes data corruption
	}
	/* 3 cases (Valid for 2 or more mirrored. Irrelevant for JBOD):
		1. {RW,D} -> {RW,W} - Delayed (hot) switch topo must be invoked
							  Do hot EVEN IF io is currently disabled coz other
							  raid is unregistered!
		2. {RW,W} -> {RW,D} - Immediate switch topo & unregister messages
		3. Every other option - Immediate switch topo & ACK message */
	if ((r1->replicas>=2)&&(true /*Read above: !nvmeibc_topo_is_in_err_state(t)*/)) {
		const int seg_id = __find_single_seg_diff_from_toma_msg(pl, r1);
		const bool has_ureged_seg = ((seg_id>=0) && (!is_seg_active(r1->segments[seg_id])));
		if (has_ureged_seg && (r1->segments[seg_id].toma_acm == NVMEIBTC_DS_MODE_DEAD)) {
			/* Delay switch topo ack, send register first */
			/* Note: if activate_seg->on_active exists it is NACK/REGISTRABLE,
			   send by the previously dead Toma. Overwrite it */
			__toma_delayed_update_topo(tr, pl, len, &r1->segments[seg_id]);
			goto ignore_msg;
		}
	}

do_switch_topo_without_delay:
	t1 = dup_topology(t);
	if (!t1) {
		_NE_TOPO(error_1_topology_toma_switch_topology, t, "No memory for a topology dup");
		goto ignore_msg;
	}

	update_rv = 0;
	if (update) {
		update_rv =__toma_update_raid1(__get_r1_by_tr(t1, tr), pl, t);
		if (update_rv < 0) {
		  _NE_TOPO(error_2_topology_toma_switch_topology, t, "Serious problem. switch_topo(@RV) failed", update_rv);
		  __poison_raid1_by_seg(tr, t1, t);
		}
	}
	if (update_rv >= 0) { /* If no error, schedule ack */
		struct nvmeibc_disk_segment *seg_new = __get_seg_by_tr(t1, tr);
		on_topo_free_schedule_seg_msg(seg_new, t, NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK, NVMEIBT_CLIENT_RT_REASON_INLINE_SW_TOPO);
	}
	__set_active_topology(nt, t1, t);

ignore_msg:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
_out:
	return 0;
}

#define is_multi_raid_topo_unreg(reason) \
	((reason==NVMEIBT_CLIENT_RT_REASON_UNREG_ALL_XXX)|| \
	 (reason==NVMEIBT_CLIENT_RT_REASON_UNREG_DISK_PAUSE))

/* Always rereg after unreg unless this is force unreg: Volume Detach/Reboot or Raid warm reboot. */
#define should_rereg_after_unreg(reason) \
	((reason!=NVMEIBT_CLIENT_RT_REASON_UNREG_ALL_XXX)&& \
	 (reason!=NVMEIBT_CLIENT_RT_REASON_WARM_UNREGISTER))

/* Returns: 1=scheduled un-reg request for all segs, 0=seg was already unreged*/
static int __toma_unregister_raid1_by_seg(struct nvmeibc_subscription_ctx *tr, struct nvmeibt_client_msg *optional_pl, enum NVMEIBT_CLIENT_TR_REASON reason)
{
	unsigned long flags = 0;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_topology *t = NULL, *t1 = NULL;
	struct nvmeibc_topologies *nt = tr->nt;
	const bool is_nt_already_locked = is_multi_raid_topo_unreg(reason);	// If multi-raid, lock was taken by caller
	const bool should_rereg = should_rereg_after_unreg(reason);
	bool deactivated = true;			// == 'rv', was praid deactivated

	NFIN;
	if (!is_nt_already_locked) {
		spin_lock_irqsave(&nt->lock, flags);
		__if_cant_handle_goto(t_07_2433, tr, out);
	}
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t))
		goto out; /* Already deactivated */
	r1  = __get_r1_by_tr( t, tr);

	if (!should_rereg) { // Force unregister praid, regardless of current seg
		deactivated = true;
	} else {			 // Unreg only if seg registered
		deactivated = is_seg_active(*__get_seg_by_tr(t, tr));
	}
	if (optional_pl) {
		const int pl_ver =     optional_pl->thick.praid_version;
		const u32 pl_lock_id = optional_pl->thick.lock_id;
		__update_reservation_version_max_seen(nt, optional_pl, NULL /*no need to update this r1*/, tr);
		if (pl_ver < r1->version) {
			_NT_TOPO(t_00_clnt_toma_unreg_praid_by_seg, t, "Ignoring msg: segment " SEGMENT_FMT " @T_PRV <= @C_PRV t_lid=@T_LID c_lid=@C_LID",
				tr->ch, tr->r1, tr->seg, pl_ver, r1->version, pl_lock_id, r1->lid.all);
			if ((pl_lock_id == r1->lid.all) && (pl_lock_id != 0)) {
				/* Old UNREG request but with correct lock id, is this result of
					1 toma doing switch topo while another decided to unreg? */
				_NW_TOPO(t_01_clnt_toma_unreg_praid_by_seg, t, "Serious problem. Unreg Out of sync " SEGMENT_FMT ". Warming", tr->ch, tr->r1, tr->seg);
				deactivated = true; // Can happen, Client either switch topology or via NACK received next version from other toma
			} else {
				deactivated = false;
				goto out;
			}
		}
	}

__do_unregister:
	if (deactivated) {
		t1 = dup_topology(t);
		if (!t1) {
			_NE_TOPO(t_02_clnt_toma_unreg_praid_by_seg, t, "No memory for a topology dup");
			goto out;
		}
		on_topo_free_schedule_praid_ack(tr, t1, t, NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT, reason);
		__set_active_topology(nt, t1, t);
		if (!should_rereg)
			goto out;

		nvmeibc_topology_put(t);
		t = nvmeibc_topology_get(nt); /* t got the head == t1*/
		WARN_ON(!t); /* Some topology must exist here, Once t drains out of IOs unregister will be sent. Schedule re-register to both segments. */
		t1 = dup_topology(t);
		if (!t1) {
			_NE_TOPO(t_03_clnt_toma_unreg_praid_by_seg, t, "No memory for a topology dup");
			goto out;
		}
		on_topo_free_schedule_praid_ack(tr, t1, t, NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT, reason);
		__set_active_topology(nt, t1, t);
	} else {
		if (optional_pl) { /* Real Toma message */
			/* Very strange, we are not registered to the segment, yet Toma requested us to unregister. Some kind of miss-understanding. Check if we have at least 1 registered segment, if we do, ask for help */
			struct nvmeibc_disk_segment *s;
			int si, any_seg_active = false;
			_NW_TOPO(t_04_clnt_toma_unreg_praid_by_seg, t, SEGMENT_FMT "cannot comply to unregister request @PROTOCOL_CLIENT_MSG_REASON_STR!", tr->ch, tr->r1, tr->seg, nvmeibt_protocol_client_msg_reason_str(reason));
			raid1_for_each_seg(r1, s, si){
				any_seg_active |= s->registration_status;
			}
			if (any_seg_active) {
				/* We have at least one registered segment so ask this Toma to put the client back on track via re-register */
				_NW_TOPO(t_05_clnt_toma_unreg_praid_by_seg, t, "Serious problem. Out of sync " SEGMENT_FMT ". Warming", tr->ch, tr->r1, tr->seg);
				deactivated = true;
				goto __do_unregister;
			} /* Else: :-( Wait 30 seconds until other Toma's will invoke disk pause. */
			_NW_TOPO(t_06_clnt_toma_unreg_praid_by_seg, t, SEGMENT_FMT "Serious problem. Stuck with no IO & no help", tr->ch, tr->r1, tr->seg);
		}
	}

out:
	if (!is_nt_already_locked)
		spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);	/* Note: dont do nvmeibc_topology_put(t1)! */
	NFOUT;
	return (deactivated ? 1 : 0);
}

/* Inverse of the above. Shcedule raid1 registration */
static struct register_seg_outcome __toma_register_raid1_by_seg(const struct nvmeibc_subscription_ctx *tr, enum NVMEIBT_CLIENT_TR_REASON reason)
{
	unsigned long flags;
	struct nvmeibc_disk_segment *seg = NULL;
	struct nvmeibc_topology *t = NULL, *t1 = NULL;
	struct nvmeibc_topologies *nt = tr->nt;
	struct register_seg_outcome outcome = {0};

	NFIN;
	spin_lock_irqsave(&nt->lock, flags);		/* Lock the spinlock before the get_topology to ensure usage of the latest topology and to serialize the updates. */
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		goto out;
	}
	seg = __get_seg_by_tr(t,tr);
	outcome.activated = !is_seg_active(*seg);

	if (outcome.activated) {
		t1 = dup_topology(t);
		if (!t1) {
			_NE_TOPO(error_topology_toma_register_raid1_by_seg, t, "No memory for a topology dup");
			outcome.rv = -ENOMEM;
			goto out;
		}
		on_topo_free_schedule_praid_ack(tr, t1, t, NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT, reason);
		__set_active_topology(nt, t1, t);
	} else {
		_NT_TOPO(trace_topology_toma_register_raid1_by_seg, t, "segment is active, doing nothing");
	}

out:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);	/* Note: dont do nvmeibc_topology_put(t1)! */
	NFOUT;
	return outcome;
}

int nvmeibc_topologies_rereg_seg(struct nvmeibc_disk_segment *seg)
{
	return __toma_unregister_raid1_by_seg(seg->toma_reg, NULL, NVMEIBT_CLIENT_RT_REASON_REREG_ON_IO_FAIL);
}

static inline void __do_on_invalid_seg_message(struct nvmeibc_subscription_ctx *tr)
{	// Ask management for a newer configuration on this volume
	const struct nvmeibc_block_device *nd = tr->nt->nd;
	const struct nvmeibc_cinst_params_main *pmain = nvmeibc_cinst_get_blok_m(nd);
	nvmeibc_cc_api_request_volume_config_in_atomic_context(pmain, nd->name);
}

static inline void __do_on_volume_mismatch_seg_message(struct nvmeibc_subscription_ctx *tr,
										struct nvmeibt_client_msg *pl, int len)
{
	unsigned long flags;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg = NULL;
	struct nvmeibc_topology *t = NULL;
	struct nvmeibc_topologies *nt = tr->nt;
	int si;

	NFIN;
	(void)len;
	spin_lock_irqsave(&nt->lock, flags);
	__if_cant_handle_goto(t_08_2433, tr, ignore_msg);

	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) goto ignore_msg;
	r1  = __get_r1_by_tr( t, tr);
	if (1) {		/* Test who has the newer volume version */
		const int clnt_ver = (int)t->configuration_version;
		const int toma_ver = (int)pl->hdr.volume_config_version;
		_NTTR(trace_topology_do_on_volume_mismatch_seg_message, "Volume versions: c_@C_VOL_VER != t_@C_VOL_VER, @C_PRV != @T_PRV",
		   clnt_ver, toma_ver, r1->version, pl->thick.praid_version);
		if (clnt_ver > toma_ver) {
			/* Toma will send us registrable when gets new conf */
			goto ignore_msg;
		}
		if (unlikely(clnt_ver == toma_ver)) {
			/* While toma msg was traveling to clnt, clnt received the new
			   config, resend registration request */
			__segment_register(&r1->segments[tr->seg]);
			goto ignore_msg;
		}
	}

	/* Daniel: Todo, break __raid1_try_to_apply_RUD() to sub funcs and use some
	   of them (note: msg doesnt have topology). don't have time now */
	raid1_for_each_seg(r1, seg, si){
		seg_mark_toma_said_apply(seg);
		if (seg_has_replacement(seg)) {
			_NTTR(trace_1_topology_do_on_volume_mismatch_seg_message, "@DEV_NAME: Toma is at least 2 configs ahead???", nt->device_name);
		}
	}

ignore_msg:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
	NFOUT;
}

/* Returns false if not live */
static bool __live_tr_handle_begin(struct nvmeibc_subscription_ctx *tr)
{
	unsigned long flags;
	bool alive;
	spin_lock_irqsave(&tr->death_lock, flags);
	alive = !is_toma_reg_already_dead(tr);
	if (alive) { // Can safely access nt here, since, before detach completes, tr needs to become dead (under death_lock).
		toma_msg_handlers_drainer_get(&tr->nt->tmhd);
	}
	spin_unlock_irqrestore(&tr->death_lock, flags);
	return alive;
}

static void __live_tr_handle_end(struct nvmeibc_subscription_ctx *tr)
{
	toma_msg_handlers_drainer_put(&tr->nt->tmhd);
}

/* This is a summary of the message handling for noobs (Mirrored volumes)
Toma message	| Dont Have Lock-id		|	Have Lock-id
-------------------------------------------------------------------------
Register_ack 	| BUG_ON()				| OK
Registrable 	| Update lock-ID		| If 1 seg is dead - ignore, Otherwise update
Nack 			| Update lock-ID		| If 1 seg is dead - ignore, Otherwise update
Unregister 		| Both cases: lock=0, request registration for full raid1 (will trigger nack)
Switch-Topolgy	| Ignore message		| if new lock != existing lock BUG_ON()
Volume Mismatch | Both cases: Do nothing
Invalid disk-id | Both cases: Do nothing
New-Topology	| Unsupported message
Toma Not ready	| Both cases: Do nothing. If received as answer to registration request, expect registrable
-------------------------------------------------------------------------
Disk Pause		| Same as 'Unregister' message but for multiple Raid1's
Disk Cont		| Allways arrives after pause completed. Request registration only on segments on disk (in case the previous registration could not be sent).

Non-Mirrored Volumes:
Toma message	| Dont Have Lock-id		|	Have Lock-id
-------------------------------------------------------------------------
Register_ack 	| OK					| OK
Registrable 	| Update lock-ID		| Update lock-ID
Nack 			| Not applicable		| Not applicable
Unregister 		| Both cases: lock=0, request registration for full raid1
Switch-Topolgy	| Not applicable		| Not applicable
Volume Mismatch | Unsupported Yet ???
Invalid disk-id | Do nothing
New-Topology	| Not applicable		| Not applicable
Toma Not ready	| Both cases: Do nothing. If received as answer to registration request, expect registrable
-------------------------------------------------------------------------
Disk Pause		| Same as 'Unregister' message but for multiple Segments
Disk Cont		| Allways arrives after pause completed. Request registration only on segments on disk (in case the previous registration could not be sent).
*/

static void __block_toma_msg_handler(void *unused_cinst, u64 handle, u8 *buf, int len)
{
	/* Implements transport: nvmeibc_disk_toma_recv_req_callback_t callback */
	struct nvmeibc_topologies *nt = NULL;
	struct nvmeibt_client_msg *pl;
	struct nvmeibc_subscription_ctx *tr;
	enum NVMEIBT_CLIENT_MSG_DECODE_RES decode_rv;
	u32 pl_lock_id;
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type;
	enum NVMEIBT_CLIENT_TR_REASON reason;

	NFIN;
	BUILD_BUG_ON(sizeof(struct nvmeibt_client_msg) +
				 sizeof(struct nvmeibt_client_topo_praid) +
				 sizeof(struct nvmeibt_client_topo_disk_segment) * N_MAX_RAID_SLICE_LEN  > NVMEIB_TOMA_REQ_MAX_LEN);
	tr = __get_tr(handle); (void)unused_cinst; // Daniel: Boiler plate for when tr hash will be per instance and not global
	if (!tr) {
		_NT_SCOPE(t01btmh, topology, "TOMA msg for handle=@HANDLE ignored, no such handle", handle);
		goto _out;
	}

	if (unlikely(!buf || (len < (int)sizeof(struct nvmeibt_client_msg)))) {
		WARN(1, "TOMA msg invalid: tr=%p buf=%p len=%d\n", tr, buf, len);
		_Dbuf(buf, len);
		goto _out;
	}

	decode_rv = nvmeibt_client_decode_new(buf, len, &pl);
	if (unlikely(decode_rv == NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_ERROR)) {
		WARN(1, "TOMA msg decode error: tr=%p buf=%p len=%d\n", tr, buf, len);
		_Dbuf(buf, len);
		goto _out;
	} else if (unlikely(decode_rv == NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_MISMATCH)) {
		//actually this should never happen: toma never starts to talk with a client.
		//The client initiates a conversation, so toma should remember the version and talk using it.
		//If somehow toma will success to send message first:
		//1) not clear what the handle value will be?
		//2) it was decided to ignore the message - the client will initiate the conversation and will pass the correct version
		struct nvmeibt_client_msg_summary prior_msg_smr = nvmeibt_client_decode_msg_summary(buf, len);
		_NT_SCOPE(t02btmh, topology, "Unsupported proto:@TOMA_CLIENT_PROTOCOL_VERSION, msg_type=@MSG_TYPE_STR", prior_msg_smr.protocol_version, nvmeibt_protocol_client_msg_str(prior_msg_smr.msg_type));
		goto _out;
	}

	if (unlikely(pl->hdr.protocol_version != tr->protocol_version)) {
		if (!(pl->hdr.msg_type == (s32)NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY && pl->hdr.reason == NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH)) { // not a version negotiation response
			_NWTR(w01btmh, "TOMA had used non-negotiated protocol version, ignoring message: negotiated=@TOMA_CLIENT_PROTOCOL_VERSION, toma=@TOMA_CLIENT_PROTOCOL_VERSION", tr->protocol_version, pl->hdr.protocol_version);
			goto _out;
		}
	}

	if (!__live_tr_handle_begin(tr)) {				// Not alive.
		goto _out; // and not goto _end_handle, since didn't take a drainer ref
	}

	// Took a drainer ref, nt won't disappear.
	nt = tr->nt; /* nt always exists for non zombies segments */
	msg_type = pl->hdr.msg_type;
	reason   = pl->hdr.reason;
	pl_lock_id = pl->thick.lock_id;
	_NTTR(t04btmh, "msg(@MSG_TYPE, cookie=@COOKIE) h=@HANDLE tr=@TR buf=@BUF len=@LEN @T_PRV toma_lid=@T_LID toma_@RES_MOD_VER", msg_type, pl->hdr.cookie, handle, tr, buf, len, pl->thick.praid_version, pl_lock_id, pl->thick.reservation_mode_version);

	if (nvmeibc_block_nt_to_b(nt)->ignore_all_toma_msgs) {
		_NTTR(t05btmh, QA_BLOCK_PREFIX "Ignoring toma message");
		goto _end_handle;
	}

	_NITR(t06btmh, "@PROTOCOL_CLIENT_MSG_STR(@PROTOCOL_CLIENT_MSG_REASON_STR) received, cfg=@CFG, conv=@CLNT_TOMA_PR_CONVER_IND",
	   nvmeibt_protocol_client_msg_str(msg_type), nvmeibt_protocol_client_msg_reason_str(reason), pl->hdr.volume_config_version, pl->thick.conversation_ind);
	switch (msg_type) {
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK:
		__toma_segment_register_succeed(tr, pl, len);
		break;
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK:
		break;

	case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK:
		__toma_update_topology_with_new(tr, pl, len);
		break;

	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT:
		__toma_unregister_raid1_by_seg(tr, pl, NVMEIBT_CLIENT_RT_REASON_INSTRUCTED_UNREG);
		break;

	case NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY:
		__toma_switch_topology(tr, pl, len);
		break;

	case NVMEIBT_CLIENT_MSG_TR_RECOVER_START:
	case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE: // Other recovery type API
		__toma_recover_start(tr, pl, len);
		break;

	case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING:
		__toma_handle_running_recovery(tr, msg_type, pl, len);
		break;

	case NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH:
		__do_on_volume_mismatch_seg_message(tr, pl, len);
		break;

	case NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY:
		__do_on_toma_not_ready(tr, pl);
		break;

	case NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID:
		__do_on_invalid_seg_message(tr);
		break;

	case NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED: {
		stale_lock_resolver_set_resolved(&tr->hdr->slr, tr->seg, (void*)&pl[1]);
		break;
	}

	default:	/* Unsupported messages */
		WARN(true, "nvmeibc bug. unsupported msg_type=0x%x\n", msg_type);
		break;
	} // End of switch (msg_type)

_end_handle:
	__live_tr_handle_end(tr);

_out:
	if (tr)
		__put_tr(tr); //give up tr ref used by the message
	NFOUT;
}

void nvmeibc_topology_pause(struct nvmeibc_topologies *nt, struct nvmeibc_idisk *disk)
{
	int c, r, si, n_unreged_praids = 0, n_segs_on_disk = 0;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	struct nvmeibc_topology *tb /* Before pause */, *tcp /* cont preventor */;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&nt->lock, flags);
	tb = nvmeibc_topology_get(nt);
	if (unlikely(!tb)) {
		_NT_SCOPE(t_00_cbtp, topology, "@DEV_NAME: ignore pause, has no topologies nt=@NT", nt->device_name, nt);
		spin_unlock_irqrestore(&nt->lock, flags);
		goto _out;
	}
	topo_for_each_seg(tb, chunk, c, r1, r, seg, si) { // Must hold nt lock through the entire loop to prevent other thread from reconfiguring the head topology
		if (seg->disk == disk) {
			n_unreged_praids += __toma_unregister_raid1_by_seg(seg->toma_reg, NULL, NVMEIBT_CLIENT_RT_REASON_UNREG_DISK_PAUSE);
			n_segs_on_disk++;
		}
	}
	/* Here Guaranteed: new IO for this disk cannot be issued on head topo, though nt->io_perm can allow IO, if all raids are in degraded mode */
	if (n_segs_on_disk > 0) { // Head topo is the one that should be free'd before cont arrives.
		// Note: n_unreged_praids can be zero (praids on disk are non ioable regardless of pause). Still need cont preventer! Why? IO can be in air to praids on disk from old topology (before tb), and it must be drained, even though tb is non iable
		struct nvmeibc_topology *t1;
		tcp = nvmeibc_topology_get(nt);	// Head is the cont preventor topo.
		t1 = dup_topology(tcp);
		if (unlikely(t1 == NULL)) {
			WARN(true, "nvmeibc: out of memory, crashing the system to prevent disk %s from corrupting data of volume %s\n", disk->ops.get_full_name(disk), nt->device_name);
			BUG();
		}
		__set_active_topology(nt, t1, tcp);
	} else {
		tcp = NULL;
		_NT_SCOPE(t_01_cbtp, topology, "@DEV_NAME: Pause on irrelevant disk to this volume\n", nt->device_name); // may happen during volume update/extented.
	}
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(tb);
	if (tcp) {
		on_topo_free_cont_preventer_inc(tcp, disk, n_unreged_praids);	 /* Set tcp as cont preventor */
		nvmeibc_topology_put(tcp);
	}
_out:
	NFOUT;
}

/* Inverse of __subscribe_seg(),
   Once this function terminates, segment will not get any more toma messages */
static inline int __toma_disconnect_segment(struct nvmeibc_disk_segment *seg)
{
	struct nvmeibc_subscription_ctx *tr = seg->toma_reg;
	struct nvmeibc_topologies   *nt	= NULL;
	struct nvmeibc_disk_id_update_params *params = NULL;
	int rv = 0;
	if (is_toma_reg_valid(tr)) {	// Common case
		WARN(!tr->hdr, "nvmeibc bug! double disconnect\n");
		tr->hdr = NULL;
		nt = tr->nt;
		/* seg is not needed anymore - it will not receive TOMA messages */
		/* tr->status == NVMEIBC_SUBSCRIPTION_STATUS_NORMAL if init failed, common case TRS_DEAD, or if abandoned locks then ZOMBIE/ZOMBIE_DEAD */
		seg->toma_reg = NULL;
		__put_tr(tr); /* Put refcount of this segment */
	} else { /* tr does not exist because init error or in dettach */
		if (seg->chunk) {
			nt = __get_topo_of_seg(seg)->nt;
			if (nvmeibc_block_nt_to_b(nt) != nt->nd)
				/* Vol update failure: new topo not attached to bdev yet, so
				   definitely segment was never subsribed yet */
				goto _out;
			else {
				BUG();	// Disconnect called twice?
			}
		} else
			goto _out; /* init error, no need to disconenct seg from volume */
	}

	/* The code below must run now regardless if tr was just destroyed or
		will be destroyed as zombie. It must not run if the segment was not
		attached to the volume (error during initialization) */
	params = kzalloc(sizeof(*params), GFP_ATOMIC); /* not accounting as free is deferred && not done in this file */
	if (!params){
		_NE_to_user(t_01_topods, DMESG_PREFIX("@DEV_NAME"), "Out of memory when trying to allocate memory to release a volume element, which will probably lead to issues detaching the volume and to the volume becoming disabled for IO, reboot is probably necessary. Error code: 1030.", nt->device_name);
		rv = -ENOMEM;
		goto _out;
	}
	params->disk = seg->disk;
	params->is_attach = false;
	params->block_dev = nt->nd;
	// Force workque to free the params if the update doesn't execute
	nvmeibc_block_set_generic_work_to_main(nvmeibc_cinst_get_blok_p(nt->nd), nt->nd->uuid, nvmeibc_volume_update_volume_single_segment, params, true);
	_NT_SCOPE(t_02_topods, topology, "@DEV_NAME: One less tie to disk @DISK_NAME", nt->device_name, seg->disk->ops.get_full_name(seg->disk));
_out:
	return rv;
}

/* Allocs a (private) seg->toma_reg and populates its fields. Then subscribes the segment */
static int __subscribe_seg(struct nvmeibc_disk_segment *seg, int c, int r1,
	int si, struct nvmeibc_topologies *nt, struct nvmeibc_raid_topo_persistent *hdr)
{
	struct nvmeibc_disk_subscription_params params = {.recv_req_cb = __block_toma_msg_handler,};
	struct nvmeibc_block_device *nd = NULL;
	struct nvmeibc_disk_id_update_params *disk_id_update = NULL;
	int rv = -EDOM;

	NFIN;
	_NT_SCOPE(trace_topology_subscribe_seg, topology, "subscribe=@PTR,@SEGMENT_UUID on @DISK" SEGMENT_FMT "",seg, seg->uuid, seg->disk,c, r1, si);
	if (seg->toma_reg) { /* If happens, this is a bug! */
		WARN_ON_ONCE(1);
		rv = 0;
		goto out;
	}
	if (nvmeibc_idisk_get_block_to_disk_sector_shift(seg->disk) < 0) { //vcfg@"nvmesh block size should be bigger or equal to the disk block size"
		char* msg = topo_kmalloc(96, GFP_ATOMIC);
		_NE_to_user(error_topology_subscribe_seg, DMESG_PREFIX("@DEV_NAME"), "Unexpected error with block size mismatch between a volume and a physical disk, IO will not be possible to this volume. Error code: 1031. Volume block size: @N_BYTES[bytes], Disk block size: @N_BYTES[bytes]", nt->device_name, NVMEIBC_SECTOR_SIZE, (1 << seg->disk->ops.get_sector_shift(seg->disk)));
		if (msg) {
			scnprintf(msg, 96, "EVol-Disk Missmatch@Vol %s block=%d[b], disk %s=%d[b]\n", nt->device_name, NVMEIBC_SECTOR_SIZE, seg->disk->ops.get_name(seg->disk), (1 << seg->disk->ops.get_sector_shift(seg->disk)));
			nvmeibc_block_send_mgmt_allert(nvmeibc_block_nt_to_b(nt), msg);//, 0, false);
		}
		rv = -EFAULT;	/* Cannot do IO to this segment */
		goto out;
	}

	/* Allocate memory for a registration. It is more general than
	 * a single nvmeibc_disk_segment pointer. Registrations are per disk
	 * segment and not per topology->segment, as these keep changing. */
	seg->toma_reg = tr_create();
	disk_id_update= kzalloc(sizeof(*disk_id_update), GFP_ATOMIC); /* not accounting as free is deferred && not done in this file */
	if ((!seg->toma_reg)||(!disk_id_update)) {
		rv = -ENOMEM - 1000;
		goto _error_on_mem_alloc;
	}

	handle_of(seg) = nvmeibc_disk_locks_seg_locks_mem_info(seg->disk, -1);
	if (!handle_of(seg)) {
		rv = -ENOMEM - 2000;
		goto _error_on_mem_alloc;
	}

	/* The fields below are part of configuration and not topology so they
	   should appear only in toma_reg, but we keep them in segment to reduce
	   amount of pointes indirection in the IO path */
	seg->toma_reg->first_lba = seg->first_lba;
	seg->toma_reg->length = seg->length;
	seg->toma_reg->hdr = hdr;

	seg->toma_reg->nt = NULL;
	seg->toma_reg->ch = c;
	seg->toma_reg->r1 = r1;
	seg->toma_reg->seg= si;
	seg->toma_reg->protocol_version = NVMEIBT_CLIENT_PROTO_VERSION;		// Default, talk on the latest (highest) protocol version
	seg->toma_reg->disk = seg->disk; // Only for debugging reason
	seg->toma_reg->nt = nt;			// To be able to print inserted tr right away
	nvmeibc_trs_hash_insert(seg->toma_reg);

	/* Subscribe */
	rv = nvmeibc_trs_hash_subscribe(seg->toma_reg, &params);
	if (unlikely(rv < 0)) {
		goto _error_on_mem_alloc;
	}

	nd = nvmeibc_block_nt_to_b(nt);
	disk_id_update->disk = seg->disk;
	disk_id_update->is_attach = true;
	disk_id_update->block_dev = nd;
	// Doron TODO: should we overload the return value (it is used everywhere)
	nvmeibc_block_set_generic_work_to_main(nvmeibc_cinst_get_blok_p(nd), nd->uuid,
		nvmeibc_volume_update_volume_single_segment, disk_id_update, true);
	rv = 0;
out:
	NFOUT;
	return rv;

_error_on_mem_alloc:
	_NE_SCOPE(t_30_toposubseg, topology, DMESG_PREFIX("@DEV_NAME")  ": Subscription failed for seg=@PTR " SEGMENT_FMT " @SEGMENT_UUID, rv=@RV", nt->nd->name, seg, c, r1, si, seg->uuid, rv);
	_NE_to_user(t_31_toposubseg, DMESG_PREFIX("@DEV_NAME"), "Out of memory when trying to setup a volume, will retry later, but detaching and attaching the volume be needed when the memory shortage has been relieved. Error code: 1032.", nt->nd->name);
	tr_destroy_unused(seg->toma_reg);  // Not __put_tr(). Don't call destructor
	seg->toma_reg = NULL;
	topo_kfree(disk_id_update);
	goto out;
}

/* Initial segments are subscribed at __digest_segment_layout(), realtime
	replacements/upgrades has to be subscribed as well via this function.
	WARNING!!! Do not call from interrupt / spinlock / irq save context! */
static int __subscribe_all_segs(struct nvmeibc_topology *t){
	int c, r = 0, si = 0, rv = 0;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	topo_for_each_seg(t, chunk, c, r1, r, seg, si){
		if (!seg->toma_reg)
			if ((rv = __subscribe_seg(seg, c, r, si, t->nt, r1->hdr)) < 0)
				goto _out;
		if (seg_has_replacement(seg))
			if ((rv = __subscribe_seg(seg->replacement, c, r, si, t->nt, r1->hdr)) < 0)
				goto _out;
		if (seg_has_upgrade(seg)){
			int ui = seg_get_upgrade_order(seg);
			if ((rv = __subscribe_seg(seg->replacement, c, r, ui, t->nt, r1->hdr)) < 0)
				goto _out;
		}
	}

	if (chunk_has_addition(t)) { /* Subscribe all segs of last N chunks */
		struct nvmeibc_topology_resize resize = t->resize;	// Reduce the probability of the bug EC-2770 by coping on stack!
		for (c = __get_topo_num_chunks(t); c < resize.nchunks; c++){
			chunk = &resize.chunks[c];
			if (unlikely(!chunk->raid1s)) continue;
			for (r = 0; r < __get_num_r1s(t); r++) {
				r1 = &chunk->raid1s[r];
				for (si = 0; si < r1->replicas; si++) {
					seg = &r1->segments[si];
					if ((rv = __subscribe_seg(seg, c, r, si, t->nt, r1->hdr)) < 0)
						goto _out;
					// newborn segments should not have replacement and/or RAID changes
					BUG_ON(seg_has_replacement(seg) || seg_has_upgrade(seg));
				}
			}
		}
	}
_out:
	if (unlikely(rv < 0)){
		_NT_SCOPE(trace_topology_subscribe_all_segs, topology, "@DEV_NAME" SEGMENT_FMT ": @C_VOL_VER, failed. aborting!", t->nt->device_name, c, r, si, (int)t->configuration_version);
	}
	return rv;
}

/************************* Segment relocation methods ************************/
/* Apply configuration change on the segment. Replace the deprecated segment in
   new topology, by its newer version, or replicate it (upgrade) or delete it
   (downgrade). Must already have the reconfiguration attached to the segment
	act - 'r' for replace, 'u' for upgrade, 'd' for downgrade */
static int __segment_apply_reconf(struct nvmeibc_disk_segment *seg,
									   struct nvmeibc_topology *old_t, char act)
{
	struct nvmeibc_disk_segment *seg_new = seg->replacement;
	struct nvmeibc_topology *t;
	const struct nvmeibc_subscription_ctx *tr 	= seg->toma_reg;
	const bool delete_old 		= ( act=='r')||(act=='d');
	const bool replace_segs		= ( act=='r');
	const bool init_new			= ( act=='r')||(act=='u');
	const char print_separator	= ((act=='u')?'+':'<');
	const char* second_uuid 	= ((act!='d')? seg_new->uuid : "NULL");

	NFIN;
	BUG_ON(!spin_is_locked(&tr->nt->lock));
	_NTTR(trace_topology_segment_apply_reconf, "reconf @ACT_CHR {@SEG @ACT_CHR @SEG}",
			act, seg->uuid, print_separator, second_uuid);

	if (init_new) { 		/* Copy the realtime information of seg */
		BUG_ON(!seg_new->toma_reg); /* Should already be subscribed */
		seg_new->sync_safety	= seg->sync_safety;
		seg_new->toma_acm 		= NVMEIBTC_DS_MODE_DEAD; // New seg is unused
		seg_new->lmap           = seg->lmap;
		seg_new->chunk 			= seg->chunk;
		nvmeibc_seg_on_active_init(seg_new);
		seg_new->toma_reg->nt 	= seg->toma_reg->nt;
		seg_new->replacement	= NULL;
		if (replace_segs) { // Need to release the copied profiler
			__seg_put_profilers(seg);
		}
	}

	if (replace_segs) {
		if (seg->toma_reg)
			__put_tr(seg->toma_reg);
		__seg_shallow_move_unsafe(seg, seg_new);
		topo_kfree(seg_new); // Unify into shallow_move() ???
	}

	t = seg->chunk->topology;	/* update new topology */
	t->num_reconfing_segs--;
	_ND_TOPO(trace_1_topology_segment_apply_reconf, t, "Topology has @NUM_RECONFING_SEGS deprecated segments", t->num_reconfing_segs);

	/* Mark that the previous topology is the last owner who needs the original
	   segment, redirect toma mesg's (If Toma sends msg on old seg = Toma bug!)
	   Will be free() as part of old_t */
	if (delete_old) {
		seg_new = __get_seg_by_tr(old_t, tr);
		__seg_prepare_for_free(seg_new, 0);
	}
	NFOUT;
	return 1;
}

static int __raid1_apply_replacement(struct nvmeibc_raid1 *r1,
				bool force_unrequested, struct nvmeibc_topology *old_t)
{
	int si, was_replaced = 0;
	struct nvmeibc_disk_segment *seg;
	raid1_for_each_seg(r1, seg, si){
		if (!seg_has_replacement(seg))
			continue;
		if (seg_has_toma_said_apply(seg) || force_unrequested) {
			/* Toma requested the transition or we force it*/
			was_replaced += __segment_apply_reconf(seg, old_t, 'r');
		}
	}
	if (was_replaced) {
		__nvmeibc_raid1_clear_old_topo_1st_tr(old_t, r1);
	}
	return was_replaced;
}

static int __raid1_apply_downgrade(struct nvmeibc_raid1 *r1,
				bool force_unrequested, struct nvmeibc_topology *old_t)
{
	int si, was_downgraded = 0;
	struct nvmeibc_disk_segment *seg;

	BUG_ON(nvmeibc_raid_is_ec(r1));
	raid1_for_each_seg(r1, seg, si){
		if (!seg_has_downgrade(seg))
			continue;
		if (seg_has_toma_said_apply(seg) || force_unrequested) {
			/* Toma downgrade or we force it*/
			was_downgraded += __segment_apply_reconf(seg, old_t, 'd');
			lock_ownership_update_raid_map(r1, si, 'd');
			r1->replicas--;
			__put_tr(seg->toma_reg); /* newly duped topo does not need this segment */
			if (seg==r1->segments) { /* If seg[0] was removed copy seg[1] into its place */
				r1->segments[1].toma_reg->seg = 0;
				r1->segments[0] = r1->segments[1]; /*EC-1473: Fix for non 2 segs 3-mirrored */
			}
		}
	}
	if (was_downgraded){
		block_api_os_change_mirorring(nvmeibc_block_t_to_b(old_t));
		__nvmeibc_raid1_clear_old_topo_1st_tr(old_t, r1);
	}

	return was_downgraded;
}

static int __raid1_apply_upgrade(struct nvmeibc_raid1 *r1,
				bool force_unrequested, struct nvmeibc_topology *old_t)
{
	int was_upgraded = 0;
	struct nvmeibc_disk_segment *seg = &r1->segments[0];
	int si, ind_of_new;

	BUG_ON(nvmeibc_raid_is_ec(r1));
	if (!seg_has_upgrade(seg))
		goto _out;
	if (seg_has_toma_said_apply(seg) || force_unrequested) {
		/* Toma upgrade or we force it*/
		ind_of_new = seg_get_upgrade_order(seg); /* index of new seg */
		was_upgraded += __segment_apply_reconf(seg, old_t, 'u');
		r1->segments = seg->replacement;	/* Array of 2 segs */
		seg->replacement = NULL;
		if (ind_of_new == 1) {
			r1->segments[1] = r1->segments[0]; 	/* Move new segment to 1 */
			r1->segments[0] = *seg;				/* Insert existing at 0 */
		} else {
			r1->segments[1] = *seg;				/* Append existing at 1*/
			r1->segments[1].toma_reg->seg = 1;  /* EC-1473: Fix for non 2 segs 3-mirrored */
		}
		topo_kfree(seg); // Unify into shallow_move() + change the above assignment of seg to shallow_move ???
		r1->replicas++;
		lock_ownership_update_raid_map(r1, ind_of_new, 'u');
		raid1_for_each_seg(r1, seg, si){
			BUG_ON(seg->toma_reg->seg != si);
		}
	}
_out:
	if (was_upgraded) {
		block_api_os_change_mirorring(nvmeibc_block_t_to_b(old_t));
		__nvmeibc_raid1_clear_old_topo_1st_tr(old_t, r1);
	}

	return was_upgraded;
}

#define __topo_dup_if_faild_ignore_msg(t1,t) \
	if (!t1) { /* Do not duplicate the topology twice */ \
		t1 = dup_topology(t);\
		if (!t1) {\
			rv = -ENOMEM;\
			goto ignore_msg;\
		}\
	}

/* Recofnigure raid1 (reconfigure its segments). Triggered by message from
   Toma, where it orders the client to use the replacement/upgrade/downgrade.
   Reconfiguration is done on duplicated new topology */
static int __raid1_try_to_apply_RUD(struct nvmeibc_subscription_ctx *tr, struct nvmeibt_client_msg *pl, int len)
{
	struct nvmeibc_topologies *nt = tr->nt;
	struct nvmeibc_topology *t = NULL, *t1 = NULL;
	struct nvmeibt_client_topo_praid *pl_raid1 =
			(struct nvmeibt_client_topo_praid *)pl->thick.data;
	struct nvmeibt_client_topo_disk_segment *seg_infos =
			(struct nvmeibt_client_topo_disk_segment *)pl_raid1->segs;
	const int tomas_replicas = pl_raid1->n_segments;
	int si, rv = 0;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg, *seg_rep;
	NFIN;
	(void)len;		/* Assume message is correct, envelope func verified it */
	BUG_ON(!spin_is_locked(&nt->lock));
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		rv = -ENODATA;
		goto ignore_msg;
	}
	r1 = __get_r1_by_tr(t, tr);
	if (tomas_replicas < r1->replicas) { /* raid1->seg downgrade */
		raid1_for_each_seg(r1, seg, si){
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
			if (!strncmp(seg->uuid, seg_infos[0].uuid, UUID_LEN))
				continue;
#ifndef __clang__
#pragma GCC diagnostic pop
#endif

			_NI_TOPO(trace_topology_raid1_try_to_apply_RUD, t, "downgrade request:(@N_SEGMENTS->@N_SEGMENTS) @SEGMENT_UUID->NULL",
			   r1->replicas, tomas_replicas, seg->uuid);
			seg_mark_toma_said_apply(seg);
			if (!seg_has_downgrade(seg)) {
				_NI_TOPO(trace_1_topology_raid1_try_to_apply_RUD, t, "toma switch req before reconf");
				rv = -EAGAIN; /* Unable to comply now */
				goto ignore_msg;
			}
			/*if (seg->perm) {
				_NT_SCOPE(__raid1_try_to_apply_RUD_t1, topology, "@STR: downgrade seg @STR active", nt->device_name, seg->uuid);
				rv = -EBUSY; // Return error and trigger warm relocation
				goto ignore_msg;
			}*/
			__topo_dup_if_faild_ignore_msg(t1, t);
			rv = __raid1_apply_downgrade(__get_r1_by_tr(t1, tr), false, t);
			if (rv)
				break;
		}
	} else if (tomas_replicas > r1->replicas) { /* seg->raid1 upgrade */
		seg = &r1->segments[0];
		_NI_TOPO(trace_2_topology_raid1_try_to_apply_RUD, t, "upgrade request:(@N_SEGMENTS->@N_SEGMENTS) @SEGMENT_UUID+@SEGMENT_UUID", r1->replicas, tomas_replicas, seg->uuid, seg_infos[1].uuid);
		seg_mark_toma_said_apply(seg);
		if (!seg_has_upgrade(seg)) {
			_NI_TOPO(trace_3_topology_raid1_try_to_apply_RUD, t, "toma switch req before reconf");
			rv = -EAGAIN;
			goto ignore_msg;
		}
		/* Note: Unlike downgrade, it is legal that: is_seg_active(seg) */
		__topo_dup_if_faild_ignore_msg(t1, t);
		rv = __raid1_apply_upgrade(__get_r1_by_tr(t1, tr), false, t);
	} else if ((r1->replicas>1) &&
			   !strncmp(r1->segments[0].uuid, seg_infos[1].uuid, UUID_LEN)){
		_NI_TOPO(trace_4_topology_raid1_try_to_apply_RUD, t, "castle request");
		__topo_dup_if_faild_ignore_msg(t1, t);
		__nvmeibc_raid1_castle_segs(__get_r1_by_tr(t1, tr));
	} else {			/* segment relocation */
		raid1_for_each_seg(r1, seg, si){
			if (!strncmp(seg->uuid, seg_infos[si].uuid, UUID_LEN))
				continue;

			_NI_TOPO(trace_5_topology_raid1_try_to_apply_RUD, t, "relocate request: @SEGMENT_UUID->@SEGMENT_UUID", seg->uuid, seg_infos[si].uuid);
			seg_mark_toma_said_apply(seg);
			if (!seg_has_replacement(seg)) {
				_NI_TOPO(trace_6_topology_raid1_try_to_apply_RUD, t, "toma switch req before reconf");
				rv = -EAGAIN;
				goto ignore_msg;
			}
			/*if (seg->perm) {
				_NT_SCOPE(__raid1_try_to_apply_RUD_t2, topology, "@STR: deprec seg @STR active", nt->device_name, seg->uuid);
				rv = -EBUSY; // Return error and trigger warm relocation
				goto ignore_msg;
			}*/
			__topo_dup_if_faild_ignore_msg(t1, t);
			seg_rep = &__get_r1_by_tr(t1, tr)->segments[si];
			rv  = __segment_apply_reconf(seg_rep, t, 'r');
			/* Don't register the replacement yet. Next switch topo will do it*/
			if (rv)
				break;
		}
	}
ignore_msg:
	if (t1)
		__set_active_topology(nt, t1, t);

	nvmeibc_topology_put(t);
	NFOUT;
	return rv;
}

/* Currently supports only warm removal, no hot. */
static int __chunk_apply_removal(struct nvmeibc_topology *t,
								 struct nvmeibc_topology *old_t)
{
	int c, r, si, diff = t->nchunks - t->resize.nchunks;
	struct nvmeibc_disk_segment *seg;
	struct nvmeibc_raid1 *r1;
	_NI_TOPO(trace_topology_chunk_apply_removal, t, "chunk removal @N_CHUNKS->@N_CHUNKS, force=@FORCE",
			t->nchunks-1, t->resize.nchunks-1, 1);
	/* Prev topo is the last one who needs the segs of the chunk. Redirect toma
	   mesg's. Chunks segs Will be free() as part of old_t */
    for (c = __get_topo_num_chunks(t)-diff; c < __get_topo_num_chunks(t); c++) {
		struct nvmeibc_chunk *chunk = &t->chunks[c];
		chunk_for_each_raid1(chunk, r1, r){
			raid1_for_each_seg(r1, seg, si){
				__seg_prepare_for_free(__get_seg_by_tr(old_t, seg->toma_reg), 0);
				__seg_free(seg);
			}
			topo_kfree(r1->segments);
			nvmeibc_raid_topo_persistent_put_ref(r1->hdr);
		}
		topo_kfree(chunk->raid1s);
		chunk->raid1s = NULL;	/* Convert to dummy chunk */
	}
	t->nchunks = t->resize.nchunks;
	t->resize.chunks = NULL;
	t->resize.nchunks = 0;
	nvmeibc_topo_update_size_of_bdev(t);
	__topo_set_resizing_to(t, 0);	/* Terminate resizing process */
	return 0;
}

static int __chunk_apply_addition(struct nvmeibc_topology *t,
								  struct nvmeibc_topology *old_t, bool force_unrequested)
{
	/* Merge old chunks and new chunks. */
	int c, r, si, rv = 1, diff = t->resize.nchunks - t->nchunks;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;

	struct nvmeibc_chunk *new_chunks = t->resize.chunks;
	(void)old_t;
	_NI_TOPO(trace_topology_chunk_apply_addition, t, "chunk addition @N_CHUNKS->@N_CHUNKS, force=@FORCE",
			t->nchunks-1, t->resize.nchunks-1, force_unrequested);
	topo_for_each_seg(t, chunk, c, r1, r, seg, si){
		seg->chunk = &new_chunks[c]; /* Update seg's to point to the new arr*/
	}
	topo_for_each_chunk(t, chunk, c){
		new_chunks[c] = *chunk; /* Move old chunks to new array */
	}
	/* Note: new dummy empty chunk at the end already appended */
	t->nchunks = t->resize.nchunks;
	topo_kfree(t->chunks);			/* Free original chunks array*/
	t->chunks = new_chunks;
	t->resize.chunks = NULL;
	t->resize.nchunks = 0;
	topo_for_each_chunk(t, chunk, c){
		chunk->topology = t;
	}
	/* Subscribe only the segments of the new chunks. Much like adding new seg*/
	if (!force_unrequested){
		/* Hot transition, request registration. Once all segs registered we
		   will update volume size. To not loose a working topo */
		for (c=__get_topo_num_chunks(t)-diff; c<__get_topo_num_chunks(t); c++){
			chunk = &t->chunks[c];
			if (unlikely(!chunk->raid1s)) continue;
			chunk_for_each_seg(chunk, r1, r, seg, si){
				BUG_ON(seg->toma_reg == NULL);
				__segment_register(seg);
			}
		}
		/* Hot transition: Will update bdev size once we release nt->lock, all
		   toma register acks on last chunk arrive and last chunks is enabled */
		t->resize.n_last_disabled_chunks = diff;
	} else {
		/* else, don't register yet. Caller func which issued the force
		request will do that to all forced segments*/
		nvmeibc_topo_update_size_of_bdev(t);
	}
	__topo_set_resizing_to(t, 0);	/* Terminate resizing process */
	return rv;
}

/* Every seg which has a reconfiguration and toma previously requested
   switch-topo to to it will be updated (Entire topo is processed).
   if 'force_unrequested' forces even unrequested segments to be updated.
   Optional 'tr'. If given will process only this specific raid1 isntead of all
   Optional 'disk'. If given will process only raid1s on this disk.
   Warning: Forcing may lead client to applying new config before toma and
   having to wait for Toma without being able to issue IOs. */
static int __apply_raid1_reconf(struct nvmeibc_raid1 *r1,
			struct nvmeibc_topology *old_t, bool force_unrequested,
			struct nvmeibc_idisk *disk)
{
	int si, rv = 0;
	struct nvmeibc_disk_segment *seg;
	const bool is_non_ec = !nvmeibc_raid_is_ec(r1);

	if (disk && !__does_raid1_uses_disk(r1, disk))
		goto _out;
	if ((is_non_ec && __raid1_apply_downgrade(  r1, force_unrequested, old_t)>0)||
		(is_non_ec && __raid1_apply_upgrade(    r1, force_unrequested, old_t)>0)||
		(__raid1_apply_replacement(r1, force_unrequested, old_t)>0)){
		rv++;
		if (!force_unrequested){
			/* Toma requested to apply reconf in the past. We did it now
				so it is our responsibility to register if needed */
			raid1_for_each_seg(r1, seg, si){
				__segment_register(seg);
			}
		}
		/* else, don't register yet. Caller func which issued the force
		   request will do that to all forced segments*/
	}
_out:
	return rv;
}

static bool __can_hot_reconf_topo(struct nvmeibc_topology *old_t)
{
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	int c, r, si;

	if (chunk_has_removal(old_t))
		return false; 		/* Shrink can be only warm */

	topo_for_each_seg(old_t, chunk, c, r1, r, seg, si) {
		if (seg_has_downgrade(seg)) return false; // Downgrade can support HOT only if last seg in praid is removed
		if (seg_has_upgrade(  seg)) return false; // Upgrade   can support HOT only if last seg is praid is added
	}
	return true;
}

/* Assume: if 'disk' is given, nt->lock is locked. Otherwise, not locked */
static int __all_segments_apply_reconf(struct nvmeibc_topologies *nt,
					bool force_unrequested, struct nvmeibc_subscription_ctx *optional_tr, struct nvmeibc_idisk *disk)
{
	struct nvmeibc_topology *t = NULL, *old_t = NULL;
	int c, r, num_need_reconf = 0, num_reconfed = 0;
	const bool was_locked = (disk!=NULL);	/*!!! don't use spin_is_locked(&nt->lock);!!!*/
	unsigned long flags = 0;
	u64 last_chunk_resize;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;


	NFIN;
	if (!was_locked) 	/* Must be done under spinlock, Take if not taken */
		spin_lock_irqsave(&nt->lock, flags);
	old_t = nvmeibc_topology_get(nt);
	if (unlikely(!old_t)) {
		num_reconfed = -ENODATA;
		goto out;
	};
	num_need_reconf = old_t->num_reconfing_segs;
	last_chunk_resize = old_t->resize.size;
	if ((num_need_reconf==0)&&(last_chunk_resize==0))
		goto out;					/* Nothing to reconfigure */

	_NT_TOPO(trace_topology_all_segments_apply_reconf, old_t, "f=@BOOL_YN, tr=@TR disk=@DISK n_reconf=@N_RECONF lchunk=@CHUNK_LENGTH, oldtopo#@TOPO_DBG_ID",
	   force_unrequested, optional_tr, disk, num_need_reconf,
	   last_chunk_resize, old_t->debug_unique_index);
	if (!force_unrequested) {		// Requested HOT transition
		if (!__can_hot_reconf_topo(old_t)) {
			num_reconfed = -EPERM;
			goto out;				/* Must be forced! No IO allowed */
		}
	}

	t = dup_topology(old_t);
	if (unlikely(!t)) {
		num_reconfed = -ENOMEM;
		goto out;
	}

	if (chunk_has_removal(t)){
		__chunk_apply_removal(t,old_t);
	} else if (chunk_has_addition(t)) {
		if (__chunk_apply_addition(t, old_t, force_unrequested) < 0) {
			num_reconfed = -ENOMEM;
			goto out;
		}
	}
	if (t->num_reconfing_segs == 0)
		goto _end_reconf;

	if (optional_tr) {
		r1 = __get_r1_by_t(t, optional_tr->ch, optional_tr->r1);
		num_reconfed+=__apply_raid1_reconf(r1, old_t, force_unrequested, disk);
	} else {
		topo_for_each_raid1(t, chunk, c, r1, r){
			num_reconfed+=__apply_raid1_reconf(r1, old_t, force_unrequested, disk);
		}
	}
_end_reconf:
	__set_active_topology(nt, t, old_t);
out:
	if (!was_locked)
		spin_unlock_irqrestore(&nt->lock, flags);
	if (likely(num_reconfed >= 0)) {
		_NI_TOPO(trace_1_topology_all_segments_apply_reconf, old_t, "Reconfed=@RECONFED out of @NUM_NEED_RECONF segments", num_reconfed, num_need_reconf);
	}
	else {
		_NE_TOPO(error_topology_all_segments_apply_reconf, old_t, "err=@ERR", num_reconfed);
	}
	nvmeibc_topology_put(old_t);
	NFOUT;
	return num_reconfed;
}

/* Typically called on error, (deletes all configuration changes) */
static void __all_segments_delete_replacements(struct nvmeibc_topology *t)
{
	struct nvmeibc_disk_segment *replacement = NULL;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	int c, r, si;

	NFIN;
	_NT_TOPO(t_21_toporep, t, "Cleaning up ALL replacement segments");
	topo_for_each_seg(t, chunk, c, r1, r, seg, si){
		if (!seg_has_replacement(seg) && !seg_has_upgrade(seg))
			continue;	/* No replacement and no upgrade */

		replacement = seg->replacement;
		if (is_toma_reg_valid(replacement->toma_reg)) {
			__seg_prepare_for_free(replacement, 0);
			__seg_free(replacement); /* Not part of any topo so free here */
		}
		topo_kfree(replacement);
		seg->replacement = NULL;
		t->num_reconfing_segs--;
	}
	NFOUT;
}

/* Append to upgrade/replacement configuration change */
static int __append_raid1_reconf_to_seg(struct nvmeibc_disk_segment *old_seg, struct nvmeibc_disk_segment *new_seg, char act)
{
	int rv = -ENOMEM;
	struct nvmeibc_disk_segment *replacement = NULL;
	const struct nvmeibc_subscription_ctx *tr = old_seg->toma_reg;
	int n_segs_to_alloc = ((act=='u') ? 2 : 1 ); /* 2-upgrade, 1-relocation*/
	if ((act=='r')&&(!strncmp(new_seg->uuid, old_seg->uuid, UUID_LEN))){
		rv = 0;		/* Nothing to do. Segment is not relocating anywhere */
		goto _out;
	}

	replacement = topo_kzalloc(sizeof(*replacement)*n_segs_to_alloc, GFP_ATOMIC);
	if (!replacement) {
		_NETR(t_22_toporep, "No mem for reconfiguration");
		goto _out;
	}

	__seg_shallow_copy_unsafe(replacement, new_seg);
	replacement->chunk = NULL;	//Important! Does not have raid1 (set a trap for incorrect usage)

	replacement->replacement = old_seg->replacement;	// old_seg->rep can store markers
	old_seg->replacement = replacement;
	rv = 1;
_out:
	return rv;
}

static int __append_ec_reconf(struct nvmeibc_raid1 *old_r1,
                                    struct nvmeibc_raid1 *new_r1 )
{   /* Todo: Unify with __append_raid1_reconf case of segment relocation */
	int res, si, rv = 0;
	struct nvmeibc_disk_segment *old_seg;
	NFIN;
    BUG_ON(false == nvmeibc_raid_is_ec(old_r1) || false == nvmeibc_raid_is_ec(new_r1)); //vcfg@"on update, previous raid definition and current raid definition should be logically same;"
    BUG_ON(old_r1->replicas != new_r1->replicas || old_r1->slice_size != new_r1->slice_size);

    raid1_for_each_seg(old_r1, old_seg, si){
        res = __append_raid1_reconf_to_seg(old_seg, &new_r1->segments[si], 'r');
        if (res<0) {
            rv = res;
            goto _out;
        }
        rv += res;
    }

_out:
	NFOUT;
	return rv;
}

/* Append any type of configuration change of raid1. */
static int __append_raid1_reconf(struct nvmeibc_raid1 *old_r1, struct nvmeibc_raid1 *new_r1 )
{
	int res, si, rv = 0;
	struct nvmeibc_disk_segment *seg;
	int mirroring = old_r1->replicas * 0x10 + new_r1->replicas;
	switch (mirroring) { /*EC-1473: Fix for non 2 segs,3-mirroring*/
		case 0x12: /* Upgrade */
			raid1_for_each_seg(new_r1, seg, si){
				if (strncmp(old_r1->segments[0].uuid, seg->uuid, UUID_LEN)){
					res = __append_raid1_reconf_to_seg(&old_r1->segments[0], seg, 'u');
					if (res<0) {
						rv = res;
						goto _out;
					}
					/* si is important for order of segments in the raid1 */
					seg_mark_for_upgr(&old_r1->segments[0], si);
					rv += res;
				}
			}
			break;
	case 0x21: /* Downgrade */
			raid1_for_each_seg(old_r1, seg, si){
				if (strncmp(seg->uuid, new_r1->segments[0].uuid, UUID_LEN)){
					seg_mark_for_down(seg);
					rv += 1;
				}
			}
			break;
	case 0x11:
	case 0x22: /* Segment relocation */
			raid1_for_each_seg(old_r1, seg, si){
				res = __append_raid1_reconf_to_seg(seg, &new_r1->segments[si], 'r');
				if (res<0) {
					rv = res;
					goto _out;
				}
				rv += res;
			}
			break;
	default:
		_NE_SCOPE(error_topology_append_raid1_reconf, topology, "Wrong configuration of praid rv=@RV", mirroring);
		rv = -EPERM;
		break;
	}
_out:
	return rv;
}

static int __append_raid_reconf(struct nvmeibc_raid1 *old_r1, struct nvmeibc_raid1 *new_r1 )
{
    if (nvmeibc_raid_is_ec(old_r1)){
        return __append_ec_reconf(   old_r1, new_r1);
    } else {
        return __append_raid1_reconf(old_r1, new_r1);
    }
}

/* For relocating segments, for each deprecated seg, append its replacement.
   For upgrade/downgrade of raid1s appends according marks & mem allocations
   Return: Amount of reconfigured segments. 0 is legal. Errors are negative */
static int __append_conf_change_of_topo(struct nvmeibc_topology *old_t, struct nvmeibc_topology *new_t)
{
	int c, r, rv = 0;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *new_r1;

	/* Test that all segments in chunks are of equal length. All segs in chunk have identical length so it is enough to test the first seg only */
	topo_for_each_chunk(new_t, chunk, c) {
		const u64 new_len =           chunk->raid1s->segments->length;
		const u64 old_len = old_t->chunks[c].raid1s->segments->length;
		if (new_len != old_len) { //vcfg@"all chunk segments should have same length
			_NT_SCOPE(warn_topology_append_conf_change_of_topo, topology, "Unsupported seg length change @LLU-->@LLU", old_len, new_len);
			rv = -EFAULT;
			goto out;
		}
	}
	topo_for_each_raid1(new_t, chunk, c, new_r1, r){
		struct nvmeibc_raid1 * old_r1= __get_r1_by_t(old_t, c, r);
		rv =  __append_raid_reconf(old_r1, new_r1);
		if (rv<0) {
			__all_segments_delete_replacements(new_t);
			goto out;
		}
		old_t->num_reconfing_segs += rv;
	}
out:
	if (rv>=0)
		rv = old_t->num_reconfing_segs;
	NFOUT;
	return rv;
}

/* Extend the device by appending chunks or shortenning by last chunks.
   Return: Amount of reconfigured chunks*10000. Errors are negative*/
static int __append_shrink_grow_topo(struct nvmeibc_topologies *nt, struct nvmeibc_topology *old_t, struct nvmeibc_topology *new_t)
{
	const int n_old_chunks = __get_topo_num_chunks(old_t);
	const int n_new_chunks = __get_topo_num_chunks(new_t);
	int diff = n_new_chunks-n_old_chunks, rv = -EFAULT, c;
	if (diff < 0){		/* Shrinking the volume by a few last chunks */
		diff = -diff;	// Return is possitive regardless of sign
		chunk_mark_for_removal(old_t);
	} else /*(diff > 0)*/{	/* Expand volume by adding at least one chunk */
		/* No space in original chunks array. Allocate new array */
		old_t->resize.chunks = topo_kzalloc(sizeof(*new_t->chunks) * new_t->nchunks,
									   GFP_ATOMIC);
		if (!old_t->resize.chunks) {
			rv = -ENOMEM;
			goto _out;
		}
		for (c = n_old_chunks; c < n_new_chunks+1; ++c) { /* Copy including dummy chunk */
			if (deep_copy_topo_chunk_unsafe(&old_t->resize.chunks[c], new_t, c)){
				rv = -ENOMEM;
				goto _out;
			}
		}
	}
	old_t->resize.nchunks = new_t->nchunks;
	__topo_set_resizing_to(old_t, new_t->chunks[n_new_chunks].first_vlba);
	rv = 10000 * diff;
_out:
	if (rv == -ENOMEM)
		_NE_SCOPE(error_topology_append_shrink_grow_topo, topology, DMESG_PREFIX("@DEV_NAME") ": No memory", nt->device_name);
	return rv;
}

int nvmeibc_calc_and_append_conf_diffs(struct nvmeibc_topologies *nt,
		struct nvmeibc_topology *new_t)
{
	struct nvmeibc_topology *old_t;
	unsigned long flags;
	int rv = 0;
	const u64 new_ver = new_t->configuration_version;
	NFIN;

	spin_lock_irqsave(&nt->lock, flags);
	old_t = nvmeibc_topology_get(nt);
	if (unlikely(!old_t)) {
		rv = -ENODATA;
		goto _out;
	}
	if (new_ver > old_t->configuration_version + 1){ //vcfg@"mgmt volume version & client volume version check"
		_NT_SCOPE(trace_topology_nvmeibc_calc_and_append_conf_diffs, topology, "Serious problem! missed configurations [@C_VOL_VER..@C_VOL_VER], rebooting!",
		   (int)old_t->configuration_version+1, (int)new_ver-1);
		rv = -EINVAL;
		goto _out;
	}
	if (old_t->configuration_version >= new_ver){
		_NT_SCOPE(trace_1_topology_nvmeibc_calc_and_append_conf_diffs, topology, "Ignoring reconf @C_VOL_VER. Already have it!", new_ver);
		WARN_ON(old_t->configuration_version > new_ver); /* How ???*/
		goto _out;	/* Probably same reconfiguration was sent a few times */
	}

	if (__get_topo_num_chunks(old_t) != __get_topo_num_chunks(new_t)) {
		rv = __append_shrink_grow_topo(nt, old_t, new_t);
	} else if (new_t->stripe_width != old_t->stripe_width) { //vcfg@"chunk number of praids cannot change, unless we are handling shrink"
		_NT_SCOPE(trace_2_topology_nvmeibc_calc_and_append_conf_diffs, topology, "Serious problem! Illegal reconfiguration: stripe width changed!");
		rv = -EFAULT;
	} else {
		rv = __append_conf_change_of_topo(old_t, new_t);
	}
	WARN(old_t->newer, "nvmeibc bug! incorrect reconfig\n"); // Daniel: appending diffs on head topo does not create newer head
	if (rv>=0)
		old_t->configuration_version = new_ver;

_out:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(old_t);
	NFOUT;
	return rv;
}

int nvmeibc_subscribe_requested_conf_diffs(struct nvmeibc_topologies *nt)
{
	/* Note: Daniel, This function has a huge race condition!
		1. This function cannot spin_lock_irqsave(&nt->lock, flags); coz core does not allow that
		2. But then dup_topo can run concurrently and move some of the diffs to a newer topology.
		Bugs: EC-2770, EXC-2433, EC-6631. There is no quick solution, will be auto solved as part of EXC-2433 */
	struct nvmeibc_topology *t = nvmeibc_topology_get(nt);
	int rv;
	// nvmeibc_topologies_duplicate(nt, NULL);		// Uncomment to imemdiately cause EC-6631
	rv = __subscribe_all_segs(t);
	WARN_TOPO(t->phased_out, t, "nvmeibc bug: Reproduction of EC-6631. System may crash!\n");
	nvmeibc_topology_put(t);	/* t may be free'd here */
	return rv;
}

void nvmeibc_topology_force_replace_t_act(struct nvmeibc_topologies *nt,
										  struct nvmeibc_topologies *new_conf_t)
{
	struct nvmeibc_topology *t = list_first_entry(&new_conf_t->topologies,
										struct nvmeibc_topology, list_n);
	t->nt = nt;
	__subscribe_all_segs(t);
}

void nvmeibc_topology_force_replace_t_inj(struct nvmeibc_topologies *nt,
										  struct nvmeibc_topologies *new_conf_t)
{
	unsigned long flags;
	struct nvmeibc_topology *t = list_first_entry(&new_conf_t->topologies,
										struct nvmeibc_topology, list_n);
	spin_lock_irqsave(&nt->lock, flags);
	BUG_ON(!list_empty(&nt->topologies));
	topos_list_update_begin(nt);
	nt->boot_state = NVMEIBC_TOPO_BOOT_APPLIED_NEW;
	nt->topo_debug_unique_index_generator = 1;
	list_move(&t->list_n, &nt->topologies);
	nvmeibc_topo_update_size_of_bdev(t); // No need for __set_error_state()
	topos_list_update_end(nt);     // Now get_topo() by IO works
	spin_unlock_irqrestore(&nt->lock, flags);
}

void nvmeibc_topology_free_new_conf_t(struct nvmeibc_topologies *next_conf)
{
	struct nvmeibc_topology *t = list_first_entry_or_null(
			&next_conf->topologies, struct nvmeibc_topology, list_n);
	__free_topology(t, false); /* t is null if configuration was applied */
	/* next_conf is allocated on stack so no free */
}

static int nvmeibc_warm_raid1_apply_conf_diffs(struct nvmeibc_topologies *nt, struct nvmeibc_subscription_ctx *tr)
{
	struct register_seg_outcome outcome = {0};
	NFIN;
	_NTTR(trace_topology_nvmeibc_warm_raid1_apply_conf_diffs, "Warm rereg of praid");
	__toma_unregister_raid1_by_seg(tr, NULL, NVMEIBT_CLIENT_RT_REASON_WARM_UNREGISTER);
	__all_segments_apply_reconf(nt, true /* force_unrequested*/, tr, NULL );
	outcome = __toma_register_raid1_by_seg(tr, NVMEIBT_CLIENT_RT_REASON_WARM_REGISTER);
	NFOUT;
	return outcome.rv;
}

int nvmeibc_warm_apply_conf_diffs(struct nvmeibc_topologies *nt)
{
	int rv = 0;
	NFIN;
	_NT_SCOPE(trace_topology_nvmeibc_warm_apply_conf_diffs, topology, "@DEV_NAME: Warm reconf fallback", nt->device_name);
	__unregister_all_segments_io_is_possible(nt);
	__all_segments_apply_reconf(nt, true /* force_unrequested*/, NULL, NULL );
	rv = __register_all_segments(nt);
	NFOUT;
	return rv;
}

int nvmeibc_apply_requested_conf_diffs( struct nvmeibc_topologies *nt){
	return __all_segments_apply_reconf(nt, false /* do not force*/, NULL, NULL);
}

/*****************************************************************************/
void nvmeibc_segment_clear_b4_reg(struct nvmeibc_disk_segment *seg, const char*debug_reason)
{
	(void)debug_reason; //if (seg_has_toma_said_apply(seg)) {if (debug_reason) BUG();}
	seg_unmark_toma_said_apply(seg); /* Toma will reply to us with instructions*/
}

/* Daniel: This is incorrect that 'register' is sent right away. In most cases it
   should be scheduled like rest of the messages to introduce order of messages.
   Otherwise we bump into lock-id mess and can stall IO for long time */
static int __segment_register(struct nvmeibc_disk_segment *seg)
{
	int rv = 0;
	const struct nvmeibc_subscription_ctx *tr = seg->toma_reg;

	NFIN;
	if (seg->registration_status)
		goto _out;		/* No need to register, Even if we are not registered vs caser, the process is already ongoing */
	BUG_ON(!tr);  		/* Should be already subscribed */
	nvmeibc_segment_clear_b4_reg(seg, NULL);
	_NT_TOPO(trace_topology_segment_register, seg->chunk->topology, "toma_register: disk=@DISK,@DISK_NAME seg=@SEG " SEGMENT_FMT " c_lid=@C_LID",
	   seg->disk, seg->disk->ops.get_full_name(seg->disk), seg->uuid,
	   tr->ch, tr->r1, tr->seg, nvmeibc_disk_segment_get_praid(seg)->lid.all);
	rv = nvmeibc_toma_send_direct_msg(seg,
						NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT, NULL);
_out:
	if (unlikely(rv)) { /* Underlying assumption - A future cont on this disk
		* will invoke registration on all failed REG segments */
	}
	NFOUT;
	return rv;
}

int nvmeibc_topologies_conf_register_mismatch_segs(struct nvmeibc_topologies*nt)
{
	unsigned long flags;
	struct nvmeibc_topology *t = NULL;
	int c, r, si, rv = 0;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;

	NFIN;
	spin_lock_irqsave(&nt->lock, flags);
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		spin_unlock_irqrestore(&nt->lock, flags);
		return -ENODATA;
	}
	topo_for_each_seg(t, chunk, c, r1, r, seg, si) {
		if ((!seg_has_replacement(seg))&&(seg_has_toma_said_apply(seg))&&
			(!seg->registration_status)&&(seg->toma_acm != NVMEIBTC_DS_MODE_DEAD)) {
			__segment_register(seg);
		}
	}
	nvmeibc_topology_put(t);
	spin_unlock_irqrestore(&nt->lock, flags);
	NFOUT;
	return rv;
}

int nvmeibc_topologies_register(struct nvmeibc_topologies *nt)
{
	return __register_all_segments(nt);
}

int nvmeibc_topologies_init(struct nvmeibc_topologies *nt, const char *device_name)
{
	int rv, i;

	INIT_LIST_HEAD(&nt->topologies);
	spin_lock_init(&nt->lock);

	nt->percpu =         topo_kzalloc(sizeof(struct topos_percpu)       * MAX_NUM_ACTIVE_CPUS, GFP_KERNEL);
	nt->percore_shared = topo_kzalloc(sizeof(struct topo_percore_shared)* MAX_NUM_ACTIVE_CPUS, GFP_KERNEL);	// Todo: Use nvmeib_public_alloc_percpu_cacheline() like c_disk allocates it percpu
	if (!nt->percpu || !nt->percore_shared) {
		_NE_to_user(error_topology_nvmeibc_topologies_init, DMESG_PREFIX("@DEV_NAME"), "Out of memory when trying to initialize a volume, detaching and attaching the volume will probably be needed when the memory shortage has been relieved. Error code: 1036.", device_name);
		rv = -ENOMEM;
		goto err;
	}
	for_each_allocated_cpu(i) {
		nt->percpu[i].ios_issued = 0;		// initialize per-cpu members
		spin_lock_init(&nt->percore_shared[i].list_access);	// initialize per-cpu shared members
		INIT_LIST_HEAD(&nt->percore_shared[i].io_wait_list);
		nt->percore_shared[i].n_wait_list = 0;
		nt->percore_shared[i].ios_completed = 0;
#ifdef DEBUG_PERCPU_ISSUED_IO_CNTRS
		nt->percore_shared[i].n_ios = 0;
#endif
	}
	nt->device_name = device_name;
	nt->topo_debug_unique_index_generator = 0;
	nt->dbg_num_enabling_io_toggles = 0;
	nt->dbg_disabling_ts = jiffies;				// Topologies are alwasy created in IO disabled state
	rv = 0;
err:
	if (unlikely(rv)) {
		if (nt->percpu)
			topo_kfree(nt->percpu);
		if (nt->percore_shared)
			topo_kfree(nt->percore_shared);
	}
	return rv;
}

#include "nvmeibc_mcs_stub.h"							// Configuration of volume/block device
#define get_allocation_index(nsegs, p, s) \
	(nsegs * p->stripeIndex + s->pRaidTypeIndex + ((s->type == TYPE_PARITY) ? p->dataBlocks : 0))

/* Sort the chunks by their virtual (block_devices) addresses. */
static void __sort_chunks(struct nvmeibc_topology *t)
{
	int i, j, nchunks = __get_topo_num_chunks(t);
	struct nvmeibc_chunk temp;
	for (i = 0; i < nchunks; i++) {
		for (j = i + 1; j < nchunks; j++) {
			if (t->chunks[i].first_vlba > t->chunks[j].first_vlba) {
				temp = t->chunks[i];
				t->chunks[i] = t->chunks[j];
				t->chunks[j] = temp;
			}
		}
	}
	return;
}

/* Allocate raid1s and segments in each chunk, and sort the chunks
 * Returns the size of block device (in blocks). 0 or neg in case of error. */
static u64 __digest_chunk_layout(struct nvmeibc_volume_conf *conf, struct nvmeibc_topology *t, int *nsegs)
{
	int na = 0, i, c, pr, s, slice_size, stripe_size, n_segs = 0;
	u64 next_first_vlba = 0, rlbas;

	NFIN;
	t->max_n_segs_in_praid = 0;
	_ND_TOPO(trace_topology_digest_chunk_layout, t, "nchunks = @N_CHUNKS", __get_topo_num_chunks(t));
	/* We will check only the first stripe index - it stores enough info */
	for (c=0;c<conf->n_chunks;c++) {
		const struct nvmeibc_chunk_conf *cur_chunk = &conf->chunks[c];
		for (pr=0;pr<cur_chunk->n_praids;pr++) {
			const struct nvmeibc_praid_conf *cur_praid = &cur_chunk->praids[pr];
			const int n_max_segs = cur_praid->numberOfMirrors + cur_praid->dataBlocks + cur_praid->parityBlocks;
			n_segs += cur_praid->n_segments;
			for (s=0;s<cur_praid->n_segments;s++) {
				const struct nvmeibc_segment_conf *cur_seg = &cur_praid->segments[s];
				if (get_allocation_index(n_max_segs, cur_praid, cur_seg) != 0)
					continue;
				stripe_size = __get_valid_stripe_size(cur_chunk, cur_praid, c);
				if (stripe_size <= 0){
					goto _err;
				}
				slice_size = cur_praid->dataBlocks; //Slice size is in the number of data segs
				t->chunks[na].stripe_size = stripe_size;
				t->chunks[na].first_vlba = cur_chunk->vlbs << MGMT2CLNT_SHIFT;
				t->chunks[na].topology = t;
				rlbas = cur_chunk->vlbs + slice_size * calc_segment_length(cur_seg) * cur_chunk->stripeWidth;
				rlbas <<= MGMT2CLNT_SHIFT;
				if (next_first_vlba < rlbas)
					next_first_vlba = rlbas;

				t->chunks[na].raid1s = topo_kzalloc(sizeof(struct nvmeibc_raid1)*__get_num_r1s(t), GFP_ATOMIC);
				if (!t->chunks[na].raid1s)
					goto _err;
				for (i = 0; i<__get_num_r1s(t); i++) {
					struct nvmeibc_raid1 *r1 = __get_r1_by_t(t,na,i);
					r1->replicas = n_max_segs;
					r1->slice_size = slice_size;
					r1->use_rdma_locks = (!nvmeibc_raid_is_jbod(r1));
					r1->toma.conversation_ind = 0x1ULL;		// Arbitrary starts from 1. 0 is illegal value
					r1->hdr = nvmeibc_raid_topo_persistent_create();
					nvmeibc_locks_scheme_build(&r1->lock_scheme, &cur_praid->lockServer);
					r1->segments = topo_kzalloc(sizeof(struct nvmeibc_disk_segment)*n_max_segs, GFP_ATOMIC);
					if ((!r1->hdr) || (!r1->segments))
						goto _err;
					MAX_WITH(t->max_n_segs_in_praid, n_max_segs); // In actual product all praid has the same structure, so this code can be simplified to jsut copy the value from prev topology or take it from first praid
					nvmeibc_raid1_clear_new_topo_upon_unreg(r1);
				}
				na++;
			}
		}
	}
	BUG_ON(na != __get_topo_num_chunks(t));
	/* Append dummy empty chunk at the end */
	t->chunks[na].first_vlba = next_first_vlba;
	t->chunks[na].raid1s = NULL;
	__sort_chunks(t);
_out:
	NFOUT;
	*nsegs = n_segs;
	return next_first_vlba;

_err:
	_NE_TOPO(error_1_topology_digest_chunk_layout, t, "No memory for processing");
	next_first_vlba = 0; /* Not! -ENOMEM, this is unsigned */
	goto _out;
}

static bool __topo_is_valid_seg_pos(struct nvmeibc_topology *topo,
									int ci, int ri, int si)
{
	if ((ci < topo->nchunks) &&
		(ri < topo->stripe_width) &&
		(si < topo->chunks[ci].raid1s[ri].replicas)) {
		return true;
	}
	return false;
}

static struct nvmeibc_idisk *get_disk_by_id(const char *diskID, struct list_head *disks
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
										   , struct nvmeib_io_stats **stats)
#else
											)
#endif
{
	struct nvmeibc_disk_id *disk;
	list_for_each_entry(disk, disks, link) {
		if (!strcmp(disk->name, diskID)) {
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
			*stats = disk->v_disk_stats;
#endif
			return &disk->disk->base;
		}
	}
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	*stats = NULL;
#endif
	return NULL;
}

static u32 __parse_seg_dbg_uuid(const char *uuid)
{
	int rv;
	long res;
	char buf[8 + 1];

	strlcpy(buf, uuid, ARRAY_SIZE(buf));	// Truncate to 8 characters
	if ((rv = kstrtol(buf, 16, &res))) {
		_NT_SCOPE(warn_nvmeibc_topology_parse_seg_dbg_uuid, topology, "nvmeibc bug! Cannot parse segment debug uuid from uuid, rv=@RV", rv);
		return (~0);	// an eye-catcher in traces
	}
	return (u32)res;
}

/* Digest all raid1s and segments and subsribe with toma */
static int __digest_segment_layout(struct nvmeibc_volume_conf *conf,
								   struct nvmeibc_topology *t, bool is_reconf,
								   struct list_head *disks)
{
	struct nvmeibc_disk_segment *seg = NULL;
	struct nvmeibc_raid1 *pr;
	int tsegs = 0; /* Segments added. */
	int ci, ri, si, c, r, s;
	int nchunks = __get_topo_num_chunks(t);

	int rv = -EDOM;
	for (c=0;c<conf->n_chunks;c++) {
		const struct nvmeibc_chunk_conf *cur_chunk = &conf->chunks[c];
		for (r=0;r<cur_chunk->n_praids;r++) {
			const struct nvmeibc_praid_conf *cur_praid = &cur_chunk->praids[r];
			const int n_max_segs = cur_praid->numberOfMirrors+cur_praid->dataBlocks+cur_praid->parityBlocks;
			for (s=0;s<cur_praid->n_segments;s++) {
				const struct nvmeibc_segment_conf *cur_seg = &cur_praid->segments[s];
				_NT_TOPO(trace_topology_digest_segment_layout, t, "segment: disk_id=@DISK_ID_STR, stripe_idx=@STRIPE_IDX, n_segs=@N_SEGMENTS, type=@TYPE, pRaidTypeIndex=@PRAIDTYPEINDEX",
				   cur_seg->diskID,
				   get_allocation_index(n_max_segs, cur_praid, cur_seg),
				   n_max_segs, cur_seg->type, cur_seg->pRaidTypeIndex);
				for (ci = 0; ci < nchunks; ci++) {
					if (t->chunks[ci].first_vlba != (cur_chunk->vlbs<<MGMT2CLNT_SHIFT))
						continue;
					tsegs++;
					ri = get_allocation_index(n_max_segs, cur_praid, cur_seg)/n_max_segs;
					si = get_allocation_index(n_max_segs, cur_praid, cur_seg)%n_max_segs;
					if (!__topo_is_valid_seg_pos(t, ci, ri, si)) {
						_NE_TOPO(error_topology_digest_segment_layout, t, SEGMENT_FMT " Invalid range >" SEGMENT_FMT "",
							ci, ri, si, t->nchunks, t->stripe_width, t->chunks[ci].raid1s[ri].replicas);
						return -EINVAL;
					}
					pr = __get_r1_by_t(t, ci, ri);
					seg = &pr->segments[si];
					strlcpy(seg->uuid, cur_seg->uuid, 37);
					seg->uuid[37] = '\0';	// Why? After strlcpy above seg->uuid is already NULL-terminated...
					seg->dbg_uuid = __parse_seg_dbg_uuid(seg->uuid);
					seg->disk = get_disk_by_id(cur_seg->diskID, disks
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
											   , &seg->v_disk_stats);
#else
												);
#endif
					if (!seg->disk){
						_NT_TOPO(trace_1_topology_digest_segment_layout, t, SEGMENT_FMT " target configuration missing disk @DISKID (@DISKUUID)",
							ci, ri, si, cur_seg->diskID, cur_seg->diskUUID);
						return -EINVAL;
					}

					seg->toma_acm = NVMEIBTC_DS_MODE_INVALID;
					seg->first_lba = (cur_seg->lbs << MGMT2CLNT_SHIFT);
					seg->length	= calc_segment_length(cur_seg) << MGMT2CLNT_SHIFT;
					seg->max_dma_size = 0;	// Unknown, yet
					seg->chunk = &(t->chunks[ci]);
					seg->sw_md_size = NVMEIBC_SGMNT_DEFAULT_MD_SIZE;

					if (!is_reconf){
						rv = __subscribe_seg(seg, ci, ri, si, t->nt, pr->hdr);
						_NT_TOPO(trace_3_topology_digest_segment_layout, t, SEGMENT_FMT ".uuid=@UUID (@RV)", ci, ri, si, seg->uuid, rv);
					} else { /* Will subscribe when reconfiguration is applied */
						seg->toma_reg = NULL; // was NULL due to topo_kzalloc()
						rv = 0;
					}

					if (unlikely(rv < 0)) {
						seg->chunk = NULL; /* Seg was not connected to volume */
						return rv;
					}
					break; /* Only one chunk represents address bd_start */
				}
			}
		}
	}
	_ND_TOPO(trace_4_topology_digest_segment_layout, t, "Segment configuration digested. na=@CHUNK_IDX, nsegs=@N_SEGMENTS", __get_topo_num_chunks(t), tsegs);
	return tsegs;
}

/* For now, assume it is valid, coz c_volume.c does all the needed checks */
static bool nvmeibc_block_disk__is_valid_config(struct nvmeibc_volume_conf *c)
{
	(void)c;
	return true;
}

/* Check the topology configuration objects for validity.
 * assume topology is sorted by volume block addresses */
static bool nvmeibc_topology__is_valid_config(struct nvmeibc_topology *topo)
{
	const int                   num_chunks = __get_topo_num_chunks(topo);
	struct nvmeibc_chunk        *chunk;
	struct nvmeibc_raid1        *raid;
	struct nvmeibc_disk_segment *seg = NULL;
	int ci, ri, si, rv = true; // all checks passed

	/* 1. Check that chunks are covering the whole volume address space
	 * 2. The topology contains only segments that are non-deprecated
	 * 3. All raids in each chunk have identical protection type Raid5/1/6 */
	for (ci=0, chunk=topo->chunks; ci < num_chunks; ci++, chunk++) {
		_NT_TOPO(trace_topology_nvmeibc_topology__is_valid_config, topo, "chunk @CI: first_vlba=@VLBA", ci, chunk->first_vlba);
		// verify the segments within the chunk.
		for (ri=0, raid = chunk->raid1s; ri < topo->stripe_width; ri++, raid++){
			BUG_ON(raid == NULL);
			for (si = 0; si < raid->replicas; si++) {
				seg = &raid->segments[si];
				if (seg->disk == NULL) {
					// A valid segment must point to the disk its located on.
					_NE_TOPO(error_1_topology_nvmeibc_topology__is_valid_config, topo, SEGMENT_FMT " missing segment", ci, ri, si);
					rv = false;
					goto done;
				}
				_ND_TOPO(trace_1_topology_nvmeibc_topology__is_valid_config, topo, SEGMENT_FMT "uuid=@UUID", ci, ri, si, seg->uuid);
			}
		}
	}
done:
	return rv;
}

/* prepare all segments in a topology for free if the segment is initialized */
static void __topology_prepare_all_segments_for_free(struct nvmeibc_topology *t){
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	int c, r, si;
	topo_for_each_seg(t, chunk, c, r1, r, seg, si){
		if (is_toma_reg_valid(seg->toma_reg))
			__seg_prepare_for_free(seg, 1);
	}
}

static void __print_incoming_topology(const struct nvmeibc_topology *t)
{
	char *buf = topo_kmalloc(PAGE_SIZE, GFP_KERNEL), *print_pos = buf;
	if (buf) {
		int len;
		struct nvmeib_txt txt = nvmeib_txt_make((struct charvec){.base = buf, .len = PAGE_SIZE});
		__topo_status_tostring(t, &txt);
		len = nvmeib_txt_finalize(&txt).len;
		_NI_TOPO(t_01_topo_pic, t, "Reconf Topo len=@LEN", len);
		while (true) {
			char *next = strchr(print_pos, '\n');
			if (next)
				*next = '\0';
			_NI_SCOPE(t_02_topo_pic, topology, "@BUF_STR", print_pos);
			if (next)
				print_pos = next + 1;
			else
				break;
		}
		topo_kfree(buf);
	}
}

int nvmeibc_topology_update_configuration(struct nvmeibc_topologies *nt,
	struct nvmeibc_volume_conf *conf, int version, bool is_update, struct list_head *disks)
{
	int nsegs, rv = 0, tsegs;
	u64 next_first_vlba = 0;
	struct nvmeibc_topology *t = NULL;

	_ND(t_01_cbconf, "Utilizing @INT segs as {D+P}={@INT+@INT}", conf->numberOfMirrors, conf->dataBlocks, conf->parityBlocks);
	// validate the new configuration (config objects) rv == number of chunks
	if (!nvmeibc_block_disk__is_valid_config(conf)) {
		rv = -EINVAL;
		_NE_SCOPE(error_topology_nvmeibc_topology_update_configuration, topology, DMESG_PREFIX("@DEV_NAME") ": Invalid configuration, cannot update", nt->device_name);
		goto out;
	}

	t = topo_kzalloc(sizeof(*t), GFP_KERNEL);
	if (unlikely(!t)) {
		_NE_SCOPE(error_1_topology_nvmeibc_topology_update_configuration, topology, DMESG_PREFIX("@DEV_NAME") ": No memory", nt->device_name);
		rv = -ENOMEM;
		goto err;
	}
	t->nt = nt;
	t->debug_unique_index = (++t->nt->topo_debug_unique_index_generator);
	INIT_LIST_HEAD(&t->list_n);
	spin_lock_init(&t->delete_lock);
#ifdef DEBUG_TOPO_CNTRS
	INIT_LIST_HEAD(&t->dbg_tcntrs);
	spin_lock_init(&t->dbg_tcntrs_lck);
#endif

	/* Update topology header (constant per configuration) */
	t->configuration_version = version;
	nvmeibc_topo_init_io_perm(t);
	t->stripe_width = conf->stripeWidth;
	t->percpu = nvmeibc_topo_percpu_create();
	if (!t->percpu) {
		_NE_TOPO(error_2_topology_nvmeibc_topology_update_configuration, t, "No memory for topology percpu");
		topo_kfree(t);
		t = NULL;
		rv = -ENOMEM;
		goto err;
	}

	t->nchunks = conf->n_chunks + 1;			// Dummy empty chunk at the end
	t->chunks = topo_kzalloc(sizeof(struct nvmeibc_chunk)*t->nchunks, GFP_KERNEL);
	if (!t->chunks) {
		_NE_TOPO(error_3_topology_nvmeibc_topology_update_configuration, t, "No memory for chunks");
		t->nchunks = 0;
		rv = -ENOMEM;
		goto err;
	}

	/* In the first loop, we digest only the first stripe index */
	next_first_vlba = __digest_chunk_layout(conf, t, &nsegs);
	if (next_first_vlba == 0) {
		_NE_TOPO(error_4_topology_nvmeibc_topology_update_configuration, t, "Chunk digest failed: @NEXT_FIRST_VLBA", next_first_vlba);
		rv = -EIO;
		goto err;
	}

	_ND_TOPO(trace_topology_nvmeibc_topology_update_configuration, t, "Chunk configuration digested. na=@CHUNK_IDX, nsegs=@N_SEGMENTS", __get_topo_num_chunks(t), nsegs);

	if ((tsegs = __digest_segment_layout(conf, t, is_update, disks)) < 0) {
		_NE_TOPO(warn_topology_nvmeibc_topology_update_configuration, t, "Invalid config rejected. @C_VOL_VER", (u32)t->configuration_version);
		rv = -EIO;
		goto err1;
	}
	if (tsegs != nsegs) {
		_NE_TOPO(error_5_topology_nvmeibc_topology_update_configuration, t, "Number of configuration segments (@N_SEGMENTS) is different that number of digested segments (@TSEGS)", nsegs, tsegs);
		// Should this cause an error?
	}

	WARN_ON(conf->blocks == 0);

	// validate the new configuration over topology
	if (!nvmeibc_topology__is_valid_config(t)) {
		rv = -EINVAL;
		goto err1;
	}

	__topo_calc_chunk_binary_search_start(t);
	list_add(&t->list_n, &nt->topologies);
	__print_incoming_topology(t);
	rv = 0;
	goto out;
err1:
	/* Some segments are OK, some uninitialized. Undo the initizlized segs */
	__topology_prepare_all_segments_for_free(t);
err:
	__free_topology(t, true);
out:
	if (!is_update && likely(rv >= 0)) {
		_NI_TOPO(trace_topology_nvmeibc_topology_update_configuration_new, t, "+ (@TOPOLOGY)", t);
	}
	return rv;
}

bool nvmeibc_topology_manual_attempt_recover_list(struct nvmeibc_topologies *nt)
{
	struct list_head *start = &nt->topologies, *cur = start->next;
	struct nvmeibc_topology* t = NULL;
	int j = 0, i, sum;
	unsigned long flags = 0;
	bool is_list_stuck = true;

	NFIN;
	/* stuck list is: all topo has one reference, the last has 0 */
	spin_lock_irqsave(&nt->lock, flags);
	atomic_set(&nt->need_update, 1);
	_NT_SCOPE(ctmarl01, topology, "@DEV_NAME: topo list recover attempt: start=@START_PTR, nt->last_freed=@INT64", nt->device_name, start, nt->topo_debug_last_freed_version);
	for (j = 0; cur!=start; cur = cur->next, j++) {
		t = list_entry(cur, struct nvmeibc_topology, list_n);
		sum = nvmeibc_topo_get_topo_users(t);
		is_list_stuck &= (sum < 2); /* 1 or zero */
		_NT_TOPO(ctmarl02, t, "@INDEX) p_out=@P_OUT sum=@SUM", j, t->phased_out, sum);
	}
	_ND(ctmarl03, "topo list end");
	{ // For deubg print the per_cpu of 'nt'
		sum = 0;
		for_each_allocated_cpu(i) {
			const int n_readers = nt->percpu[i].nt_users;
			sum += n_readers;
			if (n_readers != 0) {
				_NT_SCOPE(ctmarl04, topology, "@DEV_NAME: nt->percpu[@INDEX].nt_users=@INT", nt->device_name, i, n_readers);
			}
		}
		_NT_SCOPE(ctmarl05, topology, "@DEV_NAME: nt->percpu[sum].nt_users=@SUM", nt->device_name, sum);
	}
	atomic_set(&nt->need_update, 0);
	spin_unlock_irqrestore(&nt->lock, flags);

	if ((j > 0) && is_list_stuck) {
		t = list_entry(start->prev, struct nvmeibc_topology, list_n);
		_NT_TOPO(ctmarl06, t, "is indeed stuck. per_cpu counters:"); 	// Print the percpu readers
		spin_lock_irqsave(&t->delete_lock, flags);
		for_each_allocated_cpu(i) {
			_NT_TOPO(ctmarl07, t, "t->percpu[@INDEX].t_users=@INT", i, t->percpu[i].t_users);
		}
		sum = nvmeibc_topo_get_topo_users(t);
		is_list_stuck = (sum <= 0); /* last topo zero refs, or negative in case of a bug */
		if (is_list_stuck) {	/* Adjust ref count to be able to free it */
			const int cpu_idx = get_cpu(), add_ref = ((-sum) + 1);
			_NT_TOPO(ctmarl09, t, "t->percpu[@INDEX].t_users += @INT", cpu_idx, add_ref);
			t->percpu[cpu_idx].t_users += add_ref;		// Force refcount sum to be 1
			put_cpu();
		}
		spin_unlock_irqrestore(&t->delete_lock, flags);
		if (is_list_stuck) {	/* Force deletion */
			nvmeibc_topology_put(t);						// Now topos should free
			_NT_SCOPE(ctmarl0a, topology, "@DEV_NAME: topo list recover result: nt->last_freed=@INT64", nt->device_name, nt->topo_debug_last_freed_version);
			is_list_stuck = ((nt->topo_debug_last_freed_version + 1) < nt->topo_debug_unique_index_generator); /* recovery failed */
		}
	} else {
		is_list_stuck = false; /* empty list */
	}
	NFOUT;
	return !is_list_stuck;
}

/* Unregister the entire device. If IO still possible, schedule unregistration, otherwise immediately unregister */
static int __unregister_all_segments_io_is_possible(struct nvmeibc_topologies *nt)
{
	struct nvmeibc_topology *t = NULL;
	int c, r, si;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&nt->lock, flags);
	t = nvmeibc_topology_get(nt);
	if (likely(t)) {
		topo_for_each_raid1(t, chunk, c, r1, r) { /* Unregister all segments of praid, by force */
			for (si = 0, seg = r1->segments; (si < r1->replicas)&&(!is_seg_active(*seg)); ++seg, ++si){
				; // Find first active seg, note: during segment replacement unregistered seg might have his 'seg->toma_reg' freeing
			}
			if (si < r1->replicas)
				__toma_unregister_raid1_by_seg(seg->toma_reg, NULL, NVMEIBT_CLIENT_RT_REASON_UNREG_ALL_XXX);
			else
				_NT_SCOPE(t_01_topounreg, topology, "@DEV_NAME" SEGMENT_FMT ": @TOPO_DBG_ID skipped!", nt->nd->name, c, r, si, t->debug_unique_index);
		}
	}

	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
	NFOUT;
	return 0;
}

static int __unregister_all_segments_on_cleanup_no_io(struct nvmeibc_topologies *nt)
{
	struct nvmeibc_topology *t = NULL;
	int c, r, si;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&nt->lock, flags);
	t = __nvmeibc_topology_get_regardless_boot_state(nt);
	if (likely(t)) {
		WARN_ON(t->newer); /* This is the only topology in the system */
		topo_for_each_raid1(t, chunk, c, r1, r) {
			raid1_for_each_seg(r1, seg, si) {
				on_topo_free_schedule_seg_msg_no_io(seg, NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT, NVMEIBT_CLIENT_RT_REASON_DIRECT);
			}
			__nvmeibc_raid1_clear_old_topo_1st_tr(t, r1);	// Same as done when dup_topo() and schedule unreg
			nvmeibc_raid1_clear_new_topo_upon_unreg( r1);   // Just for debug. raid not used anymore
		}
	}
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
	NFOUT;
	return 0;
}

/* Inverse of __unregister_all_segments() */
static int __register_all_segments(struct nvmeibc_topologies *nt){
	struct nvmeibc_topology *t = NULL;
	int c, r, si;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	struct register_seg_outcome outcome = {0};

	NFIN;
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		return -ENODATA;
	}

	topo_for_each_seg(t, chunk, c, r1, r, seg, si){
		outcome = __toma_register_raid1_by_seg(seg->toma_reg, NVMEIBT_CLIENT_RT_REASON_REG_ALL_XXX);
		if (outcome.rv || outcome.activated){
			break; /* exit on error or first activation procedure start*/
		}
	}
	nvmeibc_topology_put(t);
	NFOUT;
	return outcome.rv;
}

int nvmeibc_topologies_set_suspend_state(struct nvmeibc_topologies *nt,
	bool do_suspend, void *susped_context, blk2blk_gen_work_t on_suspend_finish_cb)
{
	unsigned long flags;
	struct nvmeibc_topology *t, *t1;
	int rv = 0, need_register = false;
	spin_lock_irqsave(&nt->lock, flags);
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		rv = -ENODATA;
		goto _out;
	}
	if (is_suspended(*nt) == do_suspend){
		_NT_SCOPE(trace_topology_nvmeibc_topologies_set_suspend_state, topology, "@DEV_NAME: Already executing! boot_state=@BOOT_STATE", nt->device_name, nt->boot_state);
		rv = -EINVAL;
		goto _out;
	}
	t1 = dup_topology(t);
	if (!t1) {
		_NE_TOPO(error_topology_nvmeibc_topologies_set_suspend_state, t, "No memory for topology dup");
		rv = -ENOMEM;
		goto _out;
	}
	nt->boot_state = (do_suspend) ?	NVMEIBC_TOPO_BOOT_SUSPENDING :
									NVMEIBC_TOPO_BOOT_NORMAL; //NVMEIBC_TOPO_BOOT_REVIVING;
	nvmeibc_block_nt_to_b(nt)->ignore_all_toma_msgs = do_suspend;
	nt->susped_context       = susped_context;
	nt->on_suspend_finish_cb = on_suspend_finish_cb;
	__set_active_topology(nt, t1, t);
	if ((!do_suspend)&&(nvmeibc_topo_is_in_err_state(t1))) {
		/* During suspension we may missed toma messages & have unreg segs */
		need_register = true; /* Send registration request during reviving */
	}
_out:
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
	if (need_register) {
		rv = __register_all_segments(nt);
	}
	return rv;
}

int nvmeibc_topologies_inform_di_bug_in_raid(struct nvmeibc_topologies *nt, u64 *addrs_array, int ci, int ri)
{
	struct nvmeibc_topology *t;
	const struct nvmeibc_raid1 *pr;
	struct nvmeibc_disk_segment *seg;
	int rv = 0, si;
	if (!(t = nvmeibc_topology_get(nt)))
		goto _out;
	if (addrs_array) {
		pr = __get_r1_by_t(t, ci, ri);
		raid1_for_each_seg(pr, seg, si) {
			rv |= __send_toma_di_help(addrs_array[si], seg);
			_NE_SCOPE(t_2d_topo, topology, DMESG_PREFIX("@DEV_NAME") ": Sent DI message to @DISK_NAME, seg=@SEG", nt->device_name, seg->disk->ops.get_full_name(seg->disk), seg->uuid);
		}
	} else {
		struct nvmeibc_chunk *chunk;
		topo_for_each_seg(t, chunk, ci, pr, ri, seg, si) {
			rv |= __send_toma_di_help((~0ULL), seg);
			_NE_SCOPE(t_2e_topo, topology, DMESG_PREFIX("@DEV_NAME") ": Sent DI message to seg=@SEG", nt->device_name, seg->uuid);
		}

	}
	nvmeibc_topology_put(t);
_out:
	return rv;
}

int nvmeibc_topologies_detect_illegal_raid_conf(struct nvmeibc_topologies *nt)
{
#define __is_unknown_node(seg) ((!seg->disk) || ((seg)->disk->ops.get_host_name((seg)->disk)[0] == '?'))
	struct nvmeibc_topology *t;
	struct nvmeibc_chunk *chunk;
	const struct nvmeibc_raid1 *pr;
	const struct nvmeibc_disk_segment *si, *sj;
	int rv = 0, c , r, i, j;
	if (!(t = nvmeibc_topology_get(nt)))
		goto _out;
	topo_for_each_raid1(t, chunk, c, pr, r) {
		if (pr->slice_size == 1) {
			/* N-mirror, all segments must be on differnet nodes */
			for (i = 0  , si = &pr->segments[i]; i < pr->replicas; i++, si++) {
				for (j = i+1, sj = &pr->segments[j]; j < pr->replicas; j++, sj++) {
					if (__is_unknown_node(si)) break;    // Cannot verify it
					if (__is_unknown_node(sj)) continue; // Cannot verify it
					if (!strcmp(si->disk->ops.get_host_name(si->disk), sj->disk->ops.get_host_name(sj->disk))) {
						WARN(1, "%s: Raid(%d,%d) both segs {%d,%d} are on host %s\n",
						   nt->device_name, c, r, i, j, sj->disk->ops.get_host_name(sj->disk));
						rv++;
					}
				}
			}
		} else {
			/* Raid5/6, Consecutive P segments must be on differnet nodes */
			int np = nvmeibc_raid1_get_protect_lvl(pr);
			for (i = 0  , si = &pr->segments[i]; i < pr->replicas; i++, si++) {
				for (j = i+1, sj = &pr->segments[j]; j < i + np      ; j++) {
					sj = &pr->segments[j % pr->replicas];
					if (__is_unknown_node(si)) break;    // Cannot verify it
					if (__is_unknown_node(sj)) continue; // Cannot verify it
					if (!strcmp(si->disk->ops.get_host_name(si->disk), sj->disk->ops.get_host_name(sj->disk))) {
						WARN(1, "%s: Raid(%d,%d) both parity segs {%d,%d} are on host %s\n",
						   nt->device_name, c, r, i, j, sj->disk->ops.get_host_name(sj->disk));
						rv++;
					}
				}
			}
		}
	}
	nvmeibc_topology_put(t);
_out:
	return rv;
}

/* Disconnect from TOMA all the segments of the block dev. */
int nvmeibc_topologies_cleanup(struct nvmeibc_topologies *nt)
{
	struct nvmeibc_topology *t = NULL, *t1 = NULL;
	int rv;
	bool failed = false;
	unsigned long i, flags;
	NFIN;

	/* First: Handle remainings of ongoing reconfiguration. */
	spin_lock_irqsave(&nt->lock, flags);

	//Let's communicate to all potential users that we are going to clean the list
	topos_list_update_begin(nt);
	nt->boot_state = NVMEIBC_TOPO_BOOT_LIST_CLEANUP;
	topos_list_update_end(nt);

	t = __nvmeibc_topology_get_regardless_boot_state(nt);
	if (unlikely(!t)) {
		/* should happen only if initialization of block device failed */
		_NT_SCOPE(trace_topology_nvmeibc_topologies_cleanup, topology, "@DEV_NAME: No topology to clean up for device", nt->device_name);
		failed = true;
		goto out;
	}
	if (t->num_reconfing_segs) {
		__all_segments_delete_replacements(t);
		WARN_ON(t->num_reconfing_segs>0); /* Memory will leak, shit... */
	}
	spin_unlock_irqrestore(&nt->lock, flags); /* let toma msg handler run */

	/* Unregister from all the segments. Toma unregister acks will be redirected
	   to zombies, and ignored by dead segments. Cannot send messages anymore */
	_NI_SCOPE(trace_1_topology_nvmeibc_topologies_cleanup, topology, "@DEV_NAME: cleanup topos, @TOPO_DBG_ID", nt->device_name, t->debug_unique_index);
	__unregister_all_segments_on_cleanup_no_io(nt);

	/* Kill segments to prevent handling unwanted toma messages (switch-topo)*/
	__topology_prepare_all_segments_for_free(t);
	nvmeibc_topology_put(t);

	/* Once the topologies object has been removed from the list, it cannot
	 * be accessed anymore from any callback or timer function. */
	spin_lock_irqsave(&nt->lock, flags);
	t = __nvmeibc_topology_get_regardless_boot_state(nt);
	if (unlikely(!t)) {
		_NT_SCOPE(warn_topology_nvmeibc_topologies_cleanup, topology, "@DEV_NAME: Topology disappeared, possible race", nt->device_name);
		failed = true;
		goto out;
	}
	t1 = dup_topology(t);
	if (!t1) {
		_NE_TOPO(error_topology_nvmeibc_topologies_cleanup, t, "No memory for a topology dup");
		failed = true;
		goto out;
	}
	topos_list_update_begin(nt);
	__set_phased_out_topo(t);		// Allow the head of the list to free
	nt->boot_state = NVMEIBC_TOPO_BOOT_NO_GET_TOPO;
	topos_list_update_end(nt);
	/* There are 2 possible states:
	   1. IO is disabled (during detach)
	   2. IO is enabled, during (reboot). NO_GET_TOPO is set coz
	      otherwise, new IO will take ref on 't' and it may cause memory
	      corruption if 't' is freed */
	_NT_TOPO(trace_2_topology_cleanup, t, "putting last head topo");
	nvmeibc_topology_put(t);
	/* State of the topologies list:
	                              t1
	 /--------------------------  | ----------------------\
	 |  t-2  --> t-1    -->  t ---+      Topologies list  |
	 |  |         |          |                            |
	 |  \__refs   \__refs    \___seg_prepare_for_free     |
	 |                                                    |
	 | refs: IO's Toma messages, etc.                     |
	 \----------------------------------------------------/
	*/
	/* t1 topo is the head but is NOT in the topo list. as IO's complete, they
	   remove refs from the topologies on the list, and all in the list will be
	   freed. Wehn 't' is freed, the loop inside nvmeibc_topology_put()
	   continues to the next topo (which is t1) we hold outside the list.
	   It will now access our 't1' to decrement its refcount &
	   so we need to wait for that before we free t1. */
	for (i = 1; !list_empty(&nt->topologies) || !topo_is_del_allowed(t1); i++) {
		if (!(i & 0x3FF)) { /* Roughly ~1[mSec] */
			_NT_SCOPE(trace_3_topology_cleanup, topology, "@DEV_NAME: Looping (@N_TIMES times), waiting...", nt->device_name, i);
			if (!(i & 0xFFF))
				WARN_ON_ONCE(true);  /* After ~4[sec] issue warning */
			cond_resched();	// Here we are not in interrupt context (main wq)!
		}
		mdelay(1);
	}
	/* Here it is ensured that no segment will receive toma message or else
	   we will have a dead lock. message uses segments 'tr' but waits for
	   nt->lock, while segment cannot disconnect from toma because 'tr'
	   use count > 0 */
	_NT_TOPO(trace_4_topology_cleanup, t1, "freeing extern topo");
	__free_topology(t1, true);
	t1 = NULL;

out:
	spin_unlock_irqrestore(&nt->lock, flags);
	rv = failed ? -1 : 0;

	_NI(trace_5_topology_cleanup, "rv=@RV", rv);
	NFOUT;
	return rv;
}

#define BLKDEV_PROFILING_PROC_FRMT_VER 1
void nvmeibc_topologies_profilers_tostring(struct nvmeibc_topologies *nt,
									  struct nvmeib_txt *txt)
{
	struct nvmeibc_block_device *dev = nvmeibc_block_nt_to_b(nt);
	struct nvmeibc_topology *t;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	unsigned long flags, verb;
	int c, r, s;
	/* Print the general header */
	spin_lock_irqsave(&nt->lock, flags);
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		spin_unlock_irqrestore(&nt->lock, flags);
		nvmeib_txt_append(txt, "Topology already free, possible race");
		goto out;
	}

	if (t) {
		nvmeibc_profiling_stats_tostring(dev->preparation_profiler, txt);
		topo_for_each_raid1(t, chunk, c, r1, r) {
			for (verb = 0; verb < VERB_RW_NUM; verb++) {
				nvmeibc_profiling_stats_tostring(r1->hdr->good_path_profile[verb], txt);
			}
			raid1_for_each_seg(r1, seg, s) {
				nvmeibc_profiling_stats_tostring(seg->disk_operation_profiler, txt);
				nvmeibc_profiling_stats_tostring(seg->lock_operation_profiler, txt);
			}
			nvmeibc_profiling_stats_tostring(r1->hdr->sync_profile, txt);
		}
		nvmeibc_topology_put(t);
	}
	spin_unlock_irqrestore(&nt->lock, flags);
out:
	nvmeib_proc_add_txt_proc_epilog_txt(BLKDEV_PROFILING_PROC_FRMT_VER, txt);
}

void nvmeibc_topologies_profilers_tocsv(struct nvmeibc_topologies *nt, struct nvmeib_txt *txt)
{
	struct nvmeibc_block_device *dev = nvmeibc_block_nt_to_b(nt);
	struct nvmeibc_topology *t;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	unsigned long flags, verb;
	int c, r, s;
	/* Print the general header */
	spin_lock_irqsave(&nt->lock, flags);
	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		spin_unlock_irqrestore(&nt->lock, flags);
		nvmeib_txt_append(txt, "Topology already free, possible race");
	} else {
		nvmeibc_profiler_tocsv(dev->preparation_profiler, txt);
		topo_for_each_raid1(t, chunk, c, r1, r) {
			for (verb = 0; verb < VERB_RW_NUM; verb++) {
				nvmeibc_profiler_tocsv(r1->hdr->good_path_profile[verb], txt);
			}
			raid1_for_each_seg(r1, seg, s) {
				nvmeibc_profiler_tocsv(seg->disk_operation_profiler, txt);
				nvmeibc_profiler_tocsv(seg->lock_operation_profiler, txt);
			}
			nvmeibc_profiler_tocsv(r1->hdr->sync_profile, txt);
		}
		spin_unlock_irqrestore(&nt->lock, flags);
		nvmeibc_topology_put(t);
	}
}


void nvmeibc_topologies_stalocks_tostring(struct nvmeibc_topologies *nt, struct nvmeib_txt *txt)
{
	struct nvmeibc_topology *t = nvmeibc_topology_get(nt);
	const struct nvmeibc_chunk *chunk;
	const struct nvmeibc_raid1 *r1;
	const bool with_stats = false;
	int c, r;
	u32 i = 0, rcvr_start = NVMEIBT_RECOVERY_TYPE_INVALID+1, rcvr_end = ARRAY_SIZE(r1->hdr->recoveries);
	if (unlikely(!t)) {
		nvmeib_txt_append(txt, "Unable to obtain additional information...\n");
		goto _out;
	}
	topo_for_each_raid1(t, chunk, c, r1, r) {
		struct nvmeibc_raid_topo_persistent *hdr = r1->hdr;
		nvmeib_txt_append(txt, "Raid (%d,%d): ", c, r);	 /* Deliberately no '\n' suffix */
		stale_lock_resolver_to_str(&hdr->slr, txt);

		nvmeib_txt_append(txt, "{Job Stats:");
		for (i = rcvr_start; i < rcvr_end; i++) {
			nvmeibc_recovery_stats_to_str(hdr->recoveries[i], txt);
		}
		nvmeib_txt_append(txt, "}\n");
		for (i = rcvr_start; i < rcvr_end; i++) {
			nvmeibc_recovery_info_to_str(hdr->recoveries[i], with_stats, txt);
		}

	}
	nvmeibc_topology_put(t);
_out:
	return;
}

static const char *__volume_type(const struct nvmeibc_topology *t)
{
	const char *rv;
	if (__get_topo_num_chunks(t) == 0) {
		rv = "Multier Volume";
	} else {
		struct nvmeibc_chunk *chunk;
		struct nvmeibc_raid1 *r1;
		int c, r, replicas = t->chunks->raid1s->replicas; /* Of first raid1 */
		const bool is_stripped = (t->stripe_width > 1);
		const bool is_eras_cod = nvmeibc_raid_is_ec(t->chunks->raid1s);
		if (!is_eras_cod) {
			topo_for_each_raid1(t, chunk, c, r1, r) {
				if (replicas != r1->replicas){
					replicas = -1;						/* Hybrid volume */
					goto __done_analyzing_replicas;
				}
			}
		__done_analyzing_replicas:
			if (replicas == 1) {
				rv = is_stripped ? "RAID-0" 			: "Concatenated";
			} else if (replicas == -1){
				rv = is_stripped ? "Hybrid RAID-10" 	: "Hybrid RAID-1";
			} else {
				rv = is_stripped ? "RAID-10" 			: "RAID-1";
			}

		} else {
			uint max_parities = 0;
			topo_for_each_raid1(t, chunk, c, r1, r){
				const uint cur_parity = nvmeibc_raid1_get_protect_lvl(r1);
				max_parities = max(max_parities, cur_parity);
			}
			if (max_parities == 1) {
				rv = is_stripped ? "RAID-50" 			: "RAID-5";
			} else {
				rv = is_stripped ? "RAID-60"			: "RAID-6";
			}
		}
	}
	return rv;
}

/* Does mirroring raid has at least 1 good RW seg to make IO from */
static bool __raid1_has_good_rw_seg(const struct nvmeibc_raid1*r1)
{
	int i;
	const struct nvmeibc_disk_segment *seg;
	raid1_for_each_seg(r1, seg, i){
		if ((seg->toma_acm == NVMEIBTC_DS_MODE_RW) && !__seg_has_problem(seg))
			return true;
	}
	return false;
}

const char *nvmeib_io_type_permission_to_string(enum nvmeib_io_type_permission val)
{
	switch (val) {
	case NVMEIB_IO_TYPE_PERMIT_NEVER: 				return "IO was never enabled for volume";
	case NVMEIB_IO_TYPE_PERMIT_NONE_SUS: 			return "Volume is suspened, no IO";
	case NVMEIB_IO_TYPE_PERMIT_NONE_INV:			return "Wrong topology prevents IO";
	case NVMEIB_IO_TYPE_PERMIT_NONE_ERR:			return "No permissions to do IO (Toma or Network problem), see more info in status of each segment segs[i].act and each disk segs[i].disk.paused in the volume";
	case NVMEIB_IO_TYPE_PERMIT_NO_IO:				return "Toma allows only recoveries. no IO";
	case NVMEIB_IO_TYPE_PERMIT_NO_RM_IO:			return "Other client with higher reservation mode preempted my IO";
	case NVMEIB_IO_TYPE_PERMIT_RDONLY:				return "Readonly volume, writes are not allowed";
	case NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION:	return "IO is enabled, but data is not protected in current degrade mode";
	case NVMEIB_IO_TYPE_PERMIT_ALL: 				return "IO is enabled";
	default:						 				return "No additional info";
	};
}

const char *nvmeib_io_type_permission_to_string_short(enum nvmeib_io_type_permission val)
{
	switch (val) {
	case NVMEIB_IO_TYPE_PERMIT_NEVER:
		return "NVMEIB_IO_TYPE_PERMIT_NEVER";
	case NVMEIB_IO_TYPE_PERMIT_NONE_SUS:
		return "NVMEIB_IO_TYPE_PERMIT_NONE_SUS";
	case NVMEIB_IO_TYPE_PERMIT_NONE_INV:
		return "NVMEIB_IO_TYPE_PERMIT_NONE_INV";
	case NVMEIB_IO_TYPE_PERMIT_NONE_ERR:
		return "NVMEIB_IO_TYPE_PERMIT_NONE_ERR";
	case NVMEIB_IO_TYPE_PERMIT_NO_IO:
		return "NVMEIB_IO_TYPE_PERMIT_NO_IO";
	case NVMEIB_IO_TYPE_PERMIT_NO_RM_IO:
		return "NVMEIB_IO_TYPE_PERMIT_NO_RM_IO";
	case NVMEIB_IO_TYPE_PERMIT_RDONLY:
		return "NVMEIB_IO_TYPE_PERMIT_RDONLY";
	case NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION:
		return "NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION";
	case NVMEIB_IO_TYPE_PERMIT_ALL:
		return "NVMEIB_IO_TYPE_PERMIT_ALL";
	default:
		return "???";
	};
}

static const char *__segment_stateR1(struct nvmeibc_raid1 *r1, int s)
{	/* Note: Assumes each segment has valid seg->disk */
	#define OFFLINE_STR      "Offline, stops IO"
	#define INTERNAL_ERR_STR "Internal Error"
	#define ONLINE_STR       "Online"
	#define REBUILD_STR		 "Rebuilding"
	#define NOT_NEEDE_STR    "Offline"
	int has_good_rw_seg = __raid1_has_good_rw_seg(r1);

	switch (r1->segments[s].toma_acm) {
	case NVMEIBTC_DS_MODE_RW:
	case NVMEIBTC_DS_MODE_W_NO_DIRTY:
		return (__seg_has_problem(r1->segments + s)) ? OFFLINE_STR : ONLINE_STR;
	case NVMEIBTC_DS_MODE_W:
	case NVMEIBTC_DS_MODE_W_IS_DIRTY:
		if (  __seg_has_problem(r1->segments + s))
			return OFFLINE_STR;
		return (!has_good_rw_seg) ? "Offline, Rebuild stopped" : REBUILD_STR;
	case NVMEIBTC_DS_MODE_DEAD:
	case NVMEIBTC_DS_MODE_INVALID:
		return (!has_good_rw_seg) ? NOT_NEEDE_STR : "Offline, IO from replica";
	default:;
	}
	WARN(true, "nvmeibc bug, unknown acm=%d\n", r1->segments[s].toma_acm);
	return INTERNAL_ERR_STR;
}

static const char *__segment_stateR6(struct nvmeibc_raid1 *r1, int s)
{
	switch (r1->segments[s].toma_acm) {
	case NVMEIBTC_DS_MODE_RW:
	case NVMEIBTC_DS_MODE_W_NO_DIRTY:
		return (__seg_has_problem(r1->segments + s)) ? OFFLINE_STR : ONLINE_STR;
	case NVMEIBTC_DS_MODE_W:
	case NVMEIBTC_DS_MODE_W_IS_DIRTY:
		return (__seg_has_problem(r1->segments + s)) ? OFFLINE_STR : REBUILD_STR;
	case NVMEIBTC_DS_MODE_DEAD:
	case NVMEIBTC_DS_MODE_INVALID:
		return NOT_NEEDE_STR;
	default:;
	}
	WARN(true, "nvmeibc bug, unknown acm=%d\n", r1->segments[s].toma_acm);
	return INTERNAL_ERR_STR;
}

static const char *__segment_state(struct nvmeibc_raid1 *r1, int s)
{
	return (nvmeibc_raid_is_ec(r1)) ? __segment_stateR6(r1, s) : __segment_stateR1(r1, s);
}

/* 'a' for regular segment, 'r' for relocating, 'u' for upgrading to raid1, 'd'
   for downgrading. If Toma already has a new configuration and requested to
   apply it letter becomes capital. 'A' means toma requested to apply
   reconfguration which client does not have yet */
static char __segment_reconf_state(const struct nvmeibc_disk_segment *seg){
	const char cap = 'a'-'A';
	char res = 'a';
	if (seg_has_replacement(seg))	res = 'r';
	if (seg_has_upgrade(seg)) res = 'u';
	if (seg_has_downgrade(seg)) res = 'd';
	if (seg_has_toma_said_apply(seg)) res -= cap;
	return res;
}

static inline const char *__nt_status_str(const struct nvmeibc_topologies *nt,
										  int* rv)
{
	const int iop = nt->io_perm;
	#define ERR_F(e) 		((e)|0x80)		// Default Error flag
	if (!is_suspended(*nt)) {				// Only if not booting
		switch (iop) {
		case NVMEIB_IO_TYPE_PERMIT_NONE_SUS:
			*rv = ERR_F(0x11);	  /* 0xB1 */ return "Live, no IO, Error";
		case NVMEIB_IO_TYPE_PERMIT_NONE_INV:
			*rv = ERR_F(0x12);	  /* 0xB2 */ return "Live, no IO, Toma Error";
		case NVMEIB_IO_TYPE_PERMIT_NONE_ERR:
			*rv = ERR_F(0x21);	  /* 0xA1 */ return "Live, no IO, partial Error";
		case NVMEIB_IO_TYPE_PERMIT_NO_IO:
			*rv = ERR_F(0x10|iop);/* 0x91 */ return "Live, Only recovery";
		case NVMEIB_IO_TYPE_PERMIT_NO_RM_IO:
			*rv = ERR_F(0x10|iop);/* 0x92 */ return "Reservation Preempted, Only recovery";
		case NVMEIB_IO_TYPE_PERMIT_RDONLY: {
			const struct nvmeibc_os_api *os = nt->nd->os;
			if (!(os && os->atom.users.readonly)) {
			*rv = ERR_F(0x10|iop);/* 0x93 */ return "Live, Degraded Read-Only";
			} else {
			*rv = 0;			  /* 0x00 */ return "Live, User Requested Read-Only";
			}
		}
		case NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION:
			*rv = 0;			  /* 0x00 */ return "Live, with IO, no protection";
		case NVMEIB_IO_TYPE_PERMIT_ALL:
			*rv = 0;			  /* 0x00 */ return "Live, with IO";
		};
	}
	*rv = ERR_F(nt->boot_state);	// Booting: 0x82..0x86
	switch (nt->boot_state) {
	case NVMEIBC_TOPO_BOOT_SUSPENDING:	return "Suspending, no IO";
	case NVMEIBC_TOPO_BOOT_LIST_CLEANUP:return "Cleanup state, no IO";
	case NVMEIBC_TOPO_BOOT_NO_GET_TOPO:	return "Cleanup done, no IO";
	case NVMEIBC_TOPO_BOOT_APPLIED_NEW:	return "Booting, no IO";
	case NVMEIBC_TOPO_BOOT_REVIVING:	return "Reviving, no IO";
	default:							return "Unknown???, no IO";
	}
}

#define __dbg_sts(dev, dev_sts_int)  (((dev)->status<<8) + (dev_sts_int))
void nvmeibc_topologies_status_tostring(struct nvmeibc_topologies *nt, struct nvmeib_txt *txt)
{
	const struct nvmeibc_block_device *dev = nvmeibc_block_nt_to_b(nt);
	struct nvmeibc_topology *t;
	unsigned long flags;
	const char *dev_status_str = NULL, *dev_attach_status = NULL;
	int         dev_status_int = 0;		// For QA, auto parsing scripts

	/* Print the general header */
	nvmeib_txt_append(txt, "RVMS=0x%llx\n", nt->reservation_version_max_seen);
	dev_attach_status = nvmeibc_block_status_to_string(dev->status);
	spin_lock_irqsave(&nt->lock, flags);
	dev_status_str = __nt_status_str(nt, &dev_status_int);
	nvmeib_txt_append(txt, "Device status: %s, %s (debug:0x%x, %llu)\n", dev_attach_status,
			dev_status_str, __dbg_sts(dev, dev_status_int),
			nt->dbg_num_enabling_io_toggles);
	if (dev_status_int==0)
		nvmeib_txt_append(txt, "IO is currently enabled.\n");
	else
		nvmeib_txt_append(txt, "IO is currently disabled for %llu[msec].\n", (jiffies - nt->dbg_disabling_ts) * 1000 / HZ);

	t = nvmeibc_topology_get(nt);
	if (unlikely(!t)) {
		nvmeib_txt_append(txt, "Unable to obtain additional information...\n");
		goto _out;
	}
	nvmeib_txt_append(txt, "Raid Type: %s\n", __volume_type(t));
	__topo_status_tostring(t, txt);
_out:
	nvmeibc_topology_put(t);
	spin_unlock_irqrestore(&nt->lock, flags);
}

static void __topo_status_tostring(const struct nvmeibc_topology *t, struct nvmeib_txt *txt)
{
	int c, r, si;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	nvmeib_txt_append(txt, "Topology Debug: i=[%llu..%llu), ver=%llu, "
			"io_perm=%d nr=%d ns=%llu[blks], vl=%d\n",
			t->debug_unique_index, t->nt->topo_debug_last_freed_version,
			t->configuration_version, t->io_perm,
			t->num_reconfing_segs, t->resize.size, t->is_safe_for_view_lock);

	/* Print segments info */
	topo_for_each_chunk(t, chunk, c) {
		const bool is_eras_code = (chunk->raid1s->slice_size > 1);
		const char *slice_name  = (!is_eras_code ? "Replica" : "Slice");
		nvmeib_txt_append(txt, "Chunk #%d: Stripe{Size=%d, Width=%d} ",
		c, chunk->stripe_size, t->stripe_width);
		if (is_eras_code) {
			const uint n_data = chunk->raid1s->slice_size;
			const uint n_pari = (chunk->raid1s->replicas - n_data);
			nvmeib_txt_append(txt, "Slice{%d+%d} ",n_data, n_pari);
		} else {
			nvmeib_txt_append(txt, "Replicas=%d ",chunk->raid1s->replicas);
		}
		nvmeib_txt_append(txt, "Vol Blocks [%lld..%lld]\n",
				chunk->first_vlba, chunk[1].first_vlba - 1);
		nvmeib_txt_append(txt, "\t%-6s %-7s %-26s %-21s %-12s %-12s %-32s Debug-info\n",
			"Stripe", slice_name, "Status", "Disk NVMe ID", "0xLBA Start", "0xLBA End",
			"Last Known Target");
		chunk_for_each_raid1(chunk, r1, r) {
			bool is_r1_broken = false;
			raid1_for_each_seg(r1, seg, si)
				is_r1_broken |= (seg->disk == NULL); // Should not happen!
			if (is_r1_broken) {
				raid1_for_each_seg(r1, seg, si)
					nvmeib_txt_append(txt, "\t%-6d %-7d %-36s Disk Error\n",
							r, si, INTERNAL_ERR_STR);
				continue;							// To next r1 in chunk
			}
			raid1_for_each_seg(r1, seg, si) {
				const struct nvmeibc_idisk *disk = seg->disk;
				const char *acm = nvmeibt_client_topo_seg_access_mode_to_str(seg->toma_acm);
				char slmap[LOCK_OWNERSHIP_MAP_STRING_LEN];
				const int disk_p_state = __disk_p_state2num(disk);
				lock_ownership_map_to_string(&seg->lmap, slmap);
				nvmeib_txt_append(txt, "\t%-6d %-7d %-26s %-21s %-12llx %-12llx %-32.32s [a=%d p=%d acm=%s sy=%d lm(%s) r1v=0x%x lid=0x%x|%c uid=%-.8s]",
					r, si, __segment_state(r1, si), disk->ops.get_name(disk),
					seg->first_lba, seg->first_lba + seg->length -1,
					nvmeibc_idisk_get_host_name_for_logging(disk),
					seg->registration_status, disk_p_state, acm,
					seg->sync_safety, slmap, r1->version, r1->lid.all,
					__segment_reconf_state(seg), seg->uuid);
				if ((seg->toma_reg) && (seg->toma_reg->protocol_version != NVMEIBT_CLIENT_PROTO_VERSION)) {
					nvmeib_txt_append(txt, " proto=0x%x", seg->toma_reg->protocol_version);
				}
				nvmeib_txt_append(txt, "\n");
			}
		}
	}
	if (__topo_num_last_chunks_disabled(t)) {
		nvmeib_txt_append(txt, "Important: %d last chunks are disabled, does not affect IO\n",
				__topo_num_last_chunks_disabled(t));
	}
}

static void __topo_status_tojson(const struct nvmeibc_topology *t, struct jdr *jdr)
{
	int c, r, si;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;

	jdr_object_scope(jdr, "topo");
	jdr_write_var(jdr, cur, t->debug_unique_index);
	jdr_write_var(jdr, freed, t->nt->topo_debug_last_freed_version);
	jdr_write_var(jdr, conf_ver, t->configuration_version);
	jdr_write_var(jdr, io_perm, t->io_perm);
	jdr_write_var(jdr, nr, t->num_reconfing_segs);
	jdr_write_var(jdr, ns, t->resize.size);
	jdr_write_var(jdr, can_view_lock, t->is_safe_for_view_lock);
	jdr->ops.ascii(jdr, "verbose_status", nvmeib_io_type_permission_to_string(t->io_perm));

	{
		jdr_array_scope(jdr, "chunks");
		topo_for_each_chunk(t, chunk, c) {
			{
				jdr_object_scope(jdr, NULL);
				jdr_write_var(jdr, ci, c);
				jdr_write_var(jdr, vlba_start, chunk->first_vlba);
				jdr_write_var(jdr, vlba_end, chunk[1].first_vlba - 1);
				{
					jdr_array_scope(jdr, "prs");
					chunk_for_each_raid1(chunk, r1, r) {
						{
							jdr_object_scope(jdr, NULL);
							jdr_write_var(jdr, ri, r);
							jdr_write_var(jdr, version, r1->version);
							jdr->ops.ascii_format(jdr, "lock", "0x%x", r1->lid.all);
							jdr_write_var(jdr, dbits_off_mask, nvmeibc_raid1_get_sgmnts_bmp(r1, dbits_off_mask));
							jdr_write_var(jdr, dbits_on_mask, nvmeibc_raid1_get_sgmnts_bmp(r1, dbits_on_mask));
							{
								jdr_array_scope(jdr, "segs");
								raid1_for_each_seg(r1, seg, si) {
									const struct nvmeibc_idisk *disk = seg->disk;
									const char *acm = nvmeibt_client_topo_seg_access_mode_to_str(seg->toma_acm);
									char slmap[LOCK_OWNERSHIP_MAP_STRING_LEN];
									lock_ownership_map_to_string(&seg->lmap, slmap);
									{
										jdr_object_scope(jdr, NULL);
										jdr_write_var(jdr, si, si);
										jdr_write_var(jdr, dlba_start, seg->first_lba);
										jdr_write_var(jdr, dlba_end, seg->first_lba + seg->length - 1);
										jdr->ops.ascii_format(jdr, "uuid", "%-.8s", seg->uuid);
										jdr_write_var(jdr, act, seg->registration_status);
										jdr->ops.ascii(jdr, "acm", acm);
										jdr_write_var(jdr, locks_on_read, seg->sync_safety);
										jdr->ops.ascii(jdr, "lmap", slmap);
										jdr->ops.ascii_format(jdr, "reconf", "%c", __segment_reconf_state(seg));
										{
											jdr_object_scope(jdr, "disk");
											jdr->ops.ascii(jdr, "name", disk->ops.get_name(disk));
											jdr->ops.ascii(jdr, "host", nvmeibc_idisk_get_host_name_for_logging(disk));
											jdr_write_var(jdr, paused, __disk_p_state2num(disk));
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

#define get_io_enabled(dev) ((int)(nvmeibc_get_io_perm_for_reporting((struct nvmeibc_block_device *)(dev)) == NVMEIB_C_TO_M_IO_TYPE_PERMIT_ALL))

void nvmeibc_topologies_status_tojson(struct nvmeibc_topologies *nt, struct jdr *jdr)
{
	const struct nvmeibc_block_device *dev = nvmeibc_block_nt_to_b(nt);
	struct nvmeibc_topology *t;
	unsigned long flags;
	const char *dev_status_str = NULL;
	int         dev_status_int = 0;		// For QA, auto parsing scripts

	/* Print the general header */
	spin_lock_irqsave(&nt->lock, flags);
	jdr_write_var(jdr, RVMS, nt->reservation_version_max_seen);
	jdr->ops.ascii(jdr, "attach_status", nvmeibc_block_status_to_string(dev->status));
	dev_status_str = __nt_status_str(nt, &dev_status_int);
	t = nvmeibc_topology_get(nt);
	if (t) {
		const struct nvmeibc_volume_attach_t *vat = nvmeibc_block_get_res_vat(dev);
		jdr->ops.ascii(jdr, "raid_type", __volume_type(t));
		jdr->ops.ascii(jdr, "reservation", nvmeibc_volume_attach_t_mode_to_string(vat->res.mode));
		jdr_write_var(jdr, reservation_version, vat->res.version);
		jdr->ops.ascii(jdr, "preempt", nvmeibc_volume_attach_t_preempt_to_string(vat->res.preempt));
		jdr->ops.ascii(jdr, "status", dev_status_str);
		jdr_write_var(jdr, dbg, __dbg_sts(dev, dev_status_int));
		jdr_write_var(jdr, io-toggles, nt->dbg_num_enabling_io_toggles);
		jdr_write_var(jdr, io_enabled, get_io_enabled(dev));
		__topo_status_tojson(t, jdr);
		nvmeibc_topology_put(t);
	}
	spin_unlock_irqrestore(&nt->lock, flags);
}

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
/* For debug: wait until we have only single topology with no references to it. since older topology has a reference to the newer topology, if an older one existed then the newer would have higher ref-count.
 * since IO's also increase the ref-count, watching the total counters awaits
 * for both IO's & older topologies to complete assumption:
 * 	1) IO's arent being fired while we wait
 *	2) topologies arent created
 */
void nvmeibc_topo_wait_for_single_topo_no_io(const struct nvmeibc_topologies *nt, int n_refs)
{
	struct nvmeibc_topology *t;
	struct list_head *t_list = (void*)&nt->topologies;
	int		sum, i;
	for (i = 0; true; i++, nvmeibc_topology_put(t), udelay(10)){
		t = nvmeibc_topology_get((struct nvmeibc_topologies *)nt);
		if (unlikely(!t))		  break;	/* Bug in usage. Volume rebooring? */
		if (t->newer) continue; /* Has newer topology */
		if (!list_is_last(&t->list_n, t_list)) continue;/* Has multiple topos */
		sum = nvmeibc_topo_get_topo_users(t);
		if (sum == n_refs) break;    /* No users of our topo (IO's,locks,sync,...) */
	}
	nvmeibc_topology_put(t);
}

bool nvmeibc_topology_is_stable_noio(const struct nvmeibc_topologies *nt, bool do_assert)
{
	#define CONDITIONAL_BUG_ON(x)  if (x) { if (do_assert) BUG(); \
	                                        else { rv = false; goto _out; }}
	int c, r, si, sum = 0, rv = true;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	/* verify that there is 1 topology without users */
	struct nvmeibc_topology *t =
		nvmeibc_topology_get((struct nvmeibc_topologies *)nt);
	CONDITIONAL_BUG_ON(!t);
	CONDITIONAL_BUG_ON(nvmeibc_topo_is_in_err_state(nt));
	CONDITIONAL_BUG_ON(nvmeibc_topo_is_in_err_state(t));
	CONDITIONAL_BUG_ON(t->newer);
	sum = nvmeibc_topo_get_topo_users(t);
	#ifdef DEBUG_TOPO_CNTRS
		if ((sum != 1)&&(do_assert)) {
			const struct nvmeibc_cinst_params_blk *p = nvmeibc_cinst_get_blok_p(nt->nd);
			void __debug_topo_print_uncompleted_op(const struct nvmeibc_cinst_params_blk *p, bool analyze_current_topo);
			_ND(trace_topology_nvmeibc_topology_is_stable_noio, "sum=@SUM", sum);
			__debug_topo_print_uncompleted_op(p, true);
		}
	#endif
	CONDITIONAL_BUG_ON(sum!=1);
	CONDITIONAL_BUG_ON(nvmeibc_topology_is_reconfiguring_now(t));

	topo_for_each_seg(t, chunk, c, r1, r, seg, si){
		CONDITIONAL_BUG_ON(seg->replacement);
	}

	topo_for_each_raid1(t, chunk, c, r1, r){
		raid1_for_each_seg(r1, seg, si) {
			CONDITIONAL_BUG_ON(seg->replacement      != NULL);
			CONDITIONAL_BUG_ON(seg->toma_reg->status != NVMEIBC_SUBSCRIPTION_STATUS_NORMAL);
			CONDITIONAL_BUG_ON(!is_seg_active(*seg));
		}
	}
_out:
	nvmeibc_topology_put(t);
	return rv;
}
#endif

void nvmeibc_topologies_free(struct nvmeibc_topologies *nt)
{
	if (list_empty(&nt->topologies)) {
		if (nt->percpu) {
		/* wait until all users are done with the topologies. the last user of
		 * topologies also free the topology & in case were DETACHING, that will
		 * allow the detach procedure to continue & free the
		 * "struct nvmeibc_block_device" in which the topologies is a member.
		 *  if that happens, the attempt of the last user to idicate its NOT
		 *  using the topologies will dereference an already freed memory */
		topo_drain_users(nt);
		topo_kfree(nt->percpu);
		topo_kfree(nt->percore_shared);
		_NT_SCOPE(t_z2_cdetach, topology, "percpu topologies free");
		} else { /* Failed attach cleanup */}
	} else {
		const char *dev_name = nt->nd->name;
		_NE_SCOPE(t_z3_cdetach, topology, DMESG_PREFIX("@DEV_NAME") ": memleak. topologies list not empty", dev_name);
	}
}

void nvmeibc_topologies_duplicate(struct nvmeibc_topologies *nt, void (*fn)(struct nvmeibc_topology* new_t))
{
	unsigned long flags;
	struct nvmeibc_topology *t, *t1;
	spin_lock_irqsave(&nt->lock, flags);
	t = nvmeibc_topology_get(nt);
	if ((t) && ((t1 = dup_topology(t)) != NULL)) {
		if (fn)
			fn(t1);
		__set_active_topology(nt, t1, t);
	}
	spin_unlock_irqrestore(&nt->lock, flags);
	nvmeibc_topology_put(t);
}

/******************* Live TOMA message handlers drainer **********************/

void toma_msg_handlers_drainer_reinit(struct toma_msg_handlers_drainer *tmhd)
{
	kref_init(&tmhd->handlers);
}

void toma_msg_handlers_drainer_init(struct toma_msg_handlers_drainer *tmhd, void (*release_cb)(struct toma_msg_handlers_drainer *))
{
	tmhd->release_cb = release_cb;
	toma_msg_handlers_drainer_reinit(tmhd);
}

void toma_msg_handlers_drainer_get(struct toma_msg_handlers_drainer *tmhd)
{
	kref_get(&tmhd->handlers);
}

static void __toma_msg_handlers_drainer_release(struct kref *handlers)
{
	struct toma_msg_handlers_drainer *tmhd = container_of(handlers, struct toma_msg_handlers_drainer, handlers);
	WARN_ON(!tmhd->release_cb);
	if (tmhd->release_cb)
		tmhd->release_cb(tmhd);
}

void toma_msg_handlers_drainer_put(struct toma_msg_handlers_drainer *tmhd)
{
	kref_put(&tmhd->handlers, __toma_msg_handlers_drainer_release);
}

/**************************** IO throttle metrics (trace / module param) ****************************/

uint nvmeibc_io_throttle_metrics_trace_mask = NVMEIBC_IO_THROTTLE_METRIC_COUNT
	| NVMEIBC_IO_THROTTLE_METRIC_LATENCY | NVMEIBC_IO_THROTTLE_METRIC_NUM_THROTTLED;
module_param_named(io_throttle_metrics_trace_mask, nvmeibc_io_throttle_metrics_trace_mask, uint, 0644);
MODULE_PARM_DESC(io_throttle_metrics_trace_mask,
		 "Bitmask of IO throttle metric traces to emit: bit0=event_count, bit1=latency_highres_histogram, bit2=reserved, bit3=number_of_throttled_ios. 0 disables all.");

void nvmeibc_io_throttle_metrics_trace_selected(const char *dev_name,
						struct topo_percore_shared *percore_shared,
						unsigned int trace_mask)
{
	int cpu_id;
	const int n_cpu = MAX_NUM_ACTIVE_CPUS;
	u64 *count_vals = NULL;

	count_vals = kcalloc(n_cpu, sizeof(u64), GFP_KERNEL);
	if (!count_vals)
		goto out;

	if (trace_mask & NVMEIBC_IO_THROTTLE_METRIC_COUNT) {
		for_each_allocated_cpu(cpu_id) {
			struct topo_percore_shared *tps = &percore_shared[cpu_id];

			count_vals[cpu_id] = tps->throttle_metrics.count.counter;
		}

		NVMEIB_LOG_METRICS("@DEV_NAME io_throttle_event_count @IO_THROTTLE_CPU_ARR",
				   _T, tracer_nvmeibc, info_dev_io_throttle_event_count,
				   dev_name, count_vals, n_cpu);
	}

	if (trace_mask & NVMEIBC_IO_THROTTLE_METRIC_LATENCY) {
		for_each_allocated_cpu(cpu_id) {
			struct topo_percore_shared *tps = &percore_shared[cpu_id];

			NVMEIB_LOG_METRICS("@DEV_NAME io_throttle_latency cpu_index=@INT @HIGHRES_HISTOGRAM",
					   _T, tracer_nvmeibc, info_dev_io_throttle_latency,
					   dev_name, cpu_id,
					   "io_throttle_latency", "module=nvmeibc;component=io_throttle",
					   (u32)nvmeib_public_tsc_khz(),
					   tps->throttle_metrics.latency.max,
					   tps->throttle_metrics.latency.bins);
		}
	}

	if (trace_mask & NVMEIBC_IO_THROTTLE_METRIC_NUM_THROTTLED) {
		for_each_allocated_cpu(cpu_id) {
			struct topo_percore_shared *tps = &percore_shared[cpu_id];

			count_vals[cpu_id] = (u64)tps->throttle_metrics.num_throttled.counter;
		}

		NVMEIB_LOG_METRICS("@DEV_NAME io_throttle_num_throttled @IO_THROTTLE_CPU_ARR",
				   _T, tracer_nvmeibc, info_dev_io_throttle_num_throttled,
				   dev_name, count_vals, n_cpu);
	}

out:
	kfree(count_vals);
}
