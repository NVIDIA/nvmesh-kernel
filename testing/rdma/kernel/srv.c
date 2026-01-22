/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/string.h>

#include <linux/memory.h>
#include <linux/io.h>
#include "u_ib_incs.h"

#define DEBUG_TEST 0

#include "u.h"
#include "u.c"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("IB Server Test Driver");
MODULE_LICENSE("GPL and additional rights");

static LIST_HEAD(dev_list);

static u64 service_guid = SERVICE_GUID;

static void add_one(struct ib_device *device);
static void remove_one(struct ib_device *device);
static struct ib_client client = {
	.name   = "srvib",
	.add    = add_one,
	.remove = remove_one
};

/* per IB device - the one we get in add_one */
struct _dev {
	struct u_dev u_dev;
	struct ib_cm_id *cm_id;
	struct ib_event_handler	event_handler;
};

/* per port on IB device */
struct ib_port {
	/* owner device */
	struct _dev *_dev;
	/* port stuff */
	bool enabled;
	u8 port;
	/* link into the port list inside the device */
	struct list_head link;
};

/* the main structure - create on each client login */
struct _net {
	/* on which port the login arrives */
	struct ib_port *ib_port;
	/* the cm_id from the listen */
	struct ib_cm_id *cm_id;
	/* the qp, send_cq & receive_cq */
	struct ib_qp *qp;
	struct ib_cq *scq;
	struct ib_cq *rcq;
	/* the response to the client when accepting the login */
	struct s_login_response rsp;
	/* the test info from the client */
	int message_size;
	int n_messages;
	int srq;
	int test_type;
	/* send WQE */
	struct ib_send_wr *wr;
	/* send SGE */
	struct ib_sge *sge;
	/* the send buffers */
	struct u_iu *snd_bufs;
	/* send completions */
	struct ib_wc *s_wcs;
	/* the receive buffers - in the case we are NOT using SRQ */
	struct u_iu *rcv_bufs;
	/* receive completions */
	struct ib_wc *r_wcs;
	/* page for RDMA tests */
	void *page_ptr;
	u64 page;
	/* work for killing the net structure */
	struct work_struct release_work;
	/* true when we done */
	bool done;
};

/* initializing new IB device wrapper */
static struct _dev *init(struct ib_device *device)
{
	struct _dev *_dev = NULL;
	int srq_size;

	FIN;
	_dev = kzalloc(sizeof *_dev, GFP_KERNEL);
	if (!_dev)
		goto out;

	_dev->u_dev.ib_dev = device;

	_dev->u_dev.dev_attr =
		kzalloc(sizeof(*_dev->u_dev.dev_attr), GFP_KERNEL);
	if (!_dev->u_dev.dev_attr) 
		goto free_dev;

	if (ib_query_device(_dev->u_dev.ib_dev, _dev->u_dev.dev_attr)) {
		_W("Query device failed\n");
		goto free_attr;
	}

	_dev->u_dev.pd = ib_alloc_pd(device);
	if (IS_ERR(_dev->u_dev.pd))
		goto free_attr;

	_dev->u_dev.mr = ib_get_dma_mr(_dev->u_dev.pd,
							IB_ACCESS_LOCAL_WRITE |
							IB_ACCESS_REMOTE_READ |
							IB_ACCESS_REMOTE_WRITE);
	if (IS_ERR(_dev->u_dev.mr))
		goto err_pd;

	srq_size = _dev->u_dev.dev_attr->max_srq_wr;
	_I("SRQ size %d, msg_size %d\n", srq_size, (int)PAGE_SIZE);
	if (u_create_srq(&_dev->u_dev, srq_size, PAGE_SIZE, NULL)) {
		_E("%s ib_create_srq() failed.\n", device->name);
		goto err_pd;
	}
	goto out;

err_pd:
	ib_dealloc_pd(_dev->u_dev.pd);

free_attr:
	kfree(_dev->u_dev.dev_attr);

free_dev:
	kfree(_dev);

out:
	FOUT;
	return _dev;
}

/* find a port on the device port list */
struct ib_port *find_ib_port(struct _dev *_dev, u8 port)
{
	struct ib_port *ib_port, *tmp;

	FIN;
	list_for_each_entry_safe(ib_port, tmp, &_dev->u_dev.port_list, link) {
		if (ib_port->port == port) {
			FOUT;
			return ib_port;
		}
	}
	FOUT;
	return NULL;
}

