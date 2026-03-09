/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib.h"
#include "linux/preempt.h"
#include "linux/cpumask.h"
#include "linux/irqflags.h"
#include "linux/netdevice.h"
#include "nvmeib_wd.h"
#include "nvmeib_utils.h"
#include "nvmeib_public.h"
#include "nvmeib_wd.h"
#include "nvmeib_srq.h"
#include "nvmeib_ib_driver.h"
#include "../common_public/kth/nvmeib_public_kth.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_version_shared.h"
#include "nvmeib_version_kernel.h"
#include "nvmeib_rdma.h"
#include "nvmeibc_disk.h"
#include "poll/nvmeib_public_intr_poll.h"
#include "nvmeibm_trace.h"
#include "nvmeib_public.h"
#include "nvmeib_numa.h"
#include "common/proc_epilog.h"
#include "nvmeib_msgloop.h"
#include "nvmeib_memmgr_metrics.h"
#include "nvmeib_pcpu_wq.h"
#include "nvmeib_completion_noise.h"
#include "common_public/nvmeib_public_keeper.h"
#include "nvmeib_json.h"
#include "nvmeib_jdr.h"
#include "nvmeib_io_stats.h"

/* Must be last to override module_{init/exit} */
#include "kr_undef.h"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("NVMeIB Common");
MODULE_LICENSE("GPL and additional rights");

#define DEBUG_LEVEL (int)1
static int (*debug_level_f)(void);

static DEFINE_MUTEX(cb_lock);

#define PROCFS_COMMON_STR "nvmeib"
static struct proc_dir_entry *proc_dir = NULL;

struct proc_dir_entry *io_pet_dir = NULL;
struct msgloop_procfs_ent *io_pet_writer = NULL;

int nvmeib_cmn_debug_level = DEBUG_LEVEL;
module_param_named(debug_level, nvmeib_cmn_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Enables debug logging (to the system log not NVMesh tracer) if set above 1.");

struct nvmeib_pcpu_wq *nvmeib_system_wq = NULL;
EXPORT_SYMBOL(nvmeib_system_wq);

unsigned nvmeib_pcpu_cq_max_cqs_per_dev = 0;
module_param_named(pcpu_cq_max_cqs_per_dev, nvmeib_pcpu_cq_max_cqs_per_dev, uint, 0444);
MODULE_PARM_DESC(pcpu_cq_max_cqs_per_dev, "Maximum number of percpu cqs per device. If set to 0, use system default.");

bool nvmeib_pcpu_cq_all_cpus = false;
module_param_named(pcpu_cq_all_cpus, nvmeib_pcpu_cq_all_cpus, bool, 0444);
MODULE_PARM_DESC(pcpu_cq_all_cpus, "Allocated CQ per online-cpus (per the Linux kernel) per device, which always process completions in a thread context, i.e. on the ipoller.");

bool nvmeib_pcpu_cq_comp_vecs_per_dev = true;
module_param_named(pcpu_cq_comp_vecs_per_dev, nvmeib_pcpu_cq_comp_vecs_per_dev, bool, 0444);
MODULE_PARM_DESC(pcpu_cq_comp_vecs_per_dev, "Y = use first N comp-vectors of device where N='max num of pcpu-cqs per device'. N = use all comp-vectors spread globally between all devices, which is usually not recommended.");

#define USER_POLL_TIMEOUT_GRANULARITY_MSECS 	100
#define POLL_DISABLE_TIMEOUT_TRACE_MSECS	5

unsigned nvmeib_pcpu_cq_user_poll_timeout_msecs = 100;
module_param_named(pcpu_cq_user_poll_timeout_msecs, nvmeib_pcpu_cq_user_poll_timeout_msecs, uint, 0644);
MODULE_PARM_DESC(pcpu_cq_user_poll_timeout_msecs, "Timeout to switch from user polling, i.e., polling from a user-space application thread context ,e.g. SPDK, back to ipoller for a CQ.");

int tracer_nvmeibm_debug_level = 3;
module_param_named(tracer_debug_level, tracer_nvmeibm_debug_level, int, 0644);
MODULE_PARM_DESC(tracer_debug_level, "This determines the level of tracing for this module. Only traces with this level or lower will be issued, see tracer severities above.");
EXPORT_SYMBOL(tracer_nvmeibm_debug_level);

unsigned int nvmeib_tcp_base_port_id = NVMEIB_IWARP_PORT_ID;
module_param_named(tcp_base_port_id, nvmeib_tcp_base_port_id, uint, 0444);
MODULE_PARM_DESC(tcp_base_port_id, "The first (base) port ID for secondary SIW (iWARP) listeners.");

unsigned int nvmeib_tcp_num_ports = 0;
module_param_named(tcp_num_ports, nvmeib_tcp_num_ports, uint, 0444);
MODULE_PARM_DESC(tcp_num_ports, "TCP: Secondary iWARP listeners number of TCP ports (0 = number of RX Queues or number of online CPUs)");

/* Keep polling for this many microseconds after the last completion
 * before attempting to rearm interrupts (0: disabled) */
unsigned int nvmeib_pcpu_process_cq_retry_usecs = 0;
module_param_named(pcpu_process_cq_retry_usecs, nvmeib_pcpu_process_cq_retry_usecs, uint, 0644);
MODULE_PARM_DESC(pcpu_process_cq_retry_usecs, "The time window in micro-seconds to keep polling after last completion before rearming interrupts (0: disabled).");

#define NVMEIB_INT_SHAPER_PROC_NAME "intr_shaper.json"
#define NVMEIB_FRAME_SIZE_USECS (1000)
#define NVMEIB_MAX_BURST (64)
#define NVMEIB_MAX_IRQ_TIME_USECS (2000)
#define NVMEIB_MAX_COMP_INTR_PCT_CPU (20)


unsigned int nvmeib_intr_shaper_max_burst = NVMEIB_MAX_BURST;
module_param_named(intr_shaper_max_burst, nvmeib_intr_shaper_max_burst, uint, 0644);
MODULE_PARM_DESC(intr_shaper_max_burst, "Defines the maximum number of recv completions to handle in an interrupt before entering poll mode.");

unsigned int nvmeib_intr_shaper_max_pct_cpu = NVMEIB_MAX_COMP_INTR_PCT_CPU;
module_param_named(intr_shaper_max_pct_cpu, nvmeib_intr_shaper_max_pct_cpu, uint, 0644);
MODULE_PARM_DESC(intr_shaper_max_pct_cpu, "Defines the maximum percentage of CPU time to spend processing completions in an interrupt before entering poll mode.");

unsigned int nvmeib_intr_shaper_max_irq_time_usecs = NVMEIB_MAX_IRQ_TIME_USECS;
module_param_named(intr_shaper_max_irq_time_usecs, nvmeib_intr_shaper_max_irq_time_usecs, uint, 0644);
MODULE_PARM_DESC(intr_shaper_max_irq_time_usecs, "Defines the maximum time to spend in an interrupt before entering poll mode.");

static struct nvmeib_intr_shaper *nvmeib_intr_shaper = NULL;
static struct nvmeib_public_procfs_ent *nvmeib_intr_shaper_procfs_ent = NULL;

struct nvmeib_intr_shaper *nvmeib_get_intr_shaper(void)
{
	return nvmeib_intr_shaper;
}
EXPORT_SYMBOL(nvmeib_get_intr_shaper);

/* option to blacklist NICs
 * Format is <hca_id>:<hca_id> ...
 */
#define MAX_NIC_BLACKLIST_LEN 256
static char *nvmeib_nic_blacklist[MAX_NIC_BLACKLIST_LEN];
module_param_array_named(nic_blacklist, nvmeib_nic_blacklist, charp, NULL, 0444);
MODULE_PARM_DESC(nic_blacklist, "A comma-separated list of NICs to blacklist, i.e. not use.");

/* Check to see if ib_device is in nic blacklist
 * Checks both the ib_device name and the netdevice name */
bool nvmeib_is_dev_in_blacklist(struct ib_device *ib_dev)
{
	int i;
	struct net_device *ndev = NULL;
	static const int port = 1; // TBD: Support more than 1 port ?
	bool ret = false;

	/* [NVMESH-4799]: Disable blacklist by netdev - does not always work */
	if (ib_dev->get_netdev && false) {
		/* ndev is NULL (iWarp) - Try ib_device->get_netdev */
		if (IS_ERR_OR_NULL(ndev = (*ib_dev->get_netdev)(ib_dev, port))) {
			_NT(ib_dev_in_bl_get_netdev_fail,
			    "get_netdev failed (@RV) for device @IB_DEVICE port @PORT_NUM",
			    PTR_ERR(ndev), ib_dev->name, 1);
			ndev = NULL;
		}
	}

	for (i = 0; i < MAX_NIC_BLACKLIST_LEN && nvmeib_nic_blacklist[i]; i++) {
		if (strcmp(nvmeib_nic_blacklist[i], ib_dev->name) == 0) {
			ret = true;
			goto out;
		}
		if (ndev && strcmp(nvmeib_nic_blacklist[i], ndev->name) == 0) {
			ret = true;
			goto out;
		}
	}
out:
	if (ndev)
		dev_put(ndev);
	return ret;
}
EXPORT_SYMBOL(nvmeib_is_dev_in_blacklist);

static void nvmeib_set_tcp_base_port_id(void)
{
	if (nvmeib_tcp_base_port_id < NVMEIB_IWARP_PORT_ID) {
		_NE_dmesg(err_nvmeib_set_tcp_base_port_id,
				  "override nvmeib_tcp_base_port_id=@UINT with default @UINT",
				  nvmeib_tcp_base_port_id, NVMEIB_IWARP_PORT_ID);
		nvmeib_tcp_base_port_id = NVMEIB_IWARP_PORT_ID;
	}
}

unsigned int nvmeib_get_tcp_base_port_id(void)
{
	return nvmeib_tcp_base_port_id;
}
EXPORT_SYMBOL(nvmeib_get_tcp_base_port_id);

static void nvmeib_set_tcp_num_ports(void)
{
	if (!nvmeib_tcp_num_ports) {
		_NT(trace_nvmeib_set_tcp_num_ports, "tcp_num_ports dynamically determined by net-device #rx_queues");
	} else {
		if (nvmeib_tcp_num_ports > NVMEIB_DFLT_MAX_CPUS) {
			nvmeib_tcp_num_ports = NVMEIB_DFLT_MAX_CPUS;
			_NW_dmesg(warn_nvmeib_set_tcp_num_ports,
				  "restrict tcp_num_ports to @N_PORTS", nvmeib_tcp_num_ports);
		} else {
			_NT(trace_2_nvmeib_set_tcp_num_ports, "tcp_num_ports set to @N_PORTS", nvmeib_tcp_num_ports);
		}
	}
}

unsigned int nvmeib_get_tcp_num_ports(struct nvmeib_dev *dev)
{
	if (nvmeib_tcp_num_ports != 0)
		return nvmeib_tcp_num_ports;
	if (dev && dev->dev_type == DT_siw && dev->ib_dev->get_netdev) {
		struct net_device *ndev = dev->ib_dev->get_netdev(dev->ib_dev, 1);

		if (ndev) {
			unsigned int n = ndev->real_num_rx_queues;

			dev_put(ndev);
			return n ? n : 1;
		}
	}
	return num_online_cpus();
}
EXPORT_SYMBOL(nvmeib_get_tcp_num_ports);

struct nvmeib_public_procfs_ent *alloc_diag_proc;
struct nvmeib_public_procfs_ent *completion_noise_proc;
struct nvmeib_public_procfs_ent *with_local_completion_noise_proc;

int nvmeib_debug_level(void)
{
	return nvmeib_cmn_debug_level;
}

void nvmeib_set_debug_level(int (*dlf)(void))
{
	debug_level_f = dlf;
}
EXPORT_SYMBOL(nvmeib_set_debug_level);

/* Daniel: utsname() is an unsafe function to call from interrupt context */
const char *nvmeib_get_utsname_nodename(void)
{
	static char nodename[__NEW_UTS_LEN + 1] = { 0x0 };
	if (!nodename[0])		/* Init on first use */
		snprintf(nodename, sizeof(nodename), "%s", utsname()->nodename);
	return &(nodename[0]);
}
EXPORT_SYMBOL(nvmeib_get_utsname_nodename);

static int get_device_phys_port_count(struct nvmeib_dev *dev)
{
	struct ib_device *device = dev->ib_dev;

	int port = 0;
	int ret;
	struct ib_port_attr attr;


	if (device->node_type == RDMA_NODE_IB_SWITCH)
		return 0;

	do {
		++port;
		ret = ib_query_port(device, port, &attr);
	} while (ret == 0);

	return port - 1;
}

static int get_dev_pops_fns(struct nvmeib_dev *dev)
{
	struct nvmeib_device_public_ops *pops;
	int rv = 0;

	NFIN;
	pops = nvmeib_ibdr_hwdev_pops_get(dev->ib_dev);

	if (IS_ERR_OR_NULL(pops)) {
		rv = PTR_ERR(pops);
		_NE(error_nvmeib_get_dev_pops_fns, "nvmeib_ibdr_hwdev_pops_get failed (@RV) for device @IB_DEV_NAME",
		   rv, dev->ib_dev->name);
		goto out;
	}

	if (!pops->map_mr || !pops->post_send_atomic) {
		_NE(error_1_nvmeib_get_dev_pops_fns, "public ops for device @IB_DEV_NAME is missing valid fn ptrs",
		   dev->ib_dev->name);
		nvmeib_ibdr_hwdev_pops_put(pops);
		rv = -ENOTSUPP;
		goto out;
	}

	dev->pops = pops;
	dev->map_mr_f = pops->map_mr;
	dev->post_send_atomic_fn = pops->post_send_atomic;
	dev->peek_cq = pops->peek_cq;

out:
	NFOUT;
	return rv;
}

#if defined(IO_POLL_THREAD) && IO_POLL_THREAD
static struct nvmeib_intr_pollers_ft intr_poller_ft;

#	define __poll_init		intr_poller_ft.init
#	define __poll_sched		intr_poller_ft.sched
#	define __poll_complete	intr_poller_ft.complete
#	define __poll_is_sched	intr_poller_ft.is_sched
#	define __poll_disable	intr_poller_ft.disable
#	define __poll_enable	intr_poller_ft.enable
/*
 *	.init = nvmeib_public_intr_poll_init,
 *	.sched = nvmeib_public_intr_poll_sched,
 *  .complete = nvmeib_public_intr_poll_complete,
 *  .is_sched = nvmeib_public_intr_poll_is_sched,
 *  .disable = nvmeib_public_intr_poll_disable
 *  .enable = nvmeib_public_intr_poll_enable											,
 */
#else
/* interrupt poller stuff */
#	define __poll_init 		irq_poll_init
#	define __poll_sched 	irq_poll_sched
#	define __poll_complete	irq_poll_complete
#	define __poll_disable	irq_poll_disable
#	define __poll_enable	irq_poll_enable
#endif

#define PCPU_CQ_MAX_SIZE		4096
#define PCPU_CQ2SRQ_SIZE_MARGIN	1024

unsigned nvmeib_pcpu_cq_size = PCPU_CQ_MAX_SIZE;
module_param_named(pcpu_cq_size, nvmeib_pcpu_cq_size, int, 0444);
MODULE_PARM_DESC(pcpu_cq_size, "The length or size of the shared completion queue when employing a per CPU shared completion and receive queue.");

unsigned nvmeib_pcpu_cq2srq_size_margin = PCPU_CQ2SRQ_SIZE_MARGIN;
module_param_named(pcpu_cq2srq_size_margin, nvmeib_pcpu_cq2srq_size_margin, int, 0444);
MODULE_PARM_DESC(pcpu_cq2srq_size_margin, "When employing a per CPU shared completion and receive queue, this determines how much bigger the SCQ is than the SRQ. Margin = CQ-size - SRQ-size.");

#define IB_INTR_POLL_BUDGET_IRQ 256

unsigned nvmeib_pcpu_cq_poll_budget = IB_INTR_POLL_BUDGET_IRQ;
module_param_named(pcpu_cq_poll_budget, nvmeib_pcpu_cq_poll_budget, uint, 0644);
MODULE_PARM_DESC(pcpu_cq_poll_budget, "The per-cpu CQs polling-mode's budget, which is the maximum number of CQ entries to be processed in an interrupt before offloading to ipoller thread.");

#define CQ_POLL_INTR
#define IB_POLL_FLAGS (IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS)
#define CQ_INTR_PROCESS_BATCH 4

unsigned nvmeib_pcpu_cq_intr_budget = CQ_INTR_PROCESS_BATCH;
module_param_named(pcpu_cq_intr_budget, nvmeib_pcpu_cq_intr_budget, uint, 0644);
MODULE_PARM_DESC(pcpu_cq_intr_budget, "The per-cpu CQs interrupt-mode's budget. This is the maximum number of completions to handle in a single interrupt.");

#define USER_POLL_BUDGET 64
unsigned nvmeib_pcpu_cq_user_poll_budget = USER_POLL_BUDGET;
module_param_named(pcpu_cq_user_poll_budget, nvmeib_pcpu_cq_user_poll_budget, uint, 0644);
MODULE_PARM_DESC(pcpu_cq_user_poll_budget, "The per-cpu CQs user-mode polling budget. This is the maximum number of CQ entries to process for each user-mode poll.");

/* Deprecated by intr-shaper - use NVMEIB_MAX_IRQ_TIME_USECS instead
* #define CQ_INTR_PROCESS_MAX_TIME msecs_to_jiffies(2)
*/
#define CQ_INTR_PROCESS_MAX_RESTART 10

#define cq_index(cq) (((char *)cq - (char *)cq->dev->cqs) / sizeof(*cq))

#ifdef CQ_POLL_INTR

/* spinlock is almost always used from the same core,
   it is only needed for the rare case where we rearm
   interrupts and decide to switch to polling on this
   core but another interrupt arrives on a different
   core - in which case we want to bail on teh intrr */
#define cq_lock(_cq, _flags) 						\
do { 												\
	spin_lock_irqsave(&_cq->lock, _flags);			\
	_cq->locking_cpu = smp_processor_id();			\
} while (0)

#define cq_unlock(_cq, _flags) 						\
do { 												\
	_cq->locking_cpu = -1;							\
	spin_unlock_irqrestore(&_cq->lock, _flags);		\
} while (0)

#define cq_qps_lock(_cq, _flags) 					\
do { 												\
	spin_lock_irqsave(&_cq->qps_lock, _flags);		\
	_cq->qps_locking_cpu = smp_processor_id();		\
} while (0)

#define cq_qps_unlock(_cq, _flags) 					\
do { 												\
	_cq->qps_locking_cpu = -1;						\
	spin_unlock_irqrestore(&_cq->qps_lock, _flags);	\
} while (0)

#define cq_qps_is_locked(_cq) ({					\
	irqs_disabled() && _cq->locking_cpu == smp_processor_id();\
})

#define wc_to_qp_key(_wc) ((u64)wc->qp)
#define clear_wc_qp_key(_wc) ((_wc)->qp = NULL)
#define is_wc_qp_key_cleared(_wc) ((_wc)->qp == NULL)

#define CQ_QP_CTX_INVALID (0xcccccccc)
#define CQ_QP_DEL_STAGE_MAX_DESTROYS (8)

struct nvmeib_dev_cq {
	struct nvmeib_dev *dev;
	struct ib_cq *cq;
	int ncqe;
	struct nvmeib_srq_info *srq_info;
	int n_qps;
	int intr;
	struct mutex guard;
	spinlock_t lock;
	int locking_cpu;
	struct ib_wc wcs[CQ_POLL_BATCH];
	void (*process)(struct ib_wc *wcs, int n_wcs, unsigned long *wcs_mask, void *ctx);
	union {
		struct nvmeib_irq_poll	iop;
		struct work_struct work; //omril: what is it used for?
	};
	int qp_ch[ct_end - ct_base + 1];
	char trace_buf[128];
	#ifdef CQ_DEBUG
	u64 n_completions;
	#endif
	int cpu_id;
	int cpu_id_sched; /* when nvmeib_pcpu_cq_all_cpus is ON, this CQ defers all comps to this cpu's ipoller */
	int n_cpu_change;
	enum nvmeib_dev_cq_poll_mode poll_mode;
	int budget_intr;

	u64 n_intrs;
	u64 n_polls;
	u64 n_user_polls;
	u64 n_comps_intr;
	u64 n_comps_poll;
	u64 n_comps_user;
	u64 n_prev_comps_intr;
	u64 n_prev_comps_poll;
	u64 n_prev_comps_user;
	u64 n_sw2p_missed_events;
	u64 n_sw2p_del_qps_maint;
	u64 n_rearm_intr;
	u64 n_rearm_poll;
	u64 n_rearm_fail;
	u64 n_user_poll_arm;
	u64 n_user_poll_wd;
	u64 n_slow_poll_disable;
	u64 n_wakeups_burst;
	u64 n_wakeups_cycles;
	u64 n_wakeups_irq_time;


	/* manage cq's qps */
	spinlock_t qps_lock;
	int qps_locking_cpu;
	struct radix_tree_root qps_live_tree; 	/* qp is intializing or operational */
	struct list_head qps_stop_list;		 	/* on qp-stop 			  --> stop fwd new polled-wc to net */
	struct list_head qps_del_list_0;		/* on qp-del  			  --> wait to wcs polled before qp-stop */
	struct list_head qps_del_list_1;		/* on next empty poll-cq  --> chill */
	struct list_head qps_del_list_2;		/* on next empty poll-cq  --> run qpi actions (destroy qp, cm and rdma_e_ctx) */
	struct list_head qps_del_list_3;		/* on next empty poll-cq  --> kfree qpi ; while here, prevent alloc new qp with same qp-ptr */
	/* future use */
	unsigned long qps_jif_list_0;
	unsigned long qps_jif_list_1;
	unsigned long qps_jif_list_2;
	unsigned long qps_jif_list_3;
	/* on free-cq, force immediate qp draining
	 *  assuming qps are quite for a while now */
	bool qps_draining;
	int qps_n_stop;
	int qps_n_del;
	bool qps_del_wip;

	/* Allow polling from user-space instead of interrupts (SPDK) */
	struct nvmeib_dev_cq_user_poll {
		struct proc_dir_entry *poll_proc;
		atomic_t open_cnt;
		/* WD for user-polling. If wd expires, go back to regular polling */
		TIMER_LIST_INSTANCE(poll_wd_timer);
		unsigned long last_poll_jif;
	} user_poll;

	bool user_poll_process;
};

struct nvmeib_srq_info *nvmeib_cq_get_srq(struct nvmeib_dev_cq *cq)
{
	return cq->srq_info;
}
EXPORT_SYMBOL(nvmeib_cq_get_srq);

struct ib_cq *nvmeib_cq_get_cq(struct nvmeib_dev_cq *cq)
{
	return cq->cq;
}
EXPORT_SYMBOL(nvmeib_cq_get_cq);

int nvmeib_cq_get_cpu(struct nvmeib_dev_cq *cq)
{
	return cq->cpu_id;
}
EXPORT_SYMBOL(nvmeib_cq_get_cpu);

int nvmeib_cq_get_intr(struct nvmeib_dev_cq *cq)
{
	return cq->intr;
}
EXPORT_SYMBOL(nvmeib_cq_get_intr);

struct nvmeib_cq_qp_info {
	/* key for lookup @qp_ctx on process-wc()
	   currently we rely on qp-key being qp-ptr see wc_to_qp_key().
	   otherwise, qp->key resides in wc->qp->qp_context and we'll need
	   two lookups: (1) validate wc->qp (2) validate wc->qp->qp_context */
	u64 qp_key;
	/* ctx for process_per_dev_cq */
	void *qp_ctx;
	/* num polled WCs currently in-processed + 1 (owner cnt) */
	struct nvmeib_ref n_processing;
	/* actions to run on destroy */
	struct list_head action_list;
	/* link to cq's qps' stop/del lists */
	struct list_head sd_link;
};

static void *is_qp_deleting_(struct nvmeib_dev_cq *cq, u64 qp_key)
{
	struct nvmeib_cq_qp_info *qpi;
	void *rv = NULL;

	list_for_each_entry(qpi, &cq->qps_stop_list, sd_link) {
		if (qpi->qp_key == qp_key) {
			rv = qpi;
			goto out;
		}
	}
	list_for_each_entry(qpi, &cq->qps_del_list_0, sd_link) {
		if (qpi->qp_key == qp_key) {
			rv = qpi;
			goto out;
		}
	}
	list_for_each_entry(qpi, &cq->qps_del_list_1, sd_link) {
		if (qpi->qp_key == qp_key) {
			rv = qpi;
			goto out;
		}
	}
	list_for_each_entry(qpi, &cq->qps_del_list_2, sd_link) {
		if (qpi->qp_key == qp_key) {
			_NI(is_qp_deleting__i1, "Found qp (0x@_X) in del-list #2", qp_key);
			rv = qpi;
			goto out;
		}
	}
	list_for_each_entry(qpi, &cq->qps_del_list_3, sd_link) {
		if (qpi->qp_key == qp_key) {
			_NW(is_qp_deleting__w1, "Found qp (0x@_X) in del-list #3", qp_key);
			rv = qpi;
			goto out;
		}
	}

out:
	return rv;
}

#define DEBUG_ORPHAN
#ifndef DEBUG_ORPHAN
#define debug_orphan(cq, wc)
#else
static void debug_orphan(struct nvmeib_dev_cq *cq, struct ib_wc *wc)
{
	u64 qp_key = wc_to_qp_key(wc);
	void *qpi = is_qp_deleting_(cq, qp_key);

	_NI(debug_orphan_i1, "cq=@PTR, orphan wc=@PTR with qp-key=0x@_X", cq, wc, qp_key);
	if (!qpi) {
		_NE(debug_orphan_e1, "OOPS, wc with qp(key)=0x@_X arrived after destroy-qp", qp_key);
		BUG();
	}
}
#endif /* DEBUG_ORPHAN */

static struct nvmeib_cq_qp_info *cq_qp_ref_get(struct nvmeib_dev_cq *cq,
					struct ib_wc *wc, bool is_drain)
{
	u64 qp_key = wc_to_qp_key(wc);
	struct nvmeib_cq_qp_info *qpi;
	ulong flags;

	cq_qps_lock(cq, flags);
	if (likely(!is_drain)) {
		qpi = radix_tree_lookup(&cq->qps_live_tree, qp_key);
		if (!qpi ||
			!nvmeib_ref_get(&qpi->n_processing)) {
			qpi = NULL;
			debug_orphan(cq, wc);
		}
	} else {
		list_for_each_entry(qpi, &cq->qps_stop_list, sd_link) {
			if (qpi->qp_key == qp_key) {
				/* Found qpi, but do not do ref_get as the ref has already been increased */
				goto unlock;
			}
		}
		_NI(nvmeib_cq_qp_ref_get,
		    "cq=@DEV_CQ, qp=@QP, qp_num=@QP_NUM, qpi not found for drain",
			cq, wc->qp, wc->qp->qp_num);
		/* Did not find qp_key in stop list */
		qpi = NULL;
	}
unlock:
	cq_qps_unlock(cq, flags);

	return qpi;
}

static void cq_qp_ref_put(struct nvmeib_cq_qp_info *qpi)
{
	nvmeib_ref_put(&qpi->n_processing);
}

/* Called from create_qp_per_dev_cq() */
int nvmeib_cq_qp_add(struct nvmeib_dev_cq *cq, u64 qp_key, void *qp_ctx)
{
	struct nvmeib_cq_qp_info *qpi;
	ulong flags;
	void *prev;
 	int rv;

	_NT(nvmeib_cq_qp_add_i1, "CQ @PTR, qp-key=0x@_X", cq, qp_key);

	if (in_interrupt() || irqs_disabled()) {
		_NE(nvmeib_cq_qp_add_e1, "Atomic but may sleep");
		rv = -EINVAL;
		goto out;
	}

	if (!(qpi = kzalloc(sizeof(*qpi), GFP_KERNEL))) {
		_NE(nvmeib_cq_qp_add_e2, "Fail to alloc");
		rv = -ENOMEM;
		goto out;
	}

	cq_qps_lock(cq, flags);
	if ((prev = is_qp_deleting_(cq, qp_key))) {
		_NE(nvmeib_cq_qp_add_e3, "Fail to add qp-key=0x@_X, still del prev qpi=@PTR", qp_key, prev);
		rv = -EADDRINUSE;
	}
	else {
		qpi->qp_key = qp_key;
		qpi->qp_ctx = qp_ctx;
		nvmeib_ref_init(&qpi->n_processing);
		INIT_LIST_HEAD(&qpi->sd_link);
		INIT_LIST_HEAD(&qpi->action_list);
		_NT(nvmeib_cq_qp_add_t1, "Add to live tree, key=0x@_X, qpi=@PTR", qp_key, qpi);
		rv = radix_tree_insert(&cq->qps_live_tree, qp_key, qpi);
	}
	cq_qps_unlock(cq, flags);

	if (rv) {
		kfree(qpi);
	}

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_cq_qp_add);

void nvmeib_cq_qp_destroy_action_f(struct nvmeib_cq_qp_action *action)
{
	struct nvmeib_cq_qp_destroy_action *a =
		container_of(action, struct nvmeib_cq_qp_destroy_action, action);

	_NI(nvmeib_cq_qp_destroy_action_f_i1, "Destroy qp @PTR and cm @PTR (a=@PTR)", a->qp, a->cm_id, a);
	nvmeib_rdma_cm_owner_set(a->cm_id, -1, current->pid);
	nvmeib_rdma_destroy_qp(a->cm_id, a->qp);
	nvmeib_rdma_destroy_cm(a->cm_id);
	nvmeib_rdma_evt_ctx_destroy(a->rdma_e_ctx);
}
EXPORT_SYMBOL(nvmeib_cq_qp_destroy_action_f);

#define SCQ_STATS_PAD_BLANKS_LEN_INT (5)  //(8)
#define SCQ_STATS_PAD_BLANKS_LEN_LLU (12) //(16)
int nvmeib_dev_cq_stat_hdr(char *buffer, size_t len)
{
	int i;
	int count = 0;

#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)

	BUF_ADD("*\n%-*s| %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s ",
			16, "CQ [idx@dev]",
			SCQ_STATS_PAD_BLANKS_LEN_INT, "IRQno",
			SCQ_STATS_PAD_BLANKS_LEN_INT, "Core",
			SCQ_STATS_PAD_BLANKS_LEN_INT, "Chng",
			SCQ_STATS_PAD_BLANKS_LEN_INT, "QPs",
			SCQ_STATS_PAD_BLANKS_LEN_INT, "bgt i",
			SCQ_STATS_PAD_BLANKS_LEN_INT, "bgt p",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "intrs",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "sw2poll",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "tot comps",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "tot comps i",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "tot comps p",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "prv comps i",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "prv comps p",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "sw2p missed",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "sw2p del-qp",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "pend del-qp",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "try rearm i",
			SCQ_STATS_PAD_BLANKS_LEN_LLU, "try rearm p");

	for (i = ct_base + 1; i < ct_other; i++)
		BUF_ADD("| %*s", SCQ_STATS_PAD_BLANKS_LEN_INT, ch_type_to_str_long(i));

	BUF_ADD("\n");
#undef BUF_ADD

	return count;
}
EXPORT_SYMBOL(nvmeib_dev_cq_stat_hdr);

int nvmeib_dev_cq_stat(
		struct nvmeib_dev *dev, char *buffer, size_t len)
{
	struct nvmeib_dev_cq *cq;
	const char *s;
	u64 n_polls;
	char sign __attribute__((unused));
	int i, j;
	int count = 0;

#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	BUF_ADD("*\n");
	s = dev->ib_dev->name;
	mutex_lock(&dev->cqs_guard);
	for (i = 0; i < dev->n_cqs; ++i) {
		cq = &dev->cqs[i];
		n_polls = cq->n_intrs - cq->n_polls;
		if (cq->n_intrs > cq->n_polls)
			sign = '-';
		else {
			sign = '+';
			n_polls = - n_polls;
		}

		//SCQ-TODO: stats
		//Fix @n_polls or print sign
		//Add num comps in softirq
		//Add duration of ipoller sched-out vs. poll-handler

		BUF_ADD("%03d@%-*s| %*d | %*d | %*d | %*d | %*d | %*d | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu",
				i, 12 ,s,
				SCQ_STATS_PAD_BLANKS_LEN_INT, cq->intr,
				SCQ_STATS_PAD_BLANKS_LEN_INT, nvmeib_pcpu_cq_all_cpus ? cq->cpu_id_sched : cq->cpu_id,
				SCQ_STATS_PAD_BLANKS_LEN_INT, cq->n_cpu_change,
				SCQ_STATS_PAD_BLANKS_LEN_INT, cq->n_qps,
				SCQ_STATS_PAD_BLANKS_LEN_INT, cq->budget_intr,
				SCQ_STATS_PAD_BLANKS_LEN_INT, cq->iop.iop.weight,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_intrs,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_polls,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_comps_intr + cq->n_comps_poll + cq->n_comps_user,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_comps_intr,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_comps_poll,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_prev_comps_intr,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_prev_comps_poll,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_sw2p_missed_events,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_sw2p_del_qps_maint,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, (u64)cq->qps_n_del,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_rearm_intr,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_rearm_poll,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_rearm_fail,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_user_polls,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_comps_user,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_prev_comps_user,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_user_poll_arm,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_user_poll_wd,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_slow_poll_disable,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_wakeups_burst,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_wakeups_cycles,
				SCQ_STATS_PAD_BLANKS_LEN_LLU, cq->n_wakeups_irq_time
			);

		for (j = ct_base + 1; j < ct_other; j++)
			BUF_ADD("| %*d", SCQ_STATS_PAD_BLANKS_LEN_INT, cq->qp_ch[j]);
		BUF_ADD("\n");
	}
	mutex_unlock(&dev->cqs_guard);
#undef BUF_ADD
	return count;
}
EXPORT_SYMBOL(nvmeib_dev_cq_stat);

//SCQ-TODO: encap all stats in struct
int nvmeib_dev_cq_stat_reset(struct nvmeib_dev *dev)
{
	struct nvmeib_dev_cq *cq;
	int i;
	NFIN;

	_NT(t0_nvmeib_dev_cq_stat_reset,
		"reset pcpu-cq stats of dev @STR", dev->ib_dev->name);
	for (i = 0; i < dev->n_cqs; ++i) {
		cq = &dev->cqs[i];
		cq->n_intrs = 0;
		cq->n_polls = 0;
		cq->n_comps_intr = 0;
		cq->n_comps_poll = 0;
		cq->n_prev_comps_intr = 0;
		cq->n_prev_comps_poll = 0;
		cq->n_sw2p_missed_events = 0;
		cq->n_sw2p_del_qps_maint = 0;
		cq->n_rearm_intr = 0;
		cq->n_rearm_poll = 0;
		cq->n_rearm_fail = 0;
		cq->n_user_polls = 0;
		cq->n_comps_user = 0;
		cq->n_prev_comps_user = 0;
		cq->n_user_poll_arm = 0;
		cq->n_user_poll_wd = 0;
		cq->n_slow_poll_disable = 0;
	}

	NFOUT;
	return 0;
}
EXPORT_SYMBOL(nvmeib_dev_cq_stat_reset);

/* Wait for the processing of already polled WCs of this QP, to complete.
   Called from "net-free" after stopping rdma-events' processing */
