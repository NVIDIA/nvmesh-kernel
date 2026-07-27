#include "nvmeibt_nm_hw_iface.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_net.h"
#include "nvmeibt_ds.h"

#define NVMEIBT_UDP_SERVER_PORT 4100

struct udp_per_nic {
	struct nvmeibt_nm_per_nic base;
	struct nvmeibt_netdev *ndev;
};

struct udp_per_port {
	struct nvmeibt_nm_per_port base;
	int sockfd;
	bool wait_write;
	struct sockaddr_storage bind_sin;
	void *buf;
};

struct udp_msg_item;
typedef void (*udp_msg_free_func_t)(struct udp_msg_item *item);

struct udp_msg_item {
	struct udp_msg *msg;
	ssize_t msg_len;
	struct xdlist link;
	udp_msg_free_func_t free_func;
};

struct udp_path {
	struct nvmeibt_nm_path base;
	XDLIST_DECLARE(, struct udp_msg_item, link) pending;
};

#define path2udp_path(_path) ((struct udp_path *)_path)
#define port2udp_port(_port) container_of(_port, struct udp_per_port, base)

enum udp_msg_type {
	udp_msg_type_login_req = 0,
	udp_msg_type_login_resp = 1,
	udp_msg_type_ping_req = 2,
	udp_msg_type_srm = 3
};

struct udp_msg {
	uint32_t msg_type;
};

struct udp_login_data {
	struct nvmeibt_nm_login_data base;
	uint64_t session_id;
};

struct udp_login_msg {
	struct udp_msg base;
	struct udp_login_data data;
};

struct udp_login_resp {
	struct udp_msg base;
	uint8_t accepted : 1;
	uint32_t reject_reason;
	struct udp_login_data data;
};

struct udp_ping_request {
	struct udp_msg base;
	union nvmeibt_nm_ping_imm_data v;
};

struct udp_local_node {
	struct nvmeibt_nm_local_node base;
	int netlink_fd;
};

struct nvmeibt_nm_hw_function_table udp_func_table = {
	.allocate_local_node = nvmeibt_nm_hw_allocate_local_node,
	.free_local_node = nvmeibt_nm_hw_free_local_node,
	.init_local_node = nvmeibt_nm_hw_init_local_node,
	.hw_init = nvmeibt_nm_hw_init,
	.init_nics = nvmeibt_nm_hw_init_nics,
	.attach_nic = nvmeibt_nm_hw_attach_nic,
	.allocate_path = nvmeibt_nm_hw_allocate_path,
	.restart_path = nvmeibt_nm_hw_restart_path,
	.set_conneting_path = nvmeibt_nm_hw_set_conneting_path,
	.free_path = nvmeibt_nm_hw_free_path,
	.resolve_path = nvmeibt_nm_hw_resolve_path,
	.allocate_remote_addr = nvmeibt_nm_hw_allocate_remote_addr,
	.get_max_chunk_size = nvmeibt_nm_hw_get_max_chunk_size,
	.alloc_msgs_buffer = nvmeibt_nm_hw_alloc_msgs_buffer,
	.path_release_send_buffer = nvmeibt_nm_hw_path_release_send_buffer,
	.path_send_msg = nvmeibt_nm_hw_path_send_msg,
	.srm_get_data_offset_size = nvmeibt_nm_hw_srm_get_data_offset_size,
	.try_connect_path = nvmeibt_nm_hw_try_connect_path,
	.send_ping = nvmeibt_nm_hw_send_ping,
	.reject_connection = nvmeibt_nm_hw_reject_connection,
	.accept_connection = nvmeibt_nm_hw_accept_connection,
	.allocate_login_data = nvmeibt_nm_hw_allocate_login_data,
	.get_pp_state = nvmeibt_nm_hw_get_pp_state,
	.create_port = nvmeibt_nm_hw_create_port,
	.free_port = nvmeibt_nm_hw_free_port, 
	.free_nic = nvmeibt_nm_hw_free_nic,
	.should_revive_port = nvmeibt_nm_hw_should_revive_port,
	.wait_events = nvmeibt_nm_hw_wait_events,
	.get_port = nvmeibt_nm_hw_get_port
};

void nvmeibt_nm_hw_fill_hw_function_table(struct nvmeibt_nm_hw_function_table *in) {
	*in = udp_func_table;
};

struct nvmeibt_nm_local_node * nvmeibt_nm_hw_allocate_local_node(void) {
	return NNVMEIBT_TOMA_CALLOC(udp_nm_hw_allocate_local_node, 1, sizeof(struct udp_local_node));
}

int nvmeibt_nm_hw_init_local_node(struct nvmeibt_nm_local_node *ln) {
	(void) ln;
	return 0;
}

