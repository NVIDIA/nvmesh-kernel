/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef _GNU_SOURCE
	#define _GNU_SOURCE
#endif
#include "nvmeib_trace_userspace_poller.h"

// Include compressor.h with workarounds for basename macro.
#include "trace_compress_lib/compressor.h"
#undef basename

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>

#define FILENAME_SIZE 256
#define MAX_CPUS 256

int starts_with(const char *pre, const char *str) {
	size_t lenpre = strlen(pre), lenstr = strlen(str);
	return lenstr < lenpre ? 0 : strncmp(pre, str, lenpre) == 0;
}

int parse_log_filename(const char *filename, const char *basename, int *cpu, long *idx) {
	if (!starts_with(basename, filename))
		return 0;
	else {
		size_t lenbasename = strlen(basename);
		const char *sub = filename + lenbasename;
		*cpu = 0;
		*idx = 0;
		while (*sub >= '0' && *sub <= '9') {
			*cpu = *cpu * 10 + *sub - '0';
			++sub;
		}
		if (*sub == '\0')
			return 0;
		if (*cpu >= MAX_CPUS)
			return 0;
		++sub;
		while (*sub >= '0' && *sub <= '9') {
			*idx = *idx * 10 + *sub - '0';
			++sub;
		}
		if (*sub != '\0' && strcmp(sub, ".lz4") != 0)
			return 0;
		++idx;
		return 1;
	}
}

long get_start_log_id(const char *basename, const char *dir) {
	DIR *dp;
	struct dirent *ep;
	long max_idx = 0;

	dp = opendir(dir);
	if (dp != NULL) {
		while ((ep = readdir(dp)) != NULL) {
			int cpu;
			long idx;

			if (parse_log_filename(ep->d_name, basename, &cpu, &idx) && max_idx <= idx)
				max_idx = idx + 1;
		}
		closedir(dp);
	} else
		perror("listing working dir");

	return max_idx;
}

/**
Similar to shell touch command
Returns whether or not the operation was successful
*/
int __touch_file(const char *filename) {
	FILE *fd = fopen(filename, "w");
	if (!fd) return 0;
	fclose(fd);
	return 1;
}

/**
Loop and get logs whenever available, write to and open file descriptor.
*/
void nvmeib_trace_poll_to_fd_loop(struct trace_channel *ch, FILE *fd) {
	void *buf;
	while ((buf = nvmeib_trace_grab_unflushed_buffer(ch)) != NULL) {
		if (fwrite(buf, nvmeib_trace_get_channel_buf_size(ch), 1, fd) != 1)
			return;
		nvmeib_trace_confirm_flush(ch);
	}
	nvmeib_trace_notify_event(ch); // Notify poller loop is over
}

/**
Open a file for write and launch nvmeib_trace_poll_to_fd_loop.
*/
int nvmeib_trace_poll_to_file_loop(struct trace_channel *ch, const char *filename) {
	FILE *fd = fopen(filename, "w");
	if (!fd)
		return -1;
	nvmeib_trace_poll_to_fd_loop(ch, fd);
	fclose(fd);
	return 0;
}

static int __write_compression_result(int fd, struct compression_result cr)
{
	if (cr.error) {
		fprintf(stderr, "compression operation failed err=%d\n", cr.error);
		return -1;
	}
	if (cr.n_iovecs > 0) {
		size_t expected = iovecs_total_size(cr.iovecs, cr.n_iovecs);
		ssize_t rv = writev(fd, cr.iovecs, cr.n_iovecs);
		if (rv < 0 || (size_t)rv != expected) {
			fprintf(stderr, "compressed writev failed rv=%zd expected=%zu %m\n", rv, expected);
			return -1;
		}
	}
	return 0;
}

static void __delete_old_log(const char *workdir, const char *basename, long old_idx)
{
	char filename[FILENAME_SIZE];

	// Try both plain and compressed variants (handles upgrade/toggle scenarios)
	snprintf(filename, sizeof(filename), "%s/%s0.%ld", workdir, basename, old_idx);
	unlink(filename);
	snprintf(filename, sizeof(filename), "%s/%s0.%ld.lz4", workdir, basename, old_idx);
	unlink(filename);
}