void nvmeib_cq_qp_stop(struct nvmeib_dev_cq *cq, u64 qp_key)
{
	struct nvmeib_cq_qp_info *qpi;
	ulong flags;
	bool post_drain = false;
	int rv;

	_NI(nvmeib_cq_qp_stop_i1, "CQ @PTR, qp-key=0x@_X", cq, qp_key);

	if (NVMEIB_PCPU_CQ_DO_SQ_DRAIN_ON_QP_STOP) {
		struct ib_qp *qp = (void *)qp_key;
		struct ib_qp_attr qp_attr = {};
		struct ib_qp_init_attr init_attr = {};
		if ((rv = ib_query_qp(qp, &qp_attr, IB_QP_STATE, &init_attr)) < 0) {
			_NE(nvmeib_cq_qp_stop_e2, "ib_query_qp failed (@RV) for qp @QP", rv, qp);
		} else {
			post_drain = qp_attr.qp_state >= IB_QPS_RTS;
			_NT(nvmeib_cq_qp_stop_i4, "qp=@QP qp_num=@QP_NUM state=@QP_STATE post_drain=@BOOL_YN",
			    qp, qp->qp_num, qp_attr.qp_state, post_drain);
		}
	} else {
		(void)post_drain;
	}

	/* stop new polled-WCs of this QP from being processed */
	cq_qps_lock(cq, flags);
	qpi = radix_tree_delete(&cq->qps_live_tree, qp_key);
	if (qpi) {
		/* atomically, keep @qpi visible to is_qp_deleting_ (so it won't assert)
		   but at the same time invisiable or n/a for cq_qp_del_next_stage */
		_NI(nvmeib_cq_qp_stop_i2, "cq=@PTR, qpi=@PTR, qp-key=@_X -> drain @INT WCs (qps_n_del=@INT)",
		   cq, qpi, qpi->qp_key, nvmeib_ref_read(&qpi->n_processing) -1,
		   cq->qps_n_del);

		if (NVMEIB_PCPU_CQ_DO_SQ_DRAIN_ON_QP_STOP) {
			if (post_drain) {
				/* Inc ref_cnt +1 so we will also wait for the drain */
				BUG_ON(!nvmeib_ref_get(&qpi->n_processing));
			}
		}

		nvmeib_ref_release_start(&qpi->n_processing);
		list_add_tail(&qpi->sd_link, &cq->qps_stop_list);
		cq->qps_n_stop++;

		cq_qps_unlock(cq, flags);

		if (NVMEIB_PCPU_CQ_DO_SQ_DRAIN_ON_QP_STOP) {
			if (post_drain) {
				_NT(nvmeib_cq_qp_stop_i3, "cq=@DEV_CQ, qpi=@QPI, qp=@QP, qp=@QP_NUM, posting drain",
				    cq, qpi, (struct ib_qp *)qp_key, ((struct ib_qp *)qp_key)->qp_num);
				nvmeib_post_sq_drain((struct ib_qp *)qp_key);
			}
		}
	}
	else {
		_NE(nvmeib_cq_qp_stop_e1, "cq=@PTR, No such qp-key=0x@_X in live-tree", cq, qp_key);
		WARN_ON(1);
		cq_qps_unlock(cq, flags);
	}
}
EXPORT_SYMBOL(nvmeib_cq_qp_stop);

static void qpi_do_actions(struct nvmeib_dev_cq *cq,
						   struct nvmeib_cq_qp_info *qpi);

void nvmeib_cq_qp_del(struct nvmeib_dev_cq *cq, u64 qp_key,
					  struct list_head *qp_action_list)
{
	struct nvmeib_cq_qp_info *qpi, *t;
 	ulong flags;
	bool found;

 	_NI(nvmeib_cq_qp_del_i1, "CQ @PTR, qp-key=0x@_X -->", cq, qp_key);

	/* checks */
	if (in_interrupt() || irqs_disabled()) {
		_NE(nvmeib_cq_qp_del_e1, "Atomic but may sleep");
		BUG();
	}
	if (list_empty(qp_action_list)) {
		_NE(nvmeib_cq_qp_del_e2, "OOPS, qp-destory action is mandatory");
		BUG();
	}

	/* lookup qp-key in stop-list */
	cq_qps_lock(cq, flags);
	_NT(nvmeib_cq_qp_del_i2, "cq=@PTR, qps_n_stop=@INT", cq, cq->qps_n_stop);
	found = false;
	list_for_each_entry_safe(qpi, t, &cq->qps_stop_list, sd_link) {
		if (qpi->qp_key == qp_key) {
			found = true;
			break;
		}
	}
	cq_qps_unlock(cq, flags);
	if (!found) {
		_NE(nvmeib_cq_qp_del_e3, "cq=@PTR, No such qp-key=0x@_X in stop-list", cq, qp_key);
		WARN_ON(1);
		goto out;
	}

	_NT(nvmeib_cq_qp_del_i4, "cq=@PTR, qpi=@PTR, waiting release", cq, qpi);
	/* wait for wip wcs (and sq drain) */
	nvmeib_ref_release_wait(&qpi->n_processing);
	_NT(nvmeib_cq_qp_del_i5, "cq=@PTR, qpi=@PTR, release done", cq, qpi);

	/* start del sequence */
	cq_qps_lock(cq, flags);
	qpi->qp_ctx = (void *)CQ_QP_CTX_INVALID;
	list_splice_tail_init(qp_action_list, &qpi->action_list);
	cq->qps_n_stop--;
	list_del(&qpi->sd_link);       /* qps_stop_list */
	list_add_tail(&qpi->sd_link, &cq->qps_del_list_0);
	cq->qps_n_del++;
	if (cq->qps_n_del == 1)
		cq->qps_jif_list_0 = jiffies;
	cq_qps_unlock(cq, flags);

	if (!NVMEIB_PCPU_CQ_DEFER_RDMA_DESTROY) {
		qpi_do_actions(cq, qpi);
		/* skip to list3 - prevent qp-ptr reuse */
		cq_qps_lock(cq, flags);
		cq->qps_jif_list_0 = 0;
		list_del(&qpi->sd_link);
		if (NVMEIB_PCPU_CQ_DEFER_QPI_FREE)
			list_add_tail(&qpi->sd_link, &cq->qps_del_list_3);
		else {
			cq->qps_n_del--;
			kfree(qpi);
		}
		cq_qps_unlock(cq, flags);
	}

out:
	_NI(nvmeib_cq_qp_del_i3, "CQ @PTR, qp-key=0x@_X <--", cq, qp_key);
}
EXPORT_SYMBOL(nvmeib_cq_qp_del);

static void qpi_do_actions(struct nvmeib_dev_cq *cq,
						   struct nvmeib_cq_qp_info *qpi)
{
	struct nvmeib_cq_qp_action *a, *t;

	list_for_each_entry_safe(a, t, &qpi->action_list, action_link) {
		_NI(qpi_do_actions_i1, "cq=@PTR, qpi=@PTR, qp-key=@_X, run '@FN'",
		   cq, qpi, qpi->qp_key, a->f);
		list_del(&a->action_link);
		if (a->f) {
			/* after calling nvmeib_cq_qp_destroy_action_f -->
			   qp ptr is kfreed --> create-qp may alloc qp with
			   same ptr (i.e. qpi->qp_key) */
			a->f(a);
		}
		kfree(a);
	}
}

#define DEBUG_DEL_LISTS
static inline void cq_qp_del_lists_splice_tail(struct nvmeib_dev_cq *cq,
											   struct list_head *s,
											   struct list_head *d)
{
#ifdef DEBUG_DEL_LISTS
	struct nvmeib_cq_qp_info *qpi;
	while ((qpi = list_first_entry_or_null(s,
				struct nvmeib_cq_qp_info, sd_link))) {
		_NI(cq_qp_del_lists_splice_tail_i1, "cq=@PTR, qpi=@PTR, qp-key=@_X, 2->3", cq, qpi, qpi->qp_key);
		list_del(&qpi->sd_link);
		list_add_tail(&qpi->sd_link, d);
	}
#else
	/* we use splice's "tail" version for the case where @d is qps_del_list_1
	   which can be not empty and tail preserves order */
	list_splice_tail_init(s, d);
#endif
}

