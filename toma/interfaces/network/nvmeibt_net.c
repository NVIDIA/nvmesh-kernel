
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>


#include <linux/types.h>
#include <linux/rtnetlink.h>

#include <arpa/inet.h>

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_net.h"

nvmeibt_netdev_q_t nvmeibt_netdev_q;

const char *netdev_name(struct nvmeibt_netdev *ndev)
{
	return ndev->name;
}

int nvmeibt_ib_device_uuid_str_to_raw(union ibv_gid *ibv_gid, const char *device_uuid_str)
{
	int i, rv = -1;
	char gid[3];

	//FIN;
	if (strstr(device_uuid_str, ":") != NULL) {
		/* String is formatted, we can use IPv6 routines to decode it */
		if (inet_pton(AF_INET6, device_uuid_str, (struct in6_addr*)ibv_gid) <= 0) {
			N_Tf(trace_net_nvmeibt_ib_device_uuid_str_to_raw, "Bad device_uuid_str '@DEVICE_UUID_STR'", device_uuid_str);
        } else {
			rv = 0;
        }

		goto out;
	}

	if (strlen(device_uuid_str) != 32) {
		N_Tf(trace_1_net_nvmeibt_ib_device_uuid_str_to_raw, "Bad device_uuid_str '@DEVICE_UUID_STR'", device_uuid_str);
		goto out;
	}
	gid[2] = '\0';
	for (i = 0; i < 16; ++i) {
		memcpy(gid, device_uuid_str + i * 2, 2);
		ibv_gid->raw[i] = strtoul(gid, NULL, 16);
	}
	rv = 0;

out:
	//FOUT;
	return rv;
}

union netdev_xdlist * nvmeibt_net_get_netdevs(void) {
	return &nvmeibt_netdev_q;
}

struct nvmeibt_netdev_addr *find_netdev_addr_by_gid(union ibv_gid *gid)
{
	struct nvmeibt_netdev *netdev;
	struct nvmeibt_netdev_addr *netdev_addr, *ret = NULL;
	union sockaddr_union addr_cmp = {};

	gid_to_sockaddr(gid, &addr_cmp);

	XDLIST_FOREACH(netdev, &nvmeibt_netdev_q) {
		XDLIST_FOREACH(netdev_addr, &netdev->addr_list) {
			N_Tf(debug_find_netdev_addr_by_gid, 
			     "comparing addr @IPV4_STR (family @IFA_FAMILY) of netdev=@NETDEV with @GUID_RAW (family @IFA_FAMILY)",
			     netdev_addr->addr_str, netdev_addr->addr.raw.sa_family, netdev->name, gid, addr_cmp.raw.sa_family);
			if (sockaddr_addr_equal(&netdev_addr->addr, &addr_cmp)) {
				ret = netdev_addr;
				N_Tf(iomju87, "netdev addr found @IPV4_STR (family @IFA_FAMILY) of netdev=@NETDEV",
					 netdev_addr->addr_str, netdev_addr->addr.raw.sa_family, netdev->name);
				goto out;
			}
		}
	}

out:
	return ret;
}

static struct nvmeibt_netdev *__fetch_netdev(int ifi_index)
{
	struct nvmeibt_netdev *netdev = NULL, *ptr;

	XDLIST_FOREACH(ptr, &nvmeibt_netdev_q) {
		if (ptr->prop.index == (u16)ifi_index) {
			netdev = ptr;
			break;
		}
	}
	return netdev;
}

static struct nvmeibt_netdev *__add_netdev(int ifi_index)
{
	struct nvmeibt_netdev *netdev =
		NNVMEIBT_TOMA_CALLOC(trace_net_add_netdev, 1, sizeof(*netdev));
	netdev->prop.index = ifi_index;
	XDLIST_HEAD_INIT(&netdev->addr_list);
	XDLIST_ADD_TAIL(&nvmeibt_netdev_q, netdev);
	return netdev;
}


/* RFC 2863 operational status from include/uapi/linux/if.h */
static const char *if_operstate_name[] = {
	"IF_OPER_UNKNOWN",
	"IF_OPER_NOTPRESENT",
	"IF_OPER_DOWN",
	"IF_OPER_LOWERLAYERDOWN",
	"IF_OPER_TESTING",
	"IF_OPER_DORMANT",
	"IF_OPER_UP",
};

