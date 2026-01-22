/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_chunk.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"

BOOL nvmeibt_chunk_is_deprecated_in_config(struct nvmeibt_chunk *chunk)
{
	return (chunk && nvmeibt_block_device_is_deprecated_in_config(chunk->its_block_device));
}

const union nvmeib_uuid *nvmeibt_chunk_UUID(struct nvmeibt_chunk *chunk)
{
	return (chunk ? &chunk->from_config.id : &nvmeib_uuid_null_val);
}

const char *nvmeibt_chunk_id_str(struct nvmeibt_chunk *chunk)
{
	return (chunk ? chunk->urn_uuid.str : "");
}

const char *nvmeibt_chunk_get_blkdev_name(const struct nvmeibt_chunk *chunk)
{
	return (chunk ? nvmeibt_blkdev_name(chunk->its_block_device) : "???");
}

#ifdef TOMA_DEBUG
void nvmeibt_chunk_dump(struct nvmeibt_chunk *chunk)
{
	struct nvmeibt_chunk_config *f = &chunk->from_config;


	N_Tf(trace_chunk_nvmeibt_chunk_dump, "Config data: id=@UUID_LE its_block_device_id=@UUID_LE vlb_s=@LLD vlb_e=@LLD",
		&f->id, &f->its_block_device_id, f->vlb_s, f->vlb_e);
}
#else	// #ifdef TOMA_DEBUG
void nvmeibt_chunk_dump(__attribute__((__unused__)) struct nvmeibt_chunk *chunk) {}
#endif	// #ifdef TOMA_DEBUG

struct nvmeibt_chunk *nvmeibt_chunk_get_chunk_by_id(const union nvmeib_uuid *id, bool might_fail)
{
	struct nvmeibt_chunk	 *chunk;

	chunk = NNVMEIBT_HASH_GET_OBJ_BY_UUID(trace_chunk_nvmeibt_chunk_get_chunk_by_id, &nvmeibt_global_get_global()->chunks_hash, id, chunk);

	if (chunk == NULL) {
		if (might_fail) {
			N_Tf(1naijr9, "chunk not found id='@UUID_LE'", id);
		} else {
			N_Ef(ao94kl3, "chunk not found id='@UUID_LE'", id);
		}
	}

	return chunk;
}

/*
 * Try to remove blkdev allocation. Returns:
 *   1  : entry removed
 *   0  : entry not removed
 */
int nvmeibt_chunk_remove(struct nvmeibt_chunk *chunk)
{
	int rv = -1;

	NFIN;

	if (chunk == NULL)
		goto out;

	N_Tf(trace_chunk_nvmeibt_chunk_remove, "Removing chunk=@UUID_LE", nvmeibt_chunk_UUID(chunk));

	NNVMEIBT_HASH_DEL_OBJ(trace_1_chunk_nvmeibt_chunk_remove, &nvmeibt_global_get_global()->chunks_hash, chunk, chunk);
	// The assumption is that the surrounding objects (block_device & chunk) are also removed

	NNVMEIBT_TOMA_FREE(trace_2_chunk_nvmeibt_chunk_remove, chunk);
	rv = 0;

out:
	NFOUT;
	return rv;
}

enum nvmeibt_add_rv nvmeibt_chunk_add(struct mm_chunk_conf *conf, struct nvmeibt_block_device *blkdev, int idx_in_vol, int config_tag, struct nvmeibt_chunk **output_chunk)
{
	enum nvmeibt_add_rv			rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_chunk		*new_chunk = NULL;	// Read into it, maybe use it.
	struct nvmeibt_chunk		*chunk;
	struct nvmeibt_chunk_config *f = NULL;

	NFIN;

	chunk = nvmeibt_chunk_get_chunk_by_id(&(conf->uuid), 1);
	if (chunk) {
		chunk->trim_flags &= ~CONFIG_TRIM_MGMT;
		if (chunk->from_config.version >= blkdev->from_config.version) {
			N_Tf(9dk3lps, "chunk=@UUID_LE chunk->version=@X received version=@X, skipping", &(conf->uuid), chunk->from_config.version, blkdev->from_config.version);
			chunk->config_tag = config_tag;
			rv = NVMEIBT_ADD_ALREADY_UP_TO_DATE;
			goto out;
		}
	}
	chunk = NULL;

