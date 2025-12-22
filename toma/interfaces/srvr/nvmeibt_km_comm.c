#include "nvmeibt_srvr_proc.h"
#include "nvmeibt_common.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <getopt.h>
#include <sys/file.h>
#include <sys/ucontext.h>
#include <sys/un.h>
#include <pthread.h>
#include "nvmeibt_ds.h"

struct srv_comm_msg {
	unsigned long time;
    void (*on_done)(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *msg);
    void *ctx;
	struct xdlist link;
	bool remember;
	struct nvmeib_nl_uk_comm_msg msg;
};

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

typedef XDLIST_DECLARE(msgs_list, struct srv_comm_msg, link) msgs_list_t;
typedef XDLIST_DECLARE(cb_list, struct change_disk_cb, link) cb_list_t;
typedef XDLIST_DECLARE(disk_list, struct disk_info, link) disk_list_t;
struct nvmeibt_km_comm {
	msgs_list_t msgs1;
	msgs_list_t msgs2;
	msgs_list_t *msgs;
	msgs_list_t in_progress_msgs;
	cb_list_t cbs;
	disk_list_t disks;
	pthread_mutex_t guard;
	int nl_sock_fd;
	struct nlmsghdr *nlh;
	int nlh_len;
	struct sockaddr_nl dest_addr;
	int spair[2];
	pthread_t comm_thread;
	int valid;
	int thread_started;
	unsigned long guid;
};

static unsigned long get_guid(struct nvmeibt_km_comm *p)
{
	return __sync_add_and_fetch(&p->guid, 1);
}

static int start_netlink_sockt(struct nvmeibt_km_comm *p)
{
	int sock_fd = -1;
	struct sockaddr_nl src_addr;
	int rv = -1;

	NFIN;
	if ((sock_fd = NNVMEIBT_SOCKET(trace_km_comm_start_netlink_sockt, PF_NETLINK, SOCK_RAW, NETLINK_SRV_COMM)) < 0) {
		N_ETf(error_km_comm_start_netlink_sockt, "Fail to create netlink socket - @AUTO_ERRNO");
		goto out;
	}
	memset(&src_addr, 0, sizeof(src_addr));
	src_addr.nl_family = AF_NETLINK;
	src_addr.nl_pid = /*pthread_self() << 16 | */getpid();

	if (bind(sock_fd, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
		N_ETf(error_1_km_comm_start_netlink_sockt, "Fail to bind netlink socket - @AUTO_ERRNO");
		goto free_nl;
	}

	memset(&p->dest_addr, 0, sizeof(p->dest_addr));
	p->dest_addr.nl_family = AF_NETLINK;
	p->dest_addr.nl_pid = 0; /* For Linux Kernel */
	p->dest_addr.nl_groups = 0; /* unicast */
	p->nl_sock_fd = sock_fd;
	rv = 0;
	goto out;

free_nl:
	NNVMEIBT_CLOSE(trace_1_km_comm_start_netlink_sockt, sock_fd);

out:
	NFOUT;
	return rv;
}

static void * run(void *v);
static int start_thread(struct nvmeibt_km_comm *p)
{
	pthread_attr_t attr;
	int rv;

	NFIN;
	if ((rv = pthread_attr_init(&attr)) != 0 ||
		(rv = pthread_create(&p->comm_thread, &attr, run, p)) != 0) {
		N_Ef(error_km_comm_start_thread, "Fail to create srv comm thread @AUTO_ERRNO");
		rv = -1;
	}
	else {
		pthread_setname_np(p->comm_thread, "km_comm_srv");
		rv = 0;
	}
	NFOUT;
	return rv;
}

#define NETLINK_SRV_COMM_MAX_PAYLOAD 1024			//	TODO(NVMESH-7336, "Should actually use sezof largest msg")
struct nvmeibt_km_comm * nvmeibt_km_comm_create(void)
{
	struct nvmeibt_km_comm *p;

	NFIN;
	if (!(p = NNVMEIBT_TOMA_CALLOC(trace_km_comm_nvmeibt_km_comm_create, 1, sizeof(*p)))) {
		N_Ef(error_km_comm_nvmeibt_km_comm_create, "failed to allocate nvmeibt_km_comm object");
		goto out;
	}
	if (pthread_mutex_init(&p->guard, NULL) < 0) {
		N_Ef(error_1_km_comm_nvmeibt_km_comm_create, "Failed to create guard - @AUTO_ERRNO");
		goto free_p;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, p->spair) < 0) {
		N_Ef(error_2_km_comm_nvmeibt_km_comm_create, "Fail to create socket pairs - @AUTO_ERRNO");
		goto free_guard;
	}
	if (start_netlink_sockt(p))
		goto free_spair;
	if (nvmeibt_nonblock_fd(p->spair[1]) < 0) {
		N_Ef(error_3_km_comm_nvmeibt_km_comm_create, "fcntl on fd @FD", p->spair[1]);
		goto free_netlink;
	}
	p->nlh_len = NLMSG_SPACE(NETLINK_SRV_COMM_MAX_PAYLOAD);
	if (!(p->nlh = NNVMEIBT_TOMA_MALLOC(trace_0_km_comm_nvmeibt_km_comm_create, p->nlh_len))) {
		N_Ef(error_4_km_comm_nvmeibt_km_comm_create, "Failed to allocate netlink message");
		goto free_netlink;
	}
	XDLIST_HEAD_INIT(&p->in_progress_msgs);
	XDLIST_HEAD_INIT(&p->cbs);
	XDLIST_HEAD_INIT(&p->disks);
	XDLIST_HEAD_INIT(&p->msgs1);
	XDLIST_HEAD_INIT(&p->msgs2);
	p->msgs = &p->msgs1;
	p->valid = 1;
	if (start_thread(p) < 0) {
		goto free_nl_buffer;
	}
	else {
		p->thread_started = 1;
	}
	goto out;

free_nl_buffer:
	NNVMEIBT_TOMA_FREE(trace_1_km_comm_nvmeibt_km_comm_create, p->nlh);

free_netlink:
	NNVMEIBT_CLOSE(trace_2_km_comm_nvmeibt_km_comm_create, p->nl_sock_fd);

free_spair:
	NNVMEIBT_CLOSE(trace_3_km_comm_nvmeibt_km_comm_create, p->spair[0]);
	NNVMEIBT_CLOSE(trace_4_km_comm_nvmeibt_km_comm_create, p->spair[1]);

free_guard:
	pthread_mutex_destroy(&p->guard);

free_p:
	NNVMEIBT_TOMA_FREE(trace_5_km_comm_nvmeibt_km_comm_create, p);
	p = NULL;

out:
	NFOUT;
	return p;
}

