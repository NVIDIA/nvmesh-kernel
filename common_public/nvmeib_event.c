/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "nvmeib_event.h"

#define NVMEIB_EVENT(x) [x] = #x,

static const char * const nvmeib_event_strs[] = {
#include "nvmeib_event.hxx"
};

const char * nvmeib_event_type_to_string(enum nvmeib_event_type type)
{
	const char *ret;
	if (type >= ARRAY_SIZE(nvmeib_event_strs))
		return "INVALID_EVENT";
	ret = nvmeib_event_strs[type];
	if (!ret)
		return "UNKNOWN_EVENT";
	return ret;
}
EXPORT_SYMBOL(nvmeib_event_type_to_string);

#undef NVMEIB_EVENT

