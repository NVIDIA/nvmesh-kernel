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

#endif /* _LINUX_TYPES_H */
