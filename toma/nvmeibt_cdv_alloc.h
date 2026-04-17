/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 * nvmeibt_cdv_alloc.h — CDV extent allocator: in-memory state + on-CDV persistence.
 *
 * TOMA maintains, for each CDV (Carrier Direct Volume), a table of which data
 * CDV_extents have been allocated to which TPV (Thin-Provisioned Volume).
 *
 * Module responsibilities:
 *   - nvmeibt_cdv_extent_entry: in-memory per-extent node, owned by a per-CDV
 *     allocator.
 *   - nvmeibt_cdv_alloc: per-CDV allocator, held in a global hash table keyed
 *     by CDV UUID string.
 *
 * Persistence:
 *   Extent allocation metadata is stored on the CDV block device itself, in
 *   the allocator region at the beginning of the volume (before the data
 *   extents).  Each allocation/free dispatches a 4 KiB record write to the
 *   per-CDV I/O work queue (async, fire-and-forget).  On allocator election
 *   (or on the first ALLOC request after TOMA restart), the allocator region
 *   is scanned asynchronously to rebuild in-memory state.
 *
 * CDV allocator identity (allocator_toma_id, allocator_generation) is elected
 * by the RAFT leader via nvmeibt_cdv_alloc_elect() and distributed to all
 * TOMAs and clients:
 *   - Clients: via CDV_ALLOCATOR_UPDATE messages pushed to all registrants
 *     (nvmeibt_cdv_alloc_push_to_registrants).
 *
 * Threading:
 *   All public functions must be called from TOMA's single main thread (or
 *   with the global topology lock held).  All CDV I/O (scans + record/header
 *   writes) is dispatched to a per-CDV worker thread (alloc->io_wq); results
 *   are applied back on the main thread via the standard WQ finalize path.
 *   Multiple CDVs are serviced in parallel (one WQ per CDV).
 */

#ifndef NVMEIBT_CDV_ALLOC_H
#define NVMEIBT_CDV_ALLOC_H

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"

/* ── UUID string length (matches NVMEIBC_BD_UUID_LEN on the client side) ── */
#define NVMEIBT_CDV_UUID_STRLEN  64

/* ── In-memory per-extent node ──────────────────────────────────────────────
 *
 * Lives on nvmeibt_cdv_alloc.extents.  One node per allocated CDV_extent.
 * The 'link' field is the XDLIST embedded link; must match the field name
 * in the XDLIST_DECLARE in nvmeibt_cdv_alloc.
 *
 * When needs_zeroing is true the extent has been freed by a deleted TPV but
 * not yet zeroed on disk.  It stays in the extents list (counted in n_allocated)
 * to block reallocation until the background zero completes.
 */
struct nvmeibt_cdv_extent_entry {
	uint64_t   extent_index;
	char       tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	bool       needs_zeroing;	/* freed but awaiting background zero before reuse */
	struct xdlist link;
};

/*
 * Capacity watermarks for CDVCapacityWarning Kafka events.
 * A warning is emitted when usage rises to WARN_PCT; the flag clears when
 * usage falls back below WARN_CLEAR_PCT (hysteresis prevents Kafka floods).
 */
#define NVMEIBT_CDV_WARN_PCT       90   /* fire warning at >= 90% used */
#define NVMEIBT_CDV_WARN_CLEAR_PCT 85   /* clear flag when usage drops below 85% */

/* ── On-CDV metadata format ─────────────────────────────────────────────────
 *
 * Extent allocation records are stored at the beginning of the CDV volume
 * (the "allocator region", bytes 0 to allocator_size_gb * 1 GiB - 1).
 *
 * Layout on the CDV:
 *   Offset 0:                    Header  (CDV_ONDISK_BLOCK_SIZE bytes)
 *   Offset CDV_ONDISK_BLOCK_SIZE:  Record for extent_index 0
 *   Offset 2 * CDV_ONDISK_BLOCK_SIZE: Record for extent_index 1
 *   ...
 *
 * Each block is 4 KiB (PAGE_SIZE), matching the NVMe physical block size
 * for atomic single-block writes.  With a 1 GiB allocator region this
 * supports (1 GiB / 4 KiB) - 1 = 262,143 extent slots.
 */

