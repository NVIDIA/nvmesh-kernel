/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_ICORE_OPS_H
#define NVMEIBC_ICORE_OPS_H

#include "nvmeibc_disk.h"

struct nvmeibc_icore_ops {
	int (*toma_send)(struct nvmeibc_icore_ops const *ops,
			 struct nvmeibc_disk *disk, u64 handle,
			 struct nvmeibc_disk_toma_send_params *params);
};

struct nvmeibc_icore_ops const *nvmeibc_core_ops_get(void);

#endif /* NVMEIBC_ICORE_OPS_H */
