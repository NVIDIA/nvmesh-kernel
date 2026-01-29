/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_NM_H
#define NVMEIBT_NM_H
#include <stdio.h>
#include <endian.h>
#include <errno.h>
#include <linux/types.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <math.h>
#include <limits.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netdb.h>
#include <sys/time.h>
#include <asm/byteorder.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "nvmeibt_ib_common.h"

#define NVMEIBT_IB
#include "nvmeibt_debug.h"
#include "nvmeibt_net_version.h"
#include "cm.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_uuid.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_bm.h"
#include "nvmeibt_common.h"
#include "nvmeibt_wq.h"
#include "nvmeibt_srm.h"
#include "nvmeibt_node.h"
#include "nvmeibt_nic.h"
#include "nvmeibt_net.h"
#include "nvmeibt_topology.h"		// Daniel: Whoever did that - this is an evil hack! todo cleanup the code
#include "nvmeibt_event_tracker.h"
#undef NVMEIBT_IB

#define PFIN \
	N_Tf(__AUTOID__, "--> @STR", (path && path->name) ? path->name : "?")
#define PFOUT \
	N_Tf(__AUTOID__, "<-- @STR", (path && path->name) ? path->name : "?")

#define NEW_TOMA_NW_MIN_VERSION {.major = 2, .minor = 6}
static const union version s_min_ver = NEW_TOMA_NW_MIN_VERSION;
static const union version s_self_ver = NVMEIBT_TN_PROTOCOL_VERSION;

/* fd wrraper to be used by NM epoll */
struct nvmeibt_nm_per_fd {
	int fd;
	int wait_read;
	int wait_write;
	int (*f)(void *ctx, int is_read, int is_write, int dry_tries);
	void *ctx;

	bool need_close;
	bool need_read;
	bool need_write;
};

/* using linkable method similar for kernel list implention */
#define LINKABLE_NAME_SIZE 64
struct nvmeibt_nm_linkable {
	char name[LINKABLE_NAME_SIZE];
	unsigned type;
	void (*free)(struct nvmeibt_nm_linkable *);
	struct xdlist link;
};

/* a struct that will be used as key for hash table */
struct nvmeibt_nm_hash_key_type {
	struct nvmeibt_nm_linkable base;
	/* unique id per entries in hash */
	uint64_t guid;
};

struct nvmeibt_nm_key_type {
	struct nvmeibt_nm_hash_key_type base;
};

struct hash_listener_key_type {
	struct nvmeibt_nm_hash_key_type base;
};

struct nvmeibt_nm_hash_wrid_key_type {
	struct nvmeibt_nm_hash_key_type base;
	uint64_t path_guid;
	uint64_t sender_index;
	unsigned type;
	int response;
	int signaled;
};

typedef XDLIST_DECLARE(, struct nvmeibt_nm_linkable, link) nvmeibt_nm_l_list_t;

#define SOCKADDR_STRING_LEN 64
#define SOCKADDR_STRING_LEN 64
#define NODE_STR_LEN 256
#define GID_STR_LEN 35
struct nvmeibt_nm_per_nic;

/* Represent local port object, Trasnport may inherit */
struct nvmeibt_nm_per_port {
	struct nvmeibt_nm_per_nic *pn;
	char name[SOCKADDR_STRING_LEN];
	int port_num;
	int is_ipv4;
	int is_dead;
	int is_valid;
	int mtu;
	//int is_roce;
	uint16_t broadcast_id; /* pkey or vlan */
	union ibv_gid gid;		/* current gid of the port */
	union ibv_gid conf_gid; /* gid coming from configuration */
	struct hash_listener_key_type cmt;

	//int is_ib;
	enum nvmeib_rdma_transport transport;
	union nvmeib_uuid port_id; /* Actually refer to nic id */

	bool restart_needed;
};

struct nvmeibt_nm_local_node;

/* Represnt local nic object, Probably mostly needed only for IB, Transport may inherit */
struct nvmeibt_nm_per_nic {
	char dev_name[64];
	/* ports which are related to this nic */
	struct nvmeibt_nm_per_port **ports;
	int n_ports;
	/* is this nic dead - fail to ibv init */
	int is_dead;
	/* is this nic allowed */
	int allowed;
	/* ref back to local node */
	struct nvmeibt_nm_local_node *local_node;
};

