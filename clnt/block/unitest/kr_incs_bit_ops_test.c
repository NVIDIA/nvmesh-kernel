/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#include "kr_incs_bit_ops_test.h"

static void __ut_bit_primitives(void)
{
	DECLARE_BITMAP(map, 130) = { 0 };

	BUG_ON(!bitmap_empty(map, 130));
	BUG_ON(test_bit(0, map) != 0);

	set_bit(0, map);
	set_bit(63, map);
	set_bit(64, map);
	set_bit(129, map);

	BUG_ON(!test_bit(0, map));
	BUG_ON(!test_bit(63, map));
	BUG_ON(!test_bit(64, map));
	BUG_ON(!test_bit(129, map));

	BUG_ON(test_and_set_bit(64, map) != 1);
	BUG_ON(test_and_set_bit(5, map) != 0);
	BUG_ON(!test_bit(5, map));

	BUG_ON(test_and_clear_bit(5, map) != 1);
	BUG_ON(test_and_clear_bit(5, map) != 0);
	BUG_ON(test_bit(5, map) != 0);

	clear_bit(63, map);
	BUG_ON(test_bit(63, map) != 0);
}

static void __ut_find_next_bit(void)
{
	DECLARE_BITMAP(map, 130) = { 0 };

	set_bit(0, map);
	set_bit(5, map);
	set_bit(63, map);
	set_bit(64, map);
	set_bit(70, map);
	set_bit(129, map);

	BUG_ON(_find_next_bit(map, 130, 0, 0UL) != 0);
	BUG_ON(_find_next_bit(map, 130, 1, 0UL) != 5);
	BUG_ON(find_next_bit(map, 130, 5) != 5);
	BUG_ON(find_next_bit(map, 130, 6) != 63);
	BUG_ON(find_next_bit(map, 130, 64) != 64);
	BUG_ON(find_next_bit(map, 130, 65) != 70);
	BUG_ON(find_next_bit(map, 130, 71) != 129);
	BUG_ON(find_next_bit(map, 130, 130) != 130);

	BUG_ON(find_first_bit(map, 130) != 0);
	BUG_ON(find_last_bit(map, 130) != 129);
}

static void __ut_find_zero_bits(void)
{
	DECLARE_BITMAP(map, 130) = { 0 };
	unsigned long word;

	bitmap_set(map, 0, 130);
	clear_bit(0, map);
	clear_bit(66, map);
	clear_bit(129, map);

	BUG_ON(_find_next_bit(map, 130, 0, ~0UL) != 0);
	BUG_ON(find_first_zero_bit(map, 130) != 0);
	BUG_ON(find_next_zero_bit(map, 130, 1) != 66);
	BUG_ON(_find_next_bit(map, 130, 67, ~0UL) != 129);
	BUG_ON(find_next_zero_bit(map, 130, 130) != 130);

	word = BITMAP_LAST_WORD_MASK(10);
	BUG_ON(find_first_zero_bit(&word, 10) != 10);
}

static void __ut_find_helpers_clip_to_nbits(void)
{
	DECLARE_BITMAP(last_map, 70) = { 0 };
	unsigned long word = BIT(63);

	BUG_ON(find_first_bit(&word, 10) != 10);

	last_map[1] = ~0UL;
	BUG_ON(find_last_bit(last_map, 70) != 69);
}

static void __ut_for_each_bit_macros(void)
{
	DECLARE_BITMAP(set_map, 130) = { 0 };
	const unsigned long expected_set[] = { 2, 65, 66, 129 };
	const unsigned long expected_set_from[] = { 65, 66, 129 };
	const unsigned long expected_clear[] = { 0, 2, 3, 5 };
	const unsigned long expected_clear_from[] = { 3, 5 };
	unsigned long clear_map = BIT(1) | BIT(4);
	unsigned long bit_index;
	size_t idx;

	set_bit(2, set_map);
	set_bit(65, set_map);
	set_bit(66, set_map);
	set_bit(129, set_map);

	idx = 0;
	for_each_set_bit(bit_index, set_map, 130) {
		BUG_ON(idx >= ARRAY_SIZE(expected_set));
		BUG_ON(bit_index != expected_set[idx]);
		idx++;
	}
	BUG_ON(idx != ARRAY_SIZE(expected_set));

	idx = 0;
	bit_index = 65;
	for_each_set_bit_from(bit_index, set_map, 130) {
		BUG_ON(idx >= ARRAY_SIZE(expected_set_from));
		BUG_ON(bit_index != expected_set_from[idx]);
		idx++;
	}
	BUG_ON(idx != ARRAY_SIZE(expected_set_from));

	idx = 0;
	for_each_clear_bit(bit_index, &clear_map, 6) {
		BUG_ON(idx >= ARRAY_SIZE(expected_clear));
		BUG_ON(bit_index != expected_clear[idx]);
		idx++;
	}
	BUG_ON(idx != ARRAY_SIZE(expected_clear));

	idx = 0;
	bit_index = 3;
	for_each_clear_bit_from(bit_index, &clear_map, 6) {
		BUG_ON(idx >= ARRAY_SIZE(expected_clear_from));
		BUG_ON(bit_index != expected_clear_from[idx]);
		idx++;
	}
	BUG_ON(idx != ARRAY_SIZE(expected_clear_from));
}

