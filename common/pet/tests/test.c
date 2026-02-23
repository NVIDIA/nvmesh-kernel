#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define WARN(condition, format, ...) ({assert(!(condition)); (void)format;})

#include "nvmeib_pet_specification.h"

//{{{ instantiate the pet framework
#define TEST_PET_SECTION  "test_pet_msgs"

extern const char __start_test_pet_msgs[];
extern const char __stop_test_pet_msgs[];

#define PET_MSG(pet_journal, msg, severity,...)   \
({																																		\
    u16 __io_pet_msg_written = 0;																										\
	static const char NVMESH_USED NVMESH_SECTION(TEST_PET_SECTION) __io_pet_msg[] = msg;												\
    u16 const __io_pet_msg_offset = (u64)(&__io_pet_msg) - (u64)(&__start_test_pet_msgs); 												\
	struct nvmeib_pet_journal* __io_pet_journal = (struct nvmeib_pet_journal*)(pet_journal); /*droping const*/							\
	nvmeib_pet_journal_add_msg_verify_format(__io_pet_msg, __VA_ARGS__);															\
	__io_pet_msg_written = nvmeib_pet_journal_add_msg(__io_pet_journal, severity, NVMEIB_PET_MSG(__io_pet_msg_offset, __VA_ARGS__)); 	\
    __io_pet_msg_written;                                                                                                       		\
})

#define PET_MSG_NORM(pet_journal, msg, ...) PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_NORMAL, __VA_ARGS__)

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

struct iovec __file_pet_controller_get_buffer(struct nvmeib_pet_base_controller const* self)
{
	void* ptr = malloc(4096);
	(void)self;
	return (struct iovec){.iov_base = ptr, .iov_len = ptr ? 4096 : 0};
}

