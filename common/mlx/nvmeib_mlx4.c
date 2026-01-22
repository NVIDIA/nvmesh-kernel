/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "nvmeib_public.h"
#include "ib_incs.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib_mlx.h"
#include "nvmeib_utils.h"
#include "nvmeibs_test.h"
#include "nvmeibm_trace.h"

#include <rdma/ib_verbs.h>
#include <linux/netdevice.h>
#include <linux/mlx4/device.h>
#include <linux/mlx4/qp.h>
#include <infiniband/hw/mlx4/mlx4_ib.h>

/* Must be last to override module_{init/exit} */
#include "kr_undef.h"

const unsigned long nvmeib_mlx4_dev_caps = 
	NVMEIB_DEVCAP_RDDA | 
	NVMEIB_DEVCAP_SRQ | 
	NVMEIB_DEVCAP_SRQ_LAST_WQE |
	NVMEIB_DEVCAP_ATOMICS_REQ |
	NVMEIB_DEVCAP_ATOMICS_RESP |
	NVMEIB_DEVCAP_MASKED_ATOMICS_REQ |
	NVMEIB_DEVCAP_MASKED_ATOMICS_RESP |
	NVMEIB_DEVCAP_RD_ATOM_8;

/**
 * Copied from Mellanox mlx4
 *
 */

#ifdef MLNX_OFED
static u64 mlx4_get_db_addr(struct mlx4_ib_qp *qp)
{
#if defined(IB_RAM_TESTING) || defined(INDIRECT_INTR)
	return (u64)(qp->bf.uar->map + MLX4_SEND_DOORBELL);
#else
	return (u64)((qp->bf.uar->pfn << PAGE_SHIFT) + MLX4_SEND_DOORBELL);
#endif
}
#else
static u64 mlx4_get_db_addr(struct mlx4_ib_qp *qp)
{
	struct mlx4_ib_dev *dev = to_mdev(qp->ibqp.device);
#if defined(IB_RAM_TESTING) || defined(INDIRECT_INTR)
	return (u64)(dev->uar_map + MLX4_SEND_DOORBELL);
#else
	return (u64)((dev->priv_uar.pfn << PAGE_SHIFT) + MLX4_SEND_DOORBELL);
#endif
}
#endif

static int nvmeib_mlx4_get_qp_sqr(struct ib_device *ib_dev, struct ib_qp *ib_qp,
	struct nvmeib_sq_rsc *sqr)
{
	struct mlx4_ib_qp *qp = to_mqp(ib_qp);
	int i, rv = 0;