#define del_list_jif_reinit(_cq, _list_num) \
do { \
	_cq->qps_jif_##_list_num = \
	list_empty(&_cq->qps_del_##_list_num) ? 0 : jiffies; \
} while (0)

#define NVMEIB_PCPU_CQ_DEL_LIST_MAX_SIZE 64
#define NVMEIB_PCPU_CQ_DEL_LIST_MAX_TIME (5 * HZ)
#define del_list_is_timeout(_cq, _list_num) \
	((_cq->qps_jif_##_list_num) && \
	((jiffies - (_cq->qps_jif_##_list_num) > NVMEIB_PCPU_CQ_DEL_LIST_MAX_TIME)))

bool nvmeib_pcpu_cq_flush_del_qps = false;
module_param_named(pcpu_cq_flush_del_qps, nvmeib_pcpu_cq_flush_del_qps, bool, 0644);
MODULE_PARM_DESC(pcpu_cq_flush_del_qps, "percpu cqs flush QPs pending for deletion (bool). Do not change with consulting support.");

static inline bool is_del_list_time(struct nvmeib_dev_cq *cq)
{
	return (nvmeib_pcpu_cq_flush_del_qps ||
			cq->qps_n_del > NVMEIB_PCPU_CQ_DEL_LIST_MAX_SIZE ||
			(false &&
			 (del_list_is_timeout(cq, list_0) ||
			  del_list_is_timeout(cq, list_1) ||
			  del_list_is_timeout(cq, list_2) ||
			  del_list_is_timeout(cq, list_3))));
}

/* Called after polling ZERO cqs or on CQ free
   @is_timeout : True if last poll found wcs */
static void cq_qp_del_next_stage(struct nvmeib_dev_cq *cq)
{
	struct nvmeib_cq_qp_info *qpi;
	ulong flags;
	int i = 0;

	cq_qps_lock(cq, flags);

	if (cq->qps_del_wip) {
		/* func is expected to be triggered by either poll-thread or
		   cq_qps_drain where later sets qps-draining flag beforehand */
		if (!cq->qps_draining) {
			_NE_dmesg(cq_qp_del_next_stage_panic,
					  "CQ @PTR: unexpected qp-del concurrency, "
					  "called from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
					  cq, __builtin_return_address(0));
			BUG();
		}
		_NI(cq_qp_del_next_stage_i0,
			"CQ @PTR: qp-del already in-progress, try later", cq);
		goto unlock;
	}
	cq->qps_del_wip = true;

	if (!NVMEIB_PCPU_CQ_DEFER_QPI_FREE) {
		if (!list_empty(&cq->qps_del_list_3)) {
			_NE(cq_qp_del_next_stage_e1,
			    "CQ @PTR: unexp non-empty list(s)", cq);
			BUG();
		}
	} else {
		_NI(cq_qp_del_next_stage_i1, "CQ @PTR: 3->Free", cq);
		while ((qpi = list_first_entry_or_null(&cq->qps_del_list_3,
					struct nvmeib_cq_qp_info, sd_link))) {
			_NI(cq_qp_del_next_stage_i2, "cq=@PTR, qpi=@PTR, qp-key=@_X, 3->Free", cq, qpi, qpi->qp_key);
			cq->qps_n_del--;
			list_del(&qpi->sd_link);
			kfree(qpi);
		}
	}

	if (!NVMEIB_PCPU_CQ_DEFER_RDMA_DESTROY) {
		/* dont process other lists...*/
		if (!list_empty(&cq->qps_del_list_1) ||
			!list_empty(&cq->qps_del_list_2)) {
			_NE(cq_qp_del_next_stage_e0,
				"CQ @PTR: unexp non-empty list(s)", cq);
			BUG();
		}
		goto done;
	}

	_NT(cq_qp_del_next_stage_i3, "CQ @PTR: 2->3", cq);
	cq_qp_del_lists_splice_tail(cq, &cq->qps_del_list_2, &cq->qps_del_list_3);
	del_list_jif_reinit(cq, list_3);

	_NT(cq_qp_del_next_stage_i4, "CQ @PTR: 1->2 (partial)", cq);
	while ((qpi = list_first_entry_or_null(&cq->qps_del_list_1,
				struct nvmeib_cq_qp_info, sd_link)) &&
		   (cq->qps_draining || i++ < CQ_QP_DEL_STAGE_MAX_DESTROYS)) {
		_NT(cq_qp_del_next_stage_i5, "cq=@PTR, qpi=@PTR, qp-key=@_X, 1 -> 2", cq, qpi, qpi->qp_key);
		list_del(&qpi->sd_link);
		list_add_tail(&qpi->sd_link, &cq->qps_del_list_2);
	}
	del_list_jif_reinit(cq, list_2);
	cq_qps_unlock(cq, flags);

	/* call actions, amongst which is nvmeib_cq_qp_destroy_action_f,
	   while qpi remains in lists thus visible to is_qp_deleting_ */
	/* we're ok with traversing qps_del_list_2 without a lock as only this func
	   modifies this list && we prevent concurrency via cq->qps_del_wip above */
	list_for_each_entry(qpi, &cq->qps_del_list_2, sd_link) {
		qpi_do_actions(cq, qpi);
	}

	cq_qps_lock(cq, flags);

	_NT(cq_qp_del_next_stage_i6, "CQ @PTR: 0->1", cq);
	/* here list1 may be not-empty */
	cq_qp_del_lists_splice_tail(cq, &cq->qps_del_list_0, &cq->qps_del_list_1);
	del_list_jif_reinit(cq, list_1);
	cq->qps_jif_list_0 = 0;

done:
	cq->qps_del_wip = false;
unlock:
	cq_qps_unlock(cq, flags);
}

/* Called from:
 * nvmeib_dev_drain_cqs <- ... <- remove_nis  <- remove_one <- remove_ib <- nvmeibs_exit <- //
 *                                            <- nvmeibs_remove_ib_device <- remove_ib_device_work  <- exec_nvmeibs_remove_ib_device <- nvmeibs_ib_port_event_handler IB_EVENT_DEVICE_FATAL <- //
 * nvmeib_free 		    <- ... <- remove_nis
 */
static void cq_qps_drain(struct nvmeib_dev_cq *cq)
{
	struct radix_tree_iter iter;
	void **slot;
	struct nvmeib_cq_qp_info *qpi;
	bool err = false;
	ulong flags;

	/* DEBUG: check tree and stop-list are empty */
	cq_qps_lock(cq, flags);
	//BUG_ON(cq->qps_draining);
	if (cq->qps_draining)
		_NI(cq_qps_drain_i0, "CQ @PTR already draining", cq);
	cq->qps_draining = true;

	_NT(cq_qps_drain_i1, "CQ @PTR, scan live tree...", cq);
	radix_tree_for_each_slot(slot, &cq->qps_live_tree, &iter, 0) {
		//??? = radix_tree_deref_slot(slot); backing-dev.c
		qpi = *slot;
		_NE(cq_qps_drain_e1, "qp-key=0x@_X, qp-ctx=@PTR ", qpi->qp_key, qpi->qp_ctx);
		//WARN_ON(!radix_tree_delete(&cq->qps_live_tree, qpi->qp_key));
		err = true;
	}
	_NT(cq_qps_drain_i2, "CQ @PTR, scan stop-list...", cq);
	list_for_each_entry(qpi, &cq->qps_stop_list, sd_link) {
		_NE(cq_qps_drain_e2, "qp-key=0x@_X, qp-ctx=@PTR", qpi->qp_key, qpi->qp_ctx);
		err = true;
	}
	BUG_ON(err);
	cq_qps_unlock(cq, flags);

	while (cq->qps_n_del) {
		_NT(cq_qps_drain_i3, "CQ @PTR, drain n_qps=@INT", cq, cq->qps_n_del);
		cq_qp_del_next_stage(cq);
	}
}

/* send comp w/ err also arrive with this wc->opcode... */
#define is_wc_recv(_wc) ((_wc->opcode & IB_WC_RECV) && \
						 (nvmeib_opcode_from_wc(_wc) == NVMEIB_RECV))

#if defined(DEBUG_SCQ_IU_OWNER) && (DEBUG_SCQ_IU_OWNER==1)
static void iu_owner_switch(struct ib_wc *wc,
							struct nvmeib_srq_info *srq_info)
{
	u32 index;
	struct nvmeib_iu *iu;

	if (is_wc_recv(wc)) {
		index = nvmeib_idx_from_wc(wc);
		iu = nvmeib_srq_rtrv_recv(srq_info, index, NULL);
		nvmeib_iu_owner_hw2sw(iu, srq_info, err_iu_owner_switch_hw2sw);
	}
}
#else
#define iu_owner_switch(n, wcs)
#endif

static void post_orphan_recv(struct nvmeib_dev_cq *cq, struct ib_wc *wc)
{
	u32 index = nvmeib_idx_from_wc(wc);
	struct nvmeib_iu *recv_ioctx = nvmeib_srq_rtrv_recv(cq->srq_info, index, NULL);

	if (recv_ioctx) {
		_NT(post_orphan_recv_i1, "cq=@PTR, wc=@PTR", cq, wc);
		nvmeib_srq_post_recv(cq->srq_info, recv_ioctx);
	} else
		_NE(post_orphan_recv_e1, "OOPS, No recv-ioctx for wc=@PTR, index=@INT", wc, index);

}

static void process_wcs(struct nvmeib_dev_cq *cq, struct ib_wc *wcs, int n_wcs)
{
	struct nvmeib_cq_qp_info *qpi;
	struct ib_wc *wc;
	bool is_drain;

	/* Use a sliding window approach to process the completions.
	Each iteration moves wc forward by 1 and reduces n_wcs by 1.
	Additionally we use a look-forward loop and bitmask to batch all WCs for a single qp to the process function.
	For simplicity, we stop the look-forward when we encounter a Drain WC.
	*/

	for (wc = wcs; n_wcs > 0; n_wcs--, wc++) {
		if (is_wc_qp_key_cleared(wc)) {
			/* Skip completions that are already processed */
			continue;
		}

		is_drain = nvmeib_opcode_from_wr_id(wc->wr_id) == NVMEIB_DRAIN_QUEUE;
		BUG_ON(!NVMEIB_PCPU_CQ_DO_SQ_DRAIN_ON_QP_STOP && is_drain);

		qpi = cq_qp_ref_get(cq, wc, is_drain);

		if (qpi) {
			if (!is_drain) {
				DECLARE_BITMAP(wcs_mask, CQ_POLL_BATCH) = {0};
				int i;
				/* Start Dev-CQ interrupt measurement - assume all noise since core-mask is not known */
				nvmeib_completion_noise_start(NVMEIB_NOISE_COMPLETION);
				/* Look forward to see how many completions we have for this qp and set the mask */
				set_bit(0, wcs_mask);
				for (i = 1; i < n_wcs; i++) {
					if (wc[i].qp == wc->qp) {
						if (nvmeib_opcode_from_wr_id(wc[i].wr_id) == NVMEIB_DRAIN_QUEUE) {
							break;
						}
						set_bit(i, wcs_mask);
					}
				}
				/* process_per_dev_cq processes all completions in the mask */
				cq->process(wc, n_wcs, wcs_mask, qpi->qp_ctx);

				/* Clear all completions processed from the mask */
				for_each_set_bit(i, wcs_mask, n_wcs) {
					clear_wc_qp_key(&wc[i]);
				}
			} else {
				_NT(nvmeib_cq_process_wc, "cq=@DEV_CQ, qpi=@QPI, qp=@QP, qpn=@QP_NUM, got sq drain",
					cq, qpi, wc->qp, wc->qp->qp_num);
			}
			cq_qp_ref_put(qpi);
		}
		else {
			if (is_drain) {
				_NE(err_nvmeib_cq_process_wc, "cq=@DEV_CQ, qp=@QP, qpn=@QP_NUM, no qpi for sq drain",
					cq, wc->qp, wc->qp->qp_num);
			}
			else if (is_wc_recv(wc)) {
				post_orphan_recv(cq, wc);
			}
		}
	}
}

static int process_cq(struct nvmeib_dev_cq *cq, int budget)
{
	int n, completed = 0;
	int batch_size = ARRAY_SIZE(cq->wcs);

	if (budget < batch_size)
		batch_size = budget;
	while ((n = ib_poll_cq(cq->cq, batch_size, cq->wcs)) > 0) {

#if defined(DEBUG_SCQ_IU_OWNER) && (DEBUG_SCQ_IU_OWNER==1)
		for (i = 0; i < n ; i++)
			iu_owner_switch(&cq->wcs[i], cq->srq_info);
#endif
		process_wcs(cq, cq->wcs, n);

		completed += n;
#ifdef CQ_DEBUG
		cq->n_completions += n;
		if ((cq->n_completions % CQ_N_COMP_TRACE) == 0)
			_NI(process_cq_i1,"Dev @STR - cq[@LONG] has @INT64 completions",
				cq->dev->ib_dev->name, cq_index(cq), cq->n_completions);
#endif
		//omril: how can budget change??
		if (n != batch_size || (budget != -1 && completed >= budget))
			break;
	}

	return completed;
}

static inline bool cq_is_polling_exp_mode(struct nvmeib_dev_cq *cq, enum nvmeib_dev_cq_poll_mode poll_mode)
{
	unsigned long flags;
	int cpu = raw_smp_processor_id();
	bool rv = true;

	if (nvmeib_pcpu_cq_all_cpus) {
		WARN_ON_ONCE(in_interrupt());
		cq_lock(cq, flags);
		if (cq->poll_mode != poll_mode) {
			_NI_dmesg(i0_cq_is_pollin_exp_mode,
				  "DEV CQ: @DEV_CQ, poll handler called in invalid mode @POLL_MODE (not @POLL_MODE)",
				  cq, poll_mode, cq->poll_mode);
			rv = false;
		} else if (cpu != cq->cpu_id_sched) {
			_NE_dmesg(e0_cq_is_polling_exp_mode,
					  "Oops, CQ running on cpu @INT, exp. @INT "
					  "(poll_mode=@POLL_MODE, cq->poll_mode=@POLL_MODE)",
					cpu, cq->cpu_id_sched, poll_mode, cq->poll_mode);
			WARN_ON_ONCE(1);
		}
		cq_unlock(cq, flags);
		return rv;
	}

	/* interrupt/poller ctx had rearm interrupts, but due to missed-events
	   decided to switch-to/keep polling, while leaving/marking is_polling=1
	   and unlocked cq. Then, interrupt fired on different core and may have
	   already managed to change cq's cpu-id */

	cq_lock(cq, flags);

	/* log the event and cpu-change before we may decide to bail */
	if (cpu != cq->cpu_id) {
		if (cq->cpu_id == -1) {
			_NI_dmesg(t0_cq_is_polling_exp_mode,
					  "cq=@PTR, init cpu to @INT", cq, cpu);
		}
		else {
			_NI_dmesg(t1_cq_is_polling_exp_mode,
					  "cq=@PTR changed cpu, exp=@INT --> now=@INT (n=@INT), "
					  "poll_mode=@POLL_MODE, irqbalance or affinity?",
						cq, cq->cpu_id, cpu, cq->n_cpu_change, poll_mode);
		}
		cq->n_cpu_change++;
		cq->cpu_id = cpu;
	}

	if (cq->poll_mode != poll_mode) {
		_NI(w0_cq_is_polling_exp_mode,
			"cq=@PTR, oops, cq->poll_mode=@POLL_MODE but is_poller=@POLL_MODE, bail!",
			cq, cq->poll_mode, poll_mode);
		//WARN_ON(1);
		rv = false;
	}

	cq_unlock(cq, flags);

	return rv;
}

static int rearm_or_resched(struct nvmeib_dev_cq *cq, enum nvmeib_dev_cq_poll_mode exp_poll_mode)
{
	unsigned long flags;
	bool missed_events = false;
	bool del_qps_maint = false;
	bool enable_poll = false;

	cq_lock(cq, flags);
	if (cq->poll_mode != exp_poll_mode) {
		cq->n_rearm_fail++;
		cq_unlock(cq, flags);
		return -EBUSY;
	}
	if (cq->poll_mode == NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE) {
		enable_poll = true;
	}
	if (enable_poll ||
		(missed_events = (ib_req_notify_cq(cq->cq, IB_POLL_FLAGS) > 0)) ||
		(del_qps_maint = is_del_list_time(cq))) {
		/* rearmed but decided to switch2/keep polling,
		   next interrupt may fire while we are/will-be
		   in polling and it will bail (ignored) */
		cq->n_sw2p_missed_events += missed_events;
		cq->n_sw2p_del_qps_maint += del_qps_maint;

		cq->poll_mode = NVMEIB_DEV_CQ_POLL_MODE;
		cq_unlock(cq, flags);
		if (enable_poll) {
			__poll_enable(&cq->iop);
		}
		__poll_sched(&cq->iop, nvmeib_pcpu_cq_all_cpus ? cq->cpu_id_sched : smp_processor_id());
	}
	else {
		cq->poll_mode = NVMEIB_DEV_CQ_INTR_MODE;
		cq_unlock(cq, flags);
	}
	return 0;
}

/* @param budget is IB_INTR_POLL_BUDGET_IRQ (256) */
static int ib_poll_handler(struct irq_poll *iop, int budget)
{
	struct nvmeib_irq_poll *nviop = container_of(iop, struct nvmeib_irq_poll, iop);
	struct nvmeib_dev_cq *cq = container_of(nviop, struct nvmeib_dev_cq, iop);
	int completed;
	bool poll_linger = false;
	u64 start_ns, busy_ns;
	bool continue_polling = false;
	
	nviop->poll_linger = false;
	
	if (!cq_is_polling_exp_mode(cq, NVMEIB_DEV_CQ_POLL_MODE))
		return 0;

	if (nvmeib_pcpu_cq_poll_budget != iop->weight) {
		int weight = nvmeib_pcpu_cq_poll_budget;
		_NI_dmesg(trace_ib_poll_handler, "Dev @STR, CQ @INT, comp-vec=@INT, core=@INT, UNSAFE change poll-weight of iop=@PTR: @INT -> @INT",
				  cq->dev->ib_dev->name, (int)(cq - cq->dev->cqs), cq->intr, cq->cpu_id, iop, iop->weight, weight);

		iop->weight = weight; //SCQ-TODO: move this to nvmeib_public_intr_poll_modify
		budget = iop->weight; //update before using so it is in sync with caller's complementary cond
	}
	start_ns = local_clock();
	completed = process_cq(cq, budget);
	busy_ns = local_clock() - start_ns;
	if (completed)
		nviop->last_nonempty_comp_jif = jiffies;
	cq->n_comps_poll += completed;
	cq->n_prev_comps_poll = completed;

	/* Prefer calling destroy-qp when cq is idle (zero polled) as then
	   destroy-qp's draining of cq from that qp's comps shall be nop.
	   On the other hand, cq can be very busy and qps may pile up so we
	   add size-based and time-based trigger/threshold */
	if (completed == 0 || is_del_list_time(cq))
		cq_qp_del_next_stage(cq);

	//SCQ-TODO: we also need something to trigger cleanup when no comps - e.g.:
	//1. CQ has no nrch that does iopath-ka
	//2. When we deatch and there is no IO, qps will not be cleaned...

	/* This cond is complementary to cond that
	   caller (ipoller_run) checks on retun */
	if (nvmeib_pcpu_process_cq_retry_usecs && completed < budget) {
		unsigned long retry_delta = usecs_to_jiffies(nvmeib_pcpu_process_cq_retry_usecs);
		unsigned long now = jiffies;
		/* If we had any completion in this run or recently within retry window,
		 * keep tail-polling to reduce rearm churn. */
		if (time_before(now, nviop->last_nonempty_comp_jif + retry_delta)) {
			poll_linger = true;
		}
	}

	/* Continue polling if: interrupt budget is 0 OR shaper says continue polling */
	continue_polling = cq->budget_intr == 0 || nvmeib_intr_shaper_should_continue_polling(nvmeib_intr_shaper, completed, busy_ns);

	if (poll_linger) {
		nviop->poll_linger = true;
	} else if (!completed || !continue_polling) {
		/* Stop polling if: no completions OR continue_polling is false */
		__poll_complete(&cq->iop);
		rearm_or_resched(cq, NVMEIB_DEV_CQ_POLL_MODE);
		cq->n_rearm_poll++;
	} else if (completed && cq->budget_intr == 0) {
		nviop->poll_linger = true; /* allow poller one more time to poll */
	}

	return completed;
}

#	if defined(IO_POLL_THREAD) && IO_POLL_THREAD
/* @param budget is CQ_INTR_PROCESS_BATCH (4) */
static int ib_poll_handler_intr(struct nvmeib_irq_poll *iop, int budget)
{
	struct nvmeib_dev_cq *cq = container_of(iop, struct nvmeib_dev_cq, iop);
	int completed;

	if (!cq_is_polling_exp_mode(cq, NVMEIB_DEV_CQ_INTR_MODE))
		return 0;

	completed = process_cq(cq, budget);
	cq->n_comps_intr += completed;
	cq->n_prev_comps_intr = completed;

	if (completed < budget) {
		/* cq is not busy, basically we want to rearm interrupts */
		//TBD:
		//dont rearm-or-resched, make caller call this func once more or
		//until there's is no time/retries budget; do @completed=@budget.
		//(sanity check that completed > 0)
		rearm_or_resched(cq, NVMEIB_DEV_CQ_INTR_MODE);
		cq->n_rearm_intr++;
	}

	return completed;
}
#	endif
#endif /* CQ_POLL_INTR */

static int user_poll_handler_intr(struct nvmeib_dev_cq *cq, int budget)
{
	int completed;
	unsigned long flags;
	bool disable_ipoller = false;
	int rv = 0;

	if (unlikely(smp_processor_id() != cq->cpu_id_sched)) {
		_NW(warn_user_poll_handler_intr_inv_cpu,
		    "Invalid CPU @CPU for CQ @DEV_CQ - Should be @CPU", smp_processor_id(), cq, cq->cpu_id_sched);
		rv = -EINVAL;
		goto out;
	}

	cq_lock(cq, flags);
	switch (cq->poll_mode) {
	case NVMEIB_DEV_CQ_POLL_DISABLED:
		rv = -EBUSY;
		break;
	case NVMEIB_DEV_CQ_INTR_MODE:
	case NVMEIB_DEV_CQ_POLL_MODE:
		disable_ipoller = true;
		cq->poll_mode = NVMEIB_DEV_CQ_ENTER_USER_POLL_MODE;
		break;
	case NVMEIB_DEV_CQ_USER_POLL_MODE:
		cq->user_poll.last_poll_jif = jiffies;
		break;
	case NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE:
		/* WD routine is in the process of re-enabling ipoller.
		 * The next time the user polls, it will switch back to user-polling.
		 Question: Should we return 0 instead? */
		rv = -EAGAIN;
		break;
	default:
		BUG();
	}

	if (rv < 0) {
		cq_unlock(cq, flags);
		goto out;
	}

	/* will poll_cq soon */
	cq->user_poll_process = true;
	cq_unlock(cq, flags);

	if (disable_ipoller) {
		unsigned long start_jif = jiffies, end_jif;

		_NT(trace_2_user_poll_handler_intr,
		    "dev cq @DEV_CQ - disabling ipoller poll", cq);

		del_timer_sync(&cq->user_poll.poll_wd_timer);
		__poll_disable(&cq->iop);

		end_jif = jiffies;

		cq_lock(cq, flags);
		if (end_jif > start_jif + msecs_to_jiffies(POLL_DISABLE_TIMEOUT_TRACE_MSECS)) {
			_NT(trace_user_poll_handler_intr,
			    "dev cq @DEV_CQ - __poll_disable took @JIFFIES_TO_MSECS ms",
				cq, jiffies_to_msecs(end_jif - start_jif));
			cq->n_slow_poll_disable++;
		}
		BUG_ON(cq->poll_mode != NVMEIB_DEV_CQ_ENTER_USER_POLL_MODE);
		cq->poll_mode = NVMEIB_DEV_CQ_USER_POLL_MODE;
		cq->n_user_poll_arm++;
		cq->user_poll.last_poll_jif = jiffies;
		cq_unlock(cq, flags);

		/* Start timer for user-poll WD */
		_NT(trace_3_user_poll_handler_intr,
		    "dev cq @DEV_CQ - starting WD timer", cq);

		cq->user_poll.poll_wd_timer.expires = jiffies + msecs_to_jiffies(USER_POLL_TIMEOUT_GRANULARITY_MSECS);

		BUG_ON(!nvmeib_pcpu_cq_all_cpus);
		add_timer_on(&cq->user_poll.poll_wd_timer, cq->cpu_id_sched);
	}

	completed = process_cq(cq, budget);
	cq_lock(cq, flags);
	cq->user_poll_process = false;
	cq_unlock(cq, flags);
	cq->n_comps_user += completed;
	cq->n_prev_comps_user = completed;
	cq->n_user_polls++;
	rv = completed;

	if (completed) {
		_ND(trace_4_user_poll_handler_intr,
		"dev cq @DEV_CQ - polled @COUNT completions", cq, completed);
	}

out:
	return rv;
}

static void cq_completion_intr(struct ib_cq *cq, void *v)
{
#ifdef CQ_POLL_INTR
	struct nvmeib_dev_cq *cqw = v;
#	if defined(IO_POLL_THREAD) && IO_POLL_THREAD
	int max_restart = CQ_INTR_PROCESS_MAX_RESTART;
	int completed;
	bool cq_not_empty;
	int budget;
	enum nvmeib_intr_shaper_calc_ret wake_up_reason = NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP;
	unsigned long flags;

	nvmeib_intr_shaper_intr_enter(nvmeib_intr_shaper, INTR_SHAPER_INTR_TYPE_DEV_CQ);
	nvmeib_completion_noise_start(NVMEIB_NOISE_INTERRUPT);

	++cqw->n_intrs;

	//TBD: merge with sched logic below...

	/* Can CQ already sched on (ipoller of) cpu0 and then get intr on cpu1
	   (affinity changed by daemon or admin) and thus sched to cpu1 ??? */
	if (nvmeib_pcpu_cq_all_cpus) {
		bool sched_poll = false;

		cq_lock(cqw, flags);
		switch (cqw->poll_mode) {
		case NVMEIB_DEV_CQ_POLL_DISABLED:
			_NI_dmesg(e1_cq_completion_intr,
				  "Dev CQ @DEV_CQ - Interrupt while disabled", cqw);
			break;
		case NVMEIB_DEV_CQ_INTR_MODE:
			cqw->poll_mode = NVMEIB_DEV_CQ_POLL_MODE;
			sched_poll = true;
			break;
		case NVMEIB_DEV_CQ_POLL_MODE:
		case NVMEIB_DEV_CQ_USER_POLL_MODE:
			_NE_dmesg(e2_cq_completion_intr,
				  "Dev CQ @DEV_CQ - Interrupt while already polling (poll_mode @POLL_MODE)", cqw, cqw->poll_mode);
			break;
		case NVMEIB_DEV_CQ_ENTER_USER_POLL_MODE:
		case NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE:
			_NI_dmesg(e3_cq_completion_intr,
				  "Dev CQ @DEV_CQ - Interrupt while in transition state (poll_mode @POLL_MODE)", cqw, cqw->poll_mode);
				  break;
		default:
			BUG();
			break;
		}
		cq_unlock(cqw, flags);

		if (sched_poll) {
			++cqw->n_polls;
			__poll_sched(&cqw->iop, cqw->cpu_id_sched);
		}
		goto out;
	} else {
		bool do_intr_poll = false;

		cq_lock(cqw, flags);
		switch (cqw->poll_mode) {
		case NVMEIB_DEV_CQ_POLL_DISABLED:
			_NI_dmesg(e0_cq_completion_intr,
				  "Dev CQ @DEV_CQ - Interrupt while disabled", cqw);
			break;
		case NVMEIB_DEV_CQ_INTR_MODE:
			do_intr_poll = true;
			break;
		case NVMEIB_DEV_CQ_POLL_MODE:
			_NI_dmesg(i0_cq_completion_intr,
				  "Dev CQ @DEV_CQ - Interrupt while already-polling", cqw);
			break;
		case NVMEIB_DEV_CQ_USER_POLL_MODE:
		case NVMEIB_DEV_CQ_ENTER_USER_POLL_MODE:
		case NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE:
			_NE_dmesg(e4_cq_completion_intr,
				  "Dev CQ @DEV_CQ - Interrupt while in invalid mode (poll_mode @POLL_MODE)", cqw, cqw->poll_mode);
			BUG();
			break;
		default:
			BUG();
			break;
		}
		cq_unlock(cqw, flags);

		if (!do_intr_poll)
			goto out;
	}

restart:

	cqw->budget_intr = budget = nvmeib_pcpu_cq_intr_budget;
	if (budget && !nvmeib_intr_shaper_intr_should_wake_up_reason(nvmeib_intr_shaper, &wake_up_reason)) {
		completed = ib_poll_handler_intr(&cqw->iop, budget);
		nvmeib_intr_shaper_intr_polled(nvmeib_intr_shaper, completed);
		while ((cq_not_empty = (completed >= budget)) &&
			!nvmeib_intr_shaper_intr_should_wake_up_reason(nvmeib_intr_shaper, &wake_up_reason) &&
			--max_restart)
			goto restart;
	} else {
		cq_not_empty = true;
	}

	if (cq_not_empty) {
		/* on last poll-cq we polled n=budget CQEs and thus did not rearm interrupts (nor schedule poll).
		* If dev-cq polling has not been disabled (bring-down), then schedule the polling thread
		* [we dont really need the cq-lock as we haven't waken up poller, but for future]
		* we are in interrupt, poller is not running (yet)
		*/

		cq_lock(cqw, flags);
		switch (wake_up_reason) {
		case NVMEIB_INTR_SHAPER_RET_WAKE_UP_BURST:
			cqw->n_wakeups_burst++;
			break;
		case NVMEIB_INTR_SHAPER_RET_WAKE_UP_IRQ_TIME:
			cqw->n_wakeups_irq_time++;
			break;
		case NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES:
			cqw->n_wakeups_cycles++;
			break;
		default:
			BUG_ON(max_restart > 0);
			break;
		}
		if (cqw->poll_mode == NVMEIB_DEV_CQ_POLL_DISABLED) {
			cqw->n_rearm_fail++;
			cq_unlock(cqw, flags);
			goto out;
		}
		BUG_ON(cqw->poll_mode != NVMEIB_DEV_CQ_INTR_MODE);
		cqw->poll_mode = NVMEIB_DEV_CQ_POLL_MODE;
		cq_unlock(cqw, flags);

		++cqw->n_polls;
		__poll_sched(&cqw->iop, smp_processor_id());
	}
	else {
		/* on last poll-cq we polled n<budget CQEs and thus attempted to rearm interrupts
		* but may have scheduled the polling thread due to missed-events */
		cq_lock(cqw, flags);
		if (cqw->poll_mode == NVMEIB_DEV_CQ_POLL_MODE) {
			/* Polling was scheduled due to missed-events */
			++cqw->n_polls;
		}
		else {
			//interrupts were rearmed, sw2polling is forbiden now!!!
		}
		cq_unlock(cqw, flags);
	}
#	else
	__poll_sched(&cqw->iop, nvmeib_pcpu_cq_all_cpus ? cqw->cpu_id_sched : smp_processor_id());
#	endif
#else
	BUG();
#endif

out:
	nvmeib_completion_noise_end(NVMEIB_NOISE_INTERRUPT, NULL, 0, NVMEIB_NOISE_CTRS_CQ_INTR);
	nvmeib_intr_shaper_intr_exit(nvmeib_intr_shaper);
}

static void print_cq(struct nvmeib_dev_cq *cq, int ii)
{
	int count;
	char *b;
	int l;
	enum channel_type i;

	if (cq->n_qps) {
		b = cq->trace_buf;
		l = sizeof(cq->trace_buf);
		memset(b, 0, l);
		count = 0;
		count += scnprintf(b + count, l - count,
			"dev %s - CQ[%d] has %d QPs", cq->dev->ib_dev->name, ii, cq->n_qps);
		for (i = ct_base + 1; i < ct_end; ++i)
			count += scnprintf(b + count, l - count,
				", %s %3d", ch_type_to_str(i), cq->qp_ch[(int)i]);
		_NT(print_cq_t1, "@STR", b);
	}
}

struct nvmeib_dev_cq *nvmeib_cq_get(struct nvmeib_dev *dev,
	enum channel_type type, DEV_CQ_PROCESS_FUNC((*process)), bool is_mostly_idle, int comp_cpu)
{
	struct nvmeib_dev_cq *cq, *next_cq = NULL;
	unsigned min_n_qps = -1;
	int i;

	NFIN;
	/* sanity */
	if (!dev->cqs) {
		_NT(nvmeib_cq_get_t1, "Dev @STR has no CQs, bail", dev->ib_dev->name);
		goto out;
	}

	if (comp_cpu >= 0) {
		_NI_dmesg(trace_nvmeib_cq_get_comp_cpu, "Allocating cq with comp-cpu=@INT", comp_cpu);
	}


	mutex_lock(&dev->cqs_guard);
	for (i = 0; i < dev->n_cqs; ++i) {
		cq = &dev->cqs[i];
#ifdef CQ_POLL_INTR
		if (cq->cq && !IS_ERR(cq->cq)) {
#else
			BUG();
#endif
			if (comp_cpu >= 0) {
				int cpu = nvmeib_pcpu_cq_all_cpus ? cq->cpu_id_sched : cq->cpu_id;
				if (cpu == comp_cpu) {
					next_cq = cq;
					break;
				}
			}

			if (cq->n_qps < min_n_qps) {
				min_n_qps = cq->n_qps;
				next_cq = cq;

				if (is_mostly_idle) {
					_NI_dmesg(nvmeib_cq_get_t1_iii, "Dev @STR select cq @INT for mostly-idle ch, ct=@STR",
							  dev->ib_dev->name, i, ch_type_to_str(type));
					break;
				}
			}
		}
	}
	mutex_unlock(&dev->cqs_guard);

	if (comp_cpu >= 0 && next_cq == NULL) {
		_NI_dmesg(error_nvmeib_cq_get_comp_cpu, "Did not find cq with comp-cpu=@INT", comp_cpu);
		goto out;
	}


	BUG_ON(next_cq == NULL);

	mutex_lock(&next_cq->guard);
	if (++next_cq->n_qps == 1) {
		next_cq->process = process;
	}
	_ND(nvmeib_cq_get_d1, "Adding QP of type @STR to CQ[@LONG]",
	   ch_type_to_str(type), cq_index(next_cq));
	if (type < ct_other)
		++next_cq->qp_ch[type];
	else
		++next_cq->qp_ch[ct_other];
	_NT(nvmeib_cq_get_t2, "Dev @STR - has @INT CQs",
		dev->ib_dev->name, dev->n_cqs);
	for (i = 0; i < dev->n_cqs; ++i)
		print_cq(&dev->cqs[i], i);
	mutex_unlock(&next_cq->guard);

out:
	NFOUT;
	return next_cq;

}
EXPORT_SYMBOL(nvmeib_cq_get);

void nvmeib_cq_put(struct nvmeib_dev *dev, struct nvmeib_dev_cq *cq,
	enum channel_type type)
{
	int i;

	NFIN;
	mutex_lock(&cq->guard);
	if (--cq->n_qps == 0) {
		cq->process = NULL;
	}
	else if (cq->n_qps < 0)
		_NE(nvmeib_cq_put_e1, "Dev @STR - integrity issue on completrion "
							  "object @PTR - n_qps=@INT",
			dev->ib_dev->name, cq, cq->n_qps);
	_NT(nvmeib_cq_put_t1, "Dev @STR - has @INT CQs",
		dev->ib_dev->name, dev->n_cqs);
	if (type < ct_other)
		--cq->qp_ch[type];
	else
		--cq->qp_ch[ct_other];
	for (i = 0; i < dev->n_cqs; ++i)
		print_cq(&dev->cqs[i], i);
	mutex_unlock(&cq->guard);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_cq_put);

static void free_cq(struct nvmeib_dev *dev, struct nvmeib_dev_cq *cq)
{
	NFIN;
	_NT(free_cq_i1, "Dev @STR, free dev-cq @PTR, n_qps=@INT", dev->ib_dev->name, cq, cq->n_qps);

	if (cq->cq && !IS_ERR(cq->cq)) {
		ib_destroy_cq(cq->cq);
	}
	cq->cq = NULL;
	NFOUT;
}

static void free_cqs(struct nvmeib_dev *dev)
{
	struct nvmeib_dev_cq *cq;
	int i;

	NFIN;
	if (dev->cqs) {
		_NT(free_cqs_i1, "Dev @STR, free @INT dev-cqs", dev->ib_dev->name, dev->n_cqs);
		for (i = 0; i < dev->n_cqs; ++i) {
			cq = &dev->cqs[i];
			free_cq(dev, cq);
		}
		kfree(dev->cqs);
		dev->cqs = NULL;
		dev->n_cqs = 0;
	}
	NFOUT;
}

static void drain_cqs(struct nvmeib_dev *dev, bool on_free)
{
	struct nvmeib_dev_cq *cq;
	int i;
	unsigned long flags;

	NFIN;
	if (dev->cqs) {
		_NT(drain_cqs_i1, "Dev @STR, drain @INT dev-cqs", dev->ib_dev->name, dev->n_cqs);
		for (i = 0; i < dev->n_cqs; ++i) {
			cq = &dev->cqs[i];

			__poll_disable(&cq->iop);

			cq_lock(cq, flags);
			cq->poll_mode = NVMEIB_DEV_CQ_POLL_DISABLED;
			cq_unlock(cq, flags);

			if (on_free) {
				if (cq->user_poll.poll_proc) {
					proc_remove(cq->user_poll.poll_proc);
					cq->user_poll.poll_proc = NULL;
				}
			}

			cq_qps_drain(cq);

			if (!on_free) {
				__poll_enable(&cq->iop);

				rearm_or_resched(cq, NVMEIB_DEV_CQ_POLL_DISABLED);
			}
		}
	}
	NFOUT;
}

void nvmeib_dev_drain_cqs(struct nvmeib_dev *dev)
{
	drain_cqs(dev, false);
}
EXPORT_SYMBOL(nvmeib_dev_drain_cqs);

static int poll_dev_cq_proc_open(struct inode *inode, struct file *file)
{
	struct nvmeib_dev_cq *cq = file_get_priv_data(file);
	unsigned long flags;

	/* proc should not exist if nvmeib_pcpu_cq_all_cpus is not set */
	BUG_ON(!nvmeib_pcpu_cq_all_cpus);

	if (atomic_inc_return(&cq->user_poll.open_cnt) == 1) {
		/* Check that the cq polling is not disabled */
		cq_lock(cq, flags);
		if (cq->poll_mode == NVMEIB_DEV_CQ_POLL_DISABLED) {
			cq_unlock(cq, flags);
			atomic_dec(&cq->user_poll.open_cnt);
			return -EBUSY;
		}
		cq_unlock(cq, flags);
	}
	return 0;
}

static int poll_dev_cq_proc_close(struct inode *inode, struct file *file)
{
	struct nvmeib_dev_cq *cq = file_get_priv_data(file);
	unsigned long flags;
	bool enable_ipoller = false;
	if (atomic_dec_return(&cq->user_poll.open_cnt) == 0) {
		/* Poller is disconnecting, so rearm or reschedule the thread (also sets the poll_mode) */

		cq_lock(cq, flags);
		switch (cq->poll_mode) {
		case NVMEIB_DEV_CQ_POLL_DISABLED:
			/* Polling is disabled - CQ must be dying, nothing to do */
			break;
		case NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE:
			/* Already exiting user-poll mode. WD must have triggered and routine is running. Nothing to do */
			break;
		case NVMEIB_DEV_CQ_INTR_MODE:
		case NVMEIB_DEV_CQ_POLL_MODE:
			/* Already in ipoller mode. WD must have triggered. Nothing to do */
			break;
		case NVMEIB_DEV_CQ_ENTER_USER_POLL_MODE:
			/* This state should only happen in the middle of an ioctl/read on the proc file which cannot happen at this point */
			BUG();
			break;
		case NVMEIB_DEV_CQ_USER_POLL_MODE:
			/* Switch back to ipoller mode. Set state and then del WD timer and call rearm_or_resched */
			cq->poll_mode = NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE;
			enable_ipoller = true;
			break;
		default:
			BUG();
		}
		cq_unlock(cq, flags);

		if (enable_ipoller) {
			/* first disarm the wd timer */
			del_timer_sync(&cq->user_poll.poll_wd_timer);

			rearm_or_resched(cq, NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE);
		}
	}
	return 0;
}

static ssize_t poll_dev_cq_proc_read(struct file *file, char __user *userbuf,
				   size_t len, loff_t *offset_p)
{
	struct nvmeib_dev_cq *cq = file_get_priv_data(file);
	ssize_t rv;
	int completed;
	char buf[16];

	(void)offset_p;
	completed = user_poll_handler_intr(cq, nvmeib_pcpu_cq_user_poll_budget);
	if (completed < 0)
		return completed;

	/* rv contains number of cqes polled, return in string */
	rv = scnprintf(buf, len, "%d\n", completed);
	if (copy_to_user(userbuf, buf, rv))
		rv = -EFAULT;

	return rv;
}

static long poll_dev_cq_proc_ioctl(struct file *file, unsigned int cmd, unsigned long __arg)
{
	struct nvmeib_dev_cq *cq = file_get_priv_data(file);
	long rv;

	(void)cmd;
	(void)__arg;
	rv = user_poll_handler_intr(cq, nvmeib_pcpu_cq_user_poll_budget);
	return rv;
}

#if !KS_HAS_PROC_FS
static struct file_operations poll_dev_cq_proc_ops = {
	.open			= poll_dev_cq_proc_open,
	.read 			= poll_dev_cq_proc_read,
	.unlocked_ioctl		= poll_dev_cq_proc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= poll_dev_cq_proc_ioctl,
#endif
	.release		= poll_dev_cq_proc_close
};
#else
static struct proc_ops poll_dev_cq_proc_ops = {
	.proc_open		= poll_dev_cq_proc_open,
	.proc_read		= poll_dev_cq_proc_read,
	.proc_ioctl		= poll_dev_cq_proc_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl	= poll_dev_cq_proc_ioctl,
#endif
	.proc_release		= poll_dev_cq_proc_close
};
#endif

static int rearm_or_resched(struct nvmeib_dev_cq *cq, enum nvmeib_dev_cq_poll_mode exp_poll_mode);

TIMER_CALLBACK(user_poll_wd_fn, struct nvmeib_dev_cq, user_poll.poll_wd_timer, struct nvmeib_dev_cq, cq)
/* { - part of macro */
	unsigned long jif = jiffies;
	unsigned long flags;
	enum nvmeib_dev_cq_poll_mode curr_poll_mode;
	unsigned long last_poll_jif;
	bool enable_ipoller = false;

	NFIN;

	cq_lock(cq, flags);
	curr_poll_mode = cq->poll_mode;
	last_poll_jif = cq->user_poll.last_poll_jif;
	if (curr_poll_mode == NVMEIB_DEV_CQ_USER_POLL_MODE &&
			jif > (last_poll_jif + msecs_to_jiffies(
			nvmeib_pcpu_cq_user_poll_timeout_msecs)) && !cq->user_poll_process) {
		_NI(trace_user_poll_wd_fn_f, "dev cq @DEV_CQ , timeout since last user-poll, move to ipoller", cq);
		cq->poll_mode = NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE;
		enable_ipoller = true;
	}
	cq_unlock(cq, flags);

	if (curr_poll_mode != NVMEIB_DEV_CQ_USER_POLL_MODE) {
		_NI(trace_user_poll_wd_fn, "dev cq @DEV_CQ in unexpected polling mode @STATE",
		    cq, curr_poll_mode);
		return;
	}
	if (enable_ipoller) {
		_NI(trace_2_user_poll_wd_fn,
		    "timeout waiting on user poll for dev cq @DEV_CQ", cq);
		cq->n_user_poll_wd++;
		rearm_or_resched(cq, NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE);
		return;
	}
	/* Rearm timer */
	cq->user_poll.poll_wd_timer.expires = jif + msecs_to_jiffies(USER_POLL_TIMEOUT_GRANULARITY_MSECS);

	BUG_ON(!nvmeib_pcpu_cq_all_cpus);

	add_timer_on(&cq->user_poll.poll_wd_timer, cq->cpu_id_sched);
	NFOUT;
}

static int init_cqs(struct nvmeib_dev *dev, bool create_poll_cq_proc)
{
#ifdef CONFIG_NUMA
	int node_id = IBDEV2DMADEV(dev->ib_dev)->numa_node;
#else
	int node_id = 0;
#endif

	int max_cores;
	struct nvmeib_dev_cq *cq;
	int i, rv = -1;
	int ncqe;

	NFIN;
	/* sanity */
	if (dev->cqs || dev->n_cqs) {
		_NT(init_cqs_t1, "Dev @STR already has CQs (@PTR, @INT)",
			dev->ib_dev->name, dev->cqs, dev->n_cqs);
		goto out;
	}

	if (dev->dev_type == DT_siw) {
		#if 0
		if ((unsigned)node_id >= poller->n_nodes) {
			_NI(init_cqs_i0, "Patch: Invalid numa-node id @INT for SIW dev "
			"@STR, override to 0", node_id, dev->ib_dev->name);
			node_id = 0;
		}
		#else
		//SIW calls cq-comp-handler from different qp_tx_thread's i.e. from
		//differnt CPUs. We can endup adding CQ to more than one poller -->
		//ctxs-list corruption and polling into wc-array from differnt CPUs.
		_NE(init_cqs_e3, "Per dev CQs not supported over SIW");
		goto out;
		#endif
	}

	mutex_init(&dev->cqs_guard);

	/* By default we create E eqs per device where E=dev->num_comp_vectors.
	   (For mlx 'dev->num_comp_vectors' i.e. the number of IRQs dev uses
	   for completions (EQs), is `#cores-4`).
	   In case default is overriden with N, it is (more than) recommonded
	   to set affinity of dev's IRQs to N cores and to cores that reside
	   on same numa/socket of the dev.

	   Example: mlx5_0 resides on socket with 32 cores: 0-15,32-47, then:
	   1. Set nvmeib_pcpu_cq_max_cqs_per_dev=32
	   2. Run set_irq_affinity_cpulist.sh 0-15,32-47 mlx5_0

	   This way only one CQ per core that is close to the dev.
	   Note that the other 'dev->num_comp_vectors - N' IRQs will also be
	   assigned to same core range(s) but we only casre about the N IRQs
	   that our CQs will use, see 'cq->intr'

	 */

	if (nvmeib_pcpu_cq_all_cpus) {
		max_cores = num_online_cpus();
		_NI_dmesg(i0_init_cqs, "Init num-CQs to num-online-cpu=@INT", max_cores);
	}
	else {
		max_cores = nvmeib_pcpu_cq_max_cqs_per_dev ? : dev->num_comp_vectors;
	}

	_NI(init_cqs_w1,
		"Device @STR (numa-node=@INT, num-comp-vecs=@INT), "
		"allocating @INT pcpu-CQs...",
		dev->ib_dev->name, node_id, dev->num_comp_vectors, max_cores);
	if (!(dev->cqs = kzalloc(sizeof(*dev->cqs) * max_cores, GFP_KERNEL))) {
		_NE(init_cqs_e5,
			"OOM: failed to allocate CQs for device @STR", dev->ib_dev->name);
		goto out;
	}
	else
		dev->n_cqs = max_cores;

	if (nvmeib_pcpu_cq_all_cpus) {
		int c, cq_idx = 0;
		for_each_online_cpu(c) {
			_NI_dmesg(i1_init_cqs, "Bind CQ @INT to ipoller cpu @INT", cq_idx, c);
			dev->cqs[cq_idx].cpu_id_sched = c;
			cq_idx++;
		}
	}

	ncqe = min((int)nvmeib_pcpu_cq_size, dev->dev_attr->max_cqe);
	if ((ncqe - nvmeib_pcpu_cq2srq_size_margin) <= 0) {
		_NE(init_cqs_e6,
			"Dev @STR - Invalid params: min(pcpu_cq_size=@INT, max_cqe=@INT) - "
		   "pcpu_cq2srq_size_margin=@INT < 0",
		   dev->ib_dev->name, nvmeib_pcpu_cq_size, dev->dev_attr->max_cqe,
		   nvmeib_pcpu_cq2srq_size_margin);
		goto out;
	}

	_NI(init_cqs_i1,
		"Dev @STR - each CQ will hold @INT entries, max(@INT,@INT)",
		dev->ib_dev->name, ncqe, nvmeib_pcpu_cq_size, dev->dev_attr->max_cqe);

	for (i = 0; i < dev->n_cqs; ++i) {
		cq = &dev->cqs[i];
		cq->dev = dev;
		mutex_init(&cq->guard);

		/* if #EQs>#CQs && set_irq_affinity_cpulist.sh is given C cpus where C>#CQs (&& C<#EQs),
		   we will have 'C-#CQs' unused cores for/by CQs.
		   in such we need to have counter per dev that client and target will share */
		if (nvmeib_pcpu_cq_comp_vecs_per_dev)
			cq->intr = i % dev->num_comp_vectors;
		else {
			char dev_cq_name[IB_DEVICE_NAME_MAX + 32];
			snprintf(dev_cq_name, sizeof(dev_cq_name), "%s_dev_cq[%03d]",dev->ib_dev->name, i);
			nvmeib_cq_vector_get(dev, dev_cq_name, i, &cq->intr, NULL);
		}

		cq->cq = nvmeib_create_cq(
			dev->ib_dev, cq_completion_intr, NULL, cq, ncqe, cq->intr);
		if (IS_ERR(cq->cq)) {
			rv = PTR_ERR(cq->cq);
			_NE(init_cqs_e7, "Failed to create device @STR cq @INT, rv=@RV",
				dev->ib_dev->name, i, rv);
			goto err_cqs;
		}
		cq->ncqe = ncqe;
		cq->cpu_id = -1;
		_NT(init_cqs_i2, "Dev @STR - cq @INT32_02 @PTR",
		   dev->ib_dev->name, i, cq);

		_NI_dmesg(init_cqs_i3, "Dev @STR, added CQ @INT32_02, comp-vec=@INT",
		   dev->ib_dev->name, i, cq->intr);		// Why 2 prints? Unite to 1.

		cq->poll_mode = NVMEIB_DEV_CQ_INTR_MODE;

#ifdef CQ_POLL_INTR
		/* currently using nvmeib_public_intr_poll.h */
		__poll_init(&cq->iop, IB_INTR_POLL_BUDGET_IRQ, ib_poll_handler);
		ib_req_notify_cq(cq->cq, IB_CQ_NEXT_COMP);
#endif

		/* init qps DB */
		spin_lock_init(&cq->lock);
		spin_lock_init(&cq->qps_lock);
		INIT_RADIX_TREE(&cq->qps_live_tree, GFP_ATOMIC);
		INIT_LIST_HEAD(&cq->qps_stop_list);
		INIT_LIST_HEAD(&cq->qps_del_list_0);
		INIT_LIST_HEAD(&cq->qps_del_list_1);
		INIT_LIST_HEAD(&cq->qps_del_list_2);
		INIT_LIST_HEAD(&cq->qps_del_list_3);

		if (create_poll_cq_proc) {
			if (nvmeib_pcpu_cq_all_cpus) {
				char proc_fname[16];
				/* Create proc interface for polling from user-space (SPDK) */
				snprintf(proc_fname, sizeof(proc_fname), "poll_cq%d",
					nvmeib_pcpu_cq_all_cpus ? cq->cpu_id_sched : i);
				if (!(cq->user_poll.poll_proc = proc_create_data(proc_fname, 0700, dev->proc_dir, &poll_dev_cq_proc_ops, cq))) {
					_NE(error_init_cqs_proc_fail,
					"Fail to create proc entry @PROC_FNAME for @DEV_NAME device CQ @INDEX",
						proc_fname, dev->ib_dev->name, i);
				}
				/* Initialise WD timer */
				INIT_TIMER(&cq->user_poll.poll_wd_timer);
				cq->user_poll.poll_wd_timer.function = user_poll_wd_fn;
				TIMER_SET_DATA(cq, user_poll.poll_wd_timer, (unsigned long)cq);
			} else {
				_NW(error_init_cqs_proc_inv_mode,
				    "Invalid mode for poll-cq via proc - pcpu_cq_all_cpus not set");
			}
		}
	}

	rv = 0;
	goto out;

err_cqs:
	free_cqs(dev);

out:
	NFOUT;
	return rv;
}

static void free_cq_srq(struct nvmeib_dev *dev)
{
	struct nvmeib_dev_cq *cq;
	int i;

	NFIN;
	for (i = 0; i < dev->n_cqs; ++i) {
		cq = &dev->cqs[i];
		if (cq->srq_info) {
			nvmeib_srq_info_free(cq->srq_info);
			cq->srq_info = NULL;
		}
	}
	NFOUT;
}

bool nvmeib_support_srq(struct nvmeib_dev *dev)
{
	return dev->dev_attr->max_srq &&
		nvmeib_device_sup_cap(dev->dev_type, NVMEIB_DEVCAP_SRQ);
}
EXPORT_SYMBOL(nvmeib_support_srq);

int nvmeib_create_cq_srq(struct nvmeib_dev *dev, int msg_size, void *memmgr_metrics_ctx)
{
	int i;
	struct nvmeib_srq_params params = {};
	struct nvmeib_srq_info *srq_info;
	struct nvmeib_dev_cq *cq;
	int rv;

	NFIN;
	params.msg_size = msg_size;
	_NT(nvmeib_create_cq_srq_i1,"Dev @STR - msg_size=@INT",
		dev->ib_dev->name, params.msg_size);
	for (i = 0; i < dev->n_cqs; ++i) {
		cq = &dev->cqs[i];
		params.q_size = cq->ncqe - nvmeib_pcpu_cq2srq_size_margin;
		_NT(nvmeib_create_cq_srq_i2, "Dev @STR - q[@INT].size=@INT",
			dev->ib_dev->name, i, params.q_size);
		if ((params.q_size <= 0) ||
			!(srq_info = nvmeib_srq_info_create(dev, &params, NULL, memmgr_metrics_ctx))) {
			_NE(nvmeib_create_cq_srq_e1, "Dev @STR - fail to create "
										 "SRQ for CQ @INT\n",
				dev->ib_dev->name, i);
			rv = -1;
			goto err;
		}
		else {
			cq->srq_info = srq_info;
		}
	}
	rv = 0;
	goto out;

err:
	free_cq_srq(dev);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_create_cq_srq);

extern bool nvmeib_iwarp_find_path_sock;

static void free_keeper_frs(struct nvmeib_keeper_frs_info *frs_info)
{
	int i;
	NFIN;

	if (!frs_info) {
		goto out;
	}

	if (frs_info->mr_arr) {
		for (i = 0; i < frs_info->n_mr; i++) {
			if (frs_info->mr_arr[i]) {
				ib_dereg_mr(frs_info->mr_arr[i]);
			}
		}
		kfree(frs_info->mr_arr);
	}
	memset(frs_info, 0, sizeof(*frs_info));

out:
	NFOUT;
}

bool nvmeib_dev_use_keeper(struct nvmeib_dev *dev) {
	/* [NVMESH-6852]: Don't use the keeper for SIW - SIW module is removed during upgrade */
	return dev->dev_type != DT_siw;
}
EXPORT_SYMBOL(nvmeib_dev_use_keeper);


struct msgloop_procfs_ent *nvmeib_trace_get_io_pet_msgloop(void);
struct msgloop_procfs_ent *nvmeib_trace_get_io_pet_msgloop(void)
{
	return io_pet_writer;
}
EXPORT_SYMBOL(nvmeib_trace_get_io_pet_msgloop);


struct nvmeib_dev *nvmeib_init(struct ib_device *device,
			       const char *inst_name,
			       bool do_init_cqs, //TODO: Remove this
			       bool create_poll_cq_proc)
{
	struct nvmeib_dev *dev = NULL;
	bool prefer_fr = true;
	int rv;
	char dev_proc_dir_name[IB_DEVICE_NAME_MAX + __NEW_UTS_LEN + 16];
	struct nvmeib_keeper_ops *keeper_ops;

	NFIN;
	dev = kzalloc(sizeof *dev, GFP_KERNEL);
	if (!dev) {
		_NE(error_nvmeib_nvmeib_init, "cannot allocate memory for device");
		goto out;
	}

	dev->ib_dev = device;
	dev->dev_type = nvmeib_get_device_type(dev->ib_dev);
	if (dev->dev_type == DT_uknown) {
		_NE(error_1_nvmeib_nvmeib_init, "unknown dev type");
		goto free_dev;
	}

	dev->phys_port_cnt = get_device_phys_port_count(dev);
	dev->num_comp_vectors = min_t(int, num_online_cpus(),
		device->num_comp_vectors);
	dev->dev_attr = kzalloc(sizeof *dev->dev_attr, GFP_KERNEL);
	if (!dev->dev_attr)
		goto free_dev;

	if (ib_query_device(dev->ib_dev, dev->dev_attr)) {
		_NW(warn_nvmeib_nvmeib_init, "Query device failed");
		goto free_attr;
	}
	
	if (nvmeib_dev_use_keeper(dev)) {
		/* [NVMESH-6668]: Look for keeper module that holds FR MRs from last run */
		if ((keeper_ops = nvmeib_public_get_keeper())) {
			/* Keeper found, look for stored FRs */
			_NI(trace_nvmeib_init_keeper_found, 
			"@STR - Requesting FRs for @IB_DEV_NAME from Keeper",
				inst_name, device->name);
			if ((rv = CALL_KEEPER_OP(keeper_ops, pop_frs)(KBUILD_MODNAME, inst_name, device, &dev->keeper_frs_info)) < 0) {
				_NI(trace_nvmeib_init_keeper_pop_failed,
				"@STR - Pop FRs failed (@RV) for @IB_DEV_NAME",
				inst_name, rv, device->name);
			} else {
				BUG_ON(dev->keeper_frs_info.version != NVMEIB_KEEPER_FRS_INFO_VERSION);
				/* Stored FRs found, init dev from stored info */
				_NI(trace_nvmeib_init_keeper_pop_ok,
					"@STR - Popped @COUNT FRs for @IB_DEV_NAME",
					inst_name, dev->keeper_frs_info.n_mr, device->name);
				/* Set the PD and DMA MR in the nvmeib_dev */
				dev->pd = dev->keeper_frs_info.pd;
				dev->mr = dev->keeper_frs_info.dma_mr;
				/* Set the FR state. Must be enabled as FRs were pushed */
				dev->has_fmr = false;
				dev->has_fr = true;
				dev->use_fast_reg = true;
				dev->mr_page_mask = dev->keeper_frs_info.mr_page_mask;
				dev->mr_page_size = dev->keeper_frs_info.mr_page_size;
				dev->mr_max_size = dev->keeper_frs_info.mr_max_size;
				dev->max_pages_per_mr = dev->keeper_frs_info.max_pages_per_mr;
				dev->init_from_keeper = true;
				BUG_ON(dev->pd->device != device);
				BUG_ON(!dev->keeper_frs_info.n_mr);
				BUG_ON(dev->pd != dev->keeper_frs_info.mr_arr[0]->pd);
			}
			nvmeib_public_put_keeper();
		} else {
			_NI(trace_nvmeib_init_keeper_not_found, "Keeper not found");
		}
	}

	if (!dev->init_from_keeper) {
		dev->pd = ib_alloc_pd(device);
		if (IS_ERR_OR_NULL(dev->pd)) {
			rv = PTR_ERR(dev->pd);
			_NE(error_2_nvmeib_nvmeib_init, "alloc_pd() returne with error @RV", rv);
			dev->pd = NULL;
			goto free_attr;
		}
	}

	if (!dev->init_from_keeper) {
#if defined(HAS_IB_GET_DMA_MR) && HAS_IB_GET_DMA_MR
		dev->mr = ib_get_dma_mr(dev->pd,
								IB_ACCESS_LOCAL_WRITE |
								IB_ACCESS_REMOTE_READ |
								IB_ACCESS_REMOTE_WRITE);
#else
		if (device->get_dma_mr)
			dev->mr = device->get_dma_mr(dev->pd,
							IB_ACCESS_LOCAL_WRITE |
							IB_ACCESS_REMOTE_READ |
							IB_ACCESS_REMOTE_WRITE |
							IB_ACCESS_REMOTE_ATOMIC);
#endif
		if (IS_ERR_OR_NULL(dev->mr)) {
			rv = PTR_ERR(dev->mr);
			_NE(error_3_nvmeib_nvmeib_init, "ib_get_dma_mr() returne with error @RV", rv);
			dev->mr = NULL;
			goto err_pd;
		}
	}

	_NI(i0_nvmeib_init,
		"dev=@DEVICE_NAME, mr=@PTR, rkey=@INT32_HEX, lkey=@INT32_HEX",
		device->name, dev->mr, nvmeib_get_rkey(dev), nvmeib_get_lkey(dev));

	if (!dev->init_from_keeper) {
#if KS_IB_VERBS_SUPPORTS_FMR
		dev->has_fmr = (device->alloc_fmr && device->dealloc_fmr &&
						device->map_phys_fmr && device->unmap_fmr);
#else
		dev->has_fmr = false;
#endif
		dev->has_fr = (dev->dev_attr->device_cap_flags &
					IB_DEVICE_MEM_MGT_EXTENSIONS);

		if (dev->dev_type != DT_siw) {
			if (!dev->has_fmr && !dev->has_fr) {
				_NW(warn_1_nvmeib_nvmeib_init, "dev_type=@DEV_TYPE neither FMR nor FR is supported - cannot continue",
					dev->dev_type);
				goto err_pd;
			}
		}
		dev->use_fast_reg = (dev->has_fr && (!dev->has_fmr || prefer_fr));
	}

	if ((rv = get_dev_pops_fns(dev))) {
		_NE(error_4_nvmeib_nvmeib_init, "nvmeib_common_public module for device not loaded");
		goto err_pd;
	}

	dev->inst_name = inst_name;
	scnprintf(dev_proc_dir_name, sizeof(dev_proc_dir_name), "%s_%s", inst_name, device->name);
	if (!(dev->proc_dir = proc_mkdir(dev_proc_dir_name, proc_dir))) {
		_NE(error_5_nvmeib_nvmeib_init, "Fail to create proc dir");
		goto err_pd;
	}

	if (do_init_cqs && (rv = init_cqs(dev, create_poll_cq_proc))) {
		_NE(error_4a_nvmeib_nvmeib_init, "Failed to create device CQs");
		goto err_cqs;
	}

	_NT(trace_nvmeib_nvmeib_init, "has_fmr=@HAS_FMR, has_fr=@HAS_FR, use_fr=@USE_FR, num_comp_vectors=@NUM_COMP_VECTORS, map_mr_fn=@MAP_MR_FN <@MAP_MR_F> post_send_atomic=@POST_SEND_ATOMIC <@POST_SEND_ATOMIC_FN_PTR>",
		dev->has_fmr ? "true" : "false",
		dev->has_fr ? "true" : "false",
		dev->use_fast_reg ? "true" : "false",
		dev->num_comp_vectors,
		dev->map_mr_f, dev->map_mr_f,
		dev->post_send_atomic_fn, dev->post_send_atomic_fn);

	if (!dev->init_from_keeper)
		nvmeib_init_fast_reg(dev);

	if (dev->dev_type == DT_siw) {
		if (nvmeib_iwarp_find_path_sock)
			nvmeib_rdma_init_find_path_sock(dev);
		nvmeib_rdma_init_iw_cm_inv_stats(dev);
	}

	goto out;

err_cqs:
	free_cqs(dev);

	if (dev->proc_dir)
		remove_proc_entry(dev_proc_dir_name, proc_dir);

err_pd:
	free_keeper_frs(&dev->keeper_frs_info);
	if (dev->mr) {
		ib_dereg_mr(dev->mr);
		dev->mr = NULL;
	}
	ib_dealloc_pd(dev->pd);

free_attr:
	kfree(dev->dev_attr);

free_dev:
	kfree(dev);
	dev = NULL;

out:
	NFOUT;
	return dev;
}
EXPORT_SYMBOL(nvmeib_init);

void nvmeib_free(struct nvmeib_dev *dev)
{
	bool saved_to_keeper = false;
	NFIN;

	nvmeib_rdma_free_find_path_sock(dev);
	nvmeib_rdma_free_iw_cm_inv_stats(dev);

	drain_cqs(dev, true);
	free_cq_srq(dev);
	free_cqs(dev);
	/* Free SRQ pool.
	   Assume no channels are using private SRQs of this dev by now */
	nvmeib_srq_pool_free(dev);

	/* Release public ops module */
	nvmeib_ibdr_hwdev_pops_put(dev->pops);

	if (dev->dev_attr)
		kfree(dev->dev_attr);
	
	if (dev->save_to_keeper) {
		struct nvmeib_keeper_ops *keeper_ops = NULL;
		int rv;

		/* save_to_keeper should not be set for this device */
		BUG_ON(!nvmeib_dev_use_keeper(dev));

		/* [NVMESH-6668]: Look for keeper module that will hold the FR MRs for the next run */
		if ((keeper_ops = nvmeib_public_get_keeper())) {
			struct nvmeib_keeper_frs_info *frs_info = &dev->keeper_frs_info;
			if ((rv = CALL_KEEPER_OP(keeper_ops, push_frs)(dev->inst_name, frs_info)) < 0) {
				_NW(warn_nvmeib_free_keeper_push_frs_fail, "@IB_DEV_NAME - Failed to push FRs (@RV)", dev->ib_dev->name, rv);
			} else {
				_NI(trace_nvmeib_free_keeper_push_frs_ok, "@IB_DEV_NAME - Pushed @COUNT FR MRs for restart", 
				    dev->ib_dev->name, frs_info->n_mr);

				saved_to_keeper = true;
			}
			nvmeib_public_put_keeper();
		} else {
			_NW(warn_nvmeib_free_keeper_not_found, "@IB_DEV_NAME - Keeper disappeared under our feet", dev->ib_dev->name);
		}
	}
	
	if (!saved_to_keeper) {
		if (dev->keeper_frs_info.mr_arr) {
			free_keeper_frs(&dev->keeper_frs_info);
		}
		
		/* If FRs have been saved to the keeper, we must not destroy the DMA MR or PD */

#if defined(HAS_IB_GET_DMA_MR) && HAS_IB_GET_DMA_MR
		if (dev->mr) {
			ib_dereg_mr(dev->mr);
			dev->mr = NULL;
		}
#else
		if (dev->mr) {
			if (dev->ib_dev && dev->ib_dev->dereg_mr && (dev->dev_type == DT_siw)) {
				#if defined(IB_DEREG_MR_HAS_UDATA) && IB_DEREG_MR_HAS_UDATA
				dev->ib_dev->dereg_mr(dev->mr, NULL);
				#else
				dev->ib_dev->dereg_mr(dev->mr);
				#endif
				dev->mr = NULL;
			}
		}
#endif
		if (dev->pd) {
			ib_dealloc_pd(dev->pd);
			dev->pd = NULL;
		}
	}
	
	if (dev->proc_dir) {
		char dev_proc_dir_name[IB_DEVICE_NAME_MAX + __NEW_UTS_LEN + 16];
		scnprintf(dev_proc_dir_name, sizeof(dev_proc_dir_name), "%s_%s", dev->inst_name, dev->ib_dev->name);
		remove_proc_entry(dev_proc_dir_name, proc_dir);
	}
	kfree(dev);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_free);

const char *nvmeib_device_name(struct nvmeib_dev *dev)
{
	return dev->ib_dev ? dev->ib_dev->name : "???";
}
EXPORT_SYMBOL(nvmeib_device_name);

static inline size_t calc_fr_pool_alloc_sz(struct nvmeib_fr_pool *pool)
{
	size_t sz = 0;
	if (!pool)
		return sz;
	
	sz += sizeof(*pool);
#if !IB_NEW_FR
	sz += PAGE_ALIGN(sizeof(struct nvmeib_fr_desc) * (pool->max_page_list_len * 8));
#else
	sz += PAGE_ALIGN(sizeof(struct nvmeib_fr_desc) * sizeof(struct scatterlist) * (NVMEIB_FMR_MIN_SIZE * 8));
#endif
	return sz;
}

int nvmeib_get_dev_numa_node(struct nvmeib_dev *dev)
{
	int numa_node = NUMA_NO_NODE;
	if (dev->dev_type == DT_siw) {
		struct net_device *siw_ndev;
		if (!dev->ib_dev->get_netdev)
			goto out;
		if ((siw_ndev = dev->ib_dev->get_netdev(dev->ib_dev, 1)) == NULL)
			goto out;

		numa_node = dev_to_node(&siw_ndev->dev);
		dev_put(siw_ndev);
		goto out;
	}
	numa_node = dev->ib_dev->dma_device->numa_node;
out:
	return numa_node;
}
EXPORT_SYMBOL(nvmeib_get_dev_numa_node);

static struct kmem_cache *fr_pool_desc_cache;

static int fr_pool_cache_init(void)
{
	int rv;
	NFIN;
	if (!(fr_pool_desc_cache = KMEM_CACHE(nvmeib_fr_desc, 0))) {
		_NE(error_nvmeib_fr_pool_cache_init, "Failed to allocate kmem cache for nvmeib_fr_desc");
		rv = -ENOMEM;
		goto out;
	}
	rv = 0;
out:
	NFOUT;
	return rv;
}

static void fr_pool_cache_exit(void)
{
	NFIN;
	if (fr_pool_desc_cache) {
		kmem_cache_destroy(fr_pool_desc_cache);
		fr_pool_desc_cache = NULL;
	}
	NFOUT;
}

/**
 * nvmeib_destroy_fast_reg_pool() - free the resources owned by
 * a pool
 * @pool: Fast registration pool to be destroyed.
 * @memmgr_metrics_ctx: Memory manager metrics context.
 */
void nvmeib_destroy_fast_reg_pool(struct nvmeib_fr_pool *pool, struct ib_mr **keep_mrs_arr, int *keep_mrs_arr_sz, void *memmgr_metrics_ctx, int pool_size)
{
	struct nvmeib_fr_desc *d;
	int keep_mrs_idx = 0;
	bool use_keeper = keep_mrs_arr && keep_mrs_arr_sz;

	NFIN;
	if (!pool) {
		NFOUT;
		return;
	}

	if (pool->percpu_cache) {
		int cpu;
		cancel_work_sync(&pool->rebalance_pcpu_cache_work);
		for_each_possible_cpu(cpu) {
			struct nvmeib_fr_pool_percpu_cache *pcpu_cache = per_cpu_ptr(pool->percpu_cache, cpu);
			struct nvmeib_fr_desc *desc;

			del_timer_sync(&pcpu_cache->idle_timer);

			while ((desc = list_first_entry_or_null(&pcpu_cache->free_list, struct nvmeib_fr_desc, entry))) {
				list_del(&desc->entry);
				BUG_ON(pcpu_cache->n_free <= 0);
				pcpu_cache->n_free--;

				list_add(&desc->entry, &pool->free_list);
				pool->n_free++;
			}
			BUG_ON(pcpu_cache->n_free != 0);
		}
		nvmeib_public_free_percpu(pool->percpu_cache);
		pool->percpu_cache = NULL;
	}

	if (pool->n_free != pool->size) {
		_NW_dmesg(trace_nvmeib_nvmeib_destroy_fast_reg_pool, "FR pool (@POOL): exp=@EXP, found=@FOUND",
			pool, pool->size, pool->n_free);
		BUG_NON_PRODUCTION(8091);
	}
	
	if (use_keeper && *keep_mrs_arr_sz < pool->size) {
		_NW(warn_nvmeib_nvmeib_destroy_fast_reg_pool_keeper, "FR pool (@POOL): Keeper MRs Array is too small (@COUNT) for pool size (@COUNT)", pool, *keep_mrs_arr_sz, pool->size);
		use_keeper = false;
		*keep_mrs_arr_sz = 0;
	}

	while ((d = list_first_entry_or_null(&pool->free_list, struct nvmeib_fr_desc, entry))) {
		BUG_ON(pool->n_free <= 0);
		pool->n_free--;
		list_del(&d->entry);
#if !IB_NEW_FR
		if (d->frpl)
			ib_free_fast_reg_page_list(d->frpl);
#else
		kfree(d->map_sgl);
		d->map_sgl = NULL;
#endif
		if (d->mr) {
			if (use_keeper) {
				d->mr->need_inval = !d->valid;
				keep_mrs_arr[keep_mrs_idx++] = d->mr;
			} else {
				ib_dereg_mr(d->mr);
			}
		}
		kmem_cache_free(fr_pool_desc_cache, d);
	}
	BUG_ON(pool->n_free != 0);

	if (use_keeper)
		*keep_mrs_arr_sz = keep_mrs_idx;
	if (memmgr_metrics_ctx && pool_size > 0 && pool->size == pool_size /* as only in this case it is considered to be allocated by the memmgr*/) {
		nvmesh_memmgr_metric_on_free_update(memmgr_metrics_ctx, calc_fr_pool_alloc_sz(pool));
	}

	if (pool->proc_ent) {
		nvmeib_public_proc_remove(pool->proc_ent);
		pool->proc_ent = NULL;
	}

	kfree(pool);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_destroy_fast_reg_pool);

bool nvmeib_fr_pool_percpu_cache = true;
module_param_named(fr_pool_percpu_cache, nvmeib_fr_pool_percpu_cache, bool, 0444);
MODULE_PARM_DESC(fr_pool_percpu_cache, "Enable per-cpu cache for fast registration pool");

unsigned nvmeib_fr_pool_percpu_low = 64;
module_param_named(fr_pool_percpu_low, nvmeib_fr_pool_percpu_low, uint, 0444);
MODULE_PARM_DESC(fr_pool_percpu_low, "Refill target for per-cpu cache for fast registration pool");

unsigned nvmeib_fr_pool_percpu_high = 128;
module_param_named(fr_pool_percpu_high, nvmeib_fr_pool_percpu_high, uint, 0444);
MODULE_PARM_DESC(fr_pool_percpu_high, "Spill threshold for per-cpu cache for fast registration pool");

unsigned nvmeib_fr_pool_percpu_idle_timeout_ms = 1000;
module_param_named(fr_pool_percpu_idle_timeout_ms, nvmeib_fr_pool_percpu_idle_timeout_ms, uint, 0444);
MODULE_PARM_DESC(fr_pool_percpu_idle_timeout_ms, "Idle timeout for per-cpu cache for fast registration pool in milliseconds");

unsigned nvmeib_fr_pool_percpu_rebalance_min_interval_ms = 1000;
module_param_named(fr_pool_percpu_rebalance_min_interval_ms, nvmeib_fr_pool_percpu_rebalance_min_interval_ms, uint, 0444);
MODULE_PARM_DESC(fr_pool_percpu_rebalance_min_interval_ms, "Rebalance per-cpu cache minimum interval in milliseconds");

static void fr_pool_percpu_idle_timer_fn(struct timer_list *);
static void rebalance_fr_pool_percpu_cache_work(struct work_struct *work);
static ssize_t fill_fr_pool_proc_buf(void *arg, char *buf, size_t len);
static void fr_pool_rereg_work(struct work_struct *work);

/**
 * nvmeib_fr_pool() - allocate and initialize a pool for fast
 * registration
 * @device:            IB device to allocate fast registration descriptors for.
 * @pd:                Protection domain associated with the FR descriptors.
 * @pool_size:         Number of descriptors to allocate.
 * @max_page_list_len: Maximum fast registration work request page list length.
 */
static struct nvmeib_fr_pool *create_fr_pool(struct nvmeib_dev *dev,
	struct ib_pd *pd, int pool_size, int max_page_list_len, struct ib_mr **kept_mr_arr, int kept_mr_arr_sz,
	bool *c_retry, bool *cc_retry, void *memmgr_metrics_ctx)
{
	struct nvmeib_fr_pool *pool;
	struct nvmeib_fr_desc *d = NULL;
	struct ib_mr *mr;
#if !IB_NEW_FR
	struct ib_fast_reg_page_list *frpl;
#endif
	int i, rv = -EINVAL;

	NFIN;
	if (pool_size <= 0)
		goto err;
	rv = -ENOMEM;
	pool = kzalloc(sizeof(struct nvmeib_fr_pool), GFP_KERNEL);
	if (!pool)
		goto err;
	if (!(pool->proc_ent = nvmeib_public_proc_create("fr_pool.json", dev->proc_dir, 
		fill_fr_pool_proc_buf, NULL, pool))) 
	{
		_NE(error_nvmeib_create_fr_pool, "Failed to create proc entry for FR pool");
		goto destroy_pool;
	}

#ifndef NVMEIBC_DEBUG_FR_LEAK
	if (nvmeib_fr_pool_percpu_cache) {
		int cpu;
		pool->percpu_cache = nvmeib_public_alloc_percpu_cacheline(struct nvmeib_fr_pool_percpu_cache);
		if (!pool->percpu_cache)
			goto destroy_pool;
		pool->pcpu_low = nvmeib_fr_pool_percpu_low;
		pool->pcpu_high = nvmeib_fr_pool_percpu_high;
		INIT_WORK(&pool->rebalance_pcpu_cache_work, rebalance_fr_pool_percpu_cache_work);
		for_each_possible_cpu(cpu) {
			struct nvmeib_fr_pool_percpu_cache *pcpu_cache = per_cpu_ptr(pool->percpu_cache, cpu);
			pcpu_cache->pool = pool;
			pcpu_cache->cpu = cpu;
			INIT_LIST_HEAD(&pcpu_cache->free_list);
			pcpu_cache->n_free = 0;
			pcpu_cache->last_get_jif = jiffies;
			timer_setup(&pcpu_cache->idle_timer, fr_pool_percpu_idle_timer_fn, TIMER_DEFERRABLE);
			pcpu_cache->idle_timer.expires = jiffies + nvmeib_fr_pool_percpu_idle_timeout_ms * HZ / 1000;
			add_timer_on(&pcpu_cache->idle_timer, cpu);
		}
	}
#endif

	pool->pd = pd;
	pool->max_page_list_len = max_page_list_len;
	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->free_list);
	INIT_LIST_HEAD(&pool->err_list);
#ifdef NVMEIBC_DEBUG_FR_LEAK
	INIT_LIST_HEAD(&pool->used_list);
#endif
	INIT_WORK(&pool->rereg_work, fr_pool_rereg_work);

	for (i = 0; i < pool_size; ++i) {
		d = kmem_cache_alloc(fr_pool_desc_cache, GFP_KERNEL | __GFP_ZERO);
		if (!d) {
			_NE(error_1_nvmeib_create_fr_pool, "i=@IDX: kmem_cache_alloc() failed",	i);
			rv = -ENOMEM;
			goto destroy_pool;
		}
		list_add_tail(&d->entry, &pool->free_list);
		pool->n_free++;
		pool->size++;
		if (kept_mr_arr_sz && i < kept_mr_arr_sz) {
			BUG_ON(!kept_mr_arr[i]);
			BUG_ON(kept_mr_arr[i]->pd != pd);

			mr = kept_mr_arr[i];
			kept_mr_arr[i] = NULL;
		} else {
#if IB_NEW_FR
			mr = ib_alloc_mr(pd, IB_MR_TYPE_MEM_REG, max_page_list_len);
#else
			mr = ib_alloc_fast_reg_mr(pd, max_page_list_len);
#endif
			if (IS_ERR_OR_NULL(mr)) {
				rv = PTR_ERR(mr);
				_NE(error_2_nvmeib_create_fr_pool, "i=@IDX: ib_alloc_fast_reg_mr() returned with error @MAX_PAGE_LIST_LEN (@RV)",
					i, max_page_list_len, rv);
				mr = NULL;
				if (cc_retry)
					*cc_retry = true;
				goto destroy_pool;
			}
		}
		d->mr = mr;
#if !IB_NEW_FR
		frpl = ib_alloc_fast_reg_page_list(dev->ib_dev, max_page_list_len);
		if (IS_ERR_OR_NULL(frpl)) {
			rv = PTR_ERR(frpl);
			_NE(error_4_nvmeib_create_fr_pool, "i=@IDX: ib_alloc_fast_reg_page_list() "
			   "returned with error @MAX_PAGE_LIST_LEN (@RV)", i, max_page_list_len, rv);
			frpl = NULL;
			if (c_retry)
				*c_retry = true;
			goto destroy_pool;
		}
		d->frpl = frpl;
#else
		if (!(d->map_sgl = kzalloc(sizeof(*d->map_sgl) * NVMEIB_FMR_MIN_SIZE, GFP_KERNEL))) {
			_NE(create_fr_pool_e1,
				"i=@INT: Alloc error for sgl of size @INT",
				i, NVMEIB_FMR_MIN_SIZE);
			if (c_retry)
				*c_retry = true;
			goto destroy_pool;
		}
		d->map_sgl_pages = NVMEIB_FMR_MIN_SIZE;
#endif
		d->valid = !mr->need_inval;
	}
	BUG_ON(pool->size != pool->n_free);
	BUG_ON(pool->size != pool_size);

	_NI(trace_nvmeib_create_fr_pool, "Created FR pool (@POOL): size=@SIZE, n_free=@N_FREE, max_page_list_len=@INT",
		pool, pool->size, pool->n_free, max_page_list_len);
	if (memmgr_metrics_ctx) {
		nvmesh_memmgr_metric_on_alloc_update(memmgr_metrics_ctx, calc_fr_pool_alloc_sz(pool), true /* success */);
	}

out:
	NFOUT;
	return pool;

destroy_pool:
	nvmeib_destroy_fast_reg_pool(pool, NULL, NULL, memmgr_metrics_ctx, pool_size);

err:
	pool = ERR_PTR(rv);
	if (memmgr_metrics_ctx && pool_size > 0) {
		nvmesh_memmgr_metric_on_alloc_update(memmgr_metrics_ctx, 0, false /* success */);
	}
	goto out;
}

static void *alloc_fr_pool(struct nvmeib_dev *dev,
	int pool_size, bool *c_retry, void *memmgr_metrics_ctx)
{
	struct nvmeib_fr_pool *fr = NULL;
	int max_pages_per_mr;
	bool retry = false;

	NFIN;
	for (max_pages_per_mr = dev->max_pages_per_mr;
		 max_pages_per_mr >= NVMEIB_FMR_MIN_SIZE;
		 max_pages_per_mr /= 2) {
		fr = create_fr_pool(
			dev, dev->pd, pool_size, max_pages_per_mr, NULL, 0, &retry, c_retry, memmgr_metrics_ctx);
		if (!IS_ERR_OR_NULL(fr) || !retry) {
			dev->max_pages_per_mr = max_pages_per_mr;
			if (fr) {
				_NT(trace_nvmeib_alloc_fr_pool, "FR: max pages per mr @MAX_PAGES_PER_MR", dev->max_pages_per_mr);
			}
			break;
		}
	}

	NFOUT;
	return fr;
}

static void *alloc_fmr_pool(
	struct nvmeib_dev *dev, int pool_size, bool *c_retry, void *memmgr_metrics_ctx)
{
#if KS_IB_VERBS_SUPPORTS_FMR
	struct ib_fmr_pool_param fmr_param = {0};
	int max_pages_per_fmr;
	int fmr_page_shift;
	struct ib_fmr_pool *fmr_pool = NULL;
	int rv;

	NFIN;
	fmr_page_shift = ilog2(dev->mr_page_size);
	for (max_pages_per_fmr = dev->max_pages_per_mr;
			max_pages_per_fmr >= NVMEIB_FMR_MIN_SIZE;
			max_pages_per_fmr /= 2, dev->mr_max_size /= 2) {
		memset(&fmr_param, 0, sizeof fmr_param);
		fmr_param.pool_size = pool_size;
		fmr_param.dirty_watermark = fmr_param.pool_size / 4;
		fmr_param.cache = 1;
		fmr_param.max_pages_per_fmr = max_pages_per_fmr;
		fmr_param.page_shift = fmr_page_shift;
		fmr_param.access = (IB_ACCESS_LOCAL_WRITE |
							IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ);
		_ND(trace_nvmeib_alloc_fmr_pool, "N2: calling ib_create_fmr_pool dev = @DEV pd = @PD",
			dev, dev->pd);
		_ND(trace_1_nvmeib_alloc_fmr_pool, "N2: pooo size = @POOL_SIZE, dirty_watermark = @DIRTY_WATERMARK, max_pages_per_fmr = @MAX_PAGES_PER_FMR"
		   ", page_shift = @PAGE_SHIFT, acess = @ACCESS", fmr_param.pool_size,
			fmr_param.dirty_watermark, fmr_param.max_pages_per_fmr,
			fmr_param.page_shift, fmr_param.access);
		fmr_pool = ib_create_fmr_pool(dev->pd, &fmr_param);
		_ND(trace_2_nvmeib_alloc_fmr_pool, "N2: done @FMR_POOL", fmr_pool);
		if (!IS_ERR_OR_NULL(fmr_pool)) {
			dev->max_pages_per_mr = max_pages_per_fmr;
			_NT(trace_3_nvmeib_alloc_fmr_pool, "fmr_pool =@FMR_POOL, max_pages_per_fmr=@MAX_PAGES_PER_FMR, dev->mr_max_size=@MR_MAX_SIZE",
				fmr_pool, dev->max_pages_per_mr, dev->mr_max_size);
			break;
		}
		else {
			rv = PTR_ERR(fmr_pool);
			_NE(error_nvmeib_alloc_fmr_pool, "ib_create_fmr_pool() returned with error @RV", rv);
			fmr_pool = NULL;
			_NE(error_1_nvmeib_alloc_fmr_pool, "rv=@RV, @ERROR", rv, -EINVAL);
			if (rv != -EINVAL) {
				*c_retry = true;
				break;
			}
		}
	}
	NFOUT;
	return fmr_pool;
#else
	(void)dev;(void)pool_size;(void)c_retry;(void)memmgr_metrics_ctx;
	BUG();
	return NULL;
#endif
}

int nvmeib_init_fast_reg(struct nvmeib_dev *dev)
{
	int mr_page_shift;
	u64 max_pages_per_mr;

	NFIN;
	/*
	 * Use the smallest page size supported by the HCA, down to a
	 * minimum of 4096 bytes. We're unlikely to build large sglists
	 * out of smaller entries.
	 */
	mr_page_shift = max(12, ffs(dev->dev_attr->page_size_cap) - 1);
	dev->mr_page_size = 1 << mr_page_shift;
	dev->mr_page_mask = ~((u64) dev->mr_page_size - 1);
	max_pages_per_mr = dev->dev_attr->max_mr_size;
	do_div(max_pages_per_mr, dev->mr_page_size);
	dev->max_pages_per_mr = min_t(u64, NVMEIB_FMR_SIZE, max_pages_per_mr);

	_NI(t0_nvmeib_nvmeib_init_fast_reg,
		"AAA: "
		"dev->dev_attr: page_size_cap=@LLU, max_mr_size=@LLU, "
		"dev: mr_page_size=@INT, max_pages_per_mr=@INT",
		dev->dev_attr->page_size_cap, dev->dev_attr->max_mr_size,
		dev->mr_page_size, dev->max_pages_per_mr);

	if (dev->use_fast_reg) {
		dev->max_pages_per_mr = min_t(u32, dev->max_pages_per_mr,
			dev->dev_attr->max_fast_reg_page_list_len);
	}
	dev->mr_max_size = dev->mr_page_size * dev->max_pages_per_mr;
	_NI(trace_nvmeib_nvmeib_init_fast_reg, "mr_page_shift = @MR_PAGE_SHIFT, dev->dev_attr->max_mr_size = @MAX_MR_SIZE, "
		"dev->dev_attr->max_fast_reg_page_list_len = @MAX_FAST_REG_PAGE_LIST_LEN, "
		"max_pages_per_mr = @MAX_PAGES_PER_MR, mr_max_size = @MR_MAX_SIZE",
		mr_page_shift, (s64)dev->dev_attr->max_mr_size,
		dev->dev_attr->max_fast_reg_page_list_len,
		dev->max_pages_per_mr, dev->mr_max_size);
	NFOUT;
	return 0;
}
EXPORT_SYMBOL(nvmeib_init_fast_reg);

static void *loop_fr_fmr_pool_size(void *(f)(struct nvmeib_dev *, int, bool *, void *),
	struct nvmeib_dev *dev, int pool_size, void *memmgr_metrics_ctx)
{
	void *pool = NULL;
	bool retry;

	NFIN;
	while (!pool && pool_size) {
		_NT(trace_nvmeib_loop_fr_fmr_pool_size, "Trying FMR/FR pool size @POOL_SIZE", pool_size);
		retry = false;
		pool = f(dev, pool_size, &retry, memmgr_metrics_ctx);
		if (IS_ERR_OR_NULL(pool)) {
			_NE(error_nvmeib_loop_fr_fmr_pool_size, "FMR/FR pool allocation failed (@PTR_ERR)", PTR_ERR(pool));
			pool = NULL;
			_NE(error_1_nvmeib_loop_fr_fmr_pool_size, "retry=@RETRY", retry ? 1 : 0);
			if (retry)
				pool_size /= 2;
			else
				break;
		}
	}
	NFOUT;
	return pool;
}

int nvmeib_alloc_fast_reg_pool(struct nvmeib_dev *dev,
	struct ib_fmr_pool **fmr_pool, struct nvmeib_fr_pool **fr_pool,
	int pool_size, void *memmgr_metrics_ctx)
{
	int rv = 0;

	NFIN;
	if ((unsigned)pool_size > NVMEIB_MAX_FR_POOL_SIZE)
		pool_size = NVMEIB_MAX_FR_POOL_SIZE;
	if (dev->use_fast_reg && dev->has_fr) {
		if (dev->init_from_keeper) {
			_NT(trace_nvmeib_nvmeib_alloc_fast_reg_pool_init_kept, "Init FR pool from @COUNT kept MRs", dev->keeper_frs_info.n_mr);
			*fr_pool = create_fr_pool(dev, dev->pd, dev->keeper_frs_info.n_mr, dev->max_pages_per_mr, 
						  dev->keeper_frs_info.mr_arr, dev->keeper_frs_info.n_mr, NULL, NULL, memmgr_metrics_ctx);
		} else {
			_NT(trace_nvmeib_nvmeib_alloc_fast_reg_pool, "FR pool size @POOL_SIZE", pool_size);
			*fr_pool = loop_fr_fmr_pool_size(alloc_fr_pool, dev, pool_size, memmgr_metrics_ctx);
		}
		if (IS_ERR_OR_NULL(*fr_pool)) {
			rv = PTR_ERR(*fr_pool);
			_NE(error_nvmeib_nvmeib_alloc_fast_reg_pool, "FR pool allocation failed (@RV)", rv);
			*fr_pool = NULL;
			goto out;
		}
	} else if (!dev->use_fast_reg && dev->has_fmr) {
		_NT(trace_1_nvmeib_nvmeib_alloc_fast_reg_pool, "FMR pool size @POOL_SIZE", pool_size);
		*fmr_pool = loop_fr_fmr_pool_size(alloc_fmr_pool, dev, pool_size, memmgr_metrics_ctx);
		if (IS_ERR_OR_NULL(*fmr_pool)) {
			rv = PTR_ERR(*fmr_pool);
			_NE(error_1_nvmeib_nvmeib_alloc_fast_reg_pool, "FMR pool allocation failed (@RV)", rv);
			*fmr_pool = NULL;
			goto out;
		}
	}

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_alloc_fast_reg_pool);

static void fr_pool_percpu_idle_timer_fn(struct timer_list *timer)
{
	struct nvmeib_fr_pool_percpu_cache *pcpu_cache = from_timer(pcpu_cache, timer, idle_timer);
	struct nvmeib_fr_pool *pool = pcpu_cache->pool;
	unsigned long jif = jiffies;
	unsigned long flags;
	int n_spilled = 0;

	local_irq_save(flags);
	if (pcpu_cache->n_free > 0 && time_after(jif, pcpu_cache->last_get_jif + nvmeib_fr_pool_percpu_idle_timeout_ms * HZ / 1000)) {
		/* Per-cpu cache idle timeout, spill all descriptors to global pool */
		_ND(debug_fr_pool_percpu_idle_timer_fn, "Per-cpu cache idle timeout, spilling all descriptors to global pool");
		pcpu_cache->stats.n_spills_idle_timer++;
		spin_lock(&pool->lock);
		list_splice_init(&pcpu_cache->free_list, &pool->free_list);
		n_spilled = pcpu_cache->n_free;
		pool->n_free += pcpu_cache->n_free;
		pcpu_cache->stats.total_spills_idle_timer += pcpu_cache->n_free;
		pcpu_cache->n_free = 0;
		spin_unlock(&pool->lock);
		_ND(debug_1_fr_pool_percpu_idle_timer_fn, 
			"Spilled @COUNT descriptors from per-cpu cache to global pool", n_spilled);
	}
	local_irq_restore(flags);

	/* Restart timer to check again after timeout */
	timer->expires = jif + nvmeib_fr_pool_percpu_idle_timeout_ms * HZ / 1000;
	add_timer_on(timer, pcpu_cache->cpu);
}

static void rebalance_fr_pool_percpu_cache_percpu_fn(void *data)
{
	struct nvmeib_fr_pool *pool = (struct nvmeib_fr_pool *)data;
	struct nvmeib_fr_pool_percpu_cache *pcpu_cache = this_cpu_ptr(pool->percpu_cache);
	struct nvmeib_fr_desc *d;
	LIST_HEAD(spill_list);
	int n_spilled = 0;
	unsigned long flags;

	local_irq_save(flags);
	while (pcpu_cache->n_free > pool->pcpu_low &&
		(d = list_first_entry_or_null(&pcpu_cache->free_list, struct nvmeib_fr_desc, entry))) 
	{
		BUG_ON(pcpu_cache->n_free <= 0);
		list_del(&d->entry);
		pcpu_cache->n_free--;
		list_add(&d->entry, &spill_list);
		n_spilled++;
	}

	if (n_spilled > 0) {
		pcpu_cache->stats.n_spills_rebalance++;
		pcpu_cache->stats.total_spilled_rebalance += n_spilled;
		spin_lock(&pool->lock);
		list_splice(&spill_list, &pool->free_list);
		pool->n_free += n_spilled;
		spin_unlock(&pool->lock);
	}
	_ND(debug_1_rebalance_fr_pool_percpu_cache_percpu_fn, 
		"Spilled @COUNT descriptors from per-cpu cache to global pool", n_spilled);
	local_irq_restore(flags);
}

static void rebalance_fr_pool_percpu_cache_work(struct work_struct *work)
{
	struct nvmeib_fr_pool *pool = container_of(work, struct nvmeib_fr_pool, rebalance_pcpu_cache_work);

	/* spill descriptors from each per-cpu cache to the global pool */
	on_each_cpu(rebalance_fr_pool_percpu_cache_percpu_fn, pool, true);
}

static ssize_t fill_fr_pool_proc_buf(void *arg, char *buf, size_t len)
{
	struct nvmeib_fr_pool *pool = (struct nvmeib_fr_pool *)arg;
	struct {
		char *buf;
		size_t len;
		int count;
		int ntabs;
		const struct nvmeib_json_ops *jops;
	} data = {
		.buf = buf,
		.len = len,
		.count = 0,
		.ntabs = 0,
		.jops = &nvmeib_json_ops,
	};
	u64 n_gets;
	int cpu;

	NFIN;
	CALL_JSON_START_OBJ(&data, NULL);
	CALL_JSON_START_OBJ(&data, "params");
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "size", pool->size);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_free", pool->n_free);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_error", pool->n_error);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "rebalance_pcpu_cache_scheduled_jif", pool->rebalance_pcpu_cache_scheduled_jif);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "jiffies", jiffies);
	CALL_JSON_DATA_UVAL(&data, pool->percpu_cache ? !JSON_LAST_ELEM : JSON_LAST_ELEM, "max_page_list_len", pool->max_page_list_len);
	if (pool->percpu_cache) {
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "pcpu_low", pool->pcpu_low);
		CALL_JSON_DATA_UVAL(&data, JSON_LAST_ELEM, "pcpu_high", pool->pcpu_high);
	}
	CALL_JSON_END_OBJ(&data, !JSON_LAST_ELEM);
	CALL_JSON_START_OBJ(&data, "stats");
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_get_success", pool->stats.n_get_success);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_get_fail", pool->stats.n_get_fail);
	n_gets = pool->stats.n_get_success + pool->stats.n_get_fail;
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "average_get_n_free", n_gets > 0 ? pool->stats.total_get_n_free / n_gets : 0);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_puts", pool->stats.n_puts);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_bind_errors", pool->stats.n_bind_errors);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_reregs_scheduled", pool->stats.n_rereg_scheduled);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_rereg_mr_success", pool->stats.total_rereg_mr_success);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_rereg_mr_fail", pool->stats.total_rereg_mr_fail);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "min_n_free", pool->stats.min_n_free);
	CALL_JSON_DATA_UVAL(&data, JSON_LAST_ELEM, "max_n_error", pool->stats.max_n_error);
	CALL_JSON_END_OBJ(&data, pool->percpu_cache ? !JSON_LAST_ELEM : JSON_LAST_ELEM);
	if (pool->percpu_cache) {
		CALL_JSON_START_ARRAY(&data, "percpu_cache");
		for_each_possible_cpu(cpu) {
			struct nvmeib_fr_pool_percpu_cache *pcpu_cache = per_cpu_ptr(pool->percpu_cache, cpu);
			CALL_JSON_START_OBJ(&data, NULL);
			CALL_JSON_START_OBJ(&data, "params");
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "cpu_id", cpu);
			CALL_JSON_DATA_UVAL(&data, JSON_LAST_ELEM, "last_get_jif", pcpu_cache->last_get_jif);
			CALL_JSON_END_OBJ(&data, !JSON_LAST_ELEM);
			CALL_JSON_START_OBJ(&data, "stats");
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_free", pcpu_cache->n_free);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_get_from_cache_success", pcpu_cache->stats.n_get_from_cache_success);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_get_from_cache_fail", pcpu_cache->stats.n_get_from_cache_fail);
			n_gets = pcpu_cache->stats.n_get_from_cache_success + pcpu_cache->stats.n_get_from_cache_fail;
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "average_get_n_free", n_gets > 0 ? pcpu_cache->stats.total_get_n_free / n_gets : 0);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_get_from_cache_fail_after_refill", pcpu_cache->stats.n_get_from_cache_fail_after_refill);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_put_to_cache", pcpu_cache->stats.n_put_to_cache);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_spills_to_excess_list", pcpu_cache->stats.n_spills_to_excess_list);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_spilled_to_excess_list", pcpu_cache->stats.total_spilled_to_excess_list);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_refills_from_global_pool", pcpu_cache->stats.n_refills_from_global_pool);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_refilled_from_global_pool", pcpu_cache->stats.total_refilled_from_global_pool);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_bind_errors", pcpu_cache->stats.n_bind_errors);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_rereg_scheduled", pcpu_cache->stats.n_rereg_scheduled);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_spills_idle_timer", pcpu_cache->stats.n_spills_idle_timer);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_spills_idle_timer", pcpu_cache->stats.total_spills_idle_timer);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_rebalance_scheduled", pcpu_cache->stats.n_rebalance_scheduled);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_spills_rebalance", pcpu_cache->stats.n_spills_rebalance);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_spilled_rebalance", pcpu_cache->stats.total_spilled_rebalance);
			CALL_JSON_DATA_UVAL(&data, JSON_LAST_ELEM, "max_n_free", pcpu_cache->stats.max_n_free);
			CALL_JSON_END_OBJ(&data, JSON_LAST_ELEM);
			CALL_JSON_END_OBJ(&data, cpu == cpumask_last(&__cpu_possible_mask) ? JSON_LAST_ELEM : !JSON_LAST_ELEM);
		}
		CALL_JSON_END_ARRAY(&data, JSON_LAST_ELEM);
	}
	CALL_JSON_END_OBJ(&data, JSON_LAST_ELEM);
	NFOUT;
	return data.count;
}

