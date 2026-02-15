#ifndef NVMEIBT_TOMA_IN_SANDBOX_H
#define NVMEIBT_TOMA_IN_SANDBOX_H

/* First injection point which should be included by all Toma simulators
   This file replaces Toma's interfaces with other components, allowing it to
   run in a sandbox. Used for unitesting */

/*********************************** system Calls *****************************/
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>

#undef TOMA_ROOT_DIR
#define TOMA_ROOT_DIR "_root/"	// Toma is running in sandbox (current directory in file system)

#define _SYS_SOCKET_H 		//	#include <sys/socket.h>
enum { PF_UNIX = 1, AF_UNIX = 1, AF_INET	= 2, PF_NETLINK = 16, AF_NETLINK = 16, MSG_PEEK = 0x02, MSG_DONTWAIT = 0x40, MSG_MORE = 0x8000,
		SOL_SOCKET = 1, SO_REUSEADDR = 2,
};
enum __socket_type { SOCK_STREAM = 1, SOCK_DGRAM = 2, SOCK_RAW = 3, SOCK_PACKET = 10, SOCK_CLOEXEC = 02000000, SOCK_NONBLOCK = 00004000, };
struct sockaddr_un;
struct sockaddr;

int socket(    int __domain, int __type, int __protocol);		// Does not return fd but index pointer to fd storage
int socketpair(int __domain, int __type, int __protocol, int __fds[2]);
int __connect(int fd, const struct sockaddr_un * addr, unsigned int len);
int __bind(   int fd, const void*              __addr, unsigned int len);
#define connect(fd, addr, len) ({ fd = __connect(fd, addr, len); 0; })
#define bind(   fd, addr, len) ({ fd = __bind(   fd, addr, len); 0; })	// Todo: Solve this ugliness
int setsockopt(int __fd, int __level, int __optname, const void *__optval, unsigned int __optlen);
struct msghdr {
	void	*	msg_name;	/* Socket name			*/
	int		msg_namelen;	/* Length of name		*/
	void *	msg_iov;	/* Data blocks			*/
	unsigned int	msg_iovlen;	/* Number of blocks		*/
	void 	*	msg_control;	/* Per protocol magic (eg BSD file descriptor passing) */
	unsigned int	msg_controllen;	/* Length of cmsg list */
	unsigned int	msg_flags;
};
ssize_t sendmsg(int __fd, const struct msghdr *__message, int __flags);
ssize_t recvmsg(int __fd,       struct msghdr *__message, int __flags);
ssize_t send(   int __fd, const void *__buf, size_t __n , int __flags);
ssize_t recv(   int __fd,       void *__buf, size_t __n , int __flags);
int     listen( int __fd, int __n);
int     accept( int __fd, struct sockaddr* __addr, unsigned int *__addr_len);

#define	_ARPA_INET_H 1		//#include <arpa/inet.h>

//#include <netinet/in.h>
#define _NETINET_IN_H
#define ntohl(x)	__uint32_identity (x)
#define ntohs(x)	__uint16_identity (x)
#define htonl(x)	__uint32_identity (x)
#define htons(x)	__uint16_identity (x)

int override_open(const char *path, int flags, ... /*int mode*/);
int override_close(int fd);
int override_dup(int fd);
int override_pipe(int fds[2]);
int override_fcntl(int fd, int cmd, ...);
ssize_t override_read(  int fd,       void *buf, size_t nbytes);
ssize_t override_write( int fd, const void *buf, size_t count);
ssize_t override_pread( int fd,       void *buf, size_t count, off_t offset);
ssize_t override_pwrite(int fd, const void *buf, size_t count, off_t offset);
int override_select (int __nfds, fd_set *__restrict __readfds, fd_set *__restrict __writefds, fd_set *__restrict __exceptfds, struct timeval *__restrict __timeout);

#ifndef TOMA_SANDBOX_BYPASS_REDIRECTS
#define open    override_open
#define close   override_close
#define dup     override_dup
#define pipe    override_pipe
#define fcntl   override_fcntl
#define read    override_read
#define write   override_write
#define pread   override_pread
#define pwrite  override_pwrite
#define select  override_select
#endif // TOMA_SANDBOX_BYPASS_REDIRECTS

