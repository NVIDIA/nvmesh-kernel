/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "xkr_incs.h"
#include "xkr_version.h"
#include "xib_incs.h"
#include "main.h"
#include "manager.h"
#include "rdma_dev.h"
#include "utils.h"
#include "requests.h"
#include "poller.h"
#include "wth.h"
#include "larray.h"
#include "xtrace.h"

#include "rdma/ib_addr.h"
#if !HAS_IB_QUERY_GID
#	include "rdma/ib_cache.h"
#endif
#ifdef NO_OFED
#	include "ib_verbs_exp_def.h"
#	include "ib_verbs_exp.h"

struct ib_dct *ib_exp_create_dct(struct ib_pd *pd,
		struct ib_dct_init_attr *attr, struct ib_udata *udata)
{
	return NULL;
}

int ib_exp_destroy_dct(struct ib_dct *dct,  struct ib_udata *udata)
{
	return 0;
}

#else
#	include "rdma/ib_verbs_exp.h"
#endif
#include "rdma/ib_sa.h"

#include <linux/inet.h>
#include <rdma/ib.h>

#ifndef ALIGN_DOWN
#	define ALIGN_DOWN(x, a)	__ALIGN_KERNEL((x) - ((a) - 1), (a))
#endif

#if !IB_HAS_GID_ATTR && !IB_HAS_RDMA_GET_GID_ATTR
#	error "must have API to get GID attributes"
#endif

#define VERSION ((u32)0x1)

#define CQ_POLL_BATCH 16
#define MAX_IO_REQS_BITS_PER_CORE 6
#define MAX_IO_REQS_PER_CORE (1 << MAX_IO_REQS_BITS_PER_CORE)
#define MAX_BYTES_PER_IO_REQ (128 * 1024)
#define MAX_PAGES_PER_IO_REQ (DIV_ROUND_UP(MAX_BYTES_PER_IO_REQ, PAGE_SIZE))
/**
 * The 3 below is for the following:
 * 1 for MR invalidate
 * 1 for MR map
 * 1 for Memory age
 */
#define MAX_RDMA_WR_PER_IO_REQ (3 * MAX_PAGES_PER_IO_REQ)
#define MAX_WR_PER_IO_REQ (MAX_RDMA_WR_PER_IO_REQ + 1)
#define PCPU_QP_MAX_SENDQ (MAX_WR_PER_IO_REQ * MAX_IO_REQS_PER_CORE)
#define PCPU_CQ_MAX_SIZE 4096
#define IB_INTR_POLL_BUDGET_IRQ 256
#define IB_POLL_FLAGS (IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS)
#define CQ_INTR_PROCESS_BATCH 4
#define CQ_INTR_PROCESS_MAX_TIME msecs_to_jiffies(2)
#define CQ_INTR_PROCESS_MAX_RESTART 10
#define DC_KEY 0xdeadbeef
#define N_IB_PATH_PREFIX 4
#define CM_PAYLOAD_SIZE IB_CM_REP_PRIVATE_DATA_SIZE
#define	FMR_SIZE 512
#define FMR_MIN_SIZE 127

#if !IB_HAS_GID_ATTR && !IB_HAS_RDMA_GET_GID_ATTR
#	error "must have API to get GID attributes"
#endif

#define GUID_SIZE sizeof("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
void format_gid_raw(const u8 raw[16], char *buf)
{
	int i, n;

	for (n = 0, i = 0; i < 8; ++i) {
		n += sprintf(buf + n, "%04x", be16_to_cpu(((__be16 *)raw)[i]));
		if (i < 7)
			buf[n++] = ':';
	}
}

struct x_cm_payload {
	u8 h256[32];
	union service_id sid;
	__be64 dct_key;
};

struct _rdma_dev;
struct prot_version {
	union {
		struct {
			u32 major : 16;
			u32 minor : 8;
			u32 sub_minor : 8;
		};
		u32 v;
	};
};

struct hdr_flags {
	union {
		struct {
			u32 req : 1;
			u32 pad : 31;
		};
		u32 v;
	};
};

struct msg_hdr {
	__be32 version;
	__be32 flags;
	union service_id src;
	union service_id dst;
};

struct msg_hdr_ring {
	struct msg_hdr **ring;
	dma_addr_t *ring_dma;
	int size;
};

struct iu {
	int index;
	u64 dma;
	void *buf;
	size_t size;
};

struct srq_info {
	struct dev_per_cpu *pcpu;
	struct ib_srq *srq;
	u32 srqn;
	struct iu **rx_ring;
	struct msg_hdr_ring *msg_hdr_ring;
	int srq_queue_size;
	int srq_msg_size;
};

struct fr_desc {
	struct ib_mr *mr;
	u8 valid;
	u32 prev_rkey;
	u8 bind_err;
	struct list_head link;
};

struct fr_pool {
	int size;
	int	max_page_list_len;
	spinlock_t lock;
	struct list_head free_list;
	int n_free;
	struct list_head err_list;
	int n_error;
	struct ib_pd *pd;
	/* must be last member */
	struct fr_desc desc[0];
};

struct ib_dc_wr {
	struct ib_rdma_wr wr;
	struct ib_ah *ah;
	u64 dct_access_key;
	u32 dct_number;
};

struct dev_per_cpu;
struct wire_msg {
	struct dev_per_cpu *pcpu;
	struct x_ref in_use;
	u64 key;
	int index;
	struct list_head link;
};

struct io_wr {
	struct ib_dc_wr wr;
	struct ib_sge sg;
};

struct io_req {
	struct wire_msg w;
	struct ib_send_wr inval[MAX_PAGES_PER_IO_REQ];
	int n_inval;
	struct ib_reg_wr mrmap[MAX_PAGES_PER_IO_REQ];
	struct fr_desc * fr_desc[MAX_PAGES_PER_IO_REQ];
	int n_map;
	struct io_wr rdma[MAX_PAGES_PER_IO_REQ];
	int n_rdma;
	struct scatterlist *sg;
	int nents;
	enum dma_data_direction dir;
	struct msg_hdr *hdr;
	dma_addr_t hdr_dma;
	void *msg;
	dma_addr_t msg_dma;
	struct ib_dc_wr send;
	struct ib_sge sl[2];
	void (*send_comp)(void *ctx, struct ib_wc *wc);
	void *ctx;
	void *priv;
};

static inline struct io_req * w_2_s(struct wire_msg *w)
{
	return container_of(w, struct io_req, w);
}

struct ioreq_entry {
	u32 ioreq_index;
	u64 ioreq_id;
};

struct cq_info {
	struct dev_per_cpu *pcpu;
	struct ib_cq *cq;
	int intr_vector;
	int ncqe;
	struct srq_info *srq_info;
	struct ib_wc wcs[CQ_POLL_BATCH];
	struct irq_poll	iop;
	char trace_buf[128];
#ifdef CQ_DEBUG
	u64 n_completions;
#endif
	int cpu_id;
	int n_cpu_change;
	bool is_polling;

	/* manage cq's qps */
	spinlock_t cq_lock;
	int locking_cpu;
	u64 req_counter;
	LARRAY_DEFINE(in_progress, struct ioreq_entry, MAX_IO_REQS_BITS_PER_CORE);
};

struct ib_port;
struct dev_per_cpu {
	struct per_core_rdma rdma;
	struct ib_port *ib_port;
	struct srq_info *srq;
	struct cq_info *cq;
	struct ib_qp *qp;
	struct ib_dct *dct;
	int cpu;
	struct x_ref in_use;
	struct fr_pool *pool;
	struct io_req io_reqs[MAX_IO_REQS_PER_CORE];
	struct list_head io_req_pool;
	/* the per_cpu services */
	struct manager_per_cpu *pcpu;
	struct list_head link;
};

struct addr_reg {
	void (*f)(void *ctx, struct local_address_info *a);
	void *ctx;
	struct list_head link;
};

struct connect_request;
struct _rdma_dev {
	struct manager *owner;
	struct rdma_dev dev;
	struct ib_device_attr dev_attr;
	struct list_head port_list;
	struct ib_pd *pd;
	struct ib_mr *mr;
	u64 mr_page_mask;
	int mr_page_size;
	u64 mr_max_size;
	int	max_pages_per_mr;
	struct srq_info srq;
	int srq_size;
	int srq_msg_size;
	int num_comp_vectors;
	struct wth *wth;
	struct ib_event_handler event_handler;
	int n_cores;
	struct list_head addr_reg_list;
};

enum path_status {
	ps_new = 0,
	ps_key,
	ps_connected,
	ps_established,
	ps_bad,
};

enum hendler_type {
	pht_client,
	pht_server
};

/* path client */
struct _client {
	struct list_head link;
	enum hendler_type type;
	struct client_ft ft;
	void *ctx;
	char payload[CONNECT_PAYLOAD_SIZE];
	u64 id;
	bool connected;
};

struct _server {
	struct server_info info;
};

struct dct_info {
	u32 dct_num;
	u32 cpu_core;
};

/* remote address path */
/* used for the path server service */
struct path_pcp {
	struct dev_per_cpu *d;
};

struct remote_paths;
struct _path {
	struct ib_port *ib_port;
	void *owner_path;
	struct rdma_cm_id *cm_id;
	struct ib_gid_attr ib_gid_attr;
	struct ib_ah *ah;
	bool is_loopback;
	char accept_connect[CM_PAYLOAD_SIZE];
	int payload_size;
	unsigned long usage;
	enum path_status status;
	int array_index;
	u64 key;
	unsigned long connect_ts;
	unsigned long disconnect_ts;
	struct remote_paths *rp;
	u64 remote_dct_key;
	u32 remote_dct_num;
	struct dct_info *remote_dct_info;
	int n_dct_info;
	struct list_head clients;
	/* path server service */
	struct path_pcp __percpu *pcpu;
	union service_id sid;
	bool ready;
	struct list_head link;
};

struct paths {
	__be64 path_prefix;
	struct radix_tree_root paths;
};

struct remote_paths {
	struct list_head all_paths;
};

enum internal_msg_opcode {
	imo_discover_req,
	imo_discover_rep,
	imo_connect_payload,
	imo_accept_payload,
	imo_send_error,
};

static inline const char * internal_msg_opcode_2_str(int opcode)
{
	switch (opcode) {
	case imo_discover_req: return "imo_discover_req";
	case imo_discover_rep: return "imo_discover_rep";
	case imo_connect_payload: return "imo_connect_payload";
	case imo_accept_payload: return "imo_accept_payload";
	default: return "???";
	}
}

struct internal_msg {
	__be32 opcode;
};

struct wire_dct_info {
	__be32 dct_num;
	__be32 cpu_core;
};

struct internal_discovery {
	struct internal_msg base;
	__be32 npairs;
	struct wire_dct_info a[0];
};

struct internal_payload {
	struct internal_msg base;
	u64 cid;
	char payload[CONNECT_PAYLOAD_SIZE];
};

struct ib_port {
	/* owner device */
	struct _rdma_dev *_dev;
	struct x_ref in_use;
	u64 uuid;
	/* port stuff */
	u8 port;
	union ib_gid gid;
	struct ib_gid_attr gid_attr;
	bool ready;
	bool is_roce;
	int gid_index;
	struct device dev;
	struct completion released;
	struct ib_port_attr attr;
	int global;
	struct sockaddr_storage bind_sin;
	struct rdma_cm_id *listener;
	spinlock_t guard;
	/* listening server */
	struct _server server;
	/* connect remote paths */
	struct remote_paths crp;
	/* in progress connections */
	struct list_head in_prog_accept;
	struct list_head in_prog_conn;
	struct list_head in_prog_disconn;
	struct list_head pcpu_list;
	struct list_head link;
	/* dummy stuff used to build the mesh od completion queyes and interrupts */
	struct server_info dummy_server;
	void *dummy_server_path;
	struct server_request *tt;
	struct client_info dummy_client;
	void *dummy_client_path;
	struct connect_request *rr;
	/* the fast memory registration key pool */
	struct fr_pool *pool;
	/* for dummy test */
	void *buf;
	dma_addr_t dma;
	struct post_send_info ps;
	struct dev_per_cpu *cur_cpu;
	bool dummy_test_in_progress;
};

struct _memory_reg {
	struct memory_reg base;
	struct _rdma_dev *_dev;
	struct ib_mr *mr;
	struct sg_table sg_head;
	unsigned sg_nents;
	int nmap;
};

/**
 * Start rdma_dev requests
 */

struct cm_event_request {
	struct request_base r;
	struct rdma_cm_id *cm_id;
	u64 port_uuid;
	int port_num;
	struct sockaddr_storage src;
	struct sockaddr_storage dst;
	struct rdma_cm_event cm_event;
	char payload[CM_PAYLOAD_SIZE];
};

static void cm_event_request_free(struct request_base *r)
{
	kfree(container_of(r, struct cm_event_request, r));
}

struct ib_event_request {
	struct request_base r;
	struct ib_event event;
};

static void ib_event_request_free(struct request_base *r)
{
	kfree(container_of(r, struct ib_event_request, r));
}

struct address_request {
	struct request_base r;
	void (*f)(void *ctx, struct local_address_info *a);
	void *ctx;
};

static void address_request_free(struct request_base *r)
{
	kfree(container_of(r, struct address_request, r));
}

struct connect_request {
	struct request_base r;
	struct sockaddr_storage src;
	struct sockaddr_storage dst;
	struct client_info client;
};

static void connect_request_free(struct request_base *r)
{
	kfree(container_of(r, struct connect_request, r));
}

struct disconnect_request {
	struct request_base r;
	void *client;
	void *path;
	struct completion *c;
};

static void disconnect_request_free(struct request_base *r)
{
	kfree(container_of(r, struct disconnect_request, r));
}

struct server_request {
	struct request_base r;
	struct sockaddr_storage src;
	struct server_info server;
};

static void server_request_free(struct request_base *r)
{
	kfree(container_of(r, struct server_request, r));
}

struct server_unreg_request {
	struct request_base r;
	void *server;
	struct sockaddr_storage *src;
	struct completion *c;
};

static void server_unreg_request_free(struct request_base *r)
{
	kfree(container_of(r, struct server_unreg_request, r));
}

struct dummy_test_request {
	struct request_base r;
	int port_num;
};

static void dummy_test_request_free(struct request_base *r)
{
	kfree(container_of(r, struct dummy_test_request, r));
}

struct dummy_test_comp_request {
	struct request_base r;
	struct ib_wc wc;
	struct dev_per_cpu *cur_cpu;
	int port_num;
};

static void dummy_test_comp_request_request_free(struct request_base *r)
{
	kfree(container_of(r, struct dummy_test_comp_request, r));
}

struct path_op_request {
	struct request_base r;
	struct _path *path;
	struct dev_per_cpu *d;
	int port_num;
	union service_id sender_id;
	struct ib_wc wc;
	int index;
};

static void path_op_request_free(struct request_base *r)
{
	kfree(container_of(r, struct path_op_request, r));
}

struct path_op_send_error_request {
	struct request_base r;
	struct _path *path;
	struct dev_per_cpu *d;
	int port_num;
	int opcode;
};

static void path_op_send_error_request_free(struct request_base *r)
{
	kfree(container_of(r, struct path_op_send_error_request, r));
}

struct mem_reg_request {
	struct request_base r;
	void *addr;
	unsigned len;
	struct memory_reg **mem;
	struct completion *c;
};

static void mem_reg_request_free(struct request_base *r)
{
	kfree(container_of(r, struct mem_reg_request, r));
}

struct mem_unreg_request {
	struct request_base r;
	struct _memory_reg *mem;
	int *rv;
	struct completion *c;
};

static void mem_unreg_request_free(struct request_base *r)
{
	kfree(container_of(r, struct mem_unreg_request, r));
}

/**
 * End rdma_dev requests
 */

static void fill_msg_hdr(struct msg_hdr *hdr,
	union service_id *src, __be32 src_dct_num,
	union service_id *dst, __be32 dst_dct_num)
{
	struct prot_version p;
	
	FIN;
	p.v = cpu_to_be32(VERSION);
	hdr->src = *src;
	hdr->src.global.dct = src_dct_num;
	hdr->dst = *dst;
	hdr->dst.global.dct = dst_dct_num;
	FOUT;
}

int rdma_dev_get_src_addr(void *path, struct sockaddr_storage *a)
{
	struct _path *p = path;
	int rv;
	if (p && p->cm_id) {
		*a = p->cm_id->route.addr.src_addr;
		rv = 0;
	}
	else
		rv = -1;
	return rv;
}

int rdma_dev_get_dst_addr(void *path, struct sockaddr_storage *a)
{
	struct _path *p = path;
	int rv;
	if (p && p->cm_id) {
		*a = p->cm_id->route.addr.dst_addr;
		rv = 0;
	}
	else
		rv = -1;
	return rv;
}

static inline struct _rdma_dev * dev_from_cq(struct cq_info *cq)
{
	return cq->pcpu->ib_port->_dev;
}

static atomic_t cq_vector_value = ATOMIC_INIT(0);
int cq_vector_get(int max_vec)
{
	return atomic_inc_return(&cq_vector_value) % max_vec;
}

static struct iu * alloc_ioctx(
	struct _rdma_dev *_dev, int ioctx_size, int dma_size)
{
	struct iu *ioctx = NULL;

	//FIN;
	if (_dev->dev.ib_dev) {
		if (!(ioctx = kmalloc(ioctx_size, GFP_KERNEL))) {
			xetrace("Failed to allocate ioctx for device %s\n",
				_dev->dev.ib_dev->name);
			goto out;
		}
		if (!(ioctx->buf = ib_dma_alloc_coherent(
			_dev->dev.ib_dev, dma_size, &ioctx->dma, GFP_KERNEL))) {
			xetrace("Failed to allocate ioctx buffer for device %s\n",
				_dev->dev.ib_dev->name);
			goto freei;
		}
		ioctx->size = dma_size;
	}
	goto out;

freei:
	kfree(ioctx);
	ioctx = NULL;

out:
	//FOUT;
	return ioctx;
}

static void free_ioctx(
	struct ib_device *dev, struct iu *ioctx, int dma_size)
{
	if (ioctx) {
		ib_dma_free_coherent(dev, dma_size, ioctx->buf, ioctx->dma);
		kfree(ioctx);
	}
}

static struct iu ** alloc_ioctx_ring(struct _rdma_dev *_dev, int ring_size,
	int ioctx_size, int dma_size)
{
	struct iu **ring = NULL;
	int i;

	FIN;
	if ((ring = kmalloc(ring_size * sizeof(ring[0]), GFP_KERNEL))) {
		for (i = 0; i < ring_size; ++i) {
			ring[i] = alloc_ioctx(_dev, ioctx_size, dma_size);
			ring[i]->index = i;
		}
	}
	FOUT;
	return ring;
}

static void free_ioctx_ring(struct iu **ioctx_ring,
	struct ib_device *dev, int ring_size, int dma_size)
{
	int i;

	FIN;
	if (ioctx_ring) {
		for (i = 0; i < ring_size; ++i)
			free_ioctx(dev, ioctx_ring[i], dma_size);
		kfree(ioctx_ring);
	}
	FOUT;
}

static void free_msg_hdr_ring(
	struct _rdma_dev *_dev, struct msg_hdr_ring *ring)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	int i;

	FIN;
	if (ring) {
		for (i = 0; i < ring->size; ++i)
			if (ring->ring[i])
				ib_dma_free_coherent(ib_dev, sizeof(struct msg_hdr),
					ring->ring[i], ring->ring_dma[i]);
		kfree(ring->ring);
		kfree(ring->ring_dma);
		kfree(ring);
	}
	FOUT;
}

static struct msg_hdr_ring * alloc_msg_hdr_ring(struct _rdma_dev *_dev,
	int ring_size)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct msg_hdr_ring *ring;
	int i;

	FIN;
	if (!(ring = kzalloc(sizeof(*ring), GFP_KERNEL))) {
		xetrace("Failed to allocate msg_hdr_ring for device %s\n",
			ib_dev->name);
		goto out;
	}
	else
		ring->size = ring_size;
	if (!(ring->ring = kzalloc(ring_size * sizeof(ring[0]), GFP_KERNEL))) {
		xetrace("Failed to allocate msg_ring for device %s\n",
			ib_dev->name);
		goto free_ring;
	}
	if (!(ring->ring_dma = kzalloc(
		ring_size * sizeof(ring->ring_dma[0]), GFP_KERNEL))) {
		xetrace("Failed to allocate msg_ring_dma for device %s\n",
			ib_dev->name);
		goto free_ring;
	}
	for (i = 0; i < ring_size; ++i) {
		if (!(ring->ring[i] = ib_dma_alloc_coherent(
			ib_dev, sizeof(struct msg_hdr), &ring->ring_dma[i], GFP_KERNEL))) {
			xetrace("Failed to allocate msg_hdr for device %s\n", ib_dev->name);
			goto free_ring;
		}
	}
	goto out;


free_ring:
	free_msg_hdr_ring(_dev, ring);
	ring = NULL;

out:
	FOUT;
	return ring;
}

static int post_recv(struct _rdma_dev *_dev, struct srq_info *srq, int i)
{
	struct ib_recv_wr wr;
	IB_DECLARE_BAD_RECV_WR(bad_wr);
	struct ib_sge list[2];
	static int one_trace = 1;
	int rv;

	//FIN;
	list[0].addr = srq->msg_hdr_ring->ring_dma[i];
	list[0].length = sizeof(struct msg_hdr);
	list[0].lkey = _dev->mr->lkey;
	list[1].addr = srq->rx_ring[i]->dma;
	list[1].length = srq->rx_ring[i]->size;
	list[1].lkey = _dev->mr->lkey;

	memset(&wr, 0, sizeof(wr));
	wr.next = NULL;
	//_D("index=%d\n", iu->index);
	wr.wr_id = i;
	wr.sg_list = list;
	wr.num_sge = 2;
	if (one_trace) {
		one_trace = 0;
		xetrace("Device %s, "
				"list[0].addr=%#llx, list[0].length=%u, list[0].lkey=%#x, "
				"list[1].addr=%#llx, list[1].length=%u, list[1].lkey=%#x\n",
			_dev->dev.ib_dev->name,
			list[0].addr, list[0].length, list[0].lkey,
			list[1].addr, list[1].length, list[1].lkey);
	}
	rv = ib_post_srq_recv(srq->srq, &wr, &bad_wr);
	//FOUT;
	return rv;
}

static int fill_recv_q(struct _rdma_dev *_dev, struct srq_info *srq)
{
	int rv = 0, i;

	FIN;
	for (i = 0; i < srq->srq_queue_size; i++)
		 if ((rv = post_recv(_dev, srq, i)) < 0) {
			 xetrace("Device %s, ib_post_srq_recv() failed: %d\n",
				 _dev->dev.ib_dev->name, rv);
			 goto out;
		 }

out:
	FOUT;
	return rv;
}

static void srq_event_handler(struct ib_event *event, void *ctx)
{
	struct _rdma_dev *_dev = ctx;
	xetrace("%s SRQ event %d\n", _dev->dev.ib_dev->name, event->event);
}