/* in the case we are NOT using SRQ - return the received buffer */
static void post_rcv(struct _net *net, struct u_iu *iu)
{	
	struct ib_recv_wr wr = {0};
	struct ib_recv_wr *bad_wr;
	struct ib_sge sge;
	int rv;

	//FIN;
	sge.addr   = iu->dma;
	sge.length = iu->size;
	sge.lkey   = net->ib_port->_dev->u_dev.mr->lkey;

	wr.next     = NULL;
	wr.wr_id    = iu->index << 1;
	wr.sg_list  = &sge;
	wr.num_sge  = 1;
	_D("dma=%llx size=%lx wr_id=%llx qp=%p lkey=%x\n", iu->dma, iu->size, wr.wr_id, net->qp, sge.lkey);
	rv = ib_post_recv(net->qp, &wr, &bad_wr);
	if (rv) {
		_E("rv=%x wr.wr_id=%llx &wr=%p bad_wr=%p\n", rv, wr.wr_id, &wr, bad_wr);
		BUG_ON(1);
	}
	//FOUT;
}

/* the test net is done so receive its allocations */
static void free_net(struct _net *net)
{
	struct u_iu *iu;
	int i;

	FIN;
	for (i = 0; i < net->n_messages; ++i) {
		iu = &net->snd_bufs[i];
		if (iu->buf) {
			ib_dma_free_coherent(net->qp->device, iu->size, iu->buf, iu->dma);
			iu->buf = NULL;
		}
		iu = &net->rcv_bufs[i];
		if (iu->buf) {
			ib_dma_free_coherent(net->qp->device, iu->size, iu->buf, iu->dma);
			iu->buf = NULL;
		}
	}
	kfree(net->snd_bufs);
	net->snd_bufs = NULL;
	kfree(net->rcv_bufs);
	net->rcv_bufs = NULL;
	kfree(net->wr);
	net->wr = NULL;
	kfree(net->sge);
	net->sge = NULL;
	kfree(net->s_wcs);
	net->s_wcs = NULL;
	kfree(net->r_wcs);
	net->r_wcs = NULL;
	FOUT;
}

/* call in the context of a system workqueue -
   free the test net and its IB resources
*/
static void destroy_net(struct work_struct *work)
{
	struct _net *net = container_of(work, struct _net, release_work);

	FIN;

	free_net(net);
	if (net->cm_id) {
		ib_destroy_cm_id(net->cm_id);
		net->cm_id = NULL;
	}
	if (net->qp) {
		ib_destroy_qp(net->qp);
		net->qp = NULL;
	}
	if (net->rcq) {
		ib_destroy_cq(net->rcq);
		net->rcq = NULL;
	}
	if (net->scq) {
		ib_destroy_cq(net->scq);
		net->scq = NULL;
	}
	kfree(net);
	FOUT;
}

/* QP event - arrived in an interupt context - we this is the QP end we start
   a release work on a system workqueue - nothing else can be done
   in that context.
*/
static void qp_event(struct ib_event *event, void *p)
{
	struct _net *net = p;

	FIN;
	_W("QP event %d\n", event->event);
	if (event->event == IB_EVENT_QP_LAST_WQE_REACHED) {
		INIT_WORK(&net->release_work, destroy_net);
		schedule_work(&net->release_work);
	}
	FOUT;
}

/* send completion handler - we are in interrupt context */
static void s_completion_callback(struct ib_cq *cq, void *ctx)
{
	struct _net *net = ctx;
	int i, rv;

	FIN;
	if (net->done)
		goto out;
	/* ask for next notification */
	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	while ((rv = ib_poll_cq(cq, net->n_messages, net->s_wcs)) != 0) {
		/* chek if error and if in error we are out */
		if (rv < 0) {
			_E("rv=%d\n", rv);
			goto out;
		}
		else {
			_D("rv=%d\n", rv);
			for (i = 0; i < rv; ++i) {
				_D("wc.wr_id=%d\n", (int)(net->s_wcs[i].wr_id >> 1));
				if (net->s_wcs[i].status != IB_WC_SUCCESS) {
					_E("(net->s_wcs[%d].status=%d\n", i, net->s_wcs[i].status);
					goto out;
				}
			}
		}
	}

out:
	FOUT;
}

