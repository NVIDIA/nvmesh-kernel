/* include as a C file in client and server modules */

/* sreate and destroy SRQ */
static int u_create_srq(struct u_dev *dev, int q_size, int msg_size,
	struct srq_info *srqi);
static void u_free_srq(struct u_dev *dev, struct srq_info *srqi);

/* retrieve the received buffer */
static inline struct u_iu *u_rtrv_recv(struct u_dev *dev, int index)
{
	return dev->srq.rx_ring[index];
}

/* post a receive buffer into SRQ */
static int u_post_recv(
	struct u_dev *dev, struct u_iu *iu, struct srq_info *srqi);

static void srq_event_handler(struct ib_event *event, void *ctx)
{
	struct u_dev *dev = ctx;
	_E("%s SRQ event %d\n", dev->ib_dev->name, event->event);
}

static struct u_iu *u_alloc_ioctx(
	struct u_dev *dev, int ioctx_size, int dma_size)
{
	struct u_iu *ioctx;

	BUG_ON(!(ioctx = kmalloc(ioctx_size, GFP_KERNEL)));
	BUG_ON(!(ioctx->buf = ib_dma_alloc_coherent(
		dev->ib_dev, dma_size, &ioctx->dma, GFP_KERNEL)));
	ioctx->size = dma_size;
	return ioctx;
}

static void u_free_ioctx(
	struct ib_device *dev, struct u_iu *ioctx, int dma_size)
{
	if (ioctx) {
		ib_dma_free_coherent(dev, dma_size, ioctx->buf, ioctx->dma);
		kfree(ioctx);
	}
}

static struct u_iu **u_alloc_ioctx_ring(struct u_dev *dev, int ring_size,
	int ioctx_size, int dma_size)
{
	struct u_iu **ring;
	int i;

	FIN;
	BUG_ON(!(ring = kmalloc(ring_size * sizeof(ring[0]), GFP_KERNEL)));
	for (i = 0; i < ring_size; ++i) {
		ring[i] = u_alloc_ioctx(dev, ioctx_size, dma_size);
		ring[i]->index = i;
	}
	FOUT;
	return ring;
}

static void u_free_ioctx_ring(struct u_iu **ioctx_ring,
	struct ib_device *dev, int ring_size, int dma_size)
{
	int i;

	FIN;
	if (ioctx_ring) {
		for (i = 0; i < ring_size; ++i)
			u_free_ioctx(dev, ioctx_ring[i], dma_size);
		kfree(ioctx_ring);
	}
	FOUT;
}

static int fill_recv_q(struct u_dev *dev, struct srq_info *srq)
{
	int rv = 0, i;

	for (i = 0; i < srq->srq_queue_size; i++) {
		struct u_iu *iu = srq->rx_ring[i];
		 if ((rv = u_post_recv(dev, iu, srq)) < 0) {
			 _E("ib_post_srq_recv() failed: %d\n", rv);
			 BUG();
		 }
	}
	return rv;
}

/* create volume shared receive queue */
static int u_create_srq(struct u_dev *dev, int q_size, int msg_size,
	struct srq_info *srqi)
{
	struct srq_info srq = {0};
	struct ib_srq_init_attr srq_attr;
	int rv = 0;

	FIN;
	q_size = min(q_size, 4096);
	_I("q_size=%d, msg_size=%d\n", q_size, msg_size);
	memset(&srq_attr, 0, sizeof(srq_attr));
	srq_attr.event_handler = srq_event_handler;
	srq_attr.srq_context = dev;
	srq_attr.attr.max_wr = q_size;
	srq_attr.attr.max_sge = 1;
	srq_attr.attr.srq_limit = 0;
	srq_attr.srq_type = IB_SRQT_BASIC;

	srq.srq = ib_create_srq(dev->pd, &srq_attr);
	if (IS_ERR_OR_NULL(srq.srq)) {
		rv = PTR_ERR(srq.srq);
		_E("Fail to create volume shared receive queue %d\n", rv);
		BUG();
	}
	else
		_I("srq=%p\n", srq.srq);
	srq.srq_queue_size = q_size;
	srq.srq_msg_size = msg_size;
	/* allocate receive buffers */
	srq.rx_ring = u_alloc_ioctx_ring(dev, q_size,
		sizeof(*srq.rx_ring[0]), msg_size);
	BUG_ON((rv = fill_recv_q(dev, &srq)));
	if (srqi)
		*srqi = srq;
	else
		dev->srq = srq;
	FOUT;
	return rv;
}

static void u_free_srq(struct u_dev *dev, struct srq_info *srqi)
{
	struct srq_info *srq = srqi == NULL ? &dev->srq : srqi;

	FIN;
	if (srq->rx_ring) {
		u_free_ioctx_ring(srq->rx_ring, dev->ib_dev,
			srq->srq_queue_size, srq->srq_msg_size);
		srq->rx_ring = NULL;
	}

	if (srq->srq) {
		ib_destroy_srq(srq->srq);
		srq->srq = NULL;
	}
	FOUT;
}

static int u_post_recv(
	struct u_dev *dev, struct u_iu *iu, struct srq_info *srqi)
{
	struct srq_info *srq = srqi == NULL ? &dev->srq : srqi;
	struct ib_recv_wr wr, *bad_wr;
	struct ib_sge list;
	int rv;

	list.addr = iu->dma;
	list.length = iu->size;
	list.lkey = dev->mr->lkey;

	memset(&wr, 0, sizeof(wr));
	wr.next = NULL;
	//_D("index=%d\n", iu->index);
	wr.wr_id = iu->index << 1;
	wr.sg_list = &list;
	wr.num_sge = 1;
	BUG_ON((rv = ib_post_srq_recv(srq->srq, &wr, &bad_wr)));
	return rv;
}