#define CDV_ONDISK_BLOCK_SIZE   4096U
#define CDV_ONDISK_MAGIC        0x43444D31U     /* 'CDM1' */
#define CDV_ONDISK_VERSION      1

struct cdv_alloc_ondisk_header {
	uint32_t magic;                                  /* CDV_ONDISK_MAGIC */
	uint32_t version;                                /* CDV_ONDISK_VERSION */
	uint64_t total_data_extents;
	char     allocator_toma_id[NVMEIBT_CDV_UUID_STRLEN]; /* 64 bytes */
	uint64_t allocator_generation;
	uint32_t crc32;
	uint8_t  reserved[CDV_ONDISK_BLOCK_SIZE - 4 - 4 - 8 - 64 - 8 - 4];
} __attribute__((__packed__));

#define CDV_ONDISK_RECORD_FLAG_ALLOCATED     0x01
#define CDV_ONDISK_RECORD_FLAG_NEEDS_ZEROING 0x02  /* freed but not yet zeroed; blocked from reuse */

struct cdv_alloc_ondisk_record {
	uint8_t  flags;                                  /* CDV_ONDISK_RECORD_FLAG_* */
	uint8_t  reserved1[7];
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];     /* 64 bytes; zeroed if free */
	uint32_t crc32;
	/*
	 * Zeroing geometry — not CRC-covered; valid only when NEEDS_ZEROING is set.
	 * Stored here so the zeroing worker can recover geometry on restart without
	 * requiring a separate management query.
	 */
	uint32_t zeroing_allocator_size_gb;
	uint32_t zeroing_cdv_extent_size_mb;
	uint8_t  reserved2[CDV_ONDISK_BLOCK_SIZE - 1 - 7 - 64 - 4 - 4 - 4];
} __attribute__((__packed__));

/* ── Per-CDV allocator ──────────────────────────────────────────────────────
 *
 * One instance per CDV that has had at least one extent allocated.
 * Stored in the global hash table keyed by cdv_uuid (ASCII string).
 * cdv_uuid must be the first field: nvmeib_hash_add_ascii_str requires the
 * key to be a pointer into the object itself.
 */
/* Maximum hostname length — must match NVMEIB_HOST_NAME_LEN (64) on the client side. */
#define NVMEIBT_CDV_HOSTNAME_LEN  64

struct nvmeibt_cdv_alloc {
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN]; /* hash key; must be first field */
	uint64_t n_allocated;
	uint64_t total_data_extents;	/* CDV capacity in data extents; populated from first ALLOC */
	/*
	 * Allocator identity — the TOMA node responsible for this CDV's extent
	 * allocation.  Elected by the RAFT leader and committed to all TOMAs.
	 * allocator_toma_id is empty ("") until the first election.
	 */
	char     allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN];
	uint64_t allocator_generation;	/* incremented on each allocator change; echoed in ALLOC responses */
	bool     capacity_warning_sent;	/* true after CDVCapacityWarning sent; cleared on hysteresis */
	bool     ondisk_loaded;		/* true after CDV allocator region has been scanned */
	bool     scan_in_progress;	/* true while async scan WQ entry is in flight */
	bool     scan_fresh_seen_once;	/* true after first scan returned "fresh" (no magic);
					 * forces one retry to guard against CDV-not-yet-online
					 * during simultaneous client+TOMA restart */
	uint32_t scan_retry_delay_ms;	/* 0 on first attempt; 100 → 1000 backoff on failure
					 * while this TOMA is the elected allocator and
					 * ondisk_loaded is still false */
	/*
	 * CDV geometry — cached on first free_all_for_tpv call and on scan.
	 * Needed by the background zero worker to compute per-extent byte offsets
	 * when re-dispatching zeroing after a TOMA restart.
	 */
	uint32_t allocator_size_gb;	/* size of on-CDV allocator region in GiB */
	uint32_t cdv_extent_size_mb;	/* size of each data CDV extent in MiB */
	uint64_t n_pending_zeroing;	/* extents with needs_zeroing=true (not yet re-usable) */
	int      cdv_fd;		/* cached fd; opened/used ONLY from io_wq worker thread */
	char     dev_path[80];		/* /dev/nvmesh/<name>; resolved on main thread */
	struct nvmeibt_wq *io_wq;	/* per-CDV I/O work queue (scan + writes) */
	XDLIST_DECLARE(, struct nvmeibt_cdv_extent_entry, link) extents;
};

