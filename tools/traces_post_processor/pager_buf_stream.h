/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef PAGER_BUF_STREAM
#define PAGER_BUF_STREAM

#include "binary_heap.h"
#include "pager_infra.h"

typedef struct buf_stream_conf {
	cpu_id_t cpu;
	timestamp_t start;
	timestamp_t end;
} buf_stream_conf_t;

/**
 * Public buffer descriptor
 */
typedef struct buf_stream_buf_descr {
	struct{ /*Metadata*/
		buf_pos_t src_pos; /*The source position this buffer originated from*/
		buf_header_t hdr; /*Buf header*/
		struct { /*These are pre-computed from header*/
			timestamp_t ts; /*Timestamp in nanoseconds since epoch*/
			dict_t *dict; /*Reference to dictionary indicated by checksum*/
		} comp;
	} meta;
	void *data; /*Pointer to the actual data*/
} buf_stream_buf_descr_t;


/**
 * Descriptor of sequence of buffers from start to end timestamp, sorted by timestamp, for one given CPU
 */
struct buf_stream_per_cpu;
typedef struct buf_stream_per_cpu buf_stream_per_cpu_t;

/**
 * Initialize buf stream
 */
buf_stream_per_cpu_t *start_buf_stream(channel_ctx_t *ctx, cpu_id_t cpu, timestamp_t start, timestamp_t end);

/**
 * Free resources allocated for buf stream
 */
void stop_buf_stream(buf_stream_per_cpu_t *bs);

/**
 * Get next buffer withing the given time frame. If reached timeframe end - return NULL.
 */
buf_stream_buf_descr_t *get_next_buf_from_stream(buf_stream_per_cpu_t *bs);

/**
 * Return 1 if reached end of stream, 0 therwise
 */
int end_of_buf_stream(buf_stream_per_cpu_t *bs);

/**
 * Return 1 if reached end of stream, 0 therwise
 */
int end_of_buf_stream(buf_stream_per_cpu_t *bs);

/**
 * Retrieve given buffer stream configuration
 */
const buf_stream_conf_t *get_buf_stream_conf(buf_stream_per_cpu_t *bs);

/**
 * Retrieve current buf stream per channel context
 */
channel_ctx_t *get_buf_stream_channel_ctx(buf_stream_per_cpu_t *bs);

/**
 * Retrieve last read buffer's header
 */
buf_header_t *get_buf_stream_buf_hdr(buf_stream_per_cpu_t *bs);

#endif /*PAGER_BUF_STREAM*/
