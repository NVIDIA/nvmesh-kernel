/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_rdma.h"
#include "nvmeib.h"
#include "nvmeib_utils.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib_public.h"
#include "nvmeib_public_procfs.h"
#include "nvmeibm_trace.h"
#include "nvmeib_q.h"
#include "nvmeib_version_shared.h"

#if !HAS_IB_QUERY_GID
#include "rdma/ib_cache.h"
#endif

#include "rdma/iw_cm.h"

#if !defined(IB_HAS_CMA_PRIV_H)
#	define IB_HAS_CMA_PRIV_H 0
#	define USE_CMA_INTERNAL_QP 1
#elif IB_HAS_CMA_PRIV_H
#	include <infiniband/core/cma_priv.h>
#	define USE_CMA_INTERNAL_QP 0
#endif

#include "rdma/rdma_user_cm.h"

#include <linux/inetdevice.h>

unsigned qp_timeout = NVMEIB_QP_TIMEOUT;
module_param(qp_timeout, uint, 0644);
MODULE_PARM_DESC(qp_timeout, "QP timeout (4.096 x 2^N) us");

unsigned qp_retry_cnt = NVMEIB_RETRY_CNT;
module_param(qp_retry_cnt, uint, 0644);
MODULE_PARM_DESC(qp_retry_cnt, "QP retry count");

bool nvmeib_ib_cross_subnet_enb = false;
module_param_named(ib_cross_subnet, nvmeib_ib_cross_subnet_enb, bool, 0644);
MODULE_PARM_DESC(ib_cross_subnet, "Enable cross subnet, IB transport");

unsigned nvmeib_ipv6_mode = 1;
module_param_named(ipv6_mode, nvmeib_ipv6_mode, uint, 0644);
MODULE_PARM_DESC(ipv6_mode, "IPv6 Mode: 0 - No IPv6, 1 - IPv6 enabled, prefer IPv4 addresses, 2 - IPv6 enabled and preferred, 3 - IPv6 Only");

#if KS_INET_MATCH_HAS_SDIF | KS_HAS_NEW_INET_MATCH_LOWER | KS_HAS_NEW_INET_MATCH_CAPS
bool nvmeib_iwarp_find_path_sock = true;
module_param_named(iwarp_find_path_sock, nvmeib_iwarp_find_path_sock, bool, 0644);
MODULE_PARM_DESC(iwarp_find_path_sock, "Use a socket for iwarp_find_path. Reduces load on siw_cm_wq");
#else
/* matching listener by bound_dev_if is not supported by kernel.
 * Requires listening on all addresses of a netdev and updating when the address changes.
 * Not currently supported */
bool nvmeib_iwarp_find_path_sock = false;
#endif

int nvmeib_iwarp_find_path_sock_port = 8915;
module_param_named(iwarp_find_path_sock_port, nvmeib_iwarp_find_path_sock_port, int, 0444);
MODULE_PARM_DESC(iwarp_find_path_sock_port, "listener port for find path socket listener");

#if defined(RDMA_REJECT_HAS_REASON) && RDMA_REJECT_HAS_REASON
	#define __nvmeib_rdma_reject_reason(id, private_data, private_data_len, reason) rdma_reject(id, private_data, private_data_len, reason)
#else
	#define __nvmeib_rdma_reject_reason(id, private_data, private_data_len, reason) rdma_reject(id, private_data, private_data_len)
#endif

unsigned nvmeib_iwarp_cm_inv_time_sec = NVMEIB_WAIT_CM_INV_SEC;
module_param_named(iwarp_cm_inv_time_sec, nvmeib_iwarp_cm_inv_time_sec, uint, 0644);
MODULE_PARM_DESC(iwarp_cm_inv_time_sec, "Timeout for iWARP CM Invalidate (in seconds");

int nvmeib_cm_ephemeral_debug_level = 4;
module_param_named(cm_ephemeral_debug_level, nvmeib_cm_ephemeral_debug_level, int, 0644);
MODULE_PARM_DESC(cm_ephemeral_debug_level, "CM Ephemeral Tracing Debug Level [0..6]");

/* keeps internal parameters of connection */

/* connection enumertion listening, connecting... */
enum cm_type {
	_cm_listen,
	_cm_connection,
};

/* store the type of the connection */
struct nvmeib_rdma_cm {
	/* connection type, roce, infiniband etc... */
	enum nvmeib_rdma_type rdma_type;
	/* style of connection */
	enum cm_type cm_type;
#ifdef DEBUG_CM_OWNER
	int owner_pid;
#endif
};

/* listening descriptor object */
struct rdma_listener {
	/* the handle */
	struct nvmeib_rdma_cm cm;
	/* holds the device to listen on */
	struct nvmeib_dev *dev;
	/* connection list */
	struct list_head connections;
	/* list guard */
	spinlock_t connection_guard;
	/* caller context (cookie) */
	void *context;
	/* callbacks */
	/* called on new connection request */
	int (*new_connection)(struct nvmeib_rdma_cm *cm,
						  struct nvmeib_rdma_conn_params *p);
	/* called when the other side generated an event */
	int (*on_peer_event)(struct nvmeib_rdma_cm *cm,
						 struct nvmeib_rdma_event *p);
};

/* infiniband specific part of the listening descriptor */
struct ib_rdma_listener {
	struct ib_cm_id *cm_id;         /* ib connection descriptor */
	u64 service_id;                 /* ib service unique id */
	struct rdma_listener listener;  /* the base, common listening descriptor */
};

#define FIND_PATH_PROT_KEY "NVMesh Find Path"
static const char *find_path_prot_key = FIND_PATH_PROT_KEY;

enum find_path_prot_flags {
	FIND_PATH_PROT_FLAG_RESP = (1 << 0),
};

struct find_path_prot_resp {
	char key[16];
	__be32 len;
	__be32 flags;
	__be64 version;
	__be64 commit;
	char netdev_name[IFNAMSIZ];
	__be16 iw_primary_port;
	__be16 iw_2nd_base_port;
	__be16 iw_2nd_num_ports;
	u16 crc16;
} __attribute__((packed));

struct roce_rdma_listener;
struct find_path_sock_cep;

enum find_path_sock_work_type {
	FIND_PATH_WORK_TYPE_NONE = 0,
	FIND_PATH_WORK_TYPE_CONNECT,
	FIND_PATH_WORK_TYPE_ACCEPT,
	FIND_PATH_WORK_TYPE_RELEASE,
	FIND_PATH_WORK_TYPE_READ_RESP,
};

struct find_path_sock_work {
	struct list_head link;
	struct workqe_struct work;
	struct find_path_sock_cep *cep;
	enum find_path_sock_work_type work_type;
};

#define FIND_PATH_SOCK_SHUTDOWN_TIMEOUT_SECS	2

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
#define SK_DATA_READY_FN_SIG(fn_name) void fn_name(struct sock *sk, int bytes)
#else
#define SK_DATA_READY_FN_SIG(fn_name) void fn_name(struct sock *sk)
#endif

struct find_path_sock_dev {
	/* Ref counter */
	struct kref ref;
	/* Back pointer to device */
	struct nvmeib_dev *nvdev;
	/* Lock */
	struct mutex lock;
	/* find path socket list */
	struct list_head sock_cep_list;
	/* find path socket workqueue */
	struct workq_struct *find_path_sock_wq;
	/* number of connections ref count */
	struct nvmeib_ref ref_conn;
};

struct find_path_sock_cep {
	/* Link into listener cep list or device cep list */
	struct list_head link;
	/* cep ref count */
	struct kref ref;
	struct socket *s;
	/* Pointer to parent */
	struct find_path_sock_dev *parent;
	/* State */
	enum find_path_cep_state state;
	/* Handler State */
	enum {
		FIND_PATH_CEP_HANDLER_IDLE = 0,
		FIND_PATH_CEP_HANDLER_CALLED,
		FIND_PATH_CEP_HANDLER_INVALIDATED
	} handler_state;
	/* Lock */
	spinlock_t lock;
	/* Waitqueue for in-use */
	wait_queue_head_t	waitq;
	/* true for initiator */
	short			initiator;
	/* in-use flag */
	short			in_use;
	/* in-use pid */
	int			in_use_pid;
	/* pointer to cm listener/conn */
	union {
		struct roce_rdma_listener *listener;
		struct roce_rdma_connection *conn;
	};
	/* Saved upcalls of socket */
	void (*orig_sk_state_change)(struct sock *sk);
	SK_DATA_READY_FN_SIG((* orig_sk_data_ready));
	/* find_path_sock_work free list */
	struct list_head work_free_list;
	/* response data */
	struct find_path_prot_resp *resp;
	int resp_recv;
	/* [NVMESH-5267]: Prevent socket from being stuck in TCP_FIN_WAIT */
	TIMER_LIST_INSTANCE(sock_shutdown_timer);
};

#define FIND_PATH_SOCK_CONN_MAX_WORK		4
#define FIND_PATH_SOCK_LISTENER_MAX_WORK	64

/* roce specific part of the listening descriptor */
struct roce_rdma_listener {
	struct rdma_listener listener; /* the base, common listening descriptor */
	union {
		struct rdma_cm_id *cm_id;      /* roce internal descriptor */
		struct iw_cm_id *iw_cm_id;		/* iwarp internal descriptor */
	};
	int port;                      /* transport protocol port */
	bool ipv4_only;			/* only listen on ipv4 addresses */
	bool iw_primary;		/* For iWARP, is this the primary listener */
	int iw_2nd_base_port;		/* Base port for iWARP secondary listeners */
	int iw_2nd_num_ports;		/* Number of secondary ports for iWARP secondary listeners */
	/* Endpoint for find-path socket listener */
	struct find_path_sock_cep *find_path_sock_listen_cep;
};

/* loopback specific part of the listening descriptor */
struct lb_rdma_listener {
	struct rdma_listener listener; /* the base, common listening descriptor */
	struct ib_device *ib_dev;
	int port;
	union ib_gid gid;
	u8 gid_index;
	u8 roce_mac[ETH_ALEN];
	u16 pkey_index;
	enum rdma_link_layer link_layer;
	struct list_head link;
	bool active;
	struct nvmeib_ref n_conns;
	struct mutex guard;
};

static struct list_head lb_list = LIST_HEAD_INIT(lb_list);
static DEFINE_MUTEX(lb_list_guard);
static int num_lb_listeners = 0;
static atomic_t lb_id_cnt = ATOMIC_INIT(1);

/* connection object */
struct rdma_connection {
	/* handle */
	struct nvmeib_rdma_cm cm;
	/* the device */
	struct nvmeib_dev *dev;
	/* the creator - the listener or NULL if created by user */
	struct rdma_listener *listener;
	/* the qp */
	struct ib_qp *qp;
	enum nvmeib_rdma_expect_last_wqe expect_last_wqe;
	/* the connection event handler */
	int (*event_handler)(void *context, struct nvmeib_rdma_event *event);
	/* handler context */
	void *context;
	/* my link at my creator */
	struct list_head link;
	/* whether to wait for drep before destroying cm */
	bool wait_for_drep;
	/* remote qpn */
	u32 remote_qpn;
	/* connection lock (currently only used by roce_rdma_connection */
	spinlock_t lock;
};

enum ib_conn_state {
	NVMEIB_CM_CONN_INIT 	= 0,
	NVMEIB_CM_CONN_REP_RCVD = 1,
	NVMEIB_CM_CONN_INV  	= 2,
};

/* represent an ib connection */
struct ib_rdma_connection {
	/* base */
	struct rdma_connection conn;
	/* ib internal connection */
	struct ib_cm_id *cm_id;
	/* destination address */
	struct ib_sa_path_rec path;
	u64 service_id;
	u16 pkey;
	/* ib conn state */
	enum ib_conn_state state;
	/* From primary path */
	union ib_gid dgid;
	union ib_gid sgid;
};

enum roce_conn_state {
	NVMEIB_ROCE_CM_INIT = 0,
	NVMEIB_ROCE_CM_EST_RCVD = 1,
	NVMEIB_ROCE_CM_INV = 2,
	NVMEIB_ROCE_CM_INV_WAIT_EVENT = 3,
	NVMEIB_ROCE_CM_INV_GOT_EVENT = 4, /* Got CM event indicating cancel had been successful */
	NVMEIB_ROCE_CM_INV_WAIT_CLOSE_EVENT = 5, /* Got CM event indicating connect happened after attempt to cancel - Will need to wait for IW_CM_EVENT_CLOSE */
	NVMEIB_ROCE_CM_CONN_ERR = 6,
	NVMEIB_ROCE_CM_IW_CM_CLOSE_RCVD = 7,	/* Got IW_CM_EVENT_CLOSE - QP already disconnected by LLP */
	NVMEIB_ROCE_CM_DESTROYING = 8, /* In the process of destroying the cm_id */
};

/* represent roce connection */
struct roce_rdma_connection {
	/* base */
	struct rdma_connection conn;
	union {
		struct rdma_cm_id *cm_id;      /* roce internal descriptor */
		struct iw_cm_id *iw_cm_id;		/* iwarp internal descriptor */
	};

	/* destination and source address */
	union {
		struct sockaddr_in6 s6;
		struct sockaddr_in s;
	} dst_addr, src_addr;

	/* a completion for path search op */
	struct completion done;
	/*if true, the connection is temporary*/
	bool temp;
	/* path resolve statue */
	int status;
	/* roce conn state */
	enum roce_conn_state state;
	/* qp is internal to rdma_cm_id */
	bool cma_internal_qp;
	/* Used by iwarp for nvmeib_rdma_try_inv_cm */
	struct completion *cancel_iwcm_conn_done;
	bool srq;
	struct mutex qp_guard;
	/* set when connection is using a socket instead of the iw_cm for find-path */
	struct find_path_sock_cep *find_path_sock_cep;
	/* stats for iw_cm invalidate */
	struct {
		ktime_t start_wait;
		ktime_t end_wait;
		bool wait_close;
	} iw_cm_inv_stats;
	/* [NVMESH-4168]: Handler guard to prevent conn being destroy while handler is run.
	  Only needed for iw_cm, rdma_cm already has one */
	struct mutex handler_guard;
};

enum lb_rdma_connection_state {
	LB_CONN_IDLE = 0,
	LB_CONN_BOUND,
	LB_CONN_CONNECTING,
	LB_CONN_ACCEPTING,
	LB_CONN_CONNECTED,
	LB_CONN_DISCONNECTING,
	LB_CONN_DISCONNECTED,
	LB_CONN_ERROR,
	LB_CONN_DESTROYING,
};

struct lb_rdma_connection {
	/* base */
	struct rdma_connection conn;
	/* ids */
	u32 local_id;
	u32 remote_id;
	/* state */
	enum lb_rdma_connection_state state;
	/* guards */
	struct mutex handler_mutex;
	struct mutex qp_mutex;
	struct kref refcount;
	/* Params to pass from connect to accept */
	u32 send_psn;
	u8 req_depth;
	u8 resp_rsrc;
	u32 local_qp_num;
	u32 remote_qp_num;
};

static void lb_free_id(struct kref *kref);
static void lb_destroy_id(struct nvmeib_rdma_cm *cm_id);
static int lb_destroy_qp(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp);
static int lb_disconnect(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp);
static int lb_reject(struct nvmeib_rdma_cm *cm, void *rej, u8 len);
static int lb_accept(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp, struct rdma_conn_param *conn_params);
static int start_lb_listener(struct nvmeib_rdma_cm *cm_id);
static bool lb_try_inv_cm(struct nvmeib_rdma_cm *cm_id);

static int ib_set_qp_err(struct ib_qp *qp);

static int roce_conn_qp_to_rts(struct roce_rdma_connection *r_conn,
			       struct rdma_conn_param *conn_param);
static int roce_send_rtu(struct roce_rdma_connection *conn);

static int iwarp_connect(struct roce_rdma_connection *roce,
						 struct rdma_conn_param *conn_params,
						 int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
						 void *context);
static int iwarp_find_path_handler(void *context, struct nvmeib_rdma_event *event);
static void find_path_sock_work_handler_free_cep(struct find_path_sock_cep *cep,
						 const char *cep_put_str);

/* Private data header for IW CM */
enum {
	IW_CM_REJ_FIND_PATH = cpu_to_be16(0x0001),
};

struct iw_cm_pd_hdr {
	union {
		struct {
			__be32 qpn; /* If (-1), connection is only used to find-path */
		} req;
		struct {
			__be32 qpn;
		} rsp;
		struct {
			__be16 flags;
			__be16 reason;
		} rej;
	};
} __attribute__((packed));

struct iw_cm_pd {
	struct iw_cm_pd_hdr hdr;
	u8 user_pd[RDMA_MAX_PRIVATE_DATA - sizeof(struct iw_cm_pd_hdr)];
};

static u32 find_path_qpn = (u32)-1;

/* downcast and get the litener parametr */
static inline struct rdma_listener *cm_2_l(struct nvmeib_rdma_cm *cm)
{
	struct rdma_listener *l = NULL;

	if (cm && cm->cm_type == _cm_listen)
		l = container_of(cm, struct rdma_listener, cm);
	else if (!cm)
		_NE(error_nvmeib_rdma_cm_2_l, "got null object in cm_2_l");
	else
		_NE(error_1_nvmeib_rdma_cm_2_l, "cm_2_l expected listening object");
	return l;
}

/* downcast and get the infiniband part of the connection */
static inline struct ib_rdma_listener *l_2_ib(struct rdma_listener *listener)
{
	struct ib_rdma_listener *l = NULL;

	if (listener && listener->cm.rdma_type == _rdma_ib)
		l = container_of(listener, struct ib_rdma_listener, listener);
	else if (!listener)
		_NE(error_nvmeib_rdma_l_2_ib, "got null object in l_2_ib");
	else
		_NE(error_1_nvmeib_rdma_l_2_ib, "l_2_ib expected ib listener object");
	return l;
}

/*downcast to get the roce part of the listener*/
static inline struct roce_rdma_listener *l_2_roce(
	struct rdma_listener *listener)
{
	struct roce_rdma_listener *l = NULL;

	if (listener && (listener->cm.rdma_type == _rdma_roce ||
		listener->cm.rdma_type == _rdma_iwarp))
		l = container_of(listener, struct roce_rdma_listener, listener);
	else if (!listener)
		_NE(error_nvmeib_rdma_l_2_roce, "got null object in l_2_roce");
	else
		_NE(error_1_nvmeib_rdma_l_2_roce, "l_2_roce expected roce listener object");
	return l;
}

/*downcast to get the lb part of the listener*/
static inline struct lb_rdma_listener *l_2_lb(
	struct rdma_listener *listener)
{
	struct lb_rdma_listener *l = NULL;

	if (listener && listener->cm.rdma_type == _rdma_lb)
		l = container_of(listener, struct lb_rdma_listener, listener);
	else if (!listener)
		_NE(error_nvmeib_rdma_l_2_lb, "got null object in l_2_lb");
	else
		_NE(error_1_nvmeib_rdma_l_2_lb, "l_2_lb expected roce listener object");
	return l;
}

/* dowcast to the ib part of the connection */
static inline struct ib_rdma_listener *cm_2_ibl(struct nvmeib_rdma_cm *cm)
{
	return l_2_ib(cm_2_l(cm));
}

/* downcast to to roce part of the listener */
static inline struct roce_rdma_listener *cm_2_rocel(struct nvmeib_rdma_cm *cm)
{
	return l_2_roce(cm_2_l(cm));
}

/* downcast to to lb part of the listener */
static inline struct lb_rdma_listener *cm_2_lbl(struct nvmeib_rdma_cm *cm)
{
	return l_2_lb(cm_2_l(cm));
}

/* downcast and get the litener parametr */
static inline struct rdma_connection *cm_2_c(struct nvmeib_rdma_cm *cm)
{
	struct rdma_connection *c = NULL;

	if (cm && cm->cm_type == _cm_connection)
		c = container_of(cm, struct rdma_connection, cm);
	else if (!cm)
		_NE(error_nvmeib_rdma_cm_2_c, "got null object in cm_2_c");
	else
		_NE(error_1_nvmeib_rdma_cm_2_c, "cm_2_c expected connection object");
	return c;
}

/* downcast rdma_connection to ib connection (ib_rdma_connection) */
static inline struct ib_rdma_connection *conn_2_ib(
	struct rdma_connection *conn)
{
	struct ib_rdma_connection *ib = NULL;

	if (conn && conn->cm.rdma_type == _rdma_ib)
		ib = container_of(conn, struct ib_rdma_connection, conn);
	else if (!conn)
		_NE(error_nvmeib_rdma_conn_2_ib, "got null object in conn_2_ib");
	else
		_NE(error_1_nvmeib_rdma_conn_2_ib, "conn_2_ib expected connection ib object");
	return ib;
}

/* downcast rdma comnnection to roce connection (roce_rdma_connection) */
static inline struct roce_rdma_connection* conn_2_roce(
	struct rdma_connection *conn)
{
	struct roce_rdma_connection *roce = NULL;

	if (conn && (conn->cm.rdma_type == _rdma_roce || conn->cm.rdma_type == _rdma_iwarp))
		roce = container_of(conn, struct roce_rdma_connection, conn);
	else if (!conn)
		_NE(error_nvmeib_rdma_conn_2_roce, "got null object in conn_2_roce");
	else
		_NE(error_1_nvmeib_rdma_conn_2_roce, "conn_2_roce expected connection roce object @RDMA_TYPE", conn->cm.rdma_type);
	return roce;
}

/* downcast rdma comnnection to lb connection (lb_rdma_connection) */
static inline struct lb_rdma_connection* conn_2_lb(
	struct rdma_connection *conn)
{
	struct lb_rdma_connection *lb = NULL;

	if (conn) {
		if (conn->cm.rdma_type == _rdma_lb ||
			conn->cm.rdma_type == _rdma_lb_accept)
			lb = container_of(conn, struct lb_rdma_connection, conn);
		else
			_NE(error_nvmeib_rdma_conn_2_lb, "Invalid rdma_type @RDMA_TYPE for conn @CONN", conn->cm.rdma_type, conn);
	}
	else if (!conn)
		_NE(error_1_nvmeib_rdma_conn_2_lb, "got null object in conn_2_lb");
	else
		_NE(error_2_nvmeib_rdma_conn_2_lb, "conn_2_lb expected connection lb object (not @RDMA_TYPE)", conn->cm.rdma_type);
	return lb;
}

static int __attribute__((unused)) __ib_query_gid(struct ib_device *device, u8 port_num, int index,
	union ib_gid *gid)
{
#if HAS_IB_QUERY_GID
#	if IB_HAS_GID_ATTR
	struct ib_gid_attr attr;
	bool roce = rdma_port_get_link_layer(device, port_num) == IB_LINK_LAYER_ETHERNET;
	return ib_query_gid(device, port_num, index, gid, (roce ? &attr : NULL));
#	else
	return ib_query_gid(device, port_num, index, gid);
#	endif
#else
	return rdma_query_gid(device, port_num, index, gid);
#endif
}

/* downcast to the infinband part of the connection */
static inline struct ib_rdma_connection *cm_2_ibc(struct nvmeib_rdma_cm *cm)
{
	return conn_2_ib(cm_2_c(cm));
}

bool nvmeib_is_conn_ib(struct nvmeib_rdma_cm *cm)
{
	return (cm->rdma_type == _rdma_ib);
}
EXPORT_SYMBOL(nvmeib_is_conn_ib);

/* downcast to to roce part of the connection */
static inline struct roce_rdma_connection *cm_2_rocec(struct nvmeib_rdma_cm *cm)
{
	return conn_2_roce(cm_2_c(cm));
}

/* downcast to to lb part of the connection */
static inline struct lb_rdma_connection *cm_2_lbc(struct nvmeib_rdma_cm *cm)
{
	struct lb_rdma_connection *lbc = NULL;
	if (cm && cm->cm_type == _cm_connection) {
		if (cm->rdma_type == _rdma_lb || cm->rdma_type == _rdma_lb_accept)
			lbc = conn_2_lb(cm_2_c(cm));
	}
	return lbc;
}

/* given connection parameters create the internal
   structure of listening paramters
*/
static struct nvmeib_rdma_cm* create_listen_cm(
	struct nvmeib_rdma_listen_params *params)
{
	struct nvmeib_rdma_cm *cm = NULL;
	struct rdma_listener *listener = NULL;
	struct ib_rdma_listener *ib;
	struct roce_rdma_listener *roce;
	struct lb_rdma_listener *lb = NULL;

	NFIN;
	if (params->type == _rdma_ib) {
		if ((ib = kzalloc(sizeof(*ib), GFP_KERNEL))) {
			ib->listener.cm.rdma_type = _rdma_ib;
			ib->service_id = params->ib.service_id;
			listener = &ib->listener;
		}
		else {
			_NE(error_nvmeib_rdma_create_listen_cm, "Fail to allocate RDMA connection");
			goto out;
		}
	}
	else if (params->type == _rdma_roce || params->type == _rdma_iwarp) {
		if ((roce = kzalloc(sizeof(*roce), GFP_KERNEL))) {
			roce->listener.cm.rdma_type = params->type;
			roce->port = params->roce.port;
			roce->ipv4_only = params->roce.ipv4_only;
			roce->iw_primary = params->roce.iw_primary;
			roce->iw_2nd_base_port = params->roce.iw_2nd_base_port;
			roce->iw_2nd_num_ports = params->roce.iw_2nd_num_ports;
			listener = &roce->listener;
		}
		else {
			_NE(error_1_nvmeib_rdma_create_listen_cm, "Fail to allocate RDMA connection");
			goto out;
		}
	}
	else if (params->type == _rdma_lb) {
		if ((lb = kzalloc(sizeof(*lb), GFP_KERNEL))) {
			lb->listener.cm.rdma_type = _rdma_lb;
			lb->ib_dev = params->dev->ib_dev;
			lb->port = params->port;
			lb->gid = params->lb.gid;
			lb->gid_index = params->lb.gid_index;
			memcpy(lb->roce_mac, params->lb.roce_mac, ETH_ALEN);
			lb->pkey_index = params->lb.pkey_index;
			lb->link_layer = params->lb.link_layer;
			nvmeib_ref_init(&lb->n_conns);
			mutex_init(&lb->guard);
			listener = &lb->listener;
		}
		else {
			_NE(error_2_nvmeib_rdma_create_listen_cm, "Fail to allocate RDMA connection");
			goto out;
		}
	}
	if (listener) {
		listener->dev = params->dev;
		listener->cm.cm_type = _cm_listen;
		listener->context = params->context;
		listener->new_connection = params->new_connection;
		listener->on_peer_event = params->on_peer_event;
		INIT_LIST_HEAD(&listener->connections);
		spin_lock_init(&listener->connection_guard);
		cm = &listener->cm;
	}
	else {
		_NE(error_3_nvmeib_rdma_create_listen_cm, "Unsupperted RDMA type @INT_TYPE", (int)params->type);
		goto out;
	}

out:
	NFOUT;
	return cm;
}

static struct lb_rdma_connection *find_lbc(struct lb_rdma_listener *lbl, u32 id)
{
	struct lb_rdma_connection *lbc = NULL;
	struct rdma_connection *conn;
	mutex_lock(&lbl->guard);
	list_for_each_entry(conn, &lbl->listener.connections, link) {
		if (!(lbc = conn_2_lb(conn)))
			continue;
		if (lbc->local_id != id) {
			lbc = NULL;
			continue;
		}
		kref_get(&lbc->refcount);
		break;
	}
	mutex_unlock(&lbl->guard);
	return lbc;
}

static struct lb_rdma_connection *find_remote_lbc(struct lb_rdma_connection *lbc)
{
	struct lb_rdma_connection *remote_lbc = NULL;
	struct lb_rdma_listener *lbl = l_2_lb(lbc->conn.listener);
	if (!lbl) {
		_NT(trace_nvmeib_rdma_find_remote_lbc, "lb conn @LOCAL_ID (@LBC) (state @STATE_GUARD) not bound to listener",
		   lbc->local_id, lbc, lbc->state);
	}
	else if (!(remote_lbc = find_lbc(lbl, lbc->remote_id))) {
		_NT(trace_1_nvmeib_rdma_find_remote_lbc, "remote lb conn @REMOTE_ID not found on listener @LBL (@IB_DEV_NAME:@PORT)",
		   lbc->remote_id, lbl, lbl->ib_dev->name, lbl->port);
	}
	return remote_lbc;
}

static int lbc_handler(struct lb_rdma_connection *lbc, struct nvmeib_rdma_event *evt)
{
	unsigned long flags;
	int rv = 0;

	if (!lbc) {
		rv = -EINVAL;
		goto out;
	}

	spin_lock_irqsave(&lbc->conn.lock, flags);
	if (lbc->state == LB_CONN_DESTROYING) {
		_NT(trace_lbc_handler_lbc_destroying,
		    "loopback conn @CM_ID is being destroyed", lbc);
		rv = -EINVAL;
	}
	spin_unlock_irqrestore(&lbc->conn.lock, flags);
	if (rv < 0)
		goto out;

	mutex_lock(&lbc->handler_mutex);
	if (lbc->conn.cm.rdma_type == _rdma_lb)
		rv = lbc->conn.event_handler(lbc->conn.context, evt);
	else if (lbc->conn.cm.rdma_type == _rdma_lb_accept)
		rv = lbc->conn.listener->on_peer_event(&lbc->conn.cm, evt);
	else
		rv = -EINVAL;
	mutex_unlock(&lbc->handler_mutex);

	if (rv < 0) {
		/* CM handler returned < 0, that means we delete ourselves */
		lb_destroy_id(&lbc->conn.cm);
	}

out:
	return rv;
}

static int lbc_remote_handler(struct lb_rdma_connection *lbc, struct nvmeib_rdma_event *evt)
{
	struct lb_rdma_connection *lbc_remote = find_remote_lbc(lbc);
	int rv = 0;

	if (!lbc_remote) {
		rv = -EINVAL;
		goto out;
	}

	rv = lbc_handler(lbc_remote, evt);

	/* Dec refcount (was incremented by find_remote_lbc) */
	kref_put(&lbc_remote->refcount, &lb_free_id);

out:
	return rv;
}

static void remove_all_lb_conns(struct lb_rdma_listener *lbl)
{
	struct rdma_connection *conn;
	struct lb_rdma_connection *lbc;
	struct nvmeib_rdma_event rem_evt = {};
	int ref_cnt;

	NFIN;
	/* Stop new connections */
	nvmeib_ref_release_start(&lbl->n_conns);

	/* Send Device-Removed Event to all connections */
	rem_evt.event = NVMEIB_DEVICE_REMOVED;
	mutex_lock(&lbl->guard);
	list_for_each_entry(conn, &lbl->listener.connections, link) {
		if ((lbc = conn_2_lb(conn))) {
			_NT(trace_nvmeib_rdma_remove_all_lb_conns, "Removing lb conn @LOCAL_ID (@LBC) from listener @LBL connections",
				lbc->local_id, lbc, lbl);
			lbc_handler(lbc, &rem_evt);
		}
	}
	mutex_unlock(&lbl->guard);

	/* Wait for connections to remove themselves */
	nvmeib_ref_release_wait(&lbl->n_conns);

	/* Check to see if any failed to remove */
	mutex_lock(&lbl->guard);
	ref_cnt = nvmeib_ref_read(&lbl->n_conns);
	if (ref_cnt) {
		_NW(warn_nvmeib_rdma_remove_all_lb_conns, "Listener @LBL going down still has @REF_CNT connections", lbl, ref_cnt);
		while ((conn = list_first_entry_or_null(
				&lbl->listener.connections, struct rdma_connection, link))) {
			_NT(remove_all_lb_conns_t1, "Forcibly removing conn @PTR from listener @PTR", conn, lbl);
			list_del(&conn->link);
			conn->listener = NULL;
			if ((lbc = conn_2_lb(conn)))
				kref_put(&lbc->refcount, lb_free_id);
			nvmeib_ref_put(&lbl->n_conns);
		}
	}
	mutex_unlock(&lbl->guard);
	NFOUT;
}

static void stop_lb_listener(struct lb_rdma_listener *lbl)
{
	/* Remove from active list (if it hasn't already been stopped) */
	mutex_lock(&lb_list_guard);

	mutex_lock(&lbl->guard);
	if (lbl->active) {
		lbl->active = false;
		list_del(&lbl->link);

		num_lb_listeners--;
		_NT(trace_nvmeib_rdma_stop_lb_listener, "Removed loopback listener on IB device @IB_DEV_NAME:@PORT. Num loopback listeners @NUM_LB_LISTENERS",
			lbl->ib_dev->name, lbl->port, num_lb_listeners);
	}
	mutex_unlock(&lbl->guard);

	mutex_unlock(&lb_list_guard);
}

static void free_lb_listener(struct nvmeib_rdma_cm *cm)
{
	struct lb_rdma_listener *lbl = NULL;

	NFIN;
	if (!(lbl = cm_2_lbl(cm)))
		return;

	/* stop listener */
	stop_lb_listener(lbl);

	/* Disconnect all connections */
	remove_all_lb_conns(lbl);

	/* Free listener */
	kfree(lbl);
	NFOUT;
}

static void find_path_dev_get(struct find_path_sock_dev *dev, const char *get_desc);
static void find_path_dev_put(struct find_path_sock_dev *dev, const char *put_desc);
static void find_path_cep_free(struct kref *ref);
static void iwarp_disconnect_sock_only(struct find_path_sock_cep *cep);

static inline void find_path_cep_get(struct find_path_sock_cep *cep, const char *get_desc)
{
	kref_get(&cep->ref);
	_ND(trace_find_path_cep_get, "Get conn endpoint @PTR - Desc: @STR New refcount: @COUNT",
	    cep, get_desc, kref_read(&cep->ref));
}

static inline void find_path_cep_put(struct find_path_sock_cep *cep, const char *put_desc)
{
	int refcount = kref_read(&cep->ref);
	_ND(trace_find_path_cep_put,
	    "Put conn endpoint @PTR - Desc: @STR New refcount: @COUNT", cep, put_desc, refcount - 1);
	BUG_ON(refcount < 1);
	kref_put(&cep->ref, find_path_cep_free);
}

static void find_path_cep_set_inuse(struct find_path_sock_cep *cep)
{
	unsigned long flags;
	int rv __attribute__((unused));
retry:
	_ND(trace_find_path_cep_set_inuse, "Conn Endpoint @PTR: try use @INT, pid=@K_PID\n",
	       cep, cep->in_use, cep->in_use_pid);

	find_path_cep_get(cep, "set-inuse");

	spin_lock_irqsave(&cep->lock, flags);

	if (cep->in_use) {
		spin_unlock_irqrestore(&cep->lock, flags);
		rv = wait_event_interruptible(cep->waitq, !cep->in_use);
		if (signal_pending(current))
			flush_signals(current);
		goto retry;
	} else {
		cep->in_use = 1;
		cep->in_use_pid = current->pid;
		spin_unlock_irqrestore(&cep->lock, flags);
	}
}

static void find_path_cep_set_free(struct find_path_sock_cep *cep)
{
	unsigned long flags;

	_ND(trace_find_path_cep_set_free, "Conn Endpoint @PTR: try use @INT, pid=@K_PID\n",
	    cep, cep->in_use, cep->in_use_pid);

	spin_lock_irqsave(&cep->lock, flags);
	BUG_ON(!cep->in_use);
	BUG_ON(cep->in_use_pid != current->pid);
	cep->in_use = 0;
	cep->in_use_pid = -1;
	spin_unlock_irqrestore(&cep->lock, flags);

	wake_up(&cep->waitq);

	find_path_cep_put(cep, "set-free");
}

/* free a listener */
static void free_cm(struct nvmeib_rdma_cm *cm)
{
	struct ib_rdma_listener *ibl;
	struct roce_rdma_listener *rocel;
	struct ib_rdma_connection *ibc;
	struct roce_rdma_connection *rocec;
	unsigned long flags;
	u64 start;

	NFIN;
	_NI(trace_free_cm,
		"freeing cm=@CM, cm_type=@CM_TYPE, rdma_type=@RDMA_TYPE",
		cm, cm->cm_type, cm->rdma_type);

	if (cm->cm_type == _cm_listen) {
		if (cm->rdma_type == _rdma_ib) {
			ibl = cm_2_ibl(cm);
			if (ibl && ibl->cm_id) {
				start = jiffies;
				_NT(trace_1_nvmeib_rdma_free_cm, "ib_destroy_cm_id cm=@CM", cm);
				ib_destroy_cm_id(ibl->cm_id);
				_NT(trace_2_nvmeib_rdma_free_cm, "end of ib_destroy_cm_id cm=@CM time=@LLD", cm,
					jiffies - start);
				ibl->cm_id = NULL;
			}
			kfree(ibl);
		}
		else if (cm->rdma_type == _rdma_roce || cm->rdma_type == _rdma_iwarp) {
			if ((rocel = cm_2_rocel(cm))) {
				if (rocel->find_path_sock_listen_cep) {
					struct find_path_sock_cep *listen_cep = rocel->find_path_sock_listen_cep;

					BUG_ON(cm->rdma_type != _rdma_iwarp);
					rocel->find_path_sock_listen_cep = NULL;
					find_path_cep_put(listen_cep, "listen-cep-ptr");
				}
				if (rocel->cm_id) {
					if (cm->rdma_type == _rdma_iwarp)
						iw_destroy_cm_id(rocel->iw_cm_id);
					else
						rdma_destroy_id(rocel->cm_id);
					rocel->cm_id = NULL;
				}
				kfree(rocel);
			}
		} else if (cm->rdma_type == _rdma_lb)
			free_lb_listener(cm);
	}
	else {
		if (cm->rdma_type == _rdma_ib) {
			ibc = cm_2_ibc(cm);
			if (ibc) {
				if (ibc->conn.listener) {
					spin_lock_irqsave(&ibc->conn.listener->connection_guard, flags);
					list_del(&ibc->conn.link);
					spin_unlock_irqrestore(&ibc->conn.listener->connection_guard,
						flags);
				}
				start = jiffies;
				_NT(trace_3_nvmeib_rdma_free_cm, "ib_destroy_cm_id cm=@CM", cm);
				ib_destroy_cm_id(ibc->cm_id);
				_NT(trace_4_nvmeib_rdma_free_cm, "end of ib_destroy_cm_id cm=@CM time=@LLD", cm,
						jiffies - start);
				ibc->cm_id = NULL;
				kfree(ibc);
			}
		}
		else if (cm->rdma_type == _rdma_roce || cm->rdma_type == _rdma_iwarp) {
			rocec = cm_2_rocec(cm);
			if (rocec) {
				if (rocec->conn.listener) {
					spin_lock_irqsave(&rocec->conn.listener->connection_guard,
						flags);
					list_del(&rocec->conn.link);
					spin_unlock_irqrestore(&rocec->conn.listener->connection_guard,
						flags);
				}
				if (rocec->find_path_sock_cep) {
					struct find_path_sock_cep *cep = rocec->find_path_sock_cep;

					BUG_ON(cm->rdma_type != _rdma_iwarp);
					rocec->find_path_sock_cep = NULL;

					find_path_cep_set_inuse(cep);
					cep->conn = NULL;

					if (cep->state == FIND_PATH_EPSTATE_IDLE ||
						cep->state == FIND_PATH_EPSTATE_CONNECT_ERROR)
					{
						/* TCP_CLOSE event never happened,
						 * do the find_path_sock_work_handler free_cep step here */
						find_path_sock_work_handler_free_cep(cep, "free-cm-free-cep");
					}

					find_path_cep_set_free(cep);

					find_path_cep_put(cep, "conn-cep-ptr");
				}
				else if (rocec->cm_id) {
					if (cm->rdma_type == _rdma_iwarp) {
						enum roce_conn_state prev_state;
						/* [NVMESH-4168]: Lock out the handler and set the state to destroying.
						 * Prevents the handler running while we destroy the iw_cm_id */
						mutex_lock(&rocec->handler_guard);
						spin_lock_irqsave(&rocec->conn.lock, flags);
						prev_state = rocec->state;
						rocec->state = NVMEIB_ROCE_CM_DESTROYING;
						spin_unlock_irqrestore(&rocec->conn.lock, flags);
						mutex_unlock(&rocec->handler_guard);

						_NT(trace_5_nvmeib_rdma_free_cm,
						    "Calling iw_destroy_cm_id for cm=@CM in state @STATE (cm_id=@CM_ID)",
						    cm, prev_state, rocec->iw_cm_id);
						iw_destroy_cm_id(rocec->iw_cm_id);
					} else {
						rdma_destroy_id(rocec->cm_id);
					}
					rocec->cm_id = NULL;
				}
			}
			kfree(rocec);
		} else if (cm->rdma_type == _rdma_lb ||
			cm->rdma_type == _rdma_lb_accept) {
			lb_destroy_id(cm);
		}
	}
	NFOUT;
}

/* create roce connection */
static struct roce_rdma_connection *roce_create_connection(
	struct roce_rdma_listener *roce, void *id)
{
	struct roce_rdma_connection *conn = kzalloc(sizeof(*conn), GFP_KERNEL);

	NFIN;
	if (conn) {
		if (roce->listener.cm.rdma_type == _rdma_iwarp)
			conn->iw_cm_id = id;
		else
			conn->cm_id = id;
		conn->conn.cm.rdma_type = roce->listener.cm.rdma_type;
		conn->conn.cm.cm_type = _cm_connection;
		conn->conn.listener = &roce->listener;
		mutex_init(&conn->qp_guard);
		spin_lock_init(&conn->conn.lock);
		init_completion(&conn->done);
		mutex_init(&conn->handler_guard);
	}
	NFOUT;
	return conn;
}

/**
 * find connection in the list of managed connection
 *  */
static struct rdma_connection *roce_find_rdma_cm(struct rdma_listener *listener,
	void *cm_id)
{
	struct rdma_connection *conn;
	struct roce_rdma_connection *r_conn;
	struct roce_rdma_listener *r_l = l_2_roce(listener);
	bool found = false;
	unsigned long flags;

	NFIN;
	BUG_ON(!r_l);
	spin_lock_irqsave(&listener->connection_guard, flags);
	list_for_each_entry(conn, &listener->connections, link) {
		if (!(r_conn = conn_2_roce(conn)))
			continue;
		if (listener->cm.rdma_type == _rdma_iwarp) {
			BUG_ON(conn->cm.rdma_type != _rdma_iwarp);
			if (r_conn->iw_cm_id == cm_id) {
				found = true;
				break;
			}
		} else {
			BUG_ON(listener->cm.rdma_type != _rdma_roce);
			BUG_ON(conn->cm.rdma_type != _rdma_roce);
			if (r_conn->cm_id == cm_id) {
				found = true;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&listener->connection_guard, flags);
	if (!found)
		conn = NULL;
	NFOUT;
	return conn;
}

static void roce_event_2_nvmeib_event(struct rdma_cm_event *event,
	struct nvmeib_rdma_event *out_event)
{
	NFIN;
	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		_ND(roce_event_2_nvmeib_event_d1, "RDMA_CM_EVENT_ADDR_RESOLVED");
		out_event->event = NVMEIB_ADDR_RESOLVED;
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
		_NT(roce_event_2_nvmeib_event_t1, "RDMA_CM_EVENT_ADDR_ERROR");
		out_event->event = NVMEIB_CONNECT_ERROR;
		out_event->status = -ENETUNREACH;
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		_ND(roce_event_2_nvmeib_event_d2, "RDMA_CM_EVENT_ROUTE_RESOLVED");
		out_event->event = NVMEIB_ROUTE_RESOLVED;
		break;
	case RDMA_CM_EVENT_ROUTE_ERROR:
		_NT(roce_event_2_nvmeib_event_t2, "RDMA_CM_EVENT_ROUTE_ERROR");
		out_event->event = NVMEIB_CONNECT_ERROR;
		out_event->status = -ENETUNREACH;
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		_ND(roce_event_2_nvmeib_event_d3, "RDMA_CM_EVENT_CONNECT_REQUEST");
		out_event->event = NVMEIB_REQ_RECEIVED;
		break;
	case RDMA_CM_EVENT_CONNECT_RESPONSE:
		_ND(roce_event_2_nvmeib_event_d4, "RDMA_CM_EVENT_CONNECT_RESPONSE");
		out_event->event = NVMEIB_USER_ESTABLISHED;
		break;
	case RDMA_CM_EVENT_CONNECT_ERROR:
		_NT(roce_event_2_nvmeib_event_t3, "IB_CM_REQ_ERROR");
		out_event->event = NVMEIB_CONNECT_ERROR;
		out_event->status = -ECONNABORTED;
		break;
	case RDMA_CM_EVENT_UNREACHABLE:
		_NT(roce_event_2_nvmeib_event_t4, "RDMA_CM_EVENT_UNREACHABLE");
		out_event->event = NVMEIB_REP_ERROR;
		out_event->status = -EHOSTUNREACH;
		break;
	case RDMA_CM_EVENT_REJECTED:
		_NT(roce_event_2_nvmeib_event_t5, "RDMA_CM_EVENT_REJECTED");
		out_event->event = NVMEIB_REJ_RECEIVED;
		out_event->status = event->status;
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		_NT(roce_event_2_nvmeib_event_t6, "RDMA_CM_EVENT_ESTABLISHED");
		out_event->event = NVMEIB_USER_ESTABLISHED;
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		_NT(roce_event_2_nvmeib_event_t7, "RDMA_CM_EVENT_DISCONNECTED");
		out_event->event = NVMEIB_DREQ_RECEIVED;
		break;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		_NT(roce_event_2_nvmeib_event_t8, "RDMA_CM_EVENT_DEVICE_REMOVAL");
		out_event->event = NVMEIB_DEVICE_REMOVED;
		break;
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		_NT(roce_event_2_nvmeib_event_t9, "RDMA_CM_EVENT_TIMEWAIT_EXIT");
		out_event->event = NVMEIB_TIMEWAIT_EXIT;
		break;
	default:
		_NW(roce_event_2_nvmeib_event_w1, "unsupported ib event @INT", event->event);
		break;
	}
	NFOUT;
}

/* roce handler, called when a roce event occurs */
static int roce_cm_handler_impl(struct rdma_cm_id *cm_id,
	struct rdma_cm_event *event)
{
	struct roce_rdma_listener *roce = cm_id->context;
	int rv = 0;
	struct nvmeib_rdma_event rdma_event;
	struct nvmeib_rdma_conn_params params = {0};
	struct roce_rdma_connection *roce_conn;
	struct rdma_connection *conn;
	unsigned long flags;

	NFIN;
	_NT(trace_nvmeib_rdma_roce_cm_handler_impl, "nvmes_roce_handler got event @EVENT on cm @CM_ID", event->event, cm_id);
	 /* a client tries to connect */
	if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
		if ((roce_conn = roce_create_connection(roce, cm_id))) {
			params.d = cm_id->device;
			params.port = cm_id->port_num;
			memcpy(params.private_data, event->param.conn.private_data,
				event->param.conn.private_data_len);
			spin_lock_irqsave(&roce->listener.connection_guard, flags);
			list_add_tail(&roce_conn->conn.link,
				&roce->listener.connections);
			spin_unlock_irqrestore(&roce->listener.connection_guard, flags);
			if ((rv = roce->listener.new_connection(
				&roce_conn->conn.cm, &params)) < 0) {
				_NT(error_nvmeib_rdma_roce_cm_handler_impl, "new connection() failed");
				spin_lock_irqsave(&roce->listener.connection_guard, flags);
				list_del_init(&roce_conn->conn.link);
				spin_unlock_irqrestore(&roce->listener.connection_guard, flags);
				goto free_conn;
			}
			else {
				_ND(trace_1_nvmeib_rdma_roce_cm_handler_impl, "Accepeted a new connection");
			}
		}
		else
			rv = -ENOMEM;
	}
	else {
		roce_event_2_nvmeib_event(event, &rdma_event);
		/*
		   this is not a new connection so probably the new event is due to
		   a change in a client qp.
		*/
		if (cm_id != roce->cm_id) {
			if ((conn = roce_find_rdma_cm(&roce->listener, cm_id))) {
				roce_conn = conn_2_roce(conn);
				roce->listener.on_peer_event(&roce_conn->conn.cm, &rdma_event);
			}
			else
				_NE(error_1_nvmeib_rdma_roce_cm_handler_impl, "Received CM event @EVENT but connection is not ours",
					event->event);
		}
		else
			/* this is wrong.  we received an event on a listener cm */
			_NE(error_2_nvmeib_rdma_roce_cm_handler_impl, "Listener received @EVENT event on cm @CM_ID- ignoring", event->event,
				cm_id);
		rv = 0;
	}
	goto out;

free_conn:
	kfree(roce_conn);
out:
	NFOUT;
	return rv;

}

/* initialize listener to roce interface.
   general listener on all roce devices
*/
static int start_roce_listener(struct nvmeib_rdma_cm *cm)
{
	struct rdma_cm_id *cm_id;
	struct rdma_listener *listener = NULL;
	int port;
	struct roce_rdma_listener *roce_listener = NULL;
	int ret;

	NFIN;
	listener = cm_2_l(cm);
	if (!listener) {
		_NE(error_nvmeib_rdma_start_roce_listener, "Invalid rdma_listener @CM", cm);
		ret = -EINVAL;
		goto out;
	}
	if (!(roce_listener = l_2_roce(listener))) {
		_NE(error_1_nvmeib_rdma_start_roce_listener, "Invalid roce_listener @CM", cm);
		ret = -EINVAL;
		goto out;
	}
	port = roce_listener->port;

	cm_id = rdma_create_id(roce_cm_handler_impl, roce_listener,
		RDMA_PS_TCP, IB_QPT_RC);

	if (cm_id == NULL) {
		_NE(error_2_nvmeib_rdma_start_roce_listener, "rdma_create_id() failed");
		ret = -1;
		goto out;
	}
	else
		_ND(trace_nvmeib_rdma_start_roce_listener, "rdma_create_id returned @CM_ID", cm_id);

	if (!roce_listener->ipv4_only) {
		struct sockaddr_in6 sock6 = {
            .sin6_family = AF_INET6,
            .sin6_port = htons(port),
            .sin6_addr = IN6ADDR_ANY_INIT,
        };
		ret = rdma_bind_addr(cm_id, (struct sockaddr *)&sock6);
	} else {
		struct sockaddr_in sock = {0};
		sock.sin_family = AF_INET;
		sock.sin_port = htons(port);
		sock.sin_addr.s_addr = INADDR_ANY;
		ret = rdma_bind_addr(cm_id, (struct sockaddr *)&sock);
	}
	_ND(trace_1_nvmeib_rdma_start_roce_listener, "rdma_bind_addr returned @RET_INT", ret);
	if (ret < 0) {
		_NE(error_3_nvmeib_rdma_start_roce_listener, "rdma_bind_addr() failed: @RET_INT", ret);
		goto out_id;
	}

	ret = rdma_listen(cm_id, 999);
	_ND(trace_2_nvmeib_rdma_start_roce_listener, "rdma listen returned @RET_INT", ret);
	if (ret) {
		_NE(error_4_nvmeib_rdma_start_roce_listener, "rdma_listen() failed: @RET_INT", ret);
		goto out_id;
	}

	roce_listener->cm_id = cm_id;
	_ND(trace_3_nvmeib_rdma_start_roce_listener, "init roce returned successfully");
	goto out;

out_id:
	_ND(trace_4_nvmeib_rdma_start_roce_listener, "roce initi died!!");
	rdma_destroy_id(cm_id);

out:
	NFOUT;
	return ret;
}

static void iw_event_2_nvmeib_event(struct iw_cm_event *event,
	struct nvmeib_rdma_event *out_event)
{
	NFIN;
	switch (event->event) {
	case IW_CM_EVENT_CONNECT_REQUEST:
		_ND(__AUTOID__, "IW_CM_EVENT_CONNECT_REQUEST\n");
		out_event->event = NVMEIB_REQ_RECEIVED;
		BUG_ON(event->status != 0);
		break;
	case IW_CM_EVENT_CONNECT_REPLY:
		_ND(__AUTOID__, "IW_CM_EVENT_CONNECT_REPLY\n");
		if (event->status == 0)
			out_event->event = NVMEIB_USER_ESTABLISHED;
		else {
			out_event->event = NVMEIB_REP_ERROR;
			out_event->status = event->status;
		}
		break;
	case IW_CM_EVENT_ESTABLISHED:
		_ND(__AUTOID__, "IW_CM_EVENT_ESTABLISHED\n");
		out_event->event = NVMEIB_USER_ESTABLISHED;
		BUG_ON(event->status != 0);
		break;
	case IW_CM_EVENT_DISCONNECT:
		_ND(__AUTOID__, "IW_CM_EVENT_DISCONNECT\n");
		out_event->event = NVMEIB_DREQ_RECEIVED;
		break;
	case IW_CM_EVENT_CLOSE:
		_ND(__AUTOID__, "IW_CM_EVENT_CLOSE\n");
		out_event->event = NVMEIB_DREP_RECEIVED;
		break;
	default:
		_NW(t_04_brdma, "unsupported iw event @INT\n", event->event);
		break;
	}
	NFOUT;
}

/* Prepare close event structure */
static void iw_prepare_event(struct nvmeib_rdma_event *rdma_event,
				    enum nvmeib_rdma_event_type event_type,
				    int status, void *private_data, u8 private_data_len)
{
	rdma_event->event = event_type;
	rdma_event->status = status;
	rdma_event->private_data = private_data;
	rdma_event->private_data_len = private_data_len;
}

static void iw_send_close_events_with_handler(int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
					       void *context, int status)
{
	struct nvmeib_rdma_event rdma_event = {0};

	NFIN;
	iw_prepare_event(&rdma_event, NVMEIB_DREQ_RECEIVED, status, NULL, 0);
	_ND(trace_iw_send_close_events_dreq, "Sending nvmeib_rdma event @EVENT for IW_CM_EVENT_CLOSE",
		rdma_event.event);
	event_handler(context, &rdma_event);

	iw_prepare_event(&rdma_event, NVMEIB_DREP_RECEIVED, status, NULL, 0);
	_ND(trace_iw_send_close_events_drep, "Sending nvmeib_rdma event @EVENT for IW_CM_EVENT_CLOSE",
		rdma_event.event);
	event_handler(context, &rdma_event);
	NFOUT;
}

static void iw_send_close_events_with_peer_event(int (*on_peer_event)(struct nvmeib_rdma_cm *cm, struct nvmeib_rdma_event *p),
						   struct nvmeib_rdma_cm *cm, int status)
{
	struct nvmeib_rdma_event rdma_event = {0};

	NFIN;

	iw_prepare_event(&rdma_event, NVMEIB_DREQ_RECEIVED, status, NULL, 0);
	_ND(trace_iw_send_close_events_dreqx, "Sending nvmeib_rdma event @EVENT for IW_CM_EVENT_CLOSE",
		rdma_event.event);
	on_peer_event(cm, &rdma_event);

	iw_prepare_event(&rdma_event, NVMEIB_DREP_RECEIVED, status, NULL, 0);
	_ND(trace_iw_send_close_events_drepx, "Sending nvmeib_rdma event @EVENT for IW_CM_EVENT_CLOSE",
		rdma_event.event);
	on_peer_event(cm, &rdma_event);
	NFOUT;
}

/* iwarp handler, called when an iwarp event occurs */
static int iwarp_cm_handler_impl(struct iw_cm_id *cm_id,
	struct iw_cm_event *event)
{
	struct roce_rdma_listener *roce = cm_id->context;
	int rv = 0;
	struct nvmeib_rdma_event rdma_event;
	struct nvmeib_rdma_conn_params params = {0};
	struct roce_rdma_connection *roce_conn;
	struct rdma_connection *conn;
	unsigned long flags;

	NFIN;
	_NT(trace_nvmeib_rdma_iw_cm_handler_impl, "iwarp_cm_handler_impl got event @EVENT on cm @CM_ID", event->event, cm_id);
	 /* a client tries to connect */
	if (event->event == IW_CM_EVENT_CONNECT_REQUEST) {
		struct iw_cm_pd *iw_pd = event->private_data;
		size_t user_pd_len = event->private_data_len - offsetof(struct iw_cm_pd, user_pd);
		if (iw_pd->hdr.req.qpn == find_path_qpn) {
			struct iw_cm_pd_hdr iw_rej_pd = {
				.rej = {
					.flags = IW_CM_REJ_FIND_PATH,
					.reason = 0,
				},
			};
			/* Find-path request - Reject to send response */
			iw_cm_reject(cm_id, &iw_rej_pd, sizeof(iw_rej_pd));
			goto out;
		}
		if ((roce_conn = roce_create_connection(roce, cm_id))) {
			params.d = cm_id->device;
			params.port = 1; // TODO FIX ASAP
			roce_conn->conn.remote_qpn = be32_to_cpu(iw_pd->hdr.req.qpn);
			memcpy(params.private_data, iw_pd->user_pd, user_pd_len);
			spin_lock_irqsave(&roce->listener.connection_guard, flags);
			list_add_tail(&roce_conn->conn.link,
				&roce->listener.connections);
			spin_unlock_irqrestore(&roce->listener.connection_guard, flags);
			if ((rv = roce->listener.new_connection(
				&roce_conn->conn.cm, &params)) < 0) {
				_NE(error_nvmeib_rdma_iwarp_cm_handler_impl, "new connection() failed");
				spin_lock_irqsave(&roce->listener.connection_guard, flags);
				list_del_init(&roce_conn->conn.link);
				spin_unlock_irqrestore(&roce->listener.connection_guard, flags);
				goto free_conn;
			}
			else {
				_ND(trace_1_nvmeib_rdma_iwarp_cm_handler_impl, "Accepeted a new connection");
			}
		}
		else
			rv = -ENOMEM;
	}
	else {
		/*
		   this is not a new connection so probably the new event is due to
		   a change in a client qp.
		*/
		if (cm_id != roce->iw_cm_id) {
			if ((conn = roce_find_rdma_cm(&roce->listener, cm_id))) {
				roce_conn = conn_2_roce(conn);
				/* For IW_CM_EVENT_CLOSE, send both DREQ and DREP events as when 
				   IWCM doesn't pass disconnect event to the cm handler */
				if (event->event == IW_CM_EVENT_CLOSE) {
					iw_send_close_events_with_peer_event(roce->listener.on_peer_event,
									      &roce_conn->conn.cm,
									      event->status);
				} else {
					iw_event_2_nvmeib_event(event, &rdma_event);
					roce->listener.on_peer_event(&roce_conn->conn.cm, &rdma_event);
				}
			}
			else
				_NE(error_1_nvmeib_rdma_iwarp_cm_handler_impl, "Received CM event @EVENT but connection is not ours",
					event->event);
		}
		else
			/* this is wrong.  we received an event on a listener cm */
			_NE(error_2_nvmeib_rdma_iwarp_cm_handler_impl, "Listener received @EVENT event on cm @CM_ID- ignoring", event->event,
				cm_id);
		rv = 0;
	}
	goto out;

free_conn:
	kfree(roce_conn);
out:
	NFOUT;
	return rv;
}

static void iw_find_path_sock_put_work(struct find_path_sock_work *work, const char *put_desc)
{
	struct find_path_sock_cep *cep = work->cep;

	work->work_type = FIND_PATH_WORK_TYPE_NONE;

	spin_lock_bh(&cep->lock);
	list_add_tail(&work->link, &cep->work_free_list);
	spin_unlock_bh(&cep->lock);

	/* Matched with kref_get in iw_find_path_sock_queue_work */
	find_path_cep_put(cep, put_desc);
}

static int iw_find_path_sock_queue_work(struct find_path_sock_cep *cep,
					enum find_path_sock_work_type work_type)
{
	struct find_path_sock_dev *cep_dev = cep->parent;
	struct find_path_sock_work *work;
	unsigned long flags;
	int rv;

	NFIN;
	if (!cep_dev) {
		_NT(trace_iw_find_path_sock_queue_work_no_parent,
		    "conn endpoint @PTR has NULL parent, not queueing work", cep);
		rv = -EINVAL;
		goto out;
	}

	spin_lock_irqsave(&cep->lock, flags);
	if (!(work = list_first_entry_or_null(
		&cep->work_free_list, typeof(*work), link)))
	{
		_NT(trace_iw_find_path_sock_queue_work_no_work,
		    "Work pool empty on connection endpoint @PTR", cep);
		spin_unlock_irqrestore(&cep->lock, flags);
		rv = -ENOENT;
		goto out;
	}

	list_del_init(&work->link);
	work->work_type = work_type;

	spin_unlock_irqrestore(&cep->lock, flags);

	find_path_cep_get(cep, "queue-work");

	if (!wq_add_work(cep_dev->find_path_sock_wq, &work->work)) {
		_NT(trace_iwarp_find_path_sock_queue_work_queue_fail,
		    "Failed to queue work");

		iw_find_path_sock_put_work(work, "queue-work-fail");

		rv = -EBUSY;
		goto out;
	}

	rv = 0;
	goto out;

out:
	NFOUT;
	return rv;
}

static void iw_find_path_sock_state_change(struct sock *sk)
{
	struct find_path_sock_cep *cep;
	void (*orig_sk_state_change)(struct sock *sk);

	NFIN;
	read_lock(&sk->sk_callback_lock);
	cep = sk->sk_user_data;
	if (!cep) {
		/* Listener was destroyed under our feet */
		_NT(trace_iw_find_path_sock_state_change_null_data,
		    "Endpoint destroyed under our feet");
		read_unlock(&sk->sk_callback_lock);
		goto out;
	}
	orig_sk_state_change = cep->orig_sk_state_change;

	_ND(trace_iw_find_path_sock_state_change,
	    "New TCP State @INT on endpoint @PTR in state @STATE",
		sk->sk_state, cep, cep->state);

	switch (sk->sk_state) {
	case TCP_ESTABLISHED:
		if (cep->state == FIND_PATH_EPSTATE_LISTENING) {
			_NT(trace_iw_find_path_sock_state_change_accept_new_conn,
				"New connection on listener endpoint @PTR iwarp listener @CM_ID", cep, cep->listener);
			iw_find_path_sock_queue_work(cep, FIND_PATH_WORK_TYPE_ACCEPT);
		} else if (cep->state == FIND_PATH_EPSTATE_CONNECTING) {
			_NT(trace_iw_find_path_sock_state_change_conn,
			    "Connected connection endpoint @PTR on iwarp connection @CM_ID", cep, cep->conn);
			iw_find_path_sock_queue_work(cep, FIND_PATH_WORK_TYPE_CONNECT);
		} else {
			_ND(trace_iw_find_path_sock_state_change_est_unk_state,
			    "Ignoring TCP state @STATE on connection endpoint @PTR in state @STATE",
				sk->sk_state, cep, cep->state);
		}
		break;
	case TCP_CLOSE_WAIT:
	case TCP_CLOSE:
		if (cep->state == FIND_PATH_EPSTATE_LISTENING) {
			/* Other side sent a FIN before we managed to accept.
			 * Let later socket operations(accept/send/rcv) to handle the it the right way
			 * No reason to close the listening socket itself (FIND_PATH_WORK_TYPE_RELEASE)
			 */
			_ND(trace_iw_find_path_sock_listener_state_change_close_listen,
			    "Ignoring TCP state @STATE on listener endpoint @PTR of iwarp listener @CM_ID",
				sk->sk_state, cep, cep->listener);
		} else {
			_NT(trace_iwarp_find_path_listener_conn_state_change_close,
				"connection endpoint @PTR of iwarp conn/listener @CM_ID in state @STATE closing",
				cep, cep->conn, cep->state);
			iw_find_path_sock_queue_work(cep, FIND_PATH_WORK_TYPE_RELEASE);
		}
		break;
	default:
		_ND(trace_iw_find_path_sock_listener_state_change_unexp_state_change,
			"Ignoring new TCP State @STATE on endpoint @PTR of iwarp conn/listener @CM_ID",
			sk->sk_state, cep, cep->conn);
		break;
	}

	read_unlock(&sk->sk_callback_lock);
	if (orig_sk_state_change)
		(*orig_sk_state_change)(sk);

out:
	NFOUT;
}

static SK_DATA_READY_FN_SIG(iw_find_path_sock_data_ready)
{
	struct find_path_sock_cep *cep;
	SK_DATA_READY_FN_SIG((*orig_sk_data_ready));
	int bytes_ready = atomic_read(&sk->sk_rmem_alloc);

	NFIN;
	read_lock(&sk->sk_callback_lock);
	cep = sk->sk_user_data;
	if (!cep) {
		/* Listener was destroyed under our feet */
		_NT(trace_iwarp_sock_data_ready_null_data,
		    "Endpoint destroyed under our feet");
		read_unlock(&sk->sk_callback_lock);
		goto out;
	}
	orig_sk_data_ready = cep->orig_sk_data_ready;

	_ND(trace_iw_find_path_sock_data_ready,
	    "@BYTES of data ready on endpoint @PTR in state @STATE",
		bytes_ready, cep, cep->state);

	switch (cep->state) {
	case FIND_PATH_EPSTATE_LISTENING:
		break;
	case FIND_PATH_EPSTATE_CONNECTING:
	case FIND_PATH_EPSTATE_CONNECTED:
		if (cep->initiator) {
			/* Initiator side */
			iw_find_path_sock_queue_work(cep, FIND_PATH_WORK_TYPE_READ_RESP);
		} else {
			/* Add work here if acceptor side expects data */
		}
		break;
	default:
		_ND(trace_iw_find_path_sock_data_ready_inv_state,
		    "Unexpected data received on conn endpoint @PTR in invalid state @STATE "
		    "of iwarp conn/listener @CM_ID", cep, cep->state, cep->conn);
	}

	read_unlock(&sk->sk_callback_lock);

	if (orig_sk_data_ready) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
		(*orig_sk_data_ready)(sk, bytes);
#else
		(*orig_sk_data_ready)(sk);
#endif
	}

out:
	NFOUT;
}

static void find_path_sock_work_handler(struct workqe_struct *_work);

static void find_path_cep_free_work(struct find_path_sock_cep *cep)
{
	struct find_path_sock_work *work;

	while ((work = list_first_entry_or_null(
		&cep->work_free_list, typeof(*work), link)))
	{
		list_del(&work->link);
		kfree(work);
	}
}

static int find_path_cep_alloc_work(struct find_path_sock_cep *cep, int num_works)
{
	struct find_path_sock_work *work;
	int rv, i;

	NFIN;
	for (i = 0; i < num_works; i++) {
		if (!(work = kzalloc(sizeof(*work), GFP_KERNEL))) {
			_NT(trace_find_path_cep_alloc_works_alloc_fail, "OOM");
			rv = -ENOMEM;
			goto free_works;
		}
		WQ_INIT_WORK(&work->work, find_path_sock_work_handler);
		work->cep = cep;
		list_add_tail(&work->link, &cep->work_free_list);
	}

	rv = 0;
	goto out;

free_works:
	find_path_cep_free_work(cep);

out:
	NFOUT;
	return rv;
}

static int iwarp_find_path_sock_send_resp(struct find_path_sock_cep *cep)
{
	struct ib_device *iw_dev = cep->listener->listener.dev->ib_dev;
	struct net_device *iw_ndev = NULL;
	union nvmeib_version this_version = nvmeib_version_get();
	struct find_path_prot_resp resp = {};
	struct msghdr msg = {};
	struct kvec iov[1];
	int rv;

	NFIN;
	if (!iw_dev->get_netdev ||
		!(iw_ndev = (*iw_dev->get_netdev)(iw_dev, 1)))
	{
		_NT(trace_iwarp_find_path_sock_send_resp_no_ndev,
		    "Failed to get netdev of iwarp device @IB_DEVICE",
			iw_dev->name);
	} else {
		memcpy(resp.netdev_name, iw_ndev->name, IFNAMSIZ);
		dev_put(iw_ndev);
	}

	memcpy(resp.key, find_path_prot_key, sizeof(resp.key));
	resp.len = cpu_to_be32(sizeof(resp));
	resp.flags = cpu_to_be32(FIND_PATH_PROT_FLAG_RESP);
	resp.version = cpu_to_be64(this_version.all);
	resp.commit = cpu_to_be64((u64)COMMIT_ID);
	resp.iw_primary_port = cpu_to_be16(cep->listener->port);
	resp.iw_2nd_base_port = cpu_to_be16(cep->listener->iw_2nd_base_port);
	resp.iw_2nd_num_ports = cpu_to_be16(cep->listener->iw_2nd_num_ports);
	resp.crc16 = crc16(0, (const u8 *)&resp, sizeof(resp));

	iov[0].iov_base = &resp;
	iov[0].iov_len = sizeof(resp);

	rv = kernel_sendmsg(cep->s, &msg, iov, sizeof(iov), iov[0].iov_len);

	NFOUT;
	return rv;
}

static void find_path_cep_free(struct kref *ref)
{
	struct find_path_sock_cep *cep = container_of(ref, typeof(*cep), ref);
	struct find_path_sock_dev *dev = cep->parent;

	_NT(trace_find_path_cep_free, "Freeing conn endpoint @PTR", cep);

	/* Remove the dev reference */
	cep->parent = NULL;
	find_path_dev_put(dev, "free-cep");

	find_path_cep_free_work(cep);
	kfree(cep->resp);
	kfree(cep);
}

static TIMER_CALLBACK_DECL(find_path_shutdown_timer_fn);

static struct find_path_sock_cep *find_path_sock_accept_newconn(struct find_path_sock_cep *listen_cep)
{
	struct find_path_sock_dev *dev = listen_cep->parent;
	struct find_path_sock_cep *new_cep = NULL;
	char sock_addr_str[40];
	int rv;

	if (listen_cep->state != FIND_PATH_EPSTATE_LISTENING) {
		_NT(trace_find_path_sock_accept_newconn_listen_down,
		    "iwarp listener @CM_ID is going down", listen_cep->listener);
		goto out;
	}

	if (!nvmeib_ref_get(&dev->ref_conn)) {
		_NT(trace_find_path_sock_accept_newconn_dev_down,
		    "device @IB_DEVICE is going down", dev->nvdev->ib_dev->name);
		goto out;
	}

	if (!(new_cep = kzalloc(sizeof(*new_cep), GFP_KERNEL))) {
		_NT(trace_find_path_sock_accept_newconn_cep_alloc_fail, "OOM");
		goto dec_ref;
	}

	kref_init(&new_cep->ref);
	spin_lock_init(&new_cep->lock);
	init_waitqueue_head(&new_cep->waitq);
	INIT_LIST_HEAD(&new_cep->work_free_list);

	INIT_TIMER(&new_cep->sock_shutdown_timer);
	new_cep->sock_shutdown_timer.function = find_path_shutdown_timer_fn;
	TIMER_SET_DATA(new_cep, sock_shutdown_timer, (unsigned long)new_cep);

	if ((rv = find_path_cep_alloc_work(new_cep, FIND_PATH_SOCK_CONN_MAX_WORK)) < 0) {
		_NT(trace_find_path_sock_accept_newconn_cep_work_alloc_fail, "OOM");
		goto free_cep;
	}

	if ((rv = kernel_accept(listen_cep->s, &new_cep->s, O_NONBLOCK)) < 0) {
		if (rv == -EAGAIN || rv == -EWOULDBLOCK) {
			_NT(trace_find_path_sock_accept_newconn_accept_would_block,
				"no connection pending on iwarp listener @CM_ID", listen_cep->listener);
		} else {
			_NT(trace_find_path_sock_accept_newconn_accept_fail,
				"kernel_accept failed (@RV) failed on iwarp listener @CM_ID", rv, listen_cep->listener);
		}
		goto free_cep;
	}

#if KS_HAS_SOCK_NOT_OWNED_BY_ME
	{
		struct sock 	*sk = new_cep->s->sk;
		struct net      *net = sock_net(sk);

		/* Prevent inet_csk_clear_xmit_timers_sync being called from tcp_close()
		 * Fixes [NVMESH-5532] */
		sk->sk_net_refcnt = 1;
		get_net(net);
	}
#endif

	/* parent back-pointer reference */
	find_path_dev_get(dev, "cep-back-ptr");
	new_cep->parent = dev;

	new_cep->state = FIND_PATH_EPSTATE_CONNECTED;
	new_cep->listener = listen_cep->listener;

	new_cep->orig_sk_state_change = listen_cep->orig_sk_state_change;
	new_cep->orig_sk_data_ready = listen_cep->orig_sk_data_ready;

	/* sk_user_data reference */
	find_path_cep_get(new_cep, "sk_user_data");
	new_cep->s->sk->sk_user_data = new_cep;

	/* Add to device list (We must first unlock the listen_cep or we risk a deadlock) */
	find_path_cep_set_free(listen_cep);

	find_path_cep_get(new_cep, "dev-cep-list");
	mutex_lock(&dev->lock);
	list_add_tail(&new_cep->link, &dev->sock_cep_list);
	mutex_unlock(&dev->lock);

	find_path_cep_set_inuse(listen_cep);

	if (new_cep->s->sk->sk_family == AF_INET) {
		scnprintf(sock_addr_str, sizeof(sock_addr_str), "%pI4", &new_cep->s->sk->sk_daddr);
	} else {
		BUG_ON(new_cep->s->sk->sk_family != AF_INET6);
		scnprintf(sock_addr_str, sizeof(sock_addr_str), "%pI6c", &new_cep->s->sk->sk_v6_daddr);
	}

	_NT(trace_find_path_sock_accept_newconn_accept_conn,
		"Accepted connection from @ADDR_STR on "
		" iwarp listener @CM_ID with new conn endpoint @PTR",
		sock_addr_str, new_cep->listener, new_cep);

	goto out;

free_cep:
	find_path_cep_put(new_cep, "accept-fail");
	new_cep = NULL;

dec_ref:
	nvmeib_ref_put(&dev->ref_conn);

out:
	return new_cep;
}

static int find_path_sock_recv_resp(struct find_path_sock_cep *cep)
{
	int bytes_rem;
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_flags = MSG_DONTWAIT,
	};
	struct kvec iov = {};
	int rv;

	NFIN;
	if (!cep->resp) {
		if (!(cep->resp = kzalloc(sizeof(*cep->resp), GFP_KERNEL))) {
			_NT(trace_find_path_work_handler_resp_alloc_fail, "OOM");
			rv = -ENOMEM;
			goto out;
		}
		cep->resp_recv = 0;
	}
	bytes_rem = sizeof(*cep->resp) - cep->resp_recv;
	iov.iov_base = (void *)cep->resp + cep->resp_recv;
	iov.iov_len = bytes_rem;
	if ((rv = kernel_recvmsg(cep->s, &msg, &iov, 1, bytes_rem, MSG_DONTWAIT)) < 0) {
		_NT(trace_find_path_work_handler_recv_fail, "kernel_recvmsg failed (@RV)", rv);
		goto out;
	}
	BUG_ON(rv > bytes_rem);
	cep->resp_recv += rv;

	_ND(trace_find_path_sock_recv_resp,
	    "conn endpoint @PTR - received @N_BYTES/@N_BYTES of response",
		cep, cep->resp_recv, sizeof(*cep->resp));

	if (cep->resp_recv < sizeof(*cep->resp)) {
		rv = -EAGAIN;
		goto out;
	}

	_NT(trace_find_path_sock_recv_resp_complete,
	    "conn endpoint @PTR - full response received", cep);

	rv = 0;
out:
	NFOUT;
	return rv;
}

static int find_path_sock_proc_resp(struct find_path_sock_cep *cep,
				    union nvmeib_version *target_version, u64 *target_commit,
				    int *iw_primary_port, int *iw_2nd_base_port, int *iw_2nd_num_ports)
{
	u16 resp_crc;
	int rv;

	NFIN;
	if (memcmp(cep->resp->key, find_path_prot_key, sizeof(cep->resp->key)) != 0)
	{
		_NT(trace_find_path_work_handler_recv_inv_key,
			"iwarp conn @CM_ID conn endpoint @PTR got invalid response key @HEX",
			cep->conn, cep, cep->resp->key);
		rv = -EPROTO;
		goto out;
	}

	if (be32_to_cpu(cep->resp->len) != sizeof(*cep->resp))
	{
		_NT(trace_find_path_work_handler_recv_inv_len,
			"iwarp conn @CM_ID conn endpoint @PTR got invalid response length @LENGTH",
			cep->conn, cep, be32_to_cpu(cep->resp->len));
		rv = -EPROTO;
		goto out;
	}

	if (!(be32_to_cpu(cep->resp->flags) & FIND_PATH_PROT_FLAG_RESP))
	{
		_NT(trace_find_path_work_handler_recv_inv_flags,
			"iwarp conn @CM_ID conn endpoint @PTR got invalid response flags @BITMAP",
			cep->conn, cep, cep->resp->flags);
		rv = -EPROTO;
		goto out;
	}

	/* Put aside response crc */
	resp_crc = cep->resp->crc16;
	cep->resp->crc16 = 0;
	/* Check response crc with crc16 field set to 0 */
	if (crc16(0, (const u8 *)cep->resp, sizeof(*cep->resp)) != resp_crc) {
		_NT(trace_find_path_work_handler_recv_inv_crc,
			"iwarp conn @CM_ID conn endpoint @PTR got invalid response crc",
			cep->conn, cep);
		rv = -EPROTO;
		goto out;
	}

	target_version->all = be64_to_cpu(cep->resp->version);
	*target_commit = be64_to_cpu(cep->resp->commit);
	*iw_primary_port = be16_to_cpu(cep->resp->iw_primary_port);
	*iw_2nd_base_port = be16_to_cpu(cep->resp->iw_2nd_base_port);
	*iw_2nd_num_ports = be16_to_cpu(cep->resp->iw_2nd_num_ports);
	rv = 0;

out:
	NFOUT;
	return rv;
}

/* Note: should be called with cep in-use lock taken */
static void find_path_work_handler_release_sock(struct find_path_sock_cep *cep)
{
	_NT(trace_find_path_work_handler_release_sock,
	    "Releasing socket of cep @PTR in state @FIND_PATH_CEP_STATE", cep, cep->state);

	BUG_ON(!cep->in_use);
	BUG_ON(cep->in_use_pid != current->pid);

	/* Restore callbacks */
	write_lock_bh(&cep->s->sk->sk_callback_lock);
	cep->s->sk->sk_data_ready = cep->orig_sk_data_ready;
	cep->s->sk->sk_state_change = cep->orig_sk_state_change;
	cep->s->sk->sk_user_data = NULL;
	write_unlock_bh(&cep->s->sk->sk_callback_lock);

	/* sk_user_data reference */
	find_path_cep_put(cep, "sk_user_data");

	/* Release socket */
	sock_release(cep->s);
}

/* Note: should be called with cep in-use lock taken */
static void find_path_sock_work_handler_free_cep(struct find_path_sock_cep *cep,
						 const char *cep_put_str)
{
	_NT(trace_find_path_sock_work_handler_free_cep,
	    "Freeing cep @PTR in state @FIND_PATH_CEP_STATE from @STR",
	    cep, cep->state, cep_put_str);

	BUG_ON(!cep->in_use);
	BUG_ON(cep->in_use_pid != current->pid);

	if (!list_empty(&cep->link)) {
		/* Remove from parent's list */
		mutex_lock(&cep->parent->lock);
		list_del_init(&cep->link);
		mutex_unlock(&cep->parent->lock);

		find_path_cep_put(cep, "parent-cep-list");
	}

	nvmeib_ref_put(&cep->parent->ref_conn);

	/* Matched with kref_init */
	find_path_cep_put(cep, cep_put_str);
}

static TIMER_CALLBACK(find_path_shutdown_timer_fn, struct find_path_sock_cep, sock_shutdown_timer, struct find_path_sock_cep, cep)
	_NT(trace_find_path_shutdown_timer_fn, "Timeout on shutdown for conn endpoint @PTR. Scheduling release", cep);
	iw_find_path_sock_queue_work(cep, FIND_PATH_WORK_TYPE_RELEASE);
	find_path_cep_put(cep, "shutdown-timer");
}

static void find_path_sock_shutdown(struct find_path_sock_cep *cep)
{
	_NT(trace_find_path_sock_shutdown,
	    "Shutting down the socket for conn endpoint @PTR", cep);

	/* Shut-down the socket */
	kernel_sock_shutdown(cep->s, SHUT_RDWR);

	/* [NVMESH-5267]: Schedule timer in case we don't get FIN */
	find_path_cep_get(cep, "shutdown-timer");
	mod_timer(&cep->sock_shutdown_timer, jiffies + FIND_PATH_SOCK_SHUTDOWN_TIMEOUT_SECS * HZ);
}

static void find_path_sock_work_handler(struct workqe_struct *_work)
{
	struct find_path_sock_work *work = container_of(_work, typeof(*work), work);
	struct find_path_sock_cep *cep = work->cep, *new_cep = NULL;
	bool free_cep = false;
	int rv;

	NFIN;

	find_path_cep_set_inuse(cep);

	switch (work->work_type) {
	case FIND_PATH_WORK_TYPE_CONNECT:
		if (cep->state == FIND_PATH_EPSTATE_CONNECTING)
		{
			cep->state = FIND_PATH_EPSTATE_CONNECTED;
			_NT(trace_find_path_work_handler_conn, "conn endpoint @PTR of iwarp conn @CM_ID connected", cep, cep->conn);

			if (atomic_read(&cep->s->sk->sk_rmem_alloc) > 0) {
				/* Schedule work to read the response */
				iw_find_path_sock_queue_work(cep, FIND_PATH_WORK_TYPE_READ_RESP);
			}
		} else if (cep->state != FIND_PATH_EPSTATE_CONNECTED &&
			cep->state != FIND_PATH_EPSTATE_GOT_RESPONSE)
		{
			_NT(trace_find_path_work_handler_conn_inv_state,
			    "iwarp conn/listener @CM_ID conn endpoint @PTR in invalid state @FIND_PATH_CEP_STATE",
				cep->conn, cep, cep->state);
		}
		break;
	case FIND_PATH_WORK_TYPE_ACCEPT:
		if (!(new_cep = find_path_sock_accept_newconn(cep))) {
			goto end_handler;
		}

		if ((rv = iwarp_find_path_sock_send_resp(new_cep)) < 0) {
			_NT(trace_find_path_sock_accept_newconn_send_fail,
			    "Failed to send response to new conn endpoint @PTR", cep);
			/* Shutdown the socket (should trigger a TCP_CLOSE state-change which will schedule the release work) */
			find_path_sock_shutdown(new_cep);
		}
		break;
	case FIND_PATH_WORK_TYPE_RELEASE:
		switch (cep->state) {
		case FIND_PATH_EPSTATE_LISTENING:
		case FIND_PATH_EPSTATE_CONNECTING:
		case FIND_PATH_EPSTATE_CONNECT_ERROR:
		case FIND_PATH_EPSTATE_CONNECTED:
		case FIND_PATH_EPSTATE_RELEASING:
		case FIND_PATH_EPSTATE_GOT_RESPONSE:
			_NT(trace_find_path_work_handler_rls,
				"iwarp conn/listener @CM_ID conn endpoint @PTR in state @FIND_PATH_CEP_STATE releasing",
				cep->conn, cep, cep->state);

			/* Set state to releasing */
			cep->state = FIND_PATH_EPSTATE_RELEASING;

			if (del_timer_sync(&cep->sock_shutdown_timer)) {
				find_path_cep_put(cep, "shutdown-timer");
			}

			find_path_work_handler_release_sock(cep);

			cep->state = FIND_PATH_EPSTATE_DONE;

			free_cep = true;
			break;
		default:
			_NT(trace_find_path_work_handler_rls_inv_state,
			    "iwarp conn/listener @CM_ID conn endpoint @PTR in invalid state @FIND_PATH_CEP_STATE",
				cep->conn, cep, cep->state);
			goto end_handler;
		}

		break;
	case FIND_PATH_WORK_TYPE_READ_RESP:
	{
		union nvmeib_version target_version;
		u64 target_commit;
		int iw_primary_port, iw_2nd_base_port, iw_2nd_num_ports;
		int status = 0;
		int conn_dest_port;
		struct nvmeib_rdma_event event;
		struct iw_cm_pd_hdr iw_cm_pd = {};

		if (cep->state == FIND_PATH_EPSTATE_CONNECTING) {
			/* If we're getting data, we can move to connected state. No need to wait for state_change callback */
			cep->state = FIND_PATH_EPSTATE_CONNECTED;
		}

		if (cep->state != FIND_PATH_EPSTATE_CONNECTED)
		{
			/* Connection was released before this work */
			_NT(trace_find_path_work_handler_read_rsp_inv_state,
			    "iwarp conn/listener @CM_ID conn endpoint @PTR in invalid state @FIND_PATH_CEP_STATE",
				cep->conn, cep, cep->state);
			goto end_handler;
		}

		if (!cep->conn) {
			_NT(trace_find_path_work_handler_cep_inv,
			    "conn endpoint @PTR has been invalidated", cep);
			goto sock_shutdown;
		}

		status = find_path_sock_recv_resp(cep);
		if (status == -EAGAIN)
			goto end_handler;
		if (status < 0)
			goto call_handler;

		/* Read the entire response => Process it */
		cep->state = FIND_PATH_EPSTATE_GOT_RESPONSE;

		status = find_path_sock_proc_resp(cep, &target_version, &target_commit, &iw_primary_port, &iw_2nd_base_port, &iw_2nd_num_ports);
		if (status < 0)
			goto call_handler;

		_NT(trace_find_path_work_handler_recv_ok,
		    "iwarp conn @CM_ID conn endpoint @PTR got valid find-path response."
		    " Target Version: @MAJOR.@MINOR.@SUBMINOR.@MR Commit: @COMMIT_ID "
		    "iWarp Primary Port: @PORT, iWarp Secondary Ports: [@START_PORT, @END_PORT]",
			cep->conn, cep, target_version.product.major, target_version.product.minor,
			target_version.product.subminor, target_version.product.subminor, target_commit,
			iw_primary_port, iw_2nd_base_port, iw_2nd_base_port + iw_2nd_num_ports - 1);

		/* Check to see if the conn dest port matches */
		if (cep->conn->dst_addr.s.sin_family == AF_INET)
			conn_dest_port = ntohs(cep->conn->dst_addr.s.sin_port);
		else if (cep->conn->dst_addr.s.sin_family == AF_INET6)
			conn_dest_port = ntohs(cep->conn->dst_addr.s6.sin6_port);
		else
			BUG();

		if (!(conn_dest_port == iw_primary_port ||
			(conn_dest_port >= iw_2nd_base_port &&
			conn_dest_port < iw_2nd_base_port + iw_2nd_num_ports)))
		{
			_NT(trace_find_path_work_handler_dest_port_inv,
			    "iwarp_conn @CM_ID dest port @PORT does not match target primary port @PORT"
			    " or secondary ports [@START_PORT, @END_PORT]",
				cep->conn, conn_dest_port, iw_primary_port,
				iw_2nd_base_port, iw_2nd_base_port + iw_2nd_num_ports - 1);
			status = -ECONNREFUSED;
			goto call_handler;
		}
		status = 0;

call_handler:
		/* Call the iwarp event handler with the response expected when the cm is used */
		if (status == 0) {
			/* The CM technique uses iw_cm_reject with a special payload so we will mimic it */
			iw_cm_pd.rej.flags = IW_CM_REJ_FIND_PATH;

			event.event = NVMEIB_REJ_RECEIVED;
			event.status = IB_CM_REJ_CONSUMER_DEFINED;
			event.private_data = &iw_cm_pd;
			event.private_data_len = sizeof(iw_cm_pd);
		} else {
			event.event = NVMEIB_CONNECT_ERROR;
			event.status = status;
		}

		/* Call the event handler */
		if (cep->handler_state == FIND_PATH_CEP_HANDLER_IDLE && cep->conn) {
			_NT(trace_find_path_work_handler_call_evt,
			    "Calling iwarp conn @CM_ID event handler with event @EVENT status @STATUS",
				cep->conn, event.event, event.status);
			(*cep->conn->conn.event_handler)(cep->conn, &event);
			cep->handler_state = FIND_PATH_CEP_HANDLER_CALLED;
		}

sock_shutdown:
		/* Shutdown the socket (should trigger a TCP_CLOSE state-change which will schedule the release work) */
		find_path_sock_shutdown(cep);
	}
		break;
	default:
		BUG();
	}

end_handler:
	if (free_cep)
		find_path_sock_work_handler_free_cep(cep, "work-handler-free-cep");

	iw_find_path_sock_put_work(work, "queue-work-done");

	find_path_cep_set_free(cep);

	NFOUT;
}

static int start_iwarp_socket_listener(struct roce_rdma_listener *roce_listener,
				       struct sockaddr *laddr)
{
#if !KS_HAS_SOCK_SET_REUSEADDR
	int s_val = 1;
#endif
	struct nvmeib_dev *nvdev = roce_listener->listener.dev;
	struct ib_device *iw_dev = nvdev->ib_dev;
	struct net_device *iw_ndev = NULL;
	int rv;
	char listen_addr_str[48];
	struct find_path_sock_dev *sock_dev = nvdev->iw_find_path_sock_priv;
	struct find_path_sock_cep *listen_cep = NULL;

	NFIN;
	if (!sock_dev || !nvmeib_ref_get(&sock_dev->ref_conn)) {
		_NT(trace_start_iwarp_socket_listener_no_sock_dev,
		    "device @IB_DEVICE is going down", iw_dev->name);
		rv = -EBUSY;
		goto out;
	}

	if (!iw_dev->get_netdev ||
		!(iw_ndev = (*iw_dev->get_netdev)(iw_dev, 1)))
	{
		_NT(trace_start_iwarp_socket_listener_no_ndev,
		    "Failed to get netdev of iwarp device @IB_DEVICE",
		    iw_dev->name);
		rv = -ENOTSUPP;
		goto dec_ref;
	}

	if (!(listen_cep = kzalloc(sizeof(*listen_cep), GFP_KERNEL)))
	{
		_NT(trace_start_iwarp_socket_listener_cep_alloc_fail,
		    "Failed to allocate listener endpoint for iwarp_listener @CM_ID", roce_listener);
		rv = -ENOMEM;
		goto dec_ref;
	}

	kref_init(&listen_cep->ref);
	listen_cep->state = FIND_PATH_EPSTATE_IDLE;

	listen_cep->listener = roce_listener;
	spin_lock_init(&listen_cep->lock);
	init_waitqueue_head(&listen_cep->waitq);
	INIT_LIST_HEAD(&listen_cep->work_free_list);

	INIT_TIMER(&listen_cep->sock_shutdown_timer);
	listen_cep->sock_shutdown_timer.function = find_path_shutdown_timer_fn;
	TIMER_SET_DATA(listen_cep, sock_shutdown_timer, (unsigned long)listen_cep);

	if ((rv = find_path_cep_alloc_work(listen_cep, FIND_PATH_SOCK_LISTENER_MAX_WORK)) < 0) {
		_NT(trace_start_iwarp_socket_listener_work_alloc_fail, "OOM");
		rv = -ENOMEM;
		goto free_cep;
	}

	/* device back pointer */
	find_path_dev_get(sock_dev, "new-listener");
	listen_cep->parent = sock_dev;

	/* Add to device list */
	find_path_cep_get(listen_cep, "dev-cep-list");
	mutex_lock(&sock_dev->lock);
	list_add_tail(&listen_cep->link, &sock_dev->sock_cep_list);
	mutex_unlock(&sock_dev->lock);

	find_path_cep_set_inuse(listen_cep);

	rv = sock_create((roce_listener->ipv4_only ? AF_INET : AF_INET6),
			  SOCK_STREAM, IPPROTO_TCP, &listen_cep->s);
	if (rv < 0) {
		_NT(trace_start_iwarp_socket_listener_no_sock,
		    "Failed (@RV) on sock_create", rv);
		goto remove_from_dev;
	}

#if KS_HAS_SOCK_SET_REUSEADDR
	sock_set_reuseaddr(listen_cep->s->sk);
	rv = 0;
#else
	rv = kernel_setsockopt(listen_cep->s, SOL_SOCKET, SO_REUSEADDR, (char *)&s_val,
				sizeof s_val);
#endif
	if (rv != 0) {
		_NT(trace_start_iwarp_socket_listener_sockopt_fail,
		    "Failed (@RV) on kernel_setsockopt", rv);
		goto release;
	}

	listen_cep->s->sk->sk_bound_dev_if = iw_ndev->ifindex;
	rv = kernel_bind(listen_cep->s, laddr, laddr->sa_family == AF_INET ?
		sizeof(struct sockaddr_in) :
		sizeof(struct sockaddr_in6));
	if (rv != 0) {
		_NT(trace_start_iwarp_socket_listener_bind_fail,
		    "Failed (@RV) on bind", rv);
		goto release;
	}

	/* sk_user_data reference */
	find_path_cep_get(listen_cep, "sk_user_data");
	write_lock(&listen_cep->s->sk->sk_callback_lock);
	listen_cep->s->sk->sk_user_data = listen_cep;
	listen_cep->orig_sk_state_change = listen_cep->s->sk->sk_state_change;
	listen_cep->orig_sk_data_ready = listen_cep->s->sk->sk_data_ready;
	listen_cep->s->sk->sk_state_change = iw_find_path_sock_state_change;
	listen_cep->s->sk->sk_data_ready = iw_find_path_sock_data_ready;
	write_unlock(&listen_cep->s->sk->sk_callback_lock);

	rv = kernel_listen(listen_cep->s, FIND_PATH_SOCK_LISTENER_MAX_WORK);
	if (rv != 0) {
		_NT(trace_start_iwarp_socket_listener_listen_fail,
		    "Failed (@RV) on listen", rv);
		goto disassoc;
	}

	listen_cep->state = FIND_PATH_EPSTATE_LISTENING;

	find_path_cep_set_free(listen_cep);

	find_path_cep_get(listen_cep, "listener-ptr");
	roce_listener->find_path_sock_listen_cep = listen_cep;

	scnprintf(listen_addr_str, sizeof(listen_addr_str), "%pISpc", laddr);
	_NT(trace_start_iwarp_socket_listener_ok,
	    "Started socket listener on addr @ADDR_STR for endpoint @PTR on iwarp listener @CM_ID on device @IB_DEVICE",
		listen_addr_str, listen_cep, roce_listener, roce_listener->listener.dev->ib_dev->name);

	goto out;

disassoc:
	write_lock_bh(&listen_cep->s->sk->sk_callback_lock);
	listen_cep->s->sk->sk_state_change = listen_cep->orig_sk_state_change;
	listen_cep->s->sk->sk_data_ready = listen_cep->orig_sk_data_ready;
	listen_cep->s->sk->sk_user_data = NULL;
	write_unlock_bh(&listen_cep->s->sk->sk_callback_lock);

	/* sk_user_data reference */
	find_path_cep_put(listen_cep, "sk_user_data");

release:
	sock_release(listen_cep->s);
	listen_cep->s = NULL;

remove_from_dev:
	mutex_lock(&sock_dev->lock);
	list_del_init(&listen_cep->link);
	find_path_dev_put(sock_dev, "new-listener");
	listen_cep->parent = NULL;
	mutex_unlock(&sock_dev->lock);

free_cep:
	find_path_cep_free_work(listen_cep);
	kfree(listen_cep);

dec_ref:
	nvmeib_ref_put(&sock_dev->ref_conn);

out:
	if (iw_ndev)
		dev_put(iw_ndev);
	NFOUT;
	return rv;
}

static void iwarp_disconnect_sock_only(struct find_path_sock_cep *cep)
{
	find_path_cep_set_inuse(cep);

	if (cep->state == FIND_PATH_EPSTATE_LISTENING ||
		cep->state == FIND_PATH_EPSTATE_CONNECTING ||
		cep->state == FIND_PATH_EPSTATE_CONNECTED ||
		cep->state == FIND_PATH_EPSTATE_GOT_RESPONSE)
	{

		/* Shutdown the socket (the TCP_CLOSE state-change will take care of the rest) */
		find_path_sock_shutdown(cep);
	}

	find_path_cep_set_free(cep);
}

static void stop_iwarp_socket_listener(struct roce_rdma_listener *listener)
{
	struct find_path_sock_cep *listen_cep = listener->find_path_sock_listen_cep;
	LIST_HEAD(cep_list);

	NFIN;
	if (!listen_cep) {
		if (listener->iw_primary) {
			_NT(trace_stop_iwarp_socket_listener_no_cep,
				"No listener endpoint for primary iwarp listener @CM_ID", listener);
		}
		goto out;
	}

	_NT(trace_stop_iwarp_socket_listener,
	    "Shutting down the listening endpoint @PTR socket "
	    " for iwarp listener @CM_ID", listen_cep, listener);

	listener->find_path_sock_listen_cep = NULL;
	find_path_cep_put(listen_cep, "listener-ptr");

	/* Change the state of the listen cep */
	find_path_cep_set_inuse(listen_cep);
	if (listen_cep->state == FIND_PATH_EPSTATE_LISTENING) {
		listen_cep->state = FIND_PATH_EPSTATE_RELEASING;
		/* Shutdown the listening socket (the TCP_CLOSE state-change will take care of the rest) */
		find_path_sock_shutdown(listen_cep);
	}
	find_path_cep_set_free(listen_cep);

out:
	NFOUT;
}

/* initialize listener to roce interface.
   general listener on all roce devices
*/
static int start_iwarp_listener(struct nvmeib_rdma_cm *cm)
{
	struct iw_cm_id *cm_id;
	struct rdma_listener *listener = NULL;
	int port;
	struct roce_rdma_listener *roce_listener = NULL;
	int ret;

	NFIN;
	listener = cm_2_l(cm);
	if (!listener) {
		_NE(error_nvmeib_rdma_start_iwarp_listener, "Invalid rdma_listener @CM", cm);
		ret = -EINVAL;
		goto out;
	}
	if (!(roce_listener = l_2_roce(listener))) {
		_NE(error_1_nvmeib_rdma_start_iwarp_listener, "Invalid roce_listener @CM", cm);
		ret = -EINVAL;
		goto out;
	}
	port = roce_listener->port;

	cm_id = iw_create_cm_id(listener->dev->ib_dev, iwarp_cm_handler_impl, roce_listener);

	if (cm_id == NULL) {
		_NE(error_2_nvmeib_rdma_start_iwarp_listener, "iw_create_cm_id() failed");
		ret = -1;
		goto out;
	}
	else
		_ND(trace_nvmeib_rdma_start_iwarp_listener, "iw_create_cm_id returned @CM_ID", cm_id);

	if (!roce_listener->ipv4_only) {
		struct sockaddr_in6 *sock6 = (void *)&cm_id->local_addr;
		sock6->sin6_family = AF_INET6;
		sock6->sin6_port = htons(port);
		sock6->sin6_addr = in6addr_any;
	} else {
		struct sockaddr_in *sock = (void *)&cm_id->local_addr;
		sock->sin_family = AF_INET;
		sock->sin_port = htons(port);
		sock->sin_addr.s_addr = INADDR_ANY;
	}

	ret = iw_cm_listen(cm_id, 999);
	_ND(trace_2_nvmeib_rdma_start_iwarp_listener, "iw_cm_listen returned @RET_INT", ret);
	if (ret) {
		_NE(error_4_nvmeib_rdma_start_iwarp_listener, "iw_cm_listen() failed: @RET_INT", ret);
		goto out_id;
	}

	roce_listener->iw_cm_id = cm_id;

	if (nvmeib_iwarp_find_path_sock && roce_listener->iw_primary) {
		/* Start the socket listener for find-path */
		struct sockaddr_storage ss_listen = {0};
		if (!roce_listener->ipv4_only) {
			struct sockaddr_in6 *sock6 = (void *)&ss_listen;
			sock6->sin6_family = AF_INET6;
			sock6->sin6_port = htons(nvmeib_iwarp_find_path_sock_port);
			sock6->sin6_addr = in6addr_any;
		} else {
			struct sockaddr_in *sock = (void *)&ss_listen;
			sock->sin_family = AF_INET;
			sock->sin_port = htons(nvmeib_iwarp_find_path_sock_port);
			sock->sin_addr.s_addr = INADDR_ANY;
		}

		ret = start_iwarp_socket_listener(roce_listener, (void *)&ss_listen);
		if (ret < 0) {
			_NT(error_5_nvmeib_rdma_start_iwarp_listener,
			    "Failed (@RV) to start iwarp socket listener on port @PORT",
			    ret, nvmeib_iwarp_find_path_sock_port);
			goto out_id;
		}
	}

	_ND(trace_3_nvmeib_rdma_start_iwarp_listener, "iwarp listener init successfully");
	goto out;

out_id:
	_ND(trace_4_nvmeib_rdma_start_iwarp_listener, "iwarp listener died!!");
	iw_destroy_cm_id(cm_id);

out:
	NFOUT;
	return ret;
}


/* create infiniband connection */
static struct ib_rdma_connection* ib_create_connection(
	struct ib_rdma_listener *ib, struct ib_cm_id *cm_id)
{
	struct ib_rdma_connection *conn = kzalloc(sizeof(*conn), GFP_KERNEL);

	NFIN;
	if (conn) {
		conn->cm_id = cm_id;
		conn->conn.cm.rdma_type = ib->listener.cm.rdma_type;
		conn->conn.cm.cm_type = _cm_connection;
		conn->conn.listener = &ib->listener;
	}
	NFOUT;
	return conn;
}

/**
 * find connection in the list of managed connection
 *  */
static struct rdma_connection* ib_find_rdma_cm(struct rdma_listener *listener,
	struct ib_cm_id *cm_id)
{
	struct rdma_connection *conn;
	unsigned long flags;
	bool found = false;

	NFIN;
	spin_lock_irqsave(&listener->connection_guard, flags);
	list_for_each_entry(conn, &listener->connections, link)
		if (conn_2_ib(conn)->cm_id == cm_id) {
			found = true;
			break;
		}
	spin_unlock_irqrestore(&listener->connection_guard, flags);
	if (!found)
		conn = NULL;
	NFOUT;
	return conn;
}

/**
 * ib_event_2_nvmeib_event() - convert infiniband event to rdma
 * event and thus enable cross protocol event handlers
 *
 * @author yaron (4/29/2015)
 *
 * @param event - input infiniband event
 * @param out_event - resulted event
 */
static void ib_event_2_nvmeib_event(const struct ib_cm_event *event,
	struct nvmeib_rdma_event *out_event, struct ib_cm_id *cm_id)
{
	NFIN;
	switch (event->event) {
	case IB_CM_REQ_ERROR:
		_NT(ib_event_2_nvmeib_event_t1, "IB_CM_REQ_ERROR");
		out_event->event = NVMEIB_CONNECT_ERROR;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_REQ_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_REQ_RECEIVED:
		_ND(ib_event_2_nvmeib_event_d1, "IB_CM_REQ_RECEIVED");
		out_event->event = NVMEIB_REQ_RECEIVED;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_REQ_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_REP_ERROR:
		_NT(ib_event_2_nvmeib_event_t2, "IB_CM_REP_ERROR");
		out_event->event = NVMEIB_REP_ERROR;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_REP_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_REP_RECEIVED:
		_ND(ib_event_2_nvmeib_event_d2, "IB_CM_REP_RECEIVED");
		out_event->event = NVMEIB_USER_ESTABLISHED;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_REP_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_RTU_RECEIVED:
		_ND(ib_event_2_nvmeib_event_d3, "IB_CM_RTU_RECEIVED");
		out_event->event = NVMEIB_RTU_RECEIVED;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_RTU_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_USER_ESTABLISHED:
		_ND(ib_event_2_nvmeib_event_d4, "IB_CM_USER_ESTABLISHED");
		out_event->event = NVMEIB_USER_ESTABLISHED;
		break;
	case IB_CM_DREQ_ERROR:
		_NT(ib_event_2_nvmeib_event_t3, "IB_CM_DREQ_ERROR (cm_id=@PTR)", cm_id);
		out_event->event = NVMEIB_DREQ_ERROR;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_DREQ_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_DREQ_RECEIVED:
		_ND(ib_event_2_nvmeib_event_d5, "IB_CM_DREQ_RECEIVED (cm_id=@PTR)", cm_id);
		out_event->event = NVMEIB_DREQ_RECEIVED;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_DREQ_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_DREP_RECEIVED:
		_ND(ib_event_2_nvmeib_event_d6, "IB_CM_DREP_RECEIVED (cm_id=@PTR)", cm_id);
		out_event->event = NVMEIB_DREP_RECEIVED;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_DREP_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_TIMEWAIT_EXIT:
		_NT(ib_event_2_nvmeib_event_t4, "IB_CM_TIMEWAIT_EXIT (cm_id=@PTR)", cm_id);
		out_event->event = NVMEIB_TIMEWAIT_EXIT;
		break;
	case IB_CM_MRA_RECEIVED:
		_ND(ib_event_2_nvmeib_event_d7, "IB_CM_MRA_RECEIVED");
		out_event->event = NVMEIB_MRA_RECEIVED;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_MRA_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_REJ_RECEIVED:
		_NT(ib_event_2_nvmeib_event_t5, "IB_CM_REJ_RECEIVED (cm_id=@PTR)", cm_id);
		out_event->event = NVMEIB_REJ_RECEIVED;
		out_event->private_data = event->private_data;
		out_event->private_data_len = IB_CM_REJ_PRIVATE_DATA_SIZE;
		break;
	case IB_CM_LAP_ERROR:
	case IB_CM_LAP_RECEIVED:
	case IB_CM_APR_RECEIVED:
	case IB_CM_SIDR_REQ_ERROR:
	case IB_CM_SIDR_REQ_RECEIVED:
	case IB_CM_SIDR_REP_RECEIVED:
	default:
		_NW(ib_event_2_nvmeib_event_w1, "unsupported ib event @INT (cm_id=@PTR)", event->event, cm_id);
		break;
	}
	NFOUT;
}

/**
 * ib_cm_handler_impl() - IB connection manager callback
 * function used only for the listener.
 *
 * A non-zero return value will cause the caller destroy the CM ID.
 *
 * Note: nvmeibs_cm_handler() must only return a non-zero value
 * when transferring ownership of the cm_id to a client by
 * nvmeibs_cm_req_recv() failed.
 */
static IB_DECLARE_CM_HANDLER(ib_cm_handler_impl)
{
	struct ib_rdma_listener *ib = cm_id->context;
	struct nvmeib_rdma_conn_params params = {0};
	struct nvmeib_rdma_event rdma_event;
	struct ib_rdma_connection *ib_conn;
	struct rdma_connection *conn;
	unsigned long flags;
	int rv;

	NFIN;
	/* the listener */
	if (event->event == IB_CM_REQ_RECEIVED) { /* a client tries to connect */
		if ((ib_conn = ib_create_connection(ib, cm_id))) {
			ib_conn->dgid = event->param.req_rcvd.primary_path->dgid;
			ib_conn->sgid = event->param.req_rcvd.primary_path->sgid;
			params.d = ib->listener.dev->ib_dev;
			params.port = event->param.req_rcvd.port;
			memcpy(params.private_data, event->private_data,
				IB_CM_REQ_PRIVATE_DATA_SIZE);
			spin_lock_irqsave(&ib->listener.connection_guard, flags);
			list_add_tail(&ib_conn->conn.link, &ib->listener.connections);
			spin_unlock_irqrestore(&ib->listener.connection_guard, flags);
			if ((rv = ib->listener.new_connection(
				&ib_conn->conn.cm, &params)) < 0) {
				_NE(error_nvmeib_rdma_IB_DECLARE_CM_HANDLER, "new connection() failed");
				spin_lock_irqsave(&ib->listener.connection_guard, flags);
				list_del_init(&ib_conn->conn.link);
				spin_unlock_irqrestore(&ib->listener.connection_guard, flags);
				goto free_conn;
			}
			else {
				_NT(trace_nvmeib_rdma_IB_DECLARE_CM_HANDLER, "Accepeted a new connection");
			}
		}
		else
			rv = -ENOMEM;
	}
	else {
		ib_event_2_nvmeib_event(event, &rdma_event, cm_id);
		/*
		   this is not a new connection so probably the new event is due to
		   a change in a client state.
		*/
		if (cm_id != ib->cm_id) {
			if ((conn = ib_find_rdma_cm(&ib->listener, cm_id))) {
				ib_conn = conn_2_ib(conn);
				ib->listener.on_peer_event(&ib_conn->conn.cm, &rdma_event);
			}
			else
				_NE(error_1_nvmeib_rdma_IB_DECLARE_CM_HANDLER, "Received CM event @EVENT but connection is not ours",
					event->event);
		}
		else
			/* this is wrong.  we received an event on a listener cm */
			_NE(error_2_nvmeib_rdma_IB_DECLARE_CM_HANDLER, "Listener received @EVENT event - ignoring", event->event);
		rv = 0;
	}
	goto out;

free_conn:
	kfree(ib_conn);

out:
	NFOUT;
	return rv;
}

/* initalize infiniband listener */
static int start_ib_listener(struct nvmeib_rdma_cm *cm)
{
	struct ib_rdma_listener *ib = cm_2_ibl(cm);
	int rv = 0;

	NFIN;
	if (!ib) {
		_NE(error_nvmeib_rdma_start_ib_listener, "Invalid ib_rdma_listener @CM", cm);
		rv = -EINVAL;
		goto out;
	}
	ib->cm_id = ib_create_cm_id(ib->listener.dev->ib_dev, ib_cm_handler_impl, ib);
	if (IS_ERR(ib->cm_id)) {
		_NE(error_1_nvmeib_rdma_start_ib_listener, "@DEV_NAME ib_create_cm_id() failed - @PTR_ERR",
			ib->listener.dev->ib_dev->name, PTR_ERR(ib->cm_id));
		ib->cm_id = NULL;
		rv = -1;
		goto out;
	}

	_ND(trace_nvmeib_rdma_start_ib_listener, "start listener on @SERVICE_ID", cpu_to_be64(ib->service_id));

#if KS_RDMA_IB_CM_LISTEN_HAS_SERVICE_MASK
	rv = ib_cm_listen(ib->cm_id, cpu_to_be64(ib->service_id), 0);
#else
	rv = ib_cm_listen(ib->cm_id, cpu_to_be64(ib->service_id));
#endif

	if (rv < 0)
	{
		_NE(error_2_nvmeib_rdma_start_ib_listener, "@DEV_NAME ib_cm_listen() failed - @RV", ib->listener.dev->ib_dev->name, rv);
		goto out;
	}

out:
	NFOUT;
	return rv;
}

/* start a new lestener */
struct nvmeib_rdma_cm* nvmeib_rdma_listen(
	struct nvmeib_rdma_listen_params *params)
{
	struct nvmeib_rdma_cm *cm;

	NFIN;
	if (!(cm = create_listen_cm(params))) {
		_NE(error_nvmeib_rdma_nvmeib_rdma_listen, "Fail to create rdma connection object");
		goto out;
	}
	if (cm->rdma_type == _rdma_ib) {
		if (start_ib_listener(cm) < 0)
			goto free_cm;
	}
	else if (cm->rdma_type == _rdma_roce) {
		if (start_roce_listener(cm) < 0)
			goto free_cm;
	}
	else if (cm->rdma_type == _rdma_iwarp) {
		if (start_iwarp_listener(cm) < 0)
			goto free_cm;
	}
	else if (cm->rdma_type == _rdma_lb) {
		if (start_lb_listener(cm) < 0)
			goto free_cm;
	}
	else {
		_NE(error_1_nvmeib_rdma_nvmeib_rdma_listen, "unsupported connection type @RDMA_TYPE", cm->rdma_type);
		goto free_cm;
	}

	_NI(info_nvmeib_rdma_listen,
		"Created listener, cm=@PTR, type=@INT", cm, cm->rdma_type);
	goto out;

free_cm:
	free_cm(cm);
	cm = NULL;

out:
	NFOUT;
	return cm;
}
EXPORT_SYMBOL(nvmeib_rdma_listen);

void nvmeib_rdma_stop_listen(struct nvmeib_rdma_cm *cm)
{
	struct ib_rdma_listener *ibl;
	struct roce_rdma_listener *rocel;
	struct lb_rdma_listener *lbl;

	NFIN;

	_NI(info_nvmeib_rdma_stop_listen, "Stopping listener, cm=@PTR", cm);

	if (cm && cm->cm_type == _cm_listen) {
		if (cm->rdma_type == _rdma_ib) {
			ibl = cm_2_ibl(cm);
			if (ibl) {
				if (ibl->cm_id) {
					//WARN_ON(!list_empty(&ibl->listener.connections));
					ib_destroy_cm_id(ibl->cm_id);
					ibl->cm_id = NULL;
				} else
					_NE(error_nvmeib_rdma_nvmeib_rdma_stop_listen, "ib_rdma_listener @IBL has NULL cm_id", ibl);
			} else
				_NE(error_1_nvmeib_rdma_nvmeib_rdma_stop_listen, "Invalid ib_rdma_listener @CM", cm);
		}
		else if (cm->rdma_type == _rdma_roce || cm->rdma_type == _rdma_iwarp) {
			rocel = cm_2_rocel(cm);
			if (rocel) {
				if (nvmeib_iwarp_find_path_sock)
					stop_iwarp_socket_listener(rocel);
				if (rocel->cm_id) {
					//WARN_ON(!list_empty(&rocel->listener.connections));
					if (cm->rdma_type == _rdma_iwarp)
						iw_destroy_cm_id(rocel->iw_cm_id);
					else
						rdma_destroy_id(rocel->cm_id);
					rocel->cm_id = NULL;
				} else
					_NE(error_2_nvmeib_rdma_nvmeib_rdma_stop_listen, "roce_rdma_listener @ROCEL has NULL cm_id", rocel);
			} else
				_NE(error_3_nvmeib_rdma_nvmeib_rdma_stop_listen, "Invalid roce_rdma_listener @CM", cm);
		}
		else if (cm->rdma_type == _rdma_lb) {
			lbl = cm_2_lbl(cm);
			if (lbl) {
				//WARN_ON(!list_empty(&lbl->listener.connections));
				stop_lb_listener(lbl);
			} else
				_NE(error_4_nvmeib_rdma_nvmeib_rdma_stop_listen, "Invalid lb_rdma_listener @CM", cm);
		}
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_rdma_stop_listen);

/* As RoCE listener is per module (not dev), it might have conns when port is going down */
bool nvmeib_rdma_listener_has_conns(struct nvmeib_rdma_cm *cm)
{
	struct ib_rdma_listener *ibl;
	struct roce_rdma_listener *rocel;
	struct lb_rdma_listener *lbl;

	if (cm && cm->cm_type == _cm_listen) {
		if ((cm->rdma_type == _rdma_ib) && (ibl = cm_2_ibl(cm))) {
			return !list_empty(&ibl->listener.connections);
		}
		else if ((cm->rdma_type == _rdma_roce || cm->rdma_type == _rdma_iwarp) && (rocel = cm_2_rocel(cm))) {
			return !list_empty(&rocel->listener.connections);
		}
		else if (cm->rdma_type == _rdma_lb && (lbl = cm_2_lbl(cm))) {
			return !list_empty(&lbl->listener.connections);
		}
		else {
			_NE(error_4_nvmeib_rdma_listener_has_conns,
				"unknown listener @CM type+@INT", cm, cm->rdma_type);
		}
	}

	return false;
}
EXPORT_SYMBOL(nvmeib_rdma_listener_has_conns);

int nvmeib_rdma_update_lb_listen(struct nvmeib_rdma_cm *cm,
				 struct nvmeib_rdma_listen_lb_params *params)
{
	int rv = 0;
	struct lb_rdma_listener *lbl;

	NFIN;
	if (!(lbl = cm_2_lbl(cm))) {
		rv = -EINVAL;
		goto out;
	}

	mutex_lock(&lbl->guard);
	lbl->gid = params->gid;
	lbl->gid_index = params->gid_index;
	memcpy(lbl->roce_mac, params->roce_mac, ETH_ALEN);
	lbl->pkey_index = params->pkey_index;
	lbl->link_layer = params->link_layer;
	mutex_unlock(&lbl->guard);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_update_lb_listen);

void *nvmeib_rdma_get_listener_context(struct nvmeib_rdma_cm *cm)
{
	void *context = NULL;
	struct rdma_connection *cm_c;
	struct rdma_listener *cm_l;

	NFIN;
	if (!cm) {
		context = ERR_PTR(EINVAL);
		goto out;
	}
	if (cm->cm_type == _cm_connection) {
		if (!(cm_c = cm_2_c(cm))) {
			context = ERR_PTR(EINVAL);
			goto out;
		}
		if (!(cm_c->listener)) {
			context = ERR_PTR(EINVAL);
			goto out;
		}
		context = cm_c->listener->context;
	} else if (cm->cm_type == _cm_listen) {
		if (!(cm_l = cm_2_l(cm))) {
			context = ERR_PTR(EINVAL);
			goto out;
		}
		context = cm_l->context;
	} else
		context = ERR_PTR(EINVAL);
out:
	NFOUT;
	return context;
}
EXPORT_SYMBOL(nvmeib_rdma_get_listener_context);

void* nvmeib_rdma_get_connection_context(struct nvmeib_rdma_cm *cm)
{
	void *context = NULL;
	struct rdma_connection *cm_c;

	NFIN;
	if (!cm) {
		context = ERR_PTR(EINVAL);
		goto out;
	}
	if (cm->cm_type != _cm_connection) {
		context = ERR_PTR(EINVAL);
		goto out;
	}
	if (!(cm_c = cm_2_c(cm))) {
		context = ERR_PTR(EINVAL);
		goto out;
	}
	context = cm_c->context;
out:
	NFOUT;
	return context;
}
EXPORT_SYMBOL(nvmeib_rdma_get_connection_context);

/* reject login request accroding to connection type */
void nvmeib_rdma_login_reject(struct nvmeib_rdma_cm *cm, void *rej, u8 len)
{
	NFIN;

	if (!cm) {
		_NE(error_nvmeib_rdma_nvmeib_rdma_login_reject, "expected connection but got NULL");
		goto out;
	}

	if (cm->cm_type != _cm_connection) {
		_NE(error_1_nvmeib_rdma_nvmeib_rdma_login_reject, "Expected connection object but got @CM_TYPE", cm->cm_type);
		goto out;
	}

	if (cm->rdma_type == _rdma_ib) {
		struct ib_rdma_connection *ibc = cm_2_ibc(cm);
		if (ibc) {
			ib_send_cm_rej(ibc->cm_id, IB_CM_REJ_CONSUMER_DEFINED,
				NULL, 0, rej, len);
		} else
			_NE(error_2_nvmeib_rdma_nvmeib_rdma_login_reject, "Invalid ib_rdma_connection @CM", cm);
	}
	else if (cm->rdma_type == _rdma_roce || cm->rdma_type == _rdma_iwarp) {
		struct roce_rdma_connection *roce_conn = cm_2_rocec(cm);
		if (roce_conn && roce_conn->cm_id) {
			if (cm->rdma_type == _rdma_iwarp) {
				size_t pd_len = offsetof(struct iw_cm_pd, user_pd) + len;
				struct iw_cm_pd *iw_pd = kzalloc(pd_len, GFP_KERNEL);
				if (iw_pd) {
					iw_pd->hdr.rej.reason = IB_CM_REJ_CONSUMER_DEFINED;
					memcpy(iw_pd->user_pd, rej, len);
					iw_cm_reject(roce_conn->iw_cm_id, iw_pd, pd_len);
					kfree(iw_pd);
				} else {
					_NE(error_4_nvmeib_rdma_nvmeib_rdma_login_reject, "OOM");
				}
			}
			else {
				_NT(trace_nvmeib_rdma_nvmeib_rdma_login_reject, "Send reject to roce_conn=@PTR", roce_conn);
				__nvmeib_rdma_reject_reason(roce_conn->cm_id, rej, len, IB_CM_REJ_CONSUMER_DEFINED);
			}
		}
		else
			_NE(error_5_nvmeib_rdma_nvmeib_rdma_login_reject, "Invalid roce_rdma_connection @CM", cm);
	}
	else if (cm->rdma_type == _rdma_lb_accept) {
		lb_reject(cm, rej, len);
	} else {
		_NE(error_3_nvmeib_rdma_nvmeib_rdma_login_reject, "cannot reject, unknown protocol @RDMA_TYPE", cm->rdma_type);
	}

out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_rdma_login_reject);

static int ib_conn_modify_qp(struct ib_rdma_connection *conn)
{
	struct ib_qp_attr qp_attr = {0};
	int attr_mask = IB_QP_TIMEOUT;
	int rv;


	NFIN;
	qp_attr.qp_state = IB_QPS_RTR;
	if ((rv = ib_cm_init_qp_attr(conn->cm_id, &qp_attr, &attr_mask))) {
		_NE(error_nvmeib_rdma_ib_conn_modify_qp, "ib_cm_init_qp_attr() failed");
		goto out;
	}

	//qp_attr.path_mtu = NVMEIB_IO_QP_MTU;
	if ((rv = ib_modify_qp(conn->conn.qp, &qp_attr, attr_mask)) < 0) {
		_NE(error_1_nvmeib_rdma_ib_conn_modify_qp, "ib_modify_qp() failed");
		goto out;
	}
	if (conn->conn.qp->srq != NULL)
		conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;

	memset(&qp_attr,0, sizeof(qp_attr));
	qp_attr.qp_state = IB_QPS_RTS;
	if ((rv = ib_cm_init_qp_attr(conn->cm_id, &qp_attr, &attr_mask))) {
		_NE(error_2_nvmeib_rdma_ib_conn_modify_qp, "ib_cm_init_qp_attr() failed");
		goto out;
	}

	_NT(trace_nvmeib_rdma_ib_conn_modify_qp, "qp_timeout=@QP_TIMEOUT ret=@RET_INT,rnr=@RNR,min_tim=@MIN_TIM", qp_attr.timeout, qp_attr.retry_cnt, qp_attr.rnr_retry, qp_attr.min_rnr_timer);
	attr_mask |= IB_QP_TIMEOUT;
	qp_attr.timeout = qp_timeout;

	if ((rv = ib_modify_qp(conn->conn.qp, &qp_attr, attr_mask)) < 0) {
		_NE(error_3_nvmeib_rdma_ib_conn_modify_qp, "ib_modify_qp() failed");
		goto out;
	}

	if ((rv = ib_send_cm_rtu(conn->cm_id, NULL, 0))) {
		_NE(error_4_nvmeib_rdma_ib_conn_modify_qp, "ib_send_cm_rtu() failed");
		goto out;
	}

out:
	NFOUT;
	return rv;
}

static int ib_cm_event_data_len(enum ib_cm_event_type event_type)
{
	switch (event_type) {
	case IB_CM_REQ_ERROR:
	case IB_CM_REQ_RECEIVED:
		return IB_CM_REQ_PRIVATE_DATA_SIZE;
	case IB_CM_REJ_RECEIVED:
		return IB_CM_REJ_PRIVATE_DATA_SIZE;
	case IB_CM_REP_ERROR:
	case IB_CM_REP_RECEIVED:
		return IB_CM_REP_PRIVATE_DATA_SIZE;
	case IB_CM_RTU_RECEIVED:
		return IB_CM_RTU_PRIVATE_DATA_SIZE;
	case IB_CM_DREQ_ERROR:
	case IB_CM_DREQ_RECEIVED:
		return IB_CM_DREQ_PRIVATE_DATA_SIZE;
	case IB_CM_DREP_RECEIVED:
		return IB_CM_DREP_PRIVATE_DATA_SIZE;
	case IB_CM_LAP_ERROR:
	case IB_CM_LAP_RECEIVED:
		return IB_CM_LAP_PRIVATE_DATA_SIZE;
	case IB_CM_APR_RECEIVED:
		return IB_CM_APR_PRIVATE_DATA_SIZE;
	case IB_CM_SIDR_REQ_ERROR:
	case IB_CM_SIDR_REQ_RECEIVED:
		return IB_CM_SIDR_REQ_PRIVATE_DATA_SIZE;
	case IB_CM_SIDR_REP_RECEIVED:
		return IB_CM_SIDR_REP_INFO_LENGTH;
	default:
		return 0;
	}
}

static IB_DECLARE_CM_HANDLER(ib_conn_cm_handler)
{
	struct ib_rdma_connection *conn = cm_id->context;
	struct nvmeib_rdma_event rdma_event;
	bool conn_valid = true;
	unsigned long flags;

	NFIN;
	memset(&rdma_event, 0, sizeof(rdma_event));
	ib_event_2_nvmeib_event(event, &rdma_event, cm_id);
	rdma_event.private_data = event->private_data;
	rdma_event.private_data_len = ib_cm_event_data_len(event->event);
	/* to make the connection the same with RoCE we init the QP here */
	if (event->event == IB_CM_REP_RECEIVED) {
		/* if conn is still valid,
		   try to modify the QP and if we fail change the event */
		spin_lock_irqsave(&conn->conn.lock, flags);
		if (conn->state == NVMEIB_CM_CONN_INIT)
			conn->state = NVMEIB_CM_CONN_REP_RCVD;
		else
			conn_valid = false;
		spin_unlock_irqrestore(&conn->conn.lock, flags);
		if (!conn_valid || (ib_conn_modify_qp(conn) < 0))
			rdma_event.event = NVMEIB_CONNECT_ERROR;
	}
	else if (event->event == IB_CM_REJ_RECEIVED)
		rdma_event.status = event->param.rej_rcvd.reason;

	if (conn_valid)
		conn->conn.event_handler(conn->conn.context, &rdma_event);

	NFOUT;
	return 0;
}

struct iw_cm_inv_stats {
	spinlock_t lock;
	u64 n_inv;
	u64 n_inv_conn_err;
	u64 n_inv_wait_close;

	u64 min_wait_inv_us;
	u64 max_wait_inv_us;
	u64 tot_wait_inv_us;

	u64 min_wait_inv_conn_err_us;
	u64 max_wait_inv_conn_err_us;
	u64 tot_wait_inv_conn_err_us;

	u64 min_wait_inv_wait_close_us;
	u64 max_wait_inv_wait_close_us;
	u64 tot_wait_inv_wait_close_us;

	struct nvmeib_public_procfs_ent *proc_ent;
};

/**
 *  try to invalidte client's cm.
 *  prevent processing cm-conn-reply after timeout namely
 *  referencing caller's objects.
 */
bool nvmeib_rdma_try_inv_cm(struct nvmeib_rdma_cm *cm)
{
	struct ib_rdma_connection *ib_conn;
	struct roce_rdma_connection *roce_conn;
	bool inv = false;
	int rv_w;
	unsigned long flags;

	NFIN;
	if (cm) {
		if (cm->rdma_type == _rdma_ib && (ib_conn = cm_2_ibc(cm))) {
			spin_lock_irqsave(&ib_conn->conn.lock, flags);
			if (ib_conn->state == NVMEIB_CM_CONN_INIT) {
				/* We have not yet received a response to our connect request so we can invalidate the connection */
				inv = true;
				ib_conn->state = NVMEIB_CM_CONN_INV;
			} else {
				_NT(trace_nvmeib_rdma_try_inv_cm_fail_ib,
				    "Failed to invalidate ib conn @CM_ID due to state @STATE",
					ib_conn, ib_conn->state);
			}
			spin_unlock_irqrestore(&ib_conn->conn.lock, flags);
		} else if ((cm->rdma_type == _rdma_roce || cm->rdma_type == _rdma_iwarp) && (roce_conn = cm_2_rocec(cm))) {
			if (roce_conn->find_path_sock_cep) {
				struct find_path_sock_cep *cep = roce_conn->find_path_sock_cep;
				BUG_ON(cm->rdma_type != _rdma_iwarp);

				/* Clear the connection pointer so it can't call back */
				find_path_cep_set_inuse(cep);
				if (cep->state == FIND_PATH_EPSTATE_CONNECTING ||
					cep->state == FIND_PATH_EPSTATE_CONNECTED)
				{
					find_path_sock_shutdown(cep);
				}
				if (cep->handler_state == FIND_PATH_CEP_HANDLER_IDLE) {
					cep->handler_state = FIND_PATH_CEP_HANDLER_INVALIDATED;
					inv = true;
				}
				find_path_cep_set_free(cep);
				goto out;
			}

			spin_lock_irqsave(&roce_conn->conn.lock, flags);
			if (roce_conn->state != NVMEIB_ROCE_CM_INIT) {
				_NT(trace_nvmeib_rdma_try_inv_cm_fail_roce,
				    "Failed to invalidate roce/iwarp conn @CM_ID due to state @STATE", roce_conn, roce_conn->state);
				spin_unlock_irqrestore(&roce_conn->conn.lock, flags);
				goto out;
			}
			if (cm->rdma_type == _rdma_iwarp) {
				/* For iWARP, we still need to wait for the Connect or Connect Error event or iw_destroy_cm_id will deadlock.
				 * We can make the Connect Error event happen by moving the QP to error-state */
				DECLARE_COMPLETION(done);
				uint wait_cm_inv_timeout = nvmeib_iwarp_cm_inv_time_sec * HZ;

				NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
						     nvmeib_cm_ephemeral, try_inv_cm_state_2_wait_evt, roce_conn, roce_conn->state, NVMEIB_ROCE_CM_INV_WAIT_EVENT);

				roce_conn->cancel_iwcm_conn_done = &done;
				roce_conn->state = NVMEIB_ROCE_CM_INV_WAIT_EVENT;
				/* Unlock so we can call ib_set_qp_err */
				spin_unlock_irqrestore(&roce_conn->conn.lock, flags);
				/* Move the QP to error state to cancel the in-progress CM operation */
				ib_set_qp_err(roce_conn->conn.qp);
				roce_conn->iw_cm_inv_stats.start_wait = ktime_get();
				/* Wait for event */
				if ((rv_w = wait_for_completion_interruptible_timeout(&done, wait_cm_inv_timeout)) <= 0) {
					spin_lock_irqsave(&roce_conn->conn.lock, flags);
					_NW(warn_nvmeib_try_inv_cm_iw_wait_failed,
						"Failed (@RV) to wait for iwarp invalidate on conn @CM_ID in state @STATE",
					    rv_w, roce_conn, roce_conn->state);
					roce_conn->cancel_iwcm_conn_done = NULL;
					spin_unlock_irqrestore(&roce_conn->conn.lock, flags);
					BUG_ON(1);
					goto out;
				}
				roce_conn->iw_cm_inv_stats.end_wait = ktime_get();
				if (roce_conn->conn.dev->iw_cm_id_inv_stats_priv) {
					/* Update stats for iw_cm invalidate */
					struct iw_cm_inv_stats *stats = roce_conn->conn.dev->iw_cm_id_inv_stats_priv;
					ktime_t wait_time = ktime_sub(roce_conn->iw_cm_inv_stats.end_wait, roce_conn->iw_cm_inv_stats.start_wait);
					u64 wait_time_us = ktime_to_us(wait_time);
					spin_lock_bh(&stats->lock);
					stats->n_inv++;
					if (stats->n_inv == 1) {
						stats->min_wait_inv_us = wait_time_us;
						stats->tot_wait_inv_us = wait_time_us;
						stats->max_wait_inv_us = wait_time_us;
					} else {
						stats->min_wait_inv_us = min_t(u64, stats->min_wait_inv_us, wait_time_us);
						stats->tot_wait_inv_us += wait_time_us;
						stats->max_wait_inv_us = max_t(u64, stats->max_wait_inv_us, wait_time_us);
					}
					if (!roce_conn->iw_cm_inv_stats.wait_close) {
						stats->n_inv_conn_err++;
						if (stats->n_inv_conn_err == 1) {
							stats->min_wait_inv_conn_err_us = wait_time_us;
							stats->tot_wait_inv_conn_err_us = wait_time_us;
							stats->max_wait_inv_conn_err_us = wait_time_us;
						} else {
							stats->min_wait_inv_conn_err_us = min_t(u64, stats->min_wait_inv_conn_err_us, wait_time_us);
							stats->tot_wait_inv_conn_err_us += wait_time_us;
							stats->max_wait_inv_conn_err_us = max_t(u64, stats->max_wait_inv_conn_err_us, wait_time_us);
						}
					} else {
						stats->n_inv_wait_close++;
						if (stats->n_inv_wait_close == 1) {
							stats->min_wait_inv_wait_close_us = wait_time_us;
							stats->tot_wait_inv_wait_close_us = wait_time_us;
							stats->max_wait_inv_wait_close_us = wait_time_us;
						} else {
							stats->min_wait_inv_wait_close_us = min_t(u64, stats->min_wait_inv_wait_close_us, wait_time_us);
							stats->tot_wait_inv_wait_close_us += wait_time_us;
							stats->max_wait_inv_wait_close_us = max_t(u64, stats->max_wait_inv_wait_close_us, wait_time_us);
						}
					}
					spin_unlock_bh(&stats->lock);
				}
				spin_lock_irqsave(&roce_conn->conn.lock, flags);
				if (roce_conn->state != NVMEIB_ROCE_CM_INV_GOT_EVENT) {
					/* We have hit a hole in the flow, trace and panic */
					_NE_dmesg(err_nvmeib_try_inv_cm_iw_inv_state,
					    "Unexpected state @STATE for iwarp @CM_ID after wait", roce_conn->state, roce_conn);
					roce_conn->cancel_iwcm_conn_done = NULL;
					spin_unlock_irqrestore(&roce_conn->conn.lock, flags);
					BUG_ON(1);
				}
				roce_conn->cancel_iwcm_conn_done = NULL;
				spin_unlock_irqrestore(&roce_conn->conn.lock, flags);
				/* Now we can destroy the cm_id */
				iw_destroy_cm_id(roce_conn->iw_cm_id);
				inv = true;
			} else {
				BUG_ON(cm->rdma_type != _rdma_roce);

				NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
						     nvmeib_cm_ephemeral, try_inv_cm_roce_inv, roce_conn, roce_conn->state, NVMEIB_ROCE_CM_INV);

				/* We have not yet received a response to our connect request so we can invalidate the connection and destroy the ID */
				roce_conn->state = NVMEIB_ROCE_CM_INV;
				/* Unlock so we can call rdma_destroy_id */
				spin_unlock_irqrestore(&roce_conn->conn.lock, flags);

				/* The only way to the rdma_cm to cancel the request is to destroy the id */

				/* if @cm is already owned by dev-cq via nvmeib_rdma_cm_owner_set(),
				 a nd the owner calls nvmeib_rdma_destroy_cm(), which calls free_cm()*,
				 but rdma_destroy_id() wont be called bcz roce_conn->cm_id = NULL */
				rdma_destroy_id(roce_conn->cm_id);
			}
			roce_conn->cm_id = NULL;
			roce_conn->conn.wait_for_drep = false;
			inv = true;
		} else if (cm->rdma_type == _rdma_lb)
			inv = lb_try_inv_cm(cm);
	}

out:
	NFOUT;
	return inv;
}
EXPORT_SYMBOL(nvmeib_rdma_try_inv_cm);

static struct nvmeib_rdma_cm *create_ib_cm(struct nvmeib_dev *dev)
{
	struct ib_rdma_connection *conn;
	struct ib_cm_id *cm_id;
	struct nvmeib_rdma_cm *cm = NULL;
	int rv;

	NFIN;
	if (!(conn = kzalloc(sizeof(*conn), GFP_KERNEL))) {
		_NE(error_nvmeib_rdma_create_ib_cm, "Failed to allocate IB connection");
		goto out;
	}
	cm_id = ib_create_cm_id(dev->ib_dev, ib_conn_cm_handler, conn);
	if (IS_ERR(cm_id)) {
		rv = PTR_ERR(cm_id);
		_NE(error_1_nvmeib_rdma_create_ib_cm, "Failed to create IB connection @RV", rv);
		goto free_conn;
	}

	conn->conn.dev = dev;
	conn->conn.cm.rdma_type = _rdma_ib;
	conn->conn.cm.cm_type = _cm_connection;
	conn->cm_id = cm_id;
	spin_lock_init(&conn->conn.lock);
	conn->state = NVMEIB_CM_CONN_INIT;
	cm = &conn->conn.cm;
	goto out;

free_conn:
	kfree(conn);

out:
	NFOUT;
	return cm;
}

static void complete_roce_conn_done(struct roce_rdma_connection *conn, int stat)
{
	NFIN;
	BUG_ON(!conn);
	_ND(trace_nvmeib_rdma_complete_roce_conn_done, "roce comp done on @CONN", conn);
	if (!conn->status)
		conn->status = stat;
	complete(&conn->done);

	NFOUT;
}

static int roce_conn_cm_handler(struct rdma_cm_id *cm_id,
	struct rdma_cm_event *event)
{
	struct roce_rdma_connection *conn = cm_id->context;
	struct nvmeib_rdma_event rdma_event = {0};
	int rv;

	NFIN;
	_NT(trace_nvmeib_rdma_roce_conn_cm_handler, "event @EVENT status @STATUS conn @CONTEXT id @CM_ID conn_done @BOOL handler @BOOL",
		event->event, event->status, cm_id->context, cm_id, !!completion_done(&conn->done), !!conn->conn.event_handler);

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		if ((rv = rdma_resolve_route(cm_id, 1000)) < 0) {
			_NE(roce_conn_cm_handler_e1, "Failed to initiate rdma_resolve_route() @INT", rv);
			complete_roce_conn_done(conn, rv);
		}
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		_ND(roce_conn_cm_handler_d1, "Route resolved");
		complete_roce_conn_done(conn, 0);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
		if (event->event == RDMA_CM_EVENT_UNREACHABLE) {
			_NT(roce_conn_cm_handler_t4, "Got unreachable on id @CM_ID", cm_id);
		} else {
			_NT(roce_conn_cm_handler_t1, "Route resolve failed: @STR on id @CM_ID",
				event->event == RDMA_CM_EVENT_ADDR_ERROR ?
				"no address" : "no route", cm_id);
		}
		/* If someone is waiting..just fail it */
		complete_roce_conn_done(conn, -ENETUNREACH);
		if (conn->conn.event_handler) {
			roce_event_2_nvmeib_event(event, &rdma_event);
			_NW(roce_conn_cm_handler_d2, "Forwarding nvmeib_rdma event @INT (rdma_cm event @INT)",
			   rdma_event.event, event->event);
			conn->conn.event_handler(conn->conn.context, &rdma_event);
		} else {
			//EC-5770:
			_NW(roce_conn_cm_handler_w0,
				"conn=@PTR: @STR error but event handler does not set",
				conn, event->event == RDMA_CM_EVENT_ADDR_ERROR ? "addr" : "route");
		}
		break;
	default:
		_NI(roce_conn_cm_handler_d3, "Completion done on event (@INT)", event->event);
		complete_roce_conn_done(conn, -1);
		if(!conn->temp) {
				/* to make the connection the same with RoCE we init the QP here */
			if (event->event == RDMA_CM_EVENT_CONNECT_RESPONSE ||
				event->event == RDMA_CM_EVENT_ESTABLISHED)
			{
				unsigned long flags;
				spin_lock_irqsave(&conn->conn.lock, flags);
				if (conn->state != NVMEIB_ROCE_CM_INIT) {
					/* Roce Conn has been invalidated (probably due to timeout) */
					_NT(roce_conn_cm_handler_t2, "Roce conn @PTR invalidated", conn);
					rdma_event.event = NVMEIB_REP_ERROR;
					rdma_event.status = -ECONNRESET;
					rdma_event.private_data = NULL;
					rdma_event.private_data_len = 0;
					spin_unlock_irqrestore(&conn->conn.lock, flags);
					goto fwd_event;
				}
				NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
						     nvmeib_cm_ephemeral, roce_conn_cm_handler_est_rcvd,
							conn, conn->state, NVMEIB_ROCE_CM_EST_RCVD);
				conn->state = NVMEIB_ROCE_CM_EST_RCVD;
				spin_unlock_irqrestore(&conn->conn.lock, flags);
			}
			if (event->event == RDMA_CM_EVENT_CONNECT_RESPONSE && !conn->cma_internal_qp) {
				mutex_lock(&conn->qp_guard);
				if (!conn->conn.qp) {
					_NI(trace_nvmeib_rdma_roce_conn_cm_handler_qp_free, "cm id: @CM_ID qp already freed",
					cm_id);
					mutex_unlock(&conn->qp_guard);
					return 0;
				}
				/* Need to manually bring the QP to RTS */
				if ((rv = roce_conn_qp_to_rts(conn, &event->param.conn)) ||
					(rv = roce_send_rtu(conn)))
				{
					_NT(roce_conn_cm_handler_t3, "Failed (@INT) to connect QP @INT32_HEX (@PTR)\n", rv,
						conn->conn.qp->qp_num, conn->conn.qp);
					ib_set_qp_err(conn->conn.qp);
					__nvmeib_rdma_reject_reason(conn->cm_id, NULL, 0, IB_CM_REJ_NO_QP);
					rdma_event.event = NVMEIB_CONNECT_ERROR;
					rdma_event.status = rv;
					rdma_event.private_data = NULL;
					rdma_event.private_data_len = 0;
					mutex_unlock(&conn->qp_guard);
					goto fwd_event;
				}
				mutex_unlock(&conn->qp_guard);
			}
			if (conn->cma_internal_qp && conn->srq) {
				if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
					/* RDMA_CM generates this event after it transitions the QP to RTS */
					conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;
				} else if (event->event == RDMA_CM_EVENT_CONNECT_ERROR) {
					/* We don't know what state the QP was in before the error occurred */
					conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN;
				}
			}
			roce_event_2_nvmeib_event(event, &rdma_event);
			rdma_event.private_data = (void *)event->param.conn.private_data;
			rdma_event.private_data_len = event->param.conn.private_data_len;

		fwd_event:
			_ND(roce_conn_cm_handler_d4, "Forwarding nvmeib_rdma event @INT (rdma_cm event @INT)",
			   rdma_event.event, event->event);
			conn->conn.event_handler(conn->conn.context, &rdma_event);
		}
		break;
	}
	NFOUT;
	return 0;
}

static struct nvmeib_rdma_cm *create_roce_cm(struct nvmeib_dev *dev, bool temp)
{
	struct roce_rdma_connection *conn;
	struct rdma_cm_id *cm_id;
	struct nvmeib_rdma_cm *cm = NULL;
	int rv;

	NFIN;
	if (!(conn = kzalloc(sizeof(*conn), GFP_KERNEL))) {
		_NE(error_nvmeib_rdma_create_roce_cm, "Failed to allocate RoCE connection");
		goto out;
	}
	cm_id = rdma_create_id(roce_conn_cm_handler, conn, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cm_id)) {
		rv = PTR_ERR(cm_id);
		_NE(error_1_nvmeib_rdma_create_roce_cm, "Failed to create RoCE connection @RV", rv);
		goto free_conn;
	}
#if RDMA_CM_HAS_SET_TIMEOUT
	rdma_set_timeout(cm_id, qp_timeout);
#endif
	conn->conn.dev = dev;
	conn->conn.cm.rdma_type = _rdma_roce;
	conn->conn.cm.cm_type = _cm_connection;
	conn->temp = temp;
	conn->cm_id = cm_id;
	mutex_init(&conn->qp_guard);
	spin_lock_init(&conn->conn.lock);
	init_completion(&conn->done);
	mutex_init(&conn->handler_guard);
	NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE", _T,
			     nvmeib_cm_ephemeral, create_roce_cm_init, conn, NVMEIB_ROCE_CM_INIT);
	conn->state = NVMEIB_ROCE_CM_INIT;
	cm = &conn->conn.cm;
	_ND(trace_nvmeib_rdma_create_roce_cm, "roce cm_id=@CM_ID", cm_id);
	goto out;

free_conn:
	kfree(conn);

out:
	NFOUT;
	return cm;
}

static int iwarp_conn_qp_to_rts(struct roce_rdma_connection *r_conn,
			       struct rdma_conn_param *conn_param);

static int iw_conn_cm_handler(struct iw_cm_id *cm_id,
	struct iw_cm_event *event)
{
	struct roce_rdma_connection *conn = cm_id->context;
	struct rdma_conn_param conn_param = {0};
	struct nvmeib_rdma_event rdma_event = {0};
	struct iw_cm_pd *iw_pd = event->private_data;
	int user_pd_len = event->private_data_len - offsetof(struct iw_cm_pd, user_pd);
	int rv;
	enum roce_conn_state next_conn_state_not_inv, next_conn_state_inv;
	unsigned long flags;

	NFIN;
	_NT(trace_nvmeib_rdma_iw_conn_cm_handler, "event @EVENT status @STATUS conn @CONTEXT id @CM_ID",
		event->event, event->status, cm_id->context, cm_id);

	BUG_ON(conn->conn.cm.rdma_type != _rdma_iwarp);

	/* [NVMESH-4168]: Lock handler guard and check state, if destroying exit
	  Prevents the handler from running while the cm_id is being destroyed.
	  Note: The iw_cm_id has its own refcount so we don't need another one */
	mutex_lock(&conn->handler_guard);
	spin_lock_irqsave(&conn->conn.lock, flags);
	if (conn->state == NVMEIB_ROCE_CM_DESTROYING) {
		spin_unlock_irqrestore(&conn->conn.lock, flags);
		goto unlock;
	}
	spin_unlock_irqrestore(&conn->conn.lock, flags);

	switch (event->event) {
	case IW_CM_EVENT_CONNECT_REPLY:
		if (event->status == 0) {
			/* Connect was successful
			 * If invalidate has not been requested, the next state is NVMEIB_ROCE_CM_EST_RCVD. */
			next_conn_state_not_inv = NVMEIB_ROCE_CM_EST_RCVD;
			/* If invalidate has been requested, then we will need to wait for IW_CM_EVENT_CLOSE before completing or we will get a use-after free */
			next_conn_state_inv = NVMEIB_ROCE_CM_INV_WAIT_CLOSE_EVENT;
		} else {
			/* Connect was successful
			 * If invalidate has not been requested, the next state is NVMEIB_ROCE_CM_CONN_ERR. */
			next_conn_state_not_inv = NVMEIB_ROCE_CM_CONN_ERR;
			/* If invalidate has been requested, then we have got the event we need to complete the invalidate done */
			next_conn_state_inv = NVMEIB_ROCE_CM_INV_GOT_EVENT;
		}
		spin_lock_irqsave(&conn->conn.lock, flags);
		if (conn->state == NVMEIB_ROCE_CM_INV_WAIT_EVENT) {
			NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
					     nvmeib_cm_ephemeral, iw_conn_hndl_state_wait_evt_conn_rep,
						conn, conn->state, next_conn_state_inv);
			conn->state = next_conn_state_inv;
			/* Main thread is already waiting */
			_NT(trace_iw_conn_cm_handler_got_inv_evt_after_wait,
				"got invalidate event (@EVENT status @STATUS) after wait - conn @CONTEXT id @CM_ID",
				event->event, event->status, cm_id->context, cm_id);
			if (next_conn_state_inv == NVMEIB_ROCE_CM_INV_GOT_EVENT) {
				if (conn->cancel_iwcm_conn_done)
					complete(conn->cancel_iwcm_conn_done);
			} else {
				/* Otherwise we must wait for the IW_CM_EVENT_CLOSE */
				_NT(trace_iw_conn_cm_handler_inv_wait_close,
					"conn @CONTEXT id @CM_ID - waiting for IW_CM_EVENT_CLOSE",
					cm_id->context, cm_id);
				conn->iw_cm_inv_stats.wait_close = true;
			}
			spin_unlock_irqrestore(&conn->conn.lock, flags);
			goto unlock;
		} else if (conn->state == NVMEIB_ROCE_CM_INV) {
			/* We got the event before main thread waits, so it will just continue  */
			NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
					     nvmeib_cm_ephemeral, iw_conn_hndl_state_inv_evt_conn_rep,
						conn, conn->state, next_conn_state_inv);
			conn->state = next_conn_state_inv;
			_NT(trace_iw_conn_cm_handler_got_inv_evt_before_wait,
				"got invalidate event (@EVENT status @STATUS) before wait - conn @CONTEXT id @CM_ID",
				event->event, event->status, cm_id->context, cm_id);
			spin_unlock_irqrestore(&conn->conn.lock, flags);
			goto unlock;
		} else if (conn->state != NVMEIB_ROCE_CM_INIT) {
			/* Unexpected conn_state / event */
			_NT(trace_iw_conn_cm_handler_got_unexp_evt,
				"got unexpected event (@EVENT status @STATUS) for conn state @STATE - conn @CONTEXT id @CM_ID",
				event->event, event->status, conn->state, cm_id->context, cm_id);
			spin_unlock_irqrestore(&conn->conn.lock, flags);
			BUG_ON(1);
			goto unlock;
		}
		NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
				     nvmeib_cm_ephemeral, iw_conn_hndl_state_not_inv_conn_rep, conn, conn->state, next_conn_state_not_inv);
		conn->state = next_conn_state_not_inv;
		spin_unlock_irqrestore(&conn->conn.lock, flags);
		if (event->status != 0) {
			conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN;
			if (event->status == -ECONNREFUSED) {
				rdma_event.event = NVMEIB_REJ_RECEIVED;
				if (event->private_data_len >= sizeof(struct iw_cm_pd_hdr)) {
					rdma_event.status = IB_CM_REJ_CONSUMER_DEFINED;
					if (conn->temp) {
						/* Find-path cm_id. Send the header to the handler */
						rdma_event.private_data = event->private_data;
						rdma_event.private_data_len = event->private_data_len;
					} else {
						/* Regular cm_id. Strip the header and pass to remaining data to handler */
						if (user_pd_len > 0) {
							rdma_event.private_data = &iw_pd->user_pd;
							rdma_event.private_data_len = user_pd_len;
						} else {
							_NT(trace_iw_conn_cm_handler_rej_no_user_data, "conn @CM_ID got reject with no user data", conn);
						}
					}
				} else {
					_NT(trace_iw_conn_cm_handler_rej_inv_data, "conn @CM_ID got reject with invalid data length @LENGTH",
					    conn, event->private_data_len);
					rdma_event.status = -EPROTO;
				}
				goto send_event;
			}
			rdma_event.event = NVMEIB_CONNECT_ERROR;
			rdma_event.status = event->status;
			goto send_event;
		}
		conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;
		/* Once iwarp QP is in RTS, we need to wait for IW_CM_EVENT_CLOSE (which we translate as DREP for ULP) */
		conn->conn.wait_for_drep = true;
		/* Get remote qpn from private data */
		conn->conn.remote_qpn = be32_to_cpu(iw_pd->hdr.rsp.qpn);
		if (false) {
			/* Not needed for SIW - Bring the QP to RTS */
			conn_param.responder_resources = event->ord;
			conn_param.initiator_depth = event->ird;
			if ((rv = iwarp_conn_qp_to_rts(conn, &conn_param))) {
				struct iw_cm_pd_hdr iw_pd_rej = {
					.rej = {
						.reason = IB_CM_REJ_NO_QP,
					},
				};
				_NT(__AUTOID__, "Failed (@RV) to connect QP @QP_NUM (@QP)", rv,
					conn->conn.qp->qp_num, conn->conn.qp);
				ib_set_qp_err(conn->conn.qp);
				iw_cm_reject(conn->iw_cm_id, &iw_pd_rej, sizeof(iw_pd_rej));
				rdma_event.event = NVMEIB_CONNECT_ERROR;
				rdma_event.status = rv;
				goto send_event;
			}
		}
		rv = 0;
		break;
	case IW_CM_EVENT_DISCONNECT:
		/* TBD Add a conn_state for this */
		spin_lock_irqsave(&conn->conn.lock, flags);
		if (conn->state != NVMEIB_ROCE_CM_EST_RCVD &&
			conn->state != NVMEIB_ROCE_CM_INV_WAIT_CLOSE_EVENT) {
			_NT(trace_iw_conn_cm_handler_got_unexp_evt_dsc,
				"got unexpected event (@EVENT status @STATUS) for conn state @STATE - conn @CONTEXT id @CM_ID",
				event->event, event->status, conn->state, cm_id->context, cm_id);
			BUG_ON(1);
		}
		spin_unlock_irqrestore(&conn->conn.lock, flags);
		break;
	case IW_CM_EVENT_CLOSE:
		spin_lock_irqsave(&conn->conn.lock, flags);
		if (conn->state == NVMEIB_ROCE_CM_INV_WAIT_CLOSE_EVENT) {
			NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
					     nvmeib_cm_ephemeral, iw_conn_hndl_state_wait_close_evt_close,
						conn, conn->state, NVMEIB_ROCE_CM_INV_GOT_EVENT);
			conn->state = NVMEIB_ROCE_CM_INV_GOT_EVENT;
			_NT(trace_iw_conn_cm_handler_got_inv_close_evt,
				"got invalidate close event (@EVENT status @STATUS) - conn @CONTEXT id @CM_ID",
				event->event, event->status, cm_id->context, cm_id);
			if (conn->cancel_iwcm_conn_done)
				complete(conn->cancel_iwcm_conn_done);
		} else {
			if (conn->state == NVMEIB_ROCE_CM_EST_RCVD) {
				NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE -> @STATE", _T,
						     nvmeib_cm_ephemeral, iw_conn_hndl_state_est_rcvd_evt_close,
							conn, conn->state, NVMEIB_ROCE_CM_IW_CM_CLOSE_RCVD);
				conn->state = NVMEIB_ROCE_CM_IW_CM_CLOSE_RCVD;
				_NT(trace_iw_conn_cm_handler_got_est_close_evt,
				    "got close event (@EVENT status @STATUS) - conn @CONTEXT id @CM_ID qp @QP",
				    event->event, event->status, cm_id->context, cm_id, conn->conn.qp);
			} else {
				_NW(warn_iw_conn_cm_handler_got_unexpected_close_evt,
				    "got unexpected close event (@EVENT status @STATUS) - conn @CONTEXT id @CM_ID qp @QP",
				    event->event, event->status, cm_id->context, cm_id, conn->conn.qp);
				BUG();
			}
		}
		spin_unlock_irqrestore(&conn->conn.lock, flags);
		/* For IW_CM_EVENT_CLOSE, send both DREQ and DREP events as when 
		   IWCM doesn't pass disconnect event to the cm handler */
		iw_send_close_events_with_handler(conn->conn.event_handler,
						  conn->conn.context,
						  event->status);
		goto unlock;
	default:
		_NE(t_01_iw_conn_cm_handler_got_inv_close_evt, "invalid iwarp event @INT\n", event->event);
	}

	iw_event_2_nvmeib_event(event, &rdma_event);
	rdma_event.private_data = &iw_pd->user_pd;
	rdma_event.private_data_len = user_pd_len;

send_event:
	_ND(__AUTOID__, "Forwarding nvmeib_rdma event @EVENT (rdma_cm event @EVENT)\n",
		rdma_event.event, event->event);
	conn->conn.event_handler(conn->conn.context, &rdma_event);

unlock:
	mutex_unlock(&conn->handler_guard);

	NFOUT;
	return 0;
}

static struct nvmeib_rdma_cm *create_iwarp_cm(struct nvmeib_dev *dev, bool temp)
{
	struct roce_rdma_connection *conn;
	struct iw_cm_id *cm_id = NULL;
	struct nvmeib_rdma_cm *cm = NULL;
	int rv;

	NFIN;
	if (!(conn = kzalloc(sizeof(*conn), GFP_KERNEL))) {
		_NE(error_nvmeib_rdma_create_iwarp_cm, "Failed to allocate iWARP connection");
		goto out;
	}
	if (temp && dev->iw_find_path_sock_priv) {
		struct find_path_sock_dev *sock_dev = dev->iw_find_path_sock_priv;
		struct find_path_sock_cep *cep;

		if (!nvmeib_ref_get(&sock_dev->ref_conn)) {
			_NT(trace_create_iwarp_cm_sock_dev_down,
			    "Device @IB_DEVICE is going down", dev->ib_dev->name);
			rv = -EBUSY;
			goto free_conn;
		}

		_NT(trace_1_create_iwarp_cm,
		    "Using socket for find-path from device @IB_DEVICE", dev->ib_dev->name);

		if (!(cep = kzalloc(sizeof(*cep), GFP_KERNEL))) {
			_NT(trace_2_create_iwarp_cm, "OOM");
			rv = -ENOMEM;
			goto dec_ref;
		}

		kref_init(&cep->ref);

		cep->initiator = true;
		cep->conn = conn;
		cep->state = FIND_PATH_EPSTATE_IDLE;
		spin_lock_init(&cep->lock);
		init_waitqueue_head(&cep->waitq);
		INIT_LIST_HEAD(&cep->work_free_list);

		INIT_TIMER(&cep->sock_shutdown_timer);
		cep->sock_shutdown_timer.function = find_path_shutdown_timer_fn;
		TIMER_SET_DATA(cep, sock_shutdown_timer, (unsigned long)cep);

		if ((rv = find_path_cep_alloc_work(cep, FIND_PATH_SOCK_CONN_MAX_WORK)) < 0) {
			_NT(trace_3_create_iwarp_cm, "OOM");
			rv = -ENOMEM;
			goto free_cep;
		}

		/* Parent backpointer reference */
		find_path_dev_get(sock_dev, "new-conn");
		cep->parent = sock_dev;

		find_path_cep_get(cep, "parent-cep-list");
		mutex_lock(&sock_dev->lock);
		list_add_tail(&cep->link, &sock_dev->sock_cep_list);
		mutex_unlock(&sock_dev->lock);

		find_path_cep_get(cep, "conn-cep-ptr");
		conn->find_path_sock_cep = cep;

		goto init_conn;

free_cep:
		find_path_cep_free_work(cep);
		kfree(cep);

dec_ref:
		nvmeib_ref_put(&sock_dev->ref_conn);
		goto free_conn;
	}

	cm_id = iw_create_cm_id(dev->ib_dev, iw_conn_cm_handler, conn);
	if (IS_ERR(cm_id)) {
		rv = PTR_ERR(cm_id);
		_NE(error_1_nvmeib_rdma_create_iwarp_cm, "Failed to create iWARP connection @RV", rv);
		/* Not using find-path-sock, no need to free cep */
		goto free_conn;
	}

init_conn:
	conn->conn.dev = dev;
	conn->conn.cm.rdma_type = _rdma_iwarp;
	conn->conn.cm.cm_type = _cm_connection;
	conn->temp = temp;
	conn->iw_cm_id = cm_id;
	mutex_init(&conn->qp_guard);
	spin_lock_init(&conn->conn.lock);
	init_completion(&conn->done);
	mutex_init(&conn->handler_guard);
	NVMEIB_LOG_EPHEMERAL("@CM_ID: @STATE", _T,
			     nvmeib_cm_ephemeral, create_iwarp_cm_init, conn, NVMEIB_ROCE_CM_INIT);
	conn->state = NVMEIB_ROCE_CM_INIT;
	cm = &conn->conn.cm;
	_ND(trace_nvmeib_rdma_create_iwarp_cm, "roce cm_id=@CM_ID", cm_id);
	goto out;

free_conn:
	kfree(conn);

out:
	NFOUT;
	return cm;
}

static struct nvmeib_rdma_cm *create_lb_cm(struct nvmeib_dev *dev)
{
	struct lb_rdma_connection *conn;
	struct nvmeib_rdma_cm *cm = NULL;

	NFIN;
	if (!(conn = kzalloc(sizeof(*conn), GFP_KERNEL))) {
		_NE(error_nvmeib_rdma_create_lb_cm, "Failed to allocate lb cm");
		goto out;
	}
	conn->conn.dev = dev;
	conn->conn.cm.rdma_type = _rdma_lb;
	conn->conn.cm.cm_type = _cm_connection;
	cm = &conn->conn.cm;
	spin_lock_init(&conn->conn.lock);
	conn->state = LB_CONN_IDLE;
	mutex_init(&conn->handler_mutex);
	mutex_init(&conn->qp_mutex);
	kref_init(&conn->refcount);
	conn->local_id = (u32)atomic_inc_return(&lb_id_cnt);

	_NT(trace_nvmeib_rdma_create_lb_cm, "Created lb conn @LOCAL_ID (@CONN)", conn->local_id, conn);

out:
	NFOUT;
	return cm;
}

/* create a connection descriptor */
struct nvmeib_rdma_cm *nvmeib_rdma_create_cm(struct nvmeib_dev *dev, int port,
	enum nvmeib_rdma_type type)
{
	struct nvmeib_rdma_cm *cm = NULL;

	NFIN;
	switch (type) {
	case _rdma_ib:
		cm = create_ib_cm(dev);
		break;
	case _rdma_roce:
		cm = create_roce_cm(dev, false);
		break;
	case _rdma_iwarp:
		cm = create_iwarp_cm(dev, false);
		break;
	case _rdma_lb:
		cm = create_lb_cm(dev);
		break;
	default:
		_NE(error_nvmeib_rdma_nvmeib_rdma_create_cm, "Invalid cm type @TYPE", type);
	}

	if (cm)
		_NI(trace_nvmeib_rdma_create_cm,
			"created cm=@CM, cm_type=@CM_TYPE, rdma_type=@RDMA_TYPE",
			cm, cm->cm_type, cm->rdma_type);

	NFOUT;
	return cm;
}
EXPORT_SYMBOL(nvmeib_rdma_create_cm);

#ifdef DEBUG_CM_OWNER
void nvmeib_rdma_cm_owner_set(struct nvmeib_rdma_cm *cm, int old, int new)
{
	if (cm->owner_pid != old) {
		_NE(nvmeib_rdma_cm_owner_set_e1, "cm=@PTR, owner_pid=@INT vs. old=@INT", cm, cm->owner_pid, old);
		BUG();
	}
	cm->owner_pid = new;
}
EXPORT_SYMBOL(nvmeib_rdma_cm_owner_set);

int nvmeib_rdma_cm_is_owner(struct nvmeib_rdma_cm *cm)
{
	/* Has no owner */
	if (!cm->owner_pid)
		return true;
	/* In interrupt so can't tell */
	if (!in_interrupt())
		return true;
	return cm->owner_pid == current->pid;
}
EXPORT_SYMBOL(nvmeib_rdma_cm_is_owner);

static void cm_owner_check(struct nvmeib_rdma_cm *cm)
{
	BUG_ON(!nvmeib_rdma_cm_is_owner(cm));
}
#else
#define cm_owner_check(cm)
#endif

/* destroy connection descriptor */
void nvmeib_rdma_destroy_cm(struct nvmeib_rdma_cm *cm)
{
	unsigned long start;

	NFIN;
	if (cm) {
		cm_owner_check(cm);
		start = jiffies;
		_NT(trace_nvmeib_rdma_nvmeib_rdma_destroy_cm, "Calling free_cm @CM", cm);
		free_cm(cm);
		_NT(trace_1_nvmeib_rdma_nvmeib_rdma_destroy_cm, "free cm on @CM took @DIFF_JIFFIES", cm, jiffies - start);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_rdma_destroy_cm);

static int roce_init_conn_qp(struct rdma_cm_id *cma_id, struct ib_qp *qp)
{
	struct ib_qp_attr qp_attr = {};
	int qp_attr_mask, ret;

	qp_attr.qp_state = IB_QPS_INIT;
	ret = rdma_init_qp_attr(cma_id, &qp_attr, &qp_attr_mask);
	if (ret) {
		_NE(error_roce_init_conn_qp, "Failed rdma_init_qp_attr (@RV) to init qp @QP_NUM (@QP)", ret, qp->qp_num, qp);
		return ret;
	}

	return ib_modify_qp(qp, &qp_attr, qp_attr_mask);
}

static int _ib_destroy_qp(struct ib_qp *qp) {
	int rv;

	if ((rv = ib_destroy_qp(qp))) {
		_NE_dmesg(_ib_destroy_qp_failed, "Failed to destory @QP_NUM (@QP) - RV=@RV", qp->qp_num, qp, rv);
		BUG_NON_PRODUCTION(472);
	}

	return rv;
}

static struct ib_qp* roce_alloc_qp(struct rdma_connection *conn,
	struct ib_pd *pd, struct ib_qp_init_attr *qp_init)
{
	struct ib_qp *qp = NULL;
	int stat_ret = 0;
	struct roce_rdma_connection *roce_cm_id = NULL;

	NFIN;
    if (!conn) {
		_NE(error_nvmeib_rdma_roce_alloc_qp, "expected connection element but got null");
		qp = NULL;
		goto out;
    }

    if (conn->cm.cm_type != _cm_connection) {
		_NE(error_1_nvmeib_rdma_roce_alloc_qp, "Expected connection object but got something else @CM_TYPE",
			conn->cm.cm_type);
		goto out;
    }

    if (conn->cm.rdma_type != _rdma_roce) {
		_NE(error_2_nvmeib_rdma_roce_alloc_qp, "Expected roce object but got other @RDMA_TYPE",
			conn->cm.rdma_type);
		goto out;
    }
	if (!(roce_cm_id = conn_2_roce(conn))) {
		_NE(error_3_nvmeib_rdma_roce_alloc_qp, "Invalid rdma_connection @CONN", conn);
		goto out;
	}

	if (!USE_CMA_INTERNAL_QP) {
		if (IS_ERR_OR_NULL(qp = ib_create_qp(pd, qp_init))) {
			_NT(trace_nvmeib_rdma_roce_alloc_qp, "Failed (@PTR_ERR) to create QP on device @DEVICE_NAME", PTR_ERR(qp), pd->device->name);
			qp = NULL;
			goto out;
		}
		if ((stat_ret = roce_init_conn_qp(roce_cm_id->cm_id, qp))) {
			_NE(error_4_nvmeib_rdma_roce_alloc_qp, "Failed (@STAT_RET) to init qp @QP_NUM (@QP)", stat_ret, qp->qp_num, qp);
			_ib_destroy_qp(qp);
			qp = NULL;
			goto out;
		}
		roce_cm_id->cma_internal_qp = false;
	} else {
		stat_ret = rdma_create_qp(roce_cm_id->cm_id, pd, qp_init);
		if (stat_ret) {
			_NE(error_5_nvmeib_rdma_roce_alloc_qp, "rdma_create_qp failed for cma_id @STAT_RET", stat_ret);
			qp = NULL;
		}
		else {
			qp = roce_cm_id->cm_id->qp;
			roce_cm_id->cma_internal_qp = true;
			roce_cm_id->srq = !!qp->srq;
			_ND(trace_1_nvmeib_rdma_roce_alloc_qp, "qp_num=@QP_NUM", qp->qp_num);
		}
	}

out:
	NFOUT;
	return qp;
}

/**
 * init_qp() - Initialize queue pair attributes.
 *
 * Initialized the attributes of queue pair 'qp' by allowing local write,
 * remote read and remote write. Also transitions 'qp' to state IB_QPS_INIT.
 *
 * [Gregory] - RH linux-3.10.0-693 does not have qp::port
 */
static int ib_init_qp(struct rdma_connection *conn, struct ib_pd *pd, struct ib_qp *qp,
	int port, u16 pkey, int qp_access)
{
	struct ib_qp_attr attr = {0};
	int rv;

	NFIN;
	if (!conn->listener &&
		rdma_port_get_link_layer(qp->device, port) == IB_LINK_LAYER_INFINIBAND) {
		if ((rv = ib_find_pkey(pd->device, port, pkey, &attr.pkey_index)) < 0) {
			_NE(error_nvmeib_rdma_ib_init_qp, "ib_find_key failed with error =@RV", rv);
			goto out;
		}
		else
			_ND(trace_nvmeib_rdma_ib_init_qp, "attr.pkey_index=@PKEY_INDEX", attr.pkey_index);
	}
	else
		attr.pkey_index = 0;

	attr.qp_state = IB_QPS_INIT;
	attr.qp_access_flags = qp_access;

	attr.port_num = port;

	if ((rv = ib_modify_qp(qp, &attr,
		IB_QP_STATE |
		IB_QP_ACCESS_FLAGS |
		IB_QP_PORT |
		IB_QP_PKEY_INDEX)) < 0)
		_NE(error_1_nvmeib_rdma_ib_init_qp, "ib_modify_qp() failed with error @RV", rv);

out:
	NFOUT;
	return rv;
}

static struct ib_qp* ib_alloc_qp(struct rdma_connection *conn,
	struct ib_pd *pd, struct ib_qp_init_attr *qp_init, int port, u16 pkey,
	int qp_acess)
{
	struct ib_qp *qp = NULL;
	int stat_ret = 0;

	NFIN;

	qp = ib_create_qp(pd, qp_init);
	if (IS_ERR(qp)) {
		stat_ret = PTR_ERR(qp);
		_NE(error_nvmeib_rdma_ib_alloc_qp, "failed to create_qp rv= @STAT_RET", stat_ret);
		qp = NULL;
		goto out;
	}

	if ((stat_ret = ib_init_qp(conn, pd, qp, port, pkey, qp_acess))) {
		_ib_destroy_qp(qp);
		qp = NULL;
	}

out:
	NFOUT;
	return qp;
}

/**
 * allocates a QP associated with the specified rdma_cm_id and
 * transitions it for sending and receiving. The actual
 * capabilities and properties of the created QP will be
 * returned to the user through the qp_init_attr parameter.
 *
 * @author yaron (4/29/2015)
 *
 * @param cm - connection identifier
 * @param pd - protection domaim for the qp
 * @param qp_init - initial qp attributes
 */
struct ib_qp* nvmeib_rdma_create_qp(struct nvmeib_rdma_cm *cm_id,
	struct ib_pd *pd, struct ib_qp_init_attr *qp_init, int port, u16 pkey,
	int qp_access)
{
	struct ib_qp *qp = NULL;
	struct rdma_connection *conn;

	NFIN;
	_NT(trace_nvmeib_rdma_create_qp, "create-qp with pkey=@PKEY", pkey);

	if (cm_id->cm_type != _cm_connection) {
		_NE(error_nvmeib_rdma_nvmeib_rdma_create_qp, "Trying to create QP on a non-connection descriptor");
		goto out;
	}
	else
		conn = cm_2_c(cm_id);
	switch (cm_id->rdma_type) {
	case _rdma_roce:
		qp = roce_alloc_qp(conn, pd, qp_init);
		break;
	case _rdma_ib:
	case _rdma_lb:
	case _rdma_lb_accept:
	case _rdma_iwarp:
		qp = ib_alloc_qp(conn, pd, qp_init, port, pkey, qp_access);
		break;
	default:
		BUG_ON(1);
	}

	conn->qp = qp;

out:
	NFOUT;
	return qp;
}
EXPORT_SYMBOL(nvmeib_rdma_create_qp);

static struct ib_qp *get_cm_qp(struct nvmeib_rdma_cm *cm_id)
{
	struct ib_qp *qp = NULL;
	struct rdma_connection *conn;
	if ((conn = cm_2_c(cm_id)))
		qp = conn->qp;

	return qp;
}

/**
 *  destroys a QP, will deallocate qp on cm_id in case of roce
 *
 * @author yaron (4/29/2015)
 *
 * @param cm_id - connection identifier
 * @param qp - qp, required only in infiniband case
 */
void nvmeib_rdma_destroy_qp(struct nvmeib_rdma_cm *cm_id,
	struct ib_qp *qp)
{
	NFIN;
	if (cm_id) {
		cm_owner_check(cm_id);
		NLINE;
		if (get_cm_qp(cm_id) == qp) {
			switch (cm_id->rdma_type) {
			case _rdma_ib:
			{
				_ib_destroy_qp(qp);
				break;
			}
			case _rdma_lb:
			case _rdma_lb_accept:
				lb_destroy_qp(cm_id, qp);
				break;
			case _rdma_roce:
			{
				struct roce_rdma_connection *roce_conn = cm_2_rocec(cm_id);
				if (roce_conn) {
					mutex_lock(&roce_conn->qp_guard);
					if (roce_conn->cm_id && roce_conn->cma_internal_qp) {
						rdma_destroy_qp(roce_conn->cm_id);
						roce_conn->cma_internal_qp = false;
					}
					else
						_ib_destroy_qp(qp);
					roce_conn->conn.qp = NULL;
					mutex_unlock(&roce_conn->qp_guard);
				}
				else
					_ib_destroy_qp(qp);
			}
			break;
			case _rdma_iwarp:
			{
				struct roce_rdma_connection *roce_conn = cm_2_rocec(cm_id);
				if (roce_conn) {
					_ib_destroy_qp(qp);
				}
			}
			break;
			default:
				_NE(error_nvmeib_rdma_nvmeib_rdma_destroy_qp, "got unsupported object rdma type, won't destroy qp");
			}
		}
		else
			_NE(error_1_nvmeib_rdma_nvmeib_rdma_destroy_qp, "input qp @QP is not the connection qp @QP",
				qp, cm_2_c(cm_id)->qp);
	}
	else {
		_ib_destroy_qp(qp);
	}

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_rdma_destroy_qp);

static int roce_conn_qp_to_rts(struct roce_rdma_connection *r_conn,
			       struct rdma_conn_param *conn_param)
{
	struct ib_qp_attr qp_attr;
	int qp_attr_mask, ret;
	struct ib_qp *qp = r_conn->conn.qp;
	struct rdma_cm_id *id = r_conn->cm_id;

	if (!qp || !id) {
		_NE(error_nvmeib_rdma_roce_conn_qp_to_rts, "NULL qp/id for roce_conn_id @R_CONN", r_conn);
		ret = -EINVAL;
		goto out;
	}

	/* Re-init QP, now that the cm_id has port, access, etc. attributes */
	if ((ret = roce_init_conn_qp(id, qp))) {
		_NE(error_1_nvmeib_rdma_roce_conn_qp_to_rts, "roce_init_conn_qp failed (@RET_INT) for cm_id @ID_PTR qp: @QP_NUM (@QP)",
		   ret, id, qp->qp_num, qp);
		goto out;
	}

	qp_attr.qp_state = IB_QPS_RTR;
	ret = rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
	if (ret) {
		_NE(error_2_nvmeib_rdma_roce_conn_qp_to_rts, "rdma_init_qp_attr failed (@RET_INT) for cm_id @ID_PTR", ret, id);
		goto out;
	}

	if (conn_param)
		qp_attr.max_dest_rd_atomic = conn_param->responder_resources;
	ret = ib_modify_qp(qp, &qp_attr, qp_attr_mask);
	if (ret) {
		_NE(error_3_nvmeib_rdma_roce_conn_qp_to_rts, "ib_modify_qp failed (@RET_INT) for qpn: @QP_NUM", ret, qp->qp_num);
		goto out;
	}
	if (qp->srq != NULL)
		r_conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;

	qp_attr.qp_state = IB_QPS_RTS;
	ret = rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
	if (ret) {
		_NE(error_4_nvmeib_rdma_roce_conn_qp_to_rts, "rdma_init_qp_attr failed (@RET_INT) for cm_id @ID_PTR", ret, id);
		goto out;
	}

	if (conn_param)
		qp_attr.max_rd_atomic = conn_param->initiator_depth;
	qp_attr_mask |= IB_QP_TIMEOUT;
	qp_attr.timeout = qp_timeout;
	qp_attr.retry_cnt = qp_retry_cnt;

	ret = ib_modify_qp(qp, &qp_attr, qp_attr_mask);
	if (ret) {
		_NE(error_5_nvmeib_rdma_roce_conn_qp_to_rts, "ib_modify_qp failed (@RET_INT) for qpn: @QP_NUM", ret, qp->qp_num);
	}

out:
	return ret;
}

static int roce_accept(struct nvmeib_rdma_cm *cm_id,
	struct rdma_conn_param *conn_params)
{
	int rv;
	struct roce_rdma_connection *r_conn = cm_2_rocec(cm_id);
	struct rdma_connection *conn;
	struct ib_qp *qp;

	NFIN;
	if (!r_conn) {
		_NE(error_nvmeib_rdma_roce_accept, "invalid cm_id @CM_ID", cm_id);
		rv = -EINVAL;
		goto out;
	}
	conn = &r_conn->conn;
	qp = r_conn->conn.qp;
	conn_params->flow_control = 0;

	if (!r_conn->cma_internal_qp) {
		if ((rv = roce_conn_qp_to_rts(r_conn, conn_params)))
			goto out;
	} else {
#if RDMA_CM_HAS_SET_TIMEOUT
		rdma_set_timeout(r_conn->cm_id, qp_timeout);
#endif
	}
	if ((rv = rdma_accept(r_conn->cm_id, conn_params) < 0))
	if (r_conn->cma_internal_qp && qp->srq != NULL) {
		if (rv < 0) {
			/* We don't know what state the QP was in when the error occurred */
			conn->expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN;
		} else
			conn->expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;
	}
	NFOUT;
out:
	return rv;
}

/*
 * Gregory
 * Do not call this function - iwarp_conn_qp_to_rts
 * SiW will fail ib_modify_qp RTR->RTS transitions
 */
static __attribute__ ((unused))
int iwarp_conn_qp_to_rts(struct roce_rdma_connection *r_conn,
			       struct rdma_conn_param *conn_param)
{
	struct ib_qp_attr qp_attr;
	int qp_attr_mask, ret;
	struct ib_qp *qp = r_conn->conn.qp;
	struct iw_cm_id *id = r_conn->iw_cm_id;

	if (!qp || !id) {
		_NE(error_nvmeib_rdma_iwarp_conn_qp_to_rts, "NULL qp/id for iwarp_conn @R_CONN", r_conn);
		ret = -EINVAL;
		goto out;
	}

	qp_attr.qp_state = IB_QPS_RTR;
	qp_attr.max_dest_rd_atomic = conn_param->responder_resources;

	qp_attr_mask = IB_QP_STATE | IB_QP_MAX_DEST_RD_ATOMIC;

	ret = ib_modify_qp(qp, &qp_attr, qp_attr_mask);
	if (ret) {
		_NE(error_1_nvmeib_rdma_iwarp_conn_qp_to_rts, "ib_modify_qp failed (@RET_INT) for qpn: @QP_NUM", ret, qp->qp_num);
		goto out;
	}
	if (qp->srq != NULL)
		r_conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;

	qp_attr.qp_state = IB_QPS_RTS;
	qp_attr.max_rd_atomic = conn_param->initiator_depth;
	qp_attr_mask = IB_QP_STATE | IB_QP_MAX_QP_RD_ATOMIC;

	ret = ib_modify_qp(qp, &qp_attr, qp_attr_mask);
	if (ret) {
		_NE(error_2_nvmeib_rdma_iwarp_conn_qp_to_rts, "ib_modify_qp failed (@RET_INT) for qpn: @QP_NUM", ret, qp->qp_num);
	}

out:
	return ret;
}

static int iwarp_accept(struct nvmeib_rdma_cm *cm_id,
	struct rdma_conn_param *conn_params)
{
	int rv;
	struct roce_rdma_connection *r_conn = cm_2_rocec(cm_id);
	struct rdma_connection *conn __attribute__((unused));
	struct ib_qp *qp;
	struct iw_cm_conn_param iw_conn_param;
	size_t pd_len = offsetof(struct iw_cm_pd, user_pd) + conn_params->private_data_len;
	struct iw_cm_pd *pd = NULL;

	NFIN;
	if (!(pd = kzalloc(pd_len, GFP_KERNEL))) {
		_NE(error_nvmeib_rdma_iwarp_accept_oom, "OOM error");
		rv = -ENOMEM;
		goto out;
	}
	if (!r_conn) {
		_NE(error_nvmeib_rdma_iwarp_accept, "invalid cm_id @CM_ID", cm_id);
		rv = -EINVAL;
		goto out;
	}
	conn = &r_conn->conn;
	qp = r_conn->conn.qp;

	pd->hdr.rsp.qpn = cpu_to_be32(qp->qp_num);
	memcpy(pd->user_pd, conn_params->private_data, conn_params->private_data_len);

	if (false) {
		/*
		 * SIW driver will transfer qp to RTS in siw_accepet
		 * which is called from iw_cm_accept.
		 * Keep this code for future iWARP devices
		 * that may need explicit QP state update
		 */
		if ((rv = iwarp_conn_qp_to_rts(r_conn, conn_params)))
			goto out;
	}

	iw_conn_param.ord = conn_params->initiator_depth;
	iw_conn_param.ird = conn_params->responder_resources;
	iw_conn_param.private_data = pd;
	iw_conn_param.private_data_len = pd_len;
	iw_conn_param.qpn = qp->qp_num;

	if ((rv = iw_cm_accept(r_conn->iw_cm_id, &iw_conn_param)) < 0) {
		r_conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN;
		_NE(error_5_nvmeib_rdma_iwarp_conn_qp_to_rts,
			"iw_cm_accept failed (@RET_INT) for qpn: @QP_NUM", rv, qp->qp_num);
		goto out;
	}
	r_conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;
	/* Once SIW QP is in RTS, we need to wait for IW_CM_EVENT_CLOSE before destroying CM. We translate this as DREP for ULP */
	r_conn->conn.wait_for_drep = true;
	NFOUT;
out:
	kfree(pd);
	return rv;
}

/**
 * qp_ready_to_receive() - Change the state of a qp to 'ready to
 * receive' (RTR).
 * @net: cthe queue pair.
 *
 * Returns zero upon success and a negative value upon failure.
 *
 * Note: currently a struct ib_qp_attr takes 136 bytes on a 64-bit system.
 * If this structure ever becomes larger, it might be necessary to allocate
 * it dynamically instead of on the stack.
 */
static int ib_qp_ready_to_receive(struct ib_cm_id *cm_id, struct ib_qp *qp,
	struct rdma_conn_param *conn_params)
{
	struct ib_qp_attr qp_attr = {0};
	int attr_mask;
	int rv;

	NFIN;
	qp_attr.qp_state = IB_QPS_RTR;
	if ((rv = ib_cm_init_qp_attr(cm_id, &qp_attr, &attr_mask)) < 0) {
		_NE(error_nvmeib_rdma_ib_qp_ready_to_receive, "ib_cm_init_qp_attr() failed with error @RV", rv);
		goto out;
	}

	//qp_attr.path_mtu = NVMEIB_IO_QP_MTU;
	qp_attr.max_dest_rd_atomic = conn_params->responder_resources;
	if ((rv = ib_modify_qp(qp, &qp_attr, attr_mask)) < 0)
		_NE(error_1_nvmeib_rdma_ib_qp_ready_to_receive, "ib_modify_qp() failed with error @RV", rv);

out:
	NFOUT;
	return rv;
}

/**
 * qp_rts() - Change the state of a client to 'ready to send'
 * (RTS).
 * @net: owner the queue pair.
 *
 * Returns zero upon success and a negative value upon failure.
 *
 * Note: currently a struct ib_qp_attr takes 136 bytes on a 64-bit system.
 * If this structure ever becomes larger, it might be necessary to allocate
 * it dynamically instead of on the stack.
 */
static int ib_qp_ready_to_send(struct ib_cm_id *cm_id, struct ib_qp *qp,
	struct rdma_conn_param *conn_params)
{
	struct ib_qp_attr qp_attr = {0};
	int attr_mask;
	int rv;

	NFIN;
	qp_attr.qp_state = IB_QPS_RTS;
	if ((rv = ib_cm_init_qp_attr(cm_id, &qp_attr, &attr_mask)) < 0) {
		_NE(error_nvmeib_rdma_ib_qp_ready_to_send, "ib_cm_init_qp_attr() failed with error @RV", rv);
		goto out;
	}
	qp_attr.max_rd_atomic = conn_params->initiator_depth;
	attr_mask |= IB_QP_TIMEOUT;
	qp_attr.timeout = qp_timeout;
	qp_attr.retry_cnt = qp_retry_cnt;
	if ((rv = ib_modify_qp(qp, &qp_attr, attr_mask)) < 0)
		_NE(error_1_nvmeib_rdma_ib_qp_ready_to_send, "ib_modify_qp() failed with error@RV", rv);

out:
	NFOUT;
	return rv;
}

static int ib_accept_login(struct ib_cm_id *cm_id, struct ib_qp *qp,
	struct rdma_conn_param *conn_params)
{
	struct ib_cm_rep_param rep = {0};
	int rv;

	NFIN;
	rep.qp_num = qp->qp_num;
	rep.private_data = conn_params->private_data;
	rep.private_data_len = conn_params->private_data_len;
	rep.responder_resources = conn_params->responder_resources;
	rep.initiator_depth = conn_params->initiator_depth;
	rep.failover_accepted = 0;
	rep.flow_control = conn_params->flow_control;
	rep.rnr_retry_count = conn_params->rnr_retry_count;
	rep.srq = conn_params->srq ? 1 : 0;
	if ((rv = ib_send_cm_rep(cm_id, &rep)) < 0) {
		_NT(error_nvmeib_rdma_ib_accept_login, "Sending NVMEIB_LOGIN_REQ response failed (error code = @RV)", rv);
	}
	NFOUT;
	return rv;
}

static int ib_accept(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp,
	struct rdma_conn_param *conn_params)
{
	int rv = 0;
	struct ib_rdma_connection *ib_conn = cm_2_ibc(cm_id);

	NFIN;

    if (!ib_conn) {
		rv = -EINVAL;
		goto out;
    }

	if ((rv = ib_qp_ready_to_receive(ib_conn->cm_id, qp, conn_params)) < 0)
		goto out;

	if (qp->srq != NULL)
		ib_conn->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;

	if ((rv = ib_qp_ready_to_send(ib_conn->cm_id, qp, conn_params)) < 0)
		goto out;

	rv = ib_accept_login(ib_conn->cm_id, qp, conn_params);

out:
	if (rv < 0)
		_NE(error_nvmeib_rdma_ib_accept, "Failed to accept connection");
	NFOUT;
	return rv;
}

int nvmeib_rdma_accept(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp,
	struct rdma_conn_param *conn_params)
{
	int rv = -EINVAL;

	NFIN;
	switch (cm_id->rdma_type) {
	case _rdma_ib:
		rv = ib_accept(cm_id, qp, conn_params);
		break;
	case _rdma_roce:
		conn_params->qp_num = qp->qp_num;
		conn_params->srq = qp->srq ? 1 : 0;
		rv = roce_accept(cm_id, conn_params);
		break;
	case _rdma_iwarp:
		rv = iwarp_accept(cm_id, conn_params);
		break;
	case _rdma_lb_accept:
		rv = lb_accept(cm_id, qp, conn_params);
		break;
	case _rdma_lb:
		rv = -EINVAL;
		break;
	}

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_accept);


/**
 * qp_err() - Set the client queue pair state to 'error'.
 */
static int ib_set_qp_err(struct ib_qp *qp)
{
	struct ib_qp_attr qp_attr = {0};
	int rv;

	NFIN;
	qp_attr.qp_state = IB_QPS_ERR;
	if ((rv = ib_modify_qp(qp, &qp_attr, IB_QP_STATE)) < 0) {
		struct ib_qp_attr qp_attr = {0};
		struct ib_qp_init_attr qp_init_attr = {0};
		_NE(error_nvmeib_rdma_ib_set_qp_err, "ib_modify_qp() qp: @QP failed with error @RV", qp, rv);
		rv = ib_query_qp(qp, &qp_attr, IB_QP_STATE, &qp_init_attr);
		_NT(trace_nvmeib_rdma_ib_set_qp_err, "ib_query_qp returned (@RV) qp: @QP in state: @CUR_QP_STATE", rv, qp, qp_attr.cur_qp_state);
		WARN_ON_ONCE(qp_attr.cur_qp_state != IB_QPS_RESET);
	}
	NFOUT;
	return rv;
}

static int ib_disconnect(struct ib_rdma_connection *ibc, struct ib_qp *qp)
{
	struct ib_cm_id *cm_id = ibc->cm_id;
	int rv;

	NFIN;
	if (qp) {
		if ((rv = ib_set_qp_err(qp)) < 0) {
			_NE(error_nvmeib_rdma_ib_disconnect, "could not set qp err");
			goto out;
		}
	}
	else
		rv = 0;
	/* initiate or respond to a disconnect */
	ibc->conn.wait_for_drep = true;
	if (ib_send_cm_dreq(cm_id, NULL, 0)){
		_ND(trace_nvmeib_rdma_ib_disconnect, "Failed to send cm dreq, send drep");
		ibc->conn.wait_for_drep = false;
		if (ib_send_cm_drep(cm_id, NULL, 0))
			_ND(trace_1_nvmeib_rdma_ib_disconnect, "Failed to send cm drep");
	}

out:
	NFOUT;
	return rv;
}

static int roce_disconnect(struct nvmeib_rdma_cm *cm, struct ib_qp *qp)
{
	struct roce_rdma_connection *roce_conn = cm_2_rocec(cm);
	int rv = 0;

	if (!roce_conn) {
		rv = -EINVAL;
		goto out;
	}

	if (!roce_conn->cma_internal_qp) {
		if ((rv = ib_set_qp_err(qp)))
			goto out;
	}
	if (roce_conn->cm_id)
		rv = rdma_disconnect(roce_conn->cm_id);
	else
		_NT(trace_nvmeib_rdma_roce_disconnect, "roce_conn @ROCE_CONN has NULL rdma_cm_id", roce_conn);
out:
	return rv;
}

static int iwarp_disconnect(struct nvmeib_rdma_cm *cm, struct ib_qp *qp)
{
	struct roce_rdma_connection *roce_conn = cm_2_rocec(cm);
	enum ib_qp_state prev_qp_state = IB_QPS_ERR;
	unsigned long flags;
	int rv = 0;

	if (!roce_conn) {
		rv = -EINVAL;
		goto out;
	}

	if (roce_conn->find_path_sock_cep) {
		iwarp_disconnect_sock_only(roce_conn->find_path_sock_cep);
		goto out;
	}

	if (roce_conn->conn.expect_last_wqe == NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN) {
		/* Query QP to determine whether to Expect Last WQE or not */
		struct ib_qp_attr qp_attr = {};
		prev_qp_state = qp_attr.cur_qp_state;
	}

	/* The iw_cm complains if we call iw_cm_disconnect and the QP is already NULL
	 * The QP is nullified on IW_CM_EVENT_CLOSE */
	spin_lock_irqsave(&roce_conn->conn.lock, flags);
	if (roce_conn->iw_cm_id && roce_conn->state == NVMEIB_ROCE_CM_EST_RCVD) {
		spin_unlock_irqrestore(&roce_conn->conn.lock, flags);
		/* TBD: Add support for SQ Drained and change the abrupt flag to 0 */
		if ((rv = iw_cm_disconnect(roce_conn->iw_cm_id, 1))) {
			_NW(warn_nvmeib_rdma_iwarp_disconnect,
					"iwarp_conn @ROCE_CONN - iw_cm_disconnect failed (@RV)",
									roce_conn, rv);
			goto out;
		}
	} else {
		spin_unlock_irqrestore(&roce_conn->conn.lock, flags);
		_NT(trace_nvmeib_rdma_iwarp_disconnect,
				"iwarp_conn @ROCE_CONN has NULL rdma_cm_id", roce_conn);
		if ((rv = ib_set_qp_err(qp))) {
			_NW(warn_nvmeib_rdma_iwarp_disconnect_2,
					"iwarp_conn @ROCE_CONN - could not set QP @QPN (@QP) to error",
					roce_conn, qp->qp_num, qp);
			goto out;
		}
	}
	if (roce_conn->conn.expect_last_wqe == NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN) {
		roce_conn->conn.expect_last_wqe =
				((prev_qp_state == IB_QPS_RTS || prev_qp_state == IB_QPS_RTR) ?
						NVMEIB_RDMA_EXPECT_LAST_WQE : NVMEIB_RDMA_DONT_EXPECT_LAST_WQE);
		roce_conn->conn.wait_for_drep = prev_qp_state == IB_QPS_RTS;
	}
out:
	return rv;
}

/* send disconnet */
int nvmeib_rdma_disconnect(struct nvmeib_rdma_cm *cm, struct ib_qp *qp,
	bool *wait_for_drep)
{
	struct rdma_connection *conn = cm_2_c(cm);
	int rv = 0;

	NFIN;
	if (!conn) {
		_NE(error_1_nvmeib_rdma_nvmeib_rdma_disconnect, "cm @CM - invalid rdma connection", cm);
		rv = -EINVAL;
		goto out;
	}
	switch (cm->rdma_type) {
	case _rdma_roce:
		rv = roce_disconnect(cm, qp);
		break;
	case _rdma_iwarp:
		rv = iwarp_disconnect(cm, qp);
		break;
	case _rdma_ib:
		rv = ib_disconnect(cm_2_ibc(cm), qp);
		break;
	case _rdma_lb:
	case _rdma_lb_accept:
		rv = lb_disconnect(cm, qp);
		break;
	default:
		_NE(error_nvmeib_rdma_nvmeib_rdma_disconnect, "cm @CM - invalid rdma_type @RDMA_TYPE", cm, cm->rdma_type);
		rv = -EINVAL;
		goto out;
	}
	if (wait_for_drep) {
		*wait_for_drep = conn->wait_for_drep;
	}
out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_disconnect);

enum nvmeib_rdma_expect_last_wqe nvmeib_rdma_expect_last_wqe(struct nvmeib_rdma_cm* cm)
{
	struct rdma_connection *conn = cm_2_c(cm);
	enum nvmeib_rdma_expect_last_wqe ret = NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN;

	if (conn)
		ret = conn->expect_last_wqe;

	return ret;
}
EXPORT_SYMBOL(nvmeib_rdma_expect_last_wqe);

bool nvmeib_rdma_wait_for_drep(struct nvmeib_rdma_cm* cm)
{
	struct rdma_connection *conn = cm_2_c(cm);
	bool ret = false;

	if (conn)
		ret = conn->wait_for_drep;

	return ret;
}
EXPORT_SYMBOL(nvmeib_rdma_wait_for_drep);

int nvmeib_rdma_get_remote_qpn(struct nvmeib_rdma_cm *cm, u32 *remote_qpn)
{
	struct rdma_connection *conn = cm_2_c(cm);
	int rv;

	if (!conn->remote_qpn) {
		struct ib_qp_attr qp_attr = {};
		struct ib_qp_init_attr qp_init_attr = {};

		/* remote_qpn field is not set, query QP */
		if ((rv = ib_query_qp(conn->qp, &qp_attr, IB_QP_DEST_QPN, &qp_init_attr)) < 0) {
			_NE(error_nvmeib_rdma_get_remote_qpn_query_qp, "ib_query_qp failed (@RV) for QP @QP_NUM (@QP)", rv, conn->qp->qp_num, conn->qp);
			goto out;
		}
		if (!(conn->remote_qpn = qp_attr.dest_qp_num)) {
			_NE(error_nvmeib_rdma_get_remote_qpn_zero, "QP @QP_NUM (@QP) has invalid dest_qp_num", conn->qp->qp_num, conn->qp);
			rv = -ENOENT;
			goto out;
		}
	}

	*remote_qpn = conn->remote_qpn;
	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_get_remote_qpn);

static int roce_read_gids(struct roce_rdma_connection *conn,
		   union ib_gid *sgid,
		   union ib_gid *dgid)
{
	rdma_read_gids(conn->cm_id, sgid, dgid);
	return 0;
}

static void inet_sa_to_gid(const struct sockaddr *sa, union ib_gid *gid)
{
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *sin = (void *)sa;
		struct in6_addr *gid_addr6 = (void *)gid;

		gid_addr6->s6_addr32[0] = 0;
		gid_addr6->s6_addr32[1] = 0;
		gid_addr6->s6_addr32[2] = htonl(0xffff);
		gid_addr6->s6_addr32[3] = sin->sin_addr.s_addr;
	} else {
		struct sockaddr_in6 *sin6 = (void *)sa;
		BUG_ON(sa->sa_family != AF_INET6);
		memcpy(gid, &sin6->sin6_addr, sizeof(*gid));
	}
}

static int iwarp_read_gids(struct roce_rdma_connection *conn,
		    union ib_gid *sgid,
		    union ib_gid *dgid)
{
	struct iw_cm_id *iw_cm_id = conn->iw_cm_id;
	if (sgid)
		inet_sa_to_gid((struct sockaddr *)&iw_cm_id->m_local_addr, sgid);
	if (dgid)
		inet_sa_to_gid((struct sockaddr *)&iw_cm_id->m_remote_addr, dgid);
	return 0;
}

static int ib_read_gids(struct ib_rdma_connection *conn,
			   union ib_gid *sgid,
			   union ib_gid *dgid)
{
	if (sgid)
		*sgid = conn->sgid;
	if (dgid)
		*dgid = conn->dgid;
	return 0;
}

static int lb_read_gids(struct lb_rdma_connection *conn,
			union ib_gid *sgid,
			union ib_gid *dgid)
{
	struct lb_rdma_listener *lbl = l_2_lb(conn->conn.listener);
	int rv;

	if (!lbl) {
		rv = -EINVAL;
		goto out;
	}

	/* Loopback, so both GIDs are the same */
	if (sgid)
		*sgid = lbl->gid;
	if (dgid)
		*dgid = lbl->gid;
	rv = 0;

out:
	return rv;
}

int nvmeib_rdma_read_gids(struct nvmeib_rdma_cm *cm, union ib_gid *sgid,
			  union ib_gid *dgid)
{
	struct rdma_connection *conn = cm_2_c(cm);
	int rv;

	NFIN;
	if (!conn) {
		_NE(error_nvmeib_rdma_read_gids_inv_conn, "cm @CM - invalid rdma connection", cm);
		rv = -EINVAL;
		goto out;
	}
	switch (cm->rdma_type) {
	case _rdma_roce:
		rv = roce_read_gids(conn_2_roce(conn), sgid, dgid);
		break;
	case _rdma_iwarp:
		rv = iwarp_read_gids(conn_2_roce(conn), sgid, dgid);
		break;
	case _rdma_ib:
		rv = ib_read_gids(conn_2_ib(conn), sgid, dgid);
		break;
	case _rdma_lb:
	case _rdma_lb_accept:
		rv = lb_read_gids(conn_2_lb(conn), sgid, dgid);
		break;
	default:
		_NE(error_nvmeib_rdma_read_gids_inv_, "cm @CM - invalid rdma_type @RDMA_TYPE", cm, cm->rdma_type);
		rv = -EINVAL;
		goto out;
	}

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_read_gids);

/* send reply to peer disconnet */
int nvmeib_rdma_disconnect_reply(struct nvmeib_rdma_cm *cm)
{
	int rv = 0;

	NFIN;
	if (cm->rdma_type == _rdma_ib)
		rv = ib_send_cm_drep(cm_2_ibc(cm)->cm_id, NULL, 0);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_disconnect_reply);

/* notify about qp events */
int nvmeib_rdma_notify(struct nvmeib_rdma_cm *cm, enum ib_event_type event)
{
	int rv;

	NFIN;
	switch (cm->rdma_type) {
	case _rdma_ib:
		rv = ib_cm_notify(cm_2_ibc(cm)->cm_id, event);
		break;
	case _rdma_roce:
	{
		struct roce_rdma_connection *roce_conn = cm_2_rocec(cm);
		if (roce_conn && roce_conn->cm_id)
			rv = rdma_notify(roce_conn->cm_id, event);
		else
			rv = -EINVAL;
		break;
	}
	case _rdma_iwarp:
		/* Not necessary for iwarp */
		rv = 0;
		break;
	case _rdma_lb:
		/* Not necessary for loopback */
		rv = 0;
		break;
	default:
		rv = -EINVAL;
	}
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_notify);

struct ib_path_completion {
	/* the path to set when we find one */
	struct ib_sa_path_rec *path;
	/* the status of the op */
	int status;
	/* a completion for path search op */
	struct completion *done;
};

#if KS_IB_SA_PATH_REC_GET_CB_HAS_NUM_PRS
static void ib_path_rec_comp(int status,
			     struct ib_sa_path_rec *pathrec,
#if KS_IB_SA_PATH_REC_GET_CB_HAS_NUM_PRS_UINT
			     unsigned int num_prs __attribute__((unused)),
#else
			     int num_prs __attribute__((unused)),
#endif
			     void *priv)
#else
static void ib_path_rec_comp(int status,
	struct ib_sa_path_rec *pathrec, void *priv)
#endif
{
	struct ib_path_completion *pc = priv;

	NFIN;
	pc->status = status;
	if (unlikely(status))
		_NT(trace_nvmeib_rdma_ib_path_rec_comp, "Got failed path rec status @STATUS", status);
	else if (unlikely(!pathrec))
		_NE(error_nvmeib_rdma_ib_path_rec_comp, "Got path rec status @STATUS but path-record is NULL", status);
	else
		*pc->path = *pathrec;
	complete(pc->done);
	NFOUT;
}

enum {
	NVMEIBC_IB_CHANNEL_PATH_REC_TIMEOUT_MS	= 1000,
};

static int ib_find_path(struct ib_rdma_connection *ib_conn,
	struct nvmeib_rdma_path_info *info)
{
	int path_query_id;
	struct ib_sa_query *path_query;
	DECLARE_COMPLETION_ONSTACK(path_done);
	struct ib_path_completion pc;
	int rv = 0;

	NFIN;

	if (!nvmeib_ib_cross_subnet_enb &&
		info->path->sgid.global.subnet_prefix !=
		info->path->dgid.global.subnet_prefix) {
		_NI(trace_0_ib_find_path,
			"Skip find-path, cross subnet @SGID -> @DGID",
			&info->path->sgid, &info->path->dgid);
		rv = -EINVAL;
		goto out;
	}

	if (ib_conn) {
		ib_conn->path = *info->path;
		ib_conn->service_id = info->service_id;
		ib_conn->pkey = info->pkey;
		goto out;
	}
	_NT(trace_nvmeib_rdma_ib_find_path, "Looking for an access: s=@SGID, d=@DGID sid=@SID, pkey=@PKEY",
	   &info->path->sgid, &info->path->dgid,
	   be64_to_cpu(info->path->service_id),
	   be16_to_cpu(info->path->pkey));

	info->path->numb_path = 1;
	pc.path = info->path;
	pc.done = &path_done;

	path_query_id = ib_sa_path_rec_get(info->sa, info->dev->ib_dev,
			info->src_port, info->path,
			IB_SA_PATH_REC_SERVICE_ID | IB_SA_PATH_REC_DGID |
			IB_SA_PATH_REC_SGID | IB_SA_PATH_REC_NUMB_PATH | IB_SA_PATH_REC_PKEY,
			NVMEIBC_IB_CHANNEL_PATH_REC_TIMEOUT_MS,
#if IB_SA_PATH_REC_GET_HAS_RETRIES
			0,
#endif
			GFP_KERNEL,
			ib_path_rec_comp, &pc, &path_query);

	if (path_query_id < 0) {
		_NT(trace_1_nvmeib_rdma_ib_find_path, "failed to try lookup host path rv=@PATH_QUERY_ID", path_query_id);
		rv = path_query_id;
		goto out;
	}

	_NT(trace_2_nvmeib_rdma_ib_find_path, "Wait for completion on host lookup path (priv:@PC)...", &pc);
	wait_for_completion(&path_done);

	if ((rv = pc.status))
		_NT(trace_3_nvmeib_rdma_ib_find_path, "Find path failed with status @STATUS", pc.status);
	else
		_ND(trace_4_nvmeib_rdma_ib_find_path, "There is a path");

out:
	NFOUT;
	return rv;
}

struct roce_path_completion {
	/* the status of the op */
	int status;
	/* a completion for path search op */
	struct completion *done;
};

static int is_valid_roce_ipv4(union ib_gid *g)
{
	return (!g->global.subnet_prefix &&
		((g->global.interface_id & 0xffffffff) == 0xffff0000));
}

static int roce_find_path(struct roce_rdma_connection *roce_conn,
	struct nvmeib_rdma_path_info *info)
{
	struct roce_rdma_connection *conn = roce_conn;
	int rv, rv_w, conn_status = 0;
	char sgid[GUID_SIZE] = {0};
	char dgid[GUID_SIZE] = {0};

	NFIN;
	if (!conn) {
		struct nvmeib_rdma_cm *conn_cm = create_roce_cm(info->dev, true);
		if (!conn_cm) {
			_NE(error_nvmeib_rdma_roce_find_path, "Failed to allocate memory");
			rv = -ENOMEM;
			goto out;
		}
		conn = cm_2_rocec(conn_cm);
	}

	memset(&conn->dst_addr, 0, sizeof(conn->dst_addr));

	if (is_valid_roce_ipv4(&info->path->sgid) &&
		is_valid_roce_ipv4(&info->path->dgid)) {
		conn->dst_addr.s.sin_family = AF_INET;
		conn->dst_addr.s.sin_port = cpu_to_be16(info->service_port);
		conn->dst_addr.s.sin_addr.s_addr =
			(u32)(info->path->dgid.global.interface_id >> 32);

		conn->src_addr.s.sin_family = AF_INET;
		conn->src_addr.s.sin_port = 0;
		conn->src_addr.s.sin_addr.s_addr =
			(u32)(info->path->sgid.global.interface_id >> 32);
		_NT(trace_nvmeib_rdma_roce_find_path, "inet src=@SRC_PTR dst=@DST_PTR",
			&conn->src_addr.s.sin_addr.s_addr,
			&conn->dst_addr.s.sin_addr.s_addr);
	} else {
		conn->dst_addr.s6.sin6_family = AF_INET6;
		conn->dst_addr.s6.sin6_port = cpu_to_be16(info->service_port);
		/* this is wrong but we need to know how to extract the ip from the gid */
		memcpy(&conn->dst_addr.s6.sin6_addr, &info->path->dgid, 16);
		conn->src_addr.s6.sin6_family = AF_INET6;
		conn->src_addr.s6.sin6_port = 0;
		memcpy(&conn->src_addr.s6.sin6_addr,  &info->path->sgid, 16);
		_NT(trace_nvmeib_rdma_roce_find_path_v6, "inet src=@SRC_PTR_V6 dst=@DST_PTR_V6",
		    &conn->src_addr.s6.sin6_addr, &conn->dst_addr.s6.sin6_addr);
	}
	//todo need use a specific source address to force the routing
	format_gid_raw(info->path->sgid.raw, sgid);
	format_gid_raw(info->path->dgid.raw, dgid);
	_NT(trace_1_nvmeib_rdma_roce_find_path, "Looking for an access: s=@SGID_STR, d=@DGID_STR", sgid, dgid);
	if ((conn->status = rdma_resolve_addr(conn->cm_id,
		(struct sockaddr *)&conn->src_addr,
		(struct sockaddr *)&conn->dst_addr, NVMEIB_WAIT_FOR_EVENT)) < 0) {
		_NW(error_2_nvmeib_rdma_roce_find_path, "rdma_resolve_addr failed: @STATUS", conn->status);
		conn_status = conn->status;
		goto free_cm_id;
	}

	/* wait for addr (then route) resolve handler */
	_ND(trace_2_nvmeib_rdma_roce_find_path, "waiting comp done on @CONN", conn);
	if ((rv_w = wait_for_completion_interruptible_timeout(&conn->done,
		NVMEIB_WAIT_FOR_EVENT * 2)) <= 0) {
		_NW(trace_4_nvmeib_rdma_roce_find_path, "find-path failed (@RV) to wait for completion", rv_w);
		conn_status = rv_w ?: -ETIMEDOUT;
	} else {
		_NT(trace_5_nvmeib_rdma_roce_find_path, "find-path completed with status @STATUS", conn->status);
		conn_status = conn->status;
	}

free_cm_id:
	if (conn->temp)
		nvmeib_rdma_destroy_cm(&conn->conn.cm);
	rv = conn_status;

out:
	NFOUT;
	return rv;
}

static int iwarp_find_path_handler(void *context, struct nvmeib_rdma_event *event)
{
	struct roce_rdma_connection *conn = context;
	switch (event->event) {
	case NVMEIB_REJ_RECEIVED:
	{
		if (event->status == IB_CM_REJ_CONSUMER_DEFINED) {
			struct iw_cm_pd *iw_pd = event->private_data;
			if (iw_pd && iw_pd->hdr.rej.flags & IW_CM_REJ_FIND_PATH) {
				/* Find-path response received */
				_NT(trace_iwarp_find_path_handler_resp_ok,
					"Conn @CM_ID - Got find-path response on path src=@SRC_PTR dst=@DST_PTR",
					conn,
					&conn->src_addr.s.sin_addr.s_addr,
					&conn->dst_addr.s.sin_addr.s_addr);
				conn->status = 0;
			} else {
				_NW(warn_iwarp_find_path_handler_unexp_rej,
					"Conn @CM_ID - Invalid find-path response on path src=@SRC_PTR dst=@DST_PTR",
					conn,
					&conn->src_addr.s.sin_addr.s_addr,
					&conn->dst_addr.s.sin_addr.s_addr);
				conn->status = -EINVAL;
			}
		} else {
			_NT(trace_iwarp_find_path_handler_resp_err,
				"Conn @CM_ID - Got find-path response error @STATUS on path src=@SRC_PTR dst=@DST_PTR",
				conn, event->status,
				&conn->src_addr.s.sin_addr.s_addr,
				&conn->dst_addr.s.sin_addr.s_addr);
			conn->status = -EPROTO;
		}
		break;
	}
	case NVMEIB_CONNECT_ERROR:
		if (event->status == -ECANCELED) {
			_NT(trace_iwarp_find_path_handler_timeout,
				"Conn @CM_ID - Timed out finding path src=@SRC_PTR dst=@DST_PTR",
				conn, &conn->src_addr.s.sin_addr.s_addr,
				&conn->dst_addr.s.sin_addr.s_addr);
			conn->status = -ETIMEDOUT;
		} else {
			_NW(warn_iwarp_find_path_handler_2, "Conn @CM_ID - Unexpected NVMEIB_CONNECT_ERROR status @RV",
			    conn, event->status);
			conn->status = event->status;
		}
		break;
	default:
		/* Other events shouldn't happen in find-path. */
		_NW(warn_iwarp_find_path_handler,
			"Conn @CM_ID - Unexpected event @EVENT in find_path", conn, event->event);
		BUG_ON(1);
		conn->status = -EINVAL;
		break;
	}
	complete(&conn->done);
	return 0;
}

static int iwarp_find_path(struct roce_rdma_connection *conn,
	struct nvmeib_rdma_path_info *info)
{
	char src_addr_str[40] = "", dst_addr_str[48] = "";
	int rv;

	NFIN;

	/* For now, we don't do the path lookup, just return no error and (if the conn is not NULL),
	 * copy the addresses to the conn.
	 *
	 * Future Work to do actually to the path lookup
	 * - Use the rdma_cm as for roce
	 * - Use rdma_resolve_ip
	 * - Do iw_cm_connect and then reject the reply.
	 */
	if (!conn) {
		struct nvmeib_rdma_cm *cm_id;

		if (!(cm_id = create_iwarp_cm(info->dev, true))) {
			_NE(err_find_path_oom, "OOM");
			rv = -ENOMEM;
			goto out;
		}
		conn = cm_2_rocec(cm_id);
	}

	memset(&conn->dst_addr, 0, sizeof(conn->dst_addr));

	if (is_valid_roce_ipv4(&info->path->sgid) &&
		is_valid_roce_ipv4(&info->path->dgid)) {
		conn->dst_addr.s.sin_family = AF_INET;
		conn->dst_addr.s.sin_port = cpu_to_be16(info->service_port);
		conn->dst_addr.s.sin_addr.s_addr =
			(u32)(info->path->dgid.global.interface_id >> 32);

		conn->src_addr.s.sin_family = AF_INET;
		conn->src_addr.s.sin_port = 0;
		conn->src_addr.s.sin_addr.s_addr =
			(u32)(info->path->sgid.global.interface_id >> 32);
	} else {
		conn->dst_addr.s6.sin6_family = AF_INET6;
		conn->dst_addr.s6.sin6_port = cpu_to_be16(info->service_port);
		/* this is wrong but we need to know how to extract the ip from the gid */
		memcpy(&conn->dst_addr.s6.sin6_addr, &info->path->dgid, 16);
		conn->src_addr.s6.sin6_family = AF_INET6;
		conn->src_addr.s6.sin6_port = 0;
		memcpy(&conn->src_addr.s6.sin6_addr,  &info->path->sgid, 16);
	}

	scnprintf(src_addr_str, sizeof(src_addr_str), "%pIS", (struct sockaddr *)&conn->src_addr);
	scnprintf(dst_addr_str, sizeof(dst_addr_str), "%pISpc", (struct sockaddr *)&conn->dst_addr);
	_NT(trace_nvmeib_rdma_iwarp_find_path, "inet src=@SRC_ADDR dst=@DST_ADDR", src_addr_str, dst_addr_str);

	if (!conn->temp) {
		/* For non-temp connections, we have to just return true as the current upper-layer logic
		 * creates the QP after the find-path */
		rv = 0;
		goto out;
	} else {
		/* For temp connections, when using the cm, we create a temp qp (and cq) and
		 * do a connect which the target will then reject. When not using the cm, we don't need to because it will just use an ordinary socket */
		struct ib_cq *cq = NULL;
		struct ib_qp *qp = NULL;
		struct iw_cm_pd_hdr pd_hdr = {
			.req = {
				.qpn = find_path_qpn,
			},
		};
		struct rdma_conn_param conn_param = {
			.private_data = &pd_hdr,
			.private_data_len = sizeof(pd_hdr)
		};

		init_completion(&conn->done);
		if (!nvmeib_iwarp_find_path_sock) {
			struct ib_qp_init_attr qp_init = {
				.qp_type = IB_QPT_RC,
				.cap = {
					.max_send_wr = 1,
					.max_recv_wr = 1,
					.max_send_sge = 1,
					.max_recv_sge = 1,
				},
			};
			if (IS_ERR_OR_NULL(cq = nvmeib_create_cq(info->dev->ib_dev, NULL, NULL, NULL, 1, 0))) {
				rv = PTR_ERR(cq);
				_NE(err_find_path_create_cq, "ib_create_qp failed (@RV)", rv);
				goto destroy_temp;
			}
			qp_init.recv_cq = qp_init.send_cq = cq;
			/* For iWARP, we need to create a CQ/QP to allocate a socket */
			if (IS_ERR_OR_NULL(qp = nvmeib_rdma_create_qp(&conn->conn.cm,
				info->dev->pd, &qp_init, 0, 0, IB_ACCESS_LOCAL_WRITE))) {
				rv = PTR_ERR(qp);
				_NE(err_find_path_create_qp, "ib_create_qp failed (@RV)", rv);
				goto destroy_temp;
			}
		}
		if ((rv = iwarp_connect(conn, &conn_param, iwarp_find_path_handler, conn)) < 0) {
			_NE(err_find_path_connect, "iwarp_connect failed (@RV)", rv);
			goto destroy_temp;
		}
		if ((rv = wait_for_completion_interruptible_timeout(&conn->done,
				NVMEIB_WAIT_FOR_EVENT * 2)) <= 0) {
			_NE(err_find_path_connect_timeout, "iwarp_connect timeout (@RV)", rv);
			rv = rv < 0 ? rv : -ETIMEDOUT;
			if (!nvmeib_rdma_try_inv_cm(&conn->conn.cm)) {
				/* Failed to invalidate - wait forever */
				wait_for_completion(&conn->done);
			}
			goto destroy_temp;
		}

		if (conn->status < 0) {
			_NT(trace_nvmeib_rdma_iwarp_find_path_fail,
			    "iwarp find-path src=@SRC_ADDR -> dst=@DST_ADDR error (@STATUS)",
			    src_addr_str, dst_addr_str, conn->status);

			rv = conn->status;
			goto destroy_temp;
		}

		_NT(trace_nvmeib_rdma_iwarp_find_path_ok,
		    "iwarp find-path src=@SRC_ADDR -> dst=@DST_ADDR success",
		    src_addr_str, dst_addr_str);

		rv = 0;
destroy_temp:
		iwarp_disconnect(&conn->conn.cm, qp);
		nvmeib_rdma_destroy_cm(&conn->conn.cm);
		if (!nvmeib_iwarp_find_path_sock) {
			if (!IS_ERR_OR_NULL(qp))
				nvmeib_rdma_destroy_qp(NULL, qp);
			if (!IS_ERR_OR_NULL(cq))
				ib_destroy_cq(cq);
		}
		goto out;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}

static int lb_find_path(struct lb_rdma_connection *lbc, struct nvmeib_rdma_path_info *info)
{
	int rv = 0, ib_rv;
	struct lb_rdma_listener *ll;
	struct ib_port_attr port_attr;
	int n_conns;
	unsigned long flags;

	NFIN;
	if (lbc) {
		spin_lock_irqsave(&lbc->conn.lock, flags);
		if (lbc->conn.cm.rdma_type != _rdma_lb || lbc->state != LB_CONN_IDLE) {
			rv = -EINVAL;
		}
		spin_unlock_irqrestore(&lbc->conn.lock, flags);
		if (rv < 0)
			goto out;
	}

	if (info->path->sgid.global.interface_id != info->path->dgid.global.interface_id) {
		_NT(trace_nvmeib_rdma_lb_find_path, "lb_find_path called for a non-loopback path (@SGID -> @DGID)",
		   &info->path->sgid, &info->path->dgid);
		rv = -EINVAL;
		goto out;
	}

	_NT(trace_1_nvmeib_rdma_lb_find_path, "Looking for a lb path with GID @DGID", &info->path->dgid);
	rv = -ENODEV;
	mutex_lock(&lb_list_guard);
	list_for_each_entry(ll, &lb_list, link) {
		mutex_lock(&ll->guard);
		if (ll->ib_dev == info->dev->ib_dev &&
			ll->port == info->src_port &&
			ll->gid.global.interface_id == info->path->dgid.global.interface_id &&
			ll->active) {
			/* Found it - Check to see if it's active */
			if ((ib_rv = ib_query_port(ll->ib_dev, ll->port, &port_attr))) {
				_NE(error_nvmeib_rdma_lb_find_path, "ib_query_port() failed (@IB_RV).", ib_rv);
				goto loop_unlock;
			}
			if (port_attr.state != IB_PORT_ACTIVE) {
				_NT(trace_2_nvmeib_rdma_lb_find_path, "Loopback port @IB_DEV_NAME:@PORT in inactive state @STATE",
					ll->ib_dev->name, ll->port, port_attr.state);
				goto loop_unlock;
			}
			_NT(trace_3_nvmeib_rdma_lb_find_path, "Found a loopback listener @LL on device @IB_DEV_NAME port @PORT",
				ll, ll->ib_dev->name, ll->port);
			info->sgid_index = ll->gid_index;
			info->rdma_type = _rdma_lb;
			rv = 0;
			break;
		}
loop_unlock:
		mutex_unlock(&ll->guard);
	}
	if (rv < 0)
		goto unlock_list;

	/* ll->guard is still locked and rv == 0 */

	if (!lbc)
		goto unlock_l;

	/* We were called with a lb connection so bind it to the listener.
	*	(Listener Guard is still locked) */
	if (!(n_conns = nvmeib_ref_get(&ll->n_conns))) {
		_NT(trace_4_nvmeib_rdma_lb_find_path, "Loopback listener @LL on @IB_DEV_NAME:@PORT not accepting new connections",
			ll, ll->ib_dev->name, ll->port);
		rv = -EAGAIN;
		goto unlock_l;
	}
	spin_lock_irqsave(&lbc->conn.lock, flags);
	if (lbc->state == LB_CONN_IDLE) {
		lbc->state = LB_CONN_BOUND;
	} else {
		_NT(trace_5_nvmeib_rdma_lb_find_path,
		    "lb conn @LOCAL_ID (@LBC) in invalid state @STATE_GUARD",
		   lbc->local_id, lbc, lbc->state);
		rv = -EINVAL;
		nvmeib_ref_put(&ll->n_conns);
	}
	spin_unlock_irqrestore(&lbc->conn.lock, flags);
	if (rv < 0)
		goto unlock_l;

	lbc->conn.listener = &ll->listener;
	kref_get(&lbc->refcount);
	list_add_tail(&lbc->conn.link, &ll->listener.connections);
	_NT(trace_6_nvmeib_rdma_lb_find_path, "Bound loopback conn @LOCAL_ID (@LBC) to listener @LL on @IB_DEV_NAME:@PORT (n_conns: @N_CONNS)",
		lbc->local_id, lbc, ll, ll->ib_dev->name, ll->port, n_conns);

unlock_l:
	mutex_unlock(&ll->guard);

unlock_list:
	mutex_unlock(&lb_list_guard);

out:
	return rv;
	NFOUT;
}

static int __attribute__((__unused__)) is_valid_roce_ipv6(union ib_gid *g)
{
	return g->global.subnet_prefix && g->global.interface_id;
}

int nvmeib_rdma_find_path(struct nvmeib_rdma_path_info *info)
{
	int rv = 0;
	int is_roce;
	enum rdma_transport_type transport_type = rdma_node_get_transport(info->dev->ib_dev->node_type);
	enum rdma_link_layer link_layer;
	NFIN;

	if (transport_type == RDMA_TRANSPORT_IWARP) {
		_NT(trace_nvmeib_rdma_nvmeib_rdma_find_path, "Not using lb-cm for SIW device");
		if ((rv = iwarp_find_path(NULL, info)) == 0)
			info->rdma_type = _rdma_iwarp;
		else
			_NT(trace_iwarp_find_path_err, "iwarp_find_path failed (@RV)", rv);
		goto out;
	}
	else if (NVMEIB_USE_LB_CM) {
		if (info->path->sgid.global.interface_id == info->path->dgid.global.interface_id &&
			info->path->sgid.global.subnet_prefix == info->path->dgid.global.subnet_prefix) {
			_NT(trace_1_nvmeib_rdma_nvmeib_rdma_find_path, "Trying to find a loopback path @SGID -> @DGID",
				&info->path->sgid, &info->path->dgid);
			if ((rv = lb_find_path(NULL, info)) == 0) {
				info->rdma_type = _rdma_lb;
				goto out;
			}
		}
	}

	link_layer = rdma_port_get_link_layer(info->dev->ib_dev, info->src_port);
	is_roce = info->service_port == NVMEIB_PORT_ID;
	if ((is_roce && link_layer != IB_LINK_LAYER_ETHERNET) ||
		(!is_roce && link_layer != IB_LINK_LAYER_INFINIBAND)) {
		_NT(trace_2_nvmeib_rdma_nvmeib_rdma_find_path, "No match between link layer (@LINK_LAYER) and destination address (@IS_ROCE) "
			"info->service_port=@SERVICE_PORT info->dev->name=@NAME info->src_port=@SRC_PORT",
			link_layer, is_roce, info->service_port, info->dev->ib_dev->name, info->src_port);
		rv = -1;
		goto out;
	}

	if (!is_roce && !info->path->sgid.global.interface_id) {
		/* No source GID provided for IB this will fail */
		_NE(error_nvmeib_rdma_nvmeib_rdma_find_path, "Invalid zero source gid for IB find path");
		rv = -EINVAL;
		goto out;
	}

	if (rv == 0) {
		if (is_roce) {
			_NT(trace_3_nvmeib_rdma_nvmeib_rdma_find_path, "....");
			if ((rv = roce_find_path(NULL, info)) == 0) {
				info->rdma_type = _rdma_roce;
			}
		} else {
			_NT(trace_4_nvmeib_rdma_nvmeib_rdma_find_path, "....");
			if ((rv = ib_find_path(NULL, info)) == 0) {
				info->rdma_type = _rdma_ib;
			}
		}
	}

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_find_path);

int nvmeib_rdma_start_path_connection(struct nvmeib_rdma_cm *cm,
	struct nvmeib_rdma_path_info *info)
{
	int rv = 0;

	NFIN;
	switch (cm->rdma_type) {
	case _rdma_ib:
		rv = ib_find_path(cm_2_ibc(cm), info);
		break;
	case _rdma_roce:
		rv = roce_find_path(cm_2_rocec(cm), info);
		break;
	case _rdma_iwarp:
		rv = iwarp_find_path(cm_2_rocec(cm), info);
		break;
	case _rdma_lb:
		rv = lb_find_path(cm_2_lbc(cm), info);
		break;
	default:
		_NE(error_nvmeib_rdma_nvmeib_rdma_start_path_connection, "Invalid cm type @RDMA_TYPE", cm->rdma_type);
		rv = -EINVAL;
		break;
	}
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_start_path_connection);

static int ib_connect(struct ib_rdma_connection *ib,
	struct rdma_conn_param *conn_params,
	int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
	void *context)
{
	struct ib_cm_req_param param = {0};
	int rv;

	NFIN;
	ib->conn.event_handler = event_handler;
	ib->conn.context = context;
	param.primary_path = &ib->path;
	param.alternate_path = NULL;
	param.service_id = cpu_to_be64(ib->service_id);
	param.qp_num = conn_params->qp_num;
	param.qp_type = ib->conn.qp->qp_type;
	param.private_data = conn_params->private_data;
	param.private_data_len = conn_params->private_data_len;
	param.flow_control = 1;

	get_random_bytes(&param.starting_psn, 4);
	param.starting_psn &= 0xffffff;

	/*
	 * Pick some arbitrary defaults here; we could make these
	 * module parameters if anyone cared about setting them.
	 */
	param.responder_resources = conn_params->responder_resources;
	param.remote_cm_response_timeout = 20;
	param.local_cm_response_timeout = 20;
	param.retry_count = conn_params->retry_count;
	param.rnr_retry_count = conn_params->rnr_retry_count;
	param.max_cm_retries = 15;
	param.srq = conn_params->srq ? 1 : 0;
	_ND(trace_nvmeib_rdma_ib_connect, " service_id = @SERVICE_ID", param.service_id);
	_ND(trace_1_nvmeib_rdma_ib_connect, " qp_num = @QP_NUM", param.qp_num);
	_ND(trace_2_nvmeib_rdma_ib_connect, " qp_type = @QP_TYPE", param.qp_type);
	/* try to connect */
	if ((rv = ib_send_cm_req(ib->cm_id, &param)) < 0)
		_NE(error_nvmeib_rdma_ib_connect, "Failed to send login request @RV", rv);
	NFOUT;
	return rv;
}

static int roce_connect(struct roce_rdma_connection *roce,
	struct rdma_conn_param *conn_params,
	int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
	void *context)
{
	int rv;

	NFIN;
	roce->conn.event_handler = event_handler;
	roce->conn.context = context;
	conn_params->flow_control = 0;
	/* Only needed if cma_internal_qp is false, but no harm in setting them regardless */
	conn_params->qp_num = roce->conn.qp->qp_num;
	conn_params->srq = roce->conn.qp->srq != NULL ? 1 : 0;
	_NT(roce_connect_trace, "Calling rdma connenct on @PTR:@PTR", roce, roce->cm_id);

	rv = rdma_connect(roce->cm_id, conn_params);
	NFOUT;
	return rv;
}

static ssize_t fill_iw_cm_inv_stats(void *arg, char *buf, size_t len)
{
	struct nvmeib_dev *nvdev = arg;
	struct iw_cm_inv_stats *stats = nvdev->iw_cm_id_inv_stats_priv;
	ssize_t rv, count = 0;

	NFIN;
	if (!stats) {
		rv = -ENOENT;
		goto out;
	}
	spin_lock_bh(&stats->lock);
	count += scnprintf(buf + count, len - count,
			   "%s iw_cm invalidate stats\n", nvdev->ib_dev->name);
	count += scnprintf(buf + count, len - count,
			   "=====================================\n");
	count += scnprintf(buf + count, len - count,
			   "n_inv (tot/conn_err/wait_close):     %llu/%llu/%llu\n",
			   stats->n_inv, stats->n_inv_conn_err, stats->n_inv_wait_close);
	count += scnprintf(buf + count, len - count,
			   "wait_inv_us (min/avg/max):           %llu/%llu/%llu\n",
			stats->min_wait_inv_us,
			(stats->n_inv ? stats->tot_wait_inv_us / stats->n_inv : 0), 		stats->max_wait_inv_us);
	count += scnprintf(buf + count, len - count,
			   "wait_inv_conn_err_us (min/avg/max):   %llu/%llu/%llu\n",
			stats->min_wait_inv_conn_err_us,
			stats->n_inv_conn_err ? stats->tot_wait_inv_conn_err_us / stats->n_inv_conn_err : 0,
			stats->max_wait_inv_conn_err_us);
	count += scnprintf(buf + count, len - count,
			   "wait_inv_wait_close_us (min/avg/max): %llu/%llu/%llu\n",
			   stats->min_wait_inv_wait_close_us,
			   (stats->n_inv_wait_close ? stats->tot_wait_inv_wait_close_us / stats->n_inv_wait_close : 0),
			   stats->max_wait_inv_wait_close_us);
	spin_unlock_bh(&stats->lock);
	rv = count;

out:
	NFOUT;
	return rv;
}

static ssize_t chng_iw_cm_inv_stats(void *arg, char *buf, size_t len)
{
	struct nvmeib_dev *nvdev = arg;
	struct iw_cm_inv_stats *stats = nvdev->iw_cm_id_inv_stats_priv;
	ssize_t rv;
	int val;

	NFIN;
	if (!stats) {
		rv = -ENOENT;
		goto out;
	}
	if (sscanf(buf, "%d", &val) != 1 || val != 0) {
		_NT(trace_chng_iw_cm_inv_stats_inv, "Invalid value @STR. Write a 0 to clear the stats", buf);
		rv = -EINVAL;
		goto out;
	}

	spin_lock_bh(&stats->lock);
	stats->n_inv = 0;
	stats->n_inv_conn_err = 0;
	stats->n_inv_wait_close = 0;
	stats->min_wait_inv_us = 0;
	stats->max_wait_inv_us = 0;
	stats->tot_wait_inv_us = 0;
	stats->min_wait_inv_conn_err_us = 0;
	stats->max_wait_inv_conn_err_us = 0;
	stats->tot_wait_inv_conn_err_us = 0;
	stats->min_wait_inv_wait_close_us = 0;
	stats->max_wait_inv_wait_close_us = 0;
	stats->tot_wait_inv_wait_close_us = 0;
	spin_unlock_bh(&stats->lock);

	_NT(trace_chng_iw_cm_inv_stats_clr, "Cleared stats for @IB_DEVICE", nvdev->ib_dev->name);

	rv = len;
out:
	NFOUT;
	return rv;
}

int nvmeib_rdma_init_iw_cm_inv_stats(struct nvmeib_dev *nvdev)
{
	struct nvmeib_public_procfs_ent *proc_ent;
	struct iw_cm_inv_stats *stats;
	int rv;

	NFIN;
	if (!(proc_ent = nvmeib_public_proc_create(
		"iw_cm_inv_stats", nvdev->proc_dir, fill_iw_cm_inv_stats, chng_iw_cm_inv_stats, nvdev)))
	{
		_NT(trace_rdma_init_iw_cm_inv_stats_proc_fail, "Could not create proc");
		rv = -ENOMEM;
		goto out;
	}

	if (!(stats = kzalloc(sizeof(*stats), GFP_KERNEL))) {
		_NT(trace_rdma_init_iw_cm_inv_stats_oom, "OOM");
		rv = -ENOMEM;
		goto out;
	}

	stats->proc_ent = proc_ent;
	spin_lock_init(&stats->lock);
	nvdev->iw_cm_id_inv_stats_priv = stats;

	rv = 0;

out:
	NFOUT;
	return rv;
}

void nvmeib_rdma_free_iw_cm_inv_stats(struct nvmeib_dev *nvdev)
{
	struct iw_cm_inv_stats *stats = nvdev->iw_cm_id_inv_stats_priv;
	NFIN;
	nvdev->iw_cm_id_inv_stats_priv = NULL;
	if (stats) {
		nvmeib_public_proc_remove(stats->proc_ent);
		kfree(stats);
	}
	NFOUT;
}

int nvmeib_rdma_init_find_path_sock(struct nvmeib_dev *nvdev)
{
	struct find_path_sock_dev *dev;
	int rv;

	NFIN;
	if (!(dev = kzalloc(sizeof(*dev), GFP_KERNEL))) {
		_NT(trace_rdma_init_find_path_sock_cep_alloc_fail, "OOM");
		rv = -ENOMEM;
		goto out;
	}

	kref_init(&dev->ref);
	mutex_init(&dev->lock);
	nvmeib_ref_init(&dev->ref_conn);

	if (!(dev->find_path_sock_wq = wq_create(proc_name_format("M", "WQ", "findpath")))) {
		_NT(trace_rdma_init_find_path_sock_wq_alloc_fail, "OOM");
		rv = -ENOMEM;
		goto free_cep;
	}

	dev->nvdev = nvdev;
	INIT_LIST_HEAD(&dev->sock_cep_list);

	find_path_dev_get(dev, "nvdev-ptr");
	nvdev->iw_find_path_sock_priv = dev;
	rv = 0;
	goto out;

free_cep:
	kfree(dev);

out:
	NFOUT;
	return rv;
}

static void free_find_path_sock_dev(struct kref *ref)
{
	struct find_path_sock_dev *dev = container_of(ref, typeof(*dev), ref);

	_NT(trace_free_find_path_sock_dev,
	    "Freeing find_path_sock_dev @PTR on device @IB_DEVICE",
		dev, dev->nvdev->ib_dev->name);

	wq_destroy(dev->find_path_sock_wq);

	kfree(dev);
}

static void find_path_dev_get(struct find_path_sock_dev *dev, const char *get_desc)
{
	kref_get(&dev->ref);
	_ND(trace_find_path_dev_get, "Get find_path_dev @PTR - Desc: @STR New refcount: @COUNT",
	    dev, get_desc, kref_read(&dev->ref));
}

static void find_path_dev_put(struct find_path_sock_dev *dev, const char *put_desc)
{
	int refcount = kref_read(&dev->ref);
	_ND(trace_find_path_dev_put,
	    "Put find_path_dev @PTR - Desc: @STR New refcount: @COUNT",
		dev, put_desc, refcount - 1);
	BUG_ON(refcount < 1);
	kref_put(&dev->ref, free_find_path_sock_dev);
}

void nvmeib_rdma_free_find_path_sock(struct nvmeib_dev *nvdev)
{
	struct find_path_sock_dev *dev = nvdev->iw_find_path_sock_priv;
	struct find_path_sock_cep *cep;

	NFIN;
	if (!dev) {
		goto out;
	}

	_NT(trace_rdma_free_find_path_sock,
	    "Freeing find-path-sock on device @IB_DEVICE", nvdev->ib_dev->name);

	nvdev->iw_find_path_sock_priv = NULL;
	find_path_dev_put(dev, "nvdev-ptr");

	nvmeib_ref_release_start(&dev->ref_conn);

	list_for_each_entry(cep, &dev->sock_cep_list, link) {
		iwarp_disconnect_sock_only(cep);
	}

	nvmeib_ref_release_wait(&dev->ref_conn);

	wq_drain(dev->find_path_sock_wq);

	find_path_dev_put(dev, "free-dev");

out:
	NFOUT;
}

static int iwarp_connect_sock_only(struct roce_rdma_connection *roce,
				   struct rdma_conn_param *conn_params,
				   int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
				   void *context)
{
	struct find_path_sock_cep *cep = roce->find_path_sock_cep;
	struct find_path_sock_dev *cep_dev = cep->parent;
	struct sock *sk = NULL;
#if !KS_HAS_SOCK_SET_REUSEADDR
	int s_val = 1;
#endif
	int rv;
	struct sockaddr_storage ss_dest;
	size_t addr_size = roce->src_addr.s.sin_family == AF_INET ?
		sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

	NFIN;
	find_path_cep_set_inuse(cep);

	if (nvmeib_ref_is_dying(&cep_dev->ref_conn)) {
		_NT(trace_iwarp_connect_sock_only_dev_rls,
		    "Not connecting endpoint @PTR of iwarp conn @CM_ID. "
		    "Device @IB_DEVICE is releasing",
		    cep, roce, cep_dev->nvdev->ib_dev->name);
		rv = -EBUSY;
		find_path_cep_set_free(cep);
		goto out;
	}

	BUG_ON(!cep);
	BUG_ON(roce->iw_cm_id != NULL);

	rv = sock_create(roce->src_addr.s.sin_family, SOCK_STREAM, IPPROTO_TCP, &cep->s);
	if (rv < 0) {
		_NT(trace_iwarp_connect_sock_only_create_fail, "sock_create failed (@RV)", rv);
		find_path_cep_set_free(cep);
		goto out;
	}
	sk = cep->s->sk;

#if KS_HAS_SOCK_NOT_OWNED_BY_ME
	{
		struct net *net = sock_net(sk);

		/* Prevent inet_csk_clear_xmit_timers_sync being called from tcp_close()
		 * Fixes [NVMESH-5532] */
		sk->sk_net_refcnt = 1;
		get_net(net);
	}
#endif

	/* sk_user_data reference */
	find_path_cep_get(cep, "sk_user_data");
	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = cep;
	cep->orig_sk_state_change = sk->sk_state_change;
	cep->orig_sk_data_ready = sk->sk_data_ready;
	sk->sk_state_change = iw_find_path_sock_state_change;
	sk->sk_data_ready = iw_find_path_sock_data_ready;
	write_unlock_bh(&sk->sk_callback_lock);

#if KS_HAS_SOCK_SET_REUSEADDR
	sock_set_reuseaddr(cep->s->sk);
	rv = 0;
#else
	rv = kernel_setsockopt(cep->s, SOL_SOCKET, SO_REUSEADDR, (char *)&s_val, sizeof(s_val));
#endif
	if (rv < 0) {
		_NT(trace_iwarp_connect_sock_only_sockopt_fail, "kernel_setsockopt failed for SO_REUSEADDR (@RV)", rv);
		goto error;
	}

	rv = kernel_bind(cep->s, (struct sockaddr *)&roce->src_addr, addr_size);
	if (rv < 0) {
		_NT(trace_iwarp_connect_sock_only_bind_fail, "bind failed (@RV)", rv);
		goto error;
	}

	/* Copy dest sockaddr from roce_conn */
	memcpy(&ss_dest, &roce->dst_addr, addr_size);
	/* Update dest port to nvmeib_iwarp_find_path_sock_port */
	if (ss_dest.ss_family == AF_INET)
		((struct sockaddr_in *)&ss_dest)->sin_port = htons(nvmeib_iwarp_find_path_sock_port);
	else if (ss_dest.ss_family == AF_INET6)
		((struct sockaddr_in6 *)&ss_dest)->sin6_port = htons(nvmeib_iwarp_find_path_sock_port);
	else
		BUG();
	/* Connect */
	rv = kernel_connect(cep->s, (struct sockaddr *)&ss_dest, addr_size, O_NONBLOCK);
	if (rv == -EINPROGRESS)
		rv = 0;

	if (rv < 0) {
		_NT(trace_iwarp_connect_sock_only_connect_fail, "connect failed (@RV)", rv);
		goto error;
	}

	cep->state = FIND_PATH_EPSTATE_CONNECTING;

	find_path_cep_set_free(cep);

	_NT(trace_iwarp_connect_sock_only, "Connecting iWARP socket-only in non-blocking mode");
	goto out;

error:
	cep->state = FIND_PATH_EPSTATE_CONNECT_ERROR;

	/* No TCP_ESTABLISHED event will happen in this case.
	 * Release the socket here instead */
	find_path_work_handler_release_sock(cep);

	find_path_cep_set_free(cep);

	goto out;

out:
	NFOUT;
	return rv;
}

static int iwarp_connect(struct roce_rdma_connection *roce,
	struct rdma_conn_param *conn_params,
	int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
	void *context)
{
	size_t pd_len = offsetof(struct iw_cm_pd, user_pd) + conn_params->private_data_len;
	struct iw_cm_pd *pd = !roce->find_path_sock_cep ? kzalloc(pd_len, GFP_KERNEL) : NULL;
	struct iw_cm_conn_param iw_conn_param = {
		.private_data = pd,
		.private_data_len = pd_len,
		.ord = conn_params->initiator_depth,
		.ird = conn_params->responder_resources,
		.qpn = !roce->find_path_sock_cep ? roce->conn.qp->qp_num : 0,
	};
	int rv;

	NFIN;

	roce->conn.event_handler = event_handler;
	roce->conn.context = context;

	if (roce->find_path_sock_cep) {
		rv = iwarp_connect_sock_only(roce, conn_params, event_handler, context);
		goto out;
	}

	BUG_ON(roce->iw_cm_id == NULL);
	if (!pd) {
		_NE(error_iwarp_connect_oom, "OOM Error");
		rv = -ENOMEM;
		goto out;
	}
	if (roce->temp)
		pd->hdr.req.qpn = find_path_qpn;
	else
		pd->hdr.req.qpn = cpu_to_be32(roce->conn.qp->qp_num);
	memcpy(pd->user_pd, conn_params->private_data, conn_params->private_data_len);

	memcpy(&roce->iw_cm_id->local_addr, &roce->src_addr,
		sizeof(roce->src_addr));
	memcpy(&roce->iw_cm_id->remote_addr, &roce->dst_addr,
		sizeof(roce->dst_addr));

	rv = iw_cm_connect(roce->iw_cm_id, &iw_conn_param);
out:
	kfree(pd);
	NFOUT;
	return rv;
}

static int start_lb_listener(struct nvmeib_rdma_cm *cm_id)
{
	struct lb_rdma_listener *lbl;
	int rv = 0;

	NFIN;
	if (!(lbl = cm_2_lbl(cm_id))) {
		rv = -EINVAL;
		goto out;
	}

	mutex_lock(&lb_list_guard);
	list_add_tail(&lbl->link, &lb_list);
	num_lb_listeners++;
	_NT(trace_nvmeib_rdma_start_lb_listener, "Added loopback listener on IB device @IB_DEV_NAME:@PORT (GID @GID_IPV6). Num listeners @NUM_LB_LISTENERS",
		   lbl->ib_dev->name, lbl->port, &lbl->gid, num_lb_listeners);

	mutex_lock(&lbl->guard);
	lbl->active = true;
	mutex_unlock(&lbl->guard);

	mutex_unlock(&lb_list_guard);

out:
	NFOUT;
	return rv;
}

static void lbc_unbind(struct lb_rdma_connection *lbc)
{
	struct lb_rdma_listener *lbl;
	if ((lbl = l_2_lb(lbc->conn.listener))) {
		/* Attached to listener, remove from listener also */
		mutex_lock(&lbl->guard);
		list_del(&lbc->conn.link);
		nvmeib_ref_put(&lbl->n_conns);
		lbc->conn.listener = NULL;
		mutex_unlock(&lbl->guard);
	}
}

static void lb_free_id(struct kref *kref)
{
	struct lb_rdma_connection *lbc = container_of(kref, struct lb_rdma_connection, refcount);
	kfree(lbc);
}

static void lb_destroy_id(struct nvmeib_rdma_cm *cm_id)
{
	struct rdma_connection *c = NULL;
	struct lb_rdma_connection *lbc = NULL;
	enum lb_rdma_connection_state prev_state;
	unsigned long flags;

	NFIN;
	if (!(c = cm_2_c(cm_id)))
		return;
	if (!(lbc = conn_2_lb(c)))
		return;

	spin_lock_irqsave(&lbc->conn.lock, flags);
	_NT(trace_nvmeib_rdma_lb_destroy_id,
	    "Destroy id called on lb conn @LOCAL_ID (@LBC) in state @STATE",
	    lbc->local_id, lbc, lbc->state);

	if (lbc->state == LB_CONN_DESTROYING) {
		spin_unlock_irqrestore(&lbc->conn.lock, flags);
		_NT(trace_1_nvmeib_rdma_lb_destroy_id,
		    "lb conn @LOCAL_ID (@LBC) already destroyed",
		    lbc->local_id, lbc);
		return;
	}
	prev_state = lbc->state;
	lbc->state = LB_CONN_DESTROYING;
	spin_unlock_irqrestore(&lbc->conn.lock, flags);

	/* Wait for any handlers in progress to finish */
	mutex_lock(&lbc->handler_mutex);
	mutex_unlock(&lbc->handler_mutex);

	mutex_lock(&lbc->qp_mutex);
	if (lbc->conn.qp) {
		_NW(warn_nvmeib_rdma_lb_destroy_id, "Destroy ib called on lb conn @LOCAL_ID (@LBC) in state @STATE with non-NULL QP @QP",
			lbc->local_id, lbc, prev_state, lbc->conn.qp);
		lbc->conn.qp = NULL;
	}
	mutex_unlock(&lbc->qp_mutex);

	lbc_unbind(lbc);

	kref_put(&lbc->refcount, lb_free_id);
	NFOUT;
}

static int lb_destroy_qp(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp)
{
	struct rdma_connection *c = NULL;
	struct lb_rdma_connection *lbc = NULL;
	int rv = 0;

	NFIN;
	if (!(c = cm_2_c(cm_id)) || !(lbc = cm_2_lbc(cm_id))) {
		rv = -EINVAL;
		goto out;
	}

	_NT(trace_nvmeib_rdma_lb_destroy_qp, "Destroy qp called on lb conn @LOCAL_ID (@LBC - cm_id @CM_ID) in state @STATE_GUARD",
	   lbc->local_id, lbc, cm_id, lbc->state);

	mutex_lock(&lbc->qp_mutex);

	if (qp && c->qp != qp) {
		_NT(trace_1_nvmeib_rdma_lb_destroy_qp, "Mismatch between lb conn @LOCAL_ID (@LBC - cm_id @CM_ID) with qp @QP and qp @QP",
		   lbc->local_id, lbc, cm_id, c->qp, qp);
		rv = -EINVAL;
		goto unlock;
	}
	rv = _ib_destroy_qp(qp);
	c->qp = NULL;

unlock:
	mutex_unlock(&lbc->qp_mutex);

out:
	NFOUT;
	return rv;
}

static int lb_disconnect(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp)
{
	int rv = 0;
	struct lb_rdma_connection *lbc_local = NULL, *lbc_remote = NULL;
	struct nvmeib_rdma_event dis_event = {0};
	unsigned long flags;

	NFIN;
	if (!(lbc_local = cm_2_lbc(cm_id))) {
		rv = -EINVAL;
		goto out;
	}

	spin_lock_irqsave(&lbc_local->conn.lock, flags);
	switch (lbc_local->state) {
	case LB_CONN_BOUND:
	case LB_CONN_CONNECTING:
	case LB_CONN_CONNECTED:
	case LB_CONN_ACCEPTING:
		lbc_local->state =  LB_CONN_DISCONNECTING;
		break;
	default:
		_NT(lb_disconnect_t2, "lb conn @UINT (@PTR) in invalid state @INT",
		   lbc_local->local_id, lbc_local, lbc_local->state);
		rv = -EINVAL;
	}
	spin_unlock_irqrestore(&lbc_local->conn.lock, flags);

	if (rv < 0)
		goto out;

	mutex_lock(&lbc_local->qp_mutex);
	if (lbc_local->conn.qp) {
		_NT(error_nvmeib_rdma_lb_disconnect, "lb conn @LOCAL_ID (@LBC_LOCAL) qpn: @QP_NUM (@QP) -> error",
		   lbc_local->local_id, lbc_local, lbc_local->conn.qp->qp_num, lbc_local->conn.qp);
		/* Set the qp state to error */
		rv = ib_set_qp_err(lbc_local->conn.qp);
	} else {
		_NT(error_1_nvmeib_rdma_lb_disconnect, "lb conn @LOCAL_ID (@LBC_LOCAL) not handling transition to error",
		   lbc_local->local_id, lbc_local);
	}
	mutex_unlock(&lbc_local->qp_mutex);

	if (!(lbc_remote = find_remote_lbc(lbc_local))) {
		lbc_local->conn.wait_for_drep = false;
		goto out;
	}

	spin_lock_irqsave(&lbc_remote->conn.lock, flags);
	switch (lbc_remote->state) {
	case LB_CONN_CONNECTING:
	case LB_CONN_ACCEPTING:
	case LB_CONN_CONNECTED:
		dis_event.event = NVMEIB_DREQ_RECEIVED;
		lbc_local->conn.wait_for_drep = true;
		spin_unlock_irqrestore(&lbc_remote->conn.lock, flags);
		lbc_handler(lbc_remote, &dis_event);
		break;
	case LB_CONN_DISCONNECTING:
		dis_event.event = NVMEIB_DREP_RECEIVED;
		lbc_local->conn.wait_for_drep = false;
		spin_unlock_irqrestore(&lbc_remote->conn.lock, flags);
		lbc_handler(lbc_remote, &dis_event);
		break;
	default:
		_NT(lb_disconnect_t3, "lbc remote conn @UINT (@PTR) in invalid state @INT",
		   lbc_remote->local_id, lbc_remote, lbc_remote->state);
		spin_unlock_irqrestore(&lbc_remote->conn.lock, flags);
	}

	/* Dec refcount (incremented by find_remote_lbc) */
	kref_put(&lbc_remote->refcount, &lb_free_id);

	spin_lock_irqsave(&lbc_local->conn.lock, flags);
	BUG_ON(lbc_local->state != LB_CONN_DISCONNECTING);
	lbc_local->state = LB_CONN_DISCONNECTED;
	spin_unlock_irqrestore(&lbc_local->conn.lock, flags);

out:
	NFOUT;
	return rv;
}

static int lb_reject(struct nvmeib_rdma_cm *cm, void *rej, u8 len)
{
	struct lb_rdma_connection *lbc = NULL;
	struct nvmeib_rdma_event event = {
		.event = NVMEIB_REJ_RECEIVED,
		.status = IB_CM_REJ_CONSUMER_DEFINED,
		.private_data = rej,
		.private_data_len = len,
	};
	int rv = 0;

	NFIN;
	if (!(lbc = cm_2_lbc(cm))) {
		rv = -EINVAL;
		goto out;
	}

	rv = lbc_remote_handler(lbc, &event);

out:
	NFOUT;
	return rv;
}

static int ib_qp_get_send_psn(struct ib_qp *qp, u32 *send_psn)
{
	struct ib_qp_attr qp_attr = {};
	struct ib_qp_init_attr qp_init_attr = {};
	int rv;

	if (!(rv = ib_query_qp(qp, &qp_attr, IB_QP_SQ_PSN, &qp_init_attr))) {
		*send_psn = qp_attr.sq_psn;
	} else {
		_NE(error_nvmeib_rdma_ib_qp_get_send_psn, "ib_query_qp failed (@RV) for QPn: @QP_NUM (@QP)", rv, qp->qp_num, qp);
	}

	return rv;
}

static int __lbc_qp_2_rts(struct lb_rdma_connection *lbc, struct lb_rdma_listener *lbl,
						struct lb_rdma_connection *lbc_remote)
{
	struct ib_qp_attr qp_attr = {};
	struct ib_port_attr port_attr = {0};
	int rv = 0;

	NFIN;
	/* Configure Path and Addressing Attributes */
	if ((rv = ib_query_port(lbl->ib_dev, lbl->port, &port_attr))) {
		_NE(error_nvmeib_rdma_lbc_qp_2_rts, "ib_query_port failed (@RV) for @IB_DEV_NAME:@PORT",
		   rv, lbl->ib_dev->name, lbl->port);
		goto out;
	}

	qp_attr.path_mtu = port_attr.active_mtu;
	NVMEIB_AH_DLID(qp_attr.ah_attr) = port_attr.lid;
	qp_attr.ah_attr.port_num = lbl->port;

	if (lbl->link_layer == IB_LINK_LAYER_ETHERNET) {
		/* Ethernet Loopback uses global header */
		qp_attr.ah_attr.ah_flags = IB_AH_GRH;
		qp_attr.ah_attr.grh.sgid_index = lbl->gid_index;
		qp_attr.ah_attr.grh.dgid = lbl->gid;
		qp_attr.ah_attr.grh.hop_limit = 0xff;
		qp_attr.ah_attr.grh.flow_label = 0;
		qp_attr.ah_attr.grh.traffic_class = 0;
#if IB_HAS_RDMA_AH_ATTR_TYPE
		qp_attr.ah_attr.type = RDMA_AH_ATTR_TYPE_ROCE;
		memcpy(qp_attr.ah_attr.roce.dmac, lbl->roce_mac, ETH_ALEN);
#endif
		_NT(trace_nvmeib_rdma_lbc_qp_2_rts, "Connecting RoCE Loopback QP @LOCAL_QP_NUM (@QP) with GID[@GID_INDEX] @GID_IPV6 MAC: @ROCE_MAC",
		   lbc->local_qp_num, lbc->conn.qp, lbl->gid_index, &lbl->gid, lbl->roce_mac);
	} else {
//		qp_attr.ah_attr.NVMEIB_DLID_FIELD = port_attr.lid;
		qp_attr.ah_attr.grh.dgid = lbl->gid;
#if IB_HAS_RDMA_AH_ATTR_TYPE
		qp_attr.ah_attr.type = RDMA_AH_ATTR_TYPE_IB;
#endif
		_NT(trace_1_nvmeib_rdma_lbc_qp_2_rts, "Connecting IB Loopback QP @LOCAL_QP_NUM (@QP) with GID @GID_IPV6",
		   lbc->local_qp_num, lbc->conn.qp, &lbl->gid);
	}

	qp_attr.qp_state = IB_QPS_RTR;
	qp_attr.dest_qp_num = lbc->remote_qp_num;
	qp_attr.rq_psn = lbc_remote->send_psn;
	qp_attr.max_dest_rd_atomic = lbc_remote->req_depth;
	if ((rv = ib_modify_qp(lbc->conn.qp, &qp_attr,
			  IB_QP_STATE              |
			  IB_QP_AV                 |
			  IB_QP_PATH_MTU           |
			  IB_QP_DEST_QPN           |
			  IB_QP_RQ_PSN             |
			  IB_QP_MAX_DEST_RD_ATOMIC |
			  IB_QP_MIN_RNR_TIMER)) < 0) {
		_NE(error_1_nvmeib_rdma_lbc_qp_2_rts, "Error @RV modifying QP @LOCAL_QP_NUM (@QP) to RTR",
		   rv, lbc->local_qp_num, lbc->conn.qp);
		goto out;
	}
	if (lbc->conn.qp->srq != NULL)
		lbc->conn.expect_last_wqe = NVMEIB_RDMA_EXPECT_LAST_WQE;

	/* Put the QP into RTS */
	qp_attr.qp_state = IB_QPS_RTS;
	qp_attr.timeout = qp_timeout;
	qp_attr.retry_cnt = qp_retry_cnt;
	qp_attr.rnr_retry = 7;
	qp_attr.sq_psn = lbc->send_psn;
	qp_attr.max_rd_atomic = lbc_remote->resp_rsrc;

	if ((rv = ib_modify_qp(lbc->conn.qp, &qp_attr,
			       IB_QP_STATE              |
			       IB_QP_TIMEOUT            |
			       IB_QP_RETRY_CNT          |
			       IB_QP_RNR_RETRY          |
			       IB_QP_SQ_PSN             |
			       IB_QP_MAX_QP_RD_ATOMIC)) < 0) {
		_NE(error_2_nvmeib_rdma_lbc_qp_2_rts, "Error @RV modifying QP @LOCAL_QP_NUM (@QP) to RTR",
		   rv, lbc->local_qp_num, lbc->conn.qp);
	}

out:
	NFOUT;
	return rv;
}

static int lb_accept(struct nvmeib_rdma_cm *cm_id, struct ib_qp *qp, struct rdma_conn_param *conn_params)
{
	/* We now have both QPs created, we can simply connect them and send each side NVMEIB_USER_ESTABLISHED */
	int rv = 0;
	struct lb_rdma_connection *lbc_accept = NULL, *lbc_init = NULL;
	struct lb_rdma_listener *lbl = NULL;
	struct nvmeib_rdma_event est_event = {
		.event = NVMEIB_USER_ESTABLISHED,
		.status = 0,
		.private_data = (void *)conn_params->private_data,
		.private_data_len = conn_params->private_data_len,
	};
	unsigned long flags;
	enum lb_rdma_connection_state prev_init_state;

	NFIN;
	if (!(lbc_accept = cm_2_lbc(cm_id)) || !(lbl = l_2_lb(lbc_accept->conn.listener)) ||
		!(lbc_init = find_remote_lbc(lbc_accept))) {
		rv = -EINVAL;
		goto out;
	}

	spin_lock_irqsave(&lbc_init->conn.lock, flags);
	prev_init_state = lbc_init->state;
	if (lbc_init->state != LB_CONN_CONNECTING) {
		_NE(error_nvmeib_rdma_lb_accept, "lbc init conn @LBC_INIT in invalid state @PREV_INIT_STATE", lbc_init, lbc_init->state);
		rv = -ECONNRESET;
	}
	else if (lbc_init->conn.listener != lbc_accept->conn.listener) {
		_NE(error_1_nvmeib_rdma_lb_accept, "Mistmatched listeners (@LISTENER != @LISTENER) between init conn @LBC_INIT and accept conn @LBC_ACCEPT",
		 lbc_init->conn.listener, lbc_accept->conn.listener, lbc_init, lbc_accept);
		rv = -EINVAL;
	}
	spin_unlock_irqrestore(&lbc_init->conn.lock, flags);
	if (rv < 0)
		goto event;

	spin_lock_irqsave(&lbc_accept->conn.lock, flags);
	if (lbc_accept->state == LB_CONN_BOUND) {
		lbc_accept->state = LB_CONN_ACCEPTING;
	} else {
		_NE(error_2_nvmeib_rdma_lb_accept,
		    "lb accept conn @LBC_ACCEPT in invalid state @STATE_GUARD",
		   lbc_accept, lbc_accept->state);
		rv = -EINVAL;
	}
	spin_unlock_irqrestore(&lbc_accept->conn.lock, flags);
	if (rv < 0)
		goto event;

	/* First transition the accept QP (if it is managed by the cm) */
	mutex_lock(&lbc_accept->qp_mutex);
	lbc_accept->remote_qp_num = lbc_init->local_qp_num;
	if (!lbc_accept->conn.qp || lbc_accept->conn.qp != qp) {
		_NT(trace_nvmeib_rdma_lb_accept, "lb accept conn @LBC_ACCEPT not handling QPn: @QP_NUM (@QP) transition",
		   lbc_accept, qp->qp_num, qp);
		lbc_accept->local_qp_num = qp->qp_num;
		ib_qp_get_send_psn(qp, &lbc_accept->send_psn);
		mutex_unlock(&lbc_accept->qp_mutex);
		goto init2rts;
	}
	lbc_accept->local_qp_num = lbc_accept->conn.qp->qp_num;

	/* Accept QP to RTS */
	rv = __lbc_qp_2_rts(lbc_accept, lbl, lbc_init);
	mutex_unlock(&lbc_accept->qp_mutex);
	if (rv < 0)
		goto event;

init2rts:
	mutex_lock(&lbc_init->qp_mutex);
	prev_init_state = lbc_init->state;
	lbc_init->remote_qp_num = lbc_accept->local_qp_num;
	if (!lbc_init->conn.qp) {
		_NT(trace_1_nvmeib_rdma_lb_accept, "lb init conn @LBC_INIT not handling QP transition", lbc_init);
		mutex_unlock(&lbc_init->qp_mutex);
		goto event;
	}

	/* Init QP to RTS */
	rv = __lbc_qp_2_rts(lbc_init, lbl, lbc_accept);
	mutex_unlock(&lbc_init->qp_mutex);
	if (rv < 0)
		goto event;

	/* Transition init conn to connected state */
	spin_lock_irqsave(&lbc_init->conn.lock, flags);
	if (lbc_init->state == LB_CONN_CONNECTING) {
		lbc_init->state = LB_CONN_CONNECTED;
	} else {
		_NE(error_3_nvmeib_rdma_lb_accept, "lb init conn @LBC_INIT in invalid state @PREV_INIT_STATE", lbc_init, lbc_init->state);
		rv = -EINVAL;
	}
	spin_unlock_irqrestore(&lbc_init->conn.lock, flags);

event:
	spin_lock_irqsave(&lbc_accept->conn.lock, flags);
	if (lbc_accept->state == LB_CONN_ACCEPTING) {
		if (!rv) {
			lbc_accept->state = LB_CONN_CONNECTED;
			_NT(trace_2_nvmeib_rdma_lb_accept,
			    "Succesfully connected loopback QPs");
			est_event.event = NVMEIB_USER_ESTABLISHED;
			est_event.status = 0;
		} else {
			lbc_accept->state = LB_CONN_ERROR;
			est_event.event = NVMEIB_CONNECT_ERROR;
			est_event.status = rv;
		}
	} else {
		_NT(trace_3_nvmeib_rdma_lb_accept,
			"lb accept conn @LBC_ACCEPT in invalid state @STATE_GUARD",
			lbc_accept, lbc_accept->state);
		est_event.event = NVMEIB_CONNECT_ERROR;
		est_event.status = -EINVAL;
		rv = -EINVAL;
	}
	spin_unlock_irqrestore(&lbc_accept->conn.lock, flags);

	if (prev_init_state != LB_CONN_ERROR) {
		/* Initiator didn't invalidate - call its event handler */
		lbc_handler(lbc_init, &est_event);
	}

	/* Decrement ref of init conn (Incremented by find_lbc) */
	kref_put(&lbc_init->refcount, &lb_free_id);

	/* Only send an event on success to the acceptor, otherwise we can just return an error */
	if (!rv) {
		est_event.private_data = NULL;
		est_event.private_data_len = 0;
		lbc_handler(lbc_accept, &est_event);
	}

out:
	NFOUT;
	return rv;
}

/* Called from the connecting side after REQ sent, but before RESP/REJ received
 * Returns true if the cm_id can be be destroyed immediately without waiting for the response */
static bool lb_try_inv_cm(struct nvmeib_rdma_cm *cm_id)
{
	struct lb_rdma_connection *lbc = NULL;
	bool ret = false;
	unsigned long flags;

	NFIN;
	if (!(lbc = cm_2_lbc(cm_id))) {
		goto out;
	}

	spin_lock_irqsave(&lbc->conn.lock, flags);
	if (lbc->state == LB_CONN_CONNECTING) {
		lbc->state = LB_CONN_ERROR;
		ret = true;
	} else {
		/* The event handler has already gotten the event so we cannot invalidate the cm_id */
		_NT(trace_lb_try_inv_cm_fail, "Failed to invalidate loopback conn @CM_ID due to state @STATE",
		    lbc, lbc->state);
	}
	spin_unlock_irqrestore(&lbc->conn.lock, flags);

out:
	return ret;
}

static struct lb_rdma_connection *lbl_add_accept_cm(
	struct lb_rdma_listener *lbl, struct lb_rdma_connection *lbc_init)
{
	struct lb_rdma_connection *lbc = NULL;

	if (!nvmeib_ref_get(&lbl->n_conns)) {
		_NT(trace_nvmeib_rdma_lbl_add_accept_cm, "Listener @LBL (@IB_DEV_NAME:@PORT) is no longer accepting new connections",
		   lbl, lbl->ib_dev->name, lbl->port);
		goto out;
	}
	if (!(lbc = kzalloc(sizeof(*lbc), GFP_KERNEL))) {
		nvmeib_ref_put(&lbl->n_conns);
		goto out;
	}
	lbc->conn.cm.rdma_type = _rdma_lb_accept;
	lbc->conn.cm.cm_type = _cm_connection;
	spin_lock_init(&lbc->conn.lock);
	lbc->state = LB_CONN_BOUND;
	mutex_init(&lbc->handler_mutex);
	mutex_init(&lbc->qp_mutex);
	kref_init(&lbc->refcount);
	lbc->local_id = (u32)atomic_inc_return(&lb_id_cnt);
	lbc->remote_id = lbc_init->local_id;
	lbc->remote_qp_num = lbc_init->local_qp_num;
	get_random_bytes(&lbc->send_psn, 4);
	lbc->send_psn &= 0xffffff;

	mutex_lock(&lbl->guard);
	lbc->conn.listener = &lbl->listener;
	kref_get(&lbc->refcount);
	list_add_tail(&lbc->conn.link, &lbl->listener.connections);
	mutex_unlock(&lbl->guard);

	_NT(trace_1_nvmeib_rdma_lbl_add_accept_cm, "Added lb accept conn @LOCAL_ID (@LBC) to listener @LBL (@IB_DEV_NAME:@PORT)",
	   lbc->local_id, lbc, lbl, lbl->ib_dev->name, lbl->port);

out:
	return lbc;
}

static int lb_connect(struct lb_rdma_connection *lbc,
	struct rdma_conn_param *conn_params,
	int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
	void *context)
{
	int rv = 0;
	struct nvmeib_rdma_conn_params nv_params = {};
	struct lb_rdma_listener *lbl = l_2_lb(lbc->conn.listener);
	struct lb_rdma_connection *lbc_accept;
	unsigned long flags;

	NFIN;
	if (((unsigned)conn_params->private_data_len) >
			sizeof(nv_params.private_data)) {
		_NT(trace_nvmeib_rdma_lb_connect, "Invalid private data len @PRIVATE_DATA_LEN in conn params",
		   conn_params->private_data_len);
		rv = -EINVAL;
		goto out;
	}
	if (!lbl) {
		_NT(trace_1_nvmeib_rdma_lb_connect, "lb conn @LOCAL_ID (@LBC) has no listener", lbc->local_id, lbc);
		rv = -EINVAL;
		goto out;
	}
	spin_lock_irqsave(&lbc->conn.lock, flags);
	if (lbc->state == LB_CONN_BOUND) {
		lbc->state = LB_CONN_CONNECTING;
	} else {
		_NT(trace_2_nvmeib_rdma_lb_connect, "lb conn @LOCAL_ID (@LBC) in invalid state @STATE_GUARD",
		   lbc->local_id, lbc, lbc->state);
		rv = -EINVAL;
	}
	spin_unlock_irqrestore(&lbc->conn.lock, flags);

	if (rv < 0)
		goto out;

	mutex_lock(&lbc->qp_mutex);
	if (lbc->conn.qp)
		lbc->local_qp_num = lbc->conn.qp->qp_num;
	else {
		if (!conn_params->qp_num) {
			_NE(error_nvmeib_rdma_lb_connect, "lb conn @LOCAL_ID (@LBC) has no qp and no qp_num provided",
			   lbc->local_id, lbc);
			rv = -EINVAL;
			mutex_unlock(&lbc->qp_mutex);
			goto out;
		}
		lbc->local_qp_num = conn_params->qp_num;
	}
	mutex_unlock(&lbc->qp_mutex);

	if (!(lbc_accept = lbl_add_accept_cm(lbl, lbc))) {
		spin_lock_irqsave(&lbc->conn.lock, flags);
		BUG_ON(lbc->state != LB_CONN_CONNECTING);
		lbc->state = LB_CONN_BOUND;
		spin_unlock_irqrestore(&lbc->conn.lock, flags);
		rv = -ENOMEM;
		goto out;
	}

	get_random_bytes(&lbc->send_psn, 4);
	lbc->send_psn &= 0xffffff;

	lbc->remote_id = lbc_accept->local_id;
	nv_params.d = lbl->ib_dev;
	nv_params.port = lbl->port;
	memcpy(nv_params.private_data, conn_params->private_data, conn_params->private_data_len);
	lbc->conn.event_handler = event_handler;
	lbc->conn.context = context;
	lbc->req_depth = conn_params->initiator_depth;
	lbc->resp_rsrc = conn_params->responder_resources;

	/* Init conn params */
	_NT(trace_3_nvmeib_rdma_lb_connect, "Connect from QP num: @LOCAL_QP_NUM on Device @IB_DEV_NAME Port @PORT Loopback",
		lbc->local_qp_num, lbl->ib_dev->name, lbl->port);

	/* Send new connection event to acceptor side */
	mutex_lock(&lbl->guard);
	rv = lbl->listener.new_connection(&lbc_accept->conn.cm, &nv_params);
	mutex_unlock(&lbl->guard);

	if (rv < 0) {
		/* Did not accept new connection. Remove acceptor cm from listener and destroy */
		lbc->remote_id = 0;
		spin_lock_irqsave(&lbc->conn.lock, flags);
		BUG_ON(lbc->state != LB_CONN_CONNECTING);
		lbc->state = LB_CONN_BOUND;
		spin_unlock_irqrestore(&lbc->conn.lock, flags);

		lbc_unbind(lbc_accept);
		lb_destroy_id(&lbc_accept->conn.cm);
		goto out;
	}

out:
	NFOUT;
	return rv;
}

int nvmeib_rdma_connect(struct nvmeib_rdma_cm *cm,
	struct rdma_conn_param *conn_params,
	int (*event_handler)(void *context, struct nvmeib_rdma_event *event),
	void *context)
{
	int rv = -EINVAL;

	NFIN;
	switch (cm->rdma_type) {
	case _rdma_ib:
		rv = ib_connect(cm_2_ibc(cm), conn_params, event_handler, context);
		break;
	case _rdma_roce:
		rv = roce_connect(cm_2_rocec(cm), conn_params, event_handler, context);
		break;
	case _rdma_iwarp:
		rv = iwarp_connect(cm_2_rocec(cm), conn_params, event_handler, context);
		break;
	case _rdma_lb:
		rv = lb_connect(cm_2_lbc(cm), conn_params, event_handler, context);
		break;
	case _rdma_lb_accept:
		rv = -EINVAL;
		break;
	}
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_connect);

#if IB_HAS_GID_ATTR
static int nvmeib_rdma_from_ib_gid_type(int ib_gid_type, enum nvmeib_rdma_gid_type *gid_type)
{
	int rv = 0;

	switch (ib_gid_type) {
	case IB_GID_TYPE_IB:
		*gid_type = NVMEIB_GID_TYPE_IB_ROCE_V1;
		break;
	case IB_GID_TYPE_ROCE_V2:
		*gid_type = NVMEIB_GID_TYPE_ROCE_V2;
		break;
	default:
		rv = -ENOTSUPP;
	}

	return rv;
}
#endif

static int roce_conn_get_gid_type_or_err(struct roce_rdma_connection *roce_conn)
{
	int rv = -ENOTSUPP;
#if IB_HAS_CMA_PRIV_H
	struct rdma_id_private *cma_priv = container_of(
		roce_conn->cm_id, struct rdma_id_private, id);
	rv = cma_priv->gid_type;
#endif
	return rv;
}

int nvmeib_rdma_cm_conn_get_gid_type(struct nvmeib_rdma_cm *cm, enum nvmeib_rdma_gid_type *gid_type)
{
	int rv = 0;
	struct rdma_connection *rdma_conn;
	struct roce_rdma_connection *roce_conn;

	if (!cm || cm->cm_type != _cm_connection) {
		rv = -EINVAL;
		goto out;
	}

	if (cm->rdma_type == _rdma_ib) {
		*gid_type = NVMEIB_GID_TYPE_IB_ROCE_V1;
		goto out;
	}

	if (!(rdma_conn = cm_2_c(cm)) || !(roce_conn = conn_2_roce(rdma_conn))) {
		rv = -EINVAL;
		goto out;
	}

	rv = roce_conn_get_gid_type_or_err(roce_conn);
	if (rv >= 0) {
#if IB_HAS_GID_ATTR
		enum ib_gid_type ib_gid_type = rv;
		_NT(nvmeib_rdma_cm_conn_get_gid_type_t1, "rdma_cm has default ib gid type @INT", ib_gid_type);
		rv = nvmeib_rdma_from_ib_gid_type(ib_gid_type, gid_type);
#else
		*gid_type = NVMEIB_GID_TYPE_IB_ROCE_V1;
		rv = -ENOTSUPP;
#endif
	} else {
		_NE(error_nvmeib_rdma_nvmeib_rdma_cm_conn_get_gid_type, "roce_conn_get_gid_type_or_err failed (@RV)", rv);
		*gid_type = NVMEIB_GID_TYPE_UNKNOWN;
	}

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_cm_conn_get_gid_type);

static int rdma_cm_get_int_ib_cm_id(struct rdma_cm_id *cma_id, struct ib_cm_id **ib_cm_id)
{
	int rv = -ENOTSUPP;
#if IB_HAS_CMA_PRIV_H
	struct rdma_id_private *cma_priv = container_of(
		cma_id, struct rdma_id_private, id);
	*ib_cm_id = cma_priv->cm_id.ib;
	rv = 0;
#endif
	return rv;
}

int nvmeib_rdma_cm_conn_get_ib_ids(struct nvmeib_rdma_cm *cm, u32 *local_ib_id, u32 *remote_ib_id)
{
	int rv = 0;
	struct ib_cm_id *ib_cm_id = NULL;

	if (!cm || cm->cm_type != _cm_connection) {
		rv = -EINVAL;
		goto out;
	}

	if (cm->rdma_type == _rdma_ib)
		ib_cm_id = cm_2_ibc(cm)->cm_id;
	else if (cm->rdma_type == _rdma_roce) {
		struct roce_rdma_connection *roce_conn = cm_2_rocec(cm);
		if (!roce_conn) {
			rv = -EINVAL;
			goto out;
		}
		if ((rv = rdma_cm_get_int_ib_cm_id(roce_conn->cm_id, &ib_cm_id))) {
			goto out;
		}
	}

	if (!ib_cm_id) {
		rv = -EINVAL;
		goto out;
	}

	if (local_ib_id)
		*local_ib_id = be32_to_cpu(ib_cm_id->local_id);
	if (remote_ib_id)
		*remote_ib_id = be32_to_cpu(ib_cm_id->remote_id);

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_cm_conn_get_ib_ids);

static int roce_send_rtu(struct roce_rdma_connection *conn)
{
	int rv = -ENOTSUPP;
#if IB_HAS_CMA_PRIV_H
	struct rdma_id_private *cma_id_priv = container_of(
		conn->cm_id, struct rdma_id_private, id);
	rv = ib_send_cm_rtu(cma_id_priv->cm_id.ib, NULL, 0);
#endif
	return rv;
}

#include <net/addrconf.h>
static inline void nvmeib_rdma_addrconf_ifid_eui48(u8 *eui, struct net_device *dev)
{
        memcpy(eui, dev->dev_addr, 3);
        memcpy(eui + 5, dev->dev_addr + 3, 3);
        eui[3] = 0xff;
        eui[4] = 0xfe;
        eui[0] ^= 2;
}

static inline void nvmeib_rdma_make_default_gid(struct  net_device *dev,
                                         union ib_gid *gid,
                                         bool std)
{
        gid->global.subnet_prefix = cpu_to_be64(0xfe80000000000000LL);
        if (std)
                addrconf_ifid_eui48(&gid->raw[8], dev);
        else
        	nvmeib_rdma_addrconf_ifid_eui48(&gid->raw[8], dev);
}

static int fill_rdma_port_gid(struct ib_device *ib_device, u8 port,
			      struct nvmeib_rdma_ib_port_gid *port_gid,
							  const union ib_gid *gid, const void *gid_attr_ptr)
{
	enum rdma_link_layer link_layer = port_gid->link_layer;
	struct net_device *ndev = NULL;
	bool do_ndev_put = false;
	int rv = 0;

	port_gid->gid = *gid;

	/* Get net device from either gid_attr or from get_netdev */
#if IB_HAS_GID_ATTR
	if (gid_attr_ptr) {
		const struct ib_gid_attr *gid_attr = gid_attr_ptr;
		ndev = gid_attr->ndev;
		_NT(trace_fill_rdma_port_gid_attr,
		    "gid_attr @PTR ndev @PTR", gid_attr, ndev);
		if (port_gid->transport_type == RDMA_TRANSPORT_IWARP) {
			port_gid->gid_type = NVMEIB_GID_TYPE_SOFT_IWARP;
		} else {
			switch (gid_attr->gid_type) {
				case IB_GID_TYPE_IB:
					port_gid->gid_type = NVMEIB_GID_TYPE_IB_ROCE_V1;
					break;
				case IB_GID_TYPE_ROCE_V2:
					port_gid->gid_type = NVMEIB_GID_TYPE_ROCE_V2;
					break;
				default:
					_NT(t0_fill_rdma_port_gid,
					    "unknown gid_type @INT", gid_attr->gid_type);
					rv = -EAGAIN;
					goto out;
			}
		}
	}
#else
	if (0) {
	}
#endif
	else {
		if (port_gid->transport_type == RDMA_TRANSPORT_IWARP)
			port_gid->gid_type = NVMEIB_GID_TYPE_SOFT_IWARP;
		else
			port_gid->gid_type = NVMEIB_GID_TYPE_IB_ROCE_V1;
	}
	if (!ndev && ib_device->get_netdev) {
		/* ndev is NULL (iWarp) - Try ib_device->get_netdev */
		if (IS_ERR_OR_NULL(ndev = (*ib_device->get_netdev)(ib_device, port))) {
			_NT(fill_rdma_port_gid_get_netdev_fail,
			    "get_netdev failed (@RV) for device @IB_DEVICE port @PORT_NUM",
			    PTR_ERR(ndev), ib_device->name, port);
			ndev = NULL;
		} else {
			_NT(trace_fill_rdma_port_gid_get_netdev,
			    "ndev @PTR", ndev);
			do_ndev_put = true;
		}
	}
	if (ndev) {
		union ib_gid dflt_gid;
		strncpy(port_gid->ndev_name, ndev->name, IFNAMSIZ);
		nvmeib_rdma_make_default_gid(ndev, &dflt_gid, true);
		port_gid->is_default_gid = false; //memcmp(&dflt_gid, gid, sizeof(*gid)) == 0;
		memcpy(port_gid->roce_mac, ndev->dev_addr, ETH_ALEN);
		if ((port_gid->is_vlan = is_vlan_dev(ndev)))
			port_gid->vlan_id = vlan_dev_vlan_id(ndev);
		else
			port_gid->vlan_id = 0;
		if (do_ndev_put) /* ndev is from get_netdev -> put */
			dev_put(ndev);
	}
	else {
		port_gid->ndev_name[0] = 0;
		memset(port_gid->roce_mac, 0, sizeof(port_gid->roce_mac));
		port_gid->vlan_id = 0;
		port_gid->is_vlan = false;
	}
	if (!gid->global.interface_id) {
		_NT(t1_fill_rdma_port_gid, "gid->global.interface_id=0");
		rv = -EAGAIN;
		goto out;
	}
	if (link_layer == IB_LINK_LAYER_INFINIBAND) {
		port_gid->net_type = NVMEIB_NETWORK_IB;
		port_gid->is_ipv6_link_local = false;
	} else if (link_layer == IB_LINK_LAYER_ETHERNET) {
		if (ipv6_addr_v4mapped((struct in6_addr *)gid)) {
			port_gid->net_type = NVMEIB_NETWORK_IPV4;
			port_gid->is_ipv6_link_local = false;
		} else {
			port_gid->net_type = NVMEIB_NETWORK_IPV6;
			port_gid->is_ipv6_link_local = ipv6_addr_type((struct in6_addr *)gid) & IPV6_ADDR_LINKLOCAL;
		}
	} else {
		_NT(t2_fill_rdma_port_gid,
			"unknown link-layer @INT", link_layer);
		rv = -EAGAIN;
	}

out:
	return rv;
}

char *nvmeib_rdma_gid_type_str(enum nvmeib_rdma_gid_type gid_type, enum rdma_link_layer link_layer)
{
	switch (gid_type) {
		case NVMEIB_GID_TYPE_IB_ROCE_V1:
			switch (link_layer) {
			case IB_LINK_LAYER_INFINIBAND:
				return "IB";
			case IB_LINK_LAYER_ETHERNET:
				return "RoCE_V1";
			default:
				return "Unknown";
			}
		case NVMEIB_GID_TYPE_ROCE_V2:
			return "RoCE_V2";
		case NVMEIB_GID_TYPE_SOFT_IWARP:
			return "Soft iWARP";
		default:
			return "Unknown";
	}
}
EXPORT_SYMBOL(nvmeib_rdma_gid_type_str);

char *nvmeib_rdma_net_type_str(enum nvmeib_rdma_network_type net_type)
{
	switch (net_type) {
	case NVMEIB_NETWORK_IB:
		return "IB";
	case NVMEIB_NETWORK_IPV4:
		return "IPv4";
	case NVMEIB_NETWORK_IPV6:
		return "IPv6";
	default:
		return "Unknown";
	}
}
EXPORT_SYMBOL(nvmeib_rdma_net_type_str);

const char *nvmeib_rdma_gid_ip_str(char *buffer, union ib_gid *gid, enum nvmeib_rdma_network_type net_type)
{
	switch (net_type) {
	case NVMEIB_NETWORK_IPV4:
		sprintf(buffer, "%pI4", (__be32*)&gid->global.interface_id + 1);
		break;
	case NVMEIB_NETWORK_IPV6:
		sprintf(buffer, "%pI6", &gid->raw);
		break;
	default:
		sprintf(buffer, "N/A");
		break;
	}
	return buffer;
}
EXPORT_SYMBOL(nvmeib_rdma_gid_ip_str);

int nvmeib_rdma_port_supports_gid_type(struct ib_device *ib, int port, enum nvmeib_rdma_gid_type gid_type)
{
	enum rdma_link_layer rdma_link_layer = rdma_port_get_link_layer(ib, port);
	int rv = 0;

	if (rdma_link_layer == IB_LINK_LAYER_INFINIBAND) {
		if (gid_type != NVMEIB_GID_TYPE_IB_ROCE_V1)
			rv = -ENOTSUPP;
	} else if (rdma_link_layer == IB_LINK_LAYER_ETHERNET) {
		struct ib_port_attr port_attr;
		bool port_cap_roce_v2 = false;
		bool port_cap_roce_v1 = true;
		/* Get Port Attr to determine GID table size and if Port supports RoCE V1, V2 or both */
		if ((rv = ib_query_port(ib, port, &port_attr))) {
			_NE(error_nvmeib_rdma_nvmeib_rdma_port_supports_gid_type, "ib_query_port failed (@RV)", rv);
			goto out;
		}
#if IB_HAS_GID_ATTR
#	if defined(NO_OFED) || MOFED_VERSION_GE(4,0)
		port_cap_roce_v1 = rdma_protocol_roce_eth_encap(ib, port);
		port_cap_roce_v2 = rdma_protocol_roce_udp_encap(ib, port);
#	else
		port_cap_roce_v2 = (port_attr.port_cap_flags & IB_PORT_ROCE_V2);
		port_cap_roce_v1 = (port_attr.port_cap_flags & IB_PORT_ROCE);
#	endif
#endif
		if ((gid_type == NVMEIB_GID_TYPE_IB_ROCE_V1 && !port_cap_roce_v1) ||
			(gid_type == NVMEIB_GID_TYPE_ROCE_V2 && !port_cap_roce_v2))
			rv = -ENOTSUPP;
	}

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_port_supports_gid_type);

/*TBD: Theoretically the RoCE HW GID (Default GID) does not have to be at index 0 -
 * Need to check that it matches with the result of rdma_make_default_gid
 * Question: What happens with SRIOV? */
int nvmeib_rdma_get_port_hw_gid(struct ib_device *ib, int port, union ib_gid *hw_gid)
{
	int rv;

	if (!hw_gid) {
		rv = -EINVAL;
		goto out;
	}
#if HAS_IB_QUERY_GID
#	if IB_HAS_GID_ATTR
	rv = ib_query_gid(ib, port, 0, hw_gid, NULL);
#	else
	rv = ib_query_gid(ib, port, 0, hw_gid);
#	endif
#else
	rv = rdma_query_gid(ib, port, 0, hw_gid);
#endif

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_get_port_hw_gid);
/*
 * Scan gid-table of dev:port={@ib,@port} starting from entry @start_gid_index
 * for a matching iface-name=@match_ndev_name or gid=@match_gid (or none?).
 *
 * On success, return index of the matching gid-entry and populate @rdma_port_gid.
 * For INFINIBAND the index is always 0 and rdma_port_gid->gid is hw-gid
 * For iWARP      the index is always 0 and rdma_port_gid->gid is net-dev's-IP
 * For RoCE       the index depends on other arguments: @allow_default_gid,
 *                @roce_ipv6_opt and @roce_v1_opt.
 *
 */

enum nvmeib_rdma_select_roce_v1_opt {
	NVMEIB_RDMA_SELECT_NO_ROCE_V1 = 0,
	NVMEIB_RDMA_SELECT_PREFER_ROCE_V2,
	NVMEIB_RDMA_SELECT_PREFER_ROCE_V1,
};

enum nvmeib_rdma_select_ipv6_gid_opt {
	NVMEIB_RDMA_SELECT_NO_IPV6 = 0,
	NVMEIB_RDMA_SELECT_PREFER_IPV4,
	NVMEIB_RDMA_SELECT_PREFER_IPV6,
	NVMEIB_RDMA_SELECT_ONLY_IPV6,
};

int nvmeib_rdma_select_port_gid(struct ib_device *ib, int port, struct ib_port_attr *port_attr,
								int start_gid_index,
								struct nvmeib_rdma_ib_port_gid *rdma_port_gid,
								bool allow_default_gid, bool allow_ipv6_link_local,
								const char *match_ndev_name,
								const union ib_gid *match_hw_gid,
								const union ib_gid *match_sw_gid_value,
								const union ib_gid *match_sw_gid_mask)
{
	int i = 0, rv = -ENOENT;
	bool port_cap_roce_v2 __attribute__((unused)) = false;
	bool port_cap_roce_v1 __attribute__((unused)) = false;
	void *gid_attr_ptr = NULL;
	struct ib_port_attr port_attr_on_stack;
	bool match_by_ndev_name = match_ndev_name && strlen(match_ndev_name) > 0;
#if HAS_IB_QUERY_GID && IB_HAS_GID_ATTR
	struct ib_gid_attr gid_attr = {0};
#endif
	u8 zero_mac[ETH_ALEN] = {0};
	bool matched_hw_gid = false;
	union ib_gid gid;
	enum nvmeib_rdma_select_ipv6_gid_opt ipv6_opt = nvmeib_ipv6_mode;
	enum nvmeib_rdma_select_roce_v1_opt roce_v1_opt = NVMEIB_RDMA_SELECT_NO_ROCE_V1; /* RoCE v1 is deprecated */
	union ib_gid ff_gid = {
		.global = {
			.interface_id = ~(u64)0,
			.subnet_prefix = ~(u64)0,
		},
	};

	NFIN;

	if (!match_sw_gid_value) {
		/* Not matching by SW GID */
		match_sw_gid_value = &zgid;
		match_sw_gid_mask = &zgid;
	} else if (!match_sw_gid_mask) {
		/* Default mask is all bits */
		match_sw_gid_mask = &ff_gid;
	}

	_NT(t0_nvmeib_rdma_select_port_gid,
		"Device=@IB_NAME, port=@PORT (start_gid_index=@INT, "
		"allow_default_gid=@BOOL, ipv6_opt=@INT, roce_v1_opt=@INT),"
		"match_ndev_name=@BOOL_YN(@NDEV_NAME), "
		"match_hw_gid=@BOOL_YN(@GID),"
		"match_sw_gid_value=@GID,"
		"match_sw_gid_mask=@GID,",
		ib->name, port, start_gid_index, allow_default_gid,
		ipv6_opt, roce_v1_opt,
		!!match_by_ndev_name, match_by_ndev_name ? match_ndev_name : "n/a",
		!!match_hw_gid, match_hw_gid ? match_hw_gid : &zgid,
		match_sw_gid_value, match_sw_gid_mask);

	if (!port_attr) {
		/* Get Port Attr to determine GID table size and if Port supports RoCE V1, V2 or both */
		port_attr = &port_attr_on_stack;
		if ((rv = ib_query_port(ib, port, port_attr))) {
			_NE(error_nvmeib_rdma_nvmeib_rdma_select_port_gid, "ib_query_port failed (@RV)", rv);
			goto out;
		}
	}

	if ((rv = nvmeib_rdma_get_port_hw_gid(ib, port, &rdma_port_gid->hw_gid)) < 0) {
		_NE(err_nvmeib_rdma_select_port_gid_hw_gid_fail, "nvmeib_rdma_get_port_hw_gid failed (@RV)", rv);
		goto out;
	}

	rdma_port_gid->link_layer = rdma_port_get_link_layer(ib, port);
	_NT(trace_nvmeib_rdma_nvmeib_rdma_select_port_gid, "link_layer=@LINK_LAYER", rdma_port_gid->link_layer);

	rdma_port_gid->transport_type = rdma_node_get_transport(ib->node_type);
	_NT(trace_1_nvmeib_rdma_nvmeib_rdma_select_port_gid, "transport_type=@TRANSPORT_TYPE_INT", rdma_port_gid->transport_type);

	if (match_hw_gid) {
		/* For IB, we only match the interface_id as the subnet_prefix can be changed by the Subnet Manager */
		if (match_hw_gid->global.interface_id == rdma_port_gid->hw_gid.global.interface_id &&
			(rdma_port_gid->link_layer == IB_LINK_LAYER_INFINIBAND ||
				match_hw_gid->global.subnet_prefix == rdma_port_gid->hw_gid.global.subnet_prefix))
		{
			matched_hw_gid = true;
		}
	}

	switch (rdma_port_gid->link_layer) {
	case IB_LINK_LAYER_INFINIBAND:
		/* ------------------------------------------------------------------ *
		 * IB                           									  *
		 * -------------------------------------------------------------------*/
		if (start_gid_index > 0) {
			/* IB only has GID[0]*/
			rv = -ENOENT;
			goto out;
		}
		if (match_by_ndev_name) {
			/* IB cannot be matched by netdev */
			rv = -EINVAL;
			goto out;
		}
		if (match_hw_gid && !matched_hw_gid) {
			rv = -ENOENT;
			goto out;
		}
		rdma_port_gid->gid_index = 0;
		rdma_port_gid->gid_type = NVMEIB_GID_TYPE_IB_ROCE_V1;
		rdma_port_gid->net_type = NVMEIB_NETWORK_IB;
		rdma_port_gid->gid = rdma_port_gid->hw_gid;
		rdma_port_gid->is_vlan = false;
		rdma_port_gid->vlan_id = 0;
		rdma_port_gid->is_default_gid = true;
		format_gid_raw(rdma_port_gid->gid.raw, rdma_port_gid->gid_str);
		rdma_port_gid->valid = true;
		rdma_port_gid->preferred = true;
		goto end_loop;

	case IB_LINK_LAYER_ETHERNET:
		if (start_gid_index >= port_attr->gid_tbl_len) {
			/* Out of range for GID table */
			rv = -ENOENT;
			goto out;
		}
		break;

	case IB_LINK_LAYER_UNSPECIFIED:
		_NT(trace_4_nvmeib_rdma_nvmeib_rdma_select_port_gid, "Device @IB_NAME Port @PORT has unspecified link layer", ib->name, port);
		rv = -EINVAL;
		goto out;

	default:
		rv = -EINVAL;
		_NT(trace_5_nvmeib_rdma_nvmeib_rdma_select_port_gid, "Device @IB_NAME Port @PORT has invalid link layer @LINK_LAYER", ib->name, port, rdma_port_gid->link_layer);
		goto out;
	}

	/* ----------------------------------------------------------------------- *
	 *  																	   *
	 * From here, we are RoCE only  										   *
	 *  																	   *
	 * ------------------------------------------------------------------------*/
#	if defined(NO_OFED) || MOFED_VERSION_GE(4,0)
	port_cap_roce_v1 = rdma_protocol_roce_eth_encap(ib, port);
	port_cap_roce_v2 = rdma_protocol_roce_udp_encap(ib, port);
#else
	port_cap_roce_v2 = (port_attr->port_cap_flags & IB_PORT_ROCE_V2);
	port_cap_roce_v1 = (port_attr->port_cap_flags & IB_PORT_ROCE);
#endif

	rdma_port_gid->valid = false;
	for (i = start_gid_index; i < port_attr->gid_tbl_len; i++) {
		if (is_siw_ib_dev(ib)) {
			/* Work-around for ib-cache not dispatching GID change for non-ROCE */
#if KS_IB_DEVICE_HAS_DEVICE_OPS
			rv = ib->ops.query_gid(ib, port, i, &gid);
#else
			rv = ib->query_gid(ib, port, i, &gid);
#endif
		} else {
#if HAS_IB_QUERY_GID
#	if IB_HAS_GID_ATTR
			if (rdma_port_gid->transport_type != RDMA_TRANSPORT_IWARP)
				gid_attr_ptr = &gid_attr;
			rv = ib_query_gid(ib, port, i, &gid, gid_attr_ptr);
#	else
			rv = ib_query_gid(ib, port, i, &gid);
#	endif
#else
#	if IB_HAS_GID_ATTR
#		if IB_HAS_RDMA_GET_GID_ATTR
			rv = rdma_query_gid(ib, port, i, &gid);
			if (!rv && rdma_port_gid->link_layer == IB_LINK_LAYER_ETHERNET)
				if (IS_ERR(gid_attr_ptr = (void *)rdma_get_gid_attr(ib, port, i))) {
					_NT(trace_nvmeib_rdma_select_port_gid_attr_fail,
					"rdma_get_gid_attr failed (@RV) device @IB_DEVICE",
					rv, ib->name);
					rv = PTR_ERR(gid_attr_ptr);
				}
#		else
				gid_attr_ptr = rdma_port_gid->link_layer == IB_LINK_LAYER_ETHERNET ? &gid_attr : NULL;
				rv = ib_get_cached_gid(ib, port, i, &gid, gid_attr_ptr);
#		endif
#	else
			rv = rdma_query_gid(ib, port, i, &gid);
#	endif
#endif
		}
		if (rv != 0) {
			_NT(nvmeib_rdma_select_port_gid_e3, "[@INT] ib_query_gid failed (@INT)", i, rv);
			continue;
		}
		_NT(nvmeib_rdma_select_port_gid_query_gid, "[@INT] ib_query_gid - @GUID_RAW)", i, &gid);
		if (memcmp(&gid, &zgid, sizeof(gid)) == 0) {
			_ND(debug_2_nvmeib_rdma_select_port_gid, "Skipping Zero GID[@INDEX]", i);
			goto next_gid;
		}

		/* conditionally fill these members:
		 * &rdma_port_gid->gid_type
		 * &rdma_port_gid->ndev_name
		 * &rdma_port_gid->is_default_gid
		 * &rdma_port_gid->roce_mac
		 * &rdma_port_gid->is_vlan
		 * &rdma_port_gid->vlan_id
		 * &rdma_port_gid->net_type
		 */
		if ((rv = fill_rdma_port_gid(ib, port, rdma_port_gid, &gid, gid_attr_ptr))) {
			_NE(e4_select_port_gid, "empty GID table");
			if (rv != -EAGAIN)
				_NT(trace_nvmeib_rdma_select_port_gid_fill_rdma_port_gid_failed,
					"nvmeib_get_gid_type failed (@RV) for gid index @INDEX", rv, i);
			goto next_gid;
		}
		rdma_port_gid->is_default_gid = i == 0;
		if (rdma_port_gid->is_default_gid && !allow_default_gid) {
				_NT(trace_nvmeib_rdma_select_port_gid_hw_gid_match,
				    "Skip default GID @GUID_RAW", &gid);
			goto next_gid;
		}
		if (rdma_port_gid->is_ipv6_link_local && !allow_ipv6_link_local) {
			_NT(trace_nvmeib_rdma_select_port_gid_ipv6_link_local,
			    "Skip IPv6 Link-Local GID @GUID_RAW", &gid);
			goto next_gid;
		}

		/* in SRIOV each gid/gid-range may have its own mac-addr */
		if (memcmp(rdma_port_gid->roce_mac, zero_mac, ETH_ALEN) == 0) {
			_NW(warn_nvmeib_rdma_select_port_gid_zero_mac,
			"RoCE GID [@INDEX] @GUID_RAW ndev: @NDEV_NAME has zero MAC address",
			i, &rdma_port_gid->gid, rdma_port_gid->ndev_name);
			goto next_gid;
		}

		if (((match_sw_gid_value->global.interface_id & match_sw_gid_mask->global.interface_id) !=
				(rdma_port_gid->gid.global.interface_id & match_sw_gid_mask->global.interface_id) ||
			(match_sw_gid_value->global.subnet_prefix & match_sw_gid_mask->global.subnet_prefix) !=
				(rdma_port_gid->gid.global.subnet_prefix & match_sw_gid_mask->global.subnet_prefix)))
		{
			_ND(debug_3_nvmeib_rdma_select_port_gid,
			"GID[@INDEX] not matched - @GID & @GID != @GID & @GID",
			i, match_sw_gid_value, match_sw_gid_mask, &rdma_port_gid->gid, match_sw_gid_mask);
			goto next_gid;
		}

		/* different gids may have different iface-name,
		   e.g. VLANS (ens1f0.100 and ens1f0.150) */
		if (match_by_ndev_name) {
			if (strncmp(rdma_port_gid->ndev_name, match_ndev_name, IFNAMSIZ) != 0) {
				_ND(debug_4_nvmeib_rdma_select_port_gid,
				"GID[@INDEX] netdev not matched @NDEV_NAME != @NDEV_NAME", i,
				   match_ndev_name, rdma_port_gid->ndev_name);
				goto next_gid;
			}
		}
		if (rdma_port_gid->gid_type == NVMEIB_GID_TYPE_IB_ROCE_V1 &&
			roce_v1_opt == NVMEIB_RDMA_SELECT_NO_ROCE_V1) {
			_ND(debug_5_nvmeib_rdma_select_port_gid, "Skipping RoCEv1 GID[@INDEX] - @GUID_RAW", i, &gid);
			goto next_gid;
		}
		if (rdma_port_gid->net_type == NVMEIB_NETWORK_IPV6 &&
			ipv6_opt == NVMEIB_RDMA_SELECT_NO_IPV6) {
			_ND(debug_6_nvmeib_rdma_select_port_gid, "Skipping IPv6 GID[@INDEX] - @GUID_RAW", i, &gid);
			goto next_gid;
		}
		if (rdma_port_gid->net_type == NVMEIB_NETWORK_IPV4 &&
			ipv6_opt == NVMEIB_RDMA_SELECT_ONLY_IPV6) {
			_ND(debug_7_nvmeib_rdma_select_port_gid, "Skipping IPv4 GID[@INDEX] - @GUID_RAW", i, &gid);
			goto next_gid;
		}
		/* GID has passed our match filters */
		rdma_port_gid->gid_index = i;
		format_gid_raw(rdma_port_gid->gid.raw, rdma_port_gid->gid_str);
		rdma_port_gid->valid = true;

		rdma_port_gid->preferred = true;
		switch (ipv6_opt) {
		case NVMEIB_RDMA_SELECT_NO_IPV6:
		case NVMEIB_RDMA_SELECT_PREFER_IPV4:
			if (rdma_port_gid->net_type != NVMEIB_NETWORK_IPV4)
				rdma_port_gid->preferred = false;
			break;
		case NVMEIB_RDMA_SELECT_PREFER_IPV6:
		case NVMEIB_RDMA_SELECT_ONLY_IPV6:
			if (rdma_port_gid->net_type != NVMEIB_NETWORK_IPV6)
				rdma_port_gid->preferred = false;
			break;
		default:
			BUG();
		}
		/* RoCE is Transport IB over Link-Layer Ethernet */
		if (rdma_port_gid->transport_type == RDMA_TRANSPORT_IB && rdma_port_gid->link_layer == IB_LINK_LAYER_ETHERNET) {
			switch (roce_v1_opt) {
				case NVMEIB_RDMA_SELECT_NO_ROCE_V1:
				case NVMEIB_RDMA_SELECT_PREFER_ROCE_V2:
					if (rdma_port_gid->gid_type != NVMEIB_GID_TYPE_ROCE_V2)
						rdma_port_gid->preferred = false;
					break;
				case NVMEIB_RDMA_SELECT_PREFER_ROCE_V1:
					if (rdma_port_gid->gid_type != NVMEIB_GID_TYPE_IB_ROCE_V1)
						rdma_port_gid->preferred = false;
					break;
				default:
					BUG();
			}
		}

next_gid:
#if IB_HAS_RDMA_GET_GID_ATTR
		if (gid_attr_ptr)
			rdma_put_gid_attr((const struct ib_gid_attr *)gid_attr_ptr);
#elif IB_QUERY_ROCE_GID_DOES_DEV_HOLD
		if (gid_attr_ptr && gid_attr.ndev)
			dev_put(gid_attr.ndev);
#endif
		if (rdma_port_gid->valid)
			break;
	}
	_ND(trace_6_1_nvmeib_rdma_nvmeib_rdma_select_port_gid,
		"scanned entries [@INDEX, @INDEX)", start_gid_index, i);

end_loop:

	if (!rdma_port_gid->valid) {
		_NT(trace_9_nvmeib_rdma_nvmeib_rdma_select_port_gid_no_valid_gid,
			"No match found from [@INDEX, @INDEX) for @IB_NAME:@PORT, rv=@RV",
			   start_gid_index, port_attr->gid_tbl_len, ib->name, port, rv);
		rv = -ENOENT;
	} else {
		rv = rdma_port_gid->gid_index;
		_NT(trace_10_nvmeib_rdma_nvmeib_rdma_select_port_gid,
			"Match: @IB_NAME:@PORT, matching GID[@INDEX]=@GUID_RAW - "
			"gid-type=@NVMEIB_RDMA_GID_TYPE_STR, "
			"net-type=@NVMEIB_RDMA_NET_TYPE_STR, "
			"dev-name=@NDEV_NAME, MAC=@MAC, vlanID=@VLAN_ID, prefered=@STR,"
			"rv=@RV",
			ib->name, port, i, &rdma_port_gid->gid,
			nvmeib_rdma_gid_type_str(rdma_port_gid->gid_type, rdma_port_gid->link_layer),
			nvmeib_rdma_net_type_str(rdma_port_gid->net_type),
			(strlen(rdma_port_gid->ndev_name) ? rdma_port_gid->ndev_name : "n/a"),
			rdma_port_gid->roce_mac, rdma_port_gid->vlan_id, rdma_port_gid->preferred ? "Yes" : "No", rv);
	}

out:
	_NT(trace_11_nvmeib_rdma_select_port_gid, "Return @RV for Dev @IB_NAME Port @PORT GID[@INDEX]",
			rv, ib->name, port, start_gid_index);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_select_port_gid);

void nvmeib_rdma_print_path(struct ib_sa_path_rec *path)
{
	char sgid[GUID_SIZE] = {0};
	char dgid[GUID_SIZE] = {0};

	format_gid(&path->sgid, sgid);
	format_gid(&path->dgid, dgid);
	_ND(trace_nvmeib_rdma_nvmeib_rdma_print_path, "service_id=@SERVICE_ID", path->service_id);
	_ND(trace_1_nvmeib_rdma_nvmeib_rdma_print_path, "sgid=@SGID_STR", sgid);
	_ND(trace_2_nvmeib_rdma_nvmeib_rdma_print_path, "dgid=@DGID_STR", dgid);
	_ND(trace_3_nvmeib_rdma_nvmeib_rdma_print_path, "slid=@SLID", NVMEIB_PATH_REC_SLID(*path));
	_ND(trace_4_nvmeib_rdma_nvmeib_rdma_print_path, "dlid=@DLID", NVMEIB_PATH_REC_DLID(*path));
	_ND(trace_5_nvmeib_rdma_nvmeib_rdma_print_path, "flow_label=@FLOW_LABEL", path->flow_label);
	_ND(trace_6_nvmeib_rdma_nvmeib_rdma_print_path, "hop_limit=@HOP_LIMIT", (int)path->hop_limit);
	_ND(trace_7_nvmeib_rdma_nvmeib_rdma_print_path, "traffic_class=@TRAFFIC_CLASS", (int)path->traffic_class);
	_ND(trace_8_nvmeib_rdma_nvmeib_rdma_print_path, "reversible=@REVERSIBLE", (int)path->reversible);
	_ND(trace_9_nvmeib_rdma_nvmeib_rdma_print_path, "numb_path=@NUMB_PATH", (int)path->numb_path);
	_ND(trace_10_nvmeib_rdma_nvmeib_rdma_print_path, "pkey=@PKEY", path->pkey);
	_ND(trace_11_nvmeib_rdma_nvmeib_rdma_print_path, "qos_class=@QOS_CLASS", path->qos_class);
	_ND(trace_12_nvmeib_rdma_nvmeib_rdma_print_path, "sl=@SL", (int)path->sl);
	_ND(trace_13_nvmeib_rdma_nvmeib_rdma_print_path, "mtu_selector=@MTU_SELECTOR", (int)path->mtu_selector);
	_ND(trace_14_nvmeib_rdma_nvmeib_rdma_print_path, "mtu=@MTU", (int)path->mtu);
	_ND(trace_15_nvmeib_rdma_nvmeib_rdma_print_path, "rate_selector=@RATE_SELECTOR", (int)path->rate_selector);
	_ND(trace_16_nvmeib_rdma_nvmeib_rdma_print_path, "rate=@RATE", (int)path->rate);
	_ND(trace_17_nvmeib_rdma_nvmeib_rdma_print_path, "packet_life_time_selector=@PACKET_LIFE_TIME_SELECTOR", (int)path->packet_life_time_selector);
	_ND(trace_18_nvmeib_rdma_nvmeib_rdma_print_path, "packet_life_time=@PACKET_LIFE_TIME", (int)path->packet_life_time);
	_ND(trace_19_nvmeib_rdma_nvmeib_rdma_print_path, "preference=@PREFERENCE", (int)path->preference);
}
EXPORT_SYMBOL(nvmeib_rdma_print_path);

struct event_handler_port_data {
	struct net_device *ndev;
	bool last_carrier_ok;
};

struct event_handler_private {
	struct list_head nb_dev_link;
	struct nvmeib_rdma_event_handler event_handler;
	struct ib_event_handler ib_event_handler;
	unsigned n_ndev_ports;
	struct event_handler_port_data port_data[];
};

static void internal_ib_event_handler(struct ib_event_handler *handler,
	struct ib_event *event)
{
	struct event_handler_private *handler_private =
		container_of(handler, struct event_handler_private, ib_event_handler);
	struct ib_device *ib_dev = handler_private->event_handler.ib_dev;

#if IB_DEVICE_HAS_GET_NETDEV
	if ((event->event == IB_EVENT_PORT_ACTIVE || event->event == IB_EVENT_PORT_ERR) &&
		rdma_port_get_link_layer(ib_dev, event->element.port_num) == IB_LINK_LAYER_ETHERNET &&
		handler_private->port_data[event->element.port_num].ndev &&
		!is_siw_ib_dev(ib_dev))
	{
		_NT(trace_nvmeib_rdma_internal_ib_event_handler, "Masking IB event @EVENT on RoCE Port @IB_DEV_NAME:@PORT_NUM", event->event,
		   ib_dev->name, event->element.port_num);
		return;
	}
#endif

	(*handler_private->event_handler.handler_fn)(&handler_private->event_handler, event);
}

#if IB_DEVICE_HAS_GET_NETDEV
static struct notifier_block nvmeib_rdma_net_nb;
static struct notifier_block nvmeib_rdma_inet_nb;
static struct notifier_block nvmeib_rdma_inet6_nb;
static DEFINE_RWLOCK(nvmeib_rdma_net_nb_dev_lock);
static LIST_HEAD(nvmeib_rdma_net_nb_dev_head);

static void __internal_any_inet_event(struct event_handler_private *handler_private, struct net_device *ndev, u8 port) {
	struct ib_device *ib_dev = handler_private->event_handler.ib_dev;
	struct ib_event ib_event;
	char fn_name[64];


	/* SIW use ib_device without ROCE cap so IB_EVENT_GID_CHANGE when iface
	 *	address is change are not publish on new kernels */
	if (is_siw_ib_dev(ib_dev)) {
		BUG_ON(port != 1); /* siw always use 1 port with index 1 */
		ib_event.element.port_num = port;
		ib_event.event = IB_EVENT_GID_CHANGE;
		ib_event.device = ib_dev;

		scnprintf(fn_name, sizeof(fn_name), "%ps", handler_private->event_handler.handler_fn);
		_NI(internal_any_inet_event_publish,
		    "Calling handler function @FUNCTION for internal "
		    "IB_EVENT_GID_CHANGE event of @IB_DEVICE:@PORT",
			fn_name, ib_dev->name, (int)port);
		(*handler_private->event_handler.handler_fn)(&handler_private->event_handler, &ib_event);
	}
}

static void internal_any_inet_event(struct net_device *ndev) {
	struct event_handler_private *iter;
	int i;
	unsigned long flags;

	_ND(trace_internal_any_inet_event,
	    "Processing netdev @NETDEV_NAME any inet event", netdev_name(ndev));

	read_lock_irqsave(&nvmeib_rdma_net_nb_dev_lock, flags);
	list_for_each_entry(iter, &nvmeib_rdma_net_nb_dev_head, nb_dev_link) {
		struct ib_device *ib_dev = iter->event_handler.ib_dev;
		for (i = 1; i <= ib_dev->phys_port_cnt; i++) {
			if (iter->port_data[i].ndev == ndev) {
				_ND(trace_internal_any_inet_event_match,
				    "Matched netdev @NETDEV_NAME with @IB_DEVICE:@PORT",
					netdev_name(ndev), ib_dev->name, i);
				__internal_any_inet_event(iter, ndev, i);
			}
		}
	}
	read_unlock_irqrestore(&nvmeib_rdma_net_nb_dev_lock, flags);
}

static int internal_inet_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *ndev = ((struct in_ifaddr *)(ptr))->ifa_dev->dev;

	_ND(trace_internal_inet_event,
	    "Processing netdev @NETDEV_NAME inet event @EVENT_LONG", netdev_name(ndev), event);

	internal_any_inet_event(ndev);

	return NOTIFY_OK;
}

static int internal_inet6_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *ndev = ((struct inet6_ifaddr *)(ptr))->idev->dev;

	_ND(trace_internal_inet6_event,
	    "Processing netdev @NETDEV_NAME inet6 event @EVENT_LONG", netdev_name(ndev), event);

	internal_any_inet_event(ndev);

	return NOTIFY_OK;
}

static int internal_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct event_handler_private *handler_private = NULL;
	struct ib_device *ib_dev = NULL;
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct event_handler_port_data *port_data = NULL;
	struct ib_event ib_event;
	int i, port = -1;
	bool carrier_ok;
	unsigned long flags;

	NFIN;
	if (!ndev) {
		_NT(trace_nvmeib_rdma_internal_netdev_event, "Called with NULL netdev");
		goto out;
	}

	read_lock_irqsave(&nvmeib_rdma_net_nb_dev_lock, flags);

	_ND(trace_1_nvmeib_rdma_internal_netdev_event, "Processing netdev @NETDEV_NAME event @EVENT_LONG", netdev_name(ndev), event);

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
	case NETDEV_UNREGISTER:
		break;
	default:
		goto unlock;
	}
	list_for_each_entry(handler_private, &nvmeib_rdma_net_nb_dev_head, nb_dev_link) {
		ib_dev = handler_private->event_handler.ib_dev;
		if (!handler_private->n_ndev_ports)
			continue;
		for (i = 1; i <= ib_dev->phys_port_cnt; i++) {
			port_data = handler_private->port_data;
			if (!port_data[i].ndev)
				continue;
			_ND(internal_netdev_event_d1, "Comparing netdev @STR (@PTR) with IB device @STR:@INT netdev @STR (@PTR)",
				netdev_name(ndev), ndev, ib_dev->name, i, netdev_name(port_data[i].ndev), port_data[i].ndev);
			if (port_data[i].ndev != ndev)
				continue;
			_ND(internal_netdev_event_d2, "Matched event @EVENT_LONG on ndev @STR to IB device @STR:@INT", event, netdev_name(ndev),
				ib_dev->name, i);
			port = i;
			if (event == NETDEV_UNREGISTER) {
#if IB_QUERY_ROCE_GID_DOES_DEV_HOLD
				dev_put(port_data[port].ndev);
#endif
				port_data[port].ndev = NULL;
				handler_private->n_ndev_ports--;
				if (!handler_private->n_ndev_ports)
					break;
				continue;
			}

			ib_event.device = ib_dev;
			ib_event.element.port_num = port;
			carrier_ok = (netif_running(ndev) && netif_carrier_ok(ndev));
			switch (event) {
			case NETDEV_UP:
				ib_event.event = IB_EVENT_PORT_ACTIVE;
				port_data[port].last_carrier_ok = carrier_ok;
				break;
			case NETDEV_DOWN:
				ib_event.event = IB_EVENT_PORT_ERR;
				port_data[port].last_carrier_ok = carrier_ok;
				break;
			case NETDEV_CHANGE:
				if (port_data[port].last_carrier_ok == carrier_ok) {
					_ND(trace_2_nvmeib_rdma_internal_netdev_event, "netdev @NDEV_NAME, carrier state didn't change", ndev->name);
					goto unlock;
				}
				ib_event.event = carrier_ok ? IB_EVENT_PORT_ACTIVE : IB_EVENT_PORT_ERR;
				port_data[port].last_carrier_ok = carrier_ok;
				break;
			default:
				/* All other cases should have been taken care of already */
				BUG_ON(1);
			}
			_NT(trace_3_nvmeib_rdma_internal_netdev_event,
			    "Translated netdev @NETDEV_NAME event @EVENT_LONG to"
			    " IB device @IB_DEV_NAME:@PORT event @EVENT",
				netdev_name(ndev), event, ib_dev->name, port, ib_event.event);
			(*handler_private->event_handler.handler_fn)(&handler_private->event_handler, &ib_event);
		}
	}

unlock:
	read_unlock_irqrestore(&nvmeib_rdma_net_nb_dev_lock, flags);

out:
	NFOUT;
	return NOTIFY_DONE;
}

int nvmeib_rdma_register_net_notifiers(void)
{
	int rv;

	NFIN;
	nvmeib_rdma_net_nb.notifier_call = internal_netdev_event;
	/* We ensure getting the events last after all other modules
	 *  run their callbacks */
	nvmeib_rdma_net_nb.priority = INT_MIN;
	#if KS_HAVE_REGISTER_NETDEVICE_NOTIFIER_RH
	rv = register_netdevice_notifier_rh(&nvmeib_rdma_net_nb);
	#else
	rv = register_netdevice_notifier(&nvmeib_rdma_net_nb);
	#endif
	if (rv < 0) {
		_NW(nvmeib_rdma_register_net_event_handler_w1, "register_netdevice_notifier() failed(@RV).", rv);
		nvmeib_rdma_net_nb.notifier_call = NULL;
		goto err_out;
	}

	nvmeib_rdma_inet_nb.notifier_call = internal_inet_event;
	nvmeib_rdma_inet_nb.priority = INT_MIN;
	if ((rv = register_inetaddr_notifier(&nvmeib_rdma_inet_nb)) < 0) {
		_NW(nvmeib_rdma_register_net_notifiers_inetaddr_notifier_f, "register_inetaddr_notifier() failed(@RV).", rv);
		nvmeib_rdma_inet_nb.notifier_call = NULL;
		goto err_out;
	}
	nvmeib_rdma_inet6_nb.notifier_call = internal_inet6_event;
	nvmeib_rdma_inet6_nb.priority = INT_MIN;
	if ((rv = register_inet6addr_notifier(&nvmeib_rdma_inet6_nb)) < 0) {
		_NW(nvmeib_rdma_register_net_notifiers_inetaddr6_notifier_f, "register_inetaddr6_notifier() failed(@RV).", rv);
		nvmeib_rdma_inet6_nb.notifier_call = NULL;
		goto err_out;
	}

	rv = 0;
	goto out;

err_out:
	nvmeib_rdma_unregister_net_notifiers();

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_register_net_notifiers);

void nvmeib_rdma_unregister_net_notifiers(void)
{
	int rv;
	if (nvmeib_rdma_net_nb.notifier_call) {
		#if KS_HAVE_REGISTER_NETDEVICE_NOTIFIER_RH
		rv = unregister_netdevice_notifier_rh(&nvmeib_rdma_net_nb);
		#else
		rv = unregister_netdevice_notifier(&nvmeib_rdma_net_nb);
		#endif
		if (rv < 0) {
			_NW(nvmeib_rdma_unregister_net_notifiers_w1, "unregister_netdevice_notifier() failed(@RV).", rv);
		}
	}

	if (nvmeib_rdma_inet_nb.notifier_call) {
		rv = unregister_inetaddr_notifier(&nvmeib_rdma_inet_nb);
		if (rv < 0) {
			_NW(nvmeib_rdma_unregister_net_notifiers_w2, "unregister_inetaddr_notifier() failed(@RV).", rv);
		}
	}

	if (nvmeib_rdma_inet6_nb.notifier_call) {
		rv = unregister_inet6addr_notifier(&nvmeib_rdma_inet6_nb);
		if (rv < 0) {
			_NW(nvmeib_rdma_unregister_net_notifiers_w3, "unregister_inet6addr_notifier() failed(@RV).", rv);
		}
	}

	WARN_ON(!list_empty(&nvmeib_rdma_net_nb_dev_head));
}
EXPORT_SYMBOL(nvmeib_rdma_unregister_net_notifiers);

#else //IB_DEVICE_HAS_GET_NETDEV
int nvmeib_rdma_register_net_notifiers(void)
{
	return 0;
}
EXPORT_SYMBOL(nvmeib_rdma_register_net_notifiers);

void nvmeib_rdma_unregister_net_notifiers(void)
{
	return;
}
EXPORT_SYMBOL(nvmeib_rdma_unregister_net_notifiers);

#endif //IB_DEVICE_HAS_GET_NETDEV

int nvmeib_rdma_register_event_handler(struct ib_device *ib_dev,
				       nvmeib_rdma_event_handler_fn handler_fn,
				       void *ctx, struct nvmeib_rdma_event_handler **handler)
{
	struct event_handler_private *handler_private = NULL;
	unsigned long flags;
	int rv, i;
	char fn_name[64];

	NFIN;
	if (!ib_dev || !handler_fn || !handler) {
		rv = -EINVAL;
		goto out;
	}
	*handler = NULL;

	scnprintf(fn_name, sizeof(fn_name), "%ps", handler_fn);
	_NT(trace_nvmeib_rdma_nvmeib_rdma_register_event_handler, "Registering event handler @FUNCTION for IB device @IB_DEV_NAME (num ports @PHYS_PORT_CNT)",
	   fn_name, ib_dev->name, ib_dev->phys_port_cnt);
	if (!(handler_private = kzalloc(sizeof(struct event_handler_private) +
		((ib_dev->phys_port_cnt + 1) * sizeof(struct event_handler_port_data)), GFP_KERNEL))) {
		_NE(error_nvmeib_rdma_nvmeib_rdma_register_event_handler, "Memory allocation error");
		rv = -ENOMEM;
		goto out;
	}
	handler_private->event_handler.ib_dev = ib_dev;
	handler_private->event_handler.handler_fn = handler_fn;
	handler_private->event_handler.ctx = ctx;
	INIT_LIST_HEAD(&handler_private->nb_dev_link);

	INIT_IB_EVENT_HANDLER(&handler_private->ib_event_handler, ib_dev,
		internal_ib_event_handler);

	ib_register_event_handler(&handler_private->ib_event_handler);

#if IB_DEVICE_HAS_GET_NETDEV
	if (ib_dev->get_netdev) {
		for (i = 1; i <= ib_dev->phys_port_cnt; i++) {
			if (rdma_port_get_link_layer(ib_dev, i) == IB_LINK_LAYER_ETHERNET &&
					(handler_private->port_data[i].ndev = ib_dev->get_netdev(ib_dev, i))) {
				_NT(nvmeib_rdma_register_event_handler_t1, "Matched netdev @NETDEV_NAME to IB device @IB_DEVICE:@PORT",
				   netdev_name(handler_private->port_data[i].ndev), ib_dev->name, i);
				handler_private->port_data[i].last_carrier_ok =
					netif_carrier_ok(handler_private->port_data[i].ndev);
				handler_private->n_ndev_ports++;
			}
		}

		if (handler_private->n_ndev_ports) {
			write_lock_irqsave(&nvmeib_rdma_net_nb_dev_lock, flags);
			list_add_tail(&handler_private->nb_dev_link, &nvmeib_rdma_net_nb_dev_head);
			write_unlock_irqrestore(&nvmeib_rdma_net_nb_dev_lock, flags);
		}
	}
#endif //IB_DEVICE_HAS_GET_NETDEV
	rv = 0;

	*handler = &handler_private->event_handler;
out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_register_event_handler);

int nvmeib_rdma_unregister_event_handler(struct nvmeib_rdma_event_handler *handler)
{
	struct event_handler_private *handler_private;
	struct event_handler_port_data *port_data;
	int rv = 0;

	NFIN;

	if (!handler)
		goto out;

	handler_private =
		container_of(handler, struct event_handler_private, event_handler);

#if IB_DEVICE_HAS_GET_NETDEV
	if (!list_empty(&handler_private->nb_dev_link)) {
		struct ib_device *ib_dev = handler->ib_dev;
		int i;
		unsigned long flags;

		write_lock_irqsave(&nvmeib_rdma_net_nb_dev_lock, flags);
		list_del_init(&handler_private->nb_dev_link);
		write_unlock_irqrestore(&nvmeib_rdma_net_nb_dev_lock, flags);

		port_data = &handler_private->port_data[1];
		for (i = 1; i <= ib_dev->phys_port_cnt; i++, port_data++) {
			if (port_data->ndev) {
#if IB_QUERY_ROCE_GID_DOES_DEV_HOLD
				dev_put(port_data->ndev);
#endif
				port_data->ndev = NULL;
			}
		}
	}
#endif // IB_DEVICE_HAS_GET_NETDEV

/*	rv = ib_unregister_event_handler(&handler_private->ib_event_handler); */
	ib_unregister_event_handler(&handler_private->ib_event_handler);
	kfree(handler_private);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_unregister_event_handler);

static ssize_t chng_qp_err(void *arg, char *buf, size_t len)
{
	struct ib_qp *qp;
	unsigned long qp_ptr_val;
	ssize_t ret = -EINVAL;

	if (sscanf(buf, "%lx", &qp_ptr_val) != 1) {
		_NE(chng_qp_err_e1, "invalid qp ptr val @STR", buf);
		goto out;
	}
	qp = (void *)qp_ptr_val;
	_NT(chng_qp_err_t1, "setting qp @PTR to error state", qp);
	if ((ret = ib_set_qp_err(qp)) < 0) {
		_NE(chng_qp_err_e2, "Failed to set qp @PTR to error (@ZU)", qp, ret);
		goto out;
	}
	_NT(chng_qp_err_t2, "qp @PTR set to error state", qp);
	ret = len;

out:
	return ret;
}

static const char *rdma_proc_dir_name = "rdma";
static struct proc_dir_entry *rdma_proc_dir = NULL;
static struct nvmeib_public_procfs_ent *qp_err_proc = NULL;

int nvmeib_rdma_proc_create(struct proc_dir_entry *proc_dir)
{
	int rv = -1;

	if (!proc_dir)
		goto out;

	if (!(rdma_proc_dir = proc_mkdir(rdma_proc_dir_name, proc_dir))) {
		_NE(nvmeib_rdma_proc_create_e1, "Fail to create proc dir");
		goto out;
	}
	if (!(qp_err_proc = nvmeib_public_proc_create("qp_error",
		rdma_proc_dir, NULL, chng_qp_err, NULL))) {
		_NE(nvmeib_rdma_proc_create_e2, "Fail to create proc qp_error");
		goto out;
	}
	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_rdma_proc_create);

void nvmeib_rdma_proc_remove(struct proc_dir_entry *proc_dir)
{
	if (qp_err_proc) {
		nvmeib_public_proc_remove(qp_err_proc);
		qp_err_proc = NULL;
	}
	remove_proc_entry(rdma_proc_dir_name, proc_dir);
	rdma_proc_dir   = NULL;

	return;
}
EXPORT_SYMBOL(nvmeib_rdma_proc_remove);

struct nvmeib_rdma_evt_ctx {
	void *ctx; /* c_net , s_net */
	struct nvmeib_ref nref;
};

struct nvmeib_rdma_evt_ctx *nvmeib_rdma_evt_ctx_create(void *ctx)
{
	struct nvmeib_rdma_evt_ctx *c;

	if (!(c = kzalloc(sizeof(*c), GFP_KERNEL))) {
		_NE(nvmeib_rdma_evt_ctx_create_e1, "Fail to alloc");
	}
	else {
		nvmeib_ref_init(&c->nref);
		c->ctx = ctx;
		_NT(nvmeib_rdma_evt_ctx_create_t1, "Created c=@PTR, ctx=@PTR", c, c->ctx);
	}

	return c;
}
EXPORT_SYMBOL(nvmeib_rdma_evt_ctx_create);

void nvmeib_rdma_evt_ctx_stop(struct nvmeib_rdma_evt_ctx *c)
{
	_NT(nvmeib_rdma_evt_ctx_stop_t1, "Stop forwarding events to c=@PTR, ctx=@PTR", c, c->ctx);
	nvmeib_ref_release_start(&c->nref);
	nvmeib_ref_release_wait(&c->nref);
}
EXPORT_SYMBOL(nvmeib_rdma_evt_ctx_stop);

void nvmeib_rdma_evt_ctx_destroy(struct nvmeib_rdma_evt_ctx *c)
{
	_NT(nvmeib_rdma_evt_ctx_destroy_t1, "Destroy c=@PTR, ctx=@PTR", c, c->ctx);
	BUG_ON(nvmeib_ref_read(&c->nref));
	kfree(c);
}
EXPORT_SYMBOL(nvmeib_rdma_evt_ctx_destroy);

void *nvmeib_rdma_evt_ctx_get(struct nvmeib_rdma_evt_ctx *c)
{
	void *ctx = NULL;

	if (nvmeib_ref_get(&c->nref)) {
		_NT(nvmeib_rdma_evt_ctx_get_t1, "Ref inc c=@PTR, ctx=@PTR", c, c->ctx);
		ctx = c->ctx;
	}
	else {
		_NT(nvmeib_rdma_evt_ctx_get_e1, "Could not ref inc (c=@PTR, ctx=@PTR, d=@INT, n=@INT)",
		   c, c->ctx, nvmeib_ref_is_dying(&c->nref), nvmeib_ref_read(&c->nref));
	}

	return ctx;
}
EXPORT_SYMBOL(nvmeib_rdma_evt_ctx_get);

void nvmeib_rdma_evt_ctx_put(struct nvmeib_rdma_evt_ctx *c)
{
	_NT(nvmeib_rdma_evt_ctx_put_t1, "Ref dec c=@PTR, ctx=@PTR", c, c->ctx);
	nvmeib_ref_put(&c->nref);
}
EXPORT_SYMBOL(nvmeib_rdma_evt_ctx_put);

#if ENABLE_SIW
#include "../softiwarp/common/siw_kern_abi.h"
#endif

void *nvmeib_rdma_alloc_siw_wc_md(unsigned n_wcs, struct ib_wc *wcs, gfp_t gfp)
{
#if ENABLE_SIW
	struct siw_wc_md *siw_wc_md;
	unsigned i;
	if (!(siw_wc_md = kcalloc(n_wcs, sizeof(*siw_wc_md), gfp))) {
		goto out;
	}
	for (i = 0; i < n_wcs; i++) {
		wcs[i].wr_id = (u64)&siw_wc_md[i];
		wcs[i].wc_flags = SIW_IB_WC_WITH_SIW_MD;
	}
out:
	return siw_wc_md;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL(nvmeib_rdma_alloc_siw_wc_md);

void nvmeib_rdma_free_siw_wc_md(void *siw_wc_md, unsigned n_wcs, struct ib_wc *wcs)
{
#if ENABLE_SIW
	unsigned i;
	if (wcs && n_wcs) {
		for (i = 0; i < n_wcs; i++) {
			wcs[i].wr_id = 0;
			wcs[i].wc_flags = 0;
		}
	}
	kfree(siw_wc_md);
#else
	BUG_ON(siw_wc_md);
#endif
}
EXPORT_SYMBOL(nvmeib_rdma_free_siw_wc_md);

bool nvmeib_rdma_siw_wc_get_tx_md(const struct ib_wc *wc,
				  ktime_t	*post_send_time,
				  ktime_t 	*sent_time,
				  ktime_t 	*ack_time,
				  u16		*tx_cpu)
{
#if ENABLE_SIW
	if (wc->wc_flags & SIW_IB_WC_WITH_SIW_MD) {
		const struct siw_wc_md *siw_wc_md = (const struct siw_wc_md *)wc->wr_id;
		if (siw_wc_md->flags & SIW_WC_MD_TX_TIME) {
			*post_send_time = siw_wc_md->tx_timestamp.post_send_time;
			*sent_time = siw_wc_md->tx_timestamp.sent_time;
			*ack_time = siw_wc_md->tx_timestamp.ack_time;
			*tx_cpu = siw_wc_md->tx_timestamp.tx_cpu;
			return true;
		}
	}
#endif
	return false;
}
EXPORT_SYMBOL(nvmeib_rdma_siw_wc_get_tx_md);

bool nvmeib_rdma_siw_wc_get_rx_md(const struct ib_wc *wc,
				  ktime_t *first_ddp_recv_time,
				  ktime_t *last_ddp_recv_time,
				  ktime_t	*reap_time,
				  u16	*rx_cpu,
				  u16	*rx_queue,
				  u32	*rx_skb_hash)
{
#if ENABLE_SIW
	if (wc->wc_flags & SIW_IB_WC_WITH_SIW_MD) {
		const struct siw_wc_md *siw_wc_md = (const struct siw_wc_md *)wc->wr_id;
		if (siw_wc_md->flags & SIW_WC_MD_RX_TIME) {
			*first_ddp_recv_time = siw_wc_md->rx_timestamp.first_ddp_recv_time;
			*last_ddp_recv_time = siw_wc_md->rx_timestamp.last_ddp_recv_time;
			*reap_time = siw_wc_md->rx_timestamp.reap_time;
			*rx_cpu = siw_wc_md->rx_timestamp.rx_cpu;
			*rx_queue = siw_wc_md->rx_timestamp.rx_queue;
			*rx_skb_hash = siw_wc_md->rx_timestamp.rx_skb_hash;
			return true;
		}
	}
#endif
	return false;
}
EXPORT_SYMBOL(nvmeib_rdma_siw_wc_get_rx_md);
