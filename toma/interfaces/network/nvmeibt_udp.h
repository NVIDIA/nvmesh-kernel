#if 0
#ifndef ___NVMEIBT_UDP__H_
#define ___NVMEIBT_UDP__H_

#include <sys/types.h>
#include <sys/socket.h>

#include "nvmeibt_ds.h"
#include "nvmeibt_net.h"


#define NVMEIBT_UDP_SERVER_PORT 4100

struct nvmeibt_node;
struct udp_server;
struct udp_peer;
struct nvmeibt_srm;
struct nvmeibt_netdev;
struct udp_route_info;

struct udp_peer {
	struct nvmeibt_node *node;
	int ip_protocol;	// [ AF_INET | AF_INET6 ]
	uint16_t index;		// local
	uint16_t remote_index;
	union ibv_gid gid;
	union sockaddr_union sockaddr;
	char addrstr[INET6_ADDRSTRLEN + 1]; 
	struct nvmeibt_srm *srm;
	void *srm_msgs_buffer;
	uint16_t ping_send_id;
	uint16_t ping_recv_id;
	struct timeval last_event_timeval;
	struct udp_route_info *route;
	struct xdlist node_link;
};

static inline bool is_peer_reachable(const struct udp_peer *p)
{
	return !!p->route;
}

int udp_server_sock(void);
int udp_server_timer(void);
struct nvmeibt_srm *udp_peer_srm(struct udp_peer *peer);
int allocate_udp_server(int ip_protocol, union ibv_gid *gid, struct nvmeibt_node *node);
int start_udp_server(void);
int nvmeib_register_udp_peer(struct nvmeibt_node *node,
			const char *peer_name, const char *peer_guid);
void nvmeibt_udp_read_event(int __attribute__((unused)) fd, int is_functional);
void nvmeibt_udp_timer_event(int fd, int functional);
void nvmeib_destroy_udp_peer(struct udp_peer **peer);
struct nvmeibt_nic;
void nvmeib_destroy_udp_peer_by_nic(struct nvmeibt_nic *nic);
void nvmeibt_udp_set_timer_params(uint64_t leader_heartbeat_timeout_usec);

#endif /* ___NVMEIBT_UDP__H_ */
#endif