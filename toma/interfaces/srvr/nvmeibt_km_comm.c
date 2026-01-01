#include "nvmeibt_srvr_proc.h"
#include "nvmeibt_common.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <sys/select.h>
#include "nvmeibt_ds.h"

struct srv_comm_msg {
	void (*on_done)(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *msg);
	void *ctx;								// ctx for on_done()
	struct xdlist link;						// Link to reside in msg lists or in progress list
	struct nvmeib_nl_uk_comm_msg msg;
};

static void msg_free(struct srv_comm_msg *msg) {
	if (msg->on_done)
		msg->on_done(msg->ctx, false, NULL);
	NNVMEIBT_BM_FREE(ttkmcmf0, msg);		// Did not get any reply
}

struct change_disk_cb {
	struct nvmeib_register_change_disk cb;
	struct xdlist link;
};

struct disk_info {
	struct nvmeib_disk_info disk;
	struct nvmeib_remove_disk rm_disk;
	bool remove_in_progress;
	unsigned long ack_id;
	struct xdlist link;
};

bool disk_info_is_equal(const struct disk_info *di, const char* disk_name) {
	return !memcmp(di->disk.disk_id, disk_name, sizeof(di->disk.disk_id));
}

typedef XDLIST_DECLARE(msgs_list, struct srv_comm_msg,   link) msgs_list_t;
typedef XDLIST_DECLARE(cb_list,   struct change_disk_cb, link) cb_list_t;
typedef XDLIST_DECLARE(disk_list, struct disk_info,      link) disk_list_t;
struct nvmeibt_km_comm {
	msgs_list_t msgs1, msgs2;		// Double buffering, 1 list is draining, other getting new requests
	msgs_list_t *msgs;				// Points to current list of added entries (1 of the 2 above) ????
	msgs_list_t in_progress_msgs;	// In air messages, sent to server and awaiting reply, accessed only from main thread, or when it is dead, so no need for locks
	cb_list_t cbs;					// List of user callbacks to execute on disk add/remove events
	disk_list_t disks;
	pthread_mutex_t guard;			// Serialize Toma thread access
	int nl_sock_fd;					// Socket to which send/recv message to/from kernel server
	int max_msg_size;
	struct nlmsghdr *nlh;			// Linux netlink msg
	struct sockaddr_nl dest_addr;	// Netlink address to send msgs to
	int spair[2];					// Toma sends msgs to spair[0], our main thread selects on spair[1]. Read from spair[1] and passes msg to kernel or dispatch internally
	pthread_t comm_thread;			// main thread which processes messages
	int error_occured;				// if != 0: Object is not operational, closing due to error. Stores error code
	unsigned long unique_id_generator;		// Ever increasing counter for msg id and others
};

static unsigned long get_guid(struct nvmeibt_km_comm *p)
{
	return __sync_add_and_fetch(&p->unique_id_generator, 1);
}

static int nvmeibt_km_comm_lock(struct nvmeibt_km_comm *p)
{
	const int rv = pthread_mutex_lock(&p->guard);
	if (rv != 0) N_Ef(kmtscl0, "Failed to lock srv comm guard rv=@RV @AUTO_ERRNO", rv);
	return rv;
}

static int nvmeibt_km_comm_unlock(struct nvmeibt_km_comm *p)
{
	int rv = pthread_mutex_unlock(&p->guard);
	if (rv != 0) N_Ef(kmtscl1, "Failed to unlock srv comm guard rv=@RV @AUTO_ERRNO", rv);
	return rv;
}

