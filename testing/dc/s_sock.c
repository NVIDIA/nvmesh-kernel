/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "kr_incs.h"
#include "kr_version.h"

#include <linux/uio.h>
#include <linux/socket.h>
#include <linux/timer.h>
#include <linux/sched/mm.h>

#include "s_sock.h"
#include "u.h"
#include "u_sock.h"

static int start_listen(struct info *info);

static void stop_listening(struct info *info)
{
	struct socket *sock = info->listener.sock;

	FIN;
	if (sock == NULL) {
		/* stop the listening socket from generating work */
		write_lock_bh(&sock->sk->sk_callback_lock);
		sock->sk->sk_data_ready = info->listener.sk_ready_cb;
		sock->sk->sk_user_data = NULL;
		write_unlock_bh(&sock->sk->sk_callback_lock);
		sock_release(sock);
	}
	else
		trace("listen socket was not started\n");
	FIN;
}

#define READY_CODE                                                             \
	struct info *info;                                                         \
	FIN;                                                                       \
	read_lock_bh(&sk->sk_callback_lock);                                       \
                                                                               \
	/* This callback may called twice when a new connection                 */ \
	/* is  being established as a child socket inherits everything          */ \
	/* from a parent LISTEN socket, including the data_ready cb of          */ \
	/* the parent. This leads to a hazard. In accept_one()                  */ \
	/* we are still initializing the child socket but have not              */ \
	/* changed the inherited data_ready callback yet when                   */ \
	/* data starts arriving.                                                */ \
	/* We avoid this hazard by checking the state.                          */ \
	/* For the listening socket,  the state will be TCP_LISTEN; for the new */ \
	/* socket, will be  TCP_ESTABLISHED. Also, in this case,                */ \
	/* sk->sk_user_data is not a valid function pointer.                    */ \
                                                                               \
	if (sk->sk_state == TCP_LISTEN) {                                          \
		info = sk->sk_user_data;                                               \
		ready = info->listener.sk_ready_cb;                                    \
		if (ready == NULL) { /* check for teardown race */                     \
			ready = (void *)sk->sk_data_ready;                                 \
			goto out;                                                          \
		}                                                                      \
		queue_work(info->wq, &info->listener.work);                            \
	} else                                                                     \
		ready = NULL;                                                          \
                                                                               \
out:                                                                           \
	read_unlock_bh(&sk->sk_callback_lock);                                     \
	if (ready != NULL) CALL_READY;                                             \
	FOUT

static void __attribute__ ((unused)) listen_data_ready1(
	struct sock *sk, int bytes)
{
	void (*ready)(struct sock *sk, int bytes);
#define CALL_READY ready(sk, bytes)
	READY_CODE;
#undef CALL_READY
}

static void __attribute__ ((unused)) listen_data_ready2(struct sock *sk)
{
	void (*ready)(struct sock *sk);
#define CALL_READY ready(sk)
	READY_CODE;
#undef CALL_READY
}
#undef READY_CODE

static int accept_one(struct info *info, int *more)
{
	struct socket *sock;
	struct sockaddr_in sin;
	struct socket *new_sock = NULL;
	unsigned int noio_flag;
	int ret;
	struct per_socket *p;
	int (*a1)(struct socket *, struct socket *, int) __attribute__ ((unused));
	int (*a2)(struct socket *, struct socket *, int, bool) __attribute__ ((unused));
	int (*b1)(struct socket *, struct sockaddr *, int *, int) __attribute__ ((unused));
	int (*b2)(struct socket *, struct sockaddr *, int) __attribute__ ((unused));
	int slen __attribute__ ((unused));

	FIN;
	/*
	 * sock_create_lite allocates the sock with GFP_KERNEL. We must set
	 * per-process flag PF_MEMALLOC_NOIO so that all allocations done
	 * by this process are done as if GFP_NOIO was specified. So we
	 * are not reentering filesystem while doing memory reclaim.
	 *
	 * We should have use this critical section start and end in NVMesh
	 * when we do not want to ask the file system to release memory
	 */
	noio_flag = memalloc_noio_save();

	BUG_ON(info == NULL || info->listener.sock == NULL);
	sock = info->listener.sock;
	*more = 0;
	ret = sock_create_lite(sock->sk->sk_family, sock->sk->sk_type,
		sock->sk->sk_protocol, &new_sock);
	if (ret) {
		trace("fail to create new socket\n");
		goto out;
	}

	new_sock->type = sock->type;
	new_sock->ops = sock->ops;
	if (__same_type(a1, sock->ops->accept)) {
		a1 = (void *)sock->ops->accept;
		ret = a1(sock, new_sock, O_NONBLOCK);
	}
	else {
		a2 = (void *)sock->ops->accept;
		ret = a2(sock, new_sock, O_NONBLOCK, false);
	}
	if (ret < 0) {
		trace("fail to accept connection\n");
		goto out;
	}

	*more = 1;
	new_sock->sk->sk_allocation = GFP_ATOMIC;

	ret = set_nodelay(new_sock);
	if (ret) {
		trace("setting TCP_NODELAY failed with %d\n", ret);
		goto out;
	}

	ret = set_usertimeout(new_sock);
	if (ret) {
		trace("set TCP_USER_TIMEOUT failed with %d\n", ret);
		goto out;
	}

	if (__same_type(b1, new_sock->ops->getname)) {
		b1 = (void *)new_sock->ops->getname;
		ret = b1(new_sock, (struct sockaddr *)&sin, &slen, 1);
	}
	else {
		b2 = (void *)new_sock->ops->getname;
		ret = b2(new_sock, (struct sockaddr *)&sin, 1);
	}
	if (ret < 0) {
		trace("get connect client failed with %d\n", ret);
		goto out;
	}
	trace("Attempt to connect from machine at %pI4:%d\n",
		&sin.sin_addr.s_addr, ntohs(sin.sin_port));
	if (!(p = create_per_socket(info)))
		goto out;
	else {
		p->sock = new_sock;
		p->sin = sin;
		new_sock = NULL;
	}
	list_add_tail(&p->link, &info->sockets);
	register_callbacks(p, true);
	queue_work(info->wq, &p->rx_work);
	//reset_idle_timer(p);

out:
	if (new_sock)
		sock_release(new_sock);

	memalloc_noio_restore(noio_flag);
	FOUT;
	return ret;
}

