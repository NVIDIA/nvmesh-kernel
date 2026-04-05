/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "safe_casting_tests.h"
#include "common/safe_casting.h"


struct __ut_raid_base{int type;};
struct __ut_raid_ec{
	struct __ut_raid_base base;
	int n_parity;
	uint64_t magic;
};


void test_safe_casting(void)
{
	struct __ut_raid_ec ec = {
		.base = { 3 },
		.n_parity = 2,
		.magic = MAGIC_CAST_VALUE
	};

	__auto_type base_ptr = &ec.base;
	__auto_type derived_ptr = derived_cast(struct __ut_raid_ec, base_ptr);
	__auto_type derived_ptr_via_magic = magic_derived_cast(struct __ut_raid_ec, base_ptr);

	BUG_ON(derived_ptr != &ec);
	BUG_ON(derived_ptr_via_magic != &ec);
}
