/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DISK_HOOKS_H
#define NVMEIBC_DISK_HOOKS_H

#include "nvmeib_macro_utils.h"

/**
 * This is an interface file for disk hooks injection, that shall work both in
 * simulator and in real environment
 */

/**
 * This is the only code that differenciates between simulator and real system.
 * All the rest of the code shall be platform agnostic.
 */
#if defined(BLKDEV_SIMULATOR)
	#define get_disk_hooks(local_disk) (((struct nvmeibc_disk *)(local_disk))->disk_hooks)
#else	/* Real system only implementation */
	#define get_disk_hooks(local_disk) NULL		/* As the first stage - implemented as an empty placeholder */
#endif

/**
 * Available errors to inject
 */
enum INJECT_TRANSPORT_ERROR { // Kind of a Bitfield
	INJECT_TRANSPORT_ERROR_LOCK_ERROR = 0x1,
	INJECT_TRANSPORT_ERROR_IO_ERROR = 0x2,
	INJECT_TRANSPORT_ERROR_SEND_OWNLOCK = 200 | INJECT_TRANSPORT_ERROR_LOCK_ERROR,
	INJECT_TRANSPORT_ERROR_RELE_OWNLOCK = 300 | INJECT_TRANSPORT_ERROR_LOCK_ERROR,
	INJECT_TRANSPORT_ERROR_READ_OWNLOCK = 400 | INJECT_TRANSPORT_ERROR_LOCK_ERROR,
	INJECT_TRANSPORT_ERROR_BINFO_WR = 500,
	INJECT_TRANSPORT_ERROR_BINFO_RD = 600,
	INJECT_TRANSPORT_ERROR_GEN_CMDS = 700,
	INJECT_TRANSPORT_ERROR_IO = 800 | INJECT_TRANSPORT_ERROR_IO_ERROR,
	// Cycle of 1024 attempts (0..1023) hitting some or all of the errors above
	INJECT_TRANSPORT_ERROR_CYCLE_SIZE = 0x3FF,
};

/**
 *  Injections for simulator
 *  specifics:
 *  	enable_jentry_erase_errors
 *  	enable_lock_errors
 *  	inject_trerr_should_rand
 *  	type
 *  Can limit n_max_errors_to_inject before clearing injection -
 *  take care if used with async threads
 *  setting 'n_max_errors_to_inject' to 0 means no limit
 *  Can skip n_max_errors_to_skip before injecting errors -
 *  allows pinpointing error injection to a specific IO
 */
struct transport_err_injection {
	bool enable_jentry_erase_errors;
	bool enable_lock_errors;
	bool enable_io_errors;
	u64 n_errs_inj;
	u64 n_errs_skipped;
	enum INJECT_TRANSPORT_ERROR inject_trerr_failure_type;
	bool inject_trerr_should_rand;
	u64 n_max_errors_to_inject;
	u64 n_max_errors_to_skip;
};

/**
 * Describes the shared data structure passed to each hook.
 * A single hook may use several of the fields or not use any at all, but in total,
 * this is a complete set of data used by all hooks together.
 */
struct nvmeibc_disk_hook_args {
	struct completion wait_event;
	struct transport_err_injection trerr;

	struct iocmd_completion_args {
		struct t_ec_tx_history *hist;
	} iocmd_comp_args;
};

int inject_transport_error(struct nvmeibc_disk_hook_args *hook_args, struct nvmeibc_d_rdma_comp *comp, bool *should_return, enum INJECT_TRANSPORT_ERROR err_type);

struct nvmeibc_block_command;

struct nvmeibc_disk_hooks {
	int (*gen_cmd_completion)(struct nvmeibc_disk_hook_args *, struct nvmeibc_disk_gen_cmd *);
	int (*before_gen_cmd_comp_cb)(struct nvmeibc_disk_hook_args *, struct nvmeibc_disk_gen_cmd *);
	int (*inject_transport_error)(struct nvmeibc_disk_hook_args *, struct nvmeibc_d_rdma_comp *comp,  bool *should_return, enum INJECT_TRANSPORT_ERROR err_type);
	int (*io_cmd_completion)(struct nvmeibc_disk_hook_args *, struct nvmeibc_block_command *cmd);
	int (*on_sync_cb_stage_end)(struct nvmeibc_disk_hook_args *, struct nvmeibc_block_command *cmd);	// TODO: This shouldn't be a disk (per cmd) hook, only called when all cmds of stage complete
	struct nvmeibc_disk_hook_args args;
};


