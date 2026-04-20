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
 * region is scanned asynchronously (via a work queue) to rebuild in-memory state.
 */

#include <string.h>
#include <errno.h>
#include <unistd.h>			/* usleep for scan retry backoff */
#include "nvmeibt_cdv_alloc.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_praid_basics.h"	/* struct nvmeibt_praid_topo_ctx (field access) */
#include "nvmeibt_disk_segment.h"	/* struct nvmeibt_seg_lot (praid_lot field access) */
#include "nvmeibt_praid.h"		/* nvmeibt_praid_get_blkdev */
#include "nvmeibt_register.h"		/* struct nvmeibt_register_msg, nvmeibt_register_send_msg_to_registrant */
#include "nvmeibt_toma.h"		/* nvmeibt_toma_send_msg_to_client, nvmeibt_global_get_global */
#include "nvmeibt_global.h"		/* struct nvmeibt_topology (full definition) */
#include "nvmeibt_kafka.h"		/* nvmeibt_kafka_outgoing_msgs_queue_add, KAFKA_PRODUCER_MSG_HEADER_* */
#include "nvmeibt_local_disk.h"		/* struct nvmeibt_local_disk */
#include "nvmeibt_seg_active.h"		/* struct nvmeibt_seg_active, registrant iteration */
#include "../common/nvmeib_hash.h"
#include "vol/nvmeibt_block_device.h"	/* nvmeibt_block_device_get_block_device_by_id, nvmeibt_blkdev_is_being_deleted */
#include "utils/nvmeibt_uuid.h"		/* nvmeibt_urn_uuid_str_to_union_uuid */
#include "nvmeibt_wq.h"		/* struct nvmeibt_wq, nvmeibt_wq_addw */
#include "nvmeibt_node.h"	/* nvmeibt_node_get_node_by_id, nvmeibt_node_name */
#include "nvmeibt_raft.h"	/* nvmeibt_raft_get_current_term */

/* ── Forward declarations ────────────────────────────────────────────────── */

static void cdv_maybe_warn_capacity(struct nvmeibt_cdv_alloc *alloc);
static void cdv_publish_alloc_stats(struct nvmeibt_cdv_alloc *alloc);

/* ── Global state ────────────────────────────────────────────────────────── */

/* cdv_uuid (ASCII string) → nvmeibt_cdv_alloc * */
static struct nvmeib_hash_table *cdv_alloc_hash;

/*
 * Runtime config — see nvmeibt_cdv_alloc.h for semantics.
 * Registered in oper_params[] (nvmeibt_debug.c) as "cdv_extent_zero_on_free".
 */
int64_t nvmeibt_cdv_extent_zero_on_free = CDV_EXTENT_ZERO_ON_FREE_DEFAULT;

/* ── Per-CDV I/O infrastructure ─────────────────────────────────────────── */

/*
 * All CDV disk I/O (reads during scan, writes for record/header persistence)
 * runs on a per-CDV worker thread (alloc->io_wq).  This prevents blocking the
 * TOMA main thread on NVMesh volume I/O, which can take tens of seconds.
 *
 * Each CDV has its own WQ so multiple CDVs are serviced in parallel.
 * Within a single CDV, operations are serialized (single worker thread).
 *
 * The device path (alloc->dev_path) is resolved on the main thread (bdev hash
 * is not thread-safe).  The file descriptor (alloc->cdv_fd) is opened lazily
 * by the worker thread and cached for reuse across WQ entries.
 */

#define CDV_DEV_PATH_PREFIX	"/dev/nvmesh/"

/*
 * cdv_ensure_io_wq — ensure the per-allocator I/O work queue exists and the
 * satellite (or, in legacy mode, CDV) device path is resolved.
 *
 * After Phase 3, the allocator metadata lives on the satellite volume named
 * '<cdvName>-mgmt'.  When alloc->satellite_uuid is set (i.e., management has
 * confirmed the satellite is attached to us via Phase 2), this resolves the
 * satellite block device path and the I/O WQ does its work against it.
 *
 * In legacy/transitional mode (satellite_uuid empty), falls back to resolving
 * the CDV itself.  This keeps the function compatible with the (now-rare)
 * case where the satellite has not yet been attached but the allocator already
 * has the CDV available.  The legacy path will go away when all callers
 * gate I/O on state == ACTIVE.
 *
 * Main-thread only.
 *
 * Returns 0 on success.  Returns -ENOENT if neither bdev is available yet,
 * -ENOMEM on allocation failure.
 */
static int cdv_ensure_io_wq(const char *cdv_uuid,
			     struct nvmeibt_cdv_alloc *alloc)
{
	/* Resolve satellite path lazily once we know its UUID. */
	if (alloc->satellite_uuid[0] && !alloc->satellite_dev_path[0]) {
		union nvmeib_uuid sat_uuid;
		struct nvmeibt_block_device *bdev;

		if (nvmeibt_urn_uuid_str_to_union_uuid(&sat_uuid, alloc->satellite_uuid) >= 0) {
			bdev = nvmeibt_block_device_get_block_device_by_id(&sat_uuid);
			if (bdev) {
				snprintf(alloc->satellite_dev_path, sizeof(alloc->satellite_dev_path),
					 "%s%s", CDV_DEV_PATH_PREFIX, bdev->from_config.client_blkdev_name);
			}
		}
	}

	if (alloc->io_wq)
		return 0;   /* already set up */

	/*
	 * Legacy: resolve the CDV path too.  Used by cdv_zero_execute (data
	 * extent zeroing on free), which still targets the CDV directly.
	 * This requires the CDV to be attached to this TOMA — only true when
	 * cdv_extent_zero_on_free has been enabled with the operator-arranged
	 * CDV attach.  See struct nvmeibt_cdv_alloc::cdv_fd doc comment.
	 */
	if (!alloc->dev_path[0]) {
		union nvmeib_uuid bdev_uuid;
		struct nvmeibt_block_device *bdev;

		if (nvmeibt_urn_uuid_str_to_union_uuid(&bdev_uuid, cdv_uuid) < 0) {
			if (!alloc->satellite_dev_path[0])
				return -EINVAL;
		} else {
			bdev = nvmeibt_block_device_get_block_device_by_id(&bdev_uuid);
			if (bdev && bdev->from_config.is_cdv) {
				snprintf(alloc->dev_path, sizeof(alloc->dev_path), "%s%s",
					 CDV_DEV_PATH_PREFIX, bdev->from_config.client_blkdev_name);
			}
		}
	}

	/* We need at least one resolved path to proceed. */
	if (!alloc->satellite_dev_path[0] && !alloc->dev_path[0])
		return -ENOENT;

	{
		char wq_name[80];

		snprintf(wq_name, sizeof(wq_name), "CDV_IO_%.60s", cdv_uuid);
		alloc->io_wq = nvmeibt_wq_create(wq_name);
	}
	if (!alloc->io_wq) {
		N_Ef(cdv_io_wq_fail,
		     "CDV-alloc: failed to create I/O WQ for cdv=@STR", cdv_uuid);
		return -ENOMEM;
	}

	N_If(cdv_io_wq_created,
	     "CDV-alloc: created I/O WQ for cdv=@STR sat_path=@STR cdv_path=@STR",
	     cdv_uuid,
	     alloc->satellite_dev_path[0] ? alloc->satellite_dev_path : "(unresolved)",
	     alloc->dev_path[0] ? alloc->dev_path : "(unresolved)");
	return 0;
}

/*
 * cdv_worker_open_fd — open the allocator-metadata block device (satellite if
 * bound, else legacy CDV) from the worker thread (lazy).
 *
 * Must ONLY be called from the io_wq worker thread.  Used by the header /
 * record / scan I/O paths which target the allocator metadata region.  The
 * satellite is preferred when its path is resolved (post-Stage-B); legacy
 * CDV is only used during the transition / before satellite is bound.
 *
 * NOTE: cdv_zero_execute (data-extent zeroing on free) must NOT use this —
 * it must open the CDV directly via cdv_worker_open_cdv_fd_for_zeroing()
 * because zeroing targets data-extent offsets on the CDV, not the satellite.
 *
 * Returns the fd (>= 0) on success, negative on failure.
 */
static int cdv_worker_open_fd(struct nvmeibt_cdv_alloc *alloc)
{
	if (alloc->satellite_dev_path[0]) {
		if (alloc->satellite_fd >= 0)
			return alloc->satellite_fd;
		alloc->satellite_fd = NNVMEIBT_OPEN_LOCAL_DISK_WRITE(cdv_wq_sat_open,
								      alloc->satellite_dev_path);
		return alloc->satellite_fd;
	}

	if (alloc->cdv_fd >= 0)
		return alloc->cdv_fd;

	alloc->cdv_fd = NNVMEIBT_OPEN_LOCAL_DISK_WRITE(cdv_wq_vol_open,
							alloc->dev_path);
	return alloc->cdv_fd;
}

/*
 * cdv_worker_open_cdv_fd_for_zeroing — open the CDV block device specifically
 * for cdv_zero_execute, which writes zeros to CDV data-extent offsets.
 *
 * Must ONLY be called from the io_wq worker thread.  Always targets the CDV
 * (alloc->dev_path), never the satellite.  Returns negative if the CDV is not
 * attached to this TOMA — which is the default state after the satellite
 * migration; zero-on-free will fail gracefully in that case (the extent stays
 * in NEEDS_ZEROING and is not reused).
 */
static int cdv_worker_open_cdv_fd_for_zeroing(struct nvmeibt_cdv_alloc *alloc)
{
	if (alloc->cdv_fd >= 0)
		return alloc->cdv_fd;

	if (!alloc->dev_path[0])
		return -ENODEV;

	alloc->cdv_fd = NNVMEIBT_OPEN_LOCAL_DISK_WRITE(cdv_wq_cdv_open,
							alloc->dev_path);
	return alloc->cdv_fd;
}

/*
 * cdv_ondisk_record_offset — byte offset within the satellite volume for
 * extent_index's record.
 *
 * Extent indices are 1-based (index 0 is unused; the satellite header lives
 * at offset 0):
 *   Offset 0:                            Header (4 KiB)
 *   Offset 1 * CDV_ONDISK_BLOCK_SIZE:    Record for extent 1
 *   Offset 2 * CDV_ONDISK_BLOCK_SIZE:    Record for extent 2
 *   ...
 */
static inline uint64_t cdv_ondisk_record_offset(uint64_t extent_index)
{
	return (uint64_t)CDV_ONDISK_BLOCK_SIZE * extent_index;
}

/* Note: the old synchronous cdv_ondisk_write_record() and cdv_ondisk_write_header()
 * have been replaced by cdv_async_write_record() and cdv_async_write_header()
 * which prepare buffers on the main thread and dispatch writes to the per-CDV
 * I/O WQ.  See the "Async CDV writes" section below.
 */

/* ── Async CDV ondisk scan (work-queue based) ──────────────────────────────
 *
 * CDV scans read the entire allocator region (header + per-extent records) and
 * can block for tens of seconds waiting on NVMesh volume I/O.  Running them on
 * the main thread stalls all TOMA processing.
 *
 * The scan is split into two phases following the standard TOMA WQ pattern:
 *   execute  (worker thread): open CDV, read header + records, build result array
 *   finalize (main thread):   apply results to the per-CDV allocator struct
 *
 * Callers check alloc->ondisk_loaded and alloc->scan_in_progress: if a scan is
 * already in flight they simply return "not ready" (e.g. -EAGAIN) to the client,
 * which will retry.
 */

/* One scanned extent — populated by the worker, consumed by finalize. */
struct cdv_scan_result_entry {
	uint64_t extent_index;
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	bool     needs_zeroing;
	uint32_t zeroing_allocator_size_gib;
	uint32_t zeroing_cdv_extent_size_mib;
};

/* WQ entry for async CDV ondisk scan. */
struct cdv_ondisk_scan_wq_entry {
	struct nvmeibt_wq_entry   wq_entry;

	/* ── Input (set by dispatcher on main thread, read by worker) ── */
	struct nvmeibt_cdv_alloc *alloc;	/* for cached fd; valid: we drain before remove */
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	uint32_t pre_sleep_ms;			/* worker sleeps this long before the I/O —
						 * drives the scan retry backoff */

	/* ── Output (set by worker, consumed by finalize) ── */
	int      rv;			/* 0 = success, negative = error */
	bool     is_fresh;		/* true if CDV has no valid header (genuinely new) */
	uint64_t total_data_extents;	/* from header */
	uint64_t allocator_generation;	/* from header */
	struct cdv_scan_result_entry *results; /* heap array of loaded extents */
	uint64_t n_results;		/* number of entries in results[] */
};

