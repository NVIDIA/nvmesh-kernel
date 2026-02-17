/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

//uncomment in order to enjoy locks debug logs
#if 0
#define CONFIG_DEBUG
#ifndef CONFIG_NVMEIB_DEBUG
#define CONFIG_NVMEIB_DEBUG
#endif
#define DEBUG_LOCKS
#endif
#include "nvmeib_utils.h"

#include "nvmeib_public.h"
#include "nvmeibc_disk_locks.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_ib_admin_channel.h"
#include "nvmeibc_locks_channel.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_trace.h"
#include "nvmeibc_trend_types.h"
#include "nvmeibc_pausable.h"
#include "nvmeib_completion_noise.h"
#include "common/nvmeib_cpu_masks.h"
#include "nvmeib_pcpu_wq.h"

uint nvmeibc_skip_lock_cmds_flags = 0;
module_param_named(skip_lock_cmds_flags, nvmeibc_skip_lock_cmds_flags, uint, 0644);
MODULE_PARM_DESC(skip_lock_cmds_flags, "This is an unsafe debug mode. Skip locking operations for non-EC volumes (remote and local): "
										"0 = Disabled, "
										"Bit-0 = skip cmp_exchange (regular locks), "
										"Bit-1 = skip active table locking, "
										"Bit-2 = skip read locks, "
										"Bit-3 = skip writing block-info. Used for performance tuning and debugging.");

bool nvmeibc_disk_locks_use_system_pcpu_wq = false;
module_param_named(disk_locks_use_system_pcpu_wq, nvmeibc_disk_locks_use_system_pcpu_wq, bool, 0444);
MODULE_PARM_DESC(disk_locks_use_system_pcpu_wq, "Defines whether disk locks use the system per-cpu workqueues for requests completion handling.");


#define DEUBG_SKIP_LOCKS_TX_ID 1234

// #define DEBUG_IRQS_DISABLED

/* In case max-outstanding-atomic-ops limit is reached,
   let non-atomic ops bypass atomic-ops*/
#define DEBUG_BYPASS_ATOMIC_OPS		0


#define __NFIN  _ND(__AUTOID__, "--> @CHANNEL_PTR\n", ch)
#define __NFOUT _ND(__AUTOID__, "<-- @CHANNEL_PTR\n", ch)

#define NMCS_BI_MAX_ATTEMPTS 8

static int execute_opr_local_bypass(struct nvmeibc_locks_channel *locks_channel,
									struct nvmeibc_d_rdma_comp *comp,
									struct nvmeibc_d_rdma_comp **bad_comp);

enum lock_comp_execute_mode {
	LOCK_COMP_EXECUTE_INLINE,
	LOCK_COMP_EXECUTE_ON_WQ,
	LOCK_COMP_EXECUTE_ON_WQ_THIS_CPU,
	LOCK_COMP_EXECUTE_ON_SYSTEM_PCPU_WQ,
};

static void lock_comp_execute_cb(struct nvmeibc_locks_channel *ch,
							struct nvmeibc_d_rdma_comp *comp, void *debug_ptr,
							enum lock_comp_execute_mode exec_mode);

static bool validate_handle(struct nvmeibc_disk *disk, void *handle, bool already_locked)
{
	bool ret = false;
#ifdef DEBUG_LOCKS
	struct nvmeibc_disk_used_lock_segment *uls;
	unsigned long flags;
#endif

	NFIN;
#ifdef DEBUG_LOCKS
	if (!already_locked)
		spin_lock_irqsave(&disk->spinlock, flags);
	list_for_each_entry(uls, &disk->used_lock_segments, link) {
		if ((void *)(uls) == handle) {
			ret = true;
			break;
		}
	}
	if (!already_locked)
		spin_unlock_irqrestore(&disk->spinlock, flags);

	if (!ret)
		_NE(validate_handle_e1,
			"handle validation error on disk %s\n", disk->name);
#else
	ret = true;
#endif

	NFOUT;
	return ret;
}



#ifdef DEBUG_LOCKS
static void MyWait(void)
{
	u64 i = jiffies;
	while (i >= jiffies);
}

static void lock_check_data(struct nvmeibc_locks_channel *ch)
{
	struct nvmeibc_d_rdma_comp *itr;
	struct nvmeibc_lock_opr_in_progress *opr;
	int i;

	__NFIN;

	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(ch));

	list_for_each_entry(itr, &ch->defered, link) {
		BUG_ON(itr == NULL || itr->mark);
		BUG_ON(itr->mem_info == NULL);
		itr->mark = true;
	}

	list_for_each_entry(opr, &ch->in_progress, link) {
		BUG_ON(opr->comp == NULL);
		BUG_ON(opr->comp->mem_info == NULL);
		BUG_ON(opr->comp->mark == true);
		opr->comp->mark = true;
	}
	i = 0;
	list_for_each_entry(opr, &ch->free_ip_pool, link) {
		BUG_ON(opr->comp != NULL);
		++i;
	}

	BUG_ON(i != ch->num_of_free);

	list_for_each_entry(itr, &ch->defered, link) {
		itr->mark = false;
	}

	list_for_each_entry(opr, &ch->in_progress, link) {
		opr->comp->mark = false;
	}

	__NFOUT;

}
#else
static void lock_check_data(struct nvmeibc_locks_channel *ch)
{
}

static void MyWait(void)
{
}
#endif

static bool find_mem_info_from_set(
	struct nvmeibc_disk_segments_locks *disk_segs_locks,
	struct nvmeibc_disk *disk, struct nvmeibc_disk_used_lock_segment *handle)
{
	int i;
	struct nvmeibc_disk_seg_locks_mem_info *lock;
	bool found = false;

	NFIN;
	_ND(trace_disk_locks_find_mem_info_from_set, "LOCKS: number of segments: @NUM_OF_SEGMENTS in @DISK_SEGS_LOCKS @LOCKS",
		disk_segs_locks->num_of_segments, disk_segs_locks,
		disk_segs_locks->locks);
	for (i = 0; i < disk_segs_locks->num_of_segments; ++i) {
		lock = &(disk_segs_locks->locks[i]);
		if (!lock) {
			_NE(error_disk_locks_find_mem_info_from_set, "lock information is not initialized ignoring this segment");
			continue;
		}
		if (lock->seg_id == 0 || lock->seg_id == handle->seg_id) {
			if (disk->info) {
				lock->lock_id = disk->info->lock_id;
			}
			_ND(trace_1_disk_locks_find_mem_info_from_set, "Using @LOCKID", (u32)lock->lock_id);
			found = true;
			handle->mem_info = lock;

			_ND(trace_2_disk_locks_find_mem_info_from_set, "LOCKS: segment @SEG_ID_INT is registered as used under @DISK",
				handle->seg_id, disk);
			break;
		}
	}

	if (!found)
		_NW(warn_disk_locks_find_mem_info_from_set, "Cannot locate segment lock, try again when the disk will be "
			"connected");
	NFOUT;
	return found;
}

static void *get_mem_info_from_set(
	struct nvmeibc_disk_segments_locks *disk_segs_locks,
	struct nvmeibc_disk *disk, int seg_id)
{
	struct nvmeibc_disk_used_lock_segment *handle = NULL;

	NFIN;
	BUG_ON(disk == NULL);

	handle = kzalloc(sizeof(struct nvmeibc_disk_used_lock_segment), GFP_ATOMIC);
	if (!handle) {
		_NE(error_disk_locks_get_mem_info_from_set, "unable to allocate disk used lock segment");
		goto out;
	}
	handle->seg_id = seg_id;
	handle->mem_info = NULL;
	handle->disk = disk;
	_ND(trace_disk_locks_get_mem_info_from_set, "Adding to the used_lock_segments handle @HANDLE_PTR disk_seg_locks=@DISK_SEG_LOCKS",
		handle, disk_segs_locks);
	list_add_tail(&handle->link, &disk->used_lock_segments);
	if (disk_segs_locks) {
		_ND(trace_1_disk_locks_get_mem_info_from_set, "calling find_mem_info_from_set disk=@DISK_STR, disk_segs_locks=@DISK_SEGS_LOCKS,"
			" locks=@LOCKS handle =@HANDLE_PTR", disk->name, disk_segs_locks,
			disk_segs_locks->locks, handle);
		if (!find_mem_info_from_set(disk_segs_locks, disk, handle))
			_NW(warn_disk_locks_get_mem_info_from_set, "cannot provide lock info, disk is probably disconnected");
	}

out:
	_ND(trace_2_disk_locks_get_mem_info_from_set, "Provided handle:");
	_ND(trace_3_disk_locks_get_mem_info_from_set, "LOCKS: uls=@ULS mem_info=@MEM_INFO diskp=@DISKP disk=@DISK_STR", handle, handle->mem_info,
		disk, disk->name);
	NFOUT;
	return (void*)handle;
}

void *nvmeibc_disk_locks_seg_locks_mem_info(struct nvmeibc_disk *disk,
	int seg_id)
{
	void *ret = NULL;
	struct nvmeibc_disk_segments_locks *disk_segs_locks = NULL;

	NFIN;
	BUG_ON(!disk);
	disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
												  (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 });
	if (!disk_segs_locks)
		_NT(trace_disk_locks_nvmeibc_disk_locks_seg_locks_mem_info, "Admin channel is not connected yet");

	_ND(trace_1_disk_locks_nvmeibc_disk_locks_seg_locks_mem_info, "SEG_CR: calling  get_mem_info_from_set disk: @DISK_NAME, seg_id=@SEG_ID_INT",
		disk->name, seg_id);
	ret = get_mem_info_from_set(disk_segs_locks, disk, seg_id);
	if (!ret)
		_NE(error_disk_locks_nvmeibc_disk_locks_seg_locks_mem_info, "seg @SEG_ID_INT is not defined on disk @DISK_FULL_NAME",seg_id, disk->full_name);

	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });
	NFOUT;
	return ret;
}

void nvmeibc_disk_locks_free_mem_info(void *handle)
{
	unsigned long flags;
	struct nvmeibc_disk_used_lock_segment *uls = handle;
	struct nvmeibc_disk *disk = uls->disk;

	NFIN;
	/*in case of seg 0 wen need to chech if this is the last segment
	 in the disk*/
	_NT(trace_disk_locks_nvmeibc_disk_locks_free_mem_info, "Free locks mem info: handle=@HANDLE_PTR disk=@DISK", handle, disk);
	spin_lock_irqsave(&disk->spinlock, flags);
	if (!validate_handle(disk, handle, true)) {
		_NT(trace_1_disk_locks_nvmeibc_disk_locks_free_mem_info, "Handle could not be validated on disk @DISK_NAME", disk->name);
		goto out;
	}
	if (uls) {
		//_NE(nvmeibc_disk_locks_free_mem_info_e1,
		//	"working with that uls num_of_segs @PTR", uls);
		list_del_init(&uls->link); /* list-del from &disk->used_lock_segments */
		uls->disk = (void *)0xCECECECECECECECE;
		kfree(uls);
	}

out:
	spin_unlock_irqrestore(&disk->spinlock, flags);
	NFOUT;
}

void nvmeibc_disk_locks_attach_used_segments(struct nvmeibc_disk *disk,
											 struct nvmeibc_disk_segments_locks *disk_segs_locks)
{
	struct nvmeibc_disk_used_lock_segment *uls;
	struct nvmeibc_disk_seg_locks_mem_info *lock;
	int i, seg_id;

	NFIN;
	BUG_ON(disk == NULL);
	BUG_ON(disk->set_locks_ref_count < 0);
	_NT(trace_disk_locks_nvmeibc_disk_locks_attach_used_segments, "LOCKS: iterating and attaching under disk @DISK_NAME", disk->name);
	list_for_each_entry(uls, &disk->used_lock_segments, link) {
		_NT(trace_1_disk_locks_nvmeibc_disk_locks_attach_used_segments, "LOCKS: checking for segment @SEG_ID_INT", uls->seg_id);
		if (uls->mem_info != NULL) //Previous disconnect must have null this
		{
			_NT(trace_2_disk_locks_nvmeibc_disk_locks_attach_used_segments, "BUG: memory info remains not null when reattching handles"
			   " for disk @DISK_NAME", disk->name);
			uls->mem_info = NULL;
		}
		seg_id = uls->seg_id;
		_ND(trace_3_disk_locks_nvmeibc_disk_locks_attach_used_segments, "LOCKS: looking for segment @SEG_ID_INT", seg_id);
		_ND(trace_4_disk_locks_nvmeibc_disk_locks_attach_used_segments, "LOCKS: num_of_segments=@NUM_OF_SEGMENTS", disk_segs_locks->num_of_segments);
		for (i=0; i < disk_segs_locks->num_of_segments; ++i) {
			lock = &(disk_segs_locks->locks[i]);
			if (lock->seg_id == seg_id || lock->seg_id == 0) {
				_ND(trace_5_disk_locks_nvmeibc_disk_locks_attach_used_segments, "LOCKS: re-attching handle @ULS segment @SEG_ID_INT to @LOCK",
					uls, uls->seg_id, lock);
				uls->mem_info = lock;
				break;
			}
		}
		if (uls->mem_info == NULL)
			_NE(error_disk_locks_nvmeibc_disk_locks_attach_used_segments, "could not reattach segment @SEG_ID_INT", uls->seg_id);
	}

	NFOUT;
}

u64 __attribute__ ((unused)) nvmeibc_disk_lock_get_lock_id(void *handle)
{
	u64 ret = 0;
	struct nvmeibc_disk_used_lock_segment *uls;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;
	struct nvmeibc_disk *disk;

	NFIN;
	BUG_ON(handle == NULL);

	uls = handle;
	if (!validate_handle(uls->disk, handle, false)) {
		_NE(error_disk_locks_nvmeibc_disk_lock_get_lock_id, "handle could not be validate on disk @DISK_NAME", uls->disk->name);
		ret = -1;
		goto out;
	}

	if (uls->mem_info == NULL) {
		disk = uls->disk;
		disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
													  (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 });

		if (!disk_segs_locks) {
			_NE(error_1_disk_locks_nvmeibc_disk_lock_get_lock_id, "locks channel is not connected");
			ret = -EAGAIN;
			goto out;
		}

		find_mem_info_from_set(disk_segs_locks, disk, uls);
		nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });
		/*recheck that we have lock mem info*/
		if (uls->mem_info == NULL) {
			_NE(error_2_disk_locks_nvmeibc_disk_lock_get_lock_id, "locks channel is not connected");
			ret = -EAGAIN;
			goto out;
		}
	}
	ret = uls->mem_info->lock_id;

out:
	NFOUT;
	return ret;
}

static void locks_free_opr(struct nvmeibc_lock_opr_in_progress *opr_ip,
	bool counted, enum lock_opr_state lock_opr_state, enum lock_opr_rdma_bypass_state bypass_state)
{
	struct nvmeibc_locks_channel *ch = opr_ip->ch;
	struct nvmeibc_d_rdma_comp *comp = opr_ip->comp;

	__NFIN;
	BUG_ON(comp == NULL || comp == (void *)CONFIG_ILLEGAL_POINTER_VALUE);
	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(ch));
	_ND(trace_disk_locks_locks_free_opr, "Stopping watchdog");
	if (!opr_ip->disk_piggyb)
		nvmeib_wd_stop_wdc(&opr_ip->wdc);
	NVMEIBC_LOCK_GUARD_GET_CHECK(locks_free_opr_e7,
		&opr_ip->state, lock_opr_state);
	NVMEIBC_LOCK_GUARD_GET_CHECK(locks_free_opr_e8,
		opr_ip->bypass_state, bypass_state);
	list_del_init(&opr_ip->link); /*remove it from in_progress*/
	_ND(trace_1_disk_locks_locks_free_opr, "Delete from list");
	if (counted) {
		BUG_ON(ch->num_in_progress <= 0);
		if (opr_ip->disk_piggyb) {
			ch->num_disk_piggyb--;
			BUG_ON(ch->num_disk_piggyb < 0);
			if (!opr_ip->aborted)
				opr_ip->disk_piggyb = false;
		}
		ch->num_in_progress--;
		if (nvmeib_send_wr_common(opr_ip->wr).opcode != IB_WR_RDMA_WRITE)
			ch->num_of_atom_read_ip--;
	}
	opr_ip->disk_piggyb = false;
	if (!opr_ip->aborted) {
		opr_ip->version++;
		opr_ip->comp = (void *)CONFIG_ILLEGAL_POINTER_VALUE;
		_ND(trace_2_disk_locks_locks_free_opr, "Adding to the free pool");
		NVMEIBC_LOCK_GUARD_SWITCH_CHECK(locks_free_opr_e9,
			&opr_ip->state, lock_opr_state, LOCK_OPR_IN_FREE_LIST);
		NVMEIBC_LOCK_GUARD_SWITCH_CHECK(locks_free_opr_e10,
			&opr_ip->bypass_state, bypass_state, LOCK_OPR_NO_BYPASS);
		list_add_tail(&opr_ip->link, &ch->free_ip_pool);
		ch->num_of_free++;
	} else {
		NVMEIBC_LOCK_GUARD_SWITCH_CHECK(locks_free_opr_e11,
			&opr_ip->state, lock_opr_state, LOCK_OPR_ABORTED);
		list_add_tail(&opr_ip->link, &ch->aborted);
		ch->num_aborted++;
	}
	#ifdef DEBUG_D_RDMA_COMP
		comp->in_prog_data = NULL;
	#endif
	__NFOUT;
}

