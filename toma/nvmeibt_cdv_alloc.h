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
 * by the RAFT leader via nvmeibt_cdv_alloc_elect() and carried in the
 * RAFT-replicated pRAID topology record.  Delivery to every TOMA happens via
 * the standard AppendEntries → follower-apply pipeline; the role transition
 * (promote / demote) runs in nvmeibt_cdv_alloc_on_topo_applied(), which fires
 * Stage A of the satellite-attach handshake on the newly-elected allocator
 * and tears down local state on the demoted one.  Clients learn the new
 * identity via the existing CDV topology push (CDV_ALLOCATOR_UPDATE).
 * See design/EmbedAllocatorInRaft.md.
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
 * (the "allocator region", bytes 0 to allocator_size_gib * 1 GiB - 1).
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
	uint32_t zeroing_allocator_size_gib;
	uint32_t zeroing_cdv_extent_size_mib;
	uint8_t  reserved2[CDV_ONDISK_BLOCK_SIZE - 1 - 7 - 64 - 4 - 4 - 4];
} __attribute__((__packed__));

/* ── Per-CDV allocator ──────────────────────────────────────────────────────
 *
 * One instance per CDV that has had at least one extent allocated.
 * Stored in the global hash table (cdv_alloc_hash, an XHASHTABLE) keyed by
 * xhash_str_to_32_bits(cdv_uuid).  The hash_link field is the embedded link
 * node for the XHASHTABLE; cdv_uuid is used for exact-match within a bucket.
 */
/* Maximum hostname length — must match NVMEIB_HOST_NAME_LEN (64) on the client side. */
#define NVMEIBT_CDV_HOSTNAME_LEN  64

/*
 * Allocator state machine on the elected TOMA.  See SatelliteVolumeForCDVAlloc.md
 * §3.2 for the full election handoff protocol.
 *
 *   NOT_ALLOCATOR
 *      │ RAFT-leader picks this TOMA as allocator (handle_notify())
 *      ▼
 *   AWAITING_SATELLITE_ATTACH
 *      │ Stage A: AttachSatelliteRequest enqueued to management
 *      │ Stage B: AttachSatelliteResponse(OK) received, satellite opened, scan run
 *      ▼
 *   ACTIVE
 *      │ Reservation preempt observed on the satellite (next allocator elected)
 *      ▼
 *   NOT_ALLOCATOR
 *
 * In NOT_ALLOCATOR and AWAITING_SATELLITE_ATTACH states, ALLOC requests are
 * answered with WRONG_GEN so the client retries until the topology push
 * announces the new allocator generation.
 */
