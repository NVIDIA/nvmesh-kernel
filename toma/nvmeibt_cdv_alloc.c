/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 * nvmeibt_cdv_alloc.c — CDV extent allocator: in-memory state + on-CDV persistence.
 *
 * See nvmeibt_cdv_alloc.h for a full description.
 * Extent allocation metadata is persisted as 4 KiB records in the allocator
 * region at the beginning of the CDV volume.  Each alloc/free writes a single
 * record synchronously via O_DIRECT|O_SYNC pwrite to the underlying disk
 * segment.  On allocator election or on the first ALLOC after restart, the
 * region is scanned to rebuild in-memory state.
 */

#include <string.h>
#include <errno.h>
#include "nvmeibt_cdv_alloc.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_register.h"		/* struct nvmeibt_register_msg, nvmeibt_register_send_msg_to_registrant */
#include "nvmeibt_toma.h"		/* nvmeibt_toma_send_msg_to_client, nvmeibt_global_get_global */
#include "nvmeibt_global.h"		/* struct nvmeibt_topology (full definition) */
#include "nvmeibt_kafka.h"		/* nvmeibt_kafka_outgoing_msgs_queue_add, KAFKA_PRODUCER_MSG_HEADER_* */
#include "nvmeibt_local_disk.h"		/* struct nvmeibt_local_disk */
#include "nvmeibt_seg_active.h"		/* struct nvmeibt_seg_active, registrant iteration */
#include "../common/nvmeib_hash.h"
#include "vol/nvmeibt_block_device.h"	/* nvmeibt_block_device_get_block_device_by_id, nvmeibt_blkdev_is_being_deleted */
#include "utils/nvmeibt_uuid.h"		/* nvmeibt_urn_uuid_str_to_union_uuid */

/* ── Global state ────────────────────────────────────────────────────────── */

/* cdv_uuid (ASCII string) → nvmeibt_cdv_alloc * */
static struct nvmeib_hash_table *cdv_alloc_hash;

/* ── On-CDV disk I/O ────────────────────────────────────────────────────── */

/*
 * cdv_resolve_vol_io — resolve a CDV UUID to a file descriptor for I/O.
 *
 * Opens /dev/nvmesh/<name> (the NVMesh block device) so that all I/O goes
 * through the NVMesh RAID/RDMA transport.  This ensures CDV allocator
 * metadata is replicated for availability and accessible from any TOMA
 * that can serve as the CDV allocator.
 *
 * If the alloc struct already has a cached fd, returns it.  Otherwise opens
 * the device and caches the fd.
 *
 * Returns 0 on success, negative errno on failure.
 */
#define CDV_DEV_PATH_PREFIX	"/dev/nvmesh/"

static int cdv_resolve_vol_io(const char *cdv_uuid,
			      struct nvmeibt_cdv_alloc *alloc,
			      int *out_fd)
{
	union nvmeib_uuid bdev_uuid;
	struct nvmeibt_block_device *bdev;
	char path[sizeof(CDV_DEV_PATH_PREFIX) + 32];

	/* Return cached fd if already open. */
	if (alloc && alloc->cdv_fd > 0) {
		*out_fd = alloc->cdv_fd;
		return 0;
	}

	if (nvmeibt_urn_uuid_str_to_union_uuid(&bdev_uuid, cdv_uuid) < 0)
		return -EINVAL;

	bdev = nvmeibt_block_device_get_block_device_by_id(&bdev_uuid);
	if (!bdev || !bdev->from_config.is_cdv)
		return -ENOENT;

	snprintf(path, sizeof(path), "%s%s",
		 CDV_DEV_PATH_PREFIX, bdev->from_config.client_blkdev_name);

	{
		int fd = NNVMEIBT_OPEN_LOCAL_DISK_WRITE(cdv_vol_open, path);

		if (fd < 0)
			return -ENODEV;

		if (alloc)
			alloc->cdv_fd = fd;

		*out_fd = fd;
	}
	return 0;
}

/*
 * cdv_ondisk_record_offset — byte offset within the CDV for extent_index's record.
 *
 * Layout at the start of the CDV:
 *   Offset 0:                      Header (4 KiB)
 *   Offset CDV_ONDISK_BLOCK_SIZE:  Record for extent_index 0
 *   Offset 2 * CDV_ONDISK_BLOCK_SIZE: Record for extent_index 1
 *   ...
 */
static inline uint64_t cdv_ondisk_record_offset(uint64_t extent_index)
{
	return (uint64_t)CDV_ONDISK_BLOCK_SIZE * (1 + extent_index);
}

/*
 * cdv_ondisk_write_record — write a single extent record to the CDV.
 *
 * Allocates a page-aligned 4 KiB buffer, fills in the record, and
 * issues a synchronous pwrite.  If tpv_uuid is NULL, the record is
 * written as free (zeroed flags).
 */
static int cdv_ondisk_write_record(int fd,
				   uint64_t extent_index,
				   const char *tpv_uuid)
{
	struct cdv_alloc_ondisk_record *rec;
	uint64_t off;
	ssize_t  rv;

	rec = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_ondisk_wr_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!rec)
		return -ENOMEM;

	if (tpv_uuid) {
		rec->flags = CDV_ONDISK_RECORD_FLAG_ALLOCATED;
		strncpy(rec->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
		rec->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	}

	rec->crc32 = crc32_seedless(rec, offsetof(struct cdv_alloc_ondisk_record, crc32));

	off = cdv_ondisk_record_offset(extent_index);
	rv = NNVMEIBT_PWRITE(cdv_ondisk_wr, fd, rec, CDV_ONDISK_BLOCK_SIZE,
			     off, 0ULL);

	NNVMEIBT_BM_FREE(cdv_ondisk_wr_free, rec);
	return (rv < 0) ? -EIO : 0;
}

/*
 * cdv_ondisk_write_header — write the allocator header to CDV byte 0.
 */
static int cdv_ondisk_write_header(int fd,
				   const struct nvmeibt_cdv_alloc *alloc)
{
	struct cdv_alloc_ondisk_header *hdr;
	ssize_t rv;

	hdr = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_ondisk_hdr_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!hdr)
		return -ENOMEM;

	hdr->magic   = CDV_ONDISK_MAGIC;
	hdr->version = CDV_ONDISK_VERSION;
	hdr->total_data_extents   = alloc->total_data_extents;
	hdr->allocator_generation = alloc->allocator_generation;
	strncpy(hdr->allocator_toma_id, alloc->allocator_toma_id,
		NVMEIBT_CDV_UUID_STRLEN - 1);
	hdr->crc32 = crc32_seedless(hdr, offsetof(struct cdv_alloc_ondisk_header, crc32));

	rv = NNVMEIBT_PWRITE(cdv_ondisk_hdr_wr, fd, hdr, CDV_ONDISK_BLOCK_SIZE,
			     0ULL, 0ULL);

	NNVMEIBT_BM_FREE(cdv_ondisk_hdr_free, hdr);
	return (rv < 0) ? -EIO : 0;
}

/*
 * cdv_ondisk_scan — scan CDV allocator region, rebuild in-memory extent list.
 *
 * Reads the header, validates magic/CRC, then reads extent records one by one.
 * Called lazily on the first ALLOC for a CDV after TOMA restart.
 * Returns 0 on success, negative on I/O error.  A missing/invalid header
 * (fresh CDV) is treated as empty — not an error.
 */
static int cdv_ondisk_scan(const char *cdv_uuid,
			   struct nvmeibt_cdv_alloc *alloc)
{
	struct cdv_alloc_ondisk_header *hdr = NULL;
	struct cdv_alloc_ondisk_record *rec = NULL;
	int      fd;
	uint64_t i, total, n_loaded = 0;
	int      rv;

	rv = cdv_resolve_vol_io(cdv_uuid, alloc, &fd);
	if (rv) {
		/*
		 * CDV volume not available yet (e.g. /dev/nvmesh/<name> not
		 * created, or CDV not yet attached to this node).  Do NOT mark
		 * ondisk_loaded — allow the caller to retry.
		 */
		N_Wf(cdv_scan_no_disk,
		     "CDV-alloc: scan cdv=@STR cannot open CDV volume rv=@INT; will retry",
		     cdv_uuid, rv);
		return 0;
	}

