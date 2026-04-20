/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_simu.c — CDV/TOMA simulator for TPV unit tests.
 *
 * Provides in-memory implementations of all extern functions declared in
 * the TPV production modules:
 *
 *   nvmeibc_ib_admin_cdv_alloc_extent  (nvmeibc_tpv_allocator.c extern)
 *   nvmeibc_ib_admin_cdv_free_extent   (nvmeibc_tpv_allocator.c extern)
 *   nvmeibc_ib_admin_cdv_list_extents  (nvmeibc_tpv_recovery.c extern)
 *   nvmeibc_tpv_cdv_sync_read          (nvmeibc_tpv_persist.c extern)
 *   nvmeibc_tpv_cdv_sync_write         (nvmeibc_tpv_persist.c extern)
 *   nvmeibc_tpv_cdv_submit_bio         (nvmeibc_tpv_io.c extern — BUG stub)
 *
 * All state lives in the global g_tpv_cdv_sim, created / destroyed by the
 * test setup / teardown helpers defined at the bottom of this file.
 */

#include "common/kr_incs.h"
#include "clnt/nvmeibc_msgs_shared.h"
#include "clnt/nvmeibc_volume.h"
#include "clnt/block/nvmeibc_block_common.h"
#include "tpv/nvmeibc_tpv.h"
#include "tpv/nvmeibc_tpv_simu.h"

/* ── Global simulator instance ──────────────────────────────────────────── */

struct tpv_cdv_sim *g_tpv_cdv_sim;

/* ── nvmeibc_ib_admin_cdv_alloc_extent ──────────────────────────────────── */

/*
 * Allocate the next available data CDV extent.  Extents are allocated in
 * ascending index order starting from 1 (index 0 is the L1 tree root).
 *
 * Fault injection:
 *   inject_full     → return CDV_FULL (no new allocation performed)
 *   inject_wrong_gen → return WRONG_GEN (no new allocation performed)
 */
int nvmeibc_ib_admin_cdv_alloc_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_alloc_req   *req,
	struct nvmeibc_cdv_alloc_resp        *resp)
{
	struct tpv_cdv_sim *sim = g_tpv_cdv_sim;
	u64 i;
	(void)cdv;
	(void)toma_id;

	BUG_ON(!sim);

	memset(resp, 0, sizeof(*resp));
	resp->req_id = req->req_id;

	if (sim->inject_wrong_gen) {
		sim->inject_wrong_gen = false;
		resp->status             = NVMEIBC_CDV_ALLOC_WRONG_GEN;
		resp->allocator_generation = sim->inject_wrong_gen_val;
		return 0;
	}

	if (sim->inject_full) {
		sim->inject_full = false;
		resp->status = NVMEIBC_CDV_ALLOC_CDV_FULL;
		return 0;
	}

	/* Find the first unallocated data extent (index 1..max_extents). */
	for (i = 1; i <= sim->max_extents; i++) {
		if (!sim->extents[i].allocated) {
			sim->extents[i].allocated = true;
			strncpy(sim->extents[i].owner_uuid, req->tpv_uuid,
				sizeof(sim->extents[i].owner_uuid) - 1);
			resp->status       = NVMEIBC_CDV_ALLOC_OK;
			resp->extent_index = i;
			resp->allocator_generation = req->client_generation;
			return 0;
		}
	}

	/* No free extents: CDV is full. */
	resp->status = NVMEIBC_CDV_ALLOC_CDV_FULL;
	return 0;
}

/* ── nvmeibc_ib_admin_cdv_free_extent ───────────────────────────────────── */

int nvmeibc_ib_admin_cdv_free_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_free_req    *req)
{
	struct tpv_cdv_sim *sim = g_tpv_cdv_sim;
	u64 i = req->extent_index;
	(void)cdv;
	(void)toma_id;

	BUG_ON(!sim);

	if (i == 0 || i > sim->max_extents) {
		pr_err("TPV_SIM: free_extent: invalid index %llu\n", i);
		return -EINVAL;
	}
	if (!sim->extents[i].allocated) {
		pr_err("TPV_SIM: free_extent: extent %llu was not allocated\n", i);
		return -ENOENT;
	}

	sim->extents[i].allocated = false;
	memset(sim->extents[i].owner_uuid, 0,
	       sizeof(sim->extents[i].owner_uuid));
	return 0;
}

