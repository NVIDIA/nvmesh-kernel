/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 * nvmeibt_cdv_alloc.c — CDV extent allocator: in-memory state.
 *
 * See nvmeibt_cdv_alloc.h for a full description.
 * Allocator identity and extent table are persisted to a small binary file
 * (NVMEIBT_CDV_ALLOC_STATE_FILE) after each election and extent operation so
 * that state is fully restored on TOMA restart without waiting for clients to
 * reconnect.  The file is updated atomically (write-rename).
 *
 * Future work: wire this state into the RAFT persist_and_wire_buf TLV sections
 * so that it is also replicated to follower nodes in multi-TOMA deployments.
 */

#include <string.h>
#include <errno.h>
#include <sys/stat.h>
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

/* ── Global state ────────────────────────────────────────────────────────── */

/* cdv_uuid (ASCII string) → nvmeibt_cdv_alloc * */
static struct nvmeib_hash_table *cdv_alloc_hash;

/* ── Local file persistence ──────────────────────────────────────────────── */

#define NVMEIBT_CDV_ALLOC_STATE_FILE  (NVMEIBT_PERSISTENCY_CACHE_DIR "cdv_alloc_state.bin")
#define NVMEIBT_CDV_ALLOC_STATE_MAGIC (0x43445631U)	/* 'CDV1' in little-endian */

/*
 * Wire record layouts (all integer fields little-endian):
 *
 *   File:
 *     [uint32 magic][uint32 n_cdvs]
 *     for each CDV:
 *       [char cdv_uuid[64]][char allocator_toma_id[64]]
 *       [uint64 allocator_generation][uint32 n_extents]
 *       for each extent:
 *         [uint64 extent_index][char tpv_uuid[64]]
 *     [uint32 crc32]   -- crc32(0, entire file excluding this field)
 */

/* Write @size bytes from @buf to @fd, retrying on EINTR. */
static int cdv_write_all(int fd, const void *buf, size_t size)
{
	const char *ptr = (const char *)buf;

	while (size > 0) {
		ssize_t n = write(fd, ptr, size);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		ptr  += n;
		size -= (size_t)n;
	}
	return 0;
}

/*
 * nvmeibt_cdv_alloc_save_state — atomically persist cdv_alloc_hash.
 *
 * Builds the entire file content in memory, then writes via a temp file and
 * rename.  Called after election, extent allocation, and extent free so that
 * state survives TOMA restarts.
 */
static void nvmeibt_cdv_alloc_save_state(void)
{
	struct nvmeibt_cdv_alloc        *alloc;
	struct nvmeibt_cdv_extent_entry *entry;
	const char *path     = NVMEIBT_CDV_ALLOC_STATE_FILE;
	char        tmp_path[256];
	int         fd       = -1;
	uint32_t    magic_le = LE_SWAP32(NVMEIBT_CDV_ALLOC_STATE_MAGIC);
	uint32_t    n_cdvs   = 0;
	uint32_t    n_cdvs_le;
	uint32_t    crc      = 0;

	if (!cdv_alloc_hash)
		return;

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

	/* Count CDVs */
	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash)
		n_cdvs++;

	fd = NNVMEIBT_OPEN(cdv_save_open, tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		N_Ef(cdv_save_open_err,
		     "CDV-alloc: save: open @PATH @AUTO_ERRNO", tmp_path);
		return;
	}

/*
 * CDV_WRIT: write @sz_ bytes from @ptr_, accumulate CRC, jump to out_write_err
 * on failure.  The macro does NOT embed a trace tag so it is safe to use in
 * loops — the single error label below carries the unique trace.
 */
#define CDV_WRIT(ptr_, sz_) do {					\
	if (cdv_write_all(fd, (ptr_), (sz_)) < 0)			\
		goto out_write_err;					\
	crc = crc32(crc, (ptr_), (sz_));				\
} while (0)

	/* Header: magic + n_cdvs */
	CDV_WRIT(&magic_le, sizeof(magic_le));
	n_cdvs_le = LE_SWAP32(n_cdvs);
	CDV_WRIT(&n_cdvs_le, sizeof(n_cdvs_le));

	NVMEIB_HASH_FOREACH(alloc, cdv_alloc_hash) {
		uint64_t gen_le      = LE_SWAP64(alloc->allocator_generation);
		uint32_t n_ext_le    = LE_SWAP32((uint32_t)alloc->n_allocated);

		CDV_WRIT(alloc->cdv_uuid,          NVMEIBT_CDV_UUID_STRLEN);
		CDV_WRIT(alloc->allocator_toma_id, NVMEIBT_CDV_HOSTNAME_LEN);
		CDV_WRIT(&gen_le,                  sizeof(gen_le));
		CDV_WRIT(&n_ext_le,                sizeof(n_ext_le));

		XDLIST_FOREACH(entry, &alloc->extents) {
			uint64_t idx_le = LE_SWAP64(entry->extent_index);

			CDV_WRIT(&idx_le,          sizeof(idx_le));
			CDV_WRIT(entry->tpv_uuid,  NVMEIBT_CDV_UUID_STRLEN);
		}
	}
	goto past_write_err;