enum nvmeibt_cdv_alloc_state {
	NVMEIBT_CDV_ALLOC_STATE_NOT_ALLOCATOR = 0,
	NVMEIBT_CDV_ALLOC_STATE_AWAITING_SATELLITE_ATTACH,
	NVMEIBT_CDV_ALLOC_STATE_ACTIVE,
};

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
					 * forces one retry to guard against satellite-not-yet-online
					 * during simultaneous client+TOMA restart */
	uint32_t scan_retry_delay_ms;	/* 0 on first attempt; 100 → 1000 backoff on failure
					 * while this TOMA is the elected allocator and
					 * ondisk_loaded is still false */
	/*
	 * Allocator state machine and satellite-volume binding.  When this TOMA is
	 * the elected allocator (state != NOT_ALLOCATOR), the allocator metadata
	 * (header + cdv_extent_md[] records) lives on the satellite volume named
	 * '<cdv-name>-mgmt', NOT on the CDV itself.  The satellite is exclusively
	 * attached to this TOMA via the management Kafka path; see Phase 2 of
	 * SatelliteVolumeForCDVAlloc.md.
	 */
	enum nvmeibt_cdv_alloc_state state;
	char     satellite_uuid[NVMEIBT_CDV_UUID_STRLEN]; /* satellite volume's UUID */
	char     satellite_dev_path[80]; /* /dev/nvmesh/<cdvName>-mgmt */
	int      satellite_fd;		/* cached fd; opened/used ONLY from io_wq worker thread */
	uint64_t satellite_attach_request_id; /* idempotency key for the in-flight request */
	uint64_t satellite_reservation_version; /* from AttachSatelliteResponse, for fencing */
	/*
	 * CDV geometry — cached on first free_all_for_tpv call and on scan.
	 * Needed by the background zero worker to compute per-extent byte offsets
	 * when re-dispatching zeroing after a TOMA restart.
	 */
	uint32_t allocator_size_gib;	/* size of on-CDV allocator region in GiB */
	uint32_t cdv_extent_size_mib;	/* size of each data CDV extent in MiB */
	uint64_t n_pending_zeroing;	/* extents with needs_zeroing=true (not yet re-usable) */
	int      cdv_fd;		/* cached fd; opened/used ONLY from io_wq worker thread.
					 * Currently still used by cdv_zero_execute (data-extent
					 * zeroing on free) — that path requires the CDV itself to
					 * be attached to this TOMA, which is no longer the
					 * default after the satellite-volume migration.
					 * If cdv_extent_zero_on_free is enabled, the operator
					 * must arrange for the CDV to be attached here too. */
	char     dev_path[80];		/* /dev/nvmesh/<name>; resolved on main thread */
	struct nvmeibt_wq *io_wq;	/* per-satellite I/O work queue (scan + writes); lifetime tied
					 * to this TOMA's allocator role on the CDV */
	XDLIST_DECLARE(, struct nvmeibt_cdv_extent_entry, link) extents;
	struct xdlist hash_link;	/* embedded link for cdv_alloc_hash (XHASHTABLE) */

	/*
	 * Per-client CDV preempt (TPV_PerClientCDVPreemption.md §2.10):
	 *
	 *   admission_floor — monotonic u64. New REGISTERs on CDV segments with
	 *     reservation_mode_version < admission_floor are rejected with
	 *     NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR. Raised by the
	 *     preempt_client_from_cdv Kafka handler under handler_lock, and
	 *     seeded by the CDV topology push. Existing registrants are NOT
	 *     reconsulted against the floor — survivor immunity is deliberate.
	 *   admission_floor_seeded — tracks whether either seeding path (topology
	 *     push, or first AttachVolumes fallback) has run; guards against
	 *     overwrites from later-arriving stale AttachVolumes payloads.
	 *   handler_lock — serializes the preempt handler against the new
	 *     REGISTER predicate in nvmeibt_register.c. Both paths acquire this
	 *     lock BEFORE any per-seg_active lock. Lock order invariant:
	 *       handler_lock → seg_active locks
	 *     Never reverse.
	 */
	uint64_t         admission_floor;
	bool             admission_floor_seeded;
	pthread_mutex_t  handler_lock;
};

/* ── Runtime config ─────────────────────────────────────────────────────────
 *
 * cdv_extent_zero_on_free — if non-zero, CDV data extents freed by a deleted
 * TPV are zeroed (background WQ) before being made available for reuse.
 * Default 0 (zero-on-free disabled): extents are freed immediately.  The
 * NEEDS_ZEROING on-disk bit is still honored on restart regardless of this
 * flag, so toggling it off after a restart will not strand extents that were
 * marked for zeroing while it was on.
 *
 * Exposed via toma_rpc as the "cdv_extent_zero_on_free" parameter.
 */
extern int64_t nvmeibt_cdv_extent_zero_on_free;

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
 * ── Per-client CDV preempt (§2.10) ──────────────────────────────────────────
 *
 * nvmeibt_cdv_alloc_lookup — return the per-CDV state entry if present.
 * Safe to call from any context holding the cdv_alloc_hash read barrier.
 */
struct nvmeibt_cdv_alloc *nvmeibt_cdv_alloc_lookup(const char *cdv_uuid);

/*
 * nvmeibt_cdv_alloc_lookup_or_create_with_floor — return the per-CDV state
 * entry, creating it on the fly if not present and seeding admission_floor.
 *
 * Called from the preempt_client_from_cdv Kafka handler when the message
 * arrives before the CDV topology push has reached this TOMA. newFloor is
 * management's authoritative value, so seeding from it is correct.
 *
 * Returns NULL only on allocation failure.
 */
