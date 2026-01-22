/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DECENTRALIZED_UNREG_H
#define NVMEIBC_DECENTRALIZED_UNREG_H
/* Implementation of cache for stale locks of other clients. When my IO
   encounters a stale lock (not stale special), the question is whether I can
   take the lock or not. Generaly I can if Toma's purged all IO's of that client
   from pipes of disks. */
#include "nvmeibc_block.h"		/* external API of the block */
#include "nvmeibc_decentralized_unreg_algo.h"

struct nvmeib_txt;

// Daniel, Todo: After cache is well debugged, close it in .c file, not .h
struct _lock_cache_elem_t;
struct stale_lock_resolver_cache_t {// LRU type cache for already resolved stale locks
	struct   	list_head head;		// Points to the newest record of resolved stle lock
	unsigned    size;				// Amount of records (locks) in this cache
	struct 		rb_root cache_root; // Hash-map to find locks really fast
	spinlock_t lock;				// Protects the struct. Daniel, Todo: Change to Readers writers lock for faster access
	struct _lock_cache_elem_t *elems_arr;	// Preallocates array of elements, save mallocs
	// ------- Debug stats
	ulong longest_resolve;			// The longest time to get all results from toma
};
int  stale_lock_resolver_cache_t_create( struct stale_lock_resolver_cache_t *c);
void stale_lock_resolver_cache_t_destroy(struct stale_lock_resolver_cache_t *c);
void stale_lock_resolver_cache_t_clear(  struct stale_lock_resolver_cache_t *c);
void stale_lock_resolver_cache_t_to_log(const struct stale_lock_resolver_cache_t *c);
/* Access the cache in 2 options:
   1. Store pair (lock_id, status) or update it with resolving progress.
   2. Try-increase-status (when IO encounters a stale lock, it attempts to
      stale_lock_resolve_asked_toma). If status was already this or higher -->
      other IO already asked toma's. Only the first resolve request triggers
	  toma message.
   answers_i - Bitmap of which answers from different Tomas are needed,
      to decide that lock is safe. Also used to turn bits off when incomming
      Toma answers.
   cuuid - payload of toma answer. Needed for erasure coding */
enum stale_lock_resolve_status stale_lock_resolver_cache_t_store(
	struct stale_lock_resolver_cache_t *c, u32 lock_id, u32 answers_i,
	enum stale_lock_resolve_status status, const uuid_be *cuuid);
//enum stale_lock_resolve_status stale_lock_resolver_cache_t_load(
//	struct stale_lock_resolver_cache_t *c, u32 lock_id);

/************************** stale lock resolver *******************************/
struct stale_lock_resolver_t {
	#ifndef DP_LIB
		struct stale_lock_resolver_cache_t cache;
	#else
		void *self;			// In DP_lib stale lock resolver logic is supplied externally, and this struct points to external context
	#endif
};

int  stale_lock_resolver_get_create( struct stale_lock_resolver_t *slr);
void stale_lock_resolver_get_destroy(struct stale_lock_resolver_t *slr);

/* When IO encounters stale lock it asks the resolver for help */
enum stale_lock_resolve_status stale_lock_resolver_get_status(
	struct stale_lock_resolver_t *slr, u32 lock_id,
	const struct nvmeibc_cmd_lock *l);

/* Getter for recoveree client UUID by Lock ID (call when lock is safe to use),
   In rare case returns negative error, if LRU was purged and safe lock deleted */
int stale_lock_resolver_fill_cuuid_by_lockid(
	struct stale_lock_resolver_t *slr, u32 lock_id, uuid_be *cuuid);

/* When Toma answers with message that lock is safe to use, update that it was
   successfully partially resolved (need answer from all toma's) */
struct nvmeibt_cleaned_stalock_info;
int stale_lock_resolver_set_resolved(struct stale_lock_resolver_t *slr,
			int seg_ind_in_raid, struct nvmeibt_cleaned_stalock_info* pl);

/* Clear the cache of all the resolved locks (speeds up the search in cache) */
void stale_lock_resolver_clear_all(struct stale_lock_resolver_t *slr);
void stale_lock_resolver_to_str(const struct stale_lock_resolver_t *slr,
								struct nvmeib_txt *txt);
void stale_lock_resolver_to_log(const struct stale_lock_resolver_t *slr);
#endif // NVMEIBC_DECENTRALIZED_UNREG_H
