/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_DS
#define NVMEIBT_DS

#include <stdint.h>
#include <execinfo.h>

/**
 * Doubly linked list inspired from some (free) stuff I have
 * seen before and hashtable similar to the one we have in the
 * kernel but adjusted to our needs
 */

#ifdef CONCAT
#	undef CONCAT
#endif
#ifdef CONCAT1
#	undef CONCAT1
#endif

/* need these here to define macro line vars */
#define CONCAT1(x, y) x ## y
#define CONCAT(x, y) CONCAT1(x, y)
/* macro to call compiler builtin functionality */

struct xdlist_with_size;

/* ---------------- doubly linked list -------------------------------------- */
/* doubly linked list */
struct xdlist {
	struct xdlist *next;
	struct xdlist *prev;
	struct xdlist_with_size *parent;
};

/* double linked list + size*/
struct xdlist_with_size {
	struct xdlist list;
	int size;
};

/**
 * declare a doubly linked list head with some type info but
 * with no extra space
 */
#define XDLIST_DECLARE(head_type, type, field) \
	union head_type { \
		struct xdlist_with_size list_with_size; \
		union { \
			char (*offset)[offsetof(type, field)]; \
			type *t; \
		} *data; \
	}

#define XDLIST_INIT(name) {{ {&((name).list_with_size.list), &((name).list_with_size.list), &((name).list_with_size)}, 0}}

#define XDLIST_INIT_LINK(field, parent_list) ({ \
	(field)->next = (field); \
	(field)->prev = (field); \
	(field)->parent = (parent_list); \
})

#define XDLIST_NULL(field) 	((field)->next == (field) && (field)->prev == (field))

#define XDLIST_HEAD_INIT(name) do {												\
	XDLIST_INIT_LINK(&(name)->list_with_size.list, &((name)->list_with_size));	\
	(name)->list_with_size.size = 0;											\
} while (0)

#define XDLIST_OFFSET(head) sizeof(*(head)->data->offset)
#define XDLIST_TYPE(head) typeof((head)->data->t)

#define XDLIST_ELEM(head, list_with_size)\
	((XDLIST_TYPE(head)) ((char *)(list_with_size) - XDLIST_OFFSET(head)))

#define XDLIST_FIELD(head, elem) \
	((struct xdlist *) ((char *)(elem) + XDLIST_OFFSET(head)))

#ifdef __cplusplus
	#define XDLIST_CHECK_TYPES(     head , elem ) false
	#define XDLIST_CHECK_HEAD_TYPES(head1, head2) false
#else
	#define XDLIST_CHECK_TYPES(     head, elem)   BUILD_BUG_IF_ZERO(__builtin_types_compatible_p(XDLIST_TYPE(head), typeof(elem)))
	#define XDLIST_CHECK_HEAD_TYPES(head1, head2) BUILD_BUG_IF_ZERO(__builtin_types_compatible_p(XDLIST_TYPE(head1), XDLIST_TYPE(head2)))
#endif

#define XDLIST_EMPTY(head) ((head)->list_with_size.list.next == &(head)->list_with_size.list)

/* unsafe - please try not to use */
#define XDLIST_FIRST_(head) XDLIST_ELEM((head), (head)->list_with_size.list.next)
#define XDLIST_LAST_(head)  XDLIST_ELEM((head), (head)->list_with_size.list.prev)
#define XDLIST_NEXT_(elem, field) \
	container_of((elem)->field.next, typeof(*(elem)), field)
#define XDLIST_PREV_(elem, field) \
	container_of((elem)->field.prev, typeof(*(elem)), field)

/* the safe version */
#define XDLIST_FIRST(head)	(XDLIST_EMPTY(head) ? NULL : XDLIST_FIRST_(head))
#define XDLIST_LAST(head)	(XDLIST_EMPTY(head) ? NULL : XDLIST_LAST_(head))

#define XDLIST_SAFE(head, list_with_size) \
    ((list_with_size.list) == &(head)->list_with_size.list) ? NULL : XDLIST_ELEM((head), (list_with_size.list))

