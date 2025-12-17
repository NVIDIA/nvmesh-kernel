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
#define pipe    override_pipe
#define fcntl   override_fcntl
#define read    override_read
#define write   override_write
#define pread   override_pread
#define pwrite  override_pwrite
#define select  override_select
#endif // TOMA_SANDBOX_BYPASS_REDIRECTS

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
};
int epoll_create1(int __flags);
int epoll_ctl( int efd, enum EPOLL_CTL __op, int __fd, struct epoll_event *ev_ptr);
int epoll_wait(int efd,                                struct epoll_event *ev_arr, int arr_size, int time_out_ns);

/************************************* logging ********************************/
#include "interfaces/log/log_incs.h"
#include "common/compat/kr_incs_types.h"
#include "common/nvmeib_str.h"
#include "common/compat/kr_incs_compiler_types.h"

#define NVMEIBT_DUMPER
#define nvmeibt_dumper_enable(...)
static inline bool nvmeibt_replay_is_enabled(void){ return false; }

#define nvmeibt_dumper_change_of_leader(...)
#define nvmeibt_dumper_event_mgmt_config(...)
#define nvmeibt_dumper_event_raft_persist(...)
#define nvmeibt_dumper_event_global_topo(...)
#define nvmeibt_dumper_event_peer_applied(...)
#define nvmeibt_dumper_event_remove_disk(...)
static inline int nvmeibt_dumper_init(void) { return 0;}
static inline void nvmeibt_dumper_exit(void) { return;}

/************************************* network ********************************/
#define NVMEIBT_NETWORK_INCS_H			//#include "interfaces/network/network_incs.h"

// API SRVR<-->Toma Netlink
int scan_server_netdev(void);			// Get file descriptor on which to select
int handle_netlink_event(int sock);		// Callback upon event

// API SRM<-->Toma
int  rsrm_init_work_tmq(void);			// Create SRM
void rsrm_resend_acks(void);			// Periodic high priority job: Send acks for received packets
int  rsrm_get_fd_timer(void);			// Get Timer file descriptor for periodic timer based resent packets
int  rsrm_resend_timer(void);			// Callback for timer

// API SRM faults<-->Toma
int  rsrm_faults_init_fifo_comm(void);	// Create Fifo queue (cli file)
int  rsrm_faults_get_fd(void);			// Get descriptor of fifo to select
void rsrm_faults_handle_fifo_comm(void);// Handle fault after wakeup from select

// Todo: Remove. Encapsulate into unified layer with SRM
int  udp_server_sock(void);				// Get socket descriptor on which to select
void nvmeibt_udp_read_event(int fd);	// Callback upon event
int  udp_server_timer(void);			// Get timer descriptor on which to select
void nvmeibt_udp_timer_event(int fd);	// Callback upon event

// nm
struct nvmeibt_nm_local_node { int dummy; };
struct nvmeibt_nic; struct nvmeibt_node; struct nvmeibt_msg_request;
static inline struct nvmeibt_nm_local_node * nvmeibt_nm_init(const char *lib_path) { (void)lib_path; return (struct nvmeibt_nm_local_node*)malloc(sizeof(struct nvmeibt_nm_local_node)); }
static inline void nvmeibt_nm_done(struct nvmeibt_nm_local_node *n) { free(n); }
#define nvmeibt_nm_add_remote_nic(...)
#define nvmeibt_nm_del_remote_node(...)
static inline int nvmeibt_nm_del_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic) { (void)ln; (void)nic; return 0; }
#define nvmeibt_nm_cancel_req_node(...)
static inline int  nvmeibt_nm_queue_srm_req(           struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *remote_node, struct nvmeibt_msg_request *req) { (void)ln; (void)remote_node; (void)req; return 0; }
static inline bool nvmeibt_nm_is_remote_node_connected(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *remote_node) { (void)ln; (void)remote_node; return true; }
#define nvmeibt_nm_get_fd(...) (-1)
#define nvmeibt_nm_rsrm_resend_acks(...)
#define nvmeibt_nm_rsrm_faults_handle_fifo_com(...)
#define nvmeibt_nm_rsrm_send_timer(...) (0)
static inline int nvmeibt_nm_process_toma_requests(void* v) { (void)v; return 0; }
#define nvmeibt_nm_print_status(...) ({})
#define nvmeibt_nm_print_status_json(...) ({})


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
	//void *arg;
	void *user;
};

