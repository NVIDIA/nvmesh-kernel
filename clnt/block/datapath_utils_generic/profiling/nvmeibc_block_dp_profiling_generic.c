/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_profiling_generic.h"
#include "block/nvmeibc_block_common.h"
#include "nvmeib_macro_utils.h"

extern bool profiling_enabled;			// Module param

#define BUF_ADD(...) pos += scnprintf(buf + pos, len - pos, __VA_ARGS__)
/*************** Real-time Datapath statistics of slow IO's **************/
void topo_stats_t_clear(struct topo_stats_t *t) {
	memset(t,0, sizeof(*t));
}

void slow_io_stats_t_init( struct slow_io_stats_t *t)
{
	slow_io_stats_t_clear(t);
}

void slow_io_stats_t_clear(struct slow_io_stats_t *t)
{
	memset(t,0, sizeof(*t));
}

#define SLOW_LOCK_TIME_MSEC  (2000)						// 2[sec]
int slow_io_stats_t_tostring(const struct slow_io_stats_t *_t, char *buf, int len, char fmt)
{
	struct slow_io_stats_t t = *_t;	// Snapshot, coz it changes in real time
	struct slow_io_stats_lock_retry_t *o = &t.over_retried;
	int pos = 0;
	if (fmt == 'H') {
		BUF_ADD("Slow Locks %d[msec]: num_locks=%u, ", SLOW_LOCK_TIME_MSEC, atomic_read(&o->num_l));
		BUF_ADD("last={lock_id=0x%x n_tries=%u, n_msec=%u, op=%s, dlba[blkst]=0x%llx, ", o->lockid, o->n_retries, o->n_msecs, nvmeibc_rdma_intent_to_string(o->intent), o->dlba_blkset);
		BUF_ADD("seg=(%d,%d,%d)}", o->ci, o->ri, o->si);
		#ifdef DEBUG_LOCK_RETRY
			BUF_ADD(", pending_retry{RD=%u/WR=%u}", atomic_read(&t.pend_retry_rdl), atomic_read(&t.pend_retry_wrl));
		#endif
		BUF_ADD("\n");
	} else {
		BUF_ADD("\"slow_locks\": {\"t_msec\": %d, \"num_locks\": %d", SLOW_LOCK_TIME_MSEC, atomic_read(&o->num_l));
		BUF_ADD(", \"last\":{\"lock_id\": \"0x%x\", \"n_tries\": %u, \"n_msec\": %u, \"op\": \"%s\", \"dlba_blksts\": \"0x%llx\"", o->lockid, o->n_retries, o->n_msecs, nvmeibc_rdma_intent_to_string(o->intent), o->dlba_blkset);
		BUF_ADD(", \"seg\":{\"ci\": %u, \"ri\": %u, \"si\": %u}}}",  o->ci, o->ri, o->si);
	}
	return pos;
}

void slow_io_stats_t_log_over_retry(struct slow_io_stats_t *_t, const struct nvmeibc_cmd_lock *l, u64 lockid)
{
	const u32 n_msecs = jiffies_to_msecs(jiffies - l->last_retry_report_time);
	if (n_msecs > SLOW_LOCK_TIME_MSEC) {
		struct slow_io_stats_lock_retry_t *o = &_t->over_retried;	// The update of 'o' is not atomic. Who cares, it is used for runtime analysis only.
		const struct nvmeibc_subscription_ctx *tr = l->ds->toma_reg;
		atomic_inc(&o->num_l);
		o->lockid = (u32)lockid;
		o->dlba_blkset = (l->address/LOCKSET_4KS);
		o->ci = (u16)tr->ch;
		o->ri = (u16)tr->r1;
		o->si = (u16)tr->seg;
		o->n_retries = l->retries;
		o->intent = l->type;
		o->n_msecs = n_msecs;
	}
}

