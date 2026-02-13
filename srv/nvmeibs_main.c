/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibs_main.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_ib_port.h"
#include "nvmeibs_client.h"
#include "nvmeibs_types.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_disk.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeib_rdma.h"
#include "nvmeib_utils.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib.h"
#include "nvmeib_public.h"
#include "nvmeib_msgloop.h"
#include "nvmeibs_toma.h"
#include "nvmeib_srq.h"
#include "nvmeibs_mcs.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib_public.h"
#include "nvmeibs_serjio.h"
#include "nvmeibs_serjio_gen_cmd_handlers.h"
#include "nvmeibs_um_comm.h"
#include "nvmeibs_async_cookies.h"
#include "nvmeib_io_stats.h"
#include "common/proc_epilog.h"
#include "nvmeibs_memmgr_metrics.h"
#include "nvmeibs_nordda.h"
MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("NVMe storage device over Infiniband");
MODULE_LICENSE("GPL and additional rights");

#define DRV_VERSION "0.0.1"
const char nvmeibs_driver_version[] = DRV_VERSION;
char nvmeibs_node_name[NVMEIB_HOST_NAME_LEN];

#define PROCFS_COMMON_STR "nvmeibs"

#define NVMEIBS_CLIENTS_PROC

int nvmeibs_debug_level = 1;
int tracer_nvmeibs_debug_level = 4;	// Equivalent to: nvmeibc_debug_level = 2
int goodpath_nvmeibs_debug_level = 2;

module_param_named(debug_level, nvmeibs_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Enables debug logging (to the system log not NVMesh tracer) if set above 1. Deprecated.");

module_param_named(tracer_debug_level, tracer_nvmeibs_debug_level, int, 0644);
MODULE_PARM_DESC(tracer_debug_level, "This determines the level of tracing for this module. Only traces with this level or lower will be issued, see tracer severities above.");

module_param_named(goodpath_debug_level, goodpath_nvmeibs_debug_level, int, 0644);
MODULE_PARM_DESC(goodpath_debug_level, "This determines the level of tracing for the regular data path. Only traces with this level or lower will be issued, see tracer severities above.");

#define MAX_FP 1024
static char nvmeibs_filter_ports[MAX_FP] = "";
module_param_string(ports, nvmeibs_filter_ports, MAX_FP, 0644);
MODULE_PARM_DESC(ports, "Used for port filtering functionality. This is typically set by service startup based on nvmesh.conf information.");

static char nvmeibs_filter_guids[MAX_FP] = "";
module_param_string(guids, nvmeibs_filter_guids, MAX_FP, 0644);
MODULE_PARM_DESC(guids, "Used for port filtering functionality. Typically populated from nvmesh.conf parameters.");

bool nvmeibs_defer_recv_comps = true;
module_param_named(defer_recv_comps, nvmeibs_defer_recv_comps, bool, 0644);
MODULE_PARM_DESC(defer_recv_comps, "Defer handling of IO receive completions, so it is not done in the interrupt context.");

bool nvmeibs_defer_recv_comps_tcp = false;
module_param_named(defer_recv_comps_tcp, nvmeibs_defer_recv_comps_tcp, bool, 0644);
MODULE_PARM_DESC(defer_recv_comps_tcp, "Same as defer_recv_comps, but applied for TCP/SIW NICs.");

unsigned nvmeibs_max_nic_srqs = NVMEIB_MAX_NIC_SRQS;
module_param_named(max_nic_srqs, nvmeibs_max_nic_srqs, int, 0444);
MODULE_PARM_DESC(max_nic_srqs, "Maximum number of shared receive queues to define per NIC.");

static bool roce_ipv4_only = false;
module_param_named(roce_ipv4_only, roce_ipv4_only, bool, 0444);
MODULE_PARM_DESC(roce_ipv4_only, "Deprecated. Use IPv4 only for RoCE, which was needed for CX-3.");

bool nvmeibs_use_pcpu_cq = false; /* Disabled by service script for TCP */
module_param_named(use_pcpu_cq, nvmeibs_use_pcpu_cq, bool, 0444);
MODULE_PARM_DESC(use_pcpu_cq, "Use a per-cpu shared completion queue (SCQ) and shared receive queue (SRQ).");

bool nvmeibs_pcpu_cq_poll_proc = false;
module_param_named(pcpu_cq_poll_proc, nvmeibs_pcpu_cq_poll_proc, bool, 0444);
MODULE_PARM_DESC(pcpu_cq_poll_proc, "Create /proc files for polling the nvmeibs shared completion queues from SPDK. This requires pcpu_cq_all_cpus=Y for nvmeib_common.");

unsigned max_outstanding_cm_work_items = 8;
module_param(max_outstanding_cm_work_items, uint, 0644);
MODULE_PARM_DESC(max_outstanding_cm_work_items, "Max outstanding CM (connection manager) work items. Important for larger environments using RDMA.");

unsigned int nvmeibs_tcp_mode = 0;
module_param_named(tcp_mode, nvmeibs_tcp_mode, uint, 0444);
MODULE_PARM_DESC(tcp_mode, "Activate the SIW communicate mode exclusively, i.e., filter out any RoCE devices. Usually set by service startup from nvmesh.conf information.");

bool nvmeibs_use_tcp_locks = false;
static void set_lock_dev_mode(void)
{
	/* If TCP is enabled, s-disk has no dedicated lock-device, allow
	   lock channel to connect (and issue locks) from any target-nic */
	nvmeibs_use_tcp_locks = !!nvmeibs_tcp_mode;
}

bool nvmeibs_local_skip_disk_access = false;
module_param_named(local_skip_disk_access, nvmeibs_local_skip_disk_access, bool, 0644);
MODULE_PARM_DESC(local_skip_disk_access, "Unsafe debug mode. Skip local disk access, i.e., complete disk operations immediately instead of performing them. Used for debugging and performance optimization.");

unsigned nvmeibs_nic_io_stats_block_size = 1 << NVMEIBC_SECTOR_SHIFT;
module_param_named(nic_io_stats_block_size, nvmeibs_nic_io_stats_block_size, uint, 0444);
MODULE_PARM_DESC(nic_io_stats_block_size, "Defines the block-size to use for NIC iostats.json.");

NVMEIBS_MEMMGR_METRIC(s_dev_srq, "component=target.dev.srq");
NVMEIBS_MEMMGR_METRIC(s_dev_fr_pool, "component=target.dev.fr_pool");

NVMEIB_DECLARE_KERNEL_WARNINGS_TRAP;

/* stores the list of devices and ports to use */
static struct list_head used_dev_list;

/* indicator for a completion of disk scanning */
static bool disk_scan_done_called = false;

static bool nvmeibs_exit_called = false;

int nvmeib_debug_level(void)
{
	return nvmeibs_debug_level;
}

static bool nvmeibs_serial_console_flag = false;
bool nvmeib_serial_console(void)
{
	return nvmeibs_serial_console_flag;
}

const char *nvmeibs_device_name(struct nvmeibs_dev *dev)
{
	return nvmeib_device_name(dev->dev);
}

/* globals */
/* main user action guard */
static DEFINE_MUTEX(guard);
/* the main workqueue - everything starts here */
struct workq_struct *main_wq;
int main_wq_pid;
/* List of nvmeibs_dev structures. */
static LIST_HEAD(nvmeibs_dev_list);
static LIST_HEAD(nvmeibs_unused_dev_list);
enum rdma_link_layer nvmeibs_selected_layer = IB_LINK_LAYER_UNSPECIFIED;
EXPORT_SYMBOL(nvmeibs_selected_layer);
static int nvmeibs_dev_count;
static int nvmeibs_unused_dev_count;
/* Protects nvmeibs_dev_list. */
static DEFINE_MUTEX(nvmeibs_dev_guard);
/* Protects nvmeibs_cm_id_hash. */
static DEFINE_SPINLOCK(nvmeibs_cmid_lock);
/* client cmid hash:
   the number of cm_id is calculated as follow:
   N_clients * N_disks * N_reqs_per_disk
   */
static HLIST_HEAD(nvmeibs_cmid_hash);
static atomic64_t client_uid;
static unsigned nvmeibs_max_pages_per_mr = NVMEIBS_MAX_BOUNCE_BUFFER_PAGES;
/* RoCE listener */
static struct nvmeib_rdma_cm *roce_cm = NULL;
/* main kth thread for usermode communication */
static struct nvmeibs_um_comm *um_comm;

struct nvmeibs_um_comm * nvmeibs_get_um_comm(void)
{
	return um_comm;
}

/* params */
static u64 nvmeibs_service_guid;

struct nvmeib_intr_shaper *s_intr_shaper;

#if KS_MODULE_PARAM_CB
static int set_u64_x(const char *val, const struct kernel_param *kp)
#else
int nvmeibs_set_service_guid_param(const char *val, struct kernel_param *kp)
#endif
{
	u64 tmp;
	int res;

	if (!(res = kstrtou64(val, 0, &tmp)))
		*(u64 *)kp->arg = tmp;

	return res;
}

#if KS_MODULE_PARAM_CB
static int get_u64_x(char *buffer, const struct kernel_param *kp)
#else
int nvmeibs_get_service_guid_param(char *buffer, struct kernel_param *kp)
#endif
{
	return sprintf(buffer, "0x%016llx", *(u64 *)kp->arg);
}

#if KS_MODULE_PARAM_CB
static struct kernel_param_ops nvmeibs_service_guid_params = {
	.set = set_u64_x,
	.get = get_u64_x,
};

module_param_cb(service_guid, &nvmeibs_service_guid_params,
	&nvmeibs_service_guid, S_IRUGO | S_IWUSR);
#else
module_param_call(service_guid, nvmeibs_set_service_guid_param,
	nvmeibs_get_service_guid_param, &nvmeibs_service_guid, S_IRUGO | S_IWUSR);
#endif

MODULE_PARM_DESC(service_guid, "Override cm_listen_id with this value.");

u64 nvmeibs_get_service_guid(void)
{
	return nvmeibs_service_guid;
}

static int nvmeibs_max_req_size = roundup_pow_of_two(NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE);
module_param_named(max_req_size, nvmeibs_max_req_size, int, 0444);
MODULE_PARM_DESC(max_req_size, "Maximum size of client-target messages.");

int nvmeibs_get_max_req_size(void)
{
	return nvmeibs_max_req_size;
}

int nvmeibs_get_max_pages_in_fmr(void)
{
	return nvmeibs_max_pages_per_mr;
}

/* shared receive queue size or in plain english how many qp we are going use.
   the shared receive queue is only for management messages */
static int nvmeibs_shared_rq_size =
	(NVMEIBS_DEF_MAX_N_CLIENTS *
	NVMEIBS_DEF_MAX_N_DISKS *
	NVMEIB_MAX_NORDDA_IO_REQ) >> 2;
module_param_named(shared_rq_size, nvmeibs_shared_rq_size, int, 0644);
MODULE_PARM_DESC(shared_rq_size, "Networking shared receive queue (SRQ) size.");

int nvmeibs_get_shared_recv_queue_size(void)
{
	return nvmeibs_shared_rq_size;
}

static unsigned int nvmeibs_nordda_io_req_num = NVMEIB_MAX_NORDDA_IO_REQ;
module_param_named(nvmeibs_nordda_io_req_num, nvmeibs_nordda_io_req_num, int, 0644);
MODULE_PARM_DESC(nvmeibs_nordda_io_req_num, "Number of IO requests per IO channel. More can increase throughput, but may hurt caching. Less reduces memory consumption.");

uint nvmeibs_nr_max_wrs_per_req = 0;
module_param_named(nr_max_wrs_per_req, nvmeibs_nr_max_wrs_per_req, int, 0644);
MODULE_PARM_DESC(nr_max_wrs_per_req, "The maximum number of WRs (RDMA work requests) per IO channel request, used in response to a read request. For 0, use system's default.");

u32 nvmeibs_get_nordda_io_req_num(void)
{
	if (!nvmeibs_nordda_io_req_num || (nvmeibs_nordda_io_req_num > NVMEIB_MAX_NORDDA_IO_REQ)) {
		_NE(error_main_nvmeibs_get_nordda_io_req_num, "Override invalid module-param value @NVMEIBS_NORDDA_IO_REQ_NUM to default (@NVMEIBS_NORDDA_IO_REQ_NUM)",
		   nvmeibs_nordda_io_req_num, NVMEIB_MAX_NORDDA_IO_REQ);
		nvmeibs_nordda_io_req_num = NVMEIB_MAX_NORDDA_IO_REQ;
	}
	return nvmeibs_nordda_io_req_num;
}

u32 nvmeibs_get_nordda_max_wrs_per_req(void)
{
	return nvmeibs_nr_max_wrs_per_req;
}

#if 0
/**
#	define MAX_NUMBER_DISK_QUEUES 128
#	define MAX_FMR_POOL_SIZE (NVMEIBS_DEF_MAX_N_CLIENTS * \
	NVMEIBS_DEF_MAX_N_DISKS * \
	NVMEIB_MAX_NORDDA_IO_REQ + \
	NVMEIBS_DEF_MAX_N_DISKS * \
	MAX_NUMBER_DISK_QUEUES)
**/
#else
#	define MAX_FMR_POOL_SIZE 512
#endif

#if KS_IB_CLIENT_ADD_RV_IS_INT
static int add_one(struct ib_device *device);
#else
static void add_one(struct ib_device *device);
#endif
#if IB_REMOVE_EXTRA_ARG
static void remove_one(struct ib_device *device, void *client_data);
#else
static void remove_one(struct ib_device *device);
#endif
static bool nvmeibs_client_registered;
static struct ib_client nvmeibs_client = {
	.name   = PROCFS_COMMON_STR,
	.add    = add_one,
	.remove = remove_one
};

static int do_add_one(struct nvmeibs_dev *nis_dev);
static void remove_nis(struct nvmeibs_dev *nis_dev);

static struct proc_dir_entry *get_nic_stats_proc_dir(void);

struct ib_client *nvmeibs_get_ib_client(void)
{
	return &nvmeibs_client;
}

typedef int (*main_wq_fn_type)(void *param);

struct run_mainwq_workqe {
	struct workqe_struct work;
	main_wq_fn_type fn;
	void *param;
	int rv;
	bool free_work;
};

bool nvmeibs_on_main_wq(void)
{
	return on_wq_pid(main_wq_pid);
}

static int run_on_main_wq(main_wq_fn_type fn, void *param, bool drain_wq,
	bool can_sleep, int *p_sts);

static struct nvmeib_cm_id *cmh_get(struct nvmeib_rdma_cm *cm_id)
{
	struct hlist_node *hlink;
	struct nvmeib_cm_id *cmh;
	bool found = false;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&nvmeibs_cmid_lock, flags);
	hlist_for_each(hlink, &nvmeibs_cmid_hash) {
		if (h_to_cm_id(hlink)->cm_id == cm_id) {
			cmh = h_to_cm_id(hlink);
			if (!nvmeibs_use_pcpu_cq)
				found = true;
			else {
				/* previously it was enough to remove @cm_id from hash as
				   we would always do destry-cm (i.e. stop cm events) and
				   THEN free s_net.
				   Now that we defer destroy-cm potentially till AFTER we
				   free s_net (which @cm_id is embedded in), we must ensure
				   that caller is done using @cm_id before one can free s_net
				   --> taking refcnt */
				found = cmh->ref_chg(cmh, true); /* cmh_ref_chg */
			}
			break;
		}
	}
	spin_unlock_irqrestore(&nvmeibs_cmid_lock, flags);

	NFOUT;
	return found ? h_to_cm_id(hlink) : NULL;
}

static void cmh_put(struct nvmeib_cm_id *cmh)
{
	if (nvmeibs_use_pcpu_cq)
		cmh->ref_chg(cmh, false);
}


void nvmeibs_add_cm_id(struct nvmeib_cm_id *cm_id)
{
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&nvmeibs_cmid_lock, flags);
	hlist_add_head(&cm_id->cmid_link, &nvmeibs_cmid_hash);
	spin_unlock_irqrestore(&nvmeibs_cmid_lock, flags);

	NFOUT;
}

void nvmeibs_remove_cm_id(struct nvmeib_cm_id *cm_id)
{
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&cm_id->cm_evt_state_lock, flags);
	cm_id->cm_evt_handle = false;
	spin_unlock_irqrestore(&cm_id->cm_evt_state_lock, flags);

	spin_lock_irqsave(&nvmeibs_cmid_lock, flags);
	hlist_del_init(&cm_id->cmid_link);
	spin_unlock_irqrestore(&nvmeibs_cmid_lock, flags);

	NFOUT;
}

u64 nvmeibs_get_client_uid(void)
{
	u64 cid;

	/* client_uid (cid) is only 24 bits, and MAY NOT BE ZERO */

	cid = atomic_long_inc_return(&client_uid) & 0xffffff;
	if (cid == 0)
		cid = atomic_long_inc_return(&client_uid) & 0xffffff;

	return cid;
}

static void update_gid_change(struct workqe_struct *work)
{
	struct port_work *pw = container_of(work, struct port_work, work);
	struct cid_port_work *cw =
		container_of(pw, struct cid_port_work, port);
	struct gid_update_port_work *gw =
		container_of(cw, struct gid_update_port_work, cid);
	struct nvmeibs_client *cl;
	unsigned long flags;

	NFIN;
	flags = nvmeibs_cdb_lock();
	cl = nvmeibs_cdb_find_cid_locked(cw->cid);
	if (cl == gw->cl)
		gw->f(cl, pw->port);
	nvmeibs_cdb_unlock(flags);
	kfree(gw);
	NFOUT;
}