/* receive completion handler - we are in interrupt context */
static void r_completion_callback(struct ib_cq *cq, void *ctx)
{
	struct _net *net = ctx;
	struct ib_send_wr *bad_wr;
	struct u_iu *iu;
	int index;
	int i, m, n, rv;
	struct ib_event event;
	int call_s = net->n_messages;

	FIN;
	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	while ((rv = ib_poll_cq(cq, net->n_messages, net->r_wcs)) != 0) {
		/* chek if error and if in error we are out */
		if (rv < 0) {
			_E("rv=%d\n", rv);
			goto out;
		}
		else {
			_D("rv=%d\n", rv);
			n = rv;
			for (i = 0; i < n; ++i) {
				if (net->r_wcs[i].status != IB_WC_SUCCESS) {
					/* we have a completion with error state (probably the FLUSH
					   error) - we must check that if we are done and if
					   we are start a destroy_net process
					*/
					if (net->done) {
						/* mimic a QP event */
						event.event = IB_EVENT_QP_LAST_WQE_REACHED;
						// qp_event(&event, net);
					}
					goto out;
				}
				/* read the received buffer index from the completion */
				m = (int)(net->r_wcs[i].wr_id >> 1);
				_D("wc.wr_id=%d\n", m);
				/* get the buffer depending if we use SRQ */
				iu = net->srq ?
					u_rtrv_recv(&net->ib_port->_dev->u_dev, m) :
					&net->rcv_bufs[m];
				/* sync for CPU access */
				ib_dma_sync_single_for_cpu(net->ib_port->_dev->u_dev.ib_dev,
					iu->dma, iu->size, DMA_FROM_DEVICE);
				/* read the arrived index - this will tell which message
				   the client sent
				*/
				index = *(int *)iu->buf;
				_D("index=%d net=%p\n", index, net);
				/* once we know the index we may post back
				   the received buffer
				*/
				if (net->srq)
					u_post_recv(&net->ib_port->_dev->u_dev, iu, NULL);
				else
					post_rcv(net, iu);
				/* send back the matching send buffer */
				if ((rv = ib_post_send(
					net->qp, &net->wr[index], &bad_wr)) < 0) {
					_E("ib_post_send() returned %d for net=%p index=%d\n", rv, net, index);
					goto out;
				}

				if (--call_s) {
					call_s = net->n_messages;
					s_completion_callback(net->scq, ctx);
				}
			}
		}
	}

	s_completion_callback(net->scq, ctx);

out:
	FOUT;
}

/* when we poll we do not need compelition callback */
static void sr_completion_callback(struct ib_cq *cq, void *ctx)
{
	BUG();
}

/* create net buffers */
static void init_net_data(struct _net *net)
{
	struct u_iu *iu;
	int i;

	FIN;
	_D("Allocating info\n");
	if (!(net->snd_bufs = kzalloc(
		sizeof(*net->snd_bufs) * net->n_messages, GFP_KERNEL)) ||
		!(net->rcv_bufs = kzalloc(
			sizeof(*net->rcv_bufs) * net->n_messages, GFP_KERNEL)) ||
		!(net->wr = kzalloc(
			sizeof(*net->wr) * net->n_messages, GFP_KERNEL)) ||
		!(net->sge = kzalloc(
			sizeof(*net->sge) * net->n_messages, GFP_KERNEL)) ||
		!(net->s_wcs = kzalloc(
			sizeof(*net->s_wcs) * net->n_messages, GFP_KERNEL)) ||
		!(net->r_wcs = kzalloc(
			sizeof(*net->r_wcs) * net->n_messages, GFP_KERNEL))) {
		_E("Fail to allocate net data\n");
		BUG_ON(true);
	}
	for (i = 0; i < net->n_messages; ++i) {
		iu = &net->snd_bufs[i];
		iu->size = net->message_size;
		iu->index = i;
		BUG_ON(!(iu->buf = ib_dma_alloc_coherent(
			net->ib_port->_dev->u_dev.ib_dev, iu->size, &iu->dma, GFP_KERNEL)));
		*(int *)iu->buf = i;
		net->sge[i].addr = iu->dma;
		net->sge[i].length = net->message_size;
		net->sge[i].lkey = net->ib_port->_dev->u_dev.mr->lkey;
		iu = &net->rcv_bufs[i];
		iu->size = net->message_size;
		iu->index = i;
		BUG_ON(!(iu->buf = ib_dma_alloc_coherent(
			net->ib_port->_dev->u_dev.ib_dev, iu->size, &iu->dma, GFP_KERNEL)));
		_D("&net->rcv_bufs[%d]=%p iu=%p size=%ld dma=%llx buf=%p\n", i,
			&net->rcv_bufs[i], iu, iu->size, iu->dma, iu->buf);
		net->wr[i].opcode = IB_WR_SEND;
		net->wr[i].wr_id = (i << 1) | 1;
		net->wr[i].num_sge = 1;
		net->wr[i].sg_list = &net->sge[i];
		net->wr[i].next = NULL;
		net->wr[i].send_flags = IB_SEND_SIGNALED;
	}
	FOUT;
}

