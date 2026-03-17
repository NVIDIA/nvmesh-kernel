/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

/* Setup for trace_daemon_common.h - must be defined before including */
#define __MODULE_HDR "unlink_list_test"
#define __MODULE_HDR_ARGS "test"

#include "trace_daemon_common.h"
#include "unlink_list.h"

/* Define trace_cfg for testing (it's extern in trace_daemon_common.h) */
trace_daemon_cfg_t trace_cfg = INIT_TRACE_CFG;

/* Simple test framework */
#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); \
			return 1; \
		} \
	} while(0)

#define TEST_PASS(msg) \
	do { \
		printf("PASS: %s\n", msg); \
	} while(0)

static int test_count = 0;
static int fail_count = 0;

#define RUN_TEST(test_func) \
	do { \
		test_count++; \
		if (test_func()) { \
			fail_count++; \
			fprintf(stderr, "Test failed: %s\n", #test_func); \
		} \
	} while(0)

/* Test helper: Create a test file */
static int create_test_file(const char *dir, const char *name, int cpu, int id, const char *ext)
{
	char filename[MAX_FILENAME];
	snprintf(filename, sizeof(filename), "%s/%s%d.%d%s", dir, name, cpu, id, ext ? ext : "");
	int fd = open(filename, O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

/* Test helper: Check if file exists */
static int file_exists(const char *dir, const char *filename)
{
	char path[MAX_FILENAME];
	snprintf(path, sizeof(path), "%s/%s", dir, filename);
	return access(path, F_OK) == 0;
}

/* Test helper: Remove all test files */
static void cleanup_test_files(const char *dir, const char *name)
{
	char cmd[MAX_FILENAME * 2];
	snprintf(cmd, sizeof(cmd), "rm -f %s/%s*", dir, name);
	system(cmd);
}

/* Test 1: Basic init and destroy */
static int test_init_destroy(void)
{
	unlink_list_t list;
	const char *dir = "/tmp/test";
	const char *name = "test";

	unlink_list_init(&list, dir, name);
	TEST_ASSERT(list.dir == dir, "dir pointer should match");
	TEST_ASSERT(list.name == name, "name pointer should match");
	TEST_ASSERT(list.len == 0, "len should be 0");
	TEST_ASSERT(list.head == NULL, "head should be NULL");
	TEST_ASSERT(list.tail == NULL, "tail should be NULL");
	TEST_ASSERT(list.open_files == 0, "open_files should be 0");

	unlink_list_destroy(&list);
	TEST_PASS("init and destroy");
	return 0;
}

/* Test 2: Add and get candidates */
static int test_add_get(void)
{
	unlink_list_t list;
	const char *dir = "/tmp/test";
	const char *name = "test";

	unlink_list_init(&list, dir, name);

	/* Add candidates */
	unlink_list_add(&list, 0, 1, 100);
	unlink_list_add(&list, 0, 2, 200);
	unlink_list_add(&list, 1, 1, 150);

	TEST_ASSERT(list.len == 3, "len should be 3");

	/* Get next with max_logs = 0 (should return NULL) */
	unlink_candidate_t *cand = unlink_list_get_next(&list, 0);
	TEST_ASSERT(cand == NULL, "should return NULL when max_logs=0");

	/* Get next with max_logs = 2 (should return oldest) */
	cand = unlink_list_get_next(&list, 2);
	TEST_ASSERT(cand != NULL, "should return candidate");
	TEST_ASSERT(cand->cpu == 0, "cpu should be 0");
	TEST_ASSERT(cand->id == 1, "id should be 1");
	TEST_ASSERT(cand->ts == 100, "ts should be 100");
	free(cand);
	TEST_ASSERT(list.len == 2, "len should be 2 after removal");

	/* Keep calling unlink_list_get_next until list has single item */
	/* Now len=2, use max_logs=1 to get next candidate */
	cand = unlink_list_get_next(&list, 1);
	TEST_ASSERT(cand != NULL, "should return candidate");
	TEST_ASSERT(cand->ts == 150, "ts should be 150 (second oldest)");
	free(cand);
	TEST_ASSERT(list.len == 1, "len should be 1 after second removal");

	unlink_list_destroy(&list);
	TEST_PASS("add and get");
	return 0;
}

/* Test 3: Timestamp ordering */
static int test_timestamp_ordering(void)
{
	unlink_list_t list;
	const char *dir = "/tmp/test";
	const char *name = "test";
	time_t expected_ts[] = {100, 200, 300}; /* Expected timestamps in order */
	int expected_idx = 0;

	unlink_list_init(&list, dir, name);

	/* Add in random order */
	unlink_list_add(&list, 0, 1, 300);
	unlink_list_add(&list, 0, 2, 100);
	unlink_list_add(&list, 0, 3, 200);

	/* Iterate over the list similar to _update_workers_from_unlink_list */
	unlink_candidate_t *cand;
	pthread_mutex_t *lock = &list.lock;

	pthread_mutex_lock(lock);
	cand = list.head;
	while(cand)
	{
		TEST_ASSERT(expected_idx < 3, "should not exceed expected count");
		TEST_ASSERT(cand->ts == expected_ts[expected_idx], 
			"should be in timestamp order");
		expected_idx++;
		cand = cand->next;
	}
	pthread_mutex_unlock(lock);

	TEST_ASSERT(expected_idx == 3, "should have iterated over all 3 entries");
	TEST_ASSERT(list.len == 3, "list should still have 3 entries");

	unlink_list_destroy(&list);
	TEST_PASS("timestamp ordering");
	return 0;
}

/* Test 4: MAX_TS handling */
static int test_max_ts(void)
{
	unlink_list_t list;
	const char *dir = "/tmp/test";
	const char *name = "test";

	unlink_list_init(&list, dir, name);

	/* Add with MAX_TS (should go to tail) */
	unlink_list_add(&list, 0, 1, 100);
	unlink_list_add(&list, 0, 2, MAX_TS);
	unlink_list_add(&list, 0, 3, 200);

	/* unlink_list_get_next only returns candidates if (len + open_files) > max_logs */
	/* Should get oldest first (ts=100) */
	unlink_candidate_t *cand = unlink_list_get_next(&list, 2);
	TEST_ASSERT(cand != NULL, "should return candidate");
	TEST_ASSERT(cand->ts == 100, "should return oldest");
	free(cand);

	/* Next should be ts=200 (MAX_TS goes to tail) */
	cand = unlink_list_get_next(&list, 1);
	TEST_ASSERT(cand != NULL, "should return candidate");
	TEST_ASSERT(cand->ts == 200, "should return second oldest");
	free(cand);

	/* Last should be MAX_TS */
	/* For the last entry, we need to ensure (len + open_files) > max_logs */
	/* Since len=1, we can set open_files=1 and max_logs=1, so (1+1) > 1 */
	list.open_files = 1;
	cand = unlink_list_get_next(&list, 1);
	TEST_ASSERT(cand != NULL, "should return candidate");
	TEST_ASSERT(cand->ts == MAX_TS, "should return MAX_TS last");
	free(cand);
	list.open_files = 0;

	unlink_list_destroy(&list);
	TEST_PASS("MAX_TS handling");
	return 0;
}

/* Test 5: Open files counter */
static int test_open_files_counter(void)
{
	unlink_list_t list;
	const char *dir = "/tmp/test";
	const char *name = "test";

	unlink_list_init(&list, dir, name);
	TEST_ASSERT(list.open_files == 0, "initial open_files should be 0");

	unlink_list_increment_open_files(&list);
	TEST_ASSERT(list.open_files == 1, "open_files should be 1");

	unlink_list_increment_open_files(&list);
	TEST_ASSERT(list.open_files == 2, "open_files should be 2");

	unlink_list_decrement_open_files(&list);
	TEST_ASSERT(list.open_files == 1, "open_files should be 1");

	unlink_list_decrement_open_files(&list);
	TEST_ASSERT(list.open_files == 0, "open_files should be 0");

	unlink_list_destroy(&list);
	TEST_PASS("open files counter");
	return 0;
}

/* Test 6: Parse log filename */
static int test_parse_log_filename(void)
{
	int cpu, idx;

	/* Valid filename */
	TEST_ASSERT(unlink_list_parse_log_filename("test0.1", "test", &cpu, &idx, 1024) == 1,
		"should parse valid filename");
	TEST_ASSERT(cpu == 0, "cpu should be 0");
	TEST_ASSERT(idx == 1, "idx should be 1");

	/* Valid filename with larger numbers */
	TEST_ASSERT(unlink_list_parse_log_filename("test42.123", "test", &cpu, &idx, 1024) == 1,
		"should parse valid filename");
	TEST_ASSERT(cpu == 42, "cpu should be 42");
	TEST_ASSERT(idx == 123, "idx should be 123");

	/* Invalid: wrong prefix */
	TEST_ASSERT(unlink_list_parse_log_filename("wrong0.1", "test", &cpu, &idx, 1024) == 0,
		"should reject wrong prefix");

	/* Invalid: no dot */
	TEST_ASSERT(unlink_list_parse_log_filename("test01", "test", &cpu, &idx, 1024) == 0,
		"should reject missing dot");

	/* Invalid: cpu too large */
	TEST_ASSERT(unlink_list_parse_log_filename("test2000.1", "test", &cpu, &idx, 1024) == 0,
		"should reject cpu >= max_cpus");

	TEST_PASS("parse log filename");
	return 0;
}

/* Test 7: Populate from directory */
static int test_populate(void)
{
	extern const char *g_test_dir;
	const char *test_dir = g_test_dir;
	unlink_list_t list;
	const char *name = "test";

	/* Cleanup any existing files */
	cleanup_test_files(test_dir, name);

	/* Create test files */
	create_test_file(test_dir, name, 0, 1, "");
	create_test_file(test_dir, name, 0, 2, "");
	create_test_file(test_dir, name, 1, 1, "");
	create_test_file(test_dir, name, 2, 5, "");

	unlink_list_init(&list, test_dir, name);
	TEST_ASSERT(unlink_list_populate(&list, 1024) == 0, "populate should succeed");
	TEST_ASSERT(list.len == 4, "should find 4 files");

	unlink_list_destroy(&list);
	cleanup_test_files(test_dir, name);
	TEST_PASS("populate from directory");
	return 0;
}

/* Test 8: Try remove log file */
static int test_try_remove_log_file(void)
{
	extern const char *g_test_dir;
	const char *test_dir = g_test_dir;
	unlink_list_t list;
	const char *name = "test";
	unlink_candidate_t cand = {.cpu = 0, .id = 1, .ts = 0};

	/* Cleanup any existing files */
	cleanup_test_files(test_dir, name);

	unlink_list_init(&list, test_dir, name);

	/* Try to remove non-existent file (should succeed, ignore ENOENT) */
	int rv = unlink_list_try_remove_log_file(&list, &cand, "", "");
	TEST_ASSERT(rv == 0, "should succeed even if file doesn't exist");

	/* Create file and remove it */
	create_test_file(test_dir, name, 0, 1, "");
	TEST_ASSERT(file_exists(test_dir, "test0.1"), "file should exist");
	rv = unlink_list_try_remove_log_file(&list, &cand, "", "");
	TEST_ASSERT(rv == 0, "should succeed");
	TEST_ASSERT(!file_exists(test_dir, "test0.1"), "file should be removed");

	/* Test with extension */
	create_test_file(test_dir, name, 0, 1, ".lz4");
	TEST_ASSERT(file_exists(test_dir, "test0.1.lz4"), "file should exist");
	rv = unlink_list_try_remove_log_file(&list, &cand, "", ".lz4");
	TEST_ASSERT(rv == 0, "should succeed");
	TEST_ASSERT(!file_exists(test_dir, "test0.1.lz4"), "file should be removed");

	/* Test with subdir */
	char subdir_path[MAX_FILENAME];
	snprintf(subdir_path, sizeof(subdir_path), "%s/.cache", test_dir);
	mkdir(subdir_path, 0755);
	create_test_file(test_dir, name, 0, 1, "");
	char full_path[MAX_FILENAME];
	snprintf(full_path, sizeof(full_path), "%s/.cache/test0.1", test_dir);
	/* Move file to subdir for testing */
	char cmd[MAX_FILENAME * 2];
	snprintf(cmd, sizeof(cmd), "mv %s/test0.1 %s", test_dir, full_path);
	system(cmd);
	TEST_ASSERT(file_exists(test_dir, ".cache/test0.1"), "file should exist in subdir");
	rv = unlink_list_try_remove_log_file(&list, &cand, ".cache", "");
	TEST_ASSERT(rv == 0, "should succeed");
	TEST_ASSERT(!file_exists(test_dir, ".cache/test0.1"), "file should be removed");

	unlink_list_destroy(&list);
	cleanup_test_files(test_dir, name);
	snprintf(cmd, sizeof(cmd), "rmdir %s/.cache 2>/dev/null", test_dir);
	system(cmd);
	TEST_PASS("try remove log file");
	return 0;
}

/* Test 9: Do unlink */
static int test_do_unlink(void)
{
	extern const char *g_test_dir;
	const char *test_dir = g_test_dir;
	unlink_list_t list;
	const char *name = "test";

	/* Cleanup any existing files */
	cleanup_test_files(test_dir, name);

	unlink_list_init(&list, test_dir, name);

	/* Add candidates */
	unlink_list_add(&list, 0, 1, 100);
	unlink_list_add(&list, 0, 2, 200);
	unlink_list_add(&list, 1, 1, 150);

	/* Create corresponding files */
	create_test_file(test_dir, name, 0, 1, "");
	create_test_file(test_dir, name, 0, 2, "");
	create_test_file(test_dir, name, 1, 1, "");

	/* Set open_files to 0, max_logs to 2 - should unlink 1 file */
	list.open_files = 0;
	int unlinked = unlink_list_do_unlink(&list, 2);
	TEST_ASSERT(unlinked == 1, "should unlink 1 file");
	TEST_ASSERT(list.len == 2, "should have 2 entries left");
	TEST_ASSERT(!file_exists(test_dir, "test0.1"), "oldest file should be removed");

	unlink_list_destroy(&list);
	cleanup_test_files(test_dir, name);
	TEST_PASS("do unlink");
	return 0;
}

/* Test 10: Clear vs destroy */
static int test_clear_vs_destroy(void)
{
	unlink_list_t list;
	const char *dir = "/tmp/test";
	const char *name = "test";

	unlink_list_init(&list, dir, name);

	/* Add some entries */
	unlink_list_add(&list, 0, 1, 100);
	unlink_list_add(&list, 0, 2, 200);
	TEST_ASSERT(list.len == 2, "should have 2 entries");

	/* Clear should remove entries but keep dir/name */
	unlink_list_clear(&list);
	TEST_ASSERT(list.len == 0, "should have 0 entries after clear");
	TEST_ASSERT(list.dir == dir, "dir should still be set");
	TEST_ASSERT(list.name == name, "name should still be set");

	/* Can add again after clear */
	unlink_list_add(&list, 0, 3, 300);
	TEST_ASSERT(list.len == 1, "should be able to add after clear");

	unlink_list_destroy(&list);
	TEST_ASSERT(list.dir == NULL, "dir should be nullified after destroy");
	TEST_ASSERT(list.name == NULL, "name should be nullified after destroy");
	TEST_PASS("clear vs destroy");
	return 0;
}

const char *g_test_dir = "/tmp/unlink_list_test";

int main(int argc, char *argv[])
{
	if (argc > 1) {
		g_test_dir = argv[1];
	}

	printf("Running unlink_list tests in directory: %s\n", g_test_dir);

	/* Create test directory */
	char cmd[MAX_FILENAME * 2];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s", g_test_dir);
	system(cmd);

	RUN_TEST(test_init_destroy);
	RUN_TEST(test_add_get);
	RUN_TEST(test_timestamp_ordering);
	RUN_TEST(test_max_ts);
	RUN_TEST(test_open_files_counter);
	RUN_TEST(test_parse_log_filename);
	RUN_TEST(test_populate);
	RUN_TEST(test_try_remove_log_file);
	RUN_TEST(test_do_unlink);
	RUN_TEST(test_clear_vs_destroy);

	/* Cleanup */
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_test_dir);
	system(cmd);

	printf("\nTest Summary: %d tests, %d passed, %d failed\n", 
		test_count, test_count - fail_count, fail_count);

	return fail_count > 0 ? 1 : 0;
}

