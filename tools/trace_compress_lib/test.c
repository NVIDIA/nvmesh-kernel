#include "compressor.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t start_write_string(struct compressor *cmprs, int fd)
{
	ssize_t written;
	struct compression_result cmpr_rv = cmprs->ops.start(cmprs);
	assert(cmpr_rv.error == 0);
	assert(cmpr_rv.n_iovecs == 1);
	assert(cmpr_rv.iovecs != NULL);

	written = write(fd, cmpr_rv.iovecs[0].iov_base, cmpr_rv.iovecs[0].iov_len);
	assert(written == cmpr_rv.iovecs[0].iov_len);

	return written;
}

static size_t cat_string(struct compressor *cmprs, uint8_t *text_in, size_t len_in, uint32_t chars_in_iov, int fd_out)
{
	size_t written;
	uint32_t indx, n_iovecs = len_in  / chars_in_iov + 1; // Calculate number of iovecs needed
	struct compression_result cmpr_rv;
	struct iovec iovs[n_iovecs];

	for (indx = 0; indx < n_iovecs; ++indx) {
		iovs[indx] = (struct iovec){
			.iov_base = text_in + indx * chars_in_iov,
			.iov_len = (indx == n_iovecs - 1) ? (len_in - indx * chars_in_iov) : chars_in_iov
		};
	}
	cmpr_rv = cmprs->ops.write(cmprs, iovs, n_iovecs);
	assert(cmpr_rv.error == 0);
	assert(cmpr_rv.n_iovecs == 1);
	assert(cmpr_rv.iovecs != NULL);

	written = write(fd_out, cmpr_rv.iovecs[0].iov_base, cmpr_rv.iovecs[0].iov_len);
	assert(written == cmpr_rv.iovecs[0].iov_len);

	return written;
}

static size_t stop_write_string(struct compressor *cmprs, int fd)
{
	size_t written;
	struct compression_result cmpr_rv = cmprs->ops.stop(cmprs);
	assert(cmpr_rv.error == 0);
	assert(cmpr_rv.n_iovecs == 1);
	assert(cmpr_rv.iovecs != NULL);

	written = write(fd, cmpr_rv.iovecs[0].iov_base, cmpr_rv.iovecs[0].iov_len);
	assert(written == cmpr_rv.iovecs[0].iov_len);

	return written;
}

static size_t compress_string(struct compressor *cmprs, uint8_t *text_in, size_t len_in, int fd)
{
	size_t written;

	written = start_write_string(cmprs, fd);
	written += cat_string(cmprs, text_in, len_in, len_in, fd);
	written += stop_write_string(cmprs, fd);

	return written;
}

static void decompress_file(const char *lz4_fn, void *out, size_t out_size)
{
	struct fspath lz4_fname = fspath_create("%s", lz4_fn);
	struct fspath out_fn = fspath_stem(&lz4_fname);
	struct fspath out_dpath = fspath_create("/tmp");
	
	size_t file_size;
	struct decompress_file_result dres = decompress_file_if_needed(&lz4_fname, &out_dpath);
	assert(dres.rv == 0);

	FILE *out_file = fopen(out_fn.path, "r");
	assert(out_file != NULL);
	assert (fseek(out_file, 0, SEEK_END) == 0);
	file_size = ftell(out_file);
	assert(file_size <= out_size);
	assert(fseek(out_file, 0, SEEK_SET) == 0);
	size_t bytes_read = fread(out, 1, file_size, out_file);
	assert(bytes_read == file_size);

	fclose(out_file);
	unlink(out_fn.path); // Clean up the output file
}

static char *test_text = "Hello, world! This is a test string for LZ4 compression. Hello, world! This is a test string for LZ4 compression.";

static void fill_with_random_chars(char *data, size_t size)
{
	for (size_t i = 0; i < size; ++i) {
		data[i] = 'A' + (rand() % 26); // Random uppercase letter
	}
}

static const size_t SIZE_4MB = 4 * 1024 * 1024; // 4 MB in bytes
						//
static char *allocate_and_fill_4mb()
{
	// Allocate 4 MB of memory
	char *data = (char *)malloc(SIZE_4MB);
	if (data == NULL) {
		perror("Failed to allocate memory");
		return NULL;
	}

	// Fill the allocated memory with random characters
	fill_with_random_chars(data, SIZE_4MB);

	return data;
}

