#ifndef __NVMEIB_PET_SPECIFICATION_H__
#define __NVMEIB_PET_SPECIFICATION_H__

#if defined(__KERNEL__)
	#include <linux/string.h>
#else
	#include <string.h>
	#include <sys/uio.h>
#endif
#include "compat/kr_incs_types.h"
#include "compat/kr_incs_asserts.h"
#include "compat/kr_incs_time_rdtsc.h"
#include "compat/kr_incs_time_jiff.h"

//{{{ OS integration

static inline u64 nvmeib_pet_get_trace_time_ns(void)
{
	#ifdef __KERNEL__
		extern unsigned long nvmeib_trace_tsc_to_ns(unsigned long timestamp);
		return nvmeib_trace_tsc_to_ns(nvmeib_public_rdtsc());
	#else
		return nvmeib_public_rdtsc() + tsc_offset;
	#endif
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

struct nvmeib_pet_base_controller{
	//flush should be callable from the "interrupt context"
	void (*flush)(struct nvmeib_pet_base_controller const* self, enum nvmeib_pet_severity severity, struct iovec const data);
	struct iovec (*get_buffer)(struct nvmeib_pet_base_controller const* self);
	void (*put_buffer)(struct nvmeib_pet_base_controller const* self, struct iovec data);
};

//}}}

//{{{pet storage - implementation details
struct nvmeib_pet_stream{
	struct iovec data;
	u16 written_bytes;
	u16* written_msgs; //pointer to data.iov_base[0]
};

enum {NVMEIB_PET_MAX_STREAM_SIZE=64*1024}; //because written_bytes is u16

static inline struct nvmeib_pet_stream nvmeib_pet_stream_make(struct iovec data)
{
	struct nvmeib_pet_stream stream = {
		.data = data,
		.written_bytes = data.iov_base ? sizeof(*stream.written_msgs) : 0 , //sizeof(u16) - written_msgs
		.written_msgs = (u16*)data.iov_base //all C allocation should be aligned to "unsigned long long" or "long double"
	};

	if (stream.written_msgs){
		(*stream.written_msgs) = 0;
	}

	if (unlikely(NVMEIB_PET_MAX_STREAM_SIZE < data.iov_len)){
		stream.data.iov_len = NVMEIB_PET_MAX_STREAM_SIZE; //avoid undefined behavior
	}

	return stream;
}

__attribute__((nonnull (1)))
static inline void nvmeib_pet_stream_commit(struct nvmeib_pet_stream* self)
{
	if (self->data.iov_base) {
		self->data.iov_len = self->written_bytes;
	}
}

__attribute__((nonnull (1)))
static inline struct iovec nvmeib_pet_stream_alloc(struct nvmeib_pet_stream* self, u16 size)
{
	if (unlikely((size_t)self->written_bytes + (size_t)size > self->data.iov_len)){
		return (struct iovec){0}; //not enough memory
	} else {
		struct iovec const res = {.iov_base = (u8*)self->data.iov_base + self->written_bytes, .iov_len=size};
		self->written_bytes += size;
		(*self->written_msgs) += 1;
		return res;
	}
}

__attribute__((nonnull (1,3)))
static inline void nvmeib_pet_iovec_append_any(struct iovec* out, size_t size, u8 const* data)
{
	memcpy(out->iov_base, data, size);
	out->iov_base = (u8*)out->iov_base + size;
	out->iov_len -= size;
}

#define nvmeib_pet_iovec_append_type(out, value)                                                    \
({                                                                                                  \
	__auto_type __pet_value_append = value;                                                         \
	nvmeib_pet_iovec_append_any(out, sizeof(__pet_value_append), (u8 const*)(&__pet_value_append)); \
})

enum nvmeib_pet_store_type{
	NVMEIB_PET_STORE_TYPE_S_BYTE     = 0,
	NVMEIB_PET_STORE_TYPE_U_BYTE     = 1,
	NVMEIB_PET_STORE_TYPE_S_SHORT    = 2,
	NVMEIB_PET_STORE_TYPE_U_SHORT    = 3,
	NVMEIB_PET_STORE_TYPE_S_INT      = 4,
	NVMEIB_PET_STORE_TYPE_U_INT      = 5,
	NVMEIB_PET_STORE_TYPE_S_LONG_INT = 6,
	NVMEIB_PET_STORE_TYPE_U_LONG_INT = 7,
};

static inline u16 nvmeib_pet_store_type_get_size(enum nvmeib_pet_store_type type)
{
	static const u16 sizes[] = {
		[NVMEIB_PET_STORE_TYPE_S_BYTE] = sizeof(u8),
		[NVMEIB_PET_STORE_TYPE_U_BYTE] = sizeof(u8),
		[NVMEIB_PET_STORE_TYPE_S_SHORT] = sizeof(u16),
		[NVMEIB_PET_STORE_TYPE_U_SHORT] = sizeof(u16),
		[NVMEIB_PET_STORE_TYPE_S_INT] = sizeof(u32),
		[NVMEIB_PET_STORE_TYPE_U_INT] = sizeof(u32),
		[NVMEIB_PET_STORE_TYPE_S_LONG_INT] = sizeof(u64),
		[NVMEIB_PET_STORE_TYPE_U_LONG_INT] = sizeof(u64),
	};
	return sizes[type];
}

struct nvmeib_pet_variant{
	enum nvmeib_pet_store_type type;
	uint64_t value;
};

static inline enum nvmeib_pet_store_type 
__nvmeib_pet_optimize_store_type_if_zero(enum nvmeib_pet_store_type store_type, uint64_t value)
{
	if (value){
		return store_type;
	} 
	return NVMEIB_PET_STORE_TYPE_U_BYTE; 
	//probably we can optimize even futher, by adding special type, but this is too much work 
}

//64bit platform support only
#define nvmeib_pet_get_store_type(value)															\
({																									\
	enum nvmeib_pet_store_type const __store_type __attribute__((unused)) =							\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), bool),						\
		NVMEIB_PET_STORE_TYPE_U_BYTE,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint8_t),						\
		NVMEIB_PET_STORE_TYPE_U_BYTE,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int8_t),						\
		NVMEIB_PET_STORE_TYPE_S_BYTE,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), unsigned char),				\
		NVMEIB_PET_STORE_TYPE_U_BYTE,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), signed char),					\
		NVMEIB_PET_STORE_TYPE_S_BYTE,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), char),						\
    	NVMEIB_PET_STORE_TYPE_U_BYTE,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint16_t),					\
		NVMEIB_PET_STORE_TYPE_U_SHORT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int16_t),						\
		NVMEIB_PET_STORE_TYPE_S_SHORT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), unsigned short),				\
		NVMEIB_PET_STORE_TYPE_U_SHORT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), signed short),				\
		NVMEIB_PET_STORE_TYPE_S_SHORT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint32_t),					\
		NVMEIB_PET_STORE_TYPE_U_INT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int32_t),						\
		NVMEIB_PET_STORE_TYPE_S_INT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), unsigned int),				\
		NVMEIB_PET_STORE_TYPE_U_INT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int),							\
		NVMEIB_PET_STORE_TYPE_S_INT,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), long),						\
		(sizeof(long) == 4 ? NVMEIB_PET_STORE_TYPE_S_INT : NVMEIB_PET_STORE_TYPE_S_LONG_INT),		\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), unsigned long),				\
		(sizeof(long) == 4 ? NVMEIB_PET_STORE_TYPE_U_INT : NVMEIB_PET_STORE_TYPE_U_LONG_INT),		\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint64_t),					\
		NVMEIB_PET_STORE_TYPE_U_LONG_INT,															\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int64_t),						\
		NVMEIB_PET_STORE_TYPE_S_LONG_INT,															\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), unsigned long long),			\
		NVMEIB_PET_STORE_TYPE_U_LONG_INT,															\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), long long),					\
		NVMEIB_PET_STORE_TYPE_S_LONG_INT,															\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), size_t),						\
		NVMEIB_PET_STORE_TYPE_U_LONG_INT,															\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), ssize_t),						\
		NVMEIB_PET_STORE_TYPE_S_LONG_INT,															\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), void const*),					\
		NVMEIB_PET_STORE_TYPE_U_LONG_INT,															\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), typeof((void*)0)),			\
		NVMEIB_PET_STORE_TYPE_U_LONG_INT,															\
	NVMEIB_PET_STORE_TYPE_U_LONG_INT ))))))))))))))))))))))));															\
	BUILD_BUG_ON_MSG(sizeof(void*) != 8, "only 64bit platforms are supported"); 										\
	BUILD_BUG_ON_MSG(__builtin_types_compatible_p(typeof(value), float), "float type is not supported");				\
	BUILD_BUG_ON_MSG(__builtin_types_compatible_p(typeof(value), double), "double type is not supported"); 				\
	BUILD_BUG_ON_MSG(__builtin_types_compatible_p(typeof(value), char*), "char* type is not supported");				\
	BUILD_BUG_ON_MSG(__builtin_types_compatible_p(typeof(value), char const*), "char const* type is not supported");	\
	__nvmeib_pet_optimize_store_type_if_zero(__store_type, (uint64_t)value); 											\
})

