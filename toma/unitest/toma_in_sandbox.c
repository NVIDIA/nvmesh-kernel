#define TOMA_SANDBOX_BYPASS_REDIRECTS  // allow calling real OS I/O functions from this module

#include "nvmeibt_debug.h"
#include "toma_in_sandbox.h"
#include "sandbox_nvme.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

/************************************* Logging ********************************/

#define SANDBOX_PRINT(fmt, ...) fprintf(stderr, "SANDBOX: " fmt, __VA_ARGS__)
#define SANDBOX_PRINT_TMP(fmt, ...)  fprintf(stderr, "SANDBOX: " COL_PURPL fmt COL_RESET, __VA_ARGS__)
#define FILE_SANDBOX_PREFIX TOMA_ROOT_DIR "var/run/nvmesh/sandbox_fd_"

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

#define BUG_ON(condition)	do { const int hit__ = !!(condition); if (hit__) {fprintf(stderr, "************************** BUG!!!! at %s, %s() line %d, val=%d, condition=%s\n", __FILE__, __FUNCTION__, __LINE__, hit__, #condition); raise(SIGABRT);} } while(0)
//#define WARN(condition, fmt, ...) 	do { const int hit = !!(condition); if (hit) {/*dump_stack(); */SANDBOX_PRINT("************************** BUG!!!! at %s() line %d, val=%d, condition=%s\n", __FUNCTION__, __LINE__, hit, #condition); raise(SIGABRT);} } while(0)

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
};

struct TSB_server_toma_status_req_simu *TSB_server_toma_status_req_simu_get(void);

void TSB_server_toma_status_req_simu_init(struct TSB_server_toma_status_req_simu *me) {
	me->max_reply_length_bytes = 64;				// Ask to fill at most 64[b] of reply, currently not verifying the reply itself
}

void TSB_server_toma_status_req_simu_destroy(struct TSB_server_toma_status_req_simu *me) {
	BUG_ON(me->expecting_reply_cookie);				// Did not get a reply from Toma
	// Only check for replies if we sent messages (standalone utilities like gpt_util don't communicate with TOMA)
	if (me->n_srvr_msg_idx > 0) {
		BUG_ON(me->n_toma_replies_received <= 0);	// Coverage tests did not receive any reply from Toma
	}
}

ssize_t server_simu_get_next_msg_for_toma(int fd, void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_toma_status_req_simu *me = TSB_server_toma_status_req_simu_get();
	struct nvmeibs_toma_server_proc_buf *msg_buf = (void*)buf;
	(void)fd; (void)offset; (void)flags;
	BUG_ON(offset != OFFSET_NONE);
	BUG_ON(n <= sizeof(struct nvmeibs_toma_server_proc_buf));
	me->n_srvr_msg_idx++;
	if (me->n_srvr_msg_idx == 3) {
		struct nvmeibs_msg_s2t_launch_JGC *pl = &msg_buf->trigger_JGC_cmd;
		msg_buf->type = NVMEIBS_TOMA_TRIGGER_JGC;
		strcpy(pl->disk_segment_urn_uuid_str, "todo_disk_seg");
		strcpy(pl->disk_id_str, "todo_disk_id");
		// Currently not expecting reply.
	} else if (me->n_srvr_msg_idx == 5) {
		struct nvmeibs_msg_s2t_toma_status_req *pl = &msg_buf->status_req_msg;
		BUG_ON(me->expecting_reply_cookie);			// Still waiting for previous reply
		me->expecting_reply_cookie = 0x1000 + me->n_srvr_msg_idx;
		msg_buf->type = NVMEIBS_TOMA_WRITE_STATUS_REQ;
		pl->type = NVMEIBS_TOMA_STATUS_RAFT;
		pl->handle = 0 - me->expecting_reply_cookie;
		pl->handle_req = me->expecting_reply_cookie;
		strcpy(pl->fname, "placeholder.tmp");		// In real life should be 1 of toma_stat_proc_fname[]. We use 1 dedicated file to replace them all
		pl->max_length = me->max_reply_length_bytes;
	}
	return sizeof(struct nvmeibs_toma_server_proc_buf);
}

