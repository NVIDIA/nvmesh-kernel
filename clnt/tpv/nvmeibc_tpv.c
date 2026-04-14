/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv.c — TPV volume attach/detach and block device registration.
 *
 * A Thin-Provisioned Volume (TPV) presents a sparse virtual address space
 * to a single exclusive client.  Physical storage is provided by a hidden
 * Carrier Direct Volume (CDV).  The mapping is maintained by the client-local
 * TPV.allocator (nvmeibc_tpv_allocator.c) and persisted in CDV_extent[0]
 * (nvmeibc_tpv_persist.c).
 *
 * Attach flow (§3.8):
 *   1. Assert CDV is present in the volumes list.
 *   2. Allocate and initialise struct nvmeibc_tpv.
 *   3. Load allocator state from CDV_extent[0] (nvmeibc_tpv_load_state).
 *   4. Run recovery if tree inconsistency detected (nvmeibc_tpv_recovery).
 *   5. Set low-watermark; schedule initial CDV_extent request if pool empty.
 *   6. Register gendisk.  Open IO gates.
 *
 * Detach flow (§3.8):
 *   1. Quiesce IO (freeze queue).
 *   2. Flush dirty allocator state to CDV_extent[0] (nvmeibc_tpv_flush_state).
 *   3. Unregister gendisk.
 *   4. Free allocator (xarray + extent lists).
 *   5. Notify management via MCS.
 */

#include "common/kr_incs.h"			/* kernel headers, logging, u64, etc. */
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"		/* nvmeibc_volume, nvmeibc_volume_get_by_uuid */
#include "common/nvmeib_common_os_block_api.h"	/* REQ_RET, REQ_RET_ZERO */
#include "clnt/nvmeibc_block.h"			/* KERNEL_SECTOR_SHIFT */

/* ── Forward declarations for sibling implementation files ────────────── */

/* nvmeibc_tpv_io.c */
extern REQ_RET nvmeibc_tpv_make_request(struct request_queue *q, struct bio *bio);

/* ── Block device fops ─────────────────────────────────────────────────── */

/*
 * On kernels without make_request_fn (≥5.9), submit_bio fops takes a single
 * (struct bio *) argument, not (queue *, bio *).  Bridge the gap here so
 * nvmeibc_tpv_make_request keeps the canonical (q, bio) signature used by all
 * NVMesh make_request implementations.
 */
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
static REQ_RET nvmeibc_tpv_submit_bio_wrapper(struct bio *bio)
{
	struct nvmeibc_tpv *tpv = bio_gendisk(bio)->private_data;

	return nvmeibc_tpv_make_request(tpv->queue, bio);
}
#endif

static const struct block_device_operations nvmeibc_tpv_fops = {
	.owner     = THIS_MODULE,
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	.submit_bio = nvmeibc_tpv_submit_bio_wrapper,
#endif
};

/* ── Module-level active-TPV list ──────────────────────────────────────
 *
 * All attached TPVs are registered here so that the MCS detach handler
 * can locate a struct nvmeibc_tpv* by UUID without needing the full
 * nvmeibc_volume infrastructure (TPVs bypass that path, see §3.2).
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
 * nvmeibc_tpv_detach_all_for_inst — detach every active TPV that belongs to
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
		if (tpv->cdv_vol && tpv->cdv_vol->p == cinst) {
			spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
			_NI(tpv_shutdown_detach, "TPV: @STR detaching as part of instance shutdown",
			    tpv->tpv_name);
			nvmeibc_tpv_detach(tpv);
			goto again;
		}
	}
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);
}

/* ── Allocator helpers ─────────────────────────────────────────────────── */

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
				       u32 cdv_extent_size_mb,
				       u64 allocator_size_gb)
{
	xa_init(&alloc->extent_map);
	spin_lock_init(&alloc->lock);

	alloc->tpv_extent_size_kb      = tpv_extent_size_kb;
	alloc->virtual_extents_total   = virtual_size_bytes /
					 ((u64)tpv_extent_size_kb << 10);

	alloc->cdv_extent_size_mb      = cdv_extent_size_mb;
	alloc->allocator_size_gb       = allocator_size_gb;

	INIT_LIST_HEAD(&alloc->cdv_extent_list);
	alloc->cdv_extents_count       = 0;

	INIT_LIST_HEAD(&alloc->free_tpv_extents);
	alloc->free_tpv_extent_count   = 0;

	INIT_LIST_HEAD(&alloc->pending_return_list);

	alloc->low_watermark           = nvmeibc_tpv_calc_watermark(tpv_extent_size_kb);
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
}