static bool lock_comp_is_expired(struct nvmeibc_d_rdma_comp *comp)
{
	unsigned long now_jif = jiffies;
	if (now_jif > comp->deferred_jif + NVMEIB_MAX_LOCK_TIME_INC_RETRIES)
		return true;
	return false;
}

/* This fn just does the drain, the callbacks are processed by calling fn */
static void drain_defered_queue(struct nvmeibc_locks_channel *ch, bool only_expired,
				  struct list_head *defered_list, int *num_defered,
				  int *num_in_progress)
{
	struct nvmeibc_d_rdma_comp *defered;
	unsigned long flags;

	nvmeibc_locks_channel_spin_lock_irqsave(ch, flags);
	if (only_expired) {
		while ((defered = list_first_entry_or_null(&ch->defered, struct nvmeibc_d_rdma_comp, link_deferred)) &&
				lock_comp_is_expired(defered)) {
			list_del(&defered->link_deferred);
			list_add_tail(&defered->link_deferred, defered_list);
			(*num_defered)++;
			ch->num_defered--;
		}
	} else {
		/* splice deferred list */
		list_splice_init(&ch->defered, defered_list);
		BUG_ON(!list_empty(&ch->defered));
		(*num_defered) += ch->num_defered;
		ch->num_defered = 0;
	}
	if (num_in_progress)
		(*num_in_progress) += ch->num_in_progress;

	nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags);
}

static void fail_deferred(struct nvmeibc_locks_channel *ch,
			    struct list_head *defered_list, int *num_defered, int *num_in_progress)
{
	struct nvmeibc_d_rdma_comp *defered;
	unsigned long now_jif = jiffies;
	while ((defered = list_first_entry_or_null(defered_list, struct nvmeibc_d_rdma_comp, link_deferred))) {
		list_del_init(&defered->link_deferred);
		(*num_defered)--;
		BUG_ON(*num_defered < 0);
		defered->lock_status = NCL_STATUS_FAIL_COMP;
		_NT(trace_disk_locks_drain_defered_queue,
		    "LOCKS: completing lock-comp @LOCK_COMP because of error / expiration - "
		    " in progress @NUM_IN_PROGRESS deferred = @NUM_DEFERED @CALLBACK_PTR status "
		    " @LOCK_STATUS_INT time-passed @LD",
			defered, *num_in_progress, *num_defered, defered->callback,
			defered->lock_status, now_jif - defered->deferred_jif);
		if (defered->callback) {
			nvmeibc_disk_cmds_stats_pending_aborted(ch->base.disk, &defered->lock_cmd);
			lock_comp_execute_cb(ch, defered, NULL, LOCK_COMP_EXECUTE_INLINE);
		}
		else {
			BUG();
		}
	}
	BUG_ON(*num_defered != 0);
}

struct drain_deferred_pcpu_ch_fn_info {
	struct nvmeibc_locks_channel *primary_ch;
	bool only_expired;
	struct {
		struct list_head deferred_list;
		int num_deferred;
		int num_in_progress;
	} pcpu[NVMEIB_DFLT_MAX_CPUS];
};

static void drain_deferred_pcpu_ch_fn(void *arg)
{
	struct drain_deferred_pcpu_ch_fn_info *info = arg;
	struct nvmeibc_locks_channel *pcpu_ch, *primary_ch = info->primary_ch;
	int cpu = get_cpu();
	unsigned long flags;

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		goto put_cpu;

	local_irq_save(flags); /* this may not be necessary as we are in IPI context (need to verify) */
	pcpu_ch = primary_ch->_2nd_ch_pcpu_map[cpu];
	if (!pcpu_ch) {
		_NE(err_drain_deferred_pcpu_ch_fn_2nd_ch_null,
		    "primary lock-ch=@LOCK_CH - pcpu 2nd channel for cpu @CPU is null", primary_ch, cpu);
		goto unlock;
	}

	_NT(trace_1_drain_deferred_pcpu_ch_fn, "Draining pcpu [@CPU] lock-ch @LOCK_CH (primary @LOCK_CH) for disk @DISK_NAME",
	    cpu, pcpu_ch, primary_ch, primary_ch->base.disk->name);

	drain_defered_queue(pcpu_ch,
			info->only_expired,
			&info->pcpu[cpu].deferred_list,
			&info->pcpu[cpu].num_deferred,
			&info->pcpu[cpu].num_in_progress);

	_NT(trace_drain_deferred_pcpu_ch_fn,
	    "Drained pcpu [@CPU] lock-ch @LOCK_CH (primary @LOCK_CH) for disk @DISK_NAME -"
	    " only_expired @BOOL, deferred @NUM_DEFERED, in-progress @NUM_IN_PROGRESS",
		cpu, pcpu_ch, primary_ch, primary_ch->base.disk->name,
		info->only_expired, info->pcpu[cpu].num_deferred, info->pcpu[cpu].num_in_progress);

unlock:
	local_irq_restore(flags);

put_cpu:
	put_cpu();
}

void nvmeibc_disk_locks_drain_defered(struct nvmeibc_locks_channel *ch, bool only_expired, bool this_ch_only)
{
	int i, num_in_progress = 0, num_defered = 0;
	LIST_HEAD(defered_list);

	__NFIN;
	/*clean up the defered transactions. the rest will be cleaned by
	 *  watchdog*/
	drain_defered_queue(ch,
				only_expired,
				&defered_list,
				&num_defered,
				&num_in_progress);

	if (ch->primary_ch == ch && !this_ch_only) {
		/* Drain secondary channels also */
		if (!ch->_2nd_ch_pcpu || !ch->_2nd_ch_pcpu_lockless) {
			for (i = 0; i < ch->n_2nd_ch; i++) {
				struct nvmeibc_locks_channel *_2nd_ch = ch->_2nd_ch[i];
				if (_2nd_ch) {
					drain_defered_queue(_2nd_ch,
							only_expired,
							&defered_list,
							&num_defered,
							&num_in_progress);
				}
			}
		} else {
			struct drain_deferred_pcpu_ch_fn_info *info = kzalloc(sizeof(*info), GFP_KERNEL);
			if (info) {
				int cpu;
				/* Drain from each pcpu channel into the list. Need to jump onto the CPU of the pcpu channel to do so */
				info->primary_ch = ch;
				info->only_expired = only_expired;
				for (cpu = 0; cpu < NVMEIB_DFLT_MAX_CPUS; cpu++) {
					INIT_LIST_HEAD(&info->pcpu[cpu].deferred_list);
				}

				/* From the kernel doc:
				 * 	"You must not call this function with disabled interrupts or from a hardware interrupt handler or from a bottom half handler."
				 */
				BUG_ON(irqs_disabled() || in_interrupt());

				on_each_cpu_mask(ch->_2nd_ch_pcpu_mask, drain_deferred_pcpu_ch_fn, info, true);

				for_each_cpu(cpu, ch->_2nd_ch_pcpu_mask) {
					list_splice_init(&info->pcpu[cpu].deferred_list, &defered_list);
					num_defered += info->pcpu[cpu].num_deferred;
					num_in_progress += info->pcpu[cpu].num_in_progress;
				}
				kfree(info);
			} else {
				_NE_dmesg(err_nvmeibc_disk_locks_drain_defered_oom, "OOM");
				WARN_ON(1);
			}
		}
	}

	_NT(trace_disk_locks_drain_defered, "loch-ch @LOCK_CH for disk @DISK_NAME - "
		"deferred_list empty: @BOOL, num_deferred: @NUM_DEFERED, "
		"num_in_progress: @NUM_IN_PROGRESS",
		ch, ch->base.disk->name, list_empty(&defered_list), num_defered,
		num_in_progress);

	/* fail deferred transactions */
	fail_deferred(ch, &defered_list, &num_defered, &num_in_progress);
	__NFOUT;
}

static void lock_ch_pause(struct nvmeibc_locks_channel *ch)
{
	unsigned long flags;

	BUG_ON(nvmeibc_channel_is_ll_pcpu_ch(&ch->base));
	nvmeibc_locks_channel_spin_lock_irqsave(ch, flags);
	ch->paused = true;
	nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags);
}

static void lock_ch_pause_pcpu_fn(void *info)
{
	struct nvmeibc_locks_channel *primary_ch = info;
	struct nvmeibc_locks_channel *pcpu_ch;
	int cpu = get_cpu();
	unsigned long flags;

	local_irq_save(flags); /* this may not be necessary as we are in IPI context (need to verify) */
	pcpu_ch = primary_ch->_2nd_ch_pcpu_map[cpu];
	if (!pcpu_ch) {
		_NE(err_lock_ch_pause_pcpu_fn_2nd_ch_null,
		    "primary lock-ch=@LOCK_CH - pcpu 2nd channel for cpu @CPU is null", primary_ch, cpu);
		goto unlock;
	}

	pcpu_ch->paused = true;
	_NT(trace_lock_ch_pause_pcpu_fn,
	    "Paused pcpu [@CPU] lock-ch @LOCK_CH (primary @LOCK_CH) for disk @DISK_NAME",
	    cpu, pcpu_ch, primary_ch, primary_ch->base.disk->name);

unlock:
	local_irq_restore(flags);
	put_cpu();
}

void nvmeibc_disk_locks_abort_all_oprs(struct nvmeibc_locks_channel *ch)
{
	struct nvmeibc_locks_channel *_2nd_ch;
	int i;
	__NFIN;

	_NT(trace_disk_locks_abort_all_oprs, "ch @LOCK_CH", ch);

	/* First, pause all channels */
	if (ch->_2nd_ch_pcpu && ch->_2nd_ch_pcpu_lockless) {
		/* From the kernel doc:
		 * 	"You must not call this function with disabled interrupts or from a hardware interrupt handler or from a bottom half handler."
		 */
		BUG_ON(irqs_disabled() || in_interrupt());

		on_each_cpu_mask(ch->_2nd_ch_pcpu_mask, lock_ch_pause_pcpu_fn, ch, true);
	} else {
		for (i = 0; i < ch->n_2nd_ch; i++) {
			_2nd_ch = ch->_2nd_ch[i];
			if (_2nd_ch) {
				lock_ch_pause(_2nd_ch);
			}
		}
	}
	lock_ch_pause(ch);

	_NT(trace_1_disk_locks_abort_all_oprs, "drain deferred @LOCK_CH", ch);

	/* Drain all deferred and pending ops from all channels */
	nvmeibc_disk_locks_drain_defered(ch, false, false);

	_NT(trace_2_disk_locks_abort_all_oprs, "abort in-progress @LOCK_CH", ch);

	/* Abort all in-progress ops from all channels */
	nvmeibc_disk_locks_abort_in_progress_oprs(ch, false);

	__NFOUT;
}

static void abort_in_progress_ops(struct nvmeibc_locks_channel *ch,
				  struct list_head *comp_list, int *num_in_progress)
{
	struct nvmeibc_lock_opr_in_progress *opr_ip, *opr_t;
	int time_passed, opr_time_passed;
	unsigned long now = jiffies;
	unsigned long flags;
	enum lock_opr_rdma_bypass_state bypass_state = LOCK_OPR_BYPASS_INVALID;
	struct nvmeibc_d_rdma_comp *comp;

	NFIN;
	nvmeibc_locks_channel_spin_lock_irqsave(ch, flags);
	list_for_each_entry_safe(opr_ip, opr_t, &ch->in_progress, link) {
#if	defined(NVMEIBC_LOCKS_CHANNEL_GUARD_STATE) && (NVMEIBC_LOCKS_CHANNEL_GUARD_STATE == 1)
		bypass_state = LOCK_OPR_NO_BYPASS;
		if (opr_ip->disk_piggyb) {
			bypass_state = nvmeib_get_state_guard(&opr_ip->bypass_state);
			_NW(warn_disk_locks_nvmeibc_disk_locks_abort_in_progress_oprs, "lock ch @BASE_NAME (@CH_PTR) opr @INDEX (@OPR_IP) outstanding as disk cmd piggyback bypass_state: @LOCK_OPR_RDMA_BYPASS_STATE prev: @PREV next: @NEXT",
			   ch->base.name, ch, opr_ip->index, opr_ip, bypass_state, opr_ip->disk_lock_cmd.disk_cmd.dcmd_link.prev, opr_ip->disk_lock_cmd.disk_cmd.dcmd_link.next);
		}
#endif
		opr_ip->aborted = true;
		time_passed = now - opr_ip->wdc.called_on;
		opr_time_passed = now - opr_ip->start_time;
		comp = opr_ip->comp;
		_NT(trace_disk_locks_nvmeibc_disk_locks_abort_in_progress_oprs, "LOCKS: Channel: @BASE_NAME (@CH_PTR) opr_ip=@OPR_IP, comp=@COMP, cb=@CALLBACK_PTR, status=@STATUS "
			"WD : Time-passed @UINT(@UINT), timeout @WD_TIMEOUT_JIF(@WD_TIMEOUT_JIF), "
			"Opr: Start-time: @START_TIME, Time-passed @OPR_TIME_PASSED(@OPR_TIME_PASSED)",
			ch->base.name, ch, opr_ip, comp, comp->callback, comp->lock_status,
			time_passed, time_passed / HZ,
			ch->net.wd_timeout_jif, ch->net.wd_timeout_jif / HZ,
			opr_ip->start_time, opr_time_passed, opr_time_passed/HZ);

		comp->lock_status = NCL_STATUS_FAIL_COMP;
		locks_free_opr(opr_ip, true, LOCK_OPR_IN_PROGRESS, bypass_state);
		list_add_tail(&comp->link_deferred, comp_list);
		(*num_in_progress)++;
	}
	ch->num_of_series_ip = 0;
	nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags);
	NFOUT;
}

static void call_comp_list_cb(struct nvmeibc_locks_channel *ch,
			      struct list_head *comp_list,
			      int num_comp,
			      enum lock_comp_execute_mode exec_mode)
{
	struct nvmeibc_d_rdma_comp *comp;
	while ((comp = list_first_entry_or_null(comp_list, typeof(*comp), link_deferred))) {
		list_del_init(&comp->link_deferred);
		num_comp--;
		BUG_ON(num_comp < 0);
		if (comp->callback) {
			nvmeibc_disk_cmds_stats_pending_aborted(ch->base.disk, &comp->lock_cmd);
			lock_comp_execute_cb(ch, comp, NULL, exec_mode);
		}
		else {
			BUG();
		}
	}
	BUG_ON(num_comp != 0);
}

struct abort_in_progress_pcpu_ch_fn_info {
	struct nvmeibc_locks_channel *primary_ch;
	struct {
		struct list_head comp_list;
		int num_comp;
	} pcpu[NVMEIB_DFLT_MAX_CPUS];
};

static void abort_in_progress_pcpu_ch_fn(void *arg)
{
	struct abort_in_progress_pcpu_ch_fn_info *info = arg;
	struct nvmeibc_locks_channel *pcpu_ch, *primary_ch = info->primary_ch;
	int cpu = get_cpu();
	unsigned long flags;

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		goto put_cpu;

	local_irq_save(flags); /* this may not be necessary as we are in IPI context (need to verify) */
	pcpu_ch = primary_ch->_2nd_ch_pcpu_map[cpu];
	if (!pcpu_ch) {
		_NE(err_abort_in_progress_pcpu_ch_fn_2nd_ch_null,
		    "primary lock-ch=@LOCK_CH - pcpu 2nd channel for cpu @CPU is null", primary_ch, cpu);
		goto unlock;
	}

	_NT(trace_1_abort_in_progress_pcpu_ch_fn, "Aborting in progress for pcpu [@CPU] lock-ch @LOCK_CH (primary @LOCK_CH) for disk @DISK_NAME",
	    cpu, pcpu_ch, primary_ch, primary_ch->base.disk->name);

	abort_in_progress_ops(pcpu_ch, &info->pcpu[cpu].comp_list, &info->pcpu[cpu].num_comp);

	_NT(trace_abort_in_progress_pcpu_ch_fn,
	    "Aborted in progress for pcpu [@CPU] lock-ch @LOCK_CH (primary @LOCK_CH) for disk @DISK_NAME -"
	    " num-comp: @COUNT", cpu, pcpu_ch, primary_ch, primary_ch->base.disk->name, info->pcpu[cpu].num_comp);

unlock:
	local_irq_restore(flags);
put_cpu:
	put_cpu();
}

