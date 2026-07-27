#if 0
#include "kr_incs.h"
#include "kr_version.h"
#include "u.h"
#include "u_dc.h"
#include "ib_incs.h"
#endif

#include "rdma/ib_addr.h"
#if !HAS_IB_QUERY_GID
#include "rdma/ib_cache.h"
#endif
#ifdef NO_OFED
#	include "ib_verbs_exp_def.h"
#	include "ib_verbs_exp.h"

struct ib_dct *ib_exp_create_dct(struct ib_pd *pd,
	struct ib_dct_init_attr *attr, struct ib_udata *udata)
{
	return NULL;
}

#define IB_EXP_QPT_DC_INI IB_QPT_RESERVED3 

int ib_exp_destroy_dct(struct ib_dct *dct,  struct ib_udata *udata)
{
	return 0;
}

#else
#	include "rdma/ib_verbs_exp.h"
#endif

#include <linux/inet.h>
#include <rdma/ib.h>

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

static struct dc_info *u_info;
static struct ib_sa_client sa_cli;

static void u_add_one(struct ib_device *device);
static void u_remove_one(struct ib_device *device, void *v);
static struct ib_client client = {
	.name   = "",
	.add    = u_add_one,
	.remove = u_remove_one
};

static void call_start(struct work_struct *work)
{
	struct dc_info *info = container_of(work, struct dc_info, start_work);
	int rv;

	FIN;
	register_sa();
	if ((rv = register_rdma()))
		trace("failed to register client\n");
	else
		info->started = true;
	FOUT;
}

static void add_one(struct work_struct *w)
{
	struct add_one_work *aw = container_of(w, struct add_one_work, work);
	struct dc_info *info = aw->info;
	struct ib_device *device = aw->device;
	struct u_dev *udev;
	struct ib_port *ib_port;
	int s, e, p;
	
	FIN;
	kfree(aw);
	if (!info->add_one_started) {
		wait_for_completion(&info->add_one_started_comp);
		info->add_one_started = true;
	}
	ib_set_client_data(device, &client, NULL);
	if (!(udev = init_dev(device)))
		goto out;
	
	if (device->node_type == RDMA_NODE_IB_SWITCH) {
		s = 0;
		e = 0;
	}
	else {
		s = 1;
		e = get_device_phys_port_count(device);
	}

	trace("dev %s: %p, s=%d, e=%d\n", device->name, device, s, e);
	for (p = s; p <= e; ++p) {
		ib_port = add_port(udev, p, info->bind_sin);
		if (ib_port)
			list_add_tail(&ib_port->link, &udev->port_list);
	}
	ib_set_client_data(device, &client, udev);
	list_add_tail(&udev->link, &info->devs);
	if (info->post_add_one) {
		if (!--info->add_one_calls)
			info->post_add_one(info);
	}
	goto out;

out:
	FOUT;
}

static void remove_port(struct ib_port *ib_port)
{
	FIN;
	kfree(ib_port);
	FOUT;
}

static void remove_one(struct work_struct *w)
{
	struct remove_one_work *rw = container_of(w, struct remove_one_work, work);
	//struct dc_info *info = container_of(rw, struct dc_info, remove_o_work);
	struct u_dev *udev = rw->priv;
	struct ib_port *ib_port;
	struct completion *c = rw->c;
	
	FIN;
	if (udev) {
		while ((ib_port = list_first_entry_or_null(
			&udev->port_list, struct ib_port, link))) {
			list_del(&ib_port->link);
			remove_port(ib_port);
		}
		free_udev(udev);
		list_del(&udev->link);
		kfree(udev);
	}
	complete(c);
	FOUT;
}

static void u_add_one(struct ib_device *device)
{
	struct add_one_work *add_o_work;

	FIN;
	if (u_info) {
		if ((add_o_work = kzalloc(sizeof(*add_o_work), GFP_KERNEL))) {
			++u_info->add_one_calls;
			INIT_WORK(&add_o_work->work, add_one);
			add_o_work->info = u_info;
			add_o_work->device = device;
			queue_work(u_info->wq, &add_o_work->work);
		}
	}
	else
		trace("_info @ u.c was not set\n");
	FOUT;
}
#if IB_REMOVE_EXTRA_ARG
static void u_remove_one(struct ib_device *device, void *v)
{
	DECLARE_COMPLETION_ONSTACK(comp);

	FIN;
	if (u_info) {
		u_info->remove_o_work.device = device;
		u_info->remove_o_work.c = &comp;
		u_info->remove_o_work.priv = v;
		queue_work(u_info->wq, &u_info->remove_o_work.work);
		wait_for_completion(&comp);
	}
	else
		trace("_info @ u.c was not set\n");
	FOUT;
}
#else
#	error "remove_one does not support caller data" 
#endif

