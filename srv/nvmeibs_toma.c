/* Toma server side interface - extension of nvmeibs client object */
#define S_TOMA_C

#include <kr_incs.h>
#include <linux/gfp.h>
#include <linux/uuid.h>

#include "nvmeibs_toma.h"
#include "nvmeibs_defs.h"
#include "nvmeib_utils.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_ib_port.h"
#include "nvmeib_public_mmap.h"
#include "../toma/clnt/nvmeibt_client_protocol.h"
#include "nvmeibs_um_comm.h"
#include "nvmeibs_trace.h"
#include "vex/nvmeibs_vex.h"
#include "nvmeibs_async_cookies.h"
#include "../core_unitest/corecomm_injections.h"
#include "nvmeibs_memmgr_metrics.h"
#include "nvmeib_public_mmap.h"

#define __NFIN	NFINS(cl ? cl->name : "???")
#define __NFOUT	NFOUTS(cl ? cl->name : "???")

#define NVMEIBS_TOMA_DEBUG	0

//fires when server waits for toma to be killed
static DECLARE_COMPLETION(toma_killed_comp);

struct nvmeibs_toma_connection_hash_entry {
	u64 ib_handle;
	u64 proc_handle; /* hash key */
	struct nvmeibs_client *cl;
	struct hlist_node hlist_next;
};

/*tomahas not been connected yet*/
#define TOMA_STATUS_DISSCONNECTED 0

/*toma is now connected*/
#define TOMA_STATUS_CONNECTED 1

/* Used for TOMA status requests */
#define TOMA_STATUS_MAX_PAGES	128
#define TOMA_STATUS_RESP_TIMEOUT	(2 * HZ)
struct toma_status_req_data
{
	enum nvmeibs_toma_status_type type;
	struct mmap_procfs_ent *mmap;
	void *mmap_pages[TOMA_STATUS_MAX_PAGES];
	struct completion write_comp;
	struct mutex write_mutex; /* protects completion usage from multiple callers */
	spinlock_t handle_req_lock; /* protects handle_req read/write */
	size_t len;
	int handle_req; /* current handle_req for this status request */
};

struct nvmeibs_toma {
	/*toma process id*/
	struct task_struct *task;

	/* toma:clients /proc file interface */
	struct msgloop_procfs_ent *clients_proc;

	/* toma:server  /proc files interface */
	struct msgloop_procfs_ent *server_proc;
	struct msgloop_procfs_ent *server_proc_events;

	struct proc_dir_entry *stat_parent_dir;
	struct proc_dir_entry *stat_dir;
	atomic_t stat_file_count;
	struct completion remove_stat_comp;
	struct toma_status_req_data stat_req_data[NVMEIBS_TOMA_STATUS_AUTO_MAX];

	/* toma rd/wr shared resources lock.
	   Both write and read are process contexts thus
	   lock/unlock does not involve irq save/restore*/
	spinlock_t spinlock;

	/* TOMA write guard.
	   Avoid queueing up toma requests in client's
	   workq so that non-toma work won't have to
	   wait for a whole toma requests burst that
	   proceeded it, to end */
	struct mutex wr_guard;

	//1 if toma connected. Otherwise 0.
	int connection_status;

	/* toma:client connection info hash table */
	struct hlist_head conn_hash[TOMA_CONN_HASH_TABLE_SIZE];
};

struct nvmeibs_toma *toma = NULL;

/* toma:client buffer format as
   received/sent in proc file's
   write/read methods */
struct nvmeibs_toma_client_proc_buf {
	__be64 handle;
	u8 data[];
}__attribute__((packed));

/* container of proc file's
   write methods arguments */
struct nvmeibs_toma_proc_msg {
	char *buf;
	int len;
	void *arg;
};

/* toma send message work */
struct toma_send_msg_workq {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	struct nvmeibs_toma_proc_msg msg;
};

NVMEIBS_MEMMGR_METRIC(toma_mmap_pages, "component=target.toma.mmap_pages");

/* -------------------------------------------------------------------------- */
/*                                                                            */
/*          	     Toma:Client proc-file interface                          */
/*                                                                            */
/* -------------------------------------------------------------------------- */

static
inline u64 toma_conn_handle_ib_2_proc(u64 ib_h, struct nvmeibs_client *cl)
{
	u64 proc_handle;

	NFIN;
	proc_handle = cl->cid;
	proc_handle <<= 32;
	proc_handle |= (ib_h & 0xFFFFFFFF);

	_ND(trace_toma_toma_conn_handle_ib_2_proc, "ib_handle = @IB_H, cid=@CID --> proc_handle = @PROC_HANDLE",
	   ib_h, cl->cid, proc_handle);
	NFOUT;
	return proc_handle;
}

static
inline u64 toma_conn_handle_proc_2_ib(
		struct nvmeibs_toma_connection_hash_entry *h)
{
	return h->ib_handle;
}

static inline u32 h_to_cl_cid(struct nvmeibs_toma_connection_hash_entry *h)
{
	return h->proc_handle >> 32;
}

static struct nvmeibs_toma_connection_hash_entry *
toma_conn_hash_lookup_nolock(u64 proc_handle)
{
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibs_toma_connection_hash_entry *h_curr = NULL;  /* GCC */
	bool found = false;

	NFIN;
	__hash_for_each_possible_safe__(
		toma->conn_hash, h_curr, t_node, h_node, hlist_next, proc_handle) {
		_ND(toma_conn_hash_lookup_nolock_d1, "compare proc-handle 0x@_X", h_curr->proc_handle);
		if (h_curr->proc_handle == proc_handle) {
			_ND(toma_conn_hash_lookup_nolock_d2, "found proc-handle 0x@_X, ib-handle 0x@_X",
			   h_curr->proc_handle, h_curr->ib_handle);
			found = true;
			break;
		}
	}
	_ND(trace_toma_toma_conn_hash_lookup_nolock, "returned hash entry @H_CURR", h_curr);

	NFOUT;
	return found ? h_curr : NULL;
}

static struct nvmeibs_toma_connection_hash_entry *
toma_conn_hash_lookup(u64 handle)
{
	struct nvmeibs_toma_connection_hash_entry *h;
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&toma->spinlock, flags);
	h = toma_conn_hash_lookup_nolock(handle);
	spin_unlock_irqrestore(&toma->spinlock, flags);

	NFOUT;
	return h;
}

static int toma_conn_hash_add(u64 proc_handle, struct nvmeibs_client *cl,
							  u64 ib_handle)
{
	struct nvmeibs_toma_connection_hash_entry *h_curr;
	int rv = -1;
	unsigned long flags;

	__NFIN;

	if (!(h_curr = kzalloc(sizeof(*h_curr), GFP_KERNEL))) {
		_NE(error_toma_toma_conn_hash_add, "Fail to allocate toma connection hash entry");
		goto out;
	}
	h_curr->ib_handle = ib_handle;
	h_curr->proc_handle = proc_handle;
	h_curr->cl = cl;

	spin_lock_irqsave(&toma->spinlock, flags);
	if (toma_conn_hash_lookup_nolock(proc_handle) == NULL) {
		hash_add(toma->conn_hash, &h_curr->hlist_next, proc_handle);
		_NT(trace_toma_toma_conn_hash_add, "Added toma connection ib-handle @IB_HANDLE to hash", ib_handle);
		rv = 0;
	}
	else {
		_NE(error_1_toma_toma_conn_hash_add, "Fail to add toma connection, ib-handle @IB_HANDLE already hashed",
		   ib_handle);
		kfree(h_curr);
	}
	spin_unlock_irqrestore(&toma->spinlock, flags);

out:
	__NFOUT;
	return rv;
}

static int toma_conn_hash_del(u64 proc_handle)
{
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibs_toma_connection_hash_entry *h_curr;
	bool found = false;
	int rv;
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&toma->spinlock, flags);
	__hash_for_each_possible_safe__(toma->conn_hash, h_curr, t_node, h_node,
									hlist_next, proc_handle) {
		if (h_curr->proc_handle == proc_handle) {
			hash_del(&h_curr->hlist_next);
			kfree(h_curr);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&toma->spinlock, flags);

	if (!found) {
		_NE(error_toma_toma_conn_hash_del, "Toma connection proc-handle @PROC_HANDLE not found in hash,"
		   "was not removed", proc_handle);
	} else {
		_NT(trace_toma_toma_conn_hash_del, "Toma connection proc-handle "
			"@PROC_HANDLE removed from hash.", proc_handle);
	}

	rv = found ? 0 : -1;

	NFOUT;
	return rv;
}

/**
 * Remove all hash entries of client @cl.
 * Function is not expected to find such entries unless the
 * client side had not sent toma unregistration command.
 *
 */
void nvmeibs_toma_remove_client(struct nvmeibs_client *cl)
{
	int bucket;
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibs_toma_connection_hash_entry *h_curr;
	int cnt = 0;
	int refcnt;
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&toma->spinlock, flags);

	__hash_for_each_safe__(toma->conn_hash, bucket, t_node, h_node, h_curr,
						   hlist_next) {
		if (h_curr->cl == cl) {
			_NT(nvmeibs_toma_remove_client_t1,
			   "Found toma connection hash entry of client @STR "
			   "(ib_handle 0x@_X) while releasing client",
				cl->name, h_curr->ib_handle);
			hash_del(&h_curr->hlist_next);
			kfree(h_curr);
			cnt++;
		}
	}

	//debug check
	refcnt = atomic_read(&cl->toma_conn_refcnt);
	if (cnt != refcnt) {
		_NE(error_toma_nvmeibs_toma_remove_client, "Found total of @CNT toma connections while releasing client @CL_NAME "
		   "but was expecting @REFCNT", cnt, cl->name, refcnt);
	}
	atomic_set(&cl->toma_conn_refcnt, 0);

	spin_unlock_irqrestore(&toma->spinlock, flags);

	NFOUT;
}

/**
 * Send toma request
 *
 */
enum nvmeib_send_err {
	NVMEIB_SEND_ERR_GENERIC = 1,
	NVMEIB_SEND_ERR_TIMEOUT = 2,
};
static inline int send_toma_req_n_wait(struct nvmeibs_client *cl,
                                       struct nvmeib_iu *send_ioctx,
									   size_t send_len)
{
	int rv = -NVMEIB_SEND_ERR_GENERIC;

	__NFIN;

	/* JH IOMMU: Disabled - Redundant. Done in nvmeibs_client_send_msg
	* ib_dma_sync_single_for_device(P2IB(cl->ib_port), send_ioctx->dma,
								  send_len, DMA_TO_DEVICE);
	*/

	if (nvmeibs_client_send_msg(cl, cl->net, send_ioctx,
		send_len, NVMEIB_TOMA_SEND_REQ, NON_NR_VERSION) < 0) {
		_NT(trace_toma_send_toma_req_n_wait, "Failed to send TOMA request to server");
		goto out;
	}

	if ((rv = wait_for_completion_interruptible_timeout(
		send_ioctx->io_done, NVMEIB_WAIT_FOR_ADMIN_SEND_COMP)) <= 0) {
		_NT(trace_1_toma_send_toma_req_n_wait, "Failed to wait for toma req send completion (rv = @RV), "
		   "release net", rv);
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_TOMA_SND_TIMEOUT);
		rv = -NVMEIB_SEND_ERR_TIMEOUT;
		goto out;
	}

	if (send_ioctx->io_status != IB_WC_SUCCESS) {
		_NT(trace_2_toma_send_toma_req_n_wait, "toma req send_ioctx->io_status =  @IO_STATUS", send_ioctx->io_status);
		goto out;
	}

	/* JH IOMMU: Changed to DMA_TO_DEVICE. direction must be the same as used for map/unmap */
	ib_dma_sync_single_for_cpu(P2IB(cl->ib_port), send_ioctx->dma,
							   send_len, DMA_TO_DEVICE);

	rv = 0;

