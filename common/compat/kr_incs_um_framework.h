/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_UM_FRAMEWORK_H
#define KR_INCS_UM_FRAMEWORK_H
	#include <stdio.h>
	#include <stdlib.h>
	#include <stddef.h>
	#include <string.h>
	#include <errno.h>
	#include <stdio.h>
	#include <limits.h>
	#include <linux/kernel.h>
	#include <linux/const.h>
	#include <linux/types.h>
	#include "common/um_incs.h"		// In um repo app/...
	#include "compat/kr_incs_asserts.h"
	#include "compat/kr_incs_types.h"
	#include "compat/kr_incs_time.h"
	#include "compat/kr_incs_malloc.h"
	#include "compat/kr_incs_percpu.h"
	#include "compat/kr_incs_locks.h"
	// Note: strlcpy defined twice. DPDK does not check for #ifdef https://doc.dpdk.org/api/rte__string__fns_8h_source.html
		// So we include #include "framework/utils.h" becore #include "common/nvmeib_str.h"
#endif
