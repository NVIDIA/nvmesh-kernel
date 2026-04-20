/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeib_event.h"
#include "nvmeibc_mcs_stub.h"
#include "nvmeibc_volume.h"
#include "block/nvmeibc_block_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "nvmeibc_disk_hooks.h"
#include "tpv/nvmeibc_tpv.h"    /* nvmeibc_tpv_handle_cdv_preempted */
#include "nvmeibc_icore_ops.h"
#include "block/nvmeibc_block_api_conf.h"
#include "nvmeibc_main.h"		// Configuration/cci_api/self_detach etc
#include "nvmeibc_common.h"
#include "common/proc_epilog.h"
#include "common/nvmeib_cpu_masks.h"

#ifdef KR_UNDEF_H
#	undef KR_UNDEF_H
#endif
#include "kr_undef.h"

#if NVMEIBC_SECTOR_SHIFT != PAGE_SHIFT
	#if NVMEIBC_SECTOR_SHIFT>PAGE_SHIFT
		#error Wrong compilation flags: NVMEIBC_SECTOR_SHIFT>PAGE_SHIFT
	#endif
	#if NVMEIBC_SECTOR_SIZE<512
		#error Wrong compilation flags: NVMEIBC_SECTOR_SIZE<512
	#endif
#endif

/* Todo: Convert this to configuration parameters of block device, EXC-1602*/
#define IO_TIME_OUT_ATTACH (30) // Blocking IO during attach at most 30[sec]
#define IO_TIME_OUT_NORMAL ((unsigned long)(1 << 20)) // Virtually infinite

/*****************************************************************************/
/* WARNING: A lock or command can be sent prematurally if it was delayed not by
 * the self disk. Disregarding such cases.
 * Avoid this holding the cont()'s calls until all the relevant disks are
 * back.
 */
int nvmeibc_block_cont(struct nvmeibc_block_device *nd, struct nvmeibc_disk *d)
{
	if (nd) {
		_NI(t_09_blk_cont, "@NDU @DEV_NAME: CONT disk @D_FULL_NAME (disk=@DISK)", 0, nd->name, d->full_name, d);
		if (unlikely(nvmeibc_block_status_is_detaching(nd->status))) {
			_NT(t_0a_blk_cont, "@DEV_NAME: CONT ignorred, detaching...", nd->name);
		} else {
			nvmeibc_topology_cont(&nd->topologies, d);
		}
	}
	return 0;
}

int nvmeibc_block_pause(struct nvmeibc_block_device *nd, struct nvmeibc_disk *d)
{
	NFIN;
	if (nd) {
		_NT(trace_block_nvmeibc_block_pause, "@DEV_NAME: PAUSE disk @D_FULL_NAME (disk=@DISK)", nd->name, d->full_name, d);
		nvmeibc_topology_pause(&nd->topologies, d);
	}
	NFOUT;
	return 0;
}

#include "nvmeib_nvme.h"
static int block_invoke_disk_pause_on_catastrophic_nvme_errors(int nvme_err, struct nvmeibc_disk *disk)
{
	if (nvme_err == NVME_SC_POWER_LOSS) {
	    //(nvme_err == NVME_SC_ABORT_QUEUE)		// Daniel: Dont include that
		nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_NVME_SC_POWER_LOSS);
		return -EIO;							// Upper layer should not know about those catastrophic errors, they were already handled
	}
	return nvme_err;
}

#include "nvmeibc_pausable.h"
#include "../common/nvmeib_completion_noise.h"

void nvmeibc_block_completion(struct nvmeibc_d_iocmd_comp *comp)
{
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(comp);
	struct operation *o = cmd->o;
	struct nvmeibc_disk_command *disk_cmd = &cmd->iocmd->disk_cmd;
	struct nvmeibc_disk *disk = cmd->ds->disk;

	if (unlikely(comp->comp_code)) {
		int comp_code = comp->comp_code;
		NVMEIB_LOG_GOODPATH("{@O_DBG_ID} @CORE_BLOCK_COMPLETION_POST_FAIL", _W, goodpath_nvmeibc_transport, trace_nvmeibc_block_completion_error, o->dbg_id, (u64)cmd->iocmd, comp_code);
		_NT(error_block_nvmeibc_block_completion, "Error comp: op=@BLOCK_IO_OP rid=@RID operation=@OPERATION b=@BIO command=@COMMAND l=@LOCKSETS d=@DISK rv=@RV", o->op, cmd->iocmd->req_id, o, &o->bios[0], o->cmds, o->locks, disk, comp_code);
		if (comp_code > 0) {							// NVME error code
			comp_code = nvmeib_error_code_refine(comp_code);
			block_invoke_disk_pause_on_catastrophic_nvme_errors(comp_code, disk);
			comp->comp_code = comp_code;
		}
	} else { /* Most of the times comp code is zero, so we don't need it - save space */
		nvmeibc_disk_cmd_piggyback_lock_read_poison_verify(cmd->iocmd);
		NVMEIB_LOG_GOODPATH("{@O_DBG_ID} @CORE_BLOCK_COMPLETION_POST_OK", _I, goodpath_nvmeibc_transport,trace_nvmeibc_block_completion, o->dbg_id, (u64)cmd->iocmd);
	}

	DEBUG_TRANSFERS_detect_double_callback(comp);
	nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
	nvmeibc_pd_cb_called_cmd(disk, disk_cmd);
	
	o->nd->dp.cmd_comp_cb(comp);
}

void nvmeibc_block_comp_gencmd(struct nvmeibc_d_iocmd_comp *comp)
{
	struct nvmeibc_block_command *cmd = comp->cmd;
	struct nvmeibc_disk_command *disk_cmd = &cmd->gen_cmd->disk_cmd;
	DEBUG_TRANSFERS_detect_double_callback(comp);
	nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
	nvmeibc_pd_cb_called_cmd(cmd->ds->disk, disk_cmd);
	on_disk_hook(nvmeibc_block_comp_gencmd, cmd->ds->disk, before_gen_cmd_comp_cb, comp->cmd->gen_cmd);
	cmd->o->nd->dp.cmd_comp_cb(comp);
}

#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_sub_block_utils.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_func.h"

static enum nvmeibc_io_perm_arm __convert_io_type_permission_to_alert(enum nvmeib_io_type_permission io_perm, bool first_time){
	const struct { enum nvmeib_io_type_permission io_perm; } t = { .io_perm = io_perm };

	if (nvmeibc_topo_is_in_err_state(&t))
		return NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC;				// Dont care which error caused this
	if (nvmeibc_topo_is_partially_ok(&t)) {
		const enum nvmeibc_io_perm_arm f = (first_time ? NVMEIBC_IO_PERM_ARM_FIRST_TIME_BIT : 0);	// Set first_time marker
		if (io_perm == NVMEIB_IO_TYPE_PERMIT_RDONLY)            return f|NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_RO;
		if (io_perm == NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION) return f|NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION;
		if (io_perm == NVMEIB_IO_TYPE_PERMIT_ALL)               return f|NVMEIBC_IO_PERM_ARM_BIO_OK;
		WARN(true, "invalid io_perm=%d", (int)io_perm);			// Should never happen!
		return NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC;
	} else {
		return NVMEIBC_IO_PERM_ARM_ONLY_SYNC;					// Dont care what caused this (cold recovery, preemption, etc)
	}
}

static void __notify_all_riders_about_io_perm_change(struct nvmeibc_block_device *nd)
{
	if ((nvmeibc_block_is_d_carrier(nd)) && (nd->c_d_api.on_io_perm_change_cb))
		nd->c_d_api.on_io_perm_change_cb(&nd->c_d_api);
}

static inline const char* __get_reason_for_io_disable(const struct nvmeibc_block_device *nd, struct nvmeibc_topologies *nt, void *ctx)
{
	if (nvmeibc_block_status_is_preempted(nd->status)) {
		scnprintf(nt->io_disabled_reason, sizeof(nt->io_disabled_reason), "Preempted by other client");
	} else if (nvmeibc_block_status_is_detaching(nd->status)) {
		scnprintf(nt->io_disabled_reason, sizeof(nt->io_disabled_reason), "Detaching");
	} else if (nt->io_perm == NVMEIB_IO_TYPE_PERMIT_NONE_SUS) {
		scnprintf(nt->io_disabled_reason, sizeof(nt->io_disabled_reason), "Temporary suspended");
	} else
		nvmeibc_topologies_error_state_reason(nt, ctx);
	return (nt->io_disabled_reason[0] ? nt->io_disabled_reason : "none");
}

static int vdisk_disabled_work(void *param) {
	struct nvmeibc_block_device *dev = (struct nvmeibc_block_device *)param;
	char *uevents[] = {VDISK_UEVENT_IO_DISABLE, NULL};
	_NT(trace_vdisk_disabled_work, "Sending uevent for io disable for @DEV_NAME", dev->name);
	return kobject_uevent_env(&disk_to_dev(dev->os->atom.disk)->kobj,
									KOBJ_CHANGE, uevents);
}

static int vdisk_enabled_work(void *param) {
	struct nvmeibc_block_device *dev = (struct nvmeibc_block_device *)param;
	char *uevents[] = {VDISK_UEVENT_IO_ENABLE, NULL};
	_NT(trace_vdisk_enabled_work, "Sending uevent for io enable for @DEV_NAME", dev->name);
	return kobject_uevent_env(&disk_to_dev(dev->os->atom.disk)->kobj,
									KOBJ_CHANGE, uevents);
}


