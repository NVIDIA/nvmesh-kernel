/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/
#ifndef NVMEIB_TRACE_USERSPACE_POLLER_H
#define NVMEIB_TRACE_USERSPACE_POLLER_H

#include "nvmeib_trace_userspace.h"

#include <stdio.h>

// Descriptor of trace channel + poller properties
struct nvmeib_trace_channel_descriptor {
	struct trace_channel *ch; // Pointer to the channel descriptor
	const char *basename;     // File name pattern to dump logs to
	const char *workdir;      // Directory to put logs into
	long logs_history;         // How many logs to preserve back
	long bufs_per_log;         // After how many buffers dumped we start a new log
	long resume_old;           // Whether to keep old logs or drop them and start from 0
	long place_markers;        // Whether or not to place marker files to identify logger restart
};

/**
 * Wrapper macro to allocate and init trace channel descriptor with
 * arguments in terms of history size in MB
 */
#define nvmeib_init_trace_channel_descriptor(_ch, _basename, _workdir, _logs_history_mb, _single_file_size_mb, ...)    \
	({                                                                                                                 \
		struct nvmeib_trace_channel_descriptor *____descriptor =                                                       \
		    malloc(sizeof(struct nvmeib_trace_channel_descriptor));                                                    \
		if (____descriptor)                                                                                            \
			*____descriptor = (struct nvmeib_trace_channel_descriptor){                                                \
			    .ch           = _ch,                                                                                   \
			    .basename     = _basename,                                                                             \
			    .workdir      = _workdir,                                                                               \
			    .logs_history = ((long)_logs_history_mb + _single_file_size_mb - 1) / _single_file_size_mb,                  \
			    .bufs_per_log = ((long)_single_file_size_mb * 1024 * 1024 + nvmeib_trace_get_channel_buf_size(_ch) - 1) /    \
			                    nvmeib_trace_get_channel_buf_size(_ch),                                                \
			    ##__VA_ARGS__};                                                                                        \
		____descriptor;                                                                                                \
	})

void nvmeib_trace_poll_to_fd_loop(struct trace_channel *ch, FILE *fd);
int nvmeib_trace_poll_to_file_loop(struct trace_channel *ch, const char *filename);
int nvmeib_trace_poll_to_logrotated_file_loop(struct nvmeib_trace_channel_descriptor *ctx);

#endif /*NVMEIB_TRACE_USERSPACE_POLLER_H*/
