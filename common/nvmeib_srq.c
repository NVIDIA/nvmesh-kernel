#include "nvmeib_srq.h"

#include "ib_incs.h"
#include "nvmeib_utils.h"
#include "nvmeibm_trace.h"
#include "nvmeib_memmgr_metrics.h"

#if ENABLE_SIW
#include "../softiwarp/kernel/siw.h"
#endif

enum nvmeib_srq_info_type {
	NVMEIB_SRQ_INFO_PRIVATE		= 0,
	NVMEIB_SRQ_INFO_PRIMARY		= 1,
	NVMEIB_SRQ_INFO_SECONDARY	= 2
};

struct nvmeib_srq_info {
	struct nvmeib_dev *dev;
	/* shared receive queue */
	int q_size;
	atomic_t q_size_now;
	bool do_print_current_q_size;
	int msg_size;
	unsigned srq_limit;
	struct ib_srq *srq;
	/* receive queue buffers */
	struct nvmeib_iu **rx_ring;

	void *ctx;
	enum nvmeib_srq_info_type type;
	int idx;
	int use_cnt;
	u8 buf_pattern;
	struct list_head link;

	/* event counters */
	unsigned n_srq_limit_reached;
	unsigned n_srq_err;
	
	/* metrics */
	void *memmgr_metrics_ctx;
};
bool nvmeib_srq_is_pcpu_cq(struct nvmeib_srq_info *srq_info)
{
	return !!srq_info->dev->cqs;
}

#define SRQ_USE_CNT_BASE 1

struct nvmeib_srqs {
	/* primary SRQ params */
	struct nvmeib_srq_params p;
	/* secondary SRQs params */
	struct nvmeib_srq_params s;
	/* primary SRQ */
	struct nvmeib_srq_info *prim;
	/* secondary SRQs */
	int num_sec;
	spinlock_t guard;
	struct list_head sec;
};

#ifdef SRQ_TRACE
static inline void nvmeib_srq_info_inc(struct nvmeib_srq_info *srq_info, int n)
{
	int v = atomic_add_return(n, &srq_info->q_size_now);
	if (srq_info->do_print_current_q_size) {
		_NI(nvmeib_srq_info_inc_i1, "srq_info @PTR q_size_now=@INT", srq_info, v);
	}
	BUG_ON(v > srq_info->q_size);
}

static inline void nvmeib_srq_info_dec(struct nvmeib_srq_info *srq_info, int n)
{
	int v = atomic_sub_return(n, &srq_info->q_size_now);
	if (srq_info->do_print_current_q_size) {
		_NI(nvmeib_srq_info_dec_i1, "srq_info @PTR q_size_now=@INT", srq_info, v);
	}
	BUG_ON(v < 0);
}

static inline void nvmeib_srq_info_check_full(struct nvmeib_srq_info *srq_info)
{
	int q_size_now;
	if (srq_info->q_size != (q_size_now = atomic_read(&srq_info->q_size_now))) {
		_NE(err_nvmeib_srq_info_check_full_not_full, "srq_info @PTR q_size_now=@INT != q_size=@INT",
				srq_info, q_size_now, srq_info->q_size);
		WARN_ON(1);
	}
}

#else

#define nvmeib_srq_info_inc(s, n)
#define nvmeib_srq_info_dec(s, n)
#define nvmeib_srq_info_check_full(s)

#endif

struct nvmeib_iu *nvmeib_srq_rtrv_recv(
	struct nvmeib_srq_info *srq_info, int index, void *owner)
{
	//BUG_ON(index >= srq_info->q_size);
	if (index >= srq_info->q_size) {
		_NE(nvmeib_srq_rtrv_recv_e1, "OOPS, srq_info=@PTR, index (@INT) > q_size (@INT)",
		   srq_info, index, srq_info->q_size);
		return NULL;
	}

	nvmeib_srq_info_dec(srq_info, 1);
	BUG_ON(srq_info->rx_ring[index]->owner_ptr != NULL);
	srq_info->rx_ring[index]->owner_ptr = owner;
	return srq_info->rx_ring[index];
}
EXPORT_SYMBOL(nvmeib_srq_rtrv_recv);

struct nvmeib_iu *nvmeib_srq_find_next_owner(struct nvmeib_srq_info *srq_info,
		void *owner, struct nvmeib_iu *curr)
{
	int i = curr ? curr->index : 0;
	struct nvmeib_iu *ret = NULL;
	for (; i < srq_info->q_size; i++) {
		if (srq_info->rx_ring[i]->owner_ptr == owner) {
			ret = srq_info->rx_ring[i];
			break;
		}
	}
	return ret;
}
EXPORT_SYMBOL(nvmeib_srq_find_next_owner);