/********************** Debugging functions and utils ************************/
/*
 * IO profiling mechanism
 * This mechanism is meant for profiling IO flow which is fast, i.e.: requires
 * minimal overhead, accurate measurement & minimal output per IO (so not to
 * overflow syslog).
 * to enable it in build, define the below guard macro in the Makefile.
 * to enable on the machine, set module param /sys/module/nvmeibc/parameters/profiling_enabled to 'Y'
 * to profile a new IO flow:
 * 1) use the IO_FLOW_PROF_DECLARE() macro to declare gathering object for the new flow
 * 2) embed calls to the event point API's in the profield code.
 *
 * TODO(EBA):
 * 1) with multiple profiling flows in the code base, we'll need a way to enable a subset that we need rather than all.
 */

#if defined(BLKDEV_PROFILING)
static __inline__ u8 __find_bucket(u64 tsc){
	u8 result = 0;
	tsc >>= 10; //2**10 == 1024, I assume that dropping 3 digits from the end brings me from partial nanosecond resolution to microseconds
				//obviously it is not 100% math correctly, but it is good enough and cheap
	if (likely(tsc)){
		// fls (find last set bit (MSB)) is exactly clz (count leading zeros) and retruns the location of the last set bit == number of leading zeros (0100 == 3, 1000 == 4, 0010 == 2, 0001 == 1, 0111 == 3)
		result = fls64(tsc);
		result = min(result, (u8)(PROFILING_INTERVALS_COUNT-1));
	}
	return result;
}

static inline void __end_measureq(struct nvmeibc_profiling_stats_stage *stg, const int rv)
{
	const u64 delta_time = nvmeib_public_rdtsc() - stg->started;
	stg->count++;
	stg->exact_time += delta_time;
	if (rv) {
		stg->error_count++;
		stg->error_time += delta_time;
	}
	++stg->buckets[__find_bucket(delta_time)];
}

static int __stage_stats_tostring(const struct nvmeibc_profiling_stats_stage* stg, const char* stg_name, char *buf, int len)
{
	int pos = 0, i = 0;
	const int factor = loops_per_jiffy * HZ / 1000000;
	const u64 avg0     = stg->count       ? DIV_ROUND_CLOSEST(stg->exact_time, stg->count      ) : 0;
	const u64 avg0_err = stg->error_count ? DIV_ROUND_CLOSEST(stg->error_time, stg->error_count) : 0;
	BUF_ADD("Stage %s: count=%u, average=%llu [usec], error count=%u, error average=%llu [usec]\n",
		stg_name, stg->count, avg0/factor, stg->error_count, avg0_err/factor);
	BUF_ADD("Retries:\n");
	for (i=0;i<PROFILING_RETRY_COUNT;i++)       BUF_ADD("Retry %d: retry count=%u, retry time=%llu [usec]\n", i+1, stg->retry[i], stg->retry_time[i]);
	BUF_ADD("Bucket info:\n");
	for (i=0;i<PROFILING_INTERVALS_COUNT;i++) BUF_ADD("Bucket %d: %llu\n", 1 << i, stg->buckets[i]);
	return pos;
}
#endif/*BLKDEV_PROFILING*/


int nvmeibc_profiling_stats_tostring(const struct nvmeibc_profiler *prof, char *buf, int len)
{
	#if defined(BLKDEV_PROFILING)
	int pos = 0;
	if (prof){
		int stg;
		int n_stages = prof->info.translate(PROFILING_GET_N_STAGES);
		BUF_ADD("%s Profiler:\n", prof->desc);
		if (prof->end2end_stats){
			pos += __stage_stats_tostring(prof->end2end_stats, "end2end", buf+pos, len-pos);
		}
		for (stg = 0; stg < n_stages; ++stg){
			pos += __stage_stats_tostring(&prof->stats[stg], prof->info.stage2name(stg), buf+pos, len-pos);
		}
	}
	return pos;
	#else
	(void)prof; (void)buf; (void)len; return 0;
	#endif/*BLKDEV_PROFILING*/
}