/* ── One-time init / shutdown ────────────────────────────────────────────── */

/*
 * nvmeibt_cdv_alloc_one_time_init — allocate the global hash table.
 * Called from nvmeibt_toma_init(), after nvmeibt_local_disk_one_time_init().
 * Returns 0 on success, negative errno on fatal hash-create failure.
 * In-memory state is rebuilt lazily: the CDV allocator region is scanned
 * on the first ALLOC request for each CDV (when the disk is available).
 */
int  nvmeibt_cdv_alloc_one_time_init(void);

/*
 * nvmeibt_cdv_alloc_destroy — free all in-memory state.
 * Called at TOMA shutdown.
 */
void nvmeibt_cdv_alloc_destroy(void);

/* ── Per-CDV operations (called from CDV_ALLOC/FREE/LIST handlers) ──────── */

/*
 * nvmeibt_cdv_alloc_add_extent — record that extent_index within cdv_uuid has
 * been allocated to tpv_uuid.
 * Returns 0 on success, negative errno on OOM.
 */
int nvmeibt_cdv_alloc_add_extent(const char *cdv_uuid,
				 uint64_t    extent_index,
				 const char *tpv_uuid);

/*
 * nvmeibt_cdv_alloc_remove_extent — mark extent_index in cdv_uuid as free.
 * Returns 0 on success, -ENOENT if the record was not found.
 */
int nvmeibt_cdv_alloc_remove_extent(const char *cdv_uuid, uint64_t extent_index);

/*
 * nvmeibt_cdv_alloc_list_for_tpv — collect all extent indices allocated to
 * tpv_uuid within cdv_uuid.  On success *out_indices is a NNVMEIBT_BM_ALLOC'd
 * array of *out_count uint64_t values; the caller must NNVMEIBT_BM_FREE it.
 * Returns 0 on success (including the empty case), negative errno on OOM.
 */
int nvmeibt_cdv_alloc_list_for_tpv(const char  *cdv_uuid,
				    const char  *tpv_uuid,
				    uint64_t   **out_indices,
				    uint64_t    *out_count);

/*
 * nvmeibt_cdv_alloc_gc_stale_entries — remove CDV allocator entries that no
 * longer have a matching live bdev.
 *
 * Iterates the global CDV allocator hash and removes any entry whose CDV UUID
 * does not resolve to a live, non-being-deleted CDV bdev.  This catches any
 * code path that creates an allocator entry without a matching bdev lifecycle
 * hook.
 *
 * Safe to call repeatedly; no-op when the hash is fully consistent.
 * Called from garbage_collect_as_needed() after block-device GC.
 */
void nvmeibt_cdv_alloc_gc_stale_entries(void);

/*
 * nvmeibt_cdv_alloc_remove — tear down the per-CDV allocator when a CDV is
 * deleted.
 *
 * Removes the entry for @cdv_uuid from the global hash and frees all
 * in-memory extent records.  The on-CDV metadata is not explicitly cleared —
 * it is discarded with the volume itself.
 *
 * Must be called from block_device_remove() for CDV blkdevs only.
 * Safe to call if the CDV was never allocated (no-op with an info log).
 */