void nvmeibt_netstat(void)
{
	struct nvmeibt_netdev *ndev;
	struct nvmeibt_netdev_addr *ndev_addr;
	char inet_str[INET6_ADDRSTRLEN];

	XDLIST_FOREACH(ndev, &nvmeibt_netdev_q) {
		int addr_idx = 0;
		XDLIST_FOREACH(ndev_addr, &ndev->addr_list) {
			sockaddr_ntop(&ndev_addr->addr, inet_str, INET6_ADDRSTRLEN);
			N_Tf(trace_net_nvmeibt_netstat, 
				"netdev=@NETDEV index=@INDEX mtu=@MTU addr[@INDEX]=@ADDR_STR carrier=@CARRIER_INT oper_state=@OPER_STATE", 
				ndev->name, ndev->prop.index, ndev->prop.mtu, addr_idx, inet_str, ndev->prop.carrier, 
				if_operstate_name[ndev->prop.oper_state]);
			addr_idx++;
		}
	}
}

static inline struct nvmeibt_netdev *fetch_add_netdev(int ifi_index)
{
	return __fetch_netdev(ifi_index) ? : __add_netdev(ifi_index);
}

#define MAX_NETLINK_RECV (16*1024)
#define FETCH_RTA_DATA(type, val, rtattr) val = *(type*)RTA_DATA(rtattr)

static int netlink_comm(int sock, const void* request, size_t msg_size, int flags,
						 void (*cb)(struct nlmsghdr *))
{
	int rv = -1, len,  err;
	struct nlmsghdr *nlmsg;
	u8 *nl_recv_buf = NNVMEIBT_TOMA_CALLOC(trace_net_netlink_comm, 1, MAX_NETLINK_RECV);
	bool done = false;

	if (request && msg_size) {
		rv = send(sock, request, msg_size, 0);
		if (rv != (typeof(rv))msg_size) {
			err = errno;
			N_Ef(error_net_netlink_comm, "netlink send failure size=@SIZE_LONG rv=@RV err=@ERR",
				msg_size, rv, err);
			goto out;
		}
	}

	do {
		len = recv(sock, nl_recv_buf, MAX_NETLINK_RECV, flags);
		if (len < 0) {
			err = errno;
			N_Tf(info_net_netlink_comm, "netlink recv failure err=@ERR", err);
			rv = err;
			goto out;
		}

		if (!cb) {
			rv = 0;
			goto out;
		}

		for (nlmsg = (typeof(nlmsg))nl_recv_buf; NLMSG_OK(nlmsg, ((unsigned)len)); nlmsg = NLMSG_NEXT(nlmsg, len)) {
			if (nlmsg->nlmsg_type == NLMSG_DONE) {
				N_Df(debug_net_netlink_comm, "nlmsg done");
				rv = 0;
				goto out;
			} else if (nlmsg->nlmsg_type == NLMSG_ERROR) {
				N_Ef(error_net_netlink_comm_2, "nlmsg error");
				rv = 0;
				goto out;
			}

			cb(nlmsg);
		}
	} while (!done);
	rv = 0;

out:
	NNVMEIBT_TOMA_FREE(h76t5rc, nl_recv_buf);
	return rv;
}

static void read_if_cb(struct nlmsghdr *nlmsg)
{
	int attlen = IFLA_PAYLOAD(nlmsg);
	struct ifinfomsg *ifinf = NLMSG_DATA(nlmsg);
	struct rtattr *rtattr = IFLA_RTA(ifinf);
	struct nvmeibt_netdev *netdev = fetch_add_netdev(ifinf->ifi_index);

	while (RTA_OK(rtattr, attlen)) {
		if (0)
			N_Df(trace_net_read_if_cb, "ifla type @RTA_TYPE", rtattr->rta_type);
		switch (rtattr->rta_type) {
		case IFLA_IFNAME:
			nvmeibt_strlcpy(netdev->name, (char *)RTA_DATA(rtattr), sizeof(netdev->name));
			break;
		case IFLA_MTU:
			FETCH_RTA_DATA(u16, netdev->prop.mtu, rtattr);
			break;
		case IFLA_CARRIER:
			FETCH_RTA_DATA(u8, netdev->prop.carrier, rtattr);
			break;
		case IFLA_OPERSTATE:
			FETCH_RTA_DATA(u8, netdev->prop.oper_state, rtattr);
			break;
		default: break;
		}
		rtattr = RTA_NEXT(rtattr, attlen);
	}
}

