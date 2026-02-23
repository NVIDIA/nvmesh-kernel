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
#include <linux/scatterlist.h>
#include <linux/highmem.h>
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

static bool panic_on_rx_err = false;
module_param(panic_on_rx_err, bool, 0644);
MODULE_PARM_DESC(panic_on_rx_err, "Panic on RX Error (bool).");

static unsigned int wait_rqe_delay_ms = 10;
module_param(wait_rqe_delay_ms, int, 0644);
MODULE_PARM_DESC(wait_rqe_delay_ms, "Delay to wait on empty S(RQ) (in ms) (int).");

static unsigned int wait_rqe_max_retries = 10;
module_param(wait_rqe_max_retries, int, 0644);
MODULE_PARM_DESC(wait_rqe_max_retries, "Number of retries on empty S(RQ) (int).");

extern struct workqueue_struct *siw_rx_wq;

#ifdef SIW_DEBUG_RX_CRC
#define siw_debug_rx_crc(rctx, crc_buf, crc_len)\
			do {\
				u32 crc_val;\
				typeof(crc_buf) p_crc_buf = crc_buf;\
				typeof(crc_len) p_crc_len = crc_len;\
				crypto_shash_final(rctx->mpa_crc_hd, (u8 *)&crc_val);\
				while(p_crc_len > 0) {\
					int p_crc_iter = min_t(int, p_crc_len, 64);\
					int crc_trace_sz = scnprintf(rctx->crc_trace_log + rctx->crc_trace_prod, \
							SIW_DEBUG_RX_CRC_TRACE_LOG_SZ - rctx->crc_trace_prod,\
							"[%d] SIW_CRC: crc_val %x - crc bytes: %d @ " dprint_ptr_str() " (FPDU [%ld,%ld]) %*phN\n",\
							rctx->crc_trace_line_prod++, crc_val, p_crc_iter, p_crc_buf, rctx->curr_fpdu_crc_bytes + (p_crc_buf - crc_buf),\
							rctx->curr_fpdu_crc_bytes + (p_crc_buf - crc_buf) + p_crc_iter, p_crc_iter, p_crc_buf);\
					if (unlikely(crc_trace_sz == 0)) {\
						rctx->crc_trace_prod = 0;\
						continue;\
					}\
					rctx->crc_trace_prod += crc_trace_sz;\
					p_crc_buf += p_crc_iter;\
					p_crc_len -= p_crc_iter;\
				}\
				rctx->curr_fpdu_crc_bytes += crc_len;\
			} while(0)
#else
#define siw_debug_rx_crc(rctx, crc_len, crc_buf)
#endif


/*
 * ----------------------------
 * DDP reassembly for Softiwarp
 * ----------------------------
 * For the ordering of transmitted DDP segments, the relevant iWARP ordering
 * rules are as follows:
 *
 * - RDMAP (RFC 5040): Section 7.5, Rule 17:
 *   "RDMA Read Response Message processing at the Remote Peer (reading
 *    the specified Tagged Buffer) MUST be started only after the RDMA
 *    Read Request Message has been Delivered by the DDP layer (thus,
 *    all previous RDMA Messages have been properly submitted for
 *    ordered Placement)."
 *
 * - DDP (RFC 5041): Section 5.3:
 *   "At the Data Source, DDP:
 *    o MUST transmit DDP Messages in the order they were submitted to
 *      the DDP layer,
 *    o SHOULD transmit DDP Segments within a DDP Message in increasing
 *      MO order for Untagged DDP Messages, and in increasing TO order
 *      for Tagged DDP Messages."
 *
 * Combining these rules implies that, although RDMAP does not provide
 * ordering between operations that are generated from the two ends of an
 * RDMAP stream, DDP *must not* transmit an RDMA Read Response Message before
 * it has finished transmitting SQ operations that were already submitted
 * to the DDP layer. It follows that an iWARP transmitter must fully
 * serialize RDMAP messages belonging to the same QP.
 *
 * Given that a TCP socket receives DDP segments in peer transmit order,
 * we obtain the following ordering of received DDP segments:
 *
 * (i)  the received DDP segments of RDMAP messages for the same QP
 *      cannot be interleaved
 * (ii) the received DDP segments of a single RDMAP message *should*
 *      arrive in order.
 *
 * The Softiwarp transmitter obeys rule #2 in DDP Section 5.3.
 * With this property, the "should" becomes a "must" in (ii) above,
 * which simplifies DDP reassembly considerably.
 * The Softiwarp receiver currently relies on this property
 * and reports an error if DDP segments of the same RDMAP message
 * do not arrive in sequence.
 */

static inline int siw_crc_rxhdr(struct siw_iwarp_rx *ctx)
{
	int rv;
	crypto_shash_init(ctx->mpa_crc_hd);

	rv = siw_crc_array(ctx->mpa_crc_hd, (u8 *)&ctx->hdr,
			     ctx->fpdu_part_rcvd);
	if (!rv) {
		siw_debug_rx_crc(ctx, (u8 *)&ctx->hdr, ctx->fpdu_part_rcvd);
	}
	return rv;
}

/*
 * siw_rx_umem()
 *
 * Receive data of @len into target referenced by @rctx.
 * This function does not check if umem is within bounds requested by
 * @len and @t_off. @umem_ends indicates if routine should
 * not update chunk position pointers after the point it is
 * currently receiving
 *
 * @rctx:	Receive Context
 * @umem:	siw representation of target memory
 * @dest_addr:	1, if rctx chunk pointer should not be updated after len.
 */
static int siw_rx_umem(struct siw_iwarp_rx *rctx, struct siw_umem *umem,
		       u64 dest_addr, int len)
{
	void	*dest;
	int	pg_off = dest_addr & ~PAGE_MASK,
		copied = 0,
		bytes,
		rv;

	while (len) {
		struct page *p = siw_get_upage(umem, dest_addr);

		if (unlikely(!p)) {
			pr_warn("siw_rx_umem: QP[%d]: bogus addr: " dprint_ptr_str() ", " dprint_ptr_str() "\n",
				RX_QPID(rctx),
				(void *)dest_addr, (void *)umem->fp_addr);
			/* siw internal error */
			rctx->skb_copied += copied;
			rctx->skb_new -= copied;
			copied = -EFAULT;

			goto out;
		}

		bytes  = min(len, (int)PAGE_SIZE - pg_off);
		dest = kmap_atomic(p);

		rv = skb_copy_bits(rctx->skb, rctx->skb_offset, dest + pg_off,
				   bytes);

		dprint(DBG_RX, "(QP%d): Page " dprint_ptr_str() ", "
			"bytes=%u, rv=%d returned by skb_copy_bits()\n",
			RX_QPID(rctx), p, bytes, rv);

		if (likely(!rv)) {
			if (rctx->mpa_crc_hd) {
				/*
				 * Do CRC on original, not target buffer.
				 * Some user land applications may
				 * concurrently write the target buffer,
				 * which would yield a broken CRC.
				 * Walking the skb twice is very ineffcient.
				 * Folding the CRC into skb_copy_bits()
				 * would be much better, but is currently
				 * not supported.
				 */
				siw_crc_skb(rctx, bytes);
				siw_debug_rx_crc(rctx, dest, bytes);
			}

			rctx->skb_offset += bytes;
			copied += bytes;
			len -= bytes;
			dest_addr += bytes;
			pg_off = 0;
		}
		kunmap_atomic(dest);

		if (unlikely(rv)) {
			rctx->skb_copied += copied;
			rctx->skb_new -= copied;
			copied = -EFAULT;

			dprint(DBG_RX|DBG_ON, "(QP%d): failed with %d\n",
				RX_QPID(rctx), rv);

			goto out;
		}
	}
	/*
	 * store chunk position for resume
	 */
	rctx->skb_copied += copied;
	rctx->skb_new -= copied;
out:
	return copied;
}

static inline int siw_rx_kva(struct siw_iwarp_rx *rctx, void *kva, int len)
{
	int rv;

	dprint(DBG_RX, "(QP%d): receive %d bytes into " dprint_ptr_str() "\n", RX_QPID(rctx),
		len, kva);

	rv = skb_copy_bits(rctx->skb, rctx->skb_offset, kva, len);
	if (likely(!rv)) {
		if (rctx->mpa_crc_hd) {
			/*
			 * Do CRC on original, not target buffer.
			 * Other RDMA channels might
			 * concurrently write the target buffer,
			 * which would yield a broken CRC.
			 * Walking the skb twice is very ineffcient.
			 * Folding the CRC into skb_copy_bits()
			 * would be much better, but is currently
			 * not supported.
			 */
			siw_crc_skb(rctx, len);
			siw_debug_rx_crc(rctx, kva, len);
		}
		rctx->skb_offset += len;
		rctx->skb_copied += len;
		rctx->skb_new -= len;
		return len;
	}
	dprint(DBG_ON, "(QP%d): failed: len %d, addr " dprint_ptr_str() ", rv %d\n",
		RX_QPID(rctx), len, kva, rv);
	return rv;
}

static int siw_rx_pbl(struct siw_iwarp_rx *rctx, struct siw_mr *mr,
		      u64 addr, int len)
{
	struct siw_pbl *pbl = mr->pbl;
	u64 offset = addr - mr->mem.va;
	int copied = 0, idx = 0;

	while (len) {
		int bytes;
		u64 buf_addr;

		dprint(DBG_MM,
			"krping: lookup addr=%llx (mr->mem.va=%llx, offset=%llx)\n",
			addr, mr->mem.va, offset);

		buf_addr = siw_pbl_get_buffer(pbl, offset, &bytes, &idx);

		dprint(DBG_MM,
			"krping: lookup resolved to buf_addr=%llx)\n",
			buf_addr);

		if (buf_addr == 0)
			break;
		bytes = min(bytes, len);
		if (siw_rx_kva(rctx, (void *)buf_addr, bytes) == bytes) {
			copied += bytes;
			offset += bytes;
			len -= bytes;
		} else
			break;
	}
	return copied;
}

/*
 * siw_rresp_check_ntoh()
 *
 * Check incoming RRESP fragment header against expected
 * header values and update expected values for potential next
 * fragment.
 *
 * NOTE: This function must be called only if a RRESP DDP segment
 *       starts but not for fragmented consecutive pieces of an
 *       already started DDP segement.
 */
static inline int siw_rresp_check_ntoh(struct siw_iwarp_rx *rctx)
{
	struct iwarp_rdma_rresp	*rresp = &rctx->hdr.rresp;
	struct siw_wqe		*wqe = &rctx->wqe_active;
	u32 wqe_bytes = wqe->sqe.opcode == SIW_OP_READ ? wqe->bytes : 0;

	u32 sink_stag = be32_to_cpu(rresp->sink_stag);
	u64 sink_to   = be64_to_cpu(rresp->sink_to);

	if (rctx->first_ddp_seg) {
		if (wqe->sqe.opcode == SIW_OP_READ) {
			rctx->ddp_stag = wqe->sqe.sge[0].lkey;
			rctx->ddp_to   = wqe->sqe.sge[0].laddr;
		} else {
			BUG_ON(wqe->sqe.opcode != SIW_OP_WRITE);
			rctx->ddp_stag = wqe->sqe.rkey;
			rctx->ddp_to = wqe->sqe.raddr;
			dprint(DBG_RX, "QP(%d): WRESP(%llx) rkey: %x raddr: %llx\n",
			       RX_QPID(rctx), wqe->sqe.id, sink_stag, sink_to);
		}
	}
	if (rctx->ddp_stag != sink_stag) {
		dprint(DBG_RX|DBG_ON,
			" received STAG=%08x, expected STAG=%08x\n",
			sink_stag, rctx->ddp_stag);
		/*
		 * Verbs: RI_EVENT_QP_LLP_INTEGRITY_ERROR_BAD_FPDU
		 */
		return -EINVAL;
	}
	if (rctx->ddp_to != sink_to) {
		dprint(DBG_RX|DBG_ON,
			" received TO=%016llx, expected TO=%016llx\n",
			(unsigned long long)sink_to,
			(unsigned long long)rctx->ddp_to);
		/*
		 * Verbs: RI_EVENT_QP_LLP_INTEGRITY_ERROR_BAD_FPDU
		 */
		return -EINVAL;
	}
	if (!rctx->more_ddp_segs && (wqe->processed + rctx->fpdu_part_rem
		!= wqe_bytes)) {
		dprint(DBG_RX|DBG_ON,
			" RRESP length does not match RREQ, "
			"peer sent=%d, expected %d\n",
			wqe->processed + rctx->fpdu_part_rem, wqe_bytes);
		return -EINVAL;
	}
	return 0;
}

