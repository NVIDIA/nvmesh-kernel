#include <linux/bug.h>

#include "nvmeibc_ib_net.h"
#include "nvmeibc_main.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibc_block.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_admin_channel.h"
#include "nvmeibc_ib_nordda_channel.h"
#include "nvmeibc_trend_types.h"
#include "nvmeib_srq.h"
#include "nvmeib_version_shared.h"
#include "nvmeib_public.h"
#include <linux/bug.h>
#include "core/nvmeibc_core_common.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "nvmeibc_nr_lat_meas.h"

#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
#include "nvmeib_pcpu_wq.h"
#include "nvmeib_completion_noise.h"

//#define DEBUG 1
//#define CONFIG_NVMEIB_DEBUG 1
#define DEBUG_NET_PRINT_STATS 0
#include "nvmeib_utils.h"

bool nr_defer_recv_comps = true;
module_param(nr_defer_recv_comps, bool, 0644);
MODULE_PARM_DESC(nr_defer_recv_comps, "Defer processing of nrch recv completions (for RDMA)");

bool nr_defer_recv_comps_tcp = false;
module_param(nr_defer_recv_comps_tcp, bool, 0644);
MODULE_PARM_DESC(nr_defer_recv_comps_tcp, "Defer processing of nrch recv completions (for TCP)");


bool nr_shared_cq = true;
module_param(nr_shared_cq, bool, 0644);
MODULE_PARM_DESC(nr_shared_cq, "nrch uses shared cq (for RDMA)");

bool nr_shared_cq_tcp = false;
module_param(nr_shared_cq_tcp, bool, 0644);
MODULE_PARM_DESC(nr_shared_cq_tcp, "nrch uses shared cq (for TCP)");

bool nr_use_srq = true;
module_param(nr_use_srq, bool, 0644);
MODULE_PARM_DESC(nr_use_srq, "nrch uses SRQ (for RDMA)");

bool nr_use_srq_tcp = true;
module_param(nr_use_srq_tcp, bool, 0644);
MODULE_PARM_DESC(nr_use_srq_tcp, "nrch uses SRQ (for TCP)");

bool nvmeibc_panic_on_core_dbgdi = false;
module_param_named(panic_on_core_dbgdi, nvmeibc_panic_on_core_dbgdi, bool, 0644);
MODULE_PARM_DESC(panic_on_core_dbgdi, "On core-dbgdi detection panic both client and target");

bool nvmeibc_map_each_sg_entry = false;
module_param_named(map_each_sg_entry, nvmeibc_map_each_sg_entry, bool, 0644);
MODULE_PARM_DESC(map_each_sg_entry, "IB DMA map each sg-entry separately");

uint nvmeibc_map_sg_mode = NVMEIB_DEBUG_RDMA_CORRUPTION ? MAP_SG_MR_REG_WR_ONLY : MAP_SG_MR_COMBINED;
module_param_named(map_sg_mode, nvmeibc_map_sg_mode, uint, 0644);
MODULE_PARM_DESC(map_sg_mode, "Mapping data SG list modes:\n"
							  "\t\t 0 : Combined, use global key when able to collapse all sg-entries to one, otherwise map-mr (map WR and rdma-write WR)\n"
							  "\t\t 1 : Use only map-mr (IB_WR_REG_MR, IB_WR_FAST_REG_MR)\n"
							  "\t\t 2 : Use only global dma key, may use multiple rdma-write WRs from clnt/srv in wr/rd, respectively. Target must have enough WRs to write back on read-op");

uint nvmeibc_map_sg_result_trace = 0;
module_param_named(map_sg_result_trace, nvmeibc_map_sg_result_trace, uint, 0644);
MODULE_PARM_DESC(map_sg_result_trace, "Trace result of map data SG list: "
									  "0: Disabled, 1: One-shot (edge), 2: Continuous (level)");

bool nvmeibc_ib_net_complete_iocmd_use_pcpu_wq = false;
module_param_named(ib_net_complete_iocmd_use_pcpu_wq, nvmeibc_ib_net_complete_iocmd_use_pcpu_wq, bool, 0444);
MODULE_PARM_DESC(ib_net_complete_iocmd_use_pcpu_wq, "IB net complete iocmd use pcpu wq for requests");

uint nvmeibc_max_notify_cq_iterations = 10;
module_param_named(max_notify_cq_iterations, nvmeibc_max_notify_cq_iterations, uint, 0644);
MODULE_PARM_DESC(max_notify_cq_iterations, "Maximum number of iterations to arm cq");

#define __FIN FINS(net ? net->ioch->name : "?")
#define __FOUT FOUTS(net ? net->ioch->name : "?")
#define __IFIN IFINS(net ? net->ioch->name : "?")
#define __IFOUT IFOUTS(net ? net->ioch->name : "?")

#define __NFIN NFINS(net ? net->ioch->name : "?")
#define __NFOUT NFOUTS(net ? net->ioch->name : "?")

#undef _NIn
#undef _NWn
#undef _NTn
#undef _NEn
#undef _NDn

#define _NFn(__name, _net_, fmt, ...) \
		_NF(__name, "net @NET (qpn:@QPN, @IOCH_NAME): " fmt, \
		_net_, _net_->qp ? _net_->qp->qp_num : 0, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>", ## __VA_ARGS__)
#define _NDn(__name, _net_, fmt, ...) \
		_ND(__name, "net @NET (qpn:@QPN, @IOCH_NAME): " fmt, \
		_net_, _net_->qp ? _net_->qp->qp_num : 0, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>", ## __VA_ARGS__)
#define _NTn(__name, _net_, fmt, ...) \
		_NT(__name, "net @NET (qpn:@QPN, @IOCH_NAME): " fmt, \
		_net_, _net_->qp ? _net_->qp->qp_num : 0, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>", ## __VA_ARGS__)
#define _NIn(__name, _net_, fmt, ...) \
		_NI(__name, "net @NET (qpn:@QPN, @IOCH_NAME): " fmt, \
		_net_, _net_->qp ? _net_->qp->qp_num : 0, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>", ## __VA_ARGS__)
#define _NWn(__name, _net_, fmt, ...) \
		_NW(__name, "net @NET (qpn:@QPN, @IOCH_NAME): " fmt, \
		_net_, _net_->qp ? _net_->qp->qp_num : 0, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>", ## __VA_ARGS__)
#define _NEn(__name, _net_, fmt, ...) \
		_NE(__name, "net @NET (qpn:@QPN, @IOCH_NAME): " fmt, \
		_net_, _net_->qp ? _net_->qp->qp_num : 0, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>", ## __VA_ARGS__)

#define _NEn_dmesg(__name_dp_dbg_tools, _net_, fmt, ...) _NE_to_user(__name_dp_dbg_tools, "net @NET",  "(qpn:@QPN, @IOCH_NAME): " fmt, \
		_net_, _net_->qp ? _net_->qp->qp_num : 0, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>", ## __VA_ARGS__)

/**
 * This function rearms send-CQ interrupts.
 * Main use cases are right before post-send where:
 * (1) no context is expected to poll send cq (e.g. recv-path) and/or;
 * (2) caller waits for send-completion status assuming next comp is his
 *
 * Current code-flow does not require rearming send-cq under IO
 */
void nvmeibc_ib_net_req_notify_send_cq(struct nvmeibc_ib_net *net)
{
	__NFIN;
	//BUG_ON(!nvmeibc_channel_already_locked(net->ioch));
	if (!nvmeibc_use_pcpu_cq) {
		BUG_ON(net->qp->send_cq != net->send_cq);
		ib_req_notify_cq(net->send_cq, IB_CQ_NEXT_COMP);
	}
	else {
//		_NEn(nvmeibc_ib_net_req_notify_send_cq_e1,
//			net, "OOPS, attempt rearm interrupts of shared percpu cq "
//			"from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
//			__builtin_return_address(0));
//		WARN_ON_ONCE(1);
	}
	__NFOUT;
}

static void ib_net_disconnect_smp_call_fn(void *ctx)
{
	struct nvmeibc_ib_net *net = ctx;
	int state;

	__NFIN;

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	do {
		void *val;
		if ((val = nvmeib_c_tree_lookup((unsigned long)net)) != net) {
			_NE(nvmeibc_ib_net_req_notify_send_cq_e1,
			"OOPS, net=@PTR (val=@PTR) was deleted", net, val);
			return;
		}
	} while(0);
#endif

	if ((state = nvmeib_get_state_guard(&net->state)) ==
		NVMEIBC_IB_NET_LIVE) {
		_NTn(trace_ib_net_disconnect_smp_call_fn, net, "initiate disconnect");
		nvmeibc_ib_net_disconnect(net);
	}

	__NFOUT;
}

static void qp_event(struct ib_event *event, void *net_ptr)
{
	struct nvmeibc_ib_net *net = net_ptr;
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	void *val;
#endif
	unsigned long flags;
	int state;

	__NFIN;

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	if ((val = nvmeib_c_tree_lookup((unsigned long)net)) != net) {
		_NE(nvmeibc_ib_net_req_notify_send_cq_e1,
			"OOPS, net=@PTR (val=@PTR) was deleted, event=@INT",
			net, val, (int)event->event);
		goto out;
	}
#endif

	_NT(trace_ib_net_qp_event, "QP event @EVENT on net @NET, cm_id=@CM_ID qpn=@QPN state=@STATE, ch @IOCH_NAME",
		event->event,
		net, net->cm_id, net->qp->qp_num,
		nvmeib_get_state_guard(&net->state), net->ioch->name);

	switch (event->event) {
	case IB_EVENT_QP_LAST_WQE_REACHED:
		_NT(trace_1_ib_net_qp_event, "Received qp LAST_WQE_REACHED (#@QP_EVT_LAST_WQE)", net->qp_evt_last_wqe);
		net->qp_evt_last_wqe++;
		break;
	case IB_EVENT_QP_FATAL:
		_NTn(trace_2_ib_net_qp_event, net, "Received QP_FATAL (#@QP_EVT_QP_FATAL), terminating connection",
		   net->qp_evt_qp_fatal);
		net->qp_evt_qp_fatal++;
		if (nvmeibc_channel_is_ll_pcpu_ch(net->ioch)) {
			/* Per-CPU Channel => Trigger IPI to call nvmeibc_ib_net_disconnect on correct CPU (Question: Do we need to wait?) */
			smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(net->ioch), ib_net_disconnect_smp_call_fn, net, false);
		} else {
			if ((state = nvmeib_get_state_guard(&net->state)) ==
				NVMEIBC_IB_NET_LIVE) {
				_NTn(trace_3_ib_net_qp_event, net, "initiate disconnect");
				nvmeibc_ib_net_disconnect(net);
			}
		}
		break;
	case IB_EVENT_QP_ACCESS_ERR:
		_NTn(trace_5_ib_net_qp_event, net, "Received QP_ACCESS_ERR, terminating connection");
		if (nvmeibc_channel_is_ll_pcpu_ch(net->ioch)) {
			/* Per-CPU Channel => Trigger IPI to call nvmeibc_ib_net_disconnect on correct CPU (Question: Do we need to wait?) */
			smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(net->ioch), ib_net_disconnect_smp_call_fn, net, false);
		} else {
			if ((state = nvmeib_get_state_guard(&net->state)) ==
				NVMEIBC_IB_NET_LIVE) {
				_NTn(trace_6_ib_net_qp_event, net, "initiate disconnect");
				nvmeibc_ib_net_disconnect(net);
			}
		}
		break;
	case IB_EVENT_SQ_DRAINED:
		_NT(trace_4_ib_net_qp_event, "Received SQ_DRAINED");
		break;
	default:
		_NEn(trace_0_ib_net_qp_event, net,
			 "Unhandled QP event @EVENT, state=@STATE",
			event->event, nvmeib_get_state_guard(&net->state));
		break;
	}

	spin_lock_irqsave(&net->comp_guard, flags);
	if (net->break_qp)
		complete(net->break_qp);
	spin_unlock_irqrestore(&net->comp_guard, flags);

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
out:
#endif

	__NFOUT;
}

static void c_rdma_qp_event(struct ib_event *event, void *context)
{
	struct nvmeib_rdma_evt_ctx *c = context;
	struct nvmeibc_ib_net *net;

	net = nvmeib_rdma_evt_ctx_get(c);
	if (net) {
		qp_event(event, net);
		nvmeib_rdma_evt_ctx_put(c);
	}
	else {
		_NT(c_rdma_qp_event_t1,
			"Fail to get ctx @PTR (evt=@INT)", c, event->event);
	}
}

void nvmeibc_ib_net_destroy_cm(struct nvmeibc_ib_net *net)
{
	__NFIN;
	_NT(trace_ib_net_nvmeibc_ib_net_destroy_cm, "net->cm_id = @CM_ID", net->cm_id);
	if (net->cm_id) {
		nvmeib_rdma_destroy_cm(net->cm_id);
		net->cm_id = NULL;
	}
	_ND(trace_1_ib_net_nvmeibc_ib_net_destroy_cm, "After");
	__NFOUT;
}

void rcq_kthread_stop(struct nvmeibc_ib_net *net);
void scq_kthread_stop(struct nvmeibc_ib_net *net);

static inline void post_wc_r_range_srq(struct nvmeibc_ib_net *net, struct ib_wc *p_wc, int s, int e)
{
	struct nvmeib_iu *iu;
	int index;
	int ii;
	int rv;

	__NFIN;
	for (ii = s; ii < e; ii++) {

		if (net->shared_cq) {
			struct ib_wc *wc = &p_wc[ii];
			u32 wr_opcode = nvmeib_opcode_from_wc(wc);
			if ((u16)wr_opcode != NVMEIB_RECV) {
				_ND(trace_ib_net_post_wc_r_range_srq, "wr_opcode=@WR_OPCODE, not returning to SRQ", wr_opcode);
				rv = 0;
				continue;
			}
		}

		index = nvmeib_idx_from_wc(&p_wc[ii]);
		iu = nvmeib_srq_rtrv_recv(net->srq_info, index, net);
		rv = nvmeib_srq_post_recv(net->srq_info, iu);
		if (rv)
			_NT(error_ib_net_post_wc_r_range_srq, "Failed post-recv, error code @RV", rv);
	}
	__NFOUT;
}

static inline void rq_drain_comp(struct nvmeibc_ib_net *net, struct ib_wc *wc)
{
	__NFIN;
	if (wc->status != IB_WC_SUCCESS && wc->status != IB_WC_WR_FLUSH_ERR)
		_NE(error_ib_net_rq_drain_comp, "Flush WR received with invalid status @STATUS", wc->status);
	if (wc->opcode & IB_WC_RECV) {
		_NTn(trace_ib_net_rq_drain_comp, net, "recv_q drain received");
		net->qp_rq_drain_recv++;
	}
	if (nvmeibc_ib_net_drained(net)) {
		unsigned long flags;
		spin_lock_irqsave(&net->comp_guard, flags);
		if (net->break_qp)
			complete(net->break_qp);
		spin_unlock_irqrestore(&net->comp_guard, flags);
	}
	__NFOUT;
}

static inline void drain_recv_wcs(struct nvmeibc_ib_net *net, struct ib_wc *wc, int s, int e, bool *got_drain)
{
	__NFIN;
	if (net->srq_info) {
		/* Return the drained recv_ioctx to the SRQ */
		post_wc_r_range_srq(net, wc, s, e);
	}
	else if (net->recv_q) {
		int i;
		for (i = s; i < e; i++) {
			int rq_post_count = atomic_dec_return(&net->rq_post_count);
			_NDn(trace_ib_net_drain_recv_wcs, net, "rq_post_count @RQ_POST_COUNT", rq_post_count);
			BUG_ON(rq_post_count < 0);
			if (nvmeib_opcode_from_wc(&wc[i]) == NVMEIB_DRAIN_QUEUE) {
				/* In case of shared RCQ, drain may be for another net */
				struct nvmeibc_ib_net *drain_net = wc[i].qp->qp_context;
				_NTn(trace_1_ib_net_drain_recv_wcs, drain_net, "Got Drain for QP @QP_NUM on RCQ @RECV_Q",
					drain_net->qp->qp_num, net->recv_q);
				rq_drain_comp(drain_net, &wc[i]);
				if (got_drain)
					*got_drain = drain_net == net;
			}
		}
	}
	__NFOUT;
}

static inline void drain_send_cq(struct nvmeibc_ib_net *net)
{
	DECLARE_COMPLETION_ONSTACK(done);
	unsigned long flags;
	int rv;
	int n;
	__NFIN;

	if (!nvmeibc_use_pcpu_cq) {
		/* rearm needed although qp is in error-state */
	BUG_ON(net->qp->send_cq != net->send_cq);
		ib_req_notify_cq(net->send_cq, IB_CQ_NEXT_COMP);
	}

	/* let any following send-comps rearm notifications and
	   ensures no more post-send by send/recv comp handlers */
	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	net->drain_sq_done = &done;
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	/* send special last-wqe and wait for its completion */
	_NTn( drain_send_cq_t1,
		net,"post-send drain-wr...");
	while ((rv = nvmeib_post_sq_drain(net->qp)) == -ENOMEM) {
		_NTn(trace_ib_net_drain_send_cq, net,"scq overflow");
		if (!nvmeibc_use_pcpu_cq) {
			rv = nvmeibc_ib_net_process_send_cq(net, 1);
			_NTn(trace_1_ib_net_drain_send_cq, net,"poll-scq was @TRUE_FALSE_STR (@RV)", rv == 1 ? "OK" : "Err", rv);
		BUG_ON(net->qp->send_cq != net->send_cq);
			ib_req_notify_cq(net->send_cq, IB_CQ_NEXT_COMP);
		}
		else {
			msleep(1);
		}
	}
	if (!rv) {
		n = 0;
		while ((n < NVMEIB_N_WAIT_DRAIN_QP) &&
			   (rv = wait_for_completion_interruptible_timeout(&done,
				   NVMEIB_WAIT_DRAIN_SQ)) <= 0) {
			_NEn(trace_2_ib_net_drain_send_cq, net,
				 "Fail wait drain-sq (rv @RV) attempt #@NUM_RETRY_ATTEMPTS, "
				 "n_send_intrs=@LLU, n_recv_intrs=@LLU, intr_vec=@INT, dev=@DEV_NAME, poll-cq...",
				 rv, n, net->n_send_intrs, net->n_recv_intrs, net->send_intr_vec, P2NV(net->port)->ib_dev->name);

			/* Debug "Fail to drain SQ"... */
			{
				DECLARE_IB_WC_ONSTACK(wc);
				int p = 0;
				int i = 0;
				do {
					p = ib_poll_cq(net->send_cq, 1 , &wc);
					_NEn(trace_3_ib_net_drain_send_cq, net,
						 "[@INT] poll-cq returned @INT wc ", i, p);
					i++;
				} while (p);
			}

			n++;
		}
	}
	else
		_NEn(error_ib_net_drain_send_cq, net,"Fail to send drain-sq wr (@RV)", rv);

	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	net->drain_sq_done = NULL;
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	__NFOUT;
}

static inline void drain_private_cqs(struct nvmeibc_ib_net *net)
{
	int n, i;

	__NFIN;

	if (net->recv_cq) {
		DECLARE_IB_WC_ONSTACK(wc_drain);
		BUG_ON(net->qp->recv_cq != net->recv_cq);
		while ((n = ib_poll_cq(net->recv_cq,
			1 /*net->n_wc_r*/, &wc_drain /*net->wc_r*/)) > 0) {

			_NEn(trace_0_ib_net_drain_cqs, net,
				 "Drain recv cq: n_send_intrs=@LLU, n_recv_intrs=@LLU, intr_vec=@INT, dev=@DEV_NAME, poll-cq...",
				 net->n_send_intrs, net->n_recv_intrs, net->recv_intr_vec, P2NV(net->port)->ib_dev->name);

			_NT(trace_ib_net_drain_cqs, "Draining @COUNT recv comps...", n);
			for (i = 0; i < n ; i++) {
				struct ib_wc *wc = &wc_drain; //net->wc_r[i];
				u32 wr_opcode = nvmeib_opcode_from_wc(wc);
				_NEn(trace_1_ib_net_drain_cqs, net,
					"wc_r[@INT], wc-opcode=@WC_OPCODE, wr-opcode=@WR_OPCODE",
					i, wc->opcode, wr_opcode);
			}
			drain_recv_wcs(net, &wc_drain, 0, n, NULL);
		}
	}
	if (!net->shared_cq) {
		drain_send_cq(net);
	}

	__NFOUT;
}

static inline void drain_per_dev_cq(struct nvmeibc_ib_net *net)
{
	drain_send_cq(net);
}

static inline void drain_cqs(struct nvmeibc_ib_net *net)
{
	__NFIN;

	if (!nvmeibc_use_pcpu_cq)
		drain_private_cqs(net);
	else
		drain_per_dev_cq(net);

	__NFOUT;
}

bool nvmeibc_ib_net_drained(struct nvmeibc_ib_net *net)
{
	enum nvmeib_rdma_expect_last_wqe expect_last_wqe = nvmeib_rdma_expect_last_wqe(net->cm_id);

	if (expect_last_wqe == NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN) {
		_NWn(nvmeibc_ib_net_drained_last_wqe_unknown, net,
		     "expect_last_wqe=@EXPECT_LAST_WQE, qp_evt_last_wqe=@INT", expect_last_wqe, net->qp_evt_last_wqe);
		WARN_KNOWN(1, 731);
	} else
		_NTn(nvmeibc_ib_net_drained, net, "expect_last_wqe=@EXPECT_LAST_WQE, qp_evt_last_wqe=@INT",
			expect_last_wqe, net->qp_evt_last_wqe);

	if (expect_last_wqe == NVMEIB_RDMA_EXPECT_LAST_WQE && net->qp_evt_last_wqe == 0)
		return false;
	else if (net->recv_q) {
		if (!net->qp_rq_drain_recv)
			return false;
	}
	return true;
}
static void defer_recv_intr_wq_drain(struct nvmeibc_ib_net *net)
{
	bool stopped_defer = false;
	int n = 0;

	while (!stopped_defer) {
		switch (nvmeib_get_state_guard(&net->defer_recv_state)) {
		case NVMEIBC_IB_NET_DEFER_RECV_DISABLED:
			stopped_defer = true;
			break;
		case NVMEIBC_IB_NET_DEFER_RECV_IDLE:
			if (nvmeib_switch_state_guard(&net->defer_recv_state,
				NVMEIBC_IB_NET_DEFER_RECV_IDLE, NVMEIBC_IB_NET_DEFER_RECV_DISABLED))
				stopped_defer = true;
			break;
		case NVMEIBC_IB_NET_DEFER_RECV_SCHEDULED:
			if (!(n & 0x1FFFF)) { /* consider udelay -> print every ~1sec */
				if (net->defer_recv_intr_wq) {
					_NTn(trace_defer_recv_intr_wq_drain_0, net,
						 "wait for SCHED->RUN before drain (pid=@K_PID, n=@INT)",
						 wq_pid(net->defer_recv_intr_wq), n);
				} else {
					_NTn(trace_defer_recv_intr_wq_drain_0x, net,
						 "wait for SCHED->RUN before drain (kwq, n=@INT)", n);
				}
				/* otherwise we may wq-drain before intr-ctx adds the work */
			}
			udelay((1 << 3));
			n++;
			break;
		case NVMEIBC_IB_NET_DEFER_RECV_RUNNING:
			if (nvmeib_switch_state_guard(&net->defer_recv_state,
				NVMEIBC_IB_NET_DEFER_RECV_RUNNING, NVMEIBC_IB_NET_DEFER_RECV_TERMINATING)) {
				if (net->defer_recv_intr_kwq) {
					/* Cancel kernel workqueue work */
					_NTn(trace_defer_recv_intr_wq_drain_1, net,
						 "canceling defer_recv_intr_kwq work");
					nvmeib_public_cancel_work_sync(&net->defer_recv_kwork);
				} else if (net->defer_recv_intr_wq) {
					_NTn(trace_defer_recv_intr_wq_drain_1x, net,
						 "draining defer_recv_intr_wq pid @K_PID", wq_pid(net->defer_recv_intr_wq));
					wq_drain(net->defer_recv_intr_wq);
				}
				stopped_defer = true;
			}
			break;
		}
	}

	BUG_ON(nvmeib_get_state_guard(&net->defer_recv_state) != NVMEIBC_IB_NET_DEFER_RECV_DISABLED);
}

static void detach_from_per_dev_cq(struct nvmeibc_ib_net *net)
{
	if (net->rdma_e_ctx) {
		/* 1) stop forwarding new cm/qp events to net-layer and wait
			  for in-progress events to complete before REUSing c-net
		   2) call this BEFORE nvmeib_cq_qp_del() which asynchronously
			  (later) calls nvmeib_cq_qp_destroy_action_f which kfrees
			  net->rdma_e_ctx */
		nvmeib_rdma_evt_ctx_stop(net->rdma_e_ctx);
		net->rdma_e_ctx = NULL;
	}
	if (net->dev_cq) {
		/* detach from CQ:
		   stop forwaring polled wc to net-layer and wait
		   for already polled WCs' processing to complete */
		nvmeib_cq_qp_stop(net->dev_cq, (u64)net->qp);
		nvmeib_cq_qp_del(net->dev_cq, (u64)net->qp, &net->qp_action_list);
		BUG_ON(!list_empty(&net->qp_action_list));
		nvmeib_cq_put(P2NV(net->port), net->dev_cq,
					  net->ioch ? net->ioch->ct : ct_other);
		net->cm_id = NULL;
		net->qp = NULL;
		net->dev_cq = NULL;
		net->send_cq = NULL;
		net->recv_cq = NULL;
		net->srq_info = NULL;
	}
}

void nvmeibc_ib_net_break_qp(struct nvmeibc_ib_net *net)
{
	struct completion *break_qp;
	unsigned long flags;
	bool need_drep;
	int state;
	int rv, rvw, n = 0;
	__NFIN;

	_NTn(trace_ib_net_nvmeibc_ib_net_break_qp, net, "release - attempt start");
	if ((state = nvmeib_get_state_guard(&net->state))
		!= NVMEIBC_IB_NET_DISCONNECTING) {
		_NT(error_ib_net_nvmeibc_ib_net_break_qp, "break-qp called on net = @NET in invalid state @STATE",
		 net, state);
		return;
		//BUG();
	}

	_NTn(trace_1_ib_net_nvmeibc_ib_net_break_qp, net, "release - start");
	if (!nvmeibc_use_pcpu_cq) {
		scq_kthread_stop(net);
		rcq_kthread_stop(net);
		defer_recv_intr_wq_drain(net);
	}

	//if this is the first time that this qp breaks the comletion must be NULL
	BUG_ON(net->break_qp != NULL);

	if (nvmeib_get_state_guard(&net->state) != NVMEIBC_IB_NET_INIT || net->recv_q) {
		break_qp = kzalloc(sizeof(struct completion), GFP_KERNEL);
		if (break_qp)
			init_completion(break_qp);
		else
			_NW(warn_ib_net_nvmeibc_ib_net_break_qp, "Unable to allocate break_qp completion on net");
	}
	else {
		_NTn(trace_2_ib_net_nvmeibc_ib_net_break_qp, net, "state NET_INIT, skip qp-err-sts and wait-last-wqe");
		break_qp = NULL;
	}

	_ND(trace_4_ib_net_nvmeibc_ib_net_break_qp, "Will nvmeib_rdma_disconnect net=@NET cm_id=@CM_ID qp=@QP",
		net, net->cm_id, net->qp);
	spin_lock_irqsave(&net->comp_guard, flags);
	if (break_qp)
		net->break_qp = break_qp;
	spin_unlock_irqrestore(&net->comp_guard, flags);
	if ((rv =  nvmeib_rdma_disconnect(net->cm_id, net->qp, &need_drep)) < 0)
		_NE(trace_5_ib_net_nvmeibc_ib_net_break_qp,
			"net @NET: Failed rdma disconnect (rv @RV)", net, rv);
	else {
		if (!nvmeibc_use_pcpu_cq) {
			if (break_qp) {
				/* Make sure the dying flag is set in case we got here via the channel layer */
				atomic_inc(&net->dying);
				if (net->recv_q) {
					/* In case the recv was in polling mode when we terminated the polling thread */
					BUG_ON(net->qp->recv_cq != net->recv_cq);
					ib_req_notify_cq(net->recv_cq, IB_CQ_NEXT_COMP);
					/* Post the drain wqe to the receive queue */
					nvmeib_post_rq_drain(net->qp);
				}
				_NTn(trace_6_ib_net_nvmeibc_ib_net_break_qp, net, "Waiting break_qp. dying @ATOMIC_READ qp_rq_drain_recv @QP_RQ_DRAIN_RECV rq_posted_count @ATOMIC_READ srq @SRQ_INFO recv_q @RECV_Q rcq_poll_mode: @RCQ_POLL_MODE, drained=@DRAINED, qp_fatal=@QP_FATAL",
					atomic_read(&net->dying), net->qp_rq_drain_recv, atomic_read(&net->rq_post_count), net->srq_info, net->recv_q, net->rcq_poll_mode,
					nvmeibc_ib_net_drained(net), net->qp_evt_qp_fatal);
				while (!nvmeibc_ib_net_drained(net) && !net->qp_evt_qp_fatal &&
						((rvw = wait_for_completion_interruptible_timeout(break_qp,
					NVMEIB_WAIT_BREAK_QP)) <= 0) && n < NVMEIB_N_WAIT_BREAK_QP) {
					_NWn(trace_7_ib_net_nvmeibc_ib_net_break_qp, net, "Failed waiting for LAST WQE (rvw @RVW) / RQ Flush attempt #@NUM_RETRY_ATTEMPTS, "
						"state @STATE_GUARD, cm_id @CM_ID, dying @ATOMIC_READ qp_rq_drain_recv @QP_RQ_DRAIN_RECV rq_posted_count @ATOMIC_READ srq @SRQ_INFO recv_q @RECV_Q rcq_poll_mode: @RCQ_POLL_MODE", rvw, n,
						nvmeib_get_state_guard(&net->state), net->cm_id,
						atomic_read(&net->dying), net->qp_rq_drain_recv, atomic_read(&net->rq_post_count), net->srq_info, net->recv_q, net->rcq_poll_mode);

					WARN_ON_ONCE(true);
					n++;
				}
				drain_cqs(net);
			}
		}
		else {
			_NT(t0_nvmeibc_break_qp_pcpu_cq,
			    "SCQ: net=@NET, dev_cq=@DEV_CQ, qp=@QP, qp_num=@QP_NUM", net, net->dev_cq, net->qp, net->qp->qp_num);
			detach_from_per_dev_cq(net);
		}
		_ND(trace_8_ib_net_nvmeibc_ib_net_break_qp, "Done waiting break_qp");
		if (need_drep) {
			_NTn(trace_9_ib_net_nvmeibc_ib_net_break_qp, net, "Waiting drep");
			if ((rvw = wait_for_completion_interruptible_timeout(
					&net->wait_for_drep, NVMEIB_WAIT_DREP_TIMEOUT(P2NV(net->port)->dev_type))) <= 0) {
				_NWn(warn_ib_net_nvmeibc_ib_net_break_qp_fail_wait_drep, net, "Failed (@RV) waiting for drep", rvw);
			} else
				_NTn(trace_10_ib_net_nvmeibc_ib_net_break_qp, net, "Done waiting drep");
		}
	}

	spin_lock_irqsave(&net->comp_guard, flags);
	net->break_qp = NULL;
	spin_unlock_irqrestore(&net->comp_guard, flags);
	kfree(break_qp);
	_NTn(trace_11_ib_net_nvmeibc_ib_net_break_qp, net, "release - done");
	__NFOUT;
}

void nvmeibc_ib_net_disconnect_(struct nvmeibc_ib_net *net)
{
	int dying, state;
	__NFIN;
	BUG_ON(!nvmeibc_channel_already_locked(net->ioch));
	/* this function is called only when we initiate the disconenct. */

	dying = atomic_inc_return(&net->dying);
	_NTn(trace_ib_net_nvmeibc_ib_net_disconnect_, net, "inc dying to @DYING", dying);
	if (dying == 1) {
		state = nvmeib_get_state_guard(&net->state);
		if (state == NVMEIBC_IB_NET_LIVE ||
			state == NVMEIBC_IB_NET_CONNECTING) {
			nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_DISCONNECTING);
			if (net->on_disconnect) {
				_NTn(trace_1_ib_net_nvmeibc_ib_net_disconnect_, net, "initiate disconnect (state was @STATE)", state);
				/* on_disconnect_admin_ch
				on_disconnect_lock_ch
				on_disconnect_io_ch
				on_disconnect_nordda_ch */
				net->on_disconnect(net);
			}
		}
	}
	__NFOUT;
}

enum {
	REQ_NOTIFY_FALSE = 0,
	REQ_NOTIFY_TRUE  = 1,
};

/* -------------------------------------------------------------------------- *
 *                           net stats                                        *
 * -------------------------------------------------------------------------- */
#ifdef DEBUG_CLNT_NET_STATS

/* Net stats columns' width */
#define NET_STATS_CW_ID		(4)
#define NET_STATS_CW_NAME	(64)
#define NET_STATS_CW_DIR	(6)
void nvmeibc_ib_net_stats_columns(char *buf, size_t len, ssize_t *pcount)
{
	ssize_t count = *pcount;
	count += scnprintf(
		buf+count, len-count,
		"%-*s%-*s%-*s %-16s %-16s %-16s %-16s %-16s %-16s %-16s\n",
		NET_STATS_CW_ID, "ID",
		NET_STATS_CW_NAME, "Name",
		NET_STATS_CW_DIR, "Dir",
		"|Total CQEs", "|n_intr", "|n_poll", "|n_wakeups", "|n_external",
		"|max_intr_dur", "|n_spu_intr");
	*pcount = count;
}


void nvmeibc_ib_net_stats_fill_buf(struct nvmeibc_ib_net *net, int id,
								   char *buf, size_t len, ssize_t *pcount)
{
	struct cq_stats *cq_stats;
	ssize_t count = *pcount;
	int	off;

	__NFIN;

	/* line 1: net-id , name and recv stats */
	count += scnprintf(buf+count, len-count, "%-*d", NET_STATS_CW_ID, id);
	count += scnprintf(buf+count, len-count, "%-*s", NET_STATS_CW_NAME,
					   net->ioch ? net->ioch->name : "<UNKNOWN>");
	count += scnprintf(buf+count, len-count, "%-*s", NET_STATS_CW_DIR, "recv:");
	cq_stats = &net->rcq_stats;
	count += scnprintf(
		buf+count, len-count,
		"  %-16lld %-16lld %-16lld %-16lld %-16lld %-16lld %-16lld\n",
		cq_stats->n_intr + cq_stats->n_poll,
		cq_stats->n_intr, cq_stats->n_poll,
		cq_stats->n_wakeups, cq_stats->n_external,
		cq_stats->max_intr_duration,
		cq_stats->n_spurious_intr);

	/* line 2: net-ptr, net-state and send stats */
	off = scnprintf(buf+count, len-count, "%-*s(p=%p, state %d)",
					 NET_STATS_CW_ID, "",
					 net, nvmeib_get_state_guard(&net->state));
	count += off;
	count += scnprintf(buf+count, len-count, "%-*s",
					   NET_STATS_CW_ID + NET_STATS_CW_NAME - off, "");
	count += scnprintf(buf+count, len-count, "%-*s", NET_STATS_CW_DIR, "send:");
	cq_stats = &net->scq_stats;
	count += scnprintf(
		buf+count, len-count,
		"  %-16lld %-16lld %-16lld %-16lld %-16lld %-16lld %-16lld\n",
		cq_stats->n_intr + cq_stats->n_poll,
		cq_stats->n_intr, cq_stats->n_poll,
		cq_stats->n_wakeups, cq_stats->n_external,
		cq_stats->max_intr_duration,
		cq_stats->n_spurious_intr);

	*pcount = count;
	__NFOUT;
}
#endif /* DEBUG_CLNT_NET_STATS */

/* -------------------------------------------------------------------------- *
 *                           send-completions                                 *
 * -------------------------------------------------------------------------- */
/**
 * Note:
 * @n must be the same value as use in the preceeding call to ib-poll-cq()
 */
static inline int process_num_send_comps_(struct nvmeibc_ib_net *net, int n)
{
	int i, err = 0;

	__NFIN;
	BUG_ON(!nvmeibc_channel_already_locked(net->ioch));
	_NDn(trace_ib_net_process_num_send_comps, net, "process @COUNT comps", n);
	if (n > 0) {
		for (i = 0; i < n; ++i) {
			_NDn(trace_1_ib_net_process_num_send_comps, net, "comps i=@INDEX, wr_id=@WR_ID_INT", i,
				nvmeib_opcode_from_wc(&net->wc_s[i]));

			if (nvmeib_opcode_from_wc(&net->wc_s[i]) == NVMEIB_DRAIN_QUEUE) {
				/* In case of shared SCQ, drain may not be for this net's QP */
				if (!net->drain_sq_done)
					_NWn(warn_ib_net_process_num_send_comps, net, "No drain-sq comp-item");
				else {
					_NTn(trace_2_ib_net_process_num_send_comps, net, "drain-sq done");
					complete(net->drain_sq_done);
				}
				err = 1;
				break;
			}

			if (!atomic_read(&net->dying) &&
				net->call_send_comp_handler(
				net, &net->wc_s[i], i == (n-1)) < 0) {
				_NEn(error_ib_net_process_num_send_comps, net, "send-comp-handling error, disconnect net");
				nvmeibc_ib_net_disconnect_(net);
				err = 1;
			}
			else if (net->wc_s[i].status != IB_WC_SUCCESS) {
				struct ib_wc *wc = &net->wc_s[i];
				enum ib_wc_status wc_status = wc->status;
				int index = nvmeib_idx_from_wc(wc);
				unsigned op = nvmeib_opcode_from_wc(wc);
				_NTn(ttt_process_num_send_comps_, net,
					"send-comp w/ err:"
					"index=@INDEX, opcode=@OPCODE, status=@WC_STATUS",
					index, op, wc_status);
			}
		}

	}

	__NFOUT;
	return (!err && !atomic_read(&net->dying)) ? n : -1;
}

#if 0
/* former version of send-comp handling, prior to polling-kthread support */
static void check_send_completion_(struct nvmeibc_ib_net *net)
{
	bool arm = net->rearm_send_cq, cont = true;
	int n;

	// __NFIN;
	BUG_ON(net == NULL);
	BUG_ON(!nvmeibc_channel_already_locked(net->ioch)); /* check_send_completion_ */
	if (arm) {
		_NDn(check_send_completion__d1000, net, "rearm notifications");
		ib_req_notify_cq(net->send_cq, IB_CQ_NEXT_COMP);
	}
	do {
		if ((n = ib_poll_cq(
			net->send_cq, net->n_wc_s, net->wc_s)) > 0) {
				if (process_num_send_comps_(net, n) < 0) {
					cont = false;
					break;
				}
		}
		else if (n < 0)
			nvmeibc_ib_net_disconnect_(net, false, true);
	}
	while ((!arm && cont && n > 0) || (net->poll_interrupts && n > 0));
	// __NFOUT;
}
#endif

static int process_send_cq_(struct nvmeibc_ib_net *net, int ne)
{
	int n;

	__NFIN;

	BUG_ON(!nvmeibc_channel_already_locked(net->ioch)); /* process_send_cq_ */

	/* poll & process completions */
	BUG_ON(net->qp->send_cq != net->send_cq);
	BUG_ON(ne > net->n_wc_s);

	n = ib_poll_cq(net->send_cq, ne, net->wc_s);
	if (n > 0) {
		nvmeib_qp_stats_on_poll_cq(net->qp_stats, n, send);
		if ((n = process_num_send_comps_(net, n)) < 0)
			_NTn(error_ib_net_process_send_cq, net, "comp processing error, disconnect net");
	}
	else if (n < 0) {
		_NEn(error_1_ib_net_process_send_cq, net, "Failed poll-cq (@RV), disconnect net", n);
		nvmeibc_ib_net_disconnect_(net);
	}
	else {
		nvmeib_qp_stats_on_poll_cq_empty(net->qp_stats);
	}

	__NFOUT;
	return n;
}

/**
 * Poll @n CQEs from send-cq and process them.
 */
static int process_send_cq(struct nvmeibc_ib_net *net, int n)
{
	unsigned long flags = 0;
	bool already_locked = true;
	int rv = -1;

	__NFIN;

	if (!(already_locked = nvmeibc_channel_already_locked(net->ioch)))
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);

	//Let SCQ drain
	//if (!atomic_read(&net->dying)) {
	if (unlikely(n > net->n_wc_s))
		_NW(warn_ib_net_nvmeibc_ib_net_process_send_cq, "@COUNT exceeds q-size (@N_WC_S)", n, net->n_wc_s);
	else {
		if (!(net->scq_kthread && net->scq_kwq) || net->drain_sq_done) {
			rv = process_send_cq_(net, n);
			net->scq_stats.n_external += (rv > 0) ? rv : 0;
		}
		else {
			_NE(error_ib_net_nvmeibc_ib_net_process_send_cq, "unhandled case: called for net with scq-polling-thread"
				"&& not while draining");
		}
	}
	//}

	if (!already_locked)
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	__NFOUT;
	return rv;
}

int nvmeibc_ib_net_process_send_cq(struct nvmeibc_ib_net *net, int n)
{
	int rv;
	__NFIN;

	if (!nvmeibc_use_pcpu_cq) {
		rv = process_send_cq(net, n);
	}
	else {
		_NEn(nvmeibc_ib_net_process_send_cq_e1,
			net, "OOPS, attempt process cq of shared percpu cq "
			"from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
			__builtin_return_address(0));
		WARN_ON_ONCE(1);
		rv = -EINVAL;
	}

	__NFOUT;
	return rv;
}

#if SCQ_OFFLOAD_TRACE
static inline void scq_offload_trace(struct nvmeibc_ib_net *net, int line)
{
	BUG_ON(!nvmeibc_channel_already_locked(net->ioch));

	net->scq_offload_trace.buf[net->scq_offload_trace.cnt] = line;
	net->scq_offload_trace.mode[net->scq_offload_trace.cnt] = net->scq_poll_mode;

	net->scq_offload_trace.cnt++;
	if (net->scq_offload_trace.cnt == SCQ_OFFLOAD_TRACE_SIZE) {
		net->scq_offload_trace.cnt = 0;
	}
}

static inline void scq_offload_tarce_print(struct nvmeibc_ib_net *net)
{
	int ii;
	BUG_ON(!nvmeibc_channel_already_locked(net->ioch));

	if (true || net->scq_offload_trace.printed++ & 0xFFFF) {

		ii = net->scq_offload_trace.cnt;
		do {
			_NIn(scq_offload_tarce_print_i1, net,
				"[@INT32_02] mode=@STR(@INT),@INT",
				ii,
				cq_poll_mode_to_str(net->scq_offload_trace.mode[ii]),
				net->scq_offload_trace.mode[ii],
				net->scq_offload_trace.buf[ii]);
			ii++;
			if (ii == SCQ_OFFLOAD_TRACE_SIZE)
				ii = 0;
		} while (ii != net->scq_offload_trace.cnt);
	}
}
#else
static inline void scq_offload_trace(struct nvmeibc_ib_net *net, int line)
{
}

static inline void scq_offload_tarce_print(struct nvmeibc_ib_net *net)
{
}
#endif /* SCQ_OFFLOAD_TRACE */

enum req_notify_state {
	REQ_NOTIFY_STATE_NONE = 0,
	REQ_NOTIFY_STATE_CQ_NOT_EMPTY = 1,
	REQ_NOTIFY_STATE_NOTIFY_FAILED = 2,
	REQ_NOTIFY_STATE_NOTIFY_OK = 3,
	REQ_NOTIFY_STATE_NOTIFY_MISSED_EVENTS = 4,
};

static int process_send_cq_offload_enb_(struct nvmeibc_ib_net *net, int ne,
	bool req_notify, bool in_interrupt, enum req_notify_state *req_notify_state);

static int polling_process_send_cq_(struct nvmeibc_ib_net *net, bool req_notify, bool *continue_polling, u64 *poll_time_ns)
{
       unsigned long flags = 0;
       int rv;
       enum req_notify_state req_notify_state;
	   u64 start_ns, busy_ns;

       __NFIN;

		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
		start_ns = nvmeib_public_local_clock();
		rv = process_send_cq_offload_enb_(net, net->n_wc_s, req_notify, false, &req_notify_state);
		busy_ns = nvmeib_public_local_clock() - start_ns;
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

		if (continue_polling) {
			*continue_polling = (req_notify && (
				req_notify_state == REQ_NOTIFY_STATE_NOTIFY_MISSED_EVENTS ||
				req_notify_state == REQ_NOTIFY_STATE_CQ_NOT_EMPTY)
			);
		}

		if (poll_time_ns) {
			*poll_time_ns = busy_ns;
		}

	   __NFOUT;
	   return rv;
}

/* Wrapper for queue_work that checks congestion and uses random CPU if congested */
static inline void scq_queue_work(struct nvmeibc_ib_net *net)
{
	queue_work(net->scq_kwq, &net->scq_kwork);
}

static void scq_kwork_func(struct work_struct *work)
{
	struct nvmeibc_ib_net *net = container_of(work, struct nvmeibc_ib_net, scq_kwork);
	unsigned long flags;
	bool continue_polling = false;
	u64 busy_ns;
	int n;

	__NFIN;

	if (!nvmeib_ref_get(&net->ib_rsrc_ref)) {
		_NTn(scq_kwork_func_t1, net, "failed to get ib_rsrc");
		goto out;
	}

	nvmeib_qp_stats_on_offth_iter(net->qp_stats);
	if (READ_ONCE(net->scq_poll_mode) == NVMEIBC_IB_CQ_INTR) {
		_NEn(error_ib_net_scq_kwork_func, net, "Oops, intr-polling off");
		BUG();
	}

	n = polling_process_send_cq_(net, REQ_NOTIFY_TRUE, &continue_polling, &busy_ns);
	continue_polling = continue_polling && nvmeib_intr_shaper_should_continue_polling(net->intr_shaper, n, busy_ns);

	if (!continue_polling) {
		_NDn(trace_2_ib_net_scq_kwork_func, net, "transition back to IRQ");

		/* Step 1: update poll-mode */
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
		if (net->scq_poll_mode == NVMEIBC_IB_CQ_POLLING) {
			scq_offload_trace(net, __LINE__);
			net->scq_poll_mode = NVMEIBC_IB_CQ_INTR;
		} else if (net->scq_poll_mode == NVMEIBC_IB_CQ_KEEP_POLLING) {
			scq_offload_trace(net, __LINE__);
			net->scq_poll_mode = NVMEIBC_IB_CQ_POLLING;
			continue_polling = true;
		} else BUG();
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);
	}

	if (continue_polling && !atomic_read(&net->dying)) {
		scq_queue_work(net);
	}

	nvmeib_ref_put(&net->ib_rsrc_ref);
out:
	__NFOUT;
}

static int scq_kthread_func(void *arg)
{
	struct nvmeibc_ib_net *net = arg;
	unsigned long flags;
	int n, tot = 0;
	u64 busy_ns;
	bool continue_polling;

	_NTn(trace_ib_net_scq_kthread_func, net, "scq thread ready");
	set_current_state(TASK_INTERRUPTIBLE);
	complete(&net->scq_kth_ready);
	schedule();
	_NTn(trace_1_ib_net_scq_kthread_func, net, "scq thread started");

	if (!nvmeib_ref_get(&net->ib_rsrc_ref)) {
		_NTn(scq_kthread_func_t1, net, "failed to get ib_rsrc");
		goto out;
	}
	while (!kthread_should_stop()) {
		continue_polling = false;
		nvmeib_qp_stats_on_offth_iter(net->qp_stats);
		if (READ_ONCE(net->scq_poll_mode) == NVMEIBC_IB_CQ_INTR) {
			_NEn(error_ib_net_scq_kthread_func, net, "Oops, intr-polling off");
			BUG();
		}

		//process_send_cq_offload_enb_
		n = polling_process_send_cq_(net, REQ_NOTIFY_FALSE, NULL, &busy_ns);
		if (n >= 0) {
			tot += n;
			continue_polling = n > 0 && nvmeib_intr_shaper_should_continue_polling(net->intr_shaper, n, busy_ns);
			if (continue_polling) {
				/* prevent soft lockup */
				if (kthread_should_stop())
					goto put_ref;
				cond_resched();
				continue;
			}
		}

		_NDn(trace_2_ib_net_scq_kthread_func, net, "transition back to IRQ");

		/* The correct sequence to prevent lost-wakeups is: 
		* 1. Set state for interrupt to see
		* 2. Set_current_state(TASK_INTERRUPTIBLE);
		* 3. Write Barrier
		* 4. Enable interrupts
		* 5. Read Barrier
		* 6. Check state if interrupt has changed it to polling mode
		* 7. Schedule if state has not changed, otherwise continue polling
		*/

		/* Step 1: update poll-mode */
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
		if (net->scq_poll_mode == NVMEIBC_IB_CQ_POLLING) {
			scq_offload_trace(net, __LINE__);
			net->scq_poll_mode = NVMEIBC_IB_CQ_INTR;
		} else if (net->scq_poll_mode == NVMEIBC_IB_CQ_KEEP_POLLING) {
			scq_offload_trace(net, __LINE__);
			net->scq_poll_mode = NVMEIBC_IB_CQ_POLLING;
			continue_polling = true;
		} else BUG();
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

		if (continue_polling) {
			_NDn(trace_5_ib_net_scq_kthread_func, net, "intr during transition to intr-mode, keep running");
			if (kthread_should_stop())
				goto put_ref;
			/* prevent soft lockup */
			cond_resched();
			continue;
		}

		/* Step 2: Set task state for interrupt to see */
		set_current_state(TASK_INTERRUPTIBLE);

		/* Step 3: Write Barrier */
		smp_mb();

		/* This step is only needed if the previous poll was successful */
		if (n >= 0) {
			/* Step 4: Enable interrupts using ib_req_notify_cq */
			n = polling_process_send_cq_(net, REQ_NOTIFY_TRUE, &continue_polling, NULL);
		}
	
		if (n >= 0) {
			tot += n;
			if (continue_polling) {
				/* We missed an event or didn't manage to empty the CQ, so we need to undo the transition and poll again */
				_NDn(trace_6_ib_net_scq_kthread_func, net, "failed to arm CQ, keep running");
				if (kthread_should_stop())
					goto put_ref;
				WRITE_ONCE(net->scq_poll_mode, NVMEIBC_IB_CQ_POLLING);
				set_current_state(TASK_RUNNING);
				smp_mb();
				cond_resched();
				continue;
			}
		}

		/* Step 5: Read Barrier */
		smp_mb();

		/* Step 6: Check state if interrupt has changed it to polling mode */
		if ((READ_ONCE(net->scq_poll_mode) != NVMEIBC_IB_CQ_INTR) && (n >= 0) &&
			!atomic_read(&net->dying)) 
		{
			_NDn(trace_3_ib_net_scq_kthread_func, net, "intr during transition to intr-mode, keep running");
			set_current_state(TASK_RUNNING);
			cond_resched(); /* prevent soft lockup */
			continue;
		}

		_NDn(trace_7_ib_net_scq_kthread_func, net, "transition back to IRQ after @N_TOTAL completions", tot);
		tot = 0;

		/* Step 7: Call schedule(), after checking to prevent lost wakeup from kthread-stop */
		if (!kthread_should_stop())
			/* if kthread-stop is called now, then:
			   if stop-bit set BUT before wakeup, thread sched-out for a while
			   if stop-bit set AND wakeup called, thread continue to run */
			schedule();
	}

put_ref:
	nvmeib_ref_put(&net->ib_rsrc_ref);
out:
	_NTn(trace_4_ib_net_scq_kthread_func, net, "scq thread done");
	return 0;
}

static inline int scq_kthread_create_(struct nvmeibc_ib_net *net, int comp_cpu)
{
	struct task_struct *t = NULL;
	int rv = -1;
	proc_name_t pname;

	__NFIN;

	if (!net->scq_kthread) {
		init_completion(&net->scq_kth_ready);
		if (comp_cpu >= 0) {
			clnt_proc_name_format_extd(pname, 'C', "PL", "ScqP",
						   nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(&net->admin_ch->base)),
						   comp_cpu);
			if (!IS_ERR(t = nvmeib_public_kthread_create_on_cpu(scq_kthread_func, net, comp_cpu, pname))) {
				_NDn(trace_1_ib_net_scq_kthread_create, net, "scq thread created on cpu @INT", comp_cpu);
				wake_up_process(t);
				wait_for_completion(&net->scq_kth_ready);
				net->scq_kthread = t;
				rv = 0;
			}
		} else {
			clnt_proc_name_format(pname, 'C', "PL", "scq_pl", nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(&net->admin_ch->base)));
			if (!IS_ERR(t = kthread_run(scq_kthread_func, net, "%s", pname))) {
				_NDn(trace_ib_net_scq_kthread_create, net, "scq thread created");
				wait_for_completion(&net->scq_kth_ready);
				net->scq_kthread = t;
				rv = 0;
			}
		}
	}

	__NFOUT;
	return rv;
}