int nvmeib_srq_post_recv(struct nvmeib_srq_info *srq_info, struct nvmeib_iu *iu)
{
	struct ib_recv_wr wr;
	IB_DECLARE_BAD_RECV_WR(bad_wr);
	struct ib_sge list;
	int rv;

	if (!iu) {
		_NE(nvmeib_srq_post_recv_e1, "OOPS, srq_info=@PTR, iu is NULL", srq_info);
		return -1;
	}

	if (!list_empty(&iu->free_tx_n)) {
		_NE(nvmeib_srq_post_recv_e2, "posting iu=@PTR to srq_info=@PTR while sw's link still used, leak",
		   iu, srq_info);
		BUG_NON_PRODUCTION(5711);
		return -1;
	}

	BUG_ON(iu->index >= srq_info->q_size);
	BUG_ON(iu->priv != srq_info);
	iu->owner_ptr = NULL;

	//NFIN;
	list.addr = iu->dma;
	list.length = iu->size;
	list.lkey = nvmeib_get_lkey(srq_info->dev);

	wr.next = NULL;
	wr.wr_id = nordda_wr_id_encode(++iu->version, NVMEIB_RECV, iu->index);
	wr.sg_list = &list;
	wr.num_sge = 1;
	//NFOUT;

	//if (srq_info->buf_pattern)
	//	memset(iu->buf, srq_info->buf_pattern, iu->size);

	nvmeib_iu_owner_sw2hw(iu, srq_info, err_srq_post_recv_sw2hw);
	nvmeib_srq_info_inc(srq_info, 1);
	rv = ib_post_srq_recv(srq_info->srq, &wr, &bad_wr);
	if (rv) {
		nvmeib_srq_info_dec(srq_info, 1);
		nvmeib_iu_owner_hw2sw(iu, srq_info, err_srq_post_recv_hw2sw);
		nvmeib_srq_inc(srq_info, 1); //shouldn't this be in rv==0 case?
	}
	return rv;
}
EXPORT_SYMBOL(nvmeib_srq_post_recv);

static int fill_recv_q(struct nvmeib_srq_info *srq_info)
{
	int rv = 0, i;

	NFIN;
	for (i = 0; i < srq_info->q_size; i++) {
		struct nvmeib_iu *iu = srq_info->rx_ring[i];
		atomic_set(&iu->owner, NVMEIB_IU_OWNER_SW);
		 if ((rv = nvmeib_srq_post_recv(srq_info, iu)) < 0) {
			 _NE(error_nvmeib_srq_fill_recv_q, "ib_post_srq_recv() failed: @RV", rv);
			 goto out;
		 }
	}

out:
	NFOUT;
	return rv;
}

static void srq_info_event_handler(struct ib_event *event, void *ctx)
{
	struct nvmeib_srq_info *srq_info = ctx;

	if (event->event == IB_EVENT_SRQ_LIMIT_REACHED) {
		struct ib_srq_attr srq_attr = {
			.srq_limit = srq_info->srq_limit,
		};
		int rv;
		srq_info->n_srq_limit_reached++;
		_NW(trace_nvmeib_srq_srq_info_event_handler_limit, "SRQ @IDX.@SRQ_INFO, @IB_DEV_NAME event @EVENT limit @MIN count @COUNT", srq_info->idx, srq_info,
			srq_info->dev->ib_dev->name, event->event, srq_info->srq_limit, srq_info->n_srq_limit_reached);
#if ENABLE_SIW && SIW_SRQ_RQE_TRACK
		do {
			struct siw_srq *siw_srq = container_of(event->element.srq, struct siw_srq, ofa_srq);
			_NW(trace_nvmeib_srq_srq_info_event_handler_limit_srq, "SIW SRQ @IDX.@SRQ_INFO, @IB_DEV_NAME - n_q/max_q: @COUNT / @MAX, n_rsvd: @COUNT, n_recv: @COUNT, n_cqe: @COUNT, n_ulp: @COUNT", 
				srq_info->idx, srq_info, srq_info->dev->ib_dev->name,
				siw_srq->rqe_track.n_q, siw_srq->rqe_track.max_q,
				siw_srq->rqe_track.n_rsvd, siw_srq->rqe_track.n_recv, 
				siw_srq->rqe_track.n_cq, siw_srq->rqe_track.n_ulp);
		} while(0);
#endif
		if ((rv = ib_modify_srq(srq_info->srq, &srq_attr, IB_SRQ_LIMIT)) < 0)
			_NE(err_nvmeib_srq_srq_info_event_handler_err_mod_limit,
				"Failed (@RV) to modify SRQ @IDX.@SRQ_INFO, @IB_DEV_NAME",
				rv, srq_info->idx, srq_info, srq_info->dev->ib_dev->name);
	}
	else if (event->event == IB_EVENT_SRQ_ERR) {
		srq_info->n_srq_err++;
		_NE(error_nvmeib_srq_srq_info_event_handler_err, "SRQ @IDX.@SRQ_INFO, @IB_DEV_NAME event @EVENT count @COUNT", srq_info->idx, srq_info,
			srq_info->dev->ib_dev->name, event->event, srq_info->n_srq_err);
	} else {
		_NT(trace_nvmeib_srq_srq_info_event_handler, "SRQ @IDX.@SRQ_INFO, @IB_DEV_NAME event @EVENT", srq_info->idx, srq_info,
			srq_info->dev->ib_dev->name, event->event);
	}
}

