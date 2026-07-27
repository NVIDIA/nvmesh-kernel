#ifndef ___NVMEIBT_NET_H__
#define ___NVMEIBT_NET_H__

#include <stdbool.h>

#include <linux/types.h>
#include <linux/if.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"

#ifdef TOMA_IB_ROCE
#include <infiniband/verbs.h>
#else
union ibv_gid {
	uint8_t         raw[16];
	struct {
		uint64_t    subnet_prefix;
		uint64_t    interface_id;
	} global;
};
#endif

union sockaddr_union {
	struct sockaddr raw;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

static inline bool sockaddr_addr_equal(union sockaddr_union *u1, union sockaddr_union *u2) 
{
	if (u1->raw.sa_family != u2->raw.sa_family)
		return false;
	if (u1->raw.sa_family == AF_INET) {
		return u1->v4.sin_addr.s_addr == u2->v4.sin_addr.s_addr;
	} else if (u1->raw.sa_family == AF_INET6) {
		return memcmp(&u1->v6.sin6_addr, &u2->v6.sin6_addr, sizeof(u1->v6.sin6_addr)) == 0;
	}
	return false;
}

static inline const char *sockaddr_ntop(union sockaddr_union *sa, char *dst, size_t dst_size) {
	if (sa->raw.sa_family == AF_INET)
		return inet_ntop(AF_INET, &sa->v4.sin_addr, dst, dst_size);
	else if (sa->raw.sa_family == AF_INET6)
		return inet_ntop(AF_INET6, &sa->v6.sin6_addr, dst, dst_size);
	return "Invalid AF";
}

static inline socklen_t sockaddr_len(union sockaddr_union *sa)
{
	if (sa->raw.sa_family == AF_INET)
		return sizeof(sa->v4);
	else if (sa->raw.sa_family == AF_INET6)
		return sizeof(sa->v6);
	return sizeof(sa->raw);
}

struct nvmeibt_netdev_addr {
	struct nvmeibt_netdev *netdev;
	union sockaddr_union addr;
	char addr_str[INET6_ADDRSTRLEN + 1];
	bool connected_to_mesh;
	struct xdlist link;
};

struct nvmeibt_netdev {
	char name[IFNAMSIZ];
	XDLIST_DECLARE(, struct nvmeibt_netdev_addr, link) addr_list;
	struct {
		uint64_t mtu:16;
		uint64_t index:8;
		uint64_t oper_state:3; // IF_OPER_UNKNOWN - IF_OPER_UP;
		uint64_t carrier:1;
		uint64_t in_mesh:1;
	} prop;
	void (*del_addr_cb)(const struct nvmeibt_netdev_addr *del_addr);
	struct xdlist link;
	struct xdlist nvmesh_link;
};

static inline bool is_sender_up(const struct nvmeibt_netdev *ndev)
{
	return !!ndev->prop.carrier && ndev->prop.oper_state == IF_OPER_UP;
}

#define MAX_REMOTE_GIDS 128

enum TOMA_STATE_OF_A_CONN {
	TOMA_STATE_OF_A_CONN_CANDIDATE = 0,	// memset(0) initialized
	TOMA_STATE_OF_A_CONN_DYING,
	TOMA_STATE_OF_A_CONN_IN_USE,
	TOMA_STATE_OF_A_CONN_IN_LOOKUP, // cm event is running WQ
	TOMA_STATE_OF_A_CONN_IN_PROGRESS,
	TOMA_STATE_OF_A_CONN_WAIT_PING,
	TOMA_STATE_OF_A_CONN_BACKOFF,
	MAX_TOMA_STATE_OF_A_CONN
};

static inline bool gidcmp(const union ibv_gid *a, const union ibv_gid *b)
{
	return
			a->global.interface_id == b->global.interface_id &&
			a->global.subnet_prefix == b->global.subnet_prefix;
}

static inline void ipv4_to_gid(__be32 in_addr, union ibv_gid *gid)
{
	u32 in_addr_h = be32toh(in_addr);
	gid->global.subnet_prefix = 0;
	gid->global.interface_id = htobe64(((uint64_t)0xffff << (sizeof(in_addr) << 3)) | (uint64_t)in_addr_h);
}

static inline void ipv6_to_gid(const struct in6_addr *in6_addr, union ibv_gid *gid)
{
	*gid = *(union ibv_gid *)in6_addr;
}

static inline bool is_valid_roce_ipv4(const union ibv_gid *g)
{
	return (!g->global.subnet_prefix &&
			((g->global.interface_id & 0xffffffff) == 0xffff0000));
}

static inline unsigned roce_convert_gid_ip(const union ibv_gid *g)
{
	return g->global.interface_id >> 32;
}

static inline void roce_convert_gid_ip6(const union ibv_gid *g, struct in6_addr *a)
{
	memcpy(a, g, 16);
}

static inline void gid_to_sockaddr(const union ibv_gid *gid, union sockaddr_union *addr_out) {
	if (is_valid_roce_ipv4(gid)) {
		addr_out->v4.sin_family = AF_INET;
		addr_out->v4.sin_addr.s_addr = roce_convert_gid_ip(gid);
	} else {
		addr_out->v6.sin6_family = AF_INET6;
		roce_convert_gid_ip6(gid, &addr_out->v6.sin6_addr);
	}
}

struct nvmeibt_netdev;
int nvmeibt_ib_device_uuid_str_to_raw(union ibv_gid *ibv_gid, const char *device_uuid_str);
const char *netdev_name(struct nvmeibt_netdev *ndev);
struct nvmeibt_netdev_addr *find_netdev_addr_by_gid(union ibv_gid *gid);
void nvmeibt_netstat(void);
int scan_server_netdev(void);
int handle_netlink_event(int sock);

typedef XDLIST_DECLARE(netdev_xdlist, struct nvmeibt_netdev, link) nvmeibt_netdev_q_t;
extern nvmeibt_netdev_q_t nvmeibt_netdev_q;
union netdev_xdlist * nvmeibt_net_get_netdevs(void);

#endif /* ___NVMEIBT_NET_H__ */
