/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv.c - TPV volume attach/detach and block device registration.
 *
 * A Thin-Provisioned Volume (TPV) presents a sparse virtual address space
 * to a single exclusive client.  Physical storage is provided by a hidden
 * Carrier Direct Volume (CDV).  The mapping is maintained by the client-local
 * TPV.allocator (nvmeibc_tpv_allocator.c) and persisted in the per-TPV tree extent
 * (nvmeibc_tpv_persist.c).
 *
 * Attach flow (S.3.8):
 *   1. Assert CDV is present in the volumes list.
 *   2. Allocate and initialise struct nvmeibc_tpv.
 *   3. Schedule deferred load_state_work (tree extent read + recovery).
 *   4. Register gendisk.  IO arriving before load completes is parked on
 *      pending_bios and drained once state_loaded is set by the worker.
 *
 * Background load_state_work (nvmeibc_tpv_persist.c):
 *   a. Load allocator state from the per-TPV tree extent (nvmeibc_tpv_load_state).
 *      Retries on failure (CDV not ready, transport error) every 1 second.
 *   b. Run recovery (nvmeibc_tpv_recovery) - always, regardless of tree state.
 *   c. Set state_loaded; drain pending_bios.
 *   d. Schedule initial CDV_extent request if pool empty.
 *
 * Detach flow (S.3.8):
 *   1. Quiesce IO (freeze queue).
 *   2. Flush dirty allocator state to the tree extent (nvmeibc_tpv_flush_state).
 *   3. Unregister gendisk.
 *   4. Free allocator (xarray + extent lists).
 *   5. Notify management via MCS.
 */

#include "common/kr_incs.h"			/* kernel headers, logging, u64, etc. */
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"		/* nvmeibc_volume, cdv_allocator_toma_id */
#include "common/nvmeib_common_os_block_api.h"	/* REQ_RET, REQ_RET_ZERO */
#include "clnt/nvmeibc_block.h"			/* KERNEL_SECTOR_SHIFT */

/*
 * IO_TIME_OUT_ATTACH / IO_TIME_OUT_NORMAL are local to nvmeibc_block.c;
 * duplicate the values here for the ?: fallback when io_max_retry_secs == 0.
 */
#define TPV_IO_TIME_OUT_ATTACH	30			/* seconds */
#define TPV_IO_TIME_OUT_NORMAL	((unsigned long)(1 << 20))	/* ~12 days */

extern unsigned nvmeibc_io_max_retry_secs;

bool tp_verify_l1_l2_extent_ownership;
module_param(tp_verify_l1_l2_extent_ownership, bool, 0644);
MODULE_PARM_DESC(tp_verify_l1_l2_extent_ownership,
		 "Verify L1/L2 tree entries reference owned CDV extents during load_state");

unsigned int tpv_cdv_retry_msecs = 100;
module_param(tpv_cdv_retry_msecs, uint, 0644);
MODULE_PARM_DESC(tpv_cdv_retry_msecs,
		 "Retry delay in milliseconds for TPV CDV operations (load_state, etc.)");

/*
 * ATOM handover fops pointer - set by nvmeibc_os_api_layer_init() in
 * nvmeibc_block_api_os.c.  Contains nvmeiba's .owner, .open, .release
 * handlers.  Used to populate tpv_live_fops at fresh attach and NDU adopt.
 */
extern const struct block_device_operations *nvmeibc_atom_handover_fops;

/* -- Forward declarations for sibling implementation files -------------- */

/* nvmeibc_tpv_io.c */
extern REQ_RET nvmeibc_tpv_make_request(struct request_queue *q, struct bio *bio);

/* -- Block device fops --------------------------------------------------- */

/*
 * On kernels without make_request_fn (>=5.9), submit_bio fops takes a single
 * (struct bio *) argument, not (queue *, bio *).  Bridge the gap here so
 * nvmeibc_tpv_make_request keeps the canonical (q, bio) signature used by all
 * NVMesh make_request implementations.
 */
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
/* Forward declaration - satisfies -Wmissing-prototypes. */
REQ_RET nvmeibc_tpv_submit_bio_wrapper(struct bio *bio);

REQ_RET nvmeibc_tpv_submit_bio_wrapper(struct bio *bio)
{
	struct nvmeibc_tpv *tpv = bio_gendisk(bio)->private_data;

	return nvmeibc_tpv_make_request(tpv_queue(tpv), bio);
}
#endif

/*
 * Populate a tpv_live_fops struct with ATOM's .owner/.open/.release and
 * nvmeibc's .submit_bio.  Used at fresh attach and NDU adopt (step A6).
 */
static void nvmeibc_tpv_init_live_fops(struct block_device_operations *fops)
{
	memset(fops, 0, sizeof(*fops));
	fops->owner   = nvmeibc_atom_handover_fops->owner;
	fops->open    = nvmeibc_atom_handover_fops->open;
	fops->release = nvmeibc_atom_handover_fops->release;
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	fops->submit_bio = nvmeibc_tpv_submit_bio_wrapper;
#endif
}

/* -- Module-level active-TPV list --------------------------------------
 *
 * All attached TPVs are registered here so that the MCS detach handler
 * can locate a struct nvmeibc_tpv* by UUID without needing the full
 * nvmeibc_volume infrastructure (TPVs bypass that path, see S.3.2).
 */
static LIST_HEAD(nvmeibc_tpv_active_list);
static DEFINE_SPINLOCK(nvmeibc_tpv_list_lock);

static void nvmeibc_tpv_list_add(struct nvmeibc_tpv *tpv)
{
	unsigned long flags;

	spin_lock_irqsave(&nvmeibc_tpv_list_lock, flags);
	list_add_tail(&tpv->list_node, &nvmeibc_tpv_active_list);
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
}

static void nvmeibc_tpv_list_remove(struct nvmeibc_tpv *tpv)
{
	unsigned long flags;

	spin_lock_irqsave(&nvmeibc_tpv_list_lock, flags);
	list_del_init(&tpv->list_node);
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
}

struct nvmeibc_tpv *nvmeibc_tpv_find_by_uuid(const char *uuid)
{
	struct nvmeibc_tpv *tpv;
	unsigned long flags;

	spin_lock_irqsave(&nvmeibc_tpv_list_lock, flags);
	list_for_each_entry(tpv, &nvmeibc_tpv_active_list, list_node) {
		if (strncmp(tpv->tpv_uuid, uuid, NVMEIBC_BD_UUID_LEN) == 0) {
			spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
			return tpv;
		}
	}
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
	return NULL;
}

/*
 * nvmeibc_tpv_detach_all_for_inst - detach every active TPV that belongs to
 * client instance @cinst (identified by tpv->cdv_vol->p).
 *
 * Must be called BEFORE the CDVs of the same instance are detached, so that
 * tpv->cdv_vol is still valid when we read its ->p field.  Called from
 * __detach_all_volumes_of_inst_work() in nvmeibc_main_capi_manipulate_vols.
 *
 * Each nvmeibc_tpv_detach() removes the TPV from nvmeibc_tpv_active_list
 * internally, so we restart the search from the list head after each detach.
 */
void nvmeibc_tpv_detach_all_for_inst(const struct nvmeibc_cinst_params_main *cinst)
{
	struct nvmeibc_tpv *tpv;
	unsigned long flags;

again:
	spin_lock_irqsave(&nvmeibc_tpv_list_lock, flags);
	list_for_each_entry(tpv, &nvmeibc_tpv_active_list, list_node) {
		bool data_hit = tpv->cdv_vol && tpv->cdv_vol->p == cinst;
		bool meta_hit = tpv->meta_cdv_vol && tpv->meta_cdv_vol->p == cinst;

		if (data_hit || meta_hit) {
			spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
			_NI(tpv_shutdown_detach,
			    "TPV: @STR detaching as part of instance shutdown (side=@STR)",
			    tpv->tpv_name, data_hit ? "data" : "meta");
			nvmeibc_tpv_detach(tpv);
			goto again;
		}
	}
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
}