static size_t calc_srq_alloc_size(struct nvmeib_srq_info *srq_info)
{
	int order;
	int dma_size = srq_info->msg_size;
	int ring_size = srq_info->q_size;
	size_t size = PAGE_ALIGN(dma_size);
	order = get_order(size);
	/* TBD: Add mlx5 memory allocated for SRQ */
	return sizeof(*srq_info) + ring_size * (1 << order) * PAGE_SIZE;
}

static int srq_info_free(struct nvmeib_srq_info *srq_info)
{
	int rv = -1;
	NFIN;

	if (srq_info->use_cnt > SRQ_USE_CNT_BASE) {
		_NW(warn_nvmeib_srq_srq_info_free, "Fail to free srq_info @IDX.@SRQ_INFO, use-cnt @USE_CNT (dev @IB_DEV_NAME)",
			srq_info->idx, srq_info, srq_info->use_cnt,
			srq_info->dev->ib_dev->name);
		WARN_ON_ONCE(1);
		goto out;
	}

	nvmeib_srq_info_check_full(srq_info);

	if (srq_info->rx_ring) {
		/* JH IOMMU: Changed to DMA_FROM_DEVICE. Sink for Remote RDMA_SEND */
		nvmeib_free_ioctx_ring(srq_info->rx_ring, srq_info->dev->ib_dev,
			srq_info->q_size, srq_info->msg_size, DMA_FROM_DEVICE, NULL);
		srq_info->rx_ring = NULL;
		if (srq_info->memmgr_metrics_ctx) {
			nvmesh_memmgr_metric_on_free_update(srq_info->memmgr_metrics_ctx, calc_srq_alloc_size(srq_info));
		}
	}
	if (srq_info->srq) {
		#ifdef KS_IB_DESTROY_SRQ_RETURN_VOID
			ib_destroy_srq(srq_info->srq);
			rv = 0;
		#else
			rv = ib_destroy_srq(srq_info->srq);
		#endif
		if (rv)
			_NE(err_nvmeib_srq_srq_info_free,
				"Failed destroy SRQ=@PTR (@RV), leak mem.\n", srq_info->srq, rv); /* Used/ref'ed by QPs? */
		srq_info->srq = NULL;
	}
	kfree(srq_info);
	rv = 0;

out:
	NFOUT;
	return rv;
}

int nvmeib_srq_info_free(struct nvmeib_srq_info *srq_info)
{
	int rv = -1;
	NFIN;

	if (srq_info->type == NVMEIB_SRQ_INFO_PRIVATE)
		rv = srq_info_free(srq_info);
	else
		_NE(error_nvmeib_srq_nvmeib_srq_info_free, "Invalid srq-info type @SRQ_INFO_TYPE for operation", srq_info->type);

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_srq_info_free);

struct nvmeib_srq_info *nvmeib_srq_info_create(struct nvmeib_dev *dev,
	struct nvmeib_srq_params *params, void *ctx, void *memmgr_metrics_ctx)
{
	struct nvmeib_srq_info *srq_info = NULL;
	struct ib_srq_init_attr srq_attr = {0};
	NFIN;

