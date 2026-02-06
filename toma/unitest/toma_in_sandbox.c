#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS I/O functions from this module - must be defined before any other includes

#include "nvmeibt_debug.h"
#include "toma_in_sandbox.h"
#include "sandbox_util.h"
#include "sandbox_nvme.h"
#include "mgmt_sim.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#define FILE_SANDBOX_PREFIX TOMA_ROOT_DIR "var/run/nvmesh/sandbox_fd_"

#include <pthread.h>

#include <stdarg.h>				// va_list
void syslog(int priority, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, COL_PURPL);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, COL_RESET "\n");
	va_end(ap);
	(void)priority;
}

/************************************* Kernel ********************************/
// Determine if running with debugger
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/un.h>
#include <errno.h>

#include "interfaces/nvme/nvmeibt_nvme_defines.h"
// ioctl to nvme device
#define _IOC_NRBITS	8
#define _IOC_TYPEBITS	8
#ifndef _IOC_SIZEBITS
	#define _IOC_SIZEBITS	14
#endif

#ifndef _IOC_DIRBITS
	#define _IOC_DIRBITS	2
#endif
#define _IOC_NRMASK	((1 << _IOC_NRBITS)-1)
#define _IOC_TYPEMASK	((1 << _IOC_TYPEBITS)-1)
#define _IOC_SIZEMASK	((1 << _IOC_SIZEBITS)-1)
#define _IOC_DIRMASK	((1 << _IOC_DIRBITS)-1)

#define _IOC_NRSHIFT	0
#define _IOC_TYPESHIFT	(_IOC_NRSHIFT+_IOC_NRBITS)
#define _IOC_SIZESHIFT	(_IOC_TYPESHIFT+_IOC_TYPEBITS)
#define _IOC_DIRSHIFT	(_IOC_SIZESHIFT+_IOC_SIZEBITS)
#ifndef _IOC_NONE
	#define _IOC_NONE	0U
#endif
#ifndef _IOC_WRITE
	#define _IOC_WRITE	1U
#endif
#ifndef _IOC_READ
	#define _IOC_READ	2U
#endif
#define _IOC(dir,type,nr,size) (((dir)  << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | ((nr)   << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_TYPECHECK(t) (sizeof(t))
#define _IO(type,nr)		_IOC(_IOC_NONE,(type),(nr),0)
#define _IOR(type,nr,size)	_IOC(_IOC_READ,(type),(nr),(_IOC_TYPECHECK(size)))
#define _IOW(type,nr,size)	_IOC(_IOC_WRITE,(type),(nr),(_IOC_TYPECHECK(size)))
#define _IOWR(type,nr,size)	_IOC(_IOC_READ|_IOC_WRITE,(type),(nr),(_IOC_TYPECHECK(size)))
#define _IOR_BAD(type,nr,size)	_IOC(_IOC_READ,(type),(nr),sizeof(size))
#define _IOW_BAD(type,nr,size)	_IOC(_IOC_WRITE,(type),(nr),sizeof(size))
#define _IOWR_BAD(type,nr,size)	_IOC(_IOC_READ|_IOC_WRITE,(type),(nr),sizeof(size))

#include <stdarg.h>
#include <linux/fs.h>		// For BLKGETSIZE64, BLKSSZGET

// Offset used to indicate a non-random-access operation like send()/recv() or read()/write(),
// rather than a random access operation like pread()/pwrite().
#define OFFSET_NONE ((off_t) -1)

static bool nvmeibt_toma_is_running_as_a_utility(void);

/************************************* srvr ***********************************/
struct nvmeibs_toma_server_proc_buf; struct nvmeibt_host_name;
#include "interfaces/srvr/nvmeibt_srvr_proc.h"
#include "common/nvmeib_shared.h"
#include "srv/nvmeibs_srv_toma_messages.h"

struct TSB_server_toma_status_req_simu {
	int n_srvr_msg_idx;					// Ever increasing number
	int n_toma_replies_received;
	int expecting_reply_cookie;			// If sent a message to toma and expecting a reply, store it
	int max_reply_length_bytes;
	int n_msgs_to_registrants;
	enum nvmeibs_toma_server_msg_type msg_q[16];
};

struct TSB_server_toma_status_req_simu *TSB_server_toma_status_req_simu_get(void);

void TSB_server_toma_status_req_simu_init(struct TSB_server_toma_status_req_simu *me) {
	me->max_reply_length_bytes = 64;				// Ask to fill at most 64[b] of reply, currently not verifying the reply itself
	memset(me->msg_q, 0, sizeof(me->msg_q));
	me->msg_q[3] = NVMEIBS_TOMA_TRIGGER_JGC;		// Todo: Unitest environment should instruct this simulator to send specific messages
	me->msg_q[5] = NVMEIBS_TOMA_WRITE_STATUS_REQ;
	me->msg_q[7] = NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE;
}

bool server_simu_has_next_msg_for_toma(void) {
	struct TSB_server_toma_status_req_simu *me = TSB_server_toma_status_req_simu_get();
	me->n_srvr_msg_idx++;
	return (me->n_srvr_msg_idx < 16) && (me->msg_q[me->n_srvr_msg_idx] != 0);
}

void TSB_server_toma_status_req_simu_destroy(struct TSB_server_toma_status_req_simu *me) {
	BUG_ON(me->expecting_reply_cookie);				// Did not get a reply from Toma
	if (!nvmeibt_toma_is_running_as_a_utility()) {	// Only check for replies if we sent messages (standalone utilities like gpt_util don't communicate with TOMA)
		BUG_ON(me->n_toma_replies_received <= 0);	// Coverage tests did not receive any reply from Toma
	}
}

ssize_t server_simu_get_next_msg_for_toma(int fd, void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_toma_status_req_simu *me = TSB_server_toma_status_req_simu_get();
	struct nvmeibs_toma_server_proc_buf *msg_buf = (void*)buf;
	enum nvmeibs_toma_server_msg_type msg_type;
	(void)fd; (void)flags;
	BUG_ON(offset != OFFSET_NONE);
	BUG_ON(n <= sizeof(struct nvmeibs_toma_server_proc_buf));
	msg_type = me->msg_q[me->n_srvr_msg_idx];
	BUG_ON(msg_type == 0);			// Bug in epoll/select simulator implementation! Toma is trying to read a non existing message
	if (msg_type == NVMEIBS_TOMA_TRIGGER_JGC) {
		struct nvmeibs_msg_s2t_launch_JGC *pl = &msg_buf->trigger_JGC_cmd;
		msg_buf->type = NVMEIBS_TOMA_TRIGGER_JGC;
		strcpy(pl->disk_segment_urn_uuid_str, "todo_disk_seg");
		strcpy(pl->disk_id_str, "todo_disk_id");
		// Currently not expecting reply.
	} else if (msg_type == NVMEIBS_TOMA_WRITE_STATUS_REQ) {
		struct nvmeibs_msg_s2t_toma_status_req *pl = &msg_buf->status_req_msg;
		BUG_ON(me->expecting_reply_cookie);			// Still waiting for previous reply
		me->expecting_reply_cookie = 0x1000 + me->n_srvr_msg_idx;
		msg_buf->type = NVMEIBS_TOMA_WRITE_STATUS_REQ;
		pl->type = NVMEIBS_TOMA_STATUS_RAFT;
		pl->handle = 0 - me->expecting_reply_cookie;
		pl->handle_req = me->expecting_reply_cookie;
		strcpy(pl->fname, "placeholder.tmp");		// In real life should be 1 of toma_stat_proc_fname[]. We use 1 dedicated file to replace them all
		pl->max_length = me->max_reply_length_bytes;
	} else if (msg_type == NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE) {
		msg_buf->type = NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE; 		// Just meaningless message
	} else {
		BUG_ON(true); // BUG epoll simulator wrongly told toma that there is a msg from server but there isn't
	}
	return sizeof(struct nvmeibs_toma_server_proc_buf);
}

static ssize_t _srvr_simu_nvmeibs_toma_server_proc_recv(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_toma_status_req_simu *me = TSB_server_toma_status_req_simu_get();
	const struct nvmeibs_toma_server_proc_buf *m = buf;
	const enum nvmeibs_toma_server_msg_type type = m->type;
	BUG_ON((fd < 2) || (n != sizeof(*m)) || (offset != 0) || !buf);
	switch (type) {
		case NVMEIBS_TOMA_LOGIN:  SANDBOX_PRINT("SRVR_SIMU->Got: Toma_Hello %lu[b] via_netlink=%d\n", n, !!flags); break;
		case NVMEIBS_TOMA_LOGOUT: SANDBOX_PRINT("SRVR_SIMU->Got: TomaByeBye %lu[b] via_netlink=%d\n", n, !!flags); break;
		case NVMEIBS_TOMA_WRITE_STATUS_RESP: {
			const struct nvmeibs_msg_t2s_toma_status_resp *pl = &m->status_resp_msg;
			me->n_toma_replies_received++;
			SANDBOX_PRINT("SRVR_SIMU->Got: TomaStatusRep %lu[b], cnt=%d\n", n, me->n_toma_replies_received);
			BUG_ON(me->expecting_reply_cookie <= 0);				// Reply comes without server expecting it
			BUG_ON(pl->handle != 0 - me->expecting_reply_cookie);
			BUG_ON(pl->handle_req != me->expecting_reply_cookie);
			BUG_ON(pl->length >= (size_t)me->max_reply_length_bytes);		// Cannot reply more than permitted buf size including '\0'
			if ((pl->length + 1) == (size_t)me->max_reply_length_bytes)
				BUG_ON(!pl->is_overflow);							// Toma status reply was truncated. Verify that
			me->expecting_reply_cookie = false;
			break;
		}
		default: BUG_ON(true);		// Not supported yet
	}
	errno = 0;						// Failure not supported yet
	return n;
}

static ssize_t _srvr_simu_nvmeibs_toma_client_proc_recv(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_toma_status_req_simu *me = TSB_server_toma_status_req_simu_get();
	const struct nvmeibs_toma_client_proc_buf *m = buf;
	const u32 cid = (m->handle >> 32);		// Todo: Find client in hash
	BUG_ON((fd < 2) || (n != sizeof(*m)) || (offset != 0) || !buf);
	SANDBOX_PRINT("SRVR_SIMU->Got: 2_reg_clnt %lu[b] via_netlink=%d\n", n, flags);
	me->n_msgs_to_registrants++;
	if (!m->handle) { errno = ENXIO;	return -1; }
	if (!cid)		{ errno = EINVAL;	return -1; }
	if (0)			{ errno = ENXIO;	return -1; }	// Send fail to client
	return n;
}

/************************************* FD/Sockets ********************************/
static ssize_t _send_illegal_trap(int fd, const void *buf, size_t n, off_t offset, int flags) {
	BUG_ON(true || (fd < 2) || (n == 0) || (buf == NULL) || (offset != OFFSET_NONE) || (flags != 0));
	return 0;
}

static ssize_t _recv_illegal_trap(int fd, void *buf, size_t n, off_t offset, int flags) {
	BUG_ON(true || (fd < 2) || (n == 0) || (buf == NULL) || (offset != OFFSET_NONE) || (flags != 0));
	return 0;
}

static ssize_t _recv_empty(int fd, void *buf, size_t n, off_t offset, int flags) {
	BUG_ON(offset != OFFSET_NONE);
	BUG_ON((fd < 2) || (n == 0));
	(void)buf; (void)n; (void)offset; (void)flags;
	return 0;
}
static bool _recv_always_has_data(void) { return true; }

static ssize_t _rpc_inject(int fd, void *buf, size_t n, off_t offset, int flags) {
	static int n_rpcs_sent = 0;	// Todo: Here toma_rpc exe simulator should actually hold a list of rpcs and unitest env can add to it
	static const char* cmds[] = { "simulate dump_status\n", "simulate reread_conf\n",
		"simulate dump-clnt-hash 20\n", "simulate bm-garbage-collect 1\n", "simulate resend-praids-report vol1\n", "status\n", "status server_csvs\n"}; // Todo: This should be a linked list to which unit-test env injects rpc and toma extracts them 1 by 1.
	const bool only_checking = (flags & MSG_PEEK);
	(void)fd;
	BUG_ON(offset != OFFSET_NONE);
	if (n_rpcs_sent < (int)ARRAY_SIZE(cmds)) {
		const char* cmd = cmds[n_rpcs_sent];
		const size_t rv = strlen(cmd);
		BUG_ON(cmd[rv-1] != '\n');			// Must terminate with eol
		BUG_ON(n < rv);						// Need enough space for rpc cmd
		strncpy(buf, cmd, n);
		if (!only_checking)
			n_rpcs_sent++;
		return rv;
	}
	return 0;
}