/*
 * nvmeibc_tpv_handle_cdv_preempted - CDV preempted; tear down every TPV that
 * points at it (per-client CDV preempt cleanup barrier; S.2.10).
 *
 * Called from the CDV block-device status 'P' handler in nvmeibc_block.c when
 * the CDV enters NCBD_PREEMPTED. This happens in two cases:
 *   1. TOMA terminated this client's reg_ctx on the CDV (preemptClientFromCDV
 *      handler raised admission_floor + ran the termination).
 *   2. A REGISTER to a CDV segment was rejected with BELOW_CDV_FLOOR and the
 *      client-side handler (in nvmeibc_register.c) mapped the rejection to
 *      NCBD_PREEMPTED.
 *
 * Without this cleanup, the TPV's extent_map remains in memory and a
 * re-attached client could replay stale CDV offsets, defeating the preempt.
 *
 * The block-device status handler invokes us from the siw/RDMA recv path,
 * which runs in softirq (tasklet) context.  nvmeibc_tpv_detach() eventually
 * calls del_gendisk() -> bdev_mark_dead() -> invalidate_bh_lrus() ->
 * on_each_cpu_cond_mask(), which BUG_ON's if called from a non-sleepable
 * context (kernel/smp.c smp_call_function_many_cond WARN).  So we only
 * SNAPSHOT the CDV identity here and defer the teardown loop to a workqueue
 * that runs in process context.  The match is by UUID (string), which keeps
 * the deferred work safe against the CDV's nvmeibc_volume being torn down
 * between scheduling and firing.
 */
struct nvmeibc_tpv_cdv_preempt_ctx {
	struct work_struct work;
	char               cdv_uuid[NVMEIBC_BD_UUID_LEN];
};

static void nvmeibc_tpv_cdv_preempted_work_fn(struct work_struct *work)
{
	struct nvmeibc_tpv_cdv_preempt_ctx *ctx =
		container_of(work, struct nvmeibc_tpv_cdv_preempt_ctx, work);
	struct nvmeibc_tpv *tpv;
	unsigned long flags;

again:
	spin_lock_irqsave(&nvmeibc_tpv_list_lock, flags);
	list_for_each_entry(tpv, &nvmeibc_tpv_active_list, list_node) {
		bool data_hit = tpv->cdv_vol &&
			strncmp(tpv->cdv_vol->hdr.uuid, ctx->cdv_uuid,
				NVMEIBC_BD_UUID_LEN) == 0;
		bool meta_hit = tpv->meta_cdv_vol &&
			strncmp(tpv->meta_cdv_vol->hdr.uuid, ctx->cdv_uuid,
				NVMEIBC_BD_UUID_LEN) == 0;

		if (data_hit || meta_hit) {
			spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
			_NW(tpv_cdv_preempted,
			    "TPV: @STR tearing down due to parent CDV preempt (side=@STR)",
			    tpv->tpv_name, data_hit ? "data" : "meta");
			nvmeibc_tpv_detach(tpv);
			goto again;
		}
	}
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
	kfree(ctx);
}

void nvmeibc_tpv_handle_cdv_preempted(const struct nvmeibc_volume *cdv)
{
	struct nvmeibc_tpv_cdv_preempt_ctx *ctx;

	if (!cdv)
		return;

	/* Allocate in the caller's (possibly softirq) context.  On OOM the
	 * teardown is skipped - the CDV floor bump on TOMA still fences any
	 * further I/O, so this is best-effort cleanup rather than a correctness
	 * gate.  The TPV will be torn down on the next instance shutdown via
	 * nvmeibc_tpv_detach_all_for_inst(), or when the TPV's own detach
	 * lifecycle reaches it. */
	ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx) {
		_NE(tpv_cdv_preempt_nomem,
		    "CDV @STR preempt: deferred TPV teardown kmalloc failed",
		    cdv->hdr.uuid);
		return;
	}

	memcpy(ctx->cdv_uuid, cdv->hdr.uuid, NVMEIBC_BD_UUID_LEN);
	INIT_WORK(&ctx->work, nvmeibc_tpv_cdv_preempted_work_fn);
	schedule_work(&ctx->work);
}

/* -- Allocator helpers --------------------------------------------------- */

/*
 * Low-watermark: keep at least 50 MB worth of TPV_extents pre-allocated.
 * When free_tpv_extent_count drops below this, cdv_alloc_work fires.
 */
#define TPV_LOW_WATERMARK_MB		50ULL
#define TPV_LOW_WATERMARK_KB		(TPV_LOW_WATERMARK_MB * 1024ULL)

static u64 nvmeibc_tpv_calc_watermark(u32 tpv_extent_size_kb)
{
	return TPV_LOW_WATERMARK_KB / tpv_extent_size_kb;
}

static void nvmeibc_tpv_allocator_init(struct nvmeibc_tpv_allocator *alloc,
				       u32 tpv_extent_size_kb,
				       u64 virtual_size_bytes,
				       u32 cdv_extent_size_mib,
				       u64 allocator_size_gib)
{
	xa_init(&alloc->extent_map);
	spin_lock_init(&alloc->lock);

	alloc->tpv_extent_size_kb      = tpv_extent_size_kb;
	alloc->virtual_extents_total   = virtual_size_bytes /
					 ((u64)tpv_extent_size_kb << 10);

	alloc->cdv_extent_size_mib      = cdv_extent_size_mib;
	alloc->allocator_size_gib       = allocator_size_gib;

	INIT_LIST_HEAD(&alloc->cdv_extent_list);
	alloc->cdv_extents_count       = 0;

	INIT_LIST_HEAD(&alloc->free_tpv_extents);
	alloc->free_tpv_extent_count   = 0;

	INIT_LIST_HEAD(&alloc->pending_return_list);

	alloc->low_watermark           = nvmeibc_tpv_calc_watermark(tpv_extent_size_kb);

	/* Per-TPV L1/L2 tree tracking - populated by load_state or tpv_on_cdv_alloc_ok. */
	alloc->l1_extent_index         = 0;
	alloc->n_l2_tables_used        = 0;
	xa_init(&alloc->l1_to_l2_ctx);

	/*
	 * L1 dirty-page bitmap - one bit per 4 KB page of the L1 table slot.
	 * Allocated here because T (slot size) is known only after the
	 * allocator geometry fields above have been populated.  A NULL on
	 * OOM is tolerated: flush_state treats NULL as "mark all pages dirty"
	 * (effectively falling back to full-slot writes).
	 */
	{
		u64 T        = (u64)tpv_extent_size_kb << 10;
		u64 n_pages  = (T + 4095ULL) / 4096ULL;

		alloc->l1_dirty_pages = bitmap_zalloc(n_pages, GFP_KERNEL);
	}

	alloc->toma_extent_list        = NULL;
	alloc->toma_extent_count       = 0;
}