int handle_nl(void *ctx, int is_read, int is_write, int dry_times)
{
	struct udp_local_node *ln = ctx;
	(void) is_read;
	(void) is_write;
	(void) dry_times;

	NFIN;
	while(!handle_netlink_event(ln->netlink_fd));
	ln->base.renew_status = 1;
	NFOUT;

	return 0;
}

int nvmeibt_nm_hw_init(struct nvmeibt_nm_local_node *ln) {
	struct nvmeibt_netdev *netdev;
	struct nvmeibt_nm_per_fd pfd;
	int rv = 0;

	((struct udp_local_node *)ln)->netlink_fd = scan_server_netdev();
	
	ln->n_nics = 0;
	XDLIST_FOREACH(netdev, nvmeibt_net_get_netdevs()) {
		N_Tf(udp_fdfs, "found new netdev @STR, adding to nic list", netdev->name);
		ln->n_nics++;
	}

	if ((rv = nvmeibt_nonblock_fd(((struct udp_local_node *)ln)->netlink_fd)) < 0) {
		N_ETf(udp_init_nl, "Fail to make async_event_fd non-blocking");
		rv = -1;
	}

	pfd.ctx = ln;
	pfd.fd = ((struct udp_local_node *)ln)->netlink_fd;
	pfd.f = handle_nl;
	nvmeibt_nm_add_fd(ln, &pfd);

	return 0;
}

int nvmeibt_nm_hw_init_nics(struct nvmeibt_nm_local_node *ln) {
	int rv = 0;
	int i = 0;
	struct udp_per_nic *nics = NULL;
	struct nvmeibt_netdev *netdev;

	if (!(ln->nics = NNVMEIBT_TOMA_CALLOC(
		udp_init_nics_calloc_err, ln->n_nics, sizeof(struct udp_per_nic *)))) {
		N_ETf(udp_init_rdma_e4, "Failed to allocate NICs");
		rv = -1;
		goto out;
	}

	if (!(nics = NNVMEIBT_TOMA_CALLOC(
		udp_init_nics_p_calloc_err, ln->n_nics, sizeof(struct udp_per_nic)))) {
		N_ETf(udp_init_rdma_e63, "Failed to allocate NICs");
		rv = -1;
		goto free_nics;
	}
	
	XDLIST_FOREACH(netdev, nvmeibt_net_get_netdevs()) {
		ln->nics[i] = (struct nvmeibt_nm_per_nic *)(&nics[i]);
		strlcpy(ln->nics[i]->dev_name, netdev->name, sizeof(ln->nics[i]->dev_name));
		((struct udp_per_nic *)(ln->nics[i]))->ndev = netdev;
		ln->nics[i]->local_node = ln;
		i++;
	}
	goto out;

free_nics:
	NNVMEIBT_TOMA_FREE(udp_hw_free_nics, ln->nics);

out:
	return rv;
}

int nvmeibt_nm_hw_attach_nic(struct nvmeibt_nm_local_node *ln, int idx) {
	struct nvmeibt_nm_per_port *pp;
	int rv = 0;
	
	NFIN;
	/* In UDP we map each nic <---> port */
	ln->nics[idx]->n_ports = 1;
	if (!(ln->nics[idx]->ports = NNVMEIBT_TOMA_CALLOC(udp_alloc_p, 1, sizeof(struct udp_per_port *)))) {
		N_ETf(udp_init_rdma_e6, "Failed to allocate NICs array");
		rv = -1;
		goto out;
	}

	if (!(pp = NNVMEIBT_TOMA_CALLOC(
		udp_alloc_per_port, 1, sizeof(struct udp_per_port)))) {
		N_ETf(udp_init_rdma_e62, "Failed to allocate port");
		rv = -1;
		goto free_nics_arr;
	}

	ln->nics[idx]->ports[0] = pp;
	pp->pn = ln->nics[idx];
	pp->port_num = 1;
	/* TODO:NM what will happen with interface vlans? */
	pp->broadcast_id = 0xffff;
	pp->pn->allowed = 1;

	rv = 0;
	goto out;

	NFOUT;

free_nics_arr:
	NNVMEIBT_TOMA_FREE(udp_free_ports, ln->nics[idx]->ports);
	
out:
	return rv;
}

struct nvmeibt_nm_req * nvmeibt_nm_hw_get_req(void) {
	return NNVMEIBT_TOMA_CALLOC(nm_udp_get_req, 1, sizeof(struct nvmeibt_nm_req));
}
void nvmeibt_nm_hw_clear_req(struct nvmeibt_nm_req *req) {
	memset(req, 0, sizeof(struct nvmeibt_nm_req));
}
void nvmeibt_nm_hw_set_remote_nic_event(struct nvmeibt_nm_req *req, struct nvmeibt_nic *nic) {
	(void) req;
	(void) nic;
	return;
}