#define XDLIST_NEXT(head, elem) \
    (XDLIST_CHECK_TYPES(head, elem), \
		XDLIST_SAFE((head), XDLIST_FIELD((head), (elem))->next))
#define XDLIST_PREV(head, elem) \
	(XDLIST_CHECK_TYPES(head, elem), \
		XDLIST_SAFE((head), XDLIST_FIELD((head), (elem))->prev))

/* a general add */
#	define XDLIST_ADD(head, _prev, _next, elem) \
			do { \
				if (XDLIST_CHECK_TYPES(head, elem)); \
				else for (struct xdlist *CONCAT(new_link_, __LINE__), \
						*CONCAT(prev_, __LINE__) = _prev, \
						*CONCAT(next_, __LINE__) = _next; ; ) { \
					CONCAT(new_link_, __LINE__) 		= XDLIST_FIELD((head), (elem)); \
					CONCAT(next_, __LINE__)->prev 		= CONCAT(new_link_, __LINE__); \
					CONCAT(new_link_, __LINE__)->next 	= CONCAT(next_, __LINE__); \
					CONCAT(new_link_, __LINE__)->prev 	= CONCAT(prev_, __LINE__); \
					CONCAT(prev_, __LINE__)->next 		= CONCAT(new_link_, __LINE__); \
					CONCAT(new_link_, __LINE__)->parent	= &((head)->list_with_size); \
					(head)->list_with_size.size++; \
					break; \
				} \
			} while (0)
#	define NXDLIST_ADD_CHECK(name, head, _prev, _next, elem)							\
		do {												\
			if (!XDLIST_NULL(XDLIST_FIELD((head), (elem))))						\
				N_Ef(name ## _error_1, "XDLIST_ADD_CHECK, expected XDLIST_NULL()");				\
			else 											\
				XDLIST_ADD(head, _prev, _next, elem);						\
		} while (0)

/* add to head or tail */
#define XDLIST_ADD_HEAD(head, elem) XDLIST_ADD(head, &(head)->list_with_size.list, (head)->list_with_size.list.next, elem)
#define XDLIST_ADD_TAIL(head, elem) XDLIST_ADD(head, (head)->list_with_size.list.prev, &(head)->list_with_size.list, elem)
#define XDLIST_ADD_HEAD_CHECK(head, elem) XDLIST_ADD_CHECK(head, &(head)->list_with_size.list, (head)->list_with_size.list.next, elem)
#define NXDLIST_ADD_TAIL_CHECK(name, head, elem) NXDLIST_ADD_CHECK(name, head, (head)->list_with_size.list.prev, &(head)->list_with_size.list, elem)
#define _XDLIST_SPLICE(head, _prev, _next, to_head) \
	do { \
		if (XDLIST_CHECK_HEAD_TYPES(head, to_head) || XDLIST_EMPTY(head)); \
		else for (struct xdlist \
				*CONCAT(first_, __LINE__) = (head)->list_with_size.list.next, \
				*CONCAT(last_, __LINE__) =  (head)->list_with_size.list.prev, \
				*CONCAT(prev_, __LINE__) = _prev, \
				*CONCAT(next_, __LINE__) = _next; ; ) { \
				CONCAT(first_, __LINE__)->prev = CONCAT(prev_, __LINE__); \
				CONCAT(prev_, __LINE__)->next = CONCAT(first_, __LINE__); \
				CONCAT(last_, __LINE__)->next = CONCAT(next_, __LINE__); \
				CONCAT(next_, __LINE__)->prev = CONCAT(last_, __LINE__); \
				(to_head)->list_with_size.size += (head)->list_with_size.size; \
				(head)->list_with_size.size = 0; \
				XDLIST_HEAD_INIT(head); \
				break; \
		} \
	} while (0)

/* in splice head is the source and elem is the destination */
#define XDLIST_SPLICE(from_head, to_head) _XDLIST_SPLICE(from_head, &(to_head)->list_with_size.list,  (to_head)->list_with_size.list.next, to_head)

/* delete an element by the link */
#define XDLIST_DEL(field) do {							\
	if (!XDLIST_NULL(field)) {							\
		(field)->next->prev = (field)->prev;			\
		(field)->prev->next = (field)->next;			\
		(field)->parent->size--;						\
		XDLIST_INIT_LINK((field), (field)->parent);		\
	}													\
} while (0)

