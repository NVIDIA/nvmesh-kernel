/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_CHUNK
#define NVMEIBT_CHUNK

#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_str.h"
#include "nvmeibt_block_device.h"

struct nvmeibt_chunk_config {
	union nvmeib_uuid			id;
	int							version;
	union nvmeib_uuid			its_block_device_id;
	long long					vlb_s;
	long long					vlb_e;
	int							stripe_width;	// n_logical segs in each praid (n_segs - redundancy)
	int							stripe_size;	// n_praids
};

struct nvmeibt_praid;

struct nvmeibt_chunk {
	struct nvmeibt_chunk_config		from_config;
	struct nvmeibt_urn_uuid			urn_uuid;
	struct mm_chunk_conf			its_mm_chunk_conf;
	struct nvmeibt_block_device		*its_block_device;
	int								its_idx_in_block_device;
	int								n_praids;
	struct nvmeibt_praid			*praids[NVMEIBT_MAX_STRIPE_WIDTH_PER_CHUNK];
	BOOL							is_conf_corrupted;
	uint8_t							trim_flags;
	struct xdlist					topo_link;
	int								config_tag;
};

static inline struct nvmeibt_block_device *nvmeibt_chunk_get_blkdev(struct nvmeibt_chunk *chunk)
{
	return (chunk ? chunk->its_block_device : NULL);
}

static inline BOOL nvmeibt_chunk_is_conf_corrupted(struct nvmeibt_chunk *chunk)
{
	return (chunk && chunk->is_conf_corrupted);
}

static inline BOOL nvmeibt_chunk_is_being_deleted(struct nvmeibt_chunk *chunk)
{
	return (!chunk || nvmeibt_blkdev_is_being_deleted(chunk->its_block_device));
}

BOOL nvmeibt_chunk_is_deprecated_in_config(struct nvmeibt_chunk *chunk);
const union nvmeib_uuid *nvmeibt_chunk_UUID(struct nvmeibt_chunk *chunk);
const char *nvmeibt_chunk_id_str(struct nvmeibt_chunk *chunk);
const char *nvmeibt_chunk_get_blkdev_name(const struct nvmeibt_chunk *chunk);
struct nvmeibt_chunk *nvmeibt_chunk_get_chunk_by_id(const union nvmeib_uuid *id, bool might_fail);
enum nvmeibt_add_rv nvmeibt_chunk_add(struct mm_chunk_conf *conf, struct nvmeibt_block_device *blkdev, int idx_in_vol, int config_tag, struct nvmeibt_chunk **output_chunk);
int nvmeibt_chunk_remove(struct nvmeibt_chunk *chunk);
void nvmeibt_chunk_mark_conf_corrupted(struct nvmeibt_chunk *chunk);
void nvmeibt_chunk_trim_specific_chunk(struct nvmeibt_chunk *chunk, uint8_t trim_flag);
void nvmeibt_chunk_trim_unused_entries(int config_tag, uint8_t trim_flag);
void nvmeibt_chunk_serialize_config_section(struct mm_chunk_conf *chunk_conf, struct nvmeibt_chunk *chunk);

#endif // #ifndef NVMEIBT_CHUNK

