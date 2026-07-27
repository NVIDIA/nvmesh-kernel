#ifndef NVMEIBT_IB_COMMON_H_INCLUDED
#define NVMEIBT_IB_COMMON_H_INCLUDED

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#ifdef TOMA_IB_ROCE
#include <infiniband/verbs.h>
#include <infiniband/ib.h>
#include <rdma/rdma_cma.h>
#else
#include "nvmeibt_net.h"
#endif

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_srm.h"
#include "../common/nvmeib_shared.h"
#define MAX_SRQ_SIZE 1024
#define RDMA_CM_SERVICE_PORT 9876
#define RDMA_CM_IB_UD_SERVICE_ID (RDMA_IB_IP_PS_UDP | (RDMA_CM_SERVICE_PORT + 1))
#define RDMA_CM_IB_SERVICE_MASK (RDMA_IB_IP_PS_MASK)
#define RDMA_CM_IB_PKEY (0xffff)
#define MAX_RAFT_CONN_SEND_WR (MAX_REMOTE_GIDS * SRM_MAX_MSG_NUM)
#define RSRM_SWND_SIZE (64) // CPP does not do sizeof()
#define SRM_MAX_MSG_NUM (2 * RSRM_SWND_SIZE)
#define MAX_RAFT_CONN_SEND_WR (MAX_REMOTE_GIDS * SRM_MAX_MSG_NUM)
#define QP_PING_UD_NUM_RETRIES 3

#define TOMA_IPV4_ADDR_LEN sizeof(struct in_addr)
#define TOMA_IPV6_ADDR_LEN sizeof(struct in6_addr)

/* A string to hold the IP address and port from a sockaddr */
#define TOMA_SOCKADDR_STRING_LEN 64
#define TOMA_NODE_STR_LEN 256
#define TOMA_GID_STR_LEN 35

#define TOMA_SOCKET_INET_ADDR(_addr) \
	(((struct sockaddr_in *)(_addr))->sin_addr)
#define TOMA_SOCKET_INET_PORT(_addr) \
	(((struct sockaddr_in *)(_addr))->sin_port)

#define TOMA_SOCKET_INET6_ADDR(_addr) \
	(((struct sockaddr_in6 *)(_addr))->sin6_addr)
#define TOMA_SOCKET_INET6_PORT(_addr) \
	(((struct sockaddr_in6 *)(_addr))->sin6_port)

enum ibt_wr_cmd {
	IBT_WR_RECV = 0,
	IBT_WR_RECV_SRQ,
	IBT_WR_SEND_MSG,
	IBT_WR_SEND_PING,
	IBT_WR_LOCK,
};

typedef enum {
	NVMEIBT_IB_REJECT_START = 128,
	REJECT_OOM = 128,
	REJECT_TRY_AGAIN,
	REJECT_SHUTDOWN,
	REJECT_PASSIVE_SIDE, /* Connector is the passive side (according to the node id) */
	REJECT_DUPLICATE,
	REJECT_UNKNOWN_NODE,
	REJECT_PING_FAILED,
	REJECT_INVALID_CONN_IDX,
	REJECT_RAFT_CONN_NOT_READY,
	REJECT_INVALID_AH,
	REJECT_FAILED_ACCEPT,
	REJECT_GID_REMOVED,
	REJECT_VERSION,
} reject_reason_t;

static inline long round_up_to_page_size(long size, unsigned page_size)
{
	return ((size + (page_size - 1)) / page_size) * page_size;
}

const void *nvmeibt_sockaddr_get_inet_addr(
	const struct sockaddr_storage *addr);

union ping_imm_data {
	struct {
		u32 srm_id_lsb:16;
		u32 ping_id:8;
		u32 retry_count:4;
		u32 rsvd:3;
		u32 resp:1;
	} fields;
	u32 raw;
};