static void update_gid_change_work(struct nvmeibs_client *cl,
	struct nvmeibs_ib_port *ib_port,
	int (*f)(struct nvmeibs_client *, struct nvmeibs_ib_port *i))
{
	struct gid_update_port_work *w;

	NFIN;
	if ((w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
		w->cid.port.port = ib_port;
		w->cid.cid = cl->cid;
		w->cl = cl;
		w->f = f;
		WQ_INIT_WORK(&w->cid.port.work, update_gid_change);
		if (nvmeibs_ib_port_add_work(cl->ib_port, &w->cid.port.work))
			kfree(w);
	}
	NFOUT;
}

struct relase_port_client_ctx {
	struct nvmeibs_ib_port *ib_port;
	enum nvmeibs_logout_reason reason;
};

static int nvmeibs_release_port_client_fc(struct nvmeibs_client *cl, void *arg)
{
	struct relase_port_client_ctx *ctx = arg;
	struct nvmeibs_ib_port *ib_port = ctx->ib_port;

	if (ib_port == cl->ib_port ||
		(cl->lock_net && ib_port == cl->lock_net->params.port)) {
		_NT(trace_main_nvmeibs_release_port_client_fc, "Will call free client @CL cid @CID (device @IB_DEV_NAME port @PORT)", cl, cl->cid,
		   P2IB(ib_port)->name, ib_port->port);
		nvmeibs_ib_port_free_client(cl->ib_port, cl->cid, ctx->reason);
	}
	else {
		/* lionics may be under construction thus we run on cl-wq.
		   By the time work runs, we might be disconnecting io-channels
		   that were connected after port became valid again. well, the
		   client will have to reconnect them */
		update_gid_change_work(cl, ib_port, nvmeibs_client_stop_io_on_lgid);
	}

	return 0;
}

/* Disconnect any client's channels using this port.
 *
 * Target initiates the disconnect because:
 *  1. Port may still be alive but not allowed e.g.
 *     port's new gid does not pass user-filters.
 *  2. Speedup, dont wait for keep-alive/WD
 */
void nvmeibs_release_port_clients(struct nvmeibs_ib_port *ib_port,
								  enum nvmeibs_logout_reason reason)
{
	struct relase_port_client_ctx ctx = {.ib_port = ib_port,
										  .reason = reason};
	NFIN;

	nvmeibs_cdb_all_fast_call(nvmeibs_release_port_client_fc, &ctx);

	NFOUT;
}

static int nvmeibs_release_all_client_fc(struct nvmeibs_client *cl, void *arg)
{
	struct nvmeibs_ib_port *ib_port = cl->ib_port;

	_NT(trace_main_nvmeibs_release_all_client_fc,
		"Will call free client @CL cid @CID (device @IB_DEV_NAME port @PORT)",
		cl, cl->cid, P2IB(ib_port)->name, ib_port->port);
	nvmeibs_ib_port_free_client(cl->ib_port, cl->cid,
								(enum nvmeibs_logout_reason)arg);

	return 0;
}

static void nvmeibs_release_all_clients(enum nvmeibs_logout_reason reason)
{
	NFIN;

	nvmeibs_cdb_all_fast_call(nvmeibs_release_all_client_fc, (void *)reason);

	NFOUT;
}

static int nvmeibs_update_client_gid_change_fc(struct nvmeibs_client *cl, void *arg)
{
	struct nvmeibs_ib_port *ib_port = arg;

	_NT(trace_main_nvmeibs_update_client_gid_change_fc, "Update cl=@CL cid=@CID, device=@DEVICE, port=@PORT, hw-gid=@GID_IPV6)",
	   cl, cl->cid, P2IB(ib_port)->name, ib_port->port,
	   ib_port->gid.hw_gid.raw);

	if (cl->ib_port == ib_port)
		nvmeibs_client_update_gid_change(cl, ib_port);
	else
		update_gid_change_work(cl, ib_port, nvmeibs_client_update_gid_change);

	return 0;
}

void nvmeibs_update_all_clients_gid_change(struct nvmeibs_ib_port *ib_port)
{
	NFIN;

	nvmeibs_cdb_all_fast_call(nvmeibs_update_client_gid_change_fc, ib_port);

	NFOUT;
}

static int remove_cid_client_fc(struct nvmeibs_client *cl, void *arg)
{
	_NT(trace_main_remove_cid_client_fc, "Will call free client @CLIENT_UUID (@CID) - @CL", &cl->client_uuid, cl->cid, cl);

	nvmeibs_ib_port_free_client(cl->ib_port, cl->cid, (enum nvmeibs_logout_reason)arg);

	return 0;
}

static void remove_cid_clients(struct workqe_struct *work)
{
	struct suuid_workq *uwork =
		container_of(work, struct suuid_workq, work);

	NFIN;

	nvmeibs_cdb_cid_fast_call(uwork->cid, remove_cid_client_fc,
							 (void *)(uwork->reason));
	kfree(uwork);

	NFOUT;
}

int nvmeibs_remove_cid_clients(u64 cid, enum nvmeibs_logout_reason reason)
{
	struct suuid_workq *work;
	int rv;

	NFIN;
	if ((work = kzalloc(sizeof(*work), GFP_KERNEL))) {
		WQ_INIT_WORK(&work->work, remove_cid_clients);
		work->cid = cid;
		work->reason = reason;
		if ((rv = nvmeibs_add_work(&work->work)) < 0) {
			kfree(work);
			_NT(trace_main_nvmeibs_remove_cid_clients, "Failed to queue IB device add_one: @RV", rv);
		}
	}
	else {
		_NE(error_main_nvmeibs_remove_cid_clients, "Failed to allocate work for IB device add_one");
		rv = -ENOMEM;
	}

	NFOUT;
	return rv;
}

struct nvmeibs_client *nvmeibs_find_client_(
	u64 cid, struct nvmeibs_ib_port **ib_port)
{
	struct nvmeibs_client *cl = NULL;

	NFIN;
	_ND(trace_main_nvmeibs_find_client_, "Looking for cid @CID_LLONG", cid);
	cl = nvmeibs_cdb_find_cid_locked(cid);
	if (cl && ib_port)
		*ib_port = cl->ib_port;
	NFOUT;
	return cl;
}

struct nvmeibs_client *nvmeibs_find_client(
	u64 cid, struct nvmeibs_ib_port **ib_port)
{
	struct nvmeibs_client *cl = NULL;
	unsigned long flags;

	NFIN;
	_ND(trace_main_nvmeibs_find_client, "into nvmeibs_find_client");
	flags = nvmeibs_cdb_lock();
	cl = nvmeibs_find_client_(cid, ib_port);
	nvmeibs_cdb_unlock(flags);
	_ND(trace_1_main_nvmeibs_find_client, "Out of nvmeibs_find_client");
	NFOUT;
	return cl;
}

static struct nvmeibs_dev *find_nis_dev(struct ib_device *dev)
{
	struct nvmeibs_dev *nis_dev;
	bool found = false;

	NFIN;
	nvmeibs_get_devices(NULL);
	list_for_each_entry(nis_dev, &nvmeibs_dev_list, nvmeibs_dev_list_n)
		if (N2IB(nis_dev) == dev) {
			found = true;
			break;
		}
	nvmeibs_put_devices();
	if (!found)
		nis_dev = NULL;

	NFOUT;
	return nis_dev;
}

/**
 * cm_req_recv() - Process the event IB_CM_REQ_RECEIVED.
 *
 * Ownership of the cm_id is transferred to the client session
 * if this functions returns zero. Otherwise the caller remains
 * the owner of cm_id.
 */
static int cm_req_recv(struct nvmeib_rdma_cm *cm,
                       struct nvmeib_rdma_conn_params *param)
{
	struct nvmeibs_dev *nis_dev = nvmeib_rdma_get_listener_context(cm);
	struct nvmeibs_ib_port *ib_port = NULL;
	int rv = -EINVAL;
	u32 local_ib_id = (u32)-1;
	u32 remote_ib_id = (u32)-1;
	u32 outstanding;

	NFIN;
	WARN_ON_ONCE(irqs_disabled());

	if (IS_ERR(nis_dev)) {
		_NE(error_main_cm_req_recv, "CM request received on listener with invalid context");
		goto out;
	}

	if (!param) {
		_NT(trace_main_cm_req_recv, "New connection on device @IB_DEV_NAME but with NULL params",
			N2IB(nis_dev)->name);
		goto out;
	}
	if (!(nis_dev || (nis_dev = find_nis_dev(param->d)))) {
		_NT(trace_1_main_cm_req_recv, "Got a new connection on a NULL device - "
		   "maybe the device was filtered out");
		goto out;
	}
	if (!(ib_port = nvmeibs_ib_port_find_ib_port(nis_dev, param->port))) {
		_NT(trace_2_main_cm_req_recv, "Failed to find port on device @IB_DEV_NAME:@PORT - "
		   "probably port was filtered out",
			N2IB(nis_dev)->name, param->port);
		goto out;
	}
	if (!nvmeibs_ib_port_enabled(ib_port)) {
		_NT(trace_3_main_cm_req_recv, "Port @PORT on device @IB_DEV_NAME is not enabled\n",
			param->port, N2IB(nis_dev)->name);
		goto out;
	}
	_ND(trace_4_main_cm_req_recv, "ib_port=@IB_PORT", ib_port);
	nvmeib_rdma_cm_conn_get_ib_ids(cm, &local_ib_id, &remote_ib_id);
	_NT(trace_5_main_cm_req_recv, "Received CM REQ (IDs: @LOCAL_IB_ID->@REMOTE_IB_ID) on port @IB_DEV_NAME:@PORT",
	   local_ib_id, remote_ib_id, P2IB(ib_port)->name, ib_port->port);

	outstanding = atomic_inc_return(&ib_port->outstanding);
	if (outstanding >= max_outstanding_cm_work_items) {
		_NT(cm_req_recv_t1, "Rejecting CM REQ (IDs: @INT32_HEX->@INT32_HEX) on port @STR:@INT due to load @INT/@INT",
			local_ib_id, remote_ib_id, P2IB(ib_port)->name, ib_port->port,
			outstanding, max_outstanding_cm_work_items);
		goto dec_before_out;
	}

	rv = nvmeibs_ib_port_new_connection(ib_port, cm, param);
	if (!rv) {
		_NT(cm_req_recv_t2, "Running @INT/@INT", outstanding, max_outstanding_cm_work_items);
		goto out;
	}

dec_before_out:
	atomic_dec(&ib_port->outstanding);

out:
	NFOUT;
	return rv;
}

static void count_cm_event(struct nvmeib_cm_id *nv_cm_id,
	struct nvmeib_rdma_event *nv_event)
{
	switch (nv_event->event) {
	case NVMEIB_REJ_RECEIVED: /* connection reject received */
		nv_cm_id->cm_evt_cnt.rej++;
		break;
	case NVMEIB_RTU_RECEIVED: /* client is ready to send messages */
		nv_cm_id->cm_evt_cnt.rtu++;
		break;
	case NVMEIB_USER_ESTABLISHED: /* (RoCE) connection is established */
		nv_cm_id->cm_evt_cnt.usr_est++;
		break;
	case NVMEIB_DREQ_RECEIVED: /* connection is closed by client */
		nv_cm_id->cm_evt_cnt.dreq++;
		break;
	case NVMEIB_DREP_RECEIVED: /* we closed the connection and the client ack */
		nv_cm_id->cm_evt_cnt.drep++;
		break;
	case NVMEIB_TIMEWAIT_EXIT:
		nv_cm_id->cm_evt_cnt.timewt_exit++;
		break;
	case NVMEIB_REP_ERROR:
		nv_cm_id->cm_evt_cnt.rep_err++;
		break;
	case NVMEIB_DREQ_ERROR:
		nv_cm_id->cm_evt_cnt.dreq_err++;
		break;
	case NVMEIB_MRA_RECEIVED:
		nv_cm_id->cm_evt_cnt.mra_recv++;
		break;
	case NVMEIB_DEVICE_REMOVED:
		nv_cm_id->cm_evt_cnt.dev_err++;
		break;
	default:
		_NT(trace_main_count_cm_event, "received unrecognized CM event @EVENT", nv_event->event);
		break;
	}
}

static int client_cm_event(struct nvmeib_rdma_cm *cm_id,
	struct nvmeib_rdma_event *event)
{
	struct nvmeib_cm_id *cmh;
	unsigned long flags;

	NFIN;
	/* first try to find the cm_id in our cm_id database */
	cmh = cmh_get(cm_id);

	/* if this is a real connection so we can call its handler */
	if (cmh) {
		spin_lock_irqsave(&cmh->cm_evt_state_lock, flags);
		/* Count CM events, both to see if we missed an RTU or a DREQ
		   while bringing up the QP and for debugging */
		count_cm_event(cmh, event);
		if (cmh->cm_evt_handle) {
			spin_unlock_irqrestore(&cmh->cm_evt_state_lock, flags);
			cmh->handler(cmh, event);
			goto put;
		}
		spin_unlock_irqrestore(&cmh->cm_evt_state_lock, flags);
put:
		cmh_put(cmh);
	}
	else {
		_NT(client_cm_event_t1, "Fail to get ctx for cm_id=@PTR (evt=@INT)", cm_id, event->event);
	}


	NFOUT;
	return 0;
}

/* Called with nvmeibs_dev_guard locked */
static void clear_ports(struct nvmeibs_dev *nis_dev, bool release_clients)
{
	struct nvmeibs_ib_port *ib_port, *tmp;
	int i;

	NFIN;
	if (release_clients) {
		#if 0
		/* first release all clients on all ports */
		list_for_each_entry(ib_port, &nis_dev->port_list, port_list_n)
			nvmeibs_release_port_clients(ib_port);
		#else
		/* if here, nis-dev is used and either module is being rmmod or
		   this is a real hot-unplug of the NIC (may be cause as a side-
		   effect of physical disk pull-out). Therefore, we must release
		   any cl that may be using this dev (not only for admin or lock
		   channles but for io-channels as well. This is effectively all
		   the clients in DB */
		_NT(trace_0_clear_ports, "release ALL clients");
		nvmeibs_release_all_clients(NVMEIBS_LOGOUT_REASON_PORTS_CLEAR);
		#endif
	}
	/* first wait for clients to stop - do not kill port */
	list_for_each_entry(ib_port, &nis_dev->port_list, port_list_n)
		nvmeibs_ib_port_clear(ib_port);

	/* after all clients are stopped i.e. nvmeib_cq_qp_del called for all cl's channels &&
	   before freeing listener, drain all pending destry-cm's still linked to its conns-list */
	if (nvmeibs_use_pcpu_cq)
		nvmeib_dev_drain_cqs(nis_dev->dev);

	/* ensure listener has no conns */
	WARN_ON(nvmeib_rdma_listener_has_conns(nis_dev->ib_l_cm_id));
	WARN_ON(nvmeib_rdma_listener_has_conns(nis_dev->iw_prim_l_cm_id));
	for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++)
		WARN_ON(nvmeib_rdma_listener_has_conns(nis_dev->iw_2nd_l_cm_id[i]));
	list_for_each_entry(ib_port, &nis_dev->port_list, port_list_n)
		WARN_ON(nvmeib_rdma_listener_has_conns(ib_port->loop_listener));

	/* no more clients - kill ports */
	list_for_each_entry_safe(ib_port, tmp, &nis_dev->port_list, port_list_n) {
		list_del(&ib_port->port_list_n);
		nvmeibs_ib_port_free(ib_port);
	}
	list_for_each_entry_safe(ib_port, tmp, &nis_dev->unused_port_list, port_list_n) {
		list_del(&ib_port->port_list_n);
		nvmeibs_ib_port_free(ib_port);
	}
	nis_dev->ib_ports = 0;
	nis_dev->roce_ports = 0;
	nis_dev->iwarp_ports = 0;
	nis_dev->unused_ports = 0;

	NFOUT;
}

//handle with care...
struct list_head *nvmeibs_get_devices_no_lock(int *size)
{
	NFIN;
	*size = nvmeibs_dev_count;

	NFOUT;
	return &nvmeibs_dev_list;
}

struct list_head *nvmeibs_get_unused_devices_no_lock(int *size)
{
	NFIN;
	*size = nvmeibs_unused_dev_count;

	NFOUT;
	return &nvmeibs_unused_dev_list;
}

struct list_head *nvmeibs_get_devices(int *size)
{
	NFIN;
	mutex_lock(&nvmeibs_dev_guard);
	if (size)
		*size = nvmeibs_dev_count;
	NFOUT;
	return &nvmeibs_dev_list;
}

bool nvmeibs_is_devices_locked_by_me(void)
{
	NFIN;
	NFOUT;
	return mutex_is_locked_by_me(&nvmeibs_dev_guard);
}

void nvmeibs_put_devices(void)
{
	NFIN;
	mutex_unlock(&nvmeibs_dev_guard);
	NFOUT;
}

struct list_head *nvmeibs_get_used_dev_list(void)
{
	NFIN;
	NFOUT;
	return &used_dev_list;
}

struct nvmeibs_dev *nvmeibs_get_nis_dev(struct ib_device *device)
{
	return ib_get_client_data(device, &nvmeibs_client);
}

struct activate_device_param {
	struct nvmeibs_dev *nis_dev;
	bool new_device;
};

static int srv_activate_device_work_fn(void *param)
{
	int rv;
	struct activate_device_param *ad_param = param;
	struct nvmeibs_dev *nis_dev = ad_param->nis_dev;
	bool new_device = ad_param->new_device;

	NFIN;
	if (nis_dev->device_used) {
		rv = -EINVAL;
		goto out;
	}

	if (!new_device) {
		nvmeibs_get_devices(NULL);
		/* Not a new device, remove from existing unused device list */
		list_del(&nis_dev->nvmeibs_dev_list_n);
		nvmeibs_unused_dev_count--;
		nvmeibs_put_devices();
	}

	if ((rv = do_add_one(nis_dev)) < 0) {
		_NT(trace_main_srv_activate_device_work_fn, "Nic failed to initialize (@RV), Add to unused list", rv);
		nis_dev->device_used = false;
		nvmeibs_get_devices(NULL);
		list_add_tail(&nis_dev->nvmeibs_dev_list_n, &nvmeibs_unused_dev_list);
		nvmeibs_unused_dev_count++;
		nvmeibs_put_devices();
		goto out;
	}

	nvmeibs_get_devices(NULL);
	list_add_tail(&nis_dev->nvmeibs_dev_list_n, &nvmeibs_dev_list);
	nvmeibs_dev_count++;
	nvmeibs_put_devices();

out:
	if (!rv)
		nvmeibs_toma_report_event_nic_change(nis_dev, true);
	NFOUT;
	return rv;
}

int nvmeibs_activate_device(struct nvmeibs_dev *nis_dev, bool new_device)
{
	struct activate_device_param ad_param = {
		.nis_dev = nis_dev,
		.new_device = new_device,
	};
	return run_on_main_wq(srv_activate_device_work_fn, &ad_param, true, true,
		NULL);
}

static int allocate_fmr(struct nvmeibs_dev *nis_dev)
{
	struct ib_fmr_pool *fmr_pool = NULL;
	struct nvmeib_fr_pool *fr_pool = NULL;
	int rv = 0;

	NFIN;
	/* allocate fast memory registration pool */
	if (!nvmeib_alloc_fast_reg_pool(
		nis_dev->dev, &fmr_pool, &fr_pool, MAX_FMR_POOL_SIZE, s_dev_fr_pool)) {
		if (fmr_pool)
			nis_dev->dev->fmr_pool = fmr_pool;
		else if (fr_pool)
			nis_dev->dev->fr_pool = fr_pool;
	}
	/* at the moment we do not support FR on the controller */
	if (!fmr_pool && !fr_pool) {
		_NE(error_main_allocate_fmr, "Cannot allocate port MR.");
		rv = -1;
	}

	NFOUT;
	return rv;
}

static void free_fmr(struct nvmeibs_dev *nis_dev)
{
	NFIN;
	/* destroy fast registration pool */
	if (nis_dev->dev->use_fast_reg) {
		if (nis_dev->dev->fr_pool) {
			nvmeib_destroy_fast_reg_pool(nis_dev->dev->fr_pool, NULL, NULL, s_dev_fr_pool, nis_dev->dev->fr_pool->size);
			nis_dev->dev->fr_pool = NULL;
		}
	} else if (nis_dev->dev->fmr_pool) {
#if KS_IB_VERBS_SUPPORTS_FMR
		ib_destroy_fmr_pool(nis_dev->dev->fmr_pool);
		nis_dev->dev->fmr_pool = NULL;
#else
		BUG();
#endif
	}
	NFOUT;
}

static int cl_dma_map_virt(struct ib_device *ib, void *p, size_t len, dma_addr_t *d, enum dma_data_direction dir)
{
	dma_addr_t dma;

	dma = ib_dma_map_single(ib, p, len, dir);
	if (ib_dma_mapping_error(ib, dma)) {
		_NE(error_main_cl_dma_map_virt, "error mapping virtual addr @PTR for ib device @IB_NAME",
		   p, ib->name);
		return -EIO;
	}

	*d = dma;
	return 0;
}

static void cl_dma_unmap_virt(struct ib_device *ib, dma_addr_t *d, size_t len, enum dma_data_direction dir)
{
	if (*d) {
		ib_dma_unmap_single(ib, *d, len, dir);
		*d = 0;
	}
}

static int cl_dma_map_phys(struct ib_device *ib,
			   phys_addr_t p, size_t len,
			   dma_addr_t *d, enum dma_data_direction dir)
{
	dma_addr_t dma;

	dma = ib_dma_map_page(ib,
			      pfn_to_page(PHYS_PFN(p)),
			      offset_in_page(p),
			      len,
			      dir);
	if (ib_dma_mapping_error(ib, dma)) {
		_NE(error_main_cl_dma_map_phys, "Error mapping phys addr 0x@PHYS for IB device @IB_NAME",
		   p, ib->name);
		return -EIO;
	}

	*d = dma;
	return 0;
}

static void cl_dma_unmap_phys(struct ib_device *ib,
			      dma_addr_t *d, size_t len,
			      enum dma_data_direction dir)
{
	if (*d) {
		ib_dma_unmap_page(ib, *d, len, dir);
		*d = 0;
	}
}

static void cl_dma_unmap_resources(struct ib_device *ib, struct nvmeibs_q_info *qs, struct nvme_dma_info *info)
{
	if (!info) {
		_NT(trace_main_cl_dma_unmap_resources, "NULL info - bailing out");
		return;
	}
	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	cl_dma_unmap_phys(ib, &info->cq_db, sizeof(*qs->cq_doorbell), DMA_FROM_DEVICE);
	/* JH IOMMU: DMA_TO_DEVICE is correct, used as a source for Local RDMA_WRITE and a source for Remote RDMA_READ (OE) */
	cl_dma_unmap_virt(ib, &info->cq, PAGE_SIZE, DMA_TO_DEVICE);
	/* JH IOMMU: DMA_FROM_DEVICE is correct, (Not used, but would be a sink for Remote RDMA_WRITE) */
	cl_dma_unmap_phys(ib, &info->prpl, PAGE_SIZE, DMA_FROM_DEVICE);
	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	cl_dma_unmap_phys(ib, &info->sq_db, sizeof(*qs->sq_doorbell), DMA_FROM_DEVICE);
	if (qs->bb_mtdt_virt) {
		/* JH IOMMU: DMA_BIDIRECTIONAL is correct, used as a sink for Remote RDMA_WRITE (Write Op) and a source for Local RDMA_WRITE (Read Op) */
		cl_dma_unmap_virt(ib, &info->md, qs->bb_mtdt_nvme_len, DMA_BIDIRECTIONAL);
	}
	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	cl_dma_unmap_virt(ib, &info->sq, PAGE_SIZE, DMA_FROM_DEVICE);
}