out:
	__NFOUT;
	return rv;
}

int nvmeibs_toma_send_work_iu_alloc(struct nvmeibs_client *cl)
{
	struct nvmeib_iu *iu;
	int rv = -1;

	BUG_ON(cl->toma_send_work_iu);

	if (cl->net && (iu = nvmeibs_net_get_ioctx(cl->net))) {
		cl->toma_send_work_iu_inuse = false;
		cl->toma_send_work_iu = iu;
		rv = 0;
	}

	return rv;
}

void nvmeibs_toma_send_work_iu_free(struct nvmeibs_client *cl)
{
	struct nvmeib_iu *iu;

	if (cl->toma_send_work_iu) {
		WARN_ON_ONCE(cl->toma_send_work_iu_inuse);
		iu = cl->toma_send_work_iu;
		cl->toma_send_work_iu = NULL;
		nvmeibs_net_put_ioctx(cl->net, iu);
	}
}

static inline struct nvmeib_iu *toma_send_work_iu_get(struct nvmeibs_client *cl)
{
	struct nvmeib_iu *iu = NULL;

	if (!on_wq(cl->wq)) {
		_NW(warn_0_toma_send_work_get,
			"wrong wq");
		WARN_ON_ONCE(1);
	}
	else if (cl->toma_send_work_iu_inuse) {
		_NW(warn_1_toma_send_work_get,
			"already inuse, probably not returned in prev work");
		WARN_ON_ONCE(1);
	}
	else {
		cl->toma_send_work_iu_inuse = true;
		iu = cl->toma_send_work_iu;
	}

	return iu;
}

static inline void toma_send_work_iu_put(struct nvmeibs_client *cl,
										 struct nvmeib_iu *iu)
{
	if (!on_wq(cl->wq)) {
		_NW(warn_0_toma_send_work_iu_put,
			"cl=@CL, wrong wq", cl);
		WARN_ON_ONCE(1);
	}
	else if (!cl->toma_send_work_iu_inuse) {
		_NW(warn_1_toma_send_work_iu_put,
			"cl=@CL, iu not inuse", cl);
		WARN_ON_ONCE(1);
	}
	else if (iu != cl->toma_send_work_iu) {
		_NW(warn_2_toma_send_work_iu_put,
			"OOPS, cl=@CL, iu=@PTR, exp. @PTR",
			cl, iu, cl->toma_send_work_iu);
		BUG();
	}
	else {
		cl->toma_send_work_iu_inuse = false;
	}
}

/**
 * Send toma message over toma (IB) request(s)
 *
 */
static int send_toma_msg(struct nvmeibs_client *cl,
						 struct nvmeibs_toma_proc_msg *msg)
{
	struct nvmeibs_toma_connection_hash_entry *h;
	struct nvmeib_iu *send_ioctx;
	struct volume_server_req *req;
	struct volume_server_toma_req_base *toma_req;
	struct nvmeibs_toma_client_proc_buf *toma_cl_proc_buf;
	int len = msg->len;
	char *data;
	int data_len;
	int req_len;
	int dd;
	struct completion *send_done;
	int wr_len;
	int rv = -1;
	u64 proc_handle;
	u64 ib_handle;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_toma_clnt_req];
	DECLARE_COMPLETION_ONSTACK(rsp_done);
	unsigned long flags;
	int wait_rv;

	__NFIN;

	toma_cl_proc_buf = (struct nvmeibs_toma_client_proc_buf*)msg->buf;

	/* lookup handle and map to client obj */
	proc_handle = toma_cl_proc_buf->handle;
	if (!(h = toma_conn_hash_lookup(proc_handle))) {
		_NT(trace_toma_send_toma_msg, "Fail lookup toma connection by handle @HANDLE; "
		   "Before adding work cl was @CL",
		   toma_cl_proc_buf->handle, cl);
		goto out;
	}

	if (h->cl != cl) {
		_NT(trace_1_toma_send_toma_msg, "Fail lookup toma connection by handle @HANDLE; "
		   "Before adding work cl was @CL, now found h->cl @CL",
		   toma_cl_proc_buf->handle, cl, h->cl);
		goto out;
	}

	/* get send_ioctx */
	if (!(send_ioctx = toma_send_work_iu_get(cl))) {
		_NW(trace_2_toma_send_toma_msg, "Failed to get send_ioctx for sending toma request to client @CL_NAME", cl->name);
		WARN_ON_ONCE(1);
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_TOMA_SND_GET_IU_FAILED);
		goto out;
	}

	switch (vex_ops->vex_ext) {
	case vex_base: break;
	default: BUG();
	}

	/* build toma-ib request to client */

	cl->clnt_toma_rsp.tag_cntr++;
	if (cl->clnt_toma_rsp.tag_cntr == 0)
		cl->clnt_toma_rsp.tag_cntr++;

	req = send_ioctx->buf;
	toma_req = &req->toma_req.base;
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_TOMA_REQ;
	req->hdr.tag = cl->clnt_toma_rsp.tag_cntr;
	req->version_tag = cpu_to_be16(vex_base);
	ib_handle = toma_conn_handle_proc_2_ib(h);
	toma_req->handle = cpu_to_be64(ib_handle);
	_ND(trace_3_toma_send_toma_msg, "send to client: ib_handle=@IB_HANDLE, proc_handle=@PROC_HANDLE",
	   ib_handle, proc_handle);

	dd = 0;
	wr_len = 0;
	data = toma_cl_proc_buf->data;
	data_len = len - offsetof(struct nvmeibs_toma_client_proc_buf, data);
	_ND(trace_4_toma_send_toma_msg, "Sending toma msg data_len @DATA_LEN", data_len);
	send_done = kzalloc(sizeof (struct completion), GFP_KERNEL);
	if (!send_done) {
			_NE(error_toma_send_toma_msg, "Memory allocation problem");
			rv = -ENOMEM;
			goto free_iu;
	}
	init_completion(send_done);

	/* [NVMESH-1153]: Prepare for wait for response */
	spin_lock_irqsave(&cl->clnt_toma_rsp.comp_lock, flags);
	BUG_ON(cl->clnt_toma_rsp.comp);
	cl->clnt_toma_rsp.comp = &rsp_done;
	spin_unlock_irqrestore(&cl->clnt_toma_rsp.comp_lock, flags);

	while (data_len) {
		req_len = min(data_len, NVMEIB_TOMA_REQ_MAX_LEN);
		nvmeib_container_raw_payload_init(
			&toma_req->data_ctnr, NVMEIB_TOMA_REQ_DATA_MAGIC, vex_base, req_len);
		vex_memcpy(NVMEIB_CONT_PAYLOAD(&toma_req->data_ctnr), data, req_len);

		if (wr_len == 0) {
				toma_req->position = (data_len == req_len) ?
				NVMEIB_TOMA_REQ_STANDALONE : NVMEIB_TOMA_REQ_FIRST;
		}
		else {
				toma_req->position = (data_len == req_len)?
				NVMEIB_TOMA_REQ_LAST : NVMEIB_TOMA_REQ_MIDDLE;
		}

		data += req_len;
		data_len -= req_len;

		//BUG_ON(send_ioctx->io_done);
		send_ioctx->io_done = send_done;
		_ND(trace_5_toma_send_toma_msg, "send TOMA request (fragment @DD) position @NVMEIB_TOMA_POS_STR, remaining data length @DATA_LEN",
		   dd, nvmeib_toma_pos_str(toma_req->position), data_len);
		if ((rv = send_toma_req_n_wait(cl, send_ioctx,
			offsetof(struct volume_server_req, toma_req.base.data_ctnr.payload) + req_len)) < 0) {
			_NT(trace_6_toma_send_toma_msg, "toma send message failed @RV", rv);
			goto clear_rsp_comp;
		}
		_ND(trace_7_toma_send_toma_msg, "TOMA request (fragment @DD) send comp", dd);
		dd++;

		wr_len += req_len;
		nvmeib_reinit_completion(send_ioctx->io_done);
	}

	_ND(trace_10_toma_send_toma_msg, "@CL (@CL_NAME), Waiting for client response (tag @TAG)",
	    cl, cl->name, req->hdr.tag);
	/* [NVMESH-1153]: Wait for response so the next TOMA message doesn't clobber the accumulation buffer **/
	if ((wait_rv = wait_for_completion_timeout(&rsp_done, NVMEIB_WAIT_FOR_TOMA_RSP_TIMEOUT)) <= 0) {
		_NW(trace_8_toma_send_toma_msg, "@CL (@CL_NAME), Failed (@RV) to wait for Client Response (tag @TAG)",
		    cl, cl->name, rv, req->hdr.tag);
		rv = -ETIMEDOUT;
		goto clear_rsp_comp;
	}
	_ND(trace_11_toma_send_toma_msg, "@CL (@CL_NAME), Got client response (tag @TAG)",
	    cl, cl->name, req->hdr.tag);

	if (cl->clnt_toma_rsp.opcode == NVMEIBC_RSP_TOMA_OPCODE_ERR) {
		_NW(trace_9_toma_send_toma_msg, "@CL (@CL_NAME), Client responded with error", cl, cl->name);
		rv = -EREMOTEIO;
		goto clear_rsp_comp;
	}

	rv  = wr_len;

clear_rsp_comp:
	spin_lock_irqsave(&cl->clnt_toma_rsp.comp_lock, flags);
	BUG_ON(cl->clnt_toma_rsp.comp != &rsp_done);
	cl->clnt_toma_rsp.comp = NULL;
	spin_unlock_irqrestore(&cl->clnt_toma_rsp.comp_lock, flags);

free_iu:
	kfree(send_done);
	send_ioctx->io_done = NULL;
	toma_send_work_iu_put(cl, send_ioctx);

out:
	__NFOUT;
	return rv;
}