	/* allocate srq_info */
	if (!(srq_info = kzalloc(sizeof(*srq_info), GFP_KERNEL))) {
		_NE(error_nvmeib_srq_nvmeib_srq_info_create, "Failed to allocate srq-info");
		if (memmgr_metrics_ctx) {
			nvmesh_memmgr_metric_on_alloc_update(memmgr_metrics_ctx, sizeof(*srq_info), false /* success */);
		}
		goto out;
	}

	srq_info->dev = dev;
	srq_info->q_size = params->q_size;
	srq_info->msg_size = params->msg_size;
	srq_info->buf_pattern = params->buf_pattern;
	srq_info->type = NVMEIB_SRQ_INFO_PRIVATE;
	srq_info->idx = 0;
	srq_info->use_cnt = SRQ_USE_CNT_BASE;
	srq_info->ctx = ctx;
	srq_info->srq_limit = params->srq_limit;
	srq_info->memmgr_metrics_ctx = memmgr_metrics_ctx;
	atomic_set(&srq_info->q_size_now, 0);

	/* init srq-attr */
	srq_attr.event_handler = srq_info_event_handler;
	srq_attr.srq_context = srq_info;
	srq_attr.attr.max_wr = params->q_size;
	srq_attr.attr.max_sge = 1;
	srq_attr.attr.srq_limit = params->srq_limit;
#if KS_IB_SRQ_TYPE
	srq_attr.srq_type = IB_SRQT_BASIC;
#endif

	/* create the srq */
	srq_info->srq = ib_create_srq(dev->pd, &srq_attr);
	if (IS_ERR_OR_NULL(srq_info->srq)) {
		_NE(error_1_nvmeib_srq_nvmeib_srq_info_create, "Fail to create shared receive queue");
		srq_info->srq = NULL;
		goto err;
	}

	/* allocate receive buffers */
	/* JH IOMMU: Changed to DMA_FROM_DEVICE. (Sink for Remote RDMA_SEND) */
	srq_info->rx_ring = nvmeib_alloc_ioctx_ring(dev->ib_dev, srq_info->q_size,
		sizeof(*srq_info->rx_ring[0]), srq_info->msg_size, DMA_FROM_DEVICE, NULL,
		srq_info);
	if (!srq_info->rx_ring) {
		_NE(error_2_nvmeib_srq_nvmeib_srq_info_create, "OOM: cannot allocate rx_ring.");
		if (memmgr_metrics_ctx) {
			nvmesh_memmgr_metric_on_alloc_update(memmgr_metrics_ctx, calc_srq_alloc_size(srq_info), false /* success */);
		}
		goto err;
	}
	if (memmgr_metrics_ctx) {
		nvmesh_memmgr_metric_on_alloc_update(memmgr_metrics_ctx, calc_srq_alloc_size(srq_info), true /* success */);
	}

	/* populate srq */
	if ((fill_recv_q(srq_info)))
		goto err;
	srq_info->do_print_current_q_size = false;
	goto out;

err:
	srq_info_free(srq_info);
	srq_info = NULL;

out:
	NFOUT;
	return srq_info;
}
EXPORT_SYMBOL(nvmeib_srq_info_create);

#ifdef TRACE_SECONDARY_SRQS
void print_sec(struct nvmeib_srqs *s)
{
	struct nvmeib_srq_info *si;
	int ii = 0;

	list_for_each_entry(si, &s->sec, link) {
		if (ii == s->num_sec) {
			_NE(print_sec_e1, "List corruption detected (ii=@INT)!!!!", ii);
			break;
		}
		_NI(print_sec_i1, "[@INT32_02] idx=@INT32_02, use-cnt=@INT32_02", ii, si->idx, si->use_cnt);
		ii++;
	}
}
#else
#define print_sec(s)
#endif

