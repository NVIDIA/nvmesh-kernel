/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

#define WARN(condition, format, ...) ({assert(!(condition)); (void)format;})

#include "nvmeib_pet_specification.h"

//{{{ instantiate the pet framework
extern struct nvmeib_pet_message_description const __start_nvmeib_pet_messages[];
extern struct nvmeib_pet_message_description const __stop_nvmeib_pet_messages[];

#define PET_MSG(pet_journal, msg, severity, ...) NVMEIB_IO_PET_MSG(pet_journal, msg, severity, __VA_ARGS__)
#define PET_MSG_NORM(pet_journal, msg, ...) NVMEIB_IO_PET_MSG_NORM(pet_journal, msg, __VA_ARGS__)

#include "compat/kr_incs_time_jiff.inc.c" // Due to legacy reasons, `tsc_khz` and `int_cpu_freq_tsc_offset_jiffies()` are defined here.
//}}}

//{{{different pet controllers
struct file_pet_controller {
	struct nvmeib_pet_base_controller base;
	int fd_output;
};

static void __file_pet_controller_flush(struct nvmeib_pet_base_controller const* base, enum nvmeib_pet_severity severity, struct iovec const data)
{
	ssize_t written = 0;
	__auto_type self = (struct file_pet_controller const*)base;
	if (self->fd_output == -1){
		return;
	}

	assert(data.iov_len != 0);
	assert(data.iov_base != NULL);

	(void)severity;
	written = write(self->fd_output, data.iov_base, data.iov_len);
	if (written != (ssize_t)data.iov_len){
		perror("Failed to write IO PET to file");
		BUG();
	}
}

struct nvmeib_pet_buffer __file_pet_controller_get_buffer(struct nvmeib_pet_base_controller const *self)
{
	void* ptr = malloc(4096);
	(void)self;
	return (struct nvmeib_pet_buffer){
		.data = { .iov_base = ptr, .iov_len = ptr ? 4096 : 0 },
		.release_cpu = NVMEIB_PET_NO_RELEASE_CPU,
	};
}

void __file_pet_controller_put_buffer(struct nvmeib_pet_base_controller const *self, struct nvmeib_pet_buffer buffer)
{
	(void)self;
	free(buffer.data.iov_base);
}

struct file_pet_controller file_pet_controller = {
	.base = {
		.flush = __file_pet_controller_flush,
		.get_buffer = __file_pet_controller_get_buffer,
		.put_buffer = __file_pet_controller_put_buffer
	},
	.fd_output = -1
};

struct perf_test_controller {
	struct nvmeib_pet_base_controller base;
	struct iovec msgs_buffer;
	struct iovec memcpy_buffer;
};

static struct nvmeib_pet_buffer __perf_test_get_buffer(struct nvmeib_pet_base_controller const *base)
{
	__auto_type self = (struct perf_test_controller const*)base;
	return (struct nvmeib_pet_buffer){
		.data = self->msgs_buffer,
		.release_cpu = NVMEIB_PET_NO_RELEASE_CPU,
	};
}

static void __perf_test_put_buffer(struct nvmeib_pet_base_controller const *self, struct nvmeib_pet_buffer buffer)
{
	(void)self;
	(void)buffer;
}

static void __perf_test_flush(struct nvmeib_pet_base_controller const* self, enum nvmeib_pet_severity severity, struct iovec const data)
{
	(void)self; (void)severity; (void)data;
	//uncomment the line below to check the parser(python) speed on 66MB file
	//file_pet_controller.base.flush(&file_pet_controller.base, severity, data);
}

struct iovec iovec_malloc(size_t size)
{
	struct iovec res = {.iov_base = malloc(size), .iov_len=size};
	BUG_ON(res.iov_base == NULL);
	return res;
}
//}}}

//gdb: examine command: x /[count]xb <pointer> //b for bytes
static u8 __test_message_x_memory[512] = {0};
static struct iovec __test_message_iovec = {.iov_base = __test_message_x_memory, .iov_len = ARRAY_SIZE(__test_message_x_memory)};
static char const* __test_random_rotation_output_dir = "build/rotations";

static void __test_mkdir_if_needed(char const* path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror("failed to create random rotation output directory");
		BUG();
	}
}

static void __test_stream_reset(struct nvmeib_pet_journalbuf* stream)
{
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	*stream = nvmeib_pet_journalbuf_make(__test_message_iovec);
}

void test_msg_header_layout(void)
{
	enum {
		msg_n_bytes = sizeof_field(struct nvmeib_pet_journalbuf_message_header, msg.timestamp) +
			      sizeof_field(struct nvmeib_pet_journalbuf_message_header, msg.args_n_bytes),
		spacer_n_bytes = sizeof_field(struct nvmeib_pet_journalbuf_message_header, spacer.unused) +
				 sizeof_field(struct nvmeib_pet_journalbuf_message_header, spacer.args_n_bytes),
		header_n_bytes = sizeof_field(struct nvmeib_pet_journalbuf_message_header, message_id) + msg_n_bytes,
	};

	BUG_ON(sizeof_field(struct nvmeib_pet_journalbuf_message_header, msg) != msg_n_bytes);
	BUG_ON(sizeof_field(struct nvmeib_pet_journalbuf_message_header, spacer) != spacer_n_bytes);
	BUG_ON(sizeof_field(struct nvmeib_pet_journalbuf_message_header, msg) != sizeof_field(struct nvmeib_pet_journalbuf_message_header, spacer));
	BUG_ON(offsetof(struct nvmeib_pet_journalbuf_message_header, msg.args_n_bytes) !=
	       offsetof(struct nvmeib_pet_journalbuf_message_header, spacer.args_n_bytes));
	BUG_ON(sizeof(struct nvmeib_pet_journalbuf_message_header) != header_n_bytes);
}

void test_stream_header_layout(void)
{
	enum {
		trace_clock_n_bytes = sizeof_field(struct nvmeib_pet_trace_clock, tsc_offset) +
				      sizeof_field(struct nvmeib_pet_trace_clock, tsc_khz),
		header_n_bytes = sizeof_field(struct nvmeib_pet_journalbuf_header, commit_id) +
				 sizeof_field(struct nvmeib_pet_journalbuf_header, journalbuf_size) +
				 trace_clock_n_bytes,
	};

	BUG_ON(sizeof(struct nvmeib_pet_trace_clock) != trace_clock_n_bytes);
	BUG_ON(sizeof(struct nvmeib_pet_journalbuf_header) != header_n_bytes);
	BUG_ON(NVMEIB_PET_JOURNALBUF_HEADER_SIZE != sizeof(struct nvmeib_pet_journalbuf_header));
}

static struct nvmeib_pet_journalbuf_message_header __test_load_msg_header(size_t offset)
{
	struct nvmeib_pet_journalbuf_message_header header = {0};
	memcpy(&header, __test_message_x_memory + offset, sizeof(header));
	return header;
}

static struct nvmeib_pet_journalbuf_message_header __test_load_msg_header_from_buffer(u8 const* buffer, size_t offset)
{
	struct nvmeib_pet_journalbuf_message_header header = {0};
	memcpy(&header, buffer + offset, sizeof(header));
	return header;
}

static void __test_check_trace_clock(struct nvmeib_pet_trace_clock trace_clock)
{
	struct nvmeib_pet_trace_clock const expected = nvmeib_pet_trace_clock_get();

	BUG_ON(trace_clock.tsc_khz == 0);
	BUG_ON(trace_clock.tsc_offset != expected.tsc_offset);
	BUG_ON(trace_clock.tsc_khz != expected.tsc_khz);
}

static u64 __test_trace_ticks_to_ns(u64 ticks, struct nvmeib_pet_trace_clock trace_clock)
{
	BUG_ON(trace_clock.tsc_khz == 0);
	return MUL_X_DIV_Y(ticks + trace_clock.tsc_offset, 1000000ULL, (u64)trace_clock.tsc_khz);
}

static void __test_check_msg_header(size_t offset, u16 message_index, u8 expected_args_n_bytes)
{
	struct nvmeib_pet_journalbuf_message_header const header = __test_load_msg_header(offset);

	BUG_ON(header.message_id != message_index + 1);
	BUG_ON(header.message_id == 0);
	BUG_ON(header.msg.args_n_bytes != expected_args_n_bytes);
	BUG_ON(header.msg.timestamp == 0);
}

static void __test_check_msg_header_from_buffer(u8 const* buffer, size_t offset, u16 message_index, u8 expected_args_n_bytes)
{
	struct nvmeib_pet_journalbuf_message_header const header = __test_load_msg_header_from_buffer(buffer, offset);

	BUG_ON(header.message_id != message_index + 1);
	BUG_ON(header.message_id == 0);
	BUG_ON(header.msg.args_n_bytes != expected_args_n_bytes);
	BUG_ON(header.msg.timestamp == 0);
}

static void __test_check_spacer_from_buffer(u8 const* buffer, size_t offset, u16 expected_physical_n_bytes)
{
	struct nvmeib_pet_journalbuf_message_header const header = __test_load_msg_header_from_buffer(buffer, offset);

	BUG_ON(expected_physical_n_bytes < sizeof(header));
	BUG_ON(header.message_id != 0);
	BUG_ON(header.spacer.unused != 0);
	BUG_ON(header.spacer.args_n_bytes != expected_physical_n_bytes - sizeof(header));
}

