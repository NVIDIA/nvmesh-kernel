/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_MODULE_H
#define KR_INCS_MODULE_H

#if 0 /* [Jared]: Redundant now we have removed include to OFED linux/compat-2.6.h */
#if !defined(__copy)
	#define __copy(symbol)
	#if (__GNUC__ >= 7)
		#if __has_attribute(__copy__)
			# undef __copy
			# define __copy(symbol)	__attribute__((__copy__(symbol)))
		#endif
	#endif
#endif

#undef module_init
#define module_init(initfn)                                     \
	static inline initcall_t __inittest(void) { return initfn; } \
	int init_module(void) __copy(initfn) __attribute__((alias(#initfn)));

#undef module_exit
#define module_exit(exitfn)                                     \
	static inline exitcall_t __exittest(void) { return exitfn; } \
	void cleanup_module(void) __copy(exitfn) __attribute__((alias(#exitfn)));
#endif

#endif
