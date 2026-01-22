/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_COMMON_ALL_H
#define NVMEIB_COMMON_ALL_H
/* First injection point which should be included by all simulators. Replaces
   the most lower layer includes (kernel, ib & rdma verbs, nvmeib,
   nvmeibc_common, nvmeib_public, etc.
*/

#include "block/unitest/kr_incs.h"					// Including kernel libraries or kernel simulator from local directory instead of common/...
#define NVMEIB_WORKQ								// Mark as if we are using Ofers implementation of workqueues
#include "nvmeib_common_os_block_api.h"
#define KR_UNDEF_H									//#include "kr_undef.h
/******************************* NVME Simulator ******************************/
#include "nvmeib_nvme.h"
#include "nvmeib_shared.h"

#define CONFIG_NVMEIB_DEBUG
int nvmeib_debug_level(void);
int nvmeib_public_debug_level(void);
#include "nvmeib_utils.h"							// Allow debug printing to log
#undef CONFIG_NVMEIB_DEBUG

#define _Emerg(fmt, ...) pr_emerg("(%d)%s[%s](%d): " fmt, current->pid, FILENAME, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#include "nvmeibs_types.h"

//#define GFP_KERNEL 0
//#define __GFP_ZERO 0

#include "nvmeib_public.h"
#include "nvmeibc_types.h"
#include "nvmeib_wd.h"
#include "nvmeibc_ib_admin_channel.h"
#define NVMEIBC_NVME_H								// #include "nvmeibc_nvme.h"
#define NVMEIBS_CLIENT_H							// srv/nvmeibs_client.h
#include "nvmeibs_nvme.h"
#include "nvmeibc_stats.h"
#define NVMEIB_Q_H									// Prevent inclusion of common/nvmeib_q.h, already implemented in kernel simulator
#define NVMEIB_Q_LOG_LEVEL_VERBOSE 4

/*************************** Utils for all simulators *************************/
#define BUG_NOT_IMPLEMENTED_YET 	BUG()					// Missing compenent in the simulator
#define unitest_print(...) pr_crit(__VA_ARGS__)				// Print is always visible, regardless of debug and printk() log visibility

#define do_once(expression) ({ \
		static bool __section(.data.unlikely) __already_did = false; \
		if (!__already_did) { /* Daniel: Add cmpxchg? */\
			__already_did = true; \
			expression; \
		} \
	})

/* Simulators of user space script add reader cb to accept what kernel code
   writes. As if user space does 'cat /proc/...', SELECT, etc.
   Returns msg-loop which is listened as 'file' or NULL if not found */
void* msgloop_proc_inject_reader_cb(const char *proc_dir_path, const char* msgloop_name, void* arg,
                                    int (*cb)(void* arg, const char* buf, size_t len));
u64 nvmeib_get_guid(void);

/************************** nvme_public module Simulator **********************/
#include "kth/nvmeib_public_kth.h"
void nvmeib_public_init(void);
void nvmeib_public_module_exit(void);

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags, int max_active);
void destroy_workqueue(struct workqueue_struct *wq);
void flush_workqueue(struct workqueue_struct *wq);
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[]);
#include "nvmeib_public_keeper.h"
#endif  // H beginning
