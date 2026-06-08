/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef __NVMEIB_PET_TYPES_H__
#define __NVMEIB_PET_TYPES_H__

#if !defined(__KERNEL__)
	#include <stddef.h>
	#include <sys/types.h>
#endif

#include "compat/kr_incs_types.h"
#include "compat/kr_incs_compiler_types.h"

//{{{ supported argument types and validation

/* PET stores compact scalar values only. Pointers are allowed as addresses,
 * but string payloads and floating point values are intentionally unsupported.
 */
#define __NVMEIB_PET_ARG_TYPE_IS(value, type) __builtin_types_compatible_p(typeof(value), type)

//counts the number of elements in __VA_ARGS__
#define NVMEIB_PET_VA_NARGS_IMPL(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,N,...) N
#define NVMEIB_PET_VA_NARGS(...) NVMEIB_PET_VA_NARGS_IMPL(__VA_ARGS__,12,11,10,9,8,7,6,5,4,3,2,1)

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

//}}}

//{{{ conversion to Python struct codes

/* These codes are Python struct module format characters. The viewer unpacks
 * PET payloads with "<" + arg_struct_code, so this mapping is the binary layout
 * contract between the C writer and Python dictionary/viewer.
 */
#define __NVMEIB_PET_ARG_STRUCT_CODE_FALLBACK(value) \
	__builtin_choose_expr(__builtin_classify_type(value) == 5, \
		'Q', \
	__builtin_choose_expr(sizeof(value) == 1, \
		'B', \
	__builtin_choose_expr(sizeof(value) == 2, \
		'H', \
	__builtin_choose_expr(sizeof(value) == 4, \
		'i', \
	__builtin_choose_expr(sizeof(value) == 8, \
		'Q', \
	'?')))))

#define __NVMEIB_PET_ARG_STRUCT_CODE(value) \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, bool), \
		'B', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, u8), \
		'B', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, uint8_t), \
		'B', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, unsigned char), \
		'B', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, char), \
		'B', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, s8), \
		'b', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, int8_t), \
		'b', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, signed char), \
		'b', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, u16), \
		'H', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, uint16_t), \
		'H', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, unsigned short), \
		'H', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, s16), \
		'h', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, int16_t), \
		'h', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, signed short), \
		'h', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, u32), \
		'I', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, uint32_t), \
		'I', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, unsigned int), \
		'I', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, s32), \
		'i', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, int32_t), \
		'i', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, int), \
		'i', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, unsigned long), \
		(sizeof(unsigned long) == 4 ? 'I' : 'Q'), \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, long), \
		(sizeof(long) == 4 ? 'i' : 'q'), \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, u64), \
		'Q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, uint64_t), \
		'Q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, unsigned long long), \
		'Q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, s64), \
		'q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, int64_t), \
		'q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, long long), \
		'q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, size_t), \
		'Q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, ssize_t), \
		'q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, void const*), \
		'Q', \
	__builtin_choose_expr(__NVMEIB_PET_ARG_TYPE_IS(value, typeof((void*)0)), \
		'Q', \
	__NVMEIB_PET_ARG_STRUCT_CODE_FALLBACK(value)))))))))))))))))))))))))))))))))

#define __NVMEIB_PET_ARG_STRUCT_CODES1(exp1) \
	__NVMEIB_PET_ARG_STRUCT_CODE(exp1)
#define __NVMEIB_PET_ARG_STRUCT_CODES2(exp1, exp2) \
	__NVMEIB_PET_ARG_STRUCT_CODES1(exp1), __NVMEIB_PET_ARG_STRUCT_CODE(exp2)
#define __NVMEIB_PET_ARG_STRUCT_CODES3(exp1, exp2, exp3) \
	__NVMEIB_PET_ARG_STRUCT_CODES2(exp1, exp2), __NVMEIB_PET_ARG_STRUCT_CODE(exp3)
#define __NVMEIB_PET_ARG_STRUCT_CODES4(exp1, exp2, exp3, exp4) \
	__NVMEIB_PET_ARG_STRUCT_CODES3(exp1, exp2, exp3), __NVMEIB_PET_ARG_STRUCT_CODE(exp4)
#define __NVMEIB_PET_ARG_STRUCT_CODES5(exp1, exp2, exp3, exp4, exp5) \
	__NVMEIB_PET_ARG_STRUCT_CODES4(exp1, exp2, exp3, exp4), __NVMEIB_PET_ARG_STRUCT_CODE(exp5)
#define __NVMEIB_PET_ARG_STRUCT_CODES6(exp1, exp2, exp3, exp4, exp5, exp6) \
	__NVMEIB_PET_ARG_STRUCT_CODES5(exp1, exp2, exp3, exp4, exp5), __NVMEIB_PET_ARG_STRUCT_CODE(exp6)
#define __NVMEIB_PET_ARG_STRUCT_CODES7(exp1, exp2, exp3, exp4, exp5, exp6, exp7) \
	__NVMEIB_PET_ARG_STRUCT_CODES6(exp1, exp2, exp3, exp4, exp5, exp6), __NVMEIB_PET_ARG_STRUCT_CODE(exp7)
#define __NVMEIB_PET_ARG_STRUCT_CODES8(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8) \
	__NVMEIB_PET_ARG_STRUCT_CODES7(exp1, exp2, exp3, exp4, exp5, exp6, exp7), __NVMEIB_PET_ARG_STRUCT_CODE(exp8)
#define __NVMEIB_PET_ARG_STRUCT_CODES9(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9) \
	__NVMEIB_PET_ARG_STRUCT_CODES8(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8), __NVMEIB_PET_ARG_STRUCT_CODE(exp9)
#define __NVMEIB_PET_ARG_STRUCT_CODES10(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10) \
	__NVMEIB_PET_ARG_STRUCT_CODES9(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9), __NVMEIB_PET_ARG_STRUCT_CODE(exp10)
#define __NVMEIB_PET_ARG_STRUCT_CODES11(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11) \
	__NVMEIB_PET_ARG_STRUCT_CODES10(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10), __NVMEIB_PET_ARG_STRUCT_CODE(exp11)
#define __NVMEIB_PET_ARG_STRUCT_CODES12(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11, exp12) \
	__NVMEIB_PET_ARG_STRUCT_CODES11(exp1, exp2, exp3, exp4, exp5, exp6, exp7, exp8, exp9, exp10, exp11), __NVMEIB_PET_ARG_STRUCT_CODE(exp12)

#define __NVMEIB_PET_ARG_STRUCT_CODES_IMPL_IMPL(n_args, ...) __NVMEIB_PET_ARG_STRUCT_CODES##n_args(__VA_ARGS__)
#define __NVMEIB_PET_ARG_STRUCT_CODES_IMPL(n_args, ...) __NVMEIB_PET_ARG_STRUCT_CODES_IMPL_IMPL(n_args, __VA_ARGS__)
#define __NVMEIB_PET_ARG_STRUCT_CODES(...) __NVMEIB_PET_ARG_STRUCT_CODES_IMPL(NVMEIB_PET_VA_NARGS(__VA_ARGS__), __VA_ARGS__)

//}}}

#endif//__NVMEIB_PET_TYPES_H__
