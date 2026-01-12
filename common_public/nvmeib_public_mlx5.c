#include "nvmeib_ib_driver.h"
#include "nvmeib_public_mlx5p.h"
#ifndef NO_OFED
#include <linux/compat-2.6.h>
#endif
#include "kr_incs.h"
#include "nvmeibp_trace.h"

MODULE_AUTHOR("Excelero");
MODULE_DESCRIPTION("NVMeIB Public mlx5");
MODULE_LICENSE("GPL and additional rights");

#if !KS_HAS_MMIOWB
#define mmiowb() barrier()
#endif

#define DEBUG_LEVEL (int)0
static int (*debug_level_f)(void);

static __attribute__ ((__unused__)) int nvmeib_public_mlx5_debug_level(void)
{
	return debug_level_f ? debug_level_f() : DEBUG_LEVEL;
}

static void nvmeib_public_mlx5_set_debug_level(int (*dlf)(void))
{
	debug_level_f = dlf;
}

#if defined(IB_MLX5) || defined(CONFIG_MLX5_INFINIBAND)
#ifdef KS_HAS_DEVLINK_H
#	include <net/devlink.h>
#endif
#include "nvmeib_public_mlx5_imp.c"

#if defined(IB_MLX5_WRITE64_HAS_DB_LOCK) && (IB_MLX5_WRITE64_HAS_DB_LOCK == 0)
#if BITS_PER_LONG == 64

#define mlx5_write64(_val, _dest, _lock) mlx5_write64(_val, _dest)

#else
#error MLNX_OFED_4.7 is not supported on 32 bits. Need to check mlx5_write64() lock protection to add support.
#endif

#endif

enum {
	MLX5_IB_SQ_STRIDE	= 6,
	MLX5_IB_CACHE_LINE_SIZE	= 64,
	MLX5_OPCODE_EXT_ATOMICS_32BIT = 0x08,
	MLX5_OPCODE_EXT_ATOMICS_64BIT = 0x09,
};

static int
nvmeib_public_mlx5_alloc_n_map(struct nvmeib_alloc_n_map *mem)
{
	return mlx5_alloc_n_map(mem);
}

static int
nvmeib_public_mlx5_unmapn_n_free(struct nvmeib_alloc_n_map *mem)
{
	return mlx5_unmapn_n_free(mem);
}

static int
nvmeib_public_mlx5_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	return mlx5_map_mr(ibdev, mr, pages, n_pages);
}

static int
nvmeib_public_mlx5_query_device(struct ib_device *ibdev,
	struct ib_device_attr *props)
{
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	int rv;

	rv = ib_query_device(ibdev, props);
	if (likely(!rv)) {
		props->atomic_cap = mlx5_dev_cap_atomic(dev);
		props->masked_atomic_cap = IB_ATOMIC_HCA;
	}

	return rv;
}

static int mlx5_wq_overflow(struct mlx5_ib_wq *wq, int nreq,
	struct ib_cq *ib_cq)
{
	struct mlx5_ib_cq *cq;
	unsigned cur;

	cur = wq->head - wq->tail;
	if (likely(cur + nreq < wq->max_post))
		return 0;

	cq = to_mcq(ib_cq);
	spin_lock(&cq->lock);
	cur = wq->head - wq->tail;
	spin_unlock(&cq->lock);

	return cur + nreq >= wq->max_post;
}

static void set_data_ptr_seg(struct mlx5_wqe_data_seg *dseg, struct ib_sge *sg)
{
	dseg->byte_count = cpu_to_be32(sg->length);
	dseg->lkey       = cpu_to_be32(sg->lkey);
	dseg->addr       = cpu_to_be64(sg->addr);
}

static __always_inline void set_raddr_seg(struct mlx5_wqe_raddr_seg *rseg,
	u64 remote_addr, u32 rkey)
{
	rseg->raddr    = cpu_to_be64(remote_addr);
	rseg->rkey     = cpu_to_be32(rkey);
	rseg->reserved = 0;
}

