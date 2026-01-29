/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_io_perm_alert.h"
#include "block/nvmeibc_topology.h"
#include "block/nvmeibc_block_common.h"
#include "nvmeib_utils.h"
#include "nvmeib_consts_shared.h"

#define BUF_ADD(...) pos += scnprintf(buf + pos, buf_len - pos, __VA_ARGS__)

void nvmeibc_cc_api_notify_io_changed(/*const char*/ void *volume_uuid, const struct nvmeibc_cinst_params_main *p); // Do not include nvmeibc_main.h for a single function
static void __notify_io_changed(struct nvmeibc_block_device *dev, enum nvmeibc_io_perm_arm curr){
	(void)curr;
	_NT(t_01_niocscwq, "@DEV_NAME, schedule update, io_per=@X", dev->name, curr);
	nvmeibc_block_set_generic_work_to_main(nvmeibc_cinst_get_blok_p(dev), dev->uuid, nvmeibc_cc_api_notify_io_changed, (void*)dev->uuid, false);
}

#define jfs2secs(jfs) 	((u64)(((nvmeibc_jiffies_t)jfs)/HZ))
#define j2s(jfs) 	    ((u32)(((nvmeibc_jiffies_t)jfs)/HZ))
#define infinite_jiffies       ((nvmeibc_jiffies_t)~0UL)
#define NVMEIBC_IO_PERM_ARM_DEFAULT_STABILIZATION_PERIOD (NVMEIBC_IO_PERM_ARM_DEFAULT_STABILIZATION_PERIOD_SEC*HZ)		// the I/O status is considered stable after this period

static inline const char* nvmeibc_io_perm_arm_tostring(enum nvmeibc_io_perm_arm arm){
	switch (arm) {
		case NVMEIBC_IO_PERM_ARM_DEV_INIT:						return "init";
		case NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC:				return "io-disabled";
		case NVMEIBC_IO_PERM_ARM_ONLY_SYNC:						return "only-sync";
		case NVMEIBC_IO_PERM_ARM_BIO_OK_FIRST: 					return "bio-ok-1st";
		case NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_FIRST: 		return "bio-!p-1st";
		case NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_RO_FIRST:	return "bio-!p-ro-1st";
		case NVMEIBC_IO_PERM_ARM_BIO_OK:						return "bio-ok";
		case NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION:				return "bio-!p";
		case NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_RO:			return "bio-!p-ro";
		case NVMEIBC_IO_PERM_ARM_DETACHING:						return "detaching";
		case NVMEIBC_IO_PERM_ARM_DESTROYED:						return "destroyed";
		default:												return "unknown";
	}
}

// Todo: Unify function below with nvmeibc_io_perm_alert_tostring()
static inline void __t_ioperm_alert(const struct nvmeibc_io_perm_alert *iod, bool stats, bool config, const int line){
	_NT(trace_io_perm_alert, "@LINENO bdev=@BDEV arm=[curr=@STR stable=@STR] last_armed_at=@SECONDS uw2ro=@SECONDS", line, (const void*)iod->bdev_id, nvmeibc_io_perm_arm_tostring(iod->arm), nvmeibc_io_perm_arm_tostring(iod->arm_stable), jfs2secs(iod->last_armed_at), jfs2secs(iod->unprotected_write_to_read_only_at));
	if (stats)
		_NT(trace_io_perm_alert_stats,  "@LINENO bdev=@BDEV {n_stuck=@UINT, long=@UINT[sec]}", line, (const void*)iod->bdev_id, iod->stats.n_stucked_detach_alert_sent, iod->stats.longest_io_problem_duration_msec/1000);
	if (config)
		_NT(trace_io_perm_alert_config, "@LINENO bdev=@BDEV conf={stab=@UINT, unpW=@LLU detStu=@UINT}[sec]", line, (const void*)iod->bdev_id, j2s(iod->config.stabilization_period), jfs2secs(iod->config.unprotected_write_period), j2s(iod->config.stucked_detach_alert_freq));
}

#define __trace_io_perm_alert(iod, stats, config) __t_ioperm_alert(iod, stats, config, __LINE__)

static inline bool __should_send_detaching_alert(struct nvmeibc_io_perm_alert *iod, const nvmeibc_jiffies_t now)
{
	if ((iod->arm == NVMEIBC_IO_PERM_ARM_DETACHING) && iod->config.alert_sender){
		const nvmeibc_jiffies_t longest_period = 600ULL*HZ;
		const nvmeibc_jiffies_t curr_period = min(longest_period, (iod->config.stucked_detach_alert_freq * (iod->stats.n_stucked_detach_alert_sent+1)));
		const nvmeibc_jiffies_t send_at = iod->last_armed_at + curr_period;
		return (send_at <= now);
	}
	return false;
}

