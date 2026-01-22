/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "kr_incs.h"
#include "kr_version.h"
#include "ib_incs.h"
#include "s_dc.h"
#include "u.h"
#include "u_dc.h"

#include "u_dc.c"
#include <linux/inet.h>
#include <rdma/ib.h>

struct s_dc_info {
	struct dc_info info;
	struct work_struct listen_work;
	struct work_struct accept_work;
	struct ib_dct *dct;
	/* page for RDMA tests */
	void *page_ptr;
	u64 page;
};

static void post_add_one(struct dc_info *info)
{	
	struct s_dc_info *sinfo = container_of(info, struct s_dc_info, info);

	FIN;
	queue_work(info->wq, &sinfo->listen_work);
	FOUT;
}

static void pre_remove_one(struct dc_info *info)
{
}

static void dct_event_handler(struct ib_event *event, void *context_ptr)
{
	struct s_dc_info *sinfo = context_ptr;
	FIN;
	trace("info=%p, event=%s\n", sinfo, ib_event_msg(event->event));
	FOUT;
}

static int create_rdma_page(struct s_dc_info *sinfo)
{
	struct dc_info *info = &sinfo->info;

	FIN;
	sinfo->page_ptr = ib_dma_alloc_coherent(
		info->selected_dev->ib_dev, PAGE_SIZE, &sinfo->page, GFP_KERNEL);
	FOUT;
	return sinfo->page_ptr ? 0 : -1;
}

static int create_dct(struct s_dc_info *sinfo)
{
	struct dc_info *info = &sinfo->info;
    struct ib_dct_init_attr dctattr = {
		.pd = info->selected_dev->pd,
        .cq = info->cq,
		.srq = info->selected_dev->srq.srq,
        .dc_key = DC_KEY,
        .port = info->cm_id->port_num,
        .access_flags =
			IB_ACCESS_REMOTE_READ |
			IB_ACCESS_REMOTE_WRITE |
			IB_ACCESS_REMOTE_ATOMIC,
        .min_rnr_timer = 2,
        /*.tclass = 16,*/
        .flow_label = 0,
        .mtu = info->selected_port->attr.active_mtu,
        .pkey_index = 0,
        .gid_index = info->selected_port->gid_index,
        .hop_limit = 1,
        .create_flags = 0,
        .inline_size = 0,
		.event_handler = dct_event_handler,
		.dct_context = sinfo
    };
	int rv;

	FIN;
	trace("dct_port=%u\n", dctattr.port);
	sinfo->dct = ib_exp_create_dct(info->selected_dev->pd, &dctattr, NULL);
	if (IS_ERR_OR_NULL(sinfo->dct)) {
		rv = PTR_ERR(sinfo->dct);
		trace("ib_exp_create_dct() failed with %d\n", rv);
		sinfo->dct = NULL;
	}
	else {
		sinfo->dct->device = sinfo->info.selected_dev->ib_dev;
		sinfo->dct->event_handler = dctattr.event_handler;
		sinfo->dct->dct_context = dctattr.dct_context;
	}

	rv = sinfo->dct ? 0 : -1;
	FOUT;
	return rv;
}

static void listen(struct work_struct *w)
{
	struct s_dc_info *sinfo = container_of(w, struct s_dc_info, listen_work);
	struct dc_info *info = &sinfo->info;

	FIN;
	info->cm_id = rdma_create_id(
		rdma_cm_handler, info,
		info->layer_type == DC_PROC_IP_CHAR ? RDMA_PS_TCP : RDMA_PS_IB,
		IB_QPT_RC);
	if (info->cm_id == NULL) {
		trace("fail to create rdma_cm_id\n");
		goto out;
	}
	if (rdma_bind_addr(info->cm_id, (struct sockaddr *)&info->s_sin)) {
		trace("failed to call rdma_bind_addr()\n");
		goto out;
	}
	if (select_dev_port(info)) {
		trace("failed to select udev and port to match binded udev/port\n");
		goto out;
	}
	if (rdma_listen(info->cm_id, 3)) {
		trace("failed to call rdma_listen()\n");
		goto out;
	}
	if (create_cq(info)) {
		trace("failed to create server CQ\n");
		goto out;
	}
	if (create_rdma_page(sinfo)) {
		trace("failed to allocate page for RDMA\n");
		goto out;
	}
	if (create_dct(sinfo)) {
		trace("failed to create server DCT\n");
		goto out;
	}

out:
	FOUT;
}

