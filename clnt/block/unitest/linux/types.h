/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/types.h — minimal stub for macOS builds.
 *
 * On Linux, linux/types.h provides __u8, __be32, __le64 and similar fixed-
 * width kernel types.  In user-space simulator builds, these types are
 * provided by common/compat/kr_incs_types.h which is included later in the
 * include chain (via nvmeib_uuid_be.h's #else branch).  An empty stub here
 * satisfies the #include without redefining anything.
 */

#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H

/* Types are provided by common/compat/kr_incs_types.h in user-space. */

/* Kernel-internal long types used by linux/sysinfo.h and similar headers. */
typedef long			__kernel_long_t;
typedef unsigned long		__kernel_ulong_t;

/* Signed fixed-width kernel types used by linux/stat.h and similar headers. */
typedef signed char		__s8;
typedef signed short		__s16;
typedef signed int		__s32;
typedef signed long long	__s64;

#endif /* _LINUX_TYPES_H */