/* This currently Represnt a singleton for Toma Network Manger, Transport may inherit
    In the future we might consider few instances per trasnport type
*/
struct nvmeibt_nm_login_data;
struct nvmeibt_nm_hw_function_table {
	struct nvmeibt_nm_local_node * (*allocate_local_node)(void);
	int (*init_local_node) (struct nvmeibt_nm_local_node *);
	void (*free_local_node) (struct nvmeibt_nm_local_node *);
	int (*hw_init) (struct nvmeibt_nm_local_node *);
	int (*init_nics) (struct nvmeibt_nm_local_node *);
	int (*attach_nic) (struct nvmeibt_nm_local_node *, int);
	struct nvmeibt_nm_path * (*allocate_path)(void);
	void (*restart_path) (struct nvmeibt_nm_path *);
	void (*set_conneting_path) (struct nvmeibt_nm_path *);
	void (*free_path) (struct nvmeibt_nm_path *);
	int (*resolve_path) (struct nvmeibt_nm_path *);
	struct nvmeibt_nm_remote_addr * (*allocate_remote_addr)(void);
	int (*get_max_chunk_size) (struct nvmeibt_nm_path *);
	void * (*alloc_msgs_buffer) (struct nvmeibt_nm_path *);
	void (*path_release_send_buffer) (struct nvmeibt_nm_path *);
	int (*path_send_msg) (struct nvmeibt_nm_path *, struct nvmeibt_wire_msg *, uint16_t msg_id, bool);
	int (*srm_get_data_offset_size) (void);
	int (*try_connect_path) (struct nvmeibt_nm_path *, bool);
	int (*send_ping) (struct nvmeibt_nm_path *, int, uint8_t, int);
	int (*reject_connection)(void *, uint32_t , struct nvmeibt_nm_login_data *);
	int (*accept_connection) (struct nvmeibt_nm_path *, struct nvmeibt_nm_per_port *,
									struct nvmeibt_nm_login_data *, void *);
	struct nvmeibt_nm_login_data * (*allocate_login_data) (void);
	const char * (*get_pp_state)(struct nvmeibt_nm_per_port *);
	int (*create_port) (struct nvmeibt_nm_per_port *);
	void (*free_port) (struct nvmeibt_nm_per_port *);
	void (*free_nic) (struct nvmeibt_nm_per_nic *);
	bool (*should_revive_port) (struct nvmeibt_nm_per_port *);
	void (*wait_events)(struct nvmeibt_nm_local_node *);
	unsigned short (*get_port) (void);
};

struct nvmeibt_nm_local_node {
	/* The NM thread id */
	pthread_t thr;
	pthread_mutex_t guard;
	int epoll_fd;
	/* A list of used fds */
	struct nvmeibt_nm_per_fd *fds;
	struct epoll_event *events;
	int n_fds;
	int event_fd;
	int timer_fd;
	struct itimerspec its;
	nvmeibt_nm_l_list_t el;
	/* pool of requests */
	nvmeibt_nm_l_list_t el_pool;
	int run;
	XHASHTABLE_DECLARE(, struct nvmeibt_nm_linkable, link, 12) key_val;
	uint64_t guid;
	nvmeibt_nm_l_list_t remotes;
	int wire_event_fd;
	nvmeibt_nm_l_list_t toma_requests;
	nvmeibt_nm_l_list_t toma_request_pool;
	char *status_str1;
	int status_str1_size;
	char *status_str2;
	int status_str2_size;
	char *status_str;
	char *status_json_str1;
	int status_json_str1_size;
	char *status_json_str2;
	int status_json_str2_size;
	char *status_json_str;
	int renew_status;
	union nvmeib_uuid local_node_id;
	int n_rsrm_resend_acks;
	int n_rsrm_send_timer;

	int n_nics;
	struct nvmeibt_nm_per_nic **nics;

    struct local_nics_data lnd;
	struct nvmeibt_nm_hw_function_table hw_func_tbl;
};