void register_sa(void)
{
	ib_sa_register_client(&sa_cli);
}

void unregister_sa(void)
{
	ib_sa_unregister_client(&sa_cli);
}

int register_rdma(void)
{
	int rv = ib_register_client(&client);
	complete(&u_info->add_one_started_comp);
	return rv;
}

void unregister_rdma(void)
{
	ib_unregister_client(&client);
}

static int proc_start(void *p, char *page, int count)
{
	struct dc_info *info = p;

	FIN;
	if (info->start) {
		info->last_error = 0;
		info->start(info, page, count);
		if (!info->last_error) {
			if (!start_server_thread(&info->wq, client.name))
				queue_work(info->wq, &info->start_work);
			else
				trace("failed to create WQ. Nothing else to do...\n");
		}
		else
			trace("info->start(info, page, count) failed.  "
				  "Nothing else to do...\n");
	}
	FOUT;
	return count;
}

int init_dc_info(struct dc_info *info, const char *name,
	ssize_t (*start)(struct dc_info *p, char *page, size_t count),
	void (*stop)(struct dc_info *))
{
	int rv;

	FIN;
	if (!(info->dir = proc_mkdir(DC_PROC_STR, NULL))) {
		trace("fail to create /proc/%s dir\n", DC_PROC_STR);
		goto err;
	}
	if (!(info->proc_file = dc_proc_create(
		(char *)name, info->dir, proc_start, info))) {
		trace("fail to create /proc/%s/%s entry\n", DC_PROC_STR, name);
		goto err;
	}
	INIT_LIST_HEAD(&info->devs);
	INIT_WORK(&info->start_work, call_start);
	INIT_WORK(&info->remove_o_work.work, remove_one);
	info->start = start;
	info->stop = stop;
	client.name = kstrdup(name, GFP_KERNEL);
	init_completion(&info->add_one_started_comp);
	info->add_one_started = false;
	u_info = info;
	rv = 0;
	goto out;

err:
	if (info->proc_file)
		dc_proc_remove(info->proc_file);
	if (info->dir)
		remove_proc_entry(DC_PROC_STR, NULL);
	rv = -1;

out:
	FOUT;
	return rv;
}

void free_dc_info(struct dc_info *info, bool allocated)
{
	FIN;
	if (info->stop)
		info->stop(info);
	if (info->started) {
		if (info->ah) {
			trace("destroying AH\n");
			rdma_destroy_ah(info->ah);
			info->ah = NULL;
		}
		if (info->qp) {
			trace("destroying QP\n");
			ib_destroy_qp(info->qp);
			info->qp = NULL;
		}
		if (info->cq) {
			trace("destroying CQ\n");
			ib_destroy_cq(info->cq);
			info->cq = NULL;
		}
		if (info->cm_id) {
			trace("destroying CM_ID\n");
			rdma_destroy_id(info->cm_id);
			info->cm_id = NULL;
		}
		unregister_rdma();
		unregister_sa();
	}
	/* finish all work and tear down the work queue */
	trace("waiting for client thread to exit....\n");
	if (info->wq) {
		destroy_workqueue(info->wq);
		info->wq = NULL;
	}
	if (info->proc_file) {
		dc_proc_remove(info->proc_file);
		info->proc_file = NULL;
	}
	if (info->dir) {
		proc_remove(info->dir);
		info->dir = NULL;
	}
	if (!allocated)
		kfree(info);
	FOUT;
}

static struct u_iu *alloc_ioctx(
	struct u_dev *dev, int ioctx_size, int dma_size)
{
	struct u_iu *ioctx = NULL;

	//FIN;
	if (dev->ib_dev) {
		BUG_ON(!(ioctx = kmalloc(ioctx_size, GFP_KERNEL)));
		BUG_ON(!(ioctx->buf = ib_dma_alloc_coherent(
			dev->ib_dev, dma_size, &ioctx->dma, GFP_KERNEL)));
		ioctx->size = dma_size;
	}
	//FOUT;
	return ioctx;
}