static void send_toma_msg_work(struct workqe_struct *work)
{
	struct toma_send_msg_workq *twork =
		container_of(work, struct toma_send_msg_workq, work);
	struct nvmeibs_client *cl = twork->cl;
	int dying = atomic_read(&cl->net->dying);
	int c_dying = atomic_read(&cl->dying);

	__NFIN;

	if (dying) {
		_NT(trace_toma_send_toma_msg_work_net_dying, "net dying - not sending toma request. client dying @INT", c_dying);
		goto out;
	}
	if (c_dying) {
		_NT(trace_toma_send_toma_msg_work_client_dying, "client dying - not sending toma request");
		goto out;
	}
	send_toma_msg(cl, &twork->msg);

out:
	kfree(twork->msg.buf);
	kfree(twork);

	__NFOUT;
}

/**
 * Defer work of sending toma message over toma (IB) request(s).
 * The work itself must be time-bounded.
 *
 * rv = 0, tells the caller not to free @buf
 */
static int nvmeibs_toma_send_msg(u32 cid, void *arg, char *buf, size_t len)
{
	struct toma_send_msg_workq *work;
	unsigned long flags;
	struct nvmeibs_client *cl = NULL;
	int rv = -1;
	__NFIN;

	if (!(work = kzalloc(sizeof(*work), GFP_KERNEL))) {
		_NE(error_toma_nvmeibs_toma_send_msg, "Fail to alloc work");
		rv = -ENOMEM;
		goto out;
	}

	flags = nvmeibs_cdb_lock();
	if ((cl = nvmeibs_find_client_(cid, NULL))) {
		WQ_INIT_WORK(&work->work, send_toma_msg_work);
		work->cl = cl;
		work->msg.buf = buf;
		work->msg.len = len;
		work->msg.arg = arg;
		nvmeibs_client_add_work(cl, &work->work);
		_ND(trace_toma_nvmeibs_toma_send_msg, "Added work send toma msg to client @CL_NAME", cl->name);
		rv = 0;
	}
	else {
		kfree(work);
		rv = -ENXIO;
	}
	nvmeibs_cdb_unlock(flags);

out:

	__NFOUT;
	return rv;
}

/**
 * Process buffer from toma clients' proc file and forward a
 * toma request to client corresponding to toma connection
 * handle embedded in @buf.
 *
 * This is a callback function called from Toma proc write()
 * file op.
 *
 */
int nvmeibs_toma_client_proc_recv(void *arg, char *buf, size_t len,
	bool *posted)
{
	struct nvmeibs_toma_connection_hash_entry *h;
	struct nvmeibs_toma_client_proc_buf *toma_cl_proc_buf;
	unsigned long flags;
	int rv = -1;
	u32 cid = 0;

	NFIN;

#if NVMEIBS_TOMA_DEBUG
	_NI(nvmeibs_toma_client_proc_recv_i1, "Dump toma message of length @SZ bytes:", len);
	_Dbuf(buf, len);
#endif

	/* checks */
	if (len < offsetof(struct nvmeibs_toma_client_proc_buf, data)) {
		_NE(error_toma_nvmeibs_toma_client_proc_recv, "Invalid data length (@LEN_LONG), 'handle' bytes must be included", len);
		rv = -EINVAL;
		goto out;
	}
	if (len > NVMEIB_TOMA_BUF_MAX_LEN) {
		_NE(error_1_toma_nvmeibs_toma_client_proc_recv, "Invalid buffer length (@LEN_LONG) - exceeds max (@MAX)",
		   len, NVMEIB_TOMA_BUF_MAX_LEN);
		rv = -EFBIG;
		goto out;
	}

	/* lookup handle and map to client obj */
	toma_cl_proc_buf = (struct nvmeibs_toma_client_proc_buf*)buf;
	spin_lock_irqsave(&toma->spinlock, flags);
	h = toma_conn_hash_lookup_nolock(toma_cl_proc_buf->handle);
	if (h)
		cid = h_to_cl_cid(h);
	spin_unlock_irqrestore(&toma->spinlock, flags);

	if (!h) {
		_NT(trace_toma_nvmeibs_toma_client_proc_recv,
		    "Fail lookup toma connection by handle @HANDLE",
		   toma_cl_proc_buf->handle);
		rv = -ENXIO;
		goto out;
	}

	if (!cid) {
		_NT(trace_2_toma_nvmeibs_toma_client_proc_recv,
		    "Invalid CID 0 from TOMA hash");
		rv = -EINVAL;
		goto out;
	}

	mutex_lock(&toma->wr_guard);
	rv = nvmeibs_toma_send_msg(cid, arg, buf, len);
	mutex_unlock(&toma->wr_guard);
	*posted = (rv == 0) ? true : false;
	if (rv == 0) {
		rv = len;
	}

out:
	NFOUT;
	return rv;
}

static void prepare_subscriber_evt_msg(struct nvmeibs_client *cl,
								u64 toma_conn_proc_handle,
								struct nvmeibs_toma_server_proc_buf *proc_buf,
								bool is_subscribe)
{
	struct nvmeibs_toma_subscriber_change_msg *msg;
	NFIN;

	memset(proc_buf, 0, sizeof(*proc_buf));
	proc_buf->zero = 0; /* server event must set 0 (see common/nvmeib_shared.h) */
	proc_buf->type = NVMEIBS_TOMA_REPORT_EVENT_SUBSCRIBER_CHANGE;
	msg = &(proc_buf->subscriber_change_msg);
	memset(msg, 0, sizeof(*msg));
	strlcpy(msg->disk_name, cl->disk_name, sizeof(msg->disk_name));
	strlcpy(msg->host_name, cl->host_name, sizeof(msg->host_name));
	memcpy((void*)&msg->client_uuid.bytes, (void*)&cl->client_uuid,
		   sizeof(msg->client_uuid.bytes));
	msg->cid = cl->cid;
	msg->toma_conn_proc_handle = toma_conn_proc_handle;
	msg->is_subscribe = is_subscribe;
	_NT(trace_toma_prepare_subscriber_evt_msg, "@MSG_TYPE_STR cid=@CID, disk_name=@DISK_NAME, host_name=@HOST_NAME",
	   msg->is_subscribe ? "Subscribe" : "Unsubscribe",
	   msg->cid, msg->disk_name, msg->host_name);

	NFOUT;
}

struct toma_req_ctx {
	u8 cmd_type;
	u64 toma_conn_proc_handle;
	u64 toma_conn_ib_handle;
	struct nvmeibs_toma_server_proc_buf *toma_srv_proc_buf;
	size_t toma_srv_proc_buf_len;
	const void *pb_buf;
	size_t pb_len;
	int payload_len;
	bool connect_msg;
	struct nvmeibs_client *cl;
};

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_clnt_toma_req_srv_base_decode)
{
	const struct volume_client_toma_req_base *toma_req = wire_buf;
	struct toma_req_ctx *req_ctx = arg;
	struct nvmeibs_client *cl = req_ctx->cl;
	ssize_t rv;
	size_t ctnr_payload_sz = 0;

	BUG_ON(!wire_buf);
	BUG_ON((const void *)(toma_req + 1) > wire_buf_end);

	req_ctx->cmd_type = toma_req->cmd_type;
	req_ctx->toma_conn_ib_handle = be64_to_cpu(toma_req->handle);
	req_ctx->toma_conn_proc_handle = toma_conn_handle_ib_2_proc(req_ctx->toma_conn_ib_handle, cl);

	_ND(VEX_OPS_DECLARE_OP_FN_d1, "Process toma cmd @STR: client @PTR, handle @_X",
	   nvmeib_toma_cmd_str(req_ctx->cmd_type), cl, req_ctx->toma_conn_ib_handle);

	if (req_ctx->cmd_type == NVMEIB_TOMA_CMD_REG || req_ctx->cmd_type == NVMEIB_TOMA_CMD_UNREG) {
		req_ctx->toma_srv_proc_buf_len = sizeof(*req_ctx->toma_srv_proc_buf);
	} else {
		if ((rv = nvmeib_container_validate(
				&toma_req->data_ctnr, NVMEIB_TOMA_REQ_DATA_MAGIC, ops->vex_ext)) < 0) {
			goto out;
		}
		ctnr_payload_sz = nvmeib_container_payload_size(&toma_req->data_ctnr);
		req_ctx->toma_srv_proc_buf_len = offsetof(typeof(*req_ctx->toma_srv_proc_buf), buf) + ctnr_payload_sz;
	}
	if (!(req_ctx->toma_srv_proc_buf = kzalloc(req_ctx->toma_srv_proc_buf_len, GFP_KERNEL))) {
		_NE(VEX_OPS_DECLARE_OP_FN_e1, "OOM Error");
		rv = -ENOMEM;
		goto out;
	}
	/* update toma hash table */
	switch (req_ctx->cmd_type) {
	case NVMEIB_TOMA_CMD_REG:
		if (!(rv = toma_conn_hash_add(req_ctx->toma_conn_proc_handle, cl,
			req_ctx->toma_conn_ib_handle))) {
			atomic_inc(&cl->toma_conn_refcnt);
			prepare_subscriber_evt_msg(cl, req_ctx->toma_conn_proc_handle,
									   req_ctx->toma_srv_proc_buf, true);
			req_ctx->connect_msg = true;
		}
		break;
	case NVMEIB_TOMA_CMD_UNREG:
		if (!(rv = toma_conn_hash_del(req_ctx->toma_conn_proc_handle))) {
			atomic_dec(&cl->toma_conn_refcnt);
			prepare_subscriber_evt_msg(cl, req_ctx->toma_conn_proc_handle,
									   req_ctx->toma_srv_proc_buf, false);
		}
		break;
	default:
		req_ctx->toma_srv_proc_buf->handle = req_ctx->toma_conn_proc_handle;
		vex_memcpy(req_ctx->toma_srv_proc_buf->buf, NVMEIB_CONT_PAYLOAD(&toma_req->data_ctnr), ctnr_payload_sz);
		rv = 0;
		break;
	}

out:
	return sizeof(*toma_req) + ctnr_payload_sz;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_clnt_toma_req_srv_ext1_decode)
{
	const struct volume_client_toma_req_ext1 *toma_req = wire_buf;
	struct toma_req_ctx *req_ctx = arg;
	ssize_t rv;

	if (!wire_buf) {
		req_ctx->pb_buf = NULL;
		req_ctx->pb_len = 0;
		rv = 0;
		goto out;
	}
	BUG_ON((const void *)(toma_req + 1) > wire_buf_end);

	if ((rv = nvmeib_container_validate(
			&toma_req->srv_pb_ctnr, NVMEIB_TOMA_REQ_PB_MAGIC, ops->vex_ext)) < 0) {
		goto out;
	}
	req_ctx->pb_buf = NVMEIB_CONT_CONST_PAYLOAD(&toma_req->srv_pb_ctnr);
	req_ctx->pb_len = nvmeib_container_payload_size(&toma_req->srv_pb_ctnr);
	rv = sizeof(*toma_req) + req_ctx->pb_len;

out:
	return rv;
}

/**
 * Process toma request sent from client @cl and forward a
 * buffer over the toma clients' proc file.
 *
 * This function is called from a work queue to serialize
 * concurrent toma requests from different client's contexts
 * or different clients.
 *
 */