#define nvmeib_pet_variant_make(arg)			\
({												\
	(struct nvmeib_pet_variant){				\
		.type = nvmeib_pet_get_store_type(arg),	\
		.value=(uint64_t)(arg)					\
	};											\
})


//offset == message id;
//	the message should be stored as clear text in some compiler & linker generated section
//	at runtime it is possible to calculate the message offset from the begining of the section;
//	that offset will be used as the message id
//	offset==0 represent end-of-sequence; since we cannot ensure it is not in use, I just +1 for every offset
//timestamp is stored in {type[0] & value[0]}

//for performance reasons I use struct of arrays instead of arrays of structs
//the delta is almost x2 both in memory usage (Linux stack frame is limited) and performance
//last note: the underlying enum type is int, but this is too much for pet store type
#define __NVMEIB_PET_MESSAGE_FIELDS(n_args) 			\
	u16 offset; 										\
	u8 /*enum nvmeib_pet_store_type*/ type[1 + n_args];	\
	u64 value[1 + n_args];								\

//obviously I can create a single macro, which generates the needed struct.
//obvioulsy  can use macro to implement the function,
//BUT using the macros all the way done make it very difficult to troublshoot the compiler error.

struct nvmeib_pet_message_1{
	__NVMEIB_PET_MESSAGE_FIELDS(1);
};

