#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS I/O functions from this module - must be defined before any other includes

#include "nvmeibt_debug.h"
#include "toma_in_sandbox.h"
#include "sandbox_util.h"
#include "mgmt_sim.h"
#include "kafka/sandbox_kafka_internal.h"
#include "unit_test_main.h"

#define FILE_SANDBOX_PREFIX TOMA_ROOT_DIR "var/run/nvmesh/sandbox_fd_"

#include <stdarg.h>				// va_list
void syslog(int priority, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	if (     priority <= LOG_ERR)		fprintf(stderr, COL_RED_BOLD);
	else if (priority == LOG_WARNING)	fprintf(stderr, COL_YELLOW);
	else if (priority == LOG_NOTICE)	fprintf(stderr, COL_PURPL);
	else if (priority == LOG_INFO)		fprintf(stderr, COL_WHITE_BOLD);
	else  /* priority == LOG_DEBUG */	fprintf(stderr, COL_RESET);	// Default
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, COL_RESET "\n");
	va_end(ap);
	(void)priority;
}

/************************************* Kernel ********************************/
#include "os/os_internal.h"
#include <sys/stat.h>		// fstat()
#include <errno.h>

#include "sandbox_nvme.h"
#include "interfaces/nvme/nvmeibt_nvme_defines.h"	// nvmeioctls
#include <linux/fs.h>		// For BLKGETSIZE64, BLKSSZGET
#include "server/sandbox_nvmeibs_toma.h"

/************************************* FD/Sockets ********************************/
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
		"simulate dump-clnt-hash 20\n", "simulate bm-garbage-collect 1\n", "simulate resend-praids-report vol1\n", "status\n", "status server_csvs\n", "status errors\n"}; // Todo: This should be a linked list to which unit-test env injects rpc and toma extracts them 1 by 1.
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

bool sbfd_is_used(const struct TSB_fd_impl *s) {
	return (s->f != NULL) || (s->addr.sun_path[0] != 0);
}

struct TSB_fd_impl* TSB_all_fds_tbl_find_next_unused(struct TSB_all_fds_tbl *ts) {
	int i = ts->n_fds;
	for (i = 0; i < ts->n_fds; i++) {		// Reuse deleted fd
		if (!sbfd_is_used(&ts->fd_arr[i]))
			return &ts->fd_arr[i];
	}
	{										// Allocate next fd
		struct TSB_fd_impl *s = &ts->fd_arr[ts->n_fds++];
		BUG_ON((i >= ARRAY_SIZE(ts->fd_arr)) || sbfd_is_used(s));
		return s;
	}
}

void TSB_all_fds_tbl_destroy(struct TSB_all_fds_tbl *ts) {
	for (int i = 0; i < ts->n_fds; i++) {
		const struct TSB_fd_impl *s = &ts->fd_arr[i];
		if (sbfd_is_used(s)) {		// Leak of this file descriptor
			SANDBOX_PRINT("TSB[%2d]: " COL_RED_BOLD "Leaking fd=%2d, " COL_RESET " path=%-40s\n", i, s->fd, s->addr.sun_path);
			BUG_ON(true);
		}
	}
}

struct t_sandbox_all {
	struct TSB_operating_system_impl os;				// Sandbox for all services Toma needs from the operating system
	struct TSB_basic {								// Unit-test side connections of Toma sockets/fd's
		struct TSB_fd_otherside o;
	} TSB_udev, TSB_rpc, TSB_srm_fault, TSB_srm_timer, TSB_nm_raft;
	struct kafka_simulator_t *kafka_simu;
	struct nvmeibs_simulator *srvr;
	struct sb_cluster_conf cfg;
	struct mgmt_sim_state *mgmt;
	struct nvmeibt_nm_local_node *nm;
	bool is_running_as_a_utility;
	bool can_use_bin_traces;
} *sys;

static ssize_t _socket_pair_wakeup_send(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_comm_wakeup_mock *w = &sys->os.TSB_km_sock_pair;
	long n_wups;
	BUG_ON((w->o[0].sock->fd != fd) || (n != 1) || (buf == NULL) || (offset != 0) || (flags != 0));
	n_wups = __atomic_add_fetch(&w->n_wakeup_msgs, 1, __ATOMIC_SEQ_CST);
	N_Tf(__AUTOID__, "n_wakups_in_queue=@INT", (int)n_wups);
	return n;
}

static ssize_t _socket_pair_wakeup_recv(int fd, void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_comm_wakeup_mock *w = &sys->os.TSB_km_sock_pair;
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
	const struct TSB_server_comm_wakeup_mock *w = &sys->os.TSB_km_sock_pair;
	const long n = __atomic_load_n(&w->n_wakeup_msgs, __ATOMIC_SEQ_CST);
	return n > 0;
}

