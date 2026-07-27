/*
 *  TCP server
 */
#include <linux/module.h>
#include <linux/kernel.h>

#include "common.h"
#include "common.c"

MODULE_AUTHOR("Excelero");
MODULE_DESCRIPTION("TCP Server Test");
MODULE_LICENSE("Dual BSD/GPL");


/* depth of pending connections requests queue */
#define NVMETCP_LISTEN_BACKLOG 64
/* NVMesh TCP Server WQ - works added from:
   TBD */
static struct workqueue_struct *swq;

//static struct listen_container {
//};

struct work_struct listen_work;
struct socket *listen_sock;


u32 listen_addr = SRV_ADDR;
u16 listen_port = SRV_PORT;

struct sock_container *single_sc;

static struct work_struct hello_work;
static void hello_work_f(struct work_struct *work)
{
	_I("Hello, I'm PID of swq=%p\n", swq);
}

/* -------------------------------------------------------------------------- */
/* server 																	  */
/* -------------------------------------------------------------------------- */
static int accept_one(struct socket *sock, int *more)
{
	struct sock_container *sc = NULL;
	struct socket *new_sock = NULL;
	unsigned int noio_flag;
	int slen;
	struct sockaddr_in sin;
	int rv = -1;
	FIN;

	*more = 0;
	if (!sock) {
		_E("Invalid listen sock\n");
		goto out;
	}

	if (!(sc = sc_alloc(SC_SRV, swq)))
		goto out;

	/*
	 * sock_create_lite allocates the sock with GFP_KERNEL. We must set
	 * per-process flag PF_MEMALLOC_NOIO so that all allocations done
	 * by this process are done as if GFP_NOIO was specified. So we
	 * are not reentering filesystem while doing memory reclaim.
	 */
	noio_flag = memalloc_noio_save();

	if ((rv = sock_create_lite(sock->sk->sk_family, sock->sk->sk_type,
		sock->sk->sk_protocol, &new_sock))) {
		_E("Fail to create sock (%d)\n", rv);
		goto err_sc;
	}

	new_sock->type = sock->type;
	new_sock->ops = sock->ops;
	/* inet_accept     (from struct proto_ops inet_stream_opsnet @ /ipv4/af_inet.c) -->
	   inet_csk_accept (from struct proto     tcp_prot) */
	if ((rv = sock->ops->accept(sock, new_sock, O_NONBLOCK)) < 0) {
		_E("Fail to accept sock (%d)\n", rv);
		goto err_sock;
	}
	new_sock->sk->sk_allocation = GFP_ATOMIC;

	/* signal caller to do more accept */
	*more = 1;

	if ((rv = sock_set_nodelay(new_sock))) {
		_E("Fail to set TCP_NODELAY (%d)\n", rv);
		goto err_sock;
	}

	slen = sizeof(sin);
	if ((rv = new_sock->ops->getname(new_sock, (struct sockaddr *) &sin,
		&slen, 1)) < 0) {
		_E("Fail to getname (%d)\n", rv);
		goto err_sock;
	}

	_T("New conn from %pI4:%d ...\n",
		&sin.sin_addr.s_addr, ntohs(sin.sin_port));

	sc->sc_sock = new_sock;
	new_sock = NULL;
	sk_callbacks_register(sc->sc_sock->sk, sc);
	//Do we need this:
	//sc_queue_work(sc, &sc->sc_rx_work);
	//???? How do we know that the sk-callbacks where not called +before+ we hijacked them?

	//o2net_initialize_handshake();
	//o2net_sendpage(sc, o2net_hand, sizeof(*o2net_hand));

	sc_list_add(sc);
	single_sc = sc;
	_T("Accepted new conn: sc=%p, sk=%p\n", sc, sc->sc_sock->sk);
	rv = 0;
	goto done;

err_sock:
	_E("TBD - orderly shutdown sock - may be still getting events\n");
	sock_release(new_sock);
err_sc:
	sc_free(sc);
	sc = NULL;

done:
	memalloc_noio_restore(noio_flag);
out:
	FOUT;
	return rv;
}

