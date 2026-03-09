/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#define C_IB_ADMIN_CHANNEL_C

#include "nvmeibc_ib_admin_channel.h"
#include "nvmeibc_ib_nordda_channel.h"
#include "nvmeibc_main.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibc_disk_locks.h"
#include "nvmeibc_locks_channel.h"
#include "nvmeibc_jam.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeib.h"
#include "nvmeib_utils.h"
#include "nvmeibc_toma.h"
#include "nvmeib_srq.h"
#include "nvmeib_public.h"
#include "nvmeib_version_shared.h"
#include "nvmeibc_disk_gen_cmds.h"
#include "nvmeibc_trace.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "core/nvmeibc_core_common.h"
#include "vex/nvmeibc_vex_shared.h"
#include "nvmeib_shared.h"

#include "kr_undef.h"

#define __NFIN _ND(__AUTOID__, "--> @CH_NAME:@RANIC_GUID\n", ch->base.base.name, ch->ranic_guid)
#define __NFOUT _ND(__AUTOID__, "<-- @CH_NAME:@RANIC_GUID\n", ch->base.base.name, ch->ranic_guid)

#define TRACE_STAGING_MESSAGE(pref, ch_name, opcode, f) _NT(__AUTOID__, pref" to @BASE_NAME, opcode: @NVMEIB_WR_OPCODE_STR, function: @FUNC", ch_name,  nvmeib_wr_opcode_str(wr_opcode), f);
// static unsigned int max_db_fetched = -1;
// MODULE_PARM_DESC(max_db_fetched, "max dirty bits fetched");
// module_param(max_db_fetched, uint, 0);

static unsigned int nvmeibc_nr_max_channels_per_path = 4;
module_param_named(nr_max_channels_per_path, nvmeibc_nr_max_channels_per_path, uint, 0644);
MODULE_PARM_DESC(nr_max_channels_per_path, "Maximum number of RDMA IO channels per disk per networking path.");

static unsigned int nvmeibc_nr_max_channels_per_path_iommu = 4;
module_param_named(nr_max_channels_per_path_iommu, nvmeibc_nr_max_channels_per_path_iommu, uint, 0644);
MODULE_PARM_DESC(nr_max_channels_per_path_iommu, "Maximum number of RDMA IO channels per disk per networking path when the IOMMU is enabled.");

static unsigned int nvmeibc_nr_max_channels_per_path_tcp = NVMEIB_MAX_NR_TCP_CHANNELS_PER_PATH;
module_param_named(nr_max_channels_per_path_tcp, nvmeibc_nr_max_channels_per_path_tcp, uint, 0644);
MODULE_PARM_DESC(nr_max_channels_per_path_tcp, "Maximum number of SIW IO channels per disk per networking path.");

static bool is_ach_dying(struct nvmeibc_ib_admin_channel *ch)
{
	int d_dying = atomic_read(&ch->base.base.disk->dying);
	int c_dying = atomic_read(&ch->base.base.dying);
	int n_dying = atomic_read(&ch->net.base.dying);
	int dying = d_dying || c_dying || n_dying;

	if (dying) {
		_NT(trace_is_ach_dying,
			"ach dying: Disk @DISK_NAME (@DISK), d=@INT, c=@INT, n=@INT",
			ch->base.base.disk->name, ch->base.base.disk,
			d_dying, c_dying, n_dying);
	}

	return dying;
}

static int init(const struct nvmeibc_cinst_params_core *p, 
	struct nvmeibc_ib_admin_channel *ch)
{
	int rv = 0;

	__NFIN;
	if (!(rv = nvmeibc_admin_channel_init(p, &ch->base))) {
		INIT_LIST_HEAD(&ch->free_tx);
		INIT_LIST_HEAD(&ch->uncomp_tx);
		INIT_LIST_HEAD(&ch->recv_ioctx);
		spin_lock_init(&ch->guard);
		init_completion(&ch->send_done);
	}
	__NFOUT;
	return rv;
}

static void initialize_net(struct nvmeibc_ib_admin_channel *ch)
{
	struct nvmeibc_ib_net *net = &ch->net.base;
	NFIN;

	net->lkey = nvmeib_get_lkey(P2NV(net->port));
	net->rkey = nvmeib_get_rkey(P2NV(net->port));

	NFOUT;
}

static int alloc_msg_area(struct nvmeibc_ib_admin_channel *ch)
{
	struct ib_device *ib = P2IB(ch->net.base.port);
	size_t cmsg_buffer_sz = ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift;
	int rv;

	NFIN;
	/* allocate control msg-area */
	ch->msg_area_map.pd = P2NV(ch->net.base.port)->pd;
	ch->msg_area_map.n_pages = DIV_ROUND_UP(cmsg_buffer_sz, PAGE_SIZE);
	ch->msg_area_map.access_flags =
		IB_ACCESS_LOCAL_WRITE |
		IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE;
	ch->msg_area_map.ioaddr = 0;
	ch->msg_area = nvmeib_mem_alloc_n_vmap(&ch->msg_area_map);
	if (!ch->msg_area) {
		_NE(error_ib_admin_channel_on_login_admin_ch, "Fail to map message area");
		rv = -ENOMEM;
		goto out;
	}
	ch->msg_area_end = ch->msg_area + cmsg_buffer_sz;

	/* allocate keep-alive msg-area */
	ch->ka_msg_area = kzalloc(sizeof(*ch->ka_msg_area), GFP_KERNEL);
	if (!ch->ka_msg_area) {
		_NE(error_1_ib_admin_channel_on_login_admin_ch, "Fail to allocate client keep alive area for rdma");
		rv = -ENOMEM;
		goto err_free_msg_area;
	}
	/* JH IOMMU: Changed to DMA_TO_DEVICE. Only used as source for Local RDMA_WRITE */
	ch->ka_msg_dma_addr = ib_dma_map_single(ib,
						ch->ka_msg_area,
						sizeof(*ch->ka_msg_area),
						DMA_TO_DEVICE);
	if (ib_dma_mapping_error(ib, ch->ka_msg_dma_addr)) {
		_NE(error_2_ib_admin_channel_on_login_admin_ch, "Fail to dma_map client keep alive area for rdma");
		rv = -ENOMEM;
		goto err_free_ka_msg;
	}
	rv = 0;
	goto out;

err_free_ka_msg:
	kfree(ch->ka_msg_area);
	ch->ka_msg_area = NULL;

err_free_msg_area:
	nvmeib_mem_vunmap_n_free(&ch->msg_area_map, ch->msg_area);
	ch->msg_area = NULL;

out:
	NFOUT;
	return rv;
}

static void freee(struct nvmeibc_ib_admin_channel *ch)
{
	__NFIN;
	_NT(trace_ib_admin_channel_freee, "Free admin-ch resources");
	/* delete resources */
	nvmeibc_admin_channel_free(&ch->base);
	/* after destroying wq, free toma-recv rscs */
	nvmeibc_toma_free(ch);
	if (ch->msg_area) {
		nvmeib_mem_vunmap_n_free(&ch->msg_area_map, ch->msg_area);
		ch->msg_area = NULL;
	}
	if (ch->ka_msg_area) {
		/* JH IOMMU: Changed to DMA_TO_DEVICE. Only used as source for Local RDMA_WRITE */
		ib_dma_unmap_single(P2IB(ch->net.base.port), ch->ka_msg_dma_addr,
			sizeof(*ch->ka_msg_area), DMA_TO_DEVICE);
		kfree(ch->ka_msg_area);
		ch->ka_msg_area = NULL;
	}
	if (ch->lsi_alloc) {
		kfree(ch->lsi);
		ch->lsi = NULL;
		ch->lsi_alloc = false;
	}
	__NFOUT;
}

static void wait_io_channels(struct nvmeibc_ib_admin_channel *ch)
{
	struct list_head *rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *lionics;
	struct nvmeibc_io_lnic *lionic;
	struct nvmeibc_ib_nordda_channel *nrch;
	int i;

	NFIN;
	rionics = &ch->base.rionics;
	list_for_each_entry(rionic, rionics, admin_link) {
		lionics = &rionic->lionics;
		list_for_each_entry(lionic, lionics, rionic_link) {
			/*
			 * Wait release-wq No-RDDA channels
			 */
			for (i = 0; i < lionic->n_nr_qps; ++i) {
				nrch = lionic->nr_channels + i;
				_NT(trace_1_ib_admin_channel_wait_io_channels,
					"Waiting for NR ch=@CH_PTR (net=@NET) to stop",
					nrch, &nrch->net);
				nvmeibc_ib_nordda_channel_clear_rq(nrch, true);
				BUG_ON(!nvmeibc_disk_ioch_drained_is_empty(&nrch->base));
			}
		}
	}
	NFOUT;
}

void nvmeibc_ib_admin_channel_free(struct nvmeibc_ib_admin_channel *ch)
{
	int n_freed = atomic_inc_return(&ch->base.base.n_freed);
	__NFIN;

	_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_free, "Free admin-ch (@CH_PTR) of disk @DISK_NAME called from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		ch, ch->base.base.disk->name, __builtin_return_address(0));

	if (n_freed != 1) {
		_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_free, "already called on admin-ch (@CH_PTR) @N_FREED times", ch, n_freed);
		return;
	}

	/* Wait for admin-ch-disconnect work which waits for the
	   secondary channels remove-works if they run on ad-hoc WQs */
	if (ch->base.remove_wq) {
		_NT(trace_2_ib_admin_channel_nvmeibc_ib_admin_channel_free, "Draining admin-wq (@CH_PTR) of disk @DISK_NAME", ch, ch->base.base.disk->name);
		wq_drain(ch->base.remove_wq);
	}

	/* free net after all works that may use it for post-recv */
	/* stop connection if not already stopped.  this can happened
	   if the triggering disconnect called from a cm interrupt
	*/
	nvmeibc_ib_net_admin_free(&ch->net);

	freee(ch);
	_NT(trace_3_ib_admin_channel_nvmeibc_ib_admin_channel_free, "Free admin-ch (@CH_PTR) of disk @DISK_NAME - Done", ch, ch->base.base.disk->name);
	__NFOUT;
}

static void handle_qp_err(u64 wr_id, enum ib_wc_status wc_status,
	bool send_err, struct nvmeibc_ib_admin_channel *ch)
{
	int op_code = nvmeib_opcode_from_wr_id(wr_id);

	__NFIN;
	_NT(trace_ib_admin_channel_handle_qp_err, "status=@STATUS_STR, op_code=@OP_CODE", nvmeib_status_str(&wc_status), op_code);
	if (op_code == NVMEIB_TOMA_REQ) {
		_NT(trace_1_ib_admin_channel_handle_qp_err, "NVMEIB_TOMA_REQ failed with status @WC_STATUS", wc_status);
	}
	else if (op_code == NVMEIB_KEEP_ALIVE) {
		_NT(trace_2_ib_admin_channel_handle_qp_err, "NVMEIB_KEEP_ALIVE failed with status @WC_STATUS", wc_status);
	} else if ((u32)op_code == NVMEIB_LOCAL_INV_WR_ID)
		_NT(trace_3_ib_admin_channel_handle_qp_err, "LOCAL_INV failed with status @WC_STATUS", wc_status);
	else if ((u32)op_code == NVMEIB_FAST_REG_WR_ID)
		_NT(trace_4_ib_admin_channel_handle_qp_err, "FAST_REG_MR failed status @WC_STATUS", wc_status);
	else {
		if (op_code == NVMEIB_SEND_IO ||
			(op_code >= NVMEIB_RDMA_MID_0 && op_code <= NVMEIB_RDMA_MID_MAX) || op_code == NVMEIB_RDMA_LAST) {
			_NE(error_ib_admin_channel_handle_qp_err, "Unexpected IO opcode @OP_CODE on admin channel", op_code);
			BUG();
		}
		else
			_NT(trace_5_ib_admin_channel_handle_qp_err, "Failed @DIRECTION_STR status @WC_STATUS", send_err ? "send" : "receive",
			wc_status);
	}
	/* let the upper layer block we are having an issue here
	   and disconnect from controller.
	*/
	__NFOUT;
}

/*
 * Must be called with ch->lock held to protect free_tx.
 */
static void put_tx_iu_nolock(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeib_iu *iu)
{
	__NFIN;
	iu->priv = NULL;
	if (unlikely(iu->io_done)) {
		list_add(&iu->free_tx_n, &ch->uncomp_tx);
		_NE(trace_put_tx_iu_nolock,
			"Admin-ch @BASE_NAME (@CH_PTR), unexpected iu=@PTR add",
			ch->base.base.name, ch, iu);
	} else {
		list_add(&iu->free_tx_n, &ch->free_tx);
	}
	__NFOUT;
}

void nvmeibc_ib_admin_channel_put_tx_iu(struct nvmeibc_ib_admin_channel *ch,
										struct nvmeib_iu *iu)
{
	unsigned long flags;

	__NFIN;
	spin_lock_irqsave(&ch->guard, flags);
	put_tx_iu_nolock(ch, iu);
	spin_unlock_irqrestore(&ch->guard, flags);
	__NFOUT;
}

/*
 * If IU is not sent, it must be returned using nvmeibc_put_tx_iu().
 */
struct nvmeib_iu* nvmeibc_ib_admin_channel_get_tx_iu(
	struct nvmeibc_ib_admin_channel *ch)
{
	struct nvmeib_iu *iu = NULL;
	unsigned long flags;

	__NFIN;
	spin_lock_irqsave(&ch->guard, flags);
	if (list_empty(&ch->free_tx))
		goto out;

	iu = list_first_entry(&ch->free_tx, struct nvmeib_iu, free_tx_n);
	list_del_init(&iu->free_tx_n);

out:
	spin_unlock_irqrestore(&ch->guard, flags);

	__NFOUT;
	return iu;
}

struct nvmeib_iu* nvmeibc_ib_admin_channel_get_rx_iu(struct nvmeibc_ib_admin_channel *ch, int index)
{
	if (ch->net.base.srq_info)
		return nvmeib_srq_rtrv_recv(ch->net.base.srq_info, index, &ch->net.base);
	else if (ch->recv_q)
		return nvmeib_rq_rtrv_recv(ch->recv_q, index);
	else
		return NULL;
}

int nvmeibc_ib_admin_channel_put_rx_iu(struct nvmeibc_ib_admin_channel *ch, struct nvmeib_iu *iu)
{
	struct nvmeibc_ib_net *net = &ch->net.base;
	int rv = 0;
	if (ch->net.base.srq_info) {
		if ((rv = nvmeib_srq_post_recv(ch->net.base.srq_info, iu)))
			_NT(error_ib_admin_channel_nvmeibc_ib_admin_channel_put_rx_iu, "Post received iu failed with error code @RV", rv);
	}
	else if (ch->recv_q)
		rv = nvmeibc_ib_net_post_recvq(net, ch->recv_q, iu);
	else
		rv = -EINVAL;
	return rv;
}

static inline int ach_send_done_on_init(struct nvmeib_iu *iu,
										struct nvmeibc_ib_admin_channel *ch)
{
	struct volume_client_req *req = iu->buf;
	int rv = -1;

	if (req->hdr.opcode != NVMEIB_CONFIG &&
		!on_wq(ch->base.remove_wq)) {
		_NE(trace_ach_send_done_on_init_0,
			"wrong wq, iu->opcode=@OPCODE", (int)iu->opcode);
		WARN_ON_ONCE(1);
		goto out;
	}

	/* first reinit comp then check dying;
	   correponds to admin's net-disconnect
	   which first set dying and then complete's comp */
	reinit_completion(&ch->send_done);
	if (atomic_read(&ch->net.base.dying)) {
		_NT(trace_ach_send_done_on_init_1, "Net is dying, don't send msg");
		goto out;
	}

	/* if net-disconnects here, comp item may be completed, the
	   upcoming wait-for-comp may imm return w/o real send-comp,
	   which means sender will get false-positive for send-comp.
	   Thus sender must check if net !dying after wait-for-comp */
	iu->io_done = &ch->send_done;
	rv = 0;

out:
	return rv;
}

static inline void ach_send_done_on_cancel(struct nvmeib_iu *iu,
										   struct nvmeibc_ib_admin_channel *ch)
{
	/* net-disconnect should have already run to prevent
	   send-comp from running and racing with this nullify */
	BUG_ON(!atomic_read(&ch->net.base.dying));
	iu->io_done = NULL;
}

static inline void ach_send_done_on_send_comp(struct nvmeib_iu *iu,
											  struct nvmeibc_ib_admin_channel *ch)
{
	unsigned long flags = 0;
	bool should_lock = nvmeibc_use_pcpu_cq &&
		!nvmeibc_channel_already_locked(&ch->base.base);

	if (should_lock)
		nvmeibc_channel_spin_lock_irqsave(&ch->base.base, &flags);

	/* prevent race with ach_send_done-on_dying */
	BUG_ON(!nvmeibc_channel_already_locked(&ch->base.base));

	if (iu->io_done == NULL ||
		iu->io_done != &ch->send_done) {
		_NE(ach_send_done_on_send_comp,
			"Wrong completion io_done=@PTR, send_done=@PTR admin-ch:@BASE_NAME",
			iu->io_done, &ch->send_done, ch->base.base.name);
	}
	else {
		complete(&ch->send_done);
		iu->io_done = NULL;
	}

	if (should_lock)
		nvmeibc_channel_spin_unlock_irqrestore(&ch->base.base, flags);
}

static inline void ach_send_done_on_dying(struct nvmeibc_ib_admin_channel *ch)
{
	unsigned long flags = 0;
	bool should_lock = nvmeibc_use_pcpu_cq &&
		!nvmeibc_channel_already_locked(&ch->base.base);

	if (should_lock)
		nvmeibc_channel_spin_lock_irqsave(&ch->base.base, &flags);

	/* prevent race with ach_send_done_on-send_comp*/
	BUG_ON(!nvmeibc_channel_already_locked(&ch->base.base));
	BUG_ON(!atomic_read(&ch->net.base.dying));

	complete(&ch->send_done);

	if (should_lock)
		nvmeibc_channel_spin_unlock_irqrestore(&ch->base.base, flags);
}

static void send_completion_io_done_complete(struct nvmeibc_ib_admin_channel *ch,
											 int index, enum ib_wc_status status)
{
	struct nvmeib_iu *iu;

	iu = ch->tx_ring[index];
	iu->io_status = status;
	if (iu->io_done)
		ach_send_done_on_send_comp(iu, ch);
}

static void keep_alive_send_comp(struct nvmeibc_ib_admin_channel *ch);

static int send_completion(struct nvmeibc_ib_net *net, struct ib_wc *wc,
	bool last_in_series)
{
	struct nvmeibc_ib_admin_channel *ch = in_to_iac(net);
	int index;
	enum nvmeib_wr_opcode opcode;
	int rv = -2;

	// __NFIN;
	opcode = nvmeib_opcode_from_wc(wc);
	index = nvmeib_idx_from_wc(wc);

	_ND(trace_ib_admin_channel_send_completion, "@TRUE_FALSE_STR (@STATUS) send-comp opcode=@OPCODE_STR (#@OPCODE), iu=@INDEX",
		wc->status == IB_WC_SUCCESS ? "successful" : "failed",
		wc->status, nvmeib_wr_opcode_str(opcode), opcode, index);

	switch (opcode) {
	case NVMEIB_SEND_IO:
		_NE(error_ib_admin_channel_send_completion, "Unexpected IO send-comp on admin channel");
		BUG();
		break;
	case NVMEIB_TOMA_SEND_RSP:
		/* [NVMESH-1153]: We are now sending this from interrupt context, so no wait on io_done */
		_ND(trace_2_ib_admin_channel_send_completion,
		    "send completion of TOMA response for tag ending with @TAG", index);
		break;
	case NVMEIB_SEND_CFG:
	case NVMEIB_TOMA_SEND_REQ:
	case NVMEIB_RDMA_GET_JMDC_REQ:
	case NVMEIB_WR_DBG_CMD:
		_ND(trace_1_ib_admin_channel_send_completion, "send completion of @NVMEIB_WR_OPCODE_STR", nvmeib_wr_opcode_str(opcode));
		send_completion_io_done_complete(ch, index, wc->status);
		break;
	case NVMEIB_KEEP_ALIVE_REQ:
		keep_alive_send_comp(ch);
		break;
	default:
		break;
	}
	rv = wc->status == IB_WC_SUCCESS ? 0 : -EIO;

	// __NFOUT;
	return rv;
}

static int send_msg(struct nvmeibc_ib_admin_channel *ch, struct nvmeib_iu *iu,
					int len, int wr_opcode)
{
	struct ib_sge list;
	struct ib_send_wr wr = {0};
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;

	__NFIN;

	if (iu->io_done)
		nvmeibc_ib_net_req_notify_send_cq(&ch->net.base);

	list.addr = iu->dma;
	list.length = len;
	list.lkey = ch->net.base.lkey;

	wr.opcode = IB_WR_SEND;
	wr.wr_id = nvmeib_encode_wr_id(wr_opcode, iu->index);
	wr.sg_list = &list;
	wr.num_sge = 1;
	wr.send_flags = IB_SEND_SIGNALED;

#if ENABLE_SIW
	if (P2NV(ch->net.base.port)->dev_type == DT_siw) {
		if (ch->net.base.shared_cq)
			wr.send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
		else
			wr.send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

	_ND(trace_ib_admin_channel_send_msg, "Sending context @INDEX, op @WR_OPCODE", iu->index, wr_opcode);
	rv = nvmeibc_ib_post_send(&ch->net.base, &wr, &bad_wr);
	__NFOUT;
	return rv;
}

static int process_rsp(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeib_iu *iu)
{
	struct volume_server_rsp *rsp = iu->buf;
	unsigned long flags;
	int rv;

	_ND(trace_ib_admin_channel_process_rsp, "Received srv response, opcode=@OPCODE", rsp->opcode);

	switch (rsp->opcode) {

	case NVMEIBS_RSP_TOMA_OPCODE_OK:
	case NVMEIBS_RSP_TOMA_OPCODE_ERR:
	case NVMEIBS_RSP_MGMT_OPCODE_OK:
	case NVMEIBS_RSP_MGMT_OPCODE_ERR:
		spin_lock_irqsave(&ch->guard, flags);
		list_add_tail(&iu->free_tx_n, &ch->recv_ioctx);
		spin_unlock_irqrestore(&ch->guard, flags);
		wake_up(&ch->base.base.wqh);
		rv = 0; /* Dont put back recv iu, requester shall do it */
		break;

	case NVMEIBS_RSP_IO_OPCODE_OK:
	case NVMEIBS_RSP_IO_OPCODE_ERR:
		_NE(error_ib_admin_channel_process_rsp, "Unexpected IO recv-comp on admin channel");
		rv = 1; /* put back recv iu */
		break;

	default:
		_NT(trace_1_ib_admin_channel_process_rsp, "Invalid server response type (@OPCODE)", rsp->opcode);
		rv = 1; /* put back recv iu */
		break;
	}

	return rv;
}

static void prepare_rsp_msg(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	struct nvmeib_iu *send_ioctx)
{
	struct volume_client_rsp *rsp;

	__NFIN;
	/* JH IOMMU: Changed to DMA_TO_DEVICE. dir must be same as for map/unmap */
	ib_dma_sync_single_for_device(P2IB(ch->net.base.port), send_ioctx->dma,
		NVMEIBC_RSP_SIZE, DMA_TO_DEVICE);
	rsp = send_ioctx->buf;
	memset(rsp, 0, sizeof*rsp);
	rsp->hdr.opcode = NVMEIB_RSP;
	rsp->hdr.tag = tag;
	__NFOUT;
}

static int send_rsp_msg(struct nvmeibc_ib_admin_channel *ch, u8 opcode,
						struct nvmeib_iu *send_ioctx, void *p, int len,
						int wr_opcode)
{
	struct volume_client_rsp *rsp = send_ioctx->buf;
	int rv;

	__NFIN;
	rsp->opcode = opcode;
	/* copy extra bytes - if needed */
	if (opcode != NVMEIBC_RSP_OPCODE_ERR && p && len)
		vex_memcpy(rsp->bytes, p, len);

	if ((rv =
		 send_msg(ch, send_ioctx, NVMEIBC_RSP_SIZE, wr_opcode)))
		_NT(trace_ib_admin_channel_send_rsp_msg, "Send response to client failed");
	__NFOUT;

	return rv;
}

static int send_rsp(struct nvmeibc_ib_admin_channel *ch, u8 opcode, u64 tag,
					 struct nvmeib_iu *send_ioctx, void *p, int len,
					 int wr_opcode)
{
	int rv;

	__NFIN;
	prepare_rsp_msg(ch, tag, send_ioctx);
	rv = send_rsp_msg(ch, opcode, send_ioctx, p, len, wr_opcode);
	__NFOUT;

	return rv;
}

int nvmeibc_ib_admin_send_rsp(struct nvmeibc_ib_admin_channel *ch,
			      u64 hdr_tag, struct volume_client_rsp *rsp,
			      int rsp_opcode, int wr_opcode, int rsp_len)
{
	struct nvmeib_iu *send_ioctx = NULL;
	int rv;
	__NFIN;

	send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch);
	if (!send_ioctx) {
		_NT(trace_ib_admin_channel_nvmeibc_ib_admin_send_rsp, "No free messages for administrator to use");
		rv = -ENOMEM;
		goto err;
	}

	send_ioctx->opcode = NVMEIB_RSP;
	if (ach_send_done_on_init(send_ioctx, ch) < 0) {
		_NT(trace2_ib_admin_channel_send_toma_cmd_work, "net is dying - not sending rsp");
		rv = -1;
		goto err;
	}

	rv = send_rsp(ch, rsp_opcode, hdr_tag, send_ioctx, rsp->bytes, rsp_len, wr_opcode);
	if (rv) {
		_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_send_rsp, "Failed to send response message");
		goto err;
	}

	rv = wait_for_completion_interruptible_timeout(&ch->send_done, NVMEIB_WAIT_FOR_ADMIN_SEND_COMP);
	if (rv <= 0) {
		_NT(error_1_ib_admin_channel_nvmeibc_ib_admin_send_rsp,
			"Fail to wait on sending reply to controller");
		rv == 0 ? rv = -ETIMEDOUT : rv;
		goto err;
	}
	else if (atomic_read(&ch->net.base.dying)) {
		_NT(error_2_ib_admin_channel_nvmeibc_ib_admin_send_rsp,
			"net is dying, ignore comp");
		rv = -1;
		goto cancel;
	}

	rv = 0;
	goto put;

err:
	nvmeibc_ib_net_disconnect(&ch->net.base);
	if (!send_ioctx)
		goto out;

cancel:
	ach_send_done_on_cancel(send_ioctx, ch);

put:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, send_ioctx);

out:
	__NFOUT;
	return rv;

}

static void handle_req_(struct nvmeibc_ib_admin_channel *ch, struct nvmeib_iu *iu)
{
	struct volume_server_req *req = iu->buf;
	struct volume_client_rsp *rsp = NULL;
	int rsp_len = sizeof(*rsp) - offsetof(struct volume_client_rsp, bytes);
	struct nvmeibc_disk *disk = ch->base.base.disk;
	int rsp_opcode;
	int wr_opcode;
	int rv = -1;

	__NFIN;

	_ND(trace_0_ib_admin_channel_handle_req, "--- Start handling @MSG_OP_TO_STR message from controller @BASE_NAME",
	   msg_op_to_str(req->hdr.opcode), ch->base.base.name);

	rsp = kmalloc(sizeof(*rsp), GFP_ATOMIC);
	if (!rsp) {
		//super unlikely
		if (!((req->hdr.opcode == NVMEIB_CMD) &&
			  (req->opcode == NVMEIBS_JAM_ABND2FREE ||
			   req->opcode == NVMEIBS_RGID_CHANGE ||
			   req->opcode == NVMEIBS_IOCH_DRAINED)))
			goto out;
	}

	if (req->hdr.opcode == NVMEIB_CMD) {
		int dying = atomic_read(&disk->dying);
		_NT(trace_ib_admin_channel_handle_req_, "@BASE_NAME, NVMEIBS_CMD opcode=@CMD_OPS_TO_STR (dying=@INT)",
			ch->base.base.name, nvmeibs_cmd_ops_to_str(req->opcode), dying);

		if (req->opcode == NVMEIBS_PUT_RSC || req->opcode == NVMEIBS_GET_RSC) {
			/* find the relevant disk */
			if (!memcmp(req->g_req.disk_name, disk->name,
						sizeof(disk->name)) && !dying) {
				_ND(trace_1_ib_admin_channel_handle_req, "req on disk @DISK_NAME", disk->name);
				rv = nvmeibc_disk_handle_controller_req(disk, req, rsp, &rsp_len);
			}
		} else if (req->opcode == NVMEIBS_JAM_ABND2FREE) {
			if (!memcmp(req->j_req.base.disk_name, disk->name,
				sizeof(disk->name)) && !dying) {
				_NT(trace_2_ib_admin_channel_handle_req, "jam abnd2free on disk @DISK_NAME", disk->name);
				/* Moving the request to the disk work queue */
				rv = nvmeibc_jam_process_recv_comp(disk, req);
			}
			goto out; /* no rsp */
		}
		else if (req->opcode == NVMEIBS_RGID_CHANGE) {
			if (!memcmp(req->r_req.disk_name, disk->name,
				sizeof(disk->name)) && !dying) {
				_NT(trace_3_ib_admin_channel_handle_req, "rgid change on disk @DISK_NAME", disk->name);
				rv = nvmeibc_disk_handle_rgid_change(disk, ch, req);
			}
			goto out; /* no rsp */
		}
		else if (req->opcode == NVMEIBS_IOCH_DRAINED) {
			if (!memcmp(req->r_req.disk_name, disk->name,
				sizeof(disk->name)) && !dying) {
				_NT(trace_7_ib_admin_channel_handle_req, "ioch drained disk @DISK_NAME", disk->name);
				rv = nvmeibc_disk_handle_ioch_drained(disk, ch, req);
			}
			goto out;  /* no rsp */
		} else if (req->opcode == NVMEIBS_LOGOUT) {
			/* just sending the response */
			rv = 0;
			rsp_opcode = NVMEIBC_RSP_LOGOUT_OK;
			wr_opcode = NVMEIB_SEND_CFG;
			goto send_rsp;
		}
		rsp_opcode = !rv ? NVMEIBC_RSP_OPCODE_OK : NVMEIBC_RSP_OPCODE_ERR;
		wr_opcode = NVMEIB_SEND_CFG;
	}
	else if (req->hdr.opcode == NVMEIB_TOMA_REQ) {
		/* [NVMESH-1153]: Moved to handle_recv */
		BUG();
	}
	else{
		_NT(trace_5_ib_admin_channel_handle_req, "Unknown opcode (@OPCODE)", req->hdr.opcode);
		goto out;
	}
send_rsp:
	nvmeibc_ib_admin_send_rsp(ch, req->hdr.tag, rsp, rsp_opcode, wr_opcode, rsp_len);

out:
	kfree(rsp);
	_ND(trace_6_ib_admin_channel_handle_req, "--- Finish handling @MSG_OP_TO_STR message from controller @BASE_NAME",
	   msg_op_to_str(req->hdr.opcode), ch->base.base.name);
	__NFOUT;
}

static void handle_req(struct workqe_struct *work)
{
	struct admin_cmd_workq *acwork =
		container_of(work, struct admin_cmd_workq, work);
	struct nvmeibc_ib_admin_channel *ch = acwork->ch;
	struct nvmeib_iu *iu = acwork->iu;
	/* dying is only set while running in volume work queue thread */
	int dying = atomic_read(&ch->base.base.dying);
	int rv;

	__NFIN;
	/* we are running in the volume work queue thread */
	/* if we are gone just leave */
	if (dying) {
		_ND(trace_ib_admin_channel_handle_req, " We are history - no need to process any controller requests");
		goto out;
	}
	/* do the real work */
	handle_req_(ch, iu);

out:
	/* return the receive buffer */
	if ((rv = nvmeibc_ib_admin_channel_put_rx_iu(ch, iu)))
		_NT(error_ib_admin_channel_handle_req, "Posr received iu failed with error code @RV", rv);
	kfree(acwork);
	__NFOUT;
}

