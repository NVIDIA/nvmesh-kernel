/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibs_serjio_stats.h"
#include "nvmeibs_serjio.h"
#include "nvmeib_jdr.h"
#include "proc_epilog.h"


inline static void increase_work_count(struct nvmeibs_serjio_min_max_avg_stats *stats)
{
    stats->current_count++;
    stats->overall_count++;
}

inline static void decrease_work_count(struct nvmeibs_serjio_min_max_avg_stats *stats)
{
    stats->current_count--;
}

inline static void update_new_value(struct nvmeibs_serjio_min_max_avg_stats *stats, u64 value)
{
    // it is assumed that overall_count is already ++ed before calling this function
    //stats->_avg = ((stats->_avg * (stats->overall_count - 1)) + value) / (stats->overall_count);
    stats->_sum += value;
    stats->_min = min(stats->_min, value);
    stats->_max = max(stats->_max, value);
    decrease_work_count(stats);
}

static void update_new_value_inc(struct nvmeibs_serjio_min_max_avg_stats *stats, u64 value)
{
    stats->current_count++;
    stats->overall_count++;
    update_new_value(stats, value);
}

static void update_work_type_stats_queued_start(struct nvmeibs_serjio_stats *stats, enum nvmeibs_serjio_work_type work_type)
{
    unsigned long flags = 0;
    spin_lock_irqsave(&stats->lock, flags);
    increase_work_count(&stats->work_stats_by_type[work_type].works_queued);
    spin_unlock_irqrestore(&stats->lock, flags);
}

static void update_work_type_stats_exec_start(struct nvmeibs_serjio_stats *stats, enum nvmeibs_serjio_work_type work_type)
{
    unsigned long flags = 0;
    spin_lock_irqsave(&stats->lock, flags);
    stats->io_stats[work_type].current_work_stats = (struct nvmeibs_serjio_work_type_io_stats){0};
    stats->io_stats[work_type].current_work_stats.t_start = ktime_get();

    increase_work_count(&stats->work_stats_by_type[work_type].works_exec_time);
    spin_unlock_irqrestore(&stats->lock, flags);
}


static void update_work_type_stats_queued_end(struct nvmeibs_serjio_stats *stats, enum nvmeibs_serjio_work_type work_type, u64 time_queued)
{
    unsigned long flags = 0;
    spin_lock_irqsave(&stats->lock, flags);
    update_new_value(&stats->work_stats_by_type[work_type].works_queued, time_queued);
    spin_unlock_irqrestore(&stats->lock, flags);
}

#define CALC_PER_SEC_RATE(capacity, duration) ((capacity * NSEC_PER_SEC) / ktime_to_ns(duration))

static void update_work_io_stats(struct nvmeibs_serjio_stats *stats, enum nvmeibs_serjio_work_type work_type, ktime_t io_duration)
{
    unsigned long flags = 0;
    struct nvmeibs_serjio_io_stats *io_stats = &stats->io_stats[work_type];
    if (io_duration == 0) {
        goto out;
    }
    spin_lock_irqsave(&stats->lock, flags);

    if (io_stats->current_work_stats.submitted_read_iops > 0) {
        //read submitted stats
        update_new_value_inc(&io_stats->read_submitted_bytes, io_stats->current_work_stats.read_submitted_bytes);
        update_new_value_inc(&io_stats->read_submitted_lbas, io_stats->current_work_stats.read_submitted_lbas);

        //read completed stats
        update_new_value_inc(&io_stats->read_completed_bytes, io_stats->current_work_stats.read_completed_bytes);
        update_new_value_inc(&io_stats->read_completed_lbas, io_stats->current_work_stats.read_completed_lbas);
        update_new_value_inc(&io_stats->read_bw, CALC_PER_SEC_RATE(io_stats->current_work_stats.read_completed_bytes, io_duration));
        update_new_value_inc(&io_stats->read_iops, CALC_PER_SEC_RATE(io_stats->current_work_stats.completed_read_iops, io_duration));

        //read errors stats
        if (io_stats->current_work_stats.n_complete_read_errors > 0) {
            update_new_value_inc(&io_stats->n_read_errors, io_stats->current_work_stats.n_complete_read_errors);
        }
        if (io_stats->current_work_stats.n_submit_read_errors > 0) {
            update_new_value_inc(&io_stats->n_submit_read_errors, io_stats->current_work_stats.n_submit_read_errors);
        }
    }
    if (io_stats->current_work_stats.submitted_write_iops > 0) {
        //write submitted stats
        update_new_value_inc(&io_stats->write_submitted_bytes, io_stats->current_work_stats.write_submitted_bytes);
        update_new_value_inc(&io_stats->write_submitted_lbas, io_stats->current_work_stats.write_submitted_lbas);
        
        //write completed stats
        update_new_value_inc(&io_stats->write_completed_bytes, io_stats->current_work_stats.write_completed_bytes);
        update_new_value_inc(&io_stats->write_completed_lbas, io_stats->current_work_stats.write_completed_lbas);
        update_new_value_inc(&io_stats->write_bw, CALC_PER_SEC_RATE(io_stats->current_work_stats.write_completed_bytes, io_duration));
        update_new_value_inc(&io_stats->write_iops, CALC_PER_SEC_RATE(io_stats->current_work_stats.completed_write_iops, io_duration));

        //write errors stats
        if (io_stats->current_work_stats.n_complete_write_errors > 0) {
            update_new_value_inc(&io_stats->n_write_errors, io_stats->current_work_stats.n_complete_write_errors);
        }
        if (io_stats->current_work_stats.n_submit_write_errors > 0) {
            update_new_value_inc(&io_stats->n_submit_write_errors, io_stats->current_work_stats.n_submit_write_errors);
        }
    }

    update_new_value_inc(&io_stats->io_duration, io_duration);

    spin_unlock_irqrestore(&stats->lock, flags);
out:
    return;
}

