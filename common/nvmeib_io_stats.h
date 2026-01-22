/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_IO_STATS_H
#define NVMEIB_IO_STATS_H

#include "kr_incs.h"
#include "nvmeib_json.h"

/* Terms:
   IO timelines consists of:
     0. IO arrives to nvmeibc (kernel make_request)
     1. Optional: Waiting due to io throttling mechanism
     2. Optional: Failed execution attempts (disk stopped, broken lock, etc)
     3. Optional: Waiting in resubmition thread, coz IO on volume is disabled
     4. Execution done: can be successful, non-transient error, timeout
     5. Return result to kernel (bio_endio)

   io_e2e       - measures steps [0..5] in nsec (jiffies resolution)
   io_latency   - measures steps [2..5] in nsec
   io_execution - measures steps 2 and 4 in nsec
   io_size      - size of io in bytes (not blocks) */
struct nvmeib_io_counters {	/* Cumulative stats */
	u64 total_ops;		/* amount of operations */
	int inflight_ops;	/* amount of operations in-flight */
	int unused;             /* unused 4 bytes counter */
	u64 total_executions;   /* amount of operations executions */
	u64 total_size;		/* sum of the operation io sizes [bytes] */
	u64 total_latency;      /* sum of the operation latencies [nsecs] */
	u64 total_latency_sqr;  /* sum of the operation latency squared */
	u64 total_io_exec;	/* sum of operations io_execution [nsecs] */
	u64 total_e2e_exec;	/* sum of operations io_e2e [nsecs] */
	u64 worst_latency;      /* the longest operation latency [nsecs] */
	u64 worst_io_exec;	/* worst io execution time [nsecs] */
	u64 worst_e2e_exec; 	/* wrost end2end operation. [micro-seconds] */
	u64 total_sub_block;	/* commands representing 512b sub-block ops */
};

enum nvmeib_io_stat_verbs {
	IO_STAT_VERB_READ = 0,
	IO_STAT_VERB_WRITE,
	IO_STAT_VERB_DISCARD,
	IO_STAT_VERB_RECOV_READ,
	IO_STAT_VERB_RECOV_WRITE,
	IO_STAT_VERB_GEN_TX,
	IO_STAT_VERB_GEN_RX,
	N_IO_STAT_VERBS
};

#define VERB_TO_MASK(verb) (1 << verb)

#define VERB_RW_T_BITMASK (VERB_TO_MASK(IO_STAT_VERB_READ) | VERB_TO_MASK(IO_STAT_VERB_WRITE) | VERB_TO_MASK(IO_STAT_VERB_DISCARD))
#define VERB_RW_T_RECOV_BITMASK (VERB_RW_T_BITMASK | VERB_TO_MASK(IO_STAT_VERB_RECOV_READ) | VERB_TO_MASK(IO_STAT_VERB_RECOV_WRITE))
#define VERB_RW_T_RECOV_GEN_BITMASK (VERB_RW_T_BITMASK | VERB_TO_MASK(IO_STAT_VERB_RECOV_READ) | VERB_TO_MASK(IO_STAT_VERB_RECOV_WRITE) | VERB_TO_MASK(IO_STAT_VERB_GEN_TX) | VERB_TO_MASK(IO_STAT_VERB_GEN_RX))

#define VERB_RW_NUM 3 /* read/write/discard */

#define SQUARE_LAT_DIV_TERM_POW2	4

static inline u64 nvmeib_square_latency(u64 latency)
{
	return ((latency >> SQUARE_LAT_DIV_TERM_POW2) * (latency >> SQUARE_LAT_DIV_TERM_POW2));
}

static inline enum nvmeib_io_stat_verbs io_op_to_verb(enum nvmeib_block_io_op op, bool is_recovery)
{
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_READ:		return is_recovery ? IO_STAT_VERB_RECOV_READ : IO_STAT_VERB_READ;
	case NVMEIB_BLOCK_IO_OP_WRITE:		return is_recovery ? IO_STAT_VERB_RECOV_WRITE: IO_STAT_VERB_WRITE;
	case NVMEIB_BLOCK_IO_OP_DISCARD:	return IO_STAT_VERB_DISCARD;
	default:
		BUG();
		break;
	}
	return (enum nvmeib_io_stat_verbs)-1;
}

static inline enum nvmeib_io_stat_verbs good_path_profiler_io_op_to_rwt_verb(enum nvmeib_block_io_op op)
{
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_READ:		return IO_STAT_VERB_READ;
	case NVMEIB_BLOCK_IO_OP_WRITE:		return IO_STAT_VERB_WRITE;
	case NVMEIB_BLOCK_IO_OP_DISCARD:	return IO_STAT_VERB_DISCARD;
	default:
		BUG();
		break;
	}
	return N_IO_STAT_VERBS;
}

static inline bool io_op_is_rwt(enum nvmeib_block_io_op op)
{
	return (op >= NVMEIB_BLOCK_IO_OP_READ && op <= NVMEIB_BLOCK_IO_OP_DISCARD);
}

static inline const char *verb_to_string(enum nvmeib_io_stat_verbs verb, bool lower_case)
{
	switch (verb) {
	case IO_STAT_VERB_READ:
		return lower_case ? "read" : "READ";
	case IO_STAT_VERB_WRITE:
		return lower_case ? "write" : "WRITE";
	case IO_STAT_VERB_DISCARD:
		return lower_case ? "trim" : "TRIM";
	case IO_STAT_VERB_RECOV_READ:
		return lower_case ? "recovery_read" : "RECOVERY_READ";
	case IO_STAT_VERB_RECOV_WRITE:
		return lower_case ? "recovery_write" : "RECOVERY_WRITE";
	case IO_STAT_VERB_GEN_TX:
		return lower_case ? "gen_tx" : "GEN_TX";
	case IO_STAT_VERB_GEN_RX:
		return lower_case ? "gen_rx"  : "GEN_RX";
	default:
		return lower_case ? "unknown" : "UNKNOWN";
	}
}