void scq_kthread_stop(struct nvmeibc_ib_net *net)
{
	struct task_struct *t = NULL;
	unsigned long flags;
	int rv;

	__NFIN;

	if (net->scq_kwq) {
		/* Cancel kernel workqueue work */
		_NTn(trace_ib_net_scq_kthread_stop, net, "canceling scq_kwq work");
		nvmeib_public_cancel_work_sync(&net->scq_kwork);
		net->scq_kwq = NULL;
	} else if (net->scq_kthread) {
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
		t = net->scq_kthread;
		net->scq_kthread = NULL;
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

		if (t) {
			_NTn(trace_ib_net_scq_kthread_stopx, net, "stopping kthread");
			rv = kthread_stop(t);
			_NTn(trace_1_ib_net_scq_kthread_stop, net, "scq kthread_stop (rv @RV)", rv);
		}
	}
	else {
		_NDn(trace_2_ib_net_scq_kthread_stop, net, "No scq thread");
	}

	__NFOUT;
}

static int process_send_cq_offload_enb_(struct nvmeibc_ib_net *net, int ne,
	bool req_notify, bool in_interrupt, enum req_notify_state *req_notify_state)
{
	int n, rearm_rv;
	__NFIN;

	BUG_ON(!nvmeibc_channel_already_locked(net->ioch));
	BUG_ON(ne > net->n_wc_s);

	*req_notify_state = REQ_NOTIFY_STATE_NONE;
	do {
		n = process_send_cq_(net, ne);
		if (unlikely(n < 0)) {
			_NEn(xxx_01, net, "Failed poll-cq (@COUNT), disconnect net", n);
			nvmeibc_ib_net_disconnect(net);
			break;
		}

		if (in_interrupt)
			net->scq_stats.n_intr += n;
		else
			net->scq_stats.n_poll += n;

		if (req_notify) {
			if (n < ne) {
				/* interrupt or poll-thread in last poll attempt */
				if ((rearm_rv = ib_req_notify_cq(net->send_cq,
						IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS))) 
				{
					if (unlikely(rearm_rv < 0)) {
						_NEn(xxx_02, net, "Failed poll-cq (@COUNT), disconnect net", rearm_rv);
						nvmeibc_ib_net_disconnect(net);
						*req_notify_state = REQ_NOTIFY_STATE_NOTIFY_FAILED;
						break;
					}
					else {
						/* rearm_rv > 0 i.e. missed events:
							signal interrupt handler to switch 2 polling
							signal polling thread to keep-polling*/
						BUG_ON(rearm_rv <= 0);
						*req_notify_state = REQ_NOTIFY_STATE_NOTIFY_MISSED_EVENTS;
						net->scq_stats.n_missed_events++;
					}
				} else {
					/* we polled ZERO wcs && rearmed without missing an event */
					*req_notify_state = REQ_NOTIFY_STATE_NOTIFY_OK;
				}
			} else {
				*req_notify_state = REQ_NOTIFY_STATE_CQ_NOT_EMPTY;
			}
		} else {
			if (in_interrupt) {
				_NEn(xxx_03, net, "Caller is interrupt but didn't ask to rearm, BOOM!");
				BUG();
			}
			else {
				/* the only case here is the polling thread in its
					polling-loop, no need to signal anything */
			}
		}
	} while (0);

	__NFOUT;
	return n;
}

static inline void intr_process_send_cq_offload_enb_(struct nvmeibc_ib_net *net)
{
	enum nvmeib_intr_shaper_calc_ret wake_up_reason;
	enum cq_poll_mode poll_mode = READ_ONCE(net->scq_poll_mode);

	__NFIN;

	if (poll_mode == NVMEIBC_IB_CQ_INTR) {

		/* if this is the first intr after polling period and
		   we are in the same time-frame */
sw2polling:
		if (nvmeib_intr_shaper_intr_should_wake_up_reason(net->intr_shaper, &wake_up_reason)) {
			_NDn(trace_ib_net_intr_process_send_cq_offload_enb, net, "sw2polling (# wakeups_burst @N_WAKEUPS wakeups_cycles @N_WAKEUPS)", net->scq_stats.n_wakeups_burst, net->scq_stats.n_wakeups_cycles);
			scq_offload_trace(net, __LINE__);
			nvmeib_qp_stats_on_offload_sched(net->qp_stats);
			WRITE_ONCE(net->scq_poll_mode, NVMEIBC_IB_CQ_POLLING);
			smp_mb();
			if (net->scq_kwq) {
				scq_queue_work(net);
			} else {
				BUG_ON(!net->scq_kthread);
				wake_up_process(net->scq_kthread);
			}
			if (wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES)
				net->scq_stats.n_wakeups_cycles++;
			else if (wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_BURST)
				net->scq_stats.n_wakeups_burst++;
			else if (wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_IRQ_TIME)
				net->scq_stats.n_wakeups_irq_time++;
			else
				BUG();
		}
		else {
			enum req_notify_state req_notify_state;
			int n;
			net->scq_stats.n_external++;
			n = process_send_cq_offload_enb_(net, net->n_wc_s,
												  REQ_NOTIFY_TRUE, true, &req_notify_state);
			if (n < 0) {
				goto out;
			} else if (req_notify_state == REQ_NOTIFY_STATE_NOTIFY_MISSED_EVENTS || 
				req_notify_state == REQ_NOTIFY_STATE_CQ_NOT_EMPTY)
			{
				goto sw2polling;
			}
		}
	}
	else if (poll_mode == NVMEIBC_IB_CQ_POLLING) {
		_NDn(trace_1_ib_net_intr_process_send_cq_offload_enb, net, "scq thread already running");
		scq_offload_trace(net, __LINE__);
		WRITE_ONCE(net->scq_poll_mode, NVMEIBC_IB_CQ_KEEP_POLLING);
	}
	else if (poll_mode == NVMEIBC_IB_CQ_KEEP_POLLING) {
		/* Got an interrupt while we're still polling */
		inc_scq_spurious_intr_and_report(net);

		//_NWn(warn_ib_net_intr_process_send_cq_offload_enb, net, "unexpected, scq interrupts rearmed not by kthread");
		scq_offload_trace(net, __LINE__);
		scq_offload_tarce_print(net);

		/* SIW has some issue here, don't warn, just count */
		if (P2NV(net->port)->dev_type != DT_siw)
			WARN_ON_ONCE(true);
	}
out:
	__NFOUT;
}

static inline void intr_process_send_cq_(struct nvmeibc_ib_net *net)
{
	int n;
	int first_poll = true;

	__NFIN;

	poll_again:
	do {
		n = process_send_cq_(net, net->n_wc_s);

		/* collect stats */
		if (n > 0)
			net->scq_stats.n_intr += n;
		else if (!n && first_poll)
			inc_scq_spurious_intr_and_report(net);
		first_poll = false;
	} while (n == net->n_wc_s);

	if (ib_req_notify_cq(net->send_cq, IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS) > 0) {
		net->scq_stats.n_missed_events++;
		goto poll_again;
	}

	__NFOUT;
}

static void intr_process_send_cq_pcpu_func(void *info)
{
	struct nvmeibc_ib_net *net = info;
	unsigned long flags;
	unsigned long start, delta;

	__NFIN;
	nvmeib_intr_shaper_intr_enter(net->intr_shaper, INTR_SHAPER_INTR_TYPE_CLIENT_SCQ);
	BUG_ON(!nvmeibc_channel_is_ll_pcpu_ch(net->ioch));
	BUG_ON(nvmeibc_channel_pcpu_ch_get_cpu(net->ioch) != smp_processor_id());

	start = jiffies;
	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	net->scq_stats.n_ipi_func++;
	if (net->scq_kthread)
		intr_process_send_cq_offload_enb_(net); /* currently only lock-ch */
	else
		intr_process_send_cq_(net);
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	delta = jiffies - start;
	if (delta > net->scq_stats.max_intr_duration) {
		net->scq_stats.max_intr_duration = delta;
		_NDn(trace_ib_net_intr_process_send_cq_pcpu_func, net, "max_intr_duration = @MAX_INTR_DURATION",
		     net->scq_stats.max_intr_duration);
	}

	BUG_ON(atomic_xchg(&net->pcpu_send_comp_smp_call.call_pending, 0) != 1);

	//NOTE: Ref was increased by send_completion_intr */
	nvmeib_ref_put(&net->ib_rsrc_ref);
	nvmeib_intr_shaper_intr_exit(net->intr_shaper);
	__NFOUT;
}

// Verifies passed nvmeibc_ib_net generation object has the same
// generation as the current one. The generation is encoded in the
// upper 8 address bits.
static inline void* verify_net_generation(void* net_ptr)
{
#ifdef NVMEIBC_NET_GEN_VERIFY
	struct nvmeibc_ib_net *net;
	const u64 mask = 0xff00000000000000ul;
	unsigned char gen;

	gen = (u64)net_ptr >> 56;
	// fix canonical address
	net = (void*)(((u64)net_ptr & ~mask) | (((u64)net_ptr << 8) & mask));
	if (gen != net->gen) {
		_NE(verify_net_generation_T1,
			"net=@PTR  net_ptr=@PTR   "
			"generation number mismatch - got @INT  expected @INT "
			"(from '@__BUILTIN_RETURN_ADDRESS_FUNC')",
		   net, net_ptr, gen, net->gen, __builtin_return_address(0));
		net = NULL;
	}
	return net;
#else
	return net_ptr;
#endif
}


