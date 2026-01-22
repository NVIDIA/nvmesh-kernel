/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_RDMA_H
#define NVMEIB_RDMA_H

#include "nvmeib.h"
#include "nvmeib_types.h"

/**
 * enumeration of event handling
 */
enum nvmeib_rdma_event_type {
	NVMEIB_REQ_RECEIVED,
	NVMEIB_REJ_RECEIVED,
	NVMEIB_RTU_RECEIVED,
	NVMEIB_USER_ESTABLISHED,	/* client is ready to send messages */
	NVMEIB_CONNECT_ERROR,
	NVMEIB_DREQ_RECEIVED,	/* connection is close by client */
	NVMEIB_DREP_RECEIVED,	/* we closed the connection and the client ack */
	NVMEIB_TIMEWAIT_EXIT,
	NVMEIB_REP_ERROR,
	NVMEIB_DREQ_ERROR,
	NVMEIB_MRA_RECEIVED,
	NVMEIB_DEVICE_REMOVED,	/* device has been removed */
	NVMEIB_ADDR_RESOLVED,	/* the address has been resolved */
	NVMEIB_ROUTE_RESOLVED,	/* route to the other side has been resolved */
	NVMEIB_UNSUPPORTED,		/* unsupported event */
};

/* generic form of event */
struct nvmeib_rdma_event {
	enum nvmeib_rdma_event_type	event;
	int status;
	void *private_data;
	u8 private_data_len;
};

/**enumerate the connection type
 *  this type is the base for all other, according to type the
 *  underlying type cant be down casted
 */
enum nvmeib_rdma_type {
	_rdma_ib,    /* indicates infiniband port */
	_rdma_roce,  /* indicates roce based connection */
	_rdma_iwarp, /* indicates iwarp based connection */
	_rdma_lb,    /* indicates loopback connection */
	_rdma_lb_accept, /* indicates loopback connection from the accept side */
	
};

/* unique identification of a connection */
struct nvmeib_rdma_gid {
	enum nvmeib_rdma_type rdma_type;	/* the type of the connection */
	u8 gid[16];							/* unique identifier */
};

/* parameters given to server listener */
struct nvmeib_rdma_listen_ib_params {
	/* the service identification, in infiniband the ib id and
	   in roce the port and protocol
	*/
	u64 service_id;
};

/* roce specific parameters */
struct nvmeib_rdma_listen_roce_params {
	int port;	/* port -> in NVMesh case the tcp port */
	bool ipv4_only;
	bool iw_primary;		/* For iWARP, is this the primary listener */
	int iw_2nd_base_port;		/* Base port for iWARP secondary listeners */
	int iw_2nd_num_ports;		/* Number of secondary ports for iWARP secondary listeners */
};

/* Loobpack specific parameters */
struct nvmeib_rdma_listen_lb_params {
	union ib_gid gid;
	u8 gid_index;
	u8 roce_mac[ETH_ALEN];
	u16 pkey_index;
	enum rdma_link_layer link_layer;
};

/* connection paramters passed to server code when
   connection request is received
*/
#define LOGIN
struct nvmeib_rdma_conn_params {
	struct ib_device *d;	/* the device that received the connection */
	int port;					/* the transport layer port number */
	char private_data[NVMEIBC_LOGIN_PRIVATE_DATA_MAX_LEN];
};

struct nvmeib_rdma_peer_params {
};

struct nvmeib_dev;
/* listening paramters passed to the listen call */
struct nvmeib_rdma_listen_params {
	enum nvmeib_rdma_type type;	/* type of connection */
	/* holds the device to listen on */
	struct nvmeib_dev *dev;
	/* holds the port (used for LB) */
	int port;
	union {
		/* infiniband specific parameters */
		struct nvmeib_rdma_listen_ib_params ib;
		/* roce specific parameters */
		struct nvmeib_rdma_listen_roce_params roce;
		/* loopback specific parameters */
		struct nvmeib_rdma_listen_lb_params lb;
	};
	/* callbacks */
    /* called when new connection request is received in the server side */
	int (*new_connection)(struct nvmeib_rdma_cm *cm,
						  struct nvmeib_rdma_conn_params *p);
    /* other side event to handle */
	int (*on_peer_event)(struct nvmeib_rdma_cm *cm,
						 struct nvmeib_rdma_event *p);
	/* owner context */
	void *context;
};