static void free_ioctx(
	struct ib_device *dev, struct u_iu *ioctx, int dma_size)
{
	if (ioctx) {
		ib_dma_free_coherent(dev, dma_size, ioctx->buf, ioctx->dma);
		kfree(ioctx);
	}
}

static struct u_iu ** alloc_ioctx_ring(struct u_dev *dev, int ring_size,
	int ioctx_size, int dma_size)
{
	struct u_iu **ring = NULL;
	int i;

	FIN;
	if ((ring = kmalloc(ring_size * sizeof(ring[0]), GFP_KERNEL))) {
		for (i = 0; i < ring_size; ++i) {
			ring[i] = alloc_ioctx(dev, ioctx_size, dma_size);
			ring[i]->index = i;
		}
	}
	FOUT;
	return ring;
}

static void free_ioctx_ring(struct u_iu **ioctx_ring,
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

static int fill_recv_q(struct u_dev *dev, struct srq_info *srq)
{
	int rv = 0, i;

	FIN;
	for (i = 0; i < srq->srq_queue_size; i++) {
		struct u_iu *iu = srq->rx_ring[i];
		 if ((rv = post_recv(dev, iu, srq)) < 0) {
			 trace("ib_post_srq_recv() failed: %d\n", rv);
			 goto out;
		 }
	}

out:
	FOUT;
	return rv;
}

static void srq_event_handler(struct ib_event *event, void *ctx)
{
	struct u_dev *dev = ctx;
	trace("%s SRQ event %d\n", dev->ib_dev->name, event->event);
}

/* create volume shared receive queue */
static int create_srq(struct u_dev *dev, int q_size, int msg_size,
	struct srq_info *srqi)
{
	struct srq_info srq = {0};
	struct ib_srq_init_attr srq_attr;
	int rv = 0;

	FIN;
	q_size = min(q_size, 4096);
	trace("q_size=%d, msg_size=%d\n", q_size, msg_size);
	memset(&srq_attr, 0, sizeof(srq_attr));
	srq_attr.event_handler = srq_event_handler;
	srq_attr.srq_context = dev;
	srq_attr.attr.max_wr = q_size;
	srq_attr.attr.max_sge = 1;
	srq_attr.attr.srq_limit = 0;
	srq_attr.srq_type = IB_SRQT_BASIC;

	srq.srq = ib_create_srq(dev->pd, &srq_attr);
	if (IS_ERR_OR_NULL(srq.srq)) {
		rv = PTR_ERR(srq.srq);
		trace("fail to create volume shared receive queue %d\n", rv);
		goto out;
	}
	else if (false/* && (rv = ibv_get_srq_num(srq.srq, &srq.srqn))*/) {
		trace("fail to ibv_get_srq_num %d\n", rv);
		goto out;
	}
	else {
		srq.srqn = 0;
		trace("srq=%p\n", srq.srq);
	}
	srq.srq_queue_size = q_size;
	srq.srq_msg_size = msg_size;
	/* allocate receive buffers */
	srq.rx_ring = alloc_ioctx_ring(dev, q_size,
		sizeof(*srq.rx_ring[0]), msg_size);
	BUG_ON((rv = fill_recv_q(dev, &srq)));
	if (srqi)
		*srqi = srq;
	else
		dev->srq = srq;

out:
	FOUT;
	return rv;
}

static void free_srq(struct u_dev *dev, struct srq_info *srqi)
{
	struct srq_info *srq = srqi == NULL ? &dev->srq : srqi;

	FIN;
	if (srq->rx_ring) {
		free_ioctx_ring(srq->rx_ring, dev->ib_dev,
			srq->srq_queue_size, srq->srq_msg_size);
		srq->rx_ring = NULL;
	}

	if (srq->srq) {
		ib_destroy_srq(srq->srq);
		srq->srq = NULL;
	}
	FOUT;
}

int post_recv(struct u_dev *dev, struct u_iu *iu, struct srq_info *srqi)
{
	struct srq_info *srq = srqi == NULL ? &dev->srq : srqi;
	struct ib_recv_wr wr;
	IB_DECLARE_BAD_RECV_WR(bad_wr);
	struct ib_sge list;
	static int one_trace = 1;
	int rv;

	//FIN;
	list.addr = iu->dma;
	list.length = iu->size;
	list.lkey = dev->mr->lkey;

	memset(&wr, 0, sizeof(wr));
	wr.next = NULL;
	//_D("index=%d\n", iu->index);
	wr.wr_id = iu->index << 1;
	wr.sg_list = &list;
	wr.num_sge = 1;
	if (one_trace) {
		one_trace = 0;
		trace("list.addr=%#llx, list.length=%u, list.lkey=%#x\n",
			list.addr, list.length, list.lkey);
	}
	BUG_ON((rv = ib_post_srq_recv(srq->srq, &wr, &bad_wr)));
	//FOUT;
	return rv;
}

/* initializing new IB device wrapper */
struct u_dev * init_dev(struct ib_device *device)
{
	struct u_dev *dev = NULL;
	int srq_size;
	int rv = 0;

	FIN;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		trace("failed to allocate udev\n");
		goto out;
	}

	dev->ib_dev = device;

	if ((rv = ib_query_device(dev->ib_dev, &dev->dev_attr))) {
		trace("query device failed (%d)\n", rv);
		goto free_dev;
	}

	dev->num_comp_vectors = min_t(int, num_online_cpus(),
		device->num_comp_vectors);

	dev->pd = ib_alloc_pd(device);
	if (IS_ERR(dev->pd)) {
		trace("ib_alloc_pd failed (%ld)\n", PTR_ERR(dev->pd));
		goto free_dev;
	}

#if defined(HAS_IB_GET_DMA_MR) && HAS_IB_GET_DMA_MR
	dev->mr = ib_get_dma_mr(dev->pd,
		IB_ACCESS_LOCAL_WRITE |
		IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE);
#else
	if (device->get_dma_mr)
		dev->mr = device->get_dma_mr(dev->pd,
			IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
			IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_ATOMIC);
#endif
	if (IS_ERR_OR_NULL(dev->mr)) {
		trace("ib_get_dma_mr failed (%ld)\n", PTR_ERR(dev->mr));
		goto err_pd;
	}
	srq_size = dev->dev_attr.max_srq_wr;
	trace("SRQ size %d, msg_size %d\n", srq_size, (int)PAGE_SIZE);
	if (create_srq(dev, srq_size, PAGE_SIZE, NULL)) {
		trace("%s ib_create_srq() failed.\n", device->name);
		goto err_pd;
	}
	INIT_LIST_HEAD(&dev->port_list);
	goto out;

err_pd:
	ib_dealloc_pd(dev->pd);

free_dev:
	kfree(dev);
	dev = NULL;

out:
	FOUT;
	return dev;
}

