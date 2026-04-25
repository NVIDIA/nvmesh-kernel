/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/sched.h - stub for userspace unit-test builds.
 *
 * struct task_struct, schedule(), msleep(), TASK_* states, and related
 * scheduler primitives are provided by kr_incs.h via its userspace
 * emulation.  This file just satisfies the #include directive in shared
 * headers.
 */

#ifndef _LINUX_SCHED_SIM_H
#define _LINUX_SCHED_SIM_H

/* Definitions provided by kr_incs.h. */

#endif
