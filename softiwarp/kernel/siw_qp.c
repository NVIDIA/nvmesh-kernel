/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *          Fredy Neeser <nfd@zurich.ibm.com>
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

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/file.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <asm/barrier.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/tcp.h>

#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"

bool notify_on_wq = 1;
module_param(notify_on_wq, bool, 0644);
MODULE_PARM_DESC(notify_on_wq, "Notify CQ on Workqueue (bool).");

static bool ack_signal_wr = 1;
module_param(ack_signal_wr, bool, 0644);
MODULE_PARM_DESC(ack_signal_wr, "Request responder to ack signaled writes (bool).");


#if DPRINT_MASK > 0
//omril: potential bug if used with idx > SIW_QP_STATE_COUNT
char siw_qp_state_to_string[SIW_QP_STATE_COUNT][sizeof "TERMINATE"] = {
	[SIW_QP_STATE_IDLE]		= "IDLE",
	[SIW_QP_STATE_RTR]		= "RTR",
	[SIW_QP_STATE_RTS]		= "RTS",
	[SIW_QP_STATE_CLOSING]		= "CLOSING",
	[SIW_QP_STATE_TERMINATE]	= "TERMINATE",
	[SIW_QP_STATE_ERROR]		= "ERROR",
	[SIW_QP_STATE_MORIBUND]		= "MORIBUND",
	[SIW_QP_STATE_UNDEF]		= "UNDEF"
};
#endif

/*
 * iWARP (RDMAP, DDP and MPA) parameters as well as Softiwarp settings on a
 * per-RDMAP message basis. Please keep order of initializer. All MPA len
 * is initialized to minimum packet size.
 */
struct iwarp_msg_info iwarp_pktinfo[RDMAP_TERMINATE + 1] = {
	[RDMAP_RDMA_WRITE] = {
		.hdr_len = sizeof(struct iwarp_rdma_write),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_rdma_write) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_TAGGED | DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_RDMA_WRITE),
		.proc_data = siw_proc_write,
		.max_payload = 65534 - sizeof(struct iwarp_rdma_write),
	},
	[RDMAP_RDMA_READ_REQ] = {
		.hdr_len = sizeof(struct iwarp_rdma_rreq),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_rdma_rreq) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_RDMA_READ_REQ),
		.proc_data = siw_proc_rreq,
	},
	[RDMAP_RDMA_READ_RESP] = {
		.hdr_len = sizeof(struct iwarp_rdma_rresp),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_rdma_rresp) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_TAGGED | DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_RDMA_READ_RESP),
		.proc_data = siw_proc_rresp,
		.max_payload = 65534 - sizeof(struct iwarp_rdma_rresp),
	},
	[RDMAP_SEND] = {
		.hdr_len = sizeof(struct iwarp_send),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_send) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_SEND),
		.proc_data = siw_proc_send,
		.max_payload = 65534 - sizeof(struct iwarp_send),
	},
	[RDMAP_SEND_INVAL] = {
		.hdr_len = sizeof(struct iwarp_send_inv),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_send_inv) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_SEND_INVAL),
		.proc_data = siw_proc_send,
		.max_payload = 65534 - sizeof(struct iwarp_send_inv),
	},
	[RDMAP_SEND_SE] = {
		.hdr_len = sizeof(struct iwarp_send),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_send) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_SEND_SE),
		.proc_data = siw_proc_send,
		.max_payload = 65534 - sizeof(struct iwarp_send),
	},
	[RDMAP_SEND_SE_INVAL] = {
		.hdr_len = sizeof(struct iwarp_send_inv),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_send_inv) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_SEND_SE_INVAL),
		.proc_data = siw_proc_send,
		.max_payload = 65534 - sizeof(struct iwarp_send_inv),
	},
	[RDMAP_RDMA_ATOMIC_REQ] = {
		.hdr_len = sizeof(struct iwarp_rdma_areq),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_rdma_areq) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_RDMA_ATOMIC_REQ),
		.proc_data = siw_proc_areq
	},
	[RDMAP_RDMA_ATOMIC_RESP] = {
		.hdr_len = sizeof(struct iwarp_rdma_aresp),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_rdma_aresp) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_TAGGED | DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_RDMA_ATOMIC_RESP),
		.proc_data = siw_proc_rresp
	},
	[RDMAP_TERMINATE] = {
		.hdr_len = sizeof(struct iwarp_terminate),
		.ctrl.mpa_len = htons(sizeof(struct iwarp_terminate) - 2),
		.ctrl.ddp_rdmap_ctrl = DDP_FLAG_LAST
			| cpu_to_be16(DDP_VERSION << 8)
			| cpu_to_be16(RDMAP_VERSION << 6)
			| cpu_to_be16(RDMAP_TERMINATE),
		.proc_data = siw_proc_terminate
	}
};

#ifdef SIW_TX_COMP_WAIT_ACK
void siw_tcp_cong_ack_event(struct sock *sk, u32 ack_flags)
{
	struct siw_qp *qp;

	read_lock(&sk->sk_callback_lock);
	if (unlikely(!sk->sk_user_data || !sk_to_qp(sk))) {
		//dprint(DBG_ON, " No QP: " dprint_ptr_str() "\n", sk->sk_user_data);
		goto done;
	}
	qp = sk_to_qp(sk);

	if (ack_flags & CA_ACK_WIN_UPDATE) {
		union siw_iwarp_tx_sent_fpdu_notify sent_fpdu_notify;
		sent_fpdu_notify.all = _load_shared(qp->tx_ctx.sent_fpdu_notify.all);

		/* Check if snd_una is after the end sequence number of the first sent fpdu */
		if (sent_fpdu_notify.armed &&
			after(tcp_sk(sk)->snd_una, sent_fpdu_notify.end_seq)) {
			dprint(DBG_TX_COMP, "(QP:%d) calling siw_sq_queue_work\n", QP_ID(qp));
			siw_sq_queue_work(qp, SIW_TX_CTX_PREF_SCQ_VECT);
		}
	}
done:
	read_unlock(&sk->sk_callback_lock);
}

#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
static void siw_qp_llp_data_ready(struct sock *sk, int flags)
#else
static void siw_qp_llp_data_ready(struct sock *sk)
#endif
{
	struct siw_qp		*qp;
	int rv;

	read_lock(&sk->sk_callback_lock);

	if (unlikely(!sk->sk_user_data || !(qp = sk_to_qp(sk)))) {
		dprint(DBG_ON, " No QP: " dprint_ptr_str() "\n", sk->sk_user_data);
		goto done;
	}

	siw_qp_get(qp);

	/* This check is needed to synchronize with siw_rx_work_handler.
	 * siw_rx_work_handler acquires a process lock (lock_sock) whereas
	 * the lock held here is bh_lock_sock. The two locks can be
	 * held by different threads at the same time, but bh_lock_sock
	 * allows a thread in BH context to safely check if the process
	 * lock is held. In this case, if the lock is held, queue work.
	 */
	if (sock_owned_by_user_nocheck(sk)) {
		siw_rx_queue_work(qp, 0);
		goto put_qp;
	}

	/* Call siw_rx_work_handler internal work handler.
	 * No need for socket locks as we are in callback context */
	if ((rv = siw_do_rx_work(qp)) < 0) {
		dprint(DBG_SK|DBG_RX, "(QP%d): "
		"siw_do_rx_work() returned error %d\n",
		       QP_ID(qp), rv);
	}

put_qp:
	siw_qp_put(qp);

done:
	read_unlock(&sk->sk_callback_lock);
}

void siw_qp_llp_close(struct siw_qp *qp)
{
	unsigned long flags;
	dprint(DBG_CM, "(QP%d): Enter: SIW QP state = %s, cep=" dprint_ptr_str() "\n",
		QP_ID(qp), siw_qp_state_to_string[qp->attrs.state],
		qp->cep);

	/* Set the suspend flags so tx thread, rx ctx will exit */
	lock_rq_rxsave(qp, flags);
	qp->rx_ctx.rx_suspend = 1;
	unlock_rq_rxsave(qp, flags);

	lock_sq_rxsave(qp, flags);
	qp->tx_ctx.tx_suspend = 1;
	unlock_sq_rxsave(qp, flags);

	write_lock_qp(qp);
	dprint(DBG_CM|DBG_ON, "(QP%d): state locked\n", QP_ID(qp));
	WRITE_ONCE(qp->attrs.llp_stream_handle, NULL);

	switch (qp->attrs.state) {

	case SIW_QP_STATE_RTS:
	case SIW_QP_STATE_RTR:
	case SIW_QP_STATE_IDLE:
	case SIW_QP_STATE_TERMINATE:

		smp_store_mb(qp->attrs.state, SIW_QP_STATE_ERROR);


		break;
	/*
	 * SIW_QP_STATE_CLOSING:
	 *
	 * This is a forced close. shall the QP be moved to
	 * ERROR or IDLE ?
	 */
	case SIW_QP_STATE_CLOSING:
		if (tx_wqe(qp)->wr_status == SR_WR_IDLE)
			smp_store_mb(qp->attrs.state, SIW_QP_STATE_ERROR);
		else
			smp_store_mb(qp->attrs.state, SIW_QP_STATE_IDLE);

		break;

	default:
		dprint(DBG_CM, " No state transition needed: %d\n",
			qp->attrs.state);
		break;
	}
	siw_sq_flush(qp);
	siw_rq_flush(qp);
	dprint(DBG_OL|DBG_ON, "SIW_QP_STATE_CLOSING qp, post IB_EVENT_QP_LAST_WQE_REACHED (QP%d)\n", QP_ID(qp));
	siw_qp_event(qp, IB_EVENT_QP_LAST_WQE_REACHED);

	/*
	 * dereference closing CEP
	 */
	if (qp->cep) {
		siw_cep_put(qp->cep);
		qp->cep = NULL;
	}

	do {
		int state_lock_failed;
		if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
			dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() " in state %d",
					QP_ID(qp), state_lock_failed, qp, qp->attrs.state);
			WARN_ON(1);
		}
	} while(0);
	write_unlock_qp(qp);
	dprint(DBG_CM, "(QP%d): Exit: SIW QP state = %s, cep=" dprint_ptr_str() "\n",
		QP_ID(qp), siw_qp_state_to_string[qp->attrs.state],
		qp->cep);
}


/*
 * socket callback routine informing about newly available send space.
 * Function schedules SQ work for processing SQ items.
 */
static void siw_qp_llp_write_space(struct sock *sk)
{
	struct siw_cep	*cep = sk_to_cep(sk);

	/*
	 * TODO:
	 * Resemble sk_stream_write_space() logic for iWARP constraints:
	 * Clear SOCK_NOSPACE only if sendspace may hold some reasonable
	 * sized FPDU.
	 */
#ifdef SIW_TX_FULLSEGS
	struct socket *sock = sk->sk_socket;
	if (sk_stream_wspace(sk) >= (int)cep->qp.tx_ctx.fpdu_len && sock) {
		clear_bit(SOCK_NOSPACE, &sock->flags);
		siw_sq_queue_work(cep->qp, SIW_TX_CTX_PREF_SCQ_VECT);
	}
#else
	cep->sk_write_space(sk);

	if (!test_bit(SOCK_NOSPACE, &sk->sk_socket->flags))
		siw_sq_queue_work(cep->qp, SIW_TX_CTX_PREF_SCQ_VECT);
#endif
}