/*
 * cdv_scan_execute — worker thread: perform all blocking CDV I/O.
 *
 * Opens the CDV volume, reads the header and every extent record, and stores
 * the results in the wq_entry.  No shared state is modified here — all
 * mutations happen in cdv_scan_finalize on the main thread.
 */
static void cdv_scan_execute(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_ondisk_scan_wq_entry *e =
		container_of(wq_entry, struct cdv_ondisk_scan_wq_entry, wq_entry);
	struct cdv_alloc_ondisk_header *hdr = NULL;
	struct cdv_alloc_ondisk_record *rec = NULL;
	struct cdv_scan_result_entry *results = NULL;
	uint64_t i, total, n_results = 0, results_cap = 0;
	int fd;

	e->rv = -EIO;
	e->is_fresh = false;
	e->results = NULL;
	e->n_results = 0;

	/* Backoff delay from the previous failed scan (100ms → 1000ms). */
	if (e->pre_sleep_ms)
		usleep((useconds_t)e->pre_sleep_ms * 1000);

	/* Open the CDV volume (lazy, cached in alloc->cdv_fd). */
	fd = cdv_worker_open_fd(e->alloc);
	if (fd < 0) {
		N_Wf(cdv_async_scan_open_fail,
		     "CDV-alloc: async scan cdv=@STR cannot open volume; will retry",
		     e->cdv_uuid);
		goto done;
	}

	hdr = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_async_scan_hdr_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	rec = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_async_scan_rec_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!hdr || !rec) {
		e->rv = -ENOMEM;
		goto done;
	}

	/* Read header at CDV byte 0 */
	if (NNVMEIBT_PREAD(cdv_async_scan_hdr_rd, fd, hdr, CDV_ONDISK_BLOCK_SIZE,
			   0ULL, 1) < 0) {
		N_Wf(cdv_async_scan_hdr_err,
		     "CDV-alloc: async scan cdv=@STR header read failed; will retry",
		     e->cdv_uuid);
		e->rv = -EIO;
		goto done;
	}

	if (hdr->magic != CDV_ONDISK_MAGIC) {
		N_If(cdv_async_scan_fresh,
		     "CDV-alloc: async scan cdv=@STR no valid header (magic=@X); fresh CDV",
		     e->cdv_uuid, hdr->magic);
		e->is_fresh = true;
		e->rv = 0;
		goto done;
	}

	{
		uint32_t expected_crc = crc32_seedless(hdr,
			offsetof(struct cdv_alloc_ondisk_header, crc32));
		if (hdr->crc32 != expected_crc) {
			N_Wf(cdv_async_scan_hdr_crc,
			     "CDV-alloc: async scan cdv=@STR header CRC mismatch (got=@X want=@X); will retry",
			     e->cdv_uuid, hdr->crc32, expected_crc);
			e->rv = -EIO;
			goto done;
		}
	}

	total = hdr->total_data_extents;
	e->total_data_extents = total;
	e->allocator_generation = hdr->allocator_generation;

	N_If(cdv_async_scan_start,
	     "CDV-alloc: async scanning cdv=@STR total_data_extents=@LLU",
	     e->cdv_uuid, total);

	/* Pre-allocate a reasonable results array; grow if needed. */
	results_cap = 64;
	results = NNVMEIBT_BM_CALLOC(cdv_async_scan_results_alloc,
				      results_cap * sizeof(*results));
	if (!results) {
		e->rv = -ENOMEM;
		goto done;
	}

	for (i = 1; i <= total; i++) {
		uint64_t off = cdv_ondisk_record_offset(i);
		uint32_t expected_crc;

		if (NNVMEIBT_PREAD(cdv_async_scan_rec_rd, fd, rec, CDV_ONDISK_BLOCK_SIZE,
				   off, 1) < 0) {
			N_Wf(cdv_async_scan_rec_err,
			     "CDV-alloc: async scan cdv=@STR read failed at idx=@LLU; aborting, will retry",
			     e->cdv_uuid, i);
			e->rv = -EIO;
			goto done;
		}

		if (!(rec->flags & CDV_ONDISK_RECORD_FLAG_ALLOCATED))
			continue;

		expected_crc = crc32_seedless(rec,
			offsetof(struct cdv_alloc_ondisk_record, crc32));
		if (rec->crc32 != expected_crc) {
			N_Wf(cdv_async_scan_rec_crc,
			     "CDV-alloc: async scan cdv=@STR idx=@LLU CRC bad; skipping",
			     e->cdv_uuid, i);
			continue;
		}

		/* Grow results array if full. */
		if (n_results >= results_cap) {
			uint64_t new_cap = results_cap * 2;
			struct cdv_scan_result_entry *tmp;

			tmp = NNVMEIBT_BM_CALLOC(cdv_async_scan_results_grow,
						  new_cap * sizeof(*tmp));
			if (!tmp) {
				e->rv = -ENOMEM;
				goto done;
			}
			memcpy(tmp, results, n_results * sizeof(*results));
			NNVMEIBT_BM_FREE(cdv_async_scan_results_old, results);
			results = tmp;
			results_cap = new_cap;
		}

		results[n_results].extent_index = i;
		rec->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
		strncpy(results[n_results].tpv_uuid, rec->tpv_uuid,
			NVMEIBT_CDV_UUID_STRLEN);
		if (rec->flags & CDV_ONDISK_RECORD_FLAG_NEEDS_ZEROING) {
			results[n_results].needs_zeroing = true;
			results[n_results].zeroing_allocator_size_gib  = rec->zeroing_allocator_size_gib;
			results[n_results].zeroing_cdv_extent_size_mib = rec->zeroing_cdv_extent_size_mib;
		}
		n_results++;

		memset(rec, 0, CDV_ONDISK_BLOCK_SIZE);
	}

	e->results = results;
	e->n_results = n_results;
	results = NULL; /* ownership transferred */
	e->rv = 0;

	N_If(cdv_async_scan_done,
	     "CDV-alloc: async scan cdv=@STR read @LLU extents from @LLU slots",
	     e->cdv_uuid, n_results, total);

done:
	if (results) NNVMEIBT_BM_FREE(cdv_async_scan_results_free, results);
	if (hdr) NNVMEIBT_BM_FREE(cdv_async_scan_hdr_free, hdr);
	if (rec) NNVMEIBT_BM_FREE(cdv_async_scan_rec_free, rec);
	/* fd is cached in alloc->cdv_fd — do NOT close here. */

	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ,
				    &e->wq_entry);
}

static void cdv_ondisk_scan_async(const char *cdv_uuid,
				  struct nvmeibt_cdv_alloc *alloc);
static void cdv_dispatch_zero_extent(struct nvmeibt_cdv_alloc *alloc,
				     uint64_t extent_index,
				     const char *tpv_uuid,
				     uint32_t allocator_size_gib,
				     uint32_t cdv_extent_size_mib);

/*
 * cdv_scan_finalize — main thread: apply scan results to the allocator.
 *
 * Runs in TOMA's main thread after the worker completes.  All shared-state
 * mutations happen here (hash lookups, add_extent, flag updates).
 */
/* Close whichever fd was used by the worker (satellite first; legacy CDV).
 * Used by the scan-retry paths to force a fresh open on the next attempt
 * (the device may have come online since the previous open). */
static void cdv_close_worker_fds(struct nvmeibt_cdv_alloc *alloc)
{
	if (alloc->satellite_fd >= 0) {
		NNVMEIBT_CLOSE(cdv_close_worker_sat, alloc->satellite_fd);
		alloc->satellite_fd = -1;
	}
	if (alloc->cdv_fd >= 0) {
		NNVMEIBT_CLOSE(cdv_close_worker_cdv, alloc->cdv_fd);
		alloc->cdv_fd = -1;
	}
}

static void cdv_scan_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_ondisk_scan_wq_entry *e =
		container_of(wq_entry, struct cdv_ondisk_scan_wq_entry, wq_entry);
	struct nvmeibt_cdv_alloc *alloc;
	uint64_t i, n_loaded = 0;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, e->cdv_uuid);
	if (!alloc) {
		/* Allocator was removed while scan was in flight (CDV deleted). */
		N_Wf(cdv_scan_fin_no_alloc,
		     "CDV-alloc: scan finalize cdv=@STR allocator gone; discarding results",
		     e->cdv_uuid);
		goto out;
	}

	alloc->scan_in_progress = false;

	if (wq_entry->is_canceled) {
		N_Wf(cdv_scan_fin_canceled,
		     "CDV-alloc: scan finalize cdv=@STR canceled (shutdown?)",
		     e->cdv_uuid);
		goto out;
	}

	if (e->rv < 0) {
		/*
		 * The scan worker failed (pread returned 0 bytes or I/O error).
		 * This commonly happens when the CDV NVMesh block device is not
		 * yet fully online — the device node exists but returns no data.
		 *
		 * Close the cached fd so the next scan attempt reopens the
		 * device (it may have come online since the fd was first opened).
		 * Leave ondisk_loaded=false — the next client request will
		 * trigger another scan attempt.  Do NOT accept as fresh: the
		 * CDV may have existing data that would be lost.
		 */
		cdv_close_worker_fds(alloc);
		N_Wf(cdv_scan_fin_err,
		     "CDV-alloc: scan finalize cdv=@STR worker failed rv=@INT; closed fd, will retry",
		     e->cdv_uuid, e->rv);
		goto out;
	}

	if (e->is_fresh) {
		/*
		 * The scan found no valid CDV header (magic mismatch).  This is
		 * expected for a genuinely new CDV that has never had an extent
		 * allocated.  However, during a simultaneous client+TOMA restart
		 * the CDV NVMesh block device may not be fully online yet, causing
		 * the pread to return zeros — making an existing CDV look "fresh".
		 *
		 * Heuristic: if the first scan after (re)creating the allocator
		 * reports "fresh", we don't know whether the CDV is truly new or
		 * was unreadable.  Close the cached fd (it may point to a
		 * not-yet-connected NVMesh device) and leave ondisk_loaded=false
		 * to force a retry.  On the second consecutive "fresh" result,
		 * accept it — the CDV is genuinely new.
		 */
		if (!alloc->scan_fresh_seen_once) {
			alloc->scan_fresh_seen_once = true;
			/* Close cached fds so the retry reopens the (hopefully now
			 * online) NVMesh device with a fresh file descriptor. */
			cdv_close_worker_fds(alloc);
			N_Wf(cdv_scan_fin_fresh_retry,
			     "CDV-alloc: scan finalize cdv=@STR scan says fresh (first attempt); "
			     "will retry once to rule out CDV-not-yet-online race",
			     e->cdv_uuid);
			goto out;
		}
		alloc->ondisk_loaded = true;
		N_If(cdv_scan_fin_fresh,
		     "CDV-alloc: scan finalize cdv=@STR fresh CDV (no header, confirmed on retry)",
		     e->cdv_uuid);
		goto out;
	}

	/* Apply header fields. */
	if (alloc->total_data_extents == 0 && e->total_data_extents > 0)
		alloc->total_data_extents = e->total_data_extents;

	if (alloc->allocator_generation == 0 && e->allocator_generation > 0)
		alloc->allocator_generation = e->allocator_generation;

	/*
	 * Sanity check: if the scan found a valid header but 0 extent records,
	 * yet we already have in-memory extents (from a prior TOMA lifetime),
	 * the CDV was likely still reconnecting during the scan and returned
	 * stale/zero data for the per-extent region.  Reject and retry.
	 */
	if (e->n_results == 0 && alloc->n_allocated > 0) {
		cdv_close_worker_fds(alloc);
		N_Wf(cdv_scan_fin_zero_suspect,
		     "CDV-alloc: scan finalize cdv=@STR scan found 0 extents on-disk but @LLU in-memory; "
		     "CDV likely not fully online yet - will retry on next request",
		     e->cdv_uuid, alloc->n_allocated);
		goto out;
	}

	/* Apply extent entries — add_extent deduplicates. */
	for (i = 0; i < e->n_results; i++) {
		struct cdv_scan_result_entry *r = &e->results[i];

		if (nvmeibt_cdv_alloc_add_extent(e->cdv_uuid,
						 r->extent_index,
						 r->tpv_uuid) < 0) {
			N_Ef(cdv_scan_fin_add_fail,
			     "CDV-alloc: scan finalize cdv=@STR idx=@LLU add_extent failed",
			     e->cdv_uuid, r->extent_index);
			continue;
		}

		n_loaded++;

		if (r->needs_zeroing) {
			struct nvmeibt_cdv_extent_entry *ext;

			XDLIST_FOREACH(ext, &alloc->extents) {
				if (ext->extent_index == r->extent_index) {
					ext->needs_zeroing = true;
					alloc->n_pending_zeroing++;
					if (r->zeroing_allocator_size_gib && r->zeroing_cdv_extent_size_mib) {
						if (alloc->allocator_size_gib == 0)
							alloc->allocator_size_gib = r->zeroing_allocator_size_gib;
						if (alloc->cdv_extent_size_mib == 0)
							alloc->cdv_extent_size_mib = r->zeroing_cdv_extent_size_mib;
						cdv_dispatch_zero_extent(alloc, r->extent_index,
									 r->tpv_uuid,
									 r->zeroing_allocator_size_gib,
									 r->zeroing_cdv_extent_size_mib);
					} else {
						N_Wf(cdv_scan_fin_zero_no_geom,
						     "CDV-alloc: scan cdv=@STR idx=@LLU NEEDS_ZEROING "
						     "but geometry missing; will zero on next free_all",
						     e->cdv_uuid, r->extent_index);
					}
					break;
				}
			}
		}
	}

	alloc->ondisk_loaded = true;
	alloc->scan_fresh_seen_once = false;  /* successful load; reset for future re-scans */

	N_If(cdv_scan_fin_done,
	     "CDV-alloc: scan finalize cdv=@STR loaded @LLU extents",
	     e->cdv_uuid, n_loaded);

