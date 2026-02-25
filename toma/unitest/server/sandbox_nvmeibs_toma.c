#include "nvmeibt_debug.h"				// Binary traces
#include "sandbox_nvmeibs_toma.h"
#include "../sandbox_nvme.h"

static struct nvmeibs_simulator *g_srvr_simu = NULL;

static void TSB_server_toma_status_req_simu_init(struct TSB_server_toma_status_req_simu *me) {
	me->max_reply_length_bytes = 64;				// Ask to fill at most 64[b] of reply, currently not verifying the reply itself
	memset(me->msg_q, 0, sizeof(me->msg_q));
	me->msg_q[3] = NVMEIBS_TOMA_TRIGGER_JGC;		// Todo: Unitest environment should instruct this simulator to send specific messages
	me->msg_q[5] = NVMEIBS_TOMA_WRITE_STATUS_REQ;
	me->msg_q[7] = NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE;
}

static bool server_simu_has_next_msg_for_toma(void) {
	struct TSB_server_toma_status_req_simu *me = &g_srvr_simu->s_req_simu;
	me->n_srvr_msg_idx++;
	return (me->n_srvr_msg_idx < 16) && (me->msg_q[me->n_srvr_msg_idx] != 0);
}

static void TSB_server_toma_status_req_simu_destroy(struct TSB_server_toma_status_req_simu *me, bool do_veridy_used) {
	BUG_ON(me->expecting_reply_cookie);				// Did not get a reply from Toma
	if (do_veridy_used) {
		BUG_ON(me->n_toma_replies_received <= 0);	// Coverage tests did not receive any reply from Toma
	}
}

static ssize_t server_simu_get_next_msg_for_toma(int fd, void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_toma_status_req_simu *me = &g_srvr_simu->s_req_simu;
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
		pl->type = NVMEIBS_TOMA_STATUS_RAFT;	// NVMEIBS_TOMA_STATUS_ALL_JSON
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
	struct TSB_server_toma_status_req_simu *me = &g_srvr_simu->s_req_simu;
	const struct nvmeibs_toma_server_proc_buf *m = buf;
	const enum nvmeibs_toma_server_msg_type type = m->type;
	BUG_ON((fd < 2) || (n != sizeof(*m)) || (offset != 0) || !buf);
	switch (type) {
		case NVMEIBS_TOMA_LOGIN:  N_SANDBOX(__AUTOID__, "SRVR_SIMU->Got: Toma_Hello @ZU[b] via_netlink=@BOOL_YN", n, !!flags); break;
		case NVMEIBS_TOMA_LOGOUT: N_SANDBOX(__AUTOID__, "SRVR_SIMU->Got: TomaByeBye @ZU[b] via_netlink=@BOOL_YN", n, !!flags); break;
		case NVMEIBS_TOMA_WRITE_STATUS_RESP: {
			const struct nvmeibs_msg_t2s_toma_status_resp *pl = &m->status_resp_msg;
			me->n_toma_replies_received++;
			N_SANDBOX(__AUTOID__, "SRVR_SIMU->Got: TomaStatusRep @ZU[b], cnt=@INT", n, me->n_toma_replies_received);
			BUG_ON(me->expecting_reply_cookie <= 0);				// Reply comes without server expecting it
			BUG_ON(pl->handle != 0 - me->expecting_reply_cookie);
			BUG_ON(pl->handle_req != me->expecting_reply_cookie);
			BUG_ON(pl->length >= (size_t)me->max_reply_length_bytes);		// Cannot reply more than permitted buf size including '\0'
			if ((pl->length + 1) == (size_t)me->max_reply_length_bytes)
				BUG_ON(!pl->is_overflow);							// Toma status reply was truncated. Verify that
			me->expecting_reply_cookie = false;
			break;
		}
		case NVMEIBS_TOMA_JOURNAL_INFO: {
			const struct nvmeibs_msg_t2s_journal *pl = &m->journal_msg;
			N_SANDBOX(__AUTOID__, "SRVR_SIMU->Got: JournalInfo @ZU[b] disk=@STR lba=@ZU len=@ZU serjio{lba=@ZU, len=@ZU}",
				n, pl->disk_id, pl->lba, pl->length, pl->serjio_db_lba, pl->serjio_db_length);
			break;
		}
		default: BUG_ON(true);		// Not supported yet
	}
	errno = 0;						// Failure not supported yet
	return n;
}

