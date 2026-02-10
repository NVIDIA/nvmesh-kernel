#include <stdio.h>
#include <stdlib.h>
#include "compat/kr_incs_time.h"

#if defined IS_HASH_UNITTEST
#define IS_EXTERNAL_UNITTEST 1
#else	// #if defined IS_HASH_UNITTEST
#define IS_HASH_UNITTEST 0
#define IS_EXTERNAL_UNITTEST 0
#endif	// #if defined IS_HASH_UNITTEST

/******************************************************************************/

#if IS_HASH_UNITTEST
#define NVMEIB_HASH_ASSERT(name, cond, fmt, ...) ({			\
	if (!(cond)) {											\
		fprintf(stderr, #name "" #cond "\n");				\
	}														\
})
#include "../toma/nvmeibt_ds.h"
#include "compat/kr_incs_dummy_empty_traces.h"
#else	// #if IS_HASH_UNITTEST
void nvmeibt_abort(enum nvmeibt_error_severity es);
#define NVMEIB_HASH_ASSERT(name, cond, fmt, ...) ({			\
	if (!(cond)) {											\
		N_Ef(name, fmt, ## __VA_ARGS__);					\
		nvmeibt_abort(ES_FATAL);							\
	}														\
})
#endif	// #if IS_HASH_UNITTEST

#include "nvmeib_hash.h"

#if IS_HASH_UNITTEST
#undef HASH_MIN_LOG2_OF_N_ARR_ENTRIES
#define HASH_MIN_LOG2_OF_N_ARR_ENTRIES 3
#endif	// #if IS_HASH_UNITTEST
//
#define HASH_MIN_N_ARR_ENTRIES		(0x1 << HASH_MIN_LOG2_OF_N_ARR_ENTRIES)
#define HASH_EMERGENCY_LOAD_FACTOR_THRESHOLD		60 / 100
#define HASH_RELAXED_LOAD_FACTOR_THRESHOLD			51 / 100
#define HASH_SHRINK_FACTOR_THRESHOLD				10

static inline uint32_t hash_scrambled_to_idx(const uint32_t scrambled, const uint32_t scrambled_to_idx_mask)
{
	return (scrambled & scrambled_to_idx_mask);
}

static inline uint32_t hash_scramble_uuid (const union nvmeib_hash_key *uuid)
{
	return murmur3_32(&(uuid->c[0]), sizeof(*uuid), 0);
//	return (uuid->i[0] ^ uuid->i[1] ^ uuid->i[2] ^ uuid->i[3]);	// Generated huge chains for deletions with no empty in the synthetic (serial) test
}

static inline uint32_t hash_next_idx_on_collision(const uint32_t idx, const uint32_t scrambled_to_idx_mask)
{
	return (idx + 1) & scrambled_to_idx_mask;
}

static inline bool hash_is_same_key_OCCUPIED(const union nvmeib_hash_key *key1, const union nvmeib_hash_key *key2, int8_t key_len)
{
	bool		rv;

	if (key_len == -1) {    // is ascii
		rv = (key1->ascii_key.len == key2->ascii_key.len) && (strncmp(key1->ascii_key.str, key2->ascii_key.str, key1->ascii_key.len) == 0);
	} else {
		rv = (memcmp(key1, key2, sizeof(*key1)) == 0);
	}
	return rv;
}

static inline union nvmeib_hash_key hash_uint32_t_to_nvmeib_hash_key(const uint32_t uint32_t_key)
{
	union nvmeib_hash_key	key = {.i = {uint32_t_key, 0, 0, uint32_t_key}};
	return key;
}

static inline union nvmeib_hash_key hash_uint64_t_to_nvmeib_hash_key(const uint64_t uint64_t_key)
{
	union nvmeib_hash_key	key = {.ll = {uint64_t_key, uint64_t_key}};
	return key;
}

static inline union nvmeib_hash_key *hash_uuid_to_nvmeib_hash_key(const union nvmeib_uuid *uuid)
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
	hash_tbl->arr = calloc(hash_tbl->n_arr_entries, sizeof(hash_tbl->arr[0]));
}