static void __ut_bitmap_set_and_masks(void)
{
	unsigned long bit_index;
	DECLARE_BITMAP(single_bit_map, 130) = { 0 };
	DECLARE_BITMAP(byte_aligned_map, 130) = { 0 };
	DECLARE_BITMAP(generic_map, 130) = { 0 };

	/* Exercise the nbits == 1 fast path. */
	bitmap_set(single_bit_map, 9, 1);
	for (bit_index = 0; bit_index < 130; ++bit_index) {
		const bool expected = bit_index == 9;

		BUG_ON(!!test_bit(bit_index, single_bit_map) != expected);
	}

	/* Exercise the byte-aligned memset() path. */
	bitmap_set(byte_aligned_map, 64, 16);
	for (bit_index = 0; bit_index < 130; ++bit_index) {
		const bool expected = bit_index >= 64 && bit_index < 80;

		BUG_ON(!!test_bit(bit_index, byte_aligned_map) != expected);
	}

	/* Exercise the generic __bitmap_set() path. */
	bitmap_set(generic_map, 60, 10);
	for (bit_index = 0; bit_index < 130; ++bit_index) {
		const bool expected = bit_index >= 60 && bit_index < 70;

		BUG_ON(!!test_bit(bit_index, generic_map) != expected);
	}
}

static void __ut_bitmap_predicates(void)
{
	DECLARE_BITMAP(a, 70) = { 0 };
	DECLARE_BITMAP(b, 70) = { 0 };
	DECLARE_BITMAP(dst, 130) = { 0 };
	DECLARE_BITMAP(src1, 130) = { 0 };
	DECLARE_BITMAP(src2, 130) = { 0 };
	const unsigned long unused_tail_bit = BIT(70 - BITS_PER_LONG);

	set_bit(5, a);
	set_bit(69, a);
	bitmap_copy(b, a, 70);
	b[1] |= unused_tail_bit;
	BUG_ON(!bitmap_equal(a, b, 70));

	set_bit(68, b);
	BUG_ON(bitmap_equal(a, b, 70));

	bitmap_zero(a, 70);
	bitmap_zero(b, 70);
	a[1] |= unused_tail_bit;
	b[1] |= unused_tail_bit;
	BUG_ON(bitmap_intersects(a, b, 70));
	BUG_ON(!bitmap_empty(a, 70));

	set_bit(3, a);
	set_bit(3, b);
	BUG_ON(!bitmap_intersects(a, b, 70));
	BUG_ON(bitmap_empty(a, 70));

	bitmap_zero(src1, 130);
	bitmap_zero(src2, 130);
	set_bit(1, src1);
	set_bit(64, src1);
	set_bit(66, src1);
	set_bit(2, src2);
	set_bit(64, src2);
	set_bit(129, src2);

	bitmap_or(dst, src1, src2, 130);
	BUG_ON(bitmap_weight(dst, 130) != 5);
	BUG_ON(!test_bit(129, dst));

	bitmap_and(dst, src1, src2, 130);
	BUG_ON(bitmap_weight(dst, 130) != 1);
	BUG_ON(!test_bit(64, dst));

	bitmap_copy(dst, src2, 130);
	BUG_ON(!bitmap_equal(dst, src2, 130));

	bitmap_zero(dst, 130);
	BUG_ON(!bitmap_empty(dst, 130));
}

static void __ut_bitmap_parse_and_math_helpers(void)
{
	unsigned long mask = 0;

	BUG_ON(bitmap_parse("f", 1, &mask, 8) != 0);
	BUG_ON(mask != 0xfUL);

	mask = ~0UL;
	BUG_ON(bitmap_parse("g", 1, &mask, 8) != -EINVAL);
	BUG_ON(bitmap_parse("100", 3, &mask, 8) != -EOVERFLOW);

	BUG_ON(roundup_pow_of_two(1) != 1);
	BUG_ON(roundup_pow_of_two(3) != 4);
	BUG_ON(roundup_pow_of_two(65) != 128);

	BUG_ON(get_order(PAGE_SIZE - 1) != 0);
	BUG_ON(get_order(PAGE_SIZE) != 0);
	BUG_ON(get_order(PAGE_SIZE + 1) != 1);
	BUG_ON(get_order(2 * PAGE_SIZE) != 1);
	BUG_ON(get_order(2 * PAGE_SIZE + 1) != 2);

	BUG_ON(is_power_of_2(0));
	BUG_ON(!is_power_of_2(1));
	BUG_ON(!is_power_of_2(64));
	BUG_ON(is_power_of_2(96));
}

void kr_incs_bit_ops_tests(void)
{
	__ut_bit_primitives();
	__ut_find_next_bit();
	__ut_find_zero_bits();
	__ut_find_helpers_clip_to_nbits();
	__ut_for_each_bit_macros();
	__ut_bitmap_set_and_masks();
	__ut_bitmap_predicates();
	__ut_bitmap_parse_and_math_helpers();
}