static void __error_state_update(struct nvmeibc_topologies *nt, enum nvmeib_io_type_permission new_io_perm, void *ctx)
{
	struct nvmeibc_block_device *nd = container_of(nt, struct nvmeibc_block_device, topologies);
	/* IO has 3 states {1. BIO&Sync, 2. only Syncs, 3. Nothing}. So transition old->new is a 3x3 matrix */
	const enum nvmeib_io_type_permission old_io_perm = nt->io_perm;						// For debug only
	const bool old_io_perm_is_err = nvmeibc_topo_is_in_err_state(nt);		// State 3.
	const bool old_io_perm_is_pok = nvmeibc_topo_is_partially_ok(nt);		// State 1.
	bool is_first_bio_enabled = false;
	int uevent_rv;
	#define wrong_ctr_msg DMESG_PREFIX("%s") "io_perm=%u unexpected\n"

	nt->io_perm = new_io_perm;
	_NT(tr_1_block_bling, "@DEV_NAME io_perm=@IO_PERM -> io_perm=@IO_PERM", nd->name, old_io_perm, new_io_perm);
	if (!nvmeibc_topo_is_partially_ok(nt)) {											// No bio, maybe syncs
		const bool can_do_syncs = !nvmeibc_topo_is_in_err_state(nt);
		const bool was_preempted = (new_io_perm == NVMEIB_IO_TYPE_PERMIT_NO_RM_IO);
		if (was_preempted)
			nvmeibc_block_update_status(nd, 'P');

		if (old_io_perm_is_err) {								// Nothing to do. Bio still disabled...
			if (can_do_syncs) {
				nvmeibc_block_update_status(nd, 'S');			// Important for bookeeping of first time sync/io enabled
				_NI(tr_2_block_bling, "@DEV_NAME: Enabling recoveries (io_perm=@IO_PERM).", nd->name, new_io_perm);
			}
		} else if (old_io_perm_is_pok) {						// disabling bio
			const char *reason = __get_reason_for_io_disable(nd, nt, ctx);
			_NI(tr_3_block_bling, "@NDU @DEV_NAME: Disabling I/O@STR for a volume. Error code: 1049. Internal IO permissions: @IO_PERM. Internal additional info: @STR", 0, nd->name, (can_do_syncs? ", recoveries enabled" : " and recoveries"), new_io_perm, reason);
			__notify_all_riders_about_io_perm_change(nd);
			WARN(nt->dbg_disabling_ts, wrong_ctr_msg, nd->name, new_io_perm);	//
			nt->dbg_disabling_ts = jiffies;
			if (IS_PATH_VDISK(nd->name) && !nvmeibc_block_is_recoverer_or_hidden(nd)) {
				if ((uevent_rv = nvmeibc_run_on_main_wq(nvmeibc_isnt_params_blk2main(nd->cips),
										vdisk_disabled_work, nd, false, false, NULL)))
					_NE(__error_state_d_uevent_f, "add io-disable uevent wq entry for @DEV_NAME failed(@RV)", nd->name, uevent_rv);
				else
					_NT(__error_state_d_uevent_s, "add io-disable uevent wq entry for @DEV_NAME", nd->name);
			}
		} else {												// Previously could do only syncs
			WARN(!nt->dbg_disabling_ts, wrong_ctr_msg, nd->name, new_io_perm);	//
			if (!can_do_syncs)
				_NI(tr_4_block_bling, "@DEV_NAME: Disabling recoveries (io_perm=@IO_PERM).", nd->name, new_io_perm);
		}
	} else  {											// Has BIO
		if (!old_io_perm_is_pok) {              // Previously only nothing or syncs were allowed
			const u64 dt = (jiffies - nt->dbg_disabling_ts);
			is_first_bio_enabled = nvmeibc_block_update_status(nd, 'I');
			WARN(!nt->dbg_disabling_ts, wrong_ctr_msg, nd->name, new_io_perm);	//
			nt->dbg_disabling_ts = 0;
			_NI(tr_5_block_bling, "@NDU @DEV_NAME:Enabling I/O and recoveries for a volume after @SECONDS. Internal information (toggles=@DBG_NUM_ENABLING_IO_TOGGLES, IO permissions: @IO_PERM).", 0, nd->name, (size_t)(dt/HZ), nt->dbg_num_enabling_io_toggles, new_io_perm);  //. Error code: 0
			block_api_os_change_size(nd, is_first_bio_enabled);
			__notify_all_riders_about_io_perm_change(nd);
			if (unlikely(nt->debug_on_io_enabled.cb))
				nt->debug_on_io_enabled.cb(nt, nt->debug_on_io_enabled.ctx);
			if (IS_PATH_VDISK(nd->name) && new_io_perm == NVMEIB_IO_TYPE_PERMIT_ALL && !nvmeibc_block_is_recoverer_or_hidden(nd)) {
				if ((uevent_rv = nvmeibc_run_on_main_wq(nvmeibc_isnt_params_blk2main(nd->cips),
								 		vdisk_enabled_work, nd, false, false, NULL)))
					_NE(__error_state_e_uevent_f, "add io-enable uevent wq entry for @DEV_NAME failed(@RV)", nd->name, uevent_rv);
				else
					_NT(__error_state_e_uevent_s, "add io-enable uevent wq entry for @DEV_NAME", nd->name);
			}
		} else {
			_ND(tr_6_block_bling, "@DEV_NAME: BIO stays enabled for. Switch-Topo transitions", nd->name);
			if (old_io_perm != new_io_perm)
				__notify_all_riders_about_io_perm_change(nd);					// EC has NO_PROTECTION FLAG that might need to be updated when returning to normal
		}
	}

	if (nvmeibc_block_status_is_detaching(nd->status)) {
		_NT(tr_7_block_bling, "@DEV_NAME: detaching, ignoring new io_perm(@IO_PERM).", nd->name, new_io_perm);
	} else {
		const enum nvmeibc_io_perm_arm alert = __convert_io_type_permission_to_alert(new_io_perm, is_first_bio_enabled); // is_first_bio_enabled -> (nt->dbg_num_enabling_io_toggles == 1)));
		nvmeibc_io_perm_alert_switch(&nd->dp.io_perm_alert, alert);
	}
}

const char *nvmeibc_block_status_to_string(enum nvmeibc_block_status s)
{
	switch (s) {
	case NCBD_ATTACHING_NO_IO:	return "Attaching";
	case NCBD_ATTACHED:			return "Attached";
	case NCBD_PREEMPTED:		return "Preempted";
	case NCBD_DETACHING:		return "Detaching";
	case NCBD_DETACHING_UPGRD:	return "DetUpging";
	default: return "???";
	}
}

#define BUF_ADD(...) pos += scnprintf(buf+pos, len-pos, __VA_ARGS__)
#if defined(BLKDEV_PROFILING)
static ssize_t __export_profilers(char format, void *_dev, char *buf, size_t len)
{
	struct nvmeibc_block_device *dev = _dev;
	ssize_t pos = 0;
	if (unlikely(nvmeibc_block_status_is_detaching(dev->status))) {
		BUF_ADD("Detaching...\n");
	} else {
		if ('J' == format){
			pos += nvmeibc_topologies_profilers_tostring(&dev->topologies, buf, len);
		} else {
			pos += nvmeibc_topologies_profilers_tocsv(&dev->topologies, buf, len);
		}
	}
	return pos;
}

static ssize_t __profilers_tostring(void *_dev, char *buf, size_t len)
{
	return __export_profilers('J', _dev, buf, len);
}

static ssize_t __profilers_tocsv(void *_dev, char *buf, size_t len)
{
	return __export_profilers('C', _dev, buf, len);
}
#endif/*BLKDEV_PROFILING*/

static const char * __dev_type_2_string(const struct nvmeibc_block_device *dev)
{
	const bool is_hidden = nvmeibc_block_is_hidden(dev);			// == (dev->os->is_io_api_disabled)
	// Dont use os for 2 reasons:
	// 1. This access is unsafe. os_api could create /proc files through which to string is called but os was not conected to bdev
	// 2. dev->type holds much more information, for future exact to string of this enum
	return (is_hidden ? "hidden" : "visible");
}

// dev must not be NULL
const struct nvmeibc_volume_attach_t * nvmeibc_block_get_res_vat(const struct nvmeibc_block_device *dev)
{
	return nvmeibc_volume_get_attach_t(dev->volume);
}

bool nvmeibc_block_is_kernel_sector_io_allowed(const struct nvmeibc_block_device *dev)
{
	return nvmeibc_volume_get_attach_t(dev->volume)->res.is_512B_IO_allowed;
}

bool nvmeibc_block_dev_is_shadow(struct nvmeibc_block_device *dev)
{
	return nvmeibc_block_is_shadow(&dev->volume->hdr);
}

static ssize_t __data_path_flags_tostring(const struct nvmeibc_datapath *dp, char *buf, size_t len)
{
	ssize_t pos = 0;
	BUF_ADD("\"dp_flags\" : {\"edic\": %d, \"local_read\": %d, \"mutable_r_bio\": %d", dp->enable_edic_check, dp->enable_local_read_optimization, dp->read_has_mutable_bio_buffers);
	BUF_ADD(", \"alw\": %u, \"alr\": %u", (dp->alignment_sectors.write >> KERNEL_SECTOR_TO_SECTOR_SHIFT), (dp->alignment_sectors.read >> KERNEL_SECTOR_TO_SECTOR_SHIFT));
	BUF_ADD(", \"elev\": %u, \"snake\": %u, \"jent\": %u}", dp->elevator.max_elev_write, dp->p.snake_size, dp->p.binje);
	return pos;
}

static ssize_t __nvmeibc_sync_stats_tostring(const struct nvmeibc_sync_stats *ss, spinlock_t *lock, char *buf, size_t len, char fmt)
{
	ssize_t pos = 0;
	unsigned long flags;
	spin_lock_irqsave(lock, flags);
	if (fmt == 'H') {
		BUF_ADD("Sync Stats: r=%d/p=%d/u=%d, OK:f=%llu/p=%llu Done:t=%llu/m=%llu/ds=%llu, L=%u[msec], rr=%u",
				ss->num_running, ss->num_pending, ss->num_el_in_resources_reuse_list,
				ss->num_full_blockset_ok, ss->num_part_blockset_ok,
				ss->num_total, ss->num_total_maintainance, ss->num_dirty_bit_suspect,
				ss->longest_sync_time, ss->n_resources_reused);
	} else {
		BUF_ADD("\"sync_stas\": {\"n_running\":%d, \"n_pending\":%d, \"n_reuse_elems\":%d,", ss->num_running, ss->num_pending, ss->num_el_in_resources_reuse_list);
		BUF_ADD("\"finished_ok\": {\"full_blkst\":%llu, \"partial_blkst\":%llu}, ", ss->num_full_blockset_ok, ss->num_part_blockset_ok);
		BUF_ADD("\"finished\": {\"n_all\":%llu, \"n_maintanance\":%llu, \"n_db_suspect\":%llu, \"n_commit_stale\":%llu, \"n_commit_binfo\":%llu}, ", ss->num_total, ss->num_total_maintainance, ss->num_dirty_bit_suspect, ss->num_commit_stale, ss->num_commit_binfo);
		BUF_ADD("\"longest_sec\":%u, \"num_res_reused\":%u}", ss->longest_sync_time, ss->n_resources_reused);
	}
	spin_unlock_irqrestore(lock, flags);
	#ifdef AUTONOMOUS_SYNCS_STATS
		if (fmt == 'H')
			BUF_ADD(", auto:{OK=%llu/Err=%llu}", (u64)atomic64_read(&ss->num_autonomous_ok), (u64)atomic64_read(&ss->num_autonomous_err));
		else
			BUF_ADD(", \"autonomous_syncs\": {\"OK\":%llu, \"Fail\":%llu}", (u64)atomic64_read(&ss->num_autonomous_ok), (u64)atomic64_read(&ss->num_autonomous_err));
	#endif
	return pos;
}

static ssize_t __topo_stats_tostring(const struct topo_stats_t *ts, char *buf, size_t len, char fmt)
{
	ssize_t pos = 0;
	if (fmt == 'H') {
		BUF_ADD(",\tTopoSt:{n_Mdeg=%llu, n_1deg=%llu}\n", ts->n_multiple_deg, ts->n_single_deg);
	} else {
		BUF_ADD("\"topo_stats\": {\"n_multi_degraded\":%llu, \"n_single_degraded\":%llu}", ts->n_multiple_deg, ts->n_single_deg);
	}
	return pos;
}