static void stop(struct nvmeibt_km_comm *p)
{
	struct km_comm_msg_hdr msg = {0};

	NFIN;
	if (p->thread_started) {
		msg.opcode = -1;
		nvmeibt_km_comm_send(p, &msg);
		if (pthread_join(p->comm_thread, NULL)) {
			N_Ef(xx_20, "km_comm stop failed @AUTO_ERRNO");
		}
	}
	NFOUT;
}

static void msg_free(struct srv_comm_msg *msg) {
	if (msg->on_done) {
		msg->on_done(msg->ctx, false, NULL);
	}
	NNVMEIBT_BM_FREE(ttkmcmf0, msg);
}

static void remove_disk_from(struct nvmeibt_km_comm *p, struct disk_info *disk);
void nvmeibt_km_comm_delete(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg *msg;
	struct change_disk_cb *cb;
	struct disk_info *disk;

	NFIN;
	stop(p);
	while (!XDLIST_EMPTY(p->msgs)) {
		msg = XDLIST_FIRST(p->msgs);
		XDLIST_DEL(&msg->link);
		msg_free(msg);
	}
	while (!XDLIST_EMPTY(&p->in_progress_msgs)) {
		msg = XDLIST_FIRST(&p->in_progress_msgs);
		XDLIST_DEL(&msg->link);
		msg_free(msg);
	}
	while (!XDLIST_EMPTY(&p->cbs)) {
		cb = XDLIST_FIRST(&p->cbs);
		XDLIST_DEL(&cb->link);
		NNVMEIBT_TOMA_FREE(trace_2_km_comm_nvmeibt_km_comm_delete, cb);
	}
	while (!XDLIST_EMPTY(&p->disks)) {
		disk = XDLIST_FIRST(&p->disks);
		remove_disk_from(p, disk);
		NNVMEIBT_TOMA_FREE(trace_3_km_comm_nvmeibt_km_comm_delete, disk);
	}
	NNVMEIBT_TOMA_FREE(trace_3a_km_comm_nvmeibt_km_comm_delete, p->nlh);
	pthread_mutex_destroy(&p->guard);
	NNVMEIBT_CLOSE(trace_4_km_comm_nvmeibt_km_comm_delete, p->spair[0]);
	NNVMEIBT_CLOSE(trace_5_km_comm_nvmeibt_km_comm_delete, p->spair[1]);
	NNVMEIBT_CLOSE(trace_6_km_comm_nvmeibt_km_comm_delete, p->nl_sock_fd);
	NNVMEIBT_TOMA_FREE(trace_7_km_comm_nvmeibt_km_comm_delete, p);
	NFOUT;
}

static int build_fd_sets(struct nvmeibt_km_comm *p, fd_set *read_fds,
	fd_set *write_fds, fd_set *except_fds)
{
	NFIN;
	FD_ZERO(read_fds);
	FD_SET(p->spair[1], read_fds);
	FD_SET(p->nl_sock_fd, read_fds);

	FD_ZERO(write_fds);

	FD_ZERO(except_fds);
	FD_SET(p->spair[1], except_fds);
	FD_SET(p->nl_sock_fd, except_fds);
	NFOUT;
	return 0;
}

static int lock(pthread_mutex_t *m)
{
	int rv = pthread_mutex_lock(m);
	if (rv != 0) {
		N_Ef(error_km_comm_lock, "Failed to lock srv comm guard rv=@RV @AUTO_ERRNO", rv);
		rv = -1;
	}
	return rv;
}

static int unlock(pthread_mutex_t *m)
{
	int rv = pthread_mutex_unlock(m);
	if (rv != 0) {
		N_Ef(error_km_comm_unlock, "Failed to unlock srv comm guard rv=@RV @AUTO_ERRNO", rv);
		rv = -1;
	}
	return rv;
}

static void read_spair(struct nvmeibt_km_comm *p)
{
	char c;

	NFIN;
	while (read(p->spair[1], &c, 1) == 1);
	NFOUT;
}

static void send_msg_to_kernel(struct nvmeibt_km_comm *p,
	struct srv_comm_msg *msg)
{
	struct iovec iov;
	struct msghdr hdr = {0};
	struct nlmsghdr *nlh = p->nlh;
	int len = p->nlh_len;

	NFIN;
	if (msg->msg.len > len) {
		N_Ef(tkmcsmtk0, "message size @LEN exceeds max netlink message @LEN", msg->msg.len, len);
		goto error;
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
	N_Tf(tkmcsmtk1, "Sending message @ID to kernel pid=@PID(@GETPID)", msg->msg.id, nlh->nlmsg_pid, getpid());
	sendmsg(p->nl_sock_fd, &hdr, 0);
	if (msg->remember) {
		XDLIST_ADD_TAIL(&p->in_progress_msgs, msg);
	}
	goto out;

error:
	msg_free(msg);
out:
	NFOUT;
}

static void send_disks(struct nvmeibt_km_comm *p, struct change_disk_cb *cb)
{
	struct disk_info *disk;

	NFIN;
	XDLIST_FOREACH_SAFE(disk, &p->disks) {
		cb->cb.on_add_disk(cb->cb.add_ctx, &disk->disk);
	}
	NFOUT;
}

static void register_callbacks(
	struct nvmeibt_km_comm *p, const struct srv_comm_msg *msg)
{
	struct change_disk_cb *cb;