static int cl_dma_map_resources(struct ib_device *ib, struct nvmeibs_q_info *qs, struct nvme_dma_info *info)
{
	int rc;

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	rc = cl_dma_map_virt(ib, qs->sq, PAGE_SIZE, &info->sq, DMA_FROM_DEVICE);
	if (rc) {
		_NE(error_main_cl_dma_map_resources, "Failed mapping NVME SQ to NIC");
		goto err_unmap_rscs;
	}

	if (qs->bb_mtdt_virt) {
		/* JH IOMMU: DMA_BIDIRECTIONAL is correct, used as a sink for Remote RDMA_WRITE (Write Op) and a source for Local RDMA_WRITE (Read Op) */
		rc = cl_dma_map_virt(ib, qs->bb_mtdt_virt, qs->bb_mtdt_nvme_len,
			&info->md, DMA_BIDIRECTIONAL);
		if (rc) {
			_NE(error_1_main_cl_dma_map_resources, "Failed mapping NVME MD to NIC");
			goto err_unmap_rscs;
		}
	}

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	rc = cl_dma_map_phys(ib,
			     qs->sq_db_phys, sizeof(*qs->sq_doorbell),
			     &info->sq_db, DMA_FROM_DEVICE);
	if (rc) {
		_NE(error_2_main_cl_dma_map_resources, "Failed mapping NVME SQ doorbell to NIC");
		goto err_unmap_rscs;
	}

	/* JH IOMMU: DMA_FROM_DEVICE is correct, (Not used, but if so would be a sink for Remote RDMA_WRITE) */
	rc = cl_dma_map_phys(ib,
			     qs->prpl_phys, PAGE_SIZE,
			     &info->prpl, DMA_FROM_DEVICE);
	if (rc) {
		_NE(error_3_main_cl_dma_map_resources, "Failed mapping NVME PRP1 to NIC");
		goto err_unmap_rscs;
	}

	/* JH IOMMU: DMA_TO_DEVICE is correct, used as a source for Local RDMA_WRITE and a source for Remote RDMA_READ (OE) */
	rc = cl_dma_map_virt(ib, qs->cq, PAGE_SIZE, &info->cq, DMA_BIDIRECTIONAL);
	if (rc) {
		_NE(error_4_main_cl_dma_map_resources, "Failed mapping NVME CQ to NIC");
		goto err_unmap_rscs;
	}

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	rc = cl_dma_map_phys(ib,
			     qs->cq_db_phys, sizeof(*qs->cq_doorbell),
			     &info->cq_db, DMA_FROM_DEVICE);
	if (rc) {
		_NE(error_5_main_cl_dma_map_resources, "Failed mapping NVME CQ doorbell to NIC");
		goto err_unmap_rscs;
	}

	return rc;

err_unmap_rscs:
	cl_dma_unmap_resources(ib, qs, info);

	return rc;
}

static void free_disk_resources(struct disk_mapped_mem_info *mem, struct nvmeibs_disk_info *disk)
{
	struct nvmeib_alloc_n_map *bb_map;
	struct nvmeibs_dev *nis_dev;
	struct ib_device *ib;
	int i;

	if (!mem)
		return;

	if (!mem->bb_maps)
		goto out;

	nis_dev = mem->dev;
	ib = nis_dev->dev->ib_dev;

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	cl_dma_unmap_phys(ib,
			  &mem->nvme_msix_addr_iommu, disk->n_qs * PCI_MSIX_ENTRY_SIZE,
			  DMA_FROM_DEVICE);

	for (i = 0; i < disk->n_qs; ++i) {
		struct nvmeibs_q_info *qs = &disk->qs[i];

		cl_dma_unmap_resources(ib, qs, &mem->ib_dma[i]);

		bb_map = &mem->bb_maps[i];
		if (!bb_map)
			continue;

		if (bb_map->n_pages > 1) {
			nvmeib_mem_unmapn_n_free(bb_map);
			kfree(bb_map->pages);
		} else {
			BUG_ON(bb_map->mem_table.sgl);
			dma_unmap_page(ib->dma_device, bb_map->ioaddr, PAGE_SIZE, DMA_BIDIRECTIONAL);
		}
	}
	kfree(mem->ib_dma);
	kfree(mem->bb_maps);

out:
	kfree(mem);
}

/* Called with nvmeibs_dev_guard locked */
static int nvmeibs_register_disk_resources(struct nvmeibs_dev *nis_dev,
	struct nvmeibs_disk_info *disk)
{
	struct ib_device *ib = nis_dev->dev->ib_dev;
	struct disk_mapped_mem_info *mem;
	struct nvmeib_alloc_n_map *bb_maps, *bb_map;
	struct nvme_dma_info *ib_dma;
	struct nvmeibs_q_info *qs;
	dma_addr_t t_dma = 0;
	int i = 0;
	int rv = 0;

	NFIN;

	if (!disk->n_qs) {
		_NT(trace_register_disk_resources_no_qs,
		    "disk @DISK_NAME has no queues to register", disk->disk_id);
		rv = -ENOENT;
		goto out;
	}

	// Check for duplicate NIC
	list_for_each_entry(mem, &disk->mem_priv_list, link) {
		if (mem->dev == nis_dev) {
			_NW(warn_register_disk_resources_already,
				"disk mem @DISK_ID_STR already has mem_priv @MEM_PTR for dev @IB_DEV_NAME",
				disk->disk_id, mem, N2IB(mem->dev)->name);
			BUG_ON(1); /* Temporary to catch issue */
			rv = -EALREADY;
			goto out;
		}
	}

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	ib_dma = kcalloc(sizeof(*ib_dma), disk->n_qs, GFP_KERNEL);
	bb_maps = kzalloc(sizeof(*bb_maps) * disk->n_qs, GFP_KERNEL);
	/* allocate memory info array */
	if (!mem || (disk->n_qs && (!bb_maps || !ib_dma))) {
		_NE(error_1_main_nvmeibs_register_disk_resources, "OOM: cannot create mapped memory info for disk @DISK_ID_STR resource",
			disk->disk_id);
		rv = -ENOMEM;
		kfree(mem);
		kfree(bb_maps);
		kfree(ib_dma);
		goto out;
	}

	mem->bb_maps = bb_maps;
	mem->ib_dma = ib_dma;
	mem->dev = nis_dev;

	/* fill device ports so the client knows which remote nics
	   use the below mapped stuff
	*/
	nvmeibs_disk_locks_update_mem_gids_(nis_dev, mem);

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_WRITE */
	rv = cl_dma_map_phys(nis_dev->dev->ib_dev,
			     disk->msix_addr_phys, disk->n_qs * PCI_MSIX_ENTRY_SIZE,
			     &t_dma, DMA_FROM_DEVICE);
	if (rv) {
		_NE(error_2_main_nvmeibs_register_disk_resources, "Failed mapping NVME MSIX to NIC");
		goto free_mem;
	}
	mem->nvme_msix_addr_iommu = t_dma;

	_NT(trace_main_nvmeibs_register_disk_resources, "Disk @DISK_ID_STR, n_rscs=@N_RSCS", disk->disk_id, disk->n_qs);
	for (i = 0; i < disk->n_qs; ++i) {
		int p;

		qs = &disk->qs[i];
		bb_map = &bb_maps[i];

		/* check if we need to register memory */
		_ND(trace_1_main_nvmeibs_register_disk_resources, "Disk @DISK_ID_STR, rsc[@RSC_ID].bb_npages=@BB_NPAGES",
			disk->disk_id, i, qs->bb_npages);

		bb_map->pd = nis_dev->dev->pd;
		bb_map->n_pages = qs->bb_npages;

		if (bb_map->n_pages > 1) {
			/* Initialise bb_pages array for call to nvmeib_mem_alloc_n_map */
			if (!(bb_map->pages = kcalloc(bb_map->n_pages, sizeof(*bb_map->pages), GFP_KERNEL))) {
				_NE(error_5_main_nvmeibs_register_disk_resources, "Fail to allocate bounce buffer pages");
				rv = -ENOMEM;
				goto free_mem;
			}
			for (p = 0; p < bb_map->n_pages; p++) {
				BUG_ON((unsigned long)qs->bb_addr_virt[p] & (~PAGE_MASK)); /* Verify that virt address is top of page */
				bb_map->pages[p] = virt_to_page(qs->bb_addr_virt[p]);
			}

			/* JH IOMMU: DMA_BIDIRECTIONAL is correct, used as a sink for Remote RDMA_WRITE (Write Op) and a source for Local RDMA_WRITE (Read Op) */
			bb_map->dma_dir = DMA_BIDIRECTIONAL;
			bb_map->access_flags =
				IB_ACCESS_LOCAL_WRITE |
				IB_ACCESS_REMOTE_READ |
				IB_ACCESS_REMOTE_WRITE;
			bb_map->ioaddr = 0;
			bb_map->n_pages = qs->bb_npages;
			if ((rv = nvmeib_mem_alloc_n_map(bb_map)) < 0) {
				_NE(error_6_main_nvmeibs_register_disk_resources, "Fail to map bounce buffer memory @RV", rv);
				rv = -1;
				goto free_mem;
			}
		}
		else {
			/* Only one page, don't use MR, use the global key instead */

			BUG_ON((unsigned long)qs->bb_addr_virt[0] & (~PAGE_MASK)); /* Verify that virt address is top of page */

			/* JH IOMMU: DMA_BIDIRECTIONAL is correct, used as a sink for Remote RDMA_WRITE (Write Op) and a source for Local RDMA_WRITE (Read Op) */
			bb_map->ioaddr = dma_map_page(ib->dma_device, qs->bb_addr_virt[0], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
			if (dma_mapping_error(ib->dma_device, bb_map->ioaddr)) {
				_NE(error_nvmeibs_register_disk_resources_dma_map_fail, "Fail to map bounce buffer page");
				rv = -1;
				goto free_mem;
			}
			bb_map->lkey = nvmeib_get_lkey(nis_dev->dev);
			bb_map->rkey = nvmeib_get_rkey(nis_dev->dev);
		}

		rv = cl_dma_map_resources(nis_dev->dev->ib_dev, qs, &ib_dma[i]);
		if (rv)
		       goto free_mem;
	}
	list_add_tail(&mem->link, &disk->mem_priv_list);
	goto out;

free_mem:
	free_disk_resources(mem, disk);

out:
	NFOUT;
	return rv;
}

void nvmeibs_deregister_disk_resources(struct nvmeibs_dev *nis_dev,
	struct nvmeibs_disk_info *disk)
{
	struct list_head *mems = &disk->mem_priv_list;
	struct disk_mapped_mem_info *mem, *tmp;

	NFIN;
	list_for_each_entry_safe(mem, tmp, mems, link)
		if (!nis_dev || mem->dev == nis_dev) {
			list_del(&mem->link);
			free_disk_resources(mem, disk);
		}
	NFOUT;
}

static void deregister_disks_resources(struct nvmeibs_dev *nis_dev)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *disk;

	NFIN;
	WARN_ON(!nvmeibs_on_main_wq());

	nvmeibs_disk_lock_unmap_on_dev(nis_dev);

	nvmeibs_serjio_unmap_jmd_cache_on_dev(nis_dev);

	disks = nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(disk, disks, link) {
		nvmeibs_deregister_disk_resources(nis_dev, disk);
	}
	nvmeibs_disk_put_disks();
	NFOUT;
}

#if KS_IB_CLIENT_ADD_RV_IS_INT
static int add_one(struct ib_device *device)
#else
static void add_one(struct ib_device *device)
#endif
{
	struct nvmeibs_dev *nis_dev = NULL;
	struct nvmeib_dev *dev;
	int s, e, p;
	bool print_err = true;
	struct nvmeibs_ib_port *ib_port;
	int rv;
	bool use_device = false;
	enum nvmeib_dev_type dev_type;

	NFIN;
	_NI(trace_main_add_one_intro, "add_one called for IB Device @DEV_NAME ptr=@PTR.",
	   device->name, device);

#ifndef BLKDEV_SIMULATOR
	dev_type = nvmeib_get_device_type(device);
#else
	dev_type = DT_uknown;
#endif

	_NT(trace_s_main_add_one, "add_one called for IB Device @DEV_NAME dev_type=@NUM ptr=@PTR", device->name, dev_type, device);

	if (dev_type == DT_siw && nvmeibs_tcp_mode == 0) {
		_NI(trace_s_main_add_one_skip_tcp,  "@DEV_NAME: Skipping TCP transport due to TCP Mode == 0", device->name);
		rv = -ENOTSUPP;
		goto out;
	}
	else if (dev_type != DT_siw && nvmeibs_tcp_mode == 1) {
		_NI(trace_s_main_add_one_skip_non_tcp, "@DEV_NAME: Skipping RDMA transport due to TCP Mode == 1", device->name);
		rv = -ENOTSUPP;
		goto out;
	}
	else if (nvmeib_is_dev_in_blacklist(device)) {
		_NT(trace_s_main_add_one_dev_in_blacklist, "@DEV_NAME: Device is blacklisted, not using", device->name);
		rv = -ENOTSUPP;
		goto out;
	}

	if (!(dev = nvmeib_init(device, "nvmeibs", nvmeibs_use_pcpu_cq, nvmeibs_pcpu_cq_poll_proc))) {
		_NE(error_main_add_one, "@DEVICE_NAME init failed.", device->name);
		rv = -ENODEV;
		goto out;
	}

	_NT(trace_main_add_one, "Hooked to device vendor @VENDOR_ID vendor part id = @VENDOR_PART_ID",
		dev->dev_attr->vendor_id, dev->dev_attr->vendor_part_id);

	if (0) {
		struct device *kernel_dev = IBDEV2DMADEV(device);
		struct pci_dev *pdev = to_pci_dev(kernel_dev);

		_NI(trace_1_main_add_one, "device=@DEVICE_PTR nis_dev=@NIS_DEV vendor=@VENDOR part_id=@PART_ID kernel_dev=@KERNEL_DEV "
		   "pdev=@PDEV pci_slot=@PCI_SLOT procent=@PROCENT devfn=@DEVFN bus=@BUS subordinate=@SUBORDINATE "
		   "slot_bus=@SLOT_BUS slot_number=@SLOT_NUMBER",
		   device, nis_dev, dev->dev_attr->vendor_id,
		   dev->dev_attr->vendor_part_id, kernel_dev, pdev, pdev->slot,
		   pdev->procent, pdev->devfn, pdev->bus, pdev->subordinate,
		   pdev->slot->bus, pdev->slot->number);
	}

	nis_dev = kzalloc(sizeof *nis_dev, GFP_KERNEL);
	if (!nis_dev) {
		_NE(error_1_main_add_one, "@DEVICE_NAME kzalloc failed.", device->name);
		rv = -ENODEV;
		goto err_free_dev;
	}
	nis_dev->dev = dev;
	/* [Gregory] server needs to report atomic response capability only */
	nis_dev->atomic_ops = nvmeib_device_sup_cap(dev->dev_type, NVMEIB_DEVCAP_ATOMICS_RESP);

	INIT_LIST_HEAD(&nis_dev->port_list);
	INIT_LIST_HEAD(&nis_dev->unused_port_list);
	INIT_LIST_HEAD(&nis_dev->nvmeibs_dev_list_n);
	nvmeib_ref_init(&nis_dev->n_refresh_port);

	if (device->node_type == RDMA_NODE_IB_SWITCH) {
		s = 0;
		e = 0;
	} else {
		s = 1;
		e = N2NV(nis_dev)->phys_port_cnt;
	}

	_NT(trace_2_main_add_one, "dev @DEVICE_NAME: @DEVICE_PTR, s=@START_PORT, e=@END_PORT", device->name, device, s, e);
	nis_dev->ib_ports = nis_dev->roce_ports = nis_dev->iwarp_ports = 0;
	for (p = s; p <= e; ++p) {
		ib_port = nvmeibs_ib_port_add(nis_dev, p);
		if (ib_port) {
			if (ib_port->port_used) {
				_NT(trace_5_main_add_one, "Adding port @PORT to device @DEVICE_NAME", p, device->name);
				list_add_tail(&ib_port->port_list_n, &nis_dev->port_list);
				if (ib_port->gid.link_layer == IB_LINK_LAYER_INFINIBAND)
					++nis_dev->ib_ports;
				else if (ib_port->gid.link_layer == IB_LINK_LAYER_ETHERNET) {
					if (ib_port->gid.transport_type == RDMA_TRANSPORT_IWARP)
						++nis_dev->iwarp_ports;
					else
						++nis_dev->roce_ports;
				}
				if (nvmeibs_client_registered && nvmeibs_selected_layer == IB_LINK_LAYER_UNSPECIFIED) {
					/* ensure_devs_uniformity has already run so this new port chooses the link-layer */
					nvmeibs_selected_layer = ib_port->gid.link_layer;
				}
				use_device = true;
			} else {
				_NT(trace_6_main_add_one, "Port @PORT of device @DEVICE_NAME currently unused due to filter", p, device->name);
				list_add_tail(&ib_port->port_list_n, &nis_dev->unused_port_list);
				nis_dev->unused_ports++;
			}
		}
		else {
			_NT(trace_main_add_one_fail_add, "Failed to add port @PORT of device @DEVICE_NAME", p, device->name);
		}
	}

	_NI(trace_7_main_add_one, "Device @DEVICE_NAME @OPT_NOT", device->name,
	   (nis_dev->ib_ports > 0 || nis_dev->roce_ports > 0 || nis_dev->iwarp_ports > 0 ? "will be used" : "disabled by filter"));

	/* Start event handler on device so we can handle port change events (also for unused devices) */
	ib_set_client_data(device, &nvmeibs_client, nis_dev);
	if ((rv = nvmeib_rdma_register_event_handler(device, nvmeibs_ib_port_event_handler,
		nis_dev, &nis_dev->event_handler))) {
		_NE(error_2_main_add_one, "@DEVICE_NAME ib_register_event_handler failed (@RV).", device->name, rv);

		ib_set_client_data(device, &nvmeibs_client, NULL);

		goto err_free_nis_dev;
	}

	if (nvmeibs_client_registered && use_device) {
		/* Already registered so this is after ensure_devs_uniformity
		*  ==> Schedule the do_add_one on the main wq to activate the device */
		if ((rv = nvmeibs_activate_device(nis_dev, true)) < 0) {
			/* The nvmeibs_activate_device fn cleans up in case of failure */
			_NT(trace_8_main_add_one, "nvmeibs_activate_device failed (@RV) for device @DEVICE_NAME", rv, device->name);
		}
		goto out;
	}

	nvmeibs_get_devices(NULL);
	if (use_device) {
		list_add_tail(&nis_dev->nvmeibs_dev_list_n, &nvmeibs_dev_list);
		++nvmeibs_dev_count;
	} else {
		list_add_tail(&nis_dev->nvmeibs_dev_list_n, &nvmeibs_unused_dev_list);
		++nvmeibs_unused_dev_count;
	}
	nvmeibs_put_devices();

	_NT(trace_9_main_add_one, "Device approved dev_count = @NVMEIBS_DEV_COUNT", nvmeibs_dev_count);
	goto out;

err_free_nis_dev:
	while ((ib_port = list_first_entry_or_null(&nis_dev->port_list, struct nvmeibs_ib_port, port_list_n))) {
		list_del(&ib_port->port_list_n);
		nvmeibs_ib_port_free(ib_port);
	}
	while ((ib_port = list_first_entry_or_null(&nis_dev->unused_port_list, struct nvmeibs_ib_port, port_list_n))) {
		list_del(&ib_port->port_list_n);
		nvmeibs_ib_port_free(ib_port);
	}
	kfree(nis_dev);
err_free_dev:
	nvmeib_free(dev);

	if (print_err)
		_NT(trace_10_main_add_one, "Device add one operation aborted");
out:
	NFOUT;
#if KS_IB_CLIENT_ADD_RV_IS_INT
	return rv;
#endif
}

