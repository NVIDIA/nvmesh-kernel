/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdarg.h>
#include <errno.h>
#include <string.h>

#include "pager_msg_stream.h"
#include "binary_heap.h"

#define SPECIAL_MESSAGE_MAX_SIZE 1024

/**
 * For internal use, shortened syntax
 */
#define bs_ctx(state) (get_buf_stream_channel_ctx((state)->bs))

typedef struct buf_stream_read_state {
	msg_stream_t *ms; /* Parent msg stream */
	buf_stream_per_cpu_t *bs; /* Buffer stream we monitor */
	buf_stream_buf_descr_t *buf_descr;
	void *read_pos;
	char system_msg_buffer[SPECIAL_MESSAGE_MAX_SIZE]; /* System messages buffer used to store messages from pager to user*/
	timestamp_t tsc_ts; /* Timestamp of the head message in tsc units that change per stream */
	binary_trace_t head_msg;
} buf_stream_read_state_t;

struct msg_stream {
	heap_t heap;
	int n_greetings;
};

msg_stream_t *start_msg_stream() {
	msg_stream_t *ms = calloc(1, sizeof(msg_stream_t));
	assert(ms);
	heap_init(&ms->heap);
	return ms;
}

void stop_msg_stream(msg_stream_t *ms) {
	heap_element_t *e;
	foreach_heap_element(ms->heap, e) { free(e->data); }
	heap_destroy(&ms->heap);
	free(ms);
}

/**
 * Writes to system_msg_buffer inside state.
 */
/**
 * Writes to system_msg_buffer inside state.
 */
int _write_specific_system_message(buf_stream_read_state_t *state, trace_entry_t *entry, buf_pos_t pos, const char *fmt, ...) {
	va_list args;
	char *buf;
	if (state->head_msg.entry != &SYSTEM_TRACE_AGAIN) {
		state->head_msg.ctx   = bs_ctx(state);
		state->head_msg.msg   = state->system_msg_buffer;
		state->head_msg.ts    = tsc_to_ns(state->tsc_ts, state->buf_descr->meta.hdr.khz); /* Reuse the previous timestamp */

		if (state->head_msg.entry != &SYSTEM_TRACE) {
			state->head_msg.entry = entry;

			buf = state->system_msg_buffer + snprintf(state->system_msg_buffer, sizeof(state->system_msg_buffer),
													"PAGER MSG At " BUF_POS_FMT ": ", BUF_POS_ARG(bs_ctx(state), pos));

			va_start(args, fmt);
			vsnprintf(buf, sizeof(state->system_msg_buffer) - (buf - state->system_msg_buffer), fmt, args);
			va_end(args);
		} else {
			state->head_msg.entry = &SYSTEM_TRACE_AGAIN;
			snprintf(state->system_msg_buffer, sizeof(state->system_msg_buffer), "PAGER MSG: More system messages are hidden");
		}
		state->head_msg.len = strlen(state->head_msg.msg) + 1;
		return 0;
	} else {
		return -EAGAIN;
	}
}
#define _write_system_message(state, pos, fmt, ...) _write_specific_system_message(state, &SYSTEM_TRACE, pos, fmt, ##__VA_ARGS__)
#define _write_greeting_message(state, pos, fmt, ...) _write_specific_system_message(state, &SYSTEM_TRACE_GREETING, pos, fmt, ##__VA_ARGS__)

/**
 * Given buffer stream state, read next message. Set next read position.
 * If buffer end reached after read - set next read position to NULL.
 * Will fetch new buffers if needed.
 * Return 0 if could read, non zero otherwise
 */