void nvmeibc_disk_locks_abort_in_progress_oprs(struct nvmeibc_locks_channel *ch,
					       bool this_ch_only)
{
	LIST_HEAD(comp_list);
	int i, num_in_progress = 0;

	NFIN;
	abort_in_progress_ops(ch, &comp_list, &num_in_progress);

	if (!this_ch_only) {
		struct nvmeibc_locks_channel *_2nd_ch;
		if (!ch->_2nd_ch_pcpu || !ch->_2nd_ch_pcpu_lockless) {
			for (i = 0; i < ch->n_2nd_ch; i++) {
				_2nd_ch = ch->_2nd_ch[i];
				if (_2nd_ch) {
					_NT(trace3_disk_locks_abort_all_oprs,
					"Abort in-progress and deferred lock oprs of 2nd-lock-ch=@LOCK_CH, @INT", _2nd_ch, i);
					abort_in_progress_ops(_2nd_ch, &comp_list, &num_in_progress);
				}
			}
		} else {
			struct abort_in_progress_pcpu_ch_fn_info *info = kzalloc(sizeof(*info), GFP_KERNEL);
			int cpu;
			if (info) {
				/* Abort from each pcpu channel into one big list. Need to jump onto the CPU of the pcpu channel to do so */
				info->primary_ch = ch;
				for (cpu = 0; cpu < NVMEIB_DFLT_MAX_CPUS; cpu++) {
					INIT_LIST_HEAD(&info->pcpu[cpu].comp_list);
				}

				/* From the kernel doc:
				 * 	"You must not call this function with disabled interrupts or from a hardware interrupt handler or from a bottom half handler."
				 */
				BUG_ON(irqs_disabled() || in_interrupt());

				on_each_cpu_mask(ch->_2nd_ch_pcpu_mask, abort_in_progress_pcpu_ch_fn, info, true);

				for_each_cpu(cpu, ch->_2nd_ch_pcpu_mask) {
					list_splice_init(&info->pcpu[cpu].comp_list, &comp_list);
					num_in_progress += info->pcpu[cpu].num_comp;
				}

				kfree(info);
			} else {
				_NE_dmesg(err_nvmeibc_disk_locks_abort_in_progress_oprs_pcpu_oom, "OOM");
				WARN_ON(1);
			}
		}
	}

	call_comp_list_cb(ch, &comp_list, num_in_progress, LOCK_COMP_EXECUTE_INLINE);

	NFOUT;
}

/* Called from nvmeibc_disk_locks_handle_wd_event */
static void locks_handle_qp_error(struct nvmeibc_locks_channel *ch)
{
/*
	struct nvmeibc_lock_opr_in_progress *opr_ip, *opr_next;
    struct nvmeibc_d_rdma_comp *comp;
 */
	/* EC-3929: see description below... */
#if 0

	struct nvmeibc_disk *disk;
	struct nvmeibc_ib_net *net = &ch->net;
	struct nvmeibc_admin_channel *admin_ch = net->admin_ch;
	struct nvmeibc_ib_admin_channel *admin_ib_ch =
		container_of(admin_ch, struct nvmeibc_ib_admin_channel, base);
	struct nvmeibc_ib_net_admin *net_admin = &admin_ib_ch->net;
#endif

	__NFIN;
	_NT(error_disk_locks_locks_handle_qp_error, "Lock channel @IOCH_NAME qp-error from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
	   ch->net.ioch->name, __builtin_return_address(0));
	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(ch));

	/* Set the channel dying while we are still stuck under lock */
	nvmeibc_locks_channel_disconnect(ch);
	ch->paused = true;

	_ND(trace_disk_locks_locks_handle_qp_error, "LOCKS: in_progress @NUM_IN_PROGRESS deferred @NUM_DEFERED",
		ch->num_in_progress, ch->num_defered);

	/* EC-3929:
	   Remove this bug-pron code of unlocking inner lock; when here locking order is:
	   wd->spinlock --> net->ioch->spinlock --> ch->locks_spinlock --> wdc->spinlock

	   Also, we dont call disk-pause which means we only call it from stop-disk-io and
	   we only complete all in-progress and derferred lock-operation till lock-channel
	   disconnect. Thus, we add complete these lock-oprs from admin-ch-disconnect i.e.
	   after stop-disk-io
	*/
#if 0
	disk = nvmeibc_ib_admin_channel_disk(&net_admin->base);
	ch->paused = true;
	spin_unlock_irqrestore(&ch->locks_spinlock, *flags);

	nvmeibc_disk_pause(disk);

/* Moved this to function to be reused from locks-remove-work

	spin_lock_irqsave(&ch->locks_spinlock, *flags);
	list_for_each_entry_safe(opr_ip, opr_next, &ch->in_progress, link) {
	//EXC-1795: not needed, locks_free_opr() will call stop-wd
	//atomic_set(&opr_ip->wdc.watchdog_armed, 0);
		comp = opr_ip->comp;
		comp->lock_status = NCL_STATUS_FAIL_COMP;
		locks_free_opr(opr_ip, true);
		_ND(locks_handle_qp_error_d1,
			"LOCKS: @PTR status @INT", comp->callback, comp->lock_status);
		if (comp->callback) {
			#ifdef DEBUG_D_RDMA_COMP
				comp->callback_line = __LINE__;
			#endif
			spin_unlock_irqrestore(&ch->locks_spinlock, *flags);
			comp->callback(comp);
			spin_lock_irqsave(&ch->locks_spinlock, *flags);
		}
	}
	ch->num_of_series_ip = 0;
	spin_unlock_irqrestore(&ch->locks_spinlock, *flags);
*/

	/* Jared: Restored to prevent delay on pausing/cont'ing disk */
	nvmeibc_disk_locks_abort_in_progress_oprs(ch);

	drain_defered_queue(ch);

	spin_lock_irqsave(&ch->locks_spinlock, *flags);
#endif

	/* Jared: Moved to earlier so we can set the channel dying before we release the lock */
#if 0
	nvmeibc_locks_channel_disconnect(ch, false, true);
#endif

	__NFOUT;
}

int nvmeibc_disk_locks_handle_wd_event(
	struct nvmeibc_lock_opr_in_progress *opr_ip, unsigned long time_passed)
{
	int rv = 0;
	struct nvmeibc_locks_channel *ch = opr_ip->ch;
	//struct nvmeibc_ib_net * net = &ch->net;
	//struct ib_wc wc;
	//int num_of_pending;
	bool is_dying, is_pausing;

	__NFIN;
	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(ch));
	is_pausing = atomic_read(&ch->base.disk->paused);
	is_dying = atomic_read(&ch->base.disk->dying);
	/* make sure the interrupt did not arrive while we were bla-bla here */
#if 0
	_NTHROTTLED_T(trace_disk_locks_nvmeibc_disk_locks_handle_wd_event, NVMEIBT_THROTTLE_INTERVAL, NVMEIBT_THROTTLE_BURST,
		"ch: @CH_NAME, WD: the time, in seconds, from the watchdog time-passed @LD(@LD) jiffies(sec) timeout @WD_TIMEOUT_JIF(@WD_TIMEOUT_JIF) jiffies(sec)"
	   " send id = @SEND_ID locksetid = @LOCKSET_ID.",
		ch->base.name,
		time_passed, time_passed / HZ, ch->net.wd_timeout_jif, ch->net.wd_timeout_jif / HZ,
		opr_ip->comp->send_id, opr_ip->comp->lockset_id);
#endif

	//see if we missed an intterupt
//	num_of_pending = ib_peek_cq(net->send_cq, 1);
//	_NW(nvmeibc_disk_locks_handle_wd_event_w1,
//		"LOCKS: num of pending triggers: @INT", num_of_pending);

	if ((time_passed >= ch->net.wd_timeout_jif &&
		(opr_ip->start_time <= (jiffies - time_passed))) || is_pausing ||
		is_dying) {
		_NE(trace_01_dlwd_e,
			"Lock channel operation timed out (opr_ip = @OPR_IP send id = @SEND_ID locksetid = @LOCKSET_ID)."
			"Time-passed @LD[j], @LD[sec], timeout @WD_TIMEOUT_JIF[j], @WD_TIMEOUT_JIF[sec]",
			opr_ip, opr_ip->comp->send_id, opr_ip->comp->lockset_id, time_passed, time_passed / HZ,
			ch->net.wd_timeout_jif, ch->net.wd_timeout_jif / HZ);
		_NE(trace_02_dlwd_e,
			"ch = @CH_PTR, net = @NET, qp = @QP, SGID @RAW_IPV6 -> DGID @RAW_IPV6, QPn: @QP_NUM, Remote QPn: @REMOTE_QPN SQ PSN: @SQ_PSN, "
			"n_comp_llp={o=@LLU, k=@LLU, t=@LLU}",
			ch, &ch->net, ch->net.qp, ch->net.path.sgid.raw, ch->net.path.dgid.raw,
			ch->net.qp->qp_num, ch->net.remote_qpn, opr_ip->sq_psn,
			ch->n_comp_llp_opr, ch->n_comp_llp_ka, ch->n_comp_llp_test);
		opr_ip->ch->release_reason = NVMEIBC_DISK_RELEASE_LOCK_CHAN_WD_EVENT;
		locks_handle_qp_error(opr_ip->ch);
	}
	else
		rv = 1;

	__NFOUT;
	return rv;
}

static void lock_opr_prepare(struct nvmeibc_locks_channel *ch,
							 struct nvmeibc_lock_opr_in_progress *ret,
							 struct nvmeibc_d_rdma_comp *comp)
{
	struct nvmeib_send_wr *wr;
	enum nvmeibc_disk_locks_opr opr = comp->opr;
	struct nvmeibc_disk_seg_locks_mem_info *record = comp->mem_info;
	struct nvmeibc_ib_net *net;
	u32 rkey;

	__NFIN;
	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(ch));
	NVMEIBC_LOCK_GUARD_SWITCH_CHECK(!_e1,
		&ret->state, LOCK_OPR_IN_FREE_LIST, LOCK_OPR_PREPARING);
	lock_check_data(ch);
	BUG_ON(record == NULL);
	BUG_ON(ret == NULL);
	ret->comp = comp;
	net = &ch->net;
	rkey = record->rkey;
	_ND(trace_disk_locks_lock_opr_prepare, "LOCKS: preparing wd for compid @SEND_ID", comp->send_id);

	ret->list.addr = ret->val_phys;
	ret->list.length = sizeof(u64);
	if (opr == NVMEIBC_LOCK_BLKSET_INFO_READ ||
		opr == NVMEIBC_LOCK_BLKSET_INFO_WRITE)
		ret->list.length = sizeof_field(union nvmeib_lock_blkset_entry, blkset_info);

	if (ret->list.length > NVMEIB_LOCK_DATA_SIZE) {
		_NE(lock_opr_prepare_e1, "lock data length too big @UINT > @LU",
			ret->list.length, NVMEIB_LOCK_DATA_SIZE);
		BUG(); // incompatible data structures - this is design failure
	}
	ret->list.lkey = net->lkey;
	wr = &ret->wr;
	memset(wr, 0, sizeof(*wr));
	nvmeib_send_wr_common(*wr).wr_id = nordda_wr_id_encode(ret->version, NVMEIB_DISK_LOCK_OPR, ret->index);
	nvmeib_send_wr_common(*wr).send_flags = IB_SEND_SIGNALED;
#ifdef DEBUG_LOCKS
	_ND(lock_opr_prepare_d1, "num if free = @INT", ch->num_of_free);
#endif

	if (opr == NVMEIBC_LOCK_CMP_AND_SWAP) {
		nvmeib_send_wr_common(*wr).opcode = IB_WR_ATOMIC_CMP_AND_SWP;
		nvmeib_send_wr_atomic(*wr).remote_addr = record->addr +
			comp->lockset_id * ret->list.length;
		nvmeib_send_wr_atomic(*wr).compare_add = comp->compare;
		nvmeib_send_wr_atomic(*wr).swap = comp->exchange;
		nvmeib_send_wr_atomic(*wr).rkey = rkey;

		if (comp->lock_cnsts->w_blkset_info) {
			/* Lock contains blkset info - Need to use masked cmp-and-swap (if supported) */
			union nvmeib_lock_blkset_entry *lock_blkset_cmp =
				(void*)&nvmeib_send_wr_atomic(*wr).compare_add;
			union nvmeib_lock_blkset_entry *lock_blkset_swap =
				(void*)&nvmeib_send_wr_atomic(*wr).swap;
			if (ch->masked_atomic_cap || !ch->atomic_cap /* srv-side-op */) {
				union nvmeib_lock_blkset_entry *lock_blkset_cmp_mask =
					(void*)&nvmeib_send_wr_atomic(*wr).compare_add_mask;
				union nvmeib_lock_blkset_entry *lock_blkset_swap_mask =
					(void*)&nvmeib_send_wr_atomic(*wr).swap_mask;
				/* channel (both sides) supports masked atomics */
				nvmeib_send_wr_common(*wr).opcode = IB_WR_MASKED_ATOMIC_CMP_AND_SWP;
				lock_blkset_cmp_mask->lock_id.all = lock_blkset_swap_mask->lock_id.all = ~(u32)0;
				lock_blkset_cmp_mask->blkset_info.all = lock_blkset_swap_mask->blkset_info.all = 0;

				if (unlikely(ch->masked_atomic_req_endian_swap)) {
					nvmeib_send_wr_atomic(*wr).compare_add = __swab64(nvmeib_send_wr_atomic(*wr).compare_add);
					nvmeib_send_wr_atomic(*wr).compare_add_mask = __swab64(nvmeib_send_wr_atomic(*wr).compare_add_mask);
					nvmeib_send_wr_atomic(*wr).swap = __swab64(nvmeib_send_wr_atomic(*wr).swap);
					nvmeib_send_wr_atomic(*wr).swap_mask = __swab64(nvmeib_send_wr_atomic(*wr).swap_mask);
				}
			} else {
				if (comp->nmcs_bi.n_attempts) {
					_ND(trace_1_disk_locks_lock_opr_prepare, "CMPSWAP: comp=@COMP, add bi (@LAST_VALUE)from last cs to "
					   "cmp and swap values and retry non-masked-cs",
					   comp, comp->nmcs_bi.last_value);
					/* use bi from last cs's 'old' val */
					lock_blkset_cmp->blkset_info.all = comp->nmcs_bi.last_value;
					lock_blkset_swap->blkset_info.all = comp->nmcs_bi.last_value;
				}
			}
		}

		if (unlikely(ch->atomic_req_endian_swap && nvmeib_send_wr_common(*wr).opcode == IB_WR_ATOMIC_CMP_AND_SWP)) {
			nvmeib_send_wr_atomic(*wr).compare_add = __swab64(nvmeib_send_wr_atomic(*wr).compare_add);
			nvmeib_send_wr_atomic(*wr).swap = __swab64(nvmeib_send_wr_atomic(*wr).swap);
		}

#ifdef DEBUG_LOCKS
		_ND(lock_opr_prepare_d1,
			"LOCKS: lockesetid = @INT_ULLONG", comp->lockset_id);
		_ND(lock_opr_prepare_d2,
			"LOCKS: preparing wr comp_exchange wr_id=0x@_X remote_addr=0x@_X"
			" compare_add=@INT_ULLONG swap=@INT_ULLONG rkey=@INT32_HEX",
			nvmeib_send_wr_common(*wr).wr_id,
			nvmeib_send_wr_atomic(*wr).remote_addr,
			nvmeib_send_wr_atomic(*wr).compare_add,
			nvmeib_send_wr_atomic(*wr).swap,
			nvmeib_send_wr_atomic(*wr).rkey);

		_ND(lock_opr_prepare_d3, "LOCKS: 0x@_X, length=@INT, lkey=@INT32_HEX",
			ret->list.addr, ret->list.length, ret->list.lkey);
#endif
	} else if (opr == NVMEIBC_LOCK_FORCE_WRITE) {
		_ND(trace_2_disk_locks_lock_opr_prepare, "LOCKS: prepare a writeopr for compid @SEND_ID", comp->send_id);
		nvmeib_send_wr_common(*wr).opcode = IB_WR_RDMA_WRITE;
		nvmeib_send_wr_rdma(*wr).remote_addr = record->addr +
			comp->lockset_id * ret->list.length;
		nvmeib_send_wr_rdma(*wr).rkey = rkey;
#ifdef DEBUG_LOCKS
		_ND(lock_opr_prepare_d3,
			"remote_addr=0x@_X, rkey=@INT32_HEX, lkey=@INGT32_HEX",
			nvmeib_send_wr_rdma(*wr).remote_addr,
			nvmeib_send_wr_rdma(*wr).rkey, ret->list.lkey);
#endif
	}
	else if (opr == NVMEIBC_LOCK_READ) {
		nvmeib_send_wr_common(*wr).opcode = IB_WR_RDMA_READ;
		nvmeib_send_wr_rdma(*wr).remote_addr = record->addr +
			comp->lockset_id * ret->list.length;

		nvmeib_send_wr_rdma(*wr).rkey = rkey;
#ifdef DEBUG_LOCKS
		_ND(lock_opr_prepare_d5,
			"remote_addr=0x@_X, rkey=@INT32_HEX, lkey=@INT32_HEX",
			nvmeib_send_wr_rdma(*wr).remote_addr,
			nvmeib_send_wr_rdma(*wr).rkey, ret->list.lkey);
#endif
	} else if (opr == NVMEIBC_LOCK_BLKSET_INFO_WRITE ||
		opr == NVMEIBC_LOCK_BLKSET_INFO_READ) {
		_ND(trace_5_disk_locks_lock_opr_prepare, " LOCKS: @OP_STR flags @FLAGS_INT (to/from) lockset @SEND_ID",
		   (opr == NVMEIBC_LOCK_BLKSET_INFO_WRITE ? "writing" : "reading"),
		   (u32)comp->lock.bi, comp->send_id);

		nvmeib_send_wr_common(*wr).opcode = (opr == NVMEIBC_LOCK_BLKSET_INFO_WRITE ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ);
		nvmeib_send_wr_rdma(*wr).remote_addr = record->addr +
		comp->lockset_id * sizeof(union nvmeib_lock_blkset_entry)
			+ offsetof(union nvmeib_lock_blkset_entry, blkset_info);
		nvmeib_send_wr_rdma(*wr).rkey = rkey;

		/* Blkset Info is stored in val[1], so offset the source SGE by the size of val[0] */
		ret->list.addr += sizeof(ret->val[0]);
#ifdef DEBUG_LOCKS
		_ND(lock_opr_prepare_d9,
			"remote_addr=0x@_X, rkey=@INT32_HEX, lkey=@INT32_HEX",
			nvmeib_send_wr_rdma(*wr).remote_addr,
			nvmeib_send_wr_rdma(*wr).rkey, ret->list.lkey);
#endif
	}
	nvmeib_send_wr_common(*wr).sg_list = &ret->list;
	nvmeib_send_wr_common(*wr).num_sge = 1;
	ret->val[0] = comp->val[0];
	ret->val[1] = comp->val[1];
	nvmeib_send_wr_clear_next(*wr);
	BUG_ON(comp->mem_info == NULL);

	__NFOUT;
}