void nvmeibs_toma_client_proc_send(struct nvmeibs_client *cl,
								   struct nvmeib_iu *recv_ioctx,
								   struct nvmeib_iu *send_ioctx)
{
	const struct volume_client_req *req = recv_ioctx->buf;
	u8 rsp_opcode;
	void *payload = NULL;
	struct toma_req_ctx req_ctx = {};
	int rv;
	const struct vex_ops *req_vex_ops = cl->vex_ach_ops[vex_ach_clnt_toma_req];

	__NFIN;

	BUG_ON(!send_ioctx || !toma->server_proc_events);

	if (be16_to_cpu(req->version_tag) != req_vex_ops->vex_ext) {
		_NT(nvmeibs_toma_client_proc_send_t1, "Invalid version tag @UINT in NVMEIB_TOMA_REQ (should be @UINT)",
		   be16_to_cpu(req->version_tag), req_vex_ops->vex_ext);
		rsp_opcode = NVMEIBS_RSP_TOMA_OPCODE_ERR;
		goto send_rsp;
	}

	req_ctx.cl = cl;

	rv = CALL_VEX_OP(decode,
			vex_ach_clnt_toma_req_srv_ops, ONE_EXT,
				base, vex_ach_clnt_toma_req_srv_base_decode,
				ext1, vex_ach_clnt_toma_req_srv_ext1_decode,
					req_vex_ops, NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(req),
					recv_ioctx->buf + recv_ioctx->size, &req_ctx);
	rsp_opcode = (rv >= 0) ? NVMEIBS_RSP_TOMA_OPCODE_OK : NVMEIBS_RSP_TOMA_OPCODE_ERR;

	if (req_ctx.pb_buf && req_ctx.pb_len) {
		if (req_ctx.pb_len == sizeof(struct nvmeibs_lost_srv_resource_payload)) {
			struct nvmeibs_lost_srv_resource_payload *loser_payload = (void*)req_ctx.pb_buf;
			_NT(trace_1_toma_nvmeibs_toma_client_proc_send, "Processing piggyback nvmeibs_lost_srv_resource_payload on cmd_type @CMD_TYPE", req_ctx.cmd_type);
			BUG_ON(cl->jrnl_rng == NVMEIB_EC_INVALID_JOURNAL_RANGE);
			nvmeibs_serjio_abnd_jrnl_ents(cl->di, cl->jrnl_rng, loser_payload->bmp, loser_payload->gen_ids);
		} else {
			_NW(nvmeibs_toma_client_proc_send_w1, "Unknown piggyback of size @ZU", req_ctx.pb_len);
		}
	}

	/* foward to toma */
	if (req_ctx.toma_srv_proc_buf && req_ctx.toma_srv_proc_buf_len) {
		if (!(rv = nvmeib_msgloop_send(toma->server_proc_events,
				(char *)req_ctx.toma_srv_proc_buf, req_ctx.toma_srv_proc_buf_len))) {
			rsp_opcode = NVMEIBS_RSP_TOMA_OPCODE_OK;
			if (req_ctx.connect_msg)
				++cl->connected_to_toma;
		}
		else {
			_NT(error_toma_nvmeibs_toma_client_proc_send, "Failed to send message (cmd-type=@TYPE) from client @CL_NAME to toma, "
			   "error @RV", req_ctx.cmd_type, cl->name, rv);
			rsp_opcode = NVMEIBS_RSP_TOMA_OPCODE_ERR;
		}
	}

	/* send toma response to client */
send_rsp:
	if ((rv = nvmeibs_client_send_rsp(cl, cl->net, rsp_opcode, req->hdr.tag, req->version_tag,
		send_ioctx, payload, req_ctx.payload_len, NVMEIB_TOMA_SEND_RSP, NON_NR_VERSION))) {
		_NT(trace_2_toma_nvmeibs_toma_client_proc_send, "Failed to send TOMA response (rv = @RV) to client @CL_NAME; put send_ioctx", rv, cl->name);
		nvmeibs_net_put_ioctx(cl->net, send_ioctx);
	}
	else {
		_ND(trace_3_toma_nvmeibs_toma_client_proc_send, "sent toma-rsp tag @TAG", req->hdr.tag);
	}

	kfree(req_ctx.toma_srv_proc_buf);
	__NFOUT;
}

/* -------------------------------------------------------------------------- */
/*                                                                            */
/*          	     Toma:Server proc-file interface                          */
/*                                                                            */
/* -------------------------------------------------------------------------- */
static int process_disk_segment_lock_gid_get(
	struct nvmeibs_toma_disk_segment_lock_gid_req *req)
{
	int num_gids = MAX_PORTS_FOR_LOCKS_GIDS;
	union ib_gid gids[num_gids];
	struct nvmeibs_toma_server_proc_buf proc_buf;
	struct nvmeibs_toma_disk_segment_lock_gid_rsp *rsp;
	int rv = -1;

	NFIN;

	memset(gids, 0, sizeof(gids));
	if ((rv = nvmeibs_disk_locks_get_dev(req->disk_id, gids, &num_gids))) {
		_NE(error_toma_process_disk_segment_lock_gid_get, "Fail to find lock gid for disk @DISK_ID_STR, seg id @SEG_ID_INT",
		   req->disk_id, req->seg_id);
		rv = -ENODEV;
		goto out;
	}

	memset(&proc_buf, 0, sizeof(proc_buf));
	proc_buf.zero = 0; /* server event must use 0 (see common/nvmeib_shared.h) */
	proc_buf.type = NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_RSP;
	rsp = (struct nvmeibs_toma_disk_segment_lock_gid_rsp *)&proc_buf.lock_gid_rsp;
	memcpy(rsp->disk_id, req->disk_id, sizeof(rsp->disk_id));
	rsp->seg_id = req->seg_id;
	rsp->num_gids = num_gids;
	memcpy(rsp->gids, gids, sizeof(rsp->gids));

	if ((rv = nvmeibs_toma_server_proc_send(&proc_buf)) < 0) {
		_NT(trace_toma_process_disk_segment_lock_gid_get, "Fail to send lock-gid-rsp for disk @DISK_ID_STR, seg id @SEG_ID_INT",
		   req->disk_id, req->seg_id);
		goto out;
	}

	rv = 0;
out:
	NFOUT;
	return rv;
}

static int process_disk_segment_lock_gid_req(
	struct nvmeibs_toma_disk_segment_lock_gid_req *req)
{
	int rv = -EINVAL;
	NFIN;

	switch (req->op) {
	case NVMEIBS_TOMA_LOCK_GID_OP_GET:
		rv = process_disk_segment_lock_gid_get(req);
		break;

	case NVMEIBS_TOMA_LOCK_GID_OP_PUT:
		rv = nvmeibs_disk_locks_put_dev(req->disk_id);
		break;

	default:
		_NE(error_toma_process_disk_segment_lock_gid_req, "Unknown lock-gid req op @OP", req->op);
		break;
	}

	NFOUT;
	return rv;
}

#define CLIENT_DISCONNECT_MSG_VEC_COUNT 2
static void prepare_client_disconnect_msg(
	struct nvmeibs_client *cl,
	struct nvmeibs_toma_server_proc_buf *proc_buf,
	struct msg_vec *msg_vec,
	int *vec_cnt)
{
	NFIN;

	proc_buf->zero = 0; /* server event must use 0 (see common/nvmeib_shared.h) */
	proc_buf->type = NVMEIBS_TOMA_REPORT_EVENT_CLIENT_DISCONNECT;
	proc_buf->client_disconnect_msg_hdr.cid = cl->cid;

	msg_vec[0].data = (char *)(proc_buf);
	msg_vec[0].len = sizeof(*proc_buf);
	{
		_NT(trace_1_toma_prepare_client_disconnect_msg, "cl @CL_NAME", cl->name);
		proc_buf->client_disconnect_msg_hdr.payload_len = 0;
		msg_vec[1].data = 0;
		msg_vec[1].len = 0;
		*vec_cnt = 1;
	}

	NFOUT;
	return;
}

static int process_client_disconnect_force_cmd(
	struct nvmeibs_toma_client_disconnect_force_cmd *cmd)
{
	struct nvmeibs_ib_port *ib_port = NULL;
	struct nvmeibs_client *cl = nvmeibs_find_client(cmd->cid, &ib_port);
	int rv;

	NFIN;

	if (cl && ib_port) {
		_NT(trace_toma_process_client_disconnect_force_cmd, "Calling free client @CL  cid=@CID", cl, cmd->cid);
		rv = nvmeibs_ib_port_free_client(ib_port, cmd->cid,
		 NVMEIBS_LOGOUT_REASON_TOMA_DISCONNECT);
	}
	else {
		_NW(warn_toma_process_client_disconnect_force_cmd, "Client with cid=@CID was not found", cmd->cid);
		rv = 0;
	}

	NFOUT;
	return rv;
}


static int process_journal_info(struct nvmeibs_toma_journal_msg* journal_msg)
{
	struct list_head* disks;
	struct nvmeibs_disk_info *di;
	int found = 0;
	int rv = 0;

	NFIN;
	disks = nvmeibs_disk_get_disks(NULL);

	list_for_each_entry(di, disks, link) {
		if (strcmp(di->disk_id, journal_msg->disk_id) == 0) {
			_NT(trace_toma_process_journal_info, "found disk @DISK_ID_STR", di->disk_id);
			found = 1;
			rv = nvmeibs_serjio_set_journal_info(di, journal_msg);

			break;
		}
	}
	nvmeibs_disk_put_disks();

	if (!found) {
		_NT(trace_1_toma_process_journal_info, "Didn't find disk @DISK_ID_STR    lba @LBA_LLONG", journal_msg->disk_id, journal_msg->lba);
		rv = -1;
	}
	NFOUT;

	return rv;
}

static int process_clean_journal_msg(struct nvmeibs_toma_clean_journal_for_range_msg* clean_journal_msg)
{
	struct list_head* disks;
	struct nvmeibs_disk_info *di;
	int found = 0;
	int rv = 0;

	NFIN;
	disks = nvmeibs_disk_get_disks(NULL);

	list_for_each_entry(di, disks, link) {
		if (strcmp(di->disk_id, clean_journal_msg->disk_id) == 0 &&
			nvmeib_ref_get(&di->controller_ops_ref)) {
			_NT(trace_toma_process_clean_journal_msg, "found disk @DISK_ID_STR", di->disk_id);
			found = 1;
			break;
		}
	}
	nvmeibs_disk_put_disks();

	if (found) {
		rv = nvmeibs_serjio_clean_journal_for_disk_range(di, clean_journal_msg->seg_uuid,
								 clean_journal_msg->start_4Klba, clean_journal_msg->end_4Klba, clean_journal_msg->seg_deleted);
		nvmeib_ref_put(&di->controller_ops_ref);
	} else {
		_NT(trace_1_toma_process_clean_journal_msg, "Didn't find disk @DISK_ID_STR    lba @START_4KLBA - @END_4KLBA",
		   clean_journal_msg->disk_id, clean_journal_msg->start_4Klba,
			clean_journal_msg->end_4Klba);
		rv = -1;
	}
	NFOUT;

	return rv;
}