struct test_random_rotation_msg {
	u16 message_index;
	u16 record_n_bytes;
	u8 args_n_bytes;
	u64 timestamp;
	u8 payload[NVMEIB_PET_MAX_MSG_ARGS_N_BYTES];
};

struct test_random_rotation_stats {
	u32 useful_msgs;
	u32 useful_msg_bytes;
	u32 spacer_bytes;
	u32 padding_bytes;
};

static u32 __test_random_u32(void)
{
	return (u32)random();
}

static void __test_random_rotation_capture_msg(struct nvmeib_pet_journal const* journal,
					       u16 message_index, u16 written,
					       struct test_random_rotation_msg* msg)
{
	u16 const msg_offset = journal->journalbuf.write_offset - written;
	u8 const* const buffer = journal->journalbuf.data.iov_base;
	struct nvmeib_pet_journalbuf_message_header const header = __test_load_msg_header_from_buffer(buffer, msg_offset);

	BUG_ON(written < sizeof(header));
	BUG_ON(header.message_id != message_index + 1);
	BUG_ON(header.msg.args_n_bytes != written - sizeof(header));
	BUG_ON(header.msg.args_n_bytes > sizeof(msg->payload));

	*msg = (struct test_random_rotation_msg){
		.message_index = message_index,
		.record_n_bytes = written,
		.args_n_bytes = header.msg.args_n_bytes,
		.timestamp = header.msg.timestamp,
	};
	memcpy(msg->payload, buffer + msg_offset + sizeof(header), msg->args_n_bytes);
}

static u16 __test_random_rotation_write_msg(struct nvmeib_pet_journal* journal, u16 message_index,
					    struct test_random_rotation_msg* msg)
{
	u16 written = 0;
	u64 const a64 = ((u64)__test_random_u32() << 32) | __test_random_u32();
	u64 const b64 = ((u64)__test_random_u32() << 32) | __test_random_u32();
	u64 const c64 = ((u64)__test_random_u32() << 32) | __test_random_u32();
	u32 const a32 = __test_random_u32();
	u16 const a16 = (u16)__test_random_u32();
	u8 const a8 = (u8)__test_random_u32();

	switch (random() % 8) {
	case 0:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a8);
		break;
	case 1:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a16);
		break;
	case 2:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a32);
		break;
	case 3:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a64);
		break;
	case 4:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a8, a16, a32);
		break;
	case 5:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a64, a32, a16, a8);
		break;
	case 6:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a64, b64);
		break;
	default:
		written = nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index, a64, b64, c64, a32);
		break;
	}

	BUG_ON(written == 0);
	__test_random_rotation_capture_msg(journal, message_index, written, msg);
	while (nvmeib_pet_get_trace_time_ticks() <= msg->timestamp) {
		/* Keep test timestamps strictly ordered. */
	}
	return written;
}

static void __test_unrotate_random_rotation_msgs_by_time(struct test_random_rotation_msg* msgs, size_t n_msgs)
{
	size_t idx = 0;
	size_t rotation_at = 0;
	size_t newer_prefix_start = 0;
	size_t n_tmp = 0;

	if (n_msgs < 2) {
		return;
	}

	rotation_at = n_msgs;
	for (idx = 1; idx < n_msgs; ++idx) {
		if (msgs[idx - 1].timestamp > msgs[idx].timestamp) {
			rotation_at = idx;
			break;
		}
	}
	if (rotation_at == n_msgs) {
		return;
	}

	/* Physical order after rotation is: protected + newer-prefix + older-suffix. */
	for (idx = 0; idx < rotation_at; ++idx) {
		if (msgs[idx].timestamp > msgs[rotation_at].timestamp) {
			newer_prefix_start = idx;
			break;
		}
	}

	struct test_random_rotation_msg tmp[n_msgs];

	memcpy(tmp + n_tmp, msgs, newer_prefix_start * sizeof(*msgs));
	n_tmp += newer_prefix_start;
	memcpy(tmp + n_tmp, msgs + rotation_at, (n_msgs - rotation_at) * sizeof(*msgs));
	n_tmp += n_msgs - rotation_at;
	memcpy(tmp + n_tmp, msgs + newer_prefix_start, (rotation_at - newer_prefix_start) * sizeof(*msgs));
	n_tmp += rotation_at - newer_prefix_start;
	BUG_ON(n_tmp != n_msgs);

	memcpy(msgs, tmp, n_msgs * sizeof(*msgs));
	for (idx = 1; idx < n_msgs; ++idx) {
		BUG_ON(msgs[idx - 1].timestamp > msgs[idx].timestamp);
	}
}

static size_t __test_read_random_rotation_msgs(struct nvmeib_pet_journalbuf const* stream,
					       struct test_random_rotation_msg* msgs,
					       size_t max_msgs,
					       struct test_random_rotation_stats* stats)
{
	u8 const* const buffer = stream->data.iov_base;
	u16 pos = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	size_t n_msgs = 0;

	*stats = (struct test_random_rotation_stats){0};
	while (pos < stream->max_written_bytes) {
		u16 const remaining = stream->max_written_bytes - pos;
		struct nvmeib_pet_journalbuf_message_header header = {0};

		if (remaining < sizeof(header)) {
			stats->padding_bytes += remaining;
			break;
		}

		header = __test_load_msg_header_from_buffer(buffer, pos);
		if (header.message_id == 0) {
			u16 spacer_n_bytes = 0;

			BUG_ON(header.spacer.unused != 0);
			BUG_ON(header.spacer.args_n_bytes > remaining - sizeof(header));
			spacer_n_bytes = sizeof(header) + (u16)header.spacer.args_n_bytes;
			stats->spacer_bytes += spacer_n_bytes;
			pos += spacer_n_bytes;
			continue;
		} else {
			u16 const record_n_bytes = sizeof(header) + header.msg.args_n_bytes;

			BUG_ON(record_n_bytes > remaining);
			BUG_ON(n_msgs >= max_msgs);
			BUG_ON(header.msg.args_n_bytes > sizeof(msgs[n_msgs].payload));
			msgs[n_msgs] = (struct test_random_rotation_msg){
				.message_index = header.message_id - 1,
				.record_n_bytes = record_n_bytes,
				.args_n_bytes = header.msg.args_n_bytes,
				.timestamp = header.msg.timestamp,
			};
			memcpy(msgs[n_msgs].payload, buffer + pos + sizeof(header), header.msg.args_n_bytes);
			++n_msgs;
			stats->useful_msgs += 1;
			stats->useful_msg_bytes += record_n_bytes;
			pos += record_n_bytes;
		}
	}

	__test_unrotate_random_rotation_msgs_by_time(msgs, n_msgs);
	return n_msgs;
}

static void __test_compare_random_rotation_msgs(struct test_random_rotation_msg const* expected,
						size_t n_expected,
						struct test_random_rotation_msg const* actual,
						size_t n_actual)
{
	size_t idx = 0;

	BUG_ON(n_actual != n_expected);
	for (idx = 0; idx < n_expected; ++idx) {
		BUG_ON(actual[idx].message_index != expected[idx].message_index);
		BUG_ON(actual[idx].record_n_bytes != expected[idx].record_n_bytes);
		BUG_ON(actual[idx].args_n_bytes != expected[idx].args_n_bytes);
		BUG_ON(actual[idx].timestamp != expected[idx].timestamp);
		BUG_ON(memcmp(actual[idx].payload, expected[idx].payload, expected[idx].args_n_bytes) != 0);
	}
}

static void __test_write_all(int fd, void const* buffer, size_t n_bytes)
{
	u8 const* pos = buffer;
	size_t remaining = n_bytes;

	while (remaining) {
		ssize_t const written = write(fd, pos, remaining);

		if (written <= 0) {
			perror("failed to write random rotation journal artifact");
			BUG();
		}

		pos += written;
		remaining -= written;
	}
}

static void __test_random_rotation_write_journal_file(u32 seed, struct nvmeib_pet_journalbuf const* stream)
{
	char fname[256] = {0};
	int fd = -1;
	int n = 0;

	n = snprintf(fname, sizeof(fname), "%s/seed_0x%08x.pet",
		     __test_random_rotation_output_dir, seed);
	BUG_ON(n < 0 || (size_t)n >= sizeof(fname));
	fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror("failed to open random rotation journal artifact");
		BUG();
	}

	__test_write_all(fd, stream->data.iov_base, stream->max_written_bytes);
	if (close(fd) != 0) {
		perror("failed to close random rotation journal artifact");
		BUG();
	}
}

static void __test_random_rotation_print_payload(FILE* fp, struct test_random_rotation_msg const* msg)
{
	size_t idx = 0;

	for (idx = 0; idx < msg->args_n_bytes; ++idx) {
		fprintf(fp, "%02x", msg->payload[idx]);
	}
}

static void __test_random_rotation_write_expected_file(u32 seed,
						       struct test_random_rotation_msg const* expected_msgs,
						       size_t n_expected_msgs,
						       struct nvmeib_pet_trace_clock trace_clock)
{
	char fname[256] = {0};
	FILE* fp = NULL;
	size_t idx = 0;
	int n = 0;

