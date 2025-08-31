/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#include "nvmeibc_ib_nordda_channel.h"
#include "nvmeibc_locks_channel.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_block.h"
#include "nvmeibc_main.h"
#include "nvmeib_srq.h"
#include "nvmeibc_jam.h"
#include "module/instance/nvmeibc_cinst_params.h"
#include "core/nvmeibc_core_common.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeib_completion_noise.h"
#include "nvmeib_public.h"
/* All the code in this file does not compile in simulator,
   except some explicitly marked chunks that are shared. */

#define NORDDA_RELEASE_ON_THREAD 1
#define USE_PRIV_SRQ 0

#define get_ch_ind(ch) ch->base.index

extern bool nr_defer_recv_comps;
extern bool nr_defer_recv_comps_tcp;
extern bool nr_shared_cq;
extern bool nr_shared_cq_tcp;
extern bool nr_use_srq;
extern bool nr_use_srq_tcp;

bool nr_defer_recv_comps_use_kwq = false;
module_param(nr_defer_recv_comps_use_kwq, bool, 0644);
MODULE_PARM_DESC(nr_defer_recv_comps_use_kwq, "Determines whether to use a kernel workqueue for deferred receive completions on nordda channels.");

bool nvmeibc_nordda_wq_unbound = false;
module_param_named(nordda_wq_unbound, nvmeibc_nordda_wq_unbound, bool, 0444);
MODULE_PARM_DESC(nordda_wq_unbound, "Determines whether to use an unbound kernel workqueue for nvmeibc_nordda (true) or a bound one (false). Relevant only if nr_defer_recv_comps_use_kwq is set to true");

uint nvmeibc_nr_max_wrs_per_req = 0;
module_param_named(nr_max_wrs_per_req, nvmeibc_nr_max_wrs_per_req, int, 0644);
MODULE_PARM_DESC(nr_max_wrs_per_req, "The maximum number of WRs (RDMA work requests) per IO channel request, used for a write operation. For 0, use system's default.");

NVMEIBC_MEMMGR_METRIC(c_nordda_srq_info, "component=client.nordda.srq_info");

/* Kernel workqueue for nordda channel operations */
static struct workqueue_struct *nvmeibc_nordda_wq;

int nvmeibc_nordda_channel_wq_init(void)
{
	unsigned int flags = WQ_MEM_RECLAIM | WQ_SYSFS;
	NFIN;
	if (nvmeibc_nordda_wq_unbound)
		flags |= WQ_UNBOUND;
	nvmeibc_nordda_wq = alloc_workqueue("nvmeibc_nordda", flags, 0);
	if (!nvmeibc_nordda_wq) {
		_NE(error_nvmeibc_nordda_channel_wq_init, "Failed to allocate nordda channel workqueue");
		NFOUT;
		return -ENOMEM;
	}
	_NT(trace_nvmeibc_nordda_channel_wq_init, "Created nordda channel workqueue @PTR", nvmeibc_nordda_wq);
	NFOUT;
	return 0;
}

void nvmeibc_nordda_channel_wq_destroy(void)
{
	NFIN;
	if (nvmeibc_nordda_wq) {
		_ND(trace_nvmeibc_nordda_channel_wq_destroy, "Destroying nordda channel workqueue @PTR", nvmeibc_nordda_wq);
		destroy_workqueue(nvmeibc_nordda_wq);
		nvmeibc_nordda_wq = NULL;
	}
	NFOUT;
}

struct workqueue_struct *nvmeibc_nordda_channel_get_wq(void)
{
	return nvmeibc_nordda_wq;
}
EXPORT_SYMBOL(nvmeibc_nordda_channel_get_wq);

static int nordda_pending_io(struct nvmeibc_disk *disk,
	struct nvmeibc_channel *ch, void *context, bool sp_locked, u64 version);
static void nordda_pending_io_pcpu_func(void *ctx);
static int nordda_execute_io(struct nvmeibc_channel *ch,
	struct nvmeibc_disk *disk, struct nvmeibc_disk_command *disk_cmd,
	void *context);

static int nordda_channel_execute_ka(struct nvmeibc_channel *ch);

static int nordda_send_completion(struct nvmeibc_ib_net *net, struct ib_wc *wc,
	bool last_in_series);
static int nordda_recv_completion(struct nvmeibc_ib_net *net,
	struct ib_wc *wc);
static void on_disconnect_nordda_ch(struct nvmeibc_ib_net *net);

static void process_rsp_finalize(struct nvmeibc_ib_nordda_channel *ch,
								 struct nvmeibc_volume_req_info *req);

static const char * disk_cmd_to_str(struct nvmeibc_disk_command *disk_cmd)
{
	struct nvmeibc_disk_io_command *block_cmd;
	struct nvmeibc_disk_gen_cmd *gen_cmd;
	if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
		block_cmd = disk_to_block(disk_cmd);
		switch (block_cmd->reqs[0].op) {
		case NVMEIB_BLOCK_IO_OP_READ:
			return "R";
		case NVMEIB_BLOCK_IO_OP_WRITE:
			return "W";
		case NVMEIB_BLOCK_IO_OP_DISCARD:
			return "T";
		default:
			return "IO-???";
		}
	}
	else if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_GEN) {
		gen_cmd = disk_to_gen(disk_cmd);
		switch (gen_cmd->opcode) {
		case NVMEIB_GEN_OP_BLKSET_RECOVERED:
			return "G-BLKSET_RECOVERED";
		case NVMEIB_GEN_OP_GET_UUID_JOUR:
			return "G-GET_UUID_JOUR";
		case NVMEIB_GEN_OP_GET_EC_DB:
			return "G-GET-EC-DB";
		case NVMEIB_GEN_OP_FREE_JRNL_ENTS:
			return "G-FREE_JRNL_ENTS";
		case NVMEIB_GEN_OP_LOCK:
			return "G_LOCK";
		case NVMEIB_GEN_OP_GET_JMDC:
			return "G-GET_JMDC";
		case NVMEIB_GEN_OP_JENTRY_ERASE:
			return "G-JENTRY-ERASE";

		default:
			return "G-???";
		}
	}
	else if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK)
		return "SW-L";
	else {
#if defined(NVMEIBC_DISK_CMDS_STATS) && (NVMEIBC_DISK_CMDS_STATS==1)
		struct nvmeibc_disk_command_stats_only *lock_cmd = (void *)disk_cmd;

		BUILD_BUG_ON(offsetof(struct nvmeibc_disk_command_stats_only, cmd_type) !=
					 offsetof(struct nvmeibc_disk_command, cmd_type) ||
					 offsetof(struct nvmeibc_disk_command_stats_only, cmd_type) != 0);

		if (lock_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK_HW_OWNER)
			return "HW-L-OWNER";
		else if (lock_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK_HW_WBINFO)
			return "HW-L-WR-BINFO";
		else if (lock_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK_HW_READ)
			return "HW-L-READ";
		else
#endif
		return "?";
	}
}

/* inuse flag not protected by lock */
void nvmeibc_ib_nordda_channel_use(struct nvmeibc_ib_nordda_channel *ch)
{
	NFIN;
	_ND(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_use, "use nrch @BASE_NAME (@CH_PTR)", ch->base.name, ch);
	nvmeibc_channel_reset(&ch->base);
	bitmap_zero(ch->req_in_use, NVMEIB_MAX_NORDDA_IO_REQ);
	ch->inuse = true;
	nvmeibc_nr_lat_meas_clear_nrch_pcpu_data(ch->per_cpu_lat_data);
	NFOUT;
}

void nvmeibc_ib_nordda_channel_end_use(struct nvmeibc_ib_nordda_channel *ch)
{
	NFIN;
	_ND(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_end_use, "end use nrch @BASE_NAME (@CH_PTR)", ch->base.name, ch);
	ch->inuse = false;
	NFOUT;
}

bool nvmeibc_ib_nordda_channel_is_used(struct nvmeibc_ib_nordda_channel *ch)
{
	return ch->inuse;
}

void __attribute__((unused)) nvmeibc_ib_nordda_channel_init_stats(
	struct nvmeibc_ib_nordda_channel *ch)
{
	int i;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&ch->guard, flags);
	for (i = 0; i < ch->base.disk->nrch_ioreq_num; ++i)
		nvmeib_stats_init(&ch->reqs[i].send.common);
	spin_unlock_irqrestore(&ch->guard, flags);
	NFOUT;
}

void __attribute__((unused)) nvmeibc_ib_nordda_channel_print_stats(
	struct nvmeibc_ib_nordda_channel *ch,
	const char *str)
{
	int i;
	u64 avg;
	struct nvmeib_stats *st;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&ch->guard, flags);
	for (i = 0; i < ch->base.disk->nrch_ioreq_num; ++i) {
		st = &ch->reqs[i].send.common;
		if (st->counts) {
			avg = DIV_ROUND_CLOSEST(nvmeib_stats_sum_dt_ns(st), st->counts);
			_ND(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_print_stats, "!!!!! req(@SEQ)@@BASE_NAME-@STR: send n=@COUNTS, io_avg=@IO_AVG",
				i, ch->base.name, str, st->counts, avg);
		}
	}
	spin_unlock_irqrestore(&ch->guard, flags);
	NFOUT;
}

static void nordda_handle_error(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeibc_volume_req_info *req)
{
	struct nvmeibc_disk_command *disk_cmd = req->req.dcmd;
	unsigned long now_jif = jiffies;

	NFIN;
	_NT(trace_ib_nordda_channel_nordda_handle_error, "nrch @BASE_NAME, req @IDX, disk_cmd=@DISK_CMD",
		ch->base.name, req->idx, disk_cmd);
	if (disk_cmd) {
		_NT(trace_1_ib_nordda_channel_nordda_handle_error, "A non-direct IO request failed: op @DISK_CMD_TO_STR, disk_cmd @DISK_CMD, , rtt=@SECONDS (@JIFFIES jiffies);"
		   "Completion to block layer will occur on ch disconnect",
	  disk_cmd_to_str(disk_cmd), disk_cmd, (now_jif - disk_cmd->post_jif) / HZ, now_jif - disk_cmd->post_jif);
#if 0
		/* firstly complete the block layer I/O command */
		nvmeibc_ib_net_complete_iocmd(&ch->net.base, &req->req, -EIO);

		/* free the request */
		nvmeibc_ib_net_free_req(&ch->net.base, &req->req);
#else

		/* Dont complete the block layer I/O command here but from
		   remove-work which should have already been added by caller, or will.
		   Otherwise, if cmd was a READ, its rdma-write may occur after block
		   will free request's memory. For WRITE do same to reduce logic-paths.
		 */
#endif
	}
	NFOUT;
}

static void nordda_handle_qp_err(struct ib_wc *wc,
	bool send_err, struct nvmeibc_ib_nordda_channel *ch,
	enum nvmeib_wr_opcode op_code, int index)
{
	enum ib_wc_status wc_status = wc->status;

	NFIN;
	_NT(trace_ib_nordda_channel_nordda_handle_qp_err, "status=@STATUS_STR, op_code=@OP_CODE", nvmeib_status_str(&wc_status), op_code);
	if ((u32)op_code == NVMEIB_LOCAL_INV_WR_ID) {
		_NT(trace_1_ib_nordda_channel_nordda_handle_qp_err, "LOCAL_INV failed with status @WC_STATUS", wc_status);
		if (wc_status == IB_WC_MW_BIND_ERR) {
			/* Tell pool to set MR as having a bind error */
			nvmeib_fast_reg_pool_handle_bind_err(
				P2NV(ch->net.base.port)->fr_pool, wc->ex.invalidate_rkey);
		}
	}
	else if ((u32)op_code == NVMEIB_FAST_REG_WR_ID)
		_NT(trace_2_ib_nordda_channel_nordda_handle_qp_err, "FAST_REG_MR failed status @WC_STATUS", wc_status);
	else if ((u32)op_code == NVMEIB_RDMA_IO_KA) {
		_NT(trace_3_ib_nordda_channel_nordda_handle_qp_err, "Keep-Alive Failed on Channel @BASE_NAME with status @WC_STATUS", ch->base.name, wc_status);
	}
	else {
		if (nordda_wr_opcode_is_gen_cmd(op_code) ||
			op_code == NVMEIB_SEND_IO ||
			(op_code >= NVMEIB_RDMA_MID_0 && op_code <= NVMEIB_RDMA_MID_MAX) ||
			op_code == NVMEIB_RDMA_LAST ||
			op_code == NVMEIB_RDMA_METADATA) {
			if ((unsigned)index < ch->base.disk->nrch_ioreq_num)
				nordda_handle_error(ch, &ch->reqs[index]);
			else
				_NT(trace_4_ib_nordda_channel_nordda_handle_qp_err, "A non-direct IO request failed AND "
				   "the iu index was invalid @INDEX", index);
		}
		else
			_NT(trace_5_ib_nordda_channel_nordda_handle_qp_err, "failed @OP_STR status @WC_STATUS", send_err ? "send" : "receive",
				wc_status);
	}
	/* If transport error, disconnect all io-channels on this path */
	if (send_err && (wc_status == IB_WC_RETRY_EXC_ERR ||
					wc_status == IB_WC_RESP_TIMEOUT_ERR ||
					wc_status == IB_WC_FATAL_ERR)) {
		_NT(error_ib_nordda_channel_nordda_handle_qp_err, "Detected Transport Error on IO Path (@SGID -> @DGID). Disconnecting all io-channels",
			&ch->net.base.path.sgid, &ch->net.base.path.dgid);
			nvmeibc_disk_disconnect_io_path(ch->base.disk, &ch->lionic->ib_gid,
											&ch->lionic->rionic->ib_gid);
	}
	NFOUT;
}

/* ************************************************************************** */
/* Watchdog                                                                   */
/* ************************************************************************** */

static bool on_start_wd_event(void *cntx)
{
	struct nvmeibc_volume_req_info *req = cntx;
	//struct nvmeibc_ib_nordda_channel *ch = req->nrch;
	bool rv = true;

	NFIN;
	/* no need for irq_save because the WD thread already disabled the irq */
	ri_spin_lock(req);
	//rv = atomic_read(&ch->net.base.dying) == 0;
	NFOUT;
	return rv;
}

enum nr_wd_type {
	NR_WD_NONE,
	NR_WD_QP_TIMEOUT_SINCE_SEND, 		//Time btw Last-Recv --> Send      is More than QP-Timeout --> Now
	NR_WD_QP_TIMEOUT_SINCE_LAST_RECV, 	//Time btw Send      --> Last-Recv is More than QP-Timeout --> Now
	NR_WD_CHANNEL_TIMEOUT_SINCE_SEND, 	//Time btw Send      --> Last-Recv is Less than QP-Timeout but More than CH-Timeout since Send --> This is an issue of this particular cmd...
};

static inline const char *nr_wd_type_to_str(int t)
{
	switch (t) {
	case NR_WD_NONE	                        : return "NONE";
	case NR_WD_QP_TIMEOUT_SINCE_SEND		: return "QP_TIMEOUT_SINCE_SEND";
	case NR_WD_QP_TIMEOUT_SINCE_LAST_RECV	: return "QP_TIMEOUT_SINCE_LAST_RECV";
	case NR_WD_CHANNEL_TIMEOUT_SINCE_SEND	: return "CHANNEL_TIMEOUT_SINCE_SEND";
	default: return "???";
	}
}

ulong nvmeibc_nr_wd_long_timeout = 0;
module_param_named(nr_wd_long_timeout, nvmeibc_nr_wd_long_timeout, ulong, 0644);
MODULE_PARM_DESC(nr_wd_long_timeout, "IO watchdog timeout in jiffies.");