static int start_netlink_socket(struct nvmeibt_km_comm *p)
{
	int sock_fd = NNVMEIBT_SOCKET(tscnlss0, PF_NETLINK, SOCK_RAW, NETLINK_SRV_COMM);
	struct sockaddr_nl src_addr;

	if (sock_fd < 0) {
		N_ETf(tscnlss1, "Fail to create netlink socket - @AUTO_ERRNO");
		return -1;
	}
	memset(&src_addr, 0, sizeof(src_addr));
	src_addr.nl_family = AF_NETLINK;
	src_addr.nl_pid = /*pthread_self() << 16 | */getpid();

	if (bind(sock_fd, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
		N_ETf(tscnlss3, "Fail to bind netlink socket - @AUTO_ERRNO");
		NNVMEIBT_CLOSE(tscnlss4, sock_fd);
		return -2;
	} else {
		memset(&p->dest_addr, 0, sizeof(p->dest_addr));
		p->dest_addr.nl_family = AF_NETLINK;
		p->dest_addr.nl_pid = 0; /* For Linux Kernel */
		p->dest_addr.nl_groups = 0; /* unicast */
		p->nl_sock_fd = sock_fd;
		return 0;
	}
}

static void * run(void *v);
static int start_thread(struct nvmeibt_km_comm *p)
{
	pthread_attr_t attr;
	p->comm_thread = 0;
	if ((pthread_attr_init(&attr) != 0) ||
		(pthread_create(&p->comm_thread, &attr, run, p) != 0)) {
		p->comm_thread = 0;
		N_Ef(tscnlss6, "Fail to create srv comm thread @AUTO_ERRNO");
		return -1;
	}
	pthread_setname_np(p->comm_thread, "km_comm_srv");
	return 0;
}

#define NETLINK_SRV_COMM_MAX_PAYLOAD 1024			//	TODO(NVMESH-7336, "Should actually use sezof largest msg")
struct nvmeibt_km_comm * nvmeibt_km_comm_create(void)
{
	struct nvmeibt_km_comm *p = NNVMEIBT_TOMA_CALLOC(tscnlssa, 1, sizeof(*p));
	int rv = 0;

	if (!p) { 													rv = -__LINE__; goto out; }
	if (pthread_mutex_init(&p->guard, NULL) < 0) { 				rv = -__LINE__; goto free_p; }
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, p->spair) < 0) { 	rv = -__LINE__; goto free_guard; }
	if (start_netlink_socket(p))  { 							rv = -__LINE__; goto free_spair; }
	if (nvmeibt_nonblock_fd(p->spair[1]) < 0) {					rv = -__LINE__; goto free_netlink; }
	p->max_msg_size = NLMSG_SPACE(NETLINK_SRV_COMM_MAX_PAYLOAD);
	p->nlh = NNVMEIBT_TOMA_MALLOC(tscnlssb, p->max_msg_size);
	if (!p->nlh) {												rv = -__LINE__; goto free_netlink; }
	XDLIST_HEAD_INIT(&p->in_progress_msgs);
	XDLIST_HEAD_INIT(&p->cbs);
	XDLIST_HEAD_INIT(&p->disks);
	XDLIST_HEAD_INIT(&p->msgs1);
	XDLIST_HEAD_INIT(&p->msgs2);
	p->msgs = &p->msgs1;
	if (start_thread(p) < 0) {									rv = -__LINE__; goto free_nl_buffer;}
	goto out;

free_nl_buffer:	NNVMEIBT_TOMA_FREE(tscnlssc, p->nlh);
free_netlink:	NNVMEIBT_CLOSE(tscnlssd, p->nl_sock_fd);
free_spair:		NNVMEIBT_CLOSE(tscnlsse, p->spair[0]);
				NNVMEIBT_CLOSE(tscnlssf, p->spair[1]);
free_guard:		pthread_mutex_destroy(&p->guard);
free_p:			NNVMEIBT_TOMA_FREE(tscnlssg, p);
				N_Ef(tscnlssh, "Failed on rv=@INT, aborting. @AUTO_ERRNO", rv);
out:
	NFOUT;
	return p;
}

static void __remove_disk_and_free(struct nvmeibt_km_comm *p, struct disk_info *disk)
{
	nvmeibt_km_comm_lock(p);
	XDLIST_DEL(&disk->link);
	nvmeibt_km_comm_unlock(p);
	NNVMEIBT_TOMA_FREE(tscnlssn, disk);
}

