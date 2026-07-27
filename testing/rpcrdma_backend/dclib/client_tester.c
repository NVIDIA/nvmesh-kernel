#include "xkr_incs.h"
#include "xkr_version.h"
#include "xib_incs.h"
#include "manager.h"
#include "rdma_dev.h"
#include "utils.h"
#include "wth.h"
#include "client_tester.h"
#include "cli_srv_test_common.h"
#include "xtrace.h"

struct _client;
struct client_tester {
	struct manager *o;
	struct sockaddr_storage dst;
	struct wth *wth;
	/* list of struct local_address */
	struct list_head local_addrs;
	struct list_head clients;
	struct completion start_stop_comp;
	TIMER_LIST_INSTANCE(timer);
	bool has_timer;
};

struct _client {
	struct client_tester *ct;
	struct client_info ci;
	void *path;
	struct local_address *la;
	struct server_service ser;
	union service_id sid;
	struct cli_srv_payload him;
	struct post_send_info info;
	void *buf;
	struct scatterlist *sg;
	struct list_head link;
};

static void on_connect(void *ctx, void *path, void *payload, int payload_len);
static void on_disconnect(void *ctx, void *path);

static struct client_ft cft = {
	.connect = on_connect,
	.disconnect = on_disconnect,
};

/**
 * Start client_tester requests
 */

struct connect_request {
	struct request_base r;
	struct _client *c;
	void *path;
	struct cli_srv_payload him;
};

static void connect_request_free(struct request_base *r)
{
	kfree(container_of(r, struct connect_request, r));
}

struct disconnect_request {
	struct request_base r;
	struct _client *c;
};

static void disconnect_request_free(struct request_base *r)
{
	kfree(container_of(r, struct disconnect_request, r));
}

struct reply_request {
	struct request_base r;
	struct _client *c;
	union service_id sid;
	int op;
	union {
		struct {
			__be64 raddr;
			__be32 rkey;
		} rep1;
	};
};

static void reply_request_free(struct request_base *r)
{
	kfree(container_of(r, struct reply_request, r));
}

/**
 * End manager requests
 */

static void remove_client(struct _client *c)
{
	FIN;
	manager_unregister_service(c->ct->o, &c->sid);
	manager_disconnect(c->ct->o, c->la->local_rdma_dev, c, c->path);
	kfree(c->buf);
	kfree(c->sg);
	kfree(c);
	FOUT;
}

static int prepare_send_msg(
	void *ctx, void *buffer, int buf_size, int n_rdma, int *size)
{
	struct cli_srv_tester_req1_msg *m;
	unsigned rand;

	FIN;
	get_random_bytes(&rand, sizeof(rand));
	m = buffer;
	m->hdr.type = cst_type_client_req1;
	snprintf(m->buf, sizeof(m->buf), "CLIENT SEND %u", rand);
	xttrace("Sending %s\n", m->buf);
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
	rdma_dev_put_send_resource(c->info.rsc);
	FOUT;
}

static void send_msg(struct _client *c)
{
	struct dst_paths dst;

	FIN;
	c->info.src = &c->sid;
	c->info.dst = &c->him.sid;
	c->info.sg = NULL;
	c->info.nents = 0;
	dst.path = c->path;
	dst.next_paths = NULL;
	c->info.paths = &dst;
	xdtrace("dst.path=%p\n", dst.path);
	c->info.opcode = __IB_WR_SEND;
	c->info.raddr = 0;
	c->info.rkey = 0;
	c->info.prepare_send = prepare_send_msg;
	c->info.send_comp = send_comp;
	c->info.ctx = c;
	manager_post_send(&c->info);
	FOUT;
}

static int handle_connect(void *p, struct request_base *r)
{
	struct connect_request *rr = container_of(r, struct connect_request, r);

	FIN;
	if (rr->path) {
		rr->c->path = rr->path;
		rr->c->him = rr->him;
		send_msg(rr->c);
	}
	else {
		list_del(&rr->c->link);
		remove_client(rr->c);
	}
	FOUT;
	return 0;
}

static void on_connect(void *ctx, void *path, void *payload, int payload_len)
{
	struct _client *c = ctx;
	struct connect_request *r;
	char bs[64];

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_client_tester_connect;
		r->r.call = handle_connect;
		r->r.free = connect_request_free;
		r->c = c;
		r->path = path;
		if (payload && payload_len) {
			r->him = *(struct cli_srv_payload *)payload;
			xdtrace("accept sid %s\n", x_tsid(&r->him.sid, bs, sizeof(bs)));
		}
		wth_push_request(c->ct->wth, &r->r);
	}
	FOUT;
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
		wth_push_request(c->ct->wth, &r->r);
	}
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
	m->hdr.type = cst_type_client_req2;
	snprintf(m->buf, sizeof(m->buf), "CLIENT RDMA %u", rand);
	xttrace("Sending %s\n", m->buf);
	m->table_size = cpu_to_be32(n_rdma);
	*size = round_up(sizeof(*m), 8);
	FOUT;
	return *size;
}

