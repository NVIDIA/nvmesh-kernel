/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#pragma once

/* First injection point which should be included by all Toma simulators
   This file replaces Toma's interfaces with other components, allowing it to
   run in a sandbox. Used for unitesting */

#include "11_os/os_public.h"
#define TOMA_USE_USER_SPACE_SERVER_API (1)
#include "kafka/sandbox_kafka_public.h"

/************************************* network ********************************/
#define NVMEIBT_NETWORK_INCS_H			//#include "interfaces/network/network_incs.h"

// API SRM<-->Toma
int  rsrm_init_work_tmq(void);			// Create SRM
void rsrm_resend_acks(void);			// Periodic high priority job: Send acks for received packets
int  rsrm_get_fd_timer(void);			// Get Timer file descriptor for periodic timer based resent packets
int  rsrm_resend_timer(void);			// Callback for timer

// API SRM faults<-->Toma
int  rsrm_faults_init_fifo_comm(void);	// Create Fifo queue (cli file)
int  rsrm_faults_get_fd(void);			// Get descriptor of fifo to select
void rsrm_faults_handle_fifo_comm(void);// Handle fault after wakeup from select
void rsrm_destroy_after_run(void);

// Network manager (For raft communication) nm
struct nvmeibt_nm_local_node; struct nvmeibt_nic; struct nvmeibt_node; struct nvmeibt_msg_request;
void *nvmeibt_nm_tracer_init(const char *lib_path);
struct nvmeibt_nm_local_node *nvmeibt_nm_init(void *handle);
int  nvmeibt_nm_get_fd(         struct nvmeibt_nm_local_node *);			// For Toma epoll
void nvmeibt_nm_done(           struct nvmeibt_nm_local_node *);
int  nvmeibt_nm_add_remote_nic( struct nvmeibt_nm_local_node *, struct nvmeibt_nic *);
int  nvmeibt_nm_del_remote_nic( struct nvmeibt_nm_local_node *, struct nvmeibt_nic *);
int  nvmeibt_nm_del_remote_node(struct nvmeibt_nm_local_node *, struct nvmeibt_node *);
int  nvmeibt_nm_cancel_req_node(struct nvmeibt_nm_local_node *, struct nvmeibt_node *);
int nvmeibt_nm_process_toma_requests(    struct nvmeibt_nm_local_node *);	// Toma call this to process incoming network events
int  nvmeibt_nm_queue_srm_req(           struct nvmeibt_nm_local_node *, struct nvmeibt_node *, struct nvmeibt_msg_request *req);	// Toma sends messages with this func
bool nvmeibt_nm_is_remote_node_connected(struct nvmeibt_nm_local_node *, struct nvmeibt_node *);
void nvmeibt_nm_rsrm_resend_acks(           struct nvmeibt_nm_local_node *);
void nvmeibt_nm_rsrm_faults_handle_fifo_com(struct nvmeibt_nm_local_node *);
int nvmeibt_nm_rsrm_send_timer(             struct nvmeibt_nm_local_node *);
int nvmeibt_nm_print_status(     void *ctx, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
int nvmeibt_nm_print_status_json(void *ctx, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);


// Todo: Remove. Split nvmeibt_node.c to Toma part (config) and network part:
struct nvmeibt_sr_cb_table {
	void (*send_c)(void *arg, int status);	/* data send-completion callback */
};
struct nvmeibt_msg_request {
	unsigned int msg_type;
	unsigned int reason;
	int msg_len;
	int data_len;
	const void *cnst_msg;
	const char *cnst_data;
	struct nvmeibt_sr_cb_table cbs;
	void *arg, *user;
};

#define SRM_EMPTY_USER ((void *)(~(0ULL)))
#include "../common/nvmeib_hash.h"
struct nvmeibt_srm { int dummy; };
struct udp_peer { int dummy; };
union ibv_gid { uint8_t raw[16]; struct { uint64_t subnet_prefix; uint64_t interface_id;} global; };
struct nvmeibt_node;
static inline int is_peer_reachable(const struct udp_peer *p) { (void)p; return 0;}
int nvmeibt_srm_queue_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req);
struct nvmeibt_srm *udp_peer_srm(struct udp_peer *peer);
int allocate_udp_server(int ip_protocol, union ibv_gid *gid, struct nvmeibt_node *node);
int start_udp_server(void);
int nvmeib_register_udp_peer(struct nvmeibt_node *node, const char *peer_name, const char *peer_guid);