static void siw_qp_socket_assoc(struct socket *s, struct siw_qp *qp)
{
	struct sock *sk = s->sk;

	write_lock_bh(&sk->sk_callback_lock);

	qp->attrs.llp_stream_handle = s;
	s->sk->sk_data_ready = siw_qp_llp_data_ready;
	s->sk->sk_write_space = siw_qp_llp_write_space;

	write_unlock_bh(&sk->sk_callback_lock);
}


static int siw_qp_readq_init(struct siw_qp *qp, int irq_size, int orq_size)
{
	/* If, remote has requested Write ACKs so we need space in the IRQ */
	int in_wr_ack_max = qp->attrs.mpa.wr_ack ? SIW_WR_ACK_MAX_OUTSTANDING : 0; 
	/* If QP has ack_signal_wr set, we will use Write ACKs so we need space in the ORQ */
	int out_wr_ack_max = qp->tx_ctx.ack_signal_wr ? SIW_WR_ACK_MAX_OUTSTANDING : 0;
	dprint(DBG_CM|DBG_WR, "(QP%d): %d %d\n", QP_ID(qp), irq_size, orq_size);

	if (!irq_size)
		irq_size = 1;
	if (!orq_size)
		orq_size = 1;

	qp->attrs.irq_size = max(irq_size, in_wr_ack_max);
	qp->attrs.orq_size = max(orq_size, out_wr_ack_max);

	if (qp->kernel_verbs) {
		if (SIW_NO_VMALLOC_FOR_KVERBS_QP)
			qp->irq = (void *)__get_free_pages(GFP_KERNEL, get_order(irq_size * sizeof(struct siw_sqe)));
		else
			qp->irq = vmalloc(irq_size * sizeof(struct siw_sqe));
	} else
		qp->irq = vmalloc_user(irq_size * sizeof(struct siw_sqe));

	if (!qp->irq) {
		dprint(DBG_ON, "(QP%d): Failed\n", QP_ID(qp));
		qp->attrs.irq_size = 0;
		return -ENOMEM;
	}
	
	if (qp->kernel_verbs) {
		if (SIW_NO_VMALLOC_FOR_KVERBS_QP)
			qp->orq = (void *)__get_free_pages(GFP_KERNEL, get_order(orq_size * sizeof(struct siw_sqe)));
		else
			qp->orq = vmalloc(orq_size * sizeof(struct siw_sqe));
	} else
		qp->orq = vmalloc_user(orq_size * sizeof(struct siw_sqe));

	if (!qp->orq) {
		dprint(DBG_ON, "(QP%d): Failed\n", QP_ID(qp));
		qp->attrs.orq_size = 0;
		qp->attrs.irq_size = 0;
		if (SIW_NO_VMALLOC_FOR_KVERBS_QP)
			free_pages((unsigned long)qp->irq, get_order(irq_size * sizeof(struct siw_sqe)));
		else
			vfree(qp->irq);
		return -ENOMEM;
	}
	memset(qp->irq, 0, irq_size * sizeof(struct siw_sqe));
	memset(qp->orq, 0, orq_size * sizeof(struct siw_sqe));

	return 0;
}


static void siw_send_terminate(struct siw_qp *qp)
{
	struct iwarp_terminate	pkt;

	memset(&pkt, 0, sizeof pkt);
	/*
	 * TODO: send TERMINATE
	 */
	dprint(DBG_CM, "(QP%d): Todo\n", QP_ID(qp));
}


static int siw_qp_enable_crc(struct siw_qp *qp)
{
	struct siw_iwarp_rx *c_rx = &qp->rx_ctx;
	struct siw_iwarp_tx *c_tx = &qp->tx_ctx;
	struct crypto_shash *txsh, *rxsh;
	int rv = 0;

	txsh = crypto_alloc_shash("crc32c", 0, 0);
	if (IS_ERR(txsh))
		return -PTR_ERR(txsh);

	rxsh = crypto_alloc_shash("crc32c", 0, 0);
	if (IS_ERR(rxsh)) {
		rv = -PTR_ERR(rxsh);
		rxsh = NULL;
	 	goto error;
	}

	c_tx->mpa_crc_hd = kzalloc(sizeof(struct shash_desc) +
				   crypto_shash_descsize(txsh),
				   GFP_KERNEL);
	c_rx->mpa_crc_hd = kzalloc(sizeof(struct shash_desc) +
				   crypto_shash_descsize(rxsh),
				   GFP_KERNEL);
	if (!c_tx->mpa_crc_hd || !c_rx->mpa_crc_hd) {
		rv = -ENOMEM;
		goto error;
	}
		
	c_tx->mpa_crc_hd->tfm = txsh;
	c_rx->mpa_crc_hd->tfm = rxsh;

	return 0;
error:
	dprint(DBG_ON, "(QP%d): Failed loading crc32c: error=%d.",
			QP_ID(qp), rv);

	kfree(c_tx->mpa_crc_hd);
	kfree(c_rx->mpa_crc_hd);

	c_tx->mpa_crc_hd = c_rx->mpa_crc_hd = NULL;

	if (txsh)
		crypto_free_shash(txsh);
	if (rxsh)
		crypto_free_shash(rxsh);

	return rv;
}


/*
 * caller holds qp->state_lock
 */
int
siw_qp_modify(struct siw_qp *qp, struct siw_qp_attrs *attrs,
	      enum siw_qp_attr_mask mask)
{
	int	drop_conn = 0, rv = 0;
	unsigned long flags;

	if (!mask)
		return 0;

	dprint(DBG_CM, "Modify (QP%d)(QP=" dprint_ptr_str() ")(QPX=" dprint_ptr_str() ")\n", QP_ID(qp), &qp->ofa_qp, &qp->ofa_qp);

	if (mask != SIW_QP_ATTR_STATE) {
		/*
		 * changes of qp attributes (maybe state, too)
		 */
		if (mask & SIW_QP_ATTR_ACCESS_FLAGS) {

			if (attrs->flags & SIW_RDMA_BIND_ENABLED)
				qp->attrs.flags |= SIW_RDMA_BIND_ENABLED;
			else
				qp->attrs.flags &= ~SIW_RDMA_BIND_ENABLED;

			if (attrs->flags & SIW_RDMA_WRITE_ENABLED)
				qp->attrs.flags |= SIW_RDMA_WRITE_ENABLED;
			else
				qp->attrs.flags &= ~SIW_RDMA_WRITE_ENABLED;

			if (attrs->flags & SIW_RDMA_READ_ENABLED)
				qp->attrs.flags |= SIW_RDMA_READ_ENABLED;
			else
				qp->attrs.flags &= ~SIW_RDMA_READ_ENABLED;

		}
		/*
		 * TODO: what else ??
		 */
	}
	if (!(mask & SIW_QP_ATTR_STATE))
		return 0;

	dprint(DBG_CM|DBG_ON, "(QP%d): SIW QP state: %s => %s\n", QP_ID(qp),
		siw_qp_state_to_string[qp->attrs.state],
		siw_qp_state_to_string[attrs->state]);


	switch (qp->attrs.state) {

	case SIW_QP_STATE_IDLE:
	case SIW_QP_STATE_RTR:

		switch (attrs->state) {

		case SIW_QP_STATE_RTS:

			if (attrs->mpa.crc) {
#ifdef SIW_RX_CRC_IN_SKB
				qp->rx_ctx.do_skb_crc = true;
#else
				rv = siw_qp_enable_crc(qp);
				if (rv)
					break;
#ifdef SIW_DEBUG_RX_CRC
				/* Max value of gso_size */
				qp->rx_ctx.curr_fpdu = (void *)__get_free_pages(GFP_KERNEL, get_order(SIW_DEBUG_RX_CRC_MAX_FPDU));
				qp->rx_ctx.prev_fpdu = (void *)__get_free_pages(GFP_KERNEL, get_order(SIW_DEBUG_RX_CRC_MAX_FPDU));
				qp->rx_ctx.curr_fpdu_shash = crypto_alloc_shash("crc32c", 0, 0);
				qp->rx_ctx.curr_fpdu_shash_desc = kzalloc(sizeof(struct shash_desc) +
						   crypto_shash_descsize(qp->rx_ctx.curr_fpdu_shash),
						   GFP_KERNEL);
				qp->rx_ctx.curr_fpdu_shash_desc->tfm = qp->rx_ctx.curr_fpdu_shash;
				qp->rx_ctx.curr_fpdu_crc_bytes_rem = INT_MAX;
				qp->rx_ctx.crc_trace_log = (void *)__get_free_pages(GFP_KERNEL, get_order(SIW_DEBUG_RX_CRC_TRACE_LOG_SZ));
#endif
#endif
			} else {
				qp->tx_ctx.trailer_page_virt = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
				if (qp->tx_ctx.trailer_page_virt) {
					*(u32 *)qp->tx_ctx.trailer_page_virt = MPA_CRC_OFF_MAGIC;
					qp->tx_ctx.trailer_page = virt_to_page(qp->tx_ctx.trailer_page_virt);
				}
			}
			if (!(mask & SIW_QP_ATTR_LLP_HANDLE)) {
				dprint(DBG_ON, "(QP%d): socket?\n", QP_ID(qp));
				rv = -EINVAL;
				break;
			}
			if (!(mask & SIW_QP_ATTR_MPA)) {
				dprint(DBG_ON, "(QP%d): MPA?\n", QP_ID(qp));
				rv = -EINVAL;
				break;
			}
			do {
				struct sockaddr_storage local, peer = {};
				getname_local(qp->cep->llp.sock, &local);
				getname_peer(qp->cep->llp.sock, &peer);
				if (local.ss_family == AF_INET) {
					dprint(DBG_CM, "(QP%d): Enter RTS: "
						"peer %pI4b, local %pI4b\n", QP_ID(qp),
						&to_sockaddr_in(peer).sin_addr,
						&to_sockaddr_in(local).sin_addr);
				} else if (local.ss_family == AF_INET6) {
					dprint(DBG_CM, "(QP%d): Enter RTS: "
					"peer %pI6c, local %pI6c\n", QP_ID(qp),
					       &to_sockaddr_in6(peer).sin6_addr,
					       &to_sockaddr_in6(local).sin6_addr);
				} else
					BUG_ON(1);
			} while(0);
			/*
			 * Initialize global iWARP TX state
			 */
			qp->tx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_SEND] = 0;
			qp->tx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_RDMA_READ] = 0;
			qp->tx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_RDMA_ATOMIC] = 0;
			qp->tx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_TERMINATE] = 0;

			/*
			 * Initialize global iWARP RX state
			 */
			qp->rx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_SEND] = 1;
			qp->rx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_RDMA_READ] = 1;
			qp->rx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_RDMA_ATOMIC] = 1;
			qp->rx_ctx.ddp_msn[RDMAP_UNTAGGED_QN_TERMINATE] = 1;

			/*
			 * init IRD free queue, caller has already checked
			 * limits.
			 */
			rv = siw_qp_readq_init(qp, attrs->irq_size,
					       attrs->orq_size);
			if (rv)
				break;

			qp->attrs.mpa = attrs->mpa;
			/*
			 * move socket rx and tx under qp's control
			 */
			siw_qp_socket_assoc(attrs->llp_stream_handle, qp);

			qp->attrs.state = SIW_QP_STATE_RTS;
			/*
			 * set initial mss
			 */
			qp->tx_ctx.tcp_seglen =
				get_tcp_mss(attrs->llp_stream_handle->sk);
				
			qp->notify_on_wq = notify_on_wq;
			qp->tx_ctx.ack_signal_wr = ack_signal_wr & qp->attrs.mpa.wr_ack;

			break;

		case SIW_QP_STATE_ERROR:
			lock_sq_rxsave(qp, flags);
			BUG_ON(qp->sq_put != 0);
			qp->tx_ctx.sq_flushed = 1;
			unlock_sq_rxsave(qp, flags);

			siw_rq_flush(qp);
			qp->attrs.state = SIW_QP_STATE_ERROR;

			dprint(DBG_OL|DBG_ON, "IDLE/RTR -> ERROR: generate LAST-WQE event (QP=" dprint_ptr_str() ")...\n", qp);
			siw_qp_event(qp, IB_EVENT_QP_LAST_WQE_REACHED);