static void send_rdma(
	struct _client *c, union service_id *sid, u64 raddr, u32 rkey)
{
	struct dst_paths dst;

	FIN;
	if (!(c->buf = kzalloc(PAGE_SIZE, GFP_KERNEL))) {
		xetrace("Failed to allocate page for client test\n");
		goto out;
	}
	*((unsigned *)c->buf) = 0xdeadbeef;
	if (!(c->sg = kzalloc(sizeof(*c->sg), GFP_KERNEL))) {
		xetrace("Failed to allocate SG for client test\n");
		goto freebuf;
	}
	sg_set_buf(c->sg, c->buf, PAGE_SIZE);
	sg_mark_end(c->sg);
	c->info.src = &c->sid;
	c->info.dst = sid;
	c->info.sg = c->sg;
	c->info.nents = 1;
	dst.path = c->path;
	dst.next_paths = NULL;
	xdtrace("dst.path=%p\n", dst.path);
	c->info.paths = &dst;
	c->info.opcode = __IB_WR_RDMA_WRITE | __IB_WR_RDMA_READ;
	c->info.raddr = raddr;
	c->info.rkey = rkey;
	c->info.prepare_send = prepare_send_rdma;
	c->info.send_comp = NULL;//send_comp;
	c->info.ctx = c;
	manager_post_send(&c->info);
	goto out;

freebuf:
	kfree(c->buf);
	c->buf = NULL;

out:
	FOUT;
}

static int handle_server_rep(void *p, struct request_base *r)
{
	struct reply_request *rr = container_of(r, struct reply_request, r);
	struct _client *c = rr->c;

	FIN;
	xdtrace("rr->op=%d\n", rr->op);
	switch (rr->op) {
	case cst_type_server_rep1:
		send_rdma(c, &rr->sid,
			be64_to_cpu(rr->rep1.raddr), be32_to_cpu(rr->rep1.rkey));
		break;
	case cst_type_server_rep2:
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
	struct reply_request *r;
	struct cli_srv_tester_header *hdr = msg;
	struct ser_cli_tester_rep1_msg *m1;
	struct ser_cli_tester_rep2_msg *m2;

	FIN;
	if (wc->status == IB_WC_SUCCESS) {
		if ((r = kzalloc(sizeof(*r), GFP_ATOMIC))) {
			r->r.type = rpcrdma_server_tester_reply;
			r->r.call = handle_server_rep;
			r->r.free = reply_request_free;
			r->c = c;
			r->sid = *sid;
			r->op = hdr->type;
			switch (hdr->type) {
			case cst_type_server_rep1:
				m1 = msg;
				xttrace("Reply 1 from server %s\n", m1->buf);
				r->rep1.raddr = m1->raddr;
				r->rep1.rkey = m1->rkey;
				break;
			case cst_type_server_rep2:
				m2 = msg;
				rdma_dev_put_send_resource(c->info.rsc);
				c->info.rsc = NULL;
				xttrace("Reply 2 from server %s\n", m2->buf);
				xttrace("Data in buffer %#x\n", *((unsigned *)c->buf));
				break;
			default:
				break;
			}
			wth_push_request(c->ct->wth, &r->r);
		}
		else
			xetrace("Failed to allocate message to handle reply from server\n");		
	}
	else
		xetrace("Failed receive from server\n");
	rdma_dev_put_recv_resource(rsc, index);
	FOUT;
}

static void create_new_client(
	struct client_tester *ct, struct local_address *la)
{
	struct _client *c = NULL;
	struct cli_srv_payload *me;
	char bs[64];
	char bd[64];

	FIN;
	if (la->a.ss_family == ct->dst.ss_family) {
		if ((c = kzalloc(sizeof(*c), GFP_KERNEL))) {
			c->ct = ct;
			c->la = la;
			c->ci.client = c;
			c->ci.ft = cft;
			c->ser.recv_comp = on_recv_comp_t;
			c->ser.ctx = c;
			if (manager_register_service(ct->o, &c->ser, &c->sid)) {
				xetrace("Failed to register client service on path %s -> %s\n",
					x_tss(&la->a, bs, sizeof(bs)),
					x_tss(&ct->dst, bd, sizeof(bd)));
				kfree(c);
				goto out;
			}
			me = (void *)c->ci.payload;
			me->type = cst_type_dummy;
			me->sid = c->sid;
			if (manager_connect(
				ct->o, &c->ci, la->local_rdma_dev, &la->a, &ct->dst)) {
				manager_unregister_service(ct->o, &c->sid);
				kfree(c);
				goto out;
			}
			list_add_tail(&c->link, &ct->clients);
		}
	}

out:
	FOUT;
}

static int handle_new_local_address(void *p, struct request_base *r)
{
	struct client_tester *ct = p;
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
		list_add_tail(&la->link, &ct->local_addrs);
		create_new_client(ct, la);
	}
	else
		xetrace("Failed to allocate local_address_info\n");
	FOUT;
	return 0;
}

