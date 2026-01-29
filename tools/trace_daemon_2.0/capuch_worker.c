/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "capuch_worker.h"
#include "trace_channel.h"

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>

#include "../../common/nvmeib_trace_api.h"
#include "trace_compress_lib/compressor.h"

#define MAX_FAILURES_BEFORE_SUICIDE 5

#define _ch_meta get_ch_meta(self->ch)

#define __MODULE_HDR "%s(%d)"
#define __MODULE_HDR_ARGS _ch_meta->name, self->key.cpu
#include "trace_daemon_common.h"

#define __TRACE_DUMP_RANGE_FMT "range(%lu)"
#define __TRACE_DUMP_RANGE_ARG(r) r.count

struct capuch_worker
{
	/** Associated CPU id*/
	union nvmeib_capuch_key key;
	/** Channel back pointer */
	trace_channel_t* ch;
	struct
	{
		/** Thread handle */
		pthread_t thread;
		/** Indicates whether or not the worker thread is running */
		int started;
		/** Indicates whether or not the worker thread shall be aborted */
		volatile int aborted; /* Volatile just to disable compiler optimization, normally it is used
								 with memory barrier */
		struct
		{
			struct compressor* compressor;

			/** Memory area used to store iovec */
			struct iovec iovec[512];
			/** Next log id to open */
			int log_id;
			/** Number of bytes written to current log file */
			size_t bytes_written;
			/** Currently open file descriptor */
			int fd;
			/** Number of consecutive write failures */
			int nfailures;
		} write;
	} priv;
};

/**
 * Create per cpu channel empty object
 */
capuch_worker_t* init_capuch_worker(trace_channel_t* ch, int cpu)
{
	mmap_manager_t* mgr = get_mmap_manager(ch);
	capuch_worker_t* self = calloc(1, sizeof(capuch_worker_t));
	const size_t max_in_size = ARRAY_SIZE(self->priv.write.iovec) * get_trace_buf_size(mgr);
	assert(self);
	self->ch = ch;
	self->key.cpu = cpu;
	self->key.chid = get_ch_meta(ch)->chid;
	if (trace_cfg.compress) {
		self->priv.write.compressor = compressor_create_lz4(max_in_size);
		assert(self->priv.write.compressor);
	}

	return self;
}

/**
 * Destroy per cpu channel object
 */
void destroy_capuch_worker(capuch_worker_t* self)
{
	join_capuch_worker(self);
	if(self)
	{
		if (self->priv.write.compressor) {
			self->priv.write.compressor->ops.destroy(self->priv.write.compressor);
			self->priv.write.compressor = NULL;
		}
		free(self);
	}
}

void* _worker(void*);
/**
 * Start trace channel working thread
 */
int start_capuch_worker(capuch_worker_t* self)
{
	pthread_attr_t attr = {{0}};

	self->priv.aborted = 0;

	/* Unable to create worker thread is critical error. Better restart. */
	assert(!pthread_create(&self->priv.thread, &attr, _worker, (void*)self));

	self->priv.started = 1;

	return 0;
}

/**
 * Wait for working thread to end
 */
void join_capuch_worker(capuch_worker_t* self)
{
	if(self->priv.started)
		pthread_join(self->priv.thread, NULL);
	self->priv.started = 0;
}

/**
 * Wait for working thread to end
 */
void abort_capuch_worker(capuch_worker_t* self)
{
	self->priv.aborted = 1;
}

/**
 * Notify the per cpu channel that log #x exists on disk
 */
int add_existing_log_id(capuch_worker_t* self, int log_id)
{
	if(self->priv.write.log_id <= log_id)
		self->priv.write.log_id = log_id + 1;
	return self->priv.write.log_id;
}

static int __write_data(capuch_worker_t *self, struct iovec *iovecs, size_t n_iovecs, bool advance_pos)
{
	if (!n_iovecs){
		return 0;
	}

	size_t const total_size = iovecs_total_size(iovecs, n_iovecs);
	size_t const written = advance_pos ?
			writev(self->priv.write.fd, iovecs, n_iovecs) :
			pwritev(self->priv.write.fd, iovecs, n_iovecs, lseek(self->priv.write.fd, 0L, SEEK_CUR));

	if (written != total_size) {
		int const err = -errno;
		if (err != -EIO){
			self->priv.write.nfailures += 1;
		}
		if (self->priv.write.nfailures > MAX_FAILURES_BEFORE_SUICIDE) {
			_suicide("Too many writes failed in a row");
		} else {
			return err;
		}
	}

	_debug("Write OK");

	self->priv.write.nfailures = 0;
	if (advance_pos)
		self->priv.write.bytes_written += written;

	return 0;
}

int __write_compression_result(capuch_worker_t* self, struct compression_result cmpr_rv)
{
	if (cmpr_rv.error){ //compression error may be raised only as the result of the incorrect memory management
		_suicide("compression failed; error=%d", cmpr_rv.error);
	}

	return __write_data(self, cmpr_rv.iovecs, cmpr_rv.n_iovecs, true /* advance_pos */);
}

/**
 * Open next log for output
 */
