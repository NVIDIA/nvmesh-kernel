/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_MAIN_H
#define NVMEIBS_MAIN_H

#include "kr_incs.h"
#include "nvmeib.h"
#include "nvmeib_utils.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeibs_main_gen_cmds.h"
#include "nvmeib_shared.h"
#include "nvmeib_public.h"

extern bool nvmeibs_defer_recv_comps;
extern unsigned nvmeibs_max_nic_srqs;
extern bool nvmeibs_use_pcpu_cq;

/*
 * Device private stuff.
 */

#define NVMEIB_SERVICE_NAME_PREFIX "NVMEIB:"

enum {
	/* max disks per machine */
	NVMEIBS_DEF_MAX_N_DISKS     	= 16,
	/* max clients per machines */
	NVMEIBS_DEF_MAX_N_CLIENTS   	= 128,
};

/* Timeout(s) for waiting to remove all clients (const + mult * num_clients) */
#define WAIT_REM_ALL_CL_CNST		(30 * HZ)
#define WAIT_REM_ALL_CL_MULT		(5 * HZ)

/* Interruptible timeout (used when TOMA is sent SIGTERM) */
#define WAIT_INT_REM_ALL_CL_CNST	(15 * HZ)
#define WAIT_INT_REM_ALL_CL_MULT	(2 * HZ)

struct nvmeib_rdma_cm;
struct nvmeibs_dev {
	struct nvmeib_dev *dev;
	struct list_head port_list;
	struct list_head unused_port_list;
	wait_queue_head_t port_release_q;
	struct nvmeib_rdma_cm *ib_l_cm_id;
	struct nvmeib_rdma_cm *iw_prim_l_cm_id;
	struct nvmeib_rdma_cm *iw_2nd_l_cm_id[NVMEIB_DFLT_MAX_CPUS];
	struct nvmeib_rdma_event_handler *event_handler;
	struct list_head nvmeibs_dev_list_n;
	/* number of ib ports on the device */
	int ib_ports;
	/* number of roce ports on the device */
	int roce_ports;
	/* number of iwarp ports on the device */
	int iwarp_ports;
	/* number of unused ports on the device */
	int unused_ports;
	/* Whether the device is used (ie not filtered out) */
	bool device_used;
	/* number of refresh-port in progress */
	struct nvmeib_ref n_refresh_port;
	/* if true NIC supprots atomic ops */
	bool atomic_ops;
	/* iostats for NIC */
	struct nvmeib_io_stats *stats;
	struct nvmeib_public_procfs_ent *proc_stats;
	struct proc_dir_entry *proc_stats_dir;
	unsigned long add_jif;
};

extern bool nvmeibs_use_tcp_locks;
static inline bool nvmeibs_dev_do_atomics(const struct nvmeibs_dev *sdev)
{
	return /* !nvmeibs_use_tcp_locks && */ sdev->atomic_ops;
}

struct nvme_dma_info {
	dma_addr_t sq;
	dma_addr_t sq_db;
	dma_addr_t prpl;
	dma_addr_t cq_db;
	dma_addr_t cq;
	dma_addr_t md;
};

struct nvmeib_alloc_n_map;
struct disk_mapped_mem_info {
	struct nvmeibs_dev *dev;
	union ib_gid ports[MAX_HCA_PORTS];
	__be64 node_guid;
	int n_ports;
	struct page_list_info *plia;
	struct nvmeib_alloc_n_map *bb_maps;
	struct list_head link;

	dma_addr_t nvme_msix_addr_iommu;
	struct nvme_dma_info *ib_dma;
};

struct suuid_workq {
	struct workqe_struct work;
	u32 cid;
	enum nvmeibs_logout_reason reason;
};

struct remove_clients_workq {
	struct workqe_struct work;
	bool set_toma_connecting;
	bool toma_connecting;
};

struct snode_workq {
	struct workqe_struct work;
	char node_name[NVMEIB_HOST_NAME_LEN + 1];
};

