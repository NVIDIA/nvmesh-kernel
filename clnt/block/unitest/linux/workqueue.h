/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/workqueue.h - stub for userspace unit-test builds.
 *
 * struct work_struct, struct delayed_work, INIT_WORK, schedule_work,
 * schedule_delayed_work, flush_workqueue, cancel_work_sync, system_wq, and
 * the rest of the workqueue API are provided by kr_incs.h via its userspace
 * emulation.  This file just satisfies the #include directive in shared
 * headers.
 */

#ifndef _LINUX_WORKQUEUE_SIM_H
#define _LINUX_WORKQUEUE_SIM_H

/* Definitions provided by kr_incs.h. */

#endif
