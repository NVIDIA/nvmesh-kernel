#include "kr_incs.h"
#include "kr_version.h"
#include "ib_incs.h"
#include "c_dc.h"
#include "u.h"
#include "u_dc.h"

#include "u_dc.c"

#include <linux/string.h>
#include <linux/parser.h>
#include <linux/mlx5/qp.h>
#include <linux/inet.h>
#include <rdma/ib.h>

struct c_dc_info {
	struct dc_info info;
	__be32 c_ipv4_address;
	struct sockaddr_storage c_sin;
	struct work_struct connect_work;
	struct work_struct send_work;
	struct completion comp;
	u64 dct_key;
	u32 dct_num;
	u32 lid;
	__be64 subnet_prefix;
	__be64 interface_id;
	u32 psn;
	u32 srqn;
	u32 gid_index;
	u32 mtu;
	struct u_iu *iu;
	u64 raddr;
	u32 rkey;
};

static void post_add_one(struct dc_info *info)
{
	struct c_dc_info *cinfo = container_of(info, struct c_dc_info, info);
	
	FIN;
	queue_work(info->wq, &cinfo->connect_work);
	FOUT;
}

static void pre_remove_one(struct dc_info *info)
{
#if 0
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
#endif
}

static int connect_path(struct c_dc_info *cinfo)
{
	struct dc_info *info = &cinfo->info;
	int qp_attr_mask = 0;
	struct ib_qp_attr attr;
	struct rdma_conn_param conn_param;
	struct rdma_dct_info *b;
	int rv = -1;

	FIN;
	init_conn_param(info, &conn_param);
	nvmeib_reinit_completion(&cinfo->comp);
	if ((rv = rdma_connect(info->cm_id, &conn_param))) {
		trace("rdma_connect() failed %d\n", rv);
		goto out;
	}
	rv = -1;
	if (wait_for_completion_interruptible_timeout(
		&cinfo->comp, WAIT_FOR_SOMETHING) <= 0) {
		trace("failed to wait for rdma_connect()\n");
		goto out;
	}
	if (info->returned_cm_event.event != RDMA_CM_EVENT_CONNECT_RESPONSE) {
		trace("waiting for RDMA_CM_EVENT_CONNECT_RESPONSE but received %s\n",
			rdma_event_msg(info->returned_cm_event.event));
		goto out;
	}
	else {
		b = (struct rdma_dct_info *)info->payload;
		cinfo->dct_key = be64_to_cpu(b->dct_key);
		cinfo->dct_num = be32_to_cpu(b->dct_num);
		cinfo->lid = be32_to_cpu(b->lid);
		cinfo->subnet_prefix = b->subnet_prefix;
		cinfo->interface_id = b->interface_id;
		cinfo->psn = be32_to_cpu(b->psn);
		cinfo->srqn = be32_to_cpu(b->srqn);
		cinfo->gid_index = be32_to_cpu(b->gid_index);
		cinfo->mtu = be32_to_cpu(b->mtu);
		cinfo->raddr = be64_to_cpu(b->raddr);
		cinfo->rkey = be32_to_cpu(b->rkey);
		trace("received: dct_key=%#llx\n", cinfo->dct_key);
		trace("received: dct_num=%#x\n", cinfo->dct_num);
		trace("received: lid=%d\n", cinfo->lid);
		trace("received: subnet_prefix=%#llx\n", cinfo->subnet_prefix);
		trace("received: interface_id=%#llx\n", cinfo->interface_id);
		trace("received: psn=%d\n", cinfo->psn);
		trace("received: srqn=%d\n", cinfo->srqn);
		trace("received: gid_index=%d\n", cinfo->gid_index);
		trace("received: mtu=%d\n", cinfo->mtu);
		trace("received: raddr=%#llx\n", cinfo->raddr);
		trace("received: rkey=%#x\n", cinfo->rkey);
	}
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_RTR;
	if (rdma_init_qp_attr(info->cm_id, &attr, &qp_attr_mask)) {
		trace("rdma_init_qp_attr() failed\n");
		goto out;
	}
	info->ah = rdma_create_user_ah(info->selected_dev->pd, &attr.ah_attr, NULL);
	if (IS_ERR_OR_NULL(info->ah)) {
		rv = PTR_ERR(info->ah);
		info->ah = NULL;
		trace("rdma_create_ah() failed %d\n", rv);
		goto out;
	}
	if (rdma_accept(info->cm_id, NULL)) {
		trace("rdma_accept() failed\n");
		goto out;
	}
	rv = 0;

out:
	FOUT;
	return rv;
}