void nvmeibt_km_comm_delete(struct nvmeibt_km_comm *p)
{
	NFIN;
	if (p->comm_thread) {		// Block until main thread is stopped and join it
		const struct km_comm_msg_hdr msg = {.len = 0, .opcode = (int)csc_start - 1, .on_done = NULL };	// csc_internal_suicide
		N_Tf(tscnlsst, "Send internal suicide message, to main thread");
		nvmeibt_km_comm_send(p, &msg);
		if (pthread_join(p->comm_thread, NULL)) {
			N_Ef(tscnlssk, "join failed @PTHREAD, @AUTO_ERRNO", p->comm_thread);
		}
		p->comm_thread = 0;
		N_Tf(tscnlssu, "Main thread down");
	}
	while (!XDLIST_EMPTY(p->msgs)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(p->msgs);
		XDLIST_DEL(&msg->link);
		msg_free(msg);
	}
	while (!XDLIST_EMPTY(&p->in_progress_msgs)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(&p->in_progress_msgs);
		XDLIST_DEL(&msg->link);
		msg_free(msg);
	}
	while (!XDLIST_EMPTY(&p->cbs)) {
		struct change_disk_cb *cb = XDLIST_FIRST(&p->cbs);
		XDLIST_DEL(&cb->link);
		NNVMEIBT_TOMA_FREE(tscnlssm, cb);
	}
	while (!XDLIST_EMPTY(&p->disks)) {
		__remove_disk_and_free(p, XDLIST_FIRST(&p->disks));
	}
	NNVMEIBT_TOMA_FREE(tscnlsso, p->nlh);
	pthread_mutex_destroy(&p->guard);
	NNVMEIBT_CLOSE(tscnlssp, p->spair[0]);
	NNVMEIBT_CLOSE(tscnlssq, p->spair[1]);
	NNVMEIBT_CLOSE(tscnlssr, p->nl_sock_fd);
	NNVMEIBT_TOMA_FREE(tscnlsss, p);
	NFOUT;
}

static void read_toma_wakeup_event(struct nvmeibt_km_comm *p)
{
	char c;
	NFIN;
	while (read(p->spair[1], &c, 1) == 1);
	NFOUT;
}

static void send_msg_to_kernel(struct nvmeibt_km_comm *p, struct srv_comm_msg *msg, bool is_toma_explicit_msg)
{
	struct iovec iov;
	struct msghdr hdr = {0};
	struct nlmsghdr *nlh = p->nlh;
	int len = p->max_msg_size;

	NFIN;
	if (msg->msg.len > len) {
		N_Ef(tkmcsmtk0, "message size @LEN exceeds max netlink message @LEN", msg->msg.len, len);
		msg_free(msg);
		goto out;
	}
	msg->msg.caller_type = TOMA_CALLER;
	memset(nlh, 0, len);
	nlh->nlmsg_len = len;
	nlh->nlmsg_pid = getpid();
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_type = NVMESH_NL_MSG_TYPE;
	memcpy(NLMSG_DATA(nlh), &msg->msg, msg->msg.len);
	iov.iov_base = (void *)nlh;
	iov.iov_len = nlh->nlmsg_len;
	hdr.msg_name = (void *)&p->dest_addr;
	hdr.msg_namelen = sizeof(p->dest_addr);
	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;
	N_Tf(tkmcsmtk1, "msg[@INT].id=@ID to kernel pid=@PID(@PID)", msg->msg.opcode, msg->msg.id, nlh->nlmsg_pid, getpid());
	if (msg->on_done)
		XDLIST_ADD_TAIL(&p->in_progress_msgs, msg);		// Important: Insert before calling send, as reply can come fast and not find the in progress message
	sendmsg(p->nl_sock_fd, &hdr, 0);					// Todo: Check for error
	if (!msg->on_done && is_toma_explicit_msg)			// Internally generated messages are always on stack and dont have on_done() (for simplicity of code)
		msg_free(msg);
out:
	NFOUT;
}