#define SRM_EMPTY_USER ((void *)(~(0ULL)))
#include "nvmeibt_ds.h"
struct nvmeibt_srm { int dummy; };
struct udp_peer { struct xdlist node_link; };
union ibv_gid { int dummy; };
struct nvmeibt_node;
static inline int is_peer_reachable(const struct udp_peer *p) { (void)p; return 0;}
int nvmeibt_srm_queue_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req);
struct nvmeibt_srm *udp_peer_srm(struct udp_peer *peer);
int allocate_udp_server(int ip_protocol, union ibv_gid *gid, struct nvmeibt_node *node);
int start_udp_server(void);
int nvmeib_register_udp_peer(struct nvmeibt_node *node, const char *peer_name, const char *peer_guid);

/************************************* srvr ***********************************/
//#include "interfaces/srvr/nvmeibt_srvr_proc.h"

/************************************* Kafka ************************************/
// Implemented according to: https://docs.confluent.io/platform/current/clients/librdkafka/html/rdkafka_8h.html
#define NVMEIBT_TOMA_MSG_Q_API_H	// include "interfaces/nvmeibt_msg_queue_api.h"
enum { RD_KAFKA_OFFSET_BEGINNING = -2, RD_KAFKA_OFFSET_END  = -1,  RD_KAFKA_OFFSET_STORED = -1000, RD_KAFKA_OFFSET_INVALID = -1001};

typedef struct rd_kafka_s rd_kafka_t;
typedef struct rd_kafka_topic_s rd_kafka_topic_t;
typedef struct rd_kafka_conf_s rd_kafka_conf_t;
typedef struct rd_kafka_topic_conf_s rd_kafka_topic_conf_t;
typedef struct rd_kafka_queue_s rd_kafka_queue_t;
typedef struct rd_kafka_op_s rd_kafka_event_t;
typedef struct rd_kafka_topic_result_s rd_kafka_topic_result_t;
typedef struct rd_kafka_consumer_group_metadata_s rd_kafka_consumer_group_metadata_t;
typedef struct rd_kafka_error_s rd_kafka_error_t;
typedef struct rd_kafka_headers_s rd_kafka_headers_t;
typedef struct rd_kafka_group_result_s rd_kafka_group_result_t;
typedef struct rd_kafka_acl_result_s rd_kafka_acl_result_t;
typedef struct rd_kafka_Uuid_s rd_kafka_Uuid_t;
typedef struct rd_kafka_topic_partition_result_s rd_kafka_topic_partition_result_t;
typedef struct rd_kafka_topic_partition_s {
	rd_kafka_t* k;
	const char *topic;
	int parition, offset;
} rd_kafka_topic_partition_t;

typedef struct rd_kafka_topic_partition_list_s {
	rd_kafka_topic_partition_t elems[1];
	int cnt, size;
} rd_kafka_topic_partition_list_t;

rd_kafka_topic_partition_list_t* rd_kafka_topic_partition_list_new(int n);
rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add(    rd_kafka_topic_partition_list_t *, const char* name, int32_t partition);
void rd_kafka_topic_partition_list_destroy(rd_kafka_topic_partition_list_t*);

typedef enum {
	RD_KAFKA_RESP_ERR_NO_ERROR = 0, RD_KAFKA_RESP_ERR__SSL = -50, RD_KAFKA_RESP_ERR__AUTHENTICATION, RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_UNSUPPORTED_SASL_MECHANISM, RD_KAFKA_RESP_ERR_ILLEGAL_SASL_STATE, RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED, RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR__FATAL, RD_KAFKA_RESP_ERR__TIMED_OUT, RD_KAFKA_RESP_ERR__PARTITION_EOF,
	RD_KAFKA_PARTITION_UA = -1
} rd_kafka_resp_err_t;
enum rd_kafka_type_t { RD_KAFKA_CONSUMER = 'C', RD_KAFKA_PRODUCER = 'P' };
char* rd_kafka_err2str(rd_kafka_resp_err_t e);
char* rd_kafka_err2name(rd_kafka_resp_err_t e);
rd_kafka_resp_err_t rd_kafka_last_error(void);
typedef enum { RD_KAFKA_CONF_UNKNOWN = -2,  RD_KAFKA_CONF_INVALID = -1001,  RD_KAFKA_CONF_OK = 0 } rd_kafka_conf_res_t;
typedef struct { char* payload; void *_private; int len; int offset; rd_kafka_resp_err_t err; } rd_kafka_message_t;
void rd_kafka_message_destroy(rd_kafka_message_t*msg);

