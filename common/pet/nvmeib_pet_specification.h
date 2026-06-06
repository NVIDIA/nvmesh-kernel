/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef __NVMEIB_PET_SPECIFICATION_H__
#define __NVMEIB_PET_SPECIFICATION_H__

#if defined(__KERNEL__)
	#include <linux/string.h>
	#include "common/kr_version.h"
#else
	#include <stddef.h>
	#include <string.h>
	#include <sys/uio.h>
#endif
#include "compat/kr_incs_types.h"
#include "compat/kr_incs_asserts.h"
#include "compat/kr_incs_time_rdtsc.h"
#include "compat/kr_incs_time_jiff.h"
#include "common/nvmeib_math.h"

//{{{ OS integration

#if defined(__KERNEL__)
	#include "nvmeib_trace.h"

	static inline u64 nvmeib_pet_get_trace_time_ns(void)
	{
		return nvmeib_trace_get_time_ns();
	}
#else
	static inline u64 nvmeib_pet_get_trace_time_ns(void)
	{
		u64 ticks = nvmeib_public_rdtsc() + tsc_offset;
		return MUL_X_DIV_Y(ticks, 1000000ULL, (u64)tsc_khz);
	}
#endif

enum nvmeib_pet_severity{
	NVMEIB_PET_SEVERITY_NORMAL   = 0, //periodic dump to see what is going on
	NVMEIB_PET_SEVERITY_WARNING  = 1, //the operation experienced some delay or probably visited resubmitted
	NVMEIB_PET_SEVERITY_ERROR    = 2, //the operation received a network/disk error
	NVMEIB_PET_SEVERITY_CRITICAL = 3, //DI problem was found, something unexpected
};

static inline enum nvmeib_pet_severity nvmeib_pet_severity_get_worst(enum nvmeib_pet_severity s1, enum nvmeib_pet_severity s2)
{
	return ((unsigned)s1 < (unsigned)s2) ? s2 : s1;
}

static inline bool nvmeib_pet_severity_is_same_or_worse(unsigned base, enum nvmeib_pet_severity other)
{
		return base <= (unsigned)other;
}

#define NVMEIB_PET_NO_RELEASE_CPU ((s16) - 1)

struct nvmeib_pet_buffer {
	struct iovec data;
	s16 release_cpu;
};

struct nvmeib_pet_base_controller{
	//flush should be callable from the "interrupt context"
	void (*flush)(struct nvmeib_pet_base_controller const* self, enum nvmeib_pet_severity severity, struct iovec const data);
	struct nvmeib_pet_buffer (*get_buffer)(struct nvmeib_pet_base_controller const *self);
	void (*put_buffer)(struct nvmeib_pet_base_controller const *self, struct nvmeib_pet_buffer buffer);
};

//}}}

//{{{pet storage - implementation details

struct __attribute__((packed)) nvmeib_pet_stream_header {
	u64 commit_id;
	u16 journal_size;
};

enum { NVMEIB_PET_ENTITY_HEADER_SIZE = sizeof(struct nvmeib_pet_stream_header) };

struct nvmeib_pet_stream{
	struct iovec data;
	/* Highest byte written in data; viewer scans [0, max_written_bytes). */
	u16 max_written_bytes;
	/* Next physical byte to allocate. Before rotation it tracks
	 * max_written_bytes; after rotation it may point inside the suffix.
	 */
	u16 write_offset;
	/* Bytes in [0, protected_prefix) are retained by rotation. The area starts
	 * with the entity header and may be extended by explicit user request.
	 */
	u16 protected_prefix;
};

enum {NVMEIB_PET_MAX_STREAM_SIZE=64*1024}; //because max_written_bytes is u16
enum {NVMEIB_PET_MAX_MSG_ARGS_N_BYTES=96}; //maximal 12 arguments, 8 bytes each => 96 bytes

/* PET stores compact scalar values only. Pointers are allowed as addresses,
 * but string payloads and floating point values are intentionally unsupported.
 */
