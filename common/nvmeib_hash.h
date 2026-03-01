#ifndef NVMEIB_HASH_H
#define NVMEIB_HASH_H

#include "../common_public/nvmeib_uuid_be.h"

#define HASH_ENTRY_EMPTY		(void *)0LL
#define HASH_MIN_LOG2_OF_N_ARR_ENTRIES 5

union nvmeib_hash_key {
	uint8_t		c[16];
	int64_t		ll[2];
	int32_t		i[4];
	struct {
		char	*str;
		size_t	len;
	} ascii_key;
};

struct nvmeib_hash_entry {
	union nvmeib_hash_key			key;			// 16
	void							*ptr_to_obj;	// 24
	uint32_t						scrambled;		// 28
	int								filler_for_cacheline_alignment;	// 32
};

struct nvmeib_hash_table {		// Note that during resize, we keep the object, and allocate a new arr inside it
	char							description[128];
	uint64_t						scrambled_to_idx_mask;
	struct nvmeib_hash_entry		*arr;
	int								log2_of_n_arr_entries;
	int								initial_log2_of_n_arr_entries;
	int 							n_arr_entries;
	int								n_occupied;
	bool							is_used_outside_main_thread;    // For now, no resize at idle_time_activities. The NVMEIB_HASH_FOREACH is too complicated for an unlock()
	int8_t							key_len;
};

#if IS_HASH_UNITTEST
#define NVMEIB_HASH_DUMP_STATISTICS(_name_hash_dump, __hash_tbl) ({												\
	fprintf(stdout, #_name_hash_dump " %s key_len=%d n_arr_entries=%d n_occupied=%d\n",							\
		 __hash_tbl->description, __hash_tbl->key_len, __hash_tbl->n_arr_entries, __hash_tbl->n_occupied);		\
})
#else	// #if IS_HASH_UNITTEST
#define NVMEIB_HASH_DUMP_STATISTICS(_name_hash_dump, __hash_tbl) ({												\
	N_Tf(_name_hash_dump, "@STR key_len=@INT8_TD n_arr_entries=@INT n_occupied=@INT",							\
		 __hash_tbl->description, __hash_tbl->key_len, __hash_tbl->n_arr_entries, __hash_tbl->n_occupied);		\
})
#endif	// #if IS_HASH_UNITTEST

// NVMEIB_HASH_FOREACH is delicate since entries might be deleted, and this moves entries backwards.
//  It might be that a new entry moved into the deleted "idx", so we progress to the next only if either empty of same ptr_to_obj as before
//  Unfortunately, the for loop does not allow two different types in the first clause
#define NVMEIB_HASH_FOREACH(__x__, __hash_tbl) 																																		\
	if ((__hash_tbl) && (__hash_tbl)->n_occupied)																																	\
		for (	struct nvmeib_hash_entry *__e ## __x__ = &((__hash_tbl)->arr[0]), __OLD_ ## __x__ = *(__e ## __x__);																\
				(__e ## __x__) < ((__hash_tbl)->arr + (__hash_tbl)->n_arr_entries);																									\
				(__e ## __x__) = (!hash_is_entry_OCCUPIED(__e ## __x__) || (__e ## __x__)->ptr_to_obj == (__OLD_ ## __x__).ptr_to_obj ?												\
								  (__e ## __x__) + 1 : (__e ## __x__)),																												\
								  (__OLD_ ## __x__) = ((__e ## __x__) < ((__hash_tbl)->arr + (__hash_tbl)->n_arr_entries) ? (*(__e ## __x__)) : (__OLD_ ## __x__)))					\
			if (hash_is_entry_OCCUPIED(__e ## __x__) && (__x__ = (__e ## __x__)->ptr_to_obj))

static inline bool hash_is_entry_OCCUPIED(const struct nvmeib_hash_entry *entry)
{
	return (entry->ptr_to_obj != HASH_ENTRY_EMPTY);
}

static inline int nvmeib_hash_get_n_elements(struct nvmeib_hash_table *hash_tbl)
{
	return (hash_tbl ? hash_tbl->n_occupied : 0);
}

static inline bool nvmeib_hash_is_ascii(struct nvmeib_hash_table *hash_tbl)
{
	return (hash_tbl->key_len == -1);
}

struct nvmeib_hash_table *nvmeib_hash_create(int log2_of_n_arr_entries, const char *description, int8_t key_len, bool is_used_outside_main_thread);
#define NVMEIB_HASH_CREATE(name_, log2_of_n_arr_entries_, desc_, key_len_, is_used_outside_main_thread_) ({								\
	struct nvmeib_hash_table	*__ht__ = nvmeib_hash_create(log2_of_n_arr_entries_, desc_, key_len_, is_used_outside_main_thread_);	\
	NVMEIB_HASH_DUMP_STATISTICS(name_, __ht__);																							\
	__ht__;																																\
})

void nvmeib_hash_tbl_free(struct nvmeib_hash_table *hash_tbl);
#define NVMEIB_HASH_TBL_FREE(name_, __hash_tbl) ({								\
	if (__hash_tbl) {															\
		NVMEIB_HASH_DUMP_STATISTICS(name_, __hash_tbl);							\
		nvmeib_hash_tbl_free(__hash_tbl);										\
		__hash_tbl = NULL;														\
	}																			\
})

void *nvmeib_hash_add_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid, void *ptr_to_obj);
#define NVMEIB_HASH_ADD_UUID(name_, __ht__, uuid__, ptr_to_obj__) ({			\
	void *__old_obj = nvmeib_hash_add_uuid(__ht__, uuid__, ptr_to_obj__);		\
	NVMEIB_HASH_DUMP_STATISTICS(name_, __ht__);									\
	__old_obj;																	\
})

void *nvmeib_hash_add_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key, void *ptr_to_obj);
void *nvmeib_hash_add_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key, void *ptr_to_obj);
void *nvmeib_hash_add_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key, void *ptr_to_obj);	// Note that ascii_str_key should point to a string inside ptr_to_obj!
void *nvmeib_hash_search_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid_key);
void *nvmeib_hash_search_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key);
void *nvmeib_hash_search_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key);
void *nvmeib_hash_search_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key);
void *nvmeib_hash_delete_uuid(struct nvmeib_hash_table *hash_tbl, const union nvmeib_uuid *uuid_key);
void *nvmeib_hash_delete_uint32_t(struct nvmeib_hash_table *hash_tbl, const uint32_t uint32_t_key);
void *nvmeib_hash_delete_uint64_t(struct nvmeib_hash_table *hash_tbl, const uint64_t uint64_t_key);
void *nvmeib_hash_delete_ascii_str(struct nvmeib_hash_table *hash_tbl, const char *ascii_str_key);
void nvmeib_hash_resize_all_tables_as_needed(void);
void nvmeib_hash_dump_tbl(struct nvmeib_hash_table *hash_tbl);
void nvmeib_hash_free_all_tables(void);

#endif	// #ifndef NVMEIB_HASH_H