struct nvmeib_pet_message_2{
	__NVMEIB_PET_MESSAGE_FIELDS(2);
};

struct nvmeib_pet_message_3{
	__NVMEIB_PET_MESSAGE_FIELDS(3);
};

struct nvmeib_pet_message_4{
	__NVMEIB_PET_MESSAGE_FIELDS(4);
};

struct nvmeib_pet_message_5{
	__NVMEIB_PET_MESSAGE_FIELDS(5);
};

struct nvmeib_pet_message_6{
	__NVMEIB_PET_MESSAGE_FIELDS(6);
};

struct nvmeib_pet_message_7{
	__NVMEIB_PET_MESSAGE_FIELDS(7);
};

struct nvmeib_pet_message_8{
	__NVMEIB_PET_MESSAGE_FIELDS(8);
};

struct nvmeib_pet_message_9{
	__NVMEIB_PET_MESSAGE_FIELDS(9);
};

struct nvmeib_pet_message_10{
	__NVMEIB_PET_MESSAGE_FIELDS(10);
};

struct nvmeib_pet_message_11{
	__NVMEIB_PET_MESSAGE_FIELDS(11);
};

struct nvmeib_pet_message_12{
	__NVMEIB_PET_MESSAGE_FIELDS(12);
};


//this is the interface to create message from any supported types
#define NVMEIB_PET_MSG_1(offset_arg, arg1) 											\
({																					\
	(struct nvmeib_pet_message_1){													\
		.offset = offset_arg + 1,													\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1)}						\
	};																				\
})