#define __NVMEIB_PET_ARG_TYPE_IS(value, type) __builtin_types_compatible_p(typeof(value), type)
#define __NVMEIB_PET_VALIDATE_ARG_TYPE(value) \
do { \
	BUILD_BUG_ON_MSG(sizeof(void*) != 8, "only 64bit platforms are supported"); \
	BUILD_BUG_ON_MSG(__NVMEIB_PET_ARG_TYPE_IS(value, float), "float type is not supported"); \
	BUILD_BUG_ON_MSG(__NVMEIB_PET_ARG_TYPE_IS(value, double), "double type is not supported"); \
	BUILD_BUG_ON_MSG(__NVMEIB_PET_ARG_TYPE_IS(value, long double), "long double type is not supported"); \
	BUILD_BUG_ON_MSG(__NVMEIB_PET_ARG_TYPE_IS(value, char*), "char* type is not supported"); \
	BUILD_BUG_ON_MSG(__NVMEIB_PET_ARG_TYPE_IS(value, char const*), "char const* type is not supported"); \
	BUILD_BUG_ON_MSG(__NVMEIB_PET_ARG_TYPE_IS(value, char[sizeof(value)]), "char[] type is not supported"); \
	BUILD_BUG_ON_MSG(__NVMEIB_PET_ARG_TYPE_IS(value, char const[sizeof(value)]), "char const[] type is not supported"); \
	(void)sizeof((u64)(value)); \
} while (0)

static inline struct nvmeib_pet_stream nvmeib_pet_stream_make(struct iovec data)
{
	struct nvmeib_pet_stream stream = {
		.data = data,
		.max_written_bytes = data.iov_base ? NVMEIB_PET_ENTITY_HEADER_SIZE : 0,
		.write_offset = data.iov_base ? NVMEIB_PET_ENTITY_HEADER_SIZE : 0,
		.protected_prefix = data.iov_base ? NVMEIB_PET_ENTITY_HEADER_SIZE : 0,
	};

	if (unlikely(NVMEIB_PET_MAX_STREAM_SIZE < data.iov_len)){
		stream.data.iov_len = NVMEIB_PET_MAX_STREAM_SIZE; //avoid undefined behavior
	}

	return stream;
}

__attribute__((nonnull (1)))
static inline void nvmeib_pet_stream_commit(struct nvmeib_pet_stream* self)
{
	if (self->data.iov_base) {
		struct nvmeib_pet_stream_header const header = {
			.commit_id = (u64)COMMIT_ID,
			.journal_size = self->max_written_bytes,
		};

		memcpy(self->data.iov_base, &header, sizeof(header));
		self->data.iov_len = self->max_written_bytes;
	}
}

struct __attribute__((packed)) nvmeib_pet_msg_header {
	u16 section_offset;
	union {
		/* section_offset != 0 - stored raw section offset + 1 */
		struct __attribute__((packed)) {
			u64 timestamp;
			u8 args_n_bytes;
		} msg;
		/* section_offset == 0 - rotation spacer */
		struct __attribute__((packed)) {
			/* payload bytes after this header */
			u64 bytes;
			u8 unused;
		} spacer;
	};
};

enum {
	NVMEIB_PET_MIN_MSG_ARGS_N_BYTES = 1,
	NVMEIB_PET_MIN_MSG_N_BYTES = sizeof(struct nvmeib_pet_msg_header) + NVMEIB_PET_MIN_MSG_ARGS_N_BYTES,
	NVMEIB_PET_MAX_MSG_N_BYTES = sizeof(struct nvmeib_pet_msg_header) + NVMEIB_PET_MAX_MSG_ARGS_N_BYTES,
	NVMEIB_PET_MIN_ROTATABLE_N_BYTES = 2 * NVMEIB_PET_MAX_MSG_N_BYTES,
	NVMEIB_PET_MIN_JOURNAL_N_BYTES = NVMEIB_PET_ENTITY_HEADER_SIZE + NVMEIB_PET_MAX_MSG_N_BYTES,
};

u16 __nvmeib_pet_stream_calculate_consumable_n_bytes(struct nvmeib_pet_stream const* self, u16 physical_offset, u16 eof_offset);
u8* __nvmeib_pet_stream_allocate_rotate(struct nvmeib_pet_stream* self, u16 size);