#if defined(BLKDEV_PROFILING)
static int __profiler_stage_stats_tocsv(struct nvmeibc_profiling_stats_stage* stg, const char* stg_name, char *buf, int len)
{
	int pos = 0, i = 0;
	const int factor = loops_per_jiffy * HZ / 1000000;
	const u64 avg0     = stg->count       ? DIV_ROUND_CLOSEST(stg->exact_time, stg->count      ) : 0;
	const u64 avg0_err = stg->error_count ? DIV_ROUND_CLOSEST(stg->error_time, stg->error_count) : 0;
	BUF_ADD("stage_name,count,average,error_count,error_average");
	for (i=0;i<PROFILING_INTERVALS_COUNT;i++) BUF_ADD(",bucket%dusec", 1 << i);
	for (i=0;i<PROFILING_RETRY_COUNT;i++)     BUF_ADD(",%d_retry,%d_total_time", i+1,i+1);
	BUF_ADD("\n");
	BUF_ADD("%s,%u,%llu,%u,%llu", stg_name,stg->count,avg0/factor,stg->error_count,avg0_err/factor);
	for (i=0;i<PROFILING_INTERVALS_COUNT;i++)  BUF_ADD(",%llu", stg->buckets[i]);
	for (i=0;i<PROFILING_RETRY_COUNT;i++)      BUF_ADD(",%u,%llu", stg->retry[i], stg->retry_time[i]);
	BUF_ADD("\n");
	return pos;
}
#endif/*BLKDEV_PROFILING*/

int nvmeibc_profiler_tocsv(const struct nvmeibc_profiler *prof, char *buf, int len)
{
	#if defined(BLKDEV_PROFILING)
	int pos = 0;
	if (prof){
		int stage;
		const int n_stages = prof->info.translate(PROFILING_GET_N_STAGES);
		BUF_ADD("profiler_description,number_of_stages\n%s,%d\n", prof->desc, n_stages + ((prof->end2end_stats) ? 1 : 0));
		if (prof->end2end_stats) {
			pos += __profiler_stage_stats_tocsv(prof->end2end_stats, "end2end", buf+pos,len-pos);
		}
		for (stage=0;stage<n_stages;stage++) {
			pos += __profiler_stage_stats_tocsv(&prof->stats[stage], prof->info.stage2name(stage), buf+pos, len-pos);
		}
	}
	return pos;
	#else
	(void)prof; (void)buf; (void)len; return 0;
	#endif/*BLKDEV_PROFILING*/
}

static  __attribute__ ((unused)) u8 __stage2index(struct nvmeibc_profiler *prof, const u8 stage)
{
	const u8 idx = prof->info.translate(stage);
	const u8 n_stages = prof->info.translate(PROFILING_GET_N_STAGES);
	BUG_ON(n_stages <= idx);
	return idx;
}


void nvmeibc_profiling_start_take_stats_for_stage(struct nvmeibc_profiler *prof, const void *obj, const u8 stage)
{
	#if defined(BLKDEV_PROFILING)
	if ((prof == NULL) || (obj == NULL) || (!profiling_enabled) || (prof->stats_object != obj))
		return;
	{
		const u8 idx = __stage2index(prof, stage);
		prof->stats[idx].started = nvmeib_public_rdtsc();
	}
	#else
	(void)prof; (void)obj; (void)stage;
	#endif/*BLKDEV_PROFILING*/
}

bool nvmeibc_profiling_end_take_stats_for_stage(struct nvmeibc_profiler *prof, const void *obj, const u8 stage, const int rv)
{
	#if defined(BLKDEV_PROFILING)
	if ((prof == NULL) || (prof->stats_object == NULL)||(obj == NULL)||(!profiling_enabled)||(prof->stats_object != obj))
		return false;
	{
		const u8 index = __stage2index(prof, stage);
		__end_measureq(&prof->stats[index], rv);
		return true;
	}
	#else
	(void)prof; (void)obj; (void)stage; (void)rv;
	#endif/*BLKDEV_PROFILING*/
	return false;
}