static void update_work_type_stats_completed(struct nvmeibs_serjio_stats *stats, enum nvmeibs_serjio_work_type work_type, ktime_t time_to_run, ktime_t time_queued, ktime_t io_duration, int rv, int n_rescheds)
{
    unsigned long flags = 0;
    update_work_type_stats_queued_end(stats, work_type, time_queued);
 
    update_work_io_stats(stats, work_type, io_duration);


    spin_lock_irqsave(&stats->lock, flags);
    stats->work_stats_by_type[work_type].n_works++;
    if (rv != 0) {
        stats->work_stats_by_type[work_type].n_errors++;
    }
    stats->work_stats_by_type[work_type].n_rescheds += n_rescheds;
    update_new_value(&stats->work_stats_by_type[work_type].works_exec_time, time_to_run);
    spin_unlock_irqrestore(&stats->lock, flags);
   
}

void nvmeibs_serjio_update_work_stats_completed(struct nvmeibs_serjio_work_stats *work_stats, struct nvmeibs_serjio_stats *serjio_stats, int rv)
{
    u64 time_to_run = 0;
    u64 time_queued = 0;
    u64 io_duration = 0;

    work_stats->t_exec_end = ktime_get();
    work_stats->success = (rv == 0);

    BUG_ON(work_stats->t_queued == 0);
    BUG_ON(work_stats->t_exec_start == 0);

    time_to_run = ktime_sub(work_stats->t_exec_end, work_stats->t_exec_start);
    time_queued = ktime_sub(work_stats->t_exec_start, work_stats->t_queued);

    io_duration = serjio_stats->io_stats[work_stats->work_type].current_work_stats.accumulated_io_duration;

    update_work_type_stats_completed(serjio_stats, work_stats->work_type, time_to_run, time_queued, io_duration, rv, work_stats->n_rescheds);
}

void nvmeibs_serjio_update_work_stats_queued(struct nvmeibs_serjio_work_stats *work_stats, struct nvmeibs_serjio_stats *serjio_stats)
{
    work_stats->t_queued = ktime_get();
    update_work_type_stats_queued_start(serjio_stats, work_stats->work_type);
}

void nvmeibs_serjio_update_work_stats_exec(struct nvmeibs_serjio_work_stats *work_stats, struct nvmeibs_serjio_stats *serjio_stats)
{
    work_stats->t_exec_start = ktime_get();
    update_work_type_stats_exec_start(serjio_stats, work_stats->work_type);
}

void nvmeibs_serjio_update_work_stats_resched(struct nvmeibs_serjio_work_stats *work_stats)
{
    work_stats->n_rescheds++;
    work_stats->t_exec_start = 0;
}

void nvmeibs_serjio_state_init(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_state state)
{
    serjio_stats->states_stats[state].t_start = ktime_get();
    increase_work_count(&serjio_stats->states_stats[state].state_stats);
}