#if SIW_ATOMIC_CAP
//omril: not really needed, can be merged with the RRESP version
/*
 * siw_aresp_check_ntoh()
 *
 * Check incoming ARESP fragment header against expected
 * header values and update expected values for potential next
 * fragment.
 *
 */
static inline int siw_aresp_check_ntoh(struct siw_iwarp_rx *rctx)
{
	struct iwarp_rdma_aresp	*aresp = &rctx->hdr.aresp;
	struct siw_wqe		*wqe = &rctx->wqe_active;

	u32 sink_stag = be32_to_cpu(aresp->sink_stag);
	u64 sink_to   = be64_to_cpu(aresp->sink_to);

	if (rctx->first_ddp_seg) {
		rctx->ddp_stag = wqe->sqe.sge[0].lkey;
		rctx->ddp_to   = wqe->sqe.sge[0].laddr;
	}
	if (rctx->ddp_stag != sink_stag) {
		dprint(DBG_RX|DBG_ON,
			"(QP:%d) received STAG=%08x, expected STAG=%08x\n",
			RX_QPID(rctx), sink_stag, rctx->ddp_stag);
		/*
		 * Verbs: RI_EVENT_QP_LLP_INTEGRITY_ERROR_BAD_FPDU
		 */
		return -EINVAL;
	}
	if (rctx->ddp_to != sink_to) {
		dprint(DBG_RX|DBG_ON,
		       "(QP:%d) received TO=%016llx, expected TO=%016llx\n",
			RX_QPID(rctx),
			(unsigned long long)sink_to,
			(unsigned long long)rctx->ddp_to);
		/*
		 * Verbs: RI_EVENT_QP_LLP_INTEGRITY_ERROR_BAD_FPDU
		 */
		return -EINVAL;
	}

	//omril: optionally, send back cmp and swap values and check them too?

	if (!rctx->more_ddp_segs && (wqe->processed + rctx->fpdu_part_rem
				     != wqe->bytes)) {
		dprint(DBG_RX|DBG_ON,
			" ARESP length does not match AREQ, "
			"peer sent=%d, expected %d\n",
			wqe->processed + rctx->fpdu_part_rem, wqe->bytes);
		pr_err("ARESP error - rctx " dprint_ptr_str() " wqe " dprint_ptr_str() "\n", rctx, wqe);
		BUG_ON(1);

		//omril: DEBUG - skip this for now!
		//return -EINVAL;
	}
	return 0;
}

#else

static inline int siw_aresp_check_ntoh(struct siw_iwarp_rx *rctx)
{
	return -ENOTSUPP;
}

#endif

/*
 * siw_write_check_ntoh()
 *
 * Check incoming WRITE fragment header against expected
 * header values and update expected values for potential next
 * fragment
 *
 * NOTE: This function must be called only if a WRITE DDP segment
 *       starts but not for fragmented consecutive pieces of an
 *       already started DDP segement.
 */
static inline int siw_write_check_ntoh(struct siw_iwarp_rx *rctx)
{
	struct iwarp_rdma_write	*write = &rctx->hdr.rwrite;

	u32 sink_stag = be32_to_cpu(write->sink_stag);
	u64 sink_to   = be64_to_cpu(write->sink_to);

	if (rctx->first_ddp_seg) {
		rctx->ddp_stag = sink_stag;
		rctx->ddp_to   = sink_to;
		rctx->first_ddp_to = sink_to;
	} else {
		if (rctx->ddp_stag != sink_stag) {
			dprint(DBG_RX|DBG_ON,
				" received STAG=%08x, expected STAG=%08x\n",
				sink_stag, rctx->ddp_stag);
			/*
			 * Verbs: RI_EVENT_QP_LLP_INTEGRITY_ERROR_BAD_FPDU
			 */
			return -EINVAL;
		}
		if (rctx->ddp_to != sink_to) {
			dprint(DBG_RX|DBG_ON,
				" received TO=%016llx, expected TO=%016llx\n",
				(unsigned long long)sink_to,
				(unsigned long long)rctx->ddp_to);
			/*
			 * Verbs: RI_EVENT_QP_LLP_INTEGRITY_ERROR_BAD_FPDU
			 */
			return -EINVAL;
		}
	}
	return 0;
}

/*
 * siw_send_check_ntoh()
 *
 * Check incoming SEND fragment header against expected
 * header values and update expected MSN if no next
 * fragment expected
 *
 * NOTE: This function must be called only if a SEND DDP segment
 *       starts but not for fragmented consecutive pieces of an
 *       already started DDP segement.
 */
static inline int siw_send_check_ntoh(struct siw_iwarp_rx *rctx)
{
	struct iwarp_send_inv	*send = &rctx->hdr.send_inv;
	struct siw_wqe		*wqe = &rctx->wqe_active;

	u32 ddp_msn = be32_to_cpu(send->ddp_msn);
	u32 ddp_mo  = be32_to_cpu(send->ddp_mo);
	u32 ddp_qn  = be32_to_cpu(send->ddp_qn);

	if (ddp_qn != RDMAP_UNTAGGED_QN_SEND) {
		dprint(DBG_RX|DBG_ON, " Invalid DDP QN %d for SEND\n",
			ddp_qn);
		return -EINVAL;
	}
	if (unlikely(ddp_msn != rctx->ddp_msn[RDMAP_UNTAGGED_QN_SEND])) {
		dprint(DBG_RX|DBG_ON, " received MSN=%u, expected MSN=%u\n",
			ddp_msn, rctx->ddp_msn[RDMAP_UNTAGGED_QN_SEND]);
		/*
		 * TODO: Error handling
		 * async_event= RI_EVENT_QP_RQ_PROTECTION_ERROR_MSN_GAP;
		 * cmpl_status= RI_WC_STATUS_LOCAL_QP_CATASTROPHIC;
		 */
		return -EINVAL;
	}
	if (unlikely(ddp_mo != wqe->processed)) {
		dprint(DBG_RX|DBG_ON, " Received MO=%u, expected MO=%u\n",
			ddp_mo, wqe->processed);
		/*
		 * Verbs: RI_EVENT_QP_LLP_INTEGRITY_ERROR_BAD_FPDU
		 */
		return -EINVAL;
	}
	if (rctx->first_ddp_seg) {
		/* initialize user memory write position */
		rctx->sge_idx = 0;
		rctx->sge_off = 0;
		/* only valid for SEND_INV and SEND_SE_INV operations */
		rctx->inval_stag = be32_to_cpu(send->inval_stag);
	}
	if (unlikely(wqe->bytes < wqe->processed + rctx->fpdu_part_rem)) {
		dprint(DBG_RX|DBG_ON, " Receive space short: (%d - %d) < %d\n",
			wqe->bytes, wqe->processed, rctx->fpdu_part_rem);
		wqe->wc_status = SIW_WC_LOC_LEN_ERR;
		return -EINVAL;
	}
	return 0;
}

static struct siw_wqe *siw_rqe_get(struct siw_qp *qp)
{
	struct siw_rqe *rqe;
	struct siw_srq *srq = qp->srq;
	struct siw_wqe *wqe = NULL;
	unsigned long	flags = 0;
	bool srq_event = false;
	bool rsvd_rqe = false;

	if (!srq)
		rqe = &qp->recvq[qp->rq_get % qp->attrs.rq_size];
	else {
		lock_srq_rxsave(srq, flags);
		/* qp holds a reserved rqe, dont need to pop one from SRQ */
		if (qp->srq_rqe_ready.flags == SIW_WQE_VALID) {
			rqe = &qp->srq_rqe_ready;
			BUG_ON(rqe->srq_id != srq->hdr.id);
			rsvd_rqe = true;
			dprint(DBG_ON | DBG_RX, "(QP%d): Using reserved RQE %llx from SRQ%d (" dprint_ptr_str() ")\n", 
			       QP_ID(qp), rqe->id, rqe->srq_id, srq);
		} else {
			u32 srq_idx = srq->rq_get % srq->num_rqe;
			rqe = &srq->recvq[srq_idx];

#ifdef SIW_TEST_RQE_RETRY
			if ((get_random_u32() & 0xff) == 0) {
				srq_event = true;
				goto out;
			}
#endif
		}
	}
	if (likely(rqe->flags == SIW_WQE_VALID)) {
		int num_sge = rqe->num_sge;
		if (likely(num_sge <= SIW_MAX_SGE)) {
			int i = 0;

			wqe = rx_wqe(qp);
			wqe->wr_status = SR_WR_INPROGRESS;
			wqe->bytes = 0;
			wqe->processed = 0;

			wqe->rqe.id = rqe->id;
			wqe->rqe.num_sge = num_sge;
			wqe->rqe.srq_id = rqe->srq_id;
			
#if SIW_SRQ_RQE_TRACK
			if (srq) {
				/* SRQ is already locked */
				BUG_ON(rqe->srq_id != srq->hdr.id);
				wqe->rqe.srq_id = rqe->srq_id;
				if (rsvd_rqe) {
					BUG_ON(list_empty(&qp->srq_rqe_link) || qp->srq_rqe_state != SRQ_RQE_RSVD);
					list_del_init(&qp->srq_rqe_link);
					srq->rqe_track.n_rsvd--;
				} else {
					srq->rqe_track.n_q--;
				}
				list_add_tail(&qp->srq_rqe_link, &srq->rqe_track.recv_qp_head);
				qp->srq_rqe_state = SRQ_RQE_RECV;
				srq->rqe_track.n_recv++;
			}
#endif

			while (i < num_sge) {
				wqe->rqe.sge[i].laddr = rqe->sge[i].laddr;
				wqe->rqe.sge[i].lkey = rqe->sge[i].lkey;
				wqe->rqe.sge[i].length = rqe->sge[i].length;
				wqe->bytes += wqe->rqe.sge[i].length;
				wqe->mem[i].obj = NULL;
				i++;
			}
			/* can be re-used by appl */
			smp_store_mb(rqe->flags, 0); /* qp->srq_rqe_ready.flags <-- 0 */
		} else {
			dprint(DBG_ON, "RQE: too many SGE's: %d\n", rqe->num_sge);
			goto out;
		}
		if (!srq)
			qp->rq_get++;
		else if (!rsvd_rqe) {
			if (srq->armed) {
				/* Test SRQ limit */
				u32 off = (srq->rq_get + srq->limit) %
					  srq->num_rqe;
				struct siw_rqe *rqe2 = &srq->recvq[off];

				if (!(rqe2->flags & SIW_WQE_VALID)) {
					srq->armed = 0;
					srq_event = true;
				}
			}
			/*trace_printk("qp " dprint_ptr_str() " got rqe[%d] from srq " dprint_ptr_str() " with wr_id %llx\n",
						 qp, srq->rq_get % srq->num_rqe, srq, rqe->id);*/
			srq->rq_get++;
		}
	}
	else {
		if (!srq) {
			dprint(DBG_ON, "(QP%d): QP " dprint_ptr_str() " - RQ empty, size=%d, rq_put=%d, rq_get=%d, flags=%08x\n",
				QP_ID(qp), qp, qp->attrs.rq_size, qp->rq_put, qp->sq_get, rqe->flags);
		}
		else {
			dprint(DBG_ON, "(QP%d): QP " dprint_ptr_str() " - SRQ empty, size=%d, rq_put=%d, rq_get=%d, flags=%08x\n",
				QP_ID(qp), qp, srq->num_rqe, srq->rq_put, srq->rq_get, rqe->flags);
#if SIW_SRQ_RQE_TRACK
			dprint(DBG_ON, "(SRQ%d/" dprint_ptr_str() "): n_q/max_q: %lu/%lu n_rsvd: %lu n_recv: %lu n_cqe: %lu n_ulp: %lu\n", 
			       srq->hdr.id, srq,
			       srq->rqe_track.n_q, srq->rqe_track.max_q, srq->rqe_track.n_rsvd, 
				srq->rqe_track.n_recv, srq->rqe_track.n_cq, srq->rqe_track.n_ulp);
			if (jiffies > srq->rqe_track.last_ret_jif + SIW_SRQ_RQE_TRACK_LAST_RET_TIMEOUT) {
				BUG();
			}
#endif
		}
		SIW_WARN_KNOWN_EC_ONCE(1, 7663);
	}

out:
	if (srq) {
		unlock_srq_rxsave(qp->srq, flags);
		if (srq_event)
			siw_srq_event(srq,
					  IB_EVENT_SRQ_LIMIT_REACHED, SIW_SRQ_EVENT_ON_WQ);
	}