static void accept(struct work_struct *w)
{
	struct s_dc_info *sinfo = container_of(w, struct s_dc_info, accept_work);
	struct dc_info *info = &sinfo->info;
	struct rdma_conn_param conn_param;
	struct rdma_dct_info *b;

	FIN;
	if (info->returned_cm_event.event == RDMA_CM_EVENT_CONNECT_REQUEST) {
		trace("received connection request from peer %pIS to %pIS\n",
			(struct sockaddr *)&info->returned_cm_id->route.addr.dst_addr,
			(struct sockaddr *)&info->returned_cm_id->route.addr.src_addr);
		init_conn_param(info, &conn_param);
		conn_param.qp_num = sinfo->dct->dct_num;
		b = (struct rdma_dct_info *)info->payload;
		b->dct_key = cpu_to_be64(DC_KEY);
		b->dct_num = cpu_to_be32(sinfo->dct->dct_num);
		b->lid = cpu_to_be32(info->selected_port->attr.lid);
		b->subnet_prefix = info->selected_port->gid.global.subnet_prefix;
		b->interface_id = info->selected_port->gid.global.interface_id;
		b->psn = cpu_to_be32(0);
		b->srqn = cpu_to_be32(info->selected_dev->srq.srqn);
		b->gid_index = cpu_to_be32(info->selected_port->gid_index);
		b->mtu = cpu_to_be32(info->selected_port->attr.active_mtu);
		b->raddr = cpu_to_be64(sinfo->page);
		b->rkey = cpu_to_be32(info->selected_dev->mr->rkey);
		trace("dct_key=%#llx\n", (u64)DC_KEY);
		trace("dct_num=%#x\n", sinfo->dct->dct_num);
		trace("lid=%d\n", info->selected_port->attr.lid);
		trace("subnet_prefix=%#llx\n",
			info->selected_port->gid.global.subnet_prefix);
		trace("interface_id=%#llx\n",
			info->selected_port->gid.global.interface_id);
		trace("psn=0\n");
		trace("srqn=%d\n", info->selected_dev->srq.srqn);
		trace("gid_index=%d\n", info->selected_port->gid_index);
		trace("mtu=%d\n", info->selected_port->attr.active_mtu);
		trace("raddr=%#llx\n", sinfo->page);
		trace("rkey=%#x\n", info->selected_dev->mr->rkey);
		conn_param.private_data = info->payload;
		conn_param.private_data_len = sizeof(*b);
		trace("sent server DCT ID %#x\n", sinfo->dct->dct_num);
		if (rdma_accept(info->returned_cm_id, &conn_param))
			trace("failed to accept DCi connection request\n");
	}
	else if (info->returned_cm_event.event == RDMA_CM_EVENT_ESTABLISHED) {
		trace("received connection established...\n");
	}
	else {
		trace("waiting for RDMA_CM_EVENT_CONNECT_REQUEST | "
			  "RDMA_CM_EVENT_ESTABLISHED but received %s\n",
			rdma_event_msg(info->returned_cm_event.event));
	}
	FOUT;
}

static ssize_t store_ipv4_address(struct dc_info *info, char *page, size_t count)
{
	int i, ret;
	unsigned int octets_s[4];
	unsigned int port_s;
	struct sockaddr_in *q;

	FIN;
	info->last_error = -1;
	ret = sscanf(page, "%c:%3u.%3u.%3u.%3u:%5u", &info->layer_type,
		&octets_s[3], &octets_s[2], &octets_s[1], &octets_s[0], &port_s);
	if (ret != 5) {
		trace("fail to read server bind address\n");
		info->last_error = EINVAL;
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(octets_s); i++) {
		if (octets_s[i] > 255) {
			ret = -ERANGE;
			goto out;
		}
		be32_add_cpu(&info->s_ipv4_address, octets_s[i] << (i * 8));
	}
	if (port_s == 0) {
		ret = -EINVAL;
		goto out;
	}
	if (port_s >= (u16)-1) {
		ret = -ERANGE;
		goto out;
	}
	info->s_ipv4_port = htons(port_s);
	q = (struct sockaddr_in *)&info->s_sin;
	q->sin_family = PF_INET;
	q->sin_addr.s_addr = info->s_ipv4_address;
	q->sin_port = info->s_ipv4_port;
	trace("server ip:port is %pIS:%u\n",
		(struct sockaddr *)&info->s_sin, ntohs(info->s_ipv4_port));
	info->last_error = 0;

out:
	FOUT;
	return count;
}

