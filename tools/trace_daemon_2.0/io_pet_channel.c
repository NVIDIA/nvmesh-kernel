/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "io_pet_channel.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define __MODULE_HDR "%s"
#define __MODULE_HDR_ARGS self->meta.name
#include "trace_daemon_common.h"

#define IO_PET_READ_BUFFER_SIZE (64 * 1024) /* 64KB read buffer */
#define MAX_FAILURES_BEFORE_SUICIDE 5

/**
 * IO PET channel object
 */
struct io_pet_channel
{
	/** 1 if channel started, 0 otherwise */
	int started;

	/* Config data channel is initialized with */
	io_pet_channel_meta_t meta;

	/* Private data */
	struct
	{
		/** Thread handle */
		pthread_t thread;
		/** Indicates whether or not the worker thread is running */
		int started;
		/** Indicates whether or not the worker thread shall be aborted */
		volatile int aborted;
		/** List of data to unlink as new data files are produced */
		unlink_list_t unlink_list;
		/** Next log id to open */
		int log_id;
		/** Number of bytes written to current log file */
		size_t bytes_written;
		/** Currently open file descriptor for output */
		int out_fd;
		/** Currently open file descriptor for input (proc file) */
		int in_fd;
		/** Number of consecutive write failures */
		int nfailures;
		/** Read buffer */
		char* read_buffer;
	} priv;
};

/**
 * Worker thread function
 */
static void* _io_pet_worker(void* param);

/**
 * Process unlink list and remove old log files
 */
static void do_unlink_io_pet(io_pet_channel_t* self)
{
	unlink_list_do_unlink(&self->priv.unlink_list, self->meta.max_logs);
}

/**
 * Open next log file for output
 */
static void _open_out_log(io_pet_channel_t* self)
{
	char filename[MAX_FILENAME];
	int flags = O_WRONLY | O_CREAT;

	snprintf(filename, sizeof(filename), "%s/%s%d.%d", 
		self->meta.dir, self->meta.name, IO_PET_CPU_ID, self->priv.log_id);
	++self->priv.log_id;

	if (!trace_cfg.ramfs) {
		/* ramdisk can't open with O_DIRECT flag and not needed to */
		//flags |= O_DIRECT; io.pet channel cannot use O_DIRECT flag - msgloop limitation
	}

	_debug("Opening new file %s", filename);
	self->priv.out_fd = syscall_or_nfs_syscall(self, open, filename, flags, 0666);
	if (self->priv.out_fd == -1) {
		_suicide("Failed to open %s", filename);
	}

	unlink_list_increment_open_files(&self->priv.unlink_list);
	do_unlink_io_pet(self);
	self->priv.bytes_written = 0;
}

/**
 * Close current log file
 */
static void _close_log_file(io_pet_channel_t* self)
{
	if (self->priv.out_fd < 0) {
		return; /* Already closed */
	}

	_debug("Closing log file; fd=%d bytes_written=%zu", 
		self->priv.out_fd, self->priv.bytes_written);

	syscall_or_nfs_syscall(self, close, self->priv.out_fd);
	unlink_list_decrement_open_files(&self->priv.unlink_list);
	unlink_list_add(&self->priv.unlink_list, IO_PET_CPU_ID, self->priv.log_id - 1, MAX_TS);
	self->priv.out_fd = -1;
	self->priv.bytes_written = 0;
}

/**
 * Close input file (proc file)
 */
static void _close_input_file(io_pet_channel_t* self)
{
	if (self->priv.in_fd >= 0) {
		close(self->priv.in_fd);
		self->priv.in_fd = -1;
	}
}

/**
 * Close all files (input and output)
 */
static void _close_all_files(io_pet_channel_t* self)
{
	_close_input_file(self);
	_close_log_file(self);
}

/**
 * Write data to current log file
 */
