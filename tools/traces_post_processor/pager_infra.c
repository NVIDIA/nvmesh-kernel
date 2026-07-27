#include <assert.h>
#include <dirent.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <libgen.h>
#include "trace_compress_lib/fspath.h"
#include "trace_compress_lib/compressor.h"

#include "pager_infra.h"

buf_pos_t INVALID_BUF_POS = {-1, -1, -1};

/* Dictionaries hash table definition */
unsigned long dict_pool_t_hash(const dict_pool_t *d) { return d->key & DICTS_TABLE_MASK; }
int dict_pool_t_eq(const dict_pool_t *d1, const dict_pool_t *d2) { return d1->key == d2->key; }
DEFINE_HASHTABLE_METHODS(dict_pool_t, DICTS_TABLE_BITS, );

/**
 * Return true if string @str starts with string @pre, false otherwise
 */
static int starts_with(const char *str, const char *pre) {
	size_t lenpre = strlen(pre), lenstr = strlen(str);
	return lenstr < lenpre ? 0 : strncmp(pre, str, lenpre) == 0;
}

static int ends_with(const char *str, const char *post) {
	size_t lenstr = strlen(str), lenpost = strlen(post);
	return lenstr < lenpost ? 0 : strcmp(str + lenstr - lenpost, post) == 0;
}

/**
 * Remove .lz4 suffix from filename, if it exists
 * @param fname Filename to modify
 */
static void __remove_compress_suffix_from_filename(char *fname) {
	if (ends_with(fname, ".lz4")) {
		fname[strlen(fname) - 4] = '\0'; // Remove .lz4 suffix
	}
}

/**
 * Parse log filename, for each file that matches correct pattern get CPU id and log id
 */
static int parse_log_filename(char *fname, const char *basename, cpu_id_t *cpu, file_id_t *idx) {
	if (!starts_with(fname, basename))
		return 0;
	else {
		size_t lenbasename = strlen(basename);
		*cpu = 0;
		*idx = 0;
		const char *sub = fname + lenbasename;
		while (*sub >= '0' && *sub <= '9') {
			*cpu = *cpu * 10 + *sub - '0';
			++sub;
		}
		if (*sub == '\0')
			return 0;
		if (*cpu >= MAX_CPUS)
			return 0;
		++sub;
		while (*sub >= '0' && *sub <= '9') {
			*idx = *idx * 10 + *sub - '0';
			++sub;
		}
		if (*sub != '\0')
			return 0;
		return 1;
	}
}

void init_store(channel_ctx_t *ctx, cpu_id_t cpu_id) {
	int i;
	ctx->stats_store[cpu_id].stats =
		calloc(1, sizeof(file_stats_t) * (ctx->stats_store[cpu_id].last - ctx->stats_store[cpu_id].first + 1));
	assert(ctx->stats_store[cpu_id].stats);
	for (i = 0; i < ctx->stats_store[cpu_id].last - ctx->stats_store[cpu_id].first + 1; ++i) {
		ctx->stats_store[cpu_id].stats[i].num_of_buffers = -1;
	}
}

void init_stores(channel_ctx_t *ctx) {
	DIR *dp;
	struct dirent *ep;
	int i;
	char *scratch1 = strdup(ctx->basename);
	char *scratch2 = strdup(ctx->basename);
	char *bname, *dname;

	for (i = 0; i < MAX_CPUS; ++i) {
		ctx->stats_store[i].first = MAX_LOG_ID;
		ctx->stats_store[i].last = 0;
		ctx->stats_store[i].stats = NULL; // We will allocate this later
	}

	bname = basename(scratch1);
	dname = dirname(scratch2);

	dp = opendir(dname);
	if (!dp) {
		fprintf(stderr, "Error listing working dir '%s'\n", dname);
		assert(0);
	}
	assert(dp);
	while ((ep = readdir(dp)) != NULL) {
		cpu_id_t cpu;
		file_id_t idx;

		__remove_compress_suffix_from_filename(ep->d_name);
		if (parse_log_filename(ep->d_name, bname, &cpu, &idx)) {
			if (idx < ctx->stats_store[cpu].first)
				ctx->stats_store[cpu].first = idx;
			if (idx > ctx->stats_store[cpu].last)
				ctx->stats_store[cpu].last = idx;
		}
	}
	closedir(dp);

	// Now allocate stores
	for (i = 0; i < MAX_CPUS; ++i) {
		if (ctx->stats_store[i].last >= ctx->stats_store[i].first)
			init_store(ctx, i);
	}

	free(scratch1);
	free(scratch2);
}

