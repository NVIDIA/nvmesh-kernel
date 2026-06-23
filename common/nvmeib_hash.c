/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "toma/nvmeibt_common.h"

#define NVMEIB_HASH_ASSERT(name, cond, fmt, ...) ({			\
	if (unlikely(!(cond))) {								\
		N_Ef(name, fmt, ## __VA_ARGS__);					\
		nvmeibt_abort(ES_FATAL);							\
	}														\
})

#include "nvmeib_hash.h"

#if IS_HASH_UNITTEST
	#undef HASH_MIN_LOG2_OF_N_ARR_ENTRIES
	#define HASH_MIN_LOG2_OF_N_ARR_ENTRIES 3
#endif

#define HASH_MIN_N_ARR_ENTRIES		(0x1 << HASH_MIN_LOG2_OF_N_ARR_ENTRIES)
#define HASH_EMERGENCY_LOAD_FACTOR_THRESHOLD(n_entries)		(((n_entries) * 60) / 100)
#define HASH_RELAXED_LOAD_FACTOR_THRESHOLD(n_entries)		(((n_entries) * 51) / 100)
#define HASH_SHRINK_FACTOR_THRESHOLD				10

static bool is_hash_tbl_suitable_for_resize_relaxed_increase(struct nvmeib_hash_table *hash_tbl)
{
	return (hash_tbl->n_occupied >= HASH_RELAXED_LOAD_FACTOR_THRESHOLD(hash_tbl->n_arr_entries));
}

static bool is_hash_tbl_suitable_for_resize_emergency_increase(struct nvmeib_hash_table *hash_tbl)
{
	return (hash_tbl->n_occupied >= HASH_EMERGENCY_LOAD_FACTOR_THRESHOLD(hash_tbl->n_arr_entries) ||
			(hash_tbl->is_used_outside_main_thread && is_hash_tbl_suitable_for_resize_relaxed_increase(hash_tbl)));	// Since idle_time_activities will not resize, we don't want to wait
}

static inline uint32_t hash_scrambled_to_idx(const uint32_t scrambled, const uint32_t scrambled_to_idx_mask)
{
	return (scrambled & scrambled_to_idx_mask);
}

/* From https://en.wikipedia.org/wiki/MurmurHash */
static inline uint32_t murmur_32_scramble(uint32_t k) {
	k *= 0xcc9e2d51;
	k = (k << 15) | (k >> 17);
	k *= 0x1b873593;
	return k;
}

