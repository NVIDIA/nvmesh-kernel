/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include <kr_incs.h>
#include <linux/semaphore.h>
#include <linux/jiffies.h>

#include "nvmeib.h"
#include "nvmeib_public.h"
#include "nvmeib_types.h"
#include "nvmeib_event.h"
#include "nvmeibc_block.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_defs.h"
#include "core/nvmeibc_core_common.h"
#include "nvmeibc_main.h"
#include "nvmeibc_ib_nordda_channel.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeibc_nvme.h"
#include "nvmeibc_ib_net.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_locks_channel.h"
#include "nvmeibc_disk_locks.h"
#include "nvmeibs_nvme.h"
#include "nvmeib_nvme.h"
#include "nvmeib_public_procfs.h"
#include "nvmeibc_toma.h"
#include "nvmeibc_pausable.h"
#include "nvmeibc_targets.h"
#include "nvmeibc_jam.h"
#include "nvmeibc_disk_gen_cmds.h"
#include "nvmeibc_trace.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "nvmeibc_nr_lat_meas.h"
#include "nvmeibc_disk_local_dma_pools.h"

// EC-2944 - this file should NOT include and parse disk io cmd to get to block command
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_stages.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.h"

#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"

#include "block/datapath_ec/nvmeibc_block_dp_ec_gf.h"

//#define CONFIG_NVMEIB_DEBUG 1
//#define DEBUG 1
#include "nvmeib_utils.h"
#include "nvmeib_srq.h"
#include "kr_undef.h"
#include "common/proc_epilog.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeib_jdr.h"
#include "nvmeib_metrics.h"
#include "nvmeib_metrics_jdr.h"
#include "nvmeib_completion_noise.h"
#include "nvmeib_cpu_masks.h"
#include "nvmeib_pcpu_wq.h"

#ifdef FINS
#	undef FINS
#	undef NFINS
#endif
#ifdef FOUTS
#	undef FOUTS
#	undef NFOUTS
#endif
#ifdef IFINS
#	undef IFINS
#endif
#ifdef IFOUTS
#	undef IFOUTS
#endif

#ifndef LLVM
#	define NFINS(x) _ND(__AUTOID__, "--> @FINOUT_PARAM @ @FINOUT_PARAM\n", x ? x->name : "", x ? x->disk_host : "")
#	define NFOUTS(x) _ND(__AUTOID__, "<-- @FINOUT_PARAM @ @FINOUT_PARAM\n", x ? x->name : "", x ? x->disk_host : "")
#	define NDFINN(x) _ND(__AUTOID__, "--> @FINOUT_PARAM\n", x)
#	define NDFOUTN(x) _ND(__AUTOID__, "<-- @FINOUT_PARAM\n", x)
#else
#	define NFINS(x) do {} while (0)
#	define NFOUTS(x) do {} while (0)
#	define NDFINN(x) do {} while (0)
#	define NDFOUTN(x) do {} while (0)
#endif

#define __NFIND NFINS(disk)
#define __NFOUTD NFOUTS(disk)
#define __NFINI do { if (info) NFINS(info->disk); } while (0)
#define __NFOUTI do { if (info) NFOUTS(info->disk); } while (0)
#define __NFINR do { if (r && r->info) NFINS(r->info->disk); } while (0)
#define __NFOUTR do { if (r && r->info) NFOUTS(r->info->disk); } while (0)

#define USE_NORDDA_FOR_IO nvmeibc_use_norrda_for_io
#define USE_ONLY_NORDDA_FOR_IO nvmeibc_use_only_norrda_for_io
#define ALLOW_REDISCOVERY 1

#define RESTART_IO_TIMEOUT_SEC 	(30)
uint nvmeibc_restart_io_timeout_secs = RESTART_IO_TIMEOUT_SEC;
module_param_named(restart_io_timeout_secs, nvmeibc_restart_io_timeout_secs, uint, 0644);
MODULE_PARM_DESC(restart_io_timeout_secs, "Time in seconds to wait between full cycles of IO channels reconnection. Upon a disconnection, reconnection attempts will be more often and exponentially backoff as needed up to this value.");

#define CHECK_PATH_KA_INTERVAL 	(2 * NVMEIB_WAIT_FOR_CM_REP_TIMEOUT)

#define MAX_LIONIC_START_IOCH_PATH_FAIL_CYCLE	(1)
#define	MAX_LIONIC_START_IOCH_ATTEMPTS_CYCLE 	(64)
#define MAX_PENDING_START_IOCH_WQ 				(1)

int nvmeibc_max_ioch_path_fails = MAX_LIONIC_START_IOCH_PATH_FAIL_CYCLE;
module_param_named(max_ioch_path_fail, nvmeibc_max_ioch_path_fails, int, 0644);
MODULE_PARM_DESC(max_ioch_path_fail, "Number of failures allowed per path in a connection cycle.");

u64 max_ioch_start_route = MAX_LIONIC_START_IOCH_ATTEMPTS_CYCLE;

#define NVMEIB_MAX_NR_CHANNELS_PER_DISK	(64)
#define NVMEIB_MIN_NR_CHANNELS_PER_DISK	(1)

#define NVMEIBC_WAIT_FOR_IO_FIRST_CHAN_WARN (15 * HZ)

#define DD_STG(_disk, _stage, _state) _NT(NVMEIB_CONCAT2(__FILE_LITERAL__, NVMEIB_CONCAT2(_##_state, __LINE__)), "DISCOVER_ID=@DISCOVER_ID	DISCOVER-STAGES:"#_state"	"#_stage", Disk=@DISK_NAME,@DISK Took=@JIFFIES", _disk->discover_id, _disk->name, _disk, xstage_ts)
#define DD_STG_INIT_JIFFIES u64 xstage_ts = 0
/* The following should be after the last declaration in every function it is used */
#define DD_STG_START_WITH_DECLARE(_disk, _stage) u64 xstage_ts = 0;DD_STG_START(_disk, _stage)

#define DD_STG_START(_disk, _stage) 	\
	do {	\
		xstage_ts = jiffies;	\
		_NT(__AUTOID__, "DISCOVER_ID=@DISCOVER_ID	DISCOVER-STAGES:START	"#_stage", Disk=@DISK_NAME,@DISK", _disk->discover_id, _disk->name, _disk);	\
	} while (0)

#define DD_STG_FAIL_WITH_DISK_STATUS_IF_NEEDED(_disk, _stage, _status_enum) ({	\
	if ((_status_enum) >= 0){	\
		DISK_DISCOVER_STATUS(_disk, _status_enum);	\
	}	\
})

#define DD_STG_END(_disk, _stage, _rv, _status_enum)	\
	do{	\
		xstage_ts = jiffies - xstage_ts; \
		if ((_rv) >= 0) {	\
			DD_STG(_disk, _stage, SUCCESS);	\
		} else {	\
			DD_STG(_disk, _stage, FAIL);	\
			DD_STG_FAIL_WITH_DISK_STATUS_IF_NEEDED(_disk, _stage, _status_enum);	\
		}	\
	} while (0)

extern bool nr_defer_recv_comps;

unsigned nr_max_channels_per_disk = NVMEIB_MAX_NR_CHANNELS_PER_DISK;
module_param(nr_max_channels_per_disk, uint, 0644);
MODULE_PARM_DESC(nr_max_channels_per_disk, "Maximum number of RDMA IO channels per disk.");

bool nvmeibc_use_local_bypass = true;
module_param_named(use_local_bypass, nvmeibc_use_local_bypass, bool, 0644);
MODULE_PARM_DESC(use_local_bypass, "Access local drives directly and not via a NIC, i.e. over the network. Setting to false is mainly used for debugging local disk access.");

bool nvmeibc_use_norrda_for_io = true;
module_param_named(use_norrda_for_io, nvmeibc_use_norrda_for_io, bool, 0644);
MODULE_PARM_DESC(use_norrda_for_io, "Allow using non-RDDA operations for IO. As RDDA is deprecated, this should always be true.");

uint nvmeibc_skip_disk_iocmds_flags = 0;
module_param_named(skip_disk_iocmds_flags, nvmeibc_skip_disk_iocmds_flags, uint, 0644);
MODULE_PARM_DESC(skip_disk_iocmds_flags, "This is an unsafe debug mode. Skip disk access (remote and local): "
									"0 = Disabled, "
									"1 = Skip read operations, "
									"2 = Skip write operations, "
									"3 = Skip read & write operations, "
									"4 = Skip journal write operations or any combination using this operation, "
									"5 = Skip all IO operations including those not mentioned above. Used for performance tuning and debugging.");

bool nvmeibc_use_only_norrda_for_io = false;
module_param_named(use_only_norrda_for_io, nvmeibc_use_only_norrda_for_io, bool, 0644);
MODULE_PARM_DESC(use_only_norrda_for_io, "Use only non-RDDA operations for IO. As RDDA is deprecated, this is meaningless.");

unsigned nvmeibc_nr_pcpu_channels_per_disk = 0;
module_param_named(nr_pcpu_channels_per_disk, nvmeibc_nr_pcpu_channels_per_disk, uint , 0644);
MODULE_PARM_DESC(nr_pcpu_channels_per_disk,
				 "Connect per-cpu RDMA IO channels (up to 128) in addition to the nr_max_channels_per_disk any-cpu channels. "
				 "The total number of channels between a client and a target's disk is limited by the lower of nr_max_channels_per_path on the Client and the Target. "
				 "Typically set to true for kernel-based DPU usage.");

bool nvmeibc_nr_pcpu_ch_lockless = false;
module_param_named(nr_pcpu_ch_lockless, nvmeibc_nr_pcpu_ch_lockless, bool, 0644);
MODULE_PARM_DESC(nr_pcpu_ch_lockless, "Per-cpu RDMA IO channels are lockless. This reduces contention and increases performance, but requires a lot more channels typically.");

static char *nvmeibc_nr_pcpu_ch_ll_cpus = NULL;
module_param_named(nr_pcpu_ch_ll_cpus, nvmeibc_nr_pcpu_ch_ll_cpus, charp, 0644);
MODULE_PARM_DESC(nr_pcpu_ch_ll_cpus, "A list of CPU cores for lockless per-cpu IO RDMA channels. The format is as a hex-mask list of cores, where each entry is 32-bits.");

bool nvmeibc_ioch_ka_only_no_rdda = NVMEIB_IOCH_KA_ONLY_NO_RDDA;
module_param_named(ioch_ka_only_no_rdda, nvmeibc_ioch_ka_only_no_rdda, bool, 0644);
MODULE_PARM_DESC(ioch_ka_only_no_rdda, "Use only IO channels for IO keep alive messages, requires disk discovery to take effect.");

bool nvmeibc_disk_prio_pending = true;
module_param_named(disk_prio_pending, nvmeibc_disk_prio_pending, bool, 0644);
MODULE_PARM_DESC(disk_prio_pending, "Defines whether to prioritize IO in the pending IO queue, which hold both locks and journal entries. This was added to improve the performance of EC rebuilds.");

/*
This mainly used for ARM when we have using kernel EC calculation which need to be in enable interrupts ctx
Consider enable it only on ARM
*/
bool nvmeibc_disk_nrch_defer_block_cb = false;
module_param_named(disk_nrch_defer_block_cb, nvmeibc_disk_nrch_defer_block_cb, bool, 0644);
MODULE_PARM_DESC(disk_nrch_defer_block_cb, "Defines whether to defer the IO callbacks until after reenabling interrupts.");

bool nvmeibc_disk_local_defer_block_cb = false;
module_param_named(disk_local_defer_block_cb, nvmeibc_disk_local_defer_block_cb, bool, 0644);
MODULE_PARM_DESC(disk_local_defer_block_cb, "Determines whether to defer local IO block cb to non-interrupt context.");

bool nvmeibc_disk_pcpu_nrch_poll_proc = false;
module_param_named(disk_pcpu_nrch_poll_proc, nvmeibc_disk_pcpu_nrch_poll_proc, bool, 0444);
MODULE_PARM_DESC(disk_pcpu_nrch_poll_proc, "Allow polling of per-cpu IO channels through a proc file, which is useful for running kernel IO from SPDK. Useful for initial SNAP versions that did some IO from the kernel.");

bool nvmeibc_disk_pause_at_first_discover = true;
module_param_named(disk_pause_at_first_discover, nvmeibc_disk_pause_at_first_discover, bool, 0644);
MODULE_PARM_DESC(disk_pause_at_first_discover, "Defines whether to set the disk as paused for the first discovery to make attach operations faster. False simulates pre-2.6 behavior.");

bool nvmeibc_disk_use_async_subscribe = true;
module_param_named(use_async_subscribe, nvmeibc_disk_use_async_subscribe, bool, 0644);
MODULE_PARM_DESC(use_async_subscribe, "Determines whether to run \"subscribe\" operations asynchronously. Subscribe operations are used for clients to subscribe to TOMA for instructions regarding a volume's disk segment.");


bool nvmeibc_disk_local_io_use_prpl = true;
module_param_named(local_io_use_prpl, nvmeibc_disk_local_io_use_prpl, bool, 0644);
MODULE_PARM_DESC(local_io_use_prpl, "Determines whether local IO requests use PRPLs instead of SGLs (see the NVMe standard for more information). PRPLs are needed for environments with the IOMMU enabled.");

bool nvmeibc_disk_local_io_use_rd_md_pool = true;
module_param_named(local_io_use_rd_md_pool, nvmeibc_disk_local_io_use_rd_md_pool, bool, 0644);
MODULE_PARM_DESC(local_io_use_rd_md_pool, "Determines whether to use a preallocated pool or to dynamically allocate memory for metadata read operations, as the NVMe standard forces reading the block data with the metadata.");

bool nvmeibc_disk_local_io_use_md_dma_pool = true;
module_param_named(local_io_use_md_dma_pool, nvmeibc_disk_local_io_use_md_dma_pool, bool, 0644);
MODULE_PARM_DESC(local_io_use_md_dma_pool, "When a local IO request is made without providing space for the metadata buffer and the drive has metadata enabled, then this determines whether to use a preallocated pool of memory or to dynamically allocate memory per IO.");

bool nvmeibc_disk_local_write_use_data_copy = false;

#if !NVMESH_IS_PRODUCTION_COMPILATION
module_param_named(local_write_use_data_copy, nvmeibc_disk_local_write_use_data_copy, bool, 0644);
MODULE_PARM_DESC(local_write_use_data_copy, "Copy write data to a DMA pool buffer before submitting to the drive. Prevents CRC mismatches when application modifies buffers during DMA. Automatically enabled for fake_4kpi drives.");
#endif

/* [NVMESH-3287]: Params for throttling target-nics query to management */
uint nvmeibc_disk_tgt_nics_query_min_secs = 2;
module_param_named(tgt_nics_query_min_secs, nvmeibc_disk_tgt_nics_query_min_secs, uint, 0644);
MODULE_PARM_DESC(tgt_nics_query_min_secs, "The minimum amount of time allowed between Target NICs query to management.");

uint nvmeibc_disk_tgt_nics_query_min_n_fail = UINT_MAX;
module_param_named(tgt_nics_query_min_n_fail, nvmeibc_disk_tgt_nics_query_min_n_fail, uint, 0644);
MODULE_PARM_DESC(tgt_nics_query_min_n_fail, "The minimum number of discovery failures before sending a Target NICs query to management.");

uint nvmeibc_disk_tgt_nics_query_min_fail_secs = 10;
module_param_named(tgt_nics_query_min_fail_secs, nvmeibc_disk_tgt_nics_query_min_fail_secs, uint, 0644);
MODULE_PARM_DESC(tgt_nics_query_min_fail_secs, "Minimum number of seconds of discovery failures before sending a Target NICs query to management");

uint nvmeibc_disk_min_rediscover_timeout_ms = 1000;
module_param_named(min_rediscover_timeout_ms, nvmeibc_disk_min_rediscover_timeout_ms, uint, 0644);
MODULE_PARM_DESC(min_rediscover_timeout_ms, "Minimum time between rediscovery attempts in milliseconds");

u64 nvmeibc_disk_pause_timeout = NVMEIB_WAIT_BLOCK_DEV_PAUSE / HZ;

unsigned int nvmeibc_disk_prefix_priority_masks[16]; // Define an array of unsigned integers
int nvmeibc_disk_prefix_priority_masks_len = 0;          // Variable to hold the size of the array

module_param_array_named(prefix_priority_masks, nvmeibc_disk_prefix_priority_masks, uint, &nvmeibc_disk_prefix_priority_masks_len, 0444); // Register as module parameter
MODULE_PARM_DESC(prefix_priority_masks, "A comma separated list of prefixes length for paths with priority.");

bool nvmeibc_disk_prefix_priority_masks_rediscover_on_new = false;
module_param_named(prefix_priority_masks_rediscover_on_new, nvmeibc_disk_prefix_priority_masks_rediscover_on_new, bool, 0644);
MODULE_PARM_DESC(prefix_priority_masks_rediscover_on_new, "Determines whether to rediscover when a new common prefix priority is found.");

NVMEIBC_MEMMGR_METRIC(dirty_bits_mem, "component=client.disk.dirty_bits_mem");


bool nvmeibc_disk_coremask_support = 0;
module_param_named(disk_coremask_support, nvmeibc_disk_coremask_support, bool, 0644);
MODULE_PARM_DESC(disk_coremask_support, "Set to true to enable disk coremask support. Creates \"disk_max_coremask_nrch\" NRCHs per coremask added via block CLI. Updated on rediscover");

uint nvmeibc_disk_max_coremask_nrch = 2;
module_param_named(disk_max_coremask_nrch, nvmeibc_disk_max_coremask_nrch, uint, 0644);
MODULE_PARM_DESC(disk_max_coremask_nrch, "Determines the maximum number of channels per coremask. Updated on rediscover.");

bool nvmeibc_disk_use_coremask_pending = true;
module_param_named(disk_use_coremask_pending, nvmeibc_disk_use_coremask_pending, bool, 0644);
MODULE_PARM_DESC(disk_use_coremask_pending, "Determines whether to maintain a pending command list for the coremask. If false, any IOs that cannot be immediately processed by the coremask will be submitted to the any-core channels.");

bool nvmeibc_disk_local_use_system_pcpu_wq = false;
module_param_named(disk_local_use_system_pcpu_wq, nvmeibc_disk_local_use_system_pcpu_wq, bool, 0644);
MODULE_PARM_DESC(disk_local_use_system_pcpu_wq, "Determines whether local IO uses the system per-cpu workqueue for requests.");

struct nvmeib_cpu_mask_info_node {
	struct nvmeib_cpu_mask_info mask_info;
	struct radix_tree_node node;
	u64 update_count;
};

struct nvmeibc_disk_coremask_info {
	struct nvmeibc_disk *disk;
	struct list_head coremask_chs;
	rwlock_t coremask_chs_lock;
	struct nvmeibc_disk_coremask_chs *coremask_chs_per_core[NVMEIB_CPU_MASK_MAX_CPUS];
	u64 last_coremask_update_count;
	unsigned n_coremask;
	unsigned curr_max_coremask_nrch;

	/* Used for getting mask updates from all disk's volumes on main WQ */
	struct workqe_struct update_masks_main_work;
	/* Used for connecting/disconnecting channels on admin WQ */
	struct workqe_struct update_masks_admin_work;
	void *update_masks_scratch;
	size_t update_masks_scratch_sz;

	/* Radix-tree of all disk's volumes masks (by mask uid) */
	struct radix_tree_root masks_tree;
	/* Array of mask info per-cpu */
	struct nvmeib_cpu_mask_info_node *masks_info_per_core[NVMEIB_CPU_MASK_MAX_CPUS];
	/* Masks guard between Main WQ and Disk Admin WQ */
	struct mutex masks_guard;
	/* Used to check for deprecated masks in masks_tree */
	u64 masks_update_count;
	/* Used to decide whether to update disks coremask channels from masks tree (in disk_check_coremask_update) */
	u64 last_masks_update_count;

	/* per-cpu stats */
	struct nvmeibc_disk_coremask_pcpu_stats __percpu *pcpu_stats;
};

static void validate_slow_first_io_chan(struct nvmeibc_disk* disk);
typedef void (*qp_pcpu_cb)(struct nvmeib_qp_stats_pcpu __percpu *, struct nvmeibc_ib_net *, void *);
static void common_qps_run_cb(struct nvmeibc_disk *disk, qp_pcpu_cb qp_cb, void *data);
static void qps_fill_buf_safe(struct nvmeib_qp_stats_pcpu __percpu *qp_stats, struct nvmeibc_ib_net *net, void *arg);

static void local_io_req_free_prpl(struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *block_cmd);
static void local_defer_io_work_fn(struct workqe_struct *work);
static int disk_cmd_pend_prio(struct nvmeibc_disk *disk, struct nvmeibc_disk_command *disk_cmd);
static void disk_check_coremask_update(struct nvmeibc_disk *disk);
static ssize_t core_masks_fill_buf(void *priv, char *buf, size_t len);
static ssize_t coremask_stats_fill_buf(void *priv, char *buf, size_t len);
static ssize_t coremask_stats_reset(void *priv, char *buf, size_t len);
struct write_status_buf_data;
static void write_coremask_json_buf(struct write_status_buf_data *data);
static void write_coremask_stats_json_buf(struct write_status_buf_data *data);

struct disk_globals {
	atomic_t ranic_shuffle_n;
	atomic_t local_nic_shuffle_n;
	atomic64_t table_id;
	/* sm requests throttling */
	struct rw_semaphore sm_th_update_rwsem;
	volatile unsigned eff_sm_th;
	struct semaphore sm_th_cntsem;
};

void *nvmeibc_disk_create_globals(const struct nvmeibc_cinst_params_core *p)
{
	struct disk_globals *d = NULL;

	if (!(d = kzalloc(sizeof(*d), GFP_KERNEL))) {
		_NE(t_00_cdisk, "Failed to allocate disk globals\n");
		goto out;
	}
	atomic64_set(&d->table_id, 555);
	init_rwsem(&d->sm_th_update_rwsem);
	d->eff_sm_th = 1;
	sema_init(&d->sm_th_cntsem, 1);
out:
	return d;
}

#define __get_dg(obj) ((struct disk_globals *)nvmeibc_cinst_get_core_g(obj)->dg)

void nvmeibc_disk_delete_globals(const struct nvmeibc_cinst_params_core *p)
{
	struct { const struct nvmeibc_cinst_params_core *cips; } dummy = {.cips = p};
	struct disk_globals *d = __get_dg(&dummy);
	kfree(d);
}

/* disk work item */
struct disk_workq {
	struct workqe_struct work;
	struct nvmeibc_disk *disk;
	void *work_data;
};

static void cancel_queued_get_ec_db_reqs(struct nvmeibc_disk *disk);

struct disconnect_io_path_work {
	struct workqe_struct work;
	struct nvmeibc_disk *disk;
	struct nvmeibc_ib_admin_channel *ch;
	union ib_gid lgid, rgid;
	struct completion *comp;
	struct nvmeibc_disk_update_data disk_update_data;
};

static void disconnect_io_path_work_fn(struct workqe_struct *work);

static void nvmeibc_dma_unmap(struct nvmeibc_disk_channel_rsc *r);

static int nvmeibc_disk_toma_create(
	struct nvmeibc_disk *disk,bool is_rediscover);
static void nvmeibc_disk_toma_free(struct nvmeibc_disk *disk);

static void free_disk_lnic(struct nvmeibc_local_nic *ln);
static void remove_disk_lnic(struct nvmeibc_disk *disk, struct nvmeibc_dev *nic_dev);

static int call_for_each_lnic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_local_nic *, void *),
	void *args);

static int call_for_each_lport(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_ib_port *, void *),
	void *args, bool, bool, int *);

static int call_for_each_arnic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_admin_rnic *, void *, bool),
	void *args);

static int call_for_each_rionic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_io_rnic *, void *),
	void *args);

static int call_for_each_nr_rionic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_io_rnic *, void *, bool),
	void *args);

static int nvmeibc_disk_net_intrs_stats_fill(struct nvmeibc_disk *disk,
											char* buf, int len);
static void nvmeibc_disk_net_intrs_stats_reset(struct nvmeibc_disk *disk);

static int nvmeibc_disk_cmds_stats_fill(struct nvmeibc_disk *disk,
										char* buf, int len);
static void nvmeibc_disk_cmds_stats_reset(struct nvmeibc_disk *disk);

static int nvmeibc_disk_gen_cmds_stats_fill(struct nvmeibc_disk *disk,
										char* buf, int len);

static int nvmeibc_disk_contended_locks_stats_fill(struct nvmeibc_disk *disk,
										char* buf, int len);

static int nvmeibc_disk_counters_fill(struct nvmeibc_disk *disk,
									  char *buf, int len);
static void nvmeibc_disk_counters_reset(struct nvmeibc_disk* disk);

/* pcpu_nrch_*() */
static void pcpu_nrch_init_mode(struct nvmeibc_disk *disk);
static void pcpu_nrch_init_pool(struct nvmeibc_disk_info *info);
static int  pcpu_nrch_get_next_cpu(struct nvmeibc_disk *disk);
static void pcpu_nrch_add(struct nvmeibc_disk *disk,
						  struct nvmeibc_ib_nordda_channel *nrch);
static void pcpu_nrch_del(struct nvmeibc_disk *disk,
			struct nvmeibc_ib_nordda_channel *nrch,
			struct list_head *pend_list);
static int 	pcpu_nrch_get_channel(struct nvmeibc_disk *disk,
								  struct nvmeibc_disk_command *disk_cmd,
								  struct nvmeibc_channel **ch,
								  void **context);
static void pcpu_nrch_check_empty(struct nvmeibc_disk *disk);

/* Compare arnics/nrchs by priority, used for prio_list_* fns */
static int arnic_prio_cmp_fn(void *priv, struct list_head *a, struct list_head *b)
{
	struct nvmeibc_admin_rnic *arnic_a = container_of(a, struct nvmeibc_admin_rnic, link);
	struct nvmeibc_admin_rnic *arnic_b = container_of(b, struct nvmeibc_admin_rnic, link);
	if (arnic_a->priority.raw < arnic_b->priority.raw)
		return -1;
	if (arnic_a->priority.raw == arnic_b->priority.raw)
		return 0;
	return 1;
}

/* coremask channels */
struct nvmeibc_disk_coremask_chs {
	struct list_head link; /* link to list head in c_disk */
	struct nvmeibc_disk *disk; /* backpointer to c_disk */
	spinlock_t spinlock;
	struct list_head pending_cmds; /* list of struct disk_command */
	int n_pending;
	int max_pending;
	struct plist_head nrchs_plist; /* pcpu NRCHs in this mask; plist_node is nrch->available_link */
	u64 uid; /* copy of unique-id for core_mask from DB */
	struct nvmeib_cpu_mask cpu_coremask; /* copy of core_mask from DB */
	int n_cpus;
	struct nvmeib_cpu_mask nrchs_coremask; /* mask of NRCH CPUs in mask_nrchs */
	int n_nrchs;
	struct nvmeib_cpu_mask lchs_coremask; /* mask of Lock Ch CPUs */
	int n_lchs;
	int dying; /* dying flag */
	struct kref refcnt;
	
	/* Stats */
	u64 n_io_mask_chan;
	u64 n_io_mask_dying;
	u64 n_io_mask_uid_mismatch;
	u64 n_io_mask_no_nrch;
	u64 n_io_mask_nrch_busy;
	/* Stats for EC reuse */
	u64 n_reuse_io_mask_dying;
	u64 n_reuse_io_mask_uid_mismatch;
	u64 n_reuse_io_mask_chan;
};

/* disk_ioch_drained API */
struct nvmeibc_disk_ioch_drained_event_net_detached_info {
	struct nvmeibc_channel *ch;
	struct list_head *list;
};

struct nvmeibc_disk_ioch_drained_event_srv_approval_info {
	struct nvmeibc_channel *ch;
	u64 cs_gid;
};

enum nvmeibc_disk_ioch_drained_event {
	NVMEIBC_DISK_IOCH_DRAINED_EVENT_NET_DETACHED = 0xa,
	NVMEIBC_DISK_IOCH_DRAINED_EVENT_SRV_APPROVAL,
	NVMEIBC_DISK_IOCH_DRAINED_EVENT_TIMER,
};

static inline const char *nvmeibc_disk_ioch_drained_event_to_str(int e)
{
	switch (e) {
	case NVMEIBC_DISK_IOCH_DRAINED_EVENT_NET_DETACHED	: return "NET_DETACHED";
	case NVMEIBC_DISK_IOCH_DRAINED_EVENT_SRV_APPROVAL	: return "SRV_APPROVAL";
	case NVMEIBC_DISK_IOCH_DRAINED_EVENT_TIMER			: return "TIMER";
	default: return "???";
	}
}

static void disk_ioch_drained_event_handler(struct nvmeibc_disk *disk,
	enum nvmeibc_disk_ioch_drained_event event, void *info);
/* --- */

static struct nvmeibc_ib_admin_channel *get_alive_admin_ch(
	struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_admin_channel *ch = NULL;

	NFIN;

	list_for_each_entry(arnic, &disk->arnics, link) {
		/* local also uses admin channel to send TOMA messages */
		if (nvmeibc_disk_use_arnic_for_disk(arnic, disk,
			NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_MAIN_CH)) {
			ch = ac_to_iac(arnic->channel);
			break;
		}
	}

	if (!ch)
		_NT(trace_disk_get_alive_admin_ch, "Failed to find main arnic's admin ch");

	NFOUT;
	return ch;
}

int nvmeibc_disk_add_admin_work(struct nvmeibc_disk *disk, struct workqe_struct *work)
{
	struct nvmeibc_ib_admin_channel *ach;
	int rv;

	if (atomic_read(&disk->paused) || atomic_read(&disk->dying)) {
		_NE(err_nvmeibc_disk_schedule_admin_work, "disk @DISK_NAME - Disk Paused/Dying", disk->name);
		rv = -EBUSY;
		goto out;
	}

	if (!(ach = get_alive_admin_ch(disk))) {
		_NE(err_2_nvmeibc_disk_schedule_admin_work, "disk @DISK_NAME - No alive admin ch", disk->name);
		rv = -ENOENT;
		goto out;
	}

	rv = nvmeibc_admin_channel_add_work(&ach->base, work);
out:
	return rv;
}

static void free_disc_rsc(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_disk_channel_rsc *r)
{
	__NFINR;

	nvmeibc_dma_unmap(r);

	kfree(r->prp1_shadow);
	vfree(r->prpl_phys);
	kfree(r->sq_shadow);
	kfree(r->cq_shadow);
	__NFOUTR;
}

static void free_disc_rscs(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_disk_info *info)
{
	int i, j;

	__NFINI;
	for (j = 0; j < info->n_rscs_sets; ++j) {
		for (i = 0; i < info->n_rscs; ++i)
			free_disc_rsc(ch, &info->hcaa[j].channel_rscs[i]);
		kfree(info->hcaa[j].channel_rscs);
		kfree(info->hcaa[j].lsi);
	}
	kfree(info->rscs);
	__NFOUTI;
}

static void init_disk_rsc(int i, struct nvmeibc_disk_channel_rsc *r,
	struct nvmeibs_disk_description *d)
{
	__NFINR;
	r->id = be64_to_cpu(d->id);
	r->mem_raddr = be64_to_cpu(d->bounce_buffer_raddr);
	r->mem_size = be32_to_cpu(d->bounce_buffer_size);
	r->mem_lkey = be32_to_cpu(d->bounce_buffer_lkey);
	r->mem_rkey = be32_to_cpu(d->bounce_buffer_rkey);
	r->mem_n_pages = be32_to_cpu(d->bounce_buffer_n_pages);
	_ND(trace_disk_init_disk_rsc, "disk_mem(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, lkey=@LKEY, "
		"rkey=@RKEY, n_pages=@N_PAGES",
		i, r->id,
		r->mem_raddr, r->mem_size, r->mem_lkey, r->mem_rkey, r->mem_n_pages);

	r->md_raddr = be64_to_cpu(d->md_raddr);
	r->md_size = be32_to_cpu(d->md_size);
	r->md_lkey = be32_to_cpu(d->md_lkey);
	r->md_rkey = be32_to_cpu(d->md_rkey);
	_ND(trace_1_disk_init_disk_rsc, "disk_md_mem(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, lkey=@LKEY, "
		"rkey=@RKEY, n_pages=@N_PAGES",
		i, r->id,
		r->md_raddr, r->md_size, r->md_lkey, r->md_rkey, r->mem_n_pages);

	r->prp1_raddr = be64_to_cpu(d->prp1_raddr);
	r->prp1_size = be32_to_cpu(d->prp1_size);
	r->prp1_rkey = be32_to_cpu(d->prp1_rkey);
	_ND(trace_2_disk_init_disk_rsc, "dprp1(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, rkey=@RKEY",
		i, r->id, r->prp1_raddr, r->prp1_size, r->prp1_rkey);

	r->prpl_raddr = be64_to_cpu(d->prpl_raddr);
	r->prpl_size = be32_to_cpu(d->prpl_size);
	r->prpl_rkey = be32_to_cpu(d->prpl_rkey);
	r->prpl_n_pages = be32_to_cpu(d->prpl_n_pages);
	_ND(trace_3_disk_init_disk_rsc, "prpl(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, rkey=@RKEY, n_pages=@N_PAGES",
		i, r->id, r->prpl_raddr, r->prpl_size, r->prpl_rkey, r->prpl_n_pages);

	r->sq_raddr = be64_to_cpu(d->sq_raddr);
	r->sq_rkey = be32_to_cpu(d->sq_rkey);
	r->sq_n_entries = be32_to_cpu(d->sq_entries);
	_ND(trace_4_disk_init_disk_rsc, "sq(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, rkey=@RKEY, entries=@ENTRIES",
		i, r->id, r->sq_raddr, r->sq_size, r->sq_rkey, r->sq_n_entries);

	r->cq_addr = be64_to_cpu(d->cq_raddr);
	r->cq_lkey = be32_to_cpu(d->cq_lkey);
	r->cq_rkey = be32_to_cpu(d->cq_rkey);
	r->cq_n_entries = be32_to_cpu(d->cq_entries);
	_ND(trace_5_disk_init_disk_rsc, "cq(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, lkey=@LKEY, rkey=@RKEY, entries=@ENTRIES",
		i, r->id,
		r->cq_addr, r->cq_size, r->cq_lkey, r->cq_rkey, r->cq_n_entries);

	r->sq_db_raddr = be64_to_cpu(d->sq_db_raddr);
	r->sq_db_size = be32_to_cpu(d->sq_db_size);
	r->sq_db_rkey = be32_to_cpu(d->sq_db_rkey);
	_ND(trace_6_disk_init_disk_rsc, "sq-db(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, rkey=@RKEY",
		i, r->id, r->sq_db_raddr, r->sq_db_size, r->sq_db_rkey);

	r->cq_db_raddr = be64_to_cpu(d->cq_db_raddr);
	r->cq_db_size = be32_to_cpu(d->cq_db_size);
	r->cq_db_rkey = be32_to_cpu(d->cq_db_rkey);
	_ND(trace_7_disk_init_disk_rsc, "cq-db(@SEQ,@ID_LLONG): raddr=@RADDR, length=@LENGTH_INT, rkey=@RKEY",
		i, r->id, r->cq_db_raddr, r->cq_db_size, r->cq_db_rkey);

	r->msix_raddr = be64_to_cpu(d->msix_raddr);
	r->msix_rkey = be32_to_cpu(d->msix_rkey);
	_ND(trace_8_disk_init_disk_rsc, "msix(@SEQ,@ID_LLONG): raddr=@RADDR, rkey=@RKEY",
		i, r->id, r->msix_raddr, r->msix_rkey);
	r->bb_raddr_nvme[0] = be64_to_cpu(d->bb_raddr_nvme[0]);
	r->bb_raddr_nvme[1] = be64_to_cpu(d->bb_raddr_nvme[1]);
	r->bb_raddr_nvme[2] = be64_to_cpu(d->bb_raddr_nvme[2]);
	_ND(trace_9_disk_init_disk_rsc, "bb_raddr_nvme(@SEQ, @ID_LLONG): (@RADDR, @RADDR, @RADDR)",
		i, r->id,
		r->bb_raddr_nvme[0],
		r->bb_raddr_nvme[1],
		r->bb_raddr_nvme[2]);
	_ND(trace_190_disk_init_disk_rsc,
		"bb_raddr_nvme(@INT, @INT64): (@_X, @_X, @_X)",
		i, r->id,
		r->bb_raddr_nvme[0],
		r->bb_raddr_nvme[1],
		r->bb_raddr_nvme[2]);
	__NFOUTR;
}

static int alloc_disk_rsc(struct nvmeibc_ib_admin_channel *ch, int i,
	struct nvmeibc_disk_channel_rsc *r, struct lock_seg_info *lsi)
{
	int /*j, */rv = -ENOMEM;

	__NFINR;
	r->sq_size = r->sq_n_entries * sizeof(struct nvme_command);
	if (!(r->sq_shadow = kzalloc(r->sq_size, GFP_KERNEL))) {
		_NE(error_disk_alloc_disk_rsc, "rsc(@SEQ): Failed to allocate sq shadow", i);
		goto out;
	}
	r->cq_size = r->cq_n_entries * sizeof(struct nvme_completion);
	if (!(r->cq_shadow = kzalloc(r->cq_size, GFP_KERNEL))) {
		_NE(error_1_disk_alloc_disk_rsc, "rsc(@SEQ): Failed to allocate cq shadow", i);
		goto out;
	}
	rv = 0;
	/* the MD */
	if (r->md_size) {
		r->md_riu.raddr = r->md_raddr;
		r->md_riu.rkey = r->md_rkey;
		r->md_riu.sge = &r->md_sge;
		r->md_riu.sge_cnt = 1;
	}

	/* the remote prp1 local shadow */
	if (r->prp1_size) {
		r->prp1_riu.raddr = r->prp1_raddr;
		r->prp1_riu.rkey = r->prp1_rkey;
		r->prp1_riu.sge = &r->prp1_sge;
		r->prp1_riu.sge_cnt = 1;
		r->prp1_sge.length = r->prp1_size;
	}

	/* the remote prpl local shadow */
	if (r->prpl_n_pages > 1) {
		r->prpl_riu.raddr = r->prpl_raddr;
		r->prpl_riu.rkey = r->prpl_rkey;
		r->prpl_riu.sge = &r->prpl_sge;
		r->prpl_riu.sge_cnt = 1;
		r->prpl_sge.addr = r->prpl_shadow_dma;
		r->prpl_sge.lkey = r->prpl_shadow_l_key;
	}

	/* the remote submission_q local shadow */
	r->sq_riu.raddr = r->sq_raddr;
	r->sq_riu.rkey = r->sq_rkey;
	r->sq_riu.sge = &r->sq_sge;
	r->sq_riu.sge_cnt = 1;
	r->sq_sge.length = sizeof(struct nvme_rw_command);

	/* the remote completion queue that we copy */
	r->cq_riu.sge = &r->cq_sge;
	r->cq_riu.sge_cnt = 1;
	r->cq_sge.length = sizeof(struct nvme_completion);
	r->cq_sge.lkey = r->cq_lkey;

	/* the remote submission_q doorbell local shadow */
	r->sq_db_riu.raddr = r->sq_db_raddr;
	r->sq_db_riu.rkey = r->sq_db_rkey;
	r->sq_db_riu.sge = &r->sq_db_sge;
	r->sq_db_riu.sge_cnt = 1;
	r->sq_db_sge.length = r->sq_db_size;

	/* the remote completion_q doorbell local shadow */
	r->cq_db_riu.raddr = r->cq_db_raddr;
	r->cq_db_riu.rkey = r->cq_db_rkey;
	r->cq_db_riu.sge = &r->cq_db_sge;
	r->cq_db_riu.sge_cnt = 1;
	r->cq_db_sge.length = sizeof(r->cq_db_value);

	/* the remote disk submission queue msix */
	r->msix_riu.raddr = r->msix_raddr;
	r->msix_riu.rkey = r->msix_rkey;
	r->msix_riu.sge = &r->msix_sge;
	r->msix_riu.sge_cnt = 1;
	r->msix_sge.length = sizeof(r->msi_x_payload);

	/* the read lock buffer i.e. mapped r->read_lock_buffer */
	r->rl_sge.length = sizeof(u64);
	r->rl_riu.sge = &r->rl_sge;
	r->rl_riu.sge_cnt = 1;
	r->lsi = lsi;

out:
	__NFOUTR;
	return rv;
}

static inline int get_r_index(struct nvmeibc_disk_info *info,
	struct nvmeibc_disk_channel_rsc *r)
{
	return ((void *)r - (void *)info->hcaa[r->set_n].channel_rscs) / sizeof(*r);
}

#define get_disk_uptime() (jiffies - disk->create_jiff)

#define CORE_CLIENT_DISK_STATS_JSON_PROC_FRMT_VER 2 /* Bumped to 2 due to fix for [NVMESH-6726] */
static ssize_t stats_fill_buf_json(void *priv, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	const struct nvmeibc_disk *disk = priv;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	ssize_t count  = 0, indent = 0;
	NFIN;
	count += jops->start_obj(buf + count, len - count, NULL, indent++);
	count += jops->data_str(buf + count, len - count,
							"uuid", disk->name,
							!JSON_LAST_ELEM, indent);
	count += nvmeib_io_stats_to_json(disk->stats, buf+count, len-count, get_disk_uptime(), jops, indent,
										false);
	count += jops->data_uval(buf + count, len - count,
							 "overeager", (u64)atomic64_read(&disk->total_overeager),
							 JSON_LAST_ELEM, indent);
	count += nvmeib_proc_add_json_proc_epilog(CORE_CLIENT_DISK_STATS_JSON_PROC_FRMT_VER, buf+count, len-count);
	count += jops->end_obj(buf + count, len - count, JSON_LAST_ELEM, --indent);
	NFOUT;
	return count;
#undef BUF_ADD
}

static ssize_t stats_clear(void *priv, char *buf , size_t len) {
	struct nvmeibc_disk *disk = priv;
	int reset;

	if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
		return -EINVAL;
	}

	nvmeib_io_stats_clear(disk->stats , 'A');
	return len;
}

#define CORE_CLIENT_DISK_STATS_PROC_FRMT_VER 1
static ssize_t stats_fill_buf(void *priv, char *buf, size_t len)
{
#define BUF_ADD(...)	count += scnprintf(buf+count, len-count, __VA_ARGS__)
#define DOT1_FMT()	"%lld.%d"
#define DOT1_PARAMS(t)	(t)/10, (int)((t)%10)
	struct nvmeibc_disk *disk = priv;
	struct nvmeib_io_counters c = {0};
	ssize_t count = 0;
	const ulong cur_time = get_disk_uptime();
	u64 total_overeager = 0;
	unsigned i;
	/* Print in units of micro-seconds. Latency/factor is in units of 1/10^7 of
	   a second and we also do /10 when printing. so total of 1/10^6 sec */
	NFIN;
	// Todo: Make same format as 'iostats_sum_to_string()'
	BUF_ADD("uptime=%ld.%03ld\n", cur_time/HZ, 1000*(cur_time%HZ)/HZ); // Changed from time->uptime, remove if mgmt doesn't need it

	for (i = 0; i < N_IO_STAT_VERBS; i++) {
		if (!nvmeib_io_stats_counts_verb(disk->stats, i))
			continue;
		memset(&c, 0, sizeof(c));
		nvmeib_io_stats_readc(disk->stats, (const enum nvmeib_io_stat_verbs)i, 0, &c);
		BUF_ADD("%s_ops=%lld\n", verb_to_string(i, true), c.total_ops);
		BUF_ADD("%s_sub_block_ops=%lld\n", verb_to_string(i, true), c.total_sub_block);
		BUF_ADD("%s_size=%lld\n", verb_to_string(i, true), c.total_size);
		BUF_ADD("%s_latency=" DOT1_FMT() "\n", verb_to_string(i, true), DOT1_PARAMS(c.total_latency));
		BUF_ADD("%s_latency^2=" DOT1_FMT() "\n", verb_to_string(i, true), DOT1_PARAMS(c.total_latency_sqr));
		BUF_ADD("%s_worst_latency=" DOT1_FMT() "\n", verb_to_string(i, true), DOT1_PARAMS(c.worst_latency));
	}

	/* Get total overeager for disk */
	total_overeager = atomic64_read(&disk->total_overeager);
	BUF_ADD("total_overeager=%llu\n", total_overeager);

	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_STATS_PROC_FRMT_VER, buf+count, len-count);

	NFOUT;
	return count;
#undef BUF_ADD_DOT1
#undef BUF_ADD
}

struct chstats_fill_workq {
	struct workqe_struct work;
	struct nvmeibc_disk *disk;
	char *buf;
	size_t len;
	ssize_t count;
	struct completion *done;
};

#ifdef DEBUG_CLNT_NET_STATS
static void chstats_fill_buf_work(struct workqe_struct *work)
{
	struct chstats_fill_workq *w =
		container_of(work, struct chstats_fill_workq, work);
	struct nvmeibc_disk *disk = w->disk;
	char *buf = w->buf;
	size_t len = w->len;
	ssize_t count = w->count;
	struct nvmeibc_admin_rnic *arnic;
	bool found = false;
	struct nvmeibc_ib_admin_channel *ach;
	struct list_head *rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *lionics;
	struct nvmeibc_io_lnic *lionic;
	struct nvmeibc_ib_nordda_channel *nrch;
	unsigned long flags;
	int i;

	NFIN;
	/* check if disk became dying till we reached here ...*/
	if (atomic_read(&disk->dying) || atomic_read(&disk->shut_down_triggered)) {
		count += scnprintf(buf+count, len-count, "Disk is dying!\n");
		goto out;
	}
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (nvmeibc_disk_use_arnic_for_disk(arnic, disk,
				NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_MAIN_CH)) {
			found = true;
			break;
		}
	}
	if (found) {
		nvmeibc_ib_net_stats_columns(buf, len, &count);
		//Admin ch
		ach = ac_to_iac(arnic->channel);
		nvmeibc_ib_net_stats_fill_buf(&ach->net.base, 0, buf, len, &count);
		//No-RDDA channels
		rionics = &disk->nr_rionics;
		list_for_each_entry(rionic, rionics, disk_nrlink) {
			lionics = &rionic->nr_lionics;
			list_for_each_entry(lionic, lionics, rionic_nrlink) {
				for (i = 0; i < lionic->n_nr_qps; ++i) {
					nrch = lionic->nr_channels + i;
					nvmeibc_ib_net_stats_fill_buf(&nrch->net.base, i+1, buf, len, &count);
				}
			}
		}
		spin_unlock_irqrestore(&disk->spinlock, flags);
	}
	else {
		count += scnprintf(buf+count, len-count, "Admin channel not found!\n");
	}

out:
	w->count = count;
	complete(w->done);

	NFOUT;
}

static ssize_t chstats_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	DECLARE_COMPLETION_ONSTACK(done);
	struct chstats_fill_workq work;
	ssize_t count = 0;

	NFIN;
	if (atomic_read(&disk->dying) || atomic_read(&disk->shut_down_triggered)) {
		count += scnprintf(buf+count, len-count, "The disk is dying!\n");
		goto out;
	}

	if (!test_and_set_bit(0, &disk->chstats_rd)) {
		count += scnprintf(buf+count, len-count, "\nDisk %s channels stats:\n",
						   disk->name);
		WQ_INIT_WORK(&work.work, chstats_fill_buf_work);
		work.disk = disk;
		work.buf = buf;
		work.len = len;
		work.count = count;
		work.done = &done;
		if (nvmeibc_disk_add_work(disk, &work.work) < 0)
			goto out;
		wait_for_completion(&done);
		count = work.count;
		clear_bit(0, &disk->chstats_rd);
	}
	else
		count += scnprintf(buf+count, len-count, "Busy - read in progress\n");
	//count += nvmeib_add_proc_status_footer(buffer+count, len-count, "chstats");
out:
	NFOUT;
	return count;
}
#endif /* DEBUG_CLNT_NET_STATS */

#if defined(DISK_COUNT_REUSE) && DISK_COUNT_REUSE
static ssize_t reuse_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;

	NFIN;
	count += scnprintf(buf + count, len - count, "%d",
		atomic_read(&disk->n_reused));
	NFOUT;
	return count;
}
#endif

struct ppath_stats {
	int ppath_used_reqs;
	int ppath_reused_bb;
	unsigned long ppath_reused_bb_lru_jif;
};

struct unique_list_ent {
	void *ptr;
	struct list_head link;
};

enum write_status_type {
	WRITE_STATUS_TEXT = 0,
	WRITE_STATUS_JSON = 1,
	/* as currently procs are size limited we extract this 2 out */
	WRITE_STATUS_NRCH = 2,
	WRITE_STATUS_IOCH = 3,

	WRITE_STATUS_QPS  = 4,
	WRITE_STATUS_IOCH_JSON = 5,
	
	WRITE_STATUS_COREMASK_JSON = 6,
	WRITE_STATUS_COREMASK_STATS = 7,
};

struct write_status_buf_data {
	struct nvmeibc_disk *disk;
	char *buf;
	ssize_t *count;
	size_t len;
	struct list_head admin_ch_list;
	struct list_head lock_ch_list;
	struct list_head io_ch_list;
	enum write_status_type status_type;
	int ntabs;
	struct ppath_stats nr_ppath_stats_sum;
	int idx;
};

static int list_add_unique_ptr(struct list_head *list, void *ptr)
{
	struct unique_list_ent *ent;
	int rv = 0;

	/* Uniquely add admin channel to list to print later in status */
	list_for_each_entry( ent, list, link) {
		if ( ent->ptr == ptr) {
			/* Pointer already in list */
			goto out;
		}
	}

	/* Not found, so add to list */
	if (!( ent = kzalloc(sizeof(*ent ), GFP_ATOMIC))) {
		_NE(error_disk_list_add_unique_ptr, "Memory allocation error");
		rv = -ENOMEM;
		goto out;
	}
	ent->ptr = ptr;
	list_add_tail(&ent->link, list);

out:
	return rv;
}

#define BOOL_TO_STRING(bool_var) ((bool_var) ? "true" : "false")
#define LAYER_TO_STRING(layer) (((layer) == IB_LINK_LAYER_INFINIBAND ? "IB" : (layer == IB_LINK_LAYER_ETHERNET ? "RoCE/iWARP" : "Unknown")))
#define TRANSPORT_TO_STRING(transport) (((transport) == RDMA_TRANSPORT_IB ? "IB/RoCE" : (transport == RDMA_TRANSPORT_IWARP ? "iWARP" : "Unknown")))
#define PORT_TO_DEV_NAME(p) ((p) ? P2IB(p)->name : "(null)")
#define PORT_TO_PORT_NUM(p) ((p) ? p->port : 0)
#define PORT_TO_LINK_LAYER(p) ((p) ? LAYER_TO_STRING(p->layer) : "(null)")
#define PORT_TO_TRANSPORT_TYPE(p) ((p) ? TRANSPORT_TO_STRING(p->transport_type) : "(null)")

#if ENABLE_SIW
#include "../softiwarp/kernel/siw.h"
#endif

static void net_status_fill_buf(struct nvmeibc_ib_net *net, struct write_status_buf_data *data, const char *prefix)
{
	char _c = '?';
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	if (net) {
		enum nvmeibc_ib_net_state net_state = nvmeib_get_state_guard(&net->state);
		if (net->ioch) {
			switch (net->ioch->ct) {
				case ct_admin:
					_c = 'a';
					break;
				case ct_lock:
				case ct_lock_2nd:
					_c = 'l';
					break;
				case ct_rdda:
					_c = 'r';
					break;
				case ct_n_rdda:
					_c = 'n';
					break;
				default:
					_c = 'o';
					break;
			}
		}
		BUF_ADD("%sNet: (%px) %pI6 (QPn: %x) -> %pI6:%llu (QPn: %x) State: %s Device: %s Port: %d recv-intr-vec: %d send-intr-vec: %d\n", prefix, net, &net->path.sgid,
			(net->qp ? net->qp->qp_num : 0), &net->path.dgid,
			(net->cm_rdma_type == _rdma_ib ? net->service_id : net->service_port),
			net->remote_qpn,
			nvmeibc_ib_net_state_str(net_state), PORT_TO_DEV_NAME(net->port), PORT_TO_PORT_NUM(net->port), net->recv_intr_vec, net->send_intr_vec);
		if (net->qp) {
			BUF_ADD("%sQP: %d (%px) SCQ: %px RCQ: %px SRQ: %px cm_id: %p qp-timeout: %d\n", prefix, net->qp->qp_num, net->qp, net->send_cq,
				net->recv_cq, nvmeib_srq_info_ib_srq(net->srq_info), net->cm_id, net->wd_timeout_jif);
		}
#if ENABLE_SIW
		if (net->port && P2NV(net->port)->dev_type == DT_siw && net->qp) {
			struct siw_qp *siw_qp = container_of(net->qp, struct siw_qp, ofa_qp);
			struct socket *socket = siw_qp->attrs.llp_stream_handle;
			if (socket) {
				struct sock *sk = socket->sk;
				if (sk) {
					struct tcp_sock *tp = tcp_sk(sk);
				BUF_ADD("%s%pI4n:%u -> %pI4n:%u\n",
					prefix, &sk->sk_rcv_saddr, sk->sk_num, &sk->sk_daddr, ntohs(sk->sk_dport));
#if KS_HAS_SO_INCOMING_CPU
					BUF_ADD("%sSIW LLP\t socket: %px sk: %px incoming_cpu: %d napi_id: %d mss_cache: %d\n",
						prefix, socket, sk, READ_ONCE(sk->sk_incoming_cpu), READ_ONCE(sk->sk_napi_id),
						tp->mss_cache);
#else
					BUF_ADD("%sSIW LLP\t socket: %px sk: %px napi_id: %d mss_cache: %d\n",
						prefix, socket, sk, READ_ONCE(sk->sk_napi_id), tp->mss_cache);
#endif
				}
			}
		}
#endif
		BUF_ADD("%s%cSCQ Stats - n_tot: %llu n_intr: %llu n_poll: %llu n_wakeups_burst: %llu n_wakeups_cycles: %llu n_external: %llu n_ipi: %llu max_intr_duration: %llu n_spurious_intr: %llu n_missed_events: %llu, dev-cq={%p, cpu=%d}\n",
			prefix, _c, net->scq_stats.n_intr + net->scq_stats.n_poll, net->scq_stats.n_intr, net->scq_stats.n_poll, net->scq_stats.n_wakeups_burst, net->scq_stats.n_wakeups_cycles, net->scq_stats.n_external, net->scq_stats.n_ipi_func,
			net->scq_stats.max_intr_duration, net->scq_stats.n_spurious_intr, net->scq_stats.n_missed_events, net->dev_cq, net->dev_cq ? nvmeib_cq_get_cpu(net->dev_cq) : -1);
		BUF_ADD("%s%cRCQ Stats - n_tot: %llu n_intr: %llu n_poll: %llu n_wakeups_burst: %llu n_wakeups_cycles: %llu n_external: %llu n_ipi: %llu max_intr_duration: %llu n_spurious_intr: %llu n_missed_events: %llu, dev-cq={%px, cpu=%d}\n",
			prefix, _c, net->rcq_stats.n_intr + net->rcq_stats.n_poll, net->rcq_stats.n_intr, net->rcq_stats.n_poll, net->rcq_stats.n_wakeups_burst, net->rcq_stats.n_wakeups_cycles, net->rcq_stats.n_external, net->scq_stats.n_ipi_func,
			net->rcq_stats.max_intr_duration, net->rcq_stats.n_spurious_intr, net->rcq_stats.n_missed_events, net->dev_cq, net->dev_cq ? nvmeib_cq_get_cpu(net->dev_cq) : -1);
		if (net->ioch && (net->ioch->ct == ct_rdda || net->ioch->ct == ct_n_rdda))
			BUF_ADD("%sio_ka=%llu\n", prefix, (u64)atomic64_read(&net->io_ka.sent));
	} else
		BUF_ADD("%sNet NULL\n", prefix);
#undef BUF_ADD
}

static void net_status_json_fill_buf(struct nvmeibc_ib_net *net, struct write_status_buf_data *data)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
#define BUF_MIN_ADD(sub,...)	*data->count += (scnprintf(data->buf+(*data->count - sub), data->len-(*data->count - sub), __VA_ARGS__) - sub)
	static const char tab_list[] = "\t\t\t\t\t\t\t\t";
	/* Net info */
	BUF_ADD("%.*s\"net\":\n%.*s{\n", data->ntabs, tab_list, data->ntabs, tab_list);
	if (net) {
		enum nvmeibc_ib_net_state net_state = nvmeib_get_state_guard(&net->state);
		data->ntabs++;
		BUF_ADD("%.*s\"state\": \"%s\",\n", data->ntabs, tab_list, nvmeibc_ib_net_state_str(net_state));
		/* Local info */
		BUF_ADD("%.*s\"local\":\n%.*s{\n", data->ntabs, tab_list, data->ntabs, tab_list);
		data->ntabs++;
		BUF_ADD("%.*s\"gid\": \"%pI6\",\n", data->ntabs, tab_list, &net->path.sgid);
		BUF_ADD("%.*s\"qpn\": \"%#x\",\n", data->ntabs, tab_list, (net->qp ? net->qp->qp_num : 0));
		if (net->port) {
			BUF_ADD("%.*s\"ibdev\": \"%s\",\n", data->ntabs, tab_list, PORT_TO_DEV_NAME(net->port));
			BUF_ADD("%.*s\"ibport\": \"%d\",\n", data->ntabs, tab_list, PORT_TO_PORT_NUM(net->port));
			BUF_ADD("%.*s\"roce\": \"%s\",\n", data->ntabs, tab_list, PORT_TO_LINK_LAYER(net->port));
		} else {
			BUF_ADD("%.*s\"ibdev\": null,\n", data->ntabs, tab_list);
			BUF_ADD("%.*s\"ibport\": null,\n", data->ntabs, tab_list);
			BUF_ADD("%.*s\"roce\": null,\n", data->ntabs, tab_list);
		}
		if (net->port && net->port->layer == IB_LINK_LAYER_ETHERNET) {
			if (ipv6_addr_v4mapped((void*)&net->path.sgid)) {
				BUF_ADD("%.*s\"ip_version\": \"4\",\n", data->ntabs, tab_list);
				BUF_ADD("%.*s\"ip_addr\": \"%pI4\",\n", data->ntabs, tab_list, ((u32*)&net->path.sgid)+3);
			} else {
				BUF_ADD("%.*s\"ip_version\": \"6\",\n", data->ntabs, tab_list);
				BUF_ADD("%.*s\"ip_addr\": \"%pI6\",\n", data->ntabs, tab_list, ((u32*)&net->path.sgid));
			}
			BUF_ADD("%.*s\"ndev\": \"%s\",\n", data->ntabs, tab_list, net->port->gid.ndev_name);
		}
		data->ntabs--;
		/* End the object (removing the final comma) */
		BUF_MIN_ADD(strlen(",\n"), "\n%.*s},\n", data->ntabs, tab_list);
		/* End of Local Info */

		/* Remote info */
		BUF_ADD("%.*s\"remote\":\n%.*s{\n", data->ntabs, tab_list, data->ntabs, tab_list);
		data->ntabs++;
		BUF_ADD("%.*s\"gid\": \"%pI6\",\n", data->ntabs, tab_list, &net->path.dgid);
		BUF_ADD("%.*s\"qpn\": \"%#x\",\n", data->ntabs, tab_list, (net->qp ? net->qp->qp_num : 0));
		if (net->port && net->port->layer == IB_LINK_LAYER_ETHERNET) {
			if (ipv6_addr_v4mapped((void*)&net->path.dgid)) {
				BUF_ADD("%.*s\"ip_version\": \"4\",\n", data->ntabs, tab_list);
				BUF_ADD("%.*s\"ip_addr\": \"%pI4\",\n", data->ntabs, tab_list, ((u32*)&net->path.dgid)+3);
			} else {
				BUF_ADD("%.*s\"ip_version\": \"6\",\n", data->ntabs, tab_list);
				BUF_ADD("%.*s\"ip_addr\": \"%pI6\",\n", data->ntabs, tab_list, ((u32*)&net->path.dgid));
			}
			BUF_ADD("%.*s\"ndev\": \"%s\",\n", data->ntabs, tab_list, net->port->gid.ndev_name);
		}
		data->ntabs--;
		/* End the object (removing the final comma) */
		BUF_MIN_ADD(strlen(",\n"), "\n%.*s},\n", data->ntabs, tab_list);
		/* End of remote info */
		data->ntabs--;
		/* End the object (removing the final comma) */
		BUF_MIN_ADD(strlen(",\n"), "\n%.*s},\n", data->ntabs, tab_list);
	} else
		BUF_ADD("%.*s},\n", data->ntabs, tab_list);

	/* End of net info*/
#undef BUF_ADD
#undef BUF_MIN_ADD
}

struct fill_trend_arg {
	char *buf;
	ssize_t *count;
	size_t *len;
	const char * const *enum_to_str;
	const char *tabs;
};

static int fill_trend(struct nvmeib_single_trend_t *data, void *arg) {
	#define BUF_ADD(...)	*trend_arg->count += scnprintf(trend_arg->buf+*trend_arg->count, *trend_arg->len-*trend_arg->count, __VA_ARGS__)
	struct rtc_time tm;
	struct fill_trend_arg *trend_arg = (struct fill_trend_arg *)arg;

	/* note the no tz support in Linux kernel */
	rtc_time_to_tm(data->tv.tv_sec, &tm);

	BUF_ADD("%s%02d/%02d/%04d %02d:%02d:%02d(UTC)\t%s\n", trend_arg->tabs,
	 tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec,
	 trend_arg->enum_to_str[data->data]);

	#undef BUF_ADD
	return 0;
}

static int arnic_status_fill_buf(struct nvmeibc_admin_rnic *arnic, void *args, bool last)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	struct write_status_buf_data *data = args;
	struct fill_trend_arg trend_arg = {.buf = data->buf, .count = data->count, .len = &data->len,
									   .tabs = "\t\t", .enum_to_str=nvmeibc_arnic_discover_status_str};
	int rv = 0;
	BUF_ADD("\t[%u] - GID: %pI6 Local: %s Alive: %s Channel: %px Link-Layer: %s Transport: %s Priority: %u (%d/%d/%d/%d) \n",
		arnic->order,
		&arnic->ib_gid.raw,
		BOOL_TO_STRING(arnic->local),
		BOOL_TO_STRING(arnic->alive),
		arnic->channel,
		LAYER_TO_STRING(arnic->link_layer),
		TRANSPORT_TO_STRING(arnic->transport_type),
		arnic->priority.raw,
		arnic->priority.bw,
		arnic->priority.latency,
		arnic->priority.transport,
		arnic->priority.numa_dist);

	if (arnic->channel)
		list_add_unique_ptr(&data->admin_ch_list, arnic->channel);

	BUF_ADD("\tSTATUS HISTORY\n");
	nvmeib_trend_foreach(&arnic->discover_trend, fill_trend ,&trend_arg);

	return rv;
#undef BUF_ADD
}

static u64 get_max_per_cpu64(u64 *per_cpu64)
{
	u64 max64 = 0, cpu64;
	int cpu;
	for_each_possible_cpu(cpu) {
		cpu64 = *(per_cpu_ptr(per_cpu64, cpu));
		if (cpu64 > max64)
			max64 = cpu64;
	}
	return max64;
}

static inline u64 last_max_jif_to_ms_int(u64 *per_cpu64)
{
	return (1000 * (jiffies - get_max_per_cpu64(per_cpu64)) / HZ);
}

static inline void rionic_status_fill_line(struct nvmeibc_io_rnic *rionic, struct write_status_buf_data *data)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	BUF_ADD("\t RIONIC (%px) HW-GID: %pI6 Node GUID: %16llx acc: %d pKey: %x HW-type: %d Link-Layer: %s Transport: %s Admin Channel: %px Priority %u (%d/%d/%d/%d) \n",
		rionic, &rionic->hw_gid.raw, be64_to_cpu(rionic->node_guid), rionic->may_access, rionic->pkey, rionic->hw_type,
		LAYER_TO_STRING(rionic->layer), TRANSPORT_TO_STRING(rionic->transport_type), rionic->ch,
		rionic->priority.raw, rionic->priority.bw, rionic->priority.latency, rionic->priority.transport, rionic->priority.numa_dist);
}

static inline void lionic_status_fill_line(struct nvmeibc_io_lnic *lionic, struct write_status_buf_data *data, bool is_rdda)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	BUF_ADD("\t\t LIONIC (%px) HW-GID: %pI6 acc: %d last-(ka, tx, rx) ms: (%llu, %llu, %llu) Num QPs: %d Device: %s Port: %d Link-Layer: %s Transport: %s\n",
			lionic, &lionic->port->gid.hw_gid, lionic->may_access,
			last_max_jif_to_ms_int(lionic->last_io_ka_jif), last_max_jif_to_ms_int(lionic->last_send_success_jif),
	   last_max_jif_to_ms_int(lionic->last_recv_success_jif), lionic->n_nr_qps,
			PORT_TO_DEV_NAME(lionic->port), PORT_TO_PORT_NUM(lionic->port), PORT_TO_LINK_LAYER(lionic->port), PORT_TO_TRANSPORT_TYPE(lionic->port));
	BUF_ADD("\t\t        path sgid: %pI6 dgid: %pI6\n",
			&lionic->path.sgid, &lionic->path.dgid);
}

static int rionic_status_fill_buf(struct nvmeibc_io_rnic *rionic, void *args)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	struct write_status_buf_data *data = args;
	int rv = 0;

	rionic_status_fill_line(rionic, data);
	return rv;
#undef BUF_ADD
}

static int nr_rionic_status_fill_buf(struct nvmeibc_io_rnic *rionic, void *args, bool is_last)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	struct write_status_buf_data *data = args;
	struct nvmeibc_io_lnic *lionic;
	struct ppath_stats nr_ppath_stats = {0};
	int rv = 0, i;
	(void)is_last;

	memset(&nr_ppath_stats, 0, sizeof(nr_ppath_stats));
	rionic_status_fill_line(rionic, data);
	list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
		if (!atomic_read(&lionic->dying)) {
			lionic_status_fill_line(lionic, data, false);
			for (i = 0; i < lionic->n_nr_qps; i++) {
				struct nvmeibc_ib_nordda_channel *io_ch = (struct nvmeibc_ib_nordda_channel *)lionic->nr_channels + i;
				if (io_ch) {
					BUF_ADD("\t\t\t- NO-RDDA IO CHANNEL %d (%px) - %s Index: %d, Priority: %u (%d/%d/%d/%d), In-Use: %s, vec[s/r]=%d/%d, cpu=%d(+%d*x), comp-cpu=%d, pcpu_ll=%s, "
							"Reqs(ulp/used/max/num): %d/%d/%d/%d [%llu], "
							"reuse-bbs-lru-jif={ts=%lu, dt=%lu} \n", i, io_ch,
							io_ch->base.name, io_ch->base.index,
							io_ch->priority.raw, io_ch->priority.bw, io_ch->priority.latency, io_ch->priority.transport, io_ch->priority.numa_dist,
							BOOL_TO_STRING(io_ch->inuse), io_ch->net.base.recv_intr_vec, io_ch->net.base.send_intr_vec, io_ch->cpu,
							io_ch->base.disk->info->n_avail_norddas, pcpu_nrch_cpu_get(io_ch), BOOL_TO_STRING(is_ll_pcpu_nrch(io_ch)),
							io_ch->base.reused_bb_cnt, io_ch->n_used_reqs, nr_max_used_reqs_per_channel, io_ch->base.disk->nrch_ioreq_num, io_ch->n_uses_ever,
							io_ch->base.reused_bb_lru_jif,
							io_ch->base.reused_bb_lru_jif ? io_ch->base.reused_bb_lru_jif - jiffies : 0);
					nvmeibc_nr_lat_meas_nrch_status_fill_buf(io_ch->per_cpu_lat_data, data->buf, data->len, data->count);

					net_status_fill_buf(&io_ch->net.base, data, "\t\t\t\t- ");

					nr_ppath_stats.ppath_used_reqs += io_ch->n_used_reqs;
					nr_ppath_stats.ppath_reused_bb += io_ch->base.reused_bb_cnt;
					if (nr_ppath_stats.ppath_reused_bb_lru_jif == 0 ||
						nr_ppath_stats.ppath_reused_bb_lru_jif > io_ch->base.reused_bb_lru_jif) {
						nr_ppath_stats.ppath_reused_bb_lru_jif = io_ch->base.reused_bb_lru_jif;
					}
				} else
					BUF_ADD("\t\t\t- NO-RDDA IO CHANNEL %d - NULL\n", i);
			}
		}
	}

	BUF_ADD("\t\t> Total per-path: reqs={used=%d, ulp-owned=%d, lru-jif={ts=%lu, dt=%lu}}\n",
			nr_ppath_stats.ppath_used_reqs,
			nr_ppath_stats.ppath_reused_bb,
			nr_ppath_stats.ppath_reused_bb_lru_jif,
			nr_ppath_stats.ppath_reused_bb_lru_jif ?
			nr_ppath_stats.ppath_reused_bb_lru_jif - jiffies : 0);

	data->nr_ppath_stats_sum.ppath_used_reqs += nr_ppath_stats.ppath_used_reqs;
	data->nr_ppath_stats_sum.ppath_reused_bb += nr_ppath_stats.ppath_reused_bb;
	if (data->nr_ppath_stats_sum.ppath_reused_bb_lru_jif == 0 ||
		data->nr_ppath_stats_sum.ppath_reused_bb_lru_jif > nr_ppath_stats.ppath_reused_bb_lru_jif) {
		data->nr_ppath_stats_sum.ppath_reused_bb_lru_jif = nr_ppath_stats.ppath_reused_bb_lru_jif;
	}

	return rv;
#undef BUF_ADD
}

static void fill_lock_ch_list(struct nvmeibc_disk *disk, struct list_head *lock_ch_list) {
	unsigned long flags;
	int i;
	struct nvmeibc_disk_used_lock_segment *uls;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;

	spin_lock_irqsave(&disk->spinlock, flags);
	list_for_each_entry(uls, &disk->used_lock_segments, link) {
		struct nvmeibc_disk_seg_locks_mem_info *lmi = uls->mem_info;
		if (lmi && lmi->locks_channel)
			list_add_unique_ptr(lock_ch_list, lmi->locks_channel);
	}
	spin_unlock_irqrestore(&disk->spinlock, flags);

	disk_segs_locks = nvmeibc_disk_get_segs_locks(
		disk, (struct nvmeibc_disk_get_segs_locks_flags) { .write = 0, .dont_wait = 0});
	if (disk_segs_locks) {
		if (disk_segs_locks->locks) {
			/* disk_segs_locks->locks may be NULL if parse_read_lock_mems_msg has not been called yet*/
			for (i = 0; i < disk_segs_locks->num_of_segments; i++) {
				struct nvmeibc_disk_seg_locks_mem_info *lmi = &disk_segs_locks->locks[i];
				if (lmi->locks_channel)
					list_add_unique_ptr(lock_ch_list, lmi->locks_channel);
			}
		}
		nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });
	}
}

static int lnic_status_fill_buf(struct nvmeibc_local_nic *ln, void *args)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	struct write_status_buf_data *data = args;
	struct nvmeibc_local_nic_port *lnp;
	struct nvmeibc_ib_port *port;

	BUF_ADD("\t- Device: %s Ports: %d %s\n", ln->nic_dev->dev->ib_dev->name, ln->n_ports,
			ln->nic_dev->device_used ? "Used" : "Unused");
	list_for_each_entry(lnp, &ln->ports, link) {
		port = lnp->ib_port;
		BUF_ADD("\t\t- Port %d, HW-GID: %pI6\n", port->port, &port->gid.hw_gid);
		BUF_ADD("\t\t\t- GID: %pI6 GID Type: %s Net Device: %s Valid: %s\n",
			&port->gid.gid.raw,
			nvmeib_rdma_gid_type_str(port->gid.gid_type, port->gid.link_layer),
			port->gid.ndev_name,
			BOOL_TO_STRING(port->gid.valid));
		BUF_ADD("\t\t\t- Status: %s Link-Layer: %s Transport: %s\n",
			(port->port_active ? "Active" : (port->port_used ? "Inactive" : "Unused")),
			LAYER_TO_STRING(port->layer), TRANSPORT_TO_STRING(port->transport_type));
	}
	return 0;
#undef BUF_ADD
}

#define CORE_CLIENT_DISK_STATUS_HEAD_PROC_FRMT_VER 1
static ssize_t write_status_head(struct nvmeibc_disk *disk, char *buf, size_t len)
{
#define BUF_ADD(...)	count += scnprintf(buf+count, len-count, __VA_ARGS__)
	ssize_t count = 0;

	struct fill_trend_arg trend_arg = {.buf = buf, .count = &count, .len = &len,
									   .tabs = "\t"};

	NFIN;
	BUF_ADD("Disk: %s (Ptr: %px WQ-PID: %d)\n", disk->name, disk, nvmeib_qpid(disk->remove_wq));
	BUF_ADD("**********************************************************\n");
	BUF_ADD("STATUS\n");
	BUF_ADD("\t - Status: %s\n", (atomic_read(&disk->paused) ? "Paused" : (atomic_read(&disk->dying) ? "Dying" : "Online")));
	BUF_ADD("\t - Last Discover rv: %d Last Discover status: %s\n", disk->discover_rv,
			 nvmeibc_disk_discover_status_str[NVMEIB_TREND_HEAD(disk->discover_trend).data]);
	BUF_ADD("INFO\n");
	BUF_ADD("\t - Full Name: %s, sector=%d[b]\n", disk->full_name, (1 << disk->sector_shift));
	BUF_ADD("\t - Host: %s\n", disk->disk_host);
	BUF_ADD("\t - Node: %s\n", disk->config_node_id);
	BUF_ADD("\t - Target Version: " NVMEIB_VERSION_PRINT_FMT() "\n", NVMEIB_VERSION_PRINT_ARG((&disk->last_tgt_ver)));
	BUF_ADD("\t - Link Version: " NVMEIB_VERSION_PRINT_PROT_FMT() "\n", NVMEIB_VERSION_PRINT_PROT_ARG((&disk->last_tgt_link_ver)));
	BUF_ADD("FLAGS\n");
	BUF_ADD("\t- Paused: %d Should Pause: %s Pausing: %s Dying: %d Shutdown Triggered: %d\n",
		atomic_read(&disk->paused), BOOL_TO_STRING(disk->should_pause), BOOL_TO_STRING(disk->pausing), atomic_read(&disk->dying), atomic_read(&disk->shut_down_triggered));
	/* Get debug info for pause state */
#ifdef DEBUG_TRANSFERS
	do {
		unsigned long long n_xfer = 0;
		int cpu;
		unsigned long flags;
		for_each_possible_cpu(cpu) {
			spin_lock_irqsave(&disk->transfer_spinlock[cpu], flags);
			n_xfer += disk->n_transferring[cpu];
			spin_unlock_irqrestore(&disk->transfer_spinlock[cpu], flags);
		}
		BUF_ADD("\t- n_transferring: %llu\n", n_xfer);
	} while(0);
#endif
	BUF_ADD("PAUSABLE\n\t -");
	count += nvmeibc_pd_tostring(disk, buf + count, len - count);

	BUF_ADD("PENDING\n\t - ");
	if (disk->info)
		BUF_ADD("\t - Total=%llu, IO=%llu, NRCH-only=%llu\n",
				disk->info->tot_pending, disk->info->tot_io_pending, disk->info->n_use_nrch_only);

	BUF_ADD("RELEASE REASONS HISTORY\n");
	trend_arg.enum_to_str = nvmeibc_disk_release_reason_str;
	nvmeib_trend_foreach(&disk->release_trend, fill_trend ,&trend_arg);
	BUF_ADD("PEER RELEASE REASON HISTORY\n");
	nvmeib_trend_foreach(&disk->peer_release_reason_trend, fill_trend ,&trend_arg);

	BUF_ADD("DISCOVER STATUS HISTORY\n");
	trend_arg.enum_to_str = nvmeibc_disk_discover_status_str;
	nvmeib_trend_foreach(&disk->discover_trend, fill_trend ,&trend_arg);

	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_STATUS_HEAD_PROC_FRMT_VER, buf+count, len-count);


	NFOUT;
#undef BUF_ADD
	return count;
}

static void write_nrch_buf(struct write_status_buf_data *data) {
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	unsigned long flags;
	struct nvmeibc_disk *disk = data->disk;

	spin_lock_irqsave(&disk->spinlock, flags);

	BUF_ADD("NO-RDDA IO CHANNELS\n");
	if (!list_empty(&disk->nr_rionics)) {
		BUF_ADD("\tIO REQ NUMBER:%u\n", disk->nrch_ioreq_num);
		call_for_each_nr_rionic(disk, nr_rionic_status_fill_buf, data);

		BUF_ADD("\t> Total per-nordda: reqs={used=%d, ulp-owned=%d, lru-jif={ts=%lu, dt=%lu}}\n",
				data->nr_ppath_stats_sum.ppath_used_reqs,
				data->nr_ppath_stats_sum.ppath_reused_bb,
				data->nr_ppath_stats_sum.ppath_reused_bb_lru_jif,
				data->nr_ppath_stats_sum.ppath_reused_bb_lru_jif ? data->nr_ppath_stats_sum.ppath_reused_bb_lru_jif - jiffies : 0);

	} else {
		BUF_ADD("\t- None\n");
	}

	spin_unlock_irqrestore(&disk->spinlock, flags);

#undef BUF_ADD
}

static void write_ioch_buf(struct write_status_buf_data *data) {
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	unsigned long flags;
	struct nvmeibc_disk *disk = data->disk;

	spin_lock_irqsave(&disk->spinlock, flags);

	BUF_ADD("IO CHANNELS\n");
	if (disk->tgt_ofed_kern_mismatch) {
		BUF_ADD("\t - Target OFED/Kernel Version Mismatch\n");
		BUF_ADD("\t\t - Client: %s/%s\n", OFED_VER_STRING, KERN_VER_STRING);
		BUF_ADD("\t\t - Target: %s/%s\n", disk->tgt_ofed_ver, disk->tgt_kern_ver);
	} else {
		if (!list_empty(&disk->rionics))
			call_for_each_rionic(disk, rionic_status_fill_buf, data);
		else
			BUF_ADD("\t - None\n");
	}

	spin_unlock_irqrestore(&disk->spinlock, flags);

#undef BUF_ADD
}

enum toma_conn_subscribe_state {
	TOMA_NEED_SUBSCRIBED    	= 1,
	TOMA_ALREADY_SUBSCRIBED 	= 2,
	TOMA_ASYNC_SUBSCRIBE_SENT	= 3,
};
struct nvmeibc_toma_connection_hash_entry {
	u64 handle; /* hash key */
	nvmeibc_disk_toma_recv_req_callback_t *recv_req_cb;
	u64 arg;
	enum toma_conn_subscribe_state subscribed;
	struct hlist_node hlist_next;
};

static void write_dma_pools_buf(struct nvmeibc_disk *disk, struct write_status_buf_data *data, enum nvmeib_dma_pool_type pool_type, char *pool_name)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)

	struct nvmeib_pool_percpu_counts *pcpu_counts;
	int tot_in_use = 0, tot_alloc_fail = 0, tot_in_place_free = 0, tot_diff_cpu_free = 0, tot_free_works_executed = 0;
	int cpu;
	for_each_online_cpu(cpu) {
		pcpu_counts = &per_cpu_ptr(disk->local.dma_pools, cpu)->pcpu_counts[pool_type];
		tot_in_use += pcpu_counts->in_use;
		tot_alloc_fail += pcpu_counts->alloc_fail;
		tot_in_place_free += pcpu_counts->in_place_free;
		tot_diff_cpu_free += pcpu_counts->diff_cpu_free;
		tot_free_works_executed += pcpu_counts->free_works_executed;
	}
	BUF_ADD("\t - %s Pool In-Use: %d Alloc-Fail: %d In-Place-Free: %d Diff-CPU-Free: %d Free-Works-Executed: %d\n", pool_name, tot_in_use, tot_alloc_fail, tot_in_place_free, tot_diff_cpu_free, tot_free_works_executed);
#undef BUF_ADD
}

static void write_status_buf(struct write_status_buf_data *data)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
	struct nvmeibc_disk *disk = data->disk;
	struct nvmeibc_disk_used_lock_segment *uls;
	struct unique_list_ent *unique_ent;
	int i;
	unsigned long flags;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;

	int bucket;
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibc_toma_connection_hash_entry *h_curr;
	NFIN;
	*data->count += write_status_head(disk, data->buf+*data->count, data->len-*data->count);

#ifdef DEBUG_SUM
	BUF_ADD("TRANSFER COUNTERS\n");
	BUF_ADD("\t - Transfers: %d\n", atomic_read(&disk->in_transfers));
#endif
	BUF_ADD("SUBSCRIBE HANDLES HISTORY\n");
	__hash_for_each_safe__(disk->toma_conn_hash, bucket, t_node, h_node, h_curr, hlist_next) {
		switch (h_curr->subscribed) {
			case TOMA_NEED_SUBSCRIBED:
				BUF_ADD("\t\thandle: 0x%llx, state: TOMA_NEED_SUBSCRIBED\n",h_curr->handle);
				break;
			case TOMA_ALREADY_SUBSCRIBED:
				BUF_ADD("\t\thandle: 0x%llx, state: TOMA_ALREADY_SUBSCRIBED\n",h_curr->handle);
				break;
			case TOMA_ASYNC_SUBSCRIBE_SENT:
				BUF_ADD("\t\thandle: 0x%llx, state: TOMA_ASYNC_SUBSCRIBE_SENT\n",h_curr->handle);
				break;
			default:
				BUF_ADD("\t\thandle: 0x%llx, state: UNKNOWN STATE\n",h_curr->handle);
				break;
		}
	}

	/* Print list of arnics and add their channels to the admin_ch list */
	BUF_ADD("ARNICS\n");
	if (!list_empty(&disk->arnics))
		call_for_each_arnic(disk, arnic_status_fill_buf, data);
	else
		BUF_ADD("\t - None\n");
	BUF_ADD("LOCAL NICS\n");
	if (!list_empty(&disk->local_nics))
		call_for_each_lnic(disk, lnic_status_fill_buf, data);
	else
		BUF_ADD("\t - None\n");
	BUF_ADD("LOCAL SERVER (%px)\n", disk->local_server);
	if (disk->local_server) {
		BUF_ADD("\t- is_local: %pS, cl-reg: %pS, cl-unreg: %pS, "
				"cl-alloc-locks: %pS, cl-alloc-jrnl-rng: %pS,  "
				"local-cmd: %pS, dma-device: %pS, gen-cmd: %pS\n",
			disk->local_server->is_local_disk,
			disk->local_server->cl_register,
			disk->local_server->cl_unregister,
			disk->local_server->cl_alloc_locks,
			disk->local_server->cl_alloc_jrnl_rng,
			disk->local_server->local_cmd,
			disk->local_server->dma_device,
			disk->local_server->gen_cmd);
	}
	BUF_ADD("LOCAL DISK\n");
	BUF_ADD("\t- Ptr: %px\n", disk->local.p);
	BUF_ADD("LOCKS\n");
	BUF_ADD("\t- Used Segments:\n");
	spin_lock_irqsave(&disk->spinlock, flags);
	list_for_each_entry(uls, &disk->used_lock_segments, link) {
		struct nvmeibc_disk_seg_locks_mem_info *lmi = uls->mem_info;
		if (lmi) {
			BUF_ADD("\tHandle: %px Segment ID: %x Mem Info: %px\n", uls, uls->seg_id, uls->mem_info);
			BUF_ADD("\t\t ID: %d Start Addr: %llx Size: %llu Len: %llu Dirty Bit Offset: %llu Lock ID: %llx Channel: %p\n",
			lmi->seg_id, lmi->start_addr, lmi->lock_set_size, lmi->len, lmi->dirty_bit_offset,  lmi->lock_id,
			lmi->locks_channel);
			if (lmi->locks_channel) {
				list_add_unique_ptr(&data->lock_ch_list, lmi->locks_channel);
			}
		}
	}
	spin_unlock_irqrestore(&disk->spinlock, flags);
	disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
												  (struct nvmeibc_disk_get_segs_locks_flags) { .write = 0, .dont_wait = 0});
	if (disk_segs_locks) {
		BUF_ADD("\t- Local Segments: %d\n", disk_segs_locks->num_of_segments);
		if (!disk_segs_locks->locks) {
			BUF_ADD("\t\t NULL\n");
		} else {
			for (i = 0; i < disk_segs_locks->num_of_segments; i++) {
				struct nvmeibc_disk_seg_locks_mem_info *lmi = &disk_segs_locks->locks[i];
				BUF_ADD("\t\t [%d] ID: %d Start Addr: %llx Size: %llu Len: %llu Dirty Bit Offset: %llu Lock ID: %llx Channel: %px\n",
					i, lmi->seg_id, lmi->start_addr, lmi->lock_set_size, lmi->len, lmi->dirty_bit_offset,  lmi->lock_id,
					lmi->locks_channel);
				if (lmi->locks_channel)
					list_add_unique_ptr(&data->lock_ch_list, lmi->locks_channel);
			}
		}
		nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });
	}

	BUF_ADD("ADMIN CHANNELS\n");
	while ((unique_ent = list_first_entry_or_null(&data->admin_ch_list, struct unique_list_ent, link))) {
		struct nvmeibc_admin_channel *ac = unique_ent->ptr;
		BUF_ADD("\t - Admin Channel: %px Name: %s Version: "
			NVMEIB_VERSION_PRINT_FMT() "\n",
			ac, ac->base.name, NVMEIB_VERSION_PRINT_ARG(&ac->link_version));

		net_status_fill_buf(&ac_to_iac(ac)->net.base, data, "\t\t- ");
		list_del(&unique_ent->link);
		kfree(unique_ent);
	}

	BUF_ADD("LOCK CHANNELS\n");
	while ((unique_ent = list_first_entry_or_null(&data->lock_ch_list, struct unique_list_ent, link))) {
		struct nvmeibc_locks_channel *lock_ch = unique_ent->ptr;
		BUF_ADD("\t - Primary Lock Channel: %px Name: %s - atomic-caps={%d,%d}, Paused: %s Dying %d Ops in Progress %d/%d, "
				"n_comp_llp={o=%llu, k=%llu, t=%llu}\n",
			lock_ch, lock_ch->base.name, lock_ch->atomic_cap, lock_ch->masked_atomic_cap, BOOL_TO_STRING(lock_ch->paused),
			atomic_read(&lock_ch->base.dying), lock_ch->num_in_progress, lock_ch->num_of_free,
				lock_ch->n_comp_llp_opr, lock_ch->n_comp_llp_ka, lock_ch->n_comp_llp_test);
		net_status_fill_buf(&lock_ch->net, data, "\t\t- ");
		for (i = 0; i < NVMEIB_N_2ND_LOCK_CHS; i++) {
			struct nvmeibc_locks_channel *_2nd_lock_ch = lock_ch->_2nd_ch[i];
			if (_2nd_lock_ch) {
				BUF_ADD("\t - Secondary Lock Channel: %px Name: %s - Paused: %s Dying %d Ops in Progress %d/%d, comp-cpu=%d, pcpu_ll=%s, "
						"n_comp_llp={o=%llu, k=%llu, t=%llu}\n",
					_2nd_lock_ch, _2nd_lock_ch->base.name, BOOL_TO_STRING(_2nd_lock_ch->paused),
					atomic_read(&_2nd_lock_ch->base.dying), _2nd_lock_ch->num_in_progress, _2nd_lock_ch->num_of_free,
					nvmeibc_channel_pcpu_ch_get_cpu(&_2nd_lock_ch->base), BOOL_TO_STRING(nvmeibc_channel_is_ll_pcpu_ch(&_2nd_lock_ch->base)),
					_2nd_lock_ch->n_comp_llp_opr, _2nd_lock_ch->n_comp_llp_ka, _2nd_lock_ch->n_comp_llp_test);
				net_status_fill_buf(&_2nd_lock_ch->net, data, "\t\t- ");
			}
		}
		list_del(&unique_ent->link);
		kfree(unique_ent);
	}

	BUF_ADD("SUMMARY ALL IO CHANNELS\n");
	BUF_ADD("\t - Time without IO channels: %lu jiffies\n",
			 disk->no_io_time? (jiffies - disk->no_io_time) : 0);
	BUF_ADD("\t - Number of connected IO/RDDA channels: %d\n", atomic_read(&disk->connected_io_channels));

	write_nrch_buf(data);
	write_ioch_buf(data);

	if (disk->jam_disk) {
		BUF_ADD("JAM\n");
		*data->count += nvmeibc_jam_fill_disk_status(disk, data->buf + *data->count, data->len - *data->count);
	}

	BUF_ADD("CONTENDED LOCK STATS\n");
	*data->count += nvmeibc_disk_contended_locks_stats_fill(disk, data->buf + *data->count, data->len - *data->count);

	if (disk->access_local) {
		BUF_ADD("LOCAL DISK\n");
		if (disk->local.local_io_use_prpl) {
			write_dma_pools_buf(disk, data, NVMEIB_DMA_POOL_TYPE_PRPL, "PRPL");
		}
		if (disk->local.local_io_use_rd_md_pool) {
			write_dma_pools_buf(disk, data, NVMEIB_DMA_POOL_TYPE_RD_MD, "Read MD");
		}
		if (disk->local.local_io_use_md_dma_pool) {
			write_dma_pools_buf(disk, data, NVMEIB_DMA_POOL_TYPE_DUMMY_MD, "Dummy MD");
		}
		if (disk->local.local_io_use_data_copy) {
			write_dma_pools_buf(disk, data, NVMEIB_DMA_POOL_TYPE_DATA, "Data Copy");
		}
	}

	NFOUT;
#undef BUF_ADD
}

static int arnic_status_json_fill_buf(struct nvmeibc_admin_rnic *arnic, void *args, bool last)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
#define BUF_MIN_ADD(sub,...)	*data->count += (scnprintf(data->buf+(*data->count - sub), data->len-(*data->count - sub), __VA_ARGS__) - sub)
	struct write_status_buf_data *data = args;
	static const char tab_list[] = "\t\t\t\t\t\t\t\t";
	if (arnic->channel) {
		BUF_ADD("%.*s{\n", data->ntabs, tab_list);
		data->ntabs++;
		BUF_ADD("%.*s\"name\": \"%s\",\n", data->ntabs, tab_list, arnic->channel->base.name);
			net_status_json_fill_buf(&ac_to_iac(arnic->channel)->net.base, data);
		data->ntabs--;
		/* End the object (removing the final comma) */
		BUF_MIN_ADD(strlen(",\n"), "\n%.*s}\n", data->ntabs, tab_list);
	}
	return 0;
#undef BUF_ADD
#undef BUF_MIN_ADD
}

static void write_status_json_buf(struct write_status_buf_data *data)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)
#define BUF_MIN_ADD(sub,...)	*data->count += (scnprintf(data->buf+(*data->count - sub), data->len-(*data->count - sub), __VA_ARGS__) - sub)
	struct nvmeibc_disk *disk = data->disk;
	struct nvmeibc_disk_used_lock_segment *uls;
	unsigned long flags;
	static const char tab_list[] = "\t\t\t\t\t\t\t\t";
	const int discover_status = (NVMEIB_TREND_IS_EMPTY(disk->discover_trend)? 0 : NVMEIB_TREND_HEAD(disk->discover_trend).data);

	NFIN;
	/* Disk Info Header */
	BUF_ADD("%.*s\"%s\":\n", data->ntabs, tab_list, disk->name);
	BUF_ADD("%.*s{\n", data->ntabs, tab_list);
	/* In Disk Info */
	data->ntabs++;
	BUF_ADD("%.*s\"full_name\": \"%s\",\n", data->ntabs, tab_list, disk->full_name);
	BUF_ADD("%.*s\"status\": \"%s\",\n", data->ntabs, tab_list, (atomic_read(&disk->paused) ? "paused" : (atomic_read(&disk->dying) ? "dying" : "online")));
	BUF_ADD("%.*s\"discover_error\": \"%d\",\n", data->ntabs, tab_list, disk->discover_rv);
	BUF_ADD("%.*s\"discover_status\": \"%s\",\n", data->ntabs, tab_list,
												nvmeibc_disk_discover_status_str[discover_status]);
	BUF_ADD("%.*s\"sector\": \"%d\",\n", data->ntabs, tab_list, (1 << disk->sector_shift));
	BUF_ADD("%.*s\"host\": \"%s\",\n", data->ntabs, tab_list, disk->disk_host);
	BUF_ADD("%.*s\"config_node\": \"%s\",\n", data->ntabs, tab_list, disk->config_node_id);
	BUF_ADD("%.*s\"local\": \"%s\",\n", data->ntabs, tab_list, BOOL_TO_STRING(disk->local.p != NULL));
	/* Start Admin Channel Info */
	BUF_ADD("%.*s\"admin_ch\":\n%.*s[\n", data->ntabs, tab_list, data->ntabs, tab_list);
	/* In Admin Channel Info */
	data->ntabs++;
	if (!list_empty(&disk->arnics))
		call_for_each_arnic(disk, arnic_status_json_fill_buf, data);
	data->ntabs--;
	BUF_ADD("%.*s],\n", data->ntabs, tab_list);
	/* End Admin Channel Info */

	/* Start Lock Channel Info  */
	BUF_ADD("%.*s\"lock_ch\":\n%.*s[\n", data->ntabs, tab_list, data->ntabs, tab_list);
	/* In Lock Channel Info */
	data->ntabs++;
	spin_lock_irqsave(&disk->spinlock, flags);
	list_for_each_entry(uls, &disk->used_lock_segments, link) {
		struct nvmeibc_disk_seg_locks_mem_info *lmi = uls->mem_info;
		if (lmi && lmi->locks_channel) {
			BUF_ADD("%.*s{\n", data->ntabs, tab_list);
			data->ntabs++;
			BUF_ADD("%.*s\"name\": \"%s\",\n", data->ntabs, tab_list, lmi->locks_channel->base.name);
			BUF_ADD("%.*s\"paused\": \"%s\",\n", data->ntabs, tab_list, BOOL_TO_STRING(lmi->locks_channel->paused));
			BUF_ADD("%.*s\"dying\": \"%d\",\n", data->ntabs, tab_list, atomic_read(&lmi->locks_channel->base.dying));
			net_status_json_fill_buf(&lmi->locks_channel->net, data);
			data->ntabs--;
			/* End the object (removing the final comma) */
			BUF_MIN_ADD(strlen(",\n"), "\n%.*s}\n", data->ntabs, tab_list);
		}
	}
	spin_unlock_irqrestore(&disk->spinlock, flags);
	data->ntabs--;
	BUF_ADD("%.*s],\n", data->ntabs, tab_list);
	/* End Lock Channel Info */
	data->ntabs--;
	/* End the object (removing the final comma) */
	BUF_MIN_ADD(strlen(",\n"), "\n%.*s}\n", data->ntabs, tab_list);
	/* End Disk Info */
#undef BUF_ADD
#undef BUF_MIN_ADD
	NFOUT;
}

static void write_status_buf_done_cb(void *ctx)
{
	struct completion *comp = ctx;
	complete(comp);
}

#define CORE_CLIENT_DISK_STATUS_JSON_PROC_FRMT_VER 1
static ssize_t __status_fill_buf(struct nvmeibc_disk *disk, char *buf, size_t len, enum write_status_type status_type)
{
#define BUF_ADD(...)	count += scnprintf(buf+count, len-count, __VA_ARGS__)
	ssize_t count = 0;
	ssize_t rv = 0;
	struct write_status_buf_data write_status_data = {
		.disk = disk,
		.buf = buf,
		.count = &count,
		.len = len,
		.status_type = status_type,
	};
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = DISK_UPDATE_WRITE_STATUS,
		.update_data = &write_status_data,
		.done_cb = write_status_buf_done_cb,
		.done_cb_ctx = &comp
	};

	INIT_LIST_HEAD(&write_status_data.admin_ch_list);
	INIT_LIST_HEAD(&write_status_data.lock_ch_list);
	INIT_LIST_HEAD(&write_status_data.io_ch_list);
	if (status_type == WRITE_STATUS_JSON)
		BUF_ADD("{\n");
	write_status_data.ntabs = 1;
	if ((rv = nvmeibc_disk_update_config(disk, &disk_update_data, false)) < 0) {
		_NE(error_1_status_fill_buf, "nvmeibc_disk_update_config failed rv=@RV", rv);
		goto out;
	}

	wait_for_completion(&comp);
	if (status_type == WRITE_STATUS_JSON)
	{
		count += nvmeib_proc_add_json_proc_epilog(CORE_CLIENT_DISK_STATUS_JSON_PROC_FRMT_VER, buf+count, len-count);
		BUF_ADD("}\n");
	}
#undef BUF_ADD

	rv = count;
out:
	return rv;
}

#define CORE_CLIENT_DISK_STATUS_PROC_FRMT_VER 1
static ssize_t io_ch_fill_buf_json(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count  = 0;
	NFIN;
	count += __status_fill_buf(disk, buf + count, len - count, WRITE_STATUS_IOCH_JSON);
	NFOUT;
	return count;
}

static ssize_t status_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;
	count += __status_fill_buf(disk, buf + count, len - count, WRITE_STATUS_TEXT);
	count += nvmeib_add_proc_status_footer(buf+count, len - count, "disk_status");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_STATUS_PROC_FRMT_VER, buf+count, len-count);
	return count;
}

#define CORE_CLIENT_DISK_NRCH_STATUS_PROC_FRMT_VER 1
static ssize_t nrch_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;
	count += __status_fill_buf(disk, buf + count, len - count, WRITE_STATUS_NRCH);
	count += nvmeib_add_proc_status_footer(buf+count, len - count, "nrch_status");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_NRCH_STATUS_PROC_FRMT_VER, buf+count, len-count);
	return count;
}

#define CORE_CLIENT_DISK_IOCH_STATUS_PROC_FRMT_VER 1
static ssize_t ioch_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;
	count += __status_fill_buf(disk, buf + count, len - count, WRITE_STATUS_IOCH);
	count += nvmeib_add_proc_status_footer(buf+count, len - count, "ioch_status");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_IOCH_STATUS_PROC_FRMT_VER, buf+count, len-count);
	return count;
}

#define CORE_CLIENT_DISK_COUNTERS_PROC_FRMT_VER 1
static ssize_t disk_counters_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;
	count += nvmeibc_disk_counters_fill(disk, buf + count, len - count);
	count += nvmeib_add_proc_status_footer(buf+count, len - count, "disk_counters");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_COUNTERS_PROC_FRMT_VER, buf+count, len-count);
	return count;
}

static ssize_t disk_counters_reset(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t rv = len;

	_NT(trace_disk_counters_reset,
		"Disk @DISK_NAMEs, reset counters /proc", disk->name);
	nvmeibc_disk_counters_reset(disk);

	return rv;
}

#define CORE_CLIENT_DISK_INTERRUPTS_PROC_FRMT_VER 1
static ssize_t disk_interrupts_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;
	count += nvmeibc_disk_net_intrs_stats_fill(disk, buf + count, len - count);
	count += nvmeib_add_proc_status_footer(buf+count, len - count, "disk_interrupts");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_INTERRUPTS_PROC_FRMT_VER, buf+count, len-count);
	return count;
}

static ssize_t disk_interrupts_reset(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t rv = len;

	_NT(trace_disk_interrupts_reset,
		"Disk @DISK_NAMEs, reset interrupts /proc", disk->name);
	nvmeibc_disk_net_intrs_stats_reset(disk);

	return rv;
}

#define CORE_CLIENT_DISK_QPS_PROC_FRMT_VER 1
static ssize_t disk_qps_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;
	count += __status_fill_buf(disk, buf + count, len - count, WRITE_STATUS_QPS); /* write_qps_buf */
	count += nvmeib_add_proc_status_footer(buf+count, len - count, "qps");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_QPS_PROC_FRMT_VER, buf+count, len-count);
	return count;
}

static ssize_t disk_qps_reset(void *priv, char *buf, size_t len)
{
    struct nvmeibc_disk *disk = priv;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = DISK_UPDATE_RESET_QP_STATS, /* reset_qp_stats */
		.update_data = disk,
		.done_cb = write_status_buf_done_cb,
		.done_cb_ctx = &comp
	};

    nvmeibc_disk_update_config(disk, &disk_update_data, false);
	wait_for_completion(&comp);

    return len;
}

#define CORE_CLIENT_DISK_CMDS_PROC_FRMT_VER 1
static ssize_t disk_cmds_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0;
	count += nvmeibc_disk_cmds_stats_fill(disk, buf + count, len - count);
	count += nvmeibc_disk_gen_cmds_stats_fill(disk, buf + count, len - count);
	count += nvmeib_add_proc_status_footer(buf+count, len - count, "disk_cmds");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DISK_CMDS_PROC_FRMT_VER, buf+count, len-count);
	return count;
}

static ssize_t unsafe_status_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	return write_status_head(disk, buf, len);
}

static ssize_t status_json_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	return __status_fill_buf(disk, buf, len, WRITE_STATUS_JSON);
}

static int map_ec_dirty_bits_to_nic(struct nvmeibc_local_nic* nic, void *args)
{
	struct nvmeibc_disk* disk = args;
	struct nvmeibc_disk_dirty_bits_mapping* nic_mapping;
	int rv = 0;

	NFIN;
	_NT(trace_disk_map_ec_dirty_bits_to_nic, "map disk's ec dirty bits memory to nic=@NIC, disk=@DISK", nic, disk);

	if (!nic->nic_dev->device_used) {
		_NT(trace_2_disk_map_ec_dirty_bits_to_nic,
			"skip unused lnic @DEVICE_NAME", nic->nic_dev->dev->ib_dev->name);
		rv = 0;
		goto out;
	}

	nic_mapping = kzalloc(sizeof(*nic_mapping), GFP_KERNEL);
	if (!nic_mapping) {
		_NE(error_disk_map_ec_dirty_bits_to_nic, "allocation failed");
		rv = -1;
		goto out;
	}

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for RDMA_WRITE from Target */
	nic_mapping->map.dma_dir = DMA_FROM_DEVICE;
	nic_mapping->map.pages = disk->db.dirty_bits_mem.pages;
	nic_mapping->map.n_pages = disk->db.dirty_bits_mem.n_pages;
	nic_mapping->nic_dev = nic->nic_dev;
	nic_mapping->map.pd = nic->nic_dev->dev->pd;
	nic_mapping->map.access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_ATOMIC;
	nic_mapping->map.ioaddr = 0;
	rv = nvmeib_mem_alloc_n_map(&nic_mapping->map);
	if (rv < 0) {
		_NE(error_1_disk_map_ec_dirty_bits_to_nic, "memory map allocation failed rv=@RV", rv);
		rv = -ENOMEM;
		goto err;
	}

	nic_mapping->ioaddr = nic_mapping->map.ioaddr;
	nic_mapping->length = nic_mapping->map.n_pages << PAGE_SHIFT;
	nic_mapping->rkey = nic_mapping->map.rkey;
	_NT(trace_1_disk_map_ec_dirty_bits_to_nic, "ret_rkey=@RET_RKEY, pd=@PD, access=@ACCESS, device_name=@DEVICE_NAME",
		nic_mapping->rkey, nic_mapping->map.pd, nic_mapping->map.access_flags,
		nic_mapping->map.pd->device->name);

	list_add_tail(&nic_mapping->link, &disk->db.dirty_bits_mappings);
	goto out;
err:
	kfree(nic_mapping);
out:
	NFOUT;
	return rv;
}


static void unmap_disk_ec_dirty_bits(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_dirty_bits_mapping *mapping, *t_mapping;
	NFIN;

	list_for_each_entry_safe(mapping, t_mapping, &disk->db.dirty_bits_mappings, link) {
		_ND(unmap_disk_ec_dirty_bits_d1,
			"unmapping disk's ec dirty bits memory @PTR @PTR", disk, mapping);
		nvmeib_mem_unmapn_n_free(&mapping->map);
		list_del(&mapping->link);
		kfree(mapping);
	}


	NFOUT;
}

static inline size_t calc_dirty_bits_mem_alloc_sz(struct nvmeibc_disk *disk, int actual_allocated_pages)
{
	return (sizeof(*disk->db.dirty_bits_mem.pages) * disk->db.dirty_bits_mem.n_pages) + (actual_allocated_pages * PAGE_SIZE);
}

static int alloc_dirty_bits_mem(struct nvmeibc_disk *disk)
{
	int rv = 0;
	int i, allocated_pages = 0;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_ALLOC_DIRTY_BITS_MEM);
	NFIN;
	if (!list_empty(&disk->db.dirty_bits_mappings)) {
		_NE(error_disk_alloc_dirty_bits_mem, "Starting rediscovery but dirty_bits_mappings list is not empty");
		unmap_disk_ec_dirty_bits(disk);

	}
	if (!list_empty(&disk->db.dirty_bits_pending_reqs)) {
		_NE(error_1_disk_alloc_dirty_bits_mem, "Starting rediscovery but dirty_bits_pending_reqs "
		   "list is not empty");
		cancel_queued_get_ec_db_reqs(disk);
	}
	disk->db.dirty_bits_stopping = 0;
	_NT(trace_disk_alloc_dirty_bits_mem, "allocating ec dirty bit pages for @DISK", disk);
	disk->db.dirty_bits_mem.n_pages = NVMEIBC_DIRTY_BITS_PAGES;
	disk->db.dirty_bits_mem.pages = kcalloc(disk->db.dirty_bits_mem.n_pages, sizeof(*disk->db.dirty_bits_mem.pages), GFP_KERNEL);
	if (!disk->db.dirty_bits_mem.pages) {
		_NE(error_2_disk_alloc_dirty_bits_mem, "dirty bits memory allocation error");
		rv = -ENOMEM;
		nvmesh_memmgr_metric_on_alloc_update(dirty_bits_mem, calc_dirty_bits_mem_alloc_sz(disk, 0), false /* success */);
		goto err_free;
	}
	for (i = 0; i < disk->db.dirty_bits_mem.n_pages; ++i, ++allocated_pages) {
		disk->db.dirty_bits_mem.pages[i] = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
		if (disk->db.dirty_bits_mem.pages[i] == NULL) {
			_NE(alloc_dirty_bits_mem_e1,
				"could not allocate dirty bits memory");
			rv = -ENOMEM;
			nvmesh_memmgr_metric_on_alloc_update(dirty_bits_mem, calc_dirty_bits_mem_alloc_sz(disk, allocated_pages), false /* success */);
			goto err_free_pages;
		}
	}

	nvmesh_memmgr_metric_on_alloc_update(dirty_bits_mem, calc_dirty_bits_mem_alloc_sz(disk, disk->db.dirty_bits_mem.n_pages), true /* success */);

	if ((rv = sg_alloc_table_from_pages(&disk->db.dirty_bits_mem.sgt,
				disk->db.dirty_bits_mem.pages,
				disk->db.dirty_bits_mem.n_pages,
				0,  (disk->db.dirty_bits_mem.n_pages << PAGE_SHIFT), GFP_KERNEL)) < 0) {
		_NE(alloc_dirty_bits_mem_e2, "OOM");
		rv = -ENOMEM;
		goto err_free_pages;
	}
	if (!(disk->db.virt = vmap(disk->db.dirty_bits_mem.pages, disk->db.dirty_bits_mem.n_pages, VM_MAP, PAGE_KERNEL))) {
		_NE(error_3_disk_alloc_dirty_bits_mem, "could not map dirty bits memory");
		rv = -ENOMEM;
		goto err_free_sg_tbl;
	}
	disk->db.len = disk->db.dirty_bits_mem.n_pages << PAGE_SHIFT;

	goto out;

err_free_sg_tbl:
	sg_free_table(&disk->db.dirty_bits_mem.sgt);
err_free_pages:
	for (i--; i >= 0; i--)
		__free_page(disk->db.dirty_bits_mem.pages[i]);
err_free:
	if (disk->db.dirty_bits_mem.pages) {
		nvmesh_memmgr_metric_on_free_update(dirty_bits_mem, calc_dirty_bits_mem_alloc_sz(disk, allocated_pages));
		kfree(disk->db.dirty_bits_mem.pages);
		disk->db.dirty_bits_mem.pages = NULL;
	}
	
out:
	_NT(trace_1_disk_alloc_dirty_bits_mem, "rv=@RV", rv);

	DD_STG_END(disk, DD_STG_ALLOC_DIRTY_BITS_MEM, (rv ? -1: 0) , NVMEIBC_DISK_DISCOVER_DBITS_MEM_ALLOCATION_FAILED);
	NFOUT;
	return rv;
}


static void free_dirty_bits_mem(struct nvmeibc_disk *disk)
{
	int i;

	NFIN;
	unmap_disk_ec_dirty_bits(disk);

	if (disk->db.virt) {
		vunmap(disk->db.virt);
		disk->db.virt = NULL;
	}
	sg_free_table(&disk->db.dirty_bits_mem.sgt);
	if (disk->db.dirty_bits_mem.pages) {
		for (i = 0; i < disk->db.dirty_bits_mem.n_pages; ++i) {
			if (disk->db.dirty_bits_mem.pages[i]) {
				__free_page(disk->db.dirty_bits_mem.pages[i]);
			}
		}
	}

	if (disk->db.dirty_bits_mem.pages) {
		nvmesh_memmgr_metric_on_free_update(dirty_bits_mem, calc_dirty_bits_mem_alloc_sz(disk, disk->db.dirty_bits_mem.n_pages));
		kfree(disk->db.dirty_bits_mem.pages);
		disk->db.dirty_bits_mem.pages = NULL;
	}
	NFOUT;
}

static void disk_coremask_update_main_work(struct workqe_struct *work);
static void disk_coremask_update_admin_work(struct workqe_struct *work);

static struct nvmeibc_disk_coremask_info *alloc_coremask_info(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_coremask_info *coremask_info;
	size_t update_masks_scratch_sz = DIV_ROUND_UP(sizeof(struct nvmeib_cpu_mask_info) * NVMEIB_CPU_MASK_MAX_CPUS, PAGE_SIZE) << PAGE_SHIFT;
	int cpu;
	
	__NFIND;
	if (!(coremask_info = kzalloc(sizeof(*coremask_info), GFP_KERNEL)) ||
		!(coremask_info->update_masks_scratch = (void *)__get_free_pages(GFP_KERNEL, get_order(update_masks_scratch_sz))) ||
		!(coremask_info->pcpu_stats = nvmeib_public_alloc_percpu_cacheline(struct nvmeibc_disk_coremask_pcpu_stats)))
	{
		_NW(trace_nvmeibc_disk_alloc_coremask_info_oom, "OOM");
		goto err;
	}
	for_each_possible_cpu(cpu) {
		struct nvmeibc_disk_coremask_pcpu_stats __percpu *pcpu_ptr = per_cpu_ptr(
			coremask_info->pcpu_stats, cpu);
		memset(pcpu_ptr, 0, sizeof(*pcpu_ptr));
	}
	coremask_info->disk = disk;
	INIT_LIST_HEAD(&coremask_info->coremask_chs);
	rwlock_init(&coremask_info->coremask_chs_lock);
	coremask_info->last_coremask_update_count = 0;
	coremask_info->update_masks_scratch_sz = update_masks_scratch_sz;
	WQ_INIT_WORK(&coremask_info->update_masks_main_work, disk_coremask_update_main_work);
	WQ_INIT_WORK(&coremask_info->update_masks_admin_work, disk_coremask_update_admin_work);
	mutex_init(&coremask_info->masks_guard);
	INIT_RADIX_TREE(&coremask_info->masks_tree, GFP_KERNEL);
	goto out;
	
err:
	if (coremask_info) {
		if (coremask_info->pcpu_stats)
			nvmeib_public_free_percpu(coremask_info->pcpu_stats);
		if (coremask_info->update_masks_scratch)
			__free_pages(coremask_info->update_masks_scratch, get_order(update_masks_scratch_sz));
		kfree(coremask_info);
		coremask_info = NULL;
	}
	
out:
	__NFOUTD;
	return coremask_info;
}

static void free_coremask_info(struct nvmeibc_disk_coremask_info *cinfo)
{
	if (cinfo) {
		const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(cinfo->disk);
		struct nvmeib_cpu_mask_info_node *masks_info_node;
		struct radix_tree_iter masks_iter;
		void **slot;

		nvmeibc_cancel_work(nvmeibc_isnt_params_core2main(p), &cinfo->update_masks_main_work);

		radix_tree_for_each_slot(slot, &cinfo->masks_tree, &masks_iter, 0) {
			masks_info_node = *slot;
			radix_tree_iter_delete(&cinfo->masks_tree, &masks_iter, slot);
			kfree(masks_info_node);
		}

		if (cinfo->pcpu_stats)
			nvmeib_public_free_percpu(cinfo->pcpu_stats);

		if (cinfo->update_masks_scratch) {
			__free_pages(cinfo->update_masks_scratch, 
				     get_order(cinfo->update_masks_scratch_sz));
		}

		kfree(cinfo);
	}
}

struct nvmeibc_disk_coremask_pcpu_stats __percpu *nvmeibc_disk_get_coremask_stats_this_cpu(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_coremask_pcpu_stats __percpu *ret = NULL;

	__NFIND;

	if (!disk->info) {
		goto out;
	}

	if (!disk->info->coremask_info) {
		goto out;
	}

	ret = this_cpu_ptr(disk->info->coremask_info->pcpu_stats);
out:
	__NFOUTD;
	return ret;
}

static void __notify_coremask_disk_update_done_cb(void *ctx)
{
	struct nvmeibc_disk_update_data *disk_update_data = ctx;
	kfree(disk_update_data);
}

int nvmeibc_disk_notify_coremask_update(struct nvmeibc_idisk *disk_base)
{
	struct nvmeibc_disk_update_data *disk_update_data;
	struct nvmeibc_disk* disk = nvmeibc_disk_from_base(disk_base);
	int rv;

	__NFIND;
	/* Called by nvmeibc_volume_call_for_all_vol_disks() under volumes spinlock. Cannot sleep */
	
	if (disk->access_local) {
		_NW_dmesg(warn_c_disk_notify_coremask_update_local,
		    "Disk @DISK_ID_STR is local access. Coremask not supported", disk->name);
		/* We still want the other disks to be notified, so return 0 to continue the disks loop.
		 * 
		 */
		rv = 0;
		goto out;
	}
	
	if (!(disk_update_data = kzalloc(sizeof(*disk_update_data), GFP_ATOMIC))) {
		_NE(err_disk_notify_coremask_update_oom, "OOM");
		rv = -ENOMEM;
		goto out;
	}

	disk_update_data->update_type = DISK_UPDATE_COREMASK_UPDATE;
	disk_update_data->done_cb = __notify_coremask_disk_update_done_cb;
	disk_update_data->done_cb_ctx = disk_update_data;

	if ((rv = nvmeibc_disk_update_config(disk, disk_update_data, true)) < 0) {
		kfree(disk_update_data);
	}
out:
	__NFOUTD;
	return rv;
}

int nvmeibc_disk_create_remote(struct nvmeibc_ib_admin_channel *ach,
	struct nvmeibc_disk *disk, int nsid, int sector_shift, int p, int n, int m,
	u64 lock_counter, int n_disk_lock_segments, __be64 *rsrc_guids, bool is_rediscover)
{
	struct nvmeibc_disk_info *info = NULL;
	struct rsc_info *rscs = NULL;
	struct hca_info *hcaa = NULL;
	struct lock_seg_info *lsi;
	struct nvmeibc_disk_channel_rsc *channel_rscs;
	struct workqueue_struct *pcpu_wq;
	struct nvmeibc_disk_coremask_info *cinfo;
	int i, j, rv = 0;

	__NFIND;
	/* we can get into this function also for already allocated disk */
	if (!disk->info) {
		/* alloce remote diskinfo */
		info = kzalloc(sizeof(*info), GFP_KERNEL);
		hcaa = kzalloc(sizeof(*hcaa) * p, GFP_KERNEL);
		rscs = n ? kzalloc(sizeof(*rscs) * n, GFP_KERNEL) : NULL;
		pcpu_wq = disk->pcpu_nrchs_ll ? alloc_workqueue("pcpu_wq", 0, 0) : NULL;
		cinfo = disk->coremask_support ? alloc_coremask_info(disk) : NULL;
		if (!(info && hcaa && (!n || rscs) && (!disk->pcpu_nrchs_ll || pcpu_wq) && (!disk->coremask_support || cinfo))) {
			_NE(error_disk_nvmeibc_disk_create_remote, "Failed to allocate disk info");
			rv = -ENOMEM;
			goto no_mem;
		}
		for (i = 0; i < n; ++i) {
			rscs[i].id = i;
			INIT_LIST_HEAD(&rscs[i].link);
		}
		for (i = 0; i < p; ++i) {
			lsi =  kzalloc(sizeof(*lsi) * n_disk_lock_segments, GFP_KERNEL);
			channel_rscs =
				n ? kzalloc(n * sizeof(*channel_rscs), GFP_KERNEL) : NULL;
			if (!(lsi && (!n || channel_rscs))) {
				_NE(error_1_disk_nvmeibc_disk_create_remote, "Failed to allocate disk hca info");
				kfree(lsi);
				kfree(channel_rscs);
				rv = -ENOMEM;
				goto no_mem;
			}
			for (j = 0; j < n; ++j)
				channel_rscs[j].set_n = i;
			hcaa[i].channel_rscs = channel_rscs;
			hcaa[i].lsi = lsi;
			if (rsrc_guids)
				hcaa[i].node_guid = rsrc_guids[i];
		}
		info->disk = disk;
		info->n_rscs_sets = p;
		info->n_disk_lock_segments = n_disk_lock_segments;
		info->n_rscs = n;
		info->hcaa = hcaa;
		info->rscs = rscs;
		INIT_LIST_HEAD(&info->my_rscs);
		plist_head_init(&info->available_norddas);
		INIT_LIST_HEAD(&info->available_channels);
		INIT_LIST_HEAD(&info->free_rscs);
		for (i = 0; i < DISK_PEND_PRIO_MAX; i++)
			INIT_LIST_HEAD(&info->pending_disk_cmds[i]);
		pcpu_nrch_init_pool(info);
		info->pcpu_wq = pcpu_wq;
		info->coremask_info = cinfo;
		info->cntr_page_size = ach->base.cntr_page_size;
		info->nsid = nsid;
		info->sector_shift = sector_shift;
		info->max_client_rscs = m;
		info->ch = &ach->base;
		down_write(&info->ch->segments_locks_remote.guard);
		_NT(trace_0_disk_nvmeibc_disk_create_remote,
			"Disk @DISK_NAME (remote), info=@PTR, ch=@PTR, segments_locks_remote=@PTR",
			disk->name, info, info->ch, &info->ch->segments_locks_remote);
		info->ch->segments_locks_remote.disk = disk;
		info->ch->segments_locks_remote.lock_ch = NULL;
		info->ch->segments_locks_remote.is_local = false;
		up_write(&info->ch->segments_locks_remote.guard);
		_NT(trace_1_disk_nvmeibc_disk_create_remote, "Disk @DISK_NAME (remote), set info=@PTR", disk->name, info);
		disk->info = info;
	}
	if (!is_rediscover) {
		disk->info->lock_id = lock_counter;
	}
	goto out;

no_mem:
	kfree(cinfo);
	if (pcpu_wq)
		destroy_workqueue(pcpu_wq);
	if (rscs)
		kfree(rscs);
	if (hcaa) {
		for (i = 0; i < p; ++i) {
			kfree(hcaa[i].channel_rscs);
			kfree(hcaa[i].lsi);
		}
		kfree(hcaa);
	}
	if (info && info->ch) {
		down_write(&info->ch->segments_locks_remote.guard);
		if (info->ch->segments_locks_remote.num_of_segments > 0) {
			if (info->ch->segments_locks_remote.locks) {
				kfree(info->ch->segments_locks_remote.locks);
				info->ch->segments_locks_remote.locks = NULL;
			}
			info->ch->segments_locks_remote.disk = NULL;
		}
		up_write(&info->ch->segments_locks_remote.guard);
		kfree(info);
	}

out:
	__NFOUTD;
	return rv;
}

int nvmeibc_disk_add_rsc(struct nvmeibc_ib_admin_channel *ach,
	struct nvmeibc_disk *disk, struct nvmeibs_disk_description *d,
	int j, int p)
{
	struct nvmeibc_disk_channel_rsc *r;
	int rv = 0;

	__NFIND;
	if ((unsigned)j >= disk->info->n_rscs) {
		_NE(error_disk_nvmeibc_disk_add_rsc, "Invalid index: received @CHANNEL, resource array size @N_RSCS",
			j, disk->info->n_rscs);
		rv = -1;
		goto out;
	}
	r = &disk->info->hcaa[p].channel_rscs[j];
	/* check if alreay initialized */
	if (r->info)
		goto out;
	r->info = disk->info;
	init_disk_rsc(j, r, d);

	if (r->sq_n_entries > ((1 << NVMEIBC_DISK_RSC_CMDID_BITS_CNT) -1)) {
		_NE(error_0_disk_nvmeibc_disk_add_rsc,
			"@CHANNEL, sq_n_entries=@INT would overflow", j, r->sq_n_entries);
		rv = -EOVERFLOW;
		goto out;
	}
	if ((rv = alloc_disk_rsc(ach, j, r, disk->info->hcaa[p].lsi)) < 0) {
		_NE(error_1_disk_nvmeibc_disk_add_rsc, "Fail to allocate disk resource for channel @CHANNEL", j);
		goto out;
	}
	else {
		disk->max_rdda_bb = r->mem_size - disk->md_size;
		_ND(trace_disk_nvmeibc_disk_add_rsc, "Set max RDDA BB size to @MAX_RDDA_BB", disk->max_rdda_bb);
	}

out:
	__NFOUTD;
	return rv;
}

int nvmeibc_disk_add_lock_rsc(struct nvmeibc_ib_admin_channel *ach,
	struct nvmeibc_disk *disk, struct nvmeibs_disk_seg_lock_info *e,
	int j, int p)
{
	struct lock_seg_info *r;
	int rv = 0;

	__NFIND;
	if ((unsigned)j >= disk->info->n_disk_lock_segments) {
		_NE(error_disk_nvmeibc_disk_add_lock_rsc, "Invalid index: received @CHANNEL, resource array size @N_DISK_LOCK_SEGMENTS",
			j, disk->info->n_disk_lock_segments);
		rv = -1;
		goto out;
	}
	r = &disk->info->hcaa[p].lsi[j];
	r->seg_id = (int)be64_to_cpu(e->seg_id);
	r->start_disk_address = be64_to_cpu(e->start_disk_address);
	r->len = be64_to_cpu(e->len);
	r->lockset_size = be32_to_cpu(e->lockset_size);
	r->ioaddr = be64_to_cpu(e->ioaddr);
	r->lkey = be32_to_cpu(e->lkey);
	r->rkey = be32_to_cpu(e->rkey);
	r->lmi = e->lmi;
	_ND(trace_disk_nvmeibc_disk_add_lock_rsc, "seg_id=@SEG_ID_INT", r->seg_id);
	_ND(trace_1_disk_nvmeibc_disk_add_lock_rsc, "start_disk_address=@START_DISK_ADDRESS", r->start_disk_address);
	_ND(trace_2_disk_nvmeibc_disk_add_lock_rsc, "len=@LEN_LLONG", r->len);
	_ND(trace_3_disk_nvmeibc_disk_add_lock_rsc, "ioaddr=@IOADDR", r->ioaddr);
	_ND(trace_4_disk_nvmeibc_disk_add_lock_rsc, "lockset_size=@LOCKSET_SIZE", r->lockset_size);
	_ND(trace_5_disk_nvmeibc_disk_add_lock_rsc, "lkey=@LKEY", r->lkey);
	_ND(trace_6_disk_nvmeibc_disk_add_lock_rsc, "rkey=@RKEY", r->rkey);
	_ND(trace_7_disk_nvmeibc_disk_add_lock_rsc, "lmi=@LMI", (void*)(r->lmi));

out:
	__NFOUTD;
	return rv;
}

static void take_next_disk_config(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic, *t_arnic;
	__NFIND;
	_NT(trace_disk_take_next_disk_config, "Check if new configuration needed for disk @DISK_NAME @NEW_CONFIG_READY",
		disk->name, disk->new_config_ready);
	if (disk->new_config_ready) {
		spin_lock(&disk->disk_conf_spinlock);
		_ND(trace_1_disk_take_next_disk_config, "Configuration taken");
		if (disk->new_config_ready) {
			_NT(trace_2_disk_take_next_disk_config, "Old node id @CONFIG_NODE_ID new node id: @NEXT_CONFIG_NODE_ID", disk->config_node_id,
				disk->next_config_node_id);
			strlcpy(disk->config_node_id, disk->next_config_node_id,
				sizeof(disk->config_node_id));
			list_for_each_entry_safe(arnic, t_arnic, &disk->arnics, link) {
				list_del(&arnic->link);
				kfree(arnic);
			}
			list_splice_init(&disk->next_arnics, &disk->arnics);
			disk->next_config_node_id[0] = '\0';
			disk->new_config_ready = false;
		}
		spin_unlock(&disk->disk_conf_spinlock);
	}
	__NFOUTD;
}

static inline void disk_proc_destroy(struct nvmeibc_disk *disk)
{
	int i;
	__NFIND;
	if (disk->proc_ent_stats) {
		nvmeib_public_proc_remove(disk->proc_ent_stats);
		disk->proc_ent_stats = NULL;
	}
	if (disk->proc_ent_stats_json) {
		nvmeib_public_proc_remove(disk->proc_ent_stats_json);
		disk->proc_ent_stats_json = NULL;
	}
	if (disk->proc_ent_status) {
		nvmeib_public_proc_remove(disk->proc_ent_status);
		disk->proc_ent_status = NULL;
	}
	if (disk->proc_ent_nrch_status) {
		nvmeib_public_proc_remove(disk->proc_ent_nrch_status);
		disk->proc_ent_nrch_status = NULL;
	}
	if (disk->proc_ent_ioch_status) {
		nvmeib_public_proc_remove(disk->proc_ent_ioch_status);
		disk->proc_ent_ioch_status = NULL;
	}
	if (disk->proc_ent_ioch_json) {
		nvmeib_public_proc_remove(disk->proc_ent_ioch_json);
		disk->proc_ent_ioch_json = NULL;
	}
	if (disk->proc_ent_lock_channels) {
		nvmeib_jdr_proc_remove(disk->proc_ent_lock_channels);
		disk->proc_ent_lock_channels = NULL;
	}
	if (disk->proc_ent_counters) {
		nvmeib_public_proc_remove(disk->proc_ent_counters);
		disk->proc_ent_counters = NULL;
	}
	if (disk->proc_ent_interrupts) {
		nvmeib_public_proc_remove(disk->proc_ent_interrupts);
		disk->proc_ent_interrupts = NULL;
	}
	if (disk->proc_ent_qps) {
		nvmeib_public_proc_remove(disk->proc_ent_qps);
		disk->proc_ent_qps = NULL;
	}
	if (disk->proc_ent_cmds) {
		nvmeib_public_proc_remove(disk->proc_ent_cmds);
		disk->proc_ent_cmds = NULL;
	}
	if (disk->unsafe_status) {
		nvmeib_public_proc_remove(disk->unsafe_status);
		disk->unsafe_status = NULL;
	}
	if (disk->proc_ent_status_json) {
		nvmeib_public_proc_remove(disk->proc_ent_status_json);
		disk->proc_ent_status_json = NULL;
	}
	if (disk->proc_ent_inj_err) {
		nvmeib_public_proc_remove(disk->proc_ent_inj_err);
		disk->proc_ent_inj_err = NULL;
	}
	if (disk->proc_ent_rediscover) {
		nvmeib_public_proc_remove(disk->proc_ent_rediscover);
		disk->proc_ent_rediscover = NULL;
	}
	if (disk->proc_ent_chstats) {
		nvmeib_public_proc_remove(disk->proc_ent_chstats);
		disk->proc_ent_chstats = NULL;
	}
#if defined(DISK_COUNT_REUSE) && DISK_COUNT_REUSE
	if (disk->proc_ent_reuse) {
		nvmeib_public_proc_remove(disk->proc_ent_reuse);
		disk->proc_ent_reuse = NULL;
	}
#endif
	if (disk->proc_core_masks_json) {
		nvmeib_public_proc_remove(disk->proc_core_masks_json);
		disk->proc_core_masks_json = NULL;
	}
	if (disk->proc_coremask_stats_json) {
		nvmeib_public_proc_remove(disk->proc_coremask_stats_json);
		disk->proc_coremask_stats_json = NULL;
	}

	if (nvmeibc_disk_pcpu_nrch_poll_proc) {
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			if (disk->pcpu_nrch_poll[i].proc) {
				proc_remove(disk->pcpu_nrch_poll[i].proc);
				disk->pcpu_nrch_poll[i].proc = NULL;
			}
		}
	}

	if (disk->proc_dir) {
		remove_proc_entry(disk->name, nvmeibc_get_proc_dir_disks(nvmeibc_cinst_get_core_p(disk)));
		disk->proc_dir = NULL;
	}
	__NFOUTD;
}

static ssize_t set_inj_err(void *arg, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = arg;
	ssize_t rv = len;
	char inj;

	if (sscanf(buf, "%c", &inj) != 1) {
		rv = -EINVAL;
		goto out;
	}

	switch (inj) {
	case 'r':
	case 'w':
	case 'j':
	case 'd':
		break;
	default:
		rv = -EINVAL;
		goto out;
	}

	disk->inj_err = inj;

out:
	return rv;
}

static ssize_t set_rediscover(void *arg, char *buf, size_t len) {
	struct nvmeibc_disk *disk = arg;
	ssize_t rv = -EINVAL;
	int rediscover;

	if (sscanf(buf, "%d", &rediscover) != 1)
		goto out;

	if (rediscover == 1)
		disk->force_pause = false;
	else if (rediscover == 2)
		disk->force_pause = true;
	else
		goto out;

	if (!(rv = nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_REDISCOVER_CALLED)))
		rv = len;

out:
	return rv;
}

static int poll_pcpu_proc_open(struct inode *inode, struct file *file)
{
	struct pcpu_nrch_poll *pcpu_nrch_poll = file_get_priv_data(file);
	atomic_inc(&pcpu_nrch_poll->open_cnt);
	return 0;
}

static int poll_pcpu_proc_close(struct inode *inode, struct file *file)
{
	struct pcpu_nrch_poll *pcpu_nrch_poll = file_get_priv_data(file);
	struct nvmeibc_disk *disk = pcpu_nrch_poll->disk;
	if (atomic_dec_return(&pcpu_nrch_poll->open_cnt) == 0) {
		/* Poller is disconnecting, so kickstart the notify if needed */
		int rv, cpu = get_cpu();
		if (pcpu_nrch_poll->index != cpu) {
			_NE(err_fill_pcpu_poll_proc_close_inv_cpu,
			    "Call from invalid @CPU, expected @CPU", cpu, pcpu_nrch_poll->index);
			put_cpu();
			return 0;
		}

		if (atomic_read(&disk->paused) || atomic_read(&disk->dying)) {
			put_cpu();
			return 0;
		}

		if (disk->access_local) {
			put_cpu();
			return 0;
		}

		if (!disk->info->pcpu_nrchs[cpu].nrch) {
			_NE(err_fill_pcpu_poll_proc_close_nrch_null,
			    "pcpu_nrch[@CPU] is not ready", cpu);
			put_cpu();
			return 0;
		}

		if ((rv = nvmeibc_ib_nordda_channel_poll_cqs(disk->info->pcpu_nrchs[cpu].nrch, true)) < 0) {
			_NE(err_fill_pcpu_poll_proc_close_nrch_fail,
			    "nvmeibc_ib_nordda_channel_poll_cqs Failed (@RV)", rv);
		}
		put_cpu();
	}
	return 0;
}

static ssize_t poll_pcpu_proc_read(struct file *file, char __user *userbuf,
				 size_t len, loff_t *offset_p)
{
	struct pcpu_nrch_poll *pcpu_nrch_poll = file_get_priv_data(file);
	struct nvmeibc_disk *disk = pcpu_nrch_poll->disk;
	int cpu = get_cpu();
	ssize_t rv;
	char buf[16];

	(void)offset_p;
	if (pcpu_nrch_poll->index != cpu) {
		_NE(err_fill_pcpu_poll_proc_read_inv_cpu,
		    "Call from invalid @CPU, expected @CPU", cpu, pcpu_nrch_poll->index);
		rv = -EINVAL;
		goto out;
	}

	if (atomic_read(&disk->paused) || atomic_read(&disk->dying)) {
		rv = -EBUSY;
		goto out;
	}

	if (disk->access_local) {
		rv = -ENOTSUPP;
		goto out;
	}

	if (!disk->info->pcpu_nrchs[cpu].nrch) {
		_NE(err_fill_pcpu_poll_proc_read_nrch_null,
		    "pcpu_nrch[@CPU] is not ready", cpu);
		rv = -ENOENT;
		goto out;
	}

	if ((rv = nvmeibc_ib_nordda_channel_poll_cqs(disk->info->pcpu_nrchs[cpu].nrch, false)) < 0) {
		_NE(err_fill_pcpu_poll_proc_read_nrch_fail,
		    "nvmeibc_ib_nordda_channel_poll_cqs Failed (@RV)", rv);
		goto out;
	}

	/* rv contains number of cqes polled, return in string */
	rv = scnprintf(buf, len, "%zd\n", rv);
	if (copy_to_user(userbuf, buf, rv))
		rv = -EFAULT;

out:
	put_cpu();
	return rv;
}

static long poll_pcpu_proc_ioctl(struct file *file, unsigned int cmd, unsigned long __arg)
{
	struct pcpu_nrch_poll *pcpu_nrch_poll = file_get_priv_data(file);
	struct nvmeibc_disk *disk = pcpu_nrch_poll->disk;
	int cpu = get_cpu();
	long rv;

	(void)cmd;
	(void)__arg;
	if (pcpu_nrch_poll->index != cpu) {
		_NE(err_fill_pcpu_poll_proc_ioctl_inv_cpu,
		    "Call from invalid @CPU, expected @CPU", cpu, pcpu_nrch_poll->index);
		rv = -EINVAL;
		goto out;
	}

	if (atomic_read(&disk->paused) || atomic_read(&disk->dying)) {
		rv = -EBUSY;
		goto out;
	}

	if (disk->access_local) {
		rv = -ENOTSUPP;
		goto out;
	}

	if (!disk->info->pcpu_nrchs[cpu].nrch) {
		_NE(err_fill_pcpu_poll_proc_ioctl_nrch_null,
		    "pcpu_nrch[@CPU] is not ready", cpu);
		rv = -ENOENT;
		goto out;
	}

	if ((rv = nvmeibc_ib_nordda_channel_poll_cqs(disk->info->pcpu_nrchs[cpu].nrch, false)) < 0) {
		_NE(err_fill_pcpu_poll_proc_ioctl_nrch_fail,
		    "nvmeibc_ib_nordda_channel_poll_cqs Failed (@RV)", rv);
		goto out;
	}

out:
	put_cpu();
	return rv;
}

#if !KS_HAS_PROC_FS
static struct file_operations poll_pcpu_proc_ops = {
	.open			= poll_pcpu_proc_open,
	.read 			= poll_pcpu_proc_read,
	.unlocked_ioctl		= poll_pcpu_proc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= poll_pcpu_proc_ioctl,
#endif
	.release		= poll_pcpu_proc_close
};
#else
static struct proc_ops poll_pcpu_proc_ops = {
	.proc_open		= poll_pcpu_proc_open,
	.proc_read		= poll_pcpu_proc_read,
	.proc_ioctl		= poll_pcpu_proc_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl	= poll_pcpu_proc_ioctl,
#endif
	.proc_release		= poll_pcpu_proc_close
};
#endif


static void lock_channels_proc_fill(struct jdr *jdr, void *arg);
static ssize_t lock_channels_proc_write(void *arg, const char __user *buf, size_t count, loff_t *ppos);

static inline int disk_proc_create(struct nvmeibc_disk *disk)
{
	struct proc_dir_entry *dir = nvmeibc_get_proc_dir_disks(nvmeibc_cinst_get_core_p(disk));
	int rv = -1, i;

	__NFIND;
	if (!dir) {
		_NE(error_disk_disk_proc_create, "Oops, nvmeibc_disks_proc_dir NULL");
		goto out;
	}
	if (!(disk->proc_dir = proc_mkdir(disk->name, dir))) {
		_NE(error_1_disk_disk_proc_create, "Fail to create disk @DISK_NAME proc dir", disk->name);
		goto out;
	}
	if (!(disk->proc_ent_stats = nvmeib_public_proc_create(
		"iostats", disk->proc_dir, stats_fill_buf, NULL, disk))) {
		_NE(error_2_disk_disk_proc_create, "Fail to create disk @DISK_NAME stats proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_stats_json = nvmeib_public_proc_create(
		"iostats.json", disk->proc_dir, stats_fill_buf_json, stats_clear, disk))) {
		_NE(error_3_disk_disk_proc_create, "Fail to create disk @DISK_NAME stats proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_status = nvmeib_public_proc_create(
		"status", disk->proc_dir, status_fill_buf, NULL, disk))) {
		_NE(error_4_disk_disk_proc_create, "Fail to create disk @DISK_NAME status proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_nrch_status = nvmeib_public_proc_create(
		"nrch", disk->proc_dir, nrch_fill_buf, NULL, disk))) {
		_NE(error_disk_disk_proc_create_nrch, "Fail to create disk @DISK_NAME nrch proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_ioch_status = nvmeib_public_proc_create(
		"ioch", disk->proc_dir, ioch_fill_buf, NULL, disk))) {
		_NE(error_disk_disk_proc_create_ioch, "Fail to create disk @DISK_NAME ioch proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_counters = nvmeib_public_proc_create(
		"counters", disk->proc_dir, disk_counters_fill_buf, disk_counters_reset, disk))) {
		_NE(error_5_disk_disk_proc_create, "Fail to create disk @DISK_NAME counters proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_ioch_json = nvmeib_public_proc_create(
		"io_channels.json", disk->proc_dir, io_ch_fill_buf_json, NULL, disk))) {
		_NE(error_disk_disk_proc_create_ioch_json, "Fail to create disk @DISK_NAME io_ch_json proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_lock_channels = nvmeib_jdr_proc_create("lock_channels", disk->proc_dir,
		lock_channels_proc_fill, lock_channels_proc_write, disk))) {
		_NE(error_disk_disk_proc_create_lock_channels, "Fail to create disk @DISK_NAME lock_channels proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_interrupts = nvmeib_public_proc_create(
		"interrupts", disk->proc_dir, disk_interrupts_fill_buf, disk_interrupts_reset, disk))) {
		_NE(error_6_disk_disk_proc_create, "Fail to create disk @DISK_NAME interrupts proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_qps = nvmeib_public_proc_create(
		"qps", disk->proc_dir, disk_qps_fill_buf, disk_qps_reset, disk))) {
		_NE(error_14_disk_disk_proc_create, "Fail to create disk @DISK_NAME qps proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_cmds = nvmeib_public_proc_create(
		"cmds", disk->proc_dir, disk_cmds_fill_buf, NULL, disk))) {
		/* not implementing reset-func in order to ensure coherency with disk's pcpu in_transfer */
		_NE(error_13_disk_disk_proc_create, "Fail to create disk @DISK_NAME cmds proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->unsafe_status = nvmeib_public_proc_create(
		".unsafe_status", disk->proc_dir, unsafe_status_fill_buf, NULL, disk))) {
		_NE(error_7_disk_disk_proc_create, "Fail to create disk @DISK_NAME unsafe_debug proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_status_json = nvmeib_public_proc_create(
		"status.json", disk->proc_dir, status_json_fill_buf, NULL, disk))) {
		_NE(error_8_disk_disk_proc_create, "Fail to create disk @DISK_NAME status json proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_inj_err = nvmeib_public_proc_create(
		"inj_err", disk->proc_dir, NULL, set_inj_err, disk))) {
		_NE(error_9_disk_disk_proc_create, "Fail to create disk @DISK_NAME error injection proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_ent_rediscover = nvmeib_public_proc_create(
		"rediscover", disk->proc_dir, NULL, set_rediscover, disk))) {
		_NE(error_10_disk_disk_proc_create, "Fail to create disk @DISK_NAME rediscover proc entry\n", disk->name);
		goto destroy;
	}
#ifdef DEBUG_CLNT_NET_STATS
	if (!(disk->proc_ent_chstats = nvmeib_public_proc_create(
		"chstats", disk->proc_dir, chstats_fill_buf, NULL, disk))) {
		_NE(error_11_disk_disk_proc_create,
			"Fail to create disk @STR chstats proc entry", disk->name);
		goto destroy;
	}
	disk->chstats_rd = 0;
#endif /* DEBUG_CLNT_NET_STATS */
#if defined(DISK_COUNT_REUSE) && DISK_COUNT_REUSE
	if (!(disk->proc_ent_reuse = nvmeib_public_proc_create(
		"ch_reuse", disk->proc_dir, reuse_fill_buf, NULL, disk))) {
		_NE(error_12_disk_disk_proc_create,
			"Fail to create disk @STR channel reuse proc entry", disk->name);
		goto destroy;
	}
#endif
	if (!(disk->proc_core_masks_json = nvmeib_public_proc_create(
		"core_masks.json", disk->proc_dir, core_masks_fill_buf, NULL, disk))) {
		_NE(error_16_disk_disk_proc_create,
		    "Fail to create disk @STR core_masks.json proc entry", disk->name);
		goto destroy;
	}
	if (!(disk->proc_coremask_stats_json = nvmeib_public_proc_create(
		"coremask_stats.json", disk->proc_dir, coremask_stats_fill_buf, coremask_stats_reset, disk))) {
		_NE(error_17_disk_disk_proc_create,
		    "Fail to create disk @STR coremask_stats.json proc entry", disk->name);
		goto destroy;
	}
	if (nvmeibc_disk_pcpu_nrch_poll_proc) {
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			char proc_fname[32];
			disk->pcpu_nrch_poll[i].index = i;
			disk->pcpu_nrch_poll[i].disk = disk;
			atomic_set(&disk->pcpu_nrch_poll[i].open_cnt, 0);
			snprintf(proc_fname, sizeof(proc_fname), "pcpu_nrch_poll%d", i);
			if (!(disk->pcpu_nrch_poll[i].proc = proc_create_data(proc_fname, 0700,
				disk->proc_dir, &poll_pcpu_proc_ops, &disk->pcpu_nrch_poll[i]))) {
				_NE(error_15_disk_disk_proc_create,
				"Fail to create disk @STR channel pcpu_nrch_poll proc entry @INDEX", disk->name, i);
				goto destroy;
			}
		}
	}

	rv = 0;
	goto out;

destroy:
	disk_proc_destroy(disk);

out:
	__NFOUTD;
	return rv;
}

static void disk_trace_verb_counters_fn(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx);
static void nvmeibc_disk_net_intrs_stats_delete(struct nvmeibc_disk *disk);
static void nvmeibc_disk_free(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_local_nic *lnic;

	__NFIND;
	take_next_disk_config(disk);
	while ((arnic = list_first_entry_or_null(
		&disk->arnics, struct nvmeibc_admin_rnic, link))) {
		list_del(&arnic->link);
		kfree(arnic);
	}
	while ((lnic = list_first_entry_or_null(
		&disk->local_nics, struct nvmeibc_local_nic, link))) {
		free_disk_lnic(lnic);
		list_del(&lnic->link);
		kfree(lnic);
	}
	// verify no volume is using the disk - the deletion of ranges should be when volume is removed.
	BUG_ON(!list_empty(&disk->volumes));
	BUG_ON(!list_empty(&disk->db.dirty_bits_pending_reqs));
	nvmeib_io_stats_trace(disk->stats, disk_trace_verb_counters_fn, disk); /* forced trace before removal */
	cancel_delayed_work_sync(&disk->periodic_lock_channel_work);
	 /* called before destroying disk-wq (and disk-obj) but safe to call here as well */
	disk_proc_destroy(disk);
	nvmeib_io_stats_free(disk->stats);
	wq_destroy(disk->remove_wq);
	nvmeibc_disk_toma_free(disk);
	nvmeib_public_free_percpu(disk->percpu);
	nvmeib_public_free_percpu(disk->pcpu_cmds_stats);
	free_cpumask_var(disk->pcpu_nrchs_ll_cpumask);
	kfree(disk->local.jrnl.jmdc);
	nvmeibc_disk_net_intrs_stats_delete(disk);
	kfree(disk);
	NFOUT;
}

static bool use_lionic_path_for_ioch(struct nvmeibc_io_lnic *lionic)
{
	struct nvmeibc_disk *disk = lionic->disk;
	int max_ioch_path_fails = nvmeibc_max_ioch_path_fails;
	bool ret = true;

	if (disk->start_io_work_preempted_seqcnt) {
		_NT(t0_disk_use_lionic_path_for_ioch, "Continue with counters from old round as we preemption flag is set");
	}

	if (lionic->start_ioch_path_fail_ctr >= max_ioch_path_fails ||
		lionic->start_ioch_attempt_ctr > max_ioch_start_route) {
		_NT(trace_1_disk_use_lionic_path_for_ioch, "start_ioch attempt @START_IOCH_ATTEMPT_ID - lionic stats @LIONIC (@SGID -> @DGID) fail_ctr: @START_IOCH_PATH_FAIL_CTR/@MAX_IOCH_PATH_FAILS attempt_ctr: @START_IOCH_ATTEMPT_CTR/@MAX_IOCH_START_ROUTE",
			lionic->start_ioch_attempt_id, lionic, &lionic->path.sgid, &lionic->path.dgid,
			lionic->start_ioch_path_fail_ctr, max_ioch_path_fails,
			lionic->start_ioch_attempt_ctr, max_ioch_start_route);
			ret = false;
	}
	return ret;
}

static void nvmeibc_dma_unmap(struct nvmeibc_disk_channel_rsc *r)
{
	struct ib_device *ib;

	if (!r->nv)
		return;

	ib = r->nv->ib_dev;

	if (r->prp1_size) {
		/* JH IOMMU: Changed to DMA_TO_DEVICE - (Not currently in use, but would be used as a src for RDMA_WRITE if so) */
		ib_dma_unmap_single(ib, r->prp1_shadow_dma, r->prp1_size,
				    DMA_TO_DEVICE);
	}
	/* JH IOMMU: DMA_TO_DEVICE is correct - src for RDMA_WRITE only */
	ib_dma_unmap_single(ib, r->sq_shadow_dma, r->sq_size, DMA_TO_DEVICE);
	/* JH IOMMU: DMA_FROM_DEVICE is correct. Used as a sink either for Remote RDMA_WRITE (Normal Flow) or for Local RDMA_READ (OE) */
	ib_dma_unmap_single(ib, r->cq_shadow_dma, r->cq_size, DMA_FROM_DEVICE);
	/* JH IOMMU: DMA_TO_DEVICE is correct - src for RDMA_WRITE only */
	ib_dma_unmap_single(ib, r->sq_db_sge.addr, sizeof(r->sq_db_value), DMA_TO_DEVICE);
	ib_dma_unmap_single(ib, r->cq_db_sge.addr, sizeof(r->cq_db_value), DMA_TO_DEVICE);
	ib_dma_unmap_single(ib, r->msix_sge.addr, sizeof(r->msi_x_payload), DMA_TO_DEVICE);
	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as sink for Remote RDMA_WRITE during normal IO, and sink for Client RDMA_READ during OE */
	ib_dma_unmap_single(ib, r->rl_riu.raddr, sizeof(r->read_lock_buffer), DMA_FROM_DEVICE);
	nvmeibc_ib_net_jmdc_pb_unmap(r->nv, &r->jmdc_pb);

	r->nv = NULL;
}

static int __attribute__((unused)) set_local_keys(struct nvmeibc_disk_channel_rsc *r,
	struct nvmeib_dev *nv)
{
	struct ib_device *ib;
	int ret = -ENOMEM;

	NFIN;
	nvmeibc_dma_unmap(r);

	r->nv = nv;
	ib = r->nv->ib_dev;

#define DMA_MAP(label, pkernel, size, dir) ({\
	u64 dma; \
	int err; \
	dma = ib_dma_map_single(ib, pkernel, size, dir);\
	err = ib_dma_mapping_error(ib, dma); \
	if (err) { \
			_NE(error_DMA_MAP_ ##label, "IB DMA Mapping (" #pkernel ") failed\n"); \
			goto failed_ ##label; \
		}\
	dma; \
})

	/* the remote prp1 local shadow */
	if (r->md_size)
		r->md_sge.lkey = nvmeib_get_lkey(nv);
	if (r->prp1_size) {
		/* JH IOMMU: Changed to DMA_TO_DEVICE (not currently in use, but if so would be RDMA Written to Target) */
		r->prp1_shadow_dma = DMA_MAP(prp1, r->prp1_shadow, r->prp1_size,
					     DMA_TO_DEVICE);
		r->prp1_sge.lkey = nvmeib_get_lkey(nv);
	}

	/* the remote submission_q local shadow */
	r->sq_shadow_dma = DMA_MAP(sq, r->sq_shadow, r->sq_size, DMA_TO_DEVICE);
	r->sq_sge.lkey = nvmeib_get_lkey(nv);

	/* the remote completion queue that we copy */
	/* JH IOMMU: DMA_FROM_DEVICE is correct. Used as a sink either for Remote RDMA_WRITE (Normal Flow) or for Local RDMA_READ (OE) */
	r->cq_shadow_dma = DMA_MAP(cq, r->cq_shadow, r->cq_size, DMA_FROM_DEVICE);
	r->cq_riu.rkey = nvmeib_get_rkey(nv);
	r->cq_riu.lkey = nvmeib_get_lkey(nv);

	/* the remote submission_q doorbell local shadow */
	/* JH IOMMU: DMA_TO_DEVICE is correct, used for RDMA_WRITE only */
	r->sq_db_sge.addr = DMA_MAP(sq_db, &r->sq_db_value, sizeof(r->sq_db_value),
				    DMA_TO_DEVICE);
	r->sq_db_sge.lkey = nvmeib_get_lkey(nv);

	/* the remote completion_q doorbell local shadow */
	/* JH IOMMU: DMA_TO_DEVICE is correct, used for RDMA_WRITE only */
	r->cq_db_sge.addr = DMA_MAP(cq_db, &r->cq_db_value, sizeof(r->cq_db_value),
				    DMA_TO_DEVICE);
	r->cq_db_sge.lkey = nvmeib_get_lkey(nv);

	/* the remote completion_q doorbell local shadow */
	/* JH IOMMU: DMA_TO_DEVICE is correct, used for RDMA_WRITE only */
	r->msix_sge.addr = DMA_MAP(msix_sge, &r->msi_x_payload, sizeof(r->msi_x_payload),
				   DMA_TO_DEVICE);
	r->msix_sge.lkey = nvmeib_get_lkey(nv);

	/* the read lock buffer */
	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as sink for Remote RDMA_WRITE during regular IO, and sink for Local RDMA_READ (OE) */
	r->rl_riu.raddr = DMA_MAP(read_lock, r->read_lock_buffer, sizeof(r->read_lock_buffer),
				  DMA_FROM_DEVICE);
	r->rl_riu.lkey = nvmeib_get_lkey(nv);
	r->rl_riu.rkey = nvmeib_get_rkey(nv);

	/* JMDC Piggyback source */
	if (nvmeibc_ib_net_jmdc_pb_map(nv, &r->jmdc_pb, r->info->disk->jour.rng_binje))
		goto failed_jmdc_pb;

	ret = 0;

	goto out;
failed_jmdc_pb:
	ib_dma_unmap_single(ib, r->rl_riu.raddr, sizeof(r->read_lock_buffer), DMA_FROM_DEVICE);
failed_read_lock:
	ib_dma_unmap_single(ib, r->msix_sge.addr, sizeof(r->msi_x_payload), DMA_TO_DEVICE);
failed_msix_sge:
	ib_dma_unmap_single(ib, r->cq_db_sge.addr, sizeof(r->cq_db_value), DMA_TO_DEVICE);
failed_cq_db:
	ib_dma_unmap_single(ib, r->sq_db_sge.addr, sizeof(r->sq_db_value), DMA_TO_DEVICE);
failed_sq_db:
	ib_dma_unmap_single(ib, r->cq_shadow_dma, r->cq_size, DMA_FROM_DEVICE);
failed_cq:
	ib_dma_unmap_single(ib, r->sq_shadow_dma, r->sq_size, DMA_TO_DEVICE);
failed_sq:
	ib_dma_unmap_single(ib, r->prp1_shadow_dma, r->prp1_size, DMA_TO_DEVICE);
failed_prp1:
	r->nv = NULL;

out:
	NFOUT;

	return ret;
}

static void add_resource(struct nvmeibc_disk *disk, struct rsc_info *rsc)
{
	__NFIND;
	BUG_ON(!list_empty(&rsc->link));
	list_add_tail(&rsc->link, &disk->info->my_rscs);
	/* we have another one */
	++disk->info->mine;
	__NFOUTD;
}

static int _remove_resource(struct nvmeibc_disk *disk)
{
	struct rsc_info *rsc;

	__NFIND;
	rsc = list_first_entry_or_null(&disk->info->my_rscs, struct rsc_info, link);
	if (rsc) {
		list_del_init(&rsc->link);
		--disk->info->mine;
	}
	__NFOUTD;
	return rsc ? rsc->id : -1;
}



static int locate_resource_(struct nvmeibc_disk *disk, u64 id)
{
	unsigned i = id;
	int rv = 0;

	__NFIND;
	_NT(trace_disk_locate_resource, "Adding index @ID", (long)id);
	if (i < disk->info->n_rscs)
		add_resource(disk, &disk->info->rscs[i]);
	else {
		_NE(error_disk_locate_resource, "@DISK_NAME Locate resource with id=@ID which above max_resources index @N_RSCS",
			disk->name, (long)id, disk->info->n_rscs);
		rv = -1;
	}
	__NFOUTD;
	return rv;
}

int nvmeibc_disk_locate_resource(struct nvmeibc_disk *disk, u64 id)
{
	unsigned long flags;
	int rv;

	__NFIND;
	spin_lock_irqsave(&disk->spinlock, flags);
	rv = locate_resource_(disk, id);
	spin_unlock_irqrestore(&disk->spinlock, flags);
	__NFOUTD;
	return rv;
}

static int 
pending_cmds_move_if_timed_out(struct nvmeibc_disk *disk,
										   struct nvmeibc_disk_command *disk_cmd,
										   struct list_head *list,
										   bool pcpu_pending)
{
	struct nvmeibc_disk_info *info = disk->info;
	ulong now = jiffies;
	int rv = 0;
	__NFIND;

	if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
		if (nvmeibc_disk_io_command_is_timed_out(
			disk_to_block(disk_cmd), now)) {
			list_move(&disk_cmd->dcmd_link, list);
			if (!pcpu_pending) {
				if (disk_cmd->server_side_only)
					info->n_use_nrch_only--;
				info->tot_pending--;
				info->tot_io_pending--;
			}
			rv++;
		}
	}
	else if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_GEN) {
		if (nvmeibc_disk_gen_cmd_is_timed_out(
			disk_to_gen(disk_cmd), now)) {
			if (!pcpu_pending) {
				info->n_use_nrch_only--;
				info->tot_pending--;
			}
			list_move(&disk_cmd->dcmd_link, list);
			rv++;
		}
	}
	else if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK) {
		if (nvmeibc_locks_channel_lock_cmd_is_timed_out(
			disk_to_lock(disk_cmd), now)) {
			if (!pcpu_pending) {
				info->n_use_nrch_only--;
				info->tot_pending--;
			}
			list_move(&disk_cmd->dcmd_link, list);
			rv++;
		}
	}
	else {
		_NE(error_0_disk_drop_old_execute_pending,
			"Unknown disk command type @CMD_TYPE", disk_cmd->cmd_type);
		rv = -EINVAL;
		BUG();
	}

	__NFOUTD;
	return rv;
}

static void pending_cmds_abort(struct nvmeibc_disk *disk,
							   struct list_head *list,
							   bool is_timeout);

struct drop_old_execute_pending_pcpu_params {
	struct nvmeibc_disk *disk;
	struct list_head *obsolete_cmds;
};

static void drop_old_execute_pending_pcpu(void *ctx)
{
	struct drop_old_execute_pending_pcpu_params *params = ctx;
	struct nvmeibc_disk *disk = params->disk;
	struct nvmeibc_disk_info *info = disk->info;
	struct nvmeibc_disk_command *disk_cmd, *t;
	int cpu;
	unsigned long flags;

	cpu = get_cpu();

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		goto put_cpu;

	local_irq_save(flags);

	list_for_each_entry_safe(disk_cmd, t, &info->pcpu_nrchs[cpu].pending_disk_cmds, dcmd_link) {
		if (pending_cmds_move_if_timed_out(disk, disk_cmd, &params->obsolete_cmds[cpu], true) > 0) {
			info->pcpu_nrchs[cpu].n_pending--;
			BUG_ON(info->pcpu_nrchs[cpu].n_pending < 0);
		}
	}

	local_irq_restore(flags);

put_cpu:
	put_cpu();
}

static void drop_old_execute_pending(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *info = disk->info;
	struct nvmeibc_disk_command *disk_cmd, *t;
	unsigned long flags = 0;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;
	int i;
	LIST_HEAD(obsolete_cmds);
	struct drop_old_execute_pending_pcpu_params pcpu_params = {
		.disk = disk,
	};
	__NFIND;

	/* move timed-out pending cmds of disk */
	spin_lock_irqsave(&disk->spinlock, flags);
	for (i = 0; i < DISK_PEND_PRIO_MAX; i++) {
		list_for_each_entry_safe(disk_cmd, t, &info->pending_disk_cmds[i], dcmd_link) {
			pending_cmds_move_if_timed_out(disk, disk_cmd, &obsolete_cmds, false);
		}
	}
	spin_unlock_irqrestore(&disk->spinlock, flags);

	/* move timed-out pending cmds of disk's per pcpu-nrchs */
	if (!disk->pcpu_nrchs_ll) {
		for (i = 0; i < ARRAY_SIZE(info->pcpu_nrchs); i++) {
			spin_lock_irqsave(&info->pcpu_nrchs[i].spinlock, flags);
			list_for_each_entry_safe(disk_cmd, t, &info->pcpu_nrchs[i].pending_disk_cmds, dcmd_link) {
				pending_cmds_move_if_timed_out(disk, disk_cmd, &obsolete_cmds, true);
			}
			spin_unlock_irqrestore(&info->pcpu_nrchs[i].spinlock, flags);
		}
	} else {
		/* per-cpu are lock-less, need to schedule on each of their cpus */
		if (!(pcpu_params.obsolete_cmds = kcalloc(NVMEIB_DFLT_MAX_CPUS,
			sizeof(*pcpu_params.obsolete_cmds), GFP_KERNEL))) {
			_NE(err_drop_old_execute_pending_oom, "OOM");
			WARN_ON_ONCE(1);
		} else {
			for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++)
				INIT_LIST_HEAD(&pcpu_params.obsolete_cmds[i]);

			/* From the kernel doc:
			* 	"You must not call this function with disabled interrupts or from a hardware interrupt handler or from a bottom half handler."
			*/
			BUG_ON(irqs_disabled() || in_interrupt());

			on_each_cpu_mask(disk->pcpu_nrchs_ll_cpumask, drop_old_execute_pending_pcpu, &pcpu_params, true);

			for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++)
				list_splice(&pcpu_params.obsolete_cmds[i], &obsolete_cmds);

			kfree(pcpu_params.obsolete_cmds);
		}
	}
	
	if (info->coremask_info && info->coremask_info->n_coremask) {
		struct nvmeibc_disk_coremask_chs *coremask_chs;
		/* NOTE: This runs from same context as coremask update so we don't need to lock the list of coremasks */
		list_for_each_entry(coremask_chs, &info->coremask_info->coremask_chs, link) {
			spin_lock_irqsave(&coremask_chs->spinlock, flags);
			list_for_each_entry_safe(disk_cmd, t, &coremask_chs->pending_cmds, dcmd_link) {
				pending_cmds_move_if_timed_out(disk, disk_cmd, &obsolete_cmds, true);
			}
			spin_unlock_irqrestore(&coremask_chs->spinlock, flags);
		}
	}

	pending_cmds_abort(disk, &obsolete_cmds, true); /* abort pending cmds on timeout */

	/* Also drop all expired locks from the deferred list */
	BUG_ON(!(disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
			(struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 })));
	nvmeibc_disk_locks_drain_defered(disk_segs_locks->lock_ch, true, false);
	nvmeibc_disk_put_segs_locks(disk_segs_locks,
			(struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 });

	__NFOUTD;
}

static void nvmeibc_disk_channel_version_update(struct nvmeibc_channel *ch)
{
	struct nvmeibc_disk *disk = ch->disk;
	unsigned long flags;
	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);
	nvmeibc_channel_version_update_nolock(ch);
	spin_unlock_irqrestore(&disk->spinlock, flags);

	__NFOUTD;
}

void nvmeibc_disk_channel_version_invalidate(struct nvmeibc_channel *ch)
{
	struct nvmeibc_disk *disk = ch->disk;
	unsigned long flags;
	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);
	nvmeibc_channel_version_invalidate_nolock(ch);
	spin_unlock_irqrestore(&disk->spinlock, flags);

	__NFOUTD;
}

/* Returns true if err indicates a fault with the ioch path */
static bool start_ioch_path_error(int err)
{
	switch (err) {
	case -ENETDOWN:
	case -ENETUNREACH:
	case -ENETRESET:
	case -ETIMEDOUT:
	case -EHOSTDOWN:
	case -EHOSTUNREACH:
		return true;
	default:
		return false;
	}
}

inline static void update_avail_nordda_for_cpu_locked(struct nvmeibc_disk *disk)
{
	struct nvmeibc_ib_nordda_channel *nrch_iter;
	unsigned cpu, max_cpu = min_t(unsigned, NVMEIB_DFLT_MAX_CPUS, nr_cpu_ids);

	/* Divide the available norddas into the per-cpu array */
	if (unlikely(plist_head_empty(&disk->info->available_norddas))) {
		for (cpu = 0; cpu < NVMEIB_DFLT_MAX_CPUS; cpu++)
			disk->info->avail_nordda_for_cpu[cpu] = NULL;
	} else {
		nrch_iter = plist_first_entry(&disk->info->available_norddas, typeof(*nrch_iter), available_link);
		for (cpu = 0; cpu < max_cpu; cpu++) {
			if (!cpu_online(cpu)) {
				disk->info->avail_nordda_for_cpu[cpu] = NULL;
				continue;
			}
			disk->info->avail_nordda_for_cpu[cpu] = nrch_iter;

			_ND(t0_update_avail_nordda_for_cpu_locked,
				"add nrch @NRCH_NAME (@NRCH), idx @INDEX to avail_nordda_for_cpu entry/cpu=@INT",
				nrch_iter->base.name, nrch_iter, nrch_iter->base.index, cpu);

			if (cpu < disk->info->n_avail_norddas)
				nrch_iter->cpu = cpu;

			if (nrch_iter == plist_last_entry(&disk->info->available_norddas, typeof(*nrch_iter), available_link)) {
				/* End of the list - Wrap */
				nrch_iter = plist_first_entry(&disk->info->available_norddas, typeof(*nrch_iter), available_link);
			} else {
				nrch_iter = plist_next_entry(nrch_iter, available_link);
			}
		}
	}
}

struct pcpu_nrch_del_smp_params {
	struct nvmeibc_disk *disk;
	struct nvmeibc_ib_nordda_channel *nrch;
	struct list_head *pend_list;
};

static void pcpu_nrch_del_smp_fn(void *ctx)
{
	struct pcpu_nrch_del_smp_params *params = ctx;
	pcpu_nrch_del(params->disk, params->nrch, params->pend_list);
}

int nvmeibc_disk_prefix_priority_masks_validate_module_params(void)
{
	int i;

	for (i = 0; i < nvmeibc_disk_prefix_priority_masks_len - 1; i++) {
		if (nvmeibc_disk_prefix_priority_masks[i] >= nvmeibc_disk_prefix_priority_masks[i + 1]) {
			_NE_dmesg(common_prefix_invalidate,
					"nvmeibc_disk_prefix_priority_masks should be sorted in ascending order: "
					"Found boundary[@INT] = @INT >= boundary[@INT] = @INT",
					i, nvmeibc_disk_prefix_priority_masks[i],
					i + 1, nvmeibc_disk_prefix_priority_masks[i + 1]);
			return 1;
		}
	}

	return 0;
}

inline static void nvmeibc_disk_available_norddas_add(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_nordda_channel *nrch)
{
	unsigned long flags;

	__NFIND;

	if (!plist_node_empty(&nrch->available_link)) {
		_NW(warn_disk_nvmeibc_disk_available_norddas_add, "nrch @NRCH, already using available_link", nrch);
		WARN_ON(1);
	}
	else if (is_pcpu_nrch(nrch)) {
		pcpu_nrch_add(disk, nrch);
		nvmeibc_disk_inc_io_chan(disk);
	}
	else {
		_NT(trace_disk_nvmeibc_disk_available_norddas_add, "Add nrch @NRCH to available norddas list", nrch);
		spin_lock_irqsave(&disk->spinlock, flags);
		nvmeib_public_plist_add(&nrch->available_link, &disk->info->available_norddas);
		
		if (disk->info->avail_norddas_per_numa_node) {
			nvmeib_public_plist_add(&nrch->per_numa_node_link, &disk->info->avail_norddas_per_numa_node[nrch->base.numa_node]);
		}
		disk->info->n_avail_norddas++;
		update_avail_nordda_for_cpu_locked(disk);
		spin_unlock_irqrestore(&disk->spinlock, flags);
		nvmeibc_disk_inc_io_chan(disk);
	}

	__NFOUTD;
}

void nvmeibc_disk_available_norddas_del(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_nordda_channel *nrch)
{
	unsigned long flags;
	__NFIND;

	if (!plist_node_empty(&nrch->available_link)) {
		_NT(trace_disk_nvmeibc_disk_available_norddas_del, "Del nrch @NRCH from available norddas list", nrch);
		WARN_ON(is_pcpu_nrch(nrch));
		spin_lock_irqsave(&disk->spinlock, flags);
		nvmeib_public_plist_del(&nrch->available_link, &disk->info->available_norddas);
		if (disk->info->avail_norddas_per_numa_node) {
			nvmeib_public_plist_del(&nrch->per_numa_node_link, &disk->info->avail_norddas_per_numa_node[nrch->base.numa_node]);
		}
		disk->info->n_avail_norddas--;
		update_avail_nordda_for_cpu_locked(disk);
		spin_unlock_irqrestore(&disk->spinlock, flags);
		nvmeibc_disk_dec_io_chan(disk);
	}
	else if (is_pcpu_nrch(nrch)) {
		LIST_HEAD(pend_list);
		if (!is_ll_pcpu_nrch(nrch))
			pcpu_nrch_del(disk, nrch, &pend_list);
		else {
			struct pcpu_nrch_del_smp_params params = {
				.disk = disk,
				.nrch = nrch,
				.pend_list = &pend_list,
			};

			BUG_ON(irqs_disabled() || in_interrupt());
			smp_call_function_single(pcpu_nrch_cpu_get(nrch), pcpu_nrch_del_smp_fn, &params, true);
		}
		pcpu_nrch_cpu_clear(nrch);
		pending_cmds_abort(disk, &pend_list, false);  /* abort pending cmds on nrch-disconnect */
		nvmeibc_disk_dec_io_chan(disk);
	}
	else
		_NT(trace_1_disk_nvmeibc_disk_available_norddas_del, "nrch @NRCH, not using available_link", nrch);

	__NFOUTD;
}


/* Cleanup after a periodic start-io-nordda-channels loop:
 * Wait for nordda channels, that failed to connect (after their net
 * became live) and launched ad-hoc release wq, and free that wq.
 */
static void start_io_nordda_channels_cleanup(struct nvmeibc_disk *disk)
{
	struct list_head *rionics = &disk->nr_rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *nr_lionics = NULL;
	struct nvmeibc_io_lnic *lionic = NULL;
	struct nvmeibc_ib_nordda_channel *nrch = NULL;
	int i;
	__NFIND;

	list_for_each_entry(rionic, rionics, disk_nrlink) {
		nr_lionics = &rionic->nr_lionics;
		list_for_each_entry(lionic, nr_lionics, rionic_nrlink) {
			for (i = 0; i < lionic->n_nr_qps; ++i) {
				nrch = lionic->nr_channels + i;
				if (nvmeibc_ib_nordda_channel_is_used(nrch))
					/* if remove-work was added to admin-wq,
					   the nrch is still mark used */
					nvmeibc_ib_nordda_channel_clear_rq(nrch, true);
			}
		}
	}

	__NFOUTD;
}

static int prefix_priority_masks_cmp_fn(void *priv, struct list_head *a, struct list_head *b)
{
	struct nvmeibc_io_path *io_path_a = container_of(a, struct nvmeibc_io_path, iopaths_link);
	struct nvmeibc_io_path *io_path_b = container_of(b, struct nvmeibc_io_path, iopaths_link);
	int io_path_a_priority = io_path_a->priority;
	int io_path_b_priority = io_path_b->priority;

	/* Higher value priority goes last. */
	if (io_path_a_priority < io_path_b_priority)
		return -1;
	else if (io_path_a_priority == io_path_b_priority) {
		if (io_path_a->n_chs < io_path_b->n_chs)
			return -1;
		else if (io_path_a->n_chs == io_path_b->n_chs)
			return 0;
		return 1;
	}
	return 1;
}

static void disk_nr_iopaths_add(struct nvmeibc_io_path *new,
						 struct list_head *iopaths_list, bool init)
{
	struct nvmeibc_io_lnic *lionic = iop_to_lionic(new);
	struct nvmeibc_io_path *iop;
	struct list_head *prev = iopaths_list;

	_ND(trace_0_disk_nr_iopaths_add,
		"Disk @DISK_NAME, add iopath: "
		"[@INT] l=@IB_GID_IPV6, r=@IB_GID_IPV6, n_chs=@INT common_prefix_prio=@COMMON_PREFIX_PRIORITY init=@BOOL",
		lionic->disk->name, new->idx, &lionic->ib_gid,
		&lionic->rionic->ib_gid, new->n_chs, new->priority, init);

	if (nvmeibc_disk_prefix_priority_masks_len && init) {
		prio_list_add_tail(&new->iopaths_link, iopaths_list, prefix_priority_masks_cmp_fn, NULL);
	} else {
		list_for_each_entry(iop, iopaths_list, iopaths_link) {
			if (new->n_chs < iop->n_chs ||
				(new->n_chs == iop->n_chs && new->prefered && !iop->prefered)) {
				break;
			}
			else {
				prev = &iop->iopaths_link;
			}
		}
		list_add(&new->iopaths_link, prev);
	}

}

static void disk_nr_iopaths_add_tail(struct nvmeibc_io_path *new,
							  struct list_head *iopaths_list)
{
	struct nvmeibc_io_lnic *lionic = iop_to_lionic(new);
	struct nvmeibc_io_path *iop;
	struct list_head *next = iopaths_list;

	_ND(trace_0_disk_nr_iopaths_add_tail,
		"Disk @DISK_NAME, add iopath: "
		"[@INT] l=@IB_GID_IPV6, r=@IB_GID_IPV6, n_chs=@INT",
		lionic->disk->name, new->idx, &lionic->ib_gid,
		&lionic->rionic->ib_gid, new->n_chs);

	list_for_each_entry_reverse(iop, iopaths_list, iopaths_link) {
		if (new->n_chs > iop->n_chs ||
			(new->n_chs == iop->n_chs && !new->prefered && iop->prefered)) {
			break;
		}
		else {
			next = &iop->iopaths_link;
		}
	}
	list_add_tail(&new->iopaths_link, next);
}

static struct nvmeibc_io_path *disk_nr_iopaths_pop_first_or_null(struct list_head *iopaths_list)
{
	struct nvmeibc_io_path *iop;

	if (list_empty(iopaths_list))
		return NULL;

	iop = list_first_entry(iopaths_list, struct nvmeibc_io_path, iopaths_link);
	list_del_init(&iop->iopaths_link);
	return iop;
}

static struct nvmeibc_io_path *disk_nr_iopaths_pop_last_or_null(struct list_head *iopaths_list)
{
	struct nvmeibc_io_path *iop;

	if (list_empty(iopaths_list))
		return NULL;

	iop = list_last_entry(iopaths_list, struct nvmeibc_io_path, iopaths_link);
	list_del_init(&iop->iopaths_link);
	return iop;
}

static struct nvmeibc_io_path *disk_nr_iopaths_pop_admin_path_or_null(struct list_head *iopaths_list, struct nvmeibc_ib_admin_channel *ach)
{
	struct nvmeibc_io_path *iop, *ret = NULL;
	list_for_each_entry(iop, iopaths_list, iopaths_link) {
		struct nvmeibc_admin_rnic *arnic = ach->base.arnic;
		struct nvmeibc_ib_net *ach_net = &ach->net.base;
		struct nvmeibc_io_lnic *lionic = iop_to_lionic(iop);
		struct nvmeibc_io_rnic *rionic = lionic->rionic;

		if (lionic->port != ach_net->port)
			continue;
		if (arnic->ib_gid.global.interface_id != rionic->ib_gid.global.interface_id)
			continue;
		if (arnic->ib_gid.global.subnet_prefix != rionic->ib_gid.global.subnet_prefix)
			continue;
		if (arnic->link_layer != rionic->layer)
			continue;
		if (arnic->transport_type != rionic->transport_type)
			continue;
		list_del_init(&iop->iopaths_link);
		ret = iop;
		break;
	}
	return ret;
}

static struct nvmeibc_io_path *nr_pop_iop_prefix_or_null(struct list_head *iopaths_list, struct nvmeibc_disk *disk)
{
	struct nvmeibc_io_path *iop, *ret = NULL;

	list_for_each_entry(iop, iopaths_list, iopaths_link) {
		struct nvmeibc_io_lnic *lionic = iop_to_lionic(iop);
		struct nvmeibc_io_rnic *rionic = lionic->rionic;

		if (memcmp(&disk->current_common_prefix_path.sgid, &lionic->ib_gid, sizeof(union ib_gid)) != 0)
			continue;
		if (memcmp(&disk->current_common_prefix_path.dgid, &rionic->ib_gid, sizeof(union ib_gid)) != 0)
			continue;
		list_del_init(&iop->iopaths_link);
		ret = iop;
		break;
	}

	return ret;
}

static bool disk_nr_iopaths_is_unbalanced(struct list_head *iopaths_list)
{
	struct nvmeibc_io_path *head;
	struct nvmeibc_io_path *tail;

	if (list_empty(iopaths_list))
		return false;

	head = list_entry(iopaths_list->next, struct nvmeibc_io_path, iopaths_link);
	tail = list_entry(iopaths_list->prev, struct nvmeibc_io_path, iopaths_link);

	return (tail->n_chs - head->n_chs) > 1;
}

static void disk_nr_iopaths_print(struct nvmeibc_disk *disk,
						   struct list_head *iopaths_list)
{
	struct nvmeibc_io_path *iop;
	int i = 0;
	int n = 0;

	_NT(trace_0_disk_nr_iopaths_print,
		"Disk @DISK_NAME iopaths: ", disk->name);

	list_for_each_entry(iop, iopaths_list, iopaths_link) {
		struct nvmeibc_io_lnic *lionic = iop_to_lionic(iop);
		_NT(trace_1_disk_nr_iopaths_print,
			"Disk @DISK_NAME, add iopath: "
			"[@INT] idx=@INT, l=@IB_GID_IPV6, r=@IB_GID_IPV6, n_chs=@INT, p=@BOOL, priority=@COMMON_PREFIX_PRIORITY",
			disk->name, i, iop->idx, &lionic->ib_gid,
			&lionic->rionic->ib_gid, iop->n_chs, iop->prefered, iop->priority);
		n += iop->n_chs;
		i++;
	}

	_NT(trace_2_disk_nr_iopaths_print,
		"Disk @DISK_NAME, Total: iopaths=@INT: chs=@INT",
		disk->name, i, n);
}

/* create accessible iopaths (on-stack) list, sorted in ascending order
   of 'num of *currently* connected channels' and prefered rionic first */
static int disk_nr_iopaths_init(struct nvmeibc_disk *disk,
	struct list_head *iopaths_list, int *n_iopaths)
{
	struct list_head *nr_rionics = &disk->nr_rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *nr_lionics = NULL;
	struct nvmeibc_io_lnic *lionic = NULL;
	struct nvmeibc_ib_nordda_channel *nrch = NULL;
	struct nvmeibc_io_path *iop;
	int i, n_chs, n_iop = 0, tot_chs = 0;
	__NFIND;

	BUG_ON(!list_empty(iopaths_list));

	list_for_each_entry(rionic, nr_rionics, disk_nrlink) {
		if (NVMEIB_UPDATE_NW_PATHS && unlikely(!rionic->may_access)) {
			_ND(trace_0_disk_nr_iopaths_init, "No path, skip");
			continue;
		}
		nr_lionics = &rionic->nr_lionics;
		list_for_each_entry(lionic, nr_lionics, rionic_nrlink) {
			if (NVMEIB_UPDATE_NW_PATHS && unlikely(!lionic->may_access)) {
				_ND(trace_1_disk_nr_iopaths_init, "No path, skip");
				continue;
			}

			/* init iop */
			n_chs = 0;
			for (i = 0; i < lionic->n_nr_qps; ++i) {
				nrch = lionic->nr_channels + i;
				if (nvmeibc_ib_nordda_channel_is_used(nrch)) {
					n_chs++;
					nrch->iopaths_inuse = true;
				}
				else {
					/* treat chs with pending ioch-drained as unused, so
					   iopath-sibling-chs won't be 'disconnect-excess'
					   bcz of them. And fail their connect-attempt */
					nrch->iopaths_inuse = false;
				}
			}
			iop = &lionic->iopath;
			iop->idx = n_iop++;
			iop->n_chs = n_chs;
			iop->prefered = lionic->rionic->nr_prefered;
			disk_nr_iopaths_add(iop, iopaths_list, true);

			tot_chs += n_chs;
		}
	}

	disk_nr_iopaths_print(disk, iopaths_list);
	*n_iopaths = n_iop;

	__NFOUTD;
	return tot_chs;
}

struct nrch_try_disconnect_smp_fn_params {
	struct nvmeibc_ib_nordda_channel *ch;
	bool rv;
};

static void nrch_try_disconnect_smp_fn(void *ctx)
{
	struct nrch_try_disconnect_smp_fn_params *params = ctx;
	params->rv = nvmeibc_ib_nordda_channel_try_disconnect(params->ch);
}

static void disk_nr_iopaths_disconnect_excess_channels(struct list_head *iopaths_list,
												int n_excess)
{
	struct nvmeibc_io_path *iop;
	struct nvmeibc_io_lnic *lionic;
	struct nvmeibc_ib_nordda_channel *nrch, *iter;
	int i;
	NFIN;

	if (n_excess <= 0) {
		_NE(trace_0_disk_nr_iopaths_disconnect_excess_channels,
			"invalid n_excess @N_EXCESS", n_excess);
		WARN_ON_ONCE(1);
		goto out;
	}

	while (n_excess) {
		if (!(iop = disk_nr_iopaths_pop_last_or_null(iopaths_list))) {
			_NE(trace_1_disk_nr_iopaths_disconnect_excess_channels,
				"OOPS: @INT but iopaths empty\n", n_excess);
			WARN_ON_ONCE(1);
			break;
		}

		lionic = iop_to_lionic(iop);

		_NI(trace_2_disk_nr_iopaths_disconnect_excess_channels,
			"Disk @DISK_NAME, disconnect excess channel over iopath: "
			"[@INT] l=@IB_GID_IPV6, r=@IB_GID_IPV6, n_chs=@INT",
			lionic->disk->name, iop->idx, &lionic->ib_gid,
			&lionic->rionic->ib_gid, iop->n_chs);

		nrch = NULL;
		for (i = 0; i < lionic->n_nr_qps; ++i) {
			iter = lionic->nr_channels + i;
			if (iter->iopaths_inuse) {
				/* prefer disconnecting pcpu over legacy channels
				   unhandled case: iopath with the highest num of channels has many legacy chs */
				nrch = iter;
				if (is_ll_pcpu_nrch(nrch))
					break;
			}
		}
		if (nrch) {
			if (nvmeibc_channel_is_ll_pcpu_ch(&nrch->base)) {
				/* Per-cpu NRCH, call on correct CPU using IPI */
				struct nrch_try_disconnect_smp_fn_params params = {
					.ch = nrch,
				};

				BUG_ON(irqs_disabled() || in_interrupt());
				smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(&nrch->base), nrch_try_disconnect_smp_fn, &params, true);
			} else
				nvmeibc_ib_nordda_channel_try_disconnect(nrch);
			nrch->iopaths_inuse = false;
			iop->n_chs--;
			if (!nvmeibc_disk_prefix_priority_masks_len)
				disk_nr_iopaths_add_tail(iop, iopaths_list);
			else
				disk_nr_iopaths_add(iop, iopaths_list, false);
			n_excess--;
		}
		else {
			_NE(trace_3_disk_nr_iopaths_disconnect_excess_channels,
				"OOPS: No inuse nrch found in iop\n");
			WARN_ON_ONCE(1);
			break;
		}
	}

out:
	NFOUT;
}

static int connect_nrch(struct nvmeibc_ib_nordda_channel *nrch)
{
	struct nvmeibc_disk *disk = nrch->lionic->disk;
	struct nvmeibc_ib_admin_channel *ach = ac_to_iac(nrch->lionic->rionic->ch);
	void *context;
	int rv;

	if (is_ll_pcpu_nrch(nrch))
		BUG_ON(smp_processor_id() != pcpu_nrch_cpu_get(nrch));

	nvmeibc_ib_nordda_channel_use(nrch);

	/* Clear latency counters */
	nvmeibc_nr_lat_meas_clear_nrch_pcpu_data(nrch->per_cpu_lat_data);

	/* if ch disconnects after connecting, disconnect can take place either:
	   - on this admin-wq i.e. after we're done here
	   - on ad-hoc release-wq
	   if ch connect failed and net is not LIVE, just mark as unused.
	*/
	nvmeib_reinit_completion(&nrch->init_comp);
	nrch->lionic->start_ioch_attempt_ctr++;
	nvmeibc_disk_channel_version_update(&nrch->base);
	if ((rv = nvmeibc_ib_admin_channel_connect_nordda_channel(ach, nrch)) < 0) {
		bool nrch_connected;
		_NT(trace_1_connect_nrch,
			"Failed (@RV) to connect nrch, disk @DISK_NAME, nrch @BASE_NAME",
			rv, disk->name, nrch->base.name);
		if (start_ioch_path_error(rv)) {
			nrch->lionic->start_ioch_path_fail_ctr++;
			_NT(trace_2_connect_nrch,
			   "io-path @SGID -> @SGID: connect failure inc to #@INT",
			   &nrch->lionic->path.sgid, &nrch->lionic->path.dgid,
			   nrch->lionic->start_ioch_path_fail_ctr);
		}
		nrch_connected = nvmeibc_ib_nordda_channel_try_disconnect(nrch);

		if (!nrch_connected) {
			_NT(trace_3_connect_nrch,
				"nrch @BASE_NAME, net was not connnected (LIVE), mark unused",
				nrch->base.name);
			nvmeibc_disk_channel_version_invalidate(&nrch->base);
			nvmeibc_ib_nordda_channel_end_use(nrch);
			pcpu_nrch_cpu_clear(nrch);
		}
	}
	else {
		int n_pend_io = 0;
		int pend_rv;

		/* Get the first req (w/o locking disk as ch was not added
		   yet to available list and as remove-work waits for us) */
		if ((context = nvmeibc_ib_nordda_channel_get_io_context(nrch))) {
			nvmeibc_disk_available_norddas_add(disk, nrch);
			_NT(trace_4_connect_nrch,
				"Added nrch @BASE_NAME to available norddas for disk @DISK_NAME "
				"with QPn: @QP_NUM Remote QPn: @REMOTE_QPN", nrch->base.name, disk->name,
				nrch->net.base.qp->qp_num, nrch->net.base.remote_qpn);
			rv = 0;
			/* send as many pending as possible to improve rediscover IO delay */
			do {
				pend_rv = nrch->base.execute_pending_io(
					disk, &nrch->base, context, false, 0);
			} while (pend_rv == 0 && ++n_pend_io < disk->nrch_ioreq_num &&
				(context = nvmeibc_ib_nordda_channel_get_io_context(nrch)));
		}
		else {
			_NW(warn_0_connect_nrch,
				"Disk: @DISK_NAME (@DISK) - Unexpected, no free reqs from a brand new nrch @BASE_NAME (@NRCH), "
				"(dying={c=@DYING, n=@DYING}, n_used=@INT",
				disk->name, disk, nrch->base.name, nrch,
				atomic_read(&nrch->base.dying),
				atomic_read(&nrch->net.base.dying), nrch->n_used_reqs);
			rv = -1;
		}
	}

	complete(&nrch->init_comp);

	return rv;
}

void nvmeibc_disk_connect_nrch_pcpu_work(struct work_struct* work)
{
	struct nvmeibc_ib_nordda_channel *nrch = container_of(work, struct nvmeibc_ib_nordda_channel, pcpu_connect_work);
	nrch->connect_rv = connect_nrch(nrch);
	complete(&nrch->pcpu_connect_comp);
}

/* return 0 if found channel to try and connect
   otherwise, caller can drop iop from the loop */
static int disk_nr_iopaths_connect_channel(struct nvmeibc_io_path *iop,
				bool is_pcpu, bool pcpu_ll, bool *connected)
{
	struct nvmeibc_io_lnic *lionic = iop_to_lionic(iop);
	struct nvmeibc_ib_nordda_channel *nrch = NULL;
	int i;
	int rv = -1;

	*connected = false;

	_NT(trace_0_disk_nr_iopaths_connect_channel,
		"Disk @DISK_NAME, connect channel over iopath: "
		"[@INT] l=@IB_GID_IPV6, lionic=@PTR, r=@IB_GID_IPV6, n_chs=@INT, is_pcpu=@BOOL_YN priority=@INT",
		lionic->disk->name, iop->idx, &lionic->ib_gid, lionic,
		&lionic->rionic->ib_gid, iop->n_chs, is_pcpu, iop->priority);

	if (!use_lionic_path_for_ioch(lionic)) {
		_NT(trace_1_disk_nr_iopaths_connect_channel,
		    "Disk @DISK_NAME, lionic path not in use: "
		    "[@INT] l=@IB_GID_IPV6, r=@IB_GID_IPV6",
		lionic->disk->name, iop->idx, &lionic->ib_gid,
		&lionic->rionic->ib_gid);
		goto out;
	}

	if (iop->n_chs == lionic->n_nr_qps) {
		_NT(trace_2_disk_nr_iopaths_connect_channel,
		    "Disk @DISK_NAME, max lionic QPs reached:"
		    "[@INT] l=@IB_GID_IPV6, r=@IB_GID_IPV6, n_chs=@INT",
			lionic->disk->name, iop->idx, &lionic->ib_gid,
			&lionic->rionic->ib_gid, iop->n_chs);
		goto out;
	}

	for (i = 0; i < lionic->n_nr_qps; i++) {
		nrch = lionic->nr_channels + i;
		if (nrch->iopaths_inuse)
			continue;
		nvmeibc_ib_nordda_channel_clear_rq(nrch, true);
		if (nvmeibc_disk_ioch_drained_is_pending(&nrch->base)) {
			_NT(trace_disk_nr_iopaths_connect_channel_nrch_bailed_cmds_not_empty,
			    "skip nrch @BASE_NAME, still have pending bailed-cmds",
				nrch->base.name);
			/* just fail it so it will be removed from this
			 *   start-iochs loop w/o effecting its siblings */
			continue;
		}

		if (is_pcpu_nrch(nrch)) {
			_NE_dmesg(warn_disk_nr_iopaths_connect_channel_nrch_pcpu_set, "oops, nrch @BASE_NAME (@CHANNEL_PTR) still has valid comp-cpu=@INT",
				  nrch->base.name, &nrch->base, pcpu_nrch_cpu_get(nrch));
			BUG();
		}

		nrch->iopaths_inuse = true;

		if (nvmeibc_disk_prefix_priority_masks_len) {
			/* Nrch priority expect this order */
			nrch->priority.raw = iop->priority;
		}

		
		if (is_pcpu) {
			struct nvmeibc_disk *disk = nrch->lionic->disk;
			int nrch_pcpu_cpu = pcpu_nrch_get_next_cpu(disk);
			if (nrch_pcpu_cpu < 0) {
				_NI_dmesg(trace_disk_nr_iopaths_connect_channel_fail_alloc_cpu,
					"disk @DISK_NAME - Fail to alloc cpu for per-cpu nrch",
					disk->name);
				goto out;
			}
			pcpu_nrch_cpu_set(nrch, nrch_pcpu_cpu, pcpu_ll);
		}

		if (is_pcpu && pcpu_ll) {
			struct nvmeibc_disk *disk = nrch->lionic->disk;
			nvmeib_reinit_completion(&nrch->pcpu_connect_comp);
			if (!queue_work_on(pcpu_nrch_cpu_get(nrch), disk->info->pcpu_wq, &nrch->pcpu_connect_work)) {
				_NI_dmesg(trace_disk_nr_iopaths_connect_channel_sched_work_fail,
					"disk @DISK_NAME - Fail to schedule work for per-cpu nrch @BASE_NAME (@CHANNEL_PTR)",
					disk->name, nrch->base.name, &nrch->base);
				BUG();
			}
			wait_for_completion(&nrch->pcpu_connect_comp);
			*connected = nrch->connect_rv == 0;
		} else {
			*connected = connect_nrch(nrch) == 0;
		}
		rv = 0;
		goto out;
	}

out:
	return rv;
}

static void init_lnic_counters(struct nvmeibc_disk *disk)
{
	struct list_head *rionics = &disk->rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *lionics = NULL;
	struct nvmeibc_io_lnic *lionic = NULL;
	int prev_path_fail_ctr;

	_NT(init_lnic_counters_trace, "New round as we called withtout premmption paramater, init lnics counters");

	list_for_each_entry(rionic, rionics, disk_link) {
		if (NVMEIB_UPDATE_NW_PATHS && unlikely(!rionic->may_access)) {
			_NT(init_nr_rnic_no_access, "No path, skip");
			continue;
		}
		lionics = &rionic->lionics;
		list_for_each_entry(lionic, lionics, rionic_link) {
			if (NVMEIB_UPDATE_NW_PATHS && unlikely(!lionic->may_access)) {
				_NT(init_nr_lnic_no_access, "No path, skip");
				continue;
			}
			prev_path_fail_ctr = lionic->start_ioch_path_fail_ctr;
			/* reinit counters for this round */
			_NT(t1_disk_use_lionic_path_for_ioch,
				"re/init path-counters of lionic=@LIONIC "
				"(l=@HW_GID -> r=@HW_GID, layer=@LAYER) for this round "
				"(prev_path_fail_ctr=@INT, lionic/disk=@INT/@INT)",
				lionic, &lionic->port->gid.hw_gid, &lionic->rionic->hw_gid,
				lionic->layer, prev_path_fail_ctr,
				lionic->start_ioch_attempt_id, disk->start_ioch_ctr);
			lionic->start_ioch_attempt_id = disk->start_ioch_ctr;
			lionic->start_ioch_attempt_ctr = 0;
			lionic->start_ioch_path_fail_ctr = 0;

			if (prev_path_fail_ctr > 0) {
				int rv;
				/* Last attempt on this path failed. Refresh the path info */
				rv = nvmeibc_disk_lionic_rionic_find_path(lionic);
				if (rv) {
					lionic->start_ioch_path_fail_ctr++;
					_NT(t2_disk_use_lionic_path_for_ioch,
						"Failed to find path (@RV) -> connect failure inc to #@INT",
						rv, lionic->start_ioch_path_fail_ctr);
				}
			}
		}
	}
}

static void nvmeibc_disk_start_io_nordda_channels_(struct nvmeibc_disk *disk)
{
	struct nvmeibc_ib_admin_channel *ach;
	int d, s = -1;
	int n_nrchs;
	int cnt = 0;
	int max_nrchs = nr_max_channels_per_disk; /* read once, no lock */
	int max_nrchs_any_cpu;
	int n_iopaths;
	int n_iter = 0;
	LIST_HEAD(iopaths_list);
	bool connected;
	bool is_pcpu;
	struct nvmeibc_io_lnic *lionic;
	__NFIND;

	/* safety */
	if (!(ach = get_alive_admin_ch(disk))) {
		_NT(trace_disk_nvmeibc_disk_start_io_nordda_channels,
			"Disk @DISK_NAME, no main admin-ch ...", disk->name);
		goto out;
	}

	if (!on_wq(ach->base.remove_wq)) {
		_NW(warn_disk_nvmeibc_disk_start_io_nordda_channels,
			"Oops, main admin-ch work scheduled not on its wq (@PID vs. @PID)",
			current->pid, wq_pid(ach->base.remove_wq));
		goto out;
	}

	if (disk->md_size && max_nrchs < NVMEIB_MIN_NR_CHANNELS_PER_DISK) {
		_NI(trace_1_disk_nvmeibc_disk_start_io_nordda_channels,
			"Disk @DISK_NAME has MD, override max_nrchs (@MAX_NRCHS) to "
			"@MAX_NRCHS (for gen-cmds)",
		   disk->name, max_nrchs, NVMEIB_MIN_NR_CHANNELS_PER_DISK);
		max_nrchs = NVMEIB_MIN_NR_CHANNELS_PER_DISK;
	}

	max_nrchs_any_cpu = max_nrchs;
	if (disk->pcpu_nrchs) {
		int n = disk->pcpu_nrchs_max_per_disk;
		_NI(trace_pcpu_disk_nvmeibc_disk_start_io_nordda_channels,
			  "Disk @DISK_NAME, use pcpu nrchs, max_nrchs: @MAX_NRCHS -> @MAX_NRCHS, max_nrchs_any_cpu: @MAX_NRCHS",
			disk->name, max_nrchs, max_nrchs + n, max_nrchs_any_cpu);
		/* First ensure we have @max_nrchs_any_cpu channels which can be used
		   from any cpu (support EC rcookie) then connect @n more percpu channels */
		max_nrchs += n;
	}

	n_nrchs = disk_nr_iopaths_init(disk, &iopaths_list, &n_iopaths);

	_NT(trace_2_disk_nvmeibc_disk_start_io_nordda_channels,
		"Disk @DISK_NAME n_nrchs @N_NRCHS, max_nrchs @MAX_NRCHS, n_iopaths=@INT",
		disk->name, n_nrchs, max_nrchs, n_iopaths);

	if (n_nrchs > max_nrchs) {
		disk_nr_iopaths_disconnect_excess_channels(&iopaths_list, n_nrchs - max_nrchs);
		n_nrchs = max_nrchs;
	}

	while (n_nrchs < max_nrchs || disk_nr_iopaths_is_unbalanced(&iopaths_list)) {
		struct nvmeibc_io_path *iop = NULL;
		bool admin_or_prefix_path = false;

		if ((d = atomic_read(&disk->dying)) ||
			(s = atomic_read(&disk->shut_down_triggered))) {
			_NT(trace_3_disk_nvmeibc_disk_start_io_nordda_channels,
				"Disk @DISK_NAME: dying @DYING, shutdown @SHUTDOWN_STATUS",
				disk->name, d, s);
			break;
		}

		/* check this before any wait e.g. find-path, connect-qp
		 * NOTE: We want to try to connect at least 1 nrch in case the "high-priority" work needs GEN cmds
		 */
		if ((n_nrchs > 0 || n_iter > 0) && atomic_read(&ach->base.wq_high_pri_cnt) > 0) {
			_NT(trace_4_disk_nvmeibc_disk_start_io_nordda_channels,
				"Disk @DISK_NAME admin_ch @BASE_NAME (@ACH) - "
				"ending loop for high-priority work",
			   disk->name, ach->base.base.name, ach);
			disk->start_io_work_preempted_seqcnt++;
			break;
		}

		if (n_nrchs == 0 && n_iter == 0) {
			iop = !nvmeibc_disk_prefix_priority_masks_len? 
				disk_nr_iopaths_pop_admin_path_or_null(&iopaths_list, ach) : 
				nr_pop_iop_prefix_or_null(&iopaths_list, disk);
			admin_or_prefix_path = !!iop;
		}
		if (!iop) {
			/* get least-used iopath (or the prioritized one)*/
			if (!(iop = disk_nr_iopaths_pop_first_or_null(&iopaths_list))) {
				_NT(trace_5_disk_nvmeibc_disk_start_io_nordda_channels,
				    "Disk @DISK_NAME, no more iopaths", disk->name);
				break;
			}
		}
		lionic = iop_to_lionic(iop);
		_NT(trace_8_disk_nvmeibc_disk_start_io_nordda_channels,
			"Disk @DISK_NAME, using path (@IB_GID_IPV6 -> @IB_GID_IPV6) for first channel "
			" priority=@COMMON_PREFIX_PRIORITY (admin-path or common-prefix-path=@BOOL_YN)",
			disk->name, &lionic->ib_gid, &lionic->rionic->ib_gid, iop->priority, admin_or_prefix_path);

		n_iter++;

		is_pcpu = disk->pcpu_nrchs && n_nrchs >= max_nrchs_any_cpu;
		if (disk_nr_iopaths_connect_channel(iop, is_pcpu, disk->pcpu_nrchs_ll, &connected)) {
			/* either (1) iop had too many path-errors (2) all chs connected,
			   or (3) we've tried to connect all chs --> can drop from list */
			continue;
		}

		if (connected) {
			n_nrchs++;
			_NT(trace_7_disk_nvmeibc_disk_start_io_nordda_channels,
				"[@CNT] Disk @DISK_NAME, n_nrchs=@N_NRCHS",
				cnt, disk->name, n_nrchs);
			cnt++;
			iop->n_chs++;

			if (nvmeibc_disk_prefix_priority_masks_len && iop->priority < disk->current_common_prefix_path.priority
				&& nvmeibc_disk_prefix_priority_masks_rediscover_on_new) {
				struct nvmeibc_io_lnic *lionic = iop_to_lionic(iop);
				_NT(new_prefix_priority_masks_found,
			    "Disk @DISK_NAME, found new common prefix priority @COMMON_PREFIX_PRIORITY- rediscover (@IB_GID_IPV6 -> @IB_GID_IPV6)",
			    disk->name, iop->priority, &lionic->ib_gid, &lionic->rionic->ib_gid);
				nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_COMMON_PREFIX_PRIORITY_NEW);
				goto cleanup;
			}
		}

		disk_nr_iopaths_add(iop, &iopaths_list, false);

		/* new ch replaces ch from most-used iopath */
		if (n_nrchs > max_nrchs) {
			disk_nr_iopaths_disconnect_excess_channels(&iopaths_list, 1);
			n_nrchs--;
		}
	}

cleanup:
	/* To be removed, redundant */
	start_io_nordda_channels_cleanup(disk);

out:
	__NFOUTD;
}

static void disk_start_io_channels_(struct nvmeibc_disk *disk)
{
	int prev_preempted_seqcnt = disk->start_io_work_preempted_seqcnt;
	__NFIND;

	disk->start_ioch_ctr++;
	_NT(t0_disk_start_io_channels_,
		"Disk @DISK_NAME, work-cnt=@UINT, preempted_seqcnt=@UINT",
		disk->name, disk->start_ioch_ctr, disk->start_io_work_preempted_seqcnt);

	if (prev_preempted_seqcnt == 0) {
		init_lnic_counters(disk);
	}

	if (disk->coremask_support)
		disk_check_coremask_update(disk);

	nvmeibc_disk_start_io_nordda_channels_(disk);
	/* RDDA removed */
	if (disk->start_io_work_preempted_seqcnt == prev_preempted_seqcnt) {
		disk->start_io_work_preempted_seqcnt = 0;
		_NT(trace_non_preempted, "Full round done, reset start_io_work_preempted_seqcnt to 0");
	}

	__NFOUTD;
}

static void disk_trace_verb_counters_fn(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx)
{
	struct nvmeibc_disk *disk = ctx;

	NVMEIB_LOG_METRICS("@DISK_NAME " IO_STAT_VERB_TFMT " " IO_STAT_COUNTERS_BASIC_TFMT, _T, tracer_nvmeibc, info_disk_stats, disk->name, IO_STAT_VERB_TARG(verb), IO_STAT_COUNTERS_BASIC_TARG(c));
}

static void disk_trace_iostats_on_periodic(struct nvmeibc_disk *disk)
{
	unsigned long now_jiffies = jiffies;

	if (!on_wq_pid(disk->main_ach_wq_pid)) {
		_NT(disk_trace_iostats_on_periodic, "@DISK_NAME - wrong wq", disk->name);
		goto out;
	}

	nvmeibc_trace_stats_scheduling_adjust(&disk->trace_stats, now_jiffies);

	if (!nvmeibc_trace_stats_scheduling_should_trace(&disk->trace_stats, now_jiffies))
		goto out;

	/* do incremental trace of iostats  */
	nvmeib_io_stats_trace_ext(disk->stats, disk_trace_verb_counters_fn, disk, true /* diff_only */);

	nvmeibc_trace_stats_scheduling_set_next(&disk->trace_stats, now_jiffies);

out:
	return;
}

ulong nvmeibc_disk_lock_channel_periodic_timer_interval = 10000; /* milliseconds */
module_param_named(lock_channel_periodic_timer_interval, nvmeibc_disk_lock_channel_periodic_timer_interval, ulong, 0644);
MODULE_PARM_DESC(lock_channel_periodic_timer_interval,
	"Interval in milliseconds for periodic lock channel usage metrics tracing per disk");

/* Bitwise flags for controlling which lock channel metrics to dump */
#define LOCK_CH_METRIC_COUNT			(1U << 0)  /* Dump lock operation count */
#define LOCK_CH_METRIC_LATENCY			(1U << 1)  /* Dump lock operation latency histogram */
#define LOCK_CH_METRIC_DEFERRED_QUEUE_MAX	(1U << 2)  /* Dump max deferred queue length */
#define LOCK_CH_METRIC_DEFERRED_LATENCY		(1U << 3)  /* Dump deferred operation latency histogram */
#define LOCK_CH_METRIC_ALL			(LOCK_CH_METRIC_COUNT | LOCK_CH_METRIC_LATENCY | \
						 LOCK_CH_METRIC_DEFERRED_QUEUE_MAX | LOCK_CH_METRIC_DEFERRED_LATENCY)

static uint nvmeibc_disk_lock_channel_metrics_mask = LOCK_CH_METRIC_ALL;
module_param_named(lock_channel_metrics_mask, nvmeibc_disk_lock_channel_metrics_mask, uint, 0644);
MODULE_PARM_DESC(lock_channel_metrics_mask,
	"Bitwise mask controlling which lock channel metrics to dump in periodic traces:\n"
	"  bit 0 (0x01): lock_opr_count - lock operation count\n"
	"  bit 1 (0x02): lock_opr_latency - lock operation latency histogram\n"
	"  bit 2 (0x04): lock_opr_deferred_queue_length_max - max deferred queue length\n"
	"  bit 3 (0x08): lock_opr_deferred_latency - deferred operation latency histogram\n"
	"Note: Proc file always shows all metrics regardless of this mask.\n"
	"Default: 0x0F (all metrics enabled)");

static void lock_channels_visit_channel_metric(struct nvmeib_jdr_write_closure *jdr_writer,
						const char *disk_name,
						struct nvmeibc_locks_channel *lock_ch,
						int channel_idx,
						char *labels_buf,
						size_t labels_buf_size)
{
	struct nvmesh_metric_id const metric_id = {
		.name = "lock_opr_count",
		.labels = labels_buf
	};
	
	snprintf(labels_buf, labels_buf_size,
		 "module=nvmeibc;component=lock_channel;disk=%s;channel_index=%d",
		 disk_name, channel_idx);
	
	nvmesh_metric_visit(jdr_writer->base, NULL, lock_ch->metrics.opr.count, metric_id);
}

static void lock_channels_visit_channel_latency(struct nvmeib_jdr_write_closure *jdr_writer,
						 const char *disk_name,
						 struct nvmeibc_locks_channel *lock_ch,
						 int channel_idx,
						 char *labels_buf,
						 size_t labels_buf_size)
{
	struct nvmesh_metric_id const metric_id = {
		.name = "lock_opr_latency",
		.labels = labels_buf
	};
	
	snprintf(labels_buf, labels_buf_size,
		 "module=nvmeibc;component=lock_channel;disk=%s;channel_index=%d",
		 disk_name, channel_idx);
	
	nvmesh_metric_visit(jdr_writer->base, NULL, lock_ch->metrics.opr.latency, metric_id);
}

static void lock_channels_visit_channel_deferred_metrics(struct nvmeib_jdr_write_closure *jdr_writer,
							  const char *disk_name,
							  struct nvmeibc_locks_channel *lock_ch,
							  int channel_idx,
							  char *labels_buf,
							  size_t labels_buf_size)
{
	struct nvmesh_metric_id max_metric_id = {.name = "lock_opr_deferred_queue_length_max", .labels = labels_buf};
	struct nvmesh_metric_id latency_metric_id = {.name = "lock_opr_deferred_latency", .labels = labels_buf};
	
	snprintf(labels_buf, labels_buf_size,
		 "module=nvmeibc;component=lock_channel;disk=%s;channel_index=%d",
		 disk_name, channel_idx);
	
	nvmesh_metric_visit(jdr_writer->base, NULL, lock_ch->metrics.deferred.queue_length_max, max_metric_id);
	nvmesh_metric_visit(jdr_writer->base, NULL, lock_ch->metrics.deferred.latency, latency_metric_id);
}

static void lock_channels_visit_metrics(struct jdr *jdr,
						     struct nvmeib_jdr_write_closure *jdr_writer,
						     const char *disk_name,
						     struct nvmeibc_locks_channel *lock_ch,
						     int channel_idx,
						     char *labels_buf,
						     size_t labels_buf_size)
{
	jdr_object_scope(jdr, NULL);
	{
		jdr_write_var(jdr, channel_index, channel_idx);
		{
			jdr_array_scope(jdr, "metrics");
			lock_channels_visit_channel_metric(jdr_writer, disk_name, lock_ch,
							channel_idx, labels_buf, labels_buf_size);
			lock_channels_visit_channel_latency(jdr_writer, disk_name, lock_ch,
								channel_idx, labels_buf, labels_buf_size);
			lock_channels_visit_channel_deferred_metrics(jdr_writer, disk_name, lock_ch,
									channel_idx, labels_buf, labels_buf_size);
		}
	}
}

static void lock_channels_trace_latency(struct nvmeibc_disk *disk,
					struct nvmeibc_locks_channel *lock_ch,
					int channel_idx)
{
	NVMEIB_LOG_METRICS("@DISK_NAME lock_opr_latency channel_index=@INT " NVMESH_METRIC_LATENCY_HISTOGRAM_TFMT,
		_T, tracer_nvmeibc, info_disk_lock_channel_latency,
		disk->name, channel_idx,
		NVMESH_METRIC_LATENCY_HISTOGRAM_TARG(lock_ch->metrics.opr.latency));
}

static void lock_channels_trace_deferred_latency(struct nvmeibc_disk *disk,
						  struct nvmeibc_locks_channel *lock_ch,
						  int channel_idx)
{
	NVMEIB_LOG_METRICS("@DISK_NAME lock_opr_deferred_latency channel_index=@INT " NVMESH_METRIC_LATENCY_HISTOGRAM_TFMT,
		_T, tracer_nvmeibc, info_disk_lock_channel_deferred_latency,
		disk->name, channel_idx,
		NVMESH_METRIC_LATENCY_HISTOGRAM_TARG(lock_ch->metrics.deferred.latency));
}


static void lock_channels_do_fill(struct jdr *jdr, struct nvmeibc_disk *disk)
{
	struct list_head lock_ch_list;
	struct unique_list_ent *unique_ent;
	struct nvmeibc_locks_channel *lock_ch;
	struct nvmeib_jdr_write_closure jdr_writer;
	char labels_buf[256];
	int channel_idx = 0;
	int i;

	jdr_writer = nvmeib_jdr_write_closure_create(jdr);

	INIT_LIST_HEAD(&lock_ch_list);
	fill_lock_ch_list(disk, &lock_ch_list);

	{
		jdr_array_scope(jdr, "lock_channels");

		while ((unique_ent = list_first_entry_or_null(&lock_ch_list, struct unique_list_ent, link))) {
			lock_ch = unique_ent->ptr;

			lock_channels_visit_metrics(jdr, &jdr_writer, disk->name, lock_ch,
						    channel_idx, labels_buf, sizeof(labels_buf));
			channel_idx++;

			for (i = 0; i < lock_ch->n_2nd_ch; i++) {
				struct nvmeibc_locks_channel *_2nd_lock_ch = lock_ch->_2nd_ch[i];
				if (_2nd_lock_ch) {
					lock_channels_visit_metrics(jdr, &jdr_writer, disk->name, _2nd_lock_ch,
								    channel_idx, labels_buf, sizeof(labels_buf));
					channel_idx++;
				}
			}

			list_del(&unique_ent->link);
			kfree(unique_ent);
		}
	}
}

static void lock_channels_clear_metrics(struct nvmeibc_locks_channel *lock_ch)
{
	int i;
	
	nvmeibc_lock_ch_metrics_clear(&lock_ch->metrics);
	
	for (i = 0; i < lock_ch->n_2nd_ch; i++) {
		struct nvmeibc_locks_channel *_2nd_lock_ch = lock_ch->_2nd_ch[i];
		if (_2nd_lock_ch) {
			nvmeibc_lock_ch_metrics_clear(&_2nd_lock_ch->metrics);
		}
	}
}

static void lock_channels_reset(void *arg)
{
	struct nvmeibc_disk *disk = arg;
	struct list_head lock_ch_list;
	struct unique_list_ent *unique_ent;
	struct nvmeibc_locks_channel *lock_ch;

	INIT_LIST_HEAD(&lock_ch_list);
	fill_lock_ch_list(disk, &lock_ch_list);

	while ((unique_ent = list_first_entry_or_null(&lock_ch_list, struct unique_list_ent, link))) {
		lock_ch = unique_ent->ptr;
		lock_channels_clear_metrics(lock_ch);
		list_del(&unique_ent->link);
		kfree(unique_ent);
	}
}

static void lock_channels_proc_fill(struct jdr *jdr, void *arg)
{
	struct nvmeibc_disk *disk = arg;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = DISK_UPDATE_FILL_LOCK_CHANNELS,
		.update_data = jdr,
		.done_cb = write_status_buf_done_cb,
		.done_cb_ctx = &comp,
	};
	if (nvmeibc_disk_update_config(disk, &disk_update_data, false) < 0)
		return;
	wait_for_completion(&comp);
}

static void lock_channels_reset_dispatch(void *arg)
{
	struct nvmeibc_disk *disk = arg;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = DISK_UPDATE_RESET_LOCK_CHANNELS,
		.done_cb = write_status_buf_done_cb,
		.done_cb_ctx = &comp,
	};
	if (nvmeibc_disk_update_config(disk, &disk_update_data, false) < 0)
		return;
	wait_for_completion(&comp);
}

static ssize_t lock_channels_proc_write(void *arg, const char __user *buf, size_t count, loff_t *ppos)
{
	return nvmeib_jdr_proc_write_reset(lock_channels_reset_dispatch, arg, buf, count);
}

static int lock_ch_list_count(struct list_head *lock_ch_list)
{
	struct unique_list_ent *unique_ent;
	struct nvmeibc_locks_channel *lock_ch;
	int n = 0;
	int i;

	list_for_each_entry(unique_ent, lock_ch_list, link) {
		lock_ch = unique_ent->ptr;
		n++;
		for (i = 0; i < lock_ch->n_2nd_ch; i++) {
			if (lock_ch->_2nd_ch[i])
				n++;
		}
	}
	return n;
}

static void process_lock_channel_metrics(struct nvmeibc_disk *disk,
					  struct nvmeibc_locks_channel *lock_ch,
					  u64 *count_vals, u64 *deferred_max_vals,
					  int idx)
{
	if (count_vals)
		count_vals[idx] = lock_ch->metrics.opr.count.counter;
	if (deferred_max_vals)
		deferred_max_vals[idx] = lock_ch->metrics.deferred.queue_length_max.counter;
	if (nvmeibc_disk_lock_channel_metrics_mask & LOCK_CH_METRIC_LATENCY)
		lock_channels_trace_latency(disk, lock_ch, idx);
	if (nvmeibc_disk_lock_channel_metrics_mask & LOCK_CH_METRIC_DEFERRED_LATENCY)
		lock_channels_trace_deferred_latency(disk, lock_ch, idx);
}

static void disk_periodic_lock_channel_work_func(struct work_struct *arg)
{
	struct nvmeibc_disk *disk = container_of(to_delayed_work(arg),
						   struct nvmeibc_disk, periodic_lock_channel_work);
	struct list_head lock_ch_list;
	struct unique_list_ent *unique_ent;
	struct nvmeibc_locks_channel *lock_ch;
	const bool trace_count = nvmeibc_disk_lock_channel_metrics_mask & LOCK_CH_METRIC_COUNT;
	const bool trace_deferred_max = nvmeibc_disk_lock_channel_metrics_mask & LOCK_CH_METRIC_DEFERRED_QUEUE_MAX;
	u64 *count_vals = NULL, *deferred_max_vals = NULL;
	int n_ch, idx, i;

	__NFIND;

	INIT_LIST_HEAD(&lock_ch_list);
	fill_lock_ch_list(disk, &lock_ch_list);

	if (trace_count || trace_deferred_max) {
		n_ch = lock_ch_list_count(&lock_ch_list);
		if (trace_count) {
			count_vals = kmalloc_array(n_ch, sizeof(u64), GFP_KERNEL);
			if (!count_vals)
				goto out;
		}
		if (trace_deferred_max) {
			deferred_max_vals = kmalloc_array(n_ch, sizeof(u64), GFP_KERNEL);
			if (!deferred_max_vals)
				goto out;
		}
	}

	idx = 0;
	list_for_each_entry(unique_ent, &lock_ch_list, link) {
		lock_ch = unique_ent->ptr;

		process_lock_channel_metrics(disk, lock_ch, count_vals, deferred_max_vals, idx);
		idx++;

		for (i = 0; i < lock_ch->n_2nd_ch; i++) {
			struct nvmeibc_locks_channel *_2nd_lock_ch = lock_ch->_2nd_ch[i];
			if (!_2nd_lock_ch)
				continue;
			process_lock_channel_metrics(disk, _2nd_lock_ch, count_vals, deferred_max_vals, idx);
			idx++;
		}
	}

	if (count_vals) {
		NVMEIB_LOG_METRICS("@DISK_NAME lock_opr_count @LOCK_CH_OPR_ARR",
			_T, tracer_nvmeibc, info_disk_lock_channel_usage,
			disk->name, count_vals, idx);
	}
	if (deferred_max_vals) {
		NVMEIB_LOG_METRICS("@DISK_NAME lock_opr_deferred_queue_length_max @LOCK_CH_OPR_ARR",
			_T, tracer_nvmeibc, info_disk_lock_channel_deferred_queue_max,
			disk->name, deferred_max_vals, idx);
	}

out:
	while ((unique_ent = list_first_entry_or_null(&lock_ch_list, struct unique_list_ent, link))) {
		list_del(&unique_ent->link);
		kfree(unique_ent);
	}

	if (nvmeibc_disk_lock_channel_periodic_timer_interval > 0) {
		mod_delayed_work(system_unbound_wq, &disk->periodic_lock_channel_work,
					       msecs_to_jiffies(nvmeibc_disk_lock_channel_periodic_timer_interval));
	}

	kfree(count_vals);
	kfree(deferred_max_vals);

	__NFOUTD;
}

void nvmeibc_disk_start_io_channels_(struct nvmeibc_disk *disk)
{
	__NFIND;

	disk_start_io_channels_(disk);
	drop_old_execute_pending(disk);
	nvmeibc_jam_on_periodic(disk);
	disk_trace_iostats_on_periodic(disk);

	__NFOUTD;
}

static void start_io_channels_work(struct workqe_struct *work)
{
	struct disk_workq *rwork =
		container_of(work, struct disk_workq, work);
	struct nvmeibc_disk *disk = rwork->disk;
	struct nvmeibc_admin_channel *admin_ch = rwork->work_data;

	__NFIND;
	kfree(work);
	atomic_dec(&admin_ch->pending_start_ioch_wq);
	nvmeibc_disk_start_io_channels_(disk);
	__NFOUTD;
}

int nvmeibc_disk_start_io_channels(struct nvmeibc_disk *disk, bool high_pri)
{
	struct disk_workq *rwork = NULL;
	struct nvmeibc_admin_rnic *arnic;
	int rv = -1;

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (nvmeibc_disk_use_arnic_for_disk(arnic, disk,
			NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_MAIN_CH)) {
			if (atomic_inc_return(&arnic->channel->pending_start_ioch_wq) <= MAX_PENDING_START_IOCH_WQ) {
				if ((rwork = kzalloc(sizeof(*rwork), GFP_ATOMIC))) {
					WQ_INIT_WORK(&rwork->work, start_io_channels_work);
					rwork->disk = disk;
					rwork->work_data = arnic->channel;
					if (high_pri)
						rv = nvmeibc_admin_channel_add_work(arnic->channel, &rwork->work);
					else
						rv = nvmeibc_admin_channel_add_low_pri_work(arnic->channel, &rwork->work);
				}
				else {
					_NW(warn_disk_nvmeibc_disk_start_io_channels, "Fail to allocate memory for work request");
					rv = -ENOMEM;
				}
				if (rv < 0) {
					atomic_dec(&arnic->channel->pending_start_ioch_wq);
					goto err;
				}
			} else {
				int n_pend = atomic_dec_return(&arnic->channel->pending_start_ioch_wq);
				_NT(trace_disk_nvmeibc_disk_start_io_channels, "Maximum start_io_channels_work (@N_PEND) already on wq", n_pend);
			}
			break;
		}
	}
	goto out;

err:
	if (rwork)
		kfree(rwork);
out:
	__NFOUTD;
	return rv;
}

/* This runs on the admin channel work-queue */
static void disk_send_io_path_ka_work(struct workqe_struct *work)
{
	struct nvmeibc_disk *disk = container_of(
		work, struct nvmeibc_disk, send_io_path_ka_work);
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	struct nvmeibc_ib_nordda_channel *nrch;
	struct nvmeibc_channel *ch;
	int i, rv, n_io_ka_chs;
	u64 max_last_io_ka_jif;
	unsigned long flags;

	_NT(disk_send_io_path_ka_work_trace, "start disk_send_io_path_ka_work on @DISK_NAME, \
		 last_ka_jiffies=@JIFFIES", disk->name, disk->last_ka_jiffies);

	list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
		if (NVMEIB_UPDATE_NW_PATHS && unlikely(!rionic->may_access)) {
			_ND(disk_send_io_path_ka_work_d1,
				"No path, skip");
			continue;
		}
		if (atomic_read(&rionic->dying)) {
			_ND(disk_send_io_path_ka_work_d2,
				"Skipping dying rionic @SGID", &rionic->ib_gid);
			continue;
		}
		list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
			if (NVMEIB_UPDATE_NW_PATHS && unlikely(!lionic->may_access)) {
				_ND(disk_send_io_path_ka_work_d3,
					"No path, skip");
				continue;
			}
			if (atomic_read(&lionic->dying)) {
				_ND(disk_send_io_path_ka_work_d4,
					"Skipping dying lionic @SGID", &lionic->ib_gid);
				continue;
			}
			max_last_io_ka_jif = get_max_per_cpu64(lionic->last_io_ka_jif);
			_ND(disk_send_io_path_ka_work_d300,
			   "Last successfull IO KA on Path (@SGID -> @SGID) was "
			   "@INT_ULLONG jiffies ago",
			   &lionic->ib_gid, &rionic->ib_gid, jiffies - max_last_io_ka_jif);

			if (jiffies - max_last_io_ka_jif < ((NVMEIB_IOCH_KA_TIMEOUT_SEC * HZ) / 8))
				continue;

			/* Send Keep-alive on the path*/
			n_io_ka_chs = lionic->n_nr_qps;
			for (i = 0; i < n_io_ka_chs; i++) {
				int ch_idx = (lionic->last_ka_ch_idx + i + 1) % n_io_ka_chs;
				_ND(disk_send_io_path_ka_work_d5,
					"[@INT32_02] ch_idx=@INT", i, ch_idx);

				if (ch_idx < lionic->n_nr_qps) {
					nrch = &lionic->nr_channels[ch_idx];
					if (!nvmeibc_ib_nordda_channel_alive(nrch)) {
						_ND(disk_send_io_path_ka_work_d6,
							"skip nrch @STR(@PTR), not alive",
							nrch->base.name, nrch);
						continue;
					}
					if (nvmeibc_channel_is_ll_pcpu_ch(&nrch->base) && nvmeibc_channel_pcpu_ch_get_cpu(&nrch->base) != smp_processor_id()) {
						_ND(disk_send_io_path_ka_work_d8,
						    "skip nrch @STR(@PTR), per-cpu for different cpu [@CPU]",
						    nrch->base.name, nrch, nvmeibc_channel_pcpu_ch_get_cpu(&nrch->base));
						continue;
					}
					ch = &nrch->base;
				} else {
					continue; /* RDDA removed */
				}
				_NT(disk_send_io_path_ka_work_d69,
				   "Attempting to send keep-alive on channel @STR on path "
				   "@SGID -> @SGID",
					ch->name, &lionic->ib_gid, &rionic->ib_gid);
				nvmeibc_channel_spin_lock_irqsave(ch, &flags);
				if (ch->execute_ka)
					rv = (*ch->execute_ka)(ch);
				else
					rv = -ENOTSUPP;
				nvmeibc_channel_spin_unlock_irqrestore(ch, flags);
				if (rv) {
					_NW(disk_send_io_path_ka_work_d7,
						"Failed (@INT) sending keep-alive on channel @STR",
						rv, ch->name);
				} else {
					_NT(disk_send_io_path_ka_work_d60,
						"Sent keep-alive on channel @STR on path "
						"@SGID -> @SGID",
						ch->name, &lionic->ib_gid, &rionic->ib_gid);
					lionic->last_ka_ch_idx = ch_idx;
					break;
				}
			}
		}
	}
	atomic_set(&disk->send_io_path_work_on_q, 0);
}

static int disk_on_periodic_timer(void *arg, unsigned long t)
{
	struct nvmeibc_disk *disk = arg;
	int rv = 0;
	int connected_io_channels;

	if (t > disk->last_ka_jiffies + ((NVMEIB_IOCH_KA_TIMEOUT_SEC * HZ) / 2)) {
		struct nvmeibc_ib_admin_channel *ach = get_alive_admin_ch(disk);
		if (ach && !atomic_read(&disk->paused) &&
			atomic_cmpxchg(&disk->send_io_path_work_on_q, 0, 1) == 0) {
			WQ_INIT_WORK(&disk->send_io_path_ka_work, disk_send_io_path_ka_work);
			if (!(rv = nvmeibc_admin_channel_add_work(
					&ach->base, &disk->send_io_path_ka_work))) {
				disk->last_ka_jiffies = t;
				_NT(trace_disk_disk_on_periodic_timer_add_w,
				 "Added disk_send_io_path_ka_work for @DISK_NAME,  last_ka_jiffies=@JIFFIES", disk->name, t);
			} else {
				_NE(trace_disk_disk_on_periodic_timer, "Failed (@RV) to schedule disk_send_io_path_ka_work for disk @DISK_NAME", rv, disk->name);
				atomic_set(&disk->send_io_path_work_on_q, 0);
			}
		}
	}

	validate_slow_first_io_chan(disk);
	connected_io_channels = atomic_read(&disk->connected_io_channels);
	if (t > disk->periodic_timer + (nvmeibc_restart_io_timeout_secs * HZ) || disk->start_io_work_preempted_seqcnt || connected_io_channels == 0) {
		disk->periodic_timer = t;
		_NT(trace_1_disk_disk_on_periodic_timer,
			"add start io channels (preempted_seqcnt=@UINT, connected_io_channels=@INT)",
			disk->start_io_work_preempted_seqcnt, connected_io_channels);
		rv = nvmeibc_disk_start_io_channels(disk, false);
	}

	if (!list_empty(&disk->ioch_drained_pending_list)) {
		disk_ioch_drained_event_handler(disk,
			NVMEIBC_DISK_IOCH_DRAINED_EVENT_TIMER, NULL);
	}

	return 0;
}

static int nvmeibc_disk_start_periodic_ioch_starter(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (nvmeibc_disk_use_arnic_for_disk(arnic, disk,
			NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_MAIN_CH)) {
			/* start periodic */
			_NT(trace_disk_nvmeibc_disk_start_periodic_ioch_starter, "nvmeibc_disk_start_periodic_ioch_starter");
			disk->periodic.on_periodic_arg = disk;
			disk->periodic.on_periodic = disk_on_periodic_timer;
			disk->periodic_timer = jiffies;
			disk->start_io_work_preempted_seqcnt = 0;
			nvmeibc_admin_channel_add_periodic(arnic->channel, &disk->periodic);
			break;
		}
	}
	__NFOUTD;
	return 0;
}

static int handle_put(struct nvmeibc_disk *disk,
	struct volume_server_req *req, struct volume_client_rsp *rsp,
	int *rsp_len)
{
	struct nvmeibc_disk_info *info = disk->info;
	struct volume_server_cmd_put_req *p = &req->p_req;
	struct nvmeibc_disk_channel_rsc *r;
	int i, j, rv = 0;
	u64 rsc_num;
	unsigned id;
	int n = (int)be64_to_cpu(p->g_req.n);
	unsigned long flags;

	__NFIND;
	_NT(trace_disk_handle_put, "Disk @DISK_NAME put n=@COUNT", disk->name, n);
	*rsp_len = sizeof(rsp->p_rsp);
	spin_lock_irqsave(&disk->spinlock, flags);
	BUG_ON(disk->access_local); //local client does not need resources
	/* we do it in two passes:
	   1. sanity
	*/
	for (i = 0; i < n; ++i) {
		id = (unsigned)be64_to_cpu(p->ids[i]);
		_ND(trace_1_disk_handle_put, "Checking put id @ID_INT", id);
		if (id < info->n_rscs) {
			for (j = 0; j < info->n_rscs_sets; ++j) {
				r = &info->hcaa[j].channel_rscs[id];
				if (r->ch) {
					_NT(trace_2_disk_handle_put, "The controller gives us an in-use resource");
					rv = -1;
					goto out;
				}
			}
		}
	}
	/*
	  2. the real put
	*/
	for (i = 0; i < n; ++i) {
		rsc_num = be64_to_cpu(p->ids[i]);
		if ((rv = locate_resource_(disk, rsc_num)) < 0) {
			_NT(trace_3_disk_handle_put, "Failed to locate resources");
			break;
		}
		else
			_ND(trace_4_disk_handle_put, "Getting rsc @RSC_NUM", rsc_num);
	}

out:
	spin_unlock_irqrestore(&disk->spinlock, flags);
	nvmeibc_disk_start_io_channels_(disk);
	__NFOUTD;
	return rv;
}

static int handle_get(struct nvmeibc_disk *disk,
	struct volume_server_req *req, struct volume_client_rsp *rsp,
	int *rsp_len)
{
	struct nvmeibc_disk_info *info = disk->info;
	struct volume_client_get_rsp *g = &rsp->g_rsp;
	int rsc_num, rv = 0;
	u64 an = 0, n = be64_to_cpu(req->g_req.n);
	DECLARE_COMPLETION_ONSTACK(get_done);
	unsigned long flags;

	__NFIND;
	*rsp_len = sizeof(*g);
	g->n = 0;

	spin_lock_irqsave(&disk->spinlock, flags);

	if (n) {
		int local_resources_num;
		local_resources_num = disk->info->mine;
		_NT(trace_disk_handle_get, "@DISK_NAME get n=@COUNT have=@COUNT",
			disk->name, (int)n, local_resources_num);
		if (n > local_resources_num) {
			_NW(warn_trace_disk_handle_get,
				"@DISK_NAME reduce n to match local count",
				disk->name);
			n = local_resources_num;
		}
	}

	if (!n) {
		/*
		 * do NOT change this to `else'
		 * n can be reduced to 0 in if (n) { } block above
		 */
		_NT(trace_1_disk_handle_get, "Empty get request");
		spin_unlock_irqrestore(&disk->spinlock, flags);
		goto out;
	}

	_ND(trace_2_disk_handle_get, "disk lock");
	info->waiting_for_comp = &get_done;
	info->waiting_for = 0;

try_again:
	BUG_ON(disk->access_local); //local client does not need resources
	_NT(trace_3_disk_handle_get, "Locked");
	/* first try to get from the free list - probably nothing from here */
	while ((rsc_num = _remove_resource(disk)) != -1) {
		_NT(trace_4_disk_handle_get, "Giving rsc @RSC_NUM_INT", rsc_num);
		g->ids[an++] = cpu_to_be64(rsc_num);
		if (an == n || !info->mine) {
			_ND(trace_5_disk_handle_get, "We are done allocated=@ALLOCATED, asked=@ASKED, mine=@MINE",
				an, n, info->mine);
			g->n = cpu_to_be64(an);
			goto unlock;
		}
	}
	/* check if we are done */
	if (!g->n) {
		nvmeib_reinit_completion(&get_done);
		/* set the amount of resources we are still waiting for */
		info->waiting_for = n - an;
	}
	else {
		info->waiting_for_comp = NULL;
		info->waiting_for = 0;
	}

unlock:
	_NT(trace_9_disk_handle_get, "going to unlock");
	spin_unlock_irqrestore(&disk->spinlock, flags);
	_NT(trace_10_disk_handle_get, "disk unlock");

	/* check if we are done yet */
	if (!g->n) {
		if (unlikely(wait_for_completion_timeout(&get_done, 60 * HZ) <= 0)) {
			spin_lock_irqsave(&disk->spinlock, flags);
			if (completion_done(&get_done)) {
				_NT(trace_11_disk_handle_get, "WOW very rare case, io channel was available"
				   " at the last moment");
				goto try_again;
			} else {
				_NT(trace_12_disk_handle_get, "Time out while waiting for io channels to be available");
				info->waiting_for_comp = NULL;
				info->waiting_for = 0;
			}
			spin_unlock_irqrestore(&disk->spinlock, flags);

			/* wait failed */
			rv = -1;
		} else {
			spin_lock_irqsave(&disk->spinlock, flags);
			goto try_again;
		}
	}

out:
	spin_lock_irqsave(&disk->spinlock, flags);
	info->waiting_for_comp = NULL;
	info->waiting_for = 0;
	spin_unlock_irqrestore(&disk->spinlock, flags);
	_NT(trace_13_disk_handle_get, "Disk @DISK_NAME has n=@MINE", disk->name, info->mine);
	/* if we did well we start io_channel starter for the case there was
	   a resource_put that did not find any free io_channels since
	   the above disconnections did not finish
	*/
	if (!rv)
		nvmeibc_disk_start_io_channels(disk, false);
	__NFOUTD;
	return rv;
}

int nvmeibc_disk_handle_controller_req(struct nvmeibc_disk *disk,
	struct volume_server_req *req, struct volume_client_rsp *rsp,
	int *rsp_len)
{
	int rv;

	__NFIND;
//	mutex_lock(&disk->locate_guard);
	if (req->opcode == NVMEIBS_PUT_RSC)
		rv = handle_put(disk, req, rsp, rsp_len);
	else if (req->opcode == NVMEIBS_GET_RSC)
		rv = handle_get(disk, req, rsp, rsp_len);
	else
		rv = -1;
//	mutex_unlock(&disk->locate_guard);
	__NFOUTD;
	return rv;
}


static struct nvmeibc_channel *check_reuse(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_command *disk_cmd, void **context)
{
	struct nvmeibc_disk_io_command *block_cmd;
	struct nvmeibc_channel *ch = NULL;
	struct nvmeib_data_reuse_buf_params *rc;
	struct nvmeibc_volume_request *req;

	__NFIND;
	if (disk_cmd->cmd_type != NVMEIBC_DISK_CMD_IO || disk_cmd->server_side_only)
		goto out;
	block_cmd = disk_to_block(disk_cmd);
	rc = get_rcookie_ptr(block_cmd);
	if (rc->action == nvmeib_data_reuse_buf_SEND_REL) {
		u64 disk_version = nvmeibc_disk_version_get(disk);
		u64 channel_ver;
		bool channel_ver_valid;
		unsigned chan_ver_chng_cookie;

		if (disk_version != rc->disk_ver) {
			_NT(trace_disk_check_reuse, "Stale reuse by disk version cur=@CUR_LLONG, stored=@STORED",
				disk_version, rc->disk_ver);
			goto stale;
		}
		ch = rc->channel;
		channel_ver_valid = nvmeibc_channel_version_get_tracked(ch, &channel_ver, &chan_ver_chng_cookie);
		if (channel_ver_valid && channel_ver == rc->channel_ver && ch->comp_cpu == rc->comp_cpu) {
		req = &(c_to_inrc(ch)->reqs + rc->req_id)->req;
			if (req->reused_bb && nvmeibc_channel_try_use_req_info(ch)) {
				/* Check the version hasn't changed while we were taking the reference */
				if (nvmeibc_channel_version_tracked_changed(ch, chan_ver_chng_cookie)) {
					u64 new_ch_ver;
					bool new_ch_ver_valid = nvmeibc_channel_version_get(ch, &new_ch_ver);

					_NI(trace_2_disk_check_reuse, "Stale reuse by channel version change "
						"new_valid=@BOOL, new=@CUR_LLONG, prev=@CUR_LLONG, stored=@STORED",
						new_ch_ver_valid, new_ch_ver, channel_ver, rc->disk_ver);

					nvmeibc_channel_end_use_req_info(ch);
					goto stale;
				}

				*context = ch->ct == ct_rdda ?
				NULL : c_to_inrc(ch)->reqs + rc->req_id;

				/* own req from ulp's rcookie */
				returned_reuse_request(ch, req);
			}
			else {
				goto stale;
			}
		}
		else {
			_NT(trace_1_disk_check_reuse, "Stale reuse by channel version: "
				"valid=@BOOL, cur=@CUR_LLONG, stored=@STORED, cpu=@INT, stored_cpu=@INT",
				channel_ver_valid, channel_ver, rc->channel_ver, ch->comp_cpu, rc->comp_cpu);

stale:
			nvmeib_data_reuse_buf_zero(rc);
			ch = NULL;
		}
	}

out:
	__NFOUTD;
	return ch;
}

static bool is_reused(struct nvmeibc_disk *disk, struct nvmeibc_channel *ch,
	struct nvmeibc_disk_command *disk_cmd, void *context)
{
	struct nvmeib_data_reuse_buf_params *rc;
	struct nvmeibc_disk_io_command *block_cmd;
	bool reused = false;

	__NFIND;
	if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO &&
		!disk_cmd->server_side_only) {
		block_cmd = disk_to_block(disk_cmd);
		/* we cannot trust the version as the channel may move into release
		   while we are in execute_io
		*/
		rc = get_rcookie_ptr(block_cmd);
		if (rc->action == nvmeib_data_reuse_buf_SEND_REL) {
			reused =
				((ch->ct == ct_n_rdda) &&
				 nvmeibc_ib_nordda_channel_check_reused(
					 c_to_inrc(ch), context));
		}
	}
	__NFOUTD;
	return reused;
}

bool nvmeibc_nr_get_least_used = false;
module_param_named(nr_get_least_used, nvmeibc_nr_get_least_used, bool, 0644);
MODULE_PARM_DESC(nr_get_least_used, "Get least used nrch (preceded by nr_get_by_cpu_index)");

uint nvmeibc_nr_get_by_cpu_index = 0;
module_param_named(nr_get_by_cpu_index, nvmeibc_nr_get_by_cpu_index, uint, 0644);
MODULE_PARM_DESC(nr_get_by_cpu_index, "Get nrch based on CPU-index: 0 - False, 1 - True, Other (>=2) - True and allow fallback to other select schemes");

uint nvmeibc_nr_get_by_cpu_index_tcp = 0;
module_param_named(nr_get_by_cpu_index_tcp, nvmeibc_nr_get_by_cpu_index_tcp, uint, 0644);
MODULE_PARM_DESC(nr_get_by_cpu_index_tcp, "Get nrch based on CPU-index (for TCP): 0 - False, 1 - True, Other (>=2) - True and allow fallback to other select schemes");

#ifdef CONFIG_NUMA
uint nvmeibc_nr_get_by_numa_node = 1;
module_param_named(nr_get_by_numa_node, nvmeibc_nr_get_by_numa_node, uint, 0644);
MODULE_PARM_DESC(nr_get_by_numa_node, "Get nrch based on NUMA-node: 0 - False, 1 - True and allow fallback to other select schemes");

uint nvmeibc_nr_get_by_numa_node_tcp = 1;
module_param_named(nr_get_by_numa_node_tcp, nvmeibc_nr_get_by_numa_node_tcp, uint, 0644);
MODULE_PARM_DESC(nr_get_by_numa_node_tcp, "Get nrch based on NUMA-node (for TCP): 0 - False, 1 - True and allow fallback to other select schemes");
#endif

static struct nvmeibc_channel *nvmeibc_disk_get_channel(struct nvmeibc_disk *disk,
	int use_nrch_only, void **context, struct nvmeibc_disk_command *disk_cmd)
{
	struct nvmeibc_disk_info *info = disk->info;
	struct nvmeibc_channel *ch;
	struct nvmeibc_ib_nordda_channel *nrch;

	__NFIND;
	/* [NVMESH-6887]: Moved to execute_io_remote().
	 * if ((ch = check_reuse(disk, disk_cmd, context)))
		goto out;
	*/

	ch = NULL;
	/* check that we have free io channel to use for the command */
	if (USE_ONLY_NORDDA_FOR_IO ||
			 (USE_NORDDA_FOR_IO && info->mine <= NVMEIBC_WATERMARK_GOTO_NORDDA) ||
			 use_nrch_only) {
		*context = NULL;

		if (info->avail_norddas_per_numa_node) {
			struct plist_head *avail_norddas_this_node;
			int node = cpu_to_node(raw_smp_processor_id());

			if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
				/* For IO cmd, get the numa-node of the actual data */
				struct nvmeibc_disk_io_command *block_cmd = disk_to_block(disk_cmd);
				if (block_cmd->reqs[0].ndb->table.nents > 0) {
					struct page *page = sg_page(block_cmd->reqs[0].ndb->table.sgl);
					int sg_pg_node = page_to_nid(page);
					if (sg_pg_node != NUMA_NO_NODE)
						node = sg_pg_node;
				}
			}

			avail_norddas_this_node = &info->avail_norddas_per_numa_node[node];
			if (!plist_head_empty(avail_norddas_this_node)) {
				plist_for_each_entry(nrch, avail_norddas_this_node, per_numa_node_link) {
					_ND(trace_4_disk_nvmeibc_disk_get_channel, "nrch=@NRCH @BASE_NAME info=@INFO_PTR", 
						nrch, nrch->base.name, info);
					req_reused_bb_lru_is_timeout_stats(&nrch->base);
					if ((*context = nvmeibc_ib_nordda_channel_get_io_context(nrch))) {
						ch = &nrch->base;
						/* rotate for load balancing */
						nvmeib_public_plist_requeue(&nrch->per_numa_node_link, avail_norddas_this_node);
			
						_ND(trace_5_disk_nvmeibc_disk_get_channel, 
							"Using NORDDA channel @CH_PTR on NUMA node @NODE_ID", ch, node);
						goto found;
					}
				}
			}
		}

		if (disk->nr_get_by_cpu_index && smp_processor_id() < NVMEIB_DFLT_MAX_CPUS) {
			nrch = info->avail_nordda_for_cpu[smp_processor_id()];
			if (nrch && (*context = nvmeibc_ib_nordda_channel_get_io_context(nrch))) {
				ch = &nrch->base;
				goto found;
			}
			else if (disk->nr_get_by_cpu_index == 1) {
				/* do NOT fallback to other select schemes */
				goto out;
			}
		}

		if (nvmeibc_nr_get_least_used) {
			/* naive approach, alternatively:
			   hold per-CQ-buckets or
			   traveres first subset of fixed num of nrch each time and move it to list-tail */
			struct nvmeibc_ib_nordda_channel *min_ch = plist_first_entry_or_null(
				&info->available_norddas, struct nvmeibc_ib_nordda_channel, available_link);
			plist_for_each_entry(nrch, &info->available_norddas, available_link) {
				if (nrch->n_used_reqs < min_ch->n_used_reqs) {
					min_ch = nrch;
				}
			}
			if (min_ch) {
				if ((*context = nvmeibc_ib_nordda_channel_get_io_context(min_ch))) {
					ch = &min_ch->base;
					goto found;
				}
			}
		}
		else {
			plist_for_each_entry(nrch, &info->available_norddas, available_link) {
				_ND(trace_1_disk_nvmeibc_disk_get_channel, "nrch=@NRCH @BASE_NAME info=@INFO_PTR", nrch, nrch->base.name, info);
				req_reused_bb_lru_is_timeout_stats(&nrch->base);
				if ((*context = nvmeibc_ib_nordda_channel_get_io_context(nrch))) {
					ch = &nrch->base;
					/* rotate for load balancing */
					nvmeib_public_plist_requeue(&nrch->available_link, &info->available_norddas);

					_ND(trace_2_disk_nvmeibc_disk_get_channel, "No available IO channels, using NORDDA channel ...");
					goto found;
				}
			}
		}

		/* to be on the safe side if no channel clear the output */
		_ND(trace_3_disk_nvmeibc_disk_get_channel, "No available IO nor NORDDA channels...");
		ch = NULL;
		*context = NULL;

found:;
	}

out:
	__NFOUTD;
	return ch;
}

bool nvmeibc_nr_rotate_in_pending = false;
module_param_named(nr_rotate_in_pending, nvmeibc_nr_rotate_in_pending, bool, 0644);
MODULE_PARM_DESC(nr_rotate_in_pending, "Rotate nrch list when reusing channel for pending cmds");


struct nvmeibc_disk_command *nvmeibc_disk_get_disk_cmd_nordda(
	struct nvmeibc_disk *disk, struct nvmeibc_ib_nordda_channel *ch,
	u64 version, struct nvmeibc_volume_req_info *req,
	void (*put_req_fn)(struct nvmeibc_ib_nordda_channel *ch, struct nvmeibc_volume_req_info *req))
{
	struct nvmeibc_disk_info *info = disk->info;
	struct nvmeibc_disk_command *disk_cmd = NULL;
	unsigned long flags;
	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);

	//AAA: Remove these checks from fast-path (ported from caller)
	if (atomic_read(&ch->net.base.dying) ||
		atomic_read(&ch->base.dying) ||
		atomic_read(&disk->dying)) {
		_NT(trace_disk_nvmeibc_disk_get_disk_cmd_nordda,
			"dying: n=@INT, c=@INT, d=@INT",
			atomic_read(&ch->net.base.dying),
			atomic_read(&ch->base.dying),
			atomic_read(&disk->dying));
		goto unlock;
	}

	/* in the case we are called after reuse the save channel maybe stale */
	if (version && (!ch->base.version_valid || ch->base.version != version)) {
		_NE(trace_0_disk_nvmeibc_disk_get_disk_cmd_nordda,
			"ch=@PTR stale version: curr=@LLU vs. @LLU",
			ch, ch->base.version, version);
		goto unlock;
	}
	_ND(trace_1_disk_nvmeibc_disk_get_disk_cmd_nordda, "info=@INFO_PTR info->mine=@MINE", info, info->mine);
	/* Get IO  cmd if we dont have 'enough' rdda-chs
	   Get GEN cmd if any */
	if (USE_ONLY_NORDDA_FOR_IO ||
		(USE_NORDDA_FOR_IO && info->mine <= NVMEIBC_WATERMARK_GOTO_NORDDA) ||
		info->n_use_nrch_only > 0) {
		int i;
		if (unlikely(info->tot_pending)) {
			for (i = 0; i < DISK_PEND_PRIO_MAX; i++) {
				if ((disk_cmd = list_first_entry_or_null(&info->pending_disk_cmds[i],
						struct nvmeibc_disk_command, dcmd_link)))
					break;
			}
			BUG_ON(!disk_cmd);
			list_del_init(&disk_cmd->dcmd_link);
			info->tot_pending--;
			if (disk_cmd->cmd_type != NVMEIBC_DISK_CMD_IO ||
				disk_cmd->server_side_only)
				info->n_use_nrch_only--;
			else
				info->tot_io_pending--;
			if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK) {
				NVMEIBC_LOCK_GUARD_SWITCH_CHECK(
					nvmeibc_disk_get_disk_cmd_nordda_e7,
					&disk_to_opr(disk_to_lock(disk_cmd))->bypass_state,
					LOCK_OPR_BYPASS_IN_PENDING, LOCK_OPR_USING_BYPASS);
			}

			if (nvmeibc_nr_rotate_in_pending && !is_pcpu_nrch(ch) &&
			    !plist_node_empty(&ch->available_link)) {
				nvmeib_public_plist_requeue(&ch->available_link, &info->available_norddas);
				if (info->avail_norddas_per_numa_node)
					nvmeib_public_plist_requeue(&ch->per_numa_node_link,
						&info->avail_norddas_per_numa_node[ch->base.numa_node]);
			}
		}
		else
			_ND(trace_2_disk_nvmeibc_disk_get_disk_cmd_nordda, "info=@INFO_PTR pending_disk_cmds empty?",
				info);
	}
unlock:
	if (!disk_cmd && put_req_fn) {
		_ND(trace_3_disk_nvmeibc_disk_get_disk_cmd_nordda, "Putting req back...@IDX for info=@INFO_PTR", req->idx, disk->info);
		/* put_req_info */
		(*put_req_fn)(ch, req);
	}
	spin_unlock_irqrestore(&disk->spinlock, flags);

	__NFOUTD;
	return disk_cmd;
}

struct nvmeibc_disk_io_command * nvmeibc_disk_get_block_cmd_rdda(
	struct nvmeibc_disk *disk, struct nvmeibc_channel *ch, u32 version)
{
	struct nvmeibc_disk_io_command *block_cmd;

	__NFIND;
	/* RDDA removed - function stubbed out */
	block_cmd = NULL;
	__NFOUTD;
	return block_cmd;
}

static inline void done_local(struct nvmeibc_disk *disk)
{
	__NFIND;
	__NFOUTD;
}

static void check_local_piggyback_lock(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *block_cmd)
{
	int seg_id;
	u64 offset;
	u64 virt_index;
	u64 virt_entry;
	int i, rv;
	enum nvmeibc_disk_locks_opr opr_type;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;
	struct nvmeibc_d_rdma_comp *dc;

	__NFIND;
	if (!dp_cmds_pigbck_has_any(block_cmd)) {
		_ND(trace_disk_check_local_piggyback_lock, "No piggyback read lock");
		goto out;
	}
	dc = dp_cmds_get_pigbck_comp_dc(block_cmd);
	/* we must report that we could not read the read lock */
	dc->val[0] = -1;
	opr_type = dc->opr;
	if ((rv = nvmeibc_disk_locks_extract_info(block_cmd->lpb.handle,
		block_cmd->lpb.addr, &seg_id, opr_type, &offset)) < 0) {
		_NT(trace_1_disk_check_local_piggyback_lock, "Fail to extract read lock piggyback info");
		goto out;
	}
	/* Yaron please try to find a way to skip the search.
	   Now it is a no-issue since we only have 1 segment but if we
	   start to use more lock segment this will become an issue
	*/
	BUG_ON(!(disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
														   (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 1, .local_only = 1})));
	for (i = 0; i < disk_segs_locks->num_of_segments; ++i) {
		if (disk_segs_locks->locks[i].seg_id == seg_id) {
			virt_index = offset >> PAGE_SHIFT;
			virt_entry = offset - (virt_index << PAGE_SHIFT);
			if (opr_type == NVMEIBC_LOCK_READ) {
				union nvmeib_lock_blkset_entry lock_entry;
				lock_entry.all = *(u64 *)(page_address(disk_segs_locks->locks[i].pages[virt_index]) +
							 virt_entry);
				nvmeibc_set_lock_read(block_cmd, dc, lock_entry.all, lock_entry.lock_id.all,
										lock_entry.blkset_info.all);
				if (dc->lock.id)
					_ND(trace_200_disk_check_local_piggyback_lock, "val[0]=@_X", dc->lock.id);

			} else if (opr_type == NVMEIBC_LOCK_BLKSET_INFO_WRITE) {
				union nvmeib_lock_blkset_entry *lock_ptr =
					page_address(disk_segs_locks->locks[i].pages[virt_index]) + virt_entry;
				lock_ptr->blkset_info.all = (u32)dc->lock.bi;
			} else {
				_NE(error_disk_check_local_piggyback_lock, "Invalid lock operation @OPR_TYPE", opr_type);
			}
		}
	}
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });
	//nvmeibc_disk_cmd_piggyback_lock_read_poison_verify(block_cmd);

out:
	__NFOUTD;
}

static int on_md_rd_mod_wr_comp(struct nvmeibc_disk_io_command *block_cmd,
	int *comp_code)
{
    struct nvmeibc_disk *disk = block_cmd->disk;
	struct nvmeibc_block_io_req *req = &block_cmd->reqs[0];
    int rv = -1;
    __NFIND;

    if (!(*comp_code)) {
		if (req->req.nvme_op == nvme_cmd_read) {
			_ND(trace_disk_on_md_rd_mod_wr_comp, "MD-Trim read");
			nvmeibc_block_cmd_status_debug(block_cmd,
				NVMEIBC_BLOCK_CMD_LOCAL_MD_TRIM_RD_COMP);
			nvmeibc_disk_cmd_status_debug(&block_cmd->disk_cmd, NVMEIBC_DISK_CMD_LOCAL_MD_TRIM_RD_COMP);

			/* we've managed to read data & metadata,
			   now write data and zeros for metadata */
			req->req.nvme_op = nvme_cmd_write;
			req->req.metadata = page_address(ZERO_PAGE(0));
			/* reconstruct stuff nvme-drive destroyed */
			req->req.table.nents = req->ndb->table.orig_nents;
			req->req.disk_block = req->disk_address;
			req->req.data_len = req->ndb->length;

			if (dp_dbgdi_should_add_info_core(block_cmd)) {
				dp_dbgdi_do_add_info_core_pre(
				    req,
				    &NVMEIBC_CORE_DBGDI_PARAM(
				        pre, disk->name, ct_local,
						.start_dlba = req->disk_address,
						.io_id = 0, .ch_ptr = 0, /* local io, TODO: add special handling */
				        .lock_pgbk = &((struct t_core_dbgdi_lock_piggyback){
				            .valid = false})));
			}

			NVMEIB_LOG_GOODPATH_CORE_POST(trace_on_md_rd_mod_wr_comp,
										  disk, block_cmd,
										  NULL, 0,
										  req->req.disk_block,
										  req->req.data_len,
										  req->req.nvme_op, 0);
			if ((rv = disk->local_server->local_cmd(
				&disk->local, &req->req)) < 0) {
				_NT(trace_1_disk_on_md_rd_mod_wr_comp, "Fail to submit md-trim write");
				*comp_code = -1;
			}
			else
				/* caller won't complete block-cmd */
				rv = 0;
		}
		else
			_ND(trace_2_disk_on_md_rd_mod_wr_comp, "MD-Trim write");
    }
    else {
        _NT(trace_3_disk_on_md_rd_mod_wr_comp, "Fail md-trim @OP_STR (@NVME_OP), comp_code=@COMP_CODE",
			req->req.nvme_op == nvme_cmd_read ? "read" : "write",
			req->req.nvme_op, *comp_code);
//        _NT(trace_300_disk_on_md_rd_mod_wr_comp,
//          "Fail md-trim @STR (@INT), comp_code=@INT",
//			req->req.nvme_op == nvme_cmd_read ? "read" : "write",
//			req->req.nvme_op, *comp_code);
	}

    __NFOUTD;
    return rv;
}

static void execute_io_local_md_free_sg(struct nvmeibc_disk_io_command *block_cmd);
static void execute_io_local_md_dma_pool_return(struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *block_cmd);

static void execute_io_local_complete_bcmd_work(struct workqe_struct *work)
{
	struct nvmeibc_disk_command_workqe *workqe = container_of(work, struct nvmeibc_disk_command_workqe, work);
	struct nvmeibc_disk_command *dcmd = container_of(workqe, struct nvmeibc_disk_command, work);
	struct nvmeibc_disk_io_command *block_cmd = disk_to_block(dcmd);
	const struct nvmeib_cpu_mask cpu_mask = block_cmd->reqs[0].cpu_mask_info->mask;
	struct nvmeibc_disk *disk = block_cmd->disk;

	nvmeib_completion_noise_start(NVMEIB_NOISE_COMPLETION);
	nvmeibc_block_completion(&block_cmd->comp);
	if (atomic_dec_return(&disk->deferred_io_cnt) == 0) {
		wake_up(&disk->deferred_io_wait);
	}
	nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
								NVMEIB_NOISE_CTRS_LOCAL_IO_PCPU_WQ);
}


static void execute_io_local_cb(void *arg, int status, u32 result)
{
	struct nvmeibc_disk_io_command *block_cmd = arg;
	struct nvmeibc_d_iocmd_comp *comp = &block_cmd->comp;
	struct nvmeibc_disk *disk = block_cmd->disk;
	bool in_interrupt = in_interrupt();
	const struct nvmeib_cpu_mask cpus = block_cmd->reqs[0].cpu_mask_info->mask;
	int comp_code;
	ktime_t end_ts = ktime_get();
#if defined(TAKE_STATS)
	unsigned long flags;
#endif
	static unsigned int pcpu_cntr = 0;

	__NFIND;

	if (ktime_after(end_ts, block_cmd->disk_cmd.start_ts)) {
		u64 latency = ktime_to_ns(ktime_sub(end_ts, block_cmd->disk_cmd.start_ts));
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
		nvmeibc_disk_add_stats(disk, NULL, block_cmd->v_disk_stats, block_cmd->reqs, latency,
				       nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig));
#else
		nvmeibc_disk_add_stats(disk, NULL, NULL, block_cmd->reqs, latency,
				       nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig));
#endif
	}

#if defined(TAKE_STATS) || defined(MGMT_STATS)
	nvmeib_stats_measureq(&block_cmd->io_stat.common);
#endif
#if defined(TAKE_STATS)
	spin_lock_irqsave(&disk->stats_spinlock, flags);
	disk->local_sum_dt = ktime_add(disk->local_sum_dt, block_cmd->io_stat.common.sum_dt);
	disk->local_counts += block_cmd->io_stat.common.counts;
	spin_unlock_irqrestore(&disk->stats_spinlock, flags);
#elif defined(MGMT_STATS)
	nvmeibc_disk_add_stats(
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
		disk, NULL, block_cmd->v_disk_stats, block_cmd->reqs, nvmeib_stats_sum_dt_ns(&block_cmd->io_stat.common), nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig));
#else
		disk, NULL,					   block_cmd->reqs, nvmeib_stats_sum_dt_ns(&block_cmd->io_stat.common),
		nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig));
#endif
#endif
	if (status) {
		_NT(trace_disk_execute_io_local_cb, "@NVME_OP return status=@STATUS, result=@RESULT_INT for -"
		"Disk: @DISK_NAME, LBA: @LBA_LLONG, Length: @N_BYTES, Disk Block: @DISK_BLOCK, Data Len: @SIZE_T, SG-Count: @SGCOUNT",
		block_cmd->reqs->req.nvme_op, status, result, disk->name,
		block_cmd->reqs->disk_address, block_cmd->reqs->ndb->length,
		(unsigned long)block_cmd->reqs->req.disk_block, block_cmd->reqs->req.data_len, block_cmd->reqs->ndb->table.nents);
	}
	else if (unlikely(result)) {
		//NVNe IO commands does NOT use 'Dword 0 of the completion queue entry'
		//aka @result by our nvme-driver. There are devices that may use this
		//field e.g. aws when disk formatted to 512B. Therefore, ignore non-zero
		//@result if @status = 0.
		_NTHROTTLED_T(trace_result_disk_execute_io_local_cb,
					  NVMEIBT_THROTTLE_INTERVAL * 10, 1,
		"@NVME_OP return status=@STATUS, result=@RESULT_INT for -"
		"Disk: @DISK_NAME, LBA: @LBA_LLONG, Length: @N_BYTES, Disk Block: @DISK_BLOCK, Data Len: @SIZE_T, SG-Count: @SGCOUNT",
		block_cmd->reqs->req.nvme_op, status, result, disk->name,
		block_cmd->reqs->disk_address, block_cmd->reqs->ndb->length,
		(unsigned long)block_cmd->reqs->req.disk_block, block_cmd->reqs->req.data_len, block_cmd->reqs->ndb->table.nents);
	}
	//comp_code = result ? -1 : status;
	comp_code = status;

	if (dp_dbgdi_should_add_info_core(block_cmd))
		(void)dp_dbgdi_do_add_info_core_post(
			&block_cmd->reqs[0],
			&NVMEIBC_CORE_DBGDI_PARAM(post, disk->name, ct_local,
									  .comp_code = comp_code,
									  .start_dlba = block_cmd->reqs->disk_address));

	if (unlikely(block_cmd->reqs[0].op == NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR)) {
		if (!on_md_rd_mod_wr_comp(block_cmd, &comp_code))
			/* md-trim: read comp ok and write submitted ok */
			goto out;
	}

	if (disk->local.local_io_use_prpl && block_cmd->reqs[0].op != NVMEIB_BLOCK_IO_OP_DISCARD) {
		/* Unmap and free the prpl */
		local_io_req_free_prpl(disk, block_cmd);
	}

	if (block_cmd->reqs[0].ndb->core_sgl) {
		/* SGL allocated by core - Should be dummy area sgl created for NVMEIB_BLOCK_IO_OP_MD_READ */
		BUG_ON(block_cmd->reqs[0].op != NVMEIB_BLOCK_IO_OP_MD_READ &&
				block_cmd->reqs[0].op != NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR);
		/* Free created sgl */
		execute_io_local_md_free_sg(block_cmd);
	}

	if (block_cmd->reqs[0].ndb->md_dma_pool)
		execute_io_local_md_dma_pool_return(disk, block_cmd);
	block_cmd->reqs[0].ndb->md_dummy = 0;

	if (comp)
		comp->comp_code = comp_code;
	nvmeibc_block_cmd_status_debug(block_cmd,
		NVMEIBC_BLOCK_CMD_LOCAL_COMPLETED);
	nvmeibc_disk_cmd_status_debug(&block_cmd->disk_cmd, NVMEIBC_DISK_CMD_LOCAL_COMPLETED);

	/* check if there is a piggyback read lock */
	check_local_piggyback_lock(disk, block_cmd);

	nvmeibc_disk_cmds_stats_llp_complete(disk, &block_cmd->disk_cmd, STATS_DONE_LLP_COMPLETE_LLP_LOCAL_IO_CB, comp_code);

	if (NVMEIBC_LOCAL_DEFER_COMPLETE_IOCMD && disk->local_defer_block_cb_on_io_cmd) {
		unsigned long flags;

		spin_lock_irqsave(&disk->local_defer_io_lock, flags);
		list_add_tail(&block_cmd->disk_cmd.dcmd_link, &disk->local_defer_io_list);
		if (atomic_dec_return(&disk->local_defer_io_outstanding) == 0 && disk->local_defer_io_comp)
			complete(disk->local_defer_io_comp);
		spin_unlock_irqrestore(&disk->local_defer_io_lock, flags);

		if (nvmeib_switch_state_guard(&disk->local_defer_io_work_state,
			LOCAL_DEFER_WORK_IDLE, LOCAL_DEFER_WORK_SCHEDULED))
		{
			if (!wq_add_work(disk->local_gen_wq, &disk->local_defer_io_work)) {
				nvmeib_switch_state_guard(&disk->local_defer_io_work_state,
							  LOCAL_DEFER_WORK_SCHEDULED, LOCAL_DEFER_WORK_IDLE);
			}
		}
	} else {
		if (NVMEIBC_DISK_SHOULD_DEFER_TO_PCPU_WQ(nvmeibc_disk_local_use_system_pcpu_wq && \
			!NVMEIBC_DISK_SAFE_TEST_CURRENT_CPU_IN_BITMAP(&cpus), cpus)) {
			unsigned int resched_cpu = NVMEIBC_DISK_GET_RESCHED_CPU(cpus.cpus, pcpu_cntr);
			WQ_INIT_WORK(&block_cmd->disk_cmd.work.work, execute_io_local_complete_bcmd_work);
			atomic_inc(&disk->deferred_io_cnt);
			if (!(nvmeib_pcpu_wq_add_work_on_core(nvmeib_get_system_wq(), resched_cpu, &block_cmd->disk_cmd.work.work))) {
				WARN_ON_ONCE(1);
				_NE(fail_add_work_to_system_pcpu_wq_deferred_io, "Failed to add work to system pcpu wq");
				block_cmd->comp.comp_code = -EFAULT;
				nvmeibc_block_completion(comp);
				atomic_dec(&disk->deferred_io_cnt);
			}
		} else {
		/* update the block layer */
			nvmeibc_block_completion(comp); //LOCAL io completion
		}
		if (!in_interrupt) {
			nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, cpus.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
										NVMEIB_NOISE_CTRS_LOCAL_IO_CB);
		}
	}

	/* mark done local */
	done_local(disk);

out:
	__NFOUTD;
}

static void local_defer_io_work_fn(struct workqe_struct *work)
{
	struct nvmeibc_disk *disk = container_of(work, struct nvmeibc_disk, local_defer_io_work);
	struct nvmeibc_disk_command *dcmd;
	LIST_HEAD(defer_io_list);
	unsigned long flags;

	nvmeib_switch_state_guard(&disk->local_defer_io_work_state,
				  LOCAL_DEFER_WORK_SCHEDULED, LOCAL_DEFER_WORK_IDLE);

	spin_lock_irqsave(&disk->local_defer_io_lock, flags);
	list_splice_init(&disk->local_defer_io_list, &defer_io_list);
	spin_unlock_irqrestore(&disk->local_defer_io_lock, flags);

	while ((dcmd = list_first_entry_or_null(&defer_io_list, struct nvmeibc_disk_command, dcmd_link))) {
		struct nvmeibc_disk_io_command *block_cmd = disk_to_block(dcmd);

		list_del_init(&dcmd->dcmd_link);
		nvmeibc_block_completion(&block_cmd->comp);
	}
}

static int block_cmd_2_nvme_op_local(struct nvmeibc_disk_io_command *block_cmd,
	enum nvme_opcode *nvme_op)
{
	struct nvmeibc_block_io_req *req = &block_cmd->reqs[0];
	const enum nvmeib_block_io_op op = req->op;
	int rv = 0;
	NFIN;

	switch (op) {
	case NVMEIB_BLOCK_IO_OP_MD_READ:
	case NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR:
		if (!req->md) {
			_NE(trace_disk_block_cmd_2_nvme_op_local_no_md,
				"Missing metadata, op @BLOCK_IO_OP", op);
			WARN_ON_ONCE(1);
			rv = -1;
			goto out;
		}
		*nvme_op = nvme_cmd_read;
		break;
	case NVMEIB_BLOCK_IO_OP_READ:
		if (!req->ndb->table.sgl || !req->ndb->table.nents) {
			_NE(trace_disk_block_cmd_2_nvme_op_local_rd_no_data,
				"Missing data,op @BLOCK_IO_OP", op);
			WARN_ON_ONCE(1);
			rv = -1;
			goto out;
		}
		*nvme_op = nvme_cmd_read;
		break;
	case NVMEIB_BLOCK_IO_OP_WRITE:
		if (!req->ndb->table.sgl || !req->ndb->table.nents) {
			_NE(trace_disk_block_cmd_2_nvme_op_local_wr_no_data,
				"Missing data,op @BLOCK_IO_OP", op);
			WARN_ON_ONCE(1);
			rv = -1;
			goto out;
		}
		*nvme_op = nvme_cmd_write;
		break;

	case NVMEIB_BLOCK_IO_OP_DISCARD:
		*nvme_op = nvme_cmd_dsm;
		break;
	case NVMEIB_BLOCK_IO_OP_WRITE_UNCOR:
		*nvme_op = nvme_cmd_write_uncor;
		break;
	default:
		_NT(trace_1_disk_block_cmd_2_nvme_op_local, "Invalid op @BLOCK_IO_OP", op);
		rv = -1;
		break;
	}

out:
	NFOUT;
	return rv;
}

/*TBD: Merge this with execute_io_local_gen_work */
static void execute_io_local_lock_work(struct workqe_struct *work)
{
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd = container_of(
		work, typeof(*disk_lock_cmd), work);
	struct nvmeibc_disk *disk = disk_lock_cmd->disk;
	struct nvmeib_gen_cmd_param lock_gen_p = {
		.lock_param = disk_lock_cmd->lock_param,
	};
	union nvmeib_gen_cmd_rsp lock_gen_rsp = {};
	int rv, wq_cnt __attribute__((unused));

	wq_cnt = atomic_dec_return(&disk->local_gen_wq_cnt);

	if (atomic_read(&disk->dying))
		rv = -EBUSY;
	else {
		/* nvmeibs_handle_gen_cmd */
		rv = disk->local_server->gen_cmd(
			&disk->local, NVMEIB_GEN_OP_LOCK, &lock_gen_p, &lock_gen_rsp, disk->cid);
		disk_lock_cmd->lock_rsp = lock_gen_rsp.lock_rsp;
	}

	if (rv < 0)
		atomic64_inc(&disk->gen_cmds_cntrs_fail[NVMEIB_GEN_OP_LOCK]);
	else
		atomic64_inc(&disk->gen_cmds_cntrs_ok[NVMEIB_GEN_OP_LOCK]);
	atomic64_inc(&disk->gen_cmds_cntrs_local[NVMEIB_GEN_OP_LOCK]);

	nvmeibc_disk_cmds_stats_llp_complete(disk, &disk_lock_cmd->disk_cmd, STATS_DONE_LLP_COMPLETE_LLP_LOCAL_LOCK_WORK, rv);
	nvmeibc_locks_channel_lock_cmd_completion(  //Local disk completion
		disk_lock_cmd, rv, LOCK_OPR_BYPASS_IN_LOCAL_WQ);
}

static int execute_io_local_cmd_lock(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd)
{
	int rv, wq_cnt __attribute__((unused));

	NFIN;

	disk_lock_cmd->disk = disk;
	WQ_INIT_WORK(&disk_lock_cmd->work, execute_io_local_lock_work);
	NVMEIBC_LOCK_GUARD_SWITCH_CHECK(locks_free_opr_e7,
		&disk_to_opr(disk_lock_cmd)->bypass_state, LOCK_OPR_USING_BYPASS,
		LOCK_OPR_BYPASS_IN_LOCAL_WQ);
	wq_cnt = atomic_inc_return(&disk->local_gen_wq_cnt);
	if (!wq_add_work(disk->local_gen_wq, &disk_lock_cmd->work)) {
		wq_cnt = atomic_dec_return(&disk->local_gen_wq_cnt);
		NVMEIBC_LOCK_GUARD_SWITCH_CHECK(locks_free_opr_e8,
			&disk_to_opr(disk_lock_cmd)->bypass_state,
			LOCK_OPR_BYPASS_IN_LOCAL_WQ, LOCK_OPR_USING_BYPASS);
		rv = -1;
		goto out;
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

void nvmeibc_disk_complete_gen_cmd(union nvmeib_gen_cmd_rsp *rsp, int rv) {
	struct nvmeibc_disk_gen_cmd *gen_cmd =
		container_of(rsp, struct nvmeibc_disk_gen_cmd, rsp);
	nvmeibc_disk_cmds_stats_llp_complete(gen_cmd->disk, &gen_cmd->disk_cmd, STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_GEN_CMD, rv);
	nvmeibc_disk_gen_cmd_completion(gen_cmd, rv);
}

static void execute_io_local_gen_work(struct workqe_struct *work)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd =
		container_of(work, struct nvmeibc_disk_gen_cmd, work);
	struct nvmeibc_disk *disk = gen_cmd->disk;
	int rv, wq_cnt __attribute__((unused));

	wq_cnt = atomic_dec_return(&disk->local_gen_wq_cnt);
	if (atomic_read(&disk->dying)) {
		rv = -EBUSY;
	} else {
		switch (gen_cmd->opcode) {
		case NVMEIB_GEN_OP_BLKSET_RECOVERED:
		case NVMEIB_GEN_OP_GET_UUID_JOUR:
		case NVMEIB_GEN_OP_GET_EC_DB:
		case NVMEIB_GEN_OP_FREE_JRNL_ENTS:
		case NVMEIB_GEN_OP_GET_JMDC:
		case NVMEIB_GEN_OP_JENTRY_ERASE:
			/* nvmeibs_handle_gen_cmd */
			gen_cmd->param.async_cb = nvmeibc_disk_complete_gen_cmd;
			rv = disk->local_server->gen_cmd(&disk->local, gen_cmd->opcode, &gen_cmd->param, &gen_cmd->rsp, disk->cid);
			break;
		default:
			rv = -ENOTSUPP;
		}
	}

	if (rv != NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY) {
		if (rv) {
			_NE(execute_io_local_gen_work_e1,
					"Local @STR GEN Command Failed @IO_RSP_COMP_CODE_TO_STR (@INT)",
					nvmeib_gen_op_str(gen_cmd->opcode), io_rsp_comp_code_to_str(rv), rv);
				rv = -EIO;
		}
		nvmeibc_disk_complete_gen_cmd(&gen_cmd->rsp, rv);
	} else {
		_NT(execute_io_local_gen_work_t1,
					"Going to get async reply for gen_cmd=@CMD_PTR",
					gen_cmd);
	}
}

static inline int execute_io_local_cmd_gen(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_gen_cmd *gen_cmd)
{
	int rv;
	int wq_cnt __attribute__((unused));
	__NFIND;

	gen_cmd->disk = disk;
	gen_cmd->local_bypass = true;
	WQ_INIT_WORK(&gen_cmd->work, execute_io_local_gen_work);
	wq_cnt = atomic_inc_return(&disk->local_gen_wq_cnt);
	if (!wq_add_work(disk->local_gen_wq, &gen_cmd->work)) {
		wq_cnt = atomic_dec_return(&disk->local_gen_wq_cnt);
		rv = -1;
		goto out;
	}
	rv = 0;

out:
	__NFOUTD;
	return rv;
}

static int execute_io_local_md_alloc_sg(struct nvmeibc_disk_io_command *block_cmd)
{
	struct nvmeibc_disk *disk = block_cmd->disk;
	struct nvmeibc_block_io_req *req = &block_cmd->reqs[0];
	struct nvmeib_data_buffer *ndb = req->ndb;
	struct scatterlist *sg;
	int i, rv;
	int n_pages = DIV_ROUND_UP(ndb->length, PAGE_SIZE);
	struct page *page;
	void *rd_md_pool_ent_virt = NULL;
	dma_addr_t rd_md_pool_ent_dma = 0;

	if (disk->local.local_io_use_rd_md_pool) {
		/* TBD: Move to seperate function to improve readability */
		if (!(rd_md_pool_ent_virt = nvmeibc_percpu_dma_pool_alloc(disk->local.dma_pools, GFP_ATOMIC, &rd_md_pool_ent_dma, NVMEIB_DMA_POOL_TYPE_RD_MD, disk))) {
			/* Failed to allocate. We can continue */
			_NE(err_disk_execute_io_local_md_alloc_sg_oom_1, "Failed to allocate from rd_md_pool");
		}
	}

	if (n_pages == 1 || rd_md_pool_ent_virt) {
		/* Optimisation: Use the single sg instead of allocating an SGL */
		ndb->table.sgl = &ndb->sg_single;
		ndb->table.nents = 1;
	} else {
		/* Create SGL */
		if ((rv = sg_alloc_table(&ndb->table, n_pages, GFP_ATOMIC) < 0)) {
			_NE(err_disk_execute_io_local_md_alloc_sg_oom, "malloc error (@RV)", rv);
			goto out;
		}
	}

	BUG_ON(ndb->core_sgl);
	ndb->core_sgl = 1;

	if (rd_md_pool_ent_virt) {
		/* Use the entry from the DMA Pool.
		 * Because it's contiguous, we can use a single SGL entry and either:
		 *	- process_remote_iops (SGL mode) or
		 * 	- local_io_req_fill_prpl (PRPL mode)
		 * will paginate it for us.
		 */
		BUG_ON(ndb->length > disk->max_request_size_bytes);
		sg_set_buf(&ndb->sg_single, rd_md_pool_ent_virt, ndb->length);
		sg_dma_address(&ndb->sg_single) = rd_md_pool_ent_dma;
		sg_dma_len(&ndb->sg_single) = ndb->length;
		ndb->sg_mapped = 1;
		ndb->dma_pool = 1;
		rv = 0;
		goto out;
	}

	/* Populate SGL - Either with dummy page for MD_READ or allocated pages for RD_MOD_WR */
	for_each_sg(ndb->table.sgl, sg, ndb->table.nents, i) {
		if (req->op == NVMEIB_BLOCK_IO_OP_MD_READ)
			sg_set_buf(sg, nvmeibc_get_md_read_dummy_area(NULL), PAGE_SIZE);
		else if (req->op == NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR) {
			page = alloc_pages(GFP_ATOMIC, 0);
			if (!page) {
				_NE(err_disk_execute_io_local_md_alloc_sg_oom_2, "oom error");
				rv = -ENOMEM;
				goto out;
			}
			sg_set_page(sg, page , PAGE_SIZE, 0);
		}
		else
			BUG();
	}
	ndb->sg_mapped = 0;
	ndb->dma_pool = 0;
	rv = 0;
out:
	if (rv < 0 && rd_md_pool_ent_virt) {
		nvmeibc_percpu_dma_pool_free(disk->local.dma_pools, rd_md_pool_ent_virt, rd_md_pool_ent_dma, NVMEIB_DMA_POOL_TYPE_RD_MD, smp_processor_id());
	}
	return rv;
}

static void execute_io_local_md_free_sg(struct nvmeibc_disk_io_command *block_cmd)
{
	struct nvmeibc_block_io_req *req = &block_cmd->reqs[0];
	struct nvmeib_data_buffer *ndb = req->ndb;
	struct scatterlist *sg;
	int i;
	struct page *page;

	BUG_ON(!ndb->core_sgl);

	if (ndb->dma_pool) {
		struct nvmeibc_disk *disk = block_cmd->disk;
		BUG_ON(ndb->table.sgl == NULL);
		BUG_ON(ndb->table.nents != 1);
		nvmeibc_percpu_dma_pool_free(disk->local.dma_pools, sg_virt(&ndb->table.sgl[0]), sg_dma_address(&ndb->table.sgl[0]), NVMEIB_DMA_POOL_TYPE_RD_MD, req->submit_cpu);
	} else {
		if (req->op == NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR) {
			/* Free allocated pages */
			for_each_sg(ndb->table.sgl, sg, ndb->table.nents, i) {
				page = sg_page(sg);
				__free_pages(page, 0);
			}
		}
		else
			BUG_ON(req->op != NVMEIB_BLOCK_IO_OP_MD_READ);
	}

	if (ndb->table.sgl != &ndb->sg_single)
		sg_free_table(&ndb->table);
	ndb->table.sgl = NULL;
	ndb->table.nents = 0;
	ndb->core_sgl = 0;
	ndb->sg_mapped = 0;
	ndb->dma_pool = 0;

}

static int execute_io_local_md_dma_pool_alloc(struct nvmeibc_disk *disk, gfp_t mem_flags, struct nvmeibc_block_io_req *req)
{
	int rv;
	struct nvmeib_dma_percpu_pools *cpu_pools = this_cpu_ptr(disk->local.dma_pools);
	struct nvmeib_pool_percpu_counts __percpu *pcpu_counts = &disk->local.dma_pools->pcpu_counts[NVMEIB_DMA_POOL_TYPE_DUMMY_MD];

	BUG_ON(req->ndb->md_dma_pool);

	if (!cpu_pools->pools[NVMEIB_DMA_POOL_TYPE_DUMMY_MD].pool) {
		rv = -ENOTSUPP;
		goto out;
	}

	if (NVMEIBC_D2MD_LEN(req->ndb->length, disk) > disk->local.md_dma_pool_entry_sz) {
		this_cpu_inc(pcpu_counts->alloc_fail);
		rv = -EINVAL;
		goto out;
	}
	if (!(req->req.metadata = nvmeibc_percpu_dma_pool_alloc(disk->local.dma_pools, mem_flags, &req->req.mtdt_dma_ptr, NVMEIB_DMA_POOL_TYPE_DUMMY_MD, disk))) {
		_NE(err_disk_execute_io_local_md_dma_pool_alloc_fail, "Failed to allocate from dummy_md_pool");
		rv = -ENOMEM;
		goto out;
	}
	req->ndb->md_dma_pool = 1;
	rv = 0;

out:
	return rv;
}

static void execute_io_local_md_dma_pool_return(struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *block_cmd)
{
	/* MD was allocated from dma pool */

	if (block_cmd->reqs[0].req.use_sg) {
		BUG_ON(!block_cmd->reqs[0].req.sg_md_already_mapped);
		/* Restore mtdt_dma_ptr which might have been offset if request_size > max_request_size_bytes */
		block_cmd->reqs[0].req.mtdt_dma_ptr = block_cmd->reqs[0].req.mtdt_dma;
	}

	nvmeibc_percpu_dma_pool_free(disk->local.dma_pools, block_cmd->reqs[0].req.metadata, block_cmd->reqs[0].req.mtdt_dma_ptr, NVMEIB_DMA_POOL_TYPE_DUMMY_MD, block_cmd->reqs[0].submit_cpu);

	block_cmd->reqs[0].req.metadata = NULL;
	block_cmd->reqs[0].req.mtdt_dma_ptr = 0;
	block_cmd->reqs[0].ndb->md_dma_pool = 0;
	block_cmd->reqs[0].req.sg_md_already_mapped = 0;
}

void nvmeibc_disk_cmd_piggyback_lock_read_poison_inject(
	struct nvmeibc_disk_io_command *bcmd, u64 poison)
{
	struct nvmeibc_d_rdma_comp *c = dp_cmds_get_pigbck_comp_dc(bcmd);
	if (c && c->opr == NVMEIBC_LOCK_READ) {
		c->val[0] = poison;
		c->val[1] = poison;
	}
}

void nvmeibc_disk_cmd_piggyback_lock_read_poison_verify(
	struct nvmeibc_disk_io_command *bcmd)
{
	struct nvmeibc_d_rdma_comp *c = dp_cmds_get_pigbck_comp_dc(bcmd);

	if (c && c->opr == NVMEIBC_LOCK_READ) {
		if (nvmeibc_disk_cmd_piggyback_lock_read_poison_is_val_posioned(c->val[0]) ||
			nvmeibc_disk_cmd_piggyback_lock_read_poison_is_val_posioned(c->val[1])) {
			_NE_to_user(t_00_iicgdcc, DMESG_PD_PREFIX("@DISK_NAME"), "Unexpected internal error, crashing the operating system to prevent data corruption. Error code: 1017. Internal info {@LLX, @LLX}.",
				bcmd->disk->name, c->val[0], c->val[1]);
			BUG();
		}
	}
}

static int local_io_req_fill_prpl(struct nvmeibc_disk *disk, struct nvmeibc_block_io_req *req)
{
	struct nvmeibs_disk_info *di = disk->local.p;
	struct device *dma_dev = disk->local_server->dma_device(&disk->local);
	unsigned int prpl_idx = 0, i, j;
	size_t len_rem = req->ndb->length;
	struct scatterlist *sg;
	enum dma_data_direction dma_dir = (req->op == NVMEIB_BLOCK_IO_OP_READ ||
		req->op == NVMEIB_BLOCK_IO_OP_MD_READ) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	bool use_data_copy = disk->local.local_io_use_data_copy &&
		dma_dir == DMA_TO_DEVICE;
	
	int rv;
	if (!(req->req.buf_addrs = nvmeibc_percpu_dma_pool_alloc(disk->local.dma_pools, GFP_ATOMIC, &req->req.prpl_phys, NVMEIB_DMA_POOL_TYPE_PRPL, disk))) {
		_NE(err_disk_execute_io_local_cmd_io_err_alloc_prpl, "@DISK_STR - Failed to map prpl for req @REQ", di->disk_id, req);
		rv = -ENOMEM;
		goto out;
	}
	for_each_sg(req->ndb->table.sgl, sg, req->ndb->table.nents, i) {
		struct page *sg_pg = sg_page(sg) + (sg->offset >> PAGE_SHIFT);
		unsigned sg_n_pg = DIV_ROUND_UP(sg->length, PAGE_SIZE);
		off_t sg_pg_off = sg->offset &~PAGE_MASK;
		size_t sg_pg_len = min_t(size_t, PAGE_SIZE - sg_pg_off, len_rem);
		size_t sg_len_rem = sg->length;
		if (i == 0) {
			req->req.buf_offset = sg_pg_off;
		} else {
			/* PRPL only supports a partial page at the start or the end */
			BUG_ON(sg_pg_off != 0);
			BUG_ON(len_rem > sg->length && (sg->length &~PAGE_MASK) != 0);
		}
		for (j = 0; j < sg_n_pg && len_rem > 0 && sg_len_rem > 0; j++, sg_pg++, prpl_idx++) {
			BUG_ON(&req->req.buf_addrs[prpl_idx] >= req->req.buf_addrs + disk->local.max_prpl_sz);
			if (use_data_copy) {
				void *pool_virt;
				dma_addr_t pool_dma;
				void **virt_ptrs = (void **)((char *)req->req.buf_addrs + disk->local.max_prpl_sz);
				pool_virt = nvmeibc_percpu_dma_pool_alloc(
					disk->local.dma_pools, GFP_ATOMIC,
					&pool_dma, NVMEIB_DMA_POOL_TYPE_DATA,
					disk);
				if (!pool_virt) {
					_NE(err_disk_execute_io_local_cmd_io_data_copy_oom,
					    "@DISK_STR - Failed to alloc data copy for req @REQ",
					    di->disk_id, req);
					req->req.buf_addrs[prpl_idx] = disk->local.prpl_eof_marker;
					rv = -ENOMEM;
					goto end_map_prpl;
				}
				memcpy(pool_virt, page_address(sg_pg), PAGE_SIZE);
				req->req.buf_addrs[prpl_idx] = pool_dma;
				virt_ptrs[prpl_idx] = pool_virt;
			} else if (req->ndb->sg_mapped) {
				/* The sg has already been mapped by execute_io_local_md_alloc_sg so it is a bug if dma-address is 0.
				 * It is also a bug if the dma-address is not the top of a page */
				BUG_ON(!sg_dma_address(sg) || (sg_dma_address(sg) & ~PAGE_MASK));
				/* The dma-address that goes into this page of the prpl works as follows:
				 * - Start with the dma-address of the sg entry which has already been verified to be the top of a page
				 * - Add the full-page part of the offset (sg->offset & PAGE_MASK) to get the starting page of the sg entry with offset.
				 * - Add (j << PAGE_SHIFT) which is the number of pages into the sg entry that we are in the loop
				 * - Add sg_pg_off which is either:
				 * 	- The partial-page part of the offset (i == 0 and j == 0)
				 * 	- 0 (i > 0 or j > 0)
				 */
				req->req.buf_addrs[prpl_idx] = sg_dma_address(sg) + (sg->offset & PAGE_MASK) + (j << PAGE_SHIFT) + sg_pg_off;
			} else {
				req->req.buf_addrs[prpl_idx] = dma_map_page(dma_dev,
									sg_pg, 0, sg_pg_len, dma_dir);
				if (dma_mapping_error(dma_dev, req->req.buf_addrs[prpl_idx])) {
					_NE(err_disk_execute_io_local_cmd_io_dma_map_err,
					"@DISK_STR - Req @REQ Error DMA mapping SGL to PRPL",  di->disk_id, req);
					req->req.buf_addrs[prpl_idx] = disk->local.prpl_eof_marker;
					rv = -EFAULT;
					goto end_map_prpl;
				}
			}
			BUG_ON(sg_pg_len > len_rem);
			len_rem -= sg_pg_len;
			sg_len_rem -= sg_pg_len;
			sg_pg_len = min3((size_t)PAGE_SIZE, len_rem, sg_len_rem);
			sg_pg_off = 0;
		}
		if (len_rem == 0)
			break;
	}
	BUG_ON(len_rem != 0);
	if (use_data_copy)
		req->ndb->data_copy = 1;
	/* Mark the end of the PRPL */
	if (&req->req.buf_addrs[prpl_idx] < req->req.buf_addrs + disk->local.max_prpl_sz)
		req->req.buf_addrs[prpl_idx] = disk->local.prpl_eof_marker;
	if (req->req.metadata && !disk->md_extd && !req->req.mtdt_dma_ptr) {
		/* DMA MAP the MD */
		req->req.mtdt_dma_ptr = dma_map_single(dma_dev, req->req.metadata,
						       req->req.mtdt_size,
					 dma_dir);
		if (dma_mapping_error(dma_dev, req->req.mtdt_dma_ptr)) {
			_NE(err_err_disk_execute_io_local_cmd_io_dma_map_md_err,
			    "@DISK_STR - Req @REQ Error DMA mapping MD",  di->disk_id, req);
			req->req.mtdt_dma_ptr = 0;
			rv = -EFAULT;
			goto end_map_prpl;
		}
	}
	rv = 0;
	goto out;

end_map_prpl:
	/* Unwind */
	if (use_data_copy) {
		int submit_cpu = smp_processor_id();
		int idx;
		void **virt_ptrs = (void **)((char *)req->req.buf_addrs + disk->local.max_prpl_sz);
		for (idx = (int)prpl_idx - 1; idx >= 0; idx--) {
			if (req->req.buf_addrs[idx] != disk->local.prpl_eof_marker) {
				nvmeibc_percpu_dma_pool_free(disk->local.dma_pools,
					virt_ptrs[idx],
					req->req.buf_addrs[idx],
					NVMEIB_DMA_POOL_TYPE_DATA, submit_cpu);
			}
		}
	} else if (!req->ndb->sg_mapped) {
		for (prpl_idx--; prpl_idx > 0; prpl_idx--)
			dma_unmap_page(dma_dev, req->req.buf_addrs[prpl_idx], PAGE_SIZE, dma_dir);
	}
	nvmeibc_percpu_dma_pool_free(disk->local.dma_pools, req->req.buf_addrs, req->req.prpl_phys, NVMEIB_DMA_POOL_TYPE_PRPL, smp_processor_id());
	req->req.buf_addrs = NULL;
	req->req.prpl_phys = 0;

out:
	return rv;
}

static void local_io_req_free_prpl(struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *block_cmd)
{
	struct device *dma_dev = disk->local_server->dma_device(&disk->local);
	struct nvmeibs_nvme_req *nvme_req = &block_cmd->reqs[0].req;
	enum dma_data_direction dma_dir = (block_cmd->reqs[0].op == NVMEIB_BLOCK_IO_OP_READ ||
	block_cmd->reqs[0].op == NVMEIB_BLOCK_IO_OP_MD_READ) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	int i;

	__NFIND;
	if (nvme_req->mtdt_dma_ptr && !block_cmd->reqs[0].ndb->md_dma_pool && !block_cmd->reqs[0].ndb->md_dummy) {
		dma_unmap_single(dma_dev, nvme_req->mtdt_dma_ptr, nvme_req->mtdt_size, dma_dir);
		nvme_req->mtdt_dma_ptr = 0;
	}

	if (block_cmd->reqs[0].ndb->data_copy) {
		int submit_cpu = block_cmd->reqs[0].submit_cpu;
		void **virt_ptrs = (void **)((char *)nvme_req->buf_addrs + disk->local.max_prpl_sz);
		for (i = 0; &nvme_req->buf_addrs[i] < nvme_req->buf_addrs + disk->local.max_prpl_sz &&
			nvme_req->buf_addrs[i] != disk->local.prpl_eof_marker; i++) {
			nvmeibc_percpu_dma_pool_free(disk->local.dma_pools,
				virt_ptrs[i],
				nvme_req->buf_addrs[i],
				NVMEIB_DMA_POOL_TYPE_DATA, submit_cpu);
		}
		block_cmd->reqs[0].ndb->data_copy = 0;
	} else if (!block_cmd->reqs[0].ndb->sg_mapped) {
		for (i = 0; &nvme_req->buf_addrs[i] < nvme_req->buf_addrs + disk->local.max_prpl_sz &&
			nvme_req->buf_addrs[i] != disk->local.prpl_eof_marker; i++) {
			dma_unmap_page(dma_dev, nvme_req->buf_addrs[i], PAGE_SIZE, dma_dir);
		}
	}

	nvmeibc_percpu_dma_pool_free(disk->local.dma_pools, nvme_req->buf_addrs, nvme_req->prpl_phys, NVMEIB_DMA_POOL_TYPE_PRPL, block_cmd->reqs[0].submit_cpu);
	nvme_req->buf_addrs = NULL;
	nvme_req->prpl_phys = 0;

	__NFOUTD;
}

static inline int execute_io_local_cmd_io(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *block_cmd)
{
	struct nvmeibc_block_io_req *req = &block_cmd->reqs[0];
	struct nvmeibs_disk_info *di = disk->local.p;
	enum nvme_opcode nvme_op = -1;
	int rv;
	__NFIND;

	if (block_cmd_2_nvme_op_local(block_cmd, &nvme_op) < 0) {
		rv = -EINVAL;
		goto out;
	}

	if (req->md) {
		if (!IS_ALIGNED((unsigned long)req->md, 8)) {
			/* Metadata pointer (see MPTR), at worse, shall be QWord aligned */
			_NW(warn_execute_io_local_cmd_io_md_not_aligned,
				"NVME Op: @NVME_OP (block_cmd: @BLOCK_COMMAND) metadata ptr @MD_PTR is not QWORD aligned",
				nvme_op, block_cmd, req->md);
		}
		if (((unsigned long)req->md & PAGE_MASK) != (((unsigned long)req->md + NVMEIBC_D2MD_LEN(req->ndb->length, disk) - 1) & PAGE_MASK)) {
			_NE(err_execute_io_local_cmd_io_md_crosses_page,
				"NVME Op: @NVME_OP (block_cmd: @BLOCK_COMMAND) metadata @MD_PTR - @MD_PTR crosses a page boundary",
				nvme_op, block_cmd, req->md, req->md + NVMEIBC_D2MD_LEN(req->ndb->length, disk));
			rv = -EINVAL;
			goto out;
		}
	}

	/* Check for empty ndb SGL */
	if (!req->ndb->table.sgl) {
		BUG_ON(req->ndb->table.nents != 0);
		BUG_ON(req->op != NVMEIB_BLOCK_IO_OP_MD_READ &&
			   req->op != NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR);
		if ((rv = execute_io_local_md_alloc_sg(block_cmd)) < 0)
			goto out;
	} else {
		BUG_ON(req->ndb->core_sgl);
	}

	nvmeibc_disk_cmd_piggyback_lock_read_poison_inject(
		block_cmd, NVMEIBC_DISK_CMD_PBLR_POISON_LOCAL);

	/* fill the disk command */
	req->req.nvme_op = nvme_op;

	/* Check if we need to decode the lba */
	if (get_rcookie_ptr(block_cmd)->lba_jam_enc)
		req->req.disk_block = nvmeibc_jam_decode_lba(req->disk_address);
	else
		req->req.disk_block = req->disk_address;
	req->req.data_len = req->ndb->length;

	if (req->do_512b_sub_block_x) {
		req->req.use_hw_blocks = 1;
		req->req.disk_block = nvmeib_translate_sw_addr_to_subblock_addr(
			req->req.disk_block,
			NVMEIBC_SECTOR_SHIFT,
			disk->sector_shift,
			do_512b_sub_block_x_val(req->do_512b_sub_block_x)
		);
	}

	req->req.cb = execute_io_local_cb;
	req->req.arg = block_cmd;
	req->req.metadata = req->md;
	req->req.mtdt_size = req->req.metadata ?
		NVMEIBC_D2MD_LEN(req->ndb->length, disk) : 0;
	req->req.mtdt_dma_ptr = 0;
	req->req.stats.is_recovery = nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig);

	if (disk->md_size > 0 && !disk->md_extd && !req->req.metadata) {
		/* Drive needs metadata and the block didn't supply any */
		switch (req->op) {
		case NVMEIB_BLOCK_IO_OP_MD_READ:
			_NW(warn_disk_execute_io_local_cmd_io_md_rd_no_md,
				"@DISK_STR - NVMEIB_BLOCK_IO_OP_MD_READ (@OP) with NULL metadata",
				di->disk_id, NVMEIB_BLOCK_IO_OP_MD_READ);
		FALLTHRU;
		case NVMEIB_BLOCK_IO_OP_READ:
			if (disk->access_local) {
				if (!disk->local.local_io_use_md_dma_pool ||
					execute_io_local_md_dma_pool_alloc(disk, GFP_ATOMIC, req) < 0)
				{
					req->ndb->md_dummy = 1;
					req->req.metadata = disk->local.dummy_md_read_ptr;
					req->req.mtdt_dma_ptr = disk->local.dummy_md_read_addr;
				}
			} else {
				req->req.metadata = nvmeibc_get_md_read_dummy_area(NULL);
			}
			req->req.mtdt_size = NVMEIBC_D2MD_LEN(req->ndb->length, disk);
			break;
		case NVMEIB_BLOCK_IO_OP_WRITE:
		case NVMEIB_BLOCK_IO_OP_WRITE_UNCOR:
			/* For write, we supply the zero-page */
			if (disk->access_local) {
				if (!disk->local.local_io_use_md_dma_pool ||
					execute_io_local_md_dma_pool_alloc(disk, GFP_ATOMIC, req) < 0)
				{
					req->ndb->md_dummy = 1;
					req->req.metadata = disk->local.dummy_md_write_ptr;
					req->req.mtdt_dma_ptr = disk->local.dummy_md_write_addr;
				}
			} else {
				req->req.metadata = nvmeibc_get_md_write_dummy_area(NULL);
			}
			req->req.mtdt_size = NVMEIBC_D2MD_LEN(req->ndb->length, disk);
			break;
		default:
			break;
		}
	}

	if (!disk->local.local_io_use_prpl || block_cmd->reqs[0].op == NVMEIB_BLOCK_IO_OP_DISCARD) {
		req->req.use_sg = true;
		req->req.table = req->ndb->table;
		req->req.sg_already_mapped = req->ndb->sg_mapped;
		if (req->ndb->md_dma_pool || req->ndb->md_dummy) {
			/* Tell s_nvme not to map the metadata because we've already mapped it.
			 * If the size of the request > max_request_size, the mtdt_dma_ptr might be offset so we store the original in mtdt_dma
			 */
			req->req.sg_md_already_mapped = true;
			req->req.mtdt_dma = req->req.mtdt_dma_ptr;
		}

		if (block_cmd->reqs[0].op == NVMEIB_BLOCK_IO_OP_DISCARD) {
			/* [NVMESH-2935]: Set the number of LBAs for discard in nvme request for drive stats */
			req->req.n_dsm_lba = le32_to_cpu(block_cmd->reqs[0].trim->nlb);
		}
	} else {
		req->req.use_sg = false;
		if ((rv = local_io_req_fill_prpl(disk, req)) < 0)
			goto out;
	}

	_ND(trace_disk_execute_io_local_cmd_io, "io_cmd: disk=@DISK_STR, nvme_op=@NVME_OP, buf_addr=@BUF_ADDR, prpl_phys=@PRPL_PHYS, buf_offset=@BUF_OFFSET, "
		"disk_block=@DISK_BLOCK, data_len=@DATA_LEN_LONG, metadata=@METADATA, mtdt_size=@MTDT_SIZE",
	   di->disk_id, req->req.nvme_op, req->req.buf_addrs, req->req.prpl_phys,
	   req->req.buf_offset, (unsigned long)req->req.disk_block, req->req.data_len,
	   req->req.metadata, req->req.mtdt_size);

#if defined(TAKE_STATS) || defined(MGMT_STATS)
	nvmeib_stats_init(&block_cmd->io_stat.common);
	nvmeib_stats_set_start(&block_cmd->io_stat.common);
#endif
	if (nvmeibc_disk_is_bcmd_jour_write(req)) {
		struct nvmeib_io_piggyb_cmd piggyb_cmd = {
			.opcode = NVMEIB_IO_PIGGYB_JMDC_WRITE,
			.jmdc_wr = {
				.rng_gen_id = req->jam_op.rng_gen_id,
				.rng_idx = req->jam_op.rng_idx,
			},
		};
		int i;
		u32 sw2hw = NVMEIBC_SECTOR_SHIFT - block_cmd->disk->sector_shift;

		for (i = 0; i < req->jam_op.n_ops; i++) {
			piggyb_cmd.jmdc_wr.ent_idx = req->jam_op.jour_idx[i];
			piggyb_cmd.jmdc_wr.ent_md = req->jam_op.ent_md[i];
			/* copy the jblks-md from ulp and poison the rest of jmdc-entry's */
			nvmeibc_jentry_md_container_fill(
				&piggyb_cmd.jmdc_wr.jmdc_val, &req->jam_op.jmdc_ent[i], sw2hw,
				NVMEIBC_DCMD_LEN_TO_SW_SECTORS(block_cmd), disk->jour.rng_binje);
		}
		rv = disk->local_server->io_piggyb_cmd(&disk->local, &piggyb_cmd);
		if (rv < 0) {
			_NE(error_disk_execute_io_local_cmd_io_pb,
				"FAILED local bypass piggyback JMDC Write (@RV) - "
				"Range: @JRNL_RNG_IDX Range GenID: @JRNL_RNG_GEN_ID, "
				"Entry: @JRNL_RNG_ENT_IDX Entry GenID: @JRNL_ENT_GEN_ID, "
				"JMDC Value: @JMDC_ENT", rv, piggyb_cmd.jmdc_wr.rng_idx,
				piggyb_cmd.jmdc_wr.rng_gen_id, piggyb_cmd.jmdc_wr.ent_idx,
				piggyb_cmd.jmdc_wr.ent_md.ent_gen_id, piggyb_cmd.jmdc_wr.jmdc_val.jblks_md[0].raw);
			goto out;
		}
	}

	if (dp_dbgdi_should_add_info_core(block_cmd)) {
		dp_dbgdi_do_add_info_core_pre(
		    req,
		    &NVMEIBC_CORE_DBGDI_PARAM(
		        pre, disk->name, ct_local,
				.start_dlba = req->disk_address,
				.io_id = 0, .ch_ptr = 0, /* local io, TODO: add special handling */
		        .lock_pgbk =
		            &((struct t_core_dbgdi_lock_piggyback){.valid = false})));
	}

	NVMEIB_LOG_GOODPATH_CORE_POST(trace_execute_io_local_cmd_io,
								  disk, block_cmd,
								  NULL, 0,
								  req->req.disk_block,
								  req->req.data_len,
								  req->req.nvme_op, 0);

	if (NVMEIBC_LOCAL_DEFER_COMPLETE_IOCMD && disk->local_defer_block_cb_on_io_cmd) {
		atomic_inc(&disk->local_defer_io_outstanding);
	}

	req->submit_cpu = smp_processor_id();
	rv = disk->local_server->local_cmd(&disk->local, &req->req);
	if (rv < 0) {
		_NE(error_disk_execute_io_local_cmd_io, "FAILED io req (@RV) - @BLOCK_COMMAND: disk=@DISK_STR, nvme_op=@NVME_OP, buf_addr=@BUF_ADDR, prpl_phys=@PRPL_PHYS, buf_offset=@BUF_OFFSET, "
		"disk_block=@DISK_BLOCK, data_len=@DATA_LEN_LONG, metadata=@METADATA, mtdt_size=@MTDT_SIZE",
		rv, block_cmd, di->disk_id, req->req.nvme_op, req->req.buf_addrs, req->req.prpl_phys,
		req->req.buf_offset, (unsigned long)req->req.disk_block, req->req.data_len,
		req->req.metadata, req->req.mtdt_size);

		if (NVMEIBC_LOCAL_DEFER_COMPLETE_IOCMD && disk->local_defer_block_cb_on_io_cmd) {
			atomic_dec(&disk->local_defer_io_outstanding);
		}
	}

out:
	if (rv < 0) {
		if (disk->local.local_io_use_prpl && block_cmd->reqs[0].op != NVMEIB_BLOCK_IO_OP_DISCARD) {
			/* Free allocated prpl */
			local_io_req_free_prpl(disk, block_cmd);
		}
		if (req->ndb->core_sgl) {
			/* Free created sgl */
			execute_io_local_md_free_sg(block_cmd);
		}
		if (req->ndb->md_dma_pool) {
			/* Return dummy MD buffer to pool */
			execute_io_local_md_dma_pool_return(disk, block_cmd);
		}
		req->ndb->md_dummy = 0;
	}
	__NFOUTD;
	return rv;
}

static int execute_io_local(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_command *disk_cmd)
{
	int dying, rv;
	__NFIND;

	nvmeibc_disk_cmds_stats_done_reset(disk_cmd);
	nvmeibc_disk_cmds_stats_direct_exec_start(disk, disk_cmd);

	if ((dying = atomic_read(&disk->dying))) {
		_NT(trace_disk_execute_io_local, "Disk is dying - leave");
		rv = -1;
		goto out;
	}

	disk_cmd->local_cmd = true;
	switch (disk_cmd->cmd_type) {
	case NVMEIBC_DISK_CMD_IO:
		rv = execute_io_local_cmd_io(disk, disk_to_block(disk_cmd));
		break;
	case NVMEIBC_DISK_CMD_GEN:
		rv = execute_io_local_cmd_gen(disk, disk_to_gen(disk_cmd));
		break;
	case NVMEIBC_DISK_CMD_LOCK:
		rv = execute_io_local_cmd_lock(disk, disk_to_lock(disk_cmd));
		break;
	default:
		rv = -1;
		break;
	}

out:
	if (unlikely(rv))
		nvmeibc_disk_cmds_stats_direct_exec_err(disk, disk_cmd, rv);

	__NFOUTD;
	return rv;
}

static int disk_cmd_pend_prio(struct nvmeibc_disk *disk, struct nvmeibc_disk_command *disk_cmd)
{
	if (likely(disk->prio_pending)) {
		if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_GEN)
			return DISK_PEND_PRIO_GEN_CMDS;
		else if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK)
			return DISK_PEND_PRIO_RPC_LOCKS;
		else {
			struct nvmeibc_disk_io_command *block_cmd = disk_to_block(disk_cmd);
			const enum nvmeib_block_io_op op = block_cmd->reqs[0].op;

			if (op & NVMEIB_BLOCK_IO_OP_RECOVER_STALE)
				return DISK_PEND_PRIO_IO_RECOV;
			else if (nvmeib_block_io_op_is_write(op)) {
				struct nvmeib_data_reuse_buf_params *rcookie = get_rcookie_ptr(block_cmd);
				if (rcookie->action == nvmeib_data_reuse_buf_SAVE)
					return DISK_PEND_PRIO_EC_JRNL_WRITE;
				else if (rcookie->action == nvmeib_data_reuse_buf_SEND_REL)
					return DISK_PEND_PRIO_EC_DATA_WRITE;
				else
					return DISK_PEND_PRIO_NON_EC_WRITE;
			}
		}
		return DISK_PEND_PRIO_OTHER_IO;
	}
	return 0;
}

static bool pcpu_nrch_coremask_check_reuse(struct nvmeibc_disk *disk,
					   struct nvmeibc_disk_command *disk_cmd,
					   struct nvmeibc_channel *ch,
					   void *context)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *cinfo = dinfo->coremask_info;
	const struct nvmeib_cpu_mask_info *cmd_coremask_info = disk_cmd->cpu_mask_info;
	struct nvmeibc_disk_coremask_chs *coremask_chs = nvmeibc_channel_get_coremask_ch_cookie(ch);
	struct nvmeibc_disk_coremask_pcpu_stats *disk_pcpu_stats;
	unsigned long flags;
	bool ret;

	__NFIND;

	/* NOTE: Channel version is invalidated before the cookie is cleared */
	BUG_ON(!coremask_chs);

	spin_lock_irqsave(&coremask_chs->spinlock, flags);
	disk_pcpu_stats = this_cpu_ptr(cinfo->pcpu_stats);

	if (coremask_chs->dying) {
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_reuse_io_coremask_dying);
		coremask_chs->n_reuse_io_mask_dying++;

		ret = false;
		goto unlock;
	}

	if (coremask_chs->uid != cmd_coremask_info->gen) {
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_reuse_io_coremask_uid_mismatch);
		coremask_chs->n_reuse_io_mask_uid_mismatch++;

		ret = false;
		goto unlock;
	}

	if (!NVMEIB_CPU_MASK_TEST_CPU(smp_processor_id(), cmd_coremask_info->mask))
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_reuse_io_coremask_submit_cpu_not_in_mask);

	COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_reuse_io_coremask_op);
	coremask_chs->n_reuse_io_mask_chan++;

	ret = true;

unlock:
	spin_unlock_irqrestore(&coremask_chs->spinlock, flags);

	__NFOUTD;
	return ret;
}

static bool pcpu_nrch_check_reuse(struct nvmeibc_disk *disk,
				struct nvmeibc_disk_command *disk_cmd,
				struct nvmeibc_channel *ch,
				void *context)
{
	int cpu = smp_processor_id();
	int ch_cpu = nvmeibc_channel_pcpu_ch_get_cpu(ch);
	bool ret;

	__NFIND;
	if (disk->coremask_support) {
		ret = pcpu_nrch_coremask_check_reuse(disk, disk_cmd, ch, context);
		goto out;
	}

	if (!disk->pcpu_nrchs_ll || ch_cpu == cpu) {
		/* per-cpu channel without lock-less or lock-less and we are on correct cpu, 
		 * ==> no special handling */
		ret = true;
		goto out;
	}

	/* Lock-less per-cpu channel and we are on wrong CPU. Clear the reuse */
	_NW(trace_1_disk_execute_io_remote, 
	    "Reuse for channel @CH_NAME came on wrong CPU @CPU (instead of @CPU) ",
	    ch->name, cpu, ch_cpu);

	WARN_ON_ONCE(1);
	ret = false;

out:
	__NFOUTD;
	return ret;
}

static struct nvmeibc_channel *execute_io_remote_check_reuse(struct nvmeibc_disk *disk,
					 struct nvmeibc_disk_command *disk_cmd, void **context)
{
	struct nvmeibc_channel *ch = NULL;
	__NFIND;

	if (!(ch = check_reuse(disk, disk_cmd, context))) {
		/* Either no reuse or reuse stale */
		goto out;
	}

	if (!nvmeibc_channel_is_pcpu_ch(ch)) {
		/* any-core channel, no special handling */
		goto out;
	}

	BUG_ON(!disk->pcpu_nrchs);

	if (!pcpu_nrch_check_reuse(disk, disk_cmd, ch, context)) {
		/* per-cpu reuse failed - clear reuse and process pending */
		struct nvmeibc_disk_io_command *block_cmd = disk_to_block(disk_cmd);
		struct nvmeib_data_reuse_buf_params rc_stack, *rc = get_rcookie_ptr(block_cmd);

		rc_stack = *rc;
		nvmeib_data_reuse_buf_zero(rc);

		/* Copied from nvmeibc_disk_reused_bb_release */
		if (nvmeibc_channel_try_use_req_info(ch)) {
			void *pend_ctx = NULL;
			bool do_pending = false;

			if (ch->ct == ct_n_rdda) {
				nvmeibc_ib_nordda_channel_reused_context(c_to_inrc(ch), &rc_stack, &pend_ctx);
				do_pending = !!pend_ctx;
				/* if !@pend_ctx, nvmeibc_channel-end_use_req_info was called */
			}
			else {
				_NW(warn_execute_io_remote_check_reuse,
				    "Unexpected ch=@PTR, ct=@INT", ch, (int)ch->ct);
				BUG();
			}

			if (do_pending) {
				/* rdda_pending_io
				 *   nordda_pending_io */
				ch->execute_pending_io(disk, ch, pend_ctx, false, rc_stack.channel_ver);
			}
		}

		ch = NULL;
		*context = NULL;
	}

out:
	__NFOUTD;
	return ch;
}

static int execute_io_remote(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_command *disk_cmd)
{
	struct nvmeibc_disk_info *info = disk->info;
	int use_nrch_only =
		((disk_cmd->cmd_type != NVMEIBC_DISK_CMD_IO) ||
		 (disk_cmd->server_side_only == true)) ? 1 : 0;
	struct nvmeibc_channel *ch = NULL;
	void *context = NULL;
	int rv = 0;
	unsigned long flags;

	__NFIND;
	nvmeibc_disk_cmds_stats_done_reset(disk_cmd);

	if (atomic_read(&disk->dying)) {
		rv = -1;
		_NT(trace_disk_execute_io_remote, "Disk is dying - leave");
		nvmeibc_disk_cmds_stats_direct_exec_err(disk, disk_cmd, rv);
		nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_DISK_DYING);
		goto out;
	}

	/* [NVMESH-6887]: check reuse before splitting to per-cpu, coremask, any-core flow 
	 * 
	 * NOTE: As part of the change, disk version and channel version are now
	 * synchronised without needing to take the disk spinlock on read.
	 */
	if ((ch = execute_io_remote_check_reuse(disk, disk_cmd, &context))) {
		/* Reuse valid and channel found */
		goto if_ch;
	}

	/* Try to get pcpu nrch w/o taking disk spinlock,
	   on failure fallback to using non-pcpu channel */
	if (disk->pcpu_nrchs &&
		!pcpu_nrch_get_channel(disk, disk_cmd, &ch, &context))
		goto if_ch;

	/*
	 * Try to get io-channel or add to pending-list
	 */
	spin_lock_irqsave(&disk->spinlock, flags);
	if ((ch = nvmeibc_disk_get_channel(
		disk, use_nrch_only, &context, disk_cmd))) {
		if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO)
			nvmeibc_block_cmd_status_debug(block_cmd,
				NVMEIBC_BLOCK_CMD_REMOTE_GOT_CH);
		nvmeibc_disk_cmd_status_debug_got_remote_ch(disk_cmd, ch, context);
	}
	else if (true) {
		int pend_prio = disk_cmd_pend_prio(disk, disk_cmd);
		nvmeibc_disk_cmds_stats_pending_add(disk, disk_cmd);
		nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_REMOTE_ADD_PENDING);
		list_add_tail(&disk_cmd->dcmd_link, &info->pending_disk_cmds[pend_prio]);
		info->tot_pending++;
		info->tot_io_pending += !use_nrch_only;
		info->n_use_nrch_only += use_nrch_only;
		rv = 0;
#ifdef DEBUG_UNCOMPLETED
		if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
			nvmeibc_block_cmd_status_debug(block_cmd,
				NVMEIBC_BLOCK_CMD_REMOTE_ADD_PENDING);
		}
#endif
		if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK) {
			NVMEIBC_LOCK_GUARD_SWITCH_CHECK(execute_io_remote_e1,
				&disk_to_opr(disk_to_lock(disk_cmd))->bypass_state,
				LOCK_OPR_USING_BYPASS, LOCK_OPR_BYPASS_IN_PENDING);
		}
	} else
		rv = -ENOMEM;
	spin_unlock_irqrestore(&disk->spinlock, flags);

if_ch:
	if (!ch)
		goto out;

	/*
	 * Execute IO
	 * ---
	 * RDDA   : rdda_execute_io
	 * No-RDDA: nordda_execute_io
	 */

	nvmeibc_disk_cmds_stats_direct_exec_start(disk, disk_cmd);
	rv = ch->execute_io(ch, disk, disk_cmd, context);
	if (unlikely(rv)) {
		 _NT(t_01_disk_exec_io_remote, "I/O execution failed. err:@RV\n", rv);
		nvmeibc_disk_cmds_stats_direct_exec_err(disk, disk_cmd, rv);
		if (!is_reused(disk, ch, disk_cmd, context)) {
			/* In case disk has *no* IO in-progress now, there won't be any
			   IO-completion(s) to serve the pending list.  This can happen
			   if the (entire) pending-list had built-up while the disk had
			   only one io-channel available, this io-channel we're holding.
			   As long as we dont keep this information(under lock) we must
			   Try process pending cmd until first successfull or drain all.
			   ---
			   RDDA   : rdda_pending_io
			   No-RDDA: nordda_pending_io
			 */
			int rv_p;
			rv_p = ch->execute_pending_io(disk, ch, context, false, 0);
			if (rv_p)
				 _NT(t_02_disk_exec_io_remote, "Pending I/O execution failed. pending err:@RV\n", rv_p);

			/* if failed, ch had failed to execute all pending cmds or
			   it is no longer valid (dying) */
		}
		else {
			/* Take care of accounting of used reqs. This way, in case ch
			   disconnects it won't have to wait for ulp to return reused
			   resource i.e. call nvmeibc_disk-ec_reuse_buf_del(). If ulp
			   returns the resource while/after ch disconnets/ed, it wont
			   be actually returned as the channel version will mistmatch
			   because it is gets updated when ch-disconnect begins */
			nvmeibc_channel_end_use_req_info(ch);
			/* We dont reuse req (for pending), it is still owned by ulp.
			   Once returned by ulp, we will serve the pending list */
		}
	}

out:
	__NFOUTD;
	return rv;
}

static inline int check_md(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *block_cmd)
{
	int rv = -1;

	__NFIND;
	if (block_cmd->reqs[0].md) {
		if (disk->md_size == 0) {
			_NE(error_disk_check_md, "Disk not formatted with metadata");
			goto out;
		}
	}
	else {
#ifndef ALLOW_SIMULATED_MD
		if (disk->md_size &&
			block_cmd->reqs[0].op != NVMEIB_BLOCK_IO_OP_DISCARD) {
			_NE(error_1_disk_check_md, "Disk requires metadata");
			goto out;
		}
#endif
	}
	rv = 0;

out:
	__NFOUTD;
	return rv;
}

static void disk_cmd_link_init(struct nvmeibc_disk_command *dcmd)
{
	WARN_ON_ONCE(!list_empty(&dcmd->dcmd_link) && (dcmd->dcmd_link.next ||
												   dcmd->dcmd_link.prev));
	INIT_LIST_HEAD(&dcmd->dcmd_link);
}

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
static int execute_io_autocomp(
	struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *block_cmd)
{
	struct nvmeibc_block_io_req *req = &block_cmd->reqs[0];
	const enum nvmeib_block_io_op op = req->op;

	if (((nvmeibc_skip_disk_iocmds_flags & (1 << 0)) && (op == NVMEIB_BLOCK_IO_OP_READ)) ||
		((nvmeibc_skip_disk_iocmds_flags & (1 << 1)) && nvmeib_block_io_op_is_write(op)) ||
		((nvmeibc_skip_disk_iocmds_flags & (1 << 2)) && nvmeibc_disk_is_bcmd_jour_write(req)) ||
		  nvmeibc_skip_disk_iocmds_flags & (1 << 3)) {

		struct nvmeibc_d_iocmd_comp *comp = &block_cmd->comp;

		comp->comp_code = 0;
		nvmeibc_block_completion(comp);
		return 0;
	}

	return -1;
}
#endif

/* Debug function, validates sub block operation is allowed for the given
 * disc config + command. Will crash if not. Crash here is an indication of
 * a severe logical error. */
static void __verify_subblock_op_is_allowed(struct nvmeibc_disk_io_command *block_cmd) {
	struct nvmeibc_disk *disk = block_cmd->disk;
	BUG_ON(block_cmd->reqs[0].ndb->length != 512);
	BUG_ON(block_cmd->reqs[0].op != NVMEIB_BLOCK_IO_OP_READ &&
	       !nvmeib_block_io_op_is_write(block_cmd->reqs[0].op));
	BUG_ON(disk->sector_shift != 9);
	BUG_ON(disk->md_size != 0);
	BUG_ON(nvmeib_version_protocol_lt(&disk->last_tgt_ver, &nvmeib_2p2_version));
	BUG_ON(get_rcookie_ptr(block_cmd)->lba_jam_enc);
	BUG_ON(do_512b_sub_block_x_val(block_cmd->reqs[0].do_512b_sub_block_x) > 7);
}

static inline int execute_io(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *block_cmd)
{
	int rv = -1;

	__NFIND;
	if (unlikely(block_cmd->disk_cmd.cmd_type != NVMEIBC_DISK_CMD_IO)) {
		_NW(warn_disk_execute_io, "Invalid cmd-type @CMD_TYPE", block_cmd->disk_cmd.cmd_type);
		WARN_ON_ONCE(1);
		goto out;
	}
	if (atomic_read(&disk->paused))
		goto out;
	if (atomic_read(&disk->dying))
		goto out;

	if (check_md(disk, block_cmd) < 0)
		goto out;

	if (nvmeibc_disk_is_bcmd_jour_write(block_cmd->reqs)) {
		if (block_cmd->reqs[0].jam_op.n_ops != 1) {
			_NW(warn_1_disk_execute_io, "Unsupported journal n_ops=@INT",
				block_cmd->reqs[0].jam_op.n_ops);
			WARN_ON_ONCE(1);
			goto out;
		}
		if (NVMEIBC_DCMD_LEN_TO_SW_SECTORS(block_cmd) > disk->jour.rng_binje) {
			_NW(warn_2_disk_execute_io, "Num jour-lbas=@INT exceeds binje=@INT",
				NVMEIBC_DCMD_LEN_TO_SW_SECTORS(block_cmd), disk->jour.rng_binje);
			WARN_ON_ONCE(1);
			goto out;
		}
	}

	block_cmd->disk_cmd.start_ts = ktime_get();
	block_cmd->disk = disk;					// Todo, functions which accept 'cmd' dont need 'disk' param

	disk_cmd_link_init(&block_cmd->disk_cmd);

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibc_skip_disk_iocmds_flags)) {
		if(!execute_io_autocomp(disk, block_cmd)) {
			rv = 0;
			goto out;
		}
	}
#endif

	nvmeibc_disk_cmds_stats_done_reset(&block_cmd->disk_cmd);

	if (block_cmd->reqs[0].do_512b_sub_block_x)
		__verify_subblock_op_is_allowed(block_cmd);
	block_cmd->disk_cmd.local_cmd = false;

	if (disk->access_local &&
		(disk->md_size == 0 || !disk->md_extd)) {
		nvmeibc_block_cmd_status_debug(block_cmd, NVMEIBC_BLOCK_CMD_LOCAL);
		nvmeibc_disk_cmd_status_debug(&block_cmd->disk_cmd, NVMEIBC_DISK_CMD_LOCAL);
		if ((rv = execute_io_local(disk, &block_cmd->disk_cmd))) {
			_NT(trace_disk_execute_io, "IO failed (rv=@RV), pause (local) disk @DISK_NAME", rv, disk->name);
			//nvmeibc_disk_pause(disk);
			nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_LOCAL_IO_FAILED);
		}
	}
	else {
		nvmeibc_block_cmd_status_debug(block_cmd, NVMEIBC_BLOCK_CMD_REMOTE);
		nvmeibc_disk_cmd_status_debug(&block_cmd->disk_cmd, NVMEIBC_DISK_CMD_REMOTE);
		rv = execute_io_remote(disk, &block_cmd->disk_cmd);
	}

out:
	__NFOUTD;
	return rv;
}

struct ch_reused_bb_release_smp_fn_params {
	struct nvmeibc_disk *disk;
	struct nvmeibc_channel *ch;
	struct nvmeib_data_reuse_buf_params *r;
};

static void ch_reused_bb_release_smp_fn(void *arg)
{
	struct ch_reused_bb_release_smp_fn_params *params = arg;
	struct nvmeibc_disk *disk = params->disk;
	struct nvmeibc_channel *ch = params->ch;
	struct nvmeib_data_reuse_buf_params *r = params->r;
	void *context;
	bool do_pending = false;

	if (nvmeibc_channel_try_use_req_info(ch)) {
		if (ch->ct == ct_n_rdda) {
			nvmeibc_ib_nordda_channel_reused_context(c_to_inrc(ch), r, &context);
			do_pending = !!context;
			/* if !@context, nvmeibc_channel-end_use_req_info was called */
		}
		else {
			_NW(trace_disk_ch_reused_bb_release_smp_fn,
			    "Unexpected ch=@PTR, ct=@INT", ch, (int)ch->ct);
			BUG();
		}
	}

	if (do_pending) {
		/* rdda_pending_io
		 *   nordda_pending_io */
		ch->execute_pending_io(disk, ch, context, false, r->channel_ver);
	}
}

void nvmeibc_disk_reused_bb_release(struct nvmeibc_disk *disk,
	struct nvmeib_data_reuse_buf_params *p)
{
	struct nvmeib_data_reuse_buf_params r;
	struct nvmeibc_channel *ch = NULL;
	void *context;
	unsigned long flags;
	bool do_pending = false;
	u64 disk_version;
	u64 ch_version;

	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);

	/* copy cookie and reset it in ndb under lock */
	r = *p;
	nvmeib_data_reuse_buf_zero(p);

	_NT(trace_0_disk_nvmeibc_disk_ec_reuse_buf_del,
		"disk=@PTR, try return cookie={ch=@PTR, act=@STR(@INT),"
		"dv=@LLU, cv=@LLU}",
		disk, r.channel, nvmeib_rcookie_action_str(r.action), r.action,
		r.disk_ver, r.channel_ver);

	/* protected by pd layer thus we ONLY know that disk is not releasING,
	   check cookie is from current disk session/version */
	if ((disk_version = nvmeibc_disk_version_get(disk)) != r.disk_ver) {
		_NT(trace_1_disk_nvmeibc_disk_ec_reuse_buf_del,
			"stale cookie disk-version @STORED, curr=@CUR_LLONG",
			r.disk_ver, disk_version);
		goto unlock;
	}

	//TBD:
	//we cant really use the channel but as disk was already validated,
	//we trust cookie's ch-ptr which is fixed during disk life-cycle.
	ch = r.channel;

	/* check cookie-returns is allowed i.e. ch is not releasING and
	   check cookie is from current ch session/version */
	if (!nvmeibc_channel_version_get(ch, &ch_version)) {
		_NT(trace_2_disk_nvmeibc_disk_ec_reuse_buf_del,
			"ch=@PTR, version already invalid (for cookie returns)", ch);
		goto unlock;
	}
	if (ch_version != r.channel_ver) {
		_NT(trace_3_disk_nvmeibc_disk_ec_reuse_buf_del,
			"stale cookie ch-version @STORED, curr=@CUR_LLONG",
			r.channel_ver, ch_version);
		goto unlock;
	}

	if (nvmeibc_channel_is_ll_pcpu_ch(ch)) {
		struct ch_reused_bb_release_smp_fn_params params = {
			.disk = disk,
			.ch = ch,
			.r = &r,
		};

		spin_unlock_irqrestore(&disk->spinlock, flags);

		/* Lock-less per-cpu channel
		 * => Schedule on its CPU
		 */
		smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(ch), ch_reused_bb_release_smp_fn, &params, true);
		goto out;
	}

	//TBD:
	//check req/ch is really cookie'd, e.g. flag/list-add and/or inuse cnt

	/* try using the channel for pending cmds */
	if (nvmeibc_channel_try_use_req_info(ch)) {
		if (ch->ct == ct_n_rdda) {
			nvmeibc_ib_nordda_channel_reused_context(c_to_inrc(ch), &r, &context);
			do_pending = !!context;
			/* if !@context, nvmeibc_channel-end_use_req_info was called */
		}
		else {
			_NW(trace_4_disk_nvmeibc_disk_ec_reuse_buf_del,
				"Unexpected ch=@PTR, ct=@INT", ch, (int)ch->ct);
			BUG();
		}
	}

unlock:
	spin_unlock_irqrestore(&disk->spinlock, flags);

	if (do_pending) {
		/* rdda_pending_io
		   nordda_pending_io */
		ch->execute_pending_io(disk, ch, context, false, r.channel_ver);
	}
out:
	__NFOUTD;
}

int nvmeibc_disk_execute_io(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *block_cmd)
{
	struct nvmeibc_block_command *bcmd = block_cmd->disk_cmd.owner;
	int rv;

	switch (block_cmd->reqs[0].op) {
	case NVMEIB_BLOCK_IO_OP_READ:
		if (disk->inj_err == 'r') {
			_NT(error_disk_nvmeibc_disk_execute_io, "Disk @DISK_NAME - injecting read error", disk->name);
			block_cmd->disk_cmd.complete_w_error = true;
			disk->inj_err = 0;
		}
		block_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_IO;
		block_cmd->disk_cmd.server_side_only = false;
		break;
	case NVMEIB_BLOCK_IO_OP_WRITE:
		if (disk->inj_err == 'w' ||
			(disk->inj_err == 'j' && bcmd->my_stage == E_CMDS_STAGE_WRITE_JOURNAL) ||
			(disk->inj_err == 'd' && bcmd->my_stage == E_CMDS_STAGE_DO_IO_AND_PAR)) {
			_NT(error_1_disk_nvmeibc_disk_execute_io, "Disk @DISK_NAME - injecting write error for stage @MY_STAGE", disk->name, bcmd->my_stage);
			block_cmd->disk_cmd.complete_w_error = true;
			disk->inj_err = 0;
		}
		block_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_IO;
		block_cmd->disk_cmd.server_side_only = false;
		break;
	case NVMEIB_BLOCK_IO_OP_DISCARD:
		block_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_IO;
		block_cmd->disk_cmd.server_side_only = false;
		break;
	case NVMEIB_BLOCK_IO_OP_MD_READ:
	case NVMEIB_BLOCK_IO_OP_WRITE_UNCOR:
	case NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR:
		block_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_IO;
		block_cmd->disk_cmd.server_side_only = true;
		break;
	default:
		_NE(error_2_disk_nvmeibc_disk_execute_io, "Invalid block op @BLOCK_IO_OP for disk @DISK_NAME",
		   block_cmd->reqs[0].op, disk->name);
		BUG();
		rv = -1;
		goto out;
	}
	block_cmd->disk_cmd.cpu_mask_info = block_cmd->reqs[0].cpu_mask_info;
	nvmeibc_disk_cmd_status_debug_init(&block_cmd->disk_cmd, disk);

	rv = execute_io(disk, block_cmd);
	if (rv) {
		rv = -1;
		nvmeibc_disk_cmd_status_debug(&block_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
	}

out:
	return rv;
}

int nvmeibc_disk_execute_gen(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_gen_cmd *gen_cmd)
{
	int rv = -1;
	if (atomic_read(&disk->paused))
		goto out;
	if (atomic_read(&disk->dying))
		goto out;
	gen_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_GEN;
	gen_cmd->disk_cmd.server_side_only = true;
	gen_cmd->disk_cmd.cpu_mask_info = &gen_cmd->cpu_mask_info;
	gen_cmd->local_bypass = false;
	gen_cmd->disk = disk;

	nvmeibc_disk_cmd_status_debug_init(&gen_cmd->disk_cmd, disk);

	disk_cmd_link_init(&gen_cmd->disk_cmd);
	gen_cmd->disk_cmd.local_cmd = false;
	if (disk->access_local) {
		rv = execute_io_local(disk, &gen_cmd->disk_cmd);
	}
	else {
		rv = execute_io_remote(disk, &gen_cmd->disk_cmd);
	}

	if (rv) {
		rv = -1;
		nvmeibc_disk_cmd_status_debug(&gen_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
	}

out:
	return rv;
}

int nvmeibc_disk_execute_lock(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_lock_cmd *lock_cmd)
{
	int rv;
	lock_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_LOCK;
	lock_cmd->disk_cmd.server_side_only = true;

	nvmeibc_disk_cmd_status_debug_init(&lock_cmd->disk_cmd, disk);

	disk_cmd_link_init(&lock_cmd->disk_cmd);
	lock_cmd->disk_cmd.local_cmd = false;
	if (disk->access_local)
		rv = execute_io_local(disk, &lock_cmd->disk_cmd);
	else
		rv = execute_io_remote(disk, &lock_cmd->disk_cmd);

	if (rv) {
		rv = -1;
		nvmeibc_disk_cmd_status_debug(&lock_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
	}

	return rv;
}

bool nvmeibc_disk_gen_cmd_is_timed_out(
	const struct nvmeibc_disk_gen_cmd *gen_cmd, ulong now)
{
	return ((now - gen_cmd->jiffies_start) >= gen_cmd->timeout);
}

void nvmeibc_disk_init_stats(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *info = disk->info;
	unsigned long flags;

	__NFIND;
	/* we must check for info because local disk does not have one */
	if (info) {
		/* RDDA stats removed */
	}
	else {
		spin_lock_irqsave(&disk->stats_spinlock, flags);
		disk->local_sum_dt = ktime_set(0, 0);
		disk->local_counts = 0;
		spin_unlock_irqrestore(&disk->stats_spinlock, flags);
	}
	__NFOUTD;
}

void nvmeibc_disk_print_stats(struct nvmeibc_disk *disk, const char *str)
{
	struct nvmeibc_disk_info *info __attribute__((unused)) = disk->info;
	u64 avg = 0;
	unsigned long flags;

	__NFIND;
	/* we must check for info because local disk does not have one */
	if (info) {
		/* RDDA print stats removed */
		_NT(trace_4_disk_nvmeibc_disk_print_stats, "No statistics");
	}
	else {
		spin_lock_irqsave(&disk->stats_spinlock, flags);
		if (disk->local_counts) {
			u64 local_sum_dt_ns = ktime_to_ns(disk->local_sum_dt);
			avg = DIV_ROUND_CLOSEST(local_sum_dt_ns, disk->local_counts);
			_NT(trace_5_disk_nvmeibc_disk_print_stats, "!!!!! local@@DISK_NAME-@STR: send n=@LOCAL_COUNTS, io_avg=@IO_AVG",
				disk->name, str, disk->local_counts, avg);
		}
		spin_unlock_irqrestore(&disk->stats_spinlock, flags);
	}
	__NFOUTD;
}
/**
 * nvmeibc_volume_discover_using_port(): find the local port
 * that can access the remote admin nic
 */
static int discover_using_port(struct nvmeibc_ib_port *port, void *args)
{
	struct nvmeibc_disk *disk = args;
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_ib_net *net;
	struct nvmeib_rdma_path_info info;

	__NFIND;
	_NT(trace_disk_discover_using_port, "Find path to disk's arnics from local port @IB_DEV_NAME:@GID_IPV6",
		port->nic_dev->dev->ib_dev->name, &port->gid.gid);
	list_for_each_entry(arnic, &disk->arnics, link) {
		_NT(trace_1_disk_discover_using_port, "arnic @IB_GID_IPV6 (l=@LOCAL_INT, a=@ALIVE, ch=@CHANNEL, sp=@SUBNET_PREFIX, if=@INTERFACE_ID_INT l=@LINK_LAYER t=@TRANSPORT_TYPE)",
			&arnic->ib_gid, arnic->local, arnic->alive, !!arnic->channel,
			!!arnic->ib_gid.global.subnet_prefix,
			!!arnic->ib_gid.global.interface_id,
			arnic->link_layer, arnic->transport_type);
		/* checks */
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
				NVMEIBC_ARNIC_HAS_CH)) {
			continue;
		}
		if (!arnic->ib_gid.global.subnet_prefix &&
			!arnic->ib_gid.global.interface_id) {
			_NT(trace_2_disk_discover_using_port, "Skip, uninitialized gid");
			continue;
		}
		if (arnic->local &&
			arnic->ib_gid.global.interface_id != port->gid.gid.global.interface_id) {
			_NT(trace_3_disk_discover_using_port, "Skip, connection to local arnic, must be from that nic");
			continue;
		}
		if (arnic->link_layer != port->layer) {
			_NT(trace_disk_discover_using_port_link_mismatch,
					"Skip, connection to arnic @IB_GID_IPV6 (link layer @LINK_LAYER) from local port @IB_DEV_NAME:@GID_IPV6 (link layer @LINK_LAYER). Link layer mismatch",
					&arnic->ib_gid, arnic->link_layer, port->nic_dev->dev->ib_dev->name, &port->gid.gid, port->layer);
			continue;
		}
		if (arnic->transport_type != port->transport_type) {
			_NT(trace_disk_discover_using_port_transport_mismatch,
					"Skip, connection to arnic @IB_GID_IPV6 (transport_type @TRANSPORT_TYPE) from local port @IB_DEV_NAME:@GID_IPV6 (transport_type @TRANSPORT_TYPE). Transport type mismatch",
					&arnic->ib_gid, arnic->transport_type, port->nic_dev->dev->ib_dev->name, &port->gid.gid, port->transport_type);
			continue;
		}

		ch = ac_to_iac(arnic->channel);
		net = &ch->net.base;
		if (net->port == NULL) {
			net->path.service_id = cpu_to_be64(arnic->service_id);
			net->path.pkey = cpu_to_be16(arnic->pkey);
			net->path.sgid = port->gid.gid;
			info.dev = P2NV(port);
			info.sa = nvmeibc_sa_client(nvmeibc_cinst_get_core_p(disk));
			info.path = &net->path;
			info.src_port = port->port;
			info.pkey = arnic->pkey;
			info.service_id = arnic->service_id;
			info.service_port = arnic->service_port;
			if (!nvmeibc_disk_find_path(disk, &info)) {
				_NT(trace_4_disk_discover_using_port,
					"Found path to arnic: l=@IB_GID_IPV6 -> r=@IB_GID_IPV6 (@IB_DEV_NAME), service type=@INT",
					&port->gid.gid, &arnic->ib_gid, port->nic_dev->dev->ib_dev->name, info.rdma_type);
				net->port = port;
				net->cm_rdma_type = info.rdma_type;
			}
			else {
				_NT(trace_5_disk_discover_using_port, "Fail to find path");
				ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_NO_PATH_FROM_LOCAL);
			}
		}
		else {
			_NT(trace_6_disk_discover_using_port,
				"arnic @IB_GID_IPV6 already discovered (from other lnic)",
				&arnic->ib_gid);
		}
	}
	__NFOUTD;
	return 0;
}

/**
 * nvmeibc_check_arnics_access(): check that the current client
 * machine could access some of the remote admin nics
 */
static int check_arnics_access(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_ib_net *net;
	int rv = 0;
	int n_access = 0;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_CHECK_ARNICS_ACCESS);

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
				NVMEIBC_ARNIC_HAS_CH))
			continue;
		ch = ac_to_iac(arnic->channel);
		net = &ch->net.base;
		if (!net->port) {
			_NT(trace_disk_check_arnics_access, "No access found to arnic @IB_GID_IPV6", &arnic->ib_gid);
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_NO_PATH_FROM_LOCAL);
		}
		else
			++n_access;
	}

	if (n_access == 0) {
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_NO_ARNICS_ACCESS);
		rv = -ENODEV;
	}

	DD_STG_END(disk, DD_STG_CHECK_ARNICS_ACCESS, rv, NVMEIBC_DISK_DISCOVER_ARNIC_NO_PATHS);
	__NFOUTD;
	return rv;
}

/**
 * nvmeibc_read_all_io_rscs(): read remote disks & io nics that
 * an remote admin nic reports
 */
static int read_all_io_rsc(struct nvmeibc_disk *disk,
	struct nvmeibc_admin_rnic *arnic)
{
	int rv;

	__NFIND;
	rv = nvmeibc_ib_admin_channel_connect(ac_to_iac(arnic->channel), disk,
		arnic);
	__NFOUTD;
	return rv;
}

#ifdef NVMEIBC_ARNICS_PRIORITY
static void find_set_b_arnic(struct list_head *arnics, int num_items)
{
	struct nvmeibc_admin_rnic *b_arnic = NULL;
	struct nvmeibc_admin_rnic *arnic;

	int i = 0;
	bool good_nic;

	NFIN;
	list_for_each_entry(arnic, arnics, link) {
		if (i >= num_items)
			break;
		++i;

		if (!b_arnic) {
			b_arnic = arnic;
			continue;
		}

		if (!b_arnic->prefered && arnic->prefered) {
			b_arnic = arnic;
			continue;
		}

		if (!arnic->prefered && b_arnic->prefered) {
			continue;
		}

		BUG_ON(arnic->prefered != b_arnic->prefered);

		good_nic = arnic->priority && arnic->alive;

		if (!good_nic)
			continue;

		if (!b_arnic->priority && arnic->priority) {
			b_arnic = arnic;
			continue;
		}

		if ((b_arnic->priority > arnic->priority ) ||
			(b_arnic->priority == arnic->priority  &&
				b_arnic->num_conns > arnic->num_conns) ||
			(b_arnic->priority == arnic->priority &&
				b_arnic->num_conns == arnic->num_conns &&
				b_arnic->order > arnic->order))
			b_arnic = arnic;

	}
	if (!b_arnic) {
		_NW(find_set_b_arnic_w1, "Cannot find nic for disk");
	} else {
		list_del(&b_arnic->link);
		list_add(&b_arnic->link, arnics);
	}

	NFOUT;
}

static void reorder_arnics(struct list_head *arnics, int n)
{
	int i ,j;
	struct list_head *items;
	struct nvmeibc_admin_rnic *arnic;
	char gid_buf[GUID_SIZE];

	NFIN;

	for (i = 0; i < n; ++i) {
		items = arnics;
		for (j = 0; j < i; ++j)
			items = items->next;
		find_set_b_arnic(items, n - i);
	}

	_NT(reorder_arnics_t1, "Listing the ordered list of arnics:");
	list_for_each_entry(arnic, arnics, link) {
		format_gid(&arnic->ib_gid, gid_buf);
		_NT(reorder_arnics_t2, "R_GID=@STR prefered=@INT priority=@INT "
			"num_conss=@INT order=@INT alive=@INT",
			gid_buf, arnic->prefered, arnic->priority, arnic->num_conns,
			arnic->order, arnic->alive);
	}
	NFOUT;
}
#endif /* NVMEIBC_ARNIC_PRIORITY */

/**
 * nvmeibc_read_all_io_rscs(): read the remote disks & io nics
 * that each remote admin nic (that this machine can access)
 * reports
 */
static int read_all_io_rscs(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_ib_net *net;
	int n = 0, rv = 0, tot_arnics __attribute__((unused))  = 0;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_READ_ALL_IO_RSCS);
	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		++tot_arnics;
		arnic->priority.numa_dist = 0;
		arnic->num_conns = 0;
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk, NVMEIBC_ARNIC_HAS_CH))
			continue;
		ch = ac_to_iac(arnic->channel);
		net = &ch->net.base;
		if (net->port) {
			struct nvmeib_dev *nvmeib_dev = net->port->nic_dev->dev;
			_NT(trace_disk_read_all_io_rscs, "Connect admin-ch: l=@IB_DEV_NAME:@SGID -> r=@IB_GID_IPV6 and "
			   "Get IO rscs (disk & iornics)",
			   nvmeib_dev->ib_dev->name,
			   &net->path.sgid, &arnic->ib_gid);

			if ((rv = read_all_io_rsc(disk, arnic)) < 0) {
				_NT(trace_1_disk_read_all_io_rscs, "Fail read io rscs from nic @IB_GID_IPV6", &arnic->ib_gid);
				arnic->alive = false;
				continue;
			}

			arnic->alive = true;
			++n;
			if (disk->access_local) {
				ch->base.n_disks = 1;
				disk->local_admin_ch = &ch->base;
				disk->local_admin_ch->is_main = true;
				disk->main_ach_wq_pid = wq_pid(ch->base.remove_wq);
				disk->cid = disk->local_admin_ch->cid;
				ch->net.base.ioch = &ch->base.base;
				nvmeibc_target_arnic_new_conn(arnic);
				_NT(trace_2_disk_read_all_io_rscs, "disk @DISK_NAME (local) logged in, main admin-ch (@CH_PTR, wq=@NVMEIB_QPID), cid=@CID",
					disk->name, ch, nvmeib_qpid(arnic->channel->remove_wq), disk->cid);
			}

			_NT(trace_3_disk_read_all_io_rscs, "Connected our admin-ch, delight");
			break;
		}
	}
#ifdef NVMEIBC_ARNICS_PRIORITY
	//reorder the arnics according to priority
	if (disk->is_local)
		reorder_arnics(&disk->arnics, tot_arnics);
#endif

	DD_STG_END(disk, DD_STG_READ_ALL_IO_RSCS, (n ?: -1) , NVMEIBC_DISK_DISCOVER_IO_RESOURCES_READ_FAIL);
	_ND(trace_4_disk_read_all_io_rscs, "Found @COUNT real arnics", n);
	__NFOUTD;
	return n ?: -1;
}

/**
 * nvmeibc_volume_access_using_port(): after each remote admin
 * nic reported us with its disks and io nics we try with our
 * local io nics to access its io nics
 */
static int access_using_port(struct nvmeibc_ib_port *port, void *args)
{
	struct nvmeibc_disk *disk = args;
	struct nvmeibc_admin_rnic *arnic;
	int rv = 0;

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
				NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_CH))
			continue;
		_ND(trace_disk_access_using_port, "Checking admin remote nic @IB_GID_IPV6", &arnic->ib_gid);
		if ((rv = nvmeibc_ib_admin_channel_access_iornics(
			ac_to_iac(arnic->channel), &port->gid,
			port->pkey, port)) < 0) {
				ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_ACCESS_IORNICS_FAILED);
			goto out;
		}
	}

out:
	__NFOUTD;
	return rv;
}

static int create_admin_channels(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_admin_channel *ch;
	int rv = 0;
	int n_admin_chs = 0;
	int n = 0, m = 0;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_CREATE_ADMIN_CHANNELS);

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk, 0)) {
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_SKIPPED);
			continue;
		}
		_ND(trace_disk_create_admin_channels, "Comparing nic node @CONFIG_NODE_ID to disk node @NODE_ID_STR", disk->config_node_id,
			arnic->node_id);
		++n;
		if (strcmp(disk->config_node_id, arnic->node_id)) {
			++m;
			/*eliminate nics that don't have access to the disk in config*/
			arnic->channel = 0;
			arnic->alive = false;
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_SKIPPED_DIFF_CFG_NODE);
			continue;
		}
		if (!(ch = nvmeibc_ib_admin_channel_create(nvmeibc_cinst_get_core_p(disk), arnic))) {
			_NT(trace_1_disk_create_admin_channels, "Failed to create admin channel @IB_GID_IPV6", &arnic->ib_gid);
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_ADMIN_CHAN_CREATION_FAILED);
			rv = -ENOMEM;
			goto out;
		}
		n_admin_chs++;
		ch->base.base.disk = disk;
		arnic->channel = &ch->base;
		ch->base.arnic = arnic;
	}

	if (!n_admin_chs) {
		if (n && n == m)
			list_for_each_entry(arnic, &disk->arnics, link)
				_NT(trace_200_disk_create_admin_channels,
					"arnic @STR and disk @STR - mismatch",
					arnic->node_id, disk->config_node_id);
		_NT(trace_3_disk_create_admin_channels, "No admin channels created");
		rv = -ENODEV;
	}

out:
	__NFOUTD;
	DD_STG_END(disk, DD_STG_CREATE_ADMIN_CHANNELS, rv, NVMEIBC_DISK_DISCOVER_ADMIN_CH_CREATE_FAILED);
	return rv;
}

/**
 * nvmeibc_check_disks_access(): check that the current client
 * machine could access all remote disks
 */
static int check_disks_access(struct nvmeibc_disk *disk)
{
	int rv = 0;

	__NFIND;
	if (!disk->access_local && list_empty(&disk->rionics)) {
		_NT(trace_disk_check_disks_access, "Disk @DISK_NAME is not accessible for client machine", disk->name);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_NO_RIONICS);
		rv = -1;
	}
	__NFOUTD;
	return rv;
}

static int create_access_map(struct nvmeibc_disk *disk, bool is_rediscover)
{
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_admin_channel *ach;
	int rv = 0;
	int n_access_maps = 0;
	int n_iter __attribute__((unused)) = 0, n_skip __attribute__((unused)) = 0, n_failed __attribute__((unused)) = 0;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_CREATE_ACCESS_MAP);
	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		n_iter++;
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
			NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_CH)) {
			n_skip++;
			continue;
		}
		ach = ac_to_iac(arnic->channel);
		/* build access map only for arnics that provide io channels */
		if (arnic->channel->n_rionics_used) {
			if ((rv = nvmeibc_ib_admin_channel_create_access_map(
				ach, is_rediscover)) < 0) {
				_NT(trace_disk_create_access_map, "Fail to create access map from admin-ch of arnic @IB_GID_IPV6, "
				   "disconnect it", &arnic->ib_gid);
				arnic->alive = false;
				nvmeibc_ib_admin_channel_disconnect(ach);
				n_failed++;
				ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_ACCESS_MAP_CREATE_FAILED);
				continue;
			}
			_ND(trace_1_disk_create_access_map, "Created access map from admin-ch of arnic @IB_GID_IPV6",
				&arnic->ib_gid);
			n_access_maps++;
			/* TODO:
			   since the requested map includes all rionics and not only the
			   ones provided by this arnic, we have the whole picture, break.
			   Note that this does NOT disconnect following arnics */
		}
		else {
			/* the admin channel is not needed so just close it to reduce
			   the number of qps
			*/
			arnic->alive = false;
			_NT(trace_2_disk_create_access_map, "admin-ch of arnic @IB_GID_IPV6 added 0 rionics,"
			   "disconnect it", &arnic->ib_gid);
			nvmeibc_ib_admin_channel_disconnect(ach);
		}
	}
	if (n_access_maps == 0) {
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_ACCESS_MAP_CREATE_FAILED);
		rv = -EIO;
	} else {
		/* In case the last one failed, but others succeeded */
		rv = 0;
		DISK_DISCOVER_STATUS_FORCE(disk, NVMEIBC_DISK_DISCOVER_UNKNOWN);
	}

	DD_STG_END(disk, DD_STG_CREATE_ACCESS_MAP, rv, -1);
	__NFOUTD;
	return rv;
}

static int get_journal_range(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_admin_channel *ach;
	int rv = -1;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_GET_JOURNAL_RANGE);

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
				NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_CH))
			continue;
		ach = ac_to_iac(arnic->channel);
		/* Request journal range over admin channel */
		if ((rv = nvmeibc_ib_admin_channel_get_journal_range(ach))) {
			_NT(trace_disk_get_journal_range, "Fail to get journal range response from admin-ch of arnic @IB_GID_IPV6, "
			"disconnect it", &arnic->ib_gid);
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_JOURNAL_RANGE_GET_FAILED);
			arnic->alive = false;
			nvmeibc_ib_admin_channel_disconnect(ach);
			continue;
		}
		break;
	}

	DD_STG_END(disk, DD_STG_GET_JOURNAL_RANGE, rv, NVMEIBC_DISK_DISCOVER_REMOTE_JOURNAL_GET_FAILED);
	__NFOUTD;
	return rv;
}

static int disk_is_transport_tcp(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	enum rdma_transport_type ttype;
	bool found = false;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_CHECK_TRANSPORT_TYPE);
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
			NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_CH))
			continue;

		if (!found) {
			_NT(t0_disk_is_transport_tcp,
				"alive arnic=@IB_GID_IPV6, transport=@TRANSPORT_TYPE",
				&arnic->ib_gid, arnic->transport_type);
			found = true;
			ttype = arnic->transport_type;
			disk->is_tcp = arnic->transport_type == RDMA_TRANSPORT_IWARP;
		} else if (ttype != arnic->transport_type) {
			_NE(t1_disk_is_transport_tcp,
				"Oops, another alive arnic=@IB_GID_IPV6 && diff transport=@TRANSPORT_TYPE",
				&arnic->ib_gid, arnic->transport_type);
			found = false;
			goto out;
		}
	}

out:
	DD_STG_END(disk, DD_STG_CHECK_TRANSPORT_TYPE, (found ? 0 : -1), NVMEIBC_DISK_DISCOVER_INVALID_TRANSPORT_TYPE);
	return found ? 0 : -1;
}

static int request_disks_resources(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	int rv = 0;
	int n_rscs = 0;
	bool use_rdda;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_REQUEST_DISKS_RESOURCES);

	__NFIND;
	BUG_ON(disk->access_local); //local disk does not need resources

	use_rdda = !disk->is_local &&
		nvmeibc_cinst_get_core_p(disk)->use_rdda &&
		disk->info->max_client_rscs && !disk->is_tcp;

	/* If we wich to use the best admin channel, we need to make sure that
	   the channel is located first in disk->arnics */
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
			NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_CH))
			continue;

		rv = 0;
		/* Only remote disk requests disk resources */
		if (use_rdda &&
			(rv = nvmeibc_ib_admin_channel_request_disks_resources(
					ac_to_iac(arnic->channel))) < 0) {
			_NT(trace_disk_request_disks_resources, "Fail to get remote disk resources for disk @DISK_NAME, status @RV",
				disk->name, rv);
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_DISK_RDDA_RESOURCES_GET_FAILED);
		}
		else {
			/* we only need one admin qp for the resources for the disk */
			arnic->channel->is_main = true;
			disk->main_ach_wq_pid = wq_pid(arnic->channel->remove_wq);
			disk->cid = arnic->channel->cid;
			disk->nr_get_by_cpu_index = disk->is_tcp ? nvmeibc_nr_get_by_cpu_index_tcp : nvmeibc_nr_get_by_cpu_index;
		
#ifdef CONFIG_NUMA
			disk->nr_get_by_numa_node = disk->is_tcp ? nvmeibc_nr_get_by_numa_node_tcp : nvmeibc_nr_get_by_numa_node;
#endif
			disk->prio_pending = nvmeibc_disk_prio_pending;

			if (disk->nr_get_by_numa_node) {
				int i;
				disk->info->avail_norddas_per_numa_node = kmalloc_array(nr_node_ids, sizeof(struct plist_head), GFP_KERNEL);
				if (!disk->info->avail_norddas_per_numa_node) {
					rv = -ENOMEM;
					goto out;
				}
				for_each_node(i) {
					plist_head_init(&disk->info->avail_norddas_per_numa_node[i]);
				}
			}

			n_rscs++;
			_NT(trace_1_disk_request_disks_resources, "disk @DISK_NAME (@POSITION_STR), main admin-ch @BASE_NAME (@CHANNEL_PTR, wq=@NVMEIB_QPID), cid=@CID",
				disk->name, !disk->access_local ? "remote" : "local-bypass",
			    arnic->channel->base.name,
				arnic->channel, nvmeib_qpid(arnic->channel->remove_wq), disk->cid);
			nvmeibc_target_arnic_new_conn(arnic);
			_ND(trace_2_disk_request_disks_resources, "Selected admin nic with priority @PRIORITY num_conns=@NUM_CONNS",
				arnic->priority.raw, arnic->num_conns);
			/*Only one main admin channel per disk*/
			break;
		}
	}

	if (n_rscs == 0 && use_rdda) {
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_REQUEST_RDDA_RESOUCES_FAILED);
		rv = -EIO;
	}
out:
	DD_STG_END(disk, DD_STG_REQUEST_DISKS_RESOURCES, rv, -1);
	__NFOUTD;
	return rv;
}

static int check_arnic_using_port(struct nvmeibc_ib_port *port,
	struct nvmeibc_admin_rnic *arnic)
{
	char gid_buf[GUID_SIZE] = {0};
	struct nvmeib_rdma_ib_port_gid port_gids;

	NFIN;
	if (!port->port_active) {
		if (arnic->local_port == port) {
			_NT(trace_disk_check_arnic_using_port, "Local arnic port @RAW_IPV6 is now inactive", &arnic->ib_gid.raw);
			arnic->local = false;
			arnic->local_port = NULL;
		}
	}
	else if (nvmeibc_is_arnic_local(&port->gid, arnic)) {
		format_gid_raw(port_gids.gid.raw, gid_buf);
		_ND(trace_1_disk_check_arnic_using_port, "User nic @GID_BUF is local", gid_buf);
		arnic->local = true;
		arnic->local_port = port;
	}
	NFOUT;
	return 0;
}

static int check_arnics_using_dev(struct nvmeibc_local_nic *ln, void *args)
{
	struct nvmeibc_local_nic_port *port;
	struct nvmeibc_admin_rnic *arnic = args;
	int rv = 0;

	NFIN;
	if (arnic->local) {
		if (arnic->local_port && arnic->local_port->port_active)
			goto out;
		else
			arnic->local = false;
	}
	/* try to check if user nics is local using all ports */
	list_for_each_entry(port, &ln->ports, link) {
		check_arnic_using_port(port->ib_port, arnic);
		if (arnic->local)
			goto out;
	}

out:
	NFOUT;
	return rv;
}

static int call_for_each_lnic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_local_nic *, void *),
	void *args)
{
	struct list_head *local_nics = &disk->local_nics;
	struct nvmeibc_local_nic *ln;
	int rv = 0;

	NFIN;
	list_for_each_entry(ln, local_nics, link)
		if ((rv = f(ln, args)) < 0) {
			_NE(error_disk_call_for_each_lnic, "Failed with error @RV", rv);
			break;
		}
	NFOUT;
	return rv;
}

static int call_for_each_lport(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_ib_port *, void *),
	void *args, bool must_be_used, bool must_be_active, int *call_count)
{
	struct list_head *local_nics = &disk->local_nics;
	struct nvmeibc_local_nic *ln;
	struct nvmeibc_local_nic_port *lnp;
	struct nvmeibc_ib_port *ib_port;
	int rv = 0;
	int count = 0;

	NFIN;
	list_for_each_entry(ln, local_nics, link) {
		list_for_each_entry(lnp, &ln->ports, link) {
			ib_port = lnp->ib_port;
			if ((!must_be_used || ib_port->port_used) && (!must_be_active || ib_port->port_active)) {
				if ((rv = f(ib_port, args)) < 0) {
					_NE(error_disk_call_for_each_lport, "Failed with error @RV", rv);
					goto out;
				}
				count++;
			} else {
				_NT(trace_disk_call_for_each_lport, "count=@COUNT must_be_used=@MUST_BE_USED used=@USED must_be_active=@MUST_BE_ACTIVE active=@ACTIVE",
				   count, must_be_used, ib_port->port_used, must_be_active, ib_port->port_active);
			}
		}
	}
	if (call_count)
		*call_count = count;
out:
	NFOUT;
	return rv;
}


static int call_for_each_arnic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_admin_rnic *, void *, bool),
	void *args)
{
	struct nvmeibc_admin_rnic *arnic;
	int rv = 0;

	NFIN;
	list_for_each_entry(arnic, &disk->arnics, link)
		if ((rv = f(arnic, args, arnic->link.next == &disk->arnics)) < 0) {
			_NE(error_disk_call_for_each_arnic, "Failed with error @RV", rv);
			break;
		}
	NFOUT;
	return rv;
}

static int call_for_each_rionic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_io_rnic *, void *),
	void *args)
{
	struct nvmeibc_io_rnic *rionic;
	int rv = 0;

	NFIN;
	list_for_each_entry(rionic, &disk->rionics, disk_link)
		if ((rv = f(rionic, args)) < 0) {
			_NE(error_disk_call_for_each_rionic, "Failed with error @RV", rv);
			break;
		}
	NFOUT;
	return rv;
}

static int call_for_each_nr_rionic(struct nvmeibc_disk *disk,
	int (*f)(struct nvmeibc_io_rnic *, void *, bool),
	void *args)
{
	struct nvmeibc_io_rnic *rionic;
	int rv = 0;

	NFIN;
	list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
		if ((rv = f(rionic, args, rionic->disk_nrlink.next == &disk->nr_rionics)) < 0) {
			_NE(error_disk_call_for_each_nr_rionic, "Failed with error @RV", rv);
			break;
		}
	}

	NFOUT;
	return rv;
}

static int mark_local_arnics(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	int rv = 0;
	int n_arnics = 0;

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		n_arnics++;
		if ((rv = call_for_each_lnic(disk, check_arnics_using_dev, arnic)) < 0) {
			goto out;
		}
	}

	if (n_arnics == 0) {
		_NT(trace_disk_mark_local_arnics, "Disk @DISK_NAME has no arnics", disk->name);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_NO_ARNICS);
		rv = -ENODEV;
	}

out:
	__NFOUTD;
	return rv;
}

/* Jared: Now unused */
#if 0
static int drop_dup_local_arnics(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic, *t_arnic;
	bool found = false;
	int rv = 0;

	__NFIND;
	list_for_each_entry_safe(arnic, t_arnic, &disk->arnics, link)
		if (arnic->local) {
			if (found) {
				_ND(drop_dup_local_arnics_d1,
					"User nic @SGID is local and is removed",
					&arnic->ib_gid);
				list_del(&arnic->link);
				kfree(arnic);
			}
			else
				found = true;
		}
	__NFOUTD;
	return rv;
}
#endif

static int build_local_lock_segments(struct nvmeibc_disk *disk,
									 struct nvmeibc_disk_segments_locks *disk_segs_locks_local);
static int local_disk_locks_alloc(struct nvmeibc_disk *disk,
								  struct nvmeibc_disk_segments_locks *disk_segs_locks_local);
static void local_disk_locks_free(struct nvmeibc_disk *disk);

static int start_local_lock_channel(struct nvmeibc_disk *disk)
{
	int ret = 0, i;
	struct nvmeibc_locks_channel *ch;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_START_LOCAL_LOCK_CHANNEL);

	NFIN;

	BUG_ON(!(disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
														   (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1, .dont_wait = 0, .local_only = 1})));
	if (disk_segs_locks->lock_ch) {
		_NE(error_disk_start_local_lock_channel, "Disk's prev lock-ch=@LOCK_CH not freed",
			disk_segs_locks->lock_ch);
		ret = -1;
		goto out;
	}

	_NT(trace_disk_start_local_lock_channel, "Get lock-gids and try connect loopback lock-channel ...");
	if (!(ch = nvmeibc_ib_admin_channel_connect_lock_lb_channel(
		ac_to_iac(disk->local_admin_ch)))) {
		_NE(error_1_disk_start_local_lock_channel, "Failed to connect loopback locks channel");
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_LOCAL_LOCK_CH_CONNECT_FAILED);
		ret = -1;
		goto out;
	}
	_NT(trace_1_disk_start_local_lock_channel, "connect_lock_lb_channel - DONE!");
	disk_segs_locks->lock_ch = ch; /* #link-lock-ch (local) from 'disk->segments_locks.lock_ch' */

	if (!nvmeibc_locks_channel_all_2nd_connected(ch)) {
		ret = -1;
		goto out;
	}

	/* alllocate &disk->local.ldl_list and &disk->local.active_locks */
	if (local_disk_locks_alloc(disk, disk_segs_locks) < 0) {
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_LOCAL_LOCK_CH_ACTV_TBL_ALLOC_FAILED);
		ret = -1;
		goto out;
	}

	/* copy &disk->local.ldl_list to disk->segments_locks.locks[]
	   and set disk->segments_locks.num_of_segments */
	if ((ret = build_local_lock_segments(disk, disk_segs_locks))) {
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_LOCAL_LOCK_CH_BUILD_SEGMENTS_FAILED);
		ret = -1;
		goto free_ldl;
	}

	for (i = 0; i < disk_segs_locks->num_of_segments; ++i) {
		disk_segs_locks->locks[i].locks_channel = ch;
	}
	nvmeibc_disk_locks_attach_used_segments(disk, &disk->segments_locks_local);

free_ldl:
	local_disk_locks_free(disk);
out:
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1 });

	//JARED: Removing this as it plasters any case where we are treating local locks like remote */
	//YK:
	//Why do we set this ptr even if we didnt connect lock-ch?
	//stop-lock-channels uses it but uts irrelevant if ch was not connected
	disk->local_admin_ch->segments_locks_remote = disk->segments_locks_local; /* #link-lock-ch (local) from admin-ch (used for stopping lock-ch) */


	DD_STG_END(disk, DD_STG_START_LOCAL_LOCK_CHANNEL, ret, NVMEIBC_DISK_DISCOVER_LOCAL_LOCK_CH_STRAT_FAILED);
	NFOUT;
	return ret;
}

static int build_local_lock_segments(struct nvmeibc_disk *disk,
									 struct nvmeibc_disk_segments_locks *disk_segs_locks_local)
{
	struct nvmeib_local_disk_locks *ldl;
	int ret = 0, n_segments = 0, i = 0;
	struct nvmeibc_disk_seg_locks_mem_info *sl;
	NFIN;

	/* disk_segs_locks_local guard - should be locked by caller */
	BUG_ON(!rwsem_is_locked(&disk_segs_locks_local->guard));

	n_segments = disk->local.n_ldl;
	if (n_segments <= 0) {
		_NW(warn_disk_build_local_lock_segments, "LOCKS: No local locks to connect!!!!");
		disk_segs_locks_local->locks = NULL;
		ret = -ENODEV;
		goto out;
	}
	_ND(trace_disk_build_local_lock_segments, "LOCKS: for disk @DISK_NAME allocation @N_SEGMENTS segments", disk->name, n_segments);
	disk_segs_locks_local->locks = kzalloc(n_segments *
		sizeof(struct nvmeibc_disk_seg_locks_mem_info), GFP_KERNEL);
	if (!disk_segs_locks_local->locks) {
		_NE(error_disk_build_local_lock_segments, "Allocation error when tried to allocate memory for local disk "
		   "segments");
		ret = -ENOMEM;
		goto out;
	}
	disk_segs_locks_local->num_of_segments = n_segments;

	list_for_each_entry(ldl, &disk->local.ldl_list, link) {
		sl = &disk_segs_locks_local->locks[i];
		sl->locks_channel = NULL;
		sl->addr = ldl->addr;
		sl->disk = disk;
		sl->len = ldl->len;
		sl->lock_set_size = ldl->lock_set_size;
		sl->rkey = ldl->rkey;
		sl->seg_id = ldl->seg_id;
		sl->start_addr = ldl->start_addr;
		sl->lock_id = ldl->lock_id;
		sl->pages = ldl->pages;
		sl->dirty_bit_offset = ((ldl->len + (ldl->lock_set_size - 1)) /
			ldl->lock_set_size) * sizeof(u64);
		_ND(trace_1_disk_build_local_lock_segments, "\nLOCKS: local information gathered:");
		_ND(trace_2_disk_build_local_lock_segments, "LOCKS: addr = @ADDR", sl->addr);

		_ND(trace_3_disk_build_local_lock_segments, "LOCKS: lock_set_size = @LOCK_SET_SIZE", sl->lock_set_size);
		_ND(trace_4_disk_build_local_lock_segments, "LOCKS: rkey = @RKEY", sl->rkey);
		_ND(trace_5_disk_build_local_lock_segments, "LOCKS: seg_id = @SEG_ID_INT", sl->seg_id);
		_ND(trace_6_disk_build_local_lock_segments, "LOCKS: start_addr = @START_ADDR", sl->start_addr);
		_ND(trace_7_disk_build_local_lock_segments, "LOCKS: @LOCKID", (u32)sl->lock_id);
		_ND(trace_8_disk_build_local_lock_segments, "LOCKS: dirty_bit_offset = @DIRTY_BIT_OFFSET", sl->dirty_bit_offset);
		++i;
	}

out:
	NFOUT;
	return ret;
}

static bool is_local_disk(struct nvmeibc_disk *disk)
{
	bool rv = false;
	__NFIND;

	if (!disk->local_server)
		_NT(trace_disk_is_local_disk, "No local-server, assume Disk @DISK_NAME is NOT local", disk->name);
	else {
		/* call srv's nvmeibs_is_local_disk */
		rv = disk->local_server->is_local_disk(disk->name, &disk->md_size, &disk->md_extd);
		_NT(trace_1_disk_is_local_disk, "Disk @DISK_NAME is @POSITION_STR", disk->name, rv ? "Local" : "Remote");
	}

	__NFOUTD;
	return rv;
}

static int local_disk_cl_register(struct nvmeibc_disk *disk)
{
	int rv = -1;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_LOCAL_DISK_CL_REGISTER);
	__NFIND;

	if (!disk->local_admin_ch || !disk->local_admin_ch->arnic->alive) {
		_NE(error_disk_local_disk_cl_register, "No local-admin-ch (@LOCAL_ADMIN_CH)", disk->local_admin_ch);
		goto out;
	}
	if (disk->is_cl_reg) {
		_NE(error_1_disk_local_disk_cl_register, "Already registered");
		goto out;
	}
	/* call srv's nvmeibs_client_ldisk_register */
	if (disk->local_server->cl_register(disk->local_admin_ch->cid,
		disk->name, &disk->local) < 0) {
		_NE(error_2_disk_local_disk_cl_register, "Failed register to srv's cl (cid=@CID_LLONG)",
			disk->local_admin_ch->cid);
		goto out;
	}
	disk->is_cl_reg = true;

	//From Doron.L. commit b9c00dd0
	disk->sector_shift = disk->local.sector_shift;
	if (disk->sector_shift > NVMEIBC_SECTOR_SHIFT) {
		_NE(error_3_disk_local_disk_cl_register, "OOPS: product sector shift @SECTOR_SHIFT is less than "
		   "disk sector shift @SECTOR_SHIFT",
			disk->sector_shift, NVMEIBC_SECTOR_SHIFT);
		goto out;
	}
	//From Ofer.O. commit 3e9b36fd
	if (disk->local.max_request_size)
		disk->max_request_size_bytes =
		disk->local.max_request_size << disk->sector_shift;
	disk->md_size = disk->local.md_size;
	disk->md_extd = disk->local.md_extd;

	rv = 0;

out:
	DD_STG_END(disk, DD_STG_LOCAL_DISK_CL_REGISTER, rv, NVMEIBC_DISK_DISCOVER_LOCAL_REGISTER_SRV_FAILED);
	__NFOUTD;
	return rv;
}

static void local_disk_cl_unregister(struct nvmeibc_disk *disk, u64 cid)
{
	__NFIND;

	if (disk->is_cl_reg) {
		/* call srv's nvmeibs_client_ldisk_unregister */
		if (disk->local_server->cl_unregister(cid, &disk->local) < 0)
			_NE(error_disk_local_disk_cl_unregister, "Failed unregister from srv's cl (cid=@CID_LLONG)", cid);
		disk->is_cl_reg = false;
	}
	else
		_NE(error_1_disk_local_disk_cl_unregister, "Not registered");

	__NFOUTD;
}

static int local_disk_locks_alloc(struct nvmeibc_disk *disk,
								  struct nvmeibc_disk_segments_locks *disk_segs_locks_local)
{
	int rv = -1;
	__NFIND;

	if (!disk->is_cl_reg) {
		_NE(error_disk_local_disk_locks_alloc, "Not registered");
		goto out;
	}
	/* disk_segs_locks_local->guard should already be locked by caller */
	BUG_ON(!rwsem_is_locked(&disk_segs_locks_local->guard));
	if (!disk_segs_locks_local->lock_ch) {
		_NE(error_1_disk_local_disk_locks_alloc, "No lock channel");
		goto out;
	}
	/* call srv's nvmeibs_client_ldisk_alloc_locks */
	if ((rv = disk->local_server->cl_alloc_locks(
		disk->local_admin_ch->cid, &disk->local)) < 0) {
		_NE(error_2_disk_local_disk_locks_alloc, "Failed to get disk locks");
	}

out:
	__NFOUTD;
	return rv;
}

static void local_disk_locks_free(struct nvmeibc_disk *disk)
{
	struct nvmeib_local_disk *ldisk = &disk->local;
	__NFIND;

	if (ldisk->ldl_a) {
		INIT_LIST_HEAD(&ldisk->ldl_list);
		ldisk->n_ldl = 0;
		kfree(ldisk->ldl_a);
		ldisk->ldl_a = NULL;
	}

	__NFOUTD;
}

static void save_disk_host_name(struct nvmeibc_disk *disk)
{
	struct nvmeibc_ib_admin_channel *ch = get_alive_admin_ch(disk);
	if (ch) {
		memset(disk->disk_host, 0, sizeof(disk->disk_host));
		snprintf(disk->disk_host, sizeof(disk->disk_host), "%s",
			ch->base.base.rhost_name);
		snprintf(disk->full_name, sizeof(disk->full_name), "%.*s-%.*s",
			(int)sizeof(disk->disk_host), disk->disk_host,
			(int)sizeof(disk->name), disk->name);
		_NT(trace_disk_save_disk_host_name, "disk_host=@DISK_HOST   full_name=@FULL_NAME", disk->disk_host, disk->full_name);
	}
}

/* Jared: Removed in favour of using nvmeibc_populate_disk_local_nics on create */
#if 0
static int link_dev(struct nvmeibc_dev *nic_dev, void *args)
{
	struct nvmeibc_disk *disk = args;
	struct nvmeibc_local_nic *ln;
	struct nvmeibc_local_nic_port *lnp;
	struct nvmeibc_ib_port *port;
	int rv = 0;

	NFIN;
	if (list_empty(&nic_dev->port_list)) {
		_iND(link_dev_d1,
			 "NIC @STR list port is empty", nvmeibc_device_name(nic_dev));
		goto out;
	}
	if (!(ln = kzalloc(sizeof(*ln), GFP_KERNEL))) {
		_NE(link_dev_e1, "Fail to allocate local nic cache entry");
		goto out;
	}
	ln->nic_dev = nic_dev;
	INIT_LIST_HEAD(&ln->ports);
	/* try to check if user nics is local using all ports */
	list_for_each_entry(port, &nic_dev->port_list, port_list_n) {
		if (!(lnp = kzalloc(sizeof(*lnp), GFP_KERNEL))) {
			_NE(link_dev_e2, "Fail to allocate local nic port cache entry");
			rv = -ENOMEM;
			goto out;
		}
		lnp->ib_port = port;
		list_add_tail(&lnp->link, &ln->ports);
	}
	list_add_tail(&ln->link, &disk->local_nics);

out:
	NFOUT;
	return rv;
}
#endif

int local_nic_prio_cmp_fn(void *priv, struct list_head *a, struct list_head *b)
{
	struct nvmeibc_local_nic *lnic_a = container_of(a, struct nvmeibc_local_nic, link);
	struct nvmeibc_local_nic *lnic_b = container_of(b, struct nvmeibc_local_nic, link);
	int type_a =  lnic_a->nic_dev->dev->dev_type;
	int type_b = lnic_b->nic_dev->dev->dev_type;

	if (type_a == DT_siw && type_b != DT_siw)
		return 1;
	if (type_b == type_a)
		return 0;
	return -1;
}

/* Jared: Should eventually be renamed shuffle local nics */
static int fill_local_nics(struct nvmeibc_disk *disk)
{
	struct disk_globals *d = __get_dg(disk);
	struct list_head *local_nics;
	struct nvmeibc_local_nic *ln;
	int rv = 0, i, n = 0, p = 0, u = 0;

	__NFIND;
	local_nics = &disk->local_nics;
	list_for_each_entry(ln, local_nics, link) {
		if (NVMEIB_UPDATE_NW_PATHS)
			ln->cold_add = true;
		++n;
	}
	if (likely(n > 0)) {
		n = (atomic_add_return(1, &d->local_nic_shuffle_n) - 1) % n;
		prio_list_rotate_n(local_nics, n, local_nic_prio_cmp_fn, NULL);
	} else {
		rv = -ENODEV;/* fail the discovery, hopefully in the next discover the nic
				will be ready */
		_NT(trace_disk_fill_local_nics, "No local nics at disk @DISK_NAME", disk->name);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_NO_LOCAL_NICS);
		goto out;
	}
	list_for_each_entry(ln, local_nics, link) {
		for (i = 0; i < n; ++i)
			list_rotate_left(&ln->ports);
		p += ln->n_ports;
		u += !!ln->nic_dev->device_used;
	}

	if (u == 0 || p == 0) {
		_NT(trace_1_disk_fill_local_nics, "No local ports at disk @DISK_NAME", disk->name);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_NO_LOCAL_PORTS);
		rv = -ENOENT;
	}



out:
	__NFOUTD;
	return rv;
}

static void __print_arnics(struct list_head *arnics)
{
	struct nvmeibc_admin_rnic *arnic;
	int i = 0;
	NFIN;

	list_for_each_entry(arnic, arnics, link)
		_NT(trace_disk_print_arnics,
		        "[@INT32_02] arnic @IB_GID_IPV6 (l=@LOCAL_INT, "
		        "prio=@PRIORITY, layer=@LINK_LAYER, transport=@TRANSPORT_TYPE)",
		        i, &arnic->ib_gid, arnic->local, arnic->priority.raw,
		        arnic->link_layer, arnic->transport_type);

	NFOUT;
}

static void print_arnics(struct nvmeibc_disk *disk)
{
	__print_arnics(&disk->arnics);
}

static void trace_disk_connection(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link)
		if (arnic->channel && arnic->channel->is_main) {
			nvmeibc_ib_admin_channel_trace_path(ac_to_iac(arnic->channel));
			break;
		}
	__NFOUTD;
}

static int get_local_jrnl_rng(struct nvmeibc_disk *disk)
{
	struct nvmeib_jrange_cache jrc;
	int rv;
	DD_STG_START_WITH_DECLARE(disk, DD_STG_GET_LOCAL_JRNL_RNG);
	__NFIND;

	/* alloc and reset local journal */
	if (!disk->local.jrnl.jmdc) {
		if (!(disk->local.jrnl.jmdc = kcalloc(NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, sizeof(*disk->local.jrnl.jmdc), GFP_KERNEL))) {
			_NE(error_disk_get_local_jrnl_rng, "OOM Error");
			rv = -ENOMEM;
			goto out;
		}
	}
	disk->local.jrnl.valid = false;

	/* build jam cache for serjio */
	nvmeibc_jam_disk_cache_local(disk, &jrc);

	/* nvmeibs_client_ldisk_alloc_jrnl_rng */
	if ((rv = (*disk->local_server->cl_alloc_jrnl_rng)(
		disk->local_admin_ch->cid, &disk->local, &jrc, disk->binje_ulp)) < 0) {
		goto out;
	}

	if (!disk->local.jrnl.valid) {
		_NT(trace_0_get_local_jrnl_rng, "invalid journal reurned");
		rv = -1;
		goto out;
	}

	/* Journal Range from SERJIO */
	disk->jour.rng_id = disk->local.jrnl.rng_idx;
	disk->jour.rng_gen_id = disk->local.jrnl.gen_id;
	disk->jour.rng_slba = disk->local.jrnl.rng_slba;
	disk->jour.rng_nlba = disk->local.jrnl.rng_nlba;
	disk->jour.rng_binje = disk->local.jrnl.rng_binje;
	memcpy(disk->jour.serjio_boot_id,
		   disk->local.jrnl.serjio_boot_id, NVMEIB_GID_STR_MAX);
	disk->jour.n_ents = disk->local.jrnl.n_ents;
	disk->jour.rng_nblk = disk->local.jrnl.rng_nblk;
	disk->jour.max_rng_blk = disk->local.jrnl.max_rng_blk;
	disk->jour.tot_n_rng = disk->local.jrnl.tot_n_rng;

	_NT(trace_1_get_local_jrnl_rng,
		"journal-info: rng: id=@RNG_ID, slba=@SLBA_LLONG, nlba=@NLBA "
		"gen_id=@JRNL_RNG_GEN_ID, binje=@BINJE(@BINJE), "
		"n_ents=@N_ENTS, rng_nblk=@NBLOCKS, max_rng_blk=@NBLOCKS, tot_n_rng=@NUM_RANGES, "
		"serjio_boot_id=@STR, "
		"free_ents_bmp=" NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE "\n",
		disk->jour.rng_id, disk->jour.rng_slba, disk->jour.rng_nlba,
		disk->jour.rng_gen_id, disk->jour.rng_binje, disk->binje_ulp,
		disk->jour.n_ents, disk->jour.rng_nblk, disk->jour.max_rng_blk, disk->jour.tot_n_rng,
		disk->jour.serjio_boot_id, disk->local.jrnl.free_ents_bmp);

	if ((rv = nvmeibc_jam_disk_add(disk,
								   disk->local.jrnl.free_ents_bmp,
								   disk->local.jrnl.jmdc,
								   disk->local.jrnl.ent_md))) {
		_NE(error_1_disk_get_local_jrnl_rng,
			"Fail to add disk @DISK_NAME to jam", disk->name);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_JAM_ADD_FAILED);
	}

out:
	DD_STG_END(disk, DD_STG_GET_LOCAL_JRNL_RNG, rv, NVMEIBC_DISK_DISCOVER_LOCAL_JOURNAL_GET_FAILED);
	__NFOUTD;
	return rv;
}

static void print_local_nics(struct nvmeibc_disk *disk)
{
	struct nvmeibc_local_nic *ln;
	const struct nvmeibc_cinst_params_core *pc = nvmeibc_cinst_get_core_p(disk);
	int i = 0;

	__NFIND;
	list_for_each_entry(ln, &disk->local_nics, link) {
		const struct nvmeibc_dev *c_dev = ln->nic_dev;
		const struct nvmeibc_cinst_params_core *pn = nvmeibc_cinst_get_core_p(c_dev);
		_NT(trace_disk_print_local_nics, "@INT) local nic @IB_DEV_NAME, used=@USED",
		   i++, c_dev->dev->ib_dev->name, c_dev->device_used);
		if (unlikely(pn != pc)) {
			WARN(true, "nvmeibc instances mess nic=%d, disk=%d", nvmeibc_cinst_get_core_inst_num(pn), nvmeibc_cinst_get_core_inst_num(pc));
		}
	}

	__NFOUTD;
}

static void log_discover_statuses(struct nvmeibc_disk *disk) {
	struct nvmeibc_admin_rnic *arnic;

	_NI(trace_nvmeibc_disk_discover_reason, "DTREND: discover disk @DISK_NAME (@DISK) done, status: @DISK_DISCOVER_OP",
	disk->name, disk, NVMEIB_TREND_HEAD(disk->discover_trend).data);

	list_for_each_entry(arnic, &disk->arnics, link) {
		ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_OK);
		_NI(trace_nvmeibc_arnic_discover_reason,
			"DTREND: discover disk @DISK_NAME (@DISK) using arnic @IB_GID_IPV6 done, status: @ARNIC_DISCOVER_OP",
			 disk->name, disk, &arnic->ib_gid, NVMEIB_TREND_HEAD(arnic->discover_trend).data);
	}

}

#define IS_PAUSE_AT_FIRST_DISCOVER_HAPPENING(_disk) (_disk->pause_at_first_discover && !_disk->discover_id)

static int discover(struct nvmeibc_disk *disk, bool is_rediscover)
{
	int rv, inst_num, call_count = 0;
	u64 ts, dt, downtime;
	bool use_local_bypass = nvmeibc_use_local_bypass; /* Keep consistent throught discover flow */
	proc_name_t pname;
	struct nvmeibc_admin_rnic *arnic;
	DD_STG_INIT_JIFFIES;
#ifdef LOW_MEM
	unsigned char rand;
#endif

	__NFIND;
	if (unlikely(disk->discover_id == INT_MAX))
		disk->discover_id = 0;
	else
		disk->discover_id++;
	ts = jiffies;
	nvmeib_trend_insert(&disk->discover_trend, NVMEIBC_DISK_DISCOVER_UNKNOWN);
	list_for_each_entry(arnic, &disk->arnics, link) {
		nvmeib_trend_insert(&arnic->discover_trend, NVMEIBC_ARNIC_DISCOVER_UNKNOWN);
	}

	inst_num = nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(disk));
	_NT(trace_disk_discover,
		"DISCOVER @CLNT_INSTANCE_ID, DISCOVER_ID=@DISCOVER_ID, disk @DISK_FULL_NAME-->",
		inst_num, disk->discover_id, disk->full_name);

	if (disk->force_pause) {
		_NE(trace_force_pause_disk,
			"DEBUG: Force pause discover of disk @DISK_FULL_NAME", disk->full_name);
		rv = -1;
		goto out;
	}

	if (IS_PAUSE_AT_FIRST_DISCOVER_HAPPENING(disk)){
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_PAUSE_AT_FIRST_DISCOVER);
		rv = -1;
		_NT(nvmeibc_trace_disk_paused_at_first_attempt, "DISCOVER: Disk @DISK_FULL_NAME is paused as this is first discover attempt", disk->full_name);
		goto out;
	}

	nvmeibc_disk_cmds_stats_reset(disk);
	nvmeibc_disk_net_intrs_stats_reset(disk);
	BUG_ON(!list_empty(&disk->ioch_drained_pending_list));
	INIT_LIST_HEAD(&disk->rionics);
	INIT_LIST_HEAD(&disk->nr_rionics);
	INIT_LIST_HEAD(&disk->ioch_kill_list);
	INIT_LIST_HEAD(&disk->local_defer_io_list);
	nvmeib_init_state_guard(&disk->local_defer_io_work_state, LOCAL_DEFER_WORK_IDLE);
	atomic_set(&disk->local_defer_io_outstanding, 1);
	atomic_set(&disk->n_reused, 0);
	disk->cid = 0;
	disk->max_rdda_bb = disk->max_nordda_bb = disk->max_gen_cmd_bb = 0;
	disk->min_nordda_bb = disk->min_gen_cmd_bb = 0;
	disk->no_io_time = 0;
	atomic_set(&disk->connected_io_channels, 0);
	/* we set it for the whole disk strucutre lifetime */
	disk->io_ka_only_no_rdda = nvmeibc_ioch_ka_only_no_rdda;

	if (NVMEIBC_NRCH_DEFER_COMPLETE_IOCMD) {
		if (!nvmeibc_disk_nrch_defer_block_cb && !nvmeibc_gf_calc_in_irq_ctx()) {
			/* On ARM Kernel EC calculation requires interrupt_enabled  */
			rv = -1;
			_NE_dmesg(nvmeibc_disk_nrch_defer_block_cb_off_arm, "@DISK_FULL_NAME - won't be added as nvmeibc_disk_nrch_defer_block_cb is off", disk->full_name);
			goto out;
		}
		disk->defer_block_cb_on_io_cmd = nvmeibc_disk_nrch_defer_block_cb;
	}
	if (NVMEIBC_LOCAL_DEFER_COMPLETE_IOCMD) {
		if (!nvmeibc_disk_local_defer_block_cb && !nvmeibc_gf_calc_in_irq_ctx()) {
			/* On ARM Kernel EC calculation requires interrupt_enabled  */
			rv = -1;
			_NE_dmesg(nvmeibc_disk_lock_defer_block_cb_off_arm, "@DISK_FULL_NAME - won't be added as disk_local_defer_block_cb is off", disk->full_name);
			goto out;
		}
		disk->local_defer_block_cb_on_io_cmd = nvmeibc_disk_local_defer_block_cb;
	}

	/* Jared: Local Nics are now initialized on create *
	 *INIT_LIST_HEAD(&disk->local_nics);*/

	/* get the current nics and ports */
	if ((rv = fill_local_nics(disk)) < 0)
		goto out;

	/* mark all local arnics */
	if ((rv = mark_local_arnics(disk)) < 0)
		goto out;

	print_local_nics(disk);
	print_arnics(disk);

/* Jared: Disabling this - if we only have a single nic and it goes down,
 *				we can't contact the disk */
#if 0
	/* drop duplicate local nics - leave one */
	if ((rv = drop_dup_local_arnics(disk)) < 0)
		goto out;
#endif

	/* check if local srv hosts this disk */
	disk->is_local = is_local_disk(disk);
	if (disk->is_local && (use_local_bypass && !disk->md_extd)) {
		_NT(trace_1_disk_discover, "Accessing local disk @DISK_NAME using bypass", disk->name);
		disk->access_local = true;
	} else {
		_NT(trace_A_disk_discover, "Accessing @LOCAL disk @DISK_NAME through network", (disk->is_local) ? "local" : "remote", disk->name);
		disk->access_local = false;
	}
	disk->local_admin_ch = NULL;

	/* Create ib-admin-channel(s) obj (set arnic->channel =):
	  - Remote Disk, Done foreach remote arnic residing on the disk's node.
	  - Local  Disk, Done for THE local arnic. sets is_main, alive and ioch.
	 */
	if ((rv = create_admin_channels(disk)) < 0)
		goto out;

#ifdef LOW_MEM
	//wait a bit to prevent sm flooding
	get_random_bytes(&rand, sizeof(rand));
	_NT(trace_B_disk_discover, "sleeping for @RV msecs", rand * 5);
	msleep(rand * 5);
#endif

	rv = alloc_dirty_bits_mem(disk);
	if (rv) {
		goto out;
	}

	if (!disk->access_local) {
		DD_STG_START(disk, DD_STG_MAP_DBITS_TO_NICS);
		rv = call_for_each_lnic(disk, map_ec_dirty_bits_to_nic, disk);
		DD_STG_END(disk, DD_STG_MAP_DBITS_TO_NICS, rv, NVMEIBC_DISK_DISCOVER_DBITS_MEM_ALLOCATION_FAILED);
		if (rv < 0) {
			goto out;
		}
	}

	/* Try to access (rdma-find-path) each disk's arnics, that have channel,
	   from each local port of the disk lnics. The first lnic to succeed,
	   sets that arnic's ch->net.base.port with that lnic's src-port */
	DD_STG_START(disk, DD_STG_SET_ARNIC_PORTS_WITH_LNIC_SRC_PORT);
	if ((rv = call_for_each_lport(disk, discover_using_port, disk,
		true, true, &call_count)) < 0 || call_count == 0){
			if (call_count == 0) {
				_NT(trace_2_disk_discover, "Disk @DISK_NAME (@DISK) has no used and active local ports", disk->name, disk);
				DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_ARNIC_NO_PORTS);
				rv = -ENOENT;
			}
		}
	DD_STG_END(disk, DD_STG_SET_ARNIC_PORTS_WITH_LNIC_SRC_PORT, ((!(rv < 0 || call_count == 0)) ? 0: -1), -1);
	if (rv < 0 || call_count == 0)
		goto out;


	/* Check if at least one of the above find-paths succeeded,
	   i.e. we can really access the disk */
	if ((rv = check_arnics_access(disk)) < 0)
		goto out;

	/* Foreach (arnic w/) admin-ch:
	   - Connect admin-ch to srv (allocate net and 3-way handshake login),
	     from the (local) port chosen above.
	   - read-io:
			* send GET_IO and parse rsp (disk-info and per unfiltered nic info).
	    	* Foreach remote gid (port) in rsp,
			  alloc and add rionic to &ch->base.rionics
	   - set arnic->alive.
	   - sort arnics by these criteria order:
	    	(1) preffered user's rnic
	    	(2) priority (numa-distance)
	    	(3) num-conns (on remote) and
			(4) order in disk's arnics dup list (decrement every rediscovery)
	*/
	if ((rv = read_all_io_rscs(disk)) < 0)
		goto out;

	if (disk->access_local) {
		_ND(trace_3_disk_discover, "Local disk cid is @CID_LLONG", disk->local_admin_ch->cid);

		clnt_proc_name_format(pname, 'C', "WQ", "LDiskGn", inst_num);
		if (!(disk->local_gen_wq = wq_create(pname))) {
			DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_GEN_WQ_ALLOCATION_FAILED);
			rv = -ENOMEM;
			goto out;
		}
		atomic_set(&disk->local_gen_wq_cnt, 1);

		if ((rv = local_disk_cl_register(disk)) < 0)
			goto out;

		nvmeib_io_stats_set_block_size(disk->stats, 1 << disk->sector_shift);

		if (disk->md_size > 0 && disk->md_extd && !DISK_ALLOW_INLINE_MD) {
			_NE(error_disk_discover_local_inline_md, "Disk @DISK_NAME (@DISK) has unsupported inline-MD", disk->name, disk);
			DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_INLINE_MD_UNSUPPORTED);
			rv = -ENOTSUPP;
			goto out;
		}

		if ((rv = alloc_percpu_dma_pools(disk)) < 0) {
			goto out;
		}

		if (nvmeibc_disk_local_write_use_data_copy && !disk->local.external)
			disk->local.local_io_use_data_copy = true;

		/* Use a prpl for local-io (instead of sgl). Solves issues with Intel IOMMU on older kernels. Not relevant for external drives. */
		disk->local.local_io_use_prpl = nvmeibc_disk_local_io_use_prpl && !disk->local.external;
		if (disk->local.local_io_use_prpl) {
			if ((rv = alloc_local_io_prpl_pool(disk)) < 0)
				goto out;
		}
		/* Use a DMA-Pool for the unused data pages of the read-metadata (or rmw-metadata) operation. Not relevant for external drives. */
		disk->local.local_io_use_rd_md_pool = nvmeibc_disk_local_io_use_rd_md_pool && !disk->local.external;
		if (disk->local.local_io_use_rd_md_pool) {
			if ((rv = alloc_local_io_rd_md_pool(disk)) < 0)
				goto out;
		}
		/* Use a DMA-Pool for the dummy MD areas for IO operations. Not relevant for external drives or drives with inline MD. */
		disk->local.local_io_use_md_dma_pool = nvmeibc_disk_local_io_use_md_dma_pool && disk->md_size > 0 && !disk->md_extd && !disk->local.external;
		if (disk->local.local_io_use_md_dma_pool) {
			if ((rv = alloc_local_io_md_dma_pool(disk)) < 0)
				goto out;
		}
		if (disk->local.local_io_use_data_copy) {
			if ((rv = alloc_local_io_data_pool(disk)) < 0)
				goto out;
		}
		if (disk->md_size > 0 && !disk->md_extd) {
			/* Allocate dummy MD areas for drive */
			disk->local.dummy_md_dma_dev = disk->local_server->dma_device(&disk->local);
			if (!(disk->local.dummy_md_read_ptr = dma_alloc_coherent(
				disk->local.dummy_md_dma_dev, PAGE_SIZE, &disk->local.dummy_md_read_addr, GFP_KERNEL)))
			{
				_NE(error_disk_discover_local_dummy_md_read_alloc,
				"Disk @DISK_NAME (@DISK) failed to allocate dummy MD read area", disk->name, disk);
				DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_MEM_ALLOCATION_FAILURE);
				disk->local.dummy_md_read_addr = 0;
				rv = -ENOMEM;
				goto out;
			}
			if (!(disk->local.dummy_md_write_ptr = dma_alloc_coherent(
				disk->local.dummy_md_dma_dev, PAGE_SIZE, &disk->local.dummy_md_write_addr, GFP_KERNEL)))
			{
				_NE(error_disk_discover_local_dummy_md_write_alloc,
				"Disk @DISK_NAME (@DISK) failed to allocate dummy MD write area", disk->name, disk);
				DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_MEM_ALLOCATION_FAILURE);
				disk->local.dummy_md_write_addr = 0;
				rv = -ENOMEM;
				goto out;
			}
		}
#if 0
		/* create toma interface over the disk's admin ch */
		rv = nvmeibc_disk_toma_create(disk, is_rediscover);
		if (rv >= 0) {
			rv = start_local_lock_channel(disk);
		}
#else
		/* first start lock channel then start toma, otherwise
		   start lock channel may race over the ch->send_ioctx
		   with the toma recv path which runs on admin-ch wq */
		if ((rv = start_local_lock_channel(disk)) < 0)
			goto out;

		if (disk->md_size > 0) {
			if ((rv = get_local_jrnl_rng(disk)) < 0)
				goto out;
		}

		DD_STG_START(disk, DD_STG_NVMEIBC_DISK_TOMA_CREATE_LOCAL);
		rv = nvmeibc_disk_toma_create(disk, is_rediscover);
		DD_STG_END(disk, DD_STG_NVMEIBC_DISK_TOMA_CREATE_LOCAL, rv, -1);
		if (rv < 0)
			goto out;
#endif
		save_disk_host_name(disk);
		goto out;
	}

	/* Try to access rionics from all ports of each &disk->local_nics:
	 *
	 * From each port (gid) of each local nic, try to access (rdma find-path)
	 * every rionic of each alive arnic's admin-channel. i.e. for each
	 * {rionic, local-port} pair:
	 *
	 *  - if rionic is NOT already in disk->rionics list add it.
	 *  - if rionic NOT already paired with this local-port
	 *    > create lionic for this local-port, and;
	 *    > cross-ref lionic with rionic.
	 *    > inc this admin-ch's number of rionic accessible from it.
	 */
	DD_STG_START(disk, DD_STG_ACCESS_RIONICS_USING_LPORTS);
	if ((rv = call_for_each_lport(disk, access_using_port, disk,
		false, false, &call_count)) < 0 || call_count == 0){
			if (call_count == 0) {
				_NT(trace_4_disk_discover, "Disk @DISK_NAME (@DISK) has no used and active local ports", disk->name, disk);
				rv = -ENOENT;
			}
		}
	DD_STG_END(disk, DD_STG_ACCESS_RIONICS_USING_LPORTS, ((!(rv < 0 || call_count == 0)) ? 0 : -1), NVMEIBC_DISK_DISCOVER_ACCESS_RIONICS_FAILED);
	if (rv < 0 || call_count == 0)
		goto out;

	/* init disk->pcpu_nrchs */
	pcpu_nrch_init_mode(disk);

	/* we tried to connect with all rionics so now it is the time to check
	   that all user disks are accessible
	*/
	if ((rv = check_disks_access(disk)) < 0)
		goto out;

	/* all remote disks are accessible so send collected information of
	   all user nics and get back access instructions:

	   1) Foreach admin-ch that can access rionic(s),
	      - Build access-map
	      - Parse access-map rsp:
	    	* init admin-ch's reqs[]
	    	* read disks, nics, and rscs
	    	* read locks info and connect lock-ch
	    	* create io-ch per {rionic, lionic, qp} triplet.
	   2) Disconnect admin-ch that cant access any rionic
	 */
	if ((rv = create_access_map(disk ,is_rediscover)) < 0)
		goto out;

	nvmeib_io_stats_set_block_size(disk->stats, 1 << disk->sector_shift);

	if (disk->md_size) {
		if (disk->md_extd && !DISK_ALLOW_INLINE_MD) {
			_NE(error_disk_discover_inline_md, "Disk @DISK_NAME (@DISK) has unsupported inline-MD", disk->name, disk);
			DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_INLINE_MD_UNSUPPORTED);
			rv = -ENOTSUPP;
			goto out;
		}
		if ((rv = get_journal_range(disk)) < 0)
			goto out;
	}

	/* init disk->is_tcp */
	if ((rv = disk_is_transport_tcp(disk))) {
		goto out;
	}

	/* request disk resources - we must protect it as this one runs
	   inside the main work queue and server get/put commands run
	   inside the disk work queue
	   Here we also set the main admin-ch (arnic->channel's is_main).
	*/
//	mutex_lock(&disk->locate_guard);
	rv = request_disks_resources(disk);
//	mutex_unlock(&disk->locate_guard);
	if (rv < 0)
		goto out;


	/* create toma interface over the disk's admin ch */
	DD_STG_START(disk, DD_STG_NVMEIBC_DISK_TOMA_CREATE_REMOTE);
	rv = nvmeibc_disk_toma_create(disk, is_rediscover);
	DD_STG_END(disk, DD_STG_NVMEIBC_DISK_TOMA_CREATE_REMOTE, rv, -1);
	if (rv < 0){
		goto out;
	}

	disk->no_io_time = jiffies;
	nvmeibc_disk_start_io_channels(disk, true);
	if ((rv = nvmeibc_disk_start_periodic_ioch_starter(disk))) {
		goto out;
	}
	save_disk_host_name(disk);
	trace_disk_connection(disk);

out:
	if (!rv) {
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_OK);
		if (nvmeibc_disk_lock_channel_periodic_timer_interval > 0)
			queue_delayed_work(system_unbound_wq, &disk->periodic_lock_channel_work,
					   msecs_to_jiffies(nvmeibc_disk_lock_channel_periodic_timer_interval));
	}
	if(!rv != (NVMEIB_TREND_HEAD(disk->discover_trend).data == NVMEIBC_DISK_DISCOVER_OK)) {
		_NW(trace_discover_reason_not_match, "DTREND: RV is @RV but discover trend is @DISK_DISCOVER_OP",
		rv, NVMEIB_TREND_HEAD(disk->discover_trend).data);
	}
	log_discover_statuses(disk);

	disk->discover_rv = rv;
	dt = jiffies - ts;
	downtime = jiffies - disk->disconnect_jif;

	_NT(trace_5_disk_discover,
		"DISCOVER @CLNT_INSTANCE_ID, DISCOVER_ID=@DISCOVER_ID, disk @DISK_FULL_NAME <-- "
		"(rv @RV, n=@INT, dt=@DISK_FUNC_DT(@DISK_FUNC_DT)), "
		"downtime=@LLU(@LLU)",
		inst_num, disk->discover_id, disk->full_name, rv, disk->restart_num, dt, dt / HZ,
		downtime, downtime / HZ);

	__NFOUTD;
	return rv;
}

static void complete_disk_pause(void *v)
{
	struct nvmeibc_disk *disk = v;
	int rv;

	__NFIND;
	/*spin_lock(&disk->spinlock);*/
	if ((rv = atomic_dec_return(&disk->n_volumes_paused)) <= 0) {
		if (rv < 0)
			_NW(warn_disk_complete_disk_pause, "Block module over triggered the disk @DISK_NAME (@DISK) pause "
				"completion (@RV)", disk->name, disk, rv);
		_NT(trace_disk_complete_disk_pause, "Pause ack disk @DISK_NAME(@DISK)", disk->name, disk);
		complete(&disk->disk_paused);
	}
	/*spin_unlock(&disk->spinlock);*/
	__NFOUTD;
}

#define MAX_WAIT_IOCH_DRAINED (2*HZ)


#define	_NTch(_tarce_name, _ch, fmt, ...) _NT(_tarce_name, \
	"ch @CH_NAME (@PTR), cs_gid=@LLU, link=@BOOL, list=@BOOL, srv=@INT, " \
	"dt=@LU: " fmt, _ch->name, _ch, _ch->bailed_cmds.cs_gid, \
	list_empty(&_ch->bailed_cmds.link), list_empty(&_ch->bailed_cmds.list), \
	_ch->bailed_cmds.srv_drained, jiffies - _ch->bailed_cmds.arm_jif, \
	## __VA_ARGS__)

static void ioch_drained_complete_cmds(struct nvmeibc_disk *disk,
								struct list_head *list)
{
	struct nvmeibc_disk_command *dcmd;
	int n = 0;
	NFIN;

	while ((dcmd = list_first_entry_or_null(list,
			struct nvmeibc_disk_command, dcmd_link))) {
		list_del_init(&dcmd->dcmd_link);
		n++;
		_NT(trace_0_ioch_drained_complete_cmds,
			"Disk @DISK_NAME (@DISK), dcmd=@PTR, cmd_type=@STR(@INT)",
			disk->name, disk, dcmd, "???" /*disk_cmd_to_str(dcmd)*/,
			dcmd->cmd_type);

		if (dcmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
			nvmeibc_ib_net_complete_bcmd(dcmd, STATS_DONE_LLP_COMPLETE_IOCH_DRAINED, NULL);
		}
		else if (dcmd->cmd_type == NVMEIBC_DISK_CMD_GEN) {
			nvmeibc_ib_net_nordda_complete_gcmd(dcmd, STATS_DONE_LLP_COMPLETE_IOCH_DRAINED, NULL);
		}
		else if (dcmd->cmd_type == NVMEIBC_DISK_CMD_LOCK) {
			nvmeibc_ib_net_nordda_complete_lcmd(disk_to_lock(dcmd), STATS_DONE_LLP_COMPLETE_IOCH_DRAINED, -EIO);
		}
		else {
			BUG();
		}
	}

	_NT(trace_1_ioch_drained_complete_cmds,
		"Disk @DISK_NAME (@DISK), @INT cmds completed",
		disk->name, disk, n);
	NFOUT;
}

bool nvmeibc_disk_ioch_drained_is_pending(struct nvmeibc_channel *ch)
{
	/* dont care if ch is inuse */
	return !list_empty(&ch->bailed_cmds.link);
}

bool nvmeibc_disk_ioch_drained_is_empty(struct nvmeibc_channel *ch)
{
	return list_empty(&ch->bailed_cmds.list) ||
		/* kzalloc'ed but not init yet */
		(!ch->bailed_cmds.list.next &&
		 !ch->bailed_cmds.list.prev);
}

static void ioch_drained_done_(struct nvmeibc_channel *ch)
{
	_NTch(trace_0_ioch_drained_done_, ch,
		  "from @__BUILTIN_RETURN_ADDRESS_FUNC", __builtin_return_address(0));

	BUG_ON(!list_empty(&ch->bailed_cmds.link));
	BUG_ON(!list_empty(&ch->bailed_cmds.list));
	if (++ch->bailed_cmds.cs_gid == 0)
		++ch->bailed_cmds.cs_gid;
	ch->bailed_cmds.arm_jif = 0;
	if (ch->bailed_cmds.srv_drained == 1)
		ch->bailed_cmds.srv_drained--;
	else
		WARN(ch->bailed_cmds.srv_drained, ": %d", ch->bailed_cmds.srv_drained);
}

static void ioch_drained_on_net_detached_(struct nvmeibc_disk *disk, void *info,
										  struct list_head *ret_list)
{
	struct nvmeibc_disk_ioch_drained_event_net_detached_info *i = info;
	struct nvmeibc_channel *ch = i->ch;
	NFIN;

	_NTch(trace_0_ioch_drained_on_net_detached_, ch,
		  "i->list=@BOOL", list_empty(i->list));

	BUG_ON(!list_empty(ret_list));
	BUG_ON(!list_empty(&ch->bailed_cmds.list));

	/* @i->list empty means either all ioch's cmds where completed by srv or
	   connect sequence did not reach the point where srv sends ioch-drained.
	   --> skip waiting for srv's ack */

	if (list_empty(i->list) || ch->bailed_cmds.srv_drained) {
		list_del_init(&ch->bailed_cmds.link);
		list_splice_init(i->list, ret_list);
		ioch_drained_done_(ch);
	}
	else {
		list_splice_init(i->list, &ch->bailed_cmds.list);
		ch->bailed_cmds.arm_jif = jiffies;
		list_add_tail(&ch->bailed_cmds.link, &disk->ioch_drained_pending_list);
	}

	NFOUT;
}

static void ioch_drained_on_srv_approval_(struct nvmeibc_disk *disk, void *info,
										  struct list_head *ret_list)
{
	struct nvmeibc_disk_ioch_drained_event_srv_approval_info *i = info;
	struct nvmeibc_channel *ch = i->ch;
	NFIN;

	_NTch(trace_0_ioch_drained_on_srv_approval_, ch,
	   "i->cs_gid=@LLU", i->cs_gid);

	BUG_ON(!list_empty(ret_list));

	if (ch->bailed_cmds.cs_gid != i->cs_gid) {
		WARN_ON_ONCE(ch->bailed_cmds.cs_gid != i->cs_gid + 1);
	}
	else {
		WARN(ch->bailed_cmds.srv_drained, ": %d", ch->bailed_cmds.srv_drained);
		ch->bailed_cmds.srv_drained++;

		if (!list_empty(&ch->bailed_cmds.link)) {
			list_del_init(&ch->bailed_cmds.link);
			list_splice_init(&ch->bailed_cmds.list, ret_list);
			ioch_drained_done_(ch);
		}
	}

	NFOUT;
}

static bool ioch_drained_on_timer_(struct nvmeibc_disk *disk)
{
	struct ioch_bailed_cmds *b;
	struct nvmeibc_channel *ch;
	NFIN;

	if ((b = list_first_entry_or_null(&disk->ioch_drained_pending_list,
									  struct ioch_bailed_cmds, link)) &&
		 time_after(jiffies, b->arm_jif + MAX_WAIT_IOCH_DRAINED)) {
		ch = container_of(b, struct nvmeibc_channel, bailed_cmds);
		_NTch(trace_0_ioch_drained_on_timer_, ch, "timeout");
		return true;
	}
	else {
		return false;
	}

	NFOUT;
}

static void ioch_drained_on_disk_release(struct nvmeibc_disk *disk,
								  struct list_head *pending_list)
{
	struct ioch_bailed_cmds *b;
	struct nvmeibc_channel *ch;
	LIST_HEAD(list);
	int i = 0;
	NFIN;

	_NT(trace_0_ioch_drained_on_disk_release,
		"Disk @DISK_NAME (@DISK), complete pending bailed-cmds",
		disk->name, disk);

	while ((b = list_first_entry_or_null(pending_list,
			struct ioch_bailed_cmds, link))) {
		ch = container_of(b, struct nvmeibc_channel, bailed_cmds);
		_NTch(trace_1_ioch_drained_on_disk_release, ch,
			  "[@INT32_02], b=@PTR", i, b);
		list_del_init(&b->link);
		BUG_ON(!list_empty(&list));
		list_splice_init(&b->list, &list);
		ioch_drained_done_(ch); /* only this ctx can access @ch now, nolock */
		ioch_drained_complete_cmds(disk, &list);
	}

	_NT(trace_2_ioch_drained_on_disk_release,
		"Disk @DISK_NAME (@DISK), found @INT pending bailed-cmds",
		disk->name, disk, i);

	NFOUT;
}

static void disk_ioch_drained_event_handler(struct nvmeibc_disk *disk,
	enum nvmeibc_disk_ioch_drained_event event, void *info)
{
	unsigned long flags;
	LIST_HEAD(list);
	bool timeout = false;
	int dying;
	NFIN;

	spin_lock_irqsave(&disk->spinlock, flags);
	dying = atomic_read(&disk->dying);
	_NT(trace_0_nvmeibc_disk_ioch_drained_event_handler,
		"Disk @DISK_NAME (@DISK), evt=@STR(@IOCH_DRAINED_EVT), dying=@INT",
		disk->name, disk, nvmeibc_disk_ioch_drained_event_to_str(event), event,
		dying);

	if (dying)
		goto unlock;

	switch (event) {
	case NVMEIBC_DISK_IOCH_DRAINED_EVENT_NET_DETACHED:
		ioch_drained_on_net_detached_(disk, info, &list);
		break;
	case NVMEIBC_DISK_IOCH_DRAINED_EVENT_SRV_APPROVAL:
		ioch_drained_on_srv_approval_(disk, info, &list);
		break;
	case NVMEIBC_DISK_IOCH_DRAINED_EVENT_TIMER:
		timeout = ioch_drained_on_timer_(disk);
		break;
	default:
		_NE(trace_1_nvmeibc_disk_ioch_drained_event_handler,
			"Disk @DISK_NAME (@DISK), unknown evt=@IOCH_DRAINED_EVT",
			disk->name, disk, event);
		break;
	}


unlock:
	spin_unlock_irqrestore(&disk->spinlock, flags);

	if (!list_empty(&list))
		ioch_drained_complete_cmds(disk, &list);

	if (timeout)
		nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_IOCH_DRAINED_EVENT);

	NFOUT;
}

void nvmeibc_disk_ioch_handoff_bailed_cmds(struct nvmeibc_channel *ch,
										   struct list_head *list)
{
	struct nvmeibc_disk_ioch_drained_event_net_detached_info info =
		{ .ch = ch, .list = list };
	bool old_srv;
	NFIN;

	if ((old_srv = nvmeib_version_protocol_lt(
		&ch->disk->last_tgt_ver, &nvmeib_2p1_version))) {
		 _NT(trace_1_ioch_drained_on_net_detached_,
			 "omit wait srv-ack from older server");
	}
	else {
		disk_ioch_drained_event_handler(ch->disk,
			NVMEIBC_DISK_IOCH_DRAINED_EVENT_NET_DETACHED, &info);
	}

	if (!list_empty(list)) {
		WARN_ON_ONCE(!atomic_read(&ch->disk->dying) && !old_srv);
		ioch_drained_complete_cmds(ch->disk, list);
	}

	NFOUT;
}

static struct nvmeibc_channel *ioch_drained_lookup_ch(struct nvmeibc_disk *disk,
	struct volume_server_cmd_ioch_drained_req *i_req)
{
	union ib_gid *lgid = (void *)i_req->c_hw_gid;
	union ib_gid *rgid = (void *)i_req->s_hw_gid;
	u16 ch_num = be16_to_cpu(i_req->ch_num);
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	struct nvmeibc_ib_nordda_channel *nrch;
	struct nvmeibc_channel *ch = NULL;
	NFIN;

	_NT(trace_0_ioch_drained_lookup_ch,
		"Disk @DISK_NAME (@DISK), l=@HW_GID->r=@HW_GID, ch_num=@INT, rdda=@BOOL",
		disk->name, disk, lgid, rgid, ch_num, i_req->is_rdda);

	if (i_req->is_rdda) {
		/* RDDA removed */
	}
	else {
		list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
			if (memcmp(rgid, &rionic->hw_gid, sizeof(*rgid)) != 0)
				continue;
			list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
				if (memcmp(lgid, &lionic->port->gid.hw_gid, sizeof(*lgid)) != 0)
					continue;
				if (ch_num < lionic->n_nr_qps) {
					nrch = lionic->nr_channels + ch_num;
					ch = &nrch->base;
				}
				else {
					_NE(trace_2_ioch_drained_lookup_ch,
						"OOPS, @INT vs. n=@INT", ch_num, lionic->n_nr_qps);
					goto out;
				}
			}
		}
	}

out:
	NFOUT;
	return ch;
}

/* This req can only arrive after disk is connected && is not
   processed if disk is already dying -> not switching to disk-wq.
   This also allow traversing the disk's rionic/lionic lists w/o
   acquireing disk's spinlock */
int nvmeibc_disk_handle_ioch_drained(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_admin_channel *ach, struct volume_server_req *req)
{
	struct volume_server_cmd_ioch_drained_req *i_req = &req->i_req;
	struct nvmeibc_disk_ioch_drained_event_srv_approval_info info;
	struct nvmeibc_channel *ch;
	int rv;
	NFIN;

	if (!on_wq(ach->base.remove_wq)) {
		_NE(trace_0_nvmeibc_disk_handle_ioch_drained, "Wrong wq");
		rv = -EINVAL;
	}
	else if (nvmeib_version_protocol_lt(&disk->last_tgt_ver,
										&nvmeib_2p1_version)) {
		_NT(trace_1_nvmeibc_disk_handle_ioch_drained,
			"unexpected from older server");
		rv = -EINVAL;
		WARN_ON_ONCE(1);
	}
	else if (!(ch = ioch_drained_lookup_ch(disk, i_req))) {
		rv = -ENOENT;
		WARN_ON_ONCE(1);
	}
	else {
		info.ch = ch;
		info.cs_gid = be64_to_cpu(i_req->cs_gid);
		disk_ioch_drained_event_handler(disk,
			NVMEIBC_DISK_IOCH_DRAINED_EVENT_SRV_APPROVAL, &info);
		rv = 0;
	}

	NFOUT;
	return rv;
}

static void ioch_drained_complete_all(struct nvmeibc_disk *disk)
{
	unsigned long flags;
	LIST_HEAD(list);
	NFIN;

	/* Here, clnt may have bailed on (io/gen/lock) cmds that may still be wip
	   at srv-side. We can call blk-comp cb for such cmds as:
	   1. ULP can NOT send any more msgs, specifically UNREG that allows Toma to:
	   1.1 [IO   ] Convert client's locks to stale-special and by that allowing
	               IO to the same blkset (before current wip IO complete).
	   1.2 [LOCK ] Scan registrant's locks before all acquire-lock cmds had
	               completed and i.e leaving stale-lock behind.
	   1.3 [GEN  ] Start recoveries while journal's are changing, etc.

	   2. Toma handles the client-disconnect event **after** all clnt's cmds had
	      been completed/drained

	   Note this is done before killing lock channel as nordda channle may be
	   executing srv-side lock-ops for lock-ch
	*/

	BUG_ON(!atomic_read(&disk->dying));
	spin_lock_irqsave(&disk->spinlock, flags);
	list_splice_init(&disk->ioch_drained_pending_list, &list);
	spin_unlock_irqrestore(&disk->spinlock, flags);

	ioch_drained_on_disk_release(disk, &list);

	/* at this point there are still some iochs that have outstanding cmds
	   that will be completed directly to ulp either due to explicit recv
	   completions from srv or from remove-work due to wd-timeout,
	   net-disconnect, etc. Thus, we are ready to wait for ulp's pause-ack */

	NFOUT;
}

static void __pending_cmd_abort(struct nvmeibc_disk *disk,
								struct nvmeibc_disk_command *disk_cmd,
								bool is_timeout, unsigned long now)
{
	struct nvmeibc_disk_io_command *block_cmd;
	struct nvmeibc_lock_opr_in_progress *opr;
	NFIN;

	/* stats */
	nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_REMOTE_PENDING_DROP);
	if (is_timeout)
		nvmeibc_disk_cmds_stats_pending_timeout(disk, disk_cmd);
	else
		nvmeibc_disk_cmds_stats_pending_aborted(disk, disk_cmd);

	/* complete cmd by type */
	switch (disk_cmd->cmd_type) {
	case NVMEIBC_DISK_CMD_IO:
		block_cmd = disk_to_block(disk_cmd);
		block_cmd->comp.comp_code = -ENXIO;
		nvmeibc_block_cmd_status_debug(block_cmd, NVMEIBC_BLOCK_CMD_REMOTE_PENDING_DROP);
		nvmeibc_block_completion(&block_cmd->comp); 					//abort pending on timed-out or disk/nrch disconnect
		break;

	case NVMEIBC_DISK_CMD_GEN:
		nvmeibc_disk_gen_cmd_completion(disk_to_gen(disk_cmd), -ENXIO); //abort pending on timed-out or disk/nrch disconnect
		break;

	case NVMEIBC_DISK_CMD_LOCK:
		opr = disk_to_opr(disk_to_lock(disk_cmd));
		_NT(t1_pending_cmd_abort,
			"Abort lock-cmd for disk @DISK_NAME time-passed=@LD "
			"(timeout: @LD)",
			disk->name, now - opr->jiffies_start, opr->opr_timeout);
		nvmeibc_locks_channel_lock_cmd_completion(
			disk_to_lock(disk_cmd), -ENXIO, LOCK_OPR_BYPASS_IN_PENDING); //abort pending on timed-out or disk/nrch disconnect
		break;

	default:
		_NE(e0_pending_cmd_abort,
			"Unknown disk command type @CMD_TYPE", disk_cmd->cmd_type);
		BUG();
		break;
	}

	NFOUT;
}

static void pending_cmds_abort(struct nvmeibc_disk *disk,
							   struct list_head *list,
							   bool is_timeout)
{
	struct nvmeibc_disk_command *disk_cmd, *t;
	unsigned long now = jiffies;
	NFIN;

	_NT(t0_pending_cmd_abort, "Abort pending disk-cmds of disk @DISK_NAME, "
							  "called from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		disk->name, __builtin_return_address(0));

	WARN_ON_ONCE(irqs_disabled()); /* recommendation */

	list_for_each_entry_safe(disk_cmd, t, list, dcmd_link) {
		list_del_init(&disk_cmd->dcmd_link);
		__pending_cmd_abort(disk, disk_cmd, is_timeout, now);
	}

	NFOUT;
}

struct report_error_on_pending_pcpu_params {
	struct nvmeibc_disk *disk;
	struct list_head *pcpu_pending_cmds;
};

static void report_error_on_pending_pcpu_fn(void *ctx)
{
	struct report_error_on_pending_pcpu_params *params = ctx;
	struct nvmeibc_disk *disk = params->disk;
	struct nvmeibc_disk_info *info = disk->info;
	int cpu;
	unsigned long flags;

	cpu = get_cpu();

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		goto put_cpu;

	local_irq_save(flags);

	list_splice_tail_init(&info->pcpu_nrchs[cpu].pending_disk_cmds, &params->pcpu_pending_cmds[cpu]);
	info->pcpu_nrchs[cpu].n_pending = 0;
	info->pcpu_nrchs[cpu].max_pending = 0;

	local_irq_restore(flags);

put_cpu:
	put_cpu();
}

static void report_error_on_pending(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *info = disk->info;
	unsigned long flags;
	struct nvmeibc_disk_segments_locks *seg_locks;
	LIST_HEAD(list);
	int i;
	NFIN;

	/* must run before nvmeibc_disk_locks_abort_all_oprs() */
	ioch_drained_complete_all(disk);

	if (disk->is_local && disk->local_server)
		/* nvmeibs_bail_local_async_cookie_ch */
		disk->local_server->bail_async_cookie_ch(disk->cid);

	if (info) {
		struct report_error_on_pending_pcpu_params pcpu_params = {
			.disk = disk,
		};

		/* move ALL pending cmds of disk */
		spin_lock_irqsave(&disk->spinlock, flags);
		for (i = 0; i < DISK_PEND_PRIO_MAX; i++) {
			list_splice_tail_init(&info->pending_disk_cmds[i], &list);
		}
		info->tot_pending = 0;
		info->tot_io_pending = 0;
		spin_unlock_irqrestore(&disk->spinlock, flags);

		/* move ALL pending cmds of disk's per pcpu-nrchs */
		if (!disk->pcpu_nrchs_ll) {
			for (i = 0; i < ARRAY_SIZE(info->pcpu_nrchs); i++) {
				spin_lock_irqsave(&info->pcpu_nrchs[i].spinlock, flags);
				list_splice_tail_init(&info->pcpu_nrchs[i].pending_disk_cmds, &list);
				spin_unlock_irqrestore(&info->pcpu_nrchs[i].spinlock, flags);
			}
		} else {
			/* per-cpu are lock-less, need to schedule on each of their cpus */
			if (!(pcpu_params.pcpu_pending_cmds = kcalloc(NVMEIB_DFLT_MAX_CPUS,
				sizeof(*pcpu_params.pcpu_pending_cmds), GFP_KERNEL))) {
				_NE(err_report_error_on_pending_oom, "OOM");
			} else {
				for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++)
					INIT_LIST_HEAD(&pcpu_params.pcpu_pending_cmds[i]);

				/* From the kernel doc:
				* 	"You must not call this function with disabled interrupts or from a hardware interrupt handler or from a bottom half handler."
				*/
				BUG_ON(irqs_disabled() || in_interrupt());

				on_each_cpu_mask(disk->pcpu_nrchs_ll_cpumask, report_error_on_pending_pcpu_fn, &pcpu_params, true);

				for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++)
					list_splice_tail(&pcpu_params.pcpu_pending_cmds[i], &list);

				kfree(pcpu_params.pcpu_pending_cmds);
			}
		}

		if (info->coremask_info && info->coremask_info->n_coremask) {
			struct nvmeibc_disk_coremask_chs *coremask_chs;
			
			/* move all pending cmds from coremask(s)
			 * NOTE: All IO has finished at this point and 
			 * the periodic has been stopped so we don't need to lock.
			 */
			list_for_each_entry(coremask_chs, &info->coremask_info->coremask_chs, link) {
				list_splice_tail_init(&coremask_chs->pending_cmds, &list);
				coremask_chs->n_pending = 0;
				coremask_chs->max_pending = 0;
			}
		}

		pending_cmds_abort(disk, &list, false); /* abort pending cmds on disk-disconnect */
	}
	_ND(trace_2_disk_report_error_on_pending, "Aha! disk=@DISK (@DISK_FULL_NAME)", disk, disk->full_name);
	seg_locks = nvmeibc_disk_get_segs_locks(disk,
											(struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 });
	if (seg_locks && seg_locks->lock_ch) {
		_ND(trace_3_disk_report_error_on_pending, "Aha! disk=@DISK (@DISK_FULL_NAME)", disk, disk->full_name);
		//nvmeibc_disk_locks_drain_defered(seg_locks->lock_ch);
		/* EC-3929:
		Abort in-progress and deferred lock-operation after ULP ensueres
		no new operation
		*/
		nvmeibc_disk_locks_abort_all_oprs(seg_locks->lock_ch);
	}
	else
		_NT(trace_4_disk_report_error_on_pending, "Disk has NO segments-lock/lock-ch (@SEG_LOCKS)\n", seg_locks);
	nvmeibc_disk_put_segs_locks(seg_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });

	cancel_queued_get_ec_db_reqs(disk);

	if (NVMEIBC_LOCAL_DEFER_COMPLETE_IOCMD && disk->local_defer_block_cb_on_io_cmd) {
		DECLARE_COMPLETION_ONSTACK(comp);
		int i, rv_w = 0, outstanding;
		/* Set to dying and wait for all outstanding local IO */
		nvmeib_set_state_guard(&disk->local_defer_io_work_state, LOCAL_DEFER_WORK_DYING);

		spin_lock_irqsave(&disk->local_defer_io_lock, flags);
		disk->local_defer_io_comp = &comp;
		spin_unlock_irqrestore(&disk->local_defer_io_lock, flags);
		
		if ((outstanding = atomic_dec_return(&disk->local_defer_io_outstanding)) > 0) {
			_NT(trace_5_disk_report_error_on_pending, 
			    "Waiting for @COUNT outstanding local IOs to complete", outstanding);
			for (i = 0; rv_w <= 0;i++) {
				if ((rv_w = wait_for_completion_interruptible_timeout(&comp, NVMEIB_WAIT_BLOCK_DEV_PAUSE)) <= 0) {
					/* Timeout waiting for outstanding local IO */
					_NW(warn_disk_report_error_on_pending, 
						"TIMEOUT waiting for outstanding local IO - "
						"num outstanding: @COUNT num attempts: @NUM_RETRY_ATTEMPTS of @NUM_SEC seconds",
						atomic_read(&disk->local_defer_io_outstanding), i, NVMEIB_WAIT_BLOCK_DEV_PAUSE / HZ);
				}
			}
		}
	}

	wait_event(disk->deferred_io_wait, atomic_read(&disk->deferred_io_cnt) == 0);
	
	if (disk->local_gen_wq) {
		_NT(trace_disk_report_error_on_pending_drain_local_gen_wq,
			"Draining Local GEN WQ (pid: @NVMEIB_QPID) with @N_ELEM Elements...",
			wq_pid(disk->local_gen_wq), atomic_read(&disk->local_gen_wq_cnt) - 1);
		wq_drain(disk->local_gen_wq);
		BUG_ON(atomic_dec_return(&disk->local_gen_wq_cnt) != 0);
		wq_destroy(disk->local_gen_wq);
		disk->local_gen_wq = NULL;
	}

	if (NVMEIBC_LOCAL_DEFER_COMPLETE_IOCMD && disk->local_defer_block_cb_on_io_cmd) {
		/* Complete any remaining local-IO completions */
		local_defer_io_work_fn(&disk->local_defer_io_work);
	}

	NFOUT;
}

//@disk_used - was disk used by any volume **WHILE** stopping IO.
//We could have checked if disk has volumes after returning from this func but
//my concern is that new volume can sneak in after we had already decided that
//this disk is NOT inuse. This can potentially happen as the removal of volume
//from disk and the removal of disk from global disks list is not protected by
//a lock but only by serializing attach and detach on the main wq.
static bool stop_disk_io(struct nvmeibc_disk *disk, bool *disk_used)
{
	struct nvmeibc_disk_id *disk_id;
	int is_ready;
	unsigned long flags;
	int rv;
	bool io_stopped = false;
	unsigned long remote_disk_pause_timeout = nvmeibc_disk_pause_timeout * HZ;
	unsigned long wait_pause_timeout = (disk->access_local) ?
		NVMEIB_WAIT_BLOCK_DEV_PAUSE_LOCAL :
		((remote_disk_pause_timeout > 0 &&
		  remote_disk_pause_timeout < NVMEIB_WAIT_BLOCK_DEV_PAUSE) ?
		 remote_disk_pause_timeout : NVMEIB_WAIT_BLOCK_DEV_PAUSE);
	unsigned long wait_pause_start_jif, cur_jif;
	int i;

	__NFIND;
	spin_lock_irqsave(&disk->volume_spinlock, flags);
	disk->should_pause = true;
	atomic_inc(&disk->paused);
	atomic_set(&disk->n_volumes_paused, 1);
	*disk_used = !list_empty(&disk->volumes);
	list_for_each_entry(disk_id, &disk->volumes, slink) {
		if (disk_id->volume) {
			nvmeibc_volume_get(disk_id->volume, NULL);
			is_ready = nvmeibc_volume_is_ready_for_pause_cont(disk_id->volume);
			if (is_ready>0)
				BUG_ON(nvmeibc_block_pause(disk_id->volume->block_dev, disk));
			else
				_NI(t_01_spcvd, "Skipping pause on @DISK_NAME for volume @DEV_NAME", disk->name, disk_id->volume->hdr.devname);
			nvmeibc_volume_put(disk_id->volume, NULL);
		}
	}
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
	nvmeibc_pd_pause(disk, complete_disk_pause, disk);
	report_error_on_pending(disk);
	_NT(trace_disk_stop_disk_io, "Wait @DISK_SEGMENT_TIMEOUT seconds for pause-ack from ULP on disk @DISK_NAME (@DISK)",
	   wait_pause_timeout / HZ, disk->name, disk);
	/* Regardless if disk is used by bdev or not, wait for IO to stop, coz JAM always exists */
	for (i = 0, cur_jif = wait_pause_start_jif = jiffies;
		 !io_stopped && cur_jif < wait_pause_start_jif + wait_pause_timeout; i++, cur_jif = jiffies) {
		if ((rv = wait_for_completion_interruptible_timeout(&disk->disk_paused,
			NVMEIB_WAIT_BLOCK_DEV_PAUSE)) <= 0) {
			/* Timeout waiting for IO to stop */
			_NW(warn_disk_stop_disk_io,
				"ULP did not pause after @CNT waits of @WAIT_TIME seconds (total @DISK_SEGMENT_TIMEOUT seconds) (rv=@RV)- disk @DISK_NAME "
				" (@DISK) is dead (no rediscovery)",
				i, (long int)NVMEIB_WAIT_BLOCK_DEV_PAUSE, (wait_pause_timeout / HZ), rv, disk->name, disk);
			nvmeibc_pd_dump_transfers(disk);
			io_stopped = false;
		} else {
			io_stopped = true;
		}
	}

#ifdef DEBUG_SUM
	{
		int it = atomic_read(&disk->in_transfers);
		if (it) {
			_NT(trace_disk_stop_disk_io_t2,
				"disk=@PTR, it=@INT, Waiting timed out", disk, it);
			BUG();
		}
	}
#endif

	__NFOUTD;
	return io_stopped;
}

static int cont_disk_io(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_id *disk_id;
	unsigned long flags;
	int is_ready, rv = 0;

	__NFIND;
	nvmeibc_pd_cont(disk);
	spin_lock_irqsave(&disk->volume_spinlock, flags);
	atomic_set(&disk->paused, 0);
	disk->rediscover_timeout = 0;
	list_for_each_entry(disk_id, &disk->volumes, slink)
		if (disk_id->volume) {
			nvmeibc_volume_get(disk_id->volume, NULL);
			is_ready = nvmeibc_volume_is_ready_for_pause_cont(disk_id->volume);
			if (is_ready>0) {
				if (nvmeibc_block_cont(disk_id->volume->block_dev, disk)) {
					_NT(trace_disk_cont_disk_io, "Failed to start IO on disk @DISK_NAME for volume @DEV_NAME_FULL",
					   disk->name, disk_id->volume->full_name);
					--rv;
				}
			} else
				_NI(t_02_spcvd, "Skipping cont on @DISK_NAME for volume @DEV_NAME", disk->name, disk_id->volume->hdr.devname);
			nvmeibc_volume_put(disk_id->volume, NULL);
		}
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
	__NFOUTD;
	_NI(i_cont_disk_io_0, "@EVENT_TAG: cont disk IO on disk @DISK_NAME (@DISK) res=@INT", EV_DISK_IO_CONT(), disk->name, disk, rv);
	return rv;
}

void nvmeibc_disk_free_unused_admin_ch(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;

	__NFIND;
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!arnic->alive && arnic->channel) {
			_NT(trace_disk_nvmeibc_disk_free_unused_admin_ch, "Free unsued admin channel @BASE_NAME", arnic->channel->base.name);
			nvmeibc_ib_admin_channel_free(ac_to_iac(arnic->channel));

			//ach it self is still allocated...
			//will remain so if disk-release wont be triggered?
		}
	}

	__NFOUTD;
}

static void disconnect_disk(struct nvmeibc_disk *disk)
{
	struct nvmeibc_admin_rnic *arnic;
	bool waited_for_local_lock __attribute__((unused)) = false;
	u64 cid = NVMEIBS_CLIENT_UID_BASE;
	struct nvmeibc_disk_segments_locks *disk_seg_locks;
	__NFIND;

	if (disk->is_cl_reg) /* i.e. disk is local && has valid local-admin-ch */
		cid = disk->local_admin_ch->cid;

	list_for_each_entry(arnic, &disk->arnics, link) {
		if (arnic->alive ||
			nvmeibc_ib_admin_is_connected(ac_to_iac(arnic->channel))) {
			nvmeibc_ib_admin_channel_disconnect(ac_to_iac(arnic->channel));
			if (arnic->alive && arnic->channel->is_main) {
				nvmeibc_target_arnic_close_conn(arnic);
				disk->main_ach_wq_pid = 0;
			}
		}
		if (arnic->channel) {
			if (arnic->local)
				waited_for_local_lock = true;
			nvmeibc_ib_admin_channel_free(ac_to_iac(arnic->channel));
			arnic->alive = false;
			if (arnic->local) {
				disk->local_admin_ch = NULL;

			}
		}
	}

	disk_seg_locks = nvmeibc_disk_get_segs_locks(disk,
												 (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 });
	if (disk_seg_locks && disk_seg_locks->lock_ch) {
		_NE(error_disk_disconnect_disk, "OOPS, disk @DISK_NAME(@DISK) still refs lock-ch", disk->name, disk);
		BUG();
	}
	nvmeibc_disk_put_segs_locks(disk_seg_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });

	nvmeibc_locks_channel_disconnect_all_handles(disk);

	if (disk->jam_disk) {
		_NT(trace_disk_disconnect_disk, "Disk @DISK_NAME(@DISK), stop, cache and delete jam-disk", disk->name, disk);
		nvmeibc_jam_disk_del(disk);
	}
	disk->jour.rng_id = NVMEIB_EC_INVALID_JOURNAL_RANGE;

	if (disk->access_local) {
		if (disk->local.dma_pools) {
			free_percpu_pools(disk, disk->local.dma_pools);
			disk->local.dma_pools = NULL;	
		}
		

		if (disk->local.dummy_md_read_ptr) {
			dma_free_coherent(disk->local.dummy_md_dma_dev, PAGE_SIZE, disk->local.dummy_md_read_ptr, disk->local.dummy_md_read_addr);
			disk->local.dummy_md_read_ptr = NULL;
			disk->local.dummy_md_read_addr = 0;
		}
		if (disk->local.dummy_md_write_ptr) {
			dma_free_coherent(disk->local.dummy_md_dma_dev, PAGE_SIZE, disk->local.dummy_md_write_ptr, disk->local.dummy_md_write_addr);
			disk->local.dummy_md_write_ptr = NULL;
			disk->local.dummy_md_write_addr = 0;
		}
		disk->local.dummy_md_dma_dev = NULL;
	}

	if (disk->is_cl_reg)
		local_disk_cl_unregister(disk, cid);

	_NT(trace_1_disk_disconnect_disk, "calling free_dirty_bits_mem()");
	free_dirty_bits_mem(disk);

	pcpu_nrch_check_empty(disk);

	__NFOUTD;
}

static void free_disk_rsc(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *info;
	struct nvmeibc_admin_rnic *arnic;
	/* Jared local nics are now populated on create and updated by config work tasks
	 * -------------------------------------------------------------------------------
	 * struct list_head *local_nics = &disk->local_nics;
	 * struct nvmeibc_local_nic *ln;
	 * struct list_head *ports;
	 *struct nvmeibc_local_nic_port *lnp;*/
	unsigned long flags;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;

	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);
	info = disk->info;
	disk->info = NULL;
	nvmeibc_locks_channel_disconnect_all_handles_(disk);
	spin_unlock_irqrestore(&disk->spinlock, flags);

	if (info) {
		kfree(info->avail_norddas_per_numa_node);
		info->avail_norddas_per_numa_node = NULL;
		free_disc_rscs(ac_to_iac(info->ch), info);
		kfree(info->hcaa);
		//EC-5789
		if (!down_write_trylock(&info->ch->segments_locks_remote.guard)) {
			_NT(t0_free_disk_rsc,
				"Could not acquire seg-locks guard, ulp probably holds it...");
			//see __subscribe_seg() -> nvmeibc_disk_locks_seg_locks_mem_info()
			down_write(&info->ch->segments_locks_remote.guard);
		}
		if (info->ch->segments_locks_remote.num_of_segments > 0) {
			kfree(info->ch->segments_locks_remote.locks);
			info->ch->segments_locks_remote.locks = NULL;
			info->ch->segments_locks_remote.num_of_segments = 0;
		}
		up_write(&info->ch->segments_locks_remote.guard);
		if (info->pcpu_wq)
			destroy_workqueue(info->pcpu_wq);
		free_coremask_info(info->coremask_info);
		kfree(info);
	}

	BUG_ON(!(disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
														   (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1, .local_only = 1 })));
	if (disk_segs_locks->num_of_segments > 0) {
		kfree(disk_segs_locks->locks);
		disk_segs_locks->num_of_segments = 0;
		disk_segs_locks->locks = NULL;
	}
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1 });

	spin_lock_irqsave(&disk->spinlock, flags);
	list_for_each_entry(arnic, &disk->arnics, link) {
		kfree(ac_to_iac(arnic->channel));
		BUG_ON(arnic->alive);
		arnic->channel = NULL;
	}
	spin_unlock_irqrestore(&disk->spinlock, flags);

	/* Jared local nics are now populated on create and updated by config work tasks
	 *-------------------------------------------------------------------------------
	* while ((ln = list_first_entry_or_null(
	* 	local_nics, struct nvmeibc_local_nic, link))) {
	* 	list_del(&ln->link);
	* 	ports = &ln->ports;
	* 	while ((lnp = list_first_entry_or_null(
	*		ports, struct nvmeibc_local_nic_port, link))) {
	*		list_del(&lnp->link);
	*		kfree(lnp);
	*	}
	*	kfree(ln);
	*}
	*/

	__NFOUTD;
}

enum rediscovery {
	rr_init,
	rr_ok,
	rr_try_again,
	rr_out,
	rr_cfg_update,
};

static struct nvmeibc_admin_rnic * __arnic_dup(struct nvmeibc_admin_rnic *arnic)
{
	struct nvmeibc_admin_rnic *a;

	if ((a = kzalloc(sizeof(*a), GFP_ATOMIC))) {
		a->ib_gid = arnic->ib_gid;
		a->pkey =  arnic->pkey;
		a->service_id = arnic->service_id;
		a->service_port = arnic->service_port;
		a->link_layer = arnic->link_layer;
		a->transport_type = arnic->transport_type;
		a->score = arnic->score;
		a->priority.raw = arnic->priority.raw;
		a->num_conns = arnic->num_conns;
		nvmeib_trend_init(&a->discover_trend);
		strlcpy(a->node_id, arnic->node_id, sizeof(a->node_id));
		BUG_ON(a->score == NULL);
	}

	return a;
}

static struct nvmeibc_admin_rnic * __arnic_tcp(struct nvmeibc_admin_rnic *arnic)
{
	struct nvmeibc_admin_rnic *a;

	if ((a = kzalloc(sizeof(*a), GFP_ATOMIC))) {
		a->ib_gid = arnic->ib_gid;
		a->pkey = 0;
		a->service_id = 0;
		a->service_port = nvmeib_get_tcp_base_port_id();
		a->link_layer = IB_LINK_LAYER_ETHERNET;
		a->transport_type = RDMA_TRANSPORT_IWARP;
		a->score = arnic->score;
		//a->priority.raw = arnic->priority.raw;
		a->priority.transport = NVMEIB_TCP_PORT_PRIORITY;
		a->num_conns = arnic->num_conns;
		nvmeib_trend_init(&a->discover_trend);
		strlcpy(a->node_id, arnic->node_id, sizeof(a->node_id));
		BUG_ON(a->score == NULL);
	}

	return a;
}

static int arnics_dup(struct nvmeibc_disk *disk,
	struct list_head *target, struct list_head *source)
{
	struct disk_globals *d = __get_dg(disk);
	struct nvmeibc_admin_rnic *arnic, *a;
	char gid[GUID_SIZE];
	int i, n = 0, rv = 0;

	NFIN;
	i = 0;
	list_for_each_entry(arnic, source, link) {
		_NT(t0_arnics_dup,
		        "i=@INT32_02, n=@INT32_02, gid=@GUID_RAW (m=@BOOL)", i, n,
		        &arnic->ib_gid, arnic->is_multi_transport);
		{
			if (!(a = __arnic_dup(arnic))) {
				_NW(w0_arnics_dup, "Fail to alloc arnic");
				rv = -ENOMEM;
				goto out;
			}
			a->order = n;
			prio_list_add_tail(&a->link, target, arnic_prio_cmp_fn, NULL);
			++n;
		}

		if (arnic->is_multi_transport) {
			if (!(a = __arnic_tcp(arnic))) {
				_NW(w1_arnics_dup, "Fail to alloc tcp arnic");
				rv = -ENOMEM;
				goto out;
			}
			a->order = n;
			prio_list_add_tail(&a->link, target, arnic_prio_cmp_fn, NULL);
			++n;
		}

		i++;
	}

	__print_arnics(target);
	_NT(trace_1_disk_arnics_dup, "rotate and keep priority...");
	if (n) {
		/* Rotate list, but keep priority ordering intact */
		unsigned ranic_shuffle_n = atomic_add_return(1, &d->ranic_shuffle_n);
		i = 0;
		prio_list_rotate_n(target, ranic_shuffle_n, arnic_prio_cmp_fn, NULL);
		if ((arnic = list_first_entry_or_null(
			target, struct nvmeibc_admin_rnic, link))) {
			format_gid_raw(arnic->ib_gid.raw, gid);
			_NT(trace_2_disk_arnics_dup,
					"n=@COUNT, first arnic=@ARNIC_STR, priority=@PRIORITY, layer=@LINK_LAYER, transport=@TRANSPORT_TYPE",
					n, gid, arnic->priority.raw, arnic->link_layer, arnic->transport_type);
		}
	}
	__print_arnics(target);

out:
	NFOUT;
	return rv;
}

int nvmeibc_disk_set_next_config(struct nvmeibc_disk_id *disk_id,
	const char *node_id)
{
	struct nvmeibc_admin_rnic *arnic, *tmp_arnic;
	struct nvmeibc_disk *disk = disk_id->disk;
	int rv = 0;

	__NFIND;
	spin_lock(&disk->disk_conf_spinlock);
	_NT(trace_disk_nvmeibc_disk_set_next_config, "New config node id @NODE_ID_STR will replace the existing one disk @DISK_NAME",
		node_id, disk->name);
	strlcpy(disk->next_config_node_id, node_id,
		sizeof(disk->config_node_id));
	list_for_each_entry_safe(arnic, tmp_arnic, &disk->next_arnics, link) {
		list_del(&arnic->link);
		kfree(arnic);
	}
	_NT(trace_1_disk_nvmeibc_disk_set_next_config, "Duplicating new arnics for disk @DISK_NAME", disk->name);
	if ((rv = arnics_dup(disk, &disk->next_arnics, nvmeibc_disk_id_to_nics(disk_id))) < 0) {
		_NT(error_disk_nvmeibc_disk_set_next_config, "Cannot update next config error @RV", rv);
		goto out;
	}
	disk->new_config_ready = true;

	// wake rediscovery() so it doesn't wait unneedlessly
	nvmeibc_disk_call_discover(disk);

out:
	spin_unlock(&disk->disk_conf_spinlock);
	__NFOUTD;
	return rv;
}

static void rotate_arnics(struct nvmeibc_disk *disk)
{
	int num_arnics = 0;
	int order0;
	struct nvmeibc_admin_rnic *arnic, *arnic0;

	__NFIND;

	if (list_empty(&disk->arnics))
		goto out;

#if 1
	(void)num_arnics;
	(void)order0;
	(void)arnic;
	(void)arnic0;
	prio_list_rotate_n(&disk->arnics, 1, arnic_prio_cmp_fn, NULL);
#else
	arnic0 = list_first_entry(&disk->arnics, struct nvmeibc_admin_rnic, link);
	order0 = arnic0->order;

	list_for_each_entry(arnic, &disk->arnics, link)
		++num_arnics;

	if (order0 == (num_arnics - 1))
		goto out;

	arnic0->order = (num_arnics - 1);

	list_for_each_entry(arnic, &disk->arnics, link) {
		if (arnic->order > order0 && arnic != arnic0 ) {
			--arnic->order;
		}
	}
#endif

out:
	__NFOUTD;
}

/* must be followed with disk_toma_subscribe_unlock, using the same flags varibale */
static int disk_toma_subscribe_lock_return_permission(struct nvmeibc_disk *disk, unsigned long *flags)
{
	spin_lock_irqsave(&disk->subscribe_lock, *flags);
	_NT(disk_toma_subscribe_lock_return_permission, "SUBSCRIBE: Got lock, subscribe_flag=@INT disk @DISK_NAME", disk->subscribe_flag, disk->name);
	return disk->subscribe_flag;
}
static void disk_toma_subscribe_unlock(struct nvmeibc_disk *disk, unsigned long *flags)
{
	spin_unlock_irqrestore(&disk->subscribe_lock, *flags);
	_NT(disk_toma_subscribe_unlock, "SUBSCRIBE: Released lock, disk @DISK_NAME", disk->name);
}

static void disk_toma_subscribe_freeze(struct nvmeibc_disk *disk)
{
	unsigned long flags;
	spin_lock_irqsave(&disk->subscribe_lock, flags);
	_NT(trace_disk_toma_subscribe_freeze, "SUBSCRIBE: Got lock - setting to 1, disk @DISK_NAME", disk->name);
	disk->subscribe_flag = 1;
	spin_unlock_irqrestore(&disk->subscribe_lock, flags);
}

static void disk_toma_subscribe_unfreeze(struct nvmeibc_disk *disk)
{
	unsigned long flags;
	spin_lock_irqsave(&disk->subscribe_lock, flags);
	_NT(trace_disk_toma_subscribe_unfreeze, "SUBSCRIBE: Got lock - setting to 0, disk @DISK_NAME", disk->name);
	disk->subscribe_flag = 0;
	spin_unlock_irqrestore(&disk->subscribe_lock, flags);
}

static int toma_rereg_disk_handles(struct nvmeibc_disk *disk,
								   struct nvmeibc_ib_admin_channel *ch,
								   bool is_async);

static int disk_toma_subscribe_dispatch_pending_subscriptions(struct nvmeibc_disk *disk, bool is_async)
{
	unsigned long flags = 0;
	int rv = 0, subscribe_permission;
	struct nvmeibc_ib_admin_channel *ch = NULL;

	subscribe_permission = disk_toma_subscribe_lock_return_permission(disk, &flags);
	if (!subscribe_permission) {
		/* Not allowing to send subscriptions while release is ongoing / subscribe freeze */
		_NT(trace_disk_toma_subscribe_dispatch_pending_subscriptions_got_lock_no_perm, "SUBSCRIBE: Got lock, disk @DISK_NAME. Dont have permissions to send subscribe.", disk->name);
		rv = -1;
		goto unlock_and_out;
	}
	_NT(trace_disk_toma_subscribe_dispatch_pending_subscriptions_got_lock_start, "SUBSCRIBE: Got lock, disk @DISK_NAME. Starting dispaching all pending subscribes", disk->name);
	if ((ch = get_alive_admin_ch(disk)) && ch->toma.valid) {
		if ((rv = toma_rereg_disk_handles(disk, ch, is_async)) < 0 ){
				_NT(trace_3_disk_toma_subscribe_dispatch_pending_subscriptions, "SUBSCRIBE: toma disk rereg failed for disk @DISK_NAME ret stat @RV",
					disk->name, rv);
				DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_TOMA_REREG_FAILED);
				rv = -1;
		}
	} else {
		_NT(trace_disk_toma_subscribe_dispatch_pending_subscriptions_cant_find_ach, "SUBSCRIBE: Cant find admin channel @DISK_NAME. Doing nothing.", disk->name);
		rv = -1;
	}

	disk->subscribe_flag = 0;
unlock_and_out:
	disk_toma_subscribe_unlock(disk, &flags);
	return rv;
}

static void check_target_nics_query(struct nvmeibc_disk *disk)
{
	ulong jif = jiffies;
	int last_discover_status = NVMEIB_TREND_HEAD(disk->discover_trend).data;
	int rv;
	uint tgt_nics_query_min_n_fail = nvmeibc_disk_tgt_nics_query_min_n_fail;
	uint tgt_nics_query_min_fail_secs = nvmeibc_disk_tgt_nics_query_min_fail_secs;
	uint tgt_nics_query_min_secs = nvmeibc_disk_tgt_nics_query_min_secs;

	__NFIND;

	switch (last_discover_status) {
	case NVMEIBC_DISK_DISCOVER_NO_ARNICS:
	case NVMEIBC_DISK_DISCOVER_NO_ARNICS_ACCESS:
	case NVMEIBC_DISK_DISCOVER_ADMIN_CH_CREATE_FAILED:
	case NVMEIBC_DISK_DISCOVER_ARNIC_NO_PORTS:
	case NVMEIBC_DISK_DISCOVER_ARNIC_NO_PATHS:
	case NVMEIBC_DISK_DISCOVER_IO_RESOURCES_READ_FAIL:
		/* Failed discovery due to arnics issue, relevant for target-nics query */
		_NT(trace_check_target_nics_query_valid_status,
		    "@DISK_NAME - Last discovery failure (@STATUS) was caused by arnics",
		    disk->name, last_discover_status);
		break;
	default:
		_NT(trace_check_target_nics_query_inv_status,
			"@DISK_NAME - Last discovery failure (@STATUS) was not caused by arnics, "
			"not performing target-nics query",
			disk->name, last_discover_status);
		disk->n_arnics_discover_fail = 0;
		disk->first_arnics_discover_fail_jif = 0;
		goto out;
	}

	if (!disk->n_arnics_discover_fail) {
		disk->first_arnics_discover_fail_jif = jif;
		_NT(trace_check_target_nics_query_first_arnic_fail,
		    "@DISK_NAME - First discovery failure due to arnics at @JIFFIES",
			disk->name, disk->first_arnics_discover_fail_jif);
	}
	disk->n_arnics_discover_fail++;

	/* If we haven't met at least one of the thresholds, exit */
	if (!(disk->n_arnics_discover_fail >= tgt_nics_query_min_n_fail ||
		((jif - disk->first_arnics_discover_fail_jif) >= tgt_nics_query_min_fail_secs * HZ)))
	{
		_NT(trace_check_target_nics_query_fail_inv,
			"@DISK_NAME - Neither num-failures/failure-interval @COUNT/@SECONDS meets thresholds "
			" @COUNT/@SECONDS, not performing target-nics query",
			disk->name, disk->n_arnics_discover_fail, (jif - disk->first_arnics_discover_fail_jif) / HZ,
			tgt_nics_query_min_n_fail, tgt_nics_query_min_fail_secs);
		goto out;
	}

	if ((jif - disk->last_target_nics_query_jif) < tgt_nics_query_min_secs * HZ) {
		_NT(trace_check_target_nics_query_q_int_inv,
			"@DISK_NAME - Interval from last target-nics query: @SECONDS < minimum threshold: @SECONDS, "
			"not performing query",
			disk->name, (jif - disk->last_target_nics_query_jif) / HZ,
			tgt_nics_query_min_secs);
		goto out;
	}

	_NT(trace_check_target_nics_query_ok,
		"@DISK_NAME - Performing target-nics query", disk->name);

	disk->last_target_nics_query_jif = jif;

	rv = nvmeibc_cc_api_query_target_nics_by_node_id(nvmeibc_cinst_get_core_m(disk), disk->config_node_id);

	if (rv < 0) {
		_NT(trace_check_target_nics_query_fail,
		    "@DISK_NAME - target-nics query failed (@RV)",
		    disk->name, rv);
		goto out;
	}
	disk->target_nics_query_restart_num = disk->restart_num;

out:
	__NFOUTD;
}

static enum rediscovery rediscovery(struct nvmeibc_disk *disk)
{
	int ret, dispatch = 0;
	enum rediscovery rv;
	int rediscovery_timeout;

	__NFIND;
	/* try to rediscover - we dive in iff we are not detaching */
	if (ALLOW_REDISCOVERY && disk->allow_rediscovery && !disk->first_creation &&
		!disk->detached && !atomic_read(&disk->update_count)) {
		if (!disk->rediscover_timeout)
			disk->rediscover_timeout = msecs_to_jiffies(nvmeibc_disk_min_rediscover_timeout_ms);
		else {
			disk->rediscover_timeout <<= 1;

			if (disk->n_arnics_discover_fail) {
				uint tgt_nics_query_min_fail_secs = nvmeibc_disk_tgt_nics_query_min_fail_secs;
				uint tgt_nics_query_min_secs = nvmeibc_disk_tgt_nics_query_min_secs;
				ulong cur_jif = jiffies;
				ulong next_query_time_jif = max(disk->first_arnics_discover_fail_jif + (tgt_nics_query_min_fail_secs * HZ), disk->last_target_nics_query_jif + (tgt_nics_query_min_secs * HZ));
				ulong rediscover_timeout_next_query = time_after(next_query_time_jif, cur_jif) ? next_query_time_jif - cur_jif : 1;

				if (disk->rediscover_timeout > rediscover_timeout_next_query) {
					disk->rediscover_timeout = max((ulong)msecs_to_jiffies(nvmeibc_disk_min_rediscover_timeout_ms), rediscover_timeout_next_query);
						_NT(trace_disk_rediscovery_nics_query_rediscover_time,
							"Disk @DISK_NAME - Sending target-nics query in @SECONDS s (@JIFFIES jiffies). "
							"Adjusting rediscover timeout to @SECONDS s (@JIFFIES jiffies).",
							disk->name, rediscover_timeout_next_query / HZ, rediscover_timeout_next_query, disk->rediscover_timeout / HZ, disk->rediscover_timeout);
				}
			}

			if (disk->rediscover_timeout > NVMEIBC_REDISCOVER_WAIT)
				disk->rediscover_timeout = NVMEIBC_REDISCOVER_WAIT;
		}
		if (IS_PAUSE_AT_FIRST_DISCOVER_HAPPENING(disk)){
			/*After the first disk pause (that is done on purpose), we want the rediscovery to happen quickly*/
			disk->rediscover_timeout = 1;
		}
		rediscovery_timeout = disk->rediscover_timeout *
			(750 + get_random_u32() % 500) / 1000;

		/* wait for timeout or detach */
		_NT(trace_disk_rediscovery, "Waiting @TIMEOUT seconds before trying to rediscover",
			rediscovery_timeout / HZ);
		ret = wait_event_interruptible_timeout(
			disk->wait_queue, (disk->detached || disk->rediscovery_now || atomic_read(&disk->update_count)),
			rediscovery_timeout);

		if (!ret || (ret > 0 && disk->rediscovery_now)) {
			_NT(trace_1_disk_rediscovery, "Starting a rediscovery process @DISK_NAME...", disk->name);
			disk->rediscovery_now = 0;
			/* timeout so no detach was called so we try again */
			atomic_set(&disk->dying, 0);
			atomic_set(&disk->shut_down_triggered, 0);

			rotate_arnics(disk);
			take_next_disk_config(disk);
			ret = discover(disk, true);

			if (ret < 0/* && !disk->restart_called*/) {
				/* discover failed so we either retry in the case the discovery
				   process did not add a new disk release or we leave and the
				   new disk release we handle the new discovery
				*/
				_NT(trace_2_disk_rediscovery, "Rediscovery failed, trying again...");
				atomic_set(&disk->dying, 0);
				atomic_set(&disk->shut_down_triggered, 0);
				rv = rr_try_again;

				/* [NVMESH-3287]: Check to see if we want to sent a target nics query to mgmt */
				check_target_nics_query(disk);

				goto out;
			}
			else {
				if (!ret) {
					_ND(trace_3_disk_rediscovery, "Discovered disk @DISK_NAME", disk->name);
					dispatch = disk_toma_subscribe_dispatch_pending_subscriptions(disk, true);
					if (dispatch < 0){
						/* Need to force rediscovery. As the inner release that is triggered
						   inside toma_send_subscribe won't help, as we are inside a release */
						atomic_set(&disk->dying, 0);
						atomic_set(&disk->shut_down_triggered, 0);
						rv = rr_try_again;
						goto out;
					}
					if (cont_disk_io(disk) < 0)
						_NT(trace_4_disk_rediscovery, "Failed to restart block device IO");
					nvmeibc_disk_free_unused_admin_ch(disk);
					rv = rr_ok;
					goto out;
				}
				else {
					_NT(trace_5_disk_rediscovery, "Bailing out on disk @DISK_NAME (@RET_INT, @ATOMIC_READ)",
						disk->name, ret, atomic_read(&disk->restart_called));
					rv = rr_out;
					goto out;
				}
			}
		}
	}

	if (disk->detached) {
		_NT(trace_6_disk_rediscovery, "Bailing out on disk @DISK_NAME, detach was called", disk->name);
		rv = rr_out;
	}
	else if (atomic_read(&disk->update_count)) {
		_NT(trace_7_disk_rediscovery, "Disk @DISK_NAME, performing config update", disk->name);
		atomic_set(&disk->dying, 0);
		atomic_set(&disk->shut_down_triggered, 0);
		rv = rr_cfg_update;
	}
	else if (disk->allow_rediscovery) {
		_NT(trace_8_disk_rediscovery, "Disk @DISK_NAME, rediscovery is not allowed", disk->name);
		rv = rr_out;
	}
	else {
		_NE(error_disk_rediscovery, "Disk @DISK_NAME, ending rediscover wait for unknown reason", disk->name);
		rv = rr_out;
	}

out:
	__NFOUTD;
	return rv;
}

static void disk_version_update(struct nvmeibc_disk *disk)
{
	unsigned long flags;
	u64 disk_ver;

	__NFIND;
	/* Spinlock prevents against multiple writers */
	spin_lock_irqsave(&disk->spinlock, flags);
	/* Update disk->version under seqcount lock which
	 * takes care of SMP acquire/release semantics */
	write_seqcount_begin(&disk->version_seq);
	disk_ver = READ_ONCE(disk->version);
	if (++disk_ver == 0)
		++disk_ver;
	WRITE_ONCE(disk->version, disk_ver);
	write_seqcount_end(&disk->version_seq);
	spin_unlock_irqrestore(&disk->spinlock, flags);

	__NFOUTD;
}

u64 nvmeibc_disk_version_get(struct nvmeibc_disk *disk)
{
	unsigned seq;
	u64 disk_ver;

	/* Read disk->version under seqcount lock which
	 * takes care of necessary SMP acquire/release semantics */
	do {
		seq = read_seqcount_begin(&disk->version_seq);
		disk_ver = READ_ONCE(disk->version);
	} while(read_seqcount_retry(&disk->version_seq, seq));

	return disk_ver;
}

static void nvmeibc_disk_release(struct nvmeibc_disk *disk, enum nvmeibc_disk_release_reason reason)
{
	int dying;
	enum rediscovery rv;
	int restart_called;
	bool disk_used;
	u64 ts, dt;

	__NFIND;
	disk->restart_num = 0;
	if (!disk->discover_rv)
		disk->disconnect_jif = jiffies;

	nvmeib_trend_insert(&disk->release_trend, reason);

restart:
	_NT(trace_disk_nvmeibc_disk_release, "DTREND Disk @DISK_NAME (@DISK) release - start (#@RESTART_NUM), reason: @DISK_RELEASE_OP",
	 disk->name, disk, disk->restart_num, reason);
	disk_toma_subscribe_freeze(disk);
	rv = rr_init;
	disk_version_update(disk);

	disk->restart_num++;
	if ((dying = atomic_inc_return(&disk->dying)) > 1) {
		_NT(trace_2_disk_nvmeibc_disk_release, "Release in process for disk @DISK_FULL_NAME (@DYING)", disk->full_name, dying);
		goto out;
	}

	/* remove periodic */
	if (disk->periodic.remove_periodic && disk->periodic.ch) {
		disk->periodic.remove_periodic(disk->periodic.ch, &disk->periodic);
		memset(&disk->periodic, 0, sizeof(disk->periodic));
	}

	_NT(trace_3_disk_nvmeibc_disk_release, "Stopping disk IO...");
	ts = jiffies;
	disk->io_stopped = stop_disk_io(disk, &disk_used);
	dt = jiffies - ts;
	_NT(trace_15_disk_nvmeibc_disk_release,
		"Disk @DISK_NAME (@DISK) stop-disk-io took "
		"@DISK_FUNC_DT(@DISK_FUNC_DT)",
		disk->name, disk, dt, dt/HZ);

	_NT(trace_4_disk_nvmeibc_disk_release, "Disconnecting disk...");
	ts = jiffies;
	disconnect_disk(disk);
	dt = jiffies - ts;
	_NT(trace_16_disk_nvmeibc_disk_release,
		"Disk @DISK_NAME (@DISK) disconnect-disk took "
		"@DISK_FUNC_DT(@DISK_FUNC_DT)",
		disk->name, disk, dt, dt/HZ);

	if (!disk->io_stopped) {
		if (try_wait_for_completion(&disk->disk_paused)) {
			_NI(trace_5_disk_nvmeibc_disk_release, "ULP eventually acked pause");
			disk->io_stopped = true;
		} else {
			_NE(error_disk_nvmeibc_disk_release, "ULP still NOT paused even after disconnecting disk!");
			nvmeibc_pd_dump_transfers(disk);
		}
	}

	/* Decide if should & could rediscover disk */
	if (disk_used && disk->io_stopped) {
		nvmeib_reinit_completion(&disk->disk_paused);
		disk->allow_rediscovery = true;
	}
	else {
		disk->allow_rediscovery = false;
	}

	/* If we are local and io_stopped, clear the local data */
	if (disk->io_stopped) {
		disk->local.p = NULL;
	}

	/* kill all admin nics */
	_NT(trace_6_disk_nvmeibc_disk_release, "Freeing disk resources...");
	/* earse allocated memory */
	free_disk_rsc(disk);

	_NT(trace_7_disk_nvmeibc_disk_release, "Call rediscovery...");
	atomic_set(&disk->restart_called, 1);
	rv = rediscovery(disk);
	switch (rv) {
	case rr_try_again:
		_NT(trace_8_disk_nvmeibc_disk_release, "Rediscovery failed, try again...");
		goto restart;
	case rr_ok:
		if ((restart_called = atomic_dec_return(&disk->restart_called)) > 0) {
			_NT(trace_9_disk_nvmeibc_disk_release, "disk-release was triggered after this rediscovery started "
			   "(dec to @RESTART_CALLED), restart ... ", restart_called);
			goto restart;
		}
		else {
			disk_toma_subscribe_unfreeze(disk);
			_NT(trace_10_disk_nvmeibc_disk_release, "Rediscovery ok, march on...");
			/* a new release-work may have already enqueued, skip set to 0 */
			goto done;
		}
	case rr_out:
		_NT(trace_11_disk_nvmeibc_disk_release, "Rediscovery failed, bail out...");
		goto out;
	case rr_cfg_update:
		_NT(trace_12_disk_nvmeibc_disk_release, "Rediscovery rescheduled for config update");
		goto out;
	default:
		_NT(trace_13_disk_nvmeibc_disk_release, "Rediscovery unknown rv @RV, bail out...", rv);
		goto out;
	}

out:
	atomic_set(&disk->restart_called, 0);
done:
	_NT(trace_14_disk_nvmeibc_disk_release, "Disk @DISK_NAME (@DISK) release - done", disk->name, disk);

	__NFOUTD;
}

static void discover_work(struct workqe_struct *work)
{
	struct disk_workq *rwork =
		container_of(work, struct disk_workq, work);
	struct nvmeibc_disk *disk = rwork->disk;

	__NFIND;
	if (!nvmeibc_disk_pause_at_first_discover)
		disk_toma_subscribe_freeze(disk);
	/* discover disk connectivity */
	if (discover(disk, false) < 0) {
		atomic_set(&disk->paused, 1);
		_NT(trace_disk_discover_work, "Fail to connect to disk @DISK_NAME", disk->name);
		/* __setup_block_device_from_volume()
		   (decide if to) trigger rediscovery */
	}
	else
		atomic_set(&disk->paused, 0);
	nvmeibc_disk_free_unused_admin_ch(disk);

	if (disk->discover_comp) {
		if (!nvmeibc_disk_pause_at_first_discover)
			disk_toma_subscribe_dispatch_pending_subscriptions(disk, true);
		complete(disk->discover_comp);
	}
	kfree(rwork);
	__NFOUTD;
}

int nvmeibc_disk_wait_for_discover(struct nvmeibc_disk_id *current_disk)
{
	struct nvmeibc_disk *disk;

	NFIN;

	BUG_ON(current_disk == NULL);
	BUG_ON(current_disk->disk == NULL);
	disk = current_disk->disk;
	BUG_ON(disk->discover_comp == NULL);
	_NT(trace_disk_nvmeibc_disk_wait_for_discover, "Waiting for disk @DISK_NAME to finish discover", current_disk->disk->name);
	wait_for_completion(disk->discover_comp);
	/* wait for all disk_release on disk queue to finish */
	wq_drain(disk->remove_wq);
	/* disk is not dying anymore */
	atomic_set(&disk->dying, 0);
	//EC-1573: atomic_set(&disk->restart_called, 0);
	atomic_set(&disk->shut_down_triggered, 0);
	disk->first_creation = false;
	kfree(disk->discover_comp);
	disk->discover_comp = NULL;

	NFOUT;
	return disk->discover_rv;
}

static int call_discover(struct nvmeibc_disk *disk)
{
	int rv = 0;
	struct completion *comp;
	struct disk_workq *wq;

	__NFIND;
	if (!(comp = kzalloc(sizeof (*comp), GFP_KERNEL))) {
		_NE(error_disk_call_discover, "memory allocation problem when tried to allocate memory for"
			" discover completion");
		rv = -ENOMEM;
		goto out;
	}
	wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	if (!wq) {
		_NE(error_1_disk_call_discover, "Memory allocation problem. could not allocate work queue");
		kfree(comp);
		rv = -ENOMEM;
		goto out;
	}
	init_completion(comp);
	if (disk->discover_comp)
		kfree(disk->discover_comp);
	_NT(trace_disk_call_discover, "Discover called for disk @DISK_NAME ", disk->name);
	disk->discover_comp = comp;
	//EC-1573: atomic_set(&disk->restart_called, 1);
	atomic_set(&disk->restart_called, 0);
	wq->disk = disk;
	WQ_INIT_WORK(&wq->work, discover_work);
	disk->first_creation = true;
	disk->discover_rv = 0;
	if (nvmeibc_disk_add_work(disk, &wq->work) < 0) {
		kfree(wq);
		kfree(comp);
		//EC-1573: atomic_set(&disk->restart_called, 0);
	}

out:
	__NFOUTD;
	return rv;
}

static int nvmeibc_disk_net_intrs_stats_create(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_percpu_intr_stats __percpu *p;
	int rv;

	NFIN;
	p = nvmeib_public_alloc_percpu_zeroed(struct nvmeibc_disk_percpu_intr_stats);
	if (p == NULL) {
		_NE(error_create_pcpu_intr_stat, "Failed to allocate per_cpu "
				"interupt stat object");
		rv = -1;
	}
	else {
		disk->pcpu_intr_stats = p;
		rv = 0;
	}
	NFOUT;
	return rv;
}

static void nvmeibc_disk_net_intrs_stats_delete(struct nvmeibc_disk *disk)
{
	NFIN;
	if (disk->pcpu_intr_stats) {
		nvmeib_public_free_percpu(disk->pcpu_intr_stats);
		disk->pcpu_intr_stats = NULL;
	}
	NFOUT;
}

static void nvmeibc_disk_net_intrs_stats_reset(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_percpu_intr_stats *pcp;
	int cpu;
	__NFIND;

	for_each_online_cpu(cpu) {
		pcp = per_cpu_ptr(disk->pcpu_intr_stats, cpu);
		memset(pcp, 0, sizeof(*pcp));
	}

	__NFOUTD;
}

static atomic_t disk_create_id = ATOMIC_INIT(0);

int nvmeibc_disk_create(const struct nvmeibc_cinst_params_core *p,
	struct nvmeibc_disk_id *disk_id, struct list_head *arnics, int num_ranges,
	const char *node_id)
{
	struct nvmeibc_disk *disk;
	int rv = 0, i;
	proc_name_t pname;

	NDFINN(disk_id->name);
	if (!(disk = kzalloc(sizeof(*disk), GFP_KERNEL))) {
		_NE(error_disk_nvmeibc_disk_create, "Fail to allocate admin remote disk");
		rv = -ENOMEM;
		goto out;
	}
	nvmeibc_disk_base_init(disk);
	disk->create_id = atomic_inc_return(&disk_create_id);
	disk->discover_id = -1; /* First discovery that is forced to fail will be #0. The second (with reduced timeout) is #1 */
	disk->pause_at_first_discover = nvmeibc_disk_pause_at_first_discover;
	nvmeibc_cinst_get_core_p(disk) = p;
	WRITE_ONCE(disk->version, 1);
	seqcount_init(&disk->version_seq);
	disk->subscribe_flag = 0;
	nvmeib_trend_init(&disk->release_trend);
	nvmeib_trend_init(&disk->discover_trend);
	nvmeib_trend_init(&disk->peer_release_reason_trend);
	INIT_LIST_HEAD(&disk->arnics);
	INIT_LIST_HEAD(&disk->next_arnics);
	INIT_LIST_HEAD(&disk->link);
	INIT_LIST_HEAD(&disk->rionics);
	INIT_LIST_HEAD(&disk->nr_rionics);
	INIT_LIST_HEAD(&disk->volumes);
	INIT_LIST_HEAD(&disk->ioch_kill_list);
	INIT_LIST_HEAD(&disk->used_lock_segments);
	INIT_LIST_HEAD(&disk->pause_reqs);
	INIT_LIST_HEAD(&disk->local_nics);
	INIT_LIST_HEAD(&disk->db.dirty_bits_mappings);
	INIT_LIST_HEAD(&disk->db.dirty_bits_pending_reqs);
	INIT_LIST_HEAD(&disk->local_defer_io_list);
	WQ_INIT_WORK(&disk->local_defer_io_work, local_defer_io_work_fn);
	INIT_DELAYED_WORK(&disk->periodic_lock_channel_work, disk_periodic_lock_channel_work_func);
	init_completion(&disk->disk_paused);
	spin_lock_init(&disk->disk_conf_spinlock);
	spin_lock_init(&disk->spinlock);
	spin_lock_init(&disk->volume_spinlock);
	spin_lock_init(&disk->stats_spinlock);
	spin_lock_init(&disk->pause_reqs_lock);
	spin_lock_init(&disk->db.dirty_bits_spinlock);
	spin_lock_init(&disk->restart_lock);
	spin_lock_init(&disk->subscribe_lock);
	spin_lock_init(&disk->local_defer_io_lock);
//	mutex_init(&disk->locate_guard);
	init_waitqueue_head(&disk->wait_queue);
	init_waitqueue_head(&disk->deferred_io_wait);
	atomic_set(&disk->deferred_io_cnt, 0);
	hash_init(disk->toma_conn_hash);
	init_rwsem(&disk->segments_locks_local.guard);
	disk->segments_locks_local.is_local = true;
	atomic_set(&disk->n_cont_preventors, 0);
	disk->n_cont_prevents_waited_too_long = false;
	atomic_set(&disk->n_reused, 0);
	atomic_set(&disk->dying, 0);
	atomic_set(&disk->send_io_path_work_on_q, 0);
	INIT_LIST_HEAD(&disk->ioch_drained_pending_list);
	disk->jour.rng_id = NVMEIB_EC_INVALID_JOURNAL_RANGE;
	nvmeibc_jam_disk_cache_init(disk);
	disk->disconnect_jif = jiffies;
	disk->binje_ulp = p->binje;

	clnt_proc_name_format(pname, 'C', "WQ", "disk", nvmeibc_cinst_get_core_inst_num(p));
	if (!(disk->remove_wq = wq_create_verbose(pname))) {
		_NE(error_1_disk_nvmeibc_disk_create, "Fail to allocate volume disk removal queue");
		rv = -ENOMEM;
		goto free_disk;
	}

	strlcpy(disk->name, disk_id->name, sizeof(disk->name) - 1);
	disk->disk_host[0] = '?';
	snprintf(disk->full_name, sizeof(disk->full_name), "%.*s-%.*s",
		(int)sizeof(disk->disk_host), disk->disk_host,
		(int)sizeof(disk->name), disk->name);
	disk->create_jiff = jiffies;
	if (!(disk->stats = nvmeib_io_stats_create_traced(disk->name, VERB_RW_T_RECOV_BITMASK, 0))) {
		_NE(error_2_disk_nvmeibc_disk_create, "Failed to create disks stats for disk @DISK_NAME", disk->name);
		rv = -ENOMEM;
		goto free_disk;
	}
	if ((rv = disk_proc_create(disk)) < 0)
		goto free_disk;

	disk->percpu = nvmeib_public_alloc_percpu_cacheline(struct disk_percpu);
	if (unlikely(!disk->percpu)) {
		_NE(error_3_disk_nvmeibc_disk_create, "percpu disk failed: disk=@DISK", disk);
		rv = -ENOMEM;
		goto free_disk;
	}

	for_each_possible_cpu(i) {
		struct disk_percpu *pc = per_cpu_ptr(disk->percpu, i);
		pc->pause_preventers = 0;
		pc->in_transfers = 0;
	}

	if (!(disk->pcpu_cmds_stats = nvmeib_public_alloc_percpu_cacheline(
		struct nvmeibc_disk_percpu_cmds_stats))) {
		_NE(error_4_disk_nvmeibc_disk_create, "Failed to alloc");
		rv = -ENOMEM;
		goto free_disk;
	}

	if (!zalloc_cpumask_var(&disk->pcpu_nrchs_ll_cpumask, GFP_KERNEL)) {
		_NE(error_6_disk_nvmeibc_disk_create, "Failed to alloc");
		rv = -ENOMEM;
		goto free_disk;
	}

	if (nvmeibc_disk_net_intrs_stats_create(disk)) {
		rv = -ENOMEM;
		goto free_disk;
	}

#ifdef DEBUG_SUM
	atomic_set(&disk->in_transfers, 0);
#endif
#ifdef DEBUG_TRANSFERS
	do {
		int i;
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			spin_lock_init(&disk->transfer_spinlock[i]);
			INIT_LIST_HEAD(&disk->transferring[i]);
		}
	} while(0);
#endif
#ifdef NVMEIBC_DISK_CMD_DEBUG_UNCOMPLETED
	do {
		int i;
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			spin_lock_init(&disk->uncompleted_spinlock[i]);
			INIT_LIST_HEAD(&disk->uncompleted_cmds[i]);
			disk->n_uncompleted_cmds[i] = 0;
		}
	} while(0);
#endif
	strlcpy(disk->config_node_id, node_id, sizeof(disk->config_node_id));

	/* copy the admin channels */
	if ((rv = arnics_dup(disk, &disk->arnics, arnics)) < 0) {
		_NE(error_5_disk_nvmeibc_disk_create, "Failed to allocate nics data");
		rv = -ENOMEM;
		goto free_disk;
	}

	if ((rv = nvmeibc_populate_disk_local_nics(disk))) {
		_NT(trace_1_disk_nvmeibc_disk_create,
		   "Fail to populate local nics for disk @STR", disk->name);
		rv = -ENOMEM;
		goto free_disk;
	}

	/* register disk */
	nvmeibc_add_disk(disk);

	_NT(trace_disk_nvmeibc_disk_create, "Add (1st) volume @DEV_NAME_FULL to disk @DISK_NAME(@DISK)",
	   disk_id->volume->full_name, disk->name, disk);
	nvmeibc_disk_add_volume(disk, disk_id, num_ranges);

	/* discover disk connectivity
	   nvmeibc_disk_create() runs in the context of the main thread.
	   call_discover() runs in the context of the disk work_queue to sync with
	   disk disconnects...
	*/

	disk->local_server = nvmeibc_get_local_server(nvmeibc_cinst_get_core_p(disk));

//	if ((rv = nvmeibc_populate_disk_local_nics(disk))) {
//		_NT(trace_1_disk_nvmeibc_disk_create, "Fail to populate local nics for disk @DISK_NAME", disk->name);
//		rv = -ENOMEM;
//		goto free_disk;
//	}

	_NT(trace_2_disk_nvmeibc_disk_create, "@NUM_LNICS local nics added to disk @DISK_NAME", disk->num_lnics, disk->name);

	if (call_discover(disk) < 0) {
		_NT(trace_3_disk_nvmeibc_disk_create, "Fail to connect to disk @DISK_NAME", disk->name);
		rv = -1;
		goto out;
	}
	snprintf(disk->full_name, sizeof(disk->full_name), "%.*s-%.*s",
		(int)sizeof(disk->disk_host), disk->disk_host,
		(int)sizeof(disk->name), disk->name);
	_NT(trace_4_disk_nvmeibc_disk_create, "Disk @DISK_NAME is connected at host @DISK_HOST", disk->name, disk->disk_host);
	goto out;

free_disk:
	if (disk) {
		_NT(trace_nvmeibc_disk_create_failed, "failed to create @DISK_NAME, calling nvmeibc_disk_free", disk->name);
		nvmeibc_disk_free(disk);
	}

out:
	NDFOUTN(disk_id->name);
	return rv;
}

static void set_detach(struct nvmeibc_disk *disk)
{
	__NFIND;
	disk->detached = true;
	wake_up_interruptible(&disk->wait_queue);
	__NFOUTD;
}

void nvmeibc_disk_call_discover(struct nvmeibc_disk *disk)
{
	__NFIND;
	_NT(trace_nvmeibc_disk_call_discover,
		"Rediscover hint disk @DISK_NAME (@DISK)", disk->name, disk);
	disk->rediscover_timeout = 0;		// Someone got a hint that discovery should work. Reset exponential back-off
	disk->rediscovery_now = 1;
	wake_up_interruptible(&disk->wait_queue);
	__NFOUTD;
}

int nvmeibc_disk_add_work(struct nvmeibc_disk *disk,
	struct workqe_struct *work)
{
	return wq_add_work(disk->remove_wq, work) ? 0 : -1;
}

static void nvmeibc_disk_release_work(struct workqe_struct *work)
{
	struct disk_workq *rwork =
		container_of(work, struct disk_workq, work);
	struct nvmeibc_disk *disk = rwork->disk;
	enum nvmeibc_disk_release_reason reason = (enum nvmeibc_disk_release_reason)(rwork->work_data);

	__NFIND;
	_NT(trace_nvmeibc_disk_release_work,
		"Disk @DISK_NAME, run disk-release work "
		"(#@UINT, restart_called=@RESTART_CALLED)",
		disk->name, disk->drw_cnt, atomic_read(&disk->restart_called));

	kfree(rwork);
	nvmeibc_disk_release(disk, reason);
	__NFOUTD;
}

int nvmeibc_disk_start_release(struct nvmeibc_disk *disk, enum nvmeibc_disk_release_reason reason)
{
	struct disk_workq *rwork;
	int restart_called;
	int rv = 0;
	unsigned long flags;
	__NFIND;

	/* sanity - in case channel's ptr to disk was not set */
	if (!disk) {
		_NE(error_disk_nvmeibc_disk_start_release, "Oops: disk is NULL");
		rv = -1;
		goto out;
	}

	_NT(trace_disk_nvmeibc_disk_start_release, "disk @DISK_NAME (@DISK), attempt start-release from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		disk->name, disk, __builtin_return_address(0));

	if (!(rwork = kzalloc(sizeof(*rwork), GFP_ATOMIC))) {
		_NE(error_1_disk_nvmeibc_disk_start_release, "OOM: Fail to alloc disk-release work");
		rv = -ENOMEM;
		goto out;
	}

	/* prevent a race where one ctx manages to inc disk->restart_called 0->1 but
	   before it adds the release work,  volume-detach calls nvmeibc-disk_remove
	   which also try to add release work but cant. Thus, it drains the wq, but
	   nothing to drain and it calls nvmeibc-disk_free which frees the disk with
	   its wq -> hard lockup when ctx1 try to lock disk-wq when adding the work */
	spin_lock_irqsave(&disk->restart_lock, flags);
retry:
	if ((restart_called = atomic_inc_return(&disk->restart_called)) == 1) {
		unsigned long st_ents[2];
		struct nvmeib_stack_trace st = {
			.max_entries = 2,
			.entries = st_ents,
			.skip = 0,
		};

		WQ_INIT_WORK(&rwork->work, nvmeibc_disk_release_work);
		rwork->disk = disk;
		rwork->work_data = (void *)reason;
		atomic_inc(&disk->shut_down_triggered);
		disk->drw_cnt++;
		nvmeib_public_save_stack_trace(&st);
		_NI(trace_2_disk_nvmeibc_disk_start_release,
			"Initiate disk @DISK_NAME (@DISK) release work (#@UINT) from "
			"'@__BUILTIN_RETURN_ADDRESS_FUNC' <-- "
			"'@__BUILTIN_RETURN_ADDRESS_FUNC'",
			disk->name, disk, disk->drw_cnt,
			(void *)st_ents[0], (void *)st_ents[1]);

		_NI(trace_nvmeibc_disk_release_work_reason, "Initiate disk @DISK_NAME (@DISK) release work (#@UINT) reason: @DISK_RELEASE_OP",
		disk->name, disk, disk->drw_cnt, reason);

		if ((rv = nvmeibc_disk_add_work(disk, &rwork->work)) < 0) {
			_NW(trace_1_disk_nvmeibc_disk_start_release,
				"Failed to add disk release work (#@UINT) for disk @DISK_NAME (dec to @RESTART_CALLED)",
				disk->drw_cnt, disk->name, atomic_read(&disk->restart_called));
			atomic_set(&disk->restart_called, 0);
			goto retry;
		}
		else {
			/* we are no longer protected by atomicity of @disk->restart_called */
		}
	}
	else {
		_NT(trace_3_disk_nvmeibc_disk_start_release, "disk @DISK_NAME, already called (inc to @RESTART_CALLED)",
			disk->name, restart_called);
		if (rwork)
			kfree(rwork);
	}
	spin_unlock_irqrestore(&disk->restart_lock, flags);

out:
	__NFOUTD;
	return rv;
}

void nvmeibc_disk_block_remove(struct nvmeibc_disk_id *disk_id)
{
	NFIN;

	wq_drain(disk_id->disk->remove_wq);
	nvmeibc_del_disk(disk_id->disk);
	_NT(trace_disk_nvmeibc_disk_block_remove, "Disk:@DISK_NAME (@DISK) is released", disk_id->disk->name, disk_id->disk);
	nvmeibc_disk_free(disk_id->disk);
	disk_id->disk = NULL;
	NFOUT;
}

bool nvmeibc_disk_remove(struct nvmeibc_disk_id *disk_id, bool block)
{
	bool is_empty;
	unsigned long flags;
	struct nvmeibc_disk *disk;
	bool need_block = 0;

	NDFINN(disk_id->name);
	spin_lock_irqsave(&disk_id->disk->volume_spinlock, flags);
	list_del(&disk_id->slink);
	disk = disk_id->disk;
	disk->n_ranges -= disk_id->num_ranges;
	/* Note: disk_id->num_ranges is 0 when relocating segment.
	   > 0 when detachhing volume, since num_ranges-- still was not executed */
	is_empty = list_empty(&disk->volumes);
	spin_unlock_irqrestore(&disk_id->disk->volume_spinlock, flags);
	BUG_ON(is_empty && disk->n_ranges != 0);
	BUG_ON(disk->n_ranges < 0);
	/* Note: (!is_empty && disk->n_ranges == 0) is legal when vol attach fails
		 due to error and is immediately dettached */
	if (is_empty) {
		_ND(trace_disk_nvmeibc_disk_remove, "Finally disk @DISK_NAME is empty", disk_id->name);
		set_detach(disk_id->disk);
		/* after we set the detach we must for current rediscovery activities */
		_NT(trace_1_disk_nvmeibc_disk_remove, "Wait for disk_release right after setting detach");
		disk_proc_destroy(disk); /* before destroying disk-wq (and disk-obj) */

		/* NVMESH-4178 - speedup volume-detach...
		   wq_drain(disk->remove_wq); */

		nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_DISK_REMOVE);
		if (block) {
			wq_drain(disk->remove_wq);
			nvmeibc_del_disk(disk);
			_NT(trace_2_disk_nvmeibc_disk_remove, "Disk:@DISK_NAME (@DISK) is released", disk->name, disk);
			nvmeibc_disk_free(disk);
			disk_id->disk = NULL;
			need_block = false;// no need to block, we just did it
		} else
			need_block = true;
	} else
		need_block = false;

	NDFOUTN(disk_id->name);
	return need_block;
}

void nvmeibc_disk_add_volume(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_id *disk_id, int num_ranges)
{
	unsigned long flags;

	__NFIND;
	spin_lock_irqsave(&disk->volume_spinlock, flags);
	disk_id->disk = disk;
	list_add_tail(&disk_id->slink, &disk->volumes);
	/* disk->n_ranges += num_ranges; Do not do that!!! */
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
	__NFOUTD;
}

void __attribute__ ((unused)) nvmeibc_disk_pause(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_id *disk_id;
	unsigned long flags;
	int is_ready, num_pauses;
	BUG_ON(disk == NULL);
	__NFIND;

	spin_lock_irqsave(&disk->volume_spinlock, flags);
	disk->should_pause = true;
	num_pauses = atomic_inc_return(&disk->paused);
	list_for_each_entry(disk_id, &disk->volumes, slink)
		if (disk_id->volume) {
			nvmeibc_volume_get(disk_id->volume, NULL);
			is_ready = nvmeibc_volume_is_ready_for_pause_cont(disk_id->volume);
			if (is_ready>0)
				BUG_ON(nvmeibc_block_pause(disk_id->volume->block_dev, disk));
			else
				_NI(t_03_spcvd, "Skipping pause on @DISK_NAME for volume @DEV_NAME", disk->name, disk_id->volume->hdr.devname);
			nvmeibc_volume_put(disk_id->volume, NULL);
		}
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
	/* After this, no IO will be sent to the disk */
	if (num_pauses == 1)
		nvmeibc_pd_pause(disk, NULL, NULL);
	else { /* pd_pause() was already called in interrupt context. */ }
	__NFOUTD;
}

int nvmeibc_disk_extract_piggyback_lock_info(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *iocmd, struct lock_seg_info *lsia,
	enum nvmeibc_disk_locks_opr opr_type, struct lock_seg_info **lsi, u64 *offset)
{
	int seg_id;
	int i, rv = 0;

	__NFIND;
	*lsi = NULL;
	/* ignore if no comp */
	if (!dp_cmds_pigbck_has_any(iocmd)) {
		_ND(trace_disk_nvmeibc_disk_extract_piggyback_lock_info, "no comp therefore no need to extract piggyback lock info");
		goto out;
	}

	if ((rv = nvmeibc_disk_locks_extract_info(iocmd->lpb.handle,
		iocmd->lpb.addr, &seg_id, opr_type, offset)) < 0) {
		_NT(trace_1_disk_nvmeibc_disk_extract_piggyback_lock_info, "Failed to extract read lock piggyback info");
		goto out;
	}
	rv = -1;
	/* the following search should be removed once we moved from
	   a single segment per disk - Yaron please merge it with the lock handle
	*/
	for (i = 0; i < disk->info->n_disk_lock_segments; ++i) {
		if (lsia[i].seg_id == seg_id) {
			_ND(trace_2_disk_nvmeibc_disk_extract_piggyback_lock_info, "DB: found requested segment");
			*lsi = &lsia[i];
			rv = 0;
			break;
		}
	}
	if (rv < 0) {
		_NT(trace_3_disk_nvmeibc_disk_extract_piggyback_lock_info, "Segment @SEG_ID_INT is not available for read lock piggyback", seg_id);
		goto out;
	}

out:
	__NFOUTD;
	return rv;
}

/*
 * Disk:Toma
 */

static struct nvmeibc_toma_connection_hash_entry
*nvmeibc_disk_toma_conn_hash_lookup_nolock(struct nvmeibc_disk *disk, u64 handle)
{
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibc_toma_connection_hash_entry *h_curr = NULL;

	__NFIND;

	__hash_for_each_possible_safe__(disk->toma_conn_hash, h_curr, t_node,
		h_node, hlist_next, handle) {
		_ND(trace_disk_nvmeibc_disk_toma_conn_hash_lookup_nolock, "compare handle @HANDLE ", h_curr->handle);
		if (h_curr->handle == handle) {
			_ND(trace_1_disk_nvmeibc_disk_toma_conn_hash_lookup_nolock, "found handle @HANDLE ", h_curr->handle);
			break;
		}
	}

	__NFOUTD;
	return h_curr;
}

static struct nvmeibc_toma_connection_hash_entry *
nvmeibc_disk_toma_conn_hash_lookup(struct nvmeibc_disk *disk, u64 handle)
{
	unsigned long flags;
	struct nvmeibc_toma_connection_hash_entry *toma_conn_hent;

	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);
	toma_conn_hent = nvmeibc_disk_toma_conn_hash_lookup_nolock(disk, handle);
	spin_unlock_irqrestore(&disk->spinlock, flags);

	__NFOUTD;
	return toma_conn_hent;
}

static struct nvmeibc_toma_connection_hash_entry *
nvmeibc_disk_toma_conn_hash_add_nolock(struct nvmeibc_disk *disk, u64 handle,
	struct nvmeibc_disk_subscription_params *params)
{
	struct nvmeibc_toma_connection_hash_entry *toma_conn_hent;

	__NFIND;

	if (!(toma_conn_hent = kzalloc(sizeof(*toma_conn_hent), GFP_ATOMIC))) {
		_NE(error_disk_nvmeibc_disk_toma_conn_hash_add_nolock, "Fail to allocate toma connection hash entry");
		goto out;
	}

	toma_conn_hent->handle = handle;
	toma_conn_hent->recv_req_cb = params->recv_req_cb;
	toma_conn_hent->arg = params->arg;
	toma_conn_hent->subscribed = TOMA_NEED_SUBSCRIBED;
	hash_add(disk->toma_conn_hash, &toma_conn_hent->hlist_next, handle);

	_NT(trace_disk_nvmeibc_disk_toma_conn_hash_add_nolock, "Added toma connection handle @HANDLE to hash", handle);

out:
	__NFOUTD;
	return toma_conn_hent;
}

#if 0
static void __dump_hash(struct nvmeibc_disk *disk)
{
	int bucket;
	struct hlist_node *t_node;
	struct hlist_node *h_node;
	struct nvmeibc_toma_connection_hash_entry *h_curr;

	int n = 0;
	int rv = 0;

	void *ch = 0;

	__hash_for_each_safe__(disk->toma_conn_hash, bucket, t_node, h_node, h_curr,
		hlist_next) {
		_ND(__dump_hash_d1, "bucket = @INT", bucket);
		switch (h_curr->subscribed) {
		case TOMA_NEED_SUBSCRIBED:
			_ND(__dump_hash_d2,
				"NEED_SUBSCRIBED disk=@PTR ch=@PTR handle=@_X rv=@INT",
				disk, ch, h_curr->handle, rv);
			++n;
			break;
		case TOMA_ALREADY_SUBSCRIBED:
			_ND(__dump_hash_d3,
			   "ALREADY_SUBSCRIBED disk=@PTR ch=@PTR handle=@_X rv=@INT",
			   disk, ch, h_curr->handle, rv);
			++n;
			break;
		default:
			_ND(__dump_hash_d4,
				"Unknown subscription state @INT, disk=@PTR ch=@PTR handle=@_X",
			   h_curr->subscribed, disk, ch, h_curr->handle);
			break;
		}
	}

	_ND(__dump_hash_d5
		"@INT toma handles of disk @STR [h_curr=@PTR]",
		n, disk->name, h_curr);
}
#endif

static struct nvmeibc_toma_connection_hash_entry *
nvmeibc_disk_toma_conn_hash_add(struct nvmeibc_disk *disk, u64 handle,
	struct nvmeibc_disk_subscription_params *params)
{
	struct nvmeibc_toma_connection_hash_entry *rv = NULL;
	unsigned long flags;

	__NFIND;
	_NT(trace_disk_nvmeibc_disk_toma_conn_hash_add, "Locking @DISK_NAME", disk->name);
	spin_lock_irqsave(&disk->spinlock, flags);
	_NT(trace_1_disk_nvmeibc_disk_toma_conn_hash_add, "Locked");

	// __dump_hash(disk);

	if (nvmeibc_disk_toma_conn_hash_lookup_nolock(disk, handle) == NULL)
		rv = nvmeibc_disk_toma_conn_hash_add_nolock(disk, handle, params);
	else
		_NT(trace_2_disk_nvmeibc_disk_toma_conn_hash_add, "Fail to add toma connection, handle @HANDLE already hashed", handle);

	// __dump_hash(disk);

	spin_unlock_irqrestore(&disk->spinlock, flags);
	_NT(trace_3_disk_nvmeibc_disk_toma_conn_hash_add, "Unlocked");
	__NFOUTD;
	return rv;

}

static void nvmeibc_disk_async_subscribe_toma_comp(struct nvmeibc_disk *disk, u64 handle, int status){
	unsigned long flags;
	struct nvmeibc_toma_connection_hash_entry *h = NULL;
	spin_lock_irqsave(&disk->spinlock, flags);
	h = nvmeibc_disk_toma_conn_hash_lookup_nolock(disk, handle);
	if (!h) {
		_NE(trace_nvmeibc_disk_async_subscribe_toma_comp_not_found, "SUBSCRIBE: Toma connection handle @HANDLE not found in hash, status was not changed, disk @DISK_NAME", handle, disk->name);
		goto out;
	}
	if (!status) {
		_NT(trace_nvmeibc_disk_async_subscribe_toma_comp_changed_success, "SUBSCRIBE: Toma connection handle @HANDLE status changed to TOMA_ALREADY_SUBSCRIBED, disk @DISK_NAME", handle, disk->name);
		h->subscribed = TOMA_ALREADY_SUBSCRIBED;
	} else {
		_NT(trace_nvmeibc_disk_async_subscribe_toma_comp_changed_failed, "SUBSCRIBE: Async subscribe failed. Setting handle @HANDLE status to TOMA_NEED_SUBSCRIBED, disk @DISK_NAME", handle, disk->name);
		h->subscribed = TOMA_NEED_SUBSCRIBED;
	}
out:
	spin_unlock_irqrestore(&disk->spinlock, flags);
}

static int nvmeibc_disk_toma_conn_hash_del(struct nvmeibc_disk *disk, u64 handle)
{
	unsigned long flags;
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibc_toma_connection_hash_entry *h_curr;
	bool found = false;
	int rv;

	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);
	__hash_for_each_possible_safe__(disk->toma_conn_hash, h_curr, t_node, h_node, hlist_next, handle) {
		if (h_curr->handle == handle) {
			hash_del(&h_curr->hlist_next);
			kfree(h_curr);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&disk->spinlock, flags);

	if (!found) {
		_NT(trace_disk_nvmeibc_disk_toma_conn_hash_del, "Toma connection handle @HANDLE not found in hash, was not removed", handle);
	}

	rv = found ? 0 : -1;

	__NFOUTD;
	return rv;
}

static int toma_send_cmd(struct nvmeibc_toma_connection_hash_entry *h,
	struct nvmeibc_ib_admin_channel *ch, int op,
	struct nvmeibc_disk_toma_send_params *params, bool is_async)
{
	struct nvmeibc_disk_toma_cmd toma_cmd = {0};
	int rv = -1;

	NFIN;
	/* send toma re-registration */
	toma_cmd.handle = h->handle;
	toma_cmd.type = op;
	toma_cmd.send_params = params;
	if ((rv = is_async ?
		 nvmeibc_ib_admin_send_toma_cmd_async(ch, &toma_cmd) :
		 nvmeibc_ib_admin_send_toma_cmd(ch, &toma_cmd)) < 0) {
		_NT(trace_disk_toma_send_cmd, "Failed to send toma re-registration, handle @HANDLE", h->handle);
	}
	NFOUT;
	return rv;
}

static int toma_send_subscribe(struct nvmeibc_toma_connection_hash_entry *entry,
	struct nvmeibc_ib_admin_channel *ch, bool is_async)
{
	int rv;
	NFIN;

	rv = toma_send_cmd(entry, ch, NVMEIB_TOMA_CMD_REG, NULL, is_async);
	if (rv) {
		struct nvmeibc_disk *disk = ch->base.base.disk;
		/* we've already committed to send SUBSCRIBE(after successful discover
		   and finding main-admin-ch), as currently there's no retry mechanism
		   but next successful discover -> ensure disk release and rediscover */
		_NT(trace_0_toma_send_subscribe,
			"Disk @DISK_NAME (@DISK), failed SUBSCRIBE (@RV)",
			disk->name, disk, rv);
		nvmeibc_disk_start_release(ch->base.base.disk, NVMEIBC_DISK_RELEASE_TOMA_SUBSCRIBE_FAILURE);
	}

	NFOUT;
	return rv;
}

/**
 * Register a volume to TOMA over the disk's admin channel
 * - Send toma register request
 * - Add toma connection hash entry
 *
 */

/* EC-5023: Checking disk is online (i.e. not pauesd) was causing this race:
 *  		After discover we wakeup disk-freeze waiters but we DONT mark the
 *  		disk as online. Thus, there is a small window where toma-cmd will
 *  		be deferred but will be never sent as we are past the point where
 *  		we send deferred cmds i.e. toma_rereg_disk_handles().
 *
 * We could mark online before wakuep waiters but that's risky and frankely
 * uncalled for. Best thing is to have some refcount to control whether ulp
 * can send toma-cmd or not && will be in sync with toma-rereg.
 * Instead, we just DONT check online flag but we need to add ch->toma.valid
 * flag (which we check under disk-freeze)  to prevent from sending toma-cmd
 * before ach control sequence had completed.
 *
 * Correctness:
 *^^^^^^^^^^^
 * Term: RRII = 'redicover (=rereg) is imminent'
 *
 * As modify ch->toma.valid and sending toma-cmd are mutually exclusive
 * via disk-freeze, the following exist for the either frz0 or frz1 intervals :
 *
 *  ...|<--discovery-->|<--frz0-->|<--disk-release-->|<--frz1-->|...
 *
 * frz0, past discovery && before starting disk-release :
 *  	@ discover rv == 0 : toma-valid && ach is *NOW* either:
 *  						(1) live --> send  cmd, or;
 *							(2) dead --> defer cmd, RRII
 *   	@ discover rv != 0 : toma is either:
 *   	  					(1)  valid --> if ach still alive send cmd (in vain) o/w defer
 *  						(2) !valid --> even if ach is alive, dont send toma-cmd before
 *  					                   ach control sequence !!!
 *  						--> in any case RRII
 *
 * frz1, past disk-release && before starting discover :
 *      @ ach is NOT alive  --> defer, RRII.
 */
int nvmeibc_disk_subscribe_toma_service(struct nvmeibc_disk *disk, u64 handle,
	struct nvmeibc_disk_subscription_params *params)
{
	struct nvmeibc_ib_admin_channel *ch = NULL;
	struct nvmeibc_toma_connection_hash_entry *entry;
	unsigned long flags = 0;
	int subscribe_permission;
	int rv = -ENOMEM;	/* Fatal error */

	__NFIND;

	_NT(trace_disk_nvmeibc_disk_subscribe_toma_service, "Attempt SUBSCRIBE disk=@DISK handle=@HANDLE", disk, handle);

	if (params->recv_req_cb == NULL) {
		_NT(trace_1_disk_nvmeibc_disk_subscribe_toma_service, "No receive callback function specified!");
		goto out;
	}

	/* prevent disk to go into release mode */
	_NT(trace_2_disk_nvmeibc_disk_subscribe_toma_service, "Waiting for SUBSCRIBE lock to add subscription to hash as NEED_SUBSCRIBED");
	subscribe_permission = disk_toma_subscribe_lock_return_permission(disk, &flags);
	/* add toma connection to hash using handle as hash-key */
	if ((entry = nvmeibc_disk_toma_conn_hash_add(disk, handle, params)) ==
		NULL) {
		_NT(trace_3_disk_nvmeibc_disk_subscribe_toma_service, "Failed toma connection hash add");
		goto unlock;
	}
	if (subscribe_permission){
		/* meaning we are in the middle of release/rediscovery */
		rv = -EAGAIN;
		goto unlock;
	}

	/*
		if we got here, this means either two things:
		1. discovery didn't happen yet (and won't happen as long as we hold the subscribe_lock),
		meaning next phase will fail as there is no admin channel yet.
		OR
		2. disk was already discovered in the past, and this is new segment on the same disk.
			in this case, if for some odd reason we fail  "if ((ch = get_alive_admin_ch(disk)) && ch->toma.valid)",
			we must make sure rediscovery happens, but as keep-alive is watching over, it is guaranteed.
	*/

	/* If we fail to subscribe, we return -EAGAIN which is NOT an error
	   for upper layer which and means that transport layer will retry
	   subscribe on next successful rediscovery */
	rv = -EAGAIN;

	//EC-5023: Dont check disk is online, see comment above.
	//if (nvmeibc_disk_get_status(disk) == d_online) {
		_NT(trace_4_disk_nvmeibc_disk_subscribe_toma_service, "Disk online");
		if ((ch = get_alive_admin_ch(disk)) && ch->toma.valid) {
			_NT(trace_5_disk_nvmeibc_disk_subscribe_toma_service,
				"SUBSCRIBE disk=@DISK handle=@HANDLE", disk, handle);
			if (nvmeibc_disk_use_async_subscribe){
				_NT(trace_6_disk_nvmeibc_disk_subscribe_toma_service,
				"About to send async SUBSCRIBE disk=@DISK handle=@HANDLE", disk, handle);
				entry->subscribed = TOMA_ASYNC_SUBSCRIBE_SENT;
			}
			if ((rv = toma_send_subscribe(entry, ch, nvmeibc_disk_use_async_subscribe)) < 0) {
				_NT(trace_7_disk_nvmeibc_disk_subscribe_toma_service,
					"Failed to send toma SUBSCRIBE disk=@DISK handle @HANDLE - "
					"retry on rediscovery", disk, handle);
				rv = -EAGAIN;
			}
			else {
				_NT(trace_8_disk_nvmeibc_disk_subscribe_toma_service,
					"SUBSCRIBE SUCCESS disk=@DISK handle=@HANDLE", disk, handle);
				if (!nvmeibc_disk_use_async_subscribe)
					entry->subscribed = TOMA_ALREADY_SUBSCRIBED;
			}
		}
		else {
			_NT(trace_9_disk_nvmeibc_disk_subscribe_toma_service,
				"Deferring SUBSCRIBE disk=@DISK handle=@HANDLE (ach=@PTR)",
				disk, handle, ch);
		}
	//}

unlock:
	disk_toma_subscribe_unlock(disk, &flags);

out:
	__NFOUTD;
	return rv;
}

/**
 * Unregister a volume to TOMA over the disk's admin channel
 * - Send toma unregister request
 * - Remove toma connection hash entry
 *
 */
int nvmeibc_disk_unsubscribe_toma_service(struct nvmeibc_disk *disk, u64 handle)
{
	struct nvmeibc_ib_admin_channel *ch = NULL;
	struct nvmeibc_toma_connection_hash_entry *entry;
	int rv = -1;

	__NFIND;

	_NT(trace_disk_nvmeibc_disk_unsubscribe_toma_service, "Attempt UNSUBSCRIBE disk=@DISK handle=@HANDLE", disk, handle);

	/* check connection exists - lookup handle in toma-connection hash */
	if (!(entry = nvmeibc_disk_toma_conn_hash_lookup(disk, handle))) {
		_NT(trace_1_disk_nvmeibc_disk_unsubscribe_toma_service, "Toma connection handle @HANDLE not found in hash", handle);
		goto out;
	}

	/* [NVMESH-3829]: Also send the unsubscribe if the subscribe is still in-progress.
	 * 		(It is serialised on the admin WQ) */
	if (entry->subscribed == TOMA_ALREADY_SUBSCRIBED || entry->subscribed == TOMA_ASYNC_SUBSCRIBE_SENT) {
		if (disk->base.ops.get_status(&(disk->base)) == d_online) {
			_NT(trace_2_disk_nvmeibc_disk_unsubscribe_toma_service, "Disk online");
			if ((ch = get_alive_admin_ch(disk))) {
				_NT(trace_3_disk_nvmeibc_disk_unsubscribe_toma_service, "UNSUBSCRIBE disk=@DISK handle=@HANDLE", disk, handle);
				if ((rv = toma_send_cmd(
					entry, ch, NVMEIB_TOMA_CMD_UNREG, NULL, true)) < 0) {
					_NT(trace_4_disk_nvmeibc_disk_unsubscribe_toma_service, "Failed to send toma UNSUBSCRIBE, handle @HANDLE",
						handle);
				}
				else {
					_NT(trace_5_disk_nvmeibc_disk_unsubscribe_toma_service, "Added send-UNSUBSCRIBE work, disk=@DISK handle=@HANDLE",
					   disk, handle);
					goto out;
				}
			}
		}
	}
	else
		_NT(trace_6_disk_nvmeibc_disk_unsubscribe_toma_service, "Toma-conn is not subscribed, state @SUBSCRIBED", entry->subscribed);

	/* delete toma connection from hash */
	_NT(trace_7_disk_nvmeibc_disk_unsubscribe_toma_service, "Omitting UNSUBSCRIBE, delete handle @HANDLE of disk @DISK", handle ,disk);
	if ((rv = nvmeibc_disk_toma_conn_hash_del(disk, handle)) < 0) {
		_NE(error_disk_nvmeibc_disk_unsubscribe_toma_service, "Fail to delete toma connection from hash");
	}

out:
	__NFOUTD;
	return rv;
}

static void nvmeibc_disk_unsubscribe_toma_comp(struct nvmeibc_disk *disk, int status,
										u64 handle)
{
	__NFIND;
	_NT(trace_disk_nvmeibc_disk_unsubscribe_toma_comp, "UNSUBSCRIBE comp status @STATUS, disk=@DISK handle=@HANDLE",
	   status, disk, handle);
	if (nvmeibc_disk_toma_conn_hash_del(disk, handle) < 0)
		_NT(trace_1_disk_nvmeibc_disk_unsubscribe_toma_comp, "Failed to delete toma connection from hash");
	__NFOUTD;
}

/**
 * Send toma buffer of a volume to TOMA over the disk's admin
 * channel
 *
 */
int nvmeibc_disk_toma_send(struct nvmeibc_disk *disk, u64 handle,
						   struct nvmeibc_disk_toma_send_params *params)
{
	struct nvmeibc_ib_admin_channel *ch = NULL;
	struct nvmeibc_toma_connection_hash_entry *entry;
	int rv = -1;

	__NFIND;

	if (atomic_read(&disk->dying) ||
		atomic_read(&disk->paused)) {
		_NT(trace_disk_nvmeibc_disk_toma_send, "Disk @DISK_NAME is dying", disk->name);
		goto out;
	}

	if (!(ch = get_alive_admin_ch(disk)))
		goto out;

	/* check connection exists - lookup handle in toma-connection hash */
	if (!(entry = nvmeibc_disk_toma_conn_hash_lookup(disk, handle))) {
		_NT(trace_1_disk_nvmeibc_disk_toma_send, "Toma connection handle @HANDLE not found in hash", handle);
		goto out;
	}

	/* Note - no need to check for TOMA_ASYNC_SUBSCRIBE_SENT, as if it does,
	   that means it is already running on the admin channel wq.
	   If it will fail, it will trigger release */
	if (entry->subscribed == TOMA_NEED_SUBSCRIBED) {
		_NT(warn_disk_nvmeibc_disk_toma_send,
			"Disk=@DISK, sending toma-cmd on non-SUBSCRIBED handle=@HANDLE",
			disk, handle);
		goto out;
	}

	/* send toma data */
	_ND(trace_2_disk_nvmeibc_disk_toma_send, "send toma msg over disk=@DISK ch=@CH_PTR with handle=@HANDLE", disk, ch, handle);
	if ((rv = toma_send_cmd(
		entry, ch, NVMEIB_TOMA_CMD_SEND, params, true)) < 0) {
		_NT(trace_3_disk_nvmeibc_disk_toma_send, "Failed to send toma async cmd, handle @HANDLE", handle);
		goto out;
	}

out:
	__NFOUTD;
	return rv;
}

void nvmeibc_disk_toma_recv(struct nvmeibc_disk *disk,
							struct nvmeibc_toma_recv_msg *toma_recv_msg)
{
	struct nvmeibc_toma_connection_hash_entry *toma_conn_hent = NULL;
	u8 *buf = toma_recv_msg->buf;
	int len = toma_recv_msg->len;

	__NFIND;

	/* lookup handle in toma-connection hash */
	toma_conn_hent = nvmeibc_disk_toma_conn_hash_lookup(disk, toma_recv_msg->handle);
	if (!toma_conn_hent) {
		_NT(trace_disk_nvmeibc_disk_toma_recv, "Toma connection handle @HANDLE not found in hash, no recv cb func to call",
		   toma_recv_msg->handle);
		goto out;
	}

	if (toma_conn_hent->recv_req_cb == NULL) {
		_NT(trace_1_disk_nvmeibc_disk_toma_recv, "No receive callback function to call!");
		goto out;
	}

	toma_conn_hent->recv_req_cb((void*)nvmeibc_cinst_get_core_p(disk) /* Transition core->block layer*/, toma_conn_hent->arg, buf, len);

out:
	__NFOUTD;
}

static int toma_rereg_disk_handles(struct nvmeibc_disk *disk,
								   struct nvmeibc_ib_admin_channel *ch,
								   bool is_async)
{
	int bucket;
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibc_toma_connection_hash_entry *h_curr;

	int n_reg = 0;
	int n_rereg = 0;
	int n_async_reg = 0;
	int rv = 0;

	__NFIND;
	__hash_for_each_safe__(disk->toma_conn_hash, bucket, t_node, h_node, h_curr,
			       hlist_next) {
		switch (h_curr->subscribed) {
		case TOMA_NEED_SUBSCRIBED:
			rv = toma_send_subscribe(h_curr, ch, is_async);
			_NT(toma_rereg_disk_handles_t1,
				"NEED_SUBSCRIBED disk=@PTR ch=@PTR handle=@_X rv=@INT",
				disk, ch, h_curr->handle, rv);
			++n_reg;
			break;
		case TOMA_ALREADY_SUBSCRIBED:
			rv = toma_send_subscribe(h_curr, ch, is_async);
			_NT(toma_rereg_disk_handles_t2,
				"ALREADY_SUBSCRIBED disk=@PTR ch=@PTR handle=@_X rv=@INT",
				disk, ch, h_curr->handle, rv);
			++n_rereg;
			break;
		case TOMA_ASYNC_SUBSCRIBE_SENT:
			rv = toma_send_subscribe(h_curr, ch, is_async);
			_NT(toma_rereg_disk_handles_t3,
				"TOMA_ASYNC_SUBSCRIBE_SENT disk=@PTR ch=@PTR handle=@_X rv=@INT",
				disk, ch, h_curr->handle, rv);
			++n_async_reg;
			break;
		default:
			_NT(toma_rereg_disk_handles_t4,
				"Unknown subscription state @INT, disk=@PTR ch=@PTR handle=@_X",
			   h_curr->subscribed, disk, ch, h_curr->handle);
			rv = -1;
			break;
		}

		if (!rv)
			h_curr->subscribed = TOMA_ALREADY_SUBSCRIBED;
		else {
			_NT(toma_rereg_disk_handles_t5,
				"Failed to re/reg (all) toma handles of disk @STR (rv @INT)",
			   disk->name, rv);
			break;
		}
	}

	_NT(trace_disk_toma_rereg_disk_handles,
		"Re/registered/async @N_REREG/@N_REREG/@N_REREG toma handles of disk @DISK_NAME",
		n_rereg, n_reg, n_async_reg, disk->name);

	__NFOUTD;
	return rv;
}

static int nvmeibc_disk_toma_create(struct nvmeibc_disk *disk,
									bool is_rediscover)
{
	struct nvmeibc_ib_admin_channel *ch = NULL;
	int rv = -1;
	struct nvmeibc_admin_rnic *arnic __attribute__((unused));

	__NFIND;

	/* find the disk's ('main') admin ch */
	if (!(ch = get_alive_admin_ch(disk))) {
		_NT(trace_disk_nvmeibc_disk_toma_create, "no alive toma for disk @DISK_NAME", disk->name);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_TOMA_NO_ADMIN_CH);
		goto out;
	}
	arnic = ch->base.arnic;

	/* create toma interface over the disk's ('main') admin ch */
	if ((rv = nvmeibc_toma_create(ch, nvmeibc_disk_unsubscribe_toma_comp, nvmeibc_disk_async_subscribe_toma_comp)) < 0){
		_NT(trace_1_disk_nvmeibc_disk_toma_create, "Toma creation for disk @DISK_NAME failed code @RV", disk->name, rv);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_TOMA_DEV_CREATION_FAILED);
		goto out;
	}

out:
	__NFOUTD;
	return rv;
}

static void toma_conn_hash_flush(struct nvmeibc_disk *disk)
{
	int bucket;
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibc_toma_connection_hash_entry *h_curr;

	__NFIND;

	__hash_for_each_safe__(disk->toma_conn_hash, bucket, t_node, h_node, h_curr,
		hlist_next) {
		_NT(toma_conn_hash_flush_t1,
			"Found toma connection hash (handle @_X) while removing disk",
			h_curr->handle);
		hash_del(&h_curr->hlist_next);
		kfree(h_curr);
	}

	__NFOUTD;
}

static void nvmeibc_disk_toma_free(struct nvmeibc_disk *disk)
{
	__NFIND;

	toma_conn_hash_flush(disk);

	__NFOUTD;
}

struct nvmeibc_disk_segments_locks *  nvmeibc_disk_get_segs_locks(struct nvmeibc_disk *disk,
																struct nvmeibc_disk_get_segs_locks_flags get_segs_locks_flags)
{
	struct nvmeibc_disk_segments_locks *ret;

	NFIN;
	_ND(t0_nvmeibc_disk_get_segs_locks,
		"Disk @DISK_NAME (@PTR), flags: @INT",
		disk->name, disk, *((int *)(&get_segs_locks_flags)));

	BUG_ON(get_segs_locks_flags.local_only && get_segs_locks_flags.remote_only);
	//BUG_ON(get_segs_locks_flags.local_only && !disk->access_local);

	if (get_segs_locks_flags.local_only || disk->access_local)
		ret = &disk->segments_locks_local;
	else if (!get_segs_locks_flags.local_only && disk->info && disk->info->ch) {
		ret = &(disk->info->ch->segments_locks_remote);
	} else {
		_NT(t1_nvmeibc_disk_get_segs_locks,
			"Disk @DISK_NAME (@PTR)- Lock channel not connected",
			disk->name, disk);
		ret = NULL;
		goto out;
	}


	if (get_segs_locks_flags.write) {
		if (!get_segs_locks_flags.dont_wait) {
			down_write(&ret->guard);
		}
		else if (!down_write_trylock(&ret->guard)) {
			_ND(t2_nvmeibc_disk_get_segs_locks,
				"Disk @DISK_NAME (@PTR), ret=@PTR, locked-write - FAILED",
				disk->name, disk, ret);
			ret = NULL;
			goto out;
		}
		_ND(t3_nvmeibc_disk_get_segs_locks,
			"Disk @DISK_NAME (@PTR), ret=@PTR, lock-write, @K_PID from @FN",
			disk->name, disk, ret, current->pid, __builtin_return_address(0));
	}
	else {
		if (!get_segs_locks_flags.dont_wait) {
			if (unlikely(rwsem_is_locked(&ret->guard))) {
				_NT(t4_nvmeibc_disk_get_segs_locks,
					"Disk @DISK_NAME (@PTR), ret=@PTR, lock-read - WAITING...",
					disk->name, disk, ret);
			}
			down_read(&ret->guard);
		}
		else if (!down_read_trylock(&ret->guard)) {
			_ND(t5_nvmeibc_disk_get_segs_locks,
				"Disk @DISK_NAME (@PTR), ret=@PTR, lock-read - FAILED",
				disk->name, disk, ret);
			ret = NULL;
			goto out;
		}
		_ND(t6_nvmeibc_disk_get_segs_locks,
			"Disk @DISK_NAME (@PTR), ret=@PTR, locked-read, @K_PID from @FN",
			disk->name, disk, ret, current->pid, __builtin_return_address(0));
	}

out:
	NFOUT;
	return ret;
}

void nvmeibc_disk_put_segs_locks(struct nvmeibc_disk_segments_locks *seg_locks,
								 struct nvmeibc_disk_get_segs_locks_flags get_segs_locks_flags)
{
	if (seg_locks) {
		if (get_segs_locks_flags.write) {
			up_write(&seg_locks->guard);
			_ND(t0_nvmeibc_disk_put_segs_locks,
				"Disk @DISK_NAME (@PTR), ret=@PTR, unlocked-write, @K_PID from @FN",
				seg_locks->disk->name, seg_locks->disk, seg_locks,
				current->pid, __builtin_return_address(0));
		}
		else {
			up_read(&seg_locks->guard);
			_ND(t1_nvmeibc_disk_put_segs_locks,
				"Disk @DISK_NAME (@PTR), ret=@PTR, unlocked-read,  @K_PID from @FN",
				seg_locks->disk->name, seg_locks->disk, seg_locks,
				current->pid, __builtin_return_address(0));
		}
	}
}

void nvmeibc_disk_volumes_get(struct nvmeibc_disk *disk, unsigned long *flags)
{
	__NFIND;
	if (flags) {
		spin_lock_irqsave(&disk->volume_spinlock, *flags);
	}
	else {
		spin_lock(&disk->volume_spinlock);
	}
	__NFOUTD;
}

void nvmeibc_disk_volumes_put(struct nvmeibc_disk *disk, unsigned long *flags)
{
	__NFIND;
	if (flags) {
		spin_unlock_irqrestore(&disk->volume_spinlock, *flags);
	}
	else {
		spin_unlock(&disk->volume_spinlock);
	}
	__NFOUTD;
}

int nvmeibc_disk_find_path(struct nvmeibc_disk *disk,
						   struct nvmeib_rdma_path_info *info)
{
	struct disk_globals *d;
	int rv = -1;
	NFIN;

	if (!disk || !info)
		goto out;

	d = __get_dg(disk);
	//BUG_ON(!disk-wq); /* All reader-writer locks use uninterruptible sleep */

	if (sm_th == d->eff_sm_th)
		down_read(&d->sm_th_update_rwsem);
	else {
		down_write(&d->sm_th_update_rwsem);
		if (sm_th != d->eff_sm_th) {
			d->eff_sm_th = sm_th;
			sema_init(&d->sm_th_cntsem, d->eff_sm_th);
		}
		downgrade_write(&d->sm_th_update_rwsem);
	}
	/* we've acquired readers-lock for sm-th updates */
	down(&d->sm_th_cntsem);
	_NT(trace_disk_nvmeibc_disk_find_path, "info=@INFO_PTR", info);
	rv = nvmeib_rdma_find_path(info);
	up(&d->sm_th_cntsem);
	up_read(&d->sm_th_update_rwsem);

out:
	NFOUT;
	return rv;
}

static int update_disk_lport(struct nvmeibc_disk *disk,
							 struct nvmeibc_ib_port *port);

static void free_disk_lnic(struct nvmeibc_local_nic *ln)
{
	struct nvmeibc_local_nic_port *lnp;

	while ((lnp = list_first_entry_or_null(&ln->ports, struct nvmeibc_local_nic_port, link))) {
		list_del(&lnp->link);
		kfree(lnp);
	}
}

static void remove_disk_lnic(struct nvmeibc_disk *disk, struct nvmeibc_dev *nic_dev)
{
	struct nvmeibc_local_nic *ln, *tmp_ln;
	list_for_each_entry_safe(ln, tmp_ln, &disk->local_nics, link) {
		if (ln->nic_dev == nic_dev) {
			_NT(trace_disk_remove_disk_lnic, "Removing nic @DEVICE_NAME from disk @DISK_NAME",
				nvmeibc_device_name(nic_dev), disk->name);
			free_disk_lnic(ln);
			list_del(&ln->link);
			kfree(ln);
			break;
		}
	}
}

/* This func is called on GID change event which moved port from
   'used' to 'unused' list */
static void remove_disk_lport(struct nvmeibc_disk *disk, struct nvmeibc_dev *nic_dev,
			      struct nvmeibc_ib_port *ib_port)
{
	struct nvmeibc_local_nic *ln;
	struct nvmeibc_local_nic_port *lnp;
	struct nvmeibc_admin_rnic *arnic;

	list_for_each_entry(ln, &disk->local_nics, link) {
		if (ln->nic_dev == nic_dev) {
			list_for_each_entry(lnp, &ln->ports, link) {
				if (lnp->ib_port == ib_port) {
					if (ln->cold_add) {
						/* nic was known during discovery */
						_NT(trace_disk_remove_disk_lport, "Disk @DISK_NAME, updating @DEVICE_NAME:@PORT",
						   disk->name, nvmeibc_device_name(nic_dev),
						   ib_port->port);
						update_disk_lport(disk, ib_port);
					}
					else {
						_NT(trace_1_disk_remove_disk_lport, "Disk @DISK_NAME, removing @DEVICE_NAME:@PORT",
						   disk->name, nvmeibc_device_name(nic_dev),
						   ib_port->port);
						list_del(&lnp->link);
						kfree(lnp);
						ln->n_ports--;
					}
					goto nics_end;
				}
			}
			_NW(warn_disk_remove_disk_lport, "Disk @DISK_NAME, no such lport @PORT in lnic @DEVICE_NAME (cold=@BOOL_YN), "
			   "unexpected", disk->name, ib_port->port,
			   nvmeibc_device_name(nic_dev), ln->cold_add);
			goto nics_end;
		}
	}
nics_end:

	/* Also clear port pointer from any local arnics */
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (arnic->local_port == ib_port) {
			_NT(trace_2_disk_remove_disk_lport, "Clearing arnic @RAW_IPV6 as local port", &arnic->ib_gid.raw);
			arnic->local = false;
			arnic->local_port = NULL;
		}
	}
}


/* This func is called on GID change event which moved port from
   'unused' to 'used' list && port's device was already 'used' */
static void add_disk_lport(struct nvmeibc_disk *disk, struct nvmeibc_dev *nic_dev,
			      struct nvmeibc_ib_port *ib_port)
{
	struct nvmeibc_local_nic *ln;
	struct nvmeibc_local_nic_port *lnp;
	bool found;

	list_for_each_entry(ln, &disk->local_nics, link) {
		if (ln->nic_dev == nic_dev) {
			found = false;
			list_for_each_entry(lnp, &ln->ports, link) {
				if (lnp->ib_port == ib_port) {
					found = true;
					break;
				}
			}
			if (ln->cold_add) {
				/* nic was known during discovery */
				if (found) {
					_NT(trace_disk_add_disk_lport, "Disk @DISK_NAME, updating @DEVICE_NAME:@PORT",
					   disk->name, nvmeibc_device_name(nic_dev),
					   ib_port->port);
					update_disk_lport(disk, ib_port);
				}
				else {
					_NW(warn_disk_add_disk_lport, "Disk @DISK_NAME, no such lport @PORT in lnic @DEVICE_NAME (cold=1), "
					   "unexpected", disk->name, ib_port->port,
					   nvmeibc_device_name(nic_dev));
				}
			}
			else {
				if (!found) {
					_NT(trace_1_disk_add_disk_lport, "Disk @DISK_NAME, adding @DEVICE_NAME:@PORT",
					   disk->name, nvmeibc_device_name(nic_dev),
					   ib_port->port);
					if ((lnp = kzalloc(sizeof(*lnp), GFP_KERNEL))) {
						lnp->ib_port = ib_port;
						list_add_tail(&lnp->link, &ln->ports);
						ln->n_ports++;
					}
					else
						_NE(error_disk_add_disk_lport, "OOM");
				}
				else {
					_NW(warn_1_disk_add_disk_lport, "Disk @DISK_NAME, lport @PORT already in lnic @DEVICE_NAME (cold=0), "
					   "unexpected", disk->name, ib_port->port,
					   nvmeibc_device_name(nic_dev));
				}
			}
			break;
		}
	}
}

static bool is_lport_nic_cold(struct nvmeibc_disk *disk,
							  struct nvmeibc_ib_port *ib_port)
{
	struct nvmeibc_local_nic *ln;
	struct nvmeibc_local_nic_port *lnp;
	bool is_cold = false;
	__NFIND;

	list_for_each_entry(ln, &disk->local_nics, link) {
		list_for_each_entry(lnp, &ln->ports, link) {
			if (lnp->ib_port == ib_port) {
				is_cold = ln->cold_add;
				goto out;
			}
		}
	}

out:
	__NFOUTD;
	return is_cold;
}

static bool net_uses_nic(struct nvmeibc_ib_net *net, struct nvmeibc_dev *nic_dev) {
	struct nvmeibc_ib_port *port;
	bool uses_nic = false;

	NFIN;

	list_for_each_entry(port, &nic_dev->port_list, port_list_n) {
		if (port == net->port) {
			/* Net of Admin Channel uses this NIC - Remove the volume */
			_ND(trace_disk_net_uses_nic, "Net @NET uses port num @PORT (pointer @PORT_PTR) of NIC @IB_DEV_NAME",
				net, port->port, port, nic_dev->dev->ib_dev->name);
			uses_nic = true;
			goto out;
		}
	}

out:
	NFOUT;

	return uses_nic;
}

static bool admin_ch_uses_nic(struct nvmeibc_admin_channel *ch, struct nvmeibc_dev *nic_dev) {
	struct nvmeibc_ib_admin_channel *iac;
	bool uses_nic = false;

	NFIN;

	if (!ch)
		goto out;

	iac = ac_to_iac(ch);
	uses_nic = net_uses_nic(&iac->net.base, nic_dev);

out:
	NFOUT;

	return uses_nic;
}

static bool disk_uses_nic(struct nvmeibc_disk *disk, struct nvmeibc_dev *nic_dev) {
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_ib_port *port = NULL;
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	struct nvmeibc_disk_segments_locks *disk_segs_locks = NULL;
	bool uses_nic = false;
	unsigned long flags;

	NFIN;

	_ND(trace_disk_disk_uses_nic, "Checking if disk @DISK_NAME (pointer @DISK) uses NIC @DEVICE_NAME", disk->name, disk, nvmeibc_device_name(nic_dev));
	if (disk->access_local) {
		/* Disk is local, check if its loopback admin channel is on this NIC */
		if (admin_ch_uses_nic(disk->local_admin_ch, nic_dev)) {
			_NI(trace_1_disk_disk_uses_nic, "Local disk @DISK_NAME uses nic @IB_DEV_NAME for its admin channel",
				disk->name, nic_dev->dev->ib_dev->name);
			uses_nic = true;
			goto out;
		}
	}
	else {
		/* Loop over disks's arnics */
		list_for_each_entry(arnic, &disk->arnics, link) {
			_ND(trace_2_disk_disk_uses_nic, "Checking if disk @DISK_NAME arnic @IB_GID_IPV6 (pointer @ARNIC) uses NIC @DEVICE_NAME",
			   disk->name, &arnic->ib_gid, arnic, nvmeibc_device_name(nic_dev));
			/* Arnic is not local, find out if a port of the NIC is used for its admin channel */
			if (admin_ch_uses_nic(arnic->channel, nic_dev)) {
				/* Net of Admin Channel uses this NIC */
				_NI(trace_3_disk_disk_uses_nic, "Disk @DISK_NAME uses nic @DEVICE_NAME to access arnic @IB_GID_IPV6",
					disk->name, nvmeibc_device_name(nic_dev), &arnic->ib_gid);
				uses_nic = true;
				goto out;
			}
		}
	}

	/* Check disk lock channel */
	disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
												  (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 });
	if (disk_segs_locks && disk_segs_locks->lock_ch) {
		if (net_uses_nic(&disk_segs_locks->lock_ch->net, nic_dev)) {
			nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });
			/* Net of disks lock channel uses this nic */
			_NI(trace_4_disk_disk_uses_nic, "Disk @DISK_NAME uses nic @DEVICE_NAME for its lock channel",
				disk->name, nvmeibc_device_name(nic_dev));
			uses_nic = true;
			goto out;
		}
	}
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });

	/* Loop over disk's rionics */
	spin_lock_irqsave(&disk->spinlock, flags);
	list_for_each_entry(rionic, &disk->rionics, disk_link) {
		/* Loop over rionics lionics */
		list_for_each_entry(lionic, &rionic->lionics, rionic_link) {
			list_for_each_entry(port, &nic_dev->port_list, port_list_n) {
				if (port == lionic->port) {
					/* Net of Admin Channel uses this NIC - Remove the volume */
					_NI(trace_5_disk_disk_uses_nic, "Disk @DISK_NAME uses port @PORT_PTR of nic @DEVICE_NAME to access an rionic",
						disk->name, port, nvmeibc_device_name(nic_dev));
					uses_nic = true;
					goto unlock;
				}
			}
		}
	}
unlock:
	spin_unlock_irqrestore(&disk->spinlock, flags);

out:
	NFOUT;
	return uses_nic;
}

static bool admin_ch_uses_port(struct nvmeibc_admin_channel *ch, struct nvmeibc_ib_port *port) {
	struct nvmeibc_ib_admin_channel *iac;
	bool uses_port = false;

	NFIN;

	if (!ch)
		goto out;

	iac = ac_to_iac(ch);
	uses_port = iac->net.base.port == port;

out:
	NFOUT;

	return uses_port;
}

static bool disk_uses_port(struct nvmeibc_disk *disk, struct nvmeibc_ib_port *port) {
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_disk_segments_locks *disk_segs_locks = NULL;
	struct nvmeibc_dev *nic_dev = port->nic_dev;
	bool uses_nic = false;
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	unsigned long flags;

	NFIN;

	_ND(trace_disk_disk_uses_port, "Checking if disk @DISK_NAME (pointer @DISK) uses NIC @DEVICE_NAME", disk->name, disk, nvmeibc_device_name(nic_dev));
	if (disk->access_local) {
		/* Disk is local, check if its loopback admin channel is on this NIC */
		if (admin_ch_uses_port(disk->local_admin_ch, port)) {
			_NI(trace_1_disk_disk_uses_port, "Local disk @DISK_NAME uses port @PORT of NIC @DEVICE_NAME for its admin channel",
				disk->name, port->port, nvmeibc_device_name(port->nic_dev));
			uses_nic = true;
			goto out;
		}
	}
	else {
		/* Loop over disks's arnics */
		list_for_each_entry(arnic, &disk->arnics, link) {
			_ND(trace_2_disk_disk_uses_port, "Checking if disk @DISK_NAME arnic @IB_GID_IPV6 (pointer @ARNIC) uses port @PORT of NIC @DEVICE_NAME",
			   disk->name, &arnic->ib_gid, arnic, port->port, nvmeibc_device_name(nic_dev));
			/* Arnic is not local, find out if a port of the NIC is used for its admin channel */
			if (admin_ch_uses_port(arnic->channel, port)) {
				/* Net of Admin Channel uses this NIC */
				_NI(trace_3_disk_disk_uses_port, "Disk @DISK_NAME uses port @PORT of NIC @DEVICE_NAME to access arnic @IB_GID_IPV6",
					disk->name, port->port, nvmeibc_device_name(nic_dev), &arnic->ib_gid);
				uses_nic = true;
				goto out;
			}
		}
	}

	/* Check disk lock channel */
	disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
												  (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0, .dont_wait = 0 });
	if (disk_segs_locks && disk_segs_locks->lock_ch) {
		if (port == disk_segs_locks->lock_ch->net.port) {
			nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });
			/* Net of disks lock channel uses this nic */
			_NI(trace_4_disk_disk_uses_port, "Disk @DISK_NAME uses nic @DEVICE_NAME for its lock channel",
				disk->name, nvmeibc_device_name(nic_dev));
			uses_nic = true;
			goto out;
		}
	}
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 0 });

	if (NVMEIB_UPDATE_NW_PATHS) {
		/* Loop over disk's rionics */
		spin_lock_irqsave(&disk->spinlock, flags);
		list_for_each_entry(rionic, &disk->rionics, disk_link) {
			/* Loop over rionics lionics */
			list_for_each_entry(lionic, &rionic->lionics, rionic_link) {
				if (port == lionic->port) {
					/* Net of Admin Channel uses this NIC - Remove the volume */
					_NI(trace_5_disk_disk_uses_port, "Disk @DISK_NAME uses port @PORT_PTR of nic @DEVICE_NAME to access an rionic",
						disk->name, port, nvmeibc_device_name(nic_dev));
					uses_nic = true;
					goto unlock;
				}
			}
		}
unlock:
		spin_unlock_irqrestore(&disk->spinlock, flags);
	}

out:
	NFOUT;
	return uses_nic;
}

static bool is_disk_local_nic(struct nvmeibc_disk *disk,
							  struct nvmeibc_dev *nic_dev)
{
	struct nvmeibc_local_nic *ln;
	bool found = false;

	__NFIND;
	list_for_each_entry(ln, &disk->local_nics, link) {
		if (ln->nic_dev == nic_dev) {
			found = true;
			break;
		}
	}

	__NFOUTD;
	return found;
}

const char *nvmeibc_disk_update_type_str(enum nvmeibc_disk_update_type update_type)
{
	switch (update_type) {
	case DISK_UPDATE_LOCAL_SRV:
		return "Local Server";
	case DISK_UPDATE_REMOVE_NIC:
		return "Remove NIC";
	case DISK_UPDATE_ADD_NIC:
		return "Add NIC";
	case DISK_UPDATE_REMOVE_PORT:
		return "Remove Port";
	case DISK_UPDATE_ADD_PORT:
		return "Add Port";
	case DISK_UPDATE_WRITE_STATUS:
		return "Write Status";
	case DISK_UPDATE_PORT_UPDATE:
		return "Port Update";
	case DISK_UPDATE_FILL_LOCK_CHANNELS:
		return "Fill Lock Channels";
	case DISK_UPDATE_RESET_LOCK_CHANNELS:
		return "Reset Lock Channels";
	default:
		return "Unknown";
	}
}

struct arg_qp_pcpu_cb {
	qp_pcpu_cb cb;
	void *arg;
};

static void qps_fill_buf_safe(struct nvmeib_qp_stats_pcpu __percpu *qp_stats, struct nvmeibc_ib_net *net, void *arg)
{
#define BUF_ADD(...)	*data->count += scnprintf(data->buf+*data->count, data->len-*data->count, __VA_ARGS__)

	struct write_status_buf_data *data = (struct write_status_buf_data *)arg;

	/* no newline */
	BUF_ADD("[%03d] %-*s: ", data->idx, QPS_STATS_PAD_BLANKS_LEN_NAME, net->ioch->name);
	if (nvmeib_get_state_guard(&net->state) == NVMEIBC_IB_NET_LIVE) {
		*data->count += nvmeib_qp_stats_fill(qp_stats, data->buf + *data->count, data->len - *data->count);
	}
	else {
		BUF_ADD("\n");
	}
	data->idx++;

#undef BUF_ADD
}

static void _nvmeib_qp_stats_reset_safe(struct nvmeib_qp_stats_pcpu __percpu *qp_stats, struct nvmeibc_ib_net *net, void *arg) {
	if (nvmeib_get_state_guard(&net->state) == NVMEIBC_IB_NET_LIVE) {
			nvmeib_qp_stats_reset(qp_stats);
	}
}

static int qps_arnic_stats_common(struct nvmeibc_admin_rnic *arnic, void *args, bool last)
{
	int rv = 0;
	struct arg_qp_pcpu_cb *qp_cb = (struct arg_qp_pcpu_cb *)args;

	if (arnic->channel) {
		struct nvmeibc_ib_admin_channel *ach = ac_to_iac(arnic->channel);
		qp_cb->cb(ach->net.base.qp_stats, &ach->net.base, qp_cb->arg);
	}

	return rv;
}

static int qps_nr_rionic_stats_common(struct nvmeibc_io_rnic *rionic, void *args, bool is_last)
{
	struct nvmeibc_io_lnic *lionic;
	int rv = 0, i;
	struct arg_qp_pcpu_cb *qp_cb = (struct arg_qp_pcpu_cb *)args;
	(void)is_last;

	list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
		if (!atomic_read(&lionic->dying)) {
			for (i = 0; i < lionic->n_nr_qps; i++) {
				struct nvmeibc_ib_nordda_channel *io_ch = (struct nvmeibc_ib_nordda_channel *)lionic->nr_channels + i;
				if (io_ch) {
					qp_cb->cb(io_ch->net.base.qp_stats, &io_ch->net.base, qp_cb->arg);
				}
			}
		}
	}

	return rv;
}

static int qps_io_rionic_stats_common(struct nvmeibc_io_rnic *rionic, void *args)
{
	int rv = 0;

	/* RDDA removed */
	(void)rionic;
	(void)args;

	return rv;
}
static int ioch_json_status_per_rionic(struct nvmeibc_io_rnic *rionic, void *args, bool is_last)
{
	struct write_status_buf_data *data = (struct write_status_buf_data *)args;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	char hw_gid[65] = {0};
	struct list_head *lionics = &rionic->lionics;
	struct nvmeibc_io_lnic *lionic;
	int i;
	*data->count += jops->start_obj(data->buf + *data->count, data->len - *data->count, NULL, 2);
	snprintf(hw_gid, 65, "%pI6", rionic->hw_gid.raw);
	*data->count += jops->data_str(data->buf + *data->count, data->len - *data->count, "hw_gid", hw_gid, !JSON_LAST_ELEM, 2);
	*data->count += jops->start_array(data->buf + *data->count, data->len - *data->count, "local_nics", 2);
	if (!list_empty(lionics)) {
		list_for_each_entry(lionic, lionics, rionic_link) {
			*data->count += jops->start_obj(data->buf + *data->count, data->len - *data->count, NULL, 3);
			snprintf(hw_gid, 65, "%pI6", lionic->port->gid.hw_gid.raw);
			*data->count += jops->data_str(data->buf + *data->count, data->len - *data->count, "hw_gid", hw_gid, !JSON_LAST_ELEM, 3);
			*data->count += jops->start_array(data->buf + *data->count, data->len - *data->count, "channels", 3);
			for (i = 0; i < lionic->n_nr_qps; i++) {
				struct nvmeibc_ib_nordda_channel *io_ch = (struct nvmeibc_ib_nordda_channel *)lionic->nr_channels + i;
				bool per_cpu = io_ch && is_pcpu_nrch(io_ch);
				*data->count += jops->start_obj(data->buf + *data->count, data->len - *data->count, NULL, 4);
				*data->count += jops->data_uval(data->buf + *data->count, data->len - *data->count, "index", i, !JSON_LAST_ELEM, 4);
				*data->count += jops->data_bool(data->buf + *data->count, data->len - *data->count, "in_use", io_ch != NULL && io_ch->inuse, !JSON_LAST_ELEM, 4);
				*data->count += jops->data_uval(data->buf + *data->count, data->len - *data->count, "cpu", io_ch? io_ch->cpu : 0, !JSON_LAST_ELEM, 4);
				*data->count += jops->data_bool(data->buf + *data->count, data->len - *data->count, "per_cpu", per_cpu, !JSON_LAST_ELEM, 4);
				*data->count += jops->data_uval(data->buf + *data->count, data->len - *data->count, "comp_cpu", per_cpu ? pcpu_nrch_cpu_get(io_ch) : 0, JSON_LAST_ELEM, 4);
				*data->count += jops->end_obj(data->buf + *data->count, data->len - *data->count, lionic->n_nr_qps - 1 == i, 4);
			}
			*data->count += jops->end_array(data->buf + *data->count, data->len - *data->count, JSON_LAST_ELEM, 3);
			*data->count += jops->end_obj(data->buf + *data->count, data->len - *data->count, lionic->rionic_link.next == lionics ? JSON_LAST_ELEM : !JSON_LAST_ELEM, 2);
		}
	}
	*data->count += jops->end_array(data->buf + *data->count, data->len - *data->count, JSON_LAST_ELEM, 2);
	*data->count += jops->end_obj(data->buf + *data->count, data->len - *data->count, is_last? JSON_LAST_ELEM : !JSON_LAST_ELEM, 2);

	return 0;
}

static void write_ioch_json_buf(struct write_status_buf_data *data)
{
	#define IOCH_JSON_VERSION 1
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;

	NFIN;
	*data->count += jops->start_obj(data->buf + *data->count, data->len - *data->count, NULL, 0);
	*data->count += jops->start_array(data->buf + *data->count, data->len - *data->count, "remote_nics", 1);
	call_for_each_nr_rionic(data->disk, ioch_json_status_per_rionic, data);
	*data->count += jops->end_array(data->buf + *data->count, data->len - *data->count, JSON_LAST_ELEM, 1);
	*data->count += nvmeib_proc_add_json_proc_epilog(IOCH_JSON_VERSION, data->buf + *data->count, data->len - *data->count);
	*data->count += jops->end_obj(data->buf + *data->count, data->len - *data->count, JSON_LAST_ELEM, 0);
	NFOUT;
}

static void __reset_coremask_stats_pcpu_fn(void *ctx)
{
	struct nvmeibc_disk_coremask_info *cinfo = ctx;
	struct nvmeibc_disk_coremask_pcpu_stats __percpu *this_cpu_stats = get_cpu_ptr(cinfo->pcpu_stats); 
	memset(this_cpu_stats, 0, sizeof(*this_cpu_stats));
	put_cpu_ptr(cinfo->pcpu_stats);
}

static void update_disk_config_work(struct workqe_struct *work_qe)
{
	struct disk_workq *disk_workqe = container_of(work_qe, struct disk_workq, work);
	struct nvmeibc_disk *disk = disk_workqe->disk;
	struct nvmeibc_disk_update_data *update_data = disk_workqe->work_data;
	struct admin_rgid_work *rgidw;
	int rv;

	__NFIND;
	_NT(trace_update_disk_config_work_start,
		"Disk @DISK_NAME update config - start ", disk->name);
	kfree(disk_workqe);

	if (update_data->update_type == DISK_UPDATE_LOCAL_SRV) {
		if (disk->access_local) {
			/* Disk is accessed local in which case we must release first */
			_NT(trace_disk_update_disk_config_work, "Calling release for @POSITION_STR disk @DISK_NAME to update local server",
			disk->access_local ? "local" : "paused", disk->name);
			nvmeibc_disk_release(disk, NVMEIBC_DISK_RELEASE_CONFIG_UPDATE_LOCAL_SRV);
		}

		/* Now update the local server */
		disk->local_server = update_data->update_data;

		_NT(trace_1_disk_update_disk_config_work, "Updated disk @DISK_NAME local server to @LOCAL_SERVER", disk->name, disk->local_server);

	} else if (update_data->update_type == DISK_UPDATE_REMOVE_NIC) {
		struct nvmeibc_dev *nic_dev = update_data->update_data;

		if (!atomic_read(&disk->paused) && disk_uses_nic(disk, nic_dev)) {
			/* Disk is not paused and uses this nic so we call release to remove it */
			_NT(trace_2_disk_update_disk_config_work, "Calling release for disk @DISK_NAME that uses nic @DEVICE_NAME",
			   disk->name, nvmeibc_device_name(nic_dev));
			nvmeibc_disk_release(disk, NVMEIBC_DISK_RELEASE_CONFIG_UPDATE_NIC_REMOVE);
		}

		/* Remove it from the local nic list */
		remove_disk_lnic(disk, nic_dev);

		_NT(trace_3_disk_update_disk_config_work, "Removed local nic @DEVICE_NAME from disk @DISK_NAME",
		   nvmeibc_device_name(nic_dev), disk->name);
	} else if (update_data->update_type == DISK_UPDATE_ADD_NIC) {
		struct nvmeibc_dev *nic_dev = update_data->update_data;
		if (nic_dev->device_used) {
			if (is_disk_local_nic(disk, nic_dev)) {
				_NT(trace_4_disk_update_disk_config_work, "nic @DEVICE_NAME already in disk @DISK_NAME",
				   nvmeibc_device_name(nic_dev), disk->name);
				//if this event is not due to hot-plug but due to
				//first port, of unsed device, becomeing used, we
				//will do the update on upcoming update-port event.
				//alternatively, loop over dev's ports and call
				//update_disk_lport for the its one 'used' port
			}
			else {
				/* Add nic to disk local nic list */
				_NT(trace_5_disk_update_disk_config_work, "Adding nic @DEVICE_NAME to disk @DISK_NAME", nvmeibc_device_name(nic_dev), disk->name);
				nvmeibc_add_nic_disk_local_nics(disk, nic_dev);
			}
		}
	} else if (update_data->update_type == DISK_UPDATE_REMOVE_PORT) {
		struct nvmeibc_ib_port *ib_port = update_data->update_data;

		if (!atomic_read(&disk->paused) && disk_uses_port(disk, ib_port)) {
			_NT(trace_6_disk_update_disk_config_work, "Port @PORT of NIC @IB_DEV_NAME used by disk @DISK_NAME is removed. Releasing disk",
			   ib_port->port, P2IB(ib_port)->name, disk->name);
			nvmeibc_disk_release(disk, NVMEIBC_DISK_RELEASE_CONFIG_UPDATE_PORT_REMOVE);
		}
		/* Remove/Update port from/of local nics list */
		remove_disk_lport(disk, ib_port->nic_dev, ib_port);
	} else if (update_data->update_type == DISK_UPDATE_ADD_PORT) {
		struct nvmeibc_ib_port *ib_port = update_data->update_data;

		_NT(trace_7_disk_update_disk_config_work, "Adding port @PORT of NIC @IB_DEV_NAME to disk @DISK_NAME",
			ib_port->port, P2IB(ib_port)->name, disk->name);
		/* Add/Update port to/of local nics list */
		add_disk_lport(disk, ib_port->nic_dev, ib_port);
	} else if (update_data->update_type == DISK_UPDATE_WRITE_STATUS) {
		struct write_status_buf_data *write_buf_data = update_data->update_data;
		if (write_buf_data->status_type == WRITE_STATUS_TEXT)
			write_status_buf(write_buf_data);
		else if (write_buf_data->status_type == WRITE_STATUS_JSON)
			write_status_json_buf(write_buf_data);
		else if (write_buf_data->status_type == WRITE_STATUS_NRCH)
			write_nrch_buf(write_buf_data);
		else if (write_buf_data->status_type == WRITE_STATUS_IOCH)
			write_ioch_buf(write_buf_data);
		else if (write_buf_data->status_type == WRITE_STATUS_QPS) {
			*write_buf_data->count += scnprintf(write_buf_data->buf+*write_buf_data->count,
				 write_buf_data->len-*write_buf_data->count, "*\n");
			common_qps_run_cb(disk, qps_fill_buf_safe, write_buf_data);
		} else if (write_buf_data->status_type == WRITE_STATUS_IOCH_JSON) {
			write_ioch_json_buf(write_buf_data);
		} else if (write_buf_data->status_type == WRITE_STATUS_COREMASK_JSON) {
			write_coremask_json_buf(write_buf_data);
		} else if (write_buf_data->status_type == WRITE_STATUS_COREMASK_STATS) {
			write_coremask_stats_json_buf(write_buf_data);
		}else
			_NE(error_disk_update_disk_config_work, "Invalid status type @STATUS_TYPE", write_buf_data->status_type);
	} else if (update_data->update_type == DISK_UPDATE_PORT_UPDATE) {
		struct nvmeibc_ib_port *ib_port = update_data->update_data;

		if (!ib_port->port_active && !atomic_read(&disk->paused) && disk_uses_port(disk, ib_port)) {
			_NT(trace_8_disk_update_disk_config_work, "Port @PORT of NIC @IB_DEV_NAME used by disk @DISK_NAME has become inactive. Releasing disk",
			   ib_port->port, P2IB(ib_port)->name, disk->name);
			nvmeibc_disk_release(disk, NVMEIBC_DISK_RELEASE_CONFIG_UPDATE_PORT_UPDATE);
		}
		else if (is_lport_nic_cold(disk, ib_port)) {
			_NT(trace_9_disk_update_disk_config_work, "Disk @DISK_NAME, updating @DEVICE_NAME:@PORT", disk->name,
			   nvmeibc_device_name(ib_port->nic_dev), ib_port->port);
			update_disk_lport(disk, ib_port);
		}

		/* Trace only - the code below will call rediscover if the disk is paused */
		_NT(trace_10_disk_update_disk_config_work, "Disk @DISK_NAME - device @IB_DEV_NAME port @PORT updated", disk->name,
		   P2IB(ib_port)->name, ib_port->port);
	} else if (update_data->update_type == DISK_UPDATE_JAM_ABND2FREE) {
		struct nvmeibc_ib_admin_channel *ch = get_alive_admin_ch(disk);
		struct abnd2free *a2f = update_data->update_data;

		if (ch && disk->jam_disk) {
			if (process_jmd_free_abandoned(disk, ch, a2f))
				_NE(error_1_disk_update_disk_config_work, "Fail to process abandon to free request");
		}
		kfree(a2f);

	}
	else if (update_data->update_type == DISK_UPDATE_REMOTE_GID) {
		rgidw = update_data->update_data;
		rgidw->disk = disk;
		rgidw->ch = get_alive_admin_ch(disk);
		if (rgidw->ch && !atomic_read(&disk->paused)) {
			WQ_INIT_WORK(&rgidw->work, nvmeibc_disk_handle_rgid_change_work);
			rv = nvmeibc_admin_channel_add_work(&rgidw->ch->base, &rgidw->work);
			if (rv)
				goto free_rgidw;
		}
		else {
free_rgidw:
			kfree(rgidw->r);
			kfree(rgidw);
		}
	}
	else if (update_data->update_type == DISK_UPDATE_DISCONNECT_IO_PATH) {
		struct disconnect_io_path_work *disconnect_io_path_work = container_of(
			update_data, struct disconnect_io_path_work, disk_update_data);
		DECLARE_COMPLETION_ONSTACK(comp);
		disconnect_io_path_work->comp = &comp;
		disconnect_io_path_work->ch = get_alive_admin_ch(disk);
		if (disconnect_io_path_work->ch && !atomic_read(&disk->paused)) {
			WQ_INIT_WORK(&disconnect_io_path_work->work, disconnect_io_path_work_fn);
			rv = nvmeibc_admin_channel_add_work(
				&disconnect_io_path_work->ch->base, &disconnect_io_path_work->work);
			if (!rv)
				wait_for_completion(disconnect_io_path_work->comp);
		}
	}
	else if (update_data->update_type == DISK_UPDATE_RESET_QP_STATS) {
		common_qps_run_cb(disk, _nvmeib_qp_stats_reset_safe, NULL);
	} else if (update_data->update_type == DISK_UPDATE_RESET_COREMASK_STATS) {
		if (disk->info && disk->info->coremask_info) {
			on_each_cpu(__reset_coremask_stats_pcpu_fn, disk->info->coremask_info, true);
		}
	} else if (update_data->update_type == DISK_UPDATE_FILL_LOCK_CHANNELS) {
		lock_channels_do_fill(update_data->update_data, disk);
	} else if (update_data->update_type == DISK_UPDATE_RESET_LOCK_CHANNELS) {
		lock_channels_reset(disk);
	} else if (update_data->update_type == DISK_UPDATE_COREMASK_UPDATE) {
		struct nvmeibc_disk_coremask_info *cinfo;
		if (disk->info && (cinfo = disk->info->coremask_info)) {
			struct nvmeibc_ib_admin_channel *ach = get_alive_admin_ch(disk);
			const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(disk);
			/* Schedule the coremask update on the main WQ */
			nvmeibc_add_work(nvmeibc_isnt_params_core2main(p), &cinfo->update_masks_main_work);
			/* Schedule the coremask connect channel code on the admin WQ */
			nvmeibc_admin_channel_add_work(&ach->base, &cinfo->update_masks_admin_work);
		}
	}
	else {
		_NE(error_2_disk_update_disk_config_work, "Unknown disk update type @UPDATE_TYPE_INT", update_data->update_type);
	}

	/* Signal the callback that we have done this update */
	if (update_data->done_cb)
		(*update_data->done_cb)(update_data->done_cb_ctx);

	if (atomic_dec_and_test(&disk->update_count)) {
		if (atomic_read(&disk->paused) && !disk->detached) {
			/* Disk has been paused to update config and is not detached
			* So we call release to perform rediscovery */
			_NT(trace_11_disk_update_disk_config_work, "Updates completed. Entering rediscovery loop...");
			disk->rediscover_timeout = 0;
			disk->rediscovery_now = 1;
			nvmeibc_disk_release(disk, NVMEIBC_DISK_RELEASE_CONFIG_UPDATE_COMPLETE);
		}
	}

	_NT(trace_update_disk_config_work_done,
		"Disk @DISK_NAME update config - done ",disk->name);

	__NFOUTD;
}

int nvmeibc_disk_update_config(struct nvmeibc_disk *disk,
							   struct nvmeibc_disk_update_data *update_data, bool in_interrupt)
{
	struct disk_workq *work_qe;
	int rv = 0;

	__NFIND;
	if (!(work_qe = kzalloc(sizeof(*work_qe),
							likely(!in_interrupt) ?
							GFP_KERNEL : GFP_ATOMIC))) {
		rv = -ENOMEM;
		goto out;
	}

	work_qe->disk = disk;
	work_qe->work_data = update_data;
	WQ_INIT_WORK(&work_qe->work, update_disk_config_work);

	atomic_inc(&disk->update_count);
	wake_up_interruptible(&disk->wait_queue);
	if ((rv = nvmeibc_disk_add_work(disk, &work_qe->work)) < 0) {
		_NT(trace_disk_nvmeibc_disk_update_config, "Fail to add work, disk @DISK_NAME", disk->name);
		kfree(work_qe);
	}

out:
	__NFOUTD;
	return rv;
}

ssize_t nvmeibc_disk_print_info(struct nvmeibc_disk *disk, char *buffer,
	int len)
{
	int count = 0;
	char lgid_buf[GUID_SIZE] = {0};
	char rgid_buf[GUID_SIZE] = {0};
	struct list_head *rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *lionics;
	struct nvmeibc_io_lnic *lionic;
	bool lgid_first = true, rgid_first = true;
	unsigned long flags;
	int tot_nr_qps = 0;

	count += scnprintf(buffer + count, len - count,"{\"name\":\"%s\",\n",
		disk->name);

	if (disk->info) {
		count += scnprintf(buffer + count, len - count,"\"Ldisk\":false,\n");
		count += scnprintf(buffer + count, len - count,"\"info\": {\n");
		count += scnprintf(buffer + count, len - count,
			"\"nsid\":%d,\n\"sector_shift\":%d,\n"
		   "\"tot_d_rscs\":%d,\n\"max_c_rscs\":%d,\n",
			disk->info->nsid, disk->info->sector_shift,
			disk->info->n_rscs, disk->info->max_client_rscs);
		count += scnprintf(buffer + count, len - count,"\"rionics\":[\n");

		spin_lock_irqsave(&disk->spinlock, flags);
		rionics = &disk->rionics;
		list_for_each_entry(rionic, rionics, disk_link) {
			if (!rgid_first)
				count += scnprintf(buffer + count, len - count,",");
			rgid_first = false;
			format_gid_raw(rionic->ib_gid.raw, rgid_buf);
			count += scnprintf(buffer + count, len - count,
				"{\"gid\":\"%s\",\n",rgid_buf);
			lionics = &rionic->lionics;
			count += scnprintf(buffer + count, len - count,"\"lionics\":[\n");
			list_for_each_entry(lionic, lionics, rionic_link) {
				if (!lgid_first)
					count += scnprintf(buffer + count, len - count,",");
				lgid_first = false;
				format_gid_raw(lionic->path.sgid.raw, lgid_buf);
				count += scnprintf(buffer + count, len - count,
				"{\"gid\":\"%s\",\n",lgid_buf);
				count += scnprintf(buffer + count, len - count,
		     "\"n_qps\":%d,\n", 0); /* RDDA removed */
				count += scnprintf(buffer + count, len - count,
					"\"n_nr_qps\":%d\n}\n", lionic->n_nr_qps);
				tot_nr_qps += lionic->n_nr_qps;
			}
			lgid_first = true;
			count += scnprintf(buffer + count, len - count,"]}\n");
		}
		spin_unlock_irqrestore(&disk->spinlock, flags);
		count += scnprintf(buffer + count, len - count,"],\n");

		count += scnprintf(buffer + count, len - count,
			"\"tot_nr_qps\":%d}\n", tot_nr_qps);
	}
	else
		count += scnprintf(buffer + count, len - count,"\"Ldisk\":true\n");
	count += scnprintf(buffer + count, len - count,"}\n");

	return count;
}

static void cancel_queued_get_ec_db_reqs(struct nvmeibc_disk *disk)
{
	unsigned long flags;
	struct get_dirty_bits_ec_info *info = 0;

	__NFIND;

	spin_lock_irqsave(&disk->db.dirty_bits_spinlock, flags);
	disk->db.dirty_bits_stopping = 1;

	if (!list_empty(&disk->db.dirty_bits_pending_reqs))
		_NT(trace_disk_cancel_queued_get_ec_db_reqs, "Failing all queued dirty-bit callbacks");
	while ((info = list_first_entry_or_null(&disk->db.dirty_bits_pending_reqs, struct get_dirty_bits_ec_info, link))) {
		if (info->comp) {
			info->comp->lock_status = NCL_STATUS_FAIL_COMP;
			info->comp->callback(info->comp, nvmeibc_d_rdma_comp_tag_make());
		}
		list_del(&info->link);
		kfree(info);
	}
	spin_unlock_irqrestore(&disk->db.dirty_bits_spinlock, flags);
	__NFOUTD;
}

static int jmdc_read_local(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_jmdc_read_comp *comp)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd = kzalloc(sizeof(*gen_cmd), GFP_ATOMIC);
	int rv = -EBUSY;

	if (atomic_read(&disk->paused))
		goto out;
	if (atomic_read(&disk->dying))
		goto out;

	if (!gen_cmd) {
		_NE(error_disk_jmdc_read_local, "Memory allocation error");
		rv = -ENOMEM;
		goto out;
	}

	if ((rv = nvmeibc_disk_fill_gen_op_get_jmdc(disk, comp, gen_cmd)) < 0)
		goto out;
	rv = execute_io_local_cmd_gen(disk, gen_cmd);

out:
	if (rv < 0) {
		nvmeib_buffer_free_sgl(&gen_cmd->param.jmdc_get.jmdc_sink.local);
		kfree(gen_cmd);
	}
	NFOUT;
	return rv;
}

int nvmeibc_disk_dbg_please_kill_yourself(struct nvmeibc_disk *disk,
                                          void (*cb)(void *), void *ctx,
                                          int rsc_id, u64 dlba) {
	struct nvmeibc_ib_admin_channel *ch;
	int rv = -1;
	__NFIND;

	if (disk->access_local) {
		_NT(error_1_nvmeibc_disk_dbg_please_kill_yourself, "Trying DBG admin cmd on disk @DISK_NAME which is local ...", disk->name);
		goto out;
	}
	if (!(ch = get_alive_admin_ch(disk))) {
		_NT(error_2_nvmeibc_disk_dbg_please_kill_yourself, "Disk @DISK_NAME, no main admin-ch ...", disk->name);
		goto out;
	}

	nvmeibc_ib_admin_channel_kill_remote_and_call(
	    &(struct nvmeibc_please_kill_yourself_args){
	        .ch = ch, .rsc_id = rsc_id, .dlba = dlba, .has_death_wish = true},
	    cb, ctx);

	rv = 0;

out:
	__NFOUTD;
	return rv;
}

/* This function is called from pausable layer
   i.e. disk-pasue was not acked by block layer */
int nvmeibc_disk_jmdc_read(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_jmdc_read_comp *comp)
{
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_ib_admin_jmdc_read_req_work *req;
	int rv = -1;
	__NFIND;

	if (!comp || !comp->callback) {
		_NT(trace_disk_nvmeibc_disk_jmdc_read, "Invalid inputs");
		goto out;
	}
	if (disk->access_local) {
		rv = jmdc_read_local(disk, comp);
		goto out;
	}
	if (!(ch = get_alive_admin_ch(disk))) {
		_NT(trace_1_disk_nvmeibc_disk_jmdc_read, "Disk @DISK_NAME, no main admin-ch ...", disk->name);
		goto out;
	}
	if (!(req = kzalloc(sizeof(*req), GFP_ATOMIC))) {
		_NT(trace_2_disk_nvmeibc_disk_jmdc_read, "Fail to alloc req");
		goto out;
	}

	/* Add to pending and if needed add work */
	req->comp = comp;
	req->ch = ch;
	_NT(trace_3_disk_nvmeibc_disk_jmdc_read, "Disk: @DISK_NAME (@DISK) - Adding work for admin ch @CH_PTR", disk->name, disk, req->ch);
	WQ_INIT_WORK(&req->work, nvmeibc_ib_admin_channel_jmdc_read_work);
	if ((rv = nvmeibc_admin_channel_add_work(&ch->base, &req->work)) < 0)
		goto out;

	rv = 0;

out:
	//nvmeibc_disk_unfreeze(disk);

	__NFOUTD;
	return rv;
}

struct lock_seg_info * nvmeibc_disk_get_lock_seg_info(
	struct nvmeibc_disk *disk, struct lock_seg_info *lsia, int seg_id)
{
	int i;
	struct lock_seg_info *lsi = NULL;

	__NFIND;
	for (i = 0; i < disk->info->n_disk_lock_segments; ++i)
		if (lsia[i].seg_id == seg_id) {
			_ND(trace_disk_nvmeibc_disk_get_lock_seg_info, "DB: found requested segment @SEG_PTR", &lsia[i]);
			lsi = &lsia[i];
			break;
		}
	__NFOUTD;
	return lsi;
}

int nvmeibc_disk_free_jrnl_ents(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	int rv = 0, i;
	struct free_jrnl_ents_info *info;
	struct nvmeib_data_buffer *src_ndb;
	struct scatterlist *sg;

	__NFIND;
	if (!(info = kzalloc(sizeof(*info), GFP_ATOMIC))) {
		_NE(error_1_disk_nvmeibc_disk_free_jrnl_ents, "Memory allocation error");
		rv = -ENOMEM;
		goto out;
	}

	if ((rv = nvmeibc_disk_fill_free_jrnl_ents_info(disk, comp, info)))
		goto free_mem;

	if (!disk->access_local) {
		if ((comp->ents_enc_ai.n * PAGE_SIZE) < comp->num_ents * sizeof(*comp->ents)) {
			_NE(error_disk_nvmeibc_disk_free_jrnl_ents, "Invalid size of entries array (@N_PAGES pages) for @NUM_ENTS entries",
			   comp->ents_enc_ai.n, comp->num_ents);
			rv = -ENOMEM;
			goto free_mem;
		}
		src_ndb = &info->gen_cmd->src_ndb;
		if ((rv = sg_alloc_table(&src_ndb->table, comp->ents_enc_ai.n, GFP_ATOMIC))) {
			_NE(error_2_disk_nvmeibc_disk_free_jrnl_ents, "sg_alloc_table failed (@RV) for @ENTRIES entries", rv, comp->ents_enc_ai.n);
			goto free_mem;
		}
		for_each_sg(src_ndb->table.sgl, sg, comp->ents_enc_ai.n, i) {
			sg_set_page(sg, comp->ents_enc_ai.pages[i], PAGE_SIZE, 0);
		}
		src_ndb->length = comp->ents_enc_ai.n << PAGE_SHIFT;
	}

	rv = nvmeibc_disk_execute_gen(disk, info->gen_cmd);
	if (rv > 0)
		rv = 0;

	goto out;

free_mem:
	kfree(info);

out:
	__NFOUTD;
	return rv;
}


struct nvmeibc_io_lnic *nvmeibc_disk_create_lionic(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_port *port, struct nvmeib_rdma_path_info *info)
{
	struct nvmeibc_io_lnic *lionic;
	int cpu;

	__NFIND;
	if (!(lionic = kzalloc(sizeof(*lionic), GFP_KERNEL)) ||
		!(lionic->last_io_ka_jif =
			nvmeib_public_alloc_percpu_cacheline(u64)) ||
		!(lionic->last_send_success_jif =
			nvmeib_public_alloc_percpu_cacheline(u64)) ||
		!(lionic->last_recv_success_jif =
			nvmeib_public_alloc_percpu_cacheline(u64)) ||
		!(lionic->dummy_md_read_ptr = (void *)__get_free_page(GFP_KERNEL)) ||
		!(lionic->dummy_md_write_ptr = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO)) ||
		ib_dma_mapping_error(P2IB(port), (lionic->dummy_md_read_addr =
			ib_dma_map_single(P2IB(port), lionic->dummy_md_read_ptr, PAGE_SIZE, DMA_FROM_DEVICE))) ||
		ib_dma_mapping_error(P2IB(port), (lionic->dummy_md_write_addr =
			ib_dma_map_single(P2IB(port), lionic->dummy_md_write_ptr, PAGE_SIZE, DMA_TO_DEVICE)))) {

		_NE(error_disk_nvmeibc_disk_create_lionic, "Fail to allocate io channels");
		goto err;
	}
	lionic->disk = disk;
	lionic->port = port;
	if (info->path) {
		lionic->path = *info->path;
		lionic->path_valid = true;
	}
	else {
		_NT(t0_nvmeibc_disk_create_lionic, "Unexpected - check source code");

		/* This seems unexpected as nvmeibc_ib_admin_channel_access_iornics()
		   calls try_add_rionic() with info->path always set to stack variable
		   @path even if find-path failed.

		   If this assumption is true:
		   1) The sgid and dgid of lionic->path are always valid (since discover)
		      and thus we can remove the the call to nvmeibc_disk_lionic_rionic-
			  find_path() in nvmeibc_ib_nordda_channel_connect() as the call to
		      nvmeib_rdma_start_path_connection() in nvmeibc_ib_net_alloc() will
			  always have/use correct sgid/dgid which are copied from lionic in
			  nvmeibc_ib_nordda_channel_connect()
		   2) We may set lionic->path_valid to true even if path was not found,
		      which is not so bad as the only case we ask about this flag is in
		      nvmeibc_ib_nordda_channel_connect. Also, if we will fail to connect
		      a channel on this path, use_lionic_path_for_ioch() will call
		      nvmeibc_disk_lionic_rionic_find_path() which will reset the lionic
		      path->valid flag
		 */
	}

	/* cache stuff from port */
	lionic->may_access = nvmeibc_ib_port_enabled(port);
	lionic->ib_gid = port->gid.gid;
	lionic->layer = port->layer;
	lionic->transport_type = port->transport_type;
	lionic->rdma_type = info->rdma_type;

	spin_lock_init(&lionic->spinlock);
	INIT_LIST_HEAD(&lionic->rionic_nrlink);
	for_each_possible_cpu(cpu) {
		*per_cpu_ptr(lionic->last_io_ka_jif, cpu) = 0;
		*per_cpu_ptr(lionic->last_send_success_jif, cpu) = 0;
		*per_cpu_ptr(lionic->last_recv_success_jif, cpu) = 0;
	}

	goto out;

err:
	if (lionic) {
		if (lionic->dummy_md_write_addr && !ib_dma_mapping_error(P2IB(port), lionic->dummy_md_write_addr))
			ib_dma_unmap_single(P2IB(port), lionic->dummy_md_write_addr, PAGE_SIZE, DMA_TO_DEVICE);
		if (lionic->dummy_md_read_addr && !ib_dma_mapping_error(P2IB(port), lionic->dummy_md_read_addr))
			ib_dma_unmap_single(P2IB(port), lionic->dummy_md_read_addr, PAGE_SIZE, DMA_FROM_DEVICE);
		if (lionic->dummy_md_write_ptr)
			free_page((unsigned long)lionic->dummy_md_write_ptr);
		if (lionic->dummy_md_read_ptr)
			free_page((unsigned long)lionic->dummy_md_read_ptr);
		nvmeib_public_free_percpu(lionic->last_io_ka_jif);
		nvmeib_public_free_percpu(lionic->last_send_success_jif);
		nvmeib_public_free_percpu(lionic->last_recv_success_jif);
		kfree(lionic);
		lionic = NULL;
	}

out:
	__NFOUTD;
	return lionic;
}

static int disconnect_lionic_all_rdda_channels(struct nvmeibc_io_lnic *lionic)
{
	int n = 0;
	NFIN;
	/* RDDA removed */
	NFOUT;
	return n;
}


static int disconnect_lionic_all_nordda_channels(struct nvmeibc_io_lnic *lionic)
{
	struct nvmeibc_ib_nordda_channel *nrch;
	int n = 0;
	int i;
	NFIN;

	for (i = 0; i < lionic->n_nr_qps; ++i) {
		nrch = lionic->nr_channels + i;
		if (nvmeibc_ib_nordda_channel_is_used(nrch)) {
			if (nvmeibc_channel_is_ll_pcpu_ch(&nrch->base)) {
				/* Per-cpu NRCH, call on correct CPU using IPI */
				struct nrch_try_disconnect_smp_fn_params params = {
					.ch = nrch,
				};
				BUG_ON(irqs_disabled() || in_interrupt());
				smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(&nrch->base), nrch_try_disconnect_smp_fn, &params, true);
			} else
				nvmeibc_ib_nordda_channel_try_disconnect(nrch);
			n++;
		}
	}

	NFOUT;
	return n;
}

/* This func runs on admin-wq either:
   1. after gid attr (may-access, ib-gid and layer)
      were updated in either rionic or lionic
   2. when connecting nrch that has invalid path */
int nvmeibc_disk_lionic_rionic_find_path(struct nvmeibc_io_lnic *lionic)
{
	struct nvmeibc_ib_port *port = lionic->port;
	struct ib_sa_path_rec *path = &lionic->path;
	struct nvmeibc_io_rnic *rionic = lionic->rionic;
	struct nvmeibc_disk *disk = rionic->disk;
	struct nvmeib_rdma_path_info info;
	enum rdma_link_layer layer = lionic->layer;
	enum rdma_transport_type transport_type = lionic->transport_type;
	int rv = -1;
	__NFIND;

	if (lionic->layer != rionic->layer) {
		_NT(trace_disk_nvmeibc_disk_lionic_rionic_find_path_layer_miss, "layers mismatch: l=@LAYER, r=@LAYER",
		   lionic->layer, rionic->layer);
		goto out;
	}
	else if (lionic->transport_type != rionic->transport_type) {
		_NT(trace_3_disk_nvmeibc_disk_lionic_rionic_find_path_trans_miss, "transport_type mismatch: l=@TRANSPORT_TYPE, r=@TRANSPORT_TYPE",
		   lionic->transport_type, rionic->transport_type);
		goto out;
	}
	else if (!rionic->may_access) {
		_NT(trace_1_disk_nvmeibc_disk_lionic_rionic_find_path, "No rionic access");
		goto out;
	}
	else if (!lionic->may_access) {
		_NT(trace_2_disk_nvmeibc_disk_lionic_rionic_find_path, "No lionic access");
		goto out;
	}

	memset(path, 0, sizeof(*path));
	/* use updated lgid and rgid info*/
	path->sgid = lionic->ib_gid;
	path->dgid = rionic->ib_gid;
	path->service_id = (layer == IB_LINK_LAYER_INFINIBAND) ?
		cpu_to_be64(NVMEIB_SERVICE_ID) : 0;
	/* use stuff from port we assume never changes */
	path->pkey = cpu_to_be16(port->pkey);
	info.dev = P2NV(port);
	info.sa = nvmeibc_sa_client(nvmeibc_cinst_get_core_p(disk));
	info.path = path;
	info.src_port = port->port;
	if (layer == IB_LINK_LAYER_INFINIBAND) {
		info.service_id = NVMEIB_SERVICE_ID;
		info.pkey = port->pkey;
		info.service_port = 0;
	}
	else {
		info.service_id = 0;
		info.pkey = 0;
		info.service_port = transport_type == RDMA_TRANSPORT_IWARP ? nvmeib_get_tcp_base_port_id() : NVMEIB_PORT_ID;
	}
	rv = nvmeibc_disk_find_path(rionic->disk, &info);
	_NT(trace_3_disk_nvmeibc_disk_lionic_rionic_find_path, "@TRUE_FALSE_STR path @SGID->@DGID",
	   !rv ? "Found" : "Fail to find", &path->sgid, &path->dgid);

out:
	lionic->path_valid = !rv;

	__NFOUTD;
	return rv;
}

static void handle_rgid_change_done(void *context)
{
	struct nvmeibc_disk_update_data *u = context;
	kfree(u);
}

int nvmeibc_disk_handle_rgid_change(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_admin_channel *ch, struct volume_server_req *req)
{
	struct volume_server_cmd_rgid_change_req *r;
	struct admin_rgid_work *w;
	struct nvmeibc_disk_update_data *u;
	int rv;

	__NFIND;
	if (!(u = kzalloc(sizeof(*u), GFP_KERNEL))) {
		_NE(error_disk_nvmeibc_disk_handle_rgid_change, "OOM: failed to allocate memory for disk_update_data");
		rv = -ENOMEM;
		goto out;
	}
	if (!(w = kzalloc(sizeof(*w), GFP_KERNEL))) {
		_NE(error_1_disk_nvmeibc_disk_handle_rgid_change, "OOM: failed to allocate memory for rgid_change_work");
		rv = -ENOMEM;
		goto freeu;
	}
	if (!(r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		_NE(error_2_disk_nvmeibc_disk_handle_rgid_change, "OOM: failed to allocate memory for rgid_change_req");
		rv = -ENOMEM;
		goto freew;
	}
	else
		w->r = r;
	memcpy(r, &req->r_req, sizeof(*r));
	u->update_type = DISK_UPDATE_REMOTE_GID;
	u->update_data = w;
	u->done_cb = handle_rgid_change_done;
	u->done_cb_ctx = u;
	
	if ((rv = nvmeibc_disk_update_config(disk, u, false)) < 0) {
		_NE(error_3_nvmeibc_disk_handle_rgid_change, "nvmeibc_disk_update_config failed rv=@RV", rv);
		goto freer;
	}

	goto out;

freer:
		kfree(r);

freew:
		kfree(w);

freeu:
	kfree(u);


out:
	__NFOUTD;
	return rv;
}

/* Handle remote gid-change event - relevant only for No-RDDA channels.
 *
 * In case actual update is required (wrt to our curr version), we either
 * o if rgid is inactive: disconnect all nrch using this rgid.
 * o if rgid is active  : find-path so next start-io-channels will use it.
 *
 * Note that find-path sleeps. Luckly, disk's spinlock is not required for
 * traversing the disk's rionic/lionic lists as we run on the admin-wq.
 * Running on admin-wq ensures that both start-io-channel (which rotate
 * these lists) and disk-discover/release (which empties these lists)
 * are not running.
 *
 * Also, note that the info we update here, is not used by nr-channels
 * after they were already connected.
 */
void nvmeibc_disk_handle_rgid_change_work(struct workqe_struct *work)
{
	struct admin_rgid_work *rgidw =
		container_of(work, struct admin_rgid_work, work);
	struct nvmeibc_disk *disk = rgidw->disk;
	struct nvmeibc_ib_admin_channel *ch = rgidw->ch;
	struct nvmeibc_admin_channel *ach = &ch->base;
	struct volume_server_cmd_rgid_change_req *r = rgidw->r;
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	bool found = false;
	int n = 0, m;

	__NFIND;

	/* Find rionic with this hw-gid */
	list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
		if (!memcmp(rionic->hw_gid.raw, r->hw_gid,
					sizeof(rionic->hw_gid.raw))) {
			found = true;
			break;
		}
	}
	if (!found) {
		_NT(trace_disk_nvmeibc_disk_handle_rgid_change_work, "Disk has no such hw-rgid @HW_GID", r->hw_gid);
		goto out;
	}
	if (rionic->ch != ach)
		_NT(trace_1_disk_nvmeibc_disk_handle_rgid_change_work, "rionic of sibling ach");

	/* Check if ant attr changed */
	if (rionic->may_access != r->may_access) {
		_NT(trace_2_disk_nvmeibc_disk_handle_rgid_change_work, "new access permissions, @MAY_ACCESS->@MAY_ACCESS",
		   rionic->may_access, r->may_access);
	}
	else if (memcmp(rionic->ib_gid.raw, r->gid,
					sizeof(rionic->ib_gid.raw))) {
		_NT(trace_3_disk_nvmeibc_disk_handle_rgid_change_work, "new configured gid, @RAW_IPV6->@GID_IPV6 ",
			rionic->ib_gid.raw, r->gid);
	}
	else if (rionic->layer != r->layer) {
		_NT(trace_4_disk_nvmeibc_disk_handle_rgid_change_work, "new link layer, @LAYER->@LAYER",
		   rionic->layer, r->layer);
	}
	else {
		_NT(trace_5_disk_nvmeibc_disk_handle_rgid_change_work, "Disk's hw-rgid=@RGID_IPV6 already: acc=@ACC, gid=@GID_IPV6, layer=@LAYER",
		   rionic->hw_gid.raw, r->may_access, r->gid, r->layer);
		goto out;
	}

	/* update all attrs */
	rionic->may_access = r->may_access;
	memcpy(rionic->ib_gid.raw, r->gid, sizeof(rionic->ib_gid.raw));
	rionic->layer = r->layer;

	/* use updated attr */
	if (!rionic->may_access) {
		list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
			m = disconnect_lionic_all_nordda_channels(lionic);
			_NT(trace_6_disk_nvmeibc_disk_handle_rgid_change_work, "Disconnected @N_NRCHS nrchs, path l=@HW_GID, r=@HW_GID",
			   m, &lionic->port->gid.hw_gid, &rionic->hw_gid);
			n += m;
			m = disconnect_lionic_all_rdda_channels(lionic);
			_NT(trace_7_disk_nvmeibc_disk_handle_rgid_change_work, "Disconnected @N_NRCHS iochs, path l=@HW_GID, r=@HW_GID",
				m, &lionic->port->gid.hw_gid, &rionic->hw_gid);
			n += m;
		}
	}
	else {
		list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
			_NT(trace_8_disk_nvmeibc_disk_handle_rgid_change_work, "[@N_NRCHS] updating nordda path l=@HW_GID, r=@HW_GID",
			   n, &lionic->port->gid.hw_gid, &rionic->hw_gid);
			nvmeibc_disk_lionic_rionic_find_path(lionic);
			n++;
		}
	}
	_NT(trace_9_disk_nvmeibc_disk_handle_rgid_change_work, "Updated @N_NRCHS nordda paths to remote hw-gid=@GID_IPV6",
	   n, rionic->hw_gid.raw);

	//Optional: trigger start-io-nordda-channels.

out:
	kfree(rgidw->r);
	kfree(rgidw);

	__NFOUTD;
}

static void update_disk_lport_work(struct workqe_struct *work)
{
	struct update_lport_workq *w =
		container_of(work, struct update_lport_workq, work);
	struct nvmeibc_ib_admin_channel *ach = w->ch;
	struct nvmeibc_disk *disk = ach->base.base.disk;
	struct nvmeibc_ib_port *port = w->port;
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	bool found;
	int n = 0, m;
	__NFIND;

	list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
		found = false;
		list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
			if (lionic->port == port) {
				found = true;
				break;
			}
		}
		if (found) {
			if (rionic->ch != &ach->base)
				_NT(trace_disk_update_disk_lport_work, "rionic of sibling ach");

			/* update all attrs */
			lionic->may_access = nvmeibc_ib_port_enabled(port);
			lionic->ib_gid = port->gid.gid;
			lionic->layer = port->layer;
			lionic->transport_type = port->transport_type;

			/* use updated attr */
			if (!lionic->may_access) {
				m = disconnect_lionic_all_nordda_channels(lionic);
				_NT(trace_1_disk_update_disk_lport_work, "Disconnected @N_NRCHS nrchs, path l=@HW_GID, r=@HW_GID",
				   m, &lionic->port->gid.hw_gid, &rionic->hw_gid);
				n += m;
				m = disconnect_lionic_all_rdda_channels(lionic);
				_NT(trace_2_disk_update_disk_lport_work, "Disconnected @N_NRCHS iochs, path l=@HW_GID, r=@HW_GID",
					m, &lionic->port->gid.hw_gid, &rionic->hw_gid);
				n += m;
			}
			else {
				_NT(trace_3_disk_update_disk_lport_work, "[@N_NRCHS] updating nordda path l=@HW_GID, r=@HW_GID, layer=@LAYER",
					n, &lionic->port->gid.hw_gid,
					&rionic->hw_gid, lionic->layer);
				nvmeibc_disk_lionic_rionic_find_path(lionic);
				n++;
			}

		}
	}
	_NT(trace_4_disk_update_disk_lport_work, "Updated @N_NRCHS nordda paths from local hw-gid=@GID_IPV6",
	   n, &port->gid.hw_gid);

	w->rv_n = n;
	if (w->done) {
		complete(w->done);
	}

	__NFOUTD;
}

/* Based on one the disk's nr-lionics that uses @port,
   decide if all lionics shall be updated or not */
static int is_nr_lionic_up2date(struct nvmeibc_disk *disk,
								struct nvmeibc_ib_port *port)
{
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	unsigned long flags;
	bool found = false;
	bool rv = false;
	__NFIND;

	spin_lock_irqsave(&disk->spinlock, flags);
	list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
		list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
			if (lionic->port == port) {
				found = true;
				goto unlock;
			}
		}
	}
unlock:
	spin_unlock_irqrestore(&disk->spinlock, flags);

	if (found) {
		/* if any attr changed, all attrs will be updated */
		rv = true;
		if (lionic->may_access != nvmeibc_ib_port_enabled(port)) {
			_NT(trace_disk_is_nr_lionic_up2date, "new access permissions, @MAY_ACCESS->@BOOL",
			    lionic->may_access, nvmeibc_ib_port_enabled(port));
		}
		else if (memcmp(&lionic->ib_gid, &port->gid.gid,
						sizeof(lionic->ib_gid))) {
			_NT(trace_1_disk_is_nr_lionic_up2date, "new configured gid, @IB_GID_IPV6->@GID_IPV6 ",
			   &lionic->ib_gid, &port->gid.gid);
		}
		else if (lionic->layer != port->layer)  {
			_NT(trace_2_disk_is_nr_lionic_up2date, "new link layer, @LAYER->@LAYER",
			   lionic->layer, port->layer);
		}
		else {
			_NT(trace_3_disk_is_nr_lionic_up2date, "Disk's hw-lgid=@LGID_IPV6 already: acc=@ACC, gid=@GID_IPV6, layer=@LAYER",
			   rionic->hw_gid.raw, port->port_active, &port->gid.gid, port->layer);
			rv = false;
		}
	}
	else {
		_NT(trace_4_disk_is_nr_lionic_up2date, "Disk has no such port, hw-lgid @GID_IPV6", &port->gid.gid);
	}

	__NFOUTD;
	return rv;
}

/* Handle local gid-change event - relevant only for No-RDDA channels.
 *
 * In case event is cannot be processed (dying/paused/no-admin), it will be
 * processed implicitly on next rediscovery as by * product of the initial
 * find-path when trying to access rionics.
 *
 *
 * On success returns the number of updated NO-RRDA paths @disk has from @port,
 * -1 otherwise.
 */
static int update_disk_lport(struct nvmeibc_disk *disk,
							 struct nvmeibc_ib_port *port)
{
	struct nvmeibc_ib_admin_channel *ach;
	DECLARE_COMPLETION_ONSTACK(done);
	struct update_lport_workq w;
	int d = 0, p = 0, e = 0;
	int rv = -1;
	__NFIND;

	if ((d = atomic_read(&disk->dying)) ||
		(p = atomic_read(&disk->paused)) ||
		(e = list_empty(&disk->nr_rionics))) {
		_NT(trace_disk_update_disk_lport, "Disk @DISK_NAME is dying, paused or empty-nrrionics "
		   "(@DYING,@PAUSING,@LIST_EMPTY)", disk->name, d, p, e);
		goto out;
	}
	if (!(ach = get_alive_admin_ch(disk))) {
		_NT(trace_1_disk_update_disk_lport, "Disk @DISK_NAME, no main admin-ch", disk->name);
		goto out;
	}

	if (!is_nr_lionic_up2date(disk, port))
		goto out;

	WQ_INIT_WORK(&w.work, update_disk_lport_work);
	w.ch = ach;
	w.port = port;
	w.done = &done;
	w.rv_n = 0;
	rv = nvmeibc_admin_channel_add_work(&ach->base, &w.work);
	if (rv) {
		_NE(error_disk_update_disk_lport, "Failed to add work");
		goto out;
	}

	wait_for_completion(&done);
	rv = w.rv_n;

	//Optional: trigger start-io-nordda-channels.

out:
	__NFOUTD;
	return rv;
}

/* Runs on admin ch work-queue */
static void disconnect_io_path_work_fn(struct workqe_struct *work)
{
	struct disconnect_io_path_work *disconnect_io_path_work = container_of(
			work, struct disconnect_io_path_work, work);
	struct nvmeibc_disk *disk = disconnect_io_path_work->disk;
	union ib_gid *lgid = &disconnect_io_path_work->lgid;
	union ib_gid *rgid = &disconnect_io_path_work->rgid;
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	int n = 0, m = 0, rv;
	__NFIND;

	list_for_each_entry(rionic, &disk->rionics, disk_link) {
		if (memcmp(rgid, &rionic->ib_gid, sizeof(*rgid)) != 0)
			continue;
		list_for_each_entry(lionic, &rionic->lionics, rionic_link) {
			if (memcmp(lgid, &lionic->ib_gid, sizeof(*lgid)) != 0)
				continue;
			_NT(trace_disk_disconnect_io_path_work_fn, "Disconnecting all iochs on lionic @LIONIC - IO Path (@IB_GID_IPV6 -> @IB_GID_IPV6)",
				lionic, &lionic->ib_gid, &rionic->ib_gid);
			rv = disconnect_lionic_all_rdda_channels(lionic);
			_NT(trace_1_disk_disconnect_io_path_work_fn, "Disconnected @RV iochs, path l=@IB_GID_IPV6, r=@IB_GID_IPV6",
				rv, &lionic->ib_gid, &rionic->ib_gid);
			m += rv;
			n++;
			break;
		}
	}
	list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
		if (memcmp(rgid, &rionic->ib_gid, sizeof(*rgid)) != 0)
			continue;
		list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
			if (memcmp(lgid, &lionic->ib_gid, sizeof(*lgid)) != 0)
				continue;
			_NT(trace_2_disk_disconnect_io_path_work_fn, "Disconnecting all nrchs on lionic @LIONIC - IO Path (@IB_GID_IPV6 -> @IB_GID_IPV6)",
				lionic, &lionic->ib_gid, &rionic->ib_gid);
			rv = disconnect_lionic_all_nordda_channels(lionic);
			_NT(trace_3_disk_disconnect_io_path_work_fn, "Disconnected @RV nrchs, path l=@IB_GID_IPV6, r=@IB_GID_IPV6",
				rv, &lionic->ib_gid, &rionic->ib_gid);
			m += rv;
			n++;
			break;
		}
	}
	_NT(trace_4_disk_disconnect_io_path_work_fn, "Disconnected @COUNT IO Channels on @COUNT lionics", m, n);

	complete(disconnect_io_path_work->comp);
	__NFOUTD;
}

static void free_disk_disconnect_io_path_data(void *ctx)
{
	struct disconnect_io_path_work *disconnect_io_path_work = ctx;
	kfree(disconnect_io_path_work);
}

int nvmeibc_disk_disconnect_io_path(
	struct nvmeibc_disk *disk, union ib_gid *lgid, union ib_gid *rgid)
{
	int rv = 0;
	struct disconnect_io_path_work *disconnect_io_path_work;
	struct nvmeibc_disk_update_data *disk_update_data;
	if (atomic_read(&disk->dying)) {
		_NT(trace_disk_nvmeibc_disk_disconnect_io_path, "Disk @DISK_NAME is dying", disk->name);
		rv = -EBUSY;
		goto out;
	}
	if (!(disconnect_io_path_work = kzalloc(
		sizeof(*disconnect_io_path_work), GFP_ATOMIC))) {
		_NT(error_disk_nvmeibc_disk_disconnect_io_path, "Memory Allocation Error");
		rv = -ENOMEM;
		goto out;
	}
	disconnect_io_path_work->disk = disk;
	disconnect_io_path_work->lgid = *lgid;
	disconnect_io_path_work->rgid = *rgid;
	disk_update_data = &disconnect_io_path_work->disk_update_data;
	disk_update_data->update_type = DISK_UPDATE_DISCONNECT_IO_PATH;
	disk_update_data->done_cb = free_disk_disconnect_io_path_data;
	disk_update_data->done_cb_ctx = disconnect_io_path_work;
	if ((rv = nvmeibc_disk_update_config(disk, disk_update_data, true)) < 0) {
		_NT(trace_1_disk_nvmeibc_disk_disconnect_io_path, "nvmeibc_disk_update_config failed (@RV)", rv);
		kfree(disconnect_io_path_work);
	}

out:
	return rv;
}

#define PAD_BLANKS_LEN (16)

static int nvmeibc_disk_net_intrs_stats_fill(struct nvmeibc_disk *disk,
											 char* buf, int len)
{
	int cnt = 0;
	struct nvmeibc_disk_percpu_intr_stats __percpu *pcpu_intr_stats = disk->pcpu_intr_stats;
	struct nvmeibc_disk_percpu_intr_stats __percpu *pcpu;
	int i;

	#define BUF_ADD(...) ({ \
	cnt += scnprintf(buf+cnt, len-cnt, __VA_ARGS__); /*_T(__VA_ARGS__); */ })

	BUF_ADD("%-*s| %*s | %*s | %*s\n",
		20, "*",
		PAD_BLANKS_LEN, "TOTAL",
		PAD_BLANKS_LEN, "SEND",
		PAD_BLANKS_LEN, "RECV");

	for_each_online_cpu(i) {
		pcpu = per_cpu_ptr(pcpu_intr_stats, i);
		BUF_ADD("%%CPU%03d%*s| %*llu | %*llu | %*llu\n",
				i, 13, "",
				PAD_BLANKS_LEN, pcpu->total_intr,
				PAD_BLANKS_LEN, pcpu->total_send_intr,
				PAD_BLANKS_LEN, pcpu->total_recv_intr);
	}

	__NFOUTD;
	return cnt;

	#undef BUF_ADD
}

#if defined(NVMEIBC_DISK_CMDS_STATS) && (NVMEIBC_DISK_CMDS_STATS==1)

#define SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, _stat_name_) 		\
do {																		\
	_sum_->_cmd_type_._stat_name_ +=										\
	_pcp_->_cmd_type_._stat_name_ ;											\
} while(0)

#define SUM_PCP_CMD_TYPE_STATS(_sum_, _pcp_, _cmd_type_) 						\
do {																			\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_ulp_submissions);			\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_ulp_completions);			\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_direct_tot);				\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_pending_tot);				\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_pending_now);				\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_executing);				\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_llp_completions);			\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_err_internal_retry);		\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_err_direct_exec);			\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_err_pending_exec);		\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_err_pending_timeout);		\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_err_pending_aborted);		\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_llp_poison_check_hits);	\
	SUM_PCP_CMD_TYPE_STAT(_sum_, _pcp_, _cmd_type_, n_llp_poison_check_misses);	\
} while (0)

typedef struct nvmeibc_disk_percpu_cmds_stats nvmeibc_disk_cmds_stats_sum_t;
static void nvmeibc_disk_cmds_stats_get(struct nvmeibc_disk *disk,
										nvmeibc_disk_cmds_stats_sum_t *sum)
{
	struct nvmeibc_disk_percpu_cmds_stats *pcp;
	int cpu;
	__NFIND;

	memset(sum, 0, sizeof(*sum));

	for_each_online_cpu(cpu) {
		pcp = per_cpu_ptr(disk->pcpu_cmds_stats, cpu);
		SUM_PCP_CMD_TYPE_STATS(sum, pcp, io);
		SUM_PCP_CMD_TYPE_STATS(sum, pcp, gen);
		SUM_PCP_CMD_TYPE_STATS(sum, pcp, lock);
		SUM_PCP_CMD_TYPE_STATS(sum, pcp, lock_hw_owner);
		SUM_PCP_CMD_TYPE_STATS(sum, pcp, lock_hw_active);
		SUM_PCP_CMD_TYPE_STATS(sum, pcp, lock_hw_wbinfo);
		//_NI(nvmeibc_disk_percpu_cmds_stats_i1,
		//	 "cpu @INT: IO ulp: @INT_ULLONG-->@INT_ULLONG",
		//   cpu, pcp->io.n_ulp_submissions,
		//   pcp->io.n_ulp_completions);
	}

	__NFOUTD;
}

/* called for each discover(). We call it as close as possible to the discover procedure to postpone
 * the reset, and allow the valid values of statistics, in case the disk is "stuck" for any reason
 */
static void nvmeibc_disk_cmds_stats_reset(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_percpu_cmds_stats *pcp;
	int cpu;
	__NFIND;

	for_each_online_cpu(cpu) {
		pcp = per_cpu_ptr(disk->pcpu_cmds_stats, cpu);
		memset(pcp, 0, sizeof(*pcp));
	}

	__NFOUTD;
}

static int nvmeibc_disk_cmds_stats_dump(struct nvmeibc_disk *disk,
										nvmeibc_disk_cmds_stats_sum_t *sum,
										char* buf, int len)
{
	int cnt = 0;
	#define BUF_ADD(...) ({ \
	cnt += scnprintf(buf+cnt, len-cnt, __VA_ARGS__); /*_T(__VA_ARGS__); */ })

	#define LINE_ADD(_sum_, _str_, _stat_) 				\
			BUF_ADD("%-*s| %*llu | %*llu | %*llu | %*llu | %*llu | %*llu | %*llu\n", \
				20, _str_,								\
				PAD_BLANKS_LEN, (u64)_sum_->io._stat_,		\
				PAD_BLANKS_LEN, (u64)_sum_->gen._stat_,		\
				PAD_BLANKS_LEN, (u64)_sum_->lock._stat_,	\
				PAD_BLANKS_LEN, (u64)_sum_->lock_hw_owner._stat_,	\
				PAD_BLANKS_LEN, (u64)_sum_->lock_hw_active._stat_,	\
				PAD_BLANKS_LEN, (u64)_sum_->lock_hw_wbinfo._stat_,	\
				PAD_BLANKS_LEN, (u64)_sum_->lock_hw_read._stat_)	\

	__NFIND;
	BUF_ADD("PER CMD STATS\n");

	BUF_ADD("%-*s| %*s | %*s | %*s | %*s | %*s | %*s | %*s\n",
		20, "*",
		PAD_BLANKS_LEN, "I/O",
		PAD_BLANKS_LEN, "GEN",
		PAD_BLANKS_LEN, "SW-LOCK",
		PAD_BLANKS_LEN, "HW-LOCK-OWNER",
		PAD_BLANKS_LEN, "HW-LOCK-ACTIVE",
		PAD_BLANKS_LEN, "HW-LOCK-WBINFO",
		PAD_BLANKS_LEN, "HW-LOCK-READ");

	LINE_ADD(sum, "ulp submissions", 		n_ulp_submissions);
	LINE_ADD(sum, "ulp completions", 		n_ulp_completions);
	LINE_ADD(sum, "llp completions", 		n_llp_completions);
	LINE_ADD(sum, "now execute", 			n_executing);
	LINE_ADD(sum, "now pending", 			n_pending_now);
	LINE_ADD(sum, "tot direct  exec", 		n_direct_tot);
	LINE_ADD(sum, "tot pending exec", 		n_pending_tot);
	LINE_ADD(sum, "err internal retry", 	n_err_direct_exec);
	LINE_ADD(sum, "err direct  exec", 		n_err_direct_exec);
	LINE_ADD(sum, "err pending exec", 		n_err_pending_exec);
	LINE_ADD(sum, "err pending timeout",	n_err_pending_timeout);
	LINE_ADD(sum, "err pending aborted", 	n_err_pending_aborted);

	if (sum->io.n_llp_poison_check_hits || sum->io.n_llp_poison_check_misses) {
		/* These stats will appear only in case of DBGDI, which is per volume
		 * and not per disk. We do not want them if not in DBGDI. Hence, on disk
		 * level, just check if we have any recorded.
		 */
		BUF_ADD("%-*s| %*llu\n", 20, "poison check hits", PAD_BLANKS_LEN,
		        (u64)sum->io.n_llp_poison_check_hits);
		BUF_ADD("%-*s| %*llu\n", 20, "poison check misses", PAD_BLANKS_LEN,
		        (u64)sum->io.n_llp_poison_check_misses);
	}

	__NFOUTD;
	return cnt;

	#undef LINE_ADD
	#undef BUF_ADD
}

static int nvmeibc_disk_cmds_stats_fill(struct nvmeibc_disk *disk,
										char* buf, int len)
{
	nvmeibc_disk_cmds_stats_sum_t *sum = NULL;
	int cnt;
	__NFIND;

	if (!(sum = kzalloc(sizeof(*sum), GFP_KERNEL))) {
		cnt = scnprintf(buf, len, "OOM\n");
		goto out;
	}

	nvmeibc_disk_cmds_stats_get(disk, sum);
	cnt = nvmeibc_disk_cmds_stats_dump(disk, sum, buf, len);

out:
	kfree(sum);
	__NFOUTD;
	return cnt;
}
#else
static void nvmeibc_disk_cmds_stats_reset(struct nvmeibc_disk *disk)
{
	(void)disk;
}
static int nvmeibc_disk_cmds_stats_fill(struct nvmeibc_disk *disk,
										char* buf, int len)
{
	int cnt;
	(void)disk;

	cnt = scnprintf(buf, len, "N/A\n");

	return cnt;
}
#endif

bool nvmeibc_disk_use_arnic_for_disk(struct nvmeibc_admin_rnic *arnic,
	struct nvmeibc_disk *disk, int attrib_mask)
{
	if (arnic->local && !disk->is_local) {
		_NT(nvmeibc_disk_use_arnic_for_disk_t990,
			"Skip arnic @SGID is local and disk @SGID is not",
			&arnic->ib_gid, disk->name);
		return false;
	}
	if (!arnic->local && disk->is_local) {
		_NT(nvmeibc_disk_use_arnic_for_disk_t991,
			"Skip arnic @SGID is not local and disk @STR is local",
			&arnic->ib_gid, disk->name);
		return false;
	}
	if ((attrib_mask & NVMEIBC_ARNIC_ALIVE) && !arnic->alive) {
		_NT(nvmeibc_disk_use_arnic_for_disk_t992,
			"Skip arnic @SGID not alive", &arnic->ib_gid);
		return false;
	}
	if ((attrib_mask & (NVMEIBC_ARNIC_HAS_CH | NVMEIBC_ARNIC_HAS_MAIN_CH |
						NVMEIBC_ARNIC_HAS_RIONICS))
		&& !arnic->channel) {
		_NT(nvmeibc_disk_use_arnic_for_disk_t993,
			"Skip arnic @SGID has no channel", &arnic->ib_gid);
		return false;
	}
	if ((attrib_mask & NVMEIBC_ARNIC_HAS_MAIN_CH) && !arnic->channel->is_main) {
		_NT(nvmeibc_disk_use_arnic_for_disk_t994,
			"Skip arnic @SGID is not main channel", &arnic->ib_gid);
		return false;
	}
	if ((attrib_mask & NVMEIBC_ARNIC_HAS_RIONICS) && arnic->channel->n_rionics_used == 0) {
		_NT(nvmeibc_disk_use_arnic_for_disk_t995,
			"Skip arnic @SGID is not main channel", &arnic->ib_gid);
		return false;
	}

	return true;
}

void nvmeibc_disk_add_stats(struct nvmeibc_disk *disk, 
			struct nvmeibc_dev *local_dev,
			struct nvmeib_io_stats *stats,
			struct nvmeibc_block_io_req *req, u64 io_exec,
			bool is_recovery)
{
	u64 size = req->op == NVMEIB_BLOCK_IO_OP_DISCARD ? (le32_to_cpu(req->trim->nlb) << disk->sector_shift): req->ndb->length;
	_ND(nvmeibc_disk_add_stats_d1, "@STR op=@INT len=@UINT io_exec=@UINT",
		disk->name, req->op, req->ndb->length, io_exec);
	
	if (req->op == NVMEIB_BLOCK_IO_OP_READ || 
		req->op == NVMEIB_BLOCK_IO_OP_WRITE ||
		req->op == NVMEIB_BLOCK_IO_OP_DISCARD)
	{
		nvmeib_io_stats_adjust_and_update(disk->stats, stats,  io_op_to_verb(req->op, is_recovery), size, io_exec, !!req->do_512b_sub_block_x);
		if (local_dev) {
			nvmeib_io_stats_adjust_and_update(local_dev->stats, NULL, io_op_to_verb(req->op, is_recovery), size, io_exec, !!req->do_512b_sub_block_x);
		}
	}
}

#define BUF_ADD(...) ({ \
	cnt += scnprintf(buf+cnt, len-cnt, __VA_ARGS__); /*_T(__VA_ARGS__); */ })

static int nvmeibc_disk_gen_cmds_stats_fill(struct nvmeibc_disk *disk,
										char* buf, int len)
{
	int cnt = 0, i;

	BUF_ADD("\nGEN CMD STATS\n");

	BUF_ADD("%-*s| %*s | %*s | %*s | %*s\n",
		20, "*",
		PAD_BLANKS_LEN, "Succeeded",
		PAD_BLANKS_LEN, "Failed",
		PAD_BLANKS_LEN, "Local",
		PAD_BLANKS_LEN, "Remote");


	for (i = NVMEIB_GEN_OP_GET_UUID_JOUR; i < NVMEIB_GEN_OP_MAX; i++) {
		BUF_ADD("%-*s| %*llu | %*llu | %*llu | %*llu\n",
			20, nvmeib_gen_op_str(i),
			PAD_BLANKS_LEN, (u64)atomic64_read(&disk->gen_cmds_cntrs_ok[i]),
			PAD_BLANKS_LEN, (u64)atomic64_read(&disk->gen_cmds_cntrs_fail[i]),
			PAD_BLANKS_LEN, (u64)atomic64_read(&disk->gen_cmds_cntrs_local[i]),
			PAD_BLANKS_LEN, (u64)atomic64_read(&disk->gen_cmds_cntrs_remote[i]));
	}
	return cnt;
}
#undef PAD_BLANKS_LEN

static int nvmeibc_disk_contended_locks_stats_fill(struct nvmeibc_disk* disk, char* buf, int len)
{
	int cnt = 0;
	struct timeval tv;
	struct rtc_time tm;
	u64 count = (u64)atomic64_read(&disk->contended_locks_stats.count);

	BUF_ADD("Total locks that stayed contended for a long time: %llu\n",count);
	if (count) {
		u64 last_jif = (u64)atomic64_read(&disk->contended_locks_stats.last_jif);

		do_gettimeofday(&tv);
		rtc_time_to_tm(tv.tv_sec - sys_tz.tz_minuteswest * 60, &tm);

		BUF_ADD("Last happened on %02d/%02d/%04d %02d:%02d:%02d(UTC) %llu(JIF)\n", tm.tm_mday,
				tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec, last_jif);

		BUF_ADD("To see more info use the filter:\n\tpager.py -f trace=warn_0_disk_locks_nvmeibc_disk_locks_on_completion or trace=warn_disk_locks_nvmeibc_disk_locks_on_cmp_exchange\n");
	}
	return cnt;
}

static int nvmeibc_disk_counters_fill(struct nvmeibc_disk* disk, char* buf, int len)
{
	int cnt = 0;

	#define LINE_ADD(__counter) \
			BUF_ADD("%-*s: %llu\n", \
			40, #__counter, (u64)atomic64_read(&disk->counters.__counter))

	LINE_ADD(n_warn_locate_zero_rscs);
	LINE_ADD(n_warn_locate_timeout);
	LINE_ADD(n_warn_locate_timeout_nzero_rscs);
	LINE_ADD(n_warn_slow_first_io_ch);
	LINE_ADD(n_err_rdda_read_poison);
	LINE_ADD(n_err_rdda_oe_max);
	LINE_ADD(n_err_nrch_wd_rescue);
	LINE_ADD(n_err_nrch_wd_rescue_comp);
	LINE_ADD(n_err_ulp_reuse_req_timeout);
	LINE_ADD(n_err_core_dbgdi_detection);
	LINE_ADD(n_err_jam_non_free_entry_timeout);
	LINE_ADD(n_err_rtrn_rcook_uncomp_sends_exceeded);
	LINE_ADD(n_err_comp_rcook_uncomp_sends_exceeded);
	LINE_ADD(n_err_send_comp_tag_vs_wc);
	LINE_ADD(n_err_nrch_pcpu_lookup_failed);

	return cnt;
}

static void nvmeibc_disk_counters_reset(struct nvmeibc_disk* disk)
{
	memset(&disk->counters, 0, sizeof(disk->counters));
}

static void validate_slow_first_io_chan(struct nvmeibc_disk* disk) {
	ulong no_io_time = jiffies - disk->no_io_time;

	if(!atomic_read(&disk->connected_io_channels) &&
		 no_io_time > NVMEIBC_WAIT_FOR_IO_FIRST_CHAN_WARN) {
			_NW(nvmeibc_first_io_ch_warn,
			"disk: @DISK_NAME: NO IO Channels connected after @JIFFIES, more than @JIFFIES jiffies",
			disk->name, no_io_time, NVMEIBC_WAIT_FOR_IO_FIRST_CHAN_WARN);
			nvmeibc_disk_counters_inc(disk, n_warn_slow_first_io_ch);
	}
}

void nvmeibc_disk_inc_io_chan(struct nvmeibc_disk* disk) {
	if (atomic_inc_return(&disk->connected_io_channels) == 1) {
		_NT(nvmeibc_first_io_ch,
			 "disk @DISK_NAME: First IO Channel connected after @JIFFIES jiffies", disk->name,
			  jiffies - disk->no_io_time);
		disk->no_io_time = 0;
	}
}

void nvmeibc_disk_dec_io_chan(struct nvmeibc_disk* disk) {
	int connected;

	connected = atomic_dec_return(&disk->connected_io_channels);
	if (connected == 0) {
		disk->no_io_time = jiffies;
		_NT(nvmeibc_no_io_ch,
		 "disk @DISK_NAME: IO channel count set to 0, reset no_io_time to @JIFFIES jiffies", disk->name, disk->no_io_time);
	}
	else if (connected < 0) {
	#if !(defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1))
		WARN_ON(1);
	#endif
		_NW(nvmeibc_disk_dec_io_chan_connected_minus, "@DISK_NAME has @INT IO channels", disk->name, connected);
	}
}

static void common_qps_run_cb(struct nvmeibc_disk *disk, qp_pcpu_cb qp_cb, void *data) {
	struct arg_qp_pcpu_cb ndata = {qp_cb, data};
	struct unique_list_ent *unique_ent;
	int i;
	unsigned long flags;
	struct list_head lock_ch_list;
	INIT_LIST_HEAD(&lock_ch_list);

	/* ADMIN */
	if (!list_empty(&disk->arnics))
		call_for_each_arnic(disk, qps_arnic_stats_common, &ndata);

	spin_lock_irqsave(&disk->spinlock, flags);

	/* NRCHs */
	call_for_each_nr_rionic(disk, qps_nr_rionic_stats_common, &ndata);
	/* IOCHs */
	call_for_each_rionic(disk, qps_io_rionic_stats_common, &ndata);

	spin_unlock_irqrestore(&disk->spinlock, flags);

	fill_lock_ch_list(disk, &lock_ch_list);
	while ((unique_ent = list_first_entry_or_null(&lock_ch_list, struct unique_list_ent, link))) {
		struct nvmeibc_locks_channel *lock_ch = unique_ent->ptr;
		qp_cb(lock_ch->net.qp_stats, &lock_ch->net, data);
		for (i = 0; i < NVMEIB_N_2ND_LOCK_CHS; i++) {
			struct nvmeibc_locks_channel *_2nd_lock_ch = lock_ch->_2nd_ch[i];
			if (_2nd_lock_ch)
				qp_cb(_2nd_lock_ch->net.qp_stats, &_2nd_lock_ch->net, data);
		}
		list_del(&unique_ent->link);
		kfree(unique_ent);
	}

}
#undef BUF_ADD

/*
 * Per cpu nordda channel APIs
 */

static void pcpu_nrch_init_mode(struct nvmeibc_disk *disk)
{
	unsigned int cfg_pchs = nvmeibc_nr_pcpu_channels_per_disk; /* read once per discovery */
	unsigned int ncpus;
	unsigned int npchs;
	unsigned int max_possible;
	bool coremask_support = nvmeibc_disk_coremask_support; /* read once per discovery */
	unsigned int max_coremask_nrch = nvmeibc_disk_max_coremask_nrch; /* read once per discovery */
	__NFIND;

	/* default */
	disk->pcpu_nrchs = false;
	disk->pcpu_nrchs_max_per_disk = 0;
	disk->coremask_support = coremask_support;
	disk->max_coremask_nrch = max_coremask_nrch;

	if (coremask_support) {
		if (nvmeibc_nr_pcpu_ch_lockless) {
			_NE(err_pcpu_nrch_init_mode_inv_mode, 
			    "@DISK_NAME - Invalid mode. Cannot use coremask and lockless pcpu channels at the same time", disk->name);

			disk->coremask_support = false;
			goto cfg_pchs;
		}
		/* if cfg_pchs is not 0 or 1 (max possible nrchs), make sure
		* it is big enough for all the coremask nrchs.
		* 
		* Max number of masks is equal to max number of max CPUs as the smallest possible mask
		* must have at least 1 CPU. 
		*/
		cfg_pchs = max(cfg_pchs, NVMEIB_CPU_MASK_MAX_CPUS * max_coremask_nrch + 1);
	}

cfg_pchs:
	if (cfg_pchs) {
		char ll_mask_string[128] = "";
		cfg_pchs--; /* 1 based where 0 represents using max possible pcpu brchs */
		if (!disk->is_tcp) {
			/* use cpu bound kthread of either shared-cqs' ipoller or nrch's rc_wq */
			disk->pcpu_nrchs = true;

			/* calc max-pcpu-nrchs */
			ncpus = num_online_cpus();
			npchs = NVMEIB_DFLT_MAX_CPUS; /* disk->info->pcpu_nrchs[]*/
			max_possible = min(npchs, ncpus);
			disk->pcpu_nrchs_max_per_disk = cfg_pchs ? min(max_possible, cfg_pchs) : max_possible;
			disk->pcpu_nrchs_ll = nvmeibc_nr_pcpu_ch_lockless;
			if (disk->pcpu_nrchs_ll) {
				if (nvmeibc_nr_pcpu_ch_ll_cpus) {
					if (cpumask_parse(nvmeibc_nr_pcpu_ch_ll_cpus, disk->pcpu_nrchs_ll_cpumask)) {
						_NE_dmesg(err_pcpu_nrch_init_inv_pcpu_mask, "Invalid cpumask for nr_pcpu_ch_ll_cpus: @MASK_STRING",
							  nvmeibc_nr_pcpu_ch_ll_cpus);
						disk->pcpu_nrchs_ll = false;
					} else if (cpumask_empty(disk->pcpu_nrchs_ll_cpumask)) {
						_NE_dmesg(err_1_pcpu_nrch_init_inv_pcpu_mask, "Empty cpumask nr_pcpu_ch_ll_cpus: @MASK_STRING",
							  nvmeibc_nr_pcpu_ch_ll_cpus);
						disk->pcpu_nrchs_ll = false;
					} else if (cpumask_weight(disk->pcpu_nrchs_ll_cpumask) > disk->pcpu_nrchs_max_per_disk) {
						_NE_dmesg(err_2_pcpu_nrch_init_inv_pcpu_mask, "Too many cpus in nr_pcpu_ch_ll_cpus: @MASK_STRING (max @N_CPU)",
							  nvmeibc_nr_pcpu_ch_ll_cpus, disk->pcpu_nrchs_max_per_disk);
						disk->pcpu_nrchs_ll = false;
					} else if (NVMEIB_DFLT_MAX_CPUS < nr_cpu_ids &&
						cpumask_next(NVMEIB_DFLT_MAX_CPUS - 1, disk->pcpu_nrchs_ll_cpumask) < nr_cpu_ids) {
						_NE_dmesg(err_3_pcpu_nrch_init_inv_pcpu_mask, "CPUs in nr_pcpu_ch_ll_cpus: @MASK_STRING are > NVMesh max (@N_CPU)",
							  nvmeibc_nr_pcpu_ch_ll_cpus, NVMEIB_DFLT_MAX_CPUS);
						disk->pcpu_nrchs_ll = false;
					}
				} else {
					unsigned int i;
					cpumask_clear(disk->pcpu_nrchs_ll_cpumask);
					for (i = 0; i < disk->pcpu_nrchs_max_per_disk; i++)
						cpumask_set_cpu(i, disk->pcpu_nrchs_ll_cpumask);
				}
			} else {
				cpumask_clear(disk->pcpu_nrchs_ll_cpumask);
			}

			scnprintf(ll_mask_string, sizeof(ll_mask_string), "%*pbl",
				  nr_cpu_ids, cpumask_bits(disk->pcpu_nrchs_ll_cpumask));
			_NI_dmesg(t0_pcpu_nrch_init,
				"ncpus=@INT, npchs=@INT, max-pchs=@INT -> @INT, lockless=@BOOL_YN, lockless_cpus=@CPU_LIST_STR",
				ncpus, npchs, cfg_pchs, disk->pcpu_nrchs_max_per_disk, disk->pcpu_nrchs_ll, ll_mask_string);
		}
		else {
			_NE_dmesg(t1_pcpu_nrch_init,
					  "nr_pcpu_channels not supported in current configuration: "
					  "tcp=@BOOL, use_pcpu_cq=@BOOL, nr_defer_recv_comps=@BOOL",
					  disk->is_tcp, nvmeibc_use_pcpu_cq, nr_defer_recv_comps);
		}
	}

	__NFOUTD;
}

static void pcpu_nrch_init_pool(struct nvmeibc_disk_info *info)
{
	int i;

	if (info) {
		for (i = 0; i < ARRAY_SIZE(info->pcpu_nrchs); i++) {
			info->pcpu_nrchs[i].nrch = NULL;
			spin_lock_init(&info->pcpu_nrchs[i].spinlock);
			INIT_LIST_HEAD(&info->pcpu_nrchs[i].pending_disk_cmds);
			info->pcpu_nrchs[i].n_pending = 0;
			info->pcpu_nrchs[i].max_pending = 0;
			info->pcpu_nrchs[i].uid = 0;
		}
	}
}

static void put_coremask_chs_ref(struct kref *ref)
{
	struct nvmeibc_disk_coremask_chs *coremask_chs = container_of(ref, struct nvmeibc_disk_coremask_chs, refcnt);
	_NT(trace_put_coremask_chs_ref, "Disk @DISK_NAME - Freeing coremask @COREMASK_UID",
	    coremask_chs->disk->name, coremask_chs->uid);
	kfree(coremask_chs);
}

static void rotate_coremask_in_list(struct nvmeibc_disk_coremask_chs *coremask_chs, bool already_locked);

static void _coremask_get_ref_fn(void *ctx)
{
	struct nvmeibc_disk_coremask_chs *coremask_chs = ctx;
	kref_get(&coremask_chs->refcnt);
}

static void _coremask_put_ref_fn(void *ctx)
{
	struct nvmeibc_disk_coremask_chs *coremask_chs = ctx;
	u64 uid = coremask_chs->uid;
	if (kref_put(&coremask_chs->refcnt, put_coremask_chs_ref)) {
		_NT(trace_coremask_put_ref_fn, "Freed coremask @COREMASK_UID", uid);
	}
}

static u64 _coremask_get_uid(void *ctx)
{
	struct nvmeibc_disk_coremask_chs *coremask_chs = ctx;
	return coremask_chs->uid;
}

static const struct nvmeib_cpu_mask *_coremask_get_mask(void *ctx)
{
	struct nvmeibc_disk_coremask_chs *coremask_chs = ctx;
	return &coremask_chs->cpu_coremask;
}

static struct nvmeibc_locks_channel_coremask_ops coremask_ops = {
	.get_ref_fn = _coremask_get_ref_fn,
	.put_ref_fn = _coremask_put_ref_fn,
	.get_uid_fn = _coremask_get_uid,
	.get_mask_fn = _coremask_get_mask,
};

static void disk_coremask_check_deprecated(struct nvmeibc_disk *disk, struct list_head *dying_masks)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeib_cpu_mask_info_node *masks_info_node;
	struct nvmeibc_disk_coremask_chs *entry, *tmp;
	int cpu;
	unsigned long flags;

	__NFIND;
	write_lock_irqsave(&info->coremask_chs_lock, flags);

	/* Loop over masks and remove any that are out of date. */
	list_for_each_entry_safe(entry, tmp, &info->coremask_chs, link) {
		if ((masks_info_node = radix_tree_lookup(&info->masks_tree, entry->uid))) {
			BUG_ON(masks_info_node->mask_info.gen != entry->uid);
			BUG_ON(!NVMEIB_CPU_MASK_EQ(masks_info_node->mask_info.mask, entry->cpu_coremask));
			NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, entry->cpu_coremask) {
				BUG_ON(info->coremask_chs_per_core[cpu] != entry);
				BUG_ON(info->masks_info_per_core[cpu] != masks_info_node);
			}
			continue;
		}

		_NT(trace_disk_check_coremask_update_rem_mask,
			"Disk @DISK_NAME - Detected coremask @COREMASK_UID|@COREMASK_BITMAP "
			"removed from DB. Removing from disk", 
			disk->name, entry->uid,
			/* NOTE: The cpumask only guarantees that nr_cpu_ids bits are valid, 
			 * but the tracer will try and print up to 256.*/
			NVMEIB_CPU_MASK_BITS(entry->cpu_coremask));

		/* Out of date with coremask DB, remove this entry */
		WRITE_ONCE(entry->dying, 1);

		NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, entry->cpu_coremask) {
			WRITE_ONCE(info->coremask_chs_per_core[cpu], NULL);
		}

		list_move_tail(&entry->link, dying_masks);
		BUG_ON(info->n_coremask == 0);
		info->n_coremask--;
	}

	write_unlock_irqrestore(&info->coremask_chs_lock, flags);

	__NFOUTD;
}

static void disk_coremask_connect_locks(struct nvmeibc_disk *disk, struct nvmeibc_disk_coremask_chs *entry)
{
	/* Try and connect lock channels for the coremask */
	struct nvmeibc_ib_admin_channel *admin_ch = get_alive_admin_ch(disk);
	struct nvmeibc_disk_get_segs_locks_flags get_flags = {
		.write = 0, .dont_wait = 0, .remote_only = 1
	};
	struct nvmeibc_disk_segments_locks *disk_segs_locks = nvmeibc_disk_get_segs_locks(disk, get_flags);
	int rv;

	if (disk_segs_locks) {
		BUG_ON(disk_segs_locks->is_local);
		if ((rv = nvmeibc_locks_channel_connect_coremask_chs(
			disk_segs_locks->lock_ch, &admin_ch->base,
			entry->uid, &entry->cpu_coremask, entry, 
			&coremask_ops, &entry->lchs_coremask)) < 0)
		{
			_NT(trace_disk_check_coremask_update_fail_conn_locks,
				"Failed (@RV) to connect per-cpu lock channels for coremask @COREMASK_UID|@COREMASK_BITMAP",
				rv, entry->uid, NVMEIB_CPU_MASK_BITS(entry->cpu_coremask));
			if (rv == -ENOMEM) {
				_NW(warn_disk_check_coremask_update_fail_conn_locks,
				    "Disk @DISK_NAME - Exhausted secondary lock channels. Triggering Rediscover", disk->name);
				nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_COREMASK_LOCK_CHAN_EXHAUSTED);
			}
		} else {
			entry->n_lchs = rv;
			_NT(trace_disk_check_coremask_update_conn_locks,
				"Connected @NUM_CH per-cpu lock channels (cpus @COREMASK_BITMAP) for coremask @COREMASK_UID|@COREMASK_BITMAP",
				entry->n_lchs, NVMEIB_CPU_MASK_BITS(entry->lchs_coremask), entry->uid, NVMEIB_CPU_MASK_BITS(entry->cpu_coremask));
		}
		nvmeibc_disk_put_segs_locks(disk_segs_locks, get_flags);
	} else {
		_NE(err_disk_check_coremask_update_fail_get_locks,
			"Disk @DISK_NAME - Failed to get locks", disk->name);
	}
}

static int disk_coremask_add(struct nvmeibc_disk *disk, u64 uid, struct nvmeib_cpu_mask *cpumask)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeibc_disk_coremask_chs *entry;
	struct nvmeibc_ib_nordda_channel *nrch;
	int mask_cpu;
	unsigned long flags;
	int rv;
	
	__NFIND;
	/* Create new entry for this mask */
	if (!(entry = kzalloc(sizeof(*entry), GFP_KERNEL)))
	{
		_NE(err_disk_check_coremask_update_oom, "OOM");
		rv = -ENOMEM;
		goto out;
	}
	/* Initialise the entry */
	entry->disk = disk;
	spin_lock_init(&entry->spinlock);
	INIT_LIST_HEAD(&entry->pending_cmds);
	plist_head_init(&entry->nrchs_plist);
	kref_init(&entry->refcnt);
	entry->uid = uid;
	NVMEIB_CPU_MASK_COPY(entry->cpu_coremask, *cpumask);
	entry->n_cpus = NVMEIB_CPU_MASK_WEIGHT(entry->cpu_coremask);
	
	NVMEIB_CPU_MASK_FOR_EACH_CPU(mask_cpu, entry->cpu_coremask) {
		if ((nrch = dinfo->pcpu_nrchs[mask_cpu].nrch)) {
			/* Add existing pcpu nrch in the mask to the nrchs list for the entry */
			_NT(trace_disk_check_coremask_update_add_nrch,
			    "Disk @DISK_NAME - Adding per-cpu NRCH @NRCH_NAME to coremask @COREMASK_UID",
				disk->name, nrch->base.name, uid);
			
			/* Get reference for coremask cookie */
			kref_get(&entry->refcnt);
			set_coremask_nrch_cookie(nrch, entry);
			
			nvmeib_public_plist_add(&nrch->available_link, &entry->nrchs_plist);
			NVMEIB_CPU_MASK_SET_CPU(mask_cpu, entry->nrchs_coremask);
			entry->n_nrchs++;
		}
	}
	
	/* Now add the entry to the list and array (under lock) */
	write_lock_irqsave(&info->coremask_chs_lock, flags);
	info->n_coremask++;
	list_add(&entry->link, &info->coremask_chs);
	if (entry->n_nrchs)
		rotate_coremask_in_list(entry, true);
	NVMEIB_CPU_MASK_FOR_EACH_CPU(mask_cpu, entry->cpu_coremask) {
		info->coremask_chs_per_core[mask_cpu] = entry;
	}
	write_unlock_irqrestore(&info->coremask_chs_lock, flags);

	disk_coremask_connect_locks(disk, entry);

	rv = 0;	
out:
	__NFOUTD;
	return rv;
}

static void disk_coremask_check_new(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeib_cpu_mask_info_node *mask_info_node;
	int cpu;
	unsigned long flags;

	__NFIND;
	for (cpu = 0; cpu < NVMEIB_CPU_MASK_MAX_CPUS; cpu++) {
		if ((mask_info_node = info->masks_info_per_core[cpu])) {
			if (READ_ONCE(info->coremask_chs_per_core[cpu])) {
				read_lock_irqsave(&info->coremask_chs_lock, flags);
				/* Check again under lock */
				if (info->coremask_chs_per_core[cpu]) {
					/* Sanity Check - Should have already been removed by the code above */
					BUG_ON(mask_info_node->mask_info.gen != info->coremask_chs_per_core[cpu]->uid);
					read_unlock_irqrestore(&info->coremask_chs_lock, flags);
					continue;
				}
				read_unlock_irqrestore(&info->coremask_chs_lock, flags);
			}
			_NT(trace_disk_check_coremask_update_add_mask,
			    "Disk @DISK_NAME - Detected coremask @COREMASK_UID|@COREMASK_BITMAP added to DB. Adding to disk",
				disk->name, mask_info_node->mask_info.gen, 
				/* NOTE: The cpumask only guarantees that nr_cpu_ids bits are valid, but the tracer will try and print up to 256.*/
				mask_info_node->mask_info.mask.cpus);

			disk_coremask_add(disk, mask_info_node->mask_info.gen, &mask_info_node->mask_info.mask);
		}
	}
	__NFOUTD;
}

static unsigned disk_coremask_calc_max_nrch(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
	unsigned max_coremask_nrch;
	unsigned max_pcpu_nrchs = 0;
	
	__NFIND;

	/* Calculate the maximum number of per-cpu nrchs given the number of paths */
	list_for_each_entry(rionic, &disk->rionics, disk_link) {
		if (atomic_read(&rionic->dying))
			continue;
		list_for_each_entry(lionic, &rionic->lionics, rionic_link) {
			if (atomic_read(&lionic->dying))
				continue;
			max_pcpu_nrchs += lionic->n_nr_qps;
		}
	}
	max_pcpu_nrchs = max_pcpu_nrchs > nr_max_channels_per_disk ?
		max_pcpu_nrchs - nr_max_channels_per_disk : 0;

	/* Calculate the current maximum nrch per mask given:
		* - The maximum from the modparam disk_max_coremask_nrch
		* - The current number of masks
		* - The maximum number of per-cpu nrchs given the number of paths
		* - The maximum number of per-cpu nrchs per disk
		*/
	max_coremask_nrch = info->n_coremask 
		? min3(disk->max_coremask_nrch, 
			disk->pcpu_nrchs_max_per_disk / info->n_coremask,
			max_pcpu_nrchs / info->n_coremask)
		: disk->max_coremask_nrch;

	_ND(debug_1_disk_check_coremask_update, 
		"info->last_coremask_update_count @UINT "
		"max_coremask_nrch @UINT "
		"disk->max_coremask_nrch @UINT "
		"disk->pcpu_nrchs_max_per_disk @UINT "
		"max_pcpu_nrchs @UINT"
		"info->n_coremask @UINT "
		"min3(disk->max_coremask_nrch, disk->pcpu_nrchs_max_per_disk / info->n_coremask, max_pcpu_nrchs / info->n_coremask) @UINT",
		info->last_coremask_update_count,
		max_coremask_nrch,
		disk->max_coremask_nrch,
		disk->pcpu_nrchs_max_per_disk,
		max_pcpu_nrchs,
		info->n_coremask,
		info->n_coremask ? 
			min3(disk->max_coremask_nrch,
				disk->pcpu_nrchs_max_per_disk / info->n_coremask,
				max_pcpu_nrchs / info->n_coremask) :
			disk->max_coremask_nrch
		);

	__NFOUTD;
	return max_coremask_nrch;
}

static void disk_coremask_disconnect_extra_nrch(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeibc_disk_coremask_chs *entry;
	struct nvmeibc_ib_nordda_channel *nrch, *tmp;
	int n_nrchs;
	unsigned long flags;
	
	__NFIND;
	read_lock_irqsave(&info->coremask_chs_lock, flags);
	list_for_each_entry(entry, &info->coremask_chs, link) {
		spin_lock(&entry->spinlock);
		n_nrchs = entry->n_nrchs;
		if (n_nrchs > info->curr_max_coremask_nrch) {
			plist_for_each_entry_safe(nrch, tmp, &entry->nrchs_plist, available_link) {
				nvmeibc_ib_nordda_channel_try_disconnect(nrch);
				_NT(trace_disk_check_coremask_update_disconnect_extra_nrch,
					"Disk @DISK_NAME - Disconnecting coremask @COREMASK_UID extra NRCH @NRCH_NAME",
					disk->name, entry->uid, nrch->base.name);
				if (--n_nrchs <= info->curr_max_coremask_nrch)
					break;
			}
		}
		spin_unlock(&entry->spinlock);
	}
	read_unlock_irqrestore(&info->coremask_chs_lock, flags);
	__NFOUTD;
}

static void disk_coremask_disconnect_locks(struct nvmeibc_disk *disk, struct nvmeibc_disk_coremask_chs *entry)
{
	struct nvmeibc_disk_get_segs_locks_flags get_flags = {
		.write = 0, .dont_wait = 0, .remote_only = 1
	};
	struct nvmeibc_disk_segments_locks *disk_segs_locks;

	__NFIND;

	disk_segs_locks = nvmeibc_disk_get_segs_locks(
		disk, get_flags);
	if (disk_segs_locks) {
		BUG_ON(disk_segs_locks->is_local);
		nvmeibc_locks_channel_disconnect_coremask_chs(
			disk_segs_locks->lock_ch,
			&entry->cpu_coremask, entry);
		_NT(trace_disk_check_coremask_update_disconn_locks,
		    "Disconnected per-cpu lock channels for coremask @COREMASK_UID|@COREMASK_BITMAP", 
			entry->uid, NVMEIB_CPU_MASK_BITS(entry->cpu_coremask));
		nvmeibc_disk_put_segs_locks(disk_segs_locks, get_flags);
	} else {
		_NE(err_disk_check_coremask_update_fail_get_locks_2,
		    "Disk @DISK_NAME - Failed to get locks", disk->name);
	}

	__NFOUTD;
}

static void disk_coremask_clean_dying(struct nvmeibc_disk *disk, struct list_head *dying_masks, struct list_head *pending_cmds)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeibc_disk_coremask_chs *entry;
	struct nvmeibc_ib_nordda_channel *nrch, *tmp;
	unsigned long flags;

	__NFIND;
	while ((entry = list_first_entry_or_null(dying_masks, struct nvmeibc_disk_coremask_chs, link))) {
		_NT(trace_disk_check_coremask_update_clean_mask,
			"Disk @DISK_NAME - Cleaning up removed coremask @COREMASK_UID|@COREMASK_BITMAP with NRCH @COREMASK_BITMAP",
			disk->name, entry->uid,
			/* NOTE: The cpumask only guarantees that nr_cpu_ids bits are valid, but the tracer will try and print up to 256.*/
			NVMEIB_CPU_MASK_BITS(entry->cpu_coremask), NVMEIB_CPU_MASK_BITS(entry->nrchs_coremask));

		/* Disconnect lock channels for the coremask */
		disk_coremask_disconnect_locks(disk, entry);
		
		spin_lock_irqsave(&entry->spinlock, flags);
		
		/* Disconnect all nrch channels */
		plist_for_each_entry_safe(nrch, tmp, &entry->nrchs_plist, available_link) {
			nvmeibc_ib_nordda_channel_try_disconnect(nrch);
			_NT(trace_disk_check_coremask_update_disconnect_nrch,
			    "Disk @DISK_NAME - Disconnecting coremask @COREMASK_UID NRCH @NRCH_NAME",
				disk->name, entry->uid, nrch->base.name);
		}
		
		/* Remove all pending commands */
		list_splice_init(&entry->pending_cmds, pending_cmds);
		
		spin_unlock_irqrestore(&entry->spinlock, flags);
		
		/* Remove from remote-info coremasks list */
		write_lock_irqsave(&info->coremask_chs_lock, flags);
		list_del(&entry->link);
		write_unlock_irqrestore(&info->coremask_chs_lock, flags);
		
		/* Put reference from coremask list */
		kref_put(&entry->refcnt, put_coremask_chs_ref);
	}
	__NFOUTD;
}

static void retry_pending_cmds_or_abort(struct nvmeibc_disk *disk, struct list_head *pending_cmds)
{
	struct nvmeibc_disk_command *disk_cmd;
	int rv;

	__NFIND;
	while ((disk_cmd = list_first_entry_or_null(pending_cmds, struct nvmeibc_disk_command, dcmd_link))) {
		list_del(&disk_cmd->dcmd_link);
		if ((rv = execute_io_remote(disk, disk_cmd) < 0)) {
			_NT(trace_disk_check_coremask_update_exec_fail,
			    "Disk @DISK_NAME - Failed (@RV) executing pending commands. Aborting",
			    disk->name, rv);
			list_add(&disk_cmd->dcmd_link, pending_cmds);
			break;
		}
	}
	if (!list_empty(pending_cmds)) {
		/* Any remaining pending cmds should be aborted */
		pending_cmds_abort(disk, pending_cmds, false);
	}
	__NFOUTD;
}

/* Runs on main-wq. Avoids the need to hold the volume spinlock while traversing the disk->volumes list */
static void disk_coremask_update_main_work(struct workqe_struct *work)
{
	struct nvmeibc_disk_coremask_info *cinfo = container_of(work, struct nvmeibc_disk_coremask_info, update_masks_main_work);
	struct nvmeibc_disk *disk = cinfo->disk;
	const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(disk);
	struct nvmeibc_disk_id *disk_id;
	struct nvmeib_cpu_mask_info *masks_info_arr = cinfo->update_masks_scratch;
	struct nvmeib_cpu_mask_info_node *masks_info_node;
	struct radix_tree_iter masks_iter;
	void **slot;
	int rv, i, n_masks_arr;

	__NFIND;
	
	BUG_ON(!nvmeibc_is_on_main_wq(nvmeibc_isnt_params_core2main(p), true));

	if (atomic_read(&disk->dying))
		goto out;

	mutex_lock(&cinfo->masks_guard);
	/* Increment the counter that will be refreshed / stored in each masks info in the tree so we known which ones to prune */
	cinfo->masks_update_count++;
	/* Walk the disks volumes list, get each volumes masks and update the radix-tree */
	list_for_each_entry(disk_id, &disk->volumes, slink) {
		if ((rv = nvmeibc_volume_get_cpu_masks(disk_id->volume, masks_info_arr, NVMEIB_CPU_MASK_MAX_CPUS)) < 0) {
			_NE(err_disk_get_all_vols_coremasks_get_fail, 
			    "Failed (@RV) to get masks for volume @VOLUMEID",
			    rv, disk_id->volume->hdr.devname);
			continue;
		}
		n_masks_arr = rv;
		_NT(trace_disk_coremask_update_work, "Disk @DISK_NAME - Volume @VOLUMEID has @COUNT coremasks", 
		    disk->name, disk_id->volume->hdr.devname, n_masks_arr); 
		for (i = 0; i < n_masks_arr; i++) {
			if ((masks_info_node = radix_tree_lookup(&cinfo->masks_tree, masks_info_arr[i].gen))) {
				BUG_ON(masks_info_node->mask_info.gen != masks_info_arr[i].gen);
				BUG_ON(!NVMEIB_CPU_MASK_EQ(masks_info_node->mask_info.mask, masks_info_arr[i].mask));
				_NT(trace_1_disk_coremask_update_work, "Disk @DISK_NAME - Coremask @COREMASK_UID|@COREMASK_BITMAP already in tree",
				    disk->name, masks_info_arr[i].gen, masks_info_arr[i].mask.cpus);
				/* Refresh the update count so we don't prune this node */
				masks_info_node->update_count = cinfo->masks_update_count;
				continue;
			}
			_NT(trace_2_disk_coremask_update_work, 
			    "Disk @DISK_NAME - Adding coremask @COREMASK_UID|@COREMASK_BITMAP from volume @VOLUMEID to tree",
				disk->name, masks_info_arr[i].gen, masks_info_arr[i].mask.cpus, disk_id->volume->hdr.devname);
			if (!(masks_info_node = kzalloc(sizeof(*masks_info_node), GFP_KERNEL))) {
				_NE(err_disk_coremask_update_work_oom, "OOM");
				goto unlock;
			}
			masks_info_node->mask_info = masks_info_arr[i];
			masks_info_node->update_count = cinfo->masks_update_count;
			radix_tree_insert(&cinfo->masks_tree, masks_info_arr[i].gen, masks_info_node);
		}
	}
	/* At this point, all masks in the tree that were refreshed/created from all the volumes will have the correct update count.
	 * Now, we prune all the ones with the old count 
	 * Also update the per-cpu array.
	 */
	memset(cinfo->masks_info_per_core, 0, sizeof(cinfo->masks_info_per_core));
	radix_tree_for_each_slot(slot, &cinfo->masks_tree, &masks_iter, 0) {
		int cpu;
		masks_info_node = *slot;

		if (masks_info_node->update_count != cinfo->masks_update_count) {
			// Detected deprecated mask. Prune it from the tree */
			_NT(trace_3_disk_coremask_update_work, 
			    "Disk @DISK_NAME - Removing deprecated @COREMASK_UID|@COREMASK_BITMAP from tree",
				disk->name, masks_info_node->mask_info.gen, masks_info_node->mask_info.mask.cpus);

			radix_tree_delete(&cinfo->masks_tree, masks_iter.index);
			kfree(masks_info_node);
			continue;
		}
		NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, masks_info_node->mask_info.mask) {
			BUG_ON(cinfo->masks_info_per_core[cpu] != NULL);
			cinfo->masks_info_per_core[cpu] = masks_info_node;
		}
	}
unlock:
	mutex_unlock(&cinfo->masks_guard);
out:
	__NFOUTD;
}

static void disk_check_coremask_update(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	LIST_HEAD(dying_masks);
	LIST_HEAD(pending_cmds);
	unsigned prev_max_coremask_nrch;
	const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(disk);

	if (info->last_masks_update_count != info->masks_update_count) {
		mutex_lock(&info->masks_guard);
		_NT(trace_disk_check_coremask_update_detect_update,
		    "Disk @DISK_NAME - Detected coremask update (@COUNTS -> @COUNTS)",
		    disk->name, info->last_masks_update_count, info->masks_update_count);

		/* Check for any masks removed from the DB */
		disk_coremask_check_deprecated(disk, &dying_masks);

		/* Clean up all dying masks */
		disk_coremask_clean_dying(disk, &dying_masks, &pending_cmds);

		/* Hand-off any pending cmds from dying coremask(s) to the any-core channels */
		retry_pending_cmds_or_abort(disk, &pending_cmds);

		/* Now look for any new masks in the DB */
		disk_coremask_check_new(disk);

		prev_max_coremask_nrch = info->curr_max_coremask_nrch;
		info->curr_max_coremask_nrch = disk_coremask_calc_max_nrch(disk);

		/* If the current maximum nrch per mask has changed,
		 * disconnect any extra channels */
		if (prev_max_coremask_nrch != info->curr_max_coremask_nrch) {
			if (!info->curr_max_coremask_nrch) {
				_NW_dmesg(warn_disk_check_coremask_update_max_nrch_zero,
					  "Disk @DISK_NAME - Too many coremasks (@COUNT) - Max NRCH per mask is Zero!",
					  disk->name, info->n_coremask);
			} else {
				_NT(trace_disk_check_coremask_update_max_nrch_chng,
				"Disk @DISK_NAME - Max NRCH per mask changed from @MAX_CH to @MAX_CH (new n_coremask @COUNT)",
					disk->name, prev_max_coremask_nrch, info->curr_max_coremask_nrch, info->n_coremask);
			}
			if (info->curr_max_coremask_nrch < prev_max_coremask_nrch) {
				/* Disconnect extra channels to make them available for new masks */
				disk_coremask_disconnect_extra_nrch(disk);
			}
		}
		info->last_masks_update_count = info->masks_update_count;
		mutex_unlock(&info->masks_guard);
	}

	/* Schedule the update on the main WQ */
	nvmeibc_add_work(nvmeibc_isnt_params_core2main(p), &info->update_masks_main_work);
}

static void disk_coremask_update_admin_work(struct workqe_struct *work)
{
	struct nvmeibc_disk_coremask_info *cinfo = container_of(
		work, struct nvmeibc_disk_coremask_info, update_masks_admin_work);
	struct nvmeibc_disk *disk = cinfo->disk;

	/* This calls disk_check_coremask_update() before calling nvmeibc_disk_start_io_nordda_channels_() */
	disk_start_io_channels_(disk);
}

#define CALL_JSON_FN(fn, name, val, is_last, indent)\
	do {\
		count += (*jops->fn)(buf + count, len - count, name, val, is_last, indent);\
	} while(0)
	
#undef CALL_JSON_START_OBJ
#define CALL_JSON_START_OBJ(name, indent)\
	do {\
		count += (*jops->start_obj)(buf + count, len - count, name, indent);\
	} while(0)
	
#undef CALL_JSON_END_OBJ
#define CALL_JSON_END_OBJ(is_last, indent)\
	do {\
		count += (*jops->end_obj)(buf + count, len - count, is_last, indent);\
	} while(0)

#define CORE_CLIENT_DISK_CORE_MASKS_JSON_PROC_FRMT_VER 1
static ssize_t core_masks_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0, rv;
	struct write_status_buf_data write_status_data = {
		.disk = disk,
		.buf = buf,
		.count = &count,
		.len = len,
		.status_type = WRITE_STATUS_COREMASK_JSON,
	};
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = DISK_UPDATE_WRITE_STATUS,
		.update_data = &write_status_data,
		.done_cb = write_status_buf_done_cb,
		.done_cb_ctx = &comp
	};
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;

	NFIN;
	CALL_JSON_START_OBJ(NULL, write_status_data.ntabs++);

	if ((rv = nvmeibc_disk_update_config(disk, &disk_update_data, false)) < 0) {
		CALL_JSON_FN(data_sval, "error", rv, JSON_LAST_ELEM, write_status_data.ntabs);
		goto epilogue;
	}

	wait_for_completion(&comp);

epilogue:
	count += nvmeib_proc_add_json_proc_epilog(CORE_CLIENT_DISK_CORE_MASKS_JSON_PROC_FRMT_VER, buf + count, len - count);
	CALL_JSON_END_OBJ(JSON_LAST_ELEM, --write_status_data.ntabs);

	NFOUT;
	return count;
}

#undef CALL_JSON_FN
#undef CALL_JSON_START_OBJ
#undef CALL_JSON_END_OBJ

#define CALL_JSON_FN(data, fn, name, val, is_last)\
do {\
	*data->count += (*jops->fn)(data->buf + *data->count, data->len - *data->count, name, val, is_last, data->ntabs);\
} while(0)

#define CALL_JSON_DATA_SPRINTF(data, is_last, name, fmt, ...)\
do {\
	*data->count += (*jops->data_sprintf)(data->buf + *data->count, data->len - *data->count, is_last, data->ntabs, name, fmt, __VA_ARGS__);\
} while(0)

#define CALL_JSON_START_OBJ(data, name)\
do {\
	*data->count += (*jops->start_obj)(data->buf + *data->count, data->len - *data->count, name, data->ntabs++);\
} while(0)

#define CALL_JSON_END_OBJ(data, is_last)\
do {\
	*data->count += (*jops->end_obj)(data->buf + *data->count, data->len - *data->count, is_last, --data->ntabs);\
} while(0)

#undef CALL_JSON_START_ARRAY
#define CALL_JSON_START_ARRAY(data, name)\
do {\
	*data->count += (*jops->start_array)(data->buf + *data->count, data->len - *data->count, name, data->ntabs++);\
} while(0)

#undef CALL_JSON_END_ARRAY
#define CALL_JSON_END_ARRAY(data, is_last)\
do {\
	*data->count += (*jops->end_array)(data->buf + *data->count, data->len - *data->count, is_last, --data->ntabs);\
} while(0)
	
static void write_coremask_json_buf(struct write_status_buf_data *data)
{
	struct nvmeibc_disk *disk = data->disk;
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo ? dinfo->coremask_info : NULL;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	struct nvmeibc_disk_coremask_chs *entry;
	struct nvmeibc_ib_nordda_channel *nrch;
	struct nvmeibc_channel *lch_base;
	struct nvmeibc_disk_get_segs_locks_flags get_flags = {
		.write = 0, .dont_wait = 0, .remote_only = 1
	};
	struct nvmeibc_disk_segments_locks *disk_segs_locks;
	struct nvmeib_cpu_mask lch_cpumask = {}, all_lch_cpumask = {};
	int cpu;
	unsigned long flags;

	NFIN;
	if (!info) {
		if (!dinfo) {
			if (disk->access_local)
				CALL_JSON_FN(data, data_str, "error", "not supported for local disk", JSON_LAST_ELEM);
			else
				CALL_JSON_FN(data, data_str, "error", "disk not discovered", JSON_LAST_ELEM);
		} else {
			CALL_JSON_FN(data, data_str, "error", "coremasks not enabled", JSON_LAST_ELEM);
		}
		goto out;
	}
	
	disk_segs_locks = nvmeibc_disk_get_segs_locks(disk, get_flags);
	BUG_ON(disk_segs_locks->is_local);

	CALL_JSON_START_ARRAY(data, "core_masks");

	read_lock_irqsave(&info->coremask_chs_lock, flags);
	list_for_each_entry(entry, &info->coremask_chs, link) {
		spin_lock(&entry->spinlock);
		CALL_JSON_START_OBJ(data, NULL);
		CALL_JSON_FN(data, data_uval, "uid", entry->uid, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_sval, "dying", entry->dying, !JSON_LAST_ELEM);
		CALL_JSON_DATA_SPRINTF(data, !JSON_LAST_ELEM, "cpu_mask", NVMEIB_CPU_MASK_PR_FMT(), NVMEIB_CPU_MASK_PR_ARGS(entry->cpu_coremask));
		CALL_JSON_FN(data, data_sval, "n_pending", entry->n_pending, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_sval, "max_pending", entry->max_pending, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_io_mask_dying", entry->n_io_mask_dying, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_io_mask_uid_mismatch", entry->n_io_mask_uid_mismatch, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_io_mask_no_nrch", entry->n_io_mask_no_nrch, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_io_mask_nrch_busy", entry->n_io_mask_nrch_busy, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_io_mask_chan", entry->n_io_mask_chan, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_reuse_io_mask_dying", entry->n_reuse_io_mask_dying, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_reuse_io_mask_uid_mismatch", entry->n_reuse_io_mask_uid_mismatch, !JSON_LAST_ELEM);
		CALL_JSON_FN(data, data_uval, "n_reuse_io_mask_chan", entry->n_reuse_io_mask_chan, !JSON_LAST_ELEM);
		CALL_JSON_START_ARRAY(data, "nr_channels");
		plist_for_each_entry(nrch, &entry->nrchs_plist, available_link) {
			CALL_JSON_START_OBJ(data, NULL);
			CALL_JSON_FN(data, data_str, "name", nrch->base.name, !JSON_LAST_ELEM);
			CALL_JSON_FN(data, data_sval, "cpu", pcpu_nrch_cpu_get(nrch), JSON_LAST_ELEM);
			CALL_JSON_END_OBJ(data,
					  nrch == plist_last_entry(&entry->nrchs_plist, typeof(*nrch), available_link) ?
					  JSON_LAST_ELEM : !JSON_LAST_ELEM);
		}
		CALL_JSON_END_ARRAY(data, !JSON_LAST_ELEM);
		CALL_JSON_START_ARRAY(data, "lock_channels");
		if (disk_segs_locks) {
			int n_lch = 0;
			NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, entry->cpu_coremask) {
				if (NVMEIB_CPU_MASK_TEST_CPU(cpu, all_lch_cpumask))
					continue;
				if ((lch_base = nvmeibc_locks_channel_get_coremash_ch_for_cpu(disk_segs_locks->lock_ch, entry, cpu, &lch_cpumask))) {
					/* End the previous object (not the last) */
					if (n_lch) {
						CALL_JSON_END_OBJ(data, !JSON_LAST_ELEM);
					}
					CALL_JSON_START_OBJ(data, NULL);
					CALL_JSON_FN(data, data_str, "name", lch_base->name, !JSON_LAST_ELEM);
					CALL_JSON_FN(data, data_sval, "comp_cpu", nvmeibc_channel_get_coremask_ch_cpu(lch_base), !JSON_LAST_ELEM);
					CALL_JSON_DATA_SPRINTF(data, JSON_LAST_ELEM, "mask_cpus", NVMEIB_CPU_MASK_PR_FMT(), NVMEIB_CPU_MASK_PR_ARGS(lch_cpumask));
					n_lch++;
					NVMEIB_CPU_MASK_OR(all_lch_cpumask, all_lch_cpumask, lch_cpumask);
				}
			}
			/* End the last object */
			CALL_JSON_END_OBJ(data, JSON_LAST_ELEM);
		}
		CALL_JSON_END_ARRAY(data, JSON_LAST_ELEM);
		CALL_JSON_END_OBJ(data, entry->link.next == &info->coremask_chs ?
				JSON_LAST_ELEM : !JSON_LAST_ELEM);
		spin_unlock(&entry->spinlock);
	}
	read_unlock_irqrestore(&info->coremask_chs_lock, flags);

	CALL_JSON_END_ARRAY(data, JSON_LAST_ELEM);
	
	nvmeibc_disk_put_segs_locks(disk_segs_locks, get_flags);

out:
	NFOUT;
}

#undef CALL_JSON_FN
#undef CALL_JSON_START_OBJ
#undef CALL_JSON_END_OBJ
#undef CALL_JSON_START_ARRAY
#undef CALL_JSON_END_ARRAY
#undef CALL_JSON_DATA_SPRINTF

#define CALL_JSON_FN(fn, name, val, is_last, indent)\
do {\
	count += (*jops->fn)(buf + count, len - count, name, val, is_last, indent);\
} while(0)

#define CALL_JSON_START_OBJ(name, indent)\
do {\
	count += (*jops->start_obj)(buf + count, len - count, name, indent);\
} while(0)

#define CALL_JSON_END_OBJ(is_last, indent)\
do {\
	count += (*jops->end_obj)(buf + count, len - count, is_last, indent);\
} while(0)

#define CORE_CLIENT_DISK_CORE_MASKS_JSON_PROC_FRMT_VER 1
static ssize_t coremask_stats_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t count = 0, rv;
	struct write_status_buf_data write_status_data = {
		.disk = disk,
		.buf = buf,
		.count = &count,
		.len = len,
		.status_type = WRITE_STATUS_COREMASK_STATS,
	};
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = DISK_UPDATE_WRITE_STATUS,
		.update_data = &write_status_data,
		.done_cb = write_status_buf_done_cb,
		.done_cb_ctx = &comp
	};
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	
	NFIN;
	CALL_JSON_START_OBJ(NULL, write_status_data.ntabs++);
	
	if ((rv = nvmeibc_disk_update_config(disk, &disk_update_data, false)) < 0) {
		CALL_JSON_FN(data_sval, "error", rv, JSON_LAST_ELEM, write_status_data.ntabs);
		goto epilogue;
	}
	
	wait_for_completion(&comp);
	
epilogue:
	count += nvmeib_proc_add_json_proc_epilog(CORE_CLIENT_DISK_CORE_MASKS_JSON_PROC_FRMT_VER, buf + count, len - count);
	CALL_JSON_END_OBJ(JSON_LAST_ELEM, --write_status_data.ntabs);
	
	NFOUT;
	return count;
}

#undef CALL_JSON_FN
#undef CALL_JSON_START_OBJ
#undef CALL_JSON_END_OBJ

struct sum_coremask_stats_pcpu_ctx {
	struct nvmeibc_disk_coremask_info *cinfo;
	struct nvmeibc_disk_coremask_pcpu_stats tot_pcpu_stats;
	spinlock_t lock;
};

#define COREMASK_PCPU_STAT_SUM(_out, _pcpu, _stat) COREMASK_PCPU_STAT_ADD(_out, _stat, (_pcpu->_stat))

static void __sum_coremask_stats_pcpu_fn(void *ctx)
{
	struct sum_coremask_stats_pcpu_ctx *pcpu_ctx = ctx;
	struct nvmeibc_disk_coremask_pcpu_stats __percpu *this_cpu_stats = this_cpu_ptr(pcpu_ctx->cinfo->pcpu_stats);
	unsigned long flags;
	
	spin_lock_irqsave(&pcpu_ctx->lock, flags);
	/* Add this cpu io stats to sum */
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_not_coremask_op);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_op);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_submit_cpu_not_in_mask);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_not_exist);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_dying);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_uid_mismatch);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_nrch);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_no_nrch);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_nrch_busy);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_pending_push);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_io_coremask_pending_pop);

	/* Add this cpu reuse io stats to sum */
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_reuse_io_coremask_dying);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_reuse_io_coremask_uid_mismatch);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, 
			       n_reuse_io_coremask_submit_cpu_not_in_mask);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_reuse_io_coremask_op);

	/* Max this cpu max_io_coremask_pending with "sum" */
	COREMASK_PCPU_STAT_MAX(&pcpu_ctx->tot_pcpu_stats, max_io_coremask_pending, this_cpu_stats->max_io_coremask_pending);

	/* Add this cpu lock stats to sum */
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_lock_not_coremask_op);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_lock_coremask_op);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_lock_coremask_not_exist);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_lock_coremask_no_lock_ch);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_lock_coremask_submit_cpu_not_in_mask);
	COREMASK_PCPU_STAT_SUM(&pcpu_ctx->tot_pcpu_stats, this_cpu_stats, n_lock_coremask_uid_mismatch);

	spin_unlock_irqrestore(&pcpu_ctx->lock, flags);
}

#define CALL_JSON_FN(data, fn, name, val, is_last)\
do {\
	*data->count += (*jops->fn)(data->buf + *data->count, data->len - *data->count, name, val, is_last, data->ntabs);\
} while(0)

#define WRITE_JSON_STAT(data, _stat, _struct, is_last) CALL_JSON_FN(data, data_uval, NV_PP_STR(_stat), _struct._stat, is_last);

static void write_coremask_stats_json_buf(struct write_status_buf_data *data)
{
	struct nvmeibc_disk *disk = data->disk;
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo ? dinfo->coremask_info : NULL;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	struct sum_coremask_stats_pcpu_ctx pcpu_ctx = {};

	NFIN;
	if (!info) {
		if (!dinfo) {
			if (disk->access_local)
				CALL_JSON_FN(data, data_str, "error", "not supported for local disk", JSON_LAST_ELEM);
			else
				CALL_JSON_FN(data, data_str, "error", "disk not discovered", JSON_LAST_ELEM);
		} else {
			CALL_JSON_FN(data, data_str, "error", "coremasks not enabled", JSON_LAST_ELEM);
		}
		goto out;
	}

	pcpu_ctx.cinfo = info;
	spin_lock_init(&pcpu_ctx.lock);
	on_each_cpu(__sum_coremask_stats_pcpu_fn, &pcpu_ctx, true);

	/* Write IO Stats */
	WRITE_JSON_STAT(data, n_io_not_coremask_op, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_op, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_submit_cpu_not_in_mask, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_not_exist, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_dying, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_uid_mismatch, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_nrch, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_no_nrch, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_nrch_busy, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_pending_push, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_io_coremask_pending_pop, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, max_io_coremask_pending, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);

	/* Write Reuse IO Stats */
	WRITE_JSON_STAT(data, n_reuse_io_coremask_dying, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_reuse_io_coremask_uid_mismatch, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_reuse_io_coremask_submit_cpu_not_in_mask,
			pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_reuse_io_coremask_op, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);

	/* Write Lock Stats */
	WRITE_JSON_STAT(data, n_lock_not_coremask_op, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_lock_coremask_op, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_lock_coremask_not_exist, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_lock_coremask_no_lock_ch, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_lock_coremask_submit_cpu_not_in_mask, pcpu_ctx.tot_pcpu_stats, !JSON_LAST_ELEM);
	WRITE_JSON_STAT(data, n_lock_coremask_uid_mismatch, pcpu_ctx.tot_pcpu_stats, JSON_LAST_ELEM);

out:
	NFOUT;
}

static ssize_t coremask_stats_reset(void *priv, char *buf, size_t len)
{
	struct nvmeibc_disk *disk = priv;
	ssize_t rv;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = DISK_UPDATE_RESET_COREMASK_STATS,
		.update_data = disk,
		.done_cb = write_status_buf_done_cb,
		.done_cb_ctx = &comp
	};
	int reset;

	NFIN;
	if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
		rv = -EINVAL;
		goto out;
	}
	if ((rv = nvmeibc_disk_update_config(disk, &disk_update_data, false)) < 0) {
		_NE(error_1_coremask_stats_reset, "nvmeibc_disk_update_config failed rv=@RV", rv);
		goto out;
	}

	wait_for_completion(&comp);
	rv = len;

out:
	NFOUT;
	return rv;
}

#undef CALL_JSON_FN

static void rotate_coremask_in_list(struct nvmeibc_disk_coremask_chs *coremask_chs, bool already_locked)
{
	struct nvmeibc_disk *disk = coremask_chs->disk;
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct list_head *iter, *insert;
	struct nvmeibc_disk_coremask_chs *iter_mask;
	unsigned long flags;
	
	/* Rotate coremask_chs position in the list.
	 * We want to add it to the end of last mask with the same n_nrch */
	if (!already_locked)
		write_lock_irqsave(&info->coremask_chs_lock, flags);

	list_del(&coremask_chs->link);
	insert = &info->coremask_chs;
	list_for_each(iter, &info->coremask_chs) {
		iter_mask = list_entry(iter, struct nvmeibc_disk_coremask_chs, link);
		if (iter_mask->n_nrchs <= coremask_chs->n_nrchs)
			insert = iter;
		else
			break;
	}
	list_add(&coremask_chs->link, insert);
	
	if (!already_locked)
		write_unlock_irqrestore(&info->coremask_chs_lock, flags);
}

static int pcpu_nrch_get_next_coremask_cpu(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	/* Used as a source of randomness (for picking a core) */
	static unsigned rand_src = 0;
	unsigned rand_cnt;
	struct nvmeib_cpu_mask cpu_mask_not_nrch = {}, cpu_mask_not_nrch_not_lch = {};
	struct nvmeib_cpu_mask *mask = &cpu_mask_not_nrch_not_lch;
	struct nvmeibc_disk_coremask_chs *coremask_chs = NULL, *iter;
	int cpu, next_cpu = -1;

	__NFIND;

	/* NOTE: This runs on the same WQ as disk_check_coremask_update
	 * so we don't need to worry about coremask_list being modified under out feet */
	list_for_each_entry(iter, &info->coremask_chs, link)
	{
		if (iter->n_nrchs >= info->curr_max_coremask_nrch) {
			_NT(trace_pcpu_nrch_get_next_coremask_cpu_max_nrch, 
			    "Disk @DISK_NAME - Not creating per-cpu channel on coremask @COREMASK_UID, has NRCH @NUM_CH (max @MAX_CH)",
				disk->name, iter->uid, iter->n_nrchs, disk->max_coremask_nrch);
			continue;
		}
		/* Get a mask of all mask cpus that do not have an nrch (returns false is dest is empty) */
		if (!NVMEIB_CPU_MASK_AND_NOT(cpu_mask_not_nrch, iter->cpu_coremask, iter->nrchs_coremask)) {
			_NT(trace_pcpu_nrch_get_next_coremask_cpu_no_cpu,
			    "Disk @DISK_NAME - Not creating per-cpu channel,"
				"Coremask @COREMASK_UID has all CPUs (mask @COREMASK_BITMAP) with NRCHs (mask @COREMASK_BITMAP)", 
			    disk->name, iter->uid, NVMEIB_CPU_MASK_BITS(iter->cpu_coremask), NVMEIB_CPU_MASK_BITS(iter->nrchs_coremask));
			continue;
		}
		/* For better spreading, give preference to CPUs that don't have a lock channel */
		if (!NVMEIB_CPU_MASK_AND_NOT(cpu_mask_not_nrch_not_lch, cpu_mask_not_nrch, iter->lchs_coremask)) {
			_NT(trace_pcpu_nrch_get_next_coremask_cpu_nrch_overlap_lch_cpu,
			    "Disk @DISK_NAME - Creating per-cpu NRCHs on CPU overlapping with LCHs CPUs"
			    "Coremask @COREMASK_UID has all CPUs (mask @COREMASK_BITMAP) with either NRCH (mask @COREMASK_BITMAP) or LCH (mask @COREMASK_BITMAP)", 
			    disk->name, iter->uid, NVMEIB_CPU_MASK_BITS(iter->cpu_coremask), 
			    NVMEIB_CPU_MASK_BITS(iter->nrchs_coremask), NVMEIB_CPU_MASK_BITS(iter->lchs_coremask));
			mask = &cpu_mask_not_nrch;
		}
		coremask_chs = iter;
		break;
	}

	if (!coremask_chs) {
		_NT(trace_pcpu_nrch_get_next_coremask_cpu_no_mask, 
		    "Disk @DISK_NAME - Not creating per-cpu channels, no suitable coremasks found",
		    disk->name);
		goto out;
	}

	/* Pick a random cpu from the result */
	rand_cnt = rand_src % NVMEIB_CPU_MASK_WEIGHT(*mask);
	NVMEIB_CPU_MASK_FOR_EACH_CPU(cpu, *mask) {
		if (rand_cnt == 0) {
			next_cpu = cpu;
			break;
		}
		rand_cnt--;
	}
	/* Should have chosen at least 1 cpu */
	BUG_ON(next_cpu < 0);

	/* Not at all safe, but we don't care (it's random) */
	rand_src++;

	/* Rotate coremasks position in list */
	rotate_coremask_in_list(coremask_chs, false);

out:
	__NFOUTD;
	return next_cpu;
}

/* called from admin-wq from start-io-ch before connect-attempt, then caller
   sets nrch->comp_cpu, ask net-layer for a CQ associated to this cpu,
   connect-qp and adds that nrch to corresponding entry in pcpu_nrchs[] */
static int pcpu_nrch_get_next_cpu(struct nvmeibc_disk *disk)
{
	struct nvmeibc_ib_admin_channel *ach;
	int i;
	int next_cpu = -1;
	__NFIND;

	if (!disk->pcpu_nrchs) {
		_NI_dmesg(trace_0_nrch_pcpu_get_next_cpu, "Oops, here but pcpu_nrchs is disabled");
		return -1;
	}
	if (!(ach = get_alive_admin_ch(disk)) || !on_wq(ach->base.remove_wq)) {
		_NI_dmesg(trace_1_nrch_pcpu_get_next_cpu, "Oops, wrong wq, exp @PID",
				  ach ? wq_pid(ach->base.remove_wq) : -1);
		WARN_ON(1);
		return -1;
	}
	if (disk->coremask_support) {
		next_cpu = pcpu_nrch_get_next_coremask_cpu(disk);
		goto out;
	}

	/* As we set pcpu_nrchs[i].nrch only from this ctx, admin-wq &&
	   although we may nullify it from nrch-remove-wq, we test it
	   without a lock - This means that we might see non-NULL right
	   before remove-work nullifies it and thus will miss the chance
	   of connecting this cpu's nrch in this round, well, next time */
	for (i = 0; i < disk->pcpu_nrchs_max_per_disk; i++) {
		BUG_ON(i >= ARRAY_SIZE(disk->info->pcpu_nrchs));
		if (disk->pcpu_nrchs_ll && !cpumask_test_cpu(i, disk->pcpu_nrchs_ll_cpumask))
			continue;
		if (cpu_online(i) && disk->info->pcpu_nrchs[i].nrch == NULL) {
			next_cpu = i;
			break;
		}
	}

	_NI(trace_2_nrch_pcpu_get_next_cpu, "next_cpu=@INT", next_cpu);

out:
	__NFOUTD;
	return next_cpu;
}

static bool pcpu_nrch_add_coremask_ch(struct nvmeibc_disk *disk,
				      struct nvmeibc_ib_nordda_channel *nrch)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *cinfo = dinfo->coremask_info;
	struct nvmeibc_disk_coremask_chs *coremask_chs;
	int cpu = pcpu_nrch_cpu_get(nrch);
	unsigned long flags;
	bool ret = false;

	__NFIND;
	read_lock_irqsave(&cinfo->coremask_chs_lock, flags);
	if ((coremask_chs = cinfo->coremask_chs_per_core[cpu])) {
		/* Get reference to keep coremask alive during this function */
		kref_get(&coremask_chs->refcnt);
	}

	read_unlock_irqrestore(&cinfo->coremask_chs_lock, flags);

	if (!coremask_chs) {
		/* coremask was removed under our feet */
		_NT(trace_pcpu_nrch_add_coremask_ch_no_mask,
		    "Disk @DISK_NAME - Coremask for CPU @CPU was removed under out feet",
			disk->name, cpu);
		goto out;
	}

	if (READ_ONCE(coremask_chs->dying)) {
		/* coremask is dying, nothing to do */
		_NT(trace_pcpu_nrch_add_coremask_ch_dying,
		    "Disk @DISK_NAME - Coremask for CPU @CPU dying",
			disk->name, cpu);
		goto put_ref;
	}

	spin_lock_irqsave(&coremask_chs->spinlock, flags);

	/* Check dying flag under lock */
	if (coremask_chs->dying) {
		/* dying flag set under our feet */
		_NT(trace_pcpu_nrch_add_coremask_ch_dying_2,
		    "Disk @DISK_NAME - Coremask for CPU @CPU dying",
			disk->name, cpu);
		goto unlock_n_put;
	}

	/* Refcount increase for nrch cookie ptr to the coremask */
	kref_get(&coremask_chs->refcnt);
	set_coremask_nrch_cookie(nrch, coremask_chs);
	nvmeib_public_plist_add(&nrch->available_link, &coremask_chs->nrchs_plist);
	NVMEIB_CPU_MASK_SET_CPU(cpu, coremask_chs->nrchs_coremask);
	coremask_chs->n_nrchs++;
	ret = true;
	
	_NT(trace_pcpu_nrch_add_coremask_ch,
	    "Disk @DISK_NAME - Added per-cpu NRCH @NRCH_NAME on CPU @CPU to Coremask @COREMASK_UID",
		disk->name, nrch->base.name, cpu, coremask_chs->uid);

unlock_n_put:
	spin_unlock_irqrestore(&coremask_chs->spinlock, flags);

put_ref:
	/* Put reference keeping coremask alive during this function */
	kref_put(&coremask_chs->refcnt, put_coremask_chs_ref);

out:
	if (ret)
		rotate_coremask_in_list(coremask_chs, false);
	__NFOUTD;
	return ret;
}

/* called from work on pcpu nrch cpu */
static void pcpu_nrch_add(struct nvmeibc_disk *disk,
						  struct nvmeibc_ib_nordda_channel *nrch)
{
	struct nvmeibc_disk_pcpu_nrch *dpn;
	u64 uid;
	unsigned long flags;
	__NFIND;

	_NI(t0_pcpu_nrch_add, "Adding per-cpu nrch for cpu @CPU (@NRCH) - lockless @BOOL_YN",
		  pcpu_nrch_cpu_get(nrch), nrch, disk->pcpu_nrchs_ll);

	/* sanity */
	if (!is_pcpu_nrch(nrch)) {
		_NI_dmesg(e0_pcpu_nrch_add, "Oops, invalid comp-cpu @INT", pcpu_nrch_cpu_get(nrch));
		return;
	}

	/* uid */
	if ((uid = atomic_inc_return(&disk->info->pcpu_nrchs_cnt)) == 0)
		uid = atomic_inc_return(&disk->info->pcpu_nrchs_cnt);

	/* add */
	dpn = &disk->info->pcpu_nrchs[pcpu_nrch_cpu_get(nrch)];
	if (!disk->pcpu_nrchs_ll) {
		spin_lock_irqsave(&dpn->spinlock, flags);
		if (!cpu_online(pcpu_nrch_cpu_get(nrch))) {
			_NE_dmesg(e2_pcpu_nrch_add, "oops, cannot add to pcpu arr, cpu @INT is offline", pcpu_nrch_cpu_get(nrch));
			goto unlock;
		}
	} else {
		BUG_ON(get_cpu() != pcpu_nrch_cpu_get(nrch));
		local_irq_save(flags);
	}
	if (dpn->nrch != NULL) {
		 _NE_dmesg(e3_pcpu_nrch_add, "disk @DISK_NAME, oops, cannot add to pcpu arr, cpu @INT already has nrch", disk->name, pcpu_nrch_cpu_get(nrch));
		 BUG();
	 }
	 else {
		 dpn->nrch = nrch;
		 dpn->uid = uid;
		 nrch->pcpu_nrch_uid = uid;
	 }

unlock:
	 if (!disk->pcpu_nrchs_ll) {
		spin_unlock_irqrestore(&dpn->spinlock, flags);

		if (disk->coremask_support) {
			if (!pcpu_nrch_add_coremask_ch(disk, nrch)) {
				/* Could not find coremask for channel - disconnect it */
				nvmeibc_ib_nordda_channel_try_disconnect(nrch);
			}
		}
	 } else {
		local_irq_restore(flags);
		put_cpu();
	 }

	__NFOUTD;
}

static void pcpu_nrch_del_coremask_ch(struct nvmeibc_disk *disk,
				      struct nvmeibc_ib_nordda_channel *nrch,
				      struct list_head *pend_abort_list)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeibc_disk_coremask_chs *coremask_chs, *iter;
	int cpu = pcpu_nrch_cpu_get(nrch);
	unsigned long flags;
	LIST_HEAD(pending_cmds);
	struct nvmeibc_disk_command *disk_cmd;
	bool was_set;

	__NFIND;
	coremask_chs = get_coremask_nrch_cookie(nrch);

	BUG_ON(!coremask_chs);

	spin_lock_irqsave(&coremask_chs->spinlock, flags);
	
	_NT(trace_pcpu_nrch_del_coremask_ch,
	    "Disk @DISK_NAME - Removing per-cpu NRCH @NRCH_NAME on CPU @CPU from Coremask @COREMASK_UID",
		disk->name, nrch->base.name, cpu, coremask_chs->uid);

	/* Remove nrch from coremask */
	set_coremask_nrch_cookie(nrch, NULL);
	nvmeib_public_plist_del(&nrch->available_link, &coremask_chs->nrchs_plist);
	was_set = NVMEIB_CPU_MASK_TEST_AND_CLEAR_CPU(cpu, coremask_chs->nrchs_coremask);
	BUG_ON(!was_set);

	BUG_ON(coremask_chs->n_nrchs < 1);
	coremask_chs->n_nrchs--;
	
	if (!coremask_chs->n_nrchs) {
		/* No channels at the moment, remove all the pending cmds
		 * and hand them off to the any-core channels */
		_NT(trace_pcpu_nrch_del_coremask_ch_last,
			"Disk @DISK_NAME - Last NRCH removed from Coremask @COREMASK_UID. "
			"Draining pending",
			disk->name, coremask_chs->uid);
		list_splice_init(&coremask_chs->pending_cmds, &pending_cmds);
	}

	spin_unlock_irqrestore(&coremask_chs->spinlock, flags);

	/* Put reference from nrch coremask cookie */
	if (kref_put(&coremask_chs->refcnt, put_coremask_chs_ref)) {
		/* coremask was destroyed */
		goto drain_pending;
	}
	
	/* Rotate coremask in list, note it may not be in the list anymore, so we need to check that first */
	write_lock_irqsave(&info->coremask_chs_lock, flags);
	list_for_each_entry(iter, &info->coremask_chs, link) {
		if (iter == coremask_chs) {
			rotate_coremask_in_list(coremask_chs, true);
			break;
		}
	}
	write_unlock_irqrestore(&info->coremask_chs_lock, flags);

drain_pending:
	/* Hand off any pending cmds to the any-core channels */
	while ((disk_cmd = list_first_entry_or_null(&pending_cmds, struct nvmeibc_disk_command, dcmd_link))) {
		list_del(&disk_cmd->dcmd_link);
		if (execute_io_remote(disk, disk_cmd) < 0) {
			list_add(&disk_cmd->dcmd_link, &pending_cmds);
			break;
		}
	}
	/* Any remaining pending cmds should be aborted by caller */
	list_splice_tail(&pending_cmds, pend_abort_list);

	__NFOUTD;
}

/* called from admin-wq or ch's remove-wq */
static void pcpu_nrch_del(struct nvmeibc_disk *disk,
			struct nvmeibc_ib_nordda_channel *nrch,
			struct list_head *pend_list)
{
	struct nvmeibc_disk_pcpu_nrch *dpn;
	unsigned long flags;
	__NFIND;

	_NI(t0_pcpu_nrch_del, "Deleting nrch to cpu @INT (ch=@PTR, uid=@LLU)",
			  pcpu_nrch_cpu_get(nrch), nrch, nrch->pcpu_nrch_uid);

	/* sanity */
	if (!is_pcpu_nrch(nrch)) {
		_NI_dmesg(e0_pcpu_nrch_del, "Oops, invalid comp-cpu @INT", pcpu_nrch_cpu_get(nrch));
		return;
	}
	BUG_ON(is_ll_pcpu_nrch(nrch) && smp_processor_id() != pcpu_nrch_cpu_get(nrch));

	/* delete */
	dpn = &disk->info->pcpu_nrchs[pcpu_nrch_cpu_get(nrch)];
	if (!disk->pcpu_nrchs_ll) {
		spin_lock_irqsave(&dpn->spinlock, flags);
	} else {
		BUG_ON(get_cpu() != pcpu_nrch_cpu_get(nrch));
		local_irq_save(flags);
	}
	if (dpn->nrch == NULL) {
		 _NE_dmesg(e2_pcpu_nrch_del, "disk @DISK_NAME - oops, cannot del to pcpu arr, cpu @INT has no nrch",
			   disk->name, pcpu_nrch_cpu_get(nrch));
	 }
	 else {
		 BUG_ON(dpn->nrch != nrch);
		 list_splice_init(&dpn->pending_disk_cmds, pend_list);
		 dpn->n_pending = 0;
		 dpn->max_pending = 0;
		 dpn->nrch = NULL;
		 dpn->uid = 0;
		 nrch->pcpu_nrch_uid = 0;
	 }
	 if (!disk->pcpu_nrchs_ll) {
		spin_unlock_irqrestore(&dpn->spinlock, flags);
		if (is_coremask_nrch(nrch)) {
			BUG_ON(!disk->coremask_support);
			pcpu_nrch_del_coremask_ch(disk, nrch, pend_list);
		}
	 } else {
		local_irq_restore(flags);
		put_cpu();
	 }

	__NFOUTD;
}

static int pcpu_nrch_get_coremask_channel(struct nvmeibc_disk *disk,
					  struct nvmeibc_disk_command *disk_cmd,
					  struct nvmeibc_channel **ch,
					  void **context)
{
	const struct nvmeib_cpu_mask_info *cmd_coremask_info = disk_cmd->cpu_mask_info;
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *cinfo = dinfo->coremask_info;
	struct nvmeibc_disk_coremask_chs *coremask_chs;
	struct nvmeibc_ib_nordda_channel *nrch;
	struct nvmeibc_volume_req_info *ri;
	struct nvmeibc_disk_coremask_pcpu_stats __percpu *disk_pcpu_stats;
	unsigned long flags;
	bool found = false;
	int cpu = NVMEIB_CPU_MASK_NEXT(-1, cmd_coremask_info->mask);
	int rv;
	__NFIND;

	read_lock_irqsave(&cinfo->coremask_chs_lock, flags);
	disk_pcpu_stats = this_cpu_ptr(cinfo->pcpu_stats);
	COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_op);
	if (!NVMEIB_CPU_MASK_TEST_CPU(smp_processor_id(), cmd_coremask_info->mask))
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_submit_cpu_not_in_mask);
	if (!(coremask_chs = cinfo->coremask_chs_per_core[cpu]) || READ_ONCE(coremask_chs->dying)) {
		if (!coremask_chs)
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_not_exist);
		else
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_dying);
		read_unlock_irqrestore(&cinfo->coremask_chs_lock, flags);
		rv = -1;
		goto out;
	}
	kref_get(&coremask_chs->refcnt);
	read_unlock_irqrestore(&cinfo->coremask_chs_lock, flags);

	spin_lock_irqsave(&coremask_chs->spinlock, flags);
	disk_pcpu_stats = this_cpu_ptr(cinfo->pcpu_stats);
	/* Check dying flag again that we are under lock */
	if (coremask_chs->dying) {
		rv = -1;
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_dying);
		coremask_chs->n_io_mask_dying++;
		goto unlock_and_put_ref;
	}
	/* Check uid against sticky IO coremask */
	if (coremask_chs->uid != cmd_coremask_info->gen) {
		rv = -1;
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_uid_mismatch);
		coremask_chs->n_io_mask_uid_mismatch++;
		goto unlock_and_put_ref;
	}
	if (plist_head_empty(&coremask_chs->nrchs_plist)) {
		/* If there are no channels, instead of adding to the pending list
		 * where the IO can be stuck for a long time, return error to the caller
		 * and it will fallback to the any-core channels
		 * TBD: Stats
		 */
		rv = -1;
		coremask_chs->n_io_mask_no_nrch++;
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_no_nrch);
		goto unlock_and_put_ref;
	}
	plist_for_each_entry(nrch, &coremask_chs->nrchs_plist, available_link) {
		if ((ri = nvmeibc_ib_nordda_channel_get_io_context(nrch))) {
			*ch = &nrch->base;
			*context = ri;
			/* Rotate nrch within same priority band (matches any-core plist RR) */
			nvmeib_public_plist_requeue(&nrch->available_link, &coremask_chs->nrchs_plist);
			found = true;
			coremask_chs->n_io_mask_chan++;
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_nrch);
			break;
		}
	}
	if (!found) {
		if (!nvmeibc_disk_use_coremask_pending) {
			rv = -1;
			coremask_chs->n_io_mask_nrch_busy++;
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_nrch_busy);
			goto unlock_and_put_ref;
		}
		/* Failed to find available request from any channel - Add to coremask pending */
		nvmeibc_disk_cmds_stats_pending_add(disk, disk_cmd);
		nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_REMOTE_ADD_PENDING);
		list_add_tail(&disk_cmd->dcmd_link, &coremask_chs->pending_cmds);
		coremask_chs->n_pending++;
		coremask_chs->max_pending = max(coremask_chs->max_pending, coremask_chs->n_pending);
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_pending_push);
		COREMASK_PCPU_STAT_MAX(disk_pcpu_stats, max_io_coremask_pending, coremask_chs->max_pending);
		*ch = NULL;
		*context = NULL;
	}
	rv = 0;
	
unlock_and_put_ref:
	spin_unlock_irqrestore(&coremask_chs->spinlock, flags);
	kref_put(&coremask_chs->refcnt, put_coremask_chs_ref);

out:
	__NFOUTD;
	return rv;
}

/* called from execute-io-remote
   if pcpu-nrch can handle this cmd now/later return 0 and valid/NULL @ch and @context,
   respectively, otherwise return error (-1) so caller can fallback to legacy (any-cpu) channels */
static int pcpu_nrch_get_channel(struct nvmeibc_disk *disk,
								 struct nvmeibc_disk_command *disk_cmd,
								 struct nvmeibc_channel **ch,
								 void **context)
{
	int cpu;
	struct nvmeibc_disk_pcpu_nrch *dpn = NULL;
	struct nvmeibc_ib_nordda_channel *nrch;
	struct nvmeibc_volume_req_info *ri;
	unsigned long flags;
	int rv;
	__NFIND;

	/* Assume we dont really need to check dying under disk's lock */
	if (atomic_read(&disk->dying)) {
		//nvmeibc_disk_cmds_stats_direct_exec_err(disk, disk_cmd);
		_NT(t0_pcpu_nrch_get_channel, "Disk is dying - leave");
		rv = -1;
		goto out;
	}
	
	if (disk->coremask_support) {
		if (disk_cmd->cpu_mask_info && disk_cmd->cpu_mask_info->gen != 0) {
			rv = pcpu_nrch_get_coremask_channel(disk, disk_cmd, ch, context);
			goto out;
		}
		else {
			/* Increment counter for not coremask io */
			struct nvmeibc_disk_coremask_pcpu_stats __percpu *disk_pcpu_stats;
			unsigned long flags;

			local_irq_save(flags);
			disk_pcpu_stats = this_cpu_ptr(disk->info->coremask_info->pcpu_stats);
			COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_not_coremask_op);
			local_irq_restore(flags);
		}
	}

	/* protect from concurrent remove-work which may run on another cpu and
	   remove ch from pcpu_nrchs[] after we referenced it but before we run
	   get-io-context and hence remove-work marches on, kfree ch, while we
	   use the req */
	if (!disk->pcpu_nrchs_ll) {
		cpu = smp_processor_id() % disk->pcpu_nrchs_max_per_disk;
		dpn = &disk->info->pcpu_nrchs[cpu];
		spin_lock_irqsave(&dpn->spinlock, flags);
	} else {
		cpu = get_cpu();
		local_irq_save(flags);
		if (cpu >= disk->pcpu_nrchs_max_per_disk) {
			rv = -ENOENT;
			goto unlock;
		}
		dpn = &disk->info->pcpu_nrchs[cpu];
	}
	nrch = dpn->nrch;
	if (nrch == NULL) {
		_NW(t2_pcpu_nrch_get_channel,
				  "No pcpu-nrch for cpu @CPU, "
				  "fallback to per-disk chs or pending", cpu);
		nvmeibc_disk_counters_inc(disk, n_err_nrch_pcpu_lookup_failed);
		rv = -1;
	}
	else {
		if (!(ri = nvmeibc_ib_nordda_channel_get_io_context(nrch))) {
			_ND(t3_pcpu_nrch_get_channel,
					  "pcpu-nrch on cpu @CPU has no free req, "
					  "add to its pending list", cpu);
			nvmeibc_disk_cmds_stats_pending_add(disk, disk_cmd);
			list_add_tail(&disk_cmd->dcmd_link, &dpn->pending_disk_cmds);
			dpn->n_pending++;
			dpn->max_pending = max(dpn->max_pending, dpn->n_pending);
			*ch = NULL;
			*context = NULL;
		}
		else {
			*ch = &nrch->base;
			*context = ri;
		}

		/* pcpu-nrch will handle this cmd */
		rv = 0;
	}
unlock:
	if (!disk->pcpu_nrchs_ll) {
		spin_unlock_irqrestore(&dpn->spinlock, flags);
	} else {
		local_irq_restore(flags);
		put_cpu();
	}

out:
	__NFOUTD;
	return rv;
}

static struct nvmeibc_disk_command *pcpu_nrch_coremask_pending_cmd_get(
	struct nvmeibc_disk *disk, struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeibc_volume_req_info *req,
	void (*put_req_fn)(struct nvmeibc_ib_nordda_channel *ch, struct nvmeibc_volume_req_info *req))
{
	struct nvmeibc_disk_coremask_chs *coremask_chs;
	struct nvmeibc_disk_command *disk_cmd = NULL;
	struct nvmeibc_disk_coremask_pcpu_stats __percpu *disk_pcpu_stats;
	unsigned long flags;

	__NFIND;
	coremask_chs = get_coremask_nrch_cookie(ch);
	BUG_ON(!coremask_chs);

	spin_lock_irqsave(&coremask_chs->spinlock, flags);
	disk_pcpu_stats = this_cpu_ptr(disk->info->coremask_info->pcpu_stats);

	/* Sanity check */
	BUG_ON(!NVMEIB_CPU_MASK_TEST_CPU(get_coremask_nrch_cpu(ch), coremask_chs->nrchs_coremask));

	/* Get cmd from coremask pending list (if not empty) */
	if (!list_empty(&coremask_chs->pending_cmds)) {
		disk_cmd = list_first_entry(&coremask_chs->pending_cmds, struct nvmeibc_disk_command, dcmd_link);
		list_del(&disk_cmd->dcmd_link);
		BUG_ON(coremask_chs->n_pending == 0);
		coremask_chs->n_pending--;
		coremask_chs->n_io_mask_chan++;
		COREMASK_PCPU_STAT_INC(disk_pcpu_stats, n_io_coremask_pending_pop);
	}

	spin_unlock_irqrestore(&coremask_chs->spinlock, flags);

	if (!disk_cmd && put_req_fn)
		(*put_req_fn)(ch, req);

	__NFOUTD;
	return disk_cmd;
}

/* called from nordda-pending-io while we already have refcnt for @req */
struct nvmeibc_disk_command *nvmeibc_disk_pcpu_nrch_pending_cmd_get(
	struct nvmeibc_disk *disk, struct nvmeibc_ib_nordda_channel *ch,
	u64 version, struct nvmeibc_volume_req_info *req,
	void (*put_req_fn)(struct nvmeibc_ib_nordda_channel *ch, struct nvmeibc_volume_req_info *req))
{
	struct nvmeibc_disk_pcpu_nrch *dpn;
	struct nvmeibc_disk_command *disk_cmd = NULL;
	int nrch_cpu, cpu;
	unsigned long flags;
	__NFIND;

	BUG_ON(!is_pcpu_nrch(ch));

	if (is_coremask_nrch(ch)) {
		BUG_ON(!disk->coremask_support);
		disk_cmd = pcpu_nrch_coremask_pending_cmd_get(disk, ch, req, put_req_fn);
		goto out;
	}

	nrch_cpu = pcpu_nrch_cpu_get(ch);
	dpn = &disk->info->pcpu_nrchs[nrch_cpu];
	if (!disk->pcpu_nrchs_ll) {
		spin_lock_irqsave(&dpn->spinlock, flags);
		cpu = smp_processor_id() % disk->pcpu_nrchs_max_per_disk;
		if (nrch_cpu != cpu) {
			struct nvmeibc_ib_admin_channel *ach;
			/* can happen on start-io-ch calling execute_pending_io */
			if (!(ach = get_alive_admin_ch(disk)) || !on_wq(ach->base.remove_wq)) {
				_NE_dmesg(t1_pcpu_nrch_pending_cmd_get,
					  "Oops, pcpu-nrch received comp on unexp cpu @INT (@INT), "
					  "will have to acquire lock of another cpu (perf hit; ach=@PTR)",
					  nrch_cpu, cpu, ach);
				WARN_ON_ONCE(1);
			}
		}
	} else {
		cpu = get_cpu();
		if (nrch_cpu != cpu) {
			_NE_dmesg(e1_pcpu_nrch_pending_cmd_get,
				  "Oops, wrong cpu for lockless per-cpu nrch - @CPU (not @CPU) (ch=@NRCH, uid=@LLU)",
				  cpu, nrch_cpu, ch, ch->pcpu_nrch_uid);
			BUG();
		}
		local_irq_save(flags);
	}

	if (dpn->nrch != ch || dpn->uid != ch->pcpu_nrch_uid) {
		_NE_dmesg(e0_pcpu_nrch_pending_cmd_get,
			  "Oops, wrong nrch for cpu @CPU (ch=@NRCH/@NRCH, uid=@LLU/@LLU)",
				  nrch_cpu, dpn->nrch, ch, dpn->uid, ch->pcpu_nrch_uid);
		BUG();
	}

	if (atomic_read(&ch->net.base.dying) ||
		atomic_read(&ch->base.dying) ||
		atomic_read(&disk->dying)) {
		_NT(t0_pcpu_nrch_pending_cmd_get,
			"dying: n=@INT, c=@INT, d=@INT",
			atomic_read(&ch->net.base.dying),
			atomic_read(&ch->base.dying),
			atomic_read(&disk->dying));
		goto done;
	}

	//TBD: To support EC rcookie use and check @version
	if ((disk_cmd = list_first_entry_or_null(
		&dpn->pending_disk_cmds,
		struct nvmeibc_disk_command, dcmd_link))) {
		list_del_init(&disk_cmd->dcmd_link);
		dpn->n_pending--;
	}

done:
	if (!disk_cmd && put_req_fn) {
		/* atomic w/ adding to pcpu-nrch pending as
		   otherwise if we unlock and then put req,
		   a new cmd may arrive in btw and will have
		   no free req and stay in pending list either
		   for long or for good (in the crazy case
		   where this happen simultaneity on all reqs) */
		_ND(t2_pcpu_nrch_pending_cmd_get,
			"Putting req back...@IDX for info=@INFO_PTR", req->idx, disk->info);
		/* put_req_info */
		(*put_req_fn)(ch, req);
	}
	if (!disk->pcpu_nrchs_ll) {
		spin_unlock_irqrestore(&dpn->spinlock, flags);
	} else {
		local_irq_restore(flags);
		put_cpu();
	}

out:
	__NFOUTD;
	return disk_cmd;
}

static void pcpu_nrch_check_empty_pcpu_fn(void *ctx)
{
	struct nvmeibc_disk *disk = ctx;
	int cpu = get_cpu();
	if (disk->info->pcpu_nrchs[cpu].nrch) {
		_NE_dmesg(t0_pcpu_nrch_check_empty_pcpu_fn,
			"oops cpu=@INT (@INT/@INT) still has nrch=@PTR",
			  cpu, disk->pcpu_nrchs_max_per_disk, NVMEIB_DFLT_MAX_CPUS,
			disk->info->pcpu_nrchs[cpu].nrch);
	}

	/* How? This func is called after we've pause-ack from ulp */
	if (!list_empty(&disk->info->pcpu_nrchs[cpu].pending_disk_cmds)) {
		_NE_dmesg(t1_pcpu_nrch_check_empty_pcpu_fn,
			"oops cpu=@INT (@INT/@INT) still has (@INT/@INT) pending-cmds",
			cpu, disk->pcpu_nrchs_max_per_disk, NVMEIB_DFLT_MAX_CPUS,
			disk->info->pcpu_nrchs[cpu].n_pending,
			disk->info->pcpu_nrchs[cpu].max_pending);
		BUG();
	}
	put_cpu();
}

static void pcpu_nrch_coremask_check_empty(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_info *dinfo = disk->info;
	struct nvmeibc_disk_coremask_info *info = dinfo->coremask_info;
	struct nvmeibc_disk_coremask_chs *entry;
	__NFIND;
	while ((entry = list_first_entry_or_null(&info->coremask_chs, struct nvmeibc_disk_coremask_chs, link))) {
		if (!plist_head_empty(&entry->nrchs_plist)) {
			_NE_dmesg(err_pcpu_nrch_coremask_check_empty_still_nrch,
				  "oops coremask @PTR from disk @DISK_NAME (@PTR) still has nrchs",
				  entry, disk->name, disk);
			BUG();
		}
		if (!list_empty(&entry->pending_cmds)) {
			_NE_dmesg(err_pcpu_nrch_coremask_check_empty_still_pending,
				  "oops coremask @PTR from disk @DISK_NAME (@PTR) still has pending cmds",
				  entry, disk->name, disk);
			BUG();
		}
		list_del(&entry->link);
		if (!kref_put(&entry->refcnt, put_coremask_chs_ref)) {
			_NE_dmesg(err_pcpu_nrch_coremask_check_empty_still_ref,
				  "oops coremask @PTR from disk @DISK_NAME (@PTR) still has outstanding refs",
				  entry, disk->name, disk);
			BUG();
		}
	}
	__NFOUTD;
}

static void pcpu_nrch_check_empty(struct nvmeibc_disk *disk)
{
	__NFIND;

	if (disk->info) {
		if (!disk->pcpu_nrchs_ll) {
			int n = ARRAY_SIZE(disk->info->pcpu_nrchs);
			int cpu;
			for (cpu = 0; cpu < n; cpu++) {
				if (disk->info->pcpu_nrchs[cpu].nrch) {
					_NE_dmesg(t0_nrch_pcpu_reset,
						  "oops cpu=@INT (@INT/@INT) still has nrch=@PTR",
							cpu, disk->pcpu_nrchs_max_per_disk, n,
							disk->info->pcpu_nrchs[cpu].nrch);
				}

				/* How? This func is called after we've pause-ack from ulp */
				if (!list_empty(&disk->info->pcpu_nrchs[cpu].pending_disk_cmds)) {
					_NE_dmesg(t1_nrch_pcpu_reset,
						  "oops cpu=@INT (@INT/@INT) still has pending-cmds",
						  cpu, disk->pcpu_nrchs_max_per_disk, n);
					BUG();
				}
			}
			if (disk->coremask_support) {
				/* Also frees disk->info->coremask_info */
				pcpu_nrch_coremask_check_empty(disk);
			}
		} else {
			/* per-cpu are lock-less, need to schedule on each of their cpus */

			/* From the kernel doc:
			* 	"You must not call this function with disabled interrupts or from a hardware interrupt handler or from a bottom half handler."
			*/
			BUG_ON(irqs_disabled() || in_interrupt());

			on_each_cpu_mask(disk->pcpu_nrchs_ll_cpumask, pcpu_nrch_check_empty_pcpu_fn, disk, true);
		}
	}

	__NFOUTD;
}

/* TODO pcpu-nrch:
   o See comment in first commit of EC-7710 (Change-Id: If4c1714c322a37eb2f1fbe4c5f57d0b1328982d0)
   o Support disk's "cmds" /proc file for pcpu-nrch's mainly pending list...
   o Add limit to pcp pending-list's depth - may cause slow rebuilds due to not prioritizing recovery GEN and IO cmds [EC-7094]
   o Do not limit disk->info->pcpu_nrchs to 128, allocate it w/ alloc_percpu
   o Support this with nr_defer_recv_comps_tcp=1, will it conflict with avail_nordda_for_cpu[]
   o let nvmeibc_disk_create_remote() kzalloc disk->pcpu_nrchs_max_per_disk entries ?
   o In order to use all cores when 0 < nvmeibc_nr_pcpu_channels_per_disk < num-cpus, let each disk start have different base-cpu-idx
   o Compensate for offline CPUs witnin this configured cpus-range
*/
