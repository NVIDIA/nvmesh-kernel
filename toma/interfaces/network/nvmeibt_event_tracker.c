/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_global.h"
#include "nvmeibt_event_tracker.h"
#include "nvmeibt_debug.h"

struct nvmeibt_event_entry {
	struct timespec timestamp;
	int event;
};

struct nvmeibt_event_tracker *nvmeibt_event_tracker_init(struct nvmeibt_event_tracker *tracker, 
	int max_events, nvmeibt_event_to_str_fn event_to_str)
{
	struct nvmeibt_event_tracker *rv = NULL;
	NFIN;

	if (!tracker) {
		N_ETf(event_tracker_init_e1, "tracker is NULL");
		goto out;
	}

	memset(tracker, 0, sizeof(*tracker));
	tracker->max_events = max_events > 0 ? max_events : NVMEIBT_EVENT_TRACKER_DEFAULT_SIZE;
	tracker->event_to_str = event_to_str;
	if (!(tracker->events = NNVMEIBT_TOMA_CALLOC(event_tracker_init_t1,
		tracker->max_events, sizeof(struct nvmeibt_event_entry)))) {
		N_ETf(event_tracker_init_e2, "Failed to allocate event array");
		tracker->max_events = 0;
		goto out;
	}
	rv = tracker;
	tracker->head = 0;
	N_Df(event_tracker_init_d1, "Event tracker initialized with max_events @INT", tracker->max_events);

out:
	NFOUT;
	return rv;
}

void nvmeibt_event_tracker_free(struct nvmeibt_event_tracker *tracker)
{
	NFIN;
	if (!tracker) {
		N_ETf(event_tracker_free_e1, "tracker is NULL");
		goto out;
	}
	if (tracker->events) {
		NNVMEIBT_TOMA_FREE(event_tracker_free_t1, tracker->events);
		tracker->events = NULL;
	}
	tracker->max_events = 0;
	tracker->head = 0;
	tracker->event_to_str = NULL;
	N_Df(event_tracker_free_d1, "Event tracker freed");

out:
	NFOUT;
}

void nvmeibt_event_tracker_add(struct nvmeibt_event_tracker *tracker, int event)
{
	struct nvmeibt_event_entry *entry;
	int index;

	if (!tracker || !tracker->events) {
		N_ETf(event_tracker_add_e1, "tracker or events is NULL");
		return;
	}

	index = tracker->head;
	entry = &tracker->events[index];

	getnstimeofday_real(&entry->timestamp);
	entry->event = event;

	tracker->head = (tracker->head + 1) % tracker->max_events;

	if (tracker->event_to_str) {
		N_Df(event_tracker_add_d1, "Event added: @STR (@INT)",
			tracker->event_to_str(event), event);
	} else {
		N_Df(event_tracker_add_d2, "Event added: @INT", event);
	}
}

void nvmeibt_event_tracker_clear(struct nvmeibt_event_tracker *tracker)
{
	NFIN;
	if (!tracker) {
		N_ETf(event_tracker_clear_e1, "tracker is NULL");
		goto out;
	}

	tracker->head = 0;
	if (tracker->events) {
		memset(tracker->events, 0,
			tracker->max_events * sizeof(struct nvmeibt_event_entry));
	}
	N_Df(event_tracker_clear_d1, "Event tracker cleared");

out:
	NFOUT;
}

void nvmeibt_event_tracker_print_jdr(struct nvmeibt_event_tracker *tracker,
	struct jdr *jdr)
{
	int i, idx;
	struct nvmeibt_event_entry *entry;
	char time_buf[64];
	struct tm tm_info;
	time_t sec;
	const char *event_str;

	NFIN;
	if (!tracker || !tracker->events) {
		N_ETf(event_tracker_print_jdr_e1, "tracker or events is NULL");
		goto out;
	}

	if (!jdr) {
		N_ETf(event_tracker_print_jdr_e2, "jdr is NULL");
		goto out;
	}

	{
		jdr_object_scope(jdr, "event_tracker");
		jdr_write_var(jdr, max_events, tracker->max_events);
		{ /* event_tracker scope */
			jdr_array_scope(jdr, "events");
			for (i = 0, idx = tracker->head - 1; i < tracker->max_events; i++, idx = (idx - 1 + tracker->max_events) % tracker->max_events) {
				entry = &tracker->events[idx];
				
				if (entry->timestamp.tv_sec == 0 && entry->timestamp.tv_nsec == 0)
					continue;

				sec = entry->timestamp.tv_sec;
				localtime_r(&sec, &tm_info);
				strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
				snprintf(time_buf + strlen(time_buf), sizeof(time_buf) - strlen(time_buf),
					".%09ld", entry->timestamp.tv_nsec);

				event_str = tracker->event_to_str ? tracker->event_to_str(entry->event) : "unknown";
				{
					jdr_object_scope(jdr, NULL);
					jdr_write_var(jdr, timestamp, (char const *)time_buf);
					jdr_write_var(jdr, event_type, (char const *)event_str);
				}
			}
		} /* event_tracker scope */
	}

out:
	NFOUT;
}
