/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MAIN_BLOCK_GEN_WORK_SCHED_H
#define NVMEIBC_MAIN_BLOCK_GEN_WORK_SCHED_H

/******************************************************************************/
/* Dispatcher of block device tasks on main-wq. Dispatches the tasks to relevant
   software layer, maintaining encapsulation:
   1. attach detach actions
   2. Various counters verses transport layer */
#include "module/instance/nvmeibc_cinst_params.h"

/* Generic callback types:
    1. Block layer issues work for main layer
    2. Block layer issues work for itself
    3. Todo: Issue work to core layer...

	Generic callback type params:
	1. context of client instance. Used by main layer, can be used optionally by block layer
	2. generic context for block layer for self usage
*/
typedef void (*blk2main_gen_work_t)(void* context, const struct nvmeibc_cinst_params_main *p);
typedef void (*blk2blk_gen_work_t)( void* context);

/* Schedule work handler(context) for block device. 'owner' is used only to
   verify that detach did not remove the volume before the callback runs
   should_free will allow freeing context even if handling a case with a detached owner*/
void nvmeibc_block_set_generic_work_to_main(const struct nvmeibc_cinst_params_blk *p,
		const char *block_uuid, /* Unique identifier of the block device for a given instance */
		blk2main_gen_work_t fn, void *context, bool should_free_context);

void nvmeibc_block_set_generic_work_to_self(const struct nvmeibc_cinst_params_blk *p,
		const char *block_uuid, /* Unique identifier of the block device for a given instance */
		blk2blk_gen_work_t fn,  void *context, bool should_free_context);

/******************************************************************************/
// Transport layer dispatcher Unsorted methods, todo: Fix them
typedef int (*nvmeibc_main_wq_fn_type)(void *param);
#define nvmeibc_assert_on_main_wq(p) ({ BUG_ON(!nvmeibc_is_on_main_wq(p, true)); })
bool nvmeibc_is_on_main_wq( const struct nvmeibc_cinst_params_main *p, bool do_assert);
int nvmeibc_add_work(       const struct nvmeibc_cinst_params_main *p, struct workqe_struct *work);
bool nvmeibc_cancel_work(const struct nvmeibc_cinst_params_main *p, struct workqe_struct *work);
int nvmeibc_run_on_main_wq( const struct nvmeibc_cinst_params_main *p, nvmeibc_main_wq_fn_type fn, void *param, bool drain_wq, bool can_sleep, int *p_sts);
#define nvmeibc_run_on_main_wq1(                                    p,                         fn,       param) nvmeibc_run_on_main_wq(p,fn,param,false,false,NULL)
/******************************************************************************/

// Changing context between layers
const struct nvmeibc_cinst_params_blk  *nvmeibc_isnt_params_main2blk( const struct nvmeibc_cinst_params_main* p);
const struct nvmeibc_cinst_params_main *nvmeibc_isnt_params_blk2main( const struct nvmeibc_cinst_params_blk * p);
const struct nvmeibc_cinst_params_core *nvmeibc_isnt_params_main2core(const struct nvmeibc_cinst_params_main* p);
const struct nvmeibc_cinst_params_main *nvmeibc_isnt_params_core2main(const struct nvmeibc_cinst_params_core* p);
const struct nvmeibc_cinst_params_core *nvmeibc_isnt_params_blk2core( const struct nvmeibc_cinst_params_blk * p);
const struct nvmeibc_cinst_params_blk  *nvmeibc_isnt_params_core2blk( const struct nvmeibc_cinst_params_core* p);

// Get attribute of core from blk
int nvmeibc_cinst_params_blk_get_cinst_params_core_tcp_mode(const struct nvmeibc_cinst_params_blk * p);

#endif