	return wqe;
}

/*
 * siw_proc_send:
 *
 * Process one incoming SEND and place data into memory referenced by
 * receive wqe.
 *
 * Function supports partially received sends (suspending/resuming
 * current receive wqe processing)
 *
 * return value:
 *	0:       reached the end of a DDP segment
 *	-EAGAIN: to be called again to finish the DDP segment
 */
int siw_proc_send(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	struct siw_wqe		*wqe;
	struct siw_sge		*sge;
	u32			data_bytes,	/* all data bytes available */
				rcvd_bytes;	/* sum of data bytes rcvd */
	int rv = 0;

	if (rctx->first_ddp_seg) {
		wqe = siw_rqe_get(qp);
		if (unlikely(!wqe))
			return -ENOENT;
	} else  {
		wqe = rx_wqe(qp);
		if (unlikely(wqe->wr_status != SR_WR_INPROGRESS)) {
			/*
			 * this is a siw bug!
			 */
			dprint(DBG_ON, "QP(%d): RQ failure\n", QP_ID(qp));
			BUG();
			return -EPROTO;
		}
	}
	if (rctx->state == SIW_GET_DATA_START) {
		rv = siw_send_check_ntoh(rctx);
		if (unlikely(rv)) {
			siw_qp_event(qp, IB_EVENT_QP_FATAL);
			return rv;
		}
		if (!rctx->fpdu_part_rem) /* zero length SEND */
			return 0;
	}
	data_bytes = min(rctx->fpdu_part_rem, rctx->skb_new);
	rcvd_bytes = 0;

	/* A zero length SEND will skip below loop */
	while (data_bytes) {
		struct siw_pd *pd;
		struct siw_mr *mr;
		union siw_mem_resolved *mem;
		u32 sge_bytes;	/* data bytes avail for SGE */

		sge = &wqe->rqe.sge[rctx->sge_idx];

		if (!sge->length) {
			/* just skip empty sge's */
			rctx->sge_idx++;
			rctx->sge_off = 0;
			continue;
		}
		sge_bytes = min(data_bytes, sge->length - rctx->sge_off);
		mem = &wqe->mem[rctx->sge_idx];

		/*
		 * check with QP's PD if no SRQ present, SRQ's PD otherwise
		 */
		pd = qp->srq == NULL ? qp->pd : qp->srq->pd;

		dprint(DBG_OL, "calling siw_check_sge ...\n");

		rv = siw_check_sge(pd, sge, mem, SR_MEM_LWRITE, rctx->sge_off,
				   sge_bytes);
		if (unlikely(rv)) {
			siw_qp_event(qp, IB_EVENT_QP_ACCESS_ERR);
			break;
		}
		mr = siw_mem2mr(mem->obj);
		if (mr->mem_obj == NULL)
			rv = siw_rx_kva(rctx,
					(void *)(sge->laddr + rctx->sge_off),
					sge_bytes);
		else if (!mr->mem.is_pbl)
			rv = siw_rx_umem(rctx, mr->umem,
					 sge->laddr + rctx->sge_off, sge_bytes);
		else
			rv = siw_rx_pbl(rctx, mr,
					sge->laddr + rctx->sge_off, sge_bytes);

		if (unlikely(rv != sge_bytes)) {
			wqe->processed += rcvd_bytes;
			return -EINVAL;
		}
		rctx->sge_off += rv;

		if (rctx->sge_off == sge->length) {
			rctx->sge_idx++;
			rctx->sge_off = 0;
		}
		data_bytes -= rv;
		rcvd_bytes += rv;

		rctx->fpdu_part_rem -= rv;
		rctx->fpdu_part_rcvd += rv;
	}
	wqe->processed += rcvd_bytes;

	if (!rctx->fpdu_part_rem)
		return 0;

	return (rv < 0) ? rv : -EAGAIN;
}

/*
 * siw_proc_write:
 *
 * Place incoming WRITE after referencing and checking target buffer

 * Function supports partially received WRITEs (suspending/resuming
 * current receive processing)
 *
 * return value:
 *	0:       reached the end of a DDP segment
 *	-EAGAIN: to be called again to finish the DDP segment
 */

int siw_proc_write(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	struct siw_dev		*dev = qp->hdr.sdev;
	struct siw_mem		*mem;
	struct siw_mr		*mr;
	int			bytes,
				rv;

	if (rctx->state == SIW_GET_DATA_START) {

		if (!rctx->fpdu_part_rem) /* zero length WRITE */
			return 0;

		rv = siw_write_check_ntoh(rctx);
		if (unlikely(rv)) {
			siw_qp_event(qp, IB_EVENT_QP_FATAL);
			return rv;
		}
	}
	bytes = min(rctx->fpdu_part_rem, rctx->skb_new);

	if (rctx->first_ddp_seg) {
		/* DEBUG Code, to be removed */
		if (rx_mem(qp) != NULL) {
			dprint(DBG_RX|DBG_ON, "(QP%d): Stale rctx state!\n",
				QP_ID(qp));
			siw_print_rctx(rctx);
			BUG();
			return -EFAULT;
		}
		dprint(DBG_OL, "calling siw_mem_id2obj...\n");
		rx_mem(qp) = siw_mem_id2obj(dev, rctx->ddp_stag >> 8);
		rx_wqe(qp)->wr_status = SR_WR_INPROGRESS;
	}
	if (unlikely(!rx_mem(qp))) {
		dprint(DBG_RX|DBG_ON, "(QP%d): "
			"Sink STag not found or invalid,  STag=0x%08x\n",
			QP_ID(qp), rctx->ddp_stag);
		return -EINVAL;
	}
	mem = rx_mem(qp);
	/*
	 * Rtag not checked against mem's tag again because
	 * hdr check guarantees same tag as before if fragmented
	 */
	rv = siw_check_mem(qp->pd, mem, rctx->ddp_to + rctx->fpdu_part_rcvd,
			   SR_MEM_RWRITE, bytes);
	if (unlikely(rv)) {
		siw_qp_event(qp, IB_EVENT_QP_ACCESS_ERR);
		return rv;
	}

	mr = siw_mem2mr(mem);
	if (mr->mem_obj == NULL)
		rv = siw_rx_kva(rctx,
				(void *)(rctx->ddp_to + rctx->fpdu_part_rcvd),
				bytes);
	else if (!mr->mem.is_pbl)
		rv = siw_rx_umem(rctx, mr->umem,
				 rctx->ddp_to + rctx->fpdu_part_rcvd, bytes);
	else
		rv = siw_rx_pbl(rctx, mr,
				rctx->ddp_to + rctx->fpdu_part_rcvd, bytes);

	if (unlikely(rv != bytes))
		return -EINVAL;

	rctx->fpdu_part_rem -= rv;
	rctx->fpdu_part_rcvd += rv;

	if (!rctx->fpdu_part_rem) {
		rctx->ddp_to += rctx->fpdu_part_rcvd;
		return 0;
	}

	return (rv < 0) ? rv : -EAGAIN;
}

/*
 * inbound RREQ's cannot carry user data.
 */
int siw_proc_rreq(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	if (!rctx->fpdu_part_rem)
		return 0;

	dprint(DBG_ON|DBG_RX, "(QP%d): RREQ with MPA len %d\n", QP_ID(qp),
		be16_to_cpu(rctx->hdr.ctrl.mpa_len));

	return -EPROTO;
}

#if SIW_ATOMIC_CAP
static void siw_init_rresp(struct siw_sqe *resp, struct siw_iwarp_rx *rctx)
{
	resp->opcode = SIW_OP_READ_RESPONSE;

	resp->sge[0].length = be32_to_cpu(rctx->hdr.rreq.read_size);
	resp->sge[0].laddr = be64_to_cpu(rctx->hdr.rreq.source_to);
	resp->sge[0].lkey = be32_to_cpu(rctx->hdr.rreq.source_stag);

	resp->raddr = be64_to_cpu(rctx->hdr.rreq.sink_to);
	resp->rkey = be32_to_cpu(rctx->hdr.rreq.sink_stag);
	resp->num_sge = resp->sge[0].length ? 1 : 0;
}
static void siw_init_wresp(struct siw_sqe *resp, struct siw_iwarp_rx *rctx)
{
	resp->opcode = SIW_OP_WRITE_RESPONSE;
	resp->sge[0].length = 0;
	resp->sge[0].laddr = 0;
	resp->sge[0].lkey = 0;
	
#if 0
	resp->raddr = be64_to_cpu(rctx->hdr.rwrite.sink_to);
	resp->rkey = be32_to_cpu(rctx->hdr.rwrite.sink_stag);
#else
	resp->raddr = rctx->first_ddp_to;
	resp->rkey = rctx->ddp_stag;
#endif
	resp->num_sge = 0;
	
	dprint(DBG_TX, "(QP%d): WRESP with rkey: %x raddr: %llu\n",
	       RX_QPID(rctx), resp->rkey, resp->raddr);
}
#else
#if 0
/*
 * siw_init_rresp:
 *
 * Process inbound RDMA READ REQ. Produce a pseudo READ RESPONSE WQE.
 * Put it at the tail of the IRQ, if there is another WQE currently in
 * transmit processing. If not, make it the current WQE to be processed
 * and schedule transmit processing.
 *
 * Can be called from softirq context and from process
 * context (RREAD socket loopback case!)
 *
 * return value:
 *	0:      success,
 *		failure code otherwise
 */