static ssize_t __dev_unsorted_tostring(const struct nvmeibc_block_device *dev, char *buf, size_t len, char fmt)
{
	const bool deprecated = dev->os->atom.conf.enforce_readonly;	// Safe access to os! os has proc files through which this function is called so, 'os' was not destroyed yet and block device exists.
	ssize_t pos = 0;
	if (fmt == 'H') {
		BUF_ADD("Enforce Read Only: %c, Retry Timeout: %u[sec], ext_car_io=%c\n", (deprecated ? 'Y' : 'N'), (u32)(dev->max_retry_jiffies/HZ), (dev->allow_external_io_on_carrier ? 'Y' : 'N'));
	} else {
		BUF_ADD("\"enforce_read_only\": %u, \"retry_timeout_sec\": %u, \"allow_extern_carrier_io\": %u", deprecated, (u32)(dev->max_retry_jiffies/HZ), dev->allow_external_io_on_carrier);
	}
	return pos;
}

static ssize_t __nvmeibc_dev_flags_to_string(const struct nvmeibc_block_device *dev, char *buf, size_t len, char fmt)
{
	ssize_t pos = 0;
	if (fmt == 'H') {
		BUF_ADD("Flags: itm={%d/r=%d/%%=%d}", dev->ignore_all_toma_msgs, dev->ignore_all_recov_requests, dev->ignore_all_recov_toma_speed_req);
	} else {
		BUF_ADD("\"flags\": {\"ign_toma_msg\":%d, \"ign_toma_recov\":%d, \"ign_toma_rec_speed\":%d}", dev->ignore_all_toma_msgs, dev->ignore_all_recov_requests, dev->ignore_all_recov_toma_speed_req);
	}
	return pos;
}

static inline int get_vol_size_best_units_log1024(ulong val) {
	int rv;
	for (rv = 0; (val >>= 10) > 50; rv++); // (50 + 1)*1024[b]  = 51[kb]. Never print more than 5 digits. Visually: ~51,000[gb] becomes 51[pb]
	return rv;
}

#define BLK_STATUS_PROC_FRMT_VER 2
// Todo: Unify the 2 functions below: Print Human and Print Json
static ssize_t __block_tostring(void *_dev, char *buf, size_t len)
{
	struct nvmeibc_block_device *dev = _dev;
	const ulong size_bytes = (dev->size << NVMEIBC_SECTOR_SHIFT);
	const int unit_indx = get_vol_size_best_units_log1024(size_bytes);
	const char unit_name[] = " KMGTPE";	// bytes, kilobytes, mega, giga, tera, peta, exa
	const int size_in_units = (int)(size_bytes >> (10*unit_indx));
	ssize_t pos = 0;
	extern bool qa_ec_stress_debug;
	BUF_ADD("Name=%s, UUID=%s, size=%ld[blocks], %u[%cb], short_id=%d, ", dev->name, dev->uuid, dev->size, size_in_units, unit_name[unit_indx], nvmeibc_volume_short_id(dev));
	BUF_ADD("Sector Size=%d[bytes], ptr=0x%llx, type=%s (0x%x)\n", get_logical_block_size_from_os(dev->os), (u64)dev, __dev_type_2_string(dev), dev->type);
	pos += nvmeibc_volume_attach_t_tostring(nvmeibc_block_get_res_vat(dev), buf+pos, len-pos);
	BUF_ADD(", \t"); pos += nvmeibc_volume_tostring(   dev->volume, buf+pos, len-pos, 'H');
	BUF_ADD(", \t"); pos += __data_path_flags_tostring(&dev->dp   , buf+pos, len-pos);
	BUF_ADD(", \t"); pos += __nvmeibc_dev_flags_to_string(dev, buf+pos, len-pos, 'H');
	BUF_ADD("\n");

	if (dev->dp.enable_di_debug_mode) {
		extern int dp_dbgdi_get_sizeof_injected_data(void);
		BUF_ADD("!!! WARNING !!!\tData is NOT written! debug di mode is active, inject=%d[b]!\n", dp_dbgdi_get_sizeof_injected_data());
	}
	if (qa_ec_stress_debug) {
		BUF_ADD("!!! WARNING !!!\tStress mode activated, IO is slowed!\n");
	}
	if (dev->autoext.is_api_enabled) { // Daniel, Todo: Move to separate function
		BUF_ADD("allocated_size=%lu[blocks, ", dev->autoext.allocated_size);
		BUF_ADD("max_write_lba=%lu[blocks]\n", dev->autoext.max_write_lba);
	}
	if (unlikely(nvmeibc_block_status_is_detaching(dev->status))) {
		BUF_ADD("Detaching...\n");
		goto _out;
	}
	pos += nvmeibc_topologies_status_tostring(&dev->topologies,buf+pos,len-pos);
	pos += __dev_unsorted_tostring(dev,buf+pos,len-pos, 'H');
	pos += __nvmeibc_sync_stats_tostring(&dev->dp.sync_rsrcs.stats, &dev->dp.resub.lock, buf+pos, len-pos, 'H');
	pos += __topo_stats_tostring(&dev->topo_stats, buf+pos, len-pos, 'H');
	pos += nvmeibc_blk_op_elevator_string(&dev->merge_op, buf+pos, len-pos, 'H');

_out:
	pos += dp_io_stats_tostring(  &dev->dp.io_stats,        buf+pos, len-pos, 'H');
	pos += nvmeibc_io_perm_alert_tostring(&dev->dp.io_perm_alert, buf+pos, len-pos, 'H');
	pos += slow_io_stats_t_tostring(    &dev->dp.io_slow,         buf+pos, len-pos, 'H');
	pos += nvmeib_proc_add_txt_proc_epilog(BLK_STATUS_PROC_FRMT_VER,
						buf + pos, len - pos);
	return pos;
}

static ssize_t __block_tojson(void *_dev, char *buf, size_t len)
{
	#define BUF_ADD(...) pos += scnprintf(buf+pos, len-pos, __VA_ARGS__)
	struct nvmeibc_block_device *dev = _dev;
	ssize_t pos = 0;
	BUF_ADD("{\n\"name\": \"%s\",\n\"uuid\": \"%s\",\n\"type\": \"%s\", \"type_num\": \"0x%x\",\n\"size[blks]\": %ld,\n\"block[b]\": %d,\"short_id\": %d,\n\"ptr\": \"%p\",\n \"debug_di\": %d,\n",
			dev->name, dev->uuid, __dev_type_2_string(dev), dev->type, dev->size, get_logical_block_size_from_os(dev->os), nvmeibc_volume_short_id(dev), dev, (int)dev->dp.enable_di_debug_mode);
	pos += nvmeibc_volume_tostring(   dev->volume, buf+pos, len-pos, 'J'); BUF_ADD(",\n");
	pos += __data_path_flags_tostring(&dev->dp   , buf+pos, len-pos     ); BUF_ADD(",\n");
	pos += __nvmeibc_dev_flags_to_string(dev,      buf+pos, len-pos, 'J'); BUF_ADD(",\n");

	if (nvmeibc_block_status_is_detaching(dev->status))
		goto _out;

	pos += nvmeibc_topologies_status_tojson(&dev->topologies,buf+pos,len-pos); BUF_ADD(",\n");
	pos += __dev_unsorted_tostring(dev,buf+pos,len-pos, 'J'); BUF_ADD(",\n");
	pos += __nvmeibc_sync_stats_tostring(&dev->dp.sync_rsrcs.stats, &dev->dp.resub.lock, buf+pos, len-pos, 'J'); BUF_ADD(",\n");
	pos += __topo_stats_tostring(&dev->topo_stats, buf+pos, len-pos, 'J'); BUF_ADD(",\n");
	pos += nvmeibc_blk_op_elevator_string(&dev->merge_op, buf+pos, len-pos, 'J'); BUF_ADD(",\n");

_out:
	pos += dp_io_stats_tostring(  &dev->dp.io_stats,        buf+pos, len-pos, 'J'); BUF_ADD(",\n");
	pos += nvmeibc_io_perm_alert_tostring(&dev->dp.io_perm_alert, buf+pos, len-pos, 'J'); BUF_ADD(",\n");
	pos += slow_io_stats_t_tostring(    &dev->dp.io_slow,         buf+pos, len-pos, 'J');
	pos += nvmeib_proc_add_json_proc_epilog(BLK_STATUS_PROC_FRMT_VER, buf + pos, len - pos);
	BUF_ADD("}\n");		// Replace the last ",\n" with "}\n"
	return pos;
}

#define RECOV_STATS_PROC_FRMT_VWR 1
static ssize_t __stalocks_tostring(void *_dev, char *buf, size_t len)
{
	struct nvmeibc_block_device *dev = _dev;
	ssize_t pos   					 = 0;
	if (unlikely(nvmeibc_block_status_is_detaching(dev->status))) {
		BUF_ADD("Detaching...\n");
		goto _out;
	}
	pos += nvmeibc_topologies_stalocks_tostring(&dev->topologies,buf+pos,len-pos);
	pos += nvmeib_proc_add_txt_proc_epilog(RECOV_STATS_PROC_FRMT_VWR, buf + pos, len - pos);
_out:
	return pos;
}

#define BLOB_TXT_PROC_FRMT_VER 1

static ssize_t __ext_blob_to_txt_fill(const struct nvmeibc_block_device *dev, char *buf, size_t len)
{
	const struct nvmeibc_volume_header *hdr = &dev->volume->hdr;
	const struct ext_blob_t *ext = &hdr->ext_blob;
	ssize_t pos = 0;
	int i;
	if (true || !nvmeibc_block_status_is_detaching(dev->status)) {	// For debug, print even when detaching
		const int max_ref_len = sizeof(ext->referenceIDs[i].val);
		BUF_ADD("encoding_version = %d\n", 1);		// External scripts, like Kubernetes, csi drivers parse this file. Future compatibility
		BUF_ADD("attach_version_global = %d\n", hdr->attachment_version);
		BUF_ADD("attach_version_pervol = %llu\n", hdr->attachment_version_per_volume);
		BUF_ADD("n_strings = %d\n", ext->n_ref_ids);
		for (i = 0; i < ext->n_ref_ids; i++) {
			BUF_ADD("%.*s\n", max_ref_len, &ext->referenceIDs[i].val[0]);
		}
		pos += nvmeib_proc_add_txt_proc_epilog(BLOB_TXT_PROC_FRMT_VER, buf+pos, len-pos);
	} // pr_emerg("%s", buf);
	return pos;
}

static ssize_t __ext_blob_to_txt(void *_dev, char *buf, size_t len)
{
	ssize_t rv = 0;
	unsigned long flags = 0;
	struct nvmeibc_block_device *dev = _dev;
	struct nvmeibc_volume_header *hdr = (void*)&dev->volume->hdr; //drop const qualifier
	spin_lock_irqsave(&hdr->ext_blob_modify_guard, flags);
	rv = __ext_blob_to_txt_fill(dev, buf, len);
	spin_unlock_irqrestore(&hdr->ext_blob_modify_guard, flags);
	return rv;
}