/* NOTE: Caller must disable preemption */
static struct nvmeib_fr_desc *get_fr_desc_from_percpu_cache(struct nvmeib_fr_pool *pool)
{
	struct nvmeib_fr_desc *d = NULL;
	struct nvmeib_fr_pool_percpu_cache *pcpu_cache = this_cpu_ptr(pool->percpu_cache);
	unsigned long flags;
	unsigned long jif = jiffies;

	NFIN;
	local_irq_save(flags);
	pcpu_cache->last_get_jif = jif;
	if ((d = list_first_entry_or_null(&pcpu_cache->free_list, struct nvmeib_fr_desc, entry))) {
		list_del_init(&d->entry);
		BUG_ON(pcpu_cache->n_free <= 0);
		pcpu_cache->n_free--;
		pcpu_cache->stats.n_get_from_cache_success++;
	} else {
		pcpu_cache->stats.n_get_from_cache_fail++;
	}
	pcpu_cache->stats.total_get_n_free += pcpu_cache->n_free;
	local_irq_restore(flags);
	NFOUT;
	return d;
}

static int refill_fr_pool_percpu_cache(struct nvmeib_fr_pool *pool)
{
	struct nvmeib_fr_desc *d;
	struct nvmeib_fr_pool_percpu_cache *pcpu_cache = this_cpu_ptr(pool->percpu_cache);
	unsigned long flags;
	int n_refilled = 0;

	NFIN;
	spin_lock_irqsave(&pool->lock, flags);
	pcpu_cache->stats.n_refills_from_global_pool++;
	while (pcpu_cache->n_free < pool->pcpu_low && 
		(d = list_first_entry_or_null(&pool->free_list, struct nvmeib_fr_desc, entry))) 
	{
		list_del(&d->entry);
		pool->n_free--;
		list_add(&d->entry, &pcpu_cache->free_list);
		pcpu_cache->n_free++;
		n_refilled++;
		pcpu_cache->stats.total_refilled_from_global_pool++;
		if (pcpu_cache->n_free > pcpu_cache->stats.max_n_free)
			pcpu_cache->stats.max_n_free = pcpu_cache->n_free;
	}
	spin_unlock_irqrestore(&pool->lock, flags);
	NFOUT;
	return n_refilled;
}
/**
 * nvmeib_fast_reg_pool_get() - obtain a descriptor suitable for
 * fast registration
 * @pool: Pool to obtain descriptor from.
 */
