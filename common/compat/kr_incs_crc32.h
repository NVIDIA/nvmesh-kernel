/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_CRC32_H
#define KR_INCS_CRC32_H

#ifndef __KERNEL__		/* Kernel already has those functions. Define as compatibility for user-space */
	u32 crc32(u32 crc, const void *buf, size_t length);
	#define crc32_le crc32
#endif // !__KERNEL__

#if !KS_HAS_CRC32C			/* Centos 6.0+ kernels dont have u32 crc32c() */
	// include/linux/crc32x.h
	#if KS_CRC32C_USES_SIZE_T
		u32 crc32c(u32 crc, const void *buf, size_t length);
	#else
		u32 crc32c(u32 crc, const void *buf, unsigned int length);
	#endif
#endif // !KS_HAS_CRC32C

#endif // KR_INCS_CRC32_H
