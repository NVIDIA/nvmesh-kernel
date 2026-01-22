/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef _NVMEIBT_RECOVERY_H_
#define _NVMEIBT_RECOVERY_H_

#include "toma/nvmeibt_disk_segment.h"
#include "toma/clnt/nvmeibt_client_protocol.h"
#include "toma/nvmeibt_wq.h"

enum NVMEIBT_RECOVERY_STATUS {
	NVMEIBT_RECOVERY_STATUS_SUCCESS		= 1,
	NVMEIBT_RECOVERY_STATUS_ABORT		= 2,
	NVMEIBT_RECOVERY_STATUS_ERROR		= 3,
};

enum ENCRYPT_DELAY_E {
	ENCRYPT_DELAY_NONE					= 0,
	ENCRYPT_DELAY_BEFORE_EXECUTION		= 1,
	ENCRYPT_DELAY_AFTER_EXECUTION       = 2,
	ENCRYPT_DELAY_BEFORE_DETACH         = 3,
	ENCRYPT_DELAY_BEFORE_COMMIT         = 4
};

static inline const char *nvmeibt_recovery_type_to_str(enum NVMEIBT_RECOVERY_TYPE type)
{
	switch (type) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:		return "DIRTY_REBUILD";
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:		return "STALE_REBUILD";
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:		return "TXID_REBUILD";
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:  			return "EC_COLD_REBUILD";
	case NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON:	return "EC_DCONVICT_TURNON";
	case NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE:	return "STALE_LOCKS_PURGE";
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:			return "EC_JGC";
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:			return "SCRUBBING";
	default:
		N_Ef(t_a1_toma_rcv, "Unknown type @X", type);
		nvmeibt_abort(ES_FATAL);
		return NULL;
	}
}

static inline const char *nvmeibt_recovery_status_to_str(enum NVMEIBT_RECOVERY_STATUS status)
{
	switch (status) {
	case NVMEIBT_RECOVERY_STATUS_SUCCESS:	return "SUCCESS";
	case NVMEIBT_RECOVERY_STATUS_ABORT:		return "ABORT";
	case NVMEIBT_RECOVERY_STATUS_ERROR:		return "ERROR";
	default:
		N_Ef(t_a2_toma_rcv, "Unknown status @X", status);
		nvmeibt_abort(ES_FATAL);
		return NULL;
	}
}

struct nvmeibt_recoveries_progress_wq_entry {
	struct nvmeibt_wq_entry 	wq_entry;
	struct nvmeibt_Str			*th_infos_to_print;
};

struct nvmeibt_registrant_ctx;
struct nvmeibt_local_disk;
struct nvmeibt_register_msg;

u64 nvmeibt_recovery_start_rebuild(enum NVMEIBT_RECOVERY_TYPE task_type, struct nvmeibt_seg_active *seg_active);

void nvmeibt_recovery_set_max_n_simultaneous_dirty_rebuild(int64_t max_n_simultaneous_dirty_rebuild);
int64_t nvmeibt_recovery_get_max_n_simultaneous_dirty_rebuild(void);
void nvmeibt_recovery_set_max_n_simultaneous_stale_and_txid_rebuild(int64_t max_n_simultaneous_rebuild);
int64_t nvmeibt_recovery_get_max_n_simultaneous_stale_and_txid_rebuild(void);
void nvmeibt_recovery_set_max_n_simultaneous_scrubbing(int64_t max_n_simultaneous_scrubbing);
int64_t nvmeibt_recovery_get_max_n_simultaneous_scrubbing(void);

BOOL nvmeibt_recovery_is_any_recovery_active(void);
BOOL nvmeibt_recovery_launch_abort_all_recoveries_on_local_disk(struct nvmeibt_local_disk *local_disk);
bool nvmeibt_recovery_is_aborting_rebuild(u64 tid);
void nvmeibt_recovery_launch_abort_rebuild(u64 tid);

void nvmeibt_recovery_handle_client_registered(struct nvmeibt_registrant_ctx *ctx);
void nvmeibt_recovery_handle_client_unregistered(struct nvmeibt_registrant_ctx *ctx);

int nvmeibt_recovery_handle_incoming_msg(struct nvmeibt_register_msg *reg_msg);

int nvmeibt_recovery_timeout_occurred(void);
void nvmeibt_recovery_notify_all_dirty_rebuilds_of_params_change(void);
struct timespec nvmeibt_recovery_get_next_timeout_timespec(void);

void nvmeibt_run_exec_on_blkdev(struct run_exec_on_blkdev_ctx *ctx);
void nvmeibt_attach_vol_for_encryption(struct nvmeibt_block_device *vol, char *shadow_vol_name, struct nvmeibt_encrypt_params *encrypt_params);
void nvmeibt_detach_vol_for_encryption(struct run_exec_on_blkdev_ctx *exec_ctx);
void nvmeibt_attach_detach_shadow_vol(char *origin_vol_name, bool is_attach, struct nvmeibt_Str *out);

int nvmeibt_recovery_print_status(
		int (*printf_fn)(void *ctx, const char *fmt, ...),
		void *printf_ctx);

int nvmeibt_recovery_init(void);
void nvmeibt_recovery_exit(void);

void nvmeibt_recovery_set_endless_rebuild_state(bool is_endless);
void nvmeibt_recovery_report_rebuild_progress_to_mgmt(void);

#endif /* _NVMEIBT_RECOVERY_H_ */