void free_udev(struct u_dev *dev)
{
	FIN;
	/* remove the srq */
	free_srq(dev, NULL);
	if (dev->mr) {
		ib_dereg_mr(dev->mr);
		dev->mr = NULL;
	}
	if (dev->pd) {
		trace("Before: dev->pd->__internal_mr=%p\n", dev->pd->__internal_mr);
		ib_dealloc_pd(dev->pd);
		dev->pd = NULL;
	}
	FOUT;
}

int get_device_phys_port_count(struct ib_device *device)
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

static int query_gid(struct ib_device *device, u8 port, int index,
	struct ib_port *ib_port, bool is_roce)
{
	void *gid_attr_ptr = NULL;
	union ib_gid *gid = &ib_port->gid;
	int rv = -1;

	//FIN;
#if HAS_IB_QUERY_GID
#	if IB_HAS_GID_ATTR
	gid_attr_ptr = is_roce ? &ib_port->gid_attr : NULL;
	/* Get default GID as HW GID */
	LINE;
	rv = ib_query_gid(ib, port, index, gid, gid_attr_ptr);
#	else
	LINE;
	rv = ib_query_gid(ib, port, index, gid);
#	endif
#else
#	if IB_HAS_GID_ATTR
#		if IB_HAS_RDMA_GET_GID_ATTR
		LINE;
		rv = rdma_query_gid(device, port, index, gid);
		if (rv)
			trace("failed to get gid attributes: rv = %d\n", rv);
		if (!rv && is_roce) {
			if (IS_ERR(gid_attr_ptr =
				(void *)rdma_get_gid_attr(device, port, index)))
				rv = PTR_ERR(gid_attr_ptr);
			else
				ib_port->gid_attr = *(struct ib_gid_attr *)gid_attr_ptr;
		}
#		else
		gid_attr_ptr = is_roce ? &ib_port->gid_attr : NULL;
		LINE;
		rv = ib_get_cached_gid(ib, port, index, gid, gid_attr_ptr);
#		endif
#	else
		LINE;
		rv = rdma_query_gid(ib, port, index, gid);
#	endif
#endif
	//FOUT;
	return rv;
}