static ssize_t _rpc_accept(int fd, const void *buf, size_t n, off_t offset, int flags) {
	const int print_n_bytes = min(n, (size_t)640);
	BUG_ON(offset != OFFSET_NONE);
	BUG_ON((fd < 2) || (n == 0)); (void)flags;
	SANDBOX_PRINT("RPC reply %u[b]: " COL_YELLOW "%.*s\n" COL_RESET, (unsigned)n, print_n_bytes, (const char*)buf);
	return n;
}

struct TSB_sock_otherside {		// Every implementation must derive from this sub class. Sandbox injects data to Toma via those functions
	// The send()/recv() operations act as a generic I/O interface that is common to both
	// seekable like a block device or a local file, and non-seekable resources like a pipe, socket, or FIFO.
	// Use offset == OFFSET_NONE to indicate a non-random I/O operation like read()/write() or send()/recv().
	// When offset != OFFSET_NONE (i.e. > 0) this indicates a pread()/pwrite() operation.
	ssize_t (*send)(int fd, const void *buf, size_t n, off_t offset, int flags);	// Toma sends data to simulator
	ssize_t (*recv)(int fd,       void *buf, size_t n, off_t offset, int flags);	// Toma receives data from simulator
	bool    (*has_data)(void);									// epoll()/select() on this socket/file-descriptor
	struct t_sandbox_sock *sock;								// Pointer to the socket structure which uses me
};

struct t_sandbox_sock_tbl {
	int n_socks;
	int debug_offset;		// Prevent confusion between real descriptors and emulated
	pthread_mutex_t mutex;
	struct t_sandbox_sock {
		FILE *f;
		int fd;				// File descriptor associated with the socket
		int dom;
		int type;
		int proto;
		u32 len;
		int ref_cnt;								// Same fd' is sometimes use multiple times by the simulator. accept(). Todo, clean this
		struct sockaddr_un addr;
		struct TSB_sock_otherside *other_side;		// Here sandbox connects to socket from the other side
	} socks[32];			// Max amount of sockets used by toma
};
bool sbfd_is_used(const struct t_sandbox_sock* s) {
	return (s->f != NULL) || (s->addr.sun_path[0] != 0);
}

struct t_sandbox_sock* t_sandbox_sock_tbl_find_next_unused(struct t_sandbox_sock_tbl *ts) {
	int i = ts->n_socks;
	for (i = 0; i < ts->n_socks; i++) {		// Reuse deleted fd
		if (!sbfd_is_used(&ts->socks[i]))
			return &ts->socks[i];
	}
	{										// Allocate next fd
		struct t_sandbox_sock *s = &ts->socks[ts->n_socks++];
		BUG_ON(i >= ARRAY_SIZE(ts->socks));
		BUG_ON(sbfd_is_used(s));
		return s;
	}
}

struct t_sandbox_all {
	struct t_sandbox_sock_tbl TS;
	struct TSB_signals_queue {						// Signaling mechanism to toma
		struct TSB_sock_otherside o;
		int sig;
	} TSB_sig;
	struct TSB_basic {								// Unit-test side connections of Toma sockets/fd's
		struct TSB_sock_otherside o;
	} TSB_udev, TSB_rpc, TSB_syslog, TSB_srm_fault, TSB_srm_timer;
	struct TSB_server {
		struct TSB_sock_otherside o;
	} TSB_srvr2toma, TSB_toma2srvr, TSB_toma2clnt;	// Toma 3 extern communication via server
	struct TSB_wakeup_pipe {
		struct TSB_sock_otherside o[2];				// 1 read, 1 write file descriptor
		pthread_mutex_t mutex;
		#define TSB_WU_PIPE_QUEUE_SIZE 32			// Simple fixed-size circular buffer queue of wakeup messages
		#define TSB_WU_PIPE_MSG_SIZE 16				// Toma write wakeup messages of exactly 16[b]
		struct {
			char data[TSB_WU_PIPE_MSG_SIZE];
		} queue[TSB_WU_PIPE_QUEUE_SIZE];			// Wakeup message from toma other threads to toma main thread
		int queue_head, queue_tail, queue_count;	// Next position to dequeue from, Next position to enqueue to, Number of messages in queue
	} TSB_wake_pip;
	struct globa_epoll {
		struct TSB_sock_otherside o;
		struct epoll_event evs[16];
		int n_fds;
	} TSB_epoll;
	struct kafka_simulator_t {
		rd_kafka_t *obj[10];
		int n_obj;
		void (*notify_producer_msg_accepted)( rd_kafka_t *rk, const rd_kafka_message_t *kmsg, void *opaque);
		void (*notify_consumer_offset_commit)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque);
	} kafka_simu;
	struct TSB_server_toma_status_req_simu s_req_simu;
	struct TSB_netlink_mock {
		struct TSB_sock_otherside o;
		unsigned n_recv_msgs;
		pthread_mutex_t mutex;			// Thread-safe message queue for netlink responses
		#define TSB_NL_QUEUE_SIZE 8		// Simple fixed-size queue of messages
		#define TSB_NL_MSG_SIZE 512
		struct {
			char data[TSB_NL_MSG_SIZE];
			size_t len;
		} queue[TSB_NL_QUEUE_SIZE];	// Outgoing messages to Toma
		int queue_head;			// Next position to dequeue from
		int queue_tail;			// Next position to enqueue to
		int queue_count;		// Number of messages in queue
	} TSB_netlink;
	struct TSB_server_comm_wakeup_mock {
		struct TSB_sock_otherside o[2];
		long n_wakeup_msgs __attribute__((aligned(sizeof(long))));
	} TSB_km_sock_pair;
	struct mgmt_sim_state *mgmt;
	char my_hostname[64];
	bool is_running_as_a_utility;
	bool can_use_bin_traces;
} *sys;

static ssize_t _socket_pair_wakeup_send(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_comm_wakeup_mock *w = &sys->TSB_km_sock_pair;
	long n_wups;
	BUG_ON((w->o[0].sock->fd != fd) || (n != 1) || (buf == NULL) || (offset != 0) || (flags != 0));
	n_wups = __atomic_add_fetch(&w->n_wakeup_msgs, 1, __ATOMIC_SEQ_CST);
	N_Tf(__AUTOID__, "n_wakups_in_queue=@INT", (int)n_wups);
	return n;
}

static ssize_t _socket_pair_wakeup_recv(int fd, void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_comm_wakeup_mock *w = &sys->TSB_km_sock_pair;
	const long n_wups = __atomic_load_n(&w->n_wakeup_msgs, __ATOMIC_SEQ_CST);
	BUG_ON((w->o[1].sock->fd != fd) || (n != 1) || (buf == NULL) || (offset != OFFSET_NONE) || (flags != 0));
	BUG_ON(n_wups < 0);
	if (n_wups > 0) {
		__atomic_sub_fetch(&w->n_wakeup_msgs, 1, __ATOMIC_SEQ_CST);
		*(char*)buf = 'w';		// Just for debug, so toma reads initialized character
		return 1;
	}
	return 0;
}

static bool _socket_pair_should_wakeup(void) {
	const struct TSB_server_comm_wakeup_mock *w = &sys->TSB_km_sock_pair;
	const long n = __atomic_load_n(&w->n_wakeup_msgs, __ATOMIC_SEQ_CST);
	return n > 0;
}

static bool _wakeup_pipe_should_wakeup(void) {
	struct TSB_wakeup_pipe *w = &sys->TSB_wake_pip;
	bool rv;
	pthread_mutex_lock(&w->mutex);
	rv = (w->queue_count != 0);
	pthread_mutex_unlock(&w->mutex);
	return rv;
}

static ssize_t _wakeup_pipe_wakeup_send(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_wakeup_pipe *w = &sys->TSB_wake_pip;
	int n_wake_ups;
	pthread_mutex_lock(&w->mutex);
	BUG_ON((w->o[1].sock->fd != fd) || (offset != 0) || (flags != 0) || (w->queue_count >= TSB_WU_PIPE_QUEUE_SIZE) || (n != TSB_WU_PIPE_MSG_SIZE));
	memcpy(w->queue[w->queue_tail].data, buf, n);
	w->queue_tail = (w->queue_tail + 1) % TSB_WU_PIPE_QUEUE_SIZE;
	n_wake_ups = ++w->queue_count;
	pthread_mutex_unlock(&w->mutex);
	N_Tf(__AUTOID__, "n_wake_ups=@INT", n_wake_ups);
	return n;
}

static ssize_t _wakeup_pipe_wakeup_recv(int fd, void *buf, size_t n, off_t offset, int flags) {
	struct TSB_wakeup_pipe *w = &sys->TSB_wake_pip;
	int n_wake_ups = 0;
	pthread_mutex_lock(&w->mutex);
	BUG_ON((w->o[0].sock->fd != fd) || (offset != OFFSET_NONE) || (flags != 0) || (w->queue_count < 0) || (n != TSB_WU_PIPE_MSG_SIZE));
	if (w->queue_count > 0) {
		memcpy(buf, w->queue[w->queue_head].data, n);
		w->queue_head = (w->queue_head + 1) % TSB_WU_PIPE_QUEUE_SIZE;
		n_wake_ups = --w->queue_count;
	} else {
		n = -1;			// No data
		errno = EAGAIN;
	}
	pthread_mutex_unlock(&w->mutex);
	N_Tf(__AUTOID__, "n_wake_ups=@INT", n_wake_ups);
	return n;
}

void sandbox_server_init(void);
void t_sandbox_all_init(bool is_running_as_a_utility) {
	sys = calloc(1, sizeof(*sys));
	sys->TS.debug_offset = 10000;
	sys->is_running_as_a_utility = is_running_as_a_utility;
	gethostname(sys->my_hostname, sizeof(sys->my_hostname) - 1);
	sys->mgmt = mgmt_sim_init(sys->my_hostname);
	pthread_mutex_init(&sys->TS.mutex, NULL);
	sandbox_server_init();
	pthread_mutex_init(&sys->TSB_wake_pip.mutex, NULL);
	sandbox_nvme_init();
	TSB_server_toma_status_req_simu_init(&sys->s_req_simu);
}

static bool nvmeibt_toma_is_running_as_a_utility(void) { return sys->is_running_as_a_utility; }

static void sandbox_kafka_validate_report_target(void) {
	const char *str = mgmt_sim_get_last_report_target();
	const bool ok = str && strstr(str, "NVMD_SN_002.1") && strstr(str, "NVMD_SN_003.1");
	BUG_ON(!ok);
}

void t_sandbox_all_destroy(void) {
	if (!nvmeibt_toma_is_running_as_a_utility())
		sandbox_kafka_validate_report_target();
	TSB_server_toma_status_req_simu_destroy(&sys->s_req_simu);
	pthread_mutex_destroy(&sys->TS.mutex);
	pthread_mutex_destroy(&sys->TSB_netlink.mutex);
	pthread_mutex_destroy(&sys->TSB_wake_pip.mutex);
	mgmt_sim_destroy();
	BUG_ON(!nvmeibt_toma_is_running_as_a_utility() && (sys->TSB_netlink.n_recv_msgs <= 0));	// Only check for replies if we sent messages (standalone utilities like gpt_util don't communicate with TOMA)
	free(sys);
	sys = NULL;
}

struct TSB_server_toma_status_req_simu *TSB_server_toma_status_req_simu_get(void) {
	return &sys->s_req_simu;
}

/// Look up and return a socket object by fd. Returns null if not found.
static struct t_sandbox_sock * TSB_socket_find_by_fd_opt(int fd) {
	struct t_sandbox_sock_tbl *TS = &sys->TS;
	int i;
	if (fd >= sys->TS.debug_offset)
		return &TS->socks[fd - TS->debug_offset];
	for (i = 0; i < TS->n_socks; i++) {
		if (TS->socks[i].fd == fd)
			return &TS->socks[i];
	}
	return NULL;
}

/// Look up and return a socket object by fd. Requires fd to valid: aborts the program if not.
static struct t_sandbox_sock * TSB_socket_find_by_fd(int fd) {
	struct t_sandbox_sock *s = TSB_socket_find_by_fd_opt(fd);
	if (s)
		return s;
	fprintf(stderr, "sandbox: no such fd %d\n", fd); BUG_ON(true);
	return NULL; // not reached
}

static struct TSB_sock_otherside* TSB_socket_find_other_side_by_fd(int fd) {
	struct t_sandbox_sock *s = TSB_socket_find_by_fd_opt(fd);
	if (s)
		return s->other_side;
	fprintf(stderr, "sandbox: no such fd %d\n", fd); BUG_ON(true);
	return NULL; // not reached
}