	NFIN;
	if (p->valid && (cb = NNVMEIBT_TOMA_CALLOC(trace_km_comm_register_callbacks, 1, sizeof(*cb)))) {
		cb->cb = *(struct nvmeib_register_change_disk *)msg->msg.data;
		XDLIST_ADD_TAIL(&p->cbs, cb);
		send_disks(p, cb);
	}
	NFOUT;
}

static int process_msgs(struct nvmeibt_km_comm *p, msgs_list_t *msgs)
{
	struct srv_comm_msg *msg;
	int cont = 1;

	NFIN;
	while (!XDLIST_EMPTY(msgs)) {
		msg = XDLIST_FIRST(msgs);
		XDLIST_DEL(&msg->link);
		if (cont) {
			if (msg->msg.opcode <= csc_start) {
				/* we are done */
				cont = 0;
				NNVMEIBT_BM_FREE(trace_km_comm_process_msgs, msg);
			}
			else if (msg->msg.opcode == csc_register_disk_events) {
				register_callbacks(p, msg);
				msg_free(msg);
			} else if (msg->msg.opcode < csc_end) {
				send_msg_to_kernel(p, msg);			// Dont free msg, it is added to a different queue or freed inside
			}
		} else {
			msg_free(msg);
		}
	}
	NFOUT;
	return cont;
}

static int handle_spair(struct nvmeibt_km_comm *p)
{
	msgs_list_t *msgs;
	int cont;

	NFIN;
	lock(&p->guard);
	msgs = p->msgs;
	p->msgs = msgs == &p->msgs1 ? &p->msgs2 : &p->msgs1;
	unlock(&p->guard);
	cont = process_msgs(p, msgs);
	NFOUT;
	return cont;
}

static struct srv_comm_msg * find_in_progress_msg(struct nvmeibt_km_comm *p,
	unsigned long id)
{
	struct srv_comm_msg *msg;
	int found = 0;

	NFIN;
	XDLIST_FOREACH_SAFE(msg, &p->in_progress_msgs) {
		if (msg->msg.id == id) {
			found = 1;
			XDLIST_DEL(&msg->link);
			break;
		}
	}
	NFOUT;
	return found ? msg : NULL;
}

static void announce_disk(struct nvmeibt_km_comm *p, struct disk_info *disk)
{
	struct change_disk_cb *cb;

	NFIN;
	XDLIST_FOREACH(cb, &p->cbs) {
		cb->cb.on_add_disk(cb->cb.add_ctx, &disk->disk);
	}
	NFOUT;
}

static void add_disk_to(struct nvmeibt_km_comm *p, struct disk_info *disk)
{
	NFIN;
	lock(&p->guard);
	XDLIST_ADD_TAIL(&p->disks, disk);
	unlock(&p->guard);
	NFOUT;
}

static void remove_disk_from(struct nvmeibt_km_comm *p, struct disk_info *disk)
{
	NFIN;
	lock(&p->guard);
	XDLIST_DEL(&disk->link);
	unlock(&p->guard);
	NFOUT;
}

extern int nvmeibt_handle_serjio_state_changed_from_nl_ctx(const char* ldisk_id, u16 vendor_id, char *model_str, enum nvmeibs_serjio_status serjio_status);
static void handle_serjio_state_changed(
	struct nvmeibt_km_comm *p, struct nvmeib_disk_info_reply *disk_rep)
{
	(void)p;
	nvmeibt_handle_serjio_state_changed_from_nl_ctx(
		disk_rep->serjio_state_change.disk_id,
		disk_rep->serjio_state_change.vendor_id,
		disk_rep->serjio_state_change.model_str,
		disk_rep->serjio_state_change.serjio_status);
}

static void remove_disk_ack(
	struct nvmeibt_km_comm *p, struct nvmeib_remove_disk *disk);
static void add_disk(
	struct nvmeibt_km_comm *p, struct nvmeib_nl_uk_comm_msg *rcv_msg)
{
	struct nvmeib_nl_uk_comm_rep *rep =
		(struct nvmeib_nl_uk_comm_rep *)rcv_msg->data;
	struct nvmeib_disk_info_reply *disk_rep;
	struct disk_info *disk;
	struct change_disk_cb *cb;
	struct nvmeib_remove_disk rd;
	bool found_disk;

	NFIN;
	if (rep->error == csce_ok) {
		disk_rep = container_of(rep, struct nvmeib_disk_info_reply, base);
		switch (disk_rep->selector) {
		case nvmeib_disk_info_reply_dummy:
			// Obsolete. Need to be removed from the ENUM in order to avoid compilation warning
			break;
		case nvmeib_disk_info_reply_serjio_state:
			handle_serjio_state_changed(p, disk_rep);
			break;
		case nvmeib_disk_info_reply_dinfo:
			if (disk_rep->dinfo.disk.n_blocks > 0) {
				if ((disk = NNVMEIBT_TOMA_CALLOC(trace_1_km_comm_add_disk, 1, sizeof(*disk)))) {
					N_Tf(trace_5_km_comm_add_disk, "Adding disk=@STR", disk_rep->dinfo.disk.disk_id);
					disk->disk = disk_rep->dinfo.disk;
					add_disk_to(p, disk);
					announce_disk(p, disk);
					nvmeibt_handle_serjio_state_changed_from_nl_ctx(
						disk_rep->dinfo.disk.disk_id,
						disk_rep->dinfo.disk.vendor_id,
						disk_rep->dinfo.disk.model_str,
						disk_rep->dinfo.serjio_status);
				}
				else {
					N_Ef(error_km_comm_add_disk, "Failed to allocate memory for disk=@STR",
						disk_rep->dinfo.disk.disk_id);
				}
			}
			else {
				found_disk = false;
				XDLIST_FOREACH(disk, &p->disks) {
					if (!memcmp(disk->disk.disk_id, disk_rep->dinfo.disk.disk_id,
						sizeof(disk->disk.disk_id))) {
						found_disk = true;
						disk->remove_in_progress = true;
						disk->ack_id = get_guid(p);
						memcpy(disk->rm_disk.disk_id, disk->disk.disk_id,
							sizeof(disk->rm_disk.disk_id));
						disk->rm_disk.vendor_id = disk->disk.vendor_id;
						disk->rm_disk.ack_id = disk->ack_id;
						N_Tf(trace_2_km_comm_add_disk, "Removing disk=@STR", disk_rep->dinfo.disk.disk_id);

						XDLIST_FOREACH(cb, &p->cbs) {
							cb->cb.on_remove_disk(
								cb->cb.remove_ctx, &disk->rm_disk);
						}
						break;
					}
				}

				// Ack the remove. When the WQ is finalized we will unmap the lock table, releasing the disk.
				memset(&rd, 0, sizeof(rd));
				memcpy(rd.disk_id, disk_rep->dinfo.disk.disk_id,
					sizeof(disk_rep->dinfo.disk.disk_id));
				remove_disk_ack(p, &rd);
				if (found_disk) {
					remove_disk_from(p, disk);
					NNVMEIBT_TOMA_FREE(trace_4_km_comm_add_disk, disk);
				}
			}
			break;
		}
	}
	NFOUT;
}

int nvmeibt_add_local_clnt_msg_to_toma_nl_queue(void *msg_fr_local_clnt);
static void handle_pushed_msg(
	struct nvmeibt_km_comm *p, struct nvmeib_nl_uk_comm_msg *rcv_msg)
{
	struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep *)rcv_msg->data;
	struct nvmeib_push_msg_process *msg = container_of(rep, struct nvmeib_push_msg_process, base);
	void *data = msg->start;
	NFIN;
	N_Tf(wgydyqwdgdugy, "p=@PTR", p);
	nvmeibt_add_local_clnt_msg_to_toma_nl_queue(data);
	NFOUT;
}

