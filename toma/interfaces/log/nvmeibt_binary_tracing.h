/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_BINARY_TRACING_H
#define NVMEIBT_BINARY_TRACING_H
#define USER_SPACE_TRACING

#include <stdbool.h>
#include <pthread.h>
#include <stdlib.h>
#include "nvmeibt_trace.h"
#include "../tools/nvmeib_trace_userspace_poller.h"

#define TRACE_BUFFER_SIZE  4096
#define TRACE_CHANNEL_BUFS 1024

void nvmeibt_start_all_trace_pollers(bool is_running_as_a_utility);
void nvmeibt_flush_all_traces(void);
void nvmeibt_flush_all_and_terminate(void);
void nvmeibt_join_all_trace_pollers(void);

// Stats
unsigned long long nvmeibt_get_total_bytes(void);
unsigned long long nvmeibt_get_used_bufs(void);

// Runtime control level of traces
void nvmeibt_toggle_logging(void);
void nvmeibt_binary_tracing_set_tracer_debug_level(int64_t tracer_debug_level);
void nvmeibt_binary_tracing_enforce_active_tracer_nvmeibt_debug_level(int64_t tracer_debug_level);
int64_t nvmeibt_binary_tracing_get_tracer_debug_level(void);

#endif // NVMEIBT_BINARY_TRACING_H