static bool _wakeup_pipe_should_wakeup(void) {
	struct TSB_wakeup_pipe_impl *w = &sys->os.TSB_wake_pip;
	bool rv;
	pthread_mutex_lock(&w->mutex);
	rv = (w->q_count != 0);
	pthread_mutex_unlock(&w->mutex);
	return rv;
}

static ssize_t _wakeup_pipe_wakeup_send(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_wakeup_pipe_impl *w = &sys->os.TSB_wake_pip;
	int n_wake_ups;
	pthread_mutex_lock(&w->mutex);
	BUG_ON((w->o[1].sock->fd != fd) || (offset != 0) || (flags != 0) || (w->q_count >= TSB_WU_PIPE_QUEUE_SIZE) || (n != TSB_WU_PIPE_MSG_SIZE));
	memcpy(w->queue[w->q_tail].data, buf, n);
	w->q_tail = (w->q_tail + 1) % TSB_WU_PIPE_QUEUE_SIZE;
	n_wake_ups = ++w->q_count;
	pthread_mutex_unlock(&w->mutex);
	N_Tf(__AUTOID__, "n_wake_ups=@INT", n_wake_ups);
	return n;
}

static ssize_t _wakeup_pipe_wakeup_recv(int fd, void *buf, size_t n, off_t offset, int flags) {
	struct TSB_wakeup_pipe_impl *w = &sys->os.TSB_wake_pip;
	int n_wake_ups = 0;
	pthread_mutex_lock(&w->mutex);
	BUG_ON((w->o[0].sock->fd != fd) || (offset != OFFSET_NONE) || (flags != 0) || (w->q_count < 0) || (n != TSB_WU_PIPE_MSG_SIZE));
	if (w->q_count > 0) {
		memcpy(buf, w->queue[w->q_head].data, n);
		w->q_head = (w->q_head + 1) % TSB_WU_PIPE_QUEUE_SIZE;
		n_wake_ups = --w->q_count;
	} else {
		n = -1;			// No data
		errno = EAGAIN;
	}
	pthread_mutex_unlock(&w->mutex);
	N_Tf(__AUTOID__, "n_wake_ups=@INT", n_wake_ups);
	return n;
}

static bool nvmeibt_toma_is_running_as_a_utility(void) { return sys->is_running_as_a_utility; }

void t_sandbox_all_init(bool is_running_as_a_utility) {
	sys = calloc(1, sizeof(*sys));
	sys->os.fs.debug_offset = 10000;
	sys->is_running_as_a_utility = is_running_as_a_utility;
	sb_cluster_conf_create(&sys->cfg);
	sys->kafka_simu = sandbox_kafka_init(&mgmt_sim_wakeup_on_incomming_toma_msg);
	sys->mgmt = mgmt_sim_init(&sys->cfg);
	pthread_mutex_init(&sys->os.fs.mutex, NULL);
	sys->srvr = nvmeibs_simu_init(&sys->os.TSB_netlink);
	pthread_mutex_init(&sys->os.TSB_wake_pip.mutex, NULL);

	{ /* Build raft domain, First message: addTarget (self as 1-machine raft domain), then the other 2 */
		for (int i = 0; i < sys->cfg.n_nodes; i++)
			mgmt_sim_send_msg_change_raft_quorum(i, true);
		// Just a unitest scenario add/rmv target. Todo: should not be done in init but in a separate unitest function
		mgmt_sim_send_msg_change_raft_quorum(1, false);			// Remove First other target
		mgmt_sim_send_msg_change_raft_quorum(1, true);			// Re-add First other again
		mgmt_sim_send_msg_change_raft_quorum(2, true);			// Re-add last target again, while it already exists, verify Toma can handle this
	}
	mgmt_sim_send_msg_assign_to_zone(1);
	if (!nvmeibt_toma_is_running_as_a_utility())
		toma_unit_test_thread_create();
}

void t_sandbox_all_destroy(void) {
	if (!nvmeibt_toma_is_running_as_a_utility()) {
		toma_unit_test_thread_destroy();
		mgmt_sim_verify_at_end();
	}
	nvmeibs_simu_destroy(sys->srvr, !nvmeibt_toma_is_running_as_a_utility());			// Only check for replies if we sent messages (standalone utilities like gpt_util don't communicate with TOMA)
	mgmt_sim_destroy();				// Must destroy mgmt_sim's Kafka objects before the broker
	sandbox_kafka_destroy(sys->kafka_simu);
	pthread_mutex_destroy(&sys->os.fs.mutex);
	pthread_mutex_destroy(&sys->os.TSB_wake_pip.mutex);
	sb_cluster_conf_destroy(&sys->cfg);
	TSB_all_fds_tbl_destroy(&sys->os.fs);
	free(sys);
	sys = NULL;
}

