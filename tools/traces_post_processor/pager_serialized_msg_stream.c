/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "pager_serialized_msg_stream.h"

#define buf_len(self) (*((size_t *)self->buf)) /*Length is written at buffer header*/
#define _BAD_CURSOR ((char *)-1)

/**
 * Single serialized message header
 */
struct _msg_hdr {
	cksum_t cksum;    /*Identifies*/
	timestamp_t ts;   /*Actual timestamp*/
	cpu_id_t cpu : 8; /*Cpu it originated on*/
} __attribute__((packed));

#define _HDR_SIZE (sizeof(struct _msg_hdr))

/*WRITE*/

struct serialized_msg_write_stream {
	int fd;                         /*The file descriptor we write to*/
	char buf[TRANSFER_BUFFER_SIZE]; /*Cache buffer*/
};

serialized_msg_write_stream_t *init_serialized_msg_write_stream(int fd) {
	serialized_msg_write_stream_t *self = calloc(1, sizeof(serialized_msg_write_stream_t)); /*Alloc and init to 0*/
	assert(self);                                                                           /*No mem*/
	self->fd      = fd;
	buf_len(self) = sizeof(size_t);
	return self;
}

void free_serialized_msg_write_stream(serialized_msg_write_stream_t *self) {
	if (self) {
		write_msg_flush(self);
		free(self);
	}
}

int write_msg_serialize(serialized_msg_write_stream_t *self, binary_trace_t *bt) {
	assert(bt->len + _HDR_SIZE <= TRANSFER_BUFFER_SIZE);               /*Sane assumption on single message size*/
	if (bt->len + _HDR_SIZE <= TRANSFER_BUFFER_SIZE - buf_len(self)) { /*We have enugh space left*/
		struct _msg_hdr *hdr = ((struct _msg_hdr *)(self->buf + buf_len(self)));
		*hdr                 = (struct _msg_hdr){.cksum = bt->buf_descr->meta.hdr.cksum, .ts = bt->ts, .cpu = bt->cpu_id};
		if (IS_SYSTEM_TRACE_ID(bt->entry->trace_id))
			hdr->cksum = SYSTEM_TRACE_DICT_CKSUM;
		memcpy(self->buf + buf_len(self) + _HDR_SIZE, bt->msg, bt->len);
		buf_len(self) += bt->len + _HDR_SIZE;
		return 0;
	} else {
		int rv = write_msg_flush(self);
		if (rv)
			return rv;
		else
			return write_msg_serialize(self, bt);
	}
}

int write_msg_flush(serialized_msg_write_stream_t *self) {
	if (buf_len(self) > sizeof(size_t)) {
		int written   = write(self->fd, self->buf, TRANSFER_BUFFER_SIZE);
		buf_len(self) = sizeof(size_t);
		if (written != TRANSFER_BUFFER_SIZE)
			return -EIO;
	}
	return 0;
}

/*READ*/

struct serialized_msg_read_stream {
	int fd;                         /*The file descriptor we areading from*/
	char buf[TRANSFER_BUFFER_SIZE]; /*Read buffer*/
	char *cursor;                   /*Read cursor position inside read buffer*/
	char *wrk_dir;                  /*Working directory we look for dictionaries in*/
	struct dict_pool_t_ht dicts;    /*Dictionaries pool*/
	immutable_string_store_t *is;
};

int _fetch_next_buf(serialized_msg_read_stream_t *self) {
	assert(!is_serialized_msg_read_stream_eof(self));
	int data_read = 0;
	while (data_read != TRANSFER_BUFFER_SIZE){
		int rv = read(self->fd, self->buf + data_read, TRANSFER_BUFFER_SIZE - data_read);
		if (!rv) {
			self->cursor = _BAD_CURSOR; /*EOF*/
			if (data_read != 0)
				return -EBADMSG;
			return 0;
		}
		if (rv < 0)
			return rv;
		data_read += rv;
	}
	self->cursor = self->buf + sizeof(size_t);
	return 1;
}

serialized_msg_read_stream_t *init_serialized_msg_read_stream(int fd, const char *wrk_dir,
                                                              immutable_string_store_t *is) {
	serialized_msg_read_stream_t *self = calloc(1, sizeof(serialized_msg_read_stream_t)); /*Alloc and init to 0*/
	assert(self);                                                                         /*No mem*/
	self->fd      = fd;
	self->wrk_dir = strdup(wrk_dir);
	dict_pool_t_ht_init(&self->dicts);
	assert(wrk_dir);                                 /*No mem*/
	self->is     = is;                               /*Immutable strings store*/
	self->cursor = self->buf + TRANSFER_BUFFER_SIZE; /*Next read will fetch a buffer*/
	return self;
}

void free_serialized_msg_read_stream(serialized_msg_read_stream_t *self) {
	if (self) {
		if (self->wrk_dir)
			free(self->wrk_dir);
		free_dict_pool(&self->dicts);
		free(self);
	}
}

