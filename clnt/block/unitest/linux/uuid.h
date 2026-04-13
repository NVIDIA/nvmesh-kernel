/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/uuid.h — minimal stub for macOS builds.
 *
 * On Linux, linux/uuid.h provides uuid_t, guid_t and related helpers.
 * In user-space simulator builds, nvmeib_uuid_be.h provides its own uuid_t
 * via uuid/uuid.h (macOS) or its inline reimplementation.  An empty stub
 * here satisfies the #include without conflicts.
 */

#ifndef _LINUX_UUID_H
#define _LINUX_UUID_H

/* UUID types are provided by nvmeib_uuid_be.h's non-kernel path. */

#endif /* _LINUX_UUID_H */
