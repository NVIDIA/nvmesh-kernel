#include "xkr_incs.h"
#include "xkr_version.h"
#include "xib_incs.h"
#include "manager.h"
#include "rdma_dev.h"
#include "utils.h"
#include "wth.h"
#include "server_tester.h"
#include "cli_srv_test_common.h"
#include "xtrace.h"

struct server_tester;

struct client_self {
	enum cli_srv_test type;
	struct client_info ci;
	struct server_service ser;
	void *path;
	union service_id sid;
	struct sockaddr_storage src;
	struct sockaddr_storage dst;
	void *local_rdma_device;
	void *buf;
	struct scatterlist *sg;
	struct memory_reg *mem;
	struct post_send_info info;
};

struct client_peer {
	struct cli_srv_payload payload;
};

struct _client {
	struct client_self me;
	struct client_peer him;
	struct server_tester *st;
	struct list_head link;
};

struct server_tester {
	struct manager *o;
	struct server_info si;
	struct wth *wth;
	/* list of struct local_address */
	struct list_head local_addrs;
	struct list_head clients;
	struct completion start_stop_comp;
	TIMER_LIST_INSTANCE(timer);
	bool has_timer;
};

static void on_listen(void *ctx, struct sockaddr_storage *s, int status);
struct client_info * on_accept(void *ctx, void *path,
	struct sockaddr_storage *s,
	struct sockaddr_storage *d,
	void *spayload, int spayload_len,
	void *dpayload, int dpayload_len);

static struct server_ft sft = {
	.listen = on_listen,
	.accept = on_accept,
};

static void on_connect(void *ctx, void *path, void *payload, int payload_len);
static void on_disconnect(void *ctx, void *path);

static struct client_ft cft = {
	.connect = on_connect,
	.disconnect = on_disconnect,
};

/**
 * Start server_tester requests
 */

struct listen_request {
	struct request_base r;
	struct sockaddr_storage s;
	int status;
};

static void listen_request_free(struct request_base *r)
{
	kfree(container_of(r, struct listen_request, r));
}

struct accept_request {
	struct request_base r;
	void *path;
	struct sockaddr_storage *s;
	struct sockaddr_storage *d;
	void *spayload;
	int spayload_len;
	void *dpayload;
	int dpayload_len;
	struct client_info **ci;
	struct completion *c;
};

static void accept_request_free(struct request_base *r)
{
	kfree(container_of(r, struct accept_request, r));
}

struct disconnect_request {
	struct request_base r;
	struct _client *c;
};

static void disconnect_request_free(struct request_base *r)
{
	kfree(container_of(r, struct disconnect_request, r));
}

struct request_request {
	struct request_base r;
	struct _client *c;
	int op;
	union service_id sid;
	void *msg;
	void *rsc;
	int index;
};

static void request_request_free(struct request_base *r)
{
	kfree(container_of(r, struct request_request, r));
}

/**
 * End manager requests
 */

static void remove_client(struct _client *c)
{
	FIN;
	manager_unregister_service(c->st->o, &c->me.sid);
	manager_disconnect(c->st->o, c->me.local_rdma_device, c, c->me.path);
	if (c->me.mem) {
		manager_ureg_mem(c->me.mem);
		c->me.mem = NULL;
	}
	kfree(c->me.sg);
	kfree(c->me.buf);
	kfree(c);
	FOUT;
}

static void on_connect(void *ctx, void *path, void *payload, int payload_len)
{
}

static int handle_disconnect(void *p, struct request_base *r)
{
	struct disconnect_request *rr =
		container_of(r, struct disconnect_request, r);

	FIN;
	list_del(&rr->c->link);
	remove_client(rr->c);
	FOUT;
	return 0;
}

static void on_disconnect(void *ctx, void *path)
{
	struct _client *c = ctx;
	struct disconnect_request *r;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_client_tester_disconnect;
		r->r.call = handle_disconnect;
		r->r.free = disconnect_request_free;
		r->c = c;
		wth_push_request(c->st->wth, &r->r);
	}
	FOUT;
}