/* Represent remote node object created from Toma topology */
struct nvmeibt_nm_remote_node {
	struct nvmeibt_nm_linkable base;
	struct nvmeibt_nm_local_node *ln;
	struct nvmeibt_node *node;
	union nvmeib_uuid id;
	char name[NODE_STR_LEN];
	nvmeibt_nm_l_list_t addresses;
	/* a list of the recent paths that we `know` to be alive */
	nvmeibt_nm_l_list_t best_ready;
	int me;
	int connected;
	struct nvmeibt_event_tracker event_tracker;
};

/* Represent remote addr object created from Toma topology */
struct nvmeibt_nm_remote_addr {
	struct nvmeibt_nm_linkable base;
	struct nvmeibt_nm_local_node *ln;
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nic *nic;
	union nvmeib_uuid id;
	char guid[GID_STR_LEN];
	struct sockaddr_storage a;
	union ibv_gid gid;
	enum nvmeib_rdma_transport transport;
	uint16_t broadcast_id; /* pkey or vlan */
	nvmeibt_nm_l_list_t ready;
	nvmeibt_nm_l_list_t connecting;
};

enum nvmeibt_nm_path_state {
	nvmeibt_nm_ps_wait_start = 0,
	nvmeibt_nm_ps_wait_addr,
	nvmeibt_nm_ps_wait_route,
	nvmeibt_nm_ps_wait_connect,
	nvmeibt_nm_ps_connected,
	nvmeibt_nm_ps_wait_ping_ack,
};

enum nvmeibt_nm_path_last_error {
	nvmeibt_nm_ple_success = 0,
	nvmeibt_nm_ple_failed_address = 1,
	nvmeibt_nm_ple_failed_address_resolved,
	nvmeibt_nm_ple_failed_route,
	nvmeibt_nm_ple_failed_route_resolved,
	nvmeibt_nm_ple_failed_conenct,
	nvmeibt_nm_ple_failed_conenct_error,
	nvmeibt_nm_ple_failed_conenct_timeout,
	nvmeibt_nm_ple_failed_conenct_reject,
	nvmeibt_nm_ple_failed_peer,
	nvmeibt_nm_ple_failed_ah,
	nvmeibt_nm_ple_failed_ping,
	nvmeibt_nm_ple_failed_ping_retry,
	nvmeibt_nm_ple_cancel_node_req,
	nvmeibt_nm_ple_failed_ping_timer,
	nvmeibt_nm_ple_cm_error,
	nvmeibt_nm_ple_failed_send_comp,
	nvmeibt_nm_ple_failed_recv_comp,
	nvmeibt_nm_ple_unknown_wr_opcode,
	nvmeibt_nm_ple_different_srm_id,
	nvmeibt_nm_ple_other,
	nvmeibt_nm_ple_max  /* Keep this last for array sizing */
};

/* Typedef for path counters to provide type safety */
typedef uint64_t nvmeibt_nm_path_counter_t;

struct nvmeibt_nm_path_counters {
	nvmeibt_nm_path_counter_t path_restarts;
	nvmeibt_nm_path_counter_t error_counts[nvmeibt_nm_ple_max];
	nvmeibt_nm_path_counter_t ping_retries;
	nvmeibt_nm_path_counter_t invalid_ping_response;
};

static inline const char *nvmeibt_nm_ple_str(int v)
{
	switch (v) {
	case nvmeibt_nm_ple_success: return "Success";
	case nvmeibt_nm_ple_failed_address: return "Call address resolved failed";
	case nvmeibt_nm_ple_failed_address_resolved: return "Address resolved failed";
	case nvmeibt_nm_ple_failed_route: return "Call route resolved failed";
	case nvmeibt_nm_ple_failed_route_resolved: return "Route resolved failed";
	case nvmeibt_nm_ple_failed_conenct: return "Call connect failed";
	case nvmeibt_nm_ple_failed_conenct_error: return "Connect returned with error";
	case nvmeibt_nm_ple_failed_conenct_timeout: return "Connection timed out";
	case nvmeibt_nm_ple_failed_conenct_reject: return "Connection request rejected";
	case nvmeibt_nm_ple_failed_peer: return "Peer no ready";
	case nvmeibt_nm_ple_failed_ah: return "Failed create AH for peer";
	case nvmeibt_nm_ple_failed_ping: return "Send ping failed";
	case nvmeibt_nm_ple_failed_ping_retry: return "Ping max retries reached";
	case nvmeibt_nm_ple_cancel_node_req: return "Requests to peer were canceled";
	case nvmeibt_nm_ple_failed_ping_timer: return "Failed to create ping timer";
	case nvmeibt_nm_ple_cm_error: return "CM error";
	case nvmeibt_nm_ple_failed_send_comp: return "Send completion with error";
	case nvmeibt_nm_ple_failed_recv_comp: return "Receive completion with error";
	case nvmeibt_nm_ple_unknown_wr_opcode: return "Unknown WR opcode";
	case nvmeibt_nm_ple_other:
	default : return "Internal error";
	}
}