#define PROFILING_PROC_FRMT_VER 1
static ssize_t __flow_counters_to_json(void *_dev, char *buf, size_t len)
{
	struct nvmeibc_block_device *dev = _dev;
	ssize_t pos   					 = 0;
	BUF_ADD("{");			// JSON start
	if (unlikely(nvmeibc_block_status_is_detaching(dev->status))) {
		goto _out;
	}
	BUF_ADD("\"syncs\" : {\n");
	pos += nvmeibc_htr_fill_status(              buf+pos, len-pos); BUF_ADD(",\n");
	pos += nvmeibc_cold_stats_to_string(         buf+pos, len-pos); BUF_ADD(",\n");
	pos += nvmeibc_maintain_sync_stats_to_string(buf+pos, len-pos); BUF_ADD(",\n");
	pos += nvmeibc_nowhole_stats_to_string(      buf+pos, len-pos); BUF_ADD("}\n");
	pos += nvmeib_proc_add_json_proc_epilog(PROFILING_PROC_FRMT_VER, buf+pos, len-pos);
_out:
	BUF_ADD("}");		// JSON END
	return pos;
}

#define BLK_CPU_MASKS_PROC_FRMT_VER 1
ssize_t nvmeibc_block_cpu_masks_to_json(const struct nvmeibc_cinst_params_blk *p, char *buf, size_t len)
{
	ssize_t pos = 0;

	BUF_ADD("{");	// JSON start

	BUF_ADD("\"cpu_masks\" : ");
	pos += nvmeibc_b_cp_cpu_masks_to_json(__get_from_params_blok_globals_container(p)->cpu_masks, buf + pos, len - pos);
	pos += nvmeib_proc_add_json_proc_epilog(BLK_CPU_MASKS_PROC_FRMT_VER, buf + pos, len - pos);

	BUF_ADD("}");	// JSON END

	return pos;
}

#define BLK_VOLUME_CPU_MASKS_PROC_FRMT_VER 1
static ssize_t __cpu_masks_to_json(void *_dev, char *buf, size_t len)
{
	struct nvmeibc_block_device *dev = _dev;
	ssize_t pos = 0;

	BUF_ADD("{");	// JSON start

	if (unlikely(nvmeibc_block_status_is_detaching(dev->status))) {
		goto out;
	}

	BUF_ADD("\"cpu_masks\" : ");
	pos += nvmeibc_b_cp_cpu_masks_volume_masks_to_json(__get_from_params_blok_globals_container(dev->cips)->cpu_masks, &dev->cpu_masks , buf + pos, len - pos);
	pos += nvmeib_proc_add_json_proc_epilog(BLK_VOLUME_CPU_MASKS_PROC_FRMT_VER, buf + pos, len - pos);

out:
	BUF_ADD("}");	// JSON END

	#undef BUF_ADD

	return pos;
}

struct __cpu_mask_add_del_ctx {
	bool is_add;
	struct nvmeibc_block_device *dev;
	char *buf;
	size_t len;
	struct completion *done;
	int rv;
};

static int __notify_disk_mask_update(struct nvmeibc_disk *disk, void *ctx)
{
	(void)ctx;
	return nvmeibc_disk_notify_coremask_update(disk);
}

static int __cpu_mask_add_del_on_main_wq(void *_ctx)
{
	int rv;
	struct __cpu_mask_add_del_ctx *ctx = _ctx;
	struct nvmeibc_block_device *dev = ctx->dev;
	struct t_block_clnt_globals *b = __get_from_params_blok_globals_container(nvmeibc_cinst_get_blok_p(dev));
	struct nvmeib_cpu_mask cpu_mask = { 0 };

	nvmeibc_assert_on_main_wq(nvmeibc_isnt_params_blk2main(nvmeibc_cinst_get_blok_p(dev)));

	rv = bitmap_parse(ctx->buf, strnlen(ctx->buf, ctx->len), cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS);
	if (rv) {
		_NI_to_user(i_cpu_mask_add_del_bitmap_parse_fail, QA_BLOCK_PREFIX, "Failed to parse mask, rv=@RV", rv);
		goto out;
	}

	if (ctx->is_add) {
		u64 gen;
		rv = nvmeibc_b_cp_cpu_masks_add_volume_mask(b->cpu_masks, &dev->cpu_masks, &cpu_mask, &gen);
		if (rv) {
			_NI_to_user(i_cpu_mask_add_del_bitmap_add_fail, QA_BLOCK_PREFIX, "Failed to add CPU mask, rv=@RV", rv);
			goto out;
		}
		nvmeibc_b_dp_cpu_masks_add(&dev->dp.cpu_masks, &cpu_mask, gen);
	} else {
		rv = nvmeibc_b_cp_cpu_masks_del_volume_mask(b->cpu_masks, &dev->cpu_masks, &cpu_mask);
		if (rv) {
			_NI_to_user(i_cpu_mask_add_del_bitmap_del_fail, QA_BLOCK_PREFIX, "Failed to del CPU mask, rv=@RV", rv);
			goto out;
		}
		nvmeibc_b_dp_cpu_masks_del(&dev->dp.cpu_masks, &cpu_mask);
	}

	/* Notify all volume disks that the masks have changed */
	if ((rv = nvmeibc_block_call_for_all_disks(dev, __notify_disk_mask_update, NULL)) < 0) {
		_NI_to_user(i_cpu_mask_add_del_notify_disk_fail, QA_BLOCK_PREFIX, "Failed to notify disks, rv=@RV", rv);
		goto out;
	}
	_NI(i_cpu_mask_add_del_notify_disk_ok, "Notified @N_DISKS disks of mask change", rv);
	rv = 0;

out:
	ctx->rv = rv;
	complete(ctx->done);
	return 0;
}

static ssize_t __cpu_mask_add_del(bool is_add, struct nvmeibc_block_device *dev, char *buf, size_t len)
{
	ssize_t rv;
	DECLARE_COMPLETION_ONSTACK(work_done);
	struct __cpu_mask_add_del_ctx ctx = {
			.is_add = is_add,
			.dev = dev,
			.buf = buf,
			.len = len,
			.done = &work_done,
	};

	if ((rv = nvmeibc_run_on_main_wq(nvmeibc_isnt_params_blk2main(nvmeibc_cinst_get_blok_p(dev)), __cpu_mask_add_del_on_main_wq, &ctx, false /* drain */, true /* can_sleep */, NULL)) < 0) {
		_NW(cpu_mask_add_del_warn, DMESG_PREFIX() "Failed to run a task on the main wq");
	} else {
		wait_for_completion(&work_done);
		rv = (ctx.rv < 0) ? ctx.rv : (ssize_t)len;
	}

	return rv;
}

static ssize_t __cpu_mask_add(void *_dev, char *buf, size_t len)
{
	return __cpu_mask_add_del(true /* is_add */, (struct nvmeibc_block_device *)_dev, buf, len);
}

static ssize_t __cpu_mask_del(void *_dev, char *buf, size_t len)
{
	return __cpu_mask_add_del(false /* is_add */, (struct nvmeibc_block_device *)_dev, buf, len);
}

#include "block/controlpath/nvmeibc_b_cp_topo_common.h"
#include "block/recovery/nvmeibc_raid_recovery.h"
void nvmeibc_block_clean_debug_counters(struct nvmeibc_block_device *dev, bool io_tot, bool io_worst, bool recov, bool flow, bool profiler)
{
	extern void block_api_sync_clear_stats(struct nvmeibc_block_device *);

	assert_dev_on_mainwq(dev);
	if (unlikely(nvmeibc_block_status_is_detaching(dev->status))) {
		goto _out;		// Just an optimization, no need to clear counters
	}
	if (io_tot || io_worst) {									// Good path IO counters
		const int which = ((io_tot && io_worst) ? 'A' : (io_worst ? 'W' : 'T'));
		block_api_os_clear_io_stats(dev->os,		  which);	// Safe access to os. Cant change to NULL because we are on main-wq and not detaching
		nvmeibc_volume_disks_stats_clear(dev->volume, which);
	}
	if (io_worst) {												// Worst problems of ios
		dp_io_stats_clear(          &dev->dp.io_stats);
		topo_stats_t_clear(&dev->topo_stats);
		slow_io_stats_t_clear(            &dev->dp.io_slow);
		nvmeibc_io_perm_alert_clear_stats(&dev->dp.io_perm_alert);
	}
	if (recov || profiler) {
		struct nvmeibc_topology *t = nvmeibc_topology_get(&dev->topologies);
		const struct nvmeibc_chunk *chunk;
		const struct nvmeibc_raid1 *r1;
		int c, r, rcvr;
		if (t) {
			topo_for_each_raid1(t, chunk, c, r1, r) {
				if (recov) {
					for (rcvr = 0; rcvr < NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES; ++rcvr)
						nvmeibc_recovery_stats_clear(r1->hdr->recoveries[rcvr]);
				}
				if (profiler) {
					int si;
					struct nvmeibc_disk_segment *seg;
					raid1_for_each_seg(r1, seg, si) {
						nvmeibc_seg_topo_persistent_clear_profilers(seg);
					}
					nvmeibc_raid_topo_persistent_clear_profilers(r1->hdr);
				}
			}
			if (profiler) {
				nvmeibc_raid_topo_persistent_clear_preparation_profiler(dev);
			}
			nvmeibc_topology_put(t);
		}
		block_api_sync_clear_stats(dev);
	}
	if (flow) {
		nvmeibc_flow_counters_reset();
	}
_out:;
}

static const struct nvmeibc_procfs_cb block_proc_cbs = {
	.dev_status_to_txt = __block_tostring,
	.dev_status_to_json = __block_tojson,
	.dev_recovs_to_txt = __stalocks_tostring,
	.flows_cntr_to_json = __flow_counters_to_json,
#if defined(BLKDEV_PROFILING)
	.profiling_to_string = __profilers_tostring,
	.profiling_to_csv = __profilers_tocsv,
#else
	.profiling_to_string = NULL,
	.profiling_to_csv = NULL,
#endif/*BLKDEV_PROFILING*/
	.ext_blob_to_txt = __ext_blob_to_txt,
	.cpu_masks = {
		.to_json = __cpu_masks_to_json,
		.add = __cpu_mask_add,
		.del = __cpu_mask_del,
	},
};

static bool __block_api_os_will_execute_bio_with_wrapper(struct nvmeibc_block_device *dev)
{
	const bool is_512 = nvmeibc_block_is_kernel_sector_io_allowed(dev);
	return is_512;
}