void destroy_stores(channel_ctx_t *ctx) {
	int i;
	for (i = 0; i < MAX_CPUS; ++i) {
		if (ctx->stats_store[i].stats != NULL)
			free(ctx->stats_store[i].stats);
		ctx->stats_store[i].stats = NULL;
	}
}
/**
 * For the given buf_pos returns the filepath.  
 */

struct fspath __get_file_path(channel_ctx_t *ctx, cpu_id_t cpu_id, file_id_t file_id) {
	struct fspath const fpath = fspath_create("%s%lld.%lld", ctx->basename, cpu_id, file_id);
	assert(fpath.error == 0);
	if (0 == access(fpath.path, F_OK)){
		return fpath;
	}
	
	struct fspath const fpath_compressed = fspath_create("%s%lld.%lld.lz4", ctx->basename, cpu_id, file_id);
	assert(fpath_compressed.error == 0);
	if (0 == access(fpath_compressed.path, F_OK)){
		return fpath_compressed;
	}

	return fpath;
}

FILE* __open_file(channel_ctx_t *ctx, cpu_id_t cpu_id, file_id_t file_id)
{
	struct fspath const fpath = __get_file_path(ctx, cpu_id, file_id);
	if (ends_with(fpath.path, ".lz4")){
		//temporal, the .cache should be taken from the arguments
		struct fspath const fdir = fspath_dirname(&fpath);
		struct fspath const decompressed_dir = fspath_create("%s/.cache", fdir.path);
		struct decompress_file_result result = decompress_file_if_needed(&fpath, &decompressed_dir);
		if (result.rv){
			_verbprint(stderr, "Failed to decompress file; error=%d(%s), path=%s", result.rv, result.error, fpath.path);
			return NULL;
		}
		return fopen(result.fpath.path, "r");
	} else {
		return fopen(fpath.path, "r");
	}
}

buf_id_t get_bufs_in_file(channel_ctx_t *ctx, cpu_id_t cpu_id, file_id_t file_id) {
	FILE *fp = __open_file(ctx, cpu_id, file_id);
	if (!fp)
		return 0;
	fseek(fp, 0L, SEEK_END);
	buf_id_t const sz = ftell(fp);
	fclose(fp);
	return sz / ctx->buf_size;
}

file_stats_t *fetch_stats(channel_ctx_t *ctx, cpu_id_t cpu_id, file_id_t file_id) {
	file_stats_t *stats = &ctx->stats_store[cpu_id].stats[file_id - ctx->stats_store[cpu_id].first];
	if (stats->num_of_buffers == -1) // Uninitialized => Have to initialize
		stats->num_of_buffers = get_bufs_in_file(ctx, cpu_id, file_id);
	return stats;
}

int buf_pos_gt(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2) {
	assert(b1.cpu_id == b2.cpu_id);
	return b1.file_id > b2.file_id || (b1.file_id == b2.file_id && b1.buf_id > b2.buf_id);
}

int buf_pos_ge(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2) {
	assert(b1.cpu_id == b2.cpu_id);
	return b1.file_id > b2.file_id || (b1.file_id == b2.file_id && b1.buf_id >= b2.buf_id);
}

int buf_pos_eq(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2) {
	assert(b1.cpu_id == b2.cpu_id);
	return b1.file_id == b2.file_id && b1.buf_id == b2.buf_id;
}

buf_pos_t get_first_valid_pos(channel_ctx_t *ctx, cpu_id_t cpu_id) {
	file_id_t first = ctx->stats_store[cpu_id].first;
	file_stats_t *stats = fetch_stats(ctx, cpu_id, first);
	while (stats->num_of_buffers <= 0) {
		++first;
		if (first > ctx->stats_store[cpu_id].last)
			return INVALID_BUF_POS;
		stats = fetch_stats(ctx, cpu_id, first);
	}
	return (buf_pos_t){.cpu_id = cpu_id, .file_id = first, .buf_id = 0};
}