void nvmeibc_profiling_take_stats_for_next_stage(struct nvmeibc_profiler *prof, const void *obj, const u8 stage, const int rv)
{
	#if defined(BLKDEV_PROFILING)
	if (nvmeibc_profiling_end_take_stats_for_stage(  prof, obj, stage, rv))
		nvmeibc_profiling_start_take_stats_for_stage(prof, obj, stage+1);
	#else
	(void)prof; (void)obj; (void)stage; (void)rv;
	#endif/*BLKDEV_PROFILING*/
}

#if defined(BLKDEV_PROFILING)
static inline void __profiler_check_and_clear(struct nvmeibc_profiler *prof)
{
	if (prof->clearing_needed) {
		struct nvmeibc_profiler_configuration info = prof->info;
		const int n_stages = info.translate(PROFILING_GET_N_STAGES);
		const u32 stage_size = sizeof(struct nvmeibc_profiling_stats_stage);
		const bool is_flow_profiler = (bool)(info.type == NVMEIBC_PROFILER_TYPE_FLOW);
		if (n_stages) {
			memset(prof->stats,         0, stage_size * n_stages);
		}
		if (is_flow_profiler) {
			memset(prof->end2end_stats, 0, stage_size);
		}
		prof->clearing_needed = false;
	}
}
#endif/*BLKDEV_PROFILING*/

void nvmeibc_profiling_end_take_stats(struct nvmeibc_profiler *prof, const void *obj, const int rv)
{
	#if defined(BLKDEV_PROFILING)
	if ((prof == NULL) || (prof->stats_object == NULL)||(obj == NULL)||(!profiling_enabled)||(prof->stats_object != obj))
		return;
	if (prof->end2end_stats){
		__end_measureq(prof->end2end_stats, rv);
	}
	__profiler_check_and_clear(prof);
	prof->stats_object = NULL;
	atomic_set(&prof->in_use, 0);
	#else
	(void)prof; (void)obj; (void)rv;
	#endif/*BLKDEV_PROFILING*/
}

// Starts stage 0 if profiling is available
bool nvmeibc_profiling_start_take_stats(struct nvmeibc_profiler *prof, const void *obj)
{
	#if defined(BLKDEV_PROFILING)
	if ((prof == NULL) || (obj == NULL) || !profiling_enabled || !prof->info.stage2name)
		return false;
	// We are going to sample and not take every entry and exit.
	if (atomic_cmpxchg(&prof->in_use, 0, 1) == 0) {
		prof->stats_object = (void*)obj;
		if (prof->end2end_stats){
			prof->end2end_stats->started = nvmeib_public_rdtsc();
		}
		return true;
	}
	//else warn on if profiler is in use for too long;
	#else
	(void)prof; (void)obj;
	#endif/*BLKDEV_PROFILING*/
	return false;
}

void nvmeibc_profiling_end_take_cmd_stats(struct nvmeibc_profiler *prof, const void *obj, const int stage, const int rv)
{
	#if defined(BLKDEV_PROFILING)
	if ((prof == NULL) || (prof->stats_object == NULL)||(obj == NULL)||(!profiling_enabled)||(prof->stats_object != obj))
		return;
	{
		const u8 idx = __stage2index(prof, stage);
		__end_measureq(&prof->stats[idx], rv);
		prof->stats_object = NULL;
		atomic_set(&prof->in_use, 0);
		return;
	}
	#else
	(void)prof; (void)obj; (void)stage; (void)rv;
	#endif/*BLKDEV_PROFILING*/
	return;
}