	n = snprintf(fname, sizeof(fname), "%s/seed_0x%08x.expected.txt",
		     __test_random_rotation_output_dir, seed);
	BUG_ON(n < 0 || (size_t)n >= sizeof(fname));
	fp = fopen(fname, "w");
	if (!fp) {
		perror("failed to open random rotation expected artifact");
		BUG();
	}

	for (idx = 0; idx < n_expected_msgs; ++idx) {
		u64 const ns_timestamp = __test_trace_ticks_to_ns(expected_msgs[idx].timestamp, trace_clock);

		fprintf(fp,
			"%zu message_id=0x%04x message_index=0x%04x record_n_bytes=%u args_n_bytes=%u timestamp=%llu payload=",
			idx, expected_msgs[idx].message_index + 1, expected_msgs[idx].message_index, expected_msgs[idx].record_n_bytes,
			expected_msgs[idx].args_n_bytes, (unsigned long long)ns_timestamp);
		__test_random_rotation_print_payload(fp, &expected_msgs[idx]);
		fprintf(fp, "\n");
	}

	if (fclose(fp) != 0) {
		perror("failed to close random rotation expected artifact");
		BUG();
	}
}

static void __test_check_payload(size_t offset, void const* expected, size_t size)
{
	u8 const* payload = __test_message_x_memory + offset + sizeof(struct nvmeib_pet_journalbuf_message_header);
	BUG_ON(memcmp(payload, expected, size) != 0);
}

static void __test_check_payload_sequence(size_t offset, u8 first, size_t size)
{
	u8 const* payload = __test_message_x_memory + offset + sizeof(struct nvmeib_pet_journalbuf_message_header);
	size_t idx = 0;

	for (idx = 0; idx < size; ++idx) {
		BUG_ON(payload[idx] != (u8)(first + idx));
	}
}

static void __test_check_protected_area(struct nvmeib_pet_journalbuf const* stream, u16 protected_prefix)
{
	BUG_ON(stream->protected_prefix != protected_prefix);
	BUG_ON(stream->protected_prefix > stream->max_written_bytes);
}

static void __test_stream_set_synthetic_protected_prefix(struct nvmeib_pet_journalbuf* stream)
{
	/* Rotation-shape tests use tiny buffers on purpose; bypass the production
	 * "two max messages" capacity contract and set the protected prefix directly.
	 */
	BUG_ON(stream->max_written_bytes > stream->data.iov_len);
	stream->protected_prefix = stream->max_written_bytes;
	stream->write_offset = stream->protected_prefix;
}

#define TEST_STREAM_WRITE_SEQUENCE(stream, message_index, expected_size, first_value, ...) \
do { \
	size_t const __start = (stream).max_written_bytes; \
	size_t const __written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&(stream), message_index, __VA_ARGS__); \
	BUG_ON(__written != sizeof(struct nvmeib_pet_journalbuf_message_header) + (expected_size)); \
	BUG_ON((stream).max_written_bytes != __start + __written); \
	__test_check_msg_header(__start, message_index, expected_size); \
	__test_check_payload_sequence(__start, first_value, expected_size); \
} while (0)

void test_stream_write_all_arg_counts(void)
{
	struct nvmeib_pet_journalbuf stream = {0};
	__test_stream_reset(&stream);

	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0101, 1, 0x01, (u8)0x01);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0102, 2, 0x02, (u8)0x02, (u8)0x03);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0103, 3, 0x04, (u8)0x04, (u8)0x05, (u8)0x06);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0104, 4, 0x07, (u8)0x07, (u8)0x08, (u8)0x09, (u8)0x0a);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0105, 5, 0x0b, (u8)0x0b, (u8)0x0c, (u8)0x0d, (u8)0x0e, (u8)0x0f);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0106, 6, 0x10, (u8)0x10, (u8)0x11, (u8)0x12, (u8)0x13, (u8)0x14, (u8)0x15);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0107, 7, 0x16, (u8)0x16, (u8)0x17, (u8)0x18, (u8)0x19, (u8)0x1a, (u8)0x1b, (u8)0x1c);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0108, 8, 0x1d, (u8)0x1d, (u8)0x1e, (u8)0x1f, (u8)0x20, (u8)0x21, (u8)0x22, (u8)0x23, (u8)0x24);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0109, 9, 0x25, (u8)0x25, (u8)0x26, (u8)0x27, (u8)0x28, (u8)0x29, (u8)0x2a, (u8)0x2b, (u8)0x2c, (u8)0x2d);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x010a, 10, 0x2e, (u8)0x2e, (u8)0x2f, (u8)0x30, (u8)0x31, (u8)0x32, (u8)0x33, (u8)0x34, (u8)0x35, (u8)0x36, (u8)0x37);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x010b, 11, 0x38, (u8)0x38, (u8)0x39, (u8)0x3a, (u8)0x3b, (u8)0x3c, (u8)0x3d, (u8)0x3e, (u8)0x3f, (u8)0x40, (u8)0x41, (u8)0x42);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x010c, 12, 0x43, (u8)0x43, (u8)0x44, (u8)0x45, (u8)0x46, (u8)0x47, (u8)0x48, (u8)0x49, (u8)0x4a, (u8)0x4b, (u8)0x4c, (u8)0x4d, (u8)0x4e);

}

void test_stream_protect_empty_prefix(void)
{
	struct nvmeib_pet_journalbuf stream = {0};

	__test_stream_reset(&stream);
	nvmeib_pet_journalbuf_protect_prefix(&stream);

	__test_check_protected_area(&stream, NVMEIB_PET_JOURNALBUF_HEADER_SIZE);
	BUG_ON(stream.max_written_bytes != NVMEIB_PET_JOURNALBUF_HEADER_SIZE);
}

void test_stream_protect_prefix_expands_protected_area(void)
{
	struct nvmeib_pet_journalbuf stream = {0};
	u16 first_protected = 0;
	u16 second_protected = 0;

	__test_stream_reset(&stream);
	__test_check_protected_area(&stream, NVMEIB_PET_JOURNALBUF_HEADER_SIZE);
	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0501, 1, 0x01, (u8)0x01);
	first_protected = stream.max_written_bytes;
	nvmeib_pet_journalbuf_protect_prefix(&stream);
	__test_check_protected_area(&stream, first_protected);

	TEST_STREAM_WRITE_SEQUENCE(stream, 0x0502, 1, 0x02, (u8)0x02);
	__test_check_protected_area(&stream, first_protected);
	nvmeib_pet_journalbuf_protect_prefix(&stream);

	second_protected = stream.max_written_bytes;
	__test_check_protected_area(&stream, second_protected);
	BUG_ON(second_protected <= first_protected);
}

void test_stream_write_mixed_size_args(void)
{
	struct nvmeib_pet_journalbuf stream = {0};
	struct __attribute__((packed)) {
		int8_t arg1;
		uint16_t arg2;
		uint32_t arg3;
		uint64_t arg4;
	} expected = {
		.arg1 = (int8_t)0x11,
		.arg2 = (uint16_t)0x2223,
		.arg3 = (uint32_t)0x44444445,
		.arg4 = (uint64_t)0x8888888888888889ULL,
	};
	size_t start = 0;
	size_t written = 0;

	__test_stream_reset(&stream);
	start = stream.max_written_bytes;
	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0201,
						expected.arg1,
						expected.arg2,
						expected.arg3,
						expected.arg4);

	BUG_ON(written != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(expected));
	BUG_ON(stream.max_written_bytes != start + written);
	__test_check_msg_header(start, 0x0201, sizeof(expected));
	__test_check_payload(start, &expected, sizeof(expected));
}

void test_stream_write_zero_offset_is_stored_as_one(void)
{
	struct nvmeib_pet_journalbuf stream = {0};
	size_t start = 0;
	size_t written = 0;
	u8 expected = 0x55;

	__test_stream_reset(&stream);
	start = stream.max_written_bytes;
	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0, expected);

	BUG_ON(written != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(expected));
	__test_check_msg_header(start, 0, sizeof(expected));
	__test_check_payload(start, &expected, sizeof(expected));
}

/* Rotation diagrams:
 * P = protected prefix, M = message, S = spacer, _ = unwritten bytes,
 * EOF = max_written_bytes. M8 means a message with one u8 payload argument;
 * M64 means one u64 payload argument; M2x64 means two u64 payload arguments.
 */

/* Replace a large message with a small one when the leftover is spacer-sized.
 *
 * Before: [P][M2x64 old][M8 old][EOF]
 * After:  [P][M8 new ][S      ][M8 old][EOF]
 */