struct nvmeibt_nm_path * nvmeibt_nm_hw_allocate_path(void) {
	struct udp_path *udp_path = NNVMEIBT_TOMA_CALLOC(udp_nm_hw_allocate_path, 1, sizeof(struct udp_path));
	if (udp_path) {
		XDLIST_HEAD_INIT(&udp_path->pending);
	}
	return (struct nvmeibt_nm_path *)udp_path;
}

static int handle_udp_msg(void *ctx, int is_read, int is_write, int dry_tries);
static void mod_pp_fd(struct udp_per_port *pp, bool wait_write) {
	struct nvmeibt_nm_local_node *ln = pp->base.pn->local_node;
	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_udp_msg,
		.ctx = pp,
		.wait_read = 1,
		.wait_write = wait_write,
		.fd = pp->sockfd,
	};
	
	if (pp->wait_write != wait_write) {
		N_Tf(udp_mod_pp_fd, "modifying pp fd @FD to wait_write=@BOOL", pp->sockfd, wait_write);
		pp->wait_write = wait_write;
		nvmeibt_nm_mod_fd(ln, &pfd);
	}
	
}

static void udp_path_clear_pending(struct udp_path *path) {
	struct udp_msg_item *item;
	while (!XDLIST_EMPTY(&path->pending)) {
		item = XDLIST_FIRST(&path->pending);
		XDLIST_DEL(&item->link);
		if (item->free_func) {
			item->free_func(item);	
		}
		nvmeibt_bm_free_buffer(item);
	}
}


void nvmeibt_nm_hw_restart_path(struct nvmeibt_nm_path *path) {
	udp_path_clear_pending(path2udp_path(path));
	return;
}

void nvmeibt_nm_hw_set_conneting_path(struct nvmeibt_nm_path *path) {
	(void) path;
	return;
}

void nvmeibt_nm_hw_free_path(struct nvmeibt_nm_path *path) {
	udp_path_clear_pending(path2udp_path(path));
	return;
}

