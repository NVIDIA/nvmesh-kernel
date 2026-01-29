/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#include "nvmeibc_common.h"

unsigned nvmeibc_trace_stats_period_sec = 10;
module_param(nvmeibc_trace_stats_period_sec, uint, 0644);
MODULE_PARM_DESC(trace_stats_period_sec, "periodic iostats metrics logging period [sec], 0 means disabled");

void nvmeibc_trace_stats_scheduling_adjust(struct nvmeibc_trace_stats_scheduling *trace_stats, unsigned long now_jiffies)
{
	const unsigned trace_stats_period_sec = nvmeibc_trace_stats_period_sec;

	// Trace stats period module param changed?
	if (trace_stats->trace_stats_period_sec != trace_stats_period_sec) {
		if (!nvmeibc_trace_stats_period_sec) {
			trace_stats->next_trace_jiffies = 0;
		} else {
			// Randomize stats trace phase between volumes/disks to avoid large hiccups
			trace_stats->next_trace_jiffies = now_jiffies + (get_random_u32() % (trace_stats_period_sec * HZ));
		}
		trace_stats->trace_stats_period_sec = trace_stats_period_sec;
	}
}

bool nvmeibc_trace_stats_scheduling_should_trace(struct nvmeibc_trace_stats_scheduling *trace_stats, unsigned long now_jiffies)
{
	if (trace_stats->next_trace_jiffies == 0) // Disabled
		return false;

	if (now_jiffies < trace_stats->next_trace_jiffies) // Not yet
		return false;

	return true;
}

void nvmeibc_trace_stats_scheduling_set_next(struct nvmeibc_trace_stats_scheduling *trace_stats, unsigned long now_jiffies)
{
	if (!trace_stats->next_trace_jiffies) // Disabled
		return;

	trace_stats->next_trace_jiffies += (trace_stats->trace_stats_period_sec * HZ);

	if (unlikely(now_jiffies >= trace_stats->next_trace_jiffies)) {
		// At least one trace period was skipped, restart (re-randomize phase)
		trace_stats->next_trace_jiffies = now_jiffies + (get_random_u32() % (trace_stats->trace_stats_period_sec * HZ));
	}
}