static int handle_listen(void *p, struct request_base *r)
{
	struct server_tester *st = p;
	struct listen_request *rr = container_of(r, struct listen_request, r);
	struct local_address *la;
	char bs[64];
	bool found = false;

	FIN;
	list_for_each_entry(la, &st->local_addrs, link) {
		if (x_cmp_addr(&la->a, &rr->s)) {
			xdtrace("Found listening address %s is address_list\n",
				x_tss(&rr->s, bs, sizeof(bs)));
			found = true;
		}
	}
	if (!found)
		xdtrace("Listening on a non-registered address %s\n",
			x_tss(&rr->s, bs, sizeof(bs)));
	FOUT;
	return 0;
}

static int prepare_send_msg(
	void *ctx, void *buffer, int buf_size, int n_rdma, int *size)
{
	struct _client *c = ctx;
	struct ser_cli_tester_rep1_msg *m;
	unsigned rand;

	FIN;
	xdtrace("buf_size=%d\n", buf_size);
	get_random_bytes(&rand, sizeof(rand));
	m = buffer;
	m->hdr.type = cst_type_server_rep1;
	snprintf(m->buf, sizeof(m->buf), "SERVER SEND %u", rand);
	xttrace("Sending %s\n", m->buf);
	m->raddr = cpu_to_be64(c->me.mem->addr);
	m->rkey = cpu_to_be32(c->me.mem->rkey);
	*size = round_up(sizeof(*m), 8);
	FOUT;
	return 0;
}

static void send_comp(void *ctx, struct ib_wc *wc)
{
	struct _client *c = ctx;

	FIN;
	if (wc->status == IB_WC_SUCCESS)
		xttrace("Client %p sent a message to the server\n", c);
	else
		xdtrace("Client %p failed to send message to server: "
				"reason %s\n", c, ib_wc_status_msg(wc->status));
	rdma_dev_put_send_resource(c->me.info.rsc);
	FOUT;
}

static void send_msg(struct _client *c, union service_id *sid)
{
	struct dst_paths dst;

	FIN;
	if (!(c->me.buf = kzalloc(PAGE_SIZE, GFP_KERNEL))) {
		xetrace("Failed to allocate RDMA buffer for client\n");
		goto out;
	}
	if (!(c->me.mem = manager_reg_mem(
		c->st->o, c->me.local_rdma_device, c->me.buf, PAGE_SIZE))) {
		xetrace("Failed to register RDMA buffer for client\n");
		goto freebuf;
	}
	c->me.info.src = &c->me.sid;
	c->me.info.dst = sid;
	c->me.info.sg = NULL;
	c->me.info.nents = 0;
	dst.path = c->me.path;
	dst.next_paths = NULL;
	c->me.info.paths = &dst;
	c->me.info.opcode = __IB_WR_SEND;
	c->me.info.raddr = 0;
	c->me.info.rkey = 0;
	c->me.info.prepare_send = prepare_send_msg;
	c->me.info.send_comp = send_comp;
	c->me.info.ctx = c;
	manager_post_send(&c->me.info);
	goto out;

freebuf:
	kfree(c->me.buf);
	c->me.buf = NULL;

out:
	FOUT;
}

static int prepare_send_rdma(
	void *ctx, void *buffer, int buf_size, int n_rdma, int *size)
{
	struct cli_srv_tester_req2_msg *m;
	unsigned rand;

	FIN;
	get_random_bytes(&rand, sizeof(rand));
	m = buffer;
	m->hdr.type = cst_type_server_rep2;
	snprintf(m->buf, sizeof(m->buf), "SERVER RDMA %u", rand);
	xttrace("Sending %s\n", m->buf);
	*size = round_up(sizeof(*m), 8);
	FOUT;
	return *size;
}

