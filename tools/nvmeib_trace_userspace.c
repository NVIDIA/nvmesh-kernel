#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "nvmeib_trace_userspace.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "../common/nvmeib_trace_api.h"
#include "common/compat/kr_incs_compiler_types.h"
#include "common/compat/kr_incs_time_jiff.h"
#include "common/compat/kr_incs_time_jiff.inc.c"

/** Time synchronization data */
#define SYNC_PROC_PATH "/proc/nvmeib/tracer/sinfo"

#define MAX_WAIT_TIME_IN_SECONDS 2

typedef unsigned long long timestamp_t;

struct trace_channel {
	// Settings, set once
	int buf_size;
	int bufs_per_channel;

	int active_buf; // Buffer we currently write to
	int next_flush; // Next buffer to be flushed

	// Meta data saved per buffer
	int buf_seq;
	int flags;

	pthread_mutex_t active_guard;  // Protects active buffer only
	pthread_mutex_t history_guard; // Protects complete buffers
	pthread_cond_t newbuf;		   // Will be notified on each new buffer ready to flush to the disk

	int is_ephemeral;

	int eph_buffers_to_dump;

	char **bufs; // Pointer to the actual buffers
	int offset;  // Current write offset

	timestamp_t last_ts; // Last timestamp
	int terminate;

	/* Statistics */
	unsigned long long bufs_used;
	unsigned long long total_bytes;
};

/* Determine whether given buffer is freshly opened */
#define __is_fresh(ch) ((ch)->offset <= 8)

/***************
UTILITY FUNCTIONS
****************/

#if USE_EXTERN_TIMESTAMP
int init_khz_offset(void) {
	/*Always nanoseconds*/
	tsc_offset = 0;
	tsc_khz = 1000000UL;
	return 0;
}
#else
/**
Try to read khz and offset from the kernel.
On failure fallback to CLOCK_REALTIME
Always success. Return non zero if fallback occurred.
*/
int init_khz_offset(void) {
	/* Try to get info from kernel */
	{
		FILE *f = fopen(SYNC_PROC_PATH, "r");
		if (!f)
			goto fallback;
		if (!fscanf(f, "%u %llu", &tsc_khz, &tsc_offset)) {
			fclose(f);
			goto fallback;
		}
		fclose(f);
		return 0;
	}
fallback:
	int_cpu_freq_tsc_offset_jiffies();
	return 1;
}
#endif

/**
Abstraction over timestamp retrieval
*/
#if USE_EXTERN_TIMESTAMP
/* Timestamp to inject to the next trace, in nanoseconds since epoch */
__thread unsigned long long nvmeib_trace_timestamp_ns;
static inline timestamp_t get_timestamp(void) {
	return nvmeib_trace_timestamp_ns;
}
#else
static inline timestamp_t get_timestamp(void) {
	return nvmeib_public_rdtsc() + tsc_offset;
}
#endif

/***************
PRIVATE FUNCTIONS
****************/

/**
Retrieve a pointer to next character to be written to location
*/

static inline char *write_pos(const struct trace_channel *ch) { return ch->bufs[ch->active_buf] + ch->offset; }

/**
 * Write the initial header info to the buffer - only the info that we know in init time
 * @note Assume empty buffer
 * @note Assume locked history or init stage
 */
void _write_initial_header(struct trace_channel *ch) {
	ch->offset = 0;
	*(uint32_t *)write_pos(ch) = ((++ch->buf_seq & 0xffffff) | ch->flags << 24);
	ch->offset = 4;
	*(uint32_t *)write_pos(ch) = tsc_khz;
	ch->offset = 8;
}

/**
Open an new buffer for writes, assume locks taken
*/
void __attr_no_alignment_sanity _start_new_buf(struct trace_channel *ch) {
	nvmeib_trace_lock_history(ch);
	if (ch->offset <= ch->buf_size - 6) {
		// If buffer is not completelly full terminate it with zeros
		*(uint32_t *)write_pos(ch) = 0;
		*(uint16_t *)(write_pos(ch) + 4) = 0;
	}
	if (++ch->active_buf >= ch->bufs_per_channel)
		ch->active_buf = 0;
	if (ch->next_flush == ch->active_buf) {
		// We made a full circle without a flush. Drop the old buffer.
		// TODO: Consider other behaviours (current behaviour is probaably good enough though)
		// There is a chance that this will cause buffer corruption, but it considering solution possible performance
		// impact, it is acceptible.
		// The problem might occur if we made a full circle, while the oldest buffer is in the middle of dump to disk.
		// In this case, the oldest buffer is discarded, hence new writes to it may arrive. Now if that happens in
		// concurrency with write to disk, it is possible that part of the buffer will be overwitten, possibly
		// making it corrupted.
		// Alternative approches:
		// 1. Discard the new buffers instead (may be feasible, matter of decision). The code is below:
		// if (--ch->active_buf < 0)
		// 	ch->active_buf = ch->bufs_per_channel - 1;
		// 2. Block writes until flush is complete (requires additional lock)
		if (++ch->next_flush >= ch->bufs_per_channel)
			ch->next_flush = 0;
	}
	ch->bufs_used++;
	_write_initial_header(ch);
	// Notify pollers
	nvmeib_trace_notify_event(ch);
	nvmeib_trace_unlock_history(ch);
}