int nvmeibc_block_upgrade_os_to_ioable(struct nvmeibc_block_device *dev, const int slice_size, u64 ro_header_sectors)
{
	const bool is_ro = (nvmeibc_block_get_res_vat(dev)->res.mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO);
	const u32 hidden_dbg_id = nvmeibc_volume_short_id(dev);
	const bool is_bio_wrapper_needed = __block_api_os_will_execute_bio_with_wrapper(dev);
	int rv;
	assert_dev_on_mainwq(dev);
	if (unlikely(block_api_os_init(dev->os,
				       (is_bio_wrapper_needed) ? execute_bio_with_wrapper : execute_bio,
				       dev->size, is_ro, slice_size,
				       ro_header_sectors) < 0)) {
		dev->os = NULL;					// Upon init error, os api cleans itself. As if allocation failed
		return -ENOMEM;
	}

	rv = block_api_os_start_accepting_kernel_io(dev->os);
	if ((rv >= 0) && !nvmeibc_block_is_hidden(dev)) {
		nvmeibc_volume_short_id(dev) = MINOR(disk_devt(dev->os->atom.disk));
		_NT(t_01_bdev_init, "@DEV_NAME: os_api is ioable, vol_id changed from @VOL_ID to @VOL_ID", dev->name, hidden_dbg_id, nvmeibc_volume_short_id(dev));
	}
	return rv;
}

static void __blockdevice_init_profilers(struct nvmeibc_block_device *dev)
{
	int ci=0, ri=0, si=0;
	struct nvmeibc_raid1 *r1 = NULL;
	struct nvmeibc_chunk *chunk = NULL;
	struct nvmeibc_disk_segment *seg = NULL;
	struct nvmeibc_topology *t = nvmeibc_topology_get(&dev->topologies);
	topo_for_each_raid1(t, chunk, ci, r1, ri){
		nvmeibc_raid_topo_persistent_init_profilers(r1->hdr, dev, ci, ri);
		raid1_for_each_seg(r1, seg, si) {
			nvmeibc_seg_topo_persistent_init_profilers(seg, dev, ci, ri, si);
		}
	}
	nvmeibc_topology_put(t);
}

static inline enum nvmeibc_data_path_type __select_datapath_type(const struct nvmeibc_block_device *dev, int max_slice_size, int max_protect_level)
{
	const bool is_ec = (max_slice_size > 1);
	if (is_ec)
		return NVMEIBC_DATA_PATH_EC_R6;
	if (max_protect_level > 0) {
		if (nvmeibc_block_is_any_carrier(dev)) {
			// WARN_ON(!nvmeibc_block_is_wcv(dev));		// Currently only WCV is supported here
			return NVMEIBC_DATA_PATH_MIR_D_CARRIER;
		} else
			return NVMEIBC_DATA_PATH_MIR_BIO;
	} else {
		return NVMEIBC_DATA_PATH_JBODS;					// Jbod without locks
	}
}

static inline bool __should_optimize_local_reads(const struct nvmeibc_block_device *dev, bool is_enabled)
{
	const enum_reservation_mode rmode = nvmeibc_block_get_res_vat(dev)->res.mode;
	const bool is_rmode_ro =   (rmode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO);
	const bool is_rmode_excl = (rmode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX);
	return (is_enabled && (is_rmode_ro || is_rmode_excl));	// LRO is allowed only in shared RO or exclusive modes. Explanation: Is it possible to do it with view_lock? Daniel: I think not. Example: 2 clients read from different legs of R1 different data both waiting for sync to comimt. Imagine that we read from P, sync commits data from D to P and view lock sees post sync unlocked value. IO returns never written data. Solution: Local Read works only for exclusive RW or shared RO mode
}

static int __bdev_datapath_init(struct nvmeibc_block_device *dev, int max_slice_size, struct nvmeibc_dp_params par, int max_protect_level, bool enable_crc_check, bool use_debug_di, bool enable_local_read_optimization, int read_has_mutable_bio_buffers)
{
	const enum nvmeibc_config_volume_type type = dev->type;
	const enum nvmeibc_data_path_type dp_type = __select_datapath_type(dev, max_slice_size, max_protect_level);
	const bool optimize_local_reads = __should_optimize_local_reads(dev, enable_local_read_optimization);
	_NT(t_00_bdev_init, "@DEV_NAME: @HDR_TYPE @DATAPATH_TYPE, crc=@BOOL_YN, local_reads=@BOOL_YN", dev->name, type, dp_type, enable_crc_check, optimize_local_reads);
	nvmeibc_datapath_init(&dev->dp, dev->name, dp_type, enable_crc_check, use_debug_di, optimize_local_reads, read_has_mutable_bio_buffers, max_slice_size, par);
	nvmeibc_io_perm_alert_create(&dev->dp.io_perm_alert, (u64)dev);
	__blockdevice_init_profilers(dev);
	nvmeibc_io_resubmitter_init(&dev->dp.resub);
	nvmeibc_recovs_drainer_init(&dev->dp.running_recovs, __on_last_recovery_finish_upon_detach, dev);
	toma_msg_handlers_drainer_init(&dev->topologies.tmhd, __on_toma_msg_handlers_drained);				// Not sure this is the correct place here
	return nvmeibc_b_dp_cpu_masks_init(&dev->dp.cpu_masks);
}

static void __bdev_datapath_destroy(struct nvmeibc_block_device *dev)
{
	nvmeibc_b_dp_cpu_masks_fini(&dev->dp.cpu_masks);
	nvmeibc_io_perm_alert_destroy(&dev->dp.io_perm_alert);
	nvmeibc_io_resubmitter_destroy(&dev->dp.resub);
	BUG_ON(!nvmeibc_profiling_put(dev->preparation_profiler));
	dp_io_stats_clear(&dev->dp.io_stats);
	nvmeibc_datapath_destroy(&dev->dp);
}

static void __blockdevice_derrived_classes_destroy(struct nvmeibc_block_device *dev)
{
	assert_dev_on_mainwq(dev);					// Either detach or failed attach
	if (nvmeibc_block_is_any_carrier(dev))
		nvmeibc_api_of_d_carrier_destroy(dev);
	if (dev->os)								// False only when this is a cleanup of failed initialization
		block_api_os_destroy(dev->os);
	dev->os = (void*)0x4;	/* For debug: Put dummy NULL to detect crashes */
}

static int __blockdevice_derrived_classes_init(struct nvmeibc_block_device *dev, int slice_size, u64 ro_header_sectors)
{
	int rv = 0;
	dev->os = block_api_os_create(nvmeibc_cinst_get_blok_p(dev),
				      (!!nvmeibc_block_is_hidden(dev)),
				      dev->name, dev->uuid, dev, block_proc_cbs);
	if (dev->os == NULL) {
		rv = -ENODEV;
		goto _init_err;
	}

	if (dev->type & AUTO_EXTEND_VOLUME) { 		// Daniel, Todo: move te saparate file with api
		dev->autoext.is_api_enabled = true;
		dev->autoext.allocated_size = dev->size;
		dev->size = 244140625000UL;			// = 1000^4 / 4096
	}
	if (nvmeibc_block_is_d_carrier(dev)) {
		if ((rv = nvmeibc_api_of_d_carrier_init(dev)) < 0)
			goto _init_err;
	}
	if (rv < 0) {
		goto _init_err;
	}

	if ((rv = nvmeibc_io_resubmitter_start(&dev->dp.resub, dev)) < 0)	// Start resubmit thread before first io arrives
		goto _init_err;
	if ((rv = nvmeibc_block_upgrade_os_to_ioable(dev, slice_size, ro_header_sectors)) < 0) // Optimize IO request to slice slize
		goto _init_err;

_init_err:
	return rv;
}

static int nvmeibc_block_watchdog(void *_ctx)
{
	struct t_block_clnt_globals *b = _ctx;
	const unsigned long freq = (1024*HZ)/1000;	/* 1024[msec] */
	while (!kthread_should_stop()) {
		nvmeibc_block_watchdog_run_once(b);
		wait_event_interruptible_timeout(b->watchdog.sleep_q, kthread_should_stop(), freq);
	}
	return 0;
}

// Todo: Move this function out from this file. To the same file as t_blok_clnt_globals_init(). It does not belong here
#include "module/instance/nvmeibc_cinst_params.h"
static inline int __block_wd_start(struct t_block_clnt_globals *b)		// Add-ref/Create singletone
{
	int rv = 0;
	nvmeibc_assert_on_main_wq(nvmeibc_cinst_get_blok_m(b));	// This is serialization with attach/detach. Dont use spinlock. Illegal to call kthread_run() under lock
	if (b->watchdog.thread) {
		BUG_ON(!b->watchdog.thread_counter);
	} else {
		proc_name_t pname;
		clnt_proc_name_format(pname, 'C', "WD", "block", nvmeibc_cinst_get_blok_inst_num(nvmeibc_cinst_get_blok_p(b)));
		BUG_ON(b->watchdog.thread_counter);
		init_waitqueue_head(&b->watchdog.sleep_q);
		b->watchdog.thread = kthread_run(nvmeibc_block_watchdog, b, "%s", pname);
		if (IS_ERR(b->watchdog.thread)) {
			_NE(error_block_block_wd_start, DMESG_PREFIX() ": Failed to create wd thread, rv=@PTR_ERR", PTR_ERR(b->watchdog.thread));
			b->watchdog.thread = NULL;
			b->watchdog.thread_counter--; /* Will be offset below */
			rv = -1;
		}
	}
	b->watchdog.thread_counter++;
	_ND(trace_0_block_block_wd_start, "Block watchdog ++@WATCHDOG_THREAD_COUNTER, rv=@RV", b->watchdog.thread_counter, rv);
	return rv;
}

static inline void __block_wd_stop(struct t_block_clnt_globals *b)	// Decrease-ref, delete singletone
{
	nvmeibc_assert_on_main_wq(nvmeibc_cinst_get_blok_m(b));		// This is serialization with attach/detach. Dont use spinlock. Illegal to call kthread_run() under lock
	if ((--b->watchdog.thread_counter) == 0) {
		wake_up(&b->watchdog.sleep_q);	// Speed-up last detach by waking up watchdog to let it finish
		kthread_stop(b->watchdog.thread);	// Syncronous blocking until watchdog dies
		b->watchdog.thread = NULL;
	}
	_ND(trace_1_block_block_wd_stop, "Block watchdog --@WATCHDOG_THREAD_COUNTER", b->watchdog.thread_counter);
}

unsigned nvmeibc_io_max_retry_secs = 0; /* NVMESH-419, NVMESH-495; BF.conf */
module_param_named(io_max_retry_secs, nvmeibc_io_max_retry_secs, uint, 0644);
MODULE_PARM_DESC(io_max_retry_secs, "Max time to try and execute IO before failing it to OS [sec], if 0, use system's default");

unsigned nvmeibc_io_max_retry_encrypted_secs = 300;
module_param_named(io_max_retry_encrypted_secs, nvmeibc_io_max_retry_encrypted_secs, uint, 0644);
MODULE_PARM_DESC(io_max_retry_encrypted_secs, "Encrypted volume max time to try and execute IO before failing it to OS [sec], if 0, use system's default");

