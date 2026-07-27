#include "compressor.h"
#include <asm-generic/errno-base.h>
#include <assert.h>
#include <libgen.h>
#include <linux/limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "lz4frame_static.h"
#include "lz4file.h"

static const size_t CHUNK_SIZE = 16*1024;

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

struct lz4_compressor{
	struct compressor base;
	struct {
		size_t max_in_size;
		LZ4F_preferences_t lz4_preferences;
	} cfg;
	struct{
		// buffer for compressed data in process
		struct iovec buffer;

		// buffer that points into buffer, and holds the last compression data
		struct iovec last_compressed;

		struct LZ4F_cctx_s* lz4_ctx;
	} runtime;

};

#define BASE_TO_LZ4(base_comp) container_of(base_comp, struct lz4_compressor, base)

static struct compression_result __make_compression_result(struct lz4_compressor *compressor, size_t compressed_size)
{
	compressor->runtime.last_compressed =
		(struct iovec){ .iov_base = compressor->runtime.buffer.iov_base,
				.iov_len = compressed_size};

	return (struct compression_result){ .error = 0, .iovecs = &compressor->runtime.last_compressed, .n_iovecs = 1 };
}

static struct compression_result __make_error_compression_result(int error)
{
	return (struct compression_result){ .error = error, .iovecs = NULL, .n_iovecs = 0 };
}

static const char *__lz4_compressor_file_ext(void)
{
	return ".lz4";
}

static struct compression_result __lz4_compressor_start_write(struct compressor *base_compressor)
{
	struct lz4_compressor *compressor = BASE_TO_LZ4(base_compressor);
	__auto_type lz4_preferences = &(compressor->cfg.lz4_preferences);
	size_t const header_size = LZ4F_compressBegin(compressor->runtime.lz4_ctx, compressor->runtime.buffer.iov_base,
						      compressor->runtime.buffer.iov_len, lz4_preferences);
	if (LZ4F_isError(header_size)) {
		return __make_error_compression_result((int)header_size);
	}

	return __make_compression_result(compressor, header_size);
}

static struct compression_result __lz4_compressor_stop_write(struct compressor *base_compressor)
{
	struct lz4_compressor *compressor = BASE_TO_LZ4(base_compressor);
	size_t const capacity = compressor->runtime.buffer.iov_len;

	size_t const leftovers_size =
		LZ4F_compressEnd(compressor->runtime.lz4_ctx, compressor->runtime.buffer.iov_base, capacity, NULL);

	if (LZ4F_isError(leftovers_size)) {
		return __make_error_compression_result((int)leftovers_size);
	}

	return __make_compression_result(compressor, leftovers_size);
}

void __update_iovec_usage(struct iovec* iovec, size_t used){
	assert(iovec);
	assert(used < iovec->iov_len);
	iovec->iov_base += used;
	iovec->iov_len -= used;
}

struct compression_result __lz4_compressor_write(struct compressor *base_compressor, struct iovec *iovecs,
						       size_t n_iovecs)
{
	struct lz4_compressor *compressor = BASE_TO_LZ4(base_compressor);

	struct iovec iter = compressor->runtime.buffer;
	for(struct iovec* curr = iovecs; curr != iovecs + n_iovecs; ++curr){
		size_t const compressed_size 
	 		= LZ4F_compressUpdate(compressor->runtime.lz4_ctx
									, iter.iov_base, iter.iov_len
									, curr->iov_base, curr->iov_len
									, NULL);
		if (LZ4F_isError(compressed_size)){
			return __make_error_compression_result((int)compressed_size);
		}
		__update_iovec_usage(&iter, compressed_size);
	}

	size_t const total_compressed = compressor->runtime.buffer.iov_len - iter.iov_len; //total size - unused chunk

	return __make_compression_result(compressor, total_compressed);
}

static void __lz4_compressor_destroy(struct compressor* base_compressor){
	if(base_compressor){
		struct lz4_compressor *compressor = BASE_TO_LZ4(base_compressor);
		LZ4F_freeCompressionContext(compressor->runtime.lz4_ctx);
		free(compressor->runtime.buffer.iov_base);
		free(compressor);
	}
}


