/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_DP_SYNC_TXID_WRAPAROUND_H
#define NVMEIBC_BLOCK_DP_SYNC_TXID_WRAPAROUND_H

struct recovery_sync_op;
void dp_ec_sync_txid_wraparound_cb_stg_end(struct recovery_sync_op *so);
void dp_ec_sync_txid_wraparound_execute_op(struct recovery_sync_op *so);


/* PET trace declarations */
void pet_trace_txid_wrap_err_at(const struct recovery_sync_op *so, u16 line);
#define pet_trace_txid_wrap_err(so) \
	pet_trace_txid_wrap_err_at((so), (u16)__LINE__)

#endif // NVMEIBC_BLOCK_DP_SYNC_TXID_WRAPAROUND_H