static void handle_peer_logout_on_recv(struct nvmeibc_ib_admin_channel *ch,
									   struct volume_server_req const *req) {
	struct nvmeibc_disk *disk = ch->base.base.disk;
	enum nvmeibc_disk_release_reason reason;

	reason = LOGOUT_REASON_TO_RELEASE_REASON(req->logout_req.reason);
	nvmeib_trend_insert(&disk->peer_release_reason_trend, reason);
	ch->logout = true;

	_NT(trace_handle_peer_logout_on_recv,
			"DTREND: @DISK_NAME (@DISK) got LOGOUT with opcode: @LOGOUT_REASON, update peer_release_reason to @DISK_RELEASE_OP",
			disk->name, disk, req->logout_req.reason, reason);
}

static int process_req(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeib_iu *iu)
{
	struct admin_cmd_workq *work;
	/* by default return the iu to the shared receive queue */
	int rv;
	struct volume_server_req *req;

	__NFIN;

	req = iu->buf;
	/* fast path for logout msg */
	if (req->hdr.opcode == NVMEIB_CMD && req->opcode == NVMEIBS_LOGOUT) {
		handle_peer_logout_on_recv(ch, req);
	}

	/* rest */
	if ((work = kzalloc(sizeof(*work), GFP_ATOMIC))) {
		WQ_INIT_WORK(&work->work, handle_req);
		work->ch = ch;
		work->iu = iu;
		nvmeibc_admin_channel_add_work(&ch->base, &work->work);
		/* we must keep the iu till we finish with the request */
		rv = 0;
	}
	else {
		_NE(error_ib_admin_channel_process_req, "Fail to start handler to process controller command");
		/* error path so return the iu */
		rv = 1;
	}

	__NFOUT;
	return rv;
}

static void keep_alive_process_req(struct nvmeibc_ib_admin_channel *ch,
	u64 ka_id);

static int process_toma_req(struct nvmeibc_ib_admin_channel *ch, struct nvmeib_iu *iu)
{
	struct volume_server_req *req = iu->buf;
	nvmeibc_toma_recv_req(ch, req, iu->buf + iu->size);
	return 1;
}

static void handle_recv(struct nvmeibc_ib_admin_channel *ch, struct ib_wc *wc)
{
	struct nvmeib_dev *dev = P2NV(ch->net.base.port);
	struct ib_device *ibdev = dev->ib_dev;
	int index = nvmeib_idx_from_wc(wc);
	struct nvmeib_iu *iu = nvmeibc_ib_admin_channel_get_rx_iu(ch, index);
	struct nvmeib_hdr *hdr;
	int put_back = 1;
	unsigned long t = jiffies;
	int rv;
	u32 opcode __attribute__((unused));

	if (!iu) {
		_NE(error_ib_admin_channel_handle_recv, "Admin channel @CH_PTR got receive with NULL iu", ch);
		return;
	}

	/* JH IOMMU: Changed to DMA_FROM_DEVICE. dir must be same as map/unmap */
	ib_dma_sync_single_for_cpu(ibdev, iu->dma, NVMEIBS_MAX_ADMIN_MSG_SIZE,
		DMA_FROM_DEVICE);

	hdr = (struct nvmeib_hdr *)iu->buf;

	switch (hdr->opcode) {
	case NVMEIB_RSP:
		put_back = process_rsp(ch, iu);
		break;
	case NVMEIB_CMD:
		put_back = process_req(ch, iu);
		break;
	case NVMEIB_TOMA_REQ:
		put_back = process_toma_req(ch, iu);
		break;

	case NVMEIB_H_LOGOUT:
		_ND(trace_ib_admin_channel_handle_recv, "Got target logout request");
		break;
	case NVMEIB_KEEP_ALIVE:
		nvmeibc_admin_channel_exec_periodic(&ch->base, t);
		keep_alive_process_req(ch, hdr->tag);
		put_back = true;
		break;
	case NVMEIB_DBG_CMD:

	default:
		opcode = nvmeib_opcode_from_wc(wc);
		_NT(trace_1_ib_admin_channel_handle_recv, "Unhandled client opcode @OPCODE wc_opcode=@WC_OPCODE opcode=@OPCODE_LLONG data=@DATA_INT",
			hdr->opcode, wc->opcode, nvmeib_wr_id_from_wc(wc), be32_to_cpu(wc->ex.imm_data));
		break;
	}

	if (put_back) {
		if ((rv = nvmeibc_ib_admin_channel_put_rx_iu(ch, iu)))
			_NT(error_1_ib_admin_channel_handle_recv, "Post received iu failed with error code @RV", rv);
	}
}

static int recv_completion(struct nvmeibc_ib_net *net,
	struct ib_wc *wc)
{
	struct nvmeibc_ib_admin_channel *ch = in_to_iac(net);
	struct nvmeib_iu *iu;
	int index;
	int rv = 1;

	if (likely(wc->status == IB_WC_SUCCESS)) {
		handle_recv(ch, wc);
	}
	else {
		if (wc->status != IB_WC_WR_FLUSH_ERR || !atomic_read(&net->dying)) {
			_NT(trace_ib_admin_channel_recv_completion, "Admin channel received competion with status @NVMEIB_STATUS_STR. net @NET rq_post_count @ATOMIC_READ srq @SRQ_INFO",
				nvmeib_status_str(&wc->status), net, atomic_read(&net->rq_post_count), net->srq_info);
			handle_qp_err(nvmeib_wr_id_from_wc(wc), wc->status, false, ch);
			if (ch->net.base.srq_info) {
				index = nvmeib_idx_from_wc(wc);
				iu = nvmeib_srq_rtrv_recv(ch->net.base.srq_info, index, net);
				nvmeib_srq_post_recv(ch->net.base.srq_info, iu);
			}
			rv = -1;
		}
	}

	return rv;
}

static void free_iu_bufs(struct nvmeibc_ib_net *net)
{
	struct nvmeibc_ib_admin_channel *ch = in_to_iac(net);
	struct nvmeib_iu *iu, *tmp;

	__NFIN;
	list_for_each_entry_safe(iu, tmp, &ch->uncomp_tx, free_tx_n) {
		//omril: add check - this is not expected but is happens io_done can be the channel's comp
		_NE(trace_free_iu_bufs,
			"Admin-ch @BASE_NAME (@CH_PTR), unexpected iu=@PTR found",
			ch->base.base.name, ch, iu);
		iu->io_done = NULL;
		list_del_init(&iu->free_tx_n);
		list_add(&iu->free_tx_n, &ch->free_tx);
	}
	/* JH IOMMU: DMA_TO_DEVICE is correct, only used for source for Local RDMA_SEND */
	nvmeib_free_ioctx_ring(ch->tx_ring, P2IB(ch->net.base.port),
		ch->tx_ring_size, NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE, DMA_TO_DEVICE,
		&ch->free_tx);
	ch->tx_ring = NULL;
	__NFOUT;
}

/*
 * Note: the resources allocated in this function are freed in
 * nvmeibc_free_host_ib().
 */
static int alloc_iu_bufs(struct nvmeibc_ib_admin_channel *ch)
{
	int rv;

	__NFIN;

	/* free if previously allocated */
	free_iu_bufs(&ch->net.base);
	/* allocate */
	/* JH IOMMU: DMA_TO_DEVICE is correct, only used for source for Local RDMA_SEND */
	ch->tx_ring = nvmeib_alloc_ioctx_ring(P2IB(ch->net.base.port),
		ch->tx_ring_size, sizeof(*ch->tx_ring[0]),
		NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE, DMA_TO_DEVICE, &ch->free_tx, ch);
	if (!ch->tx_ring) {
		_NE(error_ib_admin_channel_alloc_iu_bufs, "OOM: cannot allocate tx_ring.");
		rv = -ENOMEM;
		goto out;
	}
	rv = 0;

out:
	__NFOUT;
	return rv;
}

static void on_disconnect_admin_ch(struct nvmeibc_ib_net *net)
{
	struct nvmeibc_ib_admin_channel *ch = in_to_iac(net);
	struct nvmeibc_disk *disk;
	int dying;

	NFIN;

	/* signal admin-wq that may be waiting for send-comp */
	ach_send_done_on_dying(ch);

	/* kill disk iff admin channel is the one that holds the disk */
	_NT(trace_ib_admin_channel_on_disconnect_admin_ch, "Admin-ch: attempt disk-release (n_disks @N_DISKS)", ch->base.n_disks);
	if (ch->base.n_disks && ch->base.arnic->alive) {
		if ((disk = ch->base.base.disk)){
			if (!(dying = atomic_read(&disk->dying))) {
				nvmeibc_disk_start_release(disk,
					ch->logout ?
					   NVMEIB_TREND_HEAD(disk->peer_release_reason_trend).data
					 : ch->release_reason? :
					 	 NVMEIBC_DISK_RELEASE_ADMIN_CHAN_DISCONNECT);
			}
			else {
				/* [AAA] PATCH: in case disk-release is running w/o setting
				   @restart_called, e.g. from update_disk_config_work(),
				   adding another work will cause endless disk-release */
				_NT(trace_0_on_disconnect_admin_ch,
					"Disk @DISK_NAME (@DISK), already dying (@INT)\n",
					disk->name, disk, dying);
			}
		}
	}

	NFOUT;
}

void nvmeibc_ib_admin_channel_jmdc_read_work(struct workqe_struct *work);

static void ach_set_link_ops(struct nvmeibc_ib_admin_channel *ach)
{
	union nvmeib_version link_version = ach->base.link_version;
	vex_link_ops(link_version,
		      vex_ach_clnt_ops_collection, ach->base.vex_ops,
		     vex_ach_names, vex_ach_ops_num);

	vex_link_ops(link_version,
		      vex_nrch_clnt_ops_collection, ach->base.vex_nrio_ops,
		     vex_nrch_names, vex_nrch_ops_num);
}

static int on_login_admin_ch(struct nvmeibc_ib_net *net,
	struct nvmeibs_login_response *lrsp, int n_ext)
{
	struct nvmeibc_ib_admin_channel *ch = in_to_iac(net);
	int max_server_iu_len;
	int rv = 0;

	__NFIN;
	if (nvmeib_wire_op_cid_get_rsp_opcode(&lrsp->base.op_cid) == NVMEIB_LOGIN_RSP) {
		_NT(trace_on_login_admin_ch_cid,
			"Received admin login-rsp, assigned cid=@CID n_ext=@INT",
			nvmeib_wire_op_cid_get_cid(&lrsp->base.op_cid), n_ext);
		snprintf(ch->base.base.rhost_name, sizeof(ch->base.base.rhost_name),
			"%.*s", (int)sizeof(lrsp->base.creq.host_name), lrsp->base.creq.host_name);
		snprintf(ch->base.base.name, sizeof(ch->base.base.name),
			"%.*s-%.*s~A",
			(int)sizeof(lrsp->base.creq.host_name), lrsp->base.creq.host_name,
			NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE, ch->base.base.disk->name);

		/* [EC-5111] - Check to see if the target has been upgraded since last discover */
		if (n_ext >= 1) {
			union nvmeib_version tgt_version = { .all = be64_to_cpu(lrsp->ext1.ach.s_version) };
			if (ch->base.base.disk->last_tgt_ver.all && ch->base.base.disk->last_tgt_ver.all != tgt_version.all) {
				/* Target version has changed - update and rediscover */
				_NT(trace_ib_admin_channel_on_login_admin_ch_tgt_ver_chng,
					"Target: @REMOTE_HOSTNAME - version changed to " NVMEIB_VERSION_TRACE_FMT() "",
					lrsp->base.creq.host_name,  NVMEIB_VERSION_PRINT_ARG((&tgt_version)));

				ch->base.base.disk->last_tgt_ver = tgt_version;
				ch->base.base.disk->last_tgt_link_ver.all = 0;
				ARNIC_DISCOVER_STATUS(ch->base.arnic, NVMEIBC_ARNIC_DISCOVER_TARGET_VERSION_CHANGED);
				rv = -EAGAIN;
				goto out;
			}
			ch->base.base.disk->last_tgt_ver = tgt_version;
		}

		/* This could be write more efficent but it much more easier to read this way */
		ch->base.base.disk->last_tgt_ver.all = be64_to_cpu(lrsp->base.creq.s_link_version);
		ch->base.base.disk->tgt_num_cpus = NVMEIB_DFLT_NUM_CPUS;
		ch->base.base.disk->tgt_max_nrchs_per_path_rdma = NVMEIB_COMPAT_MAX_NR_CHANNELS_PER_PATH;

		if (n_ext >= 2) {
			ch->base.base.disk->tgt_num_cpus = lrsp->ext2.ach.tgt_num_cpus;
			ch->base.base.disk->tgt_max_nrchs_per_path_rdma = lrsp->ext2.ach.tgt_max_nrchs_per_path;
			ch->base.base.disk->tgt_max_nrchs_per_path_tcp = NVMEIB_COMPAT_MAX_NR_CHANNELS_PER_PATH;
		}

		if (n_ext >= 3) {
			ch->base.base.disk->tgt_max_nrchs_per_path_tcp = lrsp->ext3.ach.tgt_max_nrchs_per_path_tcp;
		}

		if (n_ext >= 4) {
			ch->base.base.disk->tgt_num_cpus = be32_to_cpu(lrsp->ext4.ach.tgt_num_cpus);
			ch->base.base.disk->tgt_max_nrchs_per_path_tcp = be32_to_cpu(lrsp->ext4.ach.tgt_max_nrchs_per_path_tcp);
		}

		ch->base.link_version.all = be64_to_cpu(lrsp->base.creq.s_link_version);
		ch->base.base.disk->last_tgt_link_ver = ch->base.link_version;

		_NT(trace_ib_admin_channel_on_login_admin_ch, "server rsp link version "
		    NVMEIB_VERSION_TRACE_FMT() "",
		    NVMEIB_VERSION_PRINT_ARG((&ch->base.link_version)));
		ach_set_link_ops(ch);

		max_server_iu_len = be32_to_cpu(lrsp->base.creq.max_iu_len);
		if (max_server_iu_len > NVMEIBS_MAX_ADMIN_MSG_SIZE) {
			_NT(trace_1_ib_admin_channel_on_login_admin_ch, "Server messages are too big - max allowed @NVMEIBS_MAX_ADMIN_MSG_SIZE, requested @MAX_SERVER_IU_LEN",
				NVMEIBS_MAX_ADMIN_MSG_SIZE, max_server_iu_len);
			rv = -EMSGSIZE;
		}
		else {
			/* save controller info */
			ch->base.cid = nvmeib_wire_op_cid_get_cid(&lrsp->base.op_cid);
			ch->cmsg_buffer_raddr = be64_to_cpu(lrsp->base.creq.msg_buffer_raddr);
			ch->cmsg_buffer_pages = be32_to_cpu(lrsp->base.creq.msg_buffer_pages);
			ch->cmsg_buffer_rkey = be32_to_cpu(lrsp->base.creq.msg_buffer_rkey);
			ch->cmsg_buffer_page_shift = n_ext >= 6 ? lrsp->ext6.ach.tgt_pg_sz_shift : PAGE_SHIFT;

			BUG_ON(ch->ka_sent);
			_ND(trace_2_ib_admin_channel_on_login_admin_ch, "Server accepted MGMT qp connection");
			rv = 0;
		}
	}
	else {
		_ND(trace_3_ib_admin_channel_on_login_admin_ch, "Unhandled RSP opcode @OPCODE", nvmeib_wire_op_cid_get_rsp_opcode(&lrsp->base.op_cid));
		rv = -ECONNABORTED;
	}

out:
	__NFOUT;
	return rv;
}

static void on_reject_admin_ch(struct nvmeibc_ib_net *net,
	struct nvmeibs_login_reject *lrej)
{
	struct nvmeibc_ib_admin_channel *ch = in_to_iac(net);
	ch->base.base.disk->last_tgt_link_ver.all = be64_to_cpu(lrej->s_version);
	_NT(trace_c_ib_admin_ch_on_reject,
		"REJECT from ADMIN_CH: @ACH on net @NET. Target Version: " NVMEIB_VERSION_TRACE_FMT() "",
		ch, net, NVMEIB_VERSION_PRINT_ARG(&ch->base.base.disk->last_tgt_link_ver));
}

static void keep_alive_send_comp(struct nvmeibc_ib_admin_channel *ch)
{
	unsigned long flags;
	__NFIN;

	spin_lock_irqsave(&ch->ka_spinlock, flags);
	if (!ch->ka_sent)
		_NE(error_ib_admin_channel_keep_alive_send_comp, "KA: received send-comp but did not sent");
	ch->ka_sent = false;
	spin_unlock_irqrestore(&ch->ka_spinlock, flags);

	__NFOUT;
}

static void keep_alive_send_rsp_(struct nvmeibc_ib_admin_channel *ch, u64 ka_id)
{
	struct ib_send_wr wr = {0};
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;
	struct ib_sge sge;

	if (!ch->ka_sent) {
		/* JH IOMMU: Changed to DMA_TO_DEVICE. Only used as source for Local RDMA_WRITE */
		ib_dma_sync_single_for_cpu(P2IB(ch->net.base.port),
			ch->ka_msg_dma_addr, sizeof(*ch->ka_msg_area), DMA_TO_DEVICE);
		ch->ka_msg_area->opcode = NVMEIB_KEEP_ALIVE;
		ch->ka_msg_area->tag = ka_id;
		sge.addr = ch->ka_msg_dma_addr;
		sge.length = sizeof(*ch->ka_msg_area);
		sge.lkey = ch->net.base.lkey;
		wr.opcode = IB_WR_SEND;
		wr.wr_id = nvmeib_encode_wr_id(NVMEIB_KEEP_ALIVE_REQ, 0);
		wr.send_flags = IB_SEND_SIGNALED;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		ib_dma_sync_single_for_device(P2IB(ch->net.base.port), ch->ka_msg_dma_addr,
				sizeof(*ch->ka_msg_area), DMA_TO_DEVICE);
		ch->ka_sent = true;

#if ENABLE_SIW
		if (P2NV(ch->net.base.port)->dev_type == DT_siw) {
			if (ch->net.base.shared_cq)
				wr.send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
			else
				wr.send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
		}
#endif

		if ((rv = nvmeibc_ib_post_send(&ch->net.base, &wr, &bad_wr)) < 0) {
			_NT(error_ib_admin_channel_keep_alive_send_rsp, "KA: send keep alive ended with error @RV", rv);
			ch->ka_sent = false;
		}
	}
	else
		/* Expected on every other KA req as send-comp intrs are not armmed
		   instead, send-comps are polled & processed by recv-comp handler */
		_ND(trace_ib_admin_channel_keep_alive_send_rsp, "KA: already sent rsp (no send-comp yet)");
}

static void keep_alive_process_req(struct nvmeibc_ib_admin_channel *ch,
	u64 ka_id)
{
	unsigned long flags;
	__NFIN;

	spin_lock_irqsave(&ch->ka_spinlock, flags);
	if (ch->ka_running) {
		mod_timer(&ch->ka_timer, jiffies + NVMEIB_KEEP_ALIVE_TO);
		ch->ka_start_time = jiffies;
		keep_alive_send_rsp_(ch, ka_id);
	}
	spin_unlock_irqrestore(&ch->ka_spinlock, flags);

	__NFOUT;
}

static void keep_alive_net_disconnect_work(struct workqe_struct *work)
{
	struct admin_ka_work *ka_work =
		container_of(work, struct admin_ka_work, work);
	struct nvmeibc_ib_admin_channel *ch = ka_work->ch;

	__NFIN;
	_NT(trace_ib_admin_channel_keep_alive_net_disconnect_work, "keep alive time out (@BASE_NAME), going to disconnect now",
		ch->base.base.name);
	ch->release_reason = NVMEIBC_DISK_RELEASE_ADMIN_CHAN_KA_FAILED;
	nvmeibc_ib_net_disconnect(&ch->net.base);
	kfree(ka_work);

	__NFOUT;
}




TIMER_CALLBACK(keep_alive_timer_handler, struct nvmeibc_ib_admin_channel, ka_timer, struct nvmeibc_ib_admin_channel, ch)
	struct admin_ka_work *work;
	unsigned long flags;

	__NFIN;
	spin_lock_irqsave(&ch->ka_spinlock, flags);
	if (!ch->ka_running)
		goto out;
	_NT(trace_ib_admin_channel_keep_alive_timer_handler, "KA: time out (@BASE_NAME, started @KA_START_TIME, expires @EXPIRES), add timeout work",
		ch->base.base.name, ch->ka_start_time, ch->ka_timer.expires);
	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		_NE(error_ib_admin_channel_keep_alive_timer_handler, "Cannot allocate admin ka work!!!!!!");
		goto out;
	}
	atomic_inc(&ch->base.base.disk->shut_down_triggered);
	WQ_INIT_WORK(&work->work, keep_alive_net_disconnect_work);
	work->ch = ch;
	nvmeibc_admin_channel_add_work(&ch->base, &work->work);


out:
	spin_unlock_irqrestore(&ch->ka_spinlock, flags);
	__NFOUT;
}

static void keep_alive_init(struct nvmeibc_ib_admin_channel *ch)
{
	__NFIN;
	spin_lock_init(&ch->ka_spinlock);
	__NFOUT;
}

static void keep_alive_start(struct nvmeibc_ib_admin_channel *ch)
{
	unsigned long flags;
	__NFIN;

	spin_lock_irqsave(&ch->ka_spinlock, flags);
	INIT_TIMER(&ch->ka_timer);
	ch->ka_timer.function = keep_alive_timer_handler;
	TIMER_SET_DATA(ch, ka_timer, (unsigned long)ch);
	ch->ka_timer.expires = jiffies + 2 * NVMEIB_KEEP_ALIVE_TO;
	ch->ka_running = true;
	ch->ka_start_time = jiffies;
	add_timer(&ch->ka_timer);
	spin_unlock_irqrestore(&ch->ka_spinlock, flags);

	__NFOUT;
}

static void keep_alive_stop(struct nvmeibc_ib_admin_channel *ch)
{
	bool wait;
	unsigned long flags;
	__NFIN;

	_NT(trace_ib_admin_channel_keep_alive_stop, "KA: stopping keep alive of @BASE_NAME", ch->base.base.name);
	spin_lock_irqsave(&ch->ka_spinlock, flags);
	wait = ch->ka_running;
	ch->ka_running = false;
	spin_unlock_irqrestore(&ch->ka_spinlock, flags);
	if (wait)
		del_timer_sync(&ch->ka_timer);
	_NT(trace_1_ib_admin_channel_keep_alive_stop, "KA: keep alive stopped");

	__NFOUT;
}

static int login(struct nvmeibc_ib_admin_channel *ch, bool access_local)
{
	struct nvmeibc_ib_net_params *params = NULL;
	struct nvmeibc_login_request req = {};
	char gid_buf[GUID_SIZE] = {0};
	int rv = -1;
	struct nvmeibc_disk *disk;

	__NFIN;
	if (!(params = kzalloc(sizeof(*params), GFP_KERNEL))) {
		_NE(error_ib_admin_channel_login, "OOM: fail to allocate net params");
		rv = -ENOMEM;
		goto out;
	}
	ch->base.base.numa_node = nvmeib_get_dev_numa_node(P2NV(ch->net.base.port));
	ch->tx_ring_size = NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_MSGS;
	ch->net.base.pkey = ch->net.base.port->pkey;
	ch->net.base.admin_ch = &ch->base;
	ch->net.base.ioch = &ch->base.base;
	ch->net.base.rearm_send_cq = true;
	params->max_send_q = ch->tx_ring_size;
	params->max_send_sg = NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_SG;
	/* when using the shared receive queue the following can be zero */
	params->max_recv_q = 0;
	params->max_recv_sg = 0;
	params->recv_msg_size = NVMEIBS_MAX_ADMIN_MSG_SIZE;
	params->max_send_cq = params->max_send_q;
	params->max_recv_cq =
		params->max_send_cq +           /* recv reply to my sends */
		NVMEIBS_SERVER_DEFAULT_SQ_SIZE; /* recv unsolicited srv msgs (PUT-RSC)*/
	params->use_srq = nvmeibc_support_srq(P2NV(ch->net.base.port));
	params->max_rd_atomic = NVMEIB_DFLT_MAX_READ_ATOM_ON_WIRE;
	params->max_dest_rd_atomic = NVMEIB_DFLT_MAX_READ_ATOM_ON_WIRE;
	params->srq_priv = NULL;
	params->srq_type = NVMEIB_SRQ_TYPE_PRIMARY;
	params->call_send_comp_handler = send_completion;
	params->call_receive_comp_handler = recv_completion;
	params->free_net_on_connect_err = false;
	params->on_connected = NULL;
	params->on_connected_clear = NULL;
	params->on_login = on_login_admin_ch;
	params->on_reject = on_reject_admin_ch;
	params->on_disconnect = on_disconnect_admin_ch;
	params->on_free = free_iu_bufs;
	params->rcq_offload_enb = false;
	params->scq_offload_enb = false;
	params->comp_cpu = NVMEIB_CPU_INVALID;
	params->vector_type = NVMEIB_CQ_VECTOR_GET_TYPE_ADMIN;

	if (!params->use_srq) {
		/* Running without SRQ - Init Channel RQ */
		if (!(ch->recv_q = kzalloc(sizeof(*ch->recv_q), GFP_KERNEL))) {
			rv = -ENOMEM;
			goto out;
		}

		_NT(trace_ib_admin_channel_login, "Initializing RQ of size @NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_MSGS for Admin channel @CH_PTR", NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_MSGS, ch);
		rv = nvmeib_init_recvq(ch->recv_q, P2NV(ch->net.base.port),
			NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_MSGS, NVMEIBS_MAX_ADMIN_MSG_SIZE);

		if (rv < 0) {
			kfree(ch->recv_q);
			goto out;
		}
		params->recv_q = ch->recv_q;
		params->max_recv_q = ch->recv_q->rq_queue_size;
		params->max_recv_sg = NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_SG;
	}

	/* fill login data */
	disk = ch->base.base.disk;
	if (disk->last_tgt_link_ver.all != 0 && disk->last_tgt_link_ver.all != nvmeib_version_get().all) {
		/* TBD [EC-4592] - Implement this using VEX infrastructure */
		if (disk->last_tgt_link_ver.protocol.all == nvmeib_13_version.protocol.all) {
			_NT(trace_c_ib_admin_channel_login_as_13,
				"Logging in as version: " NVMEIB_VERSION_TRACE_FMT() " to target: @GID_BUF max_iu_len: @MAX_SERVER_IU_LEN",
				NVMEIB_VERSION_PRINT_ARG(&disk->last_tgt_link_ver),
				gid_buf, NVMEIBC_13_MAX_ADMIN_CLIENT_MSG_SIZE);
			nvmeibc_login_req_init(&req, nvmeib_13_version, NVMEIBC_ADMIN_CHANNEL,
						0, access_local,
						ch->ranic_gid.global.subnet_prefix,
						ch->ranic_gid.global.interface_id);
			nvmeibc_login_req_set_ach(&req, nvmeib_13_version,
						access_local ? NVMEIBC_13_MAX_ADMIN_CLIENT_MSG_SIZE : 0,
						nvmeibc_get_uuid(nvmeibc_cinst_get_core_p(&ch->base.base)));
			nvmeibc_login_req_set_msg_hdr(&req, NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED,
						      NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED, 0, 0);
			goto alloc_net;
		}
		_NW(warn_c_ib_admin_channel_login_unknown_ver,
			"Target on @GID_BUF has unexpected last link version: @MAJOR.@MINOR.@SUBMINOR.@MR",
			gid_buf, disk->last_tgt_link_ver.protocol.major, disk->last_tgt_link_ver.protocol.minor,
			disk->last_tgt_link_ver.protocol.subminor, disk->last_tgt_link_ver.protocol.mr);
	}
	_NT(trace_c_ib_admin_channel_login,
		"Logging in to target: @GID_BUF (version: " NVMEIB_VERSION_TRACE_FMT()
		") max_iu_len: @MAX_SERVER_IU_LEN local: @BOOL_YN",
		gid_buf, NVMEIB_VERSION_PRINT_ARG(&disk->last_tgt_link_ver),
		NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE, access_local);
	nvmeibc_login_req_init(&req, disk->last_tgt_link_ver, NVMEIBC_ADMIN_CHANNEL,
				0, access_local,
				ch->ranic_gid.global.subnet_prefix,
				ch->ranic_gid.global.interface_id);
	nvmeibc_login_req_set_ach(&req, nvmeib_version_get(),
				access_local ? NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE : 0,
				nvmeibc_get_uuid(nvmeibc_cinst_get_core_p(&ch->base.base)));
	nvmeibc_login_req_set_msg_hdr(&req, NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED,
				      NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED, 0, 0);

alloc_net:
	format_gid(&ch->net.base.path.dgid, gid_buf);
	_ND(trace_3_ib_admin_channel_login, "--- Trying to connect to controler @GID_BUF with cid=@CID_LLONG",
		gid_buf, (u64)nvmeib_wire_op_cid_get_cid(&req.op_cid));

	if ((rv = nvmeibc_ib_net_admin_alloc(&ch->net, params, &req)) < 0) {
		_NT(trace_4_ib_admin_channel_login, "Failed to create admin channel net");
		goto out;
	}
	else
		_ND(trace_5_ib_admin_channel_login, "--- Connected to controller @GID_BUF", gid_buf);

	if ((rv = alloc_msg_area(ch)) < 0) {
		_NT(trace_7_ib_admin_channel_login, "Failed (@RV) to allocate msg area", rv);
		goto out;
	}

	if ((rv = alloc_iu_bufs(ch)) < 0) {
		_NT(trace_8_ib_admin_channel_login, "Failed (@RV) to allocate admin net iu bufs", rv);
		goto out;
	}

	_ND(trace_6_ib_admin_channel_login, "Starting keep alive");
	keep_alive_start(ch);
	goto out;

out:
	kfree(params);

	__NFOUT;
	return rv;
}

static struct nvmeib_iu* get_pending_iu_(struct nvmeibc_ib_admin_channel *ch)
{
	struct nvmeib_iu *iu;

	__NFIN;
	if (!list_empty(&ch->recv_ioctx)) {
		iu = list_first_entry(&ch->recv_ioctx, struct nvmeib_iu, free_tx_n);
		list_del_init(&iu->free_tx_n);
	}
	else iu = NULL;

	__NFOUT;
	return iu;
}

static struct nvmeib_iu* wait_pending_iu(struct nvmeibc_ib_admin_channel *ch,
	unsigned long timeout, u64 req_tag)
{
	struct nvmeib_iu *iu = NULL;
	struct volume_server_rsp *rsp;
	unsigned long flags;
	bool wait;
	long rv;
	unsigned long loops, i=0;

	__NFIN;

	loops = DIV_ROUND_UP(timeout, NVMEIB_WAIT_FOR_PENDING_IU_TIMEOUT);
	timeout = min(timeout, (unsigned long)NVMEIB_WAIT_FOR_PENDING_IU_TIMEOUT);

	spin_lock_irqsave(&ch->guard, flags);

	do {
		wait = false;
		i++;
		_ND(trace_wait_pending_iu_retry, "retry @INT/@INT", i, loops);
		if ((rv = wait_event_interruptible_lock_irq_timeout(ch->base.base.wqh,
			!list_empty(&ch->recv_ioctx), ch->guard, timeout)) > 0) {
			iu = get_pending_iu_(ch);
			rsp = iu->buf;
			if (rsp->hdr.tag < req_tag) {
				_NT(trace_ib_admin_channel_wait_pending_iu, "rsp err, obsolete tag (rsp @TAG, req @REQ_TAG), rsp->opcode = "
				   "@OPCODE - wait another timeout",
				   rsp->hdr.tag, req_tag, rsp->opcode);
				wait = true;
				nvmeibc_ib_admin_channel_put_rx_iu(ch, iu);
				iu = NULL;
			}
		}
		else {
			if (i < loops) {
				if (!ch->logout)
					wait = true;
				else {
					wait = false;
					_NT(trace_wait_pending_iu_chan_logout, "channel logout, breaking!");
				}
			}
			if (rv < 0)
				_NT(trace_1_ib_admin_channel_wait_pending_iu, "OOPS: interrupted @RV_LONG, retry @INT/@INT",
						rv, i, loops);
			else if(rv == 0)
				_NT(trace_2_ib_admin_channel_wait_pending_iu, "OOPS: Timeout, retry @INT/@INT,",
					i, loops);
		}
	} while (wait);

	spin_unlock_irqrestore(&ch->guard, flags);

	__NFOUT;
	return iu;
}

