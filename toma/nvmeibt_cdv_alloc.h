/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 * nvmeibt_cdv_alloc.h — CDV extent allocator: in-memory state and persistence.
 *
 * TOMA maintains, for each CDV (Carrier Direct Volume), a table of which data
 * CDV_extents have been allocated to which TPV (Thin-Provisioned Volume).
 *
 * Module responsibilities:
 *   - nvmeibt_cdv_extent_md: on-disk / persistence record (one per allocated
 *     CDV_extent).
 *   - nvmeibt_cdv_extent_entry: in-memory per-extent node, owned by a per-CDV
 *     allocator.
 *   - nvmeibt_cdv_alloc: per-CDV allocator, held in a global hash table keyed
 *     by CDV UUID string.
 *   - Flat binary persistence file written atomically (write + rename) after
 *     every ALLOC/FREE and loaded once at TOMA startup.
 *
 * Threading:
 *   All public functions must be called from TOMA's single main thread (or
 *   with the global topology lock held).  No internal locks are taken.
 *
 * Persistence file:
 *   TOMA_DIR_OPT_NVMESH "/toma/cdv_alloc.bin"
 *   Format: nvmeibt_cdv_alloc_file_hdr + n_records × nvmeibt_cdv_extent_md.
 *   Written atomically via a .tmp sibling + rename(2).
 *   Loss of this file is non-fatal: allocated extents become orphans, recovered
 *   by NVCK on the next maintenance scan.
 */

#ifndef NVMEIBT_CDV_ALLOC_H
#define NVMEIBT_CDV_ALLOC_H

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"

/* ── UUID string length (matches NVMEIBC_BD_UUID_LEN on the client side) ── */
#define NVMEIBT_CDV_UUID_STRLEN  64

/* ── On-disk persistence record for one allocated CDV_extent ───────────────
 *
 * One record is written per allocated CDV_extent.  Free extents are
 * represented by absence — no "free" records are stored.
 */
#define NVMEIBT_CDV_EXTENT_MD_MAGIC   UINT64_C(0x4344565f4558544d) /* "CDV_EXTM" */
#define NVMEIBT_CDV_EXTENT_MD_VERSION 1

struct nvmeibt_cdv_extent_md {
	uint64_t magic;                             /*   8 */
	uint32_t version;                           /*  12 */
	uint32_t _pad;                              /*  16 */
	uint64_t extent_index;                      /*  24 */
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN]; /*  88 */
	char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN]; /* 152 */
} __attribute__((packed));

/* ── Persistence file header ────────────────────────────────────────────────
 *
 * Followed immediately by n_records × nvmeibt_cdv_extent_md.
 */
#define NVMEIBT_CDV_ALLOC_FILE_MAGIC   UINT64_C(0x4e564d435f434456) /* "NVMC_CDV" */
#define NVMEIBT_CDV_ALLOC_FILE_VERSION 1

struct nvmeibt_cdv_alloc_file_hdr {
	uint64_t magic;     /*  8 */
	uint32_t version;   /* 12 */
	uint32_t _pad;      /* 16 */
	uint64_t n_records; /* 24 */
} __attribute__((packed));

/* ── In-memory per-extent node ──────────────────────────────────────────────
 *
 * Lives on nvmeibt_cdv_alloc.extents.  One node per allocated CDV_extent.
 * The 'link' field is the XDLIST embedded link; must match the field name
 * in the XDLIST_DECLARE in nvmeibt_cdv_alloc.
 */
struct nvmeibt_cdv_extent_entry {
	uint64_t   extent_index;
	char       tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	struct xdlist link;
};

/*
 * Capacity watermarks for CDVCapacityWarning Kafka events.
 * A warning is emitted when usage rises to WARN_PCT; the flag clears when
 * usage falls back below WARN_CLEAR_PCT (hysteresis prevents Kafka floods).
 */
#define NVMEIBT_CDV_WARN_PCT       90   /* fire warning at >= 90% used */
#define NVMEIBT_CDV_WARN_CLEAR_PCT 85   /* clear flag when usage drops below 85% */

/* ── Per-CDV allocator ──────────────────────────────────────────────────────
 *
 * One instance per CDV that has had at least one extent allocated.
 * Stored in the global hash table keyed by cdv_uuid (ASCII string).
 * cdv_uuid must be the first field: nvmeib_hash_add_ascii_str requires the
 * key to be a pointer into the object itself.
 */
struct nvmeibt_cdv_alloc {
	char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN]; /* hash key; must be first field */
	uint64_t n_allocated;
	uint64_t total_data_extents;	/* CDV capacity in data extents; populated from first ALLOC */
	uint64_t allocator_generation;	/* current epoch; echoed in ALLOC responses; not persisted */
	bool     capacity_warning_sent;	/* true after CDVCapacityWarning sent; cleared on hysteresis */
	XDLIST_DECLARE(, struct nvmeibt_cdv_extent_entry, link) extents;
};

/* ── One-time init / shutdown ────────────────────────────────────────────── */

/*
 * nvmeibt_cdv_alloc_one_time_init — allocate the global hash and call
 * nvmeibt_cdv_alloc_load() to restore persisted state.
 * Called from nvmeibt_toma_init(), after nvmeibt_local_disk_one_time_init().
 * Returns 0 on success, negative errno on fatal hash-create failure.
 * A load failure is logged but treated as non-fatal (starts fresh).
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
 * been allocated to tpv_uuid and persist immediately.
 * Returns 0 on success, negative errno on OOM or I/O error.
 */
int nvmeibt_cdv_alloc_add_extent(const char *cdv_uuid,
				 uint64_t    extent_index,
				 const char *tpv_uuid);

/*
 * nvmeibt_cdv_alloc_remove_extent — mark extent_index in cdv_uuid as free and
 * persist immediately.
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

/* ── Persistence (also callable for an explicit flush) ──────────────────── */
int nvmeibt_cdv_alloc_persist(void);
int nvmeibt_cdv_alloc_load(void);

/*
 * nvmeibt_cdv_alloc_startup_scan — log restored state after load.
 *
 * Called from nvmeibt_cdv_alloc_one_time_init() after a successful load.
 * Iterates all loaded CDV allocators, logs per-CDV statistics, and emits
 * CDVCapacityWarning events for any CDV whose capacity is already known
 * (total_data_extents > 0) and above the warning threshold.
 * At startup total_data_extents is typically 0 (not persisted); capacity
 * checks are deferred to the first ALLOC request per CDV.
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
	char tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
};

/* CDV_LIST_EXTENTS response header; followed by n_extents × uint64_t */
struct nvmeibt_cdv_list_resp {
	uint64_t n_extents;
	uint8_t  status;
};

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