/* Helper functions for managing the pending message list */
static int udp_path_add_msg(struct udp_path *path, struct udp_msg *msg, ssize_t msg_len, udp_msg_free_func_t free_func) {
	struct udp_msg_item *item;
	int rv;

	NFIN;
	item = nvmeibt_bm_allocate_buffer(sizeof(*item));
	if (!item) {
		N_Ef(udp_path_add_msg_e1, "Failed to allocate item");
		rv = -ENOMEM;
		goto out;
	}

	item->msg = msg;
	item->msg_len = msg_len;
	item->free_func = free_func;
	XDLIST_ADD_TAIL(&path->pending, item);
	rv = 0;
	mod_pp_fd(port2udp_port(path->base.pp), true);

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_hw_resolve_path(struct nvmeibt_nm_path *path) {
	PFIN;
	nvmeibt_nm_path_resolve_wait(path);
	PFOUT;
	return nvmeibt_nm_try_connect_path(path);
}

struct nvmeibt_nm_remote_addr * nvmeibt_nm_hw_allocate_remote_addr(void) {
	return NNVMEIBT_TOMA_CALLOC(udp_nm_hw_allocate_remote_addr, 1, sizeof(struct nvmeibt_nm_remote_addr));
}

/*
	In the next function we have to calculate max_chunk_size for srm module.
	We know the phisical mtu, but we don't know L2(mac) and L3(IP) headers len
	because they are not standart. L2 len can be 14(Ethernet II) or 22(802.3)
	bytes len or even more when VLANs or MPLS headers are used.
	L3 can be IPv4(with or without options) or IPv6. If we give to srm a wrong mtu it
	can cause to further IP fragmentation and UDP port filter network mechanism will drop
	all the fragments besides the first one. Thus, we define the max mutual header len
	that is for sure enough to involve the longest possible option.
*/
extern int64_t udp_max_header_length;
int nvmeibt_nm_hw_get_max_chunk_size(struct nvmeibt_nm_path *path) {
	int64_t			mtu = path->pp->mtu;

	if (mtu < (int64_t)(sizeof(struct udp_msg)) + udp_max_header_length + 64) {
		N_Ef(jssse72, "mtu=@LLD is too short", mtu);
		nvmeibt_abort(ES_FATAL);
	}
	return (mtu - sizeof(struct udp_msg) - udp_max_header_length);
}

int nvmeibt_nm_hw_srm_get_data_offset_size(void) {
	return sizeof(struct udp_msg);
}

void * nvmeibt_nm_hw_alloc_msgs_buffer(struct nvmeibt_nm_path *path) {
	path->send_buf = NNVMEIBT_TOMA_CALLOC(trace_udp_alloc_msgs_buffer, path->max_messages, path->pp->mtu);
	path->send_buf_len = path->max_messages * path->pp->mtu;

	return path->send_buf;
}
void nvmeibt_nm_hw_path_release_send_buffer(struct nvmeibt_nm_path *path) {
	if (path->send_buf) {
		NNVMEIBT_TOMA_FREE(udp_release_send_buffer, path->send_buf);
		path->send_buf = NULL;
	}
}

int send_udp_msg_to_path(void *msg, ssize_t msg_len, struct nvmeibt_nm_path *path) {
	int rv = -1;
	ssize_t sent;

	sent = sendto(((struct udp_per_port *)(path->pp))->sockfd, msg, msg_len, 0,
				(const struct sockaddr *)(&path->ra->a), sizeof(path->ra->a));
	if (sent != (ssize_t)msg_len) {
		if (sent == -1) {
			rv = -errno;
		}
		N_ETf(udp_send_bad, "sendto failed: rv is @DATA_LLEN and errno: @AUTO_ERRNO", (unsigned long int)sent);
		goto out;
	}

	rv = 0;

out:
	return rv;
}

/* Connect a path using speicfic hw */
int nvmeibt_nm_hw_try_connect_path(struct nvmeibt_nm_path *path, bool first) {
	int rv = 0;
	struct udp_login_msg msg = {.base.msg_type = udp_msg_type_login_req};
	(void) first;

	msg.data.base = *path->payload;
	msg.data.session_id = htobe64(path->cmt.base.guid);
	if (send_udp_msg_to_path(&msg, sizeof(msg), path)) {
		N_ETf(udp_try_connect, "sendto returned @INT", rv);
	}
	return rv;
}

static void free_udp_msg_item_msg(struct udp_msg_item *item) {
	nvmeibt_bm_free_buffer(item->msg);
}

/* Keep a live path */
int nvmeibt_nm_hw_send_ping(struct nvmeibt_nm_path *path, int is_response, uint8_t ping_id, int retry_count) {
	struct udp_ping_request msg, *retry_msg;
	union nvmeibt_nm_ping_imm_data *v = &msg.v;
	int rv;

	msg.base.msg_type = htobe32(udp_msg_type_ping_req);
	v->raw = 0;
	v->fields.srm_id_lsb = (uint16_t)path->remote_srm_id;
	v->fields.ping_id = ping_id;
	v->fields.retry_count = retry_count;
	v->fields.resp = is_response ? 1 : 0;
	v->raw = htobe32(v->raw);

	if ((rv = send_udp_msg_to_path(&msg, sizeof(msg), path))) {
		if (rv == -EWOULDBLOCK) {
			retry_msg = nvmeibt_bm_allocate_buffer(sizeof(*retry_msg));
			if (retry_msg) {
				*retry_msg = msg;
				rv = udp_path_add_msg(path2udp_path(path), &retry_msg->base, sizeof(msg), free_udp_msg_item_msg);
			}
		}
		N_ETf(udp_send_ping_conn, "sendto returned - @INT", rv);
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_ping);
	}

	return rv;
}

int nvmeibt_nm_hw_path_send_msg(struct nvmeibt_nm_path *path, struct nvmeibt_wire_msg *wire, uint16_t msg_id, bool signal) {
	int rv;
	struct udp_msg *msg = wire->buffer;
	(void) signal;
	(void) msg_id;

	msg->msg_type = htobe32(udp_msg_type_srm);
	wire->len += sizeof(struct udp_msg);

	if ((rv = send_udp_msg_to_path(wire->buffer, wire->len, path))) {
		N_ETf(udp_send_ping_srm, "sendto returned - @INT", rv);
		if (rv == -EWOULDBLOCK) {
			if ((rv = udp_path_add_msg(path2udp_path(path), msg, wire->len, NULL /* wired msg will be freed by srm */))) {
				N_ETf(udp_send_ping_srm_e1, "Failed to add message to pending list rv = @INT", rv);
			}
		}
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_send_comp);
	}

	return rv;
}

struct udp_on_connection_data {
	struct sockaddr_storage *addr;
	struct udp_per_port *pp;
};

/* On new connection hw specific implenetion */
int nvmeibt_nm_hw_reject_connection(void *data, uint32_t reject_reason, struct nvmeibt_nm_login_data *login_data) {
	int rv = 0;
	ssize_t sent;
	struct udp_on_connection_data *conn = data;
	struct udp_login_resp msg = {.accepted = 0,
								 .data.session_id = ((struct udp_login_data *)(login_data))->session_id,
								 .base.msg_type = htobe32(udp_msg_type_login_resp),
								 .reject_reason = reject_reason,
								 };
	
	NFIN;
	sent = sendto(conn->pp->sockfd, &msg, sizeof(msg), 0,
		(const struct sockaddr *)(conn->addr), sizeof(*conn->addr));
	if (sent != sizeof(msg)) {
		N_ETf(udp_send_reject_f, "sendto failed: rv is @DATA_LLEN and errno: @AUTO_ERRNO", sent);
	}

	NFOUT;

	return rv;
}