	hdr = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_scan_hdr_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	rec = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_scan_rec_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!hdr || !rec) {
		rv = -ENOMEM;
		goto out;
	}

	/* Read header at CDV byte 0 */
	if (NNVMEIBT_PREAD(cdv_scan_hdr_rd, fd, hdr, CDV_ONDISK_BLOCK_SIZE,
			   0ULL, 1) < 0) {
		N_Wf(cdv_scan_hdr_err,
		     "CDV-alloc: scan cdv=@STR header read failed; treating as fresh",
		     cdv_uuid);
		alloc->ondisk_loaded = true;
		rv = 0;
		goto out;
	}

	if (hdr->magic != CDV_ONDISK_MAGIC) {
		N_If(cdv_scan_fresh,
		     "CDV-alloc: scan cdv=@STR no valid header (magic=@X); fresh CDV",
		     cdv_uuid, hdr->magic);
		alloc->ondisk_loaded = true;
		rv = 0;
		goto out;
	}

	{
		uint32_t expected_crc = crc32_seedless(hdr,
			offsetof(struct cdv_alloc_ondisk_header, crc32));
		if (hdr->crc32 != expected_crc) {
			N_Wf(cdv_scan_hdr_crc,
			     "CDV-alloc: scan cdv=@STR header CRC mismatch; treating as fresh",
			     cdv_uuid);
			alloc->ondisk_loaded = true;
			rv = 0;
			goto out;
		}
	}

	total = hdr->total_data_extents;
	if (alloc->total_data_extents == 0 && total > 0)
		alloc->total_data_extents = total;

	if (alloc->allocator_generation == 0 && hdr->allocator_generation > 0)
		alloc->allocator_generation = hdr->allocator_generation;

	N_If(cdv_scan_start,
	     "CDV-alloc: scanning cdv=@STR total_data_extents=@LLU", cdv_uuid, total);

	for (i = 1; i < total; i++) {
		uint64_t off = cdv_ondisk_record_offset(i);
		uint32_t expected_crc;

		if (NNVMEIBT_PREAD(cdv_scan_rec_rd, fd, rec, CDV_ONDISK_BLOCK_SIZE,
				   off, 1) < 0) {
			N_Wf(cdv_scan_rec_err,
			     "CDV-alloc: scan cdv=@STR read failed at idx=@LLU; stopping",
			     cdv_uuid, i);
			break;
		}

		if (!(rec->flags & CDV_ONDISK_RECORD_FLAG_ALLOCATED))
			continue;

		expected_crc = crc32_seedless(rec,
			offsetof(struct cdv_alloc_ondisk_record, crc32));
		if (rec->crc32 != expected_crc) {
			N_Wf(cdv_scan_rec_crc,
			     "CDV-alloc: scan cdv=@STR idx=@LLU CRC bad; skipping",
			     cdv_uuid, i);
			continue;
		}

		rec->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';

		if (nvmeibt_cdv_alloc_add_extent(cdv_uuid, i, rec->tpv_uuid) < 0)
			N_Ef(cdv_scan_add_fail,
			     "CDV-alloc: scan cdv=@STR idx=@LLU add_extent failed",
			     cdv_uuid, i);
		else
			n_loaded++;

		memset(rec, 0, CDV_ONDISK_BLOCK_SIZE);
	}

	alloc->ondisk_loaded = true;
	N_If(cdv_scan_done,
	     "CDV-alloc: scan cdv=@STR loaded @LLU extents from @LLU slots",
	     cdv_uuid, n_loaded, total);
	rv = 0;

out:
	if (hdr) NNVMEIBT_BM_FREE(cdv_scan_hdr_free, hdr);
	if (rec) NNVMEIBT_BM_FREE(cdv_scan_rec_free, rec);
	return rv;
}

/* ── Internal helpers ───────────────────────────────────────────────────── */

/*
 * cdv_alloc_insert — find (or create) the per-CDV allocator and append a new
 * extent entry to it.  Returns 0 on success, negative errno on OOM.
 */