int read_msg_deserialize(serialized_msg_read_stream_t *self, binary_trace_t *bt) {
	if (!is_serialized_msg_read_stream_eof(self)) {
		if (self->cursor >= self->buf + buf_len(self)) { /*Need to open a new buffer*/
			int rv = _fetch_next_buf(self);
			if (rv == 1) return read_msg_deserialize(self, bt);
			return rv;
		} else {
			struct _msg_hdr *hdr = (struct _msg_hdr *)self->cursor;
			bt->msg              = self->cursor + _HDR_SIZE;
			bt->cpu_id           = hdr->cpu;
			bt->ts               = hdr->ts;
			bt->dict_cksum       = hdr->cksum;
			if (bt->dict_cksum == SYSTEM_TRACE_DICT_CKSUM) {
				bt->entry = &SYSTEM_TRACE;
				bt->len   = strlen(bt->msg) + 1;
			} else {
				dict_t *dict = get_dict_from_pool(&self->dicts, bt->dict_cksum, self->wrk_dir);
				if (!dict) {
					fprintf(stderr, "Critical error: broken stream, unknown cksum %u\n", bt->dict_cksum);
					self->cursor = _BAD_CURSOR; /*Cut that crap*/
					return -1;
				}
				bt->entry = lazy_compile_entry(dict, self->is, bt->msg, 0);
				if (!bt->entry) {
					fprintf(stderr, "Critical error: broken stream, entry\n");
					self->cursor = _BAD_CURSOR; /*Cut that crap*/
					return -1;
				}
				bt->len = trace_actual_length(bt->entry, bt->msg);
			}
			self->cursor += (_HDR_SIZE + bt->len);
			return 1;
		}
	}
	return 0;
}

int is_serialized_msg_read_stream_eof(serialized_msg_read_stream_t *self) { return self->cursor == _BAD_CURSOR; }

/*MERGE*/

/**
 * Represents a single host in stream merger.
 */
struct _stream_merger_host {
	serialized_msg_read_stream_t *stream; /*Stream we operate on*/
	int fd;                               /*File we operate on*/
	binary_trace_t last;                  /*Head msg*/
	char *hname;                          /*Hostname*/
};

struct stream_merger {
	heap_t heap;
	char *wrk_dir;
	immutable_string_store_t *is;
};

stream_merger_t *init_stream_merger(const char *wrk_dir, immutable_string_store_t *is) {
	stream_merger_t *self = calloc(1, sizeof(stream_merger_t));
	assert(self);
	self->wrk_dir = strdup(wrk_dir);
	assert(self->wrk_dir);
	heap_init(&self->heap);
	self->is = is;
	return self;
}

void free_stream_merger(stream_merger_t *self) {
	if (self) {
		heap_element_t *elem;
		if (self->wrk_dir)
			free(self->wrk_dir);
		foreach_heap_element(self->heap, elem) {
			struct _stream_merger_host *ctx = elem->data;
			if (ctx) {
				if (ctx->stream)
					free_serialized_msg_read_stream(ctx->stream);
				if (ctx->fd > 0)
					close(ctx->fd);
				free(ctx->hname);
				free(ctx);
			}
		}
		heap_destroy(&self->heap);
	}
}

void _stream_merger_host_dtor(struct _stream_merger_host *ctx) {
	if (ctx) {
		if (ctx->fd > 0)
			close(ctx->fd);
		if (ctx->hname)
			free(ctx->hname);
		free(ctx);
	}
}

int attach_input_to_stream_merger(stream_merger_t *self, const char *_file) {
	struct _stream_merger_host *ctx = calloc(1, sizeof(struct _stream_merger_host));
	int rv                          = 0;
	char *file;
	assert(ctx);
	assert((ctx->hname = strdup(_file)));
	file = strchr(ctx->hname, ':');
	if (!file) { /*Hostname not supplied*/
		rv = -EINVAL;
		goto err;
	}
	*(file++) = '\0';
	ctx->fd   = open(file, O_RDONLY);
	if (ctx->fd < 0) {
		rv = -errno;
		goto err;
	}
	assert((ctx->stream = init_serialized_msg_read_stream(ctx->fd, self->wrk_dir, self->is)));
	if ((rv = read_msg_deserialize(ctx->stream, &ctx->last)) < 0) {
		goto err;
	} else if (!rv) { /*eof directly on the first read*/
		close(ctx->fd);
		free(ctx);
	} else {
		heap_add(&self->heap, ctx->last.ts, ctx);
	}
	return 0;
err:
	_stream_merger_host_dtor(ctx);
	return rv;
}

binary_trace_t *peek_stream_merger(stream_merger_t *self) {
	struct _stream_merger_host *ctx = heap_peek_min(&self->heap).data;
	return &ctx->last;
}

const char *peek_stream_merger_hostname(stream_merger_t *self) {
	struct _stream_merger_host *ctx = heap_peek_min(&self->heap).data;
	return ctx->hname;
}

int stream_merger_is_eof(stream_merger_t *self) { return heap_empty(&self->heap); }

int pop_stream_merger(stream_merger_t *self) {
	struct _stream_merger_host *ctx = heap_pop_min(&self->heap).data;
	int rv;
	if ((rv = read_msg_deserialize(ctx->stream, &ctx->last)) <= 0) { /*eof or error*/
		_stream_merger_host_dtor(ctx);
	} else {
		heap_add(&self->heap, ctx->last.ts, ctx); /*resuse the ctx*/
	}
	return rv;
}
