/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeib_public_mlx5_imp_c

#ifndef HAVE_DEVLINK_NET
#	define HAVE_DEVLINK_NET
#	define HAVE_DEVLINK_PORT_FLAVOUR_VIRTUAL
#endif
#include <infiniband/hw/mlx5/mlx5_ib.h>
#include <linux/mlx5/cq.h>

#include "nvmeib_public.h"
#define nvmeib_debug_level nvmeib_public_mlx5_debug_level
#include "nvmeib_utils.h"

#include "kr_undef.h"

static int get_octo_len(u64 addr, u64 len, int page_size)
{
	u64 offset;
	int npages;

	offset = addr & (page_size - 1);
	npages = ALIGN(len + offset, page_size) >> ilog2(page_size);
	return (npages + 1) / 2;
}

static inline bool mlx5_dev_cap_atomic(struct mlx5_ib_dev *dev)
{
	bool atomic_supported;


#if IB_HAS_MLX5_CAP_ATOMIC_MACRO
	u8 atomic_ops = MLX5_CAP_ATOMIC(dev->mdev, atomic_operations);
	atomic_supported = (atomic_ops != IB_ATOMIC_NONE) ? true : false;
#elif IB_MLX5_DEV_CAP_FLAG_ATOMIC
	u64 cap_flags = dev->mdev->caps.flags;
	atomic_supported = (cap_flags & MLX5_DEV_CAP_FLAG_ATOMIC) ? true : false;
#else
#error MLX5 Atomics not supported
	atomic_supported = false;
#endif

	return atomic_supported;
}

static inline void *mlx5_valloc(size_t size)
{
	void *rtn;

	rtn = kzalloc(size, GFP_KERNEL | __GFP_NOWARN);
	if (!rtn)
		rtn = vzalloc(size);
	return rtn;
}

static inline void mlx5_vfree(const void *addr)
{
	if (addr && is_vmalloc_addr(addr))
		vfree(addr);
	else
		kfree(addr);
}

static inline void mlx5_set_send_wr(struct mlx5_ib_qp *qp,
	struct nvmeib_send_wr *wr,
	unsigned idx,
	u32 mlx5_opcode,
	int nreq,
	int size)
{
#if IB_MLX5_QP_SWR_CTX
	struct swr_ctx *swr = &qp->sq.swr_ctx[idx];

	swr->wrid = nvmeib_send_wr_common(*wr).wr_id;
	swr->w_list.opcode = mlx5_opcode;
	swr->wqe_head = qp->sq.head + nreq;
	qp->sq.cur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);
	swr->w_list.next = qp->sq.cur_post;
#else
	qp->sq.wrid[idx] = nvmeib_send_wr_common(*wr).wr_id;
	qp->sq.w_list[idx].opcode = mlx5_opcode;
	qp->sq.wqe_head[idx] = qp->sq.head + nreq;
	qp->sq.cur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);
	qp->sq.w_list[idx].next = qp->sq.cur_post;
#endif
}