static inline uint32_t murmur3_32(const uint8_t* key, size_t len, uint32_t seed)
{
	uint32_t h = seed;
	uint32_t k;
	/* Read in groups of 4. */
	for (size_t i = len >> 2; i; i--) {
		// Here is a source of differing results across endiannesses.
		// A swap here has no effects on hash properties though.
		memcpy(&k, key, sizeof(uint32_t));
		key += sizeof(uint32_t);
		h ^= murmur_32_scramble(k);
		h = (h << 13) | (h >> 19);
		h = h * 5 + 0xe6546b64;
	}
	/* Read the rest. */
	k = 0;
	for (size_t i = len & 3; i; i--) {
		k <<= 8;
		k |= key[i - 1];
	}
	// A swap is *not* necessary here because the preceding loop already
	// places the low bytes in the low places according to whatever endianness
	// we use. Swaps only apply when the memory is copied in a chunk.
	h ^= murmur_32_scramble(k);
	/* Finalize. */
	h ^= len;
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

static inline uint32_t hash_scramble_uuid(const union nvmeib_hash_key *uuid)
{
	return murmur3_32(&(uuid->c[0]), sizeof(*uuid), 0);
	// return (uuid->i[0] ^ uuid->i[1] ^ uuid->i[2] ^ uuid->i[3]);	// Dont use 4 xor's as they generated huge chains for deletions with no empty in the synthetic (serial) test
}

static inline uint32_t hash_next_idx_on_collision(const uint32_t idx, const uint32_t scrambled_to_idx_mask)
{
	return (idx + 1) & scrambled_to_idx_mask;
}

static inline bool hash_is_same_key_OCCUPIED(const union nvmeib_hash_key *key1, const union nvmeib_hash_key *key2, int8_t key_len)
{
	if (key_len == -1) {    // is ascii
		return (key1->ascii_key.len == key2->ascii_key.len) && (strncmp(key1->ascii_key.str, key2->ascii_key.str, key1->ascii_key.len) == 0);
	} else {
		return (memcmp(key1, key2, sizeof(*key1)) == 0);
	}
}

static inline union nvmeib_hash_key hash_uint32_t_to_nvmeib_hash_key(const uint32_t uint32_t_key)
{
	return (union nvmeib_hash_key){.i = {uint32_t_key, 0, 0, uint32_t_key}};
}

static inline union nvmeib_hash_key hash_uint64_t_to_nvmeib_hash_key(const uint64_t uint64_t_key)
{
	return (union nvmeib_hash_key){.ll = {uint64_t_key, uint64_t_key}};
}

static inline const union nvmeib_hash_key *hash_uuid_to_nvmeib_hash_key(const union nvmeib_uuid *uuid)
{
	return (union nvmeib_hash_key *)uuid;
}

/******************************************************************************/
static void hash_init_arr(struct nvmeib_hash_table *hash_tbl, int log2_of_n_arr_entries)
{
	hash_tbl->log2_of_n_arr_entries = max(log2_of_n_arr_entries, HASH_MIN_LOG2_OF_N_ARR_ENTRIES);
	hash_tbl->n_arr_entries = (1 << hash_tbl->log2_of_n_arr_entries);	// Use a power of 2 It must be a power of 2 if we want to use any odd stride
	hash_tbl->n_occupied = 0;
	hash_tbl->scrambled_to_idx_mask = hash_tbl->n_arr_entries - 1;
	hash_tbl->arr = calloc(hash_tbl->n_arr_entries, sizeof(hash_tbl->arr[0]));	// Deliberate calloc as it fills the array with HASH_ENTRY_EMPTY
}

static void nvmeib_hash_resize(struct nvmeib_hash_table *hash_tbl)
{
	struct nvmeib_hash_entry	*old_arr;
	int							old_n_arr_entries;
	int							i;
	struct timespec				start_timespec;
	struct timespec				end_timespec;

	// Might be growing or shrinking
	if (is_hash_tbl_suitable_for_resize_relaxed_increase(hash_tbl)) {
		// Note we also get here in case of EMERGENCY (on-add)
		hash_tbl->log2_of_n_arr_entries = hash_tbl->log2_of_n_arr_entries + 1;
	} else if (hash_tbl->n_occupied * HASH_SHRINK_FACTOR_THRESHOLD < hash_tbl->n_arr_entries) {
		if (	(hash_tbl->n_arr_entries < HASH_MIN_N_ARR_ENTRIES * 2 ||
				 hash_tbl->log2_of_n_arr_entries <= hash_tbl->initial_log2_of_n_arr_entries)) {	// Say was CREATEd with a large table, and just starting to add. Do not shrink below the initial size
			goto out;	// Avoid shrinking too much
		}
		hash_tbl->log2_of_n_arr_entries = hash_tbl->log2_of_n_arr_entries - 1;
	} else {
		goto out;   // Good
	}
	getnstimeofday_boot(&start_timespec);
	old_arr = hash_tbl->arr;
	old_n_arr_entries = hash_tbl->n_arr_entries;
	hash_init_arr(hash_tbl, hash_tbl->log2_of_n_arr_entries);
	N_Tf(crgauy4, "@STR @INT->@INT", hash_tbl->description, old_n_arr_entries, hash_tbl->n_arr_entries);
	// Copy the arr
	for (i = 0; i < old_n_arr_entries; i++) {
		if (hash_is_entry_OCCUPIED(&old_arr[i])) {
			int		n_collisions = 0;
			// Re-add existing keys
			int idx = hash_scrambled_to_idx(old_arr[i].scrambled, hash_tbl->scrambled_to_idx_mask);
			while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
				if (n_collisions++ > hash_tbl->n_arr_entries) {
					N_Ef(cr78shj, "@STR n_collisions=@INT n_arr_entries=@INT", hash_tbl->description, n_collisions, hash_tbl->n_arr_entries);
					break;
				}
				idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
			}
			hash_tbl->arr[idx] = old_arr[i];
			hash_tbl->n_occupied++;
		}
	}
	free(old_arr);
	getnstimeofday_boot(&end_timespec);
out:
	return;
}

