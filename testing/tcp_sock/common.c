#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/socket.h>
#include <net/sock.h>

#include "common.h"

#if KER_VER_GE_3_15
void listen_data_ready_dummy(struct sock *sk)
#else
void listen_data_ready_dummy(struct sock *sk, int bytes)
#endif
{
	_E("################# srv's listen data-ready not defined!!!!!!!!!!!!!!\n");
}
sk_data_ready_t	listen_data_ready_addr = listen_data_ready_dummy;

static inline const char *sc_type_to_str(enum sc_type type)
{
	switch (type) {
	case SC_CLNT: return "SC_CLNT";
	case SC_SRV	: return "SC_SRV";
	default: return "???";
	}
}

void sc_checks(void)
{
	BUILD_BUG_ON(SC_MSG_MAX_DATA_LEN > PAGE_SIZE - sizeof(struct sc_msg)); /* see sc->sc_page */
}
void sc_put(struct sock_container *sc)
{
	_T("sc %p: put\n", sc);
	//kref_put(&sc->sc_kref, sc_kref_release);
}
void sc_get(struct sock_container *sc)
{
	_T("sc %p: get\n", sc);
	//kref_get(&sc->sc_kref);
	//TBD: once enabled, add sc_put() when finishing any sc-work_f
}

void sc_queue_work(struct sock_container *sc, struct work_struct *work)
{
	sc_get(sc);
	if (!queue_work(sc->wq, work)) {
		_E("Fail to add work, already on a queue!\n");
		sc_put(sc);
	}
}
void sc_cancel_delayed_works(struct sock_container *sc)
{
	FIN;
	_E("TBD\n");
	FOUT;
}

void sc_users_unregister(struct sock_container *sc)
{
	FIN;
	_E("TBD\n");
	FOUT;
}

int __attribute__((unused)) sc_users_register(struct sock_container *sc)
{
	FIN;
	_E("TBD\n");
	FOUT;
	return -1;
}

void sc_list_add(struct sock_container *sc)
{
	FIN;
	_E("TBD\n"); //always on swq or add lock
	FOUT;
}

void sc_list_del(struct sock_container *sc)
{
	FIN;
	_E("TBD\n"); //always on swq or add lock
	FOUT;
}

static void init_msg_hdr(struct sc_msg *msg,
	u32 msg_num, u16 msg_type, u16 data_len)
{
	FIN;
	memset(msg, 0, sizeof(struct sc_msg));
	msg->magic = cpu_to_be32(SC_MSG_MAGIC);
	msg->msg_num = cpu_to_be32(msg_num);
	msg->data_len = cpu_to_be16(data_len);
	msg->msg_type = cpu_to_be16(msg_type);
	FOUT;
}

static int send_tcp_msg(struct socket *sock, struct kvec *vec, size_t veclen,
	size_t tot_len)
{
	struct msghdr msg = {.msg_flags = 0,};
	int rv = -1;
	FIN;

	if (veclen != 2) {
		_E("Currently support 2 veclen=2 only!!!\n");
		goto out;
	}

	rv = kernel_sendmsg(sock, &msg, vec, veclen, tot_len);
	_T("kernel_sendmsg: tot_len=%zu, rv=%d\n", tot_len, rv);
	if (likely(rv == tot_len)) {
		_T("OK\n");
	}
	else if (rv >= 0) {
		_T("ERR - partial\n");
		rv = -EPIPE; //???
	}
	else
		_T("ERR\n");

out:
	FOUT;
	return rv;
}

int sc_send_msg(struct sock_container *sc, void *data, size_t len)
{
	struct sc_msg msg;
	struct kvec vec[2];
	int rv = -1;
	FIN;

	if (!data || !len) {
		_E("Invalid input\n");
		goto out;
	}

#if 0
	if (!(msg = kzalloc(sizeof(struct sc_msg), GFP_ATOMIC))) { //TBD: free-pool
		_E("Fail to alloc msg\n");
		goto out;
	}
#endif
	init_msg_hdr(&msg, sc->send_msg_num, SC_MSG_DATA, len);
	vec[0].iov_len = sizeof(struct sc_msg);
	vec[0].iov_base = &msg;
	vec[1].iov_len = len;
	vec[1].iov_base = data;

	//TBD - send lock
	rv = send_tcp_msg(sc->sc_sock, vec, 2, sizeof(struct sc_msg) + len);
	//TBD - send unlock

	if (!rv)
		sc->send_msg_num++;

out:
	FOUT;
	return rv;
}

