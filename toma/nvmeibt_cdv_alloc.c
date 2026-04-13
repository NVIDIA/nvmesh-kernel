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
#include "nvmeibt_kafka.h"		/* nvmeibt_kafka_outgoing_msgs_queue_add, KAFKA_PRODUCER_MSG_HEADER_* */
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

	/* Log restored state; fire capacity warnings for any already-full CDVs. */
	nvmeibt_cdv_alloc_startup_scan();

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

	if (alloc->total_data_extents == 0)
		return;   /* capacity unknown — cannot compute ratio */

	used_pct = (unsigned int)(alloc->n_allocated * 100 / alloc->total_data_extents);

	if (used_pct < NVMEIBT_CDV_WARN_CLEAR_PCT) {
		/* Usage safely below hysteresis threshold — reset flag. */
		alloc->capacity_warning_sent = false;
		return;
	}

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
	first_alloc = (alloc == NULL);

	/* ── Generation check ────────────────────────────────────────────────── */
	if (first_alloc && req->client_generation != 0) {
		/*
		 * No persisted state for this CDV; client carries a non-zero
		 * generation (stale from before a TOMA restart).  Return WRONG_GEN
		 * so the client re-syncs via the next topology push.
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

	/* ── CDV-full check ──────────────────────────────────────────────────── */
	if (!first_alloc && alloc->n_allocated >= total) {
		N_Wf(cdv_alloc_full,
		     "CDV: ALLOC cdv=@STR FULL allocated=@LLU total=@LLU",
		     cdv_uuid, alloc->n_allocated, total);
		cdv_maybe_warn_capacity(alloc);   /* ensure Kafka event reaches management */
		resp.status = NVMEIBT_CDV_ALLOC_CDV_FULL;
		resp.allocator_generation = alloc->allocator_generation;
		goto send;
	}

	/* ── Find first free extent index ────────────────────────────────────── */
	if (first_alloc) {
		candidate = 0;     /* no extents allocated yet */
		found     = true;
	} else {
		candidate = 0;
		found     = false;
		for (i = 0; i < total; i++) {
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

	/* Persist atomically; on failure, roll back to stay consistent. */
	rv = nvmeibt_cdv_alloc_persist();
	if (rv) {
		N_Ef(cdv_alloc_persist_fail,
		     "CDV: ALLOC persist failed rv=@INT; rolling back idx=@LLU",
		     rv, candidate);
		if (alloc) {
			XDLIST_FOREACH_SAFE(entry, &alloc->extents) {
				if (entry->extent_index == candidate) {
					XDLIST_ELEM_DEL(&alloc->extents, entry);
					alloc->n_allocated--;
					NNVMEIBT_BM_FREE(cdv_alloc_rollback, entry);
					break;
				}
			}
		}
		resp.status = NVMEIBT_CDV_ALLOC_ERROR;
		goto send;
	}

	resp.extent_index         = candidate;
	resp.allocator_generation = alloc ? alloc->allocator_generation : 0;
	resp.status               = NVMEIBT_CDV_ALLOC_OK;

	/* Check if this allocation pushed the CDV above the warning watermark. */
	if (alloc)
		cdv_maybe_warn_capacity(alloc);

	N_If(cdv_alloc_ok,
	     "CDV: ALLOC OK cdv=@STR idx=@LLU tpv=@STR req_id=@LLU gen=@LLU",
	     cdv_uuid, candidate, tpv_uuid, req->req_id, resp.allocator_generation);

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

		/*
		 * Hysteresis check: if the free dropped usage below
		 * NVMEIBT_CDV_WARN_CLEAR_PCT, clear capacity_warning_sent so the
		 * next rise above WARN_PCT fires a fresh Kafka event.
		 */
		cdv_maybe_warn_capacity(alloc);

		/* Best-effort persist; loss is non-fatal (NVCK reclaims orphans). */
		nvmeibt_cdv_alloc_persist();
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
		err_resp.status = 1;
		return cdv_send_response(&msg->registrant_ctx,
					 NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP,
					 sizeof(err_resp), &err_resp);
	}

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