#define NVMEIB_PET_MSG_2(offset_arg, arg1, arg2) \
({																														\
	(struct nvmeib_pet_message_2){																						\
		.offset = offset_arg + 1,																						\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2)},	\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2)}												\
	}; 																													\
})

#define NVMEIB_PET_MSG_3(offset_arg, arg1, arg2, arg3) \
({																					\
	(struct nvmeib_pet_message_3){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3)}							\
	};																				\
})

#define NVMEIB_PET_MSG_4(offset_arg, arg1, arg2, arg3, arg4) \
({																					\
	(struct nvmeib_pet_message_4){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4)}							\
	};																				\
})

#define NVMEIB_PET_MSG_5(offset_arg, arg1, arg2, arg3, arg4, arg5) \
({																					\
	(struct nvmeib_pet_message_5){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5)}							\
	};																				\
})

#define NVMEIB_PET_MSG_6(offset_arg, arg1, arg2, arg3, arg4, arg5, arg6) \
({																					\
	(struct nvmeib_pet_message_6){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5), nvmeib_pet_get_store_type(arg6)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5), (u64)(arg6)}							\
	};																				\
})

#define NVMEIB_PET_MSG_7(offset_arg, arg1, arg2, arg3, arg4, arg5, arg6, arg7) \
({																					\
	(struct nvmeib_pet_message_7){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5), nvmeib_pet_get_store_type(arg6), nvmeib_pet_get_store_type(arg7)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5), (u64)(arg6), (u64)(arg7)}							\
	};																				\
})

#define NVMEIB_PET_MSG_8(offset_arg, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8) \
({																					\
	(struct nvmeib_pet_message_8){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5), nvmeib_pet_get_store_type(arg6), nvmeib_pet_get_store_type(arg7), nvmeib_pet_get_store_type(arg8)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5), (u64)(arg6), (u64)(arg7), (u64)(arg8)}							\
	};																				\
})

#define NVMEIB_PET_MSG_9(offset_arg, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9) \
({																					\
	(struct nvmeib_pet_message_9){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5), nvmeib_pet_get_store_type(arg6), nvmeib_pet_get_store_type(arg7), nvmeib_pet_get_store_type(arg8), nvmeib_pet_get_store_type(arg9)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5), (u64)(arg6), (u64)(arg7), (u64)(arg8), (u64)(arg9)}							\
	};																				\
})

#define NVMEIB_PET_MSG_10(offset_arg, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10) \
({																					\
	(struct nvmeib_pet_message_10){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5), nvmeib_pet_get_store_type(arg6), nvmeib_pet_get_store_type(arg7), nvmeib_pet_get_store_type(arg8), nvmeib_pet_get_store_type(arg9), nvmeib_pet_get_store_type(arg10)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5), (u64)(arg6), (u64)(arg7), (u64)(arg8), (u64)(arg9), (u64)(arg10)}							\
	};																				\
})

#define NVMEIB_PET_MSG_11(offset_arg, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11) \
({																					\
	(struct nvmeib_pet_message_11){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5), nvmeib_pet_get_store_type(arg6), nvmeib_pet_get_store_type(arg7), nvmeib_pet_get_store_type(arg8), nvmeib_pet_get_store_type(arg9), nvmeib_pet_get_store_type(arg10), nvmeib_pet_get_store_type(arg11)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5), (u64)(arg6), (u64)(arg7), (u64)(arg8), (u64)(arg9), (u64)(arg10), (u64)(arg11)}							\
	};																				\
})