static int handle_new_nl(struct nvmeibt_km_comm *p)
{
	struct sockaddr_nl src_addr;
	struct iovec iov[1];
	struct msghdr hdr = {0};
	struct nlmsghdr *nlh = p->nlh;
	int len = p->nlh_len;
	struct nvmeib_nl_uk_comm_msg *rcv_msg;
	struct nvmeib_nl_uk_comm_rep *rep;
	struct srv_comm_msg *msg;
	ssize_t n;
	int cont = 1;

	NFIN;
	memset(nlh, 0, len);
	iov[0].iov_base = (void *)nlh;
	iov[0].iov_len = len;
	hdr.msg_name = (void *)&src_addr;
	hdr.msg_namelen = sizeof(src_addr);
	hdr.msg_iov = iov;
	hdr.msg_iovlen = 1;
	n = recvmsg(p->nl_sock_fd, &hdr, 0);
	if (n == -1) {
		N_Ef(error_km_comm_handle_new_nl, "Failed to recieve message from kernel");
		cont = 0;
	}
	else {
		rcv_msg = NLMSG_DATA(nlh);
		N_Tf(trace_km_comm_handle_new_nl, "rcv_msg: id=@ID, len=@LEN, n=@NNN", rcv_msg->id, rcv_msg->len, n);
		if ((msg = find_in_progress_msg(p, rcv_msg->id))) {
			N_Tf(trace_1_km_comm_handle_new_nl, "Found msg @ID", msg->msg.id);
			if (msg->on_done) {
				N_Tf(trace_2_km_comm_handle_new_nl, "Calling callback for msg @ID", msg->msg.id);
				rep = (struct nvmeib_nl_uk_comm_rep *)rcv_msg->data;
				msg->on_done(msg->ctx, (rep->error == csce_ok), rep);
			}
			NNVMEIBT_BM_FREE(trace_3_km_comm_handle_new_nl, msg);
		}
		else {
			if (rcv_msg->opcode == csc_get_disks) {
				add_disk(p, rcv_msg);
			}
			else if (rcv_msg->opcode == csc_msg_to_process) {
				handle_pushed_msg(p, rcv_msg);
			}
			else {
				N_Ef(error_1_km_comm_handle_new_nl, "Got a message from kernel that no one was waiting for");
			}
		}
	}
	NFOUT;
	return cont;
}

static int release_queue(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg *msg;
	msgs_list_t *msgs;
	int cont = 1;

	NFIN;
	lock(&p->guard);
	read_spair(p);
	p->valid = false;
	msgs = p->msgs;
	p->msgs = msgs == &p->msgs1 ? &p->msgs2 : &p->msgs1;
	while (!XDLIST_EMPTY(msgs)) {
		msg = XDLIST_FIRST(msgs);
		XDLIST_DEL(&msg->link);
		if (msg->on_done) {
			msg->on_done(msg->ctx, false, NULL);
		} else if (msg->msg.opcode < 0) {
			cont = 0;
		}
		NNVMEIBT_BM_FREE(trace_km_comm_release_queue, msg);
	}
	while (!XDLIST_EMPTY(&p->in_progress_msgs)) {
		msg = XDLIST_FIRST(&p->in_progress_msgs);
		XDLIST_DEL(&msg->link);
		msg_free(msg);
	}
	unlock(&p->guard);
	NFOUT;
	return cont;
}

static void wait_end(struct nvmeibt_km_comm *p)
{
	char c;

	NFIN;
	if (nvmeibt_fd_set_blocking(p->spair[1], 1)) {
		read(p->spair[1], &c, 1);
	}
	NFOUT;
}

static void remove_disk_ack(
	struct nvmeibt_km_comm *p, struct nvmeib_remove_disk *disk)
{
	char buf[
		sizeof(struct srv_comm_msg) +
		sizeof(struct nvmeib_remove_disk)] = {0};
	struct srv_comm_msg *kmsg = (void *)buf;
	struct nvmeib_remove_disk *rd = (void *)kmsg->msg.data;

	NFIN;
	memset(buf, 0, sizeof(buf));
	kmsg->msg.opcode = csc_remove_disk;
	kmsg->msg.len = sizeof(kmsg->msg) + sizeof(struct nvmeib_remove_disk);
	*rd = *disk;
	send_msg_to_kernel(p, kmsg);
	NFOUT;
}

static void get_disks(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg kmsg;

	NFIN;
	memset(&kmsg, 0, sizeof(kmsg));
	kmsg.msg.opcode = csc_get_disks;
	kmsg.msg.len = sizeof(kmsg.msg);
	send_msg_to_kernel(p, &kmsg);
	NFOUT;
}

static void keep_alive(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg kmsg;

	NFIN;
	memset(&kmsg, 0, sizeof(kmsg));
	kmsg.msg.opcode = csc_keep_alive;
	kmsg.msg.len = sizeof(kmsg.msg);
	send_msg_to_kernel(p, &kmsg);
	NFOUT;
}

static void * run(void *v)
{
	struct nvmeibt_km_comm *p = v;
	fd_set read_fds;
	fd_set write_fds;
	fd_set except_fds;
	struct timeval tv;
	int max_fd;
	int cont = 1;
	int n;

	NFIN;
	get_disks(p);
	max_fd = p->spair[1];
	if (max_fd < p->nl_sock_fd)
		max_fd = p->nl_sock_fd;
	while (cont) {
		tv.tv_sec = TOMA_SILENCE_MAX_PERIOD_SECS;
		tv.tv_usec = 0;
		build_fd_sets(p, &read_fds, &write_fds, &except_fds);
		n = select(max_fd + 1, &read_fds, &write_fds, &except_fds, &tv);
		if (n > 0) {
			if (FD_ISSET(p->spair[1], &read_fds)) {
				read_spair(p);
				cont = handle_spair(p);
			}
			else if (FD_ISSET(p->nl_sock_fd, &read_fds)) {
				cont = handle_new_nl(p);
			}
			else if (FD_ISSET(p->spair[1], &except_fds)) {
				N_Ef(error_km_comm_run, "Exception on socketpair 1");
				goto error;
			}
			else if (FD_ISSET(p->nl_sock_fd, &except_fds)) {
				N_Ef(error_1_km_comm_run, "Exception on netlink socket");
				goto error;
			}
			else {
				N_Wf(warn_km_comm_run, " select triggered none of our fd");
			}
		}
		else if (n == 0) {
			/* timeout */
			N_Df(trace_km_comm_run, "Timeout: try reading spair for the chance "
			   "we missed an event");
			cont = handle_spair(p);
			if (cont) {
				XDLIST_EMPTY(&p->disks) ? get_disks(p) : keep_alive(p);
			}
		}
		else {
			N_Ef(error_2_km_comm_run, "select error - @AUTO_ERRNO");
error:
			if (release_queue(p)) {
				wait_end(p);
			}
			cont = 0;
		}
	}
	NFOUT;
	return NULL;
}