int ioctl(int fd, unsigned long int req, ...) {
	struct t_sandbox_sock *tsb = TSB_socket_find_by_fd_opt(fd);
	const char *path;
	va_list ap;
	int rv = 0;
	va_start(ap, req);

	if (!tsb) {
		N_Ef(ioc5734, "no such fd=@INT", fd);
		rv = -1;
		goto done;
	}

	path = tsb->addr.sun_path;

	N_Df(sbioct0, "ioctl fd=@INT path=@STR", fd, path);
	if (req == NVME_IOCTL_ADMIN_CMD) {
		const struct sandbox_nvme_device *nvme_dev = sandbox_nvme_get_device_by_path(path);
		struct nvme_admin_cmd *cmd =  va_arg(ap, struct nvme_admin_cmd*);
		if (!nvme_dev) {
			errno = ENOTTY;		// We presume the failure is because the device is not NVMe.
			N_Wf(sbioctnv1, "ioctl:nvme:admin: no device found for path=@STR", path);
			return -1;
		}
		N_Df(sbioctnv, "ioctl:nvme:admin opcode=@INT", cmd->opcode);
		if (cmd->opcode == nvme_admin_identify) {
			if (cmd->nsid == 0) {
				// NSID 0 is special - controller identify command.
				struct nvme_id_ctrl *idctrl = (void*)cmd->addr;
				BUG_ON(cmd->data_len != sizeof(*idctrl));
				memset(idctrl, 0, cmd->data_len);
				idctrl->vid = nvme_dev->vendor_id;
				snprintf(idctrl->sn, sizeof(idctrl->sn), "%s", nvme_dev->serial_number);
				snprintf(idctrl->mn, sizeof(idctrl->mn), "%s", nvme_dev->model_number);
				snprintf(idctrl->fr, sizeof(idctrl->fr), "0.0.1");
				N_Tf(sbk3456, "ioctl:nvme:id controller fd=@INT reporting sn=@STR mn=@STR", fd, idctrl->sn, idctrl->mn);
			} else {
				// NSID > 0 is the NVME storage namespace query.
				// Note that LBAF { ms, ds, rp } are defined in NVM-Express-NVM-Command-Set-Specification-Revision-1.2-2025.08.01
				// Figure 116: LBA Format Data Structure, NVM Command Set Specific (PDF p. 91).
				struct nvme_id_ns *response = (void*)cmd->addr;
				int i;
				BUG_ON(cmd->data_len < sizeof(*response));
				memset(response, 0, cmd->data_len);

				// Populate supported LBA formats from the sandbox LBA format table
				response->nlbaf = SANDBOX_NVME_LBAF_COUNT - 1;  // Number of supported LBA formats minus 1
				for (i = 0; i < SANDBOX_NVME_LBAF_COUNT; i++) {
					const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(i);
					response->lbaf[i].ds = lbaf->block_size_exp;
					response->lbaf[i].ms = lbaf->metadata_size;
				}

				// Set current format based on device's format index
				response->flbas = nvme_dev->current_format_idx;

				// Set metadata capabilities: both inline and separate metadata are supported by the device.
				// Note: Toma will only use separate metadata (DISK_ALLOW_INLINE_MD == 0).
				response->mc = NVME_NS_MC_INLINE_MASK | NVME_NS_MC_SEP_MASK;

				response->nsze = nvme_dev->size_in_blocks;
				N_Tf(sbk5443, "ioctl:nvme:id storage ns=@INT fd=@INT flbas=@INT nlbaf=@INT mc=@INT nsze=@INT64_TD",
				     cmd->nsid, fd, response->flbas, response->nlbaf, response->mc, response->nsze);
			}
		} else if (cmd->opcode == nvme_admin_get_log_page) {
			struct nvme_smart_log *fill =  (void*)cmd->addr;
			BUG_ON(cmd->data_len != sizeof(*fill));
			memset(fill, 0, cmd->data_len);
		}
	} else if (req == NVME_IOCTL_ID) {
		rv = fd;
	} else if (req == FIONBIO) {
		rv = 0;
	} else if (req == BLKSSZGET) {
		// Return block size from device's current format
		const struct sandbox_nvme_device *nvme_dev = sandbox_nvme_get_device_by_path(path);
		int *block_size = va_arg(ap, int*);
		if (nvme_dev) {
			const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(nvme_dev->current_format_idx);
			*block_size = 1 << lbaf->block_size_exp;
		} else {
			*block_size = 4096;  // Default for non-NVMe devices
		}
		rv = 0;
	} else if (req == BLKGETSIZE64) {
		// Return device size in bytes
		uint64_t *size_bytes = va_arg(ap, uint64_t*);
		struct stat st;
		int real_fd = tsb->fd;
		if (real_fd < 0) {
			rv = -1;
		} else if (fstat(real_fd, &st) == 0) {
			*size_bytes = st.st_size;
			rv = 0;
		} else {
			rv = -1;
		}
	} else {
		BUG_ON(true);
	}

done:
	va_end(ap);
	return rv;
}

/********************************* Netlink mock *******************************/
// Forward declarations for netlink queue helpers
static bool TSB_netlink_queue_has_something(void);
static ssize_t TSB_netlink_queue_dequeue(void *buf, size_t buf_size);
static void TSB_netlink_send_disk_response(const struct sandbox_nvme_device *dev, const struct nvmeib_nl_uk_comm_msg *req_msg);
static void TSB_netlink_handle_io_to_disk(const struct nvmeib_nl_uk_comm_msg *req_msg);
static void TSB_netlink_handle_zero_disk(const struct nvmeib_nl_uk_comm_msg *req_msg);
static void TSB_netlink_handle_format_disk(const struct nvmeib_nl_uk_comm_msg *req_msg, const struct nvmeib_format_disk *fmt_disk);
static void TSB_netlink_reply_to_blocked_toma(const struct nvmeib_nl_uk_comm_msg *req_msg, int rv);
static void TSB_netlink_handle_req_info(const struct nvmeib_nl_uk_comm_msg *req_msg);

// Netlink send callback: Toma sends a request, we parse it and queue responses
static ssize_t _netlink_recv_msg_from_toma(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_netlink_mock *nl = &sys->TSB_netlink;
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
	const struct nvmeib_nl_uk_comm_msg *req_msg = NLMSG_DATA(nlh);
	int n_payload_bytes_remainig = (int)n - ((const char*)req_msg->data - (const char*)buf);

	(void)fd; (void)offset; (void)flags;
	BUG_ON((n < (sizeof(struct nlmsghdr) + sizeof(*req_msg))) || (n != nlh->nlmsg_len) || (nlh->nlmsg_type != NVMESH_NL_MSG_TYPE) || (req_msg->caller_type != TOMA_CALLER));
	nl->n_recv_msgs++;
	N_Tf(nl_send, "msg[@INT].id=@ID (@STR), total_n_msgs=@INT", req_msg->opcode, req_msg->id, uk_comm_opcode_str(req_msg->opcode), nl->n_recv_msgs);
	if (req_msg->opcode == csc_get_disks) {
		int i, count = sandbox_nvme_get_device_count();	// Queue disk info for each mock NVMe device
		for (i = 0; i < count; i++) {
			const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_index(i);
			if (dev && !dev->stock_disk) {  // Only queue NVMesh disks, not stock disks
				TSB_netlink_send_disk_response(dev, req_msg);
			}
		}
	} else if (req_msg->opcode == csc_keep_alive) {
		N_Tf(nl_keepalive, "Netlink keep_alive received");
	} else if (req_msg->opcode == csc_io_to_disk) {
		const union nvmeib_nl_msg_to_srvr_payload *pay = (const union nvmeib_nl_msg_to_srvr_payload *)req_msg->data;
		n_payload_bytes_remainig -= sizeof(pay->io2disk);
		TSB_netlink_handle_io_to_disk(req_msg);
	} else if (req_msg->opcode == csc_zero_disk) {
		const union nvmeib_nl_msg_to_srvr_payload *pay = (const union nvmeib_nl_msg_to_srvr_payload *)req_msg->data;
		n_payload_bytes_remainig -= sizeof(pay->zero_disk);
		TSB_netlink_handle_zero_disk(req_msg);
	} else if (req_msg->opcode == csc_format_disk) {
		const union nvmeib_nl_msg_to_srvr_payload *generic_payload = (const union nvmeib_nl_msg_to_srvr_payload *)req_msg->data;
		const struct nvmeib_format_disk *fmt_disk = &generic_payload->fmt_disk;
		n_payload_bytes_remainig -= sizeof(*fmt_disk);
		TSB_netlink_handle_format_disk(req_msg, fmt_disk);
	} else if (req_msg->opcode == csc_t2s_blocking_msg_other) {
		struct TSB_server *s = &sys->TSB_toma2srvr;
		const struct nvmeibs_toma_server_proc_buf *m = (typeof(m))req_msg->data;
		const ssize_t exec_rv = s->o.send(0xDEAD /*s->o.sock->fd*/, m, req_msg->len - (int)sizeof(*req_msg), 0, 'N');
		TSB_netlink_reply_to_blocked_toma(req_msg, (int)exec_rv);
		if (exec_rv > 0)
			n_payload_bytes_remainig -= (int)exec_rv;		// Mark Consumed bytes
	} else if (req_msg->opcode == csc_t2s_blocking_msg_to_io_clients) {
		struct TSB_server *s = &sys->TSB_toma2clnt;
		const struct nvmeibs_toma_client_proc_buf *m = (typeof(m))req_msg->data;
		const ssize_t exec_rv = s->o.send(0xDEAD /*s->o.sock->fd*/, m, req_msg->len - (int)sizeof(*req_msg), 0, 'N');
		TSB_netlink_reply_to_blocked_toma(req_msg, (int)exec_rv);
		if (exec_rv > 0)
			n_payload_bytes_remainig -= (int)exec_rv;		// Mark Consumed bytes
	} else if (req_msg->opcode == csc_t2s_blocking_msg_req_info) {
		const union nvmeib_nl_msg_to_srvr_payload *pay = (const union nvmeib_nl_msg_to_srvr_payload *)req_msg->data;
		n_payload_bytes_remainig -= sizeof(pay->req_info);
		TSB_netlink_handle_req_info(req_msg);
	} else {
		BUG_ON(true);		// Not implemented yet in sandbox
	}
	BUG_ON(n_payload_bytes_remainig != 0);			// Unconsumed bytes, may be treated as next message
	return (ssize_t)n;
}

// Netlink recv callback: Toma receives a response from the queue
static ssize_t _netlink_reply_to_toma(int fd, void *buf, size_t n, off_t offset, int flags) {
	const ssize_t len = TSB_netlink_queue_dequeue(buf, n);
	(void)fd; (void)offset; (void)flags;
	BUG_ON(n <= (size_t)len);
	return len;
}

// Connect the other side which communicates with Toma
void TSB_connect_sock_to_listener(struct t_sandbox_sock *s) {
	if (strstr(s->addr.sun_path, "netlink")) {				BUG_ON(s->other_side); s->other_side = &sys->TSB_netlink.o;
	} else if (strstr(s->addr.sun_path, "signal")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_sig.o;
		s->other_side->has_data = _recv_always_has_data;			// Todo: unitest env should inject
	} else if (strstr(s->addr.sun_path, "sys_log")) {		BUG_ON(s->other_side); s->other_side = &sys->TSB_syslog.o;
	} else if (strstr(s->addr.sun_path, "srm_fault")) {		BUG_ON(s->other_side); s->other_side = &sys->TSB_srm_fault.o;
		s->other_side->has_data = _recv_always_has_data;			// Todo: unitest env should inject
	} else if (strstr(s->addr.sun_path, "srm_timer")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_srm_timer.o;
	} else if (strstr(s->addr.sun_path, "udev_monitor")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_udev.o;
		sys->TSB_udev.o.recv = _recv_empty;
		s->other_side->has_data = _recv_always_has_data;			// Todo: unitest env should inject
	} else if (strstr(s->addr.sun_path, "nvmesh/toma_rpc")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_rpc.o;
		sys->TSB_rpc.o.recv = _rpc_inject;
		sys->TSB_rpc.o.send = _rpc_accept;
	} else if (strstr(s->addr.sun_path, "epoll")) {			BUG_ON(s->other_side); s->other_side = &sys->TSB_epoll.o;
	} else if (strstr(s->addr.sun_path, "wakeup_pipe_pair0")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_wake_pip.o[0];
		s->other_side->recv = _wakeup_pipe_wakeup_recv;
		s->other_side->send = _send_illegal_trap;
		s->other_side->has_data = _wakeup_pipe_should_wakeup;	// o[0] Toma main thread read wakeups messages from other threads. Never writes
	} else if (strstr(s->addr.sun_path, "wakeup_pipe_pair1")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_wake_pip.o[1];
		s->other_side->send = _wakeup_pipe_wakeup_send;			// o[1] Toma aux thread write to wakeup toma main thread. Never reads
		s->other_side->recv = _recv_illegal_trap;
	} else if (strstr(s->addr.sun_path, "server_events")) { BUG_ON(s->other_side); s->other_side = &sys->TSB_srvr2toma.o;
	} else if (strstr(s->addr.sun_path, "toma_server")) {   BUG_ON(s->other_side); s->other_side = &sys->TSB_toma2srvr.o;
	} else if (strstr(s->addr.sun_path, "toma_clients")) {  BUG_ON(s->other_side); s->other_side = &sys->TSB_toma2clnt.o;
	} else if (strstr(s->addr.sun_path, "km_comm_pair0")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_km_sock_pair.o[0];
		s->other_side->send = _socket_pair_wakeup_send;			// o[0] Toma writes to it to wakeup server lib main thread. Never reads
		s->other_side->recv = _recv_illegal_trap;
	} else if (strstr(s->addr.sun_path, "km_comm_pair1")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_km_sock_pair.o[1];
		s->other_side->send = _send_illegal_trap;				// o[1] ServerLib reads from it to wakeup. Never writes
		s->other_side->recv = _socket_pair_wakeup_recv;
		s->other_side->has_data = _socket_pair_should_wakeup;
	} else {
		return;
	}
	s->other_side->sock = s;
}

