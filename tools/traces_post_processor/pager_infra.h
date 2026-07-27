#ifndef PAGER_INFO_H
#define PAGER_INFO_H

#include "pager_hashtable.h"
#include "formatter.h"
#include <stdio.h>

#define MAX_CPUS 256
#define BUFFER_SIZE 4096
#define RINGBUFFER_SIZE 1024
#define MAX_LOG_ID INT_MAX
#define BUFFER_HEADER_SIZE sizeof(buf_header_t)
#define DICTS_TABLE_BITS 4
#define DICTS_TABLE_MASK ((1 << DICTS_TABLE_BITS) - 1)

#define BUF_POS_FMT "`%s%llu.%llu+0x%llx`"
#define BUF_POS_ARG(ctx, pos) (ctx)->basename, (pos).cpu_id, (pos).file_id, (pos).buf_id * ctx->buf_size

#define BUF_POS_FMT_SHORT "%02llu.%02llu+0x%05llx"
#define BUF_POS_ARG_SHORT(ctx, pos) (pos).cpu_id, (pos).file_id, (pos).buf_id * ctx->buf_size

typedef long long cpu_id_t;
typedef long long file_id_t;
typedef long long buf_id_t;

typedef unsigned long long timestamp_t; /* Timestamp in NS since epoch */
typedef unsigned long long timestamp_raw_t; /* Raw timestamp in CPU ticks */

typedef int cksum_t;

typedef unsigned short trace_id_t;

/**
 * An element in dictionaries pool
 * @todo: maybe move all these definitions to a separate unit
 */
typedef struct dict_pool {
	cksum_t key;
	dict_t *value;
} dict_pool_t;

DECLARE_HASHTABLE_TYPE(dict_pool_t, DICTS_TABLE_BITS);
unsigned long dict_pool_t_hash(const dict_pool_t *d);
int dict_pool_t_eq(const dict_pool_t *d1, const dict_pool_t *d2);
DECLARE_HASHTABLE_METHODS(dict_pool_t, );
/**
 * Dict pool deep destructor
 */
void free_dict_pool(struct dict_pool_t_ht *dp);
/**
 * Attach a dictionary file and formatters lib to the given channel
 * Dictionary is mandatory. Formatters lib is best effort
 * @return New dictionary object on success
 * @assert Dictionary is not in in the pool already
 */
dict_t *try_attach_dict_to_pool(struct dict_pool_t_ht *pool, const char *dict, const char *fmtlib);
/**
 * Get dict from pool or NULL. Try to load if possible.
 */
dict_t *get_dict_from_pool(struct dict_pool_t_ht *pool, cksum_t cksum, const char *wrk_dir);

/**
 * Describes the metadata required for one given file.
 */
typedef struct file_stats {
	buf_id_t num_of_buffers;
} file_stats_t;

/**
 * A store for all metadata of all files for single CPU.
 * Currenty implemented as array. TODO: Consider making it hashtable or tree.
 */
typedef struct file_stats_store {
	file_id_t first;	 // First file index for given CPU.
	file_id_t last;		 // Last file index for given CPU.
	file_stats_t *stats; // Actual info for each file
} file_stats_store_t;

/**
 * Describes position in logs by file index and offset in terms of buffers
 */
typedef struct buf_pos {
	cpu_id_t cpu_id;   // CPU id
	file_id_t file_id; // Zero based file index
	buf_id_t buf_id;   // Zero based buffer index inside the file
} buf_pos_t;

/**
 * Standard buffer header up to the first message
 */
typedef struct buf_header {
	int serial : 24;
	unsigned int flags : 8;
	unsigned int khz : 32;
	cksum_t cksum : 32;
} __attribute__((packed)) buf_header_t;

extern buf_pos_t INVALID_BUF_POS;

static inline void dbgprintpos(FILE *f, buf_pos_t pos) {
	fprintf(f, "buf(cpu=%lld,file=%lld,buf=%lld)", pos.cpu_id, pos.file_id, pos.buf_id);
}