static void free_srq(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct srq_info *srq = d->srq;

	FIN;
	if (srq->srq) {
		ib_destroy_srq(srq->srq);
		srq->srq = NULL;
	}
	if (srq->rx_ring) {
		free_ioctx_ring(srq->rx_ring, _dev->dev.ib_dev,
			srq->srq_queue_size, srq->srq_msg_size);
		srq->rx_ring = NULL;
	}
	if (srq->msg_hdr_ring) {
		free_msg_hdr_ring(_dev, srq->msg_hdr_ring);
		srq->msg_hdr_ring = NULL;
	}
	FOUT;
}

/* create volume shared receive queue */
static int create_srq(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	int q_size = _dev->srq_size;
	int msg_size = _dev->srq_msg_size;
	struct srq_info *srq;
	struct ib_srq_init_attr srq_attr;
	int rv = 0;

	FIN;
	if (!(srq = kzalloc(sizeof(*srq), GFP_KERNEL))) {
		xetrace("Failed to allocate struct srq_info for device %s\n",
			_dev->dev.ib_dev->name);
		rv = -ENOMEM;
		goto out;
	}
	q_size = min(q_size, 4096);
	xdtrace("Device %s, q_size=%d, msg_size=%d\n",
		_dev->dev.ib_dev->name, q_size, msg_size);
	memset(&srq_attr, 0, sizeof(srq_attr));
	srq_attr.event_handler = srq_event_handler;
	srq_attr.srq_context = _dev;
	srq_attr.attr.max_wr = q_size;
	srq_attr.attr.max_sge = 2;
	srq_attr.attr.srq_limit = 0;
	srq_attr.srq_type = IB_SRQT_BASIC;

	srq->srq = ib_create_srq(_dev->pd, &srq_attr);
	if (IS_ERR_OR_NULL(srq->srq)) {
		rv = PTR_ERR(srq->srq);
		xetrace("Fail to create shared receive queue for device %s: %d\n",
			_dev->dev.ib_dev->name, rv);
		goto out;
	}
	else if (false/* && (rv = ibv_get_srq_num(srq->srq, &srq->srqn))*/) {
		xetrace("Fail to ibv_get_srq_num for device %s: %d\n",
			_dev->dev.ib_dev->name, rv);
		goto out;
	}
	else {
		srq->srqn = 0;
		xdtrace("Device %s, srq=%p\n", _dev->dev.ib_dev->name, srq->srq);
	}
	srq->srq_queue_size = q_size;
	srq->srq_msg_size = msg_size;
	srq->pcpu = d;
	d->srq = srq;
	/* allocate receive buffers */
	srq->rx_ring = alloc_ioctx_ring(_dev, q_size,
		sizeof(*srq->rx_ring[0]), msg_size);
	if (!srq->rx_ring)
		goto free_srqq;
	srq->msg_hdr_ring = alloc_msg_hdr_ring(_dev, q_size);
	if (!srq->msg_hdr_ring)
		goto free_srqq;
	if ((rv = fill_recv_q(_dev, srq)))
		goto free_srqq;
	goto out;

free_srqq:
	free_srq(_dev, d);

out:
	FOUT;
	return rv;
}

#define cq_lock(_cq, _flags) 					    \
do { 												\
	spin_lock_irqsave(&_cq->cq_lock, _flags);		\
	_cq->locking_cpu = smp_processor_id();			\
} while (0)

#define cq_unlock(_cq, _flags) 					    \
do { 												\
	_cq->locking_cpu = -1;							\
	spin_unlock_irqrestore(&_cq->cq_lock, _flags);	\
} while (0)

#define cq_is_locked(_cq) ({					              \
	irqs_disabled() && _cq->locking_cpu == smp_processor_id();\
})

#define wc_to_key(_wc) ((u64)wc->wr_id)

#define is_wc_recv(_wc) (_wc->opcode & IB_WC_RECV)

static struct io_req * cq_get(struct cq_info *cq, u64 key)
{
	struct io_req *s = NULL;
	int index;
	unsigned long flags;

	cq_lock(cq, flags);
	index = cq->in_progress.a[key].ioreq_index;
	if (index != -1 && cq->pcpu->io_reqs[index].w.index != -1) {
		if (cq->pcpu->io_reqs[index].w.key == cq->in_progress.a[key].ioreq_id) {
			s = &cq->pcpu->io_reqs[index];
			if (!x_ref_get(&s->w.in_use))
				s = NULL;
		}
	}
	cq_unlock(cq, flags);
	return s;
}

static inline void cq_put(struct cq_info *cq, struct io_req *s)
{
	x_ref_put(&s->w.in_use);
}

static int idx_from_wr_id(struct cq_info *cq, u64 wr_id)
{
	return wr_id;
}

static bool check_recv_index(struct cq_info *cq, u32 index)
{
	return index < cq->srq_info->srq_queue_size;
}

static void post_orphan_recv(struct cq_info *cq, struct ib_wc *wc)
{
	u32 index = idx_from_wr_id(cq, wc->wr_id);

	//FIN;
	if (check_recv_index(cq, index)) {
		xdtrace("cq=%p, wc=%p, index=%d\n", cq, wc, index);
		post_recv(dev_from_cq(cq), cq->srq_info, index);
	}
	else
		xdtrace("OOPS, Out of range receive index for "
				"cq=%p, wc=%p, index=%d\n", cq, wc, index);
	//FOUT;
}

static inline void * get_recv_buf(struct dev_per_cpu *d, int index)
{
	return d->cq->srq_info->rx_ring[index]->buf;
}

static void process_wc_r(struct cq_info *cq, struct ib_wc *wc)
{
	struct ib_device *ib_dev = dev_from_cq(cq)->dev.ib_dev;
	struct dev_per_cpu *pcpu = cq->pcpu;
	u32 i = idx_from_wr_id(cq, wc->wr_id);
	struct msg_hdr *hdr;
	u64 hdr_dma;
	struct iu *iu;
	u64 dst_id;
	unsigned dst_index;
	recv_comp_t recv_comp = NULL;
	void *ctx;
	unsigned long _flags;

	//FIN;
	xdtrace("i=%d\n", i);
	if (check_recv_index(cq, i)) {
		hdr = cq->srq_info->msg_hdr_ring->ring[i];
		BUG_ON(hdr == NULL);
		hdr_dma = cq->srq_info->msg_hdr_ring->ring_dma[i];
		BUG_ON(hdr_dma == 0);
		iu = cq->srq_info->rx_ring[i];
		ib_dma_sync_single_for_cpu(
			ib_dev, hdr_dma, sizeof(*hdr), DMA_FROM_DEVICE);
		ib_dma_sync_single_for_cpu(ib_dev, iu->dma, iu->size, DMA_FROM_DEVICE);
		dst_id = be64_to_cpu(hdr->dst.global.sid);
		dst_index = be32_to_cpu(hdr->dst.global.index);
		xdtrace("dst_id=%lld, dst_index=%d\n", dst_id, dst_index);
		if (pcpu->pcpu && dst_index < pcpu->pcpu->len) {
			spin_lock_irqsave(&pcpu->pcpu->guard, _flags);
			if (pcpu->pcpu->srvs[dst_index].service_id == dst_id) {
				if(x_ref_get(&pcpu->pcpu->srvs[dst_index].in_use)) {
					recv_comp = pcpu->pcpu->srvs[dst_index].service.recv_comp;
					ctx = pcpu->pcpu->srvs[dst_index].service.ctx;
				}
			}
			spin_unlock_irqrestore(&pcpu->pcpu->guard, _flags);
			if (recv_comp) {
				recv_comp(ctx, wc, iu->buf, pcpu, i, &hdr->src);
				x_ref_put(&pcpu->pcpu->srvs[dst_index].in_use);
			}
			else {
				xttrace("received ser_id=%lld, expeced_ser_id=%lld\n",
					dst_id, pcpu->pcpu->srvs[dst_index].service_id);
				post_orphan_recv(cq, wc);
			}
		}
		else
			post_orphan_recv(cq, wc);
	}
	//FOUT;
}

static void process_wc_s(struct cq_info *cq, struct ib_wc *wc)
{
	struct io_req *s;

	//FIN;
	s = cq_get(cq, wc->wr_id);
	if (s) {
		if (s->send_comp)
			s->send_comp(s->ctx, wc);
		cq_put(cq, s);
	}
	//FOUT;
}

static void process_wc(struct cq_info *cq, struct ib_wc *wc)
{
	//FIN;
	if (is_wc_recv(wc))
		process_wc_r(cq, wc);
	else
		process_wc_s(cq, wc);
	//FOUT;
}

static int process_cq(struct cq_info *cq, int budget)
{
	int n, i, completed = 0;
	int batch_size = ARRAY_SIZE(cq->wcs);

	//FIN;
	if (budget < batch_size)
		batch_size = budget;
	while ((n = ib_poll_cq(cq->cq, batch_size, cq->wcs)) > 0) {
		for (i = 0; i < n ; i++)
			process_wc(cq, &cq->wcs[i]);

		completed += n;
#ifdef CQ_DEBUG
		cq->n_completions += n;
		if ((cq->n_completions % CQ_N_COMP_TRACE) == 0)
			xttrace("Device %s, cq %p has %lld completions\n",
				dev_from_cq(cq)->dev.ib_dev->name, cq, cq->n_completions);
#endif
		if (n != batch_size || (budget != -1 && completed >= budget))
			break;
	}
	//FOUT;
	return completed;
}

static void rearm_or_resched(struct cq_info *cq)
{
	unsigned long flags;

	//FIN;
	local_irq_save(flags);
	if (ib_req_notify_cq(cq->cq, IB_POLL_FLAGS) > 0) {
		cq->is_polling = true;
		local_irq_restore(flags);
		intr_poll_sched(&cq->iop);
	}
	else {
		cq->is_polling = false;
		local_irq_restore(flags);
	}
	//FOUT;
}

/* @param budget is IB_INTR_POLL_BUDGET_IRQ (256) */
static int ib_poll_handler(struct irq_poll *iop, int budget)
{
	struct cq_info *cq = container_of(iop, struct cq_info, iop);
	int completed;

	//FIN;
	completed = process_cq(cq, budget);
	/* This cond is complementary to cond that
	   caller (ipoller_run) checks on retun */
	if (completed < budget) {
		/* cq is not busy, try to switch back to interrupt-mode */
		intr_poll_complete(&cq->iop);
		rearm_or_resched(cq);
	}
	//FOUT;
	return completed;
}

/* We rearm interrupts but may decide to keep/switch2 polling while
   leaving/marking is_polling=1, if this is the case now, bail... */
bool cq_is_polling_exp_mode(struct cq_info *cq, bool exp_mode)
{
	struct ib_device *ib_dev = dev_from_cq(cq)->dev.ib_dev;
	bool rv = true;
	int i = 0;

	//FIN;
	if (cq->is_polling != exp_mode) {
		xwtrace("Device %s, cq=%p, oops, is_polling %d exp. %d, "
				"wait few usec...\n", ib_dev->name,
		   cq, cq->is_polling, exp_mode);

		while (cq->is_polling != exp_mode && i++ < 10)
			udelay(1);

		if (cq->is_polling != exp_mode) {
			xwtrace("Device %s, cq=%p, waited %d usec, is_polling %d "
					"exp. %d, bail!", ib_dev->name,
				cq, i, cq->is_polling, exp_mode);
			rv = false;
		}
	}
	//FOUT;
	return rv;
}

/* @param budget is CQ_INTR_PROCESS_BATCH (4) */
static int ib_poll_handler_intr(struct irq_poll *iop, int budget)
{
	struct cq_info *cq = container_of(iop, struct cq_info, iop);
	int completed = 0;

	//FIN;
	if (!cq_is_polling_exp_mode(cq, false))
		goto comp;

	completed = process_cq(cq, budget);
	if (completed < budget) {
		/* cq is not busy, basically we want to rearm interrupts */
		//TBD:
		//dont rearm-or-resched, make caller call this func once more or
		//until there's is no time/retries budget; do @completed=@budget.
		//(sanity check that completed > 0)
		rearm_or_resched(cq);
	}

comp:
	//FOUT;
	return completed;
}

static void cq_completion_intr(struct ib_cq *cq, void *v)
{
	struct dev_per_cpu *d = v;
	struct cq_info *cqw = d->cq;
	unsigned long end = jiffies + CQ_INTR_PROCESS_MAX_TIME;
	int max_restart = CQ_INTR_PROCESS_MAX_RESTART;
	int completed;
	bool rearm;

	//FIN;
restart:
	completed = ib_poll_handler_intr(&cqw->iop, CQ_INTR_PROCESS_BATCH);
	while ((rearm = (completed >= CQ_INTR_PROCESS_BATCH)) &&
		   time_before(jiffies, end) &&
		   --max_restart)
		goto restart;

	if (rearm) {
		/* we are in interrupt, poller is not running (yet)  */
		cqw->is_polling = true;
		intr_poll_sched(&cqw->iop);
	}
	//FOUT;
}

static void cq_event_handler(struct ib_event *e, void *v)
{
	struct dev_per_cpu *d = v;

	FIN;
	xetrace("pcpu=%p (%s), event=%s\n",
		d, d->ib_port->_dev->dev.ib_dev->name, ib_event_msg(e->event));
	FOUT;
}

static void free_cq(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct cq_info *cq = d->cq;

	FIN;
	xttrace("Device %s, cq %p\n", ib_dev->name, cq);
	if (cq->cq && !IS_ERR(cq->cq)) {
		ib_destroy_cq(cq->cq);
	}
	cq->cq = NULL;
	kfree(cq);
	FOUT;
}

static int create_cq(
	struct _rdma_dev *_dev, struct dev_per_cpu *d, int comp_vector)
{
	struct cq_info *cq;
	int ncqe;
	int rv;

	FIN;
	if (!(cq = kzalloc(sizeof(*cq), GFP_KERNEL))) {
		xetrace("Failed to allocate struct cq_info for device %s\n",
			_dev->dev.ib_dev->name);
		rv = -ENOMEM;
		goto out;
	}
	cq->req_counter = 1;
	LARRAY_INIT(cq->in_progress);
	ncqe = min((int)PCPU_CQ_MAX_SIZE, _dev->dev_attr.max_cqe);
	cq->intr_vector = comp_vector;
	cq->cq = x_create_cq(_dev->dev.ib_dev,
		cq_completion_intr,
		cq_event_handler,
		d, ncqe, cq->intr_vector);
	if (IS_ERR(cq->cq)) {
		rv = PTR_ERR(cq->cq);
		xetrace("Failed to create cq for device %s, rv=%d\n",
			_dev->dev.ib_dev->name, rv);
		goto err_cq;
	}
	cq->ncqe = ncqe;
	intr_poll_init(&cq->iop, IB_INTR_POLL_BUDGET_IRQ, ib_poll_handler);
	ib_req_notify_cq(cq->cq, IB_CQ_NEXT_COMP);
	spin_lock_init(&cq->cq_lock);
	cq->srq_info = d->srq;
	cq->pcpu = d;
	d->cq = cq;
	rv = 0;
	goto out;

err_cq:
	free_cq(_dev, d);
	cq = NULL;

out:
	FOUT;
	return rv;
}

static void qp_event_handler(struct ib_event *e, void *v)
{
	struct dev_per_cpu *d = v;

	FIN;
	xetrace("pcpu=%p (%s), event=%s\n",
		d, d->ib_port->_dev->dev.ib_dev->name, ib_event_msg(e->event));
	FOUT;
}

static int create_qp(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_qp_init_attr qp_init;
	int rv;

	FIN;
	memset(&qp_init, 0, sizeof(qp_init));
	qp_init.event_handler = qp_event_handler;
	qp_init.qp_context = d;
#ifdef NO_OFED
#else
	qp_init.qp_type = IB_EXP_QPT_DC_INI;
#endif
	qp_init.send_cq = d->cq->cq;
	qp_init.recv_cq = d->cq->cq;
	//qp_init.srq = info->selected_dev->srq.srq;
	qp_init.cap.max_send_wr = PCPU_QP_MAX_SENDQ;
	qp_init.cap.max_send_sge = 2;
	d->qp = ib_create_qp(_dev->pd, &qp_init);
	if (IS_ERR_OR_NULL(d->qp)) {
		rv = PTR_ERR(d->qp);
		xetrace("Device %s, ib_create_qp() failed with %d\n",
			ib_dev->name, rv);
		d->qp = NULL;
		rv = -1;
	}
	else {
		xdtrace("Device %s: max cap: qp_init.cap.max_send_wr=%d, "
			  "qp_init.cap.max_send_sge=%d\n",
			ib_dev->name, qp_init.cap.max_send_wr, qp_init.cap.max_send_sge);
		rv = 0;
	}
	FOUT;
	return rv;
}

static void free_qp(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	FIN;
	if (d->qp) {
		ib_destroy_qp(d->qp);
		d->qp = NULL;
	}
	FOUT;
}

static int modify_qp(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	int attr_mask = 0;
	int rv = -1;

	FIN;
	/* modify QP to INIT */
	{
		struct ib_qp_attr attr = {
			.qp_state = IB_QPS_INIT,
			.pkey_index = 0,
			.port_num = d->ib_port->port,
		};
		attr_mask = IB_QP_STATE | IB_QP_PKEY_INDEX | IB_QP_PORT;
		if ((rv = ib_modify_qp(d->qp, &attr, attr_mask))) {
			xetrace("Device %s, failed to INIT QP - %d\n", ib_dev->name, rv);
			goto out;
		}
	}

	/* modify QP to RTR */
	{
		struct ib_qp_attr attr = {
			.qp_state = IB_QPS_RTR,
			.path_mtu = d->ib_port->attr.active_mtu,
			.min_rnr_timer = 0x10,
			.rq_psn = 0,
			.ah_attr = {
				/*.is_global = info->selected_port->global,*/
				.type = d->ib_port->is_roce ?
					RDMA_AH_ATTR_TYPE_ROCE : RDMA_AH_ATTR_TYPE_IB,
				.ah_flags = IB_AH_GRH,
				.sl = 0,
				.port_num = d->ib_port->port,
				.ib.src_path_bits = 0,
				.grh.hop_limit = 1,
				.grh.sgid_index = d->ib_port->gid_index,
				.grh.traffic_class = 0,
			},
			/*
			.max_dest_rd_atomic = 16,
			.timeout = 0x10,
			.retry_cnt = 7,
			.rnr_retry = 7,
			*/
		};
		attr_mask = IB_QP_STATE | IB_QP_PATH_MTU | IB_QP_AV;
		if ((rv = ib_modify_qp(d->qp, &attr, attr_mask))) {
			xetrace("Device %s, failed to RTR QP - %d\n", ib_dev->name, rv);
			goto out;
		}
	}
	/* modify QP to RTS */
	{
		struct ib_qp_attr attr = {
			.qp_state = IB_QPS_RTS,
			.timeout = 0x10,
			.retry_cnt = 7,
			.rnr_retry = 7,
			.sq_psn = 0,
			.max_rd_atomic = 16,
		};
		attr_mask = IB_QP_STATE | IB_QP_TIMEOUT |
			IB_QP_RETRY_CNT | IB_QP_RNR_RETRY | IB_QP_MAX_QP_RD_ATOMIC |
			IB_QP_SQ_PSN;
			// Optional: IB_QP_MIN_RNR_TIMER
		if ((rv = ib_modify_qp(d->qp, &attr, attr_mask))) {
			xetrace("Device %s, failed to RTS QP - %d\n", ib_dev->name, rv);
			goto out;
		}
	}
	rv = 0;

out:
	FOUT;
	return rv;
}

static void dct_event_handler(struct ib_event *e, void *context_ptr)
{
	struct dev_per_cpu *d = context_ptr;

	FIN;
	xetrace("pcpu=%p (%s), event=%s\n",
		d, d->ib_port->_dev->dev.ib_dev->name, ib_event_msg(e->event));
	FOUT;
}

static int create_dct(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
    struct ib_dct_init_attr dctattr = {
		.pd = _dev->pd,
		.cq = d->cq->cq,
		.srq = d->srq->srq,
        .dc_key = DC_KEY,
        .port = d->ib_port->port,
        .access_flags =
			IB_ACCESS_REMOTE_READ |
			IB_ACCESS_REMOTE_WRITE |
			IB_ACCESS_REMOTE_ATOMIC,
        .min_rnr_timer = 2,
        /*.tclass = 16,*/
        .flow_label = 0,
        .mtu = d->ib_port->attr.active_mtu,
        .pkey_index = 0,
        .gid_index = d->ib_port->gid_index,
        .hop_limit = 1,
        .create_flags = 0,
        .inline_size = 0,
		.event_handler = dct_event_handler,
		.dct_context = d,
    };
	int rv;

	FIN;
	xdtrace("Device %s, dct_port=%u\n", ib_dev->name, dctattr.port);
	d->dct = ib_exp_create_dct(_dev->pd, &dctattr, NULL);
	if (IS_ERR_OR_NULL(d->dct)) {
		rv = PTR_ERR(d->dct);
		xdtrace("Device %s, ib_exp_create_dct() failed with %d\n",
			ib_dev->name, rv);
		d->dct = NULL;
	}
	else {
		d->dct->device = ib_dev;
		d->dct->event_handler = dctattr.event_handler;
		d->dct->dct_context = dctattr.dct_context;
		rv = 0;
	}
	FOUT;
	return rv;
}

static void free_dct(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;

	FIN;
	if (d->dct) {
		xdtrace("Device %s, destroying DCT\n", ib_dev->name);
		if (d->dct->device) {
			xdtrace("Device %s, d->dct->device=%p\n",
				ib_dev->name, d->dct->device);
			ib_exp_destroy_dct(d->dct, NULL);
			d->dct = NULL;
		}
		else
			xdtrace("Device %s, d->dct->device=NULL\n", ib_dev->name);
	}
	FOUT;
}


static struct fr_desc * get_frd_(struct fr_pool *pool)
{
	struct fr_desc *d = NULL;

	//FIN;
	if (!list_empty(&pool->free_list)) {
		d = list_first_entry(&pool->free_list, struct fr_desc, link);
		list_del_init(&d->link);
		pool->n_free--;
	}
	//FOUT;
	return d;
}

static struct fr_desc * get_frd(struct fr_pool *pool)
{
	struct fr_desc *d = NULL;
	unsigned long flags;

	//FIN;
	spin_lock_irqsave(&pool->lock, flags);
	d = get_frd_(pool);
	spin_unlock_irqrestore(&pool->lock, flags);
	//FOUT;
	return d;
}

static void put_frd_(struct fr_pool *pool, struct fr_desc **desc, int n)
{
	int i;

	//FIN;
	for (i = 0; i < n; ++i) {
		if (!list_empty(&desc[i]->link)) {
			xwtrace("OOPS, desc %p already linked\n", desc[i]);
			WARN_ON_ONCE(1);
		}
		if (!desc[i]->bind_err) {
			list_add(&desc[i]->link, &pool->free_list);
			pool->n_free++;
		}
		else {
			/* Bind error happened so add to error list for later maintenance */
			list_add(&desc[i]->link, &pool->err_list);
			pool->n_error++;
		}
	}
	//FOUT;
}