int nvmeibt_nm_hw_accept_connection(struct nvmeibt_nm_path *path, struct nvmeibt_nm_per_port *port,
									struct nvmeibt_nm_login_data *login_data, void *data) {
	int rv = 0;
	struct udp_login_resp msg = {.accepted = 1, .data.base = *path->payload,
								 .data.session_id = ((struct udp_login_data *)(login_data))->session_id,
								 .base.msg_type = htobe32(udp_msg_type_login_resp)};
	(void) port;
	(void) data;
	
	NFIN;
	if ((rv = send_udp_msg_to_path(&msg, sizeof(msg), path))) {
		N_ETf(udp_accept_conn, "sendto returned - @INT", rv);
	}
	NFOUT;

	return rv;									
}

struct nvmeibt_nm_login_data * nvmeibt_nm_hw_allocate_login_data(void) {
	return NNVMEIBT_TOMA_CALLOC(udp_nm_hw_allocate_login_data, 1, sizeof(struct udp_login_data));
}

static const char *if_operstate_name[] = {
	"IF_OPER_UNKNOWN",
	"IF_OPER_NOTPRESENT",
	"IF_OPER_DOWN",
	"IF_OPER_LOWERLAYERDOWN",
	"IF_OPER_TESTING",
	"IF_OPER_DORMANT",
	"IF_OPER_UP",
};

const char * nvmeibt_nm_hw_get_pp_state(struct nvmeibt_nm_per_port *pp) {
	return if_operstate_name[((struct udp_per_nic *)pp->pn)->ndev->prop.oper_state];
}

static struct local_nic_port_data * get_port_info(struct nvmeibt_nm_per_port *pp)
{
	struct local_nic_data *nic;
	struct local_nic_port_data *d = NULL;
	int i;
	struct local_nics_data *lnd = &pp->pn->local_node->lnd;

	NFIN;
	for (i = 0; i < lnd->n_nics; i++) {
		nic = &lnd->nics[i];
		if ((strncmp(pp->pn->dev_name, nic->ibv_devname, NVMEIB_IB_DEVICE_NAME_MAX) == 0 || strncmp(pp->pn->dev_name, nic->ports[1].ndev_name,
		NVMEIB_IB_DEVICE_NAME_MAX) == 0) && nic->ports[pp->port_num].valid) {
			d = &nic->ports[pp->port_num];
			break;
		}
	}
	NFOUT;
	return d;
}

static struct nvmeibt_nm_hash_cm_connect_key_type * __attribute__ ((unused)) kt2ccl(
	struct nvmeibt_nm_hash_key_type *kt)
{
	return container_of(kt, struct nvmeibt_nm_hash_cm_connect_key_type, base);
}

static inline struct nvmeibt_nm_path *cmt2p(struct nvmeibt_nm_hash_cm_connect_key_type *cmt)
{
	return container_of(cmt, struct nvmeibt_nm_path, cmt);
}

static int connection_established(struct udp_per_port *pp, struct udp_login_data *login) {
	struct nvmeibt_nm_path *path;
	struct nvmeibt_nm_hash_key_type *kt;
	uint64_t id = be64toh(login->session_id);
	struct sockaddr_storage a;
	char b[TOMA_SOCKADDR_STRING_LEN];

	NFIN;

	/* TODO:NM most of it can be extracted */
	if (!(kt = find_key(pp->base.pn->local_node, id))) {
		N_ETf(udp_hee_e3, "No such client cm_id @CMT", (long long unsigned int)id);
		goto out;
	}
	
	if (kt->base.type == kt_connect_cm) {
		path = cmt2p(kt2ccl(kt));
		nvmeibt_ib_common_rdma_gid2ip(&a, &login->base.sgid, NVMEIBT_UDP_SERVER_PORT,
			path->pp->transport == rtr_ib, path->pp->broadcast_id);
		path->remote_srm_id = be32toh(login->base.srm_id);

		N_Df(udp_hee_d11, "Received connect response from host @UUID_LE at addr @STR, "
			"srm_id @INT",
			&login->base.node_id, nvmeibt_nm_tss(&a, b, sizeof(b)),
			be32toh(login->base.srm_id));
		N_Tf(udp_hee_t3001, "Path @STR(srm @UINT, remote_srm @UINT) - connected",
            path->name, path->srm_id, path->remote_srm_id);

		if (nvmeibt_nm_connect_path(path)) {
			N_Ef(udp_hee_d2135, "path @STR failed to connect", path->name);
			nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_conenct);
			goto out;
		}

	}

out:
	NFOUT;
	return 0;
}