static void on_new_local_address(void *ctx, struct local_address_info *a)
{
	struct client_tester *ct = ctx;
	struct add_local_addr_request* r;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_client_tester_add_addr;
		r->r.call = handle_new_local_address;
		r->r.free = add_local_addr_request_free;
		r->a = *a;
		wth_push_request(ct->wth, &r->r);
	}
	FOUT;
}

static void on_ct_start(void *v)
{
	struct client_tester *ct = v;

	FIN;
	complete(&ct->start_stop_comp);
	if (manager_register_new_address(ct->o, on_new_local_address, ct))
		xetrace("Failed to register for new_local_address events\n");
	FOUT;
}

static void on_ct_exit(void *v)
{
	struct client_tester *ct = v;

	FIN;
	if (ct->has_timer)
		del_timer_sync(&ct->timer);
	FOUT;
}

static void remove_clients(struct client_tester *ct)
{
	struct _client *c;

	FIN;
	while ((c = list_first_entry_or_null(
		&ct->clients, struct _client, link))) {
		list_del(&c->link);
		remove_client(c);
	}
	FOUT;
}

void client_tester_free(struct client_tester *ct)
{
	struct local_address *la;

	FIN;
	if (ct->wth) {
		wth_free(ct->wth);
		ct->wth = NULL;
	}
	manager_unregister_new_address(ct->o, ct);
	remove_clients(ct);
	while ((la = list_first_entry_or_null(
		&ct->local_addrs, struct local_address, link))) {
		list_del(&la->link);
		kfree(la);
	}
	kfree(ct);
	FOUT;
}

static int client_tester_timer(void *p, struct request_base * dr)
{
	return 0;
}

static TIMER_CALLBACK(
		on_timer, struct client_tester, timer, struct client_tester, ct)

	struct request_base *r; 

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->type = rpcrdma_client_tester_timer;
		r->call = client_tester_timer;
		wth_push_request(ct->wth, r);
	}
	else
		xetrace("failed to allocate client_tester_timer request\n");
	FOUT;
}
#if 0
static void on_timer(unsigned long data)
{
	struct client_tester *ct = (void *)data;
	struct request_base *r; 

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->type = rpcrdma_client_tester_timer;
		r->call = client_tester_timer;
		wth_push_request(ct->wth, r);
	}
	else
		xetrace("failed to allocate client_tester_timer request\n");
	FOUT;
}
#endif
struct client_tester * client_tester_create(struct manager *o,
	struct sockaddr_storage *dst)
{
	struct client_tester *ct;
	struct wth_info info = {0};
	int rv;

	FIN;
	if (!(ct = kzalloc(sizeof(*ct), GFP_KERNEL))) {
		xetrace("Failed to allocate client tester\n");
		rv = -ENOMEM;
		goto out;
	}
	ct->o = o;
	INIT_LIST_HEAD(&ct->local_addrs);
	INIT_LIST_HEAD(&ct->clients);
	init_completion(&ct->start_stop_comp);
	//INIT_TIMER(&ct->timer);
	SETUP_TIMER(&ct->timer, on_timer, (unsigned long)ct, 0);
	TIMER_SET_DATA(ct, timer, (unsigned long)ct);
	ct->timer.function = on_timer;
	ct->dst = *dst;
	info.name = "client_tst";
	info.owner = ct;
	info.on_start = on_ct_start;
	info.on_exit = on_ct_exit;
	if (!(ct->wth = wth_create(&info))) {
		xetrace("Failed to create client_tester worker_thread\n");
		rv = -1;
		goto freeo;
	}
	wait_for_completion(&ct->start_stop_comp);
	rv = 0;
	goto out;

freeo:
	client_tester_free(ct);
	ct = NULL;

out:
	FOUT;
	return rv ? NULL : ct;
}