/* fill HW & VLAN gids */
enum nvmeib_rdma_gid_type {
	NVMEIB_GID_TYPE_UNKNOWN = -EINVAL,
	NVMEIB_GID_TYPE_IB_ROCE_V1 = 0,
	NVMEIB_GID_TYPE_ROCE_V2 = 1,
	NVMEIB_GID_TYPE_SOFT_IWARP = 2,
	NVMEIB_GID_TYPE_SIZE,
};

enum nvmeib_rdma_network_type {
	NVMEIB_NETWORK_IB = 0,
	NVMEIB_NETWORK_IPV4,
	NVMEIB_NETWORK_IPV6,
	NVMEIB_NETWORK_TYPE_SIZE
};

struct nvmeib_rdma_ib_port_gid {
	struct list_head link;
	union ib_gid gid;
	u8 gid_index;
	enum rdma_link_layer link_layer;
	enum rdma_transport_type transport_type;
	enum nvmeib_rdma_gid_type gid_type;
	enum nvmeib_rdma_network_type net_type;
	bool is_vlan;
	u16 vlan_id;
	union ib_gid hw_gid;
	bool is_default_gid;
	bool is_ipv6_link_local;

	char gid_str[GUID_SIZE];
	char ndev_name[IFNAMSIZ];
	u8 	roce_mac[ETH_ALEN];
	bool valid; /* this GID has passed our match-filters on last call to nvmeib_rdma_select_port_gid() */
	bool preferred;
};

int nvmeib_rdma_port_supports_gid_type(struct ib_device *ib, int port, enum nvmeib_rdma_gid_type gid_type);

struct ib_port_attr;
int nvmeib_rdma_select_port_gid(struct ib_device *ib, int port, struct ib_port_attr *port_attr,
								int start_gid_index,
								struct nvmeib_rdma_ib_port_gid *rdma_port_gid,
								bool allow_default_gid, bool allow_ipv6_link_local,
								const char *match_ndev_name, 
								const union ib_gid *match_hw_gid,
								const union ib_gid *match_sw_gid_value,
								const union ib_gid *match_sw_gid_mask);
int nvmeib_rdma_get_port_hw_gid(struct ib_device *ib, int port, union ib_gid *hw_gid);

int nvmeib_rdma_cm_conn_get_gid_type(struct nvmeib_rdma_cm *cm, enum nvmeib_rdma_gid_type *gid_type);
int nvmeib_rdma_cm_conn_get_ib_ids(struct nvmeib_rdma_cm *cm, u32 *local_ib_id, u32 *remote_ib_id);

char *nvmeib_rdma_gid_type_str(enum nvmeib_rdma_gid_type gid_type, enum rdma_link_layer link_layer);
char *nvmeib_rdma_net_type_str(enum nvmeib_rdma_network_type net_type);

const char *nvmeib_rdma_gid_ip_str(char *buffer, union ib_gid *gid, enum nvmeib_rdma_network_type net_type);

/* print IB path */
struct ib_sa_path_rec;
void nvmeib_rdma_print_path(struct ib_sa_path_rec *path);

struct nvmeib_rdma_cm;
/* start a linstening server */
struct nvmeib_rdma_cm *nvmeib_rdma_listen(
	struct nvmeib_rdma_listen_params *params);