buf_pos_t get_last_valid_pos(channel_ctx_t *ctx, cpu_id_t cpu_id) {
	file_id_t last = ctx->stats_store[cpu_id].last;
	file_stats_t *stats = fetch_stats(ctx, cpu_id, last);
	while (stats->num_of_buffers <= 0) {
		--last;
		if (last < ctx->stats_store[cpu_id].first)
			return INVALID_BUF_POS;
		stats = fetch_stats(ctx, cpu_id, last);
	}
	return (buf_pos_t){.cpu_id = cpu_id, .file_id = last, .buf_id = stats->num_of_buffers - 1};
}

buf_id_t get_bufs_distance(channel_ctx_t *ctx, buf_pos_t b1, buf_pos_t b2) {
	buf_id_t dst = 0;
	assert(buf_pos_ge(ctx, b2, b1));
	while (b1.file_id != b2.file_id) {
		file_stats_t *stats = fetch_stats(ctx, b1.cpu_id, b1.file_id);
		dst += (stats->num_of_buffers - b1.buf_id);
		++b1.file_id;
		b1.buf_id = 0;
	}
	// Here we know b1.file_id == b2.file_id
	dst += b2.buf_id - b1.buf_id;
	return dst;
}

buf_pos_t move_position_right(channel_ctx_t *ctx, buf_pos_t pos, buf_id_t offset) {
	buf_pos_t npos = pos; // New position
	buf_pos_t last = get_last_valid_pos(ctx, pos.cpu_id);
	while (offset > 0) {
		file_stats_t *stats = fetch_stats(ctx, npos.cpu_id, npos.file_id);
		if (offset <= (stats->num_of_buffers - npos.buf_id - 1)) {
			npos.buf_id += offset;
			return npos;
		} else {
			if (npos.file_id >= last.file_id) {
				// Last file, cannot move any further.
				return last;
			}
			offset -= (stats->num_of_buffers - npos.buf_id);
			npos.file_id += 1;
			npos.buf_id = 0;
		}
	}
	return npos;
}

buf_pos_t move_position_left(channel_ctx_t *ctx, buf_pos_t pos, buf_id_t offset) {
	buf_pos_t npos = pos; // New position
	buf_pos_t first = get_first_valid_pos(ctx, pos.cpu_id);
	while (offset > 0) {
		if (offset <= npos.buf_id) {
			npos.buf_id -= offset;
			return npos;
		} else {
			if (npos.file_id <= first.file_id) {
				// First file, cannot move any further.
				return first;
			}
			offset -= npos.buf_id;
			npos.file_id -= 1;
			npos.buf_id = fetch_stats(ctx, npos.cpu_id, npos.file_id)->num_of_buffers;
		}
	}
	return npos;
}

/**
 * Find an undamaged buffer around @pos, within limits of @right and @left
 */
buf_pos_t _get_mid_undamaged_pos(channel_ctx_t *ctx, buf_pos_t mid, buf_pos_t left, buf_pos_t right) {
	// We first look at the buffer and see whether or not it is damaged.
	// If it is - we will keep looking at mid + 1 and mid - 1.
	// If still damaged - look and mid + 2 and mid - 2, and so on.
	// If reached both edges - return left (convention).
	int deviation = 1;
	assert(buf_pos_ge(ctx, mid, left) && buf_pos_ge(ctx, right, mid));
	// Mid is ok - just return
	if (peek_timestamp(ctx, mid) != -1)
		return mid;

	// Mid is not ok ...
	while (1) {
		buf_pos_t ml = move_position_left(ctx, mid, deviation);
		buf_pos_t mr = move_position_right(ctx, mid, deviation);

		// Reached both edges
		if (buf_pos_ge(ctx, left, ml) && buf_pos_ge(ctx, mr, right))
			return left;

		// If found good position
		if (peek_timestamp(ctx, ml) != -1)
			return ml;
		if (peek_timestamp(ctx, mr) != -1)
			return mr;

		++deviation;
	}
}

