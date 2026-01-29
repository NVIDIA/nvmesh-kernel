/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_SCHED_H
#define KR_INCS_SCHED_H
#ifndef __KERNEL__
	// Kernel already has those functions. Define as compatibility for user-space
	#include "kr_incs_types.h"
	// sched.h, processor.h
	#ifdef __x86_64__
		static inline void rep_nop(void) { asm volatile("rep; nop" ::: "memory"); }
		static inline void cpu_relax(void){ rep_nop(); }
	#elif defined(__aarch64__)
		static inline void cpu_relax(void){ asm volatile("yield" ::: "memory"); }
	#endif
#endif // __KERNEL__
#endif