static int create_srq_pool(struct nvmeib_dev *dev, int q_size, int msg_size)
{
	struct nvmeib_srq_params prim;
	struct nvmeib_srq_params sec, *psec = NULL;
	int rv = -1;
	NFIN;

	prim.q_size = q_size; //TODO: reduce if using secondary SRQs
	prim.msg_size = msg_size;
	prim.srq_limit = 1;
	if (nvmeibs_max_nic_srqs > 1) {
		sec.q_size = NVMEIB_NORDDA_SRQ_MAX_SIZE;
		sec.msg_size = roundup_pow_of_two(NVMEIBC_NORDDA_CLIENT_MSG_SIZE);
		sec.srq_limit = 1;
		psec = &sec;
	}
	rv = nvmeib_srq_pool_create(dev, nvmeibs_max_nic_srqs, &prim, psec, s_dev_srq);

	NFOUT;
	return rv;
}

static int create_cq_srq(struct nvmeib_dev *dev)
{
	int rv;
	NFIN;

	rv = nvmeib_create_cq_srq(dev, nvmeibs_max_req_size, s_dev_srq);

	NFOUT;
	return rv;
}

static int nvmeibs_srq_create(struct nvmeib_dev *dev, int q_size, int msg_size)
{
	int rv;

	rv = !nvmeibs_use_pcpu_cq ?
		create_srq_pool(dev, q_size, msg_size) :/* populates dev->srqs */
		create_cq_srq(dev);						/* populates dev->cqs[].srq_info */

	return rv;
}

bool nvmeibs_support_srq(struct nvmeib_dev *dev)
{
	return nvmeibs_max_nic_srqs > 0 && nvmeib_support_srq(dev);
}

#define CORE_SERVER_NIC_IOSTATS_PROC_FRMT_VER 2 /* Bumped to 2 due to fix for [NVMESH-6726] */ 
static ssize_t fill_nic_iostats(void *arg, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	const struct nvmeibs_dev *nis_dev = arg;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	ssize_t count  = 0, indent = 0;
	unsigned long nic_uptime = jiffies - nis_dev->add_jif;
	NFIN;
	count += jops->start_obj(buf + count, len - count, NULL, indent++);
	count += nvmeib_io_stats_to_json(nis_dev->stats, buf+count, len-count, nic_uptime, jops, indent, true);
	count += nvmeib_proc_add_json_proc_epilog(CORE_SERVER_NIC_IOSTATS_PROC_FRMT_VER, buf + count, len - count);
	count += jops->end_obj(buf + count, len - count, JSON_LAST_ELEM, --indent);
	NFOUT;
	return count;
#undef BUF_ADD
}

static ssize_t clear_nic_iostats(void *arg, char *buf , size_t len) {
	struct nvmeibs_dev *nis_dev = arg;
	int reset;
	ssize_t rv;

	if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
		rv = -EINVAL;
		goto out;
	}

	nvmeib_io_stats_clear(nis_dev->stats, 'A');

	rv = len;

out:
	return rv;
}

static int do_add_one(struct nvmeibs_dev *nis_dev)
{
	struct ib_device *device;
	struct nvmeib_dev *dev;
	struct nvmeib_rdma_listen_params params;
	int rv = 0;
	struct nvmeibs_ib_port *ib_port;

	NFIN;
	if (!nis_dev || !nis_dev->dev || !nis_dev->dev->ib_dev) {
		rv = -EINVAL;
		goto out;
	}

	dev = nis_dev->dev;
	device = dev->ib_dev;

	nis_dev->add_jif = jiffies;
	if (!(nis_dev->stats = nvmeib_io_stats_create(
		device->name, VERB_RW_T_RECOV_GEN_BITMASK, nvmeibs_nic_io_stats_block_size)))
	{
		_NE(trace_do_add_one_fail_stats, "Failed to create ib_dev: @DEVICE_NAME io stats",
		    device->name);
		rv = -ENOMEM;
		goto remove_dev;
	}
	if (!(nis_dev->proc_stats_dir = proc_mkdir(device->name, get_nic_stats_proc_dir()))) {
		_NE(trace_do_add_one_fail_proc_stats_dir, "Failed to proc dir @DIR_NAME for io stats",
		    device->name);
		rv = -ENOMEM;
		goto remove_dev;
	}
	nis_dev->proc_stats = nvmeib_public_proc_create("iostats.json", nis_dev->proc_stats_dir, fill_nic_iostats, clear_nic_iostats, nis_dev);
	if (!nis_dev->proc_stats) {
		_NE(error_1_main_nvmeib_nic_create_proc_file, "Failed to create @DIR_NAME/iostats.json proc file", device->name);
		rv = -ENOMEM;
		goto remove_dev;
	}

	if ((rv = nvmeib_init_fast_reg(dev) < 0))
		goto remove_dev;

	if (dev->max_pages_per_mr <= 0) {
		_NE(trace_00_mdo_add_one, "ib dev: @DEVICE_NAME invalid max_pages_per_mr value: @MAX_PAGES_PER_MR\n",
		   dev->ib_dev->name, dev->max_pages_per_mr);
		rv = -EINVAL;
		goto remove_dev;
	}
	if (dev->max_pages_per_mr < nvmeibs_max_pages_per_mr) {
		_NT(trace_01_mdo_add_one, "Reducing max pages in single FMR from @MAX_PAGES_PER_MR to @MAX_PAGES_PER_MR",
			nvmeibs_max_pages_per_mr, dev->max_pages_per_mr);
		nvmeibs_max_pages_per_mr = dev->max_pages_per_mr;
	}

	init_waitqueue_head(&nis_dev->port_release_q);

	if (nvmeibs_support_srq(dev)) {
		/* set the shared received queue size */
		nvmeibs_shared_rq_size =
			min(nvmeibs_shared_rq_size, (int)N2NV(nis_dev)->dev_attr->max_srq_wr);
		_NT(trace_1_main_do_add_one, "SRQ size @NVMEIBS_SHARED_RQ_SIZE, msg_size @NVMEIBS_MAX_REQ_SIZE",
			nvmeibs_shared_rq_size, nvmeibs_max_req_size);
		if (nvmeibs_srq_create(dev, nvmeibs_shared_rq_size, nvmeibs_max_req_size)) {
			_NE(error_main_do_add_one, "@DEVICE_NAME ib_create_srq() failed.", device->name);
			goto remove_dev;
		}
	}

	if (!nvmeibs_service_guid)
		nvmeibs_service_guid = NVMEIB_SERVICE_ID;

	/* print out target login information */
	_ND(trace_2_main_do_add_one, "Host info: guid=@GUID_LLONG", nvmeibs_service_guid);

	if ((rv = allocate_fmr(nis_dev)) < 0) {
		_NE(error_1_main_do_add_one, "@DEVICE_NAME allocate_fmr() failed.", device->name);
		goto remove_dev;
	}

	if ((rv = nvmeibs_disk_lock_disks_map_segs_for_dev(nis_dev))) {
		_NE(error_3_main_do_add_one, "@DEVICE_NAME failed to map lock memory", device->name);
		goto remove_dev;
	}

	if ((rv = nvmeibs_serjio_map_all_jmdc_for_nic(nis_dev))) {
		_NE(error_4_main_do_add_one, "@DEVICE_NAME failed to map serjio JMD Cache", device->name);
		goto remove_dev;
	}

	if (nis_dev->ib_ports > 0) {
		memset(&params, 0, sizeof(params));
		/* initialize params according to known parameters */
		params.type = _rdma_ib;
		params.dev = nis_dev->dev;
		params.ib.service_id = nvmeibs_get_service_guid();
		params.new_connection = cm_req_recv;
		params.on_peer_event = client_cm_event;
		params.context = nis_dev;
		if (!(nis_dev->ib_l_cm_id = nvmeib_rdma_listen(&params))) {
			_NE(error_5_main_do_add_one, "@DEVICE_NAME ib_cm_listen() failed.", device->name);
			rv = -EIO;
			goto remove_dev;
		}
		else
			_NT(trace_3_main_do_add_one, "IB listener started @SERVICE_ID@@DEVICE_NAME",
				params.ib.service_id, device->name);
	} else if (rdma_node_get_transport(N2IB(nis_dev)->node_type) == RDMA_TRANSPORT_IWARP) {
		int primary_tcp_base_port = nvmeib_get_tcp_base_port_id();
		int _2nd_tcp_base_port = primary_tcp_base_port + 1;
		int _2nd_tcp_num_ports = nvmeib_get_tcp_num_ports() - 1;
		int i;

		/* Start iWARP Listener */
		memset(&params, 0, sizeof(params));
		/* Params for primary listener */
		params.type = _rdma_iwarp;
		params.dev = nis_dev->dev;
		params.roce.port = primary_tcp_base_port;
		params.roce.ipv4_only = roce_ipv4_only;
		params.roce.iw_primary = true;
		params.roce.iw_2nd_base_port = _2nd_tcp_base_port;
		params.roce.iw_2nd_num_ports = _2nd_tcp_num_ports;
		params.new_connection = cm_req_recv;
		params.on_peer_event = client_cm_event;
		params.context = nis_dev;

		/* Primary listener on NVMEIB_IWARP_PORT_ID */
		if (!(nis_dev->iw_prim_l_cm_id = nvmeib_rdma_listen(&params))) {
			_NE(error_main_do_add_one_prim_l_fail, "@DEVICE_NAME iw_cm_listen() failed.", device->name);
			rv = -EIO;
			goto remove_dev;
		}
		_NT(trace_main_do_add_one_prim_iw_l, "Primary iWARP listener started @PORT_NUM@@DEVICE_NAME",
		    params.roce.port, device->name);

		params.roce.iw_primary = false;
		params.roce.port = _2nd_tcp_base_port;
		for (i = 0; i < _2nd_tcp_num_ports; i++, params.roce.port++) {
			if (!(nis_dev->iw_2nd_l_cm_id[i] = nvmeib_rdma_listen(&params))) {
				_NE(error_main_do_add_one_2nd_l_fail, "@DEVICE_NAME iw_cm_listen() failed.", device->name);
				rv = -EIO;
				goto remove_dev;
			}
			else
				_NT(trace_main_do_add_one_2nd_iw_l, "Secondary iWARP listener started @PORT_NUM@@DEVICE_NAME",
					params.roce.port, device->name);
		}
	}

	/* Start loopback listener for all enabled ports */
	list_for_each_entry(ib_port, &nis_dev->port_list, port_list_n)
		nvmeibs_init_loopback_listener(ib_port);

	nis_dev->device_used = true;
	_ND(trace_4_main_do_add_one, "added");

out:
	NFOUT;
	return rv;

remove_dev:
	deregister_disks_resources(nis_dev);
	free_fmr(nis_dev);
	clear_ports(nis_dev, false);
	if (nis_dev->event_handler) {
		nvmeib_rdma_unregister_event_handler(nis_dev->event_handler);
		nis_dev->event_handler = NULL;
	}
	if (nis_dev->ib_l_cm_id)
		nvmeib_rdma_destroy_cm(nis_dev->ib_l_cm_id);
	do {
		int i;
		if (nis_dev->iw_prim_l_cm_id)
			nvmeib_rdma_destroy_cm(nis_dev->iw_prim_l_cm_id);
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			if (nis_dev->iw_2nd_l_cm_id[i])
				nvmeib_rdma_destroy_cm(nis_dev->iw_2nd_l_cm_id[i]);
		}
	} while(0);
	if (nis_dev->proc_stats) {
		nvmeib_public_proc_remove(nis_dev->proc_stats);
		nis_dev->proc_stats = NULL;
	}
	if (nis_dev->proc_stats_dir) {
		remove_proc_entry(device->name, get_nic_stats_proc_dir());
		nis_dev->proc_stats_dir = NULL;
	}
	if (nis_dev->stats) {
		nvmeib_io_stats_free(nis_dev->stats);
		nis_dev->stats = NULL;
	}
	_NT(trace_5_main_do_add_one, "@DEVICE_NAME failed.", device->name);
	NFOUT;
	return rv;
}

static void remove_nis(struct nvmeibs_dev *nis_dev)
{
	struct nvmeibs_ib_port *ib_port;
	int i;
	NFIN;

	_NT(trace_fin_main_remove_nis,
		"--> nis_dev=@PTR, @IB_DEV_NAME",
		nis_dev, (nis_dev && nis_dev->dev && nis_dev->dev->ib_dev) ?
		nis_dev->dev->ib_dev->name: "???");

//	/* Drain all pending cm still linked to listener conns-list */
//	if (nvmeibs_use_pcpu_cq)
//		nvmeib_dev_drain_cqs(nis_dev->dev);

	/* stop all listener so no new connection requests */
	nvmeib_rdma_stop_listen(nis_dev->ib_l_cm_id);
	for (i = 0; i < nvmeib_get_tcp_num_ports(); i++)
		nvmeib_rdma_stop_listen(nis_dev->iw_2nd_l_cm_id[i]);
	nvmeib_rdma_stop_listen(nis_dev->iw_prim_l_cm_id);
	list_for_each_entry(ib_port, &nis_dev->port_list, port_list_n) {
		if (ib_port->loop_listener) {
			_NT(trace_main_remove_nis, "Stopping loopback listener on IB device @IB_DEV_NAME:@PORT",
			   P2IB(ib_port)->name, ib_port->port);
			nvmeib_rdma_stop_listen(ib_port->loop_listener);
		}
	}
	/* first this one so no more queuing work on the port */
	if (nis_dev->event_handler) {
		nvmeib_rdma_unregister_event_handler(nis_dev->event_handler);
		nis_dev->event_handler = NULL;
	}

	/*
	 * Unregistering a host must happen after destroying sdev->cm_id
	 * such that no new NVMEIB_LOGIN_REQ information units can arrive while
	 * destroying the host.
	 */
	nvmeibs_get_devices(NULL);
	list_del(&nis_dev->nvmeibs_dev_list_n);
	if (nis_dev->device_used)
		--nvmeibs_dev_count;
	else
		--nvmeibs_unused_dev_count;
	nvmeibs_put_devices();

	_NT(remove_nis_list_del, "nis_dev=@PTR, @IB_DEV_NAME removed from list",
		nis_dev, (nis_dev && nis_dev->dev && nis_dev->dev->ib_dev) ?
		nis_dev->dev->ib_dev->name: "???");

	/* Tell toma that the nic is going down */
	nvmeibs_toma_report_event_nic_change(nis_dev, false);

	/* We continue without the devices lock as we are going to wait for
	   all port(s) clients' release and clients may try to take this lock.
	   Nevertheless, we wait for all refresh-port works to finish before
	   calling clear-ports as they modify the nis_dev->port_list */
	nvmeib_ref_release_wait(&nis_dev->n_refresh_port);

	/* clear all clients on all ports */
	clear_ports(nis_dev, nis_dev->device_used);
	if (nvmeibs_disk_locks_lock_dev_is_used(nis_dev)) {
		WARN_ON(1);
	}

	if (nis_dev->device_used) {
		/* clear all clients on all ports */
		//clear_ports(nis_dev, true);
		//mutex_unlock(&nvmeibs_dev_guard);
		/*unmap all locks made on this device*/
		nvmeibs_disk_lock_unmap_on_dev(nis_dev);

		/************************************************
		 * [Jared]: Called from deregister_disk_resources
		 * nvmeibs_serjio_unmap_jmd_cache_on_dev(nis_dev);
		 ************************************************/

		/* kill the listener descriptor */
		nvmeib_rdma_destroy_cm(nis_dev->ib_l_cm_id);
		nvmeib_rdma_destroy_cm(nis_dev->iw_prim_l_cm_id);
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++)
			nvmeib_rdma_destroy_cm(nis_dev->iw_2nd_l_cm_id[i]);
		/* free mapped disk resources */
		deregister_disks_resources(nis_dev);
		/* free the fmr pool */
		free_fmr(nis_dev);
		/* free the stats */
		if (nis_dev->proc_stats) {
			nvmeib_public_proc_remove(nis_dev->proc_stats);
			nis_dev->proc_stats = NULL;
		}
		if (nis_dev->proc_stats_dir) {
			remove_proc_entry(nis_dev->dev->ib_dev->name, get_nic_stats_proc_dir());
			nis_dev->proc_stats_dir = NULL;
		}
		if (nis_dev->stats) {
			nvmeib_io_stats_free(nis_dev->stats);
			nis_dev->stats = NULL;
		}
	} else {
		//clear_ports(nis_dev, false);
		//mutex_unlock(&nvmeibs_dev_guard);
	}

	//if (nvmeibs_disk_locks_lock_dev_is_used(nis_dev))
	//	WARN_ON(1);

	/* free the device */
	nvmeib_free(nis_dev->dev);
	kfree(nis_dev);

	_NT(trace_fout_main_remove_nis,
		"<-- nis_dev=@PTR", nis_dev);

	NFOUT;
}

static int remove_one_work_fn(void *param)
{
	struct nvmeibs_dev *nis_dev = param;
	remove_nis(nis_dev);
	return 0;
}

#if IB_REMOVE_EXTRA_ARG
static void remove_one(struct ib_device *device, void *client_data)
#else
static void remove_one(struct ib_device *device)
#endif
{
	struct nvmeibs_dev *nis_dev = ib_get_client_data(device, &nvmeibs_client);

	_NT(trace_fin_main_remove_one,
		"--> device=@PTR, @DEVICE_NAME", device, device->name);

	/* Clear the client data because we are freeing the nis_dev */
	ib_set_client_data(device, &nvmeibs_client, NULL);
	if (!nis_dev)
		_NT(trace_0_main_remove_one, "@DEVICE_NAME: nothing to do.", device->name);
	else {
		_NT(trace_1_main_remove_one, "@DEVICE_NAME: add remove_nis work "
									 "to main-wq, blocking", device->name);
		run_on_main_wq(remove_one_work_fn, nis_dev, true, true, NULL);
	}

	_NT(trace_fout_main_remove_one,
		"<-- device=@PTR, @DEVICE_NAME", device, device->name);
}

static void remove_ib(void)
{
	NFIN;
	/* let ib layer call remove_one per nic */
	ib_unregister_client(&nvmeibs_client);
	NFOUT;
}

int nvmeibs_remove_ib_device(struct ib_device *ib_dev)
{
#if IB_REMOVE_EXTRA_ARG
	remove_one(ib_dev, NULL);
#else
	remove_one(ib_dev);
#endif
	return 0;
}

static bool check_module_req_size(void)
{
	return nvmeibs_max_req_size >= NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE;
}

static char rdma_transport(enum rdma_link_layer l, enum nvmeib_dev_type dt)
{
	if (l == IB_LINK_LAYER_INFINIBAND)
		return NVMEIBS_NICS_CSV_TRANSPORT_INFINIBAND;
	else if (l == IB_LINK_LAYER_ETHERNET) {
		if (dt == DT_siw) return NVMEIBS_NICS_CSV_TRANSPORT_TCP;
		else return NVMEIBS_NICS_CSV_TRANSPORT_ROCE;
	}
	return NVMEIBS_NICS_CSV_TRANSPORT_UNDEF;
}

#if 0
/*details information about nics and ports. */
static ssize_t fill_nics_info(char *buffer, size_t len)
{
	int ndevs = 0;
	int count = 0;
	int nports, iport;
	int nic_enumerator = 0;
	struct ib_port_attr a;
	struct nvmeibs_dev *n;
	struct nvmeibs_ib_port *p;
	enum rdma_link_layer link_layer;
	struct list_head *nics;

	nics = nvmeibs_get_devices(&ndevs);
	count += scnprintf(buffer + count, len - count,
		"Number of nics=%d\n", ndevs);

	nic_enumerator = 0;

	list_for_each_entry(n, nics, nvmeibs_dev_list_n) {

		nports = 0;
		count +=  scnprintf(buffer + count, len - count,
			"\n  nic number=%d\n", nic_enumerator++);

		list_for_each_entry(p, &n->port_list, port_list_n)
			++nports;

		count += scnprintf(buffer + count, len - count,
			"  number of ports=%d\n", nports);
		iport = 0;

		list_for_each_entry(p, &n->port_list, port_list_n) {

			if ((ib_query_port(n->dev->ib_dev, p->port, &a)) < 0) {
				_NE(error_main_fill_nics_info, "ib_query_port() failed.");
				count = 0;
				goto out;
			}

			/* get the medium link layer (ethernetor  infiniband) */
			link_layer = rdma_port_get_link_layer(n->dev->ib_dev, p->port);
			count += scnprintf(buffer + count, len - count,
				"\n    port number=%d\n", iport++);
			count += scnprintf(buffer + count, len - count,
				"    port type=%c\n", rdma_transport(link_layer, n->dev->dev_type));
			count += scnprintf(buffer + count, len - count,
				"    subnet prefix=%#016llx\n",
				be64_to_cpu(p->gid.gid.global.subnet_prefix));
			count += scnprintf(buffer + count, len - count,
				"    interface_id=%#016llx\n",
				be64_to_cpu(p->gid.gid.global.interface_id));
			count += scnprintf(buffer + count, len - count,
				"    port=%d\n", p->port);
			count += scnprintf(buffer + count, len - count,
				"    partition key=%#x\n", p->pkey);
			count += scnprintf(buffer + count, len - count,
				"    state=%s\n",nvmeib_ib_port_state_t_to_s(a.state));
			count += scnprintf(buffer + count, len - count,
				"    active mtu=%d\n",ib_mtu_enum_to_int(a.active_mtu));
			count += scnprintf(buffer + count, len - count,
				"    max mtu=%d\n",ib_mtu_enum_to_int(a.max_mtu));
		}
	}

out:
	nvmeibs_put_devices();
	return count;
}
#endif