static int cdv_alloc_insert(const char *cdv_uuid,
			    uint64_t    extent_index,
			    const char *tpv_uuid)
{
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		alloc = NNVMEIBT_BM_CALLOC(cdv_alloc_insert_alloc, sizeof(*alloc));
		if (!alloc) {
			N_Ef(cdv_alloc_oom_alloc,
			     "CDV-alloc: calloc failed for cdv=@STR", cdv_uuid);
			return -ENOMEM;
		}
		strncpy(alloc->cdv_uuid, cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
		alloc->cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
		XDLIST_HEAD_INIT(&alloc->extents);
		alloc->n_allocated = 0;
		nvmeib_hash_add_ascii_str(cdv_alloc_hash, alloc->cdv_uuid, alloc);
		N_Tf(cdv_alloc_new_cdv, "CDV-alloc: new per-CDV allocator cdv=@STR",
		     cdv_uuid);
	}

	/* Guard against duplicate extent_index (replayed RAFT entry). */
	{
		struct nvmeibt_cdv_extent_entry *dup;
		XDLIST_FOREACH(dup, &alloc->extents) {
			if (dup->extent_index == extent_index) {
				N_Wf(cdv_alloc_dup,
				     "CDV-alloc: duplicate idx=@LLU cdv=@STR; updating tpv from @STR to @STR",
				     extent_index, cdv_uuid, dup->tpv_uuid, tpv_uuid);
				strncpy(dup->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
				dup->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
				return 0;
			}
		}
	}

	entry = NNVMEIBT_BM_CALLOC(cdv_alloc_insert_entry, sizeof(*entry));
	if (!entry) {
		N_Ef(cdv_alloc_oom_entry,
		     "CDV-alloc: calloc failed for entry cdv=@STR idx=@LLU",
		     cdv_uuid, extent_index);
		return -ENOMEM;
	}

	entry->extent_index = extent_index;
	strncpy(entry->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	entry->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	XDLIST_INIT_LINK(&entry->link, NULL);
	XDLIST_ADD_TAIL(&alloc->extents, entry);
	alloc->n_allocated++;

	return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int nvmeibt_cdv_alloc_add_extent(const char *cdv_uuid,
				 uint64_t    extent_index,
				 const char *tpv_uuid)
{
	int rv;

	rv = cdv_alloc_insert(cdv_uuid, extent_index, tpv_uuid);
	if (rv)
		return rv;

	N_If(cdv_alloc_add,
	     "CDV-alloc: allocated cdv=@STR idx=@LLU tpv=@STR",
	     cdv_uuid, extent_index, tpv_uuid);

	return 0;
}

int nvmeibt_cdv_alloc_remove_extent(const char *cdv_uuid, uint64_t extent_index)
{
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		N_Wf(cdv_alloc_rm_no_cdv,
		     "CDV-alloc: remove on unknown cdv=@STR idx=@LLU",
		     cdv_uuid, extent_index);
		return -ENOENT;
	}

	XDLIST_FOREACH_SAFE(entry, &alloc->extents) {
		if (entry->extent_index != extent_index)
			continue;

		XDLIST_ELEM_DEL(&alloc->extents, entry);
		alloc->n_allocated--;

		N_If(cdv_alloc_rm,
		     "CDV-alloc: freed cdv=@STR idx=@LLU tpv=@STR remaining=@LLU",
		     cdv_uuid, extent_index, entry->tpv_uuid, alloc->n_allocated);

		NNVMEIBT_BM_FREE(cdv_alloc_rm_free, entry);
		return 0;
	}

	N_Wf(cdv_alloc_rm_notfound,
	     "CDV-alloc: extent not found cdv=@STR idx=@LLU",
	     cdv_uuid, extent_index);
	return -ENOENT;
}

static struct nvmeibt_cdv_alloc *find_or_create_alloc(const char *cdv_uuid);
static int cdv_ondisk_scan(const char *cdv_uuid, struct nvmeibt_cdv_alloc *alloc);

int nvmeibt_cdv_alloc_list_for_tpv(const char  *cdv_uuid,
				    const char  *tpv_uuid,
				    uint64_t   **out_indices,
				    uint64_t    *out_count)
{
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;
	uint64_t  n = 0;
	uint64_t *indices;

	*out_indices = NULL;
	*out_count   = 0;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		/*
		 * No in-memory allocator.  This happens when the CDV was
		 * detached and re-attached (e.g. last TPV detached, then a
		 * TPV re-attached).  Rebuild state from on-disk extent
		 * records — the same cold-recovery path used at election.
		 */
		alloc = find_or_create_alloc(cdv_uuid);
		if (!alloc)
			return -ENOMEM;
	}

	if (!alloc->ondisk_loaded) {
		cdv_ondisk_scan(cdv_uuid, alloc);
		if (!alloc->ondisk_loaded) {
			/*
			 * Disk I/O not available yet (CDV segments not ready).
			 * If the allocator has known in-memory extents, we must
			 * retry (we might be missing some from disk).  If it has
			 * none, return 0 extents — either this is a fresh CDV
			 * or a restart scenario where recovery will adopt
			 * orphans once TOMA disk I/O is available.
			 */
			if (alloc->n_allocated > 0) {
				N_Wf(cdv_list_not_ready,
				     "CDV-alloc: list cdv=@STR tpv=@STR ondisk scan not ready (n_alloc=@LLU); returning EAGAIN",
				     cdv_uuid, tpv_uuid, alloc->n_allocated);
				return -EAGAIN;
			}
			N_Wf(cdv_list_scan_deferred,
			     "CDV-alloc: list cdv=@STR tpv=@STR disk not ready, 0 in-memory extents; returning empty",
			     cdv_uuid, tpv_uuid);
			/* Fall through — return 0 extents. */
		}
	}

	/* Count matches first to size the output array. */
	XDLIST_FOREACH(entry, &alloc->extents) {
		if (strncmp(entry->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN) == 0)
			n++;
	}

	if (n == 0)
		return 0;

	indices = NNVMEIBT_BM_ALLOC(cdv_alloc_list_alloc, n * sizeof(*indices));
	if (!indices) {
		N_Ef(cdv_alloc_list_oom,
		     "CDV-alloc: list alloc failed cdv=@STR tpv=@STR n=@LLU",
		     cdv_uuid, tpv_uuid, n);
		return -ENOMEM;
	}

	n = 0;
	XDLIST_FOREACH(entry, &alloc->extents) {
		if (strncmp(entry->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN) == 0)
			indices[n++] = entry->extent_index;
	}

	*out_indices = indices;
	*out_count   = n;
	return 0;
}

void nvmeibt_cdv_alloc_set_generation(const char *cdv_uuid, uint64_t generation)
{
	struct nvmeibt_cdv_alloc *alloc;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		alloc = NNVMEIBT_BM_CALLOC(cdv_alloc_set_gen_alloc, sizeof(*alloc));
		if (!alloc) {
			N_Ef(cdv_alloc_set_gen_oom,
			     "CDV-alloc: set_generation calloc failed cdv=@STR",
			     cdv_uuid);
			return;
		}
		strncpy(alloc->cdv_uuid, cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
		alloc->cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
		XDLIST_HEAD_INIT(&alloc->extents);
		nvmeib_hash_add_ascii_str(cdv_alloc_hash, alloc->cdv_uuid, alloc);
	}

	N_If(cdv_alloc_set_gen,
	     "CDV-alloc: set generation cdv=@STR old=@LLU new=@LLU",
	     cdv_uuid, alloc->allocator_generation, generation);

	alloc->allocator_generation = generation;
}

/* ── CDV removal ────────────────────────────────────────────────────────── */

void nvmeibt_cdv_alloc_remove(const char *cdv_uuid)
{
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;

	if (!cdv_alloc_hash)
		return;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		N_If(cdv_alloc_remove_notfound,
		     "CDV-alloc: remove called for unknown cdv=@STR (already absent)", cdv_uuid);
		return;
	}

	N_If(cdv_alloc_remove,
	     "CDV-alloc: removing cdv=@STR allocated=@LLU", cdv_uuid, alloc->n_allocated);

	/* Free all extent entries before removing the allocator itself. */
	while (!XDLIST_EMPTY(&alloc->extents)) {
		entry = XDLIST_FIRST(&alloc->extents);
		XDLIST_ELEM_DEL(&alloc->extents, entry);
		NNVMEIBT_BM_FREE(cdv_alloc_remove_entry, entry);
	}

	/* Close the cached CDV volume fd if open. */
	if (alloc->cdv_fd > 0)
		NNVMEIBT_CLOSE(cdv_alloc_remove_close, alloc->cdv_fd);

	nvmeib_hash_delete_ascii_str(cdv_alloc_hash, cdv_uuid);
	NNVMEIBT_BM_FREE(cdv_alloc_remove_alloc, alloc);
}

/* ── Stale-entry garbage collection ─────────────────────────────────────── */

/*
 * Maximum number of stale CDV allocator entries that can be cleaned up in a
 * single GC pass.  Enough for any realistic deployment; entries beyond this
 * limit will be caught on the next pass.
 */
#define CDV_ALLOC_GC_MAX_STALE 64