void test_stream_rotation_small_over_large_leaves_spacer(void)
{
	enum {
		msg_u8_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8),
		msg_2u64_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u64) + sizeof(u64),
		buffer_n_bytes = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_2u64_n_bytes + msg_u8_n_bytes,
	};
	u8 buffer[buffer_n_bytes];
	struct nvmeib_pet_journalbuf stream = {0};
	u16 written = 0;
	u16 const old_large_offset = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	u16 const rotated_msg_offset = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	u16 const spacer_offset = rotated_msg_offset + msg_u8_n_bytes;
	u16 const old_second_msg_offset = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_2u64_n_bytes;
	u16 const spacer_n_bytes = msg_2u64_n_bytes - msg_u8_n_bytes;

	memset(buffer, 0xcc, sizeof(buffer));
	stream = nvmeib_pet_journalbuf_make((struct iovec){.iov_base = buffer, .iov_len = sizeof(buffer)});
	__test_stream_set_synthetic_protected_prefix(&stream);

	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0701, (u64)0x11, (u64)0x12) != msg_2u64_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0702, (u8)0x22) != msg_u8_n_bytes);
	BUG_ON(stream.max_written_bytes != sizeof(buffer));
	BUG_ON(stream.write_offset != sizeof(buffer));

	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0703, (u8)0x33);
	BUG_ON(written != msg_u8_n_bytes);
	BUG_ON(stream.max_written_bytes != sizeof(buffer));
	BUG_ON(stream.write_offset != spacer_offset);
	__test_check_msg_header_from_buffer(buffer, rotated_msg_offset, 0x0703, sizeof(u8));
	__test_check_spacer_from_buffer(buffer, spacer_offset, spacer_n_bytes);
	__test_check_msg_header_from_buffer(buffer, old_second_msg_offset, 0x0702, sizeof(u8));
	BUG_ON(old_large_offset != rotated_msg_offset);
}

/* Replace a medium message with a small one when the direct leftover is too
 * small for a spacer. The allocator consumes the next record as well, so the
 * final leftover becomes parseable.
 *
 * Before: [P][M64 old][M8 old][EOF]
 * After:  [P][M8 new ][S             ][EOF]
 */
void test_stream_rotation_small_over_large_consumes_next_without_tiny_gap(void)
{
	enum {
		msg_u8_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8),
		msg_u64_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u64),
		buffer_n_bytes = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_u64_n_bytes + msg_u8_n_bytes,
	};
	u8 buffer[buffer_n_bytes];
	struct nvmeib_pet_journalbuf stream = {0};
	u16 written = 0;
	u16 const rotated_msg_offset = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	u16 const spacer_offset = rotated_msg_offset + msg_u8_n_bytes;
	u16 const spacer_n_bytes = msg_u64_n_bytes + msg_u8_n_bytes - msg_u8_n_bytes;

	memset(buffer, 0xcc, sizeof(buffer));
	stream = nvmeib_pet_journalbuf_make((struct iovec){.iov_base = buffer, .iov_len = sizeof(buffer)});
	__test_stream_set_synthetic_protected_prefix(&stream);

	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0711, (u64)0x11) != msg_u64_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0712, (u8)0x22) != msg_u8_n_bytes);
	BUG_ON(stream.max_written_bytes != sizeof(buffer));

	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0713, (u8)0x33);
	BUG_ON(written != msg_u8_n_bytes);
	BUG_ON(stream.max_written_bytes != sizeof(buffer));
	BUG_ON(stream.write_offset != spacer_offset);
	__test_check_msg_header_from_buffer(buffer, rotated_msg_offset, 0x0713, sizeof(u8));
	__test_check_spacer_from_buffer(buffer, spacer_offset, spacer_n_bytes);
}

/* Replace several small messages with one large message. The remaining tail is
 * too small for a spacer, so EOF is moved backward to hide it from the viewer.
 *
 * Before: [P][M8 old][M8 old][M8 old][EOF]
 * After:  [P][M2x64 new     ][EOF][hidden tail]
 */
void test_stream_rotation_large_over_small_messages_reduces_eof_for_tiny_tail(void)
{
	enum {
		msg_u8_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8),
		msg_2u64_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u64) + sizeof(u64),
		buffer_n_bytes = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_u8_n_bytes + msg_u8_n_bytes + msg_u8_n_bytes,
	};
	u8 buffer[buffer_n_bytes];
	struct nvmeib_pet_journalbuf stream = {0};
	u16 written = 0;
	u16 const rotated_msg_offset = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	u16 const rotated_msg_end = rotated_msg_offset + msg_2u64_n_bytes;

	memset(buffer, 0xcc, sizeof(buffer));
	stream = nvmeib_pet_journalbuf_make((struct iovec){.iov_base = buffer, .iov_len = sizeof(buffer)});
	__test_stream_set_synthetic_protected_prefix(&stream);

	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0721, (u8)0x21) != msg_u8_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0722, (u8)0x22) != msg_u8_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0723, (u8)0x23) != msg_u8_n_bytes);
	BUG_ON(stream.max_written_bytes != sizeof(buffer));

	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0724, (u64)0x24, (u64)0x25);
	BUG_ON(written != msg_2u64_n_bytes);
	BUG_ON(stream.max_written_bytes != rotated_msg_end);
	BUG_ON(stream.write_offset != rotated_msg_end);
	__test_check_msg_header_from_buffer(buffer, rotated_msg_offset, 0x0724, sizeof(u64) + sizeof(u64));
	BUG_ON(stream.max_written_bytes >= sizeof(buffer));
}

/* Grow past the old EOF into unwritten capacity. This is a fast-path append
 * shape: the new message covers the old tail and reaches physical space that
 * was never viewer-visible, so no spacer is needed.
 *
 * Before: [P][M8 old][EOF][________]
 * After:  [P][M2x64 new     ][EOF]
 */
void test_stream_rotation_uses_unwritten_tail_after_eof(void)
{
	enum {
		msg_u8_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8),
		msg_2u64_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u64) + sizeof(u64),
		buffer_n_bytes = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_2u64_n_bytes,
	};
	u8 buffer[buffer_n_bytes];
	struct nvmeib_pet_journalbuf stream = {0};
	u16 written = 0;
	u16 const prefix = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	u16 const large_msg_end = prefix + msg_2u64_n_bytes;

	memset(buffer, 0xcc, sizeof(buffer));
	stream = nvmeib_pet_journalbuf_make((struct iovec){.iov_base = buffer, .iov_len = sizeof(buffer)});
	__test_stream_set_synthetic_protected_prefix(&stream);

	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0725, (u8)0x25) != msg_u8_n_bytes);
	BUG_ON(stream.max_written_bytes == sizeof(buffer));

	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0726, (u64)0x26, (u64)0x27);
	BUG_ON(written != msg_2u64_n_bytes);
	BUG_ON(stream.max_written_bytes != sizeof(buffer));
	BUG_ON(stream.write_offset != large_msg_end);
	__test_check_msg_header_from_buffer(buffer, prefix, 0x0726, sizeof(u64) + sizeof(u64));
}

/* The bottom of allocate_rotate() is only the middle-overwrite path. EOF
 * extension belongs to the fast path, so the final update must never increase
 * max_written_bytes.
 *
 * Spacer case:
 *   Before: [P][M2x64 old][M8 old][EOF]
 *   After:  [P][M8 new ][S      ][M8 old][EOF]
 *
 * Tiny-tail case:
 *   Before: [P][M8 old][M8 old][M8 old][EOF]
 *   After:  [P][M2x64 new     ][EOF][hidden tail]
 */
void test_stream_allocate_rotate_final_update_does_not_extend_eof(void)
{
	enum {
		msg_u8_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8),
		msg_2u64_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u64) + sizeof(u64),
		spacer_buffer_n_bytes = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_2u64_n_bytes + msg_u8_n_bytes,
		tiny_tail_buffer_n_bytes = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_u8_n_bytes + msg_u8_n_bytes + msg_u8_n_bytes,
	};
	u8 spacer_buffer[spacer_buffer_n_bytes];
	u8 tiny_tail_buffer[tiny_tail_buffer_n_bytes];
	struct nvmeib_pet_journalbuf stream = {0};
	u16 const prefix = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	u16 old_max_written_bytes = 0;
	u8* dest = NULL;

	memset(spacer_buffer, 0xcc, sizeof(spacer_buffer));
	stream = nvmeib_pet_journalbuf_make((struct iovec){.iov_base = spacer_buffer, .iov_len = sizeof(spacer_buffer)});
	__test_stream_set_synthetic_protected_prefix(&stream);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0727, (u64)0x27, (u64)0x28) != msg_2u64_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0728, (u8)0x28) != msg_u8_n_bytes);
	old_max_written_bytes = stream.max_written_bytes;

	dest = __nvmeib_pet_journalbuf_allocate_rotate(&stream, msg_u8_n_bytes);
	BUG_ON(dest != spacer_buffer + prefix);
	BUG_ON(stream.max_written_bytes != old_max_written_bytes);
	BUG_ON(stream.max_written_bytes < stream.write_offset);

	memset(tiny_tail_buffer, 0xcc, sizeof(tiny_tail_buffer));
	stream = nvmeib_pet_journalbuf_make((struct iovec){.iov_base = tiny_tail_buffer, .iov_len = sizeof(tiny_tail_buffer)});
	__test_stream_set_synthetic_protected_prefix(&stream);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0729, (u8)0x29) != msg_u8_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x072a, (u8)0x2a) != msg_u8_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x072b, (u8)0x2b) != msg_u8_n_bytes);

	dest = __nvmeib_pet_journalbuf_allocate_rotate(&stream, msg_2u64_n_bytes);
	BUG_ON(dest != tiny_tail_buffer + prefix);
	BUG_ON(stream.max_written_bytes != stream.write_offset);
	BUG_ON(stream.max_written_bytes < stream.write_offset);
}

/* Exercise end-of-buffer wrap after prior middle overwrites. The final write
 * cannot fit at the physical tail, so EOF is first shrunk to hide that tail,
 * then the message is written from the prefix and any parseable leftover is a
 * spacer.
 *
 * Before final write: [P][M8][M2x64][M8 tail][EOF]
 * Wrap/shrink:        [P][M8][M2x64][EOF][hidden tail]
 * After:              [P][M2x64 new][S][EOF]
 */
