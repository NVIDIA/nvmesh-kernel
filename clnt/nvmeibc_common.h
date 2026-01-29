/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_COMMON_H
#define NVMEIBC_COMMON_H

struct nvmeibc_trace_stats_scheduling {
	unsigned long next_trace_jiffies;
	unsigned trace_stats_period_sec; // Mirror module param, change detection
};

void nvmeibc_trace_stats_scheduling_adjust(struct nvmeibc_trace_stats_scheduling *trace_stats, unsigned long now_jiffies);

bool nvmeibc_trace_stats_scheduling_should_trace(struct nvmeibc_trace_stats_scheduling *trace_stats, unsigned long now_jiffies);

void nvmeibc_trace_stats_scheduling_set_next(struct nvmeibc_trace_stats_scheduling *trace_stats, unsigned long now_jiffies);

#endif /* NVMEIBC_COMMON_H */