static void nvmeibc_tpv_allocator_free(struct nvmeibc_tpv_allocator *alloc)
{
	struct nvmeibc_cdv_extent_ref *ref, *tmp;
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;

	/* Free all extent_map entries. */
	xa_for_each(&alloc->extent_map, idx, entry)
		kfree(entry);
	xa_destroy(&alloc->extent_map);
	(void)idx;

	/* Free CDV_extent reference list (active and pending-return). */
	list_for_each_entry_safe(ref, tmp, &alloc->cdv_extent_list, node) {
		list_del(&ref->node);
		kfree(ref);
	}
	list_for_each_entry_safe(ref, tmp, &alloc->pending_return_list, node) {
		list_del(&ref->node);
		kfree(ref);
	}

	/*
	 * free_tpv_extents: list of struct nvmeibc_tpv_free_slot entries (type
	 * defined in nvmeibc_tpv_allocator.c).  Delegate to the allocator module.
	 */
	nvmeibc_tpv_free_slots_list(&alloc->free_tpv_extents);
	alloc->free_tpv_extent_count = 0;
	alloc->cdv_extents_count     = 0;

	/* Per-TPV L1/L2 tree cleanup.  Free each tpv_l2_ctx and its dirty-page
	 * bitmap before destroying the xarray. */
	{
		struct tpv_l2_ctx *ctx;
		unsigned long      li;

		xa_for_each(&alloc->l1_to_l2_ctx, li, ctx) {
			if (ctx) {
				bitmap_free(ctx->dirty_pages);
				kfree(ctx);
			}
		}
		xa_destroy(&alloc->l1_to_l2_ctx);
		(void)li;
	}
	bitmap_free(alloc->l1_dirty_pages);
	alloc->l1_dirty_pages = NULL;
	kvfree(alloc->toma_extent_list);
	alloc->toma_extent_list  = NULL;
	alloc->toma_extent_count = 0;
}

/* -- Block device registration ------------------------------------------- */

/*
 * Disk prefix used for TPV block devices: /dev/nvmesh-tpv/<name>
 * Matches the existing convention of other NVMesh volumes.
 */
#define NVMEIBC_TPV_DISK_PREFIX		"nvmesh-tpv"

static int nvmeibc_tpv_blkdev_register(struct nvmeibc_tpv *tpv)
{
	struct nvmeiba_atom_os_api *atom = &tpv->atom;
	struct gendisk       *disk  = NULL;
	struct request_queue *queue = NULL;
	sector_t              capacity;
	int                   rv    = 0;

	capacity = tpv->virtual_size >> KERNEL_SECTOR_SHIFT;

	/* -- 1. Allocate disk and queue (kernel-version-aware) ----------- */
#if KS_HAS_BLK_ALLOC_DISK
#  if KS_BLK_ALLOC_DISK_2PARAMS
	disk = blk_alloc_disk(NULL, NUMA_NO_NODE);
#  else
	disk = blk_alloc_disk(NUMA_NO_NODE);
#  endif
	if (IS_ERR_OR_NULL(disk)) {
		_NE(tpv_blkalloc_disk_fail, "TPV: blk_alloc_disk failed for @STR", tpv->tpv_name);
		return -ENOMEM;
	}
	queue = disk->queue;
#  if !KS_HAS_NEW_BLK_ALLOC_QUEUE && KS_REQUEST_QUEUE_HAS_REQUEST_FN
	blk_queue_make_request(queue, nvmeibc_tpv_make_request);
#  endif
#else	/* !KS_HAS_BLK_ALLOC_DISK */
	disk = alloc_disk(0);
	if (!disk) {
		_NE(tpv_alloc_disk_fail, "TPV: alloc_disk failed for @STR", tpv->tpv_name);
		return -ENOMEM;
	}
#  if KS_HAS_NEW_BLK_ALLOC_QUEUE
	queue = blk_alloc_queue(nvmeibc_tpv_make_request, NUMA_NO_NODE);
#  else
	queue = blk_alloc_queue(GFP_KERNEL);
#  endif
	if (!queue) {
		_NE(tpv_alloc_queue_fail, "TPV: blk_alloc_queue failed for @STR", tpv->tpv_name);
		put_disk(disk);
		return -ENOMEM;
	}
#  if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	blk_queue_make_request(queue, nvmeibc_tpv_make_request);
#  endif
	disk->queue = queue;
#endif	/* KS_HAS_BLK_ALLOC_DISK */

	/* -- 2. Wire ATOM and populate tpv_live_fops --------------------- */
	atom->disk  = disk;
	atom->queue = queue;
	atom->alloc_size = sizeof(struct nvmeibc_tpv);

	/*
	 * Initialise ATOM internal fields that kzalloc left zeroed.
	 * The regular volume path does this in __get_mem_for_os_api()
	 * (nvmeibc_block_api_os.c:2114-2125); we must replicate it here
	 * because nvmeiba_os_api_constructor() does not do it.
	 */
	spin_lock_init(&atom->disk_lock);
	atomic_set(&atom->gendisk_status, 1);
	INIT_LIST_HEAD(&atom->users.pids);
	spin_lock_init(&atom->users.lock);
	spin_lock_init(&atom->pender.lock);
	bio_list_init(&atom->pender.bio_list);
	INIT_LIST_HEAD(&atom->sub.part_list);
	atom->sub.parent = atom;	/* self-referencing for non-sub atoms */
	atom->attach_jiff = jiffies;

	nvmeibc_tpv_init_live_fops(&tpv->tpv_live_fops);
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	/* Old kernels: make_request_fn is already set above. */
#endif

	/* -- 3. Configure disk ------------------------------------------- */
	disk->major       = 0;
	disk->first_minor = 0;
	disk->minors      = 0;
	disk->flags      |= GENHD_FL_EXT_DEVT;
	disk->fops        = &tpv->tpv_live_fops;
	disk->private_data = tpv;
	queue->queuedata   = tpv;

	snprintf(disk->disk_name, DISK_NAME_LEN, "%s/%.20s",
		 NVMEIBC_TPV_DISK_PREFIX, tpv->tpv_name);

	strncpy(atom->dev_name, tpv->tpv_name, sizeof(atom->dev_name) - 1);
	atom->dev_name[sizeof(atom->dev_name) - 1] = '\0';

	/*
	 * Block size must match the CDV (4 KiB) so that every bio forwarded
	 * to the CDV is 4 KiB-aligned.  A 512-byte logical block size would
	 * allow sub-4 KiB writes that the CDV's RDMA transport and the
	 * target's NVMe command path cannot handle atomically - two
	 * concurrent sub-4 KiB writes to the same physical 4 KiB block race
	 * in the target's read-modify-write path, and one write is lost.
	 *
	 * chunk_sectors (512-byte units): guarantees each READ/WRITE bio
	 * arriving in nvmeibc_tpv_make_request is contained within one
	 * TPV_extent.
	 */
#if KS_BLK_ALLOC_DISK_2PARAMS
	/* 6.8+: blk_queue_* setters removed; write limits directly. */
	queue->limits.logical_block_size  = 4096;
	queue->limits.physical_block_size = 4096;
	queue->limits.io_min              = 4096;
	queue->limits.io_opt              = (unsigned int)tpv->allocator.tpv_extent_size_kb << 10;
	queue->limits.chunk_sectors =
		(unsigned int)((u64)tpv->allocator.tpv_extent_size_kb << 1);
#else
	blk_queue_logical_block_size(queue,  4096);
	blk_queue_physical_block_size(queue, 4096);
	blk_queue_io_min(queue, 4096);
	blk_queue_io_opt(queue, (unsigned int)tpv->allocator.tpv_extent_size_kb << 10);
	blk_queue_flag_set(QUEUE_FLAG_NONROT, queue);
	blk_queue_chunk_sectors(queue,
		(unsigned int)((u64)tpv->allocator.tpv_extent_size_kb << 1));
#endif

	/*
	 * Add with zero capacity first to avoid deadlock (see comment in
	 * __add_disk_io_starts_b4_func_ends in nvmeibc_block_api_os.c).
	 */
	set_capacity(disk, 0);

#if KS_ADD_DISK_INT_RV
	rv = add_disk(disk);
	if (rv) {
		_NE(tpv_add_disk_fail, "TPV: add_disk failed for @STR rv=@INT",
		    tpv->tpv_name, rv);
		goto err_put_disk;
	}
#else
	add_disk(disk);
#endif

	atomic_set(&atom->gendisk_status, 3);	/* gendisk added, IO possible */
	set_capacity(disk, capacity);		/* OS may start sending IO now */

	/* -- 4. Register with ATOM (adds to global atom list) ------------ */
	nvmeiba_os_api_constructor(atom);
	atom->status = nvmeiba_status_live;

	return 0;

#if KS_ADD_DISK_INT_RV
err_put_disk:
#endif
#if KS_HAS_BLK_CLEANUP_DISK
	blk_cleanup_disk(disk);
#else
	put_disk(disk);
#  if !KS_HAS_BLK_ALLOC_DISK
	blk_cleanup_queue(queue);
#  endif
#endif
	return rv;
}

