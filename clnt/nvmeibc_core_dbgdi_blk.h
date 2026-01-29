/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once

#include "nvmeibc_core_dbgdi_shared.h"

#ifdef DBGDI_REMOVED_IN_PRODUCTION
#else
#define NVMEIBC_CORE_DBG_DI_MAGIC_POISON (0x73696f5065726f43LL)    /* CorePois */
#define NVMEIBC_CORE_DBG_DI_MAGIC_INV (0x21766e4965726f43LL)    /* CoreInv! */
#define NVMEIBC_CORE_DBG_DI_MAGIC_WR (0x7274725765726f43LL)     /* CoreWrtr */
#define NVMEIBC_CORE_DBG_DI_MAGIC_RD (0x2172645265726f43LL)     /* CoreRdr! */
#define NVMEIBC_CORE_DBG_DI_MAGIC_ERR (0x2172724565726f43LL)    /* CoreErr! */

/* The two below constants define the magic area behaviour, the rest are
 * calculated */
/* The total size of magic area in bytes (512) */
#define NVMEIBC_MAGIC_AREA_TOTAL_SIZE_BYTES 512
/* The whole magic area is divided into smaller areas of this size, each one
 * stamped individually (64) */
#define NVMEIBC_MAGIC_AREA_ATOMIC_SIZE_BYTES 64

#define NVMEIBC_CORE_DBG_DI_POSION_AREA_ARR_SIZE                               \
	(NVMEIBC_MAGIC_AREA_TOTAL_SIZE_BYTES /                                     \
	 sizeof(union t_core_magic_area_cell))
#define NVMEIBC_CORE_DBG_DI_POSION_AREA_GRANULARITY                            \
	(NVMEIBC_MAGIC_AREA_ATOMIC_SIZE_BYTES /                                    \
	 sizeof(union t_core_magic_area_cell))

#define CORE_DBGDI_WR_MAX_MIRROR 2

struct t_core_dbgdi_magic_area {
	union t_core_magic_area_cell a[NVMEIBC_CORE_DBG_DI_POSION_AREA_ARR_SIZE];
};

struct t_core_dbgdi_rd {
	u64 magic;
	u8 op;
	u8 ch_type;
	u8 was_overeager;
	u8 unused;
	u32 comp_code;
	u64 container_ptr;
} __attribute__((aligned(sizeof(long))));

struct t_core_dbgdi_wr {
	u64 magic;
	char disk_name[40];
	u8 op;
	u8 ch_type;
	u8 reuse_bb;
	struct t_core_dbgdi_lock_piggyback lock_pgbk;
	struct t_core_dbgdi_magic_area magic_area;
	u64 container_ptr;
	u64 io_id;
	u64 ch_ptr;
	struct {
		u64 dlba;
		u64 jlba;
	} lba;
} __attribute__((aligned(sizeof(long))));


struct t_core_dbgdi {
	struct t_core_dbgdi_rd rd; /* on wr-op, this info is invalidated before post-send/nvme-doorbell ; on rd-op, this info is filled on comp to ulp with intended action*/
	struct t_core_dbgdi_wr wr [CORE_DBGDI_WR_MAX_MIRROR]; /* on wr-op, this info is filled before post-send/nvme-doorbell ; on rd-op, this info is filled with data we've read from disk */
};

#endif	// DBGDI_REMOVED_IN_PRODUCTION