static ssize_t _srvr_simu_nvmeibs_toma_client_proc_recv(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct TSB_server_toma_status_req_simu *me = &g_srvr_simu->s_req_simu;
	const struct nvmeibs_toma_client_proc_buf *m = buf;
	const u32 cid = (m->handle >> 32);		// Todo: Find client in hash
	BUG_ON((fd < 2) || (n != sizeof(*m)) || (offset != 0) || !buf);
	N_SANDBOX(__AUTOID__, "SRVR_SIMU->Got: 2_reg_clnt @ZU[b] via_netlink=@INT", n, flags);
	me->n_msgs_to_registrants++;
	if (!m->handle) { errno = ENXIO;	return -1; }
	if (!cid)		{ errno = EINVAL;	return -1; }
	if (0)			{ errno = ENXIO;	return -1; }	// Send fail to client
	return n;
}

/********************************* /dev/ utils *******************************/
static int __get_smart_seq_from_device_name(const char *device_name) {
	int nvme_num;
	if (!device_name || (strncmp(device_name, "nvme", 4) != 0) || !isdigit(device_name[4]))
		return -1;			// For stock drivers like "nvme0n1", returns -1 (no smart file).
	nvme_num = atoi(&device_name[4]);
	return (nvme_num < 1000) ? -1 : (nvme_num - 1000);		// For NVMesh: "nvme1001n1", seq = 1001 - 1000 = 1.
}

static const struct sandbox_nvme_device *__find_nvmesh_device_by_seq(int seq) {
	const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_arr();
	const struct sandbox_nvme_device *end = &dev[sandbox_nvme_get_device_count()];
	for (; dev < end; dev++) {
		if ((!dev->stock_disk) && (seq == __get_smart_seq_from_device_name(dev->device_name)))
			return dev;
	}
	BUG_ON(true); return NULL;
}

static void __fill_disk_info(struct nvmeib_disk_info *d, const struct sandbox_nvme_device *dev, bool is_add) {						// Fill disk info from sandbox device, using its current LBA format
	const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(dev->current_format_idx);
	d->n_hw_blocks = d->n_blocks = (is_add ? dev->size_in_blocks : 0);		// Toma uses n_blocks to distinguish between disk add/remove events
	d->vendor_id = dev->vendor_id;
	d->block_size = (1 << lbaf->block_size_exp);
	d->max_request_size = 32;
	d->max_n_hw_sectors = 32;
	d->seq = __get_smart_seq_from_device_name(dev->device_name);
	d->nsid = 1;
	d->metadata = lbaf->metadata_size;
	snprintf(d->disk_id, sizeof(d->disk_id), "%s.%d", dev->serial_number, d->nsid);
	snprintf(d->dev_name, sizeof(d->dev_name), "%s", dev->device_path);
	snprintf(d->model_str, sizeof(d->model_str), "%s", dev->model_number);
	snprintf(d->native_serial_str, sizeof(d->native_serial_str), "%s", dev->serial_number);
	snprintf(d->status, sizeof(d->status), "Ok");
}

static int format_smart_content(char *buf, size_t buf_size, const struct sandbox_nvme_device *dev) {	//	/proc/nvmeibs/smartX
	const int seq = __get_smart_seq_from_device_name(dev->device_name);
	int n, namespace = 1;
	BUG_ON(!buf || !dev || (seq < 0)); // should only be called for NVMesh disks -> -1 means stock disk
	n = snprintf(buf, buf_size,
		"Pci Address=0000:%02x:00.0\n"
		"Serial Number=%s\n"
		"Vendor=0x%04x\n"
		"Model=%s\n"
		"Submission Queues=128\nCompletion Queues=128\nMSIX Interrupts=129\nNum admin cmds=323\nNamespace Id=%d\nNuma Node=1\n",
		seq, dev->serial_number, dev->vendor_id, dev->model_number, namespace);
	BUG_ON((n < 0) || ((size_t)n >= buf_size));
	N_Tf(fsc0012, "read[@STR].serial=@STR.@INT", dev->device_name, dev->serial_number, namespace /*, buf*/);
	return (int)n;
}