static void nvmeib_hash_resize(struct nvmeib_hash_table *hash_tbl)
{
	struct nvmeib_hash_entry	*old_arr;
	int							old_n_arr_entries;
	int							i;
	int							idx;
	struct timespec				start_timespec;
	struct timespec				end_timespec;

	// Might be growing or shrinking
#if IS_HASH_UNITTEST
	fprintf(stdout, "nvmeib_hash_resize_1 n_occupied=%d n_arr_entries=%d Threshold(relaxed)=%d\n", hash_tbl->n_occupied, hash_tbl->n_arr_entries, hash_tbl->n_arr_entries * HASH_RELAXED_LOAD_FACTOR_THRESHOLD);
#endif	// #if IS_HASH_UNITTEST
	if (hash_tbl->n_occupied >= hash_tbl->n_arr_entries * HASH_RELAXED_LOAD_FACTOR_THRESHOLD) {
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
#if IS_HASH_UNITTEST
	fprintf(stdout, "nvmeib_hash_resize_2 %s %d->%d\n", hash_tbl->description, old_n_arr_entries, hash_tbl->n_arr_entries);
#else	// #if IS_HASH_UNITTEST
	N_Tf(crgauy4, "@STR @INT->@INT", hash_tbl->description, old_n_arr_entries, hash_tbl->n_arr_entries);
#endif	// #if IS_HASH_UNITTEST
	// Copy the arr
#if 0	// Not needed since the ht->arr[i].ptr_to_obj is already 0==HASH_ENTRY_EMPTY
	for(int i = 0; i < new_n_arr_entries; i++)
		hash_tbl->arr[i].ptr_to_obj = EMPTY;
#endif	// #if 0	// Not needed since the PTR is already 0==HASH_ENTRY_EMPTY

	for (i = 0; i < old_n_arr_entries; i++) {
		if (hash_is_entry_OCCUPIED(&old_arr[i])) {
			int		n_collisions = 0;
			// Re-add existing keys
			idx = hash_scrambled_to_idx(old_arr[i].scrambled, hash_tbl->scrambled_to_idx_mask);
			while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
				if (n_collisions++ > hash_tbl->n_arr_entries) {
#if IS_HASH_UNITTEST
					fprintf(stderr, "%s n_collisions=%d n_arr_entries=%d", hash_tbl->description, n_collisions, hash_tbl->n_arr_entries);
#else	// #if IS_HASH_UNITTEST
					N_Ef(cr78shj, "@STR n_collisions=@INT n_arr_entries=@INT", hash_tbl->description, n_collisions, hash_tbl->n_arr_entries);
#endif	// #if IS_HASH_UNITTEST
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
#if IS_HASH_UNITTEST
	fprintf(stdout, "nvmeib_hash_resize() took %ldns\n", timespec_diff_ns(end_timespec, start_timespec));
#endif	// #if IS_HASH_UNITTEST
out:;
}

static void *hash_add(struct nvmeib_hash_table *hash_tbl, const union nvmeib_hash_key *key, const uint32_t scrambled, void *in_ptr_to_obj)
{
	bool		is_found = 0;
	int			idx;
	int			n_collisions = 0;
	void		*rv_ptr_to_obj = NULL;

#if IS_HASH_UNITTEST
	fprintf(stdout, "hash_add n_occupied=%d n_arr_entries=%d Threshold(emergency)=%d\n", hash_tbl->n_occupied, hash_tbl->n_arr_entries, hash_tbl->n_arr_entries * HASH_EMERGENCY_LOAD_FACTOR_THRESHOLD);
#endif	// #if IS_HASH_UNITTEST
	if (hash_tbl->n_occupied >= hash_tbl->n_arr_entries * HASH_EMERGENCY_LOAD_FACTOR_THRESHOLD) {
		nvmeib_hash_resize(hash_tbl);
	}
	idx = hash_scrambled_to_idx(scrambled, hash_tbl->scrambled_to_idx_mask);
	while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
		if (n_collisions++ > hash_tbl->n_arr_entries) {
#if IS_HASH_UNITTEST
			fprintf(stdout, "hash_add_3 OOPS n_collisions=%d\n", n_collisions);
#else	// #if IS_HASH_UNITTEST
			N_Ef(vg5h8ak, "OOPS n_collisions=@INT", n_collisions);
#endif	// #if IS_HASH_UNITTEST
			goto out;
		}
		is_found = hash_is_same_key_OCCUPIED(&(hash_tbl->arr[idx].key), key, hash_tbl->key_len);
		if (is_found) {
			rv_ptr_to_obj = hash_tbl->arr[idx].ptr_to_obj;
#if IS_HASH_UNITTEST
			fprintf(stdout, "add found: key=(%lx,%lx)%p hash_tbl->arr[%d].key=(%lx,%lx)%p\n", key->ll[1], key->ll[0], in_ptr_to_obj, idx, hash_tbl->arr[idx].key.ll[1], hash_tbl->arr[idx].key.ll[0], hash_tbl->arr[idx].ptr_to_obj);
#else	// #if IS_HASH_UNITTEST
#endif	// #if IS_HASH_UNITTEST
			goto out;
		}
		idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
	}
#if IS_HASH_UNITTEST
	fprintf(stdout, "hash_add_summary n_collisions=%d\n", n_collisions);
#endif	// #if IS_HASH_UNITTEST
	// EMPTY
	hash_tbl->n_occupied++;
	hash_tbl->arr[idx].key = *key;
	hash_tbl->arr[idx].ptr_to_obj = in_ptr_to_obj;
	hash_tbl->arr[idx].scrambled = scrambled;
	rv_ptr_to_obj = NULL;
out:
	NVMEIB_HASH_DUMP_STATISTICS(4cghs89, hash_tbl);
	return rv_ptr_to_obj;
}

void *nvmeib_hash_add_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid, void *ptr_to_obj)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(vaj3nq2, hash_tbl->key_len == 16, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = *hash_uuid_to_nvmeib_hash_key(uuid);
	scrambled = hash_scramble_uuid(&key);
	return hash_add(hash_tbl, &key, scrambled, ptr_to_obj);
}

void *nvmeib_hash_add_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key, void *ptr_to_obj)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(afj3i1k, hash_tbl->key_len == 4, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = hash_uint32_t_to_nvmeib_hash_key(uint32_t_key);
	scrambled = murmur3_32( (uint8_t *)(&uint32_t_key), 4, 0);	// Don't use murmur_32_scramble() - too many collisions
	return hash_add(hash_tbl, &key, scrambled, ptr_to_obj);
}

void *nvmeib_hash_add_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key, void *ptr_to_obj)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(9sjk2m4, hash_tbl->key_len == 8, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = hash_uint64_t_to_nvmeib_hash_key(uint64_t_key);
	scrambled = murmur3_32( (uint8_t *)(&uint64_t_key), 8, 0);
	return hash_add(hash_tbl, &key, scrambled, ptr_to_obj);
}

