/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "nvmeib_nonsleepable.h"
#include "nvmeibc_locks_channel.h"
#include "nvmeib.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_ib_admin_channel.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeib_utils.h"
#include "nvmeib_public.h"
#include "nvmeibc_jam.h"
#include "nvmeibc_trace.h"
#include "core/nvmeibc_core_common.h"
#include "nvmeibc_trend_types.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib_cpu_masks.h"
#include "nvmeib_metrics.h"

#define lock_ch_from_net(net_ptr) \
	container_of(net_ptr->ioch, struct nvmeibc_locks_channel, base)

#define is_primary_ch(ch_ptr) \
	(ch_ptr->primary_ch == ch_ptr)

#define get_primary_ch(ch_ptr) \
	(ch_ptr->primary_ch)

static unsigned int nvmeibc_max_lock_channels = 5; /* backword-compat */
module_param_named(max_lock_channels, nvmeibc_max_lock_channels, uint, 0644);
MODULE_PARM_DESC(max_lock_channels, "The maximum number of lock channels for non-TCP transports per disk.");

static unsigned int nvmeibc_max_lock_channels_tcp = NVMEIB_MAX_LOCK_TCP_CHANNELS; /* backword-compat */
module_param_named(max_lock_channels_tcp, nvmeibc_max_lock_channels_tcp, uint, 0644);
MODULE_PARM_DESC(max_lock_channels_tcp, "The maximum number of lock channels for TCP transports per disk.");

static int nvmeibc_lock_channel_choosing_method = LOCK_CHANNEL_CHOOSING_METHOD_LRU;
module_param_named(lock_ch_get_method, nvmeibc_lock_channel_choosing_method, int, 0644);
MODULE_PARM_DESC(lock_ch_get_method, "Determines the method for choosing the lock channel for RDMA communication. Possible values: "
									 "0 = LRU - tie break by sharding, "
									 "1 = BY_CPU, "
									 "2 = SHARDING by destination address");

static int nvmeibc_lock_channel_choosing_method_tcp = LOCK_CHANNEL_CHOOSING_METHOD_BY_CPU;
module_param_named(lock_ch_get_method_tcp, nvmeibc_lock_channel_choosing_method_tcp, int, 0644);
MODULE_PARM_DESC(lock_ch_get_method_tcp, "Determines the method for choosing the lock channel for TCP communication, same values as for RDMA, see above.");

static bool nvmeibc_lock_ch_scq_offload_thread = true;
module_param_named(lock_ch_scq_offload_thread, nvmeibc_lock_ch_scq_offload_thread, bool, 0644);
MODULE_PARM_DESC(lock_ch_scq_offload_thread, "Use a thread for offload processing for RDMA shared completion queue handling.");

static bool nvmeibc_lock_ch_scq_offload_thread_tcp = true;
module_param_named(lock_ch_scq_offload_thread_tcp, nvmeibc_lock_ch_scq_offload_thread_tcp, bool, 0644);
MODULE_PARM_DESC(lock_ch_scq_offload_thread_tcp, "Use a thread for offload processing for SIW shared completion queue handling.");

bool nvmeibc_lock_ch_scq_use_kwq = false;
module_param_named(lock_ch_scq_use_kwq, nvmeibc_lock_ch_scq_use_kwq, bool, 0644);
MODULE_PARM_DESC(lock_ch_scq_use_kwq, "Determines whether to use kernel workqueue instead of kthread for SCQ offload processing.");

bool nvmeibc_locks_scq_wq_unbound = false;
module_param_named(locks_scq_wq_unbound, nvmeibc_locks_scq_wq_unbound, bool, 0444);
MODULE_PARM_DESC(locks_scq_wq_unbound, "Determines whether to use an unbound kernel workqueue for nvmeibc_locks_scq (true) or a bound one (false).");

/* Kernel workqueue for locks channel SCQ operations */
static struct workqueue_struct *nvmeibc_locks_channel_wq;

int nvmeibc_locks_channel_wq_init(void)
{
	NFIN;
	if (nvmeibc_lock_ch_scq_use_kwq) {
		unsigned int flags = WQ_SYSFS;
		if (nvmeibc_locks_scq_wq_unbound)
			flags |= WQ_UNBOUND;
		nvmeibc_locks_channel_wq = alloc_workqueue("nvmeibc_locks_scq", flags, 0);
		if (!nvmeibc_locks_channel_wq) {
			_NE(error_nvmeibc_locks_channel_wq_init, "Failed to allocate locks channel SCQ workqueue");
			NFOUT;
			return -ENOMEM;
		}
		_NT(trace_nvmeibc_locks_channel_wq_init, "Created locks channel SCQ workqueue");
	}
	NFOUT;
	return 0;
}

void nvmeibc_locks_channel_wq_destroy(void)
{
	NFIN;
	if (nvmeibc_locks_channel_wq) {
		_ND(trace_nvmeibc_locks_channel_wq_destroy, "Destroying locks channel SCQ workqueue");
		destroy_workqueue(nvmeibc_locks_channel_wq);
		nvmeibc_locks_channel_wq = NULL;
	}
	NFOUT;
}

struct workqueue_struct *nvmeibc_locks_channel_get_wq(void)
{
	return nvmeibc_locks_channel_wq;
}
EXPORT_SYMBOL(nvmeibc_locks_channel_get_wq);

static unsigned int nvmeibc_lock_ch_2nd_ch_pcpu = 0;
module_param_named(lock_ch_2nd_ch_pcpu, nvmeibc_lock_ch_2nd_ch_pcpu, uint, 0644);
MODULE_PARM_DESC(lock_ch_2nd_ch_pcpu,
		 "Enable, disable or set the number of secondary lock channels as per-cpu lock channels. "
		 "Offline CPUs within the configured CPUs range are not compensated for. "
		 "Possible values: "
		 "0 = disabled, "
		 "1 = use max possible channels (min(num-cpus, 128)), "
		 "Other value (when lock_ch_pcpu_cpus=\"\") = cpu [0, i) use a per-cpu secondary-channel [0, i). Other cpus share the primary lock channel.");

static bool nvmeibc_lock_ch_2nd_ch_pcpu_lockless = true;
module_param_named(lock_ch_2nd_ch_pcpu_lockless, nvmeibc_lock_ch_2nd_ch_pcpu_lockless, bool, 0644);
MODULE_PARM_DESC(lock_ch_2nd_ch_pcpu_lockless, "Determines whether the per-CPU storage-level lock channels are run lockless compared to other threads and CPUs. This reduces contention and increases performance. NOTE: There must be a pcpu channel for all submission cores or it will fallback to the shared channels with locking.");

static char *nvmeibc_lock_ch_pcpu_cpus = NULL;
module_param_named(lock_ch_pcpu_cpus, nvmeibc_lock_ch_pcpu_cpus, charp, 0644);
MODULE_PARM_DESC(lock_ch_pcpu_cpus, "A list of CPUs on which to pin the secondary per-cpu lock channels. The format is a hex-mask list where each entry is 32-bits, e.g., 1f,ff for CPUS 0-7, 32-36. If the list is empty, use all cores.");

static unsigned int nvmeibc_lock_ch_2nd_ch_coremask = 0;
module_param_named(lock_ch_2nd_ch_coremask, nvmeibc_lock_ch_2nd_ch_coremask, uint, 0644);
MODULE_PARM_DESC(lock_ch_2nd_ch_coremask, "Enable, disable or set whether to use secondary channels as coremasks channels. "
					"Possible values: "
					"0 - Disabled, "
					"> 0 - Max number of channels per mask to connect,"
					"All other IOs use primary channel.");

static int handle_locks_message(struct nvmeibc_ib_net *net, struct ib_wc *wc)
{
	NFIN;
	_NE(error_locks_channel_handle_locks_message, "lock received unexpected message, ignoring it");

	NFOUT;
	return 1;
}

/**handle login response for locking service   */
static int on_login_lock_ch(struct nvmeibc_ib_net *net,
	struct nvmeibs_login_response *lrsp, int n_ext)
{
	struct nvmeibc_locks_channel *ch = lock_ch_from_net(net);
	int rv = 0;

	NFIN;
	if (is_primary_ch(ch)) {
		_NT(trace_locks_channel_on_login_lock_ch, "Locks channel is now logged in");
		ch->tgt_atomic_cap = lrsp->base.lrsp.atomic_cap;
		ch->tgt_masked_atomic_cap = lrsp->base.lrsp.masked_atomic_cap;
		ch->atomic_test_zone_rkey = be32_to_cpu(lrsp->base.lrsp.atomic_test_zone_rkey);
		ch->atomic_test_zone_raddr = be64_to_cpu(lrsp->base.lrsp.atomic_test_zone_raddr);
	} else {
		_NT(trace_1_locks_channel_on_login_lock_ch, "Secondary lock ch @CHANNEL (@CH_PTR) is now logged in", ch->base.index - 1, ch);
		ch->tgt_atomic_cap = lrsp->base.lrsp.atomic_cap;
		ch->tgt_masked_atomic_cap = lrsp->base.lrsp.masked_atomic_cap;
		ch->atomic_test_zone_rkey = be32_to_cpu(lrsp->base.lrsp.atomic_test_zone_rkey);
		ch->atomic_test_zone_raddr = be64_to_cpu(lrsp->base.lrsp.atomic_test_zone_raddr);
	}

	NFOUT;
	return rv;
}

//we need to make sure that the net spinlock is always taken before
// locks channel spinlock to prevend dead locks. So this function is
//called prior calling the watchdog callback!!!!
static bool on_locks_start_wd_event(void *oprv)
{
	struct nvmeibc_lock_opr_in_progress *opr = oprv;
	struct nvmeibc_locks_channel *ch = opr->ch;
	bool rv = true;

	NFIN;
	/* no need for irq_save because the WD thread already disabled the irq */
	nvmeibc_channel_spin_lock(ch->net.ioch);
	_ND(trace_locks_channel_on_locks_start_wd_event, "Locked ch @IOCH, irq_off=@IRQ_OFF", ch->net.ioch, irqs_disabled() ? 1 : 0);

	nvmeibc_locks_channel_spin_lock(ch);
	//ch->locking_cpu = smp_processor_id();

	NFOUT;
	return rv;
}

static void on_locks_end_wd_event(void *oprv)
{
	struct nvmeibc_lock_opr_in_progress *opr = oprv;
	struct nvmeibc_locks_channel *ch = opr->ch;

	NFIN;
	//ch->locking_cpu = -1;
	nvmeibc_locks_channel_spin_unlock(ch);

	_ND(trace_locks_channel_on_locks_end_wd_event, "Unlock ch @IOCH, irq_off=@IRQ_OFF", ch->net.ioch, irqs_disabled() ? 1 : 0);
	nvmeibc_channel_spin_unlock(ch->net.ioch);
	NFOUT;
}

static int handle_watchdog_event_locks_ch(void *cntx, unsigned long time_passed)
{
	int rv = 1;
	struct nvmeibc_lock_opr_in_progress *opr = cntx;

	NFIN;
	if (opr->comp)
		rv = nvmeibc_disk_locks_handle_wd_event(opr, time_passed);
	NFOUT;
	return rv;
}

static void init_lock_watchdog(struct nvmeibc_locks_channel *chl,
	struct nvmeibc_lock_opr_in_progress *opr)
{
	struct nvmeibc_channel *ch = &chl->base;

	NFIN;
	opr->wdc.wd = ch->wd;
	opr->wdc.cntx = opr;
	opr->wdc.on_start = on_locks_start_wd_event;
	opr->wdc.process = handle_watchdog_event_locks_ch;
	opr->wdc.on_end = on_locks_end_wd_event;
	nvmeib_wd_init_wdc(&opr->wdc);
	nvmeib_wd_add_wdc(&opr->wdc);

	NFOUT;
}

/* Called from locks_rw_disconnect_ch and free_premature_lock_ch */
static void free_dma_resources(struct nvmeibc_locks_channel *ch)
{
	struct nvmeibc_lock_opr_in_progress *opr_ip;
	NFIN;

	WARN_ON(ch->num_disk_piggyb > 0);

	if (ch->num_of_free + ch->num_aborted != ch->total_num_opr) {
		_NE(error_locks_channel_free_dma_resources, "OOPS: Free-pool leak, @NUM_OF_FREE expected @TOTAL_NUM_OPR",
			ch->num_of_free, ch->total_num_opr);
		WARN_ON(1);
		nvmeibc_disk_locks_abort_in_progress_oprs(ch, true);
		BUG_ON(ch->num_of_free + ch->num_aborted != ch->total_num_opr);
	}

	/* ch->total_num_opr is being incremnted for the first time only after we init the aborted and free lists */
	if (!ch->total_num_opr)
		goto out;

	while ((opr_ip = list_first_entry_or_null(&ch->aborted,
		struct nvmeibc_lock_opr_in_progress, link))) {
		BUG_ON(ch->num_aborted <= 0);
		NVMEIBC_LOCK_GUARD_GET_CHECK(locks_free_opr_e7,
			&opr_ip->state, LOCK_OPR_ABORTED);
		nvmeib_wd_remove_wdc(&opr_ip->wdc);
		ch->num_aborted--;
		list_del_init(&opr_ip->link);
	}

	while ((opr_ip = list_first_entry_or_null(&ch->free_ip_pool,
		struct nvmeibc_lock_opr_in_progress, link))) {
		BUG_ON(ch->num_of_free == 0);
		NVMEIBC_LOCK_GUARD_GET_CHECK(locks_free_opr_e8,
			&opr_ip->state, LOCK_OPR_IN_FREE_LIST);
		nvmeib_wd_remove_wdc(&opr_ip->wdc);
		ch->num_of_free--;
		list_del_init(&opr_ip->link);
	}
	if (ch->opr_ip_buffer_phys) {
		ib_dma_unmap_page(P2IB(ch->net.port), ch->opr_ip_buffer_phys,
			ch->opr_ip_buffer_size, DMA_BIDIRECTIONAL);
		ch->opr_ip_buffer_phys = 0;
	}
	if (ch->atomic_test_zone_laddr) {
		ib_dma_unmap_single(P2IB(ch->net.port),
				ch->atomic_test_zone_laddr,
				sizeof(*ch->atomic_test_src),
				DMA_BIDIRECTIONAL);
		ch->atomic_test_zone_laddr = 0;
	}

out:
	NFOUT;
}