int nvmeib_srq_pool_create(struct nvmeib_dev *dev,
	int pool_size,
	struct nvmeib_srq_params *prim_q_params,
	struct nvmeib_srq_params *sec_qs_params,
	void *memmgr_metrics_ctx)
{
	struct nvmeib_srqs *s = NULL;
	struct nvmeib_srq_info *srq_info;
	int num_sec;
	int ii;
	int rv = -1;
	NFIN;

	/* checks */
	if (pool_size < 1) {
		_NE(error_nvmeib_srq_nvmeib_srq_pool_create, "Invalid pool size @POOL_SIZE", pool_size);
		goto out;
	}
	if (!prim_q_params) {
		_NE(error_1_nvmeib_srq_nvmeib_srq_pool_create, "Invalid input");
		goto out;
	}
	num_sec = pool_size  -1;
	if ((!!sec_qs_params ^ (num_sec > 0))) {
		_NE(error_2_nvmeib_srq_nvmeib_srq_pool_create, "Invalid secondary SRQs input (@NUM_SEC, @SEC_QS_PARAMS)", num_sec, sec_qs_params);
		goto out;
	}

	/* allocate and init nvmeib_srqs */
	if (!(s = kzalloc(sizeof(*s), GFP_KERNEL))) {
		_NE(error_3_nvmeib_srq_nvmeib_srq_pool_create, "Failed to allocate srqs");
		rv = -ENOMEM;
		goto out;
	}
	dev->srqs = s;
	s->num_sec = num_sec;
	INIT_LIST_HEAD(&s->sec);
	spin_lock_init(&s->guard);

	/* primary SRQ */
	if (!(s->prim = nvmeib_srq_info_create(dev, prim_q_params, NULL, memmgr_metrics_ctx))) {
		_NE(error_4_nvmeib_srq_nvmeib_srq_pool_create, "Fail to create primary srq");
		goto err;
	}
	s->prim->idx = -1;
	s->prim->type = NVMEIB_SRQ_INFO_PRIMARY;

	/* secondary SRQs */
	if (sec_qs_params) {
		for (ii = 0; ii < s->num_sec; ii++) {
			if (!(srq_info = nvmeib_srq_info_create(dev, sec_qs_params, NULL, memmgr_metrics_ctx))) {
				_NE(error_5_nvmeib_srq_nvmeib_srq_pool_create, "Fail to create secodary srq @II", ii);
				goto err;
			}
			srq_info->idx = ii;
			srq_info->type = NVMEIB_SRQ_INFO_SECONDARY;
			list_add_tail(&srq_info->link, &s->sec);
		}
		print_sec(s);
	}
	memcpy(&s->p, prim_q_params, sizeof(s->p));
	if (sec_qs_params) memcpy(&s->s, sec_qs_params, sizeof(s->s));
	rv = 0;
	goto out;

err:
	nvmeib_srq_pool_free(dev);

out:
	NFOUT;
	return rv;

}
EXPORT_SYMBOL(nvmeib_srq_pool_create);

void nvmeib_srq_pool_free(struct nvmeib_dev *dev)
{
	struct nvmeib_srqs *s = dev->srqs;
	struct nvmeib_srq_info *srq_info;
	NFIN;

	if (s) {
		while ((srq_info = list_first_entry_or_null(
			&s->sec, struct nvmeib_srq_info, link))) {
			list_del(&srq_info->link);
			if (srq_info_free(srq_info) < 0)
				goto out;
		}
		if (s->prim) {
			if (srq_info_free(s->prim) < 0)
				goto out;
			s->prim = NULL;
		}
		kfree(dev->srqs);
		dev->srqs = NULL;
	}

out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_srq_pool_free);

int nvmeib_srq_pool_query(struct nvmeib_dev *dev,
	struct nvmeib_srq_params *p, struct nvmeib_srq_params *s)
{
	int rv = 0;
	NFIN;

	if (dev && dev->srqs) {
		if (p) memcpy(p, &dev->srqs->p, sizeof(*p));
		if (s) memcpy(s, &dev->srqs->s, sizeof(*s));
	}
	else {
		_NE(error_nvmeib_srq_nvmeib_srq_pool_query, "Invalid dev @DEV", dev);
		rv = -1;
	}

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_srq_pool_query);

struct nvmeib_srq_info *nvmeib_srq_info_get(struct nvmeib_dev *dev,
	enum nvmeib_srq_type type)
{
	struct nvmeib_srqs *s = dev->srqs;
	struct nvmeib_srq_info *srq_info = NULL;
	struct nvmeib_srq_info *si;
	ulong flags;
	NFIN;

	if (!s) {
		_NE(error_nvmeib_srq_nvmeib_srq_info_get, "No SRQs, dev @IB_DEV_NAME", dev->ib_dev->name);
		goto out;
	}

	if (type == NVMEIB_SRQ_TYPE_PRIMARY) {
		srq_info = s->prim;
		goto out;
	}
	if (type != NVMEIB_SRQ_TYPE_SECONDARY) {
		_NE(error_1_nvmeib_srq_nvmeib_srq_info_get, "Invalid srq-type @TYPE", type);
		goto out;
	}

