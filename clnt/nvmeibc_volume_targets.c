/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_volume_targets.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_disk.h"
#include "nvmeib_stats.h"
#include "nvmeibc_targets.h"
#include "nvmeibc_types.h"
#include "common/safe_casting.h"

static struct dp_target_find_disk_result __dp_targets_find_disk_by_name(struct dp_targets const* self, const char *diskID)
{
    __auto_type derived = magic_derived_cast(struct nvmeibc_volume_targets, self);
    assert_base_and_derived(struct dp_targets, struct nvmeibc_volume_targets);
	return nvmeibc_volume_targets_find_disk_by_name(derived, diskID);
}

void nvmeibc_volume_targets_init(struct nvmeibc_volume_targets* self)
{
    self->base = (struct dp_targets) {
		.find_disk_by_name = __dp_targets_find_disk_by_name
    };
    INIT_LIST_HEAD(&self->disks);				// Exactly the list of volumes which use transport layer c_disks
	self->retain_disks = false;
    self->magic = MAGIC_CAST_VALUE;
}

struct dp_targets const *nvmeibc_volume_targets_base(struct nvmeibc_volume_targets const* self)
{
	return &self->base;
}

struct list_head const* __nvmeibc_volume_targets_get_disks_impl(struct nvmeibc_volume_targets const* self)
{
	return &self->disks;
}

void nvmeibc_volume_targets_add_disk_id(struct nvmeibc_volume_targets* self, struct nvmeibc_disk_id *disk_id)
{
	list_add_tail(&disk_id->link, nvmeibc_volume_targets_get_disks(self));
}

struct nvmeibc_disk_id *nvmeibc_volume_targets_find_disk_id_by_name(struct nvmeibc_volume_targets const* self, const char *disk_name)
{
	struct nvmeibc_disk_id *disk_id;
	struct list_head const *disks = nvmeibc_volume_targets_get_disks(self);

	list_for_each_entry(disk_id, disks, link) {
		if (!strncmp(disk_id->name, disk_name, sizeof(disk_id->name))) {
			return disk_id;
		}
	}
	return NULL;
}

struct nvmeibc_disk_id *nvmeibc_volume_targets_find_disk_id_by_disk(struct nvmeibc_volume_targets const* self, struct nvmeibc_disk const* disk)
{
	struct nvmeibc_disk_id *disk_id;
	struct list_head const* disks = nvmeibc_volume_targets_get_disks(self);

	list_for_each_entry(disk_id, disks, link) {
		if (disk_id->disk == disk) {
			return disk_id;
		}
	}
	return NULL;
}

int nvmeibc_volume_targets_count_disks(struct nvmeibc_volume_targets const* self)
{
	struct nvmeibc_disk_id *disk_id;
	struct list_head const* disks = nvmeibc_volume_targets_get_disks(self);
	int n_disks = 0;

	list_for_each_entry(disk_id, disks, link) {
		++n_disks;
	}
	return n_disks;
}

bool nvmeibc_volume_targets_should_retain_disks(struct nvmeibc_volume_targets const* self)
{
	return self->retain_disks;
}

void nvmeibc_volume_targets_set_retain_disks(struct nvmeibc_volume_targets* self, bool retain_disks)
{
	self->retain_disks = retain_disks;
}

struct dp_target_find_disk_result nvmeibc_volume_targets_find_disk_by_name(struct nvmeibc_volume_targets const* self, const char *diskID)
{
	struct nvmeibc_disk_id *disk_id = nvmeibc_volume_targets_find_disk_id_by_name(self, diskID);
    struct dp_target_find_disk_result found = {0};

	if (disk_id && disk_id->disk) {
		found = (struct dp_target_find_disk_result){
			.disk = &disk_id->disk->base
			#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
			, .v_disk_stats = disk_id->v_disk_stats
			#endif
		};
	}
	return found;
}