static void locks_rw_disconnect_ch(struct nvmeibc_locks_channel *ch)
{
	unsigned long flags;
	bool i, d;
	/* Pause channel (if not already paused) */
	nvmeibc_locks_channel_spin_lock_irqsave(ch, flags);
	ch->paused = true;
	nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags);

	if (nvmeibc_channel_is_coremask_ch(&ch->base)) {
		void *coremask_cookie = nvmeibc_channel_get_coremask_ch_cookie(&ch->base);
		struct nvmeibc_locks_channel *primary_ch = get_primary_ch(ch);
		int cpu;

		BUG_ON(is_primary_ch(ch));

		NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, ch->_2nd_ch_coremask_mask) {
			BUG_ON(primary_ch->_2nd_ch_coremask_map[cpu] != ch);
			primary_ch->_2nd_ch_coremask_map[cpu] = NULL;
		}

		if (ch->coremask_ops->put_ref_fn)
			(*ch->coremask_ops->put_ref_fn)(coremask_cookie);

		nvmeibc_channel_set_coremask_ch_cookie(&ch->base, NULL);
	}

	_NT(trace_0_locks_rw_disconnect_ch,
		"Lock-ch: @CH_NAME (@CH_PTR), n_comp_llp={o=@LLU, k=@LLU, t=@LLU}",
		ch->base.name, ch, ch->n_comp_llp_opr, ch->n_comp_llp_ka, ch->n_comp_llp_test);

	/* This func runs only after we've paused ULP and aborted all oprs
	   Thus, we dont expect to find any in-progress or deferred oprs */
	i = !list_empty(&ch->in_progress);
	d = !list_empty(&ch->defered);
	if (i || d) {
		_NW(trace_locks_rw_disconnect_ch,
			"Unexpected, lock-ch: @CH_NAME still have oprs, "
			"in-progress=@BOOL, deferred=@BOOL, abort oprs..",
			ch->base.name, i, d);
	}

	nvmeibc_disk_locks_drain_defered(ch, false, true);

	/* Abort in progress operations */
	nvmeibc_disk_locks_abort_in_progress_oprs(ch, true);

	/* after disk detach switch to DISCONNECTING state for subsequest qp break */
	nvmeibc_ib_net_disconnect(&ch->net);

	/* Break the QP (if not already broken) */
	nvmeibc_ib_net_break_qp(&ch->net);

	/* Free stuff */
	nvmeibc_ib_net_free(&ch->net);

	list_del_init(&ch->link);
	free_dma_resources(ch);
}

static void locks_rw_disconnect_ch_work(struct work_struct *work)
{
	struct nvmeibc_locks_channel *ch = container_of(work, struct nvmeibc_locks_channel, disconnect_work);
	int cnt;
	locks_rw_disconnect_ch(ch);
	if ((cnt = atomic_dec_return(ch->disconnect_cnt)) <= 0) {
		BUG_ON(cnt < 0);
		complete(ch->disconnect_comp);
	}
}

/* Runs on admin WQ */
static void locks_remove_work(struct workqe_struct *work)
{
	struct remove_net_workq *rwork =
		container_of(work, struct remove_net_workq, work);
	struct nvmeibc_ib_net *net =
		container_of(rwork, struct nvmeibc_ib_net, remove_work);
	struct nvmeibc_locks_channel *ch = lock_ch_from_net(net)->primary_ch;
	struct nvmeibc_disk *disk;
	struct nvmeibc_admin_channel *admin_ch = net->admin_ch;
	struct nvmeibc_ib_admin_channel *admin_ib_ch =
		container_of(admin_ch, struct nvmeibc_ib_admin_channel, base);
	struct nvmeibc_ib_net_admin *net_admin = &admin_ib_ch->net;
	//struct nvmeibc_disk_used_lock_segment *uls;
	unsigned long flags;
	int i;
	DECLARE_COMPLETION_ONSTACK(pcpu_disconnect_comp);
	atomic_t pcpu_disconnect_cnt = ATOMIC_INIT(1);
	int cnt;

	NFIN;
	_NT(trace_locks_channel_locks_remove_work, "disconnecting locks channel ch=@CH_PTR, net=@NET", ch, net);
	/* Remove Keep-Alive Periodic Handler (only primary channel) */
	if (ch->periodic_ka.remove_periodic && ch->periodic_ka.ch) {
		ch->periodic_ka.remove_periodic(ch->periodic_ka.ch, &ch->periodic_ka);
		memset(&ch->periodic_ka.ch, 0, sizeof(ch->periodic_ka.ch));
	}

	/* Disconnnect all secondary lock channels */
	if (!ch->_2nd_ch_pcpu || !ch->_2nd_ch_pcpu_lockless) {
		for (i = 0; i < NVMEIB_N_2ND_LOCK_CHS; i++) {
			if (ch->_2nd_ch[i]) {
				struct nvmeibc_locks_channel *_2nd_ch = ch->_2nd_ch[i];
				_NT(trace_1_locks_channel_locks_remove_work, "disconnect 2nd lock-ch=@_2ND_CH, net=@NET", _2nd_ch, &_2nd_ch->net);
				locks_rw_disconnect_ch(_2nd_ch);
			}
		}
	} else {
		/* Secondary channels are per-cpu, need to schedule the disconnect on their cpu */
		for (i = 0; i < NVMEIB_N_2ND_LOCK_CHS; i++) {
			struct nvmeibc_locks_channel *_2nd_ch = ch->_2nd_ch[i];
			if (!_2nd_ch)
				continue;
			/* Schedule disconnect on pcpu cpu
			 * (yes, even if it is this cpu. The wait_for_completion will put this thread to sleep and
			 *  allow the work to run on this cpu) */
			INIT_WORK(&_2nd_ch->disconnect_work, locks_rw_disconnect_ch_work);
			_2nd_ch->disconnect_comp = &pcpu_disconnect_comp;
			_2nd_ch->disconnect_cnt = &pcpu_disconnect_cnt;
			atomic_inc(&pcpu_disconnect_cnt);
			if (!queue_work_on(nvmeibc_channel_pcpu_ch_get_cpu(&_2nd_ch->base), ch->_2nd_ch_pcpu_wq, &_2nd_ch->disconnect_work)) {
				_NE_dmesg(err_locks_channel_locks_remove_work_schedule_work_fail,
						"failed to schedule disconnect_work on CPU @CPU for 2nd lock-ch=@_2ND_CH, net=@NET",
						nvmeibc_channel_pcpu_ch_get_cpu(&_2nd_ch->base), _2nd_ch, &_2nd_ch->net);
				atomic_dec(&pcpu_disconnect_cnt);
				BUG();
			}
		}
	}

	if ((cnt = atomic_dec_return(&pcpu_disconnect_cnt)) > 0) {
		wait_for_completion(&pcpu_disconnect_comp);
	}

	locks_rw_disconnect_ch(ch);

	/* Only needs to be done for primary channel */
	disk = nvmeibc_ib_admin_channel_disk(&net_admin->base);
	BUG_ON(disk == NULL);

	/* Cancel the periodic lock channel work right before we free the channels */
	cancel_delayed_work_sync(&disk->periodic_lock_channel_work);
	nvmeibc_locks_channel_free(ch);

	_ND(trace_3_locks_channel_locks_remove_work, "LOCKS: going to detach...");
	spin_lock_irqsave(&disk->spinlock, flags);
	--disk->set_locks_ref_count;
	_ND(trace_4_locks_channel_locks_remove_work, "set_locks_ref_count dec to @SET_LOCKS_REF_COUNT", disk->set_locks_ref_count);
	BUG_ON(disk->set_locks_ref_count < 0);
	spin_unlock_irqrestore(&disk->spinlock, flags);

	NFOUT;
}

void nvmeibc_locks_channel_disconnect_all_handles_(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_used_lock_segment *uls;

	NFIN;
	list_for_each_entry(uls, &disk->used_lock_segments, link) {
		struct nvmeibc_disk_seg_locks_mem_info *record = uls->mem_info;
		_NT(trace_locks_channel_nvmeibc_locks_channel_disconnect_all_handles, "LOCKS: uls=@ULS, record = @RECORD", uls, record);
		uls->mem_info = NULL;
	}

	NFOUT;
}

void nvmeibc_locks_channel_disconnect_all_handles(struct nvmeibc_disk *disk)
{
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&disk->spinlock, flags);
	nvmeibc_locks_channel_disconnect_all_handles_(disk);
	spin_unlock_irqrestore(&disk->spinlock, flags);


	NFOUT;
}

void nvmeibc_locks_channel_free(struct nvmeibc_locks_channel *ch)
{
	int i;
	NFIN;
	if (ch) {
		if (ch->callback_wq) {
#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
			destroy_workqueue(ch->callback_wq);
#else
			wq_destroy(ch->callback_wq);
#endif
			ch->callback_wq = NULL;
		}

		if (ch->_2nd_ch_pcpu_wq) {
			destroy_workqueue(ch->_2nd_ch_pcpu_wq);
			ch->_2nd_ch_pcpu_wq = NULL;
		}
		for (i = 0; i < NVMEIB_N_2ND_LOCK_CHS; i++) {
			if (ch->_2nd_ch[i]) {
				struct nvmeibc_locks_channel *tmp = ch->_2nd_ch[i];
				ch->_2nd_ch[i] = NULL;
				if (ch->callback_wq) {
#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
					destroy_workqueue(ch->callback_wq);
#else
					wq_destroy(ch->callback_wq);
#endif
					ch->callback_wq = NULL;
				}

				if (tmp->atomic_test_src)
					kfree(tmp->atomic_test_src);

				kfree(tmp->_2nd_net_params);
				kfree(tmp->locks_ip_buffer);
				kfree(tmp);
			}
		}
		kfree(ch->_2nd_net_params);
		if (ch->opr_ip_buffer_page) {
			__free_pages(ch->opr_ip_buffer_page, get_order(ch->opr_ip_buffer_size));
			ch->opr_ip_buffer_page = NULL;
			ch->opr_ip_buffer = NULL;
		}
		kfree(ch->locks_ip_buffer);

		if (ch->atomic_test_src)
			kfree(ch->atomic_test_src);

		kfree(ch->_2nd_ch_pcpu_mask_str);

		free_cpumask_var(ch->_2nd_ch_pcpu_mask);
	}

	kfree(ch);

	NFOUT;
}

/**
 * disconnect callback, called from by ib layer.
 *
 * @author yaron (6/11/2015)
 *
 * @param net
 */
static void on_disconnect_lock_ch(struct nvmeibc_ib_net *net)
{
	struct nvmeibc_locks_channel *ch = lock_ch_from_net(net);
	struct nvmeibc_disk *disk = ch->base.disk;
	enum nvmeibc_disk_release_reason reason = ch->release_reason;
	int dying;

	NFIN;
	if (!(dying = atomic_read(&disk->dying))) {
		_NT(trace_on_disconnect_lock_ch,
			"Lock-ch @CH_NAME (@CH_PTR)",
			ch->base.name, ch);
		nvmeibc_disk_start_release(disk, reason? :
										 NVMEIBC_DISK_RELEASE_LOCK_CHAN_DISCONNECT);
	}
	else {
		/* [AAA] PATCH: in case disk-release is running w/o setting
		   @restart_called, e.g. from update_disk_config_work(),
		   adding another work will cause endless disk-release */
		_NT(on_disconnect_lock_ch_t1,
			"disk @STR (@PTR), already dying (@INT)", disk->name, disk, dying);
	}

	NFOUT;
}

static struct nvmeibc_locks_channel *alloc(
	struct nvmeibc_admin_channel *admin_ch, int numa_node)
{
	int rv = 0;
	struct nvmeibc_locks_channel *ch = NULL;
	size_t opr_ip_buffer_size = sizeof(*ch->opr_ip_buffer) * NVMEIB_LOCK_DATA_BUFFERS * NVMEIBC_CHANNEL_NUM_OF_ALLOC_APR;
	NFIN;
	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch ||
		!(ch->locks_ip_buffer = kzalloc(
				sizeof(*ch->locks_ip_buffer) * NVMEIBC_CHANNEL_NUM_OF_ALLOC_APR, GFP_KERNEL)) ||
			!(ch->opr_ip_buffer_page = alloc_pages_node(numa_node, GFP_KERNEL, get_order(opr_ip_buffer_size))))
	{
		_NE(error_locks_channel_alloc, "failed to allocate memory for locks channel");
		goto out_err;
	}
	ch->opr_ip_buffer = page_address(ch->opr_ip_buffer_page);
	ch->opr_ip_buffer_size = opr_ip_buffer_size;
	ch->total_num_opr = 0;
	nvmeibc_locks_channel_spin_lock_init(ch);
	ch->locking_cpu = -1;
	nvmeibc_lock_ch_metrics_init(&ch->metrics);
	if ((rv = nvmeibc_channel_init(&ch->base, nvmeibc_cinst_get_core_p(&admin_ch->base), NUMA_NO_NODE))) {
		_NE(error_1_locks_channel_alloc, "cannot init base channel");
		goto out_err;
	}

#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
	if (!(ch->callback_wq = alloc_workqueue("lock_cb_wq", WQ_UNBOUND, 0)))
#else
	if (!(ch->callback_wq = wq_create(proc_name_format("C", "WQ", "lock_cb"))))
#endif
	{
		_NE(error_2_locks_channel_alloc, "cannot allocate cb wq");
		goto out_err;
	}

	ch->_2nd_ch_pcpu = nvmeibc_lock_ch_2nd_ch_pcpu;
	ch->_2nd_ch_pcpu_lockless = nvmeibc_lock_ch_2nd_ch_pcpu_lockless;

	if (!admin_ch->base.disk->access_local)
		ch->_2nd_ch_coremask = nvmeibc_lock_ch_2nd_ch_coremask;

	if (ch->_2nd_ch_coremask) {
		if (ch->_2nd_ch_pcpu) {
			_NE(error_locks_channel_alloc_inv_mode,
			    "Invalid mode! Secondary channels cannot be both per-cpu and coremask channels");
			goto out_err;
		}
	}

	if (ch->_2nd_ch_pcpu) {
		if (nvmeibc_lock_ch_pcpu_cpus &&
			!(ch->_2nd_ch_pcpu_mask_str = kstrdup(nvmeibc_lock_ch_pcpu_cpus, GFP_KERNEL))) {
			_NE(error_5_locks_channel_alloc,
			    "OOM. Could not allocate pcpu mask string");
			goto out_err;
		}
		if (!zalloc_cpumask_var(&ch->_2nd_ch_pcpu_mask, GFP_KERNEL)) {
			_NE(error_4_locks_channel_alloc,
			    "OOM. Could not allocate pcpu mask");
			goto out_err;
		}
		if (ch->_2nd_ch_pcpu_lockless &&
			!(ch->_2nd_ch_pcpu_wq = alloc_workqueue("lock_pcpu_wq", 0, 0)))
		{
			_NE(error_3_locks_channel_alloc, "cannot allocate pcpu wq");
			goto out_err;
		}
	}

	INIT_LIST_HEAD(&ch->link);
	/* link lock-ch to disk before trying to connect ch in case
	   on-disconnected callback called before 'connect' returns */
	ch->base.ct = ct_lock;
	ch->base.disk = admin_ch->base.disk;
	goto out;

out_err:
	if (ch) {
		kfree(ch->_2nd_ch_pcpu_mask_str);

		free_cpumask_var(ch->_2nd_ch_pcpu_mask);

		if (ch->_2nd_ch_pcpu_wq)
			destroy_workqueue(ch->_2nd_ch_pcpu_wq);

		if (ch->callback_wq) {
#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
			destroy_workqueue(ch->callback_wq);
#else
			wq_destroy(ch->callback_wq);
#endif
		}
		if (ch->opr_ip_buffer_page) {
			__free_pages(ch->opr_ip_buffer_page, get_order(ch->opr_ip_buffer_size));
			ch->opr_ip_buffer_page = NULL;
		}
		kfree(ch->locks_ip_buffer);
		kfree(ch);
		ch = NULL;
	}

