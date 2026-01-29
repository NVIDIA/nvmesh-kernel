/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KERNEL_BASE_TYPES_H
#define KERNEL_BASE_TYPES_H
#ifdef __KERNEL__
	#ifndef sizeof_field
		#define sizeof_field FIELD_SIZEOF
	#endif
	#ifndef FIELD_SIZEOF
		#define FIELD_SIZEOF sizeof_field
	#endif
#else
	// Kernel already has those functions. Define as compatibility for user-space
	#include <stdio.h>
	#include <stdint.h>
	#include <string.h>		// memset() / memcpy()
	#include <errno.h>
	#include <ctype.h>

	// Generic types.
	typedef uint8_t		u8,	__u8;
	typedef uint16_t	u16,__u16, __be16, umode_t, efi_char16_t;
	typedef uint32_t	u32,__u32, __be32, __le32, gfp_t, fmode_t /*, dev_t*/;
	typedef unsigned long long int /*uint64_t*/ u64,__u64, __le64, __be64, dma_addr_t, sector_t; // Wrong: NVMesh uses wrong definition of 64 bits as long long (which might be 128bits)
	#if !(defined(NVASSERT_H_INCLUDED))				// UM_APP, In UM app/common/nvassert.h -> includes spdk rte_log.h -> rte_common.h which defines this.
		typedef unsigned long long phys_addr_t;		//
	#endif
	typedef signed char s8;
	typedef signed short s16;
	typedef signed int s32;
	typedef signed long long s64;

	#define likely(x)	__builtin_expect(!!(x), 1)
	#define unlikely(x)	__builtin_expect(!!(x), 0)

	#if !defined(PAGE_SHIFT)			// On arm platform PAGE_SIZE is already defined
		#define PAGE_SHIFT 12			// Default 4K
	#endif

	// linux/kernel.h
	#ifndef _GNU_SOURCE							// In gnu source this is not a define but compiler def so code below will not compile
		#ifndef INT_MAX							// might be defined in limits.h
			#define INT_MAX         ((int)(~0U>>1))
			#define INT_MIN         (-INT_MAX - 1)
			#define LONG_MAX		((long)(~0UL>>1))
			#define ULONG_MAX		(~0UL)
		#endif
	#endif

	#include "../nvmeib_math.h"
	#define min3(x, y, z) min((typeof(x))min(x, y), z)
	#define max3(x, y, z) max((typeof(x))max(x, y), z)
	#define swap(x, y) ({ typeof(x) tmp = (x); (x) = (y); (y) = tmp; })
	#define sizeof_field(t, f) (sizeof(((t*)0)->f))

	// /linux/cache.h
	#define L1_CACHE_BYTES		64
	#define SMP_CACHE_BYTES 	L1_CACHE_BYTES
	#define cache_line_size()	L1_CACHE_BYTES
	#define __must_check          	__attribute__((warn_unused_result))
	#define ____cacheline_aligned 	__attribute__((__aligned__(SMP_CACHE_BYTES)))
	#define __section(S) __attribute__ ((__section__(#S)))

	// linux/limits.h
	#define NAME_MAX         255	/* # chars in a file name */
	#define PATH_MAX        4096	/* # chars in a path name including nul */

	#include <stdbool.h>	// bool
	#ifndef NULL
		#define NULL ((void*)0)
	#endif

	//#include <linux/moduleparam.h>
	#define MODULE_AUTHOR(_str)
	#define MODULE_DESCRIPTION(X)
	#define MODULE_LICENSE(X)
	#define module_param(name, type, perm)
	#define module_param_named(name, value, type, perm)
	#define module_param_string(name, string, len, perm)
	#define MODULE_PARM_DESC(_parm, desc)

	// /linux/export.h
	#define EXPORT_SYMBOL(sym)

	// include/linux/err.h
	#define MAX_ERRNO	4095
	#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
	static inline void *ERR_PTR(     unsigned long err) { return (void *)err; }
	static inline long  PTR_ERR(       const void *ptr) { return (unsigned long)ptr; }
	static inline bool  IS_ERR(        const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
	static inline bool  IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }
#endif // __KERNEL__

#ifndef __clang__
#ifndef FALLTHRU
	#if (__GNUC__ >= 7)
		#if __has_attribute(__fallthrough__)
			#define FALLTHRU __attribute__ ((__fallthrough__))
		#else
			#define FALLTHRU __attribute__ ((fallthrough))
		#endif
	#else
		#define FALLTHRU
	#endif
#endif
#else
	#define FALLTHRU __attribute__ ((__fallthrough__))
#endif
#ifndef __bitwise
	#define __bitwise
#endif

#endif // KERNEL_BASE_TYPES_H