void sandbox_server_init(void) {
	struct TSB_sock_otherside *o = &sys->TSB_toma2srvr.o;
	o->send = _srvr_simu_nvmeibs_toma_server_proc_recv;
	o->recv = _recv_illegal_trap;				// Via this fd, Toma only sends to to server. Server does not send anything to toma
	o = &sys->TSB_srvr2toma.o;
	o->recv = server_simu_get_next_msg_for_toma;
	o->send = _send_illegal_trap;				// Via this fd server sends msgs to Tom, Toma never replies back
	o->has_data = server_simu_has_next_msg_for_toma;
	o = &sys->TSB_toma2clnt.o;
	o->send = _srvr_simu_nvmeibs_toma_client_proc_recv;
	o->recv = _recv_illegal_trap;
	o = &sys->TSB_netlink.o;
	o->send = _netlink_recv_msg_from_toma;
	o->recv = _netlink_reply_to_toma;
	o->has_data = TSB_netlink_queue_has_something;
	pthread_mutex_init(&sys->TSB_netlink.mutex, NULL);
}

static bool sbfd_is_a_file(     const struct t_sandbox_sock* s) {
	return (s->type == 'f');
}

static bool sbfd_should_persist_after_close(const struct t_sandbox_sock* s) {
	// Only regular files should persist after close, but socket/pipe are non-persistent.
	return sbfd_is_a_file(s);
}

static const char* sbfd_get_open_mode(const struct t_sandbox_sock* s) {
	if (!sbfd_is_a_file(s))
		return "w+";
	// Creating or truncating: use "w+" (safe - explicit intent to overwrite)
	if ((s->proto & (O_CREAT | O_TRUNC)) != 0)
		return "w+";
	// Read-only: use "r" (safe - no modification)
	if ((s->proto & O_ACCMODE) == O_RDONLY)
		return "r";
	// Explicit append mode requested: use "a+" (safe - caller wants append)
	if (s->proto & O_APPEND)
		return "a+";
	// O_RDWR on existing file without O_APPEND: use "r+"
	// This allows pwrite() to work at any offset (needed for GPT, etc.)
	// Safe because: file must already exist, no truncation, no append
	return "r+";
}

int socket(int __domain, int __type, int __protocol) {
	struct t_sandbox_sock_tbl *TS = &sys->TS;
	struct t_sandbox_sock *s = t_sandbox_sock_tbl_find_next_unused(TS);
	s->dom =__domain;
	s->type = __type;
	s->proto =__protocol;
	s->ref_cnt = 1;
	return TS->debug_offset + (int)(s - sys->TS.socks);
}

static void socket_destroy(struct t_sandbox_sock *s) {
	const bool should_del = !sbfd_should_persist_after_close(s);
	s->ref_cnt--;
	if (s->ref_cnt > 0)
		return;
	SANDBOX_PRINT("TSB[%2d]: fd=%2d, path=%-40s, close, del=%u\n", (int)(s - sys->TS.socks), s->fd, s->addr.sun_path, should_del);
	if (sys->can_use_bin_traces)		// Some fd's are closed after binary traces were shut down
		N_Df(sbd8465, "sandbox file: close path=@STR fd=@INT mode=@STR delete=@BOOL", s->addr.sun_path, s->fd, sbfd_get_open_mode(s), should_del);
	if (s->f != NULL) {
		fclose(s->f);
	}
	if (should_del) {
		unlink(s->addr.sun_path);
	}
	if (s->other_side)
		memset(s->other_side, 0, sizeof(*s->other_side));
	memset(s, 0, sizeof(*s));
};

int TSB_sock_open(struct t_sandbox_sock *s) {
	const char* open_mode = sbfd_get_open_mode(s);
	s->f = fopen(s->addr.sun_path, open_mode);
	if (s->f == NULL) {
		s->fd = -1;
		return -1;
	}
	s->fd = fileno(s->f);
	TSB_connect_sock_to_listener(s);
	N_Df(sbo0564, "sandbox file: open path=@STR fd=@INT mode=@STR", s->addr.sun_path, s->fd, open_mode);
	SANDBOX_PRINT("TSB[%2d]: fd=%2d, path=%-40s, mode=%s (%o), listener=%c\n",
		 (int)(s - sys->TS.socks), s->fd, s->addr.sun_path, open_mode, s->proto, ((s->other_side) ? 'Y' : 'N'));
	return s->fd;
}

int __connect(int fd, const struct sockaddr_un * addr, unsigned int len) {
	struct t_sandbox_sock_tbl *TS = &sys->TS;
	struct t_sandbox_sock *s = &TS->socks[fd - TS->debug_offset];
	s->addr = *addr;
	s->len = len;
	return TSB_sock_open(s);
}

int __bind(int fd, const void* __addr, unsigned int len) {
	struct t_sandbox_sock_tbl *TS = &sys->TS;
	struct t_sandbox_sock *s = &TS->socks[fd - TS->debug_offset];
	const struct sockaddr_nl *addr = __addr;
	s->addr.sun_family = addr->nl_family;
	if (addr->nl_family == AF_NETLINK) {
		sprintf(s->addr.sun_path, FILE_SANDBOX_PREFIX "bind_netlink_sock");
	} else {
		const struct sockaddr_un *sun = __addr;
		BUG_ON(sun->sun_family != PF_UNIX);
		//sprintf(s->addr.sun_path, FILE_SANDBOX_PREFIX "bind_toma_rpc_sock");
		sprintf(s->addr.sun_path, "%s", &sun->sun_path[0]);
	}
	s->len = len;
	return TSB_sock_open(s);
}

int socketpair(int __domain, int __type, int __protocol, int fds[2]) {
	struct sockaddr_un addr;
	for (int i = 0; i < 2; i++) {
		sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "km_comm_pair%d", i);
		fds[i] = __connect(socket(__domain,__type,__protocol), &addr, 16);
	}
	return 0;
}

static bool TSB_netlink_queue_has_something(void) {		// Netlink mock queue helpers (thread-safe)
	bool rv;
	pthread_mutex_lock(&sys->TSB_netlink.mutex);
	rv = (sys->TSB_netlink.queue_count != 0);
	pthread_mutex_unlock(&sys->TSB_netlink.mutex);
	return rv;
}

static void TSB_netlink_queue_enqueue(const void *data, size_t len) {
	struct TSB_netlink_mock *nl = &sys->TSB_netlink;
	pthread_mutex_lock(&nl->mutex);
	BUG_ON(!(nl->queue_count < TSB_NL_QUEUE_SIZE && len <= TSB_NL_MSG_SIZE));
	memcpy(nl->queue[nl->queue_tail].data, data, len);
	nl->queue[nl->queue_tail].len = len;
	nl->queue_tail = (nl->queue_tail + 1) % TSB_NL_QUEUE_SIZE;
	nl->queue_count++;
	pthread_mutex_unlock(&nl->mutex);
}

static ssize_t TSB_netlink_queue_dequeue(void *buf, size_t buf_size) {
	struct TSB_netlink_mock *nl = &sys->TSB_netlink;
	ssize_t len = -1;
	pthread_mutex_lock(&nl->mutex);
	BUG_ON(nl->queue_count <= 0);	// Wrong Sandbox behaviour. Why is Toma trying to read if no msg scheduled. This will create error in netlink mechanism
	if (nl->queue_count > 0) {
		const size_t msg_len = nl->queue[nl->queue_head].len;
		BUG_ON(msg_len > buf_size);		// Toma gave too small buffer.
		memcpy(buf, nl->queue[nl->queue_head].data, msg_len);
		len = (ssize_t)msg_len;
		nl->queue_head = (nl->queue_head + 1) % TSB_NL_QUEUE_SIZE;
		nl->queue_count--;
	}
	pthread_mutex_unlock(&nl->mutex);
	N_Tf(nl_recv, "n_msgs_in_q=@INT, cur_msg=@INT[b]", nl->queue_count, (int)len);
	return len;
}

#include "srv/nvmeibs_srv_toma_messages.h"  // For nvmeib_nl_uk_comm_msg, nvmeib_disk_info_reply

// Extract the seq (smart file index) from device name.
// For NVMesh devices like "nvme1001n1", seq = 1001 - 1000 = 1.
// For stock devices like "nvme0n1", returns -1 (no smart file).
static int TSB_get_seq_from_nvmesh_device_name(const char *device_name) {
	int nvme_num;
	if (!device_name || (strncmp(device_name, "nvme", 4) != 0) || !isdigit(device_name[4]))
		return -1;
	nvme_num = atoi(&device_name[4]);
	return (nvme_num < 1000) ? -1 : (nvme_num - 1000);		// NVMesh devices have numbers >= 1000
}

static const struct sandbox_nvme_device *TSB_find_nvmesh_device_by_seq(int seq)
{
	int i;
	int n;
	BUG_ON(seq < 0);

	n = sandbox_nvme_get_device_count();
	for (i = 0; i < n; ++i) {
		const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_index(i);
		int dev_seq;
		if (!dev || dev->stock_disk)
			continue;

		dev_seq = TSB_get_seq_from_nvmesh_device_name(dev->device_name);
		if (dev_seq == seq)
			return dev;
	}
	return NULL;
}

static void reply_usermode_payload(struct nvmeib_nl_uk_comm_msg *omsg, const struct nvmeib_nl_uk_comm_msg *imsg) {		// See real server function implementation
	struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep*)omsg->data;
	rep->opcode = omsg->opcode = imsg->opcode;
	omsg->id = imsg->id;
	omsg->caller_type = imsg->caller_type;
	rep->latency_ns = 576;
}

// Queue a netlink disk info response for a given device
static void TSB_netlink_send_disk_response(const struct sandbox_nvme_device *dev, const struct nvmeib_nl_uk_comm_msg *req_msg) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_disk_info_reply *rep = (struct nvmeib_disk_info_reply *)msg->data;

	reply_usermode_payload(msg, req_msg);
	msg->len = sizeof(*msg) + sizeof(*rep);
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);

	// Fill nvmeib_disk_info_reply
	rep->base.error = csce_ok;
	rep->selector = nvmeib_disk_info_reply_dinfo;

	// Fill disk info from sandbox device, using its current LBA format
	{
		const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(dev->current_format_idx);
		rep->dinfo.disk.n_blocks = dev->size_in_blocks;
		rep->dinfo.disk.n_hw_blocks = dev->size_in_blocks;
		rep->dinfo.disk.vendor_id = dev->vendor_id;
		rep->dinfo.disk.block_size = 1 << lbaf->block_size_exp;
		rep->dinfo.disk.max_request_size = 32;
		rep->dinfo.disk.max_n_hw_sectors = 32;
		rep->dinfo.disk.seq = TSB_get_seq_from_nvmesh_device_name(dev->device_name);
		rep->dinfo.disk.nsid = 1;
		rep->dinfo.disk.metadata = lbaf->metadata_size;
	}
	snprintf(rep->dinfo.disk.disk_id, sizeof(rep->dinfo.disk.disk_id), "%s.1", dev->serial_number);
	snprintf(rep->dinfo.disk.dev_name, sizeof(rep->dinfo.disk.dev_name), "%s", dev->device_path);
	snprintf(rep->dinfo.disk.model_str, sizeof(rep->dinfo.disk.model_str), "%s", dev->model_number);
	snprintf(rep->dinfo.disk.native_serial_str, sizeof(rep->dinfo.disk.native_serial_str), "%s", dev->serial_number);
	snprintf(rep->dinfo.disk.status, sizeof(rep->dinfo.disk.status), "Ok");
	rep->dinfo.serjio_status = 0;  // nvmeibs_serjio_status_ok
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