struct nvmeibt_nm_hash_cm_connect_key_type {
	struct nvmeibt_nm_hash_key_type base;
};

/* WARNING: This structure should be wired - don't use pointers */
 struct nvmeibt_nm_login_data {
	union {
		union version version;
		int padding;
	};
	union nvmeib_uuid node_id;
	uint16_t last_rej_reason;
	uint8_t reserved;
	uint32_t srm_id;
	union ibv_gid sgid;
 };

/*
Represent a Path in the system, i.e a connection between local gid/ip to remote gid/ip.
*/
struct nvmeibt_nm_path {
	struct nvmeibt_nm_linkable base;
	char *name;
	/* The remote address connected to */
	struct nvmeibt_nm_remote_addr *ra;
	/* The local port connected from
	   FUTURE: per_port also define the gid_index so multi gids can be used by
	   creating pp per gid/ip */
	struct nvmeibt_nm_per_port *pp;
	/* Timer fd for maintaing the path */
	int timer_fd;
	struct itimerspec its;

	enum nvmeibt_nm_path_state state;
	struct nvmeibt_nm_hash_cm_connect_key_type cmt;
	int hashed;

	nvmeibt_nm_l_list_t wrids;
	struct timespec last_send_comp_time_UNUSED;
	struct timespec last_recv_comp_time_UNUSED;
	struct timespec ping_send_timespec;
	struct nvmeibt_srm *srm;

	uint32_t srm_id;
	uint32_t remote_srm_id;
	int renew_srm_id;

	int ping_retry_counter;
	int is_rtr;
	int is_rts;
	uint8_t ping_id;
	struct nvmeibt_nm_linkable poll_link;
	int poll_linked;
	int is_sender;
	int not_ready_traced;
	int loopback;
	uint64_t version;
	int last_error;
	int64_t last_received_ping_ns_UNUSED;
	struct nvmeibt_nm_linkable best_ready_link;

	void *send_buf;
	long send_buf_len;

	/* TODO:NM can somehow reuse carrier? */
	int max_chunk_size;
	int max_messages;

	struct nvmeibt_nm_login_data *payload;

	bool err_addr_resolved;
	bool err_route_resolved;

	int login_fd;
	struct itimerspec login_its;

	/* Path counters for monitoring and statistics */
	struct nvmeibt_nm_path_counters counters;

};

enum nvmeibt_nm_periodic_msg_type {
	nvmeibt_nm_pst_none = 0,
	nvmeibt_nm_pst_rsrm_resend_acks = 1,
	nvmeibt_nm_pst_rsrm_send_timer,
};

enum nvmeibt_nm_request {
	nvmeibt_nm_request_stop = 1,
	nvmeibt_nm_request_add_nic,
	nvmeibt_nm_request_del_nic,
	nvmeibt_nm_request_send_msg,
	nvmeibt_nm_request_del_remote_node,
	nvmeibt_nm_request_port_gid_changed,
	nvmeibt_nm_request_local_nic_changed,
	nvmeibt_nm_request_cancel_req,
	nvmeibt_nm_request_rsrm_resend_acks,
	nvmeibt_nm_request_rsrm_send_timer,
	nvmeibt_nm_request_rsrm_faults,
};

