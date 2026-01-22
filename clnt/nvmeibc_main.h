/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MAIN_H
#define NVMEIBC_MAIN_H

#include "nvmeib.h"
#include "nvmeib_rdma.h"
#include "nvmeibc_mcs_stub.h"
#include "nvmeib_io_stats.h"

extern unsigned sm_th;				// Daniel: Todo, remove, use core params value instead
extern bool nvmeibc_use_pcpu_cq;	// Daniel: Todo, remove, use core params value instead
extern bool nvmeibc_prof_evt_registered;
extern bool nvmeibc_iommu_enabled;

/* External API of main layer towards other software layers. */

enum nvmeibc_mod_state nvmeibc_get_state(void);			// returns the state of the module

/*********************** Todo: Move me out of here ****************************/
struct nvmeibc_cinst_params_core;
struct nvmeib_dev;
struct nvmeibc_dev {
	struct nvmeib_dev *dev;
	const struct nvmeibc_cinst_params_core *cips;
	struct list_head port_list;
	struct list_head unused_port_list;
	struct list_head dev_list_n;
	struct nvmeib_rdma_event_handler *event_handler;
	struct proc_dir_entry *proc_dir;
	struct nvmeib_public_procfs_ent *procfs_status;
	struct nvmeib_io_stats *stats;
	unsigned long add_jif;
	bool device_used;
};

struct nvmeibc_ib_port {
	struct nvmeibc_dev *nic_dev;
	u8 port;
	bool port_used; /* has port passed ports/guids filters last time nvmeib_use_dev() was called (which eventully calls nvmeib_rdma_select_port_gid()) */
	bool port_active; /* was port's state == IB_PORT_ACTIVE last time ib_query_port() was called */
	struct nvmeib_rdma_ib_port_gid gid;
	enum rdma_link_layer layer;
	enum rdma_transport_type transport_type;
	u16 pkey;
	struct device dev;
	struct list_head port_list_n;
	struct list_head gid_list;
	unsigned n_gids;
};

/* Returns true if port is used (ie enabled by the filters), active and has a valid gid */
static inline bool nvmeibc_ib_port_enabled(struct nvmeibc_ib_port *port) {
	return port->port_active && port->port_used && port->gid.valid;
}

/*********************** Todo: Move me out of here end ************************/

int nvmeib_debug_level(void);
bool nvmeib_serial_console(void);
const char *nvmeibc_device_name(struct nvmeibc_dev *dev);

/*************************** /proc/ dir+file API ******************************/
struct nvmeibc_cinst_params_main;
struct proc_dir_entry *nvmeibc_get_proc_dir_volumes(const struct nvmeibc_cinst_params_main *p);

/***************************** volume API *************************************/
struct list_head *nvmeibc_get_volumes(     const struct nvmeibc_cinst_params_main *p);
struct list_head *nvmeibc_get_mt_volumes(  const struct nvmeibc_cinst_params_main *p);
int nvmeibc_get_thick_volumes_num(         const struct nvmeibc_cinst_params_main *p);
int nvmeibc_get_all_volumes_num(           const struct nvmeibc_cinst_params_main *p);
int nvmeibc_get_volumes_num(const struct nvmeibc_cinst_params_main *p, enum nvmeibc_config_volume_type type);
int nvmeibc_get_masked_volumes_num(const struct nvmeibc_cinst_params_main *p, enum nvmeibc_config_volume_type type);


/* Add volume to the appropriate list */
struct nvmeibc_volume;
int nvmeibc_add_volume(struct nvmeibc_volume *volume);
int nvmeibc_del_volume(struct nvmeibc_volume *volume);

/******************************************************************************/
/* Constructor/Destructor of disk uses methods below to insert/remove itself
 * to/from internal list of disks */
struct nvmeibc_disk;
struct list_head *nvmeibc_get_disks(const struct nvmeibc_cinst_params_main *p);
int  nvmeibc_add_disk( struct nvmeibc_disk *disk);
int  nvmeibc_del_disk( struct nvmeibc_disk *disk);
bool nvmeibc_find_disk(struct nvmeibc_disk *disk);

/* Can only be call from main WQ - Enforced by BUG_ON */
struct list_head *nvmeibc_get_all_devices(const struct nvmeibc_cinst_params_core *p);
struct list_head *nvmeibc_get_unused_devices(const struct nvmeibc_cinst_params_core *p);

void nvmeibc_add_nic_disk_local_nics(struct nvmeibc_disk *disk, struct nvmeibc_dev *nic_dev);
/* Populate disk's local nic list.
 * Not guarded, must be called in context of Main WQ */