// Handle csc_io_to_disk requests - perform disk I/O and queue response
static void TSB_netlink_handle_io_to_disk(const struct nvmeib_nl_uk_comm_msg *req_msg) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_io_to_disk_reply *rep = (struct nvmeib_io_to_disk_reply *)msg->data;
	const struct nvmeib_io_to_disk *io_req = (const struct nvmeib_io_to_disk *)req_msg->data;
	const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id(io_req->disk_id);

	reply_usermode_payload(msg, req_msg);
	msg->len = sizeof(*msg) + sizeof(*rep);
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);

	if (!dev) {
		rep->base.error = 1;  // Device not found
		N_Wf(nl_io_err, "IO to unknown disk disk_id=@STR", io_req->disk_id);
	} else {
		// Perform the actual I/O on the sandbox disk file
		const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(dev->current_format_idx);
		int fd = sandbox_nvme_open(dev);
		off_t offset = (off_t)io_req->start_sector * (1 << lbaf->block_size_exp);
		ssize_t result;

		if (io_req->is_read) {
			result = pread(fd, io_req->data, io_req->data_len, offset);
		} else {
			result = pwrite(fd, io_req->data, io_req->data_len, offset);
		}
		close(fd);

		if (result < 0) {
			rep->base.error = 1;
			N_Wf(nl_io_fail, "IO failed disk=@STR is_read=@INT offset=@ZX len=@INT err=@AUTO_ERRNO",
				io_req->disk_id, io_req->is_read, offset, io_req->data_len);
		} else {
			rep->base.error = 0;  // csce_ok
			rep->n_data_io = io_req->data_len;
			rep->n_md_io = 0;
		}
		nvmeib_strlcpy(rep->disk_id, io_req->disk_id, sizeof(rep->disk_id));
		rep->vendor_id = io_req->vendor_id;
	}
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

// Handle csc_zero_disk requests - perform zeroing and queue response
static void TSB_netlink_handle_zero_disk(const struct nvmeib_nl_uk_comm_msg *req_msg)
{
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_zero_disk_reply *rep = (struct nvmeib_zero_disk_reply *)msg->data;
	const struct nvmeib_zero_disk *zreq = (const struct nvmeib_zero_disk *)req_msg->data;
	const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id(zreq->disk_id);
	int rv = 0;

	BUG_ON(!dev);
	reply_usermode_payload(msg, req_msg);
	msg->len = sizeof(*msg) + sizeof(*rep);
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);

	{
		size_t chunk_bytes = 1024 * 1024;
		void *zero_buf = NULL;
		int fd = sandbox_nvme_open(dev);
		const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(dev->current_format_idx);
		size_t block_size = (size_t)1U << lbaf->block_size_exp;
		uint64_t total_bytes = (uint64_t)zreq->n_hw_sectors * (uint64_t)block_size;
		uint64_t offset = (uint64_t)zreq->start_hw_sector * (uint64_t)block_size;

		if (fd < 0) {
			rep->base.error = csce_failed;
			goto out;
		}

		if (chunk_bytes % block_size != 0) {
			chunk_bytes = ((chunk_bytes + block_size - 1) / block_size) * block_size;
		}
		zero_buf = calloc(1, chunk_bytes);
		if (!zero_buf) {
			rep->base.error = csce_failed;
			close(fd);
			goto out;
		}

		while (total_bytes > 0) {
			size_t write_bytes = (total_bytes > chunk_bytes) ? chunk_bytes : (size_t)total_bytes;
			ssize_t w = pwrite(fd, zero_buf, write_bytes, (off_t)offset);
			if (w < 0 || (size_t)w != write_bytes) {
				rep->base.error = csce_failed;
				rv = -1;
				break;
			}
			offset += (uint64_t)write_bytes;
			total_bytes -= (uint64_t)write_bytes;
		}

		free(zero_buf);
		close(fd);
		rep->base.error = (rv == 0) ? csce_ok : csce_failed;
	}

out:
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

// Handle csc_format_disk requests - update device format and queue response
static void TSB_netlink_handle_format_disk(const struct nvmeib_nl_uk_comm_msg *req_msg, const struct nvmeib_format_disk *fmt_disk)
{
	char reply_buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *reply_nlhdr = (struct nlmsghdr *)reply_buf;
	struct nvmeib_nl_uk_comm_msg *reply_msg = NLMSG_DATA(reply_nlhdr);
	struct nvmeib_format_disk_reply *rep = (struct nvmeib_format_disk_reply *)reply_msg->data;
	struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id_mut(fmt_disk->disk_id);
	const int fmt_idx = fmt_disk->format_id.id;

	reply_usermode_payload(reply_msg, req_msg);
	reply_msg->len = sizeof(*reply_msg) + sizeof(*rep);
	reply_nlhdr->nlmsg_len = NLMSG_SPACE(reply_msg->len);

	// Reject inline metadata - NVMesh only supports separate metadata
	BUG_ON(fmt_disk->format_id.is_inline);

	if (!dev) {
		N_Ef(fmt_nodisk, "format_disk: device not found disk_id=@STR", fmt_disk->disk_id);
		rep->base.error = csce_failed;
	} else if (sandbox_nvme_format_disk(dev, fmt_idx) != 0) {
		N_Ef(fmt_failed, "format_disk: format failed disk_id=@STR fmt_idx=@INT", fmt_disk->disk_id, fmt_idx);
		rep->base.error = csce_failed;
	} else {
		const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(fmt_idx);
		N_Tf(fmt_ok, "format_disk: success disk_id=@STR fmt_idx=@INT blk=@INT md=@INT",
			fmt_disk->disk_id, fmt_idx, 1 << lbaf->block_size_exp, lbaf->metadata_size);
		rep->base.error = csce_ok;
		// Fill in the new format info
		snprintf(rep->info.new_dev_file_name, sizeof(rep->info.new_dev_file_name), "%s", dev->device_path);
		rep->info.new_n_pblk = dev->size_in_blocks;
		rep->info.new_seq = TSB_get_seq_from_nvmesh_device_name(dev->device_name);
	}
	TSB_netlink_queue_enqueue(reply_buf, reply_nlhdr->nlmsg_len);
}

static void TSB_netlink_send_extended_msg(void) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_push_extended_msg *rep = (struct nvmeib_push_extended_msg *)msg->data;
	msg->opcode = rep->base.opcode = csc_msg_to_process;
	rep->n_bytes_len = sprintf(&rep->content[0], "%s", "HelloFromClnt");
	msg->len = sizeof(*msg) + sizeof(*rep) + rep->n_bytes_len;
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

/**
 * Format the disks CSV content into a buffer (formerly written to /proc/nvmeibs/disks.csv).
 * Generates CSV with header and one line per NVMesh disk.
 *
 * @param buf Output buffer
 * @param buf_size Size of buffer in bytes
 * @return Number of bytes written (excluding trailing '\0').
 *
 * This helper is used in unit tests; it must fail-fast on invalid arguments or
 * insufficient buffer space.
 */
static int format_disks_csv(char *buf, size_t buf_size)
{
	int device_count = sandbox_nvme_get_device_count();
	struct nvmeib_txt txt;
	struct charvec buffer;
	struct charvec out;

	BUG_ON(buf == NULL);
	BUG_ON(buf_size == 0);

	buffer.base = buf;
	buffer.len = buf_size;
	txt = nvmeib_txt_make(buffer);

	/* Write header */
	nvmeib_txt_append(&txt, "%s\n", NVMEIBS_DISKS_CSV_HEADER);

	/* Write each NVMesh (non-stock) disk */
	for (int i = 0; i < device_count; ++i) {
		const struct sandbox_nvme_device *d = sandbox_nvme_get_device_by_index(i);
		const struct sandbox_nvme_lbaf *lbaf;
		int disk_seq;

		if (!d || d->stock_disk)
			continue;

		disk_seq = TSB_get_seq_from_nvmesh_device_name(d->device_name);
		BUG_ON(disk_seq < 0);
		lbaf = sandbox_nvme_get_lbaf(d->current_format_idx);

		/* CSV: id,blocks,hw_blocks,block_size,max_request_size,seq,nsid,dev_name,metadata,status,vendor,model,native_serial */
		nvmeib_txt_append(&txt,
			"%s.1,%llu,%llu,%u,32,%d,1,/dev/%s,%u,Ok,%d,%s,%s\n",
			d->serial_number,
			(unsigned long long)d->size_in_blocks,
			(unsigned long long)d->size_in_blocks,
			(1u << lbaf->block_size_exp),
			disk_seq,
			d->device_name,
			lbaf->metadata_size,
			d->vendor_id,
			d->model_number,
			d->serial_number);
	}

	out = nvmeib_txt_finalize(&txt);
	BUG_ON(out.base == NULL);     /* buffer overflow/truncation */
	BUG_ON(out.len >= buf_size);  /* no room for trailing '\0' */
	N_Tf(fdc0001, "formatted disks CSV: @STR", buf);
	return (int)out.len;
}

/**
 * Format the smart content into a buffer (formerly written to /proc/nvmeibs/smartX).
 */
 static int format_smart_content(char *buf, size_t buf_size, const struct sandbox_nvme_device *dev)
 {
	 int seq;
	 int n;
	 BUG_ON(!buf);
	 BUG_ON(!dev);

	 seq = TSB_get_seq_from_nvmesh_device_name(dev->device_name);
	 BUG_ON(seq < 0); // should only be called for NVMesh disks -> -1 means stock disk

	 n = snprintf(buf, buf_size,
		 "Pci Address=0000:%02x:00.0\n"
		 "Serial Number=%s\n"
		 "Vendor=0x%04x\n"
		 "Model=%s\n"
		 "Submission Queues=128\n"
		 "Completion Queues=128\n"
		 "MSIX Interrupts=129\n"
		 "Num admin cmds=323\n"
		 "Namespace Id=1\n"
		 "Numa Node=1\n",
		 seq, dev->serial_number, dev->vendor_id, dev->model_number);
	 BUG_ON(n < 0);
	 BUG_ON((size_t)n >= buf_size);

	 N_Tf(fsc0012, "formatted smart content for disk @STR: @STR", dev->device_name, buf);
	 return (int)n;
}

static void TSB_netlink_handle_req_info(const struct nvmeib_nl_uk_comm_msg *req_msg) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_t2s_request_srvr_info_rep *rep =         (struct nvmeib_t2s_request_srvr_info_rep *)msg->data;
	const struct nvmeib_t2s_request_srvr_info_req *req = &((const union nvmeib_nl_msg_to_srvr_payload *)req_msg->data)->req_info;
	rep->base.error = csce_ok;
	if (req->type == NVMEIBS_TOMA_REQ_NICS_CSV) {
		rep->n_bytes_len = sprintf(rep->content, "%s", NVMEIBS_NICS_CSV_HEADER "\n"
			"mlx5_2,0x0000000000000000bae924fffee5cfd8,1,0xffff,R,ACTIVE,4096,4096,3,true,false,true,ens2f0,0x00000000000000000000ffff0a0a0125\n"
			"mlx5_3,0x0000000000000000bae924fffee5cfd9,1,0xffff,R,ACTIVE,4096,4096,3,true,false,true,ens2f1,0x00000000000000000000ffff0a0a0225\n");
	} else if (req->type == NVMEIBS_TOMA_REQ_DISKS_CSV) {
		rep->n_bytes_len = format_disks_csv(rep->content, req->max_byte_len);
	} else if (req->type == NVMEIBS_TOMA_REQ_DISK_SMART_CNT) {
		const int seq = req->opt_arg;
		const struct sandbox_nvme_device *dev;
		dev = TSB_find_nvmesh_device_by_seq(seq);
		BUG_ON(!dev);
		rep->n_bytes_len = format_smart_content(rep->content, (size_t)req->max_byte_len, dev);
	} else {   rep->base.error = csce_dst_not_exist; }
	rep->n_bytes_len++;			// Trailing zero
	rep->was_truncated = rep->n_bytes_len > req->max_byte_len;
	BUG_ON(rep->was_truncated);
	reply_usermode_payload(msg, req_msg);
	msg->len = sizeof(*msg) + sizeof(*rep) + rep->n_bytes_len;
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

static inline enum uk_comm_err_opcode uk_comm_err_from_errno(ssize_t rv) {
	if (rv >= 0) return csce_ok;		// Proc api returns negative value on failure and type of failure in errno
	rv = errno; errno = 0;				// Convert to msg api which does not use errno and works in userspace as well
	if (rv == ENXIO) return csce_dst_not_exist;
	if (rv == EINPROGRESS) return csce_in_progress;
	if (rv == EALREADY) return csce_already_running;
	return csce_failed;
}

static void TSB_netlink_reply_to_blocked_toma(const struct nvmeib_nl_uk_comm_msg *req_msg, int rv) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep *)msg->data;
	BUG_ON((req_msg->opcode != csc_t2s_blocking_msg_other) && (req_msg->opcode != csc_t2s_blocking_msg_to_io_clients));
	reply_usermode_payload(msg, req_msg);
	rep->error = uk_comm_err_from_errno(rv);
	msg->len = sizeof(*msg) + sizeof(*rep);
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

ssize_t sendmsg(int __fd, const struct msghdr *__msg, int __flags) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(__fd);
	struct iovec *iov = (struct iovec *)__msg->msg_iov;
	BUG_ON(__msg->msg_iovlen != 1);			// Assert single iovec element (as used by km_comm)
	return s->other_side->send(__fd, iov[0].iov_base, iov[0].iov_len, OFFSET_NONE, __flags);
}