void nvmeibs_serjio_stats_clear(struct nvmeibs_serjio_stats *serjio_stats)
{
    int i;
    unsigned long flags = 0;
    spin_lock_irqsave(&serjio_stats->lock, flags);
    serjio_stats->submitted_bytes = 0;
    serjio_stats->submitted_lbas = 0;

    memset(serjio_stats->work_stats_by_type, 0, sizeof(serjio_stats->work_stats_by_type));
    memset(serjio_stats->io_stats, 0, sizeof(serjio_stats->io_stats));
    memset(serjio_stats->states_stats, 0, sizeof(serjio_stats->states_stats));

    for (i = 0; i < MAX_SERJIO_STATE; i++) {
        serjio_stats->states_stats[i].state_stats._min = U64_MAX;
    }
    for (i = 0; i < NVMEIBS_SERJIO_WORK_TYPE_MAX; i++) {
        serjio_stats->work_stats_by_type[i].works_queued._min = U64_MAX;
        serjio_stats->work_stats_by_type[i].works_exec_time._min = U64_MAX;
        
        serjio_stats->io_stats[i].read_submitted_bytes._min = U64_MAX;
        serjio_stats->io_stats[i].read_submitted_lbas._min = U64_MAX;
        serjio_stats->io_stats[i].read_bw._min = U64_MAX;
        serjio_stats->io_stats[i].read_iops._min = U64_MAX;
        serjio_stats->io_stats[i].write_submitted_bytes._min = U64_MAX;
        serjio_stats->io_stats[i].write_submitted_lbas._min = U64_MAX;
        serjio_stats->io_stats[i].write_bw._min = U64_MAX;
        serjio_stats->io_stats[i].write_iops._min = U64_MAX;
        serjio_stats->io_stats[i].io_duration._min = U64_MAX;
    }

    spin_unlock_irqrestore(&serjio_stats->lock, flags);
}

void nvmeibs_serjio_stats_init(struct nvmeibs_serjio_stats *serjio_stats)
{
    spin_lock_init(&serjio_stats->lock);
    nvmeibs_serjio_stats_clear(serjio_stats);
}

void nvmeibs_serjio_on_state_change(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_state old_state, enum nvmeibs_serjio_state new_state)
{
    unsigned long flags = 0;
    u64 duration;
    spin_lock_irqsave(&serjio_stats->lock, flags);
    serjio_stats->states_stats[new_state].t_start = ktime_get();
    duration = ktime_sub(ktime_get(), serjio_stats->states_stats[old_state].t_start);
    increase_work_count(&serjio_stats->states_stats[new_state].state_stats);
    update_new_value(&serjio_stats->states_stats[old_state].state_stats, duration);
    spin_unlock_irqrestore(&serjio_stats->lock, flags);
}


void nvmeibs_serjio_work_type_submit_io_success(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_work_type work_type, enum nvme_opcode op_code, u64 submitted_bytes, u64 block_size)
{
    unsigned long flags = 0;
    spin_lock_irqsave(&serjio_stats->lock, flags);
    if (op_code == nvme_cmd_read) {
        serjio_stats->io_stats[work_type].current_work_stats.read_submitted_bytes += submitted_bytes;
        serjio_stats->io_stats[work_type].current_work_stats.read_submitted_lbas += submitted_bytes / block_size;
        serjio_stats->io_stats[work_type].current_work_stats.submitted_read_iops++;

    } else if (op_code == nvme_cmd_write) {
        serjio_stats->io_stats[work_type].current_work_stats.write_submitted_bytes += submitted_bytes;
        serjio_stats->io_stats[work_type].current_work_stats.write_submitted_lbas += submitted_bytes / block_size;
        serjio_stats->io_stats[work_type].current_work_stats.submitted_write_iops++;
    } else {
        BUG_ON(1);
    }
    if (serjio_stats->io_stats[work_type].current_work_stats.t_start == 0) {
        serjio_stats->io_stats[work_type].current_work_stats.t_start = ktime_get();
    }
    spin_unlock_irqrestore(&serjio_stats->lock, flags);
}

void nvmeibs_serjio_work_type_submit_io_fail(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_work_type work_type, enum nvme_opcode op_code)
{
    unsigned long flags = 0;
    spin_lock_irqsave(&serjio_stats->lock, flags);
    if (op_code == nvme_cmd_read) {
        serjio_stats->io_stats[work_type].current_work_stats.n_submit_read_errors++;
    } else if (op_code == nvme_cmd_write) {
        serjio_stats->io_stats[work_type].current_work_stats.n_submit_write_errors++;
    } else {
        BUG_ON(1);
    }
    spin_unlock_irqrestore(&serjio_stats->lock, flags);
}

