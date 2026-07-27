#include "u.h"
#include "u_sock.h"

static void sock_cancel_delayed_work(struct per_socket *sock,
	struct delayed_work *work, const char *str)
{
	if (cancel_delayed_work(work))
		sclog(sock, "delayed work %s canceled\n", str);
}

void reset_idle_timer(struct per_socket *sock)
{
	FIN;
	sock_cancel_delayed_work(sock, &sock->keepalive_work, "keepalive");
	queue_delayed_work(sock->info->wq, &sock->keepalive_work,
		msecs_to_jiffies(sock->info->keepalive_delay_ms));
	sock->tv_timer = ktime_get();
	mod_timer(&sock->idle_timeout,
	       jiffies + msecs_to_jiffies(sock->info->idle_timeout_ms));
	FOUT;
}

static void __attribute__((unused)) postpone_idle(struct per_socket *sock)
{
	FIN;
	/* Only push out an existing timer */
	if (timer_pending(&sock->idle_timeout))
		reset_idle_timer(sock);
	FOUT;
}

/* called when a connect completes and after a sock is accepted. */
static void connect_completed(struct work_struct *work)
{
	struct per_socket *sock = container_of(
		work, struct per_socket, connect_work);

	FIN;
	trace("should send handshake to %pI4:%d\n",
		&sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port));
	FOUT;
}

static int recv_tcp_msg(struct socket *sock, void *data, size_t len)
{
	struct kvec vec = { .iov_len = len, .iov_base = data, };
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT, };
	int rv;

	FIN;
#if defined(KS_IOV_ITER_KVEC) && KS_IOV_ITER_KVEC
	iov_iter_kvec(&msg.msg_iter, READ, &vec, 1, len);
	rv = sock_recvmsg(sock, &msg, msg.msg_flags);
#else
#	if defined(KS_SOCKET_IOV_ITER) && KS_SOCKET_IOV_ITER
	iov_iter_init(&msg->msg_iter, READ, (struct iovec *)&vec, 1, len);
#	else
	msg.msg_iov = (struct iovec *)&vec;
	msg.msg_iovlen = 1;
#	endif
	rv  = sock_recvmsg(sock, &msg, len, msg.msg_flags);
#endif
	FOUT;
	return rv;
}

/* this returns -errno if the header was unknown or too large, etc.
 * after this is called the buffer is reused for the next message */
static int process_message(struct per_socket *sock, struct test_msg *hdr)
{
	FIN;
	msglog(hdr, "processing message\n");
	//postpone_idle(sock);
	FOUT;
	return 0;
}

/* this demuxes the queued rx bytes into header or payload bits and calls
 * handlers as each full message is read off the socket.  it returns -error,
 * == 0 eof, or > 0 for progress made.*/
static int advance_rx(struct per_socket *sock)
{
	struct test_msg *hdr;
	int ret = 0;
	void *data;
	size_t datalen;

	FIN;
	trace("receiving from %pI4:%d\n",
		&sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port));
	sock->advance_start = ktime_get();

	/* do we need more header? */
	if (sock->page_off < sizeof(struct test_msg)) {
		data = page_address(sock->page) + sock->page_off;
		datalen = sizeof(struct test_msg) - sock->page_off;
		ret = recv_tcp_msg(sock->sock, data, datalen);
		if (ret > 0) {
			sock->page_off += ret;
			/* only swab incoming here.. we can
			 * only get here once as we cross from
			 * being under to over */
			if (sock->page_off == sizeof(struct test_msg)) {
				hdr = page_address(sock->page);
				if (be16_to_cpu(hdr->data_len) > EXCELERO_MAX_PAYLOAD_BYTES)
					ret = -EOVERFLOW;
			}
		}
		if (ret <= 0)
			goto out;
	}

	if (sock->page_off < sizeof(struct test_msg)) {
		/* oof, still don't have a header */
		goto out;
	}

	/* this was swabbed above when we first read it */
	hdr = page_address(sock->page);

	msglog(hdr, "at page_off %zu\n", sock->page_off);

	/* do we need more payload? */
	if (sock->page_off - sizeof(struct test_msg) < be16_to_cpu(hdr->data_len)) {
		/* need more payload */
		data = page_address(sock->page) + sock->page_off;
		datalen = (sizeof(struct test_msg) + be16_to_cpu(hdr->data_len)) -
			sock->page_off;
		ret = recv_tcp_msg(sock->sock, data, datalen);
		if (ret > 0)
			sock->page_off += ret;
		if (ret <= 0)
			goto out;
	}

	if (sock->page_off - sizeof(struct test_msg) == be16_to_cpu(hdr->data_len)) {
		/* we can only get here once, the first time we read
		 * the payload.. so set ret to progress if the handler
		 * works out. after calling this the message is toast */
		ret = process_message(sock, hdr);
		if (ret == 0)
			ret = 1;
		sock->page_off = 0;
	}

