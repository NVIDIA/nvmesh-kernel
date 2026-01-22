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
#include <linux/parser.h>
#include <linux/dma-direction.h>
#include <linux/err.h>
#include <linux/random.h>
#include <linux/bug.h>

#include <linux/delay.h>
#include "u_ib_incs.h"

#define DEBUG_TEST 0

#include "u.h"
#include "u.c"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("IB Client Test Driver");
MODULE_LICENSE("GPL and additional rights");

static LIST_HEAD(dev_list);

struct _dev {
	struct u_dev u_dev;
};

struct ib_port {
	/* owner device */
	struct _dev *_dev;
	/* port stuff */
	u8 port;
	union ib_gid gid;
	struct device dev;
	struct completion released;
	struct list_head link;
};

struct _net {
	struct ib_cm_id *cm_id;
	struct ib_qp *qp;
	struct ib_cq *scq;
	struct ib_cq *rcq;
};

struct msg_info {
	atomic_t n;
};

/* created per test request */
struct _info {
	/* the port where the test starts */
	struct ib_port *ib_port;
	/* the network stuff */
	struct _net net;
	/* the service ID the server is listening */
	__be64 service_id;
	/* access route to server */
	union ib_gid orig_dgid;
	struct ib_sa_path_rec path;
	struct ib_sa_query *path_query;
	int path_query_id;
	int status;
	/* completion struct for the test state */
	struct completion path_lookup;
	struct completion login_done;
	struct completion test_done;
	struct completion disconnect_done;
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
	struct ib_wc *r_wcs;
	/* receive completions */
	struct msg_info *msgs;
	/* test states */
	bool test_ok;
	bool test_complete;
	bool connected;
	bool disconnect_called;
	bool disconnect_completed;
	bool disconnected;
	bool srq;
	struct u_tests tests;
	volatile bool done;
};

/* the client module create a device entry in the SysFS -
   here is where we take the device out
*/
static void release_dev(struct device *dev)
{
	struct ib_port *ib_port = container_of(dev, struct ib_port, dev);

	FIN;
	complete(&ib_port->released);
	FOUT;
}

static struct class clt_class = {
	.name    = "clt_infiniband_port",
	.dev_release = release_dev
};

static void add_one(struct ib_device *device);
static void remove_one(struct ib_device *device);
static struct ib_client client = {
	.name   = "cltib",
	.add    = add_one,
	.remove = remove_one
};

static struct ib_sa_client sa_cli;

/* initializing new IB device wrapper */
static struct _dev *init(struct ib_device *device)
{
	struct _dev *_dev = NULL;
	int srq_size;
	int rv = 0;

	FIN;
	_dev = kzalloc(sizeof *_dev, GFP_KERNEL);
	if (!_dev)
		goto out;

	_dev->u_dev.ib_dev = device;

	_dev->u_dev.dev_attr =
		kzalloc(sizeof(*_dev->u_dev.dev_attr), GFP_KERNEL);
	if (!_dev->u_dev.dev_attr) 
		goto free_dev;

	if ((rv = ib_query_device(_dev->u_dev.ib_dev, _dev->u_dev.dev_attr))) {
		_E("Query device failed (%d)\n", rv);
		goto free_attr;
	}

	_dev->u_dev.pd = ib_alloc_pd(device);
	if (IS_ERR(_dev->u_dev.pd)) {
		_E("ib_alloc_pd failed (%ld)\n", PTR_ERR(_dev->u_dev.pd));
		goto free_attr;
	}

	_dev->u_dev.mr = ib_get_dma_mr(_dev->u_dev.pd,
							IB_ACCESS_LOCAL_WRITE |
							IB_ACCESS_REMOTE_READ |
							IB_ACCESS_REMOTE_WRITE);
	if (IS_ERR(_dev->u_dev.mr)) {
		_E("ib_get_dma_mr failed (%ld)\n", PTR_ERR(_dev->u_dev.mr));
		goto err_pd;
	}
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
	_dev = NULL;

out:
	FOUT;
	return _dev;
}

#define GUID_SIZE sizeof("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
static void format_gid_raw(u8 raw[16], char *buf)
{
	int i, n;

	for (n = 0, i = 0; i < 8; ++i) {
		n += sprintf(buf + n, "%04x", be16_to_cpu(((__be16 *)raw)[i]));
		if (i < 7)
			buf[n++] = ':';
	}
}

static int __ib_query_gid(struct ib_device *device, u8 port_num, int index,
	union ib_gid *gid)
{
	return ib_query_gid(device, port_num, index, gid, NULL);
}

static int __ib_sa_path_rec_get(struct ib_sa_client *client,
	struct ib_device *device, u8 port_num,
	struct ib_sa_path_rec *rec,
	ib_sa_comp_mask comp_mask,
	int timeout_ms, gfp_t gfp_mask,
	void (*callback)(int status, struct ib_sa_path_rec *resp, void *context),
	void *context,
	struct ib_sa_query **query)
{
	return ib_sa_path_rec_get(client, device, port_num, rec, comp_mask,
		timeout_ms, 5, gfp_mask, callback, context, query);
}

/*
Test ports are added by writing into sysfs start_test attribute:                       
	see run.sh for the details
*/

enum {
	NVMEIBC_OPT_ERR			= 0,
	NVMEIBC_OPT_TEST_HOST	= 1 << 1,
	NVMEIBC_OPT_TEST	  	= 1 << 2,
};

static const match_table_t opt_tokens = {
	{ NVMEIBC_OPT_TEST_HOST,	"host=%s"			},
	{ NVMEIBC_OPT_TEST,			"test=%s"			},
	{ NVMEIBC_OPT_ERR,			NULL 				}
};

/* parse the test options */
static int parse_options(const char *buf, struct _info *info)
{
	char *options, *sep_opt;
	char *p;
	char *q;
	char *r;
	char gid[3];
	substring_t args[MAX_OPT_ARGS];
	substring_t hex;
	int token;
	int ret = -EINVAL;
	int i;
	char *strs[64];
	union ib_gid host_gid;
	int t;

	FIN;
	options = kstrdup(buf, GFP_KERNEL);
	if (!options) return -ENOMEM;

	sep_opt = options;
	while ((p = strsep(&sep_opt, ",")) != NULL) {
		if (!*p) continue;

		token = match_token(p, opt_tokens, args);

		switch (token) {
		case NVMEIBC_OPT_TEST_HOST:
			i = 0;
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			q = p;
			while ((r = strsep(&p, "|")) != NULL) {
				if (!*r) continue;
				strs[i++] = r;
			}
			p = q;
			if (i < 3) {
				_E("test host creation request is missing parameter\n");
				kfree(p);
				break;
			}
			if (strlen(strs[0]) != 32) {
				_E("bad TEST HOST GID parameter '%s'\n", strs[0]);
				kfree(p);
				goto out;
			}
			hex.from = strs[1];
			hex.to = hex.from + strlen(strs[1]);
			if (match_hex(&hex, &token)) {
				_E("bad P_Key parameter '%s'\n", strs[1]);
				kfree(p);
				goto out;
			}
			for (i = 0; i < 16; ++i) {
				strlcpy(gid, strs[0] + i * 2, 3);
				host_gid.raw[i] = simple_strtoul(gid, NULL, 16);
			}
			memcpy(info->path.dgid.raw, host_gid.raw, 16);
			memcpy(info->orig_dgid.raw, host_gid.raw, 16);
			info->path.pkey = cpu_to_be16(token);
			info->service_id =
				cpu_to_be64(SERVICE_GUID/*simple_strtoull(strs[2], NULL, 16)*/);
			info->path.service_id = info->service_id;
			_E("test host %s is ok\n", strs[0]);
			_E("test pkey %#x is ok\n", token);
			_E("test service_id %llu is ok\n", info->path.service_id);
			kfree(p);
			break;

		case NVMEIBC_OPT_TEST:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			q = p;
			i = 0;
			while ((r = strsep(&p, "|")) != NULL) {
				if (!*r) continue;
				strs[i++] = r;
			}
			p = q;
			if (i < 4) {
				_E("test request is missing parameter\n");
				kfree(p);
				goto out;
			}
			t = (u32)simple_strtoul(strs[0], NULL, 10);
			info->tests.type = t == 1 ? tt_rdma_write :
				(t == 2 ? tt_send : tt_send_poll);
			/* one shot for message size or size of send_q */
			if (!(info->tests.message_size || info->tests.n_messages)) {
				info->tests.message_size =
					(u32)simple_strtoul(strs[1], NULL, 10);
				info->tests.n_messages = (u32)simple_strtoul(strs[2], NULL, 10);
				_I("test: message_size=%d, n_messages=%d\n",
					info->tests.message_size, info->tests.n_messages);
				kfree(p);
				if (info->tests.message_size > PAGE_SIZE) {
					_E("message size for client is limited to %ld\n",
						PAGE_SIZE);
					info->tests.message_size = PAGE_SIZE;
				}
				info->tests.srq = (u32)simple_strtoul(strs[3], NULL, 10);
			}
			if (info->tests.type == tt_rdma_write) {
				if (i != 5) {
					_E("test rdma_write is missing parameter\n");
					kfree(p);
					goto out;
				}
				info->tests.rwt.seconds =
					(u32)simple_strtoul(strs[4], NULL, 10);
				_I("rdma_write test: srq=%c, message_size=%d, n_messages=%d, "
				   "seconds=%d\n", info->tests.srq ? 'Y' : 'N',
					info->tests.message_size, info->tests.n_messages,
					info->tests.rwt.seconds);
			}
			else if (info->tests.type == tt_send ||
					 info->tests.type == tt_send_poll) {
				if (i != 5) {
					_E("test rdma_write is missing parameter\n");
					kfree(p);
					goto out;
				}
				info->tests.st.messages = simple_strtoull(strs[4], NULL, 10);
				if (info->tests.st.messages < info->tests.n_messages)
					info->tests.st.messages = info->tests.n_messages;
				_I("send_iter test: srq=%c, message_size=%d, n_messages=%d, "
				   "messages=%llu\n", info->tests.srq ? 'Y' : 'N',
					info->tests.message_size, info->tests.n_messages,
					info->tests.st.messages);
			}
			break;

		default:
			_E("unknown parameter or missing value '%s' in "
			   "target creation request\n", p);
			goto out;
		}
	}
	ret = 0;

out:
	kfree(options);
	FOUT;
	return ret;
}

/* client initiates disconnect - usually at the end of a test */
static void disconnect(struct _info *info, bool send_dreq)
{
	struct ib_qp_attr qp_attr = {0};
	struct _net *net = &info->net;
	int rv;

	FIN;
	if (!info->connected)
		complete(&info->login_done);
	init_completion(&info->disconnect_done);
	info->disconnect_called = true;
	qp_attr.qp_state = IB_QPS_ERR;
	BUG_ON(ib_modify_qp(net->qp, &qp_attr, IB_QP_STATE));
	
	if (send_dreq) {
		if ((rv = ib_send_cm_dreq(net->cm_id, NULL, 0)) < 0)
			_E("Sending CM DREQ failed (%d)\n", rv);
	} else {
		if ((rv = ib_send_cm_drep(net->cm_id, NULL, 0)) < 0)
			_E("Sending CM DREP failed (%d)\n", rv);
	}

	FOUT;
}

/* in the case we are NOT using SRQ - return the received buffer */
static void post_rcv(struct _info *info, struct u_iu *iu)
{	
	struct ib_recv_wr wr = {0};
	struct ib_recv_wr *bad_wr;
	struct ib_sge list;

	//FIN;
	list.addr   = iu->dma;
	list.length = iu->size;
	list.lkey   = info->ib_port->_dev->u_dev.mr->lkey;

	wr.next     = NULL;
	wr.wr_id    = iu->index << 1;
	wr.sg_list  = &list;
	wr.num_sge  = 1;
	BUG_ON(ib_post_recv(info->net.qp, &wr, &bad_wr));
	//FOUT;
}

/* handle send completion */
static int handle_swc(struct _info *info, struct ib_wc *wc)
{
	struct msg_info *mi;
	struct ib_send_wr *bad_wr;
	int m, rv = 0;

	FIN;
	m = (int)(wc->wr_id >> 1);
	_D("wc.wr_id=%d\n", m);
	mi = &info->msgs[m];
	++info->tests.st.sc_messages;
	_D("info->tests.st.sc_messages=%llu\n", info->tests.st.sc_messages);
	/* every message that we send has a counter that in initiated with 2.
	   after either send or receive completion the counter is reduce.  the first
	   that hits 0 send again the message unless we reach the number of messages
	   the test was design to.
	*/
	if (atomic_dec_return(&mi->n) == 0) {
		/* we hit 0 so the send completion decides if we need more sends */
		if (info->tests.st.sc_messages == info->tests.st.messages) {
			/* no more messages - test is over */
			/* test is over */
			info->tests.st.end_time = ktime_get().tv64;
			info->test_ok = true;
			info->test_complete = true;
			_I("info=%p Test is done\n", info);
			/* wakeup */
			complete(&info->test_done);
		}
		else if (atomic64_inc_return(&info->tests.st.s_messages) <=
				 info->tests.st.messages) {
			/* we checked if we need to send another message or we have
			   already sent the test number of messages
			*/
			_D("Test is still on\n");
			_D("Going to send index %d\n", m);
			atomic_set(&mi->n, 2);
			if ((rv = ib_post_send(
				info->net.qp, &info->wr[m], &bad_wr)) < 0) {
				_E("ib_post_send() returned %d\n", rv);
				info->test_ok = false;
				/* wakeup */
				complete(&info->test_done);
				rv = -1;
			}
		}
	}
	FOUT;
	return rv;
}

/* send completion handler - we are in interrupt context */
static void s_completion_callback(struct ib_cq *cq, void *ctx)
{
	struct _info *info = ctx;
	int i, rv;

	FIN;
	if (info->done)
		goto out;
	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	while ((rv = ib_poll_cq(cq, info->tests.n_messages, info->s_wcs)) != 0) {
		_D("rv=%d\n", rv);
		/* chek if error and if in error we are out */
		if (rv < 0) {
			_E("rv=%d\n", rv);
			info->test_ok = false;
			/* wakeup */
			complete(&info->test_done);
			goto out;
		}
		else for (i = 0; i < rv; ++i)
			if (info->s_wcs[i].status != IB_WC_SUCCESS ||
				handle_swc(info, &info->s_wcs[i]) < 0) {
				/* we failed where either the completion was bad or we could
				   not send message - we just finish the test 
				*/
				_E("wc.status=%d\n", (int)info->s_wcs[i].status);
				info->test_ok = false;
				/* wakeup */
				complete(&info->test_done);
				goto out;
			}
	}

out:
	FOUT;
}

/* handle receive completion */
static int handle_rwc(struct _info *info, struct ib_wc *wc)
{
	struct msg_info *mi;
	struct ib_send_wr *bad_wr;
	struct u_iu *iu;
	int m, index;
	int rv = 0;

	FIN;
	/* get the receive buffer index - shift one since the first bit
	   marks if it is a send or a receive buffre
	*/
	m = (int)(wc->wr_id >> 1);
	_D("wc.wr_id=%d\n", (int)m);
	/* get the buffer depending if we use SRQ */
	iu = info->srq ?
		u_rtrv_recv(&info->ib_port->_dev->u_dev, m) :
		&info->rcv_bufs[m];
	/* sync for CPU access */
	ib_dma_sync_single_for_cpu(
		info->ib_port->_dev->u_dev.ib_dev, iu->dma,
		iu->size, DMA_FROM_DEVICE);
	/* read the arrived index - this will tell for which message
	   the server replied
	*/
	index = *(int *)iu->buf;
	_D("index=%d\n", index);
	/* once we know the index we may post back
	   the received buffer
	*/
	if (info->srq)
		u_post_recv(&info->ib_port->_dev->u_dev, iu, NULL);
	else
		post_rcv(info, iu);
	mi = &info->msgs[index];
	++info->tests.st.rc_messages;
	_D("info->tests.st.rc_messages=%llu\n", info->tests.st.rc_messages);
	/* see the comment in handle_swc() */
	if (atomic_dec_return(&mi->n) == 0) {
		if (info->tests.st.rc_messages >= info->tests.st.messages) {
			_I("info=%p Test is done\n", info);
			/* test is over */
			info->tests.st.end_time = ktime_get().tv64;
			info->test_ok = true;
			/* wakeup */
			complete(&info->test_done);
		}
		else if (atomic64_inc_return(&info->tests.st.s_messages) <=
				 info->tests.st.messages) {
			_D("Going to post send %d\n", index);
			atomic_set(&mi->n, 2);
			if ((rv = ib_post_send(
				info->net.qp, &info->wr[index], &bad_wr)) < 0) {
				_E("ib_post_send() returned %d\n", rv);
				info->test_ok = false;
				/* wakeup */
				complete(&info->test_done);
				rv = -1;
			}
		}
	}
	FOUT;
	return rv;
}

/* receive completion handler - we are in interrupt context */
static void r_completion_callback(struct ib_cq *cq, void *ctx)
{
	struct _info *info = ctx;
	int i, rv;

	FIN;

	if (info->disconnect_called && info->disconnect_completed) {
		goto out; // YR: Why does this happen?
	}

	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	while ((rv = ib_poll_cq(cq, info->tests.n_messages, info->r_wcs)) != 0) {
		_D("rv=%d\n", rv);
		/* chek if error and if in error we are out */
		if (rv < 0) {
			_E("rv=%d\n", rv);
			info->test_ok = false;
			/* wakeup */
			complete(&info->test_done);
			goto out;
		}
		else for (i = 0; i < rv; ++i) {
			if (info->r_wcs[i].status != IB_WC_SUCCESS ||
				handle_rwc(info, &info->r_wcs[i]) < 0) {
				/* we failed where either the completion was bad or we could
				   not send message.
				   bad completion may mark that the test was ended and
				   we are taking down the connection
				*/
				_E("wc.status=%d\n", (int)info->r_wcs[i].status);
				info->test_ok = false;
				/* wakeup */
				complete(&info->test_done);
				if (info->disconnect_called) {
					info->disconnect_completed = true;
					complete(&info->disconnect_done);
				}
				goto out;
			}
		}
	}

out:
	FOUT;
}

/* send/receive completion handler in polling mode -
   if we get it we probably have a bug
*/
static void sr_completion_callback(struct ib_cq *cq, void *ctx)
{
	BUG();
}

/* free the test info allocations */
static void free_info(struct _info *info)
{
	struct _net *net = &info->net;
	struct u_iu *iu;
	int i;

	FIN;
	for (i = 0; i < info->tests.n_messages; ++i) {
		iu = &info->snd_bufs[i];
		if (iu->buf) {
			ib_dma_free_coherent(
				net->qp->device, iu->size, iu->buf, iu->dma);
			iu->buf = NULL;
		}
		iu = &info->rcv_bufs[i];
		if (iu->buf) {
			ib_dma_free_coherent(
				net->qp->device, iu->size, iu->buf, iu->dma);
			iu->buf = NULL;
		}
	}
	kfree(info->snd_bufs);
	info->snd_bufs = NULL;
	kfree(info->rcv_bufs);
	info->rcv_bufs = NULL;
	kfree(info->msgs);
	info->msgs = NULL;
	kfree(info->wr);
	info->wr = NULL;
	kfree(info->sge);
	info->sge = NULL;
	kfree(info->s_wcs);
	info->s_wcs = NULL;
	kfree(info->r_wcs);
	info->r_wcs = NULL;
	FOUT;
}

/* take the test connectio down */
static void destroy_conn(struct _info *info)
{
	struct _net *net = &info->net;

	FIN;
	free_info(info);
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
	FOUT;
}

/* called in an interrupt context when we moved the QP into error state */
static void qp_event(struct ib_event *event, void *arg)
{
	struct _info *info = arg;

	FIN;
	_W("QP event %d\n", event->event);
	if (event->event == IB_EVENT_QP_LAST_WQE_REACHED) {
		if (info->disconnect_called) {
			info->disconnected = true;
			complete(&info->disconnect_done);
		}
	}
	FOUT;
}

/* when the server acks the connection we moved the
   QP into RTR and RTS state
*/
static void cm_rep_handler(struct ib_cm_id *cm_id,
	struct s_login_response *lrsp, struct _info *info)
{
	struct ib_qp_attr qp_attr = {0};
	int attr_mask = 0;

	int i;

	FIN;
	qp_attr.qp_state = IB_QPS_RTR;
	BUG_ON(ib_cm_init_qp_attr(cm_id, &qp_attr, &attr_mask));
	qp_attr.path_mtu = IB_MTU_512;
	qp_attr.max_dest_rd_atomic = 4;
	BUG_ON(ib_modify_qp(info->net.qp, &qp_attr, attr_mask));

	/* if it is not SRQ we must post receive buffers after w emoved into RTR */	
	if (!info->srq)
		for (i = 0; i < info->tests.n_messages; ++i)
			post_rcv(info, &info->rcv_bufs[i]);

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IB_QPS_RTS;
	BUG_ON(ib_cm_init_qp_attr(cm_id, &qp_attr, &attr_mask));
	BUG_ON(ib_modify_qp(info->net.qp, &qp_attr, attr_mask));
	
	_D("Received CM Response from server with RKey: %08x, "
	   "Vaddr: %016llx. Sending RTU\n", lrsp->rkey, lrsp->raddr);
	
	BUG_ON(ib_send_cm_rtu(cm_id, NULL, 0));
	info->tests.rwt.raddr = lrsp->raddr;
	info->tests.rwt.rkey = lrsp->rkey;
	info->status = 0;
	FOUT;
}

/* handle a server rejected connection request */
static void cm_rej_handler(struct ib_cm_id *cm_id,
	struct ib_cm_event *event, struct _info *info)
{
	FIN;
	switch (event->param.rej_rcvd.reason) {
	case IB_CM_REJ_PORT_CM_REDIRECT:
		_W("REJ reason: IB_CM_REJ_PORT_CM_REDIRECT\n");
		info->status = -1;
		break;
	case IB_CM_REJ_PORT_REDIRECT:
		_W("REJ reason: IB_CM_REJ_PORT_REDIRECT\n");
		info->status = -ECONNRESET;
		break;
	case IB_CM_REJ_DUPLICATE_LOCAL_COMM_ID:
		_W("REJ reason: IB_CM_REJ_DUPLICATE_LOCAL_COMM_ID\n");
		info->status = -ECONNRESET;
		break;
	case IB_CM_REJ_CONSUMER_DEFINED:
		_W("REJ reason: IB_CM_REJ_CONSUMER_DEFINED\n");
		info->status = -ECONNRESET;
		break;
	case IB_CM_REJ_STALE_CONN:
		_E("REJ reason: stale connection\n");
		info->status = -1;
		break;
	case IB_CM_REJ_INVALID_SERVICE_ID:
		_E("REJ reason: invalid service ID\n");
		info->status = ECONNRESET;
		break;
	default:
		_E("REJ reason 0x%x\n", event->param.rej_rcvd.reason);
		info->status = -ECONNRESET;
		break;
	}
	FOUT;
}

/* handle CM events */
static int cm_handler(struct ib_cm_id *cm_id, struct ib_cm_event *event)
{
	struct _info *info = cm_id->context;
	int comp = 0;

	FIN;
	switch (event->event) {
	case IB_CM_REQ_ERROR: /* oops bad connection request */
		_E("Sending CM REQ failed\n");
		comp = 1;
		info->status = -ECONNRESET;
		break;
	case IB_CM_REP_RECEIVED: /* reply on connection request */
		_I("Connection accepted\n");
		_D("Remote QPN: %04x, QKey: %06x, Starting PSN: %08x\n", 
		    event->param.rep_rcvd.remote_qpn,
			event->param.rep_rcvd.remote_qkey,
			event->param.rep_rcvd.starting_psn);
		comp = 1;
		cm_rep_handler(cm_id, event->private_data, info);
		break;
	case IB_CM_REJ_RECEIVED: /* reject connection arrived */
		_E("REJ received: cm_id=%p, net=%p\n", cm_id, info);
		comp = 1;
		cm_rej_handler(cm_id, event, info);
		break;
	case IB_CM_DREQ_RECEIVED: /* disconnection from server arrived */
		_E("DREQ received - connection closed\n");
		disconnect(info, false);
		break;
	case IB_CM_TIMEWAIT_EXIT:
		_E("connection closed\n");
		comp = 1;
		info->status = 0;
		break;
	case IB_CM_MRA_RECEIVED:
	case IB_CM_DREQ_ERROR:
	case IB_CM_DREP_RECEIVED:
		break;
	default:
		_W("Unhandled CM event %d\n", event->event);
		break;
	}

	if (comp)
		complete(&info->login_done);
	FOUT;
	return 0;
}

/* server path lookup callback */
static void path_rec_completion(
	int status, struct ib_sa_path_rec *pathrec, void *net_ptr)
{
	struct _info *info = net_ptr;

	FIN;
	info->status = status;
	if (status)
		_E("Got failed path rec status %d\n", status);
	else
		info->path = *pathrec;
	complete(&info->path_lookup);
	FOUT;
}
/* client tries to find a path to the server */
static int lookup_path(struct ib_port *ib_port, struct _info *info)
{
	char sgid_buf[GUID_SIZE];
	char dgid_buf[GUID_SIZE];
	int rv = 0;

	FIN;
	BUG_ON(__ib_query_gid(
		ib_port->_dev->u_dev.ib_dev, ib_port->port, 0, &info->path.sgid));
	format_gid_raw(info->path.sgid.raw, sgid_buf);
	format_gid_raw(info->path.dgid.raw, dgid_buf);
	_D("s=%s, d=%s\n", sgid_buf, dgid_buf);
	info->path.numb_path = 1;
	init_completion(&info->path_lookup);
	info->path_query_id = __ib_sa_path_rec_get(&sa_cli,
		ib_port->_dev->u_dev.ib_dev, ib_port->port, &info->path, 
		IB_SA_PATH_REC_SERVICE_ID | IB_SA_PATH_REC_DGID | IB_SA_PATH_REC_SGID |
		IB_SA_PATH_REC_NUMB_PATH | IB_SA_PATH_REC_PKEY,
		5000, GFP_KERNEL, path_rec_completion, info, &info->path_query);
	if (info->path_query_id < 0) {
		_E("ib_sa_path_rec_get failed (%d)\n", info->path_query_id);
		rv = -1;
		goto out;
	}

	BUG_ON(wait_for_completion_interruptible(&info->path_lookup) < 0);
	if (info->status < 0) {
		_E("Error %d returned from ib_sa_path_rec_get for %pI6 -> %pI6\n",
		    info->status, &info->path.sgid.raw, &info->path.dgid.raw);
	}
	rv = info->status;
	
out:
	FOUT;
	return rv;
}

/* after a successful server path lookup, client sends
   a login request to the server
*/
static int send_login_req(struct _info *info)
{
	struct {
		struct ib_cm_req_param param;
		struct c_login_request priv;
	} req;

	FIN;
	memset(&req, 0, sizeof(req));
	req.param.primary_path = &info->path;
	req.param.alternate_path = NULL;
	req.param.service_id = info->service_id;
	req.param.qp_num = info->net.qp->qp_num;
	req.param.qp_type = info->net.qp->qp_type;
	req.param.private_data = &req.priv;
	req.param.private_data_len = sizeof(req.priv);
	req.param.flow_control = 1;

	get_random_bytes(&req.param.starting_psn, 4);
	req.param.starting_psn &= 0xffffff;

	/*
	 * Pick some arbitrary defaults here; we could make these
	 * module parameters if anyone cared about setting them.
	 */
	req.param.responder_resources = 4;
	req.param.remote_cm_response_timeout = 20;
	req.param.local_cm_response_timeout = 20;
	req.param.retry_count = 20;
	req.param.rnr_retry_count = 20;
	req.param.max_cm_retries = 15;
	req.param.srq = info->tests.srq;

	/* the test params as the private data */
	req.priv.message_size = info->tests.message_size;
	req.priv.n_messages = info->tests.n_messages;
	req.priv.srq = info->tests.srq;
	req.priv.test_type = info->tests.type;

	/* try to connect */
	BUG_ON(ib_send_cm_req(info->net.cm_id, &req.param));
	FOUT;
	return 0;
}

/* RDMA write test - write and wait for the send completion */
static void run_rdma_write_test(struct _info *info)
{
	u64 interval_time;
	u64 end_time;
	u64 cur_time;
	struct ib_wc wc = {0};
	int i;
	struct ib_send_wr *bad_wr;
	struct rdma_write_test *t = &info->tests.rwt;
	int rv = 0;

	FIN;
	_D("Allocating WQE, raddr=%#llx, rkey=%#x\n", t->raddr, t->rkey);
	_D("n_messages=%d, message_len=%d\n",
		info->tests.n_messages, info->tests.message_size);
	for (i = 0; i < info->tests.n_messages; ++i) {
		info->wr[i].opcode = IB_WR_RDMA_WRITE;
		info->wr[i].wr_id = (i << 8) | 1;
		info->wr[i].wr.rdma.remote_addr = t->raddr;
		info->wr[i].wr.rdma.rkey = t->rkey;
		info->wr[i].num_sge = 1;
		info->wr[i].sg_list = &info->sge[i];
		info->wr[i].next = &info->wr[i + 1];
	}
	info->wr[i - 1].next = NULL;
	info->wr[i - 1].send_flags = IB_SEND_SIGNALED;

	_I("-----> Test starts\n");
	t->messages = 0;
	interval_time = (u64)t->seconds * 1000000000ULL;
	end_time = ktime_get().tv64 + interval_time;
	while ((cur_time = ktime_get().tv64) < end_time) {
		while ((rv = ib_poll_cq(info->net.scq, 1, &wc)) != 0) {
			rep_nop();
		}
		if ((rv = ib_post_send(info->net.qp, info->wr, &bad_wr)) < 0) {
			_E("ib_post_send() returned %d\n", rv);
			break;
		}
		else {
			while ((rv = ib_poll_cq(info->net.scq, 1, &wc)) == 0) {
				rep_nop();
			}
			if (rv <= 0 || wc.status != IB_WC_SUCCESS) {
				_E("rv=%d, wc.status=%d\n", rv, (int)wc.status);
				break;
			}
			else
				rv = 0;
		}
		t->messages += info->tests.n_messages;
	}
	_I("-----> Test is over\n");
	if (!rv) {
		_I("messages = %lld, mps=%lld\n",
			t->messages, DIV_ROUND_CLOSEST(t->messages * 1000000000ULL,
				cur_time - end_time + interval_time));
	}
	else {
		_E("test ended with error %d\n", rv);
	}
	FOUT;
}

/* send with interrupt test - send at most info->tests.st.messages
   and messure the time at it takes
*/
static void run_send_test(struct _info *info)
{
	int i;
	struct ib_send_wr *bad_wr;
	struct send_test *s = &info->tests.st;
	int rv = 0;

	FIN;
	_D("n_messages=%d, message_len=%d\n",
		info->tests.n_messages, info->tests.message_size);
	ib_req_notify_cq(info->net.scq, IB_CQ_NEXT_COMP);
	ib_req_notify_cq(info->net.rcq, IB_CQ_NEXT_COMP);
	for (i = 0; i < info->tests.n_messages; ++i) {
		info->wr[i].opcode = IB_WR_SEND;
		info->wr[i].wr_id = (i << 1) | 1;
		info->wr[i].num_sge = 1;
		info->wr[i].sg_list = &info->sge[i];
		info->wr[i].next = NULL;
		info->wr[i].send_flags = IB_SEND_SIGNALED;
	}

	_I("-----> Test starts\n");
	info->test_ok = false;
	init_completion(&info->test_done);
	atomic64_set(&info->tests.st.s_messages, 0);
	s->start_time = ktime_get().tv64;
	for (i = 0; i < info->tests.n_messages; ++i) {
		if (atomic64_inc_return(&s->s_messages) <= s->messages) {
			_D("--> i=%d\n", i);
			atomic_set(&info->msgs[i].n, 2);
			if ((rv = ib_post_send(info->net.qp, &info->wr[i], &bad_wr)) < 0) {
				_E("ib_post_send() returned %d\n", rv);
				break;
			}
		}
		else
			break;
	}
	if (rv)
		_E("test ended with error %d\n", rv);
	else {
		_I("-----> Waiting for test done\n");
		BUG_ON(wait_for_completion_interruptible(&info->test_done) < 0);
		_I("-----> Test is over\n");
		_I("messages = %lld, mps=%lld\n",
			s->messages, DIV_ROUND_CLOSEST(s->messages * 1000000000ULL,
				s->end_time - s->start_time));
	}
	FOUT;
}

/* send with poll test - send and poll the completion queue.  when the test is
   over send the last message to tell the server that the test is over
*/
static void poll_q(struct _info *info)
{	
	int i, m, n, rv;
	int is_send;

	FIN;
poll_again:
	while ((rv = ib_poll_cq(
		info->net.scq, info->tests.n_messages, info->s_wcs)) != 0) {
		if (rv < 0) {
			_E("rv=%d\n", rv);
			goto out;
		}
		else {
			_D("rv=%d\n", rv);
			n = rv;
			for (i = 0; i < n; ++i) {
				is_send = info->s_wcs[i].wr_id & 1;
				m = (int)(info->s_wcs[i].wr_id >> 1);
				_D("wc.wr_id=%d\n", m);
				if (is_send) {
					if (info->s_wcs[i].status != IB_WC_SUCCESS ||
						handle_swc(info, &info->s_wcs[i]) < 0) {
						_E("wc.status=%d\n", (int)info->s_wcs[i].status);
						goto out;
					}
				}
				else {
					if (info->s_wcs[i].status != IB_WC_SUCCESS ||
						handle_rwc(info, &info->s_wcs[i]) < 0) {
						_E("wc.status=%d\n", (int)info->s_wcs[i].status);
						goto out;
					}
				}
			}
			if (info->test_ok) {
				/* stop the other side thread */
				*(int *)info->snd_bufs[0].buf = -1;
				ib_post_send(info->net.qp, &info->wr[0], NULL);
				goto out;
			}
		}
	}
	/*  cond_resched(); */
	rep_nop();
	goto poll_again;

out:
	FOUT;
}

/* send with poll test - send at most info->tests.st.messages
   and messure the time at it takes
*/
static void run_send_poll_test(struct _info *info)
{
	int i;
	struct ib_send_wr *bad_wr;
	struct send_test *s = &info->tests.st;
	int rv = 0;

	FIN;
	_D("n_messages=%d, message_len=%d\n",
		info->tests.n_messages, info->tests.message_size);
	for (i = 0; i < info->tests.n_messages; ++i) {
		info->wr[i].opcode = IB_WR_SEND;
		info->wr[i].wr_id = (i << 1) | 1;
		info->wr[i].num_sge = 1;
		info->wr[i].sg_list = &info->sge[i];
		info->wr[i].next = NULL;
		info->wr[i].send_flags = IB_SEND_SIGNALED;
	}

	_I("-----> Test starts\n");
	info->test_ok = false;
	init_completion(&info->test_done);
	atomic64_set(&info->tests.st.s_messages, 0);
	s->start_time = ktime_get().tv64;
	for (i = 0; i < info->tests.n_messages; ++i) {
		if (atomic64_inc_return(&s->s_messages) <= s->messages) {
			_D("--> i=%d\n", i);
			atomic_set(&info->msgs[i].n, 2);
			if ((rv = ib_post_send(info->net.qp, &info->wr[i], &bad_wr)) < 0) {
				_E("ib_post_send() returned %d\n", rv);
				break;
			}
		}
		else
			break;
	}
	if (rv)
		_E("test ended with error %d\n", rv);
	else {
		_I("-----> Waiting for test done\n");
		poll_q(info);
		_I("-----> Test is over\n");
		_I("messages = %lld, mps=%lld\n",
			s->messages, DIV_ROUND_CLOSEST(s->messages * 1000000000ULL,
				s->end_time - s->start_time));
	}
	FOUT;
}

/* run the test by the test type */
static void run_test(struct _info *info)
{
	FIN;
	msleep(2000);
	if (info->tests.type == tt_rdma_write)
		run_rdma_write_test(info);
	else if (info->tests.type == tt_send)
		run_send_test(info);
	else if (info->tests.type == tt_send_poll)
		run_send_poll_test(info);
	msleep(500);
	info->done = true;
	msleep(500);
	FOUT;
}

/* allocate the test data */
static void init_info(struct _info *info)
{
	struct u_iu *iu;
	int i;

	FIN;
	_D("Allocating info\n");
	if (!(info->snd_bufs = kzalloc(
		sizeof(*info->snd_bufs) * info->tests.n_messages, GFP_KERNEL)) ||
		!(info->rcv_bufs = kzalloc(
			sizeof(*info->rcv_bufs) * info->tests.n_messages, GFP_KERNEL)) ||
		!(info->msgs = kzalloc(
			sizeof(*info->msgs) * info->tests.n_messages, GFP_KERNEL)) ||
		!(info->wr = kzalloc(
			sizeof(*info->wr) * info->tests.n_messages, GFP_KERNEL)) ||
		!(info->sge = kzalloc(
			sizeof(*info->sge) * info->tests.n_messages, GFP_KERNEL)) ||
		!(info->s_wcs = kzalloc(
			sizeof(*info->s_wcs) * 2 * info->tests.n_messages, GFP_KERNEL)) ||
		!(info->r_wcs = kzalloc(
			sizeof(*info->r_wcs) * 2 * info->tests.n_messages, GFP_KERNEL))) {
		_E("Fail to allocate client info\n");
		BUG_ON(true);
	}
	for (i = 0; i < info->tests.n_messages; ++i) {
		iu = &info->snd_bufs[i];
		iu->size = info->tests.message_size;
		iu->index = i;
		BUG_ON(!(iu->buf =
				 ib_dma_alloc_coherent(info->ib_port->_dev->u_dev.ib_dev,
					 iu->size, &iu->dma, GFP_KERNEL)));
		*(int *)iu->buf = i;
		info->sge[i].addr = iu->dma;
		info->sge[i].length = info->tests.message_size;
		info->sge[i].lkey = info->ib_port->_dev->u_dev.mr->lkey;
		iu = &info->rcv_bufs[i];
		iu->size = info->tests.message_size;
		iu->index = i;
		BUG_ON(!(iu->buf =
				 ib_dma_alloc_coherent(info->ib_port->_dev->u_dev.ib_dev,
					 iu->size, &iu->dma, GFP_KERNEL)));
		atomic_set(&info->msgs[i].n, 0);
	}
	info->srq = info->tests.srq;
	FOUT;
}

/* start a new test - this function is launched by writing into SysFS entry */
static ssize_t start_test(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct ib_port *ib_port = container_of(dev, struct ib_port, dev);
	struct _info *info = NULL;
	struct ib_qp_init_attr qp_init = {0};
	struct ib_qp_attr qp_attr = {0};
	int rv;
	static atomic_t a = ATOMIC_INIT(0);
	int i = atomic_inc_return(&a);

	FIN;
	_D("Allocating info\n");
	if (!(info = kzalloc(sizeof(*info), GFP_KERNEL))) {
		_E("Fail to allocate client info\n");
		BUG_ON(true);
	}
	else
		info->ib_port = ib_port;
	if ((rv = parse_options(buf, info))) {
		_E("Fail to parse input command\n");
		goto out;
	}
	/* allocate the test info stuff */
	init_info(info);
	init_completion(&info->login_done);

	if (info->tests.type != tt_send_poll) {
		info->net.scq = ib_create_cq(ib_port->_dev->u_dev.ib_dev,
			s_completion_callback, NULL, info, info->tests.message_size, i % 12);
		BUG_ON(IS_ERR(info->net.scq));
		info->net.rcq = ib_create_cq(ib_port->_dev->u_dev.ib_dev,
			r_completion_callback, NULL, info, info->tests.message_size, i % 12);
		BUG_ON(IS_ERR(info->net.rcq));
	}
	if (info->tests.type == tt_send_poll) {
		info->net.scq = ib_create_cq(ib_port->_dev->u_dev.ib_dev,
			sr_completion_callback, NULL, info,
			2 * info->tests.message_size, i % 12);
		BUG_ON(IS_ERR(info->net.scq));
	}

	/* create the test QP */
	qp_init.qp_context = (void *)info;
	qp_init.event_handler = qp_event;
	if (info->tests.type != tt_send_poll) {
		qp_init.send_cq = info->net.scq;
		qp_init.recv_cq = info->net.rcq;
	}
	else {
		qp_init.send_cq = info->net.scq;
		qp_init.recv_cq = info->net.scq;
	}
	qp_init.cap.max_send_wr = info->tests.n_messages;
	qp_init.cap.max_send_sge = 1;
	qp_init.sq_sig_type = IB_SIGNAL_REQ_WR;
	qp_init.qp_type = IB_QPT_RC;
	if (info->srq)
		qp_init.srq = info->ib_port->_dev->u_dev.srq.srq;
	else {
		qp_init.cap.max_recv_wr = info->tests.n_messages;
		qp_init.cap.max_recv_sge = 1;
	}

	info->net.qp = ib_create_qp(ib_port->_dev->u_dev.pd, &qp_init);
	BUG_ON(IS_ERR(info->net.qp));
	
	BUG_ON(ib_find_pkey(ib_port->_dev->u_dev.ib_dev, ib_port->port,
		be16_to_cpu(info->path.pkey), &qp_attr.pkey_index));

	qp_attr.qp_state = IB_QPS_INIT;
	qp_attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE |
		IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE;
	qp_attr.port_num = ib_port->port;
	BUG_ON(ib_modify_qp(info->net.qp, &qp_attr,
		IB_QP_STATE | IB_QP_PKEY_INDEX | IB_QP_ACCESS_FLAGS | IB_QP_PORT));

	/* create the CM for the connection with the server */
	info->net.cm_id = ib_create_cm_id(
		ib_port->_dev->u_dev.ib_dev, cm_handler, info);
	BUG_ON(IS_ERR(info->net.cm_id));
	
	/* look for the server */	
	if (!lookup_path(ib_port, info)) {
		_D("path_record dump:\n"
			"\t\tservice_id..............0x%016llu\n"
			"\t\tdgid....................%pI6\n"
			"\t\tsgid....................%pI6\n"
			"\t\tdlid....................0x%X\n"
			"\t\tslid....................0x%X\n"
			"\t\thop_limit...............0x%X\n"
			"\t\ttclass..................0x%X\n"
			"\t\tnum_path_revers.........0x%X\n"
			"\t\tpkey....................0x%X\n"
			"\t\tqos_class...............0x%X\n"
			"\t\tsl......................0x%X\n"
			"\t\tmtu.....................0x%X\n"
			"\t\trate....................0x%X\n"
			"\t\tpkt_life................0x%X\n"
			"\t\tpreference..............0x%X\n"
			"",
			info->service_id,
			&info->path.dgid,
			&info->path.sgid,
			__be16_to_cpu(info->path.dlid),
			__be16_to_cpu(info->path.slid),
			info->path.hop_limit,
			info->path.traffic_class,
			info->path.numb_path,
			__be16_to_cpu(info->path.pkey),
			info->path.qos_class,
			info->path.sl,
			info->path.mtu,
			info->path.rate,
			info->path.packet_life_time,
			info->path.preference);	
	}
	else
		goto out;
	
	/* try to login with the server */	
	send_login_req(info);
	_D("Sent login request from QPN %#06x\n",
	    info->net.qp->qp_num);
	
	BUG_ON(wait_for_completion_interruptible(&info->login_done) < 0);
	info->connected = true;
	/* if login was successful run the test */
	if (!info->status)
		run_test(info);

out:
	/* test (?) was over - disconnect and destroy the connection */
	if (info && info->connected && !info->disconnected) {
		disconnect(info, true);
		if (info->tests.type == tt_send)
			wait_for_completion_interruptible(&info->disconnect_done);
	}
	destroy_conn(info);
	kfree(info);
	FOUT;
	return count;
}

