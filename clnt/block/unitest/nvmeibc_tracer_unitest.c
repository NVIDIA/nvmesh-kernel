/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_common_all.h"

#include <execinfo.h>

/**
 * This file contains all possible use cases of binary tracer.
 * First, this can be used as a reference for binary tracer capabilities.
 * Second, it can be used as a regression test for binary tracer.
 * Previous runs of this unitest are saved in run_tracer_unitest.expected
 * Using run_tracer_unitest.py one can make sure the results of this test
 * are in accordance with previous runs, and if changes exist, they are as
 * expected.
 */

struct __stack_trace_and_len {
	int len;
	void **st;
};

void some_func(void) {
	void *_st[10];
	struct __stack_trace_and_len st;
	_NW(unitest_some_func, "I was called from a function named @FUNCTION", __func__);
	st.st = _st;
	st.len = backtrace(st.st, 10);
	_NW(unitest_stack_trace, "Here is a stack trace @STACK_TRACE", &st);
}

#ifdef USER_SPACE_TRACING
	#define nvmeibc_dump_ephemeral() nvmeib_dump_ephemeral(nvmeibc_trace_long)
#else
	#define nvmeibc_dump_ephemeral()
#endif
int nvmeibc_run_tracer_unitest(void) {

#ifndef NVMEIBC_TRACER_UNITEST_ALTERNATIVE_COMPILATION
	unsigned char ipv6[16] = {0};
	unsigned char uuid[16] = {
		'\x12', '4', 'V', 'x', '\x12', '4', 'V', 'x',
		'\x12', '4', 'V', 'x', '\x12', '4', 'V', 'x'}; // Expect UUID('{12345678-1234-5678-1234-567812345678}')
	unsigned char bmp[16] = {0};
	unsigned char hex[4] = {0};

	// Generate some data
	bmp[0] = 14;
	bmp[4] = 14;
	hex[0] = 0xff;

	// ==== Using high level macros ====

	// No arguments
	_NT(unitest_trace_hello_world, "Hello\tWorld!");
	// Some arguments
	errno = EAGAIN; _NE_dmesg(unitest_trace_1_plus_1, "My name is @NAME and I am @VAL_INT years old and look, btw here is an errno: @AUTO_ERRNO", "John", 29);
	// IPV6 test
	_NT(unitest_trace_ipv6, "My name is @NAME and I am using ipv6 [@GENERIC_IPV6]", "Samantha", ipv6);
	// UUID test
	_NT(unitest_trace_uuid, "My name is @NAME and my uuid is [@CLIENT_UUID]", "R2-O2", uuid);
	_NT(unitest_trace_uuid_le, "My name is @NAME and my uuid is [@UUID_LE]", "R2-O2", uuid);
	// Bitmap
	_NT(unitest_trace_bmp, "Check out my bmp: < @TEST_BITMAP128 >", bmp);
	// Hex
	_NT(unitest_trace_hex, "Check out my hex: < @TEST_HEX32 >", hex);
	// Custom formatters
	_NT(unitest_trace_db, "Custom formatters DBITS < @DBITS > LOCKID < @LOCKID >", 0x46f, 0xa0000083); // Expect (5c,3c) (all=0xa0000083,idx=3,lock_id=0x8,read)
	// Fixed length string
	_NT(unitest_str_n, "These are the first 10 characters of this string: '@TEST_STRING_10'", "These are the first 10 characters of this string");

	{
		// Illegal operation - generate too long string
		char too_big[4088];
		int i;
		for (i = 0; i < (int)sizeof(too_big) - 1; ++i) too_big[i] = 'a';
		too_big[i] = '\0';
		_NT(unitest_too_big, "This message will never be written as it is too big: @NAME", too_big);
	}

	// ==== Using low level macros ====

	// Bitfields
	NVMEIB_LOG_LONGTERM("2bits: @BITFIELD_2, 11bits: @BITFIELD_11, 24bits: @BITFIELD_24", /*Defalt*/, /*Deault*/, unitest_trace_bitfields,
						0x12345678, 0x12345678, 0x12345678);
	// Mix bitfields / Integers
	NVMEIB_LOG_LONGTERM("2bits: @BITFIELD_2, long int: @VAL, 24bits: @BITFIELD_24", /*Defalt*/, /*Deault*/, unitest_trace_bitfields_int_mix,
						0x87654321, 0x87654321LL, 0x87654321);
	// Test merging multiple channels
	NVMEIB_LOG_GOODPATH("This messages goes to a different channel, but still shall be visible", /*Defalt*/, /*Deault*/, unitest_trace_different_channel);
	// Test merging multiple channels - eternal
	NVMEIB_LOG_ETERNAL("This is eternal channel", /*Defalt*/, /*Deault*/, unitest_trace_eternal);

	// ==== Ephemeral ====
	// Only prints *before* ephemeral dump will be visible
	nvmeibc_dump_ephemeral(); // Dump nothing
	NVMEIB_LOG_EPHEMERAL("This will be visible in the ephemeral channel", /*Defalt*/, /*Deault*/, unitest_eph_visible1);
	nvmeibc_dump_ephemeral(); // Dump the first buffer
	NVMEIB_LOG_EPHEMERAL("This will be visible in the ephemeral channel, the last ephemeral message", /*Defalt*/, /*Deault*/, unitest_eph_visible2);
	nvmeibc_dump_ephemeral(); // Dump the second buffer
	NVMEIB_LOG_EPHEMERAL("This will NOT be visible in the ephemeral channel, IF YOU SEE THIS IT IS A BUG", /*Defalt*/, /*Deault*/, unitest_eph_invisible1);
	// The rest is not dumped at all

	// Fill some buffers with prints
	{
		long long i;
		for (i = 0; i < 512; ++i) {
			_NT(unitest_trace_fill_some_buffers, "Message: '@BUF_STR', Number: @VAL", "Just filling some buffers", i);
		}
	}

	_NT(sticky1, "I am a sticky message @STICKY_TEST_STR", "tiger");
	_NT(sticky2, "I am a sticky message @STICKY_TEST_STR", "wolf");
	_NT(sticky3, "My name is @NAME and I choose sticky code @STICKY_TEST_STR", "John", "tiger");
	_NT(sticky4, "My name is @NAME and I choose sticky code @STICKY_TEST_STR", "Brian", "wolf");
	_NT(sticky5, "I am a sticky message @STICKY_TEST_STR", "tiger");
	_NT(sticky6, "I am a sticky message @STICKY_TEST_STR", "wolf");
	_NT(sticky7, "Sticky breaker @STICKY_TEST_STR", "tiger");
	_NT(sticky8, "This message is after the sticky breaker @STICKY_TEST_STR", "tiger");

	_NT(unitest_composite, "Composite test @TEST_COMPOSITE", 10, "ten");

	_NT(unitest_composite_2, "My name is @NAME, and behold this: @TEST_COMPOSITE", "Alibaba", 10, "ten");

	_NT(__AUTOID__, "This message has an auto ID");

	_NE_dmesg(unitest_message_with_escape, "This message contains an \"Escape sequence\"");

	_NE_dmesg(unitest_func_fmt, "Here is function format test: '@__BUILTIN_RETURN_ADDRESS_FUNC', should be equal to 'some_func'", some_func);

	_NT(unitest_at_symbol, "Testing \\@ symbol escaping: @NAME\\@IAMNOTATAG", "IAMTAG");

	/* Test passing NULL arguments */
	_NT(unitest_null_parameter, "I have no name @NAME, I have not function @__BUILTIN_RETURN_ADDRESS_FUNC, I have no UUID @CLIENT_UUID", NULL, NULL, NULL);

	/* Testing < > = operators */
	_NT(unitest_val_int_1, "Print val=@VAL_INT", 1);
	_NT(unitest_val_int_2, "Print val=@VAL_INT", 2);
	_NT(unitest_val_int_3, "Print val=@VAL_INT", 3);
	_NT(unitest_val_int_4, "Print uuid4b_be=@UUID_4B_BE uuid4b_le=@UUID_4B_LE", uuid, uuid);

	NVMEIB_LOG_LONGTERM("I have no prefix", _T & no_prefix, /*Deault*/, unitest_no_prefix);

	some_func();
#else /* defined NVMEIBC_TRACER_UNITEST_ALTERNATIVE_COMPILATION*/
	/*This compilation mode is used to generate an alternative version of the dictionary,
	  needed to test how pager handles multiple dictionaries for the same channel. */
	_NW_dmesg(unitest_msg_from_parallel_universe, "I am @NAME, this is a message from the parrallel universe.", "Iruy");
#endif /*NVMEIBC_TRACER_UNITEST_ALTERNATIVE_COMPILATION*/
	return 0;
}