out:
	sclog(sock, "ret = %d\n", ret);
	sock->advance_stop = ktime_get();
	return ret;
}

/*
 * this is a little helper that is called by callers who have seen a problem
 * with an per_socket and want to detach it from the info if someone already
 * hasn't beat them to it.  if an error is given then the shutdown will
 * be persistent and pending transmits will be canceled.
 */
void ensure_shutdown(struct per_socket *sock, int err)
{
	FIN;
	FOUT;
}

/* this work func is triggerd by data ready.  it reads until it can read no
 * more.  it interprets 0, eof, as fatal.  if data_ready hits while we're doing
 * our work the work struct will be marked and we'll be called again. */
static void rx_until_empty(struct work_struct *work)
{
	struct per_socket *sock = container_of(
		work, struct per_socket, rx_work);
	int ret;

	FIN;
	do {
		ret = advance_rx(sock);
	} while (ret > 0);

	if (ret <= 0 && ret != -EAGAIN) {
		trace("saw error %d from %pI4:%d, closing\n", ret,
			&sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port));
		/* not permanent so read failed handshake can retry */
		ensure_shutdown(sock, 0);
	}
	FOUT;
}

static int unregister_callbacks(struct sock *sk, struct per_socket *sock)
{
	int ret = 0;

	FIN;
	write_lock_bh(&sk->sk_callback_lock);
	if (sk->sk_user_data == sock) {
		ret = 1;
		sk->sk_user_data = NULL;
		sk->sk_data_ready = sock->sk_ready_cb;
		sk->sk_state_change = sock->sk_state_change_cb;
	}
	write_unlock_bh(&sk->sk_callback_lock);
	FOUT;
	return ret;
}

/*
 * This work queue function performs the blocking parts of socket shutdown.  A
 * few paths lead here.
 */
static void shutdown_sock(struct work_struct *work)
{
	struct per_socket *sock = container_of(
		work, struct per_socket, shutdown_work);

	FIN;
	sclog(sock, "shutting down\n");
	/* drop the callbacks ref and call shutdown only once */
	if (sock->sock && unregister_callbacks(sock->sock->sk, sock)) {
		/* we shouldn't flush as we're in the thread, the
		 * races with pending sock work structs are harmless */
		del_timer_sync(&sock->idle_timeout);
		sock_cancel_delayed_work(sock, &sock->keepalive_work, "keep_alive");
		kernel_sock_shutdown(sock->sock, SHUT_RDWR);
	}

	/* not fatal so failed connects before the other guy has our
	 * heartbeat can be retried */
	ensure_shutdown(sock, 0);
	FOUT;
}

/* this is called as a work_struct func. */
static void send_keep_req(struct work_struct *work)
{
	struct per_socket *sock = container_of(
		work, struct per_socket, keepalive_work.work);

	FIN;
	trace("should send keepalive info to %pI4:%d\n",
		&sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port));
	FOUT;
}

#if defined(KS_TIMER_SETUP) && KS_TIMER_SETUP
/* socket shutdown does a del_timer_sync against this as it tears down. */
static void idle_timer(struct timer_list *t)
#else
static void idle_timer(unsigned long data)
#endif
{
#if defined(KS_TIMER_SETUP) && KS_TIMER_SETUP
	struct per_socket *sock = container_of(t, struct per_socket, idle_timeout);
#else
	struct per_socket *sock = (void *)data;
#endif
	unsigned long msecs =
		ktime_to_ms(ktime_get()) - ktime_to_ms(sock->tv_timer);

	FIN;
	trace("onnection to %pI4:%d has been idle for %lu.%lu secs.\n",
		&sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port),
		msecs / 1000, msecs % 1000);

	/* idle timerout happen, don't shutdown the connection, but
	 * make fence decision. Maybe the connection can recover before
	 * the decision is made.
	 */
	reset_idle_timer(sock);
	FOUT;
}

