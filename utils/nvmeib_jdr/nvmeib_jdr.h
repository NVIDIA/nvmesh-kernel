/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_JDR_H_INCLUDE
#define NVMEIB_JDR_H_INCLUDE

#ifdef __KERNEL__
	#include <linux/types.h>
	#include <linux/bug.h>      // WARN_ON
	#include <linux/kernel.h>   // for vsnprintf
	#include <linux/string.h> // strstr
	#include <linux/seq_file.h> // seq_file support
	#define JDR_ASSERT(cond) WARN_ON(!cond)
#else
	#include <stdint.h>
	#include <stddef.h>    // size_t
	#include <stdbool.h>   // bool, true, false
	#include <assert.h>    // assert
	#include <errno.h>
	#include <stdio.h>
	#include <string.h>
	#include <stdarg.h>    // va_list
	#define JDR_ASSERT(cond) assert(cond)
#endif

#ifndef UUID_BE
	typedef struct {
		unsigned char b[16];
	} uuid_be;
#endif

struct charvec {
	char* base;
	size_t len;
};

struct jdr_null_type{void* dummy;};

struct jdr{
	struct{
		size_t total;
		struct charvec input;
		struct charvec remaining;
		uint32_t nesting;
		bool is_first_value;
#ifdef __KERNEL__
		struct seq_file *seq; // if non-NULL, output to seq_file instead of buffer
#endif
	void (*append)(struct jdr* self, char const * const fmt, va_list args);
	struct charvec (*finalize)(struct jdr* self);

	} impl;

	struct {
		void (*null)(struct jdr* self, char const * name, struct jdr_null_type null);
		void (*boolean)(struct jdr* self, char const * name, bool value);
		void (*u8)(struct jdr* self, char const * name, uint8_t value);
		void (*s8)(struct jdr* self, char const * name, int8_t value);
		void (*u16)(struct jdr* self, char const * name, uint16_t value);
		void (*s16)(struct jdr* self, char const * name, int16_t value);
		void (*u32)(struct jdr* self, char const * name, uint32_t value);
		void (*s32)(struct jdr* self, char const * name, int32_t value);
		void (*u64)(struct jdr* self, char const * name, uint64_t value);
		void (*s64)(struct jdr* self, char const * name, int64_t value);
		void (*ull)(struct jdr* self, char const * name, unsigned long long value);
		void (*sll)(struct jdr* self, char const * name, long long value);
		void (*ptr)(struct jdr* self, char const * name, void const * value);
		void (*ascii)(struct jdr* self, char const * name, char const * text);
		void (*ascii_format)(struct jdr* self, char const* name, char const * fmt, ...);
		void (*bitmap)(struct jdr* self, char const * name, unsigned long long value);
		void (*uuid_be)(struct jdr* self, char const * name, uuid_be uuid);

		struct jdr* (*object)(struct jdr* self, char const * name);
		void (*object_done)(struct jdr* self);

		struct jdr* (*array)(struct jdr* self, char const * name);
		void (*array_done)(struct jdr* self);
	} ops;
};

//
// the following functions APIs, note that for nvmeibc/s/common they are exported by nvmeib_public.c
//

// function to write a string key and string value pair directly
void jdr_write_key_value_str(struct jdr *jdr, const char *key, const char *value);

struct jdr jdr_make(struct charvec buffer);
#ifdef __KERNEL__
struct jdr jdr_make_seq(struct seq_file *seq);
#endif

//once you done serializing all your objects into the jdr archive - you should call jdr_finalize function;
//the function should be called before jdr_free
//jdr functionality follows *printf* family convention:
//    if there is enough space in the buffer - return charvec{.base=buffer.base, .len=<number of written characters>}
//    otherwise - return charvec{.base=NULL, .len=<number of would be written bytes>}
struct charvec jdr_finalize(struct jdr* jdr);


//a small helpers to avoid calling done manually
static inline void __jdr_on_object_done(struct jdr** jdr)
{ 
	if (*jdr){
		(*jdr)->ops.object_done(*jdr); 
	}
}

#ifndef CONCATENATE
#define CONCATENATE_DETAIL(x, y) x##y
#define CONCATENATE(x, y) CONCATENATE_DETAIL(x, y)
#endif

#define UNIQUE_NAME(base) CONCATENATE(base, __COUNTER__)

#define jdr_object_scope(jdr_inst, name) \
__attribute__((cleanup(__jdr_on_object_done))) struct jdr* UNIQUE_NAME(jdr_object_scope) = (jdr_inst)->ops.object((jdr_inst), (name))

static inline void __jdr_on_array_done(struct jdr** jdr)
{ 
	if (*jdr){
		(*jdr)->ops.array_done(*jdr); 
	}
}

#define jdr_array_scope(jdr_inst, name) \
__attribute__((cleanup(__jdr_on_array_done))) struct jdr* UNIQUE_NAME(jdr_array_scope) = (jdr_inst)->ops.array((jdr_inst), (name))