static int mlx5_alloc_n_map(struct nvmeib_alloc_n_map *mem)
{
	struct ib_pd *pd = mem->pd;
	u64 length = ((u64)mem->n_pages) << PAGE_SHIFT;
	u64 ioaddr = mem->ioaddr;
	int access_flags = mem->access_flags;
	struct mlx5_ib_dev *dev = to_mdev(pd->device);
	struct mlx5_ib_mr *mr = NULL;
#if IB_MLX5_IFC
	u32 *in = NULL;
	__be64 *pas;
	void *mkc;
#else
	struct mlx5_create_mkey_mbox_in *in = NULL;
#endif
	int inlen;
	int i = 0, rv;
	struct sg_dma_page_iter sg_iter;
	NFIN;
	if (!mem->n_pages || !mem->pages || 
		(!mem->use_dma_pages && (!mem->mem_table.sgl || !mem->mem_table.nents))) {
		_NE_dmesg(error_6_nvmeib_public_mlx5_imp_mlx5_alloc_n_map,
			  "Invalid parameter(s) n_pages: @N_PAGES, pages: @PAGES, use_dma_pages: @BOOL_YN sgl: @PTR, nents: @NENTS",
			  mem->n_pages, mem->pages, mem->use_dma_pages, mem->mem_table.sgl, mem->mem_table.nents);
		rv = -EINVAL;
		goto out;
	}
	if ((access_flags & IB_ACCESS_REMOTE_ATOMIC) &&
		!mlx5_dev_cap_atomic(dev)) {
		_NE(error_nvmeib_public_mlx5_imp_mlx5_alloc_n_map, "atomic operations are not supported in this version");
		rv = EINVAL;
		goto out;
	}
	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
#if IB_MLX5_IFC
	inlen = MLX5_ST_SZ_BYTES(create_mkey_in) +
		sizeof(*pas) * ((mem->n_pages + 1) / 2) * 2;
#else
	inlen = sizeof(*in) + sizeof(*in->pas) * ((mem->n_pages + 1) / 2) * 2;
#endif
	in = mlx5_valloc(inlen);
	if (!in) {
		_NE(error_1_nvmeib_public_mlx5_imp_mlx5_alloc_n_map, "Failed to allocate memory region");
		rv = -ENOMEM;
		goto free_mr;
	}

#if IB_MLX5_IFC
	pas = (__be64 *)MLX5_ADDR_OF(create_mkey_in, in, klm_pas_mtt);
	if (mem->use_dma_pages) {
		for (i = 0; i < mem->n_pages; ++i)
			pas[i] = cpu_to_be64(mem->dma_pages[i]);
	} else {
		for_each_sg_dma_page(mem->mem_table.sgl, &sg_iter, mem->map_sg_nents, 0)
			pas[i++] = cpu_to_be64(sg_page_iter_dma_address(&sg_iter));
	}

	//mlx5_ib_populate_pas(dev, umem, page_shift, pas,
	//					 pg_cap ? MLX5_IB_MTT_PRESENT : 0);
	MLX5_SET(create_mkey_in, in, pg_access, 0);

	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	MLX5_SET(mkc, mkc, MLX5_ACCESS_MODE, MLX5_MKC_ACCESS_MODE_MTT);
	MLX5_SET(mkc, mkc, a, !!(access_flags & IB_ACCESS_REMOTE_ATOMIC));
	MLX5_SET(mkc, mkc, rw, !!(access_flags & IB_ACCESS_REMOTE_WRITE));
	MLX5_SET(mkc, mkc, rr, !!(access_flags & IB_ACCESS_REMOTE_READ));
	MLX5_SET(mkc, mkc, lw, !!(access_flags & IB_ACCESS_LOCAL_WRITE));
	MLX5_SET(mkc, mkc, lr, 1);

	MLX5_SET64(mkc, mkc, start_addr, ioaddr);
	MLX5_SET64(mkc, mkc, len, length);
	MLX5_SET(mkc, mkc, pd, to_mpd(pd)->pdn);
	MLX5_SET(mkc, mkc, bsf_octword_size, 0);
	MLX5_SET(mkc, mkc, translations_octword_size,
			 get_octo_len(ioaddr, length, 1 << PAGE_SHIFT));
	MLX5_SET(mkc, mkc, log_page_size, PAGE_SHIFT);
	MLX5_SET(mkc, mkc, qpn, 0xffffff);
	MLX5_SET(create_mkey_in, in, translations_octword_actual_size,
			 get_octo_len(ioaddr, length, 1 << PAGE_SHIFT));

#ifndef LLVM
#if KS_RDMA_MLX5_CORE_CREATE_MKEY_MKEY_U32
	rv = mlx5_core_create_mkey(dev->mdev, &mr->mmkey.key, in, inlen);
#else
	rv = mlx5_core_create_mkey(dev->mdev, &mr->mmkey, in, inlen);
#endif
#else
	rv = 0;
#endif
#else
	if (mem->use_dma_pages) {
		for (i = 0; i < mem->n_pages; ++i)
			in->pas[i] = cpu_to_be64(mem->dma_pages[i]);
	} else {
		for_each_sg_dma_page(mem->mem_table.sgl, &sg_iter, mem->mem_table.nents, 0)
			in->pas[i++] = cpu_to_be64(sg_page_iter_dma_address(&sg_iter));
	}

	in->flags = 0;
	in->seg.status = 0;
	in->seg.flags = convert_access(access_flags) | MLX5_ACCESS_MODE_MTT;
	in->seg.flags_pd = cpu_to_be32(to_mpd(pd)->pdn);
	in->seg.start_addr = cpu_to_be64(ioaddr);
	in->seg.len = cpu_to_be64(length);
	in->seg.bsfs_octo_size = 0;
	in->seg.xlt_oct_size = cpu_to_be32(
		get_octo_len(ioaddr, length, 1 << PAGE_SHIFT));
	in->seg.log2_page_size = PAGE_SHIFT;
	in->seg.qpn_mkey7_0 = cpu_to_be32(0xffffff << 8);
	in->xlat_oct_act_size = cpu_to_be32(
		get_octo_len(ioaddr, length, 1 << PAGE_SHIFT));
	rv = mlx5_core_create_mkey(
		dev->mdev,
#if KS_RDMA_MLX5_CORE_CREATE_MKEY_MKEY_U32
		&mr->mmkey.key,
#elif IB_MLX5_CORE_MKEY
		&mr->mmkey,
#else
		&mr->mmr,
#endif
		in, inlen, NULL, NULL, NULL);
#endif
	if (rv) {
		_NE(error_3_nvmeib_public_mlx5_imp_mlx5_alloc_n_map, "Fail to create memory region key on device @RV", rv);
		goto free_in;
	}
#if IB_MLX5_MR_HAS_DEV
	mr->dev = dev;
#endif
#if IB_MLX5_MR_HAS_LIVE
	mr->live = 1;
#else
	/* Yuri: For odp_mkeys we will need the line below. But our mkeys are not odp. */
	/* xa_store(&mr->dev->odp_mkeys, mlx5_base_mkey(mr->mmkey.key), &mr->mmkey, GFP_ATOMIC); */
#endif
#if IB_MLX5_MR_HAS_NPAGES
	mr->npages = mem->n_pages;
#endif
#if IB_MLX5_CORE_MKEY
	mr->ibmr.rkey = mr->ibmr.lkey = mr->mmkey.key;
#else
	mr->ibmr.rkey = mr->ibmr.lkey = mr->mmr.key;
#endif
#if IB_MLX5_MR_HAS_INVALIDATED
	atomic_set(&mr->invalidated, 0);
#endif
	mem->mr = &mr->ibmr;
	mem->lkey = mem->mr->lkey;
	mem->rkey = mem->mr->rkey;
	_ND(trace_nvmeib_public_mlx5_imp_mlx5_alloc_n_map, "mr_key=@MR_KEY", mem->lkey);
	goto out;
	
free_in:
	mlx5_vfree(in);

free_mr:
	kfree(mr);

out:	
	NFOUT;
	return rv;
}