ulong nvmeibc_nr_wd_rescue_timeout = 0; //(NVMEIBC_MIN_IO_TIMEOUT * HZ) / 2;
module_param_named(nr_wd_rescue_timeout, nvmeibc_nr_wd_rescue_timeout, ulong, 0644);
MODULE_PARM_DESC(nr_wd_rescue_timeout, "IO watchdog rescue timeout in jiffies. An IO watchdog rescue is an attempt to handle any missed receive interrupts even though there was no interrupt. This functionality was an escape and is considered unnecessary. A value under 1 second disables this functionality.");

static int handle_watchdog_event_nordda(void *cntx, unsigned long time_passed)
{
	struct nvmeibc_volume_req_info *req = cntx;
	struct nvmeibc_ib_nordda_channel *ch = req->nrch;
	struct nvmeibc_disk *disk = ch->base.disk;
	bool is_pausing, is_dying;
	int rv = 0;
	int trace_output = 0;
	unsigned long last_received;
	unsigned long from_last_received;
	bool timeout;
	enum nr_wd_type timeout_type;
	unsigned long nrch_timeout =
		nvmeibc_nr_wd_long_timeout ? : NVMEIBC_IO_LONG_TIMEOUT;

	NFIN;
	is_pausing = atomic_read(&disk->paused);
	is_dying = atomic_read(&disk->dying);
	/* make sure the interrupt did not arrive while we were bla-bla here */
	if (!req->req.dcmd) {
		_ND(trace_3_ib_nordda_channel_handle_watchdog_event, "IO cmd reply arrived while WD handler called req=@REQ", req);
		goto out;
	}
	else if (req->req.dcmd->cmd_type == NVMEIBC_DISK_CMD_IO)
		_ND(trace_4_ib_nordda_channel_handle_watchdog_event, "req->req.iocmd->req_id=@REQ_ID_LLONG", req->req.bcmd->req_id);
	else if (req->req.dcmd->cmd_type == NVMEIBC_DISK_CMD_GEN)
		_ND(trace_5_ib_nordda_channel_handle_watchdog_event, "GEN request");
	else if (req->req.dcmd->cmd_type == NVMEIBC_DISK_CMD_LOCK)
		_ND(trace_6_ib_nordda_channel_handle_watchdog_event, "LOCK request");
	else
		_ND(trace_7_ib_nordda_channel_handle_watchdog_event, "Unhandled cmd-type of request @INT", req->req.dcmd->cmd_type);


	if (nvmeibc_nr_wd_rescue_timeout &&
		time_passed > nvmeibc_nr_wd_rescue_timeout) {
		int rv_ext = nveibc_ib_net_defer_recv_interrupts_external(&ch->net.base);
		if (rv_ext == 0 || (rv_ext != -ENOTSUPP &&
							rv_ext != -EALREADY &&
							rv_ext != -ENOENT &&
							rv_ext != -EBUSY)) {
			/* poller thread may NOT be running now either due to:
			   1. real bug where we missed events and did not schedule thread
			   2. cqe was not written yet to recv-CQ or worse
			   3. corresponding sqe is still in sendq
			*/
			_NT(trace_8_ib_nordda_channel_handle_watchdog_event,
				"WD: nordda ch=@BASE_NAME, req=@REQ, net=@NET, qp=@QP, qp_num=@QP_NUM, remote_qpn=@REMOTE_QPN"
				"since wd-start time-passed @LD(@LD) [jif(sec)] "
				"timeout @WD_TIMEOUT_JIF(@WD_TIMEOUT_JIF) [jif(sec)], "
				"n_wd_events=@INT, req.n_comps={s=@LLU, r=@LLU}, rv_ext=@INT",
				ch->base.name, req, &ch->net.base, ch->net.base.qp,
				ch->net.base.qp->qp_num, ch->net.base.remote_qpn,
				time_passed, time_passed / HZ,
				ch->net.base.wd_timeout_jif, ch->net.base.wd_timeout_jif / HZ,
				req->n_wd_events, req->n_send_comp, req->n_recv_comp, rv_ext);

			req->n_wd_events++; /* guarded by ri lock */
			nvmeibc_disk_counters_inc(ch->base.disk, n_err_nrch_wd_rescue);
		}
		//WARN_ON_ONCE(1);
	}

	/* the time difference from the last received on the channel and now */
	last_received = atomic64_read(&ch->last_received);
	from_last_received = jiffies - last_received;
	/* we differentiate between network timeouts and disk timeouts.
	   network timeouts are consider as a whole for the channel.
	   disk timeouts are per request.
	   when do we call a timeout:
	   1. if our send left after the last receive we wait the short" time which
	      is the QP property and if we pass it if break.
	   2. if we got other requests' receives on the QP and these receives
	      arrived after our send we may assume that there is no networking issue
	      and then we wait for a long timeout for the request to finish and only
		  if we pass that timeout we break.
	*/
	timeout = false;
	timeout_type = NR_WD_NONE;
	if (req->wdc.called_on > last_received) {
		/* our sent came after the last_received on the QP -
		   it is a networking WD and we break after the QP timeout.
		*/
		if (time_passed > ch->net.base.wd_timeout_jif) {
			timeout = true;
			timeout_type = NR_WD_QP_TIMEOUT_SINCE_SEND;
		}
	}
	else {
		/* the last received arrived before the QP timeout -
		   it is a network WD and we break after the QP timeout.
		*/
		if (from_last_received > ch->net.base.wd_timeout_jif) {
			timeout = true;
			timeout_type = NR_WD_QP_TIMEOUT_SINCE_LAST_RECV;
		}
	}

	/* consider channel's timeout or ch-timeout < qp-timeout */
	if (!timeout && time_passed > nrch_timeout) {
		/* network is alive but the request was finished after the disk WD -
		   it is a disk issue and we break.
		   It is an issue of this particular req
		*/
		timeout = true;
		timeout_type = NR_WD_CHANNEL_TIMEOUT_SINCE_SEND;
	}
/*
	timeout = time_passed < from_last_recived ?
		(time_passed > ch->net.base.wd_timeout_jif) :
		(from_last_recived > ch->net.base.wd_timeout_jif ||
		 time_passed > nrch_timeout);
*/
	if (timeout || is_pausing || is_dying) {
		u64 last_watchdog_warning = atomic64_read(&ch->last_watchdog_warning);
		if (last_watchdog_warning + HZ < jiffies) {
			atomic64_cmpxchg(&ch->last_watchdog_warning, last_watchdog_warning, jiffies);
			trace_output = 1;
		}
		//EXC-1795: WD will disarm wdc as rv is 0.
		//atomic_set(&req->wdc.watchdog_armed, 0);
		if (trace_output) {
			_NE(trace_10_ib_nordda_channel_handle_watchdog_event,
				"WD event: nordda ch=@BASE_NAME, req=@REQ, "
				"timeout=@BOOL, disk={paused=@BOOL, dying=@BOOL}, "
				"timeout_type=@STR(@INT), "
				"time-passed @LD(@LD) jiffies(sec), "
				"last_received @LD(@LD), "
				"from_last_received  @FROM_LAST_RECEIVED(@FROM_LAST_RECEIVED), "
				"timeout @WD_TIMEOUT_JIF(@WD_TIMEOUT_JIF) jiffies(sec), "
				"req.n_comps={s=@LLU, r=@LLU}, "
				"net.n_intrs={s=@LLU, r=@LLU}",
				ch->base.name, req,
				timeout, is_pausing, is_dying,
				nr_wd_type_to_str(timeout_type), timeout_type,
				time_passed, time_passed / HZ,
				last_received, last_received / HZ,
				from_last_received, from_last_received / HZ,
				ch->net.base.wd_timeout_jif, ch->net.base.wd_timeout_jif / HZ,
				req->n_send_comp, req->n_recv_comp,
				ch->net.base.n_send_intrs, ch->net.base.n_recv_intrs);
		}
#if 0		/* Do after req lock has been unlocked to prevent deadlock */
		/* start with disconnecting the net */
		nvmeibc_ib_net_disconnect(&ch->net.base);
#endif
		req->wd_timeout_occurred = true;
        /* and then finish the command */
		nordda_handle_error(ch, req);
	}
	else
		rv = 1;

out:
	NFOUT;
	return rv;
}

static void on_end_wd_event(void *cntx)
{
	struct nvmeibc_volume_req_info *req = cntx;
	struct nvmeibc_ib_nordda_channel *nrch = req->nrch;
	bool wd_timeout_occurred = req->wd_timeout_occurred;
	//struct nvmeibc_ib_nordda_channel *ch = req->nrch;

	NFIN;
	req->wd_timeout_occurred = false;
	ri_spin_unlock(req);
	if (wd_timeout_occurred)
		nvmeibc_ib_net_disconnect(&nrch->net.base);

	NFOUT;
}


static void init_req_wd(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeibc_volume_req_info *req)
{
	NFIN;
	req->wdc.wd = ch->base.wd;
	req->wdc.cntx = req;
	req->wdc.on_start = on_start_wd_event;
	req->wdc.process = handle_watchdog_event_nordda;
	req->wdc.on_end = on_end_wd_event;
	nvmeib_wd_init_wdc(&req->wdc);
	nvmeib_wd_add_wdc(&req->wdc);
	NFOUT;
}

/* ************************************************************************** */
/* Create                                                                     */
/* ************************************************************************** */
struct nvmeibc_ib_nordda_channel *nvmeibc_ib_nordda_channel_create(
	const struct nvmeibc_cinst_params_core *p, struct nvmeibc_io_lnic *lionic,
	int qpn, int lionic_index, int rionic_index)
{
	struct nvmeibc_ib_nordda_channel *ch = lionic->nr_channels + qpn;
	struct nvmeibc_ib_admin_channel *ach = ac_to_iac(lionic->rionic->ch);
	int rv = -1;
	NFIN;

	if (nvmeibc_channel_init(&ch->base, p) < 0) {
		_NE(error_ib_nordda_channel_nvmeibc_ib_nordda_channel_create, "Fail to init base-channel");
		goto out;
	}
	ch->base.ct = ct_n_rdda;

	/* SRQ */
	if (ch->priv_srq) {
		_NE(error_2_ib_nordda_channel_nvmeibc_ib_nordda_channel_create, "old priv-srq");
		goto out;
	}
	if ((p->max_nic_srqs > 1) && USE_PRIV_SRQ) {
		struct nvmeib_srq_params params = {
			.q_size = NVMEIB_NORDDA_SRQ_MAX_SIZE,
			.msg_size = NVMEIBS_NORDDA_SERVER_MSG_SIZE
		};
		if (!(ch->priv_srq =
			  nvmeib_srq_info_create(P2NV(lionic->port), &params, ch, c_nordda_srq_info)))
			_NT(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_create, "Fail to create private srq, fallback to dev's SRQ-pool");
	}

	ch->base.disk = ach->base.base.disk;
	ch->base.disk_version = nvmeibc_disk_version_get(ch->base.disk);
	snprintf(ch->base.name, sizeof(ch->base.name),
		"%.*s-%.*s~NR.L%04dR%04dC%04d",
		(int)sizeof(ach->base.base.rhost_name), ach->base.base.rhost_name,
		(int)sizeof(ch->base.disk->name)      , ch->base.disk->name,
		lionic_index, rionic_index, qpn);

	nvmeibc_channel_init_dbgdi_uniq(&ch->base, p, lionic_index, rionic_index, qpn);

	spin_lock_init(&ch->guard);
	INIT_LIST_HEAD(&ch->available_link);
	INIT_LIST_HEAD(&ch->free_reqs);
	init_completion(&ch->init_comp);
	ch->inuse = false;
	get_ch_ind(ch) = qpn;
	ch->lionic = lionic;

	/* cross-ref from net to admin and nordda ch */
	ch->net.base.admin_ch = &ach->base;
	ch->net.base.ioch = &ch->base;
	ch->net.base.rearm_send_cq = false;

	/* set the virtual io functions */
	ch->base.execute_pending_io = nordda_pending_io;
	ch->base.execute_io = nordda_execute_io;
	ch->base.execute_ka = nordda_channel_execute_ka;
	ch->priority.raw = lionic->rionic->priority.raw;

	if (NVMEIBC_NR_LAT_MEAS) {
		/* allocate the per-cpu latency data */
		if (!(ch->per_cpu_lat_data = nvmeib_public_alloc_percpu_cacheline(struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data)))
		goto out;
	}

	pcpu_nrch_cpu_clear(ch);
	INIT_WORK(&ch->pcpu_connect_work, nvmeibc_disk_connect_nrch_pcpu_work);
	init_completion(&ch->pcpu_connect_comp);

	ch->wait_release_zero_before_cb = (P2NV(lionic->port)->dev_type != DT_siw && nvmeibc_iommu_enabled) ||
		(P2NV(lionic->port)->dev_type == DT_siw && NVMEIB_SIW_NRCH_WAIT_RLS_ZERO_BEFORE_CB);

	_ND(trace_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_create, "Created nrch @BASE_NAME (@CH_PTR), qp @INDEX", ch->base.name, ch, get_ch_ind(ch));
	rv = 0;

out:
	NFOUT;
	return !rv ? ch : NULL;
}

/* ************************************************************************** */
/* IO Requests                                                                */
/* ************************************************************************** */
/* called during channel initailization --> remove-work cant run during */
int nvmeibc_ib_nordda_channel_init_reqs(struct nvmeibc_ib_nordda_channel *ch,
	struct volume_server_config_alloc_nr_net_rsp *a_nr_net_rsp)
{
	struct nvmeibc_ib_net_nordda *nr_net = &ch->net;
	struct nvmeibc_ib_net *net = &ch->net.base;
	struct nvmeibc_disk *disk = ch->base.disk;
	struct volume_server_client_req_io_area *ioa;
	int i = 0, rv = -1;
	u32 md_size;
	u32 exp_len;
	int rn_pages_0 = 0, rn_pages;
	NFIN;

	if (disk->md_size) {
		/* journal-md-cache remote access info */
		nr_net->jmdc_rai.raddr = be64_to_cpu(a_nr_net_rsp->jmdc_desc.raddr);
		nr_net->jmdc_rai.len = be32_to_cpu(a_nr_net_rsp->jmdc_desc.len);
		nr_net->jmdc_rai.rkey = be32_to_cpu(a_nr_net_rsp->jmdc_desc.rkey);
		exp_len = disk->jour.max_rng_blk *
			sizeof(union jblock_md);
		if (nr_net->jmdc_rai.len != exp_len) {
			_NE(error_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "Unexpected journal-md-cache len (@LEN, exp @EXP_LEN)",
			   nr_net->jmdc_rai.len, exp_len);
			goto out;
		}
	}

	if (!(ch->reqs = kzalloc((sizeof(*ch->reqs) * ch->base.disk->nrch_ioreq_num), GFP_KERNEL))) {
		_NE(error_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "Failed to allocate requests");
		goto out;
	}