#if 1
			/* Allow modifying the QP to ERROR to cancel the in-progress CM operation */
			drop_conn = 1;
#else
			if (qp->cep) {
				siw_cep_put(qp->cep);
				qp->cep = NULL;
			}
#endif
			break;

		case SIW_QP_STATE_RTR:
			/* ignore */
			break;

		default:
			dprint(DBG_CM,
				" QP state transition undefined: %s => %s\n",
				siw_qp_state_to_string[qp->attrs.state],
				siw_qp_state_to_string[attrs->state]);
			//rv = -EINVAL;
			break;
		}
		break;

	case SIW_QP_STATE_RTS:

		switch (attrs->state) {

		case SIW_QP_STATE_CLOSING:
			/*
			 * Verbs: move to IDLE if SQ and ORQ are empty.
			 * Move to ERROR otherwise. But first of all we must
			 * close the connection. So we keep CLOSING or ERROR
			 * as a transient state, schedule connection drop work
			 * and wait for the socket state change upcall to
			 * come back closed.
			 */
			if (tx_wqe(qp)->wr_status == SR_WR_IDLE && !qp->tx_ctx.tx_suspend) {
				lock_sq_rxsave(qp, flags);
				BUG_ON(qp->sq_put != qp->sq_get);
				qp->tx_ctx.sq_flushed = 1;
				unlock_sq_rxsave(qp, flags);
				qp->attrs.state = SIW_QP_STATE_CLOSING;
			} else {
				siw_sq_flush(qp);
				qp->attrs.state = SIW_QP_STATE_ERROR;
			}
			siw_rq_flush(qp);

			dprint(DBG_OL|DBG_ON, "RTS -> CLOSING: generate LAST-WQE event (QP=" dprint_ptr_str() ")...\n", qp);
			siw_qp_event(qp, IB_EVENT_QP_LAST_WQE_REACHED);

			drop_conn = 1;
			break;

		case SIW_QP_STATE_TERMINATE:
			qp->attrs.state = SIW_QP_STATE_TERMINATE;
			siw_send_terminate(qp);
			drop_conn = 1;

			break;

		case SIW_QP_STATE_ERROR:
			/*
			 * This is an emergency close.
			 *
			 * Any in progress transmit operation will get
			 * cancelled.
			 * This will likely result in a protocol failure,
			 * if a TX operation is in transit. The caller
			 * could unconditional wait to give the current
			 * operation a chance to complete.
			 * Esp., how to handle the non-empty IRQ case?
			 * The peer was asking for data transfer at a valid
			 * point in time.
			 */
			dprint(DBG_OL|DBG_ON, "SIW_QP_STATE_ERROR (QP=" dprint_ptr_str() ")\n", qp);
			siw_sq_flush(qp);
			siw_rq_flush(qp);
			qp->attrs.state = SIW_QP_STATE_ERROR;
			siw_qp_event(qp, IB_EVENT_QP_LAST_WQE_REACHED);
			dprint(DBG_OL|DBG_ON, "Sent IB_EVENT_QP_LAST_WQE_REACHED (QP=" dprint_ptr_str() ")\n", qp);
			drop_conn = 1;

			break;

		default:
			dprint(DBG_ON,
				" QP state transition undefined: %s => %s\n",
				siw_qp_state_to_string[qp->attrs.state],
				siw_qp_state_to_string[attrs->state]);
			break;
		}
		break;

	case SIW_QP_STATE_TERMINATE:

		switch (attrs->state) {

		case SIW_QP_STATE_ERROR:
			siw_rq_flush(qp);
			qp->attrs.state = SIW_QP_STATE_ERROR;

			if (tx_wqe(qp)->wr_status != SR_WR_IDLE)
				siw_sq_flush(qp);

			break;

		default:
			dprint(DBG_ON,
				" QP state transition undefined: %s => %s\n",
				siw_qp_state_to_string[qp->attrs.state],
				siw_qp_state_to_string[attrs->state]);
		}
		break;

	case SIW_QP_STATE_CLOSING:

		switch (attrs->state) {

		case SIW_QP_STATE_IDLE:
			BUG_ON(tx_wqe(qp)->wr_status != SR_WR_IDLE);
			qp->attrs.state = SIW_QP_STATE_IDLE;

			break;

		case SIW_QP_STATE_CLOSING:
			/*
			 * The LLP may already moved the QP to closing
			 * due to graceful peer close init
			 */
			break;

		case SIW_QP_STATE_ERROR:
			/*
			 * QP was moved to CLOSING by LLP event
			 * not yet seen by user.
			 */
			qp->attrs.state = SIW_QP_STATE_ERROR;

			if (tx_wqe(qp)->wr_status != SR_WR_IDLE)
				siw_sq_flush(qp);

			siw_rq_flush(qp);

			break;

		default:
			dprint(DBG_CM,
				" QP state transition undefined: %s => %s\n",
				siw_qp_state_to_string[qp->attrs.state],
				siw_qp_state_to_string[attrs->state]);
			return -ECONNABORTED;
		}
		break;

	default:
		dprint(DBG_CM, " NOP: State: %d\n", qp->attrs.state);
		break;
	}
	if (drop_conn)
		siw_qp_cm_drop(qp, 0);

	return rv;
}

struct ib_qp *siw_get_ofaqp(struct ib_device *ofa_dev, int id)
{
	struct siw_qp *qp =  siw_qp_id2obj(siw_dev_ofa2siw(ofa_dev), id);

	dprint(DBG_OBJ, ": dev_name: %s, OFA QPID: %d, QP: " dprint_ptr_str() "\n",
		ofa_dev->name, id, qp);
	if (qp) {
		/*
		 * siw_qp_id2obj() increments object reference count
		 */
		siw_qp_put(qp);
		dprint(DBG_OBJ, " QPID: %d\n", QP_ID(qp));
		return &qp->ofa_qp;
	}
	return (struct ib_qp *)NULL;
}

/*
 * siw_check_mem()
 *
 * Check protection domain, STAG state, access permissions and
 * address range for memory object.
 *
 * @pd:		Protection Domain memory should belong to
 * @mem:	memory to be checked
 * @addr:	starting addr of mem
 * @perms:	requested access permissions
 * @len:	len of memory interval to be checked
 *
 */
int siw_check_mem(struct siw_pd *pd, struct siw_mem *mem, u64 addr,
		  enum siw_access_flags perms, int len)
{
	if (siw_mem2mr(mem)->pd != pd) {
		dprint(DBG_WR|DBG_ON, "(PD%d): PD mismatch " dprint_ptr_str() " : " dprint_ptr_str() "\n",
			OBJ_ID(pd),
			siw_mem2mr(mem)->pd, pd);

		return -EINVAL;
	}
	if (mem->stag_valid == 0) {
		dprint(DBG_WR|DBG_ON, "(PD%d): STAG 0x%08x invalid\n",
			OBJ_ID(pd), OBJ_ID(mem));
		return -EPERM;
	}
	/*
	 * check access permissions
	 */
	if ((mem->perms & perms) < perms) {
		dprint(DBG_WR|DBG_ON, "(PD%d, MEM%d): "
			"INSUFFICIENT permissions 0x%08x : 0x%08x\n",
			OBJ_ID(pd), siw_mem2mr(mem)->mem.hdr.id, mem->perms, perms);
		return -EPERM;
	}
	/*
	 * Check address interval: we relax check to allow memory shrinked
	 * from the start address _after_ placing or fetching len bytes.
	 * TODO: this relaxation is probably overdone
	 */
	if (addr < mem->va || addr + len > mem->va + mem->len) {
		dprint(DBG_WR|DBG_ON, "(PD%d): MEM interval len %d "
			"[0x%016llx, 0x%016llx) out of bounds "
			"[0x%016llx, 0x%016llx) for LKey=0x%08x\n",
			OBJ_ID(pd), len, (unsigned long long)addr,
			(unsigned long long)(addr + len),
			(unsigned long long)mem->va,
			(unsigned long long)(mem->va + mem->len),
			OBJ_ID(mem));

		return -EINVAL;
	}
	return 0;
}

/*
 * siw_check_sge()
 *
 * Check SGE for access rights in given interval
 *
 * @pd:		Protection Domain memory should belong to
 * @sge:	SGE to be checked
 * @mem:	resulting memory reference if successful
 * @perms:	requested access permissions
 * @off:	starting offset in SGE
 * @len:	len of memory interval to be checked
 *
 * NOTE: Function references SGE's memory object (mem->obj)
 * if not yet done. New reference is kept if check went ok and
 * released if check failed. If mem->obj is already valid, no new
 * lookup is being done and mem is not released it check fails.
 */
int
siw_check_sge(struct siw_pd *pd, struct siw_sge *sge,
	      union siw_mem_resolved *mem, enum siw_access_flags perms,
	      u32 off, int len)
{
	struct siw_dev	*sdev = pd->hdr.sdev;
	int		new_ref = 0, rv = 0;

	if (len + off > sge->length) {
		rv = -EPERM;
		goto fail;
	}
	if (mem->obj == NULL) {
		dprint(DBG_OL, "calling siw_mem_id2obj: sdev=" dprint_ptr_str() ", sge->lkey=%x\n",
			sdev, sge->lkey);

		mem->obj = siw_mem_id2obj(sdev, sge->lkey >> 8);
		if (mem->obj == NULL) {
			rv = -EINVAL;
			goto fail;
		}
		new_ref = 1;
	}

	rv = siw_check_mem(pd, mem->obj, sge->laddr + off, perms, len);
	if (rv)
		goto fail;

	return 0;

fail:
	if (new_ref) {
		siw_mem_put(mem->obj);
		mem->obj = NULL;
	}
	return rv;
}

void siw_read_to_orq(struct siw_sqe *rreq, struct siw_sqe *sqe)
{
	rreq->id = sqe->id;
	rreq->opcode = sqe->opcode;
	rreq->sge[0].laddr = sqe->sge[0].laddr;
	rreq->sge[0].length = sqe->sge[0].length;
	rreq->sge[0].lkey = sqe->sge[0].lkey;
	rreq->sge[1].lkey = sqe->sge[1].lkey;
	rreq->flags = sqe->flags | SIW_WQE_VALID;
	rreq->num_sge = 1;
	
	/* For Debug */
	rreq->compare_add = sqe->compare_add;
	rreq->compare_add_mask = sqe->compare_add_mask;
	rreq->swap = sqe->swap;
	rreq->swap_mask = sqe->swap_mask;
	rreq->rkey = sqe->rkey;
	rreq->raddr = sqe->raddr;
}

#if 1
#define SIW_ATOMIC_MAX_MASK_CMP_XCHG_ATTEMPTS 1024