static int siw_init_rresp(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	struct siw_wqe *tx_work = tx_wqe(qp);
	struct siw_sqe *resp;

	uint64_t	raddr	= be64_to_cpu(rctx->hdr.rreq.sink_to),
			laddr	= be64_to_cpu(rctx->hdr.rreq.source_to);
	uint32_t	length	= be32_to_cpu(rctx->hdr.rreq.read_size),
			lkey	= be32_to_cpu(rctx->hdr.rreq.source_stag),
			rkey	= be32_to_cpu(rctx->hdr.rreq.sink_stag);
	int run_sq = 1, rv = 0;

	lock_sq(qp);

	if (tx_work->wr_status == SR_WR_IDLE) {
		/*
		 * immediately schedule READ response w/o
		 * consuming IRQ entry: IRQ must be empty.
		 */
		tx_work->processed = 0;
		tx_work->mem[0].obj = NULL;
		tx_work->wr_status = SR_WR_QUEUED;
		resp = &tx_work->sqe;
	} else {
		resp = irq_get_free(qp);
		run_sq = 0;
	}

	if (likely(resp)) {
		resp->opcode = SIW_OP_READ_RESPONSE;

		resp->sge[0].length = length;
		resp->sge[0].laddr = laddr;
		resp->sge[0].lkey = lkey;

		resp->raddr = raddr;
		resp->rkey = rkey;
		resp->num_sge = length ? 1 : 0;

		smp_store_mb(resp->flags, SIW_WQE_VALID);
	} else {
		dprint(DBG_RX|DBG_ON, ": QP[%d]: IRQ %d exceeded %d!\n",
			QP_ID(qp), qp->irq_put % qp->attrs.irq_size,
			qp->attrs.irq_size);
		rv = -EPROTO;
	}

	unlock_sq(qp);

	if (run_sq)
		siw_sq_queue_work(qp, SIW_TX_CTX_PREF_SCQ_VECT);
	else if (rv == 0) {
		qp->irq_put++;
		dprint(DBG_OL, "(QP%d): TXTX: inc irq_put to %d (irq_get=%d)\n",
			QP_ID(qp), qp->irq_put, qp->irq_get);
	}

	return rv;
}
#endif
#endif

/*
 * Only called at start of Read.Resonse processing.
 * Fetch pending Read from ORQ, but keep it valid until
 * Read.Response processing done. No Queue locking needed.
 */
//omril: will we need lock here?
static struct siw_wqe *siw_orqe_get(struct siw_qp *qp)
{
	struct siw_sqe *orqe;
	struct siw_wqe *wqe = NULL;

	smp_mb();

//omril: do we need lock here? atomic race with read?


	orqe = &qp->orq[qp->orq_get % qp->attrs.orq_size];

	dprint(DBG_ATOMIC|DBG_OL, "(QP%d): qp->orq_get %d, orqe->flags=%08x\n",
		QP_ID(qp), qp->orq_get, orqe->flags);

	if (_load_shared(orqe->flags) & SIW_WQE_VALID) {
		wqe = rx_wqe(qp);
		wqe->sqe.id = orqe->id;
		wqe->sqe.opcode = orqe->opcode;
		wqe->sqe.sge[0].laddr = orqe->sge[0].laddr;
		wqe->sqe.sge[0].lkey = orqe->sge[0].lkey;
		wqe->sqe.sge[0].length = orqe->sge[0].length;
		wqe->sqe.flags = orqe->flags;
		wqe->sqe.num_sge = 1;
		wqe->bytes = orqe->sge[0].length;
		wqe->processed = 0;
		wqe->mem[0].obj = NULL;
		wqe->wr_status = SR_WR_INPROGRESS;
		
		/* For Debug */
		wqe->sqe.compare_add = orqe->compare_add;
		wqe->sqe.compare_add_mask = orqe->compare_add_mask;
		wqe->sqe.swap = orqe->swap;
		wqe->sqe.swap_mask = orqe->swap_mask;
		wqe->sqe.rkey = orqe->rkey;
		wqe->sqe.raddr = orqe->raddr;
		
		smp_wmb();
	}
	else {
		dprint(DBG_ATOMIC, "(QP%d): orqe->flags=%08x\n",
			QP_ID(qp), orqe->flags);
	}
	return wqe;
}

/*
 * siw_proc_rresp:
 *
 * Place incoming READ/ATOMIC RESP data into memory referenced by
 * READ/ATOMIC REQ WQE which is at the tip of the ORQ
 *
 * Function supports partially received RRESP's (suspending/resuming
 * current receive processing)
 */
int siw_proc_rresp(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	struct siw_wqe		*wqe;
	union siw_mem_resolved	*mem;
	struct siw_sge		*sge;
	struct siw_mr		*mr;
	int			bytes,
				rv;
	u8 opcode = rx_wqe(qp)->sqe.opcode;
#if SIW_ATOMIC_CAP
	bool is_atomic = 
		(opcode == SIW_OP_COMP_AND_SWAP || opcode == SIW_OP_MASKED_COMP_AND_SWAP);
#else
	bool is_atomic = false;
#endif
	dprint(DBG_TX, "(QP%d)\n", QP_ID(qp));

	if (rctx->first_ddp_seg) {
		if (unlikely(rx_wqe(qp)->wr_status != SR_WR_IDLE)) {
			pr_warn("QP[%d]: Start %sRESP: RX status %d, op %d\n",
					QP_ID(qp), 
					(opcode == SIW_OP_READ_RESPONSE ? "R" : (opcode == SIW_OP_COMP_AND_SWAP ? "A" : "W")),
					rx_wqe(qp)->wr_status,
				rx_wqe(qp)->sqe.opcode);
			rv = -EPROTO;
			goto done;
		}
		/*
		 * fetch pending RREQ from orq
		 */
		wqe = siw_orqe_get(qp);
		if (unlikely(!wqe)) {
			dprint(DBG_RX|DBG_ON, "(QP%d): ORQ empty at idx %d\n",
				QP_ID(qp),
				qp->orq_get % qp->attrs.orq_size);
			rv = -EPROTO;
			goto done;
		}
	} else {
		wqe = rx_wqe(qp);
		if (unlikely(wqe->wr_status != SR_WR_INPROGRESS)) {
			pr_warn("QP[%d]: Resume %sRESP: status %d\n",
				QP_ID(qp), 
				opcode == SIW_OP_READ_RESPONSE ? "R" : "A", rx_wqe(qp)->wr_status);
			rv = -EPROTO;
			goto done;
		}
	}
	opcode = wqe->sqe.opcode;
	is_atomic = (opcode == SIW_OP_COMP_AND_SWAP || opcode == SIW_OP_MASKED_COMP_AND_SWAP);
	if (rctx->state == SIW_GET_DATA_START) {		
		if (is_atomic)
			rv = siw_aresp_check_ntoh(rctx);
		else
			rv = siw_rresp_check_ntoh(rctx);
		if (unlikely(rv)) {
			siw_qp_event(qp, IB_EVENT_QP_FATAL);
			goto done;
		}
		if (!rctx->fpdu_part_rem) { /* zero length payload */
			if (opcode == SIW_OP_WRITE) {
				dprint(DBG_RX, "(QP%d): Write Ack for WR: %llx\n", 
				       QP_ID(qp), wqe->sqe.id);
			} else if (wqe->bytes != 0) {
				/* Zero-length payload for non zero-length Request.
				* Although technically this is allowed, it probably indicates a TX problem. 
				* WARN and break QP */
				pr_warn("QP[%d]: Unexpected zero-length FPDU\n",
					QP_ID(qp));
				rv = -EPROTO;
				goto done;
			} else {
				dprint(DBG_RX, "(QP%d) zero-length response for WR: %llx\n", 
				       QP_ID(qp), wqe->sqe.id);
			}
			return 0;
		}
	}

	sge = wqe->sqe.sge; /* there is only one */
	mem = &wqe->mem[0];

	if (mem->obj == NULL) {
		/*
		 * check target memory which resolves memory on first fragment
		 */
		dprint(DBG_OL, "calling siw_check_sge ...\n");
		dprint(DBG_TX, "siw_check_sge...\n");
		rv = siw_check_sge(qp->pd, sge, mem, SR_MEM_LWRITE, 0,
				   wqe->bytes);
		if (rv) {
			dprint(DBG_RX|DBG_ON, "(QP%d): siw_check_sge: %d\n",
				QP_ID(qp), rv);
			wqe->wc_status = SIW_WC_LOC_PROT_ERR;
			siw_qp_event(qp, IB_EVENT_QP_ACCESS_ERR);
			goto done;
		}
	}
	bytes = min(rctx->fpdu_part_rem, rctx->skb_new);

	//omril: where wqe->processed gets filled?
	mr = siw_mem2mr(mem->obj);
	if (mr->mem_obj == NULL)
		rv = siw_rx_kva(rctx, (void *)(sge->laddr + wqe->processed),
				bytes);
	else if (!mr->mem.is_pbl)
		rv = siw_rx_umem(rctx, mr->umem, sge->laddr + wqe->processed,
				 bytes);
	else
		rv = siw_rx_pbl(rctx, mr, sge->laddr + wqe->processed,
				 bytes);
	if (rv != bytes) {
		wqe->wc_status = SIW_WC_GENERAL_ERR;
		rv = -EINVAL;
		goto done;
	}
	rctx->fpdu_part_rem -= rv;
	rctx->fpdu_part_rcvd += rv;

	wqe->processed += rv;

	if (!rctx->fpdu_part_rem) {
		if (is_atomic) {
			u64 atomic_orig = ~(u64)0;
			/* TBD: umem */
			if (mr->mem_obj == NULL) {
				atomic_orig = *(u64 *)sge->laddr;
			} else if (mr->mem.is_pbl) {
				u64 buf_addr = siw_pbl_get_buffer(mr->pbl, 0, NULL, NULL);
				atomic_orig = *(u64 *)buf_addr;
			}

			dprint(DBG_ATOMIC, "(QP%d): SIW_ATOMIC_RESP: cmp=%016llx/%016llx, xchg=%016llx/%016llx, "
				"rkey=%x, raddr=%llx, resp=%llx, %s\n",
				QP_ID(qp), wqe->sqe.compare_add, wqe->sqe.compare_add_mask, 
				   wqe->sqe.swap, wqe->sqe.swap_mask, wqe->sqe.rkey, wqe->sqe.raddr, atomic_orig,
					(wqe->sqe.compare_add & wqe->sqe.compare_add_mask) == (atomic_orig & wqe->sqe.compare_add_mask) ? "SUCCESS" : "FAILURE");
		}
		rctx->ddp_to += rctx->fpdu_part_rcvd;
		dprint(DBG_TX, "(QP%d)\n", QP_ID(qp));
		return 0;
	}
done:
	dprint(DBG_TX, "(QP%d)\n", QP_ID(qp));
	return (rv < 0) ? rv : -EAGAIN;
}

//int siw_proc_areq(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
//{
//	if (rctx->fpdu_part_rem) {
//		dprint(DBG_ON|DBG_RX, "(QP%d): AREQ with MPA len %d\n", QP_ID(qp),
//			be16_to_cpu(rctx->hdr.ctrl.mpa_len));
//		return -EPROTO;
//	}
//	if (rctx->hdr.areq.read_size) {
//		dprint(DBG_ON|DBG_RX, "(QP%d): AREQ with read-size %d\n", QP_ID(qp),
//			be16_to_cpu(rctx->hdr.areq.read_size));
//		return -EPROTO;
//	}
//
//	return 0;
//}
int siw_proc_areq(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	dprint(DBG_ATOMIC, "SIW_ATOMIC: (QP%d): Recv AREQ: cmp=%016llx/%016llx, xchg=%016llx/%016llx, source_to=%016llx\n",
		QP_ID(qp),
		be64_to_cpu(rctx->hdr.areq.compare_add), 
		be64_to_cpu(rctx->hdr.areq.compare_add_mask),
		be64_to_cpu(rctx->hdr.areq.swap), 
		be64_to_cpu(rctx->hdr.areq.swap_mask),
		be64_to_cpu(rctx->hdr.areq.source_to));

	if (!rctx->fpdu_part_rem)
		return 0;

	dprint(DBG_ON|DBG_RX, "(QP%d): AREQ with MPA len %d\n", QP_ID(qp),
		be16_to_cpu(rctx->hdr.ctrl.mpa_len));

	return -EPROTO;
}