	NFIN;
	sqr->offset = qp->sq.offset;
	sqr->n_bufs = qp->buf.nbufs;
	sqr->n_pages = qp->buf.npages;
	if (qp->buf.nbufs == 1) {/* no page list */
		while (sqr->n_pages * PAGE_SIZE < qp->buf_size)
			++sqr->n_pages;
		if (!(sqr->pages = kzalloc(sqr->n_pages * sizeof(*sqr->pages),
			GFP_KERNEL))) {
			_NE(error_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "Fail to allocate qp sendq pages");
			rv = -ENOMEM;
			goto out;
		}
		for (i = 0; i < sqr->n_pages; ++i)
			sqr->pages[i] = qp->buf.direct.map + i * PAGE_SIZE;
	}
	else if (qp->buf.nbufs == qp->buf.npages) {/* page list */
		if (!(sqr->pages = kzalloc(qp->buf.npages * sizeof(*sqr->pages),
			GFP_KERNEL))) {
			_NE(error_1_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "Fail to allocate qp sendq pages");
			NFOUT;
			return -1;
		}
		for (i = 0; i < qp->buf.npages; ++i)
			sqr->pages[i] = qp->buf.page_list[i].map;
	}
	else {
		kfree(sqr->pages);
		sqr->pages = NULL;
		_NE(error_2_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "Invalide QP send_q format: n_bufs=@N_BUFS, n_pages=@N_PAGES",
			qp->buf.nbufs, qp->buf.npages);
		rv = -1;
		goto out;
	}
	sqr->size = qp->sq.wqe_cnt << qp->sq.wqe_shift;
	_ND(trace_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "n_pages=@N_PAGES, size=@SIZE", sqr->n_pages, sqr->size);
	sqr->doorbell_address = mlx4_get_db_addr(qp);
	sqr->doorbell_payload = qp->doorbell_qpn;
	/* info for shadowing */
	sqr->sq_wqe_shift = qp->sq.wqe_shift;
	sqr->sq_wqe_cnt = qp->sq.wqe_cnt;
	sqr->sq_spare_wqes = qp->sq_spare_wqes;
#if MLX4_IB_SQ_MULTIPLE_WQES_PER_WR
	sqr->sq_max_wqes_per_wr = qp->sq_max_wqes_per_wr;
#else
	sqr->sq_max_wqes_per_wr = 1;
#endif
	_ND(trace_1_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "sq_offset=@SQ_OFFSET", sqr->offset);
	_ND(trace_2_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "sq_n_bufs=@SQ_N_BUFS", sqr->n_bufs);
	_ND(trace_3_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "sq_n_pages=@SQ_N_PAGES", sqr->n_pages);
	_ND(trace_4_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "sq_wqe_shift=@SQ_WQE_SHIFT", sqr->sq_wqe_shift);
	_ND(trace_5_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "sq_wqe_cnt=@SQ_WQE_CNT", sqr->sq_wqe_cnt);
	_ND(trace_6_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "sq_spare_wqes=@SQ_SPARE_WQES", sqr->sq_spare_wqes);
	_ND(trace_7_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "sq_max_wqes_per_wr=@SQ_MAX_WQES_PER_WR", sqr->sq_max_wqes_per_wr);
	_ND(trace_8_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "doorbell_payload=@DOORBELL_PAYLOAD", sqr->doorbell_payload);
	_ND(trace_9_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "qpn=@QPN", qp->mqp.qpn);
	_ND(trace_10_nvmeib_mlx4_nvmeib_mlx4_get_qp_sqr, "doorbell_payload=@DOORBELL_PAYLOAD", qp->doorbell_qpn);

out:
	NFOUT;
	return rv;
}

static int nvmeib_mlx4_get_qp_cqr(struct ib_device *ib_dev, struct ib_cq *ib_cq,
	struct nvmeib_cq_rsc *cqr)
{
	struct mlx4_ib_cq *cq;

	NFIN;
	cq = to_mcq(ib_cq);
	cqr->ci_db_addr = cq->db.dma;
	cqr->cons_index = cq->mcq.cons_index;
	NFOUT;
	return 0;
}

static int nvmeib_mlx4_init_qp(struct ib_device *ib_dev, struct ib_qp *ib_qp,
	u64 wr_id, u32 *opcode, struct nvmeib_sq_rsc *sqr)
{
	struct mlx4_ib_qp *qp = to_mqp(ib_qp);
	struct mlx4_ib_wq *sq = &qp->sq;
	int i;

	NFIN;
	sqr->last_pos = qp->sq_next_wqe;
	sqr->head = qp->sq.head;
	
	/* Set the SQ WR IDs to be the SQ WQE Index */
	for (i = 0; i < sq->wqe_cnt; i++)
		sq->wrid[i] = i;

	_ND(trace_nvmeib_mlx4_nvmeib_mlx4_init_qp, "sq_next_wqe=@SQ_NEXT_WQE", qp->sq_next_wqe);
	_ND(trace_1_nvmeib_mlx4_nvmeib_mlx4_init_qp, "sq_head=@SQ_HEAD", qp->sq.head);
	NFOUT;
	return 0;
}

struct mlx4_shadow_qp {
	struct mlx4_ib_qp qp;
};

static __always_inline void *_mlx4_buf_offset(struct mlx4_buf *buf, int offset)
{
	return buf->direct.buf + offset;
}

static __always_inline void *_get_wqe(struct mlx4_ib_qp *qp, int offset)
{
	return _mlx4_buf_offset(&qp->buf, offset);
}