#define NVMEIB_PET_MSG_12(offset_arg, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12) \
({																					\
	(struct nvmeib_pet_message_12){													\
		.offset = offset_arg + 1,														\
		.type = {NVMEIB_PET_STORE_TYPE_U_LONG_INT, nvmeib_pet_get_store_type(arg1), nvmeib_pet_get_store_type(arg2), nvmeib_pet_get_store_type(arg3), nvmeib_pet_get_store_type(arg4), nvmeib_pet_get_store_type(arg5), nvmeib_pet_get_store_type(arg6), nvmeib_pet_get_store_type(arg7), nvmeib_pet_get_store_type(arg8), nvmeib_pet_get_store_type(arg9), nvmeib_pet_get_store_type(arg10), nvmeib_pet_get_store_type(arg11), nvmeib_pet_get_store_type(arg12)},\
		.value = {nvmeib_pet_get_trace_time_ns(), (u64)(arg1), (u64)(arg2), (u64)(arg3), (u64)(arg4), (u64)(arg5), (u64)(arg6), (u64)(arg7), (u64)(arg8), (u64)(arg9), (u64)(arg10), (u64)(arg11), (u64)(arg12)}							\
	};																				\
})


//counts the number of elements in __VA_ARGS__
#define NVMEIB_PET_VA_NARGS_IMPL(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,N,...) N
#define NVMEIB_PET_VA_NARGS(...) NVMEIB_PET_VA_NARGS_IMPL(__VA_ARGS__,12,11,10,9,8,7,6,5,4,3,2,1)

//if we know the number of arguments we can decide ourself what macro should be used
//there is a need for double indirecion in order to convert number of arguments to actual number
#define NVMEIB_PET_MSG_IMPL_IMPL(offset, n_args, ...) NVMEIB_PET_MSG_##n_args(offset, __VA_ARGS__)
#define NVMEIB_PET_MSG_IMPL(offset, n_args, ...) NVMEIB_PET_MSG_IMPL_IMPL(offset, n_args, __VA_ARGS__)
#define NVMEIB_PET_MSG(offset, ...) NVMEIB_PET_MSG_IMPL(offset, NVMEIB_PET_VA_NARGS(__VA_ARGS__), __VA_ARGS__)
//      ^^^^^^^^^^^^^^ this is the public user interface


#define __NVMEIB_PET_MESSAGE_GET_SIZE(self) 						\
({																	\
	u16 idx = 0;													\
	u16 total = sizeof(self->offset) + 1/*n_args*/;					\
	for(idx = 0; idx < ARRAY_SIZE(self->type); ++idx){				\
	    total += 1+nvmeib_pet_store_type_get_size(self->type[idx]);	\
	}																\
	total;															\
})

static inline u16 nvmeib_pet_message_1_get_size(struct nvmeib_pet_message_1 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_2_get_size(struct nvmeib_pet_message_2 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_3_get_size(struct nvmeib_pet_message_3 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_4_get_size(struct nvmeib_pet_message_4 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_5_get_size(struct nvmeib_pet_message_5 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_6_get_size(struct nvmeib_pet_message_6 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_7_get_size(struct nvmeib_pet_message_7 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_8_get_size(struct nvmeib_pet_message_8 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_9_get_size(struct nvmeib_pet_message_9 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_10_get_size(struct nvmeib_pet_message_10 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_11_get_size(struct nvmeib_pet_message_11 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

static inline u16 nvmeib_pet_message_12_get_size(struct nvmeib_pet_message_12 const* self)
{return __NVMEIB_PET_MESSAGE_GET_SIZE(self); }

#define nvmeib_pet_message_get_size(self)																	\
({																											\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_1 const*),	\
	nvmeib_pet_message_1_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_2 const*),	\
	nvmeib_pet_message_2_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_3 const*),	\
	nvmeib_pet_message_3_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_4 const*),	\
	nvmeib_pet_message_4_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_5 const*),	\
	nvmeib_pet_message_5_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_6 const*),	\
	nvmeib_pet_message_6_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_7 const*),	\
	nvmeib_pet_message_7_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_8 const*),	\
	nvmeib_pet_message_8_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_9 const*),	\
	nvmeib_pet_message_9_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_10 const*),	\
	nvmeib_pet_message_10_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_11 const*),	\
	nvmeib_pet_message_11_get_size,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(self), struct nvmeib_pet_message_12 const*),	\
	nvmeib_pet_message_12_get_size,																			\
	0))))))))))))(self);																					\
})