static ssize_t _srvr_simu_from_toma_recv_msg(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_toma_status_req_simu *me = TSB_server_toma_status_req_simu_get();
	const struct nvmeibs_toma_server_proc_buf *m = buf;
	BUG_ON((fd < 2) || (n < sizeof(*m)));
	(void)buf; (void)offset; (void)flags;
	switch (m->type) {
		case NVMEIBS_TOMA_LOGIN:  SANDBOX_PRINT("SRVR_SIMU->Got: Toma_Hello %lu[b]\n", n); break;
		case NVMEIBS_TOMA_LOGOUT: SANDBOX_PRINT("SRVR_SIMU->Got: TomaByeBye %lu[b]\n", n); break;
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

static ssize_t _rpc_inject(int fd, void *buf, size_t n, off_t offset, int flags) {
	static int n_rpcs_sent = 0;	// Todo: Here toma_rpc exe simulator should actually hold a list of rpcs and unitest env can add to it
	static const char* cmds[] = { "simulate dump_status\n", "simulate reread_conf\n",
		"simulate dump-clnt-hash 20\n", "simulate bm-garbage-collect 1\n", "simulate resend-praids-report vol1\n", "status\n", "status server_csvs\n"}; // Todo: This should be a linked list to which unit-test env injects rpc and toma extracts them 1 by 1.
	const bool only_checking = (flags & MSG_PEEK);
	(void)fd;
	(void)offset;
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
	struct t_sandbox_sock *sock;								// Pointer to the socket structure which uses me
};

struct t_sandbox_sock_tbl {
	int n_socks;
	int debug_offset;		// Prevent confusion between real descriptors and emulated
	struct t_sandbox_sock {
		FILE *f;
		int fd;				// File descriptor associated with the socket
		int dom;
		int type;
		int proto;
		u32 len;
		int ref_cnt;								// Same fd' is sometimes use multiple times by the simulator. accept() / pipe(). Todo, clean this
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
		struct TSB_sock_otherside o;
		int fds[2];
	} TSB_wake_pip;
	struct globa_epoll {
		struct TSB_sock_otherside o;
		struct epoll_event evs[16];
		int n_fds;
	} TSB_epoll;
	struct kafka_simulator_t {
		rd_kafka_t *obj[10];
		int n_obj;
		void (*notify_producer_msg_accepted)(rd_kafka_t *rk,const rd_kafka_message_t *kmsg, void *opaque);
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
	char my_hostname[64];
} *sys;

void t_sandbox_all_init(void) {
	sys = calloc(1, sizeof(*sys));
	sys->TS.debug_offset = 10000;
	gethostname(sys->my_hostname, sizeof(sys->my_hostname) - 1);
	pthread_mutex_init(&sys->TSB_netlink.mutex, NULL);
	sandbox_nvme_init();
	TSB_server_toma_status_req_simu_init(&sys->s_req_simu);
}

void t_sandbox_all_destroy(void) {
	TSB_server_toma_status_req_simu_destroy(&sys->s_req_simu);
	pthread_mutex_destroy(&sys->TSB_netlink.mutex);
	BUG_ON(sys->TSB_netlink.n_recv_msgs <= 0);
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
	if (fd > sys->TS.debug_offset)
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
	fprintf(stderr, "sandbox: no such fd %d\n", fd);
	abort();
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
		BUG_ON(!nvme_dev);
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
				struct nvme_id_ns *response = (void*)cmd->addr;
				BUG_ON(cmd->data_len < sizeof(*response));
				memset(response, 0, cmd->data_len);
				response->flbas = 5; // Choosing index 5 arbitrarily. Range is 0..15.
				response->lbaf[5].ds = SANDBOX_NVME_BLOCK_SIZE_EXPONENT; // LBA data size (logical sector size) as exponent of 2
				// Note that LBAF { ms, ds, rp } are defined in NVM-Express-NVM-Command-Set-Specification-Revision-1.2-2025.08.01
				// Figure 116: LBA Format Data Structure, NVM Command Set Specific (PDF p. 91).
				response->nsze = nvme_dev->size_in_blocks;
				N_Tf(sbk5443, "ioctl:nvme:id storage ns=@INT fd=@INT reporting ds=4K nsze=@INT64_TD", cmd->nsid, fd, response->nsze);
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
		// Return block size (4096 bytes)
		int *block_size = va_arg(ap, int*);
		*block_size = 4096;
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
static bool TSB_netlink_queue_is_empty(void);
static ssize_t TSB_netlink_queue_dequeue(void *buf, size_t buf_size);
static void TSB_netlink_send_disk_response(const struct sandbox_nvme_device *dev, const struct nvmeib_nl_uk_comm_msg *req_msg);

// Netlink send callback: Toma sends a request, we parse it and queue responses
static ssize_t _netlink_recv_msg_from_toma(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_netlink_mock *nl = &sys->TSB_netlink;
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
	const struct nvmeib_nl_uk_comm_msg *req_msg = NLMSG_DATA(nlh);

	(void)fd; (void)offset; (void)flags;
	BUG_ON(n < sizeof(struct nlmsghdr));
	nl->n_recv_msgs++;
	N_Tf(nl_send, "opcode=@INT, total_n_msgs=@INT", req_msg->opcode, nl->n_recv_msgs);
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
	} else {
		BUG_ON(true);		// Not implemented yet in sandbox
	}

	return (ssize_t)n;
}

// Netlink recv callback: Toma receives a response from the queue
static ssize_t _netlink_reply_to_toma(int fd, void *buf, size_t n, off_t offset, int flags) {
	const ssize_t len = TSB_netlink_queue_dequeue(buf, n);
	(void)fd; (void)offset; (void)flags;
	return len;
}

// Connect the other side which communicates with Toma
void TSB_connect_sock_to_listener(struct t_sandbox_sock *s) {
	if (strstr(s->addr.sun_path, "netlink")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_netlink.o;
		s->other_side->send = _netlink_recv_msg_from_toma;
		s->other_side->recv = _netlink_reply_to_toma;
	} else if (strstr(s->addr.sun_path, "signal")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_sig.o;
	} else if (strstr(s->addr.sun_path, "sys_log")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_syslog.o;
	} else if (strstr(s->addr.sun_path, "srm_fault")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_srm_fault.o;
	} else if (strstr(s->addr.sun_path, "srm_timer")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_srm_timer.o;
	} else if (strstr(s->addr.sun_path, "udev_monitor")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_udev.o;
		sys->TSB_udev.o.recv = _recv_empty;
	} else if (strstr(s->addr.sun_path, "toma_rpc_sock")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_rpc.o;
		sys->TSB_rpc.o.recv = _rpc_inject;
		sys->TSB_rpc.o.send = _rpc_accept;
	} else if (strstr(s->addr.sun_path, "epoll")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_epoll.o;
	} else if (strstr(s->addr.sun_path, "wakeup_pipe_pair")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_wake_pip.o;
	} else if (strstr(s->addr.sun_path, "server_events")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_srvr2toma.o;
		s->other_side->recv = server_simu_get_next_msg_for_toma;
		s->other_side->send = _send_illegal_trap;				// Via this fd server sends msgs to Tom, Toma never replies back
	} else if (strstr(s->addr.sun_path, "toma_server")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_toma2srvr.o;
		s->other_side->send = _srvr_simu_from_toma_recv_msg;
		s->other_side->recv = _recv_illegal_trap;				// Via this fd, Toma only sends to to server. Server does not send anything to toma
	} else if (strstr(s->addr.sun_path, "toma_clients")) {
		BUG_ON(s->other_side); s->other_side = &sys->TSB_toma2clnt.o;
		s->other_side->send = _send_illegal_trap;				// Unsupported yet
		s->other_side->recv = _recv_illegal_trap;
	} else {
		return;
	}
	s->other_side->sock = s;
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
	if (addr->nl_family == AF_NETLINK)
		sprintf(s->addr.sun_path, FILE_SANDBOX_PREFIX "bind_netlink_sock");
	else
		sprintf(s->addr.sun_path, FILE_SANDBOX_PREFIX "bind_toma_rpc_sock");
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

// Netlink mock queue helpers (thread-safe)
static bool TSB_netlink_queue_is_empty(void) {
	bool empty;
	pthread_mutex_lock(&sys->TSB_netlink.mutex);
	empty = (sys->TSB_netlink.queue_count == 0);
	pthread_mutex_unlock(&sys->TSB_netlink.mutex);
	return empty;
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
// Queue a netlink disk info response for a given device
static void TSB_netlink_send_disk_response(const struct sandbox_nvme_device *dev, const struct nvmeib_nl_uk_comm_msg *req_msg) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_disk_info_reply *rep = (struct nvmeib_disk_info_reply *)msg->data;

	msg->len = sizeof(*msg) + sizeof(*rep);
	rep->base.opcode = msg->opcode = req_msg->opcode;
	msg->id = req_msg->id;
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);

	// Fill nvmeib_disk_info_reply
	rep->base.error = csce_ok;
	rep->base.latency_ns = 555;
	rep->selector = nvmeib_disk_info_reply_dinfo;

	// Fill disk info from sandbox device
	rep->dinfo.disk.n_blocks = dev->size_in_blocks;
	rep->dinfo.disk.n_hw_blocks = dev->size_in_blocks;
	rep->dinfo.disk.vendor_id = dev->vendor_id;
	rep->dinfo.disk.block_size = 1 << SANDBOX_NVME_BLOCK_SIZE_EXPONENT;  // 4096
	rep->dinfo.disk.max_request_size = 32;
	rep->dinfo.disk.seq = 0;
	rep->dinfo.disk.nsid = 1;
	rep->dinfo.disk.metadata = 0;
	snprintf(rep->dinfo.disk.disk_id, sizeof(rep->dinfo.disk.disk_id), "%s.1", dev->serial_number);
	snprintf(rep->dinfo.disk.dev_name, sizeof(rep->dinfo.disk.dev_name), "%s", dev->device_path);
	snprintf(rep->dinfo.disk.model_str, sizeof(rep->dinfo.disk.model_str), "%s", dev->model_number);
	snprintf(rep->dinfo.disk.native_serial_str, sizeof(rep->dinfo.disk.native_serial_str), "%s", dev->serial_number);
	snprintf(rep->dinfo.disk.status, sizeof(rep->dinfo.disk.status), "Ok");
	rep->dinfo.serjio_status = 0;  // nvmeibs_serjio_status_ok
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
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
	struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
	s->ref_cnt++;
	(void)addr;  (void)addr_len;
	return fd;
}

int override_open(const char *path, int flags, ... /*int mode*/) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, "%s", path);
	return __connect(socket(0, 'f', flags), &addr, 0);
}

int override_close(int fd) {
	int *pipe_fds = sys->TSB_wake_pip.fds;
	if (pipe_fds[0] == fd) {					// Daniel, Todo, encapsulate pipe() same as others, so no special if, for this case
		close(fd);
		pipe_fds[0] = -1;
		socket_destroy(sys->TSB_wake_pip.o.sock);
	} else if (pipe_fds[1] == fd) {
		close(fd);
		pipe_fds[1] = -1;
		socket_destroy(sys->TSB_wake_pip.o.sock);
	} else {
		struct t_sandbox_sock *s = TSB_socket_find_by_fd_opt(fd);
		if (s) {
			socket_destroy(s);
		} else {
			N_Df(ovc5786, "close on untracked fd=@INT", fd);
		}
	}
	return 0;
}

int override_fcntl(int fd, int cmd, ...) {
	const int *pipe_fds = sys->TSB_wake_pip.fds;
	BUG_ON(fd < 3);
	if ((fd == pipe_fds[0]) || (fd == pipe_fds[1])) {					// Daniel, Todo, encapsulate pipe() same as others, so no special if, for this case
		int value = 0;
		va_list ap;
		va_start(ap, cmd);
		value = va_arg(ap, int);
		va_end(ap);
		return fcntl(fd, cmd, value);
	} else {
		struct t_sandbox_sock *s = TSB_socket_find_by_fd(fd);
		(void)s; (void)cmd;
		return 0;
	}
}

int override_pipe(int fds[2]) {
	struct sockaddr_un addr;
	const int pipe_rv = pipe(fds);
	int fd;
	BUG_ON(pipe_rv != 0);
	sys->TSB_wake_pip.fds[0] = fds[0];
	sys->TSB_wake_pip.fds[1] = fds[1];
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "wakeup_pipe_pair{%d,%d}", fds[0], fds[1]);
	fd = __connect(socket(0,0,0), &addr, 16);
	accept(fd, NULL, NULL);	// Increase refcount because this fd represents pair of fds
	return 0;
}