void nvmeibc_profiling_start_take_cmd_stats(struct nvmeibc_profiler *prof, const void *obj, const int stage)
{
	#if defined(BLKDEV_PROFILING)
	if ((prof == NULL) || (obj == NULL) || !profiling_enabled || !prof->info.stage2name)
		return;
	// We are going to sample and not take every entry and exit.
	if (atomic_cmpxchg(&prof->in_use, 0, 1) == 0) {
		const u8 idx = __stage2index(prof, stage);
		prof->stats_object = (void*)obj;
		if (prof->end2end_stats){
			prof->end2end_stats->started = nvmeib_public_rdtsc();
			BUG(); // REMOVE when
		}
		prof->stats[idx].started = nvmeib_public_rdtsc();
		return;
	}
	//else warn on if profiler is in use for too long;
	#else
	(void)prof; (void)obj; (void)stage;
	#endif/*BLKDEV_PROFILING*/
	return;
}

/* Disk an lock operations can proile more samples than the good path profiler.
   However that causes a distortion in analyzing the profiler. Using the op profiler
   we use the good path profiler as a preliminary requirement before using the disk
   or lock profilers, this will allow each stage to be profiled the same amount of times.
   The only exception to this is the preparation profiler which doesn't have the good path available (and is not raid specific)

   hdr - > good path raid profiler
   prof - > disk/lock segment specific profiler
   o - > operation we are attempting to profile
   obj - > Either locksets for lock operation or rldr for disk operations
   */
void nvmeibc_profiling_start_take_cmd_stats_for_op(struct nvmeibc_profiler *hdr, struct nvmeibc_profiler *prof, const void *o, const int stage, const void *obj)
{
	#if defined(BLKDEV_PROFILING)
	if ((hdr == NULL) || (o == NULL) || (prof == NULL) || !profiling_enabled || !prof->info.stage2name || (obj==NULL) || (hdr->stats_object != o))
		return;
	// We are going to sample and not take every entry and exit.
	if (atomic_cmpxchg(&prof->in_use, 0, 1) == 0) {
		const u8 idx = __stage2index(prof, stage);
		prof->stats_object = (void*)obj;
		prof->stats[idx].started = nvmeib_public_rdtsc();
		return;
	}
	//else warn on if profiler is in use for too long;
	#else
	(void)prof; (void)obj; (void)stage; (void)hdr; (void)o;
	#endif/*BLKDEV_PROFILING*/
	return;
}

bool nvmeibc_profiling_end_take_cmd_stats_for_op(struct nvmeibc_profiler *hdr, struct nvmeibc_profiler *prof, const void *o, const int stage, const void *obj, const int rv)
{
	#if defined(BLKDEV_PROFILING)
	if ((hdr == NULL)||(hdr->stats_object == NULL)||(o == NULL)||(obj==NULL)||(!profiling_enabled)||(hdr->stats_object != o)||(prof==NULL)||(prof->stats_object != obj))
		return false;
	{
		const u8 idx = __stage2index(prof, stage);
		//unitest_print("Lock obj relea %p\n", obj2);
		__end_measureq(&prof->stats[idx], rv);
		__profiler_check_and_clear(prof);
		prof->stats_object = NULL;
		atomic_set(&prof->in_use, 0);
		return true;
	}
	#else
	(void)prof; (void)obj; (void)stage; (void)rv; (void)hdr; (void)o;
	#endif/*BLKDEV_PROFILING*/
	return false;
}

void nvmeibc_profiling_add_retry_count_and_delay(struct nvmeibc_profiler *prof, const ulong delay, const int stage, const u32 retries)
{
	#if defined(BLKDEV_PROFILING)
	if ((prof == NULL) || !profiling_enabled || !prof->info.stage2name)
		return;
	{
		const u8 idx = __stage2index(prof, stage);
		const u8 rt_index = min(retries, (u32)(PROFILING_RETRY_COUNT - 1));
		prof->stats[idx].retry[rt_index]++;
		prof->stats[idx].retry_time[rt_index] += delay;
		return;
	}
	#else
	(void)prof; (void)delay; (void)stage; (void)retries;
	#endif/*BLKDEV_PROFILING*/
	return;
}