out:
	/*
	 * If this TOMA is the elected allocator but the scan has not yet
	 * succeeded, self-reschedule with backoff (100ms → 1000ms).  Otherwise
	 * the on-disk load depends on an external trigger (client request or
	 * topology recalc) that may never arrive — e.g. for a fresh CDV whose
	 * block device is still coming online.
	 */
	if (alloc) {
		if (alloc->ondisk_loaded) {
			alloc->scan_retry_delay_ms = 0;
			/*
			 * Promote to ACTIVE if the scan succeeded while we
			 * were waiting for the satellite to be open (Stage B
			 * of the satellite-attach handshake).  This unblocks
			 * handle_cdv_alloc_extent from returning WRONG_GEN.
			 * Placed here (rather than inline on the non-fresh
			 * path) so fresh-CDV scans also promote.
			 */
			if (alloc->state == NVMEIBT_CDV_ALLOC_STATE_AWAITING_SATELLITE_ATTACH) {
				alloc->state = NVMEIBT_CDV_ALLOC_STATE_ACTIVE;
				N_If(cdv_alloc_state_active,
				     "CDV-alloc: state->ACTIVE cdv=@STR gen=@LLU",
				     e->cdv_uuid, alloc->allocator_generation);
			}
		} else {
			const char *me = nvmeibt_get_my_hostname();
			bool am_allocator = me && me[0] &&
				strncmp(alloc->allocator_toma_id, me,
					NVMEIBT_CDV_HOSTNAME_LEN) == 0;

			if (am_allocator) {
				uint32_t prev = alloc->scan_retry_delay_ms;
				uint32_t next = prev ? prev * 2 : 100;

				if (next > 1000)
					next = 1000;
				alloc->scan_retry_delay_ms = next;
				N_Wf(cdv_scan_retry_backoff,
				     "CDV-alloc: scan cdv=@STR not loaded; retrying in @INT ms",
				     e->cdv_uuid, (int)next);
				cdv_ondisk_scan_async(e->cdv_uuid, alloc);
			}
		}
	}
	return; /* free callback handles memory */
}

/*
 * cdv_scan_free — release the WQ entry and its results array.
 */
static void cdv_scan_free(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_ondisk_scan_wq_entry *e =
		container_of(wq_entry, struct cdv_ondisk_scan_wq_entry, wq_entry);

	if (e->results)
		NNVMEIBT_BM_FREE(cdv_scan_wqe_results_free, e->results);
	NNVMEIBT_BM_FREE(cdv_scan_wqe_free, e);
}

/*
 * cdv_ondisk_scan_async — dispatch an asynchronous CDV scan to the per-CDV WQ.
 *
 * If a scan is already in progress for this allocator, does nothing.
 * Sets alloc->scan_in_progress to prevent duplicate dispatches.
 * Callers must check !alloc->ondisk_loaded after this returns and handle
 * the "not ready yet" case (typically return -EAGAIN or defer).
 */
static void cdv_ondisk_scan_async(const char *cdv_uuid,
				  struct nvmeibt_cdv_alloc *alloc)
{
	struct cdv_ondisk_scan_wq_entry *e;

	if (alloc->ondisk_loaded || alloc->scan_in_progress)
		return;

	if (cdv_ensure_io_wq(cdv_uuid, alloc) < 0) {
		N_Wf(cdv_scan_no_wq,
		     "CDV-alloc: scan cdv=@STR I/O WQ not ready; will retry", cdv_uuid);
		return;
	}

	e = NNVMEIBT_BM_CALLOC(cdv_scan_wqe_alloc, sizeof(*e));
	if (!e) {
		N_Ef(cdv_scan_wqe_oom,
		     "CDV-alloc: scan cdv=@STR WQ entry alloc failed", cdv_uuid);
		return;
	}

	e->wq_entry.type     = "CDV_ONDISK_SCAN";
	e->wq_entry.execute  = cdv_scan_execute;
	e->wq_entry.finalize = cdv_scan_finalize;
	e->wq_entry.abort    = nvmeibt_toma_wakeup_wq_abort_func;
	e->wq_entry.free     = cdv_scan_free;

	e->alloc = alloc;
	strncpy(e->cdv_uuid, cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	e->cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	e->pre_sleep_ms = alloc->scan_retry_delay_ms;

	alloc->scan_in_progress = true;
	nvmeibt_wq_addw(alloc->io_wq, &e->wq_entry);

	N_If(cdv_scan_dispatched,
	     "CDV-alloc: scan cdv=@STR dispatched to I/O WQ (path=@STR)",
	     cdv_uuid, alloc->dev_path);
}

/* ── Async CDV writes (work-queue based) ───────────────────────────────────
 *
 * CDV record and header writes are dispatched to the per-CDV I/O WQ so they
 * don't block the main thread.  The main thread prepares the 4 KiB write
 * buffer (including CRC), hands it to the worker, and proceeds immediately.
 *
 * Two flavors:
 *   - Fire-and-forget: for FREE, FREE_ALL, election header.  The finalize
 *     callback logs errors but takes no corrective action.
 *   - Persist-then-respond: for ALLOC.  The response to the client is sent
 *     from the finalize callback ONLY after the worker confirms the pwrite
 *     succeeded.  This guarantees the allocation record is on disk before the
 *     client acts on it (crash safety).
 */

/* WQ entry for a single CDV write (record or header). */
struct cdv_write_wq_entry {
	struct nvmeibt_wq_entry   wq_entry;
	struct nvmeibt_cdv_alloc *alloc;	/* for cached fd; valid: we drain before remove */
	void    *buf;				/* page-aligned 4 KiB, prepared by main thread */
	uint64_t offset;			/* CDV byte offset */
	int      rv;				/* result from pwrite */
};

static void cdv_write_execute(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_write_wq_entry *e =
		container_of(wq_entry, struct cdv_write_wq_entry, wq_entry);
	int fd;

	e->rv = 0;
	fd = cdv_worker_open_fd(e->alloc);
	if (fd < 0) {
		e->rv = -ENODEV;
	} else {
		ssize_t wr = NNVMEIBT_PWRITE(cdv_wq_wr, fd, e->buf,
					      CDV_ONDISK_BLOCK_SIZE,
					      e->offset, 0ULL);
		if (wr < 0)
			e->rv = -EIO;
	}

	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, &e->wq_entry);
}

static void cdv_write_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_write_wq_entry *e =
		container_of(wq_entry, struct cdv_write_wq_entry, wq_entry);

	if (wq_entry->is_canceled || e->rv < 0)
		N_Wf(cdv_write_fin_err,
		     "CDV-alloc: async write offset=@LLU rv=@INT canceled=@INT",
		     e->offset, e->rv, wq_entry->is_canceled);
}

static void cdv_write_free(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_write_wq_entry *e =
		container_of(wq_entry, struct cdv_write_wq_entry, wq_entry);

	if (e->buf)
		NNVMEIBT_BM_FREE(cdv_write_buf_free, e->buf);
	NNVMEIBT_BM_FREE(cdv_write_wqe_free, e);
}

/*
 * cdv_dispatch_write — enqueue a single 4 KiB write to the per-CDV I/O WQ.
 *
 * @alloc:   per-CDV allocator (must have io_wq set up)
 * @buf:     page-aligned 4 KiB buffer; ownership transfers to the WQ entry
 * @offset:  CDV byte offset to write at
 *
 * The caller must NOT free @buf after calling this; it is freed by the WQ.
 */
static void cdv_dispatch_write(struct nvmeibt_cdv_alloc *alloc,
			       void *buf, uint64_t offset)
{
	struct cdv_write_wq_entry *e;

	if (!alloc->io_wq) {
		/* I/O WQ not ready — discard write (best-effort). */
		NNVMEIBT_BM_FREE(cdv_dispatch_wr_no_wq, buf);
		return;
	}

	e = NNVMEIBT_BM_CALLOC(cdv_write_wqe_alloc, sizeof(*e));
	if (!e) {
		NNVMEIBT_BM_FREE(cdv_dispatch_wr_oom, buf);
		return;
	}

	e->wq_entry.type     = "CDV_WRITE";
	e->wq_entry.execute  = cdv_write_execute;
	e->wq_entry.finalize = cdv_write_finalize;
	e->wq_entry.abort    = nvmeibt_toma_wakeup_wq_abort_func;
	e->wq_entry.free     = cdv_write_free;

	e->alloc  = alloc;
	e->buf    = buf;
	e->offset = offset;

	nvmeibt_wq_addw(alloc->io_wq, &e->wq_entry);
}

/*
 * cdv_async_write_record — prepare a record buffer and dispatch to the I/O WQ.
 *
 * Called from the main thread after in-memory state has been updated.
 * If tpv_uuid is NULL, the record is written as free (zeroed flags).
 */
static void cdv_async_write_record(struct nvmeibt_cdv_alloc *alloc,
				   uint64_t extent_index,
				   const char *tpv_uuid)
{
	struct cdv_alloc_ondisk_record *rec;

	rec = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_async_wr_rec_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!rec)
		return;

	if (tpv_uuid) {
		rec->flags = CDV_ONDISK_RECORD_FLAG_ALLOCATED;
		strncpy(rec->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
		rec->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	}
	rec->crc32 = crc32_seedless(rec,
		offsetof(struct cdv_alloc_ondisk_record, crc32));

	cdv_dispatch_write(alloc, rec, cdv_ondisk_record_offset(extent_index));
}

/*
 * cdv_async_write_header — prepare a header buffer and dispatch to the I/O WQ.
 *
 * Snapshots the current allocator state into the header buffer on the main
 * thread, then hands it to the worker for persistence.
 */
static void cdv_async_write_header(struct nvmeibt_cdv_alloc *alloc)
{
	struct cdv_alloc_ondisk_header *hdr;

	hdr = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_async_wr_hdr_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!hdr)
		return;

	hdr->magic   = CDV_ONDISK_MAGIC;
	hdr->version = CDV_ONDISK_VERSION;
	hdr->total_data_extents   = alloc->total_data_extents;
	hdr->allocator_generation = alloc->allocator_generation;
	strncpy(hdr->allocator_toma_id, alloc->allocator_toma_id,
		NVMEIBT_CDV_UUID_STRLEN - 1);
	hdr->crc32 = crc32_seedless(hdr,
		offsetof(struct cdv_alloc_ondisk_header, crc32));

	cdv_dispatch_write(alloc, hdr, 0ULL);
}

/*
 * cdv_data_extent_offset — CDV byte offset of data extent @extent_index.
 *
 * Data extents are 1-based (index 0 is unused so the on-satellite record at
 * offset 0 can remain the header — see cdv_ondisk_record_offset).
 * Post-satellite-migration the allocator metadata lives on the <cdv>-mgmt
 * satellite, so the entire CDV is data:
 *   Byte offset = (extent_index - 1) * cdv_extent_size_mib * 1 MiB
 * @allocator_size_gib is retained in the signature for call-site compatibility
 * but is no longer consulted.
 */
static inline uint64_t cdv_data_extent_offset(uint64_t extent_index,
					       uint32_t allocator_size_gib,
					       uint32_t cdv_extent_size_mib)
{
	(void)allocator_size_gib;
	return (extent_index - 1) * (uint64_t)cdv_extent_size_mib * (1ULL << 20);
}

/*
 * cdv_async_write_record_needs_zeroing — write an ALLOCATED|NEEDS_ZEROING record
 * carrying geometry in the reserved2 extension fields (not CRC-covered).
 */