static void siw_init_aresp(struct siw_sqe *resp, struct siw_iwarp_rx *rctx)
{
	resp->opcode = SIW_OP_COMP_AND_SWAP_RESPONSE;

	resp->raddr = be64_to_cpu(rctx->hdr.areq.sink_to);
	resp->rkey = be32_to_cpu(rctx->hdr.areq.sink_stag);

	resp->sge[0].length = sizeof(u64);
	resp->sge[0].laddr = be64_to_cpu(rctx->hdr.areq.source_to);
	resp->sge[0].lkey = be32_to_cpu(rctx->hdr.areq.source_stag);
	resp->num_sge = 1;

	resp->compare_add = be64_to_cpu(rctx->hdr.areq.compare_add);
	resp->compare_add_mask = be64_to_cpu(rctx->hdr.areq.compare_add_mask);
	resp->swap = be64_to_cpu(rctx->hdr.areq.swap);
	resp->swap_mask = be64_to_cpu(rctx->hdr.areq.swap_mask);
}

int siw_proc_unsupp(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	dprint(DBG_ON, "QP[%d]: unrecognized packet received, %d bytes payload\n",
		QP_ID(qp), rctx->fpdu_part_rem);

	return -ECONNRESET;
}


int siw_proc_terminate(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	dprint(DBG_ON, " (QP%d): RX Terminate: type=%d, layer=%d, code=%d\n",
		QP_ID(qp),
		__rdmap_term_etype(&rctx->hdr.terminate),
		__rdmap_term_layer(&rctx->hdr.terminate),
		__rdmap_term_ecode(&rctx->hdr.terminate));

	return -ECONNRESET;
}

#if SIW_ATOMIC_CAP
/*
 * siw_init_xresp:
 *
 * Process inbound RDMA READ/ATOMIC REQ.
 * Produce a pseudo READ/ATOMIC RESPONSE WQE.
 * Put it at the tail of the IRQ, if there is another WQE currently in
 * transmit processing. If not, make it the current WQE to be processed
 * and schedule transmit processing.
 *
 * Can be called from softirq context and from process
 * context (RREAD socket loopback case!)
 *
 * return value:
 *	0:      success,
 *		failure code otherwise
 */
static int siw_init_xresp(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	struct siw_wqe *tx_work = tx_wqe(qp);
	struct siw_sqe *resp;

	u8 opcode = __rdmap_opcode(&rctx->hdr.ctrl);
	void (*init_resp_f)(struct siw_sqe *s, struct siw_iwarp_rx *r);
	int run_sq = 1, rv = 0;
	unsigned long flags;

	if (opcode == RDMAP_RDMA_READ_REQ)
		init_resp_f = siw_init_rresp;
	else if (opcode == RDMAP_RDMA_ATOMIC_REQ) {
		dprint(DBG_TX, "siw_init_aresp\n");
		init_resp_f = siw_init_aresp;
	} else if (opcode == RDMAP_RDMA_WRITE) {
		dprint(DBG_TX, "siw_init_wresp\n");
		init_resp_f = siw_init_wresp;
	} else {
		dprint(DBG_RX|DBG_ON, ": QP[%d]: unexpected opcode %d\n",
			QP_ID(qp), opcode);
		rv = -EPROTO;
		goto out;
	}

	/* We need to disable interrupts to prevent hard-lockup on the target
	 * (It calls siw_post_send from NVME IRQ context) */
	lock_sq_rxsave(qp, flags);

	//omril:
	//I think we should add here another cond of irq is empty!
	//otherwise read may bypass atomics
	//or worse, atomic may bypass prev atomic
	if (tx_work->wr_status == SR_WR_IDLE) {
		/*
		 * immediately schedule READ response w/o
		 * consuming IRQ entry: IRQ must be empty.
		 */
		tx_work->processed = 0;
		tx_work->mem[0].obj = NULL;
		tx_work->wr_status = SR_WR_QUEUED;
		resp = &tx_work->sqe;
	} else {
		resp = irq_get_free(qp);
		run_sq = 0;
	}

	if (likely(resp)) {
		init_resp_f(resp, rctx);

		if (opcode == RDMAP_RDMA_ATOMIC_REQ) {
			dprint(DBG_ATOMIC, "SIW_ATOMIC: (QP%d): Init ARESP: cmp=%016llx/%016llx, xchg=%016llx/%016llx, laddr=%016llx, run_sq=%d, qp->irq_put=%u, resp=" dprint_ptr_str() "\n",
				QP_ID(qp),
				resp->compare_add, resp->compare_add_mask,
				resp->swap, resp->swap_mask,
				resp->sge[0].laddr,
				run_sq,
				run_sq == 0 ? qp->irq_put : -1,
				resp);
			
			if (run_sq) {
				if ((rv = siw_activate_tx_atomic(qp, resp->compare_add, resp->compare_add_mask,
									   resp->swap, resp->swap_mask)) != 0)
					run_sq = false;
				goto unlock;
			}
		}

		smp_store_mb(resp->flags, SIW_WQE_VALID);
	} else {
		dprint(DBG_RX|DBG_ON, ": QP[%d]: IRQ %d exceeded %d!\n",
			QP_ID(qp), qp->irq_put % qp->attrs.irq_size,
			qp->attrs.irq_size);
		rv = -EPROTO;
		//omril: do we still want to inc qp->irq_put?
	}
	
unlock:
	unlock_sq_rxsave(qp, flags);

	if (run_sq)
		siw_sq_queue_work(qp, SIW_TX_CTX_PREF_SCQ_VECT);
	else if (rv == 0)
		qp->irq_put++;

out:
	return rv;
}
#endif

static int siw_get_trailer(struct siw_qp *qp, struct siw_iwarp_rx *rctx)
{
	struct sk_buff	*skb = rctx->skb;
	u8		*tbuf = (u8 *)&rctx->trailer.crc - rctx->pad;
	int		avail;

	avail = min(rctx->skb_new, rctx->fpdu_part_rem);

	skb_copy_bits(skb, rctx->skb_offset,
		      tbuf + rctx->fpdu_part_rcvd, avail);

	rctx->fpdu_part_rcvd += avail;
	rctx->fpdu_part_rem -= avail;

	rctx->skb_new -= avail;
	rctx->skb_offset += avail;
	rctx->skb_copied += avail;

	dprint(DBG_RX, " (QP%d): %d remaining (%d)\n", QP_ID(qp),
		rctx->fpdu_part_rem, avail);

	if (!rctx->fpdu_part_rem) {
		__be32	crc_in, crc_own = 0;
		/*
		 * check crc if required
		 */
		if (!rctx->mpa_crc_hd) {
			if (rctx->trailer.crc == MPA_CRC_OFF_MAGIC)
				return 0;
			else {
				dprint(DBG_RX|DBG_ON,
					   " (QP%d): CRC MAGIC ERROR, qp " dprint_ptr_str() " rctx " dprint_ptr_str() " skb " dprint_ptr_str() " skb_offset %d skb_new %d\n",
					   QP_ID(qp), qp, rctx, rctx->skb, rctx->skb_offset, rctx->skb_new);
				//BUG_ON(1);
				return -EINVAL;
			}
		}

		if (rctx->pad && siw_crc_array(rctx->mpa_crc_hd,
					       tbuf, rctx->pad) != 0)
			return -EINVAL;
		siw_debug_rx_crc(rctx, tbuf, rctx->pad);

		crypto_shash_final(rctx->mpa_crc_hd, (u8 *)&crc_own);

		/*
		 * CRC32 is computed, transmitted and received directly in NBO,
		 * so there's never a reason to convert byte order.
		 */
		crc_in = rctx->trailer.crc;

		if (crc_in != crc_own) {
			dprint(DBG_RX|DBG_ON,
				" (QP%d): CRC ERROR in:=%08x, own=%08x\n",
				QP_ID(qp), crc_in, crc_own);
			return -EINVAL;
		}
		return 0;
	}
	return -EAGAIN;
}

//omril: where do we check if the status of the whole pkt (or WR) is OK???
static int siw_get_hdr(struct siw_iwarp_rx *rctx)
{
	struct sk_buff		*skb = rctx->skb;
	struct iwarp_ctrl	*c_hdr = &rctx->hdr.ctrl;
	u8			opcode;

	int bytes;

	if (rctx->fpdu_part_rcvd < sizeof(struct iwarp_ctrl)) {
		/*
		 * copy first fix part of iwarp hdr
		 */
		bytes = min_t(int, rctx->skb_new, sizeof(struct iwarp_ctrl)
				- rctx->fpdu_part_rcvd);

		skb_copy_bits(skb, rctx->skb_offset,
			      (char *)c_hdr + rctx->fpdu_part_rcvd, bytes);

		rctx->fpdu_part_rcvd += bytes;

		rctx->skb_new -= bytes;
		rctx->skb_offset += bytes;
		rctx->skb_copied += bytes;

		if (!rctx->skb_new ||
			rctx->fpdu_part_rcvd < sizeof(struct iwarp_ctrl))
			return -EAGAIN;

		if (__ddp_version(c_hdr) != DDP_VERSION) {
			dprint(DBG_RX|DBG_ON, " dversion %d\n",
				__ddp_version(c_hdr));
			/* Restore previous header ctrl part so we can complete with error correctly */
			*c_hdr = rctx->prev_hdr_ctrl;
			return -EINVAL;
		}
		if (__rdmap_version(c_hdr) != RDMAP_VERSION) {
			dprint(DBG_RX|DBG_ON, " rversion %d\n",
				__rdmap_version(c_hdr));
			/* Restore previous header ctrl part so we can complete with error correctly */
			*c_hdr = rctx->prev_hdr_ctrl;
			return -EINVAL;
		}
		opcode = __rdmap_opcode(c_hdr);

		if (opcode > RDMAP_TERMINATE) {
			dprint(DBG_RX|DBG_ON, " opcode %d\n", opcode);
			/* Restore previous header ctrl part so we can complete with error correctly */
			*c_hdr = rctx->prev_hdr_ctrl;
			return -EINVAL;
		}
		dprint(DBG_RX, "(QP%d): New Header, opcode:%s(%d)\n",
			RX_QPID(rctx), rdmap_opcode_to_str(opcode), opcode);
	} else
		opcode = __rdmap_opcode(c_hdr);
	/*
	 * figure out len of current hdr: variable length of
	 * iwarp hdr forces us to copy hdr information
	 */
	bytes = min(rctx->skb_new,
		  iwarp_pktinfo[opcode].hdr_len - rctx->fpdu_part_rcvd);

	skb_copy_bits(skb, rctx->skb_offset,
		      (char *)c_hdr + rctx->fpdu_part_rcvd, bytes);

	rctx->fpdu_part_rcvd += bytes;

	rctx->skb_new -= bytes;
	rctx->skb_offset += bytes;
	rctx->skb_copied += bytes;

	if (rctx->fpdu_part_rcvd == iwarp_pktinfo[opcode].hdr_len) {
		/*
		 * HDR receive completed. Check if the current DDP segment
		 * starts a new RDMAP message or continues a previously
		 * started RDMAP message.
		 *
		 * Note well from the comments on DDP reassembly:
		 * - Support for unordered reception of DDP segments
		 *   (or FPDUs) from different RDMAP messages is not needed.
		 * - Unordered reception of DDP segments of the same
		 *   RDMAP message is not supported. It is probably not
		 *   needed with most peers.
		 */
		siw_dprint_hdr(&rctx->hdr, RX_QPID(rctx), "HDR received");

		if (rctx->more_ddp_segs != 0) {
			rctx->first_ddp_seg = 0;
			if (rctx->prev_rdmap_opcode != opcode) {
				dprint(DBG_ON,
					"packet intersection: %d <> %d\n",
					rctx->prev_rdmap_opcode, opcode);
				/* Restore previous header ctrl part so we can complete with error correctly */
				*c_hdr = rctx->prev_hdr_ctrl;
				return -EPROTO;
			}
		} else {
			rctx->prev_rdmap_opcode = opcode;
			rctx->first_ddp_seg = 1;
			rctx->prev_hdr_ctrl = rctx->hdr.ctrl;
		}
		rctx->more_ddp_segs =
			c_hdr->ddp_rdmap_ctrl & DDP_FLAG_LAST ? 0 : 1;

		return 0;
	}
	return -EAGAIN;
}