buf_pos_t get_middle_pos(channel_ctx_t *ctx, buf_pos_t left, buf_pos_t right) {
	// Simplest way to implement this function is by brute force, calling:
	// return move_position_right(ctx, left, get_bufs_distance(ctx, left, right) / 2);
	// We don't want to do this, since we want to minimize the number of files we access
	// before finding the correct one. Hence we will try to give a rough approximation by
	// finding the middle file and getting its middle buffer. Problem is - nobody
	// said all files in beween exist, or contain buffers, or not corrupted.
	buf_pos_t mid;
	assert(left.cpu_id == left.cpu_id);
	mid.cpu_id = left.cpu_id;
	if (left.file_id == right.file_id) {
		// Same file - return middle buffer
		mid.file_id = left.file_id;
		mid.buf_id = (left.buf_id + right.buf_id) / 2;
		return mid;
	} else {
		// Not same file.
		file_id_t midfile = (left.file_id + right.file_id) / 2;
		int deviation = 0;
		// We are looking for some file between left and right. Problem is - some of these files may not exist.
		// So we start with the middle, and if the file does not exist, we are looking on files close to middle.
		// Then we keep looking further and further from middle, until:
		// 1. We find some file.
		// 2. We are close to edge and it is easier to calculate exact distance now.
		// If we found something - great. If we reached an edge we will count the exact amount of buffers between
		// left and right, and return the middle (because in this case we know all the stats in between).
		while (midfile + deviation < right.file_id - 1 && midfile - deviation > left.file_id) {
			file_stats_t *stats = fetch_stats(ctx, mid.cpu_id, midfile + deviation);
			if (stats->num_of_buffers > 0) {
				mid.file_id = midfile + deviation;
				mid.buf_id = stats->num_of_buffers / 2;
				return _get_mid_undamaged_pos(ctx, mid, left, right);
			}
			stats = fetch_stats(ctx, mid.cpu_id, midfile - deviation);
			if (stats->num_of_buffers > 0) {
				mid.file_id = midfile - deviation;
				mid.buf_id = stats->num_of_buffers / 2;
				return _get_mid_undamaged_pos(ctx, mid, left, right);
			}
			++deviation;
		}
		// We are here if we reached an edge.
		mid = move_position_right(ctx, left, get_bufs_distance(ctx, left, right) / 2);
		return _get_mid_undamaged_pos(ctx, mid, left, right);
	}
}

timestamp_raw_t read_buf_timestamp(void *buf) {
	return read_timestamp(buf + BUFFER_HEADER_SIZE, 0);
}

timestamp_t read_buf_timestamp_ns(channel_ctx_t *ctx, void *buf) {

	timestamp_raw_t ts; buf_header_t hdr;
	read_buf_header(buf, &hdr);
	if (!get_dict(ctx, hdr.cksum)) {
		_verbprint(stderr, "Could not find the dictionary with signature 0x%x\n", hdr.cksum);
		return -1;
	}
	ts = read_buf_timestamp(buf);
	if (ts == -1)
		return -1;

	return tsc_to_ns(ts, hdr.khz);
}

timestamp_raw_t read_timestamp(void *buf, timestamp_raw_t prev) {
	unsigned ts32;
	unsigned long ts64;

	ts32 = *(unsigned int *)(buf);
	if ((ts32 & 1) == 0) { // compressed timestamp
		if (!prev) {
			_verbprint(stderr, "Compressed timestamp at start of buffer\n");
			return -1;
		}
		ts32 >>= 1;
		if ((prev & 0x7fffffff) <= ts32)
			ts64 = (prev & ~0x7fffffff) | ts32;
		else
			ts64 = ((prev & ~0x7fffffff) | ts32) + (1UL<<31);
	} else
		ts64 = *(unsigned long *)(buf) >> 1;

	return ts64;
}

FILE *fopen_at_buf_pos(channel_ctx_t *ctx, buf_pos_t pos) {
	FILE *fp = __open_file(ctx, pos.cpu_id, pos.file_id);
	if (!fp)
		return NULL;

	if (fseek(fp, pos.buf_id * ctx->buf_size, SEEK_SET)) {
		fclose(fp);
		return NULL; // Oops, cannot seek to buffer
	}
	return fp;
}