struct compressor *compressor_create_lz4(size_t max_in_size)
{
	struct lz4_compressor *compressor =
		(struct lz4_compressor *)calloc(1, sizeof(struct lz4_compressor));
	if (!compressor) {
		return NULL;
	}
	(*compressor) = (struct lz4_compressor) {
		.base  = {
			.ops = {
				.start = __lz4_compressor_start_write,
				.stop = __lz4_compressor_stop_write,
				.write = __lz4_compressor_write,
				.destroy = __lz4_compressor_destroy,
				.file_ext = __lz4_compressor_file_ext
			}
		},
		.cfg = {
			.max_in_size = max_in_size,
			.lz4_preferences = {
				.frameInfo = {
					.blockSizeID = LZ4F_max256KB,
					.blockMode = LZ4F_blockLinked,
					.contentChecksumFlag = LZ4F_noContentChecksum, // LZ4F_contentChecksumEnabled,
					.frameType = LZ4F_frame,
					.contentSize = 0 /* unknown content size */,
					.dictID = 0 /* no dictID */,
					.blockChecksumFlag = LZ4F_blockChecksumEnabled
				},
				.compressionLevel = 1,   /* compression level; 0 == default */
				.autoFlush = 1,   /* autoflush, needed to open files without stop */
				.favorDecSpeed = 0,   /* favor decompression speed */
				.reserved = { 0, 0, 0 },  /* reserved, must be set to 0 */
			}
		},
		.runtime = {
			.buffer = {.iov_base = 0, .iov_len = 0},
			.last_compressed = {.iov_base = 0, .iov_len = 0},
			.lz4_ctx = NULL
		}
	};

	LZ4F_errorCode_t lz4_ctx_rv = LZ4F_createCompressionContext(&compressor->runtime.lz4_ctx, LZ4F_VERSION);
	compressor->runtime.buffer.iov_len =
		LZ4F_HEADER_SIZE_MAX + LZ4F_compressBound(max_in_size, &compressor->cfg.lz4_preferences);
	compressor->runtime.buffer.iov_base = malloc(compressor->runtime.buffer.iov_len);
	compressor->runtime.last_compressed =
		(struct iovec){ .iov_base = compressor->runtime.buffer.iov_base, .iov_len = 0 };

	if (LZ4F_isError(lz4_ctx_rv) || !compressor->runtime.buffer.iov_base) {
		__lz4_compressor_destroy(&compressor->base);
		compressor = NULL;
	}

	return &compressor->base;
}

struct decompress_file_result __make_decompress_file_result_from_lz4(const char* func, int err)
{
	struct decompress_file_result result = {.rv = EIO, .error = {0}};
	snprintf(result.error, sizeof(result.error), "%s error: %d(%s)", func, err, LZ4F_getErrorName(err));
	return result;
}

struct decompress_file_result __make_decompress_file_result_from_errno(int err)
{
	struct decompress_file_result result = {.rv = err, .error = {0}};
	strncpy(result.error, strerror(err), sizeof(result.error) - 1);
	return result;
}

/* the most simplistic function to decompress a file, read the whole content, find the uncompress size and  decompress it */
struct decompress_file_result __lz4_decompress_file(FILE *f_in, FILE *f_out)
{
	LZ4F_errorCode_t ret = LZ4F_OK_NoError;
	LZ4_readFile_t *lz4f_read;
	void *buf = malloc(CHUNK_SIZE);
	struct decompress_file_result result = {0};

	assert(f_in != NULL);
	assert(f_out != NULL);
	
	if (!buf) {
		result = __make_decompress_file_result_from_errno(ENOMEM);
		goto out_direct;
	}

	ret = LZ4F_readOpen(&lz4f_read, f_in);
	if (LZ4F_isError(ret)) {
		result = __make_decompress_file_result_from_lz4("LZ4F_readOpen", ret);
		goto out_free;
	}

	while (1) {
		ret = LZ4F_read(lz4f_read, buf, CHUNK_SIZE);
		if (LZ4F_isError(ret)) {
			result = __make_decompress_file_result_from_lz4("LZ4F_read", ret);
			goto out;
		}

		/* nothing to read */
		if (ret == 0) {
			break;
		}

		if (fwrite(buf, 1, ret, f_out) != ret) {
			result = __make_decompress_file_result_from_errno(errno);
			goto out;
		}
	}

out:
	ret = LZ4F_readClose(lz4f_read);
	if (LZ4F_isError(ret)) {
		result = __make_decompress_file_result_from_lz4("LZ4F_readClose", ret);
	}
out_free:
	free(buf);
out_direct:
	return result;
}