static struct TSB_fd_impl * TSB_socket_find_by_fd(int fd) {		// Look up and return a socket object by fd. Bug if not found. Sandbox environment should emulate all fd's
	struct TSB_all_fds_tbl *TS = &sys->os.fs;
	int i;
	if (fd >= sys->os.fs.debug_offset)
		return &TS->fd_arr[fd - TS->debug_offset];
	for (i = 0; i < TS->n_fds; i++) {
		if (TS->fd_arr[i].fd == fd)
			return &TS->fd_arr[i];
	}
	BUG_ON(true);
	return NULL;
}

int ioctl(int fd, unsigned long int req, ...) {
	struct TSB_fd_impl *tsb = TSB_socket_find_by_fd(fd);
	const char *path = tsb->addr.sun_path;
	va_list ap;
	int rv = 0;
	va_start(ap, req);
	N_Df(sbioct0, "ioctl fd=@INT path=@STR", fd, path);
	if (req == NVME_IOCTL_ADMIN_CMD) {
		rv = nvme_ioctl_admin_cmd(path, fd, ap);
	} else if (req == NVME_IOCTL_ID) {
		rv = fd;
	} else if (req == FIONBIO) {
		rv = 0;
	} else if (req == BLKSSZGET) {
		rv = nvme_ioctl_get_size(path, ap);
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
	va_end(ap);
	return rv;
}

static bool _recv_has_raft_msgs_for_toma(void);

// Connect the other side which communicates with Toma
void TSB_connect_sock_to_listener(struct TSB_fd_impl *s) {
	BUG_ON(s->other_side); 								// Only 1 simulate4d listener works per socket / file descriptor
	if (strstr(s->addr.sun_path, "netlink")) {					s->other_side = &sys->os.TSB_netlink.o;
	} else if (strstr(s->addr.sun_path, "signal")) {			s->other_side = &sys->os.TSB_signal.o;
		s->other_side->has_data = _recv_always_has_data;			// Todo: unitest env should inject
	} else if (strstr(s->addr.sun_path, "sys_log")) {			s->other_side = &sys->os.TSB_syslog.o;
	} else if (strstr(s->addr.sun_path, "srm_fault")) {			s->other_side = &sys->TSB_srm_fault.o;
		s->other_side->has_data = _recv_always_has_data;			// Todo: unitest env should inject
	} else if (strstr(s->addr.sun_path, "srm_timer")) {			s->other_side = &sys->TSB_srm_timer.o;
	} else if (strstr(s->addr.sun_path, "nm_raft")) {			s->other_side = &sys->TSB_nm_raft.o;
		s->other_side->has_data = _recv_has_raft_msgs_for_toma;
	} else if (strstr(s->addr.sun_path, "udev_monitor")) {		s->other_side = &sys->TSB_udev.o;
		sys->TSB_udev.o.recv = _recv_empty;
		s->other_side->has_data = _recv_always_has_data;			// Todo: unitest env should inject
	} else if (strstr(s->addr.sun_path, "mesh/toma_rpc")) {		s->other_side = &sys->TSB_rpc.o;
		sys->TSB_rpc.o.recv = _rpc_inject;
		sys->TSB_rpc.o.send = _rpc_accept;
	} else if (strstr(s->addr.sun_path, "epoll")) {				s->other_side = &sys->os.TSB_epoll.o;
	} else if (strstr(s->addr.sun_path, "wakeup_pipe_pair0")) {	s->other_side = &sys->os.TSB_wake_pip.o[0];
		s->other_side->recv = _wakeup_pipe_wakeup_recv;
		s->other_side->send = fd_otherside_read_only_illegal_send;
		s->other_side->has_data = _wakeup_pipe_should_wakeup;	// o[0] Toma main thread read wakeups messages from other threads. Never writes
	} else if (strstr(s->addr.sun_path, "wakeup_pipe_pair1")) {	s->other_side = &sys->os.TSB_wake_pip.o[1];
		s->other_side->send = _wakeup_pipe_wakeup_send;			// o[1] Toma aux thread write to wakeup toma main thread. Never reads
		s->other_side->recv = fd_otherside_write_only_illegal_recv;
	} else if (strstr(s->addr.sun_path, "server_events")) {		s->other_side = &sys->srvr->com_srvr2toma_o;
	} else if (strstr(s->addr.sun_path, "toma_server")) {		s->other_side = &sys->srvr->com_toma2srvr_o;
	} else if (strstr(s->addr.sun_path, "toma_clients")) {		s->other_side = &sys->srvr->com_toma2clnt_o;
	} else if (strstr(s->addr.sun_path, "km_comm_pair0")) {		s->other_side = &sys->os.TSB_km_sock_pair.o[0];
		s->other_side->send = _socket_pair_wakeup_send;			// o[0] Toma writes to it to wakeup server lib main thread. Never reads
		s->other_side->recv = fd_otherside_write_only_illegal_recv;
	} else if (strstr(s->addr.sun_path, "km_comm_pair1")) {		s->other_side = &sys->os.TSB_km_sock_pair.o[1];
		s->other_side->send = fd_otherside_read_only_illegal_send;			// o[1] ServerLib reads from it to wakeup. Never writes
		s->other_side->recv = _socket_pair_wakeup_recv;
		s->other_side->has_data = _socket_pair_should_wakeup;
	} else {
		return;
	}
	s->other_side->sock = s;
}

static bool sbfd_is_a_file(const struct TSB_fd_impl* s) {
	return (s->type == 'f');
}

static bool sbfd_should_persist_after_close(const struct TSB_fd_impl* s) {
	// Only regular files should persist after close, but socket/pipe are non-persistent.
	return sbfd_is_a_file(s);
}

static const char* sbfd_get_open_mode(const struct TSB_fd_impl* s) {
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

int TSB_fd_impl_get_index_in_arr(const struct TSB_fd_impl *s) { return (int)(s - sys->os.fs.fd_arr); }
int socket(int __domain, int __type, int __protocol) {
	struct TSB_all_fds_tbl *TS = &sys->os.fs;
	struct TSB_fd_impl *s = TSB_all_fds_tbl_find_next_unused(TS);
	s->dom =__domain;
	s->type = __type;
	s->proto =__protocol;
	s->ref_cnt = 1;
	return TS->debug_offset + TSB_fd_impl_get_index_in_arr(s);
}

static void socket_destroy(struct TSB_fd_impl *s) {
	const bool should_del = !sbfd_should_persist_after_close(s);
	s->ref_cnt--;
	if (s->ref_cnt > 0)
		return;
	if (sys->can_use_bin_traces) {		// Some fd's are closed after binary traces were shut down
		N_SANDBOX(__AUTOID__, "TSB[@EI]: fd=@EI, path=@STR, close, del=@BOOL_YN", TSB_fd_impl_get_index_in_arr(s), s->fd, s->addr.sun_path, should_del);
	} else {							// Closing syslog when binary traces are disabled
		SANDBOX_PRINT("TSB[%2d]: fd=%2d, path=%-40s, close, del=%u\n", TSB_fd_impl_get_index_in_arr(s), s->fd, s->addr.sun_path, should_del);
	}
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

int TSB_sock_open(struct TSB_fd_impl *s) {
	const char* open_mode = sbfd_get_open_mode(s);
	s->f = fopen(s->addr.sun_path, open_mode);
	if (s->f == NULL) {
		N_Ef(__AUTOID__, "Cannot open file |@STR|. Crashing...", s->addr.sun_path);
		BUG_ON(true);
	}
	s->fd = fileno(s->f);
	TSB_connect_sock_to_listener(s);
	N_SANDBOX(__AUTOID__, "TSB[@EI]: fd=@EI, path=@STR, mode=@STR (@X), listener=@BOOL_YN", TSB_fd_impl_get_index_in_arr(s), s->fd, s->addr.sun_path, open_mode, s->proto, !!s->other_side);
	return s->fd;
}

struct TSB_fd_impl * TSB_fd_impl_get_by_fd(int fd) {
	struct TSB_all_fds_tbl *TS = &sys->os.fs;
	return &TS->fd_arr[fd - TS->debug_offset];
}

int __connect(int fd, const struct sockaddr_un *addr, unsigned int len) {
	struct TSB_fd_impl *s = TSB_fd_impl_get_by_fd(fd);
	s->addr = *addr;
	s->len = len;
	return TSB_sock_open(s);
}

int __bind(int fd, const void* __addr, unsigned int len) {
	struct TSB_fd_impl *s = TSB_fd_impl_get_by_fd(fd);
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

ssize_t sendmsg(int __fd, const struct msghdr *__msg, int __flags) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(__fd);
	struct iovec *iov = (struct iovec *)__msg->msg_iov;
	BUG_ON(__msg->msg_iovlen != 1);			// Assert single iovec element (as used by km_comm)
	return s->other_side->send(__fd, iov[0].iov_base, iov[0].iov_len, OFFSET_NONE, __flags);
}

ssize_t recvmsg(int __fd, struct msghdr *__msg, int __flags) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(__fd);
	struct iovec *iov = (struct iovec *)__msg->msg_iov;
	BUG_ON(__msg->msg_iovlen != 1);	// Assert single iovec element (as used by km_comm)
	return s->other_side->recv(__fd, iov[0].iov_base, iov[0].iov_len, OFFSET_NONE, __flags);
}

ssize_t send(int fd, const void *buf, size_t n , int flags) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	return s->other_side->send(fd, buf, n, OFFSET_NONE, flags);
}

ssize_t recv(int fd,       void *buf, size_t n , int flags) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	return s->other_side->recv(fd, buf, n, OFFSET_NONE, flags);
}

