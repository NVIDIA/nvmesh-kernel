

#include "nvmeib_ib_driver.h"
#include "kernel/siw.h"
#include "nvmeib_siw.h"

const unsigned long nvmeib_siw_dev_caps =
	NVMEIB_DEVCAP_SRQ |
	NVMEIB_DEVCAP_SRQ_LAST_WQE  |
	NVMEIB_DEVCAP_ATOMICS_REQ |
	NVMEIB_DEVCAP_ATOMICS_RESP |
	NVMEIB_DEVCAP_MASKED_ATOMICS_REQ |
	NVMEIB_DEVCAP_MASKED_ATOMICS_RESP |
	NVMEIB_DEVCAP_RD_ATOM_64 |
	NVMEIB_DEVCAP_RD_ATOM_32 |
	NVMEIB_DEVCAP_RD_ATOM_16 |
	NVMEIB_DEVCAP_RD_ATOM_8 | 
	NVMEIB_DEVCAP_PCIE_ATOMICS;
	
static ssize_t nvmeib_siw_get_qp_usage(struct ib_device *ib_dev, struct ib_qp *ib_qp,
				enum nvmeib_cnt_mem_type mem_type)
{
	ssize_t ret = 0;
	struct siw_qp *qp = container_of(ib_qp, struct siw_qp, ofa_qp);
	int num_sqe = roundup_pow_of_two(qp->attrs.sq_size);
	int num_rqe = roundup_pow_of_two(qp->attrs.rq_size);

	switch (mem_type) {
	case NVMEIB_CNT_MEM_KMEM:
	case NVMEIB_CNT_MEM_IB_KMEM:
		ret += sizeof(*qp);
		break;
	case NVMEIB_CNT_MEM_DMA:
	case NVMEIB_CNT_MEM_IB_DMA:
		break;
	case NVMEIB_CNT_MEM_PAGES:
	case NVMEIB_CNT_MEM_IB_PAGES:
#if SIW_NO_VMALLOC_FOR_KVERBS_QP
		ret += PAGE_SIZE << get_order(num_sqe * sizeof(struct siw_sqe));
		if (qp->recvq)
			ret += PAGE_SIZE << get_order(num_rqe * sizeof(struct siw_rqe));
		if (qp->irq)
			ret += PAGE_SIZE << get_order(qp->attrs.irq_size * sizeof(struct siw_sqe));
		if (qp->orq)
			ret += PAGE_SIZE << get_order(qp->attrs.orq_size * sizeof(struct siw_sqe));
#endif
		break;
	case NVMEIB_CNT_MEM_VIRT:
	case NVMEIB_CNT_MEM_IB_VIRT:
#if !SIW_NO_VMALLOC_FOR_KVERBS_QP
		ret += num_sqe * sizeof(struct siw_sqe);
		if (qp->recvq)
			ret += num_rqe * sizeof(struct siw_rqe);
		if (qp->irq)
			ret += qp->attrs.irq_size * sizeof(struct siw_sqe);
		if (qp->orq)
			ret += qp->attrs.orq_size * sizeof(struct siw_sqe);
#endif
		break;
	default:
		BUG_ON(1);
	}
	return ret;
}

static ssize_t nvmeib_siw_get_srq_usage(struct ib_device *ib_dev, struct ib_srq *ib_srq,
				enum nvmeib_cnt_mem_type mem_type)
{
	ssize_t ret = 0;
	struct siw_srq *srq = container_of(ib_srq, struct siw_srq, ofa_srq);

