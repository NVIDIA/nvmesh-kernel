/*
 * This is only a notes file of TCP kernel socket code
 */
#define DISABLE 0
#if DISABLE

/*
 * linux/include/linux/net.h
 */
 //Two ways to create a socket
int __sock_create(struct net *net, int family, int type, int proto,
		  struct socket **res, int kern);
int sock_create(int family, int type, int protocol, struct socket **res);
int sock_create_kern(struct net *net, int family, int type, int proto, struct socket **res);
int sock_create_lite(int family, int type, int proto, struct socket **res);

int kernel_setsockopt(struct socket *sock, int level, int optname, char *optval,
		      unsigned int optlen);
//Usage Example: Turn off Nagle's algorithm
kernel_setsockopt(sock, SOL_TCP, TCP_NODELAY, (char *)&one,
			  sizeof(one));
/**
 *  struct socket - general BSD socket
 *  @state: socket state (%SS_CONNECTED, etc)
 *  @type: socket type (%SOCK_STREAM, etc)
 *  @flags: socket flags (%SOCK_ASYNC_NOSPACE, etc)
 *  @ops: protocol specific socket operations
 *  @file: File back pointer for gc
 *  @sk: internal networking protocol agnostic socket representation
 *  @wq: wait queue for several uses
 */
struct socket {
	socket_state		state;

	kmemcheck_bitfield_begin(type);
	short			type;
	kmemcheck_bitfield_end(type);

	unsigned long		flags;

	struct socket_wq __rcu	*wq;

	struct file		*file;
	struct sock		*sk;  //interface to the network layet (L3)
	const struct proto_ops	*ops;
};

/*
 * linux/include/net/sock.h
 */
struct sock {
	...
	gfp_t			sk_allocation;
	...
	rwlock_t		sk_callback_lock;
	...
	struct socket		*sk_socket;
	...
	void			(*sk_state_change)(struct sock *sk);
	void			(*sk_data_ready)(struct sock *sk);
	void			(*sk_write_space)(struct sock *sk);
	void			(*sk_error_report)(struct sock *sk);
	int			(*sk_backlog_rcv)(struct sock *sk,
						  struct sk_buff *skb);
	void                    (*sk_destruct)(struct sock *sk);

}

/*
 * linux/include/net/inet_sock.h
 */
static inline struct inet_sock *inet_sk(const struct sock *sk)
{
	return (struct inet_sock *)sk; //????
}


/*
 * linux/include/net/tcp_states.h
 * Definitions for the TCP protocol sk_state field.
 */
enum {
	TCP_ESTABLISHED = 1,
	TCP_SYN_SENT,
	TCP_SYN_RECV,
	TCP_FIN_WAIT1,
	TCP_FIN_WAIT2,
	TCP_TIME_WAIT,
	TCP_CLOSE,
	TCP_CLOSE_WAIT,
	TCP_LAST_ACK,
	TCP_LISTEN,
	TCP_CLOSING,	/* Now a valid state */
	TCP_NEW_SYN_RECV,

	TCP_MAX_STATES	/* Leave at the end! */
};

/*
 * linux/net/ipv4/*
 */

/* net/ipv4/tcp_ipv4.c */
struct proto tcp_prot = {
	...
	.backlog_rcv		= tcp_v4_do_rcv, //if (sk->sk_state == TCP_ESTABLISHED) i.e. "Fast path" (???)
										 //		tcp_rcv_established(); return 0;
										 //if (sk->sk_state == TCP_LISTEN)
										 //		tcp_child_process(); goto reset; look like error case...


	...
}
EXPORT_SYMBOL(tcp_prot);

/* net/ipv4/af_inet.c */
static const struct net_proto_family inet_family_ops = { //used as input to sock_register()
	.family = PF_INET,
	.create = inet_create, //used by sock_create() to allocate 'struct sock *sk;' of 'struct socket' ---
						   //it also initializes sll (?) sk_ callbacks either by sock_init_data() or in inet_create() inplace,
						   //with function from linux/net/core/sock.c
	.owner	= THIS_MODULE,
}; //see:
static int __init inet_init(void);