extern struct proc_dir_entry *nvmeibs_proc_dir;
extern struct proc_dir_entry *nvmeibs_proc_disks_dir;
extern char nvmeibs_node_name[NVMEIB_HOST_NAME_LEN];
extern struct nvmeib_intr_shaper *s_intr_shaper;
int nvmeib_debug_level(void);
bool nvmeib_serial_console(void);
const char *nvmeibs_device_name(struct nvmeibs_dev *dev);
u64 nvmeibs_get_service_guid(void);
int nvmeibs_get_max_req_size(void);
int nvmeibs_get_max_pages_in_fmr(void);
int nvmeibs_get_shared_recv_queue_size(void);
struct ib_client *nvmeibs_get_ib_client(void);
void nvmeibs_add_cm_id(struct nvmeib_cm_id *cm_id);
void nvmeibs_remove_cm_id(struct nvmeib_cm_id *cm_id);
u64 nvmeibs_get_client_uid(void);
struct nvmeibs_client;
/*disconnects all connected clients*/
int nvmeibs_remove_all_clients(bool toma_connecting, bool interruptible, enum nvmeibs_logout_reason);
struct nvmeibs_ib_port;
struct nvmeibs_client *nvmeibs_find_client_(
	u64 cid, struct nvmeibs_ib_port **ib_port);
struct nvmeibs_client *nvmeibs_find_client(
	u64 cid, struct nvmeibs_ib_port **ib_port);

struct nvmeibs_disk_info;
int nvmeibs_remove_cid_clients(u64 cid, enum nvmeibs_logout_reason reason);
void nvmeibs_release_port_clients(struct nvmeibs_ib_port *ib_port,
								  enum nvmeibs_logout_reason reason);

int nvmeibs_init(void); /* Constructor */
void nvmeibs_exit(void); /* Destructor */
void nvmeibs_nvme_disk_scan_done(void);
void nvmeibs_async_to_mgmt(struct nvmeibs_disk_info *info, int code);
struct list_head *nvmeibs_get_devices(int *size);
bool nvmeibs_is_devices_locked_by_me(void);
struct list_head *nvmeibs_get_devices_no_lock(int *size);
struct list_head *nvmeibs_get_unused_devices_no_lock(int *size);

void nvmeibs_put_devices(void);
struct list_head *nvmeibs_get_used_dev_list(void);
struct nvmeibs_dev *nvmeibs_get_nis_dev(struct ib_device *device);
int nvmeibs_activate_device(struct nvmeibs_dev *nis_dev, bool new_device);
extern enum rdma_link_layer nvmeibs_selected_layer;
//get the device that own the given raw gid
struct nvmeibs_dev *nvmeibs_gid_2_dev(const char *gid);
int nvmeibs_add_work(struct workqe_struct *work);
bool nvmeibs_disk_scan_finished(void);
void nvmeibs_register_disk_resources_at_all_nics(struct nvmeibs_disk_info *disk);
void nvmeibs_init_loopback_listener(struct nvmeibs_ib_port *ib_port);
void nvmeibs_deregister_disk_resources(struct nvmeibs_dev *nis_dev, struct nvmeibs_disk_info *disk);
void nvmeibs_all_disks_register_done(void);

void nvmeibs_keep_alive(void);
bool nvmeibs_is_exit_called(void);
int nvmeibs_release_disk_clients(struct nvmeibs_disk_info *di, enum nvmeibs_logout_reason reason);
int nvmeibs_start_roce(void);
bool nvmeibs_on_main_wq(void);
int nvmeibs_remove_ib_device(struct ib_device *ib_dev);
int nvmeibs_main_start_fatal(void);
void nvmeibs_main_set_ok(void);

void nvmeibs_update_all_clients_gid_change(struct nvmeibs_ib_port *ib_port);
struct nvmeibs_um_comm;
struct nvmeibs_um_comm * nvmeibs_get_um_comm(void);
u32 nvmeibs_get_nordda_io_req_num(void);
u32 nvmeibs_get_nordda_max_wrs_per_req(void);

bool nvmeibs_support_srq(struct nvmeib_dev *dev);

#endif /* NVMEIBS_MAIN_H */