struct nvmeib_iu* nvmeibc_ib_admin_channel_pending_iu(
	struct nvmeibc_ib_admin_channel *ch, u64 req_tag)
{
	return wait_pending_iu(ch, NVMEIB_WAIT_FOR_ADMIN_SEND_COMP, req_tag);
}

static int send_rdma_msg(struct nvmeibc_ib_admin_channel *ch, size_t msg_sz)
{
	struct ib_sge list;
	struct nvmeib_send_wr wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);

	__NFIN;
	if (msg_sz > (ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift)) {
		_NT(ib_admin_ch_send_rdma_msg_inv_sz, "Invalid msg size @MSG_SIZE", (int)msg_sz);
		return -EINVAL;
	}
	nvmeib_mem_sync_map_for_cpu(&ch->msg_area_map, 0, msg_sz);
	list.addr = ch->msg_area_map.ioaddr;
	list.length = msg_sz;
	list.lkey = ch->msg_area_map.lkey;

	memset(&wr, 0, sizeof(wr));
	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_rdma(wr).remote_addr = ch->cmsg_buffer_raddr;
	nvmeib_send_wr_rdma(wr).rkey= ch->cmsg_buffer_rkey;
	nvmeib_send_wr_common(wr).sg_list = &list;
	nvmeib_send_wr_common(wr).num_sge = 1;

#if ENABLE_SIW
	if (P2NV(ch->net.base.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_MORE_WQES;
		if (ch->net.base.shared_cq)
			nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
		else
			nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

	__NFOUT;
	return nvmeibc_ib_post_send(&ch->net.base, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr);
}

int nvmeibc_ib_admin_channel_prepare_n_send_msg(
	struct nvmeibc_ib_admin_channel *ch,
	ssize_t (*f)(void *p, void *buf, const void *buf_end), void *p, int wr_opcode,
	struct nvmeib_iu *iu)
{
	int len, rv = -1;
	unsigned long timeout = NVMEIB_WAIT_FOR_ADMIN_SEND_COMP;

	__NFIN;
	/* prepare send context for cpu access */

	ib_dma_sync_single_for_cpu(P2IB(ch->net.base.port), iu->dma,
		NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE, DMA_BIDIRECTIONAL);
	/* add the message */
	if ((rv = f(p, iu->buf, iu->buf + iu->size)) < 0)
		goto out;
	len = rv;

	if (ach_send_done_on_init(iu, ch) < 0) {
		rv = -1;
		goto error;
	}

	/* prepare message for device */
	ib_dma_sync_single_for_device(P2IB(ch->net.base.port), iu->dma,
		NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE, DMA_BIDIRECTIONAL);
	/* send the message */
	TRACE_STAGING_MESSAGE("Sending message", ch->base.base.name, wr_opcode, f);
	if ((rv = send_msg(ch, iu, len, wr_opcode))) {
			_NT(trace_0_nvmeibc_ib_admin_channel_prepare_n_send_msg,
				"Failed to send message (@RV) on chnnel @BASE_NAME disk @DISK_NAME",
			rv, ch->base.base.name, ch->base.base.disk->name);
		goto error;
	}
	if (wr_opcode == NVMEIB_RDMA_GET_JMDC_REQ)
		timeout = NVMEIB_WAIT_FOR_GET_JMDC_TIMEOUT;

	TRACE_STAGING_MESSAGE("Waiting for completion of sending message", ch->base.base.name, wr_opcode, f);
	rv = wait_for_completion_interruptible_timeout(&ch->send_done, timeout);
	if (rv <= 0) {
		_NT(trace_1_nvmeibc_ib_admin_channel_prepare_n_send_msg,
			"Fail to wait for message send completion (rv=@RV)", rv);
		rv == 0 ? rv = -ETIMEDOUT : rv;
		goto error;
	}
	else if (atomic_read(&ch->net.base.dying)) {
		_NT(trace_2_nvmeibc_ib_admin_channel_prepare_n_send_msg,
			"net is dying, ignore comp");
		rv = -1;
		goto error;
	}

	rv = iu->io_status == IB_WC_SUCCESS ? 0 : -ECOMM;
	goto out;

error:
	nvmeibc_ib_net_disconnect(&ch->net.base);
	ach_send_done_on_cancel(iu, ch);

out:
	__NFOUT;
	if (!rv) {
		TRACE_STAGING_MESSAGE("Successfully sent message", ch->base.base.name, wr_opcode, f);
	} else {
		TRACE_STAGING_MESSAGE("Failed to send message", ch->base.base.name, wr_opcode, f);
	}
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_dbg_cmd_base_clnt_encode)
{
	struct nvmeibc_ib_admin_channel *ch = arg;
	struct volume_client_dbg_req_base *req = wire_buf;

	(void)ch;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*req) > wire_buf_end);
	req->version = cpu_to_be64(ops->vex_ext);
	req->death_wish.has_death_wish = cpu_to_be32(req->death_wish.has_death_wish);
	req->death_wish.rsc_id = cpu_to_be64(req->death_wish.rsc_id);
	req->death_wish.dlba = cpu_to_be64(req->death_wish.dlba);

	return sizeof(*req);
}


static ssize_t prp_dbg_please_kill_yourself(void *p, void *buf, const void *buf_end)
{
	struct nvmeibc_please_kill_yourself_args *args = p;
	struct nvmeibc_ib_admin_channel *ch = args->ch;
	struct volume_client_req *req;
	ssize_t rv = 0;

	__NFIN;
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_DBG_REQ_HDR_SIZE() > buf_end);
	/* init the command */
	req = buf;
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_DBG_CMD;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(vex_base);
	_ND(trace_ib_admin_channel_prp_dbg_please_kill_yourself, "req tag @TAG", req->hdr.tag);
	req->dbg_req.opcode = NVMEIBC_DBG_PLEASE_KILL_YOURSELF;
	req->dbg_req.pky_req.base.death_wish.has_death_wish = args->has_death_wish;
	req->dbg_req.pky_req.base.death_wish.rsc_id = args->rsc_id;
	req->dbg_req.pky_req.base.death_wish.dlba = args->dlba;

	if ((rv = CALL_VEX_OP(encode,
		vex_ach_dbg_cmd_clnt_ops, BASE_ONLY,
			base, vex_ach_dbg_cmd_base_clnt_encode,
				ch->base.vex_ops[vex_ach_dbg_cmd],
				NVMEIBC_VOLUME_CLIENT_DBG_REQ_PAYLOAD(req), buf_end, &ch)) < 0)
		goto out;

	rv += NVMEIBC_VOLUME_CLIENT_DBG_REQ_HDR_SIZE();

out:
	__NFOUT;

	return rv;
}

#define my_node_name(ch) nvmeibc_get_utsname_nodename(nvmeibc_isnt_params_core2main(nvmeibc_cinst_get_core_p(&ch->base.base)))
static ssize_t prp_ma_get_locks_mems(void *p, void *buf, const void *buf_end)
{
	struct nvmeibc_ib_admin_channel *ch = p;
	struct volume_client_req *req;
	struct volume_client_config_get_disk_locks *locks_req;
	ssize_t rv = NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(disk_locks);

	__NFIN;
	BUG_ON(buf + rv > buf_end);
	/* init the command */
	req = buf;
	memset(req, 0, sizeof(*req));
	locks_req = &req->config_req.disk_locks;
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(vex_base);
	_ND(trace_ib_admin_channel_prp_ma_get_locks_mems, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_GET_DISK_MEMS;
	snprintf(locks_req->client_name, sizeof(locks_req->client_name),
		"%s", my_node_name(ch));
	memcpy(locks_req->disk_name, ch->base.base.disk->name,
		sizeof(locks_req->disk_name));

	__NFOUT;
	return rv;
}

/* WAS: vex_shared_conf_base_encode */
VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_shared_cfg_clnt_base_encode)
{
	struct volume_client_config_share_base *conf = wire_buf;
	int i = 0;
	ssize_t rv;

	conf->version = cpu_to_be64(ops->vex_ext);

#define CNST_ELEM_ADD(_const) \
	do {\
		if (i >= NVMEIBC_CFG_SHARE_MAX_CONST || (const void *)(&conf->share_consts[i]) >= wire_buf_end) {\
			rv = -ENOSPC;\
			goto out;\
		}\
		nvmeib_strlcpy(conf->share_consts[i].const_name, #_const, sizeof(conf->share_consts[i].const_name)); \
		conf->share_consts[i].const_val = cpu_to_be64(_const);\
		i++; \
	} while(0)

	/*
	 * https://docs.google.com/document/d/1dMSGjiEBT2QUB8dlCaFLlMvitEhg_pcKElLnQZsa0Ns/edit?usp=sharing
	 */
	CNST_ELEM_ADD(NVMEIB_COMPAT_MAX_NR_CHANNELS_PER_PATH);
	CNST_ELEM_ADD(NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT);
	CNST_ELEM_ADD(NVMEIB_MAX_NORDDA_IO_REQ);
	CNST_ELEM_ADD(VOLUME_SERVER_MAX_ARRAY_SIZE);
	CNST_ELEM_ADD(NVMEIBS_MAX_ADMIN_MSG_SIZE);
	CNST_ELEM_ADD(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	CNST_ELEM_ADD(NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	CNST_ELEM_ADD(LOCAL_DISK_STATUS_STR_LEN);
	CNST_ELEM_ADD(MAX_PORTS_FOR_LOCKS_GIDS);
	CNST_ELEM_ADD(NVMEIB_IB_DEVICE_NAME_MAX);
	CNST_ELEM_ADD(NVMEIB_GID_STR_MAX);
	CNST_ELEM_ADD(NVMEIB_HOST_NAME_LEN);
	CNST_ELEM_ADD(NVMEIB_MAX_KERN_VER_STRLEN);
	CNST_ELEM_ADD(NVMEIB_MAX_OFED_VER_STRLEN);
	CNST_ELEM_ADD(NVMEIBS_LOST_SRV_RESOURCE_PAYLOAD_SIZE);
	CNST_ELEM_ADD(NVMEIBS_NORDDA_SERVER_MSG_SIZE);
	CNST_ELEM_ADD(NVMEIBS_CONFIG_MSG_RDMA_PAGES);
	CNST_ELEM_ADD(NVMEIB_IOCH_KA_WRITE_LEN);
	CNST_ELEM_ADD(NVMEIB_HOST_NAME_LEN);
	CNST_ELEM_ADD(NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE);
	CNST_ELEM_ADD(NVMEIBC_NORDDA_CLIENT_MSG_SIZE);

	nvmeib_container_cnst_stride_init(&conf->share_const_ctnr, ops->magic_val,
									  ops->vex_ext, i, sizeof(conf->share_consts[0]));

	rv = sizeof(*conf) + i * sizeof(conf->share_consts[0]);
out:
	return rv;

#undef CNST_ELEM_ADD
}

static ssize_t prp_share_config(void *p, void *buf, const void *buf_end)
{
	struct nvmeibc_ib_admin_channel *ch = p;
	struct volume_client_req *req = buf;
	ssize_t rv;

	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE() > buf_end);
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(ch->base.vex_ops[vex_ach_shared_cfg]->vex_ext);
	_ND(trace_ib_admin_channel_prp_share_config, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_SHARE_CONFIG;

	if ((rv = CALL_VEX_OP(encode,
		vex_ach_shared_cfg_clnt_ops, BASE_ONLY,
			base, vex_ach_shared_cfg_clnt_base_encode,
				ch->base.vex_ops[vex_ach_shared_cfg],
				NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_PAYLOAD(req), buf_end, NULL)) < 0)
		goto out;

	rv += NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE();
	BUG_ON(buf + rv > buf_end);

out:
	return rv;
}

struct vex_get_io_ctx {
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_disk *disk;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_io_clnt_base_encode)
{
	struct vex_get_io_ctx *ctx = arg;
	struct nvmeibc_ib_admin_channel *ch = ctx->ch;
	struct volume_client_config_ma_get_io_base *io_req = wire_buf;
	struct nvmeibc_disk *disk = ctx->disk;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*io_req) > wire_buf_end);
	snprintf(io_req->client_name, sizeof(io_req->client_name),
		"%s", nvmeib_get_utsname_nodename());
	memcpy(io_req->disk_name, disk->name,
		sizeof(io_req->disk_name));
	io_req->rdma.msg_raddr = cpu_to_be64(ch->msg_area_map.ioaddr);
	io_req->rdma.msg_size = cpu_to_be32(ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift);
	io_req->rdma.msg_rkey = cpu_to_be32(ch->msg_area_map.rkey);

	return sizeof(*io_req);
}


/* prepare remote io nics message */
static ssize_t prp_ma_get_io(void *p, void *buf, const void *buf_end)
{
	struct nvmeibc_ib_admin_channel *ch = p;
	struct volume_client_req *req = buf;
	struct vex_get_io_ctx ctx = {
		.ch = ch,
		.disk = ch->base.base.disk,
	};
	ssize_t rv;

	__NFIN;
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE()  > buf_end);
	/* init the command */
	req = buf;
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(ch->base.vex_ops[vex_ach_get_io]->vex_ext);
	_ND(trace_ib_admin_channel_prp_ma_get_io, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_GET_IO;

	if ((rv = CALL_VEX_OP(encode,
		vex_ach_get_io_clnt_ops, BASE_ONLY,
			base, vex_ach_get_io_clnt_base_encode,
				ch->base.vex_ops[vex_ach_get_io],
				NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_PAYLOAD(req), buf_end, &ctx)) < 0)
		goto out;

	rv += NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE();

out:
	__NFOUT;
	return rv;
}

struct nvmeibc_io_msg_ctx {
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_io_rnic *cur_rionic;
};

/* WAS: alloc_iornic_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_io_port_info_clnt_base_decode)
{
	struct nvmeibc_io_msg_ctx *msg_ctx = arg;
	struct list_head *rionics = &msg_ctx->ch->base.rionics;
	const struct wire_get_io_ports_info_base *pi = wire_buf;
	struct nvmeibc_io_rnic *rionic = NULL;
	char gid_buf[GUID_SIZE] = {0};
	enum nvmeib_dev_type local_hw_type, rnic_hw_type;
	int rv = 0;

	NFIN;
	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(pi + 1) > wire_buf_end);

	if (elem_idx == 0) {
		_NT(vex_ach_get_io_port_info_clnt_base_decode_t1,
			"n_remote_gid=@INT", n_elem);
	}

	if (!NVMEIBC_USE_BOTH_ROCE_AND_TCP_IO_CH) {
		rnic_hw_type = (enum nvmeib_dev_type)be32_to_cpu(pi->hw_type);
		local_hw_type = msg_ctx->ch->net.base.port->nic_dev->dev->dev_type;

		if (local_hw_type != rnic_hw_type && (local_hw_type == DT_siw || rnic_hw_type == DT_siw)) {
			_NT(vex_ach_get_io_port_info_clnt_base_decode_inf_1551,
				"skip local hw_type=@INT remote hw_type=@INT",
				local_hw_type, rnic_hw_type);
			goto skip;
		}
	}

	memset(gid_buf, 0, GUID_SIZE);
	if (!memcmp(pi->hw_gid, gid_buf, GUID_SIZE)) {
		_NE(vex_ach_get_io_port_info_clnt_base_decode_inf_1559,
			"skip remote empty gid");
		goto skip;
	}

	if (!(rionic = kzalloc(sizeof(*rionic), GFP_KERNEL))) {
		union ib_gid *hw_gid = (union ib_gid *)pi->hw_gid;
		format_gid(hw_gid, gid_buf);
		_NE(error_ib_admin_channel_alloc_iornic_base, "Failed to allocate data for remote nic @GID_BUF", gid_buf);
		rv = -ENOMEM;
		goto out;
	}
	rionic->ch = rionics_to_ac(rionics);
	rionic->local = msg_ctx->arnic->local;
	INIT_LIST_HEAD(&rionic->lionics);
	INIT_LIST_HEAD(&rionic->nr_lionics);
	INIT_LIST_HEAD(&rionic->disk_nrlink);
	INIT_LIST_HEAD(&rionic->disk_link);

	spin_lock_init(&rionic->spinlock);

	if (NVMEIB_UPDATE_NW_PATHS) {
		memcpy(rionic->hw_gid.raw, pi->hw_gid, 16);
		rionic->may_access = pi->may_access;

		_NT(trace_ib_admin_channel_alloc_iornic_base, "Recv rionic @PTR: hw-gid=@GID_IPV6, may_access=@MAY_ACCESS",
			rionic, rionic->hw_gid.raw, rionic->may_access);
	}

	memcpy(rionic->ib_gid.raw, pi->ib_gid, 16);
	format_gid(&rionic->ib_gid, gid_buf);
	rionic->pkey = (u16)be32_to_cpu(pi->pkey);
	rionic->hw_type = (u16)be32_to_cpu(pi->hw_type);
	rionic->layer = (enum rdma_link_layer)be32_to_cpu(pi->layer);
	rionic->n_msgs = (u16)be32_to_cpu(pi->n_msgs);
	rionic->nr_prefered = (u16)be32_to_cpu(pi->nr_prefered);
	_NT(trace_1_ib_admin_channel_alloc_iornic_base, "Got IO nic @GID_BUF, pkey=@PKEY, hw_type=@HW_TYPE, layer=@LAYER_CHR, max_msgs=@MAX_MSGS, "
	   "nr_prefered=@NR_PREFERED",
		gid_buf, rionic->pkey, rionic->hw_type,
		rionic->layer == IB_LINK_LAYER_INFINIBAND ? 'I' : 'E',
		rionic->n_msgs, rionic->nr_prefered);
	list_add_tail(&rionic->admin_link, rionics);

skip:
	rv = sizeof(*pi);
	goto out;

out:
	msg_ctx->cur_rionic = rionic;
	NFOUT;
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_io_port_info_clnt_ext1_decode)
{
	struct nvmeibc_io_msg_ctx *msg_ctx = arg;
	struct nvmeibc_io_rnic *cur_rionic = msg_ctx->cur_rionic; /* Could be NULL if rionic was skipped by base */
	const struct wire_get_io_ports_info_ext1 *ext1 = wire_buf;

	if (!ext1) {
		if (cur_rionic) {
			if (cur_rionic->hw_type == DT_siw)
				cur_rionic->transport_type = RDMA_TRANSPORT_IWARP;
			else
				cur_rionic->transport_type = RDMA_TRANSPORT_IB;
			cur_rionic->priority.raw = 0;
		}
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);
	if (cur_rionic) {
		cur_rionic->transport_type = ext1->transport_type;
		cur_rionic->priority.numa_dist = ext1->numa_dist;
		cur_rionic->priority.transport = ext1->trans_prio;
		cur_rionic->priority.latency = ext1->lat_prio;
		cur_rionic->priority.bw = ext1->bw_prio;

		_NI(trace_vex_ach_get_io_port_info_clnt_ext1_decode,
				"transport_type @INT num_dist @INT trans_port @INT lat_prio @INT bw_prio @INT",
				ext1->transport_type, ext1->numa_dist, ext1->trans_prio, ext1->lat_prio, ext1->bw_prio);
	}
	return sizeof(*ext1);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_io_port_info_clnt_ext2_decode)
{
	struct nvmeibc_io_msg_ctx *msg_ctx = arg;
	struct nvmeibc_io_rnic *cur_rionic = msg_ctx->cur_rionic; /* Could be NULL if rionic was skipped by base */
	const struct wire_get_io_ports_info_ext2 *ext2 = wire_buf;

	if (!ext2) {
		if (cur_rionic) {
			cur_rionic->node_guid = 0;
		}
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext2) > wire_buf_end);
	if (cur_rionic) {
		cur_rionic->node_guid = ext2->node_guid;

		_NT(trace_vex_ach_get_io_port_info_clnt_ext2_decode,
		    "node_guid @NODE_GUID", ext2->node_guid);
	}
	return sizeof(*ext2);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_io_port_info_clnt_ext3_decode)
{
	struct nvmeibc_io_msg_ctx *msg_ctx = arg;
	struct nvmeibc_io_rnic *cur_rionic = msg_ctx->cur_rionic; /* Could be NULL if rionic was skipped by base */
	const struct wire_get_io_ports_info_ext3 *ext3 = wire_buf;

	if (!ext3) {
		/* Fill defaults */
		if (cur_rionic) {
			cur_rionic->tcp_base_port = nvmeib_get_tcp_base_port_id();
			cur_rionic->tcp_num_ports = nvmeib_get_tcp_num_ports(NULL);
		}
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext3) > wire_buf_end);
	if (cur_rionic) {
		cur_rionic->tcp_base_port = be16_to_cpu(ext3->tcp_base_port);
		cur_rionic->tcp_num_ports = be16_to_cpu(ext3->tcp_num_ports);

		_NT(trace_vex_ach_get_io_port_info_clnt_ext3_decode,
			"Rionic TCP Ports: [@START_PORT, @END_PORT]",
			cur_rionic->tcp_base_port, cur_rionic->tcp_base_port + cur_rionic->tcp_num_ports - 1);
	}
	return sizeof(*ext3);
}

/* WAS: nvmeibc_alloc_disk_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_io_alloc_disk_clnt_base_decode)
{
	struct nvmeibc_io_msg_ctx *msg_ctx = arg;
	const struct wire_get_io_disks_info_base *d = wire_buf;
	struct nvmeibc_disk *disk = msg_ctx->ch->base.base.disk;
	int rv, priority;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*d) > wire_buf_end);

	if (elem_idx == 0) {
		_NT(vex_ach_get_io_alloc_disk_clnt_base_decode_t1,
			"n_remote_disks=@INT", n_elem);
	}

	if (!strncmp(disk->name, d->id, sizeof(disk->name))) {
		_NT(trace_ib_admin_channel_nvmeibc_alloc_disk_base, "received remote_disk @DISK_NAME", disk->name);
		++msg_ctx->ch->base.n_disks;
	} else {
		_NT(trace_1_ib_admin_channel_nvmeibc_alloc_disk_base, "disk names not match have @DISK_NAME got @ID_STR", disk->name, d->id);
		rv = -1;
		goto out;
	}
	msg_ctx->arnic->prefered = be32_to_cpu(d->prefered);
	msg_ctx->arnic->num_conns = be32_to_cpu(d->exising_conn_num);
	priority = be32_to_cpu(d->numa_dist);
	if (priority > msg_ctx->arnic->priority.numa_dist)
		msg_ctx->arnic->priority.numa_dist = priority;

	rv = sizeof(*d);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_io_alloc_disk_clnt_ext1_decode)
{
	struct nvmeibc_io_msg_ctx *msg_ctx = arg;
	const struct wire_get_io_disks_info_ext1 *d = wire_buf;
	struct nvmeibc_disk *disk = msg_ctx->ch->base.base.disk;
	int rv;

	if (!wire_buf) {
		/* Fill with defaults */
		disk->nrch_ioreq_num = NVMEIB_MAX_NORDDA_IO_REQ;
		rv = 0;
		goto out;
	}
	disk->nrch_ioreq_num = be32_to_cpu(d->nrch_ioreq_num);

	rv = sizeof(*d);

out:
	return rv;
}

static int parse_read_io_msg(struct nvmeibc_ib_admin_channel *ch,
			struct nvmeibc_admin_rnic *arnic)
{
	int rv = -1;
	struct nvmeibc_io_msg_ctx msg_ctx = {};
	const struct nvmeib_container *c = ch->msg_area;

	__NFIN;
	nvmeib_mem_sync_map_for_cpu(&ch->msg_area_map, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);
	//arnic->priority = 0; //set in __format_target_nic()
	arnic->num_conns = 0;
	ch->base.n_disks = 0;

	msg_ctx.ch = ch;
	msg_ctx.arnic = arnic;

	if ((rv = CALL_VEX_OP(decode_container,
		vex_ach_get_io_alloc_disk_clnt_ops, ONE_EXT,
			base, vex_ach_get_io_alloc_disk_clnt_base_decode,
			ext1, vex_ach_get_io_alloc_disk_clnt_ext1_decode,
				ch->base.vex_ops[vex_ach_get_io_alloc_disk],
				c, ch->msg_area_end, &msg_ctx)) < 0) {
		if (rv == -ENOENT) {
			_NT(trace_1_ib_admin_channel_parse_read_io_msg,
					"No disk returned from the target admin-ch @BASE_NAME",
					ch->base.base.name);
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_READ_IO_NO_DISKS);
		}

		goto out;
	}

	c = ch->msg_area + rv;
	if (ch->base.n_disks) {
		if ((rv = CALL_VEX_OP(decode_container,
			vex_ach_get_io_port_info_clnt_ops, THREE_EXT,
				base, vex_ach_get_io_port_info_clnt_base_decode,
				ext1, vex_ach_get_io_port_info_clnt_ext1_decode,
				ext2, vex_ach_get_io_port_info_clnt_ext2_decode,
				ext3, vex_ach_get_io_port_info_clnt_ext3_decode,
					ch->base.vex_ops[vex_ach_get_io_port_info],
					c, ch->msg_area_end, &msg_ctx)) < 0)
			goto out;
	}
	else {
		_ND(trace_2_ib_admin_channel_parse_read_io_msg, "Access point @DGID has no user disks", &ch->net.base.path.dgid);
	}

	rv = 0;

out:
	__NFOUT;
	return rv;
}

static int parse_read_lock_mems_msg(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeib_iu **recv_ioctx, u64 tag);

static int read_lock_mems(struct nvmeibc_ib_admin_channel *ch)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *recv_ioctx = NULL;
	struct nvmeibc_admin_rnic *arnic = ch->base.arnic;
	struct nvmeibc_disk *disk = ch->base.arnic->channel->base.disk;
	int rv = -ENOENT;

	__NFIN;
	_NT(trace_ib_admin_channel_read_lock_mems, "--- Sending NVMEIBC_MA_GET_DISK_MEMS message to controller @BASE_NAME",
		ch->base.base.name);
	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_read_lock_mems, "No free messages for administrator to use");
		ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_NO_FREE_MESSAGES);
		goto out;
	}
	/* set the iu type */
	/* set the request */
	req = ch->send_ioctx->buf;
	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(ch,
		prp_ma_get_locks_mems, ch, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0)
		goto free_iu;
	/* wait for the reply */
	if ((recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		_ND(trace_1_ib_admin_channel_read_lock_mems, "rsp->hdr.tag=@TAG req=@REQ_LLONG, rsp->opcode=@OPCODE, @NVMEIBS_RSP_MGMT_OPCODE_OK", rsp->hdr.tag,
			req->hdr.tag, rsp->opcode, NVMEIBS_RSP_MGMT_OPCODE_OK);
		if (rsp->hdr.tag != req->hdr.tag ||
			rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK ||
			parse_read_lock_mems_msg(ch, &recv_ioctx, req->hdr.tag) < 0) {
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_LOCK_MEM_MSG_FAILED);
			DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_PARSE_LOCK_MEM_MSG_FAILED);
			_NT(trace_2_ib_admin_channel_read_lock_mems, "Could not parse received read_lock_mems msg "
			   "(tag req @TAG vs. rsp @TAG, version req @VERSION_TAG, rsp @VERSION_TAG rsp->opcode = @OPCODE)\n",
			   rsp->hdr.tag, req->hdr.tag, be16_to_cpu(req->version_tag), be16_to_cpu(rsp->version_tag), rsp->opcode);
			rv = -EINVAL;
			goto free_iu;
		}
	}
	else {
		_NT(trace_3_ib_admin_channel_read_lock_mems, "Failed to wait for new receivee messages");
		rv = -1;
		goto free_iu;
	}
	_NT(trace_4_ib_admin_channel_read_lock_mems, "--- Received NVMEIBC_MA_GET_DISK_MEMS message reply "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");

free_iu:
	if (recv_ioctx) {
		nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
		recv_ioctx = NULL;
	}
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);

out:
	ch->send_ioctx = NULL;

	__NFOUT;
	return rv;
}

static int share_config(struct nvmeibc_ib_admin_channel *ch)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *recv_ioctx = NULL;
	int rv = -ENOENT;

	_NT(trace_ib_admin_channel_share_config, "--- Sending NVMEIBC_MA_SHARE_CONFIG message to controller @RHOST_NAME",
		ch->base.base.rhost_name);

	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_share_config, "No free messages for administrator to use");
		goto out;
	}

	req = ch->send_ioctx->buf;
	rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(
		ch, prp_share_config, ch, NVMEIB_SEND_CFG, ch->send_ioctx);
	if (rv < 0) {
		_NT(trace_0_ib_admin_channel_share_config, "Err (@RV), sending NVMEIBC_MA_SHARE_CONFIG", rv);
		goto free_iu;
	}

	recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, req->hdr.tag);
	if (recv_ioctx) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != req->hdr.tag) {
			_NT(trace_1_ib_admin_channel_share_config, "wrong tag");
			rv = -1;
		} else if (rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK) {
			_NT(trace_2_ib_admin_channel_share_config, "bad rsp code ");
			rv = -1;
		}
		goto post_recv;

	} else {
		_NT(trace_3_ib_admin_channel_share_config, "Err, recv comp timeout");
		rv = -1;
		goto free_iu;
	}
	rv = 0;

post_recv:
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);

out:
	ch->send_ioctx = NULL;
	return rv;

}

static int read_io(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_admin_rnic *arnic)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *recv_ioctx = NULL;
	int rv = -ENOENT;

	__NFIN;
	_NT(trace_ib_admin_channel_read_io, "--- Sending NVMEIBC_MA_GET_IO message to controller @BASE_NAME",
		ch->base.base.name);
	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_read_io, "No free messages for administrator to use");
		goto out;
	}
	/* set the iu type */
	/* set the request */
	req = ch->send_ioctx->buf;
	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(
		ch, prp_ma_get_io, ch, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0)
		goto free_iu;
	/* wait for the reply */
	if ((recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != req->hdr.tag ||
			rsp->version_tag != req->version_tag ||
			rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK ||
			(rv = parse_read_io_msg(ch, arnic)) < 0) {
			_NT(trace_1_ib_admin_channel_read_io, "Could not parse received read_io msg "
				   "(tag req @TAG vs. rsp @TAG, version req @VERSION_TAG, rsp @VERSION_TAG rsp->opcode = @OPCODE rv=@RV)\n",
				   rsp->hdr.tag, req->hdr.tag, be16_to_cpu(req->version_tag), be16_to_cpu(rsp->version_tag), rsp->opcode, rv);
			rv = -EINVAL;
			goto post_recv;
		}
	}
	else {
		_NT(trace_2_ib_admin_channel_read_io, "Failed to wait for new receivee messages");
		rv = -1;
		goto free_iu;
	}

post_recv:
	_NT(trace_3_ib_admin_channel_read_io, "--- Received NVMEIBC_MA_GET_IO message reply "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);

out:
	ch->send_ioctx = NULL;
	if (rv)
		ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_READ_IO_FAILED);

	__NFOUT;
	return rv;
}