static void read_addr_cb(struct nlmsghdr *nlmsg)
{
	int attlen = IFA_PAYLOAD(nlmsg);
	struct ifaddrmsg *ifaddrmsg = (struct ifaddrmsg *)NLMSG_DATA(nlmsg);
	struct rtattr *rtattr = (struct rtattr *)IFA_RTA(ifaddrmsg);
	struct nvmeibt_netdev *netdev = fetch_add_netdev(ifaddrmsg->ifa_index);
	struct nvmeibt_netdev_addr *netdev_addr;
	union sockaddr_union new_addr = {};

	while (RTA_OK(rtattr, attlen)) {
		if (0)
			N_Df(trace_net_read_addr_cb, "ifa type @RTA_TYPE", rtattr->rta_type);
		switch (rtattr->rta_type) {
		case IFA_ADDRESS:
			new_addr.raw.sa_family = ifaddrmsg->ifa_family;
			if (ifaddrmsg->ifa_family == AF_INET)
				FETCH_RTA_DATA(struct in_addr, new_addr.v4.sin_addr, rtattr);
			else if (ifaddrmsg->ifa_family == AF_INET6)
				FETCH_RTA_DATA(struct in6_addr, new_addr.v6.sin6_addr, rtattr);
			else {
				N_Wf(warn_net_read_addr_cb, "unsupported network family @IFA_FAMILY", ifaddrmsg->ifa_family);
				goto next;
			}
			/* RTM_NEWADDR happens twice for some reason so we need to check if it's already in the list */
			XDLIST_FOREACH(netdev_addr, &netdev->addr_list) {
				if (sockaddr_addr_equal(&netdev_addr->addr, &new_addr)) {
					N_Tf(trace_3_net_read_addr_cb, "address @IPV4_STR (family @IFA_FAMILY) already in list for netdev @NETDEV_NAME",
					     netdev_addr->addr_str, netdev_addr->addr.raw.sa_family, netdev->name);
					goto next;
				}
			}
			if (!(netdev_addr = NNVMEIBT_TOMA_CALLOC(trace_net_read_addr_cb_alloc, 1, sizeof(*netdev_addr))))
				goto next;
			XDLIST_INIT_LINK(&netdev_addr->link, NULL);
			netdev_addr->netdev = netdev;
			netdev_addr->addr = new_addr;
			sockaddr_ntop(&netdev_addr->addr, netdev_addr->addr_str, sizeof(netdev_addr->addr_str) - 1);
			N_Tf(trace_2_net_read_addr_cb, "@STR address @IPV4_STR (family @IFA_FAMILY) for netdev @NETDEV_NAME",
			     nlmsg->nlmsg_type == RTM_GETADDR ? "found" : "new",
			netdev_addr->addr_str, netdev_addr->addr.raw.sa_family, netdev->name);
			XDLIST_ADD_TAIL(&netdev->addr_list, netdev_addr);
			break;
		default: break;
		}
next:
		rtattr = RTA_NEXT(rtattr, attlen);
	}
}