/**
 * Holds global metadata for current pager.
 * There is one ctx per traces channel.
 */
typedef struct channel_ctx {
	int buf_size;
	int safe_offset;
	char *basename; //slightly misleading, as it may contain the absolute fs path - follow pager.py log_dirs argument
	immutable_string_t *hostname;

	file_stats_store_t stats_store[MAX_CPUS]; // Stats store used to quickly navigate between buffers
	
	struct dict_pool_t_ht dicts; // Hash table containing all dictionaries available by checksum as a key
	void *payload; // Additional info attached to pager ctx
	int force_success; // Indicates whether we should fail on algorithm breaking errors or simply brute force return
					   // everything.
	immutable_string_store_t *is; /* Global immutable strings store reference */

	int do_statistics; /* Whether or not pager shall collect statistics */

} channel_ctx_t;

#define if_not_force_assert(ctx, cond)                                                                                 \
	if (cond) {                                                                                                        \
	} else if (!ctx->force_success) {                                                                                  \
		assert(0);                                                                                                     \
	} else

/**
 * Create and initialize pager context object
 */
channel_ctx_t *init_channel_ctx(const char *basename, const char *host_name, int buf_size, int safe_offset, immutable_string_store_t *is);

/**
 * Get dictionary by checksum or NULL if not found
 */
dict_t *get_dict(channel_ctx_t *ctx, cksum_t cksum);

/**
 * Destroy pager context object
 */
void free_channel_ctx(channel_ctx_t *ctx);

/**
 * Initialize file stats store for one given CPU
 */
void init_store(channel_ctx_t *ctx, cpu_id_t cpu_id);

/**
 * Initialize file stats stores for each existing CPU
 */
void init_stores(channel_ctx_t *ctx);

/**
 * Init store
 */
void destroy_stores(channel_ctx_t *ctx);

/**
 * Return the number of full buffers in file omiting partial/damaged buffers
 * TODO: Do something about partial/damaged buffers. Currenly we are losing them, but we probably dont want to.
 */
buf_id_t get_bufs_in_file(channel_ctx_t *ctx, cpu_id_t cpu_id, file_id_t file_id);

file_stats_t *fetch_stats(channel_ctx_t *ctx, cpu_id_t cpu_id, file_id_t file_id);

/**
 * Returns 1 if @b1 > @b2, 0 otherwise. Assume same CPU.
 */
int buf_pos_gt(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2);

/**
 * Returns 1 if @b1 >= @b2, 0 otherwise. Assume same CPU.
 */
int buf_pos_ge(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2);

/**
 * Returns 1 if @b1 == @b2, 0 otherwise. Assume same CPU.
 */
int buf_pos_eq(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2);

/**
 * Retrieve the first readable buffer position for given cpu or INVALID_BUF_POS if none found
 */
buf_pos_t get_first_valid_pos(channel_ctx_t *ctx, cpu_id_t cpu_id);

/**
 * Retrieve the last readable buffer position for given cpu or INVALID_BUF_POS if none found
 */
buf_pos_t get_last_valid_pos(channel_ctx_t *ctx, cpu_id_t cpu_id);

/**
 * Returns the distance (in terms of buffers) between b1 and b2.
 * WARNING: do not use unless b1 and b2 are very close or
 * all files in between dont exist, since it will stat any file
 * between the two positions (which may be slow if there are many files).
 * Assume b2 > b1
 */
buf_id_t get_bufs_distance(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2);

/**
 * Move buffer position by @offset in buffer units in direction towards future
 * in time. Offset must be positive or 0. If moved after last buffer return last buffer
 */
buf_pos_t move_position_right(channel_ctx_t *ctx, buf_pos_t pos, buf_id_t offset);

/**
 * Move buffer position by @offset in buffer units in direction towards past in
 * time. Offset must be positive or 0. If moved before first buffer return first buffer
 */
buf_pos_t move_position_left(channel_ctx_t *ctx, buf_pos_t pos, buf_id_t offset);

/**
 * Some shortcuts for buffers navigation
 */