static void *_hash_add(struct nvmeib_hash_table *hash_tbl, const union nvmeib_hash_key *key, const uint32_t scrambled, void *in_ptr_to_obj, int expected_key_len)
{
	bool		is_found = 0;
	int			idx;
	int			n_collisions = 0;
	void		*rv_ptr_to_obj = NULL;

	{ _Static_assert(sizeof(union nvmeib_hash_key   ) == 16, "nvmeib_hash_key wrong size"); }
	{ _Static_assert(sizeof(struct nvmeib_hash_entry) == 32, "nvmeib_hash_entry wrong size"); }
	NVMEIB_HASH_ASSERT(vaj3nq2, hash_tbl->key_len == expected_key_len, "@STR hash_tbl->key_len=@INT, contradicts the inserted key len=@INT!", hash_tbl->description, hash_tbl->key_len, expected_key_len);
	NVMEIB_HASH_ASSERT(vaj3nq3, in_ptr_to_obj != HASH_ENTRY_EMPTY,     "@STR hash_tbl cannot add NULL pointer as it is considered empty",     hash_tbl->description);

	while (is_hash_tbl_suitable_for_resize_emergency_increase(hash_tbl)) {
		nvmeib_hash_resize(hash_tbl);
	}
	idx = hash_scrambled_to_idx(scrambled, hash_tbl->scrambled_to_idx_mask);
	while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
		if (n_collisions++ > hash_tbl->n_arr_entries) {
			N_Ef(vg5h8ak, "@STR: OOPS n_collisions=@INT, cannot add element", hash_tbl->description, n_collisions);
			goto out;	// In real system should never happen, it is OK to crash
		}
		is_found = hash_is_same_key_OCCUPIED(&(hash_tbl->arr[idx].key), key, hash_tbl->key_len);
		if (is_found) {
			rv_ptr_to_obj = hash_tbl->arr[idx].ptr_to_obj;		// Policy: only add-if-absent. If exists return previous value
			goto out;
		}
		idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
	}
	// EMPTY
	hash_tbl->n_occupied++;
	hash_tbl->arr[idx].key = *key;
	hash_tbl->arr[idx].ptr_to_obj = in_ptr_to_obj;
	hash_tbl->arr[idx].scrambled = scrambled;
	rv_ptr_to_obj = NULL;
out:
	// NVMEIB_HASH_DUMP_STATISTICS(4cghs89, hash_tbl);
	return rv_ptr_to_obj;
}

static void *hash_search(struct nvmeib_hash_table *hash_tbl, const union nvmeib_hash_key *key, const uint32_t scrambled, int expected_key_len)
{
	int			idx;
	void		*ptr_to_obj = NULL;
	int			n_collisions = 0;

	NVMEIB_HASH_ASSERT(vaj3nq5, hash_tbl->key_len == expected_key_len, "@STR hash_tbl->key_len=@INT, contradicts the inserted key len=@INT!", hash_tbl->description, hash_tbl->key_len, expected_key_len);
	idx = hash_scrambled_to_idx(scrambled, hash_tbl->scrambled_to_idx_mask);
	while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
		if (hash_is_same_key_OCCUPIED(&(hash_tbl->arr[idx].key), key, hash_tbl->key_len)) {
			ptr_to_obj = hash_tbl->arr[idx].ptr_to_obj;
			break;
		}
		if (n_collisions++ >= hash_tbl->n_arr_entries)
			break;	// Not found
		idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
	}
	return ptr_to_obj;
}