static void nvmeibc_tpv_blkdev_unregister(struct nvmeibc_tpv *tpv)
{
	struct nvmeiba_atom_os_api *atom = &tpv->atom;

	if (!tpv_disk(tpv))
		return;

	/*
	 * del_gendisk marks the device as going away and prevents new
	 * references.  On kernels >=5.15 it also drains in-flight IO.
	 */
	del_gendisk(tpv_disk(tpv));

#if KS_HAS_BLK_CLEANUP_DISK
	/* blk_cleanup_disk does put_disk + queue cleanup */
	blk_cleanup_disk(tpv_disk(tpv));
#else
	put_disk(tpv_disk(tpv));
#  if !KS_HAS_BLK_ALLOC_DISK
	blk_cleanup_queue(tpv_queue(tpv));
#  endif
#endif

	atom->disk  = NULL;
	atom->queue = NULL;
}

/* -- nvmeibc_tpv_adopt - reconnect an orphaned TPV after NDU -----------
 *
 * Steps A1-A14 from the design (S.11.6).  Called from nvmeibc_tpv_attach()
 * when an ATOM orphan is found by name.  The allocator state (xarray,
 * free lists, CDV extent refs) all survive in memory; we only need to
 * reconnect the CDV, reinitialise work-struct function pointers (which
 * pointed to the old module text), and redirect BIOs back to our
 * make_request.
 */
static struct nvmeibc_tpv *nvmeibc_tpv_adopt(struct nvmeibc_tpv *tpv,
					      struct nvmeibc_volume *cdv,
					      const char *tpv_uuid,
					      bool sync_flush,
					      struct nvmeibc_volume *meta_cdv)
{
	unsigned long flags;
	(void)sync_flush;

	/* A1. Sanity: verify UUID matches. */
	if (strncmp(tpv->tpv_uuid, tpv_uuid, NVMEIBC_BD_UUID_LEN) != 0) {
		_NE(tpv_adopt_uuid_mismatch,
		    "TPV: orphan @STR UUID mismatch: expected @STR got @STR",
		    tpv->tpv_name, tpv_uuid, tpv->tpv_uuid);
		return NULL;
	}

	/* A2. Reconnect to the (now-adopted) CDV volume object(s). */
	tpv->cdv_vol = cdv;
	if (tpv->meta_cdv_vol || meta_cdv) {
		/* Split-mode orphan: restore meta_cdv_vol from the post-NDU
		 * volume list. meta_cdv may be NULL if the caller did not
		 * locate the metadata CDV in the newly-attached volumes; in
		 * that case the TPV's existing meta_cdv_vol is a dangling
		 * pointer - we clear it and the next tree write will fail
		 * cleanly with -ENODEV until the caller re-drives adopt with
		 * the metadata CDV pointer. */
		tpv->meta_cdv_vol = meta_cdv;
	}

	/* A3. Re-initialise work structs with new module's function pointers. */
	INIT_WORK(&tpv->cdv_alloc_work, nvmeibc_tpv_cdv_alloc_work_fn);
	INIT_WORK(&tpv->meta_cdv_alloc_work, nvmeibc_tpv_meta_cdv_alloc_work_fn);
	INIT_WORK(&tpv->persist_work,   nvmeibc_tpv_persist_work_fn);
	INIT_DELAYED_WORK(&tpv->load_state_work, nvmeibc_tpv_load_state_work_fn);
	INIT_DELAYED_WORK(&tpv->timeout_work, nvmeibc_tpv_timeout_work_fn);

	/* A4. Refresh allocator identity from CDV cache. */
	{
		unsigned long vflags;

		spin_lock_irqsave(&cdv->spinlock, vflags);
		strncpy(tpv->allocator_toma_id, cdv->cdv_allocator_toma_id,
			sizeof(tpv->allocator_toma_id) - 1);
		tpv->allocator_toma_id[sizeof(tpv->allocator_toma_id) - 1] = '\0';
		tpv->allocator_generation = cdv->cdv_allocator_generation;
		spin_unlock_irqrestore(&cdv->spinlock, vflags);
	}

	/* A5. Reconnect queue context. */
	tpv_queue(tpv)->queuedata = tpv;

	/* A6. Populate tpv_live_fops with new module's function pointers. */
	nvmeibc_tpv_init_live_fops(&tpv->tpv_live_fops);
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	tpv_queue(tpv)->make_request_fn = nvmeibc_tpv_make_request;
#endif

	/*
	 * A7. Atomically redirect new BIOs from ATOM's buffer to our
	 * make_request.  The pender.lock serialises with nvmeiba_b_req_push
	 * so no BIO is lost between redirect and drain.
	 */
	spin_lock_irqsave(&tpv->atom.pender.lock, flags);
	tpv_disk(tpv)->fops = &tpv->tpv_live_fops;
	wmb();
	spin_unlock_irqrestore(&tpv->atom.pender.lock, flags);

	/* A8. Drain ATOM pending list (BIOs that arrived during NDU window). */
	{
		struct bio *bio;

		while ((bio = bio_list_pop(&tpv->atom.pender.bio_list)) != NULL) {
			tpv->atom.pender.n_bios--;
			nvmeibc_tpv_make_request(tpv_queue(tpv), bio);
		}
	}

	/* A9. Retry TPV pending bios (parked before abandon). */
	nvmeibc_tpv_retry_pending_bios(tpv);

	/* A10. Re-add to module-local active list. */
	nvmeibc_tpv_list_add(tpv);

	/* A11. Re-create /proc entries. */
	nvmeibc_tpv_proc_register(tpv);

	/* A12. Resume normal operation. */
	atomic_set(&tpv->io_inflight, 0);
	atomic_set(&tpv->state, TPV_ATTACHED);
	tpv->atom.status = nvmeiba_status_live;

