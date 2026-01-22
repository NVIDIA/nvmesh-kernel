/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_NET_H
#define NVMEIBS_NET_H

#include "kr_incs.h"
#include "nvmeib.h"
#include "nvmeib_types.h"
#include "nvmeib_shared.h"
#include "nvmeib_srq.h"
#include "nvmeib_public.h"

/**
 * enum net_state -  Connection state.
 * @CL_CONNECTING:		QP is in RTR state; waiting for RTU.
 * @CL_LIVE:			QP is in RTS state.
 * @CL_DISCONNECTING:	DREQ has been received; waiting for DREP
 *  					 or DREQ has been send and waiting for
 *  					 DREP or .
 * @CL_DRAINING:		QP is in ERR state; waiting for last WQE
 *  		             event.
 * @CL_RELEASING:		Last WQE event has been received;
 *  			         releasing resources.
 */
enum nvmeibs_net_state {
	QP_CONNECTING,
	QP_LIVE,
	QP_DISCONNECTING,
	QP_DRAINING,
	QP_RELEASING
};

enum {
	NVMEIBS_CONNECTION_LOCAL_NAME_SIZE = 16,
};

enum nvmeibs_net_type {
	S_NET_ADMIN,
	S_NET_IO,
	S_NET_LOCK,
	S_NET_NORDDA,
	S_NET_LOCK_2ND,
};

static inline enum channel_type nt_to_ct(enum nvmeibs_net_type nt)
{
	switch (nt) {
		case S_NET_ADMIN: return ct_admin;
		case S_NET_IO: return ct_rdda;
		case S_NET_LOCK: return ct_lock;
		case S_NET_NORDDA: return ct_n_rdda;
		case S_NET_LOCK_2ND: return ct_lock_2nd;
		default: return ct_other;
	}
}

struct nvmeibs_client;
struct nvmeibs_net_init {
	/* the name of the connection */
	char name[NVMEIBS_CONNECTION_LOCAL_NAME_SIZE + 1];
	/* the device we are going to use */
	struct nvmeibs_client *cl;
	/* the type of the net */
	enum nvmeibs_net_type net_type;
	/* nordda channel of the net */
	struct nvmeibs_nr_channel *nrch;
	/* the port number we are connected */
	struct nvmeibs_ib_port *port;
	/* the connection id */
	struct nvmeib_rdma_cm *cm_id;
	/* max send message size */
	int s_msg_size;
	/* max send S/G list size */
	int max_send_sge;
	/* max received message size */
	int r_msg_size;
	/* max receive S/G list size */
	int max_recv_sge;
	/* send queue size */
	int sendq_size;
	/* actual sendq size */
	int asendq_size;
	/* max completion send queue size */
	int scq_size;
	/* receive queue size */
	int recvq_size;
	/* receive completion queue size if sharing with send must be zero */
	int rcq_size;
	/* true if shared receive queue is used */
	bool use_srq;
	/* channel's private SRQ valid if @use_srq == true */
	struct nvmeib_srq_info *srq_priv;
	/* get public SRQ, valid if @use_srq == true && @srq_priv == NULL */
	enum nvmeib_srq_type srq_type;
	/*indicator to atomic operations*/
	bool use_atomic;
	/*pointer to receive queue when no SRQ */
	struct nvmeib_recvq *recv_q;
	/*use special create_rdda_qp to create the QP */
	bool rdda_qp;
	/*free on net release instead of client release (Secondary Lock Channels) */
	bool free_on_net_rls;

	/* API */
	/* connection management handler */
	void (*cm_handler)(struct nvmeib_cm_id *h, struct nvmeib_rdma_event *event);
	/* send completion handler */
	ib_comp_handler scq_handler;
	/* send completion handler context */
	void *scq_context;
	/* receive completion handler */
	ib_comp_handler rcq_handler;
	/* receive completion handler context */
	void *rcq_context;
	/* on_ready_to_send handler */
	void (*rts_handler)(void *context);
	/* on_ready_to_send handler context */
	void *rts_context;
	/* work to do before release work is added to client wq */
	void (*pre_rw_handler)(void *context);
	void *pre_rw_context;
	bool pre_rw_handler_called;
	/* extra work before finally done */
	void (*rw_handler)(void *context);
	/* release work handler context */
	void *rw_context;
	/* stuff to do before net is gone */
	void (*bn_handler)(void *context);
	/* bn_handler context */
	void *bn_context;
	/* stuff to do after drain cqs */
	void (*ad_handler)(void *context);
	/* ad_handler context */
	void *ad_context;
	/* ka stuff before main-net is gone */
	void (*ka_handler)(void *context);
	/* ka_handler context */
	void *ka_context;
	/* call after setting the qp into error state */
	void (*after_qp_error)(void *ctx);
	/* call after destroying RDMA resources */
	void (*after_destroy_ib)(void *ctx);
	/* context for above */
	void *after_destroy_ib_ctx;

	/* Use dev's percpu CQ pool */
	void (*send_comp_h)(void *ctx, struct ib_wc *wcs);
	void (*recv_comp_h)(void *ctx, struct ib_wc *wcs);
	
	unsigned ch_index;
};

