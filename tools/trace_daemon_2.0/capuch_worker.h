/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef TRACE_CHANNEL_PER_CPU_H
#define TRACE_CHANNEL_PER_CPU_H

#include "trace_channel.h"

struct capuch_worker;
typedef struct capuch_worker capuch_worker_t;

capuch_worker_t *init_capuch_worker(trace_channel_t *ch, int cpu);
void destroy_capuch_worker(capuch_worker_t *self);
int start_capuch_worker(capuch_worker_t *self);
void join_capuch_worker(capuch_worker_t *self);
void abort_capuch_worker(capuch_worker_t *self);
int add_existing_log_id(capuch_worker_t *self, int log_id);

#endif /* TRACE_CHANNEL_PER_CPU_H */