static int siw_atomic_cmpxchg_ptr(u64 *xchg_ptr, u64 cmp, u64 cmp_mask, u64 xchg, u64 xchg_mask, u64 *old)
{
	if (likely(cmp_mask == ~(u64)0 && xchg_mask == ~(u64)0))
		*old = cmpxchg(xchg_ptr, cmp, xchg);
	else {
		const u64 lo32_mask = ~(u32)0;
		const u64 hi32_mask = lo32_mask << 32;
		if (cmp_mask == lo32_mask && xchg_mask == lo32_mask) {
#if defined(__LITTLE_ENDIAN)
			u32 *lock_lo_ptr = (u32*)xchg_ptr;
#else
			u32 *lock_lo_ptr = (u32*)xchg_ptr + 1;
#endif
			/* if cmpswap succeeded we need to read the upper word again */
			*old = (u64)cmpxchg(lock_lo_ptr, (u32)cmp, (u32)xchg) | (*xchg_ptr & hi32_mask);
		} else if (cmp_mask == hi32_mask && xchg_mask == hi32_mask) {
#if defined(__LITTLE_ENDIAN)
			u32 *lock_hi_ptr = (u32*)xchg_ptr + 1;
#else
			u32 *lock_hi_ptr = (u32*)xchg_ptr;
#endif
			/* if cmpswap succeeded we need to read the lower word again */
			*old = ((u64)cmpxchg(lock_hi_ptr, (u32)(cmp >> 32), (u32)(xchg >> 32)) << 32) | (*xchg_ptr & lo32_mask);
		} else {
			/* No special case. Have to use 64-bit cmpxchg. May require multiple attempts */
			int i = 0;
			u64 old_val_mask;
			u64 new_val_mask;
			do {
				old_val_mask =
				((*old) & ~cmp_mask) | (cmp & cmp_mask);
				new_val_mask = ((*old) & ~xchg_mask) | (xchg & xchg_mask);
				(*old) = cmpxchg(xchg_ptr, old_val_mask, new_val_mask);
				
			} while ((*old) != old_val_mask &&
			((*old) & cmp_mask) == (cmp & cmp_mask) &&
			i++ < SIW_ATOMIC_MAX_MASK_CMP_XCHG_ATTEMPTS);
			if (i == SIW_ATOMIC_MAX_MASK_CMP_XCHG_ATTEMPTS) {
				dprint(DBG_ATOMIC, "SIW_ATOMIC: cmp=%016llx/%016llx, xchg=%016llx/%016llx, "
					"addr=" dprint_ptr_str() ", old=%016llx - Failed - TOO MANY ATTEMPTS!\n",
					cmp, cmp_mask, xchg, xchg_mask, xchg_ptr, *xchg_ptr);
				return -EBUSY;
			}
		}
	}
	dprint(DBG_ATOMIC, "SIW_ATOMIC: cmp=%016llx/%016llx, xchg=%016llx/%016llx, "
			"addr=" dprint_ptr_str() ", old=%016llx, new=%016llx, %s\n",
			cmp, cmp_mask, xchg, xchg_mask, xchg_ptr, *old, *xchg_ptr, 
			((cmp & cmp_mask) == (*old & cmp_mask) ? "Success" : "Failure"));

	return 0;
}

#else
static DEFINE_SPINLOCK(siw_atomic_lock);
static int siw_atomic_cmpxchg_ptr(u64 *xchg_ptr, u64 cmp, u64 cmp_mask, u64 xchg, u64 xchg_mask, u64 *old)
{
	unsigned long flags;
	u64 prev, new;
	bool success = false;
	spin_lock_irqsave(&siw_atomic_lock, flags);
	new = prev = *xchg_ptr;
	if ((*xchg_ptr & cmp_mask) == (cmp & cmp_mask)) {
		new = (xchg & xchg_mask) | (prev & ~xchg_mask);
		*xchg_ptr = new;
		success = true;
	}
	spin_unlock_irqrestore(&siw_atomic_lock, flags);
	*old = prev;

	dprint(DBG_ATOMIC, "SIW_ATOMIC: cmp=%016llx/%016llx, xchg=%016llx/%016llx, "
		"addr=" dprint_ptr_str() ", old=%016llx, new = %016llx %s\n", 
		cmp, cmp_mask, xchg, xchg_mask, xchg_ptr, prev, new, 
		(success ? "Success" : "Failure"));
	
	return 0;
}
#endif

static int siw_atomic_cmpxchg(struct siw_qp *qp, struct siw_wqe *wqe, 
							  u64 compare_add, u64 compare_add_mask, u64 swap, u64 swap_mask, u64 *old)
{
	int rv;
	struct siw_mr *mr;

	if (wqe->sqe.num_sge != 1 || wqe->sqe.sge[0].length != sizeof(u64)) {
		dprint(DBG_ATOMIC, "(QP%d): ARESP with invalid sges\n", QP_ID(qp));
		rv = -EINVAL;
		goto out;
	}

	/* Resolve the SGE to get the MR to perform the atomic operation on */
	if ((rv = siw_check_sge(qp->pd, wqe->sqe.sge, wqe->mem, SR_MEM_RATOMIC, 0, sizeof(u64))) != 0) {
		dprint(DBG_ATOMIC, "(QP%d): ARESP with invalid SGE[0]: lkey %x laddr %llx length %u\n", QP_ID(qp), wqe->sqe.sge[0].lkey, wqe->sqe.sge[0].laddr, wqe->sqe.sge[0].length);
		rv = -EINVAL;
		goto out;
	}

	mr = siw_mem2mr(wqe->mem[0].obj);
	if (mr->mem_obj == NULL) {
		/* laddr = Kernel Virtual Address */
		rv = siw_atomic_cmpxchg_ptr((u64 *)wqe->sqe.sge[0].laddr,
								compare_add,
								compare_add_mask, 
								swap,
								swap_mask, 
								old);
	} else if (!mr->mem.is_pbl) {
		/* laddr = offset into umem region */
		int pg_off = wqe->sqe.sge[0].laddr & ~PAGE_MASK;
		struct page *p = siw_get_upage(mr->umem, wqe->sqe.sge[0].laddr);
		void *pg_virt = kmap_atomic(p);

		rv = siw_atomic_cmpxchg_ptr((u64 *)(pg_virt + pg_off),
									compare_add,
									compare_add_mask, 
									swap,
									swap_mask, 
									old);

		kunmap_atomic(pg_virt);
	}
	else {
		/* laddr = offset into pbl */
		struct siw_pbl *pbl = mr->pbl;
		u64 offset = wqe->sqe.sge[0].laddr - mr->mem.va;
		int bytes, idx = 0;
		u64 buf_addr;
		
		buf_addr = siw_pbl_get_buffer(pbl, offset, &bytes, &idx);
		if (buf_addr == 0) {
			dprint(DBG_MM, "offset %llx out of range\n",
				   offset);
			rv = -EINVAL;
			goto unref;
		}
		if (bytes < sizeof(u64)) {
			dprint(DBG_MM, "offset %llx, dw cross page boundary\n",
				   offset);
			rv = -EINVAL;
			goto unref;
		}

		rv = siw_atomic_cmpxchg_ptr((u64 *)buf_addr,
									compare_add,
									compare_add_mask, 
									swap,
									swap_mask,
									old);
	}

unref:
	/* Unref the memory as we are now finished with it */
	siw_mem_put(wqe->mem[0].obj);
	wqe->mem[0].obj = NULL;
	
out:
	return rv;
}

int siw_activate_tx_atomic(struct siw_qp *qp, u64 compare_add, u64 compare_add_mask, u64 swap, u64 swap_mask)
{
	struct siw_wqe	*wqe = tx_wqe(qp);
	int rv;
	
	BUG_ON(wqe->sqe.opcode != SIW_OP_COMP_AND_SWAP_RESPONSE);

	/* First, we perform the atomic operation, and then convert the WQE to an inline SEND to transmit the response */
	if ((rv = siw_atomic_cmpxchg(qp, wqe, compare_add, compare_add_mask, 
								swap, swap_mask, (u64 *)&wqe->sqe.sge[1])) < 0) {
		dprint(DBG_ATOMIC,
			   "(QP%d): ARESP: Failed to process atomics (rv=%d)\n",\
			   QP_ID(qp), rv);
		goto out;
	}
	
	/* This is not actually necessary as these fields are not transmitted as part of the response. Useful for debugging though */
	wqe->sqe.compare_add = compare_add;
	wqe->sqe.compare_add_mask = compare_add_mask;
	wqe->sqe.swap = swap;
	wqe->sqe.swap_mask = swap_mask;
	
	/* Use INLINE send, easiest way to send 8-bytes without mucking around with fake MRs */
	wqe->sqe.sge[0].laddr = (u64)&wqe->sqe.sge[1];
	wqe->sqe.sge[0].lkey = 0;
	wqe->sqe.num_sge = 1;
	smp_store_mb(wqe->sqe.flags, SIW_WQE_VALID | SIW_WQE_INLINE);
	
	rv = 0;

out:
	return rv;
}

/*
 * Called when tx_wqe(qp)->wr_status == SR_WR_IDLE and populates it.
 * Must be called with SQ locked
 */