void test_stream_rotation_end_wrap_shrinks_eof_and_writes_from_prefix(void)
{
	enum {
		msg_u8_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8),
		msg_2u64_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u64) + sizeof(u64),
		buffer_n_bytes = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + msg_2u64_n_bytes + msg_u8_n_bytes + msg_u8_n_bytes,
	};
	u8 buffer[buffer_n_bytes];
	struct nvmeib_pet_journalbuf stream = {0};
	u16 written = 0;
	u16 const prefix = NVMEIB_PET_JOURNALBUF_HEADER_SIZE;
	u16 const first_spacer_offset = prefix + msg_u8_n_bytes;
	u16 const tail_offset = prefix + msg_u8_n_bytes + msg_2u64_n_bytes;
	u16 const final_msg_end = prefix + msg_2u64_n_bytes;
	u16 const final_spacer_n_bytes = msg_u8_n_bytes;

	memset(buffer, 0xcc, sizeof(buffer));
	stream = nvmeib_pet_journalbuf_make((struct iovec){.iov_base = buffer, .iov_len = sizeof(buffer)});
	__test_stream_set_synthetic_protected_prefix(&stream);

	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0731, (u64)0x31, (u64)0x32) != msg_2u64_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0732, (u8)0x32) != msg_u8_n_bytes);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0733, (u8)0x33) != msg_u8_n_bytes);
	BUG_ON(stream.max_written_bytes != sizeof(buffer));

	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0734, (u8)0x34) != msg_u8_n_bytes);
	BUG_ON(stream.write_offset != first_spacer_offset);
	BUG_ON(__NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0735, (u64)0x35, (u64)0x36) != msg_2u64_n_bytes);
	BUG_ON(stream.write_offset != tail_offset);

	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0736, (u64)0x37, (u64)0x38);
	BUG_ON(written != msg_2u64_n_bytes);
	BUG_ON(stream.max_written_bytes != tail_offset);
	BUG_ON(stream.write_offset != final_msg_end);
	__test_check_msg_header_from_buffer(buffer, prefix, 0x0736, sizeof(u64) + sizeof(u64));
	__test_check_spacer_from_buffer(buffer, final_msg_end, final_spacer_n_bytes);
}

void test_stream_commit_sets_journalbuf_size(void)
{
	struct nvmeib_pet_journalbuf stream = {0};
	struct nvmeib_pet_journalbuf_header committed_header = {0};
	u16 written = 0;
	u16 expected_journalbuf_size = 0;

	__test_stream_reset(&stream);

	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0741, (u8)0x41);
	expected_journalbuf_size = stream.max_written_bytes;

	BUG_ON(written != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8));

	nvmeib_pet_journalbuf_commit(&stream);

	BUG_ON(stream.data.iov_len != expected_journalbuf_size);
	memcpy(&committed_header, stream.data.iov_base, sizeof(committed_header));
	BUG_ON(committed_header.commit_id != (u64)COMMIT_ID);
	BUG_ON(committed_header.journalbuf_size != expected_journalbuf_size);
	__test_check_trace_clock(committed_header.trace_clock);
}

static size_t __test_make_random_rotation_expectation(struct test_random_rotation_msg const* generated_msgs,
						      size_t n_generated_msgs,
						      u16 readable_n_bytes,
						      struct test_random_rotation_msg* expected_msgs)
{
	size_t start = n_generated_msgs;
	size_t expected_n_bytes = 0;
	size_t n_expected_msgs = 0;

	while (start > 0 &&
	       expected_n_bytes + generated_msgs[start - 1].record_n_bytes <= readable_n_bytes) {
		--start;
		expected_n_bytes += generated_msgs[start].record_n_bytes;
	}

	n_expected_msgs = n_generated_msgs - start;
	memcpy(expected_msgs, generated_msgs + start, n_expected_msgs * sizeof(*expected_msgs));
	return n_expected_msgs;
}

/* Random rotation stress test. Each run writes about 3x the journal capacity,
 * then parses [P, EOF) as the viewer would and compares it with the newest
 * messages that fit the useful capacity after spacers/padding.
 *
 * Writes:  [P][random messages .........................]
 * Journal: [P][M|S][M|S]...[M|EOF]
 * Expect:  unrotate-by-time(newest messages that fit useful bytes)
 */
void test_journal_random_rotation_retains_last_messages(void)
{
	enum {
		journal_n_bytes = 512,
		target_written_n_bytes = 1536,
		n_runs = 42,
		max_random_msgs = 192,
	};
	unsigned run = 0;
	u32 const seed_base = (u32)time(NULL);

	for (run = 0; run < n_runs; ++run) {
		u8 buffer[journal_n_bytes] = {0};
		struct perf_test_controller controller = {
			.base = {
				.flush = __perf_test_flush,
				.get_buffer = __perf_test_get_buffer,
				.put_buffer = __perf_test_put_buffer
			},
			.msgs_buffer = {.iov_base = buffer, .iov_len = sizeof(buffer)},
			.memcpy_buffer = {0},
		};
		struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&controller.base, true);
		struct test_random_rotation_msg generated_msgs[max_random_msgs] = {0};
		struct test_random_rotation_msg expected_msgs[max_random_msgs] = {0};
		struct test_random_rotation_msg actual_msgs[max_random_msgs] = {0};
		u32 const initial_seed = seed_base + run;
		u16 total_written_n_bytes = 0;
		size_t n_generated_msgs = 0;
		size_t n_expected_msgs = 0;
		size_t n_actual_msgs = 0;
		u16 readable_n_bytes = 0;
		u16 useful_capacity = 0;
		struct test_random_rotation_stats run_stats = {0};
		struct nvmeib_pet_journalbuf committed_stream = {0};
		struct nvmeib_pet_journalbuf_header committed_header = {0};

		srandom(initial_seed);
		nvmeib_pet_journal_protect_prefix(&journal);
		while (total_written_n_bytes < target_written_n_bytes) {
			u16 written = 0;
			u16 const message_index = 0x0800 + (run * max_random_msgs) + n_generated_msgs;

			BUG_ON(n_generated_msgs >= max_random_msgs);
			written = __test_random_rotation_write_msg(&journal, message_index,
								   &generated_msgs[n_generated_msgs]);
			total_written_n_bytes += written;
			++n_generated_msgs;
		}

		BUG_ON(journal.journalbuf.max_written_bytes > sizeof(buffer));
		BUG_ON(journal.journalbuf.protected_prefix != NVMEIB_PET_JOURNALBUF_HEADER_SIZE);
		readable_n_bytes = journal.journalbuf.max_written_bytes - journal.journalbuf.protected_prefix;
		n_actual_msgs = __test_read_random_rotation_msgs(&journal.journalbuf, actual_msgs, max_random_msgs, &run_stats);
		BUG_ON(run_stats.spacer_bytes + run_stats.padding_bytes > readable_n_bytes);
		useful_capacity = readable_n_bytes - run_stats.spacer_bytes - run_stats.padding_bytes;
		n_expected_msgs = __test_make_random_rotation_expectation(
			generated_msgs, n_generated_msgs, useful_capacity, expected_msgs);

		committed_stream = journal.journalbuf;
		nvmeib_pet_journal_commit(&journal);
		memcpy(&committed_header, committed_stream.data.iov_base, sizeof(committed_header));
		BUG_ON(committed_header.commit_id != (u64)COMMIT_ID);
		BUG_ON(committed_header.journalbuf_size != committed_stream.max_written_bytes);
		__test_check_trace_clock(committed_header.trace_clock);

		__test_random_rotation_write_journal_file(initial_seed, &committed_stream);
		__test_random_rotation_write_expected_file(initial_seed, expected_msgs, n_expected_msgs,
							   committed_header.trace_clock);
		__test_compare_random_rotation_msgs(expected_msgs, n_expected_msgs, actual_msgs, n_actual_msgs);
		printf("Random rotation test[%u]: seed=0x%08x useful_msgs=%u useful_msg_bytes=%u spacer_bytes=%u padding_bytes=%u\n",
		       run, initial_seed, run_stats.useful_msgs, run_stats.useful_msg_bytes,
		       run_stats.spacer_bytes, run_stats.padding_bytes);

	}
}

