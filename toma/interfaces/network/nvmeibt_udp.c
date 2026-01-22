/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#if 0
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/timerfd.h>

#include "nvmeibt_common.h"
#include "nvmeibt_debug.h"

#include "nvmeibt_toma.h"
#include "nvmeibt_net.h"
#include "nvmeibt_node.h"
#include "nvmeibt_udp.h"
#include "nvmeibt_srm.h"
#include "nvmeibt_nic.h"

#define UNKNOWN_INDEX ((uint16_t)-1)

static inline bool is_node_reachable(struct nvmeibt_node *node)
{
	bool verdict = false;
	struct udp_peer *p;
	XDLIST_FOREACH(p, &node->udp_peers_q) {
		if (is_peer_reachable(p)) {
			verdict = true;
			break;
		}
	}
	return verdict;
}

enum udp_msg_type {
	udp_ping_request = 0,
	udp_ping_response,
	udp_srm_data,
	udp_msg_types_num // keep this one last
};

const char *udp_msg_type_name[udp_msg_types_num] = {
	"udp_ping_request",
	"udp_ping_response",
	"udp_srm_data",
};

struct udp_srm_header {
	uint16_t sender_index;
	uint16_t receiver_index;
	uint32_t msg_type;
} __attribute__ ((packed));

struct udp_ping {
	uint64_t route_id;
	struct timeval timestamp;
	uint16_t id;
} __attribute__ ((packed));

struct udp_ping_msg {
	struct udp_srm_header head;
	struct udp_ping data;
} __attribute__ ((packed));

struct udp_server {
	int socket;
	int timer_fd;
	u32 max_chunk_size;
	void *recv_buffer;
	struct udp_peer self;
	struct nvmeibt_srm *srm;
	struct timeval last_udp_recv_timeval;
};

struct udp_route_info {
#define udpr_ctrl_len (sizeof(struct cmsghdr) + sizeof(struct in_pktinfo))
#define udpr_in6_ctrl_len (sizeof(struct cmsghdr) + sizeof(struct in6_pktinfo))
	
	union {
		const struct nvmeibt_netdev_addr *netdev_addr;
		const uintptr_t route_id;
	} key;

	union {
		struct cmsghdr cmsg;
		uint8_t _aux[udpr_ctrl_len];
		uint8_t _aux_in6[udpr_in6_ctrl_len];
	} control;
	size_t ctrl_len;

	struct xdlist link;
};

static int send_udp_msg(struct udp_peer *peer, struct udp_route_info *route,
						 void *msg, size_t msglen);

static XDLIST_DECLARE(, struct udp_route_info, link) udp_routes_q;

static struct udp_route_info *udp_fetch_route(uintptr_t route_id)
{
	struct udp_route_info *route = NULL, *ptr;

	XDLIST_FOREACH(ptr, &udp_routes_q) {
		if (ptr->key.route_id == route_id) {
			route = ptr;
			goto out;
		}
	}
out:
	return route;
}

static void add_sender_netdev_addr(const struct nvmeibt_netdev_addr *netdev_addr)
{
	struct udp_route_info *route = NNVMEIBT_TOMA_CALLOC(trace_udp_add_sender_netdev, 1, sizeof(*route));
	struct cmsghdr *cmsg = &route->control.cmsg;
	char srcip[INET6_ADDRSTRLEN + 1];

	route->key.netdev_addr = netdev_addr;

	if (netdev_addr->addr.raw.sa_family == AF_INET) {
		struct in_pktinfo *in_pktinfo = (struct in_pktinfo*) CMSG_DATA(cmsg);
		cmsg->cmsg_level = IPPROTO_IP;
		cmsg->cmsg_type = IP_PKTINFO;
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
		in_pktinfo->ipi_spec_dst.s_addr = netdev_addr->addr.v4.sin_addr.s_addr;
		inet_ntop(AF_INET, &in_pktinfo->ipi_spec_dst.s_addr, srcip, sizeof(srcip) - 1);
		route->ctrl_len = udpr_ctrl_len;
	} else if (netdev_addr->addr.raw.sa_family == AF_INET6) {
		struct in6_pktinfo *in6_pktinfo = (struct in6_pktinfo*) CMSG_DATA(cmsg);
		cmsg->cmsg_level = IPPROTO_IPV6;
		cmsg->cmsg_type = IPV6_PKTINFO;
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
		in6_pktinfo->ipi6_addr = netdev_addr->addr.v6.sin6_addr;
		inet_ntop(AF_INET6, &in6_pktinfo->ipi6_addr, srcip, sizeof(srcip) - 1);
		route->ctrl_len = udpr_in6_ctrl_len;
	} else nvmeibt_abort(ES_FATAL);
	
	N_Tf(trace_add_sender_netdev, "Add source address src=@SRC_STR for netdev=@PTR", srcip, netdev_addr->netdev);

	XDLIST_ADD_TAIL(&udp_routes_q, route);
}