/**
 * get the gid that owns the given gid
 *
 * @param gid raw gid
 *
 * @return nvmeibs_dev*
 */
struct nvmeibs_dev *nvmeibs_gid_2_dev(const char *gid)
{
	struct nvmeibs_ib_port *p;
	struct nvmeibs_dev *n, *n_out = NULL;
	struct list_head *devices;

	NFIN;
	devices = nvmeibs_get_devices(NULL);
	list_for_each_entry(n, devices, nvmeibs_dev_list_n) {
		list_for_each_entry(p, &n->port_list, port_list_n) {
			_ND(trace_main_nvmeibs_gid_2_dev, "LOCKS: comparing @GID_STR to @GID", p->gid.gid_str, gid);
			if (!strncmp(gid, p->gid.gid_str, GUID_SIZE)) {
				_ND(trace_1_main_nvmeibs_gid_2_dev, "gid @GID found", gid);
				n_out = n;
				goto out;
			}
		}
	}
	_NT(trace_2_main_nvmeibs_gid_2_dev, "gid @GID not found", gid);
out:
	nvmeibs_put_devices();
	NFOUT;
	return n_out;
}

#define CORE_SERVER_RSRC_INFO_PROC_FRMT_VER 1
static ssize_t fill_rsrc_info(void *arg, char *buffer, size_t len)
{
	int count = 0, n_disks;
	struct list_head *disk_info_list;
	struct nvmeibs_disk_info *di;
	bool first = true;

	NFIN;
	count += scnprintf(buffer + count, len - count,
		"{ \"disks\": [\n");

	disk_info_list = nvmeibs_disk_get_disks(&n_disks);

	list_for_each_entry(di, disk_info_list, link) {
		if (!first)
			count += scnprintf(buffer + count, len - count,",");
		first = false;
		count +=
			nvmeibs_disk_print_disk_res_info(di, buffer + count, len - count);
	}
	count += scnprintf(buffer + count, len - count, "\n]");
	count += nvmeib_proc_add_json_proc_epilog(CORE_SERVER_RSRC_INFO_PROC_FRMT_VER, buffer + count, len - count);
	count += scnprintf(buffer + count, len - count,"}\n");
	nvmeibs_disk_put_disks();
	NFOUT;
	return count;
}

static ssize_t fill_disk_info(void *dummy, char *buffer, size_t len)
{
	struct nvmeibs_disk_info *di;
	struct nvmeibs_dev *n;
	int ndevs = 0;
	struct nvmeibs_ib_port *p;
	struct ib_port_attr a;
	enum rdma_link_layer l;
	int nports;
	int rv, count = 0;
	int n_disks;
	struct list_head *disk_info_list;
	struct list_head *nics;

	nics = nvmeibs_get_devices(&ndevs);
	count += scnprintf(buffer + count, len - count, "N%d|", ndevs);
	list_for_each_entry(n, nics, nvmeibs_dev_list_n) {
		nports = 0;
		list_for_each_entry(p, &n->port_list, port_list_n)
			++nports;
		count += scnprintf(buffer + count, len - count, "P%d|", nports);
		list_for_each_entry(p, &n->port_list, port_list_n) {
			if ((rv = ib_query_port(n->dev->ib_dev, p->port, &a)) < 0) {
				_NE(error_main_fill_disk_info, "ib_query_port() failed.");
				count = 0;
				nvmeibs_put_devices();
				goto out;
			}
			l = rdma_port_get_link_layer(n->dev->ib_dev, p->port);
			count += scnprintf(buffer + count, len - count,
				"0x%016llx%016llx,%d,%x,%c,%s,%d,%d|",
				be64_to_cpu(p->gid.gid.global.subnet_prefix),
				be64_to_cpu(p->gid.gid.global.interface_id),
				p->port, p->pkey,
				rdma_transport(l, n->dev->dev_type),
				nvmeib_ib_port_state_t_to_s(a.state),
				ib_mtu_enum_to_int(a.active_mtu),
				ib_mtu_enum_to_int(a.max_mtu));
		}
	}
	nvmeibs_put_devices();

	disk_info_list = nvmeibs_disk_get_disks(&n_disks);
	count += scnprintf(buffer + count, len - count, "D%d|", n_disks);
	list_for_each_entry(di, disk_info_list, link) {
		count += scnprintf(buffer + count, len - count,
			"%s,%lld,%lld,%d,%d,%d|",
			di->disk_id,
			di->blocks,
			di->hw_blocks,
			di->block_size,
			di->max_request_size,
			di->nsid);
	}
	count += scnprintf(buffer + count, len - count, "\n");
	nvmeibs_disk_put_disks();

out:
	if (count) {
		_NT(trace_main_fill_disk_info, "disk_info == @BUFFER_STR", buffer);
	}
	return count;
}

static noinline int scan_prefer_line(char *buf, size_t len, char *cmd,
	char **disk_name, char **nic_name, int *port)
{
	char *itr = buf, *ports = NULL, *start;
	int ret = 0;
	NFIN;

	*disk_name = NULL;
	*nic_name = NULL;
	*port = -1;

	buf[len - 1] = '\0';
	_NT(scan_prefer_line_t1, "Scanning @STR", buf);
	while (*itr && *itr == ' ')
		++itr;

	*cmd = *itr;
	if (*itr != 'r' && *itr != 's') {
		_NE(scan_prefer_line_e1, "Illegal command @CHAR (expected s or r)", *cmd);
		ret = -EINVAL;
		goto out;
	}
	_NT(scan_prefer_line_t2, "Command = @CHAR", *cmd);
	++itr;

	while (*itr && *itr == ' ')
		++itr;

	start = itr;

	while (*itr && *itr != ',' && *itr != ' ')
		++itr;

	*itr = '\0';
	*disk_name = start;
	_NT(scan_prefer_line_t3, "disk_name=@STR", *disk_name);
	++itr;

	while (*itr == ' ')
		++itr;

	start = itr;
	while (*itr && *itr != ':')
		++itr;
	if (*itr == ':') {
		*itr = '\0';
		*nic_name = start;
		_NT(scan_prefer_line_t4, "nic_name=@STR", *nic_name);
		++itr;
	}

	while (*itr == ' ')
		++itr;

	if (*itr)
		ports = itr;

	while (*itr && *itr != ' ')
		++itr;

	*itr = '\0';
	if (!*disk_name || !*nic_name || !ports) {
		_NE(scan_prefer_line_e2, "syntax error while parsing /proc/nvmeibs/arnic_prefer");
		ret = -EINVAL;
		goto out;
	}

	if (sscanf(ports, "%d", port) != 1) {
		_NE(scan_prefer_line_e3, "expected numeric value in @STR", ports);
		ret = -EINVAL;
		*port = -1;
	}

	_NT(scan_prefer_line_t5, "port=@INT", *port);

out:
	NFOUT;
	return ret;
}

/* parse disk prefered nic. Each rule takes this form:
   <disk>, <nic>:<port>
   for example CVMD5215012R400AGN.1, mlx4_0,1*/
static ssize_t arnic_prefer_set(void *arg, char *buf, size_t len)
{
	struct nvmeibs_ib_port *ib_port = NULL;
	struct nvmeibs_dev *n;
	struct list_head *disk_info_list, *nics;
	struct nvmeibs_disk_info *di;
	char *disk_name, *nic_name, cmd;
	int port = -1, n_devs, n_ports, i = 0;
	bool found = false;
	NFIN;

	if (scan_prefer_line(buf, len, &cmd, &disk_name, &nic_name, &port) < 0)
		goto out;

	disk_info_list = nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, disk_info_list, link) {
		if (!strcmp(disk_name, di->disk_id)) {
			found = true;
			break;
		}
	}
	nvmeibs_disk_put_disks();
	if (!found) {
		_NE(error_main_arnic_prefer_set, "Cannot locate disk @DISK_NAME", disk_name);
		goto rl_disk;
	}

	found = false;

	nics = nvmeibs_get_devices(&n_devs);

	_NT(trace_main_arnic_prefer_set, "Looking for nick @NIC_NAME", nic_name);
	list_for_each_entry(n, nics, nvmeibs_dev_list_n) {
		if (!strcmp(nic_name, n->dev->ib_dev->name)) {
			found = true;
			break;
		}
	}
	if (!found) {
		_NE(error_1_main_arnic_prefer_set, "Cannot locate nic @NIC_NAME", nic_name);
		goto rl_nics;
	}
	n_ports = n->ib_ports + n->roce_ports + n->iwarp_ports;
	if (port < 0 || port > n_ports) {
		_NE(error_2_main_arnic_prefer_set, "port @PORT out of range", port);
		goto rl_nics;
	}

	list_for_each_entry(ib_port, &n->port_list, port_list_n) {
		++i;
		if (i >= port)
			break;
	}

	nvmeibs_disk_get_disks(NULL);
	if (cmd == 's') {
		_NT(trace_1_main_arnic_prefer_set, "ib_port=@IB_PORT i=@II",ib_port, i);
		nvmeibs_disk_add_preferd_port(di, ib_port);
	} else
		nvmeibs_disk_remove_prefered_port(di, ib_port);
	nvmeibs_disk_put_disks();


rl_nics:
	nvmeibs_put_devices();
rl_disk:

out:
	NFOUT;
	return len;
}

#define CORE_SERVER_ARNIC_PREFER_PROC_FRMT_VER 1
static ssize_t arnic_prefer_fill(void *dummy, char *buffer, size_t len)
{
	int count = 0;
	struct nvmeibs_disk_info *di;
	struct nvmeibs_ib_port *pref_port;
	struct list_head *disk_info_list;
	struct nvmeibs_disk_prefered_port *p;
	struct list_head *prefered_ports;

	disk_info_list = nvmeibs_disk_get_disks(NULL);
	count += scnprintf(buffer + count, len - count, "disk, nic:port (gid)\n");
	list_for_each_entry(di, disk_info_list, link) {
		_NT(trace_main_arnic_prefer_fill, "disk:@DISK_ID_STR", di->disk_id);
		prefered_ports = nvmeibs_disk_prefered_ports(di);
		list_for_each_entry(p, prefered_ports, link) {
			pref_port = p->prefered_port;
			_NT(trace_1_main_arnic_prefer_fill, "pref_port=@PREF_PORT", pref_port);
			count += scnprintf(buffer + count, len - count,
				"%s, %s:%d (%s)\n",
				di->disk_id,  pref_port->nis_dev->dev->ib_dev->name,
				pref_port->port, pref_port->gid.gid_str);
		}
	}
	count += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_ARNIC_PREFER_PROC_FRMT_VER, buffer + count, len - count);

	nvmeibs_disk_put_disks();
	return count;
}




static void print_partial_string(const char *prefix, char *s, char *p)
{
	char tmp = *p;
	*p = 0;
	_NT(trace_main_print_partial_string, "@PREFIX: @STR", prefix, s);
	*p = tmp;
}



void print_separated_lines(const char *prefix, char *p, int count);
void print_separated_lines(const char *prefix, char *p, int count)
{
	char *e = p + count;
	char *s = p;

	if (nvmeib_debug_level() < MIN_TRACE)
		return;

	while (p != e) {
		if ((*p != '\n') && (*p != 0)) {
			p++;
			if (p == e)
				break;
			continue;
		}

		print_partial_string(prefix, s, p);

		if ((p == e) || (*p == 0))
			break;
		p++;    // skip newline
		s = p;
	}
}

extern ssize_t fill_disks(void *dummy, char *buffer, size_t len);

struct fill_parts_cb_args {
	char *buffer;
	int count;
	size_t len;
	struct nvmeibs_disk_info *di;
};

#define KERNEL_SECTOR_SHIFT 9

#if !KS_HAS_DISK_PART_ITER
static int fill_parts_cb(struct block_device *part, void *args)
{
	struct fill_parts_cb_args *cb_args = args;
#	if KS_GPT_SUPPORT
	const char *uuid = part->bd_meta_info->uuid;
	const char *volname = part->bd_meta_info->volname;
	#	else
	/* No GPT Support */
	const char *uuid = "N/A";
	const char *volname = "N/A";
	#	endif
	cb_args->count += scnprintf(cb_args->buffer + cb_args->count,
				    cb_args->len - cb_args->count,
			     "%s,%d,%llu,%llu,%s,%.36s\n",
			     cb_args->di->disk_id,
#if KS_HAS_BDEV_PARTNO
				bdev_partno(part),
#else
				/* A bit confusing - in block_device it is called bd_partno, in hd_struct it is just called partno */
				part->bd_partno,
#endif
			     (u64)part->bd_start_sect >> (cb_args->di->block_shift - KERNEL_SECTOR_SHIFT),
				    (u64)(part->bd_start_sect + bdev_nr_sectors(part)) >> (cb_args->di->block_shift - KERNEL_SECTOR_SHIFT),
				    uuid,
			     volname);
	return 0;
}
#else
int fill_parts_cb(struct hd_struct *part, void *args)
{
	struct fill_parts_cb_args *cb_args = args;
#	if KS_GPT_SUPPORT
	const char *uuid = part->info->uuid;
	const char *volname = part->info->volname;
#	else
	/* No GPT Support */
	const char *uuid = "N/A";
	const char *volname = "N/A";
#	endif
	cb_args->count += scnprintf(cb_args->buffer + cb_args->count,
		cb_args->len - cb_args->count,
		"%s,%d,%llu,%llu,%s,%.36s\n",
		cb_args->di->disk_id,
		/* A bit confusing - in block_device it is called bd_partno, in hd_struct it is just called partno */
		part->partno,
		(u64)part->start_sect >> (cb_args->di->block_shift - KERNEL_SECTOR_SHIFT),
		(u64)(part->start_sect + part->nr_sects) >> (cb_args->di->block_shift - KERNEL_SECTOR_SHIFT),
		uuid,
		volname);
	return 0;
}
#endif

static ssize_t fill_partitions(void *dummy, char *buffer, size_t len)
{
	struct nvmeibs_disk_info *di;
	struct list_head *disk_info_list;
	struct fill_parts_cb_args cb_args = {0};

	cb_args.buffer = buffer;
	cb_args.len = len;
	disk_info_list = nvmeibs_disk_get_disks(NULL);
	cb_args.count += scnprintf(buffer + cb_args.count, len - cb_args.count,
			   "disk_id,part_num,start,end,uuid,volname\n");
	list_for_each_entry(di, disk_info_list, link) {
		cb_args.di = di;
		nvmeib_public_call_for_each_disk_part(di->gendisk, fill_parts_cb,
						      &cb_args);
	}
	nvmeibs_disk_put_disks();
	return cb_args.count;
}

static ssize_t fill_port(struct nvmeibs_ib_port *p, bool m, char *buffer, size_t len)
{
	struct nvmeibs_dev *n = p->nis_dev;
	enum rdma_link_layer l;
	struct ib_port_attr a;
	ssize_t count = 0;
	int rv;

	if ((rv = ib_query_port(n->dev->ib_dev, p->port, &a)) < 0) {
		_NE(error_fill_port, "ib_query_port() failed.");
		goto out;
	}

	l = rdma_port_get_link_layer(n->dev->ib_dev, p->port);
	count = scnprintf(buffer + count, len - count,
			"%s,0x%016llx%016llx,%d,%#x,%c,%s,%d,%d,%d,%s,%s,%s,%s,0x%016llx%016llx\n",
		n->dev->ib_dev->name,
		/* For IB, RoCE and Multi (i.e. the representing device in this list is
		   the RoCE device), omit subnet-prefix. For TCP only, subnet-prefix
		   holds the ID which is the MAC address, cant omit */
		rdma_transport(l, n->dev->dev_type) == NVMEIBS_NICS_CSV_TRANSPORT_TCP ?
					  be64_to_cpu(p->hw_gid.global.subnet_prefix) : 0,
		be64_to_cpu(p->hw_gid.global.interface_id),
		p->port, p->pkey,
		p->is_multi_transport && m ?
					   NVMEIBS_NICS_CSV_TRANSPORT_MULTI :
					   rdma_transport(l, n->dev->dev_type),
		nvmeib_ib_port_state_t_to_s(a.state),
		ib_mtu_enum_to_int(a.active_mtu),
		ib_mtu_enum_to_int(a.max_mtu),
		p->gid.gid_index,
		p->gid.gid_type == NVMEIB_GID_TYPE_ROCE_V2 ? "true" : "false",
		p->gid.net_type == NVMEIB_NETWORK_IPV6 ? "true" : "false",
		p->port_used ? "true" : "false",
		p->gid.ndev_name,
		be64_to_cpu(p->gid.gid.global.subnet_prefix),
		be64_to_cpu(p->gid.gid.global.interface_id));

out:
	return count;
}

static ssize_t fill_dot_nics(void *dummy, char *buffer, size_t len)
{
	struct nvmeibs_dev *n;
	int ndevs = 0;
	struct nvmeibs_ib_port *p;
	int count = 0;
	struct list_head *nics;
	int i = 0;

	nics = nvmeibs_get_devices(&ndevs);
	count += scnprintf(buffer + count, len - count,
		"%s\n", NVMEIBS_NICS_CSV_HEADER);
	list_for_each_entry(n, nics, nvmeibs_dev_list_n) {
		list_for_each_entry(p, &n->port_list, port_list_n) {
			_NT(t0_fill_dot_nics,
				"[@INT32_02] ndev=@STR, @IB_NAME:@PORT, ",
				i, p->gid.ndev_name, n->dev->ib_dev->name, p->port);
			i++;
			count += fill_port(p, false, buffer + count, len - count);
		}
	}
	nvmeibs_put_devices();

	return count;
}

static ssize_t fill_nics(void *dummy, char *buffer, size_t len)
{
	struct nvmeibs_dev *n;
	int ndevs = 0;
	struct nvmeibs_ib_port *p, *u, *t;
	int count = 0;
	struct list_head *nics;
	LIST_HEAD(uniq_list);
	int i = 0;

	nics = nvmeibs_get_devices(&ndevs);
	count += scnprintf(buffer + count, len - count,
		"%s\n", NVMEIBS_NICS_CSV_HEADER);

	/* build unique list per ndev represented by the roce (not siw) rdma-dev.
	   This is not efficient but this is just a /proc && few nics, meh... */
	list_for_each_entry(n, nics, nvmeibs_dev_list_n) {
		list_for_each_entry(p, &n->port_list, port_list_n) {
			_NT(t0_fill_nics,
				"[@INT32_02] ndev=@STR, @IB_NAME:@PORT, ",
				i, p->gid.ndev_name, n->dev->ib_dev->name, p->port);
			i++;
			p->is_multi_transport = false;
			list_for_each_entry_safe(u, t, &uniq_list, uniq_link) {
				if (!memcmp(u->gid.ndev_name, p->gid.ndev_name,
							sizeof(p->gid.ndev_name))) {
					enum rdma_link_layer u_layer = rdma_port_get_link_layer(u->nis_dev->dev->ib_dev, u->port);
					enum rdma_link_layer p_layer = rdma_port_get_link_layer(p->nis_dev->dev->ib_dev, p->port);
					char u_transport = rdma_transport(u_layer, u->nis_dev->dev->dev_type);
					char p_transport = rdma_transport(p_layer, p->nis_dev->dev->dev_type);

					_NT(t1_fill_nics, "ndev=@STR, link-layer '@CHAR' vs '@CHAR'",
						u->gid.ndev_name, u_transport, p_transport);

					if (u->is_multi_transport) {
						_NT(t2_fill_nics, "unexp, already marked mixed ?!");
						break;
					}
					if (p_transport == NVMEIBS_NICS_CSV_TRANSPORT_TCP) {
						if (u_transport == NVMEIBS_NICS_CSV_TRANSPORT_ROCE) {
							u->is_multi_transport = true;
							goto next_port;
						}
						else {
							_NT(t3_fill_nics, "unexp transports combo");
						}
					}
					else if (p_transport == NVMEIBS_NICS_CSV_TRANSPORT_ROCE) {
						if (u_transport == NVMEIBS_NICS_CSV_TRANSPORT_TCP) {
							p->is_multi_transport = true;
							list_replace_init(&u->uniq_link, &p->uniq_link);
							goto next_port;
						}
						else {
							_NT(t4_fill_nics, "unexp transports combo");
						}
					}
					else {
						_NT(t5_fill_nics, "unexp transport ?!");
					}
					break;
				}
			}
			/* add port to uniq list */
			list_add_tail(&p->uniq_link, &uniq_list);
next_port:;
		}
	}

	list_for_each_entry_safe(u, t, &uniq_list, uniq_link) {
		count += fill_port(u, true, buffer + count, len - count);
		list_del_init(&u->uniq_link);
		u->is_multi_transport = false;
	}

	nvmeibs_put_devices();
	return count;
}