int create_temp_file_with_extension(const char* dir, const char* extension, char out_filename[PATH_MAX])
{
	char prefix[] = "tempfile";
	char template[PATH_MAX];
	snprintf(template, sizeof(template), "%s/%sXXXXXX", dir, prefix);

	int fd = mkstemp(template);
	assert(fd != -1);

	snprintf(out_filename, PATH_MAX, "%s%s", template, extension);

	assert(rename(template, out_filename) >= 0);

	return fd;
}

void test_simple_string(void)
{
	size_t compressed;
	char file_name_compressed[PATH_MAX];
	int fd = create_temp_file_with_extension("/tmp", ".lz4", file_name_compressed);
	char out_data[1024];

	struct compressor *cmprs = compressor_create_lz4(4096);
	assert(cmprs != NULL);

	size_t len = strlen(test_text) + 1; // +1 for null terminator
	compressed = compress_string(cmprs, (uint8_t *)test_text, len, fd);
	printf("test_simple_string: size_in=%zu, compressed size: %zu\n", len, compressed);
	close(fd);

	decompress_file(file_name_compressed, out_data, sizeof(out_data));
	assert(strcmp(out_data, test_text) == 0);

	cmprs->ops.destroy(cmprs);
	unlink(file_name_compressed); // Clean up the temporary file
}

void test_multiple_iovs(void)
{
	size_t written;
	char file_name_compressed[PATH_MAX];
	int fd = create_temp_file_with_extension("/tmp", ".lz4", file_name_compressed);
	char out_data[1024];

	struct compressor *cmprs = compressor_create_lz4(4096);
	assert(cmprs != NULL);

	// Split the test text into multiple iovecs
	size_t len = strlen(test_text) + 1;

	written = start_write_string(cmprs, fd);
	written += cat_string(cmprs, (uint8_t *)test_text, len, len/10, fd);
	written += stop_write_string(cmprs, fd);

	printf("test_multiple_iovs: size_in=%zu, compressed size: %zu\n", len, written);

	close(fd);

	decompress_file(file_name_compressed, out_data, sizeof(out_data));
	assert(strcmp(out_data, test_text) == 0);

	cmprs->ops.destroy(cmprs);
	unlink(file_name_compressed); // Clean up the temporary file
}

void test_no_write_stop(void)
{
	char file_name_compressed[PATH_MAX];
	int fd = create_temp_file_with_extension("/tmp", ".lz4", file_name_compressed);
	char out_data[1024];
	size_t len = strlen(test_text) + 1;

	struct compressor *cmprs = compressor_create_lz4(4096);
	assert(cmprs != NULL);

	// Start writing without stopping
	size_t written = start_write_string(cmprs, fd);
	written += cat_string(cmprs, (uint8_t *)test_text, len, len, fd);

	printf("test_no_write_stop: size_in=%zu, compressed size: %zu\n", strlen(test_text) + 1, written);

	close(fd);

	decompress_file(file_name_compressed, out_data, sizeof(out_data));
	assert(strcmp(out_data, test_text) == 0);

	cmprs->ops.destroy(cmprs);
	unlink(file_name_compressed); // Clean up the temporary file
}

void test_huge_random_data(void)
{
	static const size_t buf_size = 4096; // page
	size_t written, data_writen = 0;

	char file_name_compressed[PATH_MAX];
	int fd = create_temp_file_with_extension("/tmp", ".lz4", file_name_compressed);
	char *data_4mb = allocate_and_fill_4mb();
	data_4mb[SIZE_4MB - 1] = '\0'; // Null-terminate to make it a valid string
	assert(data_4mb != NULL);
	void *out_data = calloc(1, SIZE_4MB);

	assert(out_data != NULL);

	struct compressor *cmprs = compressor_create_lz4(4096);
	assert(cmprs != NULL);

	written = start_write_string(cmprs, fd);
	for (data_writen = 0; data_writen < SIZE_4MB; data_writen += buf_size) {
		size_t to_write = (data_writen + buf_size <= SIZE_4MB) ? buf_size : (SIZE_4MB - data_writen);
		written += cat_string(cmprs, (uint8_t *)(data_4mb + data_writen), to_write, to_write, fd);
	}
	close(fd);
	printf("test_huge_random_data: size_in=%zu total written size: %zu\n", SIZE_4MB, written);

	decompress_file(file_name_compressed, out_data, SIZE_4MB);
	assert(memcmp(out_data, data_4mb, SIZE_4MB) == 0);

	cmprs->ops.destroy(cmprs);
	unlink(file_name_compressed); // Clean up the temporary file
	free(data_4mb);
	free(out_data);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_simple_string();
	test_multiple_iovs();
	test_no_write_stop();
	test_huge_random_data();
	return 0;
}