/* udp network is not part of udp server struct
 * node can set number of udp servers, one for each IP
 * these server will span the same network
 */
struct udp_network {
	uint32_t nr_peers;
	struct udp_peer peers[MAX_REMOTE_GIDS];
};

static struct udp_server *udp_server = NULL;
static struct udp_network udp_network = {
	.nr_peers = 0,
};

static inline const char *peer_hostname(struct udp_peer *p)
{
	return nvmeibt_nic_get_node_name(p->node->nics[0]);
}

static void del_sender_netdev_addr(const struct nvmeibt_netdev_addr *netdev_addr)
{
	struct udp_route_info *route;
	uint32_t pi;
	
	XDLIST_FOREACH(route, &udp_routes_q) {
		if (route->key.netdev_addr == netdev_addr) {
			N_Tf(hu7nw43, "Del netdev=@PTR", netdev_addr->netdev);
			/* Delete route from list */
			XDLIST_DEL(&route->link);
			/* Clear route from all peers */
			for (pi=0; pi<udp_network.nr_peers; pi++) {	// look for a free spot
				struct udp_peer *peer = udp_network.peers + pi;

				if (peer->route == route) {
					bool was_connected = is_node_reachable(peer->node);
					peer->route = NULL;
					nvmeibt_srm_stop_all(peer->srm);
					if (!is_node_reachable(peer->node) && was_connected) {
						N_Tf(ft65rs4, "no connection to node @PEER_HOSTNAME", peer_hostname(peer));
						nvmeibt_raft_upd_n_connected_peers(true, false, peer->node);
					}
				}
			}
			/* Free route */
			NNVMEIBT_TOMA_FREE(trace_udp_del_route_free, route);
			break;
		}
	}
}

#define APPLY_UDP_PING_TIMEOUT_FACTOR(ping_timeout) \
	ping_timeout = ping_timeout * 3 / 4

struct nvmeibt_srm *udp_peer_srm(struct udp_peer *peer)
{
	return peer->srm;
}

int udp_server_sock(void)
{
	return udp_server->socket;
}

int udp_server_timer(void)
{
	return udp_server->timer_fd;
}

static struct udp_peer *find_registered_peer(struct udp_network *net, union ibv_gid *gid)
{
	uint32_t i;
	struct udp_peer *peer = NULL;

	for (i = 0; i < net->nr_peers; i++) {
		if (gidcmp(&net->peers[i].gid, gid)) {
			peer = net->peers + i;
			break;
		}
	}
	return peer;
}

static int bind_peer_sockaddr(struct udp_server *usrv)
{
	int rv;
	char addr_str[INET6_ADDRSTRLEN + 1] = "";
	if (usrv->self.ip_protocol == AF_INET) {
		inet_ntop(usrv->self.ip_protocol,
			  &usrv->self.sockaddr.v4.sin_addr,
			  addr_str, sizeof(addr_str) - 1);
	} else {
		inet_ntop(usrv->self.ip_protocol,
			  &usrv->self.sockaddr.v6.sin6_addr,
			  addr_str, sizeof(addr_str) - 1);
	}

	N_Tf(trace_udp_bind_peer_sockaddr, "bind local address=@ADDRESS_STR", addr_str);

	rv = bind(usrv->socket, &usrv->self.sockaddr.raw, sockaddr_len(&usrv->self.sockaddr));
	if (rv) {
		int err = errno;
		N_ETf(error_udp_bind_peer_sockaddr, "server=@SERVER bind @AUTO_ERRNO (@ERR)", addr_str, err);
	}
	return rv;
}

static void build_peer_sockaddr(struct udp_peer *peer)
{
	bzero(&peer->sockaddr, sizeof(peer->sockaddr));
	peer->sockaddr.raw.sa_family = peer->ip_protocol;
	// v4 && v6 sockaddr ports are at the same offset
	peer->sockaddr.v4.sin_port = nvmeib_htons((unsigned short)NVMEIBT_UDP_SERVER_PORT);
	if (peer->ip_protocol == AF_INET) {
		peer->sockaddr.v4.sin_addr.s_addr = roce_convert_gid_ip(&peer->gid);
	} else {
		roce_convert_gid_ip6(&peer->gid, &peer->sockaddr.v6.sin6_addr);
	}
}