int setsockopt(int fd, int lvl, int name, const void *val, unsigned int optlen) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	(void)s; (void)lvl; (void)name; (void)val; (void)optlen;
	return 0;
}

int listen(int fd, int n) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	(void)s; (void)n;
	return 0;
}

int accept(int fd, struct sockaddr* addr, unsigned int *addr_len) {
	struct TSB_fd_impl *s;
	pthread_mutex_lock(&sys->os.fs.mutex);
	s = TSB_socket_find_by_fd(fd);
	s->ref_cnt++;
	pthread_mutex_unlock(&sys->os.fs.mutex);
	(void)addr;  (void)addr_len;
	return fd;
}

int override_open(const char *path, int flags, ... /*int mode*/) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	int ret;
	sprintf(addr.sun_path, "%s", path);
	pthread_mutex_lock(&sys->os.fs.mutex);
	ret = __connect(socket(0, 'f', flags), &addr, 0);
	pthread_mutex_unlock(&sys->os.fs.mutex);
	return ret;
}

int override_close(int fd) {
	struct TSB_fd_impl *s;
	pthread_mutex_lock(&sys->os.fs.mutex);
	s = TSB_socket_find_by_fd(fd);
	socket_destroy(s);
	pthread_mutex_unlock(&sys->os.fs.mutex);
	errno = 0;
	return 0;
}