void *nvmeib_hash_add_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key, void *ptr_to_obj)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;
	size_t					len;

	NVMEIB_HASH_ASSERT(6dgya81, hash_tbl->key_len == -1, "@STR !(hash_tbl->is_ascii)", hash_tbl->description);
	len = strlen(ascii_str_key);
	scrambled = murmur3_32((uint8_t *)ascii_str_key, len, 0);
	key.ascii_key.str = (char *)ascii_str_key;
	key.ascii_key.len = len;
	return hash_add(hash_tbl, &key, scrambled, ptr_to_obj);
}

static void *hash_search(const struct nvmeib_hash_table *hash_tbl, const union nvmeib_hash_key *key, const uint32_t scrambled)
{
	int			idx;
	void		*ptr_to_obj = NULL;
	int			n_collisions = 0;

	idx = hash_scrambled_to_idx(scrambled, hash_tbl->scrambled_to_idx_mask);
	while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
		if (hash_is_same_key_OCCUPIED(&(hash_tbl->arr[idx].key), key, hash_tbl->key_len)) {
			ptr_to_obj = hash_tbl->arr[idx].ptr_to_obj;
			break;
		}
		if (n_collisions++ >= hash_tbl->n_arr_entries) {
			break;	// Not found
		}
#if IS_HASH_UNITTEST
		fprintf(stdout, "hash_search_3 OOPS n_collisions=%d\n", n_collisions);
#endif	// #if IS_HASH_UNITTEST
		idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
	}
	return ptr_to_obj;
}