void nvmeibt_cdv_alloc_remove(const char *cdv_uuid);

/*
 * nvmeibt_cdv_alloc_set_generation — update the allocator_generation for a CDV.
 *
 * Called from the RAFT distribution path when the leader assigns (or
 * reassigns) the allocator for cdv_uuid.  If the per-CDV allocator does not
 * exist yet it is created.  Clients whose cached generation no longer matches
 * will receive NVMEIBT_CDV_ALLOC_WRONG_GEN on their next ALLOC request.
 */
void nvmeibt_cdv_alloc_set_generation(const char *cdv_uuid, uint64_t generation);

/*
 * nvmeibt_cdv_alloc_elect — elect the CDV allocator TOMA for a CDV.
 *
 * Called by the RAFT leader for every stable CDV pRAID on each topo
 * recalculation — including on TOMA restart when topology has not changed.
 * This ensures the CDV alloc hash entry is always populated after startup.
 *
 * Election rule: if the current allocator is in @candidates, keep it (sticky)
 * and return 0.  Otherwise pick one from @candidates at random, increment
 * allocator_generation, and return 1 (newly elected — caller should push
 * CDV_ALLOCATOR_UPDATE to registrants).
 *
 * The on-disk extent records are scanned asynchronously on a worker thread
 * whenever ondisk_loaded is false, regardless of whether the sticky rule
 * fired.  If the disk I/O path is not yet ready, the scan is deferred and
 * retried on the next call (e.g. next heartbeat).
 *
 * @cdv_uuid:       CDV UUID string.
 * @candidates:     Array of TOMA node hostname strings (first-pRAID RW nodes).
 * @n_candidates:   Number of entries in @candidates.
 *
 * Returns  1 if a new allocator was elected (push_to_registrants needed),
 *          0 if the current allocator is sticky (no push needed),
 *         -EINVAL if n_candidates is 0, -ENOMEM on OOM.
 */
int nvmeibt_cdv_alloc_elect(const char *cdv_uuid,
			    const char **candidates,
			    int n_candidates);

/*
 * nvmeibt_cdv_alloc_get_allocator — retrieve the current allocator identity
 * for a CDV.
 *
 * @cdv_uuid:       CDV UUID string.
 * @out_toma_id:    Buffer of at least NVMEIBT_CDV_HOSTNAME_LEN bytes; filled
 *                  with the allocator hostname (empty string if unelected).
 * @out_generation: Filled with the current allocator_generation.
 *
 * Returns 0 on success (allocator elected), -ENOENT if no allocator exists
 * for this CDV.
 */
int nvmeibt_cdv_alloc_get_allocator(const char *cdv_uuid,
				    char *out_toma_id,
				    uint64_t *out_generation);

/*
 * nvmeibt_cdv_alloc_push_to_registrants — push CDV allocator identity to all
 * clients that have this CDV registered (attached).
 *
 * Sends a CDV_ALLOCATOR_UPDATE message carrying (allocator_toma_id,
 * allocator_generation) to every registrant of a disk segment belonging to
 * this CDV.  Called after nvmeibt_cdv_alloc_elect() succeeds.
 *
 * @cdv_uuid:  CDV UUID string.
 */
void nvmeibt_cdv_alloc_push_to_registrants(const char *cdv_uuid);

/*
 * Wire payload for the leader → chosen-allocator unicast RAFT_MSG_CDV_ALLOC_NOTIFY.
 * Fixed-size, packed; carried in raft_msg.persist_and_wire_buf.data[].
 */
struct nvmeibt_cdv_alloc_notify_payload {
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char     allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN];
	uint64_t allocator_generation;
} __attribute__((packed));