/* ── Block device registration ─────────────────────────────────────────── */

/*
 * Disk prefix used for TPV block devices: /dev/nvmesh-tpv/<name>
 * Matches the existing convention of other NVMesh volumes.
 */
#define NVMEIBC_TPV_DISK_PREFIX		"nvmesh-tpv"

static int nvmeibc_tpv_blkdev_register(struct nvmeibc_tpv *tpv)
{
	struct gendisk       *disk  = NULL;
	struct request_queue *queue = NULL;
	sector_t              capacity;
	int                   rv    = 0;

	capacity = tpv->virtual_size >> KERNEL_SECTOR_SHIFT;

	/* ── Allocate disk and queue (kernel-version-aware) ─────────────── */
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

	/* ── Configure disk ─────────────────────────────────────────────── */
	/*
	 * Use extended devt (blkext) for dynamic major assignment.
	 * major=0 + minors=0 + GENHD_FL_EXT_DEVT is the required combination;
	 * major=0 with minors>0 is invalid and causes add_disk() to return
	 * -EINVAL.  This mirrors what nvmeibc_block_api_os.c does when
	 * nvmeibc_use_block_external_major is true.
	 */
	disk->major       = 0;
	disk->first_minor = 0;
	disk->minors      = 0;
	disk->flags      |= GENHD_FL_EXT_DEVT;
	disk->fops        = &nvmeibc_tpv_fops;
	disk->private_data = tpv;
	queue->queuedata   = tpv;

	snprintf(disk->disk_name, DISK_NAME_LEN, "%s/%.30s",
		 NVMEIBC_TPV_DISK_PREFIX, tpv->tpv_name);

	/* chunk_sectors: 512-byte units; guarantees each READ/WRITE bio arriving
	 * in nvmeibc_tpv_make_request is contained within one TPV_extent.  */
#if KS_BLK_ALLOC_DISK_2PARAMS
	/* 6.8+: blk_queue_* setters removed; write limits directly. */
	queue->limits.logical_block_size  = 512;
	queue->limits.physical_block_size = 512;
	queue->limits.chunk_sectors =
		(unsigned int)((u64)tpv->allocator.tpv_extent_size_kb << 1);
#else
	blk_queue_logical_block_size(queue,  512);
	blk_queue_physical_block_size(queue, 512);
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

	set_capacity(disk, capacity);	/* OS may start sending IO now */

	tpv->disk  = disk;
	tpv->queue = queue;
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
	if (!tpv->disk)
		return;

	/*
	 * del_gendisk marks the device as going away and prevents new
	 * references.  On kernels >=5.15 it also drains in-flight IO.
	 */
	del_gendisk(tpv->disk);

#if KS_HAS_BLK_CLEANUP_DISK
	/* blk_cleanup_disk does put_disk + queue cleanup */
	blk_cleanup_disk(tpv->disk);
#else
	put_disk(tpv->disk);
#  if !KS_HAS_BLK_ALLOC_DISK
	blk_cleanup_queue(tpv->queue);
#  endif
#endif

	tpv->disk  = NULL;
	tpv->queue = NULL;
}