static void put_frd(struct fr_pool *pool, struct fr_desc **desc, int n)
{
	unsigned long flags;

	//FIN;
	spin_lock_irqsave(&pool->lock, flags);
	put_frd_(pool, desc, n);
	spin_unlock_irqrestore(&pool->lock, flags);
	//FOUT;
}

static void destroy_fr_pool(struct fr_pool *pool)
{
	struct fr_desc *d;

	FIN;
	if (pool) {
		if (pool->n_free != pool->size) {
			xetrace("FR pool %p: exp %d, found %d\n",
				pool, pool->size, pool->n_free);
			WARN_ON(1);
		}
		while ((d = list_first_entry_or_null(
			&pool->free_list, struct fr_desc, link))) {
			list_del_init(&d->link);
			if (d->mr)
				ib_dereg_mr(d->mr);
		}
		while ((d = list_first_entry_or_null(
			&pool->err_list, struct fr_desc, link))) {
			list_del_init(&d->link);
			if (d->mr)
				ib_dereg_mr(d->mr);
		}
		kvfree(pool);
	}
	FOUT;
}

static struct fr_pool * create_fr_pool(struct _rdma_dev *_dev,
	int max_pages_per_mr, int pool_size, bool *l_retry, bool *p_retry,
	const char *msg)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct fr_pool *pool;
	struct fr_desc *d;
	struct ib_mr *mr;
	enum ib_mr_type mr_type;
	int i, rv;

	FIN;
	xdtrace("Device %s - trying to create a %s FR pool of size %d, max_mr %d\n",
		ib_dev->name, msg, pool_size, _dev->dev_attr.max_mr);
	pool = kvzalloc(sizeof(struct fr_pool) +
		pool_size * sizeof(struct fr_desc), GFP_KERNEL);
	if (!pool) {
		xetrace("Device %s, failed to allocate FR pool\n", ib_dev->name);
		if (p_retry)
			*p_retry = true;
		goto out;
	}
	pool->pd = _dev->pd;
	pool->size = pool_size;
	pool->max_page_list_len = max_pages_per_mr;
	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->free_list);
	INIT_LIST_HEAD(&pool->err_list);
	pool->n_free = pool_size;
	mr_type = (ib_dev->attrs.device_cap_flags & IB_DEVICE_SG_GAPS_REG) ?
		IB_MR_TYPE_SG_GAPS : IB_MR_TYPE_MEM_REG;
	for (i = 0, d = &pool->desc[0]; i < pool->size; ++i, ++d) {
		mr = ib_alloc_mr(_dev->pd, mr_type, pool->max_page_list_len);
		if (IS_ERR_OR_NULL(mr)) {
			rv = PTR_ERR(mr);
			xetrace("Devive %s, "
					"i %d, max_page_list_len %d: "
					"ib_alloc_mr() returned with error %d\n",
				ib_dev->name, i, pool->max_page_list_len, rv);
			mr = NULL;
			if (l_retry)
				*l_retry = true;
			goto destroy_pool;
		}
		d->mr = mr;
		d->valid = true;
		list_add_tail(&d->link, &pool->free_list);
	}
	goto out;

destroy_pool:
	destroy_fr_pool(pool);
	pool = NULL;

out:
	FOUT;
	return pool;
}

static int fill_fr_pool(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_port *ib_port = d->ib_port;
	struct fr_pool *port_pool = ib_port->pool;
	struct fr_pool *pool;
	struct fr_desc *dd;
	int i, size;
	int rv;

	FIN;
	if ((pool = create_fr_pool(
		_dev, port_pool->max_page_list_len, 0, NULL, NULL, "per_CPU"))) {
		size = ib_port->pool->size / _dev->n_cores;
		xdtrace("Device %s: per_CPU %d, create FR pool of size %d\n",
			ib_dev->name, d->cpu, size);
		for (i = 0; i < size; ++i) {
			if ((dd = get_frd(port_pool)))
				put_frd(pool,  &dd, 1);
		}
		pool->size = pool->n_free;
		d->pool = pool;
		rv = 0;
	}
	else {
		xetrace("Device %s, failed to allocate FR pool for cpu %d\n",
			ib_dev->name, d->cpu);
		rv = -1;
	}
	FOUT;
	return rv;
}

static void free_fr_pool(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_port *ib_port = d->ib_port;
	struct fr_pool *port_pool = ib_port->pool;
	struct fr_pool *pool = d->pool;
	struct fr_desc *dd;

	FIN;
	if (pool) {
		if (pool->n_free + pool->n_error != pool->size) {
			xetrace("FR pool %p for cpu %d: exp %d, found %d\n",
				pool, d->cpu, pool->size, pool->n_free);
			WARN_ON(1);
		}
		while ((dd = list_first_entry_or_null(
			&pool->free_list, struct fr_desc, link))) {
			list_del_init(&dd->link);
			put_frd(port_pool, &dd, 1);
		}
		while ((dd = list_first_entry_or_null(
			&pool->err_list, struct fr_desc, link))) {
			list_del_init(&dd->link);
			put_frd(port_pool, &dd, 1);
		}
		kfree(pool);
		d->pool = NULL;
	}
	FOUT;
}

static struct io_req * get_io_req(struct dev_per_cpu *d)
{
	struct wire_msg *w;
	struct io_req *s;
	int index;
	unsigned long flags;

	//FIN;
	cq_lock(d->cq, flags);
	index = LARRAY_GET(d->cq->in_progress);
	if (index != -1) {
		if ((w = list_first_entry_or_null(
			&d->io_req_pool, struct wire_msg, link))) {
			list_del(&w->link);
			w->key = d->cq->req_counter++;
			w->index = index;
			s = w_2_s(w);
			s->n_inval = s->n_map = s->n_rdma = 0;
			s->send.wr.wr.wr_id = index;
			d->cq->in_progress.a[index].ioreq_index = s - d->io_reqs;
			d->cq->in_progress.a[index].ioreq_id = w->key;
		}
		else
			s = NULL;
	}
	else
		s = NULL;
	cq_unlock(d->cq, flags);
	//FOUT;
	return s;
}

static void put_io_req(struct io_req *s)
{
	struct ib_device *ib_dev = s->w.pcpu->ib_port->_dev->dev.ib_dev;
	int i;
	unsigned long flags;

	//FIN;
	if (s->sg) {
		ib_dma_unmap_sg(ib_dev, s->sg, s->nents, s->dir);
		s->sg = NULL;
	}
	put_frd(s->w.pcpu->pool, &s->fr_desc[0], s->n_map);
	for (i = 0; i < s->n_map; ++i)
		s->fr_desc[i] = NULL;
	s->n_inval = s->n_map = s->n_rdma = 0;
	s->send_comp = NULL;
	cq_lock(s->w.pcpu->cq, flags);
	s->w.pcpu->cq->in_progress.a[s->w.index].ioreq_id = 0;
	LARRAY_PUT(s->w.pcpu->cq->in_progress, s->w.index);
	s->w.index = -1;
	list_add_tail(&s->w.link, &s->w.pcpu->io_req_pool);
	cq_unlock(s->w.pcpu->cq, flags);
	//FOUT;
}

static void put_registrant_recv(struct dev_per_cpu *pcpu, int index)
{
	FIN;
	if (check_recv_index(pcpu->cq, index)) {
		xdtrace("cq=%p, index=%d\n", pcpu->cq, index);
		post_recv(pcpu->ib_port->_dev, pcpu->cq->srq_info, index);
	}
	FOUT;
}

static void free_io_reqs(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct io_req *p;
	unsigned long flags;
	int i, j;

	FIN;
	for (i = 0; i < ARRAY_SIZE(d->io_reqs); ++i) {
		p = &d->io_reqs[i];
		if (p->hdr) {
			cq_lock(d->cq, flags);
			p->w.index = -1;
			cq_unlock(d->cq, flags);
			x_ref_release_start(&p->w.in_use);
			x_ref_release_wait(&p->w.in_use);
			ib_dma_free_coherent(ib_dev, sizeof(*p->hdr), p->hdr, p->hdr_dma);
			p->hdr = NULL;
			p->hdr_dma = 0;
		}
		if (p->msg) {
			ib_dma_free_coherent(
				ib_dev, _dev->srq_msg_size, p->msg, p->msg_dma);
			p->msg = NULL;
			p->msg_dma = 0;
		}
		for (j = 0; j < ARRAY_SIZE(p->fr_desc); ++j)
			if (p->fr_desc[j]) {
				put_frd(d->pool, &p->fr_desc[j], 1);
				p->fr_desc[j] = NULL;
			}
	}
	FOUT;
}

static int create_io_reqs(struct _rdma_dev *_dev, struct dev_per_cpu *d)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct io_req *p;
	struct msg_hdr *hdr;
	dma_addr_t hdr_dma;
	void *msg;
	dma_addr_t msg_dma;
	int i, rv;

	FIN;
	for (i = 0; i < ARRAY_SIZE(d->io_reqs); ++i) {
		hdr = ib_dma_alloc_coherent(ib_dev, sizeof(*hdr), &hdr_dma, GFP_KERNEL);
		msg = ib_dma_alloc_coherent(
			ib_dev, _dev->srq_msg_size, &msg_dma, GFP_KERNEL);
		if (!hdr && msg) {
			xetrace("Device %s: failed to allocate io_req #%d\n",
				ib_dev->name, i);
			rv = -1;
			goto freereqss;
		}
		p = &d->io_reqs[i];
		p->w.pcpu = d;
		p->hdr = hdr;
		p->hdr_dma = hdr_dma;
		p->msg = msg;
		p->msg_dma = msg_dma;
		p->sl[0].addr = hdr_dma;
		p->sl[0].length = sizeof(*hdr);
		p->sl[0].lkey = _dev->mr->lkey;
		p->sl[1].addr = msg_dma;
		p->sl[1].length = _dev->srq_msg_size;
		p->sl[1].lkey = _dev->mr->lkey;
		x_ref_init(&p->w.in_use);
		p->send.wr.wr.sg_list = p->sl;
		p->send.wr.wr.num_sge = 2;
		p->send.wr.wr.opcode = IB_WR_SEND;
		list_add_tail(&p->w.link, &d->io_req_pool);
	}
	rv = 0;
	goto out;

freereqss:
	free_io_reqs(_dev, d);

out:
	FOUT;
	return rv;
}

static int dummy_test(void *p, struct request_base *r);
static int post_dummy_test(struct _rdma_dev *_dev, int port_num)
{
	struct dummy_test_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_dummy_test;
		r->r.call = dummy_test;
		r->r.free = dummy_test_request_free;
		r->port_num = port_num;
		wth_push_request(_dev->wth, &r->r);
		rv = 0;
	}
	else {
		xdtrace("Device %s: failed to allocate memory for dummy_test\n",
			_dev->dev.ib_dev->name);
		rv = -1;
	}
	FOUT;
	return rv;
}

static int get_device_phys_port_count(struct ib_device *device)
{
    int port = 0;
    int ret;
    struct ib_port_attr attr;

	FIN;
    if (device->node_type != RDMA_NODE_IB_SWITCH) {
		do {
			++port;
			ret = ib_query_port(device, port, &attr);
		} while (ret == 0);
		port -= 1;
	}
	FOUT;
	return port;
}

static int alloc_per_port_per_cpu_node(
	struct _rdma_dev *_dev, struct ib_port *ib_port, int cpu, int comp_vector)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct dev_per_cpu *d;
	int rv;

	FIN;
	if (!(d = kzalloc(sizeof(*d), GFP_KERNEL))) {
		xetrace("Failed to allocate dev_per_cpu struct for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -ENOMEM;
		goto out;
	}
	else {
		d->ib_port = ib_port;
		d->cpu = cpu;
		INIT_LIST_HEAD(&d->io_req_pool);
		INIT_LIST_HEAD(&d->rdma.link);
		spin_lock_init(&d->rdma.guard);
		x_ref_init(&d->in_use);
	}
	if (create_srq(_dev, d)) {
		xetrace("Failed to create_srq for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -1;
		goto free_d;
	}
	if (create_cq(_dev, d, comp_vector)) {
		xetrace("Failed to create_cq for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -1;
		goto free_srqq;
	}
	if (create_qp(_dev, d)) {
		xetrace("Failed to create_qp for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -1;
		goto free_cq;
	}
	if (modify_qp(_dev, d)) {
		xetrace("Failed to modify_qp for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -1;
		goto free_qpp;
	}
	if (create_dct(_dev, d)) {
		xetrace("Failed to create_dct for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -1;
		goto free_qpp;
	}
	if (fill_fr_pool(_dev, d)) {
		xetrace("Failed to create_fr_pool for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -1;
		goto free_dctt;
	}
	if (create_io_reqs(_dev, d)) {
		xetrace("Failed to create_io_reqs for dev %s on cpu %d\n",
			ib_dev->name, cpu);
		rv = -1;
		goto free_frr;
	}
	list_add_tail(&d->link, &ib_port->pcpu_list);
	rv = 0;
	goto out;

free_frr:
	free_fr_pool(_dev, d);

free_dctt:
	free_dct(_dev, d);

free_qpp:
	free_qp(_dev, d);

free_cq:
	free_cq(_dev, d);

free_srqq:
	free_srq(_dev, d);

free_d:
	kfree(d);

out:
	FOUT;
	return rv;
}

static void free_pcpu(struct dev_per_cpu *d)
{
	FIN;
	free_io_reqs(d->ib_port->_dev, d);
	if (d->pool) {
		free_fr_pool(d->ib_port->_dev, d);
		d->pool = NULL;
	}
	if (d->dct) {
		free_dct(d->ib_port->_dev, d);
		d->dct = NULL;
	}
	if (d->qp) {
		free_qp(d->ib_port->_dev, d);
		d->qp = NULL;
	}
	if (d->cq) {
		free_cq(d->ib_port->_dev, d);
		d->cq = NULL;
	}
	if (d->srq) {
		free_srq(d->ib_port->_dev, d);
		d->srq = NULL;
	}
	FOUT;
}

static int alloc_per_port_per_cpu(
	struct _rdma_dev *_dev, struct ib_port *ib_port)
{
	int node = _dev->dev.ib_dev->dma_device->numa_node;
	int cpu;
	int i, rv;

	FIN;
	i = 0;
	for_each_online_cpu(cpu)
		if (cpu_to_node(cpu) == node) {
			if (alloc_per_port_per_cpu_node(_dev, ib_port, cpu, i++)) {
				rv = -1;
				goto out;
			}
			else {
				if (i >= _dev->dev.ib_dev->num_comp_vectors)
					i = 0;
			}
		}
	rv = 0;

out:
	FOUT;
	return rv;
}

static int query_gid(struct ib_device *device, u8 port, int index,
	struct ib_port *ib_port, bool is_roce)
{
	void *gid_attr_ptr = NULL;
	union ib_gid *gid = &ib_port->gid;
	int rv = -1;

	//FIN;
#if HAS_IB_QUERY_GID
//#	warning "Has HAS_IB_QUERY_GID"
#	if IB_HAS_GID_ATTR
//#		warning "Has IB_HAS_GID_ATTR"
	gid_attr_ptr = &ib_port->gid_attr;
	/* Get default GID as HW GID */
	rv = ib_query_gid(ib, port, index, gid, gid_attr_ptr);
#	else
//#		warning "!Has IB_HAS_GID_ATTR"
	rv = ib_query_gid(ib, port, index, gid);
#	endif
#else
//#	warning "!Has HAS_IB_QUERY_GID"
#	if IB_HAS_GID_ATTR
//#		warning "Has IB_HAS_GID_ATTR"
#		if IB_HAS_RDMA_GET_GID_ATTR
//#			warning "Has IB_HAS_RDMA_GET_GID_ATTR"
		rv = rdma_query_gid(device, port, index, gid);
		if (rv)
			xetrace("Device %s, failed to get gid attributes: rv = %d\n",
				device->name, rv);
		if (!rv && is_roce) {
			if (IS_ERR(gid_attr_ptr =
				(void *)rdma_get_gid_attr(device, port, index)))
				rv = PTR_ERR(gid_attr_ptr);
			else
				ib_port->gid_attr = *(struct ib_gid_attr *)gid_attr_ptr;
		}
#		else
//#			warning "!Has IB_HAS_RDMA_GET_GID_ATTR"
		gid_attr_ptr = &ib_port->gid_attr;
		rv = ib_get_cached_gid(ib, port, index, gid, gid_attr_ptr);
#		endif
#	else
//#		warning "!Has IB_HAS_GID_ATTR"
		rv = rdma_query_gid(ib, port, index, gid);
#	endif
#endif
	//FOUT;
	return rv;
}


static int find_gid(
	struct _rdma_dev *_dev, struct ib_port *ib_port, struct sockaddr_storage *a)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	int sgid_family = -1;
	int a_ss_family;
	bool gid_matched = false;
	union ib_gid ip_gid;
	char gid_str1[GUID_SIZE + 1] = {0};
	char gid_str2[GUID_SIZE + 1] = {0};
	struct sockaddr_ib *pib;
	struct sockaddr_ib *qib;
	struct sockaddr_in *pip;
	int i;
	int rv = -1;

	FIN;
	if ((a && ((struct sockaddr_in *)a)->sin_family == AF_IB) ||
		(!a && !ib_port->is_roce)) {
		if (!ib_port->is_roce) {
			xdtrace("Device %s, port %d - is IB\n",
				ib_dev->name, ib_port->port);
			pib = (struct sockaddr_ib *)&ib_port->bind_sin;
			pib->sib_family = AF_IB;
			pib->sib_sid = cpu_to_be64(NVMESH_SERVICE_ID);
			pib->sib_sid_mask = cpu_to_be64(NVMESH_SERVICE_ID_MASK);
			pib->sib_pkey = cpu_to_be16(NVMESH_PKEY);
			rv = query_gid(ib_dev, ib_port->port, 0, ib_port, false);
			xdtrace("Device %s, port %d - is IB %pI6\n",
				ib_dev->name, ib_port->port, ib_port->gid.raw);
			if (!rv) {
				memcpy(pib->sib_addr.sib_raw, ib_port->gid.raw,
					sizeof(pib->sib_addr.sib_raw));
				if (a) {
					qib = (struct sockaddr_ib *)a;
					if (!memcmp(pib->sib_addr.sib_raw, qib->sib_addr.sib_raw,
						sizeof(ip_gid.raw))) {
						ib_port->gid_index = 0;
						ib_port->global = 0;
					}
					else
						xdtrace("Device %s GID %pI6 does not input GID %pI6\n",
							ib_dev->name, ib_port->gid.raw,
							qib->sib_addr.sib_raw);
				}
				else {
					ib_port->gid_index = 0;
					ib_port->global = 0;
				}
			}
			else
				xdtrace("Device %s failed to query device GID: %d\n",
					ib_dev->name, rv);
		}
		else
			xdtrace("Device %s, port %d - is not an IB device\n",
				ib_dev->name, ib_port->port);
	}
	else {
		if (ib_port->is_roce) {
			xdtrace("Device %s, port %d - is RoCE\n",
				ib_dev->name, ib_port->port);
			ib_port->global = 1;
			pip = (struct sockaddr_in *)&ib_port->bind_sin;
			pip->sin_family = PF_INET;
			pip->sin_port = htons(
				NVMESH_SERVICE_ID & NVMESH_SERVICE_ID_MASK);
			if (a) {
				rdma_ip2gid((struct sockaddr *)a, &ip_gid);
				format_gid_raw(ip_gid.raw, gid_str1);
				xdtrace("Device %s, local IP gid %s (%pIS)\n", ib_dev->name,
					gid_str1, (struct sockaddr *)a);
			}
			else
				a_ss_family = AF_INET;
			for (i = 0; i < ib_port->attr.gid_tbl_len; ++i) {
				if ((rv = query_gid(
					ib_dev, ib_port->port, i, ib_port, true))) {
					//trace("query_gid() failed on index %d\n", i);
					continue;
				}
				xdtrace("Device %s, table index %d\n", ib_dev->name, i);
				if (ib_port->gid.raw[0] == 0 && ib_port->gid.raw[1] == 0)
					sgid_family = AF_INET;
				if (a) {
					if (!memcmp(
						ib_port->gid.raw, ip_gid.raw, sizeof(ip_gid.raw)))
						gid_matched = true;
					else {
						format_gid_raw(ib_port->gid.raw, gid_str2);
						xdtrace("Device %s, local IP gid %s is NOT "
							  "the same as port gis %s\n",
							ib_dev->name,
							gid_str1, gid_str2);
						continue;
					}
				}
				else
					gid_matched = true;
				if (ib_port->gid_attr.gid_type == IB_GID_TYPE_ROCE_UDP_ENCAP &&
					sgid_family == a_ss_family && gid_matched) {
					format_gid_raw(ib_port->gid.raw, gid_str2);
					xdtrace("Device %s, gid table index %d (%s)is of type "
							"RoCE2 and AF_INET\n", ib_dev->name,
						i, gid_str2);
					ib_port->gid_index = i;
					rdma_gid2ip((struct sockaddr *)pip, &ib_port->gid);
					rv = 0;
					break;
				}
				else {
					if (ib_port->gid_attr.gid_type !=
						IB_GID_TYPE_ROCE_UDP_ENCAP)
						xdtrace("Device %s, gid table index %d (%s)is NOT of "
								"type RoCE2 - it's type is %d\n",
							ib_dev->name,
							i, gid_str2, (int)ib_port->gid_attr.gid_type);
					else if (sgid_family != a_ss_family)
						xdtrace("Device %s, gid table index %d (%s)is NOT of"
							  "family AF_INET\n", ib_dev->name,
							i, gid_str2);
				}
			}
		}
		else
			xdtrace("Device %s, port %d - is not a RoCE device\n",
				ib_dev->name, ib_port->port);
	}
	FOUT;
	return rv;
}

static struct _path * find_path(struct ib_port *ib_port,
	struct list_head *l, struct sockaddr_storage *a)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *itr;
	struct sockaddr_ib *pib;
	struct sockaddr_ib *qib;
	struct sockaddr_in *pip;
	struct sockaddr_in *qip;
	char ba[64];
	bool found;

	FIN;
	found = false;
	if (!ib_port->is_roce) {
		pib = (struct sockaddr_ib *)a;
		list_for_each_entry(itr, l, link) {
			qib = (struct sockaddr_ib *)&itr->cm_id->route.addr.dst_addr;
			if (pib->sib_addr.sib_interface_id ==
				qib->sib_addr.sib_interface_id &&
				pib->sib_addr.sib_subnet_prefix ==
				qib->sib_addr.sib_subnet_prefix) {
				found = true;
				xdtrace("Device %s, port %d: path %s already exist\n",
					ib_dev->name, ib_port->port, x_tss(a, ba, sizeof(ba)));
				break;
			}
		}
	}
	else {
		pip = (struct sockaddr_in *)a;
		list_for_each_entry(itr, l, link) {
			qip = (struct sockaddr_in *)&itr->cm_id->route.addr.dst_addr;
			if (pip->sin_addr.s_addr == qip->sin_addr.s_addr) {
				found = true;
				xdtrace("Device %s, port %d: path %s already exist\n",
					ib_dev->name, ib_port->port, x_tss(a, ba, sizeof(ba)));
				break;
			}
		}
	}
	FOUT;
	return found ? itr : NULL;
}

static struct _path * find_path_in_remote_route(struct ib_port *ib_port,
	struct sockaddr_storage *a)
{
	struct remote_paths *rp;
	struct _path *path;

	FIN;
	rp = &ib_port->crp;
	path = find_path(ib_port, &rp->all_paths, a);
	FOUT;
	return path;
}

static void add_path(
	struct ib_port *ib_port, struct remote_paths *rp, struct _path *path)
{
	FIN;
	list_add_tail(&path->link, &rp->all_paths);
	path->rp = rp;
	FOUT;
}

static void init_path(struct _path *path, struct ib_port *ib_port)
{
	FIN;
	path->ib_port = ib_port;
	path->status = ps_new;
	INIT_LIST_HEAD(&path->clients);
	FOUT;

}

static void start_remove_clients_from_path(struct _path *path)
{
	struct _client *c, *t;

	FIN;
	list_for_each_entry_safe(c, t, &path->clients, link)
		if (c->connected) {
			if (c->ft.disconnect)
				c->ft.disconnect(c->ctx, path->owner_path);
		}
		else {
			if (c->ft.connect)
				c->ft.connect(c->ctx, NULL, NULL, 0);
			else {
				list_del_init(&c->link);
				kfree(c);
			}
		}
	FOUT;
}

static void remove_path(struct ib_port *ib_port, struct _path *path)
{
	FIN;
	path->rp = NULL;
	FOUT;
}

static void free_path(struct ib_port *ib_port, struct _path *path)
{
	FIN;
	if (!list_empty(&path->link))
		list_del_init(&path->link);
	if (path->cm_id)
		rdma_destroy_id(path->cm_id);
	remove_path(ib_port, path);
	free_percpu(path->pcpu);
	kfree(path);
	FOUT;
}

static void start_free_path(struct ib_port *ib_port, struct _path *path)
{
	FIN;
	path->ready = false;
	if (!list_empty(&path->link))
		list_del_init(&path->link);
	manager_unregister_service(ib_port->_dev->owner, &path->sid);
	manager_unlink_path(ib_port->_dev->owner, path);
	start_remove_clients_from_path(path);
	if (list_empty(&path->clients))
		free_path(ib_port, path);
	else
		list_add_tail(&path->link, &ib_port->in_prog_disconn);
	FOUT;
}

static void free_paths(struct ib_port *ib_port, struct remote_paths *rp)
{
	struct _path *path;

	FIN;
	while ((path = list_first_entry_or_null(
		&rp->all_paths, struct _path, link))) {
		list_del_init(&path->link);
		start_free_path(ib_port, path);
	}
	FOUT;
}

void init_conn_param(struct ib_port *ib_port, struct _path *path,
	struct rdma_conn_param *conn_param)
{
	struct x_cm_payload *v;
	FIN;
	v = (void *)conn_param->private_data;
	v->sid = path->sid;
	v->sid.global.dct = cpu_to_be32(list_first_entry(
		&ib_port->pcpu_list, struct dev_per_cpu, link)->dct->dct_num);
	v->dct_key = cpu_to_be64(DC_KEY);
	conn_param->private_data_len = sizeof(struct x_cm_payload);
	conn_param->responder_resources = 16;
	conn_param->initiator_depth = 16;
	conn_param->retry_count = 7;
	conn_param->rnr_retry_count = 7;
	FOUT;
}

static struct _path * create_path(struct ib_port *ib_port);
static void handle_connect_request(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path = NULL;
	struct rdma_conn_param conn_param;
	struct x_cm_payload *v;
	int qp_attr_mask = 0;
	struct ib_qp_attr attr;
	char accept_connect[CM_PAYLOAD_SIZE];
	char bs[64];
	char bd[64];
	int rv;

	FIN;
	xdtrace("Device %s, port %d: "
			"received connection request from peer %s to %s\n",
		ib_dev->name, ib_port->port,
		x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
		x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)));
	if (!ib_port->ready) {
		xetrace("Device %s, port %d: is not ready to accept connections\n",
			ib_dev->name, ib_port->port);
		goto destroy;
	}
	if (!(path = create_path(ib_port))) {
		xetrace("Device %s, port %d: failed to allocate path\n",
			ib_dev->name, ib_port->port);
		goto destroy;
	}
	memcpy(path->accept_connect, rr->payload, sizeof(path->accept_connect));
	v = (void *)path->accept_connect;
	path->remote_dct_key = be64_to_cpu(v->dct_key);
	path->remote_dct_num = be32_to_cpu(v->sid.global.dct);
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_RTR;
	if (rdma_init_qp_attr(rr->cm_id, &attr, &qp_attr_mask)) {
		xetrace("Device %s, port %d: rdma_init_qp_attr() failed\n",
			ib_dev->name, ib_port->port);
		goto destroy;
	}
	path->ah = rdma_create_user_ah(ib_port->_dev->pd, &attr.ah_attr, NULL);
	if (IS_ERR_OR_NULL(path->ah)) {
		rv = PTR_ERR(path->ah);
		path->ah = NULL;
		xetrace("Device %s, port %d: rdma_create_ah() failed %d\n",
			ib_dev->name, ib_port->port, rv);
		goto destroy;
	}
	conn_param.private_data = accept_connect;
	init_conn_param(ib_port, path, &conn_param);
	if (rdma_accept(rr->cm_id, &conn_param)) {
		xetrace("Device %s, port %d: "
				"failed to accept connection request\n",
			ib_dev->name, ib_port->port);
		goto destroy;
	}
	path->cm_id = rr->cm_id;
	path->payload_size = rr->cm_event.param.conn.private_data_len;
	if (path->payload_size)
		memcpy(path->accept_connect, rr->payload, path->payload_size);
	list_add_tail(&path->link, &ib_port->in_prog_accept);
	goto out;

destroy:
	rdma_destroy_id(rr->cm_id);
	if (path)
		start_free_path(ib_port, path);

out:
	FOUT;
}

static void start_path_disconnect(struct ib_port *ib_port, struct _path *path)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	char bs[64];
	char bd[64];

	FIN;
	if (path->connect_ts &&
		!path->is_loopback &&
		rdma_disconnect(path->cm_id)) {
		xetrace("Device %s, port %d: "
				"failed to call rdma_disconnect for connection from "
				"peer %s to %s was estblished\n",
			ib_dev->name, ib_port->port,
			x_tss(&path->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
			x_tss(&path->cm_id->route.addr.src_addr, bs, sizeof(bs)));
	}
	start_free_path(ib_port, path);
	FOUT;
}

static int create_ah(struct ib_port *ib_port, struct _path *path)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct ib_qp_attr attr;
	int qp_attr_mask = 0;
	struct ib_gid_attr *ib_gid_attr;
	struct rdma_ah_attr ah_attr;
	struct rdma_ah_attr *p_ah_attr;
	int rv = -1;
	 
	FIN;
	if (!path->is_loopback) {
		memset(&attr, 0, sizeof(attr));
		attr.qp_state = IB_QPS_RTR;
		if (rdma_init_qp_attr(path->cm_id, &attr, &qp_attr_mask)) {
			xetrace("Device %s, port %d: failed to call rdma_init_qp_attr\n",
				ib_dev->name, ib_port->port);
			goto out;
		}
		p_ah_attr = &attr.ah_attr;
	}
	else {
		ib_gid_attr = &path->ib_gid_attr;
		memset(&ah_attr, 0, sizeof(ah_attr));
		memset(ib_gid_attr, 0, sizeof(*ib_gid_attr));
		ib_gid_attr->ndev = ib_port->gid_attr.ndev;
		ib_gid_attr->device = ib_dev;
		memcpy(ib_gid_attr->gid.raw, ib_port->gid.raw,
			sizeof(ib_gid_attr->gid.raw));
		ib_gid_attr->gid_type =
			ib_port->is_roce ? IB_GID_TYPE_ROCE_UDP_ENCAP : IB_GID_TYPE_IB;
		ib_gid_attr->index = ib_port->gid_index;
		ib_gid_attr->port_num = ib_port->port;
		if ((rv = ib_init_ah_attr_from_path(ib_dev, ib_port->port,
			path->cm_id->route.path_rec, &ah_attr, ib_gid_attr))) {
			xetrace("Device %s, port %d: failed to call "
					"ib_init_ah_attr_from_path, rv %d\n",
				ib_dev->name, ib_port->port, rv);
			goto out;
		}
		p_ah_attr = &ah_attr;
	}
	path->ah = rdma_create_user_ah(ib_port->_dev->pd, p_ah_attr, NULL);
	if (IS_ERR_OR_NULL(path->ah)) {
		rv = PTR_ERR(path->ah);
		path->ah = NULL;
		xetrace("Device %s, port %d: "
				"failed to call rdma_create_user_ah, rv %d\n",
			ib_dev->name, ib_port->port, rv);
		goto out;
	}
	rv = 0;

out:
	FOUT;
	return rv;
}

static void sync_io_req(struct _rdma_dev *_dev, struct io_req *io_req)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	
	FIN;
	ib_dma_sync_single_for_device(
		ib_dev, io_req->hdr_dma, sizeof(*io_req->hdr), DMA_TO_DEVICE);
	ib_dma_sync_single_for_device(
		ib_dev, io_req->msg_dma, _dev->srq_msg_size, DMA_TO_DEVICE);
	FOUT;
}