void nvmeibt_cdv_alloc_gc_stale_entries(void)
{
	struct nvmeibt_cdv_alloc *alloc;
	char stale[CDV_ALLOC_GC_MAX_STALE][NVMEIBT_CDV_UUID_STRLEN];
	int n_stale = 0;
	int i;

	if (!cdv_alloc_hash)
		return;

	/*
	 * First pass: collect stale UUIDs.  We must not call
	 * nvmeibt_cdv_alloc_remove() (which modifies the hash) while
	 * NVMEIB_HASH_FOREACH is active.
	 */
	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash) {
		union nvmeib_uuid bdev_uuid;
		struct nvmeibt_block_device *bdev;

		if (nvmeibt_urn_uuid_str_to_union_uuid(&bdev_uuid, alloc->cdv_uuid) < 0) {
			N_Wf(cdv_alloc_gc_bad_uuid,
			     "CDV-alloc: GC skipping entry with unparseable uuid=@STR",
			     alloc->cdv_uuid);
			continue;
		}

		bdev = nvmeibt_block_device_get_block_device_by_id(&bdev_uuid);

		/* Live CDV bdev — nothing to do. */
		if (bdev && bdev->from_config.is_cdv && !nvmeibt_blkdev_is_being_deleted(bdev))
			continue;

		if (bdev && !bdev->from_config.is_cdv) {
			N_Ef(cdv_alloc_gc_not_cdv,
			     "CDV-alloc: GC found allocator entry for non-CDV bdev uuid=@STR; removing",
			     alloc->cdv_uuid);
		} else if (bdev) {
			/* bdev exists but is already being deleted; block_device_remove()
			 * will call nvmeibt_cdv_alloc_remove() once GC runs, so skip here
			 * to avoid a double-remove race.
			 */
			continue;
		} else {
			N_Wf(cdv_alloc_gc_no_bdev,
			     "CDV-alloc: GC found stale entry for gone CDV uuid=@STR allocated=@LLU; removing",
			     alloc->cdv_uuid, alloc->n_allocated);
		}

		if (n_stale < CDV_ALLOC_GC_MAX_STALE) {
			strncpy(stale[n_stale], alloc->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
			stale[n_stale][NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
			n_stale++;
		} else {
			N_Wf(cdv_alloc_gc_overflow,
			     "CDV-alloc: GC stale-list full (@INT entries); will retry next pass",
			     CDV_ALLOC_GC_MAX_STALE);
			break;
		}
	}

	/* Second pass: remove outside of hash iteration. */
	for (i = 0; i < n_stale; i++)
		nvmeibt_cdv_alloc_remove(stale[i]);
}

/* ── Allocator election ─────────────────────────────────────────────────── */

/*
 * find_or_create_alloc — look up the per-CDV allocator; create if absent.
 */
static struct nvmeibt_cdv_alloc *find_or_create_alloc(const char *cdv_uuid)
{
	struct nvmeibt_cdv_alloc *alloc;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (alloc)
		return alloc;

	alloc = NNVMEIBT_BM_CALLOC(cdv_alloc_elect_alloc, sizeof(*alloc));
	if (!alloc) {
		N_Ef(cdv_alloc_elect_oom,
		     "CDV-alloc: elect calloc failed cdv=@STR", cdv_uuid);
		return NULL;
	}
	strncpy(alloc->cdv_uuid, cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	alloc->cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	alloc->allocator_toma_id[0] = '\0';
	alloc->cdv_fd = -1;
	XDLIST_HEAD_INIT(&alloc->extents);
	nvmeib_hash_add_ascii_str(cdv_alloc_hash, alloc->cdv_uuid, alloc);
	return alloc;
}

int nvmeibt_cdv_alloc_elect(const char *cdv_uuid,
			    const char **candidates,
			    int n_candidates)
{
	struct nvmeibt_cdv_alloc *alloc;
	const char *chosen;
	int i;

	if (n_candidates <= 0) {
		N_Ef(cdv_alloc_elect_no_cand,
		     "CDV-alloc: elect with 0 candidates cdv=@STR", cdv_uuid);
		return -EINVAL;
	}

	alloc = find_or_create_alloc(cdv_uuid);
	if (!alloc)
		return -ENOMEM;

	/* Sticky rule: keep the current allocator if it is still a candidate. */
	if (alloc->allocator_toma_id[0]) {
		for (i = 0; i < n_candidates; i++) {
			if (strncmp(alloc->allocator_toma_id, candidates[i],
				    NVMEIBT_CDV_HOSTNAME_LEN) == 0) {
				N_If(cdv_alloc_elect_sticky,
				     "CDV-alloc: elect sticky cdv=@STR allocator=@STR gen=@LLU",
				     cdv_uuid, alloc->allocator_toma_id,
				     alloc->allocator_generation);
				/*
				 * No change to allocator — but still scan the CDV
				 * if we haven't loaded the on-disk extent records yet
				 * (e.g. first call after TOMA restart when topology was
				 * already stable and the disk I/O path was not ready on
				 * the previous attempt).
				 */
				if (!alloc->ondisk_loaded)
					cdv_ondisk_scan(cdv_uuid, alloc);
				return 0;   /* 0 = sticky, no push needed */
			}
		}
	}

	/* Pick a candidate: single → use it; multiple → random. */
	if (n_candidates == 1) {
		chosen = candidates[0];
	} else {
		/*
		 * Simple deterministic hash (rdtsc-seeded) for randomness.
		 * TOMA is single-threaded; no race concern.
		 */
		uint64_t seed = (uint64_t)nvmeib_public_rdtsc();
		chosen = candidates[(unsigned int)(seed % (unsigned int)n_candidates)];
	}

	N_If(cdv_alloc_elect_new,
	     "CDV-alloc: elect cdv=@STR old=@STR new=@STR gen @LLU -> @LLU",
	     cdv_uuid,
	     alloc->allocator_toma_id[0] ? alloc->allocator_toma_id : "(none)",
	     chosen,
	     alloc->allocator_generation, alloc->allocator_generation + 1);

	strncpy(alloc->allocator_toma_id, chosen, NVMEIBT_CDV_HOSTNAME_LEN - 1);
	alloc->allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN - 1] = '\0';
	alloc->allocator_generation++;

	/* Scan CDV to rebuild extent state (first election or allocator change). */
	if (!alloc->ondisk_loaded)
		cdv_ondisk_scan(cdv_uuid, alloc);

	/* Write updated allocator identity to CDV header (best-effort). */
	{
		int fd;

		if (cdv_resolve_vol_io(cdv_uuid, alloc, &fd) == 0)
			cdv_ondisk_write_header(fd, alloc);
	}

	return 1;   /* 1 = newly elected — caller should push CDV_ALLOCATOR_UPDATE */
}

int nvmeibt_cdv_alloc_get_allocator(const char *cdv_uuid,
				    char *out_toma_id,
				    uint64_t *out_generation)
{
	struct nvmeibt_cdv_alloc *alloc;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc || alloc->allocator_toma_id[0] == '\0') {
		out_toma_id[0] = '\0';
		*out_generation = 0;
		return -ENOENT;
	}

	strncpy(out_toma_id, alloc->allocator_toma_id, NVMEIBT_CDV_HOSTNAME_LEN);
	*out_generation = alloc->allocator_generation;
	return 0;
}

/*
 * nvmeibt_cdv_alloc_push_to_registrants — broadcast CDV allocator identity.
 *
 * We reuse the CDV protocol signature + CDV_ALLOCATOR_UPDATE message type.
 * The message is sent to every active registrant on this TOMA node — only
 * clients that have the CDV attached will process it (matching by cdv_uuid).
 */
void nvmeibt_cdv_alloc_push_to_registrants(const char *cdv_uuid)
{
	struct nvmeibt_cdv_alloc *alloc;
	struct nvmeibt_cdv_allocator_update msg;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc || alloc->allocator_toma_id[0] == '\0') {
		N_Wf(cdv_push_no_alloc,
		     "CDV-alloc: push_to_registrants cdv=@STR no allocator elected",
		     cdv_uuid);
		return;
	}

	memset(&msg, 0, sizeof(msg));
	strncpy(msg.cdv_uuid, alloc->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	strncpy(msg.allocator_toma_id, alloc->allocator_toma_id,
		NVMEIBT_CDV_HOSTNAME_LEN - 1);
	msg.allocator_generation = alloc->allocator_generation;

	N_If(cdv_push_alloc_update,
	     "CDV-alloc: push CDV_ALLOCATOR_UPDATE cdv=@STR toma=@STR gen=@LLU",
	     msg.cdv_uuid, msg.allocator_toma_id, msg.allocator_generation);

	/*
	 * Broadcast to all active registrants on this TOMA node.  Every client
	 * that has the CDV attached (any disk segment of it) will receive this.
	 * Clients that don't have this CDV will ignore the unknown cdv_uuid.
	 */
	{
		struct nvmeibt_local_disk   *local_disk;
		struct nvmeibt_seg_active   *seg_active;
		struct nvmeibt_registrant_ctx *reg_ctx;

		NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
			NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
				NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_lockid) {
					if (nvmeibt_register_is_processing_registrant_removal(reg_ctx))
						continue;
					nvmeibt_register_send_msg_to_registrant(
						reg_ctx,
						NVMEIBT_CLIENT_MSG_TR_CDV_ALLOCATOR_UPDATE,
						NVMEIBT_CLIENT_TR_REASON_NONE,
						sizeof(msg), &msg);
				}
			}
		}
	}
}

/*
 * nvmeibt_cdv_alloc_push_all_to_new_registrant — unicast CDV_ALLOCATOR_UPDATE
 * for every elected CDV allocator to a single newly-registered client.
 *
 * Called right after a client's RT_REGISTER_DISK_SEGMENT succeeds so that
 * clients that register after the election still learn the allocator identity.
 */
void nvmeibt_cdv_alloc_push_all_to_new_registrant(struct nvmeibt_registrant_ctx *reg_ctx)
{
	struct nvmeibt_cdv_alloc *alloc;

	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash) {
		struct nvmeibt_cdv_allocator_update msg;

		if (alloc->allocator_toma_id[0] == '\0')
			continue;

		memset(&msg, 0, sizeof(msg));
		strncpy(msg.cdv_uuid, alloc->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
		strncpy(msg.allocator_toma_id, alloc->allocator_toma_id,
			NVMEIBT_CDV_HOSTNAME_LEN - 1);
		msg.allocator_generation = alloc->allocator_generation;

		N_If(cdv_push_alloc_update_new_reg,
		     "CDV-alloc: push CDV_ALLOCATOR_UPDATE to new registrant cdv=@STR toma=@STR gen=@LLU",
		     msg.cdv_uuid, msg.allocator_toma_id, msg.allocator_generation);

		nvmeibt_register_send_msg_to_registrant(
			reg_ctx,
			NVMEIBT_CLIENT_MSG_TR_CDV_ALLOCATOR_UPDATE,
			NVMEIBT_CLIENT_TR_REASON_NONE,
			sizeof(msg), &msg);
	}
}