	new_chunk = NNVMEIBT_TOMA_CALLOC(trace_chunk_nvmeibt_chunk_add, 1, sizeof *new_chunk);
	XDLIST_INIT_LINK(&new_chunk->topo_link, NULL);

	f = &(new_chunk->from_config);

	f->id = conf->uuid;
	f->version = blkdev->from_config.version;
	f->vlb_s = conf->vlbs;
	f->vlb_e = conf->vlbe;
	f->its_block_device_id = blkdev->from_config.id;
	f->stripe_size = blkdev->from_config.stripe_size;
	f->stripe_width = blkdev->from_config.stripe_width;

	if (f->stripe_width > NVMEIBT_MAX_STRIPE_WIDTH_PER_CHUNK) {
		N_Ef(error_chunk_nvmeibt_chunk_add, "stripe_width=@STRIPE_WIDTH too big", f->stripe_width);
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}

	rv = NNVMEIBT_HASH_ADD_OBJ(trace_1_chunk_nvmeibt_chunk_add, 
					&nvmeibt_global_get_global()->chunks_hash,
					new_chunk,
					config_tag,
					NVMEIBT_MAX_N_CHUNKS,
					chunk, NULL, chunk);

	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL)
		goto out;

	// In any case, update the following config-driven fields
	chunk->its_mm_chunk_conf = *conf;
	chunk->n_praids = f->stripe_width;
	chunk->its_block_device = blkdev;
	chunk->its_idx_in_block_device = idx_in_vol;
	blkdev->chunks[idx_in_vol] = chunk;

out:
	if (rv == NVMEIBT_ADD_NEW) {
		chunk->urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&conf->uuid);
		chunk->trim_flags = 0;
	} else {
		N_Tf(6cbvwuj, "Freeing unused new_chunk");
		NNVMEIBT_TOMA_FREE(trace_4_chunk_nvmeibt_chunk_add, new_chunk);
	}
	*output_chunk = chunk;
	NFOUT;
	return rv;
}

void nvmeibt_chunk_mark_conf_corrupted(struct nvmeibt_chunk *chunk)
{
	N_Wf(warn_chunk_nvmeibt_chunk_mark_conf_corrupted, "chunk=@UUID_LE", nvmeibt_chunk_UUID(chunk));
	if (chunk) {
		chunk->is_conf_corrupted = 1;
		nvmeibt_mark_conf_corrupted();
	}
}

void nvmeibt_chunk_trim_specific_chunk(struct nvmeibt_chunk *chunk, uint8_t trim_flag)
{
	if (is_trim_needed(&chunk->trim_flags, trim_flag)) {
		NVMEIBT_HASH_MARK_OBJ_OUTDATED(nvmeibt_chunk_trim_unused_entries_trace, chunk, chunk);
	}
	else {
		N_Tf(jdjs7w8, "not yet, chunk=@UUID_LE flags=@X", nvmeibt_chunk_UUID(chunk), chunk->trim_flags);
	}
}

void nvmeibt_chunk_trim_unused_entries(int config_tag, uint8_t trim_flag)
{
	struct nvmeibt_chunk	*chunk;

	NFIN;
	XHASHTABLE_FOR_EACH_SAFE(chunk, &nvmeibt_global_get_global()->chunks_hash) {
		if (NVMEIBT_HASH_IS_OLDER_OBJ(chunk, config_tag)) {
			nvmeibt_chunk_trim_specific_chunk(chunk, trim_flag);
		}
	}
	NFOUT;
}

void nvmeibt_chunk_serialize_config_section(struct mm_chunk_conf *chunk_conf, struct nvmeibt_chunk *chunk)
{
	struct nvmeibt_chunk_config		*f;

	NFIN;
	NTOMA_ASSERT(5v58sms, chunk_conf && chunk, "chunk=NULL");
	f = &(chunk->from_config);

	nvmeibt_strlcpy(chunk_conf->eyecatcher, "CHK", sizeof(chunk_conf->eyecatcher));
	chunk_conf->uuid = f->id;
	chunk_conf->vlbs = f->vlb_s;
	chunk_conf->vlbe = f->vlb_e;
	NFOUT;
}

