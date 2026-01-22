/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_TRACE_H
#define NVMEIB_TRACE_H

#if !defined(__FIRST_PASS__) && !defined(DISABLE_ALL_TRACING)
	#define _CAT_(a, ...) _CAT1_(a, __VA_ARGS__)
	#define _CAT1_(a, ...) a ## __VA_ARGS__
	#define _MAKE_TRACE_NAME(system, type, name)               _CAT_(_CAT_(_CAT_(trace_,system),_##type),_##name)
	#define NVMEIB_LOG_LONGTERM(fmt, macro, scope, name, ...)  _MAKE_TRACE_NAME(NVMESH_TRACE_NAMESPACE, LONG, name)(__VA_ARGS__)
	#define NVMEIB_LOG_SHORTTERM(fmt, macro, scope, name, ...) _MAKE_TRACE_NAME(NVMESH_TRACE_NAMESPACE, SHORT, name)(__VA_ARGS__)
	#define NVMEIB_LOG_GOODPATH(fmt, macro, scope, name, ...)  _MAKE_TRACE_NAME(NVMESH_TRACE_NAMESPACE, GOODPATH, name)(__VA_ARGS__)
	#define NVMEIB_LOG_METRICS(fmt, macro, scope, name, ...)   _MAKE_TRACE_NAME(NVMESH_TRACE_NAMESPACE, METRICS, name)(__VA_ARGS__)
	#define NVMEIB_LOG_EPHEMERAL(fmt, macro, scope, name, ...) _MAKE_TRACE_NAME(NVMESH_TRACE_NAMESPACE, EPH, name)(__VA_ARGS__)
	#define NVMEIB_LOG_ETERNAL(fmt, macro, scope, name, ...)   _MAKE_TRACE_NAME(NVMESH_TRACE_NAMESPACE, ETER, name)(__VA_ARGS__)
#endif

#ifdef DISABLE_ALL_TRACING
	#include "common/compat/kr_incs_dummy_empty_traces.h"
#elif defined(TOMA)	/* production toma code */
	#ifdef USER_SPACE_TRACING
		#include "tools/nvmeib_trace_userspace.h"
		extern struct trace_channel *nvmeibt_trace_long;
		extern struct trace_channel *nvmeibt_trace_short;
		extern struct trace_channel *nvmeibt_trace_eph;
		extern struct trace_channel *nvmeibt_trace_eter;
	#endif
#elif defined(BLKDEV_SIMULATOR)	/* kernel simulator */
	#define _NVMEIB_TRACE_MARKER "\001"
	#ifdef USER_SPACE_TRACING
		#include "tools/nvmeib_trace_userspace.h"
		extern struct trace_channel *nvmeibc_trace_long;
		extern struct trace_channel *nvmeibc_trace_short;
		extern struct trace_channel *nvmeibc_trace_goodpath;
		extern struct trace_channel *nvmeibc_trace_metrics;
		extern struct trace_channel *nvmeibc_trace_eph;
		extern struct trace_channel *nvmeibc_trace_eter;
	#endif
	#include "gen_events.h"
#elif defined(USER_SPACE_TRACING)	/* Other user space aps */
	#include "tools/nvmeib_trace_userspace.h"
	#include <string.h>
	#include <stdio.h>
	#include <limits.h>
	#include "gen_events.h"
#else	/* production kernel code */
	#include "nvmeib_trace_macro_utils.h"
	#include "nvmeib_public.h"
	#include "nvmeib_public_mmap.h"
	struct nvmeib_capuch;

	/* Define channels configuration in one place */
	#define NVMEIB_TRACE_CH_LIST                                                   \
		(nvmeibc_trace_eph), (nvmeibs_trace_eph), (nvmeibm_trace_eph), (nvmeibc_trace_long),            \
			(nvmeibc_trace_short), (nvmeibc_trace_goodpath),                 \
			(nvmeibs_trace_goodpath), (nvmeibc_trace_eter), (nvmeibs_trace_long),  \
			(nvmeibm_trace_long), (nvmeibp_trace_long), (nvmeibp_trace_eter), 	   \
			(nvmeibm_trace_eter), (nvmeibs_trace_eter), (nvmeibc_trace_metrics), (nvmeibs_trace_metrics) \

	render_per_cpu_declare(NVMEIB_OPEN_BRACKETS(NVMEIB_TRACE_CH_LIST));

	extern void *nvmeib_add_trace(int len, struct nvmeib_capuch *info, u32 cksum);

	extern void nvmeibc_dump_ephemeral(void);
	extern void nvmeibs_dump_ephemeral(void);

	struct nvmeib_tracer_public_api {
		bool (*sym_resolve_kernel_bug_can_happen)(void *addr);
	};
	extern void nvmeib_reg_tracer_public_api(struct nvmeib_tracer_public_api *api);
	extern void nvmeib_unreg_tracer_public_api(void);

	extern int nvmeib_symbol_length(void *addr);
	extern int nvmeib_symbol_strcpy(char *dest, void *addr, int len);
	extern int nvmeib_stack_trace_length(void *addr);
	extern int nvmeib_stack_trace_strcpy(char *dest, void *addr, int len);

	extern unsigned long nvmeib_trace_tsc_to_ns(unsigned long timestamp);

	struct proc_dir_entry;
	int nvmeib_init_traces(struct proc_dir_entry *proc_dir);
	void nvmeib_stop_traces(void);

	struct nvmeib_stack_trace;
	typedef void (*nvmeib_public_save_stack_trace_t)(struct nvmeib_stack_trace *);

	#define _NVMEIB_TRACE_MARKER

	/* Use this Define at the top of .c file if in order to enable all traces */
	#define FORCE_FILE_TRACES
#endif

#endif // NVMEIB_TRACE_H