struct nvmeibc_ib_admin_channel *nvmeibc_ib_admin_channel_create(
	const struct nvmeibc_cinst_params_core *p, struct nvmeibc_admin_rnic *arnic)
{
	struct nvmeibc_ib_admin_channel *ch;
	NFIN;
	if (!(ch = kzalloc(sizeof(struct nvmeibc_ib_admin_channel), GFP_KERNEL))) {
		_NE(error_ib_admin_channel_nvmeibc_ib_admin_channel_create, "Fail to allocate initial admin channels");
		goto out;
	}
	ch->ranic_gid = arnic->ib_gid;
	/* init net.base from arnic */
	memcpy(ch->net.base.path.dgid.raw, arnic->ib_gid.raw, 16);
	//ch->net.base.pkey = arnic->pkey; //EC-6781: should use *local* port's lkey
	ch->net.base.service_id = arnic->service_id;
	ch->net.base.service_port = arnic->service_port;

	/* init our channel */
	if (init(p, ch) < 0)
		goto do_err;
	goto out;

do_err:
		freee(ch);
		kfree(ch);
		ch = NULL;
out:
	NFOUT;
	return ch;
}

int nvmeibc_ib_admin_channel_connect(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_disk *disk, struct nvmeibc_admin_rnic *arnic)
{
	int rv = 0;
	bool access_local = disk->access_local;
	bool do_read_io = !access_local;
	int state;

	__NFIN;
	keep_alive_init(ch);
	/* cache some net stuff and create stuff needed for io */
	initialize_net(ch);
	ch->logout = false;

	/* try to login as a new main volume administrator */
	if ((rv = login(ch, access_local)) < 0) {
		ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_LOGIN_FAILED);
		state = nvmeib_get_state_guard(&ch->net.base.state);
		/* state may change to DISCONNECTING if RTR succeeded
		   and we received comp with err */
		_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_connect, "Admin channel failed to login net-state = @STATE (@BASE_NAME)",
		   state, ch->base.base.name);
		if (state == NVMEIBC_IB_NET_INIT) {
			_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_connect, "Admin net created but connect-qp failed - "
			   "no completions expected RTU wasn't sent");
			goto free_net;
		}
		else
			goto out;
	}

	/* try to login as a new main volume administrator */
	if (do_read_io) {
		rv = share_config(ch);
		if (rv < 0) {
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_SHARE_CONFIG_FAILED);
			if (rv != -ENOTSUPP)
				goto free_net;
			_NT(nvmeibc_ib_admin_channel_connect_t1,
				"Shared Config not supported in version: " NVMEIB_VERSION_PRINT_PROT_FMTN(),
			   	NVMEIB_VERSION_PRINT_PROT_ARG(&ch->base.link_version));
		}

		rv = read_io(ch, arnic);
		if (rv < 0) goto free_net;
	}
	goto out;


free_net:
	//omril: test this case!!!
	nvmeibc_ib_admin_channel_disconnect(ch);

out:
	__NFOUT;
	return rv;
}

/* @path may be NULL, indicating path from @port to @rionic was not found;
   used for creating lionic with path_valid = false */
static int try_add_rionic(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_io_rnic *rionic, struct nvmeibc_ib_port *port,
	struct nvmeib_rdma_path_info *info)
{
	struct nvmeibc_disk *disk = ch->base.base.disk;
	struct nvmeibc_io_rnic *drionic;
	struct nvmeibc_io_lnic *lionic;
	bool found = false;
	int rv = 0;
	__NFIN;

	/*
	 * Add rionic to disk list w/o duplication of remote-gid.
	 */
	list_for_each_entry(drionic, &disk->rionics, disk_link) {
		if (!memcmp(drionic->hw_gid.raw, rionic->hw_gid.raw,
			sizeof(drionic->hw_gid.raw))) {
			found = true; /* use rionic of another admin-ch */
			break;
		}
	}
	if (!found) {
		list_add_tail(&rionic->disk_link, &disk->rionics);
		rionic->disk = disk;
		drionic = rionic;
		found = true;
	}

	/*
	 * Create and pair (cross-ref) lionic to rionic, w/o duplication
	 */
	found = false;
	list_for_each_entry(lionic, &drionic->lionics, rionic_link) {
		if (port == lionic->port) {
			found = true;
			rv = 0;
			break;
		}
	}
	if (!found) {
		/* this is a new path so register it by creating lionic */
		if (!(lionic = nvmeibc_disk_create_lionic(disk, port, info))) {
			_NE(error_ib_admin_channel_try_add_rionic, "OOM: fail to allocate lionic");
			rv = -1;
		}
		else {
			_ND(trace_ib_admin_channel_try_add_rionic, "Disk @DISK_NAME, add lionic @SGID to rionic @IB_GID_IPV6",
				disk->name, &lionic->path.sgid, &drionic->ib_gid);
			lionic->rionic = drionic;
			list_add_tail(&lionic->rionic_link, &drionic->lionics);
			rv = 1;
		}
	}

	__NFOUT;
	return rv;
}

int nvmeibc_ib_admin_channel_access_iornics(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeib_rdma_ib_port_gid *port_gids, u16 port_pkey, struct nvmeibc_ib_port *port)
{
	struct nvmeibc_io_rnic *rionic, *t;
	struct ib_sa_path_rec path = ch->net.base.path;
	struct ib_sa_path_rec *ppath __attribute__((unused));
	struct nvmeib_rdma_path_info info = {0};
	int rv = 0;

	__NFIN;
	if (!ch->base.n_disks) {
		_ND(trace_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Controller @BASE_NAME has no user disk", ch->base.base.name);
		goto out;
	}

	/* first get the local guid */
	path.sgid = port->gid.gid;
	list_for_each_entry_safe(rionic, t, &ch->base.rionics, admin_link) {
		if (rionic->local && rionic->ib_gid.global.interface_id != port_gids->gid.global.interface_id) {
			_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Skipping connecting to local rionic @IB_GID_IPV6 from other local nic @GID_IPV6",
			   &rionic->ib_gid, &port_gids->gid);
			continue;
		}
		if (rionic->layer != port_gids->link_layer) {
			_NT(trace_nvmeibc_ib_admin_channel_access_iornics_skip_layer, "Skipping connecting to rionic @IB_GID_IPV6 with layer @LINK_LAYER from local nic @GID_IPV6 with layer @LINK_LAYER",
				&rionic->ib_gid, rionic->layer, &port_gids->gid, port_gids->link_layer);
				continue;
		}
		if (rionic->transport_type != port_gids->transport_type) {
			_NT(trace_nvmeibc_ib_admin_channel_access_iornics_skip_trans, "Skipping connecting to rionic @IB_GID_IPV6 with transport @TRANSPORT_TYPE from local nic @GID_IPV6 with transport @TRANSPORT_TYPE",
				&rionic->ib_gid, rionic->transport_type, &port_gids->gid, port_gids->transport_type);
				continue;
		}
		memcpy(path.dgid.raw, rionic->ib_gid.raw, 16);
		_NT(trace_2_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "checking src @SGID (l=@LAYER) -> dst @DGID (l=@LAYER)",
			&path.sgid, port->layer, &path.dgid, rionic->layer);

		/* do not copy from admin channel since the admin can be RoCE */
		path.service_id = cpu_to_be64(NVMEIB_SERVICE_ID);
		path.pkey = cpu_to_be16(port->pkey);
		info.dev = P2NV(port);
		info.sa = nvmeibc_sa_client(nvmeibc_cinst_get_core_p(&ch->base.base));
		info.path = &path;
		info.src_port = port->port;
		if (port->layer == IB_LINK_LAYER_INFINIBAND) {
			info.service_id = NVMEIB_SERVICE_ID;
			info.pkey = port->pkey;
			info.service_port = 0;
			info.rdma_type = _rdma_ib;
		}
		else {
			info.service_id = 0;
			info.pkey = 0;
			if (port->transport_type == RDMA_TRANSPORT_IWARP) {
				info.service_port = nvmeib_get_tcp_base_port_id();
				info.rdma_type = _rdma_iwarp;
			} else {
				info.service_port = NVMEIB_PORT_ID;
				info.rdma_type = _rdma_roce;
			}
		}

		if (!NVMEIB_UPDATE_NW_PATHS) {
			if (!nvmeibc_disk_find_path(ch->base.base.disk, &info)) {
				_ND(trace_3_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Found path to rionic");
				/* check if we need to register the rionic with any of the disks
				that this local admin channel can access.  it is possible that
				the current admin channel reports the same rionic as pervious
				admin channel - namely management gave us multiple admin nics
				of the same machine
				*/
				rv = try_add_rionic(ch, rionic, port, &info);
				if (rv < 0) {
					_NT(trace_4_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Handling remote io nic did not finish - bail out");
					goto out;
				}
				else if (rv) /* rememeber that this channel is used */
					++ch->base.n_rionics_used;
			}
		} else {
			ppath = NULL;
			if (!rionic->may_access || !nvmeibc_ib_port_enabled(port)) {
				_NT(trace_5_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Adding path although no access l=@MAY_ACCESS:r=@ENB",
				    rionic->may_access, nvmeibc_ib_port_enabled(port));
			}
			else if (rionic->layer != port->layer) {
				_NT(trace_6_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Adding path although layers mismatch");
			}
			else if (nvmeibc_disk_find_path(ch->base.base.disk, &info)) {
				_NT(trace_7_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Adding rionic although find-path failed");
			}
			else {
				ppath = info.path;
				_ND(trace_8_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Found path to rionic");
			}

			/* check if we need to register the rionic with any of the disks
			that this local admin channel can access.  it is possible that
			the current admin channel reports the same rionic as pervious
			admin channel - namely management gave us multiple admin nics
			of the same machine
			*/
			rv = try_add_rionic(ch, rionic, port, &info);
			if (rv < 0) {
				_NT(trace_9_ib_admin_channel_nvmeibc_ib_admin_channel_access_iornics, "Handling remote io nic did not finish - bail out");
				goto out;
			}
			else if (rv) /* rememeber that this channel is used */
				++ch->base.n_rionics_used;
		}
	}

out:
	__NFOUT;
	return rv;
}

/* Client Access Map context */
struct nvmeibc_access_map_ctx {
	struct nvmeibc_ib_admin_channel *ach;
	struct nvmeibc_disk *disk;
	struct nvmeibc_admin_rnic *arnic;
	struct nvmeibc_io_rnic *rionic;
	struct nvmeibc_io_lnic *lionic;
};

/* WAS: fill_lionic_map_base */
VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_acs_map_clnt_ionics_clnt_base_encode)
{
	struct nvmeibc_access_map_ctx *ctx = arg;
	struct nvmeibc_io_lnic *lionic = ctx->lionic;
	struct wire_acs_map_clnt_ionics_base *lnic = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*lnic) > wire_buf_end);
	/* copy the local io guid */
	if (!NVMEIB_UPDATE_NW_PATHS) {
		memcpy(lnic->gid, lionic->path.sgid.raw, 16);
		_NT(trace_ib_admin_channel_vex_ach_acs_map_clnt_ionics_clnt_base_encode, ">>> [@I_LIONICS] lionic @IB_GID_IPV6", elem_idx, &lionic->path.sgid);
	}
	else {
		memcpy(lnic->gid, lionic->port->gid.hw_gid.raw, 16);
		_NT(trace_1_ib_admin_channel_vex_ach_acs_map_clnt_ionics_clnt_base_encode, ">>> [@I_LIONICS] lionic hw-gid=@IB_GID_IPV6", elem_idx, &lionic->port->gid.hw_gid.raw);

		lnic->may_access = !!nvmeibc_ib_port_enabled(lionic->port);
	}
	return sizeof(*lnic);
}

/* WAS: fill_rionic_map_base */
VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_acs_map_srv_ionics_clnt_base_encode)
{
	struct wire_acs_map_srv_ionics_base *rnic = wire_buf;
	struct nvmeibc_access_map_ctx *ctx = arg;
	struct nvmeibc_ib_admin_channel *ach = ctx->ach;
	struct nvmeibc_io_rnic *rionic = ctx->rionic;
	struct list_head *lionics = &rionic->lionics;
	const struct vex_ops *clnt_ionics_ops = ach->base.vex_ops[vex_ach_acs_map_clnt_ionics];
	int rv;
	size_t cntnr_sz_cnt = sizeof(rnic->clnt_ionics_map);
	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*rnic) > wire_buf_end);

	/* copy the remote io guid */
	if (!NVMEIB_UPDATE_NW_PATHS) {
		memcpy(rnic->gid, rionic->ib_gid.raw, 16);
			_NT(trace_ib_admin_channel_vex_ach_acs_map_srv_ionics_clnt_base_encode,
				">> [@I_RIONICS] rionic @IB_GID_IPV6", elem_idx, &rionic->ib_gid);
	}
	else {
		memcpy(rnic->gid, rionic->hw_gid.raw, 16);
		_NT(trace_1_ib_admin_channel_vex_ach_acs_map_srv_ionics_clnt_base_encode,
			">>> [@I_RIONICS] rionic hw-gid=@IB_GID_IPV6", elem_idx, &rionic->hw_gid);
	}

	_NT(trace_2_ib_admin_channel_vex_ach_acs_map_srv_ionics_clnt_base_encode,
		">>>Encoding clnt ionics container at @PTR\n", &rnic->clnt_ionics_map);
	/* Encode the client ionic container */
	if ((rv = vex_encode_container_hdr(NULL, clnt_ionics_ops, &rnic->clnt_ionics_map, wire_buf_end) < 0))
		goto out;

	/* loop on all local io nics that access the remote io nic */
	list_for_each_entry(ctx->lionic, lionics, rionic_link) {
		rv = CALL_VEX_OP(encode_container_elem,
			vex_ach_acs_map_srv_ionics_clnt_ops, BASE_ONLY,
				base, vex_ach_acs_map_srv_ionics_clnt_base_encode,
				   clnt_ionics_ops, &rnic->clnt_ionics_map, wire_buf_end, ctx);
		if (rv < 0)
			goto out;
		cntnr_sz_cnt += rv;
	}

	BUG_ON(cntnr_sz_cnt != nvmeib_container_size(&rnic->clnt_ionics_map));

	_NT(trace_3_ib_admin_channel_vex_ach_acs_map_srv_ionics_clnt_base_encode,
		">>>Encoded clnt ionics container with @COUNT elements (size @ZU)\n",
	   nvmeib_container_n_elem(&rnic->clnt_ionics_map),
	   nvmeib_container_size(&rnic->clnt_ionics_map));

	rv = offsetof(typeof(*rnic), clnt_ionics_map) + nvmeib_container_size(&rnic->clnt_ionics_map);

out:
	return rv;
}

/* WAS: fill_disks_map_base */
VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_acs_map_disks_clnt_base_encode)
{
	int rv;
	struct nvmeibc_access_map_ctx *ctx = arg;
	struct wire_acs_map_disks_base *di = wire_buf;
	struct nvmeibc_disk *disk = ctx->disk;
	struct nvmeibc_ib_admin_channel *ach = ctx->ach;
	struct list_head *rionics = &disk->rionics;
	const struct vex_ops *srv_ionics_ops = ach->base.vex_ops[vex_ach_acs_map_srv_ionics];
	size_t cntnr_sz_cnt = sizeof(di->srv_ionics_map);

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*di) > wire_buf_end);

	/* copy the disk name */
	memcpy(di->disk_id, disk->name, sizeof(disk->name));

	_NT(trace_ib_admin_channel_vex_ach_acs_map_disks_clnt_base_encode,
		"> [@INDEX] disk @DISK_ID_NAME\n", elem_idx, di->disk_id);

	/* Encode the srv ionics container */
	_NT(trace_1_ib_admin_channel_vex_ach_acs_map_disks_clnt_base_encode,
		">> Encoding srv ionics container at @PTR\n", &di->srv_ionics_map);

	if ((rv = vex_encode_container_hdr(NULL, srv_ionics_ops, &di->srv_ionics_map, wire_buf_end)) < 0)
		goto out;

	list_for_each_entry(ctx->rionic, rionics, disk_link) {
		if (ctx->rionic->ch != ctx->arnic->channel)
			continue;
		rv = CALL_VEX_OP(encode_container_elem,
			vex_ach_acs_map_srv_ionics_clnt_ops, BASE_ONLY,
				base, vex_ach_acs_map_srv_ionics_clnt_base_encode,
				srv_ionics_ops, &di->srv_ionics_map, wire_buf_end, ctx);
		if (rv < 0)
			goto out;
		cntnr_sz_cnt += rv;
	}

	BUG_ON(cntnr_sz_cnt != nvmeib_container_size(&di->srv_ionics_map));

	_NT(trace_2_ib_admin_channel_vex_ach_acs_map_disks_clnt_base_encode,
		">>>Encoded srv ionics container with @COUNT elements (size @ZU)\n",
	   nvmeib_container_n_elem(&di->srv_ionics_map),
	   nvmeib_container_size(&di->srv_ionics_map));

	if (!nvmeib_container_n_elem(&di->srv_ionics_map)) {
		rv = -ENOENT;
		goto out;
//			&ctx->arnic->ib_gid, be32_to_cpu(anic->section_len));
	}

	rv = offsetof(typeof(*di), srv_ionics_map) + nvmeib_container_size(&di->srv_ionics_map);
out:
	return rv;
}

/* WAS: fill_ach_map_base */
VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_acs_map_arnics_clnt_base_encode)
{
	struct wire_acs_map_arnic_base *anic = wire_buf;
	struct nvmeibc_access_map_ctx *ctx = arg;
	struct nvmeibc_admin_rnic *arnic = ctx->arnic;
	struct nvmeibc_ib_admin_channel *ach = ctx->ach;
	const struct vex_ops *disk_ops = ach->base.vex_ops[vex_ach_acs_map_disks];
	int rv = 0;

	NFIN;
	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*anic) > wire_buf_end);

	/* Set the GID */
	memcpy(anic->gid, ctx->arnic->ib_gid.raw, 16);

	_NT(trace_ib_admin_channel_vex_ach_acs_map_arnics_clnt_base_encode,
		"> [@INDEX] arnic @IB_GID_IPV6", elem_idx, &arnic->ib_gid);

	_NT(trace_1_ib_admin_channel_vex_ach_acs_map_arnics_clnt_base_encode,
		">>Encoding disks container at @PTR",&anic->disks_map);
	/* Encode the disk container */
	if ((rv = vex_encode_container_hdr(NULL, disk_ops, &anic->disks_map, wire_buf_end)) < 0)
		goto out;

	rv = CALL_VEX_OP(encode_container_elem,
		vex_ach_acs_map_disks_clnt_ops, BASE_ONLY,
			base, vex_ach_acs_map_disks_clnt_base_encode,
				disk_ops, &anic->disks_map, wire_buf_end, ctx);
	if (rv < 0)
		goto out;

	BUG_ON(rv + sizeof(anic->disks_map) != nvmeib_container_size(&anic->disks_map));

	_NT(trace_2_ib_admin_channel_vex_ach_acs_map_arnics_clnt_base_encode,
		">> Encoded disks container with @COUNT elements (size @ZU)",
	   nvmeib_container_n_elem(&anic->disks_map),
	   nvmeib_container_size(&anic->disks_map));

	rv = offsetof(typeof(*anic), disks_map) + nvmeib_container_size(&anic->disks_map);
	anic->section_len = cpu_to_be32(rv);

	goto out;

out:
	NFOUT;
	return rv;
}

static int fill_access_map(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_disk *disk, size_t *acs_map_sz)
{
	void *p = ch->msg_area;
	void *e = p + (ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift);
	struct nvmeib_container *arnics_container = ch->msg_area;
	struct nvmeibc_admin_rnic *arnic;
	int rv = -1;
	struct nvmeibc_access_map_ctx ctx = {
		.ach = ch,
		.disk = disk,
	};
	const struct vex_ops *arnics_ops = ch->base.vex_ops[vex_ach_acs_map_arnics];
	size_t cntnr_sz_cnt = sizeof(*arnics_container);

	__NFIN;
	/* clear message area */
	memset(p, 0xcc, e - p);

	if ((rv = vex_encode_container_hdr(&arnics_container, arnics_ops, p, e)) < 0)
		goto out;

	_NT(trace_ib_admin_channel_fill_access_map,
		">Encoding arnics container at @PTR\n", arnics_container);

	/* loop on all admin channels */
	list_for_each_entry(arnic, &disk->arnics, link) {
		if (!nvmeibc_disk_use_arnic_for_disk(arnic, disk,
			NVMEIBC_ARNIC_ALIVE | NVMEIBC_ARNIC_HAS_RIONICS))
			continue;

		ctx.arnic = arnic;
		rv = CALL_VEX_OP(encode_container_elem,
			vex_ach_acs_map_arnics_clnt_ops, BASE_ONLY,
				base, vex_ach_acs_map_arnics_clnt_base_encode,
					arnics_ops, arnics_container, e, &ctx);
		if (rv < 0) {
			ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_ENCODE_CONTAINER_FAIL);
			goto out;
		}
		cntnr_sz_cnt += rv;
	}
	BUG_ON(cntnr_sz_cnt != nvmeib_container_size(arnics_container));
	_NT(trace_1_ib_admin_channel_fill_access_map,
		"> Encoded arnics container with @COUNT elements (size @ZU)\n",
		nvmeib_container_n_elem(arnics_container),
		nvmeib_container_size(arnics_container));
	rv = 0;
	if (acs_map_sz)
		*acs_map_sz = cntnr_sz_cnt;

out:
	__NFOUT;
	return rv;
}

struct vex_get_acs_ctx {
	struct nvmeibc_ib_admin_channel *ch;
	unsigned req_nrch_per_path;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_acs_clnt_base_encode)
{
	struct volume_client_config_ma_access_base *base = wire_buf;
	struct vex_get_acs_ctx *enc_ctx = arg;
	struct nvmeibc_ib_admin_channel *ch = enc_ctx->ch;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) >= wire_buf_end);

	base->rdma.msg_raddr = cpu_to_be64(ch->msg_area_map.ioaddr);
	base->rdma.msg_size = cpu_to_be32(ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift);
	base->rdma.msg_rkey = cpu_to_be32(ch->msg_area_map.rkey);

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_acs_clnt_ext1_encode)
{
	struct volume_client_config_ma_access_ext1 *ext1 = wire_buf;
	struct vex_get_acs_ctx *enc_ctx = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext1) >= wire_buf_end);

	ext1->req_nrch_per_path = cpu_to_be32(enc_ctx->req_nrch_per_path);

	return sizeof(*ext1);
}

/* prepare get access map message */
static ssize_t prp_ma_get_acs(void *p, void *buf, const void *buf_end)
{
	struct nvmeibc_ib_admin_channel *ch = p;
	struct nvmeibc_disk *disk = ch->base.base.disk;
	struct volume_client_req *req;
	struct vex_get_acs_ctx enc_ctx = {
		.ch = ch,
		.req_nrch_per_path = max_t(unsigned,
					disk->tgt_max_nrchs_per_path_rdma,
					disk->tgt_max_nrchs_per_path_tcp),
	};

	int rv = 0;

	__NFIN;
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE() > buf_end);
	/* init the command */
	req = buf;
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(vex_base);
	_ND(trace_ib_admin_channel_prp_ma_get_acs, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_GET_ACCESS;

	if ((rv = CALL_VEX_OP(encode,
		vex_ach_get_acs_clnt_ops, ONE_EXT,
			base, vex_ach_get_acs_clnt_base_encode,
			ext1, vex_ach_get_acs_clnt_ext1_encode,
				ch->base.vex_ops[vex_ach_get_acs],
				NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_PAYLOAD(req), buf_end, &enc_ctx)) < 0)
		goto out;

	rv += NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE();

out:
	__NFOUT;
	return rv;
}

static struct nvmeib_iu *get_recv_msg(struct nvmeibc_ib_admin_channel *ch,
	u64 tag)
{
	struct nvmeib_iu *recv_ioctx;
	struct volume_server_rsp *rsp;

	__NFIN;
	if ((recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != tag ||
			rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK) {
			_NT(trace_ib_admin_channel_get_recv_msg, "Received msg with tag=@TAG (sent tag=@TAG) "
				"and opcode=@OPCODE", rsp->hdr.tag, tag, rsp->opcode);
			nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
			recv_ioctx = NULL;
		}
	}
	else
		_NT(trace_1_ib_admin_channel_get_recv_msg, "Failed to wait for new receivee messages");

	__NFOUT;
	return recv_ioctx;
}

static int get_controller_info(struct nvmeibc_ib_admin_channel *ch, u64 tag)
{
	struct nvmeib_iu *recv_ioctx;
	struct volume_server_rsp *rsp;
	struct volume_server_config_access_map_gen_rsp *access_g_rsp;
	int n_disks = 0;
	__NFIN;

	/* get the general message */
	if (!(recv_ioctx = get_recv_msg(ch, tag))) {
		_NT(trace_ib_admin_channel_get_controller_info, "failed to receive the controller info access message");
		n_disks = -1;
		goto out;
	}
	rsp = recv_ioctx->buf;
	access_g_rsp = &rsp->access_g_rsp;
	ch->base.cntr_page_size = be32_to_cpu(access_g_rsp->page_size);
	_ND(trace_1_ib_admin_channel_get_controller_info, "Controller page size @CNTR_PAGE_SIZE", ch->base.cntr_page_size);
	/* read the number of remote disks */
	n_disks = be32_to_cpu(access_g_rsp->n_disks);
	_ND(trace_2_ib_admin_channel_get_controller_info, "Controller sent @N_DISKS disk infos", n_disks);
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

out:
	__NFOUT;
	return n_disks == 1 ? n_disks : -1;
}

struct load_disk_info_dec_ctx {
	struct nvmeibc_ib_admin_channel *ch;
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	int  md_size;
	bool md_extd;
	int nsid;
	int sector_shift;
	int n_disk_rscs_sets;
	int n_disk_rscs;
	int max_client_rscs;
	u64 disk_lock_counter;
	int n_disk_lock_segments;
	int max_request_size;
	int n_triplets;
	__be64 *rsrc_guids;
};

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_acs_map_disk_info_clnt_base_decode)
{
	const struct volume_server_config_access_map_per_disk_rsp_base *base = wire_buf;
	struct load_disk_info_dec_ctx *ctx = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	memcpy(ctx->disk_name, base->disk_name, sizeof(ctx->disk_name));
	ctx->nsid = be32_to_cpu(base->nsid);
	ctx->sector_shift = be32_to_cpu(base->sector_shift);
	ctx->md_size = be32_to_cpu(base->md.md_size);
	ctx->md_extd = base->md.md_extd;
	ctx->max_request_size = be32_to_cpu(base->max_request_size);
	ctx->n_triplets = be32_to_cpu(base->triplets);
	ctx->n_disk_rscs_sets = be32_to_cpu(base->disk_rscs_n_sets);
	ctx->n_disk_rscs = be32_to_cpu(base->disk_rscs);
	ctx->max_client_rscs = be32_to_cpu(base->client_rscs);
	ctx->disk_lock_counter = be64_to_cpu(base->disk_lock_counter);
	ctx->n_disk_lock_segments = be32_to_cpu(base->disk_lock_segments);

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_acs_map_di_rsrc_set_clnt_base_decode)
{
	const struct wire_disk_rsc_set_base *base = wire_buf;
	struct load_disk_info_dec_ctx *ctx = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	ctx->rsrc_guids[elem_idx] = base->node_guid;

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_acs_map_disk_info_clnt_ext1_decode)
{
	const struct volume_server_config_access_map_per_disk_rsp_ext1 *ext1 = wire_buf;
	struct load_disk_info_dec_ctx *ctx = arg;
	ssize_t rv;

	if (!wire_buf) {
		/* defaults */
		rv = 0;
		goto out;
	}

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	if (ctx->n_disk_rscs_sets > 0) {
		if (!(ctx->rsrc_guids = kcalloc(ctx->n_disk_rscs_sets, sizeof(*ctx->rsrc_guids), GFP_KERNEL))) {
			_NT(vex_ach_acs_map_disk_info_clnt_ext1_decode_oom, "OOM");
			rv = -ENOMEM;
			goto out;
		}

		if ((rv = CALL_VEX_OP(decode_container,
			vex_ach_acs_map_di_rsrc_set_clnt_ops, BASE_ONLY,
				base, vex_ach_acs_map_di_rsrc_set_clnt_base_decode,
					ctx->ch->base.vex_ops[vex_ach_acs_map_di_rsrc_set],
					wire_buf, wire_buf_end, ctx)) < 0) {
			goto out;
		}

		/* vex_decode_container has returned the size of the container (including header) */
		rv += sizeof(*ext1) - sizeof(ext1->disk_rsc_set_ctnr);
	} else {
		/* Verify container is empty */
		BUG_ON(!NVMEIB_CONT_IS_EMPTY(&ext1->disk_rsc_set_ctnr));
		rv = sizeof(*ext1);
	}
	goto out;

out:
	return rv;
}

static int load_disk_info(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	int *n_triplets, struct nvmeibc_disk *disk, bool is_rediscover)
{
	struct nvmeib_iu *recv_ioctx = NULL;
	struct volume_server_rsp *rsp;
	struct load_disk_info_dec_ctx dec_ctx = {
		.ch = ch,
	};
	int rv = 0;

	__NFIN;
	/* get the per disk info message */
	if (!(recv_ioctx = get_recv_msg(ch, tag))) {
		_NT(trace_ib_admin_channel_load_disk_info, "Fail to receive disk P part");
		rv = -1;
		goto out;
	}
	rsp = recv_ioctx->buf;

	if ((rv = CALL_VEX_OP(decode,
		vex_ach_acs_map_disk_info_clnt_ops, ONE_EXT,
			base, vex_ach_acs_map_disk_info_clnt_base_decode,
			ext1, vex_ach_acs_map_disk_info_clnt_ext1_decode,
				ch->base.vex_ops[vex_ach_acs_map_disk_info],
				&rsp->payload, &rsp->ach_payload_max,
				&dec_ctx)) < 0) {
		_NT(trace_ib_admin_channel_load_disk_info_dec_fail, "Failed (@RV) to decode", rv);
		goto post_recv;
	}

	/* find disk */
	if (memcmp(dec_ctx.disk_name, disk->name, sizeof(disk->name))) {
		_NT(trace_1_ib_admin_channel_load_disk_info, "OOPS: Controller disk @DISK_NAME was not found in our list", dec_ctx.disk_name);
		rv = -1;
		goto post_recv;
	}
	if (dec_ctx.sector_shift > NVMEIBC_SECTOR_SHIFT) {
		_NE(error_ib_admin_channel_load_disk_info, "OOPS: product sector shift @SECTOR_SHIFT is less than "
			"disk sector shift @SECTOR_SHIFT",
			dec_ctx.sector_shift, NVMEIBC_SECTOR_SHIFT);
		rv = -EINVAL;
		goto post_recv;
	}

	disk->md_size = dec_ctx.md_size;
	disk->md_extd = dec_ctx.md_extd;
	disk->sector_shift = dec_ctx.sector_shift;
	disk->max_request_size_bytes = dec_ctx.max_request_size << dec_ctx.sector_shift;
	*n_triplets = dec_ctx.n_triplets;

	_NT(trace_2_ib_admin_channel_load_disk_info, "Disk @DISK_NAME, nsid=@NSID, block_shift=@BLOCK_SHIFT_INT, n_triplets=@N_TRIPLETS, n_disk_rscs=@N_DISK_RSCS, "
	   "max_client_rscs=@MAX_CLIENT_RSCS lock_counter=@LOCK_COUNTER, n_lock_segments=@N_LOCK_SEGMENTS, md_size=@MD_SIZE,"
	   "separated=@SEPARATED",
		disk->name, dec_ctx.nsid, dec_ctx.sector_shift, dec_ctx.n_triplets,
		dec_ctx.n_disk_rscs, dec_ctx.max_client_rscs, dec_ctx.disk_lock_counter, dec_ctx.n_disk_lock_segments,
		disk->md_size, disk->md_extd ? 'N' : 'Y');

	_ND(trace_3_ib_admin_channel_load_disk_info, "Max dma/request size in bytes for disk @DISK_NAME is @MAX_REQUEST_SIZE_BYTES", disk->name,
		disk->max_request_size_bytes);
	if ((rv = nvmeibc_disk_create_remote(ch, disk, dec_ctx.nsid, dec_ctx.sector_shift,
		dec_ctx.n_disk_rscs_sets, dec_ctx.n_disk_rscs, dec_ctx.max_client_rscs, dec_ctx.disk_lock_counter,
		dec_ctx.n_disk_lock_segments,  dec_ctx.rsrc_guids, is_rediscover)) < 0)
		_NT(trace_4_ib_admin_channel_load_disk_info, "Fail to create disk @DISK_NAME resources", disk->name);

post_recv:
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

out:
	kfree(dec_ctx.rsrc_guids);
	__NFOUT;
	return rv;
}

