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
#include "compat/kr_incs_compiler_types.h"
#include "compat/kr_incs_time_rdtsc.h"
#include "compat/kr_incs_time_jiff.h"
#include "common/nvmeib_math.h"
#include "common/pet/nvmeib_pet_types.h"

//{{{ OS integration

struct __attribute__((packed)) nvmeib_pet_trace_clock {
	u64 tsc_offset;
	u32 tsc_khz;
};

#if defined(__KERNEL__)
	#include "nvmeib_trace.h"

	static inline struct nvmeib_pet_trace_clock nvmeib_pet_trace_clock_get(void)
	{
		return (struct nvmeib_pet_trace_clock){
			.tsc_offset = nvmeib_trace_tsc_offset_ticks,
			.tsc_khz = nvmeib_public_tsc_khz(),
		};
	}
#else
	static inline struct nvmeib_pet_trace_clock nvmeib_pet_trace_clock_get(void)
	{
		return (struct nvmeib_pet_trace_clock){
			.tsc_offset = (u64)tsc_offset,
			.tsc_khz = tsc_khz,
		};
	}
#endif

static inline u64 nvmeib_pet_get_trace_time_ticks(void)
{
	return (u64)nvmeib_public_rdtsc();
}

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
	/* PET may return the allocated iovec with a different iov_len; release
	 * logic must ignore the size and use only the allocation identity.
	 */
	void (*put_buffer)(struct nvmeib_pet_base_controller const *self, struct nvmeib_pet_buffer buffer);
};

//}}}

//{{{pet storage - implementation details

struct __attribute__((packed)) nvmeib_pet_journalbuf_header {
	u64 commit_id;
	u16 journalbuf_size;
	struct nvmeib_pet_trace_clock trace_clock;
};

enum { NVMEIB_PET_JOURNALBUF_HEADER_SIZE = sizeof(struct nvmeib_pet_journalbuf_header) };

struct nvmeib_pet_journalbuf{
	struct iovec data;
	/* Highest byte written in data; viewer scans [0, max_written_bytes). */
	u16 max_written_bytes;
	/* Next physical byte to allocate. Before rotation it tracks
	 * max_written_bytes; after rotation it may point inside the suffix.
	 */
	u16 write_offset;
	/* Bytes in [0, protected_prefix) are retained by rotation. The area starts
	 * with the journal buffer header and may be extended by explicit user request.
	 */
	u16 protected_prefix;
};

enum {NVMEIB_PET_MAX_JOURNALBUF_SIZE=64*1024}; //because max_written_bytes is u16

static inline struct nvmeib_pet_journalbuf nvmeib_pet_journalbuf_make(struct iovec data)
{
	bool const is_active = data.iov_len;
	struct nvmeib_pet_journalbuf journalbuf = {
		.data = data,
		.max_written_bytes = is_active ? NVMEIB_PET_JOURNALBUF_HEADER_SIZE : 0,
		.write_offset = is_active ? NVMEIB_PET_JOURNALBUF_HEADER_SIZE : 0,
		.protected_prefix = is_active ? NVMEIB_PET_JOURNALBUF_HEADER_SIZE : 0,
	};

	return journalbuf;
}

__attribute__((nonnull (1)))
static inline bool nvmeib_pet_journalbuf_is_active(struct nvmeib_pet_journalbuf const* self)
{
	return self->data.iov_len;
}

__attribute__((nonnull (1)))
static inline void nvmeib_pet_journalbuf_deactive(struct nvmeib_pet_journalbuf * self)
{
	self->data.iov_len = 0;
}

__attribute__((nonnull (1)))
static inline void nvmeib_pet_journalbuf_commit(struct nvmeib_pet_journalbuf* self)
{
	if (likely(nvmeib_pet_journalbuf_is_active(self))) {
		struct nvmeib_pet_journalbuf_header const header = {
			.commit_id = (u64)COMMIT_ID,
			.journalbuf_size = self->max_written_bytes,
			.trace_clock = nvmeib_pet_trace_clock_get(),
		};

		memcpy(self->data.iov_base, &header, sizeof(header));
		self->data.iov_len = self->max_written_bytes;
	}
}