int nvmeibt_km_comm_send(
	struct nvmeibt_km_comm *p, struct km_comm_msg_hdr *hdr)
{
	struct srv_comm_msg *kmsg;
	int		msg_size;
	int 	kmsg_size;
	char c = 1;
	int rv;

	NFIN;
	if (hdr->opcode == csc_start || hdr->opcode >= csc_end) {
		N_Ef(error_km_comm_nvmeibt_km_comm_send, "Invalid kernel message opcode @OPCODE", hdr->opcode);
		rv = -1;
		goto out;
	}
	msg_size = sizeof(kmsg->msg) + hdr->len;
	kmsg_size = sizeof(*kmsg) + msg_size;
	if (!(kmsg = NNVMEIBT_BM_CALLOC(trace_km_comm_nvmeibt_km_comm_send, kmsg_size))) {
		N_Ef(error_km_comm_nvmeibt_km_comm_send_2, "Fail to allocate nvmeibt_km_comm msg");
		rv = -1;
		goto out;
	}
	kmsg->msg.opcode = hdr->opcode;
	if (hdr->opcode > csc_start) {
		kmsg->remember = true;
		kmsg->on_done = hdr->on_done;
		kmsg->ctx =  hdr->ctx;
		kmsg->msg.len = msg_size;
		kmsg->msg.id = get_guid(p);
		memcpy(kmsg->msg.data, hdr->data, hdr->len);
		N_Tf(trace_1_km_comm_nvmeibt_km_comm_send, "Processing new message - id=@ID hdr->len=@INT kmsg->msg.len=@INT", kmsg->msg.id, hdr->len, kmsg->msg.len);
	}
	lock(&p->guard);
	if (p->valid) {
		XDLIST_ADD_TAIL(p->msgs, kmsg);
		rv = write(p->spair[0], &c, 1) == 1 ? 0 : -1;
	}
	else {
		rv = -1;
	}
	unlock(&p->guard);
	goto out;

out:
	NFOUT;
	return rv;
}

int nvmeibt_km_comm_register_disk_events(struct nvmeibt_km_comm *p,
	struct nvmeib_register_change_disk *cbs)
{
	char buf[
		sizeof(struct km_comm_msg_hdr) +
		sizeof(struct nvmeib_register_change_disk)] = {0};
	struct km_comm_msg_hdr *msg = (void *)buf;
	struct nvmeib_register_change_disk *c = (void *)msg->data;
	int rv;

	NFIN;
	msg->len = sizeof(struct nvmeib_register_change_disk);
	msg->opcode = csc_register_disk_events;
	*c = *cbs;
	rv = nvmeibt_km_comm_send(p, msg);
	NFOUT;
	return rv;
}

void nvmeibt_km_comm_ack_disk_remove(struct nvmeibt_km_comm *p,
	unsigned long ack_id)
{
	struct disk_info *disk;
	bool found = false;

	NFIN;
	XDLIST_FOREACH(disk, &p->disks) {
		if (disk->ack_id == ack_id) {
			found = true;
			break;
		}
	}
	if (found) {
		remove_disk_from(p, disk);
		remove_disk_ack(p, &disk->rm_disk);
		NNVMEIBT_TOMA_FREE(trace_km_comm_nvmeibt_km_comm_ack_disk_remove, disk);
	}
	NFOUT;
}

int nvmeibt_km_comm_get_disk_info(struct nvmeibt_km_comm *p,
	const char *disk_name, struct nvmeib_disk_info *di)
{
	struct disk_info *disk;
	int rv = -1;

	NFIN;
	lock(&p->guard);
	XDLIST_FOREACH(disk, &p->disks) {
		if (!memcmp(disk->disk.disk_id, disk_name,
			sizeof(disk->disk.disk_id))) {
			if (di) {
				*di = disk->disk;
			}
			rv = 0;
			break;
		}
	}
	unlock(&p->guard);
	NFOUT;
	return rv;
}

#if defined(UK_ZERO_TEST) && UK_ZERO_TEST

struct test_per_disk {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	int test_type;
	pthread_mutex_t test_guard;
	pthread_cond_t test_wakeup;
	struct nvmeib_get_disk_names_reply *disk_rep;
	struct nvmeib_identify_disk_reply *ident_rep;
};

static void test_per_disk_init(struct test_per_disk *pd)
{
	pthread_condattr_t attr;

	NFIN;
	if (pthread_mutex_init(&pd->test_guard, NULL) != 0) {
		N_Ef(error_km_comm_test_per_disk_init, "Failed to create test guard");
		abort();
	}
	if (pthread_condattr_init(&attr) != 0) {
		N_Ef(error_1_km_comm_test_per_disk_init, "Failed to create cond var attr");
		abort();
	}
	if (pthread_cond_init(&pd->test_wakeup, &attr) != 0) {
		N_Ef(error_2_km_comm_test_per_disk_init, "Failed to create test cond var");
		abort();
	}
	NFOUT;
}

static void test_per_disk_free(struct test_per_disk *pd)
{
	if (pd->disk_rep) {
		NNVMEIBT_TOMA_FREE(trace_km_comm_test_per_disk_free, pd->disk_rep);
	}
}

#define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))

static void test_lock(pthread_mutex_t *test_guard)
{
	int rv;
	if ((rv = pthread_mutex_lock(test_guard)) != 0) {
		N_Ef(error_km_comm_test_lock, "Failed to lock test guard - @RV", rv);
		abort();
	}
}

static void test_unlock(pthread_mutex_t *test_guard)
{
	int rv;
	if ((rv = pthread_mutex_unlock(test_guard)) != 0) {
		N_Ef(error_km_comm_test_unlock, "Failed to unlock test guard - @RV", rv);
		abort();
	}
}

static void test_wait(pthread_mutex_t *test_guard, pthread_cond_t *test_wakeup)
{
	int rv;
	if ((rv = pthread_cond_wait(test_wakeup, test_guard)) != 0) {
		N_Ef(error_km_comm_test_wait, "Failed to wait for logger processor cond - @RV", rv);
		abort();
	}
}

static void test_signal(pthread_cond_t *test_wakeup)
{
	int rv;
	if ((rv = pthread_cond_signal(test_wakeup)) != 0) {
		N_Ef(error_km_comm_test_signal, "Failed to signal test cond - @RV", rv);
		abort();
	}
}

