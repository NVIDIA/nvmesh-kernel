/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 * nvmeibt_cdv_alloc.c — CDV extent allocator: in-memory state and persistence.
 *
 * See nvmeibt_cdv_alloc.h for a full description.
 *
 * Persistence layout (TOMA_DIR_OPT_NVMESH "/toma/cdv_alloc.bin"):
 *
 *   [nvmeibt_cdv_alloc_file_hdr]      24 bytes
 *   [nvmeibt_cdv_extent_md] × n       152 bytes each
 *
 * Written atomically: temp file → rename(2).
 * Loaded once at startup to reconstruct in-memory state.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nvmeibt_cdv_alloc.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_register.h"		/* struct nvmeibt_register_msg */
#include "nvmeibt_toma.h"		/* nvmeibt_toma_send_msg_to_client */
#include "../common/nvmeib_hash.h"

/* ── Persistence paths ───────────────────────────────────────────────────── */

#define CDV_ALLOC_PATH     (TOMA_DIR_OPT_NVMESH "/toma/cdv_alloc.bin")
#define CDV_ALLOC_TMP_PATH (TOMA_DIR_OPT_NVMESH "/toma/cdv_alloc.bin.tmp")

/* ── Global state ────────────────────────────────────────────────────────── */

/* cdv_uuid (ASCII string) → nvmeibt_cdv_alloc * */
static struct nvmeib_hash_table *cdv_alloc_hash;

/* ── Internal: insert an entry without immediately persisting ───────────── */

/*
 * cdv_alloc_insert — find (or create) the per-CDV allocator and append a new
 * extent entry to it.  Does NOT call persist().  Returns 0 on success,
 * negative errno on OOM.
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
		N_Tf(cdv_alloc_new_cdv, "CDV-alloc: new per-CDV allocator cdv=@STR", cdv_uuid);
	}

	/* Guard against duplicate extent_index (corrupted file, replayed alloc). */
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

	return nvmeibt_cdv_alloc_persist();
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
		return nvmeibt_cdv_alloc_persist();
	}

	N_Wf(cdv_alloc_rm_notfound,
	     "CDV-alloc: extent not found cdv=@STR idx=@LLU",
	     cdv_uuid, extent_index);
	return -ENOENT;
}

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
	if (!alloc)
		return 0;   /* no extents from this CDV — not an error */

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

/* ── Persistence ─────────────────────────────────────────────────────────── */

int nvmeibt_cdv_alloc_persist(void)
{
	struct nvmeibt_cdv_alloc_file_hdr hdr;
	struct nvmeibt_cdv_extent_md      rec;
	struct nvmeibt_cdv_alloc         *alloc;
	struct nvmeibt_cdv_extent_entry  *entry;
	uint64_t n_total = 0;
	FILE *f;

	/* Count total records across all CDVs. */
	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash)
		n_total += alloc->n_allocated;

	f = fopen(CDV_ALLOC_TMP_PATH, "wb");
	if (!f) {
		N_Ef(cdv_alloc_persist_open,
		     "CDV-alloc: fopen cdv_alloc.bin.tmp failed errno=@INT", errno);
		return -errno;
	}

	hdr.magic     = NVMEIBT_CDV_ALLOC_FILE_MAGIC;
	hdr.version   = NVMEIBT_CDV_ALLOC_FILE_VERSION;
	hdr._pad      = 0;
	hdr.n_records = n_total;

	if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
		N_Ef(cdv_alloc_persist_whdr, "CDV-alloc: header write failed errno=@INT", errno);
		fclose(f);
		return -EIO;
	}

	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash) {
		XDLIST_FOREACH(entry, &alloc->extents) {
			rec.magic        = NVMEIBT_CDV_EXTENT_MD_MAGIC;
			rec.version      = NVMEIBT_CDV_EXTENT_MD_VERSION;
			rec._pad         = 0;
			rec.extent_index = entry->extent_index;
			memcpy(rec.cdv_uuid, alloc->cdv_uuid, NVMEIBT_CDV_UUID_STRLEN);
			memcpy(rec.tpv_uuid, entry->tpv_uuid,  NVMEIBT_CDV_UUID_STRLEN);
			if (fwrite(&rec, sizeof(rec), 1, f) != 1) {
				N_Ef(cdv_alloc_persist_wrec,
				     "CDV-alloc: record write failed errno=@INT", errno);
				fclose(f);
				return -EIO;
			}
		}
	}

	if (fclose(f) != 0) {
		N_Ef(cdv_alloc_persist_close, "CDV-alloc: fclose failed errno=@INT", errno);
		return -EIO;
	}

	if (rename(CDV_ALLOC_TMP_PATH, CDV_ALLOC_PATH) != 0) {
		N_Ef(cdv_alloc_persist_rename,
		     "CDV-alloc: rename to cdv_alloc.bin failed errno=@INT", errno);
		return -errno;
	}

	N_Tf(cdv_alloc_persist_ok, "CDV-alloc: persisted @LLU records", n_total);
	return 0;
}

