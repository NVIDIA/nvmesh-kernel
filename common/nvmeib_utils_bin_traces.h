/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_UTILS_BIN_TRACES_H
#define NVMEIB_UTILS_BIN_TRACES_H

#include "nvmeib_macro_utils.h"

#ifdef __TRACE_CONV_UTIL_RUN
	#pragma GCC diagnostic ignored "-Wformat"
	static void __magic__marker__(char *fmt, ...) {
		printf(fmt);
	}

	#define PLACE_MAGIC_MARKER(t, impl, fmt, ...) ({ \
		__magic__marker__(fmt, NVMEIB_VA_STRINGIFY(DUMMY_FIRST_ARG, t, ## __VA_ARGS__) "DUMMY_LAST_ARG"); \
		impl("%s", FILENAME, fmt, ## __VA_ARGS__); \
	})

	#define _F(fmt, ...) PLACE_MAGIC_MARKER(_F, nvmeib_fine, fmt, ## __VA_ARGS__)
	#define _DBG(fmt, ...) PLACE_MAGIC_MARKER(_DBG, nvmeib_dbg, fmt, ## __VA_ARGS__)
	#define _T(fmt, ...) PLACE_MAGIC_MARKER(_T, nvmeib_trace, fmt, ## __VA_ARGS__)
	#define _I(fmt, ...) PLACE_MAGIC_MARKER(_I, nvmeib_inf, fmt, ## __VA_ARGS__)
	#define _W(fmt, ...) PLACE_MAGIC_MARKER(_W, nvmeib_wrn, fmt, ## __VA_ARGS__)
	#define _E(fmt, ...) PLACE_MAGIC_MARKER(_E, nvmeib_err, fmt, ## __VA_ARGS__)
#else
	#define _F(fmt, ...) nvmeib_fine("%s", FILENAME, fmt, ## __VA_ARGS__)
	#define _DBG(fmt, ...) nvmeib_dbg("%s", FILENAME, fmt, ## __VA_ARGS__)
	#define _T(fmt, ...) nvmeib_trace("%s", FILENAME, fmt, ## __VA_ARGS__)
	#define _I(fmt, ...) nvmeib_inf("%s", FILENAME, fmt, ## __VA_ARGS__)
	#define _W(fmt, ...) nvmeib_wrn("%s", FILENAME, fmt, ## __VA_ARGS__)
	#define _E(fmt, ...) nvmeib_err("%s", FILENAME, fmt, ## __VA_ARGS__)
#endif

// For LTTNG tracing macro wrappers implementation, a common pattern is
// > If macro has args
// >   Print format with args
// > Else
// >   Print one dummy arg
// The reason for this is that kernel mode (only) lttng traces do not support zero arguments.
// So we will add a dummy parameter of type char in case of empty arguments list.
// On the other hand, lttng maximum number of arguments number is 10, which is sometimes not
// enough, hence we do not want to pass dummy argument unless we absolutely have to,
// so we will not have to split traces into two. This all is done on preprocessor stage, using
// macros defined in nvmeib_macro_utils.h
//

#define N_RF_TOKEN ""

#define N_FORMAT_STRING(name) ___trace_fmt_ ## name

#ifndef DISABLE_ALL_TRACING
#define N_FORMAT_STRING_FOR_PRINT(name, ...) N_FORMAT_STRING(name) "\n" _NVMEIB_TRACE_MARKER, current->pid, current->comm, in_interrupt() ? "/irq" : "", ## __VA_ARGS__
#else
#define N_FORMAT_STRING_FOR_PRINT(name, ...) "Tracing disabled\n" _NVMEIB_TRACE_MARKER
#endif

#ifndef MODULE_PREFIX
#define MODULE_PREFIX ""
#endif

#define _NLOGLEVEL(LVL, SCOPE, NVMEIB_LOG_CHANNEL, name, fmt, ...)                                                     \
	NVMEIB_LOG_CHANNEL(N_RF_TOKEN MODULE_PREFIX "[@K_PID/@K_TASK_NAME@IN_INTERRUPT]: " fmt, NVMEIB_CONCAT2(_, LVL), SCOPE, name,    \
	                   current->pid, current->comm, in_interrupt() ? "/irq" : "", ##__VA_ARGS__)

#ifndef _NVMEIB_TRACE_BACKEND_DMESG
#define _NMIRROR_LOGLEVEL(LVL, SCOPE, NVMEIB_LOG_CHANNEL, print_func, name, fmt, ...)                                  \
	do {                                                                                                               \
		_NLOGLEVEL(LVL, SCOPE, NVMEIB_LOG_CHANNEL, name, fmt, ##__VA_ARGS__);                                          \
		print_func(N_FORMAT_STRING_FOR_PRINT(name, ##__VA_ARGS__));                                                    \
	} while (0)
#else
#define _NMIRROR_LOGLEVEL(LVL, SCOPE, NVMEIB_LOG_CHANNEL, print_func, name, fmt, ...)                                  \
	NVMEIB_CONCAT2(_, LVL)(N_FORMAT_STRING_FOR_PRINT(name, ##__VA_ARGS__));
#endif


#define NVMEIB_DMESG_MIRROR_HEADER_FMT "(%lu) [%d]%s:%u [%s] "
#define NVMEIB_DMESG_MIRROR_HEADER_ARGS nvmeib_trace_tsc_to_ns(nvmeib_public_rdtsc()), raw_smp_processor_id(), FILENAME, __LINE__, __FUNCTION__
#define nvmeib_dmesg_mirror(prefix, LVL, fmt, ...) \
        pr_ ## LVL(prefix NVMEIB_DMESG_MIRROR_HEADER_FMT fmt, NVMEIB_DMESG_MIRROR_HEADER_ARGS, ## __VA_ARGS__);

#define nvmeib_dmesg_mirror_dbg(fmt, ...) \
	nvmeib_dmesg_mirror("DBG: ", dbg, fmt, ## __VA_ARGS__)
#define nvmeib_dmesg_mirror_trace(fmt, ...) \
	nvmeib_dmesg_mirror("TRACE: ", trace, fmt, ## __VA_ARGS__)
#define nvmeib_dmesg_mirror_inf(fmt, ...) \
	nvmeib_dmesg_mirror("INFO: ", info, fmt, ## __VA_ARGS__)
#define nvmeib_dmesg_mirror_wrn(fmt, ...) \
	nvmeib_dmesg_mirror("WARN: ", warn_ratelimited, fmt, ## __VA_ARGS__)
#define nvmeib_dmesg_mirror_err(fmt, ...) \
	nvmeib_dmesg_mirror("ERROR: ", err_ratelimited, fmt, ## __VA_ARGS__)

/* The following macros send their output to the longterm channel */
#define _NF(name, fmt, ...) _NLOGLEVEL(F, /*Default Scope*/, NVMEIB_LOG_LONGTERM, name, fmt, ## __VA_ARGS__)
#define _ND(name, fmt, ...) _NLOGLEVEL(DBG, /*Default Scope*/, NVMEIB_LOG_LONGTERM, name, fmt, ## __VA_ARGS__)
#define _NT(name, fmt, ...) _NLOGLEVEL(T, /*Default Scope*/, NVMEIB_LOG_LONGTERM, name, fmt, ## __VA_ARGS__)

#ifndef NVMEIB_MIRROR_TO_DMESG_BY_DEFAULT
	#define _NI(name, fmt, ...) _NLOGLEVEL(I, /*Default Scope*/, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
	#define _NW(name, fmt, ...) _NLOGLEVEL(W, /*Default Scope*/, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
	#define _NE(name, fmt, ...) _NLOGLEVEL(E, /*Default Scope*/, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
#else
	#define _NI(...) _NI_dmesg(__VA_ARGS__)
	#define _NW(...) _NW_dmesg(__VA_ARGS__)
	#define _NE(...) _NE_dmesg(__VA_ARGS__)
#endif

#define _NF_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(F, scope, NVMEIB_LOG_LONGTERM, name, fmt, ## __VA_ARGS__)
#define _ND_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(DBG, scope, NVMEIB_LOG_LONGTERM, name, fmt, ## __VA_ARGS__)
#define _NT_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(T, scope, NVMEIB_LOG_LONGTERM, name, fmt, ## __VA_ARGS__)
#define _NI_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(I, scope, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
#define _NW_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(W, scope, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
#define _NE_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(E, scope, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)

/* Those macros will print to binary tracer and dmesg */
#define _NF_dmesg(name, fmt, ...) _NMIRROR_LOGLEVEL(F, /*Default Scope*/, NVMEIB_LOG_LONGTERM, nvmeib_dmesg_mirror_dbg, name, fmt, ## __VA_ARGS__)
#define _ND_dmesg(name, fmt, ...) _NMIRROR_LOGLEVEL(DBG, /*Default Scope*/, NVMEIB_LOG_LONGTERM, nvmeib_dmesg_mirror_dbg, name, fmt, ## __VA_ARGS__)
#define _NT_dmesg(name, fmt, ...) _NMIRROR_LOGLEVEL(T, /*Default Scope*/, NVMEIB_LOG_LONGTERM, nvmeib_dmesg_mirror_trace, name, fmt, ## __VA_ARGS__)
#define _NI_dmesg(name, fmt, ...) _NMIRROR_LOGLEVEL(I, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_inf, name, fmt, ## __VA_ARGS__)
#define _NW_dmesg(name, fmt, ...) _NMIRROR_LOGLEVEL(W, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_wrn, name, fmt, ## __VA_ARGS__)
#define _NE_dmesg(name, fmt, ...) _NMIRROR_LOGLEVEL(E, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_err, name, fmt, ## __VA_ARGS__)

/* description: print to dmesg and binary tracer in a given trace level
 * parameters: trace level: trace level (see definition of nvmeib_trace_level)
 *             name: name of the trace
 * side effect: print to dmesg and binary tracer
 */
#define _N_dmesg(trace_level, name, ...)                                                                               \
	({                                                                                                             \
		nvmeib_trace_level level = trace_level;                                                                \
		switch (level) {                                                                                       \
		case T_INFO:                                                                                           \
			_NI_dmesg(name##_info, __VA_ARGS__);                                                           \
			break;                                                                                         \
		case T_WARN:                                                                                           \
			_NW_dmesg(name##_warn, __VA_ARGS__);                                                           \
			break;                                                                                         \
		case T_ERROR:                                                                                          \
		case T_BUG:                                                                                            \
			_NE_dmesg(name##_err, __VA_ARGS__);                                                            \
			break;                                                                                         \
                default:                                                                                               \
                        _NI_dmesg(name##_info, __VA_ARGS__);                                                           \
		}                                                                                                      \
	})

#define _NF_to_user(name, topic, fmt, ...) _NMIRROR_LOGLEVEL(F, /*Default Scope*/, NVMEIB_LOG_LONGTERM, nvmeib_dmesg_mirror_dbg,   name, NVMESH_TRACE_MODULE "-" topic ": " fmt, ## __VA_ARGS__)
#define _ND_to_user(name, topic, fmt, ...) _NMIRROR_LOGLEVEL(DBG, /*Default Scope*/, NVMEIB_LOG_LONGTERM, nvmeib_dmesg_mirror_dbg,   name, NVMESH_TRACE_MODULE "-" topic ": " fmt, ## __VA_ARGS__)
#define _NT_to_user(name, topic, fmt, ...) _NMIRROR_LOGLEVEL(T, /*Default Scope*/, NVMEIB_LOG_LONGTERM, nvmeib_dmesg_mirror_trace, name, NVMESH_TRACE_MODULE "-" topic ": " fmt, ## __VA_ARGS__)
#define _NI_to_user(name, topic, fmt, ...) _NMIRROR_LOGLEVEL(I, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_inf,   name, NVMESH_TRACE_MODULE "-" topic ": " fmt, ## __VA_ARGS__)
#define _NW_to_user(name, topic, fmt, ...) _NMIRROR_LOGLEVEL(W, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_wrn,   name, NVMESH_TRACE_MODULE "-" topic ": " fmt, ## __VA_ARGS__)
#define _NE_to_user(name, topic, fmt, ...) _NMIRROR_LOGLEVEL(E, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_err,   name, NVMESH_TRACE_MODULE "-" topic ": " fmt, ## __VA_ARGS__)

/* Auto binary trace ID, resolved to filename_line, requires __FILE_LITERAL__ infra */
#ifndef __FILE_LITERAL__
#pragma GCC error __FILE_LITERAL__ not defined, Makefile error, should not happen
#endif
#define __AUTOID__ NVMEIB_CONCAT2(__FILE_LITERAL__, NVMEIB_CONCAT2(_, __LINE__))

/* The following macros send their output to the eternal channel */
#define _NF_ETER(name, fmt, ...) _NLOGLEVEL(F, /*Default Scope*/, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
#define _ND_ETER(name, fmt, ...) _NLOGLEVEL(DBG, /*Default Scope*/, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
#define _NT_ETER(name, fmt, ...) _NLOGLEVEL(T, /*Default Scope*/, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
    // Those macros will print to binary tracer and dmesg (default behaviour for high severity)
#define _NI_ETER(name, fmt, ...) _NMIRROR_LOGLEVEL(I, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_inf, name, fmt, ## __VA_ARGS__)
#define _NW_ETER(name, fmt, ...) _NMIRROR_LOGLEVEL(W, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_wrn, name, fmt, ## __VA_ARGS__)
#define _NE_ETER(name, fmt, ...) _NMIRROR_LOGLEVEL(E, /*Default Scope*/, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_err, name, fmt, ## __VA_ARGS__)

/* The following macros send their output to the eternal channel */
#define _NF_ETER_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(F, scope, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
#define _ND_ETER_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(DBG, scope, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
#define _NT_ETER_SCOPE(name, scope, fmt, ...) _NLOGLEVEL(T, scope, NVMEIB_LOG_ETERNAL, name, fmt, ## __VA_ARGS__)
    // Those macros will print to binary tracer and dmesg (default behaviour for high severity)
#define _NI_ETER_SCOPE(name, scope, fmt, ...) _NMIRROR_LOGLEVEL(I, scope, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_inf, name, fmt, ## __VA_ARGS__)
#define _NW_ETER_SCOPE(name, scope, fmt, ...) _NMIRROR_LOGLEVEL(W, scope, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_wrn, name, fmt, ## __VA_ARGS__)
#define _NE_ETER_SCOPE(name, scope, fmt, ...) _NMIRROR_LOGLEVEL(E, scope, NVMEIB_LOG_ETERNAL, nvmeib_dmesg_mirror_err, name, fmt, ## __VA_ARGS__)

#define FIN _F("-->\n")
#define FOUT _F("<--\n")
#ifndef LLVM
	#define NFIN _NF(__AUTOID__, "-->")
	#define NFOUT _NF(__AUTOID__, "<--")
#else
	#define NFIN do {} while (0)
	#define NFOUT do {} while (0)
#endif
#define FINS(x) _F("--> %s\n", x)
#define FOUTS(x) _F("<-- %s\n", x)
#ifndef LLVM
	#define NFINS(x) _NF(__AUTOID__, "--> @FINOUT_PARAM", x)
	#define NFOUTS(x) _NF(__AUTOID__, "<-- @FINOUT_PARAM", x)
	#define NLINE _NF(__AUTOID__, "---\n")
#else
	#define NFINS(x) do {} while (0)
	#define NFOUTS(x) do {} while (0)
	#define NLINE do {} while (0)
#endif
#define LINE _F("---\n")

#define IFINS(x) _I("--> %s\n", x)
#define IFOUTS(x) _I("<-- %s\n", x)
#define IFIN _I("-->\n")
#define IFOUT _I("<--\n")

#endif /* NVMEIB_UTILS_BIN_TRACES_H */

