/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_SERJIO_STATS_H
#define NVMEIBS_SERJIO_STATS_H

#include "nvmeib_shared.h"

enum nvmeibs_serjio_work_type {
	NVMEIBS_SERJIO_WORK_TYPE_ALLOC_RNG = 0,
	NVMEIBS_SERJIO_WORK_TYPE_RET_RNG,
	NVMEIBS_SERJIO_WORK_TYPE_CHK_JGC,
	NVMEIBS_SERJIO_WORK_TYPE_RD_GPT,
	NVMEIBS_SERJIO_WORK_TYPE_RD_DB,
	NVMEIBS_SERJIO_WORK_TYPE_CLN_JRNL_DISK_RNG,
	NVMEIBS_SERJIO_WORK_TYPE_CLN_JRNL_DISK_RNG_START,
	NVMEIBS_SERJIO_WORK_TYPE_CALL_ASSGN_RNG,
	NVMEIBS_SERJIO_WORK_TYPE_CALL_RNG,
	NVMEIBS_SERJIO_WORK_TYPE_FREE_JRNL_ENTS,	
	NVMEIBS_SERJIO_WORK_TYPE_ABND_ENTS,
	NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD,
	NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD_DONE,	
	NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD_CANCEL,
	NVMEIBS_SERJIO_WORK_TYPE_INIT_JRNL,
	NVMEIBS_SERJIO_WORK_TYPE_RD_JRNL,
	NVMEIBS_SERJIO_WORK_TYPE_READY,
	NVMEIBS_SERJIO_WORK_TYPE_MAX,
};


struct nvmeibs_serjio_min_max_avg_stats {
    u64 _min;
    u64 _max;
    u64 _sum;
	u64 overall_count;
    u64 current_count;
};

struct nvmeibs_serjio_op_rsrc_stats {
	ktime_t t_start;
	enum nvmeibs_serjio_work_type work_type;
};

struct nvmeibs_serjio_work_type_io_stats {
	u64 read_submitted_bytes;
	u64 read_submitted_lbas;
	
	u64 write_submitted_bytes;
	u64 write_submitted_lbas;

	u64 submitted_read_iops;
	u64 submitted_write_iops;

	u64 read_completed_bytes;
	u64 read_completed_lbas;

	u64 write_completed_bytes;
	u64 write_completed_lbas;

	u64 completed_read_iops;
	u64 completed_write_iops;

	u64 n_submit_read_errors;
	u64 n_submit_write_errors;

	u64 n_complete_read_errors;
	u64 n_complete_write_errors;

	ktime_t t_start;

	ktime_t accumulated_io_duration; //in ktime (ns)
};

struct nvmeibs_serjio_io_stats {
	struct nvmeibs_serjio_work_type_io_stats current_work_stats;

	struct nvmeibs_serjio_min_max_avg_stats read_submitted_bytes;
	struct nvmeibs_serjio_min_max_avg_stats read_submitted_lbas;

	struct nvmeibs_serjio_min_max_avg_stats write_submitted_bytes;
	struct nvmeibs_serjio_min_max_avg_stats write_submitted_lbas;

	struct nvmeibs_serjio_min_max_avg_stats read_completed_bytes;
	struct nvmeibs_serjio_min_max_avg_stats read_completed_lbas;
	struct nvmeibs_serjio_min_max_avg_stats read_bw;
	struct nvmeibs_serjio_min_max_avg_stats read_iops;

	struct nvmeibs_serjio_min_max_avg_stats write_completed_bytes;
	struct nvmeibs_serjio_min_max_avg_stats write_completed_lbas;
	struct nvmeibs_serjio_min_max_avg_stats write_bw;
	struct nvmeibs_serjio_min_max_avg_stats write_iops;

	struct nvmeibs_serjio_min_max_avg_stats n_submit_read_errors;
	struct nvmeibs_serjio_min_max_avg_stats n_submit_write_errors;
	
	struct nvmeibs_serjio_min_max_avg_stats n_read_errors;
	struct nvmeibs_serjio_min_max_avg_stats error_read_bytes;
	struct nvmeibs_serjio_min_max_avg_stats error_read_lbas;

	struct nvmeibs_serjio_min_max_avg_stats n_write_errors;
	struct nvmeibs_serjio_min_max_avg_stats error_write_bytes;
	struct nvmeibs_serjio_min_max_avg_stats error_write_lbas;

	struct nvmeibs_serjio_min_max_avg_stats io_duration; //in ktime (ns)
};

struct nvmeibs_serjio_work_stats {
	ktime_t t_queued;
	ktime_t t_exec_start;
    ktime_t t_exec_end;
    u64 n_rescheds;
    bool success;
    enum nvmeibs_serjio_work_type work_type;
};

struct nvmeibs_serjio_works_stats_total {
	u64 n_works;
	u64 n_errors;
	u64 n_rescheds;
	struct nvmeibs_serjio_min_max_avg_stats works_queued; //in ktime (ns)
	struct nvmeibs_serjio_min_max_avg_stats works_exec_time; //in ktime (ns)
};


struct nvmeibs_serjio_state_stats {
    ktime_t t_start;
    struct nvmeibs_serjio_min_max_avg_stats state_stats; //in ktime (ns)
};
struct nvmeibs_serjio_stats {
	spinlock_t lock;
	u64 submitted_bytes;
	u64 submitted_lbas;
	struct nvmeibs_serjio_io_stats io_stats[NVMEIBS_SERJIO_WORK_TYPE_MAX];
	struct nvmeibs_serjio_works_stats_total work_stats_by_type[NVMEIBS_SERJIO_WORK_TYPE_MAX]; //in ktime (ns)
	struct nvmeibs_serjio_state_stats states_stats[MAX_SERJIO_STATE]; //in ktime (ns)
};

void nvmeibs_serjio_update_work_stats_queued(struct nvmeibs_serjio_work_stats *work_stats, struct nvmeibs_serjio_stats *serjio_stats);
void nvmeibs_serjio_update_work_stats_exec(struct nvmeibs_serjio_work_stats *work_stats, struct nvmeibs_serjio_stats *serjio_stats);
void nvmeibs_serjio_update_work_stats_resched(struct nvmeibs_serjio_work_stats *work_stats);
void nvmeibs_serjio_update_work_stats_completed(struct nvmeibs_serjio_work_stats *work_stats, struct nvmeibs_serjio_stats *serjio_stats, int rv);
void nvmeibs_serjio_state_init(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_state state);
void nvmeibs_serjio_on_state_change(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_state old_state, enum nvmeibs_serjio_state new_state);
void nvmeibs_serjio_stats_init(struct nvmeibs_serjio_stats *serjio_stats);
void nvmeibs_serjio_work_type_submit_io_success(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_work_type work_type, enum nvme_opcode op_code, u64 submitted_bytes, u64 block_size);
void nvmeibs_serjio_work_type_submit_io_fail(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_work_type work_type, enum nvme_opcode op_code);

struct seq_file;
int nvmeibs_serjio_show_stats_json(struct nvmeibs_serjio_stats *serjio_stats, struct seq_file *m);
ssize_t nvmeibs_serjio_fill_serjio_stats_readable(struct nvmeibs_serjio_stats *serjio_stats, char *buffer, size_t len);
void nvmeibs_serjio_stats_clear(struct nvmeibs_serjio_stats *serjio_stats);
void nvmeibs_serjio_update_io_stats_on_rsrc_completion(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_work_type work_type,
                                            size_t completed_bytes, size_t completed_lbas, int completed_iops, bool is_read, bool is_error,
                                            struct nvmeibs_serjio_op_rsrc_stats *op_rsrc_stats);
#endif