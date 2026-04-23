/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * trace_compress_test.c - Tests for LZ4 trace compression in the poller
 * Invoked via: ./nvmeibt_toma trace_compress_test [selection]
 * Validates:
 *   1. parse_log_filename accepts plain, .lz4, and rejects unknown suffixes
 *   2. Kill switch flag default value and setter/getter round-trip
 *   3. LZ4 compressor lifecycle (create/destroy) and round-trip (start/write/stop)
 *
 * Note: end-to-end poller integration (file creation, rotation, old-log deletion)
 * is validated via the main sandbox tests (./run main) where trace pollers are running.
 */

#include <stdio.h>
#include <string.h>
#include "unitest/00_framework/toma_test_framework.h"
#include "interfaces/log/nvmeibt_binary_tracing.h"

// From nvmeib_trace_userspace_poller.c - test the filename parser
extern int parse_log_filename(const char *filename, const char *basename, int *cpu, long *idx);

#define TRACE_COMPRESS_TEST_LIST \
	X(parse_plain_filename,			"Parse plain binlog filename",		"toma.binlog0.5") \
	X(parse_lz4_filename,			"Parse .lz4 binlog filename",		"toma.binlog0.5.lz4") \
	X(parse_reject_bad_suffix,		"Reject unknown suffix",			"toma.binlog0.5.zst") \
	X(compress_flag_default_off,	"Compression default is off",		"Default value check") \
	X(compress_flag_toggle,			"Toggle compression via setter",	"Set/get round-trip") \
	X(compressor_create_destroy,	"Create and destroy LZ4 compressor","Lifecycle test") \
	X(compressor_round_trip,		"Compress and verify output",		"start/write/stop cycle")

/**
 * Test that parse_log_filename correctly parses plain (uncompressed) filenames.
 */
DEFINE_TEST(parse_plain_filename)
{
	int		rv = -1;
	int		cpu = -1;
	long	idx = -1;

	(void)_ctx;
	TEST_ASSERT_TRUE(parse_log_filename("toma.binlog0.5", "toma.binlog", &cpu, &idx));
	TEST_ASSERT_EQ(cpu, 0);
	TEST_ASSERT_EQ(idx, 5);
	rv = 0;
out:
	return rv;
}

/**
 * Test that parse_log_filename correctly parses .lz4 suffixed filenames.
 */
DEFINE_TEST(parse_lz4_filename)
{
	int		rv = -1;
	int		cpu = -1;
	long	idx = -1;

	(void)_ctx;
	TEST_ASSERT_TRUE(parse_log_filename("toma.binlog0.5.lz4", "toma.binlog", &cpu, &idx));
	TEST_ASSERT_EQ(cpu, 0);
	TEST_ASSERT_EQ(idx, 5);
	rv = 0;
out:
	return rv;
}

/**
 * Test that parse_log_filename rejects filenames with unknown suffixes.
 */
DEFINE_TEST(parse_reject_bad_suffix)
{
	int		rv = -1;
	int		cpu = -1;
	long	idx = -1;

	(void)_ctx;
	TEST_ASSERT_TRUE(!parse_log_filename("toma.binlog0.5.zst", "toma.binlog", &cpu, &idx));
	rv = 0;
out:
	return rv;
}

/**
 * Test that trace compression is disabled by default.
 */
DEFINE_TEST(compress_flag_default_off)
{
	int		rv = -1;

	(void)_ctx;
	TEST_ASSERT_EQ(nvmeibt_binary_tracing_get_trace_compress(), 0);
	rv = 0;
out:
	return rv;
}

/**
 * Test the setter/getter round-trip for the compression flag.
 */
DEFINE_TEST(compress_flag_toggle)
{
	int		rv = -1;

	(void)_ctx;

	// Enable
	nvmeibt_binary_tracing_set_trace_compress(1);
	TEST_ASSERT_EQ(nvmeibt_binary_tracing_get_trace_compress(), 1);

	// Disable
	nvmeibt_binary_tracing_set_trace_compress(0);
	TEST_ASSERT_EQ(nvmeibt_binary_tracing_get_trace_compress(), 0);

	rv = 0;
out:
	// Ensure we leave it disabled
	nvmeibt_binary_tracing_set_trace_compress(0);
	return rv;
}