/* delete an element */
#define XDLIST_ELEM_DEL(head, elem) \
	if (XDLIST_CHECK_TYPES(head, elem)); \
	else XDLIST_DEL(XDLIST_FIELD(head, elem))

#define XDLIST_N_ELEMNTS(head)	(head)->list_with_size.size

/* iteration */
#define XDLIST_LOOP_NEXT(var, head) XDLIST_ELEM((head), XDLIST_FIELD((head), (var))->next)
#define XDLIST_LOOP_PREV(var, head) XDLIST_ELEM((head), XDLIST_FIELD((head), (var))->prev)

/* loop with modifing the list */
#define XDLIST_FOREACH(var, head) \
	for ((var) = XDLIST_ELEM((head), (head)->list_with_size.list.next); \
		XDLIST_FIELD((head), (var)) != &(head)->list_with_size.list; \
		(var) = XDLIST_LOOP_NEXT((var), (head)))
#define XDLIST_FOREACH_REVERSE(var, head) \
	for ((var) = XDLIST_ELEM((head), (head)->list_with_size.list.prev); \
		XDLIST_FIELD((head), (var)) != &(head)->list_with_size.list; \
		(var) = XDLIST_LOOP_PREV((var), (head)))
/* loop and allow list modifications */
#define XDLIST_FOREACH_SAFE(var, head) \
	if (1) goto CONCAT(label_, __LINE__); \
	else for (typeof(var) CONCAT(t_, __LINE__); ; ) \
		if (1) break; \
		else CONCAT(label_, __LINE__): \
			for ((var) = XDLIST_ELEM((head), (head)->list_with_size.list.next), \
				CONCAT(t_, __LINE__) = XDLIST_LOOP_NEXT((var), (head)); \
				XDLIST_FIELD((head), (var)) != &(head)->list_with_size.list; \
				(var) = CONCAT(t_, __LINE__), \
				CONCAT(t_, __LINE__) = XDLIST_LOOP_NEXT((var), (head)))
#define XDLIST_FOREACH_REVERSE_SAFE(var, head) \
	if (1) goto CONCAT(label_, __LINE__); \
	else for (typeof(var) CONCAT(t_, __LINE__); ; ) \
		if (1) break; \
		else CONCAT(label_, __LINE__): \
			for ((var) = XDLIST_ELEM((head), (head)->list_with_size.list.prev), \
				CONCAT(t_, __LINE__) = XDLIST_LOOP_PREV((var), (head)); \
				XDLIST_FIELD((head), (var)) != &(head)->list_with_size.list; \
				(var) = CONCAT(t_, __LINE__), \
				CONCAT(t_, __LINE__) = XDLIST_LOOP_PREV((var), (head)))

/* ---------------- hashtable ----------------------------------------------- */
/* Copied from kernel code */

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL

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

/* Unsafe: static inline uint64_t xhash_str_to_32_bits(const char *str) { return murmur3_32((uint8_t *)str, strlen(str), 0xa9876543);} */

static inline uint64_t xhash_64(uint64_t val, uint32_t bits)
{
	uint64_t hash = val;

	/*  Sigh, gcc can't optimise this alone like it does for 32 bits. */
	uint64_t n = hash;
	n <<= 18;
	hash -= n;
	n <<= 33;
	hash -= n;
	n <<= 3;
	hash += n;
	n <<= 3;
	hash -= n;
	n <<= 4;
	hash += n;
	n <<= 2;
	hash += n;

	/* High bits are more random, so use them. */
	return hash >> (64 - bits);
}

static inline uint32_t xhash_32(uint32_t val, uint32_t bits)
{
	/* On some cpus multiply is faster, on others gcc will do shifts */
	uint32_t hash = val * GOLDEN_RATIO_PRIME_32;

	/* High bits are more random, so use them. */
	return hash >> (32 - bits);
}

