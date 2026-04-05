/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef __TARGETS_H__
#define __TARGETS_H__

struct nvmeibc_idisk;

struct dp_target_find_disk_result{
    struct nvmeibc_idisk* disk;
    struct nvmeib_io_stats* v_disk_stats;
};

struct dp_targets{
    struct dp_target_find_disk_result (*find_disk_by_name)(struct dp_targets const* self, const char *diskID);
};

#endif//__TARGETS_H__