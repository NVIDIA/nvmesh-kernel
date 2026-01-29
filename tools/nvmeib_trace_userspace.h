/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIB_TRACE_USERSPACE_H
#define NVMEIB_TRACE_USERSPACE_H

struct trace_channel;

int nvmeib_trace_lock_history(struct trace_channel *ch);

int nvmeib_trace_unlock_history(struct trace_channel *ch);

int nvmeib_trace_lock_active_buffer(struct trace_channel *ch);

int nvmeib_trace_unlock_active_buffer(struct trace_channel *ch);

int nvmeib_trace_wait_for_events(struct trace_channel *ch, unsigned int timeout);

int nvmeib_trace_notify_event(struct trace_channel *ch);

int nvmeib_trace_is_terminated(struct trace_channel *ch);

int nvmeib_trace_get_channel_buf_size(struct trace_channel *ch);

void* nvmeib_trace_grab_unflushed_buffer(struct trace_channel *ch);

void nvmeib_trace_confirm_flush(struct trace_channel *ch);

struct trace_channel *nvmeib_init_trace_channel(int buf_size, int bufs_per_channel, int flags, int is_ephemeral);

void nvmeib_flush_and_terminate(struct trace_channel *ch);

void nvmeib_flush(struct trace_channel *ch);

void nvmeib_destroy_trace_channel(struct trace_channel *ch);

void *nvmeib_start_trace_write(struct trace_channel *ch, int len, unsigned int cksum);

void nvmeib_finish_trace_write(struct trace_channel *ch);

void nvmeib_dump_ephemeral(struct trace_channel *ch);

int nvmeib_symbol_length(void *addr);

int nvmeib_symbol_strcpy(char *dest, void *addr, int len);

int nvmeib_stack_trace_length(void *addr);

int nvmeib_stack_trace_strcpy(char *dest, void *addr, int len);

unsigned long long nvmeib_trace_get_total_bytes(struct trace_channel *ch);
unsigned long long nvmeib_trace_get_total_bufs_used(struct trace_channel *ch);
/* Use this Define at the top of .c file if in order to enable all traces */
#define FORCE_FILE_TRACES

#endif /*NVMEIB_TRACE_USERSPACE_H*/
