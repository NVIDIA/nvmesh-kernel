/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef COMMON_H
#define COMMON_H

#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/socket.h>
#include <net/sock.h>

#include "utils.h"

#define SRV_ADDR 0x0a000128U //10.0.1.40
//#define SRV_ADDR 0x0a000129U //10.0.1.41
//#define SRV_ADDR 0x0a000142U //10.0.1.66

#define SRV_PORT 20000
#define SC_TCP_USER_TIMEOUT 0x7fffffff

//TBD: change this to __builtin_types_compatible_p or a macro for func decleration
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
#define KER_VER_GE_3_15 1
typedef void (*sk_data_ready_t)(struct sock *sk);
#else
#define KER_VER_GE_3_15 0
typedef void (*sk_data_ready_t)(struct sock *sk, int bytes);
#endif

//TBD: ugly!
extern sk_data_ready_t listen_data_ready_addr;

#define SC_MSG_MAX_DATA_LEN 256
#define SC_MSG_MAGIC 0xA1A2A3A4U
#define SC_MSG_FIRST_NUM 0x1122
struct sc_msg
{
	__be32 magic;
	__be32 msg_num;
	__be16 msg_type;
	__be16 data_len;
	__u8 data[0];
} __attribute__((packed));

enum sc_msg_type {
	SC_MSG_BASE = 0xABAB,
	SC_MSG_DATA,
	SC_MSG_LAST
};


//TBD - protect static work-item from being reused while queued
#if 0
enum sc_work_state {
	SQ_WORK_IDLE,
	SQ_WORK_INUSE,
	SQ_WORK_PENDING,
};

struct sc_work {
	struct work_struct work;
	enum sc_work_state state;
};
#endif

enum sc_type {
	SC_CLNT = 0x1234,
	SC_SRV  = 0x5678,
};
struct sock_container {
	//struct kref sc_kref;
	struct socket *sc_sock;
	enum sc_type type;

	/* send */
	int send_msg_num;

	/* recv */
	struct page *sc_page;
	size_t sc_page_off;
	int recv_msg_num;

	/* original handlers for the sockets */
	void(*sc_state_change)(struct sock *sk);
	//void(*sc_data_ready)(struct sock *sk);
	sk_data_ready_t	sc_data_ready;

	struct workqueue_struct *wq;
	struct work_struct sc_rx_work;
	struct work_struct sc_shutdown_work;
	struct work_struct sc_established_work;
};

void sc_put(struct sock_container *sc);
void sc_get(struct sock_container *sc);

void sc_queue_work(struct sock_container *sc,
				struct work_struct *work);
void sc_cancel_delayed_works(struct sock_container *sc);

void sc_users_unregister(struct sock_container *sc);
int __attribute__((unused)) sc_users_register(struct sock_container *sc);

void sc_list_add(struct sock_container *sc);
void sc_list_del(struct sock_container *sc);

void sc_rx_work_f(struct work_struct *work);
void sc_shutdown_work_f(struct work_struct *work);

#if KER_VER_GE_3_15
void sc_data_ready(struct sock *sk);
#else
void sc_data_ready(struct sock *sk, int bytes);
#endif
void sc_state_change(struct sock *sk);

void print_sk_callbacks(struct sock *sk);
void sk_callbacks_register(struct sock *sk, struct sock_container *sc);
int sk_callbacks_unregister(struct sock *sk, struct sock_container *sc);

struct sock_container *sc_alloc(enum sc_type type, struct workqueue_struct *wq);
void sc_free(struct sock_container *sc);

int sock_set_nodelay(struct socket *sock);
int sock_set_usertimeout(struct socket *sock);

static inline const char *tcp_state_to_str(int state)
{
	switch (state) {
	case TCP_ESTABLISHED : return "TCP_ESTABLISHED ";
	case TCP_SYN_SENT    : return "TCP_SYN_SENT    ";
	case TCP_SYN_RECV    : return "TCP_SYN_RECV    ";
	case TCP_FIN_WAIT1   : return "TCP_FIN_WAIT1   ";
	case TCP_FIN_WAIT2   : return "TCP_FIN_WAIT2   ";
	case TCP_TIME_WAIT   : return "TCP_TIME_WAIT   ";
	case TCP_CLOSE       : return "TCP_CLOSE       ";
	case TCP_CLOSE_WAIT  : return "TCP_CLOSE_WAIT  ";
	case TCP_LAST_ACK    : return "TCP_LAST_ACK    ";
	case TCP_LISTEN      : return "TCP_LISTEN      ";
	case TCP_CLOSING     : return "TCP_CLOSING     ";
//TBD: #if ker-ver > x.y.z
//	case TCP_NEW_SYN_RECV: return "TCP_NEW_SYN_RECV";
	default: return "???";
	}
}

int sc_send_msg(struct sock_container *sc, void *data, size_t len);

#endif /* COMMON_H */