static int format_disks_csv(char *buf, int buf_size) {
	const struct sandbox_nvme_device *d = sandbox_nvme_get_device_arr();
	const struct sandbox_nvme_device *end = &d[sandbox_nvme_get_device_count()];
	int rv = 0;
	BUG_ON((buf == NULL)||(buf_size == 0));
	#define BUF_ADD(...) rv += (int)scnprintf(&buf[rv], buf_size - rv, __VA_ARGS__)
	BUF_ADD("%s\n", NVMEIBS_DISKS_CSV_HEADER);		/* Write header */
	for (; d < end; d++) {
		if (!d->stock_disk) {
			const int disk_seq = __get_smart_seq_from_device_name(d->device_name);
			const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(d->current_format_idx);
			BUG_ON(disk_seq < 0);
			BUF_ADD("%s.1,%u,%u,%u,32,%d,1,/dev/%s,%u,Ok,%d,%s,%s\n",
				d->serial_number, (unsigned)d->size_in_blocks, (unsigned)d->size_in_blocks, (1u << lbaf->block_size_exp),
				disk_seq, d->device_name, lbaf->metadata_size, d->vendor_id, d->model_number, d->serial_number);
		}
	}
	BUG_ON(rv >= buf_size);		// buffer overflow/truncation or no room for trailing '\0'
	N_Tf(fdc0001, "formatted disks CSV: @STR", buf);
	return rv;
}

/********************************* Netlink mock *******************************/
static bool TSB_netlink_queue_has_something(void) {		// Netlink mock queue helpers (thread-safe)
	struct TSB_netlink_mock *nl = g_srvr_simu->nl;
	bool rv;
	pthread_mutex_lock(&nl->mutex);
	rv = (nl->queue_count != 0);
	pthread_mutex_unlock(&nl->mutex);
	return rv;
}

static ssize_t TSB_netlink_queue_dequeue(void *buf, size_t buf_size) {
	struct TSB_netlink_mock *nl = g_srvr_simu->nl;
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

static void TSB_netlink_queue_enqueue(const void *data, size_t len) {
	struct TSB_netlink_mock *nl = g_srvr_simu->nl;
	pthread_mutex_lock(&nl->mutex);
	BUG_ON(!(nl->queue_count < TSB_NL_QUEUE_SIZE && len <= TSB_NL_MSG_SIZE));
	memcpy(nl->queue[nl->queue_tail].data, data, len);
	nl->queue[nl->queue_tail].len = len;
	nl->queue_tail = (nl->queue_tail + 1) % TSB_NL_QUEUE_SIZE;
	nl->queue_count++;
	pthread_mutex_unlock(&nl->mutex);
}

static void reply_usermode_payload(struct nvmeib_nl_uk_comm_msg *omsg, const struct nvmeib_nl_uk_comm_msg *imsg) {		// See real server function implementation
	struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep*)omsg->data;
	rep->opcode = omsg->opcode = imsg->opcode;
	omsg->id = imsg->id;
	omsg->caller_type = imsg->caller_type;
	rep->latency_ns = 576;
}

static void TSB_netlink_send_disk_response(const struct sandbox_nvme_device *dev, const struct nvmeib_nl_uk_comm_msg *req_msg) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_disk_info_reply *rep = (struct nvmeib_disk_info_reply *)msg->data;
	reply_usermode_payload(msg, req_msg);
	msg->len = sizeof(*msg) + sizeof(*rep);
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	rep->base.error = csce_ok;
	rep->selector = nvmeib_disk_info_reply_dinfo;
	__fill_disk_info(&rep->dinfo.disk, dev, true);
	rep->dinfo.serjio_status = 0;  // nvmeibs_serjio_status_ok
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