int _read_next_message(buf_stream_read_state_t *state) {
	int trace_len;
	if (!state->read_pos) {
		int newstream = (state->buf_descr == NULL);
		int exp_serial = 0;
		int old_serial = 0;
		timestamp_t old_ts = 0;
		state->head_msg.ts = 0; // When reading new buffer, expect long timestamp

		if (!newstream) {/* Buffer serial we expect to read */
			exp_serial = (state->buf_descr->meta.hdr.serial + 1) & 0xffffff;
			old_serial = state->buf_descr->meta.hdr.serial;
			old_ts = state->buf_descr->meta.comp.ts;
		}

		// Need to fetch a new buffer
		state->buf_descr = get_next_buf_from_stream(state->bs);
		if (!state->buf_descr)
			return -1; // End of stream

		state->read_pos = state->buf_descr->data + BUFFER_HEADER_SIZE;

		state->head_msg.buf_descr = state->buf_descr;

		/* Check possible buffer corruption errors */
		if (!state->buf_descr->meta.hdr.cksum && !state->buf_descr->meta.comp.ts) {
			/* empty checksum+ts hints a zeroed buffer created from crash_analyzer,
			 drop it - will cause Buffer serial jumped, an indication of traces error but thats true */
			state->read_pos = NULL;
			return -EAGAIN;
		}
		else if (!get_dict(bs_ctx(state), state->buf_descr->meta.hdr.cksum)) { // Bad signature
			state->read_pos = NULL; /*Next read will fetch a new buffer*/
			return _write_system_message(state, state->buf_descr->meta.src_pos,
				                         "No dictionary found for buffer checksum: 0x%x", state->buf_descr->meta.hdr.cksum);
		} else if (newstream) {
			/*New stream - print greeting message*/
			state->tsc_ts      = read_timestamp(state->read_pos, state->tsc_ts);
			state->head_msg.ts = tsc_to_ns(state->tsc_ts, state->buf_descr->meta.hdr.khz);
			return _write_greeting_message(state, state->buf_descr->meta.src_pos, "Data for CPU %llu start here",
			                               state->head_msg.cpu_id);
		} else if (state->buf_descr->meta.hdr.serial == old_serial && state->buf_descr->meta.comp.ts == old_ts) { /*Duplicate buffer*/
			state->read_pos = NULL; /*Drop this buffer, Next read will fetch a new buffer*/
			return _write_system_message(state, state->buf_descr->meta.src_pos,
				                         "Duplicate buffer detected");
		} else if (exp_serial != state->buf_descr->meta.hdr.serial) { // Lost buffers
			state->tsc_ts      = read_timestamp(state->read_pos, state->tsc_ts);
			state->head_msg.ts = tsc_to_ns(state->tsc_ts, state->buf_descr->meta.hdr.khz);
			return _write_system_message(state, state->buf_descr->meta.src_pos,
			                             "Buffer serial jumped, an indication of traces loss: expected %u, got %u",
			                              exp_serial, state->buf_descr->meta.hdr.serial);
		}
	}
	state->head_msg.entry = lazy_compile_entry(state->buf_descr->meta.comp.dict, bs_ctx(state)->is, state->read_pos, 0);
	state->head_msg.ctx = bs_ctx(state);

	if (!state->head_msg.entry ||
		!(trace_len = trace_actual_length(state->head_msg.entry, state->read_pos))) { // Corrupted buffer
		int corruption_post = state->read_pos - state->buf_descr->data;
		state->read_pos = NULL; // Next read will fetch a new buffer
		return _write_system_message(state, state->buf_descr->meta.src_pos, "Buffer corrupted at offset 0x%x",
		                      corruption_post);
		// Keep timestamp as previous message (or 0 if it the first one ever).
		// We don't care about values of other field.
	}

	// We are here if all was good.
	state->head_msg.msg = state->read_pos;
	state->tsc_ts = read_timestamp(state->head_msg.msg, state->tsc_ts);
	state->head_msg.ts = tsc_to_ns(state->tsc_ts, state->buf_descr->meta.hdr.khz);
	state->head_msg.len = trace_len;
	state->read_pos += trace_len;
	// Check if we reached the end of the buffer
	if (state->read_pos - state->buf_descr->data >= bs_ctx(state)->buf_size - 6) {
		state->read_pos = NULL; // Next read will fetch a new buffer
	} else {
		// Buffer was cut on purpose
		if ((*(int *)state->read_pos == 0) && (*(short *)(state->read_pos + 4)) == 0)
			state->read_pos = NULL;
	}

	return 0;
}

void attach_buf_stream(msg_stream_t *ms, buf_stream_per_cpu_t *bs) {
	int rv;
	buf_stream_read_state_t *state = calloc(1, sizeof(buf_stream_read_state_t));
	assert(state);
	state->bs = bs;
	state->head_msg.cpu_id = get_buf_stream_conf(bs)->cpu;
	state->read_pos = NULL; // This will cause next read to fetch a new buffer.
	state->buf_descr = NULL;		// Indicate it is a new stream
	state->ms = ms;
	++ms->n_greetings; /*Expect a greeting from this channel*/
	while (1) {
		rv = _read_next_message(state);
		if (rv == -EAGAIN) continue;
		if (rv != 0) 
			free(state); // We tried. No data. End of stream.
		else
			heap_add(&ms->heap, state->head_msg.ts, state);
		break;
	}
}

binary_trace_t *peek_next_msg(msg_stream_t *ms) {
	if (!end_of_msg_stream(ms))
		return &((buf_stream_read_state_t *)heap_peek_min(&ms->heap).data)->head_msg;
	return NULL;
}

int pop_next_msg(msg_stream_t *ms) {
	if (!end_of_msg_stream(ms)) {
		buf_stream_read_state_t *state = heap_pop_min(&ms->heap).data;
		int rv;
		if (ms->n_greetings > 0 && state->head_msg.entry->trace_id == SYSTEM_TRACE_GREETING_ID) {
			/*Last message was a greeting - lets see if it is the last greeting*/
			if (! --ms->n_greetings) {
				/*All greetings sent, notify about that*/
				_write_greeting_message(state, state->buf_descr->meta.src_pos, "From this point, all data on all CPUs exist");
				heap_add(&ms->heap, state->head_msg.ts, state);
				ms->n_greetings = -1;
				return 0;
			}
		}
		while (1) {
			rv = _read_next_message(state);
			if (rv == -EAGAIN) continue;
			if (rv != 0) 
				free(state); // We tried. No data. End of stream.
			else
				heap_add(&ms->heap, state->head_msg.ts, state);
			return 0; // Possibly more data
		}
	}
	return -1; // No more data
}

int end_of_msg_stream(msg_stream_t *ms) { return heap_empty(&ms->heap); }

int msg_stream_is_reliable(msg_stream_t *ms) { return ms->n_greetings == -1; }