static inline int siw_fpdu_payload_len(struct siw_iwarp_rx *rctx)
{
	return be16_to_cpu(rctx->hdr.ctrl.mpa_len) - rctx->fpdu_part_rcvd
		+ MPA_HDR_SIZE;
}

static inline int siw_fpdu_trailer_len(struct siw_iwarp_rx *rctx)
{
	int mpa_len = be16_to_cpu(rctx->hdr.ctrl.mpa_len) + MPA_HDR_SIZE;

	return MPA_CRC_SIZE + (-mpa_len & 0x3);
}

static void siw_check_tx_fence(struct siw_qp *qp, int rx_error)
{
	int resume_tx = 0;
	struct siw_sqe *rreq;
	unsigned long flags;

	/*****************************************
	 * This function is called from rx so:
	 * - We don't need to do rxsave as we are in softirq context
	 * - We don't want to lock sq_lock for performance/deadlock prevention reasons 
	 *
	 * UPDATE: Jared
	 * - We do need to do rxsave because a HW interrupt from the disk 
	 * can call a callback that does post_send
	 ****************************************/

	lock_orq_rxsave(qp, flags);

	/* free current orq entry */
	rreq = orq_get_current(qp);
	smp_store_mb(rreq->flags, 0);

	qp->orq_get++;
	dprint(DBG_OL, "(QP%d): TXTX: inc orq_get to %d (orq_put=%d, orq_fence=%d)\n",
	       QP_ID(qp), qp->orq_get, qp->orq_put, qp->tx_ctx.orq_fence);

	if (qp->tx_ctx.orq_fence == SIW_ORQ_FENCE_NONE) {
		/* No ORQ fence, we are done here */
		goto unlock_orq;
	}

	if (qp->tx_ctx.orq_fence == SIW_ORQ_FENCE_FULL) {
		/* ORQ fence was due to full ORQ, ORQ is no longer full, 
		 * clear fence and resume TX (if no rx-error) */
		qp->tx_ctx.orq_fence = SIW_ORQ_FENCE_NONE;
		resume_tx = !rx_error;
		goto unlock_orq;
	}

	/* ORQ fence is a WR fence, ORQ must be empty to continue */
	BUG_ON(qp->tx_ctx.orq_fence != SIW_ORQ_FENCE_WR);
	if (siw_orq_empty(qp)) {
		/* ORQ is empty, clear fence and resume TX  (if no rx-error) */
		BUG_ON(qp->orq_get != qp->orq_put);
		qp->tx_ctx.orq_fence = SIW_ORQ_FENCE_NONE;
		resume_tx = !rx_error;
		goto unlock_orq;
	}

unlock_orq:
	unlock_orq_rxsave(qp, flags);

	if (resume_tx)
		siw_sq_queue_work(qp, SIW_TX_CTX_PREF_SCQ_VECT);
}

/* old siw_check_tx_fence */
#if 0
static void siw_check_tx_fence(struct siw_qp *qp)
{
	struct siw_wqe *tx_waiting = tx_wqe(qp);
	struct siw_sqe *rreq;
	int resume_tx = 0;
	unsigned long flags;

	lock_sq_rxsave(qp, flags);

	/* free current orq entry */
	rreq = orq_get_current(qp);
	smp_store_mb(rreq->flags, 0);

	if (qp->tx_ctx.orq_fence) {
		if (unlikely(tx_waiting->wr_status != SR_WR_QUEUED)) {
			pr_warn("QP[%d]: Resume from fence: status %d wrong\n",
				QP_ID(qp), tx_waiting->wr_status);
			goto out;
		}
		/* resume SQ processing */
		if (tx_waiting->sqe.opcode == SIW_OP_READ ||
		    tx_waiting->sqe.opcode == SIW_OP_READ_LOCAL_INV ||
			/* atomic op fenced not by verbs but because ORQ was full */
			((tx_waiting->sqe.opcode == SIW_OP_COMP_AND_SWAP ||
			tx_waiting->sqe.opcode == SIW_OP_MASKED_COMP_AND_SWAP ||
			(tx_waiting->sqe.opcode == SIW_OP_WRITE && tx_waiting->sqe.flags & SIW_WQE_SIGNALLED)) &&
			 !(tx_waiting->sqe.flags & SIW_WQE_READ_FENCE))) {

			/* if a read/atomic was fenced, it was because ORQ was full,
			   let it be processed as now there is a free ORQ */
			rreq = orq_get_tail(qp);
			if (unlikely(!rreq))
				goto out;

			siw_read_to_orq(rreq, &tx_waiting->sqe);

			qp->orq_put++;
			dprint(DBG_OL, "(QP%d): TXTX: inc orq_put to %d (orq_get=%d), flags=%04x\n",
				QP_ID(qp), qp->orq_put, qp->orq_get, tx_waiting->sqe.flags);

			//omril: remember, there's only one tx_ctx/tx_wqe, so orq_fence can be on due
			//to one wqe that could have set this flag
			qp->tx_ctx.orq_fence = 0;
			resume_tx = 1;

		} else if (siw_orq_empty(qp)) {
			/* There are no uncompleted read/atomic ops,
			   resume processing of current tx WQE */
			qp->tx_ctx.orq_fence = 0;
			resume_tx = 1;
		} else
			pr_warn("QP[%d]:  Resume from fence: error: %d:%d\n",
				QP_ID(qp), qp->orq_get, qp->orq_put);
	}

	qp->orq_get++;
	dprint(DBG_OL, "(QP%d): TXTX: inc orq_get to %d (orq_put=%d)\n",
		QP_ID(qp), qp->orq_get, qp->orq_put);
out:
	unlock_sq_rxsave(qp, flags);

	if (resume_tx)
		siw_sq_queue_work(qp, SIW_TX_CTX_PREF_SCQ_VECT);
}
#endif

/*
 * siw_rdmap_complete()
 *
 * Complete processing of an RDMA message after receiving all
 * DDP segmens or ABort processing after encountering error case.
 *
 *   o SENDs + RRESPs will need for completion,
 *   o RREQs need for  READ RESPONSE initialization
 *   o WRITEs need memory dereferencing
 *
 * TODO: Failed WRITEs need local error to be surfaced.
 */

static inline int
siw_rdmap_complete(struct siw_qp *qp, int error, int notify)
{
	struct siw_iwarp_rx	*rctx = &qp->rx_ctx;
	struct siw_wqe		*wqe = rx_wqe(qp);
	enum siw_wc_status	wc_status = wqe->wc_status;

	u8 opcode = __rdmap_opcode(&rctx->hdr.ctrl);
	enum siw_opcode op;
	int rv = 0;

	dprint(DBG_OL, "(QP%d): complete processing of %s(%d)\n",
		   QP_ID(qp), rdmap_opcode_to_str(opcode), opcode);

	switch (opcode) {

	case RDMAP_SEND_SE:
	case RDMAP_SEND_SE_INVAL:
		wqe->rqe.flags |= SIW_WQE_SOLICITED;
		FALLTHRU;
	case RDMAP_SEND:
	case RDMAP_SEND_INVAL:
		if (wqe->wr_status == SR_WR_IDLE)
			break;

		rctx->ddp_msn[RDMAP_UNTAGGED_QN_SEND]++;

		if (error != 0 && wc_status == SIW_WC_SUCCESS)
			wc_status = SIW_WC_GENERAL_ERR;

		/*
		 * Handle STag invalidation request
		 */
		if (wc_status == SIW_WC_SUCCESS &&
		    (opcode == RDMAP_SEND_INVAL ||
		     opcode == RDMAP_SEND_SE_INVAL)) {
			rv = siw_invalidate_stag(qp->pd, rctx->inval_stag);
			if (rv)
				wc_status = SIW_WC_REM_INV_REQ_ERR;
		}
		rv = siw_rqe_complete(qp, &wqe->rqe, &wqe->rqe_md, wqe->processed,
				      wc_status, notify);
		siw_wqe_put_mem(wqe, SIW_OP_RECEIVE); //omril: when SIW_OP_RECEIVE is used? can it be used for atomic?

		break;

	case RDMAP_RDMA_READ_RESP:
	case RDMAP_RDMA_ATOMIC_RESP:
		if (wqe->wr_status == SR_WR_IDLE)
			break;

		if (opcode == RDMAP_RDMA_READ_RESP) {
			if (rctx->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_WR_ACK)
				op = SIW_OP_WRITE_RESPONSE;
			else {
				rctx->ddp_msn[RDMAP_UNTAGGED_QN_RDMA_READ]++;
				op = SIW_OP_READ; //omril: shouldn't be SIW_OP_READ_RESPONSE?
			}
		}
		else {
			dprint(DBG_ATOMIC, "(QP%d): complete atomic resp, error=%d\n",
				QP_ID(qp), error);

			rctx->ddp_msn[RDMAP_UNTAGGED_QN_RDMA_ATOMIC]++;
			op = SIW_OP_COMP_AND_SWAP_RESPONSE;
		}

		dprint(DBG_OL, "(QP%d): TXTX: error=%d\n",
			QP_ID(qp), error);

		if (error != 0) {
			if  (rctx->state == SIW_GET_HDR || error == -ENODATA) {
				/*  eventual RREQ in ORQ left untouched */

				dprint(DBG_OL, "(QP%d): TXTX: on error, ORQ left untouched!\n",
					QP_ID(qp));

				break;
			}

			if (wc_status == SIW_WC_SUCCESS)
				wc_status = SIW_WC_GENERAL_ERR;
		} else if (opcode == RDMAP_RDMA_READ_RESP &&
				   qp->kernel_verbs) {
			/*
			 * Handle any STag invalidation request
			 */
			struct siw_sqe *req = orq_get_current(qp);

			if (req->opcode == SIW_OP_READ_LOCAL_INV) {
				rv = siw_invalidate_stag(qp->pd,
							 req->sge[0].lkey);
				if (rv && wc_status == SIW_WC_SUCCESS) {
					wc_status = SIW_WC_GENERAL_ERR;
					error = rv;
				}
			}
		}
		/*
		 * All errors turn the wqe into signalled.
		 */

		dprint(DBG_OL, "(QP%d): TXTX: (WQE%llx) SIW_WQE_SIGNALLED=%d, error=%d\n",
			QP_ID(qp), wqe->sqe.id, (wqe->sqe.flags & SIW_WQE_SIGNALLED), error);

		if ((wqe->sqe.flags & SIW_WQE_SIGNALLED) || error != 0) {

			dprint(DBG_OL, "(QP%d): TXTX: (WQE%llx) call siw_sqe_complete\n",
				QP_ID(qp), wqe->sqe.id);

			rv = siw_sqe_complete(qp, &wqe->sqe, NULL, wqe->processed,
								  wc_status, notify);
		}
		siw_wqe_put_mem(wqe, op);//SIW_OP_READ);

		/* read/atomic is done - check if there are blocked WRs */
		siw_check_tx_fence(qp, error);
		break;

	case RDMAP_RDMA_READ_REQ:
		if (error == 0)
#if SIW_ATOMIC_CAP
			rv = siw_init_xresp(qp, rctx);
#else
			rv = siw_init_rresp(qp, rctx);
#endif
		break;

	case RDMAP_RDMA_WRITE:
		if (wqe->wr_status == SR_WR_IDLE)
			break;
		if (rctx->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_WR_ACK) {
			/* Send a Write Response to acknowledge the Write */
			dprint(DBG_RX,"QP(%d): Putting Write ACK on ORQ\n", QP_ID(qp));
			rv = siw_init_xresp(qp, rctx);
		}

		/*
		 * Free References from memory object if
		 * attached to receive context (inbound WRITE)
		 * While a zero-length WRITE is allowed, the
		 * current implementation does not create
		 * a memory reference (it is unclear if memory
		 * rights should be checked in that case!).
		 *
		 * TODO: check zero length WRITE semantics
		 */
		if (rx_mem(qp)) {
			siw_mem_put(rx_mem(qp));
			rx_mem(qp) = NULL;
		}
		break;

	case RDMAP_RDMA_ATOMIC_REQ:
		rv = siw_init_xresp(qp, rctx);
		break;

	default:
		break;
	}
	wqe->wr_status = SR_WR_IDLE;
	memset(&wqe->rqe_md, 0, sizeof(wqe->rqe_md));

	return rv;
}

