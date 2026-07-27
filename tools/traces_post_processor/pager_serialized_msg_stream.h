#ifndef PAGER_SERIALIZED_MSG_STREAM
#define PAGER_SERIALIZED_MSG_STREAM

#include "pager_msg_stream.h"

#define TRANSFER_BUFFER_SIZE (16*4096) /*From linux fifo size*/


/**************
 * WRITE STREAM
 *************/


struct serialized_msg_write_stream;
typedef struct serialized_msg_write_stream serialized_msg_write_stream_t;

/**
 * Initialize write stream to a given handle.
 * Return write ready object or NULL.
 */
serialized_msg_write_stream_t *init_serialized_msg_write_stream(int fd);

/**
 * Deestroy write stream. Does nothing to fd.
 */
void free_serialized_msg_write_stream(serialized_msg_write_stream_t *self);

/**
 * Write a single message to the stream.
 */
int write_msg_serialize(serialized_msg_write_stream_t *self, binary_trace_t *bt);

/**
 * Flush the write stream.
 */
int write_msg_flush(serialized_msg_write_stream_t *self);



/*************
 * READ STREAM
 ************/


struct serialized_msg_read_stream;
typedef struct serialized_msg_read_stream serialized_msg_read_stream_t;

/**
 * Initialize read stream to a given handle.
 * Return write ready object or NULL.
 */
serialized_msg_read_stream_t *init_serialized_msg_read_stream(int fd, const char *wrk_dir, immutable_string_store_t *is);

/**
 * Deestroy read stream. Does nothing to fd.
 */
void free_serialized_msg_read_stream(serialized_msg_read_stream_t *self);

/**
 * Read a single message from the stream.
 * Put the result inside bt.
 * @return 1 on success, 0 on eof or negative error code on error.
 */
int read_msg_deserialize(serialized_msg_read_stream_t *self, binary_trace_t *bt);

/**
 * Returns non-zero if eof
 */
int is_serialized_msg_read_stream_eof(serialized_msg_read_stream_t *self);


/***************
 * STREAM MERGER
 **************/

struct stream_merger;
typedef struct stream_merger stream_merger_t;

/**
 * Initialize the merger ctx
 */
stream_merger_t *init_stream_merger(const char *wrk_dir, immutable_string_store_t *is);

/**
 * Destroy stream mereger
 */
void free_stream_merger(stream_merger_t *self);

/**
 * Adds a file to stream merger
 */
int attach_input_to_stream_merger(stream_merger_t *self, const char *file);

/**
 * Get the head binary trace from the stream merger
 * @note Does not read any new data, cannot fail unless arguments are invalid
 */
binary_trace_t *peek_stream_merger(stream_merger_t *self);

/**
 * Peek head msg hostname
 */
const char *peek_stream_merger_hostname(stream_merger_t *self);

/**
 * Whether or not eof reached on all streams
 */
int stream_merger_is_eof(stream_merger_t *self);

/**
 * Remove the head trace from merger and read a new one if available
 * @return 1 on success, 0 on eof, negative value on error
 */
int pop_stream_merger(stream_merger_t *self);

/**
 * Destructor to use with __cleanup__ attribute
 */
static inline void cleanup_stream_merger(stream_merger_t **self) {
    if (*self) {
        free_stream_merger(*self);
        *self = NULL;
    }
}
#define _autoclean_stream_merger __attribute__ ((__cleanup__(cleanup_stream_merger)))

#endif /*PAGER_SERIALIZED_MSG_STREAM*/