void nvmeib_rdma_stop_listen(struct nvmeib_rdma_cm *);
bool nvmeib_rdma_listener_has_conns(struct nvmeib_rdma_cm *);
int nvmeib_rdma_update_lb_listen(struct nvmeib_rdma_cm *cm, 
				 struct nvmeib_rdma_listen_lb_params *params);
/* extract context */
void *nvmeib_rdma_get_listener_context(struct nvmeib_rdma_cm *cm);
void *nvmeib_rdma_get_connection_context(struct nvmeib_rdma_cm *cm);

/* reject login request accroding to connection type */
void nvmeib_rdma_login_reject(struct nvmeib_rdma_cm *cm, void *rej, u8 len);
/* send disconnet */
int nvmeib_rdma_disconnect(struct nvmeib_rdma_cm *cm, struct ib_qp *qp,
	bool *wait_for_drep);
/* send reply to peer disconnet */
int nvmeib_rdma_disconnect_reply(struct nvmeib_rdma_cm *cm);
/* notify about qp events */
int nvmeib_rdma_notify(struct nvmeib_rdma_cm *cm, enum ib_event_type event);
bool nvmeib_is_conn_ib(struct nvmeib_rdma_cm *cm);
/* create a connection descriptor */
struct nvmeib_rdma_cm *nvmeib_rdma_create_cm(struct nvmeib_dev *dev, int port,
	enum nvmeib_rdma_type type);
/* destroy connection descriptor */
void nvmeib_rdma_destroy_cm(struct nvmeib_rdma_cm *cm);
/* try to invalidate connection descriptor */
bool nvmeib_rdma_try_inv_cm(struct nvmeib_rdma_cm *cm);

/* whether to expect a last wqe event */
enum nvmeib_rdma_expect_last_wqe nvmeib_rdma_expect_last_wqe(struct nvmeib_rdma_cm *cm);

/* whether to wait for drep */
bool nvmeib_rdma_wait_for_drep(struct nvmeib_rdma_cm *cm);

/* remote qp number */
int nvmeib_rdma_get_remote_qpn(struct nvmeib_rdma_cm *cm, u32 *remote_qpn);

int nvmeib_rdma_read_gids(struct nvmeib_rdma_cm *cm, union ib_gid *sgid,
		    union ib_gid *dgid);

/**
 * allocates a QP associated with the specified rdma_cm_id and transitions it for
 * sending and receiving. The actual capabilities and properties
 * of the created QP will be returned to the user through the
 * qp_init_attr parameter.
 *
 * @author yaron (4/29/2015)
 *
 * @param cm - connection identifier
 * @param pd - protection domaim for the qp
 * @param qp_init - initial qp attributes
 */
struct ib_qp_init_attr;
struct ib_qp *nvmeib_rdma_create_qp(struct nvmeib_rdma_cm *cm_id,
	struct ib_pd *pd, struct ib_qp_init_attr *qp_init, int port, u16 pkey,
	int qp_access);

struct ib_qp *nvmeib_rdma_create_rdda_qp(struct nvmeib_rdma_cm *cm_id,
	struct ib_pd *pd, struct ib_qp_init_attr *qp_init, int port, u16 pkey,
	int qp_access);

/**
 *  destroys a QP, will deallocate qp on cm_id in case of roce
 *
 * @author yaron (4/29/2015)
 *
 * @param cm_id - connection identifier
 * @param qp - qp, required only in infiniband case
 */
void nvmeib_rdma_destroy_qp(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp);

void nvmeib_rdma_destroy_rdda_qp(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp);

struct rdma_conn_param;
int nvmeib_rdma_accept(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp,
	struct rdma_conn_param *conn_params);

struct nvmeib_rdma_path_info {
	struct ib_sa_path_rec *path;
	struct nvmeib_dev *dev;
	struct ib_sa_client *sa;
	int src_port;
	u8 sgid_index;
	u16 pkey;
	u64 service_id;
	u16 service_port;
	enum nvmeib_rdma_type rdma_type;
};

