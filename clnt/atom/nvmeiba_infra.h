/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBA_INFRA_H
#define NVMEIBA_INFRA_H
/* Must not include anything from the rest of NVMesh codebase! */
#if defined(BLKDEV_SIMULATOR)
	#include "block/unitest/kr_incs.h"
	#include "nvmeib_common_os_block_api.h" // Only .h can be included without any implementation
#else
	#include <linux/types.h>
	#include <linux/list.h>
	#include <linux/vmalloc.h>
	#include <linux/atomic.h>
	#include <linux/bio.h>
	#include <linux/blkdev.h>
	#include <linux/proc_fs.h>
	#include <linux/rtc.h>
	#include <linux/module.h>				// For MODULE_AUTHOR()
	#include "nvmeib_common_os_block_api.h" // Only .h can be included without any implementation
	#include "common/compat/kr_incs_module.h"
	#ifdef KR_INCS
		#error "Dont do include any other nvmesh module (common/kr_incs.h)"
	#endif
#endif

// Miniature version of nvmeibc_procfs.c/h (Readonly small proc files)
typedef ssize_t nvmeiba_proc_read_cb(void *ctx, char *buf, size_t len);
void *nvmeiba_proc_create(char *name, struct proc_dir_entry *dir, nvmeiba_proc_read_cb *read, void *arg);
void nvmeiba_proc_remove(void *p);

#define A_DMESG_PREFIX "NVMesh-A: "
bool is_verbose_mode(void);
//#define _ND(        t_id_dp_dbg_tools, fmt, ...) ({ if (is_verbose_mode()) pr_debug("(%d)%s[%s](%d): " A_DMESG_PREFIX fmt, current->pid, kbasename(__FILE__), __FUNCTION__, __LINE__, ## __VA_ARGS__); })
#define _NT(        t_id_dp_dbg_tools, fmt, ...) ({ if (is_verbose_mode()) pr_info( "(%d)%s[%s](%d): " A_DMESG_PREFIX fmt, current->pid, kbasename(__FILE__), __FUNCTION__, __LINE__, ## __VA_ARGS__); })
#define _NI_to_user(t_id_dp_dbg_tools, fmt, ...)                           pr_info( "(%d)%s[%s](%d): " A_DMESG_PREFIX fmt, current->pid, kbasename(__FILE__), __FUNCTION__, __LINE__, ## __VA_ARGS__)
#define _NE_to_user(t_id_dp_dbg_tools, fmt, ...)                           pr_err(  "(%d)%s[%s](%d): " A_DMESG_PREFIX fmt, current->pid, kbasename(__FILE__), __FUNCTION__, __LINE__, ## __VA_ARGS__)
#endif