static const char *work_type_names[NVMEIBS_SERJIO_WORK_TYPE_MAX] = {
	[NVMEIBS_SERJIO_WORK_TYPE_ALLOC_RNG]               = "alloc_rng",
	[NVMEIBS_SERJIO_WORK_TYPE_RET_RNG]                 = "ret_rng",
	[NVMEIBS_SERJIO_WORK_TYPE_CHK_JGC]                 = "chk_jgc",
	[NVMEIBS_SERJIO_WORK_TYPE_RD_GPT]                  = "rd_gpt",
	[NVMEIBS_SERJIO_WORK_TYPE_RD_DB]                   = "rd_db",
	[NVMEIBS_SERJIO_WORK_TYPE_CLN_JRNL_DISK_RNG]       = "cln_jrnl_disk_rng",
	[NVMEIBS_SERJIO_WORK_TYPE_CLN_JRNL_DISK_RNG_START] = "cln_jrnl_disk_rng_start",
	[NVMEIBS_SERJIO_WORK_TYPE_CALL_ASSGN_RNG]          = "call_assgn_rng",
	[NVMEIBS_SERJIO_WORK_TYPE_CALL_RNG]                = "call_rng",
	[NVMEIBS_SERJIO_WORK_TYPE_FREE_JRNL_ENTS]          = "free_jrnl_ents",	
	[NVMEIBS_SERJIO_WORK_TYPE_ABND_ENTS]               = "abnd_ents",
	[NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD]                 = "gpt_upd",
	[NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD_DONE]            = "gpt_upd_done",
	[NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD_CANCEL]          = "gpt_upd_cancel",
	[NVMEIBS_SERJIO_WORK_TYPE_INIT_JRNL]               = "init_jrnl",
	[NVMEIBS_SERJIO_WORK_TYPE_RD_JRNL]                 = "rd_jrnl",
	[NVMEIBS_SERJIO_WORK_TYPE_READY]                   = "ready",
};
#define CORE_SERVER_STATS_PROC_FRMT_VER 1

inline static u64 __get_avg(struct nvmeibs_serjio_min_max_avg_stats *stats)
{
	return stats->overall_count != 0 ? (stats->_sum / stats->overall_count) : 0;
}

#define STATS_MIN(_stats) ((_stats)->_min == U64_MAX ? 0 : (_stats)->_min)
#define STATS_MIN_MAX_AVG_PR_ARGS_MSECS(_stats) \
                                        ktime_to_ms(STATS_MIN(_stats)), \
                                        ktime_to_ms((_stats)->_max), \
                                        ktime_to_ms(__get_avg(_stats))

#define STATS_MIN_MAX_AVG_PR_ARGS(_stats) \
                                    STATS_MIN(_stats), \
                                    (_stats)->_max, \
                                    __get_avg(_stats)

#define STATS_JDR_ARGS(_stats, __jdr) \
                                    jdr_write_var((__jdr), min, STATS_MIN(_stats)); \
                                    jdr_write_var((__jdr), max, (_stats)->_max); \
                                    jdr_write_var((__jdr), avg, __get_avg(_stats)); \
                                    jdr_write_var((__jdr), overall_count, (_stats)->overall_count); \
                                    jdr_write_var((__jdr), current_count, (_stats)->current_count)

#define STATS_JDR_ARGS_MSECS(_stats, __jdr) \
                                    jdr_write_var((__jdr), min, ktime_to_ms(STATS_MIN(_stats))); \
                                    jdr_write_var((__jdr), max, ktime_to_ms((_stats)->_max)); \
                                    jdr_write_var((__jdr), avg, ktime_to_ms(__get_avg(_stats))); \
                                    jdr_write_var((__jdr), overall_count, (_stats)->overall_count); \
                                    jdr_write_var((__jdr), current_count, (_stats)->current_count)
                                    
#define STATS_JDR_SCOPE_NAME(_stats, __jdr, __name) \
                                    {                               \
                                        jdr_object_scope((__jdr), __name); \
                                        STATS_JDR_ARGS((_stats), (__jdr)); \
                                    }

