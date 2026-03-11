/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

/* Define IS_HASH_UNITTEST before including the hash implementation */
#define IS_HASH_UNITTEST 1

#if 0
/* Mock the uuid structure if not available */
union nvmeib_uuid {
	uint8_t bytes[16];
	uint32_t ints[4];
	uint64_t longs[2];
};

/* Mock the murmur3_32 hash function */
static inline uint32_t murmur3_32(const uint8_t* key, size_t len, uint32_t seed) {
	uint32_t h = seed;
	if (len > 3) {
		const uint32_t* key_x4 = (const uint32_t*) key;
		size_t i = len >> 2;
		do {
			uint32_t k = *key_x4++;
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			h ^= k;
			h = (h << 13) | (h >> 19);
			h = (h * 5) + 0xe6546b64;
		} while (--i);
		key = (const uint8_t*) key_x4;
	}
	if (len & 3) {
		size_t i = len & 3;
		uint32_t k = 0;
		key = &key[i - 1];
		do {
			k <<= 8;
			k |= *key--;
		} while (--i);
		k *= 0xcc9e2d51;
		k = (k << 15) | (k >> 17);
		k *= 0x1b873593;
		h ^= k;
	}
	h ^= len;
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

/* Mock time functions */
struct timespec {
	long tv_sec;
	long tv_nsec;
};

static inline int getnstimeofday_boot(struct timespec *ts) {
	struct timespec tmp;
	clock_gettime(CLOCK_BOOTTIME, &tmp);
	ts->tv_sec = tmp.tv_sec;
	ts->tv_nsec = tmp.tv_nsec;
	return 0;
}

static inline long long timespec_diff_ns(struct timespec end, struct timespec start) {
	return (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
}

#define MSEC_TO_NSEC(x) ((x) * 1000000LL)
#endif	// #if 0

/* Include the hash implementation */
#include "../../common/nvmeib_hash.h"
#include "../../common/nvmeib_hash.c"

/* Test statistics */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Test macros */
#define TEST_START(name) do { \
	fprintf(stdout, "\n=== TEST: %s ===\n", name); \
	tests_run++; \
} while(0)

#define TEST_PASS() do { \
	fprintf(stdout, "PASS\n"); \
	tests_passed++; \
} while(0)

#define TEST_FAIL(msg, ...) do { \
	fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
	tests_failed++; \
} while(0)

#define ASSERT_EQ(expected, actual, msg) do { \
	if ((expected) != (actual)) { \
		TEST_FAIL("%s: expected=%ld, actual=%ld", msg, (long)(expected), (long)(actual)); \
		return 0; \
	} \
} while(0)

#define ASSERT_PTR_EQ(expected, actual, msg) do { \
	if ((expected) != (actual)) { \
		TEST_FAIL("%s: expected=%p, actual=%p", msg, (void*)(expected), (void*)(actual)); \
		return 0; \
	} \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
	if (!(cond)) { \
		TEST_FAIL("%s", msg); \
		return 0; \
	} \
} while(0)

#define ASSERT_FALSE(cond, msg) do { \
	if (cond) { \
		TEST_FAIL("%s", msg); \
		return 0; \
	} \
} while(0)

/* Test functions */