	/* A13. Kick background CDV extent pre-fetch if pool is low. */
	if (tpv->allocator.free_tpv_extent_count < tpv->allocator.low_watermark &&
	    !atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_work(&tpv->cdv_alloc_work);

	/* A14. Optionally reconcile state - schedule load_state for recovery. */
	if (tpv->dirty) {
		_NW(tpv_adopt_dirty,
		    "TPV: @STR adopted with dirty state; scheduling recovery",
		    tpv->tpv_name);
		schedule_delayed_work(&tpv->load_state_work, 0);
	}

	_NI(tpv_adopted,
	    "TPV: @STR (uuid=@STR) adopted after NDU; allocator toma=@STR gen=@LLU",
	    tpv->tpv_name, tpv->tpv_uuid,
	    tpv->allocator_toma_id, tpv->allocator_generation);

	return tpv;
}

/* -- nvmeibc_tpv_abandon_all_for_inst - NDU abandon all TPVs ----------
 *
 * Called from __detach_all_volumes_of_inst_work() when w->is_upgrade is
 * true.  For each active TPV belonging to @cinst, flush dirty state,
 * cancel workers, orphan the ATOM, and mark as TPV_ORPHAN.
 *
 * Must be called BEFORE CDV abandon (TPV flush issues IO to CDV).
 */
void nvmeibc_tpv_abandon_all_for_inst(const struct nvmeibc_cinst_params_main *cinst)
{
	struct nvmeibc_tpv *tpv;
	unsigned long flags;

again:
	spin_lock_irqsave(&nvmeibc_tpv_list_lock, flags);
	list_for_each_entry(tpv, &nvmeibc_tpv_active_list, list_node) {
		bool data_hit = tpv->cdv_vol && tpv->cdv_vol->p == cinst;
		bool meta_hit = tpv->meta_cdv_vol && tpv->meta_cdv_vol->p == cinst;

		if (data_hit || meta_hit) {
			spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);

			_NI(tpv_ndu_abandon,
			    "TPV: @STR abandoning for NDU (side=@STR)",
			    tpv->tpv_name, data_hit ? "data" : "meta");

			/* Step 1: Cancel background CDV_extent requests. */
			cancel_work_sync(&tpv->cdv_alloc_work);
			cancel_work_sync(&tpv->meta_cdv_alloc_work);

			/* Step 2: Cancel persist work before flush. */
			cancel_work_sync(&tpv->persist_work);

			/* Step 3: Cancel deferred state loader. */
			cancel_delayed_work_sync(&tpv->load_state_work);

			/* Step 4: Flush dirty allocator state to CDV. */
			if (tpv->dirty) {
				int rv = nvmeibc_tpv_flush_state(tpv);
				if (rv)
					_NW(tpv_ndu_flush_fail,
					    "TPV: @STR flush_state failed during NDU abandon rv=@INT; recovery will reconcile on adopt",
					    tpv->tpv_name, rv);
			}

			/* Step 5: Orphan the atom - ATOM buffers new BIOs.
			 * nvmeiba_os_api_orphan_abandon() swaps the fops and
			 * increments orphan counters but does NOT set the
			 * status field; the caller must transition it to
			 * nvmeiba_status_orphan (matches the regular volume
			 * __set_atom_status_orphan pattern).  Without this,
			 * nvmeiba_os_do_on_nvmeibc_down hits WARN 1007 because
			 * the atom is still marked nvmeiba_status_live.
			 */
			nvmeiba_os_api_orphan_abandon(&tpv->atom);
			tpv->atom.status = nvmeiba_status_orphan;

			/*
			 * Step 6: Drain in-flight IOs.  After orphan_abandon,
			 * no new BIOs enter nvmeibc_tpv_make_request (they go
			 * to ATOM's buffer).  Wait for currently-executing
			 * make_request calls to finish.
			 */
			while (atomic_read(&tpv->io_inflight))
				msleep(1);

			/* Step 7: Disconnect nvmeibc context. */
			tpv_queue(tpv)->queuedata = NULL;

			/* Step 8: CDV will be abandoned separately. */
			tpv->cdv_vol = NULL;

			/* Step 9: Remove from module-local active list. */
			nvmeibc_tpv_list_remove(tpv);

			/* Step 10: Remove /proc entries (they belong to nvmeibc). */
			nvmeibc_tpv_proc_deregister(tpv);

			/* Step 11: Mark as orphaned. */
			atomic_set(&tpv->state, TPV_ORPHAN);

			_NI(tpv_ndu_abandoned, "TPV: @STR orphaned for NDU",
			    tpv->tpv_name);

			goto again;
		}
	}
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
}

/* -- nvmeibc_tpv_attach -------------------------------------------------
 *
 * Called from nvmeibc_main_capi_manipulate_vols.inc.c when volume_class
 * == NVC_TPV in an AttachVolumes MCS message.  The CDV must already be
 * attached as a hidden volume before this is invoked.
 */