ssize_t override_read(int fd, void *buf, size_t nbytes) {
	struct t_sandbox_sock *s;
	const int *pipe_fds = sys->TSB_wake_pip.fds;
	BUG_ON(fd < 3);
	if ((fd == pipe_fds[0]) || (fd == pipe_fds[1])) {					// Daniel, Todo, encapsulate pipe() same as others, so no special if, for this case
		return read(fd, buf, nbytes);		// Backward compatibility for pipe
	}
	s = TSB_socket_find_by_fd(fd);
	if (s->other_side && s->other_side->recv) {
		return s->other_side->recv(fd, buf, nbytes, OFFSET_NONE, 0);
	} else {
		return read(fd, buf, nbytes);		// Backward compatibility for fd's without backend simulator
	}
}

ssize_t override_write( int fd, const void *buf, size_t count) {
	// Pass through to real file.
	return write(fd, buf, count);
}

ssize_t override_pread( int fd,       void *buf, size_t count, off_t offset) {
	struct t_sandbox_sock *s;
	s = TSB_socket_find_by_fd(fd);
	if (s->other_side && s->other_side->recv) {
		return s->other_side->recv(fd, buf, count, offset, 0);
	} else {
		return pread(fd, buf, count, offset);
	}
}

ssize_t override_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	struct t_sandbox_sock *s;
	s = TSB_socket_find_by_fd(fd);
	if (s->other_side && s->other_side->send) {
		return s->other_side->send(fd, buf, count, offset, 0);
	} else {
		return pwrite(fd, buf, count, offset);
	}
}