static int find_gid(
	struct u_dev *dev, struct ib_port *ib_port, struct sockaddr_storage *a)
{
	enum rdma_link_layer ly;
	int sgid_family = -1;
	bool gid_matched = false;
	union ib_gid ip_gid;
	char gid_str1[GUID_SIZE + 1] = {0};
	char gid_str2[GUID_SIZE + 1] = {0};
	struct sockaddr_ib *q;
	int i;
	int rv = -1;

	FIN;
	ly = rdma_port_get_link_layer(dev->ib_dev, ib_port->port);
	if (((struct sockaddr_in *)a)->sin_family == AF_IB) {
		if (ly == IB_LINK_LAYER_INFINIBAND) {
			trace("device %s, port %d - is IB\n",
				dev->ib_dev->name, ib_port->port);
			rv = query_gid(dev->ib_dev, ib_port->port, 0, ib_port, false);
			trace("device %s, port %d - is IB %pI6\n",
				dev->ib_dev->name, ib_port->port, ib_port->gid.raw);
			if (!rv) {
				q = (struct sockaddr_ib *)a;
				if (!memcmp(ib_port->gid.raw, q->sib_addr.sib_raw,
					sizeof(ip_gid.raw))) {
					ib_port->gid_index = 0;
					ib_port->global = 0;
				}
				else
					trace("device GID %pI6 does not input GID %pI6\n",
						ib_port->gid.raw, q->sib_addr.sib_raw);
			}
			else
				trace("failed to query device GID: %d\n", rv);
		}
		else
			trace("device %s, port %d - is not an IB device\n",
				dev->ib_dev->name, ib_port->port);
	}
	else {
		if (ly == IB_LINK_LAYER_ETHERNET) {
			trace("device %s, port %d - is RoCE\n",
				dev->ib_dev->name, ib_port->port);
			ib_port->global = 1;
			rdma_ip2gid((struct sockaddr *)a, &ip_gid);
			format_gid_raw(ip_gid.raw, gid_str1);
			trace("local IP gid %s (%pIS)\n", gid_str1, (struct sockaddr *)a);
			for (i = 0; i < ib_port->attr.gid_tbl_len; ++i) {
				if ((rv = query_gid(
					dev->ib_dev, ib_port->port, i, ib_port, true))) {
					//trace("query_gid() failed on index %d\n", i);
					continue;
				}
				trace("table index %d\n", i);
				if (ib_port->gid.raw[0] == 0 && ib_port->gid.raw[1] == 0)
					sgid_family = AF_INET;
				if (!memcmp(ib_port->gid.raw, ip_gid.raw, sizeof(ip_gid.raw)))
					gid_matched = true;
				else {
					format_gid_raw(ib_port->gid.raw, gid_str2);
					trace("local IP gid %s is NOT the same as port gis %s\n",
						gid_str1, gid_str2);
					continue;
				}
				if (ib_port->gid_attr.gid_type == IB_GID_TYPE_ROCE_UDP_ENCAP &&
					sgid_family == a->ss_family && gid_matched) {
					format_gid_raw(ib_port->gid.raw, gid_str2);
					trace("gid table index %d (%s)is of type RoCE2 and AF_INET\n",
						i, gid_str2);
					ib_port->gid_index = i;
					rv = 0;
					break;
				}
				else {
					if (ib_port->gid_attr.gid_type != IB_GID_TYPE_ROCE_UDP_ENCAP)
						trace("gid table index %d (%s)is NOT of type RoCE2 - "
							  "it's type is %d\n",
							i, gid_str2, (int)ib_port->gid_attr.gid_type);
					else if (sgid_family != a->ss_family)
						trace("gid table index %d (%s)is NOT of family AF_INET\n",
							i, gid_str2);
				}
			}
		}
		else
			trace("device %s, port %d - is not a RoCE device\n",
				dev->ib_dev->name, ib_port->port);
	}
	FOUT;
	return rv;
}

struct ib_port * add_port(
	struct u_dev *dev, u8 port, struct sockaddr_storage *a)
{
	struct ib_port *ib_port;
	int rv;

	FIN;
	ib_port = kzalloc(sizeof *ib_port, GFP_KERNEL);
	if (!ib_port) {
		trace("failed to allocate ib_port\n");
		goto out;
	}
	ib_port->gid_index = -1;
	if ((rv = ib_query_port(dev->ib_dev, port, &ib_port->attr))) {
		trace("failed to query port attributes %d\n", rv);
		goto free_port;
	}