static void send_completion_intr(struct ib_cq *cq, void *net_ptr)
{
	struct nvmeibc_ib_net *net = NULL;
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	void *val;
#endif
	unsigned long flags;
	u64 start, delta;

	__NFIN;
	if (!(net = verify_net_generation(net_ptr)))
		return;
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	if ((val = nvmeib_c_tree_lookup((unsigned long)net)) != net) {
		_NE(send_completion_intr_e1,
			"OOPS, net=@PTR (val=@PTR) was deleted", net, val);
		goto out;
	}
#endif

	nvmeib_qp_stats_on_interrupt(net->qp_stats);
	net->n_send_intrs++;
	nvmeibc_disk_net_intrs_stats_inc(net->ioch->disk, true);

	//Let SCQ drain
	//if (atomic_read(&net->dying)) {
	//	_NTn(send_completion_intr_t1, net, "is dying");
	//	return;
	//}

	if (!nvmeib_ref_get(&net->ib_rsrc_ref)) {
		_NTn(send_completion_intr_t1,
			net, "failed to get ib_rsrc");
		goto out;
	}
	nvmeib_intr_shaper_intr_enter(net->intr_shaper, INTR_SHAPER_INTR_TYPE_CLIENT_SCQ);

	if (nvmeibc_channel_is_ll_pcpu_ch(net->ioch)) {
		if (net->ioch->ct == ct_n_rdda &&
			atomic_read(&net->ioch->disk->pcpu_nrch_poll[nvmeibc_channel_pcpu_ch_get_cpu(net->ioch)].open_cnt)) {
			/* NRCH Poller is connected, so ignore the interrupt */
			goto ref_put;
		} else if (smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(net->ioch)) {
			/* Interrupt came in on different CPU. Use IPI to switch to correct CPU (if not already pending) */
			if (atomic_cmpxchg(&net->pcpu_send_comp_smp_call.call_pending, 0, 1) == 0) {
				if (nvmeib_public_smp_call_function_single_async(
						nvmeibc_channel_pcpu_ch_get_cpu(net->ioch),
						&net->pcpu_send_comp_smp_call.call_data) < 0) {
					_NEn(send_completion_intr_t2, net, "smp_call_function_single_async failed");
					BUG_ON(atomic_xchg(&net->pcpu_send_comp_smp_call.call_pending, 0) != 1);
				} else {
					nvmeib_intr_shaper_intr_exit(net->intr_shaper);
					/* NOTE: We do not dec the ref count until the IPI runs so it does not get destroyed under out feet */
					goto out;
				}
			}
			goto ref_put;
		}
	}

	start = jiffies;
	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	if (net->scq_kthread || net->scq_kwq)
		intr_process_send_cq_offload_enb_(net); /* currently only lock-ch */
	else
		intr_process_send_cq_(net);
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	delta = jiffies - start;
	if (delta > net->scq_stats.max_intr_duration) {
		net->scq_stats.max_intr_duration = delta;
		_NDn(trace_ib_net_send_completion_intr, net, "max_intr_duration = @MAX_INTR_DURATION",
			net->scq_stats.max_intr_duration);
	}

ref_put:
	nvmeib_intr_shaper_intr_exit(net->intr_shaper);
	nvmeib_ref_put(&net->ib_rsrc_ref);

out:
	__NFOUT;
}

/* --------------------------------------------------------------------- *
 *                           recv-completions                                 *
 * -------------------------------------------------------------------------- */
/* former version of send-comp handling, prior to polling-kthread support */
#if 0
static void check_recv_completion_(struct nvmeibc_ib_net *net)
{
	int i, m, n, rv;

	__NFIN;
	ib_req_notify_cq(net->recv_cq, IB_CQ_NEXT_COMP);
	if ((n = ib_poll_cq(net->recv_cq, net->n_wc_r,
						net->wc_r)) > 0) {
		m = 0;
		nvmeib_srq_dec(net->srq_info, n);
		for (i = 0; i < n; ++i)
			if ((rv = net->call_receive_comp_handler(
				net, &net->wc_r[i])) < 0 || atomic_read(&net->dying)) {
				post_wc_r_range(net, i+1, n-1);
				nvmeibc_ib_net_disconnect_(net, false, true);
				m = 0;
				break;
			}
			else /* if rv == 1 the caller asked to call send_completion */
				m += rv;

		if (m)
			/* the caller asks us to try tp clean the send queue */
			check_send_completion_(net);
	}
	else if (n < 0)
		nvmeibc_ib_net_disconnect_(net, false, true);
	__NFOUT;
}
#endif

static inline int process_num_recv_comps_(struct nvmeibc_ib_net *net, int n)
{
	int i, err = 0;
	int m, rvh;
	bool got_drain = false;

	__NFIN;
	BUG_ON(!nvmeibc_channel_already_locked(net->ioch)); /* process_num_recv_comps_ */

	if (atomic_read(&net->dying)) {
		_NTn(trace_ib_net_process_num_recv_comps, net, "is dying (n: @COUNT)", n);
		if (n > 0) {
			drain_recv_wcs(net, net->wc_r, 0, n, &got_drain);
			if (got_drain)
				err = -EINTR;
		}
		goto out;
	}

	if (n > 0) {
		m = 0;
		for (i = 0; i < n; ++i) {
			if (net->recv_q) {
				int rq_post_count = atomic_dec_return(&net->rq_post_count);
				_NDn(trace_1_ib_net_process_num_recv_comps, net, "rq_post_count @RQ_POST_COUNT", rq_post_count);
				BUG_ON(rq_post_count < 0);
				if (nvmeib_opcode_from_wc(&net->wc_r[i]) == NVMEIB_DRAIN_QUEUE) {
					/* In case of shared RCQ, drain may be for another net */
					struct nvmeibc_ib_net *drain_net = net->wc_r[i].qp->qp_context;
					_NTn(trace_2_ib_net_process_num_recv_comps, drain_net, "Got Drain for QP @QP_NUM on RCQ @RECV_Q",
						drain_net->qp->qp_num, net->recv_q);
					if (!atomic_read(&drain_net->dying)) {
						_NEn(error_ib_net_process_num_recv_comps, drain_net, "RCQ got Drain WC without net dying set");
					}
					rq_drain_comp(drain_net, &net->wc_r[i]);
					if (drain_net == net) {
						err = -EINTR;
						goto out;
					}
				}
			}
			if ((rvh = net->call_receive_comp_handler(
				net, &net->wc_r[i])) < 0 || atomic_read(&net->dying)) {
				_NEn(error_1_ib_net_process_num_recv_comps, net, "recv-comp-handling error, disconnect net (n: @COUNT, i: @INDEX, rq_post_count: @ATOMIC_READ)",
					n, i, atomic_read(&net->rq_post_count));
				nvmeibc_ib_net_disconnect_(net);
				drain_recv_wcs(net, net->wc_r, i + 1, n, &got_drain);
				if (got_drain)
					err = -EINTR;
				else
					err = rvh;
				goto out;
			}
			else /* if rvh == 1 the caller asked to call send_completion */
				m += rvh;
		}

		if (!err && m)
			/* the caller asks us to try tp clean the send queue */
			nvmeibc_ib_net_process_send_cq(net, net->n_wc_s);
			/* OL: why not use the already-locked version? */

		/* collect stats */
		if (net->rcq_poll_mode != NVMEIBC_IB_CQ_INTR)
			net->rcq_stats.n_poll += i;
		else
			net->rcq_stats.n_intr += i;
	}

out:
	__NFOUT;
	return err < 0 ? err : n;
}

static inline int process_num_mixed_comps_(struct nvmeibc_ib_net *net, int n);
#define nvmeibc_net_get_cinst(net) (nvmeibc_cinst_get_core_p((net)->ioch))			// Can also use nvmeibc_cinst_get_core_p((net)->admin_ch->base)
static int process_recv_cq_(struct nvmeibc_ib_net *net, bool req_notify, enum req_notify_state *req_notify_state)
{
	int n, n_wc, rv = 0;
	struct ib_wc *wc_arr;

	__NFIN;

	BUG_ON(!nvmeibc_channel_already_locked(net->ioch)); /* process_recv_cq_ */

	/* rearm (?) and poll */
	*req_notify_state = REQ_NOTIFY_STATE_NONE;
	do {
		BUG_ON(net->qp->recv_cq != net->recv_cq);
		if (net->shared_cq) {
			n_wc = net->n_wc_mixed;
			wc_arr = net->wc_mixed;
		} else {
			n_wc = net->n_wc_r;
			wc_arr = net->wc_r;
		}
		/* process completions */
		if ((n = ib_poll_cq(net->recv_cq, n_wc, wc_arr)) >= 0) {
			if (n < 0) {
				_NEn(error_3_ib_net_process_recv_cq, net, "Failed poll-cq (@COUNT), disconnect net", n);
				nvmeibc_ib_net_disconnect(net);
				rv = n;
				break;
			}
			else if (n > 0) {
				if (net->shared_cq) {
					nvmeib_qp_stats_on_poll_cq(net->qp_stats, n, mixed);
					rv = process_num_mixed_comps_(net, n);
				} else {
					nvmeib_qp_stats_on_poll_cq(net->qp_stats, n, recv);
					rv = process_num_recv_comps_(net, n);
				}
				if (rv < 0 && rv != -EINTR) {
					_NTn(error_ib_net_process_recv_cq, net, "comp processing error @RV, disconnect net (n: @COUNT, rq_post_count: @ATOMIC_READ)",
							rv, n, atomic_read(&net->rq_post_count));
					nvmeibc_ib_net_disconnect(net);
					break;
				}
			}
			else {
				nvmeib_qp_stats_on_poll_cq_empty(net->qp_stats);
			}
			if (req_notify && n < n_wc) {
				BUG_ON(net->qp->recv_cq != net->recv_cq);
				if ((rv = ib_req_notify_cq(net->recv_cq,
					IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS))) 
				{
					if (rv < 0) {
						_NEn(error_1_ib_net_process_recv_cq, net, "ib_req_notify_cq failed (@RV), disconnect net", rv);
						nvmeibc_ib_net_disconnect(net);
						*req_notify_state = REQ_NOTIFY_STATE_NOTIFY_FAILED;
					}
					else {
						*req_notify_state = REQ_NOTIFY_STATE_NOTIFY_MISSED_EVENTS;
						net->rcq_stats.n_missed_events++;
					}
				} else {
					*req_notify_state = REQ_NOTIFY_STATE_NOTIFY_OK;
				}
			}
			else if (req_notify) {
				/* if the @num_entries we asked to poll < driver's cq-size &&
				   @n > 0  && there were more CQEs in the cq before we polled,
				   call to ib_req_notify_cq will not detect missed-events nor
				   will we get interrupt for these pending CQEs.
				   Thus, if @n > 0, we must keep polling (here or in thread) */
				   *req_notify_state = REQ_NOTIFY_STATE_CQ_NOT_EMPTY;
			}
		}
		else if (n < 0) {
			_NEn(error_2_ib_net_process_recv_cq, net, "Failed poll-cq (@COUNT), disconnect net", n);
			nvmeibc_ib_net_disconnect(net);
		}
		rv = n;
	} while (0);
	/* Loop if we are not in the thread (req_notify = false) and events have been missed */

	__NFOUT;
	return n;
}

static int polling_process_recv_cq_(struct nvmeibc_ib_net *net, bool req_notify, bool *continue_polling, u64 *poll_time_ns)
{
	unsigned long flags = 0;
	int rv;
	enum req_notify_state req_notify_state;
	u64 start_ns, busy_ns;

	__NFIN;

	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	start_ns = nvmeib_public_local_clock();
	rv = process_recv_cq_(net, req_notify, &req_notify_state);
	busy_ns = nvmeib_public_local_clock() - start_ns;
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	if (continue_polling) {
		*continue_polling = (req_notify && (
			req_notify_state == REQ_NOTIFY_STATE_NOTIFY_MISSED_EVENTS ||
			req_notify_state == REQ_NOTIFY_STATE_CQ_NOT_EMPTY)
		);
	}

	if (poll_time_ns) {
		*poll_time_ns = busy_ns;
	}

	__NFOUT;
	return rv;
}

static int rcq_kthread_func(void *arg)
{
	struct nvmeibc_ib_net *net = arg;
	unsigned long flags;
	int n, tot;
	u64 busy_ns;
	bool continue_polling;

	_NTn(trace_ib_net_rcq_kthread_func, net, "rcq thread ready");
	set_current_state(TASK_INTERRUPTIBLE);
	complete(&net->rcq_kth_ready);
	schedule();
	_NTn(trace_1_ib_net_rcq_kthread_func, net, "rcq thread started");

	tot = 0;
	if (!nvmeib_ref_get(&net->ib_rsrc_ref)) {
		_NTn(rcq_kthread_func_t1, net, "failed to get ib_rsrc");
		goto out;
	}
	while (!kthread_should_stop()) {
		continue_polling = false;
		nvmeib_qp_stats_on_offth_iter(net->qp_stats);
		if (net->rcq_poll_mode == NVMEIBC_IB_CQ_INTR) {
			_NEn(error_ib_net_rcq_kthread_func, net, "Oops, intr-polling off");
			BUG();
		}

		n = polling_process_recv_cq_(net, REQ_NOTIFY_FALSE, NULL, &busy_ns);
		if (n >= 0) {
			tot += n;
			continue_polling = n > 0 && nvmeib_intr_shaper_should_continue_polling(net->intr_shaper, tot, busy_ns);
			if (continue_polling) {
				/* prevent soft lockup */
				cond_resched();
				continue;
			}
		}

		_NDn(trace_2_ib_net_rcq_kthread_func, net, "transition back to IRQ");

		/* The correct sequence to prevent lost-wakeups is: 
		* 1. Set state for interrupt to see
		* 2. Set_current_state(TASK_INTERRUPTIBLE);
		* 3. Write Barrier
		* 4. Enable interrupts
		* 5. Read Barrier
		* 6. Check state if interrupt has changed it to polling mode
		* 7. Schedule if state has not changed, otherwise continue polling
		*/

		/* Step 1: update poll-mode */
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
		if (net->rcq_poll_mode == NVMEIBC_IB_CQ_POLLING)
			net->rcq_poll_mode = NVMEIBC_IB_CQ_INTR;
		else if (net->rcq_poll_mode == NVMEIBC_IB_CQ_KEEP_POLLING) {
			net->rcq_poll_mode = NVMEIBC_IB_CQ_POLLING;
			continue_polling = true;
		}
		else BUG();
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

		if (continue_polling) {
			_NDn(trace_6_ib_net_rcq_kthread_func, net, "intr during transition to intr-mode, keep running");
			if (kthread_should_stop())
				goto put_ref;
			/* prevent soft lockup */
			cond_resched();
			continue;
		}

		/* Step 2: Set task state for interrupt to see */
		set_current_state(TASK_INTERRUPTIBLE);

		/* Step 3: Write Barrier */
		smp_mb();

		/* This step is only needed if the previous poll was successful */
		if (n >= 0) {
			/* Step 4: Enable interrupts using ib_req_notify_cq */
			n = polling_process_recv_cq_(net, REQ_NOTIFY_TRUE, &continue_polling, NULL);
		}
		
		if (n >= 0) {
			tot += n;
			if (continue_polling) {
				/* We missed an event or didn't manage to empty the CQ, so we need to undo the transition and poll again */
				_NDn(trace_7_ib_net_rcq_kthread_func, net, "failed to arm CQ, keep running");
				if (kthread_should_stop())
					goto put_ref;
				WRITE_ONCE(net->rcq_poll_mode, NVMEIBC_IB_CQ_POLLING);
				set_current_state(TASK_RUNNING);
				smp_mb();
				cond_resched();
				continue;
			}
		}

		/* Step 5: Read Barrier */
		smp_mb();

		/* Step 6/7: Check state if interrupt has changed it to polling mode */
		if ((READ_ONCE(net->rcq_poll_mode) != NVMEIBC_IB_CQ_INTR) && (n >= 0) &&
			!atomic_read(&net->dying)) 
		{
			_NDn(trace_3_ib_net_rcq_kthread_func, net, "intr during transition to intr-mode, keep running");
			set_current_state(TASK_RUNNING);
			cond_resched(); /* prevent soft lockup */
			continue;
		}

		_NDn(trace_4_ib_net_rcq_kthread_func, net, "transition back to IRQ after @N_TOTAL completions", tot);
		tot = 0;

		/* Step 7: Call schedulle(), after checking to prevent lost wakeup from kthread-stop */
		if (!kthread_should_stop())
			/* if kthread-stop is called now, then:
			   if stop-bit set BUT before wakeup, thread sched-out for a while
			   if stop-bit set AND wakeup called, thread continue to run */
			schedule();
	}
put_ref:
	nvmeib_ref_put(&net->ib_rsrc_ref);
out:
	_NTn(trace_5_ib_net_rcq_kthread_func, net, "rcq thread done");
	return 0;
}

static inline int rcq_kthread_create_(struct nvmeibc_ib_net *net, int comp_cpu)
{
	struct task_struct *t = NULL;
	int rv = -1;
	proc_name_t pname;

	__NFIN;

	if (!net->rcq_kthread) {
		init_completion(&net->rcq_kth_ready);

		if (comp_cpu >= 0) {
			clnt_proc_name_format_extd(pname, 'C', "PL", "RcqP",
						   nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(&net->admin_ch->base)),
						comp_cpu);
			if (!IS_ERR(t = nvmeib_public_kthread_create_on_cpu(rcq_kthread_func, net, comp_cpu, pname))) {
				_NDn(trace_1_ib_net_rcq_kthread_create, net, "rcq thread created on cpu @INT", comp_cpu);
				wake_up_process(t);
				wait_for_completion(&net->rcq_kth_ready);
				net->rcq_kthread = t;
				rv = 0;
			}
		} else {
			clnt_proc_name_format(pname, 'C', "PL", "rcq_pl", nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(&net->admin_ch->base)));
			if (!IS_ERR(t = kthread_run(rcq_kthread_func, net, "%s", pname))) {
				_NDn(trace_ib_net_rcq_kthread_create, net, "rcq thread created");
				wait_for_completion(&net->rcq_kth_ready);
				net->rcq_kthread = t;
				rv = 0;
			}
		}
	}

	__NFOUT;
	return rv;
}

static inline int rcq_kthread_create(struct nvmeibc_ib_net *net, int comp_cpu)
{
	unsigned long flags;
	int rv;

	__NFIN;
	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	rv = rcq_kthread_create_(net, comp_cpu);
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);
	__NFOUT;

	return rv;
}

/**
 * Logic that may call net-disconnect will also nullify the ionet->req.iocmd
 * which the scq-kthread may be dealing with. Thus, BEFORE this nullify can
 * happen i.e. before acknowlwdging net-disconnect/qp-break, we need to make
 * sure that the scq-kthread is not running and cant be waken up
 */
void rcq_kthread_stop(struct nvmeibc_ib_net *net)
{
	struct task_struct *t = NULL;
	unsigned long flags;
	int rv;

	__NFIN;

	if (net->rcq_kthread) {
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
		t = net->rcq_kthread;
		net->rcq_kthread = NULL;
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

		if (t) {
			_NTn(trace_ib_net_rcq_kthread_stop, net, "stopping kthread");
			rv = kthread_stop(t);
			_NTn(trace_1_ib_net_rcq_kthread_stop, net, "rcq kthread_stop (rv @RV)", rv);
		}
	}
	else {
		_NDn(trace_2_ib_net_rcq_kthread_stop, net, "No rcq thread");
	}


	__NFOUT;
}
/**
 * It is likely that the heavily loaded nets will show-up at a time-frame
 * while its burst size had exceeded and thus will be the ones that get
 * switched to polling-mode.
 * Per disk, all its nets shall have the same intr-load (well except admin ch)
 *  But different disks may have different loads
 */
static inline void intr_process_recv_cq_(struct nvmeibc_ib_net *net)
{
	enum nvmeib_intr_shaper_calc_ret wake_up_reason;
	enum cq_poll_mode poll_mode = READ_ONCE(net->rcq_poll_mode);
	__NFIN;

	BUG_ON(!net->rcq_kthread);
	if (poll_mode == NVMEIBC_IB_CQ_INTR) {
		/* if this is the first intr after polling period and
		   we are in the same time-frame */
		int ncqe;

poll_again:
		ncqe = (false && net->peek_cq) ? (*net->peek_cq)(net->recv_cq, net->n_wc_r) : 0;
		if (nvmeib_intr_shaper_intr_should_wake_up_reason(net->intr_shaper, &wake_up_reason) ||
			(ncqe > nvmeib_intr_shaper_get_max_burst(net->intr_shaper))) 
		{
			_NDn(trace_ib_net_intr_process_recv_cq, net, "sw2polling (# wakeups_burst @N_WAKEUPS wakeups_cycles @N_WAKEUPS # rcqes @NCQE)", net->rcq_stats.n_wakeups_burst, net->rcq_stats.n_wakeups_cycles, ncqe);
			nvmeib_qp_stats_on_offload_sched(net->qp_stats);
			WRITE_ONCE(net->rcq_poll_mode, NVMEIBC_IB_CQ_POLLING);
			smp_mb();
			wake_up_process(net->rcq_kthread);
			if (wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES)
				net->rcq_stats.n_wakeups_cycles++;
			else if (wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_BURST)
				net->rcq_stats.n_wakeups_burst++;
			else if (wake_up_reason == NVMEIB_INTR_SHAPER_RET_WAKE_UP_IRQ_TIME)
				net->rcq_stats.n_wakeups_irq_time++;
			else
				BUG();
		}
		else {
			enum req_notify_state req_notify_state;
			int rv;
			int n_iter = 0;
			net->rcq_stats.n_external++;
			/* NOTE: Further intr-shaper logic is handled in process_recv_cq_ */
			rv = process_recv_cq_(net, REQ_NOTIFY_TRUE, &req_notify_state);
			if (rv < 0) {
				goto out;
			} else if (req_notify_state == REQ_NOTIFY_STATE_NOTIFY_MISSED_EVENTS ||
				req_notify_state == REQ_NOTIFY_STATE_CQ_NOT_EMPTY) 
			{
				if (++n_iter > nvmeibc_max_notify_cq_iterations) {
					_NEn_dmesg(err_intr_process_recv_cq_, net, "Failed to arm recv cq after @INT iterations, disconnecting", n_iter);
					nvmeibc_ib_net_disconnect(net);
					goto out;
				}
				goto poll_again;
			}
		}
	}
	else if (poll_mode == NVMEIBC_IB_CQ_POLLING) {
		_NDn(trace_1_ib_net_intr_process_recv_cq, net, "rcq thread already running");
		WRITE_ONCE(net->rcq_poll_mode, NVMEIBC_IB_CQ_KEEP_POLLING);
	}
	else if (poll_mode == NVMEIBC_IB_CQ_KEEP_POLLING) {
		/* Got an interrupt while we're still polling */
		inc_rcq_spurious_intr_and_report(net);
	}
out:
	__NFOUT;
}

struct recv_comp_workq {
	struct workqe_struct work;
	struct nvmeibc_ib_net *net;
};

static inline int process_num_mixed_comps_(struct nvmeibc_ib_net *net, int n)
{
	u32 wr_opcode;
	struct ib_wc *wc;
	int i, rv = 0;
	int n_send = 0, n_recv = 0;
	__NFIN;

	for (i = 0; i < n; i++) {
		wc = &net->wc_mixed[i];
		wr_opcode = nvmeib_opcode_from_wc(wc);

		//_NI(process_num_mixed_comps__i1, "wr_opcode="@INT32_HEX, wr_opcode);
		if ((u16)wr_opcode == NVMEIB_RECV) { /* cast to u16 so not to conflict with send WRs */
			net->wc_r[0] = *wc;
			rv = process_num_recv_comps_(net, 1);
			n_recv++;
		}
		else {
			net->wc_s[0] = *wc;
			rv = process_num_send_comps_(net, 1);
			n_send++;
		}

		if (rv < 0 && rv != -EINTR) {
			_NTn(process_num_mixed_comps_t1, net,
				"wr_opcode=@INT32_HEX comp processing error (rv=@INT32_HEX)",
				wr_opcode, rv);
			break;
		}
	}

	nvmeib_qp_stats_on_update_cqes(net->qp_stats, n_send, n_recv);

	__NFOUT;
	return rv;
}

static inline int poll_cq_and_process(struct nvmeibc_ib_net *net,
									  bool *resched)
{
	unsigned long flags;
	int dying;
	int n, rv;
	__NFIN;

	*resched = false;

	/* sanity */
	if (!net) {
		_NE(poll_cq_and_process_e1, "OOPS");
		rv = -EINVAL;
		goto out;
	}

	if ((dying = atomic_read(&net->dying))) {
		_NE(poll_cq_and_process_e2, "net @PTR is dying", net);
		rv = -EBUSY;
		goto out;
	}
	if (!nvmeib_ref_get(&net->ib_rsrc_ref)) {
		_NTn(poll_cq_and_process_e3, net, "failed to get ib_rsrc");
		rv = -EBUSY;
		goto out;
	}

	/*
	 * Poll CQ
	 */
	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	BUG_ON(net->qp->recv_cq != net->recv_cq);
	n = ib_poll_cq(net->recv_cq, net->n_wc_mixed, net->wc_mixed);
	if (n < 0) {
		_NEn(poll_cq_and_process_e4, net,
			"Failed poll-cq (@INT), disconnect net", n);
		nvmeibc_ib_net_disconnect_(net);
		rv = n;
	}
	/*
	 * Process CQEs
	 */
	else if (n > 0) {
		net->rcq_stats.n_defer_wq += n;
		nvmeib_qp_stats_on_poll_cq(net->qp_stats, n, mixed);
		rv = process_num_mixed_comps_(net, n);

		if (rv < 0) {
			if (rv != -EINTR) {
				_NTn(poll_cq_and_process_t1,
					 net, "comp processing error @INT, disconnect net "
					 "(n: @INT, rq_post_count: @INT)",
					rv, n, atomic_read(&net->rq_post_count));
				nvmeibc_ib_net_disconnect_(net);
			}
		}
		else if (n == net->n_wc_mixed) {
			/* More cqes might be waiting (namely if CQ-size > @n_wc_mixed) ->
			   caller "must poll the CQ again" */
			net->rcq_stats.n_defer_over_budget++;
			*resched = true;
		}
		else {
			/* rv >= 0 -> caller can rearm */
		}
	}
	else {
		nvmeib_qp_stats_on_poll_cq_empty(net->qp_stats);
		net->rcq_stats.n_defer_empty_cq++;
		_NDn(poll_cq_and_process_t2,
			 net, "n_defer_empty_cq=@INT",
			 net->rcq_stats.n_defer_empty_cq);
		rv = 0;
	}
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	nvmeib_ref_put(&net->ib_rsrc_ref);

out:
	__NFOUT;
	return rv;
}

static int defer_recv_interrupts_(struct nvmeibc_ib_net *net);

/* Common processing function for both custom and kernel workqueues */
static inline void poll_cq_and_process_common(struct nvmeibc_ib_net *net)
{
	bool resched = false;
	u64 start_ns, busy_ns;
	int rv;
	__NFIN;

	BUG_ON(!nvmeib_switch_state_guard(&net->defer_recv_state,
		NVMEIBC_IB_NET_DEFER_RECV_SCHEDULED,
		NVMEIBC_IB_NET_DEFER_RECV_RUNNING));

	nvmeib_qp_stats_on_offth_iter(net->qp_stats);

	/* poll upto net->n_wc_mixed CQEs into net->wc_mixed */
	start_ns = nvmeib_public_local_clock();
	rv = poll_cq_and_process(net, &resched);
	busy_ns = nvmeib_public_local_clock() - start_ns;

	if (!resched) {
		resched = rv > 0 && nvmeib_intr_shaper_should_continue_polling(net->intr_shaper, rv, busy_ns);
	}

	/* before we may rearm interrupts we must change state such that
	   interrupt-handler will be able to add another defer-recv work.
	   Otherwise, we rearm -> no missed-events -> an interrupt fires
	   but can't add work (due to state) -> work completes -> result:
	   huge max-latency / io timeout (WD) / io chokes for secs ... */
	if (!nvmeib_switch_state_guard(&net->defer_recv_state,
		NVMEIBC_IB_NET_DEFER_RECV_RUNNING,
		NVMEIBC_IB_NET_DEFER_RECV_IDLE)) {
		/* Must be terminating, switch to disabled */
		BUG_ON(!nvmeib_switch_state_guard(&net->defer_recv_state,
		NVMEIBC_IB_NET_DEFER_RECV_TERMINATING,
		NVMEIBC_IB_NET_DEFER_RECV_DISABLED));
		goto out;
	}

	if (rv < 0)
		goto out;

	if (resched)
	/* n_polled = poll-zise */
		goto resched;

	/*
	 * n_polled = [0, poll-zise), we can rearm
	 */
	rv = ib_req_notify_cq(net->recv_cq,
		IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
	if (unlikely(rv < 0)) {
		_NEn(error_poll_cq_and_process_work,
			 net, "Failed req_notify-cq, disconnect net");
		nvmeibc_ib_net_disconnect(net);
		goto out;
	}
	else if (rv > 0) {
		/* Missed events - "must poll the CQ again" */
		net->rcq_stats.n_defer_missed_events++;
		net->rcq_stats.n_missed_events++;
		goto resched;
	}
	else {
		/* "cq was emptied and no events were missed -
		    it is safe to wait for another event" */
		goto out;
	}

resched:
	defer_recv_interrupts_(net);

out:
	__NFOUT;
}

/* Kernel workqueue wrapper for poll_cq_and_process_work */
static inline void poll_cq_and_process_kwork(struct work_struct *kwork)
{
	struct nvmeibc_ib_net *net = container_of(
		kwork, struct nvmeibc_ib_net, defer_recv_kwork);
	poll_cq_and_process_common(net);
}

static inline void poll_cq_and_process_work(struct workqe_struct *work)
{
	struct nvmeibc_ib_net *net = container_of(
		work, struct nvmeibc_ib_net, defer_recv_work);
	poll_cq_and_process_common(net);
}

static int defer_recv_interrupts_(struct nvmeibc_ib_net *net)
{
	int rv;

	__NFIN;
	if (!net->nr_defer_recv_comps) {
		rv = -ENOTSUPP;
		goto out;
	}

	if (nvmeib_get_state_guard(&net->defer_recv_state) == NVMEIBC_IB_NET_DEFER_RECV_DISABLED) {
		rv = -ENOTSUPP;
		goto out;
	}

	if (!nvmeib_switch_state_guard(&net->defer_recv_state,
		NVMEIBC_IB_NET_DEFER_RECV_IDLE, NVMEIBC_IB_NET_DEFER_RECV_SCHEDULED)) {
		_NDn(defer_recv_interrupts__d1, net, "already on defer recv wq");
		net->rcq_stats.n_defer_wq_already++;
		rv = -EALREADY;
		goto out;
	}

	/* Use kernel workqueue if set, otherwise use custom workqueue */
	if (net->defer_recv_intr_kwq) {
		if (!queue_work(net->defer_recv_intr_kwq, &net->defer_recv_kwork)) {
			rv = -EALREADY;
			goto out;
		}
	} else if (!wq_add_work(net->defer_recv_intr_wq, &net->defer_recv_work)) {
		/* State check should prevent this from happening */
		BUG_ON(1);
		rv = -EALREADY;
		goto out;
	}

	nvmeib_qp_stats_on_offload_sched(net->qp_stats);
	net->rcq_stats.n_defer_poll++;
	rv = 0;

out:
	__NFOUT;
	return rv;
}


int nveibc_ib_net_defer_recv_interrupts_external(struct nvmeibc_ib_net *net)
{
	int rv;

	if (!nvmeib_ref_get(&net->ib_rsrc_ref)) {
		_NTn(trace_0_poll_cq_nr_wd_evt, net,
			 "Failed to get ib_rsrc");
		rv = -EBUSY;
	}
	else {
		if (net->peek_cq && (*net->peek_cq)(net->recv_cq, 1) == 0 &&
			(net->send_cq == net->recv_cq || (*net->peek_cq)(net->send_cq, 1) == 0)) {
			_NDn(trace_3_poll_cq_nr_wd_evt, net,
			     "CQ(s) Empty");
			rv = -ENOENT;
		} else {
			rv = defer_recv_interrupts_(net);
			if (rv == -EALREADY)
				_NDn(trace_2_poll_cq_nr_wd_evt, net,
				"Already scheduled");
			else if (rv < 0)
				_NTn(trace_1_poll_cq_nr_wd_evt, net,
					"Failed to add poll-cq work @RV", rv);
		}
		nvmeib_ref_put(&net->ib_rsrc_ref);
	}

	return rv;
}

static void intr_process_recv_cq_pcpu_func(void *info)
{
	struct nvmeibc_ib_net *net = info;
	unsigned long flags;
	unsigned long start, delta;

	__NFIN;
	nvmeib_intr_shaper_intr_enter(net->intr_shaper, INTR_SHAPER_INTR_TYPE_CLIENT_RCQ);
	BUG_ON(!nvmeibc_channel_is_ll_pcpu_ch(net->ioch));
	BUG_ON(nvmeibc_channel_pcpu_ch_get_cpu(net->ioch) != smp_processor_id());

	start = jiffies;
	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	net->rcq_stats.n_ipi_func++;
	if (net->rcq_kthread)
		intr_process_recv_cq_(net);
	else {
		enum req_notify_state req_notify_state;
		int rv, n_iter = 0;
		/* We have no offload thread, so no choice but to keep polling until notify works or we get an error */
		do {
			rv = process_recv_cq_(net, REQ_NOTIFY_TRUE, &req_notify_state);
			if (rv < 0 || req_notify_state == REQ_NOTIFY_STATE_NOTIFY_OK)
				break;
			if (++n_iter > nvmeibc_max_notify_cq_iterations) {
				_NEn_dmesg(err_intr_process_recv_cq_pcpu_func, net, "Failed to arm recv cq after @INT iterations, disconnecting", n_iter);
				nvmeibc_ib_net_disconnect(net);
				break;
			}
		} while (true);
	}
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);
	delta = jiffies - start;
	if (delta > net->rcq_stats.max_intr_duration) {
		net->rcq_stats.max_intr_duration = delta;
		_NDn(trace_ib_net_intr_process_recv_cq_pcpu_func, net, "max_intr_duration = @MAX_INTR_DURATION",
		     net->rcq_stats.max_intr_duration);
	}

	BUG_ON(atomic_xchg(&net->pcpu_recv_comp_smp_call.call_pending, 0) != 1);

	nvmeib_intr_shaper_intr_exit(net->intr_shaper);
	//NOTE: Ref count already increased by recv_completion_intr
	nvmeib_ref_put(&net->ib_rsrc_ref);
	__NFOUT;
}