static void TSB_netlink_handle_io_to_disk(const struct nvmeib_nl_uk_comm_msg *req_msg) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_io_to_disk_reply *rep = (struct nvmeib_io_to_disk_reply *)msg->data;
	const struct nvmeib_io_to_disk *io_req = (const struct nvmeib_io_to_disk *)req_msg->data;
	const int io_result = sandbox_nvme_io_to_disk(io_req->disk_id, io_req->start_sector, io_req->data_len, io_req->data, io_req->is_read);

	reply_usermode_payload(msg, req_msg);
	msg->len = sizeof(*msg) + sizeof(*rep);
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	rep->n_data_io = io_req->data_len;
	rep->n_md_io = 0;
	rep->base.error = (io_result < 0) ? csce_io_failed : csce_ok;
	nvmeib_strlcpy(rep->disk_id, io_req->disk_id, sizeof(rep->disk_id));
	rep->vendor_id = io_req->vendor_id;
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

static void TSB_netlink_handle_zero_disk(const struct nvmeib_nl_uk_comm_msg *req_msg) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_zero_disk_reply *rep = (struct nvmeib_zero_disk_reply *)msg->data;
	const struct nvmeib_zero_disk *zreq = (const struct nvmeib_zero_disk *)req_msg->data;
	reply_usermode_payload(msg, req_msg);
	msg->len = sizeof(*msg) + sizeof(*rep);
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	rep->base.error = (0 == sandbox_nvme_zero_disk_area(zreq->disk_id, zreq->start_hw_sector, zreq->n_hw_sectors)) ? csce_ok : csce_bad_zero_params;
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