/* ── nvmeibc_tpv_attach ─────────────────────────────────────────────────
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
					u32 cdv_extent_size_mb,
					u64 allocator_size_gb)
{
	struct nvmeibc_tpv *tpv;
	int rv;

	if (WARN_ON(!cdv || !tpv_uuid || !tpv_name || !virtual_size_bytes ||
		    !tpv_extent_size_kb))
		return NULL;

	/* ── 0. Idempotency: return existing TPV if already attached ─────
	 *
	 * Management may re-send an attach command (e.g. after a keepalive
	 * gap or status-reporting race).  If the TPV is already in the
	 * active list, return it so the caller sends ACK_ATTACHED again
	 * without touching the disk or allocator.
	 *
	 * Also refresh cdv_vol in case the CDV was detached and re-attached
	 * (new nvmeibc_volume object) while the TPV was still live — without
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

	/* ── 1. Verify CDV is attached ──────────────────────────────────── */
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

	/* ── 2. Allocate struct nvmeibc_tpv ─────────────────────────────── */
	tpv = kzalloc(sizeof(*tpv), GFP_KERNEL);
	if (!tpv) {
		_NE(tpv_attach_kzalloc_fail, "TPV: kzalloc failed for @STR", tpv_name);
		return NULL;
	}

	tpv->cdv_vol     = cdv;
	tpv->virtual_size = virtual_size_bytes;
	strncpy(tpv->tpv_uuid, tpv_uuid, sizeof(tpv->tpv_uuid) - 1);
	strncpy(tpv->tpv_name, tpv_name, sizeof(tpv->tpv_name) - 1);
	atomic_set(&tpv->state, TPV_ATTACHING);

	/* Allocator identity is set to zero; updated via CDV topology push. */
	spin_lock_init(&tpv->allocator_id_lock);
	memset(tpv->allocator_toma_id, 0, sizeof(tpv->allocator_toma_id));
	tpv->allocator_generation = 0;

	spin_lock_init(&tpv->persist_lock);
	tpv->dirty = false;

	INIT_LIST_HEAD(&tpv->list_node);
	INIT_WORK(&tpv->cdv_alloc_work, nvmeibc_tpv_cdv_alloc_work_fn);
	INIT_WORK(&tpv->persist_work,   nvmeibc_tpv_persist_work_fn);
	atomic_set(&tpv->cdv_alloc_pending, 0);

	bio_list_init(&tpv->pending_bios);
	spin_lock_init(&tpv->pending_bio_lock);

	/* ── 3a. Initialise allocator ───────────────────────────────────── */
	nvmeibc_tpv_allocator_init(&tpv->allocator, tpv_extent_size_kb,
				   virtual_size_bytes, cdv_extent_size_mb,
				   allocator_size_gb);

	/* ── 3a-check. Verify flat-L1 tree can address all virtual extents. */
	{
		u64 l1_capacity = ((u64)cdv_extent_size_mb << 20) /
				  sizeof(struct tpv_tree_entry);

		if (tpv->allocator.virtual_extents_total > l1_capacity) {
			_NE(tpv_l1_capacity_exceeded,
			    "TPV: @STR: virtual_extents @LLU exceeds flat-L1 capacity @LLU",
			    tpv_name,
			    tpv->allocator.virtual_extents_total,
			    l1_capacity);
			goto err_free_alloc;
		}
	}

	/* ── 3b. Load allocator state from CDV_extent[0] ────────────────── */
	rv = nvmeibc_tpv_load_state(tpv);
	if (rv) {
		_NE(tpv_load_state_fail, "TPV: load_state failed for @STR rv=@INT",
		    tpv_name, rv);
		goto err_free_alloc;
	}

	/* ── 4. Recovery: cross-check TOMA for orphaned CDV_extents ─────
	 *
	 * Always run recovery regardless of what load_state found in the
	 * tree.  We do not know what happened while the TPV was offline:
	 * TOMA may have allocated CDV_extents that were never flushed to
	 * the tree (client crash between CDV_ALLOC_OK and persist_work).
	 * The only way to find these orphans is to ask TOMA.
	 */
	rv = nvmeibc_tpv_recovery(tpv);
	if (rv) {
		_NE(tpv_recovery_fail, "TPV: recovery failed for @STR rv=@INT",
		    tpv_name, rv);
		goto err_free_alloc;
	}

	/* ── 5. Schedule initial CDV_extent pre-allocation if pool empty ── */
	if (tpv->allocator.free_tpv_extent_count == 0) {
		_NI(tpv_pool_empty_at_attach, "TPV: @STR pool empty at attach; scheduling CDV alloc",
		    tpv_name);
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
	}

	/* ── 6. Register block device and open IO gates ─────────────────── */
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
	    "TPV: @STR (uuid=@STR) attached vsize=@LLU MB tpv_ext=@UINT KB cdv_ext=@UINT MB alloc=@LLU GB wmark=@LLU",
	    tpv_name, tpv_uuid,
	    virtual_size_bytes >> 20,
	    tpv_extent_size_kb,
	    cdv_extent_size_mb,
	    allocator_size_gb,
	    tpv->allocator.low_watermark);

	return tpv;