static ssize_t fill_version_json(void *dummy, char *buffer, size_t len)
{
	int count = 0;
	count += scnprintf(buffer + count, len - count,
		"{\"module\" : \"srvr\", \"commit\" : \"%llx\", \"release\" : \"%s\", \"version\" : \"%s\", \"build_number\" : \"%s\", \"distro\" : \"%s\"}\n",
		(u64)COMMIT_ID, __stringify(NVMESH_RELEASE), __stringify(NVMESH_VERSION), __stringify(BUILD_NUMBER), __stringify(BUILD_DISTRO));
	return count;
}

#ifdef NVMEIBS_CLIENTS_PROC
struct fill_clients_ctx {
	int count;
	size_t len;
	char *buffer;

};

static int fill_clients_fc(struct nvmeibs_client *cl, void *arg)
{
	struct fill_clients_ctx *ctx = arg;
	char gid[GUID_SIZE];
	int dying;

	if (cl->ib_port)
		format_gid(&cl->ib_port->gid.hw_gid, gid);
	else
		strcpy(gid, "0000:0000:0000:0000:0000:0000:0000:0000");

	dying = atomic_read(&cl->dying);
	ctx->count += scnprintf(ctx->buffer + ctx->count,
				ctx->len - ctx->count,
				"%s, %s, %s, %s, %d, %d, %d, %llu \n",
				cl->host_name, cl->disk_name, cl->name, gid,
				wq_pid(cl->wq), wq_pid(cl->remove_wq), dying,
				dying? ((jiffies - cl->dying_start_time) * 1000 / HZ) : 0);

	return 0;
}

#define CORE_SERVER_CLIENTS_SUMMARY_PROC_FRMT_VER 1
static ssize_t fill_clients(void *dummy, char *buffer, size_t len)
{
	struct fill_clients_ctx ctx = {
		.buffer = buffer,
		.count = 0,
		.len = len,
	};

	NFIN;

	ctx.count += scnprintf(ctx.buffer + ctx.count, ctx.len - ctx.count,
		"host_name, disk_name, (full) name, gid, wq_pid, remove_wq_pid, dying, dying_time(msec)\n");

	nvmeibs_cdb_all_fast_call(fill_clients_fc, &ctx);
	nvmeibs_cdb_dying_call(fill_clients_fc, &ctx);

	ctx.count += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_CLIENTS_SUMMARY_PROC_FRMT_VER, ctx.buffer + ctx.count, ctx.len - ctx.count);

	NFOUT;

	return ctx.count;
}

struct chng_clients_ctx {
	char *host_name;
	char *disk_name;
};

static int chng_clients_fc(struct nvmeibs_client *cl, void *arg)
{
	struct chng_clients_ctx *ctx = arg;

	if (strncmp(cl->host_name, ctx->host_name,
		     NVMEIB_HOST_NAME_LEN) ||
	    strncmp(cl->disk_name, ctx->disk_name,
		     NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE))
		return 0;

	_NT(trace_main_chng_clients_fc, "Removing client '@CL_NAME'", cl->name);
	_NT(trace_1_main_chng_clients_fc, "Will call free client @CL @CID", cl, cl->cid);
	nvmeibs_ib_port_free_client(cl->ib_port, cl->cid, NVMEIBS_LOGOUT_REASON_CHANGE_CLNT);

	return 1;
}

static ssize_t chng_clients(void *arg, char *buf, size_t len)
{
	struct chng_clients_ctx ctx = {
		.host_name = "",
		.disk_name = "",
	};
	int rv = -EINVAL;

	NFIN;

	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	//host-name
	while (buf && *(ctx.host_name = strsep(&buf, " ")) == '\0');
	if (*ctx.host_name == '\0') {
		_NE(error_main_chng_clients, "No ctx.host_name");
		goto out;
	}
	if (strlen(ctx.host_name) > NVMEIB_HOST_NAME_LEN) {
		_NE(error_1_main_chng_clients, "Invalid length of ctx.host_name (@STRLEN)", strlen(ctx.host_name));
		goto out;
	}

	//disk-name
	while (buf && *(ctx.disk_name = strsep(&buf, " ")) == '\0');
	if (*ctx.disk_name == '\0') {
		_NE(error_2_main_chng_clients, "No ctx.disk_name");
		goto out;
	}
	if (strlen(ctx.disk_name) > NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE) {
		_NE(error_3_main_chng_clients, "Invalid length of ctx.disk_name (@STRLEN)", strlen(ctx.disk_name));
		goto out;
	}

	//lookup client by ctx.host_name and ctx.disk_name
	_NT(trace_main_chng_clients, "Process request to remove client: "
	   "host '@HOST_NAME' (len @STRLEN), disk '@DISK_NAME' (len @STRLEN) ",
	   ctx.host_name, strlen(ctx.host_name), ctx.disk_name, strlen(ctx.disk_name));

	rv = nvmeibs_cdb_all_fast_call(chng_clients_fc, &ctx);
	if (!rv) {
		_NT(trace_1_main_chng_clients, "No such client: host @HOST_NAME disk @DISK_NAME", ctx.host_name, ctx.disk_name);
		rv = -ENXIO;
	} else if (rv != 1) {
		rv = -ENXIO;
	} else {
		rv = len;
	}

out:
	NFOUT;
	return rv;
}
#endif /* NVMEIBS_CLIENTS_PROC */

static int release_disk_clients(struct nvmeibs_disk_info *di, enum nvmeibs_logout_reason reason);
static int fill_dot_debug_fn(void *param)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *disk;
	int rv = 0;
	(void)param;
	NFIN;

	if (0) {
		//CAUTION: not locking disks, dont run this during disk unplug !!!
		_NI(trace_main_fill_dot_debug_fn, "Per disk, release all its clients ...");
		disks = nvmeibs_disk_get_disks_nolock(NULL);
		list_for_each_entry(disk, disks, link) {
			_NI(trace_1_main_fill_dot_debug_fn, "Disk @DISK_ID_STR, release all clients ...", disk->disk_id);
			rv = release_disk_clients(disk, NVMEIBS_LOGOUT_REASON_DEBUG);
			_NI(trace_2_main_fill_dot_debug_fn, "rv=@RV", rv);
		}

		//Reset dying bit
		disks = nvmeibs_disk_get_disks(NULL);
		list_for_each_entry(disk, disks, link) {
			_NI(trace_3_main_fill_dot_debug_fn, "Disk @DISK_ID_STR, reset dying and init refcnt", disk->disk_id);
			disk->dying = false;
			nvmeib_ref_init(&disk->nref);
		}
		nvmeibs_disk_put_disks();
	}

	NFOUT;
	return 0;
}

//TBD:
//trigger the debug operation from write (not read) of op-id; remove
//the skipping code below.


static ssize_t fill_dot_debug(void *dummy, char *buffer, size_t len)
{
	int count = 0;
	static int i = 0;
	(void)dummy;

	if (nvmeib_debug_level() < MIN_TRACE) {
		count += scnprintf(buffer + count, len - count,
			"module debug level insufficient\n");
		goto out;
	}

	if (i++ & 0x1) {
		//_NI(fill_dot_debug_i1, "Skipping ...");
		goto out;
	}

	_NI(trace_main_fill_dot_debug, "Collecting data ...");
	count += scnprintf(buffer + count, len - count,
		"see dmesg...\n");
	if (run_on_main_wq(fill_dot_debug_fn, NULL, true, true, NULL))
		_NI(trace_1_main_fill_dot_debug, "Fail fill_dot_debug_fn");
	_NI(trace_2_main_fill_dot_debug, "Done");

out:
	return count;
}

#define CORE_SERVER_SHARED_CQ_PROC_FRMT_VER 1
static ssize_t fill_shared_cq(void *dummy, char *buffer, size_t len)
{
	struct list_head *nics;
	struct nvmeibs_dev *nic_dev;
	int count = 0;

#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	nics = nvmeibs_get_devices(NULL);
	count += nvmeib_dev_cq_stat_hdr(buffer, len);
	list_for_each_entry(nic_dev, nics, nvmeibs_dev_list_n)
		count += nvmeib_dev_cq_stat(nic_dev->dev, buffer + count, len - count);
	nvmeibs_put_devices();
	count += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_SHARED_CQ_PROC_FRMT_VER, buffer + count, len - count);
#undef BUF_ADD
	return count;
}

static void notify_client_server_close(void)
{
	NFIN;

	nvmeib_local_server_close_client();
	NFOUT;
}

struct nvmeibs_remove_all_client_ctx {
	int calculated_num_of_clients;
	enum nvmeibs_logout_reason reason;
};

static int nvmeibs_remove_all_clients_fc(struct nvmeibs_client *cl, void *arg)
{
	struct nvmeibs_remove_all_client_ctx *ctx = arg;
	(ctx->calculated_num_of_clients)++;

	_NT(trace_main_nvmeibs_remove_all_clients_fc, "NTNA: Removing client '@CL_NAME'", cl->name);
	_NT(trace_1_main_nvmeibs_remove_all_clients_fc, "Will call free client @CL @CID", cl, cl->cid);

	nvmeibs_ib_port_free_client(cl->ib_port, cl->cid, ctx->reason);

	return 0;
}


/*disconnects all connected clients*/
int nvmeibs_remove_all_clients(bool toma_connecting, bool interruptible, enum nvmeibs_logout_reason reason)
{
	int rv;
	struct nvmeibs_remove_all_client_ctx ctx = {.calculated_num_of_clients = 0, .reason = reason};

	NFIN;
	rv = nvmeibs_cdb_set_no_new_clients(nvmeibs_remove_all_clients_fc, &ctx, true);
	if (rv >= 0) {
		_NT(trace_main_nvmeibs_remove_all_clients,
		    "NTNA: calculated number of clients @CALCULATED_NUM_OF_CLIENTS and the stored @NUM_OF_CLIENTS",
		    ctx.calculated_num_of_clients, rv);
	} else {
		BUG_ON(rv != -EALREADY);
		/* Removing all clients work has already been started.
		 * Just wait on the completion. */
		_NT(trace_3_main_nvmeibs_remove_all_clients, "Remove all clients already begun, waiting on existing completion");
	}

	_NT(trace_4_main_nvmeibs_remove_all_clients, "NTNA: Going to wait for clients to be released. killable = @TRUE_FALSE_STR",
		interruptible ? "true" : "false");
	rv = nvmeibs_cdb_wait_no_clients(interruptible);
	if (rv < 0) {
		if (rv == -EINTR)
			_NT(trace_5_main_nvmeibs_remove_all_clients, "Interrupted waiting for all clients to release");
		else if (rv == -ETIMEDOUT)
			_NE(error_1_main_nvmeibs_remove_all_clients, "Timeout waiting for all clients to release");
		else
			_NE(error_main_nvmeibs_remove_all_clients, "Error waiting for all clients to release (@RV)", rv);
	} else {
		_NT(trace_6_main_nvmeibs_remove_all_clients, "All clients are down");
		if (rv == 0 && toma_connecting) {
			_NT(trace_7_main_nvmeibs_remove_all_clients, "NTNA: New TOMA connecting, allowing new clients");
			nvmeibs_cdb_set_allow_new_clients();
		}
	}
	NFOUT;
	return rv;
}

#ifdef REMOVE_CLIENT_PROC
static int remove_clients_fc(struct nvmeibs_client *cl, void *arg)
{
	char *node_name = arg;

	if (!strnstr(cl->name, node_name, NVMEIB_HOST_NAME_LEN))
		return 0;

	_ND(remove_clients_fc_d1, "Removing client @STR", node_name);
	_ND(remove_clients_fc_d2, "Will call free client @PTR @INT", cl, cl->cid);

	nvmeibs_ib_port_free_client(cl->ib_port, cl->cid, NVMEIBS_LOGOUT_REASON_DEBUG);

	return 0;
}

static void remove_clients(struct workqe_struct *work)
{
	struct snode_workq *swork =
		container_of(work, struct snode_workq, work);
	unsigned long flags;

	NFIN;
	flags = nvmeibs_cdb_lock();

	if (strnstr(nvmeibs_node_name, swork->node_name,
		NVMEIB_HOST_NAME_LEN)) {

	}
	else
		nvmeibs_cdb_all_fast_call_locked(remove_clients_fc, swork->node_name);

	nvmeibs_cdb_unlock(flags);
	NFOUT;
}

static ssize_t remove_client_node(void *arg, char *buf, size_t count)
/*
static ssize_t remove_client_node(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
*/
{
	struct snode_workq w;
	char *c = (char *)buf;
	int i = 0;
	int j = NVMEIB_HOST_NAME_LEN < count ?
		NVMEIB_HOST_NAME_LEN : count;

	NFIN;
	memset(&w, 0, sizeof(w));
	WQ_INIT_WORK(&w.work, remove_clients);
	while (*c && *c != '\n' && i < j)
		w.node_name[i++] = *c++;
	_ND(remove_client_node_d1, "Trying to remove node @STR", w.node_name);
	nvmeibs_add_work(&w.work);
	wq_flush(main_wq);
	NFOUT;
	return count;
}
#endif
/*
 * valid codes are:
 * 0x000 : Invalid Submission Queue
 * 0x001 : Invalid Doorbell Write Value
 * 0x002 : Diagnostic Failure
 * 0x003 : Persistent Internal Device Error
 * 0x004 : Transient Internal Device Error
 * 0x005 : Firmware Image Load Error
 * 0x100 : Device Reliability
 * 0x101 : Temperature Above Threshold
 * 0x102 : Spare Below Threshold
 */
void nvmeibs_async_to_mgmt(struct nvmeibs_disk_info *info, int code)
{
	_NW(warn_main_nvmeibs_async_to_mgmt, "async event on drive @DISK_ID_STR, code=@CODE", info->disk_id, code);
}

static bool nvmeibs_is_local_disk(char *name, int *md_size, bool *md_extd)
{
	struct list_head *disk_info_list;
	struct nvmeibs_disk_info *di;
	bool found = false;
	NFIN;

	disk_info_list = nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, disk_info_list, link) {
		if (strcmp(di->disk_id, name) == 0) {
			_NT(trace_main_nvmeibs_is_local_disk, "Target found disk @NAME", name);
			if (md_size)
				*md_size = di->metadata;
			if (md_extd)
				*md_extd = di->mtdt_extd;
			found = true;
			break;
		}
	}
	nvmeibs_disk_put_disks();

	NFOUT;
	return found;
}

static int local_cmd(struct nvmeib_local_disk *disk,
	struct nvmeibs_nvme_req *req)
{
	int rv;
	NFIN;

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibs_local_skip_disk_access)) {
		if (req->cb) {
			req->cb(req->arg, 0, 0);
			rv = 0;
		}
		else {
			rv = -EINVAL;
		}
	}
	else
#endif
	rv = submit_local_cmd(disk->p, req);

	NFOUT;
	return rv;
}

/* TBD: Move to seperate file */
static int nvmeibs_handle_io_piggyb_cmd(struct nvmeib_local_disk *disk,
									 struct nvmeib_io_piggyb_cmd *piggyb_cmd)
{
	int rv;

	NFIN;
	switch (piggyb_cmd->opcode) {
	case NVMEIB_IO_PIGGYB_JMDC_WRITE:
		rv = nvmeibs_serjio_jmdc_entry_set(
			disk->jrange_handle, piggyb_cmd->jmdc_wr.rng_gen_id, 1, &piggyb_cmd->jmdc_wr.ent_idx,
			&piggyb_cmd->jmdc_wr.ent_md, &piggyb_cmd->jmdc_wr.jmdc_val.jblks_md[0], false);
		break;
	case NVMEIB_IO_PIGGYB_LOCK:
		/* TBD: Implement */
		rv = -ENOTSUPP;
		break;
	default:
		rv = -EINVAL;
	}

	NFOUT;
	return rv;
}

static struct device *dma_device(struct nvmeib_local_disk *dev)
{
	struct nvmeibs_disk_info *di = dev->p;
	return get_nvme_dma_device(di->dev);
}

struct __nvmeibs_bail_wq {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
};

/* c-disk adds this work after pausing the disk and before waiting for pause-ack i.e. for zero in-transfer */
static void __bail_local_async_cookie_ch_from_client_wq(struct workqe_struct *_work) {
	struct cl_external_workq *work =
	    container_of(_work, struct cl_external_workq, work);

	NFIN;

	_NT(trace_bail_local_async_cookie_ch_from_client_wq, "Bailing local client async cookie store");

	//use cookie_store_remove_local_ch() ?
	nvmeibs_async_cookie_store_bail_all_chid(&work->cl->cookie_store,
	                                         NVMEIBS_ASYNC_LOCAL_CHANNEL);
	nvmeibs_async_cookie_store_wait_chid(&work->cl->cookie_store,
	                                     NVMEIBS_ASYNC_LOCAL_CHANNEL);
	kfree(work);

	NFOUT;
}

static void nvmeibs_bail_local_async_cookie_ch(u32 cid) {
	struct cl_external_workq *work = NULL;
	int rv;

	NFIN;

	if (!(work = kzalloc(sizeof(*work), GFP_KERNEL))) {
		_NW(error_bail_local_async_cookie_ch,
		    "Out of mem during bail, will ignore bail.");
		rv = -ENOMEM;
		goto err;
	}

	WQ_INIT_WORK(&work->work, __bail_local_async_cookie_ch_from_client_wq);
	if ((rv = nvmeibs_client_add_work_external(cid, work))) {
		_NW(error_1_bail_local_async_cookie_ch,
		    "Error trying to add client work: @RV", rv);
		goto err;
	}

	goto out;

err:
	kfree(work);
out:
	NFOUT;
}

static struct nvmeib_local_server local_server = {
	.is_local_disk = nvmeibs_is_local_disk,
	.cl_register = nvmeibs_client_ldisk_register,
	.cl_unregister = nvmeibs_client_ldisk_unregister,
	.cl_alloc_locks = nvmeibs_client_ldisk_alloc_locks,
	.cl_alloc_jrnl_rng = nvmeibs_client_ldisk_alloc_jrnl_rng,
	.local_cmd = local_cmd,
	.dma_device = dma_device,
	.gen_cmd = nvmeibs_handle_gen_cmd,
	.io_piggyb_cmd = nvmeibs_handle_io_piggyb_cmd,
	.bail_async_cookie_ch = nvmeibs_bail_local_async_cookie_ch,
	.send_msg_to_process = nvmeibs_send_msg_to_user_porcess,
};