// Send an unsolicited disk change event to Toma (simulates kernel's disk_freeze/unfreeze behavior), is_add: add-event/remove-event
static void TSB_netlink_send_disk_change_event(const struct sandbox_nvme_device *dev, bool is_add) {
	static unsigned long unsolicited_msg_id = 0x80000000;
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_disk_info_reply *rep = (struct nvmeib_disk_info_reply *)msg->data;
	msg->len = sizeof(*msg) + sizeof(*rep);
	rep->base.opcode = msg->opcode = csc_get_disks;		// 3 lines below do the same as reply_usermode_payload()
	msg->id = unsolicited_msg_id++;
	msg->caller_type = TOMA_CALLER;
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	rep->base.error = csce_ok;
	rep->selector = nvmeib_disk_info_reply_dinfo;
	__fill_disk_info(&rep->dinfo.disk, dev, is_add);
	rep->dinfo.serjio_status = 0;
	N_Tf(nl_disk_event, "Queuing disk @STR event for disk_id=@STR", is_add ? "ADD" : "REMOVE", rep->dinfo.disk.disk_id);
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

static void TSB_netlink_handle_format_disk(const struct nvmeib_nl_uk_comm_msg *req_msg, const struct nvmeib_format_disk *fmt_disk) {
	char reply_buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *reply_nlhdr = (struct nlmsghdr *)reply_buf;
	struct nvmeib_nl_uk_comm_msg *reply_msg = NLMSG_DATA(reply_nlhdr);
	struct nvmeib_format_disk_reply *rep = (struct nvmeib_format_disk_reply *)reply_msg->data;
	const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id(fmt_disk->disk_id);
	const enum SANDBOX_NVME_FMT_e fmt_idx = fmt_disk->format_id.id;

	reply_usermode_payload(reply_msg, req_msg);
	reply_msg->len = sizeof(*reply_msg) + sizeof(*rep);
	reply_nlhdr->nlmsg_len = NLMSG_SPACE(reply_msg->len);
	BUG_ON(fmt_disk->format_id.is_inline);					// Reject inline metadata - NVMesh only supports separate metadata

	TSB_netlink_send_disk_change_event(dev, false);  // 1. Send REMOVE event BEFORE format (simulates disk_freeze). is_add=false -> n_blocks=0
	// 2. Perform the format operation (zeros disk). Note: The NVMESH_FORMATTED_DISK header is written by production code (format_disk_wrapper) after receiving the format reply.
	if (sandbox_nvme_format_disk(fmt_disk->disk_id, fmt_idx) != 0) {
		N_Ef(fmt_failed, "format_disk: format failed disk_id=@STR fmt_idx=@INT", fmt_disk->disk_id, fmt_idx);
		rep->base.error = csce_failed;
	} else {
		rep->base.error = csce_ok;
		// Fill in the new format info
		snprintf(rep->info.new_dev_file_name, sizeof(rep->info.new_dev_file_name), "%s", dev->device_path);
		rep->info.new_n_pblk = dev->size_in_blocks;
		rep->info.new_seq = __get_smart_seq_from_device_name(dev->device_name);
		g_srvr_simu->pending_disk_add = dev;	// Schedule ADD event to be sent later. This gives Toma's work queue time to process the REMOVE event before receiving the ADD event, matching production behavior where the NVMe format operation takes time between disk_freeze and disk_unfreeze.
		N_Tf(nl_pend_add, "Scheduled pending disk ADD event for serial=@STR", dev->serial_number);
	}

	// 3. Send format reply
	rep->base.latency_ns = 1000;  // Simulate some format latency
	TSB_netlink_queue_enqueue(reply_buf, reply_nlhdr->nlmsg_len);
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
		const struct sandbox_nvme_device *dev = __find_nvmesh_device_by_seq(req->opt_arg);
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

// Netlink send callback: Toma sends a request, we parse it and queue responses
static ssize_t _netlink_recv_msg_from_toma(int fd, const void *buf, size_t n, off_t offset, int flags) {
	struct nvmeibs_simulator *s = g_srvr_simu;
	struct TSB_netlink_mock *nl = s->nl;
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
	const struct nvmeib_nl_uk_comm_msg *req_msg = NLMSG_DATA(nlh);
	int n_payload_bytes_remainig = (int)n - ((const char*)req_msg->data - (const char*)buf);

	(void)fd; (void)offset; (void)flags;
	BUG_ON((n < (sizeof(struct nlmsghdr) + sizeof(*req_msg))) || (n != nlh->nlmsg_len) || (nlh->nlmsg_type != NVMESH_NL_MSG_TYPE) || (req_msg->caller_type != TOMA_CALLER));
	nl->n_recv_msgs++;
	N_Tf(nl_send, "msg[@INT].id=@ID (@STR), total_n_msgs=@INT", req_msg->opcode, req_msg->id, uk_comm_opcode_str(req_msg->opcode), nl->n_recv_msgs);
	if (req_msg->opcode == csc_get_disks) {
		const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_arr();
		const struct sandbox_nvme_device *end = &dev[sandbox_nvme_get_device_count()];
		for (; dev < end; dev++) {	// Queue disk info for each mock NVMesh disk
			if (!dev->stock_disk)
				TSB_netlink_send_disk_response(dev, req_msg);
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
		const struct nvmeibs_toma_server_proc_buf *m = (typeof(m))req_msg->data;
		const ssize_t exec_rv = s->com_toma2srvr_o.send(0xDEAD /*s->o.sock->fd*/, m, req_msg->len - (int)sizeof(*req_msg), 0, 'N');
		TSB_netlink_reply_to_blocked_toma(req_msg, (int)exec_rv);
		if (exec_rv > 0)
			n_payload_bytes_remainig -= (int)exec_rv;		// Mark Consumed bytes
	} else if (req_msg->opcode == csc_t2s_blocking_msg_to_io_clients) {
		const struct nvmeibs_toma_client_proc_buf *m = (typeof(m))req_msg->data;
		const ssize_t exec_rv =s->com_toma2clnt_o.send(0xDEAD /*s->o.sock->fd*/, m, req_msg->len - (int)sizeof(*req_msg), 0, 'N');
		TSB_netlink_reply_to_blocked_toma(req_msg, (int)exec_rv);
		if (exec_rv > 0)
			n_payload_bytes_remainig -= (int)exec_rv;		// Mark Consumed bytes
	} else if (req_msg->opcode == csc_t2s_blocking_msg_req_info) {
		const union nvmeib_nl_msg_to_srvr_payload *pay = (const union nvmeib_nl_msg_to_srvr_payload *)req_msg->data;
		n_payload_bytes_remainig -= sizeof(pay->req_info);
		TSB_netlink_handle_req_info(req_msg);
	} else if (req_msg->opcode == csc_remove_disk_ack) {
		// Toma acknowledges disk removal - no response needed
		const union nvmeib_nl_msg_to_srvr_payload *pay = (const union nvmeib_nl_msg_to_srvr_payload *)req_msg->data;
		n_payload_bytes_remainig -= sizeof(pay->rmv_disk_ack);
		N_Tf(nl_rm_ack, "Netlink remove_disk_ack received for disk_id=@STR", pay->rmv_disk_ack.disk_id);
	} else {
		BUG_ON(true);		// Not implemented yet in sandbox
	}
	BUG_ON(n_payload_bytes_remainig != 0);			// Unconsumed bytes, may be treated as next message
	return (ssize_t)n;
}

static ssize_t _netlink_reply_to_toma(int fd, void *buf, size_t n, off_t offset, int flags) {
	const ssize_t len = TSB_netlink_queue_dequeue(buf, n);
	(void)fd; (void)offset; (void)flags;
	BUG_ON(n <= (size_t)len);
	return len;
}

/********************************* API *******************************/
void nvmeibs_simu_do_periodic(void) {
	const struct sandbox_nvme_device *dev = g_srvr_simu->pending_disk_add;
	if (dev) {	// Process any pending disk ADD event that was deferred from a format operation. Ensure the REMOVE event has been processed before the ADD event is sent.
		N_Tf(nl_pend_send, "Sending deferred disk ADD event for serial=@STR", dev->serial_number);
		TSB_netlink_send_disk_change_event(dev, true);  // is_add=true -> n_blocks > 0
		g_srvr_simu->pending_disk_add = NULL;
	}
}

void nvmeibs_simu_send_extended_msg(const char *something) {
	char buf[TSB_NL_MSG_SIZE] = {0};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nvmeib_nl_uk_comm_msg *msg = NLMSG_DATA(nlh);
	struct nvmeib_push_extended_msg *rep = (struct nvmeib_push_extended_msg *)msg->data;
	msg->opcode = rep->base.opcode = csc_msg_to_process;
	rep->n_bytes_len = sprintf(&rep->content[0], "%s", something);
	msg->len = sizeof(*msg) + sizeof(*rep) + rep->n_bytes_len;
	nlh->nlmsg_len = NLMSG_SPACE(msg->len);
	TSB_netlink_queue_enqueue(buf, nlh->nlmsg_len);
}

struct nvmeibs_simulator *nvmeibs_simu_init(struct TSB_netlink_mock *nl) {
	struct nvmeibs_simulator *s = g_srvr_simu = (typeof(s))calloc(1, sizeof(*s));
	struct TSB_fd_otherside *o = &s->com_toma2srvr_o;
	o->send = _srvr_simu_nvmeibs_toma_server_proc_recv;
	o->recv = fd_otherside_write_only_illegal_recv;				// Via this fd, Toma only sends to to server. Server does not send anything to toma
	o = &s->com_srvr2toma_o;
	o->recv = server_simu_get_next_msg_for_toma;
	o->send = fd_otherside_read_only_illegal_send;				// Via this fd server sends msgs to Tom, Toma never replies back
	o->has_data = server_simu_has_next_msg_for_toma;
	o = &s->com_toma2clnt_o;
	o->send = _srvr_simu_nvmeibs_toma_client_proc_recv;
	o->recv = fd_otherside_write_only_illegal_recv;

	s->nl = nl;
	o = &nl->o;
	o->send = _netlink_recv_msg_from_toma;
	o->recv = _netlink_reply_to_toma;
	o->has_data = TSB_netlink_queue_has_something;
	pthread_mutex_init(&nl->mutex, NULL);
	TSB_server_toma_status_req_simu_init(&s->s_req_simu);
	sandbox_nvme_init();
	return s;
}

void nvmeibs_simu_destroy(struct nvmeibs_simulator *s, bool do_verify_used) {
	BUG_ON(s != g_srvr_simu);
	TSB_server_toma_status_req_simu_destroy(&s->s_req_simu, do_verify_used);
	pthread_mutex_destroy(&s->nl->mutex);
	BUG_ON(do_verify_used && (s->nl->n_recv_msgs <= 0));	// Only check for replies if we sent messages (standalone utilities like gpt_util don't communicate with TOMA)
	free(s);
	g_srvr_simu = NULL;
}
