/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
#ifdef DBGDI_REMOVED_IN_PRODUCTION
#else
#include "common_public/nvmeib_uuid_be.h"
#define NVMEIBC_MAX_CORE_DBGDI_IO_INJECTION 32

union t_core_magic_area_cell {
	struct {
		struct nvmeibc_channel_dbg_di_magic_area_uniq {
			u16 lionic_index;
			u16 rionic_index;
			u16 qpn;
			u16 running_uniq;
			uuid_be client_uuid;
		} __attribute__((packed)) uniq;
		u64 counter;
	} __attribute__((packed));
	u64 raw[4];
} __attribute__((packed));

struct nvmeibc_channel_dbg_di_magic_data {
	struct nvmeibc_channel_dbg_di_magic_area_uniq uniq;
	u64 ever_growing;
	struct nvmeibc_channel_dbg_di_magic_data_cell {
		bool is_valid;
		struct {
			u64 dlba;
			u64 jlba;
		} lba;
		union t_core_magic_area_cell stamp;
	} bb_image[NVMEIBC_MAX_CORE_DBGDI_IO_INJECTION];
};

#define nvmeibc_channel_dbg_di_magic_data_gen_stamp(data_, i_)                 \
	({                                                                         \
		if (!++(data_)->ever_growing) (data_)->ever_growing = 1;               \
		(data_)->bb_image[i_].stamp.counter = (data_)->ever_growing;           \
		(data_)->bb_image[i_].stamp.uniq = (data_)->uniq;                      \
		&(data_)->bb_image[i_].stamp;                                          \
	})

#define nvmeibc_magic_stamp_eq(s1_, s2_) !memcmp(s1_, s2_, sizeof(*s1_))

struct t_core_dbgdi_lock_piggyback {
	u32 valid;
	struct {
		u64 type;
		u64 addr;
		u64 val0;
		u64 val1;
	} ulp;
	struct {
		u64 off;
		u64 val;
	} llp;
};

struct t_core_dbgdi_params_pre {
	const char *disk_name;
	u8 ch_type;
	u8 reuse_bb;
	struct t_core_dbgdi_lock_piggyback *lock_pgbk;
	u64 io_id;
	u64 ch_ptr;
	u64 start_dlba;
	struct nvmeibc_channel_dbg_di_magic_data *magic_data; /* read/write param */
};

struct t_core_dbgdi_params_post {
	const char *disk_name;
	u8 ch_type;
	u8 was_overeager;
	int comp_code;
	u64 start_dlba;
	struct nvmeibc_channel_dbg_di_magic_data *magic_data; /* read/write param */
	struct {
		int hits;
		int misses;
	} stats;
};

#define __INIT_MAGIC_AREA_CELL(_cell)                                          \
	({                                                                         \
		BUILD_BUG_ON(ARRAY_SIZE((_cell)->raw) != 4);                           \
		(_cell)->raw[0] = 0;                                                   \
		(_cell)->raw[1] = 0;                                                   \
		(_cell)->raw[2] = 0;                                                   \
		(_cell)->raw[3] = 0;                                                   \
	})

#define __IS_MAGIC_AREA_CELL_ZERO(_cell)                                       \
	({                                                                         \
		BUILD_BUG_ON(ARRAY_SIZE((_cell)->raw) != 4);                           \
		!(_cell)->raw[0] && !(_cell)->raw[1] && !(_cell)->raw[2] &&            \
		    !(_cell)->raw[3];                                                  \
	})

/* t_core_dbgdi_params_pre
   t_core_dbgdi_params_post */
#define NVMEIBC_CORE_DBGDI_PARAM(action_, disk_name_, ch_type_, ...)           \
	((struct t_core_dbgdi_params_##action_){                                   \
	    .disk_name = disk_name_, .ch_type = ch_type_, ##__VA_ARGS__})

#endif	// DBGDI_REMOVED_IN_PRODUCTION

