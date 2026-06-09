/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

#ifndef NVMEIBC_BLOCK_DP_PET_TRACES_TESTS_H
#define NVMEIBC_BLOCK_DP_PET_TRACES_TESTS_H

void test_pet_traces_sync_common(void);
void test_pet_traces_sync_no_write_hole(void);
void test_pet_traces_mirror(void);
void test_pet_traces_io_req_rel_locks(void);

#endif /* NVMEIBC_BLOCK_DP_PET_TRACES_TESTS_H */
