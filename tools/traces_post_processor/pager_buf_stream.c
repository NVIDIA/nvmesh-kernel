/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <string.h>

#include "pager_buf_stream.h"

struct buf_stream_per_cpu {
	channel_ctx_t *ctx;
	buf_stream_conf_t conf;
	struct {
		FILE *fp;                     /* Working file descriptor. */
		buf_pos_t fp_pos;             /* Working file descriptor position in buf_pos terms */
		buf_pos_t start_pos, end_pos; /* Caculated start and end positions for stream */
		int eos;                      /* 1 if we read all data both on disk and in heap, 0 otherwise */
		int reads_end; /* 1 if we should no more data from disk (we can still have data in the heap), 0 otherwise */
		buf_stream_buf_descr_t *last_read_buf;                 /* Pointer to the last buffer descriptor returned by get_next_buf_from_stream() */
		void *buf_pool;                      /* Memory pool, sufficiently large to hold all read ahead data */
		int buf_pool_count;                  /* Number of buffers in memory pool, specified on init once */
		buf_stream_buf_descr_t *descriptors; /* Buffer descriptors pool */
		heap_t heap;                         /* Minimum heap used to sort incoming buffers by timestamp */
	} state;
};

/**
 * Close and release resources. Leave the stream in state when every read will fail.
 */
void _close_buf_stream(buf_stream_per_cpu_t *bs) {
	if (bs->state.fp)
		fclose(bs->state.fp);
	if (bs->state.buf_pool)
		free(bs->state.buf_pool);
	if (bs->state.descriptors)
		free(bs->state.descriptors);
	heap_destroy(&bs->state.heap);
	bs->state.buf_pool    = NULL;
	bs->state.descriptors = NULL;
	bs->state.fp          = NULL;
	bs->state.eos         = 1;
	bs->state.reads_end   = 1;
}

/**
 * Reads next buffer from disk. Returns 0 on success.
 * If could not read, it is treated as end of stream.
 * Assume stream is open and space allocated.
 */
static int _read_next_buf_from_disk(buf_stream_per_cpu_t *bs, buf_stream_buf_descr_t *descr) {
	buf_pos_t next_pos;

	if (bs->state.reads_end)
		return -1; // End of stream

	// Sanity
	assert(bs->state.fp);
	assert(!bs->state.eos);

	if (buf_pos_gt(bs->ctx, bs->state.fp_pos, bs->state.end_pos)) {
		// Reached end of timeframe. Cut the reads.
		bs->state.reads_end = 1;
		fclose(bs->state.fp);
		bs->state.fp = NULL;
		return -1;
	}

	if (fread(descr->data, bs->ctx->buf_size, 1, bs->state.fp) != 1) {
		// Read failed. Cut the reads here.
		bs->state.reads_end = 1;
		fclose(bs->state.fp);
		bs->state.fp = NULL;
		return -1;
	}

	descr->meta.src_pos = bs->state.fp_pos; /*Save the position this buffer originated from*/

	next_pos = NEXTBUF(bs->ctx, bs->state.fp_pos);
	if (next_pos.file_id != bs->state.fp_pos.file_id) {
		// We need to open next file
		fclose(bs->state.fp); // Close the old one
		if (next_pos.file_id == -1) {
			// Invalid buffer position
			// !IMPORTANT: It is not a failure. Read succeeded. Only next read will fail.
			bs->state.reads_end = 1;
		} else {
			do {
				/* try open the buf */
				if ((bs->state.fp = fopen_at_buf_pos(bs->ctx, next_pos))) {
					break;
				}
			} while(++next_pos.file_id <= bs->state.end_pos.file_id);
			if (!bs->state.fp) {
				// Normally should not happen, but if we have an error here - just cut the stream.
				// !IMPORTANT: It is not a failure. Read succeeded. Only next read will fail.
				bs->state.reads_end = 1;
			}
		}
	}

	bs->state.fp_pos = next_pos;

	return 0;
}

/**
 * Put buffer into stream heap
 * @note Internal use only
 */
timestamp_t _commit_buffer(buf_stream_per_cpu_t *bs, buf_stream_buf_descr_t *descr, timestamp_t last_ts) {
	read_buf_header(descr->data, &descr->meta.hdr);
	descr->meta.comp.dict = get_dict(bs->ctx, descr->meta.hdr.cksum);
	if (!descr->meta.comp.dict) { /*Dictionary unavailable - bad checksum*/
		descr->meta.comp.ts = last_ts; /*Buffer info is unreliable*/
	} else {
		descr->meta.comp.ts = tsc_to_ns(read_buf_timestamp(descr->data), descr->meta.hdr.khz);
	}
	heap_add(&bs->state.heap, descr->meta.comp.ts, descr);
	return descr->meta.comp.ts;
}

