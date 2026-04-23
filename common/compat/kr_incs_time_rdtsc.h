/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_TIME_RDTSC_H
#define KR_INCS_TIME_RDTSC_H

#include "kr_incs_types.h"
#if !defined(__KERNEL__)
	typedef unsigned long cycles_t;
#endif
#if defined(__i386__) || defined(__x86__) || defined(__x86_64__)
	static inline cycles_t nvmeib_public_rdtsc(void) {
		uint32_t low, high;
		asm volatile ("rdtsc" : "=a" (low), "=d" (high));
		return ((uint64_t)high << 32) | (uint64_t)low;
	}
	static inline unsigned int nvmeib_public_tsc_khz(void) {
		extern unsigned int tsc_khz;
		return tsc_khz;
	}
#elif defined(__KERNEL__)
	static inline u64 nvmeib_public_rdtsc(void) {		// Emulation as if processor speed is exactly 1[GHz]
		return ktime_to_ns(ktime_get_raw());	// Wrapper of GPL: ktime_get_raw()
	}
	static inline unsigned int nvmeib_public_tsc_khz(void) {
		return 1000000;	/* We used ktime_get_raw (in ns) */
	}
	#if !KS_USE_RDTSC
		#define rdtsc() native_read_tsc()	/* Unused in our kernel code, todo, remove */
	#endif
#elif defined(__aarch64__)
#include <time.h>
	static inline u64 nvmeib_public_rdtsc(void) {		// Emulation as if processor speed is exactly 1[GHz]
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
		return ((((unsigned long long)ts.tv_sec)*1000000000ULL) + (((unsigned long long)(ts.tv_nsec))));	// Nano seconds
	}
	static inline unsigned int nvmeib_public_tsc_khz(void) {
		return 1000000;	/* We used clock_gettime in ns, same as kernel's ktime_get_raw */
	}
#elif __has_include("rte_cycles.h")
	#include "rte_cycles.h" // use Nvidia user space DPDK functionality
	#define nvmeib_public_rdtsc() rte_rdtsc()
#elif __has_include(<x86intrin.h>)
	#include <x86intrin.h>					// Should be covered by above options and never called, jsut as backup
	#define nvmeib_public_rdtsc() __rdtsc()
#elif defined(LLVM)							// LLVM Compiler specific: uint64_t __rdtsc(void)
	#define nvmeib_public_rdtsc() __rdtsc()
#elif defined(__GNUC__)						// GCC Compiler specific only
	#define nvmeib_public_rdtsc() __builtin_ia32_rdtsc()
	#error "u32 not u64, may not work, legacy Toma code. Should never enter this case!"
#elif defined(_MSC_VER)						// MSVC Compiler specific: uint64_t __rdtsc(void)
	#include <intrin.h>
	#define nvmeib_public_rdtsc() __rdtsc()
#else
	#error "rdtsc functionality is missing";
#endif

#if !defined(__KERNEL__)
	static inline unsigned long long native_read_tsc(void) { return nvmeib_public_rdtsc(); }
	static inline unsigned long      get_cycles(     void) { return nvmeib_public_rdtsc(); }
#endif

#endif