static inline int has_unflushed_traces(struct trace_channel *ch) { return ch->offset > 8; }

/***************
PUBLIC FUNCTIONS
****************/

/**
Lock history guard
*/
int nvmeib_trace_lock_history(struct trace_channel *ch) { return pthread_mutex_lock(&ch->history_guard); }

/**
Unlock history guard
*/
int nvmeib_trace_unlock_history(struct trace_channel *ch) { return pthread_mutex_unlock(&ch->history_guard); }

/**
Lock active buffer guard
*/
int nvmeib_trace_lock_active_buffer(struct trace_channel *ch) { return pthread_mutex_lock(&ch->active_guard); }

/**
Unlock active buffer guard
*/
int nvmeib_trace_unlock_active_buffer(struct trace_channel *ch) { return pthread_mutex_unlock(&ch->active_guard); }

/**
Wait for new data buf / terminate events.
Assume channel is locked before call. After return it will be locked again.
@param timed Wait with timeout, 0 to wait infinitelly
*/
int nvmeib_trace_wait_for_events(struct trace_channel *ch, unsigned int timeout) {
	if (timeout) {
		struct timespec max_wait = {0, 0};
		clock_gettime(CLOCK_REALTIME, &max_wait);
		max_wait.tv_sec += timeout;
		return pthread_cond_timedwait(&ch->newbuf, &ch->history_guard, &max_wait);
	} else
		return pthread_cond_wait(&ch->newbuf, &ch->history_guard);
}

/**
Notify on event
*/
int nvmeib_trace_notify_event(struct trace_channel *ch) { return pthread_cond_signal(&ch->newbuf); }

/**
Return whether or not tracing is terminated
*/
int nvmeib_trace_is_terminated(struct trace_channel *ch) { return ch->terminate; }

/**
Getter for channel buffer size.
*/
int nvmeib_trace_get_channel_buf_size(struct trace_channel *ch) { return ch->buf_size; }

/**
Return a pointer to an unflushed buffer, from this or associated ephemeral channels.
Block if currently no buffers available.
If everything is flushed and terminated return NULL.
*/
void *nvmeib_trace_grab_unflushed_buffer(struct trace_channel *ch) {
	nvmeib_trace_lock_history(ch);
	// Wait until we have a buffer or terminate toggled
	while (1) {
		if (nvmeib_trace_is_terminated(ch))
			break; /* Break on terminated */
		if (!ch->is_ephemeral && ch->next_flush != ch->active_buf)
			break; /* Break on more buffers on non ephemeral */
		if (ch->eph_buffers_to_dump)
			break; /* Break on ephemeral buffers available */
		if (nvmeib_trace_wait_for_events(ch, MAX_WAIT_TIME_IN_SECONDS) == ETIMEDOUT && !ch->is_ephemeral) {
			nvmeib_trace_unlock_history(ch);
			nvmeib_flush(ch);
			nvmeib_trace_lock_history(ch);
		}
	}
	// For ephemeral - dump only if explicitly requested to
	if (ch->is_ephemeral && !ch->eph_buffers_to_dump) {
		nvmeib_trace_unlock_history(ch);
		return NULL;
	}
	// Here we either have a buffer or terminated or it is ephemeral dump
	if (ch->next_flush != ch->active_buf) {
		// Have buffer
		void *p = ch->bufs[ch->next_flush];
		nvmeib_trace_unlock_history(ch);
		return p;
	} else {
		// Terminated
		nvmeib_trace_unlock_history(ch);
		return NULL;
	}
}

/**
Advance unflushed pointer. Previous unflushed buffer can now be reused
*/
void nvmeib_trace_confirm_flush(struct trace_channel *ch) {
	nvmeib_trace_lock_history(ch);
	if (ch->eph_buffers_to_dump)
		--ch->eph_buffers_to_dump;
	if (++ch->next_flush >= ch->bufs_per_channel)
		ch->next_flush = 0;
	nvmeib_trace_unlock_history(ch);
}

