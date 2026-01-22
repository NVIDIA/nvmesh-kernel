/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_MEMMGR_METRICS_SELF_H_INCLUDED
#define NVMEIB_MEMMGR_METRICS_SELF_H_INCLUDED

#include "nvmeib_memmgr_metrics.h"

#define NVMEIB_MEMMGR_METRIC(name, labels) NVMESH_DEFINE_MEMMGR_METRIC(name, "nvmeib_memmgr_metrics", "module=nvmeib;name="#name";" labels)

extern struct nvmesh_memmgr_metrics __start_nvmeib_memmgr_metrics[];
extern struct nvmesh_memmgr_metrics __stop_nvmeib_memmgr_metrics[];

#endif // NVMEIBC_MEMMGR_METRICS_SELF_H_INCLUDED
