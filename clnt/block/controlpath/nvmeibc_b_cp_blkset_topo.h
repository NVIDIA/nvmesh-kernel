/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_B_CP_BLKSET_TOPO_H
#define NVMEIBC_B_CP_BLKSET_TOPO_H

#include "block/nvmeibc_topology.h"

static inline struct nvmeibc_roles_bmps nvmeibc_roles_bmps_get_by_slba(const struct nvmeibc_raid1 *praid, const u64 slba) {
	const u16 d0_sgmnt_id = (slba >> LOCKSET_SHIFT) % praid->replicas;
	return nvmeibc_raid1_get_calculated_data(praid)->roles_bmps[d0_sgmnt_id];
}

static inline roles_bmp_t nvmeibc_get_nonrw_roles(const struct nvmeibc_roles_bmps bmps){
	return bmps.w | bmps.dead;
}

/******************************************************************************/
#endif