/* ── nvmeibc_ib_admin_cdv_list_extents ──────────────────────────────────── */

/*
 * Return a vmalloc'd array of extent indices owned by @tpv_uuid.
 * The caller must vfree(*out_indices).
 */
int nvmeibc_ib_admin_cdv_list_extents(
	struct nvmeibc_volume *cdv,
	const char *toma_id,
	const char *tpv_uuid,
	u64 **out_indices,
	u64 *out_count)
{
	struct tpv_cdv_sim *sim = g_tpv_cdv_sim;
	u64 *arr;
	u64 count = 0;
	u64 i;
	(void)cdv;
	(void)toma_id;

	BUG_ON(!sim);

	*out_indices = NULL;
	*out_count   = 0;

	/* Count matching extents first. */
	for (i = 1; i <= sim->max_extents; i++) {
		if (sim->extents[i].allocated &&
		    strncmp(sim->extents[i].owner_uuid, tpv_uuid,
			    sizeof(sim->extents[i].owner_uuid)) == 0)
			count++;
	}

	if (count == 0)
		return 0;

	arr = vmalloc(count * sizeof(u64));
	if (!arr)
		return -ENOMEM;

	count = 0;
	for (i = 1; i <= sim->max_extents; i++) {
		if (sim->extents[i].allocated &&
		    strncmp(sim->extents[i].owner_uuid, tpv_uuid,
			    sizeof(sim->extents[i].owner_uuid)) == 0)
			arr[count++] = i;
	}

	*out_indices = arr;
	*out_count   = count;
	return 0;
}

/* ── nvmeibc_tpv_cdv_sync_read ──────────────────────────────────────────── */

int nvmeibc_tpv_cdv_sync_read(struct nvmeibc_tpv *tpv,
			       u64 cdv_offset, void *buf, u64 len)
{
	struct tpv_cdv_sim *sim = g_tpv_cdv_sim;
	(void)tpv;

	BUG_ON(!sim);

	if (cdv_offset + len > sim->cdv_bytes) {
		pr_err("TPV_SIM: sync_read out of range offset=%llu len=%llu cap=%llu\n",
		       cdv_offset, len, sim->cdv_bytes);
		return -EINVAL;
	}
	memcpy(buf, sim->cdv_ram + cdv_offset, len);
	return 0;
}

/* ── nvmeibc_tpv_cdv_sync_write ─────────────────────────────────────────── */

int nvmeibc_tpv_cdv_sync_write(struct nvmeibc_tpv *tpv,
				u64 cdv_offset, const void *buf, u64 len)
{
	struct tpv_cdv_sim *sim = g_tpv_cdv_sim;
	(void)tpv;

	BUG_ON(!sim);

	if (cdv_offset + len > sim->cdv_bytes) {
		pr_err("TPV_SIM: sync_write out of range offset=%llu len=%llu cap=%llu\n",
		       cdv_offset, len, sim->cdv_bytes);
		return -EINVAL;
	}
	memcpy(sim->cdv_ram + cdv_offset, buf, len);
	return 0;
}

/* ── nvmeibc_tpv_cdv_submit_bio ─────────────────────────────────────────── */

/*
 * TPV unit tests exercise the allocator/persist/recovery path only.
 * The IO path (nvmeibc_tpv_make_request → nvmeibc_tpv_cdv_submit_bio) is
 * not exercised; any accidental call is a test bug.
 */
void nvmeibc_tpv_cdv_submit_bio(struct nvmeibc_tpv *tpv,
				 struct bio *bio,
				 u64 cdv_phys_offset)
{
	(void)tpv;
	(void)bio;
	(void)cdv_phys_offset;
	BUG_ON(1);	/* should never be called in unit tests */
}

/* ── Simulator lifecycle ─────────────────────────────────────────────────── */

struct tpv_cdv_sim *tpv_cdv_sim_create(void)
{
	struct tpv_cdv_sim *sim;

	sim = kzalloc(sizeof(*sim), GFP_KERNEL);
	if (!sim)
		return NULL;

