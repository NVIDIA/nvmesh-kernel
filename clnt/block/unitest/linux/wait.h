/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/wait.h - stub for userspace unit-test builds.
 *
 * wait_queue_head_t and the wait_event / wake_up helpers are provided by
 * kr_incs.h via its userspace emulation.  This file just satisfies the
 * #include directive in shared headers.
 */

#ifndef _LINUX_WAIT_SIM_H
#define _LINUX_WAIT_SIM_H

/* Definitions provided by kr_incs.h. */

#endif