struct nvmeibc_profiler *
nvmeibc_profiling_create(const struct nvmeibc_profiler_configuration info, const char* desc)
{
	#if defined(BLKDEV_PROFILING)
		const u32 stage_size = sizeof(struct nvmeibc_profiling_stats_stage);
		const bool is_flow_profiler = (bool)(info.type == NVMEIBC_PROFILER_TYPE_FLOW);
		const int n_stages = info.translate(PROFILING_GET_N_STAGES);
		struct nvmeibc_profiling_stats_stage* stages = n_stages ? kzalloc(stage_size*n_stages, GFP_NOWAIT) : NULL;
		struct nvmeibc_profiling_stats_stage* end2end_stats = is_flow_profiler ? kzalloc(stage_size, GFP_NOWAIT) : NULL;
		struct nvmeibc_profiler *rv = kzalloc(sizeof(*rv), GFP_NOWAIT);
		const bool failed_init = (n_stages && !stages) || (is_flow_profiler && !end2end_stats) || (rv == NULL);
	#else
		struct nvmeibc_profiling_stats_stage* stages = 0;
		struct nvmeibc_profiling_stats_stage* end2end_stats = 0;
		const bool failed_init = true;
		struct nvmeibc_profiler *rv = NULL;
	#endif/*BLKDEV_PROFILING*/

	if (!failed_init) {
		atomic_set(&rv->in_use, 0);
		rv->info = info;
		rv->stats = stages;
		rv->end2end_stats = end2end_stats;
		kref_init(&rv->num_users);
		strlcpy(rv->desc, desc, ARRAY_MEM_SIZE(rv->desc));
	} else {
		kfree(stages);
		kfree(end2end_stats);
		kfree(rv);
		rv = NULL;
	}

	#if defined(BLKDEV_PROFILING)
	if (unlikely(failed_init)){
		_NT(nvmeibc_profiling_create, "failed to allocate enough memory for profiling; profiling is disabled");
	}
	#endif/*BLKDEV_PROFILING*/
	return rv;
}

void nvmeibc_profiling_clear(struct nvmeibc_profiler *prof)
{
	#if defined(BLKDEV_PROFILING)
	prof->clearing_needed = true;
	if (atomic_cmpxchg(&prof->in_use, 0, 1) == 0) {
		__profiler_check_and_clear(prof);
		atomic_set(&prof->in_use, 0);
	}
	#else
	(void)prof;
	#endif/*BLKDEV_PROFILING*/
}

bool nvmeibc_profiling_is_initialized(const struct nvmeibc_profiler* prof){
	return (prof != NULL);
}

static void nvmeibc_profiler_destroy(struct nvmeibc_profiler* profiler)
{
	const int in_use = atomic_read(&profiler->in_use);
	if (profiler->stats_object || in_use) {
		const char *err_fmt = ((profiling_enabled) ? "tracking is still active" : "disabled during io");
		WARN(true, "nvmeibc: wrong profiler usage: %s, desc=%s, ptr=%p, in_use=%d\n", err_fmt, profiler->desc, profiler->stats_object, in_use);
	}
	kfree(profiler->stats);
	kfree(profiler->end2end_stats);
	kfree(profiler);
}

static void __on_last_profiler_finish(struct kref* num_users)
{
	struct nvmeibc_profiler *prof = container_of(num_users, struct nvmeibc_profiler, num_users);
	nvmeibc_profiler_destroy(prof);
}

int nvmeibc_profiling_put(struct nvmeibc_profiler *prof)
{
	if (prof) {
		return kref_put(&prof->num_users, __on_last_profiler_finish);
	} else // Simulate that it was freed
		return 1;
}

void nvmeibc_profiling_get(struct nvmeibc_profiler *prof)
{
	if (prof) {
		kref_get(&prof->num_users);
	}
}


