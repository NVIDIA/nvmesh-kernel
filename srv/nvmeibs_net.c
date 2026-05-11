/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibs_net.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeibs_ib_port.h"
#include "nvmeibs_client.h"
#include "nvmeibs_nordda.h"
#include "nvmeibs_main.h"
#include "nvmeib_utils.h"
#include "nvmeib_srq.h"
#include "nvmeib_version_shared.h"
#include "nvmeibs_trace.h"

#define C2NV(net) P2NV(net->params.port)
#define C2IB(net) P2IB(net->params.port)

#define ___NFIN	_ND(_name_, "--> @NAME.@NAME\n", net->params.cl->name, net->params.name)
#define ___NFOUT	_ND(_name_, "<-- @NAME.@NAME\n", net->params.cl->name, net->params.name)
#define __NFIN
#define __NFOUT

/**
 * nvmeib_test_and_set_state() - Test and set the qp state.
 *
 * Returns true if and only if the client state has been set to
 * the new state.
 */
static bool test_and_set_state(struct nvmeibs_net *net,
	enum nvmeibs_net_state old, enum nvmeibs_net_state new)
{
	unsigned long flags;
	enum nvmeibs_net_state prev;

	__NFIN;
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	prev = net->state;
	if (prev == old)
		net->state = new;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	__NFOUT;
	return prev == old;
}

/**
 * nvmeibs_test_and_set_state_not() - Test and set the qp state.
 *
 * Returns true if and only if the client state has been set to
 * the new state.
 */
static bool test_and_set_state_not(struct nvmeibs_net *net,
	enum nvmeibs_net_state masked, enum nvmeibs_net_state new)
{
	unsigned long flags;
	enum nvmeibs_net_state prev;

	__NFIN;
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	prev = net->state;
	if (prev != masked)
		net->state = new;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);

	__NFOUT;
	return prev != masked;
}

static enum nvmeibs_net_state
 __attribute__ ((unused)) set_state(struct nvmeibs_net *net,
	enum nvmeibs_net_state new_state)
{
	unsigned long flags;
	enum nvmeibs_net_state prev;

	__NFIN;
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	prev = net->state;
	net->state = new_state;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	__NFOUT;
	return prev;
}

bool nvmeibs_net_inc(struct nvmeibs_net *net)
{
	bool ret;
	__NFIN;
	ret = atomic_inc_not_zero_hint(&net->ref_cnt, 1);
	__NFOUT;
	return ret;
}

void nvmeibs_net_dec(struct nvmeibs_net *net)
{
	__NFIN;
	if (atomic_dec_and_test(&net->ref_cnt))
		complete(&net->use_done);
	__NFOUT;
}

void nvmeibs_net_wait_unused(struct nvmeibs_net *net)
{
	__NFIN;
	init_completion(&net->use_done);
	if (!atomic_dec_and_test(&net->ref_cnt))
		wait_for_completion(&net->use_done);
	__NFOUT;
}

void nvmeibs_net_dec_recv(struct nvmeibs_net *net, int n)
{
	int rq_post_count = 0;

	__NFIN;
	if (N2SI(net))
		nvmeib_srq_dec(N2SI(net), n);
	else if (net->recv_q) {
		rq_post_count = atomic_sub_return(n, &net->rq_post_count);
		_ND(trace_net_nvmeibs_net_dec_recv, "net @NET rq_post_count @RQ_POST_COUNT", net, rq_post_count);
		WARN_ON(rq_post_count < 0);
	}
	__NFOUT;
}

void nvmeibs_net_inc_recv(struct nvmeibs_net *net, int n)
{
	int rq_post_count;

	__NFIN;
	if (N2SI(net))
		nvmeib_srq_inc(N2SI(net), n);
	else if (net->recv_q) {
		rq_post_count = atomic_add_return(n, &net->rq_post_count);
		_NT(trace_net_nvmeibs_net_inc_recv, "net @NET rq_post_count @RQ_POST_COUNT", net, rq_post_count);
	}
	__NFOUT;
}

static inline bool nvmeibs_net_drained(struct nvmeibs_net *net)
{
	enum nvmeib_rdma_expect_last_wqe expect_last_wqe =
		nvmeib_rdma_expect_last_wqe(net->cm_id.cm_id);
	WARN_ON(expect_last_wqe == NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN);

	_NT(nvmeibs_net_drained, "net @NET: expect_last_wqe=@INT, qp_evt_last_wqe=@INT",
		net, expect_last_wqe, net->qp_evt_last_wqe);

	if (expect_last_wqe == NVMEIB_RDMA_EXPECT_LAST_WQE)
		return net->qp_evt_last_wqe > 0;
	else if (net->recv_q)
		return net->qp_rq_drain_recv > 0;
	else
		return true;
}

void nvmeibs_rq_drain_comp(struct nvmeibs_net *net, struct ib_wc *wc)
{
	unsigned long flags;
	
	__NFIN;
	if (wc->status != IB_WC_SUCCESS && wc->status != IB_WC_WR_FLUSH_ERR)
		_NE(error_net_nvmeibs_rq_drain_comp, "net: @NET Flush WR received with invalid status @STATUS", net, wc->status);
	if (net->state != QP_DRAINING)
		_NE(error_1_net_nvmeibs_rq_drain_comp, "net: @NET Flush @DIRECTION_STR WR received with net in invalid state @STATE", 
		   net, (wc->opcode & IB_WC_RECV ? "recv_q" : "send_q"), net->state);
	if (wc->opcode & IB_WC_RECV) {
		_NT(trace_net_nvmeibs_rq_drain_comp, "net: @NET recv_q flush received", net);
		net->qp_rq_drain_recv++;
	} else {
		_NE(error_2_net_nvmeibs_rq_drain_comp, "net: @NET Flush WR received with invalid opcode @OPCODE", net, wc->opcode);
	}
	if (nvmeibs_net_drained(net)) {
		nvmeibs_net_spin_lock_irqsave(net, &flags);
		if (net->release_done) {
			complete(net->release_done);
		}
		nvmeibs_net_spin_unlock_irqrestore(net, flags);
	}
	__NFOUT;
	
}

void nvmeibs_net_on_drain_sq(struct nvmeibs_net *net)
{
	unsigned long flags;
	NFIN;

	if (!net)
		_NE(error_net_nvmeibs_net_on_drain_sq, "net @NET, Invalid net", net);
	else {
		nvmeibs_net_spin_lock_irqsave(net, &flags);
		if (!net->drain_sq_done)
			_NW(warn_net_nvmeibs_net_on_drain_sq, "net @NET, No drain-sq comp-item", net);
		else {
			_NT(trace_net_nvmeibs_net_on_drain_sq, "net @NET, drain-sq done", net);
			complete(net->drain_sq_done);
		}
		nvmeibs_net_spin_unlock_irqrestore(net, flags);
	}

	NFOUT;
}

static inline void __drain_send_cq(struct nvmeibs_net *net)
{
	DECLARE_COMPLETION_ONSTACK(done);
	unsigned long flags;
	unsigned long timeout;
	DECLARE_IB_WC_ONSTACK(wc);
	int rv;
	int n;
	NFIN;

	nvmeibs_net_spin_lock_irqsave(net, &flags);
	net->drain_sq_done = &done;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);

	//ib_req_notify_cq(net->scq, IB_CQ_NEXT_COMP);
	while ((rv = nvmeib_post_sq_drain(net->qp)) == -ENOMEM) {
		_NT(trace_net_drain_send_cq, "scq overflow");
		if (!nvmeibs_use_pcpu_cq) {
			n = ib_poll_cq(net->scq, 1, &wc);
			_NT(trace_1_net_drain_send_cq, "net @NET, Found @NUM_MSGS send comps", net, n);
			ib_req_notify_cq(net->scq, IB_CQ_NEXT_COMP);
		}
		else {
			msleep(1);
		}
	}
	if (!rv) {
		n = 0;
		timeout = NVMEIB_WAIT_DRAIN_SQ * nvmeib_get_relax_timeouts();

		while ((n < NVMEIB_N_WAIT_DRAIN_QP) &&
			   (rv = wait_for_completion_interruptible_timeout(&done,
				   timeout)) <= 0) {
			_NE(trace_2_net_drain_send_cq, "net @PARAMS_NAME, @NET, Fail wait drain-sq (rv @RV) attempt #@NUM_RETRY_ATTEMPTS", net->params.name, net, rv, n);

			/* Debug "Fail to drain SQ"... */
			{
				DECLARE_IB_WC_ONSTACK(wc);
				int p = 0;
				int i = 0;
				do {
					p = ib_poll_cq(net->scq, 1 , &wc);
					_NE(trace_3_net_drain_send_cq,
						 "[@INT] poll-cq returned @INT wc ", i, p);
					i++;
				} while (p);
			}

			n++;
		}
	}
	else
		_NE(error_net_drain_send_cq, "net @NET, Fail to send drain-sq wr (@RV)", net, rv);

	nvmeibs_net_spin_lock_irqsave(net, &flags);
	net->drain_sq_done = NULL;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);

	NFOUT;
}

static inline void drain_send_cq(struct nvmeibs_net *net)
{
	NFIN;

	if (net->scq) {
		if (net->params.net_type != S_NET_IO) {
			__drain_send_cq(net);
		}
		else {
			_NT(drain_send_cq_t1, "Not draining SQ of RDDA-net");
		}
	}

	NFOUT;
}