struct nvmeib_fr_desc *nvmeib_fast_reg_pool_get(struct nvmeib_fr_pool *pool)
{
	struct nvmeib_fr_desc *d = NULL;
	unsigned long flags;

	NFIN;
	if (pool->percpu_cache) {
		int n_refilled;
		get_cpu();
		if ((d = get_fr_desc_from_percpu_cache(pool))) {
			_ND(debug_nvmeib_fast_reg_pool_get_percpu_cache, 
				"Got descriptor @PTR from per-cpu cache", d);
			put_cpu();
			goto out;
		}
		_ND(debug_2_nvmeib_fast_reg_pool_get_percpu_cache, "No descriptor found in per-cpu cache, refilling");
		n_refilled = refill_fr_pool_percpu_cache(pool);
		_ND(debug_3_nvmeib_fast_reg_pool_get_percpu_cache, 
			"Refilled @COUNT descriptors from global pool to per-cpu cache", n_refilled);
		if ((d = get_fr_desc_from_percpu_cache(pool))) {
			_ND(debug_4_nvmeib_fast_reg_pool_get_percpu_cache, 
				"Got descriptor @PTR from per-cpu cache after refill", d);
		} else {
			_NT(error_nvmeib_fast_reg_pool_get_percpu_cache, 
				"@IB_DEV_NAME FR Pool: Failed to get descriptor from per-cpu cache after refill",
				pool->pd->device->name);
			spin_lock_irqsave(&pool->lock, flags);
			if (time_after(jiffies, pool->rebalance_pcpu_cache_scheduled_jif + nvmeib_fr_pool_percpu_rebalance_min_interval_ms * HZ / 1000)) {
				struct nvmeib_fr_pool_percpu_cache *pcpu_cache = this_cpu_ptr(pool->percpu_cache);
				/* schedule a work to rebalance the per-cpu caches */
				pool->rebalance_pcpu_cache_scheduled_jif = jiffies;
				pcpu_cache->stats.n_get_from_cache_fail_after_refill++;
				_ND(debug_5_nvmeib_fast_reg_pool_get_percpu_cache, 
					"@IB_DEV_NAME FR Pool: Scheduling rebalance of per-cpu caches", pool->pd->device->name);
				if (schedule_work_on_sys_wq_rand_cpu(&pool->rebalance_pcpu_cache_work)) {
					pcpu_cache->stats.n_rebalance_scheduled++;
				}
			}
			spin_unlock_irqrestore(&pool->lock, flags);
		}
		put_cpu();
		goto out;
	}
	spin_lock_irqsave(&pool->lock, flags);
	if (!list_empty(&pool->free_list)) {
		d = list_first_entry(&pool->free_list, typeof(*d), entry);
		list_del_init(&d->entry);
		pool->n_free--;
		pool->stats.n_get_success++;
		if (pool->n_free < pool->stats.min_n_free)
			pool->stats.min_n_free = pool->n_free;
#ifdef NVMEIBC_DEBUG_FR_LEAK
		list_add(&d->used_entry, &pool->used_list);
#endif
	} else {
		pool->stats.n_get_fail++;
	}
	pool->stats.total_get_n_free += pool->n_free;
	spin_unlock_irqrestore(&pool->lock, flags);

out:
	NFOUT;
	return d;
}
EXPORT_SYMBOL(nvmeib_fast_reg_pool_get);

/* NOTE: Caller must disable preemption */
static void put_fr_desc_to_percpu_cache(struct nvmeib_fr_pool *pool, struct nvmeib_fr_desc **desc, int n)
{
	int i;
	struct nvmeib_fr_pool_percpu_cache *pcpu_cache = this_cpu_ptr(pool->percpu_cache);
	LIST_HEAD(excess_list);
	LIST_HEAD(bind_err_list);
	unsigned long flags;
	int n_returned = 0;
	int n_bind_err = 0;
	int n_spilled = 0;
	bool schedule_rereg = false;

	NFIN;

	local_irq_save(flags);

	for (i = 0; i < n; i++) {
		if (!list_empty(&desc[i]->entry)) {
			_NE(error_2_nvmeib_nvmeib_fast_reg_pool_put, "OOPS, desc @POOL already linked", &desc[i]);
			BUG_ON(1);
		}
		desc[i]->owner = NULL;
		if (desc[i]->bind_err) {
			list_add(&desc[i]->entry, &bind_err_list);
			n_bind_err++;
			pcpu_cache->stats.n_bind_errors++;
			schedule_rereg = true;
		} else {
			list_add(&desc[i]->entry, &pcpu_cache->free_list);
			pcpu_cache->stats.n_put_to_cache++;
			pcpu_cache->n_free++;
			if (pcpu_cache->n_free > pcpu_cache->stats.max_n_free)
				pcpu_cache->stats.max_n_free = pcpu_cache->n_free;
			n_returned++;
		}
		desc[i] = NULL;
	}
	_ND(debug_put_fr_desc_to_percpu_cache, 
		"Returned @COUNT descriptors to per-cpu cache. @COUNT bind errors", n_returned, n_bind_err);

	if (pcpu_cache->n_free > pool->pcpu_high) {
		/* Spill excess descriptors to excess list */
		struct nvmeib_fr_desc *d;
		pcpu_cache->stats.n_spills_to_excess_list++;
		while (pcpu_cache->n_free > pool->pcpu_low &&
		(d = list_first_entry_or_null(&pcpu_cache->free_list, struct nvmeib_fr_desc, entry))) 
		{
			list_del(&d->entry);
			pcpu_cache->n_free--;
			list_add_tail(&d->entry, &excess_list);
			n_spilled++;
			pcpu_cache->stats.total_spilled_to_excess_list++;
		}
		_ND(debug_1_put_fr_desc_to_percpu_cache, 
			"Spilled @COUNT descriptors from per-cpu cache to excess list", n_spilled);
	}

	if (!list_empty(&excess_list) || !list_empty(&bind_err_list)) {
		spin_lock(&pool->lock);
		list_splice(&excess_list, &pool->free_list);
		pool->n_free += n_spilled;
		list_splice(&bind_err_list, &pool->err_list);
		pool->n_error += n_bind_err;
		spin_unlock(&pool->lock);
	}
	if (schedule_rereg && schedule_work_on_sys_wq_rand_cpu(&pool->rereg_work)) {
		pcpu_cache->stats.n_rereg_scheduled++;
	}
	local_irq_restore(flags);

	NFOUT;
}

/**
 * nvmeib_fast_reg_pool_put() - put an FR descriptor back in the
 * free list
 * @pool: Pool the descriptor was allocated from.
 * @desc: Pointer to an array of fast registration descriptor pointers.
 * @n:    Number of descriptors to put back.
 *
 * Note: The caller must already have queued an invalidation request for
 * desc->mr->rkey before calling this function.
 */
void nvmeib_fast_reg_pool_put(struct nvmeib_fr_pool *pool,
	struct nvmeib_fr_desc **desc, int n)
{
	unsigned long flags;
	int i = 0;
	bool schedule_rereg = false;

	NFIN;
	if (pool->percpu_cache) {
		get_cpu();
		put_fr_desc_to_percpu_cache(pool, desc, n);
		put_cpu();
		goto out;
	}
	spin_lock_irqsave(&pool->lock, flags);
	for (i = 0; i < n; i++) {
		if (!desc[i])
			continue;
		if (!list_empty(&desc[i]->entry)) {
			_NE(error_nvmeib_nvmeib_fast_reg_pool_put, "OOPS, desc @POOL already linked", &desc[i]);
			BUG_ON(1);
		}
		if (!desc[i]->bind_err) {
			list_add_tail(&desc[i]->entry, &pool->free_list);
			pool->n_free++;
			pool->stats.n_puts++;
		} else {
			/* Bind error happened so add to error list for later maintenance */
			list_add(&desc[i]->entry, &pool->err_list);
			pool->n_error++;
			if (pool->n_error > pool->stats.max_n_error)
				pool->stats.max_n_error = pool->n_error;
			pool->stats.n_bind_errors++;
			schedule_rereg = true;
		}
	#ifdef NVMEIBC_DEBUG_FR_LEAK
		list_del_init(&desc[i]->used_entry);
	#endif
		desc[i]->owner = NULL;
		desc[i] = NULL;
	}
	if (schedule_rereg && schedule_work_on_sys_wq_rand_cpu(&pool->rereg_work)) {
		pool->stats.n_rereg_scheduled++;
	}
	spin_unlock_irqrestore(&pool->lock, flags);
out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_fast_reg_pool_put);

/**
 * nvmeib_fast_reg_pool_handle_bind_err() - handle a IB_WC_MW_BIND_ERR
 * @pool: Pool the descriptor was allocated from.
 * @rkey: rkey of the MR
 *
 */
int nvmeib_fast_reg_pool_handle_bind_err(struct nvmeib_fr_desc **desc, int n, u32 rkey)
{
	int i, rv = -ENOENT;

	NFIN;
	for (i = 0; i < n; i++) {
		if (desc[i]->mr->rkey == rkey) {
			desc[i]->bind_err = 1;
			rv = 0;
			break;
		}
	}

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_fast_reg_pool_handle_bind_err);


static int fr_desc_mr_rereg(struct nvmeib_fr_pool *pool, struct nvmeib_fr_desc *d)
{
	int rv;
	NFIN;

	if (!d->mr) {
		_NE(error_0_fr_desc_rereg, "No mr");
		rv = -1;
	}
	else if ((rv = ib_dereg_mr(d->mr))) {
		_NE(error_1_fr_desc_rereg, "Failed dereg-mr (@RV)", rv);
		//d->mr = NULL;
		rv = -2;
	}
	else {
		/* Register new MR to replace it */
	#if IB_NEW_FR
		d->mr = ib_alloc_mr(pool->pd, IB_MR_TYPE_MEM_REG, pool->max_page_list_len);
	#else
		d->mr = ib_alloc_fast_reg_mr(pool->pd, pool->max_page_list_len);
	#endif
		if (IS_ERR_OR_NULL(d->mr)) {
			rv = PTR_ERR(d->mr);
			_NE(error_2_fr_desc_rereg, "Failed reg-mr (@RV)", rv);
			d->mr = NULL;
			rv = -3;
		}
		else
			rv = 0;
	}

	NFOUT;
	return rv;
}

/**
 * nvmeib_fast_reg_pool_rereg() - reregister all MRs in error state
 * @pool: Pool the descriptor was allocated from.
 *
 */
static void fr_pool_rereg_work(struct work_struct *work)
{
	struct nvmeib_fr_pool *pool = container_of(work, struct nvmeib_fr_pool, rereg_work);
	struct nvmeib_fr_desc *d, *t;
	unsigned long flags;
	LIST_HEAD(loop_list);
	LIST_HEAD(pass_list);
	LIST_HEAD(fail_list);
	int n_pass = 0;
	int n_fail = 0;
	int i = 0;
	int n_err;
	int rv;
	bool schedule_rereg = false;
	NFIN;

	/* splice out from err-list */
	spin_lock_irqsave(&pool->lock, flags);
	list_splice_init(&pool->err_list, &loop_list);
	WARN_ON(pool->n_error == 0);
	n_err = pool->n_error;
	pool->n_error = 0;
	spin_unlock_irqrestore(&pool->lock, flags);

	_NT(trace_0_nvmeib_fast_reg_pool_rereg,
		"fr-pool=@PTR: rereg @INT MRs (len=@MAX_PAGE_LIST_LEN)",
		pool, n_err, pool->max_page_list_len);

	/* rereg MRs */
	list_for_each_entry_safe(d, t, &loop_list, entry) {
		_NT(trace_1_nvmeib_fast_reg_pool_rereg,
			"[@INT32_02] fr-desc=@PTR, key=@RKEY",
			i, d, d->mr ? d->mr->rkey : -1);
		rv = fr_desc_mr_rereg(pool, d);
		if (!rv) {
			d->bind_err = 0;
			list_move(&d->entry, &pass_list);
			n_pass++;
			pool->stats.total_rereg_mr_success++;
		}
		else {
			list_move(&d->entry, &fail_list);
			n_fail++;
			pool->stats.total_rereg_mr_fail++;
		}
		i++;
	}
	if (n_err != n_pass + n_fail) {
		_NE(trace_2_nvmeib_fast_reg_pool_rereg,
			"e=@INT, p=@INT, f=@INT\n",
			n_err, n_pass, n_fail);
		WARN_ON(1);
	}

	/* splice into proper list */
	spin_lock_irqsave(&pool->lock, flags);
	list_splice_tail(&pass_list, &pool->free_list);
	pool->n_free += n_pass;
	list_splice_tail(&fail_list, &pool->err_list);
	pool->n_error += n_fail;
	schedule_rereg = pool->n_error > 0;
	
	if (schedule_rereg && schedule_work_on_sys_wq_rand_cpu(&pool->rereg_work)) {
		pool->stats.n_rereg_scheduled++;
	}

	spin_unlock_irqrestore(&pool->lock, flags);

	NFOUT;
}

void nvmeib_fast_reg_pool_trace(struct nvmeib_fr_pool *pool)
{
	NFIN;
	_NI(trace_nvmeib_nvmeib_fast_reg_pool_trace, "FR pool (@POOL): size=@SIZE, n_free=@N_FREE",
		pool, pool->size, pool->n_free);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_fast_reg_pool_trace);

