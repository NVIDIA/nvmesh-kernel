/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/list.h - stub for userspace unit-test builds.
 *
 * Production code includes <linux/list.h>; the unitest build picks up this
 * file first via -I$(SSDA)/clnt/block/unitest. struct list_head and the
 * list_* helpers are provided by common/compat/kr_incs_*.h, pulled in via
 * kr_incs.h before any header that needs them.
 */

#ifndef _LINUX_LIST_SIM_H
#define _LINUX_LIST_SIM_H

/* Definitions provided by kr_incs.h. */

#endif