struct nvmeibt_cdv_alloc *nvmeibt_cdv_alloc_lookup_or_create_with_floor(
		const char *cdv_uuid, uint64_t initial_floor);

/*
 * nvmeibt_cdv_alloc_seed_floor — set the admission floor if not yet seeded,
 * or max-merge with the existing value. Path A (topology push) uses this with
 * the full authoritative floor. Path B (first AttachVolumes for the CDV) uses
 * this only when admission_floor_seeded is still false, to close the window
 * where a REGISTER arrives before the topology push.
 *
 * Safe to call without handler_lock — updates are monotonic via max_t.
 */
void nvmeibt_cdv_alloc_seed_floor(struct nvmeibt_cdv_alloc *alloc,
				  uint64_t floor, bool from_topology);

/*
 * nvmeibt_seg_active_get_cdv_alloc — return the per-CDV state entry for this
 * segment's parent CDV, or NULL if the segment's parent volume is not a CDV
 * (or the applied topology isn't yet available).
 *
 * Navigates the applied topology:
 *   seg_active → applied_seg_lot → praid_lot->my_praid → blkdev (via
 *   praid_mgmt.its_chunk.its_block_device) → from_config.is_cdv +
 *   urn_uuid.str → nvmeibt_cdv_alloc_lookup.
 *
 * Safe to call from the REGISTER admission path (read-only on the topology
 * chain). Used by check_cdv_admission_floor in nvmeibt_register.c and by the
 * preempt-client outer walk in nvmeibt_cdv_alloc.c.
 */
struct nvmeibt_seg_active;
struct nvmeibt_cdv_alloc *nvmeibt_seg_active_get_cdv_alloc(
		struct nvmeibt_seg_active *seg_active);

/*
 * nvmeibt_cdv_alloc_preempt_client — handle the preemptClientFromCDV Kafka
 * message. Raises admission_floor FIRST (order invariant — see §2.10.4),
 * then terminates the named client's registrants on every CDV segment under
 * handler_lock. On completion, caller publishes the response with the value
 * returned in *out_terminated.
 *
 * Returns 0 on success (even if no reg_ctx matched — idempotent no-op).
 * Returns -ENOMEM on allocation failure in the create-on-the-fly path.
 */
int nvmeibt_cdv_alloc_preempt_client(const char *cdv_uuid,
				     const char *client_id,      /* client hostname */
				     uint64_t    new_floor,
				     uint32_t   *out_terminated);

/*
 * nvmeibt_cdv_alloc_send_preempt_response — publish preemptClientFromCDVResponse
 * to management after the preempt handler completes. Management aggregates
 * ACKs across all TOMAs before clearing EVICTING.
 */
void nvmeibt_cdv_alloc_send_preempt_response(const char *cdv_uuid,
					     const char *client_id,      /* client hostname */
					     uint64_t    new_floor,
					     bool        success,
					     uint32_t    terminated,
					     const char *error);

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

/* Forward-declared here to avoid pulling praid_basics.h into this header. */
struct nvmeibt_praid_topo_ctx;

/*
 * nvmeibt_cdv_alloc_elect — elect the CDV allocator TOMA for a CDV.
 *
 * Called by the RAFT leader for every stable CDV pRAID during topology
 * recalculation.  Operates purely on the caller-supplied topo_ctx (i.e. the
 * pRAID's calculated_praid_lot.topo_ctx that is about to be committed via
 * AppendEntries): reads the current (allocator_toma_id, allocator_generation)
 * fields and, on new election, stages the new identity by writing them.
 *
 * This function does NOT touch the local cdv_alloc_hash.  Identity propagation
 * happens entirely through the RAFT-replicated pRAID topology: every TOMA's
 * update_applied_topology path invokes nvmeibt_cdv_alloc_on_topo_applied,
 * which drives the promote/demote state transitions.
 *
 * Election rule: if the current allocator_toma_id in @topo_ctx is still in
 * @candidates, keep it (sticky) and return 0.  Otherwise pick one candidate
 * (random tie-break), write allocator_toma_id + (allocator_generation+1) into
 * @topo_ctx, and return 1.
 *
 * @cdv_uuid:      CDV UUID string (for logging only).
 * @candidates:    Array of TOMA node hostname strings (alive RAFT members).
 * @n_candidates:  Number of entries in @candidates.
 * @topo_ctx:      The pRAID topo_ctx to read from and stage mutations into
 *                 (typically &praid_leader->calculated_praid_lot.topo_ctx).
 *                 Must be non-NULL.
 *
 * Returns  1 if a new allocator was elected (caller should mark the pRAID
 *            topology as changed so the commit reaches all followers);
 *          0 if the current allocator is sticky (no change);
 *         -EINVAL if @topo_ctx is NULL or @n_candidates == 0.
 */
