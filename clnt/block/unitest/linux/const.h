/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/const.h — minimal stub for macOS builds.
 *
 * On Linux, linux/const.h provides _AC(), _UL(), _ULL(), GENMASK() and
 * related integer-literal helpers.  None of those macros are referenced in
 * user-space (non-kernel) paths of the NVMesh simulator, so an empty stub
 * is sufficient.
 */

#ifndef _LINUX_CONST_H
#define _LINUX_CONST_H

/* Nothing needed for user-space simulator builds. */

#endif /* _LINUX_CONST_H */