static int lookup_n_connect_path(struct c_dc_info *cinfo)
{
	struct dc_info *info = &cinfo->info;
	int rv = -1;

	FIN;
	info->cm_id = rdma_create_id(
		rdma_cm_handler, info,
		info->layer_type == DC_PROC_IP_CHAR ? RDMA_PS_TCP : RDMA_PS_IB,
		IB_QPT_RC);
	if (info->cm_id == NULL) {
		trace("fail to create rdma_cm_id\n");
		goto out;
	}
	init_completion(&cinfo->comp);
	rv = rdma_resolve_addr(info->cm_id, (struct sockaddr *)&cinfo->c_sin,
		(struct sockaddr *)&info->s_sin, 2000);
	if (rv) {
		trace("failed to call rdma_resolve_addr()\n");
		goto out;
	}
	rv = wait_for_completion_interruptible_timeout(
		&cinfo->comp, WAIT_FOR_SOMETHING);
	if (rv <= 0) {
		trace("failed to wait for rdma_resolve_addr()\n");
		goto out;
	}
	if (info->returned_cm_event.event != RDMA_CM_EVENT_ADDR_RESOLVED) {
		trace("waiting for RDMA_CM_EVENT_ADDR_RESOLVED but received %s\n",
			rdma_event_msg(info->returned_cm_event.event));
		goto out;
	}
	nvmeib_reinit_completion(&cinfo->comp);
	rv = rdma_resolve_route(info->cm_id, 2000);
	if (rv) {
		trace("failed to call rdma_resolve_route()\n");
		goto out;
	}
	rv = wait_for_completion_interruptible_timeout(
		&cinfo->comp, WAIT_FOR_SOMETHING);
	if (rv <= 0) {
		trace("failed to wait for rdma_resolve_route()\n");
		goto out;
	}
	if (info->returned_cm_event.event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
		trace("waiting for RDMA_CM_EVENT_ROUTE_RESOLVED but received %s\n",
			rdma_event_msg(info->returned_cm_event.event));
		goto out;
	}
	if (select_dev_port(info)) {
		trace("failed to select udev and port to match route\n");
		goto out;
	}
	else
		trace("info->selected_dev=%p, info->select_dev->ib_dev=%p\n",
			info->selected_dev, info->selected_dev->ib_dev);
	rv = connect_path(cinfo);

out:
	FOUT;
	return rv;
}

static void connect(struct work_struct *w)
{
	struct c_dc_info *cinfo = container_of(w, struct c_dc_info, connect_work);
	struct dc_info *info = &cinfo->info;

	FIN;
	if (lookup_n_connect_path(cinfo)) {
		trace("failed lookuo_n_connect_path\n");
		goto out;
	}
	goto out;
	if (create_cq(info)) {
		trace("failed to create client CQ\n");
		goto out;
	}
	if (create_qp(info)) {
		trace("failed to create client QP\n");
		goto out;
	}
	if (modify_qp(info)) {
		trace("failed to modify client QP\n");
		goto out;
	}
	trace("info->selected_dev=%p, info->select_dev->ib_dev=%p\n",
		info->selected_dev, info->selected_dev->ib_dev);
	queue_work(info->wq, &cinfo->send_work);

out:
	FOUT;
}

struct ib_dc_wr {
        struct ib_rdma_wr       wr;
        struct ib_ah            *ah;
        u64                     dct_access_key;
        u32                     dct_number;
};