	/* inc use-cnt and push it down the list */
	spin_lock_irqsave(&s->guard, flags);
	if ((srq_info = list_first_entry_or_null(
		&s->sec, struct nvmeib_srq_info, link))) {
		srq_info->use_cnt++;
		_ND(trace_nvmeib_srq_nvmeib_srq_info_get, "dev @IB_DEV_NAME, srq_info @IDX.@SRQ_INFO: use-cnt inc to @USE_CNT",
			dev->ib_dev->name, srq_info->idx, srq_info, srq_info->use_cnt);
		si = srq_info;
		list_for_each_entry_continue(si, &s->sec, link) {
			if (si->use_cnt >= srq_info->use_cnt)
				break;
		}
		list_del(&srq_info->link);
		list_add_tail(&srq_info->link, &si->link);
	}
	else
		_NT(trace_1_nvmeib_srq_nvmeib_srq_info_get, "No secondary SRQs, dev @IB_DEV_NAME", dev->ib_dev->name);
	print_sec(s);
	spin_unlock_irqrestore(&s->guard, flags);

out:
	NFOUT;
	return srq_info;
}
EXPORT_SYMBOL(nvmeib_srq_info_get);

void nvmeib_srq_info_put(struct nvmeib_srq_info *srq_info, void *owner)
{
	struct nvmeib_dev *dev = srq_info->dev;
	struct nvmeib_srqs *s = dev->srqs;
	struct nvmeib_srq_info *si;
	ulong flags;
	int i;
	NFIN;

	/* checks */
	if (!s) {
		_NE(error_nvmeib_srq_nvmeib_srq_info_put, "No SRQs, dev @IB_DEV_NAME", dev->ib_dev->name);
		goto out;
	}
	if (srq_info->type == NVMEIB_SRQ_INFO_PRIMARY ||
		srq_info->type == NVMEIB_SRQ_INFO_PRIVATE) {
		/* nothing to do, not maintaining use-cnt for primary/private SRQ */
		goto out;
	}
	if (srq_info->type != NVMEIB_SRQ_INFO_SECONDARY) {
		_NE(error_1_nvmeib_srq_nvmeib_srq_info_put, "Invalid srq-info type @SRQ_INFO_TYPE", srq_info->type);
		goto out;
	}
	if (srq_info->idx < 0 || srq_info->idx >= s->num_sec) {
		_NE(error_2_nvmeib_srq_nvmeib_srq_info_put, "Invalid srq-info idx @IDX", srq_info->idx);
		goto out;
	}

	/* dec use-cnt and push it up the list */
	spin_lock_irqsave(&s->guard, flags);
	if (srq_info->use_cnt <= SRQ_USE_CNT_BASE) {
		_NW(warn_nvmeib_srq_nvmeib_srq_info_put, "dev @IB_DEV_NAME, srq_info @IDX.@SRQ_INFO: use-cnt @USE_CNT, cant dec (from '@__BUILTIN_RETURN_ADDRESS_FUNC')",
			dev->ib_dev->name, srq_info->idx, srq_info, srq_info->use_cnt,
			__builtin_return_address(0));
		WARN_ON_ONCE(1);
		goto unlock;
	}
	for (i = 0; i < srq_info->q_size; i++) {
		if (srq_info->rx_ring[i]->owner_ptr == owner) {
			_NE(err_nvmeib_srq_nvmeib_srq_info_put_not_all_ret,
					"dev @IB_DEV_NAME, srq_info @IDX.@SRQ_INFO: IU @RECV_IOCTX (@IDX) still owned by owner @PTR",
					dev->ib_dev->name, srq_info->idx, srq_info, srq_info->rx_ring[i], i, owner);
			BUG_ON(1);
		}
	}
	srq_info->use_cnt--;
	_ND(trace_nvmeib_srq_nvmeib_srq_info_put, "dev @IB_DEV_NAME, srq_info @IDX.@SRQ_INFO: use-cnt dec to @USE_CNT",
		dev->ib_dev->name, srq_info->idx, srq_info, srq_info->use_cnt);
	si = srq_info;
	list_for_each_entry_continue_reverse(si, &s->sec, link) {
		if (si->use_cnt <= srq_info->use_cnt)
			break;
	}
	list_del(&srq_info->link);
	list_add(&srq_info->link, &si->link);

unlock:
	print_sec(s);
	spin_unlock_irqrestore(&s->guard, flags);

out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_srq_info_put);

struct ib_srq *nvmeib_srq_info_ib_srq(struct nvmeib_srq_info *srq_info)
{
	NFIN;
	NFOUT;
	return srq_info ? srq_info->srq: NULL;
}
EXPORT_SYMBOL(nvmeib_srq_info_ib_srq);