static inline void drain_private_cqs(struct nvmeibs_net *net)
{
	struct nvmeib_iu *recv_ioctx;
	unsigned index;
	DECLARE_IB_WC_ONSTACK(wc);
	int n;

	NFIN;

	if (net->rcq &&
		(net->params.net_type == S_NET_ADMIN ||
		 net->params.net_type == S_NET_NORDDA)) {
		while ((n = ib_poll_cq(net->rcq, 1, &wc)) > 0) {
			_NT(trace_net_drain_cqs, "Found @COMPLETED recv comps...", n);
			index = nvmeib_idx_from_wc(&wc);
			if (net->srq_info) {
				recv_ioctx = nvmeib_srq_rtrv_recv(net->srq_info, index, net);
				nvmeib_srq_post_recv(net->srq_info, recv_ioctx);
			}
		}
	}

	drain_send_cq(net);

	NFOUT;
}

static inline void drain_per_dev_cq(struct nvmeibs_net *net)
{
	drain_send_cq(net);
}

static inline void drain_cqs(struct nvmeibs_net *net)
{
	__NFIN;

	if (!nvmeibs_use_pcpu_cq)
		drain_private_cqs(net);
	else
		drain_per_dev_cq(net);

	__NFOUT;
}

static int try_disconnect(struct nvmeibs_net *net, bool *wait_for_drep,
	bool *run_drain_cqs)
{
	DECLARE_COMPLETION_ONSTACK(release_done);
	int rv, rvw, n = 0;
	unsigned long flags;
	struct nvmeib_rdma_cm *cm;

	__NFIN;

	//make sure that release_done will be be erased from net safely
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	net->release_done = &release_done;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	cm = net->cm_id.cm_id;
	if (!(rv = nvmeib_rdma_disconnect(cm, net->qp, wait_for_drep))) {
		_NT(trace_net_try_disconnect, "net @NET state @STATE", net, net->state);
		if (!nvmeibs_use_pcpu_cq /* || !NVMEIB_PCPU_CQ_DEFER_RDMA_DESTROY */) {
		if (test_and_set_state_not(net, QP_DRAINING, QP_DRAINING)) {
			if (net->recv_q) {
				/* Post the drain wqe */
				_NT(trace_1_net_try_disconnect, "net @NET: Posting drain WR to receive queue", net);
				nvmeib_post_rq_drain(net->qp);
			}
		}

		if (net->params.after_qp_error)
			net->params.after_qp_error(net);

		while (!nvmeibs_net_drained(net) && !net->qp_evt_qp_fatal && n < NVMEIB_N_WAIT_BREAK_QP &&
		((rvw = wait_for_completion_interruptible_timeout(&release_done,
		NVMEIB_WAIT_BREAK_QP)) <= 0)) {
			_NT(trace_2_net_try_disconnect, "net @NET: Failed waiting for LAST WQE (rvw @RVW) attempt #@NUM_RETRY_ATTEMPTS, "
				"state @STATE, qp_evt_last_wqe: @QP_EVT_LAST_WQE qp_evt_qp_fatal: @QP_EVT_QP_FATAL qp_rq_drain_recv: @QP_RQ_DRAIN_RECV "
				"rq_post_count: @ATOMIC_READ cm_id @CM_ID rcq: @RCQ srq_info: @SRQ_INFO recv_q: @RECV_Q qp: @QP",
				net, rvw, n, net->state, net->qp_evt_last_wqe, net->qp_evt_qp_fatal, 
				net->qp_rq_drain_recv, atomic_read(&net->rq_post_count), net->cm_id.cm_id,
				net->rcq, net->srq_info, net->recv_q, net->qp);

			if (net->params.port->hw_type == DT_siw)
				WARN_KNOWN_EC(!n, 7629);
			else
				WARN_ON(!n);

			n++;
		}
		//moved to after bn-handler
		//drain_cqs(net);
		*run_drain_cqs = true;
		}
		else {
			if (net->params.after_qp_error)
				net->params.after_qp_error(net); //omril: not used, needed?
		}
	}
	else
		_NE(trace_3_net_try_disconnect,
			"net @NET: Failed rdma disconnect (rv @RV)", net, rv);
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	net->release_done = NULL;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	__NFOUT;
	return rv;
}

static void ib_net_unsol_disconnect(struct nvmeibs_net *net)
{
	__NFIN;
	if (net->cm_id.cm_id) {
		if (nvmeib_rdma_disconnect_reply(net->cm_id.cm_id))
			_ND(trace_net_ib_net_unsol_disconnect, "Sending CM DREP failed");
		else
			_ND(trace_1_net_ib_net_unsol_disconnect, "Sending CM DREP was OK");
	}
	nvmeibs_net_release(net, NVMEIBS_LOGOUT_REASON_UNSOL_DISCONNECTION);
	__NFOUT;
}

/**
 * drain_qp() - Drain a client by resetting the IB queue pair.
 * @cm_id: Pointer to the CM ID of the client to be drained.
 *
 */
static void drain_qp(struct nvmeibs_net *net, bool drep_rcvd)
{
	unsigned long flags;

	__NFIN;
	WARN_ON_ONCE(irqs_disabled());
	if (drep_rcvd) {
		_NT(trace_net_drain_qp_drep_rcvd, 
			"drep_rcvd, removing cm_id net=@NET, cm=@CM_ID", net, &net->cm_id);
		nvmeibs_remove_cm_id(&net->cm_id);
	}
	nvmeibs_net_spin_lock_irqsave(net, &flags);

	_NT(trace_0_net_drain_qp,
		"net=@NET, state @STATE, cm=@CM_ID, drep_comp=@BOOL",
		net, net->state, &net->cm_id, !!net->drep_comp);

	if (drep_rcvd) {
		net->drep_count++;
		if (net->drep_comp)
			complete(net->drep_comp);
	}
	if (net->state == QP_CONNECTING ||
		net->state == QP_LIVE) {
		/* if event is DERQ_RECEIVED,
		   state is already disconnecting */

		/* change to disconnecting so that
		   later cm-events will do nothing */
		net->state = QP_DISCONNECTING;
		
		nvmeibs_net_spin_unlock_irqrestore(net, flags);
		/* send cm-drep (redundant?!) and trigger net-release */
		ib_net_unsol_disconnect(net);
	}
	else {
		if (net->state == QP_DISCONNECTING) {
			_NT(trace_net_drain_qp, "reply received when the net was disconnecting net=@NET cm=@CM_ID",
				net, &net->cm_id);
			/* only net-release work can change this state !!! */
		}
		nvmeibs_net_spin_unlock_irqrestore(net, flags);
	}

	__NFOUT;
}

static void rts_handler_work(struct workqe_struct *w)
{
	struct nvmeibs_net *net = container_of(w, struct nvmeibs_net,
										   rts_handler_work);
	__NFIN;
	if (unlikely(atomic_read(&net->dying) ||
				 (nvmeibs_net_get_qp_state(net) != QP_LIVE))) {
		/* we check QP-LIVE to make sure work
		   was scheduled only from start-qp */
		_NT(trace_net_rts_handler_work, "net @NET release was scheduled - no use for rts", net);
	}
	else
		net->params.rts_handler(net->params.rts_context);
	__NFOUT;
}


/**
 * start_qp() - Process an IB_CM_RTU_RECEIVED or
 * USER_ESTABLISHED event.
 *
 * An IB_CM_RTU_RECEIVED message indicates that the connection is established
 * and that the recipient may begin transmitting (RTU = ready to use).
 *
 * ... Start QP (port WQ or cm-event-handler)
 * --> process pending CONFIG msgs (port WQ)
 * --> process CONFIG msgs (cl WQ)
 * --> process Login requests of secondary channels (cl WQ).
 *
 * Thus, before starting QP we must be ready for these login requests i.e.
 * we must first add the cl's cm_id to the port's hash table.
 *
 */
static void start_qp(struct nvmeibs_net *net)
{
	unsigned long flags;
	__NFIN;

	nvmeibs_net_spin_lock_irqsave(net, &flags);
	if (net->state == QP_CONNECTING) {
		_NT(trace_net_start_qp,
			"net @CL_NAME.@PARAMS_NAME(@NET): "
			"add rts-handler work and state CONNECTING to LIVE",
			net->params.cl->name, net->params.name, net);
		if (net->params.rts_handler) {
			if (net->params.net_type == S_NET_ADMIN)
				nvmeibs_client_add_work(net->params.cl,
					&net->rts_handler_work);
			else if (net->params.net_type == S_NET_NORDDA)
				nvmeibs_nordda_add_work(net->params.nrch,
					&net->rts_handler_work);
		}
		net->state = QP_LIVE;
	}
	else {
		_NE(error_net_start_qp, "Could not start QP, state @STATE (exp. QP_CONNECTING)", net->state);
	}
	nvmeibs_net_spin_unlock_irqrestore(net, flags);

	__NFOUT;
}

/**
 * disconnection_request() - Process reception of a DREQ
 * message.
 */
static void disconnection_request(struct nvmeibs_net *net)
{
	unsigned long flags;
	bool send_drep = false;

	__NFIN;
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	_NT(trace_net_disconnection_request, "net=@PTR, state=@STATE", net, net->state);
	switch (net->state) {
	case QP_CONNECTING:
	case QP_LIVE:
		send_drep = true;
		net->state = QP_DISCONNECTING;
		break;
	case QP_DISCONNECTING:
	case QP_DRAINING:
	case QP_RELEASING:
		_ND(trace_1_net_disconnection_request, "unexpected client state @STATE", net->state);
		break;
	}
	if (send_drep)
		/* send cm-drep and trigger net-release */
		ib_net_unsol_disconnect(net);
	nvmeibs_net_spin_unlock_irqrestore(net, flags);

	__NFOUT;
}

/* could have just added refcnt to @h but rather
   use same refcnt for both cm and qp as in clnt */