struct nvmeibc_tpv *nvmeibc_tpv_attach(struct nvmeibc_volume *cdv,
					const char *tpv_name,
					const char *tpv_uuid,
					u64 virtual_size_bytes,
					u32 tpv_extent_size_kb,
					u32 cdv_extent_size_mib,
					u64 allocator_size_gib,
					bool sync_flush,
					struct nvmeibc_volume *meta_cdv,
					u32 meta_tpv_extent_size_kb,
					u32 meta_cdv_extent_size_mib)
{
	struct nvmeibc_tpv *tpv;
	int rv;

	if (WARN_ON(!cdv || !tpv_uuid || !tpv_name || !virtual_size_bytes ||
		    !tpv_extent_size_kb))
		return NULL;

	/* Split-mode validation. */
	if (meta_cdv) {
		if (WARN_ON(meta_cdv == cdv ||
			    !meta_tpv_extent_size_kb ||
			    !meta_cdv_extent_size_mib))
			return NULL;
	}

	/* -- 0a. Idempotency: return existing TPV if already attached ----
	 *
	 * Management may re-send an attach command (e.g. after a keepalive
	 * gap or status-reporting race).  If the TPV is already in the
	 * active list, return it so the caller sends ACK_ATTACHED again
	 * without touching the disk or allocator.
	 *
	 * Also refresh cdv_vol in case the CDV was detached and re-attached
	 * (new nvmeibc_volume object) while the TPV was still live - without
	 * this, tpv->cdv_vol would be a dangling pointer.
	 */
	{
		struct nvmeibc_tpv *existing = nvmeibc_tpv_find_by_uuid(tpv_uuid);

		if (existing) {
			_NI(tpv_already_attached,
			    "TPV: @STR already attached; returning existing tpv",
			    tpv_name);
			if (existing->cdv_vol != cdv) {
				_NI(tpv_cdv_ptr_refreshed,
				    "TPV: @STR refreshing stale cdv_vol pointer",
				    tpv_name);
				existing->cdv_vol = cdv;
			}
			return existing;
		}
	}

	/* -- 0b. NDU orphan: check ATOM for an orphaned TPV with matching name */
	{
		char dev_name[DISK_NAME_LEN];
		struct nvmeiba_atom_os_api *orphan_atom;

		snprintf(dev_name, sizeof(dev_name), "%.30s", tpv_name);
		orphan_atom = nvmeiba_os_api_orphan_adopt(NVMEIBC_TPV_DISK_PREFIX,
							  dev_name);
		if (orphan_atom) {
			struct nvmeibc_tpv *orphan_tpv = container_of(orphan_atom,
								      struct nvmeibc_tpv, atom);
			_NI(tpv_ndu_orphan_found,
			    "TPV: @STR found NDU orphan atom; adopting",
			    tpv_name);
			return nvmeibc_tpv_adopt(orphan_tpv, cdv, tpv_uuid,
						 sync_flush, meta_cdv);
		}
	}

	/* -- 1. Verify CDV is attached ------------------------------------ */
	/*
	 * The CDV must already be in the volumes list (attached hidden,
	 * SHARED_RW, managed by nvmeibc_main_capi_manipulate_vols.inc.c).
	 * We received its pointer from the caller who looked it up.
	 */
	if (WARN_ON(cdv->status != NVS_ATTACHED &&
		    cdv->status != NVS_ATTACHING_HAVE_BDEV)) {
		_NE(tpv_cdv_not_attached, "TPV: CDV @STR not yet ATTACHED status=@INT",
		    cdv->hdr.uuid, cdv->status);
		return NULL;
	}

	/* -- 2. Allocate struct nvmeibc_tpv ------------------------------- */
	tpv = kzalloc(sizeof(*tpv), GFP_KERNEL);
	if (!tpv) {
		_NE(tpv_attach_kzalloc_fail, "TPV: kzalloc failed for @STR", tpv_name);
		return NULL;
	}

	tpv->cdv_vol     = cdv;
	tpv->meta_cdv_vol = meta_cdv;	/* NULL for single-CDV mode */
	tpv->virtual_size = virtual_size_bytes;
	strncpy(tpv->tpv_uuid, tpv_uuid, sizeof(tpv->tpv_uuid) - 1);
	strncpy(tpv->tpv_name, tpv_name, sizeof(tpv->tpv_name) - 1);
	atomic_set(&tpv->state, TPV_ATTACHING);

	/*
	 * Seed allocator identity from the CDV volume's cached value.
	 *
	 * The CDV_ALLOCATOR_UPDATE topology push arrives when the CDV segment
	 * is first registered, which happens BEFORE the TPV is attached.
	 * nvmeibc_topology.c caches the identity on cdv->cdv_allocator_toma_id
	 * so we can pick it up here instead of waiting for the next push.
	 * If the CDV cache is still empty (very early attach before any push),
	 * the work function will defer until the next CDV_ALLOCATOR_UPDATE.
	 */
	spin_lock_init(&tpv->allocator_id_lock);
	spin_lock_init(&tpv->meta_allocator_id_lock);
	{
		unsigned long vflags;

		spin_lock_irqsave(&cdv->spinlock, vflags);
		strncpy(tpv->allocator_toma_id, cdv->cdv_allocator_toma_id,
			sizeof(tpv->allocator_toma_id) - 1);
		tpv->allocator_toma_id[sizeof(tpv->allocator_toma_id) - 1] = '\0';
		tpv->allocator_generation = cdv->cdv_allocator_generation;
		spin_unlock_irqrestore(&cdv->spinlock, vflags);
	}
	if (meta_cdv) {
		unsigned long vflags;

		spin_lock_irqsave(&meta_cdv->spinlock, vflags);
		strncpy(tpv->meta_allocator_toma_id, meta_cdv->cdv_allocator_toma_id,
			sizeof(tpv->meta_allocator_toma_id) - 1);
		tpv->meta_allocator_toma_id[sizeof(tpv->meta_allocator_toma_id) - 1] = '\0';
		tpv->meta_allocator_generation = meta_cdv->cdv_allocator_generation;
		spin_unlock_irqrestore(&meta_cdv->spinlock, vflags);
	}
	if (tpv->allocator_toma_id[0])
		_NI(tpv_allocator_seeded,
		    "TPV @STR: seeded allocator from CDV cache: toma=@STR gen=@LLU",
		    tpv_name, tpv->allocator_toma_id, tpv->allocator_generation);
	else
		_ND(tpv_allocator_no_cache,
		    "TPV @STR: CDV allocator not yet known; will wait for CDV_ALLOCATOR_UPDATE push",
		    tpv_name);
	if (meta_cdv) {
		if (tpv->meta_allocator_toma_id[0])
			_NI(tpv_meta_allocator_seeded,
			    "TPV @STR: seeded meta allocator from metaCDV cache: toma=@STR gen=@LLU",
			    tpv_name, tpv->meta_allocator_toma_id, tpv->meta_allocator_generation);
		else
			_ND(tpv_meta_allocator_no_cache,
			    "TPV @STR: metaCDV allocator not yet known; will wait for CDV_ALLOCATOR_UPDATE push",
			    tpv_name);
	}

	spin_lock_init(&tpv->persist_lock);
	tpv->dirty = false;

	INIT_LIST_HEAD(&tpv->list_node);
	INIT_WORK(&tpv->cdv_alloc_work, nvmeibc_tpv_cdv_alloc_work_fn);
	/* Two distinct work functions (data vs. meta) so container_of in each
	 * handler unambiguously resolves to the correct embedded work_struct.
	 * See nvmeibc_tpv_{cdv,meta_cdv}_alloc_work_fn in nvmeibc_tpv_allocator.c. */
	INIT_WORK(&tpv->meta_cdv_alloc_work, nvmeibc_tpv_meta_cdv_alloc_work_fn);
	INIT_WORK(&tpv->persist_work,   nvmeibc_tpv_persist_work_fn);
	INIT_DELAYED_WORK(&tpv->load_state_work, nvmeibc_tpv_load_state_work_fn);
	INIT_DELAYED_WORK(&tpv->timeout_work, nvmeibc_tpv_timeout_work_fn);
	tpv->state_loaded = false;
	atomic_set(&tpv->cdv_alloc_pending, 0);
	atomic_set(&tpv->meta_cdv_alloc_pending, 0);
	atomic_set(&tpv->io_inflight, 0);

	/*
	 * Start with the attach timeout.  When io_max_retry_secs is 0
	 * (default), fall back to 30 s - same as IO_TIME_OUT_ATTACH for
	 * regular volumes.  Upgraded to the normal (long) timeout by
	 * load_state_work_fn after state_loaded is set.
	 */
	tpv->max_retry_jiffies =
		(nvmeibc_io_max_retry_secs ? : TPV_IO_TIME_OUT_ATTACH) * HZ;

	bio_list_init(&tpv->pending_bios);
	bio_list_init(&tpv->pending_l1_flush_bios);
	spin_lock_init(&tpv->pending_bio_lock);
	tpv->sync_flush = sync_flush;

	/* -- 3a. Initialise allocator(s) ---------------------------------
	 *
	 * Data-side allocator always initialised.
	 *
	 * Split-mode: allocate a second allocator for the metadata CDV. It
	 * manages L2-table slots on the metadata CDV (and will hold the L1
	 * extent). The L1 bookkeeping fields (l1_extent_index,
	 * n_l2_tables_used, l1_to_l2_ctx, l1_dirty_pages) live on whichever
	 * allocator owns the L1 extent - nvmeibc_tpv_meta_alloc() returns
	 * that allocator and is used throughout the persist/recovery paths.
	 *
	 * virtual_extents_total on the meta allocator is the number of L2
	 * slots the metadata CDV can host, not the TPV's virtual extent
	 * count. We pass the TPV's virtual size so the struct's book-keeping
	 * of the TPV's logical range stays consistent, but the meta allocator
	 * uses it only for the xarray key range (no xarray entries are
	 * stored there - the xarray holding virt_idx -> extent_entry is on
	 * the data-side allocator).
	 */
	nvmeibc_tpv_allocator_init(&tpv->allocator, tpv_extent_size_kb,
				   virtual_size_bytes, cdv_extent_size_mib,
				   allocator_size_gib);

	if (meta_cdv) {
		tpv->meta_allocator = kzalloc(sizeof(*tpv->meta_allocator), GFP_KERNEL);
		if (!tpv->meta_allocator) {
			_NE(tpv_attach_meta_alloc_kzalloc_fail,
			    "TPV @STR: kzalloc meta_allocator failed", tpv_name);
			goto err_free_alloc;
		}
		nvmeibc_tpv_allocator_init(tpv->meta_allocator,
					   meta_tpv_extent_size_kb,
					   virtual_size_bytes,
					   meta_cdv_extent_size_mib,
					   /* allocator_size_gib */ 0);
	}

	/* -- 3a-check. Verify 2-level L1/L2 tree can address all virtual extents. */
	{
		u64 T      = (u64)tpv_extent_size_kb << 10;
		u64 n_l1   = (T - sizeof(struct tpv_l1_header)) /
			     sizeof(struct tpv_tree_entry);
		u64 n_l2   = T / sizeof(struct tpv_tree_entry);
		u64 n_sl   = ((u64)cdv_extent_size_mib << 20) / T;
		u64 max_ve = n_l1 * n_l2 * n_sl;

		if (tpv->allocator.virtual_extents_total > max_ve) {
			_NE(tpv_tree_capacity_exceeded,
			    "TPV: @STR: virtual_extents @LLU exceeds 2-level tree capacity @LLU",
			    tpv_name,
			    tpv->allocator.virtual_extents_total,
			    max_ve);
			goto err_free_alloc;
		}
	}

	/*
	 * -- 3b. Schedule deferred load of allocator state --------------
	 *
	 * load_state (read per-TPV tree from CDV), recovery (TOMA orphan
	 * check), and the initial CDV_extent pre-allocation run in the
	 * background via load_state_work.  IO arriving before load completes
	 * is parked on pending_bios and drained once state_loaded is set.
	 *
	 * This allows attach to succeed even when the CDV block device is
	 * temporarily unavailable (transport flap, CDV still attaching).
	 * The worker retries until CDV IO succeeds.
	 */
	schedule_delayed_work(&tpv->load_state_work, 0);

	/* -- 4. Register block device and open IO gates ------------------- */
	rv = nvmeibc_tpv_blkdev_register(tpv);
	if (rv) {
		_NE(tpv_blkdev_register_fail, "TPV: blkdev_register failed for @STR rv=@INT",
		    tpv_name, rv);
		goto err_free_alloc;
	}

	nvmeibc_tpv_list_add(tpv);
	atomic_set(&tpv->state, TPV_ATTACHED);

	nvmeibc_tpv_proc_register(tpv);

	_NI(tpv_attached,
	    "TPV: @STR (uuid=@STR) attached vsize=@LLU MB tpv_ext=@UINT KB cdv_ext=@UINT MB alloc=@LLU GB wmark=@LLU sync_flush=@INT split=@INT",
	    tpv_name, tpv_uuid,
	    virtual_size_bytes >> 20,
	    tpv_extent_size_kb,
	    cdv_extent_size_mib,
	    allocator_size_gib,
	    tpv->allocator.low_watermark,
	    (int)tpv->sync_flush,
	    (int)(meta_cdv != NULL));

	if (meta_cdv) {
		_NI(tpv_meta_attached,
		    "TPV @STR: split-mode meta CDV @STR meta_tpv_ext=@UINT KB meta_cdv_ext=@UINT MB",
		    tpv_name, meta_cdv->hdr.uuid,
		    meta_tpv_extent_size_kb,
		    meta_cdv_extent_size_mib);
	}

	return tpv;

err_free_alloc:
	cancel_delayed_work_sync(&tpv->load_state_work);
	cancel_delayed_work_sync(&tpv->timeout_work);
	/* Unregister the block device if blkdev_register already succeeded. */
	nvmeibc_tpv_blkdev_unregister(tpv);
	if (tpv->meta_allocator) {
		nvmeibc_tpv_allocator_free(tpv->meta_allocator);
		kfree(tpv->meta_allocator);
		tpv->meta_allocator = NULL;
	}
	nvmeibc_tpv_allocator_free(&tpv->allocator);
	kfree(tpv);
	return NULL;
}