	INIT_LIST_HEAD(&ch->free_reqs);
	for (i = 0; i < ch->base.disk->nrch_ioreq_num; ++i) {
		ioa = &a_nr_net_rsp->a[i];
		rn_pages = be32_to_cpu(ioa->rn_pages);
		if (rn_pages <= 0) {
			_NE(nvmeibc_ib_nordda_channel_init_reqs_e1,
				"Invalid rn_pages value @INT", rn_pages);
			goto err;
		}
		if (i == 0) {
			rn_pages_0 = rn_pages;
		} else if (rn_pages != rn_pages_0) {
			_NE(nvmeibc_ib_nordda_channel_init_reqs_e2,
				"Invalid rn_pages value @INT for request @INT", rn_pages, i);
			goto err;
		}
		if (nvmeibc_ib_net_alloc_volume_req(
			net, &ch->reqs[i].req, i) < 0) {
			_NE(error_2_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "Failed to allocate volume request @SEQ", i);
			goto err;
		}
		ch->reqs[i].nrch = ch;
		ch->reqs[i].raddr = be64_to_cpu(ioa->raddr);
		ch->reqs[i].rn_pages = rn_pages;
		ch->reqs[i].rkey = be32_to_cpu(ioa->rkey);
		/* metadata */
		ch->reqs[i].req.md.remote.addr = be64_to_cpu(ioa->md_desc.raddr);
		ch->reqs[i].req.md.remote.size = be32_to_cpu(ioa->md_desc.size);
		ch->reqs[i].req.md.remote.rkey = be32_to_cpu(ioa->md_desc.rkey);
		md_size = NVMEIBC_D2MD_LEN(ch->reqs[i].rn_pages * ch->base.disk->info->cntr_page_size,
			ch->base.disk);
		if (md_size > ch->reqs[i].req.md.remote.size) {
			_NE(error_3_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "Invalid md size for @RN_PAGES pages (@SIZE, exp @MD_SIZE), i=@SEQ, "
			   "disk={sector-shift=@SHIFT, md-size=@SIZE}",
			   ch->reqs[i].rn_pages, ch->reqs[i].req.md.remote.size, md_size, i,
			   ch->base.disk->sector_shift, ch->base.disk->md_size);
			goto err_vol_req;
		}
		else
			_ND(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "remote[@SEQ]: addr=@ADDR, size=@SIZE, rkey=@RKEY",
				i, ch->reqs[i].req.md.remote.addr,
				ch->reqs[i].req.md.remote.size, ch->reqs[i].req.md.remote.rkey);
		ch->reqs[i].idx = i;
		init_req_wd(ch, &ch->reqs[i]);
		_ND(trace_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "ioa=@IOA, req=@REQ, cmd=@CMD_PTR, raddr=@RADDR, rn_pages=@RN_PAGES, rkey=@RKEY",
			i, &ch->reqs[i].req, ch->reqs[i].req.cmd, ch->reqs[i].raddr,
			ch->reqs[i].rn_pages, ch->reqs[i].rkey);

		/* JMDC piggyback */
		if (nvmeibc_ib_net_jmdc_pb_map(P2NV(nr_net->base.port),
									 &ch->reqs[i].jmdc_pb,
									 ch->base.disk->jour.rng_binje) < 0) {
			_NE(error_4_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "Failed to map jmdc-pb, @SEQ", i);
			goto err_vol_req;
		}
		spin_lock_init(&ch->reqs[i].lock);
		ch->reqs[i].locking_pid = -1;

		/* Used for reqs on lock-less per-cpu channels that need to schedule pending IO on other CPUs */
		memset(&ch->reqs[i].pcpu_pending_io_smp_call, 0, sizeof(ch->reqs[i].pcpu_pending_io_smp_call));
		ch->reqs[i].pcpu_pending_io_smp_call.call_data.func = nordda_pending_io_pcpu_func;
		ch->reqs[i].pcpu_pending_io_smp_call.call_data.info = &ch->reqs[i];

		list_add_tail(&ch->reqs[i].req.link, &ch->free_reqs);
	}
	atomic_set(&ch->base.n_user_reqs, 1);
	ch->base.reqs_comp = NULL;

	rv = 0;
	goto out;

	//omril: can we do all undo code from nordda_channel_free_volume_reqs()?
	//by changing the loop size + cond (per req) to know if und o is needed
err_vol_req:
	_NT(trace_2_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "Free volume req @SEQ", i);
	nvmeibc_ib_net_free_volume_req(net, &ch->reqs[i].req);

err:
	 while (--i >= 0) {
		_NT(trace_3_ib_nordda_channel_nvmeibc_ib_nordda_channel_init_reqs, "Free req @SEQ", i);
		nvmeibc_ib_net_jmdc_pb_unmap(P2NV(nr_net->base.port), &ch->reqs[i].jmdc_pb);
		nvmeib_wd_remove_wdc(&ch->reqs[i].wdc);
		nvmeibc_ib_net_free_volume_req(net, &ch->reqs[i].req);
	}

	INIT_LIST_HEAD(&ch->free_reqs);
	kfree(ch->reqs);
	ch->reqs = NULL;

out:
	NFOUT;
	return rv;
}

unsigned nr_max_used_reqs_per_channel = 64;
module_param(nr_max_used_reqs_per_channel, uint, 0644);
MODULE_PARM_DESC(nr_max_used_reqs_per_channel, "Maximum number of requests issued simultaneously on a channel.");

static struct nvmeibc_volume_req_info *get_req_info(
	struct nvmeibc_ib_nordda_channel *ch)
{
	struct nvmeibc_volume_request *req = NULL;
	struct nvmeibc_volume_req_info *ri = NULL;
	unsigned long flags = 0;
	NFIN;

	nrch_guard_spin_lock_irqsave(ch, flags);
	if ((ch->n_used_reqs < nr_max_used_reqs_per_channel) &&
		(req = list_first_entry_or_null(&ch->free_reqs,
		struct nvmeibc_volume_request, link))) {
		ch->n_used_reqs++;
		ch->n_uses_ever++;
		list_del_init(&req->link);
		ri = r_to_sri(req);
		_ND(trace_ib_nordda_channel_get_req_info, "nrch @BASE_NAME, req @IDX (@REQ)", ch->base.name, ri->idx, req);
	}
	else
		_ND(trace_1_ib_nordda_channel_get_req_info, "nrch @BASE_NAME, no free reqs", ch->base.name);
	nrch_guard_spin_unlock_irqrestore(ch, flags);

	if (ri)
		if (!nvmeibc_channel_try_use_req_info(&ch->base)) {
			_ND(trace_2_ib_nordda_channel_get_req_info, "nrch @BASE_NAME, no usable reqs", ch->base.name);
			ri = NULL; /* remove-work in progress, not putting back */
		}

	NFOUT;
	return ri;
}

static void put_req_info(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeibc_volume_req_info *ri)
{
	unsigned long flags = 0;
	NFIN;

	WARN_ON(ri->req.dcmd != NULL);
	nrch_guard_spin_lock_irqsave(ch, flags);
	ri_spin_lock(ri);
	ri->release_counter = 0;
	ch->n_used_reqs--;
	list_add(&ri->req.link, &ch->free_reqs);
	ri_spin_unlock(ri);
	nrch_guard_spin_unlock_irqrestore(ch, flags);

	/* end-use must be the last thing we do here as afterwords,
	   remove-work may disconnect the channel which will allow
	   freeing the channel or worse, re-using it */
	nvmeibc_channel_end_use_req_info(&ch->base);

	NFOUT;
}

static void nordda_channel_free_volume_reqs(
	struct nvmeibc_ib_nordda_channel *ch)
{
	int i, n_events, n = 0;
	unsigned long time_passed;
	bool linked;
	LIST_HEAD(list);
	NFIN;

	if (ch->reqs) {
		req_reused_bb_lru_flush(&ch->base);
		for (i = 0; i < ch->base.disk->nrch_ioreq_num; ++i) {
			/* peek wdc for deubg */
			time_passed = jiffies - ch->reqs[i].wdc.called_on;
			n_events = ch->reqs[i].wdc.n_events;

			nvmeib_wd_remove_wdc(&ch->reqs[i].wdc);
			/* finish pending block device commands with error code */
			if ((linked = ch->reqs[i].req.dcmd)) {
				struct nvmeibc_disk_command *dcmd = NULL;
				if (!r_to_sri(&ch->reqs[i].req)->release_counter) {
					_NW(warn_ib_nordda_channel_nordda_channel_free_volume_reqs,
						"req w/ incomplete iocmd but release_counter=0");
					WARN_ON_ONCE(1);
				}

				_NT(trace_ib_nordda_channel_nordda_channel_free_volume_reqs,
					"net @BASE, incomplete req[@SEQ], dcmd=@PTR, cmd_type=@STR(@INT), "
					"WD={time_passed=@LD(@LD), n_events=@INT}",
					&ch->net.base, i, ch->reqs[i].req.dcmd,
					disk_cmd_to_str(ch->reqs[i].req.dcmd),
					ch->reqs[i].req.dcmd->cmd_type,
					time_passed, time_passed/HZ, n_events);
				if (ch->reqs[i].req.dcmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
					struct nvmeibc_disk_io_command *bcmd = ch->reqs[i].req.bcmd;
					bool was_reuse;
					dcmd = &bcmd->disk_cmd;
					/* Restore put-aside SG and nmdesc if in the middle of reuse-BB */
					if (ch->reqs[i].reuse_orig.sgcount) {
						REUSE_SG_RESTORE(&(ch->reqs[i]));
						REUSE_FR_RESTORE(&(ch->reqs[i]));
					}
					was_reuse = del_reuse_request((&ch->reqs[i].req), ch->base.disk);
					nvmeibc_ib_net_unmap_data(&ch->net.base, &ch->reqs[i].req);
					nvmeibc_ib_net_unmap_and_unlink_iocmd_reuse(
						&ch->net.base, &ch->reqs[i].req, -EIO, ch->reqs[i].req.sgcount, was_reuse);
				} else if (ch->reqs[i].req.dcmd->cmd_type == NVMEIBC_DISK_CMD_GEN) {
					struct nvmeibc_disk_gen_cmd *gcmd = disk_to_gen(ch->reqs[i].req.dcmd);
					dcmd = &gcmd->disk_cmd;
					nvmeibc_ib_net_unmap_data(&ch->net.base, &ch->reqs[i].req);
					nvmeibc_ib_net_nordda_unmap_and_unlink_gcmd(
						&ch->net, &ch->reqs[i].req, -EIO);
				} else if (ch->reqs[i].req.dcmd->cmd_type == NVMEIBC_DISK_CMD_LOCK) {
					struct nvmeibc_disk_lock_cmd *lcmd = disk_to_lock(ch->reqs[i].req.dcmd);
					dcmd = &lcmd->disk_cmd;
					nvmeibc_ib_net_nordda_unlink_lcmd(
						&ch->net, &ch->reqs[i].req);
				} else
					BUG();

				WARN_ON_ONCE(!list_empty(&dcmd->dcmd_link));
				list_add_tail(&dcmd->dcmd_link, &list);
				n++;
			}

			/* Request was reused, got rcv comp but never got the send */
			if (ch->reqs[i].reuse_orig.sgcount || ch->reqs[i].reuse_orig.nmdesc) {
					_NT(nordda_channel_free_volume_unfree_reuse_req, "@REQ has reuse_orig set - restoring", &ch->reqs[i].req);
					BUG_ON(linked);
					REUSE_SG_RESTORE(&(ch->reqs[i]));
					REUSE_FR_RESTORE(&(ch->reqs[i]));

			}

			/* @nmdesc can be nz if recv-comp arrived, we've completed to ulp
			   but net died before send-comp arrived and as we unmap only after
			   both comps arrived we are left with req that still owns fr-descs.
			   In this case @linked is expected to be false.

			   @n_rdma_iu can be nz if @linked is true here, we unlink it
			   but we dont call nvmeibc_ib_net-unmap_sg_to_ib_sge which reset it
			*/
			if (ch->reqs[i].req.nmdesc || ch->reqs[i].req.cmd->n_rdma_iu) {
				_NT(nordda_channel_free_volume_unfree_req,
					"detected still mapped request @REQ "
					"(nmdesc=@NMDESC, n_rdma_iu=@N_RDMA_IU, was-linked=@BOOL)",
					&ch->reqs[i].req, ch->reqs[i].req.nmdesc,
					ch->reqs[i].req.cmd->n_rdma_iu, linked);
				nvmeibc_ib_net_free_req(&ch->net.base, &ch->reqs[i].req);
			}
			/* and then free the volume request */
			nvmeibc_ib_net_free_volume_req(&ch->net.base, &ch->reqs[i].req);
			/* unmap the jmdc pb source */
			nvmeibc_ib_net_jmdc_pb_unmap(P2NV(ch->net.base.port), &ch->reqs[i].jmdc_pb);
		}

		kfree(ch->reqs);
		ch->reqs = NULL;
		ch->base.reqs_comp = NULL;
		INIT_LIST_HEAD(&ch->free_reqs);
	}

	_NT(trace_nordda_channel_free_volume_reqs,
		"net=@BASE, @INT cmds-comps awaiting srv's approval",
		&ch->net.base, n);
	nvmeibc_disk_ioch_handoff_bailed_cmds(&ch->base, &list);

	NFOUT;
}

/* ************************************************************************** */
/* Connecting                                                                 */
/* ************************************************************************** */

static void register_nrio_vex_ops(struct nvmeibc_ib_net *net)
{
	struct nvmeibc_ib_nordda_channel *nrch = c_to_inrc(net->ioch);
	memcpy(nrch->net.vex_nrio_ops, net->admin_ch->vex_nrio_ops, sizeof(nrch->net.vex_nrio_ops));
}

static int on_login_nordda_ch(struct nvmeibc_ib_net *net,
	struct nvmeibs_login_response *lrsp, int n_ext)
{
	int rv = 0;
	NFIN;

	if (nvmeib_wire_op_cid_get_rsp_opcode(&lrsp->base.op_cid) == NVMEIB_LOGIN_RSP) {
		struct nvmeibc_ib_nordda_channel *nrch = c_to_inrc(net->ioch);
		_ND(trace_ib_nordda_channel_on_login_nordda_ch, "Server accepted I/O qp connection");
		nrch->ka_rai.raddr = be64_to_cpu(lrsp->base.nr_rsp.io_ka_raddr);
		nrch->ka_rai.rkey = be32_to_cpu(lrsp->base.nr_rsp.io_ka_rkey);
		nrch->ka_rai.len = sizeof(u64);
		register_nrio_vex_ops(net);
	} else {
		_NW(warn_ib_nordda_channel_on_login_nordda_ch, "Unhandled RSP opcode @OPCODE", nvmeib_wire_op_cid_get_rsp_opcode(&lrsp->base.op_cid));
		rv = -ECONNRESET;
	}

	NFOUT;
	return rv;
}

static int calc_nr_sq_size(struct nvmeibc_ib_nordda_channel *ch, int n_sges)
{
	struct nvmeib_dev *nvdev = P2NV(ch->lionic->port);
	struct nvmeibc_disk *disk = ch->base.disk;
	bool use_fr_fmr = !disk->md_extd && (nvdev->has_fr || nvdev->has_fmr);
	int max_wr_io = NVMEIBC_NR_CH_N_WR_CTRL + NVMEIBC_NR_CH_RDMA_WRITE_JMDC_PB;
	int rv;

	if (use_fr_fmr && !nvmeibc_nr_max_wrs_per_req) {
		max_wr_io += NVMEIBC_NR_CH_N_WR_FR + /* The SG list for IO is collapsed into an FR */
		!!(disk->md_size > 0); /* And one for the sep MD (if present) */
	}
	else if (!nvmeibc_nr_max_wrs_per_req) {
		/* We cannot use an FR - Either the NIC doesn't support it or the disk has inline MD */
		/* So we need (max sectors x (1 for no MD, 2 for MD) / n_sges) WRs */
		max_wr_io += DIV_ROUND_UP(
			(disk->max_request_size_bytes >> NVMEIBC_SECTOR_SHIFT) *
			(1 + !!(disk->md_size > 0)), n_sges);
	} else {
		max_wr_io += nvmeibc_nr_max_wrs_per_req + !!(disk->md_size > 0);
	}
	_NT(trace_ib_nordda_channel_calc_nr_sq_size, "nrch @BASE_NAME (@CH_PTR), n_sges=@INT, max_wr_io=@INT",
		ch->base.name, ch, n_sges, max_wr_io);
	rv = max_wr_io * ch->base.disk->nrch_ioreq_num + NVMEIBC_NR_CH_N_WR_IO_KA;
	return rv;
}

static void rc_wq_destroy(struct nvmeibc_ib_nordda_channel *ch)
{
	NFIN;

	/* drain defered recv-comps */
	if (ch->rc_wq) {
		_NT(rc_wq_destroy, "nrch @IOCH_NAME (@CH_PTR), drain & destroy rc-wq pid: @K_PID",
			ch->base.name, ch, wq_pid(ch->rc_wq));
		wq_drain(ch->rc_wq);
		wq_destroy(ch->rc_wq);
		ch->rc_wq = NULL;
	}

	NFOUT;
}