/**
 * keep getting recv-msgs from ch until a total of @n_triplets matching
 * triplets were received. A triplet is {lnic, rnic, n_qps} and a
 * matching-triplet is one that has lnic&rnic matched to one of the
 * disk's rionic&lionic pairs (respectively, note the reversed order).
 */
static int load_disk_nics(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	struct nvmeibc_disk *disk, int n_triplets)
{
	struct nvmeib_iu *recv_ioctx = NULL;
	struct volume_server_rsp *rsp;
	struct volume_server_config_access_map_per_disk_n_rsp *per_disk_n_rsp;
	struct volume_server_config_triplet *triplet;
	struct list_head *rionics = &disk->rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *lionics;
	struct nvmeibc_io_lnic *lionic;
	char rgid_buf[GUID_SIZE] = {0};
	char lgid_buf[GUID_SIZE] = {0};
	bool found;
	int i, j = 0 /* curr-total-recv triplets */, k, m, n, n_qps, rv = 0;
	bool cont = true;
	int n_trip_0_qs = 0;

	__NFIN;
	n = DIV_ROUND_UP(n_triplets, VOLUME_SERVER_MAX_ARRAY_SIZE);
	_NT(trace_ib_admin_channel_load_disk_nics, "n=@COUNT,n_triplets=@N_TRIPLETS", n, n_triplets);
	for (i = 0; i < n; ++i) {
		if (!(recv_ioctx = get_recv_msg(ch, tag))) {
			_NT(trace_1_ib_admin_channel_load_disk_nics, "Fail to receive disk N part");
			rv = -1;
			goto out;
		}
		rsp = recv_ioctx->buf;
		per_disk_n_rsp = &rsp->per_disk_n_rsp;
		k = min(n_triplets - j, VOLUME_SERVER_MAX_ARRAY_SIZE);
		for (m = 0; m < k && cont; ++m, ++j) {
			triplet = &per_disk_n_rsp->a[m];
			format_gid_raw(triplet->lnic, rgid_buf);
			format_gid_raw(triplet->rnic, lgid_buf);
			_ND(trace_2_ib_admin_channel_load_disk_nics, "Triplet: s=@RGID_BUF, d=@LGID_BUF", rgid_buf, lgid_buf);
			found = false;
			list_for_each_entry(rionic, rionics, disk_link) {
				/* check that rionic belongs to this channel */
				if (rionic->ch != &ch->base) {
					_ND(trace_3_ib_admin_channel_load_disk_nics, "nic ch=@CH_PTR, ch=@BASE", rionic->ch, &ch->base);
					continue;
				}
				if (!NVMEIB_UPDATE_NW_PATHS) {
					if (!memcmp(triplet->lnic, rionic->ib_gid.raw, 16)) {
						found = true;
						break;
					}
				}
				else
				{
					if (!memcmp(triplet->lnic, rionic->hw_gid.raw, 16)) {
						found = true;
						break;
					}
				}
			}
			if (unlikely(!found)) {
				format_gid_raw(triplet->lnic, rgid_buf);
				_NT(trace_4_ib_admin_channel_load_disk_nics, "OOPS: Controller lionic @RGID_BUF was not found in our list",
					rgid_buf);
				rv = -1;
				cont = false;
				break;
			}
			lionics = &rionic->lionics;
			found = false;
			list_for_each_entry(lionic, lionics, rionic_link) {
				if (!NVMEIB_UPDATE_NW_PATHS) {
					if (!memcmp(triplet->rnic, lionic->path.sgid.raw, 16)) {
						found = true;
						break;
					}
				}
				else
				{
					if (!memcmp(triplet->rnic, lionic->port->gid.hw_gid.raw, 16)) {
						found = true;
						break;
					}
				}
			}
			if (unlikely(!found)) {
				format_gid_raw(triplet->rnic, lgid_buf);
				_NT(trace_5_ib_admin_channel_load_disk_nics, "OOPS: Controller rionic @LGID_BUF was not found in our list",
					lgid_buf);
				rv = -1;
				cont = false;
				break;
			}
			/* Triplet may have n_qps=0 in case:
			 *  1. rionic (srv's nic) is not rdda enabled.
			 *  2. rionic or lionic were not active during GET_IO.
			 */
			n_qps = be32_to_cpu(triplet->n_qps);
			if (n_qps == 0)
				n_trip_0_qs++;
		/* RDDA io_channels removed */
			//lionic->disk = disk;
		}
		nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
		if (!cont)
			goto out;
	}

	if (n_trip_0_qs == n_triplets)
		_NT(trace_6_ib_admin_channel_load_disk_nics,
			"all @N_TRIPLETS triplets have 0 qs", n_triplets);

out:
	__NFOUT;
	return rv;
}

struct vex_ach_get_jrange_decode_ctx {
	u32 rng_idx;
	u64 rng_slba;
	u32 rng_nlba;
	u32 n_ents;
	u64 rng_genid;
	binje_t rng_binje;
	DECLARE_BITMAP(free_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	union jblock_md jmdc_tbl[NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3];
	struct nvmeib_jrnl_ent_md ents_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
	u32 rng_nblk;
	u32 max_rng_blk;
	u32 tot_n_rng;
};

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_rsp_clnt_base_decode)
{
	struct vex_ach_get_jrange_decode_ctx *ctx = arg;
	const struct volume_server_get_jrange_rsp_base *rsp = wire_buf;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*rsp) > wire_buf_end);

	ctx->rng_idx = be32_to_cpu(rsp->rng_idx);
	ctx->rng_slba = be64_to_cpu(rsp->jrnl_lba);
	ctx->rng_nlba = be32_to_cpu(rsp->rng_nlba);

	nvmeib_bitmap_from_be32_ext(ctx->free_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE,
								rsp->non_free_ents_bmp, true);
	_ND(trace_nvmeibc_ib_admin_channel_c_2593, "non-free (BE32): @BITMAP512 (@HEX64) -> free @BITMAP512 (@HEX64)",
		rsp->non_free_ents_bmp, rsp->non_free_ents_bmp,
		ctx->free_ents_bmp, ctx->free_ents_bmp);
	memcpy(ctx->jmdc_tbl, rsp->jmdc_ents, sizeof(ctx->jmdc_tbl));

	return sizeof(*rsp);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_rsp_clnt_ext1_decode)
{
	struct vex_ach_get_jrange_decode_ctx *ctx = arg;
	const struct volume_server_get_jrange_rsp_ext1 *rsp = wire_buf;
	u32 n_free_ents;
	ssize_t rv;

	if (!wire_buf) {
		/* Fill Defaults */
		ctx->n_ents = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
		strcpy(ctx->serjio_boot_id, UUID_ZERO_STRING);
		rv = 0;
		goto out;
	}
	BUG_ON(wire_buf + sizeof(*rsp) > wire_buf_end);

	ctx->n_ents = be32_to_cpu(rsp->n_ents);
	ctx->rng_genid = be64_to_cpu(rsp->gen_id);
	memcpy(ctx->ents_md, rsp->ents_md, sizeof(ctx->ents_md));
	memcpy(ctx->serjio_boot_id, rsp->serjio_boot_id, NVMEIB_GID_STR_MAX);

	/* The n_free_ents is used to check the bitmap is valid */
	n_free_ents = be32_to_cpu(rsp->n_free_ents);
	BUG_ON(n_free_ents != bitmap_weight(ctx->free_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE));
	BUG_ON(n_free_ents > ctx->n_ents);
	rv = sizeof(*rsp);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_rsp_clnt_ext2_decode)
{
	struct vex_ach_get_jrange_decode_ctx *ctx = arg;
	const struct volume_server_get_jrange_rsp_ext2 *rsp = wire_buf;
	ssize_t rv;

	if (!wire_buf) {
		/* fill defaults */
		ctx->rng_binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		rv = 0;
		goto out;
	}

	BUG_ON(wire_buf + sizeof(*rsp) > wire_buf_end);
	ctx->rng_binje = be32_to_cpu(rsp->binje_rsp);
	rv = sizeof(*rsp);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_rsp_clnt_ext3_decode)
{
	struct vex_ach_get_jrange_decode_ctx *ctx = arg;
	const struct volume_server_get_jrange_rsp_ext3 *rsp = wire_buf;
	ssize_t rv;

	if (!wire_buf) {
		/* fill defaults */
		ctx->rng_nblk = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3;
		ctx->max_rng_blk = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3;
		ctx->tot_n_rng = NVMEIB_EC_MAX_JOURNAL_RANGES;
		rv = 0;
		goto out;
	}

	BUG_ON(wire_buf + sizeof(*rsp) > wire_buf_end);
	ctx->rng_nblk = be32_to_cpu(rsp->rng_nblk);
	ctx->max_rng_blk = be32_to_cpu(rsp->max_rng_blk);
	ctx->tot_n_rng = be32_to_cpu(rsp->tot_n_rng);
	rv = sizeof(*rsp);

	out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jmdc_rng_hdr_clnt_base_decode)
{
	const struct volume_server_get_jmdc_rng_data_base *rsp = wire_buf;
	struct nvmeib_get_jmdc_rng_data *res = arg;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*rsp) > wire_buf_end);

	res->client_uuid = rsp->client_uuid;
	res->rng_idx = be16_to_cpu(rsp->rng_idx);
	res->rng_start_lba = be64_to_cpu(rsp->rng_start_lba);
	res->rng_size_lba = be32_to_cpu(rsp->rng_size_lba);
	res->rng_gen_id = be64_to_cpu(rsp->rng_gen_id);

	res->only_dirty_ents = !!rsp->only_dirty_ents;
	res->rng_ent_offset = be32_to_cpu(rsp->rng_ent_offset);
	nvmeib_bitmap_from_be32(res->dirty_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE,
	                        rsp->dirty_ents_bmp);
	nvmeib_bitmap_from_be32(res->abnd_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE,
	                        rsp->abnd_ents_bmp);

	return sizeof(*rsp);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jmdc_rng_hdr_clnt_ext1_decode)
{
	const struct volume_server_get_jmdc_rng_data_ext1 *rsp = wire_buf;
	struct nvmeib_get_jmdc_rng_data *res = arg;
	ssize_t rv;

	if (!wire_buf) {
		res->binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		res->num_ents = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
		res->rng_ent_block_offset = res->rng_ent_offset * NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		res->num_dirty_ents = bitmap_weight(res->dirty_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
		rv = 0;
		goto out;
	}

	BUG_ON(wire_buf + sizeof(*rsp) > wire_buf_end);
	res->binje = be32_to_cpu(rsp->binje);
	res->num_ents = be16_to_cpu(rsp->num_ents);
	res->num_dirty_ents = be16_to_cpu(rsp->num_dirty_ents);
	res->rng_ent_block_offset = be32_to_cpu(rsp->rng_ent_block_offset);
	rv = sizeof(*rsp);
out:
	return rv;
}

void nvmeibc_get_jmdc_deserialize_rng_data(void *rng_data, size_t rng_data_sz,
    struct nvmeibc_disk_jmdc_read_comp *comp,
    struct nvmeibc_ib_admin_channel *ch) {
	unsigned int i;
	ssize_t rv;

	for (i = 0; i < comp->rsp.read_jrnl_data->num_rng; ++i) {
		rv = CALL_VEX_OP(decode,
				vex_ach_get_jmdc_rng_hdr_clnt_ops, ONE_EXT,
					base, vex_ach_get_jmdc_rng_hdr_clnt_base_decode,
					ext1, vex_ach_get_jmdc_rng_hdr_clnt_ext1_decode,
						ch->base.vex_ops[vex_ach_get_jmdc_rng_hdr],
						rng_data, rng_data + rng_data_sz,
						&comp->rsp.rng_data[i]);
		BUG_ON(rv <= 0);
		rng_data += rv;
	}
}

// the payload is:
// 4 bytes (signed): range index. If is  (u32)~0, then there is no available journal range
// 8 bytes: journal lba
// 4 bytes: sector shift (in bits)
// 4 bytes: journal length (in blocks)
// 16 bytes: abandoned_entries big-endian bit-map (0 = free, !0 = abandoned)
static int parse_jrange_msg(struct nvmeibc_ib_admin_channel *ch,
			    struct nvmeibc_disk *disk,
			    union jblock_md *ext_jmdc_tbl)
{
	int rv = 0;
	struct vex_ach_get_jrange_decode_ctx *ctx;
	__NFIN;

	nvmeib_mem_sync_map_for_cpu(&ch->msg_area_map, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);

	if (!(ctx = kzalloc(sizeof(*ctx), GFP_KERNEL))) {
		_NE(parse_jrange_msg_e1, "OOM Error");
		rv = -ENOMEM;
		goto out;
	}

	if ((rv = CALL_VEX_OP(decode,
			vex_ach_get_jrange_rsp_clnt_ops, THREE_EXT,
				base, vex_ach_get_jrange_rsp_clnt_base_decode,
				ext1, vex_ach_get_jrange_rsp_clnt_ext1_decode,
				ext2, vex_ach_get_jrange_rsp_clnt_ext2_decode,
				ext3, vex_ach_get_jrange_rsp_clnt_ext3_decode,
					ch->base.vex_ops[vex_ach_get_jrange_rsp],
					ch->msg_area, ch->msg_area_end, ctx)) < 0) {
		goto out;
	}

	if ((disk->jour.rng_id = ctx->rng_idx) ==
		(u32)NVMEIB_EC_INVALID_JOURNAL_RANGE) {
		_NE(error_ib_admin_channel_parse_jrange_msg, "No journal-range assigned to disk @DISK_NAME", disk->name);
		rv = -ENOENT;
		goto out;
	}

	if (!ctx->n_ents || ctx->n_ents > NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) {
		_NE(parse_jrange_msg_e100,
			"Invalid journal range entries @UINT for disk @STR",
			ctx->n_ents, disk->name);
		rv = -EINVAL;
		goto out;
	}
	if (ctx->n_ents * ctx->rng_binje > ctx->rng_nlba) {
		_NE(parse_jrange_msg_e101,
			"Invalid journal range LBAs @NLBA < entries @N_ENTS * binje @BINJE for disk @STR",
			ctx->rng_nlba, ctx->n_ents, ctx->rng_binje, disk->name);
		rv = -EINVAL;
		goto out;
	}
	disk->jour.rng_gen_id = ctx->rng_genid;
	disk->jour.rng_slba = ctx->rng_slba;
	disk->jour.rng_nlba = ctx->rng_nlba;
	disk->jour.rng_binje = ctx->rng_binje;
	memcpy(disk->jour.serjio_boot_id, ctx->serjio_boot_id, NVMEIB_GID_STR_MAX);
	disk->jour.n_ents = ctx->n_ents;
	disk->jour.rng_nblk = ctx->rng_nblk;
	disk->jour.max_rng_blk = ctx->max_rng_blk;
	disk->jour.tot_n_rng = ctx->tot_n_rng;

	_NT(trace_ib_admin_channel_parse_jrange_msg,
		"journal-info: rng: id=@RNG_ID, slba=@SLBA_LLONG, nlba=@NLBA "
		"gen_id=@JRNL_RNG_GEN_ID, binje=@BINJE(@BINJE), n_ents=@N_ENTS, "
		"rng_nblk=@NBLOCKS, max_rng_blk=@NBLOCKS, tot_n_rng=@NUM_RANGES, serjio_boot_id=@STR, "
		"free_ents_bmp=" NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE "\n",
		ctx->rng_idx, ctx->rng_slba, ctx->rng_nlba, ctx->rng_genid,
		ctx->rng_binje, disk->binje_ulp, ctx->n_ents, ctx->rng_nblk,
		ctx->max_rng_blk, ctx->tot_n_rng, ctx->serjio_boot_id, ctx->free_ents_bmp);

	/* In 2.0 @free_ents_bmp meaning was toggled.
	   If 2.0 clnt parse map received from 1.3-srv,
	   it will find it conflicting with the jmdc[] values */
	if (disk->last_tgt_link_ver.product.all <= nvmeib_13_product_version.all) {
		_NT(trace_ib_ac_parse_jrange_msg_old_ver, "Old target version: " NVMEIB_VERSION_TRACE_FMT()
		"for disk @DISK_NAME. Not adding journal to JAM", NVMEIB_VERSION_PRINT_ARG(&disk->last_tgt_link_ver), disk->name);
		/* do not fail discover so ulp can do recovery.
		   when srv upgrades too, it will trigger rediscover */
		goto out;
	}

	if ((rv = nvmeibc_jam_disk_add(disk,
					ctx->free_ents_bmp,
					ext_jmdc_tbl ? ext_jmdc_tbl : ctx->jmdc_tbl,
					ctx->ents_md)) < 0) {
		_NT(trace_1_ib_admin_channel_parse_jrange_msg,
			"Fail to add disk @DISK_NAME to jam", disk->name);
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_JAM_ADD_FAILED);
	}

out:
	kfree(ctx);
	__NFOUT;
	return rv;
}

struct get_lock_gids_rsp_ctx {
	struct nvmeibc_ib_admin_channel *ach;
	int n_gids;
	struct list_head gid_list_head;
	struct get_lock_gids_rsp_ctx_gid {
		union ib_gid gid;
		int atomic_ops;
		enum rdma_link_layer link_layer;
		enum rdma_transport_type transport_type;
		unsigned int priority;
		struct list_head link;
		unsigned tcp_base_port;
		unsigned tcp_num_ports;
	} *ib_gids;
};

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_lock_gids_rsp_clnt_base_decode)
{
	const struct wire_lock_gid_base *lock_gid = wire_buf;
	struct get_lock_gids_rsp_ctx *ctx = arg;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*lock_gid) > wire_buf_end);

	_NT(vex_ach_get_lock_gids_rsp_clnt_base_decode_t1,
		"Received @INT lock rgids", n_elem);
	if (elem_idx == 0) {
		_NT(vex_ach_get_lock_gids_rsp_clnt_base_decode_t2,
			"Received 0 lock rgids");
		ctx->n_gids = n_elem;
		if (!(ctx->ib_gids = kcalloc(n_elem, sizeof(*ctx->ib_gids), GFP_KERNEL))) {
			_NE(vex_ach_get_lock_gids_rsp_clnt_base_decode_e1,
				"Failed to allocate memory for @INT gids", n_elem);
			rv = -ENOMEM;
			goto out;
		}
		INIT_LIST_HEAD(&ctx->gid_list_head);
	}

	_ND(vex_ach_get_lock_gids_rsp_clnt_base_decode_d1,
		"List @INT optional lock rgids:", n_elem);
	memcpy(ctx->ib_gids[elem_idx].gid.raw, lock_gid->gid.raw, 16);
	_NT(trace_3_ib_admin_channel_parse_read_lock_gids, "lock rgid #@INDEX @GID_IPV6",
		elem_idx, &ctx->ib_gids[elem_idx].gid);
	ctx->ib_gids[elem_idx].atomic_ops = be32_to_cpu(lock_gid->atomic_ops);
	list_add_tail(&ctx->ib_gids[elem_idx].link, &ctx->gid_list_head); /* we later sort the list by prio (see lock_gid_cmp_fn) so we first attemp to connect preferred gids */

	rv = sizeof(*lock_gid);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_lock_gids_rsp_clnt_ext1_decode)
{
	const struct wire_lock_gid_ext1 *ext1 = wire_buf;
	struct get_lock_gids_rsp_ctx *ctx = arg;
	struct nvmeibc_ib_admin_channel *ach = ctx->ach;

	if (!wire_buf) {
		/* Assume all NICs use the same link_layer / transport as the admin */
		ctx->ib_gids[elem_idx].link_layer = ach->net.base.port->layer;
		ctx->ib_gids[elem_idx].transport_type =ach->net.base.port->transport_type;
		ctx->ib_gids[elem_idx].priority = 0;
		return 0;
	}

	ctx->ib_gids[elem_idx].link_layer = ext1->link_layer;
	ctx->ib_gids[elem_idx].transport_type = ext1->transport_type;
	ctx->ib_gids[elem_idx].priority = be32_to_cpu(ext1->priority);

	_NT(trace_ib_admin_channel_vex_ach_get_lock_gids_rsp_clnt_ext1_decode,
			"lock rgid #@INDEX @GID_IPV6 link_layer @LAYER transport_type @TRANSPORT_TYPE priority @PRIORITY",
			elem_idx, &ctx->ib_gids[elem_idx].gid,
			ctx->ib_gids[elem_idx].link_layer, ctx->ib_gids[elem_idx].transport_type, ctx->ib_gids[elem_idx].priority);

	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	return sizeof(*ext1);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_lock_gids_rsp_clnt_ext2_decode)
{
	const struct wire_lock_gid_ext2 *ext2 = wire_buf;
	struct get_lock_gids_rsp_ctx *ctx = arg;

	if (!wire_buf) {
		/* Use defaults */
		ctx->ib_gids[elem_idx].tcp_base_port = nvmeib_get_tcp_base_port_id();
		ctx->ib_gids[elem_idx].tcp_num_ports = nvmeib_get_tcp_num_ports(NULL);
		return 0;
	}

	ctx->ib_gids[elem_idx].tcp_base_port = be16_to_cpu(ext2->tcp_base_port);
	ctx->ib_gids[elem_idx].tcp_num_ports = be16_to_cpu(ext2->tcp_num_ports);

	_NT(trace_ib_admin_channel_vex_ach_get_lock_gids_rsp_clnt_ext2_decode,
		"lock rgid #@INDEX @GID_IPV6 TCP Ports [@START_PORT, @END_PORT)",
		elem_idx, &ctx->ib_gids[elem_idx].gid,
		ctx->ib_gids[elem_idx].tcp_base_port,
		ctx->ib_gids[elem_idx].tcp_base_port + ctx->ib_gids[elem_idx].tcp_num_ports - 1);

	BUG_ON(wire_buf + sizeof(*ext2) > wire_buf_end);

	return sizeof(*ext2);
}

#if KS_LIST_SORT_CMP_FUNC_T
static int lock_gid_cmp_fn(void *priv, const struct list_head *a, const struct list_head *b)
#else
static int lock_gid_cmp_fn(void *priv, struct list_head *a, struct list_head *b)
#endif
{
	struct get_lock_gids_rsp_ctx_gid *gid_a = container_of(a, struct get_lock_gids_rsp_ctx_gid, link);
	struct get_lock_gids_rsp_ctx_gid *gid_b = container_of(b, struct get_lock_gids_rsp_ctx_gid, link);

	/* Put the lowest priority at the head of the list */
	if (gid_a->priority < gid_b->priority)
		return -1;
	else if (gid_a->priority > gid_b->priority)
		return 1;
	return 0;
}

static inline int matching_ipv4_prefix_length(const union ib_gid *gid1, const union ib_gid *gid2) {
    int match_bits = 0;
	u32 ipv4_gid1, ipv4_gid2, ipv4_diff;

	/* verify ipv4 mapped address */
	BUG_ON(!ipv6_addr_v4mapped((struct in6_addr *)gid1));
	BUG_ON(!ipv6_addr_v4mapped((struct in6_addr *)gid2));
	ipv4_gid1 = ntohl(((u32*)gid1)[3]);
	ipv4_gid2 = ntohl(((u32*)gid2)[3]);
	ipv4_diff = ipv4_gid1 ^ ipv4_gid2;
	match_bits = 32 - fls(ipv4_diff);
	_NT(matching_ipv4_ret, "Matching bits = @INT", match_bits);

    return match_bits;
}

struct local_nic_lock_gid_path {
	struct nvmeibc_local_nic *ln;
	struct nvmeibc_local_nic_port *lport;
	struct get_lock_gids_rsp_ctx_gid *lock_gid;
	int rgid_idx;
	uint32_t priority_group;
	struct list_head link;
};


extern int nvmeibc_disk_prefix_priority_masks_len;
extern unsigned int nvmeibc_disk_prefix_priority_masks[16];

static inline uint32_t matching_suffix_len_to_group(int len)
{
	size_t i;

	for (i=0; i<nvmeibc_disk_prefix_priority_masks_len; i++) {
		if (len < nvmeibc_disk_prefix_priority_masks[i]) {
			break;
		}
	}

	/* Higher priority means the smallest number */
	return nvmeibc_disk_prefix_priority_masks_len - i;
}

static int prefix_priority_masks_cmp_fn(void *priv, struct list_head *a, struct list_head *b)
{
	struct local_nic_lock_gid_path *gid_path_a = container_of(a, struct local_nic_lock_gid_path, link);
	struct local_nic_lock_gid_path *gid_path_b = container_of(b, struct local_nic_lock_gid_path, link);
	int gid_path_a_priority = gid_path_a->priority_group;
	int gid_path_b_priority = gid_path_b->priority_group;

	if (gid_path_a_priority < gid_path_b_priority)
		return -1;
	else if (gid_path_a_priority == gid_path_b_priority)
		return 0;
	return 1;
}

static struct nvmeibc_locks_channel * connect_lock_channels_by_prefix_priority_masks(struct get_lock_gids_rsp_ctx *ctx) {
	struct nvmeibc_disk *disk = ctx->ach->base.base.disk;
	size_t lock_paths_len = ctx->n_gids * disk->num_lnics; /* Should we assume 1 port per nic? */
	struct local_nic_lock_gid_path *lock_paths = NULL;
	struct list_head sorted_paths;
	struct get_lock_gids_rsp_ctx_gid *gid_iter;
	struct nvmeibc_local_nic *ln;
	struct local_nic_lock_gid_path *lock_path_iter;
	int i = 0, j;
	struct nvmeibc_locks_channel *lock_ch = NULL;
	struct nvmeibc_local_nic_port *lport;

	if (!(lock_paths = kmalloc(sizeof(struct local_nic_lock_gid_path) * lock_paths_len, GFP_KERNEL))) {
		_NW(oom_locks_paths, "Could not kmalloc locks_paths");
		goto out;
	}

	INIT_LIST_HEAD(&sorted_paths);

	list_for_each_entry(ln, &disk->local_nics, link) {
		list_for_each_entry(lport, &ln->ports, link) {
			j = 0;
			list_for_each_entry(gid_iter, &ctx->gid_list_head, link) {
				lock_paths[i].ln = ln;
				lock_paths[i].lport = lport;
				lock_paths[i].lock_gid = gid_iter;
				lock_paths[i].rgid_idx = j;
				lock_paths[i].priority_group = matching_suffix_len_to_group(matching_ipv4_prefix_length(&lport->ib_port->gid.gid, &gid_iter->gid));
				_NT(prioriry_match_trace, "Path @GID_IPV6 -> @GID_IPV6 will be in priority group @COMMON_PREFIX_PRIORITY", &lport->ib_port->gid.gid, &gid_iter->gid, lock_paths[i].priority_group);
				prio_list_add_tail(&lock_paths[i].link, &sorted_paths, prefix_priority_masks_cmp_fn, NULL);
				i++;
				j++;
			}
		}
	}

	list_for_each_entry(lock_path_iter, &sorted_paths, link) {
		_NT(connect_by_path_trace, "Trying to connect @GID_IPV6 -> @GID_IPV6 as it's priority @COMMON_PREFIX_PRIORITY", &lock_path_iter->lport->ib_port->gid.gid, &lock_path_iter->lock_gid->gid, lock_path_iter->priority_group);
		if ((lock_ch = nvmeibc_locks_channel_connect_lock_by_path(&ctx->ach->base, lock_path_iter->lport,
				&lock_path_iter->lock_gid->gid, lock_path_iter->lock_gid->atomic_ops,
				lock_path_iter->lock_gid->link_layer, lock_path_iter->lock_gid->transport_type, lock_path_iter->rgid_idx,
				lock_path_iter->lock_gid->tcp_base_port, lock_path_iter->lock_gid->tcp_num_ports)))
				{
					disk->current_common_prefix_path.sgid = lock_path_iter->lport->ib_port->gid.gid;
					disk->current_common_prefix_path.dgid = lock_path_iter->lock_gid->gid;
					disk->current_common_prefix_path.priority = lock_path_iter->priority_group;
					goto out;
				}
	}

out:
	kfree(lock_paths);
	return lock_ch;
}

/* read the server response for the optional remote gids */
static struct nvmeibc_locks_channel *parse_read_lock_gids(
	struct nvmeibc_ib_admin_channel *ch)
{
	struct nvmeibc_locks_channel *lock_ch = NULL;
	struct get_lock_gids_rsp_ctx ctx = {
		.ach = ch,
	};
	struct get_lock_gids_rsp_ctx_gid *gid_iter;
	int i = 0;
	ssize_t rv;
	struct nvmeibc_disk *disk  = ch->base.base.disk;

	__NFIN;
	nvmeib_mem_sync_map_for_cpu(&ch->msg_area_map, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);

	rv = CALL_VEX_OP(decode_container,
			vex_ach_get_lock_gids_rsp_clnt_ops, TWO_EXT,
				base, vex_ach_get_lock_gids_rsp_clnt_base_decode,
				ext1, vex_ach_get_lock_gids_rsp_clnt_ext1_decode,
				ext2, vex_ach_get_lock_gids_rsp_clnt_ext2_decode,
					ch->base.vex_ops[vex_ach_get_lock_gids_rsp],
						ch->msg_area, ch->msg_area_end, &ctx);
	if (rv < 0)
		goto out;

	/*
	 * TODO: EC-4834
	 * server in mixed mode compiles lock GID list
	 * with duplicate entries
	 */

	/* Sort ib_gids list by priority */
	if (!nvmeibc_disk_prefix_priority_masks_len) {
		list_sort(&ctx, &ctx.gid_list_head, lock_gid_cmp_fn);
		list_for_each_entry(gid_iter, &ctx.gid_list_head, link) {
			if ((gid_iter->transport_type != ch->net.base.port->transport_type ||
					gid_iter->link_layer != ch->net.base.port->layer) &&
					!NVMEIBC_USE_BOTH_ROCE_AND_TCP_LOCK_CH) {
				_NT(trace_5_ib_admin_channel_parse_read_lock_gids,
						"Skipping lock rgid @GID_IPV6, link_layer @LINK_LAYER, transport_type @TRANSPORT_TYPE does not match admin",
						&gid_iter->gid, gid_iter->link_layer, gid_iter->transport_type);
				continue;
			}

			_NT(trace_4_ib_admin_channel_parse_read_lock_gids,
					"Try to connect lock-ch, rgid [@INT32_02] @GID_IPV6, link_layer @LINK_LAYER, transport_type @TRANSPORT_TYPE, priority @PRIORITY",
					i, &gid_iter->gid, gid_iter->link_layer, gid_iter->transport_type, gid_iter->priority);
			if ((lock_ch = nvmeibc_locks_channel_connect_locks(&ch->base,
				&gid_iter->gid, gid_iter->atomic_ops,
				gid_iter->link_layer, gid_iter->transport_type, i,
				gid_iter->tcp_base_port, gid_iter->tcp_num_ports))) {
				goto out;
			}
			i++;
		}
	} else {
		lock_ch = connect_lock_channels_by_prefix_priority_masks(&ctx);
	}

out:
	if (!lock_ch)
		DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_LOCK_CHANNEL_CONNECTION_FAILED);
	kfree(ctx.ib_gids);
	__NFOUT;
	return lock_ch;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_lock_mem_seg_info_clnt_base_decode)
{
	const struct volume_server_config_per_segment_lock_info_base *base = wire_buf;
	struct nvmeibc_disk_seg_locks_mem_info *dlsi = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	dlsi->seg_id = be32_to_cpu(base->seg_id);
	dlsi->len = be32_to_cpu(base->len);
	dlsi->lock_set_size = be64_to_cpu(base->lock_set_size);
	dlsi->dirty_bit_offset = ((dlsi->len + (dlsi->lock_set_size -1)) / dlsi->lock_set_size) * sizeof(u64);
	dlsi->rkey = be32_to_cpu(base->rkey);
	dlsi->lkey = be32_to_cpu(base->lkey);
	dlsi->addr = be64_to_cpu(base->addr);
	dlsi->start_addr = be64_to_cpu(base->start_addr);
	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_lock_mem_seg_info_clnt_ext1_decode)
{
	const struct volume_server_config_per_segment_lock_info_ext1 *ext1 = wire_buf;
	struct nvmeibc_disk_seg_locks_mem_info *dlsi = arg;

	if (!ext1) {
		/* Not-present -> Defaults */
		dlsi->lmi = 0;
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	dlsi->lmi = cpu_to_be64(ext1->lmi);
	return sizeof(*ext1);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_lock_mem_seg_info_clnt_ext2_decode)
{
	const struct volume_server_config_per_segment_lock_info_ext2 *ext2 = wire_buf;
	struct nvmeibc_disk_seg_locks_mem_info *dlsi = arg;

	if (!ext2) {
		/* Not-present -> Defaults */
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext2) > wire_buf_end);

	dlsi->len = be64_to_cpu(ext2->len64);
	return sizeof(*ext2);
}