static void dump_lock_oprs_in_progress(struct nvmeibc_locks_channel *ch)
{
	int i = 0;
	int count = 0;
	struct nvmeibc_lock_opr_in_progress *opr_ip = NULL;

	list_for_each_entry(opr_ip, &ch->in_progress, link) {
		count++;
	}

	if (count != ch->num_in_progress) {
		_NE(error_disk_locks_dump_lock_oprs_in_progress, "ch=@CH_PTR count=@COUNT ch->num_in_progress=@NUM_IN_PROGRESS", ch, count, ch->num_in_progress);
	}
	else {
		if (i++ < 100)
			return;
	}

	i = 0;
	list_for_each_entry(opr_ip, &ch->in_progress, link) {
		_ND(trace_disk_locks_dump_lock_oprs_in_progress, "opr_ip=@OPR_IP", opr_ip);
	}
}

static void set_opr_in_progress(struct nvmeibc_locks_channel *ch,
								struct nvmeibc_lock_opr_in_progress *opr_ip)
{
	struct nvmeibc_d_rdma_comp *comp __attribute__((unused));
	list_add_tail(&opr_ip->link, &ch->in_progress);
	BUG_ON(ch->num_in_progress < 0);
	NVMEIBC_LOCK_GUARD_SWITCH_CHECK(!_e1,
		&opr_ip->state, LOCK_OPR_PREPARING, LOCK_OPR_IN_PROGRESS);
	ch->num_in_progress++;
	if (nvmeib_send_wr_common(opr_ip->wr).opcode != IB_WR_RDMA_WRITE)
		ch->num_of_atom_read_ip++;
	opr_ip->sq_psn = ch->net.sq_psn; /* Assumes +1 PSN per WR (may not be true) */
	ch->net.sq_psn = (ch->net.sq_psn + 1) & 0xffffff;
	if (!opr_ip->disk_piggyb) /* Don't start WD if we are piggy-backing on NRDDA cmd. It has its own WD. */
		nvmeib_wd_start_wdc(&opr_ip->wdc);
	opr_ip->jiffies_start = jiffies;
	opr_ip->opr_timeout = NVMEIB_MAX_LOCK_TIME_INC_RETRIES;
	comp = opr_ip->comp;

#ifdef DEBUG_D_RDMA_COMP
	comp->in_prog_data = opr_ip;
#endif

#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
	if (!opr_ip->disk_piggyb) {
		unsigned long dt;
		struct nvmeibc_disk_command_probes_try_data *current_try = nvmeibc_disk_command_probes_current_try(&comp->probes);
		/* Wait for NRCH to set and check llp_post_jif */
		current_try->llp_post_jif = jiffies;

		dt = current_try->llp_post_jif - current_try->ulp_post_jif;
		if (dt > 1*HZ) {
			_NW(trace_0_set_opr_in_progress,
				"@DISK_NAME, @CH_NAME, @LOCK_CH, opr=@OPR disk_piggyb=@BOOL pending for too long @LD",
				ch->base.disk->name, ch->base.name, ch, comp->opr, opr_ip->disk_piggyb, dt);
		}
	}
#endif
}

static int lock_prepare_and_send(struct nvmeibc_locks_channel *ch,
	struct nvmeibc_lock_opr_in_progress **bad_op)
{
	int rv = 0, i, id_in_row = 0;
	struct nvmeibc_d_rdma_comp *comp;
	struct nvmeib_send_wr *wr = NULL, *last_wr = NULL, *bad_wr;
	struct nvmeibc_lock_opr_in_progress *opr_ip = NULL, *first_opr = NULL;
	int num_of_wr_in_msg = 0;
	int max_nip = 0;
	LIST_HEAD(bypass_atomic_list);
	int n_bypass_atomic = 0;
	struct nvmeib_cpu_mask cpus;

	BUILD_BUG_ON_MSG(sizeof(enum nvmeib_lock_req_op) != 1, "nvmeib_lock_req_op size");
	BUILD_BUG_ON_MSG(sizeof(enum nvmeibc_disk_locks_opr) != 1, "nvmeibc_disk_locks_opr size");

	__NFIN;
	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(ch));
	if (list_empty(&ch->defered)) {
		rv = -EAGAIN;
		goto out;
	}

	_ND(trace_disk_locks_lock_prepare_and_send, "ch = @CH_PTR got @NUM_DEFERED defered in prog=@PROG free=@FREE", ch,
		ch->num_defered, ch->num_in_progress, ch->num_of_free);

	BUG_ON((ch->num_in_progress + ch->num_of_free + ch->num_aborted) !=
		ch->total_num_opr);

	_ND(trace_1_disk_locks_lock_prepare_and_send, "Sending locks channel data via ch @CH_PTR number of defered @NUM_DEFERED", ch, ch->num_defered);

	if (list_empty(&ch->free_ip_pool) || 1 /* max_nip < ch->num_in_progress*/) {
		if (max_nip <= ch->num_in_progress) {
			max_nip = ch->num_in_progress;
			_ND(trace_2_disk_locks_lock_prepare_and_send, "No available free for ch=@CH_PTR got @NUM_DEFERED defered in prog=@PROG free=@FREE",
				ch, ch->num_defered, ch->num_in_progress, ch->num_of_free);
			dump_lock_oprs_in_progress(ch);
		}
	}

	while (DEBUG_BYPASS_ATOMIC_OPS || !ch->atomic_cap ||
		   ch->num_of_atom_read_ip < (ch->net.max_dest_rd_atomic)) {
		if (!list_empty(&ch->defered) &&
				(opr_ip = nvmeibc_locks_channel_get_free_opr_ip(ch))) {
			nvmeib_completion_noise_start(NVMEIB_NOISE_SUBMISSION);
			comp = list_first_entry_or_null(&ch->defered, struct nvmeibc_d_rdma_comp,
				link_deferred);
			BUG_ON(comp == NULL);
			list_del_init(&comp->link_deferred); /* defered */
			cpus = comp->cpu_mask_info.mask;

			if (DEBUG_BYPASS_ATOMIC_OPS) {
				if (comp->opr == NVMEIBC_LOCK_CMP_AND_SWAP &&
					ch->num_of_atom_read_ip >= (ch->net.max_dest_rd_atomic)) {
					_NE(lock_prepare_and_send_e1, "ch @PTR, opr=@INT, "
						"n_atomic_wip=@INT --> n_bypass_atomic=@INT",
						ch, comp->opr, ch->num_of_atom_read_ip, n_bypass_atomic);
					list_add_tail(&comp->link_deferred, &bypass_atomic_list);
					n_bypass_atomic++;
					nvmeibc_locks_channel_free_opr_ip(ch, opr_ip, LOCK_OPR_IN_FREE_LIST);
					continue;
				}
			}

			nvmeibc_disk_cmds_stats_pending_exec_start(ch->base.disk, &comp->lock_cmd);
			BUG_ON(ch->num_defered <= 0);
			ch->num_defered--;
			lock_opr_prepare(ch, opr_ip, comp);
			BUG_ON(opr_ip->comp == NULL);
			opr_ip->first = false;
			opr_ip->id_in_row = id_in_row++;
			if (!first_opr){
				first_opr = opr_ip;
				_ND(lock_opr_prepare_d1,
					"Setting the first opr to @PTR", first_opr);
			}
			wr = &opr_ip->wr;
			opr_ip->start_time = jiffies;
			++num_of_wr_in_msg;
			if (last_wr)
				nvmeib_send_wr_set_next(*last_wr, wr);
			last_wr = wr;
			nvmeib_send_wr_clear_next(*wr);
#if ENABLE_SIW
			if (P2NV(ch->net.port)->dev_type == DT_siw) {
				nvmeib_send_wr_common(*wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
			}
#endif
		}
		_ND(lock_opr_prepare_d3, "Testing with @INT defered @INT",
			num_of_wr_in_msg, ch->num_defered);
		if (num_of_wr_in_msg == NVMEIBC_CHANNEL_AV_NUM_OF_WR_PER_MSG ||
			(num_of_wr_in_msg > 0 &&
			 (list_empty(&ch->defered) || !opr_ip))) {
			BUG_ON(first_opr == NULL);
			BUG_ON(first_opr->comp == NULL);
			BUG_ON(num_of_wr_in_msg == 1 && &first_opr->wr != last_wr);
			first_opr->first = true;
			//ib_req_notify_cq(ch->net.send_cq, IB_CQ_NEXT_COMP);
			_ND(lock_opr_prepare_d6,
				"And now sending first opr @PTR his next @PTR locset=0x@_X",
				first_opr, nvmeib_send_wr_next_ptr(first_opr->wr), first_opr->comp->lockset_id);
			rv = (ch->net.post_send_atomic_fn)(ch->net.qp, &first_opr->wr, &bad_wr);
			if (unlikely(rv)) {
				LIST_HEAD(retry_list);
				int n_retry = 0;
				struct nvmeib_send_wr *wr_iter;
				_NW(lock_opr_prepare_w1, "nvmeib_post_send_atomic "
					"ended with error @RV", rv);
				/* An error occurred, but some ops may have been sent */
				for (wr_iter = &first_opr->wr; wr_iter != bad_wr;
						wr_iter = nvmeib_send_wr_next_ptr(*wr_iter)) {
					opr_ip = container_of(wr_iter, struct nvmeibc_lock_opr_in_progress, wr);
					set_opr_in_progress(ch, opr_ip);
				}

				/* Handle bad-wr */
				opr_ip = container_of(bad_wr, struct nvmeibc_lock_opr_in_progress, wr);
				_NT(lock_opr_prepare_t3, "bad-wr rv=@RV", rv);
				if (rv == -ENOMEM) {
					/* Send Queue was full */
					list_add_tail(&opr_ip->comp->link_deferred, &retry_list);
					nvmeibc_disk_cmds_stats_err_internal_retry(ch->base.disk, &opr_ip->comp->lock_cmd);
					opr_ip->comp = NULL;
					nvmeibc_locks_channel_free_opr_ip(ch, opr_ip, LOCK_OPR_PREPARING);
					n_retry++;
					rv = 0;
				} else {
					/* Set "bad op" for caller */
					NVMEIBC_LOCK_GUARD_SWITCH_CHECK(!_e1,
						&opr_ip->state, LOCK_OPR_PREPARING, LOCK_OPR_ERROR);
					*bad_op = opr_ip;
				}
				/* Return WRs the follows bad-wr to retry list,
				   preserve orig order */
				for (wr_iter = nvmeib_send_wr_next_ptr(*bad_wr); wr_iter != NULL;
					  wr_iter = nvmeib_send_wr_next_ptr(*wr_iter)) {
					opr_ip = container_of(wr_iter, struct nvmeibc_lock_opr_in_progress, wr);
					list_add_tail(&opr_ip->comp->link_deferred, &retry_list);
					nvmeibc_disk_cmds_stats_err_internal_retry(ch->base.disk, &opr_ip->comp->lock_cmd);
					opr_ip->comp = NULL;
					nvmeibc_locks_channel_free_opr_ip(ch, opr_ip, LOCK_OPR_PREPARING);
					n_retry++;
				}

				/* add retry list to head of deferred queue */
				list_splice_init(&retry_list, &ch->defered);
				ch->num_defered += n_retry;
				nvmeib_completion_noise_end(NVMEIB_NOISE_SUBMISSION, cpus.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
					ch->base.disk->access_local? NVMEIB_NOISE_CTRS_LOCK_SUBMISSION_LOCAL : NVMEIB_NOISE_CTRS_LOCK_SUBMISSION);
				goto out;
			}
			else {
				//TBD: Unite this case with its 'if' by having bad_wr = NULL
				ch->num_of_series_ip++;
				_ND(lock_opr_prepare_d20, "num of send oprs in series is @INT",
					num_of_wr_in_msg);
				for (i = 0; i < num_of_wr_in_msg; ++i) {
					opr_ip = first_opr;
					BUG_ON(opr_ip->id_in_row != i);
					BUG_ON(opr_ip->comp == NULL);
					BUG_ON(i < (num_of_wr_in_msg - 1) && nvmeib_send_wr_common(first_opr->wr).next == NULL);
					BUG_ON(i == (num_of_wr_in_msg - 1) && nvmeib_send_wr_common(first_opr->wr).next != NULL);
					if (nvmeib_send_wr_next_valid(first_opr->wr)) {
						first_opr = container_of(
							nvmeib_send_wr_next_ptr(first_opr->wr),
							struct nvmeibc_lock_opr_in_progress, wr);
					}
					set_opr_in_progress(ch, opr_ip);
				}
			}
			BUG_ON((ch->num_in_progress + ch->num_of_free + ch->num_aborted) !=
				ch->total_num_opr);
			num_of_wr_in_msg = 0;
			id_in_row = 0;
			last_wr = NULL;
			first_opr = NULL;
			nvmeib_completion_noise_end(NVMEIB_NOISE_SUBMISSION, cpus.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
				ch->base.disk->access_local? NVMEIB_NOISE_CTRS_LOCK_SUBMISSION_LOCAL : NVMEIB_NOISE_CTRS_LOCK_SUBMISSION);
		}
		if (list_empty(&ch->defered) || !opr_ip)
			break;
	}

out:
	if (DEBUG_BYPASS_ATOMIC_OPS) {
		list_splice_init(&bypass_atomic_list, &ch->defered);
	}

	__NFOUT;
	return rv;
}

int nvmeibc_lock_opr_to_disk_cmd(
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd,
	struct nvmeibc_lock_opr_in_progress *opr);

/************************************************************************************************
 * @function: choose_locks_channel
 * @param: primary_ch - The primary lock channel
 * @desc:
 * This function chooses a lock-channel (the primary or one of the secondary channels)
 * to use to send the lock operation. It attempts to use the channel with the smallest number of
 * lock operations already in-progess. However, if it detects that it is in the context of the
 * send-completion of a lock channel, (nvmeibc_channel_already_locked returns true),
 * then it uses that lock channel. This is to prevent dead-locks.
 * ***********************************************************************************************/