static void kill_socket(struct per_socket *sock)
{
	FIN;
	sclog(sock, "releasing\n");
	trace("sock=%p, sock->sock=%p, sock->page=%p\n",
		sock, sock->sock, sock->page);
	if (sock->sock) {
		sock_release(sock->sock);
		sock->sock = NULL;
	}
	if (sock->page) {
		__free_page(sock->page);
		sock->page = NULL;
	}
	trace("sock=%p, sock->sock=%p, sock->page=%p\n",
		sock, sock->sock, sock->page);
	FOUT;
}

struct per_socket * create_per_socket(struct info *info)
{
	struct per_socket *v = NULL;
	struct page *p = NULL;

	FIN;
	v = kzalloc(sizeof(*v), GFP_KERNEL);
	p = alloc_page(GFP_NOFS);
	if (v == NULL || p == NULL) {
		trace("failed to allocate per_socket object\n");
		goto err;
	}
	v->info = info;
	INIT_WORK(&v->connect_work, connect_completed);
	INIT_WORK(&v->rx_work, rx_until_empty);
	INIT_WORK(&v->shutdown_work, shutdown_sock);
	INIT_DELAYED_WORK(&v->keepalive_work, send_keep_req);
#if defined(KS_TIMER_SETUP) && KS_TIMER_SETUP
	timer_setup(&v->idle_timeout, idle_timer, 0);
#else
	setup_timer(&v->idle_timeout, idle_timer, (unsigned long)v);
#endif
	v->page = p;
	goto out;

err:
	kfree(v);
	v = NULL;
	if (p)
		__free_page(p);

out:
	FOUT;
	return v;
}

struct info * init_info(struct info *info, const char *name,
	int (*start)(void *p, char *page, int count),
	void (*stop)(struct info *))
{
	bool allocated;
	FIN;
	if (!info) {
		allocated = true;
		if (!(info = kzalloc(sizeof(*info), GFP_KERNEL)))
			goto out;
	}
	else
		allocated = false;
	INIT_LIST_HEAD(&info->sockets);
	info->keepalive_delay_ms = EXCELERO_KEEPALIVE_DELAY_MS_DEFAULT;
	info->idle_timeout_ms = EXCELERO_IDLE_TIMEOUT_MS_DEFAULT;
	if (!(info->dir = proc_mkdir(DC_PROC_STR, NULL))) {
		trace("fail to create /proc/%s dir\n", DC_PROC_STR);
		goto err;
	}
	if (!(info->proc_file = dc_proc_create(
		(char *)name, info->dir, start, info))) {
		trace("fail to create /proc/%s/%s entry\n", DC_PROC_STR, name);
		goto err;
	}
	info->start = start;
	info->stop = stop;
	goto out;

err:
	if (info->proc_file)
		dc_proc_remove(info->proc_file);
	if (info->dir)
		remove_proc_entry(DC_PROC_STR, NULL);
	if (allocated)
		kfree(info);
	info = NULL;

out:
	FOUT;
	return info;
}

int set_nodelay(struct socket *sock)
{
	int val = EXCELERO_TCP_NODELAY;
	int rv;

	FIN;
	rv = kernel_setsockopt(
		sock, SOL_TCP, TCP_NODELAY, (void *)&val, sizeof(val));
	FOUT;
	return rv;
}

int set_usertimeout(struct socket *sock)
{
	int val = EXCELERO_TCP_USER_TIMEOUT;
	int rv;

	FIN;
	rv =  kernel_setsockopt(
		sock, SOL_TCP, TCP_USER_TIMEOUT, (void *)&val, sizeof(val));
	FOUT;
	return rv;
}

