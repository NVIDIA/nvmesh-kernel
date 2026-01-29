/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_IB_PORT_MGMT_H
#define NVMEIBS_IB_PORT_MGMT_H

#include "kr_incs.h"
#include "nvmeib.h"
#include "nvmeib_rdma.h"
#include "ib_dm_mad.h"
#include "nvmeibs_nvme.h"

enum {
	NVMEIBS_SEND_TO_IOC = 0x01,
	NVMEIBS_SEND_FROM_IOC = 0x02,
	NVMEIBS_RDMA_READ_FROM_IOC = 0x08,
	NVMEIBS_RDMA_WRITE_FROM_IOC = 0x20,
};

/**
 * struct nvmeibs_port_attrib - Attributes for HOST IB port
 * @max_rdma_size: Maximum size of RDMA transfers for new
 *  			 connections.
 * @max_req_size: Maximum size of of allocated space for client
 *  			   messages in bytes.
 * @sq_size: Shared receive queue (SRQ) size.
 * @max_requests: Max requests per disk.
 */
struct nvmeibs_port_attrib {
	unsigned max_rdma_size;
	unsigned max_req_size;
	unsigned sq_size;
	unsigned max_requests;
};

#define NVMEIB_DEF_MAX_N_CLIENTS_BITS 7

#define NUM_VLAN_GIDS 2

struct nvmeibs_dev;
struct nvmeibs_ib_port {
	/* owner device */
	struct nvmeibs_dev *nis_dev;
	/* port stuff */
	bool port_used; /* has port passed ports/guids filters last time nvmeib_use_dev() was called (which eventully calls nvmeib_rdma_select_port_gid()) */
	bool port_active; /* was port's state == IB_PORT_ACTIVE last time ib_query_port() was called */
	atomic_t outstanding;
	u8 port;
	struct nvmeib_rdma_ib_port_gid gid;
	struct list_head gid_list;
	unsigned n_gids;
	union ib_gid hw_gid;
	bool gid_change;
	enum rdma_link_layer layer;
	enum rdma_transport_type transport;
	bool is_multi_transport; /* is port's ndev roce && siw - for nics.csv */
	unsigned int transport_priority; /* Port priority */
	u16 sm_lid;
	u16	lid;
	u16 pkey;
	struct ib_mad_agent	*mad_agent;
	struct nvmeibs_port_attrib port_attrib;
	struct list_head port_list_n;
	/* nics.csv to display unique ndev */
	struct list_head uniq_link;
	/* the main workqueue - everything starts here */
	struct workq_struct *wq;
	/* any (admin and non) connect request arriving on this port */
	struct nvmeib_ref n_port_conns;
	/*hardware type, cx4, cx5 etc*/
	u16 hw_type;
	/* Loopback CM listener */
	struct nvmeib_rdma_cm *loop_listener;
	/* GIDs CSV Proc File */
	struct nvmeib_public_procfs_ent *gids_csv_proc_ent;
	/* kmem caches for nrch cmd req */
	struct kmem_cache *nrch_cmd_req_cache;
};

/* Returns true if port is used (ie included by filters), active and has a valid gid.
 on transition from 1 to 0, release-all-clients, on any transition notify Toma and update* */
static inline bool nvmeibs_ib_port_enabled(struct nvmeibs_ib_port *port) {
	return port->port_active && port->port_used && port->gid.valid;
}

struct port_work {
	struct workqe_struct work;
	struct nvmeibs_ib_port *port;
};

struct add_client_workq {
	struct port_work port;
	struct nvmeib_rdma_cm *cm_id;
	struct nvmeibc_login_request req;
	unsigned long sent_time;
	/* true if port_workq was switched */
	bool port_queue_switched;
};

struct cid_port_work {
	struct port_work port;
	u64 cid;
	enum nvmeibs_logout_reason reason;
};

struct gid_update_port_work {
	struct cid_port_work cid;
	struct nvmeibs_client *cl;
	int (*f)(struct nvmeibs_client *cl, struct nvmeibs_ib_port *ib_port);
};

struct nvmeibs_client;
struct nvmeib_rdma_cm;
struct nvmeib_rdma_conn_params;

struct nvmeibs_ib_port *nvmeibs_ib_port_find_ib_port(
	struct nvmeibs_dev *nis_dev, u8 port);

/* new port on device */
struct nvmeibs_ib_port *nvmeibs_ib_port_add(struct nvmeibs_dev *nis_dev, u8 port);

/* stop port connectivity */
void nvmeibs_ib_port_clear(struct nvmeibs_ib_port *ib_port);

/* free port resources */
void nvmeibs_ib_port_free(struct nvmeibs_ib_port *ib_port);

/* event handler for async port event */
void nvmeibs_ib_port_event_handler(struct nvmeib_rdma_event_handler *event_handler, 
			   struct ib_event *event);

/* new cleitn connection on port */
int nvmeibs_ib_port_new_connection(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm, struct nvmeib_rdma_conn_params *param);

int nvmeibs_ib_port_free_client(struct nvmeibs_ib_port *ib_port, u64 cid,
	enum nvmeibs_logout_reason reason);

int nvmeibs_ib_port_add_work(struct nvmeibs_ib_port *ib_port,
	struct workqe_struct *work);

void nvmeibs_ib_port_drain_q(struct nvmeibs_ib_port *ib_port);

/* wait for all connection to be removed */
void nvmeibs_ib_port_wait_no_conns(struct nvmeibs_ib_port *ib_port);

#endif