static void cdv_async_write_record_needs_zeroing(struct nvmeibt_cdv_alloc *alloc,
						  uint64_t extent_index,
						  const char *tpv_uuid,
						  uint32_t allocator_size_gib,
						  uint32_t cdv_extent_size_mib)
{
	struct cdv_alloc_ondisk_record *rec;

	rec = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_async_wr_nz_rec_alloc,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!rec)
		return;

	rec->flags = CDV_ONDISK_RECORD_FLAG_ALLOCATED | CDV_ONDISK_RECORD_FLAG_NEEDS_ZEROING;
	strncpy(rec->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	rec->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	rec->crc32 = crc32_seedless(rec,
		offsetof(struct cdv_alloc_ondisk_record, crc32));
	/* Geometry fields are after crc32, not CRC-covered. */
	rec->zeroing_allocator_size_gib  = allocator_size_gib;
	rec->zeroing_cdv_extent_size_mib = cdv_extent_size_mib;

	cdv_dispatch_write(alloc, rec, cdv_ondisk_record_offset(extent_index));
}

/* ── Background CDV data-extent zero (work-queue based) ────────────────────
 *
 * When a TPV is deleted, all CDV data extents it owned are zeroed in the
 * background before being made available for a new TPV.  This prevents data
 * leakage between TPVs sharing the same CDV.
 *
 * The zero WQ entry writes the full CDV extent (cdv_extent_size_mib MiB) in
 * 1 MiB chunks using an aligned calloc buffer.  On success the finalize
 * callback writes a free ondisk record and removes the extent from the
 * in-memory list.  On failure the entry is left in place so the next
 * free_all_for_tpv call (or scan after restart) will retry.
 */

struct cdv_zero_wq_entry {
	struct nvmeibt_wq_entry   wq_entry;
	struct nvmeibt_cdv_alloc *alloc;

	/* Inputs set by dispatcher (main thread). */
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	uint64_t extent_index;
	uint64_t data_offset;		/* CDV byte offset of the data extent */
	uint32_t extent_size_mb;	/* size of this data extent in MiB */

	/* Output set by worker. */
	int      rv;
};

static void cdv_zero_execute(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_zero_wq_entry *e =
		container_of(wq_entry, struct cdv_zero_wq_entry, wq_entry);
	uint8_t *zbuf;
	uint32_t chunk_size = 1U << 20; /* 1 MiB */
	uint64_t written = 0;
	uint64_t total = (uint64_t)e->extent_size_mb << 20;
	int fd;

	/*
	 * DESIGN GAP — see ThinProvisioningImplementation.md §3.9 ("Design gap:
	 * zero-on-free is non-functional after the satellite-volume migration").
	 *
	 * After the satellite-volume migration the CDV is no longer auto-attached
	 * to the allocator TOMA, so cdv_worker_open_cdv_fd_for_zeroing() will
	 * almost certainly return -ENODEV.  cdv_extent_zero_on_free is off by
	 * default, so this path is normally not entered.  If it IS entered,
	 * surface a single loud warning so operators can see the behaviour in
	 * traces; the freed extent will remain in NEEDS_ZEROING state and is not
	 * reused (capacity loss but no correctness issue).
	 */
	{
		static bool warned_once_per_process = false;
		if (!warned_once_per_process) {
			warned_once_per_process = true;
			N_Ef(cdv_zero_post_migration_warn,
			     "CDV-zero: invoked but CDV is no longer auto-attached to TOMA "
			     "after the satellite-volume migration; freed extents will stay "
			     "in NEEDS_ZEROING and not be reused.  See ThinProvisioning s3.9.  "
			     "First occurrence cdv=@STR idx=@LLU",
			     e->cdv_uuid, e->extent_index);
		}
	}

	e->rv = 0;
	/* Zero-on-free targets CDV data-extent offsets — must use the CDV fd,
	 * NOT the satellite fd.  See cdv_worker_open_cdv_fd_for_zeroing doc. */
	fd = cdv_worker_open_cdv_fd_for_zeroing(e->alloc);
	if (fd < 0) {
		e->rv = -ENODEV;
		goto done;
	}

	zbuf = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_zero_buf_alloc, PAGE_SIZE, chunk_size);
	if (!zbuf) {
		e->rv = -ENOMEM;
		goto done;
	}

	while (written < total) {
		uint64_t remaining = total - written;
		uint32_t this_chunk = (remaining < chunk_size) ? (uint32_t)remaining : chunk_size;

		if (NNVMEIBT_PWRITE(cdv_zero_wr, fd, zbuf, this_chunk,
				    e->data_offset + written, 0ULL) < 0) {
			N_Ef(cdv_zero_wr_error,
			     "CDV-zero: write failed cdv=@STR idx=@LLU offset=@LLU",
			     e->cdv_uuid, e->extent_index, e->data_offset + written);
			e->rv = -EIO;
			break;
		}
		written += this_chunk;
	}

	NNVMEIBT_BM_FREE(cdv_zero_buf_free, zbuf);

done:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, &e->wq_entry);
}

static void cdv_zero_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_zero_wq_entry *e =
		container_of(wq_entry, struct cdv_zero_wq_entry, wq_entry);
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;

	if (wq_entry->is_canceled) {
		N_Wf(cdv_zero_fin_canceled,
		     "CDV-zero: cdv=@STR idx=@LLU canceled (shutdown)",
		     e->cdv_uuid, e->extent_index);
		return;
	}

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, e->cdv_uuid);
	if (!alloc) {
		N_Wf(cdv_zero_fin_no_alloc,
		     "CDV-zero: cdv=@STR idx=@LLU allocator gone; discarding",
		     e->cdv_uuid, e->extent_index);
		return;
	}

	if (e->rv < 0) {
		N_Ef(cdv_zero_fin_err,
		     "CDV-zero: cdv=@STR idx=@LLU zero failed rv=@INT; "
		     "extent stays NEEDS_ZEROING, will retry on next restart",
		     e->cdv_uuid, e->extent_index, e->rv);
		return;
	}

	/* Zero succeeded: write a free ondisk record, then remove entry. */
	cdv_async_write_record(alloc, e->extent_index, NULL);

	XDLIST_FOREACH_SAFE(entry, &alloc->extents) {
		if (entry->extent_index != e->extent_index)
			continue;
		if (strncmp(entry->tpv_uuid, e->tpv_uuid, NVMEIBT_CDV_UUID_STRLEN) != 0)
			continue;

		XDLIST_ELEM_DEL(&alloc->extents, entry);
		alloc->n_allocated--;
		if (alloc->n_pending_zeroing > 0)
			alloc->n_pending_zeroing--;
		NNVMEIBT_BM_FREE(cdv_zero_fin_entry, entry);
		break;
	}

	cdv_async_write_header(alloc);
	cdv_maybe_warn_capacity(alloc);
	cdv_publish_alloc_stats(alloc);

	N_If(cdv_zero_fin_ok,
	     "CDV-zero: cdv=@STR idx=@LLU zeroed and freed; remaining pending=@LLU",
	     e->cdv_uuid, e->extent_index, alloc->n_pending_zeroing);
}

static void cdv_zero_free(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_zero_wq_entry *e =
		container_of(wq_entry, struct cdv_zero_wq_entry, wq_entry);

	NNVMEIBT_BM_FREE(cdv_zero_wqe_free, e);
}

/*
 * cdv_dispatch_zero_extent — enqueue a background data-extent zero on the per-CDV WQ.
 */
static void cdv_dispatch_zero_extent(struct nvmeibt_cdv_alloc *alloc,
				     uint64_t extent_index,
				     const char *tpv_uuid,
				     uint32_t allocator_size_gib,
				     uint32_t cdv_extent_size_mib)
{
	struct cdv_zero_wq_entry *e;

	if (!alloc->io_wq)
		return;

	e = NNVMEIBT_BM_CALLOC(cdv_zero_wqe_alloc, sizeof(*e));
	if (!e) {
		N_Ef(cdv_zero_wqe_oom,
		     "CDV-zero: cdv=@STR idx=@LLU WQ entry alloc failed",
		     alloc->cdv_uuid, extent_index);
		return;
	}

	e->wq_entry.type     = "CDV_ZERO_EXTENT";
	e->wq_entry.execute  = cdv_zero_execute;
	e->wq_entry.finalize = cdv_zero_finalize;
	e->wq_entry.abort    = nvmeibt_toma_wakeup_wq_abort_func;
	e->wq_entry.free     = cdv_zero_free;

	e->alloc          = alloc;
	e->extent_index   = extent_index;
	e->data_offset    = cdv_data_extent_offset(extent_index, allocator_size_gib, cdv_extent_size_mib);
	e->extent_size_mb = cdv_extent_size_mib;

	strncpy(e->cdv_uuid, alloc->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	e->cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	strncpy(e->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	e->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';

	nvmeibt_wq_addw(alloc->io_wq, &e->wq_entry);

	N_If(cdv_zero_dispatched,
	     "CDV-zero: queued cdv=@STR idx=@LLU offset=@LLU size_mb=@UINT",
	     alloc->cdv_uuid, extent_index, e->data_offset, cdv_extent_size_mib);
}

/* Forward declaration — defined in the incoming-message handler section. */
static int cdv_send_response(struct nvmeibt_registrant_ctx *reg_ctx,
			     enum NVMEIBT_CLIENT_MSG_TYPES msg_type,
			     int data_length, void *data);

/* ── Persist-then-respond: ALLOC write with deferred client response ────── */

/*
 * WQ entry for the ALLOC path: writes record + header on the worker thread,
 * then sends the ALLOC response to the client from the finalize callback
 * (main thread) ONLY after the on-disk record is confirmed written.
 *
 * This guarantees crash safety: if TOMA dies between alloc and response, the
 * client never learns about the extent.  If TOMA dies after the response,
 * the record is already on disk and will be found on scan.
 */
struct cdv_alloc_persist_wq_entry {
	struct nvmeibt_wq_entry          wq_entry;
	struct nvmeibt_cdv_alloc        *alloc;

	/* Buffers prepared by main thread, written by worker. */
	void    *record_buf;
	uint64_t record_offset;
	void    *header_buf;

	/* Response to send from finalize after write completes. */
	struct nvmeibt_registrant_ctx    reg_ctx;	/* copy — msg may be freed */
	struct nvmeibt_cdv_alloc_resp    resp;

	int      write_rv;				/* result from pwrite */
};

static void cdv_alloc_persist_execute(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_alloc_persist_wq_entry *e =
		container_of(wq_entry, struct cdv_alloc_persist_wq_entry, wq_entry);
	int fd;

	e->write_rv = 0;
	fd = cdv_worker_open_fd(e->alloc);
	if (fd < 0) {
		e->write_rv = -ENODEV;
		goto done;
	}

	/* Write the allocation record — this is the critical write. */
	if (NNVMEIBT_PWRITE(cdv_alloc_persist_rec_wr, fd, e->record_buf,
			    CDV_ONDISK_BLOCK_SIZE, e->record_offset, 0ULL) < 0) {
		e->write_rv = -EIO;
		goto done;
	}

	/* Write the updated header (best-effort; record write is the important one). */
	if (e->header_buf) {
		if (NNVMEIBT_PWRITE(cdv_alloc_persist_hdr_wr, fd, e->header_buf,
				    CDV_ONDISK_BLOCK_SIZE, 0ULL, 0ULL) < 0)
			N_Wf(cdv_alloc_persist_hdr_fail,
			     "CDV-alloc: ALLOC persist header write failed (record OK)");
	}

done:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, &e->wq_entry);
}

static void cdv_alloc_persist_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_alloc_persist_wq_entry *e =
		container_of(wq_entry, struct cdv_alloc_persist_wq_entry, wq_entry);

	if (wq_entry->is_canceled || e->write_rv < 0) {
		N_Ef(cdv_alloc_persist_fin_err,
		     "CDV-alloc: ALLOC persist failed rv=@INT canceled=@INT; sending ERROR to client",
		     e->write_rv, wq_entry->is_canceled);
		e->resp.status = NVMEIBT_CDV_ALLOC_ERROR;
	}

	/* Send the response to the client — on success the record is on disk. */
	cdv_send_response(&e->reg_ctx,
			  NVMEIBT_CLIENT_MSG_TR_CDV_ALLOC_EXTENT_RSP,
			  sizeof(e->resp), &e->resp);
}

static void cdv_alloc_persist_free(struct nvmeibt_wq_entry *wq_entry)
{
	struct cdv_alloc_persist_wq_entry *e =
		container_of(wq_entry, struct cdv_alloc_persist_wq_entry, wq_entry);

	if (e->record_buf)
		NNVMEIBT_BM_FREE(cdv_alloc_persist_rec_free, e->record_buf);
	if (e->header_buf)
		NNVMEIBT_BM_FREE(cdv_alloc_persist_hdr_free, e->header_buf);
	NNVMEIBT_BM_FREE(cdv_alloc_persist_wqe_free, e);
}

