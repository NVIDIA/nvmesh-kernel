/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef CLNT_BLOCK_UNITEST_DI_TRACKER_H_
#define CLNT_BLOCK_UNITEST_DI_TRACKER_H_

#define DI_TRACKER_VOL_SIZE 256	// in sectors. (Track only the first 1M of each volume.)
#define DI_TRACKER_TRACKED_BLOCK_PREFIX_SIZE 16	// compatible with debug_di (untouched)

enum di_tracker_trim_assumed_action {
	DT_TRIM_ZEROES,
	DT_TRIM_NOOP,
};

struct volume_di_tracker_conf {
	enum di_tracker_trim_assumed_action trim_assumed_action;
};

struct volume_di_tracker {
	int vol_i;
	bool enabled;
	u64 size;	// LBAs
	struct volume_di_tracker_conf *conf;
	struct block_di_tracker *block_di_trackers;
	u64 n_ios_inflight;
	pthread_mutex_t lock;
	pthread_cond_t idle_cond; // condition variable for no IOs in-flight
};

struct di_tracker {
	int nvols;
	struct volume_di_tracker *volume_di_trackers;
};

void di_tracker_init(struct di_tracker *dit, struct volume_di_tracker_conf *conf, int nvols);
void di_tracker_fini(struct di_tracker *dit);

void di_tracker_enable(struct di_tracker *dit);
void di_tracker_disable(struct di_tracker *dit);

void di_tracker_reconf(struct di_tracker *dit, struct volume_di_tracker_conf *conf);

void di_tracker_reset(struct di_tracker *dit);

struct di_tracker_io_context;

struct di_tracker_io_context *volume_di_tracker_track_io_start(struct volume_di_tracker *vdit, u64 start_lba, u64 nlbas, unsigned long bi_rw, struct bio_vec *bi_io_vec, unsigned short	bi_vcnt);
void volume_di_tracker_track_io_end(struct volume_di_tracker *vdit, struct di_tracker_io_context *ctx, int rv);

#endif /* CLNT_BLOCK_UNITEST_DI_TRACKER_H_ */