ssize_t nvmeibs_serjio_fill_serjio_stats_json(struct nvmeibs_serjio_stats *serjio_stats, char *buffer, size_t len)
{
    unsigned long flags = 0;
    int i;
    struct jdr jdr = jdr_make((struct charvec){.base = buffer, .len = len});
    struct charvec result;
    spin_lock_irqsave(&serjio_stats->lock, flags);
    {
		jdr_array_scope(&jdr, "work_stats");
		for (i = 0; i < NVMEIBS_SERJIO_WORK_TYPE_MAX; i++) {
			struct nvmeibs_serjio_works_stats_total *ws = &serjio_stats->work_stats_by_type[i];
			
			{
				jdr_object_scope(&jdr, NULL);
				jdr_write_var(&jdr, work_type, work_type_names[i]);
				jdr_write_var(&jdr, n_works, ws->n_works);
				jdr_write_var(&jdr, n_errors, ws->n_errors);
				jdr_write_var(&jdr, n_rescheds, ws->n_rescheds);

                STATS_JDR_SCOPE_NAME(&ws->works_queued, &jdr, "works_queued");

                STATS_JDR_SCOPE_NAME(&ws->works_exec_time, &jdr, "works_exec_time");
			}
		}
	}

	{
		jdr_array_scope(&jdr, "states_stats");
		for (i = 0; i < MAX_SERJIO_STATE; i++) {
			jdr_object_scope(&jdr, NULL);
			jdr_write_var(&jdr, state, nvmeib_shared_serjio_state_to_str(i));
            STATS_JDR_ARGS(&serjio_stats->states_stats[i].state_stats, &jdr);
		}
	}
	
	{
		jdr_object_scope(&jdr, "io_stats");
		u64 total_works = 0;
		u64 read_bw_sum = 0, write_bw_sum = 0;
		u64 read_iops_sum = 0, write_iops_sum = 0;
		u64 read_bw_count = 0, write_bw_count = 0;
		u64 read_iops_count = 0, write_iops_count = 0;
		u64 read_bw_max = 0, write_bw_max = 0;
		u64 read_iops_max = 0, write_iops_max = 0;

		u64 submit_read_errors_count = 0, submit_write_errors_count = 0;
		u64 submit_read_errors_max = 0, submit_write_errors_max = 0;

        u64 read_errors_count = 0, write_errors_count = 0;
        u64 read_errors_max = 0, write_errors_max = 0;

		for (i = 0; i < NVMEIBS_SERJIO_WORK_TYPE_MAX; i++) {
			struct nvmeibs_serjio_io_stats *io_stats = &serjio_stats->io_stats[i];

			total_works += serjio_stats->work_stats_by_type[i].n_works;

			// read_bw
			if (io_stats->read_bw.overall_count != 0) {
				read_bw_sum += io_stats->read_bw._sum;
				read_bw_count += io_stats->read_bw.overall_count;
                read_bw_max = max_t(u64,read_bw_max, io_stats->read_bw._max);
			}

			// write_bw
			if (io_stats->write_bw.overall_count != 0) {
				write_bw_sum += io_stats->write_bw._sum;
				write_bw_count += io_stats->write_bw.overall_count;
				write_bw_max = max_t(u64, write_bw_max, io_stats->write_bw._max);
			}

			// read_iops
			if (io_stats->read_iops.overall_count != 0) {
				read_iops_sum += io_stats->read_iops._sum;
				read_iops_count += io_stats->read_iops.overall_count;
				read_iops_max = max_t(u64, read_iops_max, io_stats->read_iops._max);
			}

			// write_iops
			if (io_stats->write_iops.overall_count != 0) {
				write_iops_sum += io_stats->write_iops._sum;
				write_iops_count += io_stats->write_iops.overall_count;
				write_iops_max = max_t(u64, write_iops_max, io_stats->write_iops._max);
			}

            if (io_stats->n_submit_read_errors.overall_count != 0) {
                submit_read_errors_count += io_stats->n_submit_read_errors.overall_count;
                submit_read_errors_max = max_t(u64, submit_read_errors_max, io_stats->n_submit_read_errors._max);
            }

            if (io_stats->n_submit_write_errors.overall_count != 0) {
                submit_write_errors_count += io_stats->n_submit_write_errors.overall_count;
                submit_write_errors_max = max_t(u64, submit_write_errors_max, io_stats->n_submit_write_errors._max);
            }

            if (io_stats->n_read_errors.overall_count != 0) {
                read_errors_count += io_stats->n_read_errors.overall_count;
                read_errors_max = max_t(u64, read_errors_max, io_stats->n_read_errors._max);
            }

            if (io_stats->n_write_errors.overall_count != 0) {
                write_errors_count += io_stats->n_write_errors.overall_count;
                write_errors_max = max_t(u64, write_errors_max, io_stats->n_write_errors._max);
            }

			// Write original per-work-type stats
			{
				jdr_object_scope(&jdr, work_type_names[i]);
                STATS_JDR_SCOPE_NAME(&io_stats->read_bw, &jdr, "read_bw");
                STATS_JDR_SCOPE_NAME(&io_stats->read_iops, &jdr, "read_iops");

                STATS_JDR_SCOPE_NAME(&io_stats->read_submitted_bytes, &jdr, "read_submitted_bytes");
                STATS_JDR_SCOPE_NAME(&io_stats->read_submitted_lbas, &jdr, "read_submitted_lbas");
                STATS_JDR_SCOPE_NAME(&io_stats->read_completed_bytes, &jdr, "read_completed_bytes");
                STATS_JDR_SCOPE_NAME(&io_stats->read_completed_lbas, &jdr, "read_completed_lbas");

                STATS_JDR_SCOPE_NAME(&io_stats->write_bw, &jdr, "write_bw");
                STATS_JDR_SCOPE_NAME(&io_stats->write_iops, &jdr, "write_iops");

                STATS_JDR_SCOPE_NAME(&io_stats->write_submitted_bytes, &jdr, "write_submitted_bytes");
                STATS_JDR_SCOPE_NAME(&io_stats->write_submitted_lbas, &jdr, "write_submitted_lbas");
                STATS_JDR_SCOPE_NAME(&io_stats->write_completed_bytes, &jdr, "write_completed_bytes");
                STATS_JDR_SCOPE_NAME(&io_stats->write_completed_lbas, &jdr, "write_completed_lbas");

                STATS_JDR_SCOPE_NAME(&io_stats->io_duration, &jdr, "io_duration");

                STATS_JDR_SCOPE_NAME(&io_stats->n_submit_read_errors, &jdr, "n_submit_read_errors");
                STATS_JDR_SCOPE_NAME(&io_stats->n_submit_write_errors, &jdr, "n_submit_write_errors");

                STATS_JDR_SCOPE_NAME(&io_stats->n_read_errors, &jdr, "n_read_errors");
                STATS_JDR_SCOPE_NAME(&io_stats->n_write_errors, &jdr, "n_write_errors");

                STATS_JDR_SCOPE_NAME(&io_stats->error_read_bytes, &jdr, "error_read_bytes");
                STATS_JDR_SCOPE_NAME(&io_stats->error_read_lbas, &jdr, "error_read_lbas");
                STATS_JDR_SCOPE_NAME(&io_stats->error_write_bytes, &jdr, "error_write_bytes");
                STATS_JDR_SCOPE_NAME(&io_stats->error_write_lbas, &jdr, "error_write_lbas");
                
			}
		}

		{
			// Emit total/aggregate works, avg/max bw & iops
			jdr_object_scope(&jdr, "works_io_total");
			jdr_write_var(&jdr, total_works, total_works);

			// Average: weighted by all samples, or 0 if none
			jdr_write_var(&jdr, read_bw_avg,   read_bw_count   ? (read_bw_sum   / read_bw_count)   : 0);
			jdr_write_var(&jdr, write_bw_avg,  write_bw_count  ? (write_bw_sum  / write_bw_count)  : 0);
			jdr_write_var(&jdr, read_iops_avg, read_iops_count ? (read_iops_sum / read_iops_count) : 0);
			jdr_write_var(&jdr, write_iops_avg,write_iops_count? (write_iops_sum/ write_iops_count): 0);

			jdr_write_var(&jdr, read_bw_max, read_bw_max);
			jdr_write_var(&jdr, write_bw_max, write_bw_max);
			jdr_write_var(&jdr, read_iops_max, read_iops_max);
			jdr_write_var(&jdr, write_iops_max, write_iops_max);

            jdr_write_var(&jdr, submit_read_errors_count, submit_read_errors_count);
            jdr_write_var(&jdr, submit_write_errors_count, submit_write_errors_count);
            jdr_write_var(&jdr, read_errors_count, read_errors_count);
            jdr_write_var(&jdr, write_errors_count, write_errors_count);
            jdr_write_var(&jdr, submit_read_errors_max, submit_read_errors_max);
            jdr_write_var(&jdr, submit_write_errors_max, submit_write_errors_max);
            jdr_write_var(&jdr, read_errors_max, read_errors_max);
            jdr_write_var(&jdr, write_errors_max, write_errors_max);
		}

	}
    nvmeib_proc_add_jdr_proc_epilog(CORE_SERVER_STATS_PROC_FRMT_VER, &jdr);
    spin_unlock_irqrestore(&serjio_stats->lock, flags);
    result = jdr_finalize(&jdr);
    return result.len;
}