static const char *io_perm_to_str(const enum_io_perm io_perm)
{
	switch (io_perm) {
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_NEVER:
		return "NEVER";
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_NONE_SUS:
		return "NONE_SUS";
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_NONE_ERR:
		return "NONE_ERR";
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_NO_IO:
		return "NO_IO";
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_IO_PREEMPT:
		return "IO_PREEMPT";
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_RDONLY:
		return "RDONLY";
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_ALL_NO_PROTECTION:
		return "ALL_NO_PROTECTION";
	case NVMEIB_C_TO_M_IO_TYPE_PERMIT_ALL:
		return "ALL";
	default:
		return "UNKNOWN";
	}
}

/* This function can be called in parallel from a few contexts, change 'status'
   atomically using spinlocks */
bool nvmeibc_block_update_status(struct nvmeibc_block_device* dev, char reason)
{
	unsigned long flags;
	bool revalidate_bdev = false;
	enum nvmeibc_block_status prev_status = dev->status;
	enum_io_perm io_perm;

	switch (reason) {
	case 'A': /* Attach */
		assert_dev_on_mainwq(dev);
		/* No need to take spinlocks, nothing in running in parallel */
		dev->status = NCBD_ATTACHING_NO_IO;
		dev->max_retry_jiffies = (nvmeibc_io_max_retry_secs ? : IO_TIME_OUT_ATTACH) * HZ;
		/* Do not revalidate, IO is not enable yet. */
		break;

	case 'D': /* Detach */
		assert_dev_on_mainwq(dev);
		if (!nvmeibc_block_status_is_detaching(dev->status)) {
			spin_lock_irqsave(&dev->dp.resub.lock, flags);
			dev->status = NCBD_DETACHING;
			block_api_os_stop_accepting_kernel_io(dev->os, 'D');	/* Dont let new IO's income to bdev */
			dev->max_retry_jiffies = HZ / 100;	/* Autofail all existing IOs almost immediately */
			spin_unlock_irqrestore(&dev->dp.resub.lock, flags);
			nvmeibc_io_resubmitter_wakeup(&dev->dp.resub);
			nvmeibc_io_perm_alert_switch(&dev->dp.io_perm_alert, NVMEIBC_IO_PERM_ARM_DETACHING);
		}
		/* Do not revalidate, No need, we are detaching */
		break;

	case 'U': /* Detach for upgrade, giving control to nvmeiba_os_api */
		assert_dev_on_mainwq(dev);
		if (!nvmeibc_block_status_is_detaching(dev->status)) {
			spin_lock_irqsave(&dev->dp.resub.lock, flags);
			dev->status = NCBD_DETACHING_UPGRD;
			block_api_os_stop_accepting_kernel_io(dev->os, 'U');	/* Dont let new IO's income to bdev */
			dev->max_retry_jiffies = 0; /* Autofail IO's immediately, atom will intercept them and retry after upgrade */
			spin_unlock_irqrestore(&dev->dp.resub.lock, flags);
			nvmeibc_io_resubmitter_wakeup(&dev->dp.resub);
			nvmeibc_io_perm_alert_switch(&dev->dp.io_perm_alert, NVMEIBC_IO_PERM_ARM_DETACHING);
		}
		/* Do not revalidate, No need, we are detaching */
		break;

	case 'S':
	case 'I': { /* IO / Sync-only enabled */
		const bool bio_enabled = (reason == 'I');
		spin_lock_irqsave(&dev->dp.resub.lock, flags);
		if (dev->status == NCBD_ATTACHING_NO_IO) { /* First activation of IO */
			dev->status = NCBD_ATTACHED;
			if (!nvmeibc_block_is_any_carrier(dev)) {	// consider to add '&& bio_enabled'
				if (nvmeibc_block_is_shadow(dev)){
					dev->max_retry_jiffies = (nvmeibc_io_max_retry_encrypted_secs ? : IO_TIME_OUT_NORMAL) * HZ;
				} else {
					dev->max_retry_jiffies = (nvmeibc_io_max_retry_secs ? : (IS_PATH_VDISK(dev->name)? 4 : IO_TIME_OUT_NORMAL)) * HZ;
				}
			} else { /* Rider will setup timeout for its carriers during mounting */}
			revalidate_bdev = bio_enabled;
			nvmeibc_block_set_generic_work_to_main(nvmeibc_cinst_get_blok_p(dev), dev->uuid,
					nvmeibc_cc_api_notify_io_changed, dev->uuid, false);
		} else if (nvmeibc_block_status_is_preempted(dev->status)) {	// Back from preemption, Set a trap
			BUG_ON(bio_enabled);	// This can lead to data corruption. Volume is preempted until detach. Preempted volume cannot enable IO.
		} else if (nvmeibc_block_status_is_detaching(dev->status)) {
			/* A curious case, detach/Upgrade command arrived before IO was ever
			   enabled (possibly during attach). In this case detach is
			   aborting the attach so do nothing */
		}
		spin_unlock_irqrestore(&dev->dp.resub.lock, flags);
		if (bio_enabled) {
			dev->topologies.dbg_num_enabling_io_toggles++;
			nvmeibc_io_resubmitter_wakeup(&dev->dp.resub);
		}
		break;
	}

	case 'P': { /* IO was preempted */
		bool newly_preempted;
		spin_lock_irqsave(&dev->dp.resub.lock, flags);
		newly_preempted = (!nvmeibc_block_status_is_detaching(dev->status) && !nvmeibc_block_status_is_preempted(dev->status));
		if (newly_preempted) {
			dev->status = NCBD_PREEMPTED;							// Appears in device status file only can not overwrite detaching
			// block_api_os_stop_accepting_kernel_io(dev->os, 'D');	// Todo: Consider, currently detach will do that
			dev->max_retry_jiffies = HZ / 100;	/* Autofail all existing IOs almost immediately */
			nvmeibc_block_set_generic_work_to_main(nvmeibc_cinst_get_blok_p(dev), dev->uuid,
					nvmeibc_cc_api_notify_io_changed, dev->uuid, false);		// Notify immediately to mgmt, dont wait for alert mechanism to stabilize on this io problem
		}
		spin_unlock_irqrestore(&dev->dp.resub.lock, flags);
		nvmeibc_io_resubmitter_wakeup(&dev->dp.resub);
		/*
		 * Per-client CDV preempt cleanup barrier (TPV_PerClientCDVPreemption.md §2.10).
		 * If this device is a CDV that was just preempted, tear down every TPV that
		 * references it so no stale CDV offsets remain in the TPV extent_map. The
		 * helper is a no-op when dev->volume is not a CDV (no matching TPV
		 * cdv_vol in nvmeibc_tpv_active_list). Called outside the spinlock.
		 */
		if (newly_preempted)
			nvmeibc_tpv_handle_cdv_preempted(dev->volume);
		break;
	}

	default:
		_NE(error_block_nvmeibc_block_update_status, DMESG_PREFIX("@DEV_NAME") ": wrong update status reason=@REASON_CHR", dev->name, reason);
		WARN_ON(true);
	}
	io_perm = nvmeibc_get_io_perm_for_reporting(dev);
	_NT(t_bdev_init_stat_change, "@EVENT_TAG: @DEV_NAME: dev state change @STR->@STR io_perm=@IO_PERM(@STR)",
	    EV_BLOCK_STATUS(), dev->name, nvmeibc_block_status_to_string(prev_status),
	    nvmeibc_block_status_to_string(dev->status), io_perm, io_perm_to_str(io_perm));
	return revalidate_bdev;
}

void nvmeibc_block_device_disk_stats_remove(struct nvmeibc_block_device *dev)
{
	remove_proc_entry(PROCFS_DISKS_STR, dev->os->procfs.dir);
}

int nvmeibc_block_init(struct nvmeibc_volume_conf *conf, struct nvmeibc_volume *volume)
{
	static u32 dbg_id_counter = 0;
	int rv = 0;
	struct nvmeibc_block_device *dev = NULL;
	struct nvmeibc_topology *t = NULL;
	const char *devname = volume->hdr.devname;
	const char *uuid	= volume->hdr.uuid;
	u64 ro_header_sectors;

	dev = (struct nvmeibc_block_device *)my_kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		_NE(error_block_nvmeibc_block_init, DMESG_PREFIX("@DEV_NAME") ": No memory for device, @HDR_UUID", devname, uuid);
		rv = -ENOMEM;
		goto err_do_exit;
	}
	nvmeibc_cinst_get_blok_p(dev) = nvmeibc_isnt_params_main2blk(volume->p);
	strlcpy(dev->name, devname, sizeof(dev->name));
	strlcpy(dev->uuid, uuid   , sizeof(dev->uuid));
	nvmeibc_volume_short_id(dev) = ((++dbg_id_counter) | (1u << 31));		// Visible bdevs to user are positive, hidden bdevs are negative
	nvmeibc_block_update_status(dev, 'A');
	INIT_LIST_HEAD(&dev->list_n);
	nvmeibc_blk_op_elevator_init(&dev->merge_op);
	INIT_LIST_HEAD(&dev->reboot_ops);

	rv = nvmeibc_topologies_init(&dev->topologies, devname);
	if (rv < 0) {
		_NE(error_1_block_nvmeibc_block_init, DMESG_PREFIX("@DEV_NAME") ": Could not init topology for device, @HDR_UUID", devname, uuid);
		goto err_do_exit;
	}
	dev->topologies.nd = dev;
	dev->topologies.error_state_update_cb = __error_state_update;

	if ((rv = nvmeibc_block___conf(conf, volume, dev)) < 0) {
		goto err_during_topo_creation;
	}
	nvmeibc_topo_init_io_perm(&dev->topologies);
	dev->type = volume->hdr.type;
	t = nvmeibc_topology_get(&dev->topologies);
	{
		const struct nvmeibc_raid1 *pr = t->chunks->raid1s;
		const int protect_level = nvmeibc_raid1_get_protect_lvl(pr);
		dev->size = t->chunks[t->nchunks-1].first_vlba;			// Note: This is not the final value, just copy from configuration.
		{
			struct nvmeibc_dp_params par = {
					.snake_size = conf->sliceWidth, .binje = nvmeibc_cinst_get_blok_p(dev)->binje,
					.slice_size = pr->slice_size, .protect_level = protect_level };
			rv = __bdev_datapath_init(dev, pr->slice_size, par, protect_level, conf->enableCrcCheck, conf->use_debug_di, conf->enableLocalReadOptimization, 0/*read_has_mutable_bio_buffers*/);
		}
		ro_header_sectors = nvmeibc_block_get_ro_header_sectors(conf);
		rv = rv ? : __blockdevice_derrived_classes_init(dev, pr->slice_size, ro_header_sectors);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
		if (!rv && !dev->os->is_proc_api_disabled) {
			rv = nvmeibc_volume_disk_stats_create(dev->os->procfs.dir, volume);
		}
#endif
	}
	nvmeibc_topology_put(t);									// Daniel, why do we hold topo here? We dont need it
	if (rv < 0) {
		_NE(error_2_block_nvmeibc_block_init, DMESG_PREFIX("@DEV_NAME") ": Initialization failed (@RV)", devname, rv);
		goto err_but_no_io;
	}
	/* hold the volume guard so the disk does not mess around with
	   pause/cont on the volume.  after the following call the volume will
	   have a block_device but the block device will not be ready - where ready
	   here means pause/cont can be called from the disk perspective.
	   only after nvmeibc_topology_register() the block device is ready
	*/
	nvmeibc_volume_set_block_device(volume, dev);
	rv = nvmeibc_topologies_register(&dev->topologies);
	if (rv < 0) {
		_NE(error_3_block_nvmeibc_block_init, DMESG_PREFIX("@DEV_NAME") ": Register failed (@RV)", devname, rv);
		goto __err_with_io;
	}
	if ((rv = t_block_clnt_globals_add_bdev(dev)) < 0)
		goto __err_with_io;
	rv = 0;
	goto out;

