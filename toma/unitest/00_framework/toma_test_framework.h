/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/**
 * toma_test_framework.h - Minimal test framework for TOMA
 *
 * Provides generic test registration (X-macro), execution, selection, and reporting.
 * Based on the gpt_util SELF_TEST pattern.
 *
 * Usage:
 *   1. Define a TEST_LIST X-macro in your test header:
 *        #define MY_TEST_LIST \
 *            X(test_name, "Human-readable name", "Description or emulated command")
 *
 *   2. Define test functions:
 *        DEFINE_TEST(test_name) {
 *            int rv = -1;
 *            TEST_ASSERT_EQ(1 + 1, 2);
 *            rv = 0;
 *        out:
 *            return rv;
 *        }
 *
 *   3. Build the test array and call test_run_suite():
 *        #define X(func, name, cmd) {name, cmd, test_##func},
 *        struct toma_test_entry tests[] = { MY_TEST_LIST };
 *        #undef X
 *        return test_run_suite("My Suite", tests, sizeof(tests)/sizeof(tests[0]), my_ctx, selection);
 */

#include <stdio.h>
#include <string.h>

//
// Terminal colors (same as interfaces/log/log_incs.h, duplicated here for standalone use)
//
#ifndef COL_RESET
#define COL_RED			"\x1b[31m"
#define COL_GREEN		"\x1b[32m"
#define COL_YELLOW		"\x1b[1;33m"
#define COL_RED_BOLD	"\x1b[1;31m"
#define COL_WHITE_BOLD	"\x1b[1;37m"
#define COL_RESET		"\x1b[0;0m"
#define COL_BLUE		"\x1b[16;34m"
#endif // #ifndef COL_RESET

//
// Test function signature and registration
//
typedef int (*toma_test_func_t)(void *ctx);

struct toma_test_entry {
	const char			*name;
	const char			*description;
	toma_test_func_t	func;
};

//
// Define a test function.
// Convention: return 0 on success, non-zero on failure.
// The ctx parameter is a void* that the caller can cast to their domain-specific context.
//
#define DEFINE_TEST(name) \
	static int test_##name(void *_ctx)

//
// Assertion macros.
// These use the goto out pattern per TOMA coding conventions.
// The enclosing function must declare `int rv` and have an `out:` label.
//
#define TEST_ASSERT_TRUE(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, COL_RED_BOLD "  ASSERT_TRUE failed: %s" COL_RESET " (%s:%d)\n", \
			#expr, __FILE__, __LINE__); \
		rv = -1; \
		goto out; \
	} \
} while (0)

#define TEST_ASSERT_EQ(actual, expected) do { \
	if ((actual) != (expected)) { \
		fprintf(stderr, COL_RED_BOLD "  ASSERT_EQ failed: %s != %s" COL_RESET " (%s:%d)\n", \
			#actual, #expected, __FILE__, __LINE__); \
		rv = -1; \
		goto out; \
	} \
} while (0)

#define TEST_ASSERT_NE(actual, not_expected) do { \
	if ((actual) == (not_expected)) { \
		fprintf(stderr, COL_RED_BOLD "  ASSERT_NE failed: %s == %s" COL_RESET " (%s:%d)\n", \
			#actual, #not_expected, __FILE__, __LINE__); \
		rv = -1; \
		goto out; \
	} \
} while (0)

#define TEST_ASSERT_OK(expr) TEST_ASSERT_EQ((expr), 0)

#define TEST_ASSERT_NULL(ptr) do { \
	if ((ptr) != NULL) { \
		fprintf(stderr, COL_RED_BOLD "  ASSERT_NULL failed: %s is not NULL" COL_RESET " (%s:%d)\n", \
			#ptr, __FILE__, __LINE__); \
		rv = -1; \
		goto out; \
	} \
} while (0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
	if ((ptr) == NULL) { \
		fprintf(stderr, COL_RED_BOLD "  ASSERT_NOT_NULL failed: %s is NULL" COL_RESET " (%s:%d)\n", \
			#ptr, __FILE__, __LINE__); \
		rv = -1; \
		goto out; \
	} \
} while (0)

#define TEST_ASSERT_MEM_EQ(a, b, n) do { \
	if (memcmp((a), (b), (n)) != 0) { \
		fprintf(stderr, COL_RED_BOLD "  ASSERT_MEM_EQ failed: %s != %s (%d bytes)" COL_RESET " (%s:%d)\n", \
			#a, #b, (int)(n), __FILE__, __LINE__); \
		rv = -1; \
		goto out; \
	} \
} while (0)

//
// Run a test suite.
// @suite_name:  Display name for the suite
// @tests:       Array of toma_test_entry
// @num_tests:   Number of entries in the array
// @ctx:         Opaque context pointer passed to each test function (can be NULL)
// @selection:   Comma-separated test numbers/ranges (e.g. "1,3-5"), or NULL for all
// @quiet_mode:  If non-zero, suppress decorative banners (only show results)
// Returns: 0 if all selected tests passed, 1 if any failed
//
int test_run_suite(const char *suite_name,
				   struct toma_test_entry *tests,
				   int num_tests,
				   void *ctx,
				   const char *selection,
				   int quiet_mode);