void __file_pet_controller_put_buffer(struct nvmeib_pet_base_controller const* self, struct iovec data)
{
	(void)self;
	free(data.iov_base);
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

static struct iovec __perf_test_get_buffer(struct nvmeib_pet_base_controller const* base)
{
	__auto_type self = (struct perf_test_controller const*)base;
	return self->msgs_buffer;
}

static void __perf_test_put_buffer(struct nvmeib_pet_base_controller const* self, struct iovec data)
{ (void)self; (void)data; }

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
static u8 __test_message_x_memory[256] = {0};
static struct iovec __test_message_iovec = {.iov_base = __test_message_x_memory, .iov_len = ARRAY_SIZE(__test_message_x_memory)};
/* Compare expected layout with actual buffer; expected includes entity header (commit_id u8 + num_messages u2) */
static void __bug_on_written_message_content_not_equal(u8 const* memory, size_t size)
{
	size_t idx = 0;
	for(idx = 0; idx < size; idx++){
		if(memory[idx] == '?')
			continue;
		if(memory[idx] != __test_message_x_memory[idx]){
			BUG();
		}
	}
}

void test_message_1(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04d3, (int8_t)0x11);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1; //offset(2) + n_args(1) + (type(1) + timestamp(8)) + (type(1) + value(1))
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xd4, 0x04, //offset
		0x01, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11
	};

	BUG_ON(msg.offset != 0x04d3+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_2(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04d4,
				 (int8_t)               0x11,
				 (uint8_t)              0x12);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xd5, 0x04, //offset
		0x02, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12     //arg2: U_BYTE
	};

	BUG_ON(msg.offset != 0x04d4+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));

	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_3(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04d5,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xd6, 0x04, //offset
		0x03, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22     //arg3: S_SHORT
	};

	BUG_ON(msg.offset != 0x04d5+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_4(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04d6,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xd7, 0x04, //offset
		0x04, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22     //arg4: U_SHORT
	};

	BUG_ON(msg.offset != 0x04d6+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_5(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04d7,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xd8, 0x04, //offset
		0x05, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44     //arg5: S_INT
	};

	BUG_ON(msg.offset != 0x04d7+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_6(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04d8,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444,
				 (uint32_t)       0x44444445);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4 + 1 + 4; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value) + arg6(1 type + 4 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xd9, 0x04, //offset
		0x06, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44,    //arg5: S_INT
		0x05, 0x45, 0x44, 0x44, 0x44     //arg6: U_INT
	};

	BUG_ON(msg.offset != 0x04d8+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);
	BUG_ON(msg.type[6] != NVMEIB_PET_STORE_TYPE_U_INT || msg.value[6] != 0x44444445);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_7(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04d9,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444,
				 (uint32_t)       0x44444445,
				 (int64_t) 0x8888888888888888);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4 + 1 + 4 + 1 + 8; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value) + arg6(1 type + 4 value) + arg7(1 type + 8 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xda, 0x04, //offset
		0x07, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44,    //arg5: S_INT
		0x05, 0x45, 0x44, 0x44, 0x44,    //arg6: U_INT
		0x06, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88     //arg7: S_LONG_INT
	};

	BUG_ON(msg.offset != 0x04d9+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);
	BUG_ON(msg.type[6] != NVMEIB_PET_STORE_TYPE_U_INT || msg.value[6] != 0x44444445);
	BUG_ON(msg.type[7] != NVMEIB_PET_STORE_TYPE_S_LONG_INT || msg.value[7] != 0x8888888888888888);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_8(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04da,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444,
				 (uint32_t)       0x44444445,
				 (int64_t) 0x8888888888888888,
				 (uint64_t)0x8888888888888889);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4 + 1 + 4 + 1 + 8 + 1 + 8; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value) + arg6(1 type + 4 value) + arg7(1 type + 8 value) + arg8(1 type + 8 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xdb, 0x04, //offset
		0x08, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44,    //arg5: S_INT
		0x05, 0x45, 0x44, 0x44, 0x44,    //arg6: U_INT
		0x06, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg7: S_LONG_INT
		0x07, 0x89, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88     //arg8: U_LONG_INT
	};

	BUG_ON(msg.offset != 0x04da+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);
	BUG_ON(msg.type[6] != NVMEIB_PET_STORE_TYPE_U_INT || msg.value[6] != 0x44444445);
	BUG_ON(msg.type[7] != NVMEIB_PET_STORE_TYPE_S_LONG_INT || msg.value[7] != 0x8888888888888888);
	BUG_ON(msg.type[8] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[8] != 0x8888888888888889);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_9(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04db,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444,
				 (uint32_t)       0x44444445,
				 (int64_t) 0x8888888888888888,
				 (uint64_t)0x8888888888888889,
				 (void*)           (void*)0xaaaaaaaaaaaaaaaa);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4 + 1 + 4 + 1 + 8 + 1 + 8 + 1 + 8; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value) + arg6(1 type + 4 value) + arg7(1 type + 8 value) + arg8(1 type + 8 value) + arg9(1 type + 8 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xdc, 0x04, //offset
		0x09, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44,    //arg5: S_INT
		0x05, 0x45, 0x44, 0x44, 0x44,    //arg6: U_INT
		0x06, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg7: S_LONG_INT
		0x07, 0x89, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg8: U_LONG_INT
		0x07, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa     //arg9: U_LONG_INT (pointer)
	};

	BUG_ON(msg.offset != 0x04db+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);
	BUG_ON(msg.type[6] != NVMEIB_PET_STORE_TYPE_U_INT || msg.value[6] != 0x44444445);
	BUG_ON(msg.type[7] != NVMEIB_PET_STORE_TYPE_S_LONG_INT || msg.value[7] != 0x8888888888888888);
	BUG_ON(msg.type[8] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[8] != 0x8888888888888889);
	BUG_ON(msg.type[9] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[9] != (u64)(void*)0xaaaaaaaaaaaaaaaa);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_10(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04dc,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444,
				 (uint32_t)       0x44444445,
				 (int64_t) 0x8888888888888888,
				 (uint64_t)0x8888888888888889,
				 (void*)           (void*)0xaaaaaaaaaaaaaaaa,
				 (int8_t)               0x13);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4 + 1 + 4 + 1 + 8 + 1 + 8 + 1 + 8 + 1 + 1; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value) + arg6(1 type + 4 value) + arg7(1 type + 8 value) + arg8(1 type + 8 value) + arg9(1 type + 8 value) + arg10(1 type + 1 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xdd, 0x04, //offset
		0x0a, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44,    //arg5: S_INT
		0x05, 0x45, 0x44, 0x44, 0x44,    //arg6: U_INT
		0x06, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg7: S_LONG_INT
		0x07, 0x89, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg8: U_LONG_INT
		0x07, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,    //arg9: U_LONG_INT (pointer)
		0x00, 0x13     //arg10: S_BYTE
	};

	BUG_ON(msg.offset != 0x04dc+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);
	BUG_ON(msg.type[6] != NVMEIB_PET_STORE_TYPE_U_INT || msg.value[6] != 0x44444445);
	BUG_ON(msg.type[7] != NVMEIB_PET_STORE_TYPE_S_LONG_INT || msg.value[7] != 0x8888888888888888);
	BUG_ON(msg.type[8] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[8] != 0x8888888888888889);
	BUG_ON(msg.type[9] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[9] != (u64)(void*)0xaaaaaaaaaaaaaaaa);
	BUG_ON(msg.type[10] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[10] != 0x13);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_11(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04dd,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444,
				 (uint32_t)       0x44444445,
				 (int64_t) 0x8888888888888888,
				 (uint64_t)0x8888888888888889,
				 (void*)           (void*)0xaaaaaaaaaaaaaaaa,
				 (int8_t)               0x13,
				 (uint8_t)              0x14);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4 + 1 + 4 + 1 + 8 + 1 + 8 + 1 + 8 + 1 + 1 + 1 + 1; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value) + arg6(1 type + 4 value) + arg7(1 type + 8 value) + arg8(1 type + 8 value) + arg9(1 type + 8 value) + arg10(1 type + 1 value) + arg11(1 type + 1 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xde, 0x04, //offset
		0x0b, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44,    //arg5: S_INT
		0x05, 0x45, 0x44, 0x44, 0x44,    //arg6: U_INT
		0x06, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg7: S_LONG_INT
		0x07, 0x89, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg8: U_LONG_INT
		0x07, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,    //arg9: U_LONG_INT (pointer)
		0x00, 0x13,    //arg10: S_BYTE
		0x01, 0x14     //arg11: U_BYTE
	};

	BUG_ON(msg.offset != 0x04dd+1);
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);
	BUG_ON(msg.type[6] != NVMEIB_PET_STORE_TYPE_U_INT || msg.value[6] != 0x44444445);
	BUG_ON(msg.type[7] != NVMEIB_PET_STORE_TYPE_S_LONG_INT || msg.value[7] != 0x8888888888888888);
	BUG_ON(msg.type[8] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[8] != 0x8888888888888889);
	BUG_ON(msg.type[9] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[9] != (u64)(void*)0xaaaaaaaaaaaaaaaa);
	BUG_ON(msg.type[10] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[10] != 0x13);
	BUG_ON(msg.type[11] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[11] != 0x14);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_message_12(void)
{
	__auto_type const msg = NVMEIB_PET_MSG(0x04de,
				 (int8_t)               0x11,
				 (uint8_t)              0x12,
				 (int16_t)            0x2222,
				 (uint16_t)           0x2223,
				 (int32_t)        0x44444444,
				 (uint32_t)       0x44444445,
				 (int64_t) 0x8888888888888888,
				 (uint64_t)0x8888888888888889,
				 (void*)           (void*)0xaaaaaaaaaaaaaaaa,
				 (int8_t)               0x13,
				 (uint8_t)              0x14,
				 (int16_t)            0x3333);
	size_t const msg_size = 2 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 4 + 1 + 4 + 1 + 8 + 1 + 8 + 1 + 8 + 1 + 1 + 1 + 1 + 1 + 2; //offset(2) + timestamp(1 type + 8 value) + n_args(1) + arg1(1 type + 1 value) + arg2(1 type + 1 value) + arg3(1 type + 2 value) + arg4(1 type + 2 value) + arg5(1 type + 4 value) + arg6(1 type + 4 value) + arg7(1 type + 8 value) + arg8(1 type + 8 value) + arg9(1 type + 8 value) + arg10(1 type + 1 value) + arg11(1 type + 1 value) + arg12(1 type + 2 value)
	struct nvmeib_pet_stream stream = nvmeib_pet_stream_make(__test_message_iovec);

	u8 const memory_expected[128] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //commit_id
		0x01, 0x00, //num_messages
		0xdf, 0x04, //offset
		0x0c, //n_args
		'?', '?', '?', '?', '?', '?', '?', '?', '?', //timestamp variant - skip comparison
		0x00, 0x11,    //arg1: S_BYTE
		0x01, 0x12,    //arg2: U_BYTE
		0x02, 0x22, 0x22,    //arg3: S_SHORT
		0x03, 0x23, 0x22,    //arg4: U_SHORT
		0x04, 0x44, 0x44, 0x44, 0x44,    //arg5: S_INT
		0x05, 0x45, 0x44, 0x44, 0x44,    //arg6: U_INT
		0x06, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg7: S_LONG_INT
		0x07, 0x89, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,    //arg8: U_LONG_INT
		0x07, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,    //arg9: U_LONG_INT (pointer)
		0x00, 0x13,    //arg10: S_BYTE
		0x01, 0x14,    //arg11: U_BYTE
		0x02, 0x33, 0x33     //arg12: S_SHORT
	};

	BUG_ON(msg.offset != (0x04de + 1));
	BUG_ON(msg.value[0] == 0);
	BUG_ON(msg.type[0] != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(msg.type[1] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[1] != 0x11);
	BUG_ON(msg.type[2] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[2] != 0x12);
	BUG_ON(msg.type[3] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[3] != 0x2222);
	BUG_ON(msg.type[4] != NVMEIB_PET_STORE_TYPE_U_SHORT || msg.value[4] != 0x2223);
	BUG_ON(msg.type[5] != NVMEIB_PET_STORE_TYPE_S_INT || msg.value[5] != 0x44444444);
	BUG_ON(msg.type[6] != NVMEIB_PET_STORE_TYPE_U_INT || msg.value[6] != 0x44444445);
	BUG_ON(msg.type[7] != NVMEIB_PET_STORE_TYPE_S_LONG_INT || msg.value[7] != 0x8888888888888888);
	BUG_ON(msg.type[8] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[8] != 0x8888888888888889);
	BUG_ON(msg.type[9] != NVMEIB_PET_STORE_TYPE_U_LONG_INT || msg.value[9] != (u64)(void*)0xaaaaaaaaaaaaaaaa);
	BUG_ON(msg.type[10] != NVMEIB_PET_STORE_TYPE_S_BYTE || msg.value[10] != 0x13);
	BUG_ON(msg.type[11] != NVMEIB_PET_STORE_TYPE_U_BYTE || msg.value[11] != 0x14);
	BUG_ON(msg.type[12] != NVMEIB_PET_STORE_TYPE_S_SHORT || msg.value[12] != 0x3333);

	BUG_ON(nvmeib_pet_message_get_size(&msg) != msg_size);
	memset(__test_message_x_memory, 0, sizeof(__test_message_x_memory));
	BUG_ON(nvmeib_pet_message_write(&msg, &stream) != msg_size);
	__bug_on_written_message_content_not_equal(memory_expected, NVMEIB_PET_ENTITY_HEADER_SIZE + msg_size);
}