int nvmeibt_cdv_alloc_load(void)
{
	struct nvmeibt_cdv_alloc_file_hdr hdr;
	struct nvmeibt_cdv_extent_md      rec;
	uint64_t i;
	int      rv;
	FILE    *f;

	f = fopen(CDV_ALLOC_PATH, "rb");
	if (!f) {
		if (errno == ENOENT) {
			N_If(cdv_alloc_load_fresh,
			     "CDV-alloc: no persistence file; starting fresh");
			return 0;
		}
		N_Ef(cdv_alloc_load_open,
		     "CDV-alloc: fopen cdv_alloc.bin failed errno=@INT", errno);
		return -errno;
	}

	if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
		N_Ef(cdv_alloc_load_rhdr, "CDV-alloc: header read failed errno=@INT", errno);
		fclose(f);
		return -EIO;
	}

	if (hdr.magic != NVMEIBT_CDV_ALLOC_FILE_MAGIC) {
		N_Ef(cdv_alloc_load_magic,
		     "CDV-alloc: bad magic @LLU_TX in cdv_alloc.bin; ignoring file",
		     hdr.magic);
		fclose(f);
		return -EINVAL;
	}

	if (hdr.version != NVMEIBT_CDV_ALLOC_FILE_VERSION) {
		N_Ef(cdv_alloc_load_ver,
		     "CDV-alloc: unknown version @UINT in cdv_alloc.bin; ignoring file",
		     hdr.version);
		fclose(f);
		return -EINVAL;
	}

	for (i = 0; i < hdr.n_records; i++) {
		if (fread(&rec, sizeof(rec), 1, f) != 1) {
			N_Ef(cdv_alloc_load_rrec,
			     "CDV-alloc: record @LLU read failed errno=@INT", i, errno);
			fclose(f);
			return -EIO;
		}
		if (rec.magic != NVMEIBT_CDV_EXTENT_MD_MAGIC) {
			N_Wf(cdv_alloc_load_rmagic,
			     "CDV-alloc: rec @LLU bad magic @LLU_TX; skipping", i, rec.magic);
			continue;
		}
		/* NUL-terminate defensively before use. */
		rec.cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
		rec.tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';

		/*
		 * Insert directly without persist — we're restoring state, not
		 * generating new allocations.  Suppress the per-record persist cost
		 * during a potentially large startup load.
		 */
		rv = cdv_alloc_insert(rec.cdv_uuid, rec.extent_index, rec.tpv_uuid);
		if (rv) {
			N_Ef(cdv_alloc_load_add,
			     "CDV-alloc: insert failed rec=@LLU rv=@INT; extent becomes orphan",
			     i, rv);
			/* Non-fatal: NVCK will reclaim the orphan. */
		}
	}

	fclose(f);
	N_If(cdv_alloc_load_ok,
	     "CDV-alloc: loaded @LLU records from cdv_alloc.bin", hdr.n_records);
	return 0;
}

/* ── One-time init / shutdown ────────────────────────────────────────────── */