int nvmeibc_populate_disk_local_nics(struct nvmeibc_disk *disk);

struct nvmeibc_admin_rnic;
bool nvmeibc_is_arnic_local(struct nvmeib_rdma_ib_port_gid *port_gids, struct nvmeibc_admin_rnic *arnic);
void nvmeibc_new_local_disk(void);
void nvmeibc_signal_disk_updated(struct nvmeibc_disk *disk);
extern bool profiling_enabled;

/****************** Todo: Move the below API from main to core ****************/
struct nvmeibc_cinst_params_core;
struct wd_obj;
struct wd_obj *nvmeibc_get_watchdog(          const struct nvmeibc_cinst_params_core *p);
struct wd_obj *nvmeibc_get_watchdog_pcpu(const struct nvmeibc_cinst_params_core *p, int cpu);
uuid_be *nvmeibc_get_uuid(                    const struct nvmeibc_cinst_params_core *p);
struct ib_sa_client;
struct ib_sa_client *nvmeibc_sa_client(       const struct nvmeibc_cinst_params_core *p);
struct nvmeib_intr_shaper *nvmeibc_get_shaper(const struct nvmeibc_cinst_params_core *p);

struct proc_dir_entry *nvmeibc_get_proc_dir_disks(const struct nvmeibc_cinst_params_core *p);
struct proc_dir_entry *nvmeibc_get_proc_dir_net(  const struct nvmeibc_cinst_params_core *p);
struct proc_dir_entry *nvmeibc_get_proc_dir_jam(  const struct nvmeibc_cinst_params_core *p);

struct nvmeib_local_server *nvmeibc_get_local_server(const struct nvmeibc_cinst_params_core *p);
/******************************************************************************/

/* Debug: dump all disks that are being used by the volume, and volume by disks
 *        used to debug configuration inconsistency
 */
void nvmeibc_dump_volumes_disk_ids(const struct nvmeibc_cinst_params_main *p);
void nvmeibc_dump_disks_ids(const struct nvmeibc_cinst_params_main *p);

const char *nvmeibc_get_utsname_nodename(const struct nvmeibc_cinst_params_main *p);

/*********** External API for requesting / sending configuration changes ******/
void nvmeibc_cc_api_notify_io_changed(/*const char*/ void *volume_uuid,
									 const struct nvmeibc_cinst_params_main *p);
void nvmeibc_cc_api_notify_detach_completion(/*const*/ struct nvmeibc_volume*,
									u32 /*enum_vol_status */ status);
void nvmeibc_cc_api_request_volume_config_in_atomic_context(const struct nvmeibc_cinst_params_main *p, const char *vol_name);
int  nvmeibc_cc_api_sub_vol_notification(/*const*/ struct nvmeibc_volume*,
									u32 /*enum_vol_status */ status);
int  nvmeibc_cc_api_sub_vol_unknown(const struct nvmeibc_cinst_params_main *p, const char *vol_name, const bool is_uuid);
void nvmeibc_cc_api_request_self_recov_detach(/*const char*/ void *volume_uuid,
									 const struct nvmeibc_cinst_params_main *p);

/* Send allert to mgmnt. Can update a previosly sent message using the same
   msg_id and is_update=true. If msg will not be updated, msg_id is irrelevant*/
int  nvmeibc_cc_api_send_mgmt_allert(const struct nvmeibc_cinst_params_main *p, char *msg, unsigned long msg_id, bool is_update);	// Allocated message which is freed
/*non blocking; sends target nics query to MCS via main work queue;*/

int nvmeibc_cc_api_query_target_nics(const struct nvmeibc_cinst_params_main *p, struct nvmeibc_target_nics_query query);

int nvmeibc_cc_api_query_target_nics_by_node_id(const struct nvmeibc_cinst_params_main *p, const char* target_node_id);

/* Ack a previously sent message using the above function. Marks the 'Acked'
   flag in the mgmt. */
int  nvmeibc_cc_api_send_io_disabled_ack(const char *c_name, const char *dev_name,
										 unsigned long msg_id);

void * nvmeibc_get_md_read_dummy_area( int *n_pages);
void * nvmeibc_get_md_write_dummy_area(int *n_pages);
void *nvmeibc_get_poison_area(int *n_pages);

bool nvmeibc_support_srq(struct nvmeib_dev *dev);
extern unsigned int nvmeibc_tcp_mode;

struct nvmeib_intr_shaper *nvmeib_get_intr_shaper(void);

#endif /* NVMEIBC_MAIN_H */