struct nvmeibs_net {
	struct nvmeibs_net_init params;
	spinlock_t spinlock;
	int locking_cpu;
	struct nvmeib_cm_id cm_id;
	struct ib_qp *qp;
	struct ib_cq *scq;
	struct ib_cq *rcq;
	enum nvmeibs_net_state state;
	u32 remote_qpn;
	u32 sq_psn;
	struct nvmeib_srq_info *srq_info;
	/*indicates that the login request has been accepted*/
	bool connection_accepted;
	struct nvmeib_iu **ioctx_ring;
	struct list_head free_send_msgs;
	struct list_head pending_received_msgs;
	/*uncompleted messages to be handled after net is closed */
	struct list_head uncomp_msgs;
	struct workqe_struct release_work;
	struct workqe_struct rts_handler_work;
	atomic_t dying;
	int qp_evt_last_wqe;
	int qp_evt_qp_fatal;
	int qp_rq_drain_recv;
	struct completion *release_done;
	struct completion *drep_comp;
	void *priv;
	atomic_t ref_cnt;
	struct completion use_done;
	struct completion *drain_sq_done;
	struct nvmeib_recvq *recv_q;
	atomic_t rq_post_count;
	int drep_count;
	struct nvmeib_ref ib_rsrc_guard;

	/* Use dev's percpu CQ pool */
	struct nvmeib_dev_cq *dev_cq;
	struct list_head qp_action_list;

	struct nvmeib_rdma_evt_ctx *rdma_e_ctx;

	enum nvmeibs_logout_reason logout_reason;
	struct completion *logout_sent_comp;

	struct nvmeib_qp_stats_pcpu __percpu  *qp_stats;

	struct nvmeib_msg_hdr *msg_hdr; 
	dma_addr_t msg_hdr_dma_addr;
};
#define N2SI(_net_) (_net_->srq_info)

struct nvmeibs_net_init_target {
	struct nvmeibs_net_init *common;
	/* login request message */
	struct nvmeibc_login_request *req;
	/* login response message */
	struct nvmeibs_login_response *rsp;
};

struct nvmeibc_login_request;
struct nvmeibs_login_response;
struct nvmeibs_login_reject;
struct nvmeibs_net *nvmeibs_net_allocate_target(struct nvmeibs_net **net_handle,
	struct nvmeibs_net_init_target *p,
	struct nvmeibs_login_reject *rej);
void nvmeibs_net_release(struct nvmeibs_net *net, enum nvmeibs_logout_reason reason);
struct nvmeib_iu *nvmeibs_net_get_ioctx_(struct nvmeibs_net *net);
struct nvmeib_iu *nvmeibs_net_get_ioctx(struct nvmeibs_net *net);
void nvmeibs_net_put_ioctx(struct nvmeibs_net *net,
	struct nvmeib_iu *ioctx);
enum nvmeibs_net_state nvmeibs_net_get_qp_state(
	struct nvmeibs_net *net);
void nvmeibs_net_cancel_work(struct nvmeibs_net *net, struct nvmeib_iu *iu);

 
int nvmeibs_net_post_recvq(struct nvmeibs_net *net, 
			   struct nvmeib_recvq *recv_q, struct nvmeib_iu *recv_ioctx);

//obsolete moved to common and have implementation for roce and infiniband

void nvmeibs_send_login_reject(struct nvmeib_rdma_cm *cm_id,
                               struct nvmeibs_login_reject *rej,
                               struct nvmeibc_login_request *req);

/* net ref-count */
bool nvmeibs_net_inc(struct nvmeibs_net *net);
void nvmeibs_net_dec(struct nvmeibs_net *net);
void nvmeibs_net_wait_unused(struct nvmeibs_net *net);
void nvmeibs_net_dec_recv(struct nvmeibs_net *net, int n);
void nvmeibs_net_inc_recv(struct nvmeibs_net *net, int n);
void nvmeibs_rq_drain_comp(struct nvmeibs_net *net, struct ib_wc *wc);
void nvmeibs_net_on_drain_sq(struct nvmeibs_net *net);

static inline void nvmeibs_net_spin_lock_irqsave(
	struct nvmeibs_net *net, unsigned long *pflags)
{
	spin_lock_irqsave(&net->spinlock, *pflags);
	net->locking_cpu = smp_processor_id();
}

static inline void nvmeibs_net_spin_unlock_irqrestore(
	struct nvmeibs_net *net, unsigned long flags)
{
	net->locking_cpu = -1;
	spin_unlock_irqrestore(&net->spinlock, flags);
}

static inline bool nvmeibs_net_already_locked(struct nvmeibs_net *net)
{
	/* work assumption: ch is always locked with irqsave */
    return irqs_disabled() && net->locking_cpu == smp_processor_id();
}

void nvmeibs_net_trigger_qp_last_wqe_reached(void *ctx);

#define nvmeibs_ib_post_send(_net, _send_wr, _bad_send_wr) ({	    \
        int _rv;                                                    \
    	nvmeib_qp_stats_on_post_send((_net)->qp_stats);        		\
       _rv = ib_post_send((_net)->qp, _send_wr, _bad_send_wr);      \
       _rv;                                                       	\
})

#endif
