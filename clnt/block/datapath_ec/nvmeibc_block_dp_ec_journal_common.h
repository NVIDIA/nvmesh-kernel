/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_EC_JOURNAL_COMMON_H
#define NVMEIBC_DP_EC_JOURNAL_COMMON_H

/* Request journal blocks for Tx, returns <0 on syncronous error, 0 on
   syncronous success and 1, for async cb */
int  dp_ec_journal_alloc_all_areas(  struct nvmeibc_block_command *rldr);
void nvmeibc_block_dp_ec_journal_alloc_cb(int status, u64 *res_jlbas, void *ctx);
bool dp_ec_journal_alloc_is_success( struct nvmeibc_block_command *rldr);
void dp_ec_journal_release_areas(    struct nvmeibc_block_command *rldr);
#endif  // H beginning