void test_stream_write_supported_arg_types(void)
{
	struct nvmeib_pet_journalbuf stream = {0};
	bool bool_arg = true;
	unsigned char unsigned_char_arg = 0x11;
	signed char signed_char_arg = -2;
	char char_arg = 0x12;
	uint16_t uint16_arg = 0x2122;
	int16_t int16_arg = -0x123;
	uint32_t uint32_arg = 0x31323334;
	int32_t int32_arg = -0x1234567;
	unsigned long unsigned_long_arg = 0x41424344UL;
	long long_arg = -0x12345678L;
	size_t size_arg = 0x51525354UL;
	ssize_t ssize_arg = -0x123456L;
	void *ptr_arg = &stream;
	void const *const_ptr_arg = &stream;
	size_t written = 0;
	size_t expected_payload_n_bytes = 0;

	__test_stream_reset(&stream);
	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0401,
						bool_arg,
						unsigned_char_arg,
						signed_char_arg,
						char_arg,
						uint16_arg,
						int16_arg,
						uint32_arg,
						int32_arg,
						unsigned_long_arg,
						long_arg,
						size_arg,
						ssize_arg);
	expected_payload_n_bytes = sizeof(bool_arg) +
				   sizeof(unsigned_char_arg) +
				   sizeof(signed_char_arg) +
				   sizeof(char_arg) +
				   sizeof(uint16_arg) +
				   sizeof(int16_arg) +
				   sizeof(uint32_arg) +
				   sizeof(int32_arg) +
				   sizeof(unsigned_long_arg) +
				   sizeof(long_arg) +
				   sizeof(size_arg) +
				   sizeof(ssize_arg);

	BUG_ON(written != sizeof(struct nvmeib_pet_journalbuf_message_header) + expected_payload_n_bytes);

	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0402, ptr_arg, const_ptr_arg);
	BUG_ON(written != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(ptr_arg) + sizeof(const_ptr_arg));
}

void test_stream_write_args_are_evaluated_once(void)
{
	struct nvmeib_pet_journalbuf stream = {0};
	int arg_count = 0;
	size_t start = 0;
	size_t written = 0;
	u8 expected = 1;

	__test_stream_reset(&stream);
	start = stream.max_written_bytes;
	written = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(&stream, 0x0301, (u8)++arg_count);

	BUG_ON(written != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(expected));
	BUG_ON(arg_count != 1);
	__test_check_msg_header(start, 0x0301, sizeof(expected));
	__test_check_payload(start, &expected, sizeof(expected));
}

static struct nvmeib_pet_journal* test_get_pet_journal(struct nvmeib_pet_journal* journal, unsigned* calls)
{
	(*calls)++;
	return journal;
}

void test_io_pet_macro_args_are_evaluated_once(void)
{
	struct perf_test_controller perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE),
		.memcpy_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE)
	};
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&perf_controller.base, true);
	unsigned journal_get_count = 0;
	int arg_count = 0;
	u16 written;

	written = PET_MSG_NORM(
		test_get_pet_journal(&journal, &journal_get_count),
		"io_pet_single_eval(arg=%d)", ++arg_count);

	BUG_ON(written == 0);
	BUG_ON(journal_get_count != 1);
	BUG_ON(arg_count != 1);

	nvmeib_pet_journal_commit(&journal);
	free(perf_controller.msgs_buffer.iov_base);
	free(perf_controller.memcpy_buffer.iov_base);
}

void test_io_pet_inactive_journal_does_not_evaluate_args(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(NULL, true);
	unsigned journal_get_count = 0;
	int arg_count = 0;
	u16 written;

	written = PET_MSG_NORM(
		test_get_pet_journal(&journal, &journal_get_count),
		"io_pet_inactive(arg=%d)", ++arg_count);

	BUG_ON(written != 0);
	BUG_ON(journal_get_count != 1);
	BUG_ON(arg_count != 0);
}

void test_inactive_journal_protect_prefix_is_noop(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(NULL, true);

	nvmeib_pet_journal_protect_prefix(&journal);

	BUG_ON(nvmeib_pet_journal_is_activated(&journal));
	BUG_ON(journal.journalbuf.protected_prefix != 0);
}

void test_journal_protect_prefix_after_context(void)
{
	struct perf_test_controller perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE),
		.memcpy_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE)
	};
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&perf_controller.base, true);
	struct nvmeib_pet_journalbuf_header committed_header = {0};
	u16 protected_prefix = 0;
	u16 expected_journalbuf_size = 0;

	nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, 0x0601, (u8)0x01);
	nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, 0x0602, (u8)0x02);
	protected_prefix = journal.journalbuf.max_written_bytes;
	nvmeib_pet_journal_protect_prefix(&journal);
	nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, 0x0603, (u8)0x03);

	__test_check_protected_area(&journal.journalbuf, protected_prefix);
	BUG_ON(journal.journalbuf.max_written_bytes <= protected_prefix);
	expected_journalbuf_size = journal.journalbuf.max_written_bytes;

	nvmeib_pet_journal_commit(&journal);
	memcpy(&committed_header, perf_controller.msgs_buffer.iov_base, sizeof(committed_header));
	BUG_ON(committed_header.commit_id != (u64)COMMIT_ID);
	BUG_ON(committed_header.journalbuf_size != expected_journalbuf_size);
	__test_check_trace_clock(committed_header.trace_clock);

	free(perf_controller.msgs_buffer.iov_base);
	free(perf_controller.memcpy_buffer.iov_base);
}

struct release_cookie_test_controller {
	struct nvmeib_pet_base_controller base;
	struct iovec buffer;
	s16 release_cpu;
	s16 put_release_cpu;
	struct iovec put_buffer_data;
	int put_calls;
	int flush_calls;
};

static struct nvmeib_pet_buffer __release_cookie_test_get_buffer(struct nvmeib_pet_base_controller const *base)
{
	struct release_cookie_test_controller *self = (struct release_cookie_test_controller *)base;
	return (struct nvmeib_pet_buffer){
		.data = self->buffer,
		.release_cpu = self->release_cpu,
	};
}

static void __release_cookie_test_put_buffer(struct nvmeib_pet_base_controller const *base,
					     struct nvmeib_pet_buffer buffer)
{
	struct release_cookie_test_controller *self = (struct release_cookie_test_controller *)base;
	self->put_calls++;
	self->put_release_cpu = buffer.release_cpu;
	self->put_buffer_data = buffer.data;
}

static void __release_cookie_test_flush(struct nvmeib_pet_base_controller const *base,
					enum nvmeib_pet_severity severity, struct iovec const data)
{
	struct release_cookie_test_controller *self = (struct release_cookie_test_controller *)base;
	(void)severity;
	(void)data;
	self->flush_calls++;
}

void test_journal_returns_release_cpu_to_controller(void)
{
	struct release_cookie_test_controller controller = {
		.base = {
			.flush = __release_cookie_test_flush,
			.get_buffer = __release_cookie_test_get_buffer,
			.put_buffer = __release_cookie_test_put_buffer,
		},
		.buffer = iovec_malloc(256),
		.release_cpu = 7,
		.put_release_cpu = NVMEIB_PET_NO_RELEASE_CPU,
	};
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&controller.base, true);

	BUG_ON(journal.release_cpu != 7);
	nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, 0x77, (u8)0x11);
	nvmeib_pet_journal_commit(&journal);
	BUG_ON(controller.put_calls != 1);
	BUG_ON(controller.put_release_cpu != 7);
	free(controller.buffer.iov_base);
}

void test_inactive_journal_uses_no_release_cpu(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(NULL, true);

	BUG_ON(nvmeib_pet_journal_is_activated(&journal));
	BUG_ON(journal.release_cpu != NVMEIB_PET_NO_RELEASE_CPU);
	nvmeib_pet_journal_commit(&journal);
}

void test_zero_length_owned_buffer_is_inactive_but_returned(void)
{
	struct release_cookie_test_controller controller = {
		.base = {
			.flush = __release_cookie_test_flush,
			.get_buffer = __release_cookie_test_get_buffer,
			.put_buffer = __release_cookie_test_put_buffer,
		},
		.buffer = iovec_malloc(NVMEIB_PET_MIN_JOURNAL_N_BYTES),
		.release_cpu = 11,
		.put_release_cpu = NVMEIB_PET_NO_RELEASE_CPU,
	};
	struct nvmeib_pet_journal journal = {0};
	unsigned journal_get_count = 0;
	int arg_count = 0;
	u16 written = 0;

	journal = nvmeib_pet_journal_make(&controller.base, true);
	journal.journalbuf.data.iov_len = 0;

	written = PET_MSG_NORM(
		test_get_pet_journal(&journal, &journal_get_count),
		"io_pet_zero_length_buffer(arg=%d)", ++arg_count);

	BUG_ON(nvmeib_pet_journal_is_activated(&journal));
	BUG_ON(written != 0);
	BUG_ON(journal_get_count != 1);
	BUG_ON(arg_count != 0);

	nvmeib_pet_journal_commit(&journal);
	BUG_ON(controller.flush_calls != 0);
	BUG_ON(controller.put_calls != 1);
	BUG_ON(controller.put_release_cpu != 11);
	BUG_ON(controller.put_buffer_data.iov_base != controller.buffer.iov_base);
	BUG_ON(controller.put_buffer_data.iov_len != 0);

	free(controller.buffer.iov_base);
}

static struct nvmeib_pet_journalbuf_message_header __load_msg_header_from(u8 const* msg_start)
{
	struct nvmeib_pet_journalbuf_message_header header = {0};
	memcpy(&header, msg_start, sizeof(header));
	return header;
}