static void send(struct work_struct *w)
{
	struct c_dc_info *cinfo = container_of(w, struct c_dc_info, send_work);
	struct dc_info *info = &cinfo->info;
	struct ib_dc_wr wr;
	const struct ib_send_wr *bad_wr;
	struct ib_sge sg;
	int rv;

	FIN;
	memset(&wr, 0, sizeof(wr));
	if ((cinfo->iu = alloc_ioctx(
		info->selected_dev, sizeof(*cinfo->iu), PAGE_SIZE))) {
		rv = snprintf(cinfo->iu->buf, cinfo->iu->size, "%s", "hello ofer");
		trace("going to send %d bytes\n", rv);
		sg.addr = cinfo->iu->dma;
		sg.length = rv;
		sg.lkey = info->selected_dev->mr->lkey;
		wr.wr.wr.opcode = IB_WR_SEND;//IB_WR_RDMA_WRITE;
		wr.wr.remote_addr = cinfo->raddr;
		wr.wr.rkey = cinfo->rkey;
		wr.wr.wr.wr_id = 1234567;
		wr.wr.wr.num_sge = 1;
		wr.wr.wr.sg_list = &sg;
		wr.wr.wr.send_flags = IB_SEND_SIGNALED;
		wr.wr.wr.next = NULL;
		wr.dct_access_key = DC_KEY;
		wr.dct_number = cinfo->dct_num;
		wr.ah = info->ah;
		if ((rv = ib_post_send(info->qp, &(wr.wr.wr), &bad_wr)) < 0)
			trace("ib_post_send() returned %d\n", rv);
		else
			trace("ib_post_send() rv was ok\n");
		trace("wr.ah=%p\n", wr.ah);
	}
	else
		trace("failed to allocate send buffer...\n");

	FOUT;
}

static ssize_t store_ipv4_address(struct dc_info *p, char *page, size_t count)
{
	int i, ret;
	unsigned int octets_c[4];
	unsigned int octets_s[4];
	unsigned int port_s;
	struct c_dc_info *info = container_of(p, struct c_dc_info, info);
	struct sockaddr_in *q;

	FIN;
	p->last_error = -1;
	ret = sscanf(page, "%c:%3u.%3u.%3u.%3u,%3u.%3u.%3u.%3u:%5u",
		&p->layer_type,
		&octets_c[3], &octets_c[2], &octets_c[1], &octets_c[0],
		&octets_s[3], &octets_s[2], &octets_s[1], &octets_s[0], &port_s);
	if (ret != 9) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(octets_c); i++) {
		if (octets_c[i] > 255) {
			ret = -ERANGE;
			goto out;
		}
		be32_add_cpu(&info->c_ipv4_address, octets_c[i] << (i * 8));
	}
	trace("client bind ip is %pI4\n", &info->c_ipv4_address);
	for (i = 0; i < ARRAY_SIZE(octets_s); i++) {
		if (octets_s[i] > 255) {
			ret = -ERANGE;
			goto out;
		}
		be32_add_cpu(&info->info.s_ipv4_address, octets_s[i] << (i * 8));
	}
	if (port_s == 0) {
		ret = -EINVAL;
		goto out;
	}
	if (port_s >= (u16)-1) {
		ret = -ERANGE;
		goto out;
	}
	info->info.s_ipv4_port = htons(port_s);
	q = (struct sockaddr_in *)&p->s_sin;
	q->sin_family = PF_INET;
	q->sin_addr.s_addr = p->s_ipv4_address;
	q->sin_port = p->s_ipv4_port;
	q = (struct sockaddr_in *)&info->c_sin;
	q->sin_family = PF_INET;
	q->sin_addr.s_addr = info->c_ipv4_address;
	q->sin_port = 0;
	trace("client ip %pIS server ip:port is %pIS:%u\n",
		(struct sockaddr *)&p->s_sin,
		(struct sockaddr *)&info->c_sin, ntohs(info->info.s_ipv4_port));
	p->last_error = 0;

out:
	FOUT;
	return count;
}