static void __on_user_registers_new_callbacks(struct nvmeibt_km_comm *p, const struct srv_comm_msg *msg)
{
	struct change_disk_cb *cb = NNVMEIBT_TOMA_CALLOC(tscnuc0, 1, sizeof(*cb));
	struct disk_info *disk;

	NFIN;
	if (cb) {
		cb->cb = *(struct nvmeib_register_change_disk *)msg->msg.data;
		XDLIST_ADD_TAIL(&p->cbs, cb);
		XDLIST_FOREACH_SAFE(disk, &p->disks)
			cb->cb.on_add_disk(&disk->disk);
	}
	NFOUT;
}

static bool __handle_incomming_msg_from_toma(struct nvmeibt_km_comm *p)
{
	msgs_list_t *msgs;
	bool is_alive = true;

	NFIN;
	nvmeibt_km_comm_lock(p);
	msgs = p->msgs;
	p->msgs = (msgs == &p->msgs1) ? &p->msgs2 : &p->msgs1;
	nvmeibt_km_comm_unlock(p);
	while (!XDLIST_EMPTY(msgs)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(msgs);
		XDLIST_DEL(&msg->link);
		N_Tf(tkmcsmtk2, "msg[@INT].id=@ID", msg->msg.opcode, msg->msg.id);
		if (msg->msg.opcode <= csc_start) 				// Suicide message arrived
			is_alive = false;
		if (is_alive) {
			if (msg->msg.opcode == csc_register_disk_events) {
				if (!p->error_occured)
					__on_user_registers_new_callbacks(p, msg);
				msg_free(msg);
			} else if (msg->msg.opcode < csc_end) {
				send_msg_to_kernel(p, msg, true);		// Dont free msg, it is added to a different queue or freed inside
			}
		} else {										// Autofail msg
			msg_free(msg);
		}
	}
	NFOUT;
	return is_alive;
}

static struct srv_comm_msg *find_in_progress_msg_waiting_for_reply(struct nvmeibt_km_comm *p, unsigned long id)
{
	struct srv_comm_msg *msg;
	XDLIST_FOREACH_SAFE(msg, &p->in_progress_msgs) {
		if (msg->msg.id == id) {
			XDLIST_DEL(&msg->link);
			return msg;
		}
	}
	return NULL;
}

extern int nvmeibt_handle_serjio_state_changed_from_nl_ctx(const char* ldisk_id, u16 vendor_id, const char *model_str, enum nvmeibs_serjio_status serjio_status);
static void remove_disk_ack(struct nvmeibt_km_comm *p, const struct nvmeib_disk_info *di);
static void __process_disk(struct nvmeibt_km_comm *p, const struct nvmeib_nl_uk_comm_msg *rcv_msg)
{
	struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep *)rcv_msg->data;
	struct disk_info *disk;
	struct change_disk_cb *cb;

	NFIN;
	if (rep->error == csce_ok) {
		struct nvmeib_disk_info_reply *disk_rep = container_of(rep, struct nvmeib_disk_info_reply, base);
		const struct nvmeib_disk_info *di = &disk_rep->dinfo.disk;
		switch (disk_rep->selector) {
		case nvmeib_disk_info_reply_dummy:
			// Obsolete. Need to be removed from the ENUM in order to avoid compilation warning
			break;
		case nvmeib_disk_info_reply_serjio_state: {
			const struct nvmeib_disk_info_rep_sej_state_t *ssc = &disk_rep->serjio_state_change;
			nvmeibt_handle_serjio_state_changed_from_nl_ctx(ssc->disk_id, ssc->vendor_id, ssc->model_str, ssc->serjio_status);
			break;
		}
		case nvmeib_disk_info_reply_dinfo:
			if (di->n_blocks > 0) {
				disk = NNVMEIBT_TOMA_CALLOC(t2cnlpd1, 1, sizeof(*disk));
				if (disk) {
					N_Tf(t2cnlpd2, "Adding disk=@STR", di->disk_id);
					disk->disk = *di;
					nvmeibt_km_comm_lock(p);
					XDLIST_ADD_TAIL(&p->disks, disk);
					nvmeibt_km_comm_unlock(p);
					XDLIST_FOREACH(cb, &p->cbs)
						cb->cb.on_add_disk(&disk->disk);
					nvmeibt_handle_serjio_state_changed_from_nl_ctx(di->disk_id, di->vendor_id, di->model_str, disk_rep->dinfo.serjio_status);
				} else {
					N_Ef(t2cnlpd3, "Failed to allocate memory for disk=@STR", di->disk_id);
				}
			} else {
				bool found_disk = false;
				XDLIST_FOREACH(disk, &p->disks) {
					if (disk_info_is_equal(disk, di->disk_id)) {
						found_disk = true;
						disk->remove_in_progress = true;
						disk->ack_id = get_guid(p);
						memcpy(disk->rm_disk.disk_id, disk->disk.disk_id, sizeof(disk->rm_disk.disk_id));
						disk->rm_disk.vendor_id = disk->disk.vendor_id;
						disk->rm_disk.ack_id = disk->ack_id;
						N_Tf(t2cnlpd4, "Removing disk=@STR", di->disk_id);
						XDLIST_FOREACH(cb, &p->cbs) {
							cb->cb.on_remove_disk(&disk->rm_disk);
						}
						break;
					}
				}
				// Ack the remove. When the WQ is finalized we will unmap the lock table, releasing the disk.
				remove_disk_ack(p, &disk_rep->dinfo.disk);
				if (found_disk) {
					__remove_disk_and_free(p, disk);
				}
			}
			break;
		}
	}
	NFOUT;
}