struct __attribute__((packed)) nvmeib_pet_journalbuf_message_header {
	u16 message_id;
	union {
		/* message_id != 0 - stored message record index + 1 */
		struct __attribute__((packed)) {
			u64 timestamp;
			u8 args_n_bytes;
		} msg;
		/* message_id == 0 - rotation spacer */
		struct __attribute__((packed)) {
			u64 unused;
			/* Same offset as msg.args_n_bytes; the spacer payload is bounded by
			 * one record overshoot during rotation, so it is always smaller than
			 * the maximal message record.
			 */
			u8 args_n_bytes;
		} spacer;
	};
};

enum {
	NVMEIB_PET_MAX_MSG_ARGS = 12,
	NVMEIB_PET_MIN_MSG_ARGS_N_BYTES = 1,
	NVMEIB_PET_MAX_MSG_ARGS_N_BYTES = NVMEIB_PET_MAX_MSG_ARGS * sizeof(u64),
	NVMEIB_PET_MIN_MSG_N_BYTES = sizeof(struct nvmeib_pet_journalbuf_message_header) + NVMEIB_PET_MIN_MSG_ARGS_N_BYTES,
	NVMEIB_PET_MAX_MSG_N_BYTES = sizeof(struct nvmeib_pet_journalbuf_message_header) + NVMEIB_PET_MAX_MSG_ARGS_N_BYTES,
	NVMEIB_PET_MIN_ROTATABLE_N_BYTES = 2 * NVMEIB_PET_MAX_MSG_N_BYTES,
	NVMEIB_PET_MIN_JOURNAL_N_BYTES = NVMEIB_PET_JOURNALBUF_HEADER_SIZE + NVMEIB_PET_MAX_MSG_N_BYTES,
};

/* Slow rotation allocator. Returns NULL if journalbuf state is inconsistent and
 * the next record cannot be placed safely.
 */
u8* __nvmeib_pet_journalbuf_allocate_rotate(struct nvmeib_pet_journalbuf* self, u16 size);

__attribute__((nonnull (1)))
static inline u8* nvmeib_pet_journalbuf_alloc(struct nvmeib_pet_journalbuf* self, u16 size)
{
	u8* msg = NULL;
	u16 const write_offset = self->write_offset;
	size_t const write_end = (size_t)write_offset + size;

	if (unlikely(!nvmeib_pet_journalbuf_is_active(self))) {
		return NULL;
	}

	//cannot fire - defensive local invariants
	BUG_ON(size < NVMEIB_PET_MIN_MSG_N_BYTES);
	BUG_ON(size > NVMEIB_PET_MAX_MSG_N_BYTES);

	if(unlikely((self->write_offset > self->max_written_bytes)
				|| (self->write_offset > self->data.iov_len)
				|| (self->max_written_bytes > self->data.iov_len))){
		nvmeib_pet_journalbuf_deactive(self);
		return NULL;
	}

	/* Fast append: the new message reaches EOF, so no spacer is needed. */
	if (likely(write_end >= self->max_written_bytes &&
		   write_end <= self->data.iov_len)) {
		self->write_offset = (u16)write_end;
		self->max_written_bytes = self->write_offset;
		return (u8*)self->data.iov_base + write_offset;
	}

	msg = __nvmeib_pet_journalbuf_allocate_rotate(self, size);
	if (!msg) {
		nvmeib_pet_journalbuf_deactive(self);
	}

	return msg;
}

__attribute__((nonnull (1)))
static inline void nvmeib_pet_journalbuf_protect_prefix(struct nvmeib_pet_journalbuf* self)
{
	if (unlikely(!nvmeib_pet_journalbuf_is_active(self))) {
		return;
	}

	if(unlikely((self->protected_prefix > self->max_written_bytes)
				|| (self->max_written_bytes > self->data.iov_len)
				|| (((size_t)self->data.iov_len - self->max_written_bytes) < NVMEIB_PET_MIN_ROTATABLE_N_BYTES))){
		nvmeib_pet_journalbuf_deactive(self);
		return;
	};

	self->protected_prefix = self->max_written_bytes;
	self->write_offset = self->protected_prefix;
}

