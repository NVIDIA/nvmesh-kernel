/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_DP_PROFILING_GENERIC_H
#define NVMEIBC_BLOCK_DP_PROFILING_GENERIC_H

#include "nvmeibc_block.h"					/* external API of the block */

/*************** Real-time Datapath statistics of slow IO's **************/
/* Structure which gathers statistics of topologies */
struct topo_stats_t {
	u64 n_multiple_deg;  // Number of multiple deg topologies (i.e double degraded)
	u64 n_single_deg;  // Number of single deg topologies (i.e double degraded)
};

void topo_stats_t_clear(struct topo_stats_t *t);

/* Structure which gathers statistics of problematic IO's (too much retries of
   specific lock, etc...) */
struct slow_io_stats_t {
	struct slow_io_stats_lock_retry_t {
		atomic_t num_l;				// Number of locks that each took too much time to aquare/view
		u32 lockid;					// Store the last lock id that was over-retried
		u32 n_retries;				// Number of retry attempts that this lock did. If amount of retries is low then each try takes long time
		u32 n_msecs;				// Number of millliseconds this rdma op waiting to succeed
		u64 dlba_blkset;			// Blockset lba (index of blockset from start of disk), as caluclated by block layer
		u16 ci, ri, si;				// Identifier of the segment on which lock was stuck {chunk,raid,seg}
		u8  intent;					// enum nvmeibc_rdma_intent
	} over_retried;
	#ifdef DEBUG_LOCK_RETRY
		atomic_t	pend_retry_rdl;	// Number of locks in IO operations that are waiting for retry timeout to for READ op
		atomic_t	pend_retry_wrl;	// Number of locks in IO operations that are waiting for retry timeout to for WRITE/TRIM op
	#endif
};

void slow_io_stats_t_init( struct slow_io_stats_t *t);
void slow_io_stats_t_clear(struct slow_io_stats_t *t);
/* To string to debug in proc file */
void slow_io_stats_t_tostring(const struct slow_io_stats_t *t, struct nvmeib_txt *txt);
void slow_io_stats_t_tojson(const struct slow_io_stats_t *t, struct jdr *jdr);

struct nvmeibc_cmd_lock;
void slow_io_stats_t_log_over_retry(struct slow_io_stats_t *t,const struct nvmeibc_cmd_lock *l, u64 lockid);

#ifdef DEBUG_LOCK_RETRY
#define slow_io_stats_t_rdl_inc(t) atomic_inc(&(t)->pend_retry_rdl)
#define slow_io_stats_t_rdl_dec(t) atomic_dec(&(t)->pend_retry_rdl)
#define slow_io_stats_t_wrl_inc(t) atomic_inc(&(t)->pend_retry_wrl)
#define slow_io_stats_t_wrl_dec(t) atomic_dec(&(t)->pend_retry_wrl)
#else
#define slow_io_stats_t_rdl_inc(t)
#define slow_io_stats_t_rdl_dec(t)
#define slow_io_stats_t_wrl_inc(t)
#define slow_io_stats_t_wrl_dec(t)
#endif

/********************** Debugging functions and utils ************************/
typedef const char *(*nvmeibc_profiler_get_stage_name)(u8 stage_index); //somehow we need to limit the name length

// Unique value given to translate functions to return number of stage in profiler
static const int PROFILING_GET_N_STAGES = 128;
typedef u8(*nvmeibc_profiler_get_profiling_index)(int stage);

enum nvmeibc_profiler_type{
	NVMEIBC_PROFILER_TYPE_CMD = 0, //in this case profiler contains unrelated probes, like read/write with/without piggyback
	NVMEIBC_PROFILER_TYPE_FLOW = 1 //in this case profiles attached to the high level object like operation or sync and measures
								   //the time for every logical stage and then the total time
};

struct nvmeibc_profiler_configuration {
	enum nvmeibc_profiler_type type;
	nvmeibc_profiler_get_stage_name stage2name;	// The function gets the stage index and returns it's name - set at initialization
	nvmeibc_profiler_get_profiling_index translate; // This function returns the translation of a stage to the index (when given PROFILING_GET_STAGE returns number of stages)
};

#define PROFILING_INTERVALS_COUNT (12)
//every interval counts how many times, the stage execution completed within pre-defined period of time;
//for simplicity, every interval will cover power of 2 microseconds [1,2,4,8,16,32,64,128,256,512,1024,2048]
#define PROFILING_RETRY_COUNT (3)
// How many retry attempts for the same io are monitored 3 -> (1, 2, 3+)
// Each retry count is accumulated separtely and the accumulative time delayed