int nvmeibc_ib_nordda_channel_connect(struct nvmeibc_ib_nordda_channel *ch)
{
	struct nvmeibc_ib_admin_channel *ach = ac_to_iac(ch->net.base.admin_ch);
	struct nvmeibc_ib_net_params *params = NULL;
	struct nvmeibc_login_request req = {};
	char sgid_buf[GUID_SIZE] = {0};
	char dgid_buf[GUID_SIZE] = {0};
	int rv = -1;
	int sq_size = calc_nr_sq_size(ch, NVMEIBC_CHANNEL_MAX_MAIN_NORDDA_SG);
	NFIN;

	if (!(params = kzalloc(sizeof(*params), GFP_KERNEL))) {
		_NE(error_ib_nordda_channel_nvmeibc_ib_nordda_channel_connect, "Fail to allocate net params");
		goto out;
	}

	if (!ch->lionic->path_valid) {
		if (nvmeibc_disk_lionic_rionic_find_path(ch->lionic) < 0) {
			_NT(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_connect, "Fail to find path");
			rv = -EHOSTUNREACH; /* for start_ioch_path_error() to return true */
			goto out;
		}
	}

	/* fill net base, required for find-path in net-alloc */
	atomic_set(&ch->net.base.dying, 0);
	ch->net.base.port = ch->lionic->port;
	ch->net.base.path = ch->lionic->path;
	ch->net.base.pkey = ch->lionic->port->pkey;

	ch->net.base.cm_rdma_type = ch->lionic->rdma_type;
	if (ch->lionic->rdma_type == _rdma_ib) {
	_ND(trace_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_connect, "Use (lionic=@LIONIC) path @DGID->@LIONIC_IPV6",
	   &ch->lionic->path.sgid, &ch->lionic->path.dgid, ch->lionic);

		ch->net.base.service_id = NVMEIB_SERVICE_ID;
		ch->net.base.service_port = 0;
		ch->net.base.cm_rdma_type = _rdma_ib;
	}
	else if (ch->lionic->rdma_type == _rdma_roce) {
		ch->net.base.service_id = 0;
		ch->net.base.service_port = NVMEIB_PORT_ID;
		ch->net.base.cm_rdma_type = _rdma_roce;
	}
	else if (ch->lionic->rdma_type == _rdma_iwarp) {
		uint total_offset = 0;

		/* We want to stagger the destination port to ensure a good spread amongst all RX queues for all disks.
		So we need to offset the port by the total offset so far plus the offset for this rionic plus the offset for this channel.
		The assumption is that each disk has the same number of paths and the same number of NRCHs per lionic.

		So for example with 2 lionics, 2 rionics and 4 NRCHs per lionic.
		Disk 0 lionic 0 NRCHs connected to the first rionic will have destination ports 7915 - 7919
		Disk 0 lionic 0 NRCHS connected to the second rionic will have destination ports 7920 - 7923
		Disk 1 lionic 0 NRCHs connected to the first rionic will have destination ports 7924 - 7927
		Disk 1 lionic 0 NRCHs connected to the second rionic will have destination ports 7927 - 7930
		*/

		if (ch->base.disk->create_id) {
			struct nvmeibc_io_rnic *rionic = NULL;
			struct nvmeibc_io_lnic *lionic = NULL;
			uint total_lionic_nr_qps = 0;
			/* Sum n_nr_qps only for (rionic, lionic) paths that contain this lionic */
			list_for_each_entry(rionic, &ch->base.disk->nr_rionics, disk_nrlink) {
				list_for_each_entry(lionic, &rionic->nr_lionics, rionic_nrlink) {
					if (lionic != ch->lionic)
						continue;
					total_lionic_nr_qps += lionic->n_nr_qps;
				}
			}
			/* +8 per disk for this lionic's paths (e.g. 2 rionics * 4 per path) */
			total_offset += total_lionic_nr_qps * ch->base.disk->create_id;
		}

		/* Offset by the number of lionic NRCH QPs for this rionic*/
		total_offset += (ch->lionic->rionic->nr_idx * ch->lionic->n_nr_qps);
		/* Offset by the channel index*/
		total_offset += get_ch_ind(ch);

		/* Add the base port and modulo by the number of TCP ports*/
		ch->net.base.service_id = 0;
		ch->net.base.service_port = ch->lionic->rionic->tcp_base_port + total_offset % ch->lionic->rionic->tcp_num_ports;
		ch->net.base.cm_rdma_type = _rdma_iwarp;
	}

	ch->net.base.lkey = nvmeib_get_lkey(P2NV(ch->lionic->port));
	ch->net.base.rkey = nvmeib_get_rkey(P2NV(ch->lionic->port));

	/* fill params */
	ch->net.base.rearm_send_cq = EC_PERF_CLNT_NORDDA_SHARED_CQ ? false : true;
	params->max_send_q = sq_size;
	params->max_send_sg = NVMEIBC_CHANNEL_MAX_MAIN_NORDDA_SG;
	params->max_recv_q = 0;
	params->max_recv_sg = 0;
	params->max_send_cq = params->max_send_q;
	/* reply to my sends (no pushed msgs from srv) */
	params->max_recv_cq = params->max_send_cq;
	if (nvmeibc_support_srq(P2NV(ch->net.base.port)) &&
		(P2NV(ch->lionic->port)->dev_type == DT_siw ? nr_use_srq_tcp : nr_use_srq)) {
		params->use_srq = true;
		params->srq_priv = ch->priv_srq;
		params->srq_type = ch->priv_srq ? NVMEIB_SRQ_TYPE_INVALID :
			NVMEIB_SRQ_TYPE_SECONDARY;
	}
	params->max_rd_atomic = NVMEIB_DFLT_MAX_READ_ATOM_ON_WIRE;
	params->max_dest_rd_atomic = NVMEIB_DFLT_MAX_READ_ATOM_ON_WIRE;
	params->call_send_comp_handler = nordda_send_completion;
	params->call_receive_comp_handler = nordda_recv_completion;
	params->free_net_on_connect_err = true;
	params->on_connected = NULL;
	params->on_connected_clear = NULL;
	params->on_login = on_login_nordda_ch;
	params->on_disconnect = on_disconnect_nordda_ch;
	params->on_free = NULL;
	params->ch_index = get_ch_ind(ch);
	params->comp_cpu = pcpu_nrch_cpu_get(ch);

	if (!nvmeibc_use_pcpu_cq) {
		params->nr_defer_recv_comps = P2NV(ch->lionic->port)->dev_type == DT_siw ? nr_defer_recv_comps_tcp : nr_defer_recv_comps; //get this from c-disk ?!
		if (params->nr_defer_recv_comps) {
			params->rcq_offload_enb = false;
			if (nr_defer_recv_comps_use_kwq) {
				/* Use kernel workqueue */
				params->defer_recv_intr_kwq = nvmeibc_nordda_channel_get_wq();
				params->defer_recv_intr_wq = NULL;
				if (!params->defer_recv_intr_kwq) {
					_NE(nvmeibc_ib_nordda_channel_connect_e101,
						"Kernel workqueue not available for nordda channel");
					rv = -ENOMEM;
					goto out;
				}
			} else {
				/* Use custom workqueue */
				proc_name_t pname;
				if (params->comp_cpu >= 0)
					clnt_proc_name_format_extd(pname, 'C', "WQ", "NRpc",
								   nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(&ch->base)),
								   params->comp_cpu);
				else
					clnt_proc_name_format(pname, 'C', "WQ", "nr_rc", nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(&ch->base)));
				ch->rc_wq = params->comp_cpu >= 0 ? wq_create_on(pname, params->comp_cpu) : wq_create(pname);
				if (!ch->rc_wq) {
					_NE(nvmeibc_ib_nordda_channel_connect_e100,
						"Failed to create recv comps WQ");
					rv = -ENOMEM;
					goto out;
				}
				_NT(nvmeibc_ib_nordda_channel_connect_rc_wq_pid,
					"NRCH @IOCH_NAME (@CH_PTR) - created c_nr_rc_wq with pid @K_PID",
					ch->base.name, ch, wq_pid(ch->rc_wq));
				params->defer_recv_intr_wq = ch->rc_wq;
				params->defer_recv_intr_kwq = NULL;
			}
		} else {
			params->rcq_offload_enb = true;
			params->rcq_offload_cpu = is_pcpu_nrch(ch) ? pcpu_nrch_cpu_get(ch) : WORK_CPU_UNBOUND;
			params->defer_recv_intr_wq = NULL;
			params->defer_recv_intr_kwq = NULL;
		}
		params->scq_offload_enb = false;
		params->shared_cq = P2NV(ch->lionic->port)->dev_type == DT_siw ? nr_shared_cq_tcp : nr_shared_cq;
	}
	else {
		params->rcq_offload_enb = false;
		params->defer_recv_intr_wq = NULL;
		params->scq_offload_enb = false;
		params->shared_cq = false;
	}

	if (!params->use_srq) {
		/* Running without SRQ - Init Channel RQ */
		if (!(ch->recv_q = kzalloc(sizeof(*ch->recv_q), GFP_KERNEL))) {
			rv = -ENOMEM;
			goto err;
		}

		_NT(trace_2_ib_nordda_channel_nvmeibc_ib_nordda_channel_connect, "Initializing RQ of size @RCV_SIZE for IO channel @CH_PTR",
		   ch->base.disk->nrch_ioreq_num, ch);
		rv = nvmeib_init_recvq(ch->recv_q, P2NV(ch->lionic->port),
			ch->base.disk->nrch_ioreq_num * 2, NVMEIBS_NORDDA_SERVER_MSG_SIZE);

		if (rv < 0) {
			kfree(ch->recv_q);
			goto err;
		}
		params->recv_q = ch->recv_q;
		params->max_recv_q = ch->recv_q->rq_queue_size;
		params->max_recv_sg = NVMEIBC_CHANNEL_MAX_MAIN_NORDDA_SG;
	}

	if (NVMEIBC_NR_LAT_MEAS)
		params->alloc_siw_wc_md = P2NV(ch->net.base.port)->dev_type == DT_siw;

	/* fill login data */
	nvmeibc_login_req_init(&req, ch->base.disk->last_tgt_link_ver,
				NVMEIBC_NORDDA_CHANNEL, ach->base.cid & 0xffffff, false, 0, 0);
	nvmeibc_login_req_set_msg_hdr(&req, NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED,
				      NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED, 0, 0);
	if (!NVMEIB_UPDATE_NW_PATHS) {
		nvmeibc_login_req_set_ioch(&req,
					ch->net.base.path.sgid.global.subnet_prefix,
					ch->net.base.path.sgid.global.interface_id,
					ch->net.base.path.dgid.global.subnet_prefix,
					ch->net.base.path.dgid.global.interface_id,
					ch->base.index,
					0);
	} else {
		nvmeibc_login_req_set_ioch(&req,
					ch->lionic->port->gid.hw_gid.global.subnet_prefix,
					ch->lionic->port->gid.hw_gid.global.interface_id,
					ch->lionic->rionic->hw_gid.global.subnet_prefix,
					ch->lionic->rionic->hw_gid.global.interface_id,
					ch->base.index,
					0);
	}

	format_gid_raw(ch->net.base.path.sgid.raw, sgid_buf);
	format_gid_raw(ch->net.base.path.dgid.raw, dgid_buf);

	_NT(trace_3_ib_nordda_channel_nvmeibc_ib_nordda_channel_connect, "--- Trying to connect nordda ch: cid=@CID_LLONG, s=@SGID_BUF to d=@DGID_BUF, index(qp)@INDEX, comp_cpu=@INT",
		nvmeib_wire_op_cid_get_cid(&req.op_cid), sgid_buf, dgid_buf, get_ch_ind(ch), params->comp_cpu);

	if ((rv = nvmeibc_ib_net_nordda_alloc(&ch->net, params, &req)) < 0) {
		_NT(trace_4_ib_nordda_channel_nvmeibc_ib_nordda_channel_connect, "Failed to connect nordda channel @CH_PTR (net @NET, state @STATE_GUARD)",
			ch, &ch->net, nvmeib_get_state_guard(&ch->net.base.state));
		nvmeibc_ib_nordda_channel_clear_rq(ch, true);
	} else {
		ch->net.nrch = ch;
		goto out;
	}

err:
	if (!nvmeibc_use_pcpu_cq)
		rc_wq_destroy(ch);

out:
	kfree(params);
	NFOUT;
	return rv;
}

/* ************************************************************************** */
/* Data Path                                                                  */
/* ************************************************************************** */
/* -------------------------------------------------------------------------- */
/* Send                                                                       */
/* -------------------------------------------------------------------------- */
/* ch's disk spinlock must be taken by this context so that
   while we get req from ch, it is in the available list.
   (see exception when initializing the channel) */
void *nvmeibc_ib_nordda_channel_get_io_context(
	struct nvmeibc_ib_nordda_channel *ch)
{
	struct nvmeibc_volume_req_info *ri = NULL;
	NFIN;

	if (atomic_read(&ch->base.dying)) {
		_ND(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_get_io_context, "nordda channel is dying");
		goto out;
	}
	if (atomic_read(&ch->net.base.dying)) {
		_ND(trace_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_get_io_context, "nordda channel's net is dying");
		goto out;
	}

	ri = get_req_info(ch);

out:
	NFOUT;
	return ri;
}

/*
   called by:
   execute-io-remote
   nordda-pending-io
*/
static int nordda_execute_io(struct nvmeibc_channel *ch,
	struct nvmeibc_disk *disk, struct nvmeibc_disk_command *disk_cmd,
	void *context)
{
	struct nvmeibc_ib_nordda_channel *nrch = c_to_inrc(ch);
	struct nvmeibc_volume_req_info *info = context;
	int rv;

	if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO)
		nvmeibc_block_cmd_status_debug(disk_to_block(disk_cmd),
			NVMEIBC_BLOCK_CMD_NORDDA_EXECUTE);
	nvmeibc_disk_cmd_status_debug(disk_cmd,
					NVMEIBC_DISK_CMD_NORDDA_EXECUTE);
	if (!(rv = nvmeibc_ib_net_nordda_execute_io(
		&nrch->net, disk, info, disk_cmd))) {
		/* the ri is no longer owned by user, it's in ch's net */
		nvmeibc_channel_end_use_req_info(&nrch->base);
	}

	return rv;
}

//similar to nvmeibc_ib_net-complete_iocmd_block
//but does not use req->dcmd which shall be NULL now
static void nordda_complete_pending_block_cmd(
	struct nvmeibc_disk_io_command *iocmd,
	int comp_code)
{
	struct nvmeibc_d_iocmd_comp *comp = &iocmd->comp;

	NFIN;
	_ND(trace_ib_nordda_channel_nordda_complete_pending_block_cmd, "Complete peding block_command=@BLOCK_COMMAND id=@REQ_ID_LLONG",
		iocmd, iocmd->req_id);
	if (comp_code < 0)
		_NT(trace_1_ib_nordda_channel_nordda_complete_pending_block_cmd, "complete_pending_block_cmd with code @COMP_CODE", comp_code);
	nvmeibc_block_cmd_status_debug(iocmd,
		NVMEIBC_BLOCK_CMD_COMPLETED);

	comp->comp_code = comp_code;
	/* update the block layer */
	nvmeibc_block_completion(comp); //NO-RDDA fail to execute a pending cmd

	NFOUT;
}