#define normalize_hint(_ch, _hint) _hint % ((_ch)->n_2nd_ch + 1)
#define get_lock_channel_by_sharding(_primary_ch, idx) (idx? (_primary_ch)->_2nd_ch[idx - 1] : _primary_ch)
static struct nvmeibc_locks_channel *
choose_locks_channel(struct nvmeibc_locks_channel *primary_ch, enum nvmeibc_disk_locks_opr opr, u64 hint,
							  const struct nvmeib_cpu_mask_info *coremask_info)
{
	struct nvmeibc_locks_channel *chosen_ch = NULL;
	int num_chs = primary_ch->n_2nd_ch + 1;
	hint = normalize_hint(primary_ch, hint);

	if (nvmeibc_locks_channel_is_ka_timeout(primary_ch))
		goto out;

	chosen_ch = primary_ch;
	if (nvmeibc_channel_already_locked(&primary_ch->base)) {
		chosen_ch = primary_ch;
		goto out;
	}
	if (opr == NVMEIBC_LOCK_CMP_AND_SWAP && !primary_ch->atomic_cap) {
		/* For RPC locks, don't use secondary lock channels */
		goto out;
	}
	if (primary_ch->_2nd_ch_coremask) {
		struct nvmeibc_disk *disk = primary_ch->base.disk;
		int submit_cpu = get_cpu();
		struct nvmeibc_disk_coremask_pcpu_stats __percpu *disk_pcpu_stats = nvmeibc_disk_get_coremask_stats_this_cpu(disk);
		unsigned coremask_weight;
		u64 chosen_ch_mask_uid;
		unsigned long flags;
		
		local_irq_save(flags);
		if (!coremask_info || !coremask_info->gen || !(coremask_weight = NVMEIB_CPU_MASK_WEIGHT(coremask_info->mask))) {
			/* Lock operation has no coremask, use primary channel */
			if (disk_pcpu_stats)
				COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_lock_not_coremask_op);
			chosen_ch = primary_ch;
			goto end_2nd_ch_coremask;
		}
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_lock_coremask_op);
		if (submit_cpu >= NVMEIB_CPU_MASK_MAX_CPUS ||
			!NVMEIB_CPU_MASK_TEST_CPU(submit_cpu, coremask_info->mask)) 
		{
			/* Incoming CPU is not part of the mask, choose random cpu as submission cpu using sharding */
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_lock_coremask_submit_cpu_not_in_mask);
			submit_cpu = NVMEIB_CPU_MASK_FIND_NTH_BIT(hint % coremask_weight, coremask_info->mask);
		}
		if (!(chosen_ch = primary_ch->_2nd_ch_coremask_map[submit_cpu])) {
			/* Channel for this CPU does not exist */
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_lock_coremask_no_lock_ch);
			chosen_ch = primary_ch;
			goto end_2nd_ch_coremask;
		}
		chosen_ch_mask_uid = (*chosen_ch->coremask_ops->get_uid_fn)(
			nvmeibc_channel_get_coremask_ch_cookie(&chosen_ch->base));
		if (chosen_ch_mask_uid != coremask_info->gen) {
			/* Operation coremask does not match Channel coremask */
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_lock_coremask_uid_mismatch);
			chosen_ch = primary_ch;
		}
		
end_2nd_ch_coremask:
		local_irq_restore(flags);
		put_cpu();
		goto out;
	}
	if (primary_ch->_2nd_ch_pcpu) {
		if (smp_processor_id() < NVMEIB_DFLT_MAX_CPUS &&
			(chosen_ch = primary_ch->_2nd_ch_pcpu_map[smp_processor_id()])) {
			goto out;
		}
		/* per-cpu channel is not available for this cpu, return primary channel */
		chosen_ch = primary_ch;
		goto out;
	}
	if (primary_ch->method == LOCK_CHANNEL_CHOOSING_METHOD_BY_CPU) {
		int ch_idx = smp_processor_id() % num_chs;
		if (ch_idx > 0)
			chosen_ch = primary_ch->_2nd_ch[ch_idx - 1];
		goto out;
	} else if (primary_ch->method == LOCK_CHANNEL_CHOOSING_METHOD_SHARDING) {
		if ((chosen_ch = get_lock_channel_by_sharding(primary_ch, hint))) {
			goto out;
		}
		chosen_ch = primary_ch;
		goto out;
	}
	if (primary_ch->n_2nd_ch > 0) {
		int i, min_tot_op = INT_MAX, start = hint;
		for (i = start; i < num_chs + start; i++) {
			struct nvmeibc_locks_channel *ch = get_lock_channel_by_sharding(primary_ch, hint);
			if (ch) {
				int _2nd_tot_op = ch->num_in_progress + ch->num_defered;
				if (DEBUG_2ND_LOCK_CH_KA &&
					nvmeibc_locks_channel_is_ka_timeout(ch)) {
					chosen_ch = NULL;
					goto out;
				}
				if (nvmeibc_channel_already_locked(&ch->base)) {
					chosen_ch = ch;
					goto out;
				}
				if (DEBUG_2ND_LOCK_CH_KA) {
					u64 ka_from_comp = jiffies - ch->ka_comp_jif;
					if (!(ch->n_comp_llp_ka && ka_from_comp < HZ * 30)) {
						_NI(trace_choose_locks_channel, "Lock-ch @CH_NAME (@CH_PTR), No KA comp"
							"(n_post=@LLU, n_comp=@LLU, comp_jif=@LLU,"
							"from_comp=@LLU(@LLU)), skip (@LLU)..."
							"n_comp_llp={o=@LLU, k=@LLU, t=@LLU}\n",
							ch->base.name, ch,
							ch->ka_post_cnt, ch->n_comp_llp_ka, ch->ka_comp_jif,
							ka_from_comp, ka_from_comp / HZ, ch->ka_skip_use,
							ch->n_comp_llp_opr, ch->n_comp_llp_ka, ch->n_comp_llp_test);
						ch->ka_skip_use++;
						continue;
					}
				}

				if (_2nd_tot_op < min_tot_op) {
					/* So far this secondary has the minimum number of in-progress ops */
					min_tot_op = _2nd_tot_op;
					chosen_ch = ch;
				}
			}
		}
	}

out:
	return chosen_ch;
}

//AAA: Add funcs for wq-create & wq-destroy

static inline void run_lock_comp_cb(struct nvmeibc_d_rdma_comp *comp, bool noisy)
{
	struct nvmeib_cpu_mask cpu_mask;

	cpu_mask = comp->cpu_mask_info.mask;
	comp->callback(comp);
	if (noisy)
		nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
			NVMEIB_NOISE_CTRS_LOCK_CB);
}

#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
static void lock_comp_cb_work(struct work_struct *work)
#else
static void lock_comp_cb_work(struct workqe_struct *work)
#endif
{
	struct nvmeibc_d_rdma_comp *comp = container_of(
			work, struct nvmeibc_d_rdma_comp, cb_work_transport);
	run_lock_comp_cb(comp, true);
}

static void lock_comp_cb_pcpu_wq(struct workqe_struct *work)
{
	struct nvmeibc_d_rdma_comp *comp = container_of(
			work, struct nvmeibc_d_rdma_comp, cb_workqe_transport);
	run_lock_comp_cb(comp, false);
}

static void lock_comp_cb_work_add(struct nvmeibc_locks_channel *ch,
								  struct nvmeibc_d_rdma_comp *comp)
{
#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
	INIT_WORK(&comp->cb_work_transport, lock_comp_cb_work);
	queue_work(ch->primary_ch->callback_wq, &comp->cb_work_transport);
#else
	WQ_INIT_WORK(&comp->cb_work_transport, lock_comp_cb_work);
	wq_add_work(ch->callback_wq, &comp->cb_work_transport);
#endif
}

static void lock_comp_cb_work_add_on_cpu(struct nvmeibc_locks_channel *ch,
										 struct nvmeibc_d_rdma_comp *comp)
{
#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
	INIT_WORK(&comp->cb_work_transport, lock_comp_cb_work);
	queue_work_on(comp->lockset_id % num_active_cpus(),
				  ch->primary_ch->callback_wq, &comp->cb_work_transport);
#else
	WQ_INIT_WORK(&comp->cb_work_transport, lock_comp_cb_work);
	wq_add_work(ch->callback_wq, &comp->cb_work_transport);
#endif
}

static void lock_comp_cb_work_add_on_system_pcpu_wq(struct nvmeibc_locks_channel *ch,
													struct nvmeibc_d_rdma_comp *comp)
{
	static unsigned int pcpu_cntr = 0; /* this is racy between threads/cpus but only used for randomization */
	unsigned int resched_cpu;

	WQ_INIT_WORK(&comp->cb_workqe_transport, lock_comp_cb_pcpu_wq);
	resched_cpu = NVMEIBC_DISK_GET_RESCHED_CPU(comp->cpu_mask_info.mask.cpus, pcpu_cntr);
	nvmeib_pcpu_wq_add_work_on_core(nvmeib_get_system_wq(), resched_cpu, &comp->cb_workqe_transport);
	nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, comp->cpu_mask_info.mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
								ch->base.disk->access_local? NVMEIB_NOISE_CTRS_LOCK_DEFER_CB_PCPU_WQ_LOCAL : NVMEIB_NOISE_CTRS_LOCK_DEFER_CB_PCPU_WQ);
}

static void lock_comp_execute_cb(struct nvmeibc_locks_channel *ch,
								struct nvmeibc_d_rdma_comp *comp, void *debug_ptr,
								enum lock_comp_execute_mode exec_mode)
{
#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
	nvmeibc_disk_command_probes_current_try(&comp->probes)->status = comp->lock_status;
	comp->probes.n_tries++;
#endif
#ifdef DEBUG_D_RDMA_COMP
	comp->debug_ptr = debug_ptr;
	comp->callback_line = __LINE__;
#endif
	if (unlikely(comp->lock_status == NCL_STATUS_CONTENDED && comp->compare == comp->lock.id)) {
			_NW_dmesg(contended_but_zero_xchg_w, "lock-comp @LOCK_COMP, status CONTENDED although client still own the lock", comp);
			BUG_ON(1);
	}

	switch (exec_mode) {
	case LOCK_COMP_EXECUTE_INLINE:
		run_lock_comp_cb(comp, false);
		break;
	case LOCK_COMP_EXECUTE_ON_WQ:
		lock_comp_cb_work_add(ch, comp);
		break;
	case LOCK_COMP_EXECUTE_ON_WQ_THIS_CPU:
		lock_comp_cb_work_add_on_cpu(ch, comp);
		break;
	case LOCK_COMP_EXECUTE_ON_SYSTEM_PCPU_WQ:
		lock_comp_cb_work_add_on_system_pcpu_wq(ch, comp);
		break;
	default:
		BUG();
	}
}

/* Called with lock ch spinlock locked */
static int execute_opr_local_bypass(struct nvmeibc_locks_channel *locks_channel,
									struct nvmeibc_d_rdma_comp *comp,
									struct nvmeibc_d_rdma_comp **bad_comp)
{
	struct nvmeibc_disk *disk = locks_channel->base.disk;
	struct nvmeib_gen_cmd_param lock_gen_p = {};
	union nvmeib_gen_cmd_rsp lock_gen_rsp = {};
	union nvmeib_blkset_info blkset_info = {};
	int rv;

	lock_gen_p.lock_param.seg_id = comp->mem_info->seg_id;
	lock_gen_p.lock_param.offset = comp->lockset_id * sizeof(union nvmeib_lock_blkset_entry);

	if (atomic_read(&locks_channel->base.dying) || !nvmeib_local_server_up()) {
		rv = -ENODEV;
		*bad_comp = comp;
		goto out;
	}

	switch (comp->opr) {
		case NVMEIBC_LOCK_CMP_AND_SWAP:
			lock_gen_p.lock_param.op = NVMEIB_ATOMIC_CMP_AND_SWP;
			lock_gen_p.lock_param.atomic.compare_add = comp->compare;
			lock_gen_p.lock_param.atomic.swap = comp->exchange;

			if (comp->lock_cnsts->w_blkset_info) {
				union nvmeib_lock_blkset_entry *lock_blkset_cmp_mask =
				(void*)&lock_gen_p.lock_param.atomic.compare_add_mask;
				union nvmeib_lock_blkset_entry *lock_blkset_swap_mask =
				(void*)&lock_gen_p.lock_param.atomic.swap_mask;

				lock_gen_p.lock_param.op = NVMEIB_MASKED_ATOMIC_CMP_AND_SWP;
				lock_blkset_cmp_mask->lock_id.all = lock_blkset_swap_mask->lock_id.all = ~(u32)0;
				lock_blkset_cmp_mask->blkset_info.all = lock_blkset_swap_mask->blkset_info.all = 0;
			}
			break;
		case NVMEIBC_LOCK_FORCE_WRITE:
		case NVMEIBC_LOCK_BLKSET_INFO_READ:
			/* These are currently not used so not implementing here */
			rv = -ENOTSUPP;
			goto out;
		case NVMEIBC_LOCK_READ:
			lock_gen_p.lock_param.op = NVMEIB_LOCK_RDMA_READ;
			/* Read it into the lock cmp_swap_val response so we can use common code below */
			lock_gen_p.lock_param.rdma.data = &lock_gen_rsp.lock_rsp.cmp_swap_val;
			lock_gen_p.lock_param.rdma.len = sizeof(lock_gen_rsp.lock_rsp.cmp_swap_val);
			lock_gen_p.lock_param.rdma.table_type = NVMEIB_LOCK_TABLE_OWNER;
			break;
		case NVMEIBC_LOCK_BLKSET_INFO_WRITE:
			lock_gen_p.lock_param.op = NVMEIB_LOCK_RDMA_WRITE;
			lock_gen_p.lock_param.offset +=
				offsetof(union nvmeib_lock_blkset_entry, blkset_info);
			blkset_info.all = comp->lock.bi;
			lock_gen_p.lock_param.rdma.data = &blkset_info;
			lock_gen_p.lock_param.rdma.len = sizeof(blkset_info);
			lock_gen_p.lock_param.rdma.table_type = NVMEIB_LOCK_TABLE_OWNER;
			break;
		default:
			BUG_ON(1);
	}

	rv = disk->local_server->gen_cmd( /* nvmeibs_handle_gen_cmd */
		&disk->local, NVMEIB_GEN_OP_LOCK, &lock_gen_p, &lock_gen_rsp, disk->cid);

	atomic64_inc(&disk->gen_cmds_cntrs_local[NVMEIB_GEN_OP_LOCK]);

	if (rv < 0) {
		_NE(execute_opr_local_bypass_gen_cmd_f, "NVMEIB_GEN_OP_LOCK submit failed to @DISK,\
											 	skip cb. lock_comp ptr: @LOCK_COMP", disk, comp);
		atomic64_inc(&disk->gen_cmds_cntrs_fail[NVMEIB_GEN_OP_LOCK]);
		*bad_comp = comp;
		goto out;
	}
	atomic64_inc(&disk->gen_cmds_cntrs_ok[NVMEIB_GEN_OP_LOCK]);
	if (comp->opr == NVMEIBC_LOCK_CMP_AND_SWAP || comp->opr == NVMEIBC_LOCK_READ) {
		if (comp->lock_cnsts->w_blkset_info) {
			union nvmeib_lock_blkset_entry *lock_blkset_ret = (void *)&lock_gen_rsp.lock_rsp.cmp_swap_val;
			comp->lock.id = lock_blkset_ret->lock_id.all;
			comp->lock.bi = lock_blkset_ret->blkset_info.all;
		} else {
			comp->lock.id = lock_gen_rsp.lock_rsp.cmp_swap_val;
			comp->lock.bi = 0;
		}
		if (comp->opr == NVMEIBC_LOCK_CMP_AND_SWAP)
			comp->lock_status = comp->lock.id == comp->compare ? NCL_STATUS_TAKEN : NCL_STATUS_CONTENDED;
		else {
			BUG_ON(lock_gen_rsp.lock_rsp.read_len != lock_gen_p.lock_param.rdma.len);
			comp->lock_status = (comp->lock.id ? NCL_STATUS_CONTENDED : NCL_STATUS_TAKEN);
		}
		/*if (comp->exchange == 0 && comp->lock_status == NCL_STATUS_CONTENDED) {
			pr_err("Crashing the system to prevent data corruption. Error code: 1059. comp %p lock_gen_p %p lock_gen_rsp %p \n", comp, &lock_gen_p, &lock_gen_rsp);
			BUG_ON(1);
		}*/
	} else {
		comp->lock_status = NCL_STATUS_TAKEN;
	}
	nvmeibc_disk_cmds_stats_llp_complete(disk, &comp->lock_cmd, -ENOTSUPP, -ENOTSUPP);
	lock_comp_execute_cb(locks_channel, comp, NULL, LOCK_COMP_EXECUTE_ON_WQ_THIS_CPU);
	rv = 0;

out:
	return rv;
}

/* assumes thread is pinned to a cpu */
#define NVMEIBC_DISK_LOCKS_SHOULD_DEFER_TO_PCPU_WQ(_cpus) NVMEIBC_DISK_SHOULD_DEFER_TO_PCPU_WQ(nvmeibc_disk_locks_use_system_pcpu_wq && !NVMEIBC_DISK_SAFE_TEST_CURRENT_CPU_IN_BITMAP(&_cpus), _cpus)

static int execute_opr(struct nvmeibc_disk_seg_locks_mem_info *record, u64 addr,
	struct nvmeibc_d_rdma_comp *comp)
{
	int rv = 0;
	unsigned long flags;
	struct nvmeibc_locks_channel *locks_channel;
	struct nvmeibc_ib_net *net;
	struct nvmeibc_lock_opr_in_progress *bad_opr = NULL;
	struct nvmeibc_d_rdma_comp *bad_comp = NULL;

	NFIN;
	_ND(trace_disk_locks_execute_opr, "LOCKS: in progress got @CALLBACK_PTR", comp->callback);
	comp->lock_status = NCL_STATUS_FAIL_NO_COMP; /* Errors returned with rv */
	if (!record) {
		_NE(error_disk_locks_execute_opr, "attempt to execute lock operation on NULL record");
		rv = -EINVAL;
		goto out;
	}