static void *hash_delete_key(struct nvmeib_hash_table *hash_tbl, const union nvmeib_hash_key *key, const uint32_t scrambled, int expected_key_len)
{
	int			n_collisions = 0;
	int			idx, prev_deleted_idx, idx_according_to_scrambled;
	int			gap_from_prev_deleted_to_idx_according_to_scrambled;
	int			gap_from_prev_deleted_to_idx;
	bool		is_idx_according_to_scrambled_beyond_deleted;
	void		*deleted_ptr_to_obj = NULL;

	NVMEIB_HASH_ASSERT(vaj3nq7, hash_tbl->key_len == expected_key_len, "@STR hash_tbl->key_len=@INT, contradicts the inserted key len=@INT!", hash_tbl->description, hash_tbl->key_len, expected_key_len);
	idx = hash_scrambled_to_idx(scrambled, hash_tbl->scrambled_to_idx_mask);
	while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
		if (hash_is_same_key_OCCUPIED(&(hash_tbl->arr[idx].key), key, hash_tbl->key_len)) {
			deleted_ptr_to_obj = hash_tbl->arr[idx].ptr_to_obj;
			// Push following entries backwards; skip entries whose probe path does not cross the current hole.
			prev_deleted_idx = idx;
			do {
				idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
				if (!hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
					break;	// End of chain
				}
				idx_according_to_scrambled = (hash_tbl->arr[idx].scrambled & hash_tbl->scrambled_to_idx_mask);
				gap_from_prev_deleted_to_idx_according_to_scrambled = ((idx_according_to_scrambled - prev_deleted_idx) & hash_tbl->scrambled_to_idx_mask);
				gap_from_prev_deleted_to_idx =                        ((idx                        - prev_deleted_idx) & hash_tbl->scrambled_to_idx_mask);
				is_idx_according_to_scrambled_beyond_deleted = (gap_from_prev_deleted_to_idx_according_to_scrambled > 0 &&
																gap_from_prev_deleted_to_idx_according_to_scrambled <= gap_from_prev_deleted_to_idx);
				if (is_idx_according_to_scrambled_beyond_deleted) {
					continue;	// The free prev_deleted_entry is before our chain
				}
				// Can be pushed backwards (over the deleted/moved entry)
				hash_tbl->arr[prev_deleted_idx] = hash_tbl->arr[idx];
				prev_deleted_idx = idx;
			} while (1);
			// Mark as deleted
			hash_tbl->n_occupied--;
			hash_tbl->arr[prev_deleted_idx].ptr_to_obj = HASH_ENTRY_EMPTY;
			break;
		}
		if (n_collisions++ >= hash_tbl->n_arr_entries) {
			break;	// Not found
		}
		idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
	}
	return deleted_ptr_to_obj;
}

void *nvmeib_hash_add_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid, void *ptr_to_obj)
{
	const union nvmeib_hash_key	key = *hash_uuid_to_nvmeib_hash_key(uuid);
	const uint32_t scrambled = hash_scramble_uuid(&key);
	return _hash_add(hash_tbl, &key, scrambled, ptr_to_obj, 16);
}

void *nvmeib_hash_search_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid_key)
{
	const union nvmeib_hash_key	key = *hash_uuid_to_nvmeib_hash_key(uuid_key);
	const uint32_t scrambled = hash_scramble_uuid(&key);
	return hash_search(hash_tbl, &key, scrambled, 16);
}

void *nvmeib_hash_delete_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid_key)
{
	const union nvmeib_hash_key	key = *hash_uuid_to_nvmeib_hash_key(uuid_key);
	const uint32_t scrambled = hash_scramble_uuid(&key);
	return hash_delete_key(hash_tbl, &key, scrambled, 16);
}

void *nvmeib_hash_add_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key, void *ptr_to_obj) {
	const union nvmeib_hash_key	key = hash_uint32_t_to_nvmeib_hash_key(uint32_t_key);
	const uint32_t scrambled = murmur3_32( (uint8_t *)(&uint32_t_key), 4, 0);	// Don't use murmur_32_scramble() - too many collisions;
	return _hash_add(hash_tbl, &key, scrambled, ptr_to_obj, 4);
}

void *nvmeib_hash_search_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key) {
	const union nvmeib_hash_key	key = hash_uint32_t_to_nvmeib_hash_key(uint32_t_key);
	const uint32_t scrambled = murmur3_32( (uint8_t *)(&uint32_t_key), 4, 0);	// Don't use murmur_32_scramble() - too many collisions;
	return hash_search(hash_tbl, &key, scrambled, 4);
}

void *nvmeib_hash_delete_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key) {
	const union nvmeib_hash_key	key = hash_uint32_t_to_nvmeib_hash_key(uint32_t_key);
	const uint32_t scrambled = murmur3_32( (uint8_t *)(&uint32_t_key), 4, 0);	// Don't use murmur_32_scramble() - too many collisions;
	return hash_delete_key(hash_tbl, &key, scrambled, 4);
}