#define MOVEBUF(ctx, pos, dir) move_position_##dir(ctx, pos, 1)
#define NEXTBUF(ctx, pos) MOVEBUF(ctx, pos, right)
#define PREVBUF(ctx, pos) MOVEBUF(ctx, pos, left)

/**
 * Return a position approximatelly in the middle between @left and @right.
 * The position is guaranteed to be existant. Assume same CPU.
 * Assume both @left and @right are valid.
 */
buf_pos_t get_middle_pos(channel_ctx_t *ctx, buf_pos_t left, buf_pos_t right);

/**
 * Demangle timestamp from binary data buffer start.
 * Prev timestamp = 0 means buffer start
 */
timestamp_raw_t read_buf_timestamp(void *buf);

/**
 * Read buffer timestamp in nanoseconds (using channel meta)
 */
timestamp_t read_buf_timestamp_ns(channel_ctx_t *ctx, void *buf);

/**
 * Demangle timestamp from binary data buffer start.
 * Prev timestamp = 0 means buffer start
 */
timestamp_raw_t read_timestamp(void *buf, timestamp_raw_t prev);

/**
 * decompress file if required.
 * Returns -1 on failure.
 */
int channel_decompress_file(char namebuf[MAX_FILENAME]);

/**
 * decompress file if required.
 * Returns -1 on failure.
 */
int channel_decompress(channel_ctx_t *ctx, cpu_id_t cpu_id, file_id_t file_id);

/**
 * Open file, seek to position and return file descriptor.
 * Returns NULL on failure.
 */
FILE *fopen_at_buf_pos(channel_ctx_t *ctx, buf_pos_t pos);

/**
 * Read a buffer timestamp at position @pos.
 * @return: timestamp on succees, -1 on failure (timestamp = -1 is invalid).
 */
timestamp_t peek_timestamp(channel_ctx_t *ctx, buf_pos_t pos);

/**
 * Attempt to get approximate timestamp of the latest buffer for CPU.
 * Actual value may be SMALLER then the real tail timestamp, hence
 * it is a bad idea to use it as an END point. It is a good idea
 * though to to roll it in backward X seconds direction and use the result
 * as a START pointm if we want to see at least X last seconds.
 * Return -1 on failure.
 */
timestamp_t peek_tail_timestamp_cpu(channel_ctx_t *ctx, cpu_id_t cpu_id);

/**
 * Return minimum of all peek_tail_timestamp_cpu for each active CPU.
 */
timestamp_t peek_tail_timestamp(channel_ctx_t *ctx);

/**
 * Find a safe position of timestamp @ts inside [@start,@end] segment
 * @interval_edge = 0 means @ts indicates interval start, else interval end
 * When talking about interval start, this function ensures that an interval
 * starting with @return value will not miss any timestamp > @ts.
 * When talking about interval end, this function ensures that an interval
 * ending with @return value will not miss any timestamp < @ts.
 */
buf_pos_t find_ts_pos(channel_ctx_t *ctx, timestamp_t ts, buf_pos_t start, buf_pos_t end, int interval_edge);

/**
 * Read standard buffer header up to the first message
 */
void read_buf_header(void *buf, buf_header_t *hdr);

/**
 * Aliases for find_ts_pos
 */
#define find_start_ts_pos(ctx, ts, cpu)                                                                                \
	find_ts_pos(ctx, ts, get_first_valid_pos(ctx, cpu), get_last_valid_pos(ctx, cpu), 0)
#define find_end_ts_pos(ctx, ts, cpu)                                                                                  \
	find_ts_pos(ctx, ts, get_first_valid_pos(ctx, cpu), get_last_valid_pos(ctx, cpu), 1)

/**
 * Utility
 */
#define for_each_active_cpu(ctx, cpu_id)                                                                               \
	for (cpu_id = 0; cpu_id < MAX_CPUS; ++cpu_id)                                                                      \
		if (ctx->stats_store[cpu_id].stats)

#endif /*PAGER_INFRA_H*/