#define STATS_MIN_MAX_AVG_PR_FMT "%12lld\t%12lld\t%12lld"
// Helper function to print a single io stat line
static ssize_t print_io_stat_line(char *buffer, size_t buflen, const char *type, const char *criteria, struct nvmeibs_serjio_min_max_avg_stats *stat)
{
	return scnprintf(buffer, buflen,
				  "%-22s\t|\t%-22s\t|\t%12lld\t|\t%12lld\t|\t%12lld\t|\t%12llu\n",
				  type,
				  criteria,
				  STATS_MIN_MAX_AVG_PR_ARGS(stat),
				  stat->overall_count);
}


ssize_t nvmeibs_serjio_fill_serjio_stats_readable(struct nvmeibs_serjio_stats *serjio_stats, char *buffer, size_t len)
{
    unsigned long flags = 0;

	ssize_t count = 0;
	int i;

    spin_lock_irqsave(&serjio_stats->lock, flags);

	count += scnprintf(buffer + count, len - count, "SERJIO Server Statistics\n");
	count += scnprintf(buffer + count, len - count, "========================\n\n");

	count += scnprintf(buffer + count, len - count, "Work Stats by Type:\n");
	count += scnprintf(buffer + count, len - count, "%-22s\t%10s\t%10s\t%10s\t%12s\t%12s\t%12s\t%12s\t%12s\t%12s\t%12s\t%12s\t%12s\t%12s\n",
		"Type", "n_works", "n_errors", "n_rescheds", "wq_min(ns)", "wq_max(ns)", "wq_avg(ns)", "wq_total", "wq_cur", "exec_min(ns)", "exec_max(ns)", "exec_avg(ns)", "exec_total", "exec_cur");

	for (i = 0; i < NVMEIBS_SERJIO_WORK_TYPE_MAX; i++) {
		struct nvmeibs_serjio_works_stats_total *ws = &serjio_stats->work_stats_by_type[i];

		count += scnprintf(buffer + count, len - count, "%-22s\t%10llu\t%10llu\t%10llu\t" STATS_MIN_MAX_AVG_PR_FMT "\t%12llu\t%12llu\t" STATS_MIN_MAX_AVG_PR_FMT "\t%12llu\t%12llu\n",
			work_type_names[i],
			ws->n_works,
			ws->n_errors,
			ws->n_rescheds,
			STATS_MIN_MAX_AVG_PR_ARGS(&ws->works_queued),
			ws->works_queued.overall_count, ws->works_queued.current_count,
			STATS_MIN_MAX_AVG_PR_ARGS(&ws->works_exec_time),
			ws->works_exec_time.overall_count, ws->works_exec_time.current_count
		);
	}

	count += scnprintf(buffer + count, len - count, "\nState Stats:\n");
	count += scnprintf(buffer + count, len - count, "%-18s\t%12s\t%12s\t%12s\t%12s\t%12s\n",
		"State", "min(ns)", "max(ns)", "avg(ns)", "overall", "current");
	for (i = 0; i < MAX_SERJIO_STATE; i++) {
		count += scnprintf(buffer + count, len - count, "%-18s\t" STATS_MIN_MAX_AVG_PR_FMT "\t%12llu\t%12llu\n",
			nvmeib_shared_serjio_state_to_str(i),
			STATS_MIN_MAX_AVG_PR_ARGS(&serjio_stats->states_stats[i].state_stats),
			serjio_stats->states_stats[i].state_stats.overall_count,
			serjio_stats->states_stats[i].state_stats.current_count
		);
	}

	count += scnprintf(buffer + count, len - count, "\nIO Stats by Work Type:\n");
	count += scnprintf(buffer + count, len - count, "%-22s\t|\t%-22s\t|\t%12s\t|\t%12s\t|\t%12s\t|\t%12s\n",
		"Type", "criteria", "min", "max", "avg", "n_samples");


	for (i = 0; i < NVMEIBS_SERJIO_WORK_TYPE_MAX; i++) {
		struct nvmeibs_serjio_io_stats *io_stats = &serjio_stats->io_stats[i];
		const char *type = work_type_names[i];

		// Read stats section
		count += print_io_stat_line(buffer + count, len - count, type, "read_bytes", &io_stats->read_submitted_bytes);
		count += print_io_stat_line(buffer + count, len - count, type, "read_lbas", &io_stats->read_submitted_lbas);
		count += print_io_stat_line(buffer + count, len - count, type, "read_bw(B/s)", &io_stats->read_bw);
		count += print_io_stat_line(buffer + count, len - count, type, "read_iops(IOPS)", &io_stats->read_iops);
        count += print_io_stat_line(buffer + count, len - count, type, "read_completed_bytes", &io_stats->read_completed_bytes);
        count += print_io_stat_line(buffer + count, len - count, type, "read_completed_lbas", &io_stats->read_completed_lbas);

		// Write stats section
		count += print_io_stat_line(buffer + count, len - count, type, "write_bytes", &io_stats->write_submitted_bytes);
		count += print_io_stat_line(buffer + count, len - count, type, "write_lbas", &io_stats->write_submitted_lbas);
		count += print_io_stat_line(buffer + count, len - count, type, "write_bw(B/s)", &io_stats->write_bw);
		count += print_io_stat_line(buffer + count, len - count, type, "write_iops(IOPS)", &io_stats->write_iops);
        count += print_io_stat_line(buffer + count, len - count, type, "write_completed_bytes", &io_stats->write_completed_bytes);
        count += print_io_stat_line(buffer + count, len - count, type, "write_completed_lbas", &io_stats->write_completed_lbas);
		// IO duration (common section)
		count += print_io_stat_line(buffer + count, len - count, type, "io_duration(ns)", &io_stats->io_duration);

        count += print_io_stat_line(buffer + count, len - count, type, "n_submit_read_errors", &io_stats->n_submit_read_errors);
        count += print_io_stat_line(buffer + count, len - count, type, "n_submit_write_errors", &io_stats->n_submit_write_errors);
        count += print_io_stat_line(buffer + count, len - count, type, "n_read_errors", &io_stats->n_read_errors);
        count += print_io_stat_line(buffer + count, len - count, type, "n_write_errors", &io_stats->n_write_errors);

        count += print_io_stat_line(buffer + count, len - count, type, "error_read_bytes", &io_stats->error_read_bytes);
        count += print_io_stat_line(buffer + count, len - count, type, "error_read_lbas", &io_stats->error_read_lbas);
        count += print_io_stat_line(buffer + count, len - count, type, "error_write_bytes", &io_stats->error_write_bytes);
        count += print_io_stat_line(buffer + count, len - count, type, "error_write_lbas", &io_stats->error_write_lbas);
	}

	spin_unlock_irqrestore(&serjio_stats->lock, flags);

	count += scnprintf(buffer + count, len - count, "\n");

	return count;
}