static inline const char * __attribute__ ((unused)) nvmeibt_nm_r2str(int type)
{
	switch (type) {
	case nvmeibt_nm_request_stop : return "stop";
	case nvmeibt_nm_request_add_nic : return "add_nic";
	case nvmeibt_nm_request_del_nic : return "del_nic";
	case nvmeibt_nm_request_send_msg : return "send_msg";
	case nvmeibt_nm_request_del_remote_node : return "del_remote_node";
	case nvmeibt_nm_request_port_gid_changed : return "port_gid_changed";
	case nvmeibt_nm_request_local_nic_changed : return "local_nic_changed";
	case nvmeibt_nm_request_cancel_req : return "cancel_req";
	case nvmeibt_nm_request_rsrm_resend_acks : return "rsrm_resend_acks";
	case nvmeibt_nm_request_rsrm_send_timer : return "rsrm_send_timer";
	case nvmeibt_nm_request_rsrm_faults : return "rsrm_faults";
	default: return "???";
	}
}

#define NIC_NAME_SIZE 35
struct nvmeibt_nm_add_nic_req {
	struct nvmeibt_node *node;
	union nvmeib_uuid node_id;
	char remote_node[NVMEIB_HOST_NAME_LEN];
	struct nvmeibt_nic *nic;
	union nvmeib_uuid nic_id;
	char remote_nic[NIC_NAME_SIZE];
	int me;
	union nvmeib_uuid me_id;

	enum nvmeib_rdma_transport transport;
	uint16_t brodcast_id; /* pkey or vlan */
};

struct nvmeibt_nm_del_nic_req {
	union nvmeib_uuid node_id;
	char remote_node[NVMEIB_HOST_NAME_LEN];
	union nvmeib_uuid nic_id;
	char remote_nic[NIC_NAME_SIZE];
	//int is_roce;
	enum nvmeib_rdma_transport transport;
	uint16_t broadcast_id; /* pkey or vlan */
	bool me;
};

struct nvmeibt_nm_del_remote_node {
	union nvmeib_uuid node_id;
};

struct nvmeibt_nm_send_msg_req {
	union nvmeib_uuid node_id;
	struct nvmeibt_msg_request req;
	uint64_t key;
};

struct nvmeibt_nm_local_nic_change_req {
	char *dev_name;
	int add;
};

struct nvmeibt_nm_gid_changed_req {
	char *dev_name;
	char *gid_str;
	int port;
};

struct nvmeibt_nm_cencel_remote_node_req {
	union nvmeib_uuid node_id;
};

struct nvmeibt_nm_req {
	struct nvmeibt_nm_linkable base;
	union {
		struct nvmeibt_nm_add_nic_req add_nic_req;
		struct nvmeibt_nm_send_msg_req send_msg_req;
		struct nvmeibt_nm_del_nic_req del_nic_req;
		struct nvmeibt_nm_del_remote_node del_remote_node;
		struct nvmeibt_nm_local_nic_change_req local_nic_change_req;
		struct nvmeibt_nm_gid_changed_req gid_change_req;
		struct nvmeibt_nm_cencel_remote_node_req cacnel_remote_node_req;
	};
};

/* The union to represent a ping, should be use by hw when sending/receiving ping like packets */
union nvmeibt_nm_ping_imm_data {
	struct {
		u32 srm_id_lsb:16;
		u32 ping_id:8;
		u32 retry_count:4;
		u32 rsvd:3;
		u32 resp:1;
	} fields;
	u32 raw;
};

/* the request the rdma thread sends to the main TOMA thread */
enum nvmeibt_nm_toma_requests {
	nvmeibt_nm_tr_process_recv,
	nvmeibt_nm_tr_node_key,
};

struct nvmeibt_nm_toma_req {
	struct nvmeibt_nm_linkable base;
	union {
		struct nvmeibt_big_msg *msg;
		struct {
			union nvmeib_uuid node_id;
			uint64_t key;
			int force;
		};
	};
};

enum key_types {
	kt_cq = 1,
	kt_listener_cm = 2,
	kt_accept_cm = 3,
	kt_connect_cm = 4,
	kt_wrid = 5,
};

#define pathtoln(_path) ((_path)->pp->pn->local_node)