void nvmeibc_io_perm_alert_periodic_wakeup(struct nvmeibc_block_device *dev, nvmeibc_jiffies_t now)
{
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;

	if ((iod->arm_stable != iod->arm) && ((iod->last_armed_at + iod->config.stabilization_period) < now)) {
		iod->arm_stable = iod->arm;
		if (iod->config.notify_io_changed)
			iod->config.notify_io_changed(dev, iod->arm_stable);
	}

	if (__should_send_detaching_alert(iod, now)){
		#define MAX_ALLERT_LENGTH (234) // level 8, header 96, message 128, @
		static const char *format = "%cClient Volume %s@Client: %s Volume: %s %s for the past %u.%u[sec]";
		const char *clnt_name = nvmeib_get_utsname_nodename();	// This is an incorrect name (disregards multi instance, we dont care coz it is sent via the correct client instance)
		const char severity = 'E'; // Warning or error
		//const bool should_update_prev_allert = false;
		const u32 duration_msecs = (((now - iod->last_armed_at) * 1000) / HZ);
		const char *problem = (dev->status == NCBD_DETACHING_UPGRD) ? "DetUpg stuck" : "Detach stuck";

		char *msg = kmalloc(MAX_ALLERT_LENGTH, GFP_ATOMIC);
		if (!msg)
			return;

		snprintf(msg, MAX_ALLERT_LENGTH, format, severity,
			/*Header:*/ problem,
			/*Body  :*/ clnt_name, dev->name, problem,
						duration_msecs/1000, (duration_msecs%1000)/100);
		iod->config.alert_sender(dev, msg);//, iod->last_armed_at, should_update_prev_allert);
		iod->stats.n_stucked_detach_alert_sent++;
	}
}

unsigned long nvmeibc_unprotected_write_period_seconds = jfs2secs(infinite_jiffies);		// Talekd with Josh, 2019/04, Said it is 10[min]
module_param_named(unprotected_write_period_seconds, nvmeibc_unprotected_write_period_seconds, ulong, 0644);
MODULE_PARM_DESC(unprotected_write_period_seconds, "Timeout in seconds for an unprotected volume until it is become read only");

#define __unprotected_countdown_clear(iod)      ({(iod)->unprotected_write_to_read_only_at = 0; })
#define __unprotected_countdown_start(iod, now) ({(iod)->unprotected_write_to_read_only_at = ((now) + (iod)->config.unprotected_write_period);})
#define __unprotected_countdown_is_ticking(iod) 	((iod)->unprotected_write_to_read_only_at != 0)
#define __unprotected_countdown_is_finished(iod)  (((iod)->config.unprotected_write_period == 0) || \
												   (__unprotected_countdown_is_ticking(iod) && ((iod)->unprotected_write_to_read_only_at <= now)))

void nvmeibc_io_perm_alert_set_unprotect_period(struct nvmeibc_io_perm_alert *iod, int sec)
{
	iod->config.unprotected_write_period = HZ *
		(((sec < 0) ? nvmeibc_unprotected_write_period_seconds : (ulong)sec));	// Override with value (attach/ioctl), or set default
	if (__unprotected_countdown_is_ticking(iod))		// If read_only was ticking - just update its expiration with new period. If was not ticking,, do nothing
		__unprotected_countdown_start(iod, jiffies);
  	__trace_io_perm_alert(iod, false, true);
}

nvmeibc_jiffies_t nvmeibc_io_perm_alert_create( struct nvmeibc_io_perm_alert *iod, u64 bdev_id)
{
	const nvmeibc_jiffies_t now = jiffies;

	memset(iod, 0, sizeof(*iod));
	iod->bdev_id = bdev_id;
	iod->config.stabilization_period = NVMEIBC_IO_PERM_ARM_DEFAULT_STABILIZATION_PERIOD;
	iod->config.alert_sender = nvmeibc_block_send_mgmt_allert;
	iod->config.notify_io_changed = __notify_io_changed;
	nvmeibc_io_perm_alert_set_unprotect_period(iod, -1);			// Upon attach set default value (-1) or 0[sec]. JOSH 2019/04 not sure if on attach should set timeout for read only on 0 or not: EC-2227
	nvmeibc_io_perm_alert_set_stucked_detach_alert_freq(iod, 10);	// 10[sec]

	iod->arm_stable = iod->arm = NVMEIBC_IO_PERM_ARM_DEV_INIT;
	iod->last_armed_at = now;

	__trace_io_perm_alert(iod, true, true);
	return now;
}

void nvmeibc_io_perm_alert_clear_stats(struct nvmeibc_io_perm_alert *iod)
{
	iod->stats.longest_io_problem_duration_msec = 0;
}

void nvmeibc_io_perm_alert_destroy(struct nvmeibc_io_perm_alert *iod)
{
  	__trace_io_perm_alert(iod, true, true);
	iod->arm = NVMEIBC_IO_PERM_ARM_DESTROYED;
	iod->last_armed_at = jiffies;
}