static int mlx5_unmapn_n_free(struct nvmeib_alloc_n_map *mem)
{
	struct mlx5_ib_dev *dev __attribute__((unused)) = to_mdev(mem->pd->device);
	struct mlx5_ib_mr *mr __attribute__((unused));

	NFIN;
	if (mem->mr) {
		mr = container_of(mem->mr, struct mlx5_ib_mr, ibmr);
#if !IB_MLX5_MR_HAS_LIVE
		/* Yuri: For odp_mkeys we will need the line below. But our mkeys are not odp. */
		/* xa_erase(&mr->dev->odp_mkeys, mlx5_base_mkey(mr->mmkey.key)); */
#endif
#ifndef LLVM
#if KS_RDMA_MLX5_CORE_CREATE_MKEY_MKEY_U32
		mlx5_core_destroy_mkey(dev->mdev, mr->mmkey.key);
#elif IB_MLX5_CORE_MKEY
		mlx5_core_destroy_mkey(dev->mdev, &mr->mmkey);
#else
		mlx5_core_destroy_mkey(dev->mdev, &mr->mmr);
#endif
#endif
		kfree(mem->mr);
	}
	NFOUT;
	return 0;
}

#if IB_NEW_FR
static int mlx5_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	struct mlx5_ib_mr *mr5 = to_mmr(mr);
	__be64 *descs = mr5->descs;
	int n;

	NFIN;