static void send_rdma(
	struct _client *c, struct cli_srv_tester_req2_msg *m, union service_id *sid)
{
	struct dst_paths dst;
	int table_size;
	struct ib_sge *sg;

	FIN;
	table_size = be32_to_cpu(m->table_size);
	xttrace("Table size %d\n", table_size);
	sg = (void *)m + round_up(sizeof(*m), 8);
	xttrace("sg->addr=%#llx, sg->len=%d, sg->key=%#x\n",
		be64_to_cpu(sg->addr), be32_to_cpu(sg->length), be32_to_cpu(sg->lkey));
	*((unsigned *)c->me.buf) = 0xbeefdead;
	if (!(c->me.sg = kzalloc(sizeof(*c->me.sg), GFP_KERNEL))) {
		xetrace("Failed to allocate SG for client test\n");
		goto out;
	}
	sg_set_buf(c->me.sg, c->me.buf, PAGE_SIZE);
	sg_mark_end(c->me.sg);
	c->me.info.src = &c->me.sid;
	c->me.info.dst = sid;
	c->me.info.sg = c->me.sg;
	c->me.info.nents = 1;
	dst.path = c->me.path;
	dst.next_paths = NULL;
	c->me.info.paths = &dst;
	c->me.info.opcode = __IB_WR_RDMA_WRITE;
	c->me.info.raddr = be64_to_cpu(sg->addr);
	c->me.info.rkey = be32_to_cpu(sg->lkey);
	c->me.info.prepare_send = prepare_send_rdma;
	c->me.info.send_comp = send_comp;
	c->me.info.ctx = c;
	manager_post_send(&c->me.info);

out:
	FOUT;
}

static int handle_client_req(void *p, struct request_base *r)
{
	struct request_request *rr = container_of(r, struct request_request, r);
	struct _client *c = rr->c;

	FIN;
	switch (rr->op) {
	case cst_type_client_req1:
		send_msg(c, &rr->sid);
		break;
	case cst_type_client_req2:
		send_rdma(c, rr->msg, &rr->sid);
		rdma_dev_put_recv_resource(rr->rsc, rr->index);
		break;
	default:
		break;
	}
	FOUT;
	return 0;
}

static void on_recv_comp_t(void *ctx, struct ib_wc *wc, void *msg, void *rsc,
	int index, union service_id *sid)
{
	struct _client *c = ctx;
	struct request_request *r;
	struct cli_srv_tester_header *hdr = msg;
	struct cli_srv_tester_req1_msg *m1;
	struct cli_srv_tester_req2_msg *m2;
	bool ret_now = true;;

	FIN;
	if (wc->status == IB_WC_SUCCESS) {
		if ((r = kzalloc(sizeof(*r), GFP_ATOMIC))) {
			r->r.type = rpcrdma_client_tester_request;
			r->r.call = handle_client_req;
			r->r.free = request_request_free;
			r->c = c;
			r->sid = *sid;
			switch (hdr->type) {
			case cst_type_client_req1:
				m1 = msg;
				xttrace("message_type %d, message %s\n", hdr->type, m1->buf);
				r->op = cst_type_client_req1;
				break;
			case cst_type_client_req2:
				m2 = msg;
				manager_sync_mem_for_cpu(c->me.mem, DMA_FROM_DEVICE);
				xttrace("message_type %d, message %s\n", hdr->type, m2->buf);
				xttrace("Data in buffer %#x\n", *((unsigned *)c->me.buf));
				r->op = cst_type_client_req2;
				r->msg = msg;
				r->index = index;
				r->rsc = rsc;
				ret_now = false;
				break;
			default:
				break;
			}
			wth_push_request(c->st->wth, &r->r);
		}
		else
			xetrace("Failed to allocate message to handle request "
					"from client\n");		
	}
	else
		xetrace("Failed receive from client\n");
	if (ret_now)
		rdma_dev_put_recv_resource(rsc, index);
	FOUT;
}

