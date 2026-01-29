/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DECENTRALIZED_UNREG_ALGO_H
#define NVMEIBC_DECENTRALIZED_UNREG_ALGO_H

enum stale_lock_resolve_status {		// Steps of resolving stale lock
	stale_lock_resolve_broken      =-1,	// Resolver is broken, cannot function
	stale_lock_resolve_unkown      = 0,	// kzallocked() / unknown
	stale_lock_resolve_asked_toma  = 1, // Waiting for answer from 1 or more Toma's
	stale_lock_resolve_partial_ans = 2, // At least 1 Toma answered that lock was cleaned but not all did.
	stale_lock_resolve_safe_to_use = 3, // Safe to start sync: All Toma's answered that no io is in air to the lock
};

/* Is lock id stale */
#define nvmeibc_sync_is_stale(lock_ptr, u64_lock) \
	(!!((lock_ptr)->comp.lock_cnsts->stale_bit_mask & u64_lock))

#define nvmeibc_sync_is_read_only(lock_ptr, u64_lock) ({ \
	bool __rv; \
	if ((lock_ptr)->comp.lock_cnsts->w_blkset_info) { \
		const union nvmeib_lock_blkset_entry* plid = ((void*)&(u64_lock)); \
		__rv = plid->lock_id.bits.is_read; \
	} else { \
		__rv = false; \
	} \
	__rv; \
})
#endif // NVMEIBC_DECENTRALIZED_UNREG_ALGO_H