#ifdef REMOVE_CLIENT_PROC
static struct nvmeib_public_procfs_ent *remove_procfs_ent = NULL;
#endif
static struct nvmeib_public_procfs_ent *disk_info_proc = NULL;
static struct nvmeib_public_procfs_ent *rsrc_info_proc = NULL;
static struct nvmeib_public_procfs_ent *disks_proc = NULL;
static struct nvmeib_public_procfs_ent *serjios_proc = NULL;
static struct nvmeib_public_procfs_ent *partitions_proc = NULL;
//a proc entry that selects given nics as target admin channel for a given disk
static struct nvmeib_public_procfs_ent *disks_nics_prefer_proc = NULL;
static struct nvmeib_public_procfs_ent *nics_proc = NULL;
static struct nvmeib_public_procfs_ent *dot_nics_proc = NULL;
static struct nvmeib_public_procfs_ent *version_proc = NULL;
#ifdef NVMEIBS_CLIENTS_PROC
static struct nvmeib_public_procfs_ent *clients_proc = NULL;
#endif
static struct nvmeib_public_procfs_ent *dot_debug_proc = NULL;
static struct nvmeib_public_procfs_ent *shared_cq_proc = NULL;
static struct nvmeib_public_procfs_ent *memmgr_info_proc = NULL;

static const char *gids_proc_dir_name = "nic_gids";
struct proc_dir_entry *nvmeibs_gids_proc_dir = NULL;

static const char *nic_stats_dir_name = "nic_stats";
static struct proc_dir_entry *nic_stats_proc_dir = NULL;

static struct proc_dir_entry *get_nic_stats_proc_dir(void)
{
	return nic_stats_proc_dir;
}

static int gids_proc_create(void)
{
	if (!nvmeibs_proc_dir || nvmeibs_gids_proc_dir)
		return -EINVAL;

	nvmeibs_gids_proc_dir = proc_mkdir(gids_proc_dir_name, nvmeibs_proc_dir);

	return nvmeibs_gids_proc_dir ? 0 : -1;
}

static void gids_proc_remove(void)
{
	if (nvmeibs_gids_proc_dir) {
		remove_proc_entry(gids_proc_dir_name, nvmeibs_proc_dir);
		nvmeibs_gids_proc_dir = NULL;
	}
}

static void create_proc_files(void)
{
	struct proc_dir_entry *sclients_dir = NULL;
	NFIN;
#ifdef REMOVE_CLIENT_PROC
	remove_procfs_ent = nvmeib_public_proc_create("remove_client", nvmeibs_proc_dir,
		NULL, remove_client_node, NULL);
#endif

	rsrc_info_proc = nvmeib_public_proc_create("rsrc_info.json",
		nvmeibs_proc_dir, &fill_rsrc_info, NULL, NULL);
	disk_info_proc = nvmeib_public_proc_create("disk_info",
		nvmeibs_proc_dir, &fill_disk_info, NULL, NULL);
	disks_proc = nvmeib_public_proc_create("disks.csv",
		nvmeibs_proc_dir, &fill_disks, NULL, NULL);
	serjios_proc = nvmeib_public_proc_create("serjios.csv",
		nvmeibs_proc_dir, &fill_serjios, NULL, NULL);
	partitions_proc = nvmeib_public_proc_create("partitions.csv",
					nvmeibs_proc_dir, &fill_partitions, NULL, NULL);
	disks_nics_prefer_proc = nvmeib_public_proc_create("arnic_prefer",
		nvmeibs_proc_dir, &arnic_prefer_fill, &arnic_prefer_set, NULL);
	nics_proc = nvmeib_public_proc_create("nics.csv",
		nvmeibs_proc_dir, &fill_nics, NULL, NULL);
	dot_nics_proc = nvmeib_public_proc_create(".nics.csv",
		nvmeibs_proc_dir, &fill_dot_nics, NULL, NULL);
	version_proc = nvmeib_public_proc_create("version",
		nvmeibs_proc_dir, &fill_version_json, NULL, NULL);
	dot_debug_proc = nvmeib_public_proc_create(".debug",
		nvmeibs_proc_dir, &fill_dot_debug, NULL, NULL);
	if (nvmeibs_use_pcpu_cq)
		shared_cq_proc = nvmeib_public_proc_create("shared_cq",
			nvmeibs_proc_dir, &fill_shared_cq, NULL, NULL);
	memmgr_info_proc = nvmeib_public_proc_create("memmgr_info",
		nvmeibs_proc_dir, &nvmeibs_memmgr_metrics_info, NULL, NULL);

	gids_proc_create();

	nic_stats_proc_dir = proc_mkdir(nic_stats_dir_name, nvmeibs_proc_dir);

	sclients_dir = nvmeibs_client_proc_mkdir(nvmeibs_proc_dir);
#ifdef NVMEIBS_CLIENTS_PROC
	clients_proc = nvmeib_public_proc_create("summary",
		sclients_dir, &fill_clients, &chng_clients, NULL);
#endif

	NFOUT;
}

int nvmeibs_start_roce(void)
{
	struct nvmeib_rdma_listen_params params = {0};
	int rv = 0;

	NFIN;
	if (roce_cm) {
		_NE(error_main_nvmeibs_start_roce, "RoCE already initialized");
		rv = -EEXIST;
		goto out;
	}
	_ND(trace_main_nvmeibs_start_roce, "initializing roce!!!!");
	/*initialize roce listener*/
	params.type = _rdma_roce;
	params.roce.port = NVMEIB_PORT_ID;
	params.roce.ipv4_only = roce_ipv4_only;
	params.new_connection = cm_req_recv;
	params.on_peer_event = client_cm_event;
	params.context = NULL; /* lets leave it NULL for now... we will have to give
							* a cookie afterwards.....
							*/
	if (!(roce_cm = nvmeib_rdma_listen(&params))) { /* listen to roce events*/
		_NE(error_1_main_nvmeibs_start_roce, "RoCE initialization failed");
		nvmeib_rdma_destroy_cm(roce_cm);
		roce_cm = NULL;
		rv = -EFAULT;
	}

out:
	NFOUT;
	return rv;
}

void nvmeibs_init_loopback_listener(struct nvmeibs_ib_port *ib_port)
{
	struct nvmeib_rdma_listen_params params = {0};

	NFIN;
	/*initialize roce listener*/
	params.type = _rdma_lb;
	params.dev = P2NV(ib_port);
	params.port = ib_port->port;
	params.lb.pkey_index = 0;
	params.lb.link_layer = ib_port->layer;
	params.lb.gid.global = ib_port->gid.gid.global;
	params.lb.gid_index = ib_port->gid.gid_index;
	memcpy(params.lb.roce_mac, ib_port->gid.roce_mac, ETH_ALEN);
	params.new_connection = cm_req_recv;
	params.on_peer_event = client_cm_event;
	params.context = ib_port->nis_dev;

	if (!(ib_port->loop_listener = nvmeib_rdma_listen(&params))) {
		_NE(error_main_nvmeibs_init_loopback_listener, "Failed to initialize Loopback Listener");
	}
	NFOUT;
}

/* ensure that all devices have the same layer type
   in general if no filter is used and we are in mixed system
   the infiniband cards will be used.
   if filters are imposed on the system, and then selected devices are mixed,
   error will be returned*/
static int ensure_devs_uniformity(void)
{
	int rv = 0;
	struct nvmeibs_dev *nis_dev, *nis_tmp;
	struct nvmeibs_ib_port *ib_port, *ib_port_tmp;
	bool mixed = false;


	NFIN;

	if (nvmeibs_dev_count == 0) {
		_NT(trace_main_ensure_devs_uniformity, "No RDMA devices found so far");
		goto out;
	}

	nvmeibs_get_devices(NULL);
	list_for_each_entry(nis_dev, &nvmeibs_dev_list, nvmeibs_dev_list_n) {
		list_for_each_entry(ib_port, &nis_dev->port_list, port_list_n) {
			switch (nvmeibs_selected_layer) {
			case IB_LINK_LAYER_UNSPECIFIED:
				if (ib_port->layer == IB_LINK_LAYER_INFINIBAND)
					nvmeibs_selected_layer = IB_LINK_LAYER_INFINIBAND;
				else if(ib_port->layer == IB_LINK_LAYER_ETHERNET)
					nvmeibs_selected_layer = IB_LINK_LAYER_ETHERNET;
				break;
			case IB_LINK_LAYER_INFINIBAND:
				if(ib_port->layer == IB_LINK_LAYER_ETHERNET)
					mixed = true;
				break;
			case IB_LINK_LAYER_ETHERNET:
				if (ib_port->layer == IB_LINK_LAYER_INFINIBAND) {
					nvmeibs_selected_layer = IB_LINK_LAYER_INFINIBAND;
					mixed = true;
				}
				break;
			default:
				_NE(error_main_ensure_devs_uniformity, "Unknown layer type @NVMEIBS_SELECTED_LAYER", nvmeibs_selected_layer);
				BUG();
			}
		}
	}
	nvmeibs_put_devices();

	_NT(trace_1_main_ensure_devs_uniformity, "Selected link layer is @NVMEIBS_SELECTED_LAYER mixed = @TRUE_FALSE_STR", nvmeibs_selected_layer,
		mixed ? "true" : "false");

	if (mixed) {
		if (!list_empty(&used_dev_list))
			_NE(error_1_main_ensure_devs_uniformity, "NVMesh configured nics list \"@NVMEIBS_FILTER_PORTS\" attempts to run on both Infiniband and RoCE devices. This is not supported.", nvmeibs_filter_ports);
		else
			_NE(error_2_main_ensure_devs_uniformity, "NVMesh does not support running on both Infiniband and RoCE devices simultaneously, please specify the desired NICs using the nvmesh_configure_nics tool");
		rv = -EINVAL;
		goto out;
	}

	if (!rv && nvmeibs_selected_layer == IB_LINK_LAYER_ETHERNET)
		nvmeibs_start_roce();

	nvmeibs_get_devices(NULL);
	list_for_each_entry_safe(nis_dev, nis_tmp, &nvmeibs_dev_list,
		nvmeibs_dev_list_n) {
		list_for_each_entry_safe(ib_port, ib_port_tmp, &nis_dev->port_list,
			port_list_n) {
			if (rv || ib_port->layer != nvmeibs_selected_layer) {
				if (ib_port->layer == IB_LINK_LAYER_INFINIBAND)
					--nis_dev->ib_ports;
				else if (ib_port->transport == RDMA_TRANSPORT_IWARP)
					--nis_dev->iwarp_ports;
				else
					--nis_dev->roce_ports;
				_NT(trace_2_main_ensure_devs_uniformity, "Will not use port @GID_STR due to link-layer", ib_port->gid.gid_str);
				ib_port->port_used = false;
				list_del(&ib_port->port_list_n);
				list_add_tail(&ib_port->port_list_n, &nis_dev->unused_port_list);
				nis_dev->unused_ports++;
			}
		}
		if (list_empty(&nis_dev->port_list)) {
			_NT(trace_3_main_ensure_devs_uniformity, "Nic has no enabled ports. Setting to unused");
			list_del(&nis_dev->nvmeibs_dev_list_n);
			nvmeibs_dev_count--;
			nis_dev->device_used = false;
			list_add_tail(&nis_dev->nvmeibs_dev_list_n, &nvmeibs_unused_dev_list);
			nvmeibs_unused_dev_count++;
		} else {
			/* Release device lock (Can't be held during do_add_one) */
			nvmeibs_put_devices();
			rv = do_add_one(nis_dev);
			nvmeibs_get_devices(NULL);
			if (rv < 0) {
				_NT(trace_4_main_ensure_devs_uniformity, "Nic failed to initialize (@RV),"
				"Remove from used and Add to unused list", rv);
				/* Re-acquire lock and remove from list */
				list_del(&nis_dev->nvmeibs_dev_list_n);
				--nvmeibs_dev_count;
				nis_dev->device_used = false;
				list_add_tail(&nis_dev->nvmeibs_dev_list_n, &nvmeibs_unused_dev_list);
				nvmeibs_unused_dev_count++;
				rv = 0;
			}
		}
	}
	nvmeibs_put_devices();
out:
	NFOUT;
	return rv;
}

static void run_on_main_wq_workfn(struct workqe_struct *work_qe)
{
	struct run_mainwq_workqe *run_workqe =
		container_of(work_qe, struct run_mainwq_workqe, work);

	run_workqe->rv = (*run_workqe->fn)(run_workqe->param);

	if (run_workqe->free_work)
		kfree(run_workqe);
}

static int run_on_main_wq(main_wq_fn_type fn, void *param, bool drain_wq,
	bool can_sleep, int *p_sts)
{
	struct run_mainwq_workqe stack_work = {
		.fn = fn,
		.param = param,
		.rv = 0,
		.free_work = false
	}, *work = NULL;
	int sts = 0;
	int rv = 0;

	NFIN;
	if (nvmeibs_on_main_wq()) {
		/* Already on Main WQ, so just run the fn */
		rv = (*fn)(param);
		goto out;
	}

	if (drain_wq) {
		/* Waiting for it to finish so we can use the stack work and comp */
		work = &stack_work;
	} else {
		if (!(work = kzalloc(sizeof(*work), (!can_sleep ? GFP_ATOMIC : GFP_KERNEL)))) {
			rv = -ENOMEM;
			sts = -1;
			goto out;
		}
		work->fn = fn;
		work->param = param;
		work->free_work = true;
	}
	WQ_INIT_WORK(&work->work, run_on_main_wq_workfn);

	if ((rv = nvmeibs_add_work(&work->work))) {
		if (work->free_work) {
			kfree(work);
		}
		sts = -1;
		goto out;
	}

	if (drain_wq) {
		wq_drain(main_wq);
		rv = stack_work.rv;
	}

out:
	if (p_sts)
		*p_sts = sts;

	NFOUT;
	return rv;
}

static int srv_start_ib_work_fn(void *param)
{
	int rv = 0;
	int max_cl_msg_size = NVMEIBC_MAX_CLIENT_MSG_SIZE;

	NFIN;
	if (nvmeibs_max_req_size < max_cl_msg_size) {
		_NI(srv_start_ib_work_fn_i1, "Configured 'max_req_size' module-param too small @INT, "
		   "override with @INT", nvmeibs_max_req_size, max_cl_msg_size);
		nvmeibs_max_req_size = max_cl_msg_size;
	}

	/* let ib layer call add_one per nic */
	if ((rv = ib_register_client(&nvmeibs_client)) < 0) {
		_NE(error_main_srv_start_ib_work_fn, "Couldn't register IB client");
		goto out;
	}

	/* If nvmeibs_disk_scan_finished() returned false when add_disk was called,
	 * then nvmeibs_register_disk_resources_at_all_nics was not called. Instead
	 * register_disk_resources is called for each NIC by add_one.
	 * Once ib_register_client has finished, all disks have been registered for all online NICs that support RDDA.
	 *
	 * Note that NICs that are added after ib_register_client finishes may add RDDA resources to the disk. If the disk is
	 * already "discovered" on a client that it will not be made aware of these new RDDA resources. TBD */
	nvmeibs_all_disks_register_done();

	if ((rv = ensure_devs_uniformity())) {
		_NE(error_1_main_srv_start_ib_work_fn, "Could not ensure devices uniformity, check configuration");
		ib_unregister_client(&nvmeibs_client);
		goto out;
	}
	NFOUT;

	nvmeibs_client_registered = true;

out:
	return rv;
}

static void nvmeibs_mcs_recv_callback(void *buf, int len)
{
	NFIN;
	_ND(trace_main_nvmeibs_mcs_recv_callback, "No mcs to srv msgs defined yet");
	NFOUT;
}

static void __print_hooray(bool is_start, const char* mod_name)
{	/* Hooray */
	const char *ur = ((is_start) ? "registered" : "unregistered");		// Consider up/down
	struct timeval time;
	unsigned long local_time;
	struct rtc_time tm;
	do_gettimeofday(&time);
	local_time = (u32)(time.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time_to_tm(local_time, &tm);

	_NI_to_user(nvmeibs_hooray,
				"module", "Module @STR. Module: @STR. "
						  "Timestamp: @TM_YEAR-@TM_MON-@TM_MDAY @TM_HOUR:@TM_MIN:@TM_SEC. "
						  "Internal version for support cases: @COMMIT_ID_LONG",
				ur, mod_name,
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
				(unsigned long)COMMIT_ID); // Hooray //. Error code: 0
}

static bool __pci_acs_path_enable(struct pci_dev *start,
								  struct pci_dev *end, u16 acs_flags)
{
	struct pci_dev *pdev, *parent = start;

	/* compat to linux-3.10.0-1127.13.1.el7.x86_64 */
	bool (*pci_acs_enabled)(struct pci_dev *pdev, u16 acs_flags) =
		(void *)nvmeib_kallsyms_lookup_name("pci_acs_enabled");
	bool (*pci_enable_acs)(struct pci_dev *pdev) =
		(void *)nvmeib_kallsyms_lookup_name("pci_enable_acs");
	void (*pci_request_acs)(void) =
		(void *)nvmeib_kallsyms_lookup_name("pci_request_acs");

	bool is_acs_enabled;

	if (!pci_acs_enabled) {
		_NI_dmesg(__AUTOID__, "ACS: pci_acs_enabled not found\n");
		return false;
	}
	if (!pci_enable_acs) {
		_NI_dmesg(__AUTOID__, "ACS: pci_enable_acs not found\n");
		return false;
	}
	if (!pci_request_acs) {
		_NI_dmesg(__AUTOID__, "ACS: pci_request_acs not found\n");
		return false;
	}


	_NI_dmesg(__AUTOID__, "ACS: pci_acs_enabled=@PTR, pci_enable_acs=@PTR, pci_request_acs=@PTR\n",
			  pci_acs_enabled, pci_enable_acs, pci_request_acs);

	/* ask for ACS to be enabled if supported */
	pci_request_acs();

	do {
		pdev = parent;

		is_acs_enabled = pci_acs_enabled(pdev, acs_flags);

		_NI_dmesg(__AUTOID__,
				  "ACS: pdev=@PCI_DEV, pcie-addr=@DEV_NAME, is_acs_enabled=@BOOL\n",
				  pdev, dev_name(&pdev->dev), is_acs_enabled);

		if (!is_acs_enabled) {
			pci_enable_acs(pdev);
			//return false;
		}

		if (!pdev->bus->parent) { //(pci_is_root_bus(pdev->bus))
			_NI_dmesg(__AUTOID__, "ACS: root bus\n");
			return (end == NULL);
		}

		parent = pdev->bus->self;
	} while (pdev != end);

	return true;
}

/* From drivers/iommu/intel-iommu.c, also same as pci_std_enable_acs() uses...
   The improtant flag is PCI_ACS_UF 'Upstream Forwarding' */
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)

static void __pci_acs_path_enable_all_nics(void)
{
	int num_devs = 0;
	struct list_head *devs;
	struct nvmeibs_dev *dev;
	struct pci_dev *pdev;

	_NE_dmesg(__AUTOID__, "ACS -->\n");
	devs = nvmeibs_get_devices(&num_devs);

	list_for_each_entry(dev, devs, nvmeibs_dev_list_n) {
		pdev = NVMEIBDEV2PCIDEV(dev->dev);
		_NI_dmesg(__AUTOID__, "ACS: dev=@DEVICE_NAME (@PTR), pdev=@PCI_DEV, pcie-addr=@DEV_NAME\n",
				  dev->dev->ib_dev->name, dev, pdev, dev_name(&pdev->dev));
		__pci_acs_path_enable(pdev, NULL, REQ_ACS_FLAGS);
	}

	nvmeibs_put_devices();
	_NE_dmesg(__AUTOID__, "ACS <--\n");
}

void nvmeibs_nvme_disk_scan_done(void)
{
	char *buffer = NULL;

	NFIN;
	_NI(nvmeibs_hooray_last_stage_of_init, "Hooray: last stage of server module initialization...");
	mutex_lock(&guard);
	disk_scan_done_called = true;
	create_proc_files();
	if (nvmeibs_mcs_create(nvmeibs_proc_dir,
		nvmeibs_mcs_recv_callback) < 0) {
		_NE(error_main_nvmeibs_nvme_disk_scan_done, "Fail to create srv's mcs");
		goto out;
	}
	if (nvmeibs_toma_create(nvmeibs_proc_dir, NULL) < 0) {
		_NE(error_1_main_nvmeibs_nvme_disk_scan_done, "Fail to create srv's toma");
		goto out;
	}
	if (!(buffer = kzalloc(PAGE_SIZE, GFP_KERNEL))) {
		_NE(error_2_main_nvmeibs_nvme_disk_scan_done, "OOM: cannot allocate buffer to print controller info");
		goto out;
	}

	if (!check_module_req_size()) {
		_NE(error_3_main_nvmeibs_nvme_disk_scan_done, "invalid value @NVMEIBS_MAX_REQ_SIZE for kernel module parameter"
			" nvmeibs_max_req_size -- must be at least @NVMEIBC_DEFAULT_CLIENT_MSG_SIZE.",
			nvmeibs_max_req_size, NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE);
		goto out;
	}

	/* Defer ib_register_client() on main-wq */
	if (run_on_main_wq(srv_start_ib_work_fn, NULL, true, true, NULL)) {
		_NE(error_4_main_nvmeibs_nvme_disk_scan_done, "Failed to register to an RDMA device");
		goto out;
	}

	fill_disk_info(NULL, buffer, PAGE_SIZE);

	if (false) __pci_acs_path_enable_all_nics();

out:
	if (buffer) {
		_NT(trace_1_main_nvmeibs_nvme_disk_scan_done, "\n@BUFFER_STR", buffer);
		__print_hooray(true, PROCFS_COMMON_STR);
		kfree(buffer);
	} else
		_NE_to_user(nvmeibs_hooray_register_failed, "module", "Module register failed. Error code: 1057. Module: nvmeibs.");
	nvmeib_register_local_server(&local_server);
	mutex_unlock(&guard);
	NFOUT;
}