static int connection_rejected(struct udp_per_port *pp, struct udp_login_data *login) {
	struct nvmeibt_nm_path *path;
	struct nvmeibt_nm_hash_key_type *kt;
	uint64_t id = be64toh(login->session_id);
	struct sockaddr_storage a;
	char b[TOMA_SOCKADDR_STRING_LEN];

	NFIN;

	/* TODO:NM most of it can be extracted */
	if (!(kt = find_key(pp->base.pn->local_node, id))) {
		N_ETf(udp_hee_e2, "No such client cm_id @CMT", (long long unsigned int)id);
		goto out;
	}
	
	if (kt->base.type == kt_connect_cm) {
		path = cmt2p(kt2ccl(kt));
		nvmeibt_ib_common_rdma_gid2ip(&a, &login->base.sgid, NVMEIBT_UDP_SERVER_PORT,
			path->pp->transport == rtr_ib, path->pp->broadcast_id);
		path->remote_srm_id = be32toh(login->base.srm_id);

		N_Tf(udp_reject_tt, "Received reject response from host @UUID_LE at addr @STR, "
			"srm_id @INT",
			&login->base.node_id, nvmeibt_nm_tss(&a, b, sizeof(b)),
			be32toh(login->base.srm_id));

		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_conenct_reject);
		nvmeibt_nm_restart_path(path);
	}

out:
	NFOUT;
	return 0;
}

struct nvmeibt_nm_path * get_path_by_ra_port(struct nvmeibt_nm_remote_addr *ra, struct nvmeibt_nm_per_port *pp) {
	bool found;
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_path *path;


	found = false;
	XDLIST_FOREACH(l, &ra->ready) {
		path = nvmeibt_nm_l2p(l);
		if (path->pp == pp) {
			N_Df(udp_find_path_d2000, "Found ready path @STR", path->name);
			found = true;
			break;
		}
	}
	if (found)
		goto on_found;
	/* if there is no ready path to handle the message try to find a
	 * path that is still connecting but is the one that accepted the
	 * connection from the peer.  there should be one because a message is sent
	 * only after a connection request was accepted.
	*/
	XDLIST_FOREACH(l, &ra->connecting) {
		path = nvmeibt_nm_l2p(l);
		if (path->pp == pp) {
			N_Df(udp_find_path_d2001, "Found connecting path @STR", path->name);
			found = true;
			break;
		}
	}
	if (found) {
		/* the path has to be in RTR which means that it is the one that
		 * accepted the connection from the peer.
		 */
		if (!path->is_rtr) {
			N_Tf(udp_find_path_73333, "Path @STR is NOT RTR", path->name);
			found = false;
		}
	}
	else
		path = NULL;
	if (!found) {
		if (!path) {
			//trace_remote_address_paths(ra);
			N_ETf(usp_find_path_e4, "Failed to find path to handle the request");
		}
		goto err;
	}

on_found:
	path->renew_srm_id = 1;
	goto out;

err:
	path = NULL;

out:
	//NFOUT_;
	return path;
}


static int handle_pending_messages(struct udp_per_port *pp) {
	struct udp_msg_item *item;
	int rv = 0;
	struct nvmeibt_nm_local_node *ln = pp->base.pn->local_node;
	struct nvmeibt_nm_linkable *lrn, *lra, *lp;
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_remote_addr *ra;
	struct nvmeibt_nm_path *path;
	struct udp_path *udp_path;
	
	NFIN;

	/* Traverse through all remote nodes -> addresses -> paths */
	XDLIST_FOREACH(lrn, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(lrn);
		XDLIST_FOREACH(lra, &rn->addresses) {
			ra = nvmeibt_nm_l2ra(lra);
			
			XDLIST_FOREACH(lp, &ra->ready) {
				path = nvmeibt_nm_l2p(lp);
				if (path->pp == &pp->base) {
					udp_path = path2udp_path(path);					
					XDLIST_FOREACH_SAFE(item, &udp_path->pending) {
						N_Tf(retry_pending_msg_trace, "Sending pending message to path @STR", path->name);
						rv = send_udp_msg_to_path(item->msg, item->msg_len, path);
						if (rv) {
							N_Ef(retry_pending_msg_error, "Failed to send message to path @STR rv = @INT", path->name, rv);
							goto out;
						}
						XDLIST_DEL(&item->link);
						if (item->free_func) {
							item->free_func(item);
						}
						nvmeibt_bm_free_buffer(item);
					}
				}
			}
		}
	}
	mod_pp_fd(pp, false);
	
out:
	NFOUT;
	return rv;
}