/*
message format(implemented):
    offset(2bytes), number of arguments(1byte), {pet variant}+
    where pet variant is written as {type(1bytes), value{(1|2|4|8) bytes}}
kaitai supports other format:
    offset, number of arguments, [type]+, [value]+

the second format probably has better performance - we are capable to write the whole struct in a single memcpy
nvmeib_pet_message struct would change to:
{
	u16 offset;
	u8 n_args;
	u8 type[1 + n_args]; //no alignment needed, since the type u8 - BUILD_BUG_ON to confirm
	u8 value[(1 + n_args)*sizeof(u64)];
	u8 size; //message size, it can be calculated at compile time
}

right now, I don't think we should go there, we can change this in future
*/

#define __NVMEIB_PET_MESSAGE_WRITE(self, out) 																					\
({																																\
	u16 idx = 0;																												\
	u16 const size = nvmeib_pet_message_get_size(self);																			\
	struct iovec buf = nvmeib_pet_stream_alloc(out, size);																		\
	if (likely(buf.iov_base)) {																									\
		nvmeib_pet_iovec_append_type(&buf, self->offset);																		\
		nvmeib_pet_iovec_append_type(&buf, (u8)(ARRAY_SIZE(self->type) - 1));													\
		for(idx = 0; idx < ARRAY_SIZE(self->type); ++idx){																		\
			nvmeib_pet_iovec_append_type(&buf, (u8)self->type[idx]);															\
			nvmeib_pet_iovec_append_any(&buf, nvmeib_pet_store_type_get_size(self->type[idx]), (u8 const*)(&self->value[idx]));	\
		}																														\
		BUG_ON(buf.iov_len != 0); /*size & write function should be fully synchronized*/										\
	}																															\
	buf.iov_base ? size : 0;																									\
})


__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_1_write(struct nvmeib_pet_message_1 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_2_write(struct nvmeib_pet_message_2 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_3_write(struct nvmeib_pet_message_3 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_4_write(struct nvmeib_pet_message_4 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_5_write(struct nvmeib_pet_message_5 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_6_write(struct nvmeib_pet_message_6 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_7_write(struct nvmeib_pet_message_7 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_8_write(struct nvmeib_pet_message_8 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_9_write(struct nvmeib_pet_message_9 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_10_write(struct nvmeib_pet_message_10 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_11_write(struct nvmeib_pet_message_11 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_message_12_write(struct nvmeib_pet_message_12 const* self, struct nvmeib_pet_stream* out)
{ return __NVMEIB_PET_MESSAGE_WRITE(self, out); }

#define nvmeib_pet_message_write(self, out)																	\
({																											\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_1 const),	\
		nvmeib_pet_message_1_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_2 const),	\
		nvmeib_pet_message_2_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_3 const),	\
		nvmeib_pet_message_3_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_4 const),	\
		nvmeib_pet_message_4_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_5 const),	\
		nvmeib_pet_message_5_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_6 const),	\
		nvmeib_pet_message_6_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_7 const),	\
		nvmeib_pet_message_7_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_8 const),	\
		nvmeib_pet_message_8_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_9 const),	\
		nvmeib_pet_message_9_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_10 const),	\
	nvmeib_pet_message_10_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_11 const),	\
	nvmeib_pet_message_11_write,																			\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(*self), struct nvmeib_pet_message_12 const),	\
	nvmeib_pet_message_12_write,																			\
	0))))))))))))(self, out);																				\
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
	u64 prev_timestamp_ns; //with high probability the next message may store delta between times, thus saving space
};

