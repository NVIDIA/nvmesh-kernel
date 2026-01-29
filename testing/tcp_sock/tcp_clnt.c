/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 *  TCP client
 */
#include <linux/module.h>
#include <linux/kernel.h>

#include "common.h"
#include "common.c"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("TCP Client Test");
MODULE_LICENSE("GPL and additional rights");

static struct workqueue_struct *cwq;

static struct sock_container *sc;
static struct socket *sock;

static struct work_struct hello_work;
static void hello_work_f(struct work_struct *work)
{
	_I("Hello, I'm PID of cwq=%p\n", cwq);
}

static void conn_stop(void)
{
	FIN;

	if (sc) {
		_T("Add shutdown-work of client socket, sc=%p\n", sc);
		sc_queue_work(sc, &sc->sc_shutdown_work);
	}
	else
		_T("No client socket\n");

	FOUT;
}

static int conn_start(void)
{
	unsigned int noio_flag;
	//struct sockaddr_in myaddr = {0, }, remoteaddr = {0, };
	struct sockaddr_in remoteaddr = {0, };
	int rv = -1;
	FIN;

	if (!(sc = sc_alloc(SC_CLNT, cwq)))
		goto out;

	/*
	 * sock_create allocates the sock with GFP_KERNEL. We must set
	 * per-process flag PF_MEMALLOC_NOIO so that all allocations done
	 * by this process are done as if GFP_NOIO was specified. So we
	 * are not reentering filesystem while doing memory reclaim.
	 */
	noio_flag = memalloc_noio_save();

	/* create socket */
	if ((rv = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock)) < 0) {
		_E("Fail to create socket (%d)\n", rv);
		goto err_sc;
	}
	sc->sc_sock = sock;

	sock->sk->sk_allocation = GFP_ATOMIC;

//	myaddr.sin_family = AF_INET;
//	myaddr.sin_addr.s_addr = ???;
//	myaddr.sin_port = htons(0); /* any port */
//	if ((rv = sock->ops->bind(sock, (struct sockaddr *)&myaddr,
//		sizeof(myaddr))) {
//		_E("Fail to bind socket (%d)\n", rv);
//		goto err_sc;
//	}

	if ((rv = sock_set_nodelay(sc->sc_sock))) {
		_E("Fail to set TCP_NODELAY (%d)\n", rv);
		goto err_sock;
	}

	if ((rv = sock_set_usertimeout(sc->sc_sock))) {
		_E("Fail to set TCP_USER_TIMEOUT (%d)\n", rv);
		goto err_sock;
	}

	sk_callbacks_register(sc->sc_sock->sk, sc);

	remoteaddr.sin_family = AF_INET;
	remoteaddr.sin_addr.s_addr = htonl(SRV_ADDR);
	remoteaddr.sin_port = htons(SRV_PORT);

	_T("Connecting...\n");
	rv = sc->sc_sock->ops->connect(sc->sc_sock,
		(struct sockaddr *)&remoteaddr, sizeof(remoteaddr), O_NONBLOCK);
	if (rv == 0) {
		_T("Connect OK\n");
	}
	else if (rv == -EINPROGRESS) {
		_T("Connect in progress\n");
		rv = 0;
	}
	else {
		_T("Fail to connect (%d)\n", rv);
		goto err_sock;
	}

	rv = 0;
	goto done;

err_sock:
	_E("TBD - orderly shutdown sock - may be still getting events\n");
	sock_release(sc->sc_sock);

err_sc:
	sc_free(sc);
	sc = NULL;

done:
	memalloc_noio_restore(noio_flag);
out:
	FOUT;
	return rv;
}

int __init tcp_clnt_init(void)
{
	int rv = 0;
	FIN;

	_I("TCP client init\n");

	/* create listen cwq (kthread) */
	if (!(cwq = create_singlethread_workqueue("nt_cwq"))) {
		_E("Fail to create cwq\n");
		goto out;
	}
	//DEBUG
	INIT_WORK(&hello_work, hello_work_f);
	queue_work(cwq, &hello_work);

	if ((rv = conn_start()) < 0) {
		_E("Fail to start connection\n");
		goto err_wq;
	}

	_I("Hooray: tcp_clnt registered\n");
	goto out;

err_wq:
	destroy_workqueue(cwq);
	cwq = NULL;

out:
	FOUT;
	return rv;
}

void __exit tcp_clnt_exit(void)
{
	_I("TCP client exit\n");
	sc_checks();

	conn_stop();
	_I("Destroy cwq\n");
	destroy_workqueue(cwq);
	cwq = NULL;
	_I("Hooray: tcp_clnt unregistered\n");
}

/* Without these no prints to dmesg from corrsponding funcs */
module_init(tcp_clnt_init);
module_exit(tcp_clnt_exit);