timestamp_t peek_timestamp(channel_ctx_t *ctx, buf_pos_t pos) {
	char buf_header[BUFFER_HEADER_SIZE + 8]; // Should be enought to read largest full buffer header + timestamp
	FILE *fp = fopen_at_buf_pos(ctx, pos);
	if (!fp) {
		fprintf(stderr, "Open failed\n");
		dbgprintpos(stderr, pos);
		return -1;
	}
	if (fread(buf_header, sizeof(buf_header), 1, fp) != 1) {
		fprintf(stderr, "Read failed\n");
		dbgprintpos(stderr, pos);
		fclose(fp);
		return -1; // Oops, cannot read from file
	}
	fclose(fp);
	return read_buf_timestamp_ns(ctx, buf_header);
}

timestamp_t peek_tail_timestamp_cpu(channel_ctx_t *ctx, cpu_id_t cpu_id) {
	buf_pos_t pos = get_last_valid_pos(ctx, cpu_id);
	if (pos.cpu_id == -1) // No buffers
		return -1;
	return peek_timestamp(ctx, pos);
}

timestamp_t peek_tail_timestamp(channel_ctx_t *ctx) {
	int i;
	timestamp_t max = 0;
	for_each_active_cpu(ctx, i) {
		timestamp_t ts = peek_tail_timestamp_cpu(ctx, i);
		if (ts == -1)
			continue;
		if (ts > max)
			max = ts;
	}
	return max;
}

buf_pos_t find_ts_pos(channel_ctx_t *ctx, timestamp_t ts, buf_pos_t start, buf_pos_t end, int interval_edge) {
	// It is a variation of binary search, except that we cannot assume that the input is 100% sorted.
	// For each ts[i] we can only say for sure that ts[i-ctx->safe_offset] < ts[i] and ts[i+ctx->safe_offset] > ts[i],
	// but inside the interval of ctx->safe_offset-1 around i, it is possible timestamps are not sorted.
	buf_pos_t mid = get_middle_pos(ctx, start, end);
	timestamp_t midts;
	assert(start.cpu_id != -1 && end.cpu_id != -1); // Should not be here if go invalid position

	// Stop condition is getting to a segment too narrow to be guarantee any data sorting,
	// that is segment of width 2*ctx->safe_offset or less.
	// However, we would not want to calculate the exact segment width each time, as it is
	// same as calling stat on each trace file. Out target is to minimize stat calls.
	// So we will calculate the exact segment width only when we know that the segment is,
	// somewhat narrow narrow, that is if and only if midpoint is either in the
	// same file as start or as end point.
	if (mid.file_id == start.file_id || mid.file_id == end.file_id) {
		// We are here if the segment is pretty narrow already, measure exact width now.
		if (get_bufs_distance(ctx, start, end) <= 2 * ctx->safe_offset)
			// Return start segment. We cannot get better approximation without sacrificing accuray.
			return interval_edge ? end : start;
	}
	// Peek mid timestamp
	if ((midts = peek_timestamp(ctx, mid)) == -1) return interval_edge ? end : start;
	if (ts < midts) { // Target ts is before midpoint
		// We can say for sure target is before midpoint + safe_offset
		buf_pos_t shift = move_position_right(ctx, mid, ctx->safe_offset);
		if (buf_pos_ge(ctx, shift, end)) {
			// Oops, interval is too narrow. We cannot improve accuracy any further. Return safe value.
			return interval_edge ? end : start;
		}
		return find_ts_pos(ctx, ts, start, shift, interval_edge);

	} else { // Target ts is after before midpoint (or at midpoint)
		// We can say for sure target is after midpoint - safe_offset
		buf_pos_t shift = move_position_left(ctx, mid, ctx->safe_offset);
		if (buf_pos_ge(ctx, start, shift)) {
			// Oops, interval is too narrow. We cannot improve accuracy any further. Return safe value.
			return interval_edge ? end : start;
		}
		return find_ts_pos(ctx, ts, shift, end, interval_edge);
	}
}