/*
 * This function is invoked in response to one or more
 * pending accepts at softIRQ level. We must drain the
 * entire que before returning.
 */
static void accept_many(struct work_struct *work)
{
	struct info *info = container_of(
		container_of(work, struct per_socket, work), struct info, listener);
	int	more;
	int	err;

	FIN;
	/*
	 * It is critical to note that due to interrupt moderation
	 * at the network driver level, we can't assume to get a
	 * softIRQ for every single conn since tcp SYN packets
	 * can arrive back-to-back, and therefore many pending
	 * accepts may result in just 1 softIRQ. If we terminate
	 * the accept_one() loop upon seeing an err, what happens
	 * to the rest of the conns in the queue? If no new SYN
	 * arrives for hours, no softIRQ  will be delivered,
	 * and the connections will just sit in the queue.
	 */

	for (;;) {
		err = accept_one(info, &more);
		if (!more)
			break;
		cond_resched();
	}
	FOUT;
}

static int open_listener(struct info *info)
{
	struct socket *sock = NULL;
	int ret;
	struct sockaddr_in sin = {
		.sin_family = PF_INET,
		.sin_addr = {.s_addr = info->ipv4_address },
		.sin_port = info->ipv4_port,
	};

	FIN;
	ret = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret < 0) {
		trace("Error %d while creating listen socket\n", ret);
		goto out;
	}

	sock->sk->sk_allocation = GFP_ATOMIC;
	write_lock_bh(&sock->sk->sk_callback_lock);
	info->listener.sk_ready_cb = sock->sk->sk_data_ready;
	sock->sk->sk_user_data = info;
	sock->sk->sk_data_ready =
		SK_READYCB_HAS_BYTES(listen_data_ready1, listen_data_ready2);
	write_unlock_bh(&sock->sk->sk_callback_lock);

	info->listener.sock = sock;
	INIT_WORK(&info->listener.work, accept_many);

	sock->sk->sk_reuse = SK_CAN_REUSE;
	ret = sock->ops->bind(sock, (struct sockaddr *)&sin, sizeof(sin));
	if (ret < 0) {
		trace("Error %d while binding socket at %pI4:%u\n",
			ret, &info->ipv4_address, ntohs(info->ipv4_port));
		goto out;
	}

	ret = sock->ops->listen(sock, 64);
	if (ret < 0)
		trace("Error %d while listening on %pI4:%u\n",
			ret, &info->ipv4_address, ntohs(info->ipv4_port));

out:
	if (ret) {
		info->listener.sock = NULL;
		if (sock)
			sock_release(sock);
	}
	FOUT;
	return ret;
}

static int start_listen(struct info *info)
{
	int rv;

	FIN;
	rv = start_server_thread(&info->wq, "server");
	if (rv < 0) {
		trace("fail to start server thread\n");
		goto out;
	}
	rv = open_listener(info);
	if (rv < 0) {
		trace("fail to open a listen socket\n");
		destroy_workqueue(info->wq);
		info->wq = NULL;
		goto out;
	}

out:
	FOUT;
	return rv;
}

static int store_ipv4_address(void *p, char *page, int count)
{
	int i, ret;
	unsigned int octets[4];
	unsigned int port;
	struct info *info = p;

	FIN;
	ret = sscanf(page, "%3u.%3u.%3u.%3u:%5u",
		&octets[3], &octets[2], &octets[1], &octets[0], &port);
	if (ret != 5) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(octets); i++) {
		if (octets[i] > 255) {
			ret = -ERANGE;
			goto out;
		}
		be32_add_cpu(&info->ipv4_address, octets[i] << (i * 8));
	}
	if (port == 0) {
		ret = -EINVAL;
		goto out;
	}
	if (port >= (u16)-1) {
		ret = -ERANGE;
		goto out;
	}
	info->ipv4_port = htons(port);
	trace("server ip:port is %pI4:%u\n",
		&info->ipv4_address, ntohs(info->ipv4_port));
	start_listen(info);

out:
	FOUT;
	return count;
}

static struct info *_info;
int s_in_(void) /* Constructor */
{
	int rv = 0;

	FIN;
	trace("hello from the server\n");
	rv = (_info = init_info(NULL,
		DC_PROC_S_STR, store_ipv4_address, stop_listening)) ? 0 : -1;
	FOUT;
	return rv;
}

void s_out_(void) /* Destructor */
{
	FIN;
	trace("bye from the server\n");
	if (_info)
		free_info(_info, true);
	FOUT;
}