void *nvmeib_map_fr(struct nvmeib_dev *nvdev, struct nvmeib_mr_info *info)
{
	struct nvmeib_send_wr inv_wr, fastreg_wr, *wr = NULL;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	struct nvmeib_fr_desc *desc = NULL;
	u8 fr_key;
	u64 addr;
	int rv;
	struct completion *old = NULL;
	unsigned iu_index;
	DECLARE_COMPLETION_ONSTACK(fmr_done);
	const u32 mask = 0x000000ff;

	NFIN;

#if !IB_NEW_FR
	if (rdma_node_get_transport(nvdev->ib_dev->node_type) ==
		RDMA_TRANSPORT_IWARP) {
		_NE(error_nvmeib_nvmeib_map_fr, "Not supported for softiwarp");
		goto out;
	}
#endif

	if (info->iu) {
		old = info->iu->io_done;
		info->iu->io_done = &fmr_done;
		iu_index = info->iu->index;
	}
	else
		iu_index = (u16)info->null_iu_idx;
	_ND(trace_nvmeib_nvmeib_map_fr, "Mapping @N_PAGES pages to fr mr", info->n_pages);
	if (!(desc = nvmeib_fast_reg_pool_get(info->fr_pool))) {
		_NE(error_1_nvmeib_nvmeib_map_fr, "Fail to get FR descriptor from pool");
		goto out;
	}
	desc->owner = info->owner;

	if (!desc->valid) {
		memset(&inv_wr, 0, sizeof(inv_wr));
		nvmeib_send_wr_common(inv_wr).wr_id = nvmeib_encode_wr_id(NVMEIB_LOCAL_INV_WR_ID, iu_index);
		nvmeib_send_wr_common(inv_wr).opcode = IB_WR_LOCAL_INV;
		nvmeib_send_wr_ex(inv_wr).invalidate_rkey = desc->mr->rkey;
		nvmeib_send_wr_set_next(inv_wr, &fastreg_wr);
		wr = &inv_wr;
		/* Bump the key */
		fr_key = (u8)(desc->mr->rkey & mask);
		ib_update_fast_reg_key(desc->mr, ++fr_key);
	}
	else
		wr = &fastreg_wr;
	memset(&fastreg_wr, 0, sizeof(fastreg_wr));
#if IB_NEW_FR
	if (info->use_sg) {
		unsigned int sg_offset = info->offset;
		int rv;

		/* ib_map_mr_sg sets mr->page_size, mr->length and mr->iova */
		if ((rv = ib_map_mr_sg(desc->mr, info->sg, info->count, &sg_offset, nvdev->mr_page_size)) < 0) {
			_NE(err_nvmeib_map_fr_map_mr_sg_fail, 
			    "ib_map_mr_sg failed (@RV) to map @COUNT nents",
			    rv, info->count);
			   rv = -1;
			   goto ret_desc;
		}

		_ND(trace_nvmeib_map_mr_map_mr_sg,
			"Mapped SGL @PTR with @COUNT/@COUNT nents and sg_offset @OFFSET to MR with "
			"rkey @RKEY, iova @IOADDR, offset @OFFSET, length @LENGTH, entries @COUNT",
			info->sg, rv, info->count, sg_offset, desc->mr->rkey, desc->mr->iova, sg_offset, desc->mr->length, rv);
		
		addr = desc->mr->iova;
		info->dma_len = desc->mr->length;
		info->offset = sg_offset;
		/* Return the number of entries mapped to the caller */
		info->count = rv;
	} else {
		/* In 512 byte sectors we have an offset in the first page */
		addr = info->pages[0] + info->offset;
		desc->mr->iova = addr;
		/* In 512 byte sectors the dma_len is not the n_pages */
		desc->mr->length = info->dma_len ?: info->n_pages * nvdev->mr_page_size; //omril: why would info->dma_len be 0?
		desc->mr->page_size = nvdev->mr_page_size;

		if (!nvdev->map_mr_f ||
			nvdev->map_mr_f(nvdev->ib_dev, desc->mr, info->pages, info->n_pages)) {
			_NE(nvmeib_map_fr_e1,
				"ib_map_mr_sg failed to map @INT pages", info->n_pages);
			rv = -1;
			goto ret_desc;
		}
	}
	nvmeib_send_wr_common(fastreg_wr).opcode = IB_WR_REG_MR;
	nvmeib_send_wr_reg(fastreg_wr).mr = desc->mr;
#else
	BUG_ON(info->use_sg);
	memcpy(desc->frpl->page_list, info->pages,
		sizeof(*info->pages) * info->n_pages);
	nvmeib_send_wr_common(fastreg_wr).opcode = IB_WR_FAST_REG_MR;
	nvmeib_send_wr_reg(fastreg_wr).iova_start = addr;
	nvmeib_send_wr_reg(fastreg_wr).page_list = desc->frpl;
	nvmeib_send_wr_reg(fastreg_wr).page_list_len = info->n_pages;
	nvmeib_send_wr_reg(fastreg_wr).page_shift = PAGE_SHIFT;
	/* In 512 byte sectors the dma_len is not the n_pages */
	nvmeib_send_wr_reg(fastreg_wr).length =
		info->dma_len ?: info->n_pages * nvdev->mr_page_size;
#endif
	nvmeib_send_wr_common(fastreg_wr).wr_id =
		nvmeib_encode_wr_id(NVMEIB_FAST_REG_WR_ID, iu_index);
	nvmeib_send_wr_reg_key(fastreg_wr) = desc->mr->rkey;
	nvmeib_send_wr_reg_access(fastreg_wr) = (IB_ACCESS_LOCAL_WRITE |
											 IB_ACCESS_REMOTE_READ |
											 IB_ACCESS_REMOTE_WRITE);

	if (info->iu)
		nvmeib_send_wr_common(fastreg_wr).send_flags = IB_SEND_SIGNALED;

#if ENABLE_SIW
	if (nvdev->dev_type == DT_siw) {
		nvmeib_send_wr_common(fastreg_wr).send_flags |= SIW_IB_SEND_TX_TIMESTAMP;
		if (info->qp->recv_cq == info->qp->send_cq)
			nvmeib_send_wr_common(fastreg_wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
		else
			nvmeib_send_wr_common(fastreg_wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

	if ((rv = ib_post_send(info->qp, nvmeib_send_wr_to_ib_ptr(*wr), &bad_wr)) < 0) {
		_NE(error_2_nvmeib_nvmeib_map_fr, "ib_post_send returned @RV",rv);
		goto ret_desc;
	}
	desc->valid = false;
	info->lkey = desc->mr->lkey;
	info->rkey = desc->mr->rkey;

	/*YK: todo need to handle time out so the completion will not be fired
	  after timeout*/
	rv = info->iu ? wait_for_completion_timeout(&fmr_done, 60 * HZ) : 1;
	if (rv > 0 && (!info->iu || info->iu->io_status == IB_WC_SUCCESS)) {
		info->io_addr = addr;
		goto out;
	}
	else {
		_NE(error_3_nvmeib_nvmeib_map_fr, "failed to register fmr @RV",rv);
		goto ret_desc;
	}

ret_desc:
	nvmeib_fast_reg_pool_put(info->fr_pool, &desc, 1);
	desc = NULL;

out:
	if (info->iu)
		info->iu->io_done = old;
	NFOUT;
	return desc;
}
EXPORT_SYMBOL(nvmeib_map_fr);

void *nvmeib_map_fmr(struct nvmeib_mr_info *info)
{
#if KS_IB_VERBS_SUPPORTS_FMR
	struct ib_pool_fmr *fmr = NULL;
	u64 addr = 0;

	NFIN;
	fmr = ib_fmr_pool_map_phys(info->fmr_pool, info->pages, info->n_pages,
		addr);
	if (IS_ERR_OR_NULL(fmr)) {
		_NE(error_nvmeib_nvmeib_map_fmr, "ib_fmr_pool_map_phys() failed n_pages @N_PAGES - @PTR_ERR",
			info->n_pages, PTR_ERR(fmr));
		fmr = NULL;
		goto out;
	}
	info->io_addr = addr;
	info->lkey = fmr->fmr->lkey;
	info->rkey = fmr->fmr->rkey;

out:
	NFOUT;
	return fmr;
#else
	_NE_dmesg(fmr_usupported_by_kernel, "Unexpected internal error, crashing the operating system to prevent data corruption. Error code: 1055.");
	BUG();
	return NULL;
#endif
}
EXPORT_SYMBOL(nvmeib_map_fmr);

void *nvmeib_map_mr(struct nvmeib_dev *nvdev, struct nvmeib_mr_info *info)
{
	void *rv;

	NFIN;
	BUG_ON(!nvdev->use_fast_reg && info->use_sg);
	rv = nvdev->use_fast_reg ?
		nvmeib_map_fr(nvdev, info) : nvmeib_map_fmr(info);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_map_mr);

static void free_fr(struct nvmeib_dev *nvdev, struct ib_qp *qp,
	struct nvmeib_fr_pool *fr_pool, struct nvmeib_fr_desc *fmr,
	struct nvmeib_iu *iu)
{
	DECLARE_COMPLETION_ONSTACK(fmr_done);
	struct ib_send_wr s_wr;
	IB_DECLARE_BAD_SEND_WR(failed_wr);
	int index;
	int rv = 0;

	NFIN;
	if (iu) {
		index = iu->index;
		iu->io_done = &fmr_done;
	}
	else
		index = -1;
	/* try to release the fmr */
	memset(&s_wr, 0, sizeof(s_wr));
	s_wr.wr_id = nvmeib_encode_wr_id(NVMEIB_LOCAL_INV_WR_ID, index);
	s_wr.opcode = IB_WR_LOCAL_INV;
	s_wr.ex.invalidate_rkey = fmr->mr->rkey;
	s_wr.send_flags = IB_SEND_SIGNALED;

	failed_wr = &s_wr;
	if ((rv = ib_post_send(qp, &s_wr, &failed_wr)))
		_NE(error_nvmeib_free_fr, "ib_post_send returned @RV", rv);
	else if (iu && wait_for_completion_timeout(&fmr_done, 60 * HZ) <= 0)
		_NE(error_1_nvmeib_free_fr, "failed to invalidate fmr");
	nvmeib_fast_reg_pool_put(fr_pool, &fmr, 1);
	NFOUT;
}

void nvmeib_free_mr(struct nvmeib_dev *nvdev, struct ib_qp *qp,
	struct nvmeib_fr_pool *fr_pool, void *fmr, struct nvmeib_iu *iu)
{
	NFIN;
	if (nvdev && fmr) {
		if (nvdev->use_fast_reg)
			free_fr(nvdev, qp, fr_pool, fmr, iu);
		else
#if KS_IB_VERBS_SUPPORTS_FMR
			ib_fmr_pool_unmap(fmr);
#else
			BUG();
#endif
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_free_mr);

void nvmeib_free_ioctx(struct ib_device *dev,
	struct nvmeib_iu *ioctx, int dma_size, enum dma_data_direction dir)
{
	if (!ioctx) {
		NFOUT;
		return;
	}

	nvmeib_numa_iter_dma_free(dev, dma_size, ioctx->buf, ioctx->dma);
	kfree(ioctx);
}
EXPORT_SYMBOL(nvmeib_free_ioctx);

void nvmeib_free_ioctx_ring(struct nvmeib_iu **ioctx_ring,
	struct ib_device *dev, int ring_size, int dma_size,
	enum dma_data_direction dir, struct list_head *free_tx)
{
	int i;

	NFIN;
	if (ioctx_ring) {
		for (i = 0; i < ring_size; ++i)
			nvmeib_free_ioctx(dev, ioctx_ring[i], dma_size, dir);
		kfree(ioctx_ring);
		if (free_tx)
			INIT_LIST_HEAD(free_tx);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_free_ioctx_ring);

static void iu_work(struct workqe_struct *work)
{
	struct nvmeib_iu *ioctx = container_of(work, struct nvmeib_iu, work);

	NFIN;
	if (ioctx->work_handler) {
		ioctx->work_handler(ioctx);
	}
	NFOUT;
}

struct nvmeib_iu *nvmeib_alloc_ioctx(struct ib_device *dev,
	int ioctx_size, int dma_size, enum dma_data_direction dir,
	struct nvmeib_numa_iter *iter)
{
	struct nvmeib_iu *ioctx;
	struct nvmeib_numa_iter _iter;

	if (!iter) {
		nvmeib_numa_iter_init(&_iter, nvmeib_numa_alloc_policy,
							  IBDEV2DMADEV(dev));
		iter = &_iter;
	}

	ioctx = kzalloc(ioctx_size, GFP_KERNEL);
	if (!ioctx)
		goto err;

	ioctx->buf = nvmeib_numa_iter_dma_alloc(iter, dev, dma_size, &ioctx->dma,
	                                        GFP_KERNEL);
	nvmeib_numa_iter_next_rr(iter, 1 << get_order(dma_size));
	if (!ioctx->buf) {
		_NE(error_nvmeib_alloc_ioctx,
		    "nvmeib_numa_iter_dma_alloc returned error for dev=@STR",
		    dev_name(IBDEV2DMADEV(dev)));
		goto err_free_ioctx;
	}

	ioctx->size = dma_size;
	WQ_INIT_WORK(&ioctx->work, iu_work);
	INIT_LIST_HEAD(&ioctx->free_tx_n);

	return ioctx;

err_free_ioctx:
	kfree(ioctx);

err:
	return NULL;
}
EXPORT_SYMBOL(nvmeib_alloc_ioctx);

struct nvmeib_iu **nvmeib_alloc_ioctx_ring(struct ib_device *dev, int ring_size,
	int ioctx_size, int dma_size, enum dma_data_direction dir,
	struct list_head *free_tx, void *priv)
{
	struct nvmeib_iu **ring;
	int i;
	struct nvmeib_numa_iter iter;

	NFIN;

	nvmeib_numa_iter_init(&iter, nvmeib_numa_alloc_policy,
							IBDEV2DMADEV(dev));

	ring = kzalloc(ring_size * sizeof(ring[0]), GFP_KERNEL);
	if (!ring)
		goto out;
	for (i = 0; i < ring_size; ++i) {
		ring[i] = nvmeib_alloc_ioctx(dev, ioctx_size, dma_size, dir, &iter);
		if (!ring[i]) {
			_NE(error_nvmeib_alloc_ioctx_ring, "nvmeib_alloc_ioctx returned NULL for dev=@PTR", dev);
			goto err;
		}
		ring[i]->opcode = NVMEIB_IU_POOL;
		ring[i]->index = i;
		ring[i]->priv = priv;
	}
	if (free_tx)
		for (i = 0; i < ring_size; ++i)
			list_add_tail(&ring[i]->free_tx_n, free_tx);

	goto out;

err:
	while (--i >= 0)
		nvmeib_free_ioctx(dev, ring[i], dma_size, dir);
	kfree(ring);
	ring = NULL;

out:
	NFOUT;
	return ring;
}
EXPORT_SYMBOL(nvmeib_alloc_ioctx_ring);

void *nvmeib_alloc(struct nvmeib_alloc_info *ai,
	u32 size, struct nvmesh_memmgr_metrics *mem_audit)
{
	void *vaddr = NULL;
	int i;

	NFIN;
	memset(ai, 0, sizeof(*ai));
	ai->n = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	ai->pages = kcalloc(ai->n, sizeof(*ai->pages), GFP_KERNEL);
	if (mem_audit)
		nvmesh_memmgr_metric_on_alloc_update(mem_audit, ai->pages? ksize(ai->pages): sizeof(*ai->pages), ai->pages);
	if (!ai->pages) {
		_NE(error_nvmeib_nvmeib_alloc, "Cannot allocate pages array");
		goto out;
	}
	for (i = 0; i < ai->n; ++i) {
		ai->pages[i] = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
		if (mem_audit)
			nvmesh_memmgr_metric_on_alloc_update(mem_audit, PAGE_SIZE, ai->pages[i]);
		if (!ai->pages[i]) {
			_NE(error_2_nvmeib_nvmeib_alloc, "Cannot allocate page");
			goto free_all;
		}
	}

	vaddr = vmap(ai->pages, ai->n, VM_MAP, PAGE_KERNEL);
	if (vaddr)
		goto out;
	else
		goto free_all;

free_all:
	nvmeib_release(ai, vaddr, mem_audit);
	vaddr = NULL;

out:
	NFOUT;
	return vaddr;
}
EXPORT_SYMBOL(nvmeib_alloc);

void nvmeib_release(struct nvmeib_alloc_info *ai,
	void *vaddr, struct nvmesh_memmgr_metrics *mem_audit)
{
	int i, freed_pages __attribute__((unused)) = 0;

	NFIN;
	if (vaddr)
		vunmap(vaddr);

	if (ai->pages) {
		for (i = 0; i < ai->n; ++i) {
			if (ai->pages[i]) {
				if (mem_audit)
					nvmesh_memmgr_metric_on_free_update(mem_audit, PAGE_SIZE);
				__free_page(ai->pages[i]);
				freed_pages++;
			}
		}

		if (mem_audit)
			nvmesh_memmgr_metric_on_free_update(mem_audit, ksize(ai->pages));
		kfree(ai->pages);
		ai->pages = NULL;
	}
	ai->n = 0;
	NFOUT;

}
EXPORT_SYMBOL(nvmeib_release);

void nvmeib_dump_page(void *page)
{
	u8 *p = (u8 *)((u64)page & ~0xfffL);
	u32 q;

	NFIN;
	_ND(trace_nvmeib_nvmeib_dump_page, "page=@PAGE, p=@PAGE", (u64)page, (u64)p);
	for (q = 0; q < 256; ++q) {
		_ND(trace_1_nvmeib_nvmeib_dump_page, "@DB @DB @DB @DB @DB @DB @DB @DB "
				 "@DB @DB @DB @DB @DB @DB @DB @DB",
			p[0], p[1], p[ 2], p[ 3], p[ 4], p[ 5], p[ 6], p[ 7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
		p += 16;
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_dump_page);

/* YR: Should be in a common place and generate human-readable output */
void nvmeib_dump_buf(const void *buf, int len)
{
	const u8 *p = (const u8*)buf;
	int ii, jj, prfx = 0;
	char s[32 + 3*8 +1 + 1];

	for (ii = 0; ii < len; ii++) {
		jj = ii % 8;
		if (!(jj)) {
			memset(s, 0, sizeof(s));
			sprintf(s + jj, "[0x%04x] ", ii);
			prfx = strlen(s);
		}
		sprintf(s + prfx + 3 * jj, "%02x ", p[ii]);
		if (!((ii + 1) % 8))
			_ND(trace_nvmeib_nvmeib_dump_buf, "@DBUF", s);
	}
}
EXPORT_SYMBOL(nvmeib_dump_buf);

enum nvmeib_cq_comp_vec_selection_flags {
	NVMEIB_CQ_COMP_VEC_RSRV_VEC_0	= 1 << 0,
	NVMEIB_CQ_COMP_VEC_INDEX_BASED	= 1 << 1,
	NVMEIB_CQ_COMP_VEC_SAME_SCQ_RCQ	= 1 << 2,
};

uint nvmeib_cq_vec_flags = 0;
module_param_named(cq_vec_flags, nvmeib_cq_vec_flags, uint, 0644);
MODULE_PARM_DESC(cq_vec_flags, "CQ (completion queue) completion-vector selection flags, as follows: Bit 0: Reserve vec 0 for userspace. Bit 1: Index based, set by CQ creator. Bit 2: Use the same vector for SCQ/RCQ.");

uint nvmeib_cq_vec_flags_tcp = 4;
module_param_named(cq_vec_flags_tcp, nvmeib_cq_vec_flags_tcp, uint, 0644);
MODULE_PARM_DESC(cq_vec_flags_tcp, "Same as cq_vec_flags for TCP (SIW) completion queues.");
uint nvmeib_cq_vec_snd_rcv_delta = 0;
module_param_named(cq_vec_snd_rcv_delta, nvmeib_cq_vec_snd_rcv_delta, uint, 0644);
MODULE_PARM_DESC(cq_vec_snd_rcv_delta, "If cq_vec_flags (see above) is set to use index-based selection for the vector, then this value will be the delta between the send and receive queue's vector.");

uint nvmeib_cq_vec_snd_rcv_delta_tcp = 0;
module_param_named(cq_vec_snd_rcv_delta_tcp, nvmeib_cq_vec_snd_rcv_delta_tcp, uint, 0644);
MODULE_PARM_DESC(cq_vec_snd_rcv_delta_tcp, "Same as cq_vec_snd_rcv_delta for TCP (SIW) completion queues.");

static atomic_t __attribute__((unused)) cq_vector_value = ATOMIC_INIT(0);

void nvmeib_cq_vector_get(struct nvmeib_dev *dev, const char *ch_name, unsigned index, int *scq_vector, int *rcq_vector)
{
	uint flags = dev->dev_type == DT_siw ? nvmeib_cq_vec_flags_tcp : nvmeib_cq_vec_flags;
	uint delta = dev->dev_type == DT_siw ? nvmeib_cq_vec_snd_rcv_delta_tcp : nvmeib_cq_vec_snd_rcv_delta;
	bool is_rsrv_v0 = flags & NVMEIB_CQ_COMP_VEC_RSRV_VEC_0 ? 1 : 0;
	bool use_index  = flags & NVMEIB_CQ_COMP_VEC_INDEX_BASED;
	bool same_scq_rcq = flags & NVMEIB_CQ_COMP_VEC_SAME_SCQ_RCQ;
	uint num = dev->num_comp_vectors ?: 8;
	uint mod = num - is_rsrv_v0;
	unsigned rcq_index = index;

	if (scq_vector) {
		*scq_vector = (use_index ? index : atomic_inc_return(&cq_vector_value)) % mod + is_rsrv_v0;
		_NT(trace_nvmeib_cq_vector_get_scq,
		    "Dev @DEV_NAME (@IB_DEV_PTR), SCQ vector=@VECTOR selected for ch=@STR index=@IDX (num=@UINT, mod=@UINT, flags=@INT32_HEX, delta=@UINT)",
		    dev->ib_dev->name, dev->ib_dev, *scq_vector, ch_name, index, num, mod, flags, delta);
		rcq_index += delta;
	}
	if (rcq_vector) {
		*rcq_vector = (scq_vector && same_scq_rcq) ? *scq_vector : (use_index ? rcq_index : atomic_inc_return(&cq_vector_value)) % mod + is_rsrv_v0;
		_NT(trace_nvmeib_cq_vector_get_rcq,
		    "Dev @DEV_NAME (@IB_DEV_PTR), RCQ vector=@VECTOR selected for ch=@STR index=@IDX (num=@UINT, mod=@UINT, flags=@INT32_HEX, delta=@UINT)",
		    dev->ib_dev->name, dev->ib_dev, *rcq_vector, ch_name, index, num, mod, flags, delta);
	}
	/* TBD: corner case where specific cqs are removed such that balance is broken */
}
EXPORT_SYMBOL(nvmeib_cq_vector_get);

/* DEPRECATED */
#if 0
void *nvmeib_alloc_n_map(struct nvmeib_alloc_n_map_info *info)
{
	void *vaddr;

	NFIN;
	if ((vaddr = nvmeib_alloc(info->dev, &info->alloc, info->size))) {
		info->mr.pages = info->alloc.phys;
		info->mr.n_pages = info->alloc.n;
		if (!(info->fmr = nvmeib_map_mr(info->dev, &info->mr)))
			goto free_mem;
		else
			goto out;
	}

free_mem:
	nvmeib_release(info->dev, &info->alloc, vaddr);
	vaddr = NULL;

out:
	NFOUT;
	return vaddr;
}
EXPORT_SYMBOL(nvmeib_alloc_n_map);


void nvmeib_free_n_unmap(void *vaddr, struct nvmeib_alloc_n_map_info *info)
{
	NFIN;
	nvmeib_free_mr(info->dev, info->mr.qp, info->mr.fr_pool, info->fmr,
		info->iu);
	nvmeib_release(info->dev, &info->alloc, vaddr);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_free_n_unmap);
#endif

static LIST_HEAD(client_inst_list);
struct client_inst {
	void *arg;
	void (*cb)(struct nvmeib_local_server *s, void *arg);
	void (*close_cb)(void *arg);
	struct list_head link;
};
static struct nvmeib_local_server *volatile nvmeib_local_server_p = NULL;

bool nvmeib_local_server_up(void)
{
	return nvmeib_local_server_p != NULL;
}
EXPORT_SYMBOL(nvmeib_local_server_up);

void nvmeib_register_local_server(struct nvmeib_local_server *s)
{
	struct client_inst *inst;

	mutex_lock(&cb_lock);
	nvmeib_local_server_p = s;
	list_for_each_entry(inst, &client_inst_list, link)
		if (inst->cb)
			inst->cb(s, inst->arg);
	mutex_unlock(&cb_lock);
}
EXPORT_SYMBOL(nvmeib_register_local_server);

int nvmeib_set_local_server_notification_calbacks(
	void (*cb)(struct nvmeib_local_server *s, void *arg), void *arg,
	void (*close_cb)(void *arg))
{
	struct nvmeib_local_server *s;
	struct client_inst *inst;

	if (cb) {
		if ((inst = kzalloc(sizeof(*inst), GFP_KERNEL))) {
			mutex_lock(&cb_lock);
			s = nvmeib_local_server_p;
			list_add_tail(&inst->link, &client_inst_list);
			inst->arg = arg;
			inst->cb = cb;
			inst->close_cb = close_cb;
			if (s != NULL && cb != NULL)
				(*cb)(s, arg);
			mutex_unlock(&cb_lock);
		}
		return inst ? 0 : -1;
	} else {
		int list_size = 0;
		bool found = false;
		mutex_lock(&cb_lock);
		list_for_each_entry(inst, &client_inst_list, link) {
			list_size++;
			if (inst->arg == arg) {
				list_del(&inst->link);
				kfree(inst);
				found = true;
				break;
			}
		}
		mutex_unlock(&cb_lock);
		WARN(!found, "Mess with callbacks %p not found in %d entries\n", arg, list_size);
		return 0;
	}
}
EXPORT_SYMBOL(nvmeib_set_local_server_notification_calbacks);

bool nvmeib_local_server_close_client(void)
{
	struct client_inst *inst;
	bool ret;

	NFIN;
	mutex_lock(&cb_lock);
	if (nvmeib_local_server_p) {
		_NT(trace_nvmeib_nvmeib_local_server_close_client, "Reset local-srv ops");
		nvmeib_local_server_p = NULL;
	}
	if (list_empty(&client_inst_list)) {
		_NI(trace_1_nvmeib_nvmeib_local_server_close_client, "No local client, releasing server imedialely");
		ret = false;
	} else {
		list_for_each_entry(inst, &client_inst_list, link)
			if (inst->close_cb)
				inst->close_cb(inst->arg);
		ret = true;
	}
	mutex_unlock(&cb_lock);
	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_local_server_close_client);

static LIST_HEAD(server_inst_list);
struct server_inst {
	void *arg;
	void (*cb)(struct nvmeib_local_client *c, void *arg);
	void (*close_cb)(void *arg);
	struct list_head link;
};

// A single global nvmeib_local_client_obj & nvmeib_local_client_p, that changes upon local client registration
static struct nvmeib_local_client nvmeib_local_client_obj;
static struct nvmeib_local_client *volatile nvmeib_local_client_p = NULL;

bool nvmeib_local_client_up(void)
{
	return nvmeib_local_client_p != NULL;
}
EXPORT_SYMBOL(nvmeib_local_client_up);

void nvmeib_register_local_client(struct nvmeib_local_client *c)
{
//	struct server_inst *inst;

//	mutex_lock(&cb_lock);
	nvmeib_local_client_obj = *c;
	nvmeib_local_client_p = &nvmeib_local_client_obj;
	_NT(rvyskw8, "toma_request_f=@PTR", nvmeib_local_client_p->toma_request_f);
//	list_for_each_entry(inst, &server_inst_list, link)
//		if (inst->cb)
//			inst->cb(c, inst->arg);
//	mutex_unlock(&cb_lock);
}
EXPORT_SYMBOL(nvmeib_register_local_client);

int nvmeib_set_local_client_notification_calbacks(
	void (*cb)(struct nvmeib_local_client *c, void *arg), void *arg,
	void (*close_cb)(void *arg))
{
	struct nvmeib_local_client *c;
	struct server_inst *inst;

	if (cb) {
		if ((inst = kzalloc(sizeof(*inst), GFP_KERNEL))) {
			mutex_lock(&cb_lock);
			c = nvmeib_local_client_p;
			list_add_tail(&inst->link, &server_inst_list);
			inst->arg = arg;
			inst->cb = cb;
			inst->close_cb = close_cb;
			if (c != NULL && cb != NULL)
				(*cb)(c, arg);
			mutex_unlock(&cb_lock);
		}
		return inst ? 0 : -1;
	} else {
		int list_size = 0;
		bool found = false;
		mutex_lock(&cb_lock);
		list_for_each_entry(inst, &server_inst_list, link) {
			list_size++;
			if (inst->arg == arg) {
				list_del(&inst->link);
				kfree(inst);
				found = true;
				break;
			}
		}
		mutex_unlock(&cb_lock);
		WARN(!found, "Mess with callbacks %p not found in %d entries\n", arg, list_size);
		return 0;
	}
}
EXPORT_SYMBOL(nvmeib_set_local_client_notification_calbacks);

bool nvmeib_local_client_close_server(void)
{
	struct server_inst *inst;
	bool ret;

	NFIN;
	mutex_lock(&cb_lock);
	if (nvmeib_local_client_p) {
		_NT(trace_nvmeib_nvmeib_local_client_close_server,
			"Reset local-client ops");
		memset(nvmeib_local_client_p, 0, sizeof(*nvmeib_local_client_p));
	}
	if (list_empty(&server_inst_list)) {
		_NI(trace_1_nvmeib_nvmeib_local_client_close_server,
			"No local client, releasing server imedialely");
		ret = false;
	} else {
		list_for_each_entry(inst, &server_inst_list, link)
			if (inst->close_cb)
				inst->close_cb(inst->arg);
		ret = true;
	}
	mutex_unlock(&cb_lock);
	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_local_client_close_server);

struct nvmeib_intr_shaper *nvmeib_intr_shaper_create(u64 frame_size_usecs)
{
	struct nvmeib_intr_shaper *shaper = NULL;
	struct intr_shaper_percpu *pcpu;
	int i;
	NFIN;

	if (!(shaper = kzalloc(sizeof(*shaper), GFP_KERNEL))) {
		_NT(trace_nvmeib_nvmeib_intr_shaper_create, "Fail to allocate memory for intr-shaper");
		goto out;
	}

	shaper->percpu_size = round_up(sizeof(*pcpu), cache_line_size());
	if (!(shaper->percpu =
		  kzalloc(nr_cpu_ids * shaper->percpu_size, GFP_KERNEL))) {		// Todo: Use MAX_NUM_ACTIVE_CPUS
		_NT(trace_1_nvmeib_nvmeib_intr_shaper_create, "Fail to allocate memory for intr-shaper percpu");
		goto free_shaper;
	}

	_NT(trace_2_nvmeib_nvmeib_intr_shaper_create, "loops_per_jiffy @LOOPS_PER_JIFFY, HZ @INT", loops_per_jiffy, HZ);
	shaper->frame_size_nsecs = frame_size_usecs * 1000ULL;

	for_each_possible_cpu(i) {
		pcpu = (struct intr_shaper_percpu *)(shaper->percpu + i *shaper->percpu_size);
		pcpu->max_burst_size_local = nvmeib_intr_shaper_max_burst;
		pcpu->max_percent_cpu_local = nvmeib_intr_shaper_max_pct_cpu;
		pcpu->max_irq_time_usecs_local = nvmeib_intr_shaper_max_irq_time_usecs;
	}
	goto out;

free_shaper:
	kfree(shaper);
	shaper = NULL;

out:
	NFOUT;
	return shaper;
}
EXPORT_SYMBOL(nvmeib_intr_shaper_create);

void nvmeib_intr_shaper_destroy(struct nvmeib_intr_shaper *shaper)
{
	NFIN;
	if (shaper) {
		kfree(shaper->percpu);
		kfree(shaper);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_intr_shaper_destroy);

/* EWMA α=1/16 (i.e. shift right by 4) */
#define NVMEIB_INTR_SHAPER_EWMA_ALPHA_SHIFT 4

static void nvmeib_intr_shaper_calc_percpu(struct nvmeib_intr_shaper *shaper,
					int n_polled,
				    u64 n_ns_spent)
{
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
	(shaper->percpu + get_cpu()*shaper->percpu_size);
	u64 now = local_clock(); /* TSC in units of ns */
	u64 dt, busy;
	u32 inst_load_pct_x1000, ewma, pct;
	s32 diff;
	unsigned long flags;

	NFIN;
	/* Allow it to work with soft-irqs too */
	local_irq_save(flags);

	pcpu->last_burst_size = n_polled;
	pcpu->busy_since_last_ns += n_ns_spent;

	if (unlikely(!pcpu->last_update_ns)) {
		pcpu->last_update_ns = now;
	}

	dt = now - pcpu->last_update_ns;
	if (dt < shaper->frame_size_nsecs) {
		/* Not enough time passed; just reuse previous decision */
		goto out;
	}

	pcpu->max_burst_size_local = READ_ONCE(nvmeib_intr_shaper_max_burst);
	pcpu->max_percent_cpu_local = READ_ONCE(nvmeib_intr_shaper_max_pct_cpu);
	pcpu->max_irq_time_usecs_local = READ_ONCE(nvmeib_intr_shaper_max_irq_time_usecs);

	pcpu->last_update_ns = now;

	busy = pcpu->busy_since_last_ns;
	pcpu->busy_since_last_ns = 0;

	if (!busy) {
		/* No work → decay a bit towards 0 */
		if (pcpu->ewma_load_pct_x1000)
			pcpu->ewma_load_pct_x1000 -= pcpu->ewma_load_pct_x1000 >> NVMEIB_INTR_SHAPER_EWMA_ALPHA_SHIFT; /* α=1/16 */
		goto check_thresh;
	}

	/*
	 * instantaneous_load (% * 1000) ≈ busy/dt * 100 * 1000
	 * => inst_x1000 = busy * 100000 / dt
	 */
	inst_load_pct_x1000 = div64_u64(busy * 100000ULL, dt);
	if (inst_load_pct_x1000 > 100000)
		inst_load_pct_x1000 = 100000; /* clamp at 100% */

	/* EWMA: ewma += α * (inst - ewma), α = 1/16 via shift */
	ewma = pcpu->ewma_load_pct_x1000;
	diff = (s32)inst_load_pct_x1000 - (s32)ewma;
	ewma += diff >> NVMEIB_INTR_SHAPER_EWMA_ALPHA_SHIFT;

	pcpu->ewma_load_pct_x1000 = ewma;

	/* Update statistics */
	pcpu->total_ewma_percent_cpu_x1000 += ewma;
	pcpu->n_calc_ewma_percent_cpu++;
	if (ewma > pcpu->max_ewma_percent_cpu_x1000)
		pcpu->max_ewma_percent_cpu_x1000 = ewma;
	if (pcpu->min_ewma_percent_cpu_x1000 == 0 || ewma < pcpu->min_ewma_percent_cpu_x1000)
		pcpu->min_ewma_percent_cpu_x1000 = ewma;

check_thresh:
	pct = pcpu->ewma_load_pct_x1000 / 1000;

	if (pcpu->last_result == NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP && 
		pct >= pcpu->max_percent_cpu_local + NVMEIB_INTR_SHAPER_OVERLOAD_PCT_MARGIN) 
	{
		pcpu->last_result = NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES;
	} 
	else if (pcpu->last_result == NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES && 
		pct <= pcpu->max_percent_cpu_local - NVMEIB_INTR_SHAPER_OVERLOAD_PCT_MARGIN) 
	{
		pcpu->last_result = NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP;
	}

out:
	local_irq_restore(flags);
	put_cpu();

	NFOUT;
}

void nvmeib_intr_shaper_intr_enter(struct nvmeib_intr_shaper *shaper, enum intr_shaper_intr_type intr_type)
{
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
		(shaper->percpu + smp_processor_id()*shaper->percpu_size);

	if (in_irq()) {
		BUG_ON(pcpu->hw_intr_type != INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->hw_intr_start_ns);
		BUG_ON(pcpu->hw_intr_n_polled);
		pcpu->hw_intr_type = intr_type;
		pcpu->hw_intr_start_ns = local_clock();
	}
	else if (in_softirq()) {
		BUG_ON(pcpu->sw_intr_type != INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->sw_intr_start_ns);
		BUG_ON(pcpu->sw_intr_n_polled);
		pcpu->sw_intr_type = intr_type;
		pcpu->sw_intr_start_ns = local_clock();
	} else {
		/* This can happen for SIW when flushing the queue */
	}
}
EXPORT_SYMBOL(nvmeib_intr_shaper_intr_enter);

void nvmeib_intr_shaper_intr_exit(struct nvmeib_intr_shaper *shaper)
{
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
		(shaper->percpu + smp_processor_id()*shaper->percpu_size);
	struct intr_shaper_percpu_stats *pcpu_stats;
	u64 busy_ns;
	int n_polled;
	unsigned long flags;

	if (in_irq()) {
		BUG_ON(!pcpu->hw_intr_start_ns);
		BUG_ON(pcpu->hw_intr_type == INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->hw_intr_type >= MAX_INTR_SHAPER_INTR_TYPE);
		busy_ns = local_clock() - pcpu->hw_intr_start_ns;
		n_polled = pcpu->hw_intr_n_polled;
		pcpu_stats = &pcpu->stats_per_intr_type[pcpu->hw_intr_type];
	} else if (in_softirq()) {
		BUG_ON(!pcpu->sw_intr_start_ns);
		BUG_ON(pcpu->sw_intr_type == INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->sw_intr_type >= MAX_INTR_SHAPER_INTR_TYPE);
		busy_ns = local_clock() - pcpu->sw_intr_start_ns;
		n_polled = pcpu->sw_intr_n_polled;
		pcpu_stats = &pcpu->stats_per_intr_type[pcpu->sw_intr_type];
	} else {
		/* This can happen for SIW when flushing the queue */
		return;
	}

	/* Update statistics */
	local_irq_save(flags);
	pcpu_stats->total_burst_size += pcpu->last_burst_size;
	if (pcpu->last_burst_size > pcpu_stats->max_burst_size)
		pcpu_stats->max_burst_size = pcpu->last_burst_size;
	if (pcpu_stats->min_burst_size == 0 || pcpu->last_burst_size < pcpu_stats->min_burst_size)
		pcpu_stats->min_burst_size = pcpu->last_burst_size;
	pcpu_stats->total_intr_time_ns += busy_ns;
	if (busy_ns > pcpu_stats->max_intr_time_ns)
	pcpu_stats->max_intr_time_ns = busy_ns;
	if (pcpu_stats->min_intr_time_ns == 0 || busy_ns < pcpu_stats->min_intr_time_ns)
	pcpu_stats->min_intr_time_ns = busy_ns;
	pcpu_stats->n_intrs++;
	local_irq_restore(flags);

	/* Calculate EWMA of CPU load */
	nvmeib_intr_shaper_calc_percpu(shaper, n_polled, busy_ns);

	/* Reset current interrupt status */
	if (in_irq()) {
		pcpu->hw_intr_start_ns = 0;
		pcpu->hw_intr_n_polled = 0;
		pcpu->hw_intr_type = INTR_SHAPER_INTR_TYPE_NONE;
	} else {
		BUG_ON(!in_softirq());
		pcpu->sw_intr_start_ns = 0;
		pcpu->sw_intr_n_polled = 0;
		pcpu->sw_intr_type = INTR_SHAPER_INTR_TYPE_NONE;
	}
}
EXPORT_SYMBOL(nvmeib_intr_shaper_intr_exit);

void nvmeib_intr_shaper_intr_polled(struct nvmeib_intr_shaper *shaper, int n_polled)
{
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
		(shaper->percpu + smp_processor_id()*shaper->percpu_size);
	if (in_irq()) {
		BUG_ON(pcpu->hw_intr_type == INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->hw_intr_type >= MAX_INTR_SHAPER_INTR_TYPE);
		pcpu->hw_intr_n_polled += n_polled;
	} else if (in_softirq()) {
		BUG_ON(pcpu->sw_intr_type == INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->sw_intr_type >= MAX_INTR_SHAPER_INTR_TYPE);
		pcpu->sw_intr_n_polled += n_polled;
	} else {
		/* This can happen for SIW when flushing the queue */
	}
}
EXPORT_SYMBOL(nvmeib_intr_shaper_intr_polled);

bool nvmeib_intr_shaper_intr_should_wake_up_reason(struct nvmeib_intr_shaper *shaper, enum nvmeib_intr_shaper_calc_ret *wake_up_reason)
{
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
		(shaper->percpu + smp_processor_id()*shaper->percpu_size);
	struct intr_shaper_percpu_stats *pcpu_stats = &pcpu->stats_per_intr_type[pcpu->hw_intr_type];
	enum nvmeib_intr_shaper_calc_ret local_wake_up_reason = NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP;
	u64 dt;
	int n_polled;
	unsigned long flags;

	if (in_irq()) {
		BUG_ON(pcpu->hw_intr_type == INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->hw_intr_type >= MAX_INTR_SHAPER_INTR_TYPE);
		n_polled = pcpu->hw_intr_n_polled;
		dt = local_clock() - pcpu->hw_intr_start_ns;
	} else if (in_softirq()) {
		BUG_ON(pcpu->sw_intr_type == INTR_SHAPER_INTR_TYPE_NONE);
		BUG_ON(pcpu->sw_intr_type >= MAX_INTR_SHAPER_INTR_TYPE);
		n_polled = pcpu->sw_intr_n_polled;
		dt = local_clock() - pcpu->sw_intr_start_ns;
	} else {
		/* This can happen for SIW when flushing the queue */
		return false;
	}

	local_irq_save(flags);
	if (n_polled > pcpu->max_burst_size_local) {
		local_wake_up_reason = NVMEIB_INTR_SHAPER_RET_WAKE_UP_BURST;
		goto out;
	}
	if (dt > pcpu->max_irq_time_usecs_local * 1000ULL) {
		local_wake_up_reason = NVMEIB_INTR_SHAPER_RET_WAKE_UP_IRQ_TIME;
		goto out;
	}
	local_wake_up_reason = pcpu->last_result;

out:
	if (local_wake_up_reason != NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP) {
		if (wake_up_reason)
			*wake_up_reason = local_wake_up_reason;
		if (local_wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_BURST)
			pcpu_stats->n_wakeups_burst++;
		else if (local_wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_IRQ_TIME)
			pcpu_stats->n_wakeups_irq_time++;
		else if (local_wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES)
			pcpu_stats->n_wakeups_cycles++;
	}
	local_irq_restore(flags);
	return local_wake_up_reason != NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP;
}
EXPORT_SYMBOL(nvmeib_intr_shaper_intr_should_wake_up_reason);

bool nvmeib_intr_shaper_should_continue_polling(struct nvmeib_intr_shaper *shaper, int n_polled, u64 busy_ns)
{
	int cpu = get_cpu();
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
		(shaper->percpu + cpu*shaper->percpu_size);
	bool continue_polling;
	unsigned long flags;

	local_irq_save(flags);

	/* Calculate EWMA of CPU load */
	nvmeib_intr_shaper_calc_percpu(shaper, n_polled, busy_ns);

	if (n_polled > pcpu->max_burst_size_local) {
		continue_polling = true;
		goto out;
	}

	continue_polling = pcpu->last_result != NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP;

out:
	local_irq_restore(flags);
	put_cpu();
	return continue_polling;
}
EXPORT_SYMBOL(nvmeib_intr_shaper_should_continue_polling);

unsigned int nvmeib_intr_shaper_get_max_burst(struct nvmeib_intr_shaper *shaper)
{
	int cpu = get_cpu();
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
		(shaper->percpu + cpu*shaper->percpu_size);
	unsigned int max_burst_size_local;
	unsigned long flags;

	local_irq_save(flags);
	max_burst_size_local = pcpu->max_burst_size_local;
	local_irq_restore(flags);
	put_cpu();

	return max_burst_size_local;
}
EXPORT_SYMBOL(nvmeib_intr_shaper_get_max_burst);

#define NVMEIB_INTR_SHAPER_PROC_FRMT_VER 1
static int nvmeib_intr_shaper_print_stats_json(struct nvmeib_intr_shaper *shaper, char *buf, size_t len)
{
	int rv = 0;
	int i, last_cpu, j;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	struct intr_shaper_percpu *pcpu;
	struct {
		char *buf;
		size_t len;
		int count;
		int ntabs;
		const struct nvmeib_json_ops *jops;
	} data = {
		.buf = buf,
		.len = len,
		.count = 0,
		.ntabs = 0,
		.jops = jops,
	};

	CALL_JSON_START_OBJ(&data, NULL);
	CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "frame_size_nsecs", shaper->frame_size_nsecs);
	CALL_JSON_START_ARRAY(&data, "percpu");
	last_cpu = cpumask_last(cpu_online_mask);
	for_each_online_cpu(i) {
		pcpu = (struct intr_shaper_percpu *)(shaper->percpu + i *shaper->percpu_size);
		CALL_JSON_START_OBJ(&data, NULL);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "cpu", i);
		CALL_JSON_START_OBJ(&data, "parameters");
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "max_burst_size", pcpu->max_burst_size_local);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "max_percent_cpu", pcpu->max_percent_cpu_local);
		CALL_JSON_DATA_UVAL(&data, JSON_LAST_ELEM, "max_irq_time_usecs", pcpu->max_irq_time_usecs_local);
		CALL_JSON_END_OBJ(&data, !JSON_LAST_ELEM);
		CALL_JSON_START_OBJ(&data, "statistics");
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_ewma_percent_cpu", pcpu->total_ewma_percent_cpu_x1000);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_calc_ewma_percent_cpu", pcpu->n_calc_ewma_percent_cpu);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "max_ewma_percent_cpu", pcpu->max_ewma_percent_cpu_x1000 / 1000);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "min_ewma_percent_cpu", pcpu->min_ewma_percent_cpu_x1000 / 1000);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "avg_ewma_percent_cpu", pcpu->n_calc_ewma_percent_cpu ? pcpu->total_ewma_percent_cpu_x1000 / pcpu->n_calc_ewma_percent_cpu / 1000 : 0);
		CALL_JSON_START_OBJ(&data, "per_intr_type");
		for (j = INTR_SHAPER_INTR_TYPE_NONE + 1; j < MAX_INTR_SHAPER_INTR_TYPE; j++) {
			CALL_JSON_START_OBJ(&data, intr_shaper_intr_type_to_str(j, true));
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_intrs", pcpu->stats_per_intr_type[j].n_intrs);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_intr_time_us", pcpu->stats_per_intr_type[j].total_intr_time_ns / 1000ULL);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "max_intr_time_us", pcpu->stats_per_intr_type[j].max_intr_time_ns / 1000ULL);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "min_intr_time_us", pcpu->stats_per_intr_type[j].min_intr_time_ns / 1000ULL);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "avg_intr_time_us", pcpu->stats_per_intr_type[j].n_intrs ? 
				pcpu->stats_per_intr_type[j].total_intr_time_ns / pcpu->stats_per_intr_type[j].n_intrs / 1000ULL : 0);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "total_burst_size", pcpu->stats_per_intr_type[j].total_burst_size);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "max_burst_size", pcpu->stats_per_intr_type[j].max_burst_size);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "min_burst_size", pcpu->stats_per_intr_type[j].min_burst_size);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "avg_burst_size", pcpu->stats_per_intr_type[j].n_intrs ? pcpu->stats_per_intr_type[j].total_burst_size / pcpu->stats_per_intr_type[j].n_intrs : 0);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_wakeups_burst", pcpu->stats_per_intr_type[j].n_wakeups_burst);
			CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "n_wakeups_cycles", pcpu->stats_per_intr_type[j].n_wakeups_cycles);
			CALL_JSON_DATA_UVAL(&data, JSON_LAST_ELEM, "n_wakeups_irq_time", pcpu->stats_per_intr_type[j].n_wakeups_irq_time);
			CALL_JSON_END_OBJ(&data, j == MAX_INTR_SHAPER_INTR_TYPE - 1 ? JSON_LAST_ELEM : !JSON_LAST_ELEM);
		}
		CALL_JSON_END_OBJ(&data, JSON_LAST_ELEM);
		CALL_JSON_END_OBJ(&data, !JSON_LAST_ELEM);
		CALL_JSON_START_OBJ(&data, "status");
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "last_update_ns", pcpu->last_update_ns);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "last_burst_size", pcpu->last_burst_size);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "busy_since_last_ns", pcpu->busy_since_last_ns);
		CALL_JSON_DATA_UVAL(&data, !JSON_LAST_ELEM, "ewma_load_pct_x1000", pcpu->ewma_load_pct_x1000);
		CALL_JSON_DATA_STR(&data, JSON_LAST_ELEM, "last_result", 
			nvmeib_intr_shaper_calc_ret_to_str(pcpu->last_result));
		CALL_JSON_END_OBJ(&data, JSON_LAST_ELEM);
		CALL_JSON_END_OBJ(&data, last_cpu == i ? JSON_LAST_ELEM : !JSON_LAST_ELEM);
	}
	CALL_JSON_END_ARRAY(&data, JSON_LAST_ELEM);
	data.count += nvmeib_proc_add_json_proc_epilog(NVMEIB_INTR_SHAPER_PROC_FRMT_VER, data.buf + data.count, data.len - data.count);
	CALL_JSON_END_OBJ(&data, JSON_LAST_ELEM);
	rv = data.count;
	return rv;
}

static void intr_shaper_reset_stats_cpu(void *arg)
{
	struct nvmeib_intr_shaper *shaper = arg;
	struct intr_shaper_percpu *pcpu = (struct intr_shaper_percpu *)
		(shaper->percpu + smp_processor_id()*shaper->percpu_size);
	unsigned long flags;

	local_irq_save(flags);
	memset(pcpu->stats_per_intr_type, 0, sizeof(pcpu->stats_per_intr_type));
	local_irq_restore(flags);
}

static void nvmeib_intr_shaper_reset_stats(struct nvmeib_intr_shaper *shaper)
{
	on_each_cpu(intr_shaper_reset_stats_cpu, shaper, true);
}

#undef CALL_JSON_FN
#undef CALL_JSON_DATA_UVAL
#undef CALL_JSON_DATA_STR
#undef CALL_JSON_START_OBJ
#undef CALL_JSON_END_OBJ
#undef CALL_JSON_START_ARRAY
#undef CALL_JSON_END_ARRAY

static ssize_t fill_intr_shaper_stats(void *arg, char *buf, size_t len)
{
	struct nvmeib_intr_shaper *shaper = arg;
	int rv;

	rv = nvmeib_intr_shaper_print_stats_json(shaper, buf, len);
	return rv;
}

static ssize_t reset_intr_shaper_stats(void *arg, char *buf, size_t len)
{
	struct nvmeib_intr_shaper *shaper = arg;
	int reset;
	int rv;

	if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
		rv = -EINVAL;
		goto out;
	}

	nvmeib_intr_shaper_reset_stats(shaper);
	rv = len;

out:
	return rv;
}

int nvmeib_post_recvq(struct nvmeib_recvq *rq, struct ib_qp *qp, struct nvmeib_iu *iu)
{
	struct ib_recv_wr wr = {0};
	IB_DECLARE_BAD_RECV_WR(bad_wr);
	struct ib_sge list;
	int rv = 0;

	list.addr = iu->dma;
	list.length = iu->size;
	list.lkey = rq->dev->mr->lkey;

	wr.next = NULL;
	wr.wr_id = nordda_wr_id_encode(++iu->version, NVMEIB_RECV, iu->index);
	wr.sg_list = &list;
	wr.num_sge = 1;

	if ((rv = ib_post_recv(qp, &wr, &bad_wr))) {
		_NE(error_nvmeib_nvmeib_post_recvq, "ib_post_recv failed(@RV)", rv);
	}
	return rv;
}
EXPORT_SYMBOL(nvmeib_post_recvq);

int nvmeib_init_recvq(struct nvmeib_recvq *rq, struct nvmeib_dev *dev,
		      int rq_queue_size, int rq_msg_size)
{
	int rv = 0;

	NFIN;
	_ND(trace_nvmeib_nvmeib_init_recvq, "Creating RQ of size @RQ_QUEUE_SIZE, msg size @RQ_MSG_SIZE", rq->rq_queue_size, rq->rq_msg_size);
	rq->dev = dev;
	rq->rq_queue_size = rq_queue_size;
	rq->rq_msg_size = rq_msg_size;
	/* JH IOMMU: Changed to DMA_FROM_DEVICE. Bufs used as sink for Remote RDMA_SEND */
	rq->rx_ring = nvmeib_alloc_ioctx_ring(dev->ib_dev,
			rq->rq_queue_size, sizeof(*rq->rx_ring[0]),
			rq->rq_msg_size, DMA_FROM_DEVICE, NULL, dev);

	if (!rq->rx_ring) {
		_NE(error_nvmeib_nvmeib_init_recvq, "OOM: cannot allocate rx_ring.");
		rv = -ENOMEM;
	}

	return rv;
}
EXPORT_SYMBOL(nvmeib_init_recvq);

int nvmeib_fill_recvq(struct nvmeib_recvq *rq, struct ib_qp *qp, int *n_posted)
{
	int rv = 0, i;
	_ND(trace_nvmeib_nvmeib_fill_recvq, "Filling RQ of QP @QP_NUM with @RQ_QUEUE_SIZE WQEs", qp->qp_num, rq->rq_queue_size);
	for (i = 0; i < rq->rq_queue_size; i++) {
		struct nvmeib_iu *iu = rq->rx_ring[i];
		 if ((rv = nvmeib_post_recvq(rq, qp, iu)) < 0) {
			 _NE(error_nvmeib_nvmeib_fill_recvq, "ib_post_recv() failed: @RV", rv);
			goto out;
		 }
	}
out:
	if (n_posted)
		*n_posted = i;
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_fill_recvq);

void nvmeib_free_recvq(struct nvmeib_recvq *rq)
{
	if (rq->rx_ring) {
		/* JH IOMMU: Changed to DMA_FROM_DEVICE. Sink for Remote RDMA_SEND */
		nvmeib_free_ioctx_ring(rq->rx_ring, rq->dev->ib_dev,
		rq->rq_queue_size, rq->rq_msg_size, DMA_FROM_DEVICE, NULL);
	}
}
EXPORT_SYMBOL(nvmeib_free_recvq);

int nvmeib_post_sq_drain(struct ib_qp *qp)
{
	struct nvmeib_send_wr drain_wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;
	/* Post the drain wqe to the send queue */
	memset(&drain_wr, 0, sizeof(drain_wr));
	nvmeib_send_wr_common(drain_wr).wr_id = nvmeib_encode_wr_id(NVMEIB_DRAIN_QUEUE, 0);
	nvmeib_send_wr_common(drain_wr).send_flags = IB_SEND_SIGNALED;
	rv = ib_post_send(qp, nvmeib_send_wr_to_ib_ptr(drain_wr), &bad_wr);
	if (rv)
		_NE(error_nvmeib_nvmeib_post_sq_drain, "ib_post_send failed to post drain wr (@RV) for QPn @QP_NUM", rv, qp->qp_num);
	return rv;
}
EXPORT_SYMBOL(nvmeib_post_sq_drain);

int nvmeib_post_rq_drain(struct ib_qp *qp)
{
	struct ib_recv_wr wr = {0};
	IB_DECLARE_BAD_RECV_WR(bad_wr);
	int rv = 0;
	/* Post the drain wqe to the receive queue */
	wr.wr_id = nvmeib_encode_wr_id(NVMEIB_DRAIN_QUEUE, 0);
	if ((rv = ib_post_recv(qp, &wr, &bad_wr))) {
		_NE(error_nvmeib_nvmeib_post_rq_drain, "ib_post_recv failed(@RV)", rv);
	}
	return rv;
}
EXPORT_SYMBOL(nvmeib_post_rq_drain);

/* Common EC functions to client and server filled as function pointer*/
static struct {
	void (*read_mod_wr)(void*, u64);
} block_ec_dp_funcs = {NULL};
void nvmeib_block_dp_ec_dmd_read_mod_wr(void *dmd_ptr, u64 param)
{
	if (block_ec_dp_funcs.read_mod_wr)
		block_ec_dp_funcs.read_mod_wr(dmd_ptr, param);
}
EXPORT_SYMBOL(nvmeib_block_dp_ec_dmd_read_mod_wr);

void nvmeib_set_block_dp_ec_funcs(void (*read_mod_wr_dmd)(void*, u64))
{
	block_ec_dp_funcs.read_mod_wr = read_mod_wr_dmd;
}
EXPORT_SYMBOL(nvmeib_set_block_dp_ec_funcs);

/* kth stuff*/
static struct nvmeib_public_kth_ft kth_ft;

const struct nvmeib_public_kth_ft * nvmeib_kth_ft(void)
{
	return &kth_ft;
}

bool nvmeib_same_physical_dev(struct nvmeib_dev *d1, struct nvmeib_dev *d2)
{
	bool rv = false;
	struct pci_slot *slot1, *slot2;
	__be64 guid1, guid2;

	NFIN;

	if (!d1 || !d2) {
		rv = false;
		goto out;
	}

	if (d1 == d2) {
		rv = true;
		goto out;
	}

	if (!d1->dev_attr->sys_image_guid) {
		rv = false;
		goto out;
	}

	if(d1->dev_type == DT_siw || d2->dev_type == DT_siw) {
		rv = true;
		goto out;
	}

	slot1 = NVMEIBDEV2PCIDEV(d1)->slot;
	slot2 = NVMEIBDEV2PCIDEV(d2)->slot;
	guid1 = d1->dev_attr->sys_image_guid;
	guid2 = d2->dev_attr->sys_image_guid;

	rv = (guid1 == guid2) && (slot1 == slot2);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_same_physical_dev);