static int parse_read_lock_mems_msg(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeib_iu **recv_ioctx, u64 tag)
{
	int rv = 0, i;
	struct volume_server_rsp *rsp;
	struct nvmeibc_disk *disk = ch->base.base.disk;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;
	struct nvmeibc_disk_seg_locks_mem_info *dlsi;

	__NFIN;
	disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
												  (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1, .dont_wait = 0 });
	_ND(trace_ib_admin_channel_parse_read_lock_mems_msg, "disk_segs_locks=@DISK_SEGS_LOCKS", disk_segs_locks);
	disk_segs_locks->locks = kzalloc(disk_segs_locks->num_of_segments *
		sizeof(*disk_segs_locks->locks), GFP_KERNEL);
	_ND(trace_1_ib_admin_channel_parse_read_lock_mems_msg, "disk_segs_locks=@DISK_SEGS_LOCKS, locks=@LOCKS", disk_segs_locks,
		disk_segs_locks->locks);
	if (!disk_segs_locks->locks) {
		_NE(error_ib_admin_channel_parse_read_lock_mems_msg, "locks allocation failed");
		rv = -ENOMEM;
		goto out;
	}

	_ND(trace_2_ib_admin_channel_parse_read_lock_mems_msg, "Number of segments: @NUM_OF_SEGMENTS", disk_segs_locks->num_of_segments);

	for (i = 0; i < disk_segs_locks->num_of_segments; ++i) {
		if (!*recv_ioctx && !(*recv_ioctx = get_recv_msg(ch, tag))) {
			_NT(trace_3_ib_admin_channel_parse_read_lock_mems_msg, "Fail to receive segment information");
			rv = -EAGAIN;
			goto out_err;
		}
		rsp = (*recv_ioctx)->buf;
		dlsi = &disk_segs_locks->locks[i];
		dlsi->disk = disk;

		if ((rv = CALL_VEX_OP(decode,
				vex_ach_lock_mem_seg_info_clnt_ops, TWO_EXT,
					base, vex_ach_lock_mem_seg_info_clnt_base_decode,
					ext1, vex_ach_lock_mem_seg_info_clnt_ext1_decode,
					ext2, vex_ach_lock_mem_seg_info_clnt_ext2_decode,
						ch->base.vex_ops[vex_ach_lock_mem_seg_info],
						rsp->payload, rsp->ach_payload_max,
						dlsi)) < 0)
		{
			_NE(error_3_client_send_disk_lock_mems, "Failed (@RV) decoding lock mem seg info", rv);
			rv = -1;
			goto out;
		}

		nvmeibc_ib_admin_channel_put_rx_iu(ch, *recv_ioctx);
		*recv_ioctx = NULL;
		dlsi->locks_channel = disk_segs_locks->lock_ch;
		_NT(trace_4_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: disk lock information at client:");
		_NT(trace_5_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: Record = @DISK_SEGS_LOCKS", disk_segs_locks);
		_NT(trace_6_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: channel = @LOCK_CH", disk_segs_locks->lock_ch);
		_NT(trace_7_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: seg_id = @SEG_ID_INT", dlsi->seg_id);
		_NT(trace_8_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: len = @LEN", dlsi->len);
		_NT(trace_9_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: lock_set_size = @LOCK_SET_SIZE", dlsi->lock_set_size);
		_NT(trace_10_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: start_addr = @START_ADDR", dlsi->start_addr);
		_NT(trace_11_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: address = @ADDR", dlsi->addr);
		_NT(trace_12_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: rkey = @RKEY", dlsi->rkey);
		_NT(trace_13_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: dirty_bit_offset=@DIRTY_BIT_OFFSET", dlsi->dirty_bit_offset);
		_NT(trace_14_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: lmi=@LMI", (const void *)dlsi->lmi);
	}

	if (!ch->lsi) {
		/* If disk has no RDDA resources, then we will not get the lock seg info
		 * as part of the access map.
		 * We can create it from the the response here instead. */
		if (!(ch->lsi = kcalloc(disk_segs_locks->num_of_segments, sizeof(*ch->lsi), GFP_KERNEL))) {
			_NT(trace_23_ib_admin_channel_parse_read_lock_mems_msg,
			    "Failed to allocate memory for lsi");
			rv = -ENOMEM;
			goto out_err;
		}

		for (i = 0, dlsi = disk_segs_locks->locks; i < disk_segs_locks->num_of_segments; i++, dlsi++) {
			if (!dlsi->lmi) {
				_NT(trace_24_ib_admin_channel_parse_read_lock_mems_msg,
					"Missing lmi for disk @DISK_NAME disk_segs_locks @DISK_SEGS_LOCKS seg_id @SEG_ID_INT",
					disk->name, disk_segs_locks, dlsi->seg_id);
				rv = -EINVAL;
				goto out_err;
			}
			ch->lsi_alloc = true;
			ch->lsi[i].seg_id = dlsi->seg_id;
			ch->lsi[i].len = dlsi->len;
			ch->lsi[i].lockset_size = dlsi->lock_set_size;
			ch->lsi[i].rkey = dlsi->rkey;
			ch->lsi[i].lkey = dlsi->lkey;
			ch->lsi[i].ioaddr = dlsi->addr;
			ch->lsi[i].start_disk_address = dlsi->start_addr;
			ch->lsi[i].lmi = dlsi->lmi;
		}
	}

	if (!*recv_ioctx && !(*recv_ioctx = get_recv_msg(ch, tag))) {	// NVMESH-4574 active-locks-deprecation
			_NT(trace_25_ib_admin_channel_parse_read_lock_mems_msg, "Fail to receive deprecated data");
			rv = -EAGAIN;
			goto out_err;
	}
	nvmeibc_ib_admin_channel_put_rx_iu(ch, *recv_ioctx);
	*recv_ioctx = NULL;
	_NT(trace_22_ib_admin_channel_parse_read_lock_mems_msg, "LOCKS: will try to attach used segments");
	nvmeibc_disk_locks_attach_used_segments(disk, disk_segs_locks);
	goto out;

out_err:
	if (disk_segs_locks->locks) {
		kfree(disk_segs_locks->locks);
		disk_segs_locks->locks = NULL;
	}
out:
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1 });
	__NFOUT;
	return rv;
}

static int load_disk_lock_info(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	struct nvmeibc_disk *disk)
{
	int rv = 0;
	struct nvmeib_iu *recv_ioctx = NULL;
	struct volume_server_rsp *rsp;
	struct volume_server_config_per_disk_locks_rsp *per_disk_lock_rsp;
	struct nvmeibc_disk_segments_locks *disk_segs_locks;
	struct nvmeibc_locks_channel *lock_ch;
	u32 num_of_segments;

	__NFIN;
	if (!disk || !disk->info) {
		_NT(trace_ib_admin_channel_load_disk_lock_info, "uninitialized disk aborting");
		rv = -EAGAIN;
		goto out;
	}
	if (!(recv_ioctx = get_recv_msg(ch, tag))) {
		_NT(trace_1_ib_admin_channel_load_disk_lock_info, "Fail to num of segments");
		rv = -EAGAIN;
		goto out;
	}
	BUG_ON(!(disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
														   (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1, .dont_wait = 0 })));
	rsp = recv_ioctx->buf;
	per_disk_lock_rsp = &rsp->per_disk_lock_rsp;
	_ND(trace_2_ib_admin_channel_load_disk_lock_info, "LOCKS: great got the number of locks: @MEMSEGS disk name @DISK_NAME",
		be32_to_cpu(per_disk_lock_rsp->num_of_memsegs),
		per_disk_lock_rsp->disk_name);
	_ND(trace_3_ib_admin_channel_load_disk_lock_info, "LOCKS: before reading num of segments the number is @NUM_OF_SEGMENTS",
		disk_segs_locks->num_of_segments);
	if (disk_segs_locks->num_of_segments) {
		if (disk_segs_locks->locks)
			kfree(disk_segs_locks->locks);
		disk_segs_locks->num_of_segments = 0;
		disk_segs_locks->locks = NULL;
	}
	disk_segs_locks->num_of_segments = num_of_segments =
		be32_to_cpu(per_disk_lock_rsp->num_of_memsegs);
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1 });

	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

	_ND(trace_4_ib_admin_channel_load_disk_lock_info, "LOCKS: the number of segments are @NUM_OF_SEGMENTS",
		num_of_segments);
	if (!num_of_segments) {
		_NT(trace_5_ib_admin_channel_load_disk_lock_info, "Disk @DISK_NAME with no segments - bailing out", disk->name);
		rv = -1;
		goto out;
	}

	if (!(recv_ioctx = get_recv_msg(ch, tag))) {
		_NT(trace_6_ib_admin_channel_load_disk_lock_info, "Fail to get lock devices");
		rv = -EAGAIN;
		goto out;
	}

	lock_ch = parse_read_lock_gids(ch); /* #link-lock-ch (remote) from disk->info->ch->segments_locks.lock_ch */
	if (lock_ch == NULL) {
		_NT(trace_7_ib_admin_channel_load_disk_lock_info, "Failed to locate lock channel");
		rv = -ENODEV;
		goto put_back;
	}
	BUG_ON(!(disk_segs_locks = nvmeibc_disk_get_segs_locks(disk,
														   (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1, .dont_wait = 0 })));
	disk_segs_locks->lock_ch = lock_ch;
	disk_segs_locks->lock_ch->base.disk = disk;
	nvmeibc_disk_put_segs_locks(disk_segs_locks, (struct nvmeibc_disk_get_segs_locks_flags){ .write = 1 });

	if (!nvmeibc_locks_channel_all_2nd_connected(lock_ch)) {
		rv = -1;
		goto put_back;
	}

put_back:
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

out:
	__NFOUT;
	return rv;
}

static noinline int load_disk_rscs(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	struct nvmeibc_disk *disk)
{
	struct nvmeib_iu *recv_ioctx = NULL;
	struct volume_server_rsp *rsp;
	struct volume_server_config_access_map_per_disk_d_rsp *per_disk_d_rsp;
	struct nvmeibs_disk_description *d;
	struct nvmeibs_disk_seg_lock_info *e;
	int i, j, k, m, n, p, q, rv = 0, ii, n_ports;
	char gid_str[GUID_SIZE] = {0};
	bool cont = true;

	__NFIN;
	n = DIV_ROUND_UP(disk->info->n_rscs, VOLUME_SERVER_MAX_ARRAY_SIZE);
	q = DIV_ROUND_UP(disk->info->n_disk_lock_segments,
		VOLUME_SERVER_MAX_ARRAY_SIZE);
	_NT(load_disk_rscs_t1,
		"disk->info->n_rscs_sets=@INT,n=@INT,q=@INT",
		disk->info->n_rscs_sets, n, q);
	for (p = 0; p < disk->info->n_rscs_sets; ++p) {
		/* read the port info - we must have it even if n==0 for the
		   admin channel locking...
		*/
		if (!(recv_ioctx = get_recv_msg(ch, tag))) {
			_NT(load_disk_rscs_t100, "Fail to receive disk D part");
			rv = -1;
			goto out;
		}
		rsp = recv_ioctx->buf;
		per_disk_d_rsp = &rsp->per_disk_d_rsp;
		/* save the hca info of the current disk resources */
		memcpy(disk->info->hcaa[p].ports, per_disk_d_rsp->ports,
			sizeof(disk->info->hcaa[p].ports));
		n_ports = be32_to_cpu(per_disk_d_rsp->n_ports);
		disk->info->hcaa[p].n_ports = n_ports;

		_NT(load_disk_rscs_t101,
			"N_Ports=@INT @INT", disk->info->hcaa[p].n_ports, n_ports);
		for (ii = 0; ii < disk->info->hcaa[p].n_ports; ++ii) {
			format_gid_raw(disk->info->hcaa[p].ports[ii].raw, gid_str);
			_ND(load_disk_rscs_d100, "port @INT @STR", ii, gid_str);
		}
		nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
		_ND(load_disk_rscs_d101,
			"Resource set @INT for disk @STR", p, disk->name);
		j = 0;
		for (i = 0; i < n; ++i) {
			if (!(recv_ioctx = get_recv_msg(ch, tag))) {
				_NT(load_disk_rscs_t103, "Fail to receive disk D part");
				rv = -1;
				goto out;
			}
			rsp = recv_ioctx->buf;
			per_disk_d_rsp = &rsp->per_disk_d_rsp;
			k = min(disk->info->n_rscs - j, VOLUME_SERVER_MAX_ARRAY_SIZE);
			for (m = 0; m < k && cont; ++m, ++j) {
				d = &per_disk_d_rsp->a[m];
				if ((rv = nvmeibc_disk_add_rsc(ch, disk, d, j, p)) < 0) {
					_NT(load_disk_rscs_t105, "Fail to read disk resources");
					cont = false;
				}
			}
			nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
			if (!cont)
				goto out;
		}
		j = 0;
		for (i = 0; i < q; ++i) {
			if (!(recv_ioctx = get_recv_msg(ch, tag))) {
				_NT(load_disk_rscs_t106, "Fail to receive disk D part");
				rv = -1;
				goto out;
			}
			rsp = recv_ioctx->buf;
			per_disk_d_rsp = &rsp->per_disk_d_rsp;
			k = min(disk->info->n_disk_lock_segments - j,
				VOLUME_SERVER_MAX_ARRAY_SIZE);
			for (m = 0; m < k && cont; ++m, ++j) {
				e = &per_disk_d_rsp->b[m];
				if ((rv = nvmeibc_disk_add_lock_rsc(ch, disk, e, j, p)) < 0) {
					_NT(load_disk_rscs_t108, "Fail to read disk resources");
					cont = false;
				}
			}
			nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
			if (!cont)
				goto out;
		}
		_ND(load_disk_rscs_d30,
			"Done with resource set @INT for disk @STR", p, disk->name);
	}
	/* in non-RDDA we would like to use the piggyback read lock.  for that
	   we need the rdma info of the remote nic that the admin channel
	   is pair against
	*/
	ch->lsi = NULL;
	for (p = 0; p < disk->info->n_rscs_sets; ++p) {
		for (i = 0; i < disk->info->hcaa[p].n_ports; ++i) {
			format_gid_raw(disk->info->hcaa[p].ports[i].raw, gid_str);
			_ND(load_disk_rscs_d40,
				"[p=@INT32_02, i=@INT32_02] compare to @STR", p, i, gid_str);
			if (!memcmp(ch->net.base.path.dgid.raw,
				disk->info->hcaa[p].ports[i].raw,
				sizeof(ch->net.base.path.dgid.raw))) {
				ch->lsi = disk->info->hcaa[p].lsi;
				break;
			}
		}
	}
	if (ch->base.vex_ops[vex_ach_lock_mem_seg_info] == vex_base && !ch->lsi) {
		format_gid_raw(ch->net.base.path.dgid.raw, gid_str);
		_NT(load_disk_rscs_d30_t37,
			"Could not find remote lock-gid @STR", gid_str);
		rv = -1;
		goto out;
	}

out:
	__NFOUT;
	return rv;
}

/* currently the info on which we decide whether to add nordda-rionic
   is based only on the nics-info part of the the GET-IO rsp from srv.
   In the future we'll get more info from srv over access-map rsp for
   adding nordda-rionic */
static int load_disk_nordda(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	struct nvmeibc_disk *disk)
{
	struct list_head *rionics = &disk->rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *lionics;
	struct nvmeibc_io_lnic *lionic = NULL;
	int clnt_max_nrchs_per_path_rdma = NVMEIB_NR_GET_MAX_CHANNELS_PER_PATH(nvmeibc_iommu_enabled ? nvmeibc_nr_max_channels_per_path_iommu : nvmeibc_nr_max_channels_per_path);
	int clnt_max_nrchs_per_path_tcp = nvmeibc_nr_max_channels_per_path_tcp;
	int p = 0, np = 0;
	int rv = -1;
	__NFIN;

	disk->nr_np_head = NULL;
	BUG_ON(disk->n_nr_rionics != 0);
	list_for_each_entry(rionic, rionics, disk_link) {
		/* sanity */
		if (!list_empty(&rionic->disk_nrlink)) {
			_NT(trace_ib_admin_channel_load_disk_nordda, "rionic's disk_nrlink not empty");
			goto out;
		}
		lionics = &rionic->lionics;
		list_for_each_entry(lionic, lionics, rionic_link) {
			int tgt_max_nrchs_per_path;
			int clnt_max_nrchs_per_path;
			int n_nr_qps;

			/* sanity */
			if (!list_empty(&lionic->rionic_nrlink)) {
				_NT(trace_1_ib_admin_channel_load_disk_nordda, "lionic's rionic_nrlink not empty");
				goto out;
			}

			if (P2NV(lionic->port)->dev_type == DT_siw) {
				clnt_max_nrchs_per_path = clnt_max_nrchs_per_path_tcp;
				tgt_max_nrchs_per_path = disk->tgt_max_nrchs_per_path_tcp;
			} else {
				clnt_max_nrchs_per_path = clnt_max_nrchs_per_path_rdma;
				tgt_max_nrchs_per_path = disk->tgt_max_nrchs_per_path_rdma;
			}

			n_nr_qps = min(clnt_max_nrchs_per_path, tgt_max_nrchs_per_path);

			_NT(trace_8_ib_admin_channel_load_disk_nordda,
			    "disk @DISK_NAME, lionic gid=@GID_RAW layer=@LAYER transport_type=@TRANSPORT_TYPE, tgt_max_nrchs_per_path=@INT "
			    "clt_max_nrchs_per_path=@INT -> num NR QPs per path: @INT",
				disk->name, &lionic->ib_gid, lionic->layer, lionic->transport_type,
				tgt_max_nrchs_per_path, clnt_max_nrchs_per_path, n_nr_qps);

			if (n_nr_qps && !(lionic->nr_channels = kzalloc(
				sizeof(struct nvmeibc_ib_nordda_channel) * n_nr_qps,
				GFP_KERNEL))) {
				_NE(error_ib_admin_channel_load_disk_nordda, "OOM: Fail to allocate nordda channels");
				goto out;
			}
			lionic->n_nr_qps = n_nr_qps;
			if (nvmeibc_disk_prefix_priority_masks_len) {
				/* This lionic is mapped to 1 rionic (1 to 1) so this is the place to update prefix prio */
				lionic->iopath.priority = matching_suffix_len_to_group(matching_ipv4_prefix_length(&lionic->ib_gid, &rionic->ib_gid));
			}
			/* Add lionic to rionic */
			_ND(trace_2_ib_admin_channel_load_disk_nordda, "Add lionic @LIONIC to rionic @RIONIC", lionic, rionic);
			list_add(&lionic->rionic_nrlink, &rionic->nr_lionics);
		}

		/* Add rionic to disk */
		if (rionic->nr_prefered) {
			_ND(trace_3_ib_admin_channel_load_disk_nordda, "Add prefered rionic @RIONIC to disk's nr-rionics", rionic);
			list_add(&rionic->disk_nrlink, &disk->nr_rionics);
			disk->n_nr_rionics++;
			p++;
		}
		else {
			_ND(trace_4_ib_admin_channel_load_disk_nordda, "Add non-prefered rionic @RIONIC to disk's nr-rionics", rionic);
			list_add_tail(&rionic->disk_nrlink, &disk->nr_rionics);
			disk->n_nr_rionics++;
			np++;
			if (!disk->nr_np_head)
				disk->nr_np_head = &rionic->disk_nrlink;
		}
		rionic->disk = disk;
	}
	if (!disk->nr_np_head) {
		_ND(trace_5_ib_admin_channel_load_disk_nordda, "disk has all non/prefered nr-rionics");
		disk->nr_np_head = &disk->nr_rionics;
	}

	_NT(trace_6_ib_admin_channel_load_disk_nordda, 
		"disk @DISK_NAME nr-rionics (p=@NR_PREFERED, np=@NP, total=@TOTAL):", 
		disk->name, p, np, disk->n_nr_rionics);

	/* Assign nr_idx by order in nr_rionics so service_port spread uses 0..n_rionics-1 without gaps */
	{
		uint i = 0;
		list_for_each_entry(rionic, &disk->nr_rionics, disk_nrlink) {
			_NT(trace_7_ib_admin_channel_load_disk_nordda, "nr-rionic[@IDX] @IB_GID_IPV6, prefered=@PREFERED",
				i, &rionic->ib_gid, rionic->nr_prefered);
			rionic->nr_idx = i++;
		}
	}
	rv = 0;

	#if 0
	{
		struct list_head *nr_rionics = &disk->nr_rionics;
		struct list_head *lionics;

		list_for_each_entry(rionic, nr_rionics, disk_nrlink) {
			lionics = &rionic->nr_lionics;
			list_for_each_entry(lionic, lionics, rionic_nrlink) {
				_NI(trace_6_ib_admin_channel_load_disk_nordda_i1,
					"NR path: lionic @PTR to rionic @PTR, lionic->port=@PTR",
					lionic, rionic, lionic->port);
			}
		}
	}
	#endif

out:
	__NFOUT;
	return rv;
}

static int get_disks_infos(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	bool is_rediscover)
{
	int n_triplets = 0;
	struct nvmeibc_disk *disk = ch->base.base.disk;
	int rv = 0;

	__NFIN;
	if ((rv = load_disk_info(ch, tag, &n_triplets, disk, is_rediscover)) < 0) {
		_NT(trace_ib_admin_channel_get_disks_infos, "Failed to read disk info");
		goto out;
	}
	if ((rv = load_disk_nics(ch, tag, disk, n_triplets)) < 0) {
		_NT(trace_1_ib_admin_channel_get_disks_infos, "Failed to read disk @DISK_NAME nics info", disk->name);
		goto out;
	}
	if ((rv = load_disk_rscs(ch, tag, disk)) < 0) {
		_NT(trace_2_ib_admin_channel_get_disks_infos, "Failed to read disk @DISK_NAME resource info", disk->name);
		goto out;
	}
	if ((rv = load_disk_lock_info(ch, tag, disk)) < 0) {
		_NT(trace_3_ib_admin_channel_get_disks_infos, "Failed to read disk @DISK_NAME lock info", disk->name);
		goto out;
	}
	if ((rv = load_disk_nordda(ch, tag, disk)) < 0) {
		_NT(trace_4_ib_admin_channel_get_disks_infos, "Failed to read disk @DISK_NAME nordda info", disk->name);
		goto out;
	}

out:
	__NFOUT;
	return rv;
}

static void print_controller_info(struct nvmeibc_ib_admin_channel *ch)
{
	char lgid_buf[GUID_SIZE] = {0};
	char rgid_buf[GUID_SIZE] = {0};
	struct nvmeibc_disk *disk = ch->base.base.disk;
	struct list_head *rionics;
	struct nvmeibc_io_rnic *rionic;
	struct list_head *lionics;
	struct nvmeibc_io_lnic *lionic;

	__NFIN;
	format_gid_raw(ch->net.base.path.sgid.raw, lgid_buf);
	_ND(trace_ib_admin_channel_print_controller_info, "@@@");
	_ND(trace_1_ib_admin_channel_print_controller_info, "lanic=@LANIC, page_size=@PAGE_SIZE", lgid_buf, ch->base.cntr_page_size);
	if (disk->info) {
		_ND(trace_2_ib_admin_channel_print_controller_info, "Rdisk=@RDISK, nsid=@NSID, sector_shift=@SECTOR_SHIFT, "
		   "tot_d_rscs=@TOT_D_RSCS, max_c_rscs=@MAX_C_RSCS",
			disk->name, disk->info->nsid, disk->info->sector_shift,
			disk->info->n_rscs, disk->info->max_client_rscs);
		rionics = &disk->rionics;
		list_for_each_entry(rionic, rionics, disk_link) {
			format_gid_raw(rionic->ib_gid.raw, rgid_buf);
			lionics = &rionic->lionics;
			list_for_each_entry(lionic, lionics, rionic_link) {
				format_gid_raw(lionic->path.sgid.raw, lgid_buf);
		_ND(trace_3_ib_admin_channel_print_controller_info, "IO pair (l=@LGID_BUF, r=@RGID_BUF), n_rscs=@N_RSCS", lgid_buf, rgid_buf,
		   0); /* RDDA removed */
			}
		}
	}
	else
		_ND(trace_4_ib_admin_channel_print_controller_info, "Ldisk=@LDISK", disk->name);
	_ND(trace_5_ib_admin_channel_print_controller_info, "@@@");
	__NFOUT;
}

static int parse_access_map_msg(struct nvmeibc_ib_admin_channel *ch, u64 tag,
	bool is_redisover)
{
	int rv, ndisks;

	__NFIN;
	if ((rv = ndisks = get_controller_info(ch, tag)) < 0 ||
		(rv = get_disks_infos(ch, tag, is_redisover)) < 0)
		goto out;
	print_controller_info(ch);

out:
	__NFOUT;
	return rv;
}

static int get_access_map(struct nvmeibc_ib_admin_channel *ch,
	bool is_rediscover, size_t acs_map_sz)
{
	struct volume_client_req *req = NULL;
	int rv = -ENOENT;

	__NFIN;
	_NT(trace_ib_admin_channel_get_access_map, "--- Sending NVMEIBC_MA_GET_ACCESS message to controller @BASE_NAME",
		ch->base.base.name);
	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_get_access_map, "No free messages for administrator to use");
		goto out;
	}
	BUG_ON(!acs_map_sz);
	/* send the rdma part of the message */
	if ((rv = send_rdma_msg(ch, acs_map_sz)) < 0) {
		goto free_iu;
	}
	/* set the iu type */
	/* set the request */
	req = ch->send_ioctx->buf;
	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(
		ch, prp_ma_get_acs, ch, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0) {
		goto free_iu;
		}
	/* wait for the replies andhandlethem */
	rv = parse_access_map_msg(ch, req->hdr.tag, is_rediscover);
	_NT(trace_1_ib_admin_channel_get_access_map, "--- Received NVMEIBC_MA_GET_ACCESS message reply "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");
free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);
out:
	ch->send_ioctx = NULL;

	__NFOUT;
	return rv;
}

/* prepare alloc_net message */
static ssize_t __attribute__((unused)) prp_alloc_net(void *p, void *buf, const void *buf_end)
{
	/* RDDA removed - stubbed */
	return -ENOTSUPP;
}


static int __attribute__((unused)) io_connect(struct nvmeibc_ib_admin_channel *ch,
	void *ioch)
{
	int rv = -ENOENT;

	__NFIN;
	/* RDDA removed - stubbed */
	__NFOUT;
	return rv;
}

/* prepare alloc_net message */
static ssize_t prp_alloc_nr_net(void *p, void *buf, const void *buf_end)
{
	struct nvmeibc_ib_nordda_channel *nrch = p;
	struct nvmeibc_io_lnic *lionic = nrch->lionic;
	struct nvmeibc_ib_admin_channel *ch = ac_to_iac(lionic->rionic->ch);
	struct volume_client_req *req;
	struct volume_client_config_alloc_net_req *a_net_req;

	__NFIN;
	/* init the command */
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(a_net_req) > buf_end);
	req = buf;
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(vex_base);
	_ND(trace_ib_admin_channel_prp_alloc_nr_net, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_ALLOC_NR_NET;
	a_net_req = &req->config_req.a_net_req;
	memcpy(a_net_req->def.disk_name, lionic->disk->name,
		sizeof(a_net_req->def.disk_name));
	if (!NVMEIB_UPDATE_NW_PATHS) {
		memcpy(a_net_req->def.sgid, lionic->path.sgid.raw, 16);
		memcpy(a_net_req->def.dgid, lionic->path.dgid.raw, 16);
	}
	else {
		memcpy(a_net_req->def.sgid, &lionic->port->gid.hw_gid, 16);
		memcpy(a_net_req->def.dgid, &lionic->rionic->hw_gid, 16);
	}
	a_net_req->def.qp_num = cpu_to_be16((u16)nrch->base.index);

	if (nvmeib_version_protocol_lt(&ch->base.base.disk->last_tgt_ver,
								   &nvmeib_2p1_version)) {
		_NT(trace_0_ib_admin_channel_prp_alloc_nr_net,
			"omit cs-gid for older server");
	}
	else {
		a_net_req->cs_gid = cpu_to_be64(nrch->base.bailed_cmds.cs_gid);
	}

	__NFOUT;
	return NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(a_net_req);
}

static int nordda_connect(struct nvmeibc_ib_admin_channel *ch,
			  struct nvmeibc_ib_nordda_channel *nrch)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *recv_ioctx = NULL;
	struct nvmeibc_disk *disk = ch->base.base.disk;
	int erv, rv = -ENOENT;

	__NFIN;
	_NT(trace_ib_admin_channel_nordda_connect, "--- Sending NVMEIBC_MA_ALLOC_NR_NET message to qp @INDEX @ host @BASE_NAME",
		nrch->base.index, ch->base.base.name);
	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_nordda_connect, "No free messages for administrator to use");
		goto out;
	}
	/* set the iu type */
	/* set the request */
	req = ch->send_ioctx->buf;
	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(ch, prp_alloc_nr_net,
		nrch, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0)
		goto free_iu;
	/* wait for the reply */

	recv_ioctx = wait_pending_iu(ch, NVMEIB_WAIT_FOR_RESOURCES_C, req->hdr.tag);
	if (recv_ioctx) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag == req->hdr.tag &&
			rsp->version_tag == req->version_tag &&
			rsp->opcode == NVMEIBS_RSP_MGMT_OPCODE_OK) {
			rv = nvmeibc_ib_nordda_channel_init_reqs(nrch, &rsp->a_nr_net_rsp);
			if (rv == 0) {
				nrch->max_io_sz = nrch->reqs[0].rn_pages *
					ch->base.cntr_page_size - disk->md_size;
				_NT(trace_1_ib_admin_channel_nordda_connect, "Disk @DISK_NAME - Set max no-RDDA IO size to @MAX_NORDDA_BB\n",
						disk->name, nrch->max_io_sz);
				nrch->max_gen_sz = nrch->reqs[0].rn_pages *
					ch->base.cntr_page_size;
				_NT(trace_2_ib_admin_channel_nordda_connect, "Disk @DISK_NAME - Set max GEN cmd size to @MAX_NORDDA_BB\n",
						disk->name, nrch->max_gen_sz);
				/* Update disk min, max sizes */
				disk->max_gen_cmd_bb = max(disk->max_gen_cmd_bb, nrch->max_gen_sz);
				disk->max_nordda_bb = max(disk->max_nordda_bb, nrch->max_io_sz);
				disk->min_nordda_bb = !disk->min_nordda_bb ? nrch->max_io_sz : min(disk->min_nordda_bb, nrch->max_io_sz);
				disk->min_gen_cmd_bb = !disk->min_gen_cmd_bb ? nrch->max_gen_sz : min(disk->min_gen_cmd_bb, nrch->max_gen_sz);
				_NT(trace_5_ib_admin_channel_nordda_connect,
						"Disk @DISK_NAME - Updated (Min/Max) NORDDA (@MIN_NORDDA_BB/@MAX_NORDDA_BB) Gen Cmd (@MIN_GEN_CMD_BB/@MAX_GEN_CMD_BB)\n",
						disk->name, disk->min_nordda_bb, disk->max_nordda_bb, disk->min_gen_cmd_bb, disk->max_gen_cmd_bb);
			}
		}
		else {
			_NT(error_1_ib_admin_channel_nordda_connect, "response error, tag rsp @TAG vs. req @TAG, rsp->opcode = "
				"@OPCODE", rsp->hdr.tag, req->hdr.tag, rsp->opcode);
			rv = -EINVAL;
			goto post_recv;
		}
	}
	else {
		_NT(trace_3_ib_admin_channel_nordda_connect, "Failed to wait for new receivee messages");
		rv = -1;
		goto free_iu;
	}
	_NT(trace_4_ib_admin_channel_nordda_connect, "--- Received NVMEIBC_MA_ALLOC_NR_NET message reply from "
		"qp @INDEX @ host @BASE_NAME was: @OPT_NOT ok",
		nrch->base.index, ch->base.base.name, rv ? "not " : "");