// Single profiling object with relation to a single stage in DP or a single operation in transport
struct nvmeibc_profiling_stats_stage {
	u64 started;								// rdtsc value, set when we starting to measure the current stage/transport
	u64 buckets[PROFILING_INTERVALS_COUNT]; 	// Each stage timing will go into a single bucket and increment the counter
	u64 exact_time;								// Buckets give an idea of the most common timing, this will allow exact calculation of average
	u32 count;									// Number of collected samples for this stage
	u64 error_time;								// When an error occurs accutmilate the error run stage, compare later to buckets for assurence that we are efficient on errors
	u32 error_count;							// When a stage fails, note this in an error count
	u32 retry[PROFILING_RETRY_COUNT];			// Lock retry count
	u64 retry_time[PROFILING_RETRY_COUNT];		// Total retry time
};

// Can be used as an array for profiling multiple objects in parallel
struct nvmeibc_profiler {
	struct kref num_users;							// A kref object to ensure we can use the profiler for each and every segment/praid/block device, and to free it only when the reference is 0
	void* stats_object; 							// the object the profiler is currently monitoring - stats can only be taken one at a time for now
	atomic_t in_use;								// Atomic lock use cmpxchg to acquire
	bool clearing_needed;					        // clear profiler now if possible, else on next un-acquire

	struct nvmeibc_profiler_configuration info;		// This object describes the current profiler, how many profiling points and what are their respective names (STAGE A / TRANSPORT READ / TRAMSPORT UNLOCK SECONDARY) - defined by DP
	char desc[64];									//contains the profiler object description; //expected volume/chunk/raid/sgmnt
	struct nvmeibc_profiling_stats_stage* stats;	// The actual accumulated stats used by this profiler (has buckets and error counts)
													// Some flow profilers may declare 0 stages, for example operation prepare stage
	struct nvmeibc_profiling_stats_stage* end2end_stats;	//Contains operation distribution from the moment profiler was attached to the object until detachment
															//relevant only to flow profilers
};

// Create a profiler for n_stages, include the naming output for each stage profilied
struct nvmeibc_profiler *
nvmeibc_profiling_create(const struct nvmeibc_profiler_configuration boundaries, const char* description);
void nvmeibc_profiling_clear(struct nvmeibc_profiler *prof);

bool nvmeibc_profiling_is_initialized(const struct nvmeibc_profiler* prof);

// Destroy the allocated stats part of the profiler
//void nvmeibc_profiler_destroy(struct nvmeibc_profiler* profiler);

void nvmeibc_profiling_start_take_stats_for_stage(struct nvmeibc_profiler *prof, const void *obj, const u8 stage);
bool nvmeibc_profiling_end_take_stats_for_stage(struct nvmeibc_profiler *prof, const void *obj, const u8 stage, const int rv);

//BE CAREFULL: the following function assumes that all stages executed sequentially, which is incorrect in general
//we define stages per RAID type, stages for read,write & trim are different and some of them are skipped in different operations.
void nvmeibc_profiling_take_stats_for_next_stage(struct nvmeibc_profiler *prof, const void *obj, const u8 stage, const int rv);
void nvmeibc_profiling_end_take_stats(struct nvmeibc_profiler *prof, const void *obj, const int rv);
bool nvmeibc_profiling_start_take_stats(struct nvmeibc_profiler *prof, const void *obj);

// CMD type profiler takes in use if available and starts to profile a specific stage, same thing when ending
void nvmeibc_profiling_end_take_cmd_stats(struct nvmeibc_profiler *prof, const void *obj, const int stage, const int rv);
void nvmeibc_profiling_start_take_cmd_stats(struct nvmeibc_profiler *prof, const void *obj, const int stage);

bool nvmeibc_profiling_end_take_cmd_stats_for_op(  struct nvmeibc_profiler *hdr, struct nvmeibc_profiler *prof, const void *o, const int stage, const void *obj, const int rv);
void nvmeibc_profiling_start_take_cmd_stats_for_op(struct nvmeibc_profiler *hdr, struct nvmeibc_profiler *prof, const void *o, const int stage, const void *obj);

void nvmeibc_profiling_add_retry_count_and_delay(struct nvmeibc_profiler *prof, const ulong delay, const int stage, const u32 retry);

void nvmeibc_profiling_stats_tostring(const struct nvmeibc_profiler *prof, struct nvmeib_txt *txt);
void nvmeibc_profiler_tocsv(          const struct nvmeibc_profiler *prof, struct nvmeib_txt *txt);

int  nvmeibc_profiling_put(struct nvmeibc_profiler *prof);
void nvmeibc_profiling_get(struct nvmeibc_profiler *prof);

#endif  // H beginning

