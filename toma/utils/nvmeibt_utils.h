/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_UTILS_H
#define NVMEIBT_UTILS_H

#include <stdint.h>
#include <stddef.h>
#include "toma/nvmeibt_debug.h"
#include "common/nvmeib_str.h"
#include "common/nvmeib_math.h"

static inline size_t nvmeibt_strlcpy(char *dst, const char *src, size_t max_len_unsigned) {
	return nvmeib_strlcpy(dst, src, max_len_unsigned);
}
static inline size_t nvmeibt_strlcat(char *dst, const char *src, size_t max_len) {
	size_t _len = strnlen(dst, max_len);
	if (_len == max_len)
		  return _len + strnlen(src, max_len);
	return _len + nvmeib_strlcpy(dst + _len, src, max_len - _len);
}

#include "common/compat/kr_incs_crc32.h"		/*crc32() */
#define crc32_seedless(buf, size) (crc32(~0U, (const void *)buf, size) ^ (~0U))

#endif // #ifndef NVMEIBT_UTILS_H

