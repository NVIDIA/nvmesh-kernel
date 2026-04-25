/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/mutex.h - stub for userspace unit-test builds.
 *
 * struct mutex and mutex_init/lock/unlock/trylock/destroy are provided by
 * kr_incs.h via pthread_mutex emulation.  This file just satisfies the
 * #include directive in shared headers.
 */

#ifndef _LINUX_MUTEX_SIM_H
#define _LINUX_MUTEX_SIM_H

/* Definitions provided by kr_incs.h. */

#endif