bool nvmeib_is_same_physical_ib_dev(struct ib_device *d1, struct ib_device *d2)
{
	bool rv;
	struct ib_device_attr d1_attr, d2_attr;
	struct pci_slot *slot1, *slot2;
	__be64 guid1, guid2;

	NFIN;

	if (!d1 || !d2) {
		rv = false;
		goto out;
	}

	if (d1 == d2) {
		rv = true;
		goto out;
	}

	if (ib_query_device(d1, &d1_attr)) {
		_NW(warn_0_nvmeib_is_same_physical_ib_dev,
			"Query device @IB_DEVICE failed", d1->name);
		rv = false;
		goto out;
	}
	if (ib_query_device(d2, &d2_attr)) {
		_NW(warn_1_nvmeib_is_same_physical_ib_dev,
			"Query device @IB_DEVICE failed", d2->name);
		rv = false;
		goto out;
	}
	if (!d1_attr.sys_image_guid) {
		rv = false;
		goto out;
	}

	if (nvmeib_get_device_type(d1) == DT_siw || nvmeib_get_device_type(d2) == DT_siw) {
		return true;
	}

	slot1 = IBDEV2PCIDEV(d1)->slot;
	slot2 = IBDEV2PCIDEV(d2)->slot;
	guid1 = d1_attr.sys_image_guid;
	guid2 = d2_attr.sys_image_guid;

	rv = (guid1 == guid2) && (slot1 == slot2);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_is_same_physical_ib_dev);

#ifdef NVMEIB_COUNT_MEM_USAGE
int nvmeib_mem_usage_proc_create(struct proc_dir_entry *proc_dir);
void nvmeib_mem_usage_proc_remove(struct proc_dir_entry *proc_dir);
#endif

static void procs_remove(void)
{
	if (proc_dir) {
		nvmeib_version_proc_remove();
		nvmeib_metrics_meta_proc_remove();
		nvmeib_rdma_proc_remove(proc_dir);
		if (alloc_diag_proc) {
			nvmeib_public_proc_remove(alloc_diag_proc);
			alloc_diag_proc = NULL;
		}
		if (completion_noise_proc) {
			nvmeib_public_proc_remove(completion_noise_proc);
			completion_noise_proc = NULL;
		}
		if (with_local_completion_noise_proc) {
			nvmeib_public_proc_remove(with_local_completion_noise_proc);
			with_local_completion_noise_proc = NULL;
		}
		if (nvmeib_intr_shaper_procfs_ent) {
			nvmeib_public_proc_remove(nvmeib_intr_shaper_procfs_ent);
			nvmeib_intr_shaper_procfs_ent = NULL;
		}

		if (io_pet_writer != NULL) {
			nvmeib_msgloop_remove(io_pet_writer);
			io_pet_writer = NULL;
		}
		if (io_pet_dir != NULL) {
			remove_proc_entry("io.pet", proc_dir);
			io_pet_dir = NULL;
		}

#ifdef NVMEIB_COUNT_MEM_USAGE
		nvmeib_mem_usage_proc_remove(proc_dir);
#endif
		remove_proc_entry(PROCFS_COMMON_STR, NULL);
		proc_dir   = NULL;
	}
	return;
}

static int procs_create(void)
{
	int rv = -1;

	NFIN;
	if (!(proc_dir = proc_mkdir(PROCFS_COMMON_STR, NULL))) {
		_NE(error_nvmeib_procs_create, "Fail to create proc dir");
		goto out;
	}

	if (nvmeib_version_proc_create(proc_dir) < 0) {
		_NE(error_1_nvmeib_procs_create, "Fail to create proc version");
		goto err;
	}

	if (nvmeib_metrics_meta_proc_create(proc_dir) < 0) {
		_NE(error_2_nvmeib_procs_create, "Fail to create proc for metrics meta data");
		goto err;
	}

	if (nvmeib_rdma_proc_create(proc_dir) < 0) {
		_NE(procs_create_e1, "Fail to create proc rdma");
		goto err;
	}

	if (!(alloc_diag_proc = nvmeib_public_proc_create(
	          "alloc_diag", proc_dir, nvmeib_numa_iter_diag_fill, NULL, NULL))) {
		_NE(procs_create_e2, "Fail to create proc alloc_diag");
		goto err;
	}

	if (!(completion_noise_proc = nvmeib_public_proc_create(
	          "completion_noise.json", proc_dir, nvmeib_completion_noise_fill_stats, nvmeib_completion_noise_reset_stats, NULL))) {
		_NE(procs_create_e3, "Fail to create proc completion_noise");
		goto err;
	}

	if (!(with_local_completion_noise_proc = nvmeib_public_proc_create(
	          "completion_noise_local.json", proc_dir, nvmeib_completion_noise_fill_stats_local, nvmeib_completion_noise_reset_stats, NULL))) {
		_NE(procs_create_e4, "Fail to create proc completion_noise_with_local");
		goto err;
	}

	if (!(nvmeib_intr_shaper_procfs_ent = nvmeib_public_proc_create(NVMEIB_INT_SHAPER_PROC_NAME, proc_dir,
		fill_intr_shaper_stats, reset_intr_shaper_stats, nvmeib_intr_shaper)))
   {
	   _NE_dmesg(error_nvmeib_module_init_intr_shaper_proc, "Failed to create intr-shaper proc file");
	   goto err;
   }

   	/* Prepare io.pet msgloop */
	io_pet_dir = proc_mkdir("io.pet", proc_dir);
	if (io_pet_dir == NULL) {
		_NE_dmesg(error_nvmeib_module_init_io_pet_dir, "Failed to create io.pet proc directory.");
		goto err;
	}

	io_pet_writer = nvmeib_msgloop_create("io.pet",io_pet_dir,NULL, NULL, NULL, NULL);
	if (io_pet_writer == NULL) {
		_NE_dmesg(error_nvmeib_module_init_io_pet_writer, "Failed to create io.pet msgloop instance.");
		goto err;
	}

#ifdef NVMEIB_COUNT_MEM_USAGE
	if ((rv = nvmeib_mem_usage_proc_create(proc_dir) < 0)) {
		_NE(err_procs_create_mem_usage, "Failed (@RV) to create proc mem_usage", rv);
		goto err;
	}
#endif

	rv = 0;
	goto out;

err:
	procs_remove();

out:
	NFOUT;
	return rv;
}


#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
struct nvmeib_radix_table {
	spinlock_t lock;
	struct radix_tree_root tree;
};

static struct nvmeib_radix_table crtable;
static struct nvmeib_radix_table srtable;

static void radix_t_init(struct nvmeib_radix_table *t)
{
	memset(t, 0, sizeof(*t));
	spin_lock_init(&t->lock);
	INIT_RADIX_TREE(&t->tree, GFP_ATOMIC);
}

static void radix_t_add(struct nvmeib_radix_table *t, unsigned long key, void *val)
{
	int err;
	unsigned long flags;
	int n = 0;

	_NT(radix_t_add_t1, "Add @LX", key);
	while (n++ < 10) {
		spin_lock_irqsave(&t->lock, flags);
		err = radix_tree_insert(&t->tree, key, val);
		spin_unlock_irqrestore(&t->lock, flags);
		if (err) {
			_NT(radix_t_add_t2, "key=@LX, val=@PTR, err=@RV", key, val, err);
			msleep(100);
		}
		else
			break;
	}
	if (n >= 10) {
		BUG();
	}
}

static void * radix_t_del(struct nvmeib_radix_table *t, unsigned long key)
{
	void *val;
	unsigned long flags;

	_NT(radix_t_del_t1, "Delete @LX", key);
	spin_lock_irqsave(&t->lock, flags);
	val = radix_tree_delete(&t->tree, key);
	spin_unlock_irqrestore(&t->lock, flags);
	return val;
}

static void * radix_t_lookup(struct nvmeib_radix_table *t, unsigned long key)
{
	return radix_tree_lookup(&t->tree, key);
}

void nvmeib_c_tree_add(unsigned long key, void *val)
{
	radix_t_add(&crtable, key, val);
}
EXPORT_SYMBOL(nvmeib_c_tree_add);

void * nvmeib_c_tree_del(unsigned long key)
{
	return radix_t_del(&crtable, key);
}
EXPORT_SYMBOL(nvmeib_c_tree_del);

void * nvmeib_c_tree_lookup(unsigned long key)ptr
{
	return radix_t_lookup(&crtable, key);
}
EXPORT_SYMBOL(nvmeib_c_tree_lookup);

void nvmeib_s_tree_add(unsigned long key, void *val)
{
	radix_t_add(&srtable, key, val);
}
EXPORT_SYMBOL(nvmeib_s_tree_add);

void * nvmeib_s_tree_del(unsigned long key)
{
	return radix_t_del(&srtable, key);
}
EXPORT_SYMBOL(nvmeib_s_tree_del);

void * nvmeib_s_tree_lookup(unsigned long key)
{
	return radix_t_lookup(&srtable, key);
}
EXPORT_SYMBOL(nvmeib_s_tree_lookup);
#endif

#ifdef NVMEIB_COUNT_MEM_USAGE

struct nvmeib_alloc_data {
	enum nvmeib_cnt_mem_type mem_type;
	size_t size;
	size_t alloc_size;
	gfp_t flags;
	void *ptr;
	const void *file;
	int line;
	const void *fn;
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
	const void *bt0;
	const void *bt1;
	const void *bt2;
#endif
	u64 loc_id;
	/* Hashtable by ptr */
	struct hlist_node node;
	int bkt;
	/* Link in all allocations list (used for proc seek) */
	struct list_head all_allocs_link;
};

struct nvmeib_alloc_data_loc {
	enum nvmeib_cnt_mem_type mem_type;
	size_t tot_size;
	unsigned int num_alloc;
	const void *file;
	int line;
	const void *fn;
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
	const void *bt0;
	const void *bt1;
	const void *bt2;
#endif
	u64 loc_id;
	/* Hashtable by location ID */
	struct hlist_node node;
	int bkt;
	/* Link in mem-type list (used for proc seek) */
	struct list_head mem_type_link;
};

struct nvmeib_alloc_data_str {
	const void *ptr;
	char *str;
	unsigned int n_usage;
	/* Hashtable by ptr */
	struct hlist_node node;
};

#define NVMEIB_ALLOC_HASH_BITS	16
#define NVMEIB_ALLOC_HASH_BUCKETS (1 << NVMEIB_ALLOC_HASH_BITS)

#define NVMEIB_ALLOC_LOC_HASH_BITS 16
#define NVMEIB_ALLOC_LOC_HASH_BUCKETS (1 << NVMEIB_ALLOC_LOC_HASH_BITS)

#define NVMEIB_ALLOC_STR_HASH_BITS 16
#define NVMEIB_ALLOC_STR_HASH_BUCKETS (1 << NVMEIB_ALLOC_STR_HASH_BITS)

#define NVMEIB_ALLOC_MAX_BUCKETS MAX3(NVMEIB_ALLOC_HASH_BUCKETS, NVMEIB_ALLOC_LOC_HASH_BUCKETS, NVMEIB_ALLOC_STR_HASH_BUCKETS)

static spinlock_t nvmeib_alloc_hash_guard[NVMEIB_ALLOC_MAX_BUCKETS];
static volatile int nvmeib_alloc_hash_guard_line[NVMEIB_ALLOC_MAX_BUCKETS];
static volatile const char *nvmeib_alloc_hash_guard_fn[NVMEIB_ALLOC_MAX_BUCKETS];

static struct hlist_head nvmeib_alloc_hash_tbl[NVMEIB_ALLOC_HASH_BUCKETS];
static struct hlist_head nvmeib_alloc_loc_hash_tbl[NVMEIB_ALLOC_LOC_HASH_BUCKETS];
static struct hlist_head nvmeib_alloc_str_hash_tbl[NVMEIB_ALLOC_STR_HASH_BUCKETS];

static atomic64_t nvmeib_alloc_total[NVMEIB_CNT_MEM_MAX] =
{ [0 ... (NVMEIB_CNT_MEM_MAX - 1) ] = ATOMIC_INIT(0) };
static atomic64_t nvmeib_alloc_md_total = ATOMIC_INIT(0);
static atomic_t nvmeib_alloc_cnt = ATOMIC_INIT(0);

static struct proc_dir_entry *mem_usage_proc_dir;
static struct nvmeib_public_procfs_seq_ent *mem_usage_procs[NVMEIB_CNT_MEM_MAX];
static struct nvmeib_public_procfs_ent *tot_mem_usage_proc;
static struct nvmeib_public_procfs_seq_ent *all_locs_mem_usage_proc;
static struct nvmeib_public_procfs_seq_ent *all_allocs_mem_usage_proc;

#define PTR_HASH_BKT(ptr) 		(hash_long((unsigned long)ptr, NVMEIB_ALLOC_HASH_BITS))
#define LOC_HASH_BKT(loc_id) 		(hash_long((unsigned long)loc_id, NVMEIB_ALLOC_LOC_HASH_BITS))
#define STR_HASH_BKT(ptr)		(hash_long((unsigned long)ptr, NVMEIB_ALLOC_STR_HASH_BITS))

#define spin_lock_hash_bkt(bkt, flags) do {\
	spin_lock_irqsave(&nvmeib_alloc_hash_guard[bkt], flags); \
	nvmeib_alloc_hash_guard_line[bkt] = __LINE__;\
	nvmeib_alloc_hash_guard_fn[bkt] = __FUNCTION__;\
} while(0)

#define spin_unlock_hash_bkt(bkt, flags) do {\
	nvmeib_alloc_hash_guard_line[bkt] = -1; \
	nvmeib_alloc_hash_guard_fn[bkt] = NULL; \
	spin_unlock_irqrestore(&nvmeib_alloc_hash_guard[bkt], flags); \
} while(0)

static const char *nvmeib_mem_type_str[] = {
	"Kernel",
	"DMA",
	"Whole Pages",
	"Virtual",
	"Kernel(IB)",
	"DMA(IB)",
	"Whole Pages(IB)",
	"Virtual(IB)",
};

static const char *nvmeib_mem_usage_dir_name = "mem_usage";
static const char *nvmeib_tot_mem_usage_proc_name = "total";
static const char *nvmeib_all_mem_usage_proc_name = "all";
static const char *nvmeib_all_allocs_mem_usage_proc_name = "all_allocs";

static const char *nvmeib_mem_type_proc_name[] = {
	"kmem",
	"dma",
	"pages",
	"virt",
	"ib_kmem",
	"ib_dma",
	"ib_pages",
	"ib_virt",
};

static ssize_t fill_tot_mem_usage(void *arg, char *buffer, size_t len)
{
	int count = 0, i;
	size_t tot, tot_kib, tot_mib, tot_gib, md_tot, all_tot = 0;

#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	for (i = 0; i < NVMEIB_CNT_MEM_MAX; i++) {
		tot = atomic64_read(&nvmeib_alloc_total[i]);
		all_tot += tot;
		tot_kib = tot >> 10;
		tot_mib = tot_kib >> 10;
		tot_gib = tot_mib >> 10;
		BUF_ADD("%s: %zu (%zu kiB, %zu MiB, %zu GiB)\n",
			nvmeib_mem_type_str[i], tot, tot_kib, tot_mib, tot_gib);
	}
	md_tot = atomic64_read(&nvmeib_alloc_md_total);
	all_tot += md_tot;
	BUF_ADD("Usage MD: %zu (%zu kiB, %zu MiB, %zu GiB)\n",
			md_tot, md_tot >> 10, md_tot >> 20, md_tot >> 30);
	BUF_ADD("TOTAL (%d): %zu (%zu kiB, %zu MiB, %zu GiB)\n",
			atomic_read(&nvmeib_alloc_cnt), all_tot, all_tot >> 10, all_tot >> 20, all_tot >> 30);

	return count;
#undef BUF_ADD
}

#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
#	define ALL_HDR_STR "ptr,req_size,alloc_size,type,bkt,function,line,file,callstack"
#else
#	define ALL_HDR_STR "ptr,req_size,alloc_size,type,function,line,file"
#endif

static const char *nvmeib_alloc_lookup_str(const void *ptr, int already_locked_bucket);

static inline void all_print_node(struct seq_file *m, struct hlist_node *node)
{
	struct nvmeib_alloc_data *alloc_data = hlist_entry(node, struct nvmeib_alloc_data, node);
	seq_printf(m, "%p,%zu,%zu,%s,%d,%s,%d,%s", alloc_data->ptr, alloc_data->size, alloc_data->alloc_size,
		nvmeib_mem_type_proc_name[alloc_data->mem_type], alloc_data->bkt,
	    nvmeib_alloc_lookup_str(alloc_data->fn, alloc_data->bkt), alloc_data->line, nvmeib_alloc_lookup_str(alloc_data->file, alloc_data->bkt));
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
	seq_printf(m, ",%s<-%s<-%s", nvmeib_alloc_lookup_str(alloc_data->bt0, alloc_data->bkt),
		   nvmeib_alloc_lookup_str(alloc_data->bt1, alloc_data->bkt), nvmeib_alloc_lookup_str(alloc_data->bt2, alloc_data->bkt));
#endif
	seq_printf(m, "\n");
}

static inline int all_get_node_bkt(struct hlist_node *node)
{
	struct nvmeib_alloc_data *alloc_data = hlist_entry(node, struct nvmeib_alloc_data, node);
	return alloc_data->bkt;
}

static inline void all_pre_bkt(int bkt)
{
	spin_lock(&nvmeib_alloc_hash_guard[bkt]);
}

static inline void all_post_bkt(int bkt)
{
	spin_unlock(&nvmeib_alloc_hash_guard[bkt]);
}

DEFINE_PRINT_HASH_TBL_SEQ_OPS_FNS(all, nvmeib_alloc_hash_tbl, NVMEIB_ALLOC_HASH_BUCKETS,
								  ALL_HDR_STR, all_print_node, all_get_node_bkt, all_pre_bkt, all_post_bkt)


static const struct seq_operations proc_print_all_allocs = {
	.start = all_start,
	.next = all_next,
	.stop = all_stop,
	.show = all_show
};

#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
#	define LOC_HDR_STR "total_size,num_alloc,avg_size,function,line,file,callstack,loc_id"
#else
#	define LOC_HDR_STR "total_size,num_alloc,avg_size,function,line,file,loc_id"
#endif

static void nvmeib_cnt_alloc_log_all(void)
{
	int i;
	struct nvmeib_alloc_data_loc *alloc_loc_data;
	for (i = 0; i < NVMEIB_ALLOC_LOC_HASH_BUCKETS; i++) {
		hlist_for_each_entry(alloc_loc_data, &nvmeib_alloc_loc_hash_tbl[i], node) {
#ifndef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
			_NI_dmesg(trace_nvmeib_cnt_alloc_log_all, "@SIZE_T,@COUNT,@SIZE_T,@FUNCTION,@LINENO,@FILE",
				alloc_loc_data->tot_size, alloc_loc_data->num_alloc,
				alloc_loc_data->tot_size / alloc_loc_data->num_alloc,
	     nvmeib_alloc_lookup_str(alloc_loc_data->fn, alloc_loc_data->bkt), alloc_loc_data->line, nvmeib_alloc_lookup_str(alloc_loc_data->file, alloc_loc_data->bkt));
#else
			_NI_dmesg(trace_nvmeib_cnt_alloc_log_all,
				  "@SIZE_T,@COUNT,@SIZE_T,@FUNCTION,@LINENO,@FILE,@FUNCTION<-@FUNCTION<-@FUNCTION",
				alloc_loc_data->tot_size, alloc_loc_data->num_alloc,
				alloc_loc_data->tot_size / alloc_loc_data->num_alloc,
	     nvmeib_alloc_lookup_str(alloc_loc_data->fn, alloc_loc_data->bkt), alloc_loc_data->line, nvmeib_alloc_lookup_str(alloc_loc_data->file, alloc_loc_data->bkt),
				  nvmeib_alloc_lookup_str(alloc_loc_data->bt0, alloc_loc_data->bkt), nvmeib_alloc_lookup_str(alloc_loc_data->bt1, alloc_loc_data->bkt), nvmeib_alloc_lookup_str(alloc_loc_data->bt2, alloc_loc_data->bkt));
#endif
		}
	}
}

static inline void loc_print_node(struct seq_file *m, struct hlist_node *node)
{
	struct nvmeib_alloc_data_loc *alloc_loc_data = hlist_entry(
		node, struct nvmeib_alloc_data_loc, node);
	enum nvmeib_cnt_mem_type mem_type = (long)m->private;
	if (mem_type == NVMEIB_CNT_MEM_MAX || alloc_loc_data->mem_type == mem_type) {
		seq_printf(m, "%zu,%u,%zu,%s,%d,%s", alloc_loc_data->tot_size, alloc_loc_data->num_alloc,
			alloc_loc_data->tot_size / alloc_loc_data->num_alloc,
	     nvmeib_alloc_lookup_str(alloc_loc_data->fn, alloc_loc_data->bkt),
			   alloc_loc_data->line, nvmeib_alloc_lookup_str(alloc_loc_data->file, alloc_loc_data->bkt));
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
		seq_printf(m, ",%s<-%s<-%s",
			   nvmeib_alloc_lookup_str(alloc_loc_data->bt0, alloc_loc_data->bkt),
			   nvmeib_alloc_lookup_str(alloc_loc_data->bt1, alloc_loc_data->bkt),
			   nvmeib_alloc_lookup_str(alloc_loc_data->bt2, alloc_loc_data->bkt));
#endif
		seq_printf(m, ",%llu\n", alloc_loc_data->loc_id);
	}
}

static inline int loc_get_node_bkt(struct hlist_node *node)
{
	struct nvmeib_alloc_data_loc *alloc_loc_data = hlist_entry(
		node, struct nvmeib_alloc_data_loc, node);
	return alloc_loc_data->bkt;
}

static inline void loc_pre_bkt(int bkt)
{
	spin_lock(&nvmeib_alloc_hash_guard[bkt]);
}

static inline void loc_post_bkt(int bkt)
{
	spin_unlock(&nvmeib_alloc_hash_guard[bkt]);
}

DEFINE_PRINT_HASH_TBL_SEQ_OPS_FNS(loc, nvmeib_alloc_loc_hash_tbl, NVMEIB_ALLOC_LOC_HASH_BUCKETS,
								  LOC_HDR_STR, loc_print_node, loc_get_node_bkt, loc_pre_bkt, loc_post_bkt)

static const struct seq_operations proc_print_locs = {
	.start = loc_start,
	.next = loc_next,
	.stop = loc_stop,
	.show = loc_show
};

int nvmeib_mem_usage_proc_create(struct proc_dir_entry *proc_dir)
{
	int rv = 0;
	long i;

	NFIN;
	if (!(mem_usage_proc_dir = proc_mkdir(nvmeib_mem_usage_dir_name, proc_dir))) {
		_NE(nvmeib_mem_usage_proc_create_e1, "Fail to create proc subdir");
		rv = -EFAULT;
		goto out;
	}

	if (!(tot_mem_usage_proc = nvmeib_public_proc_create(nvmeib_tot_mem_usage_proc_name,
		mem_usage_proc_dir, &fill_tot_mem_usage, NULL, NULL))) {
		rv = -EFAULT;
		goto out;
	}

	if (!(all_locs_mem_usage_proc = nvmeib_public_proc_create_seq_data(nvmeib_all_mem_usage_proc_name,
		mem_usage_proc_dir, &proc_print_locs, (void*)NVMEIB_CNT_MEM_MAX))) {
		rv = -EFAULT;
		goto out;
	}

	if (!(all_allocs_mem_usage_proc = nvmeib_public_proc_create_seq_data(nvmeib_all_allocs_mem_usage_proc_name,
		mem_usage_proc_dir, &proc_print_all_allocs, NULL))) {
		rv = -EFAULT;
		goto out;
	}

	for (i = 0; i < NVMEIB_CNT_MEM_MAX; i++) {
		if (!(mem_usage_procs[i] = nvmeib_public_proc_create_seq_data(nvmeib_mem_type_proc_name[i],
			mem_usage_proc_dir, &proc_print_locs, (void *)i))) {
			rv = -EFAULT;
			goto out;
		}
	}
	NFOUT;
out:
	return rv;
}

void nvmeib_mem_usage_proc_remove(struct proc_dir_entry *proc_dir)
{
	int i;

	NFIN;
	for (i = 0; i < NVMEIB_CNT_MEM_MAX; i++) {
		if (mem_usage_procs[i]) {
			nvmeib_public_proc_seq_remove(mem_usage_procs[i]);
			mem_usage_procs[i] = NULL;
		}
	}
	if (tot_mem_usage_proc) {
		nvmeib_public_proc_remove(tot_mem_usage_proc);
		tot_mem_usage_proc = NULL;
	}
	if (all_locs_mem_usage_proc) {
		nvmeib_public_proc_seq_remove(all_locs_mem_usage_proc);
		all_locs_mem_usage_proc = NULL;
	}
	if (all_allocs_mem_usage_proc) {
		nvmeib_public_proc_seq_remove(all_allocs_mem_usage_proc);
		all_allocs_mem_usage_proc = NULL;
	}
	remove_proc_entry(nvmeib_mem_usage_dir_name, proc_dir);

	NFOUT;
}

#pragma push_macro("kmalloc")
#pragma push_macro("kmalloc_array")
#pragma push_macro("kmalloc_node")
#pragma push_macro("kzalloc")
#pragma push_macro("kcalloc")
#pragma push_macro("kstrdup")
#pragma push_macro("kmemdup")
#pragma push_macro("kfree")
//#pragma push_macro("dma_alloc_coherent")
//#pragma push_macro("dma_free_coherent")
#pragma push_macro("ib_dma_alloc_coherent")
#pragma push_macro("ib_dma_free_coherent")
#pragma push_macro("__get_free_pages")
#pragma push_macro("__get_free_page")
#pragma push_macro("get_zeroed_page")
#pragma push_macro("free_page")
#pragma push_macro("free_pages")
#pragma push_macro("vmalloc")
#pragma push_macro("vzalloc")
#pragma push_macro("vfree")
#pragma push_macro("alloc_pages")
#pragma push_macro("alloc_pages_node")

#undef kmalloc
#undef kmalloc_array
#undef kmalloc_node
#undef kzalloc
#undef kcalloc
#undef kstrdup
#undef kmemdup
#undef kfree
//#undef dma_alloc_coherent
#pragma pop_macro("dma_alloc_coherent")
#pragma pop_macro("dma_free_coherent")
//#undef dma_free_coherent
#undef ib_dma_alloc_coherent
#undef ib_dma_free_coherent
#undef __get_free_pages
#undef __get_free_page
#undef get_zeroed_page
#undef free_page
#undef free_pages
#undef vmalloc
#undef vzalloc
#undef kstrdup
#undef vfree

static const void *nvmeib_alloc_add_str(const void *ptr, gfp_t gfp, bool is_bt)
{
	int ptr_hash_bkt = STR_HASH_BKT(ptr);
	struct nvmeib_alloc_data_str *alloc_str_iter, *alloc_str = NULL;
	const void *ret = NULL;
	unsigned long flags;

	spin_lock_hash_bkt(ptr_hash_bkt, flags);
	/* Check to see if already in hash table */
	hlist_for_each_entry(alloc_str_iter, &nvmeib_alloc_str_hash_tbl[ptr_hash_bkt], node) {
		if (alloc_str_iter->ptr == ptr) {
			/* Already in hash-table, inc usage count and return */
			alloc_str_iter->n_usage++;
			ret = ptr;
			break;
		}
	}
	spin_unlock_hash_bkt(ptr_hash_bkt, flags);

	if (ret)
		goto out;

	/* Not already in hash-table - Add */
	if (!(alloc_str = kzalloc(sizeof(*alloc_str), gfp))) {
		/* OOM */
		goto out;
	}
	alloc_str->ptr = ptr;
	alloc_str->n_usage = 1;
	if (!is_bt) {
		/* Not a backtrace point - Just duplicate the string */
		if (!(alloc_str->str = kstrdup(ptr, gfp))) {
			/* Failed to dup string */
			kfree(alloc_str);
			goto out;
		}
	} else {
		if (!(alloc_str->str = kmalloc(128, gfp))) {
			/* Failed to allocate */
			kfree(alloc_str);
			goto out;
		}
		/* Decode ptr to function-location */
		scnprintf(alloc_str->str, 128, "%pS", ptr);
	}
	/* String duplicated - Add to hash table (check to make sure it hasn't been added between the last check) */
	spin_lock_hash_bkt(ptr_hash_bkt, flags);
	/* Check to see if already in hash table */
	hlist_for_each_entry(alloc_str_iter, &nvmeib_alloc_str_hash_tbl[ptr_hash_bkt], node) {
		if (alloc_str_iter->ptr == ptr) {
			/* Already in hash-table, remove created one, inc usage count and return */
			kfree(alloc_str->str);
			kfree(alloc_str);
			alloc_str_iter->n_usage++;
			ret = ptr;
			goto unlock;
		}
	}
	/* Still not added - Add new one */
	hlist_add_head(&alloc_str->node, &nvmeib_alloc_str_hash_tbl[ptr_hash_bkt]);
	ret = ptr;

unlock:
	spin_unlock_hash_bkt(ptr_hash_bkt, flags);
out:
	BUG_ON(!ret);
	return ret;
}

static void nvmeib_alloc_remove_str(const void *ptr)
{
	int ptr_hash_bkt = STR_HASH_BKT(ptr);
	struct nvmeib_alloc_data_str *alloc_str_iter;
	bool found = false;
	unsigned long flags;

	if (!ptr)
		return;

	spin_lock_hash_bkt(ptr_hash_bkt, flags);
	/* Check to see if already in hash table */
	hlist_for_each_entry(alloc_str_iter, &nvmeib_alloc_str_hash_tbl[ptr_hash_bkt], node) {
		if (alloc_str_iter->ptr == ptr) {
			/* Found in hash-table, dec usage count and, if zero, remove */
			found = true;
			alloc_str_iter->n_usage--;
			if (alloc_str_iter->n_usage <= 0) {
				WARN_ON(alloc_str_iter->n_usage < 0);
				hlist_del(&alloc_str_iter->node);
				kfree(alloc_str_iter->str);
				kfree(alloc_str_iter);
			}
			break;
		}
	}
	spin_unlock_hash_bkt(ptr_hash_bkt, flags);
	BUG_ON(!found);
}

static const char *nvmeib_alloc_lookup_str(const void *ptr, int already_locked_bucket)
{
	int ptr_hash_bkt = STR_HASH_BKT(ptr);
	struct nvmeib_alloc_data_str *alloc_str_iter;
	char *ret = "N/A";
	unsigned long flags = 0;

	if (!ptr)
		goto out;

	if (ptr_hash_bkt != already_locked_bucket)
		spin_lock_hash_bkt(ptr_hash_bkt, flags);
	/* Check to see if already in hash table */
	hlist_for_each_entry(alloc_str_iter, &nvmeib_alloc_str_hash_tbl[ptr_hash_bkt], node) {
		if (alloc_str_iter->ptr == ptr) {
			/* Found in hash-table, */
			ret = alloc_str_iter->str;
			break;
		}
	}
	if (ptr_hash_bkt != already_locked_bucket)
		spin_unlock_hash_bkt(ptr_hash_bkt, flags);
out:
	return ret;
}