#define jdr_select_writer(jdr_inst, value)														\
({																								\
	__auto_type __jdr_func = 																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), bool),				\
			(jdr_inst)->ops.boolean,																\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint8_t),				\
			(jdr_inst)->ops.u8,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int8_t),				\
			(jdr_inst)->ops.s8,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint16_t),			\
			(jdr_inst)->ops.u16,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int16_t),				\
			(jdr_inst)->ops.s16,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint32_t),			\
			(jdr_inst)->ops.u32,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int32_t),				\
			(jdr_inst)->ops.s32,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uint64_t),			\
			(jdr_inst)->ops.u64,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), int64_t),				\
			(jdr_inst)->ops.s64,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), unsigned long long),	\
			(jdr_inst)->ops.ull,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), long long),			\
			(jdr_inst)->ops.sll,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), void const *),		\
			(jdr_inst)->ops.ptr,																	\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), char const *),		\
			(jdr_inst)->ops.ascii,																\
		__builtin_choose_expr(__builtin_types_compatible_p(typeof(value), uuid_be),				\
			(jdr_inst)->ops.uuid_be,																\
		(void)0 ))))))))))))));																	\
 	__jdr_func;																					\
})

//write any variable
#define jdr_write_var(jdr_inst, tag, value)													\
({																								\
	__auto_type __jdr_write_function = jdr_select_writer(jdr_inst, value);						\
	__jdr_write_function(jdr_inst, #tag, value);												\
})

//write member variable
#define jdr_write(jdr_inst, inst_ptr, field_tag)	\
({													\
	__auto_type __value = (inst_ptr)->field_tag;	\
 	jdr_write_var(jdr_inst, field_tag, __value);	\
})

//write member variable, which is pointer
#define jdr_write_optional(jdr_inst, inst_ptr, field_tag) 		\
({																\
	__auto_type __value_ptr = (inst_ptr)->field_tag;			\
	if (__value_ptr){ 											\
		jdr_write_var(jdr_inst, field_tag, *__value_ptr);		\
 	} else {													\
		struct jdr_null_type const null_value = {0};			\
		jdr_inst->ops.null(jdr_inst, #field_tag, null_value);	\
 	}															\
})

//+((inst_ptr)->field_tag) - triggers int promotion; thus we get the correct int type
#define jdr_write_bitfield(jdr_inst, inst_ptr, field_tag)				\
({																		\
	typeof(+((inst_ptr)->field_tag)) __value = (inst_ptr)->field_tag;	\
 	jdr_write_var(jdr_inst, field_tag, __value);						\
})

#define jdr_write_bitmap(jdr_inst, inst_ptr, field_tag) 				\
	jdr_inst->ops.bitmap(jdr_inst, #field_tag, inst_ptr->field_tag)

#define jdr_write_array(jdr_inst, name, arr, arr_size, write_function) 					\
({																						\
	jdr_array_scope(jdr_inst, name);													\
	size_t __jdr_array_idx;								\
	for(__jdr_array_idx = 0; __jdr_array_idx < arr_size; ++__jdr_array_idx){ 	\
		write_function(jdr_inst, NULL, &(arr[__jdr_array_idx]));						\
	}																					\
})

#define jdr_write_s_array(jdr_inst, name, arr, write_function) \
	jdr_write_array(jdr_inst, name, arr, ARRAY_SIZE(arr), write_function)

#define jdr_write_fundamental_array(jdr_inst, name, arr, arr_size) 							\
({																							\
	jdr_array_scope(jdr_inst, name);														\
	size_t __jdr_array_idx;									\
	for(__jdr_array_idx = 0; __jdr_array_idx < arr_size; ++__jdr_array_idx){ 		\
		__auto_type __jdr_cell_value = arr[__jdr_array_idx];								\
		__auto_type __jdr_write_function = jdr_select_writer(jdr_inst, __jdr_cell_value);	\
		__jdr_write_function(jdr_inst, NULL, __jdr_cell_value);								\
	}																						\
})

#define jdr_write_fundamental_s_array(jdr_inst, name, arr) \
	jdr_write_fundamental_array(jdr_inst, name, arr, ARRAY_SIZE(arr))

#define jdr_write_bitmap_array(jdr_inst, name, arr, arr_size) 						\
({																					\
	jdr_array_scope(jdr_inst, name);												\
	size_t __jdr_array_idx;							\
	for(__jdr_array_idx = 0; __jdr_array_idx < arr_size; ++__jdr_array_idx){ \
		jdr_inst->ops.bitmap(jdr_inst, NULL, arr[__jdr_array_idx]);					\
	}																				\
})

#define jdr_write_bitmap_s_array(jdr_inst, name, arr) 	\
	jdr_write_bitmap_array(jdr_inst, name, arr, ARRAY_SIZE(arr))

#ifndef __KERNEL__
int nvmeib_write_file(const char *filename, const char *buf, size_t len);
#endif
#endif//NVMEIB_JDR_H_INCLUDE
