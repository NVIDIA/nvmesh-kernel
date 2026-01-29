/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_MEMMGR_METRICS_H_INCLUDED
#define NVMEIBS_MEMMGR_METRICS_H_INCLUDED

#include "nvmeib_memmgr_metrics.h"

#if defined(__KERNEL__)
	#define NVMEIBS_MEMMGR_METRIC(name, labels) NVMESH_DEFINE_MEMMGR_METRIC(name, ".nvmeibs_memmgr_metrics", "module=nvmeibs;name="#name";" labels)
#else
	#define NVMEIBS_MEMMGR_METRIC(name, labels) NVMESH_DEFINE_MEMMGR_METRIC(name, "nvmeibs_memmgr_metrics", "module=nvmeibs;name="#name";" labels)
#endif


/* the diff above is a single "." dot in the section name
 * without dot - if the section name is a valid identifier, ld will generate the symbols below by itself for user space apps
 * with dot - is the only way to work with the Linux kernel module loader. The symbols below are generated
 * during the link time via "nvmeibs.lds" (ld script) */

extern struct nvmesh_memmgr_metrics __start_nvmeibs_memmgr_metrics[];
extern struct nvmesh_memmgr_metrics __stop_nvmeibs_memmgr_metrics[];

ssize_t nvmeibs_memmgr_metrics_info(void *_ctx, char *buffer, size_t len);

#endif // NVMEIBS_MEMMGR_METRICS_H_INCLUDED 