int allocate_udp_server(int ip_protocol, union ibv_gid *gid, struct nvmeibt_node *node)
{
	udp_server = NNVMEIBT_TOMA_CALLOC(trace_udp_allocate_udp_server, 1, sizeof(*udp_server));

	if (!gid) {
		if (ip_protocol == AF_INET) {
			ipv4_to_gid(INADDR_ANY, &udp_server->self.gid);
		} else if (ip_protocol== AF_INET6) {
			ipv6_to_gid(&in6addr_any, &udp_server->self.gid);
		}
	} else {
		udp_server->self.gid = *gid;
	}

	udp_server->self.ip_protocol = ip_protocol;
	udp_server->self.node = node;

	build_peer_sockaddr(&udp_server->self);
	return 0;
}

static void *udp_alloc_msgs_buffer(const struct carrier *car)
{
	struct udp_peer *p = car->udp_peer;
	p->srm_msgs_buffer = NNVMEIBT_TOMA_CALLOC(trace_udp_udp_alloc_msgs_buffer, car->max_messages, udp_server->max_chunk_size);
	if (!p->srm_msgs_buffer) {
		N_Ef(error_udp_udp_alloc_msgs_buffer, "failed to allocate memory for udp messages");
	}
	return p->srm_msgs_buffer;
}

static void udp_release_msgs_buffer(const struct carrier *car)
{
	struct udp_peer *p = car->udp_peer;
	NNVMEIBT_TOMA_FREE(trace_udp_udp_release_msgs_buffer, p->srm_msgs_buffer);
}

static void *udp_carrier_obj(const struct carrier *car)
{
	return car->type == carrier_udp ? car->udp_peer : NULL;
}

static struct nvmeibt_srm *udp_srm_obj(const struct carrier *car)
{
	return car->type == carrier_udp ? car->udp_peer->srm : NULL;
}

static const char *udp_conn_name(const struct carrier *car)
{
	return peer_hostname(car->udp_peer);
}

static int udp_send_srm(const struct carrier *car, struct nvmeibt_wire_msg *msg,
			uint64_t __attribute__ ((unused)) msg_id,
			bool __attribute__ ((unused)) send_ib_signal)
{
	int rv = -1;
	struct udp_peer *p = car->udp_peer;
	struct udp_srm_header *h = msg->buffer;

	if (!is_peer_reachable(p)) {
		N_Tf(trace_udp_udp_send_srm, "peer=@PEER no route", peer_hostname(p));
		goto out;
	} else if (!is_sender_up(p->route->key.netdev_addr->netdev)) {
		N_Tf(trace_1_udp_udp_send_srm, "peer=@PEER sender=@SENDER is down",
			peer_hostname(p), p->route->key.netdev_addr->netdev->name);
		goto out;
	}

	msg->len += sizeof(*h);
	h->sender_index = p->index;
	h->receiver_index = p->remote_index;
	h->msg_type = udp_srm_data;

	rv = send_udp_msg(p, p->route, msg->buffer, msg->len);

out:
	return rv;
}

static struct nvmeibt_srm *udp_srm_create(struct udp_peer *peer)
{
	const struct carrier car = {
		.type 		= carrier_udp,
		.max_chunk_size = udp_server->max_chunk_size - sizeof(struct udp_srm_header),
		.max_messages	= SRM_MAX_MSG_NUM,
		.srm_data_offset = sizeof(struct udp_srm_header),
		{
			.udp_peer	= peer,
		},

		.conn_name	= udp_conn_name,
		.carrier_obj	= udp_carrier_obj,
		.srm_obj	= udp_srm_obj,
		.alloc_msgs_buffer = udp_alloc_msgs_buffer,
		.release_msgs_buffer = udp_release_msgs_buffer,
		.send_msg = udp_send_srm,
	};

	return nvmeibt_srm_create(&car);
}