static void recv_completion_intr(struct ib_cq *cq, void *net_ptr)
{
	struct nvmeibc_ib_net *net = NULL;
	struct nvmeibc_channel *ioch;
	unsigned long flags = 0;
	u64 start, delta;
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	void *val;
#endif
	int rv;

	__NFIN;
	net = verify_net_generation(net_ptr);
	if (!net)
		goto out;
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	if ((val = nvmeib_c_tree_lookup((unsigned long)net)) != net) {
		_NE(recv_completion_intr_e1,
			"OOPS, net=@PTR (val=@PTR) was deleted, event=@INT",
			net, val, (int)event->event);
		goto out;
	}
#endif

	nvmeib_qp_stats_on_interrupt(net->qp_stats);
	net->n_recv_intrs++;
	nvmeibc_disk_net_intrs_stats_inc(net->ioch->disk, false);

	if (!nvmeib_ref_get(&net->ib_rsrc_ref)) {
		_NTn(recv_completion_intr_t1, net, "failed to get ib_rsrc");
		goto out;
	}
	nvmeib_intr_shaper_intr_enter(net->intr_shaper, INTR_SHAPER_INTR_TYPE_CLIENT_RCQ);

	if (nvmeibc_channel_is_ll_pcpu_ch(net->ioch)) {
		if (net->ioch->ct == ct_n_rdda &&
			atomic_read(&net->ioch->disk->pcpu_nrch_poll[nvmeibc_channel_pcpu_ch_get_cpu(net->ioch)].open_cnt)) {
			/* NRCH Poller is connected, so ignore the interrupt */
			goto ref_put;
		} else if (smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(net->ioch)) {
			/* Interrupt came in on different CPU */
			if ((rv = defer_recv_interrupts_(net)) < 0 && rv != -EALREADY) {
				/* Deferring recv interrupts failed or not enabled. Use IPI to switch to correct CPU  (if not already pending) */
				if (atomic_cmpxchg(&net->pcpu_recv_comp_smp_call.call_pending, 0, 1) == 0) {
					if (nvmeib_public_smp_call_function_single_async(
							nvmeibc_channel_pcpu_ch_get_cpu(net->ioch),
							&net->pcpu_recv_comp_smp_call.call_data) < 0) {
						_NEn(recv_completion_intr_t2, net, "smp_call_function_single_async failed");
						BUG_ON(atomic_xchg(&net->pcpu_recv_comp_smp_call.call_pending, 0) != 1);
					} else {
						/* NOTE: We do not dec the ref count until the IPI runs so it does not get destroyed under out feet */
						nvmeib_intr_shaper_intr_exit(net->intr_shaper);
						goto out;
					}
				}
				goto ref_put;
			}
			goto ref_put;
		}
	}

	ioch = net->ioch;

	start = jiffies;

	if ((rv = defer_recv_interrupts_(net)) < 0 && rv != -EALREADY) {
		/* Deferring recv interrupts failed or not enabled */
		nvmeibc_channel_spin_lock_irqsave(ioch, &flags);
		if (net->rcq_kthread)
			intr_process_recv_cq_(net);
		else {
			enum req_notify_state req_notify_state;
			int rv, n_iter = 0;
			/* We have no offload thread, so no choice but to keep polling until notify works or we get an error */
			do {
				rv = process_recv_cq_(net, REQ_NOTIFY_TRUE, &req_notify_state);
				if (rv < 0 || req_notify_state == REQ_NOTIFY_STATE_NOTIFY_OK)
					break;
				if (++n_iter > nvmeibc_max_notify_cq_iterations) {
					_NEn_dmesg(err_recv_completion_intr, net, "Failed to arm recv cq after @INT iterations, disconnecting", n_iter);
					nvmeibc_ib_net_disconnect(net);
					break;
				}
			} while (true);
		}
		nvmeibc_channel_spin_unlock_irqrestore(ioch, flags);
	}

	delta = jiffies - start;
	if (delta > net->rcq_stats.max_intr_duration) {
		net->rcq_stats.max_intr_duration = delta;
		_NDn(recv_completion_intr_d2, net, "max_intr_duration = @INT64",
			 net->rcq_stats.max_intr_duration);
	}

ref_put:
	nvmeib_intr_shaper_intr_exit(net->intr_shaper);
	nvmeib_ref_put(&net->ib_rsrc_ref);

out:
	__NFOUT;
}

int nvmeibc_ib_net_process_recv_cq(struct nvmeibc_ib_net *net, int n)
{
	int rv, n_iter = 0;
	__NFIN;

	if (!nvmeibc_use_pcpu_cq) {
		enum req_notify_state req_notify_state;
		(void)n; /* TBD */
		do {
			rv = process_recv_cq_(net, REQ_NOTIFY_TRUE, &req_notify_state);
			if (rv < 0 || req_notify_state == REQ_NOTIFY_STATE_NOTIFY_OK)
				goto out;
			if (++n_iter > nvmeibc_max_notify_cq_iterations) {
				_NEn_dmesg(err_nvmeibc_ib_net_process_recv_cq, net, "Failed to arm recv cq after @INT iterations, disconnecting", n_iter);
				nvmeibc_ib_net_disconnect(net);
				rv = -EDEADLK;
				goto out;
			}
			cond_resched();
		} while(true);
	}
	else {
		_NEn(nvmeibc_ib_net_process_recv_cq_e1,
		     net, "OOPS, attempt process cq of shared percpu cq "
		     "from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		__builtin_return_address(0));
		WARN_ON_ONCE(1);
		rv = -EINVAL;
	}
out:
	__NFOUT;
	return rv;
}

static void cq_event(struct ib_event *event, void *net_ptr)
{
	struct nvmeibc_ib_net *net = net_ptr;
	#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	void *val;
	#endif

	__NFIN;

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	if ((val = nvmeib_c_tree_lookup((unsigned long)net)) != net) {
		_NE(err_c_net_cq_event_net_inval,
			"OOPS, net=@PTR (val=@PTR) was deleted, event=@INT",
			net, val, (int)event->event);
		return;
	}
#endif

	_NT(trace_c_net_cq_event, "CQ event @EVENT on net @NET, cm_id=@CM_ID qpn=@QPN state=@STATE, ch @IOCH_NAME",
		event->event, net, net->cm_id, net->qp->qp_num, nvmeib_get_state_guard(&net->state), net->ioch->name);

	__NFOUT;
}

/* -------------------------------------------------------------------------- *
 *                                                                            *
 * -------------------------------------------------------------------------- */
static int create_qp_private_cq(struct nvmeibc_ib_net *net,
	struct nvmeibc_ib_net_params *params)
{
	struct ib_device *ibdev = P2IB(net->port);
	struct ib_qp_init_attr *init_attr;
	struct ib_cq *recv_cq = NULL, *send_cq = NULL;
	struct ib_qp *qp;
	int recv_intr, send_intr, rv = -1;
	int qp_access = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE;
	unsigned long flags;

#ifdef NVMEIBC_NET_GEN_VERIFY
	const u64 mask = 0xff00000000000000ul;
	void *_net = (void*)(((u64)net & ~mask) | (((u64)net->gen) << 56));
#else
	void *_net = net;
#endif

	__NFIN;
	if (!(init_attr = kzalloc(sizeof(*init_attr), GFP_KERNEL))) {
		_NE(error_ib_net_create_qp, "Fail to allocate QP attributes");
		rv = -ENOMEM;
		goto out;
	}

	if (params->shared_cq) {
		nvmeib_cq_vector_get(P2NV(net->port), net->ioch ? net->ioch->name : "", params->ch_index, NULL, &recv_intr);
		_NTn(trace_0_ib_net_create_qp_shared_cq, net,
		     "Shared CQ to use dev @DEV_NAME, intr_vect=@INT",
			P2NV(net->port)->ib_dev->name, recv_intr);
	} else {
		nvmeib_cq_vector_get(P2NV(net->port), net->ioch ? net->ioch->name : "", params->ch_index, &send_intr, &recv_intr);
		_NTn(trace_0_ib_net_create_qp, net,
			"CQs to use dev @DEV_NAME, send_intr_vect=@INT, recv_intr_vec=@INT",
			P2NV(net->port)->ib_dev->name, send_intr, recv_intr);
	}

	recv_cq = nvmeib_create_cq(ibdev, recv_completion_intr,
				   cq_event, _net, params->max_recv_cq, recv_intr);
	if (IS_ERR(recv_cq)) {
		rv = PTR_ERR(recv_cq);
		_NE(error_1_ib_net_create_qp, "Failed in nvmeib_create_cq for recv_cq, rv=@RV", rv);
		rv = -1;
		goto out;
	}

	if (params->shared_cq) {
		_NTn(create_qp_private_cq_t1, net,
			 "sharing recv cq @PTR as send cq", send_cq);
		send_cq = recv_cq;
		send_intr = recv_intr;
	} else {
		//omril: we ask for send-cq size that might be > from our wc-poll-array...
		send_cq = nvmeib_create_cq(ibdev, send_completion_intr, cq_event, _net,
			params->max_send_cq, send_intr);
		if (IS_ERR(send_cq)) {
			rv = PTR_ERR(send_cq);
		_NE(error_2_ib_net_create_qp, "Failed in nvmeib_create_cq for send_cq, rv=@RV", rv);
			rv = -1;
			goto err_recv_cq;
		}
	}

	ib_req_notify_cq(recv_cq, IB_CQ_NEXT_COMP);
	if (net->rearm_send_cq)
		ib_req_notify_cq(send_cq, IB_CQ_NEXT_COMP);

	init_attr->event_handler = qp_event;
	init_attr->qp_context = net;
	init_attr->cap.max_send_wr = params->max_send_q;
	init_attr->cap.max_send_sge = params->max_send_sg;

	if (params->use_srq) {
		net->srq_info = params->srq_priv ? :
			nvmeib_srq_info_get(P2NV(net->port), params->srq_type);
		if (!net->srq_info) {
			_NE(error_3_ib_net_create_qp, "Failed to set srq-info");
			rv = -1;
			goto err_send_cq;
		}
		init_attr->srq = nvmeib_srq_info_ib_srq(net->srq_info);
	}
	else {
		init_attr->cap.max_recv_wr = params->max_recv_q > 0 ? params->max_recv_q + 1 : 0; /* If not 0, add one for the drain WQE */
		init_attr->cap.max_recv_sge = params->max_recv_sg;
	}
	init_attr->sq_sig_type = IB_SIGNAL_REQ_WR;
	init_attr->qp_type = IB_QPT_RC;

	init_attr->send_cq = send_cq;
	init_attr->recv_cq = recv_cq;
	if (params->use_atomic)
		qp_access |= IB_ACCESS_REMOTE_ATOMIC;
	qp = nvmeib_rdma_create_qp(net->cm_id, P2NV(net->port)->pd,
		init_attr, net->port->port, net->pkey, qp_access);
	if (!qp) {
		_NE(error_4_ib_net_create_qp, "Failed to create-qp");
		rv = -1;
		goto err_srq_info;
	}

	_NT(trace_ib_net_create_qp, "Created QP qpn=@QPN", qp->qp_num);
	nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	net->qp = qp;
	net->recv_cq = recv_cq;
	net->send_cq = send_cq;
	net->defer_recv_intr_wq = params->defer_recv_intr_wq;
	net->defer_recv_intr_kwq = params->defer_recv_intr_kwq;
	net->recv_intr_vec = recv_intr;
	net->send_intr_vec = send_intr;
	nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);
	if (net->defer_recv_intr_wq)
		_NTn(trace_ib_net_create_qp_defer_wq, net, "Using defer_recv_intr_wq pid @K_PID", wq_pid(net->defer_recv_intr_wq));
	else if (net->defer_recv_intr_kwq)
		_NTn(trace_ib_net_create_qp_defer_wqx, net, "Using defer_recv_intr_kwq");

	rv = 0;
	goto out;

err_srq_info:
	if (params->use_srq && net->srq_info && !params->srq_priv) {
		_NE(error_5_ib_net_create_qp, "net @NET, put srq-info", net);
		nvmeib_srq_info_put(net->srq_info, net);
		net->srq_info = NULL;
	}

err_send_cq:
	if (!params->shared_cq && send_cq)
		ib_destroy_cq(send_cq);

err_recv_cq:
	if (recv_cq)
		ib_destroy_cq(recv_cq);

out:
	if (init_attr)
		kfree(init_attr);
	__NFOUT;
	return rv;
}

static DEV_CQ_PROCESS_FUNC(process_per_dev_cq);
static int create_qp_per_dev_cq(struct nvmeibc_ib_net *net,
	struct nvmeibc_ib_net_params *params)
{
	struct ib_qp_init_attr *init_attr = NULL;
	struct nvmeib_cq_qp_destroy_action *qp_da = NULL;
	struct nvmeib_dev_cq *dev_cq = NULL;
	struct ib_qp *qp;
	int rv;
	int qp_access = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE |
		(params->use_atomic ? IB_ACCESS_REMOTE_ATOMIC : 0);

	__NFIN;
	_NT(create_qp_per_dev_cq_t1, "Start: net=@PTR", net);

	/* sanity */
	if (!nvmeibc_use_pcpu_cq || !params->use_srq) {
		_NT(create_qp_per_dev_cq_t2, "Invalid arg (@INT, @INT)",
			nvmeibc_use_pcpu_cq, params->use_srq);
		rv = -EINVAL;
		goto out;
	}
	if (net->dev_cq || net->qp) {
		_NE(create_qp_per_dev_cq_e1, "OOPS net @PTR inuse (@PTR, @PTR)",
			net, net->dev_cq, net->qp);
		rv = -EINVAL;
		goto out;
	}

	if (!(init_attr = kzalloc(sizeof(*init_attr), GFP_KERNEL)) ||
		!(qp_da = kzalloc(sizeof(*qp_da), GFP_KERNEL))) {
		_NE(create_qp_per_dev_cq_e2, "Fail to alloc");
		rv = -ENOMEM;
		goto err;
	}

	/* Get CQ from dev's pcpu pool */
	dev_cq = nvmeib_cq_get(P2NV(net->port),
		net->ioch ? net->ioch->ct : ct_other, process_per_dev_cq, false, params->comp_cpu);
	if (!dev_cq) {
		_NE(create_qp_per_dev_cq_e3, "Failed to get CQ from dev");
		rv = -ENODATA;
		goto err;
	}
	_NT(create_qp_per_dev_cq_t20, "Acquired CQ @PTR", dev_cq);

	/* Init QP attr and create QP */
	init_attr->srq = nvmeib_srq_info_ib_srq(nvmeib_cq_get_srq(dev_cq));
	init_attr->event_handler = c_rdma_qp_event;
	init_attr->qp_context = net->rdma_e_ctx;
	init_attr->cap.max_send_wr = params->max_send_q;
	init_attr->cap.max_send_sge = params->max_send_sg;
	init_attr->sq_sig_type = IB_SIGNAL_REQ_WR;
	init_attr->qp_type = IB_QPT_RC;
	init_attr->send_cq = nvmeib_cq_get_cq(dev_cq);
	init_attr->recv_cq = nvmeib_cq_get_cq(dev_cq);
	qp = nvmeib_rdma_create_qp(net->cm_id, P2NV(net->port)->pd,
		init_attr, net->port->port, net->pkey, qp_access);
	if (!qp) {
		_NE(create_qp_per_dev_cq_e200, "Failed to create-qp");
		rv = -1;
		goto err_dev_cq;
	}
	_NT(create_qp_per_dev_cq_t30, "Created QP @QP, qpn=@QP_NUM", qp, qp->qp_num);

	/* Add qp to dev-cq's */
	rv = nvmeib_cq_qp_add(dev_cq, (u64)qp, net);
	if (rv) {
		_NE(create_qp_per_dev_cq_e40,
			"Fail to add QP to dev CQ (rv=@INT)", rv);
		goto err_destroy_qp;
	}

	/* Populate net */
	net->qp = qp;
	net->dev_cq = dev_cq;
	net->recv_cq = nvmeib_cq_get_cq(dev_cq);
	net->send_cq = nvmeib_cq_get_cq(dev_cq);
	net->srq_info = nvmeib_cq_get_srq(dev_cq);
	net->recv_intr_vec = nvmeib_cq_get_intr(dev_cq);
	net->send_intr_vec = nvmeib_cq_get_intr(dev_cq);

	/* Mark cm so that only nvmeib_cq_qp_*() API may destroy qp and cm */
	nvmeib_rdma_cm_owner_set(net->cm_id, 0, -1);

	/* Populate net's action for destroying qp and cm */
	qp_da->qp = qp;
	qp_da->cm_id = net->cm_id;
	qp_da->rdma_e_ctx = net->rdma_e_ctx;
	qp_da->action.f = nvmeib_cq_qp_destroy_action_f;
	list_add(&qp_da->action.action_link, &net->qp_action_list);

	_NT(create_qp_per_dev_cq_t50,
		"net=@PTR, dev_cq=@PTR, send&recv-cq=@PTR, qp=@PTR, srq_info=@PTR",
		net, net->dev_cq, net->recv_cq, net->qp, net->srq_info);

	rv = 0;
	goto out;

err_destroy_qp:
	nvmeib_rdma_destroy_qp(net->cm_id, qp);

err_dev_cq:
	if (dev_cq)
		nvmeib_cq_put(
			P2NV(net->port), dev_cq, net->ioch ? net->ioch->ct : ct_other);
err:
	kfree(qp_da);

	/* clearance */
	BUG_ON(!list_empty(&net->qp_action_list));
	BUG_ON(net->dev_cq);
	BUG_ON(net->qp);

out:
	kfree(init_attr);

	_NT(create_qp_per_dev_cq_t51, "End: net=@PTR", net);
	__NFOUT;
	return rv;
}

static int create_qp(struct nvmeibc_ib_net *net,
	struct nvmeibc_ib_net_params *params)
{
	int rv;
	__NFIN;

	rv = !nvmeibc_use_pcpu_cq ?
		create_qp_private_cq(net, params) :
		create_qp_per_dev_cq(net, params);

	__NFOUT;
	return rv;
}
static void cm_rep_handler(struct nvmeibc_ib_net *net,
	struct nvmeibs_login_response *lrsp, int lrsp_len)
{
	int rv = 0;
	int n_ext = 0;

	__NFIN;
	if (!lrsp || lrsp_len == 0) {
		_NT(trace_ib_net_cm_rep_handler, "Invalid response from server");
		rv = -EINVAL;
		goto out;
	}

	if (lrsp_len > sizeof(struct nvmeibs_login_response_base)) {
		n_ext = lrsp->ext1.n_ext;

		if (n_ext >= 5) {
			if (lrsp->ext5.msg_hdr.out_flags == NVMEIBS_LOGIN_WITH_MSG_HDR) {
				_NT(trace_3_ib_net_cm_rep_handler, "Server uses outgoing msg header - not supported");
				rv = -ENOTSUPP;
				goto out;
			}
			if (lrsp->ext5.msg_hdr.in_flags == NVMEIBS_LOGIN_WITH_MSG_HDR) {
				_NT(trace_4_ib_net_cm_rep_handler, "Server expects incoming msg header - not supported");
				rv = -ENOTSUPP;
				goto out;
			}
		}
	}

	if (net->on_login(net, lrsp, n_ext) < 0) {
		_NT(trace_1_ib_net_cm_rep_handler, "on_login() failed");
		rv = -ECONNABORTED;
		goto out;
	}
	if (net->on_connected) {
		if ((rv = net->on_connected(net)) < 0)
			_NT(trace_2_ib_net_cm_rep_handler, "on_connected() failed");
	}

out:
	net->status = rv;
	__NFOUT;
}

static void notify_reject_unsupported_reversion(struct nvmeibc_ib_net *net,
			struct nvmeibs_login_reject *rej)
{
#define ALRT_LEN (192)
	u64 c_version = nvmeib_version_get().all;
	u64 s_version = be64_to_cpu(rej->s_version);
	union nvmeib_version *c_ver = (void *)(&c_version);
	union nvmeib_version *s_ver = (void *)(&s_version);
	const struct nvmeibc_cinst_params_main *pmain = nvmeibc_isnt_params_core2main(nvmeibc_net_get_cinst(net));
	const char *clnt_name = nvmeibc_get_utsname_nodename(pmain);
	static const char *fmt = "E%s Versions mismatch@ target=%pI6 c=%llx s=%llx";
	char *msg;

	_NE(error_ib_net_notify_reject_unsupported_reversion, "NVMEIBS_LOGIN_REJ_UNSUPPORTED_VERSION: "
	   "client: " NVMEIB_VERSION_TRACE_FMT() " (@C_VERSION) vs. "
	   "server: " NVMEIB_VERSION_TRACE_FMT() " (@S_VERSION)",
	   NVMEIB_VERSION_PRINT_ARG(c_ver), c_version,
	   NVMEIB_VERSION_PRINT_ARG(s_ver), s_version);

	msg = kzalloc(ALRT_LEN, GFP_ATOMIC);
	if (msg) {
		snprintf(msg, ALRT_LEN - 1, fmt, clnt_name, net->path.dgid.raw, c_version, s_version);
		nvmeibc_cc_api_send_mgmt_allert(pmain, msg, 0, false);
	}
}

static void handle_user_reject(struct nvmeibc_ib_net *net,
	struct nvmeibs_login_reject *rej)
{
	int opcode;
	struct nvmeibc_admin_rnic *arnic = NULL;

	__NFIN;
	if (net->admin_ch)
		 arnic = net->admin_ch->arnic;

	if (!rej) {
		_NTn(trace_ib_net_handle_user_reject_no_data, net, "NVMEIB_LOGIN_REJECT: Reject with NULL data");
		goto out;
	}