#define on_disk_hook_bindump(trace_name, disk, hook, rv, ...)                  \
	({                                                                         \
		const void * __hook_inner_param_cmd = NVMEIB_FIRSTARG(__VA_ARGS__);     \
		_NT(NVMEIB_CONCAT2(trace_name, on_disk_hook)                           \
		    , "disk_hooks disk=@DISKP name=@FUNCTION rv=@RV cmd=@ARG_PTR"      \
			, disk, NVMEIB_STRINGIFY1(hook), rv, __hook_inner_param_cmd);      \
	})

/**
 * Trap to be placed in the code.
 * Invoke specific hook on a given disk
 * @param disk Pointer to disk object
 * @param hook Name of the hook function to call
 * @param ... Additional arguments to pass to the hook
 * @note If hook returns non zero value - terminate execution flow
 */
#define on_disk_hook(trace_name, disk, hook, ...)                                                                      \
	{                                                                                                                  \
		struct nvmeibc_disk_hooks *__disk_hooks;                                                                       \
		if ((disk) && (__disk_hooks = get_disk_hooks(disk)) && __disk_hooks->hook) {                                   \
			const int __hook_rv = __disk_hooks->hook(&__disk_hooks->args, ##__VA_ARGS__);                            \
			on_disk_hook_bindump(trace_name, disk, hook, __hook_rv, __VA_ARGS__);                                      \
			if (__hook_rv) {                                                                                           \
				return;                                                                                                \
			}                                                                                                          \
		}                                                                                                              \
	}

#define on_disk_hook_return_bindump(trace_name, disk, hook, rv, should_return, ...)        \
	({                                                                                     \
		const void * __hook_inner_param_cmd = NVMEIB_FIRSTARG(__VA_ARGS__);                 \
		_NT(NVMEIB_CONCAT2(trace_name, on_disk_hook)                                       \
		    , "disk_hooks disk=@DISKP name=@FUNCTION rv=@RV should_return=@BOOL cmd=@ARG_PTR"         \
			, disk, NVMEIB_STRINGIFY1(hook), rv, should_return, __hook_inner_param_cmd);   \
	})

/**
 * Trap to be placed in the code.
 * Invoke specific hook on a given disk and possibly break disk function execution
 * by returning a value, if the hook indicated so by setting @should_return
 * @param disk Pointer to disk object
 * @param hook Name of the hook function to call
 * @param ... Additional arguments to pass to the hook
 * @note Hook must be one of those with the second argument 'bool *should_return'
 */
#define on_disk_hook_return(trace_name, disk, hook, ...)                                                               \
	{                                                                                                                  \
		struct nvmeibc_disk_hooks *__disk_hooks;                                                                       \
		if ((disk) && (__disk_hooks = get_disk_hooks(disk)) &&  __disk_hooks->hook) {                                  \
			bool should_return;                                                                                        \
			void * __hook_cmd = NVMEIB_FIRSTARG(__VA_ARGS__);            \
			int __rv__ = __disk_hooks->hook(&__disk_hooks->args, __hook_cmd, &should_return, NVMEIB_RESTARGS(__VA_ARGS__)); \
			on_disk_hook_return_bindump(trace_name, disk, hook, __rv__, should_return, __VA_ARGS__);                   \
			if (should_return)                                                                                         \
				return __rv__;                                                                                         \
		}                                                                                                              \
	}

/**
 * Setup hooks on a given server
 */
#define nvmeibc_disk_hooks_setup(disk, hooks) get_disk_hooks(disk) = hooks

/**
 * Clean hooks on a given server
 */
#define nvmeibc_disk_hooks_clean(disk) nvmeibc_disk_hooks_setup(disk, NULL)

#endif /* NVMEIBC_DISK_HOOKS_H */