static ssize_t store_ib_address(struct dc_info *info, char *page, size_t count)
{
	int ret;
	char srv[DC_PROC_IB_ADDR_BUFFER] = {0};
	struct sockaddr_ib *q;

	FIN;
	trace("count = %ld\n", count);
	info->last_error = -1;
	ret = sscanf(page, "%c:%s", &info->layer_type, srv);
	if (ret != 2) {
		ret = -EINVAL;
		goto out;
	}
	trace("server input gid %s\n", srv);
	q = (struct sockaddr_ib *)&info->s_sin;
	if (in6_pton(srv, -1, q->sib_addr.sib_raw, -1, NULL))
		trace("server bind GID is %pI6\n", q->sib_addr.sib_raw);
	else {
		trace("fail to convert server address into a valid IB addrsss\n");
		goto out;
	}
	q->sib_family = AF_IB;
	q->sib_sid = cpu_to_be64(NVMEIB_SERVICE_ID);
	q->sib_sid_mask = cpu_to_be64(NVMEIB_SERVICE_ID_MASK);
	q->sib_pkey = cpu_to_be16(NVMEIB_PKEY);
	trace("server GID is %pI6\n",
		((struct sockaddr_ib *)&info->s_sin)->sib_addr.sib_raw);
	info->last_error = 0;

out:
	FOUT;
	return count;
}

static ssize_t store_ipv4_ib_address(
	struct dc_info *p, char *page, size_t count)
{
	char c;

	FIN;
	if (sscanf(page, "%c", &c) != 1)
		trace("failed to get the network layer type\n");
	else if (c == DC_PROC_IP_CHAR)
		store_ipv4_address(p, page, count);
	else if (c == DC_PROC_IB_CHAR)
		store_ib_address(p, page, count);
	else
		trace("Unsupported network layer type %c\n", c);
	FOUT;
	return count;
}

static void on_cm_event(struct dc_info *info)
{
	struct s_dc_info *sinfo = container_of(info, struct s_dc_info, info);

	FIN;
	queue_work(sinfo->info.wq, &sinfo->accept_work);
	FOUT;
}

static void stop(struct dc_info *p)
{
	struct s_dc_info *info = container_of(p, struct s_dc_info, info);

	FIN;
	if (info->dct) {
		trace("destroying DCT\n");
		if (info->dct->device) {
			trace("info->dct->device=%p\n", info->dct->device);
			ib_exp_destroy_dct(info->dct, NULL);
			info->dct = NULL;
		}
		else
			trace("info->dct->device=NULL\n");
	}
	if (info->page_ptr)
		ib_dma_free_coherent(info->info.selected_dev->ib_dev, PAGE_SIZE,
			info->page_ptr, info->page);
	FOUT;
}

static int create_s_info(struct s_dc_info **info)
{
	int rv;
	bool allocated;

	FIN;
	if (!*info) {
		allocated = true;
		*info = kzalloc(sizeof(**info), GFP_KERNEL);
	}
	else
		allocated = false;
	if (!*info) {
		trace("failed to allocate server info\n");
		rv = -1;
		goto out;
	}
	(*info)->info.on_cm_event = on_cm_event;
	(*info)->info.post_add_one = post_add_one;
	(*info)->info.pre_remove_one = pre_remove_one;
	(*info)->info.bind_sin = &(*info)->info.s_sin;
	INIT_WORK(&(*info)->listen_work, listen);
	INIT_WORK(&(*info)->accept_work, accept);
	if (!(rv = init_dc_info(&(*info)->info, DC_PROC_S_STR,
		store_ipv4_ib_address, stop)))
		goto err;
	rv = 0;
	goto out;

err:
	if (!allocated) {
		kfree(*info);
		*info = NULL;
		rv = -1;
	}

out:
	FOUT;
	return rv;
}

static void free_s_info(struct s_dc_info *info)
{
	free_dc_info(&info->info, false);
}

static struct s_dc_info *_info;
int s_dc_in_(void) /* Constructor */
{
	int rv = 0;

	FIN;
	trace("hello from the dc server\n");
	if (!(create_s_info(&_info))) {
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

void s_dc_out_(void) /* Destructor */
{
	FIN;
	trace("bye from the dc server %p\n", _info);
	if (_info)
		free_s_info(_info);
	FOUT;
}