int siw_activate_tx(struct siw_qp *qp)
{
	struct siw_sqe	*sqe;
	struct siw_sqe_md *sqe_md = NULL;
	struct siw_wqe	*wqe = tx_wqe(qp);
	int rv = 1;

	dprint(DBG_TX, "(QP%d)\n", QP_ID(qp));

	if (unlikely(wqe->wr_status != SR_WR_IDLE)) {
		WARN_ON(1);
		return -1;
	}

	/* SQ lock is held before calling this fn */
	if (qp->tx_ctx.tx_suspend) {
		dprint(DBG_ON, "(QP%d): TX suspended\n", QP_ID(qp));
		return -ESHUTDOWN;
	}

	/*
	 * This codes prefers pending READ Responses over SQ processing
	 */
	sqe = &qp->irq[qp->irq_get % qp->attrs.irq_size];

	if (sqe->flags & SIW_WQE_VALID) {
		dprint(DBG_OL, "(QP%d): TXTX: handle pending IRQ (qp->irq_get=%d), sqe=" dprint_ptr_str() "\n",
			QP_ID(qp), qp->irq_get, sqe);

		memset(&wqe->sqe_md, 0, sizeof *sqe_md);
		memset(wqe->mem, 0, sizeof *wqe->mem * SIW_MAX_SGE);
		wqe->wr_status = SR_WR_QUEUED;

		if (sqe->opcode != SIW_OP_READ_RESPONSE &&
			sqe->opcode != SIW_OP_COMP_AND_SWAP_RESPONSE &&
			sqe->opcode != SIW_OP_WRITE_RESPONSE) {
			rv = -EPROTO;
			goto out;
		}

		//omril: @sqe was filled in siw_init_rresp/siw_init_aresp
		//omril: why dont we copy the entire sqe to wqe->sqe? probably because of member that already valid
#if SIW_ATOMIC_CAP
		wqe->sqe.opcode = sqe->opcode;
#else
		wqe->sqe.opcode = SIW_OP_READ_RESPONSE;
#endif
		wqe->sqe.flags = 0;
		wqe->sqe.num_sge = 1;
		wqe->sqe.rkey = sqe->rkey;
		wqe->sqe.raddr = sqe->raddr;
		wqe->sqe.sge[0].length = sqe->sge[0].length;
		wqe->sqe.sge[0].laddr = sqe->sge[0].laddr;
		wqe->sqe.sge[0].lkey = sqe->sge[0].lkey;

		if (sqe->opcode == SIW_OP_COMP_AND_SWAP_RESPONSE) {
			if (!(rv = siw_activate_tx_atomic(qp, 
										sqe->compare_add, 
										sqe->compare_add_mask, 
										sqe->swap, 
										sqe->swap_mask))) {
				/* This function returns 1 for ready to tx, 0 for no error, but nothing to tx */
				rv = 1;
			}
		}

		wqe->processed = 0;
		qp->irq_get++;
		dprint(DBG_OL, "(QP%d): TXTX: inc irq_get to %d (irq_put=%d)\n",
			QP_ID(qp), qp->irq_get, qp->irq_put);

		smp_store_mb(sqe->flags, 0);

		goto out;
	}

	sqe = sq_get_next(qp, &sqe_md);
	if (sqe) {
		unsigned long flags;

		dprint(DBG_OL, "(QP%d): TXTX: handle pending sendq\n",
			QP_ID(qp));

		memset(wqe->mem, 0, sizeof *wqe->mem * SIW_MAX_SGE);
		wqe->wr_status = SR_WR_QUEUED;

		/* First copy SQE to kernel private memory */
		memcpy(&wqe->sqe, sqe, sizeof *sqe);
		if (sqe_md)
			memcpy(&wqe->sqe_md, sqe_md, sizeof *sqe_md);
		else
			memset(&wqe->sqe_md, 0, sizeof *sqe_md);

		if (wqe->sqe.opcode >= SIW_NUM_OPCODES) {
			dprint(DBG_TX, "---\n");
			rv = -EINVAL;
			goto out;
		}

		if (wqe->sqe.flags & SIW_WQE_INLINE) {
			if (wqe->sqe.opcode != SIW_OP_SEND && 
			    wqe->sqe.opcode != SIW_OP_WRITE &&
				wqe->sqe.opcode != SIW_OP_COMP_AND_SWAP_RESPONSE) {
				rv = -EINVAL;
				dprint(DBG_TX, "---\n");
				goto out;
			}
			if (wqe->sqe.sge[0].length > SIW_MAX_INLINE) {
				rv = -EINVAL;
				dprint(DBG_TX, "---\n");
				goto out;
			}
			wqe->sqe.sge[0].laddr = (u64)&wqe->sqe.sge[1];
			wqe->sqe.sge[0].lkey = 0;
			wqe->sqe.num_sge = 1;
		}
		
		//omril:
		//WQE is the current tx wqe,
		//if WQE marked to be fenced or;
		//if WQE's op requires orq item and there isn't any
		//Mark it (i.e. the current tx wqe) as fenced
		//Should resume once next read or atomic is done...
		//but what if there is more than one read/atomic in progress?
		//it will resume and allow it (e.g. a write) to be done before
		//other reads that were in progress when write arrived had completed.
		//See siw_check_tx_fence(), if this fenced op is
		//Read/Atomic - we only put it on ORQ
		//Other ops   - we only resume tx if the ORQ is empty - note that after
		//				fence mode is on, no more sendq items can be processed,
		//				including Read/Atomic and thus ORQ cant grow while fence is ON.

		if (wqe->sqe.flags & SIW_WQE_READ_FENCE) {
			/* A READ cannot be fenced */
			dprint(DBG_TX, "---\n");
			if (unlikely(wqe->sqe.opcode == SIW_OP_READ ||
			    wqe->sqe.opcode == SIW_OP_READ_LOCAL_INV)) {
				pr_info("QP[%d]: cannot fence READ\n",
					QP_ID(qp));
				rv = -EINVAL;
				goto out;
			}

			//omril: Test fenced ATOMIC op
			lock_orq_rxsave(qp, flags);
			if (!siw_orq_empty(qp)) {
				qp->tx_ctx.orq_fence = SIW_ORQ_FENCE_WR;
				wqe->wr_status = SR_WR_WAIT_ORQ;
				rv = 0;
			}
			else {
				dprint(DBG_TX, "no uncompleted read/atomic ops\n");
				//omril:
				//There are no uncompleted read/atomic ops.
				//nothing to fence from!
			}
			unlock_orq_rxsave(qp, flags);

		} else if (wqe->sqe.opcode == SIW_OP_READ ||
				   wqe->sqe.opcode == SIW_OP_READ_LOCAL_INV ||
				   wqe->sqe.opcode == SIW_OP_COMP_AND_SWAP ||
				   wqe->sqe.opcode == SIW_OP_MASKED_COMP_AND_SWAP ||
				   (qp->tx_ctx.ack_signal_wr && wqe->sqe.opcode == SIW_OP_WRITE && (wqe->sqe.flags & SIW_WQE_SIGNALLED))) {
			struct siw_sqe	*rreq;
			dprint(DBG_TX, "---\n");

			wqe->sqe.num_sge = 1;

			lock_orq_rxsave(qp, flags);

			rreq = orq_get_free(qp);
			if (rreq) {
				/*
				 * Make an immediate copy in ORQ to be ready
				 * to process loopback READ reply
				 */
				siw_read_to_orq(rreq, &wqe->sqe);
				qp->orq_put++;

				dprint(DBG_OL, "(QP%d): TXTX: inc orq_put to %d (orq_get=%d), flags=%04x\n",
					QP_ID(qp), qp->orq_put, qp->orq_get, wqe->sqe.flags);
			} else {
				dprint(DBG_ATOMIC, "(QP%d): qp->orq_put %d, qp->orq_get %d\n",
					QP_ID(qp), qp->orq_put, qp->orq_get);

				qp->tx_ctx.orq_fence = SIW_ORQ_FENCE_FULL;
				wqe->wr_status = SR_WR_WAIT_ORQ;
				rv = 0;
			}
			unlock_orq_rxsave(qp, flags);
		}
		dprint(DBG_TX, "---\n");

		/* Clear SQE, can be re-used by application */
		smp_store_mb(sqe->flags, 0);
		qp->sq_get++;
		dprint(DBG_OL, "(QP%d): TXTX: inc sq_get to %d (sq_put=%d)\n",
			QP_ID(qp), qp->sq_get, qp->sq_put);
	} else {
		dprint(DBG_OL, "(QP%d): TXTX: SQ empty, size=%d, sq_put=%d, sq_get=%d\n",
			QP_ID(qp), qp->attrs.sq_size, qp->sq_put, qp->sq_get);
		memset(&wqe->sqe_md, 0, sizeof *sqe_md);
		rv = 0;
	}
	
out:
	if (unlikely(rv < 0)) {
		pr_warn("QP[%d]: error %d in activate_tx\n", QP_ID(qp), rv);
		wqe->wr_status = SR_WR_IDLE;
	}
	return rv;
}

void siw_cq_notify(struct siw_cq *cq, u32 flags, bool force)
{
	u32 cq_notify;

	if (unlikely(!cq->ofa_cq.comp_handler))
		return;

	cq_notify = _load_shared(*cq->notify);

	if (force || (cq_notify & SIW_NOTIFY_NEXT_COMPLETION) ||
	    ((cq_notify & SIW_NOTIFY_SOLICITED) &&
	     (flags & SIW_WQE_SOLICITED))) {
		unsigned long handler_start_jif = jiffies, handler_jif;
		smp_store_mb(*cq->notify, SIW_NOTIFY_NOT);
		(*cq->ofa_cq.comp_handler)(&cq->ofa_cq, cq->ofa_cq.cq_context);
		handler_jif = jiffies - handler_start_jif;
		if (handler_jif > SIW_CQ_HANDLER_TIMEOUT_LOG) {
			dprint(DBG_ON, "(CQ%d): handler %pS (context " dprint_ptr_str() ") took %u ms\n", OBJ_ID(cq), cq->ofa_cq.comp_handler, cq->ofa_cq.cq_context, jiffies_to_msecs(handler_jif));
			SIW_TIMEOUT_WARN_KNOWN_THROTTLED(handler_jif, SIW_CQ_HANDLER_TIMEOUT_WARN, 9028);
		}
	}
	else {
		dprint(DBG_CQ, "(CQ%d:) - (" dprint_ptr_str() ") NOT calling cq->ofa_cq.comp_handler()... "
						"cq_notify=%x, flags=%x, force=%d\n",
			   OBJ_ID(cq), cq, cq_notify, flags, force);
	}
}

static void siw_cq_notify_if_not_empty(struct siw_cq *cq, bool force)
{
	unsigned long flags;
	struct siw_cqe *cqe;
	bool cq_not_empty;

	lock_cq_rxsave(cq, flags);
	cqe = &cq->queue[cq->cq_get % cq->num_cqe];
	cq_not_empty = cqe->flags & SIW_WQE_VALID;
	unlock_cq_rxsave(cq, flags);
	if (cq_not_empty) {
		siw_cq_notify(cq, 0, force);
	} else {
		dprint(DBG_CQ, "(CQ:%d): (" dprint_ptr_str() ") Empty!\n",
		       OBJ_ID(cq), cq);
	}
}

/* Work Fns for scheduling cq_notify from another context */
void siw_qp_notify_work(struct work_struct *work)
{
	struct siw_qp *qp = container_of(work, struct siw_qp, cq_notify_work);
	bool qp_in_err, qp_in_destroy;
	unsigned long jif = jiffies;

	if (jif > qp->cq_notify_sched_jif && ((jif - qp->cq_notify_sched_jif) > HZ / 5)) {
		dprint(DBG_CM | DBG_ON, "(QP%d/" dprint_ptr_str() "): siw_cq_notify_work delayed by %lu ms\n",
			QP_ID(qp), qp, ((1000UL * (jif - qp->cq_notify_sched_jif)) / HZ));
	}

	down_read(&qp->state_lock);
	qp_in_err = qp->attrs.state == SIW_QP_STATE_ERROR;
	qp_in_destroy = !!(qp->attrs.flags & SIW_QP_IN_DESTROY);
	up_read(&qp->state_lock);

	if (qp_in_destroy) {
		dprint(DBG_CQ | DBG_ON, "QP(%d) - in destroy\n", QP_ID(qp));
		goto qp_put;
	}

	if (qp->scq)
		siw_cq_notify_if_not_empty(qp->scq, true || qp_in_err);
	if (qp->rcq && qp->rcq != qp->scq) {
		siw_cq_notify_if_not_empty(qp->rcq, true || qp_in_err);
	}

qp_put:
	siw_qp_put(qp);
}