static void on_toma_open(void)
{
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&toma->spinlock, flags);
	toma->task = current->group_leader;
	_NI(trace_toma_on_toma_open, "TOMA (pid @PID)", toma->task->pid);
	spin_unlock_irqrestore(&toma->spinlock, flags);

	NFOUT;
}

static void toma_logout(void)
{
	unsigned long flags;

	NFIN;
	if (!toma) {
		goto out;
	}

	_NI(trace_toma_toma_logout, "TOMA (pid @PID) logging out", toma->task->pid);

	spin_lock_irqsave(&toma->spinlock, flags);
	toma->connection_status = TOMA_STATUS_DISSCONNECTED;
	nvmeibs_um_comm_update_toma_state(nvmeibs_get_um_comm(), false);
	spin_unlock_irqrestore(&toma->spinlock, flags);

	nvmeibs_remove_all_clients(false, true, NVMEIBS_LOGOUT_REASON_TOMA_LOGOUT);

	spin_lock_irqsave(&toma->spinlock, flags);

	nvmeib_msgloop_flush(toma->server_proc_events);
	nvmeib_msgloop_flush(toma->server_proc);
	nvmeib_msgloop_flush(toma->clients_proc);

	spin_unlock_irqrestore(&toma->spinlock, flags);

out:
	NFOUT;
}

static int on_toma_login(void)
{
	unsigned long flags;
	struct list_head *disks;
	struct nvmeibs_disk_info *di_iter;
	int rv;

	NFIN;

	if (!toma) {
		rv = -ECHILD;
		goto out;
	}

	spin_lock_irqsave(&toma->spinlock, flags);
	switch (toma->connection_status) {
	/*tomahas not been connected yet*/
	case TOMA_STATUS_DISSCONNECTED:
		BUG_ON(toma->task == NULL);
		spin_unlock_irqrestore(&toma->spinlock, flags);
		/* Make sure all clients from the previous TOMA
		* have been removed and clear no_new_clients */
		rv = nvmeibs_remove_all_clients(true, true, NVMEIBS_LOGOUT_REASON_TOMA_LOGIN_CLEAR_ALL_CLIENTS);
		if (rv < 0)
			goto out;

		spin_lock_irqsave(&toma->spinlock, flags);
		if (toma->connection_status == TOMA_STATUS_DISSCONNECTED) {
			toma->connection_status = TOMA_STATUS_CONNECTED;
			nvmeibs_um_comm_update_toma_state(nvmeibs_get_um_comm(), true);
			nvmeib_msgloop_flush(toma->server_proc_events);
			nvmeib_msgloop_flush(toma->server_proc);
			nvmeib_msgloop_flush(toma->clients_proc);
			spin_unlock_irqrestore(&toma->spinlock, flags);

			_NI(trace_toma_on_toma_login, "Toma connection established. toma process id is @PID",
				toma->task->pid);

			/* Inform TOMA about the status of all SERJIOs */
			disks = nvmeibs_disk_get_disks(NULL);
			list_for_each_entry(di_iter, disks, link) {
				nvmeibs_toma_report_event_serjio_state_change(
					di_iter->disk_id, nvmeibs_nvme_get_vendor(di_iter),
					nvmeibs_nvme_get_model(di_iter->dev), nvmeibs_serjio_get_status(di_iter));
			}
			nvmeibs_disk_put_disks();
			spin_lock_irqsave(&toma->spinlock, flags);
		}
		break;
	case TOMA_STATUS_CONNECTED:
		_NT(trace_1_toma_on_toma_login, "Received login connection when toma was already connected");
		rv = -EPROTO;
		break;
	default:
		_NE(error_toma_on_toma_login, "Unknown Toma:Server connection status @CONNECTION_STATUS",
		   toma->connection_status);
		rv = -EPROTO;
		break;
	}
	spin_unlock_irqrestore(&toma->spinlock, flags);

out:
	NFOUT;
	return rv;
}

/*force toma to shut down*/
void nvmeibs_toma_shut_down(void)
{
	unsigned long flags;
	int num_clients;
	int timeout;
	int wait_rv;

	NFIN;
	if (!toma) {
		goto out;
	}

	spin_lock_irqsave(&toma->spinlock, flags);
	if (toma->task == NULL) {
		_NT(trace_toma_nvmeibs_toma_shut_down, "TSHDOWN: toma is not present, no need to kill nothing");
		spin_unlock_irqrestore(&toma->spinlock, flags);
		goto out;
	}

	init_completion(&toma_killed_comp);

	_NT(trace_1_toma_nvmeibs_toma_shut_down, "TSHDOWN: sending kill signal to toma  @PID", toma->task->pid);
	send_sig(SIGKILL, toma->task, 0);

	spin_unlock_irqrestore(&toma->spinlock, flags);

	num_clients = nvmeibs_cdb_count(true);
	timeout = WAIT_REM_ALL_CL_CNST + num_clients * WAIT_REM_ALL_CL_MULT;
	_NT(trace_2_toma_nvmeibs_toma_shut_down, "TSHDOWN: Waiting for @TIMEOUT seconds for TOMA and @NUM_CLIENTS clients to shut down",
	   timeout, num_clients);
	wait_rv = wait_for_completion_timeout(&toma_killed_comp, timeout * HZ);
	_NT(trace_3_toma_nvmeibs_toma_shut_down, "TSHDOWN: toma down");
	if (!wait_rv){
		_NE(error_toma_nvmeibs_toma_shut_down, "Toma kill wait ended with timeout");
	} else if (wait_rv < 0) {
		_NE(error_1_toma_nvmeibs_toma_shut_down, "Toma wait for kill ended with error @WAIT_RV_INT", wait_rv);
	}

out:
	NFOUT;
}

void nvmeibs_toma_server_proc_close(void *arg)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *di_iter;
	unsigned long flags;

	NFIN;

	_NT(trace_toma_nvmeibs_toma_server_proc_close, "toma=@TOMA_PTR   connection_status=@CONNECTION_STATUS", toma, (toma ? toma->connection_status : 0));
	if (!toma) {
		goto out;
	}
	spin_lock_irqsave(&toma->spinlock, flags);
	switch (toma->connection_status) {
	case TOMA_STATUS_DISSCONNECTED:
		BUG_ON(toma->task == NULL);
		toma->task = NULL;
		complete(&toma_killed_comp);
		break;
	case TOMA_STATUS_CONNECTED:
		_NT(trace_1_toma_nvmeibs_toma_server_proc_close, "toma disconnecting (process id @PID)", toma->task->pid);
		toma->connection_status = TOMA_STATUS_DISSCONNECTED;
		nvmeibs_um_comm_update_toma_state(nvmeibs_get_um_comm(), false);
		spin_unlock_irqrestore(&toma->spinlock, flags);
		BUG_ON(toma->task == NULL);
		nvmeibs_remove_all_clients(false, true, NVMEIBS_LOGOUT_REASON_TOMA_PROC_CLOSE);

		/* If any SERJIO is in the middle of a GPT update, tell it to cancel it */
		disks = nvmeibs_disk_get_disks(NULL);
		list_for_each_entry(di_iter, disks, link) {
			nvmeibs_serjio_gpt_update_cancel(di_iter);
		}
		nvmeibs_disk_put_disks();

		spin_lock_irqsave(&toma->spinlock, flags);
		toma->task = NULL;
		_NT(trace_2_toma_nvmeibs_toma_server_proc_close, "TSHDOWN: signal completion of toma down");
		complete(&toma_killed_comp);
		break;
	}
	spin_unlock_irqrestore(&toma->spinlock, flags);
	/* Removing all external drives - by design only Toma should add disks */
	_NT(nvmeibs_toma_server_proc_close_nvmeof, "Toma logout, freeing all nvmeof resources");
	nvmeibs_nvme_free_all_nvmeof();
	/* cleaup after toma regardless its state */
	nvmeibs_disk_locks_toma_proc_close();

out:
	NFOUT;
}

/*return true if toma is connected*/
bool nvmeibs_toma_is_connected(void)
{
	unsigned long flags;
	bool ret;

	NFIN;
	spin_lock_irqsave(&toma->spinlock, flags);
	ret = toma->connection_status == TOMA_STATUS_CONNECTED;
	spin_unlock_irqrestore(&toma->spinlock, flags);

	NFOUT;
	return ret;
}

void nvmeibs_toma_server_proc_open(void *arg)
{
	NFIN;
	on_toma_open();
	NFOUT;
}

static int __handle_toma_blkset_recovered_ack(u64 raw_cookie, int toma_rv) {
	union nvmeibs_async_cookie cookie = {.raw = raw_cookie};
	struct nvmeibs_async_cookie_data *cookie_data = NULL;
	int rv;

	_NT(trace__handle_toma_blkset_recovered_ack,
	    "Got a cookie from toma @COOKIE, rv=@RV", raw_cookie, toma_rv);

	if ((rv = nvmeibs_cdb_cid_fast_call_code(cookie.uniq.cid, cl, {
		     cookie_data = nvmeibs_async_cookie_store_pull_cookie(
		         &cl->cookie_store, raw_cookie);
	     }))) {
		_NW(warn1__handle_toma_blkset_recovered_ack,
		    "Got a cookie for unregistered client @COOKIE cid=@CID rv=@RV", raw_cookie, cookie.uniq.cid, rv);
		goto out;
	}

	if (!cookie_data) {
		_NW(warn2__handle_toma_blkset_recovered_ack,
		    "Got an expired cookie from toma @COOKIE cid=@CID", raw_cookie, cookie.uniq.cid);
		rv = -ENOENT;
		goto out;
	}
	nvmeibs_async_cookie_store_put_cookie(cookie_data, toma_rv);

	rv = 0;

out:
	return rv;
}

/**
 * Process buffer from toma server's proc file.
 *
 * This is a callback function called from Toma proc write()
 * file op.
 *
 */