static bool cmh_ref_chg(struct nvmeib_cm_id *h, bool inc)
{
	struct nvmeibs_net *net = container_of(h, struct nvmeibs_net, cm_id);
	bool rv;

	if (!net->rdma_e_ctx) {
		_NE(cmh_ref_chg_e1, "OOPS, rdma events were stopped before removing cmh from hash");
		BUG();
	}

	if (inc) {
		rv = !!nvmeib_rdma_evt_ctx_get(net->rdma_e_ctx);
	}
	else {
		nvmeib_rdma_evt_ctx_put(net->rdma_e_ctx);
		rv = true;
	}

	return rv;
}

/* called from client_cm_event which is the on_peer_event cb.
   on_peer_event is called from ib_cm_handler_impl which is
   the generic cm event handle (i.e. new and existing conns)
   of the rdma (ib) listener the srv creates in add_one. */
static void target_cm_handler(struct nvmeib_cm_id *h,
	struct nvmeib_rdma_event *event)
{
	struct nvmeibs_net *net = container_of(h, struct nvmeibs_net, cm_id);
	u32 local_ib_id = (u32)-1;
	u32 remote_ib_id = (u32)-1;
	
	nvmeib_rdma_cm_conn_get_ib_ids(h->cm_id, &local_ib_id, &remote_ib_id);

	__NFIN;
	switch (event->event) {
	case NVMEIB_REJ_RECEIVED: /* connection reject received */
		_NT(trace_net_target_cm_handler, "Received IB REJ.");
		drain_qp(net, false);
		break;
	case NVMEIB_RTU_RECEIVED:
	case NVMEIB_USER_ESTABLISHED: /* client is ready to send messages */
		_NT(trace_1_net_target_cm_handler, "Received IB RTU (IDs: @LOCAL_IB_ID->@REMOTE_IB_ID).", local_ib_id, remote_ib_id);
		start_qp(net);
		break;
	case NVMEIB_DREQ_RECEIVED: /* connection is close by client */
		_NT(trace_2_net_target_cm_handler, "Received InfiniBand DREQ message.");
		disconnection_request(net);
		drain_qp(net, false);
		break;
	case NVMEIB_DREP_RECEIVED: /* we closed the connection and the client ack */
		_NT(trace_3_net_target_cm_handler, "Received InfiniBand DREP message.");
		drain_qp(net, true);
		break;
	case NVMEIB_DEVICE_REMOVED:
		_NT(trace_4_net_target_cm_handler, "Received device removed on net @NET CM @CM_ID", net, net->cm_id.cm_id);
		drain_qp(net, false);
		break;
	case NVMEIB_TIMEWAIT_EXIT:
		_NT(trace_5_net_target_cm_handler, "Received IB TimeWait exit.");
		drain_qp(net, false);
		break;
	case NVMEIB_REP_ERROR:
		_NT(error_net_target_cm_handler, "Received IB REP error.");
		drain_qp(net, false);
		break;
	case NVMEIB_DREQ_ERROR:
		_NT(error_1_net_target_cm_handler, "Received IB DREQ ERROR event.");
		break;
	case NVMEIB_MRA_RECEIVED:
		_NT(trace_6_net_target_cm_handler, "Received IB MRA event");
		break;
	default:
		_NE(error_2_net_target_cm_handler, "received unrecognized event @EVENT", event->event);
		break;
	}
	__NFOUT;
}

static void per_dev_cq_stop_events_n_comps(struct nvmeibs_net *net)
{
	_NT(t0_per_dev_cq_stop_events_n_comps,
		"SCQ: net=@PTR, dev_cq=@PTR, qp=@PTR", net, net->dev_cq, net->qp);

	/* remove cm_id (wrapper) from global hash
	   the order btw this and nvmeib_rdma-evt_ctx_stop doesn't matter */
	if (net->cm_id.cm_id) {
		net->cm_id.cm_id = NULL;
		nvmeibs_remove_cm_id(&net->cm_id);
	}

	if (net->rdma_e_ctx) {
		/* 1) stop forwarding new cm/qp events to net-layer and wait
			  for in-progress events to complete before FREEing s-net
		   2) call this BEFORE nvmeib_cq_qp_del() which asynchronously
			  (later) calls nvmeib_cq_qp_destroy_action_f which kfrees
			  net->rdma_e_ctx */
		nvmeib_rdma_evt_ctx_stop(net->rdma_e_ctx);
		net->rdma_e_ctx = NULL;
	}

	//if (net->qp) {
	if (net->dev_cq) {
		/* detach from CQ:
		   stop forwaring polled wc to net-layer and wait
		   for already polled WCs' processing to complete */
		nvmeib_cq_qp_stop(net->dev_cq, (u64)net->qp);
		nvmeib_cq_qp_del(net->dev_cq, (u64)net->qp, &net->qp_action_list);
		BUG_ON(!list_empty(&net->qp_action_list));
		net->scq = NULL;
		net->rcq = NULL;

		/* keep net->qp till after we drain all defered recv-ioctx's which
		   can send-rsp i.e. use the qp (e.g. nrch from its wq) */
		#if 0
		//net->qp = NULL;
		#endif

		/* keep srq-info till after ad_handler() which
		   s-nrch needs for post-recv of stashed recv-bufs */
		#if 0
		//nvmeib_cq_put(C2NV(net), net->dev_cq, nt_to_ct(net->params.net_type));
		//net->dev_cq = NULL;
		//net->srq_info = NULL;
		#endif
	}
}

static void per_dev_cq_put_cq(struct nvmeibs_net *net)
{
	//WARN_ON_ONCE(net->qp);

	if (net->dev_cq) {
		nvmeib_cq_put(C2NV(net), net->dev_cq, nt_to_ct(net->params.net_type));
		net->qp = NULL;
		net->dev_cq = NULL;
		net->srq_info = NULL;
	}
}

static void detach_from_per_dev_cq(struct nvmeibs_net *net)
{
	per_dev_cq_stop_events_n_comps(net);
	per_dev_cq_put_cq(net);
}

static void destroy_ib_rscs(struct nvmeibs_net *net)
{
	NFIN;

	if (!nvmeibs_use_pcpu_cq) {
		if (net->srq_info) {
			nvmeib_srq_info_put(net->srq_info, net);
			net->srq_info = NULL;
		}
	}
	else {
		net->srq_info = NULL;
	}

	if (!nvmeibs_use_pcpu_cq) {
		_NT(trace_net_destroy_ib_rscs,\
			"Destory QP=@QP qpn=@QP_NUM", net->qp, net->qp->qp_num);
		nvmeib_rdma_destroy_qp(net->cm_id.cm_id, net->qp);
		net->qp = NULL;

		_NT(destroy_ib_rscs_t1, "net=@PTR, net-scq=@PTR, net-rcq=@PTR", net, net->scq, net->rcq);
		// must destroy rcq before scq, because in process_rcq_completion it
		// may call process_scq_completion_imp if  net->scq is not NULL, causes crash.
		if (net->params.rcq_size) {
			ib_destroy_cq(net->rcq);
			net->rcq = NULL;
		}
		ib_destroy_cq(net->scq);
		net->scq = NULL;
	}
	else {
		_NT(destroy_ib_rscs_t2,
			"SCQ: net=@PTR, dev_cq=@PTR, qp=@PTR", net, net->dev_cq, net->qp);
		/* break-qp should have already detached qp from pcpu-cq */
		if (net->rdma_e_ctx || net->dev_cq) {
			WARN_ON_ONCE(1);
			detach_from_per_dev_cq(net);
		}
	}

	__NFOUT;
}

static void free_pending_received_msgs(struct nvmeibs_net *net)
{
	struct list_head *l = &net->pending_received_msgs;
	struct nvmeib_iu *recv_ioctx, *recv_ioctx_t;
	NFIN;
	if (net->srq_info) {
		list_for_each_entry_safe(recv_ioctx, recv_ioctx_t, l, free_tx_n) {
			list_del_init(&recv_ioctx->free_tx_n);
			NVMEIB_IU_RESET_RCV_IDX(recv_ioctx);
			nvmeib_srq_post_recv(net->srq_info, recv_ioctx);
		}
	}
	NFOUT;
}

static int send_logout_and_wait_rsp(struct nvmeibs_net const *net) 
{
	long wait_rv;
	int rv;

	WARN_ON(!net);
	WARN_ON(!net->logout_sent_comp);

	if((rv = nvmeibs_client_logout(net)))
		return rv;
	wait_rv = wait_for_completion_interruptible_timeout(net->logout_sent_comp,
	 NVMEIB_WAIT_FOR_LOGOUT_RSP);
	if (wait_rv <= 0) {
		_NW(send_logout_and_wait_rsp_timeout_f, "LOGOUT: net @NET waiting for timeout of logout msg failed with: @INT",
		 	net, wait_rv);
		return -ETIMEDOUT; 
	} else {
		_NT(send_logout_and_wait_rsp_timeout_p, "LOGOUT: net @NET waiting for timeout done successfully",
			 net); 
		return 0;
	}
}