__attribute__((nonnull (1)))
static inline u8* nvmeib_pet_stream_alloc(struct nvmeib_pet_stream* self, u16 size)
{
	u8* msg = NULL;
	u16 const write_offset = self->write_offset;
	size_t const write_end = (size_t)write_offset + size;

	BUG_ON(size < NVMEIB_PET_MIN_MSG_N_BYTES);
	BUG_ON(size > NVMEIB_PET_MAX_MSG_N_BYTES);

	BUG_ON(!self->data.iov_base);
	BUG_ON(self->write_offset > self->max_written_bytes);
	BUG_ON(self->write_offset > self->data.iov_len);
	BUG_ON(self->max_written_bytes > self->data.iov_len);

	/* Fast append: the new message reaches EOF, so no spacer is needed. */
	if (likely(write_end >= self->max_written_bytes &&
		   write_end <= self->data.iov_len)) {
		self->write_offset = (u16)write_end;
		self->max_written_bytes = self->write_offset;
		return (u8*)self->data.iov_base + write_offset;
	}

	msg = __nvmeib_pet_stream_allocate_rotate(self, size);
	if (!msg) {
		return NULL;
	}

	return msg;
}

__attribute__((nonnull (1)))
static inline void nvmeib_pet_stream_protect_prefix(struct nvmeib_pet_stream* self)
{
	if (!self->data.iov_base) {
		return;
	}

	BUG_ON(self->protected_prefix > self->max_written_bytes);
	BUG_ON(self->max_written_bytes > self->data.iov_len);
	self->protected_prefix = self->max_written_bytes;
	BUG_ON(((size_t)self->data.iov_len - self->protected_prefix) < NVMEIB_PET_MIN_ROTATABLE_N_BYTES);
	self->write_offset = self->protected_prefix;
}

static inline struct nvmeib_pet_msg_header nvmeib_pet_msg_header_make(u16 raw_section_offset, u8 args_n_bytes)
{
	return (struct nvmeib_pet_msg_header){
		.section_offset = raw_section_offset+1, /* section_offset==0 would be a special offset */
		.msg = {
			.timestamp = nvmeib_pet_get_trace_time_ns(),
			.args_n_bytes = args_n_bytes,
		},
	};
}

