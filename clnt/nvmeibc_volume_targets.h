/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_VOLUME_TARGETS_H
#define NVMEIBC_VOLUME_TARGETS_H

#include "kr_incs.h"
#include "block/targets.h"

struct nvmeibc_disk;
struct nvmeibc_disk_id;

/**
 * nvmeibc_volume_info: filled by the management.  it contains
 * two lists.  the arnics is a list of struct nvmeibc_admin_rnic
 * and the disks is a list of struct nvmeibc_disk.
 * MODIFIED: arnics list is no longer used on the volume itself, rather
 * it is held by the target that holds the disk, the disk_id points to the
 * target to access the list of it's nics
 */
struct nvmeibc_volume_targets {
	struct dp_targets base;

	//struct list_head arnics; // No longer used
	struct list_head disks; /* list of struct nvmeibc_disk_id */
	bool retain_disks; // when draining IO during detaching, do not release disks one by one with each destroyed segment, but rather do it in parallel in the detach SM after destroying topologies
	u64 magic;
};

void nvmeibc_volume_targets_init(struct nvmeibc_volume_targets* self);
struct dp_targets const *nvmeibc_volume_targets_base(struct nvmeibc_volume_targets const* self);
void nvmeibc_volume_targets_add_disk_id(struct nvmeibc_volume_targets* self, struct nvmeibc_disk_id *disk_id);

struct list_head const* __nvmeibc_volume_targets_get_disks_impl(struct nvmeibc_volume_targets const* self);

#define nvmeibc_volume_targets_get_disks(self) \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(self), const struct nvmeibc_volume_targets*), \
        __nvmeibc_volume_targets_get_disks_impl(self), \
        (struct list_head*)__nvmeibc_volume_targets_get_disks_impl(self))


int nvmeibc_volume_targets_count_disks(struct nvmeibc_volume_targets const* self);
bool nvmeibc_volume_targets_should_retain_disks(struct nvmeibc_volume_targets const* self);
void nvmeibc_volume_targets_set_retain_disks(struct nvmeibc_volume_targets* self, bool retain_disks);

struct dp_target_find_disk_result nvmeibc_volume_targets_find_disk_by_name(struct nvmeibc_volume_targets const* self, const char *diskID);
struct nvmeibc_disk_id *nvmeibc_volume_targets_find_disk_id_by_name(struct nvmeibc_volume_targets const* self, const char *disk_name);
struct nvmeibc_disk_id *nvmeibc_volume_targets_find_disk_id_by_disk(struct nvmeibc_volume_targets const* self, struct nvmeibc_disk const* disk);

#endif /* NVMEIBC_VOLUME_TARGETS_H */