/*
 * cdv_dispatch_alloc_persist — persist an ALLOC record and defer the client
 * response until the write completes.
 *
 * Returns 0 if the work was successfully dispatched (response will be sent
 * from finalize).  Returns < 0 on dispatch failure (caller must send the
 * response itself).
 */
static int cdv_dispatch_alloc_persist(struct nvmeibt_cdv_alloc *alloc,
				      uint64_t extent_index,
				      const char *tpv_uuid,
				      const struct nvmeibt_registrant_ctx *reg_ctx,
				      const struct nvmeibt_cdv_alloc_resp *resp)
{
	struct cdv_alloc_persist_wq_entry *e;
	struct cdv_alloc_ondisk_record *rec;
	struct cdv_alloc_ondisk_header *hdr;

	if (!alloc->io_wq)
		return -ENODEV;

	e = NNVMEIBT_BM_CALLOC(cdv_alloc_persist_wqe_alloc, sizeof(*e));
	if (!e)
		return -ENOMEM;

	/* Prepare the record buffer. */
	rec = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_alloc_persist_rec_buf,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (!rec) {
		NNVMEIBT_BM_FREE(cdv_alloc_persist_wqe_oom, e);
		return -ENOMEM;
	}
	rec->flags = CDV_ONDISK_RECORD_FLAG_ALLOCATED;
	strncpy(rec->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	rec->tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	rec->crc32 = crc32_seedless(rec,
		offsetof(struct cdv_alloc_ondisk_record, crc32));

	/* Prepare the header buffer. */
	hdr = NNVMEIBT_BM_ALIGNED_CALLOC(cdv_alloc_persist_hdr_buf,
					  PAGE_SIZE, CDV_ONDISK_BLOCK_SIZE);
	if (hdr) {
		hdr->magic   = CDV_ONDISK_MAGIC;
		hdr->version = CDV_ONDISK_VERSION;
		hdr->total_data_extents   = alloc->total_data_extents;
		hdr->allocator_generation = alloc->allocator_generation;
		strncpy(hdr->allocator_toma_id, alloc->allocator_toma_id,
			NVMEIBT_CDV_UUID_STRLEN - 1);
		hdr->crc32 = crc32_seedless(hdr,
			offsetof(struct cdv_alloc_ondisk_header, crc32));
	}
	/* hdr alloc failure is non-fatal — header write is best-effort. */

	e->wq_entry.type     = "CDV_ALLOC_PERSIST";
	e->wq_entry.execute  = cdv_alloc_persist_execute;
	e->wq_entry.finalize = cdv_alloc_persist_finalize;
	e->wq_entry.abort    = nvmeibt_toma_wakeup_wq_abort_func;
	e->wq_entry.free     = cdv_alloc_persist_free;

	e->alloc         = alloc;
	e->record_buf    = rec;
	e->record_offset = cdv_ondisk_record_offset(extent_index);
	e->header_buf    = hdr;
	e->reg_ctx       = *reg_ctx;	/* struct copy */
	e->resp          = *resp;	/* struct copy */

	nvmeibt_wq_addw(alloc->io_wq, &e->wq_entry);
	return 0;
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
		alloc->cdv_fd       = -1;
		alloc->satellite_fd = -1;
		alloc->state        = NVMEIBT_CDV_ALLOC_STATE_NOT_ALLOCATOR;
		XDLIST_HEAD_INIT(&alloc->extents);
		alloc->n_allocated = 0;
		pthread_mutex_init(&alloc->handler_lock, NULL);
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

/*
 * ── Per-client CDV preempt (§2.10) ──────────────────────────────────────────
 */

struct nvmeibt_cdv_alloc *nvmeibt_cdv_alloc_lookup(const char *cdv_uuid)
{
	return nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
}

struct nvmeibt_cdv_alloc *nvmeibt_cdv_alloc_lookup_or_create_with_floor(
		const char *cdv_uuid, uint64_t initial_floor)
{
	struct nvmeibt_cdv_alloc *alloc;

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (alloc) {
		/* Seed floor if this is our first knowledge of the CDV. */
		if (!alloc->admission_floor_seeded) {
			alloc->admission_floor = initial_floor;
			alloc->admission_floor_seeded = true;
		} else {
			alloc->admission_floor = (initial_floor > alloc->admission_floor)
				? initial_floor : alloc->admission_floor;
		}
		return alloc;
	}

	alloc = NNVMEIBT_BM_CALLOC(cdv_alloc_preempt_create, sizeof(*alloc));
	if (!alloc) {
		N_Ef(cdv_alloc_preempt_oom,
		     "CDV-alloc: preempt create-on-the-fly OOM cdv=@STR", cdv_uuid);
		return NULL;
	}
	strncpy(alloc->cdv_uuid, cdv_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
	alloc->cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	alloc->cdv_fd       = -1;
	alloc->satellite_fd = -1;
	alloc->state        = NVMEIBT_CDV_ALLOC_STATE_NOT_ALLOCATOR;
	XDLIST_HEAD_INIT(&alloc->extents);
	pthread_mutex_init(&alloc->handler_lock, NULL);
	alloc->admission_floor        = initial_floor;
	alloc->admission_floor_seeded = true;
	nvmeib_hash_add_ascii_str(cdv_alloc_hash, alloc->cdv_uuid, alloc);
	N_If(cdv_alloc_preempt_created,
	     "CDV-alloc: created-on-preempt cdv=@STR floor=@LLU",
	     cdv_uuid, initial_floor);
	return alloc;
}

void nvmeibt_cdv_alloc_seed_floor(struct nvmeibt_cdv_alloc *alloc,
				   uint64_t floor, bool from_topology)
{
	if (!alloc)
		return;
	/*
	 * Monotonic max-merge. This is safe to call lock-free because the u64
	 * write is atomic on all supported architectures and readers do a
	 * single-value compare. The only correctness constraint is monotonicity,
	 * which max_t guarantees.
	 */
	if (!alloc->admission_floor_seeded) {
		alloc->admission_floor = floor;
		alloc->admission_floor_seeded = true;
		N_If(cdv_alloc_floor_seeded,
		     "CDV-alloc: floor seeded cdv=@STR floor=@LLU src=@STR",
		     alloc->cdv_uuid, floor,
		     from_topology ? "topology" : "attach");
	} else if (floor > alloc->admission_floor) {
		alloc->admission_floor = floor;
		N_If(cdv_alloc_floor_raised,
		     "CDV-alloc: floor raised cdv=@STR floor=@LLU src=@STR",
		     alloc->cdv_uuid, floor,
		     from_topology ? "topology" : "attach");
	}
}

/*
 * nvmeibt_cdv_alloc_preempt_client — raise the CDV's admission floor and
 * terminate the named client's reg_ctx on every local CDV segment.
 *
 * Ordering invariant (TPV_PerClientCDVPreemption.md §2.10.4):
 *   1. Raise admission_floor FIRST under handler_lock — a concurrent REGISTER
 *      predicate (check_cdv_admission_floor) that has already taken
 *      handler_lock will observe the new floor; the reverse order would let
 *      a retry REGISTER re-register at the old version in the window between
 *      the two steps.
 *   2. Walk every seg_active belonging to this CDV via
 *      nvmeibt_seg_active_get_cdv_alloc pointer-equality filter, and call
 *      nvmeibt_register_terminate_reg_ctx on each active registrant whose
 *      registrant_node_id.str matches client_id.
 *
 * Linear inner-walk is intentional: preempt is a control-plane event, not
 * I/O-path. A dedicated by-client hash index was considered and rejected in
 * §2.10.5 — the register/unregister maintenance cost isn't justified by the
 * preempt-path savings.
 */
int nvmeibt_cdv_alloc_preempt_client(const char *cdv_uuid,
				     const char *client_id,
				     uint64_t    new_floor,
				     uint32_t   *out_terminated)
{
	struct nvmeibt_cdv_alloc     *alloc;
	struct nvmeibt_local_disk    *local_disk;
	struct nvmeibt_seg_active    *seg_active;
	struct nvmeibt_registrant_ctx *reg_ctx;
	uint32_t                      terminated = 0;

	if (out_terminated)
		*out_terminated = 0;

	alloc = nvmeibt_cdv_alloc_lookup_or_create_with_floor(cdv_uuid, new_floor);
	if (!alloc)
		return -ENOMEM;

	pthread_mutex_lock(&alloc->handler_lock);

	/* Step 1: raise floor FIRST (order invariant, §2.10.4). */
	if (new_floor > alloc->admission_floor)
		alloc->admission_floor = new_floor;
	/* admission_floor_seeded is guaranteed true by lookup_or_create_with_floor. */

	/*
	 * Step 2: terminate this client's reg_ctx on every CDV segment served
	 * by this TOMA. Outer walk: every local_disk → every seg_active; filter
	 * to segments of the target CDV via the helper (pointer-equality against
	 * our alloc entry is correct — cdv_alloc_hash returns the same pointer
	 * for the same UUID key). Inner walk: linear scan of the seg's active
	 * registrants matching on registrant_node_id.str (hostname); a dedicated
	 * by-client hash was considered and rejected per §2.10.5 (cost-of-
	 * maintenance > benefit-on-this-rare-control-plane-path).
	 *
	 * NVMEIB_HASH_FOREACH is safe against in-iteration deletion (see the
	 * macro comment in common/nvmeib_hash.h). terminate_reg_ctx removes
	 * the entry from active_registrants_hash_by_handle, which is exactly
	 * what the macro is designed to tolerate.
	 */
	NVMEIB_HASH_FOREACH(local_disk,
			nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			if (nvmeibt_seg_active_get_cdv_alloc(seg_active) != alloc)
				continue;
			NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_handle) {
				if (strcmp(reg_ctx->registrant_node_id.str, client_id) != 0)
					continue;
				nvmeibt_register_terminate_reg_ctx(reg_ctx,
					/*is_deleting_seg_active=*/false,
					NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE,
					/*is_move_from_active_reg_hash_to_stale_reg_hash=*/false);
				terminated++;
			}
		}
	}

	if (out_terminated)
		*out_terminated = terminated;

	pthread_mutex_unlock(&alloc->handler_lock);

	N_If(cdv_alloc_preempt_done,
	     "CDV-alloc: preempt cdv=@STR client=@STR floor=@LLU terminated=@UINT",
	     cdv_uuid, client_id, alloc->admission_floor, terminated);
	return 0;
}

struct nvmeibt_cdv_alloc *nvmeibt_seg_active_get_cdv_alloc(
		struct nvmeibt_seg_active *seg_active)
{
	/*
	 * Navigate from the segment to its parent CDV via the applied topology:
	 *   seg_active
	 *     → applied_seg_lot              (nvmeibt_seg_active_get_applied_seg_lot)
	 *       → praid_lot                   (seg_lot->praid_lot)
	 *         → my_praid                  (praid_lot->my_praid)
	 *           → blkdev                  (nvmeibt_praid_get_blkdev; follows
	 *                                      praid_mgmt.its_chunk.its_block_device)
	 *             → from_config.is_cdv    (CDV predicate)
	 *             → urn_uuid.str          (CDV UUID — hash key in cdv_alloc_hash)
	 *
	 * Returns NULL for non-CDV segments or when any link in the chain is
	 * not yet established (e.g., topology not applied yet). A NULL return
	 * causes check_cdv_admission_floor to admit the REGISTER, matching
	 * pre-feature behavior — safe because non-CDV volumes have no floor
	 * semantics, and a segment without an applied topology can't have a
	 * live reg_ctx to terminate.
	 */
	struct nvmeibt_seg_lot       *seg_lot;
	struct nvmeibt_praid_lot     *praid_lot;
	struct nvmeibt_praid         *praid;
	struct nvmeibt_block_device  *blkdev;

	if (!seg_active)
		return NULL;

	seg_lot = nvmeibt_seg_active_get_applied_seg_lot(seg_active);
	if (!seg_lot)
		return NULL;

	praid_lot = seg_lot->praid_lot;
	if (!praid_lot)
		return NULL;

	praid = praid_lot->my_praid;
	if (!praid)
		return NULL;

	blkdev = nvmeibt_praid_get_blkdev(praid);
	if (!blkdev || !blkdev->from_config.is_cdv)
		return NULL;

	return nvmeibt_cdv_alloc_lookup(blkdev->urn_uuid.str);
}

void nvmeibt_cdv_alloc_send_preempt_response(const char *cdv_uuid,
					     const char *client_id,
					     uint64_t    new_floor,
					     bool        success,
					     uint32_t    terminated,
					     const char *error)
{
	struct nvmeibt_Str *json;

	json = NNVMEIBT_STR_ALLOC(cdv_preempt_resp_json_alloc);
	if (!json) {
		N_Ef(cdv_preempt_resp_oom,
		     "CDV-alloc: preemptClientFromCDVResponse OOM cdv=@STR", cdv_uuid);
		return;
	}

	nvmeibt_Str_sprintf(json,
		"{" KAFKA_PRODUCER_MSG_HEADER_FMT
		"\"payload\": {"
		"\"tomaID\": \"%s\", "
		"\"cdvUUID\": \"%s\", "
		"\"clientID\": \"%s\", "
		"\"newFloor\": %llu, "
		"\"success\": %s, "
		"\"terminatedRegistrants\": %u"
		"%s%s%s"
		"}}",
		KAFKA_PRODUCER_MSG_HEADER_VAR("preemptClientFromCDVResponse", 1),
		nvmeibt_get_my_hostname(),
		cdv_uuid,
		client_id ? client_id : "",
		(unsigned long long)new_floor,
		success ? "true" : "false",
		terminated,
		error ? ", \"error\": \"" : "",
		error ? error : "",
		error ? "\"" : "");

	N_If(cdv_preempt_resp_send,
	     "CDV-alloc: preempt response cdv=@STR client=@STR floor=@LLU ok=@INT term=@UINT",
	     cdv_uuid, client_id ? client_id : "", new_floor, (int)success, terminated);

	nvmeibt_kafka_outgoing_msgs_queue_add(
		cdv_uuid,
		nvmeibt_Str_str(json),
		nvmeibt_Str_strlen(json) + 1,
		NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);

	NNVMEIBT_STR_FREE(cdv_preempt_resp_json_free, json);
}

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
static void cdv_send_attach_satellite_request(struct nvmeibt_cdv_alloc *alloc);

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
		 * No in-memory allocator on this TOMA.  Under the RAFT-embedded
		 * allocator identity design, the alloc entry is created exclusively
		 * by nvmeibt_cdv_alloc_on_topo_applied once this TOMA has processed
		 * the committed topology naming an allocator for this CDV.  If no
		 * entry exists here, topology has not yet been applied (cold-start
		 * race with client requests, or this CDV is unknown to us).  Do
		 * NOT create a zero-initialized entry — that historically led to
		 * scan loops against an unattached CDV.  Return EAGAIN and let the
		 * client retry after the topology push arrives.
		 */
		N_Wf(cdv_list_no_alloc,
		     "CDV-alloc: list cdv=@STR tpv=@STR no alloc entry yet; returning EAGAIN",
		     cdv_uuid, tpv_uuid);
		return -EAGAIN;
	}

	if (!alloc->ondisk_loaded) {
		/*
		 * Alloc entry exists but the allocator metadata has not yet been
		 * loaded.  If we are the elected allocator stuck in
		 * AWAITING_SATELLITE_ATTACH (e.g. Stage B came back with a
		 * transient INTERNAL_ERR because the previous allocator's client
		 * hadn't released the satellite yet), opportunistically re-fire
		 * Stage A on this incoming client request so the handshake can
		 * make progress without waiting on another RAFT topology change.
		 * Management caches only OK / terminal responses, so repeated
		 * INTERNAL_ERR requests with the same requestId re-run the
		 * underlying attach.  Idempotent on the management side.
		 * Mirrors the re-fire in handle_cdv_alloc_extent().
		 */
		if (alloc->state == NVMEIBT_CDV_ALLOC_STATE_AWAITING_SATELLITE_ATTACH)
			cdv_send_attach_satellite_request(alloc);
		N_Wf(cdv_list_not_ready,
		     "CDV-alloc: list cdv=@STR tpv=@STR not yet loaded (state=@INT n_alloc=@LLU); returning EAGAIN",
		     cdv_uuid, tpv_uuid, (int)alloc->state, alloc->n_allocated);
		return -EAGAIN;
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

	/* Drain and destroy the per-CDV I/O WQ before freeing state.
	 * This ensures no worker thread is using alloc->cdv_fd.
	 */
	if (alloc->io_wq) {
		nvmeibt_wq_drain(alloc->io_wq);
		nvmeibt_wq_destroy(alloc->io_wq);
		alloc->io_wq = NULL;
	}

	/* Free all extent entries before removing the allocator itself. */
	while (!XDLIST_EMPTY(&alloc->extents)) {
		entry = XDLIST_FIRST(&alloc->extents);
		XDLIST_ELEM_DEL(&alloc->extents, entry);
		NNVMEIBT_BM_FREE(cdv_alloc_remove_entry, entry);
	}

	/* Close the cached CDV and satellite fds if open. */
	if (alloc->cdv_fd >= 0)
		NNVMEIBT_CLOSE(cdv_alloc_remove_close, alloc->cdv_fd);
	if (alloc->satellite_fd >= 0)
		NNVMEIBT_CLOSE(cdv_alloc_remove_sat_close, alloc->satellite_fd);

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
	alloc->cdv_fd       = -1;
	alloc->satellite_fd = -1;
	alloc->state        = NVMEIBT_CDV_ALLOC_STATE_NOT_ALLOCATOR;
	XDLIST_HEAD_INIT(&alloc->extents);
	pthread_mutex_init(&alloc->handler_lock, NULL);
	nvmeib_hash_add_ascii_str(cdv_alloc_hash, alloc->cdv_uuid, alloc);
	return alloc;
}