#if KS_HAS_TASKLET_SETUP
void siw_cq_notify_task(struct tasklet_struct *task)
{
	struct siw_cq *cq = from_tasklet(cq, task, notify_task);
#else
void siw_cq_notify_task(unsigned long data)
{
	struct siw_cq *cq = (void *)data;
#endif
	unsigned long sched_jif = _load_shared(cq->notify_sched_jif);
	smp_store_mb(cq->notify_sched_jif, 0);
	if (sched_jif) {
		unsigned long jif = jiffies;
		unsigned long delay_jif = jif - sched_jif;
		if (delay_jif > SIW_CQ_NOTIFY_TASK_DELAY_LOG) {
			dprint(DBG_ON, "CQ(%d/" dprint_ptr_str() "): siw_cq_notify_work on cpu: %d delayed by %u ms (jif: %lu sched_jif: %lu)\n",
			       OBJ_ID(cq), cq, smp_processor_id(), jiffies_to_msecs(delay_jif), jif, sched_jif);
			if (SIW_CQ_NOTIFY_TASK_DELAY_WARN)
				SIW_TIMEOUT_WARN_KNOWN_THROTTLED(delay_jif, SIW_CQ_NOTIFY_TASK_DELAY_WARN, 9028);
		}
	}
	if (!atomic_read(&cq->dying))
		siw_cq_notify_if_not_empty(cq, false);
}

atomic_t cq_notify_sched_cnt[NR_CPUS];

void siw_cq_notify_work(struct work_struct *work)
{
	struct siw_cq *cq = container_of(work, struct siw_cq, notify_work);
	unsigned long jif = jiffies;
	unsigned long delay_jif = (cq->notify_sched_jif && jif > cq->notify_sched_jif) ? jif - cq->notify_sched_jif : 0;

	atomic_dec(&cq_notify_sched_cnt[smp_processor_id()]);

	if (delay_jif > SIW_CQ_NOTIFY_WORK_DELAY_LOG) {
		dprint(DBG_ON, "CQ(%d/" dprint_ptr_str() "): siw_cq_notify_work on cpu: %d delayed by %u ms (jif: %lu sched_jif: %lu)\n",
		       OBJ_ID(cq), cq, smp_processor_id(), jiffies_to_msecs(delay_jif), jif, cq->notify_sched_jif);
		if (SIW_CQ_NOTIFY_WORK_DELAY_WARN)
			SIW_TIMEOUT_WARN_KNOWN_THROTTLED(delay_jif, SIW_CQ_NOTIFY_WORK_DELAY_WARN, 9028);
	}

	if (!atomic_read(&cq->dying))
		siw_cq_notify_if_not_empty(cq, false);
	siw_cq_put(cq);
}

/* Schedule cq notify work on notify wq according to comp_vector */
extern struct workqueue_struct *notify_wq;

extern bool cq_notify_tasklet;

bool siw_schedule_cq_notify_work(struct siw_qp *qp, struct siw_cq *cq)
{
#if SIW_CQ_NOTIFY_WORK_QP_INDEPENDENT
	if (!cq_notify_tasklet) {
		int cpu;
		bool queued;
		
		(void)qp;

		if (!cq)
			return false;
		if (atomic_read(&cq->dying))
			return false;
		siw_cq_get(cq);
		if (cq->hdr.sdev && cq->hdr.sdev->num_tx_vector > 0)
			cpu = cq->hdr.sdev->tx_vector_cpu[cq->comp_vector % cq->hdr.sdev->num_tx_vector];
		else
			cpu = cq->comp_vector % num_possible_cpus();
		atomic_inc(&cq_notify_sched_cnt[cpu]);
		if (cpu_online(cpu))
			queued = queue_work_on(cpu, notify_wq, &cq->notify_work);
		else
			queued = queue_work(notify_wq, &cq->notify_work);

		if (!queued) {
			atomic_dec(&cq_notify_sched_cnt[cpu]);
			dprint(DBG_CQ, "(CQ:%d): (" dprint_ptr_str() ") - Schedule notify work on CPU %d Failed\n",
			OBJ_ID(cq), cq, cq->comp_vector);
			siw_cq_put(cq);
			return false;
		}
		cq->notify_sched_jif = jiffies;
		dprint(DBG_CQ, "(CQ:%d): (" dprint_ptr_str() ") - Scheduled notify work on CPU %d\n",
		OBJ_ID(cq), cq, cq->comp_vector);
	} else {
		unsigned long flags;
		if (!cq)
			return false;
		if (atomic_read(&cq->dying))
			return false;
		local_irq_save(flags);
		if (!test_and_set_bit(TASKLET_STATE_SCHED, &cq->notify_task.state)) {
			smp_store_mb(cq->notify_sched_jif, jiffies);
			__tasklet_schedule(&cq->notify_task);
		}
		local_irq_restore(flags);
	}
#else
	bool queued;
	(void)cq;
	BUG_ON(cq != qp->scq && cq != qp->rcq);
	siw_qp_get(qp);
	if (notify_on_wq_same_core)
		queued = queue_work_on(smp_processor_id(), notify_wq, &qp->cq_notify_work);
	else
		queued = queue_work(notify_wq, &qp->cq_notify_work);
	if (!queued) {
		siw_qp_put(qp);
		return false;
	}
	qp->cq_notify_sched_jif = jiffies;
#endif
	return true;
}

extern struct workqueue_struct *flush_wq;

/* Complete SQ WR's without processing */
int siw_sq_flush_wr(struct siw_qp *qp, const struct ib_send_wr *wr,
			   const struct ib_send_wr **bad_wr, bool flush_imm)
{
	bool sched_flush = false;
	int rv = 0;

	while (wr) {
		struct siw_sqe sqe = {};

		switch (wr->opcode) {
			case IB_WR_RDMA_WRITE:
				sqe.opcode = SIW_OP_WRITE;
				break;
			case IB_WR_RDMA_READ:
				sqe.opcode = SIW_OP_READ;
				break;
			case IB_WR_RDMA_READ_WITH_INV:
				sqe.opcode = SIW_OP_READ_LOCAL_INV;
				break;
			case IB_WR_SEND:
				sqe.opcode = SIW_OP_SEND;
				break;
			case IB_WR_SEND_WITH_IMM:
				sqe.opcode = SIW_OP_SEND_WITH_IMM;
				break;
			case IB_WR_SEND_WITH_INV:
				sqe.opcode = SIW_OP_SEND_REMOTE_INV;
				break;
			case IB_WR_LOCAL_INV:
				sqe.opcode = SIW_OP_INVAL_STAG;
				break;
			case IB_WR_REG_MR:
				sqe.opcode = SIW_OP_REG_MR;
				break;
			default:
				rv = -EINVAL;
				break;
		}
		if (!rv) {
			sqe.id = wr->wr_id;
			if (flush_imm) {
				rv = siw_sqe_complete(qp, &sqe, NULL, 0,
						SIW_WC_WR_FLUSH_ERR, 0);
			} else {
				struct siw_xqe_flush *flush_sqe;
				unsigned long flags;

				if (!(flush_sqe = kzalloc(sizeof(*flush_sqe), GFP_ATOMIC))) {
					dprint(DBG_ON | DBG_WR, "(QP%d): OOM flushing SQE\n", QP_ID(qp));
					WARN_ON_ONCE(1);
					rv = -ENOMEM;
				} else {
					flush_sqe->id = sqe.id;
					flush_sqe->opcode = sqe.opcode;

					sched_flush = true;

					/* TBD: Remove DBG_ON */
					dprint(DBG_ON | DBG_WR, "(QP%d): Adding SQE with opcode: %d id: %llx "
					" to flush sqes list\n", QP_ID(qp), sqe.opcode, sqe.id); 

					lock_sq_rxsave(qp, flags);
					list_add_tail(&flush_sqe->link, &qp->tx_ctx.flush_sqes);
					unlock_sq_rxsave(qp, flags);
				}
			}
		}
		if (rv) {
			if (bad_wr)
				*bad_wr = wr;
			break;
		}
		wr = wr->next;
	}

	if (flush_imm) {
		if (qp->scq) {
			if (1 || qp->notify_on_wq)
				siw_schedule_cq_notify_work(qp, qp->scq);
			else
				siw_cq_notify(qp->scq, 0, true);
		}
	} else if (sched_flush) {
		siw_qp_get(qp);
		if (!queue_delayed_work(flush_wq, &qp->flush_work, SIW_FLUSH_XQES_WORK_DELAY)) {
			/* This is not an issue, it just means the work has already been scheduled, but hasn't yet run. 
			 * It will see the new SQEs appended to the list and flush them too. */
			dprint(DBG_ON | DBG_WR, "(QP%d): flush work already queued\n", QP_ID(qp));
			siw_qp_put(qp);
		}
	}
	return rv;
}

/* Complete RQ WR's without processing */
int siw_rq_flush_wr(struct siw_qp *qp, const struct ib_recv_wr *wr,
			   const struct ib_recv_wr **bad_wr, bool flush_imm)
{
	bool sched_flush = false;
	struct siw_rqe rqe = {};
	int rv = 0;

	while (wr) {
		rqe.id = wr->wr_id;
		if (flush_imm) {
			rv = siw_rqe_complete(qp, &rqe, NULL, 0, SIW_WC_WR_FLUSH_ERR, 0);
		} else {
			struct siw_xqe_flush *flush_rqe;
			unsigned long flags;

			if (!(flush_rqe = kzalloc(sizeof(*flush_rqe), GFP_ATOMIC))) {
				dprint(DBG_ON | DBG_WR, "(QP%d): OOM flushing RQE\n", QP_ID(qp));
				WARN_ON_ONCE(1);
				rv = -ENOMEM;
			} else {
				flush_rqe->id = rqe.id;
				flush_rqe->opcode = SIW_OP_RECEIVE;

				sched_flush = true;

				/* TBD: Remove DBG_ON */
				dprint(DBG_ON | DBG_WR, "(QP%d): Adding RQE with id: %llx "
				" to flush rqes list\n", QP_ID(qp), rqe.id); 

				lock_rq_rxsave(qp, flags);
				list_add_tail(&flush_rqe->link, &qp->rx_ctx.flush_rqes);
				unlock_rq_rxsave(qp, flags);
			}
		}
		if (rv) {
			if (bad_wr)
				*bad_wr = wr;
			break;
		}
		wr = wr->next;
	}
	if (flush_imm) {
		if (qp->rcq) {
			if (qp->notify_on_wq)
				siw_schedule_cq_notify_work(qp, qp->rcq);
			else
				siw_cq_notify(qp->rcq, 0, true);
		}
	} else if (sched_flush) {
		siw_qp_get(qp);
		if (!queue_delayed_work(flush_wq, &qp->flush_work, SIW_FLUSH_XQES_WORK_DELAY)) {
			/* This is not an issue, it just means the work has already been scheduled, but hasn't yet run. 
			 * It will see the new RQEs appended to the list and flush them too. */
			dprint(DBG_ON | DBG_WR, "(QP%d): flush work already queued\n", QP_ID(qp));
			siw_qp_put(qp);
		}
	}
	return rv;
}

int siw_sqe_complete(struct siw_qp *qp, struct siw_sqe *sqe, struct siw_sqe_md *sqe_md, u32 bytes,
		     enum siw_wc_status status, int notify)
{
	struct siw_cq *cq = qp->scq;
	struct siw_cqe *cqe;
	struct siw_cqe_md *cqe_md = NULL;
	unsigned long flags;
	u32 idx;
	int rv = 0;
	u32 cq_notify;

	dprint(DBG_OL, "(QP%d): TXTX: SQE complete -->\n",
	 QP_ID(qp));

	if (cq) {
		u32 sqe_flags = sqe->flags;

		lock_cq_rxsave(cq, flags);
		cq_notify = _load_shared(*cq->notify);

		idx = cq->cq_put % cq->num_cqe;
		cqe = &cq->queue[idx];
		if (cq->cqe_md)
			cqe_md = &cq->cqe_md[idx];

		dprint(DBG_OL, "(QP%d): TXTX: handle CQE (cq_put=%d)\n",
		 QP_ID(qp), cq->cq_put);

		if (!cqe->flags) {
			cqe->id = sqe->id;
			cqe->opcode = sqe->opcode;
			cqe->status = status;
			cqe->imm_data = 0;
			cqe->bytes = bytes;

			if (cqe_md) {
				if (sqe_md) {
					cqe_md->tx_md.post_send_time = sqe_md->post_send_time;
					cqe_md->tx_md.sent_time = sqe_md->sent_time;
					cqe_md->tx_md.ack_time = sqe_md->ack_time;
					cqe_md->tx_md.tx_cpu = sqe_md->tx_cpu;
				} else
					memset(cqe_md, 0, sizeof(*cqe_md));
			}

			/* If we are not notifying now and the WQE is signalled or status is error, tell the CQ to notify later */
			if (!notify && ((sqe_flags & SIW_WQE_SIGNALLED) || status != SIW_WC_SUCCESS)) {
				smp_store_mb(*cq->notify, (cq_notify | SIW_NOTIFY_NEXT_COMPLETION));
			}

			if (cq->kernel_verbs) {
				atomic_inc(&qp->tx_ctx.scq_qp_ref_cnt);
#if SIW_CQE_REFCOUNT_QP
				siw_qp_get(qp);
#endif
				cqe->qp = qp;
			} else
				cqe->qp_id = QP_ID(qp);

			smp_store_mb(cqe->flags, SIW_WQE_VALID | 
				(sqe->flags & (SIW_WQE_TX_TIMESTAMP | SIW_WQE_INLINE_SEND)));
			smp_store_mb(sqe->flags, (sqe->flags & ~SIW_WQE_VALID));

			cq->cq_put++;

			dprint(DBG_OL, "(QP%d): TXTX: inc cq_put to %d (cq_get=%d)\n",
				QP_ID(qp), cq->cq_put, cq->cq_get);

			unlock_cq_rxsave(cq, flags);

			dprint(DBG_CQ, "(QP%d): (CQ%d) - Post send CQE with opcode: %d, id: %llx, status %d\n",
				   QP_ID(qp), OBJ_ID(cq), cqe->opcode, cqe->id, cqe->status);

			if (notify) {
				dprint(DBG_OL, "(QP%d): TXTX: CQ notify\n",
				 QP_ID(qp));

				siw_cq_notify(cq, sqe_flags, false);
			}
		} else {
			dprint(DBG_OL, "(QP%d): TXTX: CQE flags=%08x\n",
				QP_ID(qp), cqe->flags);

			unlock_cq_rxsave(cq, flags);
			dprint(DBG_CQ, "(QP%d): (CQ%d) - CQE Full! cqe_put: %u cqe_get: %u num_cqe: %u\n",
				   QP_ID(qp), OBJ_ID(cq), cq->cq_put, cq->cq_get, cq->num_cqe);
			rv = -ENOMEM;
			siw_cq_event(cq, IB_EVENT_CQ_ERR);
		}
	} else
		smp_store_mb(sqe->flags, 0);

	dprint(DBG_OL, "(QP%d): TXTX: SQE complete -->\n",
	 QP_ID(qp));

	return rv;
}

int siw_rqe_complete(struct siw_qp *qp, struct siw_rqe *rqe, struct siw_rqe_md *rqe_md,
		     u32 bytes, enum siw_wc_status status, int notify)
{
	struct siw_cq *cq = qp->rcq;
	struct siw_cqe *cqe;
	struct siw_cqe_md *cqe_md = NULL;
	unsigned long flags;
	u32 idx;
	int rv = 0;
	u32 cq_notify;

	dprint(DBG_OL, "(QP%d): RQE complete -->\n",
	 QP_ID(qp));

	if (cq) {
		u32 rqe_flags = rqe->flags;

		lock_cq_rxsave(cq, flags);
		cq_notify = _load_shared(*cq->notify);

		idx = cq->cq_put % cq->num_cqe;
		cqe = &cq->queue[idx];

		if (!cqe->flags) {
			cqe->id = rqe->id;
			cqe->opcode = SIW_OP_RECEIVE;
			cqe->status = status;
			cqe->imm_data = 0;
			cqe->bytes = bytes;
			
			if (cqe_md) {
				if (rqe_md) {
					cqe_md->rx_md.first_ddp_recv_time = rqe_md->first_ddp_recv_time;
					cqe_md->rx_md.last_ddp_recv_time = rqe_md->last_ddp_recv_time;
					cqe_md->rx_md.rx_cpu = rqe_md->rx_cpu;
					cqe_md->rx_md.rx_queue = rqe_md->rx_queue;
					cqe_md->rx_md.rx_skb_hash = rqe_md->rx_skb_hash;
				} else
					memset(cqe_md, 0, sizeof(*cqe_md));
			}

			/* If we are not notifying now and the WQE is solicited or status is error, tell the CQ to notify later */
			if (!notify && (((cq_notify & SIW_NOTIFY_SOLICITED) && (rqe_flags & SIW_WQE_SOLICITED)) || status != SIW_WC_SUCCESS)) {
				smp_store_mb(*cq->notify, (cq_notify | SIW_NOTIFY_NEXT_COMPLETION));
			}

			if (cq->kernel_verbs) {
				atomic_inc(&qp->rx_ctx.rcq_qp_ref_cnt);
#if SIW_CQE_REFCOUNT_QP
				siw_qp_get(qp);
#endif
				cqe->qp = qp;
			} else
				cqe->qp_id = QP_ID(qp);
			
#if SIW_SRQ_RQE_TRACK
			if (cq->kernel_verbs) {
				cqe->srq_id = rqe->srq_id;
			}
#endif

			smp_store_mb(cqe->flags, SIW_WQE_VALID);
			smp_store_mb(rqe->flags, 0);

			cq->cq_put++;
			//dprint(DBG_OL, "(QP%d): TXTX: inc cq_put to %d (cq_get=%d)\n",
			//	QP_ID(qp), cq->cq_put, cq->cq_gett);
			unlock_cq_rxsave(cq, flags);

#if SIW_SRQ_RQE_TRACK
			if (qp->srq) {
				struct siw_srq *srq = qp->srq;

				BUG_ON(rqe->srq_id != srq->hdr.id);

				lock_srq_rxsave(srq, flags);
				srq->rqe_track.n_recv--;
				srq->rqe_track.n_cq++;
				BUG_ON(list_empty(&qp->srq_rqe_link) || qp->srq_rqe_state != SRQ_RQE_RECV);
				list_del_init(&qp->srq_rqe_link);
				qp->srq_rqe_state = SRQ_RQE_IDLE;
				unlock_srq_rxsave(srq, flags);
			}
#endif

			dprint(DBG_CQ, "(QP%d): (CQ%d) - Post recv CQE with id: %llx, status %d\n",
				   QP_ID(qp), OBJ_ID(cq), cqe->id, cqe->status);
			if (notify)
				siw_cq_notify(cq, rqe_flags, false);
		} else {
			unlock_cq_rxsave(cq, flags);
			dprint(DBG_CQ, "(QP%d): (CQ%d) - CQE Full! cqe_put: %u cqe_get: %u num_cqe: %u\n",
				   QP_ID(qp), OBJ_ID(cq), cq->cq_put, cq->cq_get, cq->num_cqe);
			rv = -ENOMEM;
			siw_cq_event(cq, IB_EVENT_CQ_ERR);
		}
	} else
		smp_store_mb(rqe->flags, 0);

	dprint(DBG_OL, "(QP%d): RQE complete <--\n",
	 QP_ID(qp));

	return rv;
}

/*
 * siw_sq_flush()
 *
 * Flush SQ and ORRQ entries to CQ.
 * IRRQ entries are silently dropped.
 *
 * TODO: Add termination code for in-progress WQE.
 * TODO: an in-progress WQE may have been partially
 *       processed. It should be enforced, that transmission
 *       of a started DDP segment must be completed if possible
 *       by any chance.
 *
 * Must be called with qp state write lock held.
 * Therefore, SQ and ORQ lock must not be taken.
 */
void siw_sq_flush(struct siw_qp *qp)
{
	struct siw_sqe	*sqe;
	struct siw_wqe	*wqe = tx_wqe(qp);
	unsigned long	flags;
	int		async_event = 0;
	int notify = 0;
	unsigned long pre_jif, post_jif;

	dprint(DBG_OBJ|DBG_CM|DBG_WR|DBG_ON, "(QP%d): Enter\n", QP_ID(qp));

	lock_sq_rxsave(qp, flags);
	BUG_ON(!qp->tx_ctx.tx_suspend);
	if (qp->tx_ctx.sq_flushed) {
		unlock_sq_rxsave(qp, flags);
		dprint(DBG_OBJ|DBG_CM|DBG_WR|DBG_ON, "(QP%d): SQ already flushed\n", QP_ID(qp));
		return;
	}
	unlock_sq_rxsave(qp, flags);

	/*
	 * Start with completing any work currently on the ORQ
	 */
	lock_orq_rxsave(qp, flags);
	dprint(DBG_OBJ|DBG_CM|DBG_WR|DBG_ON, "(QP%d): Orq lock has been taken\n", QP_ID(qp));

	while (qp->attrs.orq_size) {
		sqe = &qp->orq[qp->orq_get % qp->attrs.orq_size];
		if (!(smp_load_acquire(&sqe->flags) & SIW_WQE_VALID))
			break;

		dprint(DBG_OL|DBG_ON, "(QP%d): TXTX: call siw_sqe_complete\n",
			QP_ID(qp));

		if (siw_sqe_complete(qp, sqe, NULL, 0,
				     SIW_WC_WR_FLUSH_ERR, 0) != 0)
			break;
		notify++;

		qp->orq_get++;
		dprint(DBG_OL|DBG_ON, "(QP%d): TXTX: inc orq_get to %d (orq_put=%d)\n",
			QP_ID(qp), qp->orq_get, qp->orq_put);
	}
	unlock_orq_rxsave(qp, flags);
	dprint(DBG_OBJ|DBG_CM|DBG_WR|DBG_ON, "(QP%d): Org lock has been release\n", QP_ID(qp));

#ifdef SIW_TX_COMP_WAIT_ACK
	/* Flush the SQ WQEs waiting for completion */
	notify += siw_qp_sq_flush_sent_fpdus(qp);
#endif

	/*
	 * Flush the in-progress wqe, if there.
	 */
	if (wqe->wr_status != SR_WR_IDLE) {
		/*
		 * TODO: Add iWARP Termination code
		 */
		dprint(DBG_WR|DBG_ON,
			" (QP%d): Flush current WQE " dprint_ptr_str() ", type %d, status %d\n",
			QP_ID(qp), wqe, tx_type(wqe), wqe->wr_status);

		//omril: BUG?
		//Dont we only know that we've got kref to mem
		//if (wqe->wr_status == SR_WR_INPROGRESS && !(wqe->sqe.flags & SIW_WQE_INLINE)) ???
		siw_wqe_put_mem(wqe, wqe->sqe.opcode); //omril: see switch-case inside

		//omril: ??? Not sure I get this condition...
		if (wqe->sqe.opcode != SIW_OP_READ_RESPONSE &&
			wqe->sqe.opcode != SIW_OP_COMP_AND_SWAP_RESPONSE &&
			((wqe->sqe.opcode != SIW_OP_READ &&
			  wqe->sqe.opcode != SIW_OP_READ_LOCAL_INV &&
			  wqe->sqe.opcode != SIW_OP_COMP_AND_SWAP &&
			  wqe->sqe.opcode != SIW_OP_MASKED_COMP_AND_SWAP &&
			  wqe->sqe.opcode != SIW_OP_WRITE_RESPONSE) ||
			wqe->wr_status == SR_WR_QUEUED)) {
			/*
			 * An in-progress RREQUEST is already in
			 * the ORQ
			 */
			dprint(DBG_OL|DBG_ON, "(QP%d): TXTX: call siw_sqe_complete\n",
				QP_ID(qp));

			siw_sqe_complete(qp, &wqe->sqe, NULL, wqe->bytes,
					 SIW_WC_WR_FLUSH_ERR, 0);
			notify++;
		}

		wqe->wr_status = SR_WR_IDLE;
	}
	/*
	 * Flush the Send Queue
	 */
	while (qp->attrs.sq_size) {
		sqe = &qp->sendq[qp->sq_get % qp->attrs.sq_size];
		if (!(smp_load_acquire(&sqe->flags) & SIW_WQE_VALID))
			break;

		async_event = 1;
		dprint(DBG_OL|DBG_ON, "(QP%d): TXTX: call siw_sqe_complete\n",
			QP_ID(qp));


		if (siw_sqe_complete(qp, sqe, NULL, 0, SIW_WC_WR_FLUSH_ERR, 0) != 0)
			/* Shall IB_EVENT_SQ_DRAINED be supressed ? */
			break;
		notify++;

		sqe->flags = 0;
		qp->sq_get++;
		dprint(DBG_OL|DBG_ON, "(QP%d): TXTX: inc sq_get to %d (sq_put=%d)\n",
			QP_ID(qp), qp->sq_get, qp->sq_put);
	}

	lock_sq_rxsave(qp, flags);
	BUG_ON(qp->tx_ctx.sq_flushed);
	qp->tx_ctx.sq_flushed = 1;
	unlock_sq_rxsave(qp, flags);
	dprint(DBG_OBJ|DBG_CM|DBG_WR|DBG_ON, "(QP%d): SQ flushed\n", QP_ID(qp));

	/* Notify CQ if we have completed at least one WQE */
	if (notify && qp->scq) {
		if (qp->notify_on_wq)
			siw_schedule_cq_notify_work(qp, qp->scq);
		else {
			pre_jif = jiffies;
			siw_cq_notify(qp->scq, 0, true);
			post_jif = jiffies;
			if (post_jif - pre_jif > HZ / 5) {
				dprint(DBG_CQ | DBG_ON, "(QP%d): siw_qp=" dprint_ptr_str() 
					", siw_cq_notify took %lu ms\n",
					QP_ID(qp), qp, (1000UL * (post_jif - pre_jif)) / HZ);
			}
		}
	}
		
	if (async_event)
		siw_qp_event(qp, IB_EVENT_SQ_DRAINED);
	
	dprint(DBG_OBJ|DBG_CM|DBG_WR|DBG_ON, "(QP%d): Exit\n", QP_ID(qp));
}

/*
 * siw_rq_flush()
 *
 * Flush recv queue entries to cq. An in-progress WQE may have some bytes
 * processed (wqe->processed).
 *
 * Must be called with qp state write lock held.
 * Therefore, RQ lock must not be taken.
 */
void siw_rq_flush(struct siw_qp *qp)
{
	struct siw_wqe		*wqe = rx_wqe(qp);
	int notify = 0;

	dprint(DBG_OBJ|DBG_CM|DBG_WR|DBG_ON, "(QP%d): Enter\n", QP_ID(qp));
	//dump_stack();
#if SIW_SRQ_WAIT_LIST	
	if (qp->srq) {
		unsigned long flags;

		/* Cancel the RX work. We are holding the write lock, so the work function should just exit */
		siw_rx_cancel_work(qp);

		lock_srq_rxsave(qp->srq, flags);
		if (qp->srq_rqe_ready.flags == SIW_WQE_VALID) {
			/* qp holds a reserved rqe, let other waiting QP use it or return it to SRQ */
			int rv;
			if (!list_empty(&qp->srq_wait_link)) {
				pr_err("(QP%d): In SRQ waiting list and has reserved RQE", QP_ID(qp));
				BUG();
			}
#if SIW_SRQ_RQE_TRACK
			BUG_ON(list_empty(&qp->srq_rqe_link) || qp->srq_rqe_state != SRQ_RQE_RSVD);
			list_del_init(&qp->srq_rqe_link);
			qp->srq_rqe_state = SRQ_RQE_IDLE;
			qp->srq->rqe_track.n_rsvd--;
#endif
			
			/* [Jared]: Thinking to just call siw_rqe_complete with SIW_WC_WR_FLUSH_ERR on this instead and
			 * let the ULP return it. Much simpler! */
			dprint(DBG_ON | DBG_RX | DBG_ON, "(QP%d): Returning reserved RQE %llx to SRQ\n", 
			       QP_ID(qp), qp->srq_rqe_ready.id);
			rv = siw_return_reserved_rqe(qp->srq, &qp->srq_rqe_ready, true);
			if (!rv) {
				qp->srq_rqe_ready.flags = 0;
			}
			else
				BUG();
		} else {
			if (!list_empty(&qp->srq_wait_link))
				list_del_init(&qp->srq_wait_link);
		}
		unlock_srq_rxsave(qp->srq, flags);
	}
#endif

	/*
	 * Flush an in-progess WQE if present
	 */
	if (wqe->wr_status != SR_WR_IDLE) {
		dprint(DBG_ON|DBG_CM|DBG_WR, "(QP%d): Flushing recv wqe opcode: %x \n", 
			   QP_ID(qp), __rdmap_opcode(&qp->rx_ctx.hdr.ctrl));
		switch (__rdmap_opcode(&qp->rx_ctx.hdr.ctrl)) {
		case RDMAP_SEND:
		case RDMAP_SEND_INVAL:
		case RDMAP_SEND_SE:
		case RDMAP_SEND_SE_INVAL:
			BUG_ON((wqe->rqe.flags & SIW_WQE_VALID));
			siw_wqe_put_mem(wqe, SIW_OP_RECEIVE);
			siw_rqe_complete(qp, &wqe->rqe, NULL, wqe->bytes,
					 SIW_WC_WR_FLUSH_ERR, 0);
			notify++;
			break;
		case RDMAP_RDMA_READ_RESP:
#if SIW_ATOMIC_CAP
		case RDMAP_RDMA_ATOMIC_RESP:
#endif
		case RDMAP_RDMA_WRITE:
			/* Read and Atomic SQ WQEs are flushed from the ORQ so we don't need to flush them from here also.
			 * Incoming Write does not notify at all 
			 * Just dec the mem ref count and that's all. */
			siw_mem_put(rx_mem(qp));
			break;
		default:
			/* Unexpected opcode */
			BUG();
		}

		wqe->wr_status = SR_WR_IDLE;
	}

	while (qp->recvq && qp->attrs.rq_size) {
		struct siw_rqe *rqe =
			&qp->recvq[qp->rq_get % qp->attrs.rq_size];

		if (!rqe->flags)
			break;

		if (siw_rqe_complete(qp, rqe, NULL, 0, SIW_WC_WR_FLUSH_ERR, 0) != 0)
			break;
		notify++;
		rqe->flags = 0;

		qp->rq_get++;
	}
	if (notify && qp->rcq) {
		if (qp->notify_on_wq)
			siw_schedule_cq_notify_work(qp, qp->rcq);
		else
			siw_cq_notify(qp->rcq, 0, true);
	}

	dprint(DBG_ON|DBG_OBJ|DBG_CM|DBG_WR, "(QP%d): Exit\n", QP_ID(qp));
}

void siw_flush_xqes_work(struct work_struct *work)
{
	struct siw_qp *qp = container_of(work, struct siw_qp, flush_work.work);
	struct siw_xqe_flush *flush_xqe;
	bool resched = false;
	bool notify_scq = false;
	bool notify_rcq = false;
	unsigned long flags;
	int rv;

	/* Acquire read-lock to ensure QP has finished moving to error state */
	down_read(&qp->state_lock);

	if (qp->attrs.flags == SIW_QP_IN_DESTROY) {
		dprint(DBG_ON | DBG_WR, "(QP%d): QP is being destroyed\n", QP_ID(qp));
		goto unlock;
	}

	if (qp->attrs.state != SIW_QP_STATE_ERROR &&
		qp->attrs.state != SIW_QP_STATE_CLOSING)
	{
		dprint(DBG_ON, "(QP%d): QP in invalid state %d\n", QP_ID(qp), qp->attrs.state);
		WARN_ON_ONCE(1);
		goto unlock;
	}

	/* Pop sqes to be flushed one at a time out of the list and complete them to the scq
	 * We do this instead of list_splice because the scq may get full */

	lock_sq_rxsave(qp, flags);
	while ((flush_xqe = list_first_entry_or_null(
		&qp->tx_ctx.flush_sqes, struct siw_xqe_flush, link))) 
	{
		struct siw_sqe sqe = {
			.id = flush_xqe->id,
			.opcode = flush_xqe->opcode,
		};

		list_del(&flush_xqe->link);
		unlock_sq_rxsave(qp, flags);

		/* TBD: Remove DBG_ON */
		dprint(DBG_ON | DBG_WR, "(QP%d): Flushing SQE with opcode: %d id: %llx\n", QP_ID(qp), sqe.opcode, sqe.id);

		if ((rv = siw_sqe_complete(
			qp, &sqe, NULL, 0, SIW_WC_WR_FLUSH_ERR, 0)) < 0) 
		{
			BUG_ON(rv != -ENOMEM);
			dprint(DBG_ON | DBG_CQ, "(QP%d): CQ is full. Rescheduling\n", QP_ID(qp));
			resched = true;

			/* Return the SQE to the head of the list */
			lock_sq_rxsave(qp, flags);
			list_add(&flush_xqe->link, &qp->tx_ctx.flush_sqes);
			unlock_sq_rxsave(qp, flags);

			if (qp->scq == qp->rcq)
				goto notify;
			else
				goto flush_rx;
			break;
		}
		
		kfree(flush_xqe);

		notify_scq = true;

		lock_sq_rxsave(qp, flags);
	}
	unlock_sq_rxsave(qp, flags);

flush_rx:
	/* Ditto for rqes */
	lock_rq_rxsave(qp, flags);
	while ((flush_xqe = list_first_entry_or_null(
		&qp->rx_ctx.flush_rqes, struct siw_xqe_flush, link))) 
	{
		struct siw_rqe rqe = {
			.id = flush_xqe->id,
		};

		list_del(&flush_xqe->link);
		unlock_rq_rxsave(qp, flags);

		/* TBD: Remove DBG_ON */
		dprint(DBG_ON | DBG_WR, "(QP%d): Flushing RQE with id: %llx\n", QP_ID(qp), rqe.id);

		if ((rv = siw_rqe_complete(
			qp, &rqe, NULL, 0, SIW_WC_WR_FLUSH_ERR, 0)) < 0)
		{
			BUG_ON(rv != -ENOMEM);
			dprint(DBG_ON | DBG_CQ, "(QP%d): CQ is full. Rescheduling\n", QP_ID(qp));

			/* Return the RQE to the head of the list */
			lock_rq_rxsave(qp, flags);
			list_add(&flush_xqe->link, &qp->rx_ctx.flush_rqes);
			unlock_rq_rxsave(qp, flags);

			resched = true;
			goto notify;
		}

		kfree(flush_xqe);

		notify_rcq = true;

		lock_rq_rxsave(qp, flags);
	}
	unlock_rq_rxsave(qp, flags);

notify:
	/* Notify the CQs if we've completed anything */
	if (notify_scq) {
		if (qp->notify_on_wq)
			siw_schedule_cq_notify_work(qp, qp->scq);
		else
			siw_cq_notify(qp->scq, 0, true);
	}

	if (notify_rcq && qp->rcq != qp->scq) {
		if (qp->notify_on_wq)
			siw_schedule_cq_notify_work(qp, qp->rcq);
		else
			siw_cq_notify(qp->rcq, 0, true);
	}

unlock:
	up_read(&qp->state_lock);

	if (resched) {
		if (!queue_delayed_work(flush_wq, &qp->flush_work, SIW_FLUSH_XQES_WORK_DELAY)) {
			/* This is not an issue, it just means the work has already been scheduled by another context. */
			dprint(DBG_ON | DBG_WR, "(QP%d): flush work already queued\n", QP_ID(qp));
			goto qp_put;
		}
		return;
	}

qp_put:
	siw_qp_put(qp);
}