int override_select(int nfds, fd_set *__restrict readfds, fd_set *__restrict writefds, fd_set *__restrict exceptfds, struct timeval *__restrict timeout) {
	struct t_sandbox_sock *nl_sock = sys->TSB_netlink.o.sock;
	const int nl_fd = nl_sock->fd;
	BUG_ON(!nl_sock || !readfds);

	if (FD_ISSET(nl_fd, readfds)) {	// Check if netlink socket is in the read set and we have queued messages
		static int n_extended_msgs_to_emulate = 1;
		const bool has_msg_for_toma = !TSB_netlink_queue_is_empty();
		const bool emulate_timeout = (sys->TSB_netlink.n_recv_msgs == 1);
		const bool emulate_extended_msg = (n_extended_msgs_to_emulate > 0) && !has_msg_for_toma && !emulate_timeout;
		if (has_msg_for_toma || emulate_timeout || emulate_extended_msg) {				// Return immediately - netlink socket is ready to read
			FD_ZERO(readfds); FD_ZERO(writefds); FD_ZERO(exceptfds);
			FD_SET(nl_fd, readfds);
			if (has_msg_for_toma) {
				N_Tf(nl_select_ready0, "Netlink fd @INT ready (queued messages)", nl_fd);
				return 1;
			} else if (emulate_timeout) {
				N_Tf(nl_select_ready1, "Netlink emulate timeout");
				return 0; // Timeout;
			} else if (emulate_extended_msg) {
				TSB_netlink_send_extended_msg();
				n_extended_msgs_to_emulate--;
				return 1;
			}
			BUG_ON(true);				// Wrong implementation
		}
	}
	msleep(100);	// Throttled km_comm select
	return select(nfds, readfds, writefds, exceptfds, timeout);
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
	struct globa_epoll *ep = &sys->TSB_epoll;
	static uint64_t loop_idx = 0;
	int i;
	BUG_ON((ep->o.sock->fd != efd)||(man_events < ep->n_fds)); (void)__timeout;
	nanosleep(&(struct timespec){0, 100*1000*1000}, NULL); // 100ms
	SANDBOX_PRINT("Toma Sandbox epoll loop %lu\n", loop_idx); loop_idx++;
	for (i = 0; i < ep->n_fds; i++) {
		evs[i] = ep->evs[i];	// As if each and every fd in which toma is sleeping has an event.
	}
	if (loop_idx < 10) {
		sys->TSB_sig.sig = ((loop_idx % 5) == 0) ? SIGCHLD : 0; // Once in a while send a signal to toma to test this mechanism
		return i;
	} else {
		// sys->TSB_sig.sig = 9;	// Daniel: This seems not to work better than epoll failure
		return -1;				// For now after 10 iterations stop toma. This is ugly! Simulate shutdown instruction via kafka from mgmt
	}
}