static int start_discovery(struct _path *path);
static void handle_connect_response(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	struct x_cm_payload *v;
	bool found = false;
	char bs[64];
	char bd[64];

	FIN;
	list_for_each_entry(path, &ib_port->in_prog_conn, link) {
		if (path->cm_id == rr->cm_id) {
			found = true;
			break;
		}
	}
	if (!found) {
		xetrace("Device %s, port %d: "
					"no path found for address resolved from "
					"peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)));
		goto out;
	}
	path->connect_ts = jiffies;
	memcpy(path->accept_connect, rr->payload, sizeof(path->accept_connect));
	v = (void *)path->accept_connect;
	path->remote_dct_key = be64_to_cpu(v->dct_key);
	path->remote_dct_num = be32_to_cpu(v->sid.global.dct);
	list_del_init(&path->link);
	if (create_ah(ib_port, path)) {
		xetrace("Device %s, port %d: "
				"failed to call create_ah after connection from "
				"peer %s to %s was estblished\n",
			ib_dev->name, ib_port->port,
			x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
			x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)));
		goto disconnectt;
	}
	if (!path->is_loopback && rdma_accept(path->cm_id, NULL)) {
		xetrace("Device %s, port %d: "
				"failed to call rdma_accept after connection from "
				"peer %s to %s was estblished\n",
			ib_dev->name, ib_port->port,
			x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
			x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)));
		goto disconnectt;
	}
	memcpy(path->accept_connect, rr->payload,
		rr->cm_event.param.conn.private_data_len);
	add_path(ib_port, &ib_port->crp, path);
	path->status = ps_connected;
	if (start_discovery(path))
		goto disconnectt;
	else
		goto out;

disconnectt:
	start_path_disconnect(ib_port, path);

out:
	FOUT;
}

static void handle_connect_error(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	list_for_each_entry(path, &ib_port->in_prog_conn, link) {
		if (path->cm_id == rr->cm_id) {
			list_del_init(&path->link);
			found = true;
			break;
		}
	}
	if (found)
		start_free_path(ib_port, path);
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
					"no path found for connect_error from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void handle_disconnect(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	path = find_path_in_remote_route(ib_port, &rr->cm_id->route.addr.dst_addr);
	if (path)
		found = true;
	else {
		list_for_each_entry(path, &ib_port->in_prog_conn, link) {
			if (path->cm_id == rr->cm_id) {
				found = true;
				break;
			}
		}
	}
	if (found)
		start_path_disconnect(ib_port, path);
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
				"no path found for disconnect from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void handle_addr_resolved(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	list_for_each_entry(path, &ib_port->in_prog_conn, link) {
		if (path->cm_id == rr->cm_id) {
			found = true;
			break;
		}
	}
	if (found) {
		if (rdma_resolve_route(path->cm_id, 2000)) {
			xetrace("Device %s, port %d: "
					"failed to call address_resolve from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
			start_free_path(ib_port, path);
		}
	}
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
					"no path found for address_resolve from "
					"peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void handle_addr_error(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	list_for_each_entry(path, &ib_port->in_prog_conn, link) {
		if (path->cm_id == rr->cm_id) {
			list_del_init(&path->link);
			found = true;
			break;
		}
	}
	if (found)
		start_free_path(ib_port, path);
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
					"no path found for address_error from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void handle_route_resolved(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	char accept_connect[CM_PAYLOAD_SIZE];
	struct rdma_conn_param conn_param = {0};
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;
	int rv;

	FIN;
	list_for_each_entry(path, &ib_port->in_prog_conn, link) {
		if (path->cm_id == rr->cm_id) {
			xdtrace("Found cm_id %p\n", rr->cm_id);
			found = true;
			break;
		}
	}
	if (found) {
		conn_param.private_data = accept_connect;
		init_conn_param(ib_port, path, &conn_param);
		if (!path->is_loopback) {
			xdtrace("rdma_connect()\n");
			if ((rv = rdma_connect(path->cm_id, &conn_param))) {
				xetrace("Device %s, port %d: "
						"failed to call rdma_connect from peer %s to %s, "
						"rv= %d\n",
					ib_dev->name, ib_port->port,
					x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
					x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
					rv);
				start_free_path(ib_port, path);
			}
		}
		else {
			xdtrace("loopback()\n");
			handle_connect_response(ib_port, rr);
		}
	}
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
					"no path found for route_resolved from "
					"peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void handle_route_error(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	list_for_each_entry(path, &ib_port->in_prog_conn, link) {
		if (path->cm_id == rr->cm_id) {
			list_del_init(&path->link);
			found = true;
			break;
		}
	}
	if (found)
		start_free_path(ib_port, path);
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
					"no path found for route_error from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void handle_established(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct _path *path;
	bool found = false;

	FIN;
	list_for_each_entry(path, &ib_port->in_prog_accept, link) {
		if (path->cm_id == rr->cm_id) {
			list_del_init(&path->link);
			add_path(ib_port, &ib_port->crp, path);
			found = true;
			break;
		}
	}
	if (!found)
		xttrace("Device %s, port %d: path not fount\n",
			ib_port->_dev->dev.ib_dev->name, ib_port->port);
	FOUT;
}

static void handle_unreachable(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	path = find_path_in_remote_route(ib_port, &rr->cm_id->route.addr.dst_addr);
	if (path)
		start_path_disconnect(ib_port, path);
	else {
		list_for_each_entry(path, &ib_port->in_prog_conn, link) {
			if (path->cm_id == rr->cm_id) {
				found = true;
				break;
			}
		}
	}
	if (found)
		start_free_path(ib_port, path);
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
				"no path found for unreachable_event from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void handle_rejected(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	xetrace("Device %s, port %d: reject message %s (cm_id %p)\n",
		ib_dev->name, ib_port->port, ibcm_reject_msg(rr->cm_event.status),
		rr->cm_id);
	path = find_path_in_remote_route(ib_port, &rr->cm_id->route.addr.dst_addr);
	if (path)
		start_path_disconnect(ib_port, path);
	else {
		list_for_each_entry(path, &ib_port->in_prog_conn, link) {
			if (path->cm_id == rr->cm_id) {
				xdtrace("Found cm_id %p in in_prog_connect\n", rr->cm_id);
				found = true;
				break;
			}
		}
		if (!found) {
			list_for_each_entry(path, &ib_port->in_prog_accept, link) {
				if (path->cm_id == rr->cm_id) {
					xdtrace("Found cm_id %p in in_prog_accept\n", rr->cm_id);
					found = true;
					break;
				}
			}
		}
	}
	if (found)
		start_free_path(ib_port, path);
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
				"no path found for reject_event from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static void dev_free(struct _rdma_dev *_dev);
static void handle_device_removal(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	FIN;
	dev_free(ib_port->_dev);
	FOUT;
}

static void handle_multicast_join(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	FIN;
	FOUT;
}

static void handle_multicast_error(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	FIN;
	FOUT;
}

static void free_port(struct _rdma_dev *_dev, struct ib_port *ib_port);
static struct ib_port * add_port(struct _rdma_dev *_dev, int port_num);
static void handle_addr_change(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct _rdma_dev *_dev = ib_port->_dev;
	int port_num = ib_port->port;

	FIN;
	list_del_init(&ib_port->link);
	free_port(_dev, ib_port);
	ib_port = add_port(_dev, port_num);
	if (ib_port)
		list_add_tail(&ib_port->link, &_dev->port_list);
	FOUT;
}