/************************************* Emulation of netlink mechanism *************************************/
#define __LINUX_NETLINK_H	// #include <linux/netlink.h>
struct sockaddr_nl {
	unsigned short	nl_family;	/* AF_NETLINK	*/
	unsigned short	nl_pad;		/* zero		*/
	u32		nl_pid;				/* port ID	*/
	u32		nl_groups;			/* multicast groups mask */
};
struct nlmsghdr {				// 16[bytes] Copied netlink header from linux include
	u32		nlmsg_len;
	u16		nlmsg_type;
	u16		nlmsg_flags;
	u32		nlmsg_seq;			// Unused
	u32		nlmsg_pid;
};
#define NLMSG_ALIGN(len) (((len)+3U) & ~3U)
#define NLMSG_HDRLEN	 ((int) NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh)  ((void *)(((char *)nlh) + NLMSG_HDRLEN))
#define NLMSG_NEXT(nlh, len) ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len), (struct nlmsghdr *)(((char *)(nlh)) + NLMSG_ALIGN((nlh)->nlmsg_len)))
#define NLMSG_OK(  nlh, len) ((len) >= (int)sizeof(struct nlmsghdr) && (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && (nlh)->nlmsg_len <= (len))
#define NLMSG_DONE 0x3
#define NLMSG_MIN_TYPE		0x10

#define TOMA_USE_USER_SPACE_SERVER_API (1)

/************************************* syslog *************************************/
#define _SYS_SYSLOG_H 1
#include <syslog.h>
enum sys_log_priorities { LOG_DEBUG = 7, LOG_INFO = 6, LOG_NOTICE = 5, LOG_WARNING = 4, LOG_ERR = 3, LOG_CRIT = 2, LOG_ALERT = 1, LOG_EMERG = 0 };
void syslog(int priority, const char *format, ...);
static inline void closelog(void) {}
#define	_PATH_LOG TOMA_ROOT_DIR "sys_log"

#define	_SYS_IOCTL_H 1
#include <sys/ioctl.h>
int ioctl(int fd, unsigned long int __request, ...);
#define FIONBIO		0x5421
#undef is_block_device_stat
#define is_block_device_stat(s) true

/************************************* OS *************************************/
//#include "./interfaces/os/nvmeibt_os_signal.h"
#define NVMEIBT_OS_SIGNAL
int init_signal_handling(const char *exe_name);
void handle_sig_fd(int signals_fd, void (*sig_handler)(int32_t n, uint64_t addr));

int nvmeibt_nonblock_fd(int fd);

// Epoll
#define	_SYS_EPOLL_H	1		// #include <sys/epoll.h>
enum EPOLL_EVENTS {
	EPOLLIN = 0x001, EPOLLPRI = 0x002, EPOLLOUT = 0x004, EPOLLERR = 0x008, EPOLLHUP = 0x010,
	EPOLL_CLOEXEC = 02000000,
};
enum EPOLL_CTL { EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3 };
typedef struct epoll_data {
	void *ptr;
} epoll_data_t;

struct epoll_event {
	epoll_data_t data;
	uint32_t events;
	int __fd;		// Private usage of the sandbox
};
int epoll_create1(int __flags);
int epoll_ctl( int efd, enum EPOLL_CTL __op, int __fd, struct epoll_event *ev_ptr);
int epoll_wait(int efd,                                struct epoll_event *ev_arr, int arr_size, int time_out_ns);

/************************************* network ********************************/
#define NVMEIBT_NETWORK_INCS_H			//#include "interfaces/network/network_incs.h"

// API SRM<-->Toma
int  rsrm_init_work_tmq(void);			// Create SRM
void rsrm_resend_acks(void);			// Periodic high priority job: Send acks for received packets
int  rsrm_get_fd_timer(void);			// Get Timer file descriptor for periodic timer based resent packets
int  rsrm_resend_timer(void);			// Callback for timer

// API SRM faults<-->Toma
int  rsrm_faults_init_fifo_comm(void);	// Create Fifo queue (cli file)
int  rsrm_faults_get_fd(void);			// Get descriptor of fifo to select
void rsrm_faults_handle_fifo_comm(void);// Handle fault after wakeup from select