int nvmeibs_toma_server_proc_recv(void *arg, char *buf, size_t len,
	bool *posted)
{
	struct nvmeibs_toma_server_proc_buf *toma_srv_proc_buf;
	int rv = -1;

	NFIN;

#if NVMEIBS_TOMA_DEBUG
	_NI(nvmeibs_toma_server_proc_recv_i1, "Dump toma message of length @SZ bytes:", len);
	_Dbuf(buf, len);
#endif

	/* checks */
	if (len != sizeof(struct nvmeibs_toma_server_proc_buf)) {
		_NE(error_toma_nvmeibs_toma_server_proc_recv, "Invalid data length (@LEN_LONG) expected (@SIZEOF_LONG)", len, sizeof(struct nvmeibs_toma_server_proc_buf));
		rv = -EINVAL;
		goto out;
	}

	toma_srv_proc_buf = (struct nvmeibs_toma_server_proc_buf *)buf;
	switch (toma_srv_proc_buf->type) {
	case NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_REQ:
		if ((rv = process_disk_segment_lock_gid_req(
			&toma_srv_proc_buf->lock_gid_req)) < 0)
			goto out;
		break;

	case NVMEIBS_TOMA_LOGIN:
		if ((rv = on_toma_login()) < 0)
			goto out;
		break;

	case NVMEIBS_TOMA_LOGOUT:
		toma_logout();
		break;

	case NVMEIBS_TOMA_CLIENT_DISCONNECT_FORCE_CMD:
		if ((rv = process_client_disconnect_force_cmd(
			&toma_srv_proc_buf->client_disconnect_force_cmd)) < 0)
			goto out;
		break;
	case NVMEIBS_TOMA_WRITE_STATUS_RESP:
		if (toma_srv_proc_buf->status_resp_msg.handle < NVMEIBS_TOMA_STATUS_AUTO_MAX) {
			struct toma_status_req_data *req_data =
				&toma->stat_req_data[toma_srv_proc_buf->status_resp_msg.handle];
			req_data->len = toma_srv_proc_buf->status_resp_msg.length;
			/* read req_data->handle_req under handle_req_lock to ensure we complete the correct request */
			spin_lock(&req_data->handle_req_lock);
			if (toma_srv_proc_buf->status_resp_msg.handle_req != req_data->handle_req) {
				spin_unlock(&req_data->handle_req_lock);
				_NE(error_2_toma_nvmeibs_toma_server_proc_recv_invalid_handle_req, "Invalid handle_req @INT for status response msg..timed out(?)",
					toma_srv_proc_buf->status_resp_msg.handle_req);
				rv = -EINVAL;
				goto out;
			} else {
				/* we under a spinlock so no new waiters can be added - we don't care if the prev/current is still waiting */
				complete(&req_data->write_comp);
				spin_unlock(&req_data->handle_req_lock);
			}
			rv = 0;
		} else {
			_NE(error_1_toma_nvmeibs_toma_server_proc_recv, "Invalid handle @HANDLE_INT for status response msg",
			   toma_srv_proc_buf->status_resp_msg.handle);
			rv = -EINVAL;
		}
		break;
	case NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED_ACK: {
		if ((rv = __handle_toma_blkset_recovered_ack(
			     toma_srv_proc_buf->blkset_recovered_ack_msg.cookie,
			     toma_srv_proc_buf->blkset_recovered_ack_msg.toma_rv)))
			goto out;
	} break;
	case NVMEIBS_TOMA_JOURNAL_INFO:
		_NT(trace_toma_nvmeibs_toma_server_proc_recv, "Received notification about journal info");
		if ((rv = process_journal_info(&toma_srv_proc_buf->journal_msg)) < 0)
			goto out;
		break;
	case NVMEIBS_TOMA_CLEAN_JOURNAL_FOR_DISK_RANGE:
		_NT(trace_1_toma_nvmeibs_toma_server_proc_recv, "Received clean journal msg");
		if ((rv = process_clean_journal_msg(&toma_srv_proc_buf->clean_journal_msg)) < 0)
			goto out;
		break;
	default:
		_NE(error_2_toma_nvmeibs_toma_server_proc_recv, "Invalid toma:srv msg type @TOMA_SRV_PROC_BUF_TYPE", toma_srv_proc_buf->type);
		rv = -EINVAL;
		goto out;
	}

	rv = len;

out:
	_ND(trace_2_toma_nvmeibs_toma_server_proc_recv, "rv is @RV and len is @LEN_LONG", rv, len);
	NFOUT;
	return rv;
}

/**
 * Report client-disconnect event to Toma.
 * This function must be called: (a) while protecting @cl object
 * (from ib_port workq or under nvmeibs_client_lock lock).
 */
int nvmeibs_toma_report_event_client_disconnect(struct nvmeibs_client *cl)
{
	struct nvmeibs_toma_server_proc_buf proc_buf;
	struct msg_vec msg_vec[CLIENT_DISCONNECT_MSG_VEC_COUNT];
	int vec_cnt = 0;
	int rv = -EINVAL;

	NFIN;

	_ND(trace_toma_nvmeibs_toma_report_event_client_disconnect, "cl @CL_NAME, cid=@CID", cl->name, cl->cid);

	prepare_client_disconnect_msg(cl, &proc_buf, msg_vec, &vec_cnt);

	if ((rv = nvmeibs_toma_server_proc_send_vec_event(
		msg_vec, vec_cnt)) < 0) {
		_NE(error_toma_nvmeibs_toma_report_event_client_disconnect, "Fail to send client-disconnect event to toma (@CL_NAME)", cl->name);
	}

	NFOUT;
	return rv;
}