void test_get_store_type(void)
{
	// Test various types to ensure nvmeib_pet_get_store_type returns correct enum values
	BUG_ON(nvmeib_pet_get_store_type((bool)true) != NVMEIB_PET_STORE_TYPE_U_BYTE);
	BUG_ON(nvmeib_pet_get_store_type((uint8_t)0x12) != NVMEIB_PET_STORE_TYPE_U_BYTE);
	BUG_ON(nvmeib_pet_get_store_type((int8_t)0x11) != NVMEIB_PET_STORE_TYPE_S_BYTE);
	BUG_ON(nvmeib_pet_get_store_type((unsigned char)0x12) != NVMEIB_PET_STORE_TYPE_U_BYTE);
	BUG_ON(nvmeib_pet_get_store_type((signed char)0x11) != NVMEIB_PET_STORE_TYPE_S_BYTE);
	BUG_ON(nvmeib_pet_get_store_type((char)0x11) != NVMEIB_PET_STORE_TYPE_U_BYTE);

	BUG_ON(nvmeib_pet_get_store_type((uint16_t)0x2222) != NVMEIB_PET_STORE_TYPE_U_SHORT);
	BUG_ON(nvmeib_pet_get_store_type((int16_t)0x2222) != NVMEIB_PET_STORE_TYPE_S_SHORT);
	BUG_ON(nvmeib_pet_get_store_type((unsigned short)0x2222) != NVMEIB_PET_STORE_TYPE_U_SHORT);
	BUG_ON(nvmeib_pet_get_store_type((signed short)0x2222) != NVMEIB_PET_STORE_TYPE_S_SHORT);

	BUG_ON(nvmeib_pet_get_store_type((uint32_t)0x44444444) != NVMEIB_PET_STORE_TYPE_U_INT);
	BUG_ON(nvmeib_pet_get_store_type((int32_t)0x44444444) != NVMEIB_PET_STORE_TYPE_S_INT);
	BUG_ON(nvmeib_pet_get_store_type((unsigned int)0x44444444) != NVMEIB_PET_STORE_TYPE_U_INT);
	BUG_ON(nvmeib_pet_get_store_type((int)0x44444444) != NVMEIB_PET_STORE_TYPE_S_INT);

	BUG_ON(nvmeib_pet_get_store_type((uint64_t)0x8888888888888888ULL) != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(nvmeib_pet_get_store_type((int64_t)0x8888888888888888LL) != NVMEIB_PET_STORE_TYPE_S_LONG_INT);
	BUG_ON(nvmeib_pet_get_store_type((unsigned long long)0x8888888888888888ULL) != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(nvmeib_pet_get_store_type((long long)0x8888888888888888LL) != NVMEIB_PET_STORE_TYPE_S_LONG_INT);

	void* ptr = (void*)0xff00ff00ff00ff00;
	BUG_ON(nvmeib_pet_get_store_type(ptr) != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(nvmeib_pet_get_store_type((void const*)ptr) != NVMEIB_PET_STORE_TYPE_U_LONG_INT);

	size_t sz = 100;
	BUG_ON(nvmeib_pet_get_store_type(sz) != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	ssize_t ssz = -1;
	BUG_ON(nvmeib_pet_get_store_type(ssz) != NVMEIB_PET_STORE_TYPE_S_LONG_INT);
}

struct nvmeib_pet_variant __load_timestamp(u8 const* msg_start){
	struct nvmeib_pet_variant variant = {0};
	variant.type = msg_start[3]; //2 offset + 1 n_args + 1 timestamp type
	memcpy(&variant.value, msg_start + 4, nvmeib_pet_store_type_get_size(variant.type)); //timestamp value
	return variant;
}

void test_journal_timestamp(void)
{
	struct perf_test_controller perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(NVMEIB_PET_MAX_STREAM_SIZE),
		.memcpy_buffer = iovec_malloc(NVMEIB_PET_MAX_STREAM_SIZE)
	};
	//not optimal, but better then nothing - I don't want to develop reader in C
	//if you want to be sure that offsets are correct, run pe_messages.py script and see the result
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&perf_controller.base, true);
	__auto_type const msg1 = NVMEIB_PET_MSG(0x10, (u8)0x11);
	__auto_type const msg2 = NVMEIB_PET_MSG(0x20, (u8)0x22);

	BUG_ON(journal.prev_timestamp_ns != 0);
	u8 const* const msg1_start = (u8 const*)(journal.stream.data.iov_base + journal.stream.written_bytes);
	nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, msg1);
	struct nvmeib_pet_variant const timestamp1 = __load_timestamp(msg1_start);

	u8 const* const msg2_start = (u8 const*)(journal.stream.data.iov_base + journal.stream.written_bytes);
	nvmeib_pet_journal_add_msg(&journal, NVMEIB_PET_SEVERITY_NORMAL, msg2);
	struct nvmeib_pet_variant const timestamp2 = __load_timestamp(msg2_start);

	BUG_ON(timestamp1.type != NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(timestamp1.value == 0);

	BUG_ON(timestamp2.type == NVMEIB_PET_STORE_TYPE_U_LONG_INT);
	BUG_ON(timestamp2.value == 0);

	BUG_ON(journal.prev_timestamp_ns != timestamp1.value + timestamp2.value);

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

static struct perf_test_stats __test_performance_journal(struct nvmeib_pet_journal* journal)
{
	struct perf_test_stats stats = {0};
	u32 arg_counter = 0;

	size_t written = true;
	struct timespec start_time, end_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	while (written) {
		arg_counter += 1;
		arg_counter %= 13;
		written = PET_MSG_NORM(journal, "msg_1arg; val=%d", (int)arg_counter);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_2arg; v1=%d v2=%u",
			(int)arg_counter, (unsigned)arg_counter + 1);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_3arg; v1=%d v2=%u v3=%lld",
			(int)arg_counter, (unsigned)arg_counter + 1, (long long)arg_counter + 2);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_4arg; v1=%d v2=%u v3=%lld v4=%llu",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_5arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_6arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_7arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_8arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_9arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_10arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p v10=%hhd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal,
			(signed char)arg_counter + 7);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_11arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p v10=%hhd v11=%hhu",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal,
			(signed char)arg_counter + 7, (unsigned char)arg_counter + 8);
		update_stats_on_msg_written(&stats, written);

		written = PET_MSG_NORM(journal, "msg_12arg; v1=%d v2=%u v3=%lld v4=%llu v5=%hd v6=%hu v7=%hhd v8=%p v9=%p v10=%hhd v11=%hhu v12=%hd",
			(int)arg_counter, (unsigned)arg_counter + 1,
			(long long)arg_counter + 2, (unsigned long long)arg_counter + 3,
			(short)arg_counter + 4, (unsigned short)arg_counter + 5,
			(signed char)arg_counter + 6, &stats, journal,
			(signed char)arg_counter + 7, (unsigned char)arg_counter + 8,
			(short)arg_counter + 9);
		update_stats_on_msg_written(&stats, written);
	}

	clock_gettime(CLOCK_MONOTONIC, &end_time);

	stats.elapsed_ns = __calc_timespec_diff_ns(&end_time, &start_time);

	return stats;
}