// Create a new TSB entry that mirrors an existing fd.
// Returns the real OS fd, matching the pattern of override_open()/TSB_sock_open().
int override_dup(int oldfd) {
	struct TSB_fd_impl *old_s = TSB_socket_find_by_fd(oldfd);
	struct TSB_all_fds_tbl *TS = &sys->os.fs;
	struct TSB_fd_impl *new_s;
	int new_os_fd = -1;
	int ret = -1;
	pthread_mutex_lock(&TS->mutex);
	new_s = TSB_all_fds_tbl_find_next_unused(TS);

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
	N_SANDBOX(__AUTOID__, "TSB dup: oldfd=@INT -> newfd=@INT, path=@STR", oldfd, ret, new_s->addr.sun_path);

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
	struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
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
	struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->recv(fd, buf, nbytes, OFFSET_NONE, 0);
	return read(s->fd, buf, nbytes);	// Use real OS fd for passthrough
}

ssize_t override_write(int fd, const void *buf, size_t count) {
	struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->send(fd, buf, count, 0, 0);
	return write(s->fd, buf, count);	// Use real OS fd for passthrough
}

ssize_t override_pread(int fd,       void *buf, size_t count, off_t offset) {
	struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->recv(fd, buf, count, offset, 0);
	return pread(s->fd, buf, count, offset);	// Use real OS fd for passthrough
}

ssize_t override_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	if (s->other_side)
		return s->other_side->send(fd, buf, count, offset, 0);
	return pwrite(s->fd, buf, count, offset);	// Use real OS fd for passthrough
}

static void __temp_wait_sleep(void) { nanosleep(&(struct timespec){0, 10*1000*1000}, NULL); /* 100ms */ }