rd_kafka_conf_t* rd_kafka_conf_new(void);
void rd_kafka_conf_destroy(rd_kafka_conf_t* me);
rd_kafka_conf_res_t rd_kafka_conf_set(rd_kafka_conf_t*kc, const char *key, const char *val, char* err_str, size_t size_of_err);
rd_kafka_t* rd_kafka_new(enum rd_kafka_type_t who, rd_kafka_conf_t*cfg, char*err_str, size_t size_of_err);
void rd_kafka_conf_set_error_cb( rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, int err, const char *reason, void *opaque));
void rd_kafka_conf_set_dr_msg_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk,const rd_kafka_message_t *kmsg, void *opaque));

rd_kafka_resp_err_t rd_kafka_poll_set_consumer(rd_kafka_t* me);
const char*         rd_kafka_name(   const rd_kafka_t* me);
void                rd_kafka_set_log_level(rd_kafka_t* me, int lvl);
void                rd_kafka_flush(        rd_kafka_t* me, int x);
rd_kafka_resp_err_t rd_kafka_unsubscribe(  rd_kafka_t* me);
void                rd_kafka_destroy(      rd_kafka_t* me);
int                 rd_kafka_poll(         rd_kafka_t* me, bool is_blocking);
rd_kafka_resp_err_t rd_kafka_commit(       rd_kafka_t* me, rd_kafka_topic_partition_list_t* pl, int is_async);
rd_kafka_resp_err_t rd_kafka_committed(    rd_kafka_t* me, rd_kafka_topic_partition_list_t* pl, int x);
rd_kafka_resp_err_t rd_kafka_query_watermark_offsets(rd_kafka_t *me, const char *str, int32_t partition, int64_t *low_oldest_beginning_offset, int64_t *high_newest_end_offset, int timeout);
rd_kafka_resp_err_t rd_kafka_position	(  rd_kafka_t *me, rd_kafka_topic_partition_list_t *pl);
rd_kafka_topic_conf_t* rd_kafka_topic_conf_new(void);
void rd_kafka_topic_conf_destroy(rd_kafka_topic_conf_t *conf);
rd_kafka_topic_t* rd_kafka_topic_new(rd_kafka_t *rk, const char* name, rd_kafka_topic_conf_t* conf);

void                rd_kafka_topic_destroy(rd_kafka_topic_t *kt);
rd_kafka_message_t* rd_kafka_consume(      rd_kafka_topic_t* kt, int32_t partition, int timeout_ms);	// Deprecated
rd_kafka_message_t* rd_kafka_consumer_poll(rd_kafka_t *rk, int timeout_ms);
void                rd_kafka_consume_stop( rd_kafka_topic_t *kt, int32_t partition);
rd_kafka_resp_err_t rd_kafka_consumer_close(rd_kafka_t *rk);
rd_kafka_resp_err_t rd_kafka_assign(rd_kafka_t *rk, const rd_kafka_topic_partition_list_t *pl);
const char*         rd_kafka_topic_name(const rd_kafka_topic_t*);
rd_kafka_resp_err_t rd_kafka_consume_start(rd_kafka_topic_t *kt, int32_t partition, int64_t offset);
rd_kafka_resp_err_t rd_kafka_offset_store( rd_kafka_topic_t *kt, int32_t partition, int64_t offset);

static inline void rd_kafka_wait_destroyed(int n_msec) { (void)n_msec; }
enum my_rd_kafka_purge_flags { RD_KAFKA_PURGE_F_INFLIGHT = 0x2, RD_KAFKA_PURGE_F_NON_BLOCKING = 0x4 };
rd_kafka_resp_err_t rd_kafka_purge(rd_kafka_t * rk, int purge_flags);
enum my_rd_kafka_producer_flags { RD_KAFKA_MSG_F_FREE = 0x1, RD_KAFKA_MSG_F_COPY = 0x2 };
int rd_kafka_produce(rd_kafka_topic_t *kt, int32_t partition, int msgflags, void *payload, size_t len, const void *key, size_t keylen, void *msg_opaque);
#define rd_kafka_fatal_error(...) RD_KAFKA_RESP_ERR__FATAL
#define LOG_DEBUG (5)

/************************************* udev ************************************/
#include "interfaces/nvme/nvmeibt_udev.h"
#define NVMEIBT_TOMA_LIB_UDEV_API_H // #include "interfaces/nvme/nvmeibt_lib_udev_api.h"
// Code below replaces: #include <libudev.h>
struct udev_list_entry { const char *name; const char* path; struct udev_list_entry* next; };
struct udev { int ref; struct udev_list_entry ent[2]; /* amount of local nvme disks */ };
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
static inline const char* udev_device_get_devpath(struct udev_device* d) { return d->e->path; }
static inline const char* udev_device_get_devnode(struct udev_device* d) { return d->e->name; }
static inline void udev_device_unref(             struct udev_device* d) { free(d); }

#endif // NVMEIBT_TOMA_IN_SANDBOX_H