__err_with_io:
	nvmeibc_volume_set_block_device(volume, NULL);
	nvmeibc_wait_for_io_drain(dev, NULL, NULL);			// This is a bug! This will not drain IO properly. Need async mechanism like nvmeibc_block_try_detach()
err_but_no_io:
	nvmeibc_del_blkdev(dev);
err_during_topo_creation:
	nvmeibc_topologies_cleanup(&dev->topologies);
err_do_exit:
	nvmeibc_block_exit(dev);
out:
	return rv;
}

ulong nvmeibc_volume_get_size(const struct nvmeibc_volume* v) {
	return v->block_dev->size;
}

int t_block_clnt_globals_add_bdev(struct nvmeibc_block_device *dev)
{
	struct t_block_clnt_globals *b = nvmeibc_cinst_get_blok_g(dev);
	int rv = __block_wd_start(b);
	if (rv >= 0) {
		ulong flags;
		spin_lock_irqsave(&b->block_devices_sl, flags);
		list_add_tail(&dev->list_n, &b->block_devices);
		spin_unlock_irqrestore(&b->block_devices_sl, flags);
		_ND(t_01_cdetach, "@DEV_NAME added to block devices list", dev->name);
	}
	return rv;

}
void t_block_clnt_globals_del_bdev(struct nvmeibc_block_device *dev)
{
	struct t_block_clnt_globals *b = nvmeibc_cinst_get_blok_g(dev);
	if (!list_empty(&dev->list_n)) {
		ulong flags;
		spin_lock_irqsave(&b->block_devices_sl, flags);
		list_del_init(&dev->list_n);
		spin_unlock_irqrestore(&b->block_devices_sl, flags);
		__block_wd_stop(b);
		nvmeibc_b_cp_cpu_masks_del_all_volume_masks(b->cpu_masks, &dev->cpu_masks);
		_ND(t_02_cdetach, "@DEV_NAME deleted from block devices list", dev->name);
	} else { /* Was never added or already removed from the list */}
}

unsigned self_recovery_detach_initial_time_sec = 5; //	Auto detach after 5 seconds if no recovery started after attach
module_param(self_recovery_detach_initial_time_sec, uint, 0644);
MODULE_PARM_DESC(self_recovery_detach_initial_time_sec, "Time-out for just attached idle recoverer volume until self detached [sec]");

unsigned self_recovery_detach_idle_time_sec = 60; //	Auto detach after 60 seconds if recovery finished. This should be larger than TOMA compound recovery retry (or next recovery) timer value
module_param(self_recovery_detach_idle_time_sec, uint, 0644);
MODULE_PARM_DESC(self_recovery_detach_idle_time_sec, "Time-out for idle recoverer volume (after recovery finished) until self detached [sec]");

unsigned self_recovery_detach_carrier_grace_time_sec = 10; //	Give enough time to rider to attach after the carrier for recovery purposes, before self-detaching
module_param(self_recovery_detach_carrier_grace_time_sec, uint, 0644);
MODULE_PARM_DESC(self_recovery_detach_carrier_grace_time_sec, "Do not self detach recovery carrier volume if no rider uses it yet for this time [sec]");

static inline bool __is_still_used_carrier(struct nvmeibc_block_device *dev, const ulong up_time_sec)
{
	return (nvmeibc_block_is_any_carrier(dev) &&
			(block_api_os_is_mounted(dev->os) ||				// Has rider
			(up_time_sec <= self_recovery_detach_carrier_grace_time_sec)));
}

static void __self_detatch_old_recovering_bdev(struct nvmeibc_block_device *dev)
{
	//const struct nvmeibc_os_api *os = dev->os;	 // Safe access to 'os' because holds block_devices_sl spinlock so traversing list of devs with non destroyed os
	if (unlikely(nvmeibc_block_is_recoverer(dev))) { // Hidden. Extreme rare race condition here! while upgrading volume from hidden to visible, autodetach of hidden volume is fired. It will just fail. No one cares
		ulong time_stamp_msec = 0;
		uint num_finished = 0, n_running_weak = 0, n_running_recov = (uint)nvmeibc_recovs_drainer_get_num(&dev->dp.running_recovs, &time_stamp_msec, &num_finished, &n_running_weak);
		if ((n_running_recov - n_running_weak) > 0) {
			// Don't touch, let the recovery take care of itself even if it seems to be stuck
		} else {
			const ulong up_time_sec = (get_nvmeibc_os_api_uptime(dev->os) / HZ);
			const ulong threshold_msec = ((num_finished == 0) ? self_recovery_detach_initial_time_sec : self_recovery_detach_idle_time_sec) * 1000;
			const bool no_recov_preventers = (time_stamp_msec > threshold_msec);
			const bool no_riders_preventers = !__is_still_used_carrier(dev, up_time_sec);
			if (no_recov_preventers && no_riders_preventers) {
				_NI(info_A_block_nvmeibc_block_try_detach, "@DEV_NAME: Initiating self hidden detach, num_recov_finished=@INT, last recov @MILISECONDS ago, attached @SECONDS ago", dev->name, num_finished, time_stamp_msec, up_time_sec);
				nvmeibc_block_set_generic_work_to_main(nvmeibc_cinst_get_blok_p(dev), dev->uuid, nvmeibc_cc_api_request_self_recov_detach, (void*)dev->uuid, false);
			}
		}
	}
}

static void __block_trace_stats(void *_volume_uuid, const struct nvmeibc_cinst_params_main *p)
{
	const char *volume_uuid = (const char *)_volume_uuid;
	struct nvmeibc_volume *volume = nvmeibc_volume_get_by_uuid(p, volume_uuid, UNKNOWN_ILLEGAL);
	nvmeibc_assert_on_main_wq(p);

	if (unlikely(!volume))	/* Volume was detached - should not happen, already checked in __do_generic_work_of_volume() */
		return;

	nvmeibc_volume_trace_stats(volume);
}

static void __block_trace_stats_periodic_wakeup(struct nvmeibc_block_device *dev, unsigned long now_jiffies)
{
	nvmeibc_trace_stats_scheduling_adjust(&dev->trace_stats, now_jiffies);

	if (!nvmeibc_trace_stats_scheduling_should_trace(&dev->trace_stats, now_jiffies))
		return;

	nvmeibc_block_set_generic_work_to_main(nvmeibc_cinst_get_blok_p(dev), dev->uuid, __block_trace_stats, (void*)dev->uuid, false);

	nvmeibc_trace_stats_scheduling_set_next(&dev->trace_stats, now_jiffies);
}

int nvmeibc_block_watchdog_run_once(void *_param)
{
	struct t_block_clnt_globals *b = _param;
	struct nvmeibc_block_device *dev;
	unsigned long flags;
	unsigned long now_jiffies = jiffies;

	spin_lock_irqsave(&b->block_devices_sl, flags);
	/* Here devices list cant change so they cant be detached/attached. main-wq Will wait with those tasks*/
	/* Launch watch dogs of problematic/disabled IO allerts */
	list_for_each_entry(dev, &b->block_devices, list_n) {
		nvmeibc_io_perm_alert_periodic_wakeup(dev, now_jiffies);
		if (nvmeibc_topo_is_io_ok(&dev->topologies) && nvmeibc_io_perm_alert_should_create_readonly_topo(&dev->dp.io_perm_alert, now_jiffies)){
			nvmeibc_topologies_duplicate(&dev->topologies, NULL);		// Might induce uneeded dup while detaching or when everything is OK coz 'io_perm_alert.arm' is changed in another context
		}
		__self_detatch_old_recovering_bdev(dev);
		__block_trace_stats_periodic_wakeup(dev, now_jiffies);
	}
	spin_unlock_irqrestore(&b->block_devices_sl, flags);
	return 0;
}

struct nvmeibc_os_api *nvmeibc_block_get_os_api(struct nvmeibc_block_device *dev)
{
	return dev->os;
}

static int __test_block_busy(const struct nvmeibc_block_device *dev)
{
	int rv = 0, n_mounts = block_api_os_is_mounted(dev->os);
	if (n_mounts) {
		char info[256];
		block_api_os_dump_users(dev->os, info, sizeof(info));
		info[sizeof(info)-1] = '\0';
		_NW(trace_1_block_test_block_busy, "@DEV_NAME: is in use ref_count=@INT info: @STR",dev->name, n_mounts, info);
		rv = -EBUSY;
	}
	return rv;
}

int nvmeibc_block_try_detach(struct nvmeibc_block_device *dev, const struct nvmeibc_vol_detach_cmd how)
{
	int rv = 0;
	assert_dev_on_mainwq(dev);
	if (how.recov || how.hidden) {
		const char *type = (how.recov) ? "recovery" : "hidden";
		if (how.recov) {
			if (!nvmeibc_block_is_recoverer_or_hidden(dev)) {	// Can be recoverer and/or hidden - if niether abort detach request
				rv = -EXDEV;								// Recovery detach ignored, the volume is attached by user
			}
		} else {
			if (!nvmeibc_block_is_hidden(dev) || nvmeibc_block_is_recoverer(dev)) {	// Must only be hidden, if recoverer must be detach with recov flag
				rv = -EXDEV;								// Hidden detach ignored, the volume is attached by user
			}
		}
		if (!rv) {
			ulong time_stamp = 0;
			uint n_running_weak = 0;
			const int n_running_recov = nvmeibc_recovs_drainer_get_num(&dev->dp.running_recovs, &time_stamp, NULL, &n_running_weak);
			const int n_running_non_weak = n_running_recov - n_running_weak;
			if ((!how.force)&&(n_running_non_weak)) {
				_NT(trace_3_block_nvmeibc_block_try_detach, "@DEV_NAME: @STR detach err, @RV recoveries running", dev->name, type, n_running_non_weak);
				rv = -EBUSY;
			} else {
				// Note: All recoveries that will start here will be aborted by the detach process
				nvmeibc_block_update_status(dev, 'D');
				dev->ignore_all_toma_msgs = true;
				_NT(trace_2_block_nvmeibc_block_try_detach, "@DEV_NAME: @STR detach started, last recov finished @MILISECONDS ago", dev->name, type, time_stamp);
			}
		}
	} else if (how.force) {
		if (nvmeibc_block_is_any_carrier(dev) && block_api_os_is_mounted(dev->os)) {
			rv = -ECHILD;							// The only case where force detach can fail. Daniel: By design we dont forward the detach request to rider. If user wants, he should detach the top most rider. Also this makes the detach --all volumes more natural, as carriers disagree but rider agrees
		} else if (how.abandon) {
			nvmeibc_block_update_status(dev, 'U');
			dev->ignore_all_toma_msgs = true;		// Daniel: Why?
		} else {
			nvmeibc_block_update_status(dev, 'D');
			dev->ignore_all_toma_msgs = true;		// Daniel: Why?
		}
	} else if ((rv = __test_block_busy(dev)) == 0) {// Proceed with safe
		nvmeibc_block_update_status(dev, 'D');
	} else {
		/* Safe detach param denies the detach request, rv already set */
	}
	return rv;
}

