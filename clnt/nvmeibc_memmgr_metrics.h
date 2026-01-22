/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MEMMGR_METRICS_H_INCLUDED
#define NVMEIBC_MEMMGR_METRICS_H_INCLUDED

#include "nvmeib_memmgr_metrics.h"

#define NVMEIBC_MEMMGR_METRIC(name, labels) NVMESH_DEFINE_MEMMGR_METRIC(name, "nvmeibc_memmgr_metrics", "module=nvmeibc;name="#name";" labels)

extern struct nvmesh_memmgr_metrics __start_nvmeibc_memmgr_metrics[];
extern struct nvmesh_memmgr_metrics __stop_nvmeibc_memmgr_metrics[];

ssize_t nvmeibc_memmgr_metrics_info(void *_ctx, char *buffer, size_t len);

#endif // NVMEIBC_MEMMGR_METRICS_H_INCLUDED
