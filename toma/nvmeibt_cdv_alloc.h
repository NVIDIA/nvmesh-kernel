/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 * nvmeibt_cdv_alloc.h — CDV extent allocator: in-memory state.
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
 * CDV allocator identity (allocator_toma_id, allocator_generation) is elected
 * by the RAFT leader via nvmeibt_cdv_alloc_elect() and distributed to all
 * TOMAs and clients:
 *   - TOMAs: via RAFT replication (nvmeibt_cdv_alloc_set_generation called
 *     during RAFT log replay on followers).
 *   - Clients: via CDV_ALLOCATOR_UPDATE messages pushed to all registrants
 *     (nvmeibt_cdv_alloc_push_to_registrants).
 *
 * Extent allocation state is distributed and persisted via RAFT; there is no
 * local persistence file.  On startup, in-memory state is rebuilt from RAFT
 * log replay.
 *
 * Threading:
 *   All public functions must be called from TOMA's single main thread (or
 *   with the global topology lock held).  No internal locks are taken.
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
	XDLIST_DECLARE(, struct nvmeibt_cdv_extent_entry, link) extents;
};

/* ── One-time init / shutdown ────────────────────────────────────────────── */

/*
 * nvmeibt_cdv_alloc_one_time_init — allocate the global hash table.
 * Called from nvmeibt_toma_init(), after nvmeibt_local_disk_one_time_init().
 * Returns 0 on success, negative errno on fatal hash-create failure.
 * In-memory state is rebuilt by RAFT log replay after init completes.
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
 * Called by the RAFT leader when a CDV is first seen, or when the current
 * allocator TOMA leaves the RAFT group.
 *
 * Election rule: if the current allocator is in @candidates, keep it (sticky).
 * Otherwise pick one from @candidates at random.
 *
 * @cdv_uuid:       CDV UUID string.
 * @candidates:     Array of TOMA node hostname strings (first-pRAID RW nodes).
 * @n_candidates:   Number of entries in @candidates.
 *
 * On success, sets allocator_toma_id and increments allocator_generation
 * in the per-CDV allocator.  Returns 0 on success, -EINVAL if n_candidates
 * is 0, -ENOMEM on OOM.
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
 * nvmeibt_cdv_alloc_startup_scan — log in-memory state.
 *
 * Called after RAFT log replay has populated the allocators.
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
	char tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];
	char cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
};

/* CDV_LIST_EXTENTS response header; followed by n_extents × uint64_t */
struct nvmeibt_cdv_list_resp {
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