struct local_nic_port_data {
	union ibv_gid hw_gid, sw_gid;
	u8 gid_index;
	uint16_t mtu;
	uint16_t pkey;
	enum nvmeib_rdma_transport transport;
	bool roce_v2;
	bool roce_ipv6;
	bool valid;
	char ndev_name[NVMEIB_IB_DEVICE_NAME_MAX];
};

struct local_nic_data {
	char ibv_devname[NVMEIB_IB_DEVICE_NAME_MAX];
	struct local_nic_port_data ports[256];
};

struct local_nics_data {
	struct local_nic_data nics[1024];
	int n_nics;
	int n_ib_nics;
};

#define IB_GID_STR_SIZE sizeof("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")

static inline void format_gid_raw(uint8_t raw[16], char *buf)
{
	int i, n;

	for (n = 0, i = 0; i < 8; ++i) {
		n += snprintf(buf + n, 6, "%04x", nvmeib_htons(((uint16_t *)raw)[i]));
		if (i < 7)
			buf[n++] = ':';
	}
}

static inline void format_gid(union ibv_gid *gid, char *buf)
{
	format_gid_raw(gid->raw, buf);
}

static inline int ipv6_addr_v4mapped(const struct in6_addr *a)
{
	return IN6_IS_ADDR_V4MAPPED(a);
}

int nvmeibt_ib_common_rdma_gid2ip( struct sockaddr_storage *out,
	union ibv_gid *gid, unsigned short port, int is_ib, uint16_t pkey);
int nvmeibt_ib_common_read_local_nics(struct local_nics_data *lnd);
int nvmeibt_ib_common_ib_is_dev_allowed(
	struct local_nics_data *lnd, char *dev_name);
int nvmeibt_ib_common_device_uuid_str_to_raw(
	union ibv_gid *ibv_gid, const char *device_uuid_str);

#ifndef TOMA_IB_ROCE
#include <linux/types.h>
#include <endian.h>
#include <string.h>

#ifndef AF_IB
#define AF_IB 27
#endif
#ifndef PF_IB
#define PF_IB AF_IB
#endif

struct ib_addr {
	union {
		__u8		uib_addr8[16];
		__be16		uib_addr16[8];
		__be32		uib_addr32[4];
		__be64		uib_addr64[2];
	} ib_u;
#define sib_addr8		ib_u.uib_addr8
#define sib_addr16		ib_u.uib_addr16
#define sib_addr32		ib_u.uib_addr32
#define sib_addr64		ib_u.uib_addr64
#define sib_raw			ib_u.uib_addr8
#define sib_subnet_prefix	ib_u.uib_addr64[0]
#define sib_interface_id	ib_u.uib_addr64[1]
};

static inline int ib_addr_any(const struct ib_addr *a)
{
	return ((a->sib_addr64[0] | a->sib_addr64[1]) == 0);
}

static inline int ib_addr_loopback(const struct ib_addr *a)
{
	return ((a->sib_addr32[0] | a->sib_addr32[1] |
		 a->sib_addr32[2] | (a->sib_addr32[3] ^ htobe32(1))) == 0);
}

static inline void ib_addr_set(struct ib_addr *addr,
			       __be32 w1, __be32 w2, __be32 w3, __be32 w4)
{
	addr->sib_addr32[0] = w1;
	addr->sib_addr32[1] = w2;
	addr->sib_addr32[2] = w3;
	addr->sib_addr32[3] = w4;
}

static inline int ib_addr_cmp(const struct ib_addr *a1, const struct ib_addr *a2)
{
	return memcmp(a1, a2, sizeof(struct ib_addr));
}

struct sockaddr_ib {
	unsigned short int	sib_family;	/* AF_IB */
	__be16			sib_pkey;
	__be32			sib_flowinfo;
	struct ib_addr		sib_addr;
	__be64			sib_sid;
	__be64			sib_sid_mask;
	__u64			sib_scope_id;
};

#endif

#endif