static void del_addr_cb(struct nlmsghdr *nlmsg)
{
	int attlen = IFA_PAYLOAD(nlmsg);
	struct ifaddrmsg *ifaddrmsg = (struct ifaddrmsg *)NLMSG_DATA(nlmsg);
	struct rtattr *rtattr = (struct rtattr *)IFA_RTA(ifaddrmsg);
	struct nvmeibt_netdev *netdev = __fetch_netdev(ifaddrmsg->ifa_index);
	union sockaddr_union del_addr = {};
	struct nvmeibt_netdev_addr *netdev_addr;
	char del_addr_str[INET6_ADDRSTRLEN + 1];

	if (!netdev) {
		N_Tf(trace_net_del_addr_cb, "netdev empty!");
		return;
	}
	while (RTA_OK(rtattr, attlen)) {
		switch (rtattr->rta_type) {
			case IFA_ADDRESS:
				del_addr.raw.sa_family = ifaddrmsg->ifa_family;
				if (ifaddrmsg->ifa_family == AF_INET)
					FETCH_RTA_DATA(struct in_addr, del_addr.v4.sin_addr, rtattr);
				else if (ifaddrmsg->ifa_family == AF_INET6)
					FETCH_RTA_DATA(struct in6_addr, del_addr.v6.sin6_addr, rtattr);
				else {
					N_Wf(warn_net_del_addr_cb, "unsupported network family @IFA_FAMILY", ifaddrmsg->ifa_family);
					goto next;
				}
				XDLIST_FOREACH(netdev_addr, &netdev->addr_list) {
					if (sockaddr_addr_equal(&netdev_addr->addr, &del_addr)) {
						N_Tf(trace_del_read_addr_cb, "deleting address @IPV4_STR (family @IFA_FAMILY) from netdev @NETDEV_NAME",
							netdev_addr->addr_str, netdev_addr->addr.raw.sa_family, netdev->name);
						XDLIST_DEL(&netdev_addr->link);
						if (netdev->del_addr_cb)
							(*netdev->del_addr_cb)(netdev_addr);
						NNVMEIBT_TOMA_FREE(trace_2_net_del_addr_cb, netdev_addr);
						goto next;
					}
				}
				sockaddr_ntop(&del_addr, del_addr_str, sizeof(del_addr_str) - 1);
				N_Tf(trace_2_del_read_addr_cb, "deleted address @IPV4_STR (family @IFA_FAMILY) not found in netdev @NETDEV_NAME",
				     del_addr_str, del_addr.raw.sa_family, netdev->name);
				break;
			default: 
				break;
		}
next:
		rtattr = RTA_NEXT(rtattr, attlen);
	}
}

static int read_interfaces(int sock)
{
	struct {
		struct nlmsghdr nlhdr;
		struct ifinfomsg infomsg;
	} nlmsg;

	memset(&nlmsg, 0, sizeof(nlmsg));
	nlmsg.nlhdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlmsg.nlhdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
	nlmsg.nlhdr.nlmsg_type = RTM_GETLINK;
	nlmsg.infomsg.ifi_family = AF_UNSPEC;

	return netlink_comm(sock, &nlmsg, nlmsg.nlhdr.nlmsg_len, 0, read_if_cb);
}

static int read_inet_addrs(int sock, int af)
{
	struct {
		struct nlmsghdr nlhdr;
		struct ifaddrmsg addrmsg;
	} msg;

	memset(&msg, 0, sizeof(msg));
	msg.nlhdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	msg.nlhdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
	msg.nlhdr.nlmsg_type = RTM_GETADDR;
	msg.addrmsg.ifa_family = af;

	return netlink_comm(sock, &msg, msg.nlhdr.nlmsg_len, 0, read_addr_cb);
}

static void nl_listner_cb(struct nlmsghdr *nlhdr)
{
	switch(nlhdr->nlmsg_type) {
	case RTM_NEWLINK:
		read_if_cb(nlhdr);
		break;

	case RTM_NEWADDR:
		read_addr_cb(nlhdr);
		break;

	case RTM_DELADDR:
		del_addr_cb(nlhdr);
		break;
	}
}

int handle_netlink_event(int sock)
{
	return netlink_comm(sock, NULL, 0, MSG_DONTWAIT, nl_listner_cb);
}

int scan_server_netdev(void)
{
	int rv, nl_sock;
	struct sockaddr_nl sock_addr;

	XDLIST_HEAD_INIT(&nvmeibt_netdev_q);

	nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (nl_sock < 0) {
		int err = errno;
		N_Ef(error_net_scan_server_netdev, "cannot opt netlink socket err=@ERR", err);
		goto out;
	}

	bzero(&sock_addr, sizeof (sock_addr));
	sock_addr.nl_family = AF_NETLINK;
	sock_addr.nl_pid = getpid();
	sock_addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
	rv = bind(nl_sock, (struct sockaddr *)&sock_addr, sizeof (sock_addr));
	if (rv < 0) {
		int err = errno;
		N_Ef(error_1_net_scan_server_netdev, "netlink bind failure err=@ERR", err);
		goto err;
	}

	// flush pending netlink messages
	// netlink_comm(nl_sock, NULL, 0, MSG_DONTWAIT, NULL);
	if (read_inet_addrs(nl_sock, AF_INET)) goto err; // order is an issue here ! ???
	if (read_inet_addrs(nl_sock, AF_INET6)) goto err; // order is an issue here ! ???
	if (read_interfaces(nl_sock)) goto err;

	nvmeibt_netstat();

out:
	return nl_sock;

err:
	close(nl_sock);
	return -1;
}