void test_journal_timestamp(void)
{
	struct perf_test_controller perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE),
		.memcpy_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE)
	};
	//not optimal, but better then nothing - I don't want to develop reader in C
	//if you want to be sure that message indexes are correct, run pet_messages.py script and see the result
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&perf_controller.base, true);

	u8 const* const msg1_start = (u8 const*)(journal.journalbuf.data.iov_base + journal.journalbuf.max_written_bytes);
	u64 const ticks_before1 = nvmeib_pet_get_trace_time_ticks();
	u16 const written1 = nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, 0x10, (u8)0x11);
	u64 const ticks_after1 = nvmeib_pet_get_trace_time_ticks();
	struct nvmeib_pet_journalbuf_message_header const header1 = __load_msg_header_from(msg1_start);

	u8 const* const msg2_start = (u8 const*)(journal.journalbuf.data.iov_base + journal.journalbuf.max_written_bytes);
	u64 const ticks_before2 = nvmeib_pet_get_trace_time_ticks();
	u16 const written2 = nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, 0x20, (u8)0x22);
	u64 const ticks_after2 = nvmeib_pet_get_trace_time_ticks();
	struct nvmeib_pet_journalbuf_message_header const header2 = __load_msg_header_from(msg2_start);

	BUG_ON(written1 != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8));
	BUG_ON(written2 != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8));
	BUG_ON(header1.message_id != 0x10 + 1);
	BUG_ON(header2.message_id != 0x20 + 1);
	BUG_ON(header1.msg.args_n_bytes != sizeof(u8));
	BUG_ON(header2.msg.args_n_bytes != sizeof(u8));
	BUG_ON(header1.msg.timestamp == 0);
	BUG_ON(header2.msg.timestamp == 0);
	BUG_ON(header1.msg.timestamp < ticks_before1);
	BUG_ON(header1.msg.timestamp > ticks_after1);
	BUG_ON(header2.msg.timestamp < ticks_before2);
	BUG_ON(header2.msg.timestamp > ticks_after2);
	BUG_ON(*(msg1_start + sizeof(struct nvmeib_pet_journalbuf_message_header)) != 0x11);
	BUG_ON(*(msg2_start + sizeof(struct nvmeib_pet_journalbuf_message_header)) != 0x22);

	nvmeib_pet_journal_commit(&journal);

	free(perf_controller.msgs_buffer.iov_base);
	free(perf_controller.memcpy_buffer.iov_base);
}

void test_journal_add_msg_accepts_pointer_arg(void)
{
	struct perf_test_controller perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE),
		.memcpy_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE)
	};
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&perf_controller.base, true);
	u8 const value = 0x33;
	u8 const* const ptr = &value;
	u8 const* const msg_start = (u8 const*)(journal.journalbuf.data.iov_base + journal.journalbuf.max_written_bytes);
	void const* written_ptr = NULL;
	u16 const written = nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, 0x30, ptr);
	struct nvmeib_pet_journalbuf_message_header const header = __load_msg_header_from(msg_start);

	memcpy(&written_ptr, msg_start + sizeof(struct nvmeib_pet_journalbuf_message_header), sizeof(written_ptr));

	BUG_ON(written != sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(ptr));
	BUG_ON(header.message_id != 0x30 + 1);
	BUG_ON(header.msg.args_n_bytes != sizeof(ptr));
	BUG_ON(written_ptr != ptr);

	nvmeib_pet_journal_commit(&journal);

	free(perf_controller.msgs_buffer.iov_base);
	free(perf_controller.memcpy_buffer.iov_base);
}

struct perf_test_stats {
	u32 total_messages;
	u32 total_bytes;
	u64 elapsed_ns; /* Elapsed time in nanoseconds */
};

static void merge_stats(struct perf_test_stats* dest, struct perf_test_stats const src)
{
	dest->total_messages += src.total_messages;
	dest->total_bytes += src.total_bytes;
	dest->elapsed_ns += src.elapsed_ns;
}

static void update_stats_on_msg_written(struct perf_test_stats* dest, size_t bytes)
{
	if (bytes){
		dest->total_messages += 1;
		dest->total_bytes += bytes;
	}
}

static void print_perf_report(const struct perf_test_stats* stats, const char* name)
{
	double const total_elapsed_us = stats->elapsed_ns / 1000.0;
	double const total_elapsed_ms = stats->elapsed_ns / 1000000.0;
	double const throughput_kb_per_ms = (stats->total_bytes / 1024.0) / total_elapsed_ms;

	printf("=== %s Test ===\n", name);

	if (stats->total_messages > 0) {
		double const throughput_msgs_per_ms = stats->total_messages / total_elapsed_ms;

		printf("  Total messages written (all runs): %u\n", stats->total_messages);
		printf("  Total bytes written (all runs): %u (%.2f MB)\n", stats->total_bytes, stats->total_bytes / (1024.0 * 1024.0));
		printf("  Total time (all runs): %llu ns (%.2f us, %.2f ms)\n",
			(unsigned long long)stats->elapsed_ns, total_elapsed_us, total_elapsed_ms);
		printf("  Throughput: %.2f KB/ms, %.2f messages/ms\n",
			throughput_kb_per_ms, throughput_msgs_per_ms);
	} else {
		printf("  Total bytes copied (all runs): %u (%.2f MB)\n", stats->total_bytes, stats->total_bytes / (1024.0 * 1024.0));
		printf("  Total time (all runs): %llu ns (%.2f us, %.2f ms)\n",
			(unsigned long long)stats->elapsed_ns, total_elapsed_us, total_elapsed_ms);
		printf("  Throughput: %.2f KB/ms\n", throughput_kb_per_ms);
	}
	printf("\n");
}

static u64 __calc_timespec_diff_ns(const struct timespec *curr, const struct timespec *prev)
{
	struct timespec res = {0};
	res.tv_sec  = curr->tv_sec  - prev->tv_sec;
	res.tv_nsec = curr->tv_nsec - prev->tv_nsec;
	if (res.tv_nsec < 0) {
		res.tv_sec--;
		res.tv_nsec += 1000000000L;
	}
	return (u64)res.tv_sec * 1000000000ULL + (u64)res.tv_nsec;
}

static struct perf_test_stats __test_performance_memcpy(struct iovec dest, const struct iovec src)
{
	struct timespec start_time, end_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	BUG_ON(src.iov_len != dest.iov_len);

	memcpy(dest.iov_base, src.iov_base, src.iov_len);

	clock_gettime(CLOCK_MONOTONIC, &end_time);

	return (struct perf_test_stats){
		.total_messages = 0,
		.total_bytes = src.iov_len,
		.elapsed_ns = __calc_timespec_diff_ns(&end_time, &start_time)
	};
}

static struct perf_test_stats __test_performance_journal_impl(struct nvmeib_pet_journal* journal,
							     size_t target_written_n_bytes,
							     bool allow_rotation)
{
	struct perf_test_stats stats = {0};
	u32 arg_counter = 0;

	size_t written = 0;
	struct timespec start_time, end_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	#define TEST_PERF_CAN_WRITE_NEXT() \
		(allow_rotation || \
		 (journal->journalbuf.write_offset == journal->journalbuf.max_written_bytes && \
		  (size_t)journal->journalbuf.write_offset + NVMEIB_PET_MAX_MSG_N_BYTES <= journal->journalbuf.data.iov_len))
	#define TEST_PERF_WRITE(msg_expr) \
	do { \
		if (!TEST_PERF_CAN_WRITE_NEXT()) { \
			goto done; \
		} \
		written = (msg_expr); \
		BUG_ON(!written); \
		update_stats_on_msg_written(&stats, written); \
	} while (0)

	while (stats.total_bytes < target_written_n_bytes) {
		arg_counter += 1;
		arg_counter %= 13;
		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_1arg; val=%d", (int)arg_counter));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_2arg; v1=%d v2=%u",
			(int)arg_counter, (unsigned)arg_counter + 1));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_3arg; v1=%d v2=%u v3=%lld",
			(int)arg_counter, (unsigned)arg_counter + 1, (long long)arg_counter + 2));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_4arg; v1=%d v2=%u v3=%lld v4=%llu",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_5arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_6arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_7arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_8arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_9arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_10arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p v10=%hhd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal,
			(signed char)arg_counter + 7));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_11arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p v10=%hhd v11=%hhu",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal,
			(signed char)arg_counter + 7, (unsigned char)arg_counter + 8));

		TEST_PERF_WRITE(PET_MSG_NORM(journal, "msg_12arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p v10=%hhd v11=%hhu v12=%hd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal,
			(signed char)arg_counter + 7, (unsigned char)arg_counter + 8,
			(short)arg_counter + 9));
	}

done:
	#undef TEST_PERF_WRITE
	#undef TEST_PERF_CAN_WRITE_NEXT

	clock_gettime(CLOCK_MONOTONIC, &end_time);

	stats.elapsed_ns = __calc_timespec_diff_ns(&end_time, &start_time);

	return stats;
}

static struct perf_test_stats __test_performance_journal(struct nvmeib_pet_journal* journal)
{
	BUG_ON(journal->journalbuf.max_written_bytes > journal->journalbuf.data.iov_len);
	return __test_performance_journal_impl(
		journal,
		journal->journalbuf.data.iov_len - journal->journalbuf.max_written_bytes,
		false);
}

static u64 __test_performance_random_u64(void)
{
	return ((u64)__test_random_u32() << 32) | __test_random_u32();
}