void test_performance(void)
{
	const int num_runs = 1000;
	struct perf_test_controller perf_controller = {
		.base = {
			.flush = __perf_test_flush,
			.get_buffer = __perf_test_get_buffer,
			.put_buffer = __perf_test_put_buffer
		},
		.msgs_buffer = iovec_malloc(NVMEIB_PET_MAX_STREAM_SIZE),
		.memcpy_buffer = iovec_malloc(NVMEIB_PET_MAX_STREAM_SIZE)
	};
	printf("Performance test: Comparing journal write vs raw memcpy\n");
	printf("Running each test %d times for averaging...\n\n", num_runs);

	struct perf_test_stats stats = {0};

	for (int run = 0; run < num_runs; run++) {
		struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&perf_controller.base, true);
		struct perf_test_stats run_stats = __test_performance_journal(&journal);
		merge_stats(&stats, run_stats);
		nvmeib_pet_journal_commit(&journal);
	}

	print_perf_report(&stats, "Journal Write");

	stats = (struct perf_test_stats){0};
	for (int run = 0; run < num_runs; run++) {
		struct perf_test_stats run_stats = __test_performance_memcpy(perf_controller.msgs_buffer, perf_controller.memcpy_buffer);
		merge_stats(&stats, run_stats);
	}

	print_perf_report(&stats, "Raw memcpy");

	free(perf_controller.memcpy_buffer.iov_base);
	free(perf_controller.msgs_buffer.iov_base);
}

enum colors{green=1, blue=2, red=3};

void test_multiple_messages(void)
{
	struct nvmeib_pet_journal journal = nvmeib_pet_journal_make(&file_pet_controller.base, true);

	PET_MSG_NORM(&journal, "operation created; o=%p offset=0x%llx size=%u", &journal, (unsigned long long)(4096*4096), 8192);
	PET_MSG_NORM(&journal, "operation completed; raid uuid=%x", 0xb1c69d3e);
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
	char const* fname = argc > 1 ? argv[1] : "test.pet";
	int const fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
		perror("failed to open 'io_pet_messages.binlog' file");
		exit(1);
	}

	file_pet_controller.fd_output = fd;

	{
		test_get_store_type();
		test_message_1();
		test_message_2();
		test_message_3();
		test_message_4();
		test_message_5();
		test_message_6();
		test_message_7();
		test_message_8();
		test_message_9();
		test_message_10();
		test_message_11();
		test_message_12();
		test_journal_timestamp();
		test_performance();
		test_multiple_messages();
		test_enums();
		test_structs_and_unions();
		test_errno();
	}

	close(file_pet_controller.fd_output);
	file_pet_controller.fd_output = -1;
}