ssize_t recvmsg(int __fd, struct msghdr *__msg, int __flags) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(__fd);
	struct iovec *iov = (struct iovec *)__msg->msg_iov;
	BUG_ON(__msg->msg_iovlen != 1);	// Assert single iovec element (as used by km_comm)
	return s->other_side->recv(__fd, iov[0].iov_base, iov[0].iov_len, OFFSET_NONE, __flags);
}

ssize_t send(int fd, const void *buf, size_t n , int flags) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	return s->other_side->send(fd, buf, n, OFFSET_NONE, flags);
}

ssize_t recv(int fd,       void *buf, size_t n , int flags) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	return s->other_side->recv(fd, buf, n, OFFSET_NONE, flags);
}

int setsockopt(int fd, int lvl, int name, const void *val, unsigned int optlen) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	(void)s; (void)lvl; (void)name; (void)val; (void)optlen;
	return 0;
}

int listen(int fd, int n) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	(void)s; (void)n;
	return 0;
}

int accept(int fd, struct sockaddr* addr, unsigned int *addr_len) {
	struct t_sandbox_sock *s;
	pthread_mutex_lock(&sys->TS.mutex);
	s = TSB_socket_find_by_fd(fd);
	s->ref_cnt++;
	pthread_mutex_unlock(&sys->TS.mutex);
	(void)addr;  (void)addr_len;
	return fd;
}

int override_open(const char *path, int flags, ... /*int mode*/) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	int ret;
	sprintf(addr.sun_path, "%s", path);
	pthread_mutex_lock(&sys->TS.mutex);
	ret = __connect(socket(0, 'f', flags), &addr, 0);
	pthread_mutex_unlock(&sys->TS.mutex);
	return ret;
}

int override_close(int fd) {
	struct t_sandbox_sock *s;
	pthread_mutex_lock(&sys->TS.mutex);
	s = TSB_socket_find_by_fd_opt(fd);
	if (s)
		socket_destroy(s);
	pthread_mutex_unlock(&sys->TS.mutex);

	if (!s) {
		// This happens during shutdown currently, as Toma closes all fd's before exiting.
		// To get a clean unit test run, we need to handle this gracefully.
		// Also, we can't use the binary trace mechanism here during shutdown, as it gets destroyed first.
		SANDBOX_PRINT("close attempted for invalid fd=%d\n", fd);
		errno = EBADF;
		return -1;
	} else {
		errno = 0;
		return 0;
	}
}

// Create a new TSB entry that mirrors an existing fd.
// Returns the real OS fd, matching the pattern of override_open()/TSB_sock_open().
int override_dup(int oldfd) {
	struct t_sandbox_sock *old_s;
	struct t_sandbox_sock_tbl *TS;
	struct t_sandbox_sock *new_s;
	int new_os_fd = -1;
	int ret = -1;

	old_s = TSB_socket_find_by_fd(oldfd);
	TS = &sys->TS;

	pthread_mutex_lock(&TS->mutex);
	new_s = t_sandbox_sock_tbl_find_next_unused(TS);

	// Copy configuration from original socket
	new_s->dom = old_s->dom;
	new_s->type = old_s->type;
	new_s->proto = old_s->proto;
	new_s->addr = old_s->addr;
	new_s->len = old_s->len;
	new_s->ref_cnt = 1;
	new_s->other_side = old_s->other_side;

	// If there's a real FILE* handle, dup the underlying OS fd
	if (old_s->f != NULL) {
		new_os_fd = dup(fileno(old_s->f));
		if (new_os_fd < 0)
			goto done;

		new_s->f = fdopen(new_os_fd, sbfd_get_open_mode(new_s));
		if (new_s->f == NULL)
			goto done;

		new_s->fd = new_os_fd;
		new_os_fd = -1;  // Ownership transferred to new_s->f
	}

	ret = new_s->fd;
	SANDBOX_PRINT("TSB dup: oldfd=%d -> newfd=%d, path=%s\n", oldfd, ret, new_s->addr.sun_path);

done:
	if (ret < 0) {
		if (new_os_fd >= 0)
			close(new_os_fd);
		memset(new_s, 0, sizeof(*new_s));
	}
	pthread_mutex_unlock(&TS->mutex);
	return ret;
}

int override_fcntl(int fd, int cmd, ...) {
	struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	/*int value = 0;
	va_list ap;
	va_start(ap, cmd);
	value = va_arg(ap, int);
	va_end(ap);*/
	(void)s; (void)cmd;
	return 0;
}

int override_pipe(int fds[2]) {
	struct sockaddr_un addr;
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "wakeup_pipe_pair0");
	fds[0] = __connect(socket(0,0,0), &addr, 16);
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "wakeup_pipe_pair1");
	fds[1] = __connect(socket(0,0,0), &addr, 16);
	return 0;
}

ssize_t override_read(int fd, void *buf, size_t nbytes) {
	struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->recv(fd, buf, nbytes, OFFSET_NONE, 0);
	return read(s->fd, buf, nbytes);	// Use real OS fd for passthrough
}

ssize_t override_write(int fd, const void *buf, size_t count) {
	struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->send(fd, buf, count, 0, 0);
	return write(s->fd, buf, count);	// Use real OS fd for passthrough
}

ssize_t override_pread(int fd,       void *buf, size_t count, off_t offset) {
	struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->recv(fd, buf, count, offset, 0);
	return pread(s->fd, buf, count, offset);	// Use real OS fd for passthrough
}

ssize_t override_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->send(fd, buf, count, offset, 0);
	return pwrite(s->fd, buf, count, offset);	// Use real OS fd for passthrough
}

static void __temp_wait_sleep(void) { nanosleep(&(struct timespec){0, 10*1000*1000}, NULL); /* 100ms */ }

int override_select(int nfds, fd_set *__restrict readfds, fd_set *__restrict writefds, fd_set *__restrict exceptfds, struct timeval *__restrict timeout) {
	struct t_sandbox_sock *nl_sock = sys->TSB_netlink.o.sock;
	struct t_sandbox_sock *ls_sock = sys->TSB_srvr2toma.o.sock;
	const struct TSB_server_comm_wakeup_mock *w = &sys->TSB_km_sock_pair;
	const int nl_fd = nl_sock->fd, ls_fd = ls_sock->fd;
	const bool monitor_nl = FD_ISSET(nl_fd, readfds), monitor_wakup = FD_ISSET(w->o[1].sock->fd, readfds), monitor_ls = FD_ISSET(ls_fd, readfds);
	int n_events, n_iterations;
	BUG_ON(!nl_sock || !readfds || !ls_sock || (nfds <= nl_fd) || (nfds <= w->o[1].sock->fd) || (nfds <= ls_fd));	// Wrong select from Toma production code
	FD_ZERO(readfds); if (writefds) FD_ZERO(writefds); FD_ZERO(exceptfds);
	for (n_events = 0, n_iterations = 0; n_events == 0; n_iterations++) { // Throttled km_comm select, todo, use timeout
		if (monitor_nl && sys->TSB_netlink.o.has_data()) {	// Check if netlink socket is in the read set and we have queued messages, prepared by server_simu_get_next_msg_for_toma
			FD_SET(nl_fd, readfds);
			n_events++;
		}
		if (monitor_wakup && w->o[1].has_data()) { 			// Check if wakeup due to toma sending message
			FD_SET(w->o[1].sock->fd, readfds);				// Toma sends message via netlink
			n_events++;
		}
		if (monitor_ls && sys->TSB_srvr2toma.o.has_data()) { // Check if local servers message arrived
			FD_SET(ls_fd, readfds);
			n_events++;
		}
		if (n_events == 0) {
			__temp_wait_sleep(); (void)timeout;				// Todo: use a real timeout
			if (n_iterations > 3)
				break; 										// Emulate timeout
		}
	}
	N_Tf(nl_select_ready0, "n_events=@INT nl=@BOOL_YN, wu=@BOOL_YN, ls=@BOOL_YN", n_events, FD_ISSET(nl_fd, readfds), FD_ISSET(w->o[1].sock->fd, readfds), FD_ISSET(ls_fd, readfds));
	return n_events;
}

/************************************* Epoll ********************************/
int epoll_create1(int flags) {
	struct globa_epoll *ep = &sys->TSB_epoll;
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_epoll_fd");
	memset(ep->evs, 0, sizeof(ep->evs));
	ep->n_fds = 0;
	return __connect(socket(0,0,flags), &addr, 0);
}

int epoll_ctl(int efd, enum EPOLL_CTL op, int __fd, struct epoll_event *ev) {
	struct globa_epoll *ep = &sys->TSB_epoll;
	BUG_ON(ep->o.sock->fd != efd);
	switch (op) {
		case EPOLL_CTL_ADD: {
			for (int i = 0; i < ep->n_fds; i++)
				BUG_ON(ep->evs[i].__fd == __fd);	// Double add to epoll
			ep->evs[ep->n_fds] = *ev;  ep->evs[ep->n_fds].__fd = __fd; ep->n_fds++;  break;
		}
		case EPOLL_CTL_DEL: ep->n_fds--; memset(&ep->evs[ep->n_fds], 0, sizeof(ep->evs[0])); break;
		case EPOLL_CTL_MOD: default : BUG_ON(true); break;
	}
	return 1;
}

int epoll_wait(int efd, struct epoll_event *evs, int man_events, int __timeout) {
	#define SANDBOX_TERMINATE_AFTER_N_LOOPS 120
	struct globa_epoll *ep = &sys->TSB_epoll;
	static uint64_t loop_idx = 0;
	static bool is_shutting_down = false;
	int i, n_events;
	BUG_ON((ep->o.sock->fd != efd)||(man_events < ep->n_fds)); (void)__timeout;
	__temp_wait_sleep();
	for (i = 0, n_events = 0; i < ep->n_fds; i++) {
		const int fd = ep->evs[i].__fd;
		if (fd != nvmeibt_nm_get_fd()) {	// Todo: Solve this hack!
			const struct TSB_sock_otherside *o = TSB_socket_find_other_side_by_fd(fd);
			if (o->has_data())
				evs[n_events++] = ep->evs[i];
		} else {
			evs[n_events++] = ep->evs[i];
		}
	}
	SANDBOX_PRINT("Toma Sandbox epoll loop %lu%s, n_events=%d\n", loop_idx, is_shutting_down ? " (dying)" : "", n_events); loop_idx++;
	if (loop_idx < SANDBOX_TERMINATE_AFTER_N_LOOPS) {
		sys->TSB_sig.sig = ((loop_idx % 5) == 0) ? SIGCHLD : 0; // Once in a while send a signal to toma to test this mechanism
		if (loop_idx == 9) TSB_netlink_send_extended_msg();		// Once send an extended message to test the flow
		return n_events;
	} else {
		N_IMf(sbexit001, "sandbox shutting down Toma app");
		is_shutting_down = true;
		// sys->TSB_sig.sig = 9;	// Daniel: This seems not to work better than epoll failure
		errno = ENOMEM;
		return -1;				// Simulate shutdown instruction via kafka from mgmt
	}
}

/*********************************************************************/
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
static void __verify_correct_dir(void) {
	DIR *root_dir_exists = opendir(TOMA_ROOT_DIR);
	if (!root_dir_exists) {
		SANDBOX_PRINT(COL_RED "Running toma from wrong directory, root sandbox(%s) is not accessible. aborting!" COL_RESET "\n", TOMA_ROOT_DIR);
		BUG_ON(!root_dir_exists);
	} else {
		closedir(root_dir_exists);
	}
}

static void toma_unitest_env_end(void) {
	// #define DICT_DIR "99bin/*/obj/"
	SANDBOX_PRINT(COL_GREEN "unitest done sys=%p" COL_RESET ". \t\tAnalyze bin logs via:\n"
		"\t" TOMA_BINLOG_DIR "/pager " TOMA_BINLOG_DIR " --toma --color " /* "--dict_preload " DICT_DIR "dict* --fmtlib_preload " DICT_DIR "libfmtrs.so" */ " > z.txt\n"
		"\t\t * If pager is not properly built, run once: ./build-verify.sh\n", sys);
	t_sandbox_all_destroy();
}
void toma_unitest_env_start(bool is_running_as_a_utility, int trace_debug_level) {
	#ifdef NDEBUG
		const char *opt = "RELEASE";
	#else
		const char *opt = "DEBUG";
	#endif
	char *pwd = getcwd(NULL, 0);
	SANDBOX_PRINT(COL_GREEN "unitest(%s) starting sys=%p" COL_RESET " is_util=%d trace=%d, dir=%s\n", opt, sys, is_running_as_a_utility, trace_debug_level, pwd);
	free(pwd);
	__verify_correct_dir();
	atexit(toma_unitest_env_end);
	t_sandbox_all_init(is_running_as_a_utility);
}

