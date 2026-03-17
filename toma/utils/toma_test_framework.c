/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * toma_test_framework.c - Minimal test framework runner
 *
 * Generic test suite execution: selection parsing, iteration, and summary reporting.
 * See toma_test_framework.h for usage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "toma_test_framework.h"

#define MAX_TESTS	256

static void test_print_banner(int test_num, const char *name, const char *description, int quiet_mode)
{
	if (quiet_mode) {
		return;
	}
	fprintf(stdout, "\n");
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
	fprintf(stdout, COL_WHITE_BOLD "TEST %d: %s" COL_RESET "\n", test_num, name);
	if (description && description[0]) {
		fprintf(stdout, "  %s\n", description);
	}
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
}

static int test_print_result(int test_num, int result)
{
	if (result == 0) {
		fprintf(stdout, COL_GREEN ">>> TEST %d: PASSED <<<" COL_RESET "\n", test_num);
		return 0;
	} else {
		fprintf(stdout, COL_RED_BOLD ">>> TEST %d: FAILED <<<" COL_RESET "\n", test_num);
		return -1;
	}
}

//
// Parse a selection string like "1,3-5,10" into a boolean array.
// If selection is NULL, all tests are selected.
//
static void parse_selection(const char *selection, int *tests_to_run, int num_tests)
{
	char	*selection_copy;
	char	*token;

	if (selection == NULL) {
		for (int i = 0; i < num_tests; i++) {
			tests_to_run[i] = 1;
		}
		return;
	}

	for (int i = 0; i < num_tests; i++) {
		tests_to_run[i] = 0;
	}

	selection_copy = strdup(selection);
	if (selection_copy == NULL) {
		return;
	}

	token = strtok(selection_copy, ",");
	while (token != NULL) {
		char	*dash = strchr(token, '-');

		if (dash) {
			// Range: "3-5"
			int start = atoi(token);
			int end = atoi(dash + 1);

			for (int j = start; j <= end && j <= num_tests; j++) {
				if (j >= 1) {
					tests_to_run[j - 1] = 1;
				}
			}
		} else {
			// Single test: "3"
			int test_num = atoi(token);

			if (test_num >= 1 && test_num <= num_tests) {
				tests_to_run[test_num - 1] = 1;
			}
		}
		token = strtok(NULL, ",");
	}
	free(selection_copy);
}

int test_run_suite(const char *suite_name,
				   struct toma_test_entry *tests,
				   int num_tests,
				   void *ctx,
				   const char *selection,
				   int quiet_mode)
{
	int		tests_to_run[MAX_TESTS] = {0};
	int		test_results[MAX_TESTS] = {0};		// 1=passed, -1=failed, 0=skipped
	int		tests_run = 0;
	int		tests_passed = 0;
	int		tests_failed = 0;

	if (num_tests > MAX_TESTS) {
		fprintf(stderr, COL_RED_BOLD "Too many tests (%d > %d)" COL_RESET "\n", num_tests, MAX_TESTS);
		return 1;
	}

	// Header
	fprintf(stdout, "\n");
	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");
	fprintf(stdout, COL_WHITE_BOLD "  Test Suite: %s  (%d tests)" COL_RESET "\n", suite_name, num_tests);
	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");

	parse_selection(selection, tests_to_run, num_tests);

	// Run selected tests
	for (int i = 0; i < num_tests; i++) {
		if (tests_to_run[i]) {
			int		test_num = i + 1;
			int		rv;

			test_print_banner(test_num, tests[i].name, tests[i].description, quiet_mode);
			rv = tests[i].func(ctx);
			tests_run++;

			if (test_print_result(test_num, rv) == 0) {
				tests_passed++;
				test_results[i] = 1;
			} else {
				tests_failed++;
				test_results[i] = -1;
			}
		}
	}

	// Summary
	fprintf(stdout, "\n");
	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");
	if (tests_failed == 0) {
		fprintf(stdout, COL_GREEN "ALL TESTS PASSED (%d/%d)" COL_RESET "\n", tests_passed, tests_run);
	} else {
		fprintf(stdout, COL_RED_BOLD "SOME TESTS FAILED: %d passed, %d failed (%d total)" COL_RESET "\n",
				tests_passed, tests_failed, tests_run);
	}
	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");

	// Per-test results table
	fprintf(stdout, "\nTest Results (%d tests):\n", num_tests);
	for (int i = 0; i < num_tests; i++) {
		const char	*status;
		const char	*color;

		if (test_results[i] == 1) {
			status = "P";
			color = COL_GREEN;
		} else if (test_results[i] == -1) {
			status = "F";
			color = COL_RED_BOLD;
		} else {
			status = "s";
			color = COL_YELLOW;
		}
		fprintf(stdout, "  %s[%s] Test %2d:%s %s\n",
				color, status, i + 1, COL_RESET, tests[i].name);
	}
	fprintf(stdout, "\n");

	return (tests_failed > 0) ? 1 : 0;
}
