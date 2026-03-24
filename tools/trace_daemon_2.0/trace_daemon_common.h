/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef TRACE_DAEMON_COMMON_H
#define TRACE_DAEMON_COMMON_H

#include <stdio.h>
#include <string.h>
#include <assert.h>

#define MAX_FILENAME_STRLEN 1023
#define MAX_FILENAME (MAX_FILENAME_STRLEN + 1) /* buffer size including NUL */
#define MAX_CGROUP_STRLEN 119
#define MAX_CGROUP_NAME_SIZE (MAX_CGROUP_STRLEN + 1) /* buffer size including NUL */

typedef struct trace_channel_cfg {
	char *name;
	int buf_per_log;
	int max_logs;
	struct trace_channel_cfg *next;
} trace_channel_cfg_t;

typedef struct trace_daemon_cfg {
	int debug_lvl;
	int buf_per_log;
	int max_logs;
	int compress;
	char logs_path[MAX_FILENAME];
	trace_channel_cfg_t *channel_cfg;
	int nfs;
	int ramfs;
	char cgroup[MAX_CGROUP_NAME_SIZE];
} trace_daemon_cfg_t;


//Pay attention - those are compiled time defaults, they don't take into account the number of CPU's.
//Thus if the system has more then 63 CPU we have a problem - no history.
//The correct solution would be merging:
//	* compile time defaults
//	* run time defaults
//	* user settings
//As always, we have limited time and resources, so the trace_channel calculates the final settings on start
//Also it looks like max_logs definition as "total number of log files" is problematic. 
//I think we should have "max_logs_per_channel". Unfortunatelly, we cannot change this easily - backward compaibility.

#define INIT_TRACE_CFG                                                                                   \
	(trace_daemon_cfg_t)                                                                             \
	{                                                                                                \
		.debug_lvl = 2, .buf_per_log = 4096, .max_logs = 64, .compress = 0, .channel_cfg = NULL, \
		.logs_path = { 0 }, .nfs = 0, .ramfs = 0, .cgroup = { 0 }                                \
	}

extern trace_daemon_cfg_t trace_cfg;

#define __DEBUG_LEVEL_SRC trace_cfg.debug_lvl
#include "trace_daemon_dbg.h"

static inline trace_channel_cfg_t *get_trace_channel_cfg(trace_daemon_cfg_t *cfg, const char *name, int create) {
	trace_channel_cfg_t *iter = cfg->channel_cfg;
	while (iter) {
		if (!strcmp(iter->name, name))
			return iter;
		iter = iter->next;
	}
	if (create) {
		iter = calloc(1, sizeof(trace_channel_cfg_t));
		iter->name = strdup(name);
		iter->max_logs = cfg->max_logs;
		iter->buf_per_log = cfg->buf_per_log;
		iter->next = cfg->channel_cfg;
		cfg->channel_cfg = iter;
	}
	return iter;
}

static inline void clean_channel_cfg(trace_daemon_cfg_t *cfg) {
	while (cfg->channel_cfg) {
		trace_channel_cfg_t *iter = cfg->channel_cfg;
		if (iter->name) free(iter->name);
		cfg->channel_cfg = iter->next;
		free(iter);
	}
	*cfg = INIT_TRACE_CFG;
}

static inline int get_buf_per_log(const char *name) {
	trace_channel_cfg_t *cfg = get_trace_channel_cfg(&trace_cfg, name, 0);
	return cfg ? cfg->buf_per_log : trace_cfg.buf_per_log;
}

static inline int get_max_logs(const char *name) {
	trace_channel_cfg_t *cfg = get_trace_channel_cfg(&trace_cfg, name, 0);
	return cfg ? cfg->max_logs : trace_cfg.max_logs;
}

#include "../../common/nvmeib_math.h"

#define syscall_or_nfs_syscall(self, __syscall, ...) ({ \
	int _rv = 0; \
	do { \
		if (trace_cfg.nfs) {\
			do {\
				_rv = __syscall(__VA_ARGS__);\
				_info(#__syscall " retrun code = %d errno = %d", _rv, errno);\
			} while (_rv == -1 && errno == EIO);\
		} \
		else { \
			_rv = __syscall(__VA_ARGS__);\
		} \
	} while (0);\
	 _rv; \
	})

#endif /*TRACE_DAEMON_COMMON_H*/