int override_select(int nfds, fd_set *__restrict readfds, fd_set *__restrict writefds, fd_set *__restrict exceptfds, struct timeval *__restrict timeout) {
	const struct TSB_server_comm_wakeup_mock *w = &sys->os.TSB_km_sock_pair;
	const int nl_fd = sys->os.TSB_netlink.o.sock->fd, ls_fd = sys->srvr->com_srvr2toma_o.sock->fd;
	const bool monitor_nl = FD_ISSET(nl_fd, readfds), monitor_wakup = FD_ISSET(w->o[1].sock->fd, readfds), monitor_ls = FD_ISSET(ls_fd, readfds);
	int n_events, n_iterations;
	BUG_ON(!readfds || (nfds <= nl_fd) || (nfds <= w->o[1].sock->fd) || (nfds <= ls_fd));	// Wrong select from Toma production code
	FD_ZERO(readfds); if (writefds) FD_ZERO(writefds); FD_ZERO(exceptfds);
	for (n_events = 0, n_iterations = 0; n_events == 0; n_iterations++) { // Throttled km_comm select, todo, use timeout
		if (monitor_nl && sys->os.TSB_netlink.o.has_data()) {	// Check if netlink socket is in the read set and we have queued messages, prepared by server_simu_get_next_msg_for_toma
			FD_SET(nl_fd, readfds);
			n_events++;
		}
		if (monitor_wakup && w->o[1].has_data()) { 			// Check if wakeup due to toma sending message
			FD_SET(w->o[1].sock->fd, readfds);				// Toma sends message via netlink
			n_events++;
		}
		if (monitor_ls && sys->srvr->com_srvr2toma_o.has_data()) { // Check if local servers message arrived
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
	struct TSB_globa_epoll_impl *ep = &sys->os.TSB_epoll;
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_epoll_fd");
	memset(ep->evs, 0, sizeof(ep->evs));
	ep->n_fds = 0;
	return __connect(socket(0,0,flags), &addr, 0);
}

int epoll_ctl(int efd, enum EPOLL_CTL op, int __fd, struct epoll_event *ev) {
	struct TSB_globa_epoll_impl *ep = &sys->os.TSB_epoll;
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
	#define SANDBOX_TERMINATE_AFTER_N_LOOPS 500
	struct TSB_globa_epoll_impl *ep = &sys->os.TSB_epoll;
	static uint64_t loop_idx = 0;
	static bool is_shutting_down = false;
	int i, n_events;
	BUG_ON((ep->o.sock->fd != efd)||(man_events < ep->n_fds)); (void)__timeout;
	toma_unit_test_thread_switch_to();
	__temp_wait_sleep();

	sys->os.TSB_signal.sig = ((loop_idx % 5) == 0) ? SIGCHLD : 0; // Once in a while send a signal to toma to test this mechanism
	if (loop_idx == 9) nvmeibs_simu_send_extended_msg("HelloFromClnt");		// Once send an extended message to test the flow
	nvmeibs_simu_do_periodic();					// Process any pending disk ADD event that was deferred from a format operation. This gives the REMOVE event time to be processed by the work queue.
	mgmt_sim_do_periodic();

	for (i = 0, n_events = 0; i < ep->n_fds; i++) {
		const int fd = ep->evs[i].__fd;
		const struct TSB_fd_otherside *o = TSB_socket_find_by_fd(fd)->other_side;
		if (o->has_data())
			evs[n_events++] = ep->evs[i];
	}
	N_SANDBOX(__AUTOID__, "epoll loop @ZU dying=@BOOL_YN, n_events=@INT", loop_idx, is_shutting_down, n_events); loop_idx++;
	if (!is_shutting_down) {
		if (mgmt_sim_is_done()) {
			SANDBOX_PRINT("test: %s\n", COL_GREEN "passed" COL_RESET);
			// At some point we'll probably have multiple test scenarios that we'll want to run in sequence,
			// and finally shut down Toma when they've all passed. For now, there's only one test scenario.
			SANDBOX_PRINT("%s", "sandbox shutting down Toma app\n");
			is_shutting_down = true;
			errno = ENOMEM;
			return -1;				// Simulate shutdown instruction via kafka from mgmt
		}
		if (loop_idx >= SANDBOX_TERMINATE_AFTER_N_LOOPS) {
			SANDBOX_PRINT("failed: test did not complete within %d cycles (mgmt_sim state: %s)\n",
			SANDBOX_TERMINATE_AFTER_N_LOOPS, mgmt_sim_get_state_name());
			BUG_ON(true);
			return -1;  // unreachable
		}
	}
	return n_events;
}

/*********************************************************************/
#include <dirent.h>
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
	sys->can_use_bin_traces = true;
}

void toma_unitest_notify_stop_traces(void) {
	sys->can_use_bin_traces = false;
}
/************************************* logging ********************************/
int init_signal_handling(const char *exe_name) {
	struct TSB_signals_queue* tsb_q = &sys->os.TSB_signal;
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", _PATH_LOG);
	sys->os.TSB_syslog.fd = __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_signal_%s", exe_name);
	tsb_q->fd = __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
	return tsb_q->fd;
}

void handle_sig_fd(int signals_fd, void (*fn)(int32_t n, uint64_t addr)) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(signals_fd);
	struct TSB_signals_queue *tsb_q = container_of(s->other_side, struct TSB_signals_queue, o);
	if (tsb_q->sig) {
		fn(tsb_q->sig, 0x12345);
	}
}

int nvmeibt_nonblock_fd(int fd) {
	const struct TSB_fd_impl *s = TSB_socket_find_by_fd(fd);
	(void)s;
	return 0;
}

void closelog(void) {
	override_close(sys->os.TSB_syslog.fd); sys->os.TSB_syslog.fd = -1;
	override_close(sys->os.TSB_signal.fd); sys->os.TSB_signal.fd = -1;
}

/************************************* nvme ***********************************/
#include "interfaces/nvme/nvmeibt_udev.h"
int  nvmeibt_udev_create(void) {
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "_udev_monitor");
	return __connect(socket(0,0,0), &addr, 0);
}