	switch (mem_type) {
	case NVMEIB_CNT_MEM_KMEM:
	case NVMEIB_CNT_MEM_IB_KMEM:
		ret += sizeof(*srq);
		break;
	case NVMEIB_CNT_MEM_DMA:
	case NVMEIB_CNT_MEM_IB_DMA:
		break;
	case NVMEIB_CNT_MEM_PAGES:
	case NVMEIB_CNT_MEM_IB_PAGES:
#if SIW_NO_VMALLOC_FOR_KVERBS_SRQ
		if (srq->recvq)
			ret += PAGE_SIZE << get_order(srq->num_rqe * sizeof(struct siw_rqe));
#endif
		break;
	case NVMEIB_CNT_MEM_VIRT:
	case NVMEIB_CNT_MEM_IB_VIRT:
#if !SIW_NO_VMALLOC_FOR_KVERBS_SRQ
		if (srq->recvq)
			ret += srq->num_rqe * sizeof(struct siw_rqe);
#endif
		break;
	default:
		BUG_ON(1);
	}
	return ret;
}

static ssize_t nvmeib_siw_get_cq_usage(struct ib_device *ib_dev, struct ib_cq *ib_cq,
				enum nvmeib_cnt_mem_type mem_type)
{
	ssize_t ret = 0;
	struct siw_cq *cq = container_of(ib_cq, struct siw_cq, ofa_cq);

	switch (mem_type) {
	case NVMEIB_CNT_MEM_KMEM:
	case NVMEIB_CNT_MEM_IB_KMEM:
		ret += sizeof(*cq);
		break;
	case NVMEIB_CNT_MEM_DMA:
	case NVMEIB_CNT_MEM_IB_DMA:
		break;
	case NVMEIB_CNT_MEM_PAGES:
	case NVMEIB_CNT_MEM_IB_PAGES:
#if SIW_NO_VMALLOC_FOR_KVERBS_CQ
		if (cq->queue)
			ret += PAGE_SIZE << get_order(cq->num_cqe * sizeof(struct siw_cqe));
#endif
		break;
	case NVMEIB_CNT_MEM_VIRT:
	case NVMEIB_CNT_MEM_IB_VIRT:
#if SIW_NO_VMALLOC_FOR_KVERBS_CQ
		if (cq->queue)
			ret += cq->num_cqe * sizeof(struct siw_cqe);
#endif		
		break;
	default:
		BUG_ON(1);
	}
	return ret;
}

static ssize_t nvmeib_siw_get_mr_usage(struct ib_device *ib_dev, struct ib_mr *ib_mr,
				enum nvmeib_cnt_mem_type mem_type)
{
	ssize_t ret = 0;
	struct siw_mr *mr = container_of(ib_mr, struct siw_mr, ofa_mr);

	switch (mem_type) {
	case NVMEIB_CNT_MEM_KMEM:
	case NVMEIB_CNT_MEM_IB_KMEM:
		ret += sizeof(*mr);
		if (mr->mem.is_pbl) {
			ret += sizeof(*mr->pbl);
			ret += mr->pbl->max_buf * sizeof(mr->pbl->pbe[0]);
		}
		break;
	case NVMEIB_CNT_MEM_DMA:
	case NVMEIB_CNT_MEM_IB_DMA:
		break;
	case NVMEIB_CNT_MEM_PAGES:
	case NVMEIB_CNT_MEM_IB_PAGES:
	case NVMEIB_CNT_MEM_VIRT:
	case NVMEIB_CNT_MEM_IB_VIRT:
		break;
	default:
		BUG_ON(1);
	}
	return ret;
}

static struct nvmeib_device_ops siw = {
	.module = THIS_MODULE,
	.get_qp_usage = nvmeib_siw_get_qp_usage,
	.get_srq_usage = nvmeib_siw_get_srq_usage,
	.get_cq_usage = nvmeib_siw_get_cq_usage,
	.get_mr_usage = nvmeib_siw_get_mr_usage,
};

int nvmeib_siw_init(void)
{
	return nvmeib_ibdr_hwdev_register(DT_siw, "siw", &siw,
									  nvmeib_siw_dev_caps, 0);
}

void nvmeib_siw_cleanup(void)
{
	nvmeib_ibdr_hwdev_unregister(DT_siw);
}
