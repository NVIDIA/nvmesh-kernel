/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_B_CP_TRS_HASH_H
#define NVMEIBC_B_CP_TRS_HASH_H

/* Manages a hash map of toma connections per client. Given a hash key
   can find the a connection (unique for {volume,chunk,raid,seg}).
   Used when receiveng messages from Toma.
   Transport layer gives a message + hash key, and this class returns the.
   channel of block layer (segment) that can serve the message

   Hash table of trs (toma registrants = volume segments) of all block
   devices. Stores in rb-tree tuples {u64 handle, struct nvmeibc_subscription_ctx* value}.
   When message from Toma arrives it caries the key 'handle' by which
   we can find the tr.
   Each 'tr' has a refcount. get() find in hash-table and increases refcount,
   Remember to do put() after each get() */

struct nvmeibc_subscription_ctx *tr_create(void); // No explicit destructor due to kref
//void __tr_destroy(struct nvmeibc_subscription_ctx *tr)
void tr_destroy_unused(struct nvmeibc_subscription_ctx *tr); // Called when construction failed

/* Insert tr to hash table. Allocate unique key to it (stored in tr->handle).
   After completion of this function get_tr(handle) will return 'tr'.
   tr->handle is used to subscribe segment to transport layer (its hash table)*/
void nvmeibc_trs_hash_insert(   struct nvmeibc_subscription_ctx *tr);

/* 0 on success, negative on error */
int  nvmeibc_trs_hash_subscribe(struct nvmeibc_subscription_ctx *tr,
								struct nvmeibc_disk_subscription_params *p);

/* After completion of this function get(handle) will return 'NULL'. (Use count
   will not rise. Deleting tr is still unsafe coz use count might != 0 */
void nvmeibc_trs_hash_remove(struct nvmeibc_subscription_ctx *tr);

/* Get tr by handle: Used for receiving msgs from Toma, refcount++ */
struct nvmeibc_subscription_ctx *__get_tr(u64 handle);

/* Inverse of the above, refcount-- */
void __put_tr(struct nvmeibc_subscription_ctx *tr);
void __add_ref_to_tr(struct nvmeibc_subscription_ctx *tr);

#endif