out_write_err:
	N_Ef(cdv_save_write_err, "CDV-alloc: save: write @AUTO_ERRNO");
	goto out;
past_write_err:
#undef CDV_WRIT

	/* Trailing CRC (not included in the checksum itself) */
	{
		uint32_t crc_le = LE_SWAP32(crc);

		if (cdv_write_all(fd, &crc_le, sizeof(crc_le)) < 0) {
			N_Ef(cdv_save_crc_err,
			     "CDV-alloc: save: CRC write @AUTO_ERRNO");
			goto out;
		}
	}

	if (NNVMEIBT_FSYNC(cdv_save_fsync, fd) < 0)
		N_Wf(cdv_save_fsync_warn, "CDV-alloc: save: fsync @AUTO_ERRNO");

	NNVMEIBT_CLOSE(cdv_save_close1, fd);
	fd = -1;

	if (NNVMEIBT_RENAME(cdv_save_rename, tmp_path, path) < 0) {
		N_Ef(cdv_save_rename_err,
		     "CDV-alloc: save: rename to @PATH @AUTO_ERRNO", path);
		goto out;
	}

	N_If(cdv_save_ok,
	     "CDV-alloc: saved n_cdvs=@UINT to @PATH", n_cdvs, path);

out:
	if (fd >= 0)
		NNVMEIBT_CLOSE(cdv_save_close2, fd);
}

/*
 * nvmeibt_cdv_alloc_load_state — restore cdv_alloc_hash from the state file.
 *
 * Called from nvmeibt_cdv_alloc_one_time_init() before RAFT log replay so
 * that CDV allocator state is available immediately on startup.  A missing or
 * corrupt file is treated as empty (first run).
 */