	init_completion(&ib_port->released);
	ib_port->udev = dev;
	ib_port->port = port;

	if ((rv = find_gid(dev, ib_port, a))) {
		trace("failed to find gid\n");
		goto free_port;
	}
	goto out;

free_port:
	kfree(ib_port);
	ib_port = NULL;

out:
	FOUT;
	return ib_port;
}

int rdma_cm_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
	struct dc_info *info = cm_id->context;

	FIN;
	trace("received RDMA_CM event %s (%d) (status=%d)\n",
		rdma_event_msg(event->event), (int)event->event, event->status);
	info->returned_cm_event = *event;
	info->returned_cm_id = cm_id;
	if (event->event == RDMA_CM_EVENT_CONNECT_RESPONSE) {
		if ((int)event->param.conn.private_data_len <= sizeof(info->payload))
			memcpy(info->payload, event->param.conn.private_data,
				event->param.conn.private_data_len);
		else
			trace("rdma event private data len %d exceed supported size %d\n",
				event->param.conn.private_data_len, (int)sizeof(info->payload));

	}
	else if (event->event == RDMA_CM_EVENT_REJECTED) {
		trace("received RDMA_CM_EVENT_REJECTED with status %d (%s)\n",
			event->status, rdma_reject_msg(cm_id, event->status));
	}
	if (info->on_cm_event)
		info->on_cm_event(info);
#if 0
	enum rdma_cm_event_type {
		RDMA_CM_EVENT_ADDR_RESOLVED,
		RDMA_CM_EVENT_ADDR_ERROR,
		RDMA_CM_EVENT_ROUTE_RESOLVED,
		RDMA_CM_EVENT_ROUTE_ERROR,
		RDMA_CM_EVENT_CONNECT_REQUEST,
		RDMA_CM_EVENT_CONNECT_RESPONSE,
		RDMA_CM_EVENT_CONNECT_ERROR,
		RDMA_CM_EVENT_UNREACHABLE,
		RDMA_CM_EVENT_REJECTED,
		RDMA_CM_EVENT_ESTABLISHED,
		RDMA_CM_EVENT_DISCONNECTED,
		RDMA_CM_EVENT_DEVICE_REMOVAL,
		RDMA_CM_EVENT_MULTICAST_JOIN,
		RDMA_CM_EVENT_MULTICAST_ERROR,
		RDMA_CM_EVENT_ADDR_CHANGE,
		RDMA_CM_EVENT_TIMEWAIT_EXIT
	};
#endif
	FOUT;
	return 0;
}

int select_dev_port(struct dc_info *info)
{
	struct u_dev *d;
	struct ib_port *p;
	int rv = -1;

	FIN;
	list_for_each_entry(d, &info->devs, link) {
		if (info->cm_id->device == d->ib_dev) {
			list_for_each_entry(p, &d->port_list, link) {
				if (p->port == info->cm_id->port_num) {
					if (p->gid_index != -1) {
						info->selected_dev = d;
						info->selected_port = p;
						rv = 0;
						break;
					}
				}
			}
		}
	}
	FOUT;
	return rv;
}

static void cq_completion_intr(struct ib_cq *cq, void *v)
{
	struct dc_info *info = v;
	struct ib_wc wc;
	int n;

	FIN;
	trace("info=%p, cq=%p\n", info, cq);
	n = ib_poll_cq(cq, 1, &wc);
	trace("n=%d\n", n);
	if (n > 0) {
		trace("wc.status=%d\n", wc.status);
	}
	else if (n == 0)
		trace("no completions\n");
	else
		trace("completions with error %d\n", n);
	if (n >= 0)
		ib_req_notify_cq(info->cq, IB_CQ_NEXT_COMP);
	FOUT;
}

static void cq_event_handler(struct ib_event *e, void *v)
{
	struct dc_info *info = v;

	FIN;
	trace("info=%p, event=%s\n", info, ib_event_msg(e->event));
	FOUT;
}

static void qp_event_handler(struct ib_event *e, void *v)
{
	struct dc_info *info = v;

	FIN;
	trace("info=%p, event=%s\n", info, ib_event_msg(e->event));
	FOUT;
}