int nvmeib_rdma_find_path(struct nvmeib_rdma_path_info *info);
int nvmeib_rdma_start_path_connection(struct nvmeib_rdma_cm *cm,
	struct nvmeib_rdma_path_info *info);
int nvmeib_rdma_connect(struct nvmeib_rdma_cm *cm,
	struct rdma_conn_param *conn_params,
	int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
	void *context);

/* Code for wrapping IB Event Handler to work around driver differences */
struct ib_event;
struct nvmeib_rdma_event_handler;
typedef void (*nvmeib_rdma_event_handler_fn)(struct nvmeib_rdma_event_handler *event_handler, 
			   struct ib_event *event);
struct nvmeib_rdma_event_handler {
	struct ib_device *ib_dev;
	nvmeib_rdma_event_handler_fn handler_fn;
	void *ctx;
};
int nvmeib_rdma_register_event_handler(struct ib_device *ib_dev, 
				       nvmeib_rdma_event_handler_fn handler_fn,
				       void *ctx, struct nvmeib_rdma_event_handler **handler);
int nvmeib_rdma_unregister_event_handler(struct nvmeib_rdma_event_handler *handler);

int nvmeib_rdma_register_net_notifiers(void);
void nvmeib_rdma_unregister_net_notifiers(void);

int nvmeib_rdma_proc_create(struct proc_dir_entry *proc_dir);
void nvmeib_rdma_proc_remove(struct proc_dir_entry *proc_dir);

struct nvmeib_rdma_evt_ctx *nvmeib_rdma_evt_ctx_create(void *ctx);
void nvmeib_rdma_evt_ctx_stop(struct nvmeib_rdma_evt_ctx *c);
void nvmeib_rdma_evt_ctx_destroy(struct nvmeib_rdma_evt_ctx *c);
void *nvmeib_rdma_evt_ctx_get(struct nvmeib_rdma_evt_ctx *c);
void nvmeib_rdma_evt_ctx_put(struct nvmeib_rdma_evt_ctx *c);

#define DEBUG_CM_OWNER
#ifdef DEBUG_CM_OWNER
void nvmeib_rdma_cm_owner_set(struct nvmeib_rdma_cm *cm, int old, int new);
int nvmeib_rdma_cm_is_owner(struct nvmeib_rdma_cm *cm);
#else

#define nvmeib_rdma_cm_owner_set(cm, old, new) do {\
	(void)cm;\
	(void)old;\
	(void)new;\
} while(0)

#define nvmeib_rdma_cm_is_owner(cm) ({\
	bool is_owner = true; \
	(void)cm;
	is_owner;
})
#endif

void *nvmeib_rdma_alloc_siw_wc_md(unsigned n_wcs, struct ib_wc *wcs, gfp_t gfp);
void nvmeib_rdma_free_siw_wc_md(void *siw_wc_md, unsigned n_wcs, struct ib_wc *wcs);

bool nvmeib_rdma_siw_wc_get_tx_md(const struct ib_wc *wc,
				ktime_t	*post_send_time,
				ktime_t *sent_time,
				ktime_t *ack_time,
				u16	*tx_cpu);

bool nvmeib_rdma_siw_wc_get_rx_md(const struct ib_wc *wc,
				ktime_t *first_ddp_recv_time,
				ktime_t *last_ddp_recv_time,
				ktime_t	*reap_time,
				u16	*rx_cpu,
				u16	*rx_queue,
				u32	*rx_skb_hash);

int nvmeib_rdma_init_find_path_sock(struct nvmeib_dev *nvdev);
void nvmeib_rdma_free_find_path_sock(struct nvmeib_dev *nvdev);

int nvmeib_rdma_init_iw_cm_inv_stats(struct nvmeib_dev *nvdev);
void nvmeib_rdma_free_iw_cm_inv_stats(struct nvmeib_dev *nvdev);

#endif