void _open_out_log(capuch_worker_t* self)
{
	int flags = O_WRONLY | O_CREAT;
	struct compressor *compressor = self->priv.write.compressor;
	const char *file_ext = compressor ? compressor->ops.file_ext() : "";

	char filename[MAX_FILENAME];
	snprintf(filename, sizeof(filename), "%s/%s%d.%d%s", _ch_meta->dir, _ch_meta->name, self->key.cpu,
		 self->priv.write.log_id, file_ext);
	++self->priv.write.log_id;

	if (!trace_cfg.ramfs && !trace_cfg.compress) {
		/* ramdisk can't open with O_DIRECT flag and not needed to */
		/* right now compressoor is returning unaligned data */
		flags |= O_DIRECT;
	}

	_debug("Opening new file %s", filename);
	self->priv.write.fd = syscall_or_nfs_syscall(self, open , filename,  flags, 0666);
	if (self->priv.write.fd == -1) {
		_suicide("Failed to open %s", filename);
	}

	if (compressor) {
		struct compression_result const cmpr_rv = compressor->ops.start(compressor);
		int const rv = __write_compression_result(self, cmpr_rv);
		_suicide_on(rv < 0,"failed to write compression header; rv=%d, filename=%s, fd=%d", rv, filename, self->priv.write.fd);
	}

	increment_open_files(self->ch);
	do_unlink(self->ch);
}

/**
 * Check whether range read from proc is empty
 */
static inline int _is_empty_range(struct nvmeib_trace_dump_range* range)
{
	/* @TODO: Currently, kernel responsibility is not to return empty ranges. May change */
	return 0;
}

int _build_iovec(capuch_worker_t* self, struct nvmeib_trace_dump_range* range)
{
	mmap_manager_t* mgr = get_mmap_manager(self->ch);
	int i;

	if(range->count > sizeof(self->priv.write.iovec) / sizeof(self->priv.write.iovec[0]))
		_suicide("Impossible page count in range %lu, whaaaat?", range->count);

	for(i = 0; i < max(1, range->count); ++i)
	{
		self->priv.write.iovec[i].iov_len = get_trace_buf_size(mgr);
		if (!(self->priv.write.iovec[i].iov_base = get_buf_addr(mgr, range->cells[i])))
		{
			return -ESHUTDOWN;
		}
	}
	return 0;
}

static void __close_log_file(capuch_worker_t* self)
{
	struct compressor *compressor = self->priv.write.compressor;

	_debug("Closing log file; fd=%d bytes_written=%zu", self->priv.write.fd, self->priv.write.bytes_written);

	if (compressor) {
		struct compression_result const cmpr_rv = compressor->ops.stop(compressor);
		ssize_t const rv = __write_compression_result(self, cmpr_rv);
		_error_on(rv, "Failed to write compression suffix; rv=%zd, fd=%d", rv, self->priv.write.fd);
	}

	syscall_or_nfs_syscall(self, close, self->priv.write.fd);
	decrement_open_files(self->ch);
	add_to_unlink(self->ch, self->key.cpu, self->priv.write.log_id - 1, MAX_TS);
	self->priv.write.fd = -1;
	self->priv.write.bytes_written = 0;

}

int _write_log_with_wraparound(capuch_worker_t* self, struct nvmeib_trace_dump_range* range)
{
	int rv = 0;
	mmap_manager_t* mgr = get_mmap_manager(self->ch);
	unsigned long trace_buf_size = get_trace_buf_size(mgr);
	size_t actual_write_pages = range->count ? range->count : 1;
	struct compressor *compressor = self->priv.write.compressor;

	_debug("Write range %ld", range->count);

	if (compressor && !range->count) {
		/* Do not write partial cells to compressor, as we cannot overwrite
		 * the compression result on disk later without breaking the compressed stream. */
		return 0;
	}

	if ((rv = _build_iovec(self, range)))
	{
		__close_log_file(self);
		return rv;
	}

	if (compressor) {
		struct compression_result const cmpr_rv =
			compressor->ops.write(compressor, self->priv.write.iovec, actual_write_pages);

		rv = __write_compression_result(self, cmpr_rv);
	} else {
		_debug("Write pos %lu", lseek(self->priv.write.fd, 0L, SEEK_CUR));
		rv = __write_data(self, self->priv.write.iovec, actual_write_pages, !!range->count /* advance_pos */);
	}

	if(!rv && (self->priv.write.bytes_written / trace_buf_size) >= _ch_meta->bufs_per_log)
	{
		__close_log_file(self);
	}

	return rv;
}

/**
 * Per cpu trace channel worker thread
 */
void* _worker(void* param)
{
	capuch_worker_t* self = param;
	cpu_set_t cpu_set = {{0}};
	struct nvmeib_trace_dump_range range;
	self->priv.write.fd = -1;
	self->priv.write.bytes_written = 0;
	int retval;

	CPU_SET(self->key.cpu, &cpu_set);
	/* In case we are unable to set thread affinity, it is critical. Better crash and retry. */
	if ((retval = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set))) {
		_suicide("Setting thread affinity. Thats not even funny retval=(%d)", retval);
	}

	_info("Worker started, log_id=%d channel=%s", self->priv.write.log_id,  get_channel_name(self->ch));

	while(!self->priv.aborted &&
		  (retval = pread(get_control_proc_fd(self->ch), &range, sizeof(range), self->key.raw)) > 0)
	{
		_debug("Read range " __TRACE_DUMP_RANGE_FMT, __TRACE_DUMP_RANGE_ARG(range));
		/* If we've got an empty range - just go back to sleep */
		if(_is_empty_range(&range))
		{
			_debug("Empty range");
			continue;
		}

		/* File descriptor starts as closed. We lazily open it, but not before some actual info
		to write arrives. On file rotation, handle is simply set to -1, and lazily waiting
		for the next write. */
		if(self->priv.write.fd <= 0){
			_open_out_log(self);
		}

		retval = _write_log_with_wraparound(self, &range);
		if (retval == -ESHUTDOWN) 
		{
			retval = 0;
			break;
		}
	}
	if(!self->priv.aborted)
	{
		if(!retval)
		{ /* Graceful close */
			_info("Graceful close of the control proc %s", get_channel_name(self->ch));
		}
		else
		{ /*Da fak is dat?!*/
			_suicide("Bad read, retval=%d", retval);
		}
	}
	else
	{
		_info("Aborted");
	}
	return NULL;
}
