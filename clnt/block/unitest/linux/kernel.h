/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/kernel.h - stub for userspace unit-test builds.
 *
 * Common helpers (container_of, min / max, ARRAY_SIZE, BUG / BUG_ON,
 * READ_ONCE / WRITE_ONCE, printk, etc.) are provided by kr_incs.h via its
 * userspace emulation.  This file just satisfies the #include directive in
 * shared headers.
 */

#ifndef _LINUX_KERNEL_SIM_H
#define _LINUX_KERNEL_SIM_H

/* Definitions provided by kr_incs.h. */

#endif