static void release_work(struct workqe_struct *w)
{
	struct nvmeibs_net *net = container_of(w, struct nvmeibs_net, release_work);
	struct nvmeibs_net_init *params = &net->params;
	struct nvmeibs_client *cl = net->params.cl;
	enum nvmeibs_net_state prev_state;
	struct nvmeibs_login_reject rej = {{0}};
	DECLARE_COMPLETION_ONSTACK(release_done);
	DECLARE_COMPLETION_ONSTACK(logout_sent);
	bool wait_for_drep = false;
	unsigned long flags;
	long wait_rv;
	int logout_rv;
	struct nvmeib_iu *iu, *tmp;
	bool run_drain_cqs = false;

	__NFIN;
	_NT(trace_net_release_work, "net @NET release - start", net);

	/* lock_ch_pre_rw - sched release of secondary nets and then resched self.
	   This is done from cl-wq to serialize with secondary nets connect work */
	if (net->params.pre_rw_handler && !net->params.pre_rw_handler_called) {
		net->params.pre_rw_handler(net->params.pre_rw_context);
		net->params.pre_rw_handler_called = true;
		_NT(trace_1_net_release_work, "net @NET release - resched", net);
		nvmeibs_client_add_work(net->params.cl, &net->release_work);
		goto out;
	}

	_ND(trace_2_net_release_work, "net = @NET, net->cm_id = @CM_ID, net->cm_id.cm_id=@CM_ID",
		net, &net->cm_id, net->cm_id.cm_id);
	WARN_ON(params->port == NULL);
	nvmeibs_net_wait_unused(net);
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	prev_state = net->state;
	switch (prev_state) {
	case QP_CONNECTING:
	case QP_LIVE:
		net->state = QP_DISCONNECTING;
		break;	
	default:
		break;
	}
	if (net->cm_id.cm_id && nvmeib_rdma_wait_for_drep(net->cm_id.cm_id) && !net->drep_count) {
		net->drep_comp = &release_done;
	}
	_NT(trace_3_net_release_work,
		"net @NET state: prev @PREV_STATE, curr @STATE,"
		"drep_comp=@BOOL, drep_cpount=@INT",
		net, prev_state, net->state, !!net->drep_comp, net->drep_count);
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	switch (prev_state) {
	case QP_CONNECTING:
		/* Sends a connection rejection message to the remote node. */
		rej.reason = __constant_cpu_to_be32(IB_CM_REJ_NO_RESOURCES);
		if (!net->connection_accepted) {
			_NT(trace_4_net_release_work, "Rejected login on net @NET", net);
			nvmeibs_send_login_reject(net->cm_id.cm_id, &rej, NULL);
		}
		FALLTHRU;
	case QP_LIVE:
		if (prev_state == QP_LIVE && net->params.net_type == S_NET_ADMIN) {
			net->logout_sent_comp = &logout_sent;
			if((logout_rv = send_logout_and_wait_rsp(net))) {
				_NW(release_work_logout_fail, "LOGOUT @NET failed with @RV",
				 	net, logout_rv);
			}
		}
		FALLTHRU;
	case QP_DISCONNECTING:
		/* set qp to err-state, send cm disconnect req/rep and
		   wait for LAST WQE qp-event */
		_NT(trace_5_net_release_work, "try disconnect net @NET", net);
		if (try_disconnect(net, &wait_for_drep, &run_drain_cqs) < 0) {
			_NT(trace_6_net_release_work, "Sending CM DREQ failed.");
		}
		_NT(trace_7_net_release_work, "finished try disconnect on @NET", net);
		break;
	case QP_DRAINING:
		//wait = true;
		run_drain_cqs = true;
		break;
	case QP_RELEASING:
		run_drain_cqs = true;
		break;
	}
	if (wait_for_drep && net->drep_comp && !cl->drep_timeout) {
		_ND(trace_8_net_release_work, "Waiting for qp to release (@PARAMS_NAME) on net @NET state=@STATE", net->params.name,
			net, net->state);
		_NT(trace_9_net_release_work, "Waiting for repl on net @NET", net);
		wait_rv = wait_for_completion_interruptible_timeout(
			net->drep_comp, NVMEIB_WAIT_DREP_TIMEOUT(P2NV(params->port)->dev_type));
		_NT(trace_10_net_release_work, "Fnished to wait on net @NET", net);
		if (!wait_rv)
			cl->drep_timeout = true;
		_ND(trace_11_net_release_work, "QP relesed with code @WAIT_RV (@PARAMS_NAME)", wait_rv, net->params.name);
	}

	_NT(trace_12_net_release_work, "@NET", net);
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	net->drep_comp = NULL;
	net->state = QP_RELEASING;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);

	/* stop ib-comp-handlers (and wait for wip ones) before freeing any rscs
	   they (or remote peer) may use via bn_handler */
	if (nvmeibs_use_pcpu_cq) {
		_NT(t0_net_release_work,
			"SCQ: net=@PTR, dev_cq=@PTR, qp=@PTR", net, net->dev_cq, net->qp);
		per_dev_cq_stop_events_n_comps(net);
	}

	/* stuff before net is closed */
	if (params->bn_handler) {
		_NT(trace_13_net_release_work, "@NET", net);
		/* nrch: nordda_bn_handler    : wait pending-recv and disk-outstanding, free iocmds and rscs
		   ioch: free_io_channel_net  : mask iosq msi and dma-unmap */
		params->bn_handler(params->bn_context);
		_NT(trace_14_net_release_work, "@NET", net);
	}
	if (run_drain_cqs)
		drain_cqs(net);

	/* stuff after-drain-cqs */
	if (params->ad_handler) {
		params->ad_handler(params->ad_context); /* nrch: nordda_ad_handler, free queued rxiu's */
	}

	/* all stashed rx-bufs have been posted back to SRQ, we can refput SRQ & CQ.
	   they are also sending cl/io-rsp (e.g. from nrch-wq ctx), can nullify qp */
	if (nvmeibs_use_pcpu_cq) {
		per_dev_cq_put_cq(net);
	}

	/* return pending recv-iu back to (private/shared) pool */
	_NT(trace_15_net_release_work, "@NET", net);
	free_pending_received_msgs(net);
	_NT(trace_16_net_release_work, "@NET", net);
	if (!nvmeibs_use_pcpu_cq && P2NV(params->port)->dev_type == DT_siw) {
		/* For SIW, we must destroy the CM before destroy QP, or it will not free the QP */
		nvmeib_rdma_destroy_cm(net->cm_id.cm_id);
		net->cm_id.cm_id = NULL;
	}

	nvmeib_ref_release_start(&net->ib_rsrc_guard);
	nvmeib_ref_release_wait(&net->ib_rsrc_guard);
	destroy_ib_rscs(net);

	if (params->after_destroy_ib)
		params->after_destroy_ib(params->after_destroy_ib_ctx); /* nrch nordda_after_destroy_ib_handler, free's wcs */

	/* at this point we can safely nullify send comp as all ib resources freed */
	net->logout_sent_comp = NULL;

	if (!nvmeibs_use_pcpu_cq) {
		_NT(trace_17_net_release_work, "@NET", net);
		nvmeib_rdma_destroy_cm(net->cm_id.cm_id);
		net->cm_id.cm_id = NULL;
		_NT(trace_18_net_release_work, "@NET", net);
		nvmeibs_remove_cm_id(&net->cm_id);
	}
	_NT(trace_19_net_release_work, "@NET", net);
	WARN_ON(params->port == NULL);
	if (params->port) {
		list_for_each_entry_safe(iu, tmp, &net->uncomp_msgs, free_tx_n) {
			if (iu->io_done) {
				kfree(iu->io_done);
				iu->io_done = NULL;
			}
			list_del_init(&iu->free_tx_n);
		}
		/* JH IOMMU: DMA_TO_DEVICE is correct. Buffers are source for local RDMA_SEND */
		nvmeib_free_ioctx_ring(net->ioctx_ring, C2IB(net), params->sendq_size,
					   params->s_msg_size, DMA_TO_DEVICE, NULL);

		nvmeib_ref_put(&params->port->n_port_conns);
	}

	if (net->msg_hdr) {
		nvmeib_public_ib_dma_free_coherent(
			C2IB(net), sizeof(*net->msg_hdr), net->msg_hdr, net->msg_hdr_dma_addr);
		net->msg_hdr = NULL;
	}

	if (net->qp_stats) {
		nvmeib_qp_stats_free(net->qp_stats);
		net->qp_stats = NULL;
	}

	_NT(trace_20_net_release_work, "net @NET release - done", net);
	if (net->params.free_on_net_rls)
		kfree(net);

out:
	NFOUT;
}

/**
 * release_client() - Release qp resources.
 *
 * Schedules the actual release because:
 * - Calling the ib_destroy_cm_id() call from inside an IB CM callback would
 *   trigger a deadlock.
 */
static void start_release(struct nvmeibs_net *net)
{
	unsigned long flags;

	__NFIN;
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	if (net->release_done) {
		_NT(trace_net_start_release, "we have release done so triggering it on net @NET", net);
		complete(net->release_done);
	} else
		_NT(trace_1_net_start_release, "release done null on net @NET", net);
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	__NFOUT;
}

/**
 * qp_event() - QP event callback function.
 */