static inline struct nvmeib_pet_journal nvmeib_pet_journal_make(struct nvmeib_pet_base_controller const* controller, bool verbose)
{
	struct iovec const buffer = controller ? controller->get_buffer(controller) : (struct iovec){0};
	return (struct nvmeib_pet_journal){
	    .controller = controller,
	    .stream = nvmeib_pet_stream_make(buffer),
	    .worst_severity = NVMEIB_PET_SEVERITY_NORMAL,
		.verbose = verbose,
	    .prev_timestamp_ns = 0
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


static inline struct nvmeib_pet_variant __nvmeib_pet_journal_get_curr_message_timestamp(u64 prev_timestamp, u64 curr_timestamp)
{
	u64 const delta = curr_timestamp - prev_timestamp;
	//if ((delta & 0xFF) == delta) <== no chance to happen, even in the test for 2 sequential messages I did not see it
	if ((delta & 0xFFFF) == delta){
		return (struct nvmeib_pet_variant){.type = NVMEIB_PET_STORE_TYPE_U_SHORT, .value = delta};
	} else if ((delta & 0xFFFFFFFF) == delta){
		return (struct nvmeib_pet_variant){.type = NVMEIB_PET_STORE_TYPE_U_INT, .value = delta};
	} else {
		return (struct nvmeib_pet_variant){.type = NVMEIB_PET_STORE_TYPE_U_LONG_INT, .value = curr_timestamp};
	}
}

//don't add nvmeib_pet_journal_is_activated check here - too late - the arguments are already evaluated
#define __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg)																						\
({																																				\
	u16 written = 0;																															\
	u64 const curr_timestamp = msg.value[0];																									\
	struct nvmeib_pet_variant const timestamp_ns = __nvmeib_pet_journal_get_curr_message_timestamp(self->prev_timestamp_ns, curr_timestamp);	\
	msg.type[0] = timestamp_ns.type;																											\
	msg.value[0] = timestamp_ns.value;																											\
	written = nvmeib_pet_message_write(&msg, &self->stream);																					\
	if (written){ 																																\
		self->worst_severity = nvmeib_pet_severity_get_worst(self->worst_severity, severity);													\
		self->prev_timestamp_ns = curr_timestamp;																								\
	}																																			\
	written;																																	\
})

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_1(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_1 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_2(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_2 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_3(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_3 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_4(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_4 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_5(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_5 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_6(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_6 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_7(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_7 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_8(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_8 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_9(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_9 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_10(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_10 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_11(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_11 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
static inline u16 nvmeib_pet_journal_add_msg_12(struct nvmeib_pet_journal* self, enum nvmeib_pet_severity severity, struct nvmeib_pet_message_12 msg)
{ return __NVMEIB_PET_JOURNAL_ADD_MSG(self, severity, msg); }

__attribute__((nonnull (1)))
__attribute__((format (printf,1,2)))
static inline void nvmeib_pet_journal_add_msg_verify_format(char const * const fmt, ...)
{ (void)fmt; }


#define nvmeib_pet_journal_add_msg(self, severity, msg)												\
({																									\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_1),	\
		nvmeib_pet_journal_add_msg_1,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_2),	\
		nvmeib_pet_journal_add_msg_2,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_3),	\
		nvmeib_pet_journal_add_msg_3,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_4),	\
		nvmeib_pet_journal_add_msg_4,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_5),	\
		nvmeib_pet_journal_add_msg_5,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_6),	\
		nvmeib_pet_journal_add_msg_6,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_7),	\
		nvmeib_pet_journal_add_msg_7,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_8),	\
		nvmeib_pet_journal_add_msg_8,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_9),	\
		nvmeib_pet_journal_add_msg_9,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_10),	\
		nvmeib_pet_journal_add_msg_10,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_11),	\
		nvmeib_pet_journal_add_msg_11,																\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(msg), struct nvmeib_pet_message_12),	\
		nvmeib_pet_journal_add_msg_12,																\
	0))))))))))))(self, severity, msg);															\
})


__attribute__((nonnull (1)))
static inline void nvmeib_pet_journal_commit(struct nvmeib_pet_journal* self)
{
	if (likely(nvmeib_pet_journal_is_activated(self))){
		if (*(self->stream.written_msgs)) {
			nvmeib_pet_stream_commit(&self->stream);
			self->controller->flush(self->controller, self->worst_severity, self->stream.data);
		}

		self->controller->put_buffer(self->controller, self->stream.data);
	}

	(*self) = (struct nvmeib_pet_journal){0};
}
//}}}

#endif//__NVMEIB_PET_SPECIFICATION_H__
