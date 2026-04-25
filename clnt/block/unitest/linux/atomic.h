/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/atomic.h - stub for userspace unit-test builds.
 *
 * atomic_t, atomic64_t, and the atomic_inc / atomic_read / cmpxchg / xchg
 * helpers are provided by kr_incs.h via its userspace emulation.  This file
 * just satisfies the #include directive in shared headers.
 */

#ifndef _LINUX_ATOMIC_SIM_H
#define _LINUX_ATOMIC_SIM_H

/* Definitions provided by kr_incs.h. */

#endif