void nvmeibt_udev_destroy(void) {
	struct TSB_fd_impl *s = (struct TSB_fd_impl *)sys->TSB_udev.o.sock;
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

struct nvmeibt_nm_local_node { 					// Network module simulator. For Toma to communicate with other simulated Toma's
	const char *dynamic_lib_path;
	struct t_raft_msg_queue_from_other_tomas {
		int n_msgs;
		struct nvmeibt_big_msg *msg_q[10];		// Up to 10 messages
	} raft_msg_queue_from_other_tomas;
	struct {
		int vote_rep;
		int append_ent_rep;
	} n_total_msmgs_sent;
	int fd;
	int8_t n_connected_remote_nodes;
	int8_t n_nics;								// Nics to communicate with with other Tomas
};

void *nvmeibt_nm_tracer_init(const char *lib_path) { return (void *)lib_path; }

struct nvmeibt_nm_local_node * nvmeibt_nm_init(void *handle) {
	struct nvmeibt_nm_local_node *rv = calloc(1, sizeof(*rv));
	struct sockaddr_un addr = { .sun_family = 0, .sun_path = {0}};
	BUG_ON(sys->nm);			// Already initialized
	rv->dynamic_lib_path = (const char*)handle;
	sprintf(addr.sun_path, FILE_SANDBOX_PREFIX "nm_raft");
	rv->fd = __connect(socket(0,0,0), &addr, 0);		// Just open files for educational purposes
	sys->nm = rv;
	return rv;
}

static bool _recv_has_raft_msgs_for_toma(void) {
	return (sys->nm->raft_msg_queue_from_other_tomas.n_msgs > 0);
}

int nvmeibt_nm_get_fd(struct nvmeibt_nm_local_node *ln) { return ln->fd; }

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

void rsrm_destroy_after_run(void) { override_close(rsrm_faults_get_fd()); }
void rsrm_faults_handle_fifo_comm(void) {}

#include "nvmeibt_global.h"
#include "nvmeibt_raft_msg_fmt.h"

int nvmeibt_nm_add_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic) {
	++ln->n_nics;
	(void)nic;
	return 0;
}

int nvmeibt_nm_del_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic) {
	--ln->n_nics;
	(void)nic;
	return 0;
}

int nvmeibt_nm_del_remote_node(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node) {
	--ln->n_connected_remote_nodes;
	BUG_ON(ln->n_connected_remote_nodes < 0);
	(void)node;
	return 0;
}
int nvmeibt_nm_cancel_req_node(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node) {
	(void)ln; (void)node;
	return 0;
}

bool nvmeibt_nm_is_remote_node_connected(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node) {
	N_Tf(__AUTOID__, "node: @STR, Check connection", node->from_config.name);
	(void)ln;
	return true;
}

void nvmeibt_nm_done(struct nvmeibt_nm_local_node *ln) {
	BUG_ON(ln != sys->nm);
	override_close(ln->fd);
	ln->fd = -1;
	if (ln->raft_msg_queue_from_other_tomas.n_msgs > 0) {		// Toma did not consume some of raft reply messages
		N_Tf(__AUTOID__, "freeing unconsumed @INT raft msgs", ln->raft_msg_queue_from_other_tomas.n_msgs);
		for (int i = 0; i < ln->raft_msg_queue_from_other_tomas.n_msgs; i++) {
			NNVMEIBT_BM_FREE(__AUTOID__, ln->raft_msg_queue_from_other_tomas.msg_q[i]);
		}
		ln->raft_msg_queue_from_other_tomas.n_msgs = 0;
	}
	sys->nm = NULL;
	BUG_ON(ln->n_total_msmgs_sent.vote_rep <= 0);
	BUG_ON(ln->n_total_msmgs_sent.append_ent_rep <= 0);
	free(ln);
	override_close(sys->TSB_srm_timer.o.sock->fd); // Like real free_local_node()
}

int nvmeibt_nm_process_toma_requests(struct nvmeibt_nm_local_node *ln) {
	struct nvmeibt_node *node;
	struct nvmeib_hash_table *h = nvmeibt_global_get_global()->nodes_hash_by_uuid;
	NVMEIB_HASH_FOREACH(node, h) {
		if (!node->conn_ctx) {
			N_Tf(__AUTOID__, "Establish connection to node: @STR", node->from_config.name);
			node->conn_ctx = (void*)0x11110000;	// Todo: Just some non null value.
		}
	}
	if (ln->raft_msg_queue_from_other_tomas.n_msgs > 0) {
		N_Tf(__AUTOID__, "@INT raft msgs arrived", ln->raft_msg_queue_from_other_tomas.n_msgs);
		for (int i = 0; i < ln->raft_msg_queue_from_other_tomas.n_msgs; i++) {
			nvmeibt_toma_dispatch_received_msg(ln->raft_msg_queue_from_other_tomas.msg_q[i]);
		}
		ln->raft_msg_queue_from_other_tomas.n_msgs = 0;
	}
	return 0;
}