static ssize_t store_ib_address(struct dc_info *p, char *page, size_t count)
{
	int ret;
	char line[DC_PROC_IB_ADDR_BUFFER * 2 + 1] = {0};
	char *copied_line;
	char *splitted_str;
	char *pp;
	char clt[DC_PROC_IB_ADDR_BUFFER ] = {0};
	char srv[DC_PROC_IB_ADDR_BUFFER] = {0};
	bool copied_clt = false;
	bool copied_srv = false;
	struct c_dc_info *info = container_of(p, struct c_dc_info, info);
	struct sockaddr_ib *q;

	FIN;
	p->last_error = -1;
	ret = sscanf(page, "%c:%s", &p->layer_type, line);
	if (ret != 2) {
		trace("sscanf returned %d - should return 2\n", ret);
		ret = -EINVAL;
		goto out;
	}
	if (!(copied_line = kstrdup(line, GFP_KERNEL))) {
		trace("kstrdup(0 failed\n");
		ret = -ENOMEM;
		goto out;
	}
	else
		splitted_str = copied_line;
	while ((pp = strsep(&splitted_str, ",\n")) != NULL) {
		if (!*pp)
			continue;
		if (!copied_clt) {
			strncpy(clt, pp, DC_PROC_IB_ADDR_BUFFER - 1);
			copied_clt = true;
		}
		else if (!copied_srv) {
			strncpy(srv, pp, DC_PROC_IB_ADDR_BUFFER - 1);
			copied_srv = true;
		}
		if (copied_clt && copied_srv)
			break;
	}
	kfree(copied_line);
	if (!(copied_clt && copied_srv)) {
		trace("failed to fetch: client: %s, server: %s\n",
			copied_clt ? "OK" : "FAILED", copied_srv ? "OK" : "FAILED");
		ret = -EINVAL;
		goto out;
	}
	trace("client input gid %s\n", clt);
	q = (struct sockaddr_ib *)&info->c_sin;
	if (in6_pton(clt, -1, q->sib_addr.sib_raw, -1, NULL))
		trace("client bind GID is %pI6\n", q->sib_addr.sib_raw);
	else {
		trace("fail to convert client bind address into a valid IB addrsss\n");
		goto out;
	}
	q->sib_family = AF_IB;
	q->sib_sid = cpu_to_be64(NVMEIB_EXCELERO_SERVICE_ID);
	q->sib_sid_mask = cpu_to_be64(NVMEIB_EXCELERO_SERVICE_ID_MASK);
	q->sib_pkey = cpu_to_be16(NVMEIB_EXCELERO_PKEY);
	trace("server input gid %s\n", srv);
	q = (struct sockaddr_ib *)&p->s_sin;
	if (in6_pton(srv, -1, q->sib_addr.sib_raw, -1, NULL))
		trace("server GID is %pI6\n", q->sib_addr.sib_raw);
	else {
		trace("fail to convert server address into a valid IB addrsss\n");
		goto out;
	}
	q->sib_family = AF_IB;
	q->sib_sid = cpu_to_be64(
		((NVMEIB_EXCELERO_SERVICE_ID & NVMEIB_EXCELERO_SERVICE_ID_MASK) |
		 RDMA_IB_IP_PS_IB));
	q->sib_sid_mask = cpu_to_be64(NVMEIB_EXCELERO_SERVICE_ID_MASK);
	q->sib_pkey = cpu_to_be16(NVMEIB_EXCELERO_PKEY);

	trace("client GID %pI6 server GID is %pI6\n",
		((struct sockaddr_ib *)&info->c_sin)->sib_addr.sib_raw,
		((struct sockaddr_ib *)&p->s_sin)->sib_addr.sib_raw);
	p->last_error = 0;

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
	struct c_dc_info *cinfo = container_of(info, struct c_dc_info, info);

	FIN;
	complete(&cinfo->comp);
	FOUT;
}

static void stop(struct dc_info *p)
{
	struct c_dc_info *info = container_of(p, struct c_dc_info, info);

	FIN;
	if (info->iu) {
		free_ioctx(p->selected_dev->ib_dev, info->iu, PAGE_SIZE);
		info->iu = NULL;
	}
	FOUT;
}

static int create_c_info(struct c_dc_info **info)
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
		trace("failed to allocate client info\n");
		rv = -1;
		goto out;
	}
	(*info)->info.on_cm_event = on_cm_event;
	(*info)->info.post_add_one = post_add_one;
	(*info)->info.pre_remove_one = pre_remove_one;
	(*info)->info.bind_sin = &(*info)->c_sin;
	INIT_WORK(&(*info)->connect_work, connect);
	INIT_WORK(&(*info)->send_work, send);
	if (!(rv = init_dc_info(&(*info)->info, DC_PROC_C_STR,
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

static void free_c_info(struct c_dc_info *info)
{
	FIN;
	free_dc_info(&info->info, true);
	kfree(info);
	FOUT;
}

static struct c_dc_info *_info;
int c_dc_in_(void) /* Constructor */
{
	int rv = 0;

	FIN;
	trace("hello from the dc client\n");
	if (!(create_c_info(&_info))) {
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

void c_dc_out_(void) /* Destructor */
{
	FIN;
	trace("bye from the dc client %p\n", _info);
	if (_info)
		free_c_info(_info);
	FOUT;
}

