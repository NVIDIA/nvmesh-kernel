/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_simu.c - CDV/TOMA simulator for TPV unit tests.
 *
 * Provides in-memory implementations of all extern functions declared in
 * the TPV production modules:
 *
 *   nvmeibc_ib_admin_cdv_alloc_extent  (nvmeibc_tpv_allocator.c extern)
 *   nvmeibc_ib_admin_cdv_free_extent   (nvmeibc_tpv_allocator.c extern)
 *   nvmeibc_ib_admin_cdv_list_extents  (nvmeibc_tpv_recovery.c extern)
 *   nvmeibc_tpv_cdv_sync_read          (nvmeibc_tpv_persist.c extern)
 *   nvmeibc_tpv_cdv_sync_write         (nvmeibc_tpv_persist.c extern)
 *   nvmeibc_tpv_cdv_submit_bio         (nvmeibc_tpv_io.c extern - BUG stub)
 *
 * All state lives in the global g_tpv_cdv_sim, created / destroyed by the
 * test setup / teardown helpers defined at the bottom of this file.
 */

#include "common/kr_incs.h"
#include "clnt/nvmeibc_msgs_shared.h"
#include "clnt/nvmeibc_volume.h"
#include "clnt/block/nvmeibc_block_common.h"
#include "clnt/module/instance/nvmeibc_cinst_params.h"	/* nvmeibc_cinst_get_by_name */
#include "tpv/nvmeibc_tpv.h"
#include "tpv/nvmeibc_tpv_simu.h"

/* -- Global simulator instance -------------------------------------------- */

struct tpv_cdv_sim *g_tpv_cdv_sim;

/* -- nvmeibc_ib_admin_cdv_alloc_extent ------------------------------------ */

/*
 * Allocate the next available data CDV extent.  Extents are allocated in
 * ascending index order starting from 1 (index 0 is the L1 tree root).
 *
 * Fault injection:
 *   inject_full     -> return CDV_FULL (no new allocation performed)
 *   inject_wrong_gen -> return WRONG_GEN (no new allocation performed)
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

/* -- nvmeibc_ib_admin_cdv_free_extent ------------------------------------- */

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

/* -- nvmeibc_ib_admin_cdv_list_extents ------------------------------------ */

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

/* -- nvmeibc_tpv_cdv_sync_read -------------------------------------------- */

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

/* -- nvmeibc_tpv_cdv_sync_write ------------------------------------------- */

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

/* -- nvmeibc_tpv_cdv_sync_read_data / write_data -------------------------- */

/*
 * Online-compaction write-conflict injection target.  When non-NULL, the
 * data-CDV read stub flips this entry's state to RELOC_CANCELLED right after
 * the read completes (mirroring a guest write that arrives between
 * tpv_reloc_one_online's S.5 data write and S.6 cancellation check).  See
 * the header for the contract.
 */
struct nvmeibc_tpv_extent_entry *g_tpv_simu_cancel_on_data_read;

/*
 * Both _data variants route to the same RAM buffer as the metadata helpers;
 * online compaction has no semantic distinction between the two CDVs in this
 * single-CDV simulator.  Production splits them via tpv_cdv_sync_io_on +
 * nvmeibc_tpv_data_cdv() / nvmeibc_tpv_meta_cdv(); the simulator keeps one
 * cdv_ram region and lets reads/writes share it.
 */
int nvmeibc_tpv_cdv_sync_read_data(struct nvmeibc_tpv *tpv,
				    u64 cdv_offset, void *buf, u64 len)
{
	int rv = nvmeibc_tpv_cdv_sync_read(tpv, cdv_offset, buf, len);

	if (rv == 0 && g_tpv_simu_cancel_on_data_read) {
		/*
		 * Inject the guest-write race deterministically: flip the
		 * targeted entry's state to RELOC_CANCELLED so that S.6 of
		 * tpv_reloc_one_online observes the cancel and aborts before
		 * committing the L2 leaf write.  Only the first call after
		 * arming the hook fires; clear the global to avoid affecting
		 * subsequent reads in the same test run.
		 */
		WRITE_ONCE(g_tpv_simu_cancel_on_data_read->state,
			   NVMEIBC_TPV_ENTRY_RELOC_CANCELLED);
		g_tpv_simu_cancel_on_data_read = NULL;
	}
	return rv;
}

int nvmeibc_tpv_cdv_sync_write_data(struct nvmeibc_tpv *tpv,
				     u64 cdv_offset, const void *buf, u64 len)
{
	return nvmeibc_tpv_cdv_sync_write(tpv, cdv_offset, buf, len);
}

/* -- nvmeibc_tpv_cdv_submit_bio ------------------------------------------- */

/*
 * TPV unit tests exercise the allocator/persist/recovery path only.
 * The IO path (nvmeibc_tpv_make_request -> nvmeibc_tpv_cdv_submit_bio) is
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

/* -- Simulator lifecycle --------------------------------------------------- */

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