static int handle_udp_msg(void *ctx, int is_read, int is_write, int dry_tries) {
	struct udp_per_port *pp = (struct udp_per_port *)ctx;
	struct udp_msg *msg;
	uint32_t msg_type;
	struct udp_login_msg *login_msg;
	struct udp_login_resp *login_resp_msg;
	struct udp_ping_request *ping_msg;
	struct sockaddr_storage from;
	socklen_t len = sizeof(from);
	union ibv_gid gid;
	struct nvmeibt_nm_remote_addr *ra;
	struct nvmeibt_nm_path *path;
	ssize_t llen;
	struct udp_on_connection_data conn_data = {.pp = pp};
	(void) is_read;
	(void) is_write;
	(void) dry_tries;

	N_Df(handle_udp_msg_start, "FD @FD is_read @INT is_write @INT dry_tries @INT", pp->sockfd, is_read, is_write, dry_tries);

	/* first handle pending messages */
	if (is_write) {
		handle_pending_messages(pp);
	}

start:
	if (dry_tries-- == 0) {
		N_Tf(handle_udp_msg_not_dry, "@FD not dry ETIMEDOUT", pp->sockfd);
		return ETIMEDOUT;
	}
	llen = recvfrom(pp->sockfd, pp->buf, pp->base.mtu, MSG_DONTWAIT, (struct sockaddr*)&from, &len);
	if (llen <= 0)
		goto out;

	msg = pp->buf;
	msg_type = be32toh(msg->msg_type);
	
	switch (from.ss_family)
	{
	case AF_INET:
		ipv4_to_gid(((struct sockaddr_in *)(&from))->sin_addr.s_addr, &gid);
		break;
	case AF_INET6:
		ipv6_to_gid(&(((struct sockaddr_in6 *)(&from))->sin6_addr), &gid);
	}

	switch (msg_type)
	{
	case udp_msg_type_login_req:
		login_msg = (struct udp_login_msg *)msg;
		conn_data.addr = &from;
		nvmeibt_nm_on_connection_request((struct nvmeibt_nm_login_data *)(&login_msg->data), &pp->base, &conn_data);
		break;
	case udp_msg_type_login_resp:
		login_resp_msg = (struct udp_login_resp *)msg;
		if (login_resp_msg->accepted)
			connection_established(pp, &login_resp_msg->data);
		else
			connection_rejected(pp, &login_resp_msg->data);
		break;
	case udp_msg_type_ping_req:
		ping_msg = (struct udp_ping_request *)msg;
		ping_msg->v.raw = be32toh(ping_msg->v.raw);
		ra = nvmeibt_nm_find_node_remote_address_by_gid(pp->base.pn, &gid);
		if (!ra) {
			N_ETf(nm_fsfds, "couldn't find matching ra");
			break;
		}
		path = get_path_by_ra_port(ra, &pp->base);
		if (!path) {
			N_ETf(nm_fsfdspath, "couldn't find matching path");
			break;
		}
		nvmeibt_nm_on_recv_ping(path, &ping_msg->v);
		break;
	case udp_msg_type_srm:
		ra = nvmeibt_nm_find_node_remote_address_by_gid(pp->base.pn, &gid);
		if (!ra) {
			N_ETf(nm_fsfds2, "couldn't find matching ra");
			break;
		}
		path = get_path_by_ra_port(ra, &pp->base);
		if (!path) {
			N_ETf(nm_fsfdspath2, "couldn't find matching path");
			break;
		}
		nvmeibt_srm_receive_completion(path->srm, (void *)msg + sizeof(struct udp_msg));
		if (path->is_rtr)
			srm_post_recv_cmpl(path->srm);
		if (path->is_rts)
			srm_post_send_cmpl(path->srm);
		break;
	default:
		N_ETf(nm_fsfdrfd, "Got unknown opcode");
	}
	goto start;

out:
	return 0;
}