/* ── One-time init / shutdown ────────────────────────────────────────────── */

int nvmeibt_cdv_alloc_one_time_init(void)
{
	cdv_alloc_hash = NVMEIB_HASH_CREATE(cdv_alloc_hash_create,
					    5,                /* 32 initial buckets */
					    "cdv_alloc_hash",
					    -1,               /* ASCII / string key */
					    false);           /* main-thread only */
	if (!cdv_alloc_hash) {
		N_Ef(cdv_alloc_init_hash, "CDV-alloc: hash_create failed");
		return -ENOMEM;
	}

	/* In-memory state starts empty.  It is rebuilt lazily per-CDV:
	 * - On allocator election (cdv_ondisk_scan from nvmeibt_cdv_alloc_elect)
	 * - On the first ALLOC request (if election hasn't scanned yet)
	 * Block devices and disks are not available at this point. */

	return 0;
}

void nvmeibt_cdv_alloc_destroy(void)
{
	struct nvmeibt_cdv_extent_entry *entry;
	int i;

	if (!cdv_alloc_hash)
		return;

	/*
	 * Walk the hash array directly to avoid reading freed pointers during
	 * NVMEIB_HASH_FOREACH's internal pointer-equality check after each free.
	 */
	for (i = 0; i < cdv_alloc_hash->n_arr_entries; i++) {
		struct nvmeibt_cdv_alloc *alloc;

		if (!hash_is_entry_OCCUPIED(&cdv_alloc_hash->arr[i]))
			continue;
		alloc = cdv_alloc_hash->arr[i].ptr_to_obj;
		if (!alloc)
			continue;

		while (!XDLIST_EMPTY(&alloc->extents)) {
			entry = XDLIST_FIRST(&alloc->extents);
			XDLIST_ELEM_DEL(&alloc->extents, entry);
			NNVMEIBT_BM_FREE(cdv_alloc_destroy_entry, entry);
		}
		NNVMEIBT_BM_FREE(cdv_alloc_destroy_alloc, alloc);
	}

	NVMEIB_HASH_TBL_FREE(cdv_alloc_hash_free, cdv_alloc_hash);
}

/* ── Capacity monitoring ─────────────────────────────────────────────────── */

/*
 * cdv_maybe_warn_capacity — check CDV usage and fire a Kafka CDVCapacityWarning
 * when the utilisation crosses NVMEIBT_CDV_WARN_PCT.
 *
 * Deduplication: the flag alloc->capacity_warning_sent suppresses repeated
 * events while usage stays above the threshold.  The flag is cleared when
 * usage drops below NVMEIBT_CDV_WARN_CLEAR_PCT (hysteresis) so that a later
 * rise above WARN_PCT fires a fresh event.
 *
 * Safe to call with total_data_extents == 0 (returns immediately).
 */
static void cdv_maybe_warn_capacity(struct nvmeibt_cdv_alloc *alloc)
{
	struct nvmeibt_Str *json;
	unsigned int used_pct;

	if (alloc->total_data_extents <= 1)
		return;   /* capacity unknown or only the reserved L1 extent */

	/* Usable extents exclude index 0 (reserved for client L1 tree). */
	used_pct = (unsigned int)(alloc->n_allocated * 100 / (alloc->total_data_extents - 1));

	if (used_pct < NVMEIBT_CDV_WARN_CLEAR_PCT) {
		/* Usage safely below hysteresis threshold — reset flag. */
		alloc->capacity_warning_sent = false;
		return;
	}

	if (used_pct < NVMEIBT_CDV_WARN_PCT)
		return;   /* in hysteresis band [85–90%) — don't fire yet */

	if (alloc->capacity_warning_sent)
		return;   /* already warned; suppress duplicate */

	json = NNVMEIBT_STR_ALLOC(cdv_cap_warn_json_alloc);
	if (!json) {
		N_Ef(cdv_cap_warn_oom,
		     "CDV: CDVCapacityWarning OOM cdv=@STR used_pct=@UINT",
		     alloc->cdv_uuid, used_pct);
		return;
	}

	nvmeibt_Str_sprintf(json,
		"{" KAFKA_PRODUCER_MSG_HEADER_FMT
		"\"payload\": {\"cdvUUID\": \"%s\", "
		"\"nAllocated\": %llu, \"totalExtents\": %llu, \"usedPct\": %u}}",
		KAFKA_PRODUCER_MSG_HEADER_VAR("cdvCapacityWarning", 1),
		alloc->cdv_uuid,
		alloc->n_allocated, alloc->total_data_extents, used_pct);

	N_Wf(cdv_cap_warn,
	     "CDV: CDVCapacityWarning cdv=@STR used_pct=@UINT n=@LLU total=@LLU",
	     alloc->cdv_uuid, used_pct, alloc->n_allocated, alloc->total_data_extents);

	nvmeibt_kafka_outgoing_msgs_queue_add(
		alloc->cdv_uuid,    /* unique_key: dedup by CDV UUID in the Kafka queue */
		nvmeibt_Str_str(json),
		nvmeibt_Str_strlen(json) + 1,
		NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);

	alloc->capacity_warning_sent = true;
	NNVMEIBT_STR_FREE(cdv_cap_warn_json_free, json);
}

/*
 * cdv_publish_alloc_stats — push a CDVAllocatorStats Kafka event.
 *
 * Sends current allocation counters (allocated / total data extents) to
 * management so the UI can display them without polling.  Uses a
 * stats-specific unique_key ("S_<cdv_uuid>") to avoid displacing the
 * CDVCapacityWarning event, which uses the bare cdv_uuid as its key.
 *
 * Called after every successful CDV_ALLOC_EXTENT and CDV_FREE_EXTENT.
 * The Kafka queue dedup mechanism coalesces rapid alloc/free sequences into
 * a single message per CDV, so the Kafka traffic is bounded.
 */
static void cdv_publish_alloc_stats(struct nvmeibt_cdv_alloc *alloc)
{
	struct nvmeibt_Str *json;
	char stats_key[NVMEIBT_CDV_UUID_STRLEN + 3];

	if (alloc->total_data_extents == 0)
		return; /* capacity not yet known; skip */

	snprintf(stats_key, sizeof(stats_key), "S_%s", alloc->cdv_uuid);

	json = NNVMEIBT_STR_ALLOC(cdv_stats_json_alloc);
	if (!json) {
		N_Ef(cdv_stats_json_oom,
		     "CDV: cdvAllocatorStats OOM cdv=@STR", alloc->cdv_uuid);
		return;
	}

	nvmeibt_Str_sprintf(json,
		"{" KAFKA_PRODUCER_MSG_HEADER_FMT
		"\"payload\": {\"cdvUUID\": \"%s\", "
		"\"allocatedExtents\": %llu, \"totalDataExtents\": %llu}}",
		KAFKA_PRODUCER_MSG_HEADER_VAR("cdvAllocatorStats", 1),
		alloc->cdv_uuid,
		alloc->n_allocated, alloc->total_data_extents);

	nvmeibt_kafka_outgoing_msgs_queue_add(
		stats_key,
		nvmeibt_Str_str(json),
		nvmeibt_Str_strlen(json) + 1,
		NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_LOW);

	NNVMEIBT_STR_FREE(cdv_stats_json_free, json);
}

/* ── Startup recovery scan ───────────────────────────────────────────────── */

void nvmeibt_cdv_alloc_startup_scan(void)
{
	struct nvmeibt_cdv_alloc *alloc;
	uint64_t n_cdvs         = 0;
	uint64_t n_extents_total = 0;

	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash) {
		N_If(cdv_startup_cdv,
		     "CDV startup: cdv=@STR n_allocated=@LLU",
		     alloc->cdv_uuid, alloc->n_allocated);
		/*
		 * total_data_extents is 0 here (not persisted); capacity checks
		 * will fire automatically on the first ALLOC request per CDV.
		 * If, via future management config, total_data_extents is set
		 * before this call, cdv_maybe_warn_capacity handles it.
		 */
		cdv_maybe_warn_capacity(alloc);
		n_cdvs++;
		n_extents_total += alloc->n_allocated;
	}

	N_If(cdv_startup_done,
	     "CDV startup: @LLU CDVs with @LLU total allocated extents restored",
	     n_cdvs, n_extents_total);
}