void toma_unitest_notify_stop_traces(void) {
	sys->can_use_bin_traces = false;
}
/************************************* logging ********************************/
int init_signal_handling(const char *exe_name) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", _PATH_LOG);
	__connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_signal_%s", exe_name);
	return __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
}

void handle_sig_fd(int signals_fd, void (*fn)(int32_t n, uint64_t addr)) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(signals_fd);
	struct TSB_signals_queue *tsb_q = container_of(s->other_side, struct TSB_signals_queue, o);
	if (tsb_q->sig) {
		fn(tsb_q->sig, 0x12345);
	}
}

int nvmeibt_nonblock_fd(int fd) {
	const struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	(void)s;
	return 0;
}

/************************************* nvme ***********************************/
#include "interfaces/nvme/nvmeibt_udev.h"
int  nvmeibt_udev_create(void) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_udev_monitor");
	return __connect(socket(0,0,0), &addr, 0);
}

void nvmeibt_udev_destroy(void) {
	struct t_sandbox_sock *s = (struct t_sandbox_sock *)sys->TSB_udev.o.sock;
	socket_destroy(s);
}

int nvmeibt_udev_get_fd( void) {
	return sys->TSB_udev.o.sock->fd;
}
enum nvmeibt_disk_type nvmeibt_udev_get_event(struct nvmeibt_udev_event *rv) { memset(rv, 0, sizeof(*rv)); return NVMEIBT_NVME_DISK_TYPE; }
void nvmeibt_udev_put_event(struct nvmeibt_udev_event *rv) { memset(rv, 0, sizeof(*rv));}

/************************************* network ********************************/
int64_t ibud_enable_periodic_traces = 0;
int64_t udp_max_header_length = 128;

int rsrm_init_work_tmq(void) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_srm_timer");
	return __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
}

void rsrm_resend_acks(void){}
int  rsrm_get_fd_timer(void){ return sys->TSB_srm_timer.o.sock->fd; }

int rsrm_resend_timer(void){ return 0;}
int rsrm_faults_init_fifo_comm(void) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_srm_fault_fifo");
	return __connect(socket(0,0,0), &addr, 0);
}
int rsrm_faults_get_fd(void) {
	return sys->TSB_srm_fault.o.sock->fd;
}

void rsrm_faults_handle_fifo_comm(void) {}

/************************************* Kafka ********************************/
struct rd_kafka_topic_conf_s {
	int dummy;
};
rd_kafka_topic_conf_t* rd_kafka_topic_conf_new(void) {
	return calloc(1, sizeof(rd_kafka_topic_conf_t));
}
void rd_kafka_topic_conf_destroy(rd_kafka_topic_conf_t *conf) { free(conf); }

struct rd_kafka_topic_s {
	char *name;
	rd_kafka_topic_conf_t* conf;
	int64_t commited_offset, cur_offset, last_offset;
	// Todo: Linked list of messages for offsets above cur,cur+1,....last_offset
	int32_t partition;		// Support only 1 partition for now. Store its index
	bool is_active;
	char type;				// For fast comparison, maybe use enum?
	int temp_store_offset;	// Daniel, not sure is needed - just for two stage store and commit.
};

struct rd_kafka_conf_s {
	char *group_id;
	bool enable_ssl;
};

struct rd_kafka_s {
	char* name;
	int log_lvl;
	enum rd_kafka_type_t who;
	rd_kafka_conf_t* conf;
	struct rd_kafka_topic_s topic;
};

static inline void __rd_kafka_topic_verify_valid(rd_kafka_topic_t *kt, int32_t partition) {
	BUG_ON((partition != kt->partition) || (!kt->is_active));
}

const char* rd_kafka_topic_name(const rd_kafka_topic_t *kt) {
	return kt->name;
}

void rd_kafka_topic_destroy(rd_kafka_topic_t *kt) {
	if (kt->conf) {
		rd_kafka_topic_conf_destroy(kt->conf);
		kt->conf = NULL;
	}
	free(kt->name);
	memset(kt, 0, sizeof(*kt));
}

rd_kafka_resp_err_t rd_kafka_offset_store(rd_kafka_topic_t *kt, int32_t partition, int64_t offset) {
	__rd_kafka_topic_verify_valid(kt, partition);
	BUG_ON((offset <= kt->commited_offset) || (offset > kt->cur_offset));		// Todo: Maybe off by 1 here
	kt->temp_store_offset = offset;
	// Todo, drop messages for previous offsets from the list.
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

void rd_kafka_consume_stop(rd_kafka_topic_t *kt, int32_t partition) {
	__rd_kafka_topic_verify_valid(kt, partition);
	kt->is_active = false;
}

static void __reset_offset(rd_kafka_topic_t *kt, int64_t offset) {
	BUG_ON(offset <= 0);
	kt->commited_offset = offset;			// Start from some non zero number
	kt->last_offset = kt->cur_offset = (kt->commited_offset + 1);
}

rd_kafka_resp_err_t rd_kafka_consume_start(rd_kafka_topic_t *kt, int32_t partition, int64_t offset) {
	kt->is_active = true;
	__rd_kafka_topic_verify_valid(kt, partition);
	if ((offset == RD_KAFKA_OFFSET_STORED) || (offset == RD_KAFKA_OFFSET_BEGINNING)) {
		// Tome relies on Kafka simulator
	} else {
		BUG_ON(offset < kt->cur_offset);		// Toma should consume messages from the start or from its persistency
		if (offset > kt->cur_offset)
			__reset_offset(kt, offset);			// Our kafka simulator does not have persistency over destroy and reinit, so just use what toma said
	}
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_assign(rd_kafka_t *ko, const rd_kafka_topic_partition_list_t *pl) {
	if (pl == NULL) {
		if (ko->topic.name) {
			rd_kafka_consume_stop(&ko->topic, ko->topic.partition);
		} // Topic was never created
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	} else {
		if (!pl->elems[0].k) {
			((rd_kafka_topic_partition_t*)&pl->elems[0])->k = ko;
			rd_kafka_topic_new(ko, pl->elems[0].topic, NULL);
		}
		BUG_ON(ko != pl->elems[0].k);
		return rd_kafka_consume_start(&ko->topic, ko->topic.partition, pl->elems[0].offset);
	}
}

rd_kafka_resp_err_t rd_kafka_assignment (rd_kafka_t *ko, rd_kafka_topic_partition_list_t **pl) {
	*pl = NULL;
	if (!ko->topic.is_active)
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	return RD_KAFKA_RESP_ERR__RETRY;		// Not implemented yet
}

static void __rd_kafka_topic_init(rd_kafka_topic_t *kt, const char* name, rd_kafka_topic_conf_t* conf) {
	BUG_ON((kt->name != NULL) || (kt->is_active));
	kt->name = strdup(name);
	kt->conf = conf;
	__reset_offset(kt, 6);
	kt->partition = 0;
	kt->is_active = false;
}

rd_kafka_resp_err_t rd_kafka_consumer_close(rd_kafka_t *rk) {
	rk->topic.is_active = false;
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_poll_set_consumer(rd_kafka_t* me) { (void)me; return RD_KAFKA_RESP_ERR_NO_ERROR; }
const char*         rd_kafka_name(   const rd_kafka_t* me) { return me->name; }
void                rd_kafka_set_log_level(rd_kafka_t* me, int lvl) { me->log_lvl = lvl; }
void                rd_kafka_flush(        rd_kafka_t* me, int x) { (void)me; (void)x;}
rd_kafka_resp_err_t rd_kafka_unsubscribe(  rd_kafka_t* me) { (void)me;return RD_KAFKA_RESP_ERR_NO_ERROR; }
int                 rd_kafka_poll(         rd_kafka_t* me, bool is_blocking) { (void)me; (void)is_blocking; return 0; }
rd_kafka_resp_err_t rd_kafka_commit(rd_kafka_t* me, rd_kafka_topic_partition_list_t* pl, int is_async) {
	const int64_t last_consumed = (pl->elems[0].offset - 1);
	BUG_ON(me != pl->elems[0].k);
	BUG_ON((last_consumed >= me->topic.cur_offset));		// Todo: Maybe off by 1 here
	me->topic.commited_offset = max(last_consumed, me->topic.commited_offset);
	//SANDBOX_PRINT_TMP("%s: Commit %lu\n", me->topic.name, me->topic.commited_offset);
	(void)is_async;
	sys->kafka_simu.notify_consumer_offset_commit(me, RD_KAFKA_RESP_ERR_NO_ERROR, pl, NULL);
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_committed(rd_kafka_t *me, rd_kafka_topic_partition_list_t *pl, int timeout_ms) {
	BUG_ON(me != pl->elems[0].k);
	BUG_ON(timeout_ms < 1000);
	pl->elems[0].offset = me->topic.cur_offset;
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}
char* rd_kafka_err2str(rd_kafka_resp_err_t e) { (void)e; return "kerr"; }
char* rd_kafka_err2name(rd_kafka_resp_err_t e) { (void)e; return "kerr"; }
rd_kafka_resp_err_t rd_kafka_last_error(void) { return RD_KAFKA_RESP_ERR_NO_ERROR; }
rd_kafka_conf_t* rd_kafka_conf_new(void) { return calloc(1, sizeof(rd_kafka_conf_t)); }
void rd_kafka_conf_destroy(rd_kafka_conf_t* me) { free(me); }
void rd_kafka_message_destroy(rd_kafka_message_t*msg) { free(msg->payload); free(msg); }
void rd_kafka_conf_set_error_cb( rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, int err, const char *reason, void *opaque)) { (void)kc; (void)fn; }
rd_kafka_resp_err_t rd_kafka_purge(rd_kafka_t * rk, int purge_flags) { (void)rk; (void)purge_flags; return RD_KAFKA_RESP_ERR_NO_ERROR; }

void rd_kafka_destroy(rd_kafka_t* k) {
	struct kafka_simulator_t *ks = &sys->kafka_simu;
	int i;
	for (i = 0; i < ks->n_obj; i++) {
		if (ks->obj[i] == k) {
			free(k->name);
			free(k->conf);
			rd_kafka_topic_destroy(&k->topic);
			free(k);
			ks->obj[i] = NULL;
			while ((ks->n_obj > 0) && (ks->obj[ks->n_obj-1] == NULL))		// Shrink the array
				ks->n_obj--;
			return;
		}
	}
	BUG_ON(i >= ARRAY_SIZE(ks->obj) || (!k));
}

static bool is_kafka_cp_used(const rd_kafka_t* o) {
	return (o->name != NULL);
}

static rd_kafka_t* kafka_simu_find_next_unused(struct kafka_simulator_t *ks) {
	rd_kafka_t *k;
	int i;
	for (i = 0; i < ks->n_obj; i++) {		// Reuse deleted
		if (ks->obj[i] == NULL)
			ks->obj[i] = calloc(1, sizeof(*k));
		if (!is_kafka_cp_used(ks->obj[i]))
			return ks->obj[i];
	}
	k = ks->obj[ks->n_obj++] = calloc(1, sizeof(*k));
	BUG_ON(i >= ARRAY_SIZE(ks->obj) || is_kafka_cp_used(k));
	return k;
}

static rd_kafka_t* kafka_simu_find_by_parition_name(const char* name) {
	struct kafka_simulator_t *ks = &sys->kafka_simu;
	int i;
	for (i = 0; i < ks->n_obj; i++) {
		rd_kafka_t *k = ks->obj[i];
		if (is_kafka_cp_used(k) && k->topic.name && !strcmp(k->topic.name, name))
			return k;
	}
	return NULL;
}

static rd_kafka_t* kafka_simu_find_by_topic(rd_kafka_topic_t *kt) {
	struct kafka_simulator_t *ks = &sys->kafka_simu;
	int i;
	for (i = 0; i < ks->n_obj; i++) {
		if (is_kafka_cp_used(ks->obj[i]) && (&ks->obj[i]->topic == kt))
			return ks->obj[i];
	}
	BUG_ON(true);
	return NULL;
}

rd_kafka_t* rd_kafka_new(enum rd_kafka_type_t who, rd_kafka_conf_t *cfg, char*err_str, size_t size_of_err) {
	struct kafka_simulator_t *ks = &sys->kafka_simu;
	rd_kafka_t *k = kafka_simu_find_next_unused(ks);
	if (who == RD_KAFKA_CONSUMER) {
	} else {	// RD_KAFKA_PRODUCER
	}
	k->conf = cfg;
	k->name = cfg->group_id;
	k->who = who;
	/* Set error 0 */ BUG_ON(size_of_err < 16); err_str[0] = 0;
	return k;
}

rd_kafka_topic_t* rd_kafka_topic_new(rd_kafka_t *k, const char* name, rd_kafka_topic_conf_t* conf) {
	BUG_ON(!is_kafka_cp_used(k));
	__rd_kafka_topic_init(&k->topic, name, conf);
	k->topic.is_active = true;
	k->topic.type = '?';
	if (k->who == RD_KAFKA_PRODUCER) {
		if (strstr(name, "management.priority."))
			k->topic.type = 'P';
		else if (strstr(name, "management.keepalive."))
			k->topic.type = 'K';
		else if (strstr(name, "management.low."))
			k->topic.type = 'L';
		else BUG_ON(true);				// unknown topic which management simulator will not listen too
	}
	return &k->topic;
}

rd_kafka_topic_partition_list_t* rd_kafka_topic_partition_list_new(int n) {
	rd_kafka_topic_partition_list_t* rv = calloc(1, sizeof(rd_kafka_topic_partition_list_t));
	BUG_ON(n != 1);
	rv->cnt = 1;
	rv->size = sizeof(*rv);
	return rv;
}

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add(rd_kafka_topic_partition_list_t *pl, const char* name, int32_t partition) {
	rd_kafka_topic_partition_t *p = &pl->elems[0];
	rd_kafka_t* k = kafka_simu_find_by_parition_name(name);
	BUG_ON(partition != 0);				// Support only 1 partition
	if (k) {
		BUG_ON(!k || p->k);		// Must add valid pointer and only 1
		p->k = k;
	}
	p->partition = partition;
	p->offset = RD_KAFKA_OFFSET_INVALID;
	p->topic = name;
	return p;
}

void rd_kafka_topic_partition_list_destroy(rd_kafka_topic_partition_list_t* pl) {
	free(pl);
}

rd_kafka_resp_err_t rd_kafka_query_watermark_offsets(rd_kafka_t *me, const char *str, int32_t partition, int64_t *low_oldest_beginning_offset, int64_t *high_newest_end_offset, int timeout) {
	BUG_ON(strcmp(me->topic.name, str));
	BUG_ON(me->topic.partition != partition);					// Only 1 partition
	(void)timeout;
	*low_oldest_beginning_offset = me->topic.cur_offset;
	*high_newest_end_offset =  me->topic.last_offset + 17;		// +17 is just for fun, meaningless
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_position(rd_kafka_t *k, rd_kafka_topic_partition_list_t *pl) {
	BUG_ON(k != pl->elems[0].k);
	pl->elems[0].offset = k->topic.cur_offset;
	pl->elems[0].offset = RD_KAFKA_OFFSET_INVALID; 	// Simulate as if it is unsupported
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

void rd_kafka_conf_set_dr_msg_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, const rd_kafka_message_t *kmsg, void *opaque)) {
	sys->kafka_simu.notify_producer_msg_accepted = fn;
	(void)kc;
}

void rd_kafka_conf_set_rebalance_cb(rd_kafka_conf_t* kc, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque)) {
	(void)kc; (void)fn;
}
void rd_kafka_conf_set_offset_commit_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque)) {
	sys->kafka_simu.notify_consumer_offset_commit = fn;
	(void)kc;
}