void sc_established_work_f(struct work_struct *work)
{
	struct sock_container *sc =
		 container_of(work, struct sock_container, sc_established_work);
	char data[32] = "Hello From Client";
	int rv;
	IFIN;

	if (sc->type == SC_CLNT) {
		_I("CLNT: sleep 1 ms...\n");
		udelay(1000);
		_I("CLNT: sending '%s' ...\n", data);
		rv = sc_send_msg(sc, data, 32);
		_I("CLNT: sent msg, rv=%d\n", rv);
	}

	IFOUT;
}

static int recv_tcp_msg(struct socket *sock, void *data, size_t len)
{
	struct kvec vec = { .iov_len = len, .iov_base = data, };
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT, };
	return kernel_recvmsg(sock, &msg, &vec, 1, len, msg.msg_flags);
}

static int process_recv_hdr(struct sock_container *sc)
{
	struct sc_msg *hdr = page_address(sc->sc_page);
	int magic = be32_to_cpu(hdr->magic);
	int msg_num = be32_to_cpu(hdr->msg_num);
	int msg_type = be16_to_cpu(hdr->msg_type);
	int data_len = be16_to_cpu(hdr->data_len);
	int rv = -1;
	FIN;

	_T("hdr: n=%d, t=%#x, l=%d\n", msg_num, msg_type, data_len);
	print_hex_dump(KERN_INFO, "HDR", DUMP_PREFIX_OFFSET, 16, 1, hdr, sizeof(struct sc_msg), 1);

	if (magic != SC_MSG_MAGIC) {
		_E("Invalid magic %#x vs. %#x\n", magic, SC_MSG_MAGIC);
		goto out;
	}
	if (msg_num != sc->recv_msg_num) {
		_E("Unexpected recv msg_num %d vs %d\n", msg_num, sc->recv_msg_num);
		goto out;
	}
	if (msg_type <= SC_MSG_BASE ||
		msg_type >= SC_MSG_LAST) {
		_E("Invalid msg-type %#x\n", msg_type);
		goto out;
	}
	if (data_len > SC_MSG_MAX_DATA_LEN) {
		_E("Exceeded max data len %#x\n", data_len);
		goto out;
	}

	sc->recv_msg_num++;
	rv = 0;

out:
	FOUT;
	return rv;
}

static int process_recv_payload(struct sock_container *sc)
{
	struct sc_msg *hdr = page_address(sc->sc_page);
	int data_len = be16_to_cpu(hdr->data_len);
	void *data = page_address(sc->sc_page) + sizeof(struct sc_msg);
	int rv = 0;
	FIN;

	print_hex_dump(KERN_INFO, "PYLD", DUMP_PREFIX_OFFSET, 16, 1, data, data_len, 1);

	FOUT;
	return rv;
}

static int recv_chunk(struct sock_container *sc)
{
	void *v_sc_page = page_address(sc->sc_page);
	void *rx_ptr;
	int rx_len;
	int rv = 0;
	FIN;

	if (sc->sc_page_off < sizeof(struct sc_msg)) {
		_T("Recv Header: sc_page_off=%zu\n", sc->sc_page_off);
		rx_ptr = v_sc_page + sc->sc_page_off;
		rx_len = sizeof(struct sc_msg) - sc->sc_page_off;
		rv = recv_tcp_msg(sc->sc_sock, rx_ptr, rx_len);
		if (rv > 0) {
			sc->sc_page_off += rv;
			if (sc->sc_page_off == sizeof(struct sc_msg)) {
				if (!(rv = process_recv_hdr(sc)))
					rv = 1; /* continue rx */
			}
		}
		else
			_E("Fail to recv header (%d bytes)\n", rx_len);
	}
	else {
		struct sc_msg *hdr = v_sc_page;
		int data_len = be16_to_cpu(hdr->data_len);
		int msg_len = sizeof(struct sc_msg) + data_len;

		_T("Recv Payload: sc_page_off=%zu (data-len=%d)\n",
			sc->sc_page_off, data_len);

		/* paranoid */
		if (unlikely(sc->sc_page_off >= msg_len)) {
			_E("Oops, overflow page\n");
			rv = -EOVERFLOW;
			goto out;
		}

		rx_ptr = v_sc_page + sc->sc_page_off;
		rx_len = msg_len - sc->sc_page_off;
		rv = recv_tcp_msg(sc->sc_sock, rx_ptr, rx_len);
		if (rv > 0) {
			sc->sc_page_off += rv;
			if (sc->sc_page_off == msg_len) {
				if (!(rv = process_recv_payload(sc)))
					rv = 1; /* continue rx */
				sc->sc_page_off = 0;
			}
		}
		else
			_E("Fail to recv payload (%d bytes)\n", rx_len);
	}

out:
	FOUT;
	return rv;
}