#define READY_CODE															   \
	struct per_socket *sock;												   \
																			   \
	FIN;																	   \
	read_lock_bh(&sk->sk_callback_lock); 									   \
	sock = sk->sk_user_data;                                                   \
	if (sock) {                                                                \
		trace("data ready from machine at %pI4:%d\n",                          \
			&sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port));            \
		trace("data_ready hit\n");                                             \
		sock->data_ready = ktime_get();                                        \
		queue_work(sock->info->wq, &sock->rx_work);                            \
		ready = (void *)sock->sk_ready_cb;                                     \
	}                                                                          \
	else                                                                       \
		ready = (void *)sk->sk_data_ready;                                     \
	read_unlock_bh(&sk->sk_callback_lock);                                     \
                                                                               \
	CALL_READY;                                                                \
	FOUT

static void __attribute__((unused)) data_ready1(struct sock *sk, int bytes)
{
	void (*ready)(struct sock *sk, int bytes);
#define CALL_READY ready(sk, bytes)
	READY_CODE;
#undef CALL_READY
}

static void __attribute__((unused)) data_ready2(struct sock *sk)
{
	void (*ready)(struct sock *sk);
#define CALL_READY ready(sk)
	READY_CODE;
#undef CALL_READY
}
#undef READY_CODE

/* see register_callbacks() */
void state_change(struct sock *sk)
{
	void (*state_change_p)(struct sock *sk);
	struct per_socket *sock;

	FIN;
	read_lock_bh(&sk->sk_callback_lock);
	sock = sk->sk_user_data;
	if (sock == NULL) {
		state_change_p = sk->sk_state_change;
		goto out;
	}
	trace("state change %d on socket from/to machine at %pI4:%d\n",
		sk->sk_state, &sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port));

	state_change_p = sock->sk_state_change_cb;

	switch(sk->sk_state) {
	/* ignore connecting sockets as they make progress */
	case TCP_SYN_SENT:
	case TCP_SYN_RECV:
		break;
	case TCP_ESTABLISHED:
		queue_work(sock->info->wq, &sock->connect_work);
		break;
	default:
		trace("connection to %pI4:%d shutdown, state %d\n",
			&sock->sin.sin_addr.s_addr, ntohs(sock->sin.sin_port),
			sk->sk_state);
		queue_work(sock->info->wq, &sock->shutdown_work);
		break;
	}

out:
	read_unlock_bh(&sk->sk_callback_lock);
	state_change_p(sk);
}

/*
 * we register callbacks so we can queue work on events before calling
 * the original callbacks.  our callbacks our careful to test user_data
 * to discover when they've reaced with o2net_unregister_callbacks().
 */
void register_callbacks(struct per_socket *sock, bool is_server)
{
	struct sock *sk = sock->sock->sk;

	FIN;
	write_lock_bh(&sk->sk_callback_lock);

	/* accepted sockets inherit the old listen socket data ready */
	if (is_server) {
		sk->sk_data_ready = sock->info->listener.sk_ready_cb;
		sk->sk_user_data = NULL;
	}

	BUG_ON(sk->sk_user_data != NULL);
	sk->sk_user_data = sock;

	sock->sk_ready_cb = sk->sk_data_ready;
	sock->sk_state_change_cb = sk->sk_state_change;
	sk->sk_data_ready = SK_READYCB_HAS_BYTES(data_ready1, data_ready2);
	sk->sk_state_change = state_change;

	mutex_init(&sock->send_lock);

	write_unlock_bh(&sk->sk_callback_lock);
	FOUT;
}

void free_info(struct info *info, bool del_info)
{
	struct per_socket *p;

	FIN;
	if (info->stop)
		info->stop(info);
	list_for_each_entry(p, &info->sockets, link)
		queue_work(info->wq, &p->shutdown_work);
	/* finish all work and tear down the work queue */
	trace("waiting for client thread to exit....\n");
	destroy_workqueue(info->wq);
	info->wq = NULL;
	while ((p = list_first_entry_or_null(
		&info->sockets, struct per_socket, link))) {
		list_del(&p->link);
		kill_socket(p);
		kfree(p);
	}
	dc_proc_remove(info->proc_file);
	proc_remove(info->dir);
	if (del_info)
		kfree(info);
	FOUT;
}