channel_ctx_t *init_channel_ctx(const char *basename, const char *hostname, int buf_size, int safe_offset, immutable_string_store_t *is) {
	channel_ctx_t *ctx = calloc(1, sizeof(channel_ctx_t));

	ctx->basename = strdup(basename);
	ctx->hostname = get_immutable_string(is, hostname);
	ctx->buf_size = buf_size;
	ctx->safe_offset = safe_offset;
	ctx->is = is;

	dict_pool_t_ht_init(&ctx->dicts);

	init_stores(ctx);

	return ctx;
}

dict_t *try_attach_dict_to_pool(struct dict_pool_t_ht *pool, const char *dict, const char *fmtlib) {
	dict_t *d = init_dict(dict, fmtlib);
	if (!d) return NULL;
	else {
		dict_pool_t *dp = calloc(1, sizeof(dict_pool_t));
		assert(dp);
		dp->key = dict_cksum(d);
		dp->value = d;
		if (dict_pool_t_ht_insert(pool, dp)) { /*@todo: should be hard assertion*/
			fprintf(stderr, "WARNING: dictionary %u supplied twice\n", dp->key);
			free(dp);
			free_dict(d);
			return NULL;
		}
	}
	return d;
}

const char *_fmt_fmtlib_filename(const char *dir) {
	static char buf[MAX_FILENAME];
	snprintf(buf, sizeof(buf), "%s/libfmtrs.so", dir);
	return buf;
}

const char *_fmt_dict_filename(const char *dir, cksum_t cksum) {
	static char buf[MAX_FILENAME];
	snprintf(buf, sizeof(buf), "%s/dict.%u.json", dir, cksum);
	return buf;
}

dict_t *get_dict_from_pool(struct dict_pool_t_ht *pool, cksum_t cksum, const char *wrk_dir) {
	/* A little dirty casting int* to dict_t*,
	   but I know for sure only first 4 bytes will be used here so yeah... */
	dict_pool_t *dp = dict_pool_t_ht_retrieve(pool, (dict_pool_t *)&cksum);
	if (!dp) { /* Dictionary is not in the pool - try to locate it */
		dict_t *d = try_attach_dict_to_pool(pool, _fmt_dict_filename(wrk_dir, cksum), _fmt_fmtlib_filename(wrk_dir));
		if (!d) { /* Could not locate - invalidate it */
			dp = calloc(1, sizeof(dict_pool_t));
			assert(dp);
			dp->key = cksum;
			dp->value = NULL;
			dict_pool_t_ht_insert(pool, dp);
		} else {
			return d;
		}
	}
	return dp->value;
}

dict_t *get_dict(channel_ctx_t *ctx, cksum_t cksum) {
	static char dirbuf[MAX_FILENAME];
	char *dir;
	strncpy(dirbuf, ctx->basename, sizeof(dirbuf) - 1);
	dir = dirname(dirbuf);
	return get_dict_from_pool(&ctx->dicts, cksum, dir);
}

void _dict_pool_destructor(dict_pool_t *dp){
	if (dp->value) free_dict(dp->value);
	free(dp);
}

void free_dict_pool(struct dict_pool_t_ht *pool) {
	dict_pool_t_ht_doall(pool, _dict_pool_destructor);
	dict_pool_t_ht_free(pool);
}

void free_channel_ctx(channel_ctx_t *ctx) {
	destroy_stores(ctx);
	free_dict_pool(&ctx->dicts);
	free(ctx->basename);
	free(ctx);
}

void read_buf_header(void *buf, buf_header_t *hdr) {
	/* Yes, I know, it is dirty to read struct field by field.
	Why don't I just read the whole struct at once? Portability.
	On the writer side the header is not written all at once,
	but gradually. Possibly with very large delay intervals.
	Simply because writer does not have all
	this info at once. Hence there is no real way to ensure its
	alignment fits the struct layout. */
	// hdr->serial = *((int *)buf) & 0xffffff;
	// hdr->flags = *((unsigned int *)buf) >> 24;
	// hdr->khz = *((unsigned int *)(buf + 4));
	// hdr->cksum = *((unsigned int *)(buf + 8));
	*hdr = *((buf_header_t *)buf);
}