void *nvmeib_cnt_alloc(enum nvmeib_alloc_type alloc_type, size_t size, gfp_t gfp,
						/* kmalloc_array - size_t n */
						/* kmalloc_node - int n */
						size_t p1,
						/* kstrdup - (char *str) */
						/* kmemdup - (void *p) */
						/* dma_alloc_coherent - (struct device *) */
						/* ib_dma_alloc_coherent - (struct ib_device *) */
						const void *p2,
						/* dma_alloc_coherent - (dma_addr_t *) */
						/* ib_dma_alloc_coherent - (dma_addr_t *) */
						const void *p3,
						const char *file, int line, const char *fn,
						const void *bt0, const void *bt1, const void *bt2, u64 loc_id)
{
	struct nvmeib_alloc_data *alloc_data = NULL, *alloc_data_iter;
	struct nvmeib_alloc_data_loc *loc_data = NULL, *loc_data_iter;
	enum nvmeib_cnt_mem_type mem_type;
	void *ptr = NULL;
	unsigned long flags;
	int ptr_hash_bkt;
	int loc_hash_bkt;
	size_t md_size = sizeof(*alloc_data) + sizeof(*loc_data);
	int alloc_cnt = 2;
	size_t alloc_size;
	bool already_alloc = false;
	struct page *pgs_ptr = NULL;
	gfp_t gfp_alloc_data = (((gfp & GFP_ATOMIC) == GFP_ATOMIC) ? GFP_ATOMIC : (((gfp & GFP_NOWAIT) == GFP_NOWAIT) ? GFP_NOWAIT : GFP_KERNEL));

	if (!(alloc_data = kzalloc(sizeof(*alloc_data), gfp_alloc_data)) ||
		!(loc_data = kzalloc(sizeof(*loc_data), gfp_alloc_data)))
		goto oom;
	alloc_cnt++;

	switch(alloc_type) {
	case NVMEIB_ALLOC_KMALLOC:
		mem_type = NVMEIB_CNT_MEM_KMEM;
		ptr = kmalloc(size, gfp);
		alloc_size = size;
		break;
	case NVMEIB_ALLOC_KMALLOC_ARRAY:
		mem_type = NVMEIB_CNT_MEM_KMEM;
		ptr = kmalloc_array(p1, size, gfp);
		alloc_size = size;
		break;
	case NVMEIB_ALLOC_KMALLOC_NODE:
		mem_type = NVMEIB_CNT_MEM_KMEM;
		ptr = kmalloc_node(size, gfp, p1);
		alloc_size = size;
		break;
	case NVMEIB_ALLOC_KSTRDUP:
		mem_type = NVMEIB_CNT_MEM_KMEM;
		ptr = kstrdup((const char *)p2, gfp);
		alloc_size = size;
		break;
	case NVMEIB_ALLOC_KMEMDUP:
		mem_type = NVMEIB_CNT_MEM_KMEM;
		ptr = kmemdup(p2, size, gfp);
		alloc_size = size;
		break;
	case NVMEIB_ALLOC_DMA_ALLOC:
		mem_type = NVMEIB_CNT_MEM_DMA;
		ptr = dma_alloc_coherent((struct device *)p2, size, (dma_addr_t *)p3, gfp);
		alloc_size = PAGE_SIZE << get_order(size);
		break;
	case NVMEIB_ALLOC_IB_DMA_ALLOC:
		mem_type = NVMEIB_CNT_MEM_DMA;
		ptr = ib_dma_alloc_coherent(((struct ib_device *)p2), size, (dma_addr_t *)p3, gfp);
		alloc_size = PAGE_SIZE << get_order(size);
		break;
	case NVMEIB_ALLOC_GET_PAGES:
		mem_type = NVMEIB_CNT_MEM_PAGES;
		ptr = (void *)__get_free_pages(gfp, p1);
		alloc_size = PAGE_SIZE << p1;
		break;
	case NVMEIB_ALLOC_PAGES:
		mem_type = NVMEIB_CNT_MEM_PAGES;
		pgs_ptr = alloc_pages_current(gfp, p1);
		ptr = page_address(pgs_ptr);
		alloc_size = PAGE_SIZE << p1;
		_ND(trace_nvmeib_cnt_alloc_pages, "alloc_pages order @COUNT pgs_ptr @PTR virtual address @PTR", (int)p1, pgs_ptr, ptr);
		break;
	case NVMEIB_ALLOC_PAGES_NODE:
		{
			unsigned long ul_node = (unsigned long)p3;
			int node = ul_node;
			mem_type = NVMEIB_CNT_MEM_PAGES;
			pgs_ptr = __alloc_pages_nodemask(gfp, p1, node_zonelist(node, gfp), NULL);
			ptr = page_address(pgs_ptr);
			alloc_size = PAGE_SIZE << p1;
			_ND(trace_nvmeib_cnt_alloc_pages_node, "alloc_pages_node order @COUNT node @INDEX pgs_ptr @PTR virtual address @PTR", (int)p1, node, pgs_ptr, ptr);
		}
		break;
	case NVMEIB_ALLOC_VMALLOC:
		mem_type = NVMEIB_CNT_MEM_VIRT;
		ptr = (gfp & __GFP_ZERO) ? vzalloc(size) : vmalloc(size);
		alloc_size = size;
		break;
	case NVMEIB_ALLOC_IB_VERBS_KMEM:
		mem_type = NVMEIB_CNT_MEM_IB_KMEM;
		ptr = (void *)p2;
		alloc_size = size;
		already_alloc = true;
		break;
	case NVMEIB_ALLOC_IB_VERBS_DMA:
		mem_type = NVMEIB_CNT_MEM_IB_DMA;
		ptr = (void *)p2;
		alloc_size = size;
		already_alloc = true;
		break;
	case NVMEIB_ALLOC_IB_VERBS_PAGES:
		mem_type = NVMEIB_CNT_MEM_IB_PAGES;
		ptr = (void *)p2;
		alloc_size = size;
		already_alloc = true;
		break;
	case NVMEIB_ALLOC_IB_VERBS_VIRT:
		mem_type = NVMEIB_CNT_MEM_IB_VIRT;
		ptr = (void *)p2;
		alloc_size = size;
		already_alloc = true;
		break;
	default:
		BUG_ON(1);
	}

	if (!ptr)
		goto oom;

	alloc_cnt++;
	ptr_hash_bkt = PTR_HASH_BKT(ptr);

	alloc_data->ptr = ptr;
	alloc_data->mem_type = mem_type;
	alloc_data->size = size;
	alloc_data->alloc_size = alloc_size;
	alloc_data->flags = gfp;
	alloc_data->loc_id = loc_id;
	alloc_data->bkt = ptr_hash_bkt;
	alloc_data->file = nvmeib_alloc_add_str(file, gfp, false);
	alloc_data->line = line;
	alloc_data->fn = nvmeib_alloc_add_str(fn, gfp, false);
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
	alloc_data->bt0 = nvmeib_alloc_add_str(bt0, gfp, true);
	alloc_data->bt1 = nvmeib_alloc_add_str(bt1, gfp, true);
	alloc_data->bt2 = nvmeib_alloc_add_str(bt2, gfp, true);
#endif

	spin_lock_hash_bkt(ptr_hash_bkt, flags);
	if (!already_alloc) {
		/* Check to see if already in hash table (means we have allocated memory and not seen the free) */
		hlist_for_each_entry(alloc_data_iter, &nvmeib_alloc_hash_tbl[ptr_hash_bkt], node) {
			if (alloc_data_iter->ptr == ptr) {
				/* Pointer not in hash table */
				_NE(nvmeib_cnt_alloc_already_alloc, "ptr @PTR (bkt @INT, alloc_data @PTR) already in hashtable - prev allocation of @SIZE_T bytes in @FUNCTION (@FILE:@LINENO)",
					ptr, ptr_hash_bkt, alloc_data_iter, alloc_data_iter->size,
					nvmeib_alloc_lookup_str(alloc_data_iter->fn, ptr_hash_bkt),
					nvmeib_alloc_lookup_str(alloc_data_iter->file, ptr_hash_bkt),
					alloc_data_iter->line);
				hlist_del(&alloc_data_iter->node);
				atomic64_sub(alloc_data_iter->alloc_size, &nvmeib_alloc_total[alloc_data_iter->mem_type]);
				kfree(alloc_data_iter);
				atomic_sub(2, &nvmeib_alloc_cnt);
				atomic64_sub(sizeof(*alloc_data_iter), &nvmeib_alloc_md_total);
				//TBD: Remove old location data
				break;
			}
		}
	}
	hlist_add_head(&alloc_data->node, &nvmeib_alloc_hash_tbl[ptr_hash_bkt]);
	spin_unlock_hash_bkt(ptr_hash_bkt, flags);

	atomic64_add(alloc_size, &nvmeib_alloc_total[mem_type]);

	/* Add to location grouping */
	loc_hash_bkt = LOC_HASH_BKT(loc_id);
	spin_lock_hash_bkt(loc_hash_bkt, flags);
	hlist_for_each_entry(loc_data_iter, &nvmeib_alloc_loc_hash_tbl[loc_hash_bkt], node) {
		if (loc_data_iter->loc_id == loc_id && loc_data_iter->mem_type == mem_type) {
			/* Found existing location - free memory allocated */
			kfree(loc_data);
			alloc_cnt--;
			md_size -= sizeof(*loc_data);
			/* Update existing allocation */
			loc_data_iter->tot_size += alloc_size;
			loc_data_iter->num_alloc++;
			goto unlock_loc;
		}
	}
	loc_data->mem_type = mem_type;
	loc_data->tot_size = alloc_size;
	loc_data->num_alloc = 1;
	loc_data->file = nvmeib_alloc_add_str(file, gfp, false);;
	loc_data->line = line;
	loc_data->fn = nvmeib_alloc_add_str(fn, gfp, false);
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
	loc_data->bt0 = nvmeib_alloc_add_str(bt0, gfp, true);
	loc_data->bt1 = nvmeib_alloc_add_str(bt1, gfp, true);
	loc_data->bt2 = nvmeib_alloc_add_str(bt2, gfp, true);
#endif
	loc_data->loc_id = loc_id;
	loc_data->bkt = loc_hash_bkt;
	hlist_add_head(&loc_data->node, &nvmeib_alloc_loc_hash_tbl[loc_hash_bkt]);

unlock_loc:
	spin_unlock_hash_bkt(loc_hash_bkt, flags);
	atomic_add(alloc_cnt, &nvmeib_alloc_cnt);
	atomic64_add(md_size, &nvmeib_alloc_md_total);

	goto out;

oom:
	kfree(alloc_data);

out:
	if (pgs_ptr)
		return pgs_ptr;
	return ptr;
}
EXPORT_SYMBOL(nvmeib_cnt_alloc);

void nvmeib_cnt_free(enum nvmeib_free_type free_type, const void *ptr,
					/* dma_free_coherent - (struct device *) */
					/* ib_dma_free_coherent - (struct ib_device *) */
					const void *p1,
					/* dma_free_coherent - (size_t size) */
					/* ib_dma_free_coherent - (size_t size) */
					/* free_pages - (int order) */
					size_t p2,
					/* dma_free_coherent - (dma_addr_t dma_handle) */
					/* ib_dma_free_coherent - (dma_addr_t dma_handle) */
					dma_addr_t p3,
					const char *file, int line, const char *fn)
{
	struct nvmeib_alloc_data *alloc_data_iter, *alloc_data = NULL;
	struct nvmeib_alloc_data_loc *loc_data_iter, *loc_data = NULL;
	unsigned long flags;
	int ptr_hash_bkt = PTR_HASH_BKT(ptr);
	int loc_hash_bkt;
	size_t md_size = 0;
	int free_cnt = 0;

	spin_lock_hash_bkt(ptr_hash_bkt, flags);
	hlist_for_each_entry(alloc_data_iter, &nvmeib_alloc_hash_tbl[ptr_hash_bkt], node) {
		if (alloc_data_iter->ptr == ptr) {
			switch (alloc_data_iter->mem_type) {
			case NVMEIB_CNT_MEM_KMEM:
				if (free_type != NVMEIB_FREE_KFREE)
					continue;
				break;
			case NVMEIB_CNT_MEM_DMA:
				if (free_type != NVMEIB_FREE_DMA_FREE &&
					free_type != NVMEIB_FREE_IB_DMA_FREE)
					continue;
				break;
			case NVMEIB_CNT_MEM_PAGES:
				if (free_type != NVMEIB_FREE_PAGES)
					continue;
				break;
			case NVMEIB_CNT_MEM_VIRT:
				if (free_type != NVMEIB_FREE_VFREE)
					continue;
				break;
			case NVMEIB_CNT_MEM_IB_KMEM:
				if (free_type != NVMEIB_FREE_IB_VERBS_KMEM)
					continue;
				break;
			case NVMEIB_CNT_MEM_IB_DMA:
				if (free_type != NVMEIB_FREE_IB_VERBS_DMA)
					continue;
				break;
			case NVMEIB_CNT_MEM_IB_PAGES:
				if (free_type != NVMEIB_FREE_IB_VERBS_PAGES)
					continue;
				break;
			case NVMEIB_CNT_MEM_IB_VIRT:
				if (free_type != NVMEIB_FREE_IB_VERBS_VFREE)
					continue;
				break;
			default:
				BUG_ON(1);
			}
			alloc_data = alloc_data_iter;
			break;
 		}
	}
	if (alloc_data) {
		md_size += sizeof(*alloc_data);
		hlist_del(&alloc_data->node);
		spin_unlock_hash_bkt(ptr_hash_bkt, flags);
		nvmeib_alloc_remove_str(alloc_data->file);
		nvmeib_alloc_remove_str(alloc_data->fn);
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
		nvmeib_alloc_remove_str(alloc_data->bt0);
		nvmeib_alloc_remove_str(alloc_data->bt1);
		nvmeib_alloc_remove_str(alloc_data->bt2);
#endif
		atomic64_sub(alloc_data->alloc_size, &nvmeib_alloc_total[alloc_data->mem_type]);
		loc_hash_bkt = LOC_HASH_BKT(alloc_data->loc_id);
		spin_lock_hash_bkt(loc_hash_bkt, flags);
		/* Find location data */
		hlist_for_each_entry(loc_data_iter, &nvmeib_alloc_loc_hash_tbl[loc_hash_bkt], node) {
			if (loc_data_iter->loc_id == alloc_data->loc_id &&
				loc_data_iter->mem_type == alloc_data->mem_type) {
				loc_data = loc_data_iter;
				break;
			}
		}
		if (loc_data) {
			loc_data->tot_size -= alloc_data->alloc_size;
			loc_data->num_alloc--;
			if (loc_data->num_alloc <= 0) {
				/* No more allocations at this location, remove */
				BUG_ON(loc_data->tot_size != 0);
				WARN_ON(loc_data->num_alloc < 0);
				hlist_del(&loc_data->node);
				spin_unlock_hash_bkt(loc_hash_bkt, flags);
				nvmeib_alloc_remove_str(loc_data->file);
				nvmeib_alloc_remove_str(loc_data->fn);
#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
				nvmeib_alloc_remove_str(loc_data->bt0);
				nvmeib_alloc_remove_str(loc_data->bt1);
				nvmeib_alloc_remove_str(loc_data->bt2);
#endif
				md_size += sizeof(*loc_data);
				kfree(loc_data);
				free_cnt++;
			} else {
				spin_unlock_hash_bkt(loc_hash_bkt, flags);
			}
		} else {
			spin_unlock_hash_bkt(loc_hash_bkt, flags);
			_NE(nvmeib_cnt_free_e1,
				"loc_data for id @INT_ULLONG (bkt @INT) not found",
				alloc_data->loc_id, loc_hash_bkt);
			BUG();
		}
	} else {
		spin_unlock_hash_bkt(ptr_hash_bkt, flags);
		if (free_type <= NVMEIB_FREE_VFREE) {
			/* Pointer not in hash table */
			_NE(nvmeib_cnt_free_e2, "ptr @PTR (bkt @INT) not found!",
				ptr, ptr_hash_bkt);
			BUG_ON(1);
		}
	}

	switch(free_type) {
	case NVMEIB_FREE_KFREE:
		if (alloc_data)
			kfree(alloc_data->ptr);
		break;
	case NVMEIB_FREE_DMA_FREE:
		if (alloc_data) {
			if ((size_t)p2 != alloc_data->size) {
				_NE(nvmeib_cnt_free_e3, "Size mismatch @@BUFF_SIZE != @@BUFF_SIZE",
					(size_t)p2, alloc_data->size);
				WARN_ON(1);
			}
			dma_free_coherent((struct device *)p1, p2,
								alloc_data->ptr, p3);
		}
		break;
	case NVMEIB_FREE_IB_DMA_FREE:
		if (alloc_data) {
			if ((size_t)p2 != alloc_data->size) {
				_NE(nvmeib_cnt_free_e4, "Size mismatch @BUFF_SIZE != @BUFF_SIZE",
					(size_t)p2, alloc_data->size);
				WARN_ON(1);
			}
			nvmeib_public_ib_dma_free_coherent(((struct ib_device *)p1), p2,
								alloc_data->ptr, p3);
		}
		break;
	case NVMEIB_FREE_PAGES:
		_ND(trace_nvmeib_cnt_free_pages, "free_pages ptr @PTR order @COUNT",
		    ptr, (int)p2);
		if (alloc_data && (PAGE_SIZE << p2) != alloc_data->size) {
			_NE(nvmeib_cnt_free_e5, "Size mismatch @BUFF_SIZE != @BUFF_SIZE",
			   PAGE_SIZE << p2, alloc_data->size);
			WARN_ON(1);
		}
		free_pages((unsigned long)ptr, p2);
		break;
	case NVMEIB_FREE_VFREE:
		vfree(ptr);
		break;
	case NVMEIB_FREE_IB_VERBS_KMEM:
	case NVMEIB_FREE_IB_VERBS_DMA:
	case NVMEIB_FREE_IB_VERBS_PAGES:
	case NVMEIB_FREE_IB_VERBS_VFREE:
		break;
	default:
		BUG_ON(1);
	}
	free_cnt++;
	kfree(alloc_data);
	free_cnt++;
	atomic_sub(free_cnt, &nvmeib_alloc_cnt);
	atomic64_sub(md_size, &nvmeib_alloc_md_total);
}
EXPORT_SYMBOL(nvmeib_cnt_free);

#pragma pop_macro("kmalloc")
#pragma pop_macro("kmalloc_array")
#pragma pop_macro("kmalloc_node")
#pragma pop_macro("kzalloc")
#pragma pop_macro("kcalloc")
#pragma pop_macro("kstrdup")
#pragma pop_macro("kmemdup")
#pragma pop_macro("kfree")
#pragma pop_macro("dma_alloc_coherent")
#pragma pop_macro("dma_free_coherent")
#pragma pop_macro("ib_dma_alloc_coherent")
#pragma pop_macro("ib_dma_free_coherent")
#pragma pop_macro("__get_free_pages")
#pragma pop_macro("__get_free_page")
#pragma pop_macro("get_zeroed_page")
#pragma pop_macro("free_page")
#pragma pop_macro("free_pages")
#pragma pop_macro("vmalloc")
#pragma pop_macro("vzalloc")
#pragma pop_macro("vfree")
#pragma pop_macro("alloc_pages")
#pragma pop_macro("alloc_pages_node")

#endif

static atomic64_t global_uid = ATOMIC64_INIT(0);
u64 nvmeib_get_guid(void)
{
	return atomic64_inc_return(&global_uid);
}
EXPORT_SYMBOL(nvmeib_get_guid);

static void __print_hooray(bool is_start, const char* mod_name)
{	/* Hooray */
	const char *ur = ((is_start) ? "registered" : "unregistered");		// Consider up/down
	struct timeval time;
	unsigned long local_time;
	struct rtc_time tm;
	do_gettimeofday(&time);
	local_time = (u32)(time.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time_to_tm(local_time, &tm);

	_NI_to_user(nvmeib_common_hooray,
			 "module", "Module @STR. Module: @STR. "
					   "Timestamp: @TM_YEAR-@TM_MON-@TM_MDAY @TM_HOUR:@TM_MIN:@TM_SEC. "
					   "Internal version for support cases: @COMMIT_ID_LONG",
			 ur, mod_name,
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			 (unsigned long)COMMIT_ID); // Hooray //. Error code: 0
}

static int __init nvmeib_module_init(void) /* Constructor */
{
	int ret;
	struct nvmeib_public_hwdev *dev;

	pr_info("NVMesh Init started\n");	//. Error code:
	nvmeib_set_tcp_base_port_id();
	nvmeib_set_tcp_num_ports();
	nvmeib_numa_iter_diag_store_init();
	memset(&kth_ft, 0, sizeof(kth_ft));
	nvmeib_public_kth_fill_ft(&kth_ft);

#if defined(IO_POLL_THREAD) && IO_POLL_THREAD
	memset(&intr_poller_ft, 0, sizeof(intr_poller_ft));
	nvmeib_public_intr_poller_fill_ft(&intr_poller_ft);
#endif

	ret = fr_pool_cache_init();
	if (ret)
		goto err_out;
	
	ret = nvmeib_rdma_register_net_notifiers();
	if (ret)
		goto err_fr_pool_cache;

	/*
	module-init of common-public - init &public_hwdev list
	module-init of common-public-xxx - alloc, populate pops add to public_hwdev list
	module-init of common - alloc, populate ops and add to common_hwdev list(nvmeib_ibdr_dev_init)
	+ copy pops from public_hwdev list
	nvmeib_init - call get-ops
	Run time - use the common_hwdev list
	*/

	nvmeib_ibdr_dev_init(nvmeib_mlx_on_demand_paging());
	list_for_each_entry(dev, nvmeib_public_get_hwdevs(), list) {
		if((ret = nvmeib_ibdr_hwdev_pops_set(dev->type, dev->m, dev->ops))) {
			goto err_net_notify;
		}
	}

	if (!(nvmeib_intr_shaper = nvmeib_intr_shaper_create(NVMEIB_FRAME_SIZE_USECS)))
	{
		_NE_dmesg(error_nvmeib_module_init_intr_shaper, "Failed to allocate interrupts shaper");
		goto err_net_notify;
	}

	ret = nvmeib_wd_init();
	if (ret)
		goto err_shaper;

	ret = procs_create();
	if (ret < 0)
		goto err_wd;

	/* Initialize system workqueue */
	nvmeib_system_wq = nvmeib_pcpu_wq_create("nvmesh_system_wq");
	if (!nvmeib_system_wq) {
		ret = -ENOMEM;
		goto err_procs;
	}

	ret = nvmeib_init_traces(proc_dir);
	if (ret)
		goto err_system_wq;

	ret = nvmeib_completion_noise_init();
	if (ret)
		goto err_wd;

	{
		union nvmeib_version ver = (union nvmeib_version)nvmeib_version_get();
		/* Print greetting as soon as trace engine is available */
		_NI(nvmeib_common_greeting, "module: " NVMEIB_VERSION_TRACE_FMT(), NVMEIB_VERSION_PRINT_ARG(&ver));
	}

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	radix_t_init(&crtable);
	radix_t_init(&srtable);
#endif
	if (0) {
		goto err_procs;
	}
	__print_hooray(true, PROCFS_COMMON_STR);
	return 0;

err_procs:
	procs_remove();

err_system_wq:
	nvmeib_pcpu_wq_destroy(nvmeib_system_wq);
	nvmeib_system_wq = NULL;
	goto err_procs;

err_wd:
	nvmeib_wd_exit();

err_shaper:
	nvmeib_intr_shaper_destroy(nvmeib_intr_shaper);
	nvmeib_intr_shaper = NULL;

err_net_notify:
	nvmeib_rdma_unregister_net_notifiers();

err_fr_pool_cache:
	fr_pool_cache_exit();

err_out:
	nvmeib_numa_iter_diag_store_destroy();
	return ret;
}

static void __exit nvmeib_module_exit(void) /* Destructor */
{
	extern void nvmeib_stop_traces(void);

	/* Cleanup system workqueue */
	nvmeib_pcpu_wq_destroy(nvmeib_system_wq);
	nvmeib_system_wq = NULL;

	nvmeib_completion_noise_exit();
	nvmeib_intr_shaper_destroy(nvmeib_intr_shaper);
	nvmeib_wd_exit();
	nvmeib_numa_iter_diag_store_destroy();
	nvmeib_ibdr_dev_cleanup();
	nvmeib_rdma_unregister_net_notifiers();
	fr_pool_cache_exit();
	__print_hooray(false, PROCFS_COMMON_STR);

#ifdef NVMEIB_COUNT_MEM_USAGE
	nvmeib_cnt_alloc_log_all();
#endif

	_NT(trace_nvmeib_module_exit,
	    "********************  Last Trace @LAST_TRACE*******************", '*');
	nvmeib_unreg_tracer_public_api();
	nvmeib_stop_traces();
	procs_remove();
}

#if 0
static void manipulate_mlx5_wc(u64 *val, int opcode) {
	NFIN;
	switch (opcode) {
	case IB_WC_COMP_SWAP:
	case IB_WC_MASKED_COMP_SWAP:
		*val = be64_to_cpu(*val);
		break;
	default:
		break;
	}

	NFOUT;
}

/*handles value returned by send completion*/
void nvmeib_net_to_cpu(struct ib_qp *qp, int opcode, u64 *val) {
	NFIN;
	switch (nvmeib_get_device_type(qp->device)) {
	case DT_mlx5:
		manipulate_mlx5_wc(val, opcode);
		break;
	default:
	case DT_mlx4:
	case DT_bnxt_re:
		break;
	}

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_net_to_cpu);
#endif

#define DEVICE_CALL(_ibdev, cb, ...) \
({ \
	struct nvmeib_device_public_ops *pops; \
	int rv; \
	\
	pops = nvmeib_ibdr_hwdev_pops_get(_ibdev); \
	if (!IS_ERR_OR_NULL(pops)) {\
		if (pops->cb) \
			rv = pops->cb(__VA_ARGS__); \
		else \
			rv = -ENOSYS; \
		nvmeib_ibdr_hwdev_pops_put(pops); \
	} \
	else \
		rv = PTR_ERR(pops);\
	rv; \
 })

int nvmeib_mem_unmapn_n_free(struct nvmeib_alloc_n_map *mem) {
	int rv = -1;
	NFIN;
	/* Call the inner unmap_n_free to unmap and free the mr */
	rv = DEVICE_CALL(mem->pd->device,
					 unmapn_n_free,
					 mem);
	if (mem->use_dma_pages) {
		goto out;
	}

	/* Remove the dma map of the sgl */
	nvmeib_public_ib_dma_unmap_sg(mem->pd->device,
				mem->mem_table.sgl,
				mem->mem_table.nents,
				mem->dma_dir);
	/* free the sgl */
	sg_free_table(&mem->mem_table);

	if (mem->allocated) {
		/* free the pages */
		nvmeib_free_pages_respecting_numa_policy(mem);
	}
out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_mem_unmapn_n_free);

void nvmeib_mem_vunmap_n_free(struct nvmeib_alloc_n_map *mem, void *vaddr)
{
	NFIN;

	if (!mem)
		goto out;

	if (vaddr)
		vunmap(vaddr);

	nvmeib_mem_unmapn_n_free(mem);

	mem->mr = NULL;

out:
	NFOUT;
	return;
}
EXPORT_SYMBOL(nvmeib_mem_vunmap_n_free);

int nvmeib_mem_alloc_n_map(struct nvmeib_alloc_n_map *mem) {
	bool allocate = !mem->pages;
	int rv;
	NFIN;
	if (!mem->n_pages) {
		_NE(mem_alloc_n_map_inv_n_pages, "n_pages must be > 0");
		rv = -EINVAL;
		goto out;
	}
	if (mem->use_dma_pages) {
		if (!mem->dma_pages) {
			_NE(mem_alloc_n_map_dma_pages_null, "dma_pages must be non-null");
			rv = -EINVAL;
			goto out;
		}
		goto dev_call;
	}
	if (allocate) {
		/* Allocate the pages using the numa policy */
		mem->alloc_policy = nvmeib_numa_alloc_policy;

		rv = nvmeib_alloc_pages_respecting_numa_policy(
		    mem, GFP_ALLOC_N_MAP);

		if (rv) {
			_NT(err_nvmeib_mem_alloc_n_map_pgs_alloc_fail,
			    "Failed to allocate @INT pages", mem->n_pages);
			goto out;
		}
	}
	/* Create an sgl from the pages
	 * (this allows us to call dma_ops->map_sg which does a lot of magic for iommu
	 * that we don't want to have to replicate) */
	if ((rv = sg_alloc_table_from_pages(&mem->mem_table, mem->pages, mem->n_pages, 0,
		mem->n_pages << PAGE_SHIFT, GFP_KERNEL)) < 0) {
		_NE(err_nvmeib_mem_alloc_n_map_sgl_alloc_fail, "Failed to allocate SGL");
		rv = -ENOMEM;

		goto free_pgs;
	}
	if (!(mem->map_sg_nents = nvmeib_public_ib_dma_map_sg(mem->pd->device,
		mem->mem_table.sgl, mem->mem_table.nents, mem->dma_dir))) {
		_NE(err_nvmeib_mem_alloc_n_map_sgl_map_fail, "Failed to DMA map memory");
		rv = -ENOMEM;
		goto free_sg;
	}

dev_call:
	/* Call the inner alloc_n_map which creates the mr from the sgl */
	rv = DEVICE_CALL(mem->pd->device, alloc_n_map, mem);
	if (rv) {
		if (mem->use_dma_pages)
			goto out;
		goto free_sg;
	}

	mem->allocated = allocate;
	goto out;

free_sg:
	sg_free_table(&mem->mem_table);

free_pgs:
	if (allocate) {
		nvmeib_free_pages_respecting_numa_policy(mem);
	}

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_mem_alloc_n_map);

static struct scatterlist *sg_get_sg_section(struct scatterlist *sgl, int orig_nents, off_t offset, size_t len, int *chunk_nents)
{
	struct scatterlist *sg, *sg_sect_start = NULL;
	int i, rem_nents = 0;
	for_each_sg(sgl, sg, orig_nents, i) {
		if (sg->length > offset) {
			sg_sect_start = sg;
			rem_nents = orig_nents - i;
			break;
		}
		offset -= sg->length;
	}
	BUG_ON(!sg_sect_start);
	BUG_ON(!rem_nents);
	*chunk_nents = 0;
	for_each_sg(sg_sect_start, sg, rem_nents, i) {
		if (sg->length >= offset + len) {
			*chunk_nents = i + 1;
			break;
		}
		len -= (sg->length - offset);
		offset = 0;
	}
	BUG_ON(!*chunk_nents);
	return sg_sect_start;
}

void nvmeib_mem_sync_map_for_device(struct nvmeib_alloc_n_map *mem, off_t offset, size_t len)
{
	if (len == NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN)
		len = (mem->n_pages << PAGE_SHIFT) - offset;
	BUG_ON(!len);
	BUG_ON(offset + len > (mem->n_pages << PAGE_SHIFT));

	if (!mem->use_dma_pages) {
		if (offset == 0 && (mem->mem_table.nents == 1 || len == mem->n_pages << PAGE_SHIFT)) {
			/* Shortcut - Can just sync the whole SGL */
			nvmeib_public_ib_dma_sync_sg_for_device(
				mem->pd->device,
				mem->mem_table.sgl,
				mem->mem_table.nents,
				mem->dma_dir);
		} else {
			/* Using the offset and length, get a section of the SGL */
			int sync_nents;
			struct scatterlist *sg_sync_start = sg_get_sg_section(
							mem->mem_table.sgl,
							mem->mem_table.nents,
							offset,
							len,
							&sync_nents);
			/* Sync the SGL section */
			nvmeib_public_ib_dma_sync_sg_for_device(
				mem->pd->device,
				sg_sync_start,
				sync_nents,
				mem->dma_dir);
		}
	} else {
		int i;
		/* SGL not used, must sync page by page */
		for (i = 0; i < mem->n_pages; i++) {
			if ((i << PAGE_SHIFT) > offset + len)
				break;
			if (((i + 1) << PAGE_SHIFT) < offset)
				continue;
			nvmeib_public_ib_dma_sync_single_for_device(
				mem->pd->device,
				mem->dma_pages[i],
				PAGE_SIZE,
				mem->dma_dir);
		}
	}
}
EXPORT_SYMBOL(nvmeib_mem_sync_map_for_device);

void nvmeib_mem_sync_map_for_cpu(struct nvmeib_alloc_n_map *mem, off_t offset, size_t len)
{
	if (len == NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN)
		len = (mem->n_pages << PAGE_SHIFT) - offset;
	BUG_ON(!len);
	BUG_ON(offset + len > (mem->n_pages << PAGE_SHIFT));

	if (!mem->use_dma_pages) {
		if (offset == 0 && (mem->mem_table.nents == 1 || len == mem->n_pages << PAGE_SHIFT)) {
			/* Shortcut - Can just sync the whole SGL */
			nvmeib_public_ib_dma_sync_sg_for_cpu(
				mem->pd->device,
				mem->mem_table.sgl,
				mem->mem_table.nents,
				mem->dma_dir);
		} else {
			/* Using the offset and length, get a section of the SGL */
			int sync_nents;
			struct scatterlist *sg_sync_start = sg_get_sg_section(
							mem->mem_table.sgl,
							mem->mem_table.nents,
							offset,
							len,
							&sync_nents);
			/* Sync the SGL section */
			nvmeib_public_ib_dma_sync_sg_for_cpu(
				mem->pd->device,
				sg_sync_start,
				sync_nents,
				mem->dma_dir);
		}
	} else {
		int i;
		/* SGL not used, must sync page by page */
		for (i = 0; i < mem->n_pages; i++) {
			if ((i << PAGE_SHIFT) > offset + len)
				break;
			if (((i + 1) << PAGE_SHIFT) < offset)
				continue;
			nvmeib_public_ib_dma_sync_single_for_cpu(
				mem->pd->device,
				mem->dma_pages[i],
				PAGE_SIZE,
				mem->dma_dir);
		}
	}
}
EXPORT_SYMBOL(nvmeib_mem_sync_map_for_cpu);

/* This function is uncalled */
static int __attribute__((unused)) _nvmeib_map_mr(
	struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages) {
	int rv = -1;

	NFIN;
	rv = DEVICE_CALL(ibdev, map_mr, ibdev, mr, pages, n_pages);
	NFOUT;
	return rv;
}

int nvmeib_peek_cq(struct ib_cq *cq, int max) {
	struct ib_device *ibdev = cq->device;
	int rv = -1;

	NFIN;
	rv = DEVICE_CALL(ibdev, peek_cq, cq, max);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_peek_cq);

int nvmeib_query_device(struct ib_device *device,
							   struct ib_device_attr *device_attr) {
	int rv = 0;

	NFIN;
	rv = DEVICE_CALL(device,
					 query_device,
					 device, device_attr);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_query_device);

int nvmeib_post_send_atomic(struct ib_qp *qp,
								   struct nvmeib_send_wr *send_wr,
								   struct nvmeib_send_wr **bad_send_wr) {
	struct ib_device *ibdev = qp->device;
	int rv = 0;

	NFIN;
	rv = DEVICE_CALL(ibdev,
					 post_send_atomic,
					 qp, send_wr, bad_send_wr);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_post_send_atomic);

static inline void* do_vmap(int n_pages, void **virt) {
	struct page **pages;
	void *vaddr = NULL;
	int i;
	NFIN;

	if (!(pages = kmalloc(n_pages * sizeof(*pages), GFP_KERNEL))) {
		_NE(error_nvmeib_public_do_vmap, "Cannot allocate pages array");
		goto out;
	}

	for (i = 0; i < n_pages; i++) pages[i] = virt_to_page(virt[i]);
	vaddr = vmap(pages, n_pages, VM_MAP, PAGE_KERNEL);
	kfree(pages);

out:
	NFOUT;
	return vaddr;
}

/* allocate pages, alloc MR and map to contiguous kernel virtual space (vmap) */
void* nvmeib_mem_alloc_n_vmap(struct nvmeib_alloc_n_map *mem) {
	void *vaddr = NULL;
	NFIN;

	/* checks */
	if (!mem->pd || mem->mem_table.sgl || mem->pages || !mem->n_pages || mem->mr) {
		_NE(error_nvmeib_public_nvmeib_mem_alloc_n_vmap, "Invalid input: @PD SGL:@PTR MR:@MR_PTR N_PAGES:@INT PAGES:@PAGES",
		    mem->pd, mem->mem_table.sgl, mem->mr, mem->n_pages, mem->pages);
		goto out;
	}

	/* alloc_n_map */
	if (nvmeib_mem_alloc_n_map(mem) < 0)
		goto out;

	/* map to contiguous kernel virtual space */
	if (!(vaddr = vmap(mem->pages, mem->n_pages, VM_MAP, PAGE_KERNEL))) {
		_NE(error_4_nvmeib_public_nvmeib_mem_alloc_n_vmap, "Fail to vmap");
		goto err_unmap;
	}

	_NT(trace_nvmeib_public_nvmeib_mem_alloc_n_vmap,
	    "Allocated @N_PAGES pages, vaddr=@VADDR", mem->n_pages, vaddr);
	goto out;


err_unmap:
	nvmeib_mem_unmapn_n_free(mem);

out:
	NFOUT;
	return vaddr;
}
EXPORT_SYMBOL(nvmeib_mem_alloc_n_vmap);

void* nvmeib_vmap(void **virts, int n) {
	return do_vmap(n, virts);
}
EXPORT_SYMBOL(nvmeib_vmap);

/* -------------------------------------------------------------------------- */
/* QP statistics                                                              */
/* -------------------------------------------------------------------------- */
struct nvmeib_qp_stats_pcpu * nvmeib_qp_stats_alloc(void)
{
	struct nvmeib_qp_stats_pcpu __percpu *s;
	NFIN;

	if (!(s = nvmeib_public_alloc_percpu_zeroed(struct nvmeib_qp_stats_pcpu)))
		_NE(e_nvmeib_qp_stats_alloc, "Failed to alloc");

	NFOUT;
	return s;
}
EXPORT_SYMBOL(nvmeib_qp_stats_alloc);

void nvmeib_qp_stats_free(struct nvmeib_qp_stats_pcpu *s)
{
	NFIN;

	nvmeib_public_free_percpu(s);

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_qp_stats_free);

struct nvmeib_pool_percpu_counts __percpu * nvmeib_alloc_percpu_pool_counts(void)
{
	return nvmeib_public_alloc_percpu_zeroed_cacheline(struct nvmeib_pool_percpu_counts);
}
EXPORT_SYMBOL(nvmeib_alloc_percpu_pool_counts);

#if defined (NVMEIB_QP_STATS) && (NVMEIB_QP_STATS==1)

#define BUF_ADD(...)	count += scnprintf(buf+count, len-count, __VA_ARGS__)

ssize_t nvmeib_qp_stats_fill(struct nvmeib_qp_stats_pcpu *s, char *buf, size_t len)
{
#define BUF_ADD_STAT(__s, __stat_member)																	\
	do {																										\
		struct nvmeib_qp_stats_pcpu __percpu *__p;																\
		int __cpu;																								\
		BUF_ADD("%-*s: ",  QPS_STATS_PAD_BLANKS_LEN_LINE_NUM + QPS_STATS_PAD_BLANKS_LEN_NAME, #__stat_member);	\
		for_each_online_cpu(__cpu) {																			\
			__p = per_cpu_ptr(__s, __cpu);																		\
			BUF_ADD("%*llu|", QPS_STATS_PAD_BLANKS_LEN_CNT, __p->__stat_member);								\
		}																										\
		BUF_ADD("\n");																							\
	} while (0)

#define BUF_ADD_CTX_STATS(__ctx)				\
	do {											\
		BUF_ADD_STAT(s, __ctx.n_executions);		\
		BUF_ADD_STAT(s, __ctx.n_post_sends);		\
		BUF_ADD_STAT(s, __ctx.n_poll_cq);			\
		BUF_ADD_STAT(s, __ctx.n_poll_cq_send_cqes);	\
		BUF_ADD_STAT(s, __ctx.n_poll_cq_recv_cqes);	\
		BUF_ADD_STAT(s, __ctx.n_poll_cq_mixed_cqes);\
		BUF_ADD_STAT(s, __ctx.n_poll_cq_empty);		\
		BUF_ADD_STAT(s, __ctx.n_offth_sched);		\
		/* BUF_ADD_STAT(s, __ctx.n_rearm_irqs); */ 	\
	} while (0)

	int i;
	char cpu_i[8 + 1];
	ssize_t count = 0;

	/* concat header (cpuX columns) after caller's QP-name */
	for_each_online_cpu(i) {
		snprintf(cpu_i, QPS_STATS_PAD_BLANKS_LEN_CNT, "CPU%d", i);
		BUF_ADD("%*s|", QPS_STATS_PAD_BLANKS_LEN_CNT, cpu_i);
	}
	BUF_ADD("\n");

	BUF_ADD_CTX_STATS(hi);
	BUF_ADD_CTX_STATS(si);
	BUF_ADD_CTX_STATS(sy);

	return count;

#undef BUF_ADD_CTX_STATS
#undef BUF_ADD_STAT
}
EXPORT_SYMBOL(nvmeib_qp_stats_fill);

#undef BUF_ADD
#endif

//#include "nvmeib_shared_ec.inc.c"
module_init(nvmeib_module_init);
module_exit(nvmeib_module_exit);