static void test_on_done_func(void *ctx, int ok,
	struct nvmeib_nl_uk_comm_rep *rep)
{
	struct test_per_disk *pd = ctx;
	struct nvmeib_get_disk_names_reply *r;
	struct nvmeib_identify_disk_reply *rr;
	int len;

	NFIN;
	test_lock(&pd->test_guard);
	if (ok) {
		if (pd->test_type == 1) {
			r = container_of(rep, struct nvmeib_get_disk_names_reply, base);
			len = sizeof(*r) + r->n_disks * sizeof(struct nvmeib_short_disk_info);
			if ((pd->disk_rep = NNVMEIBT_TOMA_CALLOC(trace_km_comm_test_on_done_func, 1, len))) {
				memcpy(pd->disk_rep, r, len);
			}
			else {
				N_Ef(error_km_comm_test_on_done_func, "Failed allocate buffer for receive message - "
					"latency_in_ns=@LATENCY_IN_NS", rep->latency_ns);
			}
		}
		else if (pd->test_type == 2) {
			N_Tf(trace_1_km_comm_test_on_done_func, "Zero was OK for disk=@STR - latency_in_ns=@LATENCY_IN_NS",
				pd->disk_id,
				rep->latency_ns);
		}
		else if (pd->test_type == 3) {
			N_Tf(trace_2_km_comm_test_on_done_func, "Contaminate was OK for disk=@STR - latency_in_ns=@LATENCY_IN_NS",
				pd->disk_id,
				rep->latency_ns);
		}
		else if (pd->test_type == 4) {
			N_Tf(trace_3_km_comm_test_on_done_func, "Test Zeor was OK for disk=@STR - latency_in_ns=@LATENCY_IN_NS",
				pd->disk_id,
				rep->latency_ns);
		}
		else if (pd->test_type == 5) {
			N_Tf(trace_4_km_comm_test_on_done_func, "Test Identify was OK for disk=@STR - latency_in_ns=@LATENCY_IN_NS",
				pd->disk_id,
				rep->latency_ns);
			rr = container_of(rep, struct nvmeib_identify_disk_reply, base);
			len = sizeof(*rr);
			if ((pd->ident_rep = NNVMEIBT_TOMA_CALLOC(trace2_km_comm_test_on_done_func, 1, len))) {
				memcpy(pd->ident_rep, rr, len);
			}
			else {
				N_Ef(error2_km_comm_test_on_done_func, "Failed allocate buffer for receive message - "
					"latency_in_ns=@LATENCY_IN_NS", rep->latency_ns);
			}
		}
		else if (pd->test_type == 6) {
			N_Tf(trace_6_km_comm_test_on_done_func, "Test Format was OK for disk=@STR - latency_in_ns=@LATENCY_IN_NS",
				pd->disk_id, rep->latency_ns);
		}
	}
	else {
		N_Ef(error_1_km_comm_test_on_done_func, "Got a failed reply from the server on disk=@STR...", pd->disk_id);
	}
	test_signal(&pd->test_wakeup);
	test_unlock(&pd->test_guard);
	NFOUT;
}

#define TEST_START_LBA (1000000UL)
#define TEST_LBA_RANGE (1024UL * 1024 * 8)

static void * run_test_zero_per_disk(void *p)
{
	struct test_per_disk *pd = p;
	char msg_buf[
		sizeof(struct km_comm_msg_hdr) +
		sizeof(
			union {
				char a[sizeof(struct nvmeib_zero_disk)];
				char b[sizeof(struct nvmeib_contaminate_disk)];
			}
		)
	] = {0};
	struct km_comm_msg_hdr *msg_hdr = (struct km_comm_msg_hdr *)msg_buf;
	struct nvmeib_zero_disk *zero_msg_data =
		(struct nvmeib_zero_disk *)msg_hdr->data;
	struct nvmeib_contaminate_disk *contaminate_msg_data =
		(struct nvmeib_contaminate_disk *)msg_hdr->data;

	NFIN;
	msg_hdr->on_done = test_on_done_func;
	msg_hdr->ctx = pd;
	/* send the zero command */
	N_Df(trace_km_comm_run_test_zero_per_disk, "Sendind first zero to disk=@STR", pd->disk_id);
	pd->test_type = 2;
	msg_hdr->opcode = csc_zero_disk;
	msg_hdr->len = sizeof(*zero_msg_data);
	nvmeibt_strlcpy(zero_msg_data->disk_id, pd->disk_id, sizeof(zero_msg_data->disk_id));
	zero_msg_data->vendor_id = 777;
	zero_msg_data->start_hw_sector = TEST_START_LBA;
	zero_msg_data->n_hw_sectors = TEST_LBA_RANGE;
	zero_msg_data->is_hw = 0;
	zero_msg_data->is_using_nvme_trim_before_zero = 1;
	zero_msg_data->is_zeroing_using_test_and_write = 0;
	test_lock(&pd->test_guard);
	if (nvmeibt_km_comm_send(nvmeibt_get_srv_comm(), msg_hdr)) {
		N_Ef(error_km_comm_run_test_zero_per_disk, "Failed to send zero_disk message to server");
		abort();
	}
	test_wait(&pd->test_guard, &pd->test_wakeup);
	test_unlock(&pd->test_guard);
	N_Df(trace_1_km_comm_run_test_zero_per_disk, "Finish first zero to disk=@STR", pd->disk_id);
//goto out;
	/* send the contaminate command */
	N_Df(trace_2_km_comm_run_test_zero_per_disk, "Sendind contaminate to disk=@STR", pd->disk_id);
	pd->test_type = 3;
	msg_hdr->opcode = csc_contaminate_disk;
	msg_hdr->len = sizeof(*contaminate_msg_data);
	contaminate_msg_data->base.vendor_id = 777;
	contaminate_msg_data->base.start_hw_sector = TEST_START_LBA;
	contaminate_msg_data->base.n_hw_sectors = TEST_LBA_RANGE;
	contaminate_msg_data->percentage_zero = 0;
	test_lock(&pd->test_guard);
	if (nvmeibt_km_comm_send(nvmeibt_get_srv_comm(), msg_hdr)) {
		N_Ef(error_1_km_comm_run_test_zero_per_disk, "Failed to send zero_disk message to server");
		abort();
	}
	test_wait(&pd->test_guard, &pd->test_wakeup);
	test_unlock(&pd->test_guard);
	N_Df(trace_3_km_comm_run_test_zero_per_disk, "Finish contaminate to disk=@STR", pd->disk_id);
	/* send the zero command */
	N_Df(trace_4_km_comm_run_test_zero_per_disk, "Sendind second zero to disk=@STR", pd->disk_id);
	pd->test_type = 2;
	msg_hdr->opcode = csc_zero_disk;
	msg_hdr->len = sizeof(*zero_msg_data);
	zero_msg_data->vendor_id = 777;
	zero_msg_data->start_hw_sector = TEST_START_LBA;
	zero_msg_data->n_hw_sectors = TEST_LBA_RANGE;
	zero_msg_data->is_hw = 0;
	zero_msg_data->is_using_nvme_trim_before_zero = 1;
	zero_msg_data->is_zeroing_using_test_and_write = 0;
	test_lock(&pd->test_guard);
	if (nvmeibt_km_comm_send(nvmeibt_get_srv_comm(), msg_hdr)) {
		N_Ef(error_2_km_comm_run_test_zero_per_disk, "Failed to send zero_disk message to server");
		abort();
	}
	test_wait(&pd->test_guard, &pd->test_wakeup);
	test_unlock(&pd->test_guard);
	N_Df(trace_5_km_comm_run_test_zero_per_disk, "Finish second zero to disk=@STR", pd->disk_id);
	/* send the test_zero command */
	N_Df(trace_6_km_comm_run_test_zero_per_disk, "Sendind test zero to disk=@STR", pd->disk_id);
	pd->test_type = 4;
	msg_hdr->opcode = csc_test_zero_disk;
	msg_hdr->len = sizeof(*zero_msg_data);
	zero_msg_data->vendor_id = 777;
	zero_msg_data->start_hw_sector = TEST_START_LBA;
	zero_msg_data->n_hw_sectors = TEST_LBA_RANGE;
	zero_msg_data->is_hw = 0;
	zero_msg_data->is_using_nvme_trim_before_zero = 1;
	zero_msg_data->is_zeroing_using_test_and_write = 0;
	test_lock(&pd->test_guard);
	if (nvmeibt_km_comm_send(nvmeibt_get_srv_comm(), msg_hdr)) {
		N_Ef(error_3_km_comm_run_test_zero_per_disk, "Failed to send zero_disk message to server");
		abort();
	}
	test_wait(&pd->test_guard, &pd->test_wakeup);
	test_unlock(&pd->test_guard);
	N_Df(trace_7_km_comm_run_test_zero_per_disk, "Finish test zero to disk=@STR", pd->disk_id);

//out:
	test_per_disk_free(pd);
	NNVMEIBT_TOMA_FREE(trace_8_km_comm_run_test_zero_per_disk, pd);
	NFOUT;
	return NULL;
}