void *nvmeib_hash_search_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(7sjk2ne, hash_tbl->key_len == 16, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = *hash_uuid_to_nvmeib_hash_key(uuid_key);
	scrambled = hash_scramble_uuid(&key);
	return hash_search(hash_tbl, &key, scrambled);
}

void *nvmeib_hash_search_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(havbgro, hash_tbl->key_len == 4, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = hash_uint32_t_to_nvmeib_hash_key(uint32_t_key);
	scrambled = murmur3_32( (uint8_t *)(&uint32_t_key), 4, 0);	// Don't use murmur_32_scramble() - too many collisions
	return hash_search(hash_tbl, &key, scrambled);
}

void *nvmeib_hash_search_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(hudi9d0, hash_tbl->key_len == 8, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = hash_uint64_t_to_nvmeib_hash_key(uint64_t_key);
	scrambled = murmur3_32( (uint8_t *)(&uint64_t_key), 8, 0);
	return hash_search(hash_tbl, &key, scrambled);
}

void *nvmeib_hash_search_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;
	size_t					len;

	NVMEIB_HASH_ASSERT(4vsy790, hash_tbl->key_len == -1, "@STR !(hash_tbl->is_ascii)", hash_tbl->description);
	len = strlen(ascii_str_key);
	scrambled = murmur3_32((uint8_t *)ascii_str_key, len, 0);
	key.ascii_key.str = (char *)ascii_str_key;
	key.ascii_key.len = len;
	return hash_search(hash_tbl, &key, scrambled);
}