// Include compressor.h with workarounds for basename macro.
#include "trace_compress_lib/compressor.h"
#undef basename

/**
 * Test creating and destroying an LZ4 compressor — verifies linkage and basic lifecycle.
 */
DEFINE_TEST(compressor_create_destroy)
{
	int					rv = -1;
	struct compressor	*c = NULL;

	(void)_ctx;
	c = compressor_create_lz4(4096);
	TEST_ASSERT_NOT_NULL(c);

	// Verify file extension
	TEST_ASSERT_TRUE(strcmp(c->ops.file_ext(), ".lz4") == 0);

	c->ops.destroy(c);
	c = NULL;

	rv = 0;
out:
	if (c) c->ops.destroy(c);
	return rv;
}

/**
 * Test a full compress round-trip: start → write → stop, run two consecutive cycles
 * on the same compressor instance to verify LZ4 state resets correctly across file rotations.
 */
DEFINE_TEST(compressor_round_trip)
{
	int						rv = -1;
	struct compressor		*c = NULL;
	struct compression_result	cr;
	size_t					total_out_1 = 0;
	size_t					total_out_2 = 0;
	char					test_data[4096];
	int						cycle;

	(void)_ctx;

	// Fill test data with a pattern (compressible)
	memset(test_data, 0xAB, sizeof(test_data));

	c = compressor_create_lz4(sizeof(test_data));
	TEST_ASSERT_NOT_NULL(c);

	// Run two consecutive start/write/stop cycles on the same compressor instance
	// to verify state resets correctly (simulates two consecutive log file rotations)
	for (cycle = 0; cycle < 2; cycle++) {
		size_t	total_out = 0;

		// Start (writes LZ4 frame header)
		cr = c->ops.start(c);
		TEST_ASSERT_EQ(cr.error, 0);
		TEST_ASSERT_TRUE(cr.n_iovecs > 0);
		total_out += iovecs_total_size(cr.iovecs, cr.n_iovecs);

		// Write data
		{
			struct iovec iov = { .iov_base = test_data, .iov_len = sizeof(test_data) };
			cr = c->ops.write(c, &iov, 1);
			TEST_ASSERT_EQ(cr.error, 0);
			total_out += iovecs_total_size(cr.iovecs, cr.n_iovecs);
		}

		// Stop (writes LZ4 frame footer)
		cr = c->ops.stop(c);
		TEST_ASSERT_EQ(cr.error, 0);
		total_out += iovecs_total_size(cr.iovecs, cr.n_iovecs);

		// Compressed output should be non-zero but smaller than input
		TEST_ASSERT_TRUE(total_out > 0);
		TEST_ASSERT_TRUE(total_out < sizeof(test_data));

		if (cycle == 0)
			total_out_1 = total_out;
		else
			total_out_2 = total_out;

		fprintf(stdout, "  Cycle %d: compressed %zu -> %zu bytes (%.1fx ratio)\n",
				cycle + 1, sizeof(test_data), total_out, (double)sizeof(test_data) / total_out);
	}

	// Both cycles should produce identical output for identical input
	TEST_ASSERT_EQ(total_out_1, total_out_2);

	c->ops.destroy(c);
	c = NULL;

	rv = 0;
out:
	if (c) c->ops.destroy(c);
	return rv;
}

int trace_compress_test_main(int argc, char *argv[])
{
	const char	*selection = NULL;

	#define X(func, name, desc) {name, desc, test_##func},
	struct toma_test_entry tests[] = { TRACE_COMPRESS_TEST_LIST };
	#undef X

	if (argc > 1) {
		selection = argv[1];
	}

	return test_run_suite("Trace Compression",
						  tests, (int)(sizeof(tests) / sizeof(tests[0])),
						  NULL, selection, 0);
}