/* ── Status / observability ──────────────────────────────────────────────── */

void nvmeibt_cdv_alloc_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_cdv_alloc *alloc;
	uint64_t n_cdvs = 0;

	(*printf_fn)(printf_ctx,
		     "CDV ALLOCATOR (this node: %s)\n",
		     nvmeibt_get_my_hostname());

	if (!cdv_alloc_hash || nvmeib_hash_get_n_elements(cdv_alloc_hash) == 0) {
		(*printf_fn)(printf_ctx, "\t(no CDVs)\n");
		return;
	}

	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash) {
		unsigned int used_pct = 0;

		if (alloc->total_data_extents > 0)
			used_pct = (unsigned int)(alloc->n_allocated * 100
						  / alloc->total_data_extents);

		(*printf_fn)(printf_ctx,
			     "\t- cdv=%-40s allocator=%-20s gen=%-6llu allocated=%-6llu / %-6llu  (%u%%)%s\n",
			     alloc->cdv_uuid,
			     alloc->allocator_toma_id[0] ? alloc->allocator_toma_id : "(unelected)",
			     alloc->allocator_generation,
			     alloc->n_allocated,
			     alloc->total_data_extents,
			     used_pct,
			     alloc->capacity_warning_sent ? " [CAPACITY WARNING]" : "");
		n_cdvs++;
	}

	(*printf_fn)(printf_ctx, "\t%llu CDV(s) total\n", n_cdvs);
}

void nvmeibt_cdv_alloc_print_status_detailed(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_cdv_alloc *alloc;

	(*printf_fn)(printf_ctx,
		     "CDV ALLOCATOR DETAILED (this node: %s)\n",
		     nvmeibt_get_my_hostname());

	if (!cdv_alloc_hash || nvmeib_hash_get_n_elements(cdv_alloc_hash) == 0) {
		(*printf_fn)(printf_ctx, "\t(no CDVs)\n");
		return;
	}

	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash) {
		struct nvmeibt_cdv_extent_entry *entry;
		unsigned int used_pct = 0;

		if (alloc->total_data_extents > 0)
			used_pct = (unsigned int)(alloc->n_allocated * 100
						  / alloc->total_data_extents);

		(*printf_fn)(printf_ctx,
			     "\tcdv=%s  allocator=%s  gen=%llu  allocated=%llu/%llu (%u%%)  ondisk_loaded=%s\n",
			     alloc->cdv_uuid,
			     alloc->allocator_toma_id[0] ? alloc->allocator_toma_id : "(unelected)",
			     alloc->allocator_generation,
			     alloc->n_allocated,
			     alloc->total_data_extents,
			     used_pct,
			     alloc->ondisk_loaded ? "yes" : "no");

		if (!XDLIST_EMPTY(&alloc->extents)) {
			(*printf_fn)(printf_ctx,
				     "\t  %-8s  %s\n",
				     "ext_idx", "tpv_uuid");
			XDLIST_FOREACH(entry, &alloc->extents) {
				(*printf_fn)(printf_ctx,
					     "\t  %-8llu  %s\n",
					     entry->extent_index,
					     entry->tpv_uuid);
			}
		} else {
			(*printf_fn)(printf_ctx, "\t  (no extents)\n");
		}
	}
}

/* ── TPV deletion: free all extents + zero L1 tree ───────────────────────── */

int nvmeibt_cdv_alloc_free_all_for_tpv(const char *cdv_uuid,
					const char *tpv_uuid,
					uint32_t    allocator_size_gb,
					uint32_t    cdv_extent_size_mb)
{
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;
	int      fd;
	uint64_t n_freed = 0;
	int      rv;

	(void)allocator_size_gb;
	(void)cdv_extent_size_mb;

	/* ── 1. Find or create the per-CDV allocator; scan if stale ── */
	alloc = find_or_create_alloc(cdv_uuid);
	if (!alloc)
		return -ENOMEM;

	rv = cdv_resolve_vol_io(cdv_uuid, alloc, &fd);
	if (rv) {
		N_Ef(cdv_free_all_no_disk,
		     "CDV-alloc: free_all cdv=@STR tpv=@STR cannot open CDV volume rv=@INT",
		     cdv_uuid, tpv_uuid, rv);
		return rv;
	}

	if (!alloc->ondisk_loaded)
		cdv_ondisk_scan(cdv_uuid, alloc);

	/* ── 2. Free in-memory entries + on-disk records for this TPV ── */
	XDLIST_FOREACH_SAFE(entry, &alloc->extents) {
		if (strncmp(entry->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN) != 0)
			continue;

		if (cdv_ondisk_write_record(fd, entry->extent_index, NULL) < 0)
			N_Wf(cdv_free_all_rec_err,
			     "CDV-alloc: free_all cdv=@STR idx=@LLU write_record failed; continuing",
			     cdv_uuid, entry->extent_index);

		XDLIST_ELEM_DEL(&alloc->extents, entry);
		alloc->n_allocated--;
		NNVMEIBT_BM_FREE(cdv_free_all_entry, entry);
		n_freed++;
	}

	/* ── 3. Rewrite header to reflect updated n_allocated ── */
	if (n_freed > 0)
		cdv_ondisk_write_header(fd, alloc);

	/* Check whether the capacity warning flag can be cleared. */
	cdv_maybe_warn_capacity(alloc);

	N_If(cdv_free_all_done,
	     "CDV-alloc: free_all cdv=@STR tpv=@STR freed=@LLU remaining=@LLU",
	     cdv_uuid, tpv_uuid, n_freed, alloc->n_allocated);

	/*
	 * ── 4. Zero the TPV's tree extent L1 header ──
	 *
	 * The TPV's L1/L2 tree lives in one of the freed CDV_extents (the
	 * "tree extent"), identified by a magic header at slot 0.  The L1
	 * header was already cleared when cdv_ondisk_write_record(NULL) reset
	 * the on-disk record for that extent in step 2.  The physical slot
	 * data (L1/L2 tables in the CDV data region) will be zeroed by the
	 * NEEDS_ZEROING → background-zero path before the extent is reused.
	 *
	 * No additional zeroing is needed here.  The old flat-L1 code zeroed
	 * CDV_extent[0] (the shared L1), which is no longer applicable —
	 * each TPV has its own tree extent.
	 */

	return 0;
}

/* ── Incoming-message handler ────────────────────────────────────────────── */

/*
 * Helper: send a CDV response to the requesting client.
 *
 * Wraps nvmeibt_toma_send_msg_to_client() with a CDV-appropriate praid_version
 * (always 0 — CDV operations are topology-independent) and a unique msg_id.
 */
static int cdv_send_response(struct nvmeibt_registrant_ctx *reg_ctx,
			     enum NVMEIBT_CLIENT_MSG_TYPES msg_type,
			     int data_length, void *data)
{
	u64 msg_id = (u64)nvmeib_public_rdtsc();

	return nvmeibt_toma_send_msg_to_client(reg_ctx,
					       0,		/* praid_version: N/A for CDV */
					       msg_type,
					       NVMEIBT_CLIENT_TR_REASON_NONE,
					       data_length, data, msg_id);
}

/*
 * handle_cdv_alloc_extent — pick a free CDV_extent, record it, and reply.
 *
 * Algorithm:
 *   1. Look up (or note absence of) the per-CDV allocator.
 *   2. Check allocator_generation against client's client_generation.
 *   3. Find the first unallocated extent index in [0, total_data_extents).
 *   4. Record via cdv_alloc_insert() and persist atomically.
 *   5. On persist failure, undo the in-memory insert and return ERROR.
 */