//similar to nvmeibc_ib_net_nordda-complete_gen_cmd
//but does not use req->dcmd which shall be NULL now
static void nordda_complete_pending_gen_cmd(
	struct nvmeibc_disk_gen_cmd *gen_cmd, int comp_code)
{
	NFIN;
	_ND(trace_ib_nordda_channel_nordda_complete_pending_gen_cmd, "Complete peding gen cmd=@CMD_PTR id=@REQ_ID_LLONG",
		gen_cmd, gen_cmd->req_id);
	if (comp_code < 0)
		_NT(trace_1_ib_nordda_channel_nordda_complete_pending_gen_cmd, "Complete peding gen cmd with code @COMP_CODE",
		   comp_code);

	nvmeibc_block_cmd_status_debug(iocmd,
		NVMEIBC_BLOCK_CMD_COMPLETED);
	nvmeibc_disk_cmd_status_debug(&gen_cmd->disk_cmd,
			NVMEIBC_DISK_CMD_REMOTE_PENDING_FAILED);

	/* update upper layer */
	nvmeibc_disk_gen_cmd_completion(gen_cmd, comp_code); //Failed execute pending

	NFOUT;
}

static void nordda_pending_fail_cb_work(struct work_struct *work)
{
	struct nvmeibc_disk_command *disk_cmd = container_of(
			work, struct nvmeibc_disk_command, auto_fail_no_rdda_work);
	int comp_code = disk_cmd->cb_comp_code;

	nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_REMOTE_PENDING_FAILED);

	if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
		_NT(trace_3_ib_nordda_channel_nordda_pending_io, "Failed IO on nordda channel");
		nordda_complete_pending_block_cmd(
			disk_to_block(disk_cmd), comp_code);
	}
	else if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_GEN) {
		_NT(trace_4_ib_nordda_channel_nordda_pending_io, "Failed Gen-CMD on nordda channel");
		nordda_complete_pending_gen_cmd(
			disk_to_gen(disk_cmd), comp_code);
	}
	else if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_LOCK) {
		struct nvmeibc_disk_lock_cmd *disk_lock_cmd = disk_to_lock(disk_cmd);
		_NT(trace_5_ib_nordda_channel_nordda_pending_io, "Failed lock cmd opr_ip @INDEX (@OPR_IP) from lock-ch @BASE_NAME (@CH_PTR)",
		   disk_lock_cmd->index, disk_lock_cmd, disk_lock_cmd->ch->base.name, disk_lock_cmd->ch);
		nvmeibc_locks_channel_lock_cmd_completion(
			disk_lock_cmd, comp_code, LOCK_OPR_USING_BYPASS);
	}
	else {
		_NE(error_ib_nordda_channel_nordda_pending_io, "Invalid disk cmd type @CMD_TYPE", disk_cmd->cmd_type);
		WARN_ON(1);
	}
}

/*
 * Try to process a pending io. req-info must already by in use by caller
 *
 * Called by:
 *  start-io-nordda-channels
 *  execute-io-remote
 *  send-completion, if req-info's release-counter == 0
 *  recv-completion, if req-info's release-counter == 0
 */
static int nordda_pending_io(struct nvmeibc_disk *disk,
	struct nvmeibc_channel *ch, void *context, bool sp_locked, u64 version)
{
	struct nvmeibc_ib_nordda_channel *nrch = c_to_inrc(ch);
	struct nvmeibc_volume_req_info *info = context;
	struct nvmeibc_disk_command *disk_cmd;
	int comp_code = 0;
	struct nvmeib_cpu_mask cpu_mask;

	NFINS(ch->name);

	if (is_ll_pcpu_nrch(nrch) && smp_processor_id() != pcpu_nrch_cpu_get(nrch)) {
		_NW(warn_ib_nordda_channel_nordda_pending_io_pcpu_cpu,
		    "wrong CPU (not @INT) for pcpu nrch (@PTR) @BASE_NAME. Scheduling on correct CPU",
		    pcpu_nrch_cpu_get(nrch), nrch, nrch->base.name);
		/* Safe because the request has not yet been returned to the pool by put_req_info */
		if (smp_call_function_single_async(
			pcpu_nrch_cpu_get(nrch), &info->pcpu_pending_io_smp_call.call_data) < 0) {
			_NE(err_ib_nordda_channel_nordda_pending_io_pcpu_cpu,
			    "Failed to reschedule req (@REQ) for pcpu nrch (@PTR) @BASE_NAME on cpu @CPU",
			    info, nrch, nrch->base.name, pcpu_nrch_cpu_get(nrch));
			/* We can't return the request because we're on the wrong CPU so we can either leak or panic. */
			BUG();
		}
		goto out;
	}

try_next:
	BUG_ON(info->release_counter != 0);

	/* Try to reuse req (=context).
	   If no pending disk-cmds, put req back under disk's lock i.e. atomic w/ adding pending cmds */
	disk_cmd = is_pcpu_nrch(nrch) ?
		nvmeibc_disk_pcpu_nrch_pending_cmd_get(disk, nrch, version, context, put_req_info) :
		nvmeibc_disk_get_disk_cmd_nordda(disk, nrch, version, context, put_req_info);

	if (disk_cmd) {
		nvmeib_completion_noise_start(NVMEIB_NOISE_SUBMISSION);
		BUG_ON(!disk_cmd->cpu_mask_info);
		cpu_mask = disk_cmd->cpu_mask_info->mask;
		if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO)
			nvmeibc_block_cmd_status_debug(disk_to_block(disk_cmd),
				NVMEIBC_BLOCK_CMD_NORDDA_EXECUTE);

		nvmeibc_disk_cmds_stats_pending_exec_start(disk, disk_cmd);
		if (!(comp_code = nordda_execute_io(ch, disk, disk_cmd, info))) {
			_ND(trace_2_ib_nordda_channel_nordda_pending_io, "Pending I/O execution success disk_cmd=@DISK_CMD idx=@IDX "
			   "release_counter=@RELEASE_COUNTER", disk_cmd, info->idx,
				info->release_counter);
		}
		else {
			nvmeibc_disk_cmds_stats_pending_exec_err(disk, disk_cmd);
			/* Schedule callback on System WQ */
			INIT_WORK(&disk_cmd->auto_fail_no_rdda_work, nordda_pending_fail_cb_work);
			disk_cmd->cb_comp_code = comp_code;
			schedule_work(&disk_cmd->auto_fail_no_rdda_work);

			_NT(trace_6_ib_nordda_channel_nordda_pending_io, "Try next pending block-cmd");
			nvmeib_completion_noise_end(NVMEIB_NOISE_SUBMISSION, cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS, NVMEIB_NOISE_CTRS_NORDDA_PENDING_IO);
			goto try_next;
			//TODO: (for rdda channels too)
			//Dont drain pending list, allow other channels to handle it.
			//Need to make sure we have or will have such other channels
			//otherwise, pending list wont be drained (IO stuck).
		}
	}

out:
	NFOUTS(ch->name);
	return comp_code;
}

static void nordda_pending_io_pcpu_func(void *ctx)
{
	struct nvmeibc_volume_req_info *info = ctx;
	struct nvmeibc_ib_nordda_channel *ch = info->nrch;
	struct nvmeibc_disk *disk = info->nrch->base.disk;

	if (atomic_read(&ch->base.dying) || atomic_read(&ch->net.base.dying) || atomic_read(&disk->paused)) {
		put_req_info(ch, info);
		return;
	}
	nordda_pending_io(disk, &ch->base, info, false, info->version);
}

//static void send_completion_send_io(struct nvmeibc_ib_nordda_channel *ch,
//									int index)
static int send_completion_has_rsp(struct nvmeibc_ib_nordda_channel *ch,
	int wc_req_index, u16 wc_req_version, bool wc_reused, const struct ib_wc *wc)
{
	struct nvmeibc_volume_req_info *req;
	struct nvmeib_iu *iu;
	u64 sent_tag;
	u16 sent_req_idx;
	u32 sent_chn_ver;
	u16 sent_req_ver;
	unsigned long flags = 0;
	int rv = 0;

	NFIN;
	if ((unsigned)wc_req_index < ch->base.disk->nrch_ioreq_num) {
		/* get nrch's req and req's iu pointed by wr_id's decoded values */
		req = &ch->reqs[wc_req_index];
		iu = wc_reused ? req->req.reuse_cmd : req->req.cmd;
		/* get and decode sent-tag from iu and cmp to wr_id */
		sent_tag = be64_to_cpu(((struct volume_client_req *)iu->buf)->hdr.tag);
		sent_req_idx = nordda_tag_decode_index(sent_tag);
		sent_chn_ver = nordda_tag_decode_ch_version(sent_tag);
		sent_req_ver = nordda_tag_decode_version(sent_tag);
		if (sent_req_idx != wc_req_index ||
			nordda_tag_decode_verify_any_ver_consider_reserved((u16)ch->base.version, sent_chn_ver)) {
			_NE(e0_send_completion_has_rsp,
				"nrch @BASE_NAME, req=@PTR, "
				"idx=@UINT/@UINT, ch-ver=@UINT/@UINT (consider rsvd-ver)",
				ch->base.name, req,
				sent_req_idx, wc_req_index,
				sent_chn_ver, (u32)ch->base.version);
			nvmeibc_disk_counters_inc(ch->base.disk, n_err_send_comp_tag_vs_wc);
			rv = -1; /* --> caller to disconnect-net */
		}

		ri_spin_lock_irqsave(req, flags);

		nvmeibc_nr_lat_meas_send_comp(&req->lat_meas, wc);

		if (++req->send_comp_counter < req->send_counter)
			_ND(trace_ib_nordda_channel_send_completion_has_rsp, "Received send completion on previous send s: @SEND_COUNTER, c: @SEND_COMP_COUNTER",
				req->send_counter, req->send_comp_counter);

		/* in case ulp returned reused-bb, we wait for send-comp of the
		   corresponding jour-write before reusing the req for pending */
		if (unlikely(req->reused_bb_wait_send_comp)) {
			/* sanity check there are no more send-comp we expect besides this very one and
			   that all accounting were done upon recv-comp of this assumed to be jour-write op  */
			if (req->send_comp_counter != req->send_counter ||
				test_bit(req->idx, req->nrch->req_in_use) || wc_reused || req->release_counter) {
				_NE(e1_send_completion_has_rsp,
					"nrch @BASE_NAME, req=@PTR, wc_reused=@BOOL, rel-cnt=@INT",
					ch->base.name, req, wc_reused, req->release_counter);
				nvmeibc_disk_counters_inc(ch->base.disk, n_err_comp_rcook_uncomp_sends_exceeded);
				rv = -1; /* --> caller to disconnect-net */
			}

			req->reused_bb_wait_send_comp_finish_cnt++;
			req->reused_bb_wait_send_comp = false;
			ri_spin_unlock_irqrestore(req, flags);

			if (!rv && nvmeibc_channel_try_use_req_info(&ch->base)) {
				_ND(t0_send_completion_has_rsp,
					"Call pending from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
					__builtin_return_address(0));
				_ND(t1_send_completion_has_rsp,
					"ch=@CH_PTR, index=@INDEX, version=@VERSION,"
					"release_counter=@RELEASE_COUNTER bcmd=@BCMD",
					ch, req->idx, req->version, req->release_counter, req->req.bcmd);
				nordda_pending_io(ch->base.disk, &ch->base, req, false, 0);
			}
			goto out;
		}

		/* Was if (!EC_PERF_CLNT_NORDDA_REDUCE_SEND_COMPS) */
		if (true) {
			if (wc_req_version != req->version) {
				_ND(trace_0_send_completion_has_rsp,
					"ch=@CH_PTR index=@INDEX, Wrong version @INT, exp. @INT",
					 ch, wc_req_index, wc_req_version, req->version);
			} else {
				if (nordda_tag_decode_verify_any_ver_consider_reserved(wc_req_version, sent_req_ver)) {
					_NE(e2_send_completion_has_rsp,
						"nrch @BASE_NAME, req=@PTR, "
						"req-ver=@UINT/@UINT (consider rsvd-ver)",
						ch->base.name, req, sent_req_ver, wc_req_version);
					BUG();
				}

				if (req->release_counter <= 0) {
					_NE(trace_1_send_completion_has_rsp,
						"ch=@CH_PTR index=@INDEX, Both send and recv already arrived",
						ch, wc_req_index);
					BUG_ON(1);
				}
				else if (--req->release_counter > 0) {
					_ND(trace_2_send_completion_has_rsp,
						"ch=@CH_PTR index=@INDEX, version=@VERSION send-comp before recv-comp",
						ch, wc_req_index, wc_req_version);
					req->n_send_comp++;
				}
				else {
					_ND(trace_3_send_completion_has_rsp,
						"ch=@CH_PTR index=@INDEX, version=@VERSION send-comp after recv-comp",
						ch, wc_req_index, wc_req_version);
					BUG_ON(req->release_counter != 0); /* paranoid */
					req->n_send_comp++; /* does NOT count jour send-comp that arrive after their recv-comp */

					/* Stop the WD now that we have received both completions */
					nvmeibc_ib_nordda_channel_req_stop_wd(req);

					/* unmap sg dma and unmap data */
					nvmeibc_ib_net_free_req(&ch->net.base, &req->req);
					ri_spin_unlock_irqrestore(req, flags);

					BUG_ON(!test_and_clear_bit(req->idx, req->nrch->req_in_use));

					/* comp to ulp, if not done yet, unlock and do pending-io */
					process_rsp_finalize(ch, req);
					goto out;
				}
			}
		}
		ri_spin_unlock_irqrestore(req, flags);
	}
out:
	NFOUT;
	return rv;
}

#if 0
static void __attribute__ ((unused)) send_completion_gen_cmd(struct nvmeibc_ib_nordda_channel *ch,
	int index, enum nvmeib_wr_opcode opcode)
{
	struct nvmeibc_volume_req_info *req;
	unsigned long flags;
//	enum nvmeib_gen_cmd_op gen_op = nordda_wr_opcode_get_gen_op(opcode);
	NFIN;

	if (true /* gen_op != NVMEIB_GEN_OP_XYZ */) {
		_NW(warn_ib_nordda_channel_send_completion_gen_cmd, "OOPS, opcode=@OPCODE", opcode);
		//For future gen-cmd that terminate base ONLY on send-comp.
		//This means that on send-comp, we can both [1] complete the
		//cmd ( i.e. wd-stop, callback upper layer) and [2] reuse it.
		goto out;
	}

	/* checks */
	if ((unsigned)index > ch->base.disk->nrch_ioreq_num) {
		_NW(warn_1_ib_nordda_channel_send_completion_gen_cmd, "OOPS: index=@INDEX, opcode=@OPCODE", index, opcode);
		WARN_ON(1);
		goto out;
	}
	req = &ch->reqs[index];
	ri_spin_lock_irqsave(req, flags);

	if (++req->send_comp_counter < req->send_counter)
		_NW(warn_2_ib_nordda_channel_send_completion_gen_cmd, "Received send completion on previous send s: @SEND_COUNTER, c: @SEND_COMP_COUNTER",
			req->send_counter, req->send_comp_counter);
	if (!req->req.dcmd) {
		_NW(warn_3_ib_nordda_channel_send_completion_gen_cmd, "net @BASE, req[@INDEX].req->dcmd is already NULL - bail",
		   &ch->net.base, index);
		WARN_ON(1);
		goto unlock_req; /* not putting back to pool */
	}
	if (req->release_counter != 1) {
		_NW(warn_4_ib_nordda_channel_send_completion_gen_cmd, "OOPS, release_counter=@RELEASE_COUNTER", req->release_counter);
		goto unlock_req;
	}

	/* stop the watchdog */
	nvmeibc_ib_nordda_channel_req_stop_wd(req);

	/* unlink cmd from req and and callback upper layer */
	nvmeibc_ib_net_nordda_complete_gen_cmd(&ch->net, &req->req, 0);

	//Nothing to do in net-layer for gen-cmd as the dma-map
	//is done by upper layer, which is only the jam right now
#if 0
	/* free the request */
	nvmeibc_ib_net_free_req(&ch->net.base, &req->req);
#endif


	_ND(trace_ib_nordda_channel_send_completion_gen_cmd, "ch=@CH_PTR index=@INDEX release_counter=@RELEASE_COUNTER",
		ch, index, req->release_counter);
	if (--req->release_counter == 0 &&
		nvmeibc_channel_try_use_req_info(&ch->base)) {
		/* try to get more work */
		_ND(trace_1_ib_nordda_channel_send_completion_gen_cmd, "Call pending during send completion");
		ri_spin_unlock_irqrestore(req, flags);
		nordda_pending_io(ch->base.disk, &ch->base, req, false, 0);
		goto out;
	}

unlock_req:
	ri_spin_unlock_irqrestore(req, flags);

out:
	NFOUT;
}
#endif