/* move QP to ready to receive */
static int qp_rtr(struct _net *net)
{
	struct ib_qp_attr qp_attr = {0};
	int attr_mask;
	int rv;

	FIN;
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IB_QPS_RTR;
	BUG_ON(ib_cm_init_qp_attr(net->cm_id, &qp_attr, &attr_mask));
	qp_attr.path_mtu = IB_MTU_512;
	qp_attr.max_dest_rd_atomic = 4;
	if ((rv = ib_modify_qp(net->qp, &qp_attr, attr_mask))) {
		_E("ib_modify_qp() returned %d\n", rv);
		BUG();
	}
	FOUT;
	return 0;
}

/* move QP to ready to send */
static int qp_rts(struct _net *net)
{
	struct ib_qp_attr qp_attr = {0};
	int attr_mask;
	int rv;

	FIN;
	qp_attr.qp_state = IB_QPS_RTS;
	BUG_ON(ib_cm_init_qp_attr(net->cm_id, &qp_attr, &attr_mask));
	qp_attr.max_rd_atomic = 4;
	if ((rv = ib_modify_qp(net->qp, &qp_attr, attr_mask))) {
		_E("ib_modify_qp() returned %d\n", rv);
		BUG();
	}
	FOUT;
	return 0;
}

/* the polling thread function when we test send without interrupts */
static int thread_func(void *arg)
{
	struct _net *net = arg;
	int i, m, n, rv;
	int is_send;
	struct ib_send_wr *bad_wr;
	struct u_iu *iu;
	int index;

	FIN;
poll_again:
	while ((rv = ib_poll_cq(net->scq, net->n_messages, net->s_wcs)) != 0) {
		if (rv < 0) {
			_E("rv=%d\n", rv);
			goto out;
		}
		else {
			_D("rv=%d\n", rv);
			n = rv;
			for (i = 0; i < n; ++i) {
				/* check if this is a send completion */
				is_send = net->s_wcs[i].wr_id & 1;
				m = (int)(net->s_wcs[i].wr_id >> 1);
				_D("wc.wr_id=%d\n", m);
				if (is_send) {
					if (net->s_wcs[i].status != IB_WC_SUCCESS) {
						_E("(net->s_wcs[%d].status=%d\n",
							i, net->s_wcs[i].status);
						goto out;
					}
				}
				else {
					/* we received a message from the client - get the buffer
					   depending if we are using SRQ
					*/
					iu = net->srq ?
						u_rtrv_recv(&net->ib_port->_dev->u_dev, m) :
						&net->rcv_bufs[m];
					/* sync for CPU access */
					ib_dma_sync_single_for_cpu(net->ib_port->_dev->u_dev.ib_dev,
						iu->dma, iu->size, DMA_FROM_DEVICE);
					/* get the index of the client send buffer */
					index = *(int *)iu->buf;
					_D("index=%d\n", index);
					/* post back the received buffer */
					if (net->srq)
						u_post_recv(&net->ib_port->_dev->u_dev, iu, NULL);
					else
						post_rcv(net, iu);
					/* send back the matching send buffer if index is positive.
					   if index is negative the test is done.
					*/
					if (index >= 0) {
						if ((rv = ib_post_send(
							net->qp, &net->wr[index], &bad_wr)) < 0) {
							_E("ib_post_send() returned %d\n", rv);
							goto out;
						}
					}
					else {
						_I("Test is over\n");
						goto out;
					}
				}
			}
		}
	}
	/* idle a bit */
	/*  cond_resched(); */
	rep_nop();
	goto poll_again;

out:
	FOUT;
	return 0;
}

/* new client login */
static void ib_port_new_connection(struct ib_port *ib_port,
	struct ib_cm_id *cm_id, struct ib_cm_req_event_param *param,
	void *private_data)
{
	struct _net *net;
	struct ib_qp_init_attr qp_init = {0};
	struct ib_qp_attr qp_attr = {0};
	struct ib_cm_rep_param rep_param = {0};
	struct c_login_request *clnt = private_data;
	static atomic_t a = ATOMIC_INIT(0);
	int i = atomic_inc_return(&a);

	
	FIN;
	if (!(net = kzalloc(sizeof(*net), GFP_KERNEL))) {
		_E("Fail to allocate server net\n");
		BUG_ON(true);
	}
	net->ib_port = ib_port;
	net->message_size = clnt->message_size;
	net->n_messages = clnt->n_messages;
	net->srq = clnt->srq;
	net->test_type = clnt->test_type;
	net->cm_id = cm_id;
	cm_id->context = net;
	_I("message_size=%d, n_messages=%d, srq=%d, test_type=%d\n",
		net->message_size, net->n_messages, net->srq, net->test_type);
	if (net->test_type != (int)tt_send_poll) {
		/* no polling - so we have separate send and
		   receive completion queues
		*/
		net->scq = ib_create_cq(ib_port->_dev->u_dev.ib_dev,
			s_completion_callback, NULL, net, max(net->n_messages, 1024), i % 12);
		BUG_ON(IS_ERR(net->scq));
		net->rcq = ib_create_cq(ib_port->_dev->u_dev.ib_dev,
			r_completion_callback, NULL, net,  max(net->n_messages, 1024), i % 12);
		_I("scq[%x]=%p rcq[%x]=%p\n", net->scq->cqe, net->scq, net->rcq->cqe, net->rcq);
		BUG_ON(IS_ERR(net->rcq));
	}
	if (net->test_type == (int)tt_send) {
		/* no polling so we must ask for notification on new completions */
		ib_req_notify_cq(net->scq, IB_CQ_NEXT_COMP);
		ib_req_notify_cq(net->rcq, IB_CQ_NEXT_COMP);
		_D("scq=%p rcq=%p\n", net->scq, net->rcq);
	}
	if (net->test_type == (int)tt_send_poll) {
		/* only send completion queue we share between the send and receive */
		net->scq = ib_create_cq(ib_port->_dev->u_dev.ib_dev,
			sr_completion_callback, NULL, net, 2 *  max(net->n_messages, 1024), i % 12);
		BUG_ON(IS_ERR(net->scq));
		_D("scq=%p rcq=%p\n", net->scq, net->rcq);
	}

	qp_init.qp_context = (void *)net;
	qp_init.event_handler = qp_event;
	if (net->test_type != (int)tt_send_poll) {
		/* separate completions for send and receive */
		qp_init.send_cq = net->scq;
		qp_init.recv_cq = net->rcq;
	}
	else {
		/* same completion for send and receive */
		qp_init.send_cq = net->scq;
		qp_init.recv_cq = net->scq;
	}
	_D("scq=%p rcq=%p\n", qp_init.send_cq, qp_init.recv_cq);
	/* init the QP params */
	qp_init.sq_sig_type = IB_SIGNAL_REQ_WR;
	qp_init.qp_type = IB_QPT_RC;
	qp_init.cap.max_send_wr = net->n_messages * 2;
	qp_init.cap.max_send_sge = 1;
	if (net->srq)
		qp_init.srq = ib_port->_dev->u_dev.srq.srq;
	else {
		qp_init.cap.max_recv_wr = net->n_messages * 2;
		qp_init.cap.max_recv_sge = 1;
	}

	net->qp = ib_create_qp(ib_port->_dev->u_dev.pd, &qp_init);
	BUG_ON(IS_ERR(net->qp));

	qp_attr.qp_state = IB_QPS_INIT;
	qp_attr.qp_access_flags =
		IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE;
	qp_attr.port_num = ib_port->port;
	qp_attr.pkey_index = 0;

	BUG_ON(ib_modify_qp(net->qp, &qp_attr,
		IB_QP_STATE | IB_QP_ACCESS_FLAGS | IB_QP_PORT | IB_QP_PKEY_INDEX));
	init_net_data(net);
	BUG_ON(!(net->page_ptr = ib_dma_alloc_coherent(ib_port->_dev->u_dev.ib_dev,
		PAGE_SIZE, &net->page, GFP_KERNEL)));
	/* move to READY_TO_RECEIVE */
	qp_rtr(net);
	/* we enabled RTR so we must receive buffer if we are NOT using SRQ */
	if (!net->srq)
		for (i = 0; i < net->n_messages; ++i) {
			_D("i=%d\n", i);
			post_rcv(net, &net->rcv_bufs[i]);
		}
	/* move to READY_TO_SEND */
	qp_rts(net);

	net->rsp.raddr = net->page;
	net->rsp.rkey = ib_port->_dev->u_dev.mr->rkey;
	_D("qpn=0x%x raddr=%#llx, rkey=%#x\n",
		net->qp->qp_num, net->rsp.raddr, ib_port->_dev->u_dev.mr->rkey);
	rep_param.qp_num = net->qp->qp_num;
	rep_param.private_data = (void *)&net->rsp;
	rep_param.private_data_len = sizeof(net->rsp);
	rep_param.rnr_retry_count = 20;
	rep_param.flow_control = 1;
	rep_param.failover_accepted = 0;
	rep_param.srq = net->srq;
	rep_param.responder_resources = 4;
	rep_param.initiator_depth = 4;
	BUG_ON(ib_send_cm_rep(cm_id, &rep_param));
	/* if we are polling start a polling thread */
	if (net->test_type == (int)tt_send_poll) {
		_D("Poll thread\n");
		if (IS_ERR(kthread_run(thread_func, net, "%s", "send_pt"))) {
			_E("Failed to create thread: send_pt\n");
			BUG();
		}
	}

	FOUT; 
}

/* new login request arrives */
static int cm_req_recv(struct ib_cm_id *cm_id,
	struct ib_cm_req_event_param *param, void *private_data)
{
	struct _dev *_dev = cm_id->context;
	struct ib_port *ib_port;
	int rv = 0;

	FIN;
	WARN_ON_ONCE(irqs_disabled());

	if (WARN_ON(!_dev || !private_data ||
		!(ib_port = find_ib_port(_dev, param->port)) || !ib_port->port_enabled)) {
		rv = -EINVAL;
		goto out;
	}
	ib_port_new_connection(ib_port, cm_id, param, private_data);
	
out:
	FOUT;
	return rv;
}

/* move QP to error state that will trigger the qp_event
   when the client disconnect
*/
static int qp_dreq(struct _net *net)
{
	struct ib_qp_attr qp_attr = {0};

	FIN;
	net->done = true;
	qp_attr.qp_state = IB_QPS_ERR;	
	BUG_ON(ib_modify_qp(net->qp, &qp_attr, IB_QP_STATE));
	ib_send_cm_drep(net->cm_id, NULL, 0);

	FOUT;
	return 0;
}

/* CM event handler */
static int cm_handler(struct ib_cm_id *cm_id, struct ib_cm_event *event)
{
	int rv = 0;

	FIN;
	switch (event->event) {
		case IB_CM_REQ_RECEIVED:
			rv = cm_req_recv(
				cm_id, &event->param.req_rcvd, event->private_data);
			break;
		case IB_CM_REJ_RECEIVED:
			_E("IB_CM_REJ_RECEIVED\n");
			break;
		case IB_CM_RTU_RECEIVED:
		case IB_CM_USER_ESTABLISHED:
			_I("IB_CM_USER_ESTABLISHED\n");
			//qp_rts();
			break;
	case IB_CM_DREQ_RECEIVED:
			/* disconnect arrives from the client */
			qp_dreq(cm_id->context);
			break;
		case IB_CM_DREP_RECEIVED:
			_E("IB_CM_DREP_RECEIVED\n");
			break;
		case IB_CM_TIMEWAIT_EXIT:
			_E("IB_CM_TIMEWAIT_EXIT\n");
			break;
		case IB_CM_REP_ERROR:
			_E("IB_CM_REP_ERROR\n");
			break;
		case IB_CM_DREQ_ERROR:
			_E("IB_CM_DREQ_ERROR\n");
			break;
		case IB_CM_MRA_RECEIVED:
			_E("IB_CM_MRA_RECEIVED\n");
			break;
		default:
			_E("received unrecognized IB CM event %d\n", event->event);
			break;
	}

	FOUT;
	return rv;
}

/* port event such as GID change and etc... */
static void ib_port_event_handler(
	struct ib_event_handler *handler, struct ib_event *event)
{
	BUG_ON(true);
}