/**
Poll in a loop for traces, writing them to logs in accordance with supplied channel descriptor.
Supports optional LZ4 compression when compressor and compress_enabled are set.
*/
int nvmeib_trace_poll_to_logrotated_file_loop(struct nvmeib_trace_channel_descriptor *ctx) {
	void *buf;
	char filename[FILENAME_SIZE];
	long logidx = ctx->resume_old ? get_start_log_id(ctx->basename, ctx->workdir) : 0;
	int bufs_written;

	if (ctx->place_markers) {
		snprintf(filename, sizeof(filename), "%s/%s_marker.%ld", ctx->workdir, ctx->basename, logidx);
		if (!__touch_file(filename)) {
			fprintf(stderr, "Failed to create marker file %s %m\n", filename);
			return -1;
		}
	}

	while (1) {
		int _fd;
		// Decide compression per-file so runtime toggle takes effect on next rotation
		bool use_compress = ctx->compressor && ctx->compress_enabled && *ctx->compress_enabled;
		const char *file_ext = use_compress ? ctx->compressor->ops.file_ext() : "";
		int flags = O_WRONLY | O_CREAT | O_TRUNC;

		if (!use_compress) {
			flags |= O_DIRECT;  // O_DIRECT incompatible with compressed (unaligned) output
		}

		snprintf(filename, sizeof(filename), "%s/%s0.%ld%s", ctx->workdir, ctx->basename, logidx, file_ext);
		_fd = open(filename, flags, 0755);
		if (_fd < 0) {
			fprintf(stderr, "nvmeib_trace_poll_to_logrotated_file_loop failed to open %s %m\n", filename);
			return -1;
		}

		// Write LZ4 frame header
		if (use_compress) {
			if (__write_compression_result(_fd, ctx->compressor->ops.start(ctx->compressor)) < 0) {
				close(_fd);
				return -1;
			}
		}

		if (ctx->logs_history != -1 && logidx >= ctx->logs_history) {
			__delete_old_log(ctx->workdir, ctx->basename, logidx - ctx->logs_history);
			if (ctx->place_markers) {
				snprintf(filename, sizeof(filename), "%s/%s_marker.%ld", ctx->workdir, ctx->basename, logidx - ctx->logs_history);
				unlink(filename);
			}
		}

		bufs_written = 0;

		while ((buf = nvmeib_trace_grab_unflushed_buffer(ctx->ch)) != NULL) {
			if (use_compress) {
				struct iovec iov = { .iov_base = buf, .iov_len = nvmeib_trace_get_channel_buf_size(ctx->ch) };
				if (__write_compression_result(_fd, ctx->compressor->ops.write(ctx->compressor, &iov, 1)) < 0) {
					close(_fd);
					return -1;
				}
			} else {
				int rv = write(_fd, buf, nvmeib_trace_get_channel_buf_size(ctx->ch));
				if (rv != nvmeib_trace_get_channel_buf_size(ctx->ch)) {
					fprintf(stderr, "nvmeib_trace_poll_to_logrotated_file_loop failed on fwrite rv = %d %m\n", rv);
					return -1;
				}
			}
			nvmeib_trace_confirm_flush(ctx->ch);

			if (ctx->bufs_per_log != -1 && ++bufs_written >= ctx->bufs_per_log) // We need to open a new file
				break;
		}

		// Write LZ4 frame footer
		if (use_compress) {
			if (__write_compression_result(_fd, ctx->compressor->ops.stop(ctx->compressor)) < 0) {
				close(_fd);
				return -1;
			}
		}

		close(_fd);
		if (buf == NULL) // No more logs will come
			break;
		++logidx;
	}

	nvmeib_trace_notify_event(ctx->ch);

	return 0;
}

void nvmeib_trace_channel_descriptor_cleanup(struct nvmeib_trace_channel_descriptor *desc) {
	if (desc && desc->compressor) {
		desc->compressor->ops.destroy(desc->compressor);
		desc->compressor = NULL;
	}
}