static bool __handle_new_srvr_msg(struct nvmeibt_km_comm *p)
{
	struct sockaddr_nl src_addr;
	struct iovec iov[1];
	struct msghdr hdr = {0};
	struct nlmsghdr *nlh = p->nlh;
	ssize_t n;
	bool is_alive = true;

	NFIN;
	memset(nlh, 0, p->max_msg_size);
	iov[0].iov_base = (void *)nlh;
	iov[0].iov_len = p->max_msg_size;
	hdr.msg_name = (void *)&src_addr;
	hdr.msg_namelen = sizeof(src_addr);
	hdr.msg_iov = iov;
	hdr.msg_iovlen = 1;
	n = recvmsg(p->nl_sock_fd, &hdr, 0);
	if (n == -1) {
		N_Ef(t2shnnm0, "Failed to recieve message from kernel");
		is_alive = false;
	} else {
		struct nvmeib_nl_uk_comm_msg *rcv_msg = NLMSG_DATA(nlh);
		struct srv_comm_msg *msg = find_in_progress_msg_waiting_for_reply(p, rcv_msg->id);
		N_Tf(t2shnnm1, "rcv_msg[@INT].id=@ID, @LEN[b], was_blocking=@BOOL_YN, n=@ZU[b]", rcv_msg->opcode, rcv_msg->id, rcv_msg->len, !!msg, n);
		if (msg) {
			if (msg->on_done) {
				struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep *)rcv_msg->data;
				N_Tf(t2shnnm3, "Calling callback for msg @ID rp_rv=@RV", msg->msg.id, rep->error);
				msg->on_done(msg->ctx, (rep->error == csce_ok), rep);
				msg->on_done = NULL;
			}
			msg_free(msg);
		} else if (rcv_msg->opcode == csc_get_disks) {			// Reply to internal periodic get disks message
			__process_disk(p, rcv_msg);
		} else if (rcv_msg->opcode == csc_msg_to_process) {		// Server initiated extended msg
			extern int nvmeibt_add_local_clnt_msg_to_toma_nl_queue(const struct nvmeib_push_extended_msg *);
			struct nvmeib_nl_msg_to_toma *tm = (void*)rcv_msg->data;
			nvmeibt_add_local_clnt_msg_to_toma_nl_queue(&tm->payload.extended_msg);
		} else {
			N_Ef(t2shnnm5, "Got a message from kernel that no one was waiting for");
		}
	}
	NFOUT;
	return is_alive;
}