/*
 * nvmeibt_cdv_alloc_send_notify_to_elected — leader calls this after a
 * successful nvmeibt_cdv_alloc_elect() (returns 1).  Sends RAFT_MSG_CDV_ALLOC_NOTIFY
 * unicast to the chosen TOMA.  If the chosen TOMA is the leader itself, applies
 * the notify locally instead (same effect without going through the wire).
 */
void nvmeibt_cdv_alloc_send_notify_to_elected(const char *cdv_uuid,
					      const char *allocator_toma_id,
					      uint64_t    allocator_generation);

/*
 * nvmeibt_cdv_alloc_handle_notify — receiver: called from the RAFT dispatch
 * on RAFT_MSG_CDV_ALLOC_NOTIFY.  Applies the monotonicity guard, updates the
 * local alloc entry, schedules the on-disk scan, persists the identity to the
 * CDV header, and pushes CDV_ALLOCATOR_UPDATE to local registrants.
 */
void nvmeibt_cdv_alloc_handle_notify(const struct nvmeibt_cdv_alloc_notify_payload *payload);

/*
 * nvmeibt_cdv_alloc_push_all_to_new_registrant — unicast CDV_ALLOCATOR_UPDATE
 * for every known elected CDV allocator to a single newly-registered client.
 *
 * Called after a successful RT_REGISTER_DISK_SEGMENT so that clients which
 * register after the initial election still receive allocator identity.
 *
 * @reg_ctx:  The newly-active registrant to notify.
 */
struct nvmeibt_registrant_ctx;
void nvmeibt_cdv_alloc_push_all_to_new_registrant(struct nvmeibt_registrant_ctx *reg_ctx);

/*
 * nvmeibt_cdv_alloc_free_all_for_tpv — queue zeroing of all CDV data extents
 * owned by a deleted TPV before making them available for reuse.
 *
 * Called from the Kafka CDVAllocatorFreeAll handler when a TPV is deleted.
 * The function:
 *   1. Finds (or creates) the per-CDV allocator; scans on-disk state if not yet
 *      loaded (handles TOMA-restart-before-handler race).
 *   2. Stores the CDV geometry (@allocator_size_gb, @cdv_extent_size_mb) in
 *      the allocator struct for use by the background zero worker.
 *   3. Iterates the in-memory extent list; for each extent owned by @tpv_uuid:
 *      - sets entry->needs_zeroing = true and increments n_pending_zeroing
 *      - writes an ALLOCATED|NEEDS_ZEROING ondisk record (geometry in reserved2)
 *      - dispatches a background zero write for the full CDV data extent
 *      - does NOT remove the entry from the list or decrement n_allocated —
 *        the entry blocks reallocation until zeroing completes
 *   4. Rewrites the allocator header.
 *
 * When the background zero finishes for each extent, the finalize callback
 * writes a free ondisk record (NULL tpv_uuid), removes the entry from the list,
 * decrements n_allocated and n_pending_zeroing, and triggers a header rewrite.
 *
 * Returns 0 on success.  On disk-resolve failure returns a negative errno.
 * Individual zero-dispatch failures are logged but do not abort remaining extents.
 *
 * Must be called from TOMA's single main thread (or with the topology lock held).
 */
int nvmeibt_cdv_alloc_free_all_for_tpv(const char *cdv_uuid,
					const char *tpv_uuid,
					uint32_t    allocator_size_gb,
					uint32_t    cdv_extent_size_mb);

/*
 * nvmeibt_cdv_alloc_startup_scan — log in-memory state.
 *
 * Iterates all CDV allocators, logs per-CDV statistics, and emits
 * CDVCapacityWarning events for any CDV whose capacity is already known
 * (total_data_extents > 0) and above the warning threshold.
 */
void nvmeibt_cdv_alloc_startup_scan(void);

/* ── CDV wire-format structs (TOMA-local copies) ─────────────────────────────
 *
 * Mirror the definitions in clnt/nvmeibc_msgs_shared.h so that TOMA
 * (user-space) can decode/encode CDV request and response payloads
 * without pulling in the kernel client header chain.
 * Field layout MUST stay in sync with the client-side definitions.
 */

