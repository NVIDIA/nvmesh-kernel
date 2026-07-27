#ifndef _GNU_SOURCE
	#define _GNU_SOURCE
#endif
#include "nvmeib_trace_userspace_poller.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

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
		if (*sub != '\0')
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

/**
Poll in a loop for traces, writing them to logs in accordance with supplied channel descriptor
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
		snprintf(filename, sizeof(filename), "%s/%s0.%ld", ctx->workdir, ctx->basename, logidx);
		_fd = open(filename, O_WRONLY | O_DIRECT | O_CREAT | O_TRUNC, 0755);
		if (!_fd) {
			fprintf(stderr, "nvmeib_trace_poll_to_logrotated_file_loop failed on fd=%d %m\n", _fd);
			return -1;
		}

		if (ctx->logs_history != -1 && logidx >= ctx->logs_history) { // Delete old logs if needed
			snprintf(filename, sizeof(filename), "%s/%s0.%ld", ctx->workdir, ctx->basename, logidx - ctx->logs_history);
			unlink(filename);
			if (ctx->place_markers) {
				snprintf(filename, sizeof(filename), "%s/%s_marker.%ld", ctx->workdir, ctx->basename, logidx - ctx->logs_history);
				unlink(filename);
			}
		}

		bufs_written = 0;

		while ((buf = nvmeib_trace_grab_unflushed_buffer(ctx->ch)) != NULL) {
			int rv = write(_fd, buf, nvmeib_trace_get_channel_buf_size(ctx->ch));
			if (rv != nvmeib_trace_get_channel_buf_size(ctx->ch)) {
				fprintf(stderr, "nvmeib_trace_poll_to_logrotated_file_loop failed on fwrite rv = %d %m\n", rv);
				return -1;
			}
			nvmeib_trace_confirm_flush(ctx->ch);

			if (ctx->bufs_per_log != -1 && ++bufs_written >= ctx->bufs_per_log) // We need to open a new file
				break;
		}
		close(_fd);
		if (buf == NULL) // No more logs will come
			break;
		++logidx;
	}

	nvmeib_trace_notify_event(ctx->ch);

	return 0;
}