static int handle_cdv_alloc_extent(struct nvmeibt_register_msg *msg)
{
	const struct nvmeibt_cdv_alloc_req *req;
	struct nvmeibt_cdv_alloc_resp       resp;
	struct nvmeibt_cdv_alloc           *alloc;
	struct nvmeibt_cdv_extent_entry    *entry;
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	uint64_t candidate, total, i;
	bool     first_alloc, found, occupied;
	int      rv;

	if (msg->data_length < (int)sizeof(*req)) {
		N_Ef(cdv_handle_alloc_short,
		     "CDV: ALLOC_EXTENT short len=@INT", msg->data_length);
		return -EINVAL;
	}

	req = (const struct nvmeibt_cdv_alloc_req *)msg->msg_data;

	/* NUL-terminate defensively into local buffers. */
	memcpy(cdv_uuid, req->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN);
	cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	memcpy(tpv_uuid, req->tpv_uuid, NVMEIBT_CDV_UUID_STRLEN);
	tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';

	memset(&resp, 0, sizeof(resp));
	resp.req_id = req->req_id;

	alloc       = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);

	/* On-demand CDV scan: if we have an allocator entry but haven't yet
	 * loaded the on-CDV extent records, do it now before serving ALLOCs.
	 */
	if (alloc && !alloc->ondisk_loaded) {
		if (alloc->total_data_extents == 0 && req->total_data_extents > 0)
			alloc->total_data_extents = req->total_data_extents;
		cdv_ondisk_scan(cdv_uuid, alloc);
	}

	first_alloc = (alloc == NULL);

	/*
	 * ── Verify we are the elected allocator for this CDV ────────────────
	 *
	 * Only the elected allocator TOMA should serve ALLOC requests.  If
	 * the allocator has been elected but it's a different node, reject
	 * with WRONG_GEN so the client re-syncs from the CDV topology push.
	 */
	if (!first_alloc && alloc->allocator_toma_id[0] &&
	    strncmp(alloc->allocator_toma_id, nvmeibt_get_my_hostname(),
		    NVMEIBT_CDV_HOSTNAME_LEN) != 0) {
		N_Wf(cdv_alloc_not_allocator,
		     "CDV: ALLOC cdv=@STR rejected: this node=@STR is not allocator=@STR",
		     cdv_uuid, nvmeibt_get_my_hostname(), alloc->allocator_toma_id);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/* ── Generation check ────────────────────────────────────────────────── */
	if (first_alloc && req->client_generation != 0) {
		/*
		 * No allocator for this CDV yet (RAFT has not assigned one, or
		 * state was lost).  Client carries a stale non-zero generation.
		 * Return WRONG_GEN with 0; client will re-sync when RAFT
		 * distributes the current generation.
		 */
		N_Wf(cdv_alloc_wrong_gen_new,
		     "CDV: ALLOC no state cdv=@STR client_gen=@LLU => WRONG_GEN",
		     cdv_uuid, req->client_generation);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = 0;
		goto send;
	}
	if (!first_alloc && req->client_generation != alloc->allocator_generation) {
		N_If(cdv_alloc_wrong_gen,
		     "CDV: ALLOC WRONG_GEN cdv=@STR client=@LLU toma=@LLU",
		     cdv_uuid, req->client_generation, alloc->allocator_generation);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/* ── Capacity info ───────────────────────────────────────────────────── */
	if (first_alloc && req->total_data_extents == 0) {
		N_Ef(cdv_alloc_no_cap,
		     "CDV: ALLOC cdv=@STR total_data_extents=0 on first alloc; refusing",
		     cdv_uuid);
		resp.status = NVMEIBT_CDV_ALLOC_ERROR;
		goto send;
	}

	/* Refresh cached capacity after a TOMA restart (stored value is 0). */
	if (!first_alloc && alloc->total_data_extents == 0 && req->total_data_extents > 0)
		alloc->total_data_extents = req->total_data_extents;

	total = first_alloc ? req->total_data_extents : alloc->total_data_extents;

	if (total == 0) {
		/* alloc exists but capacity unknown; client sent 0 too */
		N_Ef(cdv_alloc_no_cap_post_restart,
		     "CDV: ALLOC cdv=@STR no capacity known after restart; refusing",
		     cdv_uuid);
		resp.status = NVMEIBT_CDV_ALLOC_ERROR;
		goto send;
	}

	/* ── CDV-full check (extent 0 reserved for L1 tree) ─────────────────── */
	if (!first_alloc && alloc->n_allocated >= total - 1) {
		N_Wf(cdv_alloc_full,
		     "CDV: ALLOC cdv=@STR FULL allocated=@LLU usable=@LLU",
		     cdv_uuid, alloc->n_allocated, total - 1);
		cdv_maybe_warn_capacity(alloc);   /* ensure Kafka event reaches management */
		resp.status = NVMEIBT_CDV_ALLOC_CDV_FULL;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/*
	 * ── Find first free extent index ────────────────────────────────────
	 *
	 * CDV_extent[0] is reserved for the client-side L1 metadata tree
	 * (tpv_tree_entry array persisted by nvmeibc_tpv_flush_state).
	 * Data extents start at index 1.
	 */
	if (first_alloc) {
		candidate = 1;
		found     = true;
	} else {
		candidate = 0;
		found     = false;
		for (i = 1; i < total; i++) {
			occupied = false;
			XDLIST_FOREACH(entry, &alloc->extents) {
				if (entry->extent_index == i) {
					occupied = true;
					break;
				}
			}
			if (!occupied) {
				candidate = i;
				found = true;
				break;
			}
		}
	}

	if (!found) {
		/* n_allocated < total yet no free slot — internal inconsistency */
		N_Ef(cdv_alloc_scan_bug,
		     "CDV: ALLOC scan bug cdv=@STR n=@LLU total=@LLU",
		     cdv_uuid, alloc->n_allocated, total);
		resp.status = NVMEIBT_CDV_ALLOC_ERROR;
		goto send;
	}

	/* ── Record the allocation ───────────────────────────────────────────── */
	rv = cdv_alloc_insert(cdv_uuid, candidate, tpv_uuid);
	if (rv) {
		N_Ef(cdv_alloc_insert_fail,
		     "CDV: ALLOC insert failed rv=@INT cdv=@STR idx=@LLU",
		     rv, cdv_uuid, candidate);
		resp.status = NVMEIBT_CDV_ALLOC_ERROR;
		goto send;
	}

	/*
	 * Re-look up alloc: if this was the first allocation, cdv_alloc_insert
	 * just created the struct and added it to the hash.
	 */
	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);

	/* Populate cached capacity on freshly created allocator. */
	if (alloc && alloc->total_data_extents == 0)
		alloc->total_data_extents = req->total_data_extents;

	/*
	 * Mark ondisk_loaded on first-ever allocation.
	 *
	 * cdv_alloc_insert() creates the alloc struct with ondisk_loaded=false.
	 * Without this, the next ALLOC request would see (alloc && !ondisk_loaded)
	 * and call cdv_ondisk_scan(), which would re-read the just-written on-disk
	 * record for extent[0] and call cdv_alloc_insert() again — doubling
	 * n_allocated.  With a small CDV (1–2 data extents) that pushes
	 * n_allocated >= total_data_extents and causes a false CDV_FULL.
	 *
	 * Setting ondisk_loaded=true here is safe: in-memory state was built from
	 * live ALLOC requests and is authoritative; no disk scan is needed.
	 * The scan path (ondisk_loaded=false on entry) is reserved for the
	 * post-restart case where alloc exists but extent list was not yet rebuilt.
	 */
	if (alloc && first_alloc)
		alloc->ondisk_loaded = true;

	resp.extent_index         = candidate;
	resp.allocator_generation = alloc ? alloc->allocator_generation : 0;
	resp.status               = NVMEIBT_CDV_ALLOC_OK;

	/* Check if this allocation pushed the CDV above the warning watermark. */
	if (alloc) {
		cdv_maybe_warn_capacity(alloc);
		cdv_publish_alloc_stats(alloc);
	}

	N_If(cdv_alloc_ok,
	     "CDV: ALLOC OK cdv=@STR idx=@LLU tpv=@STR req_id=@LLU gen=@LLU",
	     cdv_uuid, candidate, tpv_uuid, req->req_id, resp.allocator_generation);

	/* Persist the allocation record to the CDV itself. */
	{
		int _fd;

		if (cdv_resolve_vol_io(cdv_uuid, alloc, &_fd) == 0) {
			if (cdv_ondisk_write_record(_fd, candidate, tpv_uuid) < 0)
				N_Wf(cdv_alloc_persist_fail,
				     "CDV: ALLOC cdv=@STR idx=@LLU on-CDV write failed",
				     cdv_uuid, candidate);
			if (alloc)
				cdv_ondisk_write_header(_fd, alloc);
		}
	}

send:
	return cdv_send_response(&msg->registrant_ctx,
				 NVMEIBT_CLIENT_MSG_TR_CDV_ALLOC_EXTENT_RSP,
				 sizeof(resp), &resp);
}

/*
 * handle_cdv_free_extent — CDV_FREE_EXTENT: validate ownership, free the extent.
 *
 * Fire-and-forget: no response is sent.  The handler is idempotent — freeing
 * an already-free or unknown extent is a no-op (logged at WARN).
 */
static int handle_cdv_free_extent(struct nvmeibt_register_msg *msg)
{
	const struct nvmeibt_cdv_free_req *req;
	struct nvmeibt_cdv_alloc          *alloc;
	struct nvmeibt_cdv_extent_entry   *entry;
	char cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];

	if (msg->data_length < (int)sizeof(*req)) {
		N_Ef(cdv_handle_free_short,
		     "CDV: FREE_EXTENT short len=@INT", msg->data_length);
		return -EINVAL;
	}

	req = (const struct nvmeibt_cdv_free_req *)msg->msg_data;

	memcpy(cdv_uuid, req->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN);
	cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	memcpy(tpv_uuid, req->tpv_uuid, NVMEIBT_CDV_UUID_STRLEN);
	tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		N_Wf(cdv_free_no_cdv,
		     "CDV: FREE cdv=@STR idx=@LLU no allocator found (already freed?)",
		     cdv_uuid, req->extent_index);
		return 0;
	}

	XDLIST_FOREACH_SAFE(entry, &alloc->extents) {
		if (entry->extent_index != req->extent_index)
			continue;

		/* Ownership check: only the owning TPV may free the extent. */
		if (strncmp(entry->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN) != 0) {
			N_Wf(cdv_free_wrong_owner,
			     "CDV: FREE cdv=@STR idx=@LLU owner=@STR requester=@STR; ignoring",
			     cdv_uuid, req->extent_index, entry->tpv_uuid, tpv_uuid);
			return 0;
		}

		XDLIST_ELEM_DEL(&alloc->extents, entry);
		alloc->n_allocated--;

		N_If(cdv_free_ok,
		     "CDV: FREE OK cdv=@STR idx=@LLU tpv=@STR remaining=@LLU",
		     cdv_uuid, req->extent_index, tpv_uuid, alloc->n_allocated);

		NNVMEIBT_BM_FREE(cdv_free_entry, entry);

		/* Clear the extent record on the CDV itself. */
		{
			int _fd;

			if (cdv_resolve_vol_io(cdv_uuid, alloc, &_fd) == 0) {
				if (cdv_ondisk_write_record(_fd,
							   req->extent_index, NULL) < 0)
					N_Wf(cdv_free_persist_fail,
					     "CDV: FREE cdv=@STR idx=@LLU on-CDV clear failed",
					     cdv_uuid, req->extent_index);
			}
		}

		/*
		 * Hysteresis check: if the free dropped usage below
		 * NVMEIBT_CDV_WARN_CLEAR_PCT, clear capacity_warning_sent so the
		 * next rise above WARN_PCT fires a fresh Kafka event.
		 */
		cdv_maybe_warn_capacity(alloc);
		cdv_publish_alloc_stats(alloc);
		return 0;
	}

	N_Wf(cdv_free_notfound,
	     "CDV: FREE cdv=@STR idx=@LLU tpv=@STR not found (already freed?)",
	     cdv_uuid, req->extent_index, tpv_uuid);
	return 0;
}