/*
 * This function is invoked in response to one or more
 * pending accepts at softIRQ level. We must drain the
 * entire que before returning.
 */
static void accept_many_work(struct work_struct *work)
{
	struct socket *sock = listen_sock;
	int	more;
	int	err;
	int i = 0;
	IFIN;

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

	_T("listen sk %p: Start listen-work to accept new conn(s)\n", sock->sk);

	for (;;i++) {
		_T("accept conn #%d\n", i);
		if (single_sc) {
			_I("Already have client connection - Ignore\n");
			//BUG: if we 'return' here, we crash on rmmod !!!
		}

		err = accept_one(sock, &more);
		if (!more)
			break;
		cond_resched();
	}

	IFOUT;
}

#if KER_VER_GE_3_15
void listen_data_ready(struct sock *sk)
#else
void listen_data_ready(struct sock *sk, int bytes)
#endif
{
	//void (*ready)(struct sock *sk);
	sk_data_ready_t	ready;
	IFIN;

	_T("listen (?) sk %p: data-ready callback\n", sk);
	read_lock(&sk->sk_callback_lock);
	ready = sk->sk_user_data;
	if (ready == NULL) { /* check for teardown race */
		_T("listen sk %p: data-ready during teardown\n", sk);
		ready = sk->sk_data_ready;
		goto out;
	}

	/* This callback may called twice when a new connection
	 * is  being established as a child socket inherits everything
	 * from a parent LISTEN socket, including the data_ready cb of
	 * the parent. This leads to a hazard. In accept_one()
	 * we are still initializing the child socket but have not
	 * changed the inherited data_ready callback yet when
	 * data starts arriving.
	 * We avoid this hazard by checking the state.
	 * For the listening socket,  the state will be TCP_LISTEN; for the new
	 * socket, will be  TCP_ESTABLISHED. Also, in this case,
	 * sk->sk_user_data is not a valid function pointer.
	 */

	if (sk->sk_state == TCP_LISTEN) {
		_T("Add listen-work to accept new conn(s)\n"); //accept_many_work
		if (!queue_work(swq, &listen_work))
			_E("Fail to add work, already on a queue!\n");
	} else {
		_T("new sk %p inherited data-ready callback called (state %d),"
		   "dont call orig sk's cb\n", sk, sk->sk_state);
		ready = NULL;
	}

out:
	read_unlock(&sk->sk_callback_lock);
	if (ready != NULL)  {
#if KER_VER_GE_3_15
		ready(sk);
#else
		ready(sk, bytes);
#endif
	}

	IFOUT;
}