void _init_pools(buf_stream_per_cpu_t *bs) {
	// Fill the initial heap
	// Note: it is crucial to do the initial read in size at least of @safe_offset * 2 + 1
	// This is because of the two guarantees of the start point finding algorithm:
	//    1. The actual start point is AT MOST @safe_offset buffers from returned start point
	//    2. The buffers sorting guarantee is in intervals of @safe_offset, i.e:
	//       Let us denote timestamp of buffer #i as ts(i)
	//       For each two numbers i,j:
	//          If j >= @safe_offset, it is guaranteed that ts(i+j) >= ts(i)
	//          If j < @safe_offset, there is no guarantee about any relation between ts(i+j) and ts(i).
	// Hence, to maintain a sorted heap, we need to read ahead:
	//    1. At least @safe_offset buffers to ensure we read up to the start point
	//    2. One buffer to ensure read past the start point.
	//    3. At least @safe_offset more buffers to ensure the next buffer i to be read satisfies
	//       ts(i) >= ts(start point)
	int i;
	bs->state.buf_pool_count =bs->ctx->safe_offset * 2 + 1;
	bs->state.buf_pool       = calloc(1, bs->ctx->buf_size * bs->state.buf_pool_count);
	bs->state.descriptors    = calloc(1, sizeof(buf_stream_buf_descr_t) * bs->state.buf_pool_count);
	assert(bs->state.buf_pool && bs->state.descriptors);
	for (i = 0; i < bs->state.buf_pool_count; ++i) {
		bs->state.descriptors[i].data = bs->state.buf_pool + bs->ctx->buf_size * i;
	}
}

buf_stream_per_cpu_t *start_buf_stream(channel_ctx_t *ctx, cpu_id_t cpu, timestamp_t start, timestamp_t end) {
	int i;
	buf_stream_per_cpu_t *bs = calloc(1, sizeof(buf_stream_per_cpu_t));
	timestamp_t last_ts = 0;
	assert(bs);

	*bs = (buf_stream_per_cpu_t){.conf.cpu = cpu, .conf.start = start, .conf.end = end, .ctx = ctx};
	_init_pools(bs);
	heap_init(&bs->state.heap);
	bs->state.last_read_buf = NULL;

	if (get_first_valid_pos(ctx, cpu).cpu_id == -1) {
		bs->state.eos       = 1; // Empty stream
		bs->state.reads_end = 1;
		return bs;
	}
	bs->state.start_pos = find_start_ts_pos(ctx, start, cpu);
	bs->state.fp_pos    = bs->state.start_pos;
	bs->state.end_pos   = find_end_ts_pos(ctx, end, cpu);

	bs->state.fp        = fopen_at_buf_pos(ctx, bs->state.fp_pos);
	if (!bs->state.fp) {
		/* Could not even open first file. Critical error. Cut the stream, it is empty. */
		bs->state.eos       = 1;
		bs->state.reads_end = 1;
		return bs;
	} else
		bs->state.reads_end = 0;

	for (i = 0; i < (ctx->safe_offset * 2 + 1); ++i) {
		if (_read_next_buf_from_disk(bs, &bs->state.descriptors[i])) {
			/* End of stream :( */
			break;
		}
		last_ts = _commit_buffer(bs, &bs->state.descriptors[i], last_ts);
	}

	return bs;
}

void stop_buf_stream(buf_stream_per_cpu_t *bs) {
	_close_buf_stream(bs);
	free(bs);
}

buf_stream_buf_descr_t *get_next_buf_from_stream(buf_stream_per_cpu_t *bs) {
	if (bs->state.eos)
		return NULL;

	while (1) {
		heap_element_t pop;
		if (!bs->state.heap.count) {
			_close_buf_stream(bs);
			return NULL;
		}
		// Get next element in line
		pop = heap_pop_min(&bs->state.heap);

		if (!bs->state.reads_end && bs->state.last_read_buf) {
			// Reuse the old buffer to read new data if needed
			if (_read_next_buf_from_disk(bs, bs->state.last_read_buf)) 
				assert(bs->state.reads_end); // Sanity
			else
				_commit_buffer(bs, bs->state.last_read_buf, (timestamp_t)pop.key);
		}
		bs->state.last_read_buf = ((buf_stream_buf_descr_t *)pop.data);
		// It is possible that several first buffers we read will be out of time frame.
		// In this case, however, we don't filter them out. We cannot know for sure
		// how many traces this buffer contains, and how large time frame it spans.
		// If the buffer contains no relevant information, it will be filtered out in the
		// future stages.
		if ((timestamp_t)pop.key > bs->conf.end) {
			// If we reach the end of the time frame, we close the stream
			_close_buf_stream(bs);
			return NULL;
		}
		// Before returning the data, read its header and store it
		return bs->state.last_read_buf;
	}

	assert(0); // Should never hit this
	return NULL;
}

int end_of_buf_stream(buf_stream_per_cpu_t *bs) { return bs->state.eos; }

const buf_stream_conf_t *get_buf_stream_conf(buf_stream_per_cpu_t *bs) { return &bs->conf; }

channel_ctx_t *get_buf_stream_channel_ctx(buf_stream_per_cpu_t *bs) { return bs->ctx; }