static atomic_t cq_vector_value = ATOMIC_INIT(0);
int create_cq(struct dc_info *info)
{
	int vec = atomic_inc_return(&cq_vector_value) %
		info->selected_dev->num_comp_vectors;
	int rv;

	FIN;
	info->cq = nvmeib_create_cq(
		info->selected_dev->ib_dev,
		cq_completion_intr,
		cq_event_handler,
		info, N_COMP_ENTRIES, vec);
	if (IS_ERR_OR_NULL(info->cq)) {
		rv = PTR_ERR(info->cq);
		info->cq = NULL;
		trace("nvmeib_create_cq() failed %d\n", rv);
	}
	else
		ib_req_notify_cq(info->cq, IB_CQ_NEXT_COMP);
	FOUT;
	return info->cq ? 0 : -1; 
}

int create_qp(struct dc_info *info)
{
#if 1
	struct ib_qp_init_attr qp_init;
	int rv;

	FIN;
	memset(&qp_init, 0, sizeof(qp_init));
	qp_init.event_handler = qp_event_handler;
	qp_init.qp_context = info;
	qp_init.qp_type = IB_EXP_QPT_DC_INI;
	qp_init.send_cq = info->cq;
	qp_init.recv_cq = info->cq;
	//qp_init.srq = info->selected_dev->srq.srq;
	qp_init.cap.max_send_wr = N_COMP_ENTRIES / 2;
	qp_init.cap.max_send_sge = 1;
	//qp_init.cap.max_recv_wr = 0;
	//qp_init.cap.max_recv_sge = 0;
	trace("Before: info->selected_dev->pd->__internal_mr=%p\n",
		info->selected_dev->pd->__internal_mr);
	info->qp = ib_create_qp(info->selected_dev->pd, &qp_init);
	if (IS_ERR_OR_NULL(info->qp)) {
		rv = PTR_ERR(info->qp);
		trace("ib_create_qp() failed with %d\n", rv);
		info->qp = NULL;
	}
	else {
		trace("After: info->selected_dev->pd->__internal_mr=%p\n",
			info->selected_dev->pd->__internal_mr);
		trace("max cap: qp_init.cap.max_send_wr=%d, "
			  "qp_init.cap.max_send_sge=%d, qp->pd=%p\n",
			qp_init.cap.max_send_wr,
			qp_init.cap.max_send_sge,
			info->qp->pd);
	}
	FOUT;
	return info->qp ? 0 : -1;
#else
	struct ib_exp_qp_init_attr qp_init;
	int rv;

	FIN;
	memset(&qp_init, 0, sizeof(qp_init));
	qp_init.event_handler = qp_event_handler;
	qp_init.qp_context = info;
	qp_init.qp_type = IB_EXP_QPT_DC_INI;
	qp_init.send_cq = info->cq;
	qp_init.recv_cq = info->cq;
	qp_init.srq = info->selected_dev->srq.srq;
	qp_init.sq_sig_type = IB_SIGNAL_ALL_WR;
	qp_init.cap.max_send_wr = N_COMP_ENTRIES / 2;
	qp_init.cap.max_send_sge = 1;
	qp_init.cap.max_recv_wr = 0;
	qp_init.cap.max_recv_sge = 0;
	qp_init.port_num = info->selected_port->port;
	trace("Before: info->selected_dev->pd->__internal_mr=%p\n",
		info->selected_dev->pd->__internal_mr);
	info->qp = info->selected_dev->ib_dev->ops.exp_create_qp(
		info->selected_dev->pd, &qp_init, NULL);
	if (IS_ERR_OR_NULL(info->qp)) {
		rv = PTR_ERR(info->qp);
		trace("ib_create_qp() failed with %d\n", rv);
		info->qp = NULL;
		goto out;
	}
	trace("After: info->selected_dev->pd->__internal_mr=%p\n",
		info->selected_dev->pd->__internal_mr);
	/*
	 * We don't track XRC QPs for now, because they don't have PD
	 * and more importantly they are created internaly by driver,
	 * see mlx5 create_dev_resources() as an example.
	 */
	info->qp->res.type = RDMA_RESTRACK_QP;
	if (qp_init.qp_type < IB_QPT_XRC_INI)
		rdma_restrack_kadd(&info->qp->res);
	else
		rdma_restrack_dontrack(&info->qp->res);
	info->qp->real_qp = info->qp;
	info->qp->device = info->selected_dev->ib_dev;
	info->qp->pd = info->selected_dev->pd;
	info->qp->send_cq = qp_init.send_cq;
	if (qp_init.send_cq)
		atomic_inc(&qp_init.send_cq->usecnt);
	info->qp->recv_cq = qp_init.recv_cq;
	if (qp_init.recv_cq)
		atomic_inc(&qp_init.recv_cq->usecnt);
	info->qp->srq = qp_init.srq;
	if (qp_init.srq)
		atomic_inc(&qp_init.srq->usecnt);
	info->qp->xrcd = NULL;
	info->qp->rwq_ind_tbl = qp_init.rwq_ind_tbl;
	if (qp_init.rwq_ind_tbl)
		atomic_inc(&qp_init.rwq_ind_tbl->usecnt);
	info->qp->event_handler = qp_init.event_handler;
	info->qp->qp_context = qp_init.qp_context;
	info->qp->qp_type = qp_init.qp_type;
	atomic_set(&info->qp->usecnt, 0);
	info->qp->mrs_used = 0;
	spin_lock_init(&info->qp->mr_lock);
	INIT_LIST_HEAD(&info->qp->rdma_mrs);
	INIT_LIST_HEAD(&info->qp->sig_mrs);
	/*
	 * Note: all hw drivers guarantee that max_send_sge is lower than
	 * the device RDMA WRITE SGE limit but not all hw drivers ensure that
	 * max_send_sge <= max_sge_rd.
	 */
	info->qp->max_write_sge = qp_init.cap.max_send_sge;
	info->qp->max_read_sge = min_t(u32, qp_init.cap.max_send_sge,
		info->selected_dev->ib_dev->attrs.max_sge_rd);
	if (qp_init.create_flags & IB_QP_CREATE_INTEGRITY_EN)
		info->qp->integrity_en = true;
	atomic_inc(&info->selected_dev->pd->usecnt);

	goto out;

out:
	FOUT;
	return info->qp ? 0 : -1;
#endif
}