out:
	NFOUT;
	return ch;
}

/*
log the device attributres that are linked to atomic operations
*/
static void log_device_atomic_params(struct ib_device *dev)
{
	struct ib_device_attr attrs;
	NFIN;

	if (nvmeib_query_device(dev, &attrs)) {
		_NE(error_locks_channel_log_device_atomic_params, "LOCKS: cannot log the device locks attributes");
		goto out;
	}

	_ND(trace_locks_channel_log_device_atomic_params, "LOCKS: dev attributes of device @DEV_NAME", dev->name);
	_ND(trace_1_locks_channel_log_device_atomic_params, "LOCKS: atomic_cap @ATOMIC_CAP", attrs.atomic_cap);
	_ND(trace_2_locks_channel_log_device_atomic_params, "LOCKS: max_qp_rd_atom @MAX_QP_RD_ATOM", attrs.max_qp_rd_atom);
	_ND(trace_3_locks_channel_log_device_atomic_params, "LOCKS: max_srq_wr @MAX_SRQ_WR", attrs.max_srq_wr);
	_ND(trace_4_locks_channel_log_device_atomic_params, "LOCKS: max_qp_init_rd_atom @MAX_QP_INIT_RD_ATOM", attrs.max_qp_init_rd_atom);
	_ND(trace_5_locks_channel_log_device_atomic_params, "LOCKS: max_ee_rd_atom @MAX_EE_RD_ATOM", attrs.max_ee_rd_atom);
	_ND(trace_6_locks_channel_log_device_atomic_params, "LOCKS: max_res_rd_atom @MAX_RES_RD_ATOM", attrs.max_res_rd_atom);
	_ND(trace_7_locks_channel_log_device_atomic_params, "LOCKS: max_ee_init_rd_atpm @MAX_EE_INIT_RD_ATOM", attrs.max_ee_init_rd_atom);
	_ND(trace_8_locks_channel_log_device_atomic_params, "LOCKS: masked_atomic_cap @MASKED_ATOMIC_CAP", attrs.masked_atomic_cap);

out:
	NFOUT;
	return;
}

static int try_connect(struct nvmeibc_locks_channel *ch,
	struct nvmeibc_admin_channel *admin_ch, union ib_gid *dgid,
	struct nvmeibc_ib_port *lport, int tgt_atomic_ops,
	unsigned tcp_base_port, unsigned tcp_num_ports);

static int init_2nd_ch(struct nvmeibc_locks_channel *primary_ch,
	int n_idx, u64 cid, int comp_cpu,
	unsigned tcp_base_port, unsigned tcp_num_ports, bool comp_ll);

static void free_2nd_ch(struct nvmeibc_locks_channel *ch);

static int try_connect_2nd_ch(struct nvmeibc_locks_channel *ch);

static void compute_max_2nd_lock_chs(struct nvmeibc_locks_channel *ch,
	struct nvmeibc_admin_channel *admin_ch, struct nvmeibc_ib_port *port)
{
	struct nvmeibc_disk *disk = admin_ch->base.disk;

	if (P2NV(port)->dev_type == DT_siw)
		disk->max_2nd_lock_chs = min3((int)nvmeibc_max_lock_channels_tcp,
						NVMEIB_DFLT_MAX_CPUS,
						disk->tgt_num_cpus) - 1;
	else
		disk->max_2nd_lock_chs = min3((int)nvmeibc_max_lock_channels,
						NVMEIB_DFLT_MAX_CPUS,
						disk->tgt_num_cpus) - 1;

	if (ch->_2nd_ch_pcpu) {
		int max_pcpu_cpus = min_t(int, num_online_cpus(), NVMEIB_DFLT_MAX_CPUS);
		disk->max_2nd_lock_chs = ch->_2nd_ch_pcpu == 1 ?
			max_pcpu_cpus : min_t(int, max_pcpu_cpus, ch->_2nd_ch_pcpu - 1);
	}
}

static struct nvmeibc_locks_channel *try_connect_with_lport(
	struct nvmeibc_admin_channel *admin_ch,
	struct nvmeibc_local_nic_port *lport, union ib_gid *dgid, int max_tgt_atomic_ops,
	enum rdma_link_layer dest_link_layer, enum rdma_transport_type dest_transport_type, int rgid_idx, int lnic_idx,
	unsigned tcp_base_port, unsigned tcp_num_ports)
{
	int rv = -ENODEV;
	struct nvmeibc_locks_channel *locks_channel = NULL;
	struct nvmeibc_disk *disk = admin_ch->base.disk;
	int numa_node = nvmeib_get_dev_numa_node(lport->ib_port->nic_dev->dev);

	NFIN;

	if (lport->ib_port->layer != dest_link_layer) {
		_NT(trace_locks_channel_try_connect_with_dev_layer_miss,
			"Skip, connecting to dest @GID_IPV6 with link_layer @LINK_LAYER from lport @GID_IPV6 with link_layer @LINK_LAYER",
			dgid, dest_link_layer, &lport->ib_port->gid.gid, lport->ib_port->layer);
		goto out;
	}
	if (lport->ib_port->layer != dest_link_layer) {
		_NT(trace_locks_channel_try_connect_with_lport_transport_miss,
			"Skip, connecting to dest @GID_IPV6 with transport_type @TRANSPORT_TYPE from lport @GID_IPV6 with transport_type @TRANSPORT_TYPE",
			dgid, dest_transport_type, &lport->ib_port->gid.gid, lport->ib_port->transport_type);
		goto out;
	}
	if (disk->access_local &&
		memcmp(&lport->ib_port->gid.gid, dgid, sizeof(*dgid))) {
		_NT(trace_0_locks_channel_try_connect_with_lport,
			"Skip, loopback but l=@GID_IPV6 vs r=@GID_IPV6",
			&lport->ib_port->gid.gid, dgid);
		goto out;
	}

	if ((locks_channel = alloc(admin_ch, numa_node)) == NULL) {
		goto out;
	}
	compute_max_2nd_lock_chs(locks_channel, admin_ch, lport->ib_port);

	rv = try_connect(
		locks_channel, admin_ch, dgid, lport->ib_port,
		max_tgt_atomic_ops, tcp_base_port, tcp_num_ports);
	if (rv) {
		_NT(trace_locks_channel_try_connect_with_dev, "Could not connect lock channel");
		nvmeibc_locks_channel_free(locks_channel);
		locks_channel = NULL;
	}

out:
	NFOUT;
	return locks_channel;
}

static struct nvmeibc_locks_channel *try_connect_with_dev(
	struct nvmeibc_admin_channel *admin_ch,
	struct nvmeibc_local_nic *ln, union ib_gid *dgid, int max_tgt_atomic_ops,
	enum rdma_link_layer dest_link_layer, enum rdma_transport_type dest_transport_type, int rgid_idx, int lnic_idx,
	unsigned tcp_base_port, unsigned tcp_num_ports)
{
	struct nvmeibc_local_nic_port *lport;
	int rv = -ENODEV;
	struct nvmeibc_locks_channel *locks_channel = NULL;
	int i = 0;

	NFIN;
	BUG_ON(admin_ch == NULL);

	/* try to check if user nics is local using the local ports of the local nic */
	list_for_each_entry(lport, &ln->ports, link) {
		_NT(trace_locks_channel_try_connect_with_dev_base,
			"[@INT32_02][@INT32_02][@INT32_02] Try connect l=@GID_IPV6 --> r=@GID_IPV6",
			rgid_idx, lnic_idx, i, &lport->ib_port->gid.gid, dgid);
		if ((locks_channel = try_connect_with_lport(admin_ch, lport, dgid, max_tgt_atomic_ops, dest_link_layer, dest_transport_type, rgid_idx, lnic_idx, tcp_base_port, tcp_num_ports))) {
			_NT(connect_lchannel_done, "Managed to connect lock channel!");
			break;
		}
	}

	if (rv == -ENODEV) {
		_NT(trace_locks_channel_try_connect_with_dev_no_ports, "No valid local ports found");
	}

	NFOUT;
	return locks_channel;
}

/* Lock KA has no WD - dont post more than one w/o comp */
bool nvmeibc_locks_channel_is_ka_timeout(struct nvmeibc_locks_channel *ch)
{
	u64 ka_from_sent;
	bool rv = false;

	if (ch->ka_post_cnt && ch->ka_post_cnt != ch->n_comp_llp_ka) {
		ka_from_sent = jiffies - ch->ka_post_jif;
		if (ka_from_sent > ch->net.wd_timeout_jif) {
			_NW(trace_is_ka_timeout,
				"Lock-ch @CH_NAME (@CH_PTR), No KA comp"
				"(n_post=@LLU, n_comp=@LLU, comp_jif=@LLU,"
				"from_sent=@LLU(@LLU)), skip (@LLU)..."
				"n_comp_llp={o=@LLU, k=@LLU, t=@LLU}\n",
				ch->base.name, ch,
				ch->ka_post_cnt, ch->n_comp_llp_ka, ch->ka_comp_jif,
				ka_from_sent, ka_from_sent / HZ, ch->ka_skip_use,
				ch->n_comp_llp_opr, ch->n_comp_llp_ka, ch->n_comp_llp_test);
			ch->ka_skip_use++;
			/* we dont want to trigger net-disconnect as it acquires the
			   net-lock, but this func might be called from context that
			   already acquired net-lock of another net e.g. send-comp of
			   another lock-channel, causing reversed-order dead-lock */
			rv = true;
		}
	}

	return rv;
}