/*
==============================================================
APIs to be called from nvmeibt_nm and nvmeibt_nm_hw
implementations
==============================================================
*/

/* Add fd into the main event loop of nm */
int nvmeibt_nm_add_fd(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_per_fd *pfd);
/* Remove fd from the main event loop of nm */
int nvmeibt_nm_del_fd(struct nvmeibt_nm_local_node *ln, int fd);
/* Modify fd in the main event loop of nm */
int nvmeibt_nm_mod_fd(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_per_fd *pfd);
/* Some locking to sync between NM thread to callers */
void nvmeibt_nm_mutex_lock(pthread_mutex_t *p);
void nvmeibt_nm_mutex_release(pthread_mutex_t *p);

void nvmeibt_nm_free_linkable(struct nvmeibt_nm_linkable *l);
void nvmeibt_nm_set_path_state(struct nvmeibt_nm_path *path, enum nvmeibt_nm_path_state new_state);
/* Restart a path */
struct nvmeibt_nm_path * nvmeibt_nm_restart_path(struct nvmeibt_nm_path *path);

/* Should be call by hw when it waits for resolving a path(as active side) */
void nvmeibt_nm_path_resolve_wait(struct nvmeibt_nm_path *path);
/* Should be call when needs to try connect a path */
int nvmeibt_nm_try_connect_path(struct nvmeibt_nm_path *path);
/* Should be called by hw when it secesfully connected a path(as active side) */
int nvmeibt_nm_connect_path(struct nvmeibt_nm_path *path);

void nvmeibt_nm_add_key(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_linkable *l, uint64_t key,
	const char namefmt[], ...);
void nvmeibt_nm_del_key(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_linkable *l, int free_mem);
struct nvmeibt_nm_hash_key_type * find_key(struct nvmeibt_nm_local_node *ln, uint64_t key);

/* Some helpers to find a remote node by specific attr */
struct nvmeibt_nm_remote_node * nvmeibt_nm_find_remote_node(struct nvmeibt_nm_local_node *ln, const union nvmeib_uuid *id);
struct nvmeibt_nm_remote_addr * nvmeibt_nm_find_remote_address_by_gid(
	struct nvmeibt_nm_remote_node *rn, union ibv_gid *gid);
struct nvmeibt_nm_remote_addr * nvmeibt_nm_find_remote_address_by_uuid(
	struct nvmeibt_nm_remote_node *rn, const union nvmeib_uuid *id);
struct nvmeibt_nm_path * nvmeibt_nm_get_path(struct nvmeibt_nm_remote_addr *ra,
 struct nvmeibt_nm_per_port *pp, int force_restart, uint32_t remote_srm_id);
void nvmeibt_nm_path_modify_to_rtr(struct nvmeibt_nm_path *path);
struct nvmeibt_nm_remote_addr * nvmeibt_nm_find_node_remote_address_by_gid(
	struct nvmeibt_nm_per_nic *pn, union ibv_gid *gid);

/* API to be called as cb when hw received a ping like frame */
void nvmeibt_nm_on_recv_ping(struct nvmeibt_nm_path *path, union nvmeibt_nm_ping_imm_data *v);

/* API to be called on getting connection request */
int nvmeibt_nm_on_connection_request(struct nvmeibt_nm_login_data *login, struct nvmeibt_nm_per_port *pp, void *data);

void nvmeibt_nm_free_nic(struct nvmeibt_nm_per_nic *pn);

bool nvmeibt_nm_is_remote_node_connected(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *remote_node);

/* TODO:NM Make us macros */
struct nvmeibt_nm_remote_addr * nvmeibt_nm_l2ra(struct nvmeibt_nm_linkable *l);
struct nvmeibt_nm_remote_node * nvmeibt_nm_l2rn(struct nvmeibt_nm_linkable *l);
struct nvmeibt_nm_path * nvmeibt_nm_l2p(struct nvmeibt_nm_linkable *l);