#ifdef SIW_DEBUG_RX_CRC
static inline void siw_rx_debug_crc(struct siw_iwarp_rx *rctx, bool crc_error)
{
	if (rctx->curr_fpdu_skb_copied < rctx->skb_copied && rctx->curr_fpdu_bytes < SIW_DEBUG_RX_CRC_MAX_FPDU) {
		size_t bytes = min_t(size_t, rctx->skb_copied - rctx->curr_fpdu_skb_copied, SIW_DEBUG_RX_CRC_MAX_FPDU - rctx->curr_fpdu_bytes);
		size_t crc_bytes = min_t(size_t, bytes, rctx->curr_fpdu_crc_bytes_rem);
		skb_copy_bits(rctx->skb, rctx->curr_fpdu_skb_offset, 
					  rctx->curr_fpdu + rctx->curr_fpdu_bytes, bytes);
		crypto_shash_update(rctx->curr_fpdu_shash_desc, rctx->curr_fpdu + rctx->curr_fpdu_bytes, crc_bytes);
		if (rctx->curr_fpdu_crc_bytes_rem != ~(size_t)0)
			rctx->curr_fpdu_crc_bytes_rem -= bytes;
		rctx->curr_fpdu_bytes += bytes;
		rctx->curr_fpdu_skb_copied += bytes;
		rctx->curr_fpdu_skb_offset += bytes;
	}
	if (rctx->state == SIW_GET_TRAILER) {
		u32 curr_fpdu_crc;
		crypto_shash_final(rctx->curr_fpdu_shash_desc, (u8 *)&curr_fpdu_crc);
		if (crc_error) {
			const struct tcphdr *th = tcp_hdr(rctx->skb);
			const struct iphdr *ip = ip_hdr(rctx->skb);
			pr_err("SIW_DEBUG_CRC: %pI4:%u -> %pI4:%u - start seq %u rctx " dprint_ptr_str() " curr_fpdu " dprint_ptr_str() " curr_fpdu_len %zu curr_fpdu_bytes %zu curr_fpdu_crc_bytes %zu curr_fpdu_crc: %x\n",
				   &ip->saddr, be16_to_cpu(th->source), &ip->daddr,
				   be16_to_cpu(th->dest), rctx->curr_fpdu_seq, rctx,
				   rctx->curr_fpdu, rctx->curr_fpdu_len, rctx->curr_fpdu_bytes, rctx->curr_fpdu_crc_bytes, curr_fpdu_crc);
			print_hex_dump(KERN_INFO, "SIW_DEBUG_CRC: ", DUMP_PREFIX_OFFSET, 16, 1, rctx->curr_fpdu, rctx->curr_fpdu_len, false);
			BUG_ON(1);
		}
	} else if (rctx->state == SIW_GET_HDR) {
		/* Just finished state SIW_GET_TRAILER */
		void *new_curr_fpdu = rctx->prev_fpdu;
		const struct tcphdr *th = tcp_hdr(rctx->skb);
		
		rctx->prev_fpdu = rctx->curr_fpdu;
		rctx->prev_fpdu_len = rctx->curr_fpdu_len;
		rctx->prev_fpdu_pad = rctx->curr_fpdu_pad;
		rctx->prev_fpdu_seq = rctx->curr_fpdu_seq;
		rctx->curr_fpdu = new_curr_fpdu;
		rctx->curr_fpdu_len = 0;
		rctx->curr_fpdu_bytes = 0;
		rctx->curr_fpdu_crc_bytes = 0;
		rctx->curr_fpdu_seq = be32_to_cpu(th->seq) + rctx->skb_offset;
		crypto_shash_init(rctx->curr_fpdu_shash_desc);
		rctx->curr_fpdu_crc_bytes_rem = ~(size_t)0; // We don't know yet
	} else if (rctx->state == SIW_GET_DATA_START) {
		/* Just finished state SIW_GET_HDR */
		struct iwarp_ctrl *c_hdr = rctx->curr_fpdu;
		rctx->curr_fpdu_len = be16_to_cpu(c_hdr->mpa_len) + MPA_HDR_SIZE;
		rctx->curr_fpdu_pad = -rctx->curr_fpdu_len & 0x3;
		rctx->curr_fpdu_len += rctx->curr_fpdu_pad;
		rctx->curr_fpdu_crc_bytes_rem = rctx->curr_fpdu_len - rctx->curr_fpdu_bytes;
		rctx->curr_fpdu_len += MPA_CRC_SIZE;
	}
}
#else
static inline void siw_rx_debug_crc(struct siw_iwarp_rx *rctx, bool crc_error) 
{
	(void)rctx;
	(void)crc_error;
}
#endif

int siw_do_rx_work(struct siw_qp* qp)
{
	struct siw_iwarp_rx *rctx = &qp->rx_ctx;
	unsigned long flags;
	int rv;

	if (likely(down_read_trylock(&qp->state_lock))) {
		read_descriptor_t rd_desc = {.arg.data = qp, .count = 1};

		dprint(DBG_SK|DBG_RX, "(QP%d): "
		"state (before tcp_read_sock)=%d\n",
		       QP_ID(qp), qp->attrs.state);

		if (likely(qp->attrs.state == SIW_QP_STATE_RTS)) {
			struct sock *sk = qp->attrs.llp_stream_handle->sk;

#if SIW_ENABLE_PANIC_REMOTE_ON_RX_ERR
			do {
				extern bool panic_remote_on_rx_err;
				if (unlikely(panic_remote_on_rx_err)) {
					struct tcp_sock *tp = tcp_sk(sk);

					if (tp->urg_data) {
						dprint(DBG_RX | DBG_ON, "(QP%d): Panic from Remote! qp: " dprint_ptr_str() "\n", QP_ID(qp), qp);
						panic("(QP%d): Panic from Remote! qp: " dprint_ptr_str() "\n", QP_ID(qp), qp);
					}
				}
			} while(0);
#endif

			if (tcp_inq(sk) < 1) {
				/* Nothing to receive */
				up_read(&qp->state_lock);
				dprint(DBG_RX, "(QP%d): Nothing to receive\n", QP_ID(qp));
				rv = 0;
				goto done;
			}

			/* Check for RX suspension (need to acquire rq lock) */
			lock_rq_rxsave(qp, flags);
			if (qp->rx_ctx.rx_suspend) {
				unlock_rq_rxsave(qp, flags);
				up_read(&qp->state_lock);
				dprint(DBG_RX | DBG_ON, "(QP%d): RX suspended\n", QP_ID(qp));
				rv = -ESHUTDOWN;
				goto done;
			}
			unlock_rq_rxsave(qp, flags);

			if (rctx->state == SIW_WAIT_RQE) {
				/* Some Sanity Checks */
				BUG_ON(!rctx->first_ddp_seg);
				BUG_ON(iwarp_pktinfo[__rdmap_opcode(&rctx->hdr.ctrl)].proc_data != siw_proc_send);
				BUG_ON(!qp->srq);

#if SIW_SRQ_WAIT_LIST
				BUG_ON(qp->srq_rqe_state == SRQ_RQE_RECV);
				if (qp->srq_rqe_ready.flags != SIW_WQE_VALID) {
					/* Still waiting on RQE */
					up_read(&qp->state_lock);
					dprint(DBG_RX | DBG_ON, "(QP%d): Still waiting on RQE\n", QP_ID(qp));
					rv = -ENOENT;
					goto done;
				}
#endif
				/* Either we have an RQE in srq_rqe_ready or we are retrying on a delayed work.
				 * Switch state back to SIW_GET_DATA_START and then resume processing
				 * If it fails to get RQE again, it will reschedule the delayed work. */
				rctx->state = SIW_GET_DATA_START;
			}

			/*
			 * Implements data receive operation during
			 * socket callback. TCP gracefully catches
			 * the case where there is nothing to receive
			 * (not calling siw_tcp_rx_data() then).
			 */
			rv = tcp_read_sock(sk, &rd_desc, siw_tcp_rx_data);
		} else {
			rv = -EBUSY;
		}

		dprint(DBG_SK|DBG_RX, "(QP%d): "
		"state (after tcp_read_sock)=%d\n",
		       QP_ID(qp), qp->attrs.state);

		up_read(&qp->state_lock);
	} else {
		/* state-lock is write-locked, the QP must be going down
		 * Check that rx suspend flag is set, otherwise something has gone wrong
		 * NOTE: Seen on Coreweave with KASAN, this callback sneaks in before siw_qp_modify
		 * finishes transitioning to RTS, so removing the bug-on.
		 */
		lock_rq_rxsave(qp, flags);
		if (!qp->rx_ctx.rx_suspend) {
			dprint(DBG_ON, "(QP%d): QP (" dprint_ptr_str() ") write-locked, but rx_suspend = 0\n",
			       QP_ID(qp), qp);
		}
		unlock_rq_rxsave(qp, flags);
		rv = -EBUSY;
	}
done:
	return rv;
}

void siw_rx_work_handler(struct work_struct* work)
{
	struct siw_iwarp_rx *rctx = container_of(work, struct siw_iwarp_rx, rx_work.work);
	struct siw_qp *qp = RX_QP(rctx);
	struct socket *s = READ_ONCE(qp->attrs.llp_stream_handle);
	struct sock *sk;
	int rv;

	if (unlikely(!s)) {
		goto put;
	}

	sk = s->sk;
	lock_sock(sk);
	if ((rv = siw_do_rx_work(qp)) < 0) {
		dprint(DBG_SK|DBG_RX, "(QP%d): "
		"siw_do_rx_work() returned error %d\n",
		       QP_ID(qp), rv);
	}
	release_sock(sk);

put:
	siw_qp_put(qp); /* Put ref from siw_rx_queue_work */
}