int nvmeibt_nm_queue_srm_req(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node, struct nvmeibt_msg_request *req) {
	const struct raft_msg *in_r_msg = (typeof(in_r_msg))req->cnst_msg;
	const struct nvmeibt_persist_and_wire_buf *r_topo = (typeof(r_topo))req->cnst_data;
	const enum nvmeibt_raft_msg_type in_msg_type = LE_SWAP32((uint32_t)in_r_msg->msg_type);
	N_Tf(__AUTOID__, "node: @STR, received msg=@STR[@X], @INT[b]", node->from_config.name, nvmeibt_ib_protocol_signature_to_str(req->msg_type), in_msg_type, req->msg_len);
	if (req->cbs.send_c) {
		req->cbs.send_c(NULL, 0);	// Ack that message was sent to peer
	}
	{ // Add reply to to list, no needs for locks. Accessed only from Toma main threads
		// const int node_idx = sb_cluster_conf_find_node_idx_by_name(&sys->cfg, node->from_config.name);
		struct t_raft_msg_queue_from_other_tomas *rq = &ln->raft_msg_queue_from_other_tomas;
		struct nvmeibt_big_msg *msg = NNVMEIBT_BM_CALLOC(__AUTOID__, sizeof(*msg) + req->msg_len + req->data_len);
		struct raft_msg *out_r_msg = (typeof(out_r_msg))msg->data;
		BUG_ON(rq->n_msgs >= (int)ARRAY_SIZE(rq->msg_q) || (req->msg_type != NVMEIBT_IB_PROTOCOL_SIGNATURE_RAFT));
		msg->msg_type = req->msg_type;
		msg->data_len = (req->msg_len + req->data_len);		// Reply has the same length/payload as request
		memcpy(out_r_msg, in_r_msg, req->msg_len);			// DHS: Copy the incomming message as a reply so most fields would be already initialized
		out_r_msg->src_node_id =  in_r_msg->dst_node_id;	// Switch 'src' and 'dst' which will make them both correct
		out_r_msg->dst_node_id =  in_r_msg->src_node_id;
		out_r_msg->src_node_idx = in_r_msg->dst_node_idx;
		out_r_msg->dst_node_idx = in_r_msg->src_node_idx;
		switch (in_msg_type) {
			case RAFT_MSG_REQ_VOTE:
				out_r_msg->msg_type = LE_SWAP32(RAFT_MSG_REQ_VOTE_REP);
				out_r_msg->is_vote_granted = true;			// Currently always vote for live toma.
				ln->n_total_msmgs_sent.vote_rep++;
				break;
			case RAFT_MSG_APPEND_ENTRIES:
				out_r_msg->msg_type = LE_SWAP32(RAFT_MSG_APPEND_ENTRIES_REP);
				if (req->data_len)
					memcpy(out_r_msg->persist_and_wire_buf.data, req->cnst_data, req->data_len);	// DHS: Copy the incomming topology as a reply. All fields are ok. Todo: Parse and analyze degraded modes
				out_r_msg->is_vote_granted = true;			// Relevant for Node which joins already existing quorum with leader
				ln->n_total_msmgs_sent.append_ent_rep++;
				break;
			default: BUG_ON(true);							// Currently only support reply as follower on leader/candidate msgs
		}
		out_r_msg->raft_hdr_crc = 0;
		out_r_msg->raft_hdr_crc = LE_SWAP32(crc32(0, out_r_msg, sizeof(*out_r_msg)));
		rq->msg_q[rq->n_msgs++] = msg;
	}
	return 0;
}

int nvmeibt_nm_rsrm_send_timer(struct nvmeibt_nm_local_node *ln) {
	(void)ln; return 0;
}
void nvmeibt_nm_rsrm_faults_handle_fifo_com(struct nvmeibt_nm_local_node *ln) {
	(void)ln;
}

void nvmeibt_nm_rsrm_resend_acks(struct nvmeibt_nm_local_node *ln) {
	(void)ln;
}

int nvmeibt_nm_print_status_json(void *ctx, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx) {
	const struct nvmeibt_nm_local_node *ln = ctx;
	if (ln) {
		(*printf_fn)(printf_ctx, "fd=%d, n_nodes=%d, n_nics=%d\n", ln->fd, ln->n_connected_remote_nodes, ln->n_nics);
	}
	return 0;
}
int nvmeibt_nm_print_status(void *ctx, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx) { return nvmeibt_nm_print_status_json(ctx, printf_fn, printf_ctx); }

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