static int nordda_send_completion(struct nvmeibc_ib_net *net, struct ib_wc *wc,
	bool last_in_series)
{
	struct nvmeibc_ib_nordda_channel *ch = in_to_inrc(net);
	int index;
	enum nvmeib_wr_opcode opcode;
	u16 version;
	int rv = 0;
	// NFIN;

	opcode = nordda_wr_id_decode_opcode(nvmeib_wr_id_from_wc(wc));
	index = nordda_wr_id_decode_index(nvmeib_wr_id_from_wc(wc));

	if (likely(wc->status == IB_WC_SUCCESS)) {

		if (wc->opcode == IB_WC_SEND ||
			wc->opcode == IB_WC_RDMA_READ ||
			wc->opcode == IB_WC_RDMA_WRITE) {
			/* Successful send completion on this path - update lionic jiffies (per-cpu) */
			this_cpu_write(*ch->lionic->last_send_success_jif, jiffies);
		}
		if (wc->opcode == IB_WC_RDMA_WRITE && opcode == NVMEIB_RDMA_IO_KA) {
			this_cpu_write(*ch->lionic->last_io_ka_jif, jiffies);
		}

		if (nordda_wr_opcode_is_gen_cmd(opcode)) {
			/* opcode in range of Gen Commands */
			enum nvmeib_gen_cmd_op gen_op = nordda_wr_opcode_get_gen_op(opcode);
			switch (gen_op) {
			case NVMEIB_GEN_OP_GET_UUID_JOUR:
			case NVMEIB_GEN_OP_GET_EC_DB:
			case NVMEIB_GEN_OP_FREE_JRNL_ENTS:
			case NVMEIB_GEN_OP_BLKSET_RECOVERED:
			case NVMEIB_GEN_OP_LOCK:
			case NVMEIB_GEN_OP_GET_JMDC:
			case NVMEIB_GEN_OP_JENTRY_ERASE:
				version = nordda_wr_id_decode_version(nvmeib_wr_id_from_wc(wc));
				rv = send_completion_has_rsp(ch, index, version, false, wc);
				break;
			default:
				_NE(error_ib_nordda_channel_nordda_send_completion, "Invalid Gen Op: @RDMA_OP in wr_id: @WR_ID_LLONG",
				   (int)gen_op, nvmeib_wr_id_from_wc(wc));
			}
		} else {
			switch (opcode) {
			case NVMEIB_SEND_IO:
				version = nordda_wr_id_decode_version(nvmeib_wr_id_from_wc(wc));
				rv = send_completion_has_rsp(ch, index, version,
							nordda_wr_id_decode_reused(nvmeib_wr_id_from_wc(wc)), wc);
				break;
			case NVMEIB_RDMA_IO_KA:
				net->io_ka.in_progress = false;
				atomic64_inc(&net->io_ka.sent);
				break;

			default:
				if (!NVMEIB_FAST_REG_WR_ID)
					_NW(warn_ib_nordda_channel_nordda_send_completion, "Unexpected opcode @OPCODE but iu @INDEX finished successfully",
						opcode, index);
				break;
			}
		}
	}
	else {
		struct nvmeibc_volume_req_info *req = NULL;
		int nmdesc = -1;
		int release_counter = -1;
		int version = -1;
		unsigned long flags;

		if ((unsigned)index < ch->base.disk->nrch_ioreq_num) {
			req = &ch->reqs[index];
			nmdesc = req->req.nmdesc;
			ri_spin_lock_irqsave(req, flags);
			release_counter = req->release_counter;
			version = req->version;
			ri_spin_unlock_irqrestore(req, flags);
		}

		/* see dmesg, it may have 'dump error cqe' for some/"severe" errors */
		_NE(error_1_ib_nordda_channel_nordda_send_completion,
			"nrch @CH_NAME req=@REQ send-comp w/ error status=@STATUS @STATUS_STR "
			"opcode=@OPCODE @OPCODE_STR, index=@INT, nmdesc=@INT, release_counter=@INT, "
			"reused(!jour)=@BOOL, wc-version=@VERSION vs. req-version=@INT",
		    ch->base.name, req, wc->status, nvmeib_status_str(&wc->status),
		   opcode, nvmeib_wr_opcode_str(opcode), index, nmdesc, release_counter,
			nordda_wr_id_decode_reused(nvmeib_wr_id_from_wc(wc)),
			nordda_wr_id_decode_version(nvmeib_wr_id_from_wc(wc)), version);
		nordda_handle_qp_err(wc, true, ch, opcode, index);
		rv = -1;

		/* LOC_PROT err may be result of reusing (invalidating)
		   fr before send-comp of its previous use had arrived.
		   Let automation catch this... */
		if (wc->status == IB_WC_LOC_PROT_ERR) {
			WARN_KNOWN_ONCE(1, 8225);
		}
	}

	// NFOUT;
	return rv;
}

/* -------------------------------------------------------------------------- */
/* Receive                                                                    */
/* -------------------------------------------------------------------------- */

static int process_io_rsp(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeibc_volume_req_info *req, struct volume_server_rsp *rsp, struct ib_wc *wc)
{
	struct nvmeibc_disk_io_command *bcmd = req->req.bcmd;
	struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(bcmd);
	int rv = -1;
	NFIN;
	(void)wc;
	if (rsp->opcode != NVMEIBS_RSP_IO_OPCODE_OK)
		_NT(error_ib_nordda_channel_process_io_rsp, "IO request returned error: request @TAG code=@CODE", rsp->hdr.tag,
			req->comp_code);

#if	defined(TAKE_STATS) || defined(MGMT_STATS)
	nvmeib_stats_measureq(&req->send.common);
#	if defined(MGMT_STATS)
	if (req->nrch->base.disk && req->req.dcmd->cmd_type != NVMEIBC_DISK_CMD_IO)
		nvmeibc_disk_add_stats(req->nrch->base.disk, ch->net.base.port->nic_dev,
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
							   bcmd->v_disk_stats,
#else
							   NULL,
#endif // defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
							   bcmd->reqs, req->send.common.last_dt,
					nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig));
#	endif
#endif

	/* we piggyback an io ka update on each io req */
	this_cpu_write(*ch->lionic->last_io_ka_jif, jiffies);

	//TBD: unite this code with other channels + skip if comp_code!=0
	/* firstly complete the block layer I/O command and
	   start with checking if there was a piggyback read */
	if (dp_cmds_pigbck_has_any(bcmd)) {
		if (dc->opr == NVMEIBC_LOCK_READ) {
			union nvmeib_lock_blkset_entry *lock_entry =
				(void *)&rsp->io_rsp.base.piggyback_read;
			nvmeibc_set_lock_read(bcmd, dc, rsp->io_rsp.base.piggyback_read,
								  lock_entry->lock_id.all, lock_entry->blkset_info.all);

			if (dc->lock.id)
				_ND(trace_ib_nordda_channel_process_io_rsp, "val[0]=@VA", dc->lock.id);
		}
		else if (dc->opr == NVMEIBC_LOCK_BLKSET_INFO_WRITE) {
			/* The value written by s-nordda is "read" and sent back for debug */
			__attribute__ ((unused)) const union nvmeib_lock_blkset_entry *lock_entry = (void *)&rsp->io_rsp.base.piggyback_read;
			nvmeibc_ib_net_vol_req_dbgdi_check_rsp((&req->req), lock_entry, req->comp_code);
		}
	}

	/* If we are completing only on recv completion (wait_release_zero_before_cb = false) */
	if (!ch->wait_release_zero_before_cb)
	{
		bool was_reuse;
		int orig_sgcount;
		/* check for reuse mode */
		if (req->comp_code == 0 && do_reuse_request(&req->req)) {
			/* increase version and therefore disable any action on the
			send completion...
			*/
			_ND(info_ib_nordda_channel_process_io_rsp,
					"reusing ch @BASE_NAME(@CH_PTR), index=@REQ_INDEX, version=@VERSION",
					ch->base.name, ch, req->idx, req->version);

			nvmeibc_nr_lat_meas_update_nrch_pcpu_data(
				ch->per_cpu_lat_data, &req->lat_meas,
				!EC_PERF_CLNT_NORDDA_REDUCE_SEND_COMPS &&
				req->release_counter == 2,
				P2NV(ch->net.base.port)->dev_type == DT_siw);

			BUG_ON(!test_and_clear_bit(req->idx, req->nrch->req_in_use));
			++req->version;
			req->release_counter = 0;
			init_reuse_request(&req->req, &ch->base, req - ch->reqs, ch->base.comp_cpu,
			ch->base.version, ch->base.disk_version, ch->base.disk);

			/* we must clear the request before calling the IO completion
			since the block may use it immediately...
			*/

			/* TBD: Dont free jour-write's fr before its send-comp arrive.
			The free-req here, puts the fr-desc back to pool before send-comp
			arrives. This may lead to LOC_PROT err even though recv-comp had
			already arrived. Note we shall also consider the md's mapping, but
			as currently we map the md via dma/global-key (not mr), it shall
			cause the LOC_PROT error.

			possible solution:
			1. unmap on send-comp ; nope, it may stall and casue perf-hit.
			2. dont set SIGNALED in jour-write wr ; [JH] Tested, error persists.
			3. store aside the mapping results of the data and unmap it either
				on data-write's send-comp or on error i.e. nrch-disconnect.
			4. reuse the jour-write's map result for data-write stage (dont map).
				This cannot be applied to jmd and dmd w/o coordinating with ulp
				that src-buff is the same for both.

			headsup, handle these correctly:
			1. req->release-counter
			2. req->version which today "helps" us ignore send-comp of jour-write
			*/

			/* Attempt #1: move fr_desc to req->reuse_orig_fr_desc */
			orig_sgcount = REUSE_SG_FR_STORE(req);

			nvmeibc_ib_net_free_req(&ch->net.base, &req->req);
			nvmeibc_nr_lat_meas_recv_comp_process(&req->lat_meas);
			
			bcmd->disk_cmd.stats_done.type = STATS_DONE_LLP_COMPLETE_IO_RESPONSE_BUF_SAVE;
			nvmeibc_ib_net_complete_iocmd_reuse(&ch->net.base, &req->req, req->comp_code, orig_sgcount, false);
			goto out;
		}
		if (req->req.reused_bb) {
			REUSE_FR_RESTORE(req);
			if (req->reuse_orig.sgcount) {
				REUSE_SG_RESTORE(req);
			}
		}
		was_reuse = del_reuse_request(&req->req, ch->base.disk);
		nvmeibc_nr_lat_meas_recv_comp_process(&req->lat_meas);
		bcmd->disk_cmd.stats_done.type = STATS_DONE_LLP_COMPLETE_IO_RESPONSE_WAIT_RECV_COMP;
		nvmeibc_ib_net_complete_iocmd_reuse(&ch->net.base, &req->req, req->comp_code, req->req.sgcount, was_reuse);
	}

	/* Dont do this before send-comp of the fast-reg (of this IO) arrives.
	   free-req --> unmap-data --> fast-reg-pool-put --> allows other IO
	   to reuse same key i.e. invalidate it --> was getting send-comp w/
	   IB_WC_LOC_PROT_ERR (local-protection) err for wr NVMEIB_RDMA_LAST */
	#if 0
	/* free the request */
	nvmeibc_ib_net_free_req(&ch->net.base, &req->req);
	#endif

	rv = 0;

out:
	NFOUT;
	return rv;
}

static void process_lock_rsp(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeibc_volume_req_info *req, struct volume_server_rsp *rsp)
{
	struct nvmeibc_disk_lock_cmd *lock_cmd;

	NFIN;
	lock_cmd = disk_to_lock(req->req.dcmd);
	BUG_ON(lock_cmd->lock_rsp.comp_code != NVMEIBC_DISK_CMD_COMP_CODE_INVALID);

	if (rsp->opcode != NVMEIBS_RSP_IO_OPCODE_OK)
		_NT(error_ib_nordda_channel_process_lock_rsp, "LOCK request returned error: request @TAG code=@CODE opr @INDEX (@OPR_PTR) of lock-ch @BASE_NAME (@CH_PTR)",
		   rsp->hdr.tag, req->comp_code, lock_cmd->index, lock_cmd, lock_cmd->ch->base.name, lock_cmd->ch);

	req->comp_code = nvmeibc_ib_net_nordda_decode_lock_rsp(&ch->net, &req->req, rsp);

	if (!ch->wait_release_zero_before_cb)
	{
		nvmeibc_ib_net_nordda_complete_lock_cmd(&ch->net, &req->req, req->comp_code);
	}

	NFOUT;
}

static int process_gen_rsp(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeibc_volume_req_info *req, struct volume_server_rsp *rsp, struct ib_wc *wc)
{
	struct nvmeibc_disk_gen_cmd *g = disk_to_gen(req->req.dcmd);
	NFIN;

	g->recv_comp_time = ktime_get();
	g->recv_sz = wc->byte_len;
	if (rsp->opcode != NVMEIBS_RSP_GEN_OPCODE_OK)
		_NT(error_ib_nordda_channel_process_gen_rsp, "ch @BASE_NAME: GEN request returned error: request @TAG code=@CODE, OPCODE=@OPCODE",
			ch->base.name, rsp->hdr.tag, req->comp_code, rsp->opcode);

	if (!req->comp_code) {
		if (!(req->comp_code = nvmeibc_ib_net_nordda_decode_gen_rsp(&ch->net, g, rsp))) {
			if (g->opcode == NVMEIB_GEN_OP_GET_UUID_JOUR) {
				g->recv_sz += g->rsp.uj.jmdc_len + g->rsp.uj.ent_md_len;
			} else if (g->opcode == NVMEIB_GEN_OP_GET_EC_DB) {
				/* GET_EC_DB currently sends the entire BB */
				g->recv_sz = req->rn_pages * ch->base.disk->info->cntr_page_size;
			}
		}
	} else {
		_NE(error_4_ib_nordda_channel_process_gen_rsp, "ch @BASE_NAME: Failed gen-cmd @DISK_CMD_TO_STR, opcode=@OPCODE",
		   ch->base.name, disk_cmd_to_str(req->req.dcmd), disk_to_gen(req->req.dcmd)->opcode);
	}
	if (!ch->wait_release_zero_before_cb)
	{
		nvmeibc_ib_net_nordda_complete_gen_cmd(&ch->net, &req->req, req->comp_code);
	}

	NFOUT;
	return 0;
}

static inline void process_rsp_finalize(struct nvmeibc_ib_nordda_channel *ch,
										struct nvmeibc_volume_req_info *req)
{
	struct nvmeibc_disk_command *dcmd = req->req.dcmd;
	bool is_reuse = false, was_reuse = false;
	NFIN;

	/* checks */
	BUG_ON(ri_already_locked(req));
	BUG_ON(req->release_counter);
	BUG_ON(nvmeib_wd_is_armed_wdc(&req->wdc));
	BUG_ON(req->req.nmdesc);