// Network manager (For raft communication) nm
struct nvmeibt_nm_local_node; struct nvmeibt_nic; struct nvmeibt_node; struct nvmeibt_msg_request;
void *nvmeibt_nm_tracer_init(const char *lib_path);
struct nvmeibt_nm_local_node *nvmeibt_nm_init(void *handle);
int  nvmeibt_nm_get_fd(         struct nvmeibt_nm_local_node *);			// For Toma epoll
void nvmeibt_nm_done(           struct nvmeibt_nm_local_node *);
int  nvmeibt_nm_add_remote_nic( struct nvmeibt_nm_local_node *, struct nvmeibt_nic *);
int  nvmeibt_nm_del_remote_nic( struct nvmeibt_nm_local_node *, struct nvmeibt_nic *);
int  nvmeibt_nm_del_remote_node(struct nvmeibt_nm_local_node *, struct nvmeibt_node *);
int  nvmeibt_nm_cancel_req_node(struct nvmeibt_nm_local_node *, struct nvmeibt_node *);
int nvmeibt_nm_process_toma_requests(    struct nvmeibt_nm_local_node *);	// Toma call this to process incoming network events
int  nvmeibt_nm_queue_srm_req(           struct nvmeibt_nm_local_node *, struct nvmeibt_node *, struct nvmeibt_msg_request *req);	// Toma sends messages with this func
bool nvmeibt_nm_is_remote_node_connected(struct nvmeibt_nm_local_node *, struct nvmeibt_node *);
void nvmeibt_nm_rsrm_resend_acks(           struct nvmeibt_nm_local_node *);
void nvmeibt_nm_rsrm_faults_handle_fifo_com(struct nvmeibt_nm_local_node *);
int nvmeibt_nm_rsrm_send_timer(             struct nvmeibt_nm_local_node *);
int nvmeibt_nm_print_status(     void *ctx, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
int nvmeibt_nm_print_status_json(void *ctx, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);


// Todo: Remove. Split nvmeibt_node.c to Toma part (config) and network part:
struct nvmeibt_sr_cb_table {
	void (*send_c)(void *arg, int status);	/* data send-completion callback */
};
struct nvmeibt_msg_request {
	unsigned int msg_type;
	unsigned int reason;
	int msg_len;
	int data_len;
	const void *cnst_msg;
	const char *cnst_data;
	struct nvmeibt_sr_cb_table cbs;
	void *arg, *user;
};

#define SRM_EMPTY_USER ((void *)(~(0ULL)))
#include "../common/nvmeib_hash.h"
struct nvmeibt_srm { int dummy; };
struct udp_peer { int dummy; };
union ibv_gid { uint8_t raw[16]; struct { uint64_t subnet_prefix; uint64_t interface_id;} global; };
struct nvmeibt_node;
static inline int is_peer_reachable(const struct udp_peer *p) { (void)p; return 0;}
int nvmeibt_srm_queue_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req);
struct nvmeibt_srm *udp_peer_srm(struct udp_peer *peer);
int allocate_udp_server(int ip_protocol, union ibv_gid *gid, struct nvmeibt_node *node);
int start_udp_server(void);
int nvmeib_register_udp_peer(struct nvmeibt_node *node, const char *peer_name, const char *peer_guid);

/************************************* Kafka ************************************/
#include "sandbox_kafka_public.h"

/************************************* udev ************************************/
#include "interfaces/nvme/nvmeibt_udev.h"
#define NVMEIBT_TOMA_LIB_UDEV_API_H // #include "interfaces/nvme/nvmeibt_lib_udev_api.h"
// Code below replaces: #include <libudev.h>
struct udev_list_entry { const char *name; const char* path; struct udev_list_entry* next; };
struct udev { int ref; struct udev_list_entry ent[3]; /* amount of local nvme disks */ };
struct udev* udev_new(void);
static inline void udev_unref(struct udev* u) { u->ref--; if (u->ref == 0) free(u); }

struct udev_enumerate { int ref; };
static inline struct udev_enumerate* udev_enumerate_new(struct udev* u) {  u->ref++; return (struct udev_enumerate*)u; }
static inline void udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char* sub) { (void)e; (void)sub; }
static inline void udev_enumerate_add_match_property( struct udev_enumerate *e, const char* key, const char* val) { (void)e; (void)key; (void)val; }
static inline void udev_enumerate_scan_devices(       struct udev_enumerate *e) { (void)e; }
static inline void udev_enumerate_unref(              struct udev_enumerate* e) { udev_unref((struct udev* )e); }

static inline struct udev_list_entry* udev_enumerate_get_list_entry(struct udev_enumerate *e) { return ((struct udev*)e)->ent; }
#define udev_list_entry_foreach(list_entry, first_entry) for (list_entry = first_entry; list_entry != NULL; list_entry = list_entry->next)
static inline const char* udev_list_entry_get_name(struct udev_list_entry *u) { return u->path; }

struct udev_device { struct udev_list_entry *e; };
struct udev_device* udev_device_new_from_syspath(struct udev *u, const char *path);
/// The devpath is the path under /sys to the device. E.g. `/devices/ACPI0004:00/0/host0/block/sda`
static inline const char* udev_device_get_devpath(struct udev_device* d) { return d->e->path; }
/// The devnode is the name of the device (full path to the /dev node). E.g. `/dev/sda `
static inline const char* udev_device_get_devnode(struct udev_device* d) { return d->e->name; }
static inline void udev_device_unref(             struct udev_device* d) { free(d); }

#endif // NVMEIBT_TOMA_IN_SANDBOX_H