static enum_io_perm __to_mcs_io_perm_enum(enum nvmeib_io_type_permission io_perm)
{
	switch (io_perm) {		// Daniel: Todo: consolidate the enums and remove this function. Just do +10 on the value!
		case NVMEIB_IO_TYPE_PERMIT_NEVER:				return NVMEIB_C_TO_M_IO_TYPE_PERMIT_NEVER;
		case NVMEIB_IO_TYPE_PERMIT_NONE_SUS:			return NVMEIB_C_TO_M_IO_TYPE_PERMIT_NONE_SUS;
		case NVMEIB_IO_TYPE_PERMIT_NONE_INV:			return NVMEIB_C_TO_M_IO_TYPE_PERMIT_NONE_ERR;
		case NVMEIB_IO_TYPE_PERMIT_NONE_ERR:			return NVMEIB_C_TO_M_IO_TYPE_PERMIT_NONE_ERR;
		case NVMEIB_IO_TYPE_PERMIT_NO_IO:				return NVMEIB_C_TO_M_IO_TYPE_PERMIT_NO_IO;
		case NVMEIB_IO_TYPE_PERMIT_NO_RM_IO:			return NVMEIB_C_TO_M_IO_TYPE_PERMIT_IO_PREEMPT;
		case NVMEIB_IO_TYPE_PERMIT_RDONLY:				return NVMEIB_C_TO_M_IO_TYPE_PERMIT_RDONLY;
		case NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION:	return NVMEIB_C_TO_M_IO_TYPE_PERMIT_ALL_NO_PROTECTION;
		case NVMEIB_IO_TYPE_PERMIT_ALL:      			return NVMEIB_C_TO_M_IO_TYPE_PERMIT_ALL;
		default:
			WARN(true, "invalid io_perm=%d", (int)io_perm);
			return NVMEIB_C_TO_M_IO_TYPE_PERMIT_NONE_ERR;
	}
}

enum_io_perm nvmeibc_get_io_perm_for_reporting(struct nvmeibc_block_device *dev)
{
	enum nvmeib_io_type_permission rv;			// Squashed stable result of jittering topologies io_perm, which also includes too much info
	if (dev) {
		ulong flags;
		//const enum nvmeib_io_type_permission cur_io_perm = dev->topologies.io_perm;	// Most correct but jitterning (rapidly changing, even during execution of this function)
		spin_lock_irqsave(&dev->dp.resub.lock, flags);
		if (dev->status == NCBD_PREEMPTED) { 										// Permanent state until detach
			rv = NVMEIB_IO_TYPE_PERMIT_NO_RM_IO;
		} else if (dev->status != NCBD_ATTACHING_NO_IO) {
			if (nvmeibc_io_perm_alert_is_no_bio_for_long_time(&dev->dp.io_perm_alert)) {// using mgmt allerts, has delay but does not jitter
				rv = NVMEIB_IO_TYPE_PERMIT_NONE_ERR;									// Mgmt does not care if syncs are enabled or disabled
			} else {
				rv = NVMEIB_IO_TYPE_PERMIT_ALL; 										// Assume all is good until bio is not stopped for a long time
			}
		} else {
			rv = NVMEIB_IO_TYPE_PERMIT_NEVER;											// IO, never enabled, but possibly will be
		}
		spin_unlock_irqrestore(&dev->dp.resub.lock, flags);
	} else {
		rv = NVMEIB_IO_TYPE_PERMIT_NONE_SUS;	// Treat as if dev existed but detaching
	}
	return __to_mcs_io_perm_enum(rv);
}

bool nvmeibc_block_is_during_attach_stabilization_period(struct nvmeibc_block_device *dev)
{
	bool result = false;
	if (dev){
		ulong flags;
		spin_lock_irqsave(&dev->dp.resub.lock, flags);
	
		result = nvmeibc_io_perm_alert_is_during_attach_stabilization_period(&dev->dp.io_perm_alert);
		spin_unlock_irqrestore(&dev->dp.resub.lock, flags);
	}
	return result;
}

void nvmeibc_del_blkdev(struct nvmeibc_block_device	*dev)
{
	assert_dev_on_mainwq(dev);
	t_block_clnt_globals_del_bdev(dev);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	if (dev->volume->disks_dir) {
		nvmeibc_volume_disks_stats_destroy((void*)dev->volume);
	}
#endif
	__blockdevice_derrived_classes_destroy(dev);
	__bdev_datapath_destroy(dev);
	_NT(t_03_cdetach, "@DEV_NAME was removed from the system", dev->name);
}

int nvmeibc_block_exit(struct nvmeibc_block_device *dev)		// Todo: Unify this function into previous
{
	if (dev) {
		dev->os = NULL;
		nvmeibc_blk_op_elevator_destroy(&dev->merge_op);
		nvmeibc_topologies_free(&dev->topologies);
		my_kfree(dev);
	}
	return 0;
}

int nvmeibc_block_send_mgmt_allert(const struct nvmeibc_block_device* dev, char *msg)
{
	const struct nvmeibc_cinst_params_main *pmain = nvmeibc_cinst_get_blok_m(dev);
	return nvmeibc_cc_api_send_mgmt_allert(pmain, msg, 0, false);
}

/********************* API for suspention and reviving ***********************/
int nvmeibc_block_suspend(struct nvmeibc_block_device *dev, void *susped_context, blk2blk_gen_work_t on_suspend_finish_cb)
{
	if (dev) {
		_NT(trace_block_nvmeibc_block_suspend, "volume @DEV_NAME: Suspend request arrived", dev->name);
		return nvmeibc_topologies_set_suspend_state(&dev->topologies, true,
							susped_context, on_suspend_finish_cb);
	}
	return 0;
}

int nvmeibc_block_revive(struct nvmeibc_block_device *dev)
{
	if (dev) {
		_NT(trace_block_nvmeibc_block_revive, "volume @DEV_NAME:, Revive request arrived", dev->name);
		return nvmeibc_topologies_set_suspend_state(&dev->topologies, false, NULL, NULL);
	}
	return 0;
}

int nvmeibc_block_call_for_all_disks(struct nvmeibc_block_device *dev, int (*call_fn)(struct nvmeibc_disk *disk, void *ctx), void *ctx)
{
	return nvmeibc_volume_call_for_all_vol_disks(dev->volume, call_fn, ctx);
}

u32 nvmeibc_block_bcmd_o_dbg_id(struct nvmeibc_disk_io_command *c)
{
	return c->comp.cmd->o->dbg_id;
}

u64 nvmeibc_block_get_ro_header_sectors(struct nvmeibc_volume_conf *conf)
{
	u64 ro_header_sectors = conf->isEncrypted ? conf->encryption.headerSizeBlocks : 0;
	ro_header_sectors *= (conf->blockSize >> KERNEL_SECTOR_SHIFT);
	return ro_header_sectors;
}

static void __block_trace_verb_counters_fn(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx)
{
	const struct nvmeibc_block_device *dev = ctx;
	const u32 vol_id = nvmeibc_volume_short_id(dev);

	NVMEIB_LOG_METRICS("VOL_MINOR: @VOL_ID " IO_STAT_VERB_TFMT " " IO_STAT_COUNTERS_TFMT, _I, tracer_nvmeibc, info_block_stats, vol_id, IO_STAT_VERB_TARG(verb), IO_STAT_COUNTERS_TARG(c));
}

#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS) && (NVMEIBC_ENABLE_PER_VOLUME_STATS == 1)
static void __block_disk_trace_verb_counters_fn(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx)
{
	struct nvmeibc_disk_id *d = ctx;
	const struct nvmeibc_block_device *dev = d->volume->block_dev;
	const u32 vol_id = nvmeibc_volume_short_id(dev);

	NVMEIB_LOG_METRICS("VOL_MINOR: @VOL_ID DISK: @DISK_NAME " IO_STAT_VERB_TFMT " " IO_STAT_COUNTERS_BASIC_TFMT, _T, tracer_nvmeibc, info_block_disk_stats, vol_id, d->name, IO_STAT_VERB_TARG(verb), IO_STAT_COUNTERS_BASIC_TARG(c));
}
#endif

static void __block_trace_per_disk_stats(const struct nvmeibc_block_device *dev)
{
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS) && (NVMEIBC_ENABLE_PER_VOLUME_STATS == 1)
	struct nvmeibc_disk_id *d;
	list_for_each_entry(d, &dev->volume->info.disks, link) {
		nvmeib_io_stats_trace(d->v_disk_stats, __block_disk_trace_verb_counters_fn, d);
	}
#else
	(void)dev;
#endif
}

void nvmeibc_block_trace_stats(const struct nvmeibc_block_device *dev)
{
	struct nvmeibc_os_api *os = dev->os;

	if ((!os->is_io_api_disabled) && (os->stats)) {
		nvmeib_io_stats_trace(os->stats, __block_trace_verb_counters_fn, (void *)dev);
	}

	//must remove the const in order to let io_stats update trace flag.
	dp_io_stats_periodic_trace(nvmeibc_volume_short_id(dev), (struct dp_io_stats *)&dev->dp.io_stats);

	__block_trace_per_disk_stats(dev);
}

int nvmeibc_block_get_cpu_masks(const struct nvmeibc_block_device *dev, struct nvmeib_cpu_mask_info *mask_infos, int max_masks)
{
	return nvmeibc_b_cp_cpu_masks_get_all_for_volume(__get_from_params_blok_globals_container(dev->cips)->cpu_masks, &dev->cpu_masks, mask_infos, max_masks);
}

static int nvmeibc_icore_toma_send(struct nvmeibc_icore_ops const *ops,
				   struct nvmeibc_disk *disk, u64 handle,
				   struct nvmeibc_disk_toma_send_params *params)
{
	(void)ops;
	return nvmeibc_pd_toma_send(disk, handle, params);
}

static const struct nvmeibc_icore_ops nvmeibc_core_ops_instance = {
	.toma_send = nvmeibc_icore_toma_send,
};

struct nvmeibc_icore_ops const *nvmeibc_core_ops_get(void)
{
	return &nvmeibc_core_ops_instance;
}
