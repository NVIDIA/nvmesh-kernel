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

static struct dp_target_find_disk_result __dp_targets_find_disk_by_id(struct dp_targets const* self, const char *diskID)
{
    __auto_type derived = magic_derived_cast(struct nvmeibc_volume_targets, self);
    assert_base_and_derived(struct dp_targets, struct nvmeibc_volume_targets);
    return nvmeibc_volume_targets_find_disk_by_id(derived, diskID);
}

void nvmeibc_volume_targets_init(struct nvmeibc_volume_targets* self)
{
    self->base = (struct dp_targets) {
        .find_disk_by_id = __dp_targets_find_disk_by_id
    };
    INIT_LIST_HEAD(&self->disks);				// Exactly the list of volumes which use transport layer c_disks
	self->retain_disks = false;
    self->magic = MAGIC_CAST_VALUE;
}

struct dp_target_find_disk_result nvmeibc_volume_targets_find_disk_by_id(struct nvmeibc_volume_targets const* self, const char *diskID)
{
	struct nvmeibc_disk_id *disk = NULL;
    struct dp_target_find_disk_result found = {0};

	list_for_each_entry(disk, &(self->disks), link) {
		if (!strcmp(disk->name, diskID)) {
            found = (struct dp_target_find_disk_result){
                .disk = &disk->disk->base
                #if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
			    , .v_disk_stats = disk->v_disk_stats
                #endif
            };
            break;
		}
	}
	return found;
}