int nvmeibt_cdv_alloc_elect(const char *cdv_uuid,
			    const char **candidates,
			    int n_candidates,
			    struct nvmeibt_praid_topo_ctx *topo_ctx)
{
	const char *chosen;
	int i;

	if (!topo_ctx) {
		N_Ef(cdv_alloc_elect_null_topo,
		     "CDV-alloc: elect with NULL topo_ctx cdv=@STR", cdv_uuid);
		return -EINVAL;
	}
	if (n_candidates <= 0) {
		N_Ef(cdv_alloc_elect_no_cand,
		     "CDV-alloc: elect with 0 candidates cdv=@STR", cdv_uuid);
		return -EINVAL;
	}

	/* Sticky rule: keep the current allocator if it is still a candidate. */
	if (topo_ctx->allocator_toma_id[0]) {
		for (i = 0; i < n_candidates; i++) {
			if (strncmp(topo_ctx->allocator_toma_id, candidates[i],
				    sizeof(topo_ctx->allocator_toma_id)) == 0) {
				N_If(cdv_alloc_elect_sticky,
				     "CDV-alloc: elect sticky cdv=@STR allocator=@STR gen=@LLU",
				     cdv_uuid, topo_ctx->allocator_toma_id,
				     topo_ctx->allocator_generation);
				return 0;   /* 0 = sticky, no topo mutation */
			}
		}
	}

	/* Pick a candidate: single → use it; multiple → random. */
	if (n_candidates == 1) {
		chosen = candidates[0];
	} else {
		uint64_t seed = (uint64_t)nvmeib_public_rdtsc();
		chosen = candidates[(unsigned int)(seed % (unsigned int)n_candidates)];
	}

	N_If(cdv_alloc_elect_new,
	     "CDV-alloc: elect cdv=@STR old=@STR new=@STR gen @LLU -> @LLU",
	     cdv_uuid,
	     topo_ctx->allocator_toma_id[0] ? topo_ctx->allocator_toma_id : "(none)",
	     chosen,
	     topo_ctx->allocator_generation,
	     topo_ctx->allocator_generation + 1);

	/*
	 * Stage the new identity into the pRAID topo_ctx.  RAFT AppendEntries
	 * will deliver it to every follower; the topology-apply hook
	 * (nvmeibt_cdv_alloc_on_topo_applied) fires the local role transition
	 * on the chosen TOMA.  No in-memory cdv_alloc_hash writes happen here.
	 */
	strncpy(topo_ctx->allocator_toma_id, chosen,
		sizeof(topo_ctx->allocator_toma_id) - 1);
	topo_ctx->allocator_toma_id[sizeof(topo_ctx->allocator_toma_id) - 1] = '\0';
	topo_ctx->allocator_generation++;

	return 1;   /* 1 = newly elected — caller marks topo as changed */
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

/*
 * cdv_send_attach_satellite_request — Stage A of the satellite attach handshake.
 *
 * Publishes an `attachSatelliteRequest` Kafka message asking management to attach
 * the CDV's satellite volume to this TOMA in EXCLUSIVE_READ_WRITE mode (with
 * preempt+isDetachOthers).  Management replies with `attachSatelliteResponse`,
 * which dispatches to nvmeibt_cdv_alloc_handle_satellite_attach_response (Stage B).
 *
 * Idempotent: the request_id stays stable across retries while in
 * AWAITING_SATELLITE_ATTACH.  Caller bumps the request_id only when starting a
 * new attach (e.g., on transition from NOT_ALLOCATOR).
 */
static void cdv_send_attach_satellite_request(struct nvmeibt_cdv_alloc *alloc)
{
	struct nvmeibt_Str *json;

	json = NNVMEIBT_STR_ALLOC(cdv_sat_req_json_alloc);
	if (!json) {
		N_Ef(cdv_sat_req_oom,
		     "CDV-alloc: attachSatelliteRequest OOM cdv=@STR", alloc->cdv_uuid);
		return;
	}

	nvmeibt_Str_sprintf(json,
		"{" KAFKA_PRODUCER_MSG_HEADER_FMT
		"\"payload\": {\"cdvUUID\": \"%s\", "
		"\"allocatorTomaHostname\": \"%s\", "
		"\"allocatorGeneration\": %llu, "
		"\"raftTerm\": %llu, "
		"\"requestId\": \"%llu\"}}",
		KAFKA_PRODUCER_MSG_HEADER_VAR("attachSatelliteRequest", 1),
		alloc->cdv_uuid,
		nvmeibt_get_my_hostname(),
		alloc->allocator_generation,
		nvmeibt_raft_get_current_term(),
		alloc->satellite_attach_request_id);

	N_If(cdv_sat_req_send,
	     "CDV-alloc: attachSatelliteRequest cdv=@STR gen=@LLU reqId=@LLU",
	     alloc->cdv_uuid, alloc->allocator_generation,
	     alloc->satellite_attach_request_id);

	nvmeibt_kafka_outgoing_msgs_queue_add(
		alloc->cdv_uuid,
		nvmeibt_Str_str(json),
		nvmeibt_Str_strlen(json) + 1,
		NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);

	NNVMEIBT_STR_FREE(cdv_sat_req_json_free, json);
}

/*
 * nvmeibt_cdv_alloc_on_topo_applied — hook fired from update_applied_topology
 * on every committed topology change, on every TOMA (leader and followers).
 *
 * Called only for the first pRAID of a CDV (the caller filters).  Diffs the
 * previously-applied (allocator_toma_id, allocator_generation) against the
 * newly-applied values and drives the local role transition.  On promote
 * (new allocator is me), transitions to AWAITING_SATELLITE_ATTACH and fires
 * Stage A of the satellite-attach handshake.  On demote (I was the allocator,
 * new one is someone else), tears down local allocator-side state.  Pushes
 * CDV_ALLOCATOR_UPDATE to local clients so they learn the new identity.
 */
void nvmeibt_cdv_alloc_on_topo_applied(const char *cdv_uuid,
				       const struct nvmeibt_praid_topo_ctx *prev_applied,
				       const struct nvmeibt_praid_topo_ctx *new_applied)
{
	const char *my_hostname;
	bool was_me, is_me;
	struct nvmeibt_cdv_alloc *alloc;
	bool toma_changed;

	if (!cdv_uuid || !prev_applied || !new_applied)
		return;

	toma_changed = strncmp(new_applied->allocator_toma_id,
			       prev_applied->allocator_toma_id,
			       sizeof(new_applied->allocator_toma_id)) != 0;

	/* No-op if allocator identity did not change. */
	if (new_applied->allocator_generation == prev_applied->allocator_generation &&
	    !toma_changed) {
		return;
	}

	/* Generation regression is not expected under RAFT-ordered apply. */
	if (new_applied->allocator_generation < prev_applied->allocator_generation) {
		N_Ef(cdv_topo_applied_gen_regress,
		     "CDV-alloc: topo_applied cdv=@STR gen regressed @LLU -> @LLU",
		     cdv_uuid, prev_applied->allocator_generation,
		     new_applied->allocator_generation);
		return;
	}

	/* Empty new allocator_toma_id: pRAID is not yet elected (first ever commit
	 * may carry zero fields).  Nothing to act on locally. */
	if (!new_applied->allocator_toma_id[0])
		return;

	my_hostname = nvmeibt_get_my_hostname();
	is_me  = my_hostname && my_hostname[0] &&
		 strncmp(new_applied->allocator_toma_id, my_hostname,
			 sizeof(new_applied->allocator_toma_id)) == 0;
	was_me = my_hostname && my_hostname[0] && prev_applied->allocator_toma_id[0] &&
		 strncmp(prev_applied->allocator_toma_id, my_hostname,
			 sizeof(prev_applied->allocator_toma_id)) == 0;

	N_If(cdv_topo_applied,
	     "CDV-alloc: topo_applied cdv=@STR new_toma=@STR gen @LLU->@LLU is_me=@BOOL was_me=@BOOL",
	     cdv_uuid, new_applied->allocator_toma_id,
	     prev_applied->allocator_generation, new_applied->allocator_generation,
	     (int)is_me, (int)was_me);

	alloc = find_or_create_alloc(cdv_uuid);
	if (!alloc) {
		N_Ef(cdv_topo_applied_oom,
		     "CDV-alloc: topo_applied cdv=@STR find_or_create_alloc failed", cdv_uuid);
		return;
	}

	/* Sync local (allocator_toma_id, allocator_generation) from applied topology. */
	strncpy(alloc->allocator_toma_id, new_applied->allocator_toma_id,
		NVMEIBT_CDV_HOSTNAME_LEN - 1);
	alloc->allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN - 1] = '\0';
	alloc->allocator_generation = new_applied->allocator_generation;

