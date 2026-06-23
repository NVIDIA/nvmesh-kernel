/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_HASH_H
#define NVMEIB_HASH_H

#include "../common_public/nvmeib_uuid_be.h"

#define HASH_ENTRY_EMPTY		(void *)0LL
#define HASH_MIN_LOG2_OF_N_ARR_ENTRIES 5

union nvmeib_hash_key {							// 16[b] key
	uint8_t		c[16];
	int64_t		ll[2];
	int32_t		i[4];
	struct {
		const char *str;						// External null terminating buffer. Must be valid and live while key is in hash table
		size_t	len;
	} ascii_key;
};

struct nvmeib_hash_entry {
	union nvmeib_hash_key			key;			// 16
	void							*ptr_to_obj;	// 24	Pointer to value, NULL if unused entry
	uint32_t						scrambled;		// 28	Hashed key. Hint for the initial position in hash table array (due to collision can be shifted slightly)
	int								filler_for_cacheline_alignment;	// 32
};

struct nvmeib_hash_table {		// Note that during resize, we keep the object, and allocate a new arr inside it
	char							description[128];
	uint64_t						scrambled_to_idx_mask;			// n_arr_entries-1
	struct nvmeib_hash_entry		*arr;
	int								log2_of_n_arr_entries;
	int								initial_log2_of_n_arr_entries;
	int 							n_arr_entries;					// Pow2. Entire math is done with ints (31bits) so dont hashes which are more than 1[billion] entries or so.
	int								n_occupied;
	bool							is_used_outside_main_thread;    // Protected by an external mutex. Skip idle resize by toma main thread; callers synchronize all access.
	int8_t							key_len;						// -1 for ascii str, otherwise size of the key (supports 4,8,16[b]). Set once upon create
};

#define NVMEIB_HASH_DUMP_STATISTICS(_name_, h) ({ \
	N_Tf(_name_, "HTBL @STR[@INT8_TD[b]] @INT/@INT", h->description, h->key_len, h->n_occupied, h->n_arr_entries);	\
})

// NVMEIB_HASH_FOREACH is delicate since another thread might delete entries, and this moves entries backwards. It is unsafe (not guarded by mutex). USe it with care
// Deletion: It might be that a new entry moved into the deleted "idx", so we progress to the next only if either empty of same ptr_to_obj as before. Unfortunately, the for loop does not allow two different types in the first clause
#define NVMEIB_HASH_FOREACH(__x__, __hash_tbl) 																																		\
	if ((__hash_tbl) && (__hash_tbl)->n_occupied)																																	\
		for (	struct nvmeib_hash_entry *__e ## __x__ = &((__hash_tbl)->arr[0]), __OLD_ ## __x__ = *(__e ## __x__);																\
				(__e ## __x__) < ((__hash_tbl)->arr + (__hash_tbl)->n_arr_entries);																									\
				(__e ## __x__) = (!hash_is_entry_OCCUPIED(__e ## __x__) || (__e ## __x__)->ptr_to_obj == (__OLD_ ## __x__).ptr_to_obj ?												\
								  (__e ## __x__) + 1 : (__e ## __x__)),																												\
								  (__OLD_ ## __x__) = ((__e ## __x__) < ((__hash_tbl)->arr + (__hash_tbl)->n_arr_entries) ? (*(__e ## __x__)) : (__OLD_ ## __x__)))					\
			if (hash_is_entry_OCCUPIED(__e ## __x__) && (__x__ = (__e ## __x__)->ptr_to_obj))

static inline bool hash_is_entry_OCCUPIED(const struct nvmeib_hash_entry *entry) { return (entry->ptr_to_obj != HASH_ENTRY_EMPTY); }
static inline int nvmeib_hash_get_n_elements(const struct nvmeib_hash_table *hash_tbl) { return (hash_tbl ? hash_tbl->n_occupied : 0); } // No need to lock. hash_tbl itself survives add/del/resize

struct nvmeib_hash_table *nvmeib_hash_create(int log2_of_n_arr_entries, const char *description, int8_t key_len, bool is_used_outside_main_thread);
#define NVMEIB_HASH_CREATE(name_, log2_of_n_arr_entries_, desc_, key_len_, is_used_outside_main_thread_) ({								\
	struct nvmeib_hash_table	*__ht__ = nvmeib_hash_create(log2_of_n_arr_entries_, desc_, key_len_, is_used_outside_main_thread_);	\
	NVMEIB_HASH_DUMP_STATISTICS(name_, __ht__);																							\
	__ht__;																																\
})

#define NVMEIB_HASH_TBL_FREE(name_, __hash_tbl) ({								\
	if (__hash_tbl) {															\
		NVMEIB_HASH_DUMP_STATISTICS(name_, __hash_tbl);							\
		nvmeib_hash_tbl_free(__hash_tbl);										\
		__hash_tbl = NULL;														\
	}																			\
})

#define NVMEIB_HASH_ADD_UUID(name_, __ht__, uuid__, ptr_to_obj__) ({			\
	void *__old_obj = nvmeib_hash_add_uuid(__ht__, uuid__, ptr_to_obj__);		\
	NVMEIB_HASH_DUMP_STATISTICS(name_, __ht__);									\
	__old_obj;																	\
})

void *nvmeib_hash_add_uuid(        struct nvmeib_hash_table *, const union nvmeib_uuid *key, void *ptr_to_obj);
void *nvmeib_hash_add_uint32_t(    struct nvmeib_hash_table *, const uint32_t           key, void *ptr_to_obj);
void *nvmeib_hash_add_uint64_t(    struct nvmeib_hash_table *, const uint64_t           key, void *ptr_to_obj);
void *nvmeib_hash_add_ascii_str(   struct nvmeib_hash_table *, const char *             key, void *ptr_to_obj);	// Note that ascii_str_key should point to a string inside ptr_to_obj!
void *nvmeib_hash_search_uuid(     struct nvmeib_hash_table *, const union nvmeib_uuid *key);
void *nvmeib_hash_search_uint32_t( struct nvmeib_hash_table *, const uint32_t           key);
void *nvmeib_hash_search_uint64_t( struct nvmeib_hash_table *, const uint64_t           key);
void *nvmeib_hash_search_ascii_str(struct nvmeib_hash_table *, const char *             key);
void *nvmeib_hash_delete_uuid(     struct nvmeib_hash_table *, const union nvmeib_uuid *key);
void *nvmeib_hash_delete_uint32_t( struct nvmeib_hash_table *, const uint32_t           key);
void *nvmeib_hash_delete_uint64_t( struct nvmeib_hash_table *, const uint64_t           key);
void *nvmeib_hash_delete_ascii_str(struct nvmeib_hash_table *, const char *             key);
void nvmeib_hash_resize_all_tables_as_needed(void);
void nvmeib_hash_dump_tbl(         struct nvmeib_hash_table *);
void nvmeib_hash_tbl_free(         struct nvmeib_hash_table *);
void nvmeib_hash_free_all_tables(void);

#endif	// #ifndef NVMEIB_HASH_H
