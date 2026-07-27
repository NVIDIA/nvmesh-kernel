#ifndef U_SOCK_H_INCLUDED
#define U_SOCK_H_INCLUDED

#include "kr_version.h"
#include "kr_incs.h"
#include <net/sock.h>
#include <uapi/linux/tcp.h>

#define SK_READYCB_HAS_BYTES(a, b) (__same_type((( \
	struct sock *)0)->sk_data_ready, void (*)(struct sock *, int)) ? \
	(void *)a : (void *)b)

#define EXCELERO_KEEPALIVE_DELAY_MS_DEFAULT 2000
#define EXCELERO_IDLE_TIMEOUT_MS_DEFAULT 30000

/*
	TCP_NODELAY
              If set, disable the Nagle algorithm.  This means that segments
              are always sent as soon as possible, even if there is only a
              small amount of data.  When not set, data is buffered until
              there is a sufficient amount to send out, thereby avoiding the
              frequent sending of small packets, which results in poor
              utilization of the network.  This option is overridden by
              TCP_CORK; however, setting this option forces an explicit
              flush of pending output, even if TCP_CORK is currently set.
*/
#define EXCELERO_TCP_NODELAY 1

/*
	TCP_USER_TIMEOUT (since Linux 2.6.37)
              This option takes an unsigned int as an argument.  When the
              value is greater than 0, it specifies the maximum amount of
              time in milliseconds that transmitted data may remain
              unacknowledged before TCP will forcibly close the
              corresponding connection and return ETIMEDOUT to the
              application.  If the option value is specified as 0, TCP will
              use the system default.

              Increasing user timeouts allows a TCP connection to survive
              extended periods without end-to-end connectivity.  Decreasing
              user timeouts allows applications to "fail fast", if so
              desired.  Otherwise, failure may take up to 20 minutes with
              the current system defaults in a normal WAN environment.

              This option can be set during any state of a TCP connection,
              but is effective only during the synchronized states of a
              connection (ESTABLISHED, FIN-WAIT-1, FIN-WAIT-2, CLOSE-WAIT,
              CLOSING, and LAST-ACK).  Moreover, when used with the TCP
              keepalive (SO_KEEPALIVE) option, TCP_USER_TIMEOUT will
              override keepalive to determine when to close a connection due
              to keepalive failure.

              The option has no effect on when TCP retransmits a packet, nor
              when a keepalive probe is sent.

              This option, like many others, will be inherited by the socket
              returned by accept(2), if it was set on the listening socket.

              Further details on the user timeout feature can be found in
              RFC 793 and RFC 5482 ("TCP User Timeout Option").
*/
#define EXCELERO_TCP_USER_TIMEOUT 0x7fffffff

#define EXCELERO_MAX_PAYLOAD_BYTES (4096 - sizeof(struct test_msg))

struct test_msg {
	__be16 magic;
	__be16 data_len;
	__be16 msg_type;
	__be16 pad1;
	__be32 pad2;
	__u8  buf[0];
};

/*
 * In the following two log macros, the whitespace after the ',' just
 * before ##args is intentional. Otherwise, gcc 2.95 will eat the
 * previous token if args expands to nothing.
 */
#define msglog(hdr, fmt, args...) do { \
	typeof(hdr) __hdr = (hdr); \
	trace("[mag %u len %u type %u] " fmt, \
	    be16_to_cpu(__hdr->magic), be16_to_cpu(__hdr->data_len), \
	    be16_to_cpu(__hdr->msg_type),  ##args); \
} while (0)

#define sclog(sock, fmt, args...) do { \
	typeof(sock) __sc = (sock); \
	trace("[per_socket %p sock %p page %p pg_off %zu] " fmt, __sc, \
		__sc->sock,	__sc->page, __sc->page_off , ##args); \
} while (0)

struct info;
struct per_socket {
	struct info *info;
	struct socket *sock;
	struct sockaddr_in sin;
	struct work_struct work;
	/* rx and connect work are generated from socket callbacks.
	 * shutdown removes the callbacks and then flushes the work queue */
	struct work_struct rx_work;
	struct work_struct connect_work;
	struct work_struct shutdown_work;
	struct delayed_work keepalive_work;
	struct timer_list idle_timeout;
	void *sk_ready_cb;
	void *sk_state_change_cb;
	struct mutex send_lock;
	struct page *page;
	size_t page_off;
	ktime_t data_ready;
	ktime_t advance_start;
	ktime_t advance_stop;
	ktime_t tv_timer;
	struct list_head link;
};

struct info {
	__be32 ipv4_address;
	__be16 ipv4_port;
	struct workqueue_struct *wq;
	struct per_socket listener;
	unsigned int keepalive_delay_ms;
	unsigned int idle_timeout_ms;
	struct proc_dir_entry *dir;
	void * proc_file;
	int (*start)(void *p, char *page, int count);
	void (*stop)(struct info *info);
	struct list_head sockets;
};

struct info * init_info(struct info *info, const char *name,
	int (*start)(void *p, char *page, int count),
	void (*stop)(struct info *));
void free_info(struct info *info, bool del_info);
int set_nodelay(struct socket *sock);
int set_usertimeout(struct socket *sock);
void register_callbacks(struct per_socket *sock, bool is_server);
struct per_socket * create_per_socket(struct info *info);
void reset_idle_timer(struct per_socket *sock);
void ensure_shutdown(struct per_socket *sock, int err);

#endif