/**
Initialize a channel with given number of buffers and given buffer size.
Allocate the resources required.
Associates this channel with another ephemeral channel if specified.
Return pointer to the new channel on success, NULL on failure.
*/
struct trace_channel *nvmeib_init_trace_channel(int buf_size, int bufs_per_channel, int flags, int is_ephemeral) {
	int i = 0;
	struct trace_channel *ch = calloc(1, sizeof(struct trace_channel));
	if (!ch)
		return NULL;
	ch->active_buf = ch->next_flush = ch->buf_seq = ch->offset = ch->terminate = 0;
	ch->flags = flags;
	ch->buf_size = buf_size;
	ch->bufs_per_channel = bufs_per_channel;
	ch->eph_buffers_to_dump = 0;
	ch->last_ts = 0;
	ch->is_ephemeral = is_ephemeral;
	if (pthread_mutex_init(&ch->active_guard, NULL))
		goto releaseobject;
	if (pthread_mutex_init(&ch->history_guard, NULL))
		goto destroyactiveguard;
	if (pthread_cond_init(&ch->newbuf, NULL))
		goto destroystructguard;
	if (!(ch->bufs = malloc(sizeof(char *) * bufs_per_channel)))
		goto destroynewbufcond;
	for (i = 0; i < bufs_per_channel; ++i) {
		if (!(ch->bufs[i] = aligned_alloc(4096, buf_size)))
			goto freemem;
		memset(ch->bufs[i], 0, buf_size);
	}
	if (!tsc_offset) init_khz_offset();
	_write_initial_header(ch);
	return ch;
freemem:
	while (--i >= 0)
		free(ch->bufs[i]);
	free(ch->bufs);
destroynewbufcond:
	pthread_cond_destroy(&ch->newbuf);
destroystructguard:
	pthread_mutex_destroy(&ch->history_guard);
destroyactiveguard:
	pthread_mutex_destroy(&ch->active_guard);
releaseobject:
	free(ch);
	return NULL;
}

/**
Order channel termination. Wait for poller to flush, then return.
*/
void nvmeib_flush_and_terminate(struct trace_channel *ch) {
	nvmeib_trace_lock_active_buffer(ch);
	if (has_unflushed_traces(ch))
		_start_new_buf(ch); // Terminate the active buf
	nvmeib_trace_lock_history(ch);
	ch->terminate = 1;
	nvmeib_trace_notify_event(ch);	// Wake poller up
	nvmeib_trace_wait_for_events(ch, 0); // Wait for poller's response
	nvmeib_trace_unlock_history(ch);
	nvmeib_trace_unlock_active_buffer(ch);
}

/**
Wait for poller to flush, then return. This flushes an entire *
buffer even tho the buffer is not full, don't use often... *
*/
void nvmeib_flush(struct trace_channel *ch) {
	nvmeib_trace_lock_active_buffer(ch);
	if (has_unflushed_traces(ch))
		_start_new_buf(ch); // Terminate the active buf
	nvmeib_trace_lock_history(ch);
	nvmeib_trace_notify_event(ch);	// Wake poller up
	nvmeib_trace_unlock_history(ch);
	nvmeib_trace_unlock_active_buffer(ch);
}

/**
Release the resources of the given channel.
*/
void nvmeib_destroy_trace_channel(struct trace_channel *ch) {
	while (--ch->bufs_per_channel >= 0)
		free(ch->bufs[ch->bufs_per_channel]);
	free(ch->bufs);
	pthread_mutex_destroy(&ch->history_guard);
	pthread_mutex_destroy(&ch->active_guard);
	pthread_cond_destroy(&ch->newbuf);
	free(ch);
}

void __put_error_in_buffer(void *buf, int error_code) {
	(void)error_code; /* For now - only one error code */
	*((unsigned short *)buf) = error_code;
}

#define HEADER_PLUS_TIMESTAMP_SIZE 16