	opcode = rej->hdr.opcode;
	if (opcode == NVMEIB_LOGIN_REJ) {
		u32 reason = rej ? be32_to_cpu(rej->reason) : NVMEIBS_LOGIN_REJ_INVALID_CMD;
		switch (reason) {
		case NVMEIBS_LOGIN_REJ_UNABLE_ESTABLISH_CONNECTION:
			_NT(trace_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: requested connection is bad");
			break;
		case NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES:
			_NT(trace_1_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: server is out of resources");
			break;
		case NVMEIBS_LOGIN_REJ_REQ_IT_IU_INSUFF_CRED:
			_NT(trace_2_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: client has insufficient credentials");
			break;
		case NVMEIBS_LOGIN_REJ_CLIENT_IS_ALREADY_CONNECTED:
			_NT(trace_3_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: client is already "
				"connected to server port");
			break;
		case NVMEIBS_LOGIN_REJ_CLIENT_MSG_TOO_SMALL:
			_NT(trace_4_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: client msg is too big for client");
			break;
		case NVMEIBS_LOGIN_REJ_TOMA_NOT_CONNECTED:
			_NT(trace_5_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: toma service on"
				" the server side is down");
			if (arnic)
				ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_LOGIN_REMOTE_TOMA_DOWN);
			break;
		case NVMEIBS_LOGIN_REJ_INVALID_LOCK_PORT:
			_NT(trace_6_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: invalid lock port");
			break;
		case NVMEIBS_LOGIN_REJ_IO_CHANNEL_ALREADY_CONNECTED:
			_NT(trace_7_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT: channel already connected");
			break;
		case NVMEIBS_LOGIN_REJ_ADMIN_INVALID_GID:
			_NT(trace_8_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_ADMIN_INVALID_GID: Invalid GID for Admin Channel");
			break;
		case NVMEIBS_LOGIN_REJ_MODULE_EXIT:
			_NT(trace_9_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_MODULE_EXIT: nvmeibs module is shutting down");
			break;
		case NVMEIBS_LOGIN_REJ_PORT_DISABLED:
			_NT(trace_10_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_PORT_DISABLED: Network port has been disabled");
			break;
		case NVMEIBS_LOGIN_REJ_LOCAL_DISK_DYING:
			_NT(trace_11_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_LOCAL_DISK_DYING: Local disk is dying");
			break;
		case NVMEIBS_LOGIN_REJ_LOCAL_DISK_NOT_FOUND:
			_NT(trace_12_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_LOCAL_DISK_NOT_FOUND: Local disk not found");
			break;
		case NVMEIBS_LOGIN_REJ_LOCAL_DISK_INVALID_LOCKS:
			_NT(trace_13_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_LOCAL_DISK_DYING: Local disk has invalid lock table");
			break;
		case NVMEIBS_LOGIN_REJ_NO_PRIMARY_LOCK_CHANNEL:
			_NT(trace_14_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_NO_PRIMARY_LOCK_CHANNEL: Primary lock channel is not connected");
			break;
		case NVMEIBS_LOGIN_REJ_MAX_SECONDARY_LOCK_NETS:
			_NT(trace_15_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_MAX_SECONDARY_LOCK_NETS: Maximum secondary lock nets already connected");
			break;
		case NVMEIBS_LOGIN_REJ_UNSUPPORTED_VERSION:
			notify_reject_unsupported_reversion(net, rej);
			break;
		case NVMEIBS_LOGIN_REJ_LOCK_2ND_NO_PRIMARY:
			_NT(trace_16_ib_net_handle_user_reject, "NVMEIBS_LOGIN_REJ_LOCK_2ND_NO_PRIMARY: Primary lock net is dying");
			break;
		default:
			_NT(trace_17_ib_net_handle_user_reject, "NVMEIB_LOGIN_REJECT from @RAW_IPV6 to @RAW_IPV6 REJECTED, "
				"reason @REASON", net->path.sgid.raw, net->path.dgid.raw,
				reason);
			break;
		}
	} else
		_NT(trace_18_ib_net_handle_user_reject, "REJ reason: NO CLUE (@REASON), opcode @OPCODE",
			be32_to_cpu(rej->reason), opcode);

	if (net->on_reject)
		net->on_reject(net, rej);
out:
	net->status = -ECONNRESET;
	__NFOUT;
}

static void cm_rej_handler(struct nvmeibc_ib_net *net,
	struct nvmeib_rdma_event *event)
{
	__NFIN;
	switch (event->status) {
	case IB_CM_REJ_PORT_CM_REDIRECT:
		_NT(cm_rej_handler_t1, "REJ reason: IB_CM_REJ_PORT_CM_REDIRECT");
		net->status = -(int)(NVMEIB_PATH_REC_DLID(net->path) ?
			NVMEIBC_IB_DLID_REDIRECT : NVMEIBC_IB_PORT_REDIRECT);
		break;
	case IB_CM_REJ_PORT_REDIRECT:
		_NT(cm_rej_handler_t2, "REJ reason: IB_CM_REJ_PORT_REDIRECT");
		net->status = -ECONNRESET;
		break;
	case IB_CM_REJ_DUPLICATE_LOCAL_COMM_ID:
		_NT(cm_rej_handler_t3, "REJ reason: IB_CM_REJ_DUPLICATE_LOCAL_COMM_ID");
		net->status = -ECONNRESET;
		break;
	case IB_CM_REJ_CONSUMER_DEFINED:
		_NT(cm_rej_handler_t4, "REJ reason: IB_CM_REJ_CONSUMER_DEFINED");
		handle_user_reject(net, event->private_data);
		break;
	case IB_CM_REJ_STALE_CONN:
		_NT(cm_rej_handler_t5, "REJ reason: stale connection");
		net->status = -(int)NVMEIBC_IB_STALE_CONN;
		break;
	case IB_CM_REJ_INVALID_SERVICE_ID:
		_NT(cm_rej_handler_t6, "REJ reason: invalid service ID");
		net->status = -ECONNRESET;
		break;
	case IB_CM_REJ_NO_QP:
		_NT(cm_rej_handler_t7, "REJ reason: IB_CM_REJ_NO_QP");
		net->status = -ECONNRESET;
		break;
	default:
		_NT(trace_ib_net_cm_rej_handler, "REJ reason @STATUS", event->status);
		net->status = -ECONNRESET;
		break;
	}
	__NFOUT;
}

static void ib_net_peer_disconnect(struct nvmeibc_ib_net *net)
{
	unsigned long flags = 0;
	bool has_ioch = (net && net->ioch);
	bool already_locked;
	int dying, state;

	__NFIN;
	if (!has_ioch)
		goto out;
	already_locked = nvmeibc_channel_already_locked(net->ioch);
	if (!already_locked)
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);

	dying = atomic_inc_return(&net->dying);
	_NTn(trace_ib_net_ib_net_peer_disconnect, net, "inc dying to @DYING", dying);
	if (dying == 1) {
		/* here we can only send approve on the disconnect but
		   we cannot close the connection since the connection lock
		   is held by cm_handler.  we do not want to delay the disconenct
		   reply and thus we do not call the nvmeibc_ib_net_disconnect()
		*/
		if (net->cm_id) {
			if (nvmeib_rdma_disconnect_reply(net->cm_id))
				_NT(trace_1_ib_net_ib_net_peer_disconnect, "Sending CM DREP failed");
		}
		net->peer_disconnected = true;
		state = nvmeib_get_state_guard(&net->state);
		if (state == NVMEIBC_IB_NET_LIVE ||
			state == NVMEIBC_IB_NET_CONNECTING) {
			nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_DISCONNECTING);
			if (net->on_disconnect) {
				_NTn(trace_2_ib_net_ib_net_peer_disconnect, net, "initiate disconnect (state was @STATE)", state);
				/* on_disconnect_admin_ch
				   on_disconnect_lock_ch
				   on_disconnect_io_ch
				   on_disconnect_nordda_ch */
				net->on_disconnect(net);
			}
		}
		else
			_ND(trace_3_ib_net_ib_net_peer_disconnect, "not initiating net @NET disconnect, state @STATE", net, state);
	}
	if (!already_locked)
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

out:
	__NFOUT;
}

static void ib_net_peer_disconnect_smp_call_fn(void *ctx)
{
	struct nvmeibc_ib_net *net = ctx;

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	do {
		void *val;
		if ((val = nvmeib_c_tree_lookup((unsigned long)net)) != net) {
			_NE(nvmeibc_ib_net_ib_net_peer_disconnect_smp_call_fn_e1,
			"OOPS, net=@PTR (val=@PTR) was deleted", net, val);
			return;
		}
	} while(0);
#endif

	ib_net_peer_disconnect(net);
}

static int cm_handler(void *context, struct nvmeib_rdma_event *event)
{
	struct nvmeibc_ib_net *net = context;
	int comp = 0;
	int drep_comp = 0;
	u32 local_ib_id = (u32)-1;
	u32 remote_ib_id = (u32)-1;

	if (net && net->cm_id)
		nvmeib_rdma_cm_conn_get_ib_ids(net->cm_id, &local_ib_id, &remote_ib_id);

	NFIN;
	_ND(trace_ib_net_cm_handler, "net=@NET", net);
	switch (event->event) {
	case NVMEIB_REP_ERROR: /* oops bad connection request */
		_ND(trace_1_ib_net_cm_handler, "Sending CM REQ failed");
		comp = 1;
		net->status = -ECONNRESET;
		break;
	case NVMEIB_USER_ESTABLISHED: /* reply on connection request */
		_ND(trace_2_ib_net_cm_handler, "Connection accepted");
		_NT(trace_3_ib_net_cm_handler, "Received CM RESP (IDs: @LOCAL_IB_ID -> @REMOTE_IB_ID)", local_ib_id, remote_ib_id);
		comp = 1;
		cm_rep_handler(net, event->private_data, event->private_data_len);
		break;
	case NVMEIB_CONNECT_ERROR:
		_ND(trace_4_ib_net_cm_handler, "Connection accepted but failed to start it");
		comp = 1;
		net->status = event->status;
		break;
	case NVMEIB_REJ_RECEIVED: /* reject connection arrived */
		_ND(trace_5_ib_net_cm_handler, "REJ received: net=@NET", net);
		comp = 1;
		cm_rej_handler(net, event);
		break;
	case NVMEIB_DREQ_RECEIVED: /* disconnection from server arrived */
		_NT(trace_6_ib_net_cm_handler, "LOGOUT: DREQ received - connection closed: net=@NET",
		 net);
		/* let the upper layer block device know that we are having an issue */
		if (nvmeibc_channel_is_ll_pcpu_ch(net->ioch)) {
			/* Per-CPU Channel => Trigger IPI to call ib_net_peer_disconnect on correct CPU (Question: Do we need to wait?) */
			smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(net->ioch), ib_net_peer_disconnect_smp_call_fn, net, false);
		} else
			ib_net_peer_disconnect(net);
		break;
	case NVMEIB_DREP_RECEIVED:
		_ND(trace_7_ib_net_cm_handler, "DREP received");
		drep_comp = 1;
		break;
	case NVMEIB_DEVICE_REMOVED:
		_NT(trace_8_ib_net_cm_handler, "Device Removed on net @NET", net);
		if (nvmeibc_channel_is_ll_pcpu_ch(net->ioch)) {
			/* Per-CPU Channel => Trigger IPI to call ib_net_peer_disconnect on correct CPU (Question: Do we need to wait?) */
			smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(net->ioch), ib_net_peer_disconnect_smp_call_fn, net, false);
		} else
			ib_net_peer_disconnect(net);
		break;
	case NVMEIB_TIMEWAIT_EXIT:
		_ND(trace_9_ib_net_cm_handler, "connection closed");
		comp = 1;
		drep_comp = 1;
		net->status = 0;
		break;
	default:
		_NW(warn_ib_net_cm_handler, "Unhandled CM event @EVENT", event->event);
		break;
	}

	if (net) {
		if (comp)
			complete(&net->connect_done);
		if (drep_comp) {
			complete(&net->wait_for_drep);
		}
	}

	__NFOUT;
	return 0;
}

static int c_rdma_cm_event(void *context, struct nvmeib_rdma_event *event)
{
	struct nvmeib_rdma_evt_ctx *c = context;
	struct nvmeibc_ib_net *net;
	int rv = 0;

	net = nvmeib_rdma_evt_ctx_get(c);
	if (net) {
		rv = cm_handler(net, event);
		nvmeib_rdma_evt_ctx_put(c);
	}
	else {
		_NT(c_rdma_cm_event_t1,
			"Fail to get ctx @PTR (evt=@INT)", c, event->event);
	}

	return rv;
}

static int connect_qp(struct nvmeibc_ib_net *net,
	struct nvmeibc_ib_net_params *params, struct nvmeibc_login_request *lreq)
{
	struct rdma_conn_param conn_params = {0};
	u32 local_ib_id = (u32)-1;
	int state;
	int rv;

	NFIN;
	conn_params.private_data = lreq;
	conn_params.private_data_len = sizeof(*lreq);
	conn_params.initiator_depth = params->max_rd_atomic;
	conn_params.responder_resources = params->max_dest_rd_atomic;
	conn_params.flow_control = 1;
	conn_params.retry_count = NVMEIB_RETRY_CNT;
	conn_params.rnr_retry_count = 7;
	/* Fields below ignored if a QP is created on the rdma_cm_id. */
	conn_params.srq = net->srq_info ? 1 : 0;
	conn_params.qp_num = net->qp->qp_num;
	/* wait for the connection to established */
	init_completion(&net->connect_done);
	init_completion(&net->wait_for_drep);
	nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_CONNECTING);

	if (!nvmeibc_use_pcpu_cq)
		rv = nvmeib_rdma_connect(net->cm_id, &conn_params, cm_handler, net);
	else
		rv = nvmeib_rdma_connect(net->cm_id, &conn_params, c_rdma_cm_event, net->rdma_e_ctx);

	if (rv) {
		_NTn(trace_ib_net_connect_qp, net, "Failed to send login request ib_dev=@STR rv=@RV",
			net->port->nic_dev->dev->ib_dev->name, rv);
		goto out;
	}

	nvmeib_rdma_cm_conn_get_ib_ids(net->cm_id, &local_ib_id, NULL);
	_NT(trace_1_ib_net_connect_qp, "Sent CM REQ (ID: @LOCAL_IB_ID)", local_ib_id);

	if ((rv = wait_for_completion_interruptible_timeout(&net->connect_done,
			NVMEIB_WAIT_FOR_CONNECTION)) <= 0) {
		unsigned long start_jiffs;
		unsigned int timeout = NVMEIB_WAIT_FOR_CM_REP_TIMEOUT;

		_NT(trace_2_ib_net_connect_qp, "Failed to wait for cm connection reply (rv @RV)", rv);

		if (nvmeib_rdma_try_inv_cm(net->cm_id)) {
			_NT(trace_3_ib_net_connect_qp, "invalidated cm");
			rv = -ETIMEDOUT;
			goto out;
		}

		_NTn(trace_4_ib_net_connect_qp, net, "cm response already received, waiting for cm-handler");
		start_jiffs = jiffies;
		do {
			rv = wait_for_completion_interruptible_timeout(&net->connect_done, timeout);
			if (rv < 0) {
				if (rv == -ERESTARTSYS)
					_NWn(warn_ib_net_connect_qp, net, "Interrupted while waiting for cm-handler, continuing. (This might crash)");
				else
					_NWn(warn_1_ib_net_connect_qp, net, "Invalid value (@RV) returned from wait_for_completion_interruptible_timeout", rv);
				goto out;
                        }
			else if (rv == 0) {
				_NWn(warn_2_ib_net_connect_qp, net, "cm-handler not finished after @DIFF secs. Continuing to wait", (jiffies - start_jiffs) / HZ);
				if (timeout < NVMEIB_WAIT_FOR_CM_REP_MAX_TIMEOUT)
					timeout <<= 2;
			}
		} while (rv <= 0);
		_NTn(trace_5_ib_net_connect_qp, net, "cm-handler eventually finished after @DIFF secs", (jiffies - start_jiffs) / HZ);
	}

	rv = net->status;
	if (!rv) {
		/* Set the timeout */
		struct ib_qp_attr qp_attr = {0};
		struct ib_qp_init_attr qp_init_attr = {0};

		/* Get the net's destination QPnum, SQ PSN, retry count and retry timeout */
		rv = ib_query_qp(net->qp, &qp_attr, IB_QP_RETRY_CNT | IB_QP_TIMEOUT |
					IB_QP_DEST_QPN | IB_QP_SQ_PSN |
					IB_QP_MAX_QP_RD_ATOMIC | IB_QP_MAX_DEST_RD_ATOMIC, &qp_init_attr);
		_NT(trace_6_ib_net_connect_qp, "ib_query_qp (@RV) QPn: @QP_NUM Retry Count: @RETRY_CNT Timeout: @TIMEOUT Max Rd Atomic: @MAX_RD_ATOMIC Dest Max Rd Atomic: @MAX_DEST_RD_ATOMIC",
			rv, net->qp->qp_num, qp_attr.retry_cnt, qp_attr.timeout,
			qp_attr.max_rd_atomic, qp_attr.max_dest_rd_atomic);
		rv = 0;

		nvmeib_rdma_get_remote_qpn(net->cm_id, &net->remote_qpn);
		net->sq_psn = qp_attr.sq_psn;
		net->max_rd_atomic = qp_attr.max_rd_atomic;
		net->max_dest_rd_atomic = qp_attr.max_dest_rd_atomic;

		/* Calc the WD timeout in jiffies based on the retry count and timeout (minimum of NVMEIBC_IO_TIMEOUT seconds)
			* -------------------------------------------------------------------
			* We use twice the retry count x timeout (which is in usec) for the WD timeout
			* The timeout (in usec) is determined by the formulat 4.096 * 2 ^ value, which we can simplify to:
			* 1 << (value + 2) (usec). We can simplify the whole thing to be:
			* (retry_count << (timeout + 3)) (usec) or approx:
			* (retry_count << ((timeout + 3) - 20)) (sec) or approx:
			* (retry_count << ((timeout + 3) - (20 - SHIFT_HZ))) (jiffies)
			*/
		net->wd_timeout_jif = (qp_attr.timeout + 3) > (20 - SHIFT_HZ) ?
			(qp_attr.retry_cnt << ((qp_attr.timeout + 3) - (20 - SHIFT_HZ))) :
			(NVMEIBC_MIN_IO_TIMEOUT * HZ);
		if (net->wd_timeout_jif < (NVMEIBC_MIN_IO_TIMEOUT * HZ)) {
			net->wd_timeout_jif = (NVMEIBC_MIN_IO_TIMEOUT * HZ);
		}


		/* attempt change net's state from CONNECTING to LIVE,
		   protect from any evnet (cm/qp) that might have already
		   changed to DISCONNECTING. In any case, return rv=0 */

		//Other option:
		//dont let events changing state and scheduling remove-work
		//before we switched to LIVE. Instead let they mark dying under
		//a lock and check it here before changing to LIVE.
		if (!nvmeib_switch_state_guard(&net->state, NVMEIBC_IB_NET_CONNECTING, NVMEIBC_IB_NET_LIVE)) {
			state = nvmeib_get_state_guard(&net->state);
			_NE(error_ib_net_connect_qp, "net @NET, cannot change from @IB_NET_STATE_STR(@STATE) to LIVE",
			   net, nvmeibc_ib_net_state_str(state), state);
			rv = -ECONNABORTED;
		}
	}

out:
	nvmeib_reinit_completion(&net->connect_done);
	NFOUT;
	return rv;
}

void nvmeibc_ib_net_free(struct nvmeibc_ib_net *net)
{
	__NFIN;

	_NTn(trace_ib_net_nvmeibc_ib_net_free, net, "Start free net");
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	nvmeib_c_tree_del((unsigned long)net);
#endif

	if (!nvmeibc_use_pcpu_cq) {
		scq_kthread_stop(net);

		/* in case net was not fully created/enabled */
		rcq_kthread_stop(net);

		if (net->srq_info) {
			nvmeib_srq_info_put(net->srq_info, net);
			net->srq_info = NULL;
		}
	}

	nvmeib_ref_release_start(&net->ib_rsrc_ref);
	nvmeib_ref_release_wait(&net->ib_rsrc_ref);

	if (!nvmeibc_use_pcpu_cq) {
		if (net->port && P2NV(net->port)->dev_type == DT_siw) {
			/* For SIW, we need to destroy the CM first otherwise destroy_qp will not free the QP */
			nvmeibc_ib_net_destroy_cm(net);
		}
		if (net->qp) {
			_NT(trace_1_ib_net_nvmeibc_ib_net_free,
				"Destroy QP qpn=@QPN", net->qp->qp_num);
			nvmeib_rdma_destroy_qp(net->cm_id, net->qp);
			net->qp = NULL;
		}
		nvmeibc_ib_net_destroy_cm(net);

		if (net->send_cq) {
			if (!net->shared_cq)
				ib_destroy_cq(net->send_cq);
			net->send_cq = NULL;
		}
		if (net->recv_cq) {
			ib_destroy_cq(net->recv_cq);
			net->recv_cq = NULL;
		}
	}
	else {
		int dying = atomic_read(&net->dying);
		_NT(nvmeibc_ib_net_free_t50,
			"SCQ: net=@PTR, dev_cq=@PTR, qp=@PTR, dying=@INT",
			net, net->dev_cq, net->qp, dying);
		/* break-qp should have already detached qp from pcpu-cq, well,
		   except from cases where it had not run i.e. net-disconnect
		   was skipped e.g. nvmeibc_disk-free_unused_admin_ch() */

		//AAA: dont run net-free w/o running net-break-qp first
		if (net->rdma_e_ctx || net->dev_cq) {
			//WARN_ON_ONCE(1);
			detach_from_per_dev_cq(net);
		}
	}

#ifdef NVMEIBC_DEBUG_FR_LEAK
	if (net->port && P2NV(net->port)->use_fast_reg && P2NV(net->port)->fr_pool) {
		unsigned long flags;
		struct nvmeib_fr_desc *d;
		spin_lock_irqsave(&P2NV(net->port)->fr_pool->lock, flags);
		list_for_each_entry(d, &P2NV(net->port)->fr_pool->used_list, used_entry) {
			if (d->owner == net) {
				_NW_dmesg(nvmeibc_ib_net_free_fr_leak, "detected leaked FR pool (@POOL) for @NET - @PX", P2NV(net->port)->fr_pool, net, d);
				BUG_NON_PRODUCTION(8091);
			}
		}
		spin_unlock_irqrestore(&P2NV(net->port)->fr_pool->lock, flags);
	}
#endif

	if (net->on_free) {
		net->on_free(net);
	}
	/* mark that the net is cleared */
	nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_REMOVED);
	net->on_disconnect = NULL;
	kfree(net->break_qp);
	net->break_qp = NULL;

	if (!nvmeibc_use_pcpu_cq) {
		kfree(net->wc_s);
		net->wc_s = NULL;
		kfree(net->wc_r);
		net->wc_r = NULL;
		kfree(net->wc_mixed);
		net->wc_mixed = NULL;
		nvmeib_rdma_free_siw_wc_md(net->wc_md_s, 0, NULL);
		net->wc_md_s = NULL;
		nvmeib_rdma_free_siw_wc_md(net->wc_md_r, 0, NULL);
		net->wc_md_r = NULL;
		nvmeib_rdma_free_siw_wc_md(net->wc_md_mixed, 0, NULL);
		net->wc_md_mixed = NULL;
	}

	if (net->io_ka.src) {
		/* free the io_ka source */
		nvmeib_public_ib_dma_free_coherent(P2IB(net->port), net->io_ka.src_len,
						net->io_ka.src, net->io_ka.src_dma);
		net->io_ka.src = NULL;
		net->io_ka.src_len = 0;
		net->io_ka.src_dma = 0;
		net->io_ka.in_progress = false;
	}
	nvmeibc_ib_net_poison_unmap(net);

	net->peer_disconnected = false;

	memset(&net->rcq_stats, 0, sizeof(net->rcq_stats));
	memset(&net->scq_stats, 0, sizeof(net->scq_stats));

	if (net->qp_stats) {
		nvmeib_qp_stats_free(net->qp_stats);
		net->qp_stats = NULL;
	}

	_NTn(trace_2_ib_net_nvmeibc_ib_net_free, net, "End free net");
	__NFOUT;
}

static void clear_net(struct nvmeibc_ib_net *net)
{
	NFIN;
	net->on_connected = NULL;
	net->on_connected_clear = NULL;
	net->on_remove_work = NULL;
	net->on_login = NULL;
	net->on_reject = NULL;
	net->on_disconnect = NULL;
	net->on_free = NULL;
	net->call_send_comp_handler = NULL;
	net->call_receive_comp_handler = NULL;
	net->poll_interrupts = NULL;
	net->status = 0;
	NFOUT;
}

static bool free_premature_net(struct nvmeibc_ib_net *net)
{
	unsigned long flags = -1;
	bool already_locked;
	int dying, state;
	int break_free = false;

	NFIN;
	_NT(trace_ib_net_free_premature_net, "Disconnect and free ib and cm");
	already_locked = nvmeibc_channel_already_locked(net->ioch);
	if (!already_locked)
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);

	dying = atomic_inc_return(&net->dying);
	_NTn(trace_1_ib_net_free_premature_net, net, "inc dying to @DYING", dying);
	if (dying == 1) {
		state = nvmeib_get_state_guard(&net->state);
		if (state == NVMEIBC_IB_NET_CONNECTING) {
			nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_DISCONNECTING);
			break_free = true;
		}
	}

	if (!already_locked)
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);

	if (break_free) {
		nvmeibc_ib_net_break_qp(net);
		nvmeibc_ib_net_free(net);
	}

	NFOUT;
	return break_free;
}

int nvmeibc_ib_net_alloc(struct nvmeibc_ib_net *net,
	struct nvmeibc_ib_net_params *params,
	struct nvmeibc_login_request *lreq)
{
	struct nvmeib_rdma_path_info info;
	struct ib_wc *wc_s = NULL;
	struct ib_wc *wc_r = NULL;
	struct ib_wc *wc_m = NULL;
	/* funny but the lines do not fit in 80 chars if i use the constant */
	int n_wc_s = min(NVMEIBC_IB_NET_WC_ARR_SIZE, params->max_send_cq);
	int n_wc_r = min(NVMEIBC_IB_NET_WC_ARR_SIZE, params->max_recv_cq);
	int n_wc_m = min(n_wc_s, n_wc_r); /* we first poll into this buffer */
	void *wc_md_s = NULL;
	void *wc_md_r = NULL;
	void *wc_md_m = NULL;
	int rv = 0;

	NFIN;

	_NTn(trace_ib_net_nvmeibc_ib_net_alloc, net, "allocate @LOGIN_OPS_TO_STR - start",
		nvmeibc_login_ops_to_str(nvmeib_wire_op_cid_get_req_opcode(&lreq->op_cid)));

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	nvmeib_c_tree_add((unsigned long)net, net);
#endif

	if (!nvmeibc_use_pcpu_cq) {
		if (params->rcq_offload_enb && (params->defer_recv_intr_wq || params->defer_recv_intr_kwq)) {
			_NT(trace_defer_recv_intr_wq_invalid_params, "Invalid params: mutual exclusive features");
			rv = -1;
			goto out;
		}
		if (params->defer_recv_intr_wq && params->defer_recv_intr_kwq) {
			_NT(trace_1_ib_net_nvmeibc_ib_net_alloc, "Invalid params: both defer_recv_intr_wq and defer_recv_intr_kwq set");
			rv = -1;
			goto out;
		}
	}
	else {
		if (!params->use_srq) {
			_NT(trace_1a_ib_net_nvmeibc_ib_net_alloc, "Invalid params: RQ with shared percpu cq");
			rv = -1;
			goto out;
		}
	}

	if (!(net->qp_stats = nvmeib_qp_stats_alloc())) {
		_NE(nvmeibc_ib_net_alloc_dev_stats, "Fail to alloc pcpu-stats");
		goto out;
	}

	if (nvmeibc_use_pcpu_cq) {
		/* create and init ctx for rdma (qp and cm) events handlers */
		BUG_ON(net->rdma_e_ctx);
		if (!(net->rdma_e_ctx = nvmeib_rdma_evt_ctx_create(net))) {
			_NE(nvmeibc_ib_net_alloc_e100, "Fail create rdma-evt-ctx");
			goto free_stats;
		}
	}

	nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_ERR);
	nvmeib_ref_init(&net->ib_rsrc_ref);
	spin_lock_init(&net->comp_guard);
	INIT_LIST_HEAD(&net->qp_action_list);

	/* create the connection descriptor and
	   register cm events callback ([ib, roce, ...]_conn_cm_handler) */
	if (!(net->cm_id = nvmeib_rdma_create_cm(P2NV(net->port),
		net->port->port, net->cm_rdma_type))) {
		_NTn(trace_2_ib_net_nvmeibc_ib_net_alloc, net, "Fail to create connection descriptor");
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
		nvmeib_c_tree_del((unsigned long)net);
#endif
		rv = -1;
		goto free_ctx;
	}
	_NTn(trace_3_ib_net_nvmeibc_ib_net_alloc, net, "created net->cm_id=@CM_ID", net->cm_id);

	info.dev = P2NV(net->port);
	info.sa = nvmeibc_sa_client(nvmeibc_cinst_get_core_p(&net->admin_ch->base));
	info.path = &net->path;
	info.src_port = net->port->port;
	info.pkey = net->pkey;
	info.service_id = net->service_id;
	info.service_port = net->service_port;
	//nvmeib_rdma_print_path(info.path);
	if ((rv = nvmeib_rdma_start_path_connection(net->cm_id, &info)) < 0) {
		_NTn(trace_4_ib_net_nvmeibc_ib_net_alloc, net, "Failed (@RV) to start path @SGID -> @DGID",
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
		nvmeib_c_tree_del((unsigned long)net);
#endif
			rv, &net->path.sgid, &net->path.dgid);
		goto free_cm;
	}

	net->gen++;
	net->on_connected = params->on_connected;
	net->on_connected_clear = params->on_connected_clear;
	net->on_remove_work = params->on_remove_work;
	net->on_login = params->on_login;
	net->on_reject = params->on_reject;
	net->on_disconnect = params->on_disconnect;
	net->on_free = params->on_free;
	net->on_poison = params->on_poison;
	net->call_send_comp_handler = params->call_send_comp_handler;
	net->call_receive_comp_handler = params->call_receive_comp_handler;
	net->poll_interrupts = params->poll_interrupts;
	net->post_send_atomic_fn = P2NV(net->port)->post_send_atomic_fn;
	net->peek_cq = P2NV(net->port)->peek_cq;
	net->nr_defer_recv_comps = params->nr_defer_recv_comps;

	//reset reuseable members
	net->qp_evt_last_wqe = 0;
	net->qp_evt_qp_fatal = 0;
	net->qp_rq_drain_recv = 0;
	net->n_recv_intrs = 0;
	net->n_send_intrs = 0;
	net->recv_intr_vec = 0;
	net->send_intr_vec = 0;

	/* These are used for nets with lock-less per-cpu channels that need to schedule IPIs */
	memset(&net->pcpu_send_comp_smp_call, 0, sizeof(net->pcpu_send_comp_smp_call));
	atomic_set(&net->pcpu_send_comp_smp_call.call_pending, 0);
	net->pcpu_send_comp_smp_call.call_data.func = intr_process_send_cq_pcpu_func;
	net->pcpu_send_comp_smp_call.call_data.info = net;

	memset(&net->pcpu_recv_comp_smp_call, 0, sizeof(net->pcpu_recv_comp_smp_call));
	atomic_set(&net->pcpu_recv_comp_smp_call.call_pending, 0);
	net->pcpu_recv_comp_smp_call.call_data.func = intr_process_recv_cq_pcpu_func;
	net->pcpu_recv_comp_smp_call.call_data.info = net;

	if (!nvmeibc_use_pcpu_cq) {
		net->rcq_kthread = NULL;
		net->rcq_poll_mode = NVMEIBC_IB_CQ_INTR;
		//memset(&net->rcq_stats, 0, sizeof(net->rcq_stats));

		net->scq_kthread = NULL;
		net->scq_kwq = NULL;
		net->scq_poll_mode = NVMEIBC_IB_CQ_INTR;
		//memset(&net->scq_stats, 0, sizeof(net->scq_stats));

		net->shared_cq = params->shared_cq;
		net->defer_recv_intr_wq = params->defer_recv_intr_wq;
		net->defer_recv_intr_kwq = params->defer_recv_intr_kwq;
		nvmeib_init_state_guard(&net->defer_recv_state,
							   ((params->defer_recv_intr_wq || params->defer_recv_intr_kwq) ? NVMEIBC_IB_NET_DEFER_RECV_IDLE : NVMEIBC_IB_NET_DEFER_RECV_DISABLED));
	}
	//TODO: do we use this stats in pcpu-cq mode?
	memset(&net->rcq_stats, 0, sizeof(net->rcq_stats));
	memset(&net->scq_stats, 0, sizeof(net->scq_stats));

	/* create rcq, scq, arm intrrupts, create QP and modify its state to INIT */
	if ((rv = create_qp(net, params)) < 0) {
		_NTn(trace_5_ib_net_nvmeibc_ib_net_alloc, net, "nvmeibc_create_qp(): failed");
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
		nvmeib_c_tree_del((unsigned long)net);
#endif
		goto free_cm;
	}

	if (!nvmeibc_use_pcpu_cq) {
		/* Alloc completions array */
		if (!((wc_s = kzalloc(sizeof(*wc_s) * n_wc_s, GFP_KERNEL)) &&
			  (wc_r = kzalloc(sizeof(*wc_r) * n_wc_r, GFP_KERNEL)) &&
			  (wc_m = kzalloc(sizeof(*wc_m) * n_wc_m, GFP_KERNEL)))) {
			_NTn(trace_6_ib_net_nvmeibc_ib_net_alloc, net, "Failed to allocate send/receive completions");
			kfree(wc_s);
			kfree(wc_r);
			kfree(wc_m);
			rv = -1;
			goto err_free_ib;
		}
		else {
			if (!nvmeibc_nr_lat_meas_alloc_wc_md(params->alloc_siw_wc_md,
							wc_s, wc_r, wc_m,
							n_wc_s, n_wc_r, n_wc_m,
							&wc_md_s, &wc_md_r, &wc_md_m)) {
				_NTn(trace_c_net_lat_meas_alloc_wc_md_oom, net, "Failed to allocate siw_wc_md");
				/* Can continue, just without the MD
				 * (We check for wc_flags & SIW_IB_WC_WITH_SIW_MD before dereferencing the in wr_id) */
			}

			net->n_wc_s = n_wc_s;
			net->n_wc_r = n_wc_r;
			net->n_wc_mixed = n_wc_m;
			net->wc_s = wc_s;
			net->wc_r = wc_r;
			net->wc_mixed = wc_m;
			net->wc_md_s = wc_md_s;
			net->wc_md_r = wc_md_r;
			net->wc_md_mixed = wc_md_m;
		}

		if (params->rcq_offload_enb) {
			if ((rv = rcq_kthread_create_(net, params->comp_cpu)) < 0) {
				_NE(nvmeibc_ib_net_alloc_e99, "Fail to create rcq thread");
				goto err_free_ib;
			}
		}

		if (params->scq_kwq) {
			/* Use kernel workqueue */
			net->scq_kwq = params->scq_kwq;
			net->scq_kthread = NULL;
		} else if (params->scq_offload_enb) {
			/* Use kthread */
			if ((rv = scq_kthread_create_(net, params->comp_cpu)) < 0) {
				_NE(error_1_ib_net_nvmeibc_ib_net_alloc, "Fail to create scq thread");
				goto err_free_ib;
			}
		}
	}

	if (params->recv_q) {
		int rq_num_posted;
		net->recv_q = params->recv_q;
		nvmeib_fill_recvq(net->recv_q, net->qp, &rq_num_posted);
		atomic_set(&net->rq_post_count, rq_num_posted + 1);
	} else {
		net->recv_q = NULL;
		atomic_set(&net->rq_post_count, 0);
	}

	nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_INIT);
	net->intr_shaper = nvmeibc_get_shaper(nvmeibc_net_get_cinst(net));
	/* send cm conn req and wait for IB_CM_REP_RECEIVED cm-event.
	   on-reply modify QP ->RTR->RTS and send cm RTU */
	if ((rv = connect_qp(net, params, lreq)) < 0) {
		_NTn(trace_7_ib_net_nvmeibc_ib_net_alloc, net, "Connection failed (@RV)", rv);
		if (params->free_net_on_connect_err)
			goto err_free_ib;
		else
			goto out;
	}
	if (net->ioch->ct == ct_rdda || net->ioch->ct == ct_n_rdda) {
		/* Allocate source memory for IO-KA */
		net->io_ka.src_len = NVMEIB_IOCH_KA_WRITE_LEN;
		/* JH IOMMU: DMA_TO_DEVICE is correct, only used for source for Local RDMA_WRITE */
		if (!(net->io_ka.src = nvmeib_public_ib_dma_alloc_coherent(P2IB(net->port),
			net->io_ka.src_len, &net->io_ka.src_dma, GFP_KERNEL))) {
			_NTn(trace_8_ib_net_nvmeibc_ib_net_alloc, net, "Failed to allocate memory for IO-KA");
		}
	}
	if (net->ioch->ct == ct_rdda) {
		if (nvmeibc_ib_net_poison_map(net)) {
			_NTn(trace_10_ib_net_nvmeibc_ib_net_alloc, net, "Failed to map poison page");
			rv = -1;
			goto out;
		}
	}

	WQ_INIT_WORK(&net->defer_recv_work, poll_cq_and_process_work);
	INIT_WORK(&net->defer_recv_kwork, poll_cq_and_process_kwork);
	INIT_WORK(&net->scq_kwork, scq_kwork_func);

	_NTn(trace_9_ib_net_nvmeibc_ib_net_alloc, net, "allocate - done");
	goto out;

err_free_ib:
	if (!free_premature_net(net)) {
		_NTn(trace_11_ib_net_nvmeibc_ib_net_alloc, net,
		     "dying was set elsewhere, not freed here");
		goto out;
	}

free_cm:
	/* If free_premature_net fails when nvmeibc_use_pcpu_cq is on, then the owner of the cm_id is -1.
	 * The cm_id will be destroyed by nvmeib_cq_qp_destroy_action_f called by the pcpu cq callback */
	if (net->cm_id && nvmeib_rdma_cm_is_owner(net->cm_id))
		nvmeibc_ib_net_destroy_cm(net); //omril: move this to undo-code
	nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_ERR);
	clear_net(net);

free_ctx:
	if (net->rdma_e_ctx) {
		nvmeib_rdma_evt_ctx_stop(net->rdma_e_ctx);
		nvmeib_rdma_evt_ctx_destroy(net->rdma_e_ctx);
		net->rdma_e_ctx = NULL;
	}

free_stats:
	if (net->qp_stats) {
		nvmeib_qp_stats_free(net->qp_stats);
		net->qp_stats = NULL;
	}

out:
	__NFOUT;
	return rv;
}

/* this function is called only when we initiate the disconenct. */
void nvmeibc_ib_net_disconnect(struct nvmeibc_ib_net *net)
{
	unsigned long flags = -1;
	bool already_locked;

	__NFIN;
	_NT(trace_ib_net_nvmeibc_ib_net_disconnect, "Locking ch @IOCH", net->ioch);
	already_locked = nvmeibc_channel_already_locked(net->ioch);
	if (!already_locked)
		nvmeibc_channel_spin_lock_irqsave(net->ioch, &flags);
	_NT(trace_1_ib_net_nvmeibc_ib_net_disconnect, "Locked");
	nvmeibc_ib_net_disconnect_(net);
	if (!already_locked)
		nvmeibc_channel_spin_unlock_irqrestore(net->ioch, flags);
	_NT(trace_2_ib_net_nvmeibc_ib_net_disconnect, "Unlocked");
	__NFOUT;
}

/**
 * nvmeibc_map_desc() - update an RDMA desriptor
 *
 */
static void map_desc(struct nvmeibc_map_state *state, dma_addr_t dma_addr,
	unsigned dma_len, u32 key, u32 okey)
{
	struct nvmeib_direct_buf *desc = state->desc;

	NFIN;
	desc->va = dma_addr;
	desc->key = key;
	desc->len = dma_len;
	desc->okey = okey;

	state->total_len += dma_len;
	state->desc++;
	state->ndesc++;
	NFOUT;
}

static int finish_mapping(struct nvmeibc_ib_net *net,
	struct nvmeibc_map_state *state, u32 key, u32 okey)
{
	struct nvmeib_dev *nvdev = P2NV(net->port);
	struct nvmeib_mr_info mri = {{0}};
	void *fmr;
	int rv = 0;