int nvmeibt_nm_hw_create_port(struct nvmeibt_nm_per_port *pp) {
	struct udp_per_port *udp_port = (struct udp_per_port *)(pp); 
	struct udp_per_nic *udp_nic = (struct udp_per_nic *)(pp->pn); 
	int rv = -1;
	int optval = 1;
	struct sockaddr_storage a;
	char b[TOMA_SOCKADDR_STRING_LEN];
	struct local_nic_port_data *d;
	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_udp_msg,
		.ctx = pp,
		.wait_read = 1,
		.wait_write = 0,
	};

	NFIN;
	pp->mtu = udp_nic->ndev->prop.mtu;

	pp->pn->local_node->renew_status = 1;
	pp->is_dead = 0;
	pp->transport = rtr_tcp;
	udp_port->sockfd = -1;

	if ((d = get_port_info(pp))) {
		nvmeibt_ib_common_rdma_gid2ip(&a, &d->sw_gid, NVMEIBT_UDP_SERVER_PORT, 0, 0);
		N_Tf(nm_handle_roce_port_t1003, "Now working on port @STR",
			nvmeibt_nm_tss(&a, b, sizeof(b)));
		nvmeibt_nm_tss(&a, pp->name, sizeof(pp->name));
	} else {
		N_ETf(no_portinfo, "Can't match nic");
		goto dead;
	}

	pp->gid = d->sw_gid;

	if (memcmp(&pp->gid, &pp->conf_gid, sizeof(pp->conf_gid))) {
		N_Tf(udp_create_port, "Conf gid doesn't match current gid on @STR", pp->name);
		goto dead;
	}

	if ( (udp_port->sockfd = socket(a.ss_family, SOCK_DGRAM, 0)) < 0 ) {
		N_ETf(udp_create_pp, "Failed to allocate socket");
		goto dead;
    }

	if (setsockopt(udp_port->sockfd, SOL_SOCKET, SO_REUSEADDR,
					&optval , sizeof(optval))) {
		N_ETf(udp_reuseaddr, "Failed to resuse address:  @AUTO_ERRNO");
		goto close;
	}

	if (setsockopt(udp_port->sockfd, SOL_SOCKET, SO_REUSEPORT,
					&optval , sizeof(optval))) {
		N_ETf(udp_reuseport, "Failed to resuse address: @AUTO_ERRNO");
		goto close;
	}

	if (setsockopt(udp_port->sockfd, SOL_SOCKET, SO_BINDTODEVICE,
					 pp->pn->dev_name, strlen(pp->pn->dev_name))) {
		N_ETf(udp_setsockopt, "Failed to setsockopt: @AUTO_ERRNO");
		goto close;
	}
	
	if (bind(udp_port->sockfd, (struct sockaddr *)&a, 
            sizeof(a)) < 0)
    {
		N_ETf(udp_bind, "Failed to bind socket: @AUTO_ERRNO");
		goto close;
    }

	if ((rv = nvmeibt_nonblock_fd(udp_port->sockfd)) < 0) {
		N_ETf(udp_add_nic_e11, "Fail to make async_event_fd non-blocking");
		goto close;
	}

	pfd.fd = udp_port->sockfd;
	if (nvmeibt_nm_add_fd(pp->pn->local_node, &pfd)) {
		N_ETf(udp_add_nic_e112, "Fail to add fd");
		goto close;
	}

	if(!(udp_port->buf = NNVMEIBT_TOMA_CALLOC(udp_allocate_buf, 1, pp->mtu))) {
		N_ETf(udp_buf_alloc_fail, "Fail to alloc buf");
		goto del_fd;
	}
	pp->is_valid = 1;
	
	rv = 0;
	goto out;

del_fd:
	nvmeibt_nm_del_fd(pp->pn->local_node, udp_port->sockfd);

close:
	NNVMEIBT_CLOSE(udp_close_fail, udp_port->sockfd);

dead:
	pp->is_dead = 1;

out:
	NFOUT;
	return rv;
}

void nvmeibt_nm_hw_free_port(struct nvmeibt_nm_per_port *pp) {
	struct udp_per_port *udp_pp = (struct udp_per_port *)pp;

	if (udp_pp->sockfd >= 0) {
		nvmeibt_nm_del_fd(pp->pn->local_node, udp_pp->sockfd);
		NNVMEIBT_CLOSE(udp_port_close, udp_pp->sockfd);
	}
	NNVMEIBT_TOMA_FREE(udp_free_port, udp_pp->buf);
}

void nvmeibt_nm_hw_free_nic(struct nvmeibt_nm_per_nic *pn) {
	(void) pn;
	return;
}

bool nvmeibt_nm_hw_should_revive_port(struct nvmeibt_nm_per_port *pp) {
	(void) pp;
	return false;
}

void nvmeibt_nm_hw_free_local_node(struct nvmeibt_nm_local_node *ln) {
	struct udp_local_node *udp_ln = (struct udp_local_node *)ln;
	struct nvmeibt_netdev *netdev;
	struct nvmeibt_netdev_addr *netdev_addr;
	

	nvmeibt_nm_del_fd(ln, udp_ln->netlink_fd);
	NNVMEIBT_CLOSE(local_node_close_netlink, udp_ln->netlink_fd);

	XDLIST_FOREACH_SAFE(netdev, nvmeibt_net_get_netdevs()) {
		XDLIST_FOREACH_SAFE(netdev_addr, &netdev->addr_list) {
			NNVMEIBT_TOMA_FREE(udp_free_netdev_addr, netdev_addr);
		}
		XDLIST_DEL(&netdev->link);
		NNVMEIBT_TOMA_FREE(udp_free_netdev, netdev);
	}

	return;
}

void nvmeibt_nm_hw_wait_events(struct nvmeibt_nm_local_node *ln) {
	(void) ln;
	return;
}

unsigned short nvmeibt_nm_hw_get_port(void) {
	return NVMEIBT_UDP_SERVER_PORT;
}
