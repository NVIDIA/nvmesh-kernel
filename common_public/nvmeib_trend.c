/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_trend.h"

int nvmeib_trend_foreach(struct nvmeib_trend *arr, nvmeib_trend_call_fn fn, void *arg)  {
    int ret, i;

	if(!arr) {
		WARN_ON_ONCE(1);
		ret = 1;
		goto out;
	}

	if (NVMEIB_TREND_IS_EMPTY(*arr)) {
		return 0;
	}

	/* iterate until head and use Euclidean modulo */
	for(i=NVMEIB_TREND_TAIL_IDX(*arr); i != NVMEIB_TREND_HEAD_IDX(*arr); i = EUCMOD(i+1, NVMEIB_TREND_MAX(*arr))) {
        fn(&arr->data[i], arg);
    }
	/* run on head */
	fn(&NVMEIB_TREND_HEAD(*arr), arg);
	ret = 0;

out:
	return ret;
}
EXPORT_SYMBOL(nvmeib_trend_foreach);