	__NFIN;
	if (state->npages == 0)
		goto out;

	if (state->npages == 1 && state->allow_dma_key)
		map_desc(state, state->base_dma_addr, state->dma_len, key,okey);
	else {
		mri.fmr_pool = P2NV(net->port)->fmr_pool;
		mri.iu = NULL;
		mri.null_iu_idx = -1; /* TBD(Put something more meaningful in here) */
		mri.qp = net->qp;
		mri.use_sg = false;
		mri.pages = state->pages;
		mri.n_pages = state->npages;
		/* we need the offset in the first page and exact dma_len
		   to cover all cases of 512 mappings.
		*/
		mri.offset = state->base_dma_addr & ~nvdev->mr_page_mask;
		mri.dma_len = state->dma_len;
		mri.owner = net;
		if ((fmr = nvmeib_map_mr(nvdev, &mri))) {
			*state->next_fmr++ = fmr;
			state->nmdesc++;
			if (net->lkey == key)
				map_desc(state, mri.io_addr, state->dma_len, mri.lkey,
					mri.rkey);
			else
				map_desc(state, mri.io_addr, state->dma_len, mri.rkey,
					mri.lkey);
			rv = 0;
		}
		else
			rv = -1;
	}

	if (!rv) {
		state->npages = 0;
		state->dma_len = 0;
	}

out:
	__NFOUT;
	return rv;
}

static void map_update_start(struct nvmeibc_map_state *state,
	struct scatterlist *sg, int sg_index, dma_addr_t dma_addr)
{
	NFIN;
	state->unmapped_sg = sg;
	state->unmapped_index = sg_index;
	state->unmapped_addr = dma_addr;
	NFOUT;
}

static int map_sg_entry(struct nvmeibc_ib_net *net,
	struct nvmeibc_map_state *state, struct scatterlist *sg, int sg_index,
	bool use_fmr, u32 key, u32 okey, bool inline_md)
{
	struct nvmeib_dev *nvdev = P2NV(net->port);
	dma_addr_t dma_addr = sg_dma_address(sg);
	unsigned dma_len = sg_dma_len(sg);
	unsigned len;
	int rv;

	__NFIN;
	_ND(trace_ib_net_map_sg_entry, "dma_addr=@DMA_ADDR, dma_len=@DMA_LEN", dma_addr, dma_len);
	if (!dma_len) {
		_ND(trace_1_ib_net_map_sg_entry, "ZERO dma_len - leave");
		rv = 0;
		goto out;
	}

	if (!use_fmr) {
		/* Once we're in direct map mode for a request, we don't
		 * go back to FMR or FR mode, so no need to update anything
		 * other than the descriptor.
		 */
		if (unlikely(inline_md)) {
			len = 1 << NVMEIBC_SECTOR_SHIFT;
			while (dma_len > 0) {
				map_desc(state, dma_addr, len, key, okey);
				dma_addr += len;
				dma_len -= len;
			}
		}
		else
			map_desc(state, dma_addr, dma_len, key, okey);
		_ND(trace_2_ib_net_map_sg_entry, "No FMR - leave");
		rv = 0;
		goto out;
	}

	/*
	 * Since not all RDMA HW drivers support non-zero page offsets for
	 * FMR, if we start at an offset into a page, don't merge into the
	 * current FMR mapping. Finish it out, and use the kernel's MR for
	 * this sg entry.
	 */
	if ((!nvdev->use_fast_reg && (dma_addr & ~nvdev->mr_page_mask)) ||
	    dma_len > nvdev->mr_max_size) {
		_ND(trace_3_ib_net_map_sg_entry, "Must leave - dma_len=@DMA_LEN, "
		   "nvdev->mr_max_size=@MR_MAX_SIZE, dma_addr=@DMA_ADDR, "
		   "nvdev->use_fast_reg=@USE_FAST_REG",
			dma_len, nvdev->mr_max_size, dma_addr,
			nvdev->use_fast_reg ? 'Y' : 'N');
		if ((rv = finish_mapping(net, state, key, okey))) {
			_NT(trace_4_ib_net_map_sg_entry, "Finish mapping failed - @RV", rv);
			goto out;
		}
		WARN_ON(!state->allow_dma_key);

		map_desc(state, dma_addr, dma_len, key, okey);
		map_update_start(state, NULL, 0, 0);
		goto out;
	}

	/* If this is the first sg that will be mapped via FMR or via FR, save
	 * our position. We need to know the first unmapped entry, its index,
	 * and the first unmapped address within that entry to be able to
	 * restart mapping after an error.
	 */
	if (!state->unmapped_sg) {
		_ND(trace_5_ib_net_map_sg_entry, "state->unmapped_sg is true");
		map_update_start(state, sg, sg_index, dma_addr);
	}

	while (dma_len) {
		/* we check the offset and can get into two scenarios:
		   1. this is not the first page so we need to finish the current
		      registration and start a new one
		   2. this is the first page so we just marked it as being the first
		*/
		unsigned offset = dma_addr & ~nvdev->mr_page_mask;
		_ND(trace_6_ib_net_map_sg_entry, "offset=@OFFSET_INT", offset);
		if (state->npages == nvdev->max_pages_per_mr || offset != 0) {
			if ((rv = finish_mapping(net, state, key, okey))) {
				_NT(trace_7_ib_net_map_sg_entry, "Finish mapping failed - @RV", rv);
				goto out;
			}
			map_update_start(state, sg, sg_index, dma_addr);
		}

		len = min_t(unsigned int, dma_len, nvdev->mr_page_size - offset);

		if (!state->npages)
			state->base_dma_addr = dma_addr;
		state->pages[state->npages++] = dma_addr & nvdev->mr_page_mask;
		state->dma_len += len;
		dma_addr += len;
		dma_len -= len;
	}

	/*
	 * Iff the last entry of the MR did not end on a page boundary we
	 * must close it and start a new one.  If the last entry ends on
	 * page boundary it is possible that if the next entry starts on a page
	 * bounday we combine the two and save WQE. And this is real latency
	 * boost.
	 */
	rv = 0;
	if ((dma_addr & ~nvdev->mr_page_mask) != 0)
		if (!(rv = finish_mapping(net, state, key, okey)))
			map_update_start(state, NULL, 0, 0);

out:
	__NFOUT;
	return rv;
}

static int map_sg(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, struct scatterlist *scat, int count,
	u32 key, u32 okey)
{
	struct nvmeib_dev *nvdev = P2NV(net->port);
	struct scatterlist *sg;
	struct nvmeibc_map_state _state, *state;
	int i;
	bool use_mr;
	bool inline_md;

	__NFIN;
	state = &_state;
	memset(state, 0, sizeof(*state));
	state->desc	= req->indirect_desc;
	state->pages = req->map_page;
	inline_md = req->md.has && req->md.has_inline;
	if (inline_md || req->map_sg_mode == MAP_SG_MR_GLOBAL_ONLY)
		use_mr = false;
	else if (nvdev->use_fast_reg) {
		state->next_fr = req->fr_list;
		use_mr = !!P2NV(net->port)->fr_pool;
	} else {
		state->next_fmr = req->fmr_list;
		use_mr = !!P2NV(net->port)->fmr_pool;
	}

	state->allow_dma_key = req->map_sg_mode != MAP_SG_MR_REG_WR_ONLY;
	if (!use_mr && !state->allow_dma_key) {
		_NW(warn_map_sg, "conflicting map constraints, "
						 "allow using global (dma) key");
		WARN_ON(1);
	}

	for_each_sg(scat, sg, count, i) {
		if (map_sg_entry(net, state, sg, i, use_mr, key, okey, inline_md)) {
			/*
			 * Memory registration failed, so backtrack to the
			 * first unmapped entry and continue on without using
			 * memory registration.
			 */
			dma_addr_t dma_addr;
			unsigned int dma_len;

backtrack:
			sg = state->unmapped_sg;
			i = state->unmapped_index;

			dma_addr = sg_dma_address(sg);
			dma_len = sg_dma_len(sg);
			dma_len -= (state->unmapped_addr - dma_addr);
			dma_addr = state->unmapped_addr;
			use_mr = false;
			map_desc(state, dma_addr, dma_len, key, okey);
		}
	}

	/* do not forget to register the last mapping */
	if (use_mr && finish_mapping(net, state, key, okey))
		goto backtrack;

	req->ndesc = state->ndesc;
	req->nmdesc = state->nmdesc;
	req->total_dma_len = state->total_len;
	__NFOUT;
	return 0;
}

static inline void __map_sg_result_trace(struct nvmeibc_volume_request *req)
{
	struct sg_table *sg_table = &req->bcmd->reqs[0].ndb->table;
	struct scatterlist *sg_i;
	struct scatterlist *sg = sg_table->sgl;
	int i, n = sg_table->nents;

	/* input: SG */
	_NT(map_sg_result_trace_0, "orig_nents=@LU, nents=@LU",
		sg_table->orig_nents, sg_table->nents);

	for_each_sg(sg, sg_i, n, i) {
		_NT(map_sg_result_trace_1,
			"sg[@INT] = { .page_link(phy,virt)=@LX(@PTR ,@PTR) .offset=@LLX .length=@LU .dma_addr=@LX .dma_len=@LU }",
			i, sg_i->page_link, sg_page(sg_i), sg_virt(sg_i),
			sg_i->offset, sg_i->length, sg_dma_address(sg_i), sg_dma_len(sg_i));
	}

	/* output: mapping result */
	_NT(map_sg_result_trace_2,
		"req={ndesc=@UINT, nmdesc=@UINT, total_dma_len=@UINT, sgcount=@INT, map_sg_mode=@INT}",
		req->ndesc, req->nmdesc, req->total_dma_len, (int)req->sgcount, req->map_sg_mode);

	if (req->ndesc > 1) {
		struct nvmeib_direct_buf *desc = req->indirect_desc;
		for (i = 0; i < req->ndesc; i++) {
			_NT(map_sg_result_trace_3,
				"desc[@INT] = {va=@LLU, len=@UINT, key=@UINT, okey=@UINT}",
				i, desc->va, desc->len, desc->key, desc->okey);
			desc++;
		}
	}
	else {
		struct nvmeib_direct_buf *desc = &req->table_desc;
		_NT(map_sg_result_trace_4,
			"ndesc=1, va=@LLU, len=@UINT, key=@UINT, okey=@UINT",
			desc->va, desc->len, desc->key, desc->okey);
	}
}

int nvmeibc_ib_net_map_data(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int n_msgs)
{
	struct ib_device *ibdev = P2IB(net->port);
	struct scatterlist *sg;
	/* use for the case that we need debug the RDMA channel */
	struct volume_client_req *cmd = req->cmd->buf;
	int len, nents;
	u32 table_len;
	u32 key;
	u32 okey;
	enum dma_data_direction dir = req->dma_dir;
	__NFIN;

	if (!req->fr_list) {
		_NE(trace_nvmeibc_ib_net_map_data_null_fr_list,
		    "net @NET req @REQ - NULL fr_list\n", net, req);
		BUG();
	}

	/* Clear the indirect fields of the io request */
	req->total_dma_len = 0;
	req->ndesc = 0;

	nents = req->bcmd->reqs[0].ndb->table.nents;
	sg = req->bcmd->reqs[0].ndb->table.sgl;

	if (nvmeibc_map_each_sg_entry) {
		struct scatterlist *sg_i;
		int i, ne, __nents = 0;
		for_each_sg(sg, sg_i, nents, i) {
			/* dir is set to DMA_FROM_DEVICE for Read Ops and DMA_TO_DEVICE for Write ops */
			ne = ib_dma_map_sg(ibdev, sg_i, 1, dir);
			if (ne != 1) {
				_NI_dmesg(error_map_each_s, "net=@PTR, ne=@INT, exp 1", net, ne);
				goto err_out;
			}
			__nents++;
		}
		nents = __nents;
	}
	else {
		/* dir is set to DMA_FROM_DEVICE for Read Ops and DMA_TO_DEVICE for Write ops */
		nents = ib_dma_map_sg(ibdev, sg, nents, dir);
	}

	if (!nents) {
		_NE(error_ib_net_nvmeibc_ib_net_map_data, "DMA mapping failed for sgcount");
		goto err_out;
	}

	req->sgcount = nents;
	req->map_sg_mode = nvmeibc_map_sg_mode;

	/* we need to make sure that the block did generate an SG list
	   with more than NVMEIBS_MAX_IO_CHANNEL_MSGS entires which is enough for
	   128K BIO and 4K sectors
	*/
	if (nents > NVMEIBS_MAX_IO_CHANNEL_MSGS ||
		req->sgcount > NVMEIBS_MAX_IO_CHANNEL_MSGS ||
		unlikely(req->sgcount == 0)) {
		_NE(error_1_ib_net_nvmeibc_ib_net_map_data, "Issue with block device S/G list - @NENTS, @SGCOUNT (@NENTS)",
			nents, req->sgcount, NVMEIBS_MAX_IO_CHANNEL_MSGS);
		goto unmap_sg;
	}
	else
		_ND(trace_ib_net_nvmeibc_ib_net_map_data, "S/G count @SGCOUNT", req->sgcount);

	/* get the correct memory key by the direction of the I/O request */
	_ND(trace_1_ib_net_nvmeibc_ib_net_map_data, "keys: lkey=@LKEY, rkey=@RKEY",
		(unsigned)net->lkey, (unsigned)net->rkey);
	if (dir == DMA_FROM_DEVICE) {
		key = net->rkey; /* read cmd so the controller pushes us */
		okey = net->lkey;
	}
	else {
		key = net->lkey;  /* write cmd so we, the client, pushes */
		okey = net->rkey;
	}
	len = sizeof(struct volume_client_req) + sizeof(struct nvmeib_direct_buf);
	_ND(trace_2_ib_net_nvmeibc_ib_net_map_data, "OTW max len @LEN", len);

	if (req->sgcount == 1 && req->map_sg_mode != MAP_SG_MR_REG_WR_ONLY &&
		!(req->bcmd->reqs->op != NVMEIB_BLOCK_IO_OP_DISCARD && req->md.has && req->md.has_inline)) {
		/*
		 * The block layer only generated a single gather/scatter
		 * entry, or DMA mapping coalesced everything to a
		 * single entry.  So a direct descriptor along with
		 * the DMA MR suffices.
		 */

		/* Set the direct buf fields */
		struct nvmeib_direct_buf *buf = &req->table_desc;
		buf->va  = sg_dma_address(sg);
		buf->key = key;
		buf->len = sg_dma_len(sg);
		buf->okey = okey;
		req->nmdesc = 0;
		goto map_complete;
	}

	/* We have more than one scatter/gather entry, so build our indirect
	 * descriptor table, trying to merge as many entries with FR as we
	 * can.
	 */

	/* Map the block's SG to IB device using as less map-fr(s) as poissible.
	   The mapping description is filled in @req->indirect_desc and will be
	   used to build the rdma-write WRs of the request, on Write op, or of
	   the response, on Read op */
	map_sg(net, req, sg, req->sgcount, key, okey);

	if (unlikely(n_msgs && (n_msgs < req->ndesc))) {
		_NE(error_2_ib_net_nvmeibc_ib_net_map_data, "Could not fit block dev S/G list into NVMEIB_CMD: "
		   "n_msgs=@N_MSGS, count=@COUNT", n_msgs, req->ndesc);
		goto unmap_data;
	}

	/* We've mapped the request, now pull as much of the indirect
	 * descriptor table as we can into the command buffer.
	 */
	if (req->ndesc == 1) {
		/* FR mapping was able to collapse this to one entry,
		 * so use a direct descriptor.  this is like count == 1
		 */

		/* Set the direct buf fields */
		struct nvmeib_direct_buf *buf = &req->table_desc;
		buf[0] = req->indirect_desc[0];
		_ND(trace_3_ib_net_nvmeibc_ib_net_map_data, "addr=@ADDR, len=@LEN, key=@KEY_INT, okey=@OKEY",
			buf->va, buf->len, buf->key, buf->okey);
		goto map_complete;
	}

	/* save the number of memory descriptors */
	/* the length of the array of the memory descriptors  */
	table_len = req->ndesc * sizeof(struct nvmeib_direct_buf);

	/* the registration type: direct means we have one inline descriptor
	   and indirect means that we have a table that describes the descriptors.
	   these are only used for debug purposes when instead of direct RDMA
	   we send the memory descriptor(s) as a message.
	*/
	len = sizeof(*cmd) + sizeof(struct nvmeib_indirect_buf) + table_len;

	req->table_desc.va = req->indirect_dma_addr;
	req->table_desc.key = key;
	req->table_desc.len = table_len;

	goto map_complete;

unmap_data:
	nvmeibc_ib_net_unmap_data(net, req);

unmap_sg:
	nvmeibc_ib_net_complete_iocmd_sg(net, req);

err_out:
	len = -EIO;

map_complete:
#ifdef DEBUG_TRANSFERS_CHECK_NDB_MAPPED
#ifndef DEBUG_TRANSFERS
#	error "DEBUG_TRANSFERS must also be defined"
#else
#	error "Currently not working due to LOC_PROT issue"
#endif
	if (len >= 0 && req->nmdesc > 0) {
		BUG_ON(len == 0);
		req->bcmd->reqs->ndb_mapped = true;
	}
#endif

	if (unlikely(nvmeibc_map_sg_result_trace)) {
		if (nvmeibc_map_sg_result_trace == 1) /* oneshot mode */
			nvmeibc_map_sg_result_trace = 0;
		__map_sg_result_trace(req);
	}

	__NFOUT;
	return len;
}

/**
 * nvmeibc_unmap_sg_to_ib_sge() - Unmap an IB SGE list.
 */
void nvmeibc_ib_net_unmap_sg_to_ib_sge(struct nvmeib_iu *iu)
{
	NFIN;

	/* if logic is added here other than just resseting n_rdma_iu,
	   do so under if (n_rdma_iu != 0) */
	iu->n_rdma_iu = 0;
	NFOUT;
}

int nvmeibc_ib_net_read_map_sg_to_ib_sge(struct nvmeib_iu *iu,
	struct nvmeib_direct_buf *bufs, int nbufs, u64 read_from_addr, u32 lkey,
	int n_md_descs, int md_entry_size)
{
	int i;
	struct nvmeib_rdma_iu *riu;
	struct nvmeib_direct_buf *pbuf;
	u64 laddr = read_from_addr;
	int rv = 0;

	NFIN;
	/* No SG on the read side since we have multiple remote address */
	iu->n_rdma_iu = nbufs;
	if (iu->n_rdma_iu + n_md_descs > iu->max_rdma_iu) {
		_NE(error_ib_net_nvmeibc_ib_net_read_map_sg_to_ib_sge, "Read OP SG is too big needs @N_RDMA_IU (md @N_MD_DESCS), max=@MAX",
			iu->n_rdma_iu, n_md_descs, iu->max_rdma_iu);
		iu->n_rdma_iu = 0;
		rv = -1;
		goto free_mem;
	}

	pbuf = bufs;
	riu = iu->rius;
	for (i = 0; i < iu->n_rdma_iu; ++i, ++pbuf, ++riu) {
		riu->raddr = pbuf->va;
		riu->rkey = pbuf->key;
		riu->lkey = pbuf->okey;
		riu->sge_cnt = 1;
		riu->sge[0].addr = laddr;
		riu->sge[0].length = pbuf->len;
		riu->sge[0].lkey = lkey;
		_ND(trace_ib_net_nvmeibc_ib_net_read_map_sg_to_ib_sge, "raddr=@RADDR, rkey=@RKEY", riu->raddr, riu->rkey);
		_ND(trace_1_ib_net_nvmeibc_ib_net_read_map_sg_to_ib_sge, "@INDEX: addr=@ADDR, len=@LEN, key=@KEY_INT", i,
			riu->sge[0].addr, riu->sge[0].length, riu->sge[0].lkey);
		laddr += riu->sge[0].length + md_entry_size;
	}
	iu->n_rdma_iu += n_md_descs;
	goto out;

free_mem:
	nvmeibc_ib_net_unmap_sg_to_ib_sge(iu);

out:
	NFOUT;
	return rv;
}

/* Fill @iu->rius[] with:
 *	1. increasing raddr starting with req's BB remote-address
 * 	2. rkey
 * 	3. sg-list and sge_cnt (using the result of the map-data)
 * This is used when building RDMA-WRITE WQEs
 */
int nvmeibc_ib_net_write_map_sg_to_ib_sge(struct nvmeib_iu *iu,
	struct nvmeib_direct_buf *bufs, int nbufs, u64 write_to_addr, u32 rkey)
{
	int i, j;
	struct nvmeib_rdma_iu *riu;
	struct nvmeib_direct_buf *pbuf;
	u64 raddr = write_to_addr;
	u32 size;
	int len;
	int rv = 0;

	NFIN;
	_ND(trace_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge, "raddr=@RADDR, rkey=@RKEY, nbufs=@NBUFS", write_to_addr, rkey, nbufs);
	iu->n_rdma_iu = (nbufs + NVMEIB_DEF_SG_PER_WQE - 1) / NVMEIB_DEF_SG_PER_WQE;
	if (iu->n_rdma_iu > iu->max_rdma_iu) {
		_NE(error_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge, "Needed riu @N_RDMA_IU exceeds max riu @MAX_RDMA_IU",
			iu->n_rdma_iu, iu->max_rdma_iu);
		rv = -ENOMEM;
		goto free_mem;
	}
	else
		_ND(trace_1_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge, "n_rdma_iu=@N_RDMA_IU", (int)iu->n_rdma_iu);