	if (is_me) {
		/* Promote: transition to AWAITING_SATELLITE_ATTACH and fire Stage A.
		 * Tear down any satellite state left from a previous tenure as
		 * allocator on this CDV so Stage B re-opens the satellite fresh
		 * (the new attach carries a bumped reservation version at the target). */
		if (alloc->state == NVMEIBT_CDV_ALLOC_STATE_ACTIVE &&
		    alloc->satellite_attach_request_id == new_applied->allocator_generation) {
			/* Already ACTIVE for this generation — nothing to do. */
			goto push;
		}
		if (alloc->io_wq) {
			nvmeibt_wq_drain(alloc->io_wq);
			nvmeibt_wq_destroy(alloc->io_wq);
			alloc->io_wq = NULL;
		}
		cdv_close_worker_fds(alloc);
		alloc->satellite_dev_path[0] = '\0';
		alloc->satellite_uuid[0]     = '\0';
		alloc->satellite_reservation_version = 0;

		alloc->state                       = NVMEIBT_CDV_ALLOC_STATE_AWAITING_SATELLITE_ATTACH;
		alloc->satellite_attach_request_id = new_applied->allocator_generation;
		alloc->ondisk_loaded               = false;

		cdv_send_attach_satellite_request(alloc);
	} else if (was_me) {
		/* Demote: I was the allocator, I no longer am.  The new allocator's
		 * attachSatelliteRequest will preempt any residual satellite hold via
		 * the reservation-version bump; tear down our local allocator-side
		 * state proactively. */
		alloc->state         = NVMEIBT_CDV_ALLOC_STATE_NOT_ALLOCATOR;
		alloc->ondisk_loaded = false;
		if (alloc->io_wq) {
			nvmeibt_wq_drain(alloc->io_wq);
			nvmeibt_wq_destroy(alloc->io_wq);
			alloc->io_wq = NULL;
		}
		cdv_close_worker_fds(alloc);
		alloc->satellite_dev_path[0] = '\0';
		alloc->satellite_uuid[0]     = '\0';
		alloc->satellite_reservation_version = 0;
		N_If(cdv_alloc_demoted,
		     "CDV-alloc: demoted cdv=@STR (allocator is now @STR gen=@LLU)",
		     cdv_uuid, new_applied->allocator_toma_id,
		     new_applied->allocator_generation);
	}
	/* else: the allocator is still someone else — we just recorded identity. */

push:
	/* Push the updated allocator identity to any local registrants so clients
	 * attached to this TOMA learn the new allocator and redirect their next
	 * CDV_ALLOC_EXTENT / CDV_LIST_EXTENTS requests accordingly. */
	nvmeibt_cdv_alloc_push_to_registrants(cdv_uuid);
}

/*
 * Stage B: invoked from the Kafka dispatch on attachSatelliteResponse.
 *
 * On OK: store the satellite UUID, build the satellite device path, kick off
 * the scan WQ which (a) opens the satellite block device and (b) loads any
 * previously-persisted allocator state.  Promote to ACTIVE on scan finalize.
 *
 * On non-OK: log + leave state as AWAITING_SATELLITE_ATTACH so retries
 * (delivered as fresh AttachSatelliteRequest from cdv_alloc_retry_pending,
 * not yet implemented) can proceed.  Terminal failures (CDV_NOT_FOUND,
 * CDV_BEING_DELETED) tear down the allocator entry.
 */