	/* If only complete after both send and recv completions (wait_release_zero_before_cb = true) */
	if (ch->wait_release_zero_before_cb)
	{
		switch (dcmd->cmd_type) {
		case NVMEIBC_DISK_CMD_IO:
			if (req->comp_code == 0 && do_reuse_request(&req->req)) {
				init_reuse_request(&req->req, &ch->base, req->idx, ch->base.comp_cpu,
								   ch->base.version, ch->base.disk_version,
								   ch->base.disk);
				is_reuse = true;
			} else {
				was_reuse = del_reuse_request(&req->req, ch->base.disk);
			}
			nvmeibc_nr_lat_meas_recv_comp_process(&req->lat_meas);
			req->req.dcmd->stats_done.type = STATS_DONE_LLP_COMPLETE_IO_RESPONSE_FINALIZE;
			nvmeibc_ib_net_complete_iocmd_reuse(&ch->net.base, &req->req, 
				req->comp_code, req->req.sgcount, was_reuse);

			break;

		case NVMEIBC_DISK_CMD_GEN:
			nvmeibc_ib_net_nordda_complete_gen_cmd(&ch->net, &req->req, req->comp_code);
			break;

		case NVMEIBC_DISK_CMD_LOCK:
			nvmeibc_ib_net_nordda_complete_lock_cmd(&ch->net, &req->req, req->comp_code);
			break;

		default:
			BUG_ON(1);
		}
	}

	nvmeibc_nr_lat_meas_update_nrch_pcpu_data(
		ch->per_cpu_lat_data, &req->lat_meas,
		true, P2NV(ch->net.base.port)->dev_type == DT_siw);

	if (is_reuse) {
		/* The request is saved aside until the reuse comes
		 * (or nvmeibc_disk_reused_bb_release() is called) */
		goto out;
	}

	if (nvmeibc_channel_try_use_req_info(&ch->base)) {
		_ND(trace_1_process_rsp_finalize,
			"Call pending from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
			__builtin_return_address(0));
		_ND(trace_2_process_rsp_finalize,
			"ch=@CH_PTR, index=@INDEX, version=@VERSION,"
			"release_counter=@RELEASE_COUNTER bcmd=@BCMD",
			ch, req->idx, req->version, req->release_counter, req->req.bcmd);
		nordda_pending_io(ch->base.disk, &ch->base, req, false, 0);
	}

out:
	NFOUT;
}

/* When wait_release_zero_before_cb is set, the defer is redundant because
 * the the callback is called from process_rsp_finalize which is after
 * the lock has already been released */
#define PROCESS_RSP_COND_DEFER_CB(_ch, _req, _rsp, _wc, _rsp_func, _deferred_cmd) ({ \
	int __rv = 0; \
	if (NVMEIBC_NRCH_DEFER_COMPLETE_IOCMD && \
		(_ch)->base.disk->defer_block_cb_on_io_cmd && \
		!(_ch)->wait_release_zero_before_cb) \
	{ \
		_deferred_cmd = (_req)->req.dcmd; \
		(_deferred_cmd)->defer_cb = true; \
		__rv = _rsp_func(_ch, _req, _rsp, _wc); \
		(_deferred_cmd)->defer_cb = false; \
	} else { \
		__rv = _rsp_func(_ch, _req, _rsp, _wc); \
	} \
	__rv; \
	})

#define COMPLETE_DEFERRED_IOCMD(_iocmd, _ch, _cpu_mask_info) \
	do { \
		struct nvmeibc_dev *_nic = (_ch)->net.base.port->nic_dev;\
		if (NVMEIBC_NRCH_DEFER_COMPLETE_IOCMD && _iocmd) { \
			struct nvmeib_cpu_mask __cpu_mask = {0}; \
			BUG_ON((_ch)->wait_release_zero_before_cb);\
			if (_cpu_mask_info) \
				__cpu_mask = (_cpu_mask_info)->mask; \
			(_iocmd)->ulp_cb(_iocmd, _nic); \
			nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, __cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS, \
										NVMEIB_NOISE_CTRS_DEFER_COMPLETE_IOCMD); \
		} \
	} while(0);

static int process_rsp(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeib_iu *iu, struct ib_wc *wc)
{
	struct volume_server_rsp *rsp = iu->buf;
	struct nvmeibc_volume_req_info *req;
	int put_back = 1;
	unsigned long flags = 0;
	u64 rsp_tag;
	u16 req_index, req_version;
	u32 req_ch_version;
	enum nvmeibc_disk_command_type dcmd_type;
	struct nvmeibc_disk_command *deferred_cmd = NULL;
	const struct nvmeib_cpu_mask_info *cpu_mask_info = NULL;
	NFIN;

	rsp_tag = be64_to_cpu(rsp->hdr.tag);
	req_index = nordda_tag_decode_index(rsp_tag);
	req_version = nordda_tag_decode_version(rsp_tag);
	req_ch_version = nordda_tag_decode_ch_version(rsp_tag);
	if (req_index >= ch->base.disk->nrch_ioreq_num ||
			(req_version != NVMEIB_TAG_VERSION_RESERVED && req_version != ch->reqs[req_index].version) ||
			(req_ch_version != NVMEIB_TAG_VERSION_RESERVED && req_ch_version != (u32)ch->base.version)) {
		_NW(warn_ib_nordda_channel_process_rsp,
			"OOPS: ch @BASE_NAME(@CH_PTR), tag=@TAG, index=@REQ_INDEX, version=@VERSION, ch_version=@VERSION, opcode=@OPCODE",
			ch->base.name, ch, rsp_tag, req_index, req_version, req_ch_version, rsp->opcode);
		//WARN_ON_ONCE(1);
		BUG_ON(1);
		put_back = 0;
		goto out;
	}

	req = &ch->reqs[req_index];
	WARN_ON(!test_bit(req->idx, req->nrch->req_in_use));

	ri_spin_lock_irqsave(req, flags);
	if (atomic_read(&ch->net.base.dying))
		_NT(trace_3_ib_nordda_channel_process_rsp, "net @BASE is dying, complete (nullify) ch.req[@TAG].req->dcmd "
		   "(req @REQ)...", &ch->net.base, rsp_tag, &req->req);

	if (!req->req.dcmd) {
		_NW(warn_1_ib_nordda_channel_process_rsp, "net @BASE, req[@TAG].req->dcmd is already NULL - bail",
		   &ch->net.base, rsp_tag);
		WARN_ON_ONCE(1);
		put_back = 0;
		goto unlock_req;
	}

	BUG_ON(req->comp_code != NVMEIBC_DISK_CMD_COMP_CODE_INVALID);
	WARN_ON(NVMEIBC_NRCH_DEFER_COMPLETE_IOCMD && req->req.dcmd->defer_cb);

	if (req->n_wd_events) {
		_NW(trace_4_ib_nordda_channel_process_rsp,
			"ch @BASE_NAME(@CH_PTR), wd-rescued req @PTR (idx=@INT)",
			ch->base.name, ch, req, req->idx);
		nvmeibc_disk_counters_inc(ch->base.disk, n_err_nrch_wd_rescue_comp);
	}

	if (rsp->opcode == NVMEIBS_RSP_IO_OPCODE_OK ||
		rsp->opcode == NVMEIBS_RSP_IO_OPCODE_ERR) {
		req->comp_code = be32_to_cpu(rsp->io_rsp.base.comp_code);
		nvmeibc_nr_lat_meas_recv_comp(&req->lat_meas, wc);
	}
	else if (rsp->opcode == NVMEIBS_RSP_GEN_OPCODE_OK ||
		rsp->opcode == NVMEIBS_RSP_GEN_OPCODE_ERR) {
		req->comp_code = be32_to_cpu(rsp->gen_rsp.comp_code);
	}
	else {
		_NT(trace_2_ib_nordda_channel_process_rsp, "Invalid server response type (@OPCODE)", rsp->opcode);
		req->comp_code = -EINVAL;
		goto unlock_req;
	}

	if (req->comp_code < 0) {
		_NE(error_1_ib_nordda_channel_process_rsp, "Failed IO, ch @BASE_NAME comp-code @IO_RSP_COMP_CODE_TO_STR (@INT32_HEX)",
			ch->base.name, io_rsp_comp_code_to_str(req->comp_code), req->comp_code);
		if (req->comp_code == NVMEIBS_IO_RSP_ERR_SUBMIT ||
			req->comp_code == NVMEIBS_IO_RSP_ERR_TXID_TRIM) {
			/* Failed to submit cmd to disk, disk is rst/rm,
				*	   Dont wait for srv to disconnect this client.
				*	   According to Dhs this can take upto a second
				*	   during which the client will keep attempting
				*	   IO, leaving many stale locks and flood TOMA
				*	   with many topo REG/UNREG -> select stuck */
			nvmeibc_disk_start_release(ch->base.disk, NVMEIBC_DISK_RELEASE_REM_DRV_ERR);
			}
			req->comp_code = -EIO;
	}

	req->n_recv_comp++;
	BUG_ON(req->recv_comp_arrived);
	/* protect against double completion */
	req->recv_comp_arrived = true;

	if (!ch->wait_release_zero_before_cb)
		nvmeibc_ib_nordda_channel_req_stop_wd(req);

	switch ((dcmd_type = req->req.dcmd->cmd_type)) {
	int io_rsp_rv;
	case NVMEIBC_DISK_CMD_IO:
		if (req->req.bcmd->reqs[0].cpu_mask_info)
			cpu_mask_info = req->req.bcmd->reqs[0].cpu_mask_info;
		io_rsp_rv = PROCESS_RSP_COND_DEFER_CB(ch, req, rsp, wc, process_io_rsp, deferred_cmd);
		if (io_rsp_rv)
			goto unlock_req;
		break;
	case NVMEIBC_DISK_CMD_GEN:
		io_rsp_rv = PROCESS_RSP_COND_DEFER_CB(ch, req, rsp, wc, process_gen_rsp, deferred_cmd);
		/* should never happen */
		BUG_ON(io_rsp_rv);
		break;
	case NVMEIBC_DISK_CMD_LOCK:
		process_lock_rsp(ch, req, rsp);
		break;
	default:
		_NW(warn_2_ib_nordda_channel_process_rsp, "net @BASE, unexp cmd-type=@TYPE (req[@TAG]) - bail",
		&ch->net.base, req->req.dcmd->cmd_type, rsp_tag);
		WARN_ON(1);
		put_back = 0;
		goto unlock_req;
	}

	/* we only do more work iff the send completion for the req arrives */
	if (--req->release_counter == 0) {
		_ND(trace_7_ib_nordda_channel_process_rsp,
			"ch=@CH_PTR tag=@TAG, recv-comp after send-comp",
			ch, rsp_tag);

		/* Stop the WD now that we have received both completions */
		nvmeibc_ib_nordda_channel_req_stop_wd(req);

		/* unmap sg dma and unmap data */
		nvmeibc_ib_net_free_req(&ch->net.base, &req->req);
		ri_spin_unlock_irqrestore(req, flags);

		BUG_ON(!test_and_clear_bit(req->idx, req->nrch->req_in_use));

		COMPLETE_DEFERRED_IOCMD(deferred_cmd, ch, cpu_mask_info);

		/* comp to ulp, if not done yet, unlock and do pending-io */
		process_rsp_finalize(ch, req);

		goto out;

	}
	else {
		_ND(trace_6_ib_nordda_channel_process_rsp, "ch=@CH_PTR index=@REQ_INDEX version=@VERSION release_counter=@RELEASE_COUNTER bcmd=@BCMD",
			ch, req_index, req_version, req->release_counter, req->req.bcmd);
		BUG_ON(req->release_counter < 0);
	}

unlock_req:
	ri_spin_unlock_irqrestore(req, flags);
	COMPLETE_DEFERRED_IOCMD(deferred_cmd, ch, cpu_mask_info);

out:
	NFOUT;

	return put_back;
}

static void nordda_handle_recv(struct nvmeibc_ib_nordda_channel *ch,
	struct ib_wc *wc)
{
	struct nvmeib_dev *dev = P2NV(ch->net.base.port);
	struct ib_device *ibdev = dev->ib_dev;
	int index = nvmeib_idx_from_wc(wc);
	struct nvmeib_iu *iu = nvmeibc_ib_nordda_channel_get_rx_iu(ch, index);
	struct nvmeib_hdr *hdr;
	int put_back = 0;//1;
	int rv;
	u32 opcode __attribute__((unused));
	int wr_opcode = nvmeib_opcode_from_wc(wc);

	// NVMEIB_LOG_LONGTERM("NVMEIB_SEND_IO: @WR_ID, @WR_OP,@INDEX", nordda_recv,
	//		nvmeib_wr_id_from_wc(wc), wr_opcode, index);

	if ((u16)wr_opcode == NVMEIB_RECV) {
		_ND(trace_ib_nordda_channel_nordda_handle_recv, "Received recv CQE: wr_id=@WR_ID_LLONG, wr_opcode=@WR_OPCODE, index=@INDEX",
			nvmeib_wr_id_from_wc(wc), wr_opcode, index);
	}
	else if ((u16)wr_opcode == NVMEIB_SEND_IO) {
		_ND(trace_1_ib_nordda_channel_nordda_handle_recv, "Received NVMEIB_SEND_IO: wr_id=@WR_ID_LLONG, wr_opcode=@WR_OPCODE, index=@INDEX",
			nvmeib_wr_id_from_wc(wc), wr_opcode, index);
		return;
	}
	else {
		_NW(warn_ib_nordda_channel_nordda_handle_recv, "Received unexpected: wr_id=@WR_ID_LLONG, wr_opcode=@WR_OPCODE, index=@INDEX",
			nvmeib_wr_id_from_wc(wc), wr_opcode, index);
		WARN_ON_ONCE(1);
		return;
	}

	if (!iu) {
		_NT(trace_2_ib_nordda_channel_nordda_handle_recv, "Got receive with no iu");
		return;
	}

	/* JH IOMMU: DMA_FROM_DEVICE is correct, sink for Remote RDMA_SEND */
	ib_dma_sync_single_for_cpu(ibdev, iu->dma, NVMEIBC_NORDDA_CLIENT_MSG_SIZE,
		DMA_FROM_DEVICE);

	hdr = (struct nvmeib_hdr *)iu->buf;

	switch (hdr->opcode) {
	case NVMEIB_RSP:
		put_back = process_rsp(ch, iu, wc);
		break;

	case NVMEIB_SIW_ACK_TGT_SEND:
		BUG_ON(1);
		break;

	default:
		opcode = nvmeib_opcode_from_wr_id(nvmeib_wr_id_from_wc(wc));
		_NT(trace_3_ib_nordda_channel_nordda_handle_recv, "Unhandled client opcode @OPCODE wc_opcode=@WC_OPCODE opcode=@OPCODE_LLONG data=@DATA_INT",
			hdr->opcode, wc->opcode, nvmeib_wr_id_from_wc(wc), be32_to_cpu(wc->ex.imm_data));
		break;
	}

	if (put_back) {
		if ((rv = nvmeibc_ib_nordda_channel_put_rx_iu(ch, iu)))
			_NT(error_ib_nordda_channel_nordda_handle_recv, "Post received iu failed with error code @RV", rv);
	}
}

static int nordda_recv_completion(struct nvmeibc_ib_net *net,
	struct ib_wc *wc)
{
	struct nvmeibc_ib_nordda_channel *ch = in_to_inrc(net);
	struct nvmeib_iu *iu = NULL;
	int index;
	int rv, cpu;

	if (is_ll_pcpu_nrch(ch) && (cpu = pcpu_nrch_cpu_get(ch)) != smp_processor_id()) {
		_NE_dmesg(__AUTOID__, "oops, nrch of cpu @INT completed on cpu @INT",
				  cpu, smp_processor_id());
		//WARN_ON_ONCE(1);
		BUG_ON(1);
	}

