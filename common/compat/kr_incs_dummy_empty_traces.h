/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_DUMMY_EMPTY_TRACES_H
#define KR_INCS_DUMMY_EMPTY_TRACES_H

#define NVMEIBT_TRACE_H		// include "toma/interface/log.h"

#define TRACE_INCLUDE_FILE "kr_incs.h"
#define _ND(...) ({})
#define _NT(...) ({})
#define _NE(...) ({})
#define _NF(...) ({})
#define _NW(...) ({})
#define _NI(...) ({})
#define _NI_to_user(...) ({})
#define _NE_to_user(...) ({})
#define _NW_to_user(...) ({})
#define _NI_dmesg(...) ({})
#define _NW_dmesg(...) ({})

#define _NF_SCOPE(...) ({})
#define _ND_SCOPE(...) ({})
#define _NT_SCOPE(...) ({})
#define _NI_SCOPE(...) ({})
#define _NW_SCOPE(...) ({})
#define _NE_SCOPE(...) ({})


#define _N_dmesg(trace_level, name, ...) _NI_dmesg(name, ## __VA_ARGS__)
#define _T(fmt, ...)
#define NFIN  do {} while (0)
#define NFOUT do {} while (0)
#define N_FORMAT_STRING_FOR_PRINT(name, ...)  "%s", "dummy\n" // name, ## __VA_ARGS__

static inline int __do_nothing_trace(int dummy, const char *fmt, ...) {(void)fmt; return dummy;}
#define NVMEIB_LOG_LONGTERM(fmt, name, ...)		__do_nothing_trace(0, fmt) //, __VA_ARGS__)
//#define NVMEIB_LOG_LONGTERM(fmt, name, ...)		__do_nothing_trace(0, fmt __VA_OPT__(,) __VA_ARGS__)
#define NVMEIB_LOG_SHORTTERM  NVMEIB_LOG_LONGTERM
#define NVMEIB_LOG_GOODPATH   NVMEIB_LOG_LONGTERM
#define NVMEIB_LOG_METRICS    NVMEIB_LOG_LONGTERM
#define NVMEIB_LOG_EPHEMERAL  NVMEIB_LOG_LONGTERM
#define NVMEIB_LOG_ETERNAL    NVMEIB_LOG_LONGTERM
#endif
