/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "xkr_incs.h"
#include "xkr_version.h"
#include "requests.h"
#include "xtrace.h"

#define REQUEST_FOR_SEP_S(x, y) [x] = NULL,
#define REQUEST_FOR_SEP_E(x) [x] = NULL,
#define REQUEST_FOR(x) [x] = #x,

static const char * const request_strs[] = {
#include "requests.hxx"
};

const char * rpcrdma_request_to_str(int i)
{
	if (unlikely((unsigned)i >= ARRAY_SIZE(request_strs))) {
		xetrace("Invalid request code : %d (last request code is %d\n",
			i, rpcrdma_last_request);
		return "INVALID EVENT CODE";
	}
	if (unlikely(request_strs[i] == NULL))
		return "UNKNOWN";
	return request_strs[i];
}
EXPORT_SYMBOL(rpcrdma_request_to_str);


