/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * toma_test_framework_smoke.c - Smoke test for the test framework itself
 * Invoked via: ./nvmeibt_toma test_framework [selection]
 * Validates that the framework's assertion macros, test registration, and
 * selection/reporting work correctly.
 */

#include <stdio.h>
#include <string.h>
#include "unitest/00_framework/toma_test_framework.h"

//
// Smoke test definitions
//
#define SMOKE_TEST_LIST \
	X(pass_trivial,			"Trivial pass",						"Return 0") \
	X(assert_true_pass,		"ASSERT_TRUE (pass)",				"Assert 1 == 1") \
	X(assert_eq_pass,		"ASSERT_EQ (pass)",					"Assert 42 == 42") \
	X(assert_ne_pass,		"ASSERT_NE (pass)",					"Assert 1 != 2") \
	X(assert_null_pass,		"ASSERT_NULL (pass)",				"Assert NULL is NULL") \
	X(assert_not_null_pass,	"ASSERT_NOT_NULL (pass)",			"Assert non-NULL pointer") \
	X(assert_mem_eq_pass,	"ASSERT_MEM_EQ (pass)",				"Compare identical buffers") \
	X(multi_step_pass,		"Multi-step with goto out",			"Multiple assertions, all pass")

DEFINE_TEST(pass_trivial)
{
	(void)_ctx;
	return 0;
}

DEFINE_TEST(assert_true_pass)
{
	int		rv = -1;

	(void)_ctx;
	TEST_ASSERT_TRUE(1 == 1);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(assert_eq_pass)
{
	int		rv = -1;
	int		val = 42;

	(void)_ctx;
	TEST_ASSERT_EQ(val, 42);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(assert_ne_pass)
{
	int		rv = -1;

	(void)_ctx;
	TEST_ASSERT_NE(1, 2);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(assert_null_pass)
{
	int		rv = -1;
	void	*p = NULL;

	(void)_ctx;
	TEST_ASSERT_NULL(p);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(assert_not_null_pass)
{
	int		rv = -1;
	int		dummy = 0;

	(void)_ctx;
	TEST_ASSERT_NOT_NULL(&dummy);
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(assert_mem_eq_pass)
{
	int		rv = -1;
	char	a[] = "hello";
	char	b[] = "hello";

	(void)_ctx;
	TEST_ASSERT_MEM_EQ(a, b, sizeof(a));
	rv = 0;
out:
	return rv;
}

DEFINE_TEST(multi_step_pass)
{
	int		rv = -1;
	int		x = 10;
	int		y = 20;

	(void)_ctx;
	TEST_ASSERT_TRUE(x < y);
	TEST_ASSERT_EQ(x + y, 30);
	TEST_ASSERT_NE(x, y);
	rv = 0;
out:
	return rv;
}

int test_framework_smoke_main(int argc, char *argv[])
{
	const char	*selection = NULL;

	#define X(func, name, desc) {name, desc, test_##func},
	struct toma_test_entry tests[] = { SMOKE_TEST_LIST };
	#undef X

	if (argc > 1) {
		selection = argv[1];
	}

	return test_run_suite("Test Framework Smoke",
						  tests, (int)(sizeof(tests) / sizeof(tests[0])),
						  NULL, selection, 0);
}