/*********************************************************************/
static void toma_unitest_env_end(void) {
	#define LOGS_DIR TOMA_ROOT_DIR "var/log/nvmesh/trace_daemon"
	#define DICT_DIR "99bin/*/obj/"
	SANDBOX_PRINT(COL_GREEN "unitest done sys=%p" COL_RESET ". \t\tAnalyze bin logs via:\n", sys);
	SANDBOX_PRINT(LOGS_DIR "/pager " LOGS_DIR " --toma --color --dict_preload " DICT_DIR "dict* --fmtlib_preload " DICT_DIR "/libfmtrs.so > %s\n", "z.txt");
	t_sandbox_all_destroy();
}
void toma_unitest_env_start(void) {
	bool is_running_as_a_utility = false;
	char *pwd = getcwd(NULL, 0);
	SANDBOX_PRINT("init, is_running_as_a_utility=%d dir: %s\n", is_running_as_a_utility, pwd);
	free(pwd);
	atexit(toma_unitest_env_end);
	SANDBOX_PRINT(COL_GREEN "unitest starting sys=%p" COL_RESET "\n", sys);
	t_sandbox_all_init();
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
static int udp_sock_fd, udp_timer_fd;
int udp_server_sock(void) { return udp_sock_fd; }
int udp_server_timer(void){ return udp_timer_fd; }

void nvmeibt_udp_read_event(int fd) {
	(void)fd;
}

void nvmeibt_udp_timer_event(int fd) {
	(void)fd;
}

int scan_server_netdev(void) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_net_dev");
	return __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
}
int handle_netlink_event(int sock) { (void)sock; return 0; }
int nvmeibt_srm_queue_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req) { (void)srm; (void)req; return 0;}
struct nvmeibt_srm *udp_peer_srm(struct udp_peer *peer) {(void)peer; return NULL;}
int nvmeib_register_udp_peer(struct nvmeibt_node *n, const char *pn, const char *pg){ (void)n; (void)pn; (void)pg; return 0; }
int allocate_udp_server(int p, union ibv_gid *g, struct nvmeibt_node *node) { (void)p; (void)g; (void)node; return 0; }
int start_udp_server(void) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_udp_srvr");
	udp_sock_fd = __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_udp_timer");
	udp_timer_fd = __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
	return 0;
}

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
typedef struct rd_kafka_topic_conf_s {
	int dummy;
} rd_kafka_topic_conf_t;
rd_kafka_topic_conf_t* rd_kafka_topic_conf_new(void) {
	return calloc(1, sizeof(rd_kafka_topic_conf_t));
};
void rd_kafka_topic_conf_destroy(rd_kafka_topic_conf_t *conf) { free(conf); }