struct nvmeib_json_ops;

/* 'nvmeib_io_counters' cant be used directly in multi-cpu environment so the
   actual set of counter is allocated per-cpu, per-io-type, per-io-size*/
struct nvmeib_io_stats;
struct nvmeib_io_stats *nvmeib_io_stats_create(const char *name, unsigned long verbs_bitmask, unsigned block_size);
struct nvmeib_io_stats *nvmeib_io_stats_create_traced(const char *name, unsigned long verbs_bitmask, unsigned block_size);
void nvmeib_io_stats_free(struct nvmeib_io_stats *ds);

void nvmeib_io_stats_set_block_size(struct nvmeib_io_stats *ds, unsigned block_size);

bool nvmeib_io_stats_counts_verb(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb);

/* Used for debug only. reset counters of all CPU's to zero. */
// which = 'A' = All, 'W' = Worst counters, 'T' = Total counters
void nvmeib_io_stats_clear(struct nvmeib_io_stats *ds, 		  const int which);	// All counters

/* Current cpu updates the io counters */
void nvmeib_io_stats_update_one(struct nvmeib_io_stats *child_stats, struct nvmeib_io_stats *parent_stats,
			    const enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters src);


#ifndef UM_APP
void nvmeib_io_stats_operation_start(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb, u64 size);

void nvmeib_io_stats_operation_end(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb, u64 size);
#else
static inline void nvmeib_io_stats_operation_start(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb, u64 size)
{
	(void)ds; (void)verb; (void)size;
}

static inline void nvmeib_io_stats_operation_end(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb, u64 size)
{
	(void)ds; (void)verb; (void)size;
}
#endif


static inline void nvmeib_io_stats_adjust_and_update(struct nvmeib_io_stats *child_stats, struct nvmeib_io_stats *parent_stats,
			    const enum nvmeib_io_stat_verbs verb, const u64 size, const u64 lat, const bool is_sub_block)
{
	struct nvmeib_io_counters io_counters = {
		.total_ops = 1,
		.total_executions = 1,
		.inflight_ops = 0,
		.total_size = size,
		.total_latency = lat,
		.total_latency_sqr = nvmeib_square_latency(lat),
		.total_io_exec = lat,
		.total_e2e_exec = lat,
		.worst_latency = lat,
		.worst_io_exec = lat,
		.worst_e2e_exec = lat,
		.total_sub_block = !!is_sub_block
	};

	nvmeib_io_stats_update_one(child_stats, parent_stats, verb, io_counters);
}

/* Warning: Heavy cpu-load functions.
   1. Calculates sums of counters according to a given filter
    	{IO type: R/W/T, IO size: 0-All or X for IO's of that size }
   2. Converts all values to units of 10^-7[secs] (1/10 micro second) */
void nvmeib_io_stats_readc(struct nvmeib_io_stats *ds,
	const enum nvmeib_io_stat_verbs verb, const u64 size, struct nvmeib_io_counters *result_c);
void nvmeib_io_stats_readc_per_bin(struct nvmeib_io_stats *ds,
	const enum nvmeib_io_stat_verbs verb, const unsigned bin, struct nvmeib_io_counters *result_c);
unsigned nvmeib_io_stats_get_n_bins(void);
const char *nvmeib_io_stats_get_bin_name(struct nvmeib_io_stats *ds,
	const unsigned bin, char *buf, size_t buf_size);

/* Encode IO stats in json, for user space apps */
ssize_t nvmeib_io_stats_to_json(struct nvmeib_io_stats *ds,
								char *buf, size_t len, const ulong uptime_jiff,
								const struct nvmeib_json_ops *jops, size_t indent,
								bool is_last);

#define IO_STAT_VERB_TFMT "VERB: @IO_STAT_VERB"
#define IO_STAT_VERB_TARG(verb) verb
#define IO_STAT_COUNTERS_BASIC_TFMT "OPS: @IO_STAT_COUNTER_OPS SIZE: @IO_STAT_COUNTER_SIZE LAT_100NS: @IO_STAT_COUNTER_LAT_100NS LAT_SQR_100NS2: @IO_STAT_COUNTER_LAT_SQR_100NS2"
#define IO_STAT_COUNTERS_TFMT IO_STAT_COUNTERS_BASIC_TFMT " IO_EXEC_LAT: @IO_STAT_COUNTER_IO_EXEC_LAT E2E_EXEC_LAT: @IO_STAT_COUNTER_E2E_EXEC_LAT"
#define IO_STAT_COUNTERS_BASIC_TARG(c) (c)->total_ops, (c)->total_size, (c)->total_latency / 10, (c)->total_latency_sqr / 10
#define IO_STAT_COUNTERS_TARG(c) IO_STAT_COUNTERS_BASIC_TARG(c), (c)->total_io_exec, (c)->total_e2e_exec

void nvmeib_io_stats_trace(struct nvmeib_io_stats *ds, void (*trace_fn)(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx), void *trace_fn_ctx);
void nvmeib_io_stats_trace_ext(struct nvmeib_io_stats *ds, void (*trace_fn)(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx), void *trace_fn_ctx, bool diff_only);

struct nvmeib_txt;
void nvmeib_iostats_sum_to_string(struct nvmeib_io_stats *stats, const ulong up_time, const u64 io_prob,
				     struct nvmeib_txt *txt);
struct jdr;
void nvmeib_io_stats_tojson_jdr(struct nvmeib_io_stats *ds, const ulong uptime_jiff, struct jdr *jdr);
#endif