/* -- nvmeibc_tpv_detach -------------------------------------------------
 *
 * Called from nvmeibc_main_capi_manipulate_vols.inc.c when a DetachVolumes
 * MCS message arrives for a TPV UUID.
 */
void nvmeibc_tpv_detach(struct nvmeibc_tpv *tpv)
{
	if (WARN_ON(!tpv))
		return;

	atomic_set(&tpv->state, TPV_DETACHING);
	nvmeibc_tpv_list_remove(tpv);

	/*
	 * Shorten the pending-bio timeout so any parked bios are failed
	 * quickly (10 ms), matching regular volume detach behaviour.
	 * mod_delayed_work re-arms timeout_work at the new short delay
	 * if it was pending; otherwise it schedules a new firing.
	 */
	tpv->max_retry_jiffies = HZ / 100;
	mod_delayed_work(system_wq, &tpv->timeout_work, tpv->max_retry_jiffies);

	/* -- 1. Remove from active list and cancel pending work ---------- */
	cancel_delayed_work_sync(&tpv->load_state_work);
	cancel_work_sync(&tpv->cdv_alloc_work);
	cancel_work_sync(&tpv->meta_cdv_alloc_work);
	cancel_work_sync(&tpv->persist_work);
	cancel_delayed_work_sync(&tpv->timeout_work);

	/* -- 2. Flush dirty allocator state synchronously ----------------- */
	if (tpv->dirty) {
		int rv = nvmeibc_tpv_flush_state(tpv);

		if (rv)
			_NW(tpv_detach_flush_fail,
			    "TPV: flush_state failed for @STR rv=@INT; state may be lost",
			    tpv->tpv_name, rv);
	}

	/* -- 3. Unregister block device (quiesces IO via queue freeze) ---- */
	nvmeibc_tpv_blkdev_unregister(tpv);

	/*
	 * -- 3b. Fail any bios parked waiting for CDV_extent allocation --
	 *
	 * Safe after blkdev_unregister: del_gendisk + queue cleanup
	 * guarantee that no make_request call is in-flight, so no new
	 * bios can be added to pending_bios after this point.
	 */
	{
		struct bio_list  pending;
		struct bio      *bio;
		unsigned long    flags;

		bio_list_init(&pending);
		spin_lock_irqsave(&tpv->pending_bio_lock, flags);
		bio_list_merge(&pending, &tpv->pending_bios);
		bio_list_init(&tpv->pending_bios);
		bio_list_merge(&pending, &tpv->pending_l1_flush_bios);
		bio_list_init(&tpv->pending_l1_flush_bios);
		spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

		while ((bio = bio_list_pop(&pending)) != NULL)
			bio_endio(bio, -EIO);
	}

	/* -- 4. Deregister proc entries and free allocator state ---------- */
	nvmeibc_tpv_proc_deregister(tpv);
	if (tpv->meta_allocator) {
		nvmeibc_tpv_allocator_free(tpv->meta_allocator);
		kfree(tpv->meta_allocator);
		tpv->meta_allocator = NULL;
	}
	nvmeibc_tpv_allocator_free(&tpv->allocator);

	_NI(tpv_detached, "TPV: @STR (uuid=@STR) detached", tpv->tpv_name, tpv->tpv_uuid);

	/* -- 5. MCS notification is sent by the caller (the MCS handler) -- */
	/*
	 * The MCS detach-completion reply is sent by
	 * nvmeibc_main_capi_manipulate_vols.inc.c after we return, using
	 * the same completion path as regular volumes.
	 */

	/*
	 * ATOM lifecycle: del_gendisk (in blkdev_unregister above) sets
	 * atom->disk = NULL.  When the last open handle closes, ATOM's
	 * __dec_ref_and_destroy_if_needed() sees (n_opens == 0 && disk == NULL)
	 * and calls nvmeiba_os_api_destructor(), which removes the atom from
	 * the global list and kfree's it (== tpv, since atom is at offset 0).
	 *
	 * If no handles are currently open, nobody will trigger that path,
	 * so we must call the destructor ourselves.
	 */
	if (atomic_read(&tpv->atom.users.n_opens) == 0)
		nvmeiba_os_api_destructor(&tpv->atom);
}

/* -- nvmeibc_tpv_grow ---------------------------------------------------
 *
 * Handle UpdateVolume MCS for a TPV (S.3.10): management has extended the
 * TPV's virtual size.  Update in-kernel state and gendisk capacity.
 * No allocator flush needed - new extents start unmapped (reads = zero).
 */