	pbuf = bufs;
	riu = iu->rius;
	len = nbufs;
	for (i = 0; i < iu->n_rdma_iu; ++i, ++riu) {
		size = 0;
		riu->raddr = raddr;
		riu->rkey = rkey;
		riu->sge_cnt = 0;
		while (riu->sge_cnt < NVMEIB_DEF_SG_PER_WQE && len > 0) {
			size += pbuf->len;
			++riu->sge_cnt;
			--len;
			++pbuf;
		}
		raddr += size;
	}
	pbuf = bufs;
	riu = iu->rius;
	len = nbufs;
	for (i = 0; i < iu->n_rdma_iu; ++i, ++riu) {
		_ND(trace_2_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge, "raddr=@RADDR, rkey=@RKEY", riu->raddr, riu->rkey);
		for (j = 0; j < riu->sge_cnt; ++j, ++pbuf) {
			riu->sge[j].addr = pbuf->va;
			riu->sge[j].length = pbuf->len;
			riu->sge[j].lkey = pbuf->key;
			_ND(trace_3_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge, "@INDEX: addr=@ADDR, len=@LEN, key=@KEY_INT", j,
				riu->sge[j].addr, riu->sge[j].length, riu->sge[j].lkey);
		}
	}
	goto out;

free_mem:
	nvmeibc_ib_net_unmap_sg_to_ib_sge(iu);

out:
	NFOUT;
	return rv;
}

int nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md(struct nvmeib_iu *iu,
	struct nvmeib_direct_buf *bufs, int nbufs, u64 write_to_addr, u32 rkey,
	struct nvmeibc_volume_request *req, int io_len,
	unsigned sw_sc_shift, unsigned hw_sc_shift)
{
	int i, ii;
	struct nvmeib_rdma_iu *riu;
	struct nvmeib_direct_buf *pbuf;
	u64 raddr = write_to_addr;
	unsigned sg_per_wq;
	int len;
	u64 mda = req->md.local.addr;
	int sw_sector_size = 1 << sw_sc_shift;
	int sw_md_size = req->md.entry_size << (sw_sc_shift - hw_sc_shift);
	u64 v;
	u64 e;
	int rv = 0;

	NFIN;
	_ND(trace_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "raddr=@RADDR, rkey=@RKEY, nbufs=@NBUFS, io_len=@IO_LEN",
		write_to_addr, rkey, nbufs, io_len);
	_ND(trace_1_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "@IO_LEN SW sectors in io request", io_len >> sw_sc_shift);
	if (!(sg_per_wq = (NVMEIB_DEF_SG_PER_WQE >> 1) << 1)) {
		_NE(error_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "NVMEIB_DEF_SG_PER_WQE @NVMEIB_DEF_SG_PER_WQE is less than 2", NVMEIB_DEF_SG_PER_WQE);
		rv = -ENOMEM;
		goto out;
	}
	iu->n_rdma_iu =
		(((io_len >> sw_sc_shift) << 1) + sg_per_wq - 1) / sg_per_wq;
	if (iu->n_rdma_iu > iu->max_rdma_iu) {
		_NE(error_1_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "Needed riu @N_RDMA_IU exceeds max riu @MAX_RDMA_IU",
			iu->n_rdma_iu, iu->max_rdma_iu);
		rv = -ENOMEM;
		goto out;
	}
	else
		_ND(trace_2_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "n_rdma_iu=@N_RDMA_IU", (int)iu->n_rdma_iu);

	pbuf = bufs;
	riu = iu->rius;
	len = io_len;
	for (i = 0; i < iu->n_rdma_iu; ++i, ++riu) {
		riu->raddr = raddr;
		riu->rkey = rkey;
		riu->sge_cnt = 0;
		while (riu->sge_cnt < sg_per_wq && len > 0) {
			riu->sge_cnt += 2;
			len -= sw_sector_size;
			raddr += sw_sector_size + sw_md_size;
		}
	}
	pbuf = bufs;
	riu = iu->rius;
	len = nbufs;
	v = pbuf->va;
	e = v + pbuf->len;
	for (i = 0; i < iu->n_rdma_iu && len; ++i, ++riu) {
		_ND(trace_3_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "raddr=@RADDR, rkey=@RKEY", riu->raddr, riu->rkey);
		ii = 0;
back:
		for ( ; ii < riu->sge_cnt && v < e;
			  ii += 2, v += sw_sector_size, mda += sw_md_size) {
			riu->sge[ii].addr = v;
			riu->sge[ii].length = sw_sector_size;
			riu->sge[ii].lkey = pbuf->key;
			riu->sge[ii + 1].addr = mda;
			riu->sge[ii + 1].length = sw_md_size;
			riu->sge[ii + 1].lkey = req->md.local.lkey;
			_ND(trace_4_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "@II: addr=@ADDR, len=@LEN, key=@KEY_INT", ii,
				riu->sge[ii].addr, riu->sge[ii].length, riu->sge[ii].lkey);
			_ND(trace_5_ib_net_nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md, "@INDEX: addr=@ADDR, len=@LEN, key=@KEY_INT, md=@MD_PTR", ii + 1,
				riu->sge[ii + 1].addr, riu->sge[ii + 1].length,
				riu->sge[ii + 1].lkey, (req->md.data + (mda - req->md.local.addr)));
		}
		if (v >= e && --len) {
			++pbuf;
			v = pbuf->va;
			e = v + pbuf->len;
		}
		if (len && ii < riu->sge_cnt)
			goto back;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibc_ib_net_map_md(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int io_len,
	unsigned sw_sc_shift, unsigned hw_sc_shift)
{
    struct ib_device *ibdev = P2IB(net->port);
	int n_sw_sectors __attribute__((unused));
	int n_hw_sectors;
	int md_size;
	int n_pages;
	int m_pages;
    int rv;

	__NFIN;
	n_sw_sectors = io_len >> sw_sc_shift;
	n_hw_sectors = io_len >> hw_sc_shift;
	md_size = n_hw_sectors * req->md.entry_size;
	n_pages = DIV_ROUND_UP(io_len, net->admin_ch->cntr_page_size);
	m_pages = DIV_ROUND_UP(md_size, net->admin_ch->cntr_page_size);
	if (req->md.has_inline) {
		if (n_pages + m_pages > req->bb_pages) {
			_NE(error_ib_net_nvmeibc_ib_net_map_md, "Data + MD pages (@N_PAGES) > BB (@BB_PAGES) pages",
				n_pages + m_pages, req->bb_pages);
			rv = -1;
			goto out;
		}
	}
	else if (m_pages > (req->md.remote.size / net->admin_ch->cntr_page_size)) {
		_NE(error_1_ib_net_nvmeibc_ib_net_map_md, "MD pages (@M_PAGES) > MD BB pages (@M_PAGES) size=@SIZE",
			m_pages, (req->md.remote.size / net->admin_ch->cntr_page_size),
			req->md.remote.size);
		rv = -1;
		goto out;
	}
	BUG_ON(req->md.is_dummy && md_size > PAGE_SIZE);
	if (!req->md.is_dummy) {
		/* JH IOMMU: req->dma_dir is set to DMA_FROM_DEVICE for Read Ops, DMA_TO_DEVICE for Write Ops */
		req->md.local.addr = ib_dma_map_single(ibdev, req->md.data,
			md_size, req->dma_dir);
		if (ib_dma_mapping_error(ibdev, req->md.local.addr)) {
			_NE(error_2_ib_net_nvmeibc_ib_net_map_md, "fail to dma_map MD buffer");
			rv = -1;
			goto out;
		}
	}
	_NDn(trace_3_ib_net_nvmeibc_ib_net_map_md, net, "req MD mapped to addr @DMA_ADDR (size @SIZE) for ibdev @IB_DEVICE",
	    req->md.local.addr, md_size, ibdev->name);
	req->md.local.size = md_size;
	rv = 0;

out:
    __NFOUT;
    return rv;
}

void nvmeibc_ib_net_unmap_md(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req)
{
    struct ib_device *ibdev = P2IB(net->port);

	__NFIN;
	if (req->md.has && req->md.local.size) {
		if (!req->md.is_dummy) {
			/* JH IOMMU: req->dma_dir is set to DMA_FROM_DEVICE for Read Ops, DMA_TO_DEVICE for Write Ops */
			ib_dma_unmap_single(
				ibdev, req->md.local.addr, req->md.local.size, req->dma_dir);
		}
		req->md.local.addr = 0;
		req->md.local.size = 0;
	}
    __NFOUT;
}

int nvmeibc_ib_net_build_wriu(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, struct nvmeib_iu *iu,
	enum nvmeib_block_io_op op, int *io_len,
	unsigned sw_sc_shift, unsigned hw_sc_shift,
	u64 raddr, u32 rkey)
{
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct nvmeib_direct_buf *bufs;
	int nbufs;
	int rv = 0;

	NFIN;
	if (nvmeib_block_io_op_is_write(op) || op == NVMEIB_BLOCK_IO_OP_DISCARD) {
		nvmeibc_ib_net_get_bufs_nbufs(req, &bufs, &nbufs, io_len);
		if (likely(nvmeib_block_io_op_is_write(op))) {
			if (req->md.has) {
				if ((rv = nvmeibc_ib_net_map_md(net, req, *io_len, sw_sc_shift,
					hw_sc_shift)))
					goto out;
				if (req->md.has_inline)
					rv = nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md(
						iu, bufs, nbufs, raddr, rkey, req, *io_len,
						sw_sc_shift, hw_sc_shift);
				else
					goto no_md;
			}
			else
				goto no_md;
		}
		else {
no_md:
			rv = nvmeibc_ib_net_write_map_sg_to_ib_sge(
				iu, bufs, nbufs, raddr, rkey);
		}
	}
	else
		*io_len = block_cmd->reqs[0].ndb->length;

out:
	if (rv)
		nvmeibc_ib_net_unmap_md(net, req);
	NFOUT;
	return rv;
}

#if defined(NVMEIBC_READ_POISON_BB) && (NVMEIBC_READ_POISON_BB==1)

/* In order to test this:
 * (1) Force RDMA write-back of 1st BB-page only e.g. for RDDA,
 *     pass @len=1 to nvmeibc_fill_wsq()
 * (2) Fill poison in 2nd BB page
 * (3) Run read of 8K
 */

static void __poison_area_fill(struct nvmeibc_ib_net *net)
{
	const u64 pattern = NVMEIBC_POISON_AREA_PATTERN_U64;
	u64 *u64_p = net->pois_src.vaddr;
	int n = PAGE_SIZE / sizeof(*u64_p);
	int i;

	BUILD_BUG_ON(sizeof(*u64_p) != sizeof(pattern));
	for (i = 0; i < n; i++)
		memcpy(&u64_p[i], &pattern, sizeof(*u64_p));

	BUG_ON(net->pois_src.hdr != net->pois_src.vaddr);
	nvmeib_public_uuid_gen(&net->pois_src.hdr->uniq);
	net->pois_src.hdr->tsc = 0xdeadbeef;

	if (net->pois_src.n_pages > 1)
		_NE(t_b1_dp_dbg_tools, "OOPS, not filling *all* poison area...");

	_NI(t_b2_dp_dbg_tools, "poison: net=@NET, uuid=@CLIENT_UUID",
		net, &net->pois_src.hdr->uniq);
}

static int __poison_area_alloc(struct nvmeibc_ib_net *net)
{
	int rv;

	if (!(net->pois_src.vaddr= (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO))) {
		_NT(error_0_poison_area_alloc, "Failed to alloc poison area");
		net->pois_src.n_pages = 0;
		rv = -ENOMEM;
	}
	else {
		net->pois_src.n_pages = 1;
		net->pois_src.hdr = net->pois_src.vaddr;
		__poison_area_fill(net);
		rv = 0;
	}

	return rv;
}

static void __poison_area_free(struct nvmeibc_ib_net *net)
{
	if (net->pois_src.n_pages) {
		free_page((unsigned long)net->pois_src.vaddr);
		net->pois_src.n_pages = 0;
		net->pois_src.vaddr = NULL;
		net->pois_src.hdr = NULL;
	}
}

int nvmeibc_ib_net_poison_map(struct nvmeibc_ib_net *net)
{
    struct ib_device *ibdev = P2IB(net->port);
	void *vaddr;
	size_t len = NVMEIBC_READ_POISON_BB_BYTES;
	u64 laddr;
	int rv;

	WARN_ON(net->pois_src.vaddr);
	if ((rv = __poison_area_alloc(net))) {
		/* Error print inside */
		goto out;
	}

	vaddr = net->pois_src.vaddr;
	BUG_ON(!vaddr);

	/* JH IOMMU: DMA_TO_DEVICE is correct, only used for source for Local RDMA_WRITE */
	laddr = ib_dma_map_single(ibdev, vaddr, len, DMA_TO_DEVICE);
	if (ib_dma_mapping_error(ibdev, laddr)) {
		_NE(t_b3_dp_dbg_tools, "Fail map poison buffer");
		__poison_area_free(net);
		rv = -1;
	}
	else {
		net->pois_src.sge.length = len;
		net->pois_src.sge.addr = laddr;
		net->pois_src.sge.lkey = net->lkey;

		_NI(t_b4_dp_dbg_tools,
			"mapped poison: vaddr=@PTR, len=@SIZE_T, laddr=@LLX, lkey=@X",
			vaddr, len, laddr, net->lkey);
		rv = 0;
	}

out:

	return rv;
}

void nvmeibc_ib_net_poison_unmap(struct nvmeibc_ib_net *net)
{
	if (net->pois_src.vaddr) {
		/* JH IOMMU: DMA_TO_DEVICE is correct, only used for source for Local RDMA_WRITE */
		ib_dma_unmap_single(P2IB(net->port),
							net->pois_src.sge.addr,
							net->pois_src.sge.length,
							DMA_TO_DEVICE);
		__poison_area_free(net);
	}
}

/* Assume @raddr points to 1st BB page */
void nvmeibc_ib_net_poison_fill_wr(struct nvmeib_send_wr *wr, u64 id,
								   struct nvmeibc_ib_net *net,
								   struct nvmeibc_volume_request *req,
								   u64 raddr, u32 rkey)
{
	BUG_ON(req->bcmd->reqs[0].op != NVMEIB_BLOCK_IO_OP_READ);
	BUG_ON(!req->rd_pois);

	BUG_ON(!net->pois_src.hdr);

	net->pois_src.hdr->tsc = nvmeib_public_rdtsc();

	_NDn(trace_nvmeibc_ib_net_poison_fill_wr, net,
		"poison: addr=@LLX, len=@UINT, lkey=@X, pattern={addr=@PTR, val=@LLX}, tsc=@LLX",
		net->pois_src.sge.addr, net->pois_src.sge.length,
		net->pois_src.sge.lkey, net->pois_src.vaddr, *((u64 *)net->pois_src.vaddr),
		net->pois_src.hdr->tsc);

	nvmeib_send_wr_common(*wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(*wr).wr_id = id;
	nvmeib_send_wr_rdma(*wr).remote_addr = raddr;
	nvmeib_send_wr_rdma(*wr).rkey = rkey;
	nvmeib_send_wr_common(*wr).num_sge = 1;
	nvmeib_send_wr_common(*wr).sg_list = &net->pois_src.sge;
}

void nvmeibc_ib_net_poison_verify(struct nvmeibc_ib_net *net,
								  struct nvmeibc_volume_request *req,
								  int *comp_code)
{
	/* update for any io (duplicates 'net->ioch->dbg_di.dbg_of_dbg.last') */
	net->pois_prev_io.opcode = req->bcmd->reqs[0].op;
	net->pois_prev_io.dlba = req->bcmd->reqs[0].disk_address;
	net->pois_prev_io.len = req->bcmd->reqs[0].ndb->length;

#if defined(NVMEIBC_READ_POISON_BB_PANIC) && (NVMEIBC_READ_POISON_BB_PANIC==1)
#define _NEpv _NE_dmesg						// dp_dbg_tools - For debug only
#else
#define _NEpv _NE
#endif

	if (req->rd_pois &&
		req->bcmd->reqs[0].op == NVMEIB_BLOCK_IO_OP_READ && *comp_code == 0) {

		struct sg_table *sg_tbl = &req->bcmd->reqs[0].ndb->table;
		int n = sg_tbl->nents;
		struct scatterlist *sgl = sg_tbl->sgl;
		struct scatterlist *sg_i;
		int i, j, pg_cnt = 0;
		int tot_off = 0;
		int pois_len = min(net->pois_src.sge.length,
						   req->bcmd->reqs[0].ndb->length);
		void *pois_p = net->pois_src.vaddr;
		int n_err = 0;

		/* Assumes poison starts at 1st BB page for @net->pois_src.sge.length bytes */
		for_each_sg(sgl, sg_i, n, i) {
			const int length = sg_i->length;
			void *buff_base = sg_virt(sg_i);
			void *buff_p = buff_base;
			for (j = 0; j < length; j += NVMEIBC_SECTOR_SIZE) {
				const int cmp_len = (tot_off + NVMEIBC_SECTOR_SIZE < pois_len) ?
					NVMEIBC_SECTOR_SIZE : pois_len - tot_off;
				BUG_ON(cmp_len <= 0);
				if (!memcmp(buff_p, pois_p, cmp_len)) {
					_NEpv(e0_net_poison_verify,
						"Found poison data in S/G at offset @INT/@INT (pg/byte) len=@INT",
						tot_off, pg_cnt, cmp_len);
					n_err++;
				}

				tot_off += cmp_len;
				if (tot_off == pois_len)
					goto done;

				buff_p += NVMEIBC_SECTOR_SIZE;
				pois_p += NVMEIBC_SECTOR_SIZE;
				pg_cnt++;
			}
		}
done:
		if (n_err) {
			struct sg_page_iter sg_iter;
			int i = 0;

			_NEpv(e1_net_poison_verify,
				"Found total of @INT poison pages of "
				"total compared @INT/@INT pages/bytes "
				"net=@NET, @IOCH_NAME, req=@REQ, comp_code=@COMP_CODE, "
				"prev-io={op=@INT, dlba=@LLU, len=@LLU",
				  n_err, pg_cnt, tot_off, net, net->ioch->name, req, *comp_code,
				  net->pois_prev_io.opcode,
				  net->pois_prev_io.dlba,
				  net->pois_prev_io.len);

			/* on_poison_io_ch */
			if (net->on_poison)
				net->on_poison(net);

			for_each_sg_page(sgl, &sg_iter, n, 0) {
				void *p = page_address(sg_page_iter_page(&sg_iter));
				_NEpv(e2_net_poison_verify, "Page @INT", i);
#if defined(NVMEIBC_READ_POISON_BB_PANIC) && (NVMEIBC_READ_POISON_BB_PANIC==1)
				print_hex_dump(KERN_ERR, "S/G Page: ",
							   DUMP_PREFIX_OFFSET, 16, 1, p,
							   PAGE_SIZE, 1);
#else
				_NEpv(e3_net_poison_verify, "@HEX64", p);
#endif
				i++;
			}

#if defined(NVMEIBC_READ_POISON_BB_PANIC) && (NVMEIBC_READ_POISON_BB_PANIC==1)
			nvmeibc_ib_admin_channel_kill_remote_and_die(container_of(
			    net->admin_ch, struct nvmeibc_ib_admin_channel, base));
			while (1);
#else
			nvmeibc_disk_counters_inc(net->ioch->disk, n_err_rdda_read_poison);
			_NEpv(e4_net_poison_verify,
				"Found poisoned data (@NET) revoke comp-code", net);
			WARN_ON(1); //EC-6112
			*comp_code = -EIO;
#endif
		}
	}

#undef _NEpv

}
#endif /* NVMEIBC_READ_POISON_BB */

void nvmeibc_ib_net_complete_iocmd_sg_reuse(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int orig_sgcount, bool was_reuse)
{
	struct nvmeibc_block_io_req *bcmd = &req->bcmd->reqs[0];

	__NFIN;
	if (orig_sgcount && bcmd->ndb->table.sgl) {
		if (req->sgcount) {
			/* JH IOMMU: req->dma_dir is set to DMA_FROM_DEVICE for Read Ops, DMA_TO_DEVICE for Write Ops */
			ib_dma_unmap_sg(P2IB(net->port), bcmd->ndb->table.sgl,
				bcmd->ndb->table.nents, req->dma_dir);
			req->sgcount = 0;
		}
	}
	else if (bcmd->op == NVMEIB_BLOCK_IO_OP_MD_READ ||
		bcmd->op == NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR) 
	{
		/* ops which SG's (if any), was not ib-dma-mapped */
	}
	else if (!(nvmeib_block_io_op_is_write(bcmd->op) && was_reuse)) 
	{
		_NE(error_ib_net_nvmeibc_ib_net_complete_iocmd_sg,
			"Invalid I/O command: NULL S/G (req=@REQ, op=@BLOCK_IO_OP, reused_bb=@BOOL_YN, was_reuse=@BOOL_YN)", 
			req, bcmd->op, req->reused_bb, was_reuse);
	}

	/* if we have mapped MD unmapped it here */
	nvmeibc_ib_net_unmap_md(net, req);

	/* Not calling nvmeibc_ib_net_unmap_data() here due to LOC_PROT issue  */

	__NFOUT;
}

int nvmeibc_ib_io_channel_get_rsc_id_from_opaque_pointer(
    struct nvmeibc_channel *ch);

#ifdef DBGDI_REMOVED_IN_PRODUCTION
	#define dp_dbgdi_add_info_core_post_with_magic(...)
#else
static void
dp_dbgdi_add_info_core_post_with_magic(struct nvmeibc_ib_net *net,
                                       struct nvmeibc_volume_request *req,
                                       int comp_code) {
	int rv;
	if (dp_dbgdi_should_add_info_core(req->bcmd)) {
		struct t_core_dbgdi_params_post param = NVMEIBC_CORE_DBGDI_PARAM(
		    post, net->admin_ch->base.disk->name, net->ioch->ct,
		    .comp_code  = comp_code,
		    .start_dlba = req->bcmd->reqs[0].disk_address);

		/* Debugs the debug DI. TODO: maybe ifdef? For now, overhead is very
		   low. Recommend to leave as is. */
		net->ioch->dbg_di.dbg_of_dbg.last = net->ioch->dbg_di.dbg_of_dbg.cur;
		net->ioch->dbg_di.dbg_of_dbg.cur.ch_type = net->ioch->ct;
		net->ioch->dbg_di.dbg_of_dbg.cur.opcode  = req->bcmd->reqs[0].op;
		net->ioch->dbg_di.dbg_of_dbg.cur.jif     = jiffies;
		net->ioch->dbg_di.dbg_of_dbg.cur.dlba = req->bcmd->reqs[0].disk_address;
		net->ioch->dbg_di.dbg_of_dbg.cur.len =
		    (req->bcmd->reqs[0].ndb->length >> NVMEIBC_SECTOR_SHIFT);

		if (net->ioch->ct == ct_rdda ||
		    (net->ioch->ct == ct_n_rdda &&
		     net->ioch->dbg_di.dbg_of_dbg.cur.opcode ==
		         NVMEIB_BLOCK_IO_OP_WRITE)) {

			param.was_overeager = net->ioch->dbg_di.was_overeager;
			param.magic_data    = &net->ioch->dbg_di.magic_data;
		}

		if (((rv = dp_dbgdi_do_add_info_core_post(&req->bcmd->reqs[0],
		                                          &param)))) {
			if (!nvmeibc_panic_on_core_dbgdi) {
				_NEn(e_aA_dp_dbg_tools, net,
					 "Core DBGDI poison triggered - killing the server and "
					 "myself (param=@PTR, rv=@RV), req=@PTR",
					 &param, rv, req);
				nvmeibc_disk_counters_inc(net->ioch->disk, n_err_core_dbgdi_detection);
			}
			else {
				_NEn_dmesg(t_aA_dp_dbg_tools, net,
						   "Core DBGDI poison triggered - killing the server and "
						   "myself (param=@PTR, rv=@RV), req=@PTR",
						   &param, rv, req);
				if (rv == 4) {
					_NE_dmesg(t01_dp_dbg_tools, "NOTE: RV = 4, hence this could be a false alarm if you have created the volume manually and not via INFRA. This is the infamous hidden attach problem. In that case shame on you. If not - it could be a real issue");
				}
				if (net->ioch->ct == ct_rdda) {
					nvmeibc_ib_admin_channel_kill_remote_and_die(&(
						struct nvmeibc_please_kill_yourself_args){
						.ch = container_of(net->admin_ch,
										   struct nvmeibc_ib_admin_channel, base),
						.has_death_wish = true,
						.rsc_id =
							nvmeibc_ib_io_channel_get_rsc_id_from_opaque_pointer(
								net->ioch),
						.dlba = req->bcmd->reqs[0].disk_address});
				} else {
					nvmeibc_ib_admin_channel_kill_remote_and_die(&(
						struct nvmeibc_please_kill_yourself_args){
						.ch = container_of(net->admin_ch,
										   struct nvmeibc_ib_admin_channel, base),
						.has_death_wish = false});
				}
				while (1)
					;
			}
		}

		/* Statistics */
		nvmeibc_disk_cmds_stats_record_poison_check_hit(
		    net->admin_ch->base.disk, &req->bcmd->disk_cmd, param.stats.hits);
		nvmeibc_disk_cmds_stats_record_posion_check_miss(
		    net->admin_ch->base.disk, &req->bcmd->disk_cmd, param.stats.misses);
	}
}
#endif // DBGDI_REMOVED_IN_PRODUCTION

//[IOCH-DRAINED TBD]: merge to new funcs below, still used by rdda-oe-finish-write
void nvmeibc_ib_net_complete_iocmd_block(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, enum stats_done_info_type done_type, int comp_code)
{
	struct nvmeibc_d_iocmd_comp *comp = &req->bcmd->comp;

	__NFIN;
	_ND(trace_ib_net_nvmeibc_ib_net_complete_iocmd_block, "Complete block_command=@BLOCK_COMMAND id=@REQ_ID_LLONG",
		req->bcmd, req->bcmd ? req->bcmd->req_id : 0);
	if (comp_code < 0)
		_NT(trace_1_ib_net_nvmeibc_ib_net_complete_iocmd_block, "nvmeibc_ib_net_complete_iocmd_block with code @COMP_CODE", comp_code);
	nvmeibc_block_cmd_status_debug(req->bcmd, NVMEIBC_BLOCK_CMD_COMPLETED);
	nvmeibc_disk_cmd_status_debug(req->dcmd,
			NVMEIBC_DISK_CMD_REMOTE_COMPLETED);

	if (atomic_read(&net->dying)) {
		_NE(trace_2_ib_net_nvmeibc_ib_net_complete_iocmd_block,
			"completion while net @NET is dying, cmd may still be wip @ srv-side", net);
		WARN_ON(1); //EC-5113
	}

	if (req->bcmd->disk_cmd.complete_w_error)
		comp_code = -EIO;
	nvmeibc_disk_cmds_stats_llp_complete(net->admin_ch->base.disk, &req->bcmd->disk_cmd, done_type, comp_code);

	nvmeibc_ib_net_poison_verify(net, req, &comp_code);
	dp_dbgdi_add_info_core_post_with_magic(net, req, comp_code);

	req->bcmd = NULL; /* unlink req from iocmd */
	comp->comp_code = comp_code;
	/* update the block layer */
	nvmeibc_block_completion(comp); //RDDA-OE only (originaly from any IO channel NO/RDDA completed IO)
	__NFOUT;
}

void nvmeibc_ib_net_unmap_and_unlink_iocmd_reuse(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int comp_code, int orig_sgcount, bool was_reuse)
{
	struct nvmeibc_channel *ioch = net->ioch;
	struct nvmeibc_disk_io_command *bcmd;

	nvmeibc_channel_iocmd_cnt(ioch,req->bcmd->reqs[0].op, comp_code);

	/* First unmap SG and md-buffer and only then call
	   block's completion handler so that if block reruse
	   these addreeses for a new io-req, we won't endup
	   unmapping what we've just mapped for the new io-req */
	nvmeibc_ib_net_complete_iocmd_sg_reuse(net, req, orig_sgcount, was_reuse);

	nvmeibc_ib_net_poison_verify(net, req, &comp_code);
	dp_dbgdi_add_info_core_post_with_magic(net, req, comp_code);

	bcmd = req->bcmd;
	req->bcmd = NULL; /* unlink req from iocmd */

	if (bcmd->disk_cmd.complete_w_error) {
		_NTn(trace_0_nvmeibc_ib_net_unmap_and_unlink_iocmd,
			 net, "inject err on comp\n");
		comp_code = -EIO;
	}
	bcmd->comp.comp_code = comp_code;
}

void nvmeibc_ib_net_complete_bcmd(struct nvmeibc_disk_command *dcmd, enum stats_done_info_type done_type, struct nvmeibc_dev *local_dev)
{
	struct nvmeibc_disk_io_command *bcmd = disk_to_block(dcmd);
	//logic we've skipped @ nvmeibc_ib_net_complete_iocmd
	if (bcmd->comp.comp_code == 0) {
		ktime_t end_ts = nvmeib_public_ktime_get();
		if (ktime_after(end_ts, bcmd->disk_cmd.start_ts)) {
			u64 latency = ktime_to_ns(ktime_sub(end_ts, bcmd->disk_cmd.start_ts));
		
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
		nvmeibc_disk_add_stats(bcmd->disk, local_dev, bcmd->v_disk_stats, bcmd->reqs, latency,
				       nvmeibc_disk_io_cmd_originator_is_recov(bcmd->orig));
#else
		nvmeibc_disk_add_stats(bcmd->disk, local_dev, NULL, bcmd->reqs, latency,
				       nvmeibc_disk_io_cmd_originator_is_recov(bcmd->orig));
#endif
		} else {
			_NTHROTTLED_T(trace_ib_net_nvmeibc_ib_net_complete_iocmd,
						NVMEIBT_THROTTLE_INTERVAL, 1,
						"negative latency value");
		}
	}

	//logic we've skipped @ nvmeibc_ib_net_complete_iocmd_block
	if (bcmd->comp.comp_code < 0)
		_NT(trace_1_complete_bcmd,
			"complete bcdm with comp-code=@COMP_CODE", bcmd->comp.comp_code);
	nvmeibc_block_cmd_status_debug(bcmd, NVMEIBC_BLOCK_CMD_COMPLETED);
	nvmeibc_disk_cmds_stats_llp_complete(bcmd->disk, &bcmd->disk_cmd, done_type, bcmd->comp.comp_code);
	nvmeibc_block_completion(&bcmd->comp); //Any IO channel NO/RDDA completed IO
}

static void nvmeibc_ib_net_complete_bcmd_work(struct workqe_struct *work)
{
	struct nvmeibc_disk_command_workqe *workqe = container_of(work, struct nvmeibc_disk_command_workqe, work);
	struct nvmeibc_disk_command *dcmd = container_of(workqe, struct nvmeibc_disk_command, work);
	struct nvmeibc_disk_io_command *bcmd = disk_to_block(dcmd);
	const struct nvmeib_cpu_mask cpu_mask = bcmd->reqs[0].cpu_mask_info->mask;

	nvmeib_completion_noise_start(NVMEIB_NOISE_COMPLETION);
	nvmeibc_ib_net_complete_bcmd(dcmd, workqe->done_type, workqe->dev);
	nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
			NVMEIB_NOISE_CTRS_IO_COMPLETE_CB_PCPU_WQ);
}

void nvmeibc_ib_net_complete_iocmd_reuse(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, enum stats_done_info_type done_type, int comp_code, int orig_sgcount, bool was_reuse)
{
	struct nvmeibc_disk_io_command *bcmd = req->bcmd;
	const struct nvmeib_cpu_mask cpu_mask = req->bcmd->reqs[0].cpu_mask_info->mask;
	static unsigned int pcpu_cntr = 0;

	nvmeibc_in_net_warn_on_remote_cmd_wip(net, comp_code);
	nvmeibc_ib_net_unmap_and_unlink_iocmd_reuse(net, req, comp_code, orig_sgcount, was_reuse);

	if (NVMEIBC_DISK_SHOULD_DEFER_TO_PCPU_WQ(nvmeibc_ib_net_complete_iocmd_use_pcpu_wq && \
		!NVMEIBC_DISK_SAFE_TEST_CURRENT_CPU_IN_BITMAP(&cpu_mask), cpu_mask)) {
			unsigned int resched_cpu = NVMEIBC_DISK_GET_RESCHED_CPU(cpu_mask.cpus, pcpu_cntr);
			WQ_INIT_WORK(&bcmd->disk_cmd.work.work, nvmeibc_ib_net_complete_bcmd_work);
			bcmd->disk_cmd.ulp_cb_done_type = done_type;
			nvmeib_pcpu_wq_add_work_on_core(nvmeib_get_system_wq(), resched_cpu, &bcmd->disk_cmd.work.work);
			nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
										NVMEIB_NOISE_CTRS_IO_PCPU_CHANNEL_NOT_IN_MASK);
	} else if (NVMEIBC_NRCH_DEFER_COMPLETE_IOCMD && bcmd->disk_cmd.defer_cb) {
		bcmd->disk_cmd.ulp_cb = nvmeibc_ib_net_complete_bcmd;
		bcmd->disk_cmd.ulp_cb_done_type = done_type;
	} else {
		nvmeibc_ib_net_complete_bcmd(&bcmd->disk_cmd, done_type, net->port->nic_dev);
		nvmeib_completion_noise_end(NVMEIB_NOISE_COMPLETION, cpu_mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS,
										NVMEIB_NOISE_CTRS_IO_COMPLETE_CB);
	}
}

void
nvmeibc_ib_net_unmap_data(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req)
{
	struct nvmeib_dev *nvdev = P2NV(net->port);
	struct ib_pool_fmr **pfmr;
	int i;

	__NFIN;
	if (nvdev->use_fast_reg) {
		if (req->nmdesc) {
			struct nvmeib_fr_pool *fr_pool = P2NV(net->port)->fr_pool;
			nvmeib_fast_reg_pool_put(fr_pool, req->fr_list,
				req->nmdesc);
			if (fr_pool->n_error) {	/* Schedule maintenance on main wq */
				nvmeibc_run_on_main_wq1(nvmeibc_cinst_get_core_m(&net->admin_ch->base),
										(nvmeibc_main_wq_fn_type)nvmeib_fast_reg_pool_rereg, fr_pool);
			}
		}
	}
#if KS_IB_VERBS_SUPPORTS_FMR
	else
		for (i = req->nmdesc, pfmr = req->fmr_list; i > 0; i--, pfmr++)
			ib_fmr_pool_unmap(*pfmr);
#else
	(void)i;(void)pfmr;
#endif

	_ND(debug_c_net_unmap_data, "req @REQ nmdesc @NMDESC bcmd @BCMD",
		req, req->nmdesc, req->bcmd);
	if (req->nmdesc) {
#ifdef DEBUG_TRANSFERS_CHECK_NDB_MAPPED
		if (req->bcmd && req->bcmd->disk_cmd.cmd_type == NVMEIBC_DISK_CMD_IO) {
			BUG_ON(!req->bcmd->reqs->ndb_mapped);
			req->bcmd->reqs->ndb_mapped = false;
		}
#endif
		req->nmdesc = 0;
	}
	__NFOUT;
}

void nvmeibc_ib_net_free_req(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req)
{
	__NFIN;
	nvmeibc_ib_net_unmap_sg_to_ib_sge(req->cmd);
	nvmeibc_ib_net_unmap_data(net, req);
	__NFOUT;
}

int nvmeibc_ib_net_alloc_volume_req(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int index)
{
	struct nvmeib_dev *dev = P2NV(net->port);
	dma_addr_t dma_addr;
	void *mr_list;
	int max_rdma_iu;
	struct nvmeib_rdma_iu *rius;
	int indirect_size = NVMEIBC_DEFAULT_MAX_INDIRECT_IO;
	int j, rv = -ENOMEM;

	__NFIN;
	if (!(mr_list = kzalloc(NVMEIBS_MAX_IO_CHANNEL_MSGS * sizeof(void *),
		GFP_KERNEL))) {
		_NE(error_ib_net_nvmeibc_ib_net_alloc_volume_req, "OOM: Fail to allocate mr_list");
		goto out;
	}
	if (dev->use_fast_reg)
		req->fr_list = mr_list;
	else
		req->fmr_list = mr_list;
	if (!(req->map_page = kzalloc(dev->max_pages_per_mr * sizeof(void *), GFP_KERNEL))) {
		_NE(error_1_ib_net_nvmeibc_ib_net_alloc_volume_req, "OOM: Fail to allocate req->map_page");
		goto free_mr;
	}
	if (!(req->indirect_desc = kzalloc(indirect_size, GFP_KERNEL))) {
		_NE(error_2_ib_net_nvmeibc_ib_net_alloc_volume_req, "OOM: Fail to allocate req->indirect_desc");
		goto free_map_page;
	}
	/* JH IOMMU: DMA_TO_DEVICE is correct, only used for source for Local RDMA_SEND */
	if (!(req->cmd = nvmeib_alloc_ioctx(dev->ib_dev, sizeof(*req->cmd),
		NVMEIBC_NORDDA_CLIENT_MSG_SIZE, DMA_TO_DEVICE, NULL))) {
		_NE(error_3_ib_net_nvmeibc_ib_net_alloc_volume_req, "OOM: Fail to allocate req->cmd");
		goto free_indirect;
	}
	/* JH IOMMU: DMA_TO_DEVICE is correct, only used for source for Local RDMA_SEND */
	if (!(req->reuse_cmd = nvmeib_alloc_ioctx(dev->ib_dev, sizeof(*req->reuse_cmd),
		NVMEIBC_NORDDA_CLIENT_IO_REQ_MIN_SIZE, DMA_TO_DEVICE, NULL))) {
		_NE(error_7_ib_net_nvmeibc_ib_net_alloc_volume_req, "OOM: Fail to allocate req->reuse_cmd");
		goto free_cmd;
	}

	/* allocate rdma info buffers - we must have as many rdma_iu as the
	   total messages since we cannot do SG on the read back */
	max_rdma_iu = NVMEIBS_MAX_IO_CHANNEL_MSGS;
	if (!max_rdma_iu ||
		!(rius = kzalloc(max_rdma_iu * sizeof(*rius), GFP_KERNEL))) {
		_NE(error_4_ib_net_nvmeibc_ib_net_alloc_volume_req, "OOM: Fail to allocate rius");
		goto free_cmd;
	}
	for (j = 0; j < max_rdma_iu; ++j) {
		if (!(rius[j].sge = kzalloc(
			NVMEIB_DEF_SG_PER_WQE * sizeof(*rius[j].sge), GFP_KERNEL))) {
			_NE(error_5_ib_net_nvmeibc_ib_net_alloc_volume_req, "OOM: Fail to allocate rius[@INDEX].sge", j);
			goto free_rius;
		}
	}
	req->cmd->opcode = NVMEIB_IU_PRIV;
	req->cmd->max_rdma_iu = max_rdma_iu;
	req->cmd->rius = rius;
	req->cmd->index = index;
	req->cmd->priv = req;

	req->reuse_cmd->opcode = NVMEIB_IU_PRIV;
	req->reuse_cmd->max_rdma_iu = max_rdma_iu;
	req->reuse_cmd->rius = rius;
	req->reuse_cmd->index = index;
	req->reuse_cmd->priv = req;

	/* JH IOMMU: DMA_TO_DEVICE is correct, (Not currently used, but was intended as a source for Remote RDMA_READ) */
	dma_addr = ib_dma_map_single(dev->ib_dev, req->indirect_desc,
		NVMEIBC_DEFAULT_MAX_INDIRECT_IO, DMA_TO_DEVICE);
	if (ib_dma_mapping_error(dev->ib_dev, dma_addr)) {
		_NE(error_6_ib_net_nvmeibc_ib_net_alloc_volume_req, "ib_dma_map_single() error");
		goto free_rius;
	}
	req->indirect_dma_addr = dma_addr;
	req->index = index;
	req->reused_bb = 0;
	req_reused_bb_state_init(req);
	req_reused_bb_lru_init(req);
	rv = 0;

out:
	__NFOUT;
	return rv;

free_rius:
	for (j = 0; j < max_rdma_iu; ++j)
		kfree(rius[j].sge);
	kfree(rius);

free_cmd:
	if (req->reuse_cmd) {
		/* JH IOMMU: DMA_TO_DEVICE is correct, used as a source for Local RDMA_SEND */
		nvmeib_free_ioctx(dev->ib_dev, req->reuse_cmd,
						  NVMEIBC_NORDDA_CLIENT_IO_REQ_MIN_SIZE, DMA_TO_DEVICE);
	}
	req->reuse_cmd = NULL;
	if (req->cmd) {
		/* JH IOMMU: DMA_TO_DEVICE is correct, used as a source for Local RDMA_SEND */
		nvmeib_free_ioctx(dev->ib_dev, req->cmd,
						  NVMEIBC_NORDDA_CLIENT_MSG_SIZE, DMA_TO_DEVICE);
	}
	req->cmd = NULL;

free_indirect:
	kfree(req->indirect_desc);
	req->indirect_desc = NULL;

free_map_page:
	kfree(req->map_page);
	req->map_page = NULL;

free_mr:
	kfree(req->fr_list);
	kfree(req->fmr_list);
	req->fr_list = NULL;
	req->fmr_list = NULL;
	goto out;
}

void nvmeibc_ib_net_free_volume_req(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req)
{
	struct nvmeib_dev *dev;
	int j;

	__NFIN;
	kfree(req->fmr_list);
	req->fmr_list = NULL;
	kfree(req->map_page);
	req->map_page = NULL;
	if (req->indirect_dma_addr) {
		dev = P2NV(net->port);
		/* JH IOMMU: DMA_TO_DEVICE is correct, (Not currently used, but designed to be a source for Remote RDMA_READ) */
		ib_dma_unmap_single(dev->ib_dev, req->indirect_dma_addr,
			NVMEIBC_DEFAULT_MAX_INDIRECT_IO, DMA_TO_DEVICE);
	}
	req->indirect_dma_addr = 0;
	kfree(req->indirect_desc);
	req->indirect_desc = NULL;
	/* free command if allocated */
	if (req->cmd) {
		/* free rdma iu */
		for (j = 0; j < req->cmd->max_rdma_iu; ++j)
			kfree(req->cmd->rius[j].sge);
		kfree(req->cmd->rius);
		req->cmd->rius = NULL;
		/* free attached iu */
		dev = P2NV(net->port);
		/* JH IOMMU: DMA_TO_DEVICE is correct, used as a source for Local RDMA_SEND */
		nvmeib_free_ioctx(dev->ib_dev, req->cmd,
						  NVMEIBC_NORDDA_CLIENT_MSG_SIZE, DMA_TO_DEVICE);
		req->cmd = NULL;
	}
	if (req->reuse_cmd) {
		/* free attached iu */
		dev = P2NV(net->port);
		/* JH IOMMU: DMA_TO_DEVICE is correct, used as a source for Local RDMA_SEND */
		nvmeib_free_ioctx(dev->ib_dev, req->reuse_cmd,
						  NVMEIBC_NORDDA_CLIENT_IO_REQ_MIN_SIZE, DMA_TO_DEVICE);
		req->cmd = NULL;
	}
	__NFOUT;
}

void nvmeibc_ib_net_get_bufs_nbufs(struct nvmeibc_volume_request *req,
	struct nvmeib_direct_buf **bufs, int *nbufs, int *len)
{
	NFIN;

	if (req->ndesc > 1) {
		*bufs = req->indirect_desc;
		*nbufs = req->ndesc;
		*len = req->total_dma_len;
		_ND(trace_ib_net_nvmeibc_ib_net_get_bufs_nbufs, "len=@LEN", *len);
	}
	else {
		*bufs = &req->table_desc;
		*nbufs = 1;
		*len = (*bufs)->len;
		_ND(trace_1_ib_net_nvmeibc_ib_net_get_bufs_nbufs, "len=@LEN", *len);
	}
	NFOUT;
}

void nvmeibc_ib_net_print_riu(struct nvmeib_rdma_iu *riu)
{
	int i;

	_ND(trace_ib_net_nvmeibc_ib_net_print_riu, "raddr=@RADDR, rkey=@RKEY", riu->raddr, riu->rkey);
	for (i = 0; i < riu->sge_cnt; ++i)
		_ND(trace_1_ib_net_nvmeibc_ib_net_print_riu, "laddr=@LADDR, length=@LENGTH_INT, lkey=@LKEY",
			riu->sge[i].addr, riu->sge[i].length, riu->sge[i].lkey);
}

void nvmeibc_ib_net_fill_wr(struct nvmeib_send_wr *wr, u64 id,
	struct nvmeib_rdma_iu *riu)
{
	NFIN;
	nvmeibc_ib_net_print_riu(riu);
	nvmeib_send_wr_common(*wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(*wr).wr_id = id;
	nvmeib_send_wr_rdma(*wr).remote_addr = riu->raddr;
	nvmeib_send_wr_rdma(*wr).rkey = riu->rkey;
	nvmeib_send_wr_common(*wr).num_sge = riu->sge_cnt;
	nvmeib_send_wr_common(*wr).sg_list = riu->sge;
	NFOUT;
}

int nvmeibc_ib_net_post_recvq(struct nvmeibc_ib_net *net, struct nvmeib_recvq *rq, struct nvmeib_iu *iu)
{
	int rv = 0;

	NFIN;
	if (!atomic_read(&net->dying)) {
		if (!net->qp) {
			_NE(error_ib_net_nvmeibc_ib_net_post_recvq, "nvmeibc_ib_net_post_recvq called with NULL QP");
			rv = -EIO;
		}
		else if (!(rv = nvmeib_post_recvq(rq, net->qp, iu))) {
			atomic_inc(&net->rq_post_count);
		}
	} else
		rv = -ENOTCONN;
	NFOUT;
	return rv;
}

const char *nvmeibc_ib_net_state_str(enum nvmeibc_ib_net_state state)
{
	switch (state) {
	case NVMEIBC_IB_NET_INIT:
		return "Init";
	case NVMEIBC_IB_NET_CONNECTING:
		return "Connecting";
	case NVMEIBC_IB_NET_LIVE:
		return "Live";
	case NVMEIBC_IB_NET_DISCONNECTING:
		return "Disconnecting";
	case NVMEIBC_IB_NET_REMOVED:
		return "Removed";
	case NVMEIBC_IB_NET_ERR:
		return "Error";
	default:
		return "Unknown";
	}
}

int nvmeibc_ib_net_map_gen_data(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req)
{
	struct nvmeib_dev *nvdev = P2NV(net->port);
	struct nvmeibc_disk_gen_cmd *gen_cmd = disk_to_gen(req->dcmd);
	struct nvmeib_mr_info mri = {};
	void *fmr;
	int rv, i;
	__NFIN;

	/* checks */
	if (gen_cmd->n_data_sink > NVMEIB_MAX_GEN_CMD_DATA_SINK) {
		_NE(error_ib_net_nvmeibc_ib_net_map_gen_data, "n_data_sink: @COUNT > max: @COUNT",
			gen_cmd->n_data_sink, NVMEIB_MAX_GEN_CMD_DATA_SINK);
		rv = -EINVAL;
		goto out;
	}
	req->ndesc = 0;
	req->nmdesc = 0;
	req->pgcount = 0;
	req->total_dma_len = 0;

	for (i = 0; i < gen_cmd->n_data_sink; i++) {
		struct nvmeib_buffer *gen_sink_buf = &gen_cmd->data_sink[i]->local;
		int dma_nents;
		_NT(trace_ib_net_nvmeibc_ib_net_map_gen_data,
			"DMA map SGL @PTR with @NENTS entries and length @LENGTH_INT bytes",
			gen_sink_buf->sgt.sgl,
			gen_sink_buf->sgt.nents,
			gen_sink_buf->size);

		/* JH IOMMU: DMA_FROM_DEVICE is correct. These maps are only used for sinks of Remote RDMA Write */
		if ((rv = nvmeib_public_ib_dma_map_sg(P2IB(net->port), gen_sink_buf->sgt.sgl,
			gen_sink_buf->sgt.nents, DMA_FROM_DEVICE)) <= 0)
		{
			_NT(trace_map_gen_data_map_sg_fail, 
				"Failed (@RV) to DMA map SGL @PTR with "
				"@NENTS entries and length @LENGTH_INT bytes",
				rv, gen_sink_buf->sgt.sgl,
				gen_sink_buf->sgt.nents,
				gen_sink_buf->size);
			rv = -EFAULT;
			goto unmap_gen_data;
		}
		dma_nents = rv;

		mri.fmr_pool = nvdev->fmr_pool;
		mri.iu = NULL;
		mri.null_iu_idx = -1; /* TBD(Put something more meaningful in here) */
		mri.qp = net->qp;
		mri.use_sg = true;
		mri.sg = gen_sink_buf->sgt.sgl;
		mri.count = dma_nents;
		mri.offset = gen_sink_buf->offset;
		mri.dma_len = 0; /* Will be set by ib_map_mr_sg */

		mri.owner = net;
		if (!(fmr = nvmeib_map_mr(nvdev, &mri))) {
			_NE(error_3_ib_net_nvmeibc_ib_net_map_gen_data, "Fail to map mr");
			rv = -ENOMEM;
			goto unmap_gen_data;
		}
		_NT(trace_3_ib_net_nvmeibc_ib_net_map_gen_data, "mapped gen_cmd sink SGL @PTR (ents @NENTS) (@SIZE_T bytes) to FMR @PTR with @N_PAGES pages - rkey: @RKEY raddr: @RADDR offset: @OFFSET_LONG dma_len: @LENGTH\n",
			gen_sink_buf->sgt.sgl,
			gen_sink_buf->sgt.nents,
			gen_sink_buf->size,
			fmr, mri.n_pages, mri.rkey, mri.io_addr, mri.offset, mri.dma_len);
		req->fmr_list[req->nmdesc++] = fmr;
		req->total_dma_len += mri.dma_len;
		gen_cmd->data_sink[i]->remote.raddr = mri.io_addr;
		gen_cmd->data_sink[i]->remote.rkey = mri.rkey;
		gen_cmd->data_sink[i]->remote.len = mri.dma_len;
	}
	rv = 0;
	goto out;

unmap_gen_data:
	nvmeibc_ib_net_unmap_gen_data(net, req);

out:
	__NFOUT;
	return rv;
}

void nvmeibc_ib_net_unmap_gen_data(struct nvmeibc_ib_net *net,
								struct nvmeibc_volume_request *req)
{
	int i;
	struct nvmeibc_disk_gen_cmd *gen_cmd = disk_to_gen(req->dcmd);
	__NFIN;

	/* Unmap all the FR/FMRs */
	nvmeibc_ib_net_unmap_data(net, req); //Potential LOC_PROT send-comp err

	for (i = 0; i < gen_cmd->n_data_sink; i++) {
		struct nvmeib_buffer *gen_sink_buf = &gen_cmd->data_sink[i]->local;
		_NT(trace_1_ib_net_nvmeibc_ib_net_unmap_gen_data,
		    "gen_cmd @GEN_CMD_OP (@PTR) sink[@IDX]: SGL @PTR entries @NENTS\n", gen_cmd->opcode,
		    gen_cmd, i, gen_cmd->data_sink[i]->local.sgt.sgl, gen_cmd->data_sink[i]->local.sgt.nents);
		
		nvmeib_public_ib_dma_sync_sg_for_cpu(P2IB(net->port), gen_sink_buf->sgt.sgl,
						     gen_sink_buf->sgt.nents, DMA_FROM_DEVICE);
		/* JH IOMMU: DMA_FROM_DEVICE is correct. These maps are only used for sinks of Remote RDMA Write */
		nvmeib_public_ib_dma_unmap_sg(P2IB(net->port), gen_sink_buf->sgt.sgl,
					      gen_sink_buf->sgt.orig_nents, DMA_FROM_DEVICE);
	}

	__NFOUT;
}

int nvmeibc_ib_net_jmdc_pb_map(struct nvmeib_dev *nv,
							   struct nvmeibc_jmdc_pb_rsrc *pb_rsrc,
							   binje_t binje)
{
	struct ib_device *ib_dev = nv->ib_dev;
	int rv = 0, i;

	if (pb_rsrc->mapped) {
		rv = -EALREADY;
		goto out;
	}

	/* JH IOMMU: DMA_TO_DEVICE is correct, only used for RDMA_WRITE */
	pb_rsrc->src_laddr = ib_dma_map_single(ib_dev, &pb_rsrc->src,
										   sizeof(pb_rsrc->src),
										   DMA_TO_DEVICE);
	if (ib_dma_mapping_error(ib_dev, pb_rsrc->src_laddr)) {
		_NE(error_disk_nvmeibc_ib_net_jmdc_pb_map,
			"IB DMA Mapping JMDC piggy-back source failed");
		rv = -EFAULT;
		goto out;
	}

	for (i = 0; i < NVMEIBC_MAX_JAM_RDMA_OPS; i++) {
		pb_rsrc->src_sge[i].lkey = nvmeib_get_lkey(nv);
		pb_rsrc->src_sge[i].addr = pb_rsrc->src_laddr +
			i * sizeof(pb_rsrc->src[i]);
		/* we may only rdma-access part of src[i] */
		pb_rsrc->src_sge[i].length = sizeof(pb_rsrc->src[i].jblks_md[0]) * binje;
	}

	pb_rsrc->mapped = true;

out:
	return rv;
}

void nvmeibc_ib_net_jmdc_pb_unmap(struct nvmeib_dev *nv,
								  struct nvmeibc_jmdc_pb_rsrc *pb_rsrc)
{
	if (pb_rsrc->mapped) {
		pb_rsrc->mapped = false;
		/* JH IOMMU: DMA_TO_DEVICE is correct, used as source for local RDMA_WRITE (Write to remote JMDC) */
		ib_dma_unmap_single(nv->ib_dev, pb_rsrc->src_laddr,
							sizeof(pb_rsrc->src), DMA_TO_DEVICE);
	}
}

int nvmeibc_ib_net_jmdc_pb_fill_wrs(struct nvmeibc_ib_net *net,
								struct nvmeibc_disk_io_command *bcmd,
								struct nvmeib_remote_access_info *jmdc_rai,
								struct nvmeibc_jmdc_pb_rsrc *pb_rsrc,
								struct nvmeib_send_wr *wr_jmdc)
{
	int rv = 0, i;
	u32 sw2hw = NVMEIBC_SECTOR_SHIFT - bcmd->disk->sector_shift;

	NFIN;
	if (bcmd->reqs[0].jam_op.n_ops > NVMEIBC_MAX_JAM_RDMA_OPS) {
		_NE(error_ib_net_nvmeibc_ib_net_jmdc_pb_fill_wrs, "Invalid number of jmdc piggy-back rdma ops @N_OPS", bcmd->reqs[0].jam_op.n_ops);
		rv = -EINVAL;
		goto out;
	}
	for (i = 0; i < bcmd->reqs[0].jam_op.n_ops; i++, wr_jmdc++) {
		/* copy the jblks-md from ulp and poison the rest of jmdc-entry's */
		nvmeibc_jentry_md_container_fill(
			&pb_rsrc->src[i], &bcmd->reqs[0].jam_op.jmdc_ent[i], sw2hw,
			NVMEIBC_DCMD_LEN_TO_SW_SECTORS(bcmd), bcmd->disk->jour.rng_binje);

		memset(wr_jmdc, 0, sizeof(*wr_jmdc));
		nvmeib_send_wr_common(*wr_jmdc).wr_id = NVMEIB_RDMA_JMDC_PB;
		nvmeib_send_wr_common(*wr_jmdc).send_flags = 0;
		nvmeib_send_wr_common(*wr_jmdc).opcode = IB_WR_RDMA_WRITE;
		nvmeib_send_wr_set_next(*wr_jmdc, (wr_jmdc + 1));
		nvmeib_send_wr_rdma(*wr_jmdc).rkey = jmdc_rai->rkey;
		nvmeib_send_wr_rdma(*wr_jmdc).remote_addr = jmdc_rai->raddr +
			bcmd->reqs[0].jam_op.jour_idx[i] * (sizeof(pb_rsrc->src[i].jblks_md[0]) * net->ioch->disk->jour.rng_binje);
		nvmeib_send_wr_common(*wr_jmdc).sg_list = &pb_rsrc->src_sge[i]; /* pb_rsrc->src_sge[i].length is sizeof(pb_rsrc->src[i].jblks_md[0]) * binje) */
		nvmeib_send_wr_common(*wr_jmdc).num_sge = 1;
		_ND(trace_ib_net_nvmeibc_ib_net_jmdc_pb_fill_wrs, "JMDC PIGGYBACK @INDEX: @RAW @ @LKEY:@ADDR (@LENGTH_INT) -> @RKEY:@REMOTE_ADDR_LLONG (@RADDR + @IO_OFFSET) - Net @NET Remote QPn: @REMOTE_QPN",
		i, pb_rsrc->src[i].jblks_md[0].raw, pb_rsrc->src_sge[i].lkey,
			pb_rsrc->src_sge[i].addr, pb_rsrc->src_sge[i].length,
			nvmeib_send_wr_rdma(*wr_jmdc).rkey, nvmeib_send_wr_rdma(*wr_jmdc).remote_addr,
			jmdc_rai->raddr,
			(int)(bcmd->reqs[0].jam_op.jour_idx[i] *
				  (sizeof(union jblock_md) * net->ioch->disk->jour.rng_binje)),
			net, net->remote_qpn);
	}

	/* JH IOMMU: Correct. Syncs data from CPU to Device to be used as a source for Local RDMA_WRITE (Write to remote JMDC) */
	ib_dma_sync_single_for_device(P2IB(net->port), pb_rsrc->src_laddr,
								sizeof(pb_rsrc->src), DMA_TO_DEVICE);

	rv = i;

out:
	NFOUT;
	return rv;
}

int nvmeibc_ib_net_execute_ka(struct nvmeibc_ib_net *net,
							  struct nvmeib_remote_access_info *rai,
							  void *src, size_t src_len)
{
	struct nvmeib_send_wr wr = {};
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	struct nvmeibc_channel *ch = net->ioch;
	int rv = 0;
	struct ib_sge sge;

	NFIN;
	BUG_ON(!nvmeibc_channel_already_locked(ch));
	if (atomic_read(&net->dying)) {
		_NT(trace_ib_net_nvmeibc_ib_net_execute_ka, "Channel @CH_NAME net is dying", ch->name);
		rv = -ENOTCONN;
		goto out;
	}

	if (net->io_ka.in_progress) {
		_NT(trace_1_ib_net_nvmeibc_ib_net_execute_ka, "Channel @CH_NAME keep-alive is already in progress", ch->name);
		rv = -EALREADY;
		goto out;
	}

	/* Default is zero-length RDMA Write for Keep-Alive */
	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(wr).wr_id = nvmeib_encode_wr_id(NVMEIB_RDMA_IO_KA, 0);
	nvmeib_send_wr_common(wr).send_flags = IB_SEND_SIGNALED;
	nvmeib_send_wr_common(wr).num_sge = 0;
	nvmeib_send_wr_common(wr).sg_list = NULL;
	nvmeib_send_wr_clear_next(wr);

	if (src && src_len > 0) {
		if (!net->io_ka.src || net->io_ka.src_len < src_len) {
			_NT(trace_2_ib_net_nvmeibc_ib_net_execute_ka, "Channel @CH_NAME keep-alive src mem invalid src: @SRC_PTR src_len: @SRC_LEN",
			   ch->name, net->io_ka.src, net->io_ka.src_len);
			rv = -ENOMEM;
			goto out;
		} else if (!rai || rai->rkey == 0 || rai->len < src_len) {
			_NT(trace_3_ib_net_nvmeibc_ib_net_execute_ka, "Invalid Remote Access Info rai: @RAI rkey: @RKEY rlen: @LEN",
			   rai, rai ? rai->rkey : 0, rai ? rai->len : 0);
			rv = -EINVAL;
			goto out;
		}

		memcpy(net->io_ka.src, src, src_len);
		/* JH IOMMU: Correct. Syncs data from CPU to Device to be used as a source for Local RDMA_WRITE (Write to Remote IO KA) */
		ib_dma_sync_single_for_device(P2IB(net->port), net->io_ka.src_dma,
									  src_len, DMA_TO_DEVICE);

		nvmeib_send_wr_rdma(wr).remote_addr = rai->raddr;
		nvmeib_send_wr_rdma(wr).rkey = rai->rkey;
		nvmeib_send_wr_common(wr).num_sge = 1;
		nvmeib_send_wr_common(wr).sg_list = &sge;
		sge.lkey = nvmeib_get_lkey(P2NV(net->port));
		sge.addr = net->io_ka.src_dma;
		sge.length = src_len;
	}

#if ENABLE_SIW
	if (P2NV(net->port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_TIMESTAMP;
		if (net->shared_cq)
			nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
		else
			nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

	_NT(trace_4_ib_net_nvmeibc_ib_net_execute_ka, "Sending Keep-Alive on Channel @CH_NAME", ch->name);
	if (!net->dev_cq && !nvmeibc_channel_is_pcpu_ch(net->ioch))
		nvmeibc_ib_net_req_notify_send_cq(net);
	if ((rv = nvmeibc_ib_post_send(net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr))) {
		_NT(trace_5_ib_net_nvmeibc_ib_net_execute_ka, "ib-post_send failed (@RV) for Channel @CH_NAME", rv, ch->name);
		goto out;
	}

	net->io_ka.in_progress = true;

out:
	NFOUT;
	return rv;
}

#if	defined(DEBUG_REQ_REUSED_BB_STATE) && (DEBUG_REQ_REUSED_BB_STATE == 1)
void req_reused_bb_lru_init(struct nvmeibc_volume_request *req)
{
	req->reused_bb_lru_jif = 0;
	INIT_LIST_HEAD(&req->reused_bb_lru_link);
}

void req_reused_bb_lru_add(struct nvmeibc_channel *ch,
						   struct nvmeibc_volume_request *req)
{
	unsigned long flags = 0;

	if (!nvmeibc_use_pcpu_cq)
		return;

	spin_lock_irqsave(&ch->reused_bb_list_guard, flags);
	if (!list_empty(&req->reused_bb_lru_link)) {
		_NE_dmesg(err_req_reused_bb_lru_add,
				  "REQ_REUSED_LRU: ch=@PTR, req=@PTR already linked", ch, req);
		BUG();
	}
	req->reused_bb_lru_jif = jiffies;
	list_add_tail(&req->reused_bb_lru_link, &ch->reused_bb_list);
	ch->reused_bb_cnt++;
	ch->reused_bb_lru_jif = req->reused_bb_lru_jif;
	spin_unlock_irqrestore(&ch->reused_bb_list_guard, flags);
}

void req_reused_bb_lru_del(struct nvmeibc_channel *ch,
						   struct nvmeibc_volume_request *req)
{
	unsigned long flags = 0;

	if (!nvmeibc_use_pcpu_cq)
		return;

	spin_lock_irqsave(&ch->reused_bb_list_guard, flags);
	if (list_empty(&req->reused_bb_lru_link)) {
		_NE_dmesg(err_req_reused_bb_lru_del,
				  "REQ_REUSED_LRU: ch=@PTR, req=@PTR not linked", ch, req);
		BUG();
	}
	req->reused_bb_lru_jif = 0;
	list_del_init(&req->reused_bb_lru_link);
	ch->reused_bb_cnt--;
	ch->reused_bb_lru_jif = list_empty(&ch->reused_bb_list) ? 0 :
		list_first_entry(&ch->reused_bb_list, struct nvmeibc_volume_request,
						 reused_bb_lru_link)->reused_bb_lru_jif;
	spin_unlock_irqrestore(&ch->reused_bb_list_guard, flags);
}

/* call this before freeing reqs so disk's /proc wont traverse freed reqs mem */
void req_reused_bb_lru_flush(struct nvmeibc_channel *ch)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&ch->reused_bb_list_guard, flags);
	_NT(t_req_reused_bb_lru_flush,
		"ch=@PTR, flush @UINT reqs owned by ulp", ch, ch->reused_bb_cnt);
	INIT_LIST_HEAD(&ch->reused_bb_list);
	ch->reused_bb_cnt = 0;
	ch->reused_bb_lru_jif = 0;
	spin_unlock_irqrestore(&ch->reused_bb_list_guard, flags);
}

#define REQ_REUSED_BB_TIMEOUT (HZ / 100) /* every 10 (effectively 9 to 11) msec */
void req_reused_bb_lru_is_timeout_stats(struct nvmeibc_channel *ch)
{
	if (ch->reused_bb_lru_jif - jiffies > REQ_REUSED_BB_TIMEOUT)
		nvmeibc_disk_counters_inc(ch->disk, n_err_ulp_reuse_req_timeout);
}
#endif /* DEBUG_REQ_REUSED_BB_STATE */

/*
 * Per device (shared) CQs
 */
static void handle_recv_dying(struct nvmeibc_ib_net *net, struct ib_wc *wc)
{
	struct nvmeib_iu *iu;
	int index;
	int rv;
	__NFIN;

	BUG_ON(!net->dev_cq);
	index = nvmeib_idx_from_wc(wc);
	iu = nvmeib_srq_rtrv_recv(net->srq_info, index, net);
	rv = nvmeib_srq_post_recv(net->srq_info, iu);
	if (rv)
		_NT(handle_recv_dying_t1, "Failed post-recv, error code @INT", rv);

	__NFOUT;
}

static void handle_recv(struct nvmeibc_ib_net *net, struct ib_wc *wc)
{
	int rv;
	__NFIN;

	BUG_ON(!net->dev_cq);

	if (atomic_read(&net->dying)) {
		_NTn(handle_recv_t1, net, "net is dying");
		handle_recv_dying(net, wc);
	} else {
		if ((rv = net->call_receive_comp_handler(net, wc)) < 0 ||
			atomic_read(&net->dying)) {
			_NEn(handle_recv_e1, net,
				 "recv-comp-handling error? (@INT) or net is dying ? (@STR), "
				 "disconnect net (rq_post_count: @INT)",
				 rv, atomic_read(&net->dying) ? "TRUE" : "FALSE",
				atomic_read(&net->rq_post_count));
			nvmeibc_ib_net_disconnect(net);

			//omril: if called, handler must always post iu back to SRQ (regardless of rv)
			//handle_recv_dying(net, wc);
		}
	}

	__NFOUT;
}

static void handle_send(struct nvmeibc_ib_net *net, struct ib_wc *wc)
{
	__NFIN;
	if (nvmeib_opcode_from_wc(wc) == NVMEIB_DRAIN_QUEUE) {
		/* In case of shared SCQ, drain may not be for this net's QP */
		if (!net->drain_sq_done)
			_NWn(handle_send_w1, net, "No drain-sq comp-item");
		else {
			_NTn(handle_send_t1, net, "drain-sq done");
			complete(net->drain_sq_done);
		}
		goto out;
	}

	if (!atomic_read(&net->dying) &&
		net->call_send_comp_handler(net, wc, true) < 0) {
		_NEn(handle_send_e20, net,
			 "send-comp-handling error (@INT32_HEX), disconnect net qp=@PTR",
			 wc->status, net->qp);
		nvmeibc_ib_net_disconnect(net);
	}
out:
	__NFOUT;
}

static DEV_CQ_PROCESS_FUNC(process_per_dev_cq)
{
	struct nvmeibc_ib_net *net = ctx;
	struct nvmeibc_channel *ioch = net->ioch;
	unsigned long flags;
	u64 start, delta;
	int i;

	__NFIN;

	start = jiffies;
	nvmeibc_channel_spin_lock_irqsave(ioch, &flags);

	for_each_set_bit(i, wcs_mask, n_wcs) {
		struct ib_wc *wc = &wcs[i];
		if (wc->status != IB_WC_SUCCESS) {
			_NE(c_process_per_dev_cq, "wc=@PTR, wc-status=@NVMEIB_STATUS_STR(@WC_STATUS): "
				"wc->wr_id=@LLX={ver=@X, opc=@X, idx=@X (is_recv=@BOOL)}, "
				"wc->opcode=@WC_OPCODE, "
				"wc->qp=@QP, net=@NET, ct=(@INT, @STR)",
				wc, nvmeib_status_str(&wc->status), wc->status,
				nvmeib_wr_id_from_wc(wc),
				nordda_wr_id_decode_version(nvmeib_wr_id_from_wc(wc)),
				nvmeib_opcode_from_wc(wc),
				nvmeib_idx_from_wc(wc),
				nvmeib_opcode_from_wc(wc) == NVMEIB_RECV,
				wc->opcode,
				wc->qp, net,
				ioch ? ioch->ct : -1, ch_type_to_str(ioch ? ioch->ct : -1));
		}

		
		//if (wc->opcode & IB_WC_RECV)
		if (nvmeib_opcode_from_wc(wc) == NVMEIB_RECV) {
			nvmeibc_disk_net_intrs_stats_inc(net->ioch->disk, true);
			handle_recv(net, wc);
			if (in_interrupt()) net->rcq_stats.n_intr++;
			else 				net->rcq_stats.n_poll++;
		}
		else {
			nvmeibc_disk_net_intrs_stats_inc(net->ioch->disk, false);
			handle_send(net, wc);
			if (in_interrupt()) net->scq_stats.n_intr++;
			else 				net->scq_stats.n_poll++;

		}
	}

	nvmeibc_channel_spin_unlock_irqrestore(ioch, flags);
	delta = jiffies - start;
	_ND(process_per_dev_cq_e23, "processing time = @INT64", delta);
	__NFOUT;
}

int nvmeibc_ib_net_poll_cqs(struct nvmeibc_ib_net *net, bool notify)
{
	int n = 0, n_iter = 0;
	int rv;
	bool continue_polling;

	do {
		if ((rv = polling_process_recv_cq_(net, notify, &continue_polling, NULL)) < 0)
			goto out;
		n += rv;
		if (continue_polling) {
			if (++n_iter > nvmeibc_max_notify_cq_iterations) {
				_NEn_dmesg(err_nvmeibc_ib_net_poll_cqs, net, "Failed to arm recv cq after @INT iterations, disconnecting", n_iter);
				nvmeibc_ib_net_disconnect(net);
				rv = -EDEADLK;
				goto out;
			}
			cond_resched();
			continue;
		}
	} while (continue_polling);

	if (!net->shared_cq) {
		n_iter = 0;
		do {
			if ((rv = polling_process_send_cq_(net, notify, &continue_polling, NULL)) < 0)
				goto out;
			n += rv;

			if (continue_polling) {
				if (++n_iter > nvmeibc_max_notify_cq_iterations) {
					_NEn_dmesg(err_2_nvmeibc_ib_net_poll_cqs, net, "Failed to arm send cq after @INT iterations, disconnecting", n_iter);
					nvmeibc_ib_net_disconnect(net);
					rv = -EDEADLK;
					goto out;
				}
				cond_resched();
			}
		} while (continue_polling);
	}

	rv = n;

out:
	return rv;
}