void sc_rx_work_f(struct work_struct *work)
{
	struct sock_container *sc =
		 container_of(work, struct sock_container, sc_rx_work);
	int i = 0;
	int rv;
	IFIN;
	_T("sk %p: Receiving... (sc=%p, %s)\n",
		sc->sc_sock->sk, sc, sc_type_to_str(sc->type));

	do {
		rv = recv_chunk(sc);
		_T("recv_chunk %d, rv=%d\n", i, rv);
		i++;
	} while (rv > 0);

	if (rv == -EAGAIN) {
		if (i == 0)
			_T("Nothing to read, spourios rx-work/data-ready-event!!!\n");
		else
			_T("No more data\n");
	}
	else {
		if (rv <= 0) {
			_E("Recv error %d, add shutdown work\n", rv);
			if (rv == 0)
				_I("EOF (peer has performed an orderly shutdown)\n");
			sc_queue_work(sc, &sc->sc_shutdown_work);
		}
		else
			_E("Unexpected!!!\n"); //in case recv-loop changes
	}

	IFOUT;
}

void sc_shutdown_work_f(struct work_struct *work)
{
	struct sock_container *sc =
		 container_of(work, struct sock_container, sc_shutdown_work);
	bool first;
	IFIN;
	_T("sk %p: shutdown (sc=%p, %s)\n",
		sc->sc_sock->sk, sc, sc_type_to_str(sc->type));

	/* prevent new works to be added by sk's callbacks */
	first = !sk_callbacks_unregister(sc->sc_sock->sk, sc);

	if (first) {
		/* prevent other refs from external sources */
		sc_users_unregister(sc);
		/* cancel sc's delayed works on swq */
		sc_cancel_delayed_works(sc);
		/* delete from srv sc list */
		sc_list_del(sc);
		/* shutdown socket */
		_T("shutdown sock\n");
		kernel_sock_shutdown(sc->sc_sock, SHUT_RDWR);
	}

	//???? ensure_shutdown(,,);

	if (0 /* last */) {
		_E("TBD: sc free\n"); //Support kref so if last sc-work is added after this ...
		sc_free(sc);
	}

	IFOUT;
}

#if 0
void sc_connect_work_f(struct work_struct *work)
{
	struct sock_container *sc =
		 container_of(work, struct sock_container, sc_connect_work);
	FIN;
	_E("TBD\n");
	FOUT;
}
#endif

#if KER_VER_GE_3_15
void sc_data_ready(struct sock *sk)
#else
void sc_data_ready(struct sock *sk, int bytes)
#endif
{
	//void (*ready)(struct sock *sk);
	sk_data_ready_t	ready;
	IFIN;

	_T("sk %p: data-ready\n", sk);

	read_lock(&sk->sk_callback_lock);
	if (sk->sk_user_data) {
		struct sock_container *sc = sk->sk_user_data;
		_T("Add rx work (sc=%p, %s)\n", sc, sc_type_to_str(sc->type));
		sc_queue_work(sc, &sc->sc_rx_work);
		ready = sc->sc_data_ready;
	} else {
		_T("During teardown, just call orig cb\n");
		ready = sk->sk_data_ready;
	}
	read_unlock(&sk->sk_callback_lock);
	#if KER_VER_GE_3_15
	ready(sk);
#else
	ready(sk, bytes);
#endif

	IFOUT;
}

void sc_state_change(struct sock *sk)
{
	void (*state_change)(struct sock *sk);
	struct sock_container *sc;
	IFIN;

	_T("sk %p: state-change to %s(%d)\n",
		sk, tcp_state_to_str(sk->sk_state), sk->sk_state);

	read_lock(&sk->sk_callback_lock);
	sc = sk->sk_user_data;
	if (sc == NULL) {
		_T("During teardown, just call orig cb\n");
		state_change = sk->sk_state_change;
		goto unlock;
	}

	state_change = sc->sc_state_change;
	_T("sc=%p, %s\n", sc, sc_type_to_str(sc->type));
	switch(sk->sk_state) {
	/* ignore connecting sockets as they make progress */
	case TCP_SYN_SENT:
	case TCP_SYN_RECV:
		break;
	case TCP_ESTABLISHED:
		_T("Conn ESTABLISHED - Add established work\n");
		sc_queue_work(sc, &sc->sc_established_work);
		break;
	default:
		_E("Unhandled state %d, add shutdown work\n", sk->sk_state);
		sc_queue_work(sc, &sc->sc_shutdown_work);
		break;
	}

unlock:
	read_unlock(&sk->sk_callback_lock);
	state_change(sk);

	IFOUT;
}

