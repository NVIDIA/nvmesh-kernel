/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_TEST_H
#define NVMEIBS_TEST_H

//#define IB_RAM_TESTING
//#define IB_DISK_TESTING

//#define INDIRECT_INTR

//#define SIM_NO_MSI
#define SIM_NO_MSI_WR_ID 0xdeadbeef

#if defined(IB_RAM_TESTING)

enum {
	VOLUME_SIZE = (1 << 24),
};

#endif

#endif
