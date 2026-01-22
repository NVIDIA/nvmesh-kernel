/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef PAGER_MSG_STREAM_H
#define PAGER_MSG_STREAM_H

#include "pager_buf_stream.h"

/**
 * Descriptor of a binary trace
 */
typedef struct binary_trace {
	union { /* Local run of pager has buf_descr. Remote run of pager only has dict_cksum. */
		cksum_t dict_cksum;
		buf_stream_buf_descr_t *buf_descr;
	};
	trace_entry_t *entry;   // Trace entry in dictionary used to parse it
	cpu_id_t cpu_id;		// CPU this trace originates from
	timestamp_t ts;			// Timestamp
	channel_ctx_t *ctx;		// Pointer to pager channel ctx this message comes from
	struct { // Fields in case of a regular message
		void *msg;				// Buffer containing the unparsed trace
		size_t len;				// Message actual lengthbt->msg
		long arg_vec[MAX_ARGS]; // Parsed arguments
		int arg_count;			// Length of arg_vec
	};
} binary_trace_t;

struct msg_stream;
typedef struct msg_stream msg_stream_t;

/**
 * Creates an empty message stream. Buffer sreams should be added to it before using,
 * else end of stream will be reached.
 */
msg_stream_t *start_msg_stream();

/**
 * Release resources associated with message stream.
 * !Attached buffer streams are not closed!
 * Make sure to close them manually, either before or after closing msg stream.
 */
void stop_msg_stream(msg_stream_t *ms);

/**
 * Attach buffer stream to msg stream
 */
void attach_buf_stream(msg_stream_t *ms, buf_stream_per_cpu_t *bs);

/**
 * Get current top message in stream. Dont advance the stream. Return NULL if no messages.
 */
binary_trace_t *peek_next_msg(msg_stream_t *ms);

/**
 * Advance the stream and get next message. Return 0 if could advance, non 0 if not
 * *Attention: If current pop causes stream to be empty, we will know it only on the next pop.
 */
int pop_next_msg(msg_stream_t *ms);

/**
 * Return 1 if end of msg stream reached, 0 otherwise
 */
int end_of_msg_stream(msg_stream_t *ms);

/**
 * Return 1 if msg stream is reliable from this point on, 0 if not yet
 */
int msg_stream_is_reliable(msg_stream_t *ms);

#endif /*PAGER_MSG_STREAM_H*/