/* declare a new hashtable */
#define XHASHTABLE_DECLARE_SIZE(head_type, type, field, bits, key_size) \
	union head_type { \
		char (*__key_size__)[key_size]; \
		struct {						\
				XDLIST_DECLARE(, type, field) hash[1 <<(bits)];	\
				int n_elements;									\
		} hash_data;											\
		char (*nbits)[bits]; 									\
	} head_type

#define XHASHTABLE_DECLARE(head_type, type, field, bits) \
	XHASHTABLE_DECLARE_SIZE(head_type, type, field, bits, sizeof(unsigned long long))

#define XHASHTABLE_KEYSIZE(name) sizeof(*(name)->__key_size__)

/* Use hash_32 when possible to allow for fast 32b hashing in 64b kernels. */
#define XHASHTABLE_MIN(val, name) \
	(XHASHTABLE_KEYSIZE(name) <= 4 ? xhash_32(val, XHASHTABLE_BITS(name)) : xhash_64(val, XHASHTABLE_BITS(name)))

/* Copied from kernel code however using XDLIST */
#define XHASHTABLE_N_BUCKETS(name) (ARRAY_SIZE((name)->hash_data.hash))
#define XHASHTABLE_BITS(name) sizeof(*(name)->nbits)

#define XHASHTABLE_TYPE(head)  XDLIST_TYPE(((head)->hash_data.hash))

/* initialize a hashtable*/
#define XHASHTABLE_INIT(name) \
	do { \
		for (int CONCAT(i_, __LINE__) = 0; \
			 (int)CONCAT(i_, __LINE__) < (int)XHASHTABLE_N_BUCKETS(name); \
			 ++CONCAT(i_, __LINE__)) \
			XDLIST_HEAD_INIT(&(name)->hash_data.hash[CONCAT(i_, __LINE__)]); \
		(name)->hash_data.n_elements = 0; \
	} while (0)

/* add an element to hashtable by key */
#define XHASHTABLE_ADD(name, elem, key) \
	(name)->hash_data.n_elements++; \
	XDLIST_ADD_TAIL(&(name)->hash_data.hash[ \
	XHASHTABLE_MIN(key, name)], elem)

#define XHASHTABLE_ADD_CHECK(trace_name, name, elem, key) \
	NXDLIST_ADD_TAIL_CHECK(trace_name ## AC, &(name)->hash[ \
	XHASHTABLE_MIN(key, name)], elem)

/* delete an element from hashtable */
#define XHASHTABLE_DEL(name, field)	do {	\
	if (!XDLIST_NULL(field)) {				\
		(name)->hash_data.n_elements--;		\
		XDLIST_DEL(field);					\
	}										\
} while (0)

/* check how many elements the hashtable has in any of the buckets */
#define XHASHTABLE_N_ELEMENTS(name) (name)->hash_data.n_elements

/* check if hashtable has elements in any of the buckets */
#define XHASHTABLE_EMPTY(name) (XHASHTABLE_N_ELEMENTS(name) == 0)


/* hashtable loop */
#define XHASHTABLE_FOR_EACH_SAFE(var, name) \
	if (1) { \
		(var) = NULL; \
		goto CONCAT(llabel_, __LINE__); \
	} else for (;;) \
		if (1) break; \
		else CONCAT(llabel_, __LINE__): \
			for (int CONCAT(i_, __LINE__) = 0, CONCAT(j_, __LINE__); \
				((var) == NULL || \
				 XDLIST_FIELD(&(name)->hash_data.hash[CONCAT(j_, __LINE__)], var) == \
				 &(name)->hash_data.hash[CONCAT(j_, __LINE__)].list_with_size.list) && \
				 (int)CONCAT(i_, __LINE__) < (int)XHASHTABLE_N_BUCKETS(name); \
				 CONCAT(j_, __LINE__) = CONCAT(i_, __LINE__), \
				 ++CONCAT(i_, __LINE__)) \
				 XDLIST_FOREACH_SAFE(var, &(name)->hash_data.hash[CONCAT(i_, __LINE__)])