static void qp_event(struct ib_event *event, struct nvmeibs_net *net)
{
	__NFIN;

	_NT(trace_net_qp_event, "QP event @EVENT on net @CL_NAME.@PARAMS_NAME(@NET), cm_id=@CM_ID state=@STATE",
		event->event, net->params.cl->name, net->params.name, net, net->cm_id.cm_id, nvmeibs_net_get_qp_state(net));

	switch (event->event) {
	case IB_EVENT_COMM_EST:
		nvmeib_rdma_notify(net->cm_id.cm_id, event->event);
		break;
	case IB_EVENT_QP_LAST_WQE_REACHED:
		_NT(trace_1_net_qp_event, "Received qp LAST_WQE_REACHED (#@QP_EVT_LAST_WQE)", net->qp_evt_last_wqe);
		net->qp_evt_last_wqe++;

		if (nvmeibs_net_get_qp_state(net) == QP_DISCONNECTING)
			test_and_set_state_not(net, QP_RELEASING, QP_DRAINING);
		if (test_and_set_state(net, QP_DRAINING, QP_RELEASING)) {
			_NT(trace_2_net_qp_event, "calling start_release on net @NET", net);
			start_release(net);
		} else
			_NT(trace_3_net_qp_event, "net state (@QP_STATE) unchanged, unsolicited LAST_WQE",
				nvmeibs_net_get_qp_state(net));
		break;
	case IB_EVENT_QP_FATAL:
		_NT(trace_4_net_qp_event, "Received QP_FATAL (#@QP_EVT_QP_FATAL), terminating connection",
		   net->qp_evt_qp_fatal);
		net->qp_evt_qp_fatal++;
		nvmeibs_net_release(net, NVMEIBS_LOGOUT_REASON_IB_EVENT_QP_FATAL);
		break;
	case IB_EVENT_QP_ACCESS_ERR:
		_NT(trace_6_net_qp_event, "Received QP_ACCESS_ERR, terminating connection");
		nvmeibs_net_release(net, NVMEIBS_LOGOUT_REASON_IB_EVENT_QP_ACCESS_ERR);
		break;
	case IB_EVENT_SQ_DRAINED:
		_NT(trace_5_net_qp_event, "Received SQ_DRAINED");
		break;
	default:
		_NE(error_net_qp_event, "Received unrecognized IB QP event @EVENT", event->event);
		break;
	}
	__NFOUT;
}

static void s_rdma_qp_event(struct ib_event *event, void *context)
{
	struct nvmeib_rdma_evt_ctx *c = context;
	struct nvmeibs_net *net;

	net = nvmeib_rdma_evt_ctx_get(c);
	if (net) {
		qp_event(event, net);
		nvmeib_rdma_evt_ctx_put(c);
	}
	else {
		_NT(s_rdma_qp_event_t1, "Fail to get ctx @PTR (evt=@INT)", c, event->event);
	}
}

/**
 * cq_event() - CQ event callback function.
 */
static void cq_event(struct ib_event *event, void *context)
{
	struct nvmeibs_net *net = context;
	__NFIN;

	_NT(trace_s_net_cq_event, "CQ event @EVENT on net @CL_NAME.@PARAMS_NAME(@NET), cm_id=@CM_ID state=@STATE",
		event->event, net->params.cl->name, net->params.name, 
		net, net->cm_id.cm_id,nvmeibs_net_get_qp_state(net));
	
	__NFOUT;
}

extern struct nvmeib_intr_shaper *s_intr_shaper;

/**
 * scq_handler() - SCQ event handler
 */
static void s_net_scq_handler(struct ib_cq *cq, void *context)
{
	struct nvmeibs_net *net = context;
	__NFIN;
	if (nvmeib_ref_get(&net->ib_rsrc_guard)) {
		nvmeib_intr_shaper_intr_enter(s_intr_shaper, INTR_SHAPER_INTR_TYPE_SERVER_SCQ);
		net->params.scq_handler(cq, net->params.scq_context);
		nvmeib_intr_shaper_intr_exit(s_intr_shaper);
		nvmeib_ref_put(&net->ib_rsrc_guard);
	}
	__NFOUT;
}

/**
 * rcq_handler() - RCQ event handler
 */
static void s_net_rcq_handler(struct ib_cq *cq, void *context)
{
	struct nvmeibs_net *net = context;
	__NFIN;
	nvmeib_qp_stats_on_interrupt(net->qp_stats);
	if (nvmeib_ref_get(&net->ib_rsrc_guard)) {
		nvmeib_intr_shaper_intr_enter(s_intr_shaper, INTR_SHAPER_INTR_TYPE_SERVER_RCQ);
		net->params.rcq_handler(cq, net->params.rcq_context);
		nvmeib_intr_shaper_intr_exit(s_intr_shaper);
		nvmeib_ref_put(&net->ib_rsrc_guard);
	}
	__NFOUT;
}

/**
 * create_ib() - Create receive and send completion queues.
 */
static int create_ib_private_cq(struct nvmeibs_net *net)
{
	struct nvmeibs_net_init *params = &net->params;
	struct ib_qp_init_attr *qp_init;
	int qp_access = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE;
	int rv, scq_vect, rcq_vect;
	enum nvmeib_cq_vector_get_type vector_type;

	__NFIN;
	BUG_ON(!params->scq_handler);
	BUG_ON(!params->scq_size);
	WARN_ON(params->sendq_size < 1);

	rv = -ENOMEM;
	qp_init = kzalloc(sizeof(*qp_init), GFP_KERNEL);
	if (!qp_init)
		goto out;
	switch (params->net_type) {
		case S_NET_ADMIN:
			vector_type = NVMEIB_CQ_VECTOR_GET_TYPE_ADMIN;
			break;
		case S_NET_IO:
			vector_type = NVMEIB_CQ_VECTOR_GET_TYPE_IO;
			break;
		case S_NET_LOCK:
		case S_NET_LOCK_2ND:
			vector_type = NVMEIB_CQ_VECTOR_GET_TYPE_LOCK;
			break;
		case S_NET_NORDDA:
			vector_type = NVMEIB_CQ_VECTOR_GET_TYPE_NORDDA;
			break;
		default:
			BUG();
	}
	nvmeib_cq_vector_get(C2NV(net), params->name, vector_type, params->ch_index, &scq_vect, params->rcq_size ? &rcq_vect : NULL);
	net->scq = nvmeib_create_cq(C2IB(net), s_net_scq_handler, cq_event,
				    net, params->scq_size, scq_vect);
	if (IS_ERR(net->scq)) {
		rv = PTR_ERR(net->scq);
		_NE(error_net_create_ib, "failed to create send CQ cqe= @SCQ_SIZE rv= @RV", params->scq_size, rv);
		goto out;
	}

	ib_req_notify_cq(net->scq, IB_CQ_NEXT_COMP);
	if (!params->rcq_size)
		net->rcq = net->scq;
	else {
		BUG_ON(!params->rcq_handler);
		net->rcq = nvmeib_create_cq(C2IB(net), s_net_rcq_handler, cq_event,
					    net, params->rcq_size, rcq_vect);
		if (IS_ERR(net->rcq)) {
			rv = PTR_ERR(net->rcq);
			_NE(error_1_net_create_ib, "failed to create receive CQ cqe= @RCQ_SIZE rv= @RV",
				params->rcq_size, rv);
			goto err_destroy_scq;
		}
#ifdef SIM_NO_MSI
		ib_req_notify_cq(net->rcq, IB_CQ_NEXT_COMP);
#else
		if (params->net_type == S_NET_ADMIN ||
			params->net_type == S_NET_NORDDA)
			ib_req_notify_cq(net->rcq, IB_CQ_NEXT_COMP);
#endif
	}

	qp_init->qp_context = (void *)net;
	qp_init->event_handler =
		(void(*)(struct ib_event *, void*))qp_event;
	qp_init->send_cq = net->scq;
	qp_init->recv_cq = net->rcq;
	if (params->use_srq) {
		net->srq_info = params->srq_priv ? :
			nvmeib_srq_info_get(C2NV(net), params->srq_type);
		if (!net->srq_info) {
			_NE(error_2_net_create_ib, "Failed to set srq-info");
			rv = -1;
			goto err_destroy_rcq;
		}
		qp_init->srq = nvmeib_srq_info_ib_srq(net->srq_info);
	}
	else {
		if (params->recv_q) {
			qp_init->cap.max_recv_sge = params->max_recv_sge;
			qp_init->cap.max_recv_wr = params->recvq_size + 1;
		}
	}
	qp_init->sq_sig_type = IB_SIGNAL_REQ_WR;
	qp_init->qp_type = IB_QPT_RC;
	qp_init->cap.max_send_wr = params->sendq_size;
	qp_init->cap.max_send_sge = params->max_send_sge;

	_NT(trace_net_create_ib, "requested: max_cqe= @CQE max_sge= @MAX_SEND_SGE sq_size = @SENDQ_SIZE",
		net->scq->cqe, params->max_send_sge, params->sendq_size);
	if (net->params.use_atomic) {
		qp_access |= IB_ACCESS_REMOTE_ATOMIC;
	}
	net->qp = nvmeib_rdma_create_qp(net->cm_id.cm_id,
		P2NV(net->params.port)->pd, qp_init, net->params.port->port, 0,
		qp_access);
	if (!net->qp) {
		_NE(error_3_net_create_ib, "failed to create_qp");
		rv = -1;
		goto err_srq_info;
	}
	else {
		_NT(trace_1_net_create_ib, "------> New QP with index @QP_NUM", net->qp->qp_num);
		rv = 0;
	}
	params->asendq_size = qp_init->cap.max_send_wr;
	_ND(trace_2_net_create_ib, "granted: max_cqe= @CQE max_sge= @MAX_SEND_SGE sq_size = @MAX_SEND_WR",
		net->scq->cqe, qp_init->cap.max_send_sge, qp_init->cap.max_send_wr);

	if (params->recv_q) {
		int n_posted;
		net->recv_q = params->recv_q;
		rv = nvmeib_fill_recvq(net->recv_q, net->qp, &n_posted);
		if (rv < 0)
			goto err_destroy_qp;

		atomic_set(&net->rq_post_count, n_posted + 1); /* Offset by 1 until we start draining it */
	}

out:
	kfree(qp_init);
	__NFOUT;
	return rv;
	
err_destroy_qp:
	nvmeib_rdma_destroy_qp(NULL, net->qp);
	net->qp = NULL;

err_srq_info:
	if (params->use_srq && net->srq_info && !params->srq_priv) {
		nvmeib_srq_info_put(net->srq_info, net);
		net->srq_info = NULL;
	}

err_destroy_rcq:
	if (params->rcq_size) {
		ib_destroy_cq(net->rcq);
		net->rcq = NULL;
	}

err_destroy_scq:
	ib_destroy_cq(net->scq);
	net->scq = NULL;
	goto out;
}