typedef struct rd_kafka_topic_s {
	char *name;
	rd_kafka_topic_conf_t* conf;
	int64_t commited_offset, cur_offset, last_offset;
	// Todo: Linked list of messages for offsets above cur,cur+1,....last_offset
	int32_t partition;		// Support only 1 partition for now. Store its index
	bool is_active;
	int temp_store_offset;	// Daniel, not sure is needed - just for two stage store and commit.
} rd_kafka_topic_t;

typedef struct rd_kafka_conf_s {
	char *group_id;
	bool enable_ssl;
} rd_kafka_conf_t;

typedef struct rd_kafka_s {
	char* name;
	int log_lvl;
	enum rd_kafka_type_t who;
	rd_kafka_conf_t* conf;
	rd_kafka_topic_t topic;
} rd_kafka_t;

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
	p->parition = partition;
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

void rd_kafka_conf_set_dr_msg_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk,const rd_kafka_message_t *kmsg, void *opaque)) {
	sys->kafka_simu.notify_producer_msg_accepted = fn;
	(void)kc;
}

int rd_kafka_produce(rd_kafka_topic_t *kt, int32_t partition, int msgflags, void *payload, size_t len, const void *key, size_t keylen, void *msg_opaque) {
	static int fail_once_every = 0;
	rd_kafka_t *ko = kafka_simu_find_by_topic(kt);
	rd_kafka_message_t km;
	km._private = msg_opaque;
	km.err = (fail_once_every++ % 3) ? 0 : RD_KAFKA_RESP_ERR__TIMED_OUT;		// Once every few messages fail completion
	BUG_ON((partition != RD_KAFKA_PARTITION_UA) || (key == NULL) || (len == 0) || (keylen == 0));
	(void)msgflags;
	if (0) SANDBOX_PRINT("> %d > |%s|  :  |%s|\n", fail_once_every, (char*)key, (char*)payload);
	// No, put this on to kt, in a list and then poll_cb will return the callbacks
	sys->kafka_simu.notify_producer_msg_accepted(ko, &km, NULL);
	// Todo: Here, submit msg to management simulator
	errno = 0;
	return 0;
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
const char* mgmt_simu_kafka_msg_cache[] = {
	"{\"messageType\":\"updateLeaderKeepaliveToken\",\"messageTypeVersion\":1,\"payload\":{\"token\":1,\"keepaliveInterval\":5}}",
	"{\"messageType\":\"updateTomaKeepaliveToken\""",\"messageTypeVersion\":1,\"payload\":{\"nodeID\":\"%s\",\"token\":3,\"zone\":\"1\",\"keepaliveInterval\":5}}",
	"{\"messageType\":\"addTarget\",\"messageTypeVersion\":1,\"payload\":{\"nodeID\":\"%s\",\"uuid\":\"e8c70c10-db79-11f0-8f35-cb935b7ef6ae\",\"targetsInZone\":0,\"targetUpdatesSequence\":1}}",
	// hardwareConfiguration message: disk IDs must match those from disks.csv (serial.nsid format)
	// Simulated disks: NVMD_SN_002.1 (vendor 5122), NVMD_SN_003.1 (vendor 5123) with 2000 blocks each
	"{\"messageType\":\"hardwareConfiguration\"   "",\"messageTypeVersion\":1,\"payload\":{\"managementConfiguration\":{\"_id\":\"1\",\"configurationVersion\":17,\"leaderToken\":1,\"kafkaMessageSequence\""
		":%d,\"raftTerm\":9,\"stopSendingKeepaliveToken\":false,\"dbUUID\":\"141d3140-c3c0-11f0-bc49-e391b6ca4c2b\"},"
		"\"targets\":["
			"{\"_id\":\"nvme38.mlnx\",\"node_id\":\"%s\",\"uuid\":\"cde269b0-c3c0-11f0-bc49-e391b6ca4c2b\","
				"\"disks\":["
					"{\"diskID\":\"NVMD_SN_002.1\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5122,\"uuid\":\"f39cebd0-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false},"
					"{\"diskID\":\"NVMD_SN_003.1\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5123,\"uuid\":\"f39cebd1-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5d008\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef0-c3c0-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5d009\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce10-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
			"{\"_id\":\"nvme39.mlnx\",\"node_id\":\"n39@google.com\",\"uuid\":\"cde269b1-c3c0-11f0-bc49-e391b6ca4c2b\","
				"\"disks\":["
					"{\"diskID\":\"D0_n39\",\"blocks\":195353046,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5197,\"uuid\":\"f39cebd0-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false},"
					"{\"diskID\":\"D1_n39\",\"blocks\":195353046,\"block_size\":1024,\"activeFormatRequestCounter\":0,\"vendorID\":3333,\"uuid\":\"f39cebd1-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":1,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5d008\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef0-c3c0-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5d009\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce10-c3c0-11f0-bc49-e391b6ca4c2b\"}]}"
		"]}",
	"{\"messageType\":\"addVolume\",\"messageTypeVersion\":1,\"payload\":{\"_id\":\"V1\",\"uuid\":\"f1b17590-c52b-11f0-bc49-e391b6ca4c2b\","
		"\"version\":1,\"name\":\"V1\",\"blockSize\":4096,\"lockServer\":{\"maxNOwners\":1,\"type\":4,\"locksetShift\":-1},\"blocks\":4882432,\"RAIDLevel\":\"Striped RAID-0\",\"numberOfMirrors\":0,\"stripeSize\":32,\"stripeWidth\":2,\"status\":\"unavailable\","
		"\"action\":\"initializing\",\"relativeRebuildPriority\":10,\"reservation\":{\"mode\":0,\"version\":1,\"reservedBy\":null,\"attachedClients\":[],\"lastTransitionDate\":null},\"use_debug_di\":false,"
		"\"chunks\":["
			"{\"uuid\":\"09c2f550-c52c-11f0-bc49-e391b6ca4c2b\",\"vlbs\":0,\"vlbe\":4882431,\"pRaids\":["
				"{\"uuid\":\"09c2f552-c52c-11f0-bc49-e391b6ca4c2b\",\"activated\":false,\"stripeIndex\":0,\"zone\":\"1\",\"diskSegments\":["
					"{\"uuid\":\"09c2f551-c52c-11f0-bc49-e391b6ca4c2b\",\"lbs\":26900224,\"lbe\":29341439,\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"f3a2b830-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
				"{\"uuid\":\"09c34371-c52c-11f0-bc49-e391b6ca4c2b\",\"activated\":false,\"stripeIndex\":1,\"zone\":\"1\",\"diskSegments\":["
					"{\"uuid\":\"09c34370-c52c-11f0-bc49-e391b6ca4c2b\",\"lbs\":1509632,\"lbe\":3950847,\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"f3a24300-c3c0-11f0-bc49-e391b6ca4c2b\"}"
		"]}]}]}}",
};

rd_kafka_message_t* rd_kafka_consumer_poll(rd_kafka_t *ko, int timeout_ms) {
	rd_kafka_message_t *m = calloc(1, sizeof(*m));
	BUG_ON((timeout_ms != 0) || (!ko->topic.is_active));
	m->err = RD_KAFKA_RESP_ERR_NO_ERROR;
	if (!strncmp(ko->name, "HW", 2)) {
		static int once_every = 0;
		if (once_every == 0) {
			m->payload = malloc(256);
			m->len = snprintf(m->payload, 256, mgmt_simu_kafka_msg_cache[1], sys->my_hostname);
			once_every++;
		} else if (((once_every++ % 4) == 0) && true) {		// Inject conf msg once every few iterations. Todo, make this actual conf msg
			m->payload = malloc(4096);
			m->len = snprintf(m->payload, 4096, mgmt_simu_kafka_msg_cache[3], once_every, sys->my_hostname);
		}
	} else if (!strncmp(ko->name, "CMD", 3)) {
		static int cmds_order = 0;
		if (cmds_order == 0) {					// First command is the zone. Later send keepalive once every 3 requests.
			m->payload = malloc(256);
			m->len = snprintf(m->payload, 256, mgmt_simu_kafka_msg_cache[1], sys->my_hostname);
			cmds_order++;
		} else if ((cmds_order++ % 3) == 0) {
			m->payload = strdup(mgmt_simu_kafka_msg_cache[0]);	// Because it is treated by toma not as const
			m->len = strlen(m->payload);
		}
	} else if (!strstr(ko->name, "incrementalTarget")) {
		// Not called yet, because in toma nvmeibt_kafka_req_start_consuming_leader_TARGET_msgs() is not called yet. Solve it
		static int cmds_order = 0;
		if (cmds_order == 0) {
			m->payload = malloc(256);	// Insert self machine as 1 machine raft domain. Will auto become leader
			m->len = snprintf(m->payload, 256, mgmt_simu_kafka_msg_cache[2], sys->my_hostname);
			cmds_order++;
		}
	} else {
		BUG_ON(true);		// Not supported yet. Insert messages to other kafka queues as well
	}
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