static void handle_timewait_exit(
	struct ib_port *ib_port, struct cm_event_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	char bs[64];
	char bd[64];
	bool found = false;

	FIN;
	path = find_path_in_remote_route(ib_port, &rr->cm_id->route.addr.dst_addr);
	if (path)
		start_path_disconnect(ib_port, path);
	else {
		list_for_each_entry(path, &ib_port->in_prog_conn, link) {
			if (path->cm_id == rr->cm_id) {
				found = true;
				break;
			}
		}
	}
	if (found)
		start_free_path(ib_port, path);
	else if (rr->cm_id)
		xetrace("Device %s, port %d: "
				"no path found for timewait_exit from peer %s to %s\n",
				ib_dev->name, ib_port->port,
				x_tss(&rr->cm_id->route.addr.src_addr, bs, sizeof(bs)),
				x_tss(&rr->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	FOUT;
}

static struct ib_port * find_port_by_num(struct _rdma_dev *_dev, int port_num)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_port *ib_port;
	bool found = false;

	FIN;
	list_for_each_entry(ib_port, &_dev->port_list, link)
		if (ib_port->port == port_num) {
			xttrace("Device %s, found port %d\n", ib_dev->name, port_num);
			found = true;
			break;
		}
	FOUT;
	return found ? ib_port : NULL;
}

static struct ib_port * find_port_by_uuid(struct _rdma_dev *_dev, u64 port_uuid)
{
	struct ib_port *ib_port;
	bool found = false;

	FIN;
	list_for_each_entry(ib_port, &_dev->port_list, link)
		if (ib_port->uuid == port_uuid) {
			found = true;
			break;
		}
	FOUT;
	return found ? ib_port : NULL;
}

static int on_cm_event(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct cm_event_request *rr =
		container_of(r, struct cm_event_request, r);
	struct ib_port *ib_port;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	char bs[64];
	char bd[64];

	FIN;
	xdtrace("Device %s, port %d: "
			"received cm_event %s from peer %s to %s\n",
		ib_dev->name, rr->port_num, rdma_event_msg(rr->cm_event.event),
		x_tss(&rr->src, bs, sizeof(bs)), x_tss(&rr->dst, bd, sizeof(bd)));
	if (!(ib_port = find_port_by_uuid(_dev, rr->port_uuid))) {
		xdtrace("Device %s, port %d: failed to find port with uuid %lld\n",
			ib_dev->name, rr->port_num, rr->port_uuid);
		goto out;
	}
	switch (rr->cm_event.event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		handle_addr_resolved(ib_port, rr);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
		handle_addr_error(ib_port, rr);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		handle_route_resolved(ib_port, rr);
		break;
	case RDMA_CM_EVENT_ROUTE_ERROR:
		handle_route_error(ib_port, rr);
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		handle_connect_request(ib_port, rr);
		break;
	case RDMA_CM_EVENT_CONNECT_RESPONSE:
		handle_connect_response(ib_port, rr);
		break;
	case RDMA_CM_EVENT_CONNECT_ERROR:
		handle_connect_error(ib_port, rr);
		break;
	case RDMA_CM_EVENT_UNREACHABLE:
		handle_unreachable(ib_port, rr);
		break;
	case RDMA_CM_EVENT_REJECTED:
		handle_rejected(ib_port, rr);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		handle_established(ib_port, rr);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		handle_disconnect(ib_port, rr);
		break;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		handle_device_removal(ib_port, rr);
		break;
	case RDMA_CM_EVENT_MULTICAST_JOIN:
		handle_multicast_join(ib_port, rr);
		break;
	case RDMA_CM_EVENT_MULTICAST_ERROR:
		handle_multicast_error(ib_port, rr);
		break;
	case RDMA_CM_EVENT_ADDR_CHANGE:
		handle_addr_change(ib_port, rr);
		break;
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		handle_timewait_exit(ib_port, rr);
		break;
	default:
		break;
	}

out:
	FOUT;
	return 0;
}

static int rdma_cm_handler(
	struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
	struct ib_port *ib_port = cm_id->context;
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct cm_event_request *r;
	int rv;

	FIN;
	xdtrace("Device %s, port %d: received RDMA_CM event %s (%d), "
			"cm_id %p, (status=%d)\n",
		ib_dev->name, ib_port->port,
		rdma_event_msg(event->event), (int)event->event, cm_id, event->status);
	if (!(r = kzalloc(sizeof(*r), GFP_ATOMIC))) {
		xetrace("Failed to allocate cm_event for device %s, port %d: "
				"received RDMA_CM event %s (%d) (status=%d)\n",
			ib_dev->name, ib_port->port,
			rdma_event_msg(event->event), (int)event->event, event->status);
		rv = -1;
		goto out;
	}
	if (event->param.conn.private_data_len) {
		if (event->param.conn.private_data_len <= sizeof(r->payload))
			memcpy(r->payload, event->param.conn.private_data,
				event->param.conn.private_data_len);
		else {
			xetrace("Device %s, port %d: rdma event private data len %d "
					"exceed supported size %d\n",
				ib_dev->name, ib_port->port,
				event->param.conn.private_data_len, (int)sizeof(r->payload));
			kfree(r);
			rv = -1;
			goto out;
		}
	}
	r->r.type = rpcrdma_cm_event;
	r->r.call = on_cm_event;
	r->r.free = cm_event_request_free;
	r->cm_id = cm_id;
	r->port_uuid = ib_port->uuid;
	r->port_num = cm_id->port_num;
	r->src = cm_id->route.addr.src_addr;
	r->dst = cm_id->route.addr.dst_addr;
	r->cm_event = *event;
	if (!rdma_dev_push_request(&ib_port->_dev->dev, &r->r))
		BUG();
	rv = 0;

out:
	FOUT;
	return rv;
}

static int start_new_connection(struct ib_port *ib_port,
	struct _client *client, struct connect_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _path *path;
	struct sockaddr_ib *pib;
	struct sockaddr_ib *qib;
	struct sockaddr_in *pip;
	struct sockaddr_in *qip;
	int rv;

	FIN;
	if (!(path = create_path(ib_port))) {
		rv = -ENOMEM;
		goto out;
	}
	path->cm_id = rdma_create_id(rdma_cm_handler, ib_port,
		ib_port->is_roce ? RDMA_PS_TCP : RDMA_PS_IB, IB_QPT_RC);
	if (path->cm_id == NULL) {
		xetrace("Device %s port %d, fail to create rdma_cm_id\n",
			ib_dev->name, ib_port->port);
		rv = -1;
		goto free_pathh;
	}
	else
		xdtrace("New cm_id %p\n", path->cm_id);
	if (ib_port->is_roce) {
		pip = (struct sockaddr_in *)&rr->src;
		qip = (struct sockaddr_in *)&rr->dst;
		path->is_loopback = pip->sin_addr.s_addr == qip->sin_addr.s_addr;
	}
	else {
		pib = (struct sockaddr_ib *)&rr->src;
		qib = (struct sockaddr_ib *)&rr->dst;
		path->is_loopback = memcmp(pib->sib_addr.sib_raw,
			qib->sib_addr.sib_raw, sizeof(pib->sib_addr.sib_raw)) == 0;
	}
	if (rr->dst.ss_family == AF_IB) {
		qib = (struct sockaddr_ib *)&rr->dst;
		xttrace("qib->sib_sid=%llx\n", be64_to_cpu(qib->sib_sid));
	}
	rv = rdma_resolve_addr(path->cm_id, (struct sockaddr *)&rr->src,
		(struct sockaddr *)&rr->dst, 2000);
	if (rv) {
		xetrace("Device %s port %d, failed to call rdma_resolve_addr(), %d\n",
			ib_dev->name, ib_port->port, rv);
		rv = -1;
		goto free_pathh;
	}
	list_add_tail(&client->link, &path->clients);
	list_add_tail(&path->link, &ib_port->in_prog_conn);
	rv = 0;
	goto out;

free_pathh:
	start_free_path(ib_port ,path);

out:
	FOUT;
	return rv;
}

static int send_payload(struct _path *path, struct _client *client);
static int start_client(struct ib_port *ib_port, struct connect_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	struct _client *client;
	struct _path *path;
	char bs[64];
	int rv = -1;

	FIN;
	if (!(client = kzalloc(sizeof(*client), GFP_KERNEL))) {
		xetrace("Device %s: failed to alllocate memory for client of "
				"port %s\n", ib_dev->name, x_tss(&rr->src, bs, sizeof(bs)));
		goto out;
	}
	else {
		client->type = pht_client;
		client->ft = rr->client.ft;
		client->ctx = rr->client.client;
		memcpy(client->payload, rr->client.payload, sizeof(client->payload));
		client->id = x_get_guid();
	}
	/* connect from client connect */
	/* try to register client in already established paths */
	if (!(path = find_path(ib_port, &ib_port->crp.all_paths, &rr->dst))) {
		/* path was not established yet so look in the
		   in_progress connections
		*/
		path = find_path(ib_port, &ib_port->in_prog_conn, &rr->dst);
	}
	if (path) {
		list_add_tail(&client->link, &path->clients);
		if (path->status == ps_established) {
			if (send_payload(path, client)) {
				list_del_init(&client->link);
				kfree(client);
				goto out;
			}
		}
		rv = 0;
	}
	else {
		if ((rv = start_new_connection(ib_port, client, rr)))
			kfree(client);
		else
			rv = 0;
	}

out:
	FOUT;
	return rv;
}

static struct ib_port * find_port_by_addr(
	struct _rdma_dev *_dev, struct sockaddr_storage *src)
{
	struct ib_port *ib_port;
	struct sockaddr_ib *pib;
	struct sockaddr_ib *qib;
	struct sockaddr_in *pip;
	struct sockaddr_in *qip;
	bool found = false;

	FIN;
	list_for_each_entry(ib_port, &_dev->port_list, link) {
		if (ib_port->is_roce) {
			pip = (struct sockaddr_in *)src;
			qip = (struct sockaddr_in *)&ib_port->bind_sin;
			if (pip->sin_addr.s_addr == qip->sin_addr.s_addr) {
				xttrace("Device %s, RoCE port %d: match source address %pIS\n",
					_dev->dev.ib_dev->name, ib_port->port,
					(struct sockaddr *)src);
				found = true;
				break;
			}
		}
		else {
			pib = (struct sockaddr_ib *)src;
			qib = (struct sockaddr_ib *)&ib_port->bind_sin;
			if (!memcmp(pib->sib_addr.sib_raw, qib->sib_addr.sib_raw,
						sizeof(pib->sib_addr.sib_raw))) {
				xttrace("Device %s, IB port %d: match source address %pI6\n",
					_dev->dev.ib_dev->name, ib_port->port,
					((struct sockaddr_ib *)src)->sib_addr.sib_raw);
				found = true;
				break;
			}
		}
	}
	FOUT;
	return found ? ib_port : NULL;
}

static int connect(void *p, struct request_base *r)
{
	struct connect_request *rr =
		container_of(r, struct connect_request, r);
	struct _rdma_dev *_dev = p;
	struct ib_port *ib_port;
	char bs[64];
	char bd[64];

	FIN;
	xttrace("Device %s: connect request from peer %s to %s\n",
		_dev->dev.ib_dev->name,
		x_tss(&rr->src, bs, sizeof(bs)), x_tss(&rr->dst, bd, sizeof(bd)));
	ib_port = find_port_by_addr(_dev, &rr->src);
	if (!ib_port) {
		xetrace("Device %s: failed to find port with source address %s\n",
			_dev->dev.ib_dev->name, x_tss(&rr->src, bs, sizeof(bs)));
		goto disconnect;
	}
	if (start_client(ib_port, rr)) {
		xetrace("Device %s: failed to alllocate memory for client of "
				"port %s\n",
			_dev->dev.ib_dev->name, x_tss(&rr->src, bs, sizeof(bs)));
		goto disconnect;
	}
	goto out;

disconnect:
	if (rr->client.ft.connect)
		rr->client.ft.connect(rr->client.client, NULL, NULL, 0);

out:
	FOUT;
	return 0;
}

static bool try_remove_client_from_path(
	struct ib_port *ib_port, void *client, struct _path *path)
{
	struct _client *c;
	bool found = false;

	FIN;
	list_for_each_entry(c, &path->clients, link) {
		if (c->ctx == client) {
			list_del_init(&c->link);
			kfree(c);
			if (list_empty(&path->clients))
				start_free_path(ib_port, path);
			found = true;
			break;
		}
	}
	FOUT;
	return found;
}

static bool try_remove_client(
	struct ib_port *ib_port, void *client, void *path)
{
	struct _path *p;
	bool found = false;

	FIN;
	list_for_each_entry(p, &ib_port->crp.all_paths, link) {
		if (p->owner_path == path) {
			found = true;
			goto on_found;
		}
	}
	if (!found) {
		list_for_each_entry(p, &ib_port->in_prog_conn, link) {
			if (p->owner_path == path) {
				found = true;
				goto on_found;
			}
		}

	}
	if (!found) {
		list_for_each_entry(p, &ib_port->in_prog_accept, link) {
			if (p->owner_path == path) {
				found = true;
				goto on_found;
			}
		}

	}
	if (!found) {
		list_for_each_entry(p, &ib_port->in_prog_disconn, link) {
			if (p->owner_path == path) {
				found = true;
				goto on_found;
			}
		}

	}
	goto out;

on_found:
	try_remove_client_from_path(ib_port, client, p);

out:
	FOUT;
	return found;
}

static bool try_remove_client_from_path_list(
	struct ib_port *ib_port, struct list_head *l, void *client)
{
	struct _path *p;
	bool found = false;

	FIN;
	list_for_each_entry(p, l, link)
		if (try_remove_client_from_path(ib_port, client, p)) {
			found = true;
			break;
		}
	FOUT;
	return found;
}

static bool try_remove_client_no_path(struct ib_port *ib_port, void *client)
{
	bool found;

	FIN;
	found =
		try_remove_client_from_path_list(
			ib_port, &ib_port->crp.all_paths, client) ||
		try_remove_client_from_path_list(
			ib_port, &ib_port->in_prog_conn, client) ||
		try_remove_client_from_path_list(
			ib_port, &ib_port->in_prog_accept, client) ||
		try_remove_client_from_path_list(
			ib_port, &ib_port->in_prog_disconn, client);
	FOUT;
	return found;
}

static int disconnect(void *p, struct request_base *r)
{
	struct disconnect_request *rr =
		container_of(r, struct disconnect_request, r);
	struct _rdma_dev *_dev = p;
	struct ib_port *ib_port;

	FIN;
	list_for_each_entry(ib_port, &_dev->port_list, link) {
		if (rr->path) {
			if (try_remove_client(ib_port, rr->client, rr->path))
				break;
		}
		else {
			if (try_remove_client_no_path(ib_port, rr->client))
				break;
		}
	}
	FOUT;
	complete(rr->c);
	return 0;
}

static int start_listener_on_port(struct ib_port *ib_port,
	struct server_request *rr)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	char bs[64];
	int status;

	FIN;
	if (ib_port->listener) {
		xetrace("Device %s: is already listening source address %s\n",
			ib_port->_dev->dev.ib_dev->name, x_tss(&rr->src, bs, sizeof(bs)));
		status = l_already_exist;
		goto out;
	}
	ib_port->listener = rdma_create_id(rdma_cm_handler,
		ib_port, ib_port->is_roce ? RDMA_PS_TCP : RDMA_PS_IB, IB_QPT_RC);
	if (ib_port->listener == NULL) {
		xetrace("Device %s port %d, fail to create rdma_cm_id\n",
			ib_dev->name, ib_port->port);
		status = l_start_fail;
		goto out;
	}
	else
		xdtrace("listener cm_id %p\n", ib_port->listener);
	if (rdma_bind_addr(
		ib_port->listener, (struct sockaddr *)&ib_port->bind_sin)) {
		xetrace("Device %s port %d, failed to call rdma_bind_addr()\n",
			ib_dev->name, ib_port->port);
		status = l_start_fail;
		goto out;
	}
	if (rdma_listen(ib_port->listener, 3)) {
		xetrace("Device %s port %d, failed to call rdma_listen()\n",
			ib_dev->name, ib_port->port);
		status = l_start_fail;
		goto out;
	}
	ib_port->server.info = rr->server;
	status = l_ok;

out:
	FOUT;
	return status;
}

static int listen(void *p, struct request_base *r)
{
	struct server_request *rr =
		container_of(r, struct server_request, r);
	struct _rdma_dev *_dev = p;
	struct ib_port *ib_port;
	char bs[64];
	int status;

	FIN;
	xttrace("Device %s: listen request on address %s\n",
		_dev->dev.ib_dev->name, x_tss(&rr->src, bs, sizeof(bs)));
	ib_port = find_port_by_addr(_dev, &rr->src);
	if (!ib_port) {
		xetrace("Device %s: failed to find port with source address %s\n",
			_dev->dev.ib_dev->name, x_tss(&rr->src, bs, sizeof(bs)));
		status = l_no_such_addr;
	}
	else if (!ib_port->ready) {
		xetrace("Device %s: source address %s is not ready\n",
			_dev->dev.ib_dev->name, x_tss(&rr->src, bs, sizeof(bs)));
		status = l_not_ready;
	}
	else
		status = start_listener_on_port(ib_port, rr);
	if (rr->server.ft.listen)
		rr->server.ft.listen(rr->server.server, &rr->src, status);
	FOUT;
	return 0;
}

static void stop_listener_on_port(struct ib_port *ib_port)
{
	FIN;
	if (ib_port->listener) {
		rdma_destroy_id(ib_port->listener);
		ib_port->listener = NULL;
	}
	FOUT;
}

static int listen_unreg(void *p, struct request_base *r)
{
	struct server_unreg_request *rr =
		container_of(r, struct server_unreg_request, r);
	struct _rdma_dev *_dev = p;
	struct ib_port *ib_port;
	char bs[64];

	FIN;
	xttrace("Device %s: unregister listen request on address %s\n",
		_dev->dev.ib_dev->name, x_tss(rr->src, bs, sizeof(bs)));
	ib_port = find_port_by_addr(_dev, rr->src);
	if (ib_port) {
		if (ib_port->server.info.server == rr->server) {
			stop_listener_on_port(ib_port);
			ib_port->server.info.server = NULL;
		}
	}
	complete(rr->c);
	FOUT;
	return 0;
}

static void free_port_test_data(struct ib_port *ib_port);
static void free_port(struct _rdma_dev *_dev, struct ib_port *ib_port)
{
	struct dev_per_cpu *d;

	FIN;
	x_ref_release_start(&ib_port->in_use);
	x_ref_release_wait(&ib_port->in_use);
	stop_listener_on_port(ib_port);
	while ((d = list_first_entry_or_null(
		&ib_port->pcpu_list, struct dev_per_cpu, link))) {
		manager_pcpu_rem_dev(_dev->owner, d->cpu, &d->rdma);
		x_ref_release_start(&d->in_use);
		x_ref_release_wait(&d->in_use);
		list_del_init(&d->link);
		free_pcpu(d);
		kfree(d);
	}
	free_paths(ib_port, &ib_port->crp);
	destroy_fr_pool(ib_port->pool);
	kfree(ib_port->tt);
	kfree(ib_port->rr);
	free_port_test_data(ib_port);
	kfree(ib_port);
	FOUT;
}

static const char * print_port_state(enum ib_port_state state)
{
	switch (state) {
	case IB_PORT_NOP: return "IB_PORT_NOP";
	case IB_PORT_DOWN: return "IB_PORT_DOWN";
	case IB_PORT_INIT: return "IB_PORT_INIT";
	case IB_PORT_ARMED: return "IB_PORT_ARMED";
	case IB_PORT_ACTIVE: return "IB_PORT_ACTIVE";
	case IB_PORT_ACTIVE_DEFER: return "IB_PORT_ACTIVE_DEFER";
	default: return "???";
	}
}

static int alloc_port_fr(struct ib_port *ib_port)
{
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	int max_pages_per_mr;
	int pool_size = PCPU_QP_MAX_SENDQ;
	bool p_retry;
	bool l_retry;

	FIN;
	while (!ib_port->pool && pool_size) {
		p_retry = false;
		for (max_pages_per_mr = _dev->max_pages_per_mr;
			 max_pages_per_mr >= FMR_MIN_SIZE;
			 max_pages_per_mr /= 2) {
			l_retry = false;
			ib_port->pool = create_fr_pool(_dev, max_pages_per_mr,
				pool_size * _dev->n_cores, &l_retry, &p_retry, "per_port");
			if (ib_port->pool) {
				_dev->max_pages_per_mr = max_pages_per_mr;
				xttrace("Devive %s, port %d, FR: max pages per mr %d\n",
					ib_dev->name, ib_port->port, _dev->max_pages_per_mr);
				break;
			}
			else if (p_retry || !l_retry)
				break;
		}
		pool_size /= 2;
	}
	FOUT;
	return ib_port->pool ? 0 : -1;
}

static struct ib_port * add_port(struct _rdma_dev *_dev, int port_num)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	enum rdma_link_layer ly;
	struct ib_port *ib_port = NULL;
	int rv;

	FIN;
	ib_port = kzalloc(sizeof(*ib_port), GFP_KERNEL);
	if (!ib_port) {
		xetrace("Failed to allocate ib_port %d for device %s\n",
			port_num, ib_dev->name);
		goto out;
	}
	else
		x_ref_init(&ib_port->in_use);
	if (!(ib_port->tt = kzalloc(sizeof(*ib_port->tt), GFP_KERNEL))) {
		xetrace("Failed to allocate internal data for ib_port\n");
		goto free_portt;
	}
	if (!(ib_port->rr = kzalloc(sizeof(*ib_port->rr), GFP_KERNEL))) {
		xetrace("Failed to allocate internal data for ib_port\n");
		goto free_portt;
	}
	ib_port->uuid = x_get_guid();
	ib_port->ready = false;
	ly = rdma_port_get_link_layer(ib_dev, port_num);
	ib_port->is_roce = ly == IB_LINK_LAYER_ETHERNET;
	INIT_LIST_HEAD(&ib_port->crp.all_paths);
	spin_lock_init(&ib_port->guard);
	INIT_LIST_HEAD(&ib_port->in_prog_accept);
	INIT_LIST_HEAD(&ib_port->in_prog_conn);
	INIT_LIST_HEAD(&ib_port->in_prog_disconn);
	INIT_LIST_HEAD(&ib_port->pcpu_list);
	init_completion(&ib_port->released);
	ib_port->_dev = _dev;
	ib_port->port = port_num;
	ib_port->gid_index = -1;
	if ((rv = ib_query_port(ib_dev, port_num, &ib_port->attr))) {
		xetrace("Failed to query port attributes on device %s:port %d: %d\n",
			ib_dev->name, port_num, rv);
		goto free_portt;
	}
	else if (ib_port->attr.state != IB_PORT_ACTIVE) {
		xetrace("Device %s: port %d: port is not active with "
				"current state %s\n",
			_dev->dev.ib_dev->name, port_num,
			print_port_state(ib_port->attr.state));
		goto free_portt;
	}
	if ((rv = find_gid(_dev, ib_port, NULL))) {
		xetrace("Failed to find GID on device %s:port %d: %d\n",
			ib_dev->name, port_num, rv);
		goto free_portt;
	}
	if (alloc_port_fr(ib_port))
		goto free_portt;
	if (alloc_per_port_per_cpu(_dev, ib_port))
		goto free_portt;
	goto out;

free_portt:
	free_port(_dev, ib_port);
	ib_port = NULL;

out:
	FOUT;
	return ib_port;
}

static int init_dev_ports(struct _rdma_dev *_dev)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_port *ib_port;
	int s, e, p;

	FIN;
	if (ib_dev->node_type == RDMA_NODE_IB_SWITCH) {
		s = 0;
		e = 0;
	}
	else {
		s = 1;
		e = get_device_phys_port_count(ib_dev);
	}
	xdtrace("Device %s: %p, s=%d, e=%d\n", ib_dev->name, ib_dev, s, e);
	for (p = s; p <= e; ++p) {
		ib_port = add_port(_dev, p);
		if (ib_port) {
			list_add_tail(&ib_port->link, &_dev->port_list);
			if (/*start_dummy_server(ib_port) || start_dummy_client(ib_port)*/
				post_dummy_test(_dev, ib_port->port)) {
				list_del_init(&ib_port->link);
				free_port(_dev, ib_port);
			}
			else
				ib_port->dummy_test_in_progress = true;
		}
	}
	FOUT;
	return 0;
}

static int on_port_active(struct _rdma_dev *_dev, int port_num)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_port *ib_port;
	int rv;

	FIN;
	if (!((ib_port = find_port_by_num(_dev, port_num)))) {
		xttrace("Device %s, trying to add port %d\n", ib_dev->name, port_num);
		if ((ib_port = add_port(_dev, port_num))) {
			xttrace("Device %s, adding port %d\n", ib_dev->name, port_num);
			list_add_tail(&ib_port->link, &_dev->port_list);
			rv = 0;
		}
		else {
			xetrace("Device %s, failed to add port %d\n",
				ib_dev->name, port_num);
			rv = -1;
		}
	}
	else
		rv = 0;
	FOUT;
	return rv;
}

static int on_port_error(struct _rdma_dev *_dev, int port_num)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_port *ib_port;

	FIN;
	if ((ib_port = find_port_by_num(_dev, port_num))) {
		xttrace("Device %s, removing port %d\n", ib_dev->name, port_num);
		list_del_init(&ib_port->link);
		free_port(_dev, ib_port);
	}
	else
		xttrace("Device %s, port %d was not found\n", ib_dev->name, port_num);
	FOUT;
	return 0;
}

static int handle_acync_event(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_event_request *rr =
		container_of(r, struct ib_event_request, r);

	FIN;
	xttrace("Device %s: handling async_event %s\n",
		ib_dev->name, ib_event_msg(rr->event.event));
	switch (rr->event.event) {
	case IB_EVENT_CQ_ERR:
	case IB_EVENT_QP_FATAL:
	case IB_EVENT_QP_REQ_ERR:
	case IB_EVENT_QP_ACCESS_ERR:
	case IB_EVENT_COMM_EST:
	case IB_EVENT_SQ_DRAINED:
	case IB_EVENT_PATH_MIG:
	case IB_EVENT_PATH_MIG_ERR:
	case IB_EVENT_DEVICE_FATAL:
		break;
	case IB_EVENT_PORT_ACTIVE:
		on_port_active(_dev, rr->event.element.port_num);
		break;
	case IB_EVENT_PORT_ERR:
		on_port_error(_dev, rr->event.element.port_num);
		break;
	case IB_EVENT_LID_CHANGE:
	case IB_EVENT_PKEY_CHANGE:
	case IB_EVENT_SM_CHANGE:
		on_port_error(_dev, rr->event.element.port_num);
		on_port_active(_dev, rr->event.element.port_num);
		break;
	case IB_EVENT_SRQ_ERR:
	case IB_EVENT_SRQ_LIMIT_REACHED:
	case IB_EVENT_QP_LAST_WQE_REACHED:
		break;
	case IB_EVENT_CLIENT_REREGISTER:
	case IB_EVENT_GID_CHANGE:
		on_port_error(_dev, rr->event.element.port_num);
		on_port_active(_dev, rr->event.element.port_num);
		break;
	case IB_EVENT_WQ_FATAL:
#ifndef NO_OFED
	case IB_EXP_EVENT_DCT_KEY_VIOLATION:
	case IB_EXP_EVENT_DCT_ACCESS_ERR:
	case IB_EXP_EVENT_DCT_REQ_ERR:
	case IB_EXP_EVENT_XRQ_QP_ERR:
	case IB_EXP_EVENT_XRQ_NVMF_BACKEND_CTRL_ERR:
#endif
		break;
	default:
		break;
	}
	FOUT;
	return 0;
}

static void async_event_handler(
	struct ib_event_handler *event_handler, struct ib_event *event)
{
	struct _rdma_dev *_dev = container_of(
		event_handler, struct _rdma_dev, event_handler);
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_event_request *rr;

	FIN;
	xttrace("Device %s: received async_event %s\n",
		ib_dev->name, ib_event_msg(event->event));
	if ((rr = kzalloc(sizeof(*rr), GFP_KERNEL))) {
		rr->r.type = rpcrdma_ib_event;
		rr->r.free = ib_event_request_free;
		rr->r.call = handle_acync_event;
		rr->event = *event;
		wth_push_request(_dev->wth, &rr->r);
	}
	else
		xetrace("Device %s: failed to alloc request for async_event %s\n",
			ib_dev->name, ib_event_msg(event->event));

	FOUT;
}

static void init_fast_reg(struct _rdma_dev *_dev)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	int mr_page_shift;
	u64 max_pages_per_mr;

	FIN;
	/*
	 * Use the smallest page size supported by the HCA, down to a
	 * minimum of 4096 bytes. We're unlikely to build large sglists
	 * out of smaller entries.
	 */
	mr_page_shift = max(12, ffs(_dev->dev_attr.page_size_cap) - 1);
	_dev->mr_page_size = 1 << mr_page_shift;
	_dev->mr_page_mask = ~(((u64)_dev->mr_page_size) - 1);
	max_pages_per_mr = _dev->dev_attr.max_fast_reg_page_list_len;
	_dev->max_pages_per_mr =
		max_t(u64, min_t(u64, FMR_SIZE, max_pages_per_mr), FMR_MIN_SIZE);
	_dev->mr_max_size = (u64)_dev->mr_page_size * _dev->max_pages_per_mr;
	xttrace("Device %s: "
			"mr_page_shift %d, "
			"dev_attr.max_fast_reg_page_list_len %d, "
			"max_pages_per_mr %d, "
			"mr_max_size %lld\n", ib_dev->name,
		mr_page_shift, _dev->dev_attr.max_fast_reg_page_list_len,
		_dev->max_pages_per_mr, _dev->mr_max_size);
	FOUT;
}