bool nvmeibs_mostly_idle_ch = false;
module_param_named(mostly_idle_ch, nvmeibs_mostly_idle_ch, bool, 0644);
MODULE_PARM_DESC(mostly_idle_ch, "Defines whether to use the first shared CQ for \"mostly\" idle channels.");


static DEV_CQ_PROCESS_FUNC(process_per_dev_cq);
static int create_ib_per_dev_cq(struct nvmeibs_net *net)
{
	struct nvmeibs_net_init *params = &net->params;
	struct ib_qp_init_attr *qp_init = NULL;
	struct nvmeib_cq_qp_destroy_action *qp_da = NULL;
	int qp_access = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE |
		(net->params.use_atomic ? IB_ACCESS_REMOTE_ATOMIC : 0);
	struct nvmeib_dev_cq *dev_cq = NULL;
	struct ib_qp *qp;
	bool is_mostly_idle;
	int rv;
	__NFIN;

	/* sanity */
	if (!nvmeibs_use_pcpu_cq || !params->use_srq) {
		_NT(create_ib_per_dev_cq_t1, "Invalid arg (@INT, @INT)",
			nvmeibs_use_pcpu_cq, params->use_srq);
		rv = -EINVAL;
		goto out;
	}
	if (net->dev_cq || net->qp) {
		_NE(create_ib_per_dev_cq_e1, "OOPS net @PTR inuse (@PTR, @PTR)",
			net, net->dev_cq, net->qp);
		rv = -EINVAL;
		goto out;
	}

	if (!(qp_init = kzalloc(sizeof(*qp_init), GFP_KERNEL)) ||
		!(qp_da = kzalloc(sizeof(*qp_da), GFP_KERNEL))) {
		_NE(create_ib_per_dev_cq_e2, "Fail to alloc");
		rv = -ENOMEM;
		goto err;
	}

	is_mostly_idle = nvmeibs_mostly_idle_ch &&
		(net->params.net_type == S_NET_ADMIN || 
		 net->params.net_type == S_NET_LOCK  ||
		 net->params.net_type == S_NET_LOCK_2ND);

	/* Get CQ from dev's pcpu pool */
	dev_cq = nvmeib_cq_get(
		C2NV(net), nt_to_ct(net->params.net_type), process_per_dev_cq, is_mostly_idle, -1);
	if (!dev_cq) {
		_NE(create_ib_per_dev_cq_e3, "Failed to receive CQ object");
		rv = -ENODATA;
		goto err;
	}
	_NT(create_ib_per_dev_cq_t2, "Acquired CQ @PTR", dev_cq);
	
	/* Init QP attr */
	qp_init->srq = nvmeib_srq_info_ib_srq(nvmeib_cq_get_srq(dev_cq));
	qp_init->event_handler = s_rdma_qp_event;
	qp_init->qp_context = net->rdma_e_ctx;
	qp_init->cap.max_send_wr = params->sendq_size;
	qp_init->cap.max_send_sge = params->max_send_sge;
	qp_init->sq_sig_type = IB_SIGNAL_REQ_WR;
	qp_init->qp_type = IB_QPT_RC;
	qp_init->send_cq = nvmeib_cq_get_cq(dev_cq);
	qp_init->recv_cq = nvmeib_cq_get_cq(dev_cq);

	/* Create QP */
	qp = nvmeib_rdma_create_qp(net->cm_id.cm_id,
		P2NV(net->params.port)->pd, qp_init, net->params.port->port, 0,
		qp_access);
	if (!qp) {
		_NE(create_ib_per_dev_cq_e4, "Failed to create-qp");
		rv = -1;
		goto err_dev_cq;
	}
	_NT(create_ib_per_dev_cq_t3, "Created QP @PTR, qpn=@INT", qp, qp->qp_num);
	_ND(create_ib_per_dev_cq_d1, "requested/granted: max_sge=@INT/@INT, sq_size=@INT/@INT",
	   params->max_send_sge, qp_init->cap.max_send_sge,
	   params->sendq_size, qp_init->cap.max_send_wr);
	params->asendq_size = qp_init->cap.max_send_wr;

	rv = nvmeib_cq_qp_add(dev_cq, (u64)qp, net);
	if (rv) {
		_NE(create_ib_per_dev_cq_e5, "Fail to add QP to dev CQ (rv=@INT)", rv);
		goto err_destroy_qp;
	}

	/* Populate net */
	net->qp = qp;
	net->dev_cq = dev_cq;
	net->scq = nvmeib_cq_get_cq(dev_cq);
	net->rcq = nvmeib_cq_get_cq(dev_cq);
	net->srq_info = nvmeib_cq_get_srq(dev_cq);

	/* Mark cm so that only nvmeib_cq_qp_*() API may destroy qp and cm */
	nvmeib_rdma_cm_owner_set(net->cm_id.cm_id, 0, -1);

	/* Populate net's action for destroying qp and cm */
	qp_da->qp = qp;
	qp_da->cm_id = net->cm_id.cm_id;
	qp_da->rdma_e_ctx = net->rdma_e_ctx;
	qp_da->action.f = nvmeib_cq_qp_destroy_action_f;
	list_add(&qp_da->action.action_link, &net->qp_action_list);

	_NT(create_ib_per_dev_cq_t4, "net=@PTR, dev_cq=@PTR, send&recv-cq=@PTR, qp=@PTR, srq_info=@PTR, qp_da=@PTR",
		net, net->dev_cq, net->rcq, net->qp, net->srq_info, qp_da);

	rv = 0;
	goto out;

err_destroy_qp:
	nvmeib_rdma_destroy_qp(net->cm_id.cm_id, qp);

err_dev_cq:
	if (dev_cq)
		nvmeib_cq_put(C2NV(net), dev_cq, nt_to_ct(net->params.net_type));
err:
	kfree(qp_da);

	/* clearance */
	BUG_ON(!list_empty(&net->qp_action_list));
	BUG_ON(net->dev_cq);
	BUG_ON(net->qp);

out:
	kfree(qp_init);

	__NFOUT;
	return rv;
}

static int create_ib(struct nvmeibs_net *net)
{
	int rv;
	__NFIN;

	rv = !nvmeibs_use_pcpu_cq ?
		create_ib_private_cq(net) :
		create_ib_per_dev_cq(net);

	__NFOUT;
	return rv;
}

static void reuse_ioctx(struct nvmeib_iu *ioctx)
{
	NFIN;
	ioctx->work_handler = NULL;
	ioctx->work_payload = NULL;
	ioctx->io_done = NULL;
	NFOUT;
}

struct nvmeibs_net *nvmeibs_net_allocate_target(struct nvmeibs_net **net_handle,
	struct nvmeibs_net_init_target *p,
	struct nvmeibs_login_reject *rej)
{
	struct nvmeibs_net_init *params = p->common;
	struct nvmeibs_net *net = NULL;
	struct rdma_conn_param conn_params = {0};
	int rv;
	unsigned long flags;
	struct ib_qp_attr qp_attr = {0};
	struct ib_qp_init_attr qp_init_attr = {0};
	u32 local_ib_id, remote_ib_id;
	enum nvmeibs_logout_reason reason;
	u8 req_msg_hdr_in_flags;

	NFIN;
	*net_handle = NULL;
	net = kzalloc(sizeof(*net), GFP_KERNEL);
	if (!net) {
		rej->reason =
			__constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		_NE(error_net_nvmeibs_net_allocate_target, "rejected NVMEIB_LOGIN_REQ because no memory.");
		goto reject;
	}

	if (nvmeibs_use_pcpu_cq) {
		/* create and init ctx for rdma (qp and cm) events handlers */
		BUG_ON(net->rdma_e_ctx);
		if (!(net->rdma_e_ctx = nvmeib_rdma_evt_ctx_create(net))) {
			_NE(nvmeibs_net_allocate_target_e1, "Fail create rdma-evt-ctx");
			goto free_net;
		}
	}

	_NT(trace_net_nvmeibs_net_allocate_target, "net @NET, init IB rscs and accept cm_id=@CM_ID...", net, params->cm_id);
	if (!(net->qp_stats = nvmeib_qp_stats_alloc())) {
		_NE(nvmeibs_net_allocate_target_allco_qpstats_fail, "Fail to alloc pcpu-stats");
		goto free_ctx;
	}

	INIT_LIST_HEAD(&net->qp_action_list);
	atomic_set(&net->ref_cnt, 1);
	net->locking_cpu = -1;
	/* start state */
	//*net_handle = net;
	WQ_INIT_WORK(&net->release_work, release_work);
	WQ_INIT_WORK(&net->rts_handler_work, rts_handler_work);
	INIT_LIST_HEAD(&net->pending_received_msgs);
	spin_lock_init(&net->spinlock);
	memcpy(&net->params, params, sizeof(net->params));
	net->state = QP_CONNECTING;
	net->cm_id.cm_id = params->cm_id;
	net->cm_id.ref_chg = !nvmeibs_use_pcpu_cq ? NULL : cmh_ref_chg;
	net->cm_id.handler = params->cm_handler ?: target_cm_handler;
	net->cm_id.cm_evt_handle = false;
	spin_lock_init(&net->cm_id.cm_evt_state_lock);
	nvmeib_ref_init(&net->ib_rsrc_guard);

	/* init send messages */
	INIT_LIST_HEAD(&net->free_send_msgs);
	INIT_LIST_HEAD(&net->uncomp_msgs);
	