static void nvmeibt_cdv_alloc_load_state(void)
{
	const char          *path = NVMEIBT_CDV_ALLOC_STATE_FILE;
	int                  fd   = -1;
	struct nvmeibt_Str  *buf  = NULL;
	const char          *ptr;
	size_t               file_len;
	uint32_t             magic_le, n_cdvs_le, n_cdvs, crc, stored_crc_le, stored_crc;
	uint32_t             cdv_i;

	if (access(path, F_OK) < 0) {
		N_Tf(cdv_load_no_file,
		     "CDV-alloc: no state file @PATH (first run)", path);
		return;
	}

	fd = NNVMEIBT_OPEN_READ(cdv_load_open, path, 1);
	if (fd < 0) {
		N_Ef(cdv_load_open_err,
		     "CDV-alloc: load: open @PATH @AUTO_ERRNO", path);
		return;
	}

	buf = NNVMEIBT_STR_ALLOC(cdv_load_str_alloc);
	if (!buf) {
		N_Ef(cdv_load_oom, "CDV-alloc: load: OOM for read buffer");
		goto out;
	}

	NNVMEIBT_STR_FREAD(cdv_load_fread, buf, fd);
	file_len = nvmeibt_Str_strlen(buf);

	/* Minimum: magic(4) + n_cdvs(4) + crc(4) */
	if (file_len < 3 * sizeof(uint32_t)) {
		N_Ef(cdv_load_short,
		     "CDV-alloc: load: file too short size=@SIZE_T", file_len);
		goto out;
	}

	/* Verify CRC over all bytes except the trailing 4 */
	ptr = nvmeibt_Str_str(buf);
	crc = crc32(0, ptr, file_len - sizeof(uint32_t));
	memcpy(&stored_crc_le, ptr + file_len - sizeof(uint32_t), sizeof(uint32_t));
	stored_crc = LE_SWAP32(stored_crc_le);
	if (crc != stored_crc) {
		N_Ef(cdv_load_crc,
		     "CDV-alloc: load: CRC mismatch computed=@X stored=@X; ignoring",
		     crc, stored_crc);
		goto out;
	}

	/* Parse header */
	memcpy(&magic_le, ptr, sizeof(magic_le));
	if (LE_SWAP32(magic_le) != NVMEIBT_CDV_ALLOC_STATE_MAGIC) {
		N_Ef(cdv_load_magic,
		     "CDV-alloc: load: bad magic=@X", LE_SWAP32(magic_le));
		goto out;
	}
	ptr += sizeof(magic_le);

	memcpy(&n_cdvs_le, ptr, sizeof(n_cdvs_le));
	n_cdvs = LE_SWAP32(n_cdvs_le);
	ptr += sizeof(n_cdvs_le);

	N_If(cdv_load_start,
	     "CDV-alloc: loading n_cdvs=@UINT from @PATH", n_cdvs, path);

	for (cdv_i = 0; cdv_i < n_cdvs; cdv_i++) {
		char     cdv_uuid[NVMEIBT_CDV_UUID_STRLEN];
		char     allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN];
		uint64_t gen_le, generation;
		uint32_t n_ext_le, n_extents, ext_i;
		struct nvmeibt_cdv_alloc *alloc;

		/* Bounds: uuid + toma_id + generation + n_extents */
		if ((size_t)(ptr - nvmeibt_Str_str(buf)) +
		    NVMEIBT_CDV_UUID_STRLEN + NVMEIBT_CDV_HOSTNAME_LEN +
		    sizeof(uint64_t) + sizeof(uint32_t) >
		    file_len - sizeof(uint32_t)) {
			N_Ef(cdv_load_trunc,
			     "CDV-alloc: load: truncated at CDV @LLU", cdv_i);
			goto out;
		}

		memcpy(cdv_uuid, ptr, NVMEIBT_CDV_UUID_STRLEN);
		cdv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
		ptr += NVMEIBT_CDV_UUID_STRLEN;

		memcpy(allocator_toma_id, ptr, NVMEIBT_CDV_HOSTNAME_LEN);
		allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN - 1] = '\0';
		ptr += NVMEIBT_CDV_HOSTNAME_LEN;

		memcpy(&gen_le, ptr, sizeof(gen_le));
		generation = LE_SWAP64(gen_le);
		ptr += sizeof(gen_le);

		memcpy(&n_ext_le, ptr, sizeof(n_ext_le));
		n_extents = LE_SWAP32(n_ext_le);
		ptr += sizeof(n_ext_le);

		/* Create allocator entry with persisted generation */
		nvmeibt_cdv_alloc_set_generation(cdv_uuid, generation);

		/* Restore allocator_toma_id (set_generation leaves it empty) */
		alloc = nvmeib_hash_search_ascii_str(cdv_alloc_hash, cdv_uuid);
		if (alloc && allocator_toma_id[0]) {
			strncpy(alloc->allocator_toma_id, allocator_toma_id,
				NVMEIBT_CDV_HOSTNAME_LEN - 1);
			alloc->allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN - 1] = '\0';
		}

		for (ext_i = 0; ext_i < n_extents; ext_i++) {
			uint64_t idx_le, idx;
			char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];

			if ((size_t)(ptr - nvmeibt_Str_str(buf)) +
			    sizeof(uint64_t) + NVMEIBT_CDV_UUID_STRLEN >
			    file_len - sizeof(uint32_t)) {
				N_Ef(cdv_load_trunc_ext,
				     "CDV-alloc: load: truncated at extent @LLU/@LLU cdv=@STR",
				     ext_i, n_extents, cdv_uuid);
				goto out;
			}

			memcpy(&idx_le, ptr, sizeof(idx_le));
			idx = LE_SWAP64(idx_le);
			ptr += sizeof(idx_le);

			memcpy(tpv_uuid, ptr, NVMEIBT_CDV_UUID_STRLEN);
			tpv_uuid[NVMEIBT_CDV_UUID_STRLEN - 1] = '\0';
			ptr += NVMEIBT_CDV_UUID_STRLEN;

			if (nvmeibt_cdv_alloc_add_extent(cdv_uuid, idx, tpv_uuid) < 0)
				N_Ef(cdv_load_add_fail,
				     "CDV-alloc: load: add_extent failed cdv=@STR idx=@LLU",
				     cdv_uuid, idx);
		}
	}

	N_If(cdv_load_ok,
	     "CDV-alloc: loaded n_cdvs=@UINT from @PATH", n_cdvs, path);
	nvmeibt_cdv_alloc_startup_scan();

out:
	if (buf)
		NNVMEIBT_STR_FREE(cdv_load_str_free, buf);
	NNVMEIBT_CLOSE(cdv_load_close, fd);
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
				return 0;   /* no change needed */
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

	nvmeibt_cdv_alloc_save_state();
	return 0;
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

	/* Restore allocator state from the previous run's persistence file.
	 * This populates cdv_alloc_hash before RAFT log replay so that
	 * CDV status is immediately available after restart even with no
	 * clients attached. */
	nvmeibt_cdv_alloc_load_state();

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

	resp.extent_index         = candidate;
	resp.allocator_generation = alloc ? alloc->allocator_generation : 0;
	resp.status               = NVMEIBT_CDV_ALLOC_OK;

	/* Check if this allocation pushed the CDV above the warning watermark. */
	if (alloc)
		cdv_maybe_warn_capacity(alloc);

	N_If(cdv_alloc_ok,
	     "CDV: ALLOC OK cdv=@STR idx=@LLU tpv=@STR req_id=@LLU gen=@LLU",
	     cdv_uuid, candidate, tpv_uuid, req->req_id, resp.allocator_generation);

	/* Persist the new allocation so it survives a TOMA restart. */
	nvmeibt_cdv_alloc_save_state();

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

		/* Persist the freed allocation so TOMA restart doesn't re-use this slot. */
		nvmeibt_cdv_alloc_save_state();

		/*
		 * Hysteresis check: if the free dropped usage below
		 * NVMEIBT_CDV_WARN_CLEAR_PCT, clear capacity_warning_sent so the
		 * next rise above WARN_PCT fires a fresh Kafka event.
		 */
		cdv_maybe_warn_capacity(alloc);
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