void print_sk_callbacks(struct sock *sk)
{
	//check they are as expected: 'accepted socket' vs. not
	if (sk) {
		if (sk->sk_data_ready)
			_I("sk_data_ready='%ps'(%p)\n",
				sk->sk_data_ready, sk->sk_data_ready);
		if (sk->sk_state_change)
			_I("sk_state_change='%ps'(%p)\n",
				sk->sk_state_change, sk->sk_state_change);
		if (sk->sk_write_space)
			_I("sk_write_space='%ps'(%p)\n",
				sk->sk_write_space, sk->sk_write_space);
		if (sk->sk_error_report)
			_I("sk_error_report='%ps'(%p)\n",
				sk->sk_error_report, sk->sk_error_report);
	}
}

void sk_callbacks_register(struct sock *sk, struct sock_container *sc)
{
	FIN;

	write_lock_bh(&sk->sk_callback_lock);
	print_sk_callbacks(sk);
	_T("sc %p: register sk's callbacks\n", sc);

	/* accepted sockets inherit the old listen socket data ready */
	//TBD: use sc->type
	if (sk->sk_data_ready == listen_data_ready_addr) {
		_T("Change 'accepted socket' data-ready cb\n");
		sk->sk_data_ready = sk->sk_user_data; /* sk's 'real' orig cb */
		sk->sk_user_data = NULL;
	}
	BUG_ON(sk->sk_user_data != NULL);

	sk->sk_user_data = sc;
	sc->sc_data_ready = sk->sk_data_ready;
	sc->sc_state_change = sk->sk_state_change;
	sk->sk_data_ready = sc_data_ready;
	sk->sk_state_change = sc_state_change;
	print_sk_callbacks(sk);

	write_unlock_bh(&sk->sk_callback_lock);

	FOUT;
}

int sk_callbacks_unregister(struct sock *sk, struct sock_container *sc)
{
	int rv = -1;
	FIN;

	write_lock_bh(&sk->sk_callback_lock);
	print_sk_callbacks(sk);
	if (sk->sk_user_data == sc) {
		_T("sc %p: unregister sk's callbacks\n", sc);
		rv = 0;
		sk->sk_user_data = NULL;
		sk->sk_data_ready = sc->sc_data_ready;
		sk->sk_state_change = sc->sc_state_change;
	}
	else if (!sk->sk_user_data)
		_T("sc %p: already unregistered sk's callbacks\n", sc);
	else
		_E("Conflict sk->sk_user_data=%p vs sc=%p\n", sk->sk_user_data, sc);
	print_sk_callbacks(sk);
	write_unlock_bh(&sk->sk_callback_lock);

	FOUT;
	return rv;
}

void sc_free(struct sock_container *sc)
{
	FIN;
	if (sc) {
		if (sc->sc_page)
			__free_page(sc->sc_page);
		kfree(sc);
	}
	else
		_E("sc NULL\n");
	FOUT;
}

struct sock_container *sc_alloc(enum sc_type type, struct workqueue_struct *wq)
{
	struct sock_container *sc = NULL;
	FIN;

	if (type != SC_CLNT && type != SC_SRV) {
		_E("Invalid sc type %d\n", type);
		goto out;
	}

	if (!(sc = kzalloc(sizeof(*sc), GFP_NOFS)) ||
		!(sc->sc_page = alloc_page(GFP_NOFS))) {
		_E("Fail to alloc sc\n");
		goto err;
	}

	//kref_init(&sc->sc_kref);
	sc->type = type;
	INIT_WORK(&sc->sc_rx_work, sc_rx_work_f);
	INIT_WORK(&sc->sc_shutdown_work, sc_shutdown_work_f);
	INIT_WORK(&sc->sc_established_work, sc_established_work_f);
	sc->wq = wq;
	sc->send_msg_num = SC_MSG_FIRST_NUM;
	sc->recv_msg_num = SC_MSG_FIRST_NUM;

	//TODO:
	//INIT_WORK(&sc->sc_connect_work, o2net_sc_connect_completed);
	//INIT_DELAYED_WORK(&sc->sc_keepalive_work, o2net_sc_send_keep_req);
	goto out;

err:
	sc_free(sc);
	sc = NULL;

out:
	FOUT;
	return sc;
}

int sock_set_nodelay(struct socket *sock)
{
	int rv, val = 1;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	/* ????
	 * Don't use sock_setsockopt() for SOL_TCP.  It doesn't check its level
	 * argument and assumes SOL_SOCKET so, say, your TCP_NODELAY will
	 * silently turn into SO_DEBUG.
	 */
	rv = sock->ops->setsockopt(sock, SOL_TCP, TCP_NODELAY,
				    (char __user *)&val, sizeof(val));

	set_fs(oldfs);
	return rv;
}

int sock_set_usertimeout(struct socket *sock)
{
	int user_timeout = SC_TCP_USER_TIMEOUT;

	return kernel_setsockopt(sock, SOL_TCP, TCP_USER_TIMEOUT,
				(char *)&user_timeout, sizeof(user_timeout));
}