#if KS_RDMA_MLX5_IB_MKEY_HAS_NDESCS
	mr5->mmkey.ndescs = 0;
#else
	mr5->ndescs = 0;
#endif
	/* JH IOMMU: DMA_TO_DEVICE is correct, entries are r/o for NIC */
	ib_dma_sync_single_for_cpu(ibdev, mr5->desc_map,
		mr5->desc_size * mr5->max_descs, DMA_TO_DEVICE);
	for (n = 0; n < n_pages; ++n)
	#if KS_RDMA_MLX5_IB_MKEY_HAS_NDESCS
		descs[mr5->mmkey.ndescs++] = cpu_to_be64(pages[n] | MLX5_EN_RD | MLX5_EN_WR);
	#else
		descs[mr5->ndescs++] = cpu_to_be64(pages[n] | MLX5_EN_RD | MLX5_EN_WR);
	#endif
	ib_dma_sync_single_for_device(ibdev, mr5->desc_map,
		mr5->desc_size * mr5->max_descs, DMA_TO_DEVICE);
	NFOUT;
	return 0;
}
#else
static int mlx5_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	_NE(error_nvmeib_public_mlx5_imp_mlx5_map_mr, "Calling mlx5_map_mr(...) for old_kernel or OFED installation");
	return -1;
}
#endif

#if KS_RDMA_MLX5_MISSING_MLX5_BUF_OFFSET
static inline void *mlx5_buf_offset(struct mlx5_frag_buf *buf, int offset)
{
		return buf->frags->buf + offset;
}
#endif

static void *get_cqe_from_buf(struct mlx5_ib_cq_buf *buf, int n, int size)
{
#if MLX5_IB_CQ_FRAG_BUF_CTRL
	return mlx5_frag_buf_get_wqe(&buf->fbc, n);
#else
	return mlx5_buf_offset(&buf->buf, n * size);
#endif
}

static void *get_cqe(struct mlx5_ib_cq *cq, int n)
{
	return get_cqe_from_buf(&cq->buf, n, cq->mcq.cqe_sz);
}

static void *get_sw_cqe(struct mlx5_ib_cq *cq, int n)
{
	void *cqe = get_cqe(cq, n & cq->ibcq.cqe);
	struct mlx5_cqe64 *cqe64;

	cqe64 = (cq->mcq.cqe_sz == 64) ? cqe : cqe + 64;

	if (likely((cqe64->op_own) >> 4 != MLX5_CQE_INVALID) &&
	    !((cqe64->op_own & MLX5_CQE_OWNER_MASK) ^ !!(n & (cq->ibcq.cqe + 1)))) {
		return cqe;
	} else {
		return NULL;
	}
}

static int mlx5_peek_cq(struct ib_cq *ib_cq, int max)
{
	struct mlx5_ib_cq *cq = to_mcq(ib_cq);
	u32 peek_cons = cq->mcq.cons_index;
	int n_cqe;
	unsigned long flags;

	spin_lock_irqsave(&cq->lock, flags);
	for (n_cqe = 0; n_cqe < max; n_cqe++, peek_cons++) {
		if (!get_sw_cqe(cq, peek_cons))
			break;
	}
	spin_unlock_irqrestore(&cq->lock, flags);
	return n_cqe;
}

#pragma pop_macro("__FILE_LITERAL__")
