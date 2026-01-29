/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_ERROR_REPORT_H
#define NVMEIBC_ERROR_REPORT_H

#include "nvmeib_utils_bin_traces.h"

#define NUM_DBGDUMP_PER_CALL 10

static inline void NO_REPORT(void *obj, int trace_level)
{
	(void)obj;
	(void)trace_level;
}

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR == 1) || defined(NVMESH_SIMULATOR)
extern bool debug_dump_funcs;
#define NVMESH_WARN(condition, do_report, op_object, frmt, ...)                                    \
	({                                                                                         \
		static unsigned n_skip_start = 0;                                                  \
		static unsigned counter = 0;                                                       \
		static unsigned n_skip_count = 0;                                                  \
		bool dumped = false;                                                               \
		if (!n_skip_start) {                                                               \
			n_skip_start = (rand() % 31) + 1;                                          \
			n_skip_count = n_skip_start;                                               \
		}                                                                                  \
		if (debug_dump_funcs && counter++ < NUM_DBGDUMP_PER_CALL && --n_skip_count == 0) { \
			dumped = true;                                                             \
			n_skip_count = n_skip_start;                                               \
			do_report(op_object, T_INFO);                                              \
		}                                                                                  \
		if (condition) {                                                                   \
			if (!dumped) {                                                             \
				do_report(op_object, T_WARN);                                      \
			}                                                                          \
			WARN(true, "condition: " #condition " " frmt, ##__VA_ARGS__);              \
		}                                                                                  \
		if (!n_skip_count)                                                                 \
			n_skip_count = n_skip_start;                                               \
	})

#define NVMESH_BUG(condition, do_report, op_object, frmt, ...)                                     \
	({                                                                                         \
		static unsigned n_skip_start = 0;                                                  \
		static unsigned counter = 0;                                                       \
		static unsigned n_skip_count = 0;                                                  \
		bool dumped = false;                                                               \
		if (!n_skip_start) {                                                               \
			n_skip_start = (rand() % 31) + 1;                                          \
			n_skip_count = n_skip_start;                                               \
		}                                                                                  \
		if (debug_dump_funcs && counter++ < NUM_DBGDUMP_PER_CALL && --n_skip_count == 0) { \
			dumped = true;                                                             \
			n_skip_count = n_skip_start;                                               \
			do_report(op_object, T_INFO);                                              \
		}                                                                                  \
		if (condition) {                                                                   \
			WARN(true, "condition: " #condition " " frmt, ##__VA_ARGS__);              \
			if (!dumped) {                                                             \
				do_report(op_object, T_BUG);                                       \
			}                                                                          \
			BUG();                                                                     \
		}                                                                                  \
		if (!n_skip_count)                                                                 \
			n_skip_count = n_skip_start;                                               \
	})
#else
#define NVMESH_WARN(condition, do_report, op_object, frmt, ...)                       \
	({                                                                            \
		if (condition) {                                                      \
			WARN(true, "condition: " #condition " " frmt, ##__VA_ARGS__); \
			do_report(op_object, T_WARN);                                 \
		}                                                                     \
	})

#define NVMESH_BUG(condition, do_report, op_object, frmt, ...)                        \
	({                                                                            \
		if (condition) {                                                      \
			WARN(true, "condition: " #condition " " frmt, ##__VA_ARGS__); \
			do_report(op_object, T_BUG);                                  \
			BUG();                                                        \
		}                                                                     \
	})

#endif // BLKDEV_SIMULATOR
#endif // NVMEIBC_ERROR_REPORT_H