	if (!comp) {
		_NE(error_1_disk_locks_execute_opr, "attempt to execute lock operation with NULL compare descriptor");
		rv = -EINVAL;
		goto out;
	}
	BUG_ON(comp->callback == NULL);

	if (addr < record->start_addr ||
		addr > (record->start_addr + record->len)) {
		_NE(error_2_disk_locks_execute_opr, "addr @ADDR is not in a segment range", addr);
		_NE(error_3_disk_locks_execute_opr, "debug information");
		_NE(error_4_disk_locks_execute_opr, "record = @RECORD", record);
		_NE(error_5_disk_locks_execute_opr, "LOCKS: seg_id = @SEG_ID_INT", record->seg_id);
		_NE(error_6_disk_locks_execute_opr, "LOCKS: locks channel=@CHANNEL_PTR",record->locks_channel);
		_NE(error_7_disk_locks_execute_opr, "LOCKS: len = @LEN", record->len);
		_NE(error_8_disk_locks_execute_opr, "LOCKS: lock_set_size = @LOCK_SET_SIZE", record->lock_set_size);
		_NE(error_9_disk_locks_execute_opr, "LOCKS: start_addr = @START_ADDR", record->start_addr);
		_NE(error_10_disk_locks_execute_opr, "LOCKS: address = @ADDR", record->addr);
		_NE(error_11_disk_locks_execute_opr, "LOCKS: rkey = @RKEY", record->rkey);
		rv = -EINVAL;
		goto out;
	}

	locks_channel = choose_locks_channel(record->locks_channel, comp->opr, addr, &comp->cpu_mask_info);
	if (!locks_channel) {
		rv = -EINVAL;
		goto out;
	}

	net = &locks_channel->net;
	nvmeibc_locks_channel_spin_lock_irqsave(locks_channel, flags);
	if (locks_channel->paused) {
		rv = -EIO;
		/* channel's @paused may be set before ULP is paused (from disk-release
		   work) in case it had qp-err or WD event, so this can happen... */
		_NW(warn_disk_locks_execute_opr, "locks channel paused, IO canceled");
		goto out_err;
	}

	_ND(trace_1_disk_locks_execute_opr, "record is @RECORD", record);
	comp->lockset_id = (addr - record->start_addr) / record->lock_set_size;
	comp->mem_info = record;
	comp->send_id = locks_channel->send_enumerator++;
	_ND(trace_2_disk_locks_execute_opr, "LOCKSUP: @LOCKS_CHANNEL compid @SEND_ID opr = @OPR exchange = @EXCHANGE compare = @COMPARE"
	" lockset @LOCKSET_ID", locks_channel, comp->send_id, comp->opr,
	 comp->exchange, comp->compare, comp->lockset_id);
	BUG_ON(comp->mem_info == NULL);
	comp->mark = false;

	if (NVMEIB_DEV_USE_LOCAL_LOCK(P2NV(net->port)->dev_type) &&
			test_bit(comp->opr, locks_channel->local_bypass_bmp)) {
		rv = execute_opr_local_bypass(locks_channel, comp, &bad_comp);
		goto out_err;
	}

	if (!net->send_cq) {
		_NE(error_12_disk_locks_execute_opr, "locks channel not connected");
		rv = -EPIPE;
		goto out_err;
	}
	//ib_req_notify_cq(net->send_cq, IB_CQ_NEXT_COMP);
	comp->deferred_jif = jiffies;
	list_add_tail(&comp->link_deferred, &locks_channel->defered);
	++locks_channel->num_defered;
	nvmeibc_disk_cmds_stats_pending_add(locks_channel->base.disk, &comp->lock_cmd);

#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
	do {
		struct nvmeibc_disk_command_probes_try_data *probes_current_try;
		probes_current_try = nvmeibc_disk_command_probes_current_try(&comp->probes);
		memset(probes_current_try, 0, sizeof(*probes_current_try));
		probes_current_try->ulp_post_jif = jiffies;
		if (comp->probes.n_tries == 0)
			comp->probes.ulp_first_try_jif = jiffies;
	} while(0);
#endif

	BUG_ON(locks_channel->num_defered <= 0);
	rv = lock_prepare_and_send(locks_channel, &bad_opr);
	if (rv && bad_opr) {
		/* Error occurred - fail bad opr */
		_NE(error_13_disk_locks_execute_opr,
				"lock completion comp @COMP ended with error @RV", bad_opr->comp, rv);
		bad_comp = bad_opr->comp;
		locks_free_opr(bad_opr, false, LOCK_OPR_ERROR,
					   !bad_opr->disk_piggyb ? LOCK_OPR_NO_BYPASS : LOCK_OPR_BYPASS_ERROR);
	}

out_err:
	if (bad_comp) {
		if (bad_comp == comp)
			comp->lock_status = NCL_STATUS_FAIL_NO_COMP;
		else {
			/* Different lock operation failed - queue callback */
			if (bad_comp->callback) {
				nvmeibc_disk_cmds_stats_llp_complete(locks_channel->base.disk, &bad_comp->lock_cmd, -ENOTSUPP, -ENOTSUPP);
				bad_comp->lock_status = NCL_STATUS_FAIL_COMP;
				if (NVMEIBC_DISK_LOCKS_SHOULD_DEFER_TO_PCPU_WQ(bad_comp->cpu_mask_info.mask)) {
					lock_comp_execute_cb(locks_channel, bad_comp, NULL, LOCK_COMP_EXECUTE_ON_SYSTEM_PCPU_WQ);
				} else {
					lock_comp_execute_cb(locks_channel, bad_comp, NULL, LOCK_COMP_EXECUTE_ON_WQ);
				}
			}
			/* Current lock operation hasn't failed -> clear error */
			rv = 0;
		}
	}
	nvmeibc_locks_channel_spin_unlock_irqrestore(locks_channel, flags);

out:
	NFOUT;
	return rv;
}

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
#define fill_skipped_binfo(_comp) \
	do { \
		union nvmeib_blkset_info binfo = {.bits = {.txid = DEUBG_SKIP_LOCKS_TX_ID, .dirty = 0}}; \
		_comp->val[1] = binfo.all; \
	} while(0)

#define common_skip_locks(_comp) \
	do { \
		(_comp)->lock_status = NCL_STATUS_TAKEN; \
		if ((_comp)->lock_cnsts->w_blkset_info) { \
			fill_skipped_binfo(_comp); \
		} \
		lock_comp_execute_cb(NULL, _comp, NULL, LOCK_COMP_EXECUTE_INLINE); \
	} while(0)
#endif


int nvmeibc_disk_locks_interlocked_cmp_exchange( void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp)
{
	struct nvmeibc_disk_seg_locks_mem_info *record;
	struct nvmeibc_disk_used_lock_segment *uls;
	int rv = 0;

	NFIN;
	BUG_ON(comp == NULL);
	BUG_ON(handle == NULL);
	comp->opr = NVMEIBC_LOCK_CMP_AND_SWAP;
	comp->n_retries_cmpxcng = 0;
	nvmeibc_disk_cmds_stats_init_lock_cmd(
		nvmeibc_disk_locks_interlocked_cmp_exchange_e1, comp);

	_ND(trace_disk_locks_nvmeibc_disk_locks_interlocked_cmp_exchange, "CMPSWAP: comp={@COMP, opr=@OPR, lockset_id=@LOCKSET_ID, "
	   "compare=@COMPARE, exchange=@EXCHANGE, "
	   "val[0]=@VA, val[1]=@VA, w_blkset_info=@W_BLKSET_INFO} ",
	   comp, comp->opr, comp->lockset_id, comp->compare, comp->exchange,
	   comp->val[0], comp->val[1], !!comp->lock_cnsts->w_blkset_info);

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibc_skip_lock_cmds_flags & (1 << 0))) {
		comp->n_retries_cmpxcng = 0;
		comp->compare = comp->lock.id;
		common_skip_locks(comp);
		goto out;
	}
#endif

	uls = handle;
	if (!validate_handle(uls->disk, handle, false)) {
		_NE(error_disk_locks_nvmeibc_disk_locks_interlocked_cmp_exchange, "handle could not be validate on disk @DISK_NAME", uls->disk->name);
		rv = -1;
		goto out;
	}

	record = uls->mem_info;
	if (record == NULL) {
		_NE(error_1_disk_locks_nvmeibc_disk_locks_interlocked_cmp_exchange, "locks channel is not connected");
		rv = -EAGAIN;
		goto out;
	}
	INIT_LIST_HEAD(&comp->link_deferred);
	rv = execute_opr(record, addr, comp);
	if (rv) {
		_NE(error_2_disk_locks_nvmeibc_disk_locks_interlocked_cmp_exchange, "compare exchange ended with error @RV", rv);
	}

out:
	NFOUT;
	return rv;
}

int __attribute__ ((unused)) nvmeibc_disk_locks_write_lock(void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp)
{
	int rv = 0;
	struct nvmeibc_disk_seg_locks_mem_info *record;
	struct nvmeibc_disk_used_lock_segment *uls;

	NFIN;
	BUG_ON(comp == NULL);
	BUG_ON(handle == NULL);

	comp->opr = NVMEIBC_LOCK_FORCE_WRITE;
	uls = handle;
	if (!validate_handle(uls->disk, handle, false)) {
		_NE(error_disk_locks_nvmeibc_disk_locks_write_lock, "handle could not be validate on disk @DISK_NAME", uls->disk->name);
		rv = -1;
		goto out;
	}

	if (uls->mem_info == NULL) {
		_NE(error_1_disk_locks_nvmeibc_disk_locks_write_lock, "locks channel is not connected");
		rv = -ENODEV;
		goto out;
	}
	record = uls->mem_info;
	INIT_LIST_HEAD(&comp->link_deferred);
	rv = execute_opr(record, addr, comp);
	if (rv) {
		_NE(error_2_disk_locks_nvmeibc_disk_locks_write_lock, "force write lock ended with error @RV", rv);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibc_disk_locks_write_blkset_info(void *handle, u64 addr,
						struct nvmeibc_d_rdma_comp *comp)
{
	int rv = 0;
	struct nvmeibc_disk_seg_locks_mem_info *record;
	struct nvmeibc_disk_used_lock_segment *uls;

	NFIN;
	BUG_ON(comp == NULL);
	BUG_ON(handle == NULL);
	if (!comp->lock_cnsts->w_blkset_info) {
		_NE(error_disk_locks_nvmeibc_disk_locks_write_blkset_info, "lock does not contain Blockset Info");
		WARN_ON_ONCE(1);
		rv = -1;
		goto out;
	}

	comp->opr = NVMEIBC_LOCK_BLKSET_INFO_WRITE;
	nvmeibc_disk_cmds_stats_init_lock_cmd(
		nvmeibc_disk_locks_write_blkset_info_e1, comp);

	uls = handle;
	if (!validate_handle(uls->disk, handle, false)) {
		_NE(error_1_disk_locks_nvmeibc_disk_locks_write_blkset_info, "handle could not be validate on disk @DISK_NAME", uls->disk->name);
		rv = -1;
		goto out;
	}

	if (uls->mem_info == NULL) {
		_NE(error_2_disk_locks_nvmeibc_disk_locks_write_blkset_info, "locks channel is not connected");
		rv = -ENODEV;
		goto out;
	}

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibc_skip_lock_cmds_flags & (1 << 3))) {
		comp->lock_status = NCL_STATUS_TAKEN;
		lock_comp_execute_cb(NULL, comp, NULL, LOCK_COMP_EXECUTE_INLINE);
		rv = 0;
		goto out;
	}
#endif

	record = uls->mem_info;
	INIT_LIST_HEAD(&comp->link_deferred);
	rv = execute_opr(record, addr, comp);
	if (rv) {
		_NE(error_3_disk_locks_nvmeibc_disk_locks_write_blkset_info, "write TxID ended with error @RV", rv);
	}

	out:
	NFOUT;
	return rv;
}

int nvmeibc_disk_locks_read_blkset_info(void* handle, u64 addr, struct nvmeibc_d_rdma_comp* comp)
{
	int rv = 0;
	struct nvmeibc_disk_seg_locks_mem_info *record;
	struct nvmeibc_disk_used_lock_segment *uls;

	NFIN;
	BUG_ON(comp == NULL);
	BUG_ON(handle == NULL);

	if (!comp->lock_cnsts->w_blkset_info) {
		_NE(error_disk_locks_nvmeibc_disk_locks_read_blkset_info, "lock does not contain Blockset Info");
		WARN_ON_ONCE(1);
		rv = -1;
		goto out;
	}

	comp->opr = NVMEIBC_LOCK_BLKSET_INFO_READ;
	uls = handle;
	if (!validate_handle(uls->disk, handle, false)) {
		_NE(error_1_disk_locks_nvmeibc_disk_locks_read_blkset_info, "handle could not be validate on disk @DISK_NAME", uls->disk->name);
		rv = -1;
		goto out;
	}

	if (uls->mem_info == NULL) {
		_NE(error_2_disk_locks_nvmeibc_disk_locks_read_blkset_info, "locks channel is not connected");
		rv = -ENODEV;
		goto out;
	}
	record = uls->mem_info;
	INIT_LIST_HEAD(&comp->link_deferred);
	rv = execute_opr(record, addr, comp);
	if (rv) {
		_NE(error_3_disk_locks_nvmeibc_disk_locks_read_blkset_info, "read TxID ended with error @RV", rv);
	}

	out:
	NFOUT;
	return rv;
}

int nvmeibc_disk_locks_read_lock(void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp)
{
	int rv = 0;
	struct nvmeibc_disk_used_lock_segment *uls;
	struct nvmeibc_disk_seg_locks_mem_info *record;

	NFIN;
	BUG_ON(handle == NULL);

	comp->opr = NVMEIBC_LOCK_READ;
	nvmeibc_disk_cmds_stats_init_lock_cmd(nvmeibc_disk_locks_read_lock_e1, comp);

	uls = handle;
	if (!validate_handle(uls->disk, handle, false)) {
		_NE(error_disk_locks_nvmeibc_disk_locks_read_lock, "handle could not be validate on disk @DISK_NAME", uls->disk->name);
		rv = -1;
		goto out;
	}

	if (uls->mem_info == NULL) {
		_NE(error_1_disk_locks_nvmeibc_disk_locks_read_lock, "locks channel is not connected");
		rv = -EAGAIN;
		goto out;
	}

	BUG_ON(comp == NULL);

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibc_skip_lock_cmds_flags & (1 << 2))) {
		common_skip_locks(comp);
		rv = 0;
		goto out;
	}
#endif

	record = uls->mem_info;
	INIT_LIST_HEAD(&comp->link_deferred);
	rv = execute_opr(record, addr, comp);
	if (rv) {
		_NE(error_2_disk_locks_nvmeibc_disk_locks_read_lock, "read lock ended with error @RV", rv);
	}

out:
	NFOUT;
	return rv;
}


int nvmeibc_disk_locks_extract_info(void *handle, u64 addr, int *segid,
	enum nvmeibc_disk_locks_opr opr_type, u64 *offset)
{
	int rv = 0;
	struct nvmeibc_disk_used_lock_segment *uls = handle;
	struct nvmeibc_disk_seg_locks_mem_info *record;
	u64 lockset_id;

	NFIN;
	BUG_ON(uls == NULL);
	record = uls->mem_info;

	if (!validate_handle(uls->disk, handle, false)) {
		_NE(error_disk_locks_nvmeibc_disk_locks_extract_info, "handle could not be validate on disk @DISK_NAME", uls->disk->name);
		rv = -1;
		goto out;
	}

	if (record == NULL) {
		_NE(error_1_disk_locks_nvmeibc_disk_locks_extract_info, "attempt to get lock information before connection seg=@SI, addr=@ADDR"
		   " disk=@DISK_STR", uls->seg_id, addr, uls->disk->name);
		rv = -EINVAL;
		goto out;
	}