static int nvmeibs_keep_alive_fc(struct nvmeibs_client *cl, void *arg)
{
	nvmeibs_client_keep_alive(cl);

	return 0;
}

void nvmeibs_keep_alive(void)
{
	nvmeibs_cdb_all_fast_call(nvmeibs_keep_alive_fc, NULL);
}

#if 0 /* DEBUG ONLY */
void nvmeibs_trigger_ib(struct nvmeibs_q_info *q)
{
	struct hlist_node *hlink;

	spin_lock(&nvmeibs_client_lock);
	hlist_for_each(hlink, &nvmeibs_client_hash)
		nvmeibs_client_check_trigger(h_to_client(hlink));
	spin_unlock(&nvmeibs_client_lock);
}
#endif

int nvmeibs_add_work(struct workqe_struct *work)
{
	return wq_add_work(main_wq, work) ? 0 : -1;
}

bool nvmeibs_disk_scan_finished(void)
{
	NFIN;

	NFOUT;
	return disk_scan_done_called;
}

struct release_disk_clients_workq {
	struct workqe_struct work;
	struct nvmeibs_disk_info *di;
	struct completion *done;
	int rv;
	enum nvmeibs_logout_reason reason;
};

struct ib_port_item {
	struct nvmeibs_ib_port *ib_port;
	struct list_head link;
};

struct release_disk_clients_ctx {
	struct nvmeibs_disk_info *di;
	int n_cl;
	enum nvmeibs_logout_reason reason;
};

static int release_disk_clients_fc(struct nvmeibs_client *cl, void *arg)
{
	struct release_disk_clients_ctx *ctx = arg;
	int rv;

	if (memcmp(ctx->di->disk_id, cl->disk_name, sizeof(ctx->di->disk_id)) ||
	    !cl->di) {
		_NT(trace_main_release_disk_clients_fc, "cl @CL: not linked to disk @DISK_ID_STR", cl, ctx->di->disk_id);
		return 0;
	}

	if (ctx->di != cl->di) {
		_NE(error_main_release_disk_clients_fc, "Oops, @DI vs @DI", ctx->di, cl->di);
		return -1;
	}

	/* add client-release work  */
	rv = nvmeibs_ib_port_free_client(cl->ib_port, cl->cid, ctx->reason);
	if (rv < 0) {
		_NE(error_1_main_release_disk_clients_fc, "Fail to add client-release work (cl @CL)", cl);
		return -1;
	}

	ctx->n_cl++;

	_NT(trace_1_main_release_disk_clients_fc, "Added client-release work, cl @CL (n_cl=@N_CL)", cl, ctx->n_cl);

	return 0;
}

int release_disk_clients(struct nvmeibs_disk_info *di, enum nvmeibs_logout_reason reason)
{
	DECLARE_COMPLETION_ONSTACK(zero_clients);
	struct release_disk_clients_ctx ctx = {
		.di = di,
		.n_cl = 0,
		.reason = reason
	};
	u64 ts, dt;
	int rv;

	NFIN;

	_NT(trace_main_release_disk_clients,
		"--> disk @DISK_ID_STR, release all clients", di->disk_id);
	mutex_lock(&guard);

	nvmeibs_disk_get_disks(NULL);
	if (!di->dying)
		di->dying = true;
	else
		_NT(trace_1_main_release_disk_clients, "Disk is already dying"); /* we wait via clients' lock */
	nvmeibs_disk_put_disks();

	do {
		rv = nvmeibs_cdb_all_fast_call(release_disk_clients_fc, &ctx);
	} while (rv);

	_NT(trace_2_main_release_disk_clients,
		"Found total of @N_CL clients (nref=@INT)",
		ctx.n_cl, nvmeib_ref_read(&di->nref));
	ts = jiffies;
	nvmeib_ref_release_wait(&di->nref);
	dt = jiffies - ts;
	_NT(trace_3_main_release_disk_clients,
		"release all clients "
		"took=@DISK_CLIENTS_RELEASE_DT(@DISK_CLIENTS_RELEASE_DT)", dt, dt/HZ);

	mutex_unlock(&guard);

	_NT(trace_4_main_release_disk_clients,
		"deregister disk resources(@DISK_ID_STR)", di->disk_id);
	nvmeibs_deregister_disk_resources(NULL, di);

	_NT(trace_5_main_release_disk_clients, "<-- disk @DISK_ID_STR, release all clients", di->disk_id);

	NFOUT;
	return rv;
}

static void release_disk_clients_work(struct workqe_struct *work)
{
	struct release_disk_clients_workq *w =
		container_of(work, struct release_disk_clients_workq, work);
	struct nvmeibs_disk_info *di = w->di;

	NFIN;

	w->rv = release_disk_clients(di, w->reason);
	complete(w->done);

	NFOUT;
}

int nvmeibs_release_disk_clients(struct nvmeibs_disk_info *di, enum nvmeibs_logout_reason reason)
{
	struct release_disk_clients_workq work;
	DECLARE_COMPLETION_ONSTACK(done);
	int mwq;
	int n_cls;
	int rv = 0;

	NFIN;

	_ND(trace_main_nvmeibs_release_disk_clients, "disk @DISK_ID_STR release all clients - start", di->disk_id);
	mutex_lock(&guard);
	mwq = !!main_wq;
	if (mwq) {
		WQ_INIT_WORK(&work.work, release_disk_clients_work);
		work.di = di;
		work.done = &done;
		work.reason = reason;
		if ((rv = nvmeibs_add_work(&work.work)) < 0)
			_NE(error_main_nvmeibs_release_disk_clients, "Fail to add work (rv @RV)", rv);
	}
	mutex_unlock(&guard);

	if (mwq) {
		if (!rv) {
			_NT(trace_1_main_nvmeibs_release_disk_clients, "wait for disk @DISK_ID_STR release-clients completion", di->disk_id);
			wait_for_completion(&done);
			rv = work.rv;
		}
	}
	else {
		if ((n_cls = nvmeibs_cdb_count(true))) {
			_NE(error_1_main_nvmeibs_release_disk_clients, "Unexpected: no main-wq but @N_CLS clients connected", n_cls);
			//rv = release_disk_clients(di);
			BUG();
		}
	}
	_ND(trace_2_main_nvmeibs_release_disk_clients, "disk @DISK_ID_STR release all clients - end", di->disk_id);

	NFOUT;
	return rv;
}

void nvmeibs_register_disk_resources_at_all_nics(struct nvmeibs_disk_info *disk)
{
	struct nvmeibs_dev *nis_dev;
	struct list_head *devices;
	int rv;

	NFIN;
	devices = nvmeibs_get_devices(NULL);
	list_for_each_entry(nis_dev, devices, nvmeibs_dev_list_n) {
		if (!disk->n_qs) {
			_NT(trace_4_main_nvmeibs_register_disk_resources_at_all_nics,
			    "disk @DISK_NAME has no queues to register", disk->disk_id);
		} else {
			if ((rv = nvmeibs_register_disk_resources(nis_dev, disk) < 0)) {
				_ND(trace_main_nvmeibs_register_disk_resources_at_all_nics, "Registering disk @DISK_ID_STR failed (@RV)",
				    disk->disk_id, rv);
				continue;
			}
		}
		_NT(trace_2_main_nvmeibs_register_disk_resources_at_all_nics, "Registered disk @DISK_ID_STR", disk->disk_id);
		nvmeibs_disk_lock_disk_map_segs_for_dev_(disk, nis_dev);
		if (disk->metadata)
			nvmeibs_serjio_map_disk_jmd_cache(disk, nis_dev);
	}

	nvmeibs_put_devices();

	/* Set register resources is done - Could be set here, after ib_register_client has finished (ie all add_one) or both */
	atomic_inc(&disk->register_done);
	NFOUT;
}

void nvmeibs_all_disks_register_done(void)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *di;

	disks = nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, disks, link)
	atomic_inc(&di->register_done);
	nvmeibs_disk_put_disks();
}

static void close_procs(void)
{
	NFIN;
#ifdef REMOVE_CLIENT_PROC
	nvmeib_public_proc_remove(remove_procfs_ent);
#endif

	if (rsrc_info_proc != NULL) {
		nvmeib_public_proc_remove(rsrc_info_proc);
		rsrc_info_proc = NULL;
	}
	if (disk_info_proc != NULL) {
		nvmeib_public_proc_remove(disk_info_proc);
		disk_info_proc = NULL;
	}
	if (disks_proc != NULL) {
		nvmeib_public_proc_remove(disks_proc);
		disks_proc = NULL;
	}
	if (serjios_proc != NULL) {
		nvmeib_public_proc_remove(serjios_proc);
		serjios_proc = NULL;
	}
	if (partitions_proc != NULL) {
		nvmeib_public_proc_remove(partitions_proc);
		partitions_proc = NULL;
	}
	if (disks_nics_prefer_proc != NULL) {
		nvmeib_public_proc_remove(disks_nics_prefer_proc);
		disks_nics_prefer_proc = NULL;
	}
	if (nics_proc != NULL) {
		nvmeib_public_proc_remove(nics_proc);
		nics_proc = NULL;
	}
	if (dot_nics_proc != NULL) {
		nvmeib_public_proc_remove(dot_nics_proc);
		dot_nics_proc = NULL;
	}
	if (version_proc != NULL) {
		nvmeib_public_proc_remove(version_proc);
		version_proc = NULL;
	}

#ifdef NVMEIBS_CLIENTS_PROC
	if (clients_proc != NULL) {
		nvmeib_public_proc_remove(clients_proc);
		clients_proc = NULL;
	}
#endif

	if (dot_debug_proc != NULL) {
		nvmeib_public_proc_remove(dot_debug_proc);
		dot_debug_proc = NULL;
	}

	if (shared_cq_proc != NULL) {
		nvmeib_public_proc_remove(shared_cq_proc);
		shared_cq_proc = NULL;
	}

	if (memmgr_info_proc != NULL) {
		nvmeib_public_proc_remove(memmgr_info_proc);
		memmgr_info_proc = NULL;
	}

	nvmeibs_client_proc_umkdir(nvmeibs_proc_dir);

	NFOUT;
}

int nvmeibs_init(void) /* Constructor */
{
	int rv = 0;

	NFIN;
	BUILD_BUG_ON(
		sizeof(struct nvmeibs_login_response) > NVMEIB_MAX_CM_REP_PAYLOAD_SIZE);
	_NI(trace_main_nvmeibs_init, "Version: @COMMIT_ID_LONG, ports=@PORTS, guids=@GUIDS",
		(unsigned long)COMMIT_ID,
		nvmeibs_filter_ports,
		nvmeibs_filter_guids);
	
	rv = nvmesh_memmgr_metrics_alloc_pcpu(__start_nvmeibs_memmgr_metrics, __stop_nvmeibs_memmgr_metrics);
	if (rv < 0) {
		_NE(error_main_nvmeibs_init_pcpu_alloc, "Failed to initialize memmgr metrics");
		goto unlock;
	}
	set_lock_dev_mode();
	mutex_lock(&guard);
	nvmeib_set_debug_level(nvmeib_debug_level);
	nvmeib_public_set_debug_level(nvmeib_debug_level);

	if (nvmeibs_tcp_mode && nvmeibs_use_pcpu_cq) {
		_NE_dmesg(nvmeibs_init_tcp_pcpu_err, "Invalid configuration: pcpu-cqs is not supported over TCP");
		rv = -EINVAL;
		goto unlock;
	}
	INIT_LIST_HEAD(&used_dev_list);
	nvmeib_set_used_dev_list(nvmeibs_filter_ports, MAX_FP, &used_dev_list);
	nvmeib_set_used_pots_guids(nvmeibs_filter_guids, MAX_FP, &used_dev_list);
	scnprintf(nvmeibs_node_name, NVMEIB_HOST_NAME_LEN, "%s",
		nvmeib_get_utsname_nodename());
	atomic64_set(&client_uid, NVMEIBS_CLIENT_UID_BASE);
	nvmeibs_client_registered = false;

	if (!(s_intr_shaper = nvmeib_get_intr_shaper())) {
		_NE(error_main_nvmeibs_init, "Failed to get interrupts shaper");
		rv = -1;
		goto unlock;
	}
	if (!(main_wq = wq_create_verbose(proc_name_format("S", "WQ", "main")))) {
		_NE(error_1_main_nvmeibs_init, "Failed to allocate main controller work queue");
		rv = -1;
		goto unlock;
	}
	if (!(um_comm = nvmeibs_um_comm_start())) {
		_NE(error_2_main_nvmeibs_init, "Failed to usermode communication channel");
		rv = -1;
		goto unlock;
	}
	main_wq_pid = wq_pid(main_wq);

	if ((rv = nvmeibs_nordda_kwq_init()) != 0) {
		_NE(error_3_main_nvmeibs_init, "Failed to initialize nordda kernel workqueue");
		goto unlock;
	}

	nvmeibs_serial_console_flag = nvmeib_public_serial_console();
	if (nvmeibs_serial_console_flag)
		_NI(trace_1_main_nvmeibs_init, "Kernel has a serial console, reducing output");
	goto unlock;

unlock:
	mutex_unlock(&guard);
	NFOUT;
	return rv;
}

bool nvmeibs_is_exit_called(void)
{
	NFIN;
	NFOUT;
	return nvmeibs_exit_called;
}

static void stop_all_ports(void)
{
	struct nvmeibs_dev *nis_dev, **devs_array;
	struct nvmeibs_ib_port *port;
	struct list_head *devs;
	int devs_n, i;

	NFIN;
	nvmeibs_exit_called = true;
	devs = nvmeibs_get_devices(&devs_n);
	_NT(trace_main_stop_all_ports, "Number of devices to be iterated @DEVS_N", devs_n);
	if (devs_n == 0) {
		_NT(trace_1_main_stop_all_ports, "Zero devices, no port is to be drained");
		nvmeibs_put_devices();
		goto out;
	}
	devs_array = kcalloc(devs_n, sizeof(*devs_array), GFP_KERNEL);
	if (!devs_array) {
		_NE(error_main_stop_all_ports, "Failed to allocate memory");

		goto no_devs_array;
	}

	i = 0;
	list_for_each_entry(nis_dev, devs, nvmeibs_dev_list_n)
		devs_array[i++] = nis_dev;

	for (i = 0; i < devs_n; ++i) {
		nis_dev = devs_array[i];
		if (!nis_dev)
			continue;
		list_for_each_entry(port, &nis_dev->port_list, port_list_n) {
			if (port->wq) {
				_NT(trace_2_main_stop_all_ports, "Draining a port");
				wq_drain(port->wq);
			}
		}
	}
	kfree(devs_array);

no_devs_array:
	nvmeibs_put_devices();
	_NT(trace_3_main_stop_all_ports, "All ibports workqueue are drained");
	_NT(trace_4_main_stop_all_ports, "If we have work on the main queue let it finish");
	if (main_wq)
		wq_drain(main_wq);

out:
	NFOUT;
}



static void wait_for_no_toma(void)
{
	struct task_struct *p;
	int found_toma = 1;

	_NT(trace_main_wait_for_no_toma, "Verify no toma processes exist");
	do {
		found_toma = 0;
		for_each_process(p) {
			if (!strcmp(p->comm, TOMA_THREAD_NAME)) {
				_NT(trace_1_main_wait_for_no_toma, "found a toma process @PID", p->pid);
				msleep(1000);
				found_toma = 1;
			}
		}
	} while (found_toma);

	_NT(trace_2_main_wait_for_no_toma, "No toma process found");
}



enum IB_EVENT {
	IB_EVENT_OK = 0,
	IB_EVENT_FATAL,
	IB_EVENT_EXIT
};

static atomic_t exit_fatal;


int nvmeibs_main_start_fatal(void)
{
	return (atomic_cmpxchg(&exit_fatal, IB_EVENT_OK, IB_EVENT_FATAL) == IB_EVENT_OK);
}

void nvmeibs_main_set_ok(void)
{
	atomic_set(&exit_fatal, IB_EVENT_OK);
}

static void nvmeibs_main_start_exit(void)
{
	while (atomic_cmpxchg(&exit_fatal, IB_EVENT_OK, IB_EVENT_EXIT) != IB_EVENT_OK) {
		_NT(trace_main_nvmeibs_main_start_exit, "calling msleep()");
		msleep(10);
	}
}


void nvmeibs_exit(void) /* Destructor */
{
	NFIN;

	nvmeibs_main_start_exit();

	mutex_lock(&guard);

	_NI(trace_main_nvmeibs_exit, "Unloading NVMesh server...");
	_NT(trace_1_main_nvmeibs_exit, "Disable new client connections");
	stop_all_ports();
	_NT(trace_2_main_nvmeibs_exit, "Close procs...");
	close_procs();
	nvmeibs_mcs_destroy();
	_NT(trace_3_main_nvmeibs_exit, "Close TOMA...");
	nvmeibs_toma_shut_down();
	wait_for_no_toma();
	_NT(trace_4_main_nvmeibs_exit, "Removing any remaining clients...");
	nvmeibs_remove_all_clients(false, false, NVMEIBS_LOGOUT_REASON_SERVER_EXIT);
	_NT(trace_5_main_nvmeibs_exit, "Close local client...");
	notify_client_server_close();
	_NT(trace_6_main_nvmeibs_exit, "Close RoCE (if up) listener...");
	/* stop listener so no new connection requests */
	nvmeib_rdma_stop_listen(roce_cm);
	_NT(trace_7_main_nvmeibs_exit, "Close IB (if up) listener...");
	if (nvmeibs_client_registered)
		remove_ib();
	_NT(trace_8_main_nvmeibs_exit, "Remove TOMA server interface...");
	nvmeibs_toma_remove();
	_NT(trace_9_main_nvmeibs_exit, "nvmeibs_toma_remove finished");
	/* kill the listener descriptor */

	nvmeib_rdma_destroy_cm(roce_cm);
	_NT(trace_10_main_nvmeibs_exit, "Roce listener is now destroyed");

#if 0
	/* [NVMESH-3367]: This is now redundant as we clear nvmeib_local_server_p
	 * with the call to notify_client_server_close
	 */
	nvmeib_register_local_server(NULL);
	_NT(trace_11_main_nvmeibs_exit, "Local server is now unregistered");
#endif

	nvmeib_free_used_dev_list(&used_dev_list);

	gids_proc_remove();

	if (nic_stats_proc_dir) {
		proc_remove(nic_stats_proc_dir);
		nic_stats_proc_dir = NULL;
	}

	nvmeibs_disk_all_free_lock_resources();

	/* stop nordda kernel workqueue */
	_NT(trace_12_main_nvmeibs_exit, "Stop nordda kernel wq...");
	nvmeibs_nordda_kwq_exit();
	/* wait for main-q works e.g. proc-locks remove */
	_NT(trace_13_main_nvmeibs_exit, "Stop main wq...");
	if (main_wq) {
		wq_drain(main_wq);
		wq_destroy(main_wq);
		main_wq = NULL;
	}
	/* stop usermode communication */
	nvmeibs_um_comm_stop(um_comm);	
	s_intr_shaper = NULL;
	nvmesh_memmgr_metrics_free_pcpu(__start_nvmeibs_memmgr_metrics, __stop_nvmeibs_memmgr_metrics);
	mutex_unlock(&guard);
	nvmeib_public_set_debug_level(NULL);
	__print_hooray(false, PROCFS_COMMON_STR);
	NFOUT;
}