static bool __release_msg_queues_on_error(struct nvmeibt_km_comm *p, const char *reason)
{
	msgs_list_t *msgs;
	bool is_alive = true;

	N_Ef(t2srmqon0, "Error: @STR, @AUTO_ERRNO", reason);
	nvmeibt_km_comm_lock(p);
	read_toma_wakeup_event(p);
	p->error_occured = -1;				// No need to deferntiate by 'reason', we have it in logs
	msgs = p->msgs;
	p->msgs = msgs == &p->msgs1 ? &p->msgs2 : &p->msgs1;
	while (!XDLIST_EMPTY(msgs)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(msgs);
		XDLIST_DEL(&msg->link);
 		if (msg->msg.opcode < csc_start)
			is_alive = false;
		msg_free(msg);
	}
	while (!XDLIST_EMPTY(&p->in_progress_msgs)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(&p->in_progress_msgs);
		XDLIST_DEL(&msg->link);
		msg_free(msg);
	}
	nvmeibt_km_comm_unlock(p);
	if (is_alive) {
		char c;
		N_Tf(t2scqrl1, "Wait for wakeup to terminate main loop");
		if (nvmeibt_fd_set_blocking(p->spair[1], 1))
			read(p->spair[1], &c, 1);
		N_Tf(t2scqrl2, "Wakeup arrived");
	}
	NFOUT;
	return false;	// should stop due to error
}

static void remove_disk_ack(struct nvmeibt_km_comm *p, const struct nvmeib_disk_info *di)
{
	char buf[sizeof(struct srv_comm_msg) + sizeof(struct nvmeib_remove_disk)];
	struct srv_comm_msg *kmsg = (void *)buf;
	struct nvmeib_remove_disk *rd = (void *)kmsg->msg.data;
	memset(buf, 0, sizeof(buf));
	kmsg->msg.opcode = csc_remove_disk_ack;
	kmsg->msg.len = sizeof(kmsg->msg) + sizeof(struct nvmeib_remove_disk);
	memcpy(rd->disk_id, di->disk_id, sizeof(di->disk_id));
	send_msg_to_kernel(p, kmsg, false);
}

static void get_disks(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg kmsg;
	memset(&kmsg, 0, sizeof(kmsg));
	kmsg.msg.opcode = csc_get_disks;
	kmsg.msg.len = sizeof(kmsg.msg);
	send_msg_to_kernel(p, &kmsg, false);
}

static void _send_keep_alive_to_server(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg kmsg;
	memset(&kmsg, 0, sizeof(kmsg));
	kmsg.msg.opcode = csc_keep_alive;
	kmsg.msg.len = sizeof(kmsg.msg);
	send_msg_to_kernel(p, &kmsg, false);
}

static void * run(void *v)
{
	struct nvmeibt_km_comm *p = v;
	fd_set read_fds, write_fds, except_fds;			// For select
	const int max_fd = max(p->spair[1], p->nl_sock_fd);
	bool is_alive = true;
	int n;

	NFIN;
	get_disks(p);
	while (is_alive) {
		struct timeval tv = {.tv_sec = TOMA_SILENCE_MAX_PERIOD_SECS, .tv_usec = 0};
		FD_ZERO(&read_fds);
		FD_SET(p->spair[1], &read_fds);
		FD_SET(p->nl_sock_fd, &read_fds);
		FD_ZERO(&write_fds);					// We dont write anything, just wakeup on incomming msg from server or from toma
		except_fds = read_fds;
		n = select(max_fd + 1, &read_fds, &write_fds, &except_fds, &tv);
		if (n > 0) {
			if (FD_ISSET(p->spair[1], &read_fds)) {
				read_toma_wakeup_event(p);
				is_alive = __handle_incomming_msg_from_toma(p);
			} else if (FD_ISSET(p->nl_sock_fd, &read_fds)) {
				is_alive = __handle_new_srvr_msg(p);
			} else if (FD_ISSET(p->spair[1], &except_fds)) {
				is_alive = __release_msg_queues_on_error(p, "toma sock");
			} else if (FD_ISSET(p->nl_sock_fd, &except_fds)) {
				is_alive = __release_msg_queues_on_error(p, "server sock");
			} else {
				N_Wf(warn_km_comm_run, " select triggered none of our fd");
			}
		} else if (n == 0) {	// timeout
			N_Df(trace_km_comm_run, "Timeout");
			is_alive = __handle_incomming_msg_from_toma(p);		// Try for the chance we missed an event
			if (is_alive) {
				XDLIST_EMPTY(&p->disks) ? get_disks(p) : _send_keep_alive_to_server(p);
			}
		} else {
			is_alive = __release_msg_queues_on_error(p, "select");
		}
	}
	NFOUT;
	return NULL;
}