static int _write_data(io_pet_channel_t* self, const void* data, size_t len)
{
	if (!len) {
		return 0;
	}

	ssize_t written = write(self->priv.out_fd, data, len);
	if (written != (ssize_t)len) {
		int const err = -errno;
		if (err != -EIO) {
			self->priv.nfailures += 1;
		}
		if (self->priv.nfailures > MAX_FAILURES_BEFORE_SUICIDE) {
			_suicide("Too many writes failed in a row");
		} else {
			return err;
		}
	}

	_debug("Write OK, %zu bytes", written);
	self->priv.nfailures = 0;
	self->priv.bytes_written += written;

	return 0;
}

/**
 * Initialize IO PET channel object
 */
io_pet_channel_t* init_io_pet_channel(const char* dir, const char* name)
{
	int max_logs = get_max_logs(name);
	int bufs_per_log = get_buf_per_log(name);
	/* Calculate max_file_size: bufs_per_log * PAGE_SIZE (4096 bytes) */
	size_t max_file_size = (size_t)bufs_per_log * 4096;
	
	io_pet_channel_t* self = calloc(sizeof(io_pet_channel_t), 1);
	assert(self);
	
	*self = (io_pet_channel_t){
		.started = 0,
		.meta = {
            .dir = strdup(dir),
		    .name = strdup(name),
		    .max_logs = max_logs,
		    .max_file_size = max_file_size
        },
		.priv = {
            .out_fd = -1,
		    .in_fd = -1,
		    .log_id = 0,
		    .bytes_written = 0,
		    .nfailures = 0,
		    .read_buffer = malloc(IO_PET_READ_BUFFER_SIZE)
		}
	};
	
	assert(self->meta.dir && self->meta.name && self->priv.read_buffer);
	unlink_list_init(&self->priv.unlink_list, self->meta.dir, self->meta.name);

	return self;
}

/**
 * Destroy IO PET channel object
 */
void destroy_io_pet_channel(io_pet_channel_t* self)
{
	if (self) {
		join_io_pet_channel(self);
		
		unlink_list_destroy(&self->priv.unlink_list);
		free(self->priv.read_buffer);
		free(self->meta.dir);
		free(self->meta.name);
		free(self);
	}
}

/**
 * Scan directory and populate unlink list with existing log files
 */
static int _populate_unlink_list(io_pet_channel_t* self)
{
	if (unlink_list_populate(&self->priv.unlink_list, 1)) { /* max_cpus = 1 */
		_error("Scanning previous logs");
		return -1;
	}
	
	/* Update log_id based on existing files */
	unlink_candidate_t* cand = self->priv.unlink_list.head;
	int max_id = -1;
	while (cand) {
		if (cand->cpu == IO_PET_CPU_ID && cand->id > max_id) {
			max_id = cand->id;
		}
		cand = cand->next;
	}
	if (max_id >= 0) {
		self->priv.log_id = max_id + 1;
	}
	
	return 0;
}

/**
 * Start IO PET channel worker thread
 */
int start_io_pet_channel(io_pet_channel_t* self)
{
	pthread_attr_t attr = {{0}};

	/* Populate unlink list with existing files */
	if (_populate_unlink_list(self)) {
		_error("Failed to populate unlink list");
		return -1;
	}

	/* Open input file (proc file) */
	self->priv.in_fd = open(IO_PET_PROC_PATH, O_RDONLY);
	if (self->priv.in_fd == -1) {
		_error("Failed to open %s: %s", IO_PET_PROC_PATH, strerror(errno));
		return -1;
	}

	self->priv.aborted = 0;

	/* Unable to create worker thread is critical error. Better restart. */
	if (pthread_create(&self->priv.thread, &attr, _io_pet_worker, (void*)self))
		_suicide("Failed to create IO PET worker thread");

	self->priv.started = 1;
	self->started = 1;

	_info("IO PET channel started: name=%s, log_id=%d, max_logs=%d, max_file_size=%zu", 
		self->meta.name, self->priv.log_id, self->meta.max_logs, self->meta.max_file_size);

	return 0;
}