static int __lock_ch_periodic_ka_fn(void *arg, unsigned long t)
{
	struct nvmeibc_locks_channel *ch = arg;
	struct nvmeib_send_wr rdma_wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	struct ib_sge sge = {0};
	u64 write_val = 0x1234123412341234;
	int rv = -1;

	if (nvmeibc_locks_channel_is_ka_timeout(ch))
		goto out;

	/* First we RDMA WRITE our compare value */
	*ch->atomic_test_src = write_val;

	sge.addr = ch->atomic_test_zone_laddr;
	sge.length = sizeof(*ch->atomic_test_src);
	sge.lkey = ch->atomic_test_zone_lkey;

	memset(&rdma_wr, 0, sizeof( rdma_wr ));
	nvmeib_send_wr_common( rdma_wr ).wr_id = nvmeib_encode_wr_id(NVMEIB_LOCK_KA, 0);
	nvmeib_send_wr_common( rdma_wr ).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_rdma( rdma_wr ).rkey = ch->atomic_test_zone_rkey;
	nvmeib_send_wr_rdma( rdma_wr ).remote_addr = ch->atomic_test_zone_raddr;
	nvmeib_send_wr_common( rdma_wr ).send_flags = IB_SEND_SIGNALED;
	nvmeib_send_wr_common( rdma_wr ).sg_list = &sge;
	nvmeib_send_wr_common( rdma_wr ).num_sge = 1;

#if ENABLE_SIW
	if (P2NV(ch->net.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(rdma_wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

	if ((rv = nvmeibc_ib_post_send(&ch->net, nvmeib_send_wr_to_ib_ptr( rdma_wr ), &bad_wr))) {
		_NE(error_locks_channel_lock_ch_periodic_ka_fn, "ib-post_send failed (@RV)", rv);
	} else {
		ch->ka_post_cnt++;
		ch->ka_post_jif = jiffies;
		_ND(trace_locks_channel_lock_ch_periodic_ka_fn, "Locks Channel to @RHOST_NAME Keep-Alive", ch->base.rhost_name);
	}

out:
	return rv;
}

static int lock_ch_periodic_ka_fn(void *arg, unsigned long t)
{
	struct nvmeibc_locks_channel *primary_ch = arg;
	int i;

	if (__lock_ch_periodic_ka_fn(primary_ch, t))
		goto out;

	if (DEBUG_2ND_LOCK_CH_KA) {
		for (i = 0; i < primary_ch->n_2nd_ch; i++) {
			struct nvmeibc_locks_channel *_2nd_ch = primary_ch->_2nd_ch[i];
			if (__lock_ch_periodic_ka_fn(_2nd_ch, t))
				goto out;
		}
	}

out:
	return 0;
}

static void try_connect_2nd_ch_work(struct work_struct *work)
{
	struct nvmeibc_locks_channel *ch = container_of(work, struct nvmeibc_locks_channel, disconnect_work);
	int cnt;
	ch->conn_rv = try_connect_2nd_ch(ch);
	if ((cnt = atomic_dec_return(ch->disconnect_cnt)) <= 0) {
		BUG_ON(cnt < 0);
		complete(ch->disconnect_comp);
	}
}

static struct nvmeibc_locks_channel *connect_2nd_lock_chs(struct nvmeibc_locks_channel *ch,
	struct nvmeibc_admin_channel *admin_ch, union ib_gid *gid, unsigned tcp_base_port, unsigned tcp_num_ports)
{
	DECLARE_COMPLETION_ONSTACK(pcpu_connect_comp);
	atomic_t pcpu_connect_cnt = ATOMIC_INIT(1);
	int i = 0;
	int rv = 0, _2nd_ch_cpu, cnt;

	_ND(trace_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: Was able to connect with this dev"
		" adding channel @CH_PTR", ch);
	++admin_ch->base.disk->set_locks_ref_count;
	/* Add periodic handler to test locks channel is still alive */
	ch->periodic_ka.on_periodic_arg = ch;
	ch->periodic_ka.on_periodic = lock_ch_periodic_ka_fn;
	nvmeibc_admin_channel_add_periodic(admin_ch, &ch->periodic_ka);
	_ND(trace_1_locks_channel_nvmeibc_locks_channel_connect_locks, "set_locks_ref_count increased to @SET_LOCKS_REF_COUNT",
		admin_ch->base.disk->set_locks_ref_count);

	if (admin_ch->base.disk->rpc_locks ||
			test_bit(NVMEIBC_LOCK_CMP_AND_SWAP, ch->local_bypass_bmp)) {
		/* Don't create secondary lock channels when doing RPC locks */
		goto out;
	}

	if (ch->_2nd_ch_coremask) {
		admin_ch->base.disk->max_2nd_lock_chs = min_t(int, num_online_cpus(), NVMEIB_DFLT_MAX_CPUS);
		_NT(trace_connect_2nd_lock_chs, "Not connecting secondary channels in coremask mode."
						"Will be connected on demand");
		goto out;
	}

	/* Now try and connect secondary lock channels */
	if (P2NV(ch->net.port)->dev_type == DT_siw) {
		ch->method = nvmeibc_lock_channel_choosing_method_tcp;
	} else {
		ch->method = nvmeibc_lock_channel_choosing_method;
	}

	if (ch->_2nd_ch_pcpu) {
		if (ch->_2nd_ch_pcpu_mask_str) {
			if (cpumask_parse(ch->_2nd_ch_pcpu_mask_str, ch->_2nd_ch_pcpu_mask)) {
				_NE_dmesg(error_connect_locks_inv_pcpu_mask, "Invalid cpumask for nr_pcpu_ch_ll_cpus: @MASK_STRING",
					  ch->_2nd_ch_pcpu_mask_str);
				ch->_2nd_ch_pcpu = 0;
			} else if (cpumask_empty(ch->_2nd_ch_pcpu_mask)) {
				_NE_dmesg(error_1_connect_locks_inv_pcpu_mask, "Empty cpumask nr_pcpu_ch_ll_cpus: @MASK_STRING",
					  ch->_2nd_ch_pcpu_mask_str);
				ch->_2nd_ch_pcpu = 0;
			} else if (cpumask_weight(ch->_2nd_ch_pcpu_mask) > admin_ch->base.disk->max_2nd_lock_chs) {
				_NE_dmesg(error_2_connect_locks_inv_pcpu_mask, "Too many cpus in nr_pcpu_ch_ll_cpus: @MASK_STRING (max @MAX_CH)",
					  ch->_2nd_ch_pcpu_mask_str, admin_ch->base.disk->max_2nd_lock_chs);
				ch->_2nd_ch_pcpu = 0;
			} else if (NVMEIB_DFLT_MAX_CPUS < nr_cpu_ids &&
				cpumask_next(NVMEIB_DFLT_MAX_CPUS - 1, ch->_2nd_ch_pcpu_mask) < nr_cpu_ids) {
				_NE_dmesg(error_3_connect_locks_inv_pcpu_mask, "CPUs in nr_pcpu_ch_ll_cpus: @MASK_STRING are > NVMesh max (@MAX_CPU)",
					  ch->_2nd_ch_pcpu_mask_str, NVMEIB_DFLT_MAX_CPUS);
				ch->_2nd_ch_pcpu = 0;
			}
		} else {
			int cpu, n_cpus = 0;
			for_each_online_cpu(cpu) {
				cpumask_set_cpu(cpu, ch->_2nd_ch_pcpu_mask);
				n_cpus++;
				if (n_cpus == admin_ch->base.disk->max_2nd_lock_chs)
					break;
			}
		}
		if (ch->_2nd_ch_pcpu) {
			char ll_mask_string[128] = "";
			scnprintf(ll_mask_string, sizeof(ll_mask_string), "%*pbl",
				  nr_cpu_ids, cpumask_bits(ch->_2nd_ch_pcpu_mask));
			_NT(trace_9_locks_channel_nvmeibc_locks_channel_connect_locks,
			    "LOCKS: Using @NUM_CH Secondary Channels as per-cpu channels (lockless @BOOL_YN) on CPUs @CPU_LIST_STR",
				cpumask_weight(ch->_2nd_ch_pcpu_mask), ch->_2nd_ch_pcpu_lockless, ll_mask_string);
		}
	}

	_2nd_ch_cpu = NVMEIB_CPU_INVALID;
	for (i = 0; i < admin_ch->base.disk->max_2nd_lock_chs; i++) {
		if (ch->_2nd_ch_pcpu) {
			/* Get next online cpu to assign to secondary channel */
			_2nd_ch_cpu = cpumask_next(_2nd_ch_cpu, ch->_2nd_ch_pcpu_mask);
			if (_2nd_ch_cpu >= nr_cpu_ids) {
				/* All cpus assigned a channel */
				break;
			}
		}
		if ((rv = init_2nd_ch(ch, i, admin_ch->cid, _2nd_ch_cpu, tcp_base_port, tcp_num_ports, ch->_2nd_ch_pcpu_lockless)) < 0) {
			_NT(trace_12_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: Failed (@RV) to initialise Secondary Channel @CHANNEL from Device @IB_DEV_NAME: @PORT", rv, i, P2IB(ch->net.port)->name, ch->net.port->port);
			break;
		}
		else {
			struct nvmeibc_locks_channel *_2nd_ch = ch->_2nd_ch[i];
			if (_2nd_ch_cpu == NVMEIB_CPU_INVALID || !ch->_2nd_ch_pcpu_lockless) {
				rv = try_connect_2nd_ch(_2nd_ch);
				if (!rv) {
					_ND(trace_4_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: Connected Secondary Channel @CHANNEL from Device @IB_DEV_NAME: @PORT", i,
					    P2IB(ch->_2nd_ch[i]->net.port)->name, ch->_2nd_ch[i]->net.port->port);
					ch->n_2nd_ch++;
				} else {
					_NT(trace_5_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: Failed (@RV) to connect Secondary Channel @CHANNEL from Device @IB_DEV_NAME: @PORT", rv, i,
					    P2IB(ch->net.port)->name, ch->net.port->port);
					free_2nd_ch(_2nd_ch);
					ch->_2nd_ch[i] = NULL;
					break;
				}
			}
			else {
				/* Schedule connect on pcpu cpu
				 * (yes, even if it is this cpu. The wait_for_completion will put this thread to sleep and
				 *  allow the work to run on this cpu) */
				INIT_WORK(&_2nd_ch->disconnect_work, try_connect_2nd_ch_work);
				_2nd_ch->disconnect_comp = &pcpu_connect_comp;
				_2nd_ch->disconnect_cnt = &pcpu_connect_cnt;
				atomic_inc(&pcpu_connect_cnt);

				if (!queue_work_on(_2nd_ch_cpu, ch->_2nd_ch_pcpu_wq, &_2nd_ch->disconnect_work)) {
					_NE_dmesg(trace_6_locks_channel_nvmeibc_locks_channel_connect_locks,
						"failed to schedule try_connect_2nd_ch_work on CPU @CPU for 2nd lock-ch=@_2ND_CH, net=@NET",
						_2nd_ch_cpu, _2nd_ch, &_2nd_ch->net);
					atomic_dec(&pcpu_connect_cnt);
					BUG();
				}
			}
		}
	}

	if (ch->_2nd_ch_pcpu) {
		char ll_mask_string[128] = "";
		if ((cnt = atomic_dec_return(&pcpu_connect_cnt)) > 0) {
			wait_for_completion(&pcpu_connect_comp);
		}
		for (i = 0; i < admin_ch->base.disk->max_2nd_lock_chs; i++) {
			struct nvmeibc_locks_channel *_2nd_ch = ch->_2nd_ch[i];
			if (!_2nd_ch) {
				/* Don't break because there might be other channels that failed and need to be freed */
				_NT(trace_13_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: NULL Secondary per-cpu [@CPU] Channel @CHANNEL of primary (@CH_PTR) from Device @IB_DEV_NAME: @PORT", _2nd_ch_cpu, i, ch, P2IB(ch->net.port)->name, ch->net.port->port);
				continue;
			}
			_2nd_ch_cpu = nvmeibc_channel_pcpu_ch_get_cpu(&_2nd_ch->base);
			BUG_ON(!cpumask_test_cpu(_2nd_ch_cpu, ch->_2nd_ch_pcpu_mask));
			if (!_2nd_ch->conn_rv) {
				ch->n_2nd_ch++;
				ch->_2nd_ch_pcpu_map[_2nd_ch_cpu] = ch->_2nd_ch[i];

				_NT(trace_7_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: Connected Secondary per-cpu [@CPU] Channel @CHANNEL (@CH_PTR) from Device @IB_DEV_NAME: @PORT", _2nd_ch_cpu, i, _2nd_ch, P2IB(ch->_2nd_ch[i]->net.port)->name, ch->_2nd_ch[i]->net.port->port);
			} else {
				_NT(trace_8_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: Failed (@RV) to connect Secondary per-cpu [@CPU] Channel @CHANNEL (@CH_PTR) from Device @IB_DEV_NAME: @PORT", _2nd_ch->conn_rv, _2nd_ch_cpu, i, _2nd_ch, P2IB(ch->net.port)->name, ch->net.port->port);
				cpumask_clear_cpu(_2nd_ch_cpu, ch->_2nd_ch_pcpu_mask);
				free_2nd_ch(_2nd_ch);
				ch->_2nd_ch[i] = NULL;

				/* Don't break because there might be other channels that failed and need to be freed */
			}
		}

		scnprintf(ll_mask_string, sizeof(ll_mask_string), "%*pbl",
			  nr_cpu_ids, cpumask_bits(ch->_2nd_ch_pcpu_mask));
		_NT(trace_10_locks_channel_nvmeibc_locks_channel_connect_locks,
		    "LOCKS: Connected @NUM_CH Secondary Channels as lockless per-cpu channels on CPUs @CPU_LIST_STR",
			cpumask_weight(ch->_2nd_ch_pcpu_mask), ll_mask_string);
	}

out:
	NFOUT;
	return ch;
}

struct nvmeibc_locks_channel *nvmeibc_locks_channel_connect_locks(
	struct nvmeibc_admin_channel *admin_ch, union ib_gid *gid, int max_tgt_atomic_ops, enum rdma_link_layer dest_link_layer, enum rdma_transport_type dest_transport_type, int rgid_idx,
	unsigned tcp_base_port, unsigned tcp_num_ports)
{
	struct nvmeibc_locks_channel *ch = NULL;
	struct nvmeibc_local_nic *ln;
	int i;

	NFIN;
	i = 0;

	list_for_each_entry(ln, &admin_ch->base.disk->local_nics, link) {
		if ((ch = try_connect_with_dev(
			admin_ch, ln, gid, max_tgt_atomic_ops,
			dest_link_layer, dest_transport_type, rgid_idx, i, tcp_base_port, tcp_num_ports)) != NULL)
		{
			break;
		}
		else {
			_NT(trace_2_locks_channel_nvmeibc_locks_channel_connect_locks, "LOCKS: Failed to connect with this dev"
				" adding channel @CH_PTR", ch);
		}
		i++;
	}

	if (!ch) {
		_NT(trace_3_locks_channel_nvmeibc_locks_channel_connect_locks, "could not find any port to connect to target gid @GID_IPV6", gid);
		goto out;
	}
	ch = connect_2nd_lock_chs(ch, admin_ch, gid, tcp_base_port, tcp_num_ports);

out:
	NFOUT;
	return ch;
}

struct nvmeibc_locks_channel *nvmeibc_locks_channel_connect_lock_by_path(
	struct nvmeibc_admin_channel *admin_ch, struct nvmeibc_local_nic_port *lport, union ib_gid *gid, int max_tgt_atomic_ops,
	enum rdma_link_layer dest_link_layer, enum rdma_transport_type dest_transport_type, int rgid_idx,
	unsigned tcp_base_port, unsigned tcp_num_ports)
{
	struct nvmeibc_locks_channel *ch = NULL;

	NFIN;

	if ((ch = try_connect_with_lport(
			admin_ch, lport, gid, max_tgt_atomic_ops,
			dest_link_layer, dest_transport_type, rgid_idx, 0, tcp_base_port, tcp_num_ports)) == NULL) {
		_NT(nvmeibc_locks_channel_connect_lock_by_path_failed, "could not find any port to connect to target gid @GID_IPV6", gid);
		goto out;
	}

	ch = connect_2nd_lock_chs(ch, admin_ch, gid, tcp_base_port, tcp_num_ports);

out:
	NFOUT;
	return ch;
}


bool nvmeibc_locks_channel_all_2nd_connected(struct nvmeibc_locks_channel *ch)
{
	int max_2nd_lock_chs = ch->_2nd_ch_pcpu ? cpumask_weight(ch->_2nd_ch_pcpu_mask) :
						ch->base.disk->max_2nd_lock_chs;

	/* Don't wait for secondary channels in coremask mode, they will be connected on demand */
	if (ch->_2nd_ch_coremask)
		return true;
	/* Don't create secondary channels if we are bypassing all ops */
	if (ch->atomic_cap == IB_ATOMIC_NONE ||
			test_bit(NVMEIBC_LOCK_CMP_AND_SWAP, ch->local_bypass_bmp))
		return true;

	if (ch->n_2nd_ch == max_2nd_lock_chs) {
		return true;
	} else {
		_NT(trace_locks_channel_nvmeibc_locks_channel_all_2nd_connected,
			"Not all (@INT of @INT) secondary connected (@CH_PTR)",
		    ch->n_2nd_ch, max_2nd_lock_chs, ch);
		return false;
	}
}

static int lock_send_completion(struct nvmeibc_ib_net *net,
				struct ib_wc *wc, bool last_wc_in_series)
{
	int rv = 0;
	struct nvmeibc_locks_channel *ch = lock_ch_from_net(net);
	u32 wr_opcode = nvmeib_opcode_from_wc(wc);
	if (likely(wr_opcode == NVMEIB_DISK_LOCK_OPR)) {
		ch->n_comp_llp_opr++;
		nvmesh_metric_update(ch->metrics.opr.count, 1);
		rv = nvmeibc_disk_locks_on_completion(ch, net, wc, last_wc_in_series);
		goto out;
	} else if (wr_opcode == NVMEIB_ATOMIC_TEST) {
		ch->n_comp_llp_test++;
		ch->atomic_test_wc_status = wc->status;
		complete(&ch->atomic_test_comp);
	} else if (wr_opcode == NVMEIB_LOCK_KA) {
		ch->n_comp_llp_ka++;
		ch->ka_comp_jif = jiffies;
		_ND(trace_locks_channel_lock_send_completion, "Lock channel to @RHOST_NAME Keep-Alive Complete", ch->base.rhost_name);
		if (last_wc_in_series) {
			//yup, ugly! but follows nvmeibc_disk_locks_on_completion()
			//which processes deferred only on last wc - why? ask YK!
			nvmeibc_disk_locks_process_deferred(ch);
		}
	} else {
		_NT(trace_1_locks_channel_lock_send_completion, "Unexpected wr_opcode @WR_OPCODE", wr_opcode);
		rv = -EINVAL;
	}

out:
	return rv;
}

/* To test the atomics, we first RDMA_WRITE a value, then do an CMP_AND_SWAP and then an RDMA_READ */
/* The aim is to determine whether atomics are supported and also whether there is any endianness swapping */
static int test_atomics(struct nvmeibc_locks_channel *ch, bool masked, bool *req_endian_swap, bool *reply_endian_swap)
{
	struct nvmeib_send_wr rdma_wr, atomic_wr, *bad_atomic_wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	struct ib_sge sge = {0};
	u64 write_val;
	u64 atomic_cmp_val = 0x1234123412341234;
	u64 atomic_val_masked_out = 0xdeadbeefdeadbeef;
	u64 atomic_cmp_mask = masked ? ~(u32)0 : ~(u64)0;
	u64 atomic_swap_val = 0x7890789078907890;
	u64 atomic_swap_mask = masked ? ~(u32)0 : ~(u64)0;
	u64 atomic_swap_ret;
	u64 read_result;
	int rv = 0, wait_rv;

	/* Start with the assumption that there is no endianness-swapping needed */
	*req_endian_swap = false;
	*reply_endian_swap = false;

retry_op:
	/* First we RDMA WRITE our compare value */
	write_val = (atomic_cmp_val & atomic_cmp_mask) | (atomic_val_masked_out & ~atomic_cmp_mask);
	*ch->atomic_test_src = write_val;

	sge.addr = ch->atomic_test_zone_laddr;
	sge.length = sizeof(*ch->atomic_test_src);
	sge.lkey = ch->atomic_test_zone_lkey;

	memset(&rdma_wr, 0, sizeof( rdma_wr ));
	nvmeib_send_wr_common( rdma_wr ).wr_id = nvmeib_encode_wr_id(NVMEIB_ATOMIC_TEST, 0);
	nvmeib_send_wr_common( rdma_wr ).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_rdma( rdma_wr ).rkey = ch->atomic_test_zone_rkey;
	nvmeib_send_wr_rdma( rdma_wr ).remote_addr = ch->atomic_test_zone_raddr;
	nvmeib_send_wr_common( rdma_wr ).send_flags = IB_SEND_SIGNALED;
	nvmeib_send_wr_common( rdma_wr ).sg_list = &sge;
	nvmeib_send_wr_common( rdma_wr ).num_sge = 1;

	nvmeib_reinit_completion(&ch->atomic_test_comp);
	if ((rv = nvmeibc_ib_post_send(&ch->net, nvmeib_send_wr_to_ib_ptr( rdma_wr ), &bad_wr))) {
		_NE(error_locks_channel_test_atomics, "ib-post_send failed (@RV)", rv);
		goto out;
	}

	wait_rv = 2 * HZ;
	if ((wait_rv = wait_for_completion_timeout(&ch->atomic_test_comp, wait_rv /* ch->net.wd_timeout_jif*/)) <= 0) {
		_NE(error_1_locks_channel_test_atomics, "Atomic test timeout, @CH_NAME (@CH_PTR)", ch->base.name, ch);
		rv = -ETIMEDOUT;
		goto out;
	}

	if (ch->atomic_test_wc_status != IB_WC_SUCCESS) {
		_NE(error_2_locks_channel_test_atomics, "RDMA Write Failed (@ATOMIC_TEST_WC_STATUS)", ch->atomic_test_wc_status);
		rv = -EIO;
		goto out;
	}

	/* Now we CMP_AND_SWAP it */
	memset(&atomic_wr, 0, sizeof(atomic_wr));
	nvmeib_send_wr_common(atomic_wr).wr_id = nvmeib_encode_wr_id(NVMEIB_ATOMIC_TEST, 0);
	nvmeib_send_wr_common(atomic_wr).opcode = masked ? IB_WR_MASKED_ATOMIC_CMP_AND_SWP : IB_WR_ATOMIC_CMP_AND_SWP;
	nvmeib_send_wr_atomic(atomic_wr).rkey = ch->atomic_test_zone_rkey;
	nvmeib_send_wr_atomic(atomic_wr).remote_addr = ch->atomic_test_zone_raddr;
	nvmeib_send_wr_atomic(atomic_wr).compare_add = !ch->atomic_req_endian_swap ? atomic_cmp_val : __swab64(atomic_cmp_val);
	nvmeib_send_wr_atomic(atomic_wr).compare_add_mask = !ch->atomic_req_endian_swap ? atomic_cmp_mask : __swab64(atomic_cmp_mask);
	nvmeib_send_wr_atomic(atomic_wr).swap = !ch->atomic_req_endian_swap ? atomic_swap_val : __swab64(atomic_swap_val);
	nvmeib_send_wr_atomic(atomic_wr).swap_mask = !ch->atomic_req_endian_swap ? atomic_swap_mask : __swab64(atomic_swap_mask);
	nvmeib_send_wr_common(atomic_wr).send_flags = IB_SEND_SIGNALED;
	nvmeib_send_wr_common(atomic_wr).sg_list = &sge;
	nvmeib_send_wr_common(atomic_wr).num_sge = 1;

	nvmeib_reinit_completion(&ch->atomic_test_comp);
	if ((rv = (*ch->net.post_send_atomic_fn)(ch->net.qp, &atomic_wr, &bad_atomic_wr))) {
		_NE(error_3_locks_channel_test_atomics, "ib-post_send failed (@RV)", rv);
		goto out;
	}

	if ((wait_rv = wait_for_completion_timeout(&ch->atomic_test_comp, wait_rv /* ch->net.wd_timeout_jif*/)) <= 0) {
		_NE(error_4_locks_channel_test_atomics, "Atomic test timeout, @CH_NAME (@CH_PTR)", ch->base.name, ch);
		rv = -ETIMEDOUT;
		goto out;
	}

	if (ch->atomic_test_wc_status != IB_WC_SUCCESS) {
		_NE(error_5_locks_channel_test_atomics, "CMP_AND_SWAP Failed (@ATOMIC_TEST_WC_STATUS)", ch->atomic_test_wc_status);
		rv = -EIO;
		goto out;
	}

	atomic_swap_ret = *ch->atomic_test_src;
	if ((atomic_swap_ret & atomic_cmp_mask) != (atomic_cmp_val & atomic_cmp_mask)) {
		atomic_swap_ret = __swab64(*ch->atomic_test_src);
		if ((atomic_swap_ret & atomic_cmp_mask) != (atomic_cmp_val & atomic_cmp_mask)) {
			_NT(trace_locks_channel_test_atomics, "LOCKS: Atomic Replies with Endianness swap");
			ch->atomic_reply_endian_swap = true;
		} else {
			_NT(trace_1_locks_channel_test_atomics, "LOCKS: Unexpected Atomic Return Value @ATOMIC_TEST_SRC (mask @ATOMIC_CMP_MASK)",
			   *ch->atomic_test_src, atomic_cmp_mask);
			rv = -EFAULT;
			goto out;
		}
	}

	/* And now RDMA read */
	nvmeib_send_wr_common( rdma_wr ).opcode = IB_WR_RDMA_READ;

	nvmeib_reinit_completion(&ch->atomic_test_comp);
	if ((rv = nvmeibc_ib_post_send(&ch->net, nvmeib_send_wr_to_ib_ptr(rdma_wr), &bad_wr))) {
		_NE(error_6_locks_channel_test_atomics, "ib-post_send failed (@RV)", rv);
		goto out;
	}

	if ((wait_rv = wait_for_completion_timeout(&ch->atomic_test_comp, wait_rv /* ch->net.wd_timeout_jif*/)) <= 0) {
		_NE(error_7_locks_channel_test_atomics, "Atomic test timeout, @CH_NAME (@CH_PTR)", ch->base.name, ch);
		rv = -ETIMEDOUT;
		goto out;
	}

	if (ch->atomic_test_wc_status != IB_WC_SUCCESS) {
		_NE(error_8_locks_channel_test_atomics, "RDMA Read Failed (@ATOMIC_TEST_WC_STATUS)", ch->atomic_test_wc_status);
		rv = -EIO;
		goto out;
	}

	read_result = (*reply_endian_swap) ? __swab64(*ch->atomic_test_src) : (*ch->atomic_test_src);
	if ((read_result & atomic_swap_mask) == (atomic_swap_val & atomic_swap_mask)) {
		_NT(trace_2_locks_channel_test_atomics, "LOCKS: Successfull @OP_STR @WRITE_VAL (cmp_val @ATOMIC_CMP_VAL cmp_mask @ATOMIC_CMP_MASK) -> @READ_RESULT (swap_val @ATOMIC_SWAP_VAL swap_mask @ATOMIC_SWAP_MASK)."
		" Request Endianness Swap: @TRUE_FALSE_STR, Reply Endianness Swap: @TRUE_FALSE_STR",
			masked ? "IB_WR_MASKED_ATOMIC_CMP_AND_SWP" : "IB_WR_ATOMIC_CMP_AND_SWP",
			write_val, atomic_cmp_val, atomic_cmp_mask, read_result, atomic_swap_val, atomic_swap_mask,
			*req_endian_swap ? "true" : "false",
			*reply_endian_swap ? "true" : "false");
		goto out;
	} else if (!(*req_endian_swap)) {
		/* Try swapping the endianness of the request */
		(*req_endian_swap) = true;
		goto retry_op;
	} else {
		if (masked)
			_NT(trace_3_locks_channel_test_atomics, "LOCKS: IB_WR_MASKED_ATOMIC_CMP_AND_SWP Failed");
		else
			_NT(trace_4_locks_channel_test_atomics, "LOCKS: IB_WR_ATOMIC_CMP_AND_SWP Failed");
		rv = -ENOTSUPP;
	}

out:
	return rv;
}

static void free_premature_lock_ch(struct nvmeibc_locks_channel *ch)
{
	unsigned long flags = -1;
	bool already_locked;
	struct nvmeibc_ib_net *net;
	int dying;

	NFIN;
	_NT(trace_locks_channel_free_premature_lock_ch, "Disconnect and free ib and cm");
	net = &ch->net;
	already_locked = nvmeibc_channel_already_locked(&ch->base);
	if (!already_locked)
		nvmeibc_channel_spin_lock_irqsave(&ch->base, &flags);
	dying = atomic_inc_return(&net->dying);
	if (dying > 1) {
		_NT(trace_0_locks_channel_free_premature_lock_ch,
			"net=@NET, dying=@INT", net, dying);
		WARN_ON_ONCE(net->on_disconnect);
		//net-disconnect was already called but shouldn't have been able to call
		//on_disconnect_lock_ch as we only set cb-ptr iff not calling this func.
		//Note:
		//1. if net->on_disconnect is called during discover, disk-release work
		//   is not added but we will restart disk-release after this discover
		//2. if this func caller(s) return error stop_lock_channels() will not
		//   disconnect/free lock-chs as ch->base.segments_locks_remote.lock_ch
		//   wil still be NULL
	}
	nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_DISCONNECTING);
	if (!already_locked)
		nvmeibc_channel_spin_unlock_irqrestore(&ch->base, flags);
	nvmeibc_ib_net_break_qp(net);
	nvmeibc_ib_net_free(net);
	free_dma_resources(ch);
	if (ch->atomic_test_src) {
		kfree(ch->atomic_test_src);
		ch->atomic_test_src = NULL;
	}
	NFOUT;
}

static int resolve_ch_atomic_caps_and_endianness(struct nvmeibc_locks_channel *ch)
{
	bool l_atomic_cap, l_masked_atomic_cap;
	bool r_atomic_cap, r_masked_atomic_cap;
	int rv;
	struct nvmeibc_disk *disk = ch->base.disk;
	NFIN;

	/* local caps */
	l_atomic_cap = nvmeib_device_sup_cap(
		P2NV(ch->net.port)->dev_type, NVMEIB_DEVCAP_ATOMICS_REQ);
	l_masked_atomic_cap = nvmeib_device_sup_cap(
		P2NV(ch->net.port)->dev_type, NVMEIB_DEVCAP_MASKED_ATOMICS_REQ);

	/* remote caps */
	r_atomic_cap = ch->tgt_atomic_cap != IB_ATOMIC_NONE;
	r_masked_atomic_cap = ch->tgt_masked_atomic_cap != IB_ATOMIC_NONE;

	/* joint caps */
	ch->atomic_cap = l_atomic_cap && r_atomic_cap;
	ch->masked_atomic_cap = ch->atomic_cap &&
		l_masked_atomic_cap && r_masked_atomic_cap;

	if (ch->atomic_cap != IB_ATOMIC_NONE) {
		init_completion(&ch->atomic_test_comp);
		if ((rv = test_atomics(ch, false,
							   &ch->atomic_req_endian_swap,
							   &ch->atomic_reply_endian_swap))) {
			_NE(error_locks_channel_resolve_ch_atomic_caps_and_endianness, "Failed atomics test (@RV)", rv);
			DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_TEST_ATOMICS_FAILED);
			goto out;
		}
		if (ch->masked_atomic_cap &&
			((rv = test_atomics(ch, true,
								&ch->masked_atomic_req_endian_swap,
								&ch->masked_atomic_reply_endian_swap)))) {
			_NE(error_1_locks_channel_resolve_ch_atomic_caps_and_endianness, "Failed masked atomics test (@RV)", rv);
			DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_TEST_ATOMICS_FAILED);
			goto out;
		}
		ch->base.disk->rpc_locks = false;
		if (ch->base.disk->access_local) {
			/* Set the operations we can perform using the local bypass */
			set_bit(NVMEIBC_LOCK_READ, ch->local_bypass_bmp);
			set_bit(NVMEIBC_LOCK_BLKSET_INFO_WRITE, ch->local_bypass_bmp);
			set_bit(NVMEIBC_LOCK_BLKSET_INFO_READ, ch->local_bypass_bmp);
			/* local lock-ch is loopback i.e. same dev thus checking
			   cap of clnt's dev (ldev) = checking cap of ldev&&rdev */
			if (nvmeib_device_sup_cap(
				P2NV(ch->net.port)->dev_type, NVMEIB_DEVCAP_PCIE_ATOMICS)) {
				/* IB Devices that do PCIe atomics (or SIW) can also use cmpxchg via the local bypass */
				set_bit(NVMEIBC_LOCK_CMP_AND_SWAP, ch->local_bypass_bmp);
			}
		}
	}
	else {
		/* use RPC */
		ch->atomic_req_endian_swap = false;
		ch->atomic_reply_endian_swap = false;
		ch->masked_atomic_req_endian_swap = false;
		ch->masked_atomic_reply_endian_swap = false;
		ch->net.post_send_atomic_fn =
			nvmeibc_disk_locks_server_side_post_send_atomic;
		ch->base.disk->rpc_locks = true;

		if (ch->base.disk->access_local)
			bitmap_fill(ch->local_bypass_bmp, NVMEIBC_LOCK_NUM_OPR);

		rv = 0;
	}

	BUILD_BUG_ON(NVMEIBC_LOCK_NUM_OPR > 32);
	_NT(trace_locks_channel_resolve_ch_atomic_caps_and_endianness,
		"atomic_cap: l={@INT, @INT}, r={@INT, @INT} -> {@INT, @INT},"
		"use-rpc-locks=@BOOL, acc-loc=@BOOL, local_bypass_bmp=@BITMAP32",
		l_atomic_cap, l_masked_atomic_cap,
		r_atomic_cap, r_masked_atomic_cap,
		ch->atomic_cap, ch->masked_atomic_cap,
		ch->base.disk->rpc_locks,
		ch->base.disk->access_local, ch->local_bypass_bmp);

out:
	NFOUT;
	return rv;
}

/**
 * allocates and connect to locks channel
 *
 * @param ionet
 * @param params
 * @param lreq
 *
 * @return int o uppon success
 */
static int try_connect(struct nvmeibc_locks_channel *ch,
	struct nvmeibc_admin_channel *admin_ch, union ib_gid *dgid,
	struct nvmeibc_ib_port *lport, int max_tgt_atomic_ops,
	unsigned tcp_base_port, unsigned tcp_num_ports)
{
	int rv = 0, i;
	struct nvmeibc_lock_opr_in_progress *opr_ip;
	struct nvmeib_rdma_path_info info = {0};
	struct nvmeibc_login_request req = {};
	struct nvmeibc_ib_net_params *params = NULL;
	char lgid_str[GUID_SIZE] = {0}, dgid_str[GUID_SIZE] = {0};

	NFIN;
	BUG_ON(admin_ch == NULL);

	format_gid(&lport->gid.gid, lgid_str);
	format_gid(dgid, dgid_str);
	if (!(params = kzalloc(sizeof(*params), GFP_KERNEL))) {
		_NE(error_locks_channel_try_connect, "Fail to allocate net params");
		rv = -1;
		goto out;
	}
	_NT(trace_locks_channel_try_connect, "LOCKS: request to create locks channel for gid @DGID_STR from local gid"
	   " @LGID_STR", dgid_str, lgid_str);
	ch->primary_ch = ch;
	ch->net.port = lport;
	/* first get the local guid */
	memcpy(&ch->net.path.sgid.raw, &lport->gid.gid, 16);
	memcpy(&ch->net.path.dgid.raw, dgid, 16);
	ch->net.path.service_id = cpu_to_be64(NVMEIB_SERVICE_ID);
	ch->net.path.pkey = cpu_to_be16(lport->pkey);
	ch->net.ioch = &ch->base;
	ch->net.admin_ch = admin_ch;
	snprintf(ch->base.name, sizeof(ch->base.name),
			 "%.*s-%.*s~LOCK[000]",
			 (int)sizeof(ch->base.rhost_name), admin_ch->base.rhost_name,
			 (int)sizeof(ch->base.disk->name), ch->base.disk->name);
	_ND(trace_1_locks_channel_try_connect, "LOCKS net ioch is @IOCH nptr @IOCH_NAME_PTR str @IOCH_NAME pkey=", ch->net.ioch, ch->net.ioch->name,
			ch->net.ioch->name);
	info.dev = P2NV(lport);
	info.sa = nvmeibc_sa_client(nvmeibc_cinst_get_core_p(&ch->base));
	info.path = &ch->net.path;
	info.src_port = lport->port;
	if (lport->layer == IB_LINK_LAYER_INFINIBAND) {
		_ND(trace_2_locks_channel_try_connect, "LOCKS: locks channel is trying to connect via infiniband");
		info.service_id = NVMEIB_SERVICE_ID;
		ch->net.service_id = NVMEIB_SERVICE_ID;
		info.pkey = lport->pkey;
		info.service_port = 0;
		ch->net.service_port = 0;
	}
	else {
		bool is_tcp = lport->transport_type == RDMA_TRANSPORT_IWARP;
		u16 service_port = NVMEIB_PORT_ID;

		if (is_tcp) {
			/* Lock channels use ports from the high end of the range (reverse of nordda) to spread target CPU load. */
			uint offset = (ch->base.disk->create_id * (ch->base.disk->max_2nd_lock_chs + 1)) % tcp_num_ports;
			service_port = tcp_base_port + (tcp_num_ports - 1 - offset);
		}

		_ND(trace_3_locks_channel_try_connect,
			"LOCKS locks channel is trying to connect via @STRING_LITERAL port @NVMEIB_PORT_ID",
			is_tcp ? "TCP" : "RoCE", service_port);
		info.service_id = 0;
		info.pkey = 0;
		info.service_port = service_port;
		ch->net.service_id = 0;
		ch->net.service_port = service_port;
	}
	if ((rv = nvmeibc_disk_find_path(admin_ch->base.disk, &info))) {
		_NT(trace_4_locks_channel_try_connect, "lock device is not reachable");
		rv = -1;
		goto out_err2;
	}
	ch->net.cm_rdma_type = info.rdma_type;
	/*set rkey and lkey for message passing*/
	log_device_atomic_params(lport->nic_dev->dev->ib_dev);
	ch->net.lkey = nvmeib_get_lkey(P2NV(lport));
	ch->net.rkey = nvmeib_get_rkey(P2NV(lport));
	ch->net.pkey = lport->pkey;
	_ND(trace_5_locks_channel_try_connect, "LOCKS: pkey is @PKEY", ch->net.pkey);
	ch->base.execute_io = NULL;
	ch->base.execute_pending_io = NULL;
	ch->net.rearm_send_cq = true;
	/* set the params to create the net */
	params->use_atomic = true;
	params->max_send_q = NVMEIBC_CHANNEL_NUM_OF_ALLOC_APR;
	params->max_send_sg = 1;
	/* Locks channel does not handle recv */
	params->max_recv_q = 0;
	params->max_recv_sg = 0;
	params->recv_msg_size = 0;
	params->max_send_cq = NVMEIBC_CHANNEL_NUM_OF_ALLOC_APR * 2;
	params->max_recv_cq = 1;
	params->use_srq = !nvmeibc_use_pcpu_cq ? false : nvmeibc_support_srq(P2NV(lport));
	params->max_rd_atomic = min_t(int, nvmeib_device_get_max_rd_atom_on_wire(P2NV(lport)->dev_type), max_tgt_atomic_ops);
	params->max_dest_rd_atomic = 0; /* This is the max incoming, so can be zero */
	params->srq_priv = NULL;
	params->call_send_comp_handler = lock_send_completion;
	params->call_receive_comp_handler = handle_locks_message;
	params->free_net_on_connect_err = true;
	params->on_login = on_login_lock_ch;
	/* do not set the disconnect callback till we are done testing - we do not
	   want the lock_channel default disconnect function to trigger the disk
	   release beforw we are done testing atomics as if the test fail we tead
	   down the channel here...
	*/
	params->on_disconnect = NULL;
	nvmeibc_login_req_init(&req,
				ch->base.disk->last_tgt_link_ver,
				NVMEIBC_LOCK_CHANNEL,
				admin_ch->cid,
				ch->base.disk->access_local,
				ch->net.path.dgid.global.subnet_prefix,
				ch->net.path.dgid.global.interface_id);
	nvmeibc_login_req_set_msg_hdr(&req, NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED,
				      NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED, 0, 0);
	_ND(trace_6_locks_channel_try_connect, "req local is @LOCAL_INT", nvmeibc_login_req_get_local(&req));
	INIT_LIST_HEAD(&ch->defered);
	INIT_LIST_HEAD(&ch->in_progress);
	ch->net.cm_id = NULL;
	ch->send_enumerator = 0;
	if (nvmeibc_lock_ch_scq_use_kwq) {
		/* Use kernel workqueue */
		params->scq_kwq = nvmeibc_locks_channel_get_wq();
		params->scq_offload_enb = false;
		if (!params->scq_kwq) {
			_NE(error_locks_channel_scq_kwq, "Kernel workqueue not available for SCQ");
			rv = -ENOMEM;
			goto out_err;
		}
	} else {
		/* Use kthread */
		params->scq_kwq = NULL;
		params->scq_offload_enb =
			P2NV(lport)->dev_type == DT_siw ?
				nvmeibc_lock_ch_scq_offload_thread_tcp :
				nvmeibc_lock_ch_scq_offload_thread;
	}
	params->ch_index = 0;
	params->comp_cpu = NVMEIB_CPU_INVALID;
	params->vector_type = NVMEIB_CQ_VECTOR_GET_TYPE_LOCK;

	if ((rv = nvmeibc_ib_net_alloc(&ch->net, params, &req)) < 0) {
		_NT(error_1_locks_channel_try_connect, "cannot connect. error @RV",rv);
		goto out_err;
	}

	_NT(trace_7_locks_channel_try_connect, "LOCKS: Able to create locks channel for gid @DGID_STR from local gid"
	    " @LGID_STR ch=@CHANNEL_PTR, QPn: @QP_NUM Remote QPn: @QP_NUM SQ PSN: @SQ_PSN", dgid_str,
		lgid_str, ch, ch->net.qp->qp_num, ch->net.remote_qpn, ch->net.sq_psn);

	ch->max_atom_read_ip = ch->net.max_dest_rd_atomic * 2;

	/*now that we are connected we can allocate all local resources*/
	INIT_LIST_HEAD(&ch->free_ip_pool);
	INIT_LIST_HEAD(&ch->aborted);

	ch->opr_ip_buffer_phys = ib_dma_map_page(P2IB(ch->net.port), ch->opr_ip_buffer_page,
	0, ch->opr_ip_buffer_size, DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(P2IB(ch->net.port), ch->opr_ip_buffer_phys)) {
		_NE(error_6_locks_channel_try_connect, "dma mapping opr_ip_buffer failed");
		ch->opr_ip_buffer_phys = 0;
		rv = -1;
		goto out_err;
	}
	for (i = 0; i < NVMEIBC_CHANNEL_NUM_OF_ALLOC_APR; ++i) {
		opr_ip = &ch->locks_ip_buffer[i];
		opr_ip->index = i;
		opr_ip->ch = ch;
		opr_ip->val = &ch->opr_ip_buffer[i * NVMEIB_LOCK_DATA_BUFFERS];
		opr_ip->opr_ip_buffer_offset = i * sizeof(*ch->opr_ip_buffer) * NVMEIB_LOCK_DATA_BUFFERS;
		opr_ip->val[0] = 0;
		opr_ip->val[1] = 0;
		opr_ip->comp = NULL;
		NVMEIBC_LOCK_GUARD_INIT(&opr_ip->state, LOCK_OPR_IN_FREE_LIST);
		NVMEIBC_LOCK_GUARD_INIT(&opr_ip->bypass_state, LOCK_OPR_NO_BYPASS);
		list_add_tail(&opr_ip->link, &ch->free_ip_pool);
		ch->num_of_free++;
		ch->total_num_opr++;
		init_lock_watchdog(ch, opr_ip);
	}
	_ND(trace_8_locks_channel_try_connect, "LOCKS: net @NET and send_cq @SEND_CQ", &ch->net, ch->net.send_cq);

	if (!(ch->atomic_test_src = kzalloc(sizeof(*ch->atomic_test_src), GFP_KERNEL))) {
		_NE(error_3_locks_channel_try_connect, "Memory allocation error");
		rv = -ENOMEM;
		goto out_err;
	}
	ch->atomic_test_zone_lkey = nvmeib_get_lkey(P2NV(ch->net.port));
	ch->atomic_test_zone_laddr = ib_dma_map_single(P2IB(ch->net.port), ch->atomic_test_src,
						       sizeof(*ch->atomic_test_src), DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(P2IB(ch->net.port), ch->atomic_test_zone_laddr)) {
		_NE(error_4_locks_channel_try_connect, "dma mapping failed");
		ch->atomic_test_zone_laddr = 0;
		rv = -EFAULT;
		goto out_err;
	}

	if ((rv = resolve_ch_atomic_caps_and_endianness(ch)))
		goto out_err;

	_NT(trace_9_locks_channel_try_connect, "ch @CH_PTR, atmoic_cap=@ATMOIC_CAP, masked_atomic_cap=@MASKED_ATOMIC_CAP, post-send='@POST_SEND_ATOMIC_FN'",
	   ch, ch->atomic_cap, ch->masked_atomic_cap, ch->net.post_send_atomic_fn);

	//PATCH: Fix me!
	/* after we are done with testing atomics we can set the disconnect function
	   which when called will release the disk...
	*/
	{
		struct nvmeibc_ib_net *net = &ch->net;
		unsigned long flags;
		int dying;

		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags); //To sync with events (DREQ, etc).
		dying = atomic_read(&net->dying);
		if (!dying) {
			ch->net.on_disconnect = on_disconnect_lock_ch;
			ch->_2nd_net_params = params;
		}
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

		if (dying) {
			_NE(error_5_locks_channel_try_connect, "net @NET marked dying (@DYING) before "
			   "assigning on-disconnect cb", net, dying);
			rv = -1;
			goto out_err;
		}
	}

	goto out;
	/*test the locks channel*/

out_err:
	free_premature_lock_ch(ch);
out_err2:
	kfree(params);
out:
	NFOUT;
	return rv;
}

static void free_2nd_ch(struct nvmeibc_locks_channel *ch)
{
	if (ch->callback_wq) {
		#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
		destroy_workqueue(ch->callback_wq);
		#else
		wq_destroy(ch->callback_wq);
		#endif
		ch->callback_wq = NULL;
	}
	kfree(ch->_2nd_net_params);
	if (ch->opr_ip_buffer_page) {
		__free_pages(ch->opr_ip_buffer_page, get_order(ch->opr_ip_buffer_size));
		ch->opr_ip_buffer_page = NULL;
		ch->opr_ip_buffer = NULL;
	}
	kfree(ch->locks_ip_buffer);
	kfree(ch);
}

static int init_2nd_ch(struct nvmeibc_locks_channel *primary_ch, int n_idx,
	u64 cid, int comp_cpu, unsigned tcp_base_port, unsigned tcp_num_ports, bool comp_ll)
{

	struct nvmeibc_ib_port *lport = primary_ch->net.port;
	union ib_gid *dgid = &primary_ch->net.path.dgid;
	int rv = 0;
	struct nvmeib_rdma_path_info info = {0};
	struct nvmeibc_locks_channel *ch = NULL;
	struct nvmeibc_ib_net *net = NULL;

	NFIN;
	BUG_ON(primary_ch == NULL);

	if (!(primary_ch->_2nd_ch[n_idx] = kzalloc(sizeof(struct nvmeibc_locks_channel), GFP_KERNEL))) {
		_NE(error_locks_channel_init_2nd_ch, "Memory allocation error");
		rv = -ENOMEM;
		goto out;
	}
	ch = primary_ch->_2nd_ch[n_idx];
	ch->opr_ip_buffer_size = sizeof(*ch->opr_ip_buffer) * NVMEIB_LOCK_DATA_BUFFERS * NVMEIBC_LOCK_2ND_CH_NUM_OF_OPR;
	if (!(ch->locks_ip_buffer = kzalloc(sizeof(*ch->locks_ip_buffer) * NVMEIBC_LOCK_2ND_CH_NUM_OF_OPR, GFP_KERNEL)) ||
		!(ch->opr_ip_buffer_page = alloc_pages_node(primary_ch->base.numa_node, GFP_KERNEL, get_order(ch->opr_ip_buffer_size))))
	{
		_NE(error_1_locks_channel_init_2nd_ch, "Memory allocation error");
		rv = -ENOMEM;
		goto free_ch;
	}
	ch->opr_ip_buffer = page_address(ch->opr_ip_buffer_page);
	ch->total_num_opr = 0;
	ch->primary_ch = primary_ch;
	nvmeibc_lock_ch_metrics_init(&ch->metrics);
	if (!(ch->_2nd_net_params = kmemdup(primary_ch->_2nd_net_params, sizeof(*primary_ch->_2nd_net_params), GFP_KERNEL))) {
		_NE(error_9_locks_channel_init_2nd_ch, "Memory allocation error");
		rv = -ENOMEM;
		goto free_ch;
	}
	nvmeibc_locks_channel_spin_lock_init(ch);
	ch->locking_cpu = -1;

	if ((rv = nvmeibc_channel_init(&ch->base,
		nvmeibc_cinst_get_core_p(&primary_ch->base), primary_ch->base.numa_node)))
	{
		_NE(error_2_locks_channel_init_2nd_ch, "cannot init base channel");
		goto free_ch;
	}
#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
	_NT(trace_0_locks_channel_init_2nd_ch,
		"Not creating dedicated kernel WQ for 2nd lock channel callbacks");
#else
	if (!(ch->callback_wq = wq_create(proc_name_format("C", "WQ", "lock_cb")))) {
		_NE(error_8_locks_channel_init_2nd_ch, "cannot allocate cb wq");
		goto free_ch;
	}
#endif

	INIT_LIST_HEAD(&ch->link);
	ch->base.ct = ct_lock_2nd;
	ch->base.disk = primary_ch->base.disk;

	net = &ch->net;

	/* Initialize lists, state, resources, etc. */
	ch->send_enumerator = 0;
	INIT_LIST_HEAD(&ch->defered);
	INIT_LIST_HEAD(&ch->in_progress);
	INIT_LIST_HEAD(&ch->free_ip_pool);
	INIT_LIST_HEAD(&ch->aborted);

	_ND(trace_locks_channel_init_2nd_ch, "LOCKS: request to connect secondary lock net for gid @DGID from local gid @GID_IPV6",
	   dgid, &lport->gid.gid);
	net->port = lport;
	/* first get the local guid */
	net->path.sgid = lport->gid.gid;
	net->path.dgid = *dgid;
	net->path.service_id = cpu_to_be64(NVMEIB_SERVICE_ID);
	net->path.pkey = cpu_to_be16(lport->pkey);
	net->ioch = &ch->base;
	net->admin_ch = primary_ch->net.admin_ch;
	snprintf(ch->base.name, sizeof(ch->base.name),
			 "%.*s-%.*s~LOCK[%03d]",
			 (int)sizeof(ch->base.rhost_name), ch->net.admin_ch->base.rhost_name,
			 (int)sizeof(ch->base.disk->name), ch->base.disk->name, n_idx + 1);
	ch->base.index = n_idx + 1;
	_ND(trace_1_locks_channel_init_2nd_ch, "LOCKS: 2nd lock ioch is @IOCH name @IOCH_NAME", net->ioch, net->ioch->name);
	info.dev = P2NV(lport);
	info.sa = nvmeibc_sa_client(nvmeibc_cinst_get_core_p(&ch->base));
	info.path = &net->path;
	info.src_port = lport->port;
	if (lport->layer == IB_LINK_LAYER_INFINIBAND) {
		info.service_id = NVMEIB_SERVICE_ID;
		net->service_id = NVMEIB_SERVICE_ID;
		info.pkey = lport->pkey;
		info.service_port = 0;
		net->service_port = 0;
	}
	else {
		info.service_id = 0;
		info.pkey = 0;
		if (lport->transport_type == RDMA_TRANSPORT_IWARP) {
			/* Lock channels use ports from the high end of the range (reverse of nordda) to spread target CPU load. */
			uint offset = (ch->base.disk->create_id * (ch->base.disk->max_2nd_lock_chs + 1) + ch->base.index) % tcp_num_ports;
			info.service_port = tcp_base_port + (tcp_num_ports - 1 - offset);
		} else {
			info.service_port = NVMEIB_PORT_ID;
		}
		net->service_id = 0;
		net->service_port = info.service_port;
	}
	if ((rv = nvmeibc_disk_find_path(ch->base.disk, &info))) {
		_NT(trace_2_locks_channel_init_2nd_ch, "lock device is not reachable");
		rv = -ENETUNREACH;
		goto free_ch;
	}
	net->cm_rdma_type = info.rdma_type;
	/*set rkey and lkey for message passing*/
	log_device_atomic_params(lport->nic_dev->dev->ib_dev);
	net->lkey = nvmeib_get_lkey(P2NV(lport));
	net->rkey = nvmeib_get_rkey(P2NV(lport));
	net->pkey = lport->pkey;
	_ND(trace_3_locks_channel_init_2nd_ch, "LOCKS: pkey is @PKEY", net->pkey);
	ch->base.execute_io = NULL;
	ch->base.execute_pending_io = NULL;
	net->rearm_send_cq = true;
	nvmeibc_login_req_init(&ch->lreq,
				ch->base.disk->last_tgt_link_ver,
				NVMEIBC_SECONDARY_LOCK_CH,
				cid,
				ch->base.disk->access_local,
				ch->net.path.dgid.global.subnet_prefix,
				ch->net.path.dgid.global.interface_id);
	nvmeibc_login_req_set_msg_hdr(&ch->lreq, NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED,
				      NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED, 0, 0);
	_ND(trace_4_locks_channel_init_2nd_ch, "req local is @LOCAL_INT", nvmeibc_login_req_get_local(&ch->lreq));
	net->cm_id = NULL;

	ch->_2nd_net_params->ch_index = n_idx + 1;

	/* comp_cpu is either the completion cpu or NVMEIBC_COMP_CPU_INVALID from caller */
	nvmeibc_channel_pcpu_ch_set_cpu(&ch->base, comp_cpu, comp_ll);
	ch->_2nd_net_params->comp_cpu = comp_cpu;

	goto out;

free_ch:
	free_2nd_ch(ch);
	primary_ch->_2nd_ch[n_idx] = NULL;

out:
	NFOUT;
	return rv;
}

static int try_connect_2nd_ch(struct nvmeibc_locks_channel *ch) {
	struct nvmeibc_ib_net *net = &ch->net;
	struct nvmeibc_locks_channel *primary_ch = ch->primary_ch;
	struct nvmeibc_lock_opr_in_progress *opr_ip;
	int rv, i;

	if ((rv = nvmeibc_ib_net_alloc(net, ch->_2nd_net_params, &ch->lreq)) < 0) {
		_NT(error_3_locks_channel_try_connect_2nd_ch, "cannot connect. error @RV",rv);
		rv = -ECONNRESET;
		goto out_err;
	}

	_NT(trace_5_locks_channel_try_connect_2nd_ch, "LOCKS: Able to create secondary locks net for gid @DGID from local gid"
		"@GID_IPV6 ch=@CH_PTR, QPn: @QP_NUM Remote QPn: @REMOTE_QPN SQ PSN: @SQ_PSN", &net->path.dgid, &net->path.sgid,
		ch, net->qp->qp_num, net->remote_qpn, net->sq_psn);

	ch->max_atom_read_ip = ch->net.max_dest_rd_atomic * 2;

	/* Copy the common info from the primary channel,
	   No need to retest */
	ch->atomic_cap = primary_ch->atomic_cap;
	ch->masked_atomic_cap = primary_ch->masked_atomic_cap;
	ch->atomic_req_endian_swap = primary_ch->atomic_req_endian_swap;
	ch->atomic_reply_endian_swap = primary_ch->atomic_reply_endian_swap;
	ch->masked_atomic_req_endian_swap = primary_ch->masked_atomic_req_endian_swap;
	ch->masked_atomic_reply_endian_swap = primary_ch->masked_atomic_reply_endian_swap;
#if DEBUG_2ND_LOCK_CH_TEST_ATMOIC
	ch->net.post_send_atomic_fn = primary_ch->net.post_send_atomic_fn;
	bitmap_copy(ch->local_bypass_bmp, primary_ch->local_bypass_bmp, NVMEIBC_LOCK_NUM_OPR);
#endif

	ch->opr_ip_buffer_phys = ib_dma_map_page(P2IB(ch->net.port), ch->opr_ip_buffer_page,
		0, ch->opr_ip_buffer_size, DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(P2IB(ch->net.port), ch->opr_ip_buffer_phys)) {
		_NE(error_8_locks_channel_try_connect_2nd_ch, "dma mapping opr_ip_buffer failed");
		ch->opr_ip_buffer_phys = 0;
		rv = -1;
		goto out_err;
	}
	/* Allocate opr_ip entries */
	for (i = 0; i < NVMEIBC_LOCK_2ND_CH_NUM_OF_OPR; ++i) {
		opr_ip = &ch->locks_ip_buffer[i];
		opr_ip->index = i;
		opr_ip->ch = ch;
		opr_ip->opr_ip_buffer_offset = i * sizeof(*ch->opr_ip_buffer) * NVMEIB_LOCK_DATA_BUFFERS;
		opr_ip->val = &ch->opr_ip_buffer[i * NVMEIB_LOCK_DATA_BUFFERS];
		opr_ip->comp = NULL;
		NVMEIBC_LOCK_GUARD_INIT(&ch->locks_ip_buffer[i].state, LOCK_OPR_IN_FREE_LIST);
		NVMEIBC_LOCK_GUARD_INIT(&ch->locks_ip_buffer[i].bypass_state, LOCK_OPR_NO_BYPASS);
		ch->locks_ip_buffer[i].comp = (void *)CONFIG_ILLEGAL_POINTER_VALUE;
		list_add_tail(&opr_ip->link, &ch->free_ip_pool);
		ch->num_of_free++;
		ch->total_num_opr++;
		init_lock_watchdog(ch, opr_ip);
	}

#if (DEBUG_2ND_LOCK_CH_KA || DEBUG_2ND_LOCK_CH_TEST_ATMOIC)
	if (!(ch->atomic_test_src = kzalloc(sizeof(*ch->atomic_test_src), GFP_KERNEL))) {
		_NE(error_5_locks_channel_try_connect_2nd_ch, "Memory allocation error")
		rv = -ENOMEM;
		goto out_err;
	}
	ch->atomic_test_zone_lkey = nvmeib_get_lkey(P2NV(ch->net.port));
	ch->atomic_test_zone_laddr = ib_dma_map_single(P2IB(ch->net.port), ch->atomic_test_src,
						       sizeof(*ch->atomic_test_src), DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(P2IB(ch->net.port), ch->atomic_test_zone_laddr)) {
		_NE(error_6_locks_channel_try_connect_2nd_ch, "dma mapping failed");
		rv = -EFAULT;
		goto out_err;
	}
#endif

#if DEBUG_2ND_LOCK_CH_TEST_ATMOIC
	if ((rv = resolve_ch_atomic_caps_and_endianness(ch)))
		goto out_err;

	_NT(trace_7_locks_channel_try_connect_2nd_ch, "ch @CH_PTR, atmoic_cap=@ATMOIC_CAP, masked_atomic_cap=@MASKED_ATOMIC_CAP, post-send='@POST_SEND_ATOMIC_FN'",
	   ch, ch->atomic_cap, ch->masked_atomic_cap, ch->net.post_send_atomic_fn);
#endif

	//PATCH: Fix me!
	/* after we are done with testing atomics we can set the disconnect function
	   which when called will release the disk...
	*/
	{
		struct nvmeibc_ib_net *net = &ch->net;
		unsigned long flags;
		int dying;

		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags); //To sync with events (DREQ, etc).
		dying = atomic_read(&net->dying);
		if (!dying) {
			ch->net.on_disconnect = on_disconnect_lock_ch;
			//ch->_2nd_net_params = params;
		}
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

		if (dying) {
			_NE(trace_8_locks_channel_try_connect_2nd_ch, "net @NET marked dying (@DYING) before "
			   "assigning on-disconnect cb", net, dying);
			rv = -1;
			goto out_err;
		}
	}


	goto out;

out_err:
	free_premature_lock_ch(ch);

out:
	NFOUT;
	return rv;
}

struct nvmeibc_lock_opr_in_progress *nvmeibc_locks_channel_get_free_opr_ip(
	struct nvmeibc_locks_channel *ch)
{
	struct nvmeibc_lock_opr_in_progress *rv = NULL;

	NFIN;

	rv = list_first_entry_or_null(&ch->free_ip_pool,
		struct nvmeibc_lock_opr_in_progress, link);

	if (rv){
		list_del_init(&rv->link);
		NVMEIBC_LOCK_GUARD_GET_CHECK(locks_free_opr_e7,
			&rv->state, LOCK_OPR_IN_FREE_LIST);
		BUG_ON(ch->num_of_free <= 0);
		ch->num_of_free--;
	}

	NFOUT;
	return rv;
}

void nvmeibc_locks_channel_free_opr_ip(struct nvmeibc_locks_channel *ch,
	struct nvmeibc_lock_opr_in_progress *item, enum lock_opr_state curr_state)
{
	NFIN;
	NVMEIBC_LOCK_GUARD_SWITCH_CHECK(nvmeibc_locks_channel_free_opr_ip_e1,
		&item->state, curr_state, LOCK_OPR_IN_FREE_LIST);
	if (item->disk_piggyb) {
		NVMEIBC_LOCK_GUARD_SWITCH_CHECK(
			nvmeibc_locks_channel_free_opr_ip_e2,
			&item->bypass_state,
			LOCK_OPR_BYPASS_ERROR, LOCK_OPR_NO_BYPASS);
	}
	list_add_tail(&item->link, &ch->free_ip_pool);
	ch->num_of_free++;

	NFOUT;
}

void nvmeibc_locks_channel_start_disconnect(struct nvmeibc_locks_channel *ch)
{
	NFIN;
	ch = get_primary_ch(ch);
	_NT(trace_locks_channel_nvmeibc_locks_channel_start_disconnect, "Disconnecting lock-ch @CH_PTR (net @NET)", ch, &ch->net);
	WQ_INIT_WORK(&ch->net.remove_work.work, locks_remove_work);
	if (on_wq(ch->net.admin_ch->remove_wq))
		locks_remove_work(&ch->net.remove_work.work);
	else {
		_NW(warn_locks_channel_nvmeibc_locks_channel_start_disconnect, "Unexpected: lock-ch disconnect triggered not from admin-wq");
		WARN_ON_ONCE(1); /* caller assumes lock-remove happens here !!! */
		#if 0
		nvmeibc_admin_channel_add_work(
			ch->net.admin_ch, &ch->net.remove_work.work);
		#endif
	}
	NFOUT;
}

void nvmeibc_locks_channel_disconnect(struct nvmeibc_locks_channel *lock_ch)
{
	struct nvmeibc_locks_channel *ch;
	int dying;

	NFIN;
	ch = get_primary_ch(lock_ch);
	BUG_ON(ch == NULL);
	BUG_ON(ch->net.ioch == NULL);
	_NT(trace_locks_channel_nvmeibc_locks_channel_disconnect, "atomic_inc_return");
	if ((dying = atomic_inc_return(&ch->base.dying)) == 1) {
		if (ch->net.on_disconnect) {
			_ND(trace_1_locks_channel_nvmeibc_locks_channel_disconnect, "LOCKS: net ioch is @IOCH nptr @IOCH_NAME_PTR str @IOCH_NAME", ch->net.ioch,
				ch->net.ioch->name, ch->net.ioch->name);
			nvmeibc_ib_net_disconnect(&ch->net);
		}
	}
	else {
		_ND(trace_2_locks_channel_nvmeibc_locks_channel_disconnect, "LOCKS: channel already died");
	}

	NFOUT;
}

void nvmeibc_locks_channel_lock_cmd_completion(struct nvmeibc_disk_lock_cmd *disk_lock_cmd,
	int err_code, enum lock_opr_rdma_bypass_state bypass_state)
{
	struct nvmeib_gen_cmd_lock_param *lock_param = &disk_lock_cmd->lock_param;
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp = &disk_lock_cmd->lock_rsp;
	struct nvmeibc_lock_opr_in_progress *opr = container_of(
		disk_lock_cmd, struct nvmeibc_lock_opr_in_progress, disk_lock_cmd);
	DECLARE_IB_WC_ONSTACK(wc);
	struct nvmeibc_ib_net *net = &opr->ch->net;
	u64 start, delta;
	unsigned long flags;

	NFIN;
	nvmesh_enter_nonsleepable();
	wc.wr_id = nordda_wr_id_encode(opr->version, NVMEIB_DISK_LOCK_OPR, opr->index);
	wc.status = err_code ? IB_WC_GENERAL_ERR : IB_WC_SUCCESS;

	switch (nvmeib_send_wr_common(opr->wr).opcode) {
	case IB_WR_ATOMIC_CMP_AND_SWP:
		wc.opcode = IB_WC_COMP_SWAP;
		break;
	case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
		wc.opcode = IB_WC_MASKED_COMP_SWAP;
		break;
	case IB_WR_RDMA_READ:
		wc.opcode = IB_WC_RDMA_READ;
		break;
	case IB_WR_RDMA_WRITE:
		wc.opcode = IB_WC_RDMA_WRITE;
		break;
	default:
		wc.status = IB_WC_GENERAL_ERR;
		_NT(nvmeibc_locks_channel_lock_cmd_completion_t1,
			"invalid lock op=@INT", nvmeib_send_wr_common(opr->wr).opcode);
	}

	if (wc.status == IB_WC_SUCCESS) {
		NVMEIBC_LOCK_GUARD_SWITCH_CHECK(
			nvmeibc_locks_channel_lock_cmd_completion_e1,
			&opr->bypass_state, bypass_state, LOCK_OPR_BYPASS_COMPLETE);

		switch (nvmeib_send_wr_common(opr->wr).opcode) {
		case IB_WR_ATOMIC_CMP_AND_SWP:
		case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
			opr->val[0] = lock_rsp->cmp_swap_val;
			break;

		case IB_WR_RDMA_READ:
			BUG_ON(lock_param->rdma.len != lock_rsp->read_len);
			memcpy(lock_param->rdma.data, lock_rsp->read_data, lock_rsp->read_len);
			break;
		case IB_WR_RDMA_WRITE:
			break;
		default:
			BUG_ON(1);
		}
	} else {
		NVMEIBC_LOCK_GUARD_SWITCH_CHECK(
			nvmeibc_locks_channel_lock_cmd_completion_e2,
			&opr->bypass_state, bypass_state, LOCK_OPR_BYPASS_ERROR);
	}

	start = jiffies;
	nvmeibc_disk_cmd_status_debug(&disk_lock_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
	BUG_ON(nvmeibc_channel_is_ll_pcpu_ch(&opr->ch->base));
	nvmeibc_channel_spin_lock_irqsave(&opr->ch->base, &flags);
	/* lock_send_completion */
	if (net->call_send_comp_handler(net, &wc, true) < 0) {
		_NE(error_locks_channel_nvmeibc_locks_channel_lock_cmd_completion, "net @NET send-comp-handling error, disconnect net", net);
		nvmeibc_ib_net_disconnect_(net);
	}
	nvmeibc_channel_spin_unlock_irqrestore(&opr->ch->base, flags);
	delta = jiffies - start;
	if (delta > net->scq_stats.max_intr_duration) {
		net->scq_stats.max_intr_duration = delta;
		_ND(trace_locks_channel_nvmeibc_locks_channel_lock_cmd_completion, "net @NET max_intr_duration = @MAX_INTR_DURATION", net,
			net->scq_stats.max_intr_duration);
	}
	nvmesh_exit_nonsleepable();
	NFOUT;
}

int
nvmeibc_locks_channel_connect_coremask_chs(struct nvmeibc_locks_channel *ch, struct nvmeibc_admin_channel *admin_ch,
					u64 coremask_uid, const struct nvmeib_cpu_mask *cpumask, void *coremask_cookie,
					const struct nvmeibc_locks_channel_coremask_ops *coremask_ops,
					struct nvmeib_cpu_mask *lch_cpumask)
{
	struct nvmeibc_locks_channel *primary_ch = get_primary_ch(ch);
	struct nvmeibc_locks_channel *pcpu_ch = NULL;
	struct nvmeib_cpu_mask chmask = {}, core_not_ch_mask = {};
	unsigned n_mask_ch = 0;
	int last_cpu, cpu, start_cpu, next_cpu;
	int rv;

	NFIN;
	if (!primary_ch->_2nd_ch_coremask) {
		_NW_dmesg(warn_nvmeibc_locks_channel_connect_coremask_chs_not_supp,
			  "Coremask Lock Channels are not enabled");
		rv = -ENOTSUPP;
		goto out;
	}
	if (primary_ch->_2nd_ch_pcpu_lockless) {
		_NW_dmesg(warn_nvmeibc_locks_channel_connect_coremask_chs_pcpu_ll,
			  "Coremask per-cpu Lock Channels cannot be lockless");
		rv = -ENOTSUPP;
		goto out;
	}
	NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, *cpumask) {
		if (cpu >= NVMEIB_DFLT_MAX_CPUS)
			break;
		if (primary_ch->_2nd_ch_coremask_map[cpu]) {
			_NE_dmesg(err_nvmeibc_locks_channel_connect_coremask_chs_overlap,
				  "OOPS! A coremask channel @CH_PTR already exists on this CPU @CPU",
				primary_ch->_2nd_ch_coremask_map[cpu], cpu);
			BUG();
		}
		if ((pcpu_ch = primary_ch->_2nd_ch_pcpu_map[cpu])) {
			BUG_ON(!nvmeibc_channel_is_pcpu_ch(&pcpu_ch->base));
			BUG_ON(nvmeibc_channel_is_ll_pcpu_ch(&pcpu_ch->base));
			BUG_ON(nvmeibc_channel_pcpu_ch_get_cpu(&pcpu_ch->base) != cpu);
			BUG_ON(nvmeibc_channel_is_coremask_ch(&pcpu_ch->base));
			NVMEIB_CPU_MASK_SET_CPU(cpu, chmask);
			n_mask_ch++;
		}
	}
	if (n_mask_ch < primary_ch->_2nd_ch_coremask &&
		primary_ch->n_2nd_ch < primary_ch->base.disk->max_2nd_lock_chs &&
		NVMEIB_CPU_MASK_AND_NOT(core_not_ch_mask, *cpumask, chmask))
	{
		/* Pick start cpu as (disk_create_id * num_channels % bitmap_weight(core_not_ch_mask)).
		 *
		 * The idea is that subsequent disks will not overlap completion cpus.
		 * For example with a mask with 8 bits set and 2 channels per mask, then:
		 * Disk 1: Will create channels on the CPUs of the first 2 bits set in the mask
		 * Disk 2: Will create channels on the CPUs of the second 2 bits set in the mask
		 * Disk 3: Will create channels on the CPUs of the third 2 bits set in the mask
		 * etc.
		 */
		start_cpu = NVMEIB_CPU_MASK_FIND_NTH_BIT((admin_ch->base.disk->create_id * primary_ch->_2nd_ch_coremask) % NVMEIB_CPU_MASK_WEIGHT(core_not_ch_mask), core_not_ch_mask);
		/* Starting from start cpu, connect n secondary channels to use as lock channels */
		for (cpu = start_cpu; n_mask_ch < primary_ch->_2nd_ch_coremask; cpu = next_cpu)
		{
			if ((rv = init_2nd_ch(primary_ch, primary_ch->n_2nd_ch, admin_ch->cid, cpu,
					0 /* TBD: tcp_base_port */,
					0 /* TBD: tcp_num_ports */,
					false /* comp_ll */
				)) < 0)
			{
				_NT(trace_nvmeibc_locks_channel_connect_coremask_chs_fail_init,
				    "LOCKS: Failed (@RV) to initialise Coremask @COREMASK_UID Lock Channel from Device @IB_DEV_NAME: @PORT",
				    rv, coremask_uid, P2IB(primary_ch->net.port)->name, primary_ch->net.port->port);
				break;
			}
			pcpu_ch = primary_ch->_2nd_ch[primary_ch->n_2nd_ch];
			rv = try_connect_2nd_ch(pcpu_ch);
			if (rv) {
				_NT(trace_nvmeibc_locks_channel_connect_coremask_chs_fail_connect,
				    "LOCKS: Failed (@RV) to connect Coremask @COREMASK_UID Lock Channel @CH_NAME from Device @IB_DEV_NAME: @PORT",
				    rv, coremask_uid, pcpu_ch->base.name, P2IB(pcpu_ch->net.port)->name, pcpu_ch->net.port->port);
				free_2nd_ch(pcpu_ch);
				primary_ch->_2nd_ch[primary_ch->n_2nd_ch] = NULL;
				break;
			}
			_NT(trace_nvmeibc_locks_channel_connect_coremask_chs_connect_ok,
			    "LOCKS: Connected Coremask @COREMASK_UID Lock Channel @CH_NAME on CPU @CPU from Device @IB_DEV_NAME: @PORT",
				coremask_uid, pcpu_ch->base.name, cpu, P2IB(pcpu_ch->net.port)->name, pcpu_ch->net.port->port);
			primary_ch->n_2nd_ch++;

			/* This map stores the percpu channels that are actually on that cpu.
			 * Later the _2nd_ch_coremask_map will be populated either
			 * by percpu channels that are either on that cpu or will be used by that cpu
			 */
			primary_ch->_2nd_ch_pcpu_map[cpu] = pcpu_ch;
			n_mask_ch++;

			/* Set cpu in chmask and recalculate mask of coremask cpus without channels */
			NVMEIB_CPU_MASK_SET_CPU(cpu, chmask);
			if (!NVMEIB_CPU_MASK_AND_NOT(core_not_ch_mask, *cpumask, chmask)) {
				/* No more coremask cpus without channels - break loop */
				break;
			}
			/* Calculate next cpu */
			if ((next_cpu = NVMEIB_CPU_MASK_NEXT(cpu, core_not_ch_mask)) >= NVMEIB_CPU_MASK_MAX_CPUS) {
				/* Wrap around mask */
				next_cpu = NVMEIB_CPU_MASK_NEXT(-1, core_not_ch_mask);
				BUG_ON(next_cpu >= NVMEIB_CPU_MASK_MAX_CPUS);
			}
		}
	}
	if (!n_mask_ch) {
		_NW(err_nvmeibc_locks_channel_connect_coremask_chs_no_mask_ch,
		    "LOCKS: Could not create lock channels for coremask @COREMASK_UID",
		    coremask_uid);
		rv = -ENOMEM;
		goto out;
	}
	/* Distribute pcpu channels for coremask either by using the per-cpu channel for this cpu
	 * if present or one of the other pcpu channels in the mask in RR fashion */
	last_cpu = -1;
	NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, *cpumask) {
		if (cpu >= NVMEIB_DFLT_MAX_CPUS)
			break;
		if ((pcpu_ch = primary_ch->_2nd_ch_pcpu_map[cpu])) {
			BUG_ON(nvmeibc_channel_pcpu_ch_get_cpu(&pcpu_ch->base) != cpu);
		} else {
			/* Go to the next set cpu in chmask, wrapping around if necessary */
			unsigned next_cpu = NVMEIB_CPU_MASK_NEXT(last_cpu, chmask);
			if (next_cpu >= NVMEIB_CPU_MASK_MAX_CPUS)
				next_cpu = NVMEIB_CPU_MASK_NEXT(-1, chmask);
			BUG_ON(next_cpu >= NVMEIB_CPU_MASK_MAX_CPUS);
			last_cpu = next_cpu;
			pcpu_ch = primary_ch->_2nd_ch_pcpu_map[next_cpu];
			BUG_ON(nvmeibc_channel_pcpu_ch_get_cpu(&pcpu_ch->base) != next_cpu);
		}

		/* Sanity checks */
		BUG_ON(!pcpu_ch);
		BUG_ON(!nvmeibc_channel_is_pcpu_ch(&pcpu_ch->base));
		BUG_ON(nvmeibc_channel_is_ll_pcpu_ch(&pcpu_ch->base));

		/* Set the coremask cookie (if not already set in previous iteration) */
		if (nvmeibc_channel_is_coremask_ch(&pcpu_ch->base)) {
			BUG_ON(nvmeibc_channel_get_coremask_ch_cookie(&pcpu_ch->base) != coremask_cookie);
		} else {
			if (coremask_ops->get_ref_fn)
				(*coremask_ops->get_ref_fn)(coremask_cookie);

			nvmeibc_channel_set_coremask_ch_cookie(&pcpu_ch->base, coremask_cookie);
			pcpu_ch->coremask_ops = coremask_ops;
		}

		primary_ch->_2nd_ch_coremask_map[cpu] = pcpu_ch;
		NVMEIB_CPU_MASK_SET_CPU(cpu, pcpu_ch->_2nd_ch_coremask_mask);
	}
	NVMEIB_CPU_MASK_COPY(*lch_cpumask, chmask);
	rv = n_mask_ch;

out:
	NFOUT;
	return rv;
}

void nvmeibc_locks_channel_disconnect_coremask_chs(struct nvmeibc_locks_channel *ch, const struct nvmeib_cpu_mask *cpumask,
						   void *coremask_cookie)
{
	struct nvmeibc_locks_channel *primary_ch = get_primary_ch(ch);
	struct nvmeibc_locks_channel *pcpu_ch;
	unsigned int cpu;
	NFIN;

	/* The target doesn't support disconnect of lock channels without rediscover so we just clear them from the map and keep them connected */
	NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, *cpumask) {
		pcpu_ch = primary_ch->_2nd_ch_coremask_map[cpu];

		/* Sanity checks */
		BUG_ON(!pcpu_ch);
		BUG_ON(!nvmeibc_channel_is_pcpu_ch(&pcpu_ch->base));
		BUG_ON(nvmeibc_channel_is_ll_pcpu_ch(&pcpu_ch->base));
		BUG_ON(!NVMEIB_CPU_MASK_TEST_CPU(nvmeibc_channel_pcpu_ch_get_cpu(&pcpu_ch->base), *cpumask));

		/* Clear coremask cookie (if not already cleared in previous iteration) */
		if (nvmeibc_channel_get_coremask_ch_cookie(&pcpu_ch->base)) {
			BUG_ON(nvmeibc_channel_get_coremask_ch_cookie(&pcpu_ch->base) != coremask_cookie);

			nvmeibc_channel_set_coremask_ch_cookie(&pcpu_ch->base, NULL);

			if (pcpu_ch->coremask_ops->put_ref_fn)
				(*pcpu_ch->coremask_ops->put_ref_fn)(coremask_cookie);
		}

		primary_ch->_2nd_ch_coremask_map[cpu] = NULL;
		NVMEIB_CPU_MASK_CLEAR_CPU(cpu, pcpu_ch->_2nd_ch_coremask_mask);
	}

	BUG_ON(!NVMEIB_CPU_MASK_IS_EMPTY(pcpu_ch->_2nd_ch_coremask_mask));
	NFOUT;
}

struct nvmeibc_channel *nvmeibc_locks_channel_get_coremash_ch_for_cpu(struct nvmeibc_locks_channel *ch,
								      void *coremask_cookie, int cpu,
								      struct nvmeib_cpu_mask *ch_cpumask)
{
	struct nvmeibc_locks_channel *primary_ch = get_primary_ch(ch);
	struct nvmeibc_locks_channel *pcpu_ch;
	struct nvmeibc_channel *ret = NULL;

	NFIN;
	if (cpu < 0 || cpu >= NVMEIB_DFLT_MAX_CPUS) {
		_NE(err_nvmeibc_locks_channel_get_coremash_ch_for_cpu_inv_cpu, "Invalid CPU @CPU", cpu);
		goto out;
	}

	if (!(pcpu_ch = primary_ch->_2nd_ch_coremask_map[cpu])) {
		goto out;
	}

	if (coremask_cookie != nvmeibc_channel_get_coremask_ch_cookie(&pcpu_ch->base)) {
		goto out;
	}

	NVMEIB_CPU_MASK_COPY(*ch_cpumask, pcpu_ch->_2nd_ch_coremask_mask);
	ret = &pcpu_ch->base;
out:
	NFOUT;
	return ret;
}