int nvmeibt_km_comm_send(struct nvmeibt_km_comm *p, const struct km_comm_msg_hdr *hdr)
{
	struct srv_comm_msg *kmsg;
	const int		msg_size = sizeof(kmsg->msg) + hdr->len;
	char c = 1;
	int rv;

	NFIN;
	if (hdr->opcode == csc_start || hdr->opcode >= csc_end) {
		N_Ef(stkmcnl0, "Invalid kernel message @INT", hdr->opcode);
		rv = -1;
		goto out;
	}
	if (!(kmsg = NNVMEIBT_BM_CALLOC(stkmcnl1, sizeof(*kmsg) + msg_size))) {
		N_Ef(stkmcnl2, "Fail to allocate nvmeibt_km_comm msg");
		rv = -1;
		goto out;
	}
	kmsg->msg.opcode = hdr->opcode;
	if (hdr->opcode > csc_start) {
		kmsg->on_done = hdr->on_done;
		kmsg->ctx =  hdr->ctx;
		kmsg->msg.len = msg_size;
		kmsg->msg.id = get_guid(p);
		memcpy(kmsg->msg.data, hdr->data, hdr->len);
		N_Tf(stkmcnl3, "msg[@INT].id=@ID, hdr=@INT[b] msg=@INT[b]", hdr->opcode, kmsg->msg.id, hdr->len, kmsg->msg.len);
	}
	nvmeibt_km_comm_lock(p);
	if (!p->error_occured) {
		XDLIST_ADD_TAIL(p->msgs, kmsg);
		rv = write(p->spair[0], &c, 1) == 1 ? 0 : -1;		// Wakeup our main thread to handle the message
	} else {
		rv = -1;
	}
	nvmeibt_km_comm_unlock(p);
	goto out;

out:
	NFOUT;
	return rv;
}

int nvmeibt_km_comm_register_disk_events(struct nvmeibt_km_comm *p, const struct nvmeib_register_change_disk *cbs)
{
	char buf[sizeof(struct km_comm_msg_hdr) + sizeof(struct nvmeib_register_change_disk)] = {0};
	struct km_comm_msg_hdr *msg = (void *)buf;
	int rv;

	NFIN;
	msg->len = sizeof(struct nvmeib_register_change_disk);
	msg->opcode = csc_register_disk_events;
	*((struct nvmeib_register_change_disk *)msg->data) = *cbs;
	rv = nvmeibt_km_comm_send(p, msg);			// Wakeup our main thread to process the message, never reaches the server
	NFOUT;
	return rv;
}

int nvmeibt_km_comm_get_disk_info(struct nvmeibt_km_comm *p, const char *disk_name, struct nvmeib_disk_info *di)
{
	struct disk_info *disk;
	int rv = -1;

	NFIN;
	nvmeibt_km_comm_lock(p);
	XDLIST_FOREACH(disk, &p->disks) {
		if (disk_info_is_equal(disk, disk_name)) {
			if (di)
				*di = disk->disk;
			rv = 0;
			break;
		}
	}
	nvmeibt_km_comm_unlock(p);
	NFOUT;
	return rv;
}
