/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#include "nvmeib_shared.h"
#include "nvmeibc_block_dp_defs.h"
#include "nvmeibc_mcs_stub.h"
#include "nvmeibc_block_dp_locks_scheme.h"

void nvmeibc_locks_scheme_build(struct nvmeibc_locks_scheme *dst, const struct nvmeibc_locks_scheme_conf *src)
{
	dst->type			= (lock_server_type_e)src->type;
	dst->max_n_owners	= src->maxNOwners;
	dst->lockset_shift	= src->locksetShift;
}

static inline int __find_zigzag_dst_seg(u64 slba, int n_members)
{
	return (slba >> LOCKSET_SHIFT) % n_members;
}

int nvmeibc_locks_scheme_find_initial_owner_seg(const struct nvmeibc_locks_scheme *ls, const u64 slba, const int replicas)
{
	const int max_lock_count = min(replicas, (const int)ls->max_n_owners);
	const int n_members = (ls->type == OWNER_SCHEME_FIRST_L_INC_A) ?
			max_lock_count : replicas;
	return __find_zigzag_dst_seg(slba, n_members);
}

int nvmeibc_locks_scheme_find_slice_start_seg(const u64 slba, const int replicas)
{
	return __find_zigzag_dst_seg(slba, replicas);
}