	atomic64_set(&ch->last_received, jiffies);
	if (likely(wc->status == IB_WC_SUCCESS)) {
		/* Successful recv completion on this path - update lionic jiffies (per-cpu) */
		this_cpu_write(*ch->lionic->last_recv_success_jif, jiffies);
		nordda_handle_recv(ch, wc);
		//rv = 1; dont ask for processing scq
		rv = 0;
	}
	else {
		index = nvmeib_idx_from_wc(wc);
		if (wc->status != IB_WC_WR_FLUSH_ERR || !atomic_read(&net->dying)) {
			_NT(error_ib_nordda_channel_nordda_recv_completion, 
			    "nordda channel @BASE_NAME (net @NET) received completion with error "
			   "(status @STATUS, wr_id @WR_ID_LLONG, index @INDEX, qp_num @QP_NUM, remote qp_num @REMOTE_QPN)", 
				ch->base.name, net, wc->status, nvmeib_wr_id_from_wc(wc), index,
				net->qp->qp_num, net->remote_qpn);
			nordda_handle_qp_err(wc, false, ch,
				nvmeib_opcode_from_wc(wc), index);
		}
		iu = nvmeibc_ib_nordda_channel_get_rx_iu(ch, index);
		if (iu)
			nvmeibc_ib_nordda_channel_put_rx_iu(ch, iu);
		else
			_NT(trace_ib_nordda_channel_nordda_recv_completion, "net @NET (@BASE_NAME): NULL iu  (status @STATUS, wr_id @WR_ID_LLONG, index @INDEX)",
				net, ch->base.name, wc->status, nvmeib_wr_id_from_wc(wc), index);

		rv = -1;
	}

	return rv;
}

/* ************************************************************************** */
/* Disconnecting                                                              */
/* ************************************************************************** */

static void remove_wdc(struct nvmeibc_ib_nordda_channel *ch)
{
	int i;
	NFIN;

	if (ch->reqs)
		for (i = 0; i < ch->base.disk->nrch_ioreq_num; ++i)
			nvmeib_wd_remove_wdc(&ch->reqs[i].wdc);

	NFOUT;
}

static bool nordda_channel_try_launch_release_wq(
	struct nvmeibc_ib_nordda_channel *ch)
{
	struct nvmeibc_ib_net *net = &ch->net.base;
	bool rv;
	proc_name_t pname;

	clnt_proc_name_format(pname, 'C', "WQ", "nr", nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(&ch->base)));
	NFIN;
	if (NORDDA_RELEASE_ON_THREAD &&
		(ch && (ch->release_wq ||
			(ch->release_wq = is_ll_pcpu_nrch(ch) ?
				wq_create_on_verbose(pname, pcpu_nrch_cpu_get(ch)) :
				wq_create_verbose(pname))))) {
		wq_add_work(ch->release_wq, &net->remove_work.work);
		rv = true;
	}
	else {
#if NORDDA_RELEASE_ON_THREAD
		_NE(error_ib_nordda_channel_nordda_channel_try_launch_release_wq, "Fail to allocate nordda channel removal queue");
#endif
		rv = false;
	}
	NFOUT;
	return rv;
}

static void nordda_channel_remove_work(struct workqe_struct *work)
{
	struct remove_net_workq *rwork =
		container_of(work, struct remove_net_workq, work);
	struct nvmeibc_ib_net *net =
		container_of(rwork, struct nvmeibc_ib_net, remove_work);
	struct nvmeibc_ib_nordda_channel *ch = in_to_inrc(net);
	struct nvmeibc_disk *disk = ch->base.disk;
	int state;
	NFIN;

	if ((state = nvmeib_get_state_guard(&net->state))
		!= NVMEIBC_IB_NET_DISCONNECTING) {
		_NE(error_ib_nordda_channel_nordda_channel_remove_work, "remove_work called on net = @NET in invalid state @STATE",
		 net, state);
		BUG();
	}

	_NT(trace_ib_nordda_channel_nordda_channel_remove_work,
		"nrch @IOCH_NAME (@CH_PTR) remove-work, attempt start", ch->base.name, ch);

	/* if we need to run current function in a separate thread try to
	   launch it here and leave on success otherwise continue here */
	if (rwork->defer_release_wq_create) {
		if (nvmeibc_channel_rm_work_get()) {
			net->remove_work.defer_release_wq_create = false;
			WQ_INIT_WORK(&net->remove_work.work, nordda_channel_remove_work);
			if (nordda_channel_try_launch_release_wq(ch))
				goto out;
		}
	}

	_NT(trace_1_ib_nordda_channel_nordda_channel_remove_work,
		"nrch @IOCH_NAME (@CH_PTR) remove-work, start", ch->base.name, ch);
	/* prevent cookies from returning this channel to available-pool.
	   note that new cookies with current version can be given to ulp
	   as long as comps can arrive i.e. break-qp (drain-cq) */
	nvmeibc_disk_channel_version_invalidate(&ch->base);

	/* wait for ch initialization completion */
	wait_for_completion(&ch->init_comp);

	/* prevent new iocmds on this channel (initiated from execute-io-remote).
	   iocmd completion path won't issue as req-info is not reusable */
	nvmeibc_disk_available_norddas_del(ch->base.disk, ch);

	/* wait for req-info that user took but still had not re/used for io */
	nvmeibc_channel_wait_used_req_infos(&ch->base);

	/* remove wd */
	remove_wdc(ch);

	/* break-qp */
	nvmeibc_ib_net_break_qp(net);

	/* drain defered recv-comps */
	rc_wq_destroy(ch);

	/* complete and free reqs */
	nordda_channel_free_volume_reqs(ch);

	/* destroy qp, cm, send/recv cq, on_free cb, free comp arrays */
	nvmeibc_ib_net_nordda_free(&ch->net);

	/* mark ch unused */
	nvmeibc_ib_nordda_channel_end_use(ch);

	ch->n_used_reqs = 0;
	ch->n_uses_ever = 0;

	/* if we manage to read disk->dying before it was set
	   but add the start-io-channel work after it was set,
	   the start-io-channel work may end-up running after
	   the admin-ch-disconnect work but NOT after the
	   disk-release work because:
	   (a) It is added while admin-ch-disconnnect runs (as
	       the later waits for us) and disk-release work,
	       which added the admin-ch-disconnect work, drains
	       the admin-wq i.e. will wait to start-io-channel
		   work to finish.
	   (b) start-io-channel work, checks for alive admin-ch
	       and disk !dying so it shall exit immediately w/o
		   referencing admin-ch stuff.
    */
	if (!atomic_read(&disk->dying) &&
		!nvmeibc_disk_ioch_drained_is_pending(&ch->base)) {
		_NT(trace_4_ib_nordda_channel_nordda_channel_remove_work, "Speedup reconnecting nr-channels disk @DISK_NAME", disk->name);
		nvmeibc_disk_start_io_channels(disk, false);
	}

	if (!rwork->defer_release_wq_create)
		nvmeibc_channel_rm_work_put();
out:
	_NT(trace_5_ib_nordda_channel_nordda_channel_remove_work, "nrch @BASE_NAME remove-work, done", ch->base.name);
	NFOUT;
}

static void on_disconnect_nordda_ch(struct nvmeibc_ib_net *net)
{
	NFIN;

	net->remove_work.defer_release_wq_create = true; //irqs_disabled();
	WQ_INIT_WORK(&net->remove_work.work, nordda_channel_remove_work);
	nvmeibc_admin_channel_add_work(net->admin_ch, &net->remove_work.work);

	NFOUT;
}

bool nvmeibc_ib_nordda_channel_try_disconnect(
	struct nvmeibc_ib_nordda_channel *ch)
{
	unsigned long flags = -1;
	bool rv = false;
	bool already_locked;
	NFIN;

	_NT(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_try_disconnect, "nrch @BASE_NAME (@CH_PTR), initiated from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		ch->base.name, ch, __builtin_return_address(0));

	if (!(already_locked = nvmeibc_channel_already_locked(&ch->base)))
		nvmeibc_channel_spin_lock_irqsave(&ch->base, &flags);
	if (nvmeibc_net_is_connected(&ch->net.base)) {
		/* if ch's net is not already dying, on_disconnect cb is called */
		nvmeibc_ib_net_disconnect_(&ch->net.base);
		rv = true;
	}
	/* else: note that if ch was conneted but its state was already
	   changed to DISCONNECTING (i.e. remove-work is/will be running),
	   caller marks ch as unused and thus on next conectc attempt must
	   verify remove work is done */
	if (!already_locked)
		nvmeibc_channel_spin_unlock_irqrestore(&ch->base, flags);
	if (!rv)
		_NT(trace_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_try_disconnect, "ch @CH_PTR, net not alive", ch);

	NFOUT;
	return rv;
}

void nvmeibc_ib_nordda_channel_clear_rq(struct nvmeibc_ib_nordda_channel *ch,
	bool cd)
{
	NFIN;
	if (ch && ch->release_wq) {
		if (cd) {
			_NT(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_clear_rq, "Draining nordda channel @BASE_NAME disk @DISK_NAME",
				ch->base.name, ch->base.disk->name);
			wq_drain(ch->release_wq);
			_NT(trace_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_clear_rq, "Drained channel @BASE_NAME disk @DISK_NAME", ch->base.name,
				ch->base.disk->name);
		}
		wq_destroy(ch->release_wq);
		ch->release_wq = NULL;
	}
	NFOUT;
}

/* ************************************************************************** */
/* Free                                                                       */
/* ************************************************************************** */
void nvmeibc_ib_nordda_channel_free(struct nvmeibc_ib_nordda_channel *ch)
{
	NFIN;

	nvmeibc_ib_nordda_channel_clear_rq(ch, true);

	if (ch->rc_wq) {
		_NE(error_ib_nordda_channel_nvmeibc_ib_nordda_channel_free,
			"OOPS, nrch @CH_PTR @BASE_NAME still has rc_wq, drain & destroy...",
		   ch, ch->base.name);
		WARN_ON(1);
		rc_wq_destroy(ch);
	}

	if (ch->priv_srq) {
		nvmeib_srq_info_free(ch->priv_srq);
		ch->priv_srq = NULL;
	}

	if (ch->recv_q) {
		nvmeib_free_recvq(ch->recv_q);
		kfree(ch->recv_q);
		ch->recv_q = NULL;
	}

	/* sanity */
	if (ch->inuse) {
		_NT(trace_ib_nordda_channel_nvmeibc_ib_nordda_channel_free, "nrch @BASE_NAME (@CH_PTR), still inuse", ch->base.name, ch);
		WARN_ON(1);
	}

	if ((ch->available_link.next ||
		 ch->available_link.prev) && /* if here before init-list-head */
		!list_empty(&ch->available_link)) {
		_NT(trace_1_ib_nordda_channel_nvmeibc_ib_nordda_channel_free, "nrch @BASE_NAME (@CH_PTR), still linked to available list", ch->base.name, ch);
		WARN_ON(1);
	}

	nvmeib_public_free_percpu(ch->per_cpu_lat_data);

	NFOUT;
}

struct nvmeib_iu* nvmeibc_ib_nordda_channel_get_rx_iu(struct nvmeibc_ib_nordda_channel *ch, int index)
{
	struct nvmeibc_ib_net *net = &ch->net.base;
	if (net->srq_info)
		return nvmeib_srq_rtrv_recv(net->srq_info, index, net);
	else if (ch->recv_q)
		return nvmeib_rq_rtrv_recv(ch->recv_q, index);
	else
		return NULL;
}

int nvmeibc_ib_nordda_channel_put_rx_iu(struct nvmeibc_ib_nordda_channel *ch, struct nvmeib_iu *iu)
{
	struct nvmeibc_ib_net *net = &ch->net.base;
	int rv = 0;
	if (net->srq_info)
		rv = nvmeib_srq_post_recv(net->srq_info, iu);
	else if (ch->recv_q)
		rv = nvmeibc_ib_net_post_recvq(net, ch->recv_q, iu);
	else
		rv = -EINVAL;
	return rv;
}

static int nordda_channel_execute_ka(struct nvmeibc_channel *ch)
{
	struct nvmeibc_ib_nordda_channel *nrch = c_to_inrc(ch);
	struct nvmeibc_ib_net *net = &nrch->net.base;
	u64 val = jiffies;

	return nvmeibc_ib_net_execute_ka(net, &nrch->ka_rai, &val, sizeof(val));
}

bool nvmeibc_ib_nordda_channel_check_reused(
	struct nvmeibc_ib_nordda_channel *ch, void *context)
{
	bool reused;

	NFIN;
	reused = context && ((struct nvmeibc_volume_req_info *)context)->req.reused_bb;
	NFOUT;
	return reused;
}

void nvmeibc_ib_nordda_channel_reused_context(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeib_data_reuse_buf_params *p, void **context)
{
	struct nvmeibc_volume_req_info *req = ch->reqs + p->req_id;
	int n_send_comps;
	unsigned long flags = 0;
	bool already_locked = ri_already_locked(req);

	BUG_ON(p->req_id >= NVMEIB_MAX_NORDDA_IO_REQ);

	if (!already_locked)
		ri_spin_lock_irqsave(req, flags);
	n_send_comps = req->send_counter - req->send_comp_counter;

	_NT(t0_nordda_channel_reused_context,
		"nrch=@PTR, return req=@PTR (idx=@INT), taken for @LLU/@LLU jif/sec, "
		"sent/comp=@LLU/@LLU (n=@INT)",
		ch, req, req->idx, jiffies - ch->base.reused_bb_lru_jif,
		(jiffies - ch->base.reused_bb_lru_jif) / HZ,
		req->send_counter, req->send_comp_counter, n_send_comps);

	if (req->req.reused_bb != 1 || req->release_counter) {
		_NE(error_nvmeibc_ib_nordda_channel_reused_context,
			"req=@PTR, reused_bb=@INT, release-counter=@INT\n",
		   req, req->req.reused_bb, req->release_counter);
		BUG();
	}
	if (req->reuse_orig.sgcount) {
		struct nvmeibc_ib_net *net = &ch->net.base;
		REUSE_SG_RESTORE(req);
		/* bcmd is NULL at this point so we can't DMA unmap the SGL, not an issue on x86_64, but we could leak DMA memory on other platforms */
		req->req.sgcount = 0;
		if (req->reuse_orig.nmdesc) {
			REUSE_FR_RESTORE(req);
			nvmeibc_ib_net_unmap_data(net, &req->req);
		}
	}

	del_reuse_request_no_rcookie(&ch->base, &req->req);

	/* wait for pending send-comps before reusing this req (for pending cmds)
	   i.e. before allowing new sends so we survive checks of send_completion_has_rsp */
	if (n_send_comps == 0) {
		*context = req;
	}
	else if (n_send_comps == 1) {
		req->reused_bb_wait_send_comp = true;
		req->reused_bb_wait_send_comp_start_cnt++;
		/* caller had ref-counted this req as used, undo this under ri's lock
		   to prevent race with send-comp which does try-use and if all reqs
		   are in use, it will not process pending cmds -> starvation, not really,
		   bcz if all are in use when the next one complete is handles pending */
		nvmeibc_channel_end_use_req_info(&ch->base);
		*context = NULL;
	}
	else {
		nvmeibc_disk_counters_inc(ch->base.disk, n_err_rtrn_rcook_uncomp_sends_exceeded);
		*context = NULL;

		/* too harsh but unlikely case */
		nvmeibc_disk_start_release(ch->base.disk, NVMEIBC_DISK_RELEASE_RETURN_RCOOKIE);
	}
	if (!already_locked)
		ri_spin_unlock_irqrestore(req, flags);
}

bool nvmeibc_ib_nordda_channel_alive(struct nvmeibc_ib_nordda_channel *ch)
{
	return nvmeib_get_state_guard(&ch->net.base.state) == NVMEIBC_IB_NET_LIVE;
}

int nvmeibc_ib_nordda_channel_poll_cqs(struct nvmeibc_ib_nordda_channel *ch, bool notify)
{
	return nvmeibc_ib_net_poll_cqs(&ch->net.base, notify);
}