int start_udp_server(void)
{
	int rv, err, optval;
	NFIN;

	XDLIST_HEAD_INIT(&udp_routes_q);

	udp_server->socket = socket(udp_server->self.ip_protocol, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (udp_server->socket < 0) {
		err = errno;
		N_Ef(error_udp_start_udp_server, "failed to open server socket gid=@GID_LONG@INTERFACE_ID err=@ERR",
			(uint64_t)udp_server->self.gid.global.subnet_prefix,
			(uint64_t)udp_server->self.gid.global.interface_id,
			err);
		goto out;
	}

	/* rerun the server immediately after we kill it;
	* otherwise we have to wait about 20 secs.
	* Eliminates "ERROR on binding: Address already in use" error.
	*/
	optval = 1;
	rv = setsockopt(udp_server->socket, SOL_SOCKET, SO_REUSEADDR,
					&optval , sizeof(int));
	if (rv) {
		err = errno;
		N_Ef(error_1_udp_start_udp_server, "setsock failure gid=@GID_LONG@INTERFACE_ID err=@ERR",
			(uint64_t)udp_server->self.gid.global.subnet_prefix,
			(uint64_t)udp_server->self.gid.global.interface_id,
			err);
		goto close_sock;
	}

	N_Tf(trace_udp_start_udp_server, "starting udp server gid=@GID_LONG@INTERFACE_ID",
		(uint64_t)udp_server->self.gid.global.subnet_prefix,
		(uint64_t)udp_server->self.gid.global.interface_id);

	rv = bind_peer_sockaddr(udp_server);
	if (rv) {
		goto close_sock;
	}

	udp_server->max_chunk_size = 1500 - 100; /* WTF */
	udp_server->recv_buffer = NNVMEIBT_TOMA_CALLOC(trace_1_udp_start_udp_server, 1, udp_server->max_chunk_size);
	if (!udp_server->recv_buffer) {
		N_Ef(error_2_udp_start_udp_server, "failed to allocate udp recv buffer");
		goto close_sock;
	}

	udp_server->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (udp_server->timer_fd == -1) {
		err = errno;
		N_Ef(error_3_udp_start_udp_server, "failed to create udp server timer err=@ERR", err);
		goto close_sock;
	} else {
		struct itimerspec tmspec;
		uint64_t ping_timeout_nsec;

		tmspec.it_value.tv_sec = 0;
		tmspec.it_value.tv_nsec = 500000000;
		ping_timeout_nsec = nvmeibt_raft_get_leader_heartbeat_timeout_usec() * 1000;
		APPLY_UDP_PING_TIMEOUT_FACTOR(ping_timeout_nsec);
		tmspec.it_interval.tv_sec = ping_timeout_nsec / 1000000000;
		tmspec.it_interval.tv_nsec = ping_timeout_nsec % 1000000000;
		rv = timerfd_settime(udp_server->timer_fd, 0, &tmspec, NULL);
		if (rv) {
			err = errno;
			N_Ef(error_4_udp_start_udp_server, "failed to start udp server timer err=@ERR", err);
			goto close_tm;
		}
	}

	N_Tf(trace_2_udp_start_udp_server, "udp server is running gid=@GID_LONG@INTERFACE_ID",
		(uint64_t)udp_server->self.gid.global.subnet_prefix,
		(uint64_t)udp_server->self.gid.global.interface_id);

	NFOUT;
	return 0;

close_tm:
	close(udp_server->timer_fd);
	udp_server->timer_fd = -EINVAL;
close_sock:
	close(udp_server->socket);
	udp_server->socket = -EINVAL;
out:
	return -1;
}


static struct nvmeibt_netdev_addr *add_peer_to_mesh_if_local_and_new(struct udp_peer *peer)
{
	struct nvmeibt_netdev_addr *sender_addr;

	sender_addr = find_netdev_addr_by_gid(&peer->gid);
	if (sender_addr && !sender_addr->connected_to_mesh) {
		sender_addr->netdev->prop.in_mesh = 1;
		sender_addr->netdev->del_addr_cb = del_sender_netdev_addr;
		add_sender_netdev_addr(sender_addr);
		sender_addr->connected_to_mesh = 1;
	}
	return sender_addr;
}

int nvmeib_register_udp_peer(struct nvmeibt_node *node,
			const char *peer_name, const char *peer_guid)
{
	int rv = -1;
	struct udp_peer *p;
	union ibv_gid gid;
	struct nvmeibt_netdev_addr *sender_addr;
	uint16_t pi;

	NFIN;

	rv = nvmeibt_ib_device_uuid_str_to_raw(&gid, peer_guid);
	if (rv) {
		N_Wf(warn_1_udp_nvmeib_register_udp_peer, "peer=@PEER failed to extract gid from guid=@GUID", peer_name, peer_guid);
		goto out;
	}

	p = find_registered_peer(&udp_network, &gid);
	if (p) {
		N_Df(trace_udp_nvmeib_register_udp_peer, "existing peer=@PEER found guid=@GUID index=@INDEX",
			peer_name, peer_guid, p->index);
		add_peer_to_mesh_if_local_and_new(p);
		rv = 0;
		goto out;
	}

	for (pi=0; pi<udp_network.nr_peers; pi++) {	// look for a free spot
		if (udp_network.peers[pi].index == UNKNOWN_INDEX) {
			p = &udp_network.peers[pi];
			p->index = pi;
			break;
		}
	}
	if (!p) {
		if (udp_network.nr_peers == MAX_REMOTE_GIDS) {
			N_Wf(warn_udp_nvmeib_register_udp_peer, "cannot add new peer");
			goto out;
		}
		p = &udp_network.peers[udp_network.nr_peers];
		p->index = udp_network.nr_peers++;
	}

	/* Should be called address_family.
	 * An IPv6 listener on ipv6_addr_any can also handle IPv4 connections,
	  * but the AF returned from recvfrom is still AF_INET6 */
	p->ip_protocol = udp_server->self.ip_protocol;

	p->gid = gid;
	p->node = node;
	p->srm = udp_srm_create(p);
	if (!p->srm) {
		N_Ef(error_udp_nvmeib_register_udp_peer, "failed to init peer srm");
		p->index = UNKNOWN_INDEX;
		rv = -1;
		goto out;
	}
	p->ping_send_id = 0;
	p->ping_recv_id = 0;
	p->remote_index = UNKNOWN_INDEX;
	XDLIST_ADD_TAIL(&node->udp_peers_q, p);
	build_peer_sockaddr(p);

	sender_addr = add_peer_to_mesh_if_local_and_new(p);

	sockaddr_ntop(&p->sockaddr, p->addrstr, sizeof(p->addrstr) - 1);
	N_Tf(trace_1_udp_nvmeib_register_udp_peer, "new peer id=@INDEX node=@NODE addr=@ADDR_STR gid=@GID_LONG@INTERFACE_ID netdev=@NETDEV", p->index,
		peer_name, p->addrstr,
		(uint64_t)p->gid.global.subnet_prefix,
		(uint64_t)p->gid.global.interface_id,
		sender_addr ? netdev_name(sender_addr->netdev) : "REMOTE");
	rv = 0;

out:
	NFOUT;
	return rv;
}

void nvmeib_destroy_udp_peer(struct udp_peer **peer)
{
	struct udp_peer					*p;
	struct nvmeibt_srm				*srm;

	if (!peer)
		return;

	p = *peer;
	if (p && p->index < udp_network.nr_peers && p == &udp_network.peers[p->index]) {
		srm = udp_peer_srm(p);
		if (srm)
			nvmeibt_srm_free(srm);

		N_Tf(bg67y54, "delete peer id=@INDEX addr=@ADDR_STR gid=@GID_LONG@INTERFACE_ID",
			 p->index, p->addrstr, (uint64_t)p->gid.global.subnet_prefix, (uint64_t)p->gid.global.interface_id);
		XDLIST_DEL(&p->node_link);
		if (p->index == udp_network.nr_peers-1) {
			udp_network.nr_peers--;
		}
		else {
			memset(p, 0, sizeof(*p));
			p->index = UNKNOWN_INDEX;		// mark entry as unused
		}
	}
	*peer = NULL;
}

void nvmeib_destroy_udp_peer_by_nic(struct nvmeibt_nic *nic)
{
	struct udp_peer					*p;
	union ibv_gid					gid;

	if (nvmeibt_ib_device_uuid_str_to_raw(&gid, nic->from_config.guid_str)) {
		N_Wf(dr56y7z, "failed to extract gid from guid=@GUID", nic->from_config.guid_str);
		return;
	}

	p = find_registered_peer(&udp_network, &gid);
	if (p) {
		nvmeib_destroy_udp_peer(&p);
	}
	else {
		N_Wf(sdfg6a1, "peer not found, guid=@GUID", nic->from_config.guid_str);
	}
}

static bool validate_received_index(uint16_t pi, union sockaddr_union *addr)
{
	if (pi == UNKNOWN_INDEX)
		return false;
	if (pi >= udp_network.nr_peers)
		return false;
	if (udp_network.peers[pi].index != pi)
		return false;
	return sockaddr_addr_equal(&udp_network.peers[pi].sockaddr, addr);
}

static struct udp_peer *locate_receiver_peer(struct udp_srm_header *h, union sockaddr_union *addr)
{
	struct udp_peer *p = NULL;

	if (validate_received_index(h->receiver_index, addr)) {
		p = udp_network.peers + h->receiver_index;
	} else for (uint32_t pi = 0; pi < udp_network.nr_peers; pi++) {
		if (validate_received_index(pi, addr)) {
			p = udp_network.peers + pi;
			N_Df(trace_udp_locate_receiver_peer, "peer=@PEER has local index=@INDEX",
			    peer_hostname(p), pi);
			goto out;
		}
	}
out:
	return p;
}

typedef int (*udp_recv_cb_t)(struct udp_peer *peer, void *recv_buf, uint32_t buf_size);
static int udp_recv_ping_request(struct udp_peer *peer, void *buf, uint32_t size);
static int udp_recv_ping_response(struct udp_peer *peer, void *buf, uint32_t size);
static int udp_recv_srm(struct udp_peer *peer, void *buf, uint32_t size);
static const udp_recv_cb_t udp_recv_cb[udp_msg_types_num] = {
	[udp_ping_request] = udp_recv_ping_request,
	[udp_ping_response] = udp_recv_ping_response,
	[udp_srm_data] = udp_recv_srm,
};

void nvmeibt_udp_read_event(int __attribute__((unused)) fd, int is_functional)
{
	int rv, cont = 1;
	union sockaddr_union sockaddr;
	socklen_t addrlen = sizeof(sockaddr);
	struct udp_peer *p;
	struct udp_srm_header *h;
	void *data;
	uint32_t data_sz, px;
	char ip_str[INET6_ADDRSTRLEN + 1];

	NFIN;
	do {
		rv = recvfrom(udp_server->socket, udp_server->recv_buffer,
				udp_server->max_chunk_size, MSG_DONTWAIT,
				&sockaddr.raw, &addrlen);
		if (rv < 0) {
			int err = errno;
			if (err != EAGAIN && err != EWOULDBLOCK) {
				N_Ef(error_udp_nvmeibt_udp_read_event, "recvfrom failed err=@ERR", err);
			}
			goto out;
		} else if (!rv) {
			N_Tf(trace_udp_nvmeibt_udp_read_event, "no data to read from udp socket");
			goto out;
		}

		if (!is_functional)
			continue;

		if (sockaddr.raw.sa_family == AF_INET)
			inet_ntop(AF_INET, &sockaddr.v4.sin_addr,
				  ip_str, sizeof(ip_str) - 1);
		else if (sockaddr.raw.sa_family == AF_INET6)
			inet_ntop(AF_INET6, &sockaddr.v6.sin6_addr,
					ip_str, sizeof(ip_str) - 1);
		else
			nvmeibt_abort(ES_FATAL);

		h = udp_server->recv_buffer;
		if (h->msg_type >= udp_msg_types_num) {
			N_Ef(error_1_udp_nvmeibt_udp_read_event, "invalid msg_type=@MSG_TYPE from @IPV4_STR", h->msg_type, ip_str);
			nvmeibt_abort(ES_FATAL);
		}
		data = h + 1;
		data_sz = rv - sizeof(*h);
		N_Tf(trace_1_udp_nvmeibt_udp_read_event, "received data from @IPV4_STR: @SENDER_INDEX:@RECEIVER_INDEX:@UDP_MSG_TYPE_NAME",
			ip_str, h->sender_index, h->receiver_index,
			udp_msg_type_name[h->msg_type]);

		p = locate_receiver_peer(h, &sockaddr);
		if (!p) {
			N_Wf(warn_udp_nvmeibt_udp_read_event, "peer @IPV4_STR not found", ip_str);
			continue;
		} else if (p->remote_index != h->sender_index) {
			N_Tf(trace_2_udp_nvmeibt_udp_read_event,
				 "update peer=@PEER index @REMOTE_INDEX->@SENDER_INDEX",
				 peer_hostname(p),
				 p->remote_index, h->sender_index);
			p->remote_index = h->sender_index;
		}

		gettimeofday(&p->last_event_timeval, NULL);
		udp_recv_cb[h->msg_type](p, data, data_sz);
	} while (cont);

out:
	for (px = 0; px < udp_network.nr_peers; px++) {
		p = udp_network.peers + px;
		if (p->index != UNKNOWN_INDEX) {
			nvmeibt_srm_post_cmpl(p->srm);
		}
	}
	NFOUT;
	return;
}

static int udp_recv_srm(struct udp_peer *peer, void *buf, uint32_t size)
{
	N_Tf(trace_udp_udp_recv_srm,
		 "peer=@PEER data size @SIZE", peer_hostname(peer), size);
	nvmeibt_srm_receive_completion(peer->srm, buf);
	return 0;
}

static int send_udp_msg(struct udp_peer *peer, struct udp_route_info *route,
						 void *msg, size_t msglen)
{
	int rv;
	struct msghdr msghdr;
	struct iovec iov = { .iov_base = msg, .iov_len = msglen };

	bzero(&msghdr, sizeof(msghdr));
	msghdr.msg_name = &peer->sockaddr.raw;
	msghdr.msg_namelen = sockaddr_len(&udp_server->self.sockaddr);
	msghdr.msg_iov = &iov;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = &route->control;
	msghdr.msg_controllen = route->ctrl_len;

	rv = sendmsg(udp_server->socket, &msghdr, MSG_DONTWAIT);
	if (rv != (typeof(rv))msglen) {
		int err = errno;
		struct cmsghdr *cmsg = &route->control.cmsg;
		char srcip[INET6_ADDRSTRLEN + 1] = "";

		if (cmsg->cmsg_type == IP_PKTINFO) {
			struct in_pktinfo *pktinfo = (struct in_pktinfo*) CMSG_DATA(cmsg);
			inet_ntop(AF_INET, &pktinfo->ipi_spec_dst.s_addr, srcip, INET6_ADDRSTRLEN);
		} else if (cmsg->cmsg_type == IPV6_PKTINFO) {
			struct in6_pktinfo *pktinfo = (struct in6_pktinfo *) CMSG_DATA(cmsg);
			inet_ntop(AF_INET6, &pktinfo->ipi6_addr, srcip, INET6_ADDRSTRLEN);
		} else nvmeibt_abort(ES_FATAL);
		N_Tf(trace_udp_send_udp_msg, "peer=@PEER src=@SRC_STR dst=@DST_STR sendmsg failed "
			"msglen=@MSGLEN rv=@RV netdev=@PTR err=@ERR",
			peer_hostname(peer), srcip, peer->addrstr, msglen, rv, route->key.netdev_addr->netdev, err);

		// Handle EAGAIN strange case, it shouldn't happen on UDP but it does
		if (err == EAGAIN) {
			int optval;

			N_Ef(trace_send_udp_msg_e1, "EAGAIN received on UDP socket, restarting UDP comm");
			close(udp_server->socket);
			udp_server->socket = -EINVAL;
			udp_server->socket = socket(udp_server->self.ip_protocol, SOCK_DGRAM | SOCK_CLOEXEC, 0);
			if (udp_server->socket < 0) {
				err = errno;
				N_Ef(trace_send_udp_msg_e2, "failed to open server socket gid=@GID_LONG@INTERFACE_ID err=@ERR",
					(uint64_t)udp_server->self.gid.global.subnet_prefix,
					(uint64_t)udp_server->self.gid.global.interface_id,
					err);
				nvmeibt_abort(ES_FATAL);
			}
			optval = 1;
			setsockopt(udp_server->socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
			rv = bind_peer_sockaddr(udp_server);
			if (rv) {
				close(udp_server->socket);
				udp_server->socket = -EINVAL;
			}
			nvmeibt_toma_mark_is_need_to_update_the_main_select_fds();
		}
		rv = -1;
	} else {
		rv = 0;
	}
	return rv;
}


static void udp_ping_format(struct udp_ping_msg *ping, struct udp_peer *peer,
								enum udp_msg_type type,
								uint64_t route_id, uint16_t ping_id)
{
	ping->head.sender_index = peer->index;
	ping->head.receiver_index = peer->remote_index;
	ping->head.msg_type = type;
	ping->data.id = ping_id;
	ping->data.route_id = route_id;
}

static void send_udp_ping(struct udp_ping_msg *ping, struct udp_peer *peer,
							enum udp_msg_type type)
{
retry:
	if (is_peer_reachable(peer)) {
		if (!is_sender_up(peer->route->key.netdev_addr->netdev)) {
			peer->route = NULL;
			nvmeibt_srm_stop_all(peer->srm);
			if (!is_node_reachable(peer->node)) {
				N_Tf(info_udp_send_udp_ping, "no connection to node @PEER_HOSTNAME", peer_hostname(peer));
				nvmeibt_raft_upd_n_connected_peers(true, false, peer->node);
			}
			goto retry;
		}

		if (type == udp_ping_request)
			ping->data.route_id = peer->route->key.route_id;
		send_udp_msg(peer, peer->route, ping, sizeof(*ping));
	} else {
		struct udp_route_info *route;
		XDLIST_FOREACH(route, &udp_routes_q) {
			if (!is_sender_up(route->key.netdev_addr->netdev)) continue;

			if (type == udp_ping_request)
				ping->data.route_id = route->key.route_id;
			send_udp_msg(peer, route, ping, sizeof(*ping));
		}

	}

	N_Df(trace_udp_send_udp_ping, "peer=@PEER type=@TYPE_STR local=@LOCAL_INT remote=@REMOTE_INT ping_id=@PING_ID af=@IFA_FAMILY src=@SRC_STR dst=@DST_STR",
		peer_hostname(peer), udp_msg_type_name[type],
		peer->index, peer->remote_index, ping->data.id,
		udp_server->self.sockaddr.raw.sa_family,
		peer->route ? peer->route->key.netdev_addr->addr_str : "none", peer->addrstr);
}

static int udp_recv_ping_response(struct udp_peer *peer, void *buf,
				  uint32_t __attribute__((unused)) size)
{
	struct udp_ping *ping = buf;
	bool was_connected = is_node_reachable(peer->node);
	struct timeval now;
	long rtrip;

	gettimeofday(&now, NULL);
	rtrip = timeval_diff_usec(&now, &ping->timestamp);
	N_Tf(trace_udp_udp_recv_ping_response, "peer=@PEER id=@ID_INT route_id=@ROUTE_ID connected=@CONNECTED rtrip=@RTRIP us",
		peer_hostname(peer), ping->id, ping->route_id, was_connected, rtrip);

	peer->ping_recv_id = ping->id;

	if (!peer->route) {
		peer->route = udp_fetch_route(ping->route_id);

		if (!was_connected) {
			N_Tf(info_udp_udp_recv_ping_response, "new connection to node @PEER_HOSTNAME", peer_hostname(peer));
		}
		if (!was_connected)
			nvmeibt_raft_upd_n_connected_peers(false, true, peer->node);

		nvmeibt_srm_allow_send(peer->srm, 0);
		nvmeibt_srm_allow_receive(peer->srm, 0);
	}

	return 0;
}

static int udp_recv_ping_request(struct udp_peer *peer, void *buf,
				 uint32_t __attribute__((unused)) size)
{
	struct udp_ping_msg ping;
	struct udp_ping *request = buf;
	N_Tf(trace_udp_udp_recv_ping_request,
		 "peer=@PEER ping_id=@PING_ID route_id=@ROUTE_ID",
		 peer_hostname(peer), request->id, request->route_id);
	udp_ping_format(&ping, peer, udp_ping_response, request->route_id, request->id);
	ping.data.timestamp = request->timestamp;
	send_udp_ping(&ping, peer, udp_ping_response);
	return 0;
}

void nvmeibt_udp_timer_event(int fd, int functional)
{
	struct udp_ping_msg ping;

	if (!functional)
		goto drain_fd;

	for (typeof (udp_network.nr_peers) pi = 0; pi < udp_network.nr_peers; pi++) {
		struct udp_peer *peer = udp_network.peers + pi;
		if (peer->index == UNKNOWN_INDEX)
			continue;

		if (is_peer_reachable(peer) &&
		    peer->ping_send_id - peer->ping_recv_id > 3) {
			peer->route = NULL;
			N_Tf(trace_udp_nvmeibt_udp_timer_event, "peer=@PEER is idle too long", peer_hostname(peer));
			nvmeibt_srm_stop_all(peer->srm);
			if (!is_node_reachable(peer->node)) {
				N_Tf(trace_1_udp_nvmeibt_udp_timer_event, "no connection to node @PEER_HOSTNAME", peer_hostname(peer));
				nvmeibt_raft_upd_n_connected_peers(true, false, peer->node);
			}
		}
		udp_ping_format(&ping, peer, udp_ping_request, 0, peer->ping_send_id++);
		gettimeofday(&ping.data.timestamp, NULL);
		send_udp_ping(&ping, peer, udp_ping_request);
	}

drain_fd:
	do {
		uint64_t tm_buffer;
		read(fd, &tm_buffer, sizeof(tm_buffer));
	} while (0);
}

void nvmeibt_udp_set_timer_params(uint64_t leader_heartbeat_timeout_usec)
{
	struct itimerspec 		tmspec;
	uint64_t 				ping_timeout_nsec;
	int						rv, err;

	if (udp_server && udp_server->timer_fd) {
		ping_timeout_nsec = leader_heartbeat_timeout_usec * 1000;
		APPLY_UDP_PING_TIMEOUT_FACTOR(ping_timeout_nsec);
		tmspec.it_interval.tv_sec = ping_timeout_nsec / 1000000000;
		tmspec.it_interval.tv_nsec = ping_timeout_nsec % 1000000000;
		tmspec.it_value= tmspec.it_interval;
		rv = timerfd_settime(udp_server->timer_fd, 0, &tmspec, NULL);
		if (rv) {
			err = errno;
			N_Ef(f7w2amp, "failed to restart udp server timer err=@ERR", err);
		}
	}
}
#endif