post_recv:
	erv = nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
	if (!rv && erv)
		rv = erv;
free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);

out:
	ch->send_ioctx = NULL;

	__NFOUT;
	return rv;
}

static int connect_lionics(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_io_rnic *rionic, int rionic_index)
{
	struct list_head *lionics = &rionic->lionics;
	struct nvmeibc_io_lnic *lionic;
	char gid_buf[GUID_SIZE] = {0};
	int i, lionic_index = 0, rv = 0;
	struct nvmeibc_admin_rnic *arnic = ch->base.arnic;
	struct nvmeibc_disk *disk = ch->base.arnic->channel->base.disk;

	__NFIN;
	list_for_each_entry(lionic, lionics, rionic_link) {
		format_gid_raw(lionic->path.sgid.raw, gid_buf);
		/*
		 * Create No-RDDA channels
		 */
		for (i = 0; i < lionic->n_nr_qps; ++i) {
			if (!nvmeibc_ib_nordda_channel_create(nvmeibc_cinst_get_core_p(&ch->base.base),
				lionic, i, lionic_index, rionic_index)) {
				ARNIC_DISCOVER_STATUS(arnic, NVMEIBC_ARNIC_DISCOVER_NRCH_CREATE_FAILED);
				DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_NRCH_CREATE_FAILED);
				_NT(trace_2_ib_admin_channel_connect_lionics, "Fail to create nordda channel");
				rv = -ENOMEM;
				goto out;
			}
		}

		++lionic_index;
	}

out:
	__NFOUT;
	return rv;
}

static int connect_access_map(struct nvmeibc_ib_admin_channel *ch)
{
	struct list_head *rionics = &ch->base.rionics;
	struct nvmeibc_io_rnic *rionic;
	int rionic_index = 0, rv = 0;

	__NFIN;
	list_for_each_entry(rionic, rionics, admin_link)
		if ((rv = connect_lionics(ch, rionic, rionic_index++)) < 0)
			goto out;

out:
	__NFOUT;
	return rv;
}

int nvmeibc_ib_admin_channel_create_access_map(
	struct nvmeibc_ib_admin_channel *ch, bool is_rediscover)
{
	struct nvmeibc_disk *disk = ch->base.base.disk;
	size_t acs_map_sz = 0;
	int rv = 0;

	__NFIN;
	if (!ch->base.n_disks) {
		_ND(trace_ib_admin_channel_nvmeibc_ib_admin_channel_create_access_map, "Controller @BASE_NAME has no user disk", ch->base.base.name);
		goto out;
	}
	if ((rv = fill_access_map(ch, disk, &acs_map_sz)) < 0)
		goto out;
	if ((rv = get_access_map(ch, is_rediscover, acs_map_sz)) < 0) {
		goto out;
	}
	if ((rv = read_lock_mems(ch)) < 0)
		goto out;
	if ((rv = connect_access_map(ch)) < 0)
		goto out;

out:
	__NFOUT;
	return rv;
}

static void nrch_try_disconnect_smp_fn(void *ctx)
{
	struct nvmeibc_ib_nordda_channel *ch = ctx;
	_NT(trace_try_disconnect_smp_fn, "@BASE_NAME", ch->base.name);
	nvmeibc_ib_nordda_channel_try_disconnect(ch);
}

static void disconnect_lionic(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_io_lnic *lionic)
{
	int dying;
	int i;

	__NFIN;
	if ((dying = atomic_inc_return(&lionic->dying)) == 1) {
		/*
		 * Disconnect No-RDDA channels
		 */
		for (i = 0; i < lionic->n_nr_qps; ++i) {
			struct nvmeibc_ib_nordda_channel *nrch = lionic->nr_channels + i;
			if (nvmeibc_channel_is_ll_pcpu_ch(&nrch->base)) {
				/* Per-cpu NRCH, call on correct CPU using IPI */
				smp_call_function_single(nvmeibc_channel_pcpu_ch_get_cpu(&nrch->base), nrch_try_disconnect_smp_fn, nrch, true);
			} else
				nvmeibc_ib_nordda_channel_try_disconnect(nrch);
		}
	}
	else
		_ND(trace_ib_admin_channel_disconnect_lionic, "We are already scheduled for death @DYING times", dying);
	__NFOUT;
}

static void disconnect_rionic(
	struct nvmeibc_ib_admin_channel *ch, struct nvmeibc_io_rnic *rionic)
{
	int dying;
	struct list_head *lionics;
	struct nvmeibc_io_lnic *lionic;

	__NFIN;
	if ((dying = atomic_inc_return(&rionic->dying)) == 1) {
		lionics = &rionic->lionics;
		list_for_each_entry(lionic, lionics, rionic_link)
			disconnect_lionic(ch, lionic);
	}
	else
		_ND(trace_ib_admin_channel_disconnect_rionic, "We are already scheduled for death @DYING times", dying);
	__NFOUT;
}

static void stop_io_channels(struct nvmeibc_ib_admin_channel *ch)
{
	struct list_head *rionics;
	struct nvmeibc_io_rnic *rionic;

	NFIN;
	rionics = &ch->base.rionics;
	list_for_each_entry(rionic, rionics, admin_link)
		disconnect_rionic(ch, rionic);
	NFOUT;
}

static void stop_lock_channels(struct nvmeibc_ib_admin_channel *ch)
{
	struct nvmeibc_locks_channel *lock_ch = ch->base.segments_locks_remote.lock_ch;
	NFIN;

	if (lock_ch) {
		struct nvmeibc_disk_segments_locks *seg_locks =
		    nvmeibc_disk_get_segs_locks(
		        ch->base.base.disk, (struct nvmeibc_disk_get_segs_locks_flags){
		                                .write = 1, .dont_wait = 0});
		if (seg_locks && seg_locks->lock_ch == lock_ch) {
			/* Unlink seg_locks lock channel only if it is the same one that we
			 * are disconnecting */
			_NT(trace_1_ib_admin_channel_stop_lock_channels,
			    "Unlink disk from segments-lock");
			seg_locks->lock_ch = NULL;
		}
		nvmeibc_disk_put_segs_locks(
		    seg_locks, (struct nvmeibc_disk_get_segs_locks_flags){.write = 1});
		_NT(trace_ib_admin_channel_stop_lock_channels,
		    "Disconnect @POSITION_STR lock-ch=@LOCK_CH",
		    seg_locks->is_local ? "Local" : "Remote", lock_ch);
		nvmeibc_locks_channel_start_disconnect(lock_ch);
	}
	else {
		_NT(trace_2_ib_admin_channel_stop_lock_channels, "Admin-ch @CH_PTR has no lock-ch to disconnect", ch);
	}

	NFOUT;
}

static void nvmeibc_ib_admin_channel_disconnect_work_cont(struct workqe_struct *work)
{
	struct admin_remove_workq *arwork =
		container_of(work, struct admin_remove_workq, work);
	struct nvmeibc_ib_admin_channel *ch = arwork->ch;
	struct nvmeibc_ib_net *net = &ch->net.base;
	int dying, state;

	__NFIN;
	_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work_cont, "Wait for all IO channels queues to finish, admin-ch @BASE_NAME disk @DISK_NAME",
		ch->base.base.name, ch->base.base.disk->name);
	wait_io_channels(ch);

	_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work_cont, "Stopping LOCK channel...");
	stop_lock_channels(ch);

	/* admin-ch's on-disconnect callback only schedules disk-release and not
	   some remove-work as other chs do. Thus, we should & can call break-qp
	   even if dying was already inc'ed (e.g. by peer-disconnect) which runs
	   only if net state is DISCONNECTING. */
	dying = atomic_inc_return(&net->dying);
	_NT(trace_2_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work_cont,
		"net @NET, inc dying to @DYING", net, dying);
	if (dying == 1) {
		state = nvmeib_get_state_guard(&net->state);
		nvmeib_set_state_guard(&net->state, NVMEIBC_IB_NET_DISCONNECTING);
		_NT(trace_3_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work_cont,
			"net @NET, initiate disconnect (state was @STATE)", net, state);
	}
	_NT(trace_4_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work_cont,
		"net @NET, break-qp...", net);
	nvmeibc_ib_net_break_qp(net);

	if (ch->recv_q) {
		nvmeib_free_recvq(ch->recv_q);
		kfree(ch->recv_q);
		ch->recv_q = NULL;
	}

	kfree(arwork);
	NFOUT;
}

static void nvmeibc_ib_admin_channel_disconnect_work(struct workqe_struct *work)
{
	struct admin_remove_workq *arwork =
		container_of(work, struct admin_remove_workq, work);
	struct nvmeibc_ib_admin_channel *ch = arwork->ch;

	__NFIN;
	_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work, "Stopping KA...");
	keep_alive_stop(ch);
	_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work, "Initiate IO channels remove-work...");
	stop_io_channels(ch);
	WQ_INIT_WORK(&arwork->work, nvmeibc_ib_admin_channel_disconnect_work_cont);
	if (nvmeibc_admin_channel_add_work(&ch->base, &arwork->work) < 0)
		_NE(error_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect_work, "Fail to add admin-ch @BASE_NAME disconnect work cont",
			ch->base.base.name);
	NFOUT;
}

void nvmeibc_ib_admin_channel_disconnect(struct nvmeibc_ib_admin_channel *ch)
{
	int dying;
	struct admin_remove_workq *work;
	bool already_locked;
	unsigned long flags = -1;

	__NFIN;

	_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect, "admin-ch @BASE_NAME (@CH_PTR), attempt disconnect from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		ch->base.base.name, ch, __builtin_return_address(0));
	already_locked = nvmeibc_channel_already_locked(&ch->base.base);
	if (!already_locked)
		nvmeibc_channel_spin_lock_irqsave(&ch->base.base, &flags);
	if ((dying = atomic_inc_return(&ch->base.base.dying)) == 1) {
		if ((work = kzalloc(sizeof(*work), GFP_ATOMIC))) {
			/* submit the work */
			_NT(trace_2_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect,
				"Add disconnect-work for admin-ch @BASE_NAME (wq-pid=@INT)",
				ch->base.base.name, wq_pid(ch->base.remove_wq));

			WQ_INIT_WORK(&work->work, nvmeibc_ib_admin_channel_disconnect_work);
			work->ch = ch;
			if (nvmeibc_admin_channel_add_work(&ch->base, &work->work) < 0)
				_NE(error_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect, "Fail to add admin-ch @BASE_NAME disconnect work",
					ch->base.base.name);
		}
		else
			_NE(error_1_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect, "OOM: admin-ch @BASE_NAME disconnect work", ch->base.base.name);
	}
	else
		_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_disconnect, "admin-ch @BASE_NAME already dying (inc to @DYING)",
			ch->base.base.name, dying);
	if (!already_locked)
		nvmeibc_channel_spin_unlock_irqrestore(&ch->base.base, flags);
	NFOUT;
}

struct vex_ach_get_jrange_encode_ctx {
	struct nvmeibc_disk *disk;
	struct nvmeibc_ib_admin_channel *ch;
	dma_addr_t jmdc_rsp_dma;
	size_t jmdc_rsp_len;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_req_clnt_base_encode)
{
	struct vex_ach_get_jrange_encode_ctx *ctx = arg;
	struct nvmeibc_disk *disk = ctx->disk;
	struct nvmeibc_ib_admin_channel *ch = ctx->ch;
	struct volume_client_config_get_jrange_base *jrange_req = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*jrange_req) > wire_buf_end);

	snprintf(jrange_req->client_name, sizeof(jrange_req->client_name),
		 "%s", nvmeib_get_utsname_nodename());
	memcpy(jrange_req->disk_name, ch->base.base.disk->name,
	       sizeof(jrange_req->disk_name));
	jrange_req->client_uuid = *nvmeibc_get_uuid(nvmeibc_cinst_get_core_p(&ch->base.base));
	jrange_req->rdma.msg_raddr = cpu_to_be64(ch->msg_area_map.ioaddr);
	jrange_req->rdma.msg_size = cpu_to_be32(ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift);
	jrange_req->rdma.msg_rkey = cpu_to_be32(ch->msg_area_map.rkey);
	jrange_req->prev_rng = cpu_to_be32(disk->jrc.jour.rng_id);

	return sizeof(*jrange_req);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_req_clnt_ext1_encode)
{
	struct vex_ach_get_jrange_encode_ctx *ctx = arg;
	struct nvmeibc_disk *disk = ctx->disk;
	struct volume_client_config_get_jrange_ext1 *jrange_req = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*jrange_req) > wire_buf_end);

	nvmeibc_jam_disk_cache_hton(disk, &jrange_req->clnt_jrc);

	return sizeof(*jrange_req);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_req_clnt_ext2_encode)
{
	struct vex_ach_get_jrange_encode_ctx *ctx = arg;
	struct nvmeibc_disk *disk = ctx->disk;
	struct volume_client_config_get_jrange_ext2 *jrange_req = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*jrange_req) > wire_buf_end);

	jrange_req->binje_req = cpu_to_be32(disk->binje_ulp);

	/* Future: if UUID or N can change dynamically (without client restart),
	   we can still send jam-cache and target can decide whether to use it */
	jrange_req->binje_jrc = cpu_to_be32(disk->jrc.jour.rng_binje);

	return sizeof(*jrange_req);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_req_clnt_ext3_encode)
{
	struct vex_ach_get_jrange_encode_ctx *ctx = arg;
	struct volume_client_config_get_jrange_ext3 *jrange_req = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*jrange_req) > wire_buf_end);

	jrange_req->jmdc_rai.len = cpu_to_be32(ctx->jmdc_rsp_len);
	jrange_req->jmdc_rai.raddr = cpu_to_be64(ctx->jmdc_rsp_dma);
	jrange_req->jmdc_rai.rkey = cpu_to_be32(nvmeib_get_rkey(P2NV(ctx->ch->net.base.port)));

	return sizeof(*jrange_req);
}

/* prepare journal range message */
static ssize_t prp_ma_get_jrange(void *p, void *buf, const void *buf_end)
{
	struct volume_client_req *req = buf;
	struct vex_ach_get_jrange_encode_ctx *ctx = p;
	struct nvmeibc_ib_admin_channel *ch = ctx->ch;
	int rv = 0;

	__NFIN;
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE() > buf_end);
	/* init the command */
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(ch->base.vex_ops[vex_ach_get_jrange_req]->vex_ext);
	_ND(trace_ib_admin_channel_prp_ma_get_jrange, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_GET_JRANGE;

	if ((rv = CALL_VEX_OP(encode,
		vex_ach_get_jrange_req_clnt_ops, THREE_EXT,
			base, vex_ach_get_jrange_req_clnt_base_encode,
			ext1, vex_ach_get_jrange_req_clnt_ext1_encode,
			ext2, vex_ach_get_jrange_req_clnt_ext2_encode,
			ext3, vex_ach_get_jrange_req_clnt_ext3_encode,
				ch->base.vex_ops[vex_ach_get_jrange_req],
				NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_PAYLOAD(req), buf_end, ctx)) < 0)
		goto out;

	rv += NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE();

out:
	__NFOUT;
	return rv;
}

int nvmeibc_ib_admin_channel_get_journal_range(struct nvmeibc_ib_admin_channel* ch)
{
	struct nvmeibc_disk *disk = ch->base.base.disk;
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *send_ioctx;
	struct nvmeib_iu *recv_ioctx = NULL;
	struct vex_ach_get_jrange_encode_ctx ctx = {
		.ch = ch,
		.disk = disk,
		.jmdc_rsp_dma = 0,
		.jmdc_rsp_len = 0,
	};
	union jblock_md *ext_jmdc_rsp = NULL;
	int rv = -ENOENT;

	__NFIN;
	if (!ch->base.n_disks) {
		_ND(trace_ib_admin_channel_nvmeibc_ib_admin_channel_get_journal_range, "Controller @BASE_NAME has no user disk", ch->base.base.name);
		goto out;
	}

	if (ch->base.vex_ops[vex_ach_get_jrange_req]->vex_ext >= vex_ext3) {
		/* Allocate a DMA buffer for the target to fill with the JMDC */
		ctx.jmdc_rsp_len = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE * sizeof(*ext_jmdc_rsp);
		ext_jmdc_rsp = nvmeib_public_ib_dma_alloc_coherent(P2IB(ch->net.base.port),
						 ctx.jmdc_rsp_len,
						 &ctx.jmdc_rsp_dma,
						GFP_KERNEL);
		if (!ext_jmdc_rsp) {
			_NE(err_c_admin_get_journal_range, "DMA Allocation Error");
			rv = -ENOMEM;
			goto out;
		}
	}

	_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_get_journal_range, "--- Sending NVMEIBC_MA_GET_JRANGE message to controller @BASE_NAME @DISK_NAME",
	   ch->base.base.name, disk->name);
	if (!(send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_nvmeibc_ib_admin_channel_get_journal_range, "No free messages for administrator to use");
		goto out;
	}
	/* set the iu type */
	/* set the request */
	req = send_ioctx->buf;
	/* send and wait for send completion */
	ch->send_ioctx = send_ioctx;
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(ch, prp_ma_get_jrange,
		&ctx, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0)
		goto free_iu;
	ch->send_ioctx = NULL;
	/* wait for the reply */
	if ((recv_ioctx = wait_pending_iu(ch, NVMEIB_WAIT_FOR_RESOURCES_C,
		req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != req->hdr.tag ||
			rsp->version_tag != req->version_tag ||
			rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK ||
			parse_jrange_msg(ch, disk, ext_jmdc_rsp) < 0) {
				_NT(trace_2_ib_admin_channel_nvmeibc_ib_admin_channel_get_journal_range, "Could not parse received journal range msg - "
				"rsp->hdr.tag=@TAG, req->hdr.tag=@TAG, rsp->version_tag=@VERSION_TAG, req->version_tag=@VERSION_TAG, rsp->opcode=@OPCODE",
				rsp->hdr.tag, req->hdr.tag, be16_to_cpu(req->version_tag), be16_to_cpu(rsp->version_tag), rsp->opcode);
//				_NT(nvmeibc_ib_admin_channel_get_journal_range_t1,
//					"Could not parse received journal range msg - "
//					"rsp->hdr.tag=@INT^$, req->hdr.tag=@INT64,"
//					"rsp->opcode=@INT, @INT",
//					rsp->hdr.tag, req->hdr.tag, (int)rsp->opcode,
//					(int)NVMEIBS_RSP_MGMT_OPCODE_OK);
				rv = -EINVAL;
				goto post_recv;
			}
		}
		else {
			_NT(trace_3_ib_admin_channel_nvmeibc_ib_admin_channel_get_journal_range, "Failed to wait for new receive messages");
			rv = -1;
			goto free_iu;
		}
		_NT(trace_4_ib_admin_channel_nvmeibc_ib_admin_channel_get_journal_range, "--- Received NVMEIBC_MA_GET_JRANGE message reply "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");

post_recv:
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, send_ioctx);

out:
	ch->send_ioctx = NULL;
	if (ext_jmdc_rsp) {
		nvmeib_public_ib_dma_free_coherent(P2IB(ch->net.base.port),
					ctx.jmdc_rsp_len,
					ext_jmdc_rsp,
					ctx.jmdc_rsp_dma);
		ext_jmdc_rsp = NULL;
	}
	__NFOUT;
	return rv;
}



struct prp_ma_locate_info {
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_disk *disk;
};

/* prepare remote io nics message */
static ssize_t prp_ma_locate(void *p, void *buf, const void *buf_end)
{
	struct prp_ma_locate_info *info = p;
	struct nvmeibc_ib_admin_channel *ch = info->ch;
	struct volume_client_req *req;
	struct volume_client_config_ma_locate *loc_req;

	__NFIN;
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(loc_req) > buf_end);
	/* init the command */
	req = buf;
	memset(req, 0, sizeof(*req));
	loc_req = &req->config_req.loc_req;
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(vex_base);
	_ND(trace_ib_admin_channel_prp_ma_locate, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_LOCATE_RSC;
	memcpy(loc_req->disk_name, info->disk->name, sizeof(loc_req->disk_name));
	loc_req->rdma.msg_raddr = cpu_to_be64(ch->msg_area_map.ioaddr);
	loc_req->rdma.msg_size = cpu_to_be32(ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift);
	loc_req->rdma.msg_rkey = cpu_to_be32(ch->msg_area_map.rkey);
#ifdef NO_OFED
	loc_req->no_ofed = 1;
#endif
	strncpy(loc_req->ofed_ver, OFED_VER_STRING, NVMEIB_MAX_OFED_VER_STRLEN);
	strncpy(loc_req->kern_ver, KERN_VER_STRING, NVMEIB_MAX_KERN_VER_STRLEN);

	__NFOUT;
	return NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(loc_req);
}

static int parse_locate_msg(struct nvmeibc_ib_admin_channel *ch,
			    struct nvmeibc_disk *disk, struct volume_server_config_locate_rsp *lrsp, unsigned long elapsed)
{
	void *p = ch->msg_area;
	u64 n, id;
	int i, rv = 0;

	__NFIN;
	disk->tgt_ofed_kern_mismatch = lrsp->ofed_kern_mismatch;
	disk->tgt_no_ofed = lrsp->no_ofed;
	strncpy(disk->tgt_ofed_ver, lrsp->ofed_ver, NVMEIB_MAX_OFED_VER_STRLEN + 1);
	strncpy(disk->tgt_kern_ver, lrsp->kern_ver, NVMEIB_MAX_KERN_VER_STRLEN + 1);
	nvmeib_mem_sync_map_for_cpu(&ch->msg_area_map, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);
	n = be64_to_cpu(*(__be64 *)p);

	/* If timed out on server side - expect to receive 0 resources */
	if (elapsed >= NVMEIB_WAIT_FOR_RESOURCES_S) {
		nvmeibc_disk_counters_inc(disk, n_warn_locate_timeout);
		if (n != 0) {
			nvmeibc_disk_counters_inc(disk, n_warn_locate_timeout_nzero_rscs);
			_NW(locate_response_sucess_after_timeout,
			    "Receviced successful locate response after long time, "
			    "@JIFFIES",
			    elapsed);
		}
	}
	if (n == 0)
		nvmeibc_disk_counters_inc(disk, n_warn_locate_zero_rscs);

	p += sizeof(__be64);
	_NT(trace_ib_admin_channel_parse_locate_msg, "Received @NUM_RESOURCES disk resources for disk @DISK_NAME", n, disk->name);
	if (n == 0 && lrsp->ofed_kern_mismatch) {
		_NT(trace_1_ib_admin_channel_parse_locate_msg, "Failed to get disk resources due to OFED/Kernel mismatch. "
		"Target has OFED @OFED_VER Kernel @KERN_VER", lrsp->ofed_ver, lrsp->kern_ver);
	}
	if (n > disk->info->max_client_rscs) {
		_NE(error_ib_admin_channel_parse_locate_msg, "Received @NUM_RESOURCES resources which is more than pre-allocated "
		"for client @MAX_CLIENT_RSCS, reducing to @MAX_CLIENT_RSCS",
     n, disk->info->max_client_rscs, disk->info->max_client_rscs);
		n = disk->info->max_client_rscs;
	}
	for (i = 0; i < n; ++i) {
		id = be64_to_cpu(*(__be64 *)p);
		p += sizeof(__be64);
		_NT(trace_2_ib_admin_channel_parse_locate_msg, "Disk resources id @ID_LLONG", id);
		if ((rv = nvmeibc_disk_locate_resource(disk, id)) < 0) {
			_NE(error_1_ib_admin_channel_parse_locate_msg, "Fail to locate disk resources");
			break;
		}
	}
	__NFOUT;
	return rv;
}

static int request_disk_resources(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_disk *disk)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *send_ioctx;
	struct nvmeib_iu *recv_ioctx = NULL;
	struct prp_ma_locate_info info = {ch, disk};
	int rv = -ENOENT;
	unsigned long start_jiffies;

	__NFIN;
	_NT(trace_ib_admin_channel_request_disk_resources, "--- Sending NVMEIBC_MA_LOCATE_RSC message to controller @BASE_NAME @DISK_NAME",
		ch->base.base.name, disk->name);
	if (!(send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_request_disk_resources, "No free messages for administrator to use");
		goto out;
	}
	/* set the iu type */
	/* set the request */
	req = send_ioctx->buf;
	/* send and wait for send completion */
	ch->send_ioctx = send_ioctx;
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(ch, prp_ma_locate,
		&info, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0)
		goto free_iu;
	ch->send_ioctx = NULL;
	/* wait for the reply */
	start_jiffies = jiffies;
	if ((recv_ioctx = wait_pending_iu(ch, NVMEIB_WAIT_FOR_RESOURCES_C,
									  req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != req->hdr.tag ||
			rsp->version_tag != req->version_tag ||
			rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK ||
			parse_locate_msg(ch, disk, &rsp->locate_rsp, jiffies - start_jiffies) < 0) {
			_NT(trace_1_ib_admin_channel_request_disk_resources, "Could not parse received locate msg - "
			   "rsp->hdr.tag=@TAG, req->hdr.tag=@TAG, rsp->version_tag=@VERSION_TAG, req->version_tag=@VERSION_TAG, rsp->opcode=@OPCODE",
			   rsp->hdr.tag, req->hdr.tag, be16_to_cpu(req->version_tag), be16_to_cpu(rsp->version_tag), rsp->opcode);
//				_NT(request_disk_resources_t1,
//					"Could not parse received locate msg - "
//					"rsp->hdr.tag=@INT64, req->hdr.tag=@INT64,"
//					"rsp->opcode=@INT, @INT",
//					rsp->hdr.tag, req->hdr.tag, (int)rsp->opcode,
//					(int)NVMEIBS_RSP_MGMT_OPCODE_OK);
			rv = -EINVAL;
			goto post_recv;
		}
	}
	else {
		_NT(trace_2_ib_admin_channel_request_disk_resources, "Failed to wait for new receivee messages");
		rv = -1;
		goto free_iu;
	}
	_NT(trace_3_ib_admin_channel_request_disk_resources, "--- Received NVMEIBC_MA_LOCATE_RSC message reply "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");

post_recv:
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, send_ioctx);

out:
	ch->send_ioctx = NULL;
	__NFOUT;
	return rv;
}

static void request_disk_resources_work(struct workqe_struct *work)
{
	int rv;
	struct admin_cmd_workq *disk_rsrc_work =
		container_of(work, struct admin_cmd_workq, work);
	struct nvmeibc_disk *disk = disk_rsrc_work->ch->base.base.disk;

	NFIN;

	rv = request_disk_resources(disk_rsrc_work->ch, disk);
	if (rv < 0) {
		_NE(err_request_disk_resources_work,
			"Fail to receive my disk resources @DISK_NAME rv=@RV",
			disk->name, rv);
		nvmeibc_ib_net_disconnect(&disk_rsrc_work->ch->net.base);
	}

	kfree(work);
	NFOUT;
}

int nvmeibc_ib_admin_channel_request_disks_resources(
	struct nvmeibc_ib_admin_channel *ch)
{
	int rv;
	struct admin_cmd_workq *disk_rsrc_work;

	__NFIN;

	if ((disk_rsrc_work = kzalloc(sizeof(*disk_rsrc_work), GFP_ATOMIC))) {
		WQ_INIT_WORK(&disk_rsrc_work->work, request_disk_resources_work);
		disk_rsrc_work->ch = ch;
		disk_rsrc_work->rv = 0;
		disk_rsrc_work->iu = NULL;
		rv = nvmeibc_admin_channel_add_work(&ch->base, &disk_rsrc_work->work);
	}
	else {
		rv = -ENOMEM;
		_NE(nvmeibc_ib_admin_channel_request_disks_resources_e1,
			"Fail allocate memory for admin channel work");
	}

	__NFOUT;
	return rv;
}

int nvmeibc_ib_admin_channel_connect_io_channel(
	struct nvmeibc_ib_admin_channel *ch, void *ioch)
{
	int rv;

	__NFIN;
	/* RDDA removed - stubbed */
	rv = -ENOTSUPP;
	__NFOUT;
	return rv;
}


int nvmeibc_ib_admin_channel_connect_nordda_channel(
	struct nvmeibc_ib_admin_channel *ch, struct nvmeibc_ib_nordda_channel *nrch)
{
	int rv;

	__NFIN;
	if ((rv = nvmeibc_ib_nordda_channel_connect(nrch)) < 0) {
		_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_connect_nordda_channel, "Fail to start connection of IB nordda channel");
	}
	else if ((rv = nordda_connect(ch, nrch)) < 0) {
		_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_connect_nordda_channel, "Fail to finish connection of IB nordda channel");
	}
	__NFOUT;
	return rv;
}


struct prp_ma_reset_info {
	void *ioch;
	u64 disk_rsc_id;
	u64 msix_table_addr;
	u64 msix_raddr;
	u32 msix_payload;
};

/* prepare disk io reset message */
static ssize_t __attribute__((unused)) prp_reset_io(void *p, void *buf, const void *buf_end)
{
	/* RDDA removed - stubbed */
	NFIN;
	NFOUT;
	return -ENOTSUPP;
}


static int io_init(struct nvmeibc_ib_admin_channel *ch,
	void *ioch, u64 disk_rsc_id,
	u64 msix_table_addr, u64 msix_raddr, u32 msix_payload)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *recv_ioctx = NULL;
	int erv, rv = -ENOENT;
	struct prp_ma_reset_info info = {
        ioch, disk_rsc_id, msix_table_addr, msix_raddr, msix_payload};

	__NFIN;
	_NT(trace_ib_admin_channel_io_init, "--- Sending NVMEIBC_MA_RESET_RSC message to qp @INDEX @ host @BASE_NAME",
		0, ch->base.base.name);
	_NT(trace_1_ib_admin_channel_io_init, "disk_rsc_id=@DISK_RSC_ID, qp_num=@QP_NUM, msix_table_addr=@MSIX_TABLE_ADDR, msix_raddr=@MSIX_RADDR, "
	   "msix_payload=@MSIX_PAYLOAD", disk_rsc_id, 0,
		msix_table_addr, msix_raddr, msix_payload);
	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_io_init, "No free messages for administrator to use");
		goto out;
	}
	/* set the iu type */
	/* set the request */
	req = ch->send_ioctx->buf;
	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(ch, prp_reset_io,
		&info, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0)
		goto free_iu;
	/* wait for the reply */
	if ((recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag == req->hdr.tag &&
			rsp->version_tag == req->version_tag &&
			rsp->opcode == NVMEIBS_RSP_MGMT_OPCODE_OK) {
			_ND(trace_2_ib_admin_channel_io_init, "Managed to reset disk resource @DISK_RSC_ID",
				disk_rsc_id);
			rv = 0;
		}
		else {
			_NT(error_1_ib_admin_channel_io_init, "response error, rsp->hdr.tag=@TAG, req->hdr.tag=@TAG, rsp->version_tag=@VERSION_TAG, req->version_tag=@VERSION_TAG, rsp->opcode=@OPCODE",
			rsp->hdr.tag, req->hdr.tag, be16_to_cpu(req->version_tag), be16_to_cpu(rsp->version_tag), rsp->opcode);
			rv = -EINVAL;
			goto post_recv;
		}
	}
	else {
		_NT(trace_3_ib_admin_channel_io_init, "Failed to wait for new receivee messages");
		rv = -1;
		goto free_iu;
	}
	_NT(trace_4_ib_admin_channel_io_init, "--- Received NVMEIBC_MA_RESET_RSC message reply from "
		"qp @INDEX @ host @BASE_NAME was: @OPT_NOT ok",
		0, ch->base.base.name, rv ? "not " : "");