static int handle_accept(void *p, struct request_base *r)
{
	struct server_tester *st = p;
	struct accept_request *rr = container_of(r, struct accept_request, r);
	struct _client *c;
	struct cli_srv_payload *me;
	struct cli_srv_payload *him;
	struct local_address *la;
	bool found_device;
	char bs[64];
	char bd[64];

	FIN;
	memset(rr->dpayload, 0, rr->dpayload_len);
	if (!(c = kzalloc(sizeof(*c), GFP_KERNEL))) {
		xetrace("Failed to allocate memory for new client on path %s -> %s\n",
			x_tss(rr->s, bs, sizeof(bs)), x_tss(rr->d, bd, sizeof(bd)));
		goto out;
	}
	c->st = st;
	c->me.ci.ft = cft;
	c->me.ci.client = c;
	c->me.ser.recv_comp = on_recv_comp_t;
	c->me.ser.ctx = c;
	c->me.src = *rr->s;
	c->me.dst = *rr->d;
	if (manager_register_service(st->o, &c->me.ser, &c->me.sid)) {
		xetrace("Failed to register client service on path %s -> %s\n",
			x_tss(rr->s, bs, sizeof(bs)), x_tss(rr->d, bd, sizeof(bd)));
		goto freec;
	}
	else
		xdtrace("accept sid %s\n", x_tsid(&c->me.sid, bs, sizeof(bs)));
	found_device = false;
	list_for_each_entry(la, &st->local_addrs, link) {
		if (x_cmp_addr(&la->a, rr->s)) {
			c->me.local_rdma_device = la->local_rdma_dev;
			found_device = true;
			break;
		}
	}
	if (!found_device)
		goto freec;
	c->me.path = rr->path;
	him = rr->spayload;
	c->him.payload = *him;
	me = (void *)c->me.ci.payload;
	me->type = him->type;
	me->sid = c->me.sid;
	*((struct cli_srv_payload *)rr->dpayload) = *me;
	*rr->ci = &c->me.ci;
	list_add_tail(&c->link, &st->clients);
	goto out;

freec:
	kfree(c);

out:
	complete(rr->c);
	FOUT;
	return 0;
}

static void on_listen(void *ctx, struct sockaddr_storage *s, int status)
{
	struct server_tester *st = ctx;
	struct listen_request *r;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_server_tester_listen;
		r->r.call = handle_listen;
		r->r.free = listen_request_free;
		r->s = *s;
		r->status = status;
		wth_push_request(st->wth, &r->r);
	}
	FOUT;
}

struct client_info * on_accept(void *ctx, void *path,
	struct sockaddr_storage *s,
	struct sockaddr_storage *d,
	void *spayload, int spayload_len,
	void *dpayload, int dpayload_len)
{
	struct server_tester *st = ctx;
	struct client_info *ci = NULL;
	struct accept_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_server_tester_accept;
		r->r.call = handle_accept;
		r->r.free = accept_request_free;
		r->path = path;
		r->s = s;
		r->d = d;
		r->spayload = spayload;
		r->spayload_len = spayload_len;
		r->dpayload = dpayload;
		r->dpayload_len = dpayload_len;
		r->ci = &ci;
		r->c = &comp;
		wth_push_request(st->wth, &r->r);
		wait_for_completion(&comp);
	}
	FOUT;
	return ci;
}

static int handle_new_local_address(void *p, struct request_base *r)
{
	struct server_tester *st = p;
	struct add_local_addr_request *rr =
		container_of(r, struct add_local_addr_request, r);
	struct local_address_info *a = &rr->a;
	struct local_address *la;
	char bs[64];

	FIN;
	xdtrace("New local address %s\n", x_tss(a->a, bs, sizeof(bs)));
	if ((la = kzalloc(sizeof(*la), GFP_KERNEL))) {
		la->a = *a->a;
		la->local_rdma_dev = a->local_rdma_dev;
		list_add_tail(&la->link, &st->local_addrs);
		manager_register_server(st->o, &st->si, la->local_rdma_dev, &la->a);
	}
	else
		xetrace("Failed to allocate local_address_info\n");
	FOUT;
	return 0;
}