struct decompress_file_result __lz4_decompress_fpath(struct fspath const* compressed_fpath, struct fspath const* decompressed_fpath){
	FILE *f_in = fopen(compressed_fpath->path, "r");
	if (!f_in) {
		return __make_decompress_file_result_from_errno(errno);
	}

	FILE *f_out = fopen(decompressed_fpath->path, "w+");
	if (!f_out) {
		struct decompress_file_result const result = __make_decompress_file_result_from_errno(errno);
		fclose(f_in);
		return result;
	}

	struct decompress_file_result const result = __lz4_decompress_file(f_in, f_out);
	fclose(f_in);
	fclose(f_out);

	if(result.rv){
		//also not a good solution - extraction should be done to a temp file on the same filesystem and then moved

		//rational - in case of "out of space" - the file will be already created and out.ctime > in.ctime
		//thus we may skip decompression in future and may lost few important traces.
		//in this case, let's just remove it and retry once again in future
		unlink(decompressed_fpath->path);
	}
	return result;
}

static int __ends_with(const char *str, const char *post) {
	size_t lenstr = strlen(str), lenpost = strlen(post);
	return lenstr < lenpost ? 0 : strcmp(str + lenstr - lenpost, post) == 0;
}

static inline struct fspath __create_destination_file_name(struct fspath const* compressed_fpath, struct fspath const* decompressed_dir_path)
{
	struct fspath base_path = *compressed_fpath;
	char *base_name = basename(base_path.path );
	if (__ends_with(base_name, __lz4_compressor_file_ext())) {
		base_name[strlen(base_name) - strlen(__lz4_compressor_file_ext())] = '\0'; // Remove .lz4 suffix
	}

	return fspath_create("%s/%s", decompressed_dir_path->path, base_name);
}

struct should_decompress_file_result{
	bool rv; //yes/no - valid only in case error == 0
	int error;
};

static inline struct should_decompress_file_result __should_decompress_file(struct fspath const* compressed, struct fspath const* decompressed) {
    struct stat cmpr, decmpr;

	// Check if the .lz4 file exists
    if (stat(decompressed->path, &decmpr) != 0) {
		if (errno == ENOENT){
			return (struct should_decompress_file_result){.rv = true, .error=0};
		} else {
			return (struct should_decompress_file_result){.rv = false, .error = errno};
		}
    }

    // Check if the original file exists
    if (stat(compressed->path, &cmpr) != 0) {
        return (struct should_decompress_file_result){.rv = false, .error = errno};
    }

    // Compare the last change dates
    return (struct should_decompress_file_result){.rv = cmpr.st_ctime >= decmpr.st_ctime, .error=0};
}

struct decompress_file_result decompress_file_if_needed(struct fspath const* compressed_fpath, struct fspath const* decompressed_dir_path)
{
	int const mkdir_rv = fspath_mkdir(decompressed_dir_path, 0755);
	if(mkdir_rv){
		return __make_decompress_file_result_from_errno(mkdir_rv);
	}

	struct fspath const decompressed_fpath = __create_destination_file_name(compressed_fpath, decompressed_dir_path);
	if (decompressed_fpath.error){
		return __make_decompress_file_result_from_errno(decompressed_fpath.error);
	}
	struct should_decompress_file_result stats_result = __should_decompress_file(compressed_fpath, &decompressed_fpath);
	if (stats_result.error){
		return __make_decompress_file_result_from_errno(stats_result.error);
	}
	if (stats_result.rv == true){
		struct decompress_file_result dresult = __lz4_decompress_fpath(compressed_fpath, &decompressed_fpath);
		if (dresult.rv){
			return dresult;
		}
	}
	return (struct decompress_file_result){.fpath = decompressed_fpath};
}

size_t iovecs_total_size(struct iovec *iovecs, size_t n_iovecs)
{
	size_t tot_size = 0;
	for (struct iovec *curr = iovecs; curr != iovecs + n_iovecs; ++curr) {
		tot_size += curr->iov_len;
	}
	return tot_size;
}