	if (addr < record->start_addr ||
		addr > (record->start_addr + record->len)) {
		_NE(error_2_disk_locks_nvmeibc_disk_locks_extract_info, "addr @ADDR is not in a segment range", addr);
		_NE(error_3_disk_locks_nvmeibc_disk_locks_extract_info, "debug information");
		_NE(error_4_disk_locks_nvmeibc_disk_locks_extract_info, "record = @RECORD", record);
		_NE(error_5_disk_locks_nvmeibc_disk_locks_extract_info, "LOCKS: seg_id = @SEG_ID_INT", record->seg_id);
		_NE(error_6_disk_locks_nvmeibc_disk_locks_extract_info, "LOCKS: locks channel=@CHANNEL_PTR",record->locks_channel);
		_NE(error_7_disk_locks_nvmeibc_disk_locks_extract_info, "LOCKS: len = @LEN", record->len);
		_NE(error_8_disk_locks_nvmeibc_disk_locks_extract_info, "LOCKS: lock_set_size = @LOCK_SET_SIZE", record->lock_set_size);
		_NE(error_9_disk_locks_nvmeibc_disk_locks_extract_info, "LOCKS: start_addr = @START_ADDR", record->start_addr);
		_NE(error_10_disk_locks_nvmeibc_disk_locks_extract_info, "LOCKS: address = @ADDR", record->addr);
		_NE(error_11_disk_locks_nvmeibc_disk_locks_extract_info, "LOCKS: rkey = @RKEY", record->rkey);
		rv = -EINVAL;
		goto out;
	}
	lockset_id = (addr - record->start_addr) / record->lock_set_size;
	*segid = record->seg_id;
	if (opr_type == NVMEIBC_LOCK_READ) {
		*offset = lockset_id * sizeof(u64);
	}
	else if (opr_type == NVMEIBC_LOCK_BLKSET_INFO_WRITE ||
		opr_type == NVMEIBC_LOCK_BLKSET_INFO_READ) {
		*offset = lockset_id * sizeof(u64);
		_ND(trace_1_disk_locks_nvmeibc_disk_locks_extract_info, "Write to lockset_id=@LOCKSET_ID blkset_info offset=@OFFSET", lockset_id, *offset);
	}
	else {
		_NE(error_12_disk_locks_nvmeibc_disk_locks_extract_info, "Unsupported operation type (@OPR_TYPE)", opr_type);
		rv = -EINVAL;
	}

out:
	NFOUT;
	return rv;
}

static void lock_opr_log_error(struct nvmeibc_lock_opr_in_progress *opr_ip)
{
	NFIN;
	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(opr_ip->ch));
	_NE(error_disk_locks_lock_opr_log_error, "wr opcode = @OPCODE", nvmeib_send_wr_common(opr_ip->wr).opcode);
	switch (nvmeib_send_wr_common(opr_ip->wr).opcode) {
	case IB_WR_RDMA_WRITE:
	case IB_WR_RDMA_READ:
		_NE(lock_opr_log_error_e1, "remote_addr = @INT_ULLONG = 0x@_X",
			nvmeib_send_wr_rdma(opr_ip->wr).remote_addr,
			nvmeib_send_wr_rdma(opr_ip->wr).remote_addr);
		_NE(lock_opr_log_error_e2, "rkey=@INT32_HEX",
			nvmeib_send_wr_rdma(opr_ip->wr).rkey);
		break;
	case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
	case IB_WR_ATOMIC_CMP_AND_SWP:
		_NE(lock_opr_log_error_e3, "remote_addr = @INT_ULLONG = 0x@_X",
			nvmeib_send_wr_atomic(opr_ip->wr).remote_addr,
			nvmeib_send_wr_atomic(opr_ip->wr).remote_addr);
		_NE(lock_opr_log_error_e4, "rkey=@INT32_HEX",
			nvmeib_send_wr_atomic(opr_ip->wr).rkey);
		break;
	default:
		break;
	}
	_NE(error_1_disk_locks_lock_opr_log_error, "was defered = @HAD_TO_DEFER", opr_ip->had_to_defer);
	if (opr_ip->comp)
		_NE(error_2_disk_locks_lock_opr_log_error, "id = @SEND_ID", opr_ip->comp->send_id);

	NFOUT;
}

static void update_comp_status(int id, struct nvmeibc_d_rdma_comp *lock_comp,
	enum ib_wc_opcode opcode, struct nvmeibc_ib_net *net)
{
	NFIN;

	switch (opcode) {
	case IB_WC_RDMA_READ:
		BUG_ON(lock_comp->opr != NVMEIBC_LOCK_READ);
		lock_comp->lock_status = (lock_comp->lock.id ? NCL_STATUS_CONTENDED : NCL_STATUS_TAKEN);
		break;
	case IB_WC_RDMA_WRITE:
		BUG_ON(lock_comp->opr != NVMEIBC_LOCK_FORCE_WRITE &&
			   lock_comp->opr != NVMEIBC_LOCK_BLKSET_INFO_WRITE);
		lock_comp->lock_status = NCL_STATUS_TAKEN;
		break;
	case IB_WC_MASKED_COMP_SWAP:
	case IB_WC_COMP_SWAP:
		BUG_ON(lock_comp->opr != NVMEIBC_LOCK_CMP_AND_SWAP);
		nvmeibc_disk_locks_on_cmp_exchange(id, lock_comp, net);
		break;
	default:
		_NE(update_comp_status_e1, "unssuported case\n");
		lock_comp->lock_status = NCL_STATUS_FAIL_COMP;
		break;
	}

	NFOUT;
}

/* Called from nvmeibc_disk_locks_on_completion */
static int /*noinline*/ disk_locks_on_completion(struct nvmeibc_locks_channel *ch,
												 struct nvmeibc_ib_net *net, struct ib_wc *wc,
												 struct nvmeibc_lock_opr_in_progress *opr_ip)
{
	int rv = 0;
	struct nvmeibc_d_rdma_comp *lock_comp = opr_ip->comp;

	__NFIN;
	BUG_ON(lock_comp == NULL);
	BUG_ON(lock_comp->mem_info == NULL);
	BUG_ON(lock_comp->mem_info->locks_channel == NULL);

#ifdef DEBUG_LOCKS
	_ND(disk_locks_on_completion_d1,
		"LOCKS: ch=@PTR compid = @INT nvmeibc_disk_locks_on_completion"
		" opcode @INT operation = @INT",
		ch, lock_comp->send_id, wc->opcode, lock_comp->opr);
	_ND(disk_locks_on_completion_d2,
		"LOCKS: compare exchange ended with status @INT",
		wc->status);
#endif
	BUG_ON(!nvmeibc_locks_channel_spin_is_locked(ch));
	lock_check_data(ch);

	//EXC-1795: Dont see how this cond could be true (Ask YK ??) because ...
	//This cb is called under ioch and locks_spinlock && if net !dying.
	//While WD-event sets net to dying under these two locks. Added WARN-ON...
	//************************************************************************
	//UPDATE: 	This can happen if the completion sneaks in between WD timeout and setting the net->dying.
	//			We still want to complete to the block and free the opr_ip though.
	if (!opr_ip->disk_piggyb && !nvmeib_wd_is_armed_wdc(&opr_ip->wdc)) {
		_NT(trace_disk_locks_disk_locks_on_completion, "LOCKS: got completion event after timeout");
	}

	//nvmeib_net_to_cpu(net->qp, wc->opcode, opr_ip->val);
	if (wc->opcode != IB_WC_RDMA_WRITE && wc->status == IB_WC_SUCCESS) {
		if (unlikely((ch->atomic_reply_endian_swap && wc->opcode == IB_WC_COMP_SWAP) ||
		(ch->masked_atomic_reply_endian_swap && wc->opcode == IB_WC_MASKED_COMP_SWAP)))
			opr_ip->val[0] = __swab64(opr_ip->val[0]);
		if (!lock_comp->lock_cnsts->w_blkset_info) {
			/* Entire value returned is the lock id */
			lock_comp->val[0] = opr_ip->val[0];
		} else {
			/* Decompose lock into ID and blkset info */
			union nvmeib_lock_blkset_entry *ret_lock_blkset = (void*)&opr_ip->val[0];
			if (wc->opcode == IB_WC_COMP_SWAP &&
				lock_comp->nmcs_bi.n_attempts < NMCS_BI_MAX_ATTEMPTS &&
				ret_lock_blkset->blkset_info.all != lock_comp->nmcs_bi.last_value) {
				/* Responder did not support IB_WR_MASKED_ATOMIC_CMP_AND_SWP
				and original blkset_info (in val[1]) was incorrect, update and resubmit */
				_ND(trace_1_disk_locks_disk_locks_on_completion, "CMPSWAP: comp=@COMP, fail cs, store bi (@ALL) and resubmit",
				   lock_comp, ret_lock_blkset->blkset_info.all);
				lock_comp->nmcs_bi.last_value = ret_lock_blkset->blkset_info.all;
				lock_comp->nmcs_bi.n_attempts++;
				locks_free_opr(opr_ip, true, LOCK_OPR_IN_PROGRESS,
							   !opr_ip->disk_piggyb ? LOCK_OPR_NO_BYPASS : LOCK_OPR_BYPASS_COMPLETE);
				rv = -EAGAIN;
				goto out;
			}
			lock_comp->val[0] = ret_lock_blkset->lock_id.all;
			lock_comp->val[1] = ret_lock_blkset->blkset_info.all;

			if (lock_comp->nmcs_bi.n_attempts) {
				if (lock_comp->nmcs_bi.n_attempts == NMCS_BI_MAX_ATTEMPTS) {
					_NT(trace_2_disk_locks_disk_locks_on_completion, "CMPSWAP: comp=@COMP, max nmcs-bi attempts (@LAST_VALUE)",
					   lock_comp, lock_comp->nmcs_bi.last_value);
				}
				/* reset @last_value as ulp uses same comp obj for unlock.
				   otherwise, unlock's cmp&swap will fail but we'll think it had
				   succeeded coz blkset_info.all we'll read will be = last_value */
				lock_comp->nmcs_bi.n_attempts = 0;
				lock_comp->nmcs_bi.last_value = 0;
			}

			/* caller will compare lock_comp->compare to lock_comp->val[0],
			   if TRUE, lock is considered TAKEN */
		}
	}

	if (wc->status != IB_WC_SUCCESS) {
		lock_opr_log_error(opr_ip);
	}

	if (likely(wc->status == IB_WC_SUCCESS)) {
		update_comp_status(lock_comp->send_id, lock_comp, wc->opcode, net);
		locks_free_opr(opr_ip, true, LOCK_OPR_IN_PROGRESS,
			!opr_ip->disk_piggyb ? LOCK_OPR_NO_BYPASS : LOCK_OPR_BYPASS_COMPLETE);
	}
	else {
		lock_comp->lock_status = NCL_STATUS_FAIL_COMP;
		_NE(error_disk_locks_disk_locks_on_completion, "lock completion task ended with error @STATUS", wc->status);
		locks_free_opr(opr_ip, true, LOCK_OPR_IN_PROGRESS,
			!opr_ip->disk_piggyb ? LOCK_OPR_NO_BYPASS : LOCK_OPR_BYPASS_ERROR);
		/* Jared:
		 * To prevent a deadlock, don't call this here as it
		 * tries to acquire the primary lock ch spinlock.
		 * Instead we propogate the rv back to the net layer
		 locks_handle_qp_error(opr_ip->ch);*/
		rv = -1;
		goto out;
	}

out:
	_ND(trace_3_disk_locks_disk_locks_on_completion, "LOCKS: in ch = @CH_PTR free @NUM_OF_FREE progress @NUM_IN_PROGRESS deferd = @NUM_DEFERED @CALLBACK_PTR status @LOCK_STATUS_INT", ch,
		ch->num_of_free, ch->num_in_progress, ch->num_defered,
		lock_comp->callback, lock_comp->lock_status);
	__NFOUT;
	return rv;
}

static void bad_opr_comp_callback(struct nvmeibc_locks_channel *ch, struct nvmeibc_d_rdma_comp *bad_opr_comp)
{
	_ND(trace_disk_locks_bad_opr_comp_callback, "LOCKSUP: result    comp @SEND_ID opr = @OPR exchange = @EXCHANGE compare = @COMPARE"
	" lockset @LOCKSET_ID val=@VAL status=@STATUS", bad_opr_comp->send_id, bad_opr_comp->opr,
		bad_opr_comp->exchange, bad_opr_comp->compare, bad_opr_comp->lockset_id,
		bad_opr_comp->val[0], bad_opr_comp->lock_status);
	nvmeibc_disk_cmds_stats_llp_complete(ch->base.disk, &bad_opr_comp->lock_cmd, -ENOTSUPP, -ENOTSUPP);
	bad_opr_comp->lock_status = NCL_STATUS_FAIL_COMP;
	lock_comp_execute_cb(ch, bad_opr_comp, NULL, NVMEIBC_DISK_LOCKS_SHOULD_DEFER_TO_PCPU_WQ(bad_opr_comp->cpu_mask_info.mask) ? LOCK_COMP_EXECUTE_ON_SYSTEM_PCPU_WQ : LOCK_COMP_EXECUTE_INLINE);
}

bool nvmeibc_disk_locks_fast_reuse = false;
module_param_named(disk_locks_fast_reuse, nvmeibc_disk_locks_fast_reuse, bool, 0644);
MODULE_PARM_DESC(disk_locks_fast_reuse, "Reuse a disk lock (disk-lock opr) before calling the callback from releasing the lock (ulp cb). This is a boolean. This is a potential optimization.");

/* Called from lock_send_completion (net->call_send_comp_handler)   */
int nvmeibc_disk_locks_on_completion(struct nvmeibc_locks_channel *ch,
									 struct nvmeibc_ib_net *net, struct ib_wc *wc, bool last_wc_in_series)
{
	int rv = 0;
	u32 wr_idx = nvmeib_idx_from_wc(wc);
	u16 wc_version = nordda_wr_id_decode_version(nvmeib_wr_id_from_wc(wc));
	struct nvmeibc_lock_opr_in_progress *opr_ip = &ch->locks_ip_buffer[wr_idx], *bad_opr = NULL;
	struct nvmeibc_d_rdma_comp *lock_comp, *bad_opr_comp = NULL;
	unsigned long flags;
	bool err = false;
	bool execute_callback = true;

	MyWait();
	__NFIN;

	nvmeibc_locks_channel_spin_lock_irqsave(ch, flags);

	if (opr_ip->version != wc_version) {
		_NE(nvmeibc_disk_locks_on_completion_ver_mismatch,
		    "LOCKS: ch @CH_PTR opr_ip @OPR_IP index @INDEX version @VERSION does not match wr_id @WR_ID version @VERSION", ch, opr_ip, opr_ip->index, opr_ip->version, nvmeib_wr_id_from_wc(wc), wc_version);
		BUG();
	}

	lock_comp = opr_ip->comp;
	if (!lock_comp || atomic_read(&ch->base.dying) || opr_ip->aborted) {
		nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags);
		goto out;
	}

	if ((ch->num_in_progress + ch->num_of_free + ch->num_aborted) != ch->total_num_opr) {
		_NE(nvmeibc_disk_locks_on_completion_e1,
			"BAD SUM: num_in_progress=@INT num_of_free=@INT "
			"num_aborted=@INT total_num_op=@INT",
			ch->num_in_progress, ch->num_of_free, ch->num_aborted,
			ch->total_num_opr);
		_NT(nvmeibc_disk_locks_on_completion_t1,
			"-->LOCKS: in ch = @PTR progress free @INT @INT "
			"deferd = @INT @PTR status @INT", ch,
			ch->num_in_progress, ch->num_of_free,
			ch->num_defered, lock_comp->callback, lock_comp->lock_status);
		_NT(nvmeibc_disk_locks_on_completion_t2,
			"LOCKS: nvmeibc_disk_locks_on_completion opcode @INT status @INT",
			wc->opcode, wc->status);
		BUG();
	}

	if (lock_comp->opr == NVMEIBC_LOCK_CMP_AND_SWAP) {
		struct nvmeibc_d_rdma_comp *comp = lock_comp;
		_ND(trace_disk_locks_nvmeibc_disk_locks_on_completion, "CMPSWAP: comp={@COMP, opr=@OPR, lockset_id=@LOCKSET_ID, compare=@COMPARE, "
		   "exchange=@EXCHANGE, val[0]=@VA, val[1]=@VA, w_blkset_info=@W_BLKSET_INFO}, "
		   "opr_ip={@OPR_IP, val[0]=@VA, val[1]=@VA}",
		   comp, comp->opr, comp->lockset_id, comp->compare, comp->exchange,
		   comp->val[0], comp->val[1], comp->lock_cnsts->w_blkset_info,
		   opr_ip, opr_ip->val[0], opr_ip->val[1]);
	}

#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
	if (wc->status != IB_WC_GENERAL_ERR) /* IB_WC_GENERAL_ERR is set by drop_error_on_pending */
		nvmeibc_disk_command_probes_current_try(&lock_comp->probes)->llp_comp_jif = jiffies;
