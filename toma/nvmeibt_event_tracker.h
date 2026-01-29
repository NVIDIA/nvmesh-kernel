/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_EVENT_TRACKER_H
#define NVMEIBT_EVENT_TRACKER_H

#include <sys/time.h>
#include "common/nvmeib_jdr.h"

#define NVMEIBT_EVENT_TRACKER_DEFAULT_SIZE 10
typedef const char * (*nvmeibt_event_to_str_fn)(int event_id);

struct nvmeibt_event_entry;

struct nvmeibt_event_tracker {
	struct nvmeibt_event_entry *events;
	int max_events;
	int head;
	nvmeibt_event_to_str_fn event_to_str;
};

struct nvmeibt_event_tracker *nvmeibt_event_tracker_init(struct nvmeibt_event_tracker *tracker, 
	int max_events, nvmeibt_event_to_str_fn event_to_str);

void nvmeibt_event_tracker_free(struct nvmeibt_event_tracker *tracker);

void nvmeibt_event_tracker_add(struct nvmeibt_event_tracker *tracker, int event);

void nvmeibt_event_tracker_clear(struct nvmeibt_event_tracker *tracker);

void nvmeibt_event_tracker_print_jdr(struct nvmeibt_event_tracker *tracker,
	struct jdr *jdr);

#endif /* NVMEIBT_EVENT_TRACKER_H */
