/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_NET_VERSION_H
#define NVMEIBT_NET_VERSION_H

union version {
	struct {
		u8 mr;
		u8 subminor;
		u8 minor;
		u8 major;
	};

	u32 all;
};

/* the TOMA networking version */
#define NVMEIBT_TN_PROTOCOL_VERSION {.major = 2, .minor = 6}

#endif