#endif

	_ND(trace_1_disk_locks_nvmeibc_disk_locks_on_completion, "LOCKS: calling @LOCKSET_ID...", opr_ip->comp->lockset_id);
	if ((rv = disk_locks_on_completion(ch, net, wc, opr_ip))) {
		if (rv == -EAGAIN && !ch->paused) {
			/* Resubmit lock */
			_ND(trace_2_disk_locks_nvmeibc_disk_locks_on_completion, "LOCKS: resubmitting lock comp @SEND_ID opr = @OPR exchange = @EXCHANGE compare = @COMPARE"
			" lockset @LOCKSET_ID val=@VAL", lock_comp->send_id, lock_comp->opr,
			lock_comp->exchange, lock_comp->compare, lock_comp->lockset_id, lock_comp->val[0]);
			lock_comp->deferred_jif = jiffies;
			list_add_tail(&lock_comp->link_deferred, &ch->defered);
			++ch->num_defered;
			nvmeibc_disk_cmds_stats_err_internal_retry(ch->base.disk, &lock_comp->lock_cmd);
			execute_callback = false;
			rv = 0;
		} else {
			_NE(error_disk_locks_nvmeibc_disk_locks_on_completion, "operator ended with error.. bailing");
			err = true;
		}
	}
	_ND(trace_3_disk_locks_nvmeibc_disk_locks_on_completion, "returned...");

	if (!err && opr_ip->first)
		ch->num_of_series_ip--;
	//ib_req_notify_cq(ch->net.send_cq, IB_CQ_NEXT_COMP);
	if (!err && (last_wc_in_series || unlikely(nvmeibc_disk_locks_fast_reuse))) {
		lock_prepare_and_send(ch, &bad_opr);
		if (bad_opr) {
			/* Error occurred - fail bad opr */
			_NE(error_1_disk_locks_nvmeibc_disk_locks_on_completion, "lock completion task ended with error @RV", rv);
			bad_opr_comp = bad_opr->comp;
			locks_free_opr(bad_opr, false, LOCK_OPR_ERROR, LOCK_OPR_NO_BYPASS);
		}
	}
	nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags);

	if (execute_callback) {
#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
		struct nvmeibc_disk_command_probes probes_copy;
		struct nvmeibc_disk_command_probes_try_data *current_try_copy;
		enum nvmeibc_disk_locks_opr t_opr = lock_comp->opr;
		u64 lockset_id = lock_comp->lockset_id;
		u64 cmp = lock_comp->compare;
		u64 xchg = lock_comp->exchange;
		u64 val0 = lock_comp->val[0];
		u64 val1 = lock_comp->val[1];

		nvmeibc_disk_command_probes_current_try(&lock_comp->probes)->ulp_comp_jif = jiffies;
		probes_copy = lock_comp->probes;
		current_try_copy = &probes_copy.tries[probes_copy.n_tries % NVMEIBC_DISK_COMMAND_PROBES_N_TRIES];
#endif

		_ND(trace_4_disk_locks_nvmeibc_disk_locks_on_completion, "LOCKSUP: result    comp @SEND_ID opr = @OPR exchange = @EXCHANGE compare = @COMPARE"
		" lockset @LOCKSET_ID val=@VAL status=@STATUS", lock_comp->send_id, lock_comp->opr,
		lock_comp->exchange, lock_comp->compare, lock_comp->lockset_id,
		lock_comp->val[0], lock_comp->lock_status);
		BUG_ON(lock_comp->callback == NULL);
		nvmeibc_disk_cmds_stats_llp_complete(ch->base.disk, &lock_comp->lock_cmd, -ENOTSUPP, -ENOTSUPP);
		lock_comp_execute_cb(ch, lock_comp, wc, NVMEIBC_DISK_LOCKS_SHOULD_DEFER_TO_PCPU_WQ(lock_comp->cpu_mask_info.mask) ? LOCK_COMP_EXECUTE_ON_SYSTEM_PCPU_WQ : LOCK_COMP_EXECUTE_INLINE);

#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
		do {
			unsigned long end_jif, rtt, total_rtt;
			//EC-4571: Loop of single netwrok disaster --> cont_preventors for 27s completed via good-path lock-comp
			end_jif = jiffies;
			rtt = end_jif - current_try_copy->ulp_post_jif;
			total_rtt = end_jif - probes_copy.ulp_first_try_jif;
			if (rtt > ch->net.wd_timeout_jif || total_rtt > NVMEIB_MAX_LOCK_TIME_INC_RETRIES) {
				_NW(warn_0_disk_locks_nvmeibc_disk_locks_on_completion,
					"@CH_NAME, @LOCK_CH, opr_ip=@OPR_IP, opr=@OPR, lockset @LOCKSET_ID, compare = @COMPARE, exchange = @EXCHANGE, val[0] = @VAL, val[1] = @VAL : ulp-RTT > WD-Timeout [jif] || total-RTT > NVMEIB_MAX_LOCK_TIME_INC_RETRIES, "
					"(@LD > @WD_TIMEOUT_JIF), rtt = {"
					"pending=@LD, "
					"wire-rtt=@LD, "
					"rsc-reuse=@LD "
					"ulp-cb=@LD"
					"}, n_retries=@RETRY_CNT, total-RTT=@LD",
					ch->base.name, ch, opr_ip, t_opr, lockset_id, cmp, xchg, val0, val1,
					rtt, ch->net.wd_timeout_jif,
					(current_try_copy->llp_post_jif ?
							current_try_copy->llp_post_jif : end_jif) - current_try_copy->ulp_post_jif,
					current_try_copy->llp_comp_jif ? current_try_copy->llp_comp_jif - current_try_copy->llp_post_jif : -1,
					current_try_copy->ulp_comp_jif - current_try_copy->llp_comp_jif,
					end_jif - current_try_copy->ulp_comp_jif,
					probes_copy.n_tries, total_rtt);
			}
		} while(0);
#endif
	}

	if (bad_opr_comp) {
		/* Also call the callback for the bad operation from lock_prepare_and_send */
		bad_opr_comp_callback(ch, bad_opr_comp);
	}

out:
	__NFOUT;
	return rv;
}

/* Called from lock_send_completion (net->call_send_comp_handler)   */
void nvmeibc_disk_locks_process_deferred(struct nvmeibc_locks_channel *ch)
{
	struct nvmeibc_lock_opr_in_progress *bad_opr = NULL;
	struct nvmeibc_d_rdma_comp *bad_opr_comp = NULL;
	bool already_locked = nvmeibc_locks_channel_spin_is_locked(ch);
	unsigned long flags;
	__NFIN;

	BUG_ON(already_locked);
	nvmeibc_locks_channel_spin_lock_irqsave(ch, flags);
	lock_prepare_and_send(ch, &bad_opr);
	if (bad_opr) {
		bad_opr_comp = bad_opr->comp;
		locks_free_opr(bad_opr, false, LOCK_OPR_ERROR, LOCK_OPR_NO_BYPASS);
	}
	nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags);

	if (bad_opr_comp) {
		bad_opr_comp_callback(ch, bad_opr_comp);
	}

	__NFOUT;
}

void nvmeibc_disk_locks_on_cmp_exchange( int id,
	struct nvmeibc_d_rdma_comp *lock_comp, struct nvmeibc_ib_net *net)
{
	static const uint64_t LOCK_CONTENTION_RETRY_WARNING = 512;
	NFIN;
	//if the channel is on mlx5, reverse byte order.
	if (lock_comp->compare == lock_comp->lock.id) {
		lock_comp->lock_status = NCL_STATUS_TAKEN;
		lock_comp->n_retries_cmpxcng = 0;
	} else {
		_ND(t_01_cdlcce, "LOCKS: contention id=@ID_INT, compare=@COMPARE holder_lock_id=@LOCK_ENT_U64 val[1]=@ADDR desired_lock_id=@DESIRED_LOCK_ID addr=@ADDR=@ADDR",
		   id, lock_comp->compare, lock_comp->val[0], lock_comp->val[1], lock_comp->exchange,
			lock_comp->lockset_id, lock_comp->lockset_id << 5);
		lock_comp->lock_status = NCL_STATUS_CONTENDED;
		lock_comp->n_retries_cmpxcng++;
		if (lock_comp->n_retries_cmpxcng == LOCK_CONTENTION_RETRY_WARNING) {
			_NT(warn_disk_locks_nvmeibc_disk_locks_on_cmp_exchange,
				"LOCKS: Contended after @RETRY_COUNT_LLONG retries, id=@ID_INT, "
				"compare=@COMPARE, holder_lock_id=@LOCK_ENT_U64, "
				"val[1]=@ADDR desired_lock_id=@DESIRED_LOCK_ID, addr=@ADDR=@ADDR, "
				"(qpn: @QP_NUM, remote_qpn: @REMOTE_QPN, remote: @RHOST_NAME, "
				"disk=@DISK_STR)",
				lock_comp->n_retries_cmpxcng, id,
				lock_comp->compare, lock_comp->val[0],
				lock_comp->val[1], lock_comp->exchange, lock_comp->lockset_id, lock_comp->lockset_id << 5,
				net->qp->qp_num, net->remote_qpn, net->admin_ch->base.rhost_name,
				net->admin_ch->base.disk->name);
			nvmeibc_disk_report_long_contended_lock(net->admin_ch->base.disk);
		}
	}

	NFOUT;
}

#define wr_to_opr(_wr) ((struct nvmeibc_lock_opr_in_progress *)\
	container_of(_wr, struct nvmeibc_lock_opr_in_progress, wr))

static int lock_opr_cmp_swap_to_disk_cmd(
	struct nvmeib_gen_cmd_lock_param *lock_param,
	struct nvmeibc_lock_opr_in_progress *opr)
{
	int rv = 0;
	struct nvmeibc_disk_seg_locks_mem_info *record = opr->comp->mem_info;
	u64 remote_offset = nvmeib_send_wr_atomic(opr->wr).remote_addr - record->addr;
	u64 opcode = nvmeib_send_wr_common(opr->wr).opcode;

	if (opcode == IB_WR_ATOMIC_CMP_AND_SWP) {
		lock_param->op = NVMEIB_ATOMIC_CMP_AND_SWP;
	} else if (opcode == IB_WR_MASKED_ATOMIC_CMP_AND_SWP) {
		lock_param->op = NVMEIB_MASKED_ATOMIC_CMP_AND_SWP;
		lock_param->atomic.compare_add_mask = nvmeib_send_wr_atomic(opr->wr).compare_add_mask;
		lock_param->atomic.swap_mask = nvmeib_send_wr_atomic(opr->wr).swap_mask;
	} else {
		_NE(lock_opr_cmp_swap_to_disk_cmd_e1,
			"invalid lock opcode=@INT_ULLONG", opcode);
		rv = -1;
		goto out;
	}
	lock_param->atomic.compare_add = nvmeib_send_wr_atomic(opr->wr).compare_add;
	lock_param->atomic.swap = nvmeib_send_wr_atomic(opr->wr).swap;
	lock_param->offset = remote_offset;

out:
	return rv;
}

static int lock_opr_rdma_to_disk_cmd(
	struct nvmeib_gen_cmd_lock_param *lock_param,
	struct nvmeibc_lock_opr_in_progress *opr)
{
	int rv = 0;
	struct nvmeibc_disk_seg_locks_mem_info *record = opr->comp->mem_info;
	u64 remote_addr = nvmeib_send_wr_rdma(opr->wr).remote_addr;
	u64 remote_base_addr;

	enum nvmeibc_disk_locks_opr lock_op = opr->comp->opr;
	struct ib_sge *ib_sge = nvmeib_send_wr_common(opr->wr).sg_list;

	switch (lock_op) {
	case NVMEIBC_LOCK_FORCE_WRITE:
	case NVMEIBC_LOCK_BLKSET_INFO_WRITE:
		lock_param->op = NVMEIB_LOCK_RDMA_WRITE;
		break;

	case NVMEIBC_LOCK_READ:
	case NVMEIBC_LOCK_BLKSET_INFO_READ:
		lock_param->op = NVMEIB_LOCK_RDMA_READ;
		break;
	default:
		_NE(lock_opr_rdma_to_disk_cmd_e1, "invalid lock opcode=@INT", lock_op);
		rv = -1;
		goto out;
	}

	lock_param->rdma.data = (void *)opr->val + (ib_sge->addr - opr->val_phys);
	lock_param->rdma.len = ib_sge->length;

	{
		remote_base_addr = record->addr;
		lock_param->rdma.table_type = NVMEIB_LOCK_TABLE_OWNER;
	}

	lock_param->offset = remote_addr - remote_base_addr;

out:
	return rv;
}

int nvmeibc_lock_opr_to_disk_cmd(
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd,
	struct nvmeibc_lock_opr_in_progress *opr)
{
	struct nvmeib_gen_cmd_lock_param *lock_param = &disk_lock_cmd->lock_param;
	int rv = 0;
	enum nvmeibc_disk_locks_opr lock_op = opr->comp->opr;

	memset(lock_param, 0, sizeof(*lock_param));
	memset(&disk_lock_cmd->lock_rsp, 0, sizeof(disk_lock_cmd->lock_rsp));
	disk_lock_cmd->lock_rsp.comp_code = NVMEIBC_DISK_CMD_COMP_CODE_INVALID;

	nvmeibc_disk_cmds_stats_done_reset(&disk_lock_cmd->disk_cmd);
	disk_lock_cmd->index = opr->index;
	disk_lock_cmd->ch = opr->ch;
	lock_param->seg_id = opr->comp->mem_info->seg_id;

	switch (lock_op) {
	case NVMEIBC_LOCK_CMP_AND_SWAP:
		rv = lock_opr_cmp_swap_to_disk_cmd(lock_param, opr);
		break;

	case NVMEIBC_LOCK_FORCE_WRITE:
	case NVMEIBC_LOCK_BLKSET_INFO_WRITE:
	case NVMEIBC_LOCK_READ:
	case NVMEIBC_LOCK_BLKSET_INFO_READ:
		rv = lock_opr_rdma_to_disk_cmd(lock_param, opr);
		break;

	default:
		_NE(nvmeibc_lock_opr_to_disk_cmd_e1,
			"unrecognized lock operation @INT", lock_op);
		rv = -1;
	}

	return rv;
}

int nvmeibc_disk_locks_server_side_post_send_atomic(struct ib_qp *qp,
	struct nvmeib_send_wr *send_wr, struct nvmeib_send_wr **bad_send_wr)
{
	struct nvmeib_send_wr *wr = send_wr;
	struct nvmeib_send_wr *twr;
	struct nvmeibc_lock_opr_in_progress *opr_ip;
	struct nvmeibc_locks_channel *ch;
	bool rdma_bypass;
	u64 opcode;
	int rv = 0;

	NFIN;
	while (wr) {
		twr = wr;
		wr = nvmeib_send_wr_next_ptr(*wr);
		nvmeib_send_wr_clear_next(*twr);
		opr_ip = wr_to_opr(twr);
		ch = opr_ip->ch;
		opcode = nvmeib_send_wr_common(*twr).opcode;

		if (unlikely(ch->atomic_cap)) {
			// _NE(nvmeibc_disk_locks_server_side_post_send_atomic_e1,
			//		"Reached srv-side locks although HW supports atomic-ops");
			_NE(error_disk_locks_nvmeibc_disk_locks_server_side_post_send_atomic, "Reached srv-side locks although HW supports atomic-ops\n");
			WARN_ON_ONCE(1);
			rv = -EINVAL;
			break;
		}

		NVMEIBC_LOCK_GUARD_GET_CHECK(locks_free_opr_e7,
			&opr_ip->state, LOCK_OPR_PREPARING);
		NVMEIBC_LOCK_GUARD_GET_CHECK(locks_free_opr_e8,
			&opr_ip->bypass_state, LOCK_OPR_NO_BYPASS);

		if (opcode == IB_WR_ATOMIC_CMP_AND_SWP ||
			opcode == IB_WR_MASKED_ATOMIC_CMP_AND_SWP) {
			/* any dev w/o atomic-ops (broadcom, siw, etc.)*/
			rdma_bypass = true;
		} else if ((ch->base.disk->access_local ||
			P2NV(ch->net.port)->dev_type == DT_siw) &&
				(opcode == IB_WR_RDMA_WRITE ||
				 opcode == IB_WR_RDMA_READ)) {
			/* siw must use srv-side to compensate on notifying send-comp
			   to upper-layer before reaching receiver's mem */
			rdma_bypass = true;
		} else {
			rdma_bypass = false;
		}

		if (rdma_bypass) {
			if ((rv = nvmeibc_lock_opr_to_disk_cmd(&opr_ip->disk_lock_cmd, opr_ip)) < 0)
				goto out;
			NVMEIBC_LOCK_GUARD_SWITCH_CHECK(
				nvmeibc_disk_locks_server_side_post_send_atomic_e1000,
				&opr_ip->bypass_state,
				LOCK_OPR_NO_BYPASS, LOCK_OPR_USING_BYPASS);
			if ((rv = nvmeibc_disk_execute_lock(ch->base.disk, &opr_ip->disk_lock_cmd)) >= 0) {
				ch->num_disk_piggyb++;
				opr_ip->disk_piggyb = true;
			} else if (rv == -ENOMEM) {
				NVMEIBC_LOCK_GUARD_SWITCH_CHECK(
					nvmeibc_disk_locks_server_side_post_send_atomic_e1001,
					&opr_ip->bypass_state, LOCK_OPR_USING_BYPASS,
					LOCK_OPR_NO_BYPASS);
			} else {
				NVMEIBC_LOCK_GUARD_SWITCH_CHECK(
					nvmeibc_disk_locks_server_side_post_send_atomic_e1001,
					&opr_ip->bypass_state, LOCK_OPR_USING_BYPASS,
					LOCK_OPR_BYPASS_ERROR);
			}
		} else {
			rv = nvmeibc_ib_post_send(&ch->net, nvmeib_send_wr_to_ib_ptr(*twr), NULL);
		}

		nvmeib_send_wr_set_next(*twr, wr);
		if (rv) {
			*bad_send_wr = twr;
			break;
		}
	}
out:
	NFOUT;
	return rv;
}