#define XHASHTABLE_FOR_EACH_POSSIBLE_SAFE(var, name, key) \
	XDLIST_FOREACH_SAFE(var, &(name)->hash_data.hash[XHASHTABLE_MIN(key, name)])

/* Double Hash-table with 2-keys */
/* declare a new double-hashtable */
#define XDBL_HASHTABLE_DECLARE_SIZE(head_type, type, field, bits1, bits2, key_size) \
	union head_type { \
		char (*__key_size__)[key_size]; \
		struct {					\
			XDLIST_DECLARE(, type, field) hash[1 << (bits1)][1 << (bits2)]; \
			int n_elements;							\
		};									\
		char (*nbits1)[bits1]; \
		char (*nbits2)[bits2]; \
	}

#define XDBL_HASHTABLE_DECLARE(head_type, type, field, bits1, bits2) \
	XDBL_HASHTABLE_DECLARE_SIZE(head_type, type, field, bits1, bits2, sizeof(unsigned long long))

#define XDBL_HASHTABLE_N_BUCKETS2(name) (sizeof((name)->hash)/sizeof((name)->hash[0]))
#define XDBL_HASHTABLE_BITS2(name) (sizeof(*(name)->nbits2))

#define XDBL_HASHTABLE_N_BUCKETS1(name) (sizeof((name)->hash[0])/sizeof((name)->hash[0][0]))
#define XDBL_HASHTABLE_BITS1(name) (sizeof(*(name)->nbits1))

#define XHASHTABLE_MIN1(val, name) \
	(XHASHTABLE_KEYSIZE(name) <= 4 ? xhash_32(val, XDBL_HASHTABLE_BITS1(name)) : xhash_64(val, XDBL_HASHTABLE_BITS1(name)))

#define XHASHTABLE_MIN2(val, name) \
	(XHASHTABLE_KEYSIZE(name) <= 4 ? xhash_32(val, XDBL_HASHTABLE_BITS2(name)) : xhash_64(val, XDBL_HASHTABLE_BITS2(name)))

/* initialize a hashtable*/
#define XDBL_HASHTABLE_INIT(name) \
	do { \
		for (int CONCAT(i_, __LINE__) = 0; \
			(int)CONCAT(i_, __LINE__) < (int)XDBL_HASHTABLE_N_BUCKETS1(name); \
			++CONCAT(i_, __LINE__)) \
			for (int CONCAT(j_, __LINE__) = 0; \
				(int)CONCAT(j_, __LINE__) < (int)XDBL_HASHTABLE_N_BUCKETS2(name); \
				++CONCAT(j_, __LINE__)) \
				XDLIST_HEAD_INIT(&(name)->hash[CONCAT(i_, __LINE__)][CONCAT(j_, __LINE__)]); \
	} while (0)

/* add an element to hashtable by key */
#define XDBL_HASHTABLE_ADD(name, elem, key1, key2) \
	do { \
		XDLIST_ADD_TAIL(&(name)->hash[ \
		XHASHTABLE_MIN1(key1, name)][XHASHTABLE_MIN2(key2, name)], elem);\
		(name)->n_elements++;\
	} while (0)

#define XDBL_HASHTABLE_ADD_CHECK(name, elem, key) \
	do { \
		NXDLIST_ADD_TAIL_CHECK(&(name)->hash[ \
		XHASHTABLE_MIN1(key1, name)][XHASHTABLE_MIN2(key2, name)], elem);\
		(name)->n_elements++;\
	}

/* delete an element from hashtable */
#define XDBL_HASHTABLE_DEL(name, field) \
	do { \
		XDLIST_DEL(field);\
		(name)->n_elements--;\
	} while (0)

#define XDBL_HASHTABLE_N_ELEMENTS(name) ((name)->n_elements)