void nvmeibs_serjio_update_io_stats_on_rsrc_completion(struct nvmeibs_serjio_stats *serjio_stats, enum nvmeibs_serjio_work_type work_type,
                                            size_t completed_bytes, size_t completed_lbas, int completed_iops, bool is_read, bool is_error,
                                            struct nvmeibs_serjio_op_rsrc_stats *op_rsrc_stats)
{
    unsigned long flags = 0;
    ktime_t t_completion = ktime_get();
    ktime_t duration = 0;
    spin_lock_irqsave(&serjio_stats->lock, flags);
    duration = ktime_sub(t_completion, op_rsrc_stats->t_start);
    if (is_error) {
        if (is_read) {
            serjio_stats->io_stats[work_type].current_work_stats.n_complete_read_errors++;
            update_new_value_inc(&serjio_stats->io_stats[work_type].error_read_bytes, completed_bytes);
            update_new_value_inc(&serjio_stats->io_stats[work_type].error_read_lbas, completed_lbas);
        } else {
            serjio_stats->io_stats[work_type].current_work_stats.n_complete_write_errors++;
            update_new_value_inc(&serjio_stats->io_stats[work_type].error_write_bytes, completed_bytes);
            update_new_value_inc(&serjio_stats->io_stats[work_type].error_write_lbas, completed_lbas);
        }
    } else {
        //on success
        if (is_read) {
            serjio_stats->io_stats[work_type].current_work_stats.read_completed_bytes += completed_bytes;
            serjio_stats->io_stats[work_type].current_work_stats.read_completed_lbas += completed_lbas;
            serjio_stats->io_stats[work_type].current_work_stats.completed_read_iops += completed_iops;
        } else {
            serjio_stats->io_stats[work_type].current_work_stats.write_completed_bytes += completed_bytes;
            serjio_stats->io_stats[work_type].current_work_stats.write_completed_lbas += completed_lbas;
            serjio_stats->io_stats[work_type].current_work_stats.completed_write_iops += completed_iops;
        }
        serjio_stats->io_stats[work_type].current_work_stats.accumulated_io_duration += duration;
    }

    spin_unlock_irqrestore(&serjio_stats->lock, flags);
}