void siw_rx_queue_work(struct siw_qp *qp, unsigned long delay)
{
	siw_qp_get(qp);
	/* using schedule_delayed_work() to share code path with wait_rqe_delay_ms which uses a non-zero delay */
	/* siw_retry_get_rqe_work */
	if (!queue_delayed_work(siw_rx_wq, &qp->rx_ctx.rx_work, delay)) {
		/* Already scheduled */
		siw_qp_put(qp);
	}
}

void siw_rx_cancel_work(struct siw_qp *qp) {
	if (cancel_delayed_work_sync(&qp->rx_ctx.rx_work)) {
		siw_qp_put(qp);
	}
}

/*
 * siw_tcp_rx_data()
 *
 * Main routine to consume inbound TCP payload
 *
 * @rd_desc:	read descriptor
 * @skb:	socket buffer
 * @off:	offset in skb
 * @len:	skb->len - offset : payload in skb
 */
int siw_tcp_rx_data(read_descriptor_t *rd_desc, struct sk_buff *skb,
		    unsigned int off, size_t len)
{
	struct siw_qp		*qp = rd_desc->arg.data;
	struct siw_iwarp_rx	*rctx = &qp->rx_ctx;
	int			rv;

	rctx->skb = skb;
	rctx->skb_new = skb->len - off;
	rctx->skb_offset = off;
	rctx->skb_copied = 0;

	dprint(DBG_RX, "(QP%d): new data %d (skb->len=%u, off=%u)\n",
		QP_ID(qp), rctx->skb_new, skb->len, off);

#ifdef SIW_DEBUG_RX_CRC
	rctx->curr_fpdu_skb_copied = 0;
	rctx->curr_fpdu_skb_offset = off;
#endif

	while (rctx->skb_new) {
		int run_completion = 1;

		switch (rctx->state) {
		case SIW_GET_HDR:
			rv = siw_get_hdr(rctx);
			if (!rv) {
				if (rctx->mpa_crc_hd &&
				    siw_crc_rxhdr(rctx) != 0) {
					rv = -EINVAL;
					break;
				}
				rctx->fpdu_part_rem =
					siw_fpdu_payload_len(rctx);

				dprint(DBG_OL, "(QP%d): rctx->fpdu_part_rem=%d\n",
					QP_ID(qp), rctx->fpdu_part_rem);

				if (rctx->fpdu_part_rem)
					rctx->pad = -rctx->fpdu_part_rem & 0x3;
				else
					rctx->pad = 0;

				rctx->state = SIW_GET_DATA_START;
				rctx->fpdu_part_rcvd = 0;
			}
			break;

		case SIW_GET_DATA_MORE:
			/*
			 * Another data fragment of the same DDP segment.
			 * Headers will not be checked again by the
			 * opcode-specific data receive function below.
			 * Setting first_ddp_seg = 0 avoids repeating
			 * initializations that may occur only once per
			 * DDP segment.
			 */
			rctx->first_ddp_seg = 0;
			FALLTHRU;

		case SIW_GET_DATA_START:
			/*
			 * Headers will be checked by the opcode-specific
			 * data receive function below.
			 */
			/*
			 * siw_proc_rreq, siw_proc_rresp
			 * siw_proc_areq, siw_proc_aresp 
			 * siw_proc_write, siw_proc_send
			 */
			rv = siw_rx_data(qp, rctx);
			if (!rv) {
				rctx->fpdu_part_rem =
					siw_fpdu_trailer_len(rctx);

			dprint(DBG_OL, "(QP%d): rctx->fpdu_part_rem=%d\n",
				QP_ID(qp), rctx->fpdu_part_rem);

				rctx->fpdu_part_rcvd = 0;
				rctx->state = SIW_GET_TRAILER;
			} else {
				if (rv == -ENOENT) {
					unsigned long flags;
					BUG_ON(iwarp_pktinfo[__rdmap_opcode(&rctx->hdr.ctrl)].proc_data != siw_proc_send);
					BUG_ON(!rctx->first_ddp_seg);
					BUG_ON(!qp->srq);
					
#if SIW_SRQ_WAIT_LIST
					dprint(DBG_ON | DBG_RX, "(QP%d): Adding to SRQ(" dprint_ptr_str() ") Wait List\n", QP_ID(qp), qp->srq);
					lock_srq_rxsave(qp->srq, flags);
					/* This state will make us skip siw_tcp_rx_data() from next
					   calls to siw_qp_llp_data_ready().
					   This scope shall be (is) protected by qp->state_lock */
					rctx->state = SIW_WAIT_RQE; 
					list_add_tail(&qp->srq_wait_link, &qp->srq->wait_qp_head);
					unlock_srq_rxsave(qp->srq, flags);
					/* We don't return an error this time around because we want tcp_read_sock to update the copied_seq ptr. The state ensures we will return an error next time. */
					goto out;
#else
					if (++rctx->n_retries < wait_rqe_max_retries) {
						dprint(DBG_ON | DBG_RX, "(QP%d): S(RQ) empty, scheduling retry %d in %u ms\n", 
							   QP_ID(qp), rctx->n_retries, wait_rqe_delay_ms);

						rctx->state = SIW_WAIT_RQE;
						siw_rx_queue_work(qp, wait_rqe_delay_ms * 1000 / HZ);
						/* We don't return an error this time around because we want tcp_read_sock to update the copied_seq ptr. The state ensures we will return an error next time. */
						goto out;
					}
#endif
				} else if (unlikely(rv == -ECONNRESET))
					run_completion = 0;
				else {
					rctx->state = SIW_GET_DATA_MORE;
					rctx->n_retries = 0;
				}
			}
			break;

		case SIW_GET_TRAILER:
			/*
			 * read CRC + any padding
			 */
			rv = siw_get_trailer(qp, rctx);
			if (!rv) {
				/*
				 * FPDU completed.
				 * complete RDMAP message if last fragment
				 */
				rctx->state = SIW_GET_HDR;
				rctx->fpdu_part_rcvd = 0;
				
				if (rctx->first_ddp_seg)
					rctx->wqe_active.rqe_md.first_ddp_recv_time = ktime_get();

				if (!(rctx->hdr.ctrl.ddp_rdmap_ctrl
					& DDP_FLAG_LAST))
					/* more frags */
					break;

				rctx->wqe_active.rqe_md.last_ddp_recv_time = ktime_get();
				rctx->wqe_active.rqe_md.rx_cpu = smp_processor_id();
				if (skb_rx_queue_recorded(skb))
					rctx->wqe_active.rqe_md.rx_queue = skb_get_rx_queue(skb);
				rctx->wqe_active.rqe_md.rx_skb_hash = skb_get_hash(skb);

				rv = siw_rdmap_complete(qp, 0, !qp->notify_on_wq); //omril: call siw_sqe_complete
				if (qp->notify_on_wq) {
					u8 opcode = __rdmap_opcode(&rctx->hdr.ctrl);
					switch (opcode) {
					case RDMAP_RDMA_READ_RESP:
					case RDMAP_RDMA_ATOMIC_RESP:
						/* Completes on SCQ */
						siw_schedule_cq_notify_work(qp, qp->scq);
						break;
					case RDMAP_SEND_SE:
					case RDMAP_SEND_SE_INVAL:
					case RDMAP_SEND:
					case RDMAP_SEND_INVAL:
						/* Completes on RCQ */
						siw_schedule_cq_notify_work(qp, qp->rcq);
						break;
					default:
						break;
					}
				}
				else {
					unsigned long flags;
					/* Check for RX suspend (we just called ULP) */
					lock_rq_rxsave(qp, flags);
					if (qp->rx_ctx.rx_suspend) {
						unlock_rq_rxsave(qp, flags);
						dprint(DBG_RX | DBG_ON, 
						       "(QP%d): RX suspended\n", QP_ID(qp));
						rv = -ESHUTDOWN;
						goto out;
					}
					unlock_rq_rxsave(qp, flags);
				}

#if defined(SIW_TX_COMP_WAIT_ACK) && 0
				/*
				 * In case of delayed ACK, force ACK now so transmitter knows we have processed this WQE
				 */
#	if KS_HAS___TCP_SEND_ACK
				do {
					const struct tcphdr *th = tcp_hdr(rctx->skb);
					u32 end_fpdu_seq = ntohl(th->seq) + rctx->skb_offset;
					__tcp_send_ack(qp->attrs.llp_stream_handle->sk, end_fpdu_seq);
				} while (0);
#	else
				tcp_send_ack(qp->attrs.llp_stream_handle->sk);
#	endif
#endif

				run_completion = 0;
			}
			break;
		case SIW_WAIT_RQE:
			/* Can't receive any more, waiting for RQE */
			return -EBUSY;

		default:
			pr_warn("QP[%d]: RX out of state\n", QP_ID(qp));
			rv = -EPROTO;
			run_completion = 0;
		}

		if (rctx->mpa_crc_hd)
			siw_rx_debug_crc(rctx, rctx->state == SIW_GET_TRAILER && rv == -EINVAL);

		if (unlikely(rv != 0 && rv != -EAGAIN)) {
			/*
			 * TODO: implement graceful error handling including
			 *       generation (and processing) of TERMINATE
			 *       messages.
			 *
			 *	 for now we are left with a bogus rx status
			 *	 unable to receive any further byte.
			 *	 BUT: code must handle difference between
			 *	 errors:
			 *
			 *	 o protocol syntax (FATAL, framing lost)
			 *	 o crc	(FATAL, framing lost since we do not
			 *	        trust packet header (??))
			 *	 o local resource (maybe non fatal, framing
			 *	   not lost)
			 *
			 */
			if ((rctx->state > SIW_GET_HDR ||
				rctx->more_ddp_segs) && run_completion)
					siw_rdmap_complete(qp, rv, 1); //omril: call siw_sqe_complete

			pr_warn("(QP%d): RX ERROR %d at RX state %d\n",
				QP_ID(qp), rv, rctx->state);

			siw_dprint_rctx(rctx);

#if SIW_ENABLE_PANIC_REMOTE_ON_RX_ERR
			do {
				extern bool panic_remote_on_rx_err;
				if (panic_remote_on_rx_err) {
					/* Send OOB message to remote */
					char oob = 'X';
					struct kvec	iov = {
						.iov_base = &oob,
						.iov_len = sizeof(oob),
					};
					struct msghdr	msg = {
						.msg_name = NULL,
						.msg_flags = MSG_OOB,
					};
					kernel_sendmsg_locked(qp->attrs.llp_stream_handle->sk,
						&msg, &iov, 1, sizeof(oob));
					WARN_ON_ONCE(1);
					if (panic_on_rx_err)
						msleep(200); /* Give the remote time to receive the OOB msg */
				}
			} while (0);
#endif
			if (panic_on_rx_err)
				BUG_ON(1);
			else
				WARN_ON_ONCE(1);
			/* 
			 * Calling siw_cm_queue_work() is safe without
			 * releasing qp->state_lock because the QP state
			 * will be transitioned to SIW_QP_STATE_ERROR
			 * by the siw_work_handler() workqueue handler
			 * after we return from siw_qp_llp_data_ready().
			 */
			siw_qp_cm_drop(qp, 1);

			break;
		}
		if (rv) {
			dprint(DBG_RX, "(QP%d): "
				"Misaligned FPDU: State: %d, missing: %d\n",
				QP_ID(qp), rctx->state, rctx->fpdu_part_rem);
			break;
		}
	}
out:
	return rctx->skb_copied;
}
