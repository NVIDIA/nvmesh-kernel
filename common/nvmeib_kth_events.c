/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "nvmeib_kth_events.h"
#include "nvmeib.h"
#include "nvmeibp_trace.h"

#define kth_e2s(x) "?"

#define KTH_EVENT_SEP_S(x, y) [x] = NULL,
#define KTH_EVENT_SEP_E(x) [x] = NULL,
#define KTH_EVENT(x) [x] = #x,

static const char * const events_strs[] = {
#include "nvmeib_kth_events.hxx"
};

const char * nvmeib_kth_event_to_str(int i)
{
	if (unlikely((unsigned)i >= ARRAY_SIZE(events_strs))) {
		_NE(error_nvmeib_kth_events_nvmeib_kth_event_to_str, "invalid kth event code : @CODE (last event is @NKE_KTH_SERVER_EVENTS_LAST)", i,
		   nke_kth_server_events_last);
		return "INVALID EVENT CODE";
	}
	if (unlikely(events_strs[i] == NULL))
		return "UNKNOWN";
	return events_strs[i];
}
EXPORT_SYMBOL(nvmeib_kth_event_to_str);