err_free_alloc:
	/* Unregister the block device if blkdev_register already succeeded. */
	nvmeibc_tpv_blkdev_unregister(tpv);
	nvmeibc_tpv_allocator_free(&tpv->allocator);
	kfree(tpv);
	return NULL;
}

/* ── nvmeibc_tpv_detach ─────────────────────────────────────────────────
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

	/* ── 1. Remove from active list and cancel pending work ────────── */
	cancel_work_sync(&tpv->cdv_alloc_work);
	cancel_work_sync(&tpv->persist_work);

	/* ── 2. Flush dirty allocator state synchronously ───────────────── */
	if (tpv->dirty) {
		int rv = nvmeibc_tpv_flush_state(tpv);

		if (rv)
			_NW(tpv_detach_flush_fail,
			    "TPV: flush_state failed for @STR rv=@INT; state may be lost",
			    tpv->tpv_name, rv);
	}

	/* ── 3. Unregister block device (quiesces IO via queue freeze) ──── */
	nvmeibc_tpv_blkdev_unregister(tpv);

	/*
	 * ── 3b. Fail any bios parked waiting for CDV_extent allocation ──
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
		spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

		while ((bio = bio_list_pop(&pending)) != NULL)
			bio_endio(bio, -EIO);
	}

	/* ── 4. Deregister proc entries and free allocator state ────────── */
	nvmeibc_tpv_proc_deregister(tpv);
	nvmeibc_tpv_allocator_free(&tpv->allocator);

	_NI(tpv_detached, "TPV: @STR (uuid=@STR) detached", tpv->tpv_name, tpv->tpv_uuid);

	/* ── 5. MCS notification is sent by the caller (the MCS handler) ── */
	/*
	 * The MCS detach-completion reply is sent by
	 * nvmeibc_main_capi_manipulate_vols.inc.c after we return, using
	 * the same completion path as regular volumes.
	 */

	kfree(tpv);
}

/* ── nvmeibc_tpv_grow ───────────────────────────────────────────────────
 *
 * Handle UpdateVolume MCS for a TPV (§3.10): management has extended the
 * TPV's virtual size.  Update in-kernel state and gendisk capacity.
 * No allocator flush needed — new extents start unmapped (reads = zero).
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

	if (tpv->disk)
		set_capacity(tpv->disk,
			     new_virtual_size_bytes >> KERNEL_SECTOR_SHIFT);

	_NI(tpv_grown, "TPV: @STR grown to @LLU MB (@LLU virtual extents)",
	    tpv->tpv_name, new_virtual_size_bytes >> 20, new_total_extents);
}

/* ── Allocator identity update (called on CDV topology push) ───────────── */

/*
 * nvmeibc_tpv_update_allocator_id — update the CDV.allocator TOMA identity.
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
 * nvmeibc_tpv_update_allocator_for_cdv — update allocator for all TPVs on a CDV.
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
		if (!tpv->cdv_vol)
			continue;
		if (strncmp(tpv->cdv_vol->hdr.uuid, cdv_uuid,
			    NVMEIBC_BD_UUID_LEN) != 0)
			continue;

		_NI(tpv_allocator_cdv_update,
		    "TPV @STR: CDV allocator update cdv=@STR toma=@STR gen=@LLU",
		    tpv->tpv_name, cdv_uuid, toma_id, generation);

		nvmeibc_tpv_update_allocator_id(tpv, toma_id, generation);

		/*
		 * Re-arm cdv_alloc_work in case it had previously deferred due
		 * to an empty allocator_toma_id.
		 */
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
		n_updated++;
	}
	spin_unlock_irqrestore(&nvmeibc_tpv_list_lock, flags);

	if (n_updated == 0)
		_ND(tpv_allocator_cdv_no_match,
		    "TPV: CDV allocator update cdv=@STR toma=@STR gen=@LLU — no matching TPVs",
		    cdv_uuid, toma_id, generation);
}
EXPORT_SYMBOL(nvmeibc_tpv_update_allocator_for_cdv);

