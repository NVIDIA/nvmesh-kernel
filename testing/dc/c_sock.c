/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "kr_version.h"
#include "kr_incs.h"
#include "c_sock.h"
#include "u.h"
#include "u_sock.h"

#include <linux/sched/mm.h>

struct c_info {
	struct info info;
	__be32 c_ipv4_address;
	struct sockaddr_in c_sin;
	unsigned long last_connect_attempt;
};

static int open_conenct(struct c_info *info)
{
	struct sockaddr_in myaddr = {0, }, remoteaddr = {0, };
	struct socket *sock = NULL;
	struct per_socket *p = NULL;
	unsigned int noio_flag;
	int ret = -1;

	FIN;
	/*
	 * sock_create allocates the sock with GFP_KERNEL. We must set
	 * per-process flag PF_MEMALLOC_NOIO so that all allocations done
	 * by this process are done as if GFP_NOIO was specified. So we
	 * are not reentering filesystem while doing memory reclaim.
	 */
	noio_flag = memalloc_noio_save();
	info->last_connect_attempt = jiffies;
	if (!(p = create_per_socket(&info->info)))
		goto out;
	ret = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret < 0) {
		trace("Error %d while creating client socket\n", ret);
		goto free_p;
	}
	sock->sk->sk_allocation = GFP_ATOMIC;
	myaddr.sin_family = AF_INET;
	myaddr.sin_addr.s_addr = info->c_ipv4_address;
	myaddr.sin_port = htons(0); /* any port */
	info->c_sin = myaddr;

	ret = sock->ops->bind(sock, (struct sockaddr *)&myaddr, sizeof(myaddr));
	if (ret) {
		trace("bind failed with %d at address %pI4\n",
			ret, &info->c_ipv4_address);
		goto free_p;
	}

	ret = set_nodelay(sock);
	if (ret) {
		trace("setting TCP_NODELAY failed with %d\n", ret);
		goto free_p;
	}

	ret = set_usertimeout(sock);
	if (ret) {
		trace("set TCP_USER_TIMEOUT failed with %d\n", ret);
		goto free_p;
	}
	p->sock = sock;
	sock = NULL;
	list_add_tail(&p->link, &info->info.sockets);
	register_callbacks(p, false);

	remoteaddr.sin_family = AF_INET;
	remoteaddr.sin_addr.s_addr = info->info.ipv4_address;
	remoteaddr.sin_port = info->info.ipv4_port;
	p->sin = remoteaddr;

	ret = p->sock->ops->connect(p->sock, (struct sockaddr *)&remoteaddr,
		sizeof(remoteaddr), O_NONBLOCK);
	if (ret == -EINPROGRESS)
		ret = 0;
	goto check_ret;

free_p:
	trace("start handling label free_p:\n");
	if (p) {
		if (p->page)
			__free_page(p->page);
		kfree(p);
		p = NULL;
	}

check_ret:
	if (ret && p) {
		trace("Connect attempt to %pI4:%d failed with errno %d\n",
		&remoteaddr.sin_addr.s_addr, ntohs(remoteaddr.sin_port), ret);
		ensure_shutdown(p, 0);
	}

out:
	if (sock)
		sock_release(sock);

	memalloc_noio_restore(noio_flag);
	FOUT;
	return ret;
}

static int start_connect(struct c_info *info)
{
	int rv;

	FIN;
	rv = start_server_thread(&info->info.wq, "client");
	if (rv < 0) {
		trace("fail to start client thread\n");
		goto out;
	}
	rv = open_conenct(info);
	if (rv < 0) {
		trace("fail to open a conenct socket\n");
		destroy_workqueue(info->info.wq);
		info->info.wq = NULL;
		goto out;
	}

out:
	FOUT;
	return rv;
}

static void stop_connect(struct info *info)
{
	FIN;
	FOUT;
}

static int store_ipv4_address(void *p, char *page, int count)
{
	int i, ret;
	unsigned int octets_c[4];
	unsigned int octets_s[4];
	unsigned int port_s;
	struct c_info *info = container_of(p, struct c_info, info);

	FIN;
	ret = sscanf(page, "%3u.%3u.%3u.%3u,%3u.%3u.%3u.%3u:%5u",
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
		be32_add_cpu(&info->info.ipv4_address, octets_s[i] << (i * 8));
	}
	if (port_s == 0) {
		ret = -EINVAL;
		goto out;
	}
	if (port_s >= (u16)-1) {
		ret = -ERANGE;
		goto out;
	}
	info->info.ipv4_port = htons(port_s);
	trace("server ip:port is %pI4:%u\n",
		&info->info.ipv4_address, ntohs(info->info.ipv4_port));
	start_connect(info);

out:
	FOUT;
	return count;
}

static struct c_info *_info;
int c_in_(void) /* Constructor */
{
	int rv = 0;

	FIN;
	trace("hello from the client\n");
	if ((_info = kzalloc(sizeof(*_info), GFP_KERNEL))) {
		rv = init_info(&_info->info, DC_PROC_C_STR,
			store_ipv4_address, stop_connect) ? 0 : -1;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

void c_out_(void) /* Destructor */
{
	FIN;
	trace("bye from the client\n");
	if (_info) {
		free_info(&_info->info, false);
		kfree(_info);
	}
	FOUT;
}