int nvmeibt_cdv_alloc_elect(const char *cdv_uuid,
			    const char **candidates,
			    int n_candidates,
			    struct nvmeibt_praid_topo_ctx *topo_ctx);

/*
 * nvmeibt_cdv_alloc_on_topo_applied — hook fired from update_applied_topology
 * on every committed topology change, on every TOMA (leader and followers).
 *
 * Called only for the first pRAID of a CDV (caller filters).  Diffs the
 * previously-applied (allocator_toma_id, allocator_generation) against the
 * newly-applied values and drives local role transitions:
 *  - Generation increased and allocator_toma_id == my_hostname, state not
 *    ACTIVE → promote to AWAITING_SATELLITE_ATTACH and fire Stage A
 *    (cdv_send_attach_satellite_request).
 *  - Generation increased and previous allocator was me, new one isn't →
 *    demote to NOT_ALLOCATOR, tear down satellite state.
 *  - Generation unchanged → no-op.
 *
 * Also triggers CDV_ALLOCATOR_UPDATE push to any clients registered locally
 * so they learn the new allocator identity.
 */
void nvmeibt_cdv_alloc_on_topo_applied(const char *cdv_uuid,
				       const struct nvmeibt_praid_topo_ctx *prev_applied,
				       const struct nvmeibt_praid_topo_ctx *new_applied);

/*
 * nvmeibt_cdv_alloc_push_to_registrants — push CDV allocator identity to all
 * clients that have this CDV registered (attached) on this TOMA.
 *
 * Sends a CDV_ALLOCATOR_UPDATE message carrying (allocator_toma_id,
 * allocator_generation) to every registrant of a disk segment belonging to
 * this CDV.  Called from the topology-apply hook when the allocator identity
 * changes for this CDV.
 *
 * @cdv_uuid:  CDV UUID string.
 */
void nvmeibt_cdv_alloc_push_to_registrants(const char *cdv_uuid);

/*
 * nvmeibt_cdv_alloc_handle_satellite_attach_response — Stage B handler.
 *
 * Called from the Kafka dispatch on attachSatelliteResponse (management → TOMA).
 * On status OK with a matching cdv_uuid + request_id: opens the satellite
 * volume's block device, dispatches the on-disk scan, and on scan completion
 * promotes the state to ACTIVE and pushes CDV_ALLOCATOR_UPDATE to registrants.
 *
 * On non-OK status (STALE_GENERATION, CDV_NOT_FOUND, CDV_BEING_DELETED,
 * INTERNAL_ERR): logs and either retries (transient) or tears down the local
 * allocator entry (terminal).
 *
 * @cdv_uuid:                 parent CDV UUID (echoed from the request)
 * @request_id:               idempotency key (echoed from the request)
 * @status:                   "OK" / "STALE_GENERATION" / "CDV_NOT_FOUND" / etc.
 * @satellite_uuid:           satellite volume UUID (only used on OK)
 * @reservation_version:      satellite reservation version after preempt
 * @allocator_generation:     echoed from the request
 */
void nvmeibt_cdv_alloc_handle_satellite_attach_response(
	const char *cdv_uuid,
	uint64_t    request_id,
	const char *status,
	const char *satellite_uuid,
	uint64_t    reservation_version,
	uint64_t    allocator_generation);

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
 *   2. Stores the CDV geometry (@allocator_size_gib, @cdv_extent_size_mib) in
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
					uint32_t    allocator_size_gib,
					uint32_t    cdv_extent_size_mib);

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
