/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_VOLUME_TARGETS_H
#define NVMEIBC_VOLUME_TARGETS_H

#include "kr_incs.h"

/**
 * nvmeibc_volume_info: filled by the management.  it contains
 * two lists.  the arnics is a list of struct nvmeibc_admin_rnic
 * and the disks is a list of struct nvmeibc_disk.
 * MODIFIED: arnics list is no longer used on the volume itself, rather
 * it is held by the target that holds the disk, the disk_id points to the
 * target to access the list of it's nics
 */
struct nvmeibc_volume_targets {
	//struct list_head arnics; // No longer used
	struct list_head disks; /* list of struct nvmeibc_disk_id */
	bool retain_disks; // when draining IO during detaching, do not release disks one by one with each destroyed segment, but rather do it in parallel in the detach SM after destroying topologies
};

#endif /* NVMEIBC_VOLUME_TARGETS_H */