static void * run_test_format_per_disk(void *p)
{
	struct test_per_disk *pd = p;
	struct nvme_id_ns *ns;
	int nlbaf;
	unsigned metadata_size;
	unsigned block_size;
	struct nvme_lbaf format;
	int inline_md;
	int i;
	char msg_buf[
		sizeof(struct km_comm_msg_hdr) +
		sizeof(
			union {
				char a[sizeof(struct nvmeib_identify_disk)];
				char b[sizeof(struct nvmeib_format_disk)];
			}
		)
	] = {0};
	struct km_comm_msg_hdr *msg_hdr = (struct km_comm_msg_hdr *)msg_buf;
	struct nvmeib_identify_disk *identify_data =
		(struct nvmeib_identify_disk *)msg_hdr->data;
	struct nvmeib_format_disk *format_data =
		(struct nvmeib_format_disk *)msg_hdr->data;

	NFIN;
	msg_hdr->on_done = test_on_done_func;
	msg_hdr->ctx = pd;
	/* send the identify command */
	N_Df(trace_km_comm_run_test_format_per_disk1, "Sendind identify to disk=@STR", pd->disk_id);
	pd->test_type = 5;
	msg_hdr->opcode = csc_identify_disk;
	msg_hdr->len = sizeof(*identify_data);
	nvmeibt_strlcpy(identify_data->disk_id, pd->disk_id, sizeof(identify_data->disk_id));
	identify_data->data = NNVMEIBT_TOMA_CALLOC(run_test_format_per_disk_1, 1, 4096);
	identify_data->pid = getpid();
	if (!identify_data->data) {
		N_Ef(run_test_format_per_disk_2, "Failed to allocate buffer for disk identify object");
		abort();
	}
	identify_data->data_len = 4096;
	test_lock(&pd->test_guard);
	if (nvmeibt_km_comm_send(nvmeibt_get_srv_comm(), msg_hdr)) {
		N_Ef(error_km_comm_run_test_per_disk, "Failed to send identify message to server");
		abort();
	}
	test_wait(&pd->test_guard, &pd->test_wakeup);
	test_unlock(&pd->test_guard);
	N_Df(trace2_km_comm_run_test_format_per_disk, "Finish identify disk=@STR", pd->disk_id);
	if (pd->ident_rep) {
		N_Tf(run_test_format_per_disk_2,
			 "Identify for disk=@STR returned data with length @INT - latency_ns=@LLD",
			pd->ident_rep->disk_id,
			pd->ident_rep->data_len,
			pd->ident_rep->base.latency_ns);
		if (pd->ident_rep->data_len) {
			ns = identify_data->data;
			nlbaf = ns->nlbaf;
			N_Tf(run_test_format_per_disk_3,
				 "Identify for disk=@STR: supports @INT formats",
				pd->ident_rep->disk_id, nlbaf);
			format = ns->lbaf[ns->flbas & 0xf];
			block_size = 1 << format.ds;
			metadata_size = le16toh(format.ms);
			inline_md = (ns->flbas & 0x10) ? 1 : 0;
			if (metadata_size) {
				N_Tf(run_test_format_per_disk_4,
					 "Current format for disk=@STR: "
					 "block_size=@INT md_size@INT inline_md=@INT",
					pd->ident_rep->disk_id,
					block_size, metadata_size, inline_md);
			}
			else {
				N_Tf(run_test_format_per_disk_5,
					 "Current format for disk=@STR: "
					 "block_size=@INT no MD",
					pd->ident_rep->disk_id, block_size);
			}
			for (i = 0; i < nlbaf; ++i) {
				metadata_size = ns->lbaf[i].ms;	// note: little-endian
				block_size = 1 << ns->lbaf[i].ds;
				N_Tf(run_test_format_per_disk_6,
					 "Format @INT for disk=@STR: "
					 "block_size=@INT md_size@INT",
					i, pd->ident_rep->disk_id, block_size, metadata_size);
			}
		}

		NNVMEIBT_TOMA_FREE(trace3_km_comm_run_test_format_per_disk, pd->ident_rep);
		pd->ident_rep = NULL;
	}
	/* send the format command */
	N_Df(trace_km_comm_run_test_format_per_disk2, "Sendind format to disk=@STR", pd->disk_id);
	pd->test_type = 6;
	msg_hdr->opcode = csc_format_disk;
	msg_hdr->len = sizeof(*format_data);
	nvmeibt_strlcpy(identify_data->disk_id, pd->disk_id, sizeof(identify_data->disk_id));
	format_data->vendor_id = 0;
	// For TOSHIBA it means 4k+8 inline
	format_data->format_id.id = 4;
	format_data->format_id.is_inline = 1;
	format_data->format_id.is_by_ns = 1;
	test_lock(&pd->test_guard);
	if (nvmeibt_km_comm_send(nvmeibt_get_srv_comm(), msg_hdr)) {
		N_Ef(error_km_comm_run_test_per_disk2, "Failed to send format message to server");
		abort();
	}
	test_wait(&pd->test_guard, &pd->test_wakeup);
	test_unlock(&pd->test_guard);

//goto out;

//out:
	test_per_disk_free(pd);
	NNVMEIBT_TOMA_FREE(trace_8_km_comm_run_test_format_per_disk, pd);
	NFOUT;
	return NULL;
}