/* Path counters functions */
void nvmeibt_nm_path_counters_init(struct nvmeibt_nm_path *path);
void nvmeibt_nm_path_counters_reset(struct nvmeibt_nm_path *path);
void nvmeibt_nm_path_counter_inc(struct nvmeibt_nm_path *path, nvmeibt_nm_path_counter_t *counter);
void nvmeibt_nm_path_counter_add(struct nvmeibt_nm_path *path, nvmeibt_nm_path_counter_t *counter, uint64_t value);
void nvmeibt_nm_path_set_last_error(struct nvmeibt_nm_path *path, enum nvmeibt_nm_path_last_error error);

/* Path counter convenience macros - type-safe counter operations */
#define NVMEIBT_PATH_COUNTER_INC(path, counter_name) \
	nvmeibt_nm_path_counter_inc((path), &(path)->counters.counter_name)

#define NVMEIBT_PATH_COUNTER_ADD(path, counter_name, value) \
	nvmeibt_nm_path_counter_add((path), &(path)->counters.counter_name, (value))

struct hash_listener_key_type * nvmeibt_nm_kt2cml(struct nvmeibt_nm_hash_key_type *kt);
struct nvmeibt_nm_per_port * nvmeibt_nm_cmt2pp(struct hash_listener_key_type *cmt);
/*
==============================================================
The follwing API's are for external module (Main Toma/Srm/Raft)
==============================================================
*/

/*
Init Internal structures, wq and spawn the networking thread.
Calls nvmeibt_nm_hw_init for specfic hw.
*/
struct nvmeibt_nm_local_node * nvmeibt_nm_init(const char *lib_path);

/* The function should be called when a new remote nic is detected by
   Toma Configuration, that will call to hw specific */
int nvmeibt_nm_add_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic);

/* The function should be called when existing remote nic is deleted from
   Toma Configuration, that will call to hw specific */
int nvmeibt_nm_del_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic);

/*  Removes remote node from the network manager */
int nvmeibt_nm_del_remote_node(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node);

/* The function should be called when Toma Main Thread wants to process
   The requests passed by the Network Manager
*/
int nvmeibt_nm_process_toma_requests(struct nvmeibt_nm_local_node *ln);

/* The function should be call when Toma want to make Network Manager receive msg*/
int nvmeibt_nm_add_wire_recv(struct nvmeibt_nm_local_node *ln, void *buf);

/* The function should be call from Toma when it needs to queue an srm request(i.e send msg to remote node) */
int nvmeibt_nm_queue_srm_req(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *remote_node,
	struct nvmeibt_msg_request *req);

/* The function should be call by Toma(As Recivier) when it needs to retransmit the acks */
void nvmeibt_nm_rsrm_resend_acks(struct nvmeibt_nm_local_node *ln);

/* The function should be call by Toma(As Transmitter) when it needs to restransmit frames */
int nvmeibt_nm_rsrm_send_timer(struct nvmeibt_nm_local_node *ln);

/* The function should be call by Toma when a nic goes up/down */
void nvmeibt_nm_handle_local_nic_change(struct nvmeibt_nm_local_node *ln, const char *dev_name, int add);

/* Expose needed nvmeibt_nm fds for Toma */
int nvmeibt_nm_get_fd(struct nvmeibt_nm_local_node *ln);

/* The function should be call when local port change it's gid */
void nvmeibt_nm_handle_local_port_gid_change(
	struct nvmeibt_nm_local_node *ln, const char *dev_name, int port, const char *gid_str);

void nvmeibt_nm_rsrm_faults_handle_fifo_com(struct nvmeibt_nm_local_node *ln);

/* Prints Local Node status */
int nvmeibt_nm_print_status(void *ctx,
	int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

/* Prints Local Node status in JSON format using JDR */
int nvmeibt_nm_print_status_json(void *ctx,
	int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

int nvmeibt_nm_cancel_req_node(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node);

void nvmeibt_nm_done(struct nvmeibt_nm_local_node *ln);


char * __attribute__ ((unused)) nvmeibt_nm_tss(void *_a, char b[], int len);


int create_timer(void);
int clear_timer_fd(int fd, int is_read, int is_write);
int set_periodic(int *fd, struct itimerspec *its, int64_t ns);
int cancel_timer(int *fd, struct itimerspec *its);
void free_timer(int *fd);

#endif