//this is the interface to create message from any supported types
#define __NVMEIB_PET_STREAM_WRITE1(self, offset, exp1) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE2(self, offset, exp1, exp2) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE3(self, offset, exp1, exp2, exp3) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE4(self, offset, exp1, exp2, exp3, exp4) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE5(self, offset, exp1, exp2, exp3, exp4, exp5) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE6(self, offset, exp1, exp2, exp3, exp4, exp5, exp6) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
			.arg6 = (exp6), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE7(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
			.arg6 = (exp6), \
			.arg7 = (exp7), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE8(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
			typeof(exp8) arg8; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
			.arg6 = (exp6), \
			.arg7 = (exp7), \
			.arg8 = (exp8), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE9(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
			typeof(exp8) arg8; \
			typeof(exp9) arg9; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
			.arg6 = (exp6), \
			.arg7 = (exp7), \
			.arg8 = (exp8), \
			.arg9 = (exp9), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE10(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9) + sizeof(exp10); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
			typeof(exp8) arg8; \
			typeof(exp9) arg9; \
			typeof(exp10) arg10; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
			.arg6 = (exp6), \
			.arg7 = (exp7), \
			.arg8 = (exp8), \
			.arg9 = (exp9), \
			.arg10 = (exp10), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE11(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9) + sizeof(exp10) + sizeof(exp11); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
			typeof(exp8) arg8; \
			typeof(exp9) arg9; \
			typeof(exp10) arg10; \
			typeof(exp11) arg11; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
			.arg6 = (exp6), \
			.arg7 = (exp7), \
			.arg8 = (exp8), \
			.arg9 = (exp9), \
			.arg10 = (exp10), \
			.arg11 = (exp11), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_STREAM_WRITE12(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11, exp12) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9) + sizeof(exp10) + sizeof(exp11) + sizeof(exp12); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_msg_header) + n_bytes; \
	u8* dest = nvmeib_pet_stream_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_msg_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
			typeof(exp8) arg8; \
			typeof(exp9) arg9; \
			typeof(exp10) arg10; \
			typeof(exp11) arg11; \
			typeof(exp12) arg12; \
		} __tmp = { \
			.header = nvmeib_pet_msg_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
			.arg3 = (exp3), \
			.arg4 = (exp4), \
			.arg5 = (exp5), \
			.arg6 = (exp6), \
			.arg7 = (exp7), \
			.arg8 = (exp8), \
			.arg9 = (exp9), \
			.arg10 = (exp10), \
			.arg11 = (exp11), \
			.arg12 = (exp12), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

//counts the number of elements in __VA_ARGS__
#define NVMEIB_PET_VA_NARGS_IMPL(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,N,...) N
#define NVMEIB_PET_VA_NARGS(...) NVMEIB_PET_VA_NARGS_IMPL(__VA_ARGS__,12,11,10,9,8,7,6,5,4,3,2,1)

#define __NVMEIB_PET_VALIDATE_ARGS1(exp1) \
do { \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp1); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS2(exp1, exp2) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS1(exp1); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp2); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS3(exp1, exp2, exp3) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS2(exp1, exp2); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp3); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS4(exp1, exp2, exp3, exp4) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS3(exp1, exp2, exp3); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp4); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS5(exp1, exp2, exp3, exp4, exp5) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS4(exp1, exp2, exp3, exp4); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp5); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS6(exp1, exp2, exp3, exp4, exp5, exp6) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS5(exp1, exp2, exp3, exp4, exp5); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp6); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS7(exp1, exp2, exp3, exp4, exp5, exp6, exp7) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS6(exp1, exp2, exp3, exp4, exp5, exp6); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp7); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS8(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS7(exp1, exp2, exp3, exp4, exp5, exp6, exp7); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp8); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS9(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS8(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp9); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS10(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS9(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp10); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS11(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS10(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp11); \
} while (0)
#define __NVMEIB_PET_VALIDATE_ARGS12(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11, exp12) \
do { \
	__NVMEIB_PET_VALIDATE_ARGS11(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11); \
	__NVMEIB_PET_VALIDATE_ARG_TYPE(exp12); \
} while (0)

#define __NVMEIB_PET_VALIDATE_ARGS_IMPL_IMPL(n_args, ...) __NVMEIB_PET_VALIDATE_ARGS##n_args(__VA_ARGS__)
#define __NVMEIB_PET_VALIDATE_ARGS_IMPL(n_args, ...) __NVMEIB_PET_VALIDATE_ARGS_IMPL_IMPL(n_args, __VA_ARGS__)
#define __NVMEIB_PET_VALIDATE_MSG_ARGS(...) __NVMEIB_PET_VALIDATE_ARGS_IMPL(NVMEIB_PET_VA_NARGS(__VA_ARGS__), __VA_ARGS__)

//if we know the number of arguments we can decide ourself what macro should be used
//there is a need for double indirecion in order to convert number of arguments to actual number
#define __NVMEIB_PET_STREAM_WRITE_IMPL_IMPL(self, offset, n_args, ...) __NVMEIB_PET_STREAM_WRITE##n_args(self, offset, __VA_ARGS__)
#define __NVMEIB_PET_STREAM_WRITE_IMPL(self, offset, n_args, ...) __NVMEIB_PET_STREAM_WRITE_IMPL_IMPL(self, offset, n_args, __VA_ARGS__)
#define __NVMEIB_PET_STREAM_WRITE_MSG(self, offset, ...) \
({ \
	__NVMEIB_PET_VALIDATE_MSG_ARGS(__VA_ARGS__); \
	__NVMEIB_PET_STREAM_WRITE_IMPL(self, offset, NVMEIB_PET_VA_NARGS(__VA_ARGS__), __VA_ARGS__); \
})


//severity & verbosity
//the main difference between traditional logging systems and PET is the following:
//* PET must accumulate the whole history and the history will be stored only in case it saw some "problematic" record.
//  The problematic record is identified by the severity. The severity of the all records is the worst one.
//Now, "verbose", on the other side defines how much information we collect through the process.
//For example, we may decide to print first 8 bytes and edic for every read/write block. Obviously will hurt the performance.

struct nvmeib_pet_journal{
	struct nvmeib_pet_base_controller const* controller;
	struct nvmeib_pet_stream stream;
	enum nvmeib_pet_severity worst_severity;
	bool verbose;
	u8 concurrent_access_detector; //don't bother to remove it in the production build - we have padding here;
	s16 release_cpu;
};

static inline struct nvmeib_pet_journal nvmeib_pet_journal_make(struct nvmeib_pet_base_controller const* controller, bool verbose)
{
	struct nvmeib_pet_buffer const buffer = controller ? controller->get_buffer(controller) :
							     (struct nvmeib_pet_buffer){
								     .data = { 0 },
								     .release_cpu = NVMEIB_PET_NO_RELEASE_CPU,
							     };

	if (buffer.data.iov_base) {
		BUG_ON(buffer.data.iov_len <= NVMEIB_PET_MIN_JOURNAL_N_BYTES);
	}

	return (struct nvmeib_pet_journal){
		.controller = controller,
		.stream = nvmeib_pet_stream_make(buffer.data),
		.worst_severity = NVMEIB_PET_SEVERITY_NORMAL,
		.verbose = verbose,
		.concurrent_access_detector = 0,
		.release_cpu = buffer.release_cpu,
	};
}

__attribute__((nonnull (1)))
static inline bool nvmeib_pet_journal_is_activated(struct nvmeib_pet_journal const* self)
{
	return self->stream.data.iov_base;
}

__attribute__((nonnull (1)))
static inline bool nvmeib_pet_journal_is_verbose(struct nvmeib_pet_journal const* self)
{
	return self->verbose;
}

static inline bool __nvmeib_pet_journal_test_and_set_in_use(struct nvmeib_pet_journal* self)
{
	#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR==1
		return __atomic_test_and_set(&(self->concurrent_access_detector), __ATOMIC_ACQUIRE);
	#else
		(void)self;
		return false;
	#endif
}

static inline void __nvmeib_pet_journal_clear_in_use(struct nvmeib_pet_journal* self)
{
	#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR==1
		__atomic_clear(&(self->concurrent_access_detector), __ATOMIC_RELEASE);
	#else
		(void)self;
	#endif
}

__attribute__((nonnull (1)))
__attribute__((format (printf, 1, 2)))
static inline void nvmeib_pet_journal_add_msg_verify_format(char const * const fmt, ...)
{
	(void)fmt;
}

/* Mark all currently written journal bytes as protected prefix. Call this after
 * writing stable entity context; later rotation may overwrite only bytes after
 * stream.protected_prefix. Inactive journals are ignored.
 */
__attribute__((nonnull (1)))
static inline void nvmeib_pet_journal_protect_prefix(struct nvmeib_pet_journal* self)
{
	bool is_in_use = false;

	if (!nvmeib_pet_journal_is_activated(self)) {
		return;
	}

	is_in_use = __nvmeib_pet_journal_test_and_set_in_use(self);
	BUG_ON(is_in_use);
	nvmeib_pet_stream_protect_prefix(&self->stream);
	__nvmeib_pet_journal_clear_in_use(self);
}

//don't add nvmeib_pet_journal_is_activated check here - too late - the arguments are already evaluated
#define nvmeib_pet_journal_add_msg(self, severity, offset, ...)	\
({	\
	u16 msg_written_bytes = 0;	\
	__auto_type __nvmeib_pet_journal = (self); \
	struct nvmeib_pet_stream* __nvmeib_pet_stream = &(__nvmeib_pet_journal->stream); \
	bool const is_in_use = __nvmeib_pet_journal_test_and_set_in_use(__nvmeib_pet_journal); \
	BUG_ON(is_in_use);	\
	msg_written_bytes = __NVMEIB_PET_STREAM_WRITE_MSG(__nvmeib_pet_stream, offset, __VA_ARGS__); \
	if (msg_written_bytes){ \
		__nvmeib_pet_journal->worst_severity = nvmeib_pet_severity_get_worst(__nvmeib_pet_journal->worst_severity, severity); \
	} \
	__nvmeib_pet_journal_clear_in_use(__nvmeib_pet_journal); \
	msg_written_bytes; \
})


__attribute__((nonnull (1)))
static inline void nvmeib_pet_journal_commit(struct nvmeib_pet_journal* self)
{
	if (likely(nvmeib_pet_journal_is_activated(self))){
		if (self->stream.max_written_bytes > NVMEIB_PET_ENTITY_HEADER_SIZE) {
			nvmeib_pet_stream_commit(&self->stream);
			self->controller->flush(self->controller, self->worst_severity, self->stream.data);
		}

		self->controller->put_buffer(self->controller, (struct nvmeib_pet_buffer){
								       .data = self->stream.data,
								       .release_cpu = self->release_cpu,
							       });
	}

	(*self) = (struct nvmeib_pet_journal){0};
}
//}}}

#endif//__NVMEIB_PET_SPECIFICATION_H__