static void set_atomic_seg(struct mlx5_wqe_atomic_seg *aseg, struct nvmeib_send_wr *wr)
{
	if (nvmeib_send_wr_common(*wr).opcode == IB_WR_ATOMIC_CMP_AND_SWP) {
		aseg->swap_add = cpu_to_be64(nvmeib_send_wr_atomic(*wr).swap);
		aseg->compare  = cpu_to_be64(nvmeib_send_wr_atomic(*wr).compare_add);
	} else if (nvmeib_send_wr_common(*wr).opcode == IB_WR_MASKED_ATOMIC_FETCH_AND_ADD) {
		aseg->swap_add = cpu_to_be64(nvmeib_send_wr_atomic(*wr).compare_add);
		aseg->compare  = cpu_to_be64(nvmeib_send_wr_atomic(*wr).compare_add_mask);
	} else {
		aseg->swap_add = cpu_to_be64(nvmeib_send_wr_atomic(*wr).compare_add);
		aseg->compare  = 0;
	}
}

static void set_masked_atomic_seg(struct mlx5_wqe_masked_atomic_seg *aseg,
	struct nvmeib_send_wr *wr)
{
	aseg->swap_add		= cpu_to_be64(nvmeib_send_wr_atomic(*wr).swap);
	aseg->swap_add_mask	= cpu_to_be64(nvmeib_send_wr_atomic(*wr).swap_mask);
	aseg->compare		= cpu_to_be64(nvmeib_send_wr_atomic(*wr).compare_add);
	aseg->compare_mask	= cpu_to_be64(nvmeib_send_wr_atomic(*wr).compare_add_mask);
}

static u8 calc_sig(void *wqe, int size)
{
	u8 *p = wqe;
	u8 res = 0;
	int i;

	for (i = 0; i < size; i++)
		res ^= p[i];

	return ~res;
}

static u8 wq_sig(void *wqe)
{
	return calc_sig(wqe, (*((u8 *)wqe + 8) & 0x3f) << 4);
}

#if !MLX5_IB_WQ_FRAG_BUF_CTRL
static void *get_wqe(struct mlx5_ib_qp *qp, int offset)
{
	return mlx5_buf_offset(&qp->buf, offset);
}

void *mlx5_get_send_wqe(struct mlx5_ib_qp *qp, int n)
{
	return get_wqe(qp, qp->sq.offset + (n << MLX5_IB_SQ_STRIDE));
}

#else

/* get_sq_edge - Get the next nearby edge.
 *
 * Edge can be an end of a page or the end of the SQ.
 * The edge points to an overrun address (e.g. page's last address + 1) so
 * during the WQE construction which repetitively increases the pointer
 * to the place to write the next data, it simply should check if it got
 * to an overrun address.
 *
 * @idx - SQ buffer offset [strides]
 *
 * Return:
 *	The new edge.
 */
static void *get_sq_edge(struct mlx5_ib_wq *sq, u32 idx)
{
	u32 last_frag_stride_idx;
	u32 qend_frag_stride_idx;
	void *edge;

	last_frag_stride_idx = (idx + sq->fbc.strides_offset) | sq->fbc.frag_sz_m1;
	qend_frag_stride_idx = (sq->wqe_cnt - 1) + sq->fbc.strides_offset;

	if (last_frag_stride_idx < qend_frag_stride_idx)
		edge = mlx5_frag_buf_get_wqe(&sq->fbc,
				last_frag_stride_idx - sq->fbc.strides_offset);
	else
		edge = mlx5_frag_buf_get_wqe(&sq->fbc, sq->wqe_cnt - 1);

	return edge + MLX5_SEND_WQE_BB;
}

static void _handle_post_send_edge(struct mlx5_ib_wq *sq, void **seg,
				   u32 wqe_sz, void **cur_edge)
{
	u32 idx;

	idx = (sq->cur_post + (wqe_sz >> 2)) & (sq->wqe_cnt - 1);
	*cur_edge = get_sq_edge(sq, idx);

	*seg = mlx5_frag_buf_get_wqe(&sq->fbc, idx);
}

/* handle_post_send_edge - Check if we get to SQ edge. If yes, update to the
 * next nearby edge and get new address translation for current WQE position.
 * @seg: Address inside the current WQE to fill data.
 * @wqe_sz: Total size was written inside the WQE [16B] and must be aligned
 * to 4.
 */
static inline void handle_post_send_edge(struct mlx5_ib_wq *sq, void **seg,
					 u32 wqe_sz, void **cur_edge)
{
	if (likely(*seg != *cur_edge))
		return;

	_handle_post_send_edge(sq, seg, wqe_sz, cur_edge);
}
#endif

static int mlx5_post_send_atomic(struct ib_qp *ibqp,
	struct nvmeib_send_wr *wr,
	struct nvmeib_send_wr **bad_wr)
{
	struct mlx5_wqe_ctrl_seg *ctrl = NULL;  /* compiler warning */
	struct mlx5_ib_dev *dev = to_mdev(ibqp->device);
	struct mlx5_ib_qp *qp = to_mqp(ibqp);
#if IB_MLX5_NEW_BF
	struct mlx5_bf *bf = &qp->bf;
#else
	struct mlx5_bf *bf = qp->bf;
#endif
	int size = 0;  /* GCC */
#if MLX5_IB_WQ_FRAG_BUF_CTRL
	void *cur_edge;
#else
	void *qend = qp->sq.qend;
#endif
	unsigned long flags;
	u32 mlx5_opcode;
	u8 mlx5_opmod = 0;
	unsigned idx;
	int err = 0;
	int num_sge;
	void *seg;
	int nreq;
	int i;

	spin_lock_irqsave(&qp->sq.lock, flags);

	if (unlikely(ibqp->qp_type != IB_QPT_RC)) {
		mlx5_ib_warn(dev, "\n");
		err = -EINVAL;
		*bad_wr = wr;
		nreq = 0;
		goto out;
	}

	for (nreq = 0; wr; nreq++, wr = nvmeib_send_wr_next_ptr(*wr)) {

		if (unlikely(mlx5_wq_overflow(&qp->sq, nreq, qp->ibqp.send_cq))) {
			mlx5_ib_warn(dev, "nreq:%d wq_overflow\n", nreq);
			err = -ENOMEM;
			*bad_wr = wr;
			goto out;
		}

		num_sge = nvmeib_send_wr_common(*wr).num_sge;
		if (unlikely(num_sge != 1)) {
			mlx5_ib_warn(dev, "nreq:%d num_sge:%d\n", nreq, num_sge);
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		if (unlikely(nvmeib_send_wr_common(*wr).send_flags & IB_SEND_INLINE)) {
			mlx5_ib_warn(dev, "nreq:%d inline requested\n", nreq);
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		idx = qp->sq.cur_post & (qp->sq.wqe_cnt - 1);
		/* NVMESH-6851 Later when completion arrive the driver will check for this value */
		qp->sq.wr_data[idx] = nvmeib_send_wr_common(*wr).opcode;
#if MLX5_IB_WQ_FRAG_BUF_CTRL
		seg = mlx5_frag_buf_get_wqe(&qp->sq.fbc, idx);
		cur_edge = qp->sq.cur_edge;
#else
		seg = mlx5_get_send_wqe(qp, idx);
#endif
		ctrl = seg;
		*(uint32_t *)(seg + 8) = 0;
		ctrl->imm = 0;
		ctrl->fm_ce_se = qp->sq_signal_bits |
			(nvmeib_send_wr_common(*wr).send_flags & IB_SEND_SIGNALED ?
			 MLX5_WQE_CTRL_CQ_UPDATE : 0) |
			(nvmeib_send_wr_common(*wr).send_flags & IB_SEND_SOLICITED ?
			 MLX5_WQE_CTRL_SOLICITED : 0);

		seg += sizeof(*ctrl);
		size = sizeof(*ctrl) / 16;

		switch (nvmeib_send_wr_common(*wr).opcode) {

		case IB_WR_ATOMIC_CMP_AND_SWP:
		case IB_WR_ATOMIC_FETCH_AND_ADD:
			mlx5_opcode = (nvmeib_send_wr_common(*wr).opcode == IB_WR_ATOMIC_CMP_AND_SWP) ?
							MLX5_OPCODE_ATOMIC_CS : MLX5_OPCODE_ATOMIC_FA;
			set_raddr_seg(seg, nvmeib_send_wr_atomic(*wr).remote_addr,
						  nvmeib_send_wr_atomic(*wr).rkey);
			seg  += sizeof(struct mlx5_wqe_raddr_seg);
			size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
#if MLX5_IB_WQ_FRAG_BUF_CTRL
			handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);
#endif

			set_atomic_seg(seg, wr);
			seg  += sizeof(struct mlx5_wqe_atomic_seg);
			size += sizeof(struct mlx5_wqe_atomic_seg) / 16;
			break;

		case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
			mlx5_opcode = MLX5_OPCODE_ATOMIC_MASKED_CS;
			set_raddr_seg(seg, nvmeib_send_wr_atomic(*wr).remote_addr,
						  nvmeib_send_wr_atomic(*wr).rkey);
			seg  += sizeof(struct mlx5_wqe_raddr_seg);
			size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
#if MLX5_IB_WQ_FRAG_BUF_CTRL
			handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);
#endif

			set_masked_atomic_seg(seg, wr);
			seg  += sizeof(struct mlx5_wqe_masked_atomic_seg);
			size += sizeof(struct mlx5_wqe_masked_atomic_seg) / 16;
			mlx5_opmod = MLX5_OPCODE_EXT_ATOMICS_64BIT;
			break;

		default:
			mlx5_ib_warn(dev, "nreq:%d unexpectd op:%d\n", nreq,
				     nvmeib_send_wr_common(*wr).opcode);
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

#if MLX5_IB_WQ_FRAG_BUF_CTRL
		handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);
#else
		if (unlikely(seg == qend))
			seg = mlx5_get_send_wqe(qp, 0);
#endif

		for (i = 0; i < num_sge; i++) {
			if (likely(nvmeib_send_wr_common(*wr).sg_list[i].length)) {
				set_data_ptr_seg((struct mlx5_wqe_data_seg *)seg, nvmeib_send_wr_common(*wr).sg_list + i);
				size += sizeof(struct mlx5_wqe_data_seg) / 16;
				seg += sizeof(struct mlx5_wqe_data_seg);
#if MLX5_IB_WQ_FRAG_BUF_CTRL
				handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);
#else
				if (unlikely(seg == qend))
					seg = mlx5_get_send_wqe(qp, 0);
#endif
			}
		}

		ctrl->opmod_idx_opcode = cpu_to_be32(((u32)(qp->sq.cur_post) << 8) | ((u32)mlx5_opmod << 24) |
											 mlx5_opcode);

#if IB_MLX5_QP_TRANS
		ctrl->qpn_ds = cpu_to_be32(size | (qp->trans_qp.base.mqp.qpn << 8));
#else
		ctrl->qpn_ds = cpu_to_be32(size | (qp->mqp.qpn << 8));
#endif

#if IB_MLX5_QP_HAS_WQ_SIG
		if (unlikely(qp->wq_sig))
#else
		if (unlikely(qp->flags_en & MLX5_QP_FLAG_SIGNATURE))
#endif
			ctrl->signature = wq_sig(ctrl);

		mlx5_set_send_wr(qp, wr, idx, mlx5_opcode, nreq, size);

#if MLX5_IB_WQ_FRAG_BUF_CTRL
		/* We save the edge which was possibly updated during the WQE
		* construction, into SQ's cache.
		*/
		seg = PTR_ALIGN(seg, MLX5_SEND_WQE_BB);
		qp->sq.cur_edge = (unlikely(seg == cur_edge)) ?
				get_sq_edge(&qp->sq, qp->sq.cur_post & (qp->sq.wqe_cnt - 1)) :
				cur_edge;
#endif
	}

out:
	if (likely(nreq)) {
		qp->sq.head += nreq;

		/* Make sure that descriptors are written before
		 * updating doorbell record and ringing the doorbell
		 */
		wmb();

		qp->db.db[MLX5_SND_DBR] = cpu_to_be32(qp->sq.cur_post);

		/* Make sure doorbell record is visible to the HCA before
		 * we hit doorbell */
		wmb();
#if IB_MLX5_NEW_BF
		/* currently we support only regular doorbells */
		mlx5_write64((__be32 *)ctrl, bf->bfreg->map + bf->offset, NULL);
		/* Make sure doorbells don't leak out of SQ spinlock
		 * and reach the HCA out of order.
		 */
		mmiowb();
		bf->offset ^= bf->buf_size;
#else
		if (bf->need_lock)
			spin_lock(&bf->lock);

		mlx5_write64((__be32 *)ctrl, bf->regreg + bf->offset,
				 MLX5_GET_DOORBELL_LOCK(&bf->lock32));
		/* Make sure doorbells don't leak out of SQ spinlock
		 * and reach the HCA out of order.
		 */
		mmiowb();

		bf->offset ^= bf->buf_size;
		if (bf->need_lock)
			spin_unlock(&bf->lock);
#endif
	}

	spin_unlock_irqrestore(&qp->sq.lock, flags);

	return err;
}

static inline bool is_wr_atomic_op(struct nvmeib_send_wr *wr)
{
	switch (nvmeib_send_wr_common(*wr).opcode) {
	case IB_WR_ATOMIC_CMP_AND_SWP:
	case IB_WR_ATOMIC_FETCH_AND_ADD:
	case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
		return true;
	default:
		return false;
	}
}

/* Our implementation of mlx5 atomic operations does not support sending
   regular read and write transactions. so we send the regular operations
   with ib_post_send, where atomic operations are send with
   nvmeib_public_mlx5_post_send_atomic */
static int send_mlx5_in_partitions(struct ib_qp *ibqp, struct nvmeib_send_wr *send_wr,
	struct nvmeib_send_wr **bad_send_wr)
{
	struct nvmeib_send_wr *wr_itr, *next;
	IB_DECLARE_BAD_SEND_WR(bad_ib_wr_ptr);
	bool send_wr_atomic, next_wr_atomic;
	int rv = 0;

	NFIN;
	if (!send_wr) {
		rv = -EINVAL;
		goto out;
	}

	send_wr_atomic = is_wr_atomic_op(send_wr);

	nvmeib_send_wr_for_each(wr_itr, send_wr) {
		next = nvmeib_send_wr_next_ptr(*wr_itr);
		next_wr_atomic = next ? is_wr_atomic_op(next) : false;
		if (!next || send_wr_atomic != next_wr_atomic) {
			/* We've hit the end of the chain or the type has changed
			 * from atomic to non atomic or vice-versa.
			 * Break the chain and send the first part (or last part) */
			nvmeib_send_wr_clear_next(*wr_itr);
			if (send_wr_atomic) {
				/* Atomic only chain, use our atomic post send */
				rv = mlx5_post_send_atomic(ibqp, send_wr, bad_send_wr);
				if (rv)
					break;
			} else {
				/* Non-atomic only chain, use the IB post send */
				rv = ib_post_send(ibqp, nvmeib_send_wr_to_ib_ptr(*send_wr), &bad_ib_wr_ptr);
				if (rv) {
					/* Error - set the bad_send_wr */
					*bad_send_wr = nvmeib_send_wr_ptr_from_ib(bad_ib_wr_ptr);
					/* Restore the broken link */
					nvmeib_send_wr_set_next(*wr_itr, next);
					break;
				}
			}
			/* Restore the broken link */
			nvmeib_send_wr_set_next(*wr_itr, next);
			/* Start looping over the next part of the chain */
			send_wr = next;
			send_wr_atomic = next_wr_atomic;
		}
	}

out:
	NFOUT;
	return rv;
}

static int nvmeib_public_mlx5_post_send_atomic(struct ib_qp *ibqp,
	struct nvmeib_send_wr *wr,
	struct nvmeib_send_wr **bad_wr)
{
	IB_DECLARE_BAD_SEND_WR(bad_ib_wr_ptr);
	int rv;

	NFIN;
	if (likely(!nvmeib_send_wr_next_valid(*wr))) {
		/* Fast-path short-cut */
		if (is_wr_atomic_op(wr))
			rv = mlx5_post_send_atomic(ibqp, wr, bad_wr);
		else {
			rv = ib_post_send(ibqp, nvmeib_send_wr_to_ib_ptr(*wr), &bad_ib_wr_ptr);
			if (rv)
				*bad_wr = nvmeib_send_wr_ptr_from_ib(bad_ib_wr_ptr);
		}
	} else {
		/* Seperate WR chain into atomic and non-atomic */
		rv = send_mlx5_in_partitions(ibqp, wr, bad_wr);
	}

	NFOUT;
	return rv;
}

static int nvmeib_public_mlx5_peek_cq(struct ib_cq *ibcq, int max)
{
	return mlx5_peek_cq(ibcq, max);
}

#else
# warning MLX5 Driver not supported by this OFED/Kernel version

static int
nvmeib_public_mlx5_alloc_n_map(struct nvmeib_alloc_n_map *mem, struct pages **pages, unsigned int n_pages)
{
	printk(KERN_ERR "mlx5 devices not supported by this kernel");
	return -ENOSYS;
}

static int
nvmeib_public_mlx5_unmapn_n_free(struct nvmeib_alloc_n_map *mem)
{
	printk(KERN_ERR "mlx5 devices not supported by this kernel");
	return -ENOSYS;
}

static int
nvmeib_public_mlx5_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	printk(KERN_ERR "mlx5 devices not supported by this kernel");
	return -ENOSYS;
}

static int nvmeib_public_mlx5_query_device(struct ib_device *ibdev,
	struct ib_device_attr *props)
{
	printk(KERN_ERR "mlx5 devices not supported by this kernel");
	return -ENOSYS;
}

static int nvmeib_public_mlx5_post_send_atomic(struct ib_qp *ibqp,
	struct nvmeib_send_wr *wr,
	struct nvmeib_send_wr **bad_wr)
{
	printk(KERN_ERR "mlx5 devices not supported by this kernel");
	return -ENOSYS;
}

static int nvmeib_public_mlx5_peek_cq(struct ib_cq *ibcq, int max)
{
	printk(KERN_ERR "mlx5 devices not supported by this kernel");
	return -ENOSYS;
}

#endif

static struct nvmeib_device_public_ops mlx5 = {
	.module = THIS_MODULE,
	.alloc_n_map = nvmeib_public_mlx5_alloc_n_map,
	.unmapn_n_free = nvmeib_public_mlx5_unmapn_n_free,
	.map_mr = nvmeib_public_mlx5_map_mr,

	.query_device = nvmeib_public_mlx5_query_device,
	.post_send_atomic = nvmeib_public_mlx5_post_send_atomic,
	.set_debug_level = nvmeib_public_mlx5_set_debug_level,
	.peek_cq = nvmeib_public_mlx5_peek_cq,
	.create_rdda_qp = ib_create_qp,
	.destroy_rdda_qp = ib_destroy_qp,
};

static struct nvmeib_device_public_ops mlx5_odp = {
	.module = THIS_MODULE,
	.alloc_n_map = nvmeib_public_mlx5p_alloc_n_map,
	.unmapn_n_free = nvmeib_public_mlx5p_unmapn_n_free,
	.map_mr = nvmeib_public_mlx5p_map_mr,

	.query_device = nvmeib_public_mlx5_query_device,
	.post_send_atomic = nvmeib_public_mlx5_post_send_atomic,
	.set_debug_level = nvmeib_public_mlx5_set_debug_level,
	.peek_cq = nvmeib_public_mlx5_peek_cq,
	.create_rdda_qp = ib_create_qp,
	.destroy_rdda_qp = ib_destroy_qp,
};

#define DEV_MODNAME "mlx5_ib"

static int __init nvmeib_public_mlx5_init(void)
{
	return nvmeib_public_set_pops(DT_mlx5, DEV_MODNAME, &mlx5, &mlx5_odp);
}

static void __exit nvmeib_public_mlx5_exit(void)
{
	nvmeib_public_clear_pops(DT_mlx5);
}

module_init(nvmeib_public_mlx5_init);
module_exit(nvmeib_public_mlx5_exit);