static __always_inline void *_get_send_wqe(struct mlx4_ib_qp *qp, int n)
{
	return _get_wqe(qp, qp->sq.offset + (n << qp->sq.wqe_shift));
}

/*
 * Stamp a SQ WQE so that it is invalid if prefetched by marking the
 * first four bytes of every 64 byte chunk with
 *     0x7FFFFFF | (invalid_ownership_value << 31).
 *
 * When the max work request size is less than or equal to the WQE
 * basic block size, as an optimization, we can stamp all WQEs with
 * 0xffffffff, and skip the very first chunk of each WQE.
 */
static void _stamp_send_wqe(struct mlx4_ib_qp *qp, int n, int size)
{
	__be32 *wqe;
	int i;
	int s;
	int ind;
	void *buf;
	__be32 stamp;
	struct mlx4_wqe_ctrl_seg *ctrl;

//	NFIN;
#if MLX4_IB_SQ_MULTIPLE_WQES_PER_WR
	if (qp->sq_max_wqes_per_wr > 1) {
#else
	if (0) {
#endif
		s = roundup(size, 1U << qp->sq.wqe_shift);
		for (i = 0; i < s; i += 64) {
			ind = (i >> qp->sq.wqe_shift) + n;
			stamp = ind & qp->sq.wqe_cnt ? cpu_to_be32(0x7fffffff) :
				cpu_to_be32(0xffffffff);
			buf = _get_send_wqe(qp, ind & (qp->sq.wqe_cnt - 1));
			wqe = buf + (i & ((1 << qp->sq.wqe_shift) - 1));
			*wqe = stamp;
		}
	} else {
		ctrl = buf = _get_send_wqe(qp, n & (qp->sq.wqe_cnt - 1));
#	if IB_MLX_FENCE_VLAN
		s = (ctrl->qpn_vlan.fence_size & 0x3f) << 4;
#	else
		s = (ctrl->fence_size & 0x3f) << 4;
#	endif
		for (i = 64; i < s; i += 64) {
			wqe = buf + i;
			*wqe = cpu_to_be32(0xffffffff);
		}
	}
//	NFOUT;
}

static void nvmeib_mlx4_init_stamp_shadow_qp(struct mlx4_ib_qp *qp, int s,
	int e)
{
	struct mlx4_wqe_ctrl_seg *ctrl;
	int i;

	NFIN;
	_ND(trace_nvmeib_mlx4_nvmeib_mlx4_init_stamp_shadow_qp, "s=@START_PORT, e=@END_PORT", s, e);
	for (i = s; i < e; ++i) {
		ctrl = _get_send_wqe(qp, i);
		ctrl->owner_opcode = cpu_to_be32(1 << 31);
#if MLX4_IB_SQ_MULTIPLE_WQES_PER_WR
		if (qp->sq_max_wqes_per_wr == 1)
#else
		if (1)
#endif
#if IB_MLX_FENCE_VLAN
			ctrl->qpn_vlan.fence_size = 1 << (qp->sq.wqe_shift - 4);
#else
			ctrl->fence_size = 1 << (qp->sq.wqe_shift - 4);
#endif

		_stamp_send_wqe(qp, i, 1 << qp->sq.wqe_shift);
	}
	NFOUT;
}

static void nvmeib_mlx4_stamp_shadow_qp(struct mlx4_ib_qp *qp, int s,
	int e)
{
	struct mlx4_wqe_ctrl_seg *ctrl;
	int i;

	NFIN;
	_ND(trace_nvmeib_mlx4_nvmeib_mlx4_stamp_shadow_qp, "s=@START_PORT, e=@END_PORT", s, e);
	for (i = s; i < e; ++i) {
		ctrl = _get_send_wqe(qp, i);
#if IB_MLX_FENCE_VLAN
		_stamp_send_wqe(qp, i, ctrl->qpn_vlan.fence_size << 4);
#else
		_stamp_send_wqe(qp, i, ctrl->fence_size << 4);
#endif
	}
	NFOUT;
}

static int nvmeib_mlx4_init_remote_qp_shadow(struct nvmeibc_remote_net *rnet,
	void **priv)
{
	struct mlx4_shadow_qp *sqp;
	struct mlx4_ib_qp *qp;

	NFIN;
	if (!(sqp = kzalloc(sizeof(*sqp), GFP_KERNEL))) {
		_NE(error_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "Fail to allocate shadow qp");
		NFOUT;
		return -ENOMEM;
	}
	sqp->qp.buf.direct.buf = rnet->rsq;
	sqp->qp.sq.offset = 0;
	sqp->qp.sq.wqe_shift = rnet->sq_wqe_shift;
	sqp->qp.sq.wqe_cnt = rnet->sq_wqe_cnt;
	sqp->qp.sq_spare_wqes = rnet->sq_spare_wqes;
#if MLX4_IB_SQ_MULTIPLE_WQES_PER_WR
	sqp->qp.sq_max_wqes_per_wr = rnet->sq_max_wqes_per_wr;
#endif
	sqp->qp.sq_next_wqe = rnet->sq_last_pos;
	sqp->qp.sq.head = rnet->sq_head;
	sqp->qp.sq.tail = 0;
	rnet->rsqe = sqp->qp.buf.direct.buf + sqp->qp.sq.offset +
		(sqp->qp.sq.wqe_cnt << sqp->qp.sq.wqe_shift);

	/*
	 * Before passing a kernel QP to the HW, make sure that the
	 * ownership bits of the send queue are set and the SQ
	 * headroom is stamped so that the hardware doesn't start
	 * processing stale work requests.
	 */
	qp = &sqp->qp;
	nvmeib_mlx4_init_stamp_shadow_qp(qp, 0, qp->sq.wqe_cnt);
	*priv = sqp;

	_ND(trace_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_vaddr=@SQ_VADDR", (u64)sqp->qp.buf.direct.buf);
	_ND(trace_1_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_offset=@SQ_OFFSET", sqp->qp.sq.offset);
	_ND(trace_2_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_wqe_shift=@SQ_WQE_SHIFT", sqp->qp.sq.wqe_shift);
	_ND(trace_3_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_wqe_cnt=@SQ_WQE_CNT", sqp->qp.sq.wqe_cnt);
	_ND(trace_4_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_spare_wqes=@SQ_SPARE_WQES", sqp->qp.sq_spare_wqes);
#if MLX4_IB_SQ_MULTIPLE_WQES_PER_WR
	_ND(nvmeib_mlx4_init_remote_qp_shadow_d1, "sq_max_wqes_per_wr=@INT", sqp->qp.sq_max_wqes_per_wr);
#endif
	_ND(trace_5_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_next_wqe=@SQ_NEXT_WQE", sqp->qp.sq_next_wqe);
	_ND(trace_6_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_head=@SQ_HEAD", sqp->qp.sq.head);
	_ND(trace_7_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_tail=@SQ_TAIL", sqp->qp.sq.tail);
	_ND(trace_8_nvmeib_mlx4_nvmeib_mlx4_init_remote_qp_shadow, "sq_start=@SQ_START, sq_end=@SQ_END", (u64)rnet->rsq, (u64)rnet->rsqe);

	NFOUT;
	return 0;
}

static const __be32 _mlx4_ib_opcode[] = {
	[IB_WR_SEND] = cpu_to_be32(MLX4_OPCODE_SEND),
	[IB_WR_LSO]	= cpu_to_be32(MLX4_OPCODE_LSO),
	[IB_WR_SEND_WITH_IMM] = cpu_to_be32(MLX4_OPCODE_SEND_IMM),
	[IB_WR_RDMA_WRITE] = cpu_to_be32(MLX4_OPCODE_RDMA_WRITE),
	[IB_WR_RDMA_WRITE_WITH_IMM] = cpu_to_be32(MLX4_OPCODE_RDMA_WRITE_IMM),
	[IB_WR_RDMA_READ] = cpu_to_be32(MLX4_OPCODE_RDMA_READ),
	[IB_WR_ATOMIC_CMP_AND_SWP] = cpu_to_be32(MLX4_OPCODE_ATOMIC_CS),
	[IB_WR_ATOMIC_FETCH_AND_ADD] = cpu_to_be32(MLX4_OPCODE_ATOMIC_FA),
	[IB_WR_SEND_WITH_INV] = cpu_to_be32(MLX4_OPCODE_SEND_INVAL),
	[IB_WR_LOCAL_INV] = cpu_to_be32(MLX4_OPCODE_LOCAL_INVAL),
#if IB_NEW_FR
	[IB_WR_REG_MR] = cpu_to_be32(MLX4_OPCODE_FMR),
#else
	[IB_WR_FAST_REG_MR] = cpu_to_be32(MLX4_OPCODE_FMR),
#endif
	[IB_WR_MASKED_ATOMIC_CMP_AND_SWP] =
		cpu_to_be32(MLX4_OPCODE_MASKED_ATOMIC_CS),
	[IB_WR_MASKED_ATOMIC_FETCH_AND_ADD]	=
		cpu_to_be32(MLX4_OPCODE_MASKED_ATOMIC_FA),
#if IB_HAS_BIND_MW
	[IB_WR_BIND_MW] = cpu_to_be32(MLX4_OPCODE_BIND_MW),
#endif
};

static __always_inline __be32 _send_ieth(struct nvmeib_send_wr *wr)
{
	__be32 rv;

	NFIN;
	switch (nvmeib_send_wr_common(*wr).opcode) {
	case IB_WR_SEND_WITH_IMM:
	case IB_WR_RDMA_WRITE_WITH_IMM:
		rv = nvmeib_send_wr_ex(*wr).imm_data;
		break;
	default:
		rv = 0;
		break;
	}
	NFOUT;
	return rv;
}

static __always_inline void _set_raddr_seg(struct mlx4_wqe_raddr_seg *rseg,
	u64 remote_addr, u32 rkey)
{
	rseg->raddr    = cpu_to_be64(remote_addr);
	rseg->rkey     = cpu_to_be32(rkey);
	rseg->reserved = 0;
}

static __always_inline void _set_data_seg(struct mlx4_wqe_data_seg *dseg,
	struct ib_sge *sg)
{
	dseg->lkey       = cpu_to_be32(sg->lkey);
	dseg->addr       = cpu_to_be64(sg->addr);
	dseg->byte_count = cpu_to_be32(sg->length);
}

#if MLX4_IB_SQ_MULTIPLE_WQES_PER_WR
static void _post_nop_wqe(struct mlx4_ib_qp *qp, int n, int size)
{
	struct mlx4_wqe_ctrl_seg *ctrl;
	struct mlx4_wqe_inline_seg *inl;
	void *wqe;
	int s;

	NFIN;
	ctrl = wqe = _get_send_wqe(qp, n & (qp->sq.wqe_cnt - 1));
	s = sizeof(struct mlx4_wqe_ctrl_seg);

	/* Pad the remainder of the WQE with an inline data segment. */
	if (size > s) {
		inl = wqe + s;
		inl->byte_count = cpu_to_be32(1 << 31 | (size - s - sizeof *inl));
	}
	else
		size = s;
	ctrl->srcrb_flags = 0;
#if IB_MLX_FENCE_VLAN
	ctrl->qpn_vlan.fence_size = size / 16;
#else
	ctrl->fence_size = size / 16;
#endif
	ctrl->owner_opcode = cpu_to_be32(MLX4_OPCODE_NOP | MLX4_WQE_CTRL_NEC) |
		(n & qp->sq.wqe_cnt ? cpu_to_be32(1 << 31) : 0);

	/*_stamp_send_wqe(qp, n + qp->sq_spare_wqes, size);*/
	NFOUT;
}
#endif

/* Post NOP WQE to prevent wrap-around in the middle of WR */
static __always_inline unsigned _pad_wraparound(struct mlx4_ib_qp *qp, int ind)
{
	unsigned __attribute__((unused)) s = qp->sq.wqe_cnt - (ind & (qp->sq.wqe_cnt - 1));
	NFIN;
#if MLX4_IB_SQ_MULTIPLE_WQES_PER_WR
	if (unlikely(s < qp->sq_max_wqes_per_wr)) {
		_post_nop_wqe(qp, ind, s << qp->sq.wqe_shift);
		ind += s;
	}
#endif
	NFOUT;
	return ind;
}

static int nvmeib_mlx4_clear_remote_qp_shadow(struct nvmeibc_remote_net *rnet,
	void *priv)
{
	struct mlx4_shadow_qp *sqp = priv;
	struct mlx4_ib_qp *qp = &sqp->qp;
	int s, e;

	NFIN;
	_ND(trace_nvmeib_mlx4_nvmeib_mlx4_clear_remote_qp_shadow, "s=@LAST_POST_START_IND, e=@LAST_POST_END_IND", rnet->last_post_start_ind, rnet->last_post_end_ind);
	s = rnet->last_post_start_ind;
	if (rnet->last_post_end_ind < rnet->last_post_start_ind) {
		e = qp->sq.wqe_cnt;
		nvmeib_mlx4_stamp_shadow_qp(qp, s, e);
		s = 0;
	}
	e = rnet->last_post_end_ind;
	nvmeib_mlx4_stamp_shadow_qp(qp, s, e);

	NFOUT;
	return 0;
}

static int nvmeib_mlx4_free_remote_qp_shadow(struct nvmeibc_remote_net *rnet,
	void *priv)
{
	struct mlx4_shadow_qp *sqp = priv;

	NFIN;
	kfree(sqp);
	NFOUT;
	return 0;
}

static int nvmeib_mlx4_send_remote_qp_shadow(struct nvmeibc_remote_net *rnet,
	void *priv, struct nvmeib_send_wr *wr)
{
	struct mlx4_shadow_qp *sqp = priv;
	struct mlx4_ib_qp *qp = &sqp->qp;
	void *wqe;
	struct mlx4_wqe_ctrl_seg *ctrl;
	struct mlx4_wqe_data_seg *dseg;
	int nreq;
	int err = 0;
	unsigned ind;
	int stamp = 0;  /* GCC */
	int size = 0;  /* GCC */
	int i;
	void *start;
	void *end;
	struct ib_sge *rsq_sge;
	struct nvmeib_rdma_iu *rsq_riu;

	NFIN;
	ind = qp->sq_next_wqe;
	rnet->last_post_start_ind = ind & (qp->sq.wqe_cnt - 1);
	start = end = _get_send_wqe(qp, ind & (qp->sq.wqe_cnt - 1));
	qp->sq.tail = qp->sq.head;
	_ND(trace_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "ind=@IND, start=end=@END_PTR, sq_start=@SQ_START_PTR, sq_end=@SQ_END_PTR",
		ind, start, rnet->rsq, rnet->rsqe);

	for (nreq = 0; wr; ++nreq, wr = nvmeib_send_wr_next_ptr(*wr)) {
		_ND(trace_1_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "nreq=@NREQ", nreq);
		ctrl = wqe = _get_send_wqe(qp, ind & (qp->sq.wqe_cnt - 1));
		ctrl->srcrb_flags = 0;
		ctrl->imm = _send_ieth(wr);
		wqe += sizeof(*ctrl);
		size = sizeof(*ctrl) / 16;
		_ND(trace_2_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "imm=@IMM, size=@SIZE", ctrl->imm, size);

		switch (nvmeib_send_wr_common(*wr).opcode) {
		case IB_WR_RDMA_WRITE:
		case IB_WR_RDMA_WRITE_WITH_IMM:
			_set_raddr_seg(wqe, nvmeib_send_wr_rdma(*wr).remote_addr, nvmeib_send_wr_rdma(*wr).rkey);
			wqe  += sizeof (struct mlx4_wqe_raddr_seg);
			size += sizeof (struct mlx4_wqe_raddr_seg) / 16;
			_ND(nvmeib_mlx4_send_remote_qp_shadow_d1, "size=@INT", size);
			break;
		default:
			/* No extra segments required for sends */
			break;
		}

		/*
		 * Write data segments in reverse order, so as to
		 * overwrite cacheline stamp last within each
		 * cacheline.  This avoids issues with WQE
		 * prefetching.
		 */

		dseg = wqe;
		dseg += nvmeib_send_wr_common(*wr).num_sge - 1;
		size += nvmeib_send_wr_common(*wr).num_sge * (sizeof (struct mlx4_wqe_data_seg) / 16);
		_ND(trace_3_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "size=@SIZE", size);

		for (i = nvmeib_send_wr_common(*wr).num_sge - 1; i >= 0; --i, --dseg)
			_set_data_seg(dseg, nvmeib_send_wr_common(*wr).sg_list + i);
		ctrl->owner_opcode = _mlx4_ib_opcode[nvmeib_send_wr_common(*wr).opcode] |
			(ind & qp->sq.wqe_cnt ? cpu_to_be32(1 << 31) : 0);

#if IB_MLX_FENCE_VLAN
		ctrl->qpn_vlan.fence_size = size;
#else
		ctrl->fence_size = size;
#endif

		stamp = ind + qp->sq_spare_wqes;
		ind += DIV_ROUND_UP(size * 16, 1U << qp->sq.wqe_shift);
		_ND(trace_4_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "stamp=@STAMP, ind=@IND", stamp, ind);

		/*_stamp_send_wqe(qp, stamp, size * 16);*/
		ind = _pad_wraparound(qp, ind);
		_ND(trace_5_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "ind=@IND", ind);
	}
	_ND(trace_6_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "nreq=@NREQ", nreq);
	_ND(trace_7_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "ind=@IND", ind);
#if 0
	/* we add a nop in case nreq is even */
	if (!(nreq & 1)) {
		_post_nop_wqe(qp, ind, 0);
		++ind;
		++nreq;
	}
#endif
	if (likely(nreq)) {
		int len2 = (qp->sq.wqe_cnt >> 1);
		void *middle = _get_send_wqe(qp, len2);;
		len2 <<= qp->sq.wqe_shift;
		qp->sq.head += nreq;
		qp->sq_next_wqe = ind;
		rnet->last_post_end_ind = ind & (qp->sq.wqe_cnt - 1);
		end = _get_send_wqe(qp, ind & (qp->sq.wqe_cnt - 1));
		nvmeib_mlx_calc_riu(rnet, start, end);

		// When we pass the middle of the queue, we release the first half
		// When we pass the end of the queue, we release the second half
		_ND(trace_8_nvmeib_mlx4_nvmeib_mlx4_send_remote_qp_shadow, "start=@START_PTR, middle=@MIDDLE, end=@END_PTR, len2=@LEN2", start, middle, end, len2);
		rnet->send_cleanup = false;
		if (end >= middle && start < middle) {
			rsq_sge = &rnet->rsq_clr_sge1;
			rsq_riu = &rnet->rsq_clr_riu1;
			rsq_sge->addr = rnet->rsq_dma;
			rsq_sge->length = len2;
			rsq_riu->raddr = rnet->remote_sendq_buffer_raddr;
			rnet->send_cleanup = true;
		}
		else if (end <= start){
			rsq_sge = &rnet->rsq_clr_sge1;
			rsq_riu = &rnet->rsq_clr_riu1;
			rsq_sge->addr = rnet->rsq_dma + len2;
			rsq_sge->length = len2;
			rsq_riu->raddr = rnet->remote_sendq_buffer_raddr + len2;
			rnet->send_cleanup = true;
		}
	}
	NFOUT;
	return err;
}

static int nvmeib_mlx4_check_rdda_fw(struct ib_device *ib_dev)
{
	return 0;
}

static struct nvmeib_device_ops mlx4 = {
	.module = THIS_MODULE,
	.check_rdda_fw = nvmeib_mlx4_check_rdda_fw,
};

int nvmeib_mlx4_init(void)
{
	return nvmeib_ibdr_hwdev_register(DT_mlx4, "mlx4", &mlx4, nvmeib_mlx4_dev_caps, INT_MAX);
}

void nvmeib_mlx4_cleanup(void)
{
	nvmeib_ibdr_hwdev_unregister(DT_mlx4);
}