	nvmeibc_login_req_get_msg_hdr(p->req, NULL, &req_msg_hdr_in_flags, NULL, NULL);
	if (req_msg_hdr_in_flags == NVMEIBC_LOGIN_MSG_HDR_REQUIRED) {
		/* Allocate the msg header to be prepended to each outgoing msg */
		if (!(net->msg_hdr = nvmeib_public_ib_dma_alloc_coherent(
			C2IB(net), sizeof(*net->msg_hdr), 
			&net->msg_hdr_dma_addr, GFP_KERNEL | ___GFP_ZERO)))
		{
			rej->reason =
				__constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
			_NE(error_net_nvmeibs_net_allocate_target_msg_hdr_oom, "rejected NVMEIB_LOGIN_REQ because no memory.");
			goto free_qp_stats;
		}
	}

	/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer are source for local RDMA_SEND */
	net->ioctx_ring = nvmeib_alloc_ioctx_ring(C2IB(net),
		params->sendq_size, sizeof(*net->ioctx_ring[0]),
		params->s_msg_size, DMA_TO_DEVICE, &net->free_send_msgs, net);
	if (!net->ioctx_ring) {
		rej->reason =
			__constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		_NE(error_1_net_nvmeibs_net_allocate_target, "rejected NVMEIB_LOGIN_REQ because no memory.");
		goto free_msg_hdr;
	}

	/* init IB resources */
	if ((rv = create_ib(net))) {
		rej->reason = __constant_cpu_to_be32(
			NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		_NE(error_2_net_nvmeibs_net_allocate_target, "rejected NVMEIB_LOGIN_REQ because creating "
		   "a new IB resources failed (error code = @RV).", rv);
		goto free_ring;
	}

	if (net->msg_hdr) {
		/* Initialise msg header and login response (now that we have qp_num) */
		p->rsp->ext5.msg_hdr.out_flags = NVMEIBS_LOGIN_WITH_MSG_HDR;
		p->rsp->ext5.msg_hdr.index = cpu_to_be32(net->qp->qp_num);
		p->rsp->ext5.msg_hdr.sid = cpu_to_be64((unsigned long)net);
		
		net->msg_hdr->version = cpu_to_be32(nvmeib_version_get().protocol.all);
		net->msg_hdr->flags = 0;
		net->msg_hdr->src.global.index = p->rsp->ext5.msg_hdr.index;
		net->msg_hdr->src.global.sid = p->rsp->ext5.msg_hdr.sid;
		nvmeibc_login_req_get_msg_hdr(p->req, NULL, NULL,
					&net->msg_hdr->dst.global.index, 
					&net->msg_hdr->dst.global.sid);
	}

	/* Expose @net now that nvmeibs-net_release can be safely called on it.
	   This was originally above */
	*net_handle = net;

	/* Add the CM_ID to the hash table before we do the nvmeib_rdma_accept.
	   This solves the race where the client replies with the RTU before
	   nvmeib_rdma_accept returns */
	nvmeibs_add_cm_id(&net->cm_id);
	
	conn_params.private_data = (void *)p->rsp;
	conn_params.private_data_len = sizeof(*p->rsp);
	conn_params.responder_resources = nvmeib_device_get_max_rd_atom_on_wire(P2NV(net->params.port)->dev_type);
	conn_params.initiator_depth = 0; // No Read/Atomic Ops go from target to client on lock channel
	conn_params.flow_control = 1;
	conn_params.rnr_retry_count = 7;
	conn_params.srq = params->use_srq ? 1 : 0;
	conn_params.qp_num = net->qp->qp_num;
	/* modify QP ->RTR->RTS and send cm connection reply */
	net->connection_accepted = true;
	
	nvmeib_rdma_cm_conn_get_ib_ids(net->cm_id.cm_id, &local_ib_id, &remote_ib_id);
	_NT(trace_1_net_nvmeibs_net_allocate_target, "Sending CM RESP (IDs: @LOCAL_IB_ID->@REMOTE_IB_ID) net:@NET qp:@QP", local_ib_id, remote_ib_id, net, net->qp);
	
	if ((rv = nvmeib_rdma_accept(
		net->cm_id.cm_id, net->qp, &conn_params)) < 0) {
		rej->reason = __constant_cpu_to_be32(
			NVMEIBS_LOGIN_REJ_UNABLE_ESTABLISH_CONNECTION);
		nvmeibs_send_login_reject(net->cm_id.cm_id, rej, p->req);
		_NE(error_3_net_nvmeibs_net_allocate_target, "Failed rdma accept (rv = @RV)", rv);
		reason = NVMEIBS_LOGOUT_REASON_CONNECTION_ERR;
		goto release_net;
	}

	/* Get the net's destination QPnum, SQ PSN, retry count and retry timeout */
	rv = ib_query_qp(net->qp, &qp_attr, IB_QP_RETRY_CNT | IB_QP_TIMEOUT | 
				IB_QP_DEST_QPN | IB_QP_SQ_PSN, &qp_init_attr);
	rv = 0;
	nvmeib_rdma_get_remote_qpn(net->cm_id.cm_id, &net->remote_qpn);
	net->sq_psn = qp_attr.sq_psn;

	_NT(trace_2_net_nvmeibs_net_allocate_target, "rdma accept completed QPn: @QP_NUM Remote QPn: @REMOTE_QPN SQ PSN: @SQ_PSN Retry Count: @RETRY_CNT Timeout: @TIMEOUT",
		net->qp->qp_num, net->remote_qpn, net->sq_psn, qp_attr.retry_cnt, qp_attr.timeout);

	/* Check for any CM events that occurred while we were bringing up the QP */
	spin_lock_irqsave(&net->cm_id.cm_evt_state_lock, flags);

	D_TRACE_CM_EVENT_CNT(error_4_net_nvmeibs_net_allocate_target, net->cm_id.cm_evt_cnt);
//	D_PRINT_CM_EVENT_CNT(net->cm_id.cm_evt_cnt);

	if (net->cm_id.cm_evt_cnt.dreq ||
	    net->cm_id.cm_evt_cnt.timewt_exit) {
		_NT(trace_3_net_nvmeibs_net_allocate_target, "client sent dreq (@DREQ) or timewt_exit (@INT)",
		   net->cm_id.cm_evt_cnt.dreq, net->cm_id.cm_evt_cnt.timewt_exit);
		net->cm_id.cm_evt_handle = false;
		spin_unlock_irqrestore(&net->cm_id.cm_evt_state_lock, flags);
		nvmeib_rdma_disconnect_reply(net->cm_id.cm_id);
		reason = NVMEIBS_LOGOUT_REASON_DREQ_FROM_CLNT;
		goto release_net;
	} else if (net->cm_id.cm_evt_cnt.rej || net->cm_id.cm_evt_cnt.drep ||
		   net->cm_id.cm_evt_cnt.rep_err || net->cm_id.cm_evt_cnt.dreq_err || 
		   net->cm_id.cm_evt_cnt.dev_err) {
		/* Received an unexpected CM Event - Report error and destroy QP */
		_NT(trace_4_net_nvmeibs_net_allocate_target, "Unexpected CM Event, destroying QP");
		net->cm_id.cm_evt_handle = false;
		spin_unlock_irqrestore(&net->cm_id.cm_evt_state_lock, flags);
		reason = NVMEIBS_LOGOUT_REASON_CM_ERR;
		goto release_net;
	}

	/* Set CM Event State to process CM Events */
	net->cm_id.cm_evt_handle = true;
	spin_unlock_irqrestore(&net->cm_id.cm_evt_state_lock, flags);

	if (net->cm_id.cm_evt_cnt.rtu || net->cm_id.cm_evt_cnt.usr_est) {
		_NT(trace_5_net_nvmeibs_net_allocate_target, "RTU already received. Calling start_qp");
		/* Received RTU or User-Established Event - Start QP */
		start_qp(net);
	}
	goto out;

release_net:
	_NT(trace_6_net_nvmeibs_net_allocate_target, "cm/qp err after net (qp) created and (recv) completions may arrive");
	/* initiate net-release (graceful break qp) and return @net to caller */
	nvmeibs_net_release(net, reason);
	goto out;

free_ring:
	/* JH IOMMU: DMA_TO_DEVICE is correct. Buffers are source for local RDMA_SEND */
	nvmeib_free_ioctx_ring(net->ioctx_ring, C2IB(net), params->sendq_size,
		params->s_msg_size, DMA_TO_DEVICE, &net->free_send_msgs);
	
free_msg_hdr:
	if (net->msg_hdr) {
		nvmeib_public_ib_dma_free_coherent(
			C2IB(net), sizeof(*net->msg_hdr), 
			net->msg_hdr, net->msg_hdr_dma_addr);
	}

free_qp_stats:
	nvmeib_qp_stats_free(net->qp_stats);

free_ctx:
	if (net->rdma_e_ctx) {
		nvmeib_rdma_evt_ctx_stop(net->rdma_e_ctx);
		nvmeib_rdma_evt_ctx_destroy(net->rdma_e_ctx);
		net->rdma_e_ctx = NULL;
	}

free_net:
//	/* Remove the CM_ID from the hash table */
//	nvmeibs_remove_cm_id(&net->cm_id);

	_NT(trace_7_net_nvmeibs_net_allocate_target, "free net @NET - net-release-work shall not run", net);
	kfree(net);
	*net_handle = net = NULL;

reject:
	/*nvmeibs_send_login_reject(params->cm_id, rej, p->req);*/

out:
	NFOUT;
	return net;
}

/**
 * nvmeibs_release_net() - Close qp by setting the QP error
 * state.
 *
 * Reset the QP and make sure all resources associated with the
 * client will be deallocated at an appropriate time.
 *
 */
void nvmeibs_net_release(struct nvmeibs_net *net, enum nvmeibs_logout_reason reason)
{
	bool already_locked;
	unsigned long flags = 0;

	__NFIN;

	/* before locking the net let it trigger net-release of its parent */
	if (net->params.net_type == S_NET_LOCK_2ND) {
		_NT(trace_4_net_nvmeibs_net_release,
			"net @CL_NAME.@PARAMS_NAME(@NET), trigget net-release of lock-ch",
			net->params.cl->name, net->params.name, net);
		nvmeibs_net_release(net->params.cl->lock_net, reason);
	}
	if (net->params.net_type == S_NET_LOCK) {
		_NT(trace_5_net_nvmeibs_net_release,
			"net @CL_NAME.@PARAMS_NAME(@NET), trigget net-release of main-ch",
			net->params.cl->name, net->params.name, net);
		nvmeibs_net_release(net->params.cl->net, reason);
	}

	if (!(already_locked = nvmeibs_net_already_locked(net)))
		nvmeibs_net_spin_lock_irqsave(net, &flags);
	if (atomic_inc_return(&net->dying) == 1) {
		_NT(trace_net_nvmeibs_net_release,
			"net @CL_NAME.@PARAMS_NAME(@NET), state @STATE, "
			"initiated from '@__BUILTIN_RETURN_ADDRESS_FUNC' with reason: @INT",
			net->params.cl->name, net->params.name,
			net, net->state, __builtin_return_address(0), reason);

		if (!nvmeibs_use_pcpu_cq) {
			/* Re-enable RCQ notification to drain the RQ / SRQ */
			ib_req_notify_cq(net->rcq, IB_CQ_NEXT_COMP);
		}
		
		if (net->params.ka_handler)
			net->params.ka_handler(net->params.ka_context);

#if 0
		/* lock_ch_pre_rw */
		if (net->params.pre_rw_handler)
			net->params.pre_rw_handler(net->params.pre_rw_context);
#endif
		
		if (net->params.net_type == S_NET_ADMIN) {
			net->logout_reason = reason;
		}
		nvmeibs_client_add_work(net->params.cl, &net->release_work);
		/* if this function was triggered by the client peer and we
		   are on the main admin channel we must tell the client that
		   the other side is on die process
		*/
		if (net->params.rw_handler) {
			_NT(trace_1_net_nvmeibs_net_release, "Main admin q of client @CL_NAME is disconnecting,"
			   "schedule client-release work", net->params.cl->name);
			net->params.rw_handler(net->params.rw_context);
		}
		else 
			_ND(trace_2_net_nvmeibs_net_release, "LOCK/IO q @PRIV_INT of client @CL_NAME is disconnecting",
				(int)(u64)net->priv, net->params.cl->name);
	}
	else
		_NT(trace_3_net_nvmeibs_net_release, "net @NET, already dying", net);
	if (!already_locked)
		nvmeibs_net_spin_unlock_irqrestore(net, flags);
	__NFOUT;
}

struct nvmeib_iu *nvmeibs_net_get_ioctx_(struct nvmeibs_net *net)
{
	struct nvmeib_iu *ioctx;