static const struct net_protocol tcp_protocol = {
	.early_demux	=	tcp_v4_early_demux,
	.handler	=	tcp_v4_rcv,
	.err_handler	=	tcp_v4_err,
	.no_policy	=	1,
	.netns_ok	=	1,
	.icmp_strict_tag_validation = 1,
};

/* net/ipv4/tcp_input.c */
//TCP state-machine where state is sock->sk->sk_state
//Called from:
// (*) tcp_v4_rcv() or;
// (*)".backlog_rcv		= tcp_v4_do_rcv," in 'struct proto tcp_prot'
int tcp_v4_do_rcv(struct sock *sk, struct sk_buff *skb)
{
	if (sk->sk_state == TCP_ESTABLISHED) { /* Fast path */
		tcp_rcv_established(sk)    //calls sk->sk_data_ready(sk);
		return 0;
	}

	if (sk->sk_state == TCP_LISTEN) {
		struct sock *nsk = tcp_v4_hnd_req(sk, skb);
		...
		if (nsk != sk) {
			...
			tcp_child_process(sk, nsk, skb);
			...
		}
		return 0;
	}

	tcp_rcv_state_process(sk, skb, tcp_hdr(skb), skb->len)
}

/*
 * linux/net/core/sock.[c,h]
 */
void sock_init_data(struct socket *sock, struct sock *sk);


int __sock_create(struct net *net, int family, int type, int protocol,
			 struct socket **res, int kern) //@linux/linux/socket.c
{
	//Allocate the socket and allow the family to set things up.
	sock = sock_alloc(); //@linux/linux/socket.c

	pf = rcu_dereference(net_families[family]);
	err = pf->create(net, sock, protocol, kern); //inet_create() @linux/net/ipv4/af_inet.c
	{
		struct inet_protosw *answer;

		sock->state = SS_UNCONNECTED;

		/* Look for the requested type/protocol pair. */
		list_for_each_entry_rcu(answer, &inetsw[sock->type], list) { //see inetsw_array[] @linux/net/ipv4/af_inet.c
			...
		}
		sock->ops = answer->ops; 		//'struct proto_ops inet_stream_ops',
		answer_prot = answer->prot; 	//'struct proto tcp_prot' --> will set into sock->sk->sk_prot
		//For example: sock->ops.sendmsg (=inet_sendmsg) calls
		//             sock->sk->sk_prot->sendmsg (=tcp_sendmsg)
		// 			   ??? how come IP layer is filled first???
		//
		//             I think that the first one is called from
		//             * sock_sendmsg or( SYSCALL_DEFINE3(sendmsg... )) for kernel-space or
		//             * sys_sendmsg for userspace



		//Allocate a new inode and socket object.
		//The two are bound together and initialised.
		//Note that we pass answer_prot which sets sk->sk_prot
		sk = sk_alloc(net, PF_INET, GFP_KERNEL, answer_prot, kern); //@linux/net/core/sock.c


		sock_init_data(sock, sk); //@linux/net/core/sock.c
		{
			sk->sk_allocation	=	GFP_KERNEL;
			sk->sk_rcvbuf		=	sysctl_rmem_default;
			sk->sk_sndbuf		=	sysctl_wmem_default;
			sk->sk_state		=	TCP_CLOSE; //??? TCP specific although sock_init_data() is called from unix_create() too (af_unix.c)
			...
			sk_set_socket(sk, sock); //sk->sk_socket = sock;
			...
			sk->sk_state_change	=	sock_def_wakeup;
			sk->sk_data_ready	=	sock_def_readable;
			sk->sk_write_space	=	sock_def_write_space;
			sk->sk_error_report	=	sock_def_error_report;
			...
		}
	}
}

//==============================================================================
//==============================================================================
//==============================================================================

/* Oracle Cluster File System
   linux/fs/ocfs2/cluster/tcp.[c,h] */
o2net_init
o2net_sock_container

/* Reliable Datagram Sockets (RDS)
   net/rds/tcp.h */
/* net/rds/tcp_listen.c  */ rds_tcp_listen_init
/* net/rds/tcp_connect.c */ rds_tcp_conn_connect

/* Distributed Lock Manager
   linux/fs/dlm */

/* iSCSI?
   drivers/scsi/iscsi_tcp.c */






#endif