void *nvmeib_hash_add_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key, void *ptr_to_obj) {
	const union nvmeib_hash_key	key = hash_uint64_t_to_nvmeib_hash_key(uint64_t_key);
	const uint32_t scrambled = murmur3_32( (uint8_t *)(&uint64_t_key), 8, 0);
	return _hash_add(hash_tbl, &key, scrambled, ptr_to_obj, 8);
}

void *nvmeib_hash_search_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key) {
	const union nvmeib_hash_key	key = hash_uint64_t_to_nvmeib_hash_key(uint64_t_key);
	const uint32_t scrambled = murmur3_32( (uint8_t *)(&uint64_t_key), 8, 0);
	return hash_search(hash_tbl, &key, scrambled, 8);
}

void *nvmeib_hash_delete_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key) {
	const union nvmeib_hash_key	key = hash_uint64_t_to_nvmeib_hash_key(uint64_t_key);
	const uint32_t scrambled = murmur3_32( (uint8_t *)(&uint64_t_key), 8, 0);
	return hash_delete_key(hash_tbl, &key, scrambled, 8);
}

void *nvmeib_hash_add_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key, void *ptr_to_obj) {	// Note that ascii_str_key should point to a string inside ptr_to_obj!
	const size_t len = strlen(ascii_str_key);
	const uint32_t scrambled = murmur3_32((const uint8_t *)ascii_str_key, len, 0);
	const union nvmeib_hash_key	key = {.ascii_key.str = ascii_str_key, .ascii_key.len = len};
	return _hash_add(hash_tbl, &key, scrambled, ptr_to_obj, -1);
}

void *nvmeib_hash_search_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key) {
	const size_t len = strlen(ascii_str_key);
	const uint32_t scrambled = murmur3_32((const uint8_t *)ascii_str_key, len, 0);
	const union nvmeib_hash_key	key = {.ascii_key.str = ascii_str_key, .ascii_key.len = len};
	return hash_search(hash_tbl, &key, scrambled, -1);
}

void *nvmeib_hash_delete_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key) {
	const size_t len = strlen(ascii_str_key);
	const uint32_t scrambled = murmur3_32((const uint8_t *)ascii_str_key, len, 0);
	const union nvmeib_hash_key	key = {.ascii_key.str = ascii_str_key, .ascii_key.len = len};
	return hash_delete_key(hash_tbl, &key, scrambled, -1);
}

/*****************************************************************************/
static inline struct nvmeib_hash_table *__nvmeib_hash_create(int log2_of_n_arr_entries, const char *description, int8_t key_len, bool is_used_outside_main_thread)
{
	struct nvmeib_hash_table *hash_tbl = calloc(1, sizeof(*hash_tbl));
	strncpy(hash_tbl->description, description, sizeof(hash_tbl->description) - 1);
	hash_tbl->key_len = key_len;
	hash_tbl->is_used_outside_main_thread = is_used_outside_main_thread;
	if (log2_of_n_arr_entries >= 24) {
		N_Ef(ianyr1z, "log2_of_n_arr_entries=@INT", log2_of_n_arr_entries);
		log2_of_n_arr_entries = 16;
	}
	hash_tbl->initial_log2_of_n_arr_entries = log2_of_n_arr_entries;
	hash_init_arr(hash_tbl, log2_of_n_arr_entries);
	N_Tf(cruyjao, "@STR", hash_tbl->description);
	return hash_tbl;
}

static pthread_mutex_t all_active_hashs_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct nvmeib_hash_table *all_active_hashs;	// Registry of all created hashes. Protected only by all_active_hashs_mutex.
struct nvmeib_hash_table *nvmeib_hash_create(int log2_of_n_arr_entries, const char *description, int8_t key_len, bool is_used_outside_main_thread)
{
	struct nvmeib_hash_table		*hash_tbl = __nvmeib_hash_create(log2_of_n_arr_entries, description, key_len, is_used_outside_main_thread);
	pthread_mutex_lock(&all_active_hashs_mutex);
	if (unlikely(!all_active_hashs))
		all_active_hashs = __nvmeib_hash_create(HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "Hash_of_all_active_hashes", 8, true /* Toma main thread/srm-thread create hashes*/);
	nvmeib_hash_add_uint64_t(all_active_hashs, (uint64_t)hash_tbl, hash_tbl);
	pthread_mutex_unlock(&all_active_hashs_mutex);
	return hash_tbl;
}

void nvmeib_hash_tbl_free(struct nvmeib_hash_table *hash_tbl)
{
	if (!hash_tbl)
		return;
	N_Tf(xbuj6qo, "@STR", hash_tbl->description);
	pthread_mutex_lock(&all_active_hashs_mutex);
	nvmeib_hash_delete_uint64_t(all_active_hashs, (uint64_t)hash_tbl);
	pthread_mutex_unlock(&all_active_hashs_mutex);
	free(hash_tbl->arr);
	hash_tbl->arr = NULL;
	hash_tbl->n_occupied = 0;
	hash_tbl->n_arr_entries = 0;
	free(hash_tbl);
}

void nvmeib_hash_resize_all_tables_as_needed(void)
{
	struct timespec					now;
	static struct timespec			last_invocation;
	static int						last_scanned_idx = 0;

	NFIN;
	getnstimeofday_boot(&now);
	pthread_mutex_lock(&all_active_hashs_mutex);
	if ((!all_active_hashs) || (timespec_diff_ns(now, last_invocation) < MSEC_TO_NSEC(2000))) {
		goto unlock_out;
	}
	last_invocation = now;
	for (int i = 0; i < all_active_hashs->n_arr_entries; i++) {
		last_scanned_idx = hash_next_idx_on_collision(last_scanned_idx, all_active_hashs->scrambled_to_idx_mask);	// In range, also if size changed
		if (hash_is_entry_OCCUPIED(&(all_active_hashs->arr[last_scanned_idx]))) {
			struct nvmeib_hash_table *hash_tbl = (struct nvmeib_hash_table *)(all_active_hashs->arr[last_scanned_idx].ptr_to_obj);
			if (!hash_tbl->is_used_outside_main_thread) 	// Assumption: This function is called periodically from Toma main thread. Cannot resize other hashes
				nvmeib_hash_resize(hash_tbl);
		}
		getnstimeofday_boot(&now);
		if (timespec_diff_ns(now, last_invocation) > MSEC_TO_NSEC(10))
			break;											// Spent too much time here. Other threads can be stuck now waiting to create a new hash table
	}
unlock_out:
	pthread_mutex_unlock(&all_active_hashs_mutex);
	NFOUT;
}

void nvmeib_hash_dump_tbl(struct nvmeib_hash_table *hash_tbl)
{
	int							i;
	struct nvmeib_hash_entry	e_zero = {0};

	NVMEIB_HASH_DUMP_STATISTICS(2h8sjrw, hash_tbl);
	for (i = 0; i < hash_tbl->n_arr_entries; i++) {
		const struct nvmeib_hash_entry e = hash_tbl->arr[i];
		if (!memcmp(&e, &e_zero, sizeof(e)))
			continue;
		if (hash_tbl->key_len == -1) {
			N_Tf(7sboifn, "		@INT: @X  @PTR  @INT @STR", i, e.scrambled, e.ptr_to_obj, (int)e.key.ascii_key.len, (e.key.ascii_key.str ? e.key.ascii_key.str : "????"));
		} else {
			N_Tf(wmpu6n1, "		@INT: @X  @PTR  @INT64_TX @INT64_TX", i, e.scrambled, e.ptr_to_obj, e.key.ll[0], e.key.ll[1]);
		}
	}
}

void nvmeib_hash_free_all_tables(void)
{
	pthread_mutex_lock(&all_active_hashs_mutex);
	if (all_active_hashs) {	// Here is a strong assumption that all hashes destroy are called once on toma main thread when no new hashes can be created
		struct nvmeib_hash_table *hash_tbl = NULL;
		NVMEIB_HASH_FOREACH(hash_tbl, all_active_hashs) {
			N_Tf(xbuj6qr, "@STR", hash_tbl->description);
			free(hash_tbl->arr);
			free(hash_tbl);
		}
		N_Tf(xbuj6qp, "@STR", all_active_hashs->description);
		free(all_active_hashs->arr);
		free(all_active_hashs);
		all_active_hashs = NULL;
	}
	pthread_mutex_unlock(&all_active_hashs_mutex);
}