/* CDV_ALLOC_EXTENT request */
struct nvmeibt_cdv_alloc_req {
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	uint64_t req_id;
	uint64_t client_generation;
	uint64_t total_data_extents;	/* CDV data capacity in extents; TOMA uses for full-check */
};

/* CDV_ALLOC_EXTENT response status codes */
enum nvmeibt_cdv_alloc_status {
	NVMEIBT_CDV_ALLOC_OK		= 0,
	NVMEIBT_CDV_ALLOC_CDV_FULL	= 1,
	NVMEIBT_CDV_ALLOC_WRONG_GEN	= 2,
	NVMEIBT_CDV_ALLOC_ERROR		= 3,
};

/* CDV_ALLOC_EXTENT response */
struct nvmeibt_cdv_alloc_resp {
	uint64_t req_id;
	uint64_t extent_index;
	uint64_t allocator_generation;
	uint8_t  status;			/* enum nvmeibt_cdv_alloc_status */
};

/* CDV_FREE_EXTENT request (fire-and-forget; no dedicated response) */
struct nvmeibt_cdv_free_req {
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	uint64_t extent_index;
};

/* CDV_LIST_EXTENTS request */
struct nvmeibt_cdv_list_req {
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	uint64_t req_id;
};

/* CDV_LIST_EXTENTS response header; followed by n_extents × uint64_t */
struct nvmeibt_cdv_list_resp {
	uint64_t req_id;
	uint64_t n_extents;
	uint8_t  status;
};

/*
 * CDV_ALLOCATOR_UPDATE — TOMA → client push.
 *
 * Sent to every client registrant of a CDV when the allocator identity is
 * first elected or changes.  The client stores (allocator_toma_id, generation)
 * in its per-TPV struct and uses it for CDV_ALLOC_EXTENT requests.
 *
 * Field layout MUST stay in sync with struct nvmeibc_cdv_allocator_update
 * in clnt/nvmeibc_msgs_shared.h.
 */
struct nvmeibt_cdv_allocator_update {
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char     allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN];
	uint64_t allocator_generation;
};

/* ── Status / observability ──────────────────────────────────────────────── */

/*
 * nvmeibt_cdv_alloc_print_status — print CDV extent allocator state.
 *
 * For each CDV known to this TOMA node, prints: cdv_uuid, allocator_toma_id,
 * n_allocated, total_data_extents (if known), allocator_generation, and
 * capacity percentage.
 * Called from print_status_str() for NVMEIBS_TOMA_STATUS_ALL and
 * NVMEIBS_TOMA_STATUS_CDV.
 */
void nvmeibt_cdv_alloc_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

/*
 * nvmeibt_cdv_alloc_print_status_detailed — per-CDV extent table dump.
 *
 * For each CDV, prints every allocated extent entry (extent_index + tpv_uuid).
 * Called from print_status_str() for NVMEIBS_TOMA_STATUS_CDV_DETAILED.
 */
void nvmeibt_cdv_alloc_print_status_detailed(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

/* ── Incoming-message handler (wired from nvmeibt_client.c dispatch) ─────── */

struct nvmeibt_register_msg;	/* forward; defined in nvmeibt_register.h */

/*
 * nvmeibt_cdv_handle_incoming_msg — dispatch CDV_ALLOC/FREE/LIST messages.
 *
 * Called from nvmeibt_client_handle_incoming_message() when the message
 * signature is NVMEIBT_PROTOCOL_SIGNATURE_CDV.  The request payload is in
 * msg->msg_data with length msg->data_length.  Responses are sent back
 * via nvmeibt_toma_send_msg_to_client().
 *
 * Returns 0 on success, negative on dispatch/handler error.
 */
int nvmeibt_cdv_handle_incoming_msg(struct nvmeibt_register_msg *msg);

#endif /* NVMEIBT_CDV_ALLOC_H */
