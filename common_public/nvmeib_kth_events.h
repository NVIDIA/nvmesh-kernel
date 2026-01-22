/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_KTH_EVENTS_H
#define NVMEIB_KTH_EVENTS_H

#include "kth/nvmeib_public_kth.h"

#undef KTH_EVENT_SEP_S
#undef KTH_EVENT_SEP_E
#undef KTH_EVENT_SEP

#define KTH_EVENT_SEP_S(x, y) x = y,
#define KTH_EVENT_SEP_E(x) x,
#define KTH_EVENT(x) x,

enum nvmein_kth_event_id {
#include "nvmeib_kth_events.hxx"
};
#undef KTH_EVENT_SEP_S
#undef KTH_EVENT_SEP_E
#undef KTH_EVENT
 
const char * nvmeib_kth_event_to_str(int i);

#endif 