static void *hash_delete_key(struct nvmeib_hash_table *hash_tbl, const union nvmeib_hash_key *key, const uint32_t scrambled) {
	int			n_collisions = 0;
	int			idx, prev_deleted_idx, idx_according_to_scrambled;
	int			gap_from_idx_according_to_scrambled_to_prev_deleted;
	bool		is_scrambled_beyond_deleted;
	void		*deleted_ptr_to_obj = NULL;

	idx = hash_scrambled_to_idx(scrambled, hash_tbl->scrambled_to_idx_mask);
#if IS_HASH_UNITTEST
	fprintf(stdout, "hash_delete_key_1 idx=%d n_occupied=%d\n", idx, hash_tbl->n_occupied);
#endif	// #if IS_HASH_UNITTEST
	while (hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
		if (hash_is_same_key_OCCUPIED(&(hash_tbl->arr[idx].key), key, hash_tbl->key_len)) {
			deleted_ptr_to_obj = hash_tbl->arr[idx].ptr_to_obj;
			// Now, push the following entries backwards (stop at empry or properly located)
			prev_deleted_idx = idx;
			do {
				idx = hash_next_idx_on_collision(idx, hash_tbl->scrambled_to_idx_mask);
				idx_according_to_scrambled = (hash_tbl->arr[idx].scrambled & hash_tbl->scrambled_to_idx_mask);
#if IS_HASH_UNITTEST
				fprintf(stdout, "Delete iteration ------ prev_deleted_idx=%d idx=%d idx_according_to_scrambled=%d\n", prev_deleted_idx, idx, idx_according_to_scrambled);
#endif	// #if IS_HASH_UNITTEST
				if (!hash_is_entry_OCCUPIED(&(hash_tbl->arr[idx]))) {
					break;	// End of chain
				}
				if (idx == idx_according_to_scrambled) {
#if IS_HASH_UNITTEST
					fprintf(stdout, "Delete iteration ------ idx == idx_according_to_scrambled\n");
#endif	// #if IS_HASH_UNITTEST
					continue;	// Located properly
				}
				gap_from_idx_according_to_scrambled_to_prev_deleted = ((idx_according_to_scrambled - prev_deleted_idx) & hash_tbl->scrambled_to_idx_mask);
				is_scrambled_beyond_deleted = (gap_from_idx_according_to_scrambled_to_prev_deleted > 0 && gap_from_idx_according_to_scrambled_to_prev_deleted <= hash_tbl->n_occupied);
#if IS_HASH_UNITTEST
fprintf(stdout, "gap_from_idx_according_to_scrambled_to_prev_deleted=%d    n_occupied=%d\n", gap_from_idx_according_to_scrambled_to_prev_deleted, hash_tbl->n_occupied);
#endif	// #if IS_HASH_UNITTEST
				if (is_scrambled_beyond_deleted) {   // if (idx_according_to_scrambled > prev_deleted_idx) // with wraparound
#if IS_HASH_UNITTEST
					fprintf(stdout, "Delete iteration ------ >>>>>>\n");
#endif	// #if IS_HASH_UNITTEST
					continue;	// The free prev_deleted_entry is before our chain
				}
				// Can be pushed backwards (over the deleted/moved entry)
#if IS_HASH_UNITTEST
				fprintf(stdout, "Delete-overide: prev_deleted_idx=%d idx=%d idx_according_to_scrambled=%d\n", prev_deleted_idx, idx, idx_according_to_scrambled);
#endif	// #if IS_HASH_UNITTEST
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
#if IS_HASH_UNITTEST
	fprintf(stdout, "hash_delete_key_END n_occupied=%d\n", hash_tbl->n_occupied);
#endif	// #if IS_HASH_UNITTEST
	return deleted_ptr_to_obj;
}

void *nvmeib_hash_delete_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(alxon6b, hash_tbl->key_len == 16, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = *hash_uuid_to_nvmeib_hash_key(uuid_key);
	scrambled = hash_scramble_uuid(&key);
	return hash_delete_key(hash_tbl, &key, scrambled);
}

void *nvmeib_hash_delete_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(5bjs90l, hash_tbl->key_len == 4, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = hash_uint32_t_to_nvmeib_hash_key(uint32_t_key);
	scrambled = murmur3_32( (uint8_t *)(&uint32_t_key), 4, 0);	// Don't use murmur_32_scramble() - too many collisions
	return hash_delete_key(hash_tbl, &key, scrambled);
}

void *nvmeib_hash_delete_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;

	NVMEIB_HASH_ASSERT(ndke9sk, hash_tbl->key_len == 8, "@STR hash_tbl->key_len=@INT", hash_tbl->description, hash_tbl->key_len);
	key = hash_uint64_t_to_nvmeib_hash_key(uint64_t_key);
	scrambled = murmur3_32( (uint8_t *)(&uint64_t_key), 8, 0);
	return hash_delete_key(hash_tbl, &key, scrambled);
}

void *nvmeib_hash_delete_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key)
{
	uint32_t				scrambled;
	union nvmeib_hash_key	key;
	size_t					len;

	NVMEIB_HASH_ASSERT(cga0l3p, hash_tbl->key_len == -1, "@STR !(hash_tbl->is_ascii)", hash_tbl->description);
	len = strlen(ascii_str_key);
	scrambled = murmur3_32((uint8_t *)ascii_str_key, len, 0);
	key.ascii_key.str = (char *)ascii_str_key;
	key.ascii_key.len = len;
	return hash_delete_key(hash_tbl, &key, scrambled);
}

static struct nvmeib_hash_table *all_active_hashs;	// Use a hash for the list of all created/active hashes

static inline struct nvmeib_hash_table *__nvmeib_hash_create(int log2_of_n_arr_entries, const char *description, int8_t key_len)
{
	struct nvmeib_hash_table		*hash_tbl = NULL;

	hash_tbl = calloc(1, sizeof(*hash_tbl));
	strncpy(hash_tbl->description, description, sizeof(hash_tbl->description) - 1);
	hash_tbl->key_len = key_len;
	if (log2_of_n_arr_entries >= 24) {
#if IS_HASH_UNITTEST
		fprintf(stdout, "__nvmeib_hash_create log2_of_n_arr_entries=%d\n", log2_of_n_arr_entries);
#else	// #if IS_HASH_UNITTEST
		N_Ef(ianyr1z, "log2_of_n_arr_entries=@INT", log2_of_n_arr_entries);
#endif	// #if IS_HASH_UNITTEST
		log2_of_n_arr_entries = 16;
	}
	hash_tbl->initial_log2_of_n_arr_entries = log2_of_n_arr_entries;
	hash_init_arr(hash_tbl, log2_of_n_arr_entries);
#if 0	// Not needed since the ht->arr[i].ptr_to_obj is already 0==HASH_ENTRY_EMPTY
	for(int i = 0; i < n_arr_entries; i++)
		ht->arr[i].ptr_to_obj = HASH_ENTRY_EMPTY;
#endif	// #if 0	// Not needed since the PTR is already 0==HASH_ENTRY_EMPTY
#if IS_HASH_UNITTEST
	fprintf(stdout, "__nvmeib_hash_create %s\n", hash_tbl->description);
#else	// #if IS_HASH_UNITTEST
	N_Tf(cruyjao, "@STR", hash_tbl->description);
#endif	// #if IS_HASH_UNITTEST
	return hash_tbl;
}

struct nvmeib_hash_table *nvmeib_hash_create(int log2_of_n_arr_entries, const char *description, int8_t key_len)
{
	struct nvmeib_hash_table		*hash_tbl = NULL;

	hash_tbl = __nvmeib_hash_create(log2_of_n_arr_entries, description, key_len);
	if (!all_active_hashs) {
		all_active_hashs = __nvmeib_hash_create(HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "Hash_of_all_active_hashes", 8);
	}
	nvmeib_hash_add_uint64_t(all_active_hashs, (uint64_t)hash_tbl, hash_tbl);
	return hash_tbl;
}

void nvmeib_hash_tbl_free(struct nvmeib_hash_table *hash_tbl)
{
	if (!hash_tbl) {
		goto out;
	}
#if IS_HASH_UNITTEST
	fprintf(stdout, "nvmeib_hash_tbl_free_1 %s\n", hash_tbl->description);
#else	// #if IS_HASH_UNITTEST
	N_Tf(xbuj6qo, "@STR", hash_tbl->description);
#endif	// #if IS_HASH_UNITTEST
	nvmeib_hash_delete_uint64_t(all_active_hashs, (uint64_t)hash_tbl);
	free(hash_tbl->arr);
	hash_tbl->arr = NULL;
	hash_tbl->n_occupied = 0;
	hash_tbl->n_arr_entries = 0;
	free(hash_tbl);
out:;
}

void nvmeib_hash_resize_all_tables_as_needed(void)
{
	struct timespec					now;
	static struct timespec			last_invocation;
	static int						last_scanned_idx = 0;

	NFIN;
	if (!all_active_hashs) {
		goto out;
	}
	getnstimeofday_boot(&now);
	if (timespec_diff_ns(now, last_invocation) < MSEC_TO_NSEC(2000)) {
		goto out;
	}
	last_invocation = now;
	for (int i = 0; i < all_active_hashs->n_arr_entries; i++) {
		last_scanned_idx = hash_next_idx_on_collision(last_scanned_idx, all_active_hashs->scrambled_to_idx_mask);	// In range, also if size changed
		if (hash_is_entry_OCCUPIED(&(all_active_hashs->arr[last_scanned_idx]))) {
			nvmeib_hash_resize((struct nvmeib_hash_table *)(all_active_hashs->arr[last_scanned_idx].ptr_to_obj));
		}
		getnstimeofday_boot(&now);
		if (timespec_diff_ns(now, last_invocation) > MSEC_TO_NSEC(10)) {
			goto out;
		}
	}
out:;
	NFOUT;
}

void nvmeib_hash_dump_tbl(struct nvmeib_hash_table *hash_tbl)
{
	struct nvmeib_hash_entry	e;
	int							i;
	struct nvmeib_hash_entry	e_zero = {0};

	NVMEIB_HASH_DUMP_STATISTICS(2h8sjrw, hash_tbl);
	for (i = 0; i < hash_tbl->n_arr_entries; i++) {
		e = (hash_tbl->arr[i]);
		if (memcmp(&e, &e_zero, sizeof(e))) {
			continue;
		}
		if (hash_tbl->key_len == -1) {
#if IS_HASH_UNITTEST
			fprintf(stdout, "		%d: %x  %p  %d %s", i, e.scrambled, e.ptr_to_obj, (int)e.key.ascii_key.len, (e.key.ascii_key.str ? e.key.ascii_key.str : "????"));
#else	// #if IS_HASH_UNITTEST
			N_Tf(7sboifn, "		@INT: @X  @PTR  @INT @STR", i, e.scrambled, e.ptr_to_obj, (int)e.key.ascii_key.len, (e.key.ascii_key.str ? e.key.ascii_key.str : "????"));
#endif	// #if IS_HASH_UNITTEST
		} else {
#if IS_HASH_UNITTEST
			fprintf(stdout, "		%d: %x  %p  %lx %lx", i, e.scrambled, e.ptr_to_obj, e.key.ll[0], e.key.ll[1]);
#else	// #if IS_HASH_UNITTEST
			N_Tf(wmpu6n1, "		@INT: @X  @PTR  @INT64_TX @INT64_TX", i, e.scrambled, e.ptr_to_obj, e.key.ll[0], e.key.ll[1]);
#endif	// #if IS_HASH_UNITTEST
		}
	}
}

void nvmeib_hash_free_all_tables(void)
{
	struct nvmeib_hash_table		*hash_tbl = NULL;

	if (!all_active_hashs) {
		goto out;
	}
	NVMEIB_HASH_FOREACH(hash_tbl, all_active_hashs) {
		nvmeib_hash_tbl_free(hash_tbl);
	}
	nvmeib_hash_tbl_free(all_active_hashs);
out:;
}

#if IS_EXTERNAL_UNITTEST
#else	// #if IS_EXTERNAL_UNITTEST
#if IS_HASH_UNITTEST
int main() {
	struct nvmeib_hash_table	*ht1, *ht2, *ht3, *ht4;
	void						*ptr_to_obj;
	void						*v;


	fprintf(stdout, "\n520 entries - u32- Test collisions and scale\n");
	ht1 = NVMEIB_HASH_CREATE(vsgvdhgwe, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "TEST_HASH_uint32", 4);

	fprintf(stdout, "\n\n523 entries - u32 - (add) Test collisions and scale\n");
	for (int i = 0; i < 523; i++) {
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uint32_t(ht1, i, ptr_to_obj);
	}

	fprintf(stdout, "\n\nALREADY existing - u32- (add)\n");
	for (int i = 10; i < 20; i++) {
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uint32_t(ht1, i, ptr_to_obj);
	}

	fprintf(stdout, "\n\n523 entries - u32- (search) Test collisions and scale\n");
	for (int i = 0; i < 523; i++) {
		void *v = nvmeib_hash_search_uint32_t(ht1, i);
		fprintf(stdout, "uint32_t_1 Key=%d Value=%p\n", i, v);
	}

	fprintf(stdout, "\n\n25 entries - u32- (del)\n");
	for (int i = 0; i < 25; i++)
		nvmeib_hash_delete_uint32_t(ht1, i);

	fprintf(stdout, "\n\n523 entries - u32- (del)\n");
	for (int i = 0; i < 523; i++)
		nvmeib_hash_delete_uint32_t(ht1, i);

	fprintf(stdout, "\n\n523 entries - u32 - (add) Test collisions and scale\n");
	for (int i = 0; i < 523; i++) {
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uint32_t(ht1, i, ptr_to_obj);
	}

	fprintf(stdout, "\n\n523 entries - u32- (search) Test NVMEIB_HASH_FOREACH deletions\n");
	NVMEIB_HASH_FOREACH (v, ht1) {
		void	*deleted_v;
		deleted_v = nvmeib_hash_delete_uint32_t(ht1, (uint32_t)((uint64_t)v >> 32));
		fprintf(stdout, "v=%llx deleted_v=%p\n", v, deleted_v);
	}

	fprintf(stdout, "\n520 entries - uuid - Test collisions and reuse of deleted\n");
	ht2 = NVMEIB_HASH_CREATE(vsgvdhgwe, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "TEST_HASH_uuid", 16);

	fprintf(stdout, "\n\n523 entries - uuid - (add) Test collisions and scale\n");
	for (int i = 0; i < 523; i++) {
		union nvmeib_uuid	key = {.ints = {i, i, i, i}};
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uuid(ht2, &key, ptr_to_obj);
	}

	fprintf(stdout, "\n\nALREADY existing - uuid- (add)\n");
	for (int i = 40; i < 50; i++) {
		union nvmeib_uuid	key = {.ints = {i, i, i, i}};
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uuid(ht2, &key, ptr_to_obj);
	}

	fprintf(stdout, "\n\n25 entries - uuid - (del)\n");
	for (int i = 0; i < 25; i++) {
		union nvmeib_uuid	key = {.ints = {i, i, i, i}};
		nvmeib_hash_delete_uuid(ht2, &key);
	}

	fprintf(stdout, "\n\n20 entries - uuid - (add)\n");
	for (int i = 10; i < 20; i++) {
		union nvmeib_uuid	key = {.ints = {i, i, i, i}};
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uuid(ht2, &key, ptr_to_obj);
	}

	fprintf(stdout, "\n\n50 entries - uuid - (search)\n");
	for (int i = 0; i < 50; i++) {
		union nvmeib_uuid	key = {.ints = {i, i, i, i}};
		void *v = nvmeib_hash_search_uuid(ht2, &key);
		fprintf(stdout, "search uuid Key=%d Value=%p\n", i, v);
	}

	//
	fprintf(stdout, "\n\n520 entries - u64 - Test collisions\n");
	ht3 = NVMEIB_HASH_CREATE(vsgvdhgwe, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "TEST_HASH_int64", 8);
	for (int64_t i = 0; i < 523; i++) {
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uint64_t(ht3, i, ptr_to_obj);
	}

	for (int64_t i = 70; i < 80; i++) {
		ptr_to_obj = (void *)((uint64_t)i << 32);
		nvmeib_hash_add_uint64_t(ht3, i, ptr_to_obj);
	}

	//
	fprintf(stdout, "\ntest ASCII deletions\n");
	ht4 = NVMEIB_HASH_CREATE(vsgvdhgwe, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "TEST_HASH_ascii", -1);

	fprintf(stdout, "\n\n20 entries - ASCII - (add)\n");
	for (int i = 0; i < 20; i++) {
		char 	*s = malloc(100);
		snprintf(s, 99, "ascii_2_temp_string_unused_%d", i);
		nvmeib_hash_add_ascii_str(ht4, s, s);
	}

	fprintf(stdout, "\n\nALREADY existing - ASCII - (add)\n");
	for (int i = 5; i < 15; i++) {
		char 	*s = malloc(100);
		snprintf(s, 99, "ascii_2_temp_string_unused_%d", i);
		nvmeib_hash_add_ascii_str(ht4, s, s);
	}

	fprintf(stdout, "\n\n20 entries - ASCII - (search)\n");
	for (int i = 0; i < 20; i++) {
		char 	*s = malloc(100);
		snprintf(s, 99, "ascii_2_temp_string_unused_%d", i);
		v = nvmeib_hash_search_ascii_str(ht4, s);
		fprintf(stdout, "ascii_3 Key=%s Value=%p\n", s, v);
	}

	fprintf(stdout, "\n\n15 entries - ASCII - (delete)\n");
	for (int i = 5; i < 15; i++) {
		char 	*s = malloc(100);
		snprintf(s, 99, "ascii_2_temp_string_unused_%d", i);
		nvmeib_hash_delete_ascii_str(ht4, s);
	}

	fprintf(stdout, "\ntest ASCII FOREACH\n");
	NVMEIB_HASH_FOREACH(v, ht4) {
		fprintf(stdout, "NVMEIB_HASH_FOREACH %p\n", v);
	}

	nvmeib_hash_resize_all_tables_as_needed();
	NVMEIB_HASH_TBL_FREE(rbhjkx8, ht1);
	NVMEIB_HASH_TBL_FREE(5s9k30k, ht2);
	NVMEIB_HASH_TBL_FREE(vnx9kql, ht3);
	NVMEIB_HASH_TBL_FREE(3v80spq, ht4);

	return 0;
}
#endif	// #if IS_HASH_UNITTEST
#endif	// #if IS_EXTERNAL_UNITTEST