post_recv:
	erv = nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);
	if (!rv && erv)
		rv = erv;

free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);

out:
	ch->send_ioctx = NULL;

	__NFOUT;
	return rv;
}

int nvmeibc_ib_admin_channel_init_io_channel(
	struct nvmeibc_ib_admin_channel *ch, void *ioch,
	u64 disk_rsc_id, u64 msix_table_addr, u64 msix_address, u32 msix_payload)
{
	int rv;

	__NFIN;
	/* RDDA removed - stubbed */
	rv = -ENOTSUPP;
	if (0 && (rv = io_init(ch, ioch, disk_rsc_id,
		msix_table_addr, msix_address, msix_payload)) < 0) {
		_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_init_io_channel, "Fail to init disk IO resource");
	}
	__NFOUT;
	return rv;
}



struct prp_ma_get_jmdc_params {
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeib_alloc_n_map rng_data_map;
	struct nvmeib_alloc_n_map ent_md_map;
	struct nvmeib_alloc_n_map jmdc_ent_map;
	struct nvmeibc_disk_jmdc_read_comp *comp;
};

static ssize_t prp_ma_get_jmdc(void *p, void *buf, const void *buf_end)
{
	struct prp_ma_get_jmdc_params *params = p;
	struct nvmeibc_ib_admin_channel *ch = params->ch;
	struct nvmeibc_disk_jmdc_read_comp *comp = params->comp;
	struct volume_client_req *req;
	struct volume_client_jmdc_req *jmdc_req;

	__NFIN;
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_ADMIN_REQ_SIZE(jmdc_req) > buf_end);
	req = buf;
	memset(req, 0, sizeof(*req));
	jmdc_req = &req->jmdc_req;
	req->hdr.opcode = NVMEIB_GET_JMDC;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(vex_base);
	_ND(trace_ib_admin_channel_prp_ma_get_jmdc, "req tag @TAG", req->hdr.tag);
	snprintf(jmdc_req->client_name, sizeof(jmdc_req->client_name),
		 "%s", my_node_name(ch));
	memcpy(jmdc_req->disk_name, ch->base.base.disk->name,
	       sizeof(jmdc_req->disk_name));
	memcpy(jmdc_req->seg_uuid, comp->seg_uuid, sizeof(jmdc_req->seg_uuid));

	if (comp->rng_data_ai && comp->jmdc_ent_ai && comp->ent_md_ai) {
		jmdc_req->rng_data_rdma.msg_raddr = cpu_to_be64(params->rng_data_map.ioaddr);
		jmdc_req->rng_data_rdma.msg_size = cpu_to_be32(params->rng_data_map.n_pages * PAGE_SIZE);
		jmdc_req->rng_data_rdma.msg_rkey = cpu_to_be32(params->rng_data_map.rkey);

		jmdc_req->ents_md_rdma.msg_raddr = cpu_to_be64(params->ent_md_map.ioaddr);
		jmdc_req->ents_md_rdma.msg_size = cpu_to_be32(params->ent_md_map.n_pages * PAGE_SIZE);
		jmdc_req->ents_md_rdma.msg_rkey = cpu_to_be32(params->ent_md_map.rkey);

		jmdc_req->jmdc_ents_rdma.msg_raddr = cpu_to_be64(params->jmdc_ent_map.ioaddr);
		jmdc_req->jmdc_ents_rdma.msg_size = cpu_to_be32(params->jmdc_ent_map.n_pages * PAGE_SIZE);
		jmdc_req->jmdc_ents_rdma.msg_rkey = cpu_to_be32(params->jmdc_ent_map.rkey);
	} else {
		jmdc_req->get_len_only = 1;
	}
	jmdc_req->get_dirty_only = comp->dirty_only;
	jmdc_req->recov_src = comp->recov_src;
	strncpy(jmdc_req->seg_uuid, comp->seg_uuid, NVMEIB_GID_STR_MAX);

	jmdc_req->start_rng = cpu_to_be16(comp->start_rng);
	jmdc_req->num_rng = cpu_to_be16(comp->num_rng);
	jmdc_req->client_uuid = comp->client_uuid;
	jmdc_req->get_client_uuid_only = comp->client_uuid_only;


	__NFOUT;
	return NVMEIBC_VOLUME_CLIENT_ADMIN_REQ_SIZE(jmdc_req);
}

struct __please_kill_yourself_args_work {
	struct workqe_struct work;
	void (*cb)(void*);
	void *ctx;
	struct nvmeibc_please_kill_yourself_args args;
};

void nvmeibc_ib_admin_channel_dbg_please_kill_yourself_work(struct workqe_struct *_work)
{
	struct __please_kill_yourself_args_work *work =
	    container_of(_work, struct __please_kill_yourself_args_work, work);
	struct nvmeibc_ib_admin_channel *ch = work->args.ch;
	int rv;

	__NFIN;

	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NT(kill_yourself_no_send_ioctx,
		    "Error trying to send KILL_YOURSELF - no send_ioctx");
		rv = -ENOBUFS;
		goto out;
	}

	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(
	         ch, prp_dbg_please_kill_yourself, &work->args, NVMEIB_WR_DBG_CMD,
	         ch->send_ioctx)) < 0) {
		_NT(kill_yourself_send_error,
		    "Error trying to send KILL_YOURSELF dbg message to the server");
	}

out:
	if (work->cb) work->cb(work->ctx);

	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);
	ch->send_ioctx = NULL;

	kfree(work);

	__NFOUT;

}

static void __nvmeibc_ib_admin_channel_kill_remote_and_die_cb(void*ctx) {
	(void)ctx;
	BUG();
}

void nvmeibc_ib_admin_channel_kill_remote_and_call(
    struct nvmeibc_please_kill_yourself_args *args,
	void (*cb)(void *), void *ctx) {

	struct __please_kill_yourself_args_work *work;
	struct nvmeibc_ib_admin_channel *ch = args->ch;

	__NFIN;

	if (!(work = kzalloc(sizeof(*work), GFP_ATOMIC))) {
		_NT(error_1_admin_channel_kill_remote_and_die, "Fail to alloc work");
		BUG();
	}

	work->cb = cb;
	work->ctx = ctx;
	work->args = *args;

	_NT(trace_3_admin_channel_kill_remote_and_die, "Adding DBG - KILL YOURSELF work for admin ch @CH_PTR", ch);
	WQ_INIT_WORK(&work->work, nvmeibc_ib_admin_channel_dbg_please_kill_yourself_work);
	nvmeibc_admin_channel_add_work(&ch->base, &work->work);

	__NFOUT;
}

void nvmeibc_ib_admin_channel_kill_remote_and_die(
    struct nvmeibc_please_kill_yourself_args *args) {
	nvmeibc_ib_admin_channel_kill_remote_and_call(
	    args, __nvmeibc_ib_admin_channel_kill_remote_and_die_cb, NULL);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jmdc_rsp_clnt_base_decode)
{
	const struct volume_server_get_jmdc_rsp_base *base = wire_buf;
	struct nvmeibc_disk_jmdc_read_comp *comp = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	memcpy(comp->rsp.read_jrnl_data->serjio_boot_id, base->serjio_boot_id, NVMEIB_GID_STR_MAX);
	comp->rsp.read_jrnl_data->num_ents_rng = be16_to_cpu(base->num_ents_rng);
	comp->rsp.read_jrnl_data->num_dirty_rng = be16_to_cpu(base->num_dirty_rng);
	comp->rsp.read_jrnl_data->num_rng = be16_to_cpu(base->num_rng);
	comp->rsp.read_jrnl_data->jrnl_start_lba = be64_to_cpu(base->jrnl_start_lba);
	comp->rsp.read_jrnl_data->jrnl_len_lba = be64_to_cpu(base->jrnl_len_lba);
	comp->rsp.read_jrnl_data->num_ents = be32_to_cpu(base->num_ents);
	*comp->rsp.rng_data_len = be32_to_cpu(base->hdr_len);
	*comp->rsp.ent_md_len = be32_to_cpu(base->ents_md_len);
	*comp->rsp.jmdc_ent_len = be32_to_cpu(base->ents_len);

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jmdc_rsp_clnt_ext1_decode)
{
	const struct volume_server_get_jmdc_rsp_ext1 *ext1 = wire_buf;
	struct nvmeibc_disk_jmdc_read_comp *comp = arg;

	if (!wire_buf) {
		/* Extension not present - fill defaults */
		comp->rsp.read_jrnl_data->rng_hdr_stride = sizeof(struct volume_server_get_jmdc_rng_data_base);
		comp->rsp.read_jrnl_data->num_ent_blocks = comp->rsp.read_jrnl_data->num_ents * NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	comp->rsp.read_jrnl_data->rng_hdr_version = be16_to_cpu(ext1->hdr_version);
	comp->rsp.read_jrnl_data->rng_hdr_stride = be16_to_cpu(ext1->hdr_stride);
	comp->rsp.read_jrnl_data->num_ent_blocks = be32_to_cpu(ext1->num_ent_blocks);

	return sizeof(*ext1);
}

void nvmeibc_ib_admin_channel_jmdc_read_work(struct workqe_struct *work)
{
	struct nvmeibc_ib_admin_jmdc_read_req_work *read_req_work =
		container_of(work, struct nvmeibc_ib_admin_jmdc_read_req_work, work);
	struct nvmeibc_ib_admin_channel *ch = read_req_work->ch;
	struct nvmeibc_disk_jmdc_read_comp *comp = read_req_work->comp;
	int rv = 0;
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct prp_ma_get_jmdc_params params = {0};
	struct nvmeib_iu *recv_ioctx;
	void *rng_data_buf = NULL;
	ssize_t rng_data_sz = 0;

	__NFIN;
	params.ch = ch;
	params.comp = comp;

	if (comp->rng_data_ai) {
		/* Create a temporary buffer to receive the range data so we can decode it into the block buffer.
		 * This was not necessary before, but now that we have extensions, it is. */
		rng_data_sz = vex_size(ch->base.vex_ops[vex_ach_get_jmdc_rng_hdr], NULL, true);
		BUG_ON(rng_data_sz <= 0);
		rng_data_sz *= comp->num_rng;
		params.rng_data_map.pd = ch->net.base.qp->pd;
		params.rng_data_map.n_pages = DIV_ROUND_UP(rng_data_sz, PAGE_SIZE);
		params.rng_data_map.access_flags = IB_ACCESS_REMOTE_WRITE;
		/* JH IOMMU: DMA_FROM_DEVICE is correct, used as sink for Remote RDMA_WRITE */
		params.rng_data_map.dma_dir = DMA_FROM_DEVICE;
		if (!(rng_data_buf = nvmeib_mem_alloc_n_vmap(&params.rng_data_map))) {
			_NE(error_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "nvmeib_alloc_n_map failed");
			goto out;
		}
	}
	if (comp->ent_md_ai) {
		params.ent_md_map.pd = ch->net.base.qp->pd;
		params.ent_md_map.n_pages = comp->ent_md_ai->n;
		params.ent_md_map.access_flags = IB_ACCESS_REMOTE_WRITE;
		params.ent_md_map.pages = comp->ent_md_ai->pages;
		/* JH IOMMU: DMA_FROM_DEVICE is correct, used as sink for Remote RDMA_WRITE */
		params.ent_md_map.dma_dir = DMA_FROM_DEVICE;
		if ((rv = nvmeib_mem_alloc_n_map(&params.ent_md_map))) {
			_NE(error_1_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "nvmeib_alloc_n_map failed (@RV)", rv);
			goto out;
		}
	}
	if (comp->jmdc_ent_ai) {
		params.jmdc_ent_map.pd = ch->net.base.qp->pd;
		params.jmdc_ent_map.n_pages = comp->jmdc_ent_ai->n;
		params.jmdc_ent_map.access_flags = IB_ACCESS_REMOTE_WRITE;
		params.jmdc_ent_map.pages = comp->jmdc_ent_ai->pages;
		/* JH IOMMU: DMA_FROM_DEVICE is correct, used as sink for Remote RDMA_WRITE */
		params.jmdc_ent_map.dma_dir = DMA_FROM_DEVICE;
		if ((rv = nvmeib_mem_alloc_n_map(&params.jmdc_ent_map))) {
			_NE(error_2_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "nvmeib_alloc_n_map failed (@RV)", rv);
			goto free_map;
		}
	}
	_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "--- Sending NVMEIB_GET_JMDC message to controller @BASE_NAME",
		ch->base.base.name);
	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_3_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "No free messages for administrator to use");
		rv = -ENOMEM;
		goto free_map;
	}
	/* set the iu type */
	/* set the request */
	req = ch->send_ioctx->buf;
	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(
		ch, prp_ma_get_jmdc, &params, NVMEIB_RDMA_GET_JMDC_REQ, ch->send_ioctx)) < 0)
		goto free_iu;
	/* wait for the reply */
	if ((recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != req->hdr.tag ||
			rsp->version_tag != req->version_tag ||
			rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK) {
			_NT(error_4_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "response error, rsp=@PTR, rsp->hdr.tag=@TAG, req->hdr.tag=@TAG, rsp->version_tag=@VERSION_TAG, req->version_tag=@VERSION_TAG, rsp->opcode=@OPCODE",
				rsp, rsp->hdr.tag, req->hdr.tag, be16_to_cpu(req->version_tag), be16_to_cpu(rsp->version_tag), rsp->opcode);
			rv = -EINVAL;
			goto post_recv;
		}
	}
	else {
		_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "Failed to wait for new receive messages");
		rv = -ETIMEDOUT;
		goto free_iu;
	}

	rv = CALL_VEX_OP(decode,
						vex_ach_get_jmdc_rsp_clnt_ops, ONE_EXT,
							base, vex_ach_get_jmdc_rsp_clnt_base_decode,
							ext1, vex_ach_get_jmdc_rsp_clnt_ext1_decode,
								ch->base.vex_ops[vex_ach_get_jmdc_rsp],
								rsp->payload, recv_ioctx->buf + recv_ioctx->size, comp);
	BUG_ON(rv <= 0);
	rv = 0;

	/* Sanity Checks */

	_NT(trace_14343254, "comp @PTR ch @PTR rsp @PTR recv_ioctx @PTR", comp, ch, rsp, recv_ioctx);

	BUG_ON(comp->rsp.read_jrnl_data->rng_hdr_version != ch->base.vex_ops[vex_ach_get_jmdc_rng_hdr]->vex_ext);
	BUG_ON(comp->rsp.read_jrnl_data->rng_hdr_stride != vex_size(ch->base.vex_ops[vex_ach_get_jmdc_rng_hdr], NULL, true));

	if (rng_data_buf)
		nvmeibc_get_jmdc_deserialize_rng_data(rng_data_buf, rng_data_sz, comp, ch);
	_NT(trace_2_ib_admin_channel_nvmeibc_ib_admin_channel_jmdc_read_work, "--- Received NVMEIB_GET_JMDC message reply "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");

post_recv:
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);
	ch->send_ioctx = NULL;

free_map:
	if (params.jmdc_ent_map.mr)
		nvmeib_mem_unmapn_n_free(&params.jmdc_ent_map);
	if (params.ent_md_map.mr)
		nvmeib_mem_unmapn_n_free(&params.ent_md_map);
	if (params.rng_data_map.mr)
		nvmeib_mem_vunmap_n_free(&params.rng_data_map, rng_data_buf);

out:
	__NFOUT;
	kfree(read_req_work);
	comp->rsp.status = (!rv) ? NCL_STATUS_TAKEN : NCL_STATUS_FAIL_COMP;
	comp->callback(comp);
}

static ssize_t prp_ma_get_lock_gids(void *p, void *buf, const void *buf_end)
{
	struct nvmeibc_ib_admin_channel *ch = p;
	struct volume_client_req *req;
	struct volume_client_config_get_lock_gids *gids_req;

	__NFIN;
	BUG_ON(buf + NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(get_lock_gids) > buf_end);
	/* init the command */
	req = buf;
	memset(req, 0, sizeof(*req));
	gids_req = &req->config_req.get_lock_gids;
	req->hdr.opcode = NVMEIB_CONFIG;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(vex_base);
	_ND(trace_ib_admin_channel_prp_ma_get_lock_gids, "req tag @TAG", req->hdr.tag);
	req->config_req.opcode = NVMEIBC_MA_GET_LOCK_GIDS;
	snprintf(gids_req->client_name, sizeof(gids_req->client_name),
		"%s", my_node_name(ch));
	memcpy(gids_req->disk_name, ch->base.base.disk->name,
		sizeof(gids_req->disk_name));
	gids_req->rdma.msg_raddr = cpu_to_be64(ch->msg_area_map.ioaddr);
	gids_req->rdma.msg_size = cpu_to_be32(ch->cmsg_buffer_pages << ch->cmsg_buffer_page_shift);
	gids_req->rdma.msg_rkey = cpu_to_be32(ch->msg_area_map.rkey);
	__NFOUT;
	return NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(get_lock_gids);
}

static struct nvmeibc_locks_channel *get_lock_gids_and_connect(
	struct nvmeibc_ib_admin_channel *ch)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *recv_ioctx = NULL;
	struct nvmeibc_locks_channel *lock_ch = NULL;
	int rv = -ENOENT;

	__NFIN;
	_NT(trace_ib_admin_channel_get_lock_gids_and_connect, "--- Sending NVMEIBC_MA_GET_LOCK_GIDS message to controller @BASE_NAME",
		ch->base.base.name);
	if (!(ch->send_ioctx = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_ib_admin_channel_get_lock_gids_and_connect, "No free messages for administrator to use");
		goto out;
	}
	/* set the iu type */
	/* set the request */
	req = ch->send_ioctx->buf;
	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(
		ch, prp_ma_get_lock_gids, ch, NVMEIB_SEND_CFG, ch->send_ioctx)) < 0)
		goto free_iu;
	if ((recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != req->hdr.tag ||
			/* rsp->version_tag != req->version_tag ||
			   It was an invalid assumption that the version of the request and the response would be the same. In this case only the response is extended. */
			rsp->opcode != NVMEIBS_RSP_MGMT_OPCODE_OK ||
			((lock_ch = parse_read_lock_gids(ch)) == NULL)) {
			_NT(trace_1_ib_admin_channel_get_lock_gids_and_connect, "Could not parse received read_lock_gids msg - rsp->hdr.tag=@TAG, req->hdr.tag=@TAG, rsp->version_tag=@VERSION_TAG, req->version_tag=@VERSION_TAG, rsp->opcode=@OPCODE",
			rsp->hdr.tag, req->hdr.tag, be16_to_cpu(req->version_tag), be16_to_cpu(rsp->version_tag), rsp->opcode);
			rv = -EINVAL;
			goto post_recv;
		}
	}
	else {
		_NT(trace_2_ib_admin_channel_get_lock_gids_and_connect, "Failed to wait for new receivee messages");
		rv = -1;
		goto free_iu;
	}
	_NT(trace_3_ib_admin_channel_get_lock_gids_and_connect, "--- Received NVMEIBC_MA_GET_LOCK_GIDS message reply "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");
post_recv:
	_NT(trace_4_ib_admin_channel_get_lock_gids_and_connect, "post_recv");
	 nvmeib_srq_post_recv(ch->net.base.srq_info, recv_ioctx);

free_iu:
	_NT(trace_5_ib_admin_channel_get_lock_gids_and_connect, "free_iu");
	nvmeibc_ib_admin_channel_put_tx_iu(ch, ch->send_ioctx);

out:
	_NT(trace_6_ib_admin_channel_get_lock_gids_and_connect, "out");
	ch->send_ioctx = NULL;

	__NFOUT;
	return lock_ch; //rv;
}

struct nvmeibc_locks_channel *nvmeibc_ib_admin_channel_connect_lock_lb_channel(
	struct nvmeibc_ib_admin_channel *ch)
{
	struct nvmeibc_locks_channel *lock_ch = NULL;
	__NFIN;

	_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_connect_lock_lb_channel, "calling get_lock_gids_and_connect...");
	lock_ch = get_lock_gids_and_connect(ch);
	_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_channel_connect_lock_lb_channel, "get_lock_gids_and_connect - DONE!");

	__NFOUT;
	return lock_ch;
}

static void send_toma_cmd_work(struct workqe_struct *work)
{
	struct admin_toma_cmd_workq *twork =
		container_of(work, struct admin_toma_cmd_workq, work);
	struct nvmeibc_ib_admin_channel *ch = twork->ch;
	struct nvmeibc_disk_toma_cmd *toma_cmd = twork->toma_cmd;
	int rv = -1;

	__NFIN;

	if (is_ach_dying(ch)) {
		_ND(debug_ib_admin_channel_send_toma_cmd_work, " We are history - not sending toma command");
		goto out;
	}

	rv = nvmeibc_toma_send_req(ch, toma_cmd);

out:
	twork->rv = rv;
	if (twork->done) {
		complete(twork->done);
	}

	__NFOUT;
}

/**
 * Send toma cmd to server over IB towards TOMA via admin
 * channel's workq and wait for server's response i.e. wait for
 * work to complete. Implies that work must be time-bounded.
 */
int nvmeibc_ib_admin_send_toma_cmd(struct nvmeibc_ib_admin_channel *ch,
								   struct nvmeibc_disk_toma_cmd *toma_cmd)
{
	DECLARE_COMPLETION_ONSTACK(done);
	struct admin_toma_cmd_workq work;
	int rv = -1;

	__NFIN;

	/* serialize toma with other admin messages */
	WQ_INIT_WORK(&work.work, send_toma_cmd_work);
	work.ch = ch;
	work.toma_cmd = toma_cmd;
	work.done = &done;
	work.rv = 0;
	nvmeibc_admin_channel_add_work(&ch->base, &work.work);

	/* wait (without timeout) until work is completed/interrupted/timeout
	 * so the onstack-completion object remains valid when work access it.
	 */
	_ND(trace_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd, "wait for completion of synced toma-cmd (type @TOMA_CMD_TYPE)", toma_cmd->type);
	wait_for_completion(&done);
	_ND(trace_1_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd, "synced toma-cmd completion (rv = @RV)", work.rv);
	rv = work.rv;

	__NFOUT;
	return rv;
}

static inline struct admin_toma_cmd_workq *toma_cmd_workq_zalloc(
	struct nvmeibc_disk_toma_cmd *toma_cmd)
{
	struct admin_toma_cmd_workq *work = kzalloc(sizeof(*work), GFP_ATOMIC);
	struct nvmeibc_disk_toma_cmd *cmd = kzalloc(sizeof(*cmd), GFP_ATOMIC);
	bool cmd_sp = !!toma_cmd->send_params;
	struct nvmeibc_disk_toma_send_params *sp =
		cmd_sp ? kzalloc(sizeof(*sp), GFP_ATOMIC) : NULL;
	u8 *buf = sp ? kzalloc(toma_cmd->send_params->len_srvr, GFP_ATOMIC) : NULL;

	NFIN;


	if (!work || !cmd || (cmd_sp && (!sp || !buf))) {
		_NE(error_ib_admin_channel_toma_cmd_workq_zalloc, "OOM: fail to allocate work for toma cmd work item");
		kfree(buf);
		kfree(sp);
		kfree(cmd);
		kfree(work);
		work = NULL;
		goto out;
	}

	if (cmd_sp)
		sp->buf = buf;
	cmd->send_params = sp;
	work->toma_cmd = cmd;

out:
	NFOUT;
	return work;
}

static inline void toma_cmd_workq_free(struct admin_toma_cmd_workq *twork)
{
	NFIN;

	if (twork->toma_cmd->send_params) {
		kfree(twork->toma_cmd->send_params->buf);
		kfree(twork->toma_cmd->send_params);
	}
	kfree(twork->toma_cmd);
	kfree(twork);

	NFOUT;
}

static void send_toma_cmd_async_work(struct workqe_struct *work)
{
	struct admin_toma_cmd_workq *twork =
		container_of(work, struct admin_toma_cmd_workq, work);
	struct nvmeibc_ib_admin_channel *ch = twork->ch;
	struct nvmeibc_disk_toma_cmd *toma_cmd = twork->toma_cmd;
	int status;

	__NFIN;

	if (is_ach_dying(ch)) {
		_ND(trace_ib_admin_channel_send_toma_cmd_async_work, " We are history - not sending toma command");
		goto out;
	}

	if (toma_cmd->type == NVMEIB_TOMA_CMD_REG) {
		_NT(trace_ib_admin_channel_send_toma_cmd_async_work_reg, "About to send register request");
	}
	status = nvmeibc_toma_send_req(ch, toma_cmd);
	if (status) {
		/* Trigger disk release and kick s_client release */
		nvmeibc_ib_net_disconnect(&ch->net.base);
		goto out;
	}
	if (toma_cmd->type == NVMEIB_TOMA_CMD_REG) {
		/* set subscribe entry status from TOMA_ASYNC_SENT to TOMA_ALREADY_SUBSCRIBED */
		WARN_ON_ONCE(!ch->toma.async_subscribe_comp_cb);
		if (ch->toma.async_subscribe_comp_cb)
			ch->toma.async_subscribe_comp_cb(ch->base.base.disk, toma_cmd->handle, status);
		else
			_NE(error_ib_admin_channel_send_toma_cmd_async_work_reg, "OOPS, async_subscribe_comp_cb is NULL");
	}

	if (toma_cmd->type == NVMEIB_TOMA_CMD_UNREG) {
		//BUG_ON(!ch->toma.unsubscribe_comp_cb);
		if (ch->toma.unsubscribe_comp_cb)
			ch->toma.unsubscribe_comp_cb(ch->base.base.disk,
				status, toma_cmd->handle);
		else
			_NE(error_ib_admin_channel_send_toma_cmd_async_work, "OOPS, unsubscribe_comp_cb is NULL");

	}

out:
	toma_cmd_workq_free(twork);

	__NFOUT;
}

static void toma_send_params_deep_copy(struct nvmeibc_disk_toma_send_params *d,
										struct nvmeibc_disk_toma_send_params *s)
{
	u8 *buf = d->buf;
	*d = *s;	// Copy full struct
	d->buf = buf;
	memcpy(d->buf, s->buf, s->len_srvr);
}

/**
 * Send toma cmd to server over IB towards TOMA via admin
 * channel's workq w/o waiting.
 *
 */
extern bool nvmeibc_disk_use_async_subscribe;
int nvmeibc_ib_admin_send_toma_cmd_async(struct nvmeibc_ib_admin_channel *ch,
										 struct nvmeibc_disk_toma_cmd *toma_cmd)
{
	struct admin_toma_cmd_workq *work;
	struct nvmeibc_disk_toma_send_params *sp = toma_cmd->send_params;
	int rv = -1;

	__NFIN;

	if (toma_cmd->type == NVMEIB_TOMA_CMD_SEND) {
		if (!sp) {
			_NT(trace_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd_async, "Invalid send-params for toma send cmd");
			goto out;
		}
		if (sp->len_srvr > NVMEIB_TOMA_REQ_MAX_LEN) {
			_NT(trace_1_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd_async, "Invalid length for toma send cmd");
			goto out;
		}
	}
	else {
		if (toma_cmd->type != NVMEIB_TOMA_CMD_UNREG && !nvmeibc_disk_use_async_subscribe) {
			_NT(trace_2_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd_async, "Invalid toma cmd type (@NVMEIB_TOMA_CMD_STR) for send async",
			   nvmeib_toma_cmd_str(toma_cmd->type));
			goto out;
		}
		if (sp) {
			_NT(trace_3_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd_async, "Invalid toma cmd type (@NVMEIB_TOMA_CMD_STR) with send-params",
			   nvmeib_toma_cmd_str(toma_cmd->type));
			goto out;
		}
	}

	if (!(work = toma_cmd_workq_zalloc(toma_cmd))) {
		_NT(trace_4_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd_async, "OOM: fail to add work for toma send cmd");
		goto out;
	}

	_ND(trace_5_ib_admin_channel_nvmeibc_ib_admin_send_toma_cmd_async, "Add work for sending toma cmd, type = @NVMEIB_TOMA_CMD_STR",
	   nvmeib_toma_cmd_str(toma_cmd->type));

	/* serialize toma with other admin messages */
	WQ_INIT_WORK(&work->work, send_toma_cmd_async_work);
	work->ch = ch;
	work->done = NULL;
	work->rv = 0;
	//toma_cmd
	work->toma_cmd->handle = toma_cmd->handle;
	work->toma_cmd->type = toma_cmd->type;
	if (sp) {		/* Deep-Copy send_params from sp to wq work */
		toma_send_params_deep_copy(work->toma_cmd->send_params, sp);
	}

	rv = nvmeibc_admin_channel_add_work(&ch->base, &work->work);

out:
	__NFOUT;
	return rv;
}

static void disk_abnd2free_done(void *context)
{
	struct nvmeibc_disk_update_data *ud = context;
	kfree(ud);
}

int nvmeibc_ib_admin_schedule_abnd2free(struct nvmeibc_disk *disk,
					struct volume_server_req *req)
{
	int rv;
	struct nvmeibc_disk_update_data *ud = NULL;
	struct abnd2free *a2f = NULL;

	NFIN;
	ud = kzalloc(sizeof(struct nvmeibc_disk_update_data), GFP_KERNEL);
	if (!ud) {
		_NE(error_ib_admin_channel_nvmeibc_ib_admin_schedule_abnd2free, "OOM: failed to allocate memory for disk_update_data");
		rv = -ENOMEM;
		goto err;
	}
	a2f = kzalloc(sizeof(struct abnd2free), GFP_KERNEL);
	if (!a2f) {
		_NE(error_1_ib_admin_channel_nvmeibc_ib_admin_schedule_abnd2free, "OOM: failed to allocate memory for abnd2free_work");\
		rv = -ENOMEM;
		goto err;
	}

	memcpy(&a2f->jreq, &req->j_req, sizeof(struct volume_server_cmd_jmd_free_abnd));
	a2f->hdr_tag = req->hdr.tag;

	ud->update_type = DISK_UPDATE_JAM_ABND2FREE;
	ud->update_data = a2f;
	ud->done_cb = disk_abnd2free_done;
	ud->done_cb_ctx = ud;
	rv = nvmeibc_disk_update_config(disk, ud, false);
	if (!rv)
		goto out;
err:
	kfree(a2f);
	kfree(ud);
out:
	NFOUT;
	return rv;
}


/**
 * Receive toma cmd from server over IB towards TOMA.
 *
 * ch - ib admin channel holding the received toma msg
 *
 */
void nvmeibc_ib_admin_recv_toma_cmd(struct nvmeibc_ib_admin_channel *ch)
{
	struct nvmeibc_disk *disk = ch->base.base.disk;

	__NFIN;
	nvmeibc_disk_toma_recv(disk, &ch->toma.recv_msg);
	__NFOUT;
}

void nvmeibc_ib_admin_channel_trace_path(struct nvmeibc_ib_admin_channel *ch)
{
	_NT(trace_ib_admin_channel_nvmeibc_ib_admin_channel_trace_path, "Admin connection: @BASE_NAME:@GID_STR->@RANIC_GUID",
		ch->base.base.name, ch->net.base.port->gid.gid_str, ch->ranic_guid);
}

bool nvmeibc_ib_admin_is_connected(struct nvmeibc_ib_admin_channel *ch)
{
	return ch && nvmeibc_net_is_connected(&ch->net.base);
}
