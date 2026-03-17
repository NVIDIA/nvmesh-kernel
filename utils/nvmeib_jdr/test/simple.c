/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_jdr.h"
#include <assert.h>

int main(void) {
	struct charvec buffer;
	char buf[96];
	struct jdr jdr;

	buffer = (struct charvec){.base = buf, .len = sizeof(buf)};
	jdr = jdr_make(buffer);
	{
		jdr_object_scope(&jdr, NULL);
		jdr_write_var(&jdr, answer, 42);
	}
	jdr_finalize(&jdr);
	assert(buffer.len > 0);
	return 0;
}