static DEVICE_ATTR(start_test, S_IWUSR|S_IWGRP, NULL, start_test);

static ssize_t show_ibdev(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ib_port *ib_port = container_of(dev, struct ib_port, dev);
	return sprintf(buf, "%s\n", ib_port->_dev->u_dev.ib_dev->name);
}
static DEVICE_ATTR(ibdev, S_IRUGO, show_ibdev, NULL);

static ssize_t show_port(struct device *dev, struct device_attribute *attr,
						 char *buf)
{
	struct ib_port *ib_port = container_of(dev, struct ib_port, dev);
	return sprintf(buf, "%d\n", ib_port->port);
}
static DEVICE_ATTR(port, S_IRUGO, show_port, NULL);

static struct ib_port *add_port(struct _dev *_dev, u8 port)
{
	struct ib_port *ib_port;
	int rv;

	FIN;
	ib_port = kzalloc(sizeof *ib_port, GFP_KERNEL);
	if (!ib_port) {
		FOUT;
		return NULL;
	}

	init_completion(&ib_port->released);
	ib_port->_dev = _dev;
	ib_port->port = port;

	if ((rv = __ib_query_gid(
		_dev->u_dev.ib_dev, ib_port->port, 0, &ib_port->gid))) {
		_E("ib_query_gid() failed (%d).\n", rv);
		goto free_port;
	}

	/* create a device and SysFS entries */
	ib_port->dev.class = &clt_class;
	ib_port->dev.parent = _dev->u_dev.ib_dev->dma_device;
	dev_set_name(&ib_port->dev, "nvmeib-%s-%d", _dev->u_dev.ib_dev->name, port);
	_I("device %s, parane %s\n",
		dev_name(&ib_port->dev), dev_name(_dev->u_dev.ib_dev->dma_device));

	if (device_register(&ib_port->dev))
		goto free_port;

	if (device_create_file(&ib_port->dev, &dev_attr_start_test))
		goto err_class;
	if (device_create_file(&ib_port->dev, &dev_attr_ibdev))
		goto err_class;
	if (device_create_file(&ib_port->dev, &dev_attr_port))
		goto err_class;

	FOUT;
	return ib_port;

err_class:
	device_unregister(&ib_port->dev);

free_port:
	kfree(ib_port);
	FOUT;
	return NULL;
}

static void unregister_port(struct ib_port *ib_port)
{
	FIN;
	device_unregister(&ib_port->dev);
	/*
	 * Wait for the sysfs entry to go away, so that no new
	 * storages can be created.
	 */
	wait_for_completion(&ib_port->released);
	FOUT;
}

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

static void add_one(struct ib_device *device)
{
	struct _dev *_dev;
	struct ib_port *ib_port;
	int s, e, p;
	
	FIN;
	if (!(_dev = init(device))) {
		FOUT;
		return;
	}
	
	INIT_LIST_HEAD(&_dev->u_dev.port_list);

	if (device->node_type == RDMA_NODE_IB_SWITCH) {
		s = 0;
		e = 0;
	} else {
		s = 1;
		e = get_device_phys_port_count(device);
	}

	_D("dev %s: %p, s=%d, e=%d\n", device->name, device, s, e);
	for (p = s; p <= e; ++p) {
		ib_port = add_port(_dev, p);
		if (ib_port)
			list_add_tail(&ib_port->link, &_dev->u_dev.port_list);
		else
			goto free_dev;
	}
	ib_set_client_data(device, &client, _dev);
	list_add_tail(&_dev->u_dev.link, &dev_list);
	FOUT;
	return;

free_dev:
	freee(_dev);
	kfree(_dev);
	_dev = NULL;
	_I("%s failed.\n", device->name);
}

static void remove_port(struct ib_port *ib_port)
{
	FIN;
	/* unregister ib port so no more storages can be access to sys fs */
	unregister_port(ib_port);
	kfree(ib_port);
	FOUT;
}

static void remove_one(struct ib_device *device)
{
	struct _dev *_dev;
	struct ib_port *ib_port, *tmp_ib_port;
	
	FIN;
	_dev = ib_get_client_data(device, &client);
	if (!_dev) {
		FOUT;
		return;
	}

	list_for_each_entry_safe(ib_port, tmp_ib_port, &_dev->u_dev.port_list, link)
		remove_port(ib_port);
	
	list_del(&_dev->u_dev.link);
	freee(_dev);
	kfree(_dev);
	FOUT;
}

static int __init cltib_init(void) /* Constructor */
{
	int rv;

	FIN;
	if ((rv = class_register(&clt_class))) {
		_E("couldn't register class %s\n", clt_class.name);
		goto out;
	}
	ib_sa_register_client(&sa_cli);

	if ((rv = ib_register_client(&client)))
		goto err_sa;

	_I("Hooray: cltib registered\n");
	goto out;

err_sa:
	ib_sa_unregister_client(&sa_cli);
	class_unregister(&clt_class);

out:
	FOUT;
	return rv;
}
 
static void __exit cltib_exit(void) /* Destructor */
{
	FIN;
	ib_unregister_client(&client);
	ib_sa_unregister_client(&sa_cli);
	class_unregister(&clt_class);
	
    _I("Hooray: cltib unregistered\n");
	FOUT;
}
 
module_init(cltib_init);
module_exit(cltib_exit);