int nvmeibs_toma_report_event_disk_change(char *disk_id, unsigned long long n_blocks,
										  unsigned int block_size, unsigned int max_request_size,
										  unsigned int seq, unsigned int nsid,
										  const char *dev_name, unsigned int metadata,
										  const char *status, unsigned int vendor, char op, char *model_str, char *native_serial_str)
{
	struct nvmeibs_toma_server_proc_buf proc_buf;
	int rv = -1;

	NFIN;

	_NI(trace_toma_nvmeibs_toma_report_event_disk_change, "Report disk-change id @DISK_ID_STR op=@OP_CHR", disk_id, op);

	/* check if called originated from nvmeibs_remove(),
	   i.e. after nvmeibs_exit() which removed the toma */
	if (!toma)
		goto out;

	memset(&proc_buf, 0, sizeof(proc_buf));
	proc_buf.zero = 0; /* server event must use 0 (see common/nvmeib_shared.h) */
	proc_buf.type = NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE;
	memcpy(proc_buf.disk_change_msg.disk_id, disk_id,
		   sizeof(proc_buf.disk_change_msg.disk_id));
	proc_buf.disk_change_msg.n_blocks = n_blocks;
	proc_buf.disk_change_msg.block_size = block_size;
	proc_buf.disk_change_msg.max_request_size = max_request_size;
	proc_buf.disk_change_msg.seq = seq;
	proc_buf.disk_change_msg.nsid = nsid;
	snprintf(proc_buf.disk_change_msg.dev_name, sizeof(proc_buf.disk_change_msg.dev_name), "/dev/%s", dev_name);
	proc_buf.disk_change_msg.metadata = metadata;
	memcpy(proc_buf.disk_change_msg.status, status,
		   min(sizeof(proc_buf.disk_change_msg.status), strlen(status)));
	proc_buf.disk_change_msg.op = op;
	proc_buf.disk_change_msg.vendor_id = vendor;
	memcpy(proc_buf.disk_change_msg.model_str, model_str, sizeof(proc_buf.disk_change_msg.model_str));
	memcpy(proc_buf.disk_change_msg.native_serial_str, native_serial_str, sizeof(proc_buf.disk_change_msg.native_serial_str));

	if ((rv = nvmeibs_toma_server_proc_send_event(&proc_buf)) < 0) {
		_NE(error_toma_nvmeibs_toma_report_event_disk_change, "Fail to send disk-down event to toma (@DISK_ID_STR)", disk_id);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_toma_report_event_serjio_disk_range_cleaned(void *arg, const char *seg_id)
{
	struct nvmeibs_toma_server_proc_buf proc_buf;
	int rv = -1;
	(void)arg;
	NFIN;

	_ND(trace_toma_nvmeibs_toma_report_event_serjio_disk_range_cleaned, "Report disk_range_cleaned seg=@SEG", seg_id);

	if (!toma)
		goto out;

	memset(&proc_buf, 0, sizeof(proc_buf));
	proc_buf.zero = 0; /* server event must use 0 (see common/nvmeib_shared.h) */
	proc_buf.type = NVMEIBS_TOMA_REPORT_EVENT_SERJIO_RANGE_CLEANED;
	memcpy(proc_buf.serjio_range_cleaned_msg.seg_id, seg_id,
		   sizeof(proc_buf.serjio_range_cleaned_msg.seg_id));

	if ((rv = nvmeibs_toma_server_proc_send_event(&proc_buf)) < 0) {
		_NE(error_toma_nvmeibs_toma_report_event_serjio_disk_range_cleaned, "Fail to send range_cleaned event to toma (@SEG_ID)", seg_id);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_toma_report_event_serjio_request_jgc(const char *seg_id, char *disk_id_str)
{
	struct nvmeibs_toma_server_proc_buf proc_buf;
	int rv = -1;

	NFIN;

	_ND(trace_toma_nvmeibs_toma_report_event_serjio_request_jgc, "Report SERJIO JGC Request seg_id @SEG_ID", seg_id);

	/* check if called originated from nvmeibs_remove(),
	   i.e. after nvmeibs_exit() which removed the toma */

	if (!toma) {
		_NT(trace_1_toma_nvmeibs_toma_report_event_serjio_request_jgc, "SERJIO JGC Request - TOMA not connected");
		goto out;
	}

	memset(&proc_buf, 0, sizeof(proc_buf));
	proc_buf.zero = 0; /* server event must use 0 (see common/nvmeib_shared.h) */
	proc_buf.type = NVMEIBS_TOMA_TRIGGER_JGC;
	memcpy(proc_buf.trigger_JGC_cmd.disk_segment_urn_uuid_str, seg_id, NVMEIB_GID_STR_MAX);
	memcpy(proc_buf.trigger_JGC_cmd.disk_id_str, disk_id_str, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);

	_NT(trace_2_toma_nvmeibs_toma_report_event_serjio_request_jgc, "SERJIO JGC Request - Segment: @SEG_ID", seg_id);

	if ((rv = nvmeibs_toma_server_proc_send_event(&proc_buf)) < 0) {
		_NE(error_toma_nvmeibs_toma_report_event_serjio_request_jgc, "Fail to send SERJIO JGC Request to toma (@SEG_ID)", seg_id);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_toma_report_event_port_gid_change(struct nvmeibs_ib_port *ib_port)
{
	struct nvmeibs_toma_server_proc_buf proc_buf;
	int rv = -1;

	NFIN;

	_NT(trace_toma_nvmeibs_toma_report_event_port_gid_change, "Report GID-change Device @IB_DEV_NAME Port @PORT GID @RAW_IPV6", P2IB(ib_port)->name,
		ib_port->port, &ib_port->gid.gid.raw);

	/* check if called originated from nvmeibs_remove(),
	   i.e. after nvmeibs_exit() which removed the toma */
	if (!toma)
		goto out;

	memset(&proc_buf, 0, sizeof(proc_buf));
	proc_buf.zero = 0; /* server event must set 0 (see common/nvmeib_shared.h) */
	proc_buf.type = NVMEIBS_TOMA_REPORT_EVENT_PORT_GID_CHANGE;
	strncpy(proc_buf.port_gid_change_msg.ib_dev, P2IB(ib_port)->name, NVMEIB_IB_DEVICE_NAME_MAX);
	proc_buf.port_gid_change_msg.port = ib_port->port;
	strncpy(proc_buf.port_gid_change_msg.gid_str, ib_port->gid.gid_str, NVMEIB_GID_STR_MAX);

	if ((rv = nvmeibs_toma_server_proc_send_event(&proc_buf)) < 0) {
		_NE(error_toma_nvmeibs_toma_report_event_port_gid_change, "Fail to send gid-change event to toma (ib_dev @IB_DEV_NAME)", P2IB(ib_port)->name);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_toma_report_event_nic_change(struct nvmeibs_dev *nis_dev, bool add)
{
	struct nvmeibs_toma_server_proc_buf proc_buf;
	int rv = -1;

	NFIN;

	_NT(trace_toma_nvmeibs_toma_report_event_nic_change, "Report @OP_STR Device @IB_DEV_NAME", (add ? "Add" : "Remove"), N2IB(nis_dev)->name);

	/* check if called originated from nvmeibs_remove(),
	   i.e. after nvmeibs_exit() which removed the toma */
	if (!toma)
		goto out;

	memset(&proc_buf, 0, sizeof(proc_buf));
	proc_buf.zero = 0; /* server event must set 0 (see common/nvmeib_shared.h) */
	proc_buf.type = NVMEIBS_TOMA_REPORT_EVENT_NIC_CHANGE;
	strncpy(proc_buf.nic_change_msg.ib_dev, N2IB(nis_dev)->name, NVMEIB_IB_DEVICE_NAME_MAX);
	proc_buf.nic_change_msg.add = add;

	if ((rv = nvmeibs_toma_server_proc_send_event(&proc_buf)) < 0) {
		_NE(error_toma_nvmeibs_toma_report_event_nic_change, "Fail to send nic-change event to toma (ib_dev @IB_DEV_NAME)", N2IB(nis_dev)->name);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_toma_report_event_blkset_recovered(struct nvmeibs_disk_info *di, const char *ds_uuid, u64 blkset_num,
	u64 blkset_slba, u64 pre_recov_lock_val, struct nvmeibs_async_cookie_params *cookie_params)
{
	struct nvmeibs_toma_server_proc_buf proc_buf;
	int rv = -1;

	NFIN;

	/* Disk info is needed only for simulator.
	 * This is because in simulator multiple tomas run in same code,
	 * so it does not really know what server a toma belogs to unless
	 * explicitly told, and this is what di does.
	 * Here, this argument can be ignored. */
	(void)di;

	_NT(trace_toma_nvmeibs_toma_report_event_blkset_recovered, "Report send blkset-recovered event to toma "
	   "(ds=@DS_UUID_UUID, blkset-num=@NUM_LLONG)", ds_uuid, blkset_num);

	/* check if called originated from nvmeibs_remove(),
	   i.e. after nvmeibs_exit() which removed the toma */
	if (!toma)
		goto out;

	memset(&proc_buf, 0, sizeof(proc_buf));
	proc_buf.zero = 0; /* server event must set 0 (see common/nvmeib_shared.h) */
	proc_buf.type = NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED;
	memcpy(proc_buf.blkset_recovered_msg.disk_segment_urn_uuid_str, ds_uuid,
		   sizeof(proc_buf.blkset_recovered_msg.disk_segment_urn_uuid_str));
	proc_buf.blkset_recovered_msg.blkset_no = blkset_num;
	proc_buf.blkset_recovered_msg.blkset_slba = blkset_slba;
	proc_buf.blkset_recovered_msg.pre_recov_lock_val = pre_recov_lock_val;

	rv = nvmeibs_async_cookie_store_add_cookie(cookie_params, GFP_KERNEL, &proc_buf.blkset_recovered_msg.cookie);
	if (rv) {
		_NT(error_toma_nvmeibs_toma_report_event_blkset_recovered,
		    "Cookie add error: @RV", rv);
		goto out;
	} else {
		_NT(trace_1_toma_nvmeibs_toma_report_event_blkset_recovered,
		    "Cookie added: @COOKIE", proc_buf.blkset_recovered_msg.cookie);
	} /* From this point, if there is an error - we must PULL the cookie */

	if ((rv = nvmeibs_toma_server_proc_send_event(&proc_buf))) {
		_NE(error_1_toma_nvmeibs_toma_report_event_blkset_recovered, "Fail to send blkset-recovered event to toma "
		   "(ds=@DS_UUID_UUID, blkset-num=@NUM_LLONG, lid=@LID_LLONG)", ds_uuid, blkset_num, pre_recov_lock_val);
		/* In case of synchronous failure - put the cookie immediatelly.
		   Problem here is that there is no pointer to store. We can either
		   obtain it via cid or save it from prev steps totally breaking layers
		   interface. I chose to obtain it from cid. Just reusing completion.
		   Meh, OK. */
		__handle_toma_blkset_recovered_ack(proc_buf.blkset_recovered_msg.cookie, rv);
	}

	if (!rv) rv = NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY;

out:
	NFOUT;
	return rv;
}

struct corecomm_inj_delayed_toma_send_event {
	struct delayed_work work;
	struct nvmeibs_toma_server_proc_buf buf;
};
int nvmeibs_toma_server_proc_send_(struct nvmeibs_toma_server_proc_buf *, struct msgloop_procfs_ent *);
static void corecomm_inj_delayed_toma_send_event_work(struct work_struct *w) {
	struct corecomm_inj_delayed_toma_send_event *evt = container_of(w, struct corecomm_inj_delayed_toma_send_event, work.work);
	BUG_ON(!toma || !toma->server_proc_events);
	nvmeibs_toma_server_proc_send_(&evt->buf, toma->server_proc_events);
	kfree(evt);
}

/**
 * Send buffer to toma server's proc file.
 *
 */
int nvmeibs_toma_server_proc_send_(struct nvmeibs_toma_server_proc_buf *buf,
								   struct msgloop_procfs_ent *srv_proc)
{
	int rv = -EINVAL;

	NFIN;

	corecomm_inj_code(rv = 0);
	if (corecomm_inj_var(NULL, int, corecomm_inj_toma_do_send_event, 1)) {
		/* forward the toma request */
		if ((rv = nvmeib_msgloop_send(srv_proc, (char *)buf, sizeof(*buf))))
			_NE(error_toma_nvmeibs_toma_server_proc_send,
			    "Failed to send toma message, error @RV", rv);
	} else {
		corecomm_inj_code(({
			if (buf->type == NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED) {
				struct nvmeibs_toma_server_proc_buf proc_buf = {
					.type = NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED_ACK,
					.blkset_recovered_ack_msg.cookie = buf->blkset_recovered_msg.cookie,
				};
				nvmeibs_toma_server_proc_recv(NULL, (char *)&proc_buf, sizeof(proc_buf), NULL);
			}
		}));
	}

	NFOUT;
	return rv;
}

static int nvmeibs_toma_server_proc_send_vec_(struct msg_vec *vec, int cnt,
									   struct msgloop_procfs_ent *srv_proc)
{
	int rv = -EINVAL;

	NFIN;

	/* checks */
	if (!cnt) {
		_NE(error_toma_nvmeibs_toma_server_proc_send_vec, "msg vector with zero cnt");
		goto out;
	}

	if (vec[0].len < sizeof(struct nvmeibs_toma_server_proc_buf)) {
		_NE(error_1_toma_nvmeibs_toma_server_proc_send_vec, "Invalid data length (@LEN_LONG) ", vec[0].len);
		goto out;
	}

	/* forward the toma request */
	if ((rv = nvmeib_msgloop_sendv(srv_proc, vec, cnt)))
		_NE(error_2_toma_nvmeibs_toma_server_proc_send_vec, "Failed to send toma message, error @RV", rv);

out:
	NFOUT;
	return rv;
}

int nvmeibs_toma_server_proc_send(struct nvmeibs_toma_server_proc_buf *buf)
{
	int rv;

	NFIN;
	BUG_ON(!toma || !toma->server_proc);
	rv = nvmeibs_toma_server_proc_send_(buf, toma->server_proc);
	NFOUT;

	return rv;
}

int nvmeibs_toma_server_proc_send_vec(struct msg_vec *vec, int cnt)
{
	int rv;

	NFIN;
	BUG_ON(!toma || !toma->server_proc);
	rv = nvmeibs_toma_server_proc_send_vec_(vec, cnt, toma->server_proc);
	NFOUT;

	return rv;
}

int nvmeibs_toma_server_proc_send_event(struct nvmeibs_toma_server_proc_buf *buf)
{
	int rv;

	if (corecomm_inj_var(NULL, u64, corecomm_inj_toma_send_event_delay, 0)) {
		struct corecomm_inj_delayed_toma_send_event *evt = kzalloc(sizeof(*evt), GFP_KERNEL);
		if (!evt) return -ENOMEM;
		evt->buf = *buf;
		INIT_DELAYED_WORK(&evt->work, corecomm_inj_delayed_toma_send_event_work);
		schedule_delayed_work(
		    &evt->work,
		    corecomm_inj_var(NULL, u64, corecomm_inj_toma_send_event_delay, 0));
		return 0;
	}

	NFIN;
	BUG_ON(!toma || !toma->server_proc_events);
	rv = nvmeibs_toma_server_proc_send_(buf, toma->server_proc_events);
	NFOUT;

	return rv;
}

int nvmeibs_toma_server_proc_send_vec_event(struct msg_vec *vec, int cnt)
{
	int rv;

	NFIN;
	BUG_ON(!toma || !toma->server_proc_events);
	rv = nvmeibs_toma_server_proc_send_vec_(vec, cnt, toma->server_proc_events);
	NFOUT;

	return rv;
}

static const char *toma_stat_proc_fname[] = {
	"all",
	"raft",
	"dseg",
	"bdev",
	"disk",
	"rdma",
	"recover",
	"cfg",
	"topo",
	"leader",
	"local_disk",
	"mem",
	"all.json",
	"nm.json",
	"kafka.txt",
};

static int toma_req_status_mmap_fault(void *arg, unsigned long pg_offset, struct page **page)
{
	int rv = 0;
	struct toma_status_req_data *req_data = arg;

	NFIN;
	if (!toma || toma->connection_status == TOMA_STATUS_DISSCONNECTED) {
		_NE(error_toma_toma_req_status_mmap_fault, "TOMA is not connected");
		rv = -ENOTCONN;
		goto out;
	}
	if (pg_offset >= TOMA_STATUS_MAX_PAGES) {
		_NE(error_1_toma_toma_req_status_mmap_fault, "mmap requested pg-offset=@OFFSET_LONG (max=@MAX)",
		   pg_offset, TOMA_STATUS_MAX_PAGES);
		rv = -EOVERFLOW;
		goto out;
	}

	if (!req_data->mmap_pages[pg_offset]) {
		if (!(req_data->mmap_pages[pg_offset] =
			(void *)__get_free_page(GFP_KERNEL | __GFP_ZERO))) {
			_NE(error_2_toma_toma_req_status_mmap_fault, "Could not get free page");
			rv = -ENOMEM;
			nvmesh_memmgr_metric_on_alloc_update(toma_mmap_pages, PAGE_SIZE, false /* success */);
			goto out;
		}
		nvmesh_memmgr_metric_on_alloc_update(toma_mmap_pages, PAGE_SIZE, true /* success */);
	}

	*page = virt_to_page(req_data->mmap_pages[pg_offset]);

out:

	NFOUT;
	return rv;
}

static void toma_req_status_last_mmap(void *arg)
{
	int i;
	struct toma_status_req_data *req_data = arg;

	NFIN;
	if (!toma) {
		_NE(error_toma_toma_req_status_last_mmap, "TOMA is not connected");
		return;
	}
	/* Free pages */
	for (i = 0; i < TOMA_STATUS_MAX_PAGES; i++) {
		if (req_data->mmap_pages[i]) {
			free_page((unsigned long)req_data->mmap_pages[i]);
			req_data->mmap_pages[i] = NULL;
			nvmesh_memmgr_metric_on_free_update(toma_mmap_pages, PAGE_SIZE);
		}
	}
	nvmeib_public_mmap_remove(req_data->mmap);
	if (atomic_dec_return(&toma->stat_file_count) == 0) {
		complete(&toma->remove_stat_comp);
	}
}

static ssize_t toma_req_status_mmap_read(void *arg, char __user *userbuf,
				  size_t len, loff_t *offset_p, struct file *file)
{
	return seq_read(file, userbuf, len, offset_p);
}

static void destroy_toma_stat_proc_files(struct nvmeibs_toma *t)
{
	int i, stat_file_count;
	atomic_inc(&t->stat_file_count);
	for (i = 0; i < NVMEIBS_TOMA_STATUS_AUTO_MAX; i++) {
		struct toma_status_req_data *req_data = &t->stat_req_data[i];
		if (req_data->mmap) {
			nvmeib_public_mmap_remove_ent(req_data->mmap);
			complete(&req_data->write_comp);
			nvmeib_public_mmap_release(req_data->mmap);
		}
	}
	if ((stat_file_count = atomic_dec_return(&t->stat_file_count)) > 0) {
		_NT(trace_toma_destroy_toma_stat_proc_files, "Waiting for @STAT_FILE_COUNT TOMA status /proc files to be released", stat_file_count);
		if (wait_for_completion_interruptible_timeout(&t->remove_stat_comp, 2 * HZ)) {
			_NW(warn_toma_destroy_toma_stat_proc_files, "Timeout or interruption waiting for TOMA status /proc files to be removed");
		}
	}
	remove_proc_entry(TOMA_STATUS_PROC_DIR, t->stat_parent_dir);
}

static int toma_req_status_mmap_show(struct seq_file *m, void *v)
{
	struct toma_status_req_data *req_data = (struct toma_status_req_data *)m->private;
	struct nvmeibs_toma_server_proc_buf proc_buf = { { .zero = 0, .type = NVMEIBS_TOMA_WRITE_STATUS_REQ } };
	int i;
	bool unlock_mutex = false;
	ssize_t rv = 0;
	static atomic_t handle_counter = ATOMIC_INIT(0);

	if (!toma || toma->connection_status == TOMA_STATUS_DISSCONNECTED) {
		rv = -ENOTCONN;
		goto out;
	}

	mutex_lock(&req_data->write_mutex);
	unlock_mutex = true;
	proc_buf.status_req_msg.handle = req_data->type;
	proc_buf.status_req_msg.type = req_data->type;
	proc_buf.status_req_msg.max_length = (TOMA_STATUS_MAX_PAGES << PAGE_SHIFT);
	strlcpy(proc_buf.status_req_msg.fname, toma_stat_proc_fname[req_data->type], sizeof(proc_buf.status_req_msg.fname));
	proc_buf.status_req_msg.handle_req = atomic_add_return(1, &handle_counter);
	spin_lock(&req_data->handle_req_lock);
	req_data->handle_req = proc_buf.status_req_msg.handle_req;
	spin_unlock(&req_data->handle_req_lock);
	nvmeib_reinit_completion(&req_data->write_comp);

	if ((rv = nvmeibs_toma_server_proc_send_event(&proc_buf)) < 0) {
		rv = -EFAULT;
		goto out;
	}
	if (wait_for_completion_interruptible_timeout(&req_data->write_comp, TOMA_STATUS_RESP_TIMEOUT) <= 0) {
		rv = -ETIMEDOUT;
		goto out;
	}

	for (i = 0; i < TOMA_STATUS_MAX_PAGES; i++) {
		void *page = req_data->mmap_pages[i];
		size_t to_write = PAGE_SIZE;
		if (!page)
			break;
		if (req_data->len <= i * PAGE_SIZE)
			break;
		if (req_data->len < (i + 1) * PAGE_SIZE)
			to_write = req_data->len - (i * PAGE_SIZE);
		seq_write(m, page, to_write);
	}

out:
	if (unlock_mutex)
		mutex_unlock(&req_data->write_mutex);
	return rv;
}

static int toma_req_status_mmap_open(struct inode *inode, struct file *file, void *arg)
{
	return single_open(file, toma_req_status_mmap_show, arg);
}

static int toma_req_status_mmap_release(struct inode *inode, struct file *file)
{
	return single_release(inode, file);
}

static int create_toma_stat_proc_files(struct proc_dir_entry *dir, struct nvmeibs_toma *t)
{
	int rv = 0, i;

	NFIN;
	atomic_set(&t->stat_file_count, 0);
	init_completion(&t->remove_stat_comp);

	t->stat_parent_dir = dir;
	if (!(t->stat_dir = proc_mkdir(TOMA_STATUS_PROC_DIR, t->stat_parent_dir))) {
		_NE(error_toma_create_toma_stat_proc_files, "Fail to create toma status /proc dir @DIR_NAME", TOMA_STATUS_PROC_DIR);
		goto err;
	}

	for (i = 0; i < NVMEIBS_TOMA_STATUS_AUTO_MAX; i++) {
		struct toma_status_req_data *req_data = &t->stat_req_data[i];
		req_data->type = i;
		init_completion(&req_data->write_comp);
		mutex_init(&req_data->write_mutex);
		spin_lock_init(&req_data->handle_req_lock);
		atomic_inc(&t->stat_file_count);
		if (!(req_data->mmap = nvmeib_public_mmap_create((char*)toma_stat_proc_fname[i], t->stat_dir,
								&toma_req_status_mmap_fault, req_data,
								&toma_req_status_last_mmap,
								&toma_req_status_mmap_read,
								&toma_req_status_mmap_open,
								&toma_req_status_mmap_release,
								0644))) {
			_NE(error_1_toma_create_toma_stat_proc_files, "nvmeib_public_mmap_create failed for @DIR_NAME@FILE_NAME", TOMA_STATUS_PROC_PATH, toma_stat_proc_fname[i]);
			atomic_dec(&t->stat_file_count);
			goto err;
		}
	}
	goto out;

err:
	destroy_toma_stat_proc_files(t);

out:
	NFOUT;
	return rv;
}

/* -------------------------------------------------------------------------- */
/*                                                                            */
/*            	       nvmeibs_toma class                                     */
/*                                                                            */
/* -------------------------------------------------------------------------- */

int nvmeibs_toma_create(struct proc_dir_entry *dir, void *arg)
{
	int rv = -1;
	/* start with a temp pointer and only arm global variable when done */
	struct nvmeibs_toma *ttoma = NULL;

	NFIN;

	if (toma) {
		_NE(error_toma_nvmeibs_toma_create, "nvmeibs toma already exist");
		goto out;
	}

	if (!(ttoma = kzalloc(sizeof(*ttoma), GFP_KERNEL))) {
		_NE(error_1_toma_nvmeibs_toma_create, "Fail to allocate nvmeibs toma");
		rv = -ENOMEM;
		goto out;
	}

	mutex_init(&ttoma->wr_guard);
	spin_lock_init(&ttoma->spinlock);
	hash_init(ttoma->conn_hash);
	ttoma->task = NULL; /*we don't know the toma pid yet*/
	ttoma->connection_status = TOMA_STATUS_DISSCONNECTED;

	if (!(ttoma->clients_proc =
		  nvmeib_msgloop_create("toma_clients",
							dir, &nvmeibs_toma_client_proc_recv, NULL, NULL, arg))) {
		_NE(error_2_toma_nvmeibs_toma_create, "Fail to create toma:clients /proc file");
		goto err;
	}
	if (!(ttoma->server_proc =
		  nvmeib_msgloop_create("toma_server",
								dir, &nvmeibs_toma_server_proc_recv,
			nvmeibs_toma_server_proc_open, nvmeibs_toma_server_proc_close,
			arg))) {
		_NE(error_3_toma_nvmeibs_toma_create, "Fail to create toma:server /proc file");
		goto err;
	}
	if (!(ttoma->server_proc_events =
		  nvmeib_msgloop_create("toma_server_events",
								dir, NULL, NULL, NULL, arg))) {
		_NE(error_4_toma_nvmeibs_toma_create, "Fail to create toma:server events /proc file");
		goto err;
	}

	create_toma_stat_proc_files(dir, ttoma);

	_NT(trace_toma_nvmeibs_toma_create, "Toma interface created successfully");
	rv = 0;
	/* Now arm the global variable */
	toma = ttoma;
	goto out;

err:
	_NE(error_5_toma_nvmeibs_toma_create, "Fail to create toma interface");
	if (ttoma) {
		if (ttoma->server_proc_events)
			nvmeib_msgloop_remove(ttoma->server_proc_events);
		if (ttoma->server_proc) nvmeib_msgloop_remove(ttoma->server_proc);
		if (ttoma->clients_proc) nvmeib_msgloop_remove(ttoma->clients_proc);
		kfree(ttoma);
		ttoma = NULL;
	}

out:

	NFOUT;
	return rv;
}

static void toma_conn_hash_flush_nolock(void)
{
	int bucket;
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct nvmeibs_toma_connection_hash_entry *h_curr;

	NFIN;

	__hash_for_each_safe__(toma->conn_hash, bucket, t_node, h_node, h_curr,
						   hlist_next) {
		_NE(toma_conn_hash_flush_nolock_e1, "Found toma connection hash entry (handle 0x@_X) while removing "
		   "toma", h_curr->ib_handle);
		hash_del(&h_curr->hlist_next);
		kfree(h_curr);
	}

	NFOUT;
}

void nvmeibs_toma_remove(void)
{
	NFIN;
	if (toma) {
		_NT(trace_toma_nvmeibs_toma_remove, "Removing nvmeibs_toma (procs and per client hash table)");
		/*have to wait for toma to shut down before removin procs*/
		//nvmeibs_toma_shut_down();
		destroy_toma_stat_proc_files(toma);
		if (toma->server_proc_events)
			nvmeib_msgloop_remove(toma->server_proc_events);
		if (toma->server_proc) nvmeib_msgloop_remove(toma->server_proc);
		if (toma->clients_proc) nvmeib_msgloop_remove(toma->clients_proc);
		toma_conn_hash_flush_nolock();

		kfree(toma);
		toma = NULL;
	}

	NFOUT;
	return;
}