	__NFIN;
	if ((ioctx = list_first_entry_or_null(
		&net->free_send_msgs, struct nvmeib_iu, free_tx_n))) {
		list_del_init(&ioctx->free_tx_n);
		BUG_ON(ioctx->priv != net);
		reuse_ioctx(ioctx);
	}
	__NFOUT;
	return ioctx;
}

/**
 * nvmeibs_get_send_ioctx() - Obtain an I/O context for sending
 * to the client.
 */
struct nvmeib_iu *nvmeibs_net_get_ioctx(struct nvmeibs_net *net)
{
	struct nvmeib_iu *ioctx;
	unsigned long flags;

	__NFIN;
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	ioctx = nvmeibs_net_get_ioctx_(net);
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	__NFOUT;
	return ioctx;
}

void nvmeibs_net_put_ioctx(struct nvmeibs_net *net, struct nvmeib_iu *ioctx)
{
	unsigned long flags;

	__NFIN;
	nvmeibs_net_spin_lock_irqsave(net, &flags);
	if (ioctx->io_done == NULL) {
		list_add(&ioctx->free_tx_n, &net->free_send_msgs);
	} else {
		list_add(&ioctx->free_tx_n, &net->uncomp_msgs);
	}
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	__NFOUT;
}

int nvmeibs_net_post_recvq(struct nvmeibs_net *net, 
			   struct nvmeib_recvq *recv_q, struct nvmeib_iu *recv_ioctx)
{
	int rv;

	if (!atomic_read(&net->dying)) {
		if (!(rv = nvmeib_post_recvq(recv_q, net->qp, recv_ioctx))) {
			int rq_post_count = atomic_inc_return(&net->rq_post_count);
			_ND(trace_net_nvmeibs_net_post_recvq, "rq_post_count @RQ_POST_COUNT", rq_post_count);
		}
	} else {
		if (atomic_read(&net->rq_post_count) == 0 && net->release_done)
			complete(net->release_done);
		rv = -ESHUTDOWN;
	}

	return rv;
}

enum nvmeibs_net_state nvmeibs_net_get_qp_state(struct nvmeibs_net *net)
{
	unsigned long flags;
	enum nvmeibs_net_state state;

	nvmeibs_net_spin_lock_irqsave(net, &flags);
	state = net->state;
	nvmeibs_net_spin_unlock_irqrestore(net, flags);
	return state;
}

void nvmeibs_net_cancel_work(struct nvmeibs_net *net, struct nvmeib_iu *iu)
{
	int i;

	__NFIN;
	for (i = 0; i < net->params.sendq_size; ++i)
		if (net->ioctx_ring[i] != iu) {
			_ND(nvmeibs_net_cancel_work_d1, "remove: iu=@PTR, ring[@INT]=@PTR", iu, i, net->ioctx_ring[i]);
			//cancel_work_sync(&net->ioctx_ring[i]->work);
		}
		else
			_ND(nvmeibs_net_cancel_work_d2, "keep: iu=@PTR, ring[@INT]=@PTR", iu, i, net->ioctx_ring[i]);
	__NFOUT;
}

void nvmeibs_send_login_reject(struct nvmeib_rdma_cm *cm_id,
	struct nvmeibs_login_reject *rej,
	struct nvmeibc_login_request *req)
{
	NFIN;
	if (likely(cm_id && rej)) {
		rej->hdr.opcode = NVMEIB_LOGIN_REJ;
		rej->hdr.tag = 0;
		rej->s_version = cpu_to_be64(nvmeib_version_get().all);
		_NT(nvmeibs_send_login_reject_t1, "reject: opcode=@STR, reason=@INT",
			msg_op_to_str(rej->hdr.opcode), be32_to_cpu(rej->reason));
		nvmeib_rdma_login_reject(cm_id, (void *)rej, sizeof(*rej));
	}
	else
		_NE(error_net_nvmeibs_send_login_reject, "Inavlid cm_id=@CM_ID or rej=@REJ_PTR", cm_id, rej);
	NFOUT;
}

void nvmeibs_net_trigger_qp_last_wqe_reached(void *ctx)
{
	struct nvmeibs_net *net = ctx;

	if (!nvmeibs_use_pcpu_cq) {
		_NE(nvmeibs_net_trigger_qp_last_wqe_reached_e1, "OOPS, net @PTR here but pcpu cq is off", net);
		return;
	}

	if (net->srq_info)
		++net->qp_evt_last_wqe;
	else if (net->recv_q)
		++net->qp_rq_drain_recv;
}

/*
 * Per device (shared) CQs
 */

static void handle_recv(struct nvmeibs_net *net, struct ib_wc *wc)
{
	net->params.recv_comp_h(net->params.rcq_context, wc);
}

static void handle_send(struct nvmeibs_net *net, struct ib_wc *wc)
{
	NFIN;

	if (nvmeib_opcode_from_wc(wc) == NVMEIB_DRAIN_QUEUE) {
		nvmeibs_net_on_drain_sq(net);
	}
	else {
		net->params.send_comp_h(net->params.scq_context, wc);
	}

	NFOUT;
}

static DEV_CQ_PROCESS_FUNC(process_per_dev_cq)
{
	struct nvmeibs_net *net = ctx;
	u64 start, delta;
	int i;

	NFIN;

	start = jiffies;
	for_each_set_bit(i, wcs_mask, n_wcs) {
		struct ib_wc *wc = &wcs[i];
		if (wc->status != IB_WC_SUCCESS) {
			_NE(s_process_per_dev_cq, "wc=@PTR wc-status=@NVMEIB_STATUS_STR(@WC_STATUS): "
				"wc->wr_id=@LLX={ver=@X, opc=@X, idx=@X (is_recv=@BOOL)}, "
				"wc->opcode=@WC_OPCODE, "
				"wc->qp=@QP, net=@NET, ct=(@INT, @STR)",
				wc, nvmeib_status_str(&wc->status), wc->status,
				wc->wr_id,
				nordda_wr_id_decode_version(nvmeib_wr_id_from_wc(wc)),
				nvmeib_opcode_from_wc(wc),
				nvmeib_idx_from_wc(wc),
				nvmeib_opcode_from_wc(wc) == NVMEIB_RECV,
				wc->opcode,
				wc->qp, net,
				nt_to_ct(net->params.net_type), ch_type_to_str(nt_to_ct(net->params.net_type)));
		}
		//if (wc->opcode & IB_WC_RECV)
		if (nvmeib_opcode_from_wc(wc) == NVMEIB_RECV)
			handle_recv(net, wc);
		else
			handle_send(net, wc);
	}
	delta = jiffies - start;
	_ND(process_per_dev_cq_d1, "processing time = @INT64", delta);
	NFOUT;
}