static void count_dev_core(struct _rdma_dev *_dev)
{
	int node = _dev->dev.ib_dev->dma_device->numa_node;
	int cpu;

	FIN;
	for_each_online_cpu(cpu)
		_dev->n_cores += cpu_to_node(cpu) == node ? 1 : 0;
	xttrace("Device %s is on NUMA node %d with %d cores\n",
		_dev->dev.ib_dev->name, node, _dev->n_cores);
	FOUT;
}

/* initializing new IB device wrapper */
static int init_dev(struct _rdma_dev *_dev)
{
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	int rv = 0;

	FIN;
	if ((rv = ib_query_device(ib_dev, &_dev->dev_attr))) {
		xetrace("Query device %s failed (%d)\n", ib_dev->name, rv);
		goto out;
	}

	_dev->num_comp_vectors = min_t(int, num_online_cpus(),
		ib_dev->num_comp_vectors);
	if (!_dev->num_comp_vectors) {
		xetrace("Device %s has 0 completion_vectors - will no set it to 8\n",
			ib_dev->name);
		_dev->num_comp_vectors = 8;
	}

	_dev->pd = ib_alloc_pd(ib_dev);
	if (IS_ERR(_dev->pd)) {
		rv = PTR_ERR(_dev->pd);
		xetrace("ib_alloc_pd for device %s failed (%d)\n",
			ib_dev->name, rv);
		goto out;
	}

	_dev->mr = 
#ifdef NO_OFED
	_dev->pd->device->get_dma_mr(
#else
	ib_get_dma_mr(
#endif
		_dev->pd,
		IB_ACCESS_LOCAL_WRITE |
		IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE);
	if (IS_ERR(_dev->mr)) {
		rv = PTR_ERR(_dev->mr);
		xetrace("ib_get_dma_mr for device %s failed (%d)\n",
			ib_dev->name, rv);
		goto err_pd;
	}
	INIT_LIST_HEAD(&_dev->port_list);
	INIT_LIST_HEAD(&_dev->addr_reg_list);
	count_dev_core(_dev);
	if (!_dev->n_cores) {
		xetrace("Device %s has no CPU cores to use - device will be removed\n",
			ib_dev->name);
		rv = -1;
		goto err_pd;
	}
	xttrace("Device %s has %d core and %d comp_vectors\n",
		ib_dev->name, _dev->n_cores, ib_dev->num_comp_vectors);
	_dev->srq_size = _dev->dev_attr.max_srq_wr;
	xdtrace("SRQ size for device %s is  %d, msg_size %d\n",
		ib_dev->name, _dev->srq_size, _dev->srq_msg_size);
	INIT_IB_EVENT_HANDLER(&_dev->event_handler, ib_dev, async_event_handler);
	ib_register_event_handler(&_dev->event_handler);
	init_fast_reg(_dev);
	rv = 0;
	goto out;

err_pd:
	ib_dealloc_pd(_dev->pd);
	_dev->pd = NULL;

out:
	FOUT;
	return rv;
}

static void on_start(void *v)
{
	struct _rdma_dev *_dev = v;

	FIN;
	if (init_dev(_dev) || init_dev_ports(_dev))
		wth_send_stop(_dev->wth);
	FOUT;
}

static void free_ports(struct _rdma_dev *_dev)
{
	struct ib_port *ib_port;

	FIN;
	while ((ib_port = list_first_entry_or_null(
		&_dev->port_list, struct ib_port, link))) {
		list_del_init(&ib_port->link);
		free_port(_dev, ib_port);
	}
	FOUT;
}

static void dev_free(struct _rdma_dev *_dev)
{
	struct addr_reg *i;

	FIN;
	while ((i = list_first_entry_or_null(
		&_dev->addr_reg_list, struct addr_reg, link))) {
		list_del(&i->link);
		kfree(i);
	}
	if (_dev->event_handler.device) {
		ib_unregister_event_handler(&_dev->event_handler);
		memset(&_dev->event_handler, 0, sizeof(_dev->event_handler));
	}
	free_ports(_dev);
	if (_dev->mr) {
		ib_dereg_mr(_dev->mr);
		_dev->mr = NULL;
	}
	if (_dev->pd) {
		ib_dealloc_pd(_dev->pd);
		_dev->pd = NULL;
	}
	FOUT;
}

static void on_exit(void *v)
{
	struct _rdma_dev *_dev = v;

	FIN;
	dev_free(_dev);
	manager_rdma_dev_exit(_dev->owner, &_dev->dev);
	FOUT;
}

struct rdma_dev * rdma_dev_create(struct rdma_dev_info *p)
{
	struct _rdma_dev *_dev;
	struct rdma_dev *dev = NULL;
	struct wth_info info = {0};
	char name[80] = {0};

	FIN;
	BUILD_BUG_ON(sizeof(struct x_cm_payload) > CM_PAYLOAD_SIZE);
	if (!(_dev = kzalloc(sizeof(*_dev), GFP_KERNEL))) {
		xetrace("Failed to allocate RDMA device\n");
		goto out;
	}
	_dev->owner = p->owner;
	_dev->dev.ib_dev = p->ib_dev;
	_dev->srq_msg_size = p->max_receive_msg_size;
	snprintf(name, sizeof(name), "rdma_dev_%s", _dev->dev.ib_dev->name);
	info.name = name;
	info.owner = _dev;
	info.on_start = on_start;
	info.on_exit = on_exit;
	if (!(_dev->wth = wth_create(&info))) {
		xetrace("Failed to create rdma_dev_%s worker_thread\n",
			_dev->dev.ib_dev->name);
		goto freed;
	}
	dev = &_dev->dev;
	goto out;

freed:
	kfree(_dev);
	_dev = NULL;

out:
	FOUT;
	return dev;
}

void rdma_dev_stop(struct rdma_dev *d)
{
	struct _rdma_dev *_dev = container_of(d, struct _rdma_dev, dev);
	wth_send_stop(_dev->wth);
}

void rdma_dev_free(struct rdma_dev *d)
{
	struct _rdma_dev *_dev = container_of(d, struct _rdma_dev, dev);

	FIN;
	wth_free(_dev->wth);
	kfree(_dev);
	FOUT;
}

void rdma_dev_per_core_rdma_free(struct per_core_rdma *rdma)
{
	struct dev_per_cpu *d = container_of(rdma, struct dev_per_cpu, rdma);

	FIN;
	xetrace("Should never happened - all entries here must be cleared "
			"during the device unregister callback\n");
	kfree(d);
	FOUT;
}

bool rdma_dev_push_request(struct rdma_dev *d, struct request_base *r)
{
	struct _rdma_dev *_dev = container_of(d, struct _rdma_dev, dev);
	if (!wth_is_current(_dev->wth)) {
		wth_push_request(_dev->wth, r);
		return true;
	}
	return false;
}

int rdma_dev_connect(struct rdma_dev *d, struct client_info *client,
	struct sockaddr_storage *src, struct sockaddr_storage *dst)
{
	struct connect_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_connect;
		r->r.call = connect;
		r->r.free = connect_request_free;
		r->src = *src;
		r->dst = *dst;
		r->client = *client;
		if (!rdma_dev_push_request(d, &r->r))
			BUG();
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

int rdma_dev_register_server(struct rdma_dev *d,
	struct server_info *server, struct sockaddr_storage *src)
{
	struct server_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_server;
		r->r.call = listen;
		r->r.free = server_request_free;
		r->src = *src;
		r->server = *server;
		if (!rdma_dev_push_request(d, &r->r))
			BUG();
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

int rdma_dev_unregister_server(struct rdma_dev *d,
	void *server, struct sockaddr_storage *src)
{
	struct server_unreg_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_server_unreg;
		r->r.call = listen_unreg;
		r->r.free = server_unreg_request_free;
		r->server = server;
		r->src = src;
		r->c = &comp;
		if (rdma_dev_push_request(d, &r->r))
			wait_for_completion(&comp);
		else {
			listen_unreg(container_of(d, struct _rdma_dev, dev), &r->r);
			kfree(r);
		}
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

static int distribute_per_cpu(struct ib_port *ib_port)
{
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct dev_per_cpu *cur_cpu;
	int node = _dev->dev.ib_dev->dma_device->numa_node;
	int n = num_online_cpus();
	int i, cpu = 0;
	int found;
	struct {int cpu; int n; bool node;} *cores = NULL;
	int rv;

	FIN;
	if (!(cores = kzalloc(sizeof(*cores) * n, GFP_KERNEL))) {
		xetrace("Device %s, port %d: failed to alloc ate memory\n",
			ib_dev->name, ib_port->port);
		rv = -1;
		goto out;
	}
	i = 0;
	for_each_online_cpu(cpu)
		if (i < n) {
			cores[i].cpu = cpu;
			if (cpu_to_node(cpu) == node)
				cores[i].node = true;
			++i;
		}
	list_for_each_entry(cur_cpu, &ib_port->pcpu_list, link)
		for (i = 0; i < n; ++i) {
			if (cores[i].cpu == cur_cpu->cpu) {
				++cores[i].n;
				break;
			}
		}
	found = 0;
	for (i = 0; i < n; ++i)
		if (cores[i].node) {
			if (cores[i].n > 1) {
				++found;
				xwtrace("Device %s, port %d: core %d of CPU %d has %d CQ\n",
					ib_dev->name, ib_port->port,
					cores[i].cpu, node,
					cores[i].n);
			}
			else if (cores[i].n < 1) {
				++found;
				xwtrace("Device %s, port %d: core %d of CPU %d has 0 CQ\n",
					ib_dev->name, ib_port->port, cores[i].cpu, node);
			}
		}
	if (found)
		xwtrace("Device %s, port %d: CQ distribution is bad - "
				"please run `mlnx_affinity`\n", ib_dev->name, ib_port->port);
	list_for_each_entry(cur_cpu, &ib_port->pcpu_list, link)
		if ((rv = manager_pcpu_add_dev(
			_dev->owner, cur_cpu->cpu, &cur_cpu->rdma, true)))
			goto out;
		else
			cur_cpu->pcpu = manager_get_services_cpu(
				_dev->owner, cur_cpu->cpu);
	i = 0;
	for_each_online_cpu(cpu) {
		if (i < n) {
			if (cores[i].n < 1) {
				cur_cpu = list_first_entry(
					&ib_port->pcpu_list, struct dev_per_cpu, link);
				list_del(&cur_cpu->link);
				rv = manager_pcpu_add_dev(
					_dev->owner, cpu, &cur_cpu->rdma, false);
				list_add_tail(&cur_cpu->link, &ib_port->pcpu_list);
				if (rv)
					goto out;
			}
			++i;
		}
	}
	ib_port->ready = true;

out:
	kfree(cores);
	FOUT;
	return rv;
}

static void announce_new_port(struct _rdma_dev *_dev, struct ib_port *ib_port)
{
	struct addr_reg *a;
	struct local_address_info ai;

	FIN;
	ai.a = &ib_port->bind_sin;
	ai.local_rdma_dev = _dev;
	list_for_each_entry(a, &_dev->addr_reg_list, link)
		if (a->f)
			a->f(a->ctx, &ai);
	FOUT;
}

static void free_port_test_data(struct ib_port *ib_port)
{
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;

	FIN;
	if (ib_port->ps.sg) {
		kfree(ib_port->ps.sg);
		ib_port->ps.sg = NULL;
		ib_port->ps.nents = 0;
	}
	if (ib_port->buf) {
		ib_dma_free_coherent(ib_dev, PAGE_SIZE, ib_port->buf, ib_port->dma);
		ib_port->buf = NULL;
		ib_port->dma = 0;
	}
	ib_port->ps.send_comp = NULL;
	ib_port->cur_cpu = NULL;
	FOUT;
}

static void run_dummy_test_for_port(
	struct ib_port *ib_port, struct dev_per_cpu *cur_cpu);
static int handle_test_completion(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct dummy_test_comp_request *rr =
		container_of(r, struct dummy_test_comp_request, r);
	struct dev_per_cpu *cur_cpu = rr->cur_cpu;
	int port_num = rr->port_num;
	struct ib_port *ib_port;

	FIN;
	if ((ib_port = find_port_by_num(_dev, port_num))) {
		rdma_dev_put_send_resource(ib_port->ps.rsc);
		if (rr->wc.status == IB_WC_SUCCESS) {
			xttrace("Device %s, port %d: test for cpu %d was OK\n",
				ib_dev->name, port_num, cur_cpu->cpu);
			if (ib_port->dummy_test_in_progress) {
				if (!list_is_last(&cur_cpu->link, &ib_port->pcpu_list)) {
					cur_cpu = list_next_entry(cur_cpu, link);
					run_dummy_test_for_port(ib_port, cur_cpu);
				}
				else {
					free_port_test_data(ib_port);
					if (!distribute_per_cpu(ib_port))
						announce_new_port(_dev, ib_port);
					else
						on_port_error(_dev, ib_port->port);
				}
			}
		}
		else {
			free_port_test_data(ib_port);
			xttrace("Device %s, port %d: test for cpu %d "
					"failed with error %s\n",
				ib_dev->name, port_num, cur_cpu->cpu,
				ib_wc_status_msg(rr->wc.status));
		}
	}
	else
		xttrace("Device %s, no port_num %d\n", ib_dev->name, port_num);
	FOUT;
	return 0;
}

static void dummy_test_completion(void *ctx, struct ib_wc *wc)
{
	struct dummy_test_comp_request *r = ctx;
	struct _rdma_dev *_dev = r->cur_cpu->ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;

	FIN;
	xttrace("Device %s on node %d, port %d: "
			"per_cpu %d assigned_vector %d "
			"has completion interrupt on core #%d\n",
		ib_dev->name, ib_dev->dma_device->numa_node, r->cur_cpu->ib_port->port,
		r->cur_cpu->cq->intr_vector,
		r->cur_cpu->cpu, raw_smp_processor_id());
	r->cur_cpu->cpu = raw_smp_processor_id();
	r->wc = *wc;
	wth_push_request(_dev->wth, &r->r);
	x_ref_put(&r->cur_cpu->ib_port->in_use);
	FOUT;
}

static int run_dummy_test_for_cpu(struct dev_per_cpu *cur_cpu)
{
	struct ib_port *ib_port = cur_cpu->ib_port;
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct dummy_test_comp_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.call = handle_test_completion;
		r->r.free = dummy_test_comp_request_request_free;
		r->r.type = rpcrdma_dummy_test_comp;
		r->cur_cpu = cur_cpu;
		r->port_num = ib_port->port;
		ib_port->ps.ctx = r;
		rv = rdma_dev_post_send(&ib_port->ps, NULL, &cur_cpu->rdma, NULL);
	}
	else {
		xdtrace("Device %s, port %d: "
				"per_cpu %d: failed to allocate completion message\n",
			ib_dev->name, ib_port->port, cur_cpu->cpu);
		rv = -ENOMEM;
	}
	FOUT;
	return rv;
}

static void run_dummy_test_for_port(
	struct ib_port *ib_port, struct dev_per_cpu *cur_cpu)
{
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	void *buf;
	dma_addr_t dma;
	struct scatterlist *sg;

	FIN;
	xttrace("Running test for cpu %d\n", cur_cpu->cpu);
	if (!x_ref_get(&ib_port->in_use)) {
		xetrace("Device %s: port %d is about to be removed\n",
			ib_dev->name, ib_port->port);
		goto out;
	}
	if (!(buf = ib_dma_alloc_coherent(ib_dev, PAGE_SIZE, &dma, GFP_KERNEL))) {
		xetrace("Device %s: failed to allocate page for dummy test\n",
			ib_dev->name);
		goto out;
	}
	if (!(sg = kzalloc(sizeof(*sg), GFP_KERNEL))) {
		xetrace("Device %s: failed to allocate SG for dummy test\n",
			ib_dev->name);
		goto freep;
	}
	sg_set_buf(sg, buf, PAGE_SIZE);
	sg_mark_end(sg);
	ib_port->buf = buf;
	ib_port->dma = dma;
	ib_port->ps.sg = sg;
	ib_port->ps.nents = 1;
	ib_port->ps.opcode = __IB_WR_REG_MR;
	ib_port->ps.send_comp = dummy_test_completion;
	ib_port->cur_cpu = cur_cpu;
	if (run_dummy_test_for_cpu(ib_port->cur_cpu)) {
		xetrace("Device %s: failed to start dummy test for cpu %d\n",
			ib_dev->name, ib_port->cur_cpu->cpu);
		goto freesg;
	}
	goto out;

freesg:
	kfree(sg);

freep:
	free_port_test_data(ib_port);
	x_ref_put(&ib_port->in_use);

out:
	FOUT;
}

static int dummy_test(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct dummy_test_request *rr =
		container_of(r, struct dummy_test_request, r);
	int port_num = rr->port_num;
	struct ib_port *ib_port;

	FIN;
	if ((ib_port = find_port_by_num(_dev, port_num)))
		run_dummy_test_for_port(ib_port,
			list_first_entry(&ib_port->pcpu_list, struct dev_per_cpu, link));
	else
		xttrace("Device %s, port %d was not found\n",
			ib_dev->name, rr->port_num);
	FOUT;
	return 0;
}

bool rdma_dev_is_path_port(struct _path *path, struct per_core_rdma *d)
{
	struct dev_per_cpu *dd = container_of(d, struct dev_per_cpu, rdma);
	return path->ib_port == dd->ib_port;
}

static void add_addr_reg(struct _rdma_dev *_dev, struct address_request *r)
{
	struct addr_reg *a;
	struct ib_port *ib_port;
	struct local_address_info ai;

	FIN;
	if ((a = kzalloc(sizeof(*a), GFP_KERNEL))) {
		a->f = r->f;
		a->ctx = r->ctx;
		list_add_tail(&a->link, &_dev->addr_reg_list);
		list_for_each_entry(ib_port, &_dev->port_list, link) {
			ai.a = &ib_port->bind_sin;
			ai.local_rdma_dev = &_dev->dev;
			a->f(a->ctx, &ai);
		}
	}
	FOUT;
}

static void del_addr_reg(struct _rdma_dev *_dev, struct address_request *r)
{
	struct addr_reg *a;
	bool found = false;

	FIN;
	list_for_each_entry(a, &_dev->addr_reg_list, link)
		if (a->ctx == r->ctx) {
			found = true;
			break;
		}
	if (found) {
		list_del_init(&a->link);
		kfree(a);
	}
	FOUT;
}

static int register_address(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct address_request *rr = container_of(r, struct address_request, r);

	FIN;
	if (rr->f)
		add_addr_reg(_dev, rr);
	else
		del_addr_reg(_dev, rr);
	FOUT;
	return 0;
}

int rdma_dev_register_address(struct rdma_dev *d,
	void (*f)(void *ctx, struct local_address_info *a), void *ctx)
{
	struct address_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_address;
		r->r.call = register_address;
		r->r.free = address_request_free;
		r->f = f;
		r->ctx = ctx;
		if (!rdma_dev_push_request(d, &r->r))
			BUG();
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

static int map_one_fr(struct _rdma_dev *_dev, struct dev_per_cpu *d,
	struct scatterlist *sg, int sg_nents, struct io_req *s)
{
	struct ib_port *ib_port = d->ib_port;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct fr_desc *fr;
	unsigned int sg_offset = 0;
	struct ib_send_wr *wr1;
	struct ib_reg_wr *wr2;
	u32 rkey;
	int n;
	int rv;

	FIN;
	if (!(fr = get_frd(d->pool))) {
		xetrace("Devive %s, port %d, cpu %d: failed to get FR\n",
			ib_dev->name, ib_port->port, d->cpu);
		rv = -ENOMEM;
		goto out;
	}
	if (!fr->valid) {
		wr1 = &s->inval[s->n_inval++];
		memset(wr1, 0, sizeof(*wr1));
		wr1->wr_id = s->send.wr.wr.wr_id;
		wr1->opcode = IB_WR_LOCAL_INV;
		wr1->ex.invalidate_rkey = fr->mr->rkey;
	}
	rkey = ib_inc_rkey(fr->mr->rkey);
	ib_update_fast_reg_key(fr->mr, rkey);
	n = ib_map_mr_sg(fr->mr, sg, sg_nents, &sg_offset, _dev->mr_page_size);
	if (unlikely(n < 0)) {
		put_frd(d->pool, &fr, 1);
		xetrace("Devive %s, port %d, cpu %d: ib_map_mr_sg() failed with %d\n",
			ib_dev->name, ib_port->port, d->cpu, n);
		rv = n;
		goto out;
	}
	s->fr_desc[s->n_map] = fr;
	wr2 = &s->mrmap[s->n_map++];
	memset(wr2, 0, sizeof(*wr2));
	wr2->wr.wr_id = s->send.wr.wr.wr_id;
	wr2->wr.opcode = IB_WR_REG_MR;
	wr2->mr = fr->mr;
	wr2->key = fr->mr->rkey;
	wr2->access = (IB_ACCESS_LOCAL_WRITE |
				   IB_ACCESS_REMOTE_READ |
				   IB_ACCESS_REMOTE_WRITE);
	fr->valid = false;
	rv = n;

out:
	FOUT;
	return rv;
}

static int map_sg(
	struct dev_per_cpu *d, struct post_send_info *ps, struct io_req *s)
{
	struct ib_port *ib_port = d->ib_port;
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	enum dma_data_direction dir;
	struct scatterlist *sg;
	int count;
	int i, n, rv;

	FIN;
	s->n_inval = s->n_map = s->n_rdma = 0;
	if (!ps->sg) {
		rv = 0;
		goto out;
	}
	if (ps->opcode & (__IB_WR_RDMA_READ | __IB_WR_RDMA_READ_WITH_INV)) {
		if (ps->opcode & (__IB_WR_RDMA_WRITE | __IB_WR_RDMA_WRITE_WITH_IMM))
			dir = DMA_BIDIRECTIONAL;
		else
			dir = DMA_FROM_DEVICE;
	}
	else
		dir = DMA_TO_DEVICE;
	count = ib_dma_map_sg(ib_dev, ps->sg, ps->nents, dir);
	if (unlikely(count == 0)) {
		rv = -EIO;
		goto out;
	}
	sg = ps->sg;
	s->n_inval = s->n_map = s->n_rdma = 0;
	while (count) {
		if ((n = map_one_fr(_dev, d, sg, count, s)) < 0) {
			rv = -1;
			goto unmap;
		}
		count -= n;
		for (i = 0; i < n; i++)
			sg = sg_next(sg);
	}
	for (i = 1; i < s->n_inval; ++i) {
		s->inval[i - 1].next = &s->inval[i];
		s->inval[i].next = NULL;
	}
	for (i = 1; i < s->n_map; ++i) {
		s->mrmap[i - 1].wr.next = &s->mrmap[i].wr;
		s->mrmap[i].wr.next = NULL;
	}
	if (s->n_inval && s->n_map)
		s->inval[s->n_inval - 1].next = &s->mrmap[0].wr;
	s->sg = ps->sg;
	s->nents = ps->nents;
	s->dir = dir;
	rv = 0;
	goto out;

unmap:
	ib_dma_unmap_sg(ib_dev, ps->sg, ps->nents, dir);

out:
	FOUT;
	return rv;
}

static struct ib_send_wr * build_rdma_write(struct io_req *s, u64 raddr,
	u32 rkey, struct ib_ah *ah, u64 dct_access_key, u32 dct_number,
	struct ib_send_wr **e)
{
	struct ib_send_wr *start = NULL;
	struct ib_send_wr *end = NULL;
	struct fr_desc *fr;
	struct io_wr *dc;
	u64 addr = raddr;
	int i;

	FIN;
	if (s->n_map > 0) {
		start = s->n_inval ? &s->inval[0] : &s->mrmap[0].wr;
		end = &s->mrmap[s->n_map - 1].wr;
		for (i = 0; i < s->n_map; ++i) {
			fr = s->fr_desc[i];
			dc = &s->rdma[i];
			memset(dc, 0, sizeof(*dc));
			dc->sg.addr = fr->mr->iova;
			dc->sg.length = fr->mr->length;
			dc->sg.lkey = fr->mr->lkey;
			dc->wr.wr.wr.wr_id = s->send.wr.wr.wr_id;
			dc->wr.wr.wr.sg_list = &dc->sg;
			dc->wr.wr.wr.num_sge = 1;
			dc->wr.wr.wr.opcode = IB_WR_RDMA_WRITE;
			dc->wr.wr.remote_addr = addr;
			dc->wr.wr.rkey = rkey;
			dc->wr.ah = ah;
			dc->wr.dct_access_key = dct_access_key;
			dc->wr.dct_number = dct_number;
			end->next = &dc->wr.wr.wr;
			end = &dc->wr.wr.wr;
			addr += dc->sg.length;
		}
		s->n_rdma = s->n_map;
	}
	*e = end;
	FOUT;
	return start;
}

static int build_table(struct io_req *s, int table_offset)
{
	struct ib_sge *sg = s->msg + table_offset;
	struct fr_desc *fr;
	int i, rv;

	FIN;
	if ((void *)sg + sizeof(sg) * s->n_map <=
		s->msg + s->w.pcpu->ib_port->_dev->srq_msg_size) {
		for (i = 0; i < s->n_map; ++i) {
			fr = s->fr_desc[i];
			xdtrace("%d: iova=%#llx, length=%lld, rkey=%#x\n",
				i, fr->mr->iova, fr->mr->length, fr->mr->rkey);
			sg[i].addr = cpu_to_be64(fr->mr->iova);
			sg[i].length = cpu_to_be32(fr->mr->length);
			sg[i].lkey = cpu_to_be32(fr->mr->rkey);
		}
		rv = s->n_map * sizeof(*sg);
	}
	else {
		xetrace("cpu %d: memory descriptor table is larger "
				"than message area\n", s->w.pcpu->cpu);
		rv = -1;
	}
	FOUT;
	return rv;
}

int rdma_dev_post_send(struct post_send_info *ps, void *path,
	struct per_core_rdma *d, int *index_remote_dct)
{
	struct dev_per_cpu *dd = container_of(d, struct dev_per_cpu, rdma);
	struct ib_port *ib_port = dd->ib_port;
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct _path *p = path;
	u32 dct_num;
	int n;
	struct io_req *io_req;
	int table_offset;
	struct ib_send_wr *start = NULL;
	struct ib_send_wr *end = NULL;
	int msg_len;
	int len;
	int rv = -1;

	FIN;
	if (!(io_req = get_io_req(dd))) {
		xetrace("Failed to get io_req from cpu %d\n", dd->cpu);
		rv = -ENOMEM;
		goto out;
	}
	io_req->send_comp = ps->send_comp;
	io_req->ctx = ps->ctx;
	if (map_sg(dd, ps, io_req))
		goto freeioreq;
	if (ps->opcode != __IB_WR_REG_MR) {
		n = *index_remote_dct;
		if (n >= p->n_dct_info)
			n = 0;
		dct_num = p->remote_dct_info[n++].dct_num;
		if (n >= p->n_dct_info)
			n = 0;
		*index_remote_dct = n;
		if (ps->raddr)
			start = build_rdma_write(io_req, ps->raddr, ps->rkey, p->ah,
				p->remote_dct_key, dct_num, &end);
		if (ps->prepare_send) {
			msg_len = 0;
			if ((table_offset = ps->prepare_send(ps->ctx,
				io_req->msg, dd->ib_port->_dev->srq_msg_size,
				io_req->n_rdma, &msg_len)) > 0) {
				len = build_table(io_req, table_offset);
				if (len < 0)
					goto freeioreq;
				else
					msg_len += len;
			}
			else if (table_offset < 0)
				goto freeioreq;
			fill_msg_hdr(io_req->hdr,
				ps->src, cpu_to_be32(dd->dct->dct_num),
				ps->dst, cpu_to_be32(dct_num));
			sync_io_req(_dev, io_req);
			io_req->send.wr.wr.sg_list[1].length = msg_len;
			io_req->send.wr.wr.send_flags =
				ps->send_comp ? IB_SEND_SIGNALED : 0;
			io_req->send.ah = p->ah;
			io_req->send.dct_access_key = p->remote_dct_key;
			io_req->send.dct_number = dct_num;
			if (start)
				end->next = &io_req->send.wr.wr;
			else
				start = &io_req->send.wr.wr;
			end = &io_req->send.wr.wr;
		}
	}
	else {
		if (io_req->n_inval && io_req->n_map) {
			start = &io_req->inval[0];
			end = &io_req->mrmap[io_req->n_map - 1].wr;
			end->send_flags = io_req->send_comp ? IB_SEND_SIGNALED : 0;
		}
		else if (io_req->n_inval) {
			start = &io_req->inval[0];
			end = &io_req->inval[io_req->n_inval - 1];
			end->send_flags = io_req->send_comp ? IB_SEND_SIGNALED : 0;
		}
		else if (io_req->n_map) {
			start = &io_req->mrmap[0].wr;
			end = &io_req->mrmap[io_req->n_map - 1].wr;
			end->send_flags = io_req->send_comp ? IB_SEND_SIGNALED : 0;
		}
	}
	if (start && end) {
		end->next = NULL;
		xdtrace("wr_id %lld\n", end->wr_id);
		ps->rsc = io_req;
		if ((rv = ib_post_send(dd->qp, start, NULL))) {
			xetrace("Device %s, port %d: post_send failed\n",
					ib_dev->name, ib_port->port);
			rv = -1;
			ps->rsc = NULL;
			goto freeioreq;
		}
	}
	else {
		rv = -1;
		goto freeioreq;
	}
	rv = 0;
	goto out;

freeioreq:
	put_io_req(io_req);

out:
	FOUT;
	return rv;
}

int rdma_dev_put_send_resource(void *rsc)
{
	put_io_req(rsc);
	return 0;
}

int rdma_dev_put_recv_resource(void *rsc, int index)
{
	put_registrant_recv(rsc, index);
	return 0;
}

static inline unsigned long mem_num_pages(
	unsigned long address, unsigned long length)
{
	return (ALIGN(address + length, PAGE_SIZE) -
			ALIGN_DOWN(address, PAGE_SIZE)) >> PAGE_SHIFT;
}

static struct scatterlist * add_sg_table(struct scatterlist *sg,
	struct page **page_list, unsigned long npages, unsigned int max_seg_sz,
	int *nents)
{
	/* the physical page in the Linux notattion the page frame number */
	unsigned long first_pfn;
	unsigned long i = 0;
	bool update_cur_sg = false;
	bool first = !sg_page(sg);
	unsigned long len;
	struct page *first_page;
	unsigned max_pages_in_segment = max_seg_sz >> PAGE_SHIFT;

	FIN;
	/* check if new page_list is contiguous with end of previous page_list.
	   sg->length here is a multiple of PAGE_SIZE and sg->offset is 0.
	 */
	if (!first && (page_to_pfn(sg_page(sg)) + (sg->length >> PAGE_SHIFT) ==
				   page_to_pfn(page_list[0])))
		update_cur_sg = true;
	while (i != npages) {
		first_page = page_list[i];
		first_pfn = page_to_pfn(first_page);

		/* compute the number of contiguous pages we have starting at i */
		for (len = 0;
			  i != npages && first_pfn + len == page_to_pfn(page_list[i]) &&
			  len < max_pages_in_segment; len++)
			i++;
		/* squash N contiguous pages from page_list into current sge */
		if (update_cur_sg) {
			if ((max_seg_sz - sg->length) >= (len << PAGE_SHIFT)) {
				sg_set_page(
					sg, sg_page(sg), sg->length + (len << PAGE_SHIFT), 0);
				update_cur_sg = false;
				continue;
			}
			update_cur_sg = false;
		}

		/* squash N contiguous pages into next sge or first sge */
		if (!first)
			sg = sg_next(sg);

		(*nents)++;
		sg_set_page(sg, first_page, len << PAGE_SHIFT, 0);
		first = false;
	}
	FOUT;
	return sg;
}

struct reg_mem_comp {
	struct completion *c;
	enum ib_wc_status status;
};

void reg_mem_send_comp(void *ctx, struct ib_wc *wc)
{
	struct reg_mem_comp *s = ctx;
	s->status = wc->status;
	complete(s->c);
}

static int register_memory(void *p, struct request_base *r)
{
	struct mem_reg_request *rr = container_of(r, struct mem_reg_request, r);
	struct _rdma_dev *_dev = p;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_port *ib_port;
	struct dev_per_cpu *d = NULL;
	struct io_req *io_req = NULL;
	struct _memory_reg *mem;
	struct scatterlist *sg;
	struct page **page_list;
	unsigned long cur_base;
	unsigned max_seg_size = dma_get_max_seg_size(ib_dev->dma_device);
	struct ib_mr *mr = NULL;
	enum ib_mr_type mr_type;
	unsigned long npages;
	unsigned int sg_offset = 0;
	unsigned delta;
	u32 rkey;
	struct ib_reg_wr wr;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct reg_mem_comp rmc;
	int i, j, err;

	FIN;
	/* start with a NUL reply namely registration failed */
	*rr->mem = NULL;
	/* check whether the RDMA device has ports */
	if (list_empty(&_dev->port_list)) {
		xetrace("Device %s has no ports\n", ib_dev->name);
		goto out;
	}
	else
		ib_port = list_first_entry(&_dev->port_list, struct ib_port, link);
	/* check whether the ib_prt has per_cpu RDMA devices */
	if (list_empty(&ib_port->pcpu_list)) {
		xetrace("Device %s, port %d has no per_cpu RDMA devices\n",
			ib_dev->name, ib_port->port);
		goto out;
	}
	else
		d = list_first_entry(&ib_port->pcpu_list, struct dev_per_cpu, link);
	/* try to get an IO request to post message to HW */
	if (!(io_req = get_io_req(d))) {
		xetrace("Failed to get io_req from cpu %d\n", d->cpu);
		goto out;
	}
	/* count how many pages the caller is trying to map */
	npages = mem_num_pages((unsigned long)rr->addr, rr->len);
	xdtrace("addr=%p, len=%d\n", rr->addr, rr->len);
	xdtrace("npages=%ld\n", npages);
	xdtrace("_dev->dev_attr.max_fast_reg_page_list_len=%d, max_seg_size=%u\n",
		_dev->dev_attr.max_fast_reg_page_list_len, max_seg_size);
	if (npages > _dev->dev_attr.max_fast_reg_page_list_len) {
		xetrace("Memory chunk of %ld pages exceeds the max mapped pages %u\n",
			npages, _dev->dev_attr.max_fast_reg_page_list_len);
		goto out;
	}
	mr_type = (ib_dev->attrs.device_cap_flags & IB_DEVICE_SG_GAPS_REG) ?
		IB_MR_TYPE_SG_GAPS : IB_MR_TYPE_MEM_REG;
	/* allocate a memory registration object */
	mr = ib_alloc_mr(_dev->pd, mr_type, npages);
	if (IS_ERR_OR_NULL(mr)) {
		err = PTR_ERR(mr);
		xetrace("Devive %s, allocating MR for %ld pages failed: "
				"ib_alloc_mr() returned with error %d\n",
			ib_dev->name, npages, err);
		mr = NULL;
		goto out;
	}
	/* we need a table to hold the pages of the caller memory - allocating
	   a single page so we can add up to 512 pages ina  single call.  if that
	   is not enough we also enable concatenating page_list to previous SG
	 */
	page_list = (struct page **)__get_free_page(GFP_KERNEL);
	if (!page_list) {
		xetrace("Failed to allocate page_list for registering memory\n");
		goto freemr;
	}
	/* allocate the reply to the user - only now since we need the sg_table */
	if (!(mem = kzalloc(sizeof(*mem), GFP_KERNEL))) {
		xetrace("Failed to allocate memory for registering memory\n");
		goto freepage;
	}
	else
		mem->_dev = _dev;
	/* initialize the SG table for the memory registration */
	if (sg_alloc_table(&mem->sg_head, npages, GFP_KERNEL)) {
		xetrace("Failed to sg_table for registering memory\n");
		goto freemem;
	}
	else
		sg = mem->sg_head.sgl;
	/* start with first page */
	cur_base = ((unsigned long)rr->addr) & PAGE_MASK;
	while (npages) {
		j = min_t(unsigned long, npages, PAGE_SIZE / sizeof(struct page *));
		/* fill the page list */
		for (i = 0; i < j; ++i) {
			page_list[i] = virt_to_page(cur_base);
			cur_base += PAGE_SIZE;
			--npages;
		}
		/* the pages to the SG - inside the add_sg_table we will try to join
		   this page_list with previous if the pages are contiguous 
		 */
		sg = add_sg_table(sg, page_list, j, max_seg_size, &mem->sg_nents);
	}
	/* end the SG list */
	sg_mark_end(sg);
	mem->sg_head.sgl->offset = ((unsigned long)rr->addr) & (~PAGE_MASK);
	mem->sg_head.sgl->length -= mem->sg_head.sgl->offset;
	delta = (unsigned)(((unsigned long)rr->addr + rr->len) & (~PAGE_MASK));
	if (delta)
		sg->length -= PAGE_SIZE - delta;
	/* map the SG to the NIC device */
	if (!(mem->nmap = ib_dma_map_sg(
		ib_dev, mem->sg_head.sgl, mem->sg_nents, DMA_BIDIRECTIONAL))) {
		xetrace("Failed to map memory for DMA\n");
		goto freesg;
	}
	/* get a key from the MR object */
	rkey = ib_inc_rkey(mr->rkey);
	ib_update_fast_reg_key(mr, rkey);
	/* and try to register the map SG with the device */
	j = ib_map_mr_sg(
		mr, mem->sg_head.sgl, mem->sg_nents, &sg_offset, PAGE_SIZE);
	if (j != mem->sg_nents) {
		xetrace("failed to map SG to MR\n");
		goto unmapsg;
	}
	/* do the actuall registration using the fast method as follows:
	   1. fill the WR
	   2. post the WR
	   3. wait for the post completion callback
	 */
	memset(&wr, 0, sizeof(wr));
	wr.wr.wr_id = io_req->send.wr.wr.wr_id;
	wr.wr.opcode = IB_WR_REG_MR;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.mr = mr;
	wr.key = mr->rkey;
	wr.access = (IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
				 IB_ACCESS_REMOTE_WRITE |IB_ACCESS_REMOTE_ATOMIC);
	rmc.c = &comp;
	io_req->send_comp = reg_mem_send_comp;
	io_req->ctx = &rmc;
	if ((err = ib_post_send(d->qp, &wr.wr, NULL))) {
		xetrace("Device %s, port %d: post_send failed\n",
				ib_dev->name, ib_port->port);
		goto unmapsg;
	}
	else
		wait_for_completion(&comp);
	if (rmc.status != IB_WC_SUCCESS) {
		xetrace("Device %s, port %d: failed to register memory, reason %s\n",
			ib_dev->name, ib_port->port, ib_wc_status_msg(rmc.status));
		goto unmapsg;
	}
	/* everything went smoothly - return to the caller with
	   the registration info
	*/
	mem->mr = mr;
	mem->base.addr = mem->mr->iova;
	mem->base.lkey = mem->mr->lkey;
	mem->base.rkey = mem->mr->rkey;
	*rr->mem = &mem->base;
	free_page((unsigned long)page_list);
	goto out;

unmapsg:
	ib_dma_unmap_sg(ib_dev, mem->sg_head.sgl, mem->sg_nents, DMA_BIDIRECTIONAL);

freesg:
	sg_free_table(&mem->sg_head);

freepage:
	free_page((unsigned long)page_list);

freemem:
	kfree(mem);

freemr:
	ib_dereg_mr(mr);

out:
	if (io_req)
		put_io_req(io_req);
	FOUT;
	complete(rr->c);
	return 0;
}

struct memory_reg * rdma_dev_reg_mem(
	struct rdma_dev *d, void *addr, unsigned len)
{
	struct mem_reg_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct memory_reg *rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_memory_registration;
		r->r.call = register_memory;
		r->r.free = mem_reg_request_free;
		r->addr = addr;
		r->len = len;
		r->mem = &rv;
		r->c = &comp;
		if (rdma_dev_push_request(d, &r->r))
			wait_for_completion(&comp);
		else {
			register_memory(container_of(d, struct _rdma_dev, dev), &r->r);
			kfree(r);
		}
	}
	else {
		xetrace("Failed to allocate memory for mem_reg_request\n");
		rv = NULL;
	}
	FOUT;
	return rv;
}

static int unregister_memory(void *p, struct request_base *r)
{
	struct mem_unreg_request *rr = container_of(r, struct mem_unreg_request, r);
	struct _rdma_dev *_dev = p;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct ib_port *ib_port;
	struct dev_per_cpu *d = NULL;
	struct io_req *io_req = NULL;
	struct _memory_reg *mem = rr->mem;
	struct ib_send_wr wr;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct reg_mem_comp rmc;
	int rv;

	FIN;
	/* start with a NUL reply namely registration failed */
	*rr->rv = -1;
	/* check whether the RDMA device has ports */
	if (list_empty(&_dev->port_list)) {
		xetrace("Device %s has no ports\n", ib_dev->name);
		goto out;
	}
	else
		ib_port = list_first_entry(&_dev->port_list, struct ib_port, link);
	/* check whether the ib_prt has per_cpu RDMA devices */
	if (list_empty(&ib_port->pcpu_list)) {
		xetrace("Device %s, port %d has no per_cpu RDMA devices\n",
			ib_dev->name, ib_port->port);
		goto out;
	}
	else
		d = list_first_entry(&ib_port->pcpu_list, struct dev_per_cpu, link);
	/* try to get an IO request to post message to HW */
	if (!(io_req = get_io_req(d))) {
		xetrace("Failed to get io_req from cpu %d\n", d->cpu);
		goto out;
	}
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = io_req->send.wr.wr.wr_id;
	wr.opcode = IB_WR_LOCAL_INV;
	wr.ex.invalidate_rkey = mem->mr->rkey;
	rmc.c = &comp;
	io_req->send_comp = reg_mem_send_comp;
	io_req->ctx = &rmc;
	if ((rv = ib_post_send(d->qp, &wr, NULL))) {
		xetrace("Device %s, port %d: post_send failed\n",
				ib_dev->name, ib_port->port);
		goto out;
	}
	else
		wait_for_completion(&comp);
	if (rmc.status != IB_WC_SUCCESS) {
		xetrace("Device %s, port %d: failed to register memory, reason %s\n",
			ib_dev->name, ib_port->port, ib_wc_status_msg(rmc.status));
		goto out;
	}
	ib_dma_unmap_sg(ib_dev, mem->sg_head.sgl, mem->sg_nents, DMA_BIDIRECTIONAL);
	sg_free_table(&mem->sg_head);
	ib_dereg_mr(mem->mr);
	kfree(mem);
	*rr->rv = 0;

out:
	if (io_req)
		put_io_req(io_req);
	FOUT;
	complete(rr->c);
	return 0;
}

int rdma_dev_unreg_mem(struct memory_reg *mem)
{
	struct mem_unreg_request *r;
	struct _memory_reg *m = container_of(mem, struct _memory_reg, base);
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv = -1;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_memory_unregistration;
		r->r.call = unregister_memory;
		r->r.free = mem_unreg_request_free;
		r->mem = m;
		r->rv = &rv;
		r->c = &comp;
		if (rdma_dev_push_request(&m->_dev->dev, &r->r))
			wait_for_completion(&comp);
		else {
			unregister_memory(m->_dev, &r->r);
			kfree(r);
		}
	}
	else
		xetrace("Failed to allocate memory for mem_unreg_request\n");
	FOUT;
	return rv;
}

void rdma_dev_sync_mem_for_cpu(
	struct memory_reg *mem, enum dma_data_direction dir)
{
	struct _memory_reg *m = container_of(mem, struct _memory_reg, base);

	FIN;
	__dma_sync_sgtable_for_cpu(
		m->_dev->dev.ib_dev->dma_device, &m->sg_head, dir);
	FOUT;
}

void rdma_dev_sync_mem_for_dev(
	struct memory_reg *mem, enum dma_data_direction dir)
{
	struct _memory_reg *m = container_of(mem, struct _memory_reg, base);

	FIN;
	__dma_sync_sgtable_for_device(
		m->_dev->dev.ib_dev->dma_device, &m->sg_head, dir);
	FOUT;
}

/*
 
path creation and the discovery process

Some notes about path, client service and server:
With DC, path between nodes is not related to a client/server communication.
Instead a path is an handle that we can use to send data between nodes.
Client is an entity that can send - in QP nomenclature client is an entity that
is RTS.  In order for a client to send data using a path it must have
a reciver on the destination of the path.  In QP nomenclature a reciver is
an entity that is RTR.  A reciver is called here a service.  Therefore client
sends data to a service that is waiting on the destination of the path.
In DC the server concept is only used as a listening entity that enables
the creation of paths - see belows.

In order to start a path a client must call rdma_dev_connect().  However for
that to work a listner must be registered on the receiving end.  The listener
here is called a server.  It listen on the serving side.  To create a listener
we must call rdma_dev_register_server().

Starting a new path will do the following on the client side:
1.  create a path entity
2.  link the client to the path
3.  the path tries:
3.1 send resolve_address
3.2 send resolve_route
3.3 send connect
3.4 receive conenct_reply
3.5 send accept

On the receiving end we will have:
1. receive connect_request
2. send accept
3. receive established

Since the client is not binded to a path, a path by itself is a standalone
entity that must communicate between its two peers for discovery.
The discovery process provides the information how a client can use the path
to send data to the service.  The discovery process also allows
for clients running of the service to sue the path to send data for services
running on the client end.

The path discovery process is run once when the path is created.  the process
goes as follows:
On the initiation side of the path - the side that called rdma_dev_connect():
1. register a path service to be able to receive messages
2. create path access map
2. send discovery request
3. receive discovery reply
4. make path available for clients 

On the listening side of the path:
1. receive a discovery request
2. save the access map of the sender 
3. create path access map
4. send discovery reply
5. make path available for clients

Once the path is ready we continue with the connect request:
On the client side:
1. send the connect payload to the path service on the destination
2. receive the payload from the acceptor of the connecion
3. call the client connect callback with the payload received from the acceptor
4. the client is connected

On the listening side:
1. recive the connect payload
2. call the server accept callback with the client payload
3. the accept will probably create a servive to handle the client and may also
   return a client to send  requests to the sender
4. if there is an acceopt client link it the path so it can receive path events
5. send the acceptor payload as a reply to the connect payload

At the end of the process we have path that can beused for both
side communication and client and service that are linked. 

*/
static void handle_path_event(struct dev_per_cpu *d, struct _path *path,
	struct ib_wc *wc, union service_id *sender_id, int index,
	int (*call)(void *, struct request_base *), int type, const char *str)
{
	struct ib_port *ib_port = d->ib_port;
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct path_op_request *r;
	int rv;

	FIN;
	xdtrace("Device %s, port %d: handling path event %s\n",
		ib_dev->name, ib_port->port, str);
	if ((r = kzalloc(sizeof(*r), GFP_ATOMIC))) {
		r->r.type = type;
		r->r.call = call;
		r->r.free = path_op_request_free;
		r->d = d;
		r->path = path;
		r->port_num = ib_port->port;
		r->sender_id = *sender_id;
		r->wc = *wc;
		r->index = index;
		if (!rdma_dev_push_request(&_dev->dev, &r->r))
			BUG();
		rv = 0;
	}
	else {
		xetrace("Device %s, port %d: failed to allocate memory for "
				"path_event %s\n", ib_dev->name, ib_port->port, str);
		rv = -1;
	}
	FOUT;
}

static struct dev_per_cpu * check_stale_msg(struct _rdma_dev *_dev,
	int port_num, struct dev_per_cpu *d, struct _path *path,
	enum ib_wc_status status, int *err)
{
	struct ib_port *ib_port;
	struct dev_per_cpu *dd = NULL;
	struct _path *p;
	char bs[64];
	char bd[64];
	bool found;

	FIN;
	*err = -1;
	ib_port = find_port_by_num(_dev, port_num);
	if (!ib_port) {
		xetrace("Device %s: failed to find port num %d\n",
			_dev->dev.ib_dev->name, port_num);
		goto out;
	}
	found = false;
	list_for_each_entry(dd, &ib_port->pcpu_list, link) {
		if (dd == d) {
			found = true;
			break;
		}
	}
	if (!found) {
		xetrace("Device %s: failed to find dev_per_cpu %p\n",
			_dev->dev.ib_dev->name, d);
		goto out;
	}
	found = false;
	list_for_each_entry(p, &ib_port->crp.all_paths, link) {
		if (p == path) {
			found = true;
			break;
		}
	}
	if (!found) {
		/* path not found so maybe accept procedure is still in_progress */
		list_for_each_entry(p, &ib_port->in_prog_accept, link) {
			if (p == path) {
				found = true;
				break;
			}
		}
	}
	if (!found) {
		xetrace("Device %s: failed to find path %p\n",
			_dev->dev.ib_dev->name, path);
		goto out;
	}
	if (status != IB_WC_SUCCESS) {
		xetrace("Device %s, port %d: discovery failed for path from "
				"peer %s to %s, status %s\n",
			_dev->dev.ib_dev->name, ib_port->port,
			x_tss(&path->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
			x_tss(&path->cm_id->route.addr.src_addr, bs, sizeof(bs)),
			ib_wc_status_msg(status));
		goto out;
	}
	*err = 0;

out:
	FOUT;
	return *err ? NULL : dd;
}

static int handle_path_send_error(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct path_op_send_error_request *rr =
		container_of(r, struct path_op_send_error_request, r);
	struct dev_per_cpu *d;
	int rv;
	
	FIN;
	d = check_stale_msg(
		_dev, rr->port_num, rr->d, rr->path, IB_WC_SUCCESS, &rv);
	if (rv)
		goto out;
	xetrace("Failed to send %s\n", internal_msg_opcode_2_str(rr->opcode));
	start_path_disconnect(rr->path->ib_port, rr->path);

out:
	FOUT;
	return 0;
}

static void on_path_send_error(struct dev_per_cpu *d, struct _path *path,
	int opcode)
{
	struct ib_port *ib_port = d->ib_port;
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct path_op_send_error_request *r;
	int rv;

	FIN;
	xdtrace("Device %s, port %d: handling path send error event\n",
		ib_dev->name, ib_port->port);
	if ((r = kzalloc(sizeof(*r), GFP_ATOMIC))) {
		r->r.type = rpcrdma_path_send_error;
		r->r.call = handle_path_send_error;
		r->r.free = path_op_send_error_request_free;
		r->d = d;
		r->path = path;
		r->port_num = ib_port->port;
		r->opcode = opcode;
		if (!rdma_dev_push_request(&_dev->dev, &r->r))
			BUG();
		rv = 0;
	}
	else {
		xetrace("Device %s, port %d: failed to allocate memory for "
				"path send error event\n", ib_dev->name, ib_port->port);
		rv = -1;
	}
	FOUT;
}

static void discovery_send_comp(void *ctx, struct ib_wc *wc)
{
	struct io_req *io_req = ctx;
	struct ib_port *ib_port = io_req->w.pcpu->ib_port;
	struct _rdma_dev *_dev = ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct internal_payload *msg;

	FIN;
	if (wc->status != IB_WC_SUCCESS) {
		xdtrace("Device %s, port %d: failed to send discovery request: "
				"reason %s\n", ib_dev->name, ib_port->port,
			ib_wc_status_msg(wc->status));
		msg = io_req->msg;
		on_path_send_error(io_req->w.pcpu, io_req->priv, msg->base.opcode);
	}
	put_io_req(io_req);
	FOUT;
}

static int send_payload(struct _path *path, struct _client *client)
{
	struct _rdma_dev *_dev = path->ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct x_cm_payload *v = (void *)path->accept_connect;
	struct dev_per_cpu *d;
	struct io_req *io_req;
	struct internal_payload *msg;
	char bs[64];
	char bd[64];
	int rv;
	
	FIN;
	xdtrace("Device %s, port %d: send_payload from %s to %s\n",
		ib_dev->name, path->ib_port->port,
		x_tss(&path->cm_id->route.addr.src_addr, bs, sizeof(bs)),
		x_tss(&path->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	d = list_first_entry(&path->ib_port->pcpu_list, struct dev_per_cpu, link);
	io_req = get_io_req(d);
	if (!io_req) {
		xetrace("Device %s, port %d: no io_req to start discovery\n",
				ib_dev->name, path->ib_port->port);
		rv = -1;
		goto out;
	}
	fill_msg_hdr(io_req->hdr,
		&path->sid, cpu_to_be32(d->dct->dct_num),
		&v->sid, v->sid.global.dct);
	xdtrace("Sending payload from sid %s to sid %s\n",
		x_tsid(&io_req->hdr->src, bs, sizeof(bs)),
		x_tsid(&io_req->hdr->dst, bd, sizeof(bd)));
	msg = io_req->msg;
	msg->base.opcode = cpu_to_be32(imo_connect_payload);
	msg->cid = client->id;
	memcpy(msg->payload, client->payload, sizeof(msg->payload));
	sync_io_req(_dev, io_req);
	io_req->send.wr.wr.send_flags = IB_SEND_SIGNALED;
	io_req->send.ah = path->ah;
	io_req->send.dct_access_key = path->remote_dct_key;
	io_req->send.dct_number = path->remote_dct_num;
	io_req->send_comp = discovery_send_comp;
	io_req->ctx = io_req;
	io_req->priv = path;
	if ((rv = ib_post_send(d->qp, &io_req->send.wr.wr, NULL))) {
		xetrace("Device %s, port %d: post_send failed\n",
				ib_dev->name, path->ib_port->port);
		rv = -1;
		goto putback;
	}
	rv = 0;
	goto out;
	
putback:
	put_io_req(io_req);
	
out:
	FOUT;
	return rv;
}

static void send_payloads(struct _path *path)
{
	struct _client *c;
	struct list_head *pos, *q;

	FIN;
	list_for_each_safe(pos, q, &path->clients) {
		c = list_entry(pos, struct _client, link);
		if (send_payload(path, c)) {
			list_del_init(pos);
			if (c->ft.connect)
				c->ft.connect(c->ctx, NULL, NULL, 0);
			kfree(c);
		}
	}
	FOUT;
}

static int save_discovery(struct _path *path, struct internal_discovery *msg)
{
	struct dct_info *p;
	struct wire_dct_info *q;
	char bs[64];
	char bd[64];
	int i, n, rv;

	FIN;
	n = be32_to_cpu(msg->npairs);
	if (!(p = kzalloc(sizeof(*p) * n, GFP_KERNEL))) {
		xetrace("path %s -> %s: failed to allocate memory for discovery info\n",
			x_tss(&path->cm_id->route.addr.dst_addr, bd, sizeof(bd)),
			x_tss(&path->cm_id->route.addr.src_addr, bs, sizeof(bs)));
		rv = -1;
		goto out;
	}
	q = msg->a;
	for (i = 0; i < n; ++i) {
		p[i].dct_num = be32_to_cpu(q->dct_num);
		p[i].cpu_core = be32_to_cpu(q->cpu_core);
	}
	path->remote_dct_info = p;
	path->n_dct_info = n;
	if ((path->owner_path =
		 manager_link_new_path(path->ib_port->_dev->owner, path))) {
		path->status = ps_established;
		rv = 0;
	}
	else
		rv = -1;

out:
	FOUT;
	return rv;
}

static void build_discovery(struct dev_per_cpu *d, struct _path *path,
	struct internal_discovery *msg)
{
	struct _rdma_dev *_dev = path->ib_port->_dev;
	struct path_pcp *pcpu;
	struct wire_dct_info *wdi;
	int i, cpu;

	FIN;
	i = 0;
	wdi = msg->a;
	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(path->pcpu, cpu);
		if (pcpu->d) {
			if (((void *)&wdi[i] + sizeof(*wdi)) - (void *)msg <
				_dev->srq_msg_size) {
				wdi[i].cpu_core = cpu_to_be32(cpu);
				wdi[i].dct_num = cpu_to_be32(d->dct->dct_num);
				++i;
			}
			else
				break;
		}
	}
	msg->npairs = cpu_to_be32(i);
	FOUT;
}

static int start_discovery(struct _path *path)
{
	struct _rdma_dev *_dev = path->ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct x_cm_payload *v = (void *)path->accept_connect;
	struct dev_per_cpu *d;
	struct io_req *io_req;
	struct internal_discovery *msg;
	char bs[64];
	char bd[64];
	int rv;
	
	FIN;
	xdtrace("Device %s, port %d: discovery from %s to %s\n",
		ib_dev->name, path->ib_port->port,
		x_tss(&path->cm_id->route.addr.src_addr, bs, sizeof(bs)),
		x_tss(&path->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	d = list_first_entry(&path->ib_port->pcpu_list, struct dev_per_cpu, link);
	io_req = get_io_req(d);
	if (!io_req) {
		xetrace("Device %s, port %d: no io_req to start discovery\n",
				ib_dev->name, path->ib_port->port);
		rv = -1;
		goto out;
	}
	fill_msg_hdr(io_req->hdr,
		&path->sid, cpu_to_be32(d->dct->dct_num),
		&v->sid, v->sid.global.dct);
	msg = io_req->msg;
	msg->base.opcode = cpu_to_be32(imo_discover_req);
	build_discovery(d, path, msg);
	sync_io_req(_dev, io_req);
	io_req->send.wr.wr.send_flags = IB_SEND_SIGNALED;
	io_req->send.ah = path->ah;
	io_req->send.dct_access_key = path->remote_dct_key;
	io_req->send.dct_number = path->remote_dct_num;
	io_req->send_comp = discovery_send_comp;
	io_req->ctx = io_req;
	io_req->priv = path;
	if ((rv = ib_post_send(d->qp, &io_req->send.wr.wr, NULL))) {
		xetrace("Device %s, port %d: post_send failed\n",
				ib_dev->name, path->ib_port->port);
		rv = -1;
		goto putback;
	}
	rv = 0;
	goto out;
	
putback:
	put_io_req(io_req);
	
out:
	FOUT;
	return rv;
}

static int build_discovery_rep(
	struct dev_per_cpu *d, struct _path *path, union service_id *sender_id)
{
	struct _rdma_dev *_dev = path->ib_port->_dev;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct io_req *io_req;
	struct internal_discovery *msg;
	char bs[64];
	char bd[64];
	int rv;
	
	FIN;
	xdtrace("Device %s, port %d: discovery reply from %s to %s\n",
		ib_dev->name, path->ib_port->port,
		x_tss(&path->cm_id->route.addr.src_addr, bs, sizeof(bs)),
		x_tss(&path->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	io_req = get_io_req(d);
	if (!io_req) {
		xetrace("Device %s, port %d: no io_req to start discovery\n",
				ib_dev->name, path->ib_port->port);
		rv = -1;
		goto out;
	}
	fill_msg_hdr(io_req->hdr,
		&path->sid, cpu_to_be32(d->dct->dct_num),
		sender_id, sender_id->global.dct);
	xdtrace("Sending reply from sid %s to sid %s\n",
		x_tsid(&io_req->hdr->src, bs, sizeof(bs)),
		x_tsid(&io_req->hdr->dst, bd, sizeof(bd)));
	msg = io_req->msg;
	msg->base.opcode = cpu_to_be32(imo_discover_rep);
	build_discovery(d, path, msg);
	sync_io_req(_dev, io_req);
	io_req->send.wr.wr.send_flags = IB_SEND_SIGNALED;
	io_req->send.ah = path->ah;
	io_req->send.dct_access_key = path->remote_dct_key;
	io_req->send.dct_number = be32_to_cpu(sender_id->global.dct);
	io_req->send_comp = discovery_send_comp;
	io_req->ctx = io_req;
	io_req->priv = path;
	if ((rv = ib_post_send(d->qp, &io_req->send.wr.wr, NULL))) {
		xetrace("Device %s, port %d: post_send failed\n",
				ib_dev->name, path->ib_port->port);
		rv = -1;
		goto putback;
	}
	rv = 0;
	goto out;
	
putback:
	put_io_req(io_req);
	
out:
	FOUT;
	return rv;
}

static int handle_discovery_req(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct path_op_request *rr = container_of(r, struct path_op_request, r);
	struct dev_per_cpu *d;
	int rv;
	
	FIN;
	d = check_stale_msg(
		_dev, rr->port_num, rr->d, rr->path, rr->wc.status, &rv);
	if (rv) {
		if (d)
			goto putback;
		else
			goto out;
	}
	if (save_discovery(rr->path, get_recv_buf(d, rr->index)) ||
		build_discovery_rep(d, rr->path, &rr->sender_id))
		start_path_disconnect(d->ib_port, rr->path);

putback:
	rdma_dev_put_recv_resource(d, rr->index);

out:
	FOUT;
	return 0;
}

static int handle_discovery_rep(void *v, struct request_base *r)
{
	struct _rdma_dev *_dev = v;
	struct path_op_request *rr = container_of(r, struct path_op_request, r);
	struct dev_per_cpu *d;
	int rv;

	FIN;
	d = check_stale_msg(
		_dev, rr->port_num, rr->d, rr->path, rr->wc.status, &rv);
	if (rv) {
		if (d)
			goto putback;
		else
			goto out;
	}
	rv = save_discovery(rr->path, get_recv_buf(d, rr->index));
	if (!rv) {
		send_payloads(rr->path);
	}
	else
		start_path_disconnect(rr->path->ib_port, rr->path);

putback:
	rdma_dev_put_recv_resource(d, rr->index);

out:
	FOUT;
	return 0;
}

static int handle_connect_payload(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct path_op_request *rr = container_of(r, struct path_op_request, r);
	struct dev_per_cpu *d;
	struct internal_payload *msg;
	char payload[CONNECT_PAYLOAD_SIZE];
	u64 cid;
	struct io_req *io_req;
	struct client_info *c;
	struct _client *client;
	int rv;
	
	FIN;
	d = check_stale_msg(
		_dev, rr->port_num, rr->d, rr->path, rr->wc.status, &rv);
	if (rv) {
		if (d)
			goto putback;
		else
			goto out;
	}
	msg = get_recv_buf(d, rr->index);
	cid = msg->cid;
	if ((client = kzalloc(sizeof(*client), GFP_KERNEL))) {
		c = d->ib_port->server.info.ft.accept(d->ib_port->server.info.server,
			rr->path->owner_path,
			&rr->path->cm_id->route.addr.src_addr,
			&rr->path->cm_id->route.addr.dst_addr,
			msg->payload, sizeof(msg->payload),
			payload, sizeof(payload));
		if (c) {
			client->type = pht_client;
			client->ft = c->ft;
			client->ctx = c->client;
			client->id = x_get_guid();
			list_add_tail(&client->link, &rr->path->clients);
		}
		else
			kfree(client);
	}
	else {
		xetrace("Device %s: failed to alllocate memory for accept client\n",
			ib_dev->name);
		memset(payload, 0, sizeof(payload));
	}
	io_req = get_io_req(d);
	if (!io_req) {
		xetrace("Device %s, port %d: no io_req to start discovery\n",
				ib_dev->name, rr->path->ib_port->port);
		goto putback;
	}
	fill_msg_hdr(io_req->hdr,
		&rr->path->sid, cpu_to_be32(d->dct->dct_num),
		&rr->sender_id, rr->sender_id.global.dct);
	msg = io_req->msg;
	msg->base.opcode = cpu_to_be32(imo_accept_payload);
	msg->cid = cid;
	memcpy(msg->payload, payload, sizeof(msg->payload));
	sync_io_req(_dev, io_req);
	io_req->send.wr.wr.send_flags = IB_SEND_SIGNALED;
	io_req->send.ah = rr->path->ah;
	io_req->send.dct_access_key = rr->path->remote_dct_key;
	io_req->send.dct_number = be32_to_cpu(rr->sender_id.global.dct);
	io_req->send_comp = discovery_send_comp;
	io_req->ctx = io_req;
	io_req->priv = rr->path;
	if ((rv = ib_post_send(d->qp, &io_req->send.wr.wr, NULL))) {
		xetrace("Device %s, port %d: post_send failed\n",
				ib_dev->name, rr->path->ib_port->port);
		goto putback;
	}

putback:
	rdma_dev_put_recv_resource(d, rr->index);

out:
	FOUT;
	return 0;
}

static int handle_accept_payload(void *p, struct request_base *r)
{
	struct _rdma_dev *_dev = p;
	struct ib_device *ib_dev = _dev->dev.ib_dev;
	struct path_op_request *rr = container_of(r, struct path_op_request, r);
	struct dev_per_cpu *d;
	struct internal_payload *msg;
	u64 cid;
	struct _client *c;
	char bs[64];
	char bd[64];
	int rv;

	FIN;
	d = check_stale_msg(
		_dev, rr->port_num, rr->d, rr->path, rr->wc.status, &rv);
	if (rv) {
		if (d)
			goto putback;
		else
			goto out;
	}
	msg = get_recv_buf(d, rr->index);
	cid = msg->cid;
	xdtrace("Device %s, port %d: Received payload for client %lld on "
			"path from %s to %s\n", ib_dev->name, rr->path->ib_port->port, cid,
		x_tss(&rr->path->cm_id->route.addr.src_addr, bs, sizeof(bs)),
		x_tss(&rr->path->cm_id->route.addr.dst_addr, bd, sizeof(bd)));
	list_for_each_entry(c, &rr->path->clients, link) {
		if (c->id == cid) {
			if (c->ft.connect) {
				c->connected = true;
				c->ft.connect(c->ctx, rr->path->owner_path,
					msg->payload, sizeof(msg->payload));
			}
			break;
		}
	}

putback:
	rdma_dev_put_recv_resource(d, rr->index);

out:
	FOUT;
	return 0;
}

static void on_path_event(void *ctx, struct ib_wc *wc, void *msg, void *rsc,
	int index, union service_id *sender_id)
{
	struct _path *path = ctx;
	struct dev_per_cpu *d = rsc;
	struct internal_msg *p = msg;

	FIN;
	switch (be32_to_cpu(p->opcode)) {
	case imo_discover_req:
		handle_path_event(d, path, wc, sender_id, index,
			handle_discovery_req, rpcrdma_discovery_req, "discover_req");
		break;
	case imo_discover_rep:
		handle_path_event(d, path, wc, sender_id, index,
			handle_discovery_rep, rpcrdma_discovery_rep, "discover_rep");
		break;
	case imo_connect_payload:
		handle_path_event(d, path, wc, sender_id, index,
			handle_connect_payload, rpcrdma_connect_payload, "connect_payload");
		break;
	case imo_accept_payload:
		handle_path_event(d, path, wc, sender_id, index,
			handle_accept_payload, rpcrdma_accept_payload, "accept_payload");
		break;
	default:
		rdma_dev_put_recv_resource(rsc, index);
		break;
	}
	FOUT;
}

static struct _path * create_path(struct ib_port *ib_port)
{
	struct ib_device *ib_dev = ib_port->_dev->dev.ib_dev;
	int node = ib_dev->dma_device->numa_node;
	struct _path *path;
	struct path_pcp __percpu *percpu;
	struct path_pcp *pcpu;
	struct dev_per_cpu *d;
	struct server_service service = {0};
	int cpu;

	FIN;
	if (!(path = kzalloc(sizeof(*path), GFP_KERNEL))) {
		xetrace("Device %s, port %d: failed to allocate path\n",
			ib_dev->name, ib_port->port);
		goto out;
	}
	percpu = alloc_percpu(struct path_pcp);
	if (percpu == NULL) {
		xetrace("Failed to allocate per_cpu data\n");
		goto freee;
	}
	else
		path->pcpu = percpu;
	init_path(path, ib_port);
	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(percpu, cpu);
		pcpu->d = NULL;
		if (cpu_to_node(cpu) == node)
			list_for_each_entry(d, &path->ib_port->pcpu_list, link) {
				if (d->cpu == cpu) {
					pcpu->d = d;
					break;
				}
			}
	}
	service.recv_comp = on_path_event;
	service.ctx = path;
	path->ready = true;
	if (manager_register_service(ib_port->_dev->owner, &service, &path->sid)) {
		start_free_path(ib_port, path);
		path = NULL;
	}
	goto out;

freee:
	kfree(path);
	path = NULL;

out:
	FOUT;
	return path;
}

int rdma_dev_disconnect(struct rdma_dev *d, void *client, void *path)
{
	struct disconnect_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_disconnect;
		r->r.call = disconnect;
		r->r.free = disconnect_request_free;
		r->client = client;
		r->path = path;
		r->c = &comp;
		if (rdma_dev_push_request(d, &r->r))
			wait_for_completion(&comp);
		else {
			disconnect(container_of(d, struct _rdma_dev, dev), &r->r);
			kfree(r);
		}
		rv = 0;
	}
	else {
		xetrace("Failed to allocate memory for disconnect_request\n");
		rv = -1;
	}
	FOUT;
	return rv;
}