int modify_qp(struct dc_info *info)
{
	int attr_mask = 0;
	int rv = -1;

	FIN;
	/* modify QP to INIT */
	trace("KM-------------------->: INIT\n");
	{
		struct ib_qp_attr attr = {
			.qp_state = IB_QPS_INIT,
			.pkey_index = 0,
			.port_num = info->cm_id->port_num,
			/*.dct_key = DC_KEY,*/
		};
		attr_mask = IB_QP_STATE | IB_QP_PKEY_INDEX | IB_QP_PORT/* | IB_QP_DC_KEY*/;
		if ((rv = ib_modify_qp(info->qp, &attr, attr_mask))) {
			trace("failed to INIT QP - %d\n", rv);
			goto out;
		}
	}

	trace("KM<-------------------_: INIT\n");
	trace("KM-------------------->: RTR: mtu=%d, max_msg_sz=%d\n",
		info->selected_port->attr.active_mtu,
		info->selected_port->attr.max_msg_sz);
	/* modify QP to RTR */
	{
		struct ib_qp_attr attr = {
			.qp_state = IB_QPS_RTR,
			.path_mtu = info->selected_port->attr.active_mtu,
			.min_rnr_timer = 0x10,
			.rq_psn = 0,
			.ah_attr = {
				/*.is_global = info->selected_port->global,*/
				.type = RDMA_AH_ATTR_TYPE_ROCE,
				.ah_flags = IB_AH_GRH,
				.sl = 0,
				.port_num = info->cm_id->port_num,
				.ib.src_path_bits = 0,
				.grh.hop_limit = 1,
				.grh.sgid_index = info->selected_port->gid_index,
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
		if ((rv = ib_modify_qp(info->qp, &attr, attr_mask))) {
			trace("failed to RTR QP - %d\n", rv);
			goto out;
		}
	}
	trace("KM<--------------------: RTR\n");
	trace("KM-------------------->: RTS\n");
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
		if ((rv = ib_modify_qp(info->qp, &attr, attr_mask))) {
			trace("failed to RTS QP - %d\n", rv);
			goto out;
		}
	}
	rv = 0;
	trace("KM<--------------------: RTS\n");

out:
	FOUT;
	return rv;
}

void init_conn_param(struct dc_info *info, struct rdma_conn_param *conn_param)
{
	FIN;
	memset(conn_param, 0, sizeof(*conn_param));
	conn_param->responder_resources = 16;
	conn_param->initiator_depth = 16;
	conn_param->retry_count = 7;
	conn_param->rnr_retry_count = 7;
	FOUT;
}

