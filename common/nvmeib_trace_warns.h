/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_TRACE_WARNS_H
#define NVMEIB_TRACE_WARNS_H	1

#include "linux/kernel.h"
/**
 * This file must be included after #include gen_events.h,
 * at both first and second pass in order to have effect.
 * The purpose of this file is to mirror calls to WARN / WARN_ON
 * family of macros into binary log.
 * This is a hack. ftrace cannot be used for it, due to
 * the licensing issues.
 * Point is that any WARN family macro, at the bottom line expands
 * into one of the three: warn_slowpath_null/fmt/fmt_taint.
 * The trick is to define each one of these as a macro that expands
 * into itself + binary trace.
 * C preprocessor is not recursive, hence warn_slowpath* outer call
 * is treated as a macro, while the inner call is treated as a
 * function. This way each call is turned into call + trace.
 */

#define EC_BUG_PREFIX  "#EC-"
#define EC_BUG_PREFIX_FMT EC_BUG_PREFIX "%d"
#define NVMESH_BUG_PREFIX  "#NVMESH-"
#define NVMESH_BUG_PREFIX_FMT NVMESH_BUG_PREFIX "%d"

bool __nvmeib_trace_kernel_warning(const char* file, int line, const char* fmt, ...);

#if defined(BLKDEV_SIMULATOR)	// Kernel, Production code
	#define NVMEIB_DECLARE_KERNEL_WARNINGS_TRAP /* For simulator - empty implementation, because it aborts on bugs */
#else
#define NVMEIB_DECLARE_KERNEL_WARNINGS_TRAP                                                      \
	unsigned int num_warnings = 0;                                                               \
	module_param(num_warnings, uint, 0644);                                                      \
	MODULE_PARM_DESC(num_warnings, "The number of warnings the module has triggered. Contact support if this is not 0");    \
	bool __nvmeib_trace_kernel_warning(const char* file, int line, const char* fmt, ...)         \
	{                                                                                            \
		const u64 __cookie = nvmeib_public_rdtsc();                                              \
		char __strbuf[320] = {0};                                                                \
		extern nvmeib_public_save_stack_trace_t nvmeib_public_save_stack_trace_ptr;              \
		extern bool nvmeib_get_hide_warnings_stack(void);                                        \
		unsigned long st_ents[16];                                                               \
		bool known;																				 \
		struct nvmeib_stack_trace _st = {                                                        \
			.max_entries = 16, .entries = st_ents, .skip = 0, },                                 \
								  *st = (nvmeib_public_save_stack_trace_ptr) ? &_st : NULL;      \
		va_list ap;                                                                              \
		va_start(ap, fmt);                                                                       \
		vsnprintf(__strbuf, sizeof(__strbuf), fmt, ap);                                          \
		va_end(ap);                                                                              \
		if (st)                                                                                  \
			nvmeib_public_save_stack_trace_ptr(st);                                              \
		if (nvmeib_get_hide_warnings_stack()) { \
			pr_err("Error in critical resource. Error code: 1052, uuid=%llu, Additional info in nvmesh log\n", __cookie); \
		} else {	\
			pr_err(___trace_fmt_kernel_warning ". Error code: 1053\n", file, line, __cookie, __strbuf);            \
		} \
		NVMEIB_LOG_LONGTERM("KERNEL WARNING TRIGGERED AT @FILE:@LINENO SEE DMESG FOR MORE INFO COOKIE=@COOKIE, @TEXT_BUF", \
			_E, /*Deafult*/, kernel_warning, file, line, __cookie, __strbuf);                    \
		NVMEIB_LOG_LONGTERM( "Stack Trace:\n@STACK_TRACE", _E, /*Default*/, kernel_warning_stack, st);            \
		known = (!strncmp(fmt, EC_BUG_PREFIX, sizeof(EC_BUG_PREFIX) - 1)  || !strncmp(fmt, NVMESH_BUG_PREFIX, sizeof(NVMESH_BUG_PREFIX) - 1)); \
		if (NVMESH_IS_PRODUCTION_COMPILATION || !known) {							\
			num_warnings++; /* Not Atomic, So what */                                                \
		}																			\
		return !nvmeib_get_hide_warnings_stack();                                                \
	}
#endif/*BLKDEV_SIMULATOR*/

#define warn_slowpath_fmt(file,line,fmt,...) ({if (__nvmeib_trace_kernel_warning(file,line,fmt,##__VA_ARGS__)) warn_slowpath_fmt(file,line,fmt,##__VA_ARGS__);})
#define warn_slowpath_fmt_taint(file,line,taint,fmt,...) ({if (__nvmeib_trace_kernel_warning(file,line,fmt,##__VA_ARGS__)) warn_slowpath_fmt_taint(file,line,taint,fmt,##__VA_ARGS__);})
#define warn_slowpath_null(file,line) ({if (__nvmeib_trace_kernel_warning(file,line,"")) warn_slowpath_null(file,line);})
#define __warn_printk(...) ({if (__nvmeib_trace_kernel_warning(__FILE__,__LINE__,__VA_ARGS__)) __warn_printk(__VA_ARGS__);})

#define WARN_KNOWN_WRAPPER(op, cond, fmt, bug_num) ({ \
	int __rv = 0; \
	bool _panic_on_warn = nvmeib_public_panic_on_warn? !!*nvmeib_public_panic_on_warn : false; \
	bool _cond = cond; \
	if (_cond) { \
		if (!_panic_on_warn) \
			__rv = op(1, fmt, bug_num); \
		else \
			pr_warn("Known warning " fmt " ignored as panic_on_warn is set", bug_num); \
	} \
	__rv; \
})

#define WARN_KNOWN_EC(cond, bug_num) WARN_KNOWN_WRAPPER(WARN, cond, EC_BUG_PREFIX_FMT, bug_num)
#define WARN_KNOWN_EC_ONCE(cond, bug_num) WARN_KNOWN_WRAPPER(WARN_ONCE, cond, EC_BUG_PREFIX_FMT, bug_num)
#define WARN_KNOWN(cond, bug_num) WARN_KNOWN_WRAPPER(WARN, cond, NVMESH_BUG_PREFIX_FMT, bug_num)
#define WARN_KNOWN_ONCE(cond, bug_num) WARN_KNOWN_WRAPPER(WARN_ONCE, cond, NVMESH_BUG_PREFIX_FMT, bug_num)

#if defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1)
	#define BUG_NON_PRODUCTION(bug_num) WARN(1, EC_BUG_PREFIX_FMT, bug_num)
#else
	#define BUG_NON_PRODUCTION(bug_num) do { \
		_NE_dmesg(__AUTOID__, "KERNEL WARNING TRIGGERED AT @FILE:@LINENO - Bug Number: @INT, BUG_ON for debug", __FILE__, __LINE__, bug_num); \
		BUG();\
	} while (0)
#endif

#endif/*NVMEIB_TRACE_WARNS_H*/