#define TEST_ZERO 1
#define TEST_FORMAT 2

static void create_test_per_disk(char *disk_id, int test_type)
{
	struct test_per_disk *pd;
	pthread_t tid;
	pthread_attr_t attr;

	NFIN;
	if (!(pd = NNVMEIBT_TOMA_CALLOC(create_test_per_disk_1, 1, sizeof(*pd)))) {
		N_Ef(create_test_per_disk_2, "Failed to allocate test_per_disk object");
		abort();
	}
	nvmeibt_strlcpy(pd->disk_id, disk_id, sizeof(pd->disk_id));
	test_per_disk_init(pd);
	if (pthread_attr_init(&attr)) {
		N_Ef(xx_10, "Failed to create run_test 1 thread @AUTO_ERRNO");
		abort();
	}
	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED)) {
		N_Ef(xx_11, "Failed to create run_test 2 thread @AUTO_ERRNO");
		abort();
	}
	if (test_type == TEST_ZERO) {
		if (pthread_create(&tid, &attr, run_test_zero_per_disk, pd)) {
			N_Ef(xx_1, "Failed to create run_test_zero_per_disk thread @AUTO_ERRNO");
			abort();
		}
		pthread_setname_np(tid, "km_comm_run_test_zero_per_disk");
	}
	else if (test_type == TEST_FORMAT) {
		if (pthread_create(&tid, &attr, run_test_format_per_disk, pd)) {
			N_Ef(xx_2, "Failed to create run_test_format_per_disk thread @AUTO_ERRNO");
			abort();
		}
		pthread_setname_np(tid, "km_comm_run_test_format_per_disk");
	}
	NFOUT;
}

static void * run_test_srv_comm(void *UNUSED(arg))
{
	char msg_buf[
		sizeof(struct km_comm_msg_hdr) +
		sizeof(union {
			char a[sizeof(struct nvmeib_zero_disk)];
			char b[sizeof(struct nvmeib_contaminate_disk)];})
		] = {0};
	struct km_comm_msg_hdr *msg_hdr =
		(struct km_comm_msg_hdr *)msg_buf;
	struct nvmeib_short_disk_info *di;
	int i;
	int cont = 1;
	int n_tries = 20;
	struct test_per_disk disks;

	NFIN;
	memset(&disks, 0, sizeof(disks));
	test_per_disk_init(&disks);
	msg_hdr->on_done = test_on_done_func;
	msg_hdr->ctx = &disks;
	disks.test_type = 1;
	while (cont && n_tries--) {
		/* read the disks from the server */
		msg_hdr->len = sizeof(*msg_hdr);
		msg_hdr->opcode = csc_get_disk_names;
		test_lock(&disks.test_guard);
		if (nvmeibt_km_comm_send(nvmeibt_get_srv_comm(), msg_hdr)) {
			N_Ef(error_km_comm_run_test_srv_comm, "Failed to send get_disk message to server");
			abort();
		}
		test_wait(&disks.test_guard, &disks.test_wakeup);
		test_unlock(&disks.test_guard);
		if (disks.disk_rep) {
			N_Tf(run_test_srv_comm_1,
				 "Received @INT disks from the server - latency_ns=@LLD",
				disks.disk_rep->n_disks, disks.disk_rep->base.latency_ns);
			di = disks.disk_rep->info;
			for (i = 0; i < disks.disk_rep->n_disks; ++i) {
				N_Tf(run_test_srv_comm_2, "disk[@INT]=@STR",
					i, di->disk_id);
				++di;
			}
			if (disks.disk_rep->n_disks) {
				cont = 0;
			}
			else {
				sleep(10);
				NNVMEIBT_TOMA_FREE(trace_km_comm_run_test_srv_comm, disks.disk_rep);
				disks.disk_rep = NULL;
			}
		}
	}
	if (cont) {
		N_Ef(run_test_srv_comm_3, "Did not find server disks - cannot conntinue with testing...");
		goto out;
	}
	di = disks.disk_rep->info;
	for (i = 0; i < disks.disk_rep->n_disks; ++i, ++di) {
#if 0
		if (strncmp("CJH0010020CF.1", *disk_id, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
			continue;
		}
#endif
#if 1
		if (strncmp("19J0A00ETVXE.1", di->disk_id, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
			continue;
		}
#endif
		N_Tf(run_test_srv_comm_12, "disk=@STR is a TOSHIBA one", di->disk_id);
		create_test_per_disk(di->disk_id, TEST_FORMAT);
	}

out:
	test_per_disk_free(&disks);
	NFOUT;
	return NULL;
}

void nvmeibt_km_comm_test(void)
{
	pthread_t tid;
	pthread_attr_t attr;

	if (pthread_attr_init(&attr)) {
		N_Ef(xx_3, "pthread_attr_init failed @AUTO_ERRNO");
		abort();
	}
	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED)) {
		N_Ef(xx_4, "pthread_attr_setdetachstate failed @AUTO_ERRNO");
		abort();
	}
	if (pthread_create(&tid, &attr, run_test_srv_comm, NULL)) {
		N_Ef(xx_5, "pthread_attr_init failed @AUTO_ERRNO");
		abort();
	}
	pthread_setname_np(tid.thread, "km_comm_run_test_srv_comm");
}
#endif