int rd_kafka_produce(rd_kafka_topic_t *kt, int32_t partition, int msgflags, void *payload, size_t len, const void *key, size_t keylen, void *msg_opaque) {
	static int fail_once_every = 0;
	rd_kafka_t *ko = kafka_simu_find_by_topic(kt);
	rd_kafka_message_t km;
	km._private = msg_opaque;
	km.err = (fail_once_every++ % 3) ? 0 : RD_KAFKA_RESP_ERR__TIMED_OUT;		// Once every few messages fail completion
	BUG_ON((partition != RD_KAFKA_PARTITION_UA) || (len == 0) || ((key == NULL) != (keylen == 0)));
	(void)msgflags;
	if (0) SANDBOX_PRINT("> %d > |%s|  :  |%s|\n", fail_once_every, (const char*)key, (const char*)payload);

	if (kt->type == 'P') {
		mgmt_sim_on_toma_produced(payload, len);
	} else if (kt->type == 'K') {
		// Todo: handle keepalives
	} else if (kt->type == 'L') {
		// Todo: handle drive zeroing reports here
	} else { BUG_ON(true);	}

	// No, put this on to kt, in a list and then poll_cb will return the callbacks
	sys->kafka_simu.notify_producer_msg_accepted(ko, &km, NULL);
	// Todo: Here, submit msg to management simulator
	errno = 0;
	return 0;
}

rd_kafka_resp_err_t rd_kafka_fatal_error(rd_kafka_t *k, char *errstr, size_t errstr_size) {
	(void)k; (void)errstr_size;
	errstr[0] = 0;
	return RD_KAFKA_RESP_ERR_NO_ERROR;	// Or RD_KAFKA_RESP_ERR__FATAL???
}

rd_kafka_conf_res_t rd_kafka_conf_set(rd_kafka_conf_t *kc, const char *key, const char *val, char* err_str, size_t size_of_err) {
	 BUG_ON(!kc || !key || !val);
	if (!strcmp(key, "group.id") || !strcmp(key, "client.id")) {
		if (!kc->group_id)
			kc->group_id = strdup(val);
	} else if (strstr(key, "ssl.") != 0) {
		kc->enable_ssl = true;
	}
	// SANDBOX_PRINT("KAFKA_SIMU::conf_set(): %p) %s=%s\n", kc, key, val);
	/* Set error 0 */ BUG_ON(size_of_err < 16); err_str[0] = 0;
	return RD_KAFKA_CONF_OK;
}

/* Management simulation message templates are now in mgmt_sim.c.
 * Mgmt sandbox receives those msg from toma and parses them properly
 {"originType":"TOMA","messageType":"leaderKeepalive","messageTypeVersion":1,"hostname":"nvme39.mec01.nbulabs.nvidia.com","tomaToken":2,"messageSequence":24312,"leaderToken":1,"keepaliveInterval":5,"payload":{"raftTerm":10,"zone":"1","featureCompatibilityVersion":"0","tomaSoftwareVersion":"784","version":"3.3.0-1332","buildNumber":""}}
 {"originType":"TOMA","messageType":"keepalive","messageTypeVersion":2,"hostname":"nvme34.mec01.nbulabs.nvidia.com","tomaToken":2,"messageSequence":11831,"leaderToken":null,"keepaliveInterval":5,"payload":{"zone":"1","leaderUUID":"nvme39.mec01.nbulabs.nvidia.com","bootTime":1767279770145,"featureCompatibilityVersion":"0","tomaSoftwareVersion":"784","version":"3.3.0-1332","buildNumber":"","rebuildStats":{"nRunningDirtyRebuild":0,"nPendingDirtyRebuild":0,"nRunningStaleRebuild":0,"nPendingStaleRebuild":6,"nRunningTxidRebuild":0,"nPendingTxidRebuild":0,"nRunningColdRecovery":0,"nPendingColdRecovery":0,"nRunningJGCRebuild":0,"nPendingJGCRebuild":0,"nRunningScrubbing":0,"nPendingScrubbing":3}}}
 {"originType":"TOMA","messageType":"driveZeroingProgress","messageTypeVersion":1,"hostname":"nvme37.mec01.nbulabs.nvidia.com","tomaToken":2,"messageSequence":27,"leaderToken":null,"payload":{"zeroWriteCounter":303100870554,"nZeroedBlks":1953536,"diskUUID":"de2eadb0-e972-11f0-8e2a-434e55e1d7f7","node_id":"nvme37.mec01.nbulabs.nvidia.com"}}
 {"originType":"TOMA","messageType":"reportTarget","messageTypeVersion":1,"hostname":"nvme37.mec01.nbulabs.nvidia.com","tomaToken":2,"messageSequence":24,"leaderToken":null,"payload":{"node":{"zone":"1","bootTime":1767535819492,"cpu_temp":"30.0","version":"3.3.0-1340","buildNumber":"","reportID":25,"branch":"master","commit":"18af9e759c828cc1d3776bfdd6cb68ade54ef738","configProfile":{"id":"569407c0-e976-11f0-b6cb-871ca30885b4","name":"Cluster Default","version":"1"},"node_status":"1","node_id":"nvme37.mec01.nbulabs.nvidia.com","targetUpdatesSequence":4,"cpu_load":"0.0","disks":[{"diskID":"S3HCNX0JC01918.1","disk_version":0,"blocks":1562500000,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01918","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"1_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5f","Power_On_Hours":"0x10049","Unsafe_Shutdowns":"0x56","Media_Errors":"0x2","Number_of_Error_Information_Log_Entries":"0x1a5d","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":25439882564,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700220.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700220","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xb4","Power_On_Hours":"0xfb7f","Unsafe_Shutdowns":"0x94","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x5980","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4546392587,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0JC01904.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01904","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"2_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5e","Power_On_Hours":"0x10046","Unsafe_Shutdowns":"0x54","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x2beb","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":49694267592,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700164.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700164","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x6e","Power_On_Hours":"0x1062c","Unsafe_Shutdowns":"0x5c","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x46","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4675551222,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0K600397.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0K600397","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xca","Power_On_Hours":"0xe767","Unsafe_Shutdowns":"0xb5","Media_Errors":"0x3","Number_of_Error_Information_Log_Entries":"0x165c","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":29489346822,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"disID":"S4C9NF0M500226.1","disk_version":0,"blocks":3125627568,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S4C9NF0M500226","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL1T6HAJQ-00005","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"4_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x56","Power_On_Hours":"0xda5d","Unsafe_Shutdowns":"0x49","Media_Errors":"0x57","Number_of_Error_Information_Log_Entries":"0x1c5a","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":303100870572,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S665NE0R702075.1","disk_version":0,"blocks":1875385008,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S665NE0R702075","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZ1L2960HCJR-00A07","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"3_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x70","Power_On_Hours":"0x817d","Unsafe_Shutdowns":"0x53","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x0","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":10392940702,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0}],"nics":[{"nicID":"0x0000000000000000bae924fffee5cfd8","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0125","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_2"},{"nicID":"0x0000000000000000bae924fffee5cfd9","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0225","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_3"}]}}}
 {"originType":"TOMA","messageType":"updatePRaidReport","messageTypeVersion":1,"hostname":"nvme39.mec01.nbulabs.nvidia.com","tomaToken":2,"messageSequence":85,"leaderToken":1,"payload":{"pRaidsUpdate":[{"uuid":"b60b04b1-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60b04b0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60b2bc0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]},{"uuid":"b60ab692-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60ab691-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60adda0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]}]}}
 {"originType":"TOMA","messageType":"updateDiskSegmentsDirtyBits","messageTypeVersion":1,"hostname":"nvme37.mec01.nbulabs.nvidia.com","tomaToken":2,"messageSequence":97,"leaderToken":null,"payload":{"segmentsDirtyBitsUpdate":[{"pRaidMinorVersion":2,"pRaidMajorVersion":259,"segmentID":"c1105e50-e97b-11f0-995c-3792ee0db955","pRaidUUID":"c1103742-e97b-11f0-995c-3792ee0db955","remainingDirtyBits":67932,"reappearingCounter":3}]}}
 {"originType":"TOMA","messageType":"segmentZeroingProgress","messageTypeVersion":1,"hostname":"nvme34.mec01.nbulabs.nvidia.com","tomaToken":2,"messageSequence":335,"leaderToken":null,"payload":{"praidVersion":"258.0","segmentUUID":"98e46d20-ea22-11f0-bad8-af65dd8e6ead","pRaidUUID":"98e44612-ea22-11f0-bad8-af65dd8e6ead","nZeroedBlks":262144}}
  */

rd_kafka_message_t* rd_kafka_consumer_poll(rd_kafka_t *ko, int timeout_ms) {
	rd_kafka_message_t *m = calloc(1, sizeof(*m));
	size_t len = 0;
	BUG_ON((timeout_ms != 0) || (!ko->topic.is_active));
	m->err = RD_KAFKA_RESP_ERR_NO_ERROR;

	/* Delegate message selection to the management simulator */
	m->payload = mgmt_sim_next_kafka_payload(ko->name, &len);
	m->len = len;

	if (m->payload == NULL) {			// No message prepared to current consumer
		free(m);
		return NULL;
	}
	m->offset = ko->topic.cur_offset;
	ko->topic.last_offset = ++ko->topic.cur_offset;
	m->_private = NULL;
	return m;
}

rd_kafka_message_t* rd_kafka_consume(rd_kafka_topic_t* kt, int32_t partition, int timeout_ms) {
	BUG_ON(0 != partition);
	return rd_kafka_consumer_poll(kafka_simu_find_by_topic(kt), timeout_ms);
}

int nvmeibt_ib_common_device_uuid_str_to_raw(union ibv_gid *ibv_gid, const char *device_uuid_str) {		// Todo: No, do not reimplement, use production code!
	int i;
	char gid[3];
	if (strlen(device_uuid_str) != 32) {
		N_Tf(tibc_dustr_t2, "Bad device_uuid_str '@DEVICE_UUID_STR'", device_uuid_str);
		return -1;
	}
	gid[2] = '\0';
	for (i = 0; i < 16; ++i) {
		memcpy(gid, device_uuid_str + i * 2, 2);
		ibv_gid->raw[i] = strtoul(gid, NULL, 16);
	}
	return 0;
}