static u16 __test_performance_write_random_warmup_msg(struct nvmeib_pet_journal* journal, u16 remaining)
{
	u16 const header_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header);
	u16 const min_msg_n_bytes = header_n_bytes + sizeof(u8);
	u16 max_u64_args = 0;
	u16 message_index = (u16)(__test_random_u32() & 0x7fff);
	u64 args[12] = {0};
	u16 idx = 0;

	BUG_ON(remaining < min_msg_n_bytes);
	for (idx = 0; idx < ARRAY_SIZE(args); ++idx) {
		args[idx] = __test_performance_random_u64();
	}

	if (remaining < header_n_bytes + sizeof(u64)) {
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL,
						  message_index, (u8)args[0]);
	}

	max_u64_args = (remaining - header_n_bytes) / sizeof(u64);
	if (max_u64_args > ARRAY_SIZE(args)) {
		max_u64_args = ARRAY_SIZE(args);
	}

	switch ((__test_random_u32() % max_u64_args) + 1) {
	case 1:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0]);
	case 2:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1]);
	case 3:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2]);
	case 4:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3]);
	case 5:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4]);
	case 6:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4], args[5]);
	case 7:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4], args[5], args[6]);
	case 8:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4], args[5], args[6], args[7]);
	case 9:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4], args[5], args[6], args[7],
						  args[8]);
	case 10:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4], args[5], args[6], args[7],
						  args[8], args[9]);
	case 11:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4], args[5], args[6], args[7],
						  args[8], args[9], args[10]);
	default:
		return nvmeib_pet_journal_add_msg(journal, NVMEIB_PET_SEVERITY_NORMAL, message_index,
						  args[0], args[1], args[2], args[3],
						  args[4], args[5], args[6], args[7],
						  args[8], args[9], args[10], args[11]);
	}
}

static void __test_performance_prepare_rotating_journal(struct nvmeib_pet_journal* journal)
{
	u16 const min_msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + sizeof(u8);

	while ((size_t)journal->journalbuf.write_offset + min_msg_n_bytes <= journal->journalbuf.data.iov_len) {
		u16 const remaining = journal->journalbuf.data.iov_len - journal->journalbuf.write_offset;
		size_t const written = __test_performance_write_random_warmup_msg(journal, remaining);

		BUG_ON(written == 0);
		BUG_ON(written > remaining);
	}
}

static struct perf_test_stats __test_performance_journal_rotating(struct nvmeib_pet_journal* journal)
{
	__test_performance_prepare_rotating_journal(journal);
	return __test_performance_journal_impl(
		journal,
		NVMEIB_PET_MAX_JOURNALBUF_SIZE - NVMEIB_PET_JOURNALBUF_HEADER_SIZE,
		true);
}

void test_performance(void)
{
	enum {
		num_runs = 10000,
		rotating_journal_n_bytes = 1024,
	};
	struct perf_test_controller perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE),
		.memcpy_buffer = iovec_malloc(NVMEIB_PET_MAX_JOURNALBUF_SIZE)
	};
	struct perf_test_controller rotating_perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(rotating_journal_n_bytes),
		.memcpy_buffer = {0}
	};
	printf("Performance test: Comparing journal write, rotating journal write, and raw memcpy\n");
	printf("Running each test %d times for averaging...\n\n", num_runs);

	struct perf_test_stats stats = {0};

	for (int run = 0; run < num_runs; run++) {
		struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&perf_controller.base, true);
		struct perf_test_stats run_stats = __test_performance_journal(&journal);
		merge_stats(&stats, run_stats);
		nvmeib_pet_journal_commit(&journal);
	}

	print_perf_report(&stats, "Journal Write");

	srandom(0x50455452);
	stats = (struct perf_test_stats){0};
	for (int run = 0; run < num_runs; run++) {
		struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&rotating_perf_controller.base, true);
		struct perf_test_stats run_stats = __test_performance_journal_rotating(&journal);
		merge_stats(&stats, run_stats);
		nvmeib_pet_journal_commit(&journal);
	}

	print_perf_report(&stats, "Journal Rotating Write");

	stats = (struct perf_test_stats){0};
	for (int run = 0; run < num_runs; run++) {
		struct perf_test_stats run_stats = __test_performance_memcpy(perf_controller.msgs_buffer, perf_controller.memcpy_buffer);
		merge_stats(&stats, run_stats);
	}

	print_perf_report(&stats, "Raw memcpy");

	free(rotating_perf_controller.msgs_buffer.iov_base);
	free(perf_controller.memcpy_buffer.iov_base);
	free(perf_controller.msgs_buffer.iov_base);
}

enum colors{green=1, blue=2, red=3};

void test_multiple_messages(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&file_pet_controller.base, true);

	PET_MSG_NORM(&journal, "operation created; o=%p offset=0x%llx size=%u", &journal, (unsigned long long)(4096*4096), 8192);
	PET_MSG_NORM(&journal, "operation completed; raid uuid=%x", 0xb1c69d3e);
	PET_MSG_NORM(&journal, "operation compact flag=0x%x", (u8)0xab);
	PET_MSG_NORM(&journal, "operation completed; rv=%d", -1);

	nvmeib_pet_journal_commit(&journal);
}

void test_enums(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&file_pet_controller.base, true);

	PET_MSG_NORM(&journal, "I know to print enums; green=%d<enum colors> blue=%d<enum colors> red=%d<coooooolors>", green, blue, red);

	nvmeib_pet_journal_commit(&journal);
}

union topo_status{
	struct{
		u8 n_sgmnts;
		struct {
			u8 sgmnt;
			enum colors mode : 8;
		} __attribute__((packed)) dgrd_sgmnts[2] ;
	} info;
	u64 all;
};

union pet_test_blkset_info {
	struct {
		uint32_t txid : 20;
		uint32_t dirty : 12;
	} bits;
	uint32_t all;
};

void test_structs_and_unions(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&file_pet_controller.base, true);

	union pet_test_blkset_info const blkset_info = {.bits = {.txid = 1977, .dirty = 6}};
	PET_MSG_NORM(&journal,
		"pet_test_blkset_info txid=%u(1977) dirty=%u(6) all=%u all=%u<union pet_test_blkset_info>",
		(uint32_t)blkset_info.bits.txid, (uint32_t)blkset_info.bits.dirty, blkset_info.all, blkset_info.all);

	union topo_status const status = {
		.info = {.n_sgmnts = 4, .dgrd_sgmnts ={{2,blue}, {3, red}}}
	};

	PET_MSG_NORM(&journal,
		"topo_status n_sgmnts=%hhu(4) dgrd_sgmnts={{%hhu, %hhu},{%hhu, %hhu}} all=%llx all=%llx<union topo_status>",
		status.info.n_sgmnts,
		status.info.dgrd_sgmnts[0].sgmnt, (uint8_t)status.info.dgrd_sgmnts[0].mode,
		status.info.dgrd_sgmnts[1].sgmnt, (uint8_t)status.info.dgrd_sgmnts[1].mode,
		status.all, status.all);

	PET_MSG_NORM(&journal, "topo_status 0 all=%llx<union topo_status>", 0LLU);

	nvmeib_pet_journal_commit(&journal);
}


void test_errno(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&file_pet_controller.base, true);

	PET_MSG_NORM(&journal, "errno rv=%d<const errno> rv2=%x<const errno>", EIO, 0xDEADBEAF);

	nvmeib_pet_journal_commit(&journal);
}


int main(int argc, char* argv[]){
	int_cpu_freq_tsc_offset_jiffies();  // measure CPU freq, initialize tsc_khz
	char const* fname = argc > 1 ? argv[1] : "build/test.pet";
	__test_random_rotation_output_dir = argc > 2 ? argv[2] : "build/rotations";
	if (argc <= 2) {
		__test_mkdir_if_needed("build");
	}
	__test_mkdir_if_needed(__test_random_rotation_output_dir);

	int const fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
		perror("failed to open 'io_pet_messages.binlog' file");
		exit(1);
	}

	file_pet_controller.fd_output = fd;

	{
		test_msg_header_layout();
		test_stream_header_layout();
		test_stream_write_all_arg_counts();
		test_stream_protect_empty_prefix();
		test_stream_protect_prefix_expands_protected_area();
		test_stream_write_mixed_size_args();
		test_stream_write_zero_offset_is_stored_as_one();
		test_stream_rotation_small_over_large_leaves_spacer();
		test_stream_rotation_small_over_large_consumes_next_without_tiny_gap();
		test_stream_rotation_large_over_small_messages_reduces_eof_for_tiny_tail();
		test_stream_rotation_uses_unwritten_tail_after_eof();
		test_stream_allocate_rotate_final_update_does_not_extend_eof();
		test_stream_rotation_end_wrap_shrinks_eof_and_writes_from_prefix();
		test_stream_commit_sets_journalbuf_size();
		test_journal_random_rotation_retains_last_messages();
		test_stream_write_supported_arg_types();
		test_stream_write_args_are_evaluated_once();
		test_io_pet_macro_args_are_evaluated_once();
		test_io_pet_inactive_journal_does_not_evaluate_args();
		test_inactive_journal_protect_prefix_is_noop();
		test_journal_protect_prefix_after_context();
		test_journal_returns_release_cpu_to_controller();
		test_inactive_journal_uses_no_release_cpu();
		test_zero_length_owned_buffer_is_inactive_but_returned();
		test_journal_timestamp();
		test_journal_add_msg_accepts_pointer_arg();
		test_performance();
		test_multiple_messages();
		test_enums();
		test_structs_and_unions();
		test_errno();
	}

	close(file_pet_controller.fd_output);
	file_pet_controller.fd_output = -1;
}