static void listen_stop(void)
{
	struct sock	*sk;
	FIN;

	_I("SRV: listen stop...\n");

	if (!listen_sock) {
		_E("Invalid listen socket\n");
		goto out;
	}
	if (!swq) {
		_E("Invalid swq\n");
		goto out;
	}

	/* prevent new works on swq
	   by unregistering listener sk's callbacks */
	sk = listen_sock->sk;
	write_lock_bh(&sk->sk_callback_lock);
	_I("Unregister data-ready: '%ps'(%p) to '%ps'(%p)\n",
		sk->sk_data_ready, sk->sk_data_ready,
		sk->sk_user_data, sk->sk_user_data);
	sk->sk_data_ready = sk->sk_user_data;
	sk->sk_user_data = NULL;
	write_unlock_bh(&sk->sk_callback_lock);

	/* remove all accepted sokects (sc) */
	if (single_sc) {
		_T("Add shutdown-work of accepted socket, sc=%p\n", single_sc);
		sc_queue_work(single_sc, &single_sc->sc_shutdown_work);
	}
	else
		_T("No accepted socket\n");


	/* cancel delayed works on swq */
	_E("TBD, cancel delayed works on swq\n");

	/* wait for queued and current works and destroy swq */
	_I("Destroy swq\n");
	destroy_workqueue(swq);
	swq = NULL;

	/* release listen sock  */
	sock_release(listen_sock);
	listen_sock = NULL;

out:
	FOUT;
}
static int listen_sock_open(__be32 addr, __be16 port)
{
	struct socket *sock = NULL;
	struct sockaddr_in sin = {
		.sin_family = PF_INET,
		.sin_addr = { .s_addr = INADDR_ANY}, //htonl(addr) }, //INADDR_ANY /*addr*/ },
		.sin_port = htons(port), //port,
	};
	int rv = -1;
	FIN;

	_T("SRV: Open listen socket...\n");

	/* create socket */
	if ((rv = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock)) < 0) {
		_E("Fail to create socket (%d)\n", rv);
		goto out;
	}

	sock->sk->sk_allocation = GFP_ATOMIC;
	/* register (hijack) sk's callbacks */
	write_lock_bh(&sock->sk->sk_callback_lock);
	_I("Register data-ready: '%ps'(%p) to '%ps'(%p)\n",
		sock->sk->sk_data_ready, sock->sk->sk_data_ready,
		listen_data_ready, listen_data_ready);
	sock->sk->sk_user_data = sock->sk->sk_data_ready;
	sock->sk->sk_data_ready = listen_data_ready;
	write_unlock_bh(&sock->sk->sk_callback_lock);

	/* init stuff for listen-work */
	listen_sock = sock;
	INIT_WORK(&listen_work, accept_many_work);

	/* bind */
	sock->sk->sk_reuse = SK_CAN_REUSE;
	if ((rv = sock->ops->bind(sock, (struct sockaddr *)&sin, sizeof(sin))) < 0) {
		_E("Fail to bind sock (%d)\n", rv);
		goto err;
	}

	/* listen */
	if ((rv = sock->ops->listen(sock, NVMETCP_LISTEN_BACKLOG)) < 0) {
		_E("Fail to listen sock (%d)\n", rv);
		goto err;
	}

	_T("listen sock %p (sk=%p) opened\n", sock, sock->sk);
	rv = 0;
	goto out;

err:
	_E("TBD - orderly shutdown sock - may be still getting events\n");
	sock_release(sock);
	listen_sock = NULL;

out:
	FOUT;
	return rv;
}

static int listen_start(void)
{
	int rv = -1;
	FIN;

	_I("SRV: listen start...\n");

	/* sanity */
	if (swq) {
		_E("Oops, swq already exist\n");
		goto out;
	}
	if (!listen_addr || !listen_port) {
		_E("Invalid listen addr/port\n");
		goto out;
	}

	/* create listen swq (kthread) */
	if (!(swq = create_singlethread_workqueue("nt_swq"))) {
		_E("Fail to create swq\n");
		goto out;
	}
	//DEBUG
	INIT_WORK(&hello_work, hello_work_f);
	queue_work(swq, &hello_work);

	/* create listen socket */
	if (listen_sock_open(listen_addr, listen_port) < 0) {
		goto err_wq;
	}

	rv = 0;
	goto out;

err_wq:
	destroy_workqueue(swq);
	swq = NULL;

out:
	FOUT;
	return rv;
}

int __init tcp_srv_init(void)
{
	int rv = 0;
	FIN;

	_I("TCP server init\n");
	sc_checks();

	listen_data_ready_addr = listen_data_ready;
	if (!(rv = listen_start()))
		_I("Hooray: tcp_srv registered\n");

	FOUT;
	return rv;
}

void __exit tcp_srv_exit(void)
{
	_I("TCP server exit\n");
	listen_stop();
	_I("Hooray: tcp_srv unregistered\n");
}

/* Without these no prints to dmesg from corrsponding funcs */
module_init(tcp_srv_init);
module_exit(tcp_srv_exit);