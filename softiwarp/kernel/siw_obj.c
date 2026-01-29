/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/vmalloc.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"


void siw_objhdr_init(struct siw_objhdr *hdr)
{
	kref_init(&hdr->ref);
}

void siw_idr_init(struct siw_dev *sdev)
{
	spin_lock_init(&sdev->idr_lock);

	idr_init(&sdev->qp_idr);
	idr_init(&sdev->cq_idr);
	idr_init(&sdev->pd_idr);
	idr_init(&sdev->mem_idr);
}

void siw_idr_release(struct siw_dev *sdev)
{
	idr_destroy(&sdev->qp_idr);
	idr_destroy(&sdev->cq_idr);
	idr_destroy(&sdev->pd_idr);
	idr_destroy(&sdev->mem_idr);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
static inline int siw_add_obj(spinlock_t *lock, struct idr *idr,
			      struct siw_objhdr *obj)
{
	u32		pre_id, id;
	unsigned long	flags;
	int		rv;

	get_random_bytes(&pre_id, sizeof pre_id);
	pre_id &= 0xffff;
again:
	do {
		if (!(idr_pre_get(idr, GFP_KERNEL)))
			return -ENOMEM;

		spin_lock_irqsave(lock, flags);
		rv = idr_get_new_above(idr, obj, pre_id, &id);
		spin_unlock_irqrestore(lock, flags);

	} while  (rv == -EAGAIN);

	if (rv == 0) {
		siw_objhdr_init(obj);
		obj->id = id;
		dprint(DBG_OBJ, "(OBJ%d): IDR New Object\n", id);
	} else if (rv == -ENOSPC && pre_id != 1) {
		pre_id = 1;
		goto again;
	} else {
		dprint(DBG_OBJ|DBG_ON, "(OBJ?): IDR New Object failed!\n");
	}
	return rv;
}
#else
static inline int siw_add_obj(spinlock_t *lock, struct idr *idr,
			      struct siw_objhdr *obj)
{
	unsigned long flags;
	int id, pre_id;

	do {
		get_random_bytes(&pre_id, sizeof pre_id);
		pre_id &= 0xffffff;
	} while (pre_id == 0);
again:
	spin_lock_irqsave(lock, flags);
	id = idr_alloc(idr, obj, pre_id, 0xffffff - 1, GFP_ATOMIC);
	spin_unlock_irqrestore(lock, flags);

	if (id > 0) {
		siw_objhdr_init(obj);
		obj->id = id;
		dprint(DBG_OBJ, "(OBJ%d): IDR New Object\n", id);
	} else if (id == -ENOSPC && pre_id != 1) {
		pre_id = 1;
		goto again;
	} else {
		BUG_ON(id == 0);
		dprint(DBG_OBJ|DBG_ON, "(OBJ?): IDR New Object failed!\n");
	}
	return id > 0 ? 0 : id;
}
#endif

static inline struct siw_objhdr *siw_get_obj(struct idr *idr, int id)
{
	struct siw_objhdr *obj;

	obj = idr_find(idr, id);
	if (obj)
		kref_get(&obj->ref);

	return obj;
}

struct siw_cq *siw_cq_id2obj(struct siw_dev *sdev, int id)
{
	struct siw_objhdr *obj = siw_get_obj(&sdev->cq_idr, id);
	if (obj)
		return container_of(obj, struct siw_cq, hdr);

	return NULL;
}

struct siw_qp *siw_qp_id2obj(struct siw_dev *sdev, int id)
{
	struct siw_objhdr *obj = siw_get_obj(&sdev->qp_idr, id);
	if (obj)
		return container_of(obj, struct siw_qp, hdr);

	return NULL;
}

/*
 * siw_mem_id2obj()
 *
 * resolves memory from stag given by id. might be called from:
 * o process context before sending out of sgl, or
 * o in softirq when resolving target memory
 */
struct siw_mem *siw_mem_id2obj(struct siw_dev *sdev, int id)
{
	struct siw_objhdr *obj;

	rcu_read_lock();
	obj = siw_get_obj(&sdev->mem_idr, id);
	rcu_read_unlock();

	if (obj) {
		dprint(DBG_MM|DBG_OBJ, "(MEM%d): New refcount: %d\n",
		       obj->id, kref_read(&obj->ref));

		return container_of(obj, struct siw_mem, hdr);
	}
	dprint(DBG_MM|DBG_OBJ|DBG_ON, "(MEM%d): not found!\n", id);

	return NULL;
}

struct siw_srq *siw_srq_id2obj(struct siw_dev *sdev, int id)
{
	struct siw_objhdr *obj = siw_get_obj(&sdev->srq_idr, id);
	if (obj)
		return container_of(obj, struct siw_srq, hdr);

	return NULL;
}

int siw_qp_add(struct siw_dev *sdev, struct siw_qp *qp)
{
	int rv = siw_add_obj(&sdev->idr_lock, &sdev->qp_idr, &qp->hdr);
	if (!rv) {
		dprint(DBG_OBJ, "(QP%d): New Object\n", QP_ID(qp));
		qp->hdr.sdev = sdev;
	}
	return rv;
}

int siw_cq_add(struct siw_dev *sdev, struct siw_cq *cq)
{
	int rv = siw_add_obj(&sdev->idr_lock, &sdev->cq_idr, &cq->hdr);
	if (!rv) {
		dprint(DBG_OBJ, "(CQ%d): New Object\n", cq->hdr.id);
		cq->hdr.sdev = sdev;
	}
	return rv;
}

int siw_pd_add(struct siw_dev *sdev, struct siw_pd *pd)
{
	int rv = siw_add_obj(&sdev->idr_lock, &sdev->pd_idr, &pd->hdr);
	if (!rv) {
		dprint(DBG_OBJ, "(PD%d): New Object\n", pd->hdr.id);
		pd->hdr.sdev = sdev;
	}
	return rv;
}

int siw_srq_add(struct siw_dev *sdev, struct siw_srq *srq)
{
	int rv = siw_add_obj(&sdev->idr_lock, &sdev->srq_idr, &srq->hdr);
	if (!rv) {
		dprint(DBG_OBJ, "(SRQ%d): New Object\n", srq->hdr.id);
		srq->hdr.sdev = sdev;
	}
	return rv;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
/*
 * Stag lookup is based on its index part only (24 bits)
 * It is assumed that the idr_get_new_above(,,1,) function will
 * always return a new id within this range (0x1...0xffffff),
 * if one is available.
 * The code avoids special Stag of zero and tries to randomize
 * STag values.
 */
int siw_mem_add(struct siw_dev *sdev, struct siw_mem *m)
{
	u32		id, pre_id;
	unsigned long	flags;
	int		rv;

	do {
		get_random_bytes(&pre_id, sizeof pre_id);
		pre_id &= 0xffff;
	} while (pre_id == 0);
again:
	do {
		if (!(idr_pre_get(&sdev->mem_idr, GFP_KERNEL)))
			return -ENOMEM;

		spin_lock_irqsave(&sdev->idr_lock, flags);
		rv = idr_get_new_above(&sdev->mem_idr, m, pre_id, &id);
		spin_unlock_irqrestore(&sdev->idr_lock, flags);

	} while (rv == -EAGAIN);

	if (rv == -ENOSPC || (rv == 0 && id > SIW_STAG_MAX)) {
		if (rv == 0) {
			spin_lock_irqsave(&sdev->idr_lock, flags);
			idr_remove(&sdev->mem_idr, id);
			spin_unlock_irqrestore(&sdev->idr_lock, flags);
		}
		if (pre_id == 1) {
			dprint(DBG_OBJ|DBG_MM|DBG_ON,
				"(IDR): New Object failed: %d\n", pre_id);
			return -ENOSPC;
		}
		pre_id = 1;
		goto again;
	} else if (rv) {
		dprint(DBG_OBJ|DBG_MM|DBG_ON,
			"(IDR%d): New Object failed: rv %d\n", id, rv);
		return rv;
	}
	siw_objhdr_init(&m->hdr);
	m->hdr.id = id;
	m->hdr.sdev = sdev;
	dprint(DBG_OBJ|DBG_MM, "(IDR%d): New Object\n", id);

	return 0;
}
#else
/*
 * Stag lookup is based on its index part only (24 bits).
 * The code avoids special Stag of zero and tries to randomize
 * STag values between 1 and SIW_STAG_MAX.
 */
int siw_mem_add(struct siw_dev *sdev, struct siw_mem *m)
{
	unsigned long flags;
	int id, pre_id;

	do {
		get_random_bytes(&pre_id, sizeof pre_id);
		pre_id &= 0xffffff;
	} while (pre_id == 0);
again:
	spin_lock_irqsave(&sdev->idr_lock, flags);
	id = idr_alloc(&sdev->mem_idr, m, pre_id, SIW_STAG_MAX, GFP_ATOMIC);
	spin_unlock_irqrestore(&sdev->idr_lock, flags);

	if (id == -ENOSPC || id > SIW_STAG_MAX) {
		if (pre_id == 1) {
			dprint(DBG_OBJ|DBG_MM|DBG_ON,
				"(IDR): New Object failed: %d\n", pre_id);
			return -ENOSPC;
		}
		pre_id = 1;
		goto again;
	} 
	siw_objhdr_init(&m->hdr);
	m->hdr.id = id;
	m->hdr.sdev = sdev;
	dprint(DBG_OBJ|DBG_MM, "(IDR%d): New Object\n", id);

	return 0;
}
#endif

void siw_remove_obj(spinlock_t *lock, struct idr *idr,
		      struct siw_objhdr *hdr)
{
	unsigned long	flags;

	dprint(DBG_OBJ, "(OBJ%d): IDR Remove Object\n", hdr->id);

	spin_lock_irqsave(lock, flags);
	idr_remove(idr, hdr->id);
	spin_unlock_irqrestore(lock, flags);
}


/********** routines to put objs back and free if no ref left *****/

static void siw_free_cq(struct kref *ref)
{
	struct siw_cq *cq =
		(container_of(container_of(ref, struct siw_objhdr, ref),
			      struct siw_cq, hdr));

	dprint(DBG_OBJ, "(CQ%d): Free Object\n", cq->hdr.id);

	atomic_dec(&cq->hdr.sdev->num_cq);
	if (cq->queue) {
		if (cq->kernel_verbs && SIW_NO_VMALLOC_FOR_KVERBS_CQ) {
			free_pages((unsigned long)cq->queue, 
				   get_order(cq->num_cqe * sizeof(struct siw_cqe) + sizeof(struct siw_cq_ctrl)));
			if (cq->cqe_md)
				free_pages((unsigned long)cq->cqe_md, 
					   get_order(cq->num_cqe * sizeof(struct siw_cqe_md)));
		} else {
			vfree(cq->queue);
			if (cq->cqe_md)
				vfree(cq->cqe_md);
		}
	}
#if KS_IB_CREATE_CQ_HAS_IB_DEVICE /*If we did not allocate it, we cant destroy it*/
	kfree(cq);
#else
	complete(&cq->free_comp);
#endif
}

static void siw_free_qp(struct kref *ref)
{
	struct siw_qp	*qp =
		container_of(container_of(ref, struct siw_objhdr, ref),
			     struct siw_qp, hdr);
	struct siw_dev	*sdev = qp->hdr.sdev;
	unsigned long flags;

	dprint(DBG_OBJ|DBG_CM, "(QP%d): Free Object\n", QP_ID(qp));
	if (qp->cep)
		siw_cep_put(qp->cep);

	siw_remove_obj(&sdev->idr_lock, &sdev->qp_idr, &qp->hdr);

	spin_lock_irqsave(&sdev->idr_lock, flags);
	list_del(&qp->devq);
	spin_unlock_irqrestore(&sdev->idr_lock, flags);

	if (qp->sendq) {
		if (qp->kernel_verbs && SIW_NO_VMALLOC_FOR_KVERBS_QP)
			free_pages((unsigned long)qp->sendq, get_order(qp->attrs.sq_size * sizeof(struct siw_sqe)));
		else
			vfree(qp->sendq);
	}
	if (qp->recvq) {
		if (qp->kernel_verbs && SIW_NO_VMALLOC_FOR_KVERBS_QP)
			free_pages((unsigned long)qp->recvq, get_order(qp->attrs.rq_size * sizeof(struct siw_rqe)));
		else
			vfree(qp->recvq);
	}
	if (qp->irq) {
		if (qp->kernel_verbs && SIW_NO_VMALLOC_FOR_KVERBS_QP)
			free_pages((unsigned long)qp->irq, get_order(qp->attrs.irq_size * sizeof(struct siw_sqe)));
		else
			vfree(qp->irq);
	}
	if (qp->orq) {
		if (qp->kernel_verbs && SIW_NO_VMALLOC_FOR_KVERBS_QP)
			free_pages((unsigned long)qp->orq, get_order(qp->attrs.orq_size * sizeof(struct siw_sqe)));
		else
			vfree(qp->orq);
	}

	atomic_dec(&sdev->num_qp);

#if KS_IB_DEVICE_OPS_HAS_QP_SIZE
	/* on newer kernels we will wakeup ib core to free us */
	complete(&qp->free_comp);
#else
	/* on older kernels we responsible to free */
	kfree(qp);
#endif
}

static void siw_free_pd(struct kref *ref)
{
	struct siw_pd	*pd =
		container_of(container_of(ref, struct siw_objhdr, ref),
			     struct siw_pd, hdr);
	int n;

	dprint(DBG_OBJ|DBG_OL, "(PD%d): Free Object\n", pd->hdr.id);

	n = atomic_dec_return(&pd->hdr.sdev->num_pd);
	dprint(DBG_OBJ|DBG_OL, "dec num_pd to %d\n", n);

#if KS_IB_DEVICE_PD_USES_UCONTEXT
	kfree(pd);
#endif
}

static void siw_free_mem(struct kref *ref)
{
	struct siw_mem *m;

	m = container_of(container_of(ref, struct siw_objhdr, ref),
			 struct siw_mem, hdr);

	dprint(DBG_MM|DBG_OBJ, "(MEM%d): Free\n", OBJ_ID(m));

	atomic_dec(&m->hdr.sdev->num_mem);

	if (SIW_MEM_IS_MW(m)) {
		struct siw_mw *mw = container_of(m, struct siw_mw, mem);
		kfree_rcu(mw, rcu);
	} else {
		struct siw_mr *mr = container_of(m, struct siw_mr, mem);
		dprint(DBG_MM|DBG_OBJ, "(MEM%d): Release obj " dprint_ptr_str() ", (PBL %d)\n",
			OBJ_ID(m), mr->mem_obj, mr->mem.is_pbl ? 1 : 0);
		if (mr->mem_obj) {
			if (mr->mem.is_pbl == 0)
				siw_umem_release(mr->umem);
			else
				siw_pbl_free(mr->pbl);
		}
		kfree_rcu(mr, rcu);
	}
}

static void siw_free_srq(struct kref *ref) {
	struct siw_srq *srq = container_of(container_of(ref, struct siw_objhdr, ref),
					   struct siw_srq, hdr);
	struct siw_dev	*sdev = srq->hdr.sdev;
	dprint(DBG_OBJ|DBG_OL, "(SRQ%d): Free Object\n", srq->hdr.id);

	siw_remove_obj(&sdev->idr_lock, &sdev->srq_idr, &srq->hdr);

	complete(&srq->free_comp);
}

void siw_cq_put(struct siw_cq *cq)
{
	dprint(DBG_OBJ, "(CQ%d): Old refcount: %d\n",
		OBJ_ID(cq), kref_read(&cq->hdr.ref));
	kref_put(&cq->hdr.ref, siw_free_cq);
}

void siw_qp_put(struct siw_qp *qp)
{
	dprint(DBG_OBJ, "(QP%d): Old refcount: %d\n",
		QP_ID(qp), kref_read(&qp->hdr.ref));
	kref_put(&qp->hdr.ref, siw_free_qp);
}

void siw_pd_put(struct siw_pd *pd)
{
	dprint(DBG_OBJ, "(PD%d): Old refcount: %d\n",
		OBJ_ID(pd), kref_read(&pd->hdr.ref));
	kref_put(&pd->hdr.ref, siw_free_pd);
}

void siw_mem_put(struct siw_mem *m)
{
	dprint(DBG_MM|DBG_OBJ, "(MEM%d): Old refcount: %d\n",
		OBJ_ID(m), kref_read(&m->hdr.ref));
	kref_put(&m->hdr.ref, siw_free_mem);
}

void siw_srq_put(struct siw_srq *srq)
{
	dprint(DBG_MM|DBG_OBJ, "(SRQ%d): Old refcount: %d\n",
	       OBJ_ID(srq), kref_read(&srq->hdr.ref));
	kref_put(&srq->hdr.ref, siw_free_srq);
}


/***** routines for WQE handling ***/

static inline void siw_unref_mem_sgl(union siw_mem_resolved *mem, int num_sge)
{
	while (num_sge--) {
		if (mem->obj != NULL) {
			siw_mem_put(mem->obj);
			mem->obj = NULL;
			mem++;
		} else
			break;
	}
}


//omril: is siw_mem_id2obj the corresponding get?
void siw_wqe_put_mem(struct siw_wqe *wqe, enum siw_opcode op)
{
	switch (op) {

	case SIW_OP_SEND:
	case SIW_OP_WRITE:
	case SIW_OP_SEND_WITH_IMM:
	case SIW_OP_SEND_REMOTE_INV:
	case SIW_OP_READ:
	case SIW_OP_READ_LOCAL_INV:
	case SIW_OP_COMP_AND_SWAP:
	case SIW_OP_MASKED_COMP_AND_SWAP:
		if (!(wqe->sqe.flags & SIW_WQE_INLINE))
			siw_unref_mem_sgl(wqe->mem, wqe->sqe.num_sge);
		break;

	case SIW_OP_RECEIVE:
		siw_unref_mem_sgl(wqe->mem, wqe->rqe.num_sge);
		break;

	case SIW_OP_READ_RESPONSE:
	case SIW_OP_COMP_AND_SWAP_RESPONSE:
		if (!(wqe->sqe.flags & SIW_WQE_INLINE))
			siw_unref_mem_sgl(wqe->mem, 1); //omril: why 1 ? see siw_qp_sq_proc_tx()
		break;

	default:
		/*
		 *  SIW_OP_INVAL_STAG and SIW_OP_REG_MR, SIW_OP_WRITE_RESPONSE 
		 * do not hold memory references
		 */
		break;
	}
}

int siw_invalidate_stag(struct siw_pd *pd, u32 stag)
{
	u32 stag_idx = stag >> 8;
	struct siw_mem *mem = siw_mem_id2obj(pd->hdr.sdev, stag_idx);
	int rv = 0;

	dprint(DBG_MM, ": STag %u Enter\n", stag_idx);

	if (unlikely(!mem)) {
		dprint(DBG_MM, ": STag %u unknown\n", stag_idx);
		return -EINVAL;
	}
	if (unlikely(siw_mem2mr(mem)->pd != pd)) {
		dprint(DBG_MM, ": PD mismatch for STag %u\n", stag_idx);
		rv = -EINVAL;
		goto out;
	}
	/*
	 * Per RDMA verbs definition, an STag may already be in invalid
	 * state if invalidation is requested. So no state check here.
	 */
	mem->stag_valid = 0;

	dprint(DBG_MM, ": STag now invalid: %u\n", stag_idx);
out:
	siw_mem_put(mem);
	return rv;
}