void nvmeibc_tpv_grow(struct nvmeibc_tpv *tpv, u64 new_virtual_size_bytes)
{
	u64 new_total_extents;

	if (WARN_ON(!tpv || new_virtual_size_bytes <= tpv->virtual_size))
		return;

	new_total_extents = new_virtual_size_bytes /
			    ((u64)tpv->allocator.tpv_extent_size_kb << 10);

	spin_lock(&tpv->allocator.lock);
	tpv->virtual_size                     = new_virtual_size_bytes;
	tpv->allocator.virtual_extents_total  = new_total_extents;
	spin_unlock(&tpv->allocator.lock);

	/*
	 * Split mode: keep meta_allocator->virtual_extents_total in sync with
	 * the grown virtual size. The field is not consulted on the IO hot
	 * path (L2 tables are demand-allocated from the meta free pool), but
	 * /proc and future tree-capacity checks read it, so mismatched values
	 * would mislead. Use the meta-side tpv_extent_size_kb - it may differ
	 * from the data side.
	 */
	if (tpv->meta_allocator) {
		u64 meta_total = new_virtual_size_bytes /
				 ((u64)tpv->meta_allocator->tpv_extent_size_kb << 10);

		spin_lock(&tpv->meta_allocator->lock);
		tpv->meta_allocator->virtual_extents_total = meta_total;
		spin_unlock(&tpv->meta_allocator->lock);

		_NI(tpv_meta_grown,
		    "TPV: @STR meta allocator virtual_extents_total=@LLU",
		    tpv->tpv_name, meta_total);
	}

	/*
	 * Verify the 2-level tree can still address the grown virtual size.
	 * Management refuses extends that would overflow the tree, so this
	 * should never fire; log loudly if it does to flag the protocol bug.
	 * Uses the tree-owning allocator's geometry (meta in split mode).
	 */
	{
		struct nvmeibc_tpv_allocator *tree = nvmeibc_tpv_meta_alloc(tpv);
		u64 T      = (u64)tree->tpv_extent_size_kb << 10;
		u64 n_l1   = (T - sizeof(struct tpv_l1_header)) /
			     sizeof(struct tpv_tree_entry);
		u64 n_l2   = T / sizeof(struct tpv_tree_entry);
		u64 n_sl   = ((u64)tree->cdv_extent_size_mib << 20) / T;
		u64 max_ve = n_l1 * n_l2 * n_sl;

		if (new_total_extents > max_ve)
			_NE(tpv_grow_tree_overflow,
			    "TPV: @STR grow to @LLU virt extents exceeds 2-level tree cap @LLU; writes beyond cap will fail",
			    tpv->tpv_name, new_total_extents, max_ve);
	}

	if (tpv_disk(tpv))
		set_capacity(tpv_disk(tpv),
			     new_virtual_size_bytes >> KERNEL_SECTOR_SHIFT);

	_NI(tpv_grown, "TPV: @STR grown to @LLU MB (@LLU virtual extents)",
	    tpv->tpv_name, new_virtual_size_bytes >> 20, new_total_extents);
}

/* -- Allocator identity update (called on CDV topology push) ------------- */

/*
 * nvmeibc_tpv_update_allocator_id - update the CDV.allocator TOMA identity.
 *
 * Called when TOMA pushes an updated CDV topology (new allocator elected or
 * generation incremented).  Clients must fence in-flight CDV_ALLOC_EXTENT
 * requests with the old generation.
 */
void nvmeibc_tpv_update_allocator_id(struct nvmeibc_tpv *tpv,
				     const char *toma_id,
				     u64 generation)
{
	unsigned long flags;

	spin_lock_irqsave(&tpv->allocator_id_lock, flags);
	strncpy(tpv->allocator_toma_id, toma_id,
		sizeof(tpv->allocator_toma_id) - 1);
	tpv->allocator_generation = generation;
	spin_unlock_irqrestore(&tpv->allocator_id_lock, flags);
}
EXPORT_SYMBOL(nvmeibc_tpv_update_allocator_id);

/*
 * nvmeibc_tpv_update_meta_allocator_id - mirror of nvmeibc_tpv_update_allocator_id
 * for the metadata-side allocator identity (split-mode TPVs). Keeps the two
 * sides symmetric and gives callers a single named entry point per side
 * instead of open-coding the lock acquisition.
 */
void nvmeibc_tpv_update_meta_allocator_id(struct nvmeibc_tpv *tpv,
					   const char *toma_id,
					   u64 generation)
{
	unsigned long flags;

	spin_lock_irqsave(&tpv->meta_allocator_id_lock, flags);
	strncpy(tpv->meta_allocator_toma_id, toma_id,
		sizeof(tpv->meta_allocator_toma_id) - 1);
	tpv->meta_allocator_toma_id[sizeof(tpv->meta_allocator_toma_id) - 1] = '\0';
	tpv->meta_allocator_generation = generation;
	spin_unlock_irqrestore(&tpv->meta_allocator_id_lock, flags);
}
EXPORT_SYMBOL(nvmeibc_tpv_update_meta_allocator_id);

/*
 * nvmeibc_tpv_update_allocator_for_cdv - update allocator for all TPVs on a CDV.
 *
 * Iterates the active TPV list, finds all TPVs whose parent CDV UUID matches,
 * and calls nvmeibc_tpv_update_allocator_id() + re-arms cdv_alloc_work on each.
 */
void nvmeibc_tpv_update_allocator_for_cdv(const char *cdv_uuid,
					   const char *toma_id,
					   u64 generation)
{
	struct nvmeibc_tpv *tpv;
	unsigned long flags;
	int n_updated = 0;

	spin_lock_irqsave(&nvmeibc_tpv_list_lock, flags);
	list_for_each_entry(tpv, &nvmeibc_tpv_active_list, list_node) {
		bool data_match = tpv->cdv_vol &&
			strncmp(tpv->cdv_vol->hdr.uuid, cdv_uuid,
				NVMEIBC_BD_UUID_LEN) == 0;
		bool meta_match = tpv->meta_cdv_vol &&
			strncmp(tpv->meta_cdv_vol->hdr.uuid, cdv_uuid,
				NVMEIBC_BD_UUID_LEN) == 0;

		if (!data_match && !meta_match)
			continue;

		_NI(tpv_allocator_cdv_update,
		    "TPV @STR: CDV allocator update cdv=@STR toma=@STR gen=@LLU side=@STR",
		    tpv->tpv_name, cdv_uuid, toma_id, generation,
		    data_match ? "data" : "meta");

		if (data_match) {
			nvmeibc_tpv_update_allocator_id(tpv, toma_id, generation);
			if (!READ_ONCE(tpv->state_loaded))
				schedule_delayed_work(&tpv->load_state_work, 0);
			else if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
				schedule_work(&tpv->cdv_alloc_work);
		} else {
			nvmeibc_tpv_update_meta_allocator_id(tpv, toma_id, generation);

			if (!READ_ONCE(tpv->state_loaded))
				schedule_delayed_work(&tpv->load_state_work, 0);
			else if (!atomic_xchg(&tpv->meta_cdv_alloc_pending, 1))
				schedule_work(&tpv->meta_cdv_alloc_work);
		}
		n_updated++;
	}
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);

	if (n_updated == 0)
		_ND(tpv_allocator_cdv_no_match,
		    "TPV: CDV allocator update cdv=@STR toma=@STR gen=@LLU -- no matching TPVs",
		    cdv_uuid, toma_id, generation);
}
EXPORT_SYMBOL(nvmeibc_tpv_update_allocator_for_cdv);