/* Test basic uint32_t operations */
static int test_uint32_basic(void) {
	TEST_START("uint32_t basic operations");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_uint32", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	/* Test add */
	void *obj1 = (void *)0x1000;
	void *result = nvmeib_hash_add_uint32_t(ht, 42, obj1);
	ASSERT_PTR_EQ(NULL, result, "Add should return NULL for new key");
	ASSERT_EQ(1, ht->n_occupied, "Hash table should have 1 entry");
	
	/* Test search */
	void *found = nvmeib_hash_search_uint32_t(ht, 42);
	ASSERT_PTR_EQ(obj1, found, "Search should find the added object");
	
	/* Test add duplicate */
	void *obj2 = (void *)0x2000;
	result = nvmeib_hash_add_uint32_t(ht, 42, obj2);
	ASSERT_PTR_EQ(obj1, result, "Add duplicate should return old object");
	ASSERT_EQ(1, ht->n_occupied, "Hash table should still have 1 entry");
	
	/* Test delete */
	void *deleted = nvmeib_hash_delete_uint32_t(ht, 42);
	ASSERT_PTR_EQ(obj1, deleted, "Delete should return the deleted object");
	ASSERT_EQ(0, ht->n_occupied, "Hash table should be empty");
	
	/* Test search after delete */
	found = nvmeib_hash_search_uint32_t(ht, 42);
	ASSERT_PTR_EQ(NULL, found, "Search should not find deleted key");
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test uint64_t operations */
static int test_uint64_basic(void) {
	TEST_START("uint64_t basic operations");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_uint64", 8, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	void *obj1 = (void *)0x1000;
	void *result = nvmeib_hash_add_uint64_t(ht, 0x123456789ABCDEF0ULL, obj1);
	ASSERT_PTR_EQ(NULL, result, "Add should return NULL for new key");
	
	void *found = nvmeib_hash_search_uint64_t(ht, 0x123456789ABCDEF0ULL);
	ASSERT_PTR_EQ(obj1, found, "Search should find the added object");
	
	void *deleted = nvmeib_hash_delete_uint64_t(ht, 0x123456789ABCDEF0ULL);
	ASSERT_PTR_EQ(obj1, deleted, "Delete should return the deleted object");
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test UUID operations */
static int test_uuid_basic(void) {
	TEST_START("UUID basic operations");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_uuid", 16, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	union nvmeib_uuid uuid1 = {.ints = {1, 2, 3, 4}};
	void *obj1 = (void *)0x1000;
	
	void *result = nvmeib_hash_add_uuid(ht, &uuid1, obj1);
	ASSERT_PTR_EQ(NULL, result, "Add should return NULL for new key");
	
	void *found = nvmeib_hash_search_uuid(ht, &uuid1);
	ASSERT_PTR_EQ(obj1, found, "Search should find the added object");
	
	/* Test with different UUID */
	union nvmeib_uuid uuid2 = {.ints = {5, 6, 7, 8}};
	found = nvmeib_hash_search_uuid(ht, &uuid2);
	ASSERT_PTR_EQ(NULL, found, "Search should not find non-existent key");
	
	void *deleted = nvmeib_hash_delete_uuid(ht, &uuid1);
	ASSERT_PTR_EQ(obj1, deleted, "Delete should return the deleted object");
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test ASCII string operations */
static int test_ascii_basic(void) {
	TEST_START("ASCII string basic operations");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_ascii", -1, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	const char *key1 = "hello_world";
	void *obj1 = (void *)0x1000;
	
	void *result = nvmeib_hash_add_ascii_str(ht, key1, obj1);
	ASSERT_PTR_EQ(NULL, result, "Add should return NULL for new key");
	
	void *found = nvmeib_hash_search_ascii_str(ht, key1);
	ASSERT_PTR_EQ(obj1, found, "Search should find the added object");
	
	/* Test with different string */
	const char *key2 = "hello_world_different";
	found = nvmeib_hash_search_ascii_str(ht, key2);
	ASSERT_PTR_EQ(NULL, found, "Search should not find non-existent key");
	
	void *deleted = nvmeib_hash_delete_ascii_str(ht, key1);
	ASSERT_PTR_EQ(obj1, deleted, "Delete should return the deleted object");
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test collision handling */
static int test_collisions(void) {
	TEST_START("Collision handling");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(3, "test_collisions", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	/* Add multiple entries to force collisions */
	for (int i = 0; i < 20; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		void *result = nvmeib_hash_add_uint32_t(ht, i, obj);
		ASSERT_PTR_EQ(NULL, result, "Add should succeed for all keys");
	}
	
	ASSERT_EQ(20, ht->n_occupied, "Hash table should have 20 entries");
	
	/* Verify all entries can be found */
	for (int i = 0; i < 20; i++) {
		void *expected = (void *)(long)(0x1000 + i);
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		ASSERT_PTR_EQ(expected, found, "All keys should be findable");
	}
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test resize operations */
static int test_resize(void) {
	TEST_START("Hash table resize");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(3, "test_resize", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	int initial_size = ht->n_arr_entries;
	fprintf(stdout, "Initial size: %d\n", initial_size);
	
	/* Add enough entries to trigger resize */
	for (int i = 0; i < 100; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		nvmeib_hash_add_uint32_t(ht, i, obj);
	}
	
	int final_size = ht->n_arr_entries;
	fprintf(stdout, "Final size: %d\n", final_size);
	
	ASSERT_TRUE(final_size > initial_size, "Hash table should have grown");
	ASSERT_EQ(100, ht->n_occupied, "Hash table should have 100 entries");
	
	/* Verify all entries are still accessible */
	for (int i = 0; i < 100; i++) {
		void *expected = (void *)(long)(0x1000 + i);
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		ASSERT_PTR_EQ(expected, found, "All keys should still be findable after resize");
	}
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test shrink operations */
static int test_shrink(void) {
	TEST_START("Hash table shrink");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(8, "test_shrink", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	/* Add many entries */
	for (int i = 0; i < 200; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		nvmeib_hash_add_uint32_t(ht, i, obj);
	}
	
	int size_after_add = ht->n_arr_entries;
	fprintf(stdout, "Size after adding 200 entries: %d\n", size_after_add);
	
	/* Delete most entries */
	for (int i = 0; i < 180; i++) {
		nvmeib_hash_delete_uint32_t(ht, i);
	}
	
	/* Trigger resize check */
	nvmeib_hash_resize(ht);
	
	int size_after_delete = ht->n_arr_entries;
	fprintf(stdout, "Size after deleting 180 entries: %d\n", size_after_delete);
	
	ASSERT_EQ(20, ht->n_occupied, "Hash table should have 20 entries");
	
	/* Verify remaining entries */
	for (int i = 180; i < 200; i++) {
		void *expected = (void *)(long)(0x1000 + i);
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		ASSERT_PTR_EQ(expected, found, "Remaining keys should be findable");
	}
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test delete with chain rearrangement */
static int test_delete_chain(void) {
	TEST_START("Delete with chain rearrangement");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(3, "test_delete_chain", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	/* Add entries */
	for (int i = 0; i < 50; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		nvmeib_hash_add_uint32_t(ht, i, obj);
	}
	
	/* Delete entries from the middle */
	for (int i = 10; i < 20; i++) {
		void *deleted = nvmeib_hash_delete_uint32_t(ht, i);
		ASSERT_TRUE(deleted != NULL, "Delete should succeed");
	}
	
	ASSERT_EQ(40, ht->n_occupied, "Hash table should have 40 entries");
	
	/* Verify remaining entries are still accessible */
	for (int i = 0; i < 10; i++) {
		void *expected = (void *)(long)(0x1000 + i);
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		ASSERT_PTR_EQ(expected, found, "Keys before deleted range should be findable");
	}
	
	for (int i = 20; i < 50; i++) {
		void *expected = (void *)(long)(0x1000 + i);
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		ASSERT_PTR_EQ(expected, found, "Keys after deleted range should be findable");
	}
	
	/* Verify deleted entries are not found */
	for (int i = 10; i < 20; i++) {
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		ASSERT_PTR_EQ(NULL, found, "Deleted keys should not be findable");
	}
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test FOREACH macro */
static int test_foreach(void) {
	TEST_START("FOREACH macro");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_foreach", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	/* Add entries */
	for (int i = 0; i < 10; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		nvmeib_hash_add_uint32_t(ht, i, obj);
	}
	
	/* Count entries using FOREACH */
	int count = 0;
	void *obj;
	NVMEIB_HASH_FOREACH(obj, ht) {
		count++;
		ASSERT_TRUE(obj != NULL, "FOREACH should not yield NULL objects");
	}
	
	ASSERT_EQ(10, count, "FOREACH should iterate over all entries");
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test empty hash table operations */
static int test_empty_table(void) {
	TEST_START("Empty hash table operations");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_empty", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	/* Search in empty table */
	void *found = nvmeib_hash_search_uint32_t(ht, 42);
	ASSERT_PTR_EQ(NULL, found, "Search in empty table should return NULL");
	
	/* Delete from empty table */
	void *deleted = nvmeib_hash_delete_uint32_t(ht, 42);
	ASSERT_PTR_EQ(NULL, deleted, "Delete from empty table should return NULL");
	
	/* FOREACH on empty table */
	int count = 0;
	void *obj;
	NVMEIB_HASH_FOREACH(obj, ht) {
		count++;
	}
	ASSERT_EQ(0, count, "FOREACH on empty table should not iterate");
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test reuse of deleted slots */
static int test_reuse_deleted_slots(void) {
	TEST_START("Reuse of deleted slots");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_reuse", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	/* Add and delete entries */
	for (int i = 0; i < 20; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		nvmeib_hash_add_uint32_t(ht, i, obj);
	}
	
	for (int i = 0; i < 10; i++) {
		nvmeib_hash_delete_uint32_t(ht, i);
	}
	
	int size_after_delete = ht->n_arr_entries;
	
	/* Add new entries */
	for (int i = 20; i < 30; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		nvmeib_hash_add_uint32_t(ht, i, obj);
	}
	
	/* Size should not have changed significantly if slots were reused */
	ASSERT_EQ(20, ht->n_occupied, "Hash table should have 20 entries");
	
	/* Verify all current entries */
	for (int i = 10; i < 30; i++) {
		void *expected = (void *)(long)(0x1000 + i);
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		ASSERT_PTR_EQ(expected, found, "All current keys should be findable");
	}
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test large dataset */
static int test_large_dataset(void) {
	TEST_START("Large dataset");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(8, "test_large", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	const int N = 10000;
	
	/* Add large number of entries */
	for (int i = 0; i < N; i++) {
		void *obj = (void *)(long)(0x1000 + i);
		void *result = nvmeib_hash_add_uint32_t(ht, i, obj);
		if (result != NULL) {
			TEST_FAIL("Add failed at iteration %d", i);
			nvmeib_hash_tbl_free(ht);
			return 0;
		}
	}
	
	ASSERT_EQ(N, ht->n_occupied, "Hash table should have all entries");
	
	/* Verify all entries */
	for (int i = 0; i < N; i++) {
		void *expected = (void *)(long)(0x1000 + i);
		void *found = nvmeib_hash_search_uint32_t(ht, i);
		if (found != expected) {
			TEST_FAIL("Search failed at iteration %d: expected=%p, found=%p", 
				i, expected, found);
			nvmeib_hash_tbl_free(ht);
			return 0;
		}
	}
	
	/* Delete all entries */
	for (int i = 0; i < N; i++) {
		void *deleted = nvmeib_hash_delete_uint32_t(ht, i);
		if (deleted != (void *)(long)(0x1000 + i)) {
			TEST_FAIL("Delete failed at iteration %d", i);
			nvmeib_hash_tbl_free(ht);
			return 0;
		}
	}
	
	ASSERT_EQ(0, ht->n_occupied, "Hash table should be empty");
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test ASCII strings with varying lengths */
static int test_ascii_varying_lengths(void) {
	TEST_START("ASCII strings with varying lengths");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(4, "test_ascii_var", -1, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	char buffer[256];
	
	/* Add strings of different lengths */
	for (int i = 1; i <= 100; i++) {
		snprintf(buffer, sizeof(buffer), "string_%d_%.*s", i, i, 
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
		char *key = strdup(buffer);
		nvmeib_hash_add_ascii_str(ht, key, key);
	}
	
	ASSERT_EQ(100, ht->n_occupied, "Hash table should have 100 entries");
	
	/* Verify all strings */
	for (int i = 1; i <= 100; i++) {
		snprintf(buffer, sizeof(buffer), "string_%d_%.*s", i, i, 
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
		void *found = nvmeib_hash_search_ascii_str(ht, buffer);
		ASSERT_TRUE(found != NULL, "All strings should be findable");
		ASSERT_EQ(0, strcmp((char *)found, buffer), "String content should match");
	}
	
	/* Clean up */
	char *str;
	NVMEIB_HASH_FOREACH(str, ht) {
		free(str);
	}
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Test multiple hash tables */
static int test_multiple_tables(void) {
	TEST_START("Multiple hash tables");
	
	struct nvmeib_hash_table *ht1 = nvmeib_hash_create(4, "table1", 4, false);
	struct nvmeib_hash_table *ht2 = nvmeib_hash_create(4, "table2", 8, false);
	struct nvmeib_hash_table *ht3 = nvmeib_hash_create(4, "table3", 16, false);
	
	ASSERT_TRUE(ht1 != NULL && ht2 != NULL && ht3 != NULL, 
		"Failed to create hash tables");
	
	/* Add to each table */
	for (int i = 0; i < 50; i++) {
		nvmeib_hash_add_uint32_t(ht1, i, (void *)(long)(0x1000 + i));
		nvmeib_hash_add_uint64_t(ht2, i, (void *)(long)(0x2000 + i));
		union nvmeib_uuid uuid = {.ints = {i, i, i, i}};
		nvmeib_hash_add_uuid(ht3, &uuid, (void *)(long)(0x3000 + i));
	}
	
	/* Verify each table */
	for (int i = 0; i < 50; i++) {
		void *found1 = nvmeib_hash_search_uint32_t(ht1, i);
		ASSERT_PTR_EQ((void *)(long)(0x1000 + i), found1, "Table 1 lookup failed");
		
		void *found2 = nvmeib_hash_search_uint64_t(ht2, i);
		ASSERT_PTR_EQ((void *)(long)(0x2000 + i), found2, "Table 2 lookup failed");
		
		union nvmeib_uuid uuid = {.ints = {i, i, i, i}};
		void *found3 = nvmeib_hash_search_uuid(ht3, &uuid);
		ASSERT_PTR_EQ((void *)(long)(0x3000 + i), found3, "Table 3 lookup failed");
	}
	
	nvmeib_hash_tbl_free(ht1);
	nvmeib_hash_tbl_free(ht2);
	nvmeib_hash_tbl_free(ht3);
	
	TEST_PASS();
	return 1;
}

/* Performance benchmark */
static int benchmark_operations(void) {
	TEST_START("Performance benchmark");
	
	struct nvmeib_hash_table *ht = nvmeib_hash_create(12, "benchmark", 4, false);
	ASSERT_TRUE(ht != NULL, "Failed to create hash table");
	
	struct timespec start, end;
	const int N = 100000;
	
	/* Benchmark adds */
	getnstimeofday_boot(&start);
	for (int i = 0; i < N; i++) {
		nvmeib_hash_add_uint32_t(ht, i, (void *)(long)i);
	}
	getnstimeofday_boot(&end);
	long long add_time = timespec_diff_ns(end, start);
	fprintf(stdout, "Add %d entries: %lld ns (%.2f ns/op)\n", 
		N, add_time, (double)add_time / N);
	
	/* Benchmark searches */
	getnstimeofday_boot(&start);
	for (int i = 0; i < N; i++) {
		nvmeib_hash_search_uint32_t(ht, i);
	}
	getnstimeofday_boot(&end);
	long long search_time = timespec_diff_ns(end, start);
	fprintf(stdout, "Search %d entries: %lld ns (%.2f ns/op)\n", 
		N, search_time, (double)search_time / N);
	
	/* Benchmark deletes */
	getnstimeofday_boot(&start);
	for (int i = 0; i < N; i++) {
		nvmeib_hash_delete_uint32_t(ht, i);
	}
	getnstimeofday_boot(&end);
	long long delete_time = timespec_diff_ns(end, start);
	fprintf(stdout, "Delete %d entries: %lld ns (%.2f ns/op)\n", 
		N, delete_time, (double)delete_time / N);
	
	nvmeib_hash_tbl_free(ht);
	TEST_PASS();
	return 1;
}

/* Main test runner */
int main(int argc, char **argv) {
	fprintf(stdout, "========================================\n");
	fprintf(stdout, "  NVMEIB Hash Table Test Suite\n");
	fprintf(stdout, "========================================\n");
	
	/* Run all tests */
	test_uint32_basic();
	test_uint64_basic();
	test_uuid_basic();
	test_ascii_basic();
	test_collisions();
	test_resize();
	test_shrink();
	test_delete_chain();
	test_foreach();
	test_empty_table();
	test_reuse_deleted_slots();
	test_large_dataset();
	test_ascii_varying_lengths();
	test_multiple_tables();
	benchmark_operations();
	
	/* Print summary */
	fprintf(stdout, "\n========================================\n");
	fprintf(stdout, "  Test Summary\n");
	fprintf(stdout, "========================================\n");
	fprintf(stdout, "Tests run:    %d\n", tests_run);
	fprintf(stdout, "Tests passed: %d\n", tests_passed);
	fprintf(stdout, "Tests failed: %d\n", tests_failed);
	fprintf(stdout, "========================================\n");
	
	if (tests_failed == 0) {
		fprintf(stdout, "✓ ALL TESTS PASSED\n");
		return 0;
	} else {
		fprintf(stdout, "✗ SOME TESTS FAILED\n");
		return 1;
	}
}