	sim->cdv_bytes   = TPV_SIMU_CDV_BYTES;
	sim->max_extents = TPV_SIMU_N_DATA_EXT;

	sim->cdv_ram = kzalloc(sim->cdv_bytes, GFP_KERNEL);
	if (!sim->cdv_ram) {
		kfree(sim);
		return NULL;
	}

	g_tpv_cdv_sim = sim;
	return sim;
}

void tpv_cdv_sim_destroy(void)
{
	if (!g_tpv_cdv_sim)
		return;
	kfree(g_tpv_cdv_sim->cdv_ram);
	kfree(g_tpv_cdv_sim);
	g_tpv_cdv_sim = NULL;
}

/* ── Mock CDV volume ─────────────────────────────────────────────────────── */

struct nvmeibc_volume *tpv_cdv_vol_create(void)
{
	struct nvmeibc_volume       *cdv;
	struct nvmeibc_block_device *bdev;

	cdv = kzalloc(sizeof(*cdv), GFP_KERNEL);
	if (!cdv)
		return NULL;

	bdev = kzalloc(sizeof(*bdev), GFP_KERNEL);
	if (!bdev) {
		kfree(cdv);
		return NULL;
	}

	/*
	 * Set the block device size in 4 KiB sectors.
	 * nvmeibc_volume_get_size(cdv) returns bdev->size (in 4 KB sectors).
	 */
	bdev->size = (ulong)TPV_SIMU_CDV_SECTORS;
	strncpy(bdev->uuid, TPV_SIMU_CDV_UUID, sizeof(bdev->uuid) - 1);

	cdv->block_dev = bdev;
	cdv->status    = NVS_ATTACHED;
	strncpy(cdv->hdr.uuid, TPV_SIMU_CDV_UUID, sizeof(cdv->hdr.uuid) - 1);

	/* Initialise the spinlock embedded in nvmeibc_volume_header. */
	spin_lock_init(&cdv->hdr.ext_blob_modify_guard);
	spin_lock_init(&cdv->spinlock);

	return cdv;
}

void tpv_cdv_vol_destroy(struct nvmeibc_volume *cdv)
{
	if (!cdv)
		return;
	kfree(cdv->block_dev);
	kfree(cdv);
}

/* ── Test helper functions ───────────────────────────────────────────────── */

void tpv_simu_set_toma_id(struct nvmeibc_tpv *tpv,
			   const char *toma_id, u64 generation)
{
	nvmeibc_tpv_update_allocator_id(tpv, toma_id, generation);
}

void tpv_simu_fill_pool(struct nvmeibc_tpv *tpv)
{
	u64 prev_full = atomic64_read(&tpv->allocator.stat_cdv_alloc_full);
	int max_iter = TPV_SIMU_N_DATA_EXT + 2;	/* +2 for CDV_FULL detection */

	while (max_iter-- > 0) {
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
		flush_workqueue(system_wq);

		/* Stop once CDV_FULL is returned at least once. */
		if (atomic64_read(&tpv->allocator.stat_cdv_alloc_full) > (long long)prev_full)
			break;
	}
	/* Also drain any pending persist_work. */
	flush_workqueue(system_wq);
}

/* ── Linker stubs for async IB-response paths ───────────────────────────────
 *
 * In the simulator, CDV alloc/list operations are synchronous (implemented
 * above).  The async response-dispatch functions called from nvmeibc_topology.c
 * are never reached; stub them out to satisfy the linker.
 */

void nvmeibc_cdv_dispatch_alloc_response(const struct nvmeibc_cdv_alloc_resp *rsp)
{
	(void)rsp;
	BUG_ON(1); /* should never be called in the simulator */
}

void nvmeibc_cdv_dispatch_list_response(const struct nvmeibc_cdv_list_resp *rsp,
					 const u64 *indices, u64 n_idx)
{
	(void)rsp;
	(void)indices;
	(void)n_idx;
	BUG_ON(1); /* should never be called in the simulator */
}

ssize_t nvmeibc_tpv_run_selftests(void *arg, char *buf, size_t len)
{
	(void)arg;
	(void)buf;
	(void)len;
	return -ENOSYS; /* kernel self-tests (nvmeibc_tpv_test.c) not compiled in simulator */
}