void nvmeibt_cdv_alloc_handle_satellite_attach_response(
	const char *cdv_uuid,
	uint64_t    request_id,
	const char *status,
	const char *satellite_uuid,
	uint64_t    reservation_version,
	uint64_t    allocator_generation)
{
	struct nvmeibt_cdv_alloc *alloc;

	if (!cdv_uuid || !status) {
		N_Ef(cdv_sat_resp_bad_args, "CDV-alloc: attachSatelliteResponse bad args");
		return;
	}

	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		N_Wf(cdv_sat_resp_no_alloc,
		     "CDV-alloc: attachSatelliteResponse cdv=@STR has no local alloc entry; ignoring",
		     cdv_uuid);
		return;
	}

	/* Reject responses that do not match our in-flight request. */
	if (alloc->state != NVMEIBT_CDV_ALLOC_STATE_AWAITING_SATELLITE_ATTACH) {
		N_Wf(cdv_sat_resp_bad_state,
		     "CDV-alloc: attachSatelliteResponse cdv=@STR state=@INT not AWAITING; ignoring",
		     cdv_uuid, (int)alloc->state);
		return;
	}
	if (request_id != alloc->satellite_attach_request_id) {
		N_Wf(cdv_sat_resp_stale_reqid,
		     "CDV-alloc: attachSatelliteResponse cdv=@STR stale reqId=@LLU expected=@LLU; ignoring",
		     cdv_uuid, request_id, alloc->satellite_attach_request_id);
		return;
	}
	if (allocator_generation != alloc->allocator_generation) {
		N_Wf(cdv_sat_resp_stale_gen,
		     "CDV-alloc: attachSatelliteResponse cdv=@STR stale gen=@LLU expected=@LLU; ignoring",
		     cdv_uuid, allocator_generation, alloc->allocator_generation);
		return;
	}

	if (strcmp(status, "OK") != 0) {
		N_Wf(cdv_sat_resp_not_ok,
		     "CDV-alloc: attachSatelliteResponse cdv=@STR status=@STR; will retry on next notify",
		     cdv_uuid, status);
		/*
		 * Terminal failures: drop our claim on this CDV.  Transient failures
		 * (INTERNAL_ERR, etc.) leave us in AWAITING_SATELLITE_ATTACH; the
		 * RAFT layer will redeliver a notify when the allocator is re-elected.
		 */
		if (!strcmp(status, "CDV_NOT_FOUND") || !strcmp(status, "CDV_BEING_DELETED"))
			alloc->state = NVMEIBT_CDV_ALLOC_STATE_NOT_ALLOCATOR;
		return;
	}

	/* OK path: bind the satellite to this allocator entry and start the scan. */
	if (satellite_uuid && satellite_uuid[0]) {
		strncpy(alloc->satellite_uuid, satellite_uuid, NVMEIBT_CDV_UUID_STRLEN - 1);
		alloc->satellite_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
	}
	alloc->satellite_reservation_version = reservation_version;

	/*
	 * Set up the per-CDV I/O WQ now (resolves satellite_dev_path from
	 * satellite_uuid) so subsequent scan + header writes can dispatch.  If the
	 * satellite block device is not yet visible to this TOMA (timing race —
	 * the AddVolume Kafka message for the satellite may still be in flight),
	 * leave state in AWAITING; the lazy retry on incoming ALLOC requests
	 * (handle_cdv_alloc_extent state gate) will re-fire Stage A and we'll
	 * land here again.
	 */
	if (cdv_ensure_io_wq(cdv_uuid, alloc) < 0) {
		N_Wf(cdv_sat_resp_no_wq,
		     "CDV-alloc: attachSatelliteResponse OK but satellite bdev not ready for cdv=@STR; will retry",
		     cdv_uuid);
		return;
	}

	/*
	 * Scan the satellite to load any persisted allocator state.  The scan
	 * worker opens alloc->satellite_dev_path lazily (see cdv_worker_open_fd).
	 * On scan finalize the state is promoted to ACTIVE and the registrants
	 * are notified — see cdv_scan_finalize / promote-on-load logic.
	 */
	if (!alloc->ondisk_loaded)
		cdv_ondisk_scan_async(cdv_uuid, alloc);

	/* Persist the allocator identity to the satellite header (durability). */
	cdv_async_write_header(alloc);

	/*
	 * Until the scan finalizes we remain in AWAITING_SATELLITE_ATTACH so
	 * incoming ALLOC requests are rejected with WRONG_GEN.  When ondisk_loaded
	 * flips to true, handle_cdv_alloc_extent will see state == ACTIVE and
	 * begin serving.  (Promotion is wired in the scan finalize path.)
	 */
	N_If(cdv_sat_resp_ok,
	     "CDV-alloc: attachSatelliteResponse OK cdv=@STR sat=@STR resv=@LLU gen=@LLU",
	     cdv_uuid, satellite_uuid ? satellite_uuid : "", reservation_version, allocator_generation);

	nvmeibt_cdv_alloc_push_to_registrants(cdv_uuid);
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
	 * - On allocator election (async scan from nvmeibt_cdv_alloc_elect)
	 * - On the first ALLOC request (if election hasn't scanned yet)
	 * Per-CDV I/O work queues are created on demand. */

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

		/* Drain and destroy the per-CDV I/O WQ before freeing state. */
		if (alloc->io_wq) {
			nvmeibt_wq_drain(alloc->io_wq);
			nvmeibt_wq_destroy(alloc->io_wq);
			alloc->io_wq = NULL;
		}

		if (alloc->cdv_fd >= 0)
			NNVMEIBT_CLOSE(cdv_alloc_destroy_close, alloc->cdv_fd);

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

	if (alloc->total_data_extents == 0)
		return;   /* capacity unknown */

	used_pct = (unsigned int)(alloc->n_allocated * 100 / alloc->total_data_extents);

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
		const char *cdv_name = alloc->dev_path[0] ?
			alloc->dev_path + strlen(CDV_DEV_PATH_PREFIX) : "(unknown)";

		if (alloc->total_data_extents > 0)
			used_pct = (unsigned int)(alloc->n_allocated * 100
						  / alloc->total_data_extents);

		(*printf_fn)(printf_ctx,
			     "\t- cdv=%-20s [%-40s] allocator=%-20s gen=%-6llu allocated=%-6llu / %-6llu  (%u%%)%s%s\n",
			     cdv_name,
			     alloc->cdv_uuid,
			     alloc->allocator_toma_id[0] ? alloc->allocator_toma_id : "(unelected)",
			     alloc->allocator_generation,
			     alloc->n_allocated,
			     alloc->total_data_extents,
			     used_pct,
			     alloc->capacity_warning_sent ? " [CAPACITY WARNING]" : "",
			     alloc->n_pending_zeroing ? " [ZEROING]" : "");
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
		const char *cdv_name = alloc->dev_path[0] ?
			alloc->dev_path + strlen(CDV_DEV_PATH_PREFIX) : "(unknown)";

		if (alloc->total_data_extents > 0)
			used_pct = (unsigned int)(alloc->n_allocated * 100
						  / alloc->total_data_extents);

		(*printf_fn)(printf_ctx,
			     "\tcdv=%-20s [%s]  allocator=%s  gen=%llu  allocated=%llu/%llu (%u%%)  ondisk_loaded=%s\n",
			     cdv_name,
			     alloc->cdv_uuid,
			     alloc->allocator_toma_id[0] ? alloc->allocator_toma_id : "(unelected)",
			     alloc->allocator_generation,
			     alloc->n_allocated,
			     alloc->total_data_extents,
			     used_pct,
			     alloc->ondisk_loaded ? "yes" : "no");

		if (!XDLIST_EMPTY(&alloc->extents)) {
			(*printf_fn)(printf_ctx,
				     "\t  %-8s  %-6s  %s\n",
				     "ext_idx", "state", "tpv_uuid");
			XDLIST_FOREACH(entry, &alloc->extents) {
				(*printf_fn)(printf_ctx,
					     "\t  %-8llu  %-6s  %s\n",
					     entry->extent_index,
					     entry->needs_zeroing ? "ZERO" : "alloc",
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
					uint32_t    allocator_size_gib,
					uint32_t    cdv_extent_size_mib)
{
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;
	uint64_t n_released = 0;

	/* ── 1. Look up the per-CDV allocator ── */
	alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
	if (!alloc) {
		/* Under the RAFT-embedded identity design, the alloc entry is
		 * created exclusively by the topology-apply hook.  If no entry
		 * exists here, this TOMA is not (yet) the allocator for this CDV;
		 * the elected allocator will handle the same Kafka fan-out. */
		N_If(cdv_free_all_no_alloc,
		     "CDV-alloc: free_all_for_tpv cdv=@STR tpv=@STR no alloc entry; skipping (allocator handles it)",
		     cdv_uuid, tpv_uuid);
		return 0;
	}

	/*
	 * Only the elected allocator TOMA owns the satellite write path.
	 * Management's cdvAllocatorFreeAll Kafka message fans out to all
	 * first-pRAID host TOMAs (legacy delivery pattern); on non-allocator
	 * nodes the record/header writes would fail.  Silently ignore on
	 * non-allocators so the elected allocator handles the work.
	 */
	if (alloc->state != NVMEIBT_CDV_ALLOC_STATE_ACTIVE) {
		N_If(cdv_free_all_skip_not_alloc,
		     "CDV-alloc: free_all_for_tpv cdv=@STR tpv=@STR state=@INT not ACTIVE; skipping (allocator handles it)",
		     cdv_uuid, tpv_uuid, (int)alloc->state);
		return 0;
	}

	/* Cache CDV geometry for use by the background zero worker. */
	if (allocator_size_gib && !alloc->allocator_size_gib)
		alloc->allocator_size_gib = allocator_size_gib;
	if (cdv_extent_size_mib && !alloc->cdv_extent_size_mib)
		alloc->cdv_extent_size_mib = cdv_extent_size_mib;

	/* Best-effort: set up I/O WQ for async writes + zeroing. */
	(void)cdv_ensure_io_wq(cdv_uuid, alloc);

	if (!alloc->ondisk_loaded) {
		cdv_ondisk_scan_async(cdv_uuid, alloc);
		/*
		 * Scan in progress — in-memory extent list may be incomplete.
		 * Proceed with whatever is loaded; the scan finalize will
		 * re-dispatch zeroing for any NEEDS_ZEROING entries it finds.
		 */
	}

	/*
	 * ── 2. Release extents ──
	 *
	 * Two modes, chosen by the cdv_extent_zero_on_free TOMA config param:
	 *
	 *   zero-on-free OFF (default): write a free ondisk record, remove the
	 *       in-memory entry, decrement n_allocated.  Fast path; data on the
	 *       CDV remains until overwritten by the next TPV that allocates
	 *       the slot.
	 *
	 *   zero-on-free ON: mark the entry needs_zeroing, persist an
	 *       ALLOCATED|NEEDS_ZEROING ondisk record with geometry, dispatch a
	 *       background zero write.  The entry stays in alloc->extents
	 *       (blocking reallocation) until cdv_zero_finalize removes it.
	 *
	 * NEEDS_ZEROING on-disk records from a previous zero-on-free-ON era are
	 * always honored on scan, regardless of the current flag.
	 */
	if (nvmeibt_cdv_extent_zero_on_free) {
		XDLIST_FOREACH(entry, &alloc->extents) {
			if (strncmp(entry->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN) != 0)
				continue;
			if (entry->needs_zeroing)
				continue;   /* already queued (e.g. duplicate Kafka delivery) */

			entry->needs_zeroing = true;
			alloc->n_pending_zeroing++;

			cdv_async_write_record_needs_zeroing(alloc, entry->extent_index,
							     tpv_uuid,
							     allocator_size_gib,
							     cdv_extent_size_mib);

			cdv_dispatch_zero_extent(alloc, entry->extent_index,
						 tpv_uuid,
						 allocator_size_gib,
						 cdv_extent_size_mib);
			n_released++;
		}
	} else {
		XDLIST_FOREACH_SAFE(entry, &alloc->extents) {
			if (strncmp(entry->tpv_uuid, tpv_uuid, NVMEIBT_CDV_UUID_STRLEN) != 0)
				continue;
			if (entry->needs_zeroing)
				continue;   /* already in the pending-zero flow; let it finish */

			cdv_async_write_record(alloc, entry->extent_index, NULL);

			XDLIST_ELEM_DEL(&alloc->extents, entry);
			alloc->n_allocated--;
			NNVMEIBT_BM_FREE(cdv_free_all_entry, entry);
			n_released++;
		}
	}

	/* ── 3. Rewrite header (extent count may have changed) ── */
	if (n_released > 0)
		cdv_async_write_header(alloc);

	if (!nvmeibt_cdv_extent_zero_on_free)
		cdv_maybe_warn_capacity(alloc);  /* may clear the warning flag */

	if (n_released > 0)
		cdv_publish_alloc_stats(alloc);

	N_If(cdv_free_all_done,
	     "CDV-alloc: free_all cdv=@STR tpv=@STR released=@LLU zero_on_free=@LLU pending_zero=@LLU total=@LLU",
	     cdv_uuid, tpv_uuid, n_released, nvmeibt_cdv_extent_zero_on_free,
	     alloc->n_pending_zeroing, alloc->n_allocated);

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
 *   3. Find the first unallocated extent index in [1, total_data_extents].
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
	bool     found, occupied;
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

	/*
	 * Split-brain gate: an allocator that has lost RAFT quorum contact
	 * must not serve ALLOC.  The chosen allocator's identity is only ever
	 * bumped by a RAFT leader (unique per term), so a partitioned minority
	 * can never have elected a newer allocator — if we're still cached as
	 * allocator here but RAFT is no longer valid, we're the stale side.
	 * Respond WRONG_GEN so the client retries; when the partition heals,
	 * the leader's notify will carry a higher gen and we'll update.
	 */
	if (!nvmeibt_raft_is_raft_valid()) {
		N_Wf(cdv_alloc_no_majority,
		     "CDV-alloc: ALLOC cdv=@STR rejected: lost RAFT majority",
		     cdv_uuid);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = 0;
		goto send;
	}

	alloc       = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);

	/*
	 * Allocator-state gate: only serve ALLOC when we are in the ACTIVE state.
	 * In NOT_ALLOCATOR we have no business serving (someone else holds the
	 * role).  In AWAITING_SATELLITE_ATTACH the satellite is not yet open and
	 * any record write would fail; reject so the client retries until the
	 * satellite-attach handshake completes (handle_satellite_attach_response
	 * promotes us to ACTIVE on scan finalize).
	 *
	 * Lazy retry: if we're stuck in AWAITING (response dropped or transient
	 * failure), opportunistically re-fire Stage A on incoming ALLOC requests
	 * so the handshake makes progress without needing a dedicated timer.
	 * Idempotent on management's side via (cdvUUID, requestId).
	 */
	if (alloc && alloc->state != NVMEIBT_CDV_ALLOC_STATE_ACTIVE) {
		N_Wf(cdv_alloc_state_not_active,
		     "CDV-alloc: ALLOC cdv=@STR rejected: state=@INT not ACTIVE",
		     cdv_uuid, (int)alloc->state);
		if (alloc->state == NVMEIBT_CDV_ALLOC_STATE_AWAITING_SATELLITE_ATTACH)
			cdv_send_attach_satellite_request(alloc);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/* On-demand CDV scan: if we have an allocator entry but haven't yet
	 * loaded the on-CDV extent records, dispatch an async scan and tell the
	 * client to retry (WRONG_GEN) so we don't block the main thread.
	 */
	if (alloc && !alloc->ondisk_loaded) {
		if (alloc->total_data_extents == 0 && req->total_data_extents > 0)
			alloc->total_data_extents = req->total_data_extents;
		cdv_ondisk_scan_async(cdv_uuid, alloc);
		N_Wf(cdv_alloc_scan_pending,
		     "CDV-alloc: ALLOC cdv=@STR scan in progress; returning WRONG_GEN to client",
		     cdv_uuid);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/*
	 * No in-memory allocator for this CDV — under the RAFT-embedded identity
	 * design, the alloc entry is created exclusively by the topology-apply
	 * hook once this TOMA applies a committed pRAID topology naming us the
	 * allocator.  If the client reached us before our apply ran (cold-start
	 * race) or we are not the allocator at all, we reply WRONG_GEN and let
	 * the client redirect to the correct allocator on its next CDV topology
	 * push.  Do NOT create a zero-initialized entry here.
	 */
	if (!alloc) {
		N_Wf(cdv_alloc_no_alloc,
		     "CDV: ALLOC cdv=@STR no alloc entry; returning WRONG_GEN",
		     cdv_uuid);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = 0;
		goto send;
	}

	/* Ensure per-CDV I/O WQ exists for async persistence.
	 * Best-effort: writes are fire-and-forget, so failure is non-fatal.
	 */
	(void)cdv_ensure_io_wq(cdv_uuid, alloc);

	/*
	 * ── Verify we are the elected allocator for this CDV ────────────────
	 *
	 * Only the elected allocator TOMA should serve ALLOC requests.  If
	 * the allocator has been elected but it's a different node, reject
	 * with WRONG_GEN so the client re-syncs from the CDV topology push.
	 */
	if (alloc->allocator_toma_id[0] &&
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
	if (req->client_generation != alloc->allocator_generation) {
		N_If(cdv_alloc_wrong_gen,
		     "CDV: ALLOC WRONG_GEN cdv=@STR client=@LLU toma=@LLU",
		     cdv_uuid, req->client_generation, alloc->allocator_generation);
		resp.status = NVMEIBT_CDV_ALLOC_WRONG_GEN;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/* ── Capacity info ───────────────────────────────────────────────────── */

	/* Refresh cached capacity after a TOMA restart (stored value is 0). */
	if (alloc->total_data_extents == 0 && req->total_data_extents > 0)
		alloc->total_data_extents = req->total_data_extents;

	total = alloc->total_data_extents;

	if (total == 0) {
		/* alloc exists but capacity unknown; client sent 0 too */
		N_Ef(cdv_alloc_no_cap_post_restart,
		     "CDV: ALLOC cdv=@STR no capacity known after restart; refusing",
		     cdv_uuid);
		resp.status = NVMEIBT_CDV_ALLOC_ERROR;
		goto send;
	}

	/* ── CDV-full check ─────────────────────────────────────────────────── */
	if (alloc->n_allocated >= total) {
		N_Wf(cdv_alloc_full,
		     "CDV: ALLOC cdv=@STR FULL allocated=@LLU total=@LLU",
		     cdv_uuid, alloc->n_allocated, total);
		cdv_maybe_warn_capacity(alloc);   /* ensure Kafka event reaches management */
		resp.status = NVMEIBT_CDV_ALLOC_CDV_FULL;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/*
	 * ── Find first free extent index ────────────────────────────────────
	 *
	 * Extent indices are 1-based: index 0 is unused (kept so the satellite
	 * record at offset 0 stays the header); data extents are numbered
	 * 1 .. total_data_extents.
	 */
	candidate = 0;
	found     = false;
	for (i = 1; i <= total; i++) {
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

	resp.extent_index         = candidate;
	resp.allocator_generation = alloc->allocator_generation;
	resp.status               = NVMEIBT_CDV_ALLOC_OK;

	/* Check if this allocation pushed the CDV above the warning watermark. */
	if (alloc) {
		cdv_maybe_warn_capacity(alloc);
		cdv_publish_alloc_stats(alloc);
	}

	N_If(cdv_alloc_ok,
	     "CDV: ALLOC OK cdv=@STR idx=@LLU tpv=@STR req_id=@LLU gen=@LLU",
	     cdv_uuid, candidate, tpv_uuid, req->req_id, resp.allocator_generation);

	/*
	 * Persist the allocation record to the CDV and THEN send the response.
	 * The client must not learn about the extent until the on-disk record
	 * is confirmed written — otherwise a TOMA crash between response and
	 * write would leave the client holding an untracked extent.
	 *
	 * cdv_dispatch_alloc_persist() queues the write on the per-CDV I/O WQ;
	 * the response is sent from the finalize callback on the main thread
	 * after the worker confirms the pwrite succeeded.
	 *
	 * If dispatch fails (OOM, no WQ), fall through to synchronous response
	 * with ERROR status so the client retries.
	 */
	if (alloc && cdv_dispatch_alloc_persist(alloc, candidate, tpv_uuid,
						&msg->registrant_ctx, &resp) == 0)
		return 0;   /* response will be sent from finalize */

	/* Dispatch failed — send error response immediately. */
	if (resp.status == NVMEIBT_CDV_ALLOC_OK)
		resp.status = NVMEIBT_CDV_ALLOC_ERROR;

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

		/* Clear the extent record on the CDV (async, best-effort). */
		cdv_async_write_record(alloc, req->extent_index, NULL);

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