/**
 * Wait for worker thread to end
 */
void join_io_pet_channel(io_pet_channel_t* self)
{
	if (self && self->priv.started) {
		self->priv.aborted = 1;
		pthread_join(self->priv.thread, NULL);
		self->priv.started = 0;
		self->started = 0;
	}
	
	/* Close files in case worker thread didn't close them */
	if (self) {
		_close_all_files(self);
		unlink_list_clear(&self->priv.unlink_list);
	}
}

/**
 * Reconfigure IO PET channel
 */
void reconf_io_pet_channel(io_pet_channel_t* self)
{
	int bufs_per_log = get_buf_per_log(self->meta.name);
	_info("Reconf IO PET channel");
	self->meta.max_logs = get_max_logs(self->meta.name);
	/* Recalculate max_file_size: bufs_per_log * PAGE_SIZE (4096 bytes) */
	self->meta.max_file_size = (size_t)bufs_per_log * 4096;
	do_unlink_io_pet(self);
}

/**
 * Get channel metadata
 */
const io_pet_channel_meta_t* get_io_pet_ch_meta(io_pet_channel_t* self)
{
	return &self->meta;
}

/**
 * Get channel name
 */
char* get_io_pet_channel_name(io_pet_channel_t* self)
{
	return self->meta.name;
}

/**
 * Per CPU trace channel worker thread
 */
static void* _io_pet_worker(void* param)
{
	io_pet_channel_t* self = param;
	ssize_t bytes_read;
	int retval = 0;

	_info("IO PET worker started, channel=%s, tid=%lu", 
		self->meta.name, (unsigned long)pthread_self());

	while (!self->priv.aborted) {
		/* Read data from proc file */
		bytes_read = read(self->priv.in_fd, self->priv.read_buffer, 
			IO_PET_READ_BUFFER_SIZE);
		
		if (bytes_read < 0) {
			if (errno == EAGAIN || errno == EINTR || errno == EOVERFLOW) {
				continue;
			}
			_error("Read error from %s: %s", IO_PET_PROC_PATH, strerror(errno));
			retval = -errno;
			break;
		}
		
		if (bytes_read == 0) {
			/* EOF - kernel closed the file, wait a bit and try to reopen */
			_debug("EOF reached, closing input file");
			_close_input_file(self);
			
			/* Wait a bit before trying to reopen */
			usleep(100000); /* 100ms */
			
			/* Try to reopen */
			self->priv.in_fd = open(IO_PET_PROC_PATH, O_RDONLY);
			if (self->priv.in_fd == -1) {
				if (errno == ENOENT) {
					_info("Proc file %s does not exist, waiting...", IO_PET_PROC_PATH);
					usleep(1000000); /* 1 second */
					continue;
				}
				_error("Failed to reopen %s: %s", IO_PET_PROC_PATH, strerror(errno));
				retval = -errno;
				break;
			}
			continue;
		}

		/* File descriptor starts as closed. We lazily open it, but not before 
		 * some actual data to write arrives. */
		if (self->priv.out_fd < 0) {
			_open_out_log(self);
		}

		/* Write data to output file */
		retval = _write_data(self, self->priv.read_buffer, bytes_read);
		if (retval < 0) {
			_error("Write failed: %d", retval);
			break;
		}

		/* Check if we need to rotate the file */
		if (self->priv.bytes_written >= self->meta.max_file_size) {
			_close_log_file(self);
		}
	}

	if (!self->priv.aborted) {
		if (!retval) {
			_info("Graceful close of IO PET channel %s", self->meta.name);
		} else {
			_suicide("Bad read/write, retval=%d", retval);
		}
	} else {
		_info("IO PET channel aborted");
	}

	/* Close all files before exiting */
	_close_all_files(self);

	return NULL;
}