nvmeibc_jiffies_t nvmeibc_io_perm_alert_switch(struct nvmeibc_io_perm_alert *iod, enum nvmeibc_io_perm_arm next)
{
	const nvmeibc_jiffies_t now = jiffies;
	const enum nvmeibc_io_perm_arm prev =            iod->arm;
	const nvmeibc_jiffies_t        prev_last_armed = iod->last_armed_at;

	if (prev == next)
		return now; // No change

  	__trace_io_perm_alert(iod, false, false);

	iod->arm = next;
	iod->last_armed_at = now;

	BUG_ON(prev == NVMEIBC_IO_PERM_ARM_DESTROYED); // use after free?
	BUG_ON(next == NVMEIBC_IO_PERM_ARM_DEV_INIT);  // init set on create

	if (false == nvmeibc_io_perm_arm_has_bio(prev) &&
		true  == nvmeibc_io_perm_arm_has_bio(next)) {
		const u32 duration_msecs = (((now - prev_last_armed) * 1000) / HZ);
		MAX_WITH(iod->stats.longest_io_problem_duration_msec, duration_msecs);
	}

	if (nvmeibc_io_perm_arm_has_bio(iod->arm)){
		BUG_ON(prev == NVMEIBC_IO_PERM_ARM_DETACHING);
		if (nvmeibc_io_perm_arm_has_bio_with_protection(next)){
			__unprotected_countdown_clear(iod);
		} else {
			if (!__unprotected_countdown_is_ticking(iod))
				__unprotected_countdown_start(iod, now);
		}
	} else {
		if (next == NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC){
			//we may decide here what to do with unprotected write <=> io disabled triggerig, setting unprotected_write_to_read_only_at to 0 will restart unprotected write period, leaving the same value and returing to unprotected write may trigger "read only" volume sooner
		} else {
			__unprotected_countdown_clear(iod);
		}
	}

	__trace_io_perm_alert(iod, false, false);
	return now;
}

bool nvmeibc_io_perm_alert_should_create_readonly_topo(const struct nvmeibc_io_perm_alert *iod, nvmeibc_jiffies_t now)
{
	if (nvmeibc_io_perm_arm_has_bio_with_protection(iod->arm))
		return false;
	if (iod->config.unprotected_write_period == (HZ*jfs2secs(infinite_jiffies)))	// Max value means never
		return false;
	if (__unprotected_countdown_is_finished(iod)) {
		__trace_io_perm_alert(iod, false, false);
		return true;
	}
	return false;
}

void nvmeibc_io_perm_alert_set_stucked_detach_alert_freq(struct nvmeibc_io_perm_alert *iod, u32 freq)
{
	if (freq > 0)
		iod->config.stucked_detach_alert_freq = freq*HZ;
}

int nvmeibc_io_perm_alert_tostring(const struct nvmeibc_io_perm_alert *_iod, char *buf, int buf_len, char fmt)
{
	struct nvmeibc_io_perm_alert iod = *_iod;	// Copy to stack to prevent change
	const char *cur = nvmeibc_io_perm_arm_tostring(iod.arm), *stbl = nvmeibc_io_perm_arm_tostring(iod.arm_stable);
	int pos = 0;
	if (fmt == 'H') {
		BUF_ADD("MgmtAlert: {arm=%s stable=%s}", cur, stbl);
		BUF_ADD(" last_armed: {at=%llu, uw2ro=%llu}[sec]", jfs2secs(iod.last_armed_at), jfs2secs(iod.unprotected_write_to_read_only_at));
		BUF_ADD(" {n=%u, long=%u[sec]}", iod.stats.n_stucked_detach_alert_sent, iod.stats.longest_io_problem_duration_msec/1000);
		BUF_ADD(" conf={stab=%u, unpW=%llu, detStu=%u}[sec]\n", j2s(iod.config.stabilization_period), jfs2secs(iod.config.unprotected_write_period), j2s(iod.config.stucked_detach_alert_freq));
	} else {
		BUF_ADD("\"mgmt_alerts\": {\"armed\": {\"current\":\"%s\", \"stable\":\"%s\"},", cur, stbl);
		BUF_ADD("\"last_armed_sec\": {\"at\":%llu, \"unpw2ro\":%llu},", jfs2secs(iod.last_armed_at), jfs2secs(iod.unprotected_write_to_read_only_at));
		BUF_ADD("\"stats\": {\"n_sent\":%u, \"longest_sec\":%u},", iod.stats.n_stucked_detach_alert_sent, iod.stats.longest_io_problem_duration_msec/1000);
		BUF_ADD("\"conf_sec\": {\"stab\":%u, \"unpW\":%llu, \"det_stuck\":%u}}", j2s(iod.config.stabilization_period), jfs2secs(iod.config.unprotected_write_period), j2s(iod.config.stucked_detach_alert_freq));
	}
	return pos;
}