/* -- Mock CDV volume ------------------------------------------------------- */

struct nvmeibc_volume *tpv_cdv_vol_create(void)
{
	struct nvmeibc_volume       *cdv;
	struct nvmeibc_block_device *bdev;
	const struct nvmeibc_cinst_params *cinst;

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

	/*
	 * Wire cdv->p to the simulator's default client-instance params.  The
	 * production attach path's nvmeibc_tpv_blkdev_register dereferences
	 * cdv_vol->p via nvmeibc_isnt_params_main2blk() to reach the
	 * client-instance disk-id allocator.  Without this, p is NULL and
	 * nvmeibc_isnt_params_main2blk(NULL) computes a small bogus offset
	 * (e.g. 0xf0) that segfaults on first deref.  The simulator framework
	 * registers a default client named "nvmeibc" via clientSimulator_ismod
	 * before any test runs, so by the time tpv_test_setup gets here the
	 * lookup is non-NULL.
	 */
	cinst = nvmeibc_cinst_get_by_name("nvmeibc");
	BUG_ON(!cinst);	/* simulator framework should have registered the default */
	cdv->p = &cinst->main;

	return cdv;
}

void tpv_cdv_vol_destroy(struct nvmeibc_volume *cdv)
{
	if (!cdv)
		return;
	kfree(cdv->block_dev);
	kfree(cdv);
}

/* -- Test helper functions ------------------------------------------------- */

void tpv_simu_set_toma_id(struct nvmeibc_tpv *tpv,
			   const char *toma_id, u64 generation)
{
	nvmeibc_tpv_update_allocator_id(tpv, toma_id, generation);

	/*
	 * Mirror nvmeibc_tpv_update_allocator_for_cdv (the production CDV
	 * topology-push path): if state_loaded is still false, re-arm
	 * load_state_work so the next drain can flip it to true.  Otherwise
	 * re-arm cdv_alloc_work so subsequent allocations observe the new
	 * allocator identity.  Without this, cdv_alloc_work_fn bails on
	 * !state_loaded and the pool is never refilled.
	 *
	 * Use mod_delayed_work (cancel + requeue) rather than
	 * schedule_delayed_work: a previous load_state retry is likely still
	 * pending on its 100ms timer, and the simulator's
	 * __queue_delayed_work BUGs on a double-queue.  mod_delayed_work
	 * matches the kernel's pending-idempotent semantics.
	 */
	if (!READ_ONCE(tpv->state_loaded))
		mod_delayed_work(system_wq, &tpv->load_state_work, 0);
	else if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
		schedule_work(&tpv->cdv_alloc_work);
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

/* -- Exhaustion / capacity-return helpers --------------------------------- */

void tpv_simu_exhaust_cdv(void)
{
	struct tpv_cdv_sim *sim = g_tpv_cdv_sim;
	u64 i;

	BUG_ON(!sim);

	for (i = 1; i <= sim->max_extents; i++) {
		sim->extents[i].allocated = true;
		strncpy(sim->extents[i].owner_uuid,
			"cdv-full-test-dummy-tenant-uuid0",
			sizeof(sim->extents[i].owner_uuid) - 1);
	}
}

int tpv_simu_release_one_cdv_extent(u64 extent_index)
{
	struct tpv_cdv_sim *sim = g_tpv_cdv_sim;

	BUG_ON(!sim);

	if (extent_index == 0 || extent_index > sim->max_extents)
		return -EINVAL;
	if (!sim->extents[extent_index].allocated)
		return -ENOENT;

	sim->extents[extent_index].allocated = false;
	memset(sim->extents[extent_index].owner_uuid, 0,
	       sizeof(sim->extents[extent_index].owner_uuid));
	return 0;
}

/* -- Linker stubs for async IB-response paths -------------------------------
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

/*
 * Test hook function pointers — normally defined in nvmeibc_tpv_cdv.c and
 * nvmeibc_tpv_ib_admin.c, which are not compiled in the simulator build.
 * nvmeibc_tpv_test.c sets these to its in-memory stubs before each selftest
 * run and restores them to NULL afterward.
 */
int (*nvmeibc_tpv_cdv_test_sync_read_fn)(struct nvmeibc_tpv *tpv,
					  u64 cdv_offset, void *buf, u64 len);
int (*nvmeibc_tpv_cdv_test_sync_write_fn)(struct nvmeibc_tpv *tpv,
					   u64 cdv_offset, const void *buf,
					   u64 len);

int (*nvmeibc_tpv_test_cdv_alloc_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_alloc_req *req,
	struct nvmeibc_cdv_alloc_resp *resp);

int (*nvmeibc_tpv_test_cdv_free_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_free_req *req);

int (*nvmeibc_tpv_test_cdv_list_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const char *tpv_uuid, u64 **out_indices, u64 *out_count);