/**
Get a pointer to buffer of size enough to hold 'len' bytes trace.
Require 'len' is at least 4 (for sake of consistency with the
kernel version). Typical usage:
1) ptr = start_trace_write(len)
2) write some data to *ptr (must be fast)
3) finish_trace_write()
*/
void* __attr_no_alignment_sanity nvmeib_start_trace_write(struct trace_channel *ch, int len, unsigned int cksum) {
	int write_error = 0; /* Indicates that an error message shall be written to buffer */
	if (len > ch->buf_size - (int)sizeof(struct nvmeib_trace_header) - 8) {
		write_error = 8;
		len = 6; /*4 for (low) timestamp + 2 for trace id*/
	}

	nvmeib_trace_lock_active_buffer(ch);
	// Starting from this point ==> mutex aquired

	if (ch->terminate) {
		nvmeib_trace_unlock_active_buffer(ch);
		return NULL;
	}

	if (len > ch->buf_size - HEADER_PLUS_TIMESTAMP_SIZE) {
		/* Invalid length - more than max theoretical trace size */
		return NULL;
	}

	{
		int long_ts = 0;
		int write_cksum = 0;
		timestamp_t ts = get_timestamp();
		char *p;
		if (ts - ch->last_ts > (1LL << 31)) {
			// We need more than 32 bits to store timestamp
			long_ts = 1;
			len += 4;
		}
		if (ch->buf_size - ch->offset < len) {
			_start_new_buf(ch);
		}

		if (__is_fresh(ch)) { // It is a fresh buffer
			// When opening a new buffer - always make sure to start with long timestamp
			if (!long_ts) {
				long_ts = 1;
				len += 4;
			}
			write_cksum = 1;
			len += sizeof(cksum);
		}

		p = write_pos(ch);
		ch->offset += len;
		ch->last_ts = ts;
		if (write_cksum) {
			*(uint32_t *)p = cksum;
			p += sizeof(cksum);
		}
		if (long_ts) {
			*(uint64_t *)p = (ts << 1) | 1;
			p += 8;
		} else {
			*(uint32_t *)p = (uint32_t)ts << 1;
			p += 4;
		}
		ch->total_bytes += len;
		if (!write_error) return p;
		else {
			__put_error_in_buffer(p, write_error);
			nvmeib_trace_unlock_active_buffer(ch);
			return NULL;
		}
	}
}

/**
Finished writing the trace, release resources
*/
void nvmeib_finish_trace_write(struct trace_channel *ch) { nvmeib_trace_unlock_active_buffer(ch); }

/**
Dump ephemeral channel dump.
Assume such channel exists and is initialized.
*/
void nvmeib_dump_ephemeral(struct trace_channel *ch) {
	nvmeib_trace_lock_active_buffer(ch);
	// Cut active buffer if needed.
	if (has_unflushed_traces(ch))
		_start_new_buf(ch);
	nvmeib_trace_unlock_active_buffer(ch);

	nvmeib_trace_lock_history(ch);
	// Count the buffers we need to flush
	if (ch->next_flush <= ch->active_buf) {
		ch->eph_buffers_to_dump = ch->active_buf - ch->next_flush;
	} else {
		ch->eph_buffers_to_dump = ch->bufs_per_channel - ch->next_flush + ch->active_buf;
	}
	nvmeib_trace_unlock_history(ch);

	nvmeib_trace_notify_event(ch); // Wake up for new traces
}


/**
 * User space implementation of symbol resolution by address.
 * See also the user space implementation in nvmeib_trace.c
 * @todo: Not sure it is supposed to be in this file
 */
/**
 * Get the length of the symbol string pointed by address
 */
int nvmeib_symbol_length(void *addr) {
	Dl_info info;
	if (!dladdr(addr, &info) || !info.dli_sname) {
		return snprintf(NULL, 0, "%p", addr); /* Run snprintf with length = 0 to just get the length */
	} else {
		return strlen(info.dli_sname);
	}
}
/**
 * Print symbol string into dest
 */
int nvmeib_symbol_strcpy(char *dest, void *addr, int len) {
	Dl_info info;
	if (!dladdr(addr, &info) || !info.dli_sname) {
		return snprintf(dest, len, "%p", addr);
	} else {
		return snprintf(dest, len, "%s", info.dli_sname);
	}
}

struct __stack_trace_and_len {
	int len;
	void **st;
};

/**
 * Get the length of the stack trace string pointed by address
 */
int nvmeib_stack_trace_length(void *addr) {
	int sum = 0;
	int i;
	struct __stack_trace_and_len *st = addr;
	for (i = 0; i < st->len; ++i)
		sum += nvmeib_symbol_length(st->st[i]) + 2; /*1 for \n 1 for >*/
	return sum + 1; /*1 for the extra \n*/
}

/**
 * Print stack trace string into dest
 */
int nvmeib_stack_trace_strcpy(char *dest, void *addr, int len) {
	int i;
	int sum = 0;
	struct __stack_trace_and_len *st = addr;
	dest[sum] = '\n';
	sum ++;
	for (i = 0; i < st->len; ++i) {
		dest[sum] = '>';
		sum ++;
		sum += nvmeib_symbol_strcpy(dest + sum, st->st[i], len - sum);
		dest[sum] = '\n';
		sum ++;
	}
	dest[sum] = '\0';
	return sum;
}

unsigned long long nvmeib_trace_get_total_bytes(struct trace_channel *ch) {
	return ch->total_bytes;
}

unsigned long long nvmeib_trace_get_total_bufs_used(struct trace_channel *ch) {
	return ch->bufs_used;
}
