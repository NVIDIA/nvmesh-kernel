/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * linux/xarray.h — stub for userspace unit-test builds.
 *
 * Production code includes <linux/xarray.h>; the unitest build uses
 * -I$(SSDA)/clnt/block/unitest so that this file is found first.
 * The real implementation lives in common/compat/kr_incs_xarray.h,
 * which is pulled in via kr_incs.h before this file is parsed.
 */

/* kr_incs_xarray.h is already included via kr_incs.h. */
#ifndef KERNEL_XARRAY_SIM_H
#include "common/compat/kr_incs_xarray.h"
#endif