/* check if hashtable has elements in any of the buckets */
#define XDBL_HASHTABLE_EMPTY(name) ({ \
	bool CONCAT(f_, __LINE__) = true; \
	for (int CONCAT(i_, __LINE__); \
		CONCAT(i_, __LINE__) < XDBL_HASHTABLE_N_BUCKETS1(name); ++i); \
		++CONCAT(i_, __LINE__) { \
			for (int CONCAT(j_, __LINE__); \
				CONCAT(j_, __LINE__) < XDBL_HASHTABLE_N_BUCKETS2(name); ++j); \
				++CONCAT(j_, __LINE__) { \
				if (!XDLIST_EMPTY(&(name)->hash[CONCAT(i_, __LINE__)][CONCAT(j_, __LINE__)])) { \
					CONCAT(f_, __LINE__)= false; \
					break; \
				} \
			} \
		} \
		CONCAT(f_, __LINE__); \
})

/* hashtable loop */
#define XDBL_HASHTABLE_FOR_EACH(var, name) \
	if (1) { \
		(var) = NULL; \
		goto CONCAT(label_, __LINE__); \
	} \
	else for (;;) \
		if (1) break; \
			else CONCAT(label_, __LINE__): \
				for (int CONCAT(ii_, __LINE__) = 0; \
					CONCAT(ii_, __LINE__) < (int)XDBL_HASHTABLE_N_BUCKETS1(name); \
					++CONCAT(ii_, __LINE__)) \
					for (int CONCAT(i_, __LINE__) = 0, CONCAT(j_, __LINE__); \
						((var) == NULL || \
						XDLIST_FIELD(&(name)->hash[CONCAT(ii_, __LINE__)][CONCAT(j_, __LINE__)], var) == \
						&(name)->hash[CONCAT(ii_, __LINE__)][CONCAT(j_, __LINE__)].list_with_size.list) && \
						CONCAT(i_, __LINE__) < XDBL_HASHTABLE_N_BUCKETS2(name); \
						CONCAT(j_, __LINE__) = CONCAT(i_, __LINE__), \
						++CONCAT(i_, __LINE__)) \
						XDLIST_FOREACH(var, &(name)->hash[CONCAT(ii_, __LINE__)][CONCAT(i_, __LINE__)])

#define XDBL_HASHTABLE_FOR_EACH_SAFE(var, name) \
if (1) { \
	(var) = NULL; \
	goto CONCAT(llabel_, __LINE__); \
} \
else for (;;) \
	if (1) break; \
		else CONCAT(llabel_, __LINE__): \
			for (int CONCAT(ii_, __LINE__) = 0; \
				CONCAT(ii_, __LINE__) < (int)XDBL_HASHTABLE_N_BUCKETS1(name); \
				++CONCAT(ii_, __LINE__)) \
				for (int CONCAT(i_, __LINE__) = 0, CONCAT(j_, __LINE__) = 0; \
					((var) == NULL || \
					XDLIST_FIELD(&(name)->hash[CONCAT(ii_, __LINE__)][CONCAT(j_, __LINE__)], var) == \
					&(name)->hash[CONCAT(ii_, __LINE__)][CONCAT(j_, __LINE__)].list_with_size.list) && \
					(int)CONCAT(i_, __LINE__) < (int)XDBL_HASHTABLE_N_BUCKETS2(name); \
					CONCAT(j_, __LINE__) = CONCAT(i_, __LINE__), \
					++CONCAT(i_, __LINE__)) \
					XDLIST_FOREACH_SAFE(var, &(name)->hash[CONCAT(ii_, __LINE__)][CONCAT(i_, __LINE__)])

#define XDBL_HASHTABLE_FOR_EACH_POSSIBLE(var, name, key1, key2) \
	XDLIST_FOREACH(var, &(name)->hash[ \
	XHASHTABLE_MIN1(key1, name)][XHASHTABLE_MIN2(key2, name)])

#define XDBL_HASHTABLE_FOR_EACH_POSSIBLE_SAFE(var, name, key1, key2) \
	XDLIST_FOREACH_SAFE(var, &(name)->hash[ \
	XHASHTABLE_MIN1(key1, name)][XHASHTABLE_MIN2(key2, name)])

#endif	// NVMEIBT_DS