int nvmeibt_cdv_alloc_one_time_init(void)
{
	int rv;

	cdv_alloc_hash = NVMEIB_HASH_CREATE(cdv_alloc_hash_create,
					    5,                /* 32 initial buckets */
					    "cdv_alloc_hash",
					    -1,               /* ASCII / string key */
					    false);           /* main-thread only */
	if (!cdv_alloc_hash) {
		N_Ef(cdv_alloc_init_hash, "CDV-alloc: hash_create failed");
		return -ENOMEM;
	}

	rv = nvmeibt_cdv_alloc_load();
	if (rv) {
		/*
		 * Load failure is non-fatal: log it and continue with an empty
		 * allocator.  Any orphaned extents will be reclaimed by NVCK.
		 */
		N_Wf(cdv_alloc_init_load_fail,
		     "CDV-alloc: startup load failed rv=@INT; orphans expected until NVCK",
		     rv);
	}

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
 * handle_cdv_alloc_extent — CDV_ALLOC_EXTENT stub.
 *
 * Step 12c will implement: pick a free CDV_extent from the allocator,
 * call nvmeibt_cdv_alloc_add_extent(), send back nvmeibt_cdv_alloc_resp.
 */
static int handle_cdv_alloc_extent(struct nvmeibt_register_msg *msg)
{
	struct nvmeibt_cdv_alloc_resp resp;

	if (msg->data_length < (int)sizeof(struct nvmeibt_cdv_alloc_req)) {
		N_Ef(cdv_handle_alloc_short,
		     "CDV-handle: ALLOC_EXTENT payload too short len=@INT", msg->data_length);
		return -EINVAL;
	}

	N_Tf(cdv_handle_alloc_stub,
	     "CDV-handle: ALLOC_EXTENT stub — returning ENOSYS");

	memset(&resp, 0, sizeof(resp));
	resp.status = NVMEIBT_CDV_ALLOC_ERROR;

	return cdv_send_response(&msg->registrant_ctx,
				 NVMEIBT_CLIENT_MSG_TR_CDV_ALLOC_EXTENT_RSP,
				 sizeof(resp), &resp);
}

/*
 * handle_cdv_free_extent — CDV_FREE_EXTENT stub.
 *
 * Step 12c will implement: validate ownership, call
 * nvmeibt_cdv_alloc_remove_extent().  Fire-and-forget — no response.
 */
static int handle_cdv_free_extent(struct nvmeibt_register_msg *msg)
{
	if (msg->data_length < (int)sizeof(struct nvmeibt_cdv_free_req)) {
		N_Ef(cdv_handle_free_short,
		     "CDV-handle: FREE_EXTENT payload too short len=@INT", msg->data_length);
		return -EINVAL;
	}

	N_Tf(cdv_handle_free_stub,
	     "CDV-handle: FREE_EXTENT stub — ignoring");

	/* No response: CDV_FREE_EXTENT is fire-and-forget. */
	return 0;
}

/*
 * handle_cdv_list_extents — CDV_LIST_EXTENTS stub.
 *
 * Step 12c will implement: call nvmeibt_cdv_alloc_list_for_tpv(),
 * build variable-length response with nvmeibt_cdv_list_resp + index array.
 */
static int handle_cdv_list_extents(struct nvmeibt_register_msg *msg)
{
	struct nvmeibt_cdv_list_resp resp;

	if (msg->data_length < (int)sizeof(struct nvmeibt_cdv_list_req)) {
		N_Ef(cdv_handle_list_short,
		     "CDV-handle: LIST_EXTENTS payload too short len=@INT", msg->data_length);
		return -EINVAL;
	}

	N_Tf(cdv_handle_list_stub,
	     "CDV-handle: LIST_EXTENTS stub — returning empty list");

	memset(&resp, 0, sizeof(resp));
	resp.n_extents = 0;
	resp.status    = 0;

	return cdv_send_response(&msg->registrant_ctx,
				 NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP,
				 sizeof(resp), &resp);
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