static inline struct nvmeib_pet_journalbuf_message_header nvmeib_pet_journalbuf_message_header_make(u16 message_index, u8 args_n_bytes)
{
	return (struct nvmeib_pet_journalbuf_message_header){
		.message_id = message_index + 1, /* message_id==0 is reserved for spacers */
		.msg = {
			.timestamp = nvmeib_pet_get_trace_time_ticks(),
			.args_n_bytes = args_n_bytes,
		},
	};
}

//this is the interface to create message from any supported types
#define __NVMEIB_PET_JOURNALBUF_WRITE1(self, offset, exp1) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_JOURNALBUF_WRITE2(self, offset, exp1, exp2) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
			.arg1 = (exp1), \
			.arg2 = (exp2), \
		}; \
		\
		memcpy(dest, &__tmp, sizeof(__tmp)); \
		written = sizeof(__tmp); \
	} \
	written; \
})

#define __NVMEIB_PET_JOURNALBUF_WRITE3(self, offset, exp1, exp2, exp3) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE4(self, offset, exp1, exp2, exp3, exp4) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE5(self, offset, exp1, exp2, exp3, exp4, exp5) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE6(self, offset, exp1, exp2, exp3, exp4, exp5, exp6) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE7(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE8(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
			typeof(exp1) arg1; \
			typeof(exp2) arg2; \
			typeof(exp3) arg3; \
			typeof(exp4) arg4; \
			typeof(exp5) arg5; \
			typeof(exp6) arg6; \
			typeof(exp7) arg7; \
			typeof(exp8) arg8; \
		} __tmp = { \
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE9(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
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
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE10(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9) + sizeof(exp10); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
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
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE11(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9) + sizeof(exp10) + sizeof(exp11); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
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
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

#define __NVMEIB_PET_JOURNALBUF_WRITE12(self, offset, exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11, exp12) \
({ \
	size_t written = 0; \
	size_t const n_bytes = sizeof(exp1) + sizeof(exp2) + sizeof(exp3) + sizeof(exp4) + sizeof(exp5) + sizeof(exp6) + sizeof(exp7) + sizeof(exp8) + sizeof(exp9) + sizeof(exp10) + sizeof(exp11) + sizeof(exp12); \
	size_t const msg_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header) + n_bytes; \
	u8* dest = nvmeib_pet_journalbuf_alloc(self, msg_n_bytes); \
	\
	BUG_ON(n_bytes > NVMEIB_PET_MAX_MSG_ARGS_N_BYTES); \
	\
	if (dest) { \
		struct __attribute__((packed)) { \
			struct nvmeib_pet_journalbuf_message_header header; \
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
			.header = nvmeib_pet_journalbuf_message_header_make(offset, n_bytes), \
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

//if we know the number of arguments we can decide ourself what macro should be used
//there is a need for double indirecion in order to convert number of arguments to actual number
#define __NVMEIB_PET_JOURNALBUF_WRITE_IMPL_IMPL(self, offset, n_args, ...) __NVMEIB_PET_JOURNALBUF_WRITE##n_args(self, offset, __VA_ARGS__)
#define __NVMEIB_PET_JOURNALBUF_WRITE_IMPL(self, offset, n_args, ...) __NVMEIB_PET_JOURNALBUF_WRITE_IMPL_IMPL(self, offset, n_args, __VA_ARGS__)
#define __NVMEIB_PET_JOURNALBUF_WRITE_MSG(self, offset, ...) \
({ \
	__NVMEIB_PET_VALIDATE_MSG_ARGS(__VA_ARGS__); \
	__NVMEIB_PET_JOURNALBUF_WRITE_IMPL(self, offset, NVMEIB_PET_VA_NARGS(__VA_ARGS__), __VA_ARGS__); \
})

//severity & verbosity
//the main difference between traditional logging systems and PET is the following:
//* PET must accumulate the whole history and the history will be stored only in case it saw some "problematic" record.
//  The problematic record is identified by the severity. The severity of the all records is the worst one.
//Now, "verbose", on the other side defines how much information we collect through the process.
//For example, we may decide to print first 8 bytes and edic for every read/write block. Obviously will hurt the performance.

struct nvmeib_pet_journal{
	struct nvmeib_pet_base_controller const* controller;
	struct nvmeib_pet_journalbuf journalbuf;
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
		BUG_ON(!buffer.data.iov_base);
		BUG_ON(buffer.data.iov_len < NVMEIB_PET_MIN_JOURNAL_N_BYTES);
		BUG_ON(buffer.data.iov_len > NVMEIB_PET_MAX_JOURNALBUF_SIZE);
	}

	return (struct nvmeib_pet_journal){
		.controller = controller,
		.journalbuf = nvmeib_pet_journalbuf_make(buffer.data),
		.worst_severity = NVMEIB_PET_SEVERITY_NORMAL,
		.verbose = verbose,
		.concurrent_access_detector = 0,
		.release_cpu = buffer.release_cpu,
	};
}

__attribute__((nonnull (1)))
static inline bool nvmeib_pet_journal_is_activated(struct nvmeib_pet_journal const* self)
{
	return nvmeib_pet_journalbuf_is_active(&self->journalbuf);
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
 * writing stable journal context; later rotation may overwrite only bytes after
 * journalbuf.protected_prefix. Inactive journals are ignored.
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
	nvmeib_pet_journalbuf_protect_prefix(&self->journalbuf);
	__nvmeib_pet_journal_clear_in_use(self);
}

__attribute__((nonnull (1)))
static inline void nvmeib_pet_journal_commit(struct nvmeib_pet_journal* self)
{
	if (likely(nvmeib_pet_journal_is_activated(self))){
		if (self->journalbuf.max_written_bytes > NVMEIB_PET_JOURNALBUF_HEADER_SIZE) {
			nvmeib_pet_journalbuf_commit(&self->journalbuf);
			self->controller->flush(self->controller, self->worst_severity, self->journalbuf.data);
		}
	}

	if (self->journalbuf.data.iov_base) {
		self->controller->put_buffer(self->controller, (struct nvmeib_pet_buffer){
								       .data = self->journalbuf.data,
								       .release_cpu = self->release_cpu,
							       });
	}

	(*self) = (struct nvmeib_pet_journal){0};
}

//don't add nvmeib_pet_journal_is_activated check here - too late - the arguments are already evaluated
#define nvmeib_pet_journal_add_msg(self, severity, offset, ...)	\
({	\
	u16 msg_written_bytes = 0;	\
	__auto_type __nvmeib_pet_journal = (self); \
	struct nvmeib_pet_journalbuf* __nvmeib_pet_journalbuf = &(__nvmeib_pet_journal->journalbuf); \
	bool const is_in_use = __nvmeib_pet_journal_test_and_set_in_use(__nvmeib_pet_journal); \
	BUG_ON(is_in_use);	\
	msg_written_bytes = __NVMEIB_PET_JOURNALBUF_WRITE_MSG(__nvmeib_pet_journalbuf, offset, __VA_ARGS__); \
	if (msg_written_bytes){ \
		__nvmeib_pet_journal->worst_severity = nvmeib_pet_severity_get_worst(__nvmeib_pet_journal->worst_severity, severity); \
	} \
	__nvmeib_pet_journal_clear_in_use(__nvmeib_pet_journal); \
	msg_written_bytes; \
})

#define NVMEIB_PET_MESSAGE_SECTION "nvmeib_pet_messages"

enum {
	NVMEIB_PET_MESSAGE_N_BYTES_SHIFT = 9,
	NVMEIB_PET_MESSAGE_N_BYTES = 1 << NVMEIB_PET_MESSAGE_N_BYTES_SHIFT,
	NVMEIB_PET_MESSAGE_ARG_STRUCT_CODE_N_BYTES = NVMEIB_PET_MAX_MSG_ARGS + 1,
	NVMEIB_PET_MESSAGE_FORMAT_N_BYTES = NVMEIB_PET_MESSAGE_N_BYTES - NVMEIB_PET_MESSAGE_ARG_STRUCT_CODE_N_BYTES,
};

struct __attribute__((packed, aligned(NVMEIB_PET_MESSAGE_N_BYTES))) nvmeib_pet_message_description {
	char arg_struct_code[NVMEIB_PET_MAX_MSG_ARGS + 1];
	char format[NVMEIB_PET_MESSAGE_FORMAT_N_BYTES];
};

enum {
	NVMEIB_PET_MESSAGE_FIELDS_N_BYTES = sizeof_field(struct nvmeib_pet_message_description, arg_struct_code) +
					    sizeof_field(struct nvmeib_pet_message_description, format),
	NVMEIB_PET_MESSAGE_SIZE_CHECK = 1 / (sizeof(struct nvmeib_pet_message_description) == NVMEIB_PET_MESSAGE_FIELDS_N_BYTES),
	NVMEIB_PET_MESSAGE_EXPECTED_SIZE_CHECK = 1 / (sizeof(struct nvmeib_pet_message_description) == NVMEIB_PET_MESSAGE_N_BYTES),
	NVMEIB_PET_MESSAGE_NOT_EMPTY_CHECK = 1 / (sizeof(struct nvmeib_pet_message_description) != 0),
};

#define NVMEIB_IO_PET_MSG(pet_journal, msg, severity,...) \
({ \
	u16 __io_pet_msg_written = 0; \
	__auto_type __io_pet_journal_param = (pet_journal); \
	if (nvmeib_pet_journal_is_activated(__io_pet_journal_param)) { \
		static const struct nvmeib_pet_message_description NVMESH_USED NVMESH_ALIGNED(NVMEIB_PET_MESSAGE_N_BYTES) NVMESH_SECTION(NVMEIB_PET_MESSAGE_SECTION) __io_pet_message = { \
			.arg_struct_code = { __NVMEIB_PET_ARG_STRUCT_CODES(__VA_ARGS__) }, \
			.format = msg, \
		}; \
		u64 const __io_pet_message_offset = (u64)&__io_pet_message - (u64)__start_nvmeib_pet_messages; \
		u64 const __io_pet_message_index64 = __io_pet_message_offset >> NVMEIB_PET_MESSAGE_N_BYTES_SHIFT; \
		u16 const __io_pet_message_index = (u16)__io_pet_message_index64; \
		struct nvmeib_pet_journal* __io_pet_journal = (struct nvmeib_pet_journal*)__io_pet_journal_param; \
		BUG_ON(__io_pet_message_offset & (NVMEIB_PET_MESSAGE_N_BYTES - 1)); \
		BUG_ON(__io_pet_message_index64 >= 0xffff); \
		BUILD_BUG_ON_MSG(sizeof(msg) > NVMEIB_PET_MESSAGE_FORMAT_N_BYTES, "PET format string is too long"); \
		if (0) nvmeib_pet_journal_add_msg_verify_format(msg, __VA_ARGS__); \
		__io_pet_msg_written = nvmeib_pet_journal_add_msg(__io_pet_journal, severity, __io_pet_message_index, __VA_ARGS__); \
	} \
	__io_pet_msg_written; \
})

#define NVMEIB_IO_PET_MSG_NORM(pet_journal, msg, ...) NVMEIB_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_NORMAL, __VA_ARGS__)
#define NVMEIB_IO_PET_MSG_WARN(pet_journal, msg, ...) NVMEIB_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_WARNING, __VA_ARGS__)
#define NVMEIB_IO_PET_MSG_ERROR(pet_journal, msg, ...) NVMEIB_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_ERROR, __VA_ARGS__)
#define NVMEIB_IO_PET_MSG_CRIT(pet_journal, msg, ...) NVMEIB_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_CRITICAL, __VA_ARGS__)


//}}}

#endif//__NVMEIB_PET_SPECIFICATION_H__