static void on_new_local_address(void *ctx, struct local_address_info *a)
{
	struct server_tester *st = ctx;
	struct add_local_addr_request* r;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_server_tester_add_addr;
		r->r.call = handle_new_local_address;
		r->r.free = add_local_addr_request_free;
		r->a = *a;
		wth_push_request(st->wth, &r->r);
	}
	FOUT;
}

static void on_st_start(void *v)
{
	struct server_tester *st = v;

	FIN;
	complete(&st->start_stop_comp);
	if (manager_register_new_address(st->o, on_new_local_address, st))
		xetrace("Failed to register for new_local_address events\n");
	FOUT;
}

static void on_st_exit(void *v)
{
	struct server_tester *st = v;

	FIN;
	if (st->has_timer)
		del_timer_sync(&st->timer);
	FOUT;
}

static void remove_clients(struct server_tester *st)
{
	struct _client *c;

	FIN;
	while ((c = list_first_entry_or_null(
		&st->clients, struct _client, link))) {
		list_del(&c->link);
		remove_client(c);
	}
	FOUT;
}

void server_tester_free(struct server_tester *st)
{
	struct local_address *la;

	FIN;
	if (st->wth) {
		wth_free(st->wth);
		st->wth = NULL;
	}
	manager_unregister_new_address(st->o, st);
	while ((la = list_first_entry_or_null(
		&st->local_addrs, struct local_address, link))) {
		list_del(&la->link);
		manager_unregister_server(st->o, &st->si, la->local_rdma_dev, &la->a);
		kfree(la);
	}
	remove_clients(st);
	kfree(st);
	FOUT;
}

static int server_tester_timer(void *p, struct request_base * dr)
{
	return 0;
}
static TIMER_CALLBACK(
		on_timer, struct server_tester, timer, struct server_tester, st)

	struct request_base *r; 

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->type = rpcrdma_server_tester_timer;
		r->call = server_tester_timer;
		wth_push_request(st->wth, r);
	}
	else
		xetrace("failed to allocate server_tester_timer request\n");
	FOUT;
}
#if 0
static void on_timer(unsigned long data)
{
	struct server_tester *st = (void *)data;
	struct request_base *r; 

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->type = rpcrdma_server_tester_timer;
		r->call = server_tester_timer;
		wth_push_request(st->wth, r);
	}
	else
		xetrace("failed to allocate server_tester_timer request\n");
	FOUT;
}
#endif
struct server_tester * server_tester_create(struct manager *o)
{
	struct server_tester *st;
	struct wth_info info = {0};
	int rv;

	FIN;
	if (!(st = kzalloc(sizeof(*st), GFP_KERNEL))) {
		xetrace("Failed to allocate server tester\n");
		rv = -ENOMEM;
		goto out;
	}
	st->o = o;
	INIT_LIST_HEAD(&st->local_addrs);
	INIT_LIST_HEAD(&st->clients);
	init_completion(&st->start_stop_comp);
	INIT_TIMER(&st->timer);
	st->si.server = st;
	st->si.ft = sft;
	st->timer.function = on_timer;
	info.name = "server_tst";
	info.owner = st;
	info.on_start = on_st_start;
	info.on_exit = on_st_exit;
	if (!(st->wth = wth_create(&info))) {
		xetrace("Failed to create server_tester worker_thread\n");
		rv = -1;
		goto freeo;
	}
	wait_for_completion(&st->start_stop_comp);
	rv = 0;
	goto out;

freeo:
	server_tester_free(st);
	st = NULL;

out:
	FOUT;
	return rv ? NULL : st;
}