/*
 * handle_cdv_list_extents — CDV_LIST_EXTENTS: return all extents owned by tpv.
 *
 * Sends a variable-length response: nvmeibt_cdv_list_resp header followed
 * immediately by n_extents × uint64_t extent indices.
 */
static int handle_cdv_list_extents(struct nvmeibt_register_msg *msg)
{
	const struct nvmeibt_cdv_list_req *req;
	struct nvmeibt_cdv_list_resp      *resp;
	struct nvmeibt_cdv_list_resp       err_resp;
	uint64_t *indices;
	uint64_t *dst;
	uint64_t  n_extents, i;
	char      cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char      tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	size_t    resp_size;
	int       rv;

	if (msg->data_length < (int)sizeof(*req)) {
		N_Ef(cdv_handle_list_short,
		     "CDV: LIST_EXTENTS short len=@INT", msg->data_length);
		return -EINVAL;
	}

	req = (const struct nvmeibt_cdv_list_req *)msg->msg_data;

	memcpy(cdv_uuid, req->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN);
	cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	memcpy(tpv_uuid, req->tpv_uuid, NVMEIBT_CDV_UUID_STRLEN);
	tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';

	rv = nvmeibt_cdv_alloc_list_for_tpv(cdv_uuid, tpv_uuid, &indices, &n_extents);
	if (rv) {
		N_Ef(cdv_list_query_fail,
		     "CDV: LIST_EXTENTS query failed rv=@INT cdv=@STR tpv=@STR",
		     rv, cdv_uuid, tpv_uuid);
		memset(&err_resp, 0, sizeof(err_resp));
		err_resp.req_id = req->req_id;
		err_resp.status = 1;
		return cdv_send_response(&msg->registrant_ctx,
					 NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP,
					 sizeof(err_resp), &err_resp);
	}

	resp_size = sizeof(*resp) + n_extents * sizeof(*dst);
	resp      = NNVMEIBT_BM_ALLOC(cdv_list_resp_alloc, resp_size);
	if (!resp) {
		N_Ef(cdv_list_resp_oom,
		     "CDV: LIST_EXTENTS OOM cdv=@STR tpv=@STR n=@LLU",
		     cdv_uuid, tpv_uuid, n_extents);
		if (indices)
			NNVMEIBT_BM_FREE(cdv_list_indices_free, indices);
		memset(&err_resp, 0, sizeof(err_resp));
		err_resp.req_id = req->req_id;
		err_resp.status = 1;
		return cdv_send_response(&msg->registrant_ctx,
					 NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP,
					 sizeof(err_resp), &err_resp);
	}

	resp->req_id    = req->req_id;
	resp->n_extents = n_extents;
	resp->status    = 0;
	dst = (uint64_t *)((uint8_t *)resp + sizeof(*resp));
	for (i = 0; i < n_extents; i++)
		dst[i] = indices[i];

	if (indices)
		NNVMEIBT_BM_FREE(cdv_list_indices_free2, indices);

	N_If(cdv_list_ok,
	     "CDV: LIST_EXTENTS OK cdv=@STR tpv=@STR n=@LLU",
	     cdv_uuid, tpv_uuid, n_extents);

	rv = cdv_send_response(&msg->registrant_ctx,
			       NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP,
			       (int)resp_size, resp);
	NNVMEIBT_BM_FREE(cdv_list_resp_free, resp);
	return rv;
}

int nvmeibt_cdv_handle_incoming_msg(struct nvmeibt_register_msg *msg)
{
	N_Tf(cdv_handle_dispatch,
	     "CDV-handle: msg_type=@MSG_TYPE data_len=@DATA_LEN cookie=@COOKIE",
	     msg->msg_type, msg->data_length, msg->cookie);

	switch (msg->msg_type) {
	case NVMEIBT_CLIENT_MSG_RT_CDV_ALLOC_EXTENT:
		return handle_cdv_alloc_extent(msg);

	case NVMEIBT_CLIENT_MSG_RT_CDV_FREE_EXTENT:
		return handle_cdv_free_extent(msg);

	case NVMEIBT_CLIENT_MSG_RT_CDV_LIST_EXTENTS:
		return handle_cdv_list_extents(msg);

	default:
		N_Ef(cdv_handle_unknown,
		     "CDV-handle: unexpected msg_type=@MSG_TYPE", msg->msg_type);
		return -EINVAL;
	}
}