/* called on every IB device port */
struct ib_port *ib_port_add(struct _dev *_dev, u8 port)
{
	struct ib_port *ib_port = NULL;

	FIN;
	ib_port = kzalloc(sizeof *ib_port, GFP_KERNEL);
	if (!ib_port)
		goto out;
	
	ib_port->_dev = _dev;
	ib_port->port = port;

	ib_port->port_enabled = true;

out:
	FOUT;
	return ib_port;
}

/* free the device - including the device SRQ */
static void freee(struct _dev *_dev)
{
	FIN;
	/* remove the srq */
	u_free_srq(&_dev->u_dev, NULL);
	if (_dev->u_dev.dev_attr)
		kfree(_dev->u_dev.dev_attr);
	if (_dev->u_dev.mr) {
		ib_dereg_mr(_dev->u_dev.mr);
		_dev->u_dev.mr = NULL;
	}
	if (_dev->u_dev.pd) {
		ib_dealloc_pd(_dev->u_dev.pd);
		_dev->u_dev.pd = NULL;
	}
	FOUT;
}

/* the number of ports the device has */
static int get_device_phys_port_count(struct ib_device *device)
{
    int port = 0;
    int ret;
    struct ib_port_attr attr;


    if (device->node_type == RDMA_NODE_IB_SWITCH)
        return 0;

    do {
        ++port;
        ret = ib_query_port(device, port, &attr);
    } while (ret == 0);

    return port - 1;
}

/* call on any IB device in the system - when the module is coming up */
static void add_one(struct ib_device *device)
{
	struct _dev *_dev = NULL;
	struct ib_port *ib_port;
	int s, e, p;

	FIN;
	if (!(_dev = init(device))) {
		_E("%s init failed.\n", device->name);
		FOUT;
		return;
	}

	INIT_LIST_HEAD(&_dev->u_dev.port_list);
	_dev->cm_id = ib_create_cm_id(device, cm_handler, _dev);
	BUG_ON(IS_ERR(_dev->cm_id));

	INIT_IB_EVENT_HANDLER(&_dev->event_handler, device, ib_port_event_handler);
	BUG_ON(ib_register_event_handler(&_dev->event_handler));

	if (device->node_type == RDMA_NODE_IB_SWITCH) {
		s = 0;
		e = 0;
	} else {
		s = 1;
		e = get_device_phys_port_count(device);
	}

	for (p = s; p <= e; ++p) {
		ib_port = ib_port_add(_dev, p);
		if (ib_port)
			list_add_tail(&ib_port->link, &_dev->u_dev.port_list);
		else {
			_E("%s nvmeibs_ib_port_add() failed.\n", device->name);
			BUG_ON(true);
		}
	}

	list_add_tail(&_dev->u_dev.link, &dev_list);

	if (ib_cm_listen(_dev->cm_id, cpu_to_be64(service_guid), 0, NULL)) {
		_E("%s ib_cm_listen() failed.\n", device->name);
		goto remove_dev;
	} else
		_I("%s listening for clients on service id %llu\n",
		    device->name, service_guid);

	ib_set_client_data(device, &client, _dev);
	FOUT;
	return;

remove_dev:
	list_del_init(&_dev->u_dev.link);

	freee(_dev);
	kfree(_dev);
	_dev = NULL;
	_I("%s failed.\n", device->name);
}

/* called when a device is removed - when the module is going down */
static void remove_one(struct ib_device *device)
{
	struct _dev *_dev;

	FIN;
	_dev = ib_get_client_data(device, &client);
	if (!_dev) {
		_I("%s: nothing to do.\n", device->name);
		FOUT;
		return;
	}

	/* first this one so no more queuing work on the port */
	ib_unregister_event_handler(&_dev->event_handler);
	ib_destroy_cm_id(_dev->cm_id);

	list_del(&_dev->u_dev.link);

	/* free the device */
	freee(_dev);
	kfree(_dev);
	FOUT;
}

int __init srvib_init(void) /* Constructor */
{
	int ret = 0;

	FIN;
	if ((ret = ib_register_client(&client))) {
		_E("couldn't register IB client\n");
	}
	_I("Hooray: srvib registered\n");
	FOUT;
	return ret;
}

void __exit srvib_exit(void) /* Destructor */
{
	ib_unregister_client(&client);
    _I("Hooray: srvib unregistered\n");
	FOUT;
}

module_init(srvib_init);
module_exit(srvib_exit);

