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

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <linux/llist.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/tcp.h>
#include <linux/cpu.h>

#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"

#ifdef USE_SQ_KTHREAD
#include <linux/kthread.h>

extern struct task_struct *qp_tx_thread[];
extern int qp_tx_vector_cpu[];
extern int num_tx_vector;
extern int default_tx_cpu;
#endif

static bool zcopy_tx = 1;
module_param(zcopy_tx, bool, 0644);
MODULE_PARM_DESC(zcopy_tx, "Zero copy user data transmit if possible (bool).");

static bool low_delay_tx = 1;
module_param(low_delay_tx, bool, 0644);
MODULE_PARM_DESC(low_delay_tx, "Run tight transmit thread loop if activated (bool).");

static bool zero_delay_tx = 1;
module_param(zero_delay_tx, bool, 0644);
MODULE_PARM_DESC(zero_delay_tx, "Run tight transmit thread loop always (bool).");

#if DPRINT_MASK > 0
extern char siw_qp_state_to_string[SIW_QP_STATE_COUNT][sizeof "TERMINATE"];
#endif

static inline int siw_crc_txhdr(struct siw_iwarp_tx *ctx)
{
	crypto_shash_init(ctx->mpa_crc_hd);
	return siw_crc_array(ctx->mpa_crc_hd, (u8 *)&ctx->pkt,
			     ctx->ctrl_len);
}

#ifndef MSG_SENDPAGE_NOTLAST
#define MSG_SENDPAGE_NOTLAST MSG_SPLICE_PAGES
#endif

//omril: 32
#define MAX_HDR_INLINE					\
	(((uint32_t)(sizeof(struct siw_rreq_pkt) -	\
	sizeof(struct iwarp_send))) & 0xF8)
	
#ifdef SIW_TX_COMP_WAIT_ACK
/* Used to verify CRC after transmission */
struct siw_iwarp_tx_fpdu {
	struct list_head link;
	union {
		union iwarp_hdrs hdr;
		char short_pkt[sizeof(struct iwarp_send) + MAX_HDR_INLINE];
	};
	struct mpa_trailer trailer;
	struct siw_wqe wqe;
	unsigned sge_idx;
	unsigned sge_off;
	u32 send_seq;
	u32 end_seq;
	bool is_short_pkt;
	bool mem_inc_ref;
};
#endif

static inline struct page *siw_get_pblpage(struct siw_mr *mr,
					   u64 addr, int *idx)
{
	struct siw_pbl *pbl = mr->pbl;
	u64 offset = addr - mr->mem.va;
	u64 paddr;

	dprint(DBG_MM,
		"krping: lookup addr=%llx (mr->mem.va=%llx, offset=%llx)\n",
		addr, mr->mem.va, offset);

	paddr = siw_pbl_get_buffer(pbl, offset, NULL, idx);

	dprint(DBG_MM,
		"krping: lookup resolved to paddr=%llx)\n",
		paddr);

	if (paddr)
		return virt_to_page(paddr);
	return NULL;
}

/*
 * Copy short payload at provided destination address and
 * update address pointer to the address behind data
 * including potential padding
 */
static int siw_try_1seg(struct siw_iwarp_tx *c_tx, char *payload)
{
	struct siw_wqe *wqe = &c_tx->wqe_active;
	struct siw_sge *sge = &wqe->sqe.sge[0];
	u32 bytes = sge->length;

	static int dbg = 0;

	//omril: payload is &pkt.crc and probably,
	//if we can send short-fpdu we dont need the rest of pkt members (after crc)
	//

	if (!dbg) {
		dbg = 1;
		dprint(DBG_ON, "dbg: MAX_HDR_INLINE=%d\n",
			MAX_HDR_INLINE);
	}

	if (bytes > MAX_HDR_INLINE || wqe->sqe.num_sge > 1) {
//		dprint(DBG_ON, "bytes=%d,wqe->sqe.num_sge=%d\n",
//			bytes, wqe->sqe.num_sge);
		return -1;
	}

	if (bytes == 0)
		return 0;
	
	BUG_ON(!wqe->sqe.num_sge);

	if (tx_flags(wqe) & SIW_WQE_INLINE)
		memcpy(payload, &wqe->sqe.sge[1], bytes);
	else {
		struct siw_mr *mr = siw_mem2mr(wqe->mem[0].obj);

		if (!mr->mem_obj) /* Kernel client using kva */
			memcpy(payload, (void *)sge->laddr, bytes);
		else if (c_tx->in_syscall) {
			if (copy_from_user(payload,
					   (void *)sge->laddr,
					   bytes)) {
				WARN_ON(1);
				return -1;
			}
		} else {
			unsigned int off =  sge->laddr & ~PAGE_MASK;
			struct page *p;
			char *buffer;
			int index = 0;

			if (!mr->mem.is_pbl)
				p = siw_get_upage(mr->umem, sge->laddr);
			else
				p = siw_get_pblpage(mr, sge->laddr, &index);

			BUG_ON(!p);
			buffer = kmap_atomic(p);

			if (likely(PAGE_SIZE - off >= bytes)) {
				memcpy(payload, buffer + off, bytes);
				kunmap_atomic(buffer);
			} else {
				unsigned long part = bytes - (PAGE_SIZE - off);

				memcpy(payload, buffer + off, part);
				kunmap_atomic(buffer);
				payload += part;

				if (!mr->mem.is_pbl)
					p = siw_get_upage(mr->umem,
							  sge->laddr + part);
				else
					p = siw_get_pblpage(mr,
							    sge->laddr + part,
							    &index);
				BUG_ON(!p);

				buffer = kmap_atomic(p);
				memcpy(payload, buffer, bytes - part);
				kunmap_atomic(buffer);
			}
		}
	}
	return (int)bytes;
}

#define PKT_FRAGMENTED 1
#define PKT_COMPLETE 0

/*
 * siw_qp_prepare_tx()
 *
 * Prepare tx state for sending out one fpdu. Builds complete pkt
 * if no user data or only immediate data are present.
 *
 * returns PKT_COMPLETE if complete pkt built, PKT_FRAGMENTED otherwise.
 */

static int siw_qp_prepare_tx(struct siw_iwarp_tx *c_tx)
{
	struct siw_wqe		*wqe = &c_tx->wqe_active;
	char 			*crc = NULL;
	int			data = 0;

	dprint(DBG_TX, "(QP%d): tx_type %x\n", TX_QPID(c_tx), tx_type(wqe));

	switch (tx_type(wqe)) {
	case SIW_OP_READ:
	case SIW_OP_READ_LOCAL_INV:
		memcpy(&c_tx->pkt.ctrl,
		       &iwarp_pktinfo[RDMAP_RDMA_READ_REQ].ctrl,
		       sizeof(struct iwarp_ctrl));

		c_tx->pkt.rreq.rsvd = 0;
		c_tx->pkt.rreq.ddp_qn = htonl(RDMAP_UNTAGGED_QN_RDMA_READ);
		c_tx->pkt.rreq.ddp_msn =
			htonl(++c_tx->ddp_msn[RDMAP_UNTAGGED_QN_RDMA_READ]);
		c_tx->pkt.rreq.ddp_mo = 0;
		c_tx->pkt.rreq.sink_stag = htonl(wqe->sqe.sge[0].lkey);
		c_tx->pkt.rreq.sink_to =
			cpu_to_be64(wqe->sqe.sge[0].laddr); /* abs addr! */
		c_tx->pkt.rreq.source_stag = htonl(wqe->sqe.rkey);
		c_tx->pkt.rreq.source_to = cpu_to_be64(wqe->sqe.raddr);
		c_tx->pkt.rreq.read_size = htonl(wqe->sqe.sge[0].length);

		dprint(DBG_TX, ": RREQ: Sink: %x, 0x%016llx\n",
			wqe->sqe.sge[0].lkey, wqe->sqe.sge[0].laddr);

		c_tx->ctrl_len = sizeof(struct iwarp_rdma_rreq);
		crc = (char *)&c_tx->pkt.rreq_pkt.crc;
		break;

	case SIW_OP_COMP_AND_SWAP:
	case SIW_OP_MASKED_COMP_AND_SWAP:
		memcpy(&c_tx->pkt.ctrl,
		       &iwarp_pktinfo[RDMAP_RDMA_ATOMIC_REQ].ctrl,
		       sizeof(struct iwarp_ctrl));

		c_tx->pkt.areq.rsvd = 0;
		c_tx->pkt.areq.ddp_qn = htonl(RDMAP_UNTAGGED_QN_RDMA_ATOMIC);
		c_tx->pkt.areq.ddp_msn =
			htonl(++c_tx->ddp_msn[RDMAP_UNTAGGED_QN_RDMA_ATOMIC]);
		c_tx->pkt.areq.ddp_mo = 0;
		c_tx->pkt.areq.sink_stag = htonl(wqe->sqe.sge[0].lkey);
		c_tx->pkt.areq.sink_to =
			cpu_to_be64(wqe->sqe.sge[0].laddr); /* abs addr! */
		c_tx->pkt.areq.source_stag = htonl(wqe->sqe.rkey);
		c_tx->pkt.areq.source_to = cpu_to_be64(wqe->sqe.raddr);
		//c_tx->pkt.areq.read_size = htonl(wqe->sqe.sge[0].length);

		c_tx->pkt.areq.compare_add = cpu_to_be64(wqe->sqe.compare_add);
		c_tx->pkt.areq.compare_add_mask = cpu_to_be64(wqe->sqe.compare_add_mask);
		c_tx->pkt.areq.swap = cpu_to_be64(wqe->sqe.swap);
		c_tx->pkt.areq.swap_mask = cpu_to_be64(wqe->sqe.swap_mask);

		if (tx_type(wqe) != SIW_OP_MASKED_COMP_AND_SWAP) {
			BUG_ON(wqe->sqe.compare_add_mask != ~(u64)0);
			BUG_ON(wqe->sqe.swap_mask != ~(u64)0);
		}

		dprint(DBG_TX|DBG_ATOMIC, ": AREQ: Sink: %x, 0x%016llx\n",
			wqe->sqe.sge[0].lkey, wqe->sqe.sge[0].laddr);

		c_tx->ctrl_len = sizeof(struct iwarp_rdma_areq);
		crc = (char *)&c_tx->pkt.areq_pkt.crc;

		//omril: why READ (and here) we dont call siw_try_1seg()?
		break;

	case SIW_OP_SEND: /* omril: siw_qp_prepare_tx */
		if (tx_flags(wqe) & SIW_WQE_SOLICITED)
			memcpy(&c_tx->pkt.ctrl,
			       &iwarp_pktinfo[RDMAP_SEND_SE].ctrl,
			       sizeof(struct iwarp_ctrl));
		else
			memcpy(&c_tx->pkt.ctrl,
			       &iwarp_pktinfo[RDMAP_SEND].ctrl,
			       sizeof(struct iwarp_ctrl));

		c_tx->pkt.send.ddp_qn = RDMAP_UNTAGGED_QN_SEND;
		c_tx->pkt.send.ddp_msn =
			htonl(++c_tx->ddp_msn[RDMAP_UNTAGGED_QN_SEND]);
		c_tx->pkt.send.ddp_mo = 0;

		c_tx->pkt.send_inv.inval_stag = 0;

		c_tx->ctrl_len = sizeof(struct iwarp_send);

		crc = (char *)&c_tx->pkt.send_pkt.crc;
		data = siw_try_1seg(c_tx, crc);
		break;

	case SIW_OP_SEND_REMOTE_INV:
		if (tx_flags(wqe) & SIW_WQE_SOLICITED)
			memcpy(&c_tx->pkt.ctrl,
			       &iwarp_pktinfo[RDMAP_SEND_SE_INVAL].ctrl,
			       sizeof(struct iwarp_ctrl));
		else
			memcpy(&c_tx->pkt.ctrl,
			       &iwarp_pktinfo[RDMAP_SEND_INVAL].ctrl,
			       sizeof(struct iwarp_ctrl));

		c_tx->pkt.send.ddp_qn = RDMAP_UNTAGGED_QN_SEND;
		c_tx->pkt.send.ddp_msn =
			htonl(++c_tx->ddp_msn[RDMAP_UNTAGGED_QN_SEND]);
		c_tx->pkt.send.ddp_mo = 0;

		c_tx->pkt.send_inv.inval_stag = cpu_to_be32(wqe->sqe.rkey);

		c_tx->ctrl_len = sizeof(struct iwarp_send_inv);

		crc = (char *)&c_tx->pkt.send_pkt.crc;
		data = siw_try_1seg(c_tx, crc);
		break;

	case SIW_OP_WRITE:
		memcpy(&c_tx->pkt.ctrl, &iwarp_pktinfo[RDMAP_RDMA_WRITE].ctrl,
		       sizeof(struct iwarp_ctrl));
		if (c_tx->ack_signal_wr && wqe->sqe.flags & SIW_WQE_SIGNALLED)
			c_tx->pkt.ctrl.ddp_rdmap_ctrl |= DDP_FLAG_WR_ACK;

		c_tx->pkt.rwrite.sink_stag = htonl(wqe->sqe.rkey);
		c_tx->pkt.rwrite.sink_to = cpu_to_be64(wqe->sqe.raddr);
		c_tx->ctrl_len = sizeof(struct iwarp_rdma_write);

		crc = (char *)&c_tx->pkt.write_pkt.crc;
		data = siw_try_1seg(c_tx, crc);
		break;

	case SIW_OP_WRITE_RESPONSE:
	case SIW_OP_READ_RESPONSE:
		memcpy(&c_tx->pkt.ctrl,
		       &iwarp_pktinfo[RDMAP_RDMA_READ_RESP].ctrl,
		       sizeof(struct iwarp_ctrl));
		if (tx_type(wqe) == SIW_OP_WRITE_RESPONSE) {
			c_tx->pkt.ctrl.ddp_rdmap_ctrl |= DDP_FLAG_WR_ACK;
			dprint(DBG_TX, "(QP%d): WRESP with rkey: %x raddr: %llu\n",
			       TX_QPID(c_tx), wqe->sqe.rkey, wqe->sqe.raddr);
		}

		/* NBO */
		c_tx->pkt.rresp.sink_stag = cpu_to_be32(wqe->sqe.rkey);
		c_tx->pkt.rresp.sink_to = cpu_to_be64(wqe->sqe.raddr);

		c_tx->ctrl_len = sizeof(struct iwarp_rdma_rresp);

		dprint(DBG_TX, ": RRESP: Sink: %x, 0x%016llx\n",
			wqe->sqe.rkey, wqe->sqe.raddr);

		crc = (char *)&c_tx->pkt.write_pkt.crc; //omril: BUG?? write_pkt?!
		data = siw_try_1seg(c_tx, crc);

		break;

	case SIW_OP_COMP_AND_SWAP_RESPONSE:

		dprint(DBG_ATOMIC, "SIW_ATOMIC: (QP%d): Process (and Tx) ARESP: cmp=%016llx/%016llx, xchg=%016llx/%016llx\n",
		  TX_QPID(c_tx),
			   wqe->sqe.compare_add, wqe->sqe.compare_add_mask,
			   wqe->sqe.swap, wqe->sqe.swap_mask);

		memcpy(&c_tx->pkt.ctrl,
		       &iwarp_pktinfo[RDMAP_RDMA_ATOMIC_RESP].ctrl,
		       sizeof(struct iwarp_ctrl));

		c_tx->pkt.aresp.sink_stag = cpu_to_be32(wqe->sqe.rkey);
		c_tx->pkt.aresp.sink_to = cpu_to_be64(wqe->sqe.raddr);
		BUG_ON(!(wqe->sqe.flags & SIW_WQE_INLINE));
		BUG_ON(wqe->sqe.sge[0].length != sizeof(u64));
		BUG_ON(wqe->sqe.sge[0].laddr != (u64)&wqe->sqe.sge[1]);

		c_tx->ctrl_len = sizeof(struct iwarp_rdma_aresp);

		dprint(DBG_TX|DBG_ATOMIC, "(QP%d): ARESP: Sink: %x, 0x%016llx\n",
		       TX_QPID(c_tx), wqe->sqe.rkey, wqe->sqe.raddr);

		crc = (char *)&c_tx->pkt.aresp_pkt.crc;
		data = siw_try_1seg(c_tx, crc);
		break;

	default:
		dprint(DBG_ON, "Unsupported WQE type %d\n", tx_type(wqe));
		BUG();
		break;
	}
	c_tx->ctrl_sent = 0;

	if (data >= 0) {
		if (data > 0) {
			wqe->processed = data;

			c_tx->pkt.ctrl.mpa_len =
				htons(c_tx->ctrl_len + data - MPA_HDR_SIZE);

			/* compute eventual pad */
			data += -(int)data & 0x3;
			/* point CRC after data or pad */
			crc += data;
			c_tx->ctrl_len += data;

			if (!(c_tx->pkt.ctrl.ddp_rdmap_ctrl & DDP_FLAG_TAGGED))
				c_tx->pkt.c_untagged.ddp_mo = 0;
			else
				c_tx->pkt.c_tagged.ddp_to =
				    cpu_to_be64(wqe->sqe.raddr);
		}

		*(u32 *)crc = MPA_CRC_OFF_MAGIC;
		/*
		 * Do complete CRC if enabled and short packet
		 */
		if (c_tx->mpa_crc_hd) {
			if (siw_crc_txhdr(c_tx) != 0)
				return -EINVAL;
			crypto_shash_final(c_tx->mpa_crc_hd, (u8 *)crc);
		}
		c_tx->ctrl_len += MPA_CRC_SIZE;
		
#ifdef SIW_TX_COMP_WAIT_ACK
		/* New short packet (ie one fpdu per wqe).
		 *
		 * fpdu_in_prog must have been pre-allocated by the caller
		 * (siw_qp_sq_proc_tx) before any protocol-state mutation.
		 * This guarantees we never silently lose WAIT_ACK bookkeeping
		 * under memory pressure: the caller bails with -EAGAIN long
		 * before we get here, and siw_run_sq reschedules the QP.
		 */
		BUG_ON(!c_tx->fpdu_in_prog);
		memcpy(c_tx->fpdu_in_prog->short_pkt, &c_tx->pkt, sizeof(c_tx->fpdu_in_prog->short_pkt));
		c_tx->fpdu_in_prog->wqe = *wqe;
		c_tx->fpdu_in_prog->is_short_pkt = true;
#endif
		return PKT_COMPLETE;
	}
	else {
		//omril: BUG? added return error - seems like caller
		//does not need the rest of the code here to execute
//		dprint(DBG_ON, "siw_try_1seg() returned %d\n", data);
//		return -EINVAL; //data;


		//omril: caller ignore errors!!! how to return error in atomic?
	}

	c_tx->ctrl_len += MPA_CRC_SIZE;
	c_tx->sge_idx = 0;
	c_tx->sge_off = 0;

	/*
	 * Allow direct sending out of user buffer if WR is non signalled
	 * and payload is over threshold and no CRC is enabled.
	 * Per RDMA verbs, the application should not change the send buffer
	 * until the work completed. In iWarp, work completion is only
	 * local delivery to TCP. TCP may reuse the buffer for
	 * retransmission. Changing unsent data also breaks the CRC,
	 * if applied.
	 */
	if (zcopy_tx
	    && wqe->bytes >= SENDPAGE_THRESH
	    && !(tx_flags(wqe) & SIW_WQE_SIGNALLED)
	    && tx_type(wqe) != SIW_OP_READ
	    && tx_type(wqe) != SIW_OP_READ_LOCAL_INV
		&& tx_type(wqe) != SIW_OP_COMP_AND_SWAP
		&& tx_type(wqe) != SIW_OP_MASKED_COMP_AND_SWAP) //omril: correct?
		c_tx->use_sendpage = 1;
	else
		c_tx->use_sendpage = 0;

	return PKT_FRAGMENTED;
}

/*
 * Send out one complete FPDU. Used for fixed sized packets like
 * Read Requests or zero length SENDs, WRITEs, READ.responses.
 * Also used for pushing an FPDU hdr only.
 */
static inline int siw_tx_ctrl(struct siw_iwarp_tx *c_tx, struct socket *s,
			      int flags)
{
	struct msghdr msg = {.msg_flags = flags};
	struct kvec iov = {
		.iov_base = (char *)&c_tx->pkt.ctrl + c_tx->ctrl_sent,
		.iov_len = c_tx->ctrl_len - c_tx->ctrl_sent};
	unsigned long start_send_jif = jiffies;
	int rv = kernel_sendmsg(s, &msg, &iov, 1,
				c_tx->ctrl_len - c_tx->ctrl_sent);
	unsigned long send_jif = jiffies - start_send_jif;

	if (send_jif > SIW_KERNEL_SENDMSG_LOG_TIMEOUT) {
		dprint(DBG_ON, "(QP%d): kernel_sendmsg took %u ms\n", TX_QPID(c_tx), jiffies_to_msecs(send_jif));
		SIW_TIMEOUT_WARN_ON_ONCE(send_jif, SIW_KERNEL_SENDMSG_WARN_TIMEOUT);
	}

	dprint(DBG_TX, " (QP%d): op=%d, %d of %d sent (%d)\n",
		TX_QPID(c_tx), __rdmap_opcode(&c_tx->pkt.ctrl),
		c_tx->ctrl_sent + rv, c_tx->ctrl_len, rv);

	if (rv >= 0) {
		c_tx->ctrl_sent += rv;

		if (c_tx->ctrl_sent == c_tx->ctrl_len) {
			siw_dprint_hdr(&c_tx->pkt.hdr, TX_QPID(c_tx),
					"CTRL sent");
			if (!(flags & MSG_MORE)) {
				if (!c_tx->mpa_crc_hd) {
					BUG_ON(*(__be32 *)(iov.iov_base + iov.iov_len - 4) != MPA_CRC_OFF_MAGIC);
				}
				c_tx->new_tcpseg = 1;
			}
			rv = 0;
		} else if (c_tx->ctrl_sent < c_tx->ctrl_len)
			rv = -EAGAIN;
		else
			BUG();
	}
	return rv;
}


static bool tx_flags_from_upstream = false;
module_param(tx_flags_from_upstream, bool, 0644);
MODULE_PARM_DESC(tx_flags_from_upstream, "Tx flags from upstream...");


static bool tx_flags_use_eor = false;
module_param(tx_flags_use_eor, bool, 0644);
MODULE_PARM_DESC(tx_flags_use_eor, "Tx flags from upstream use EOR...");

#if !KS_HAS_TCP_SENDPAGE
static int tcp_sendpage(struct socket *sock, struct page *page,
			     int offset, size_t size, int more)
{
	struct msghdr msg = {
		.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL | more,
	};
	struct bio_vec bvec;
	int ret;

	/*
	 * MSG_SPLICE_PAGES cannot properly handle pages with page_count == 0,
	 * we need to fall back to sendmsg if that's the case.
	 *
	 * Same goes for slab pages: skb_can_coalesce() allows
	 * coalescing neighboring slab objects into a single frag which
	 * triggers one of hardened usercopy checks.
	 */
	if (sendpage_ok(page))
		msg.msg_flags |= MSG_SPLICE_PAGES;

	bvec_set_page(&bvec, page, size, offset);
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, size);

	ret = sock_sendmsg(sock, &msg);
	if (ret == -EAGAIN)
		ret = 0;

	return ret;
}
#endif

/*
 * 0copy TCP transmit interface.
 *
 * Push page array page by page or in one shot.
 * Pushing the whole page array requires the inner do_tcp_sendpages
 * function to be exported by the kernel.
 */
static int siw_tcp_sendpages(struct socket *s, struct page **page,
			     int offset, size_t size, int last_flags)
{
	int i, rv = 0;
	size_t todo = size;

	for (i = 0; size > 0; i++) {
		size_t bytes = min_t(size_t, PAGE_SIZE - offset, size);
		int flags;

		if (tx_flags_from_upstream) {
			flags = MSG_DONTWAIT | MSG_MORE | MSG_SENDPAGE_NOTLAST;
			//if (bytes < size) flags = MSG_MORE | MSG_DONTWAIT | (tx_flags_use_eor ? MSG_EOR : 0);
			//if (size <= PAGE_SIZE) flags = MSG_MORE | MSG_DONTWAIT | (tx_flags_use_eor ? MSG_EOR : 0);
			//if (size <= PAGE_SIZE) flags = MSG_DONTWAIT | (tx_flags_use_eor ? MSG_EOR : 0);
			if (size <= PAGE_SIZE)
				flags = last_flags;
		}
		else {
			flags = MSG_DONTWAIT | MSG_MORE;
			//if (bytes < size) flags |= MSG_MORE;
			if (bytes <= size)
				flags = last_flags;
		}

		BUG_ON(page_count(page[i]) < 1);
#if KS_HAS_TCP_SENDPAGE
		rv = tcp_sendpage(s->sk, page[i], offset, bytes, flags);
#else
		rv = tcp_sendpage(s, page[i], offset, bytes, flags);
#endif
		if (rv <= 0)
			break;

		size -= rv;

		if (rv != bytes)
			break;

		offset = 0;
	}
	if (rv >= 0 || rv == -EAGAIN)
		rv = todo - size;

	return rv;
}

/*
 * siw_0copy_tx()
 *
 * Pushes list of pages to TCP socket. If pages from multiple
 * SGE's, all referenced pages of each SGE are pushed in one
 * shot.
 */
static int siw_0copy_tx(struct socket *s, struct page **page,
			struct siw_sge *sge, unsigned int offset,
			unsigned int size, struct page *trl_page, 
			unsigned int trl_page_len, int new_tcpseg)
{
	int i = 0, sent = 0, rv;
	int sge_bytes = min(sge->length - offset, size);
	/* Flags for the last page of siw_tcp_sendpages - We have to send the trailer, so set MSG_MORE and dont set MSG_EOR. If we are using siw_tcp_sendpages for the trailer, also set MSG_SENDPAGE_NOTLAST */
	int sp_last_pg_flags = MSG_DONTWAIT | MSG_MORE | 
		(tx_flags_from_upstream ? MSG_SENDPAGE_NOTLAST : 0);
	int trl_flags = MSG_DONTWAIT;
	
	if (!new_tcpseg) {
		/* More WQEs coming, try to keep within the same aggregation */
		trl_flags |= MSG_MORE;
		if (tx_flags_from_upstream)
			trl_flags |= MSG_SENDPAGE_NOTLAST;
	} else {
		/* TCP Segment ends with this WQE */
		if (tx_flags_from_upstream)
			trl_flags |= MSG_EOR;
	}

	offset  = (sge->laddr + offset) & ~PAGE_MASK;

	while (sent != size) {

		rv = siw_tcp_sendpages(s, &page[i], offset, sge_bytes, sp_last_pg_flags);
		if (rv >= 0) {
			sent += rv;
			if (size == sent || sge_bytes > rv)
				break;

			i += PAGE_ALIGN(sge_bytes + offset) >> PAGE_SHIFT;
			sge++;
			sge_bytes = min(sge->length, size - sent);
			offset = sge->laddr & ~PAGE_MASK;
		} else {
			sent = rv;
			break;
		}
	}
	if (size == sent && trl_page) {
		/* Sending trailer using tcp_sendpage to prevent starting a new segment just for the trailer */
		rv = siw_tcp_sendpages(s, &trl_page, 0, trl_page_len, trl_flags);
		if (rv >= 0)
			sent += rv;
		else
			sent = rv;
	}
	return sent;
}

#define MAX_TRAILER (MPA_CRC_SIZE + 4)

/*
 * siw_tx_hdt() tries to push a complete packet to TCP where all
 * packet fragments are referenced by the elements of one iovec.
 * For the data portion, each involved page must be referenced by
 * one extra element. All sge's data can be non-aligned to page
 * boundaries. Two more elements are referencing iWARP header
 * and trailer:
 * MAX_ARRAY = 64KB/PAGE_SIZE + 1 + (2 * (SIW_MAX_SGE - 1) + HDR + TRL
 */
#define MAX_ARRAY ((0xffff / PAGE_SIZE) + 1 + (2 * (SIW_MAX_SGE - 1) + 2))

/*
 * Write out iov referencing hdr, data and trailer of current FPDU.
 * Update transmit state dependent on write return status
 */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-larger-than"
#endif
static int siw_tx_hdt(struct siw_iwarp_tx *c_tx, struct socket *s)
{
	struct siw_wqe		*wqe = &c_tx->wqe_active;
	struct siw_sge		*sge = &wqe->sqe.sge[c_tx->sge_idx],
				*first_sge = sge;
	union siw_mem_resolved	*mem = &wqe->mem[c_tx->sge_idx];
	struct siw_mr		*mr = NULL;

	struct kvec		iov[MAX_ARRAY];
	struct page		*page_array[MAX_ARRAY];
	struct msghdr		msg = {.msg_flags = MSG_DONTWAIT};

	int			seg = 0, do_crc = c_tx->do_crc, is_kva = 0, rv, is_kva_vm = 0;
	unsigned int		data_len = c_tx->bytes_unsent,
				hdr_len = 0,
				trl_len = 0,
				sge_off = c_tx->sge_off,
				sge_idx = c_tx->sge_idx;
	unsigned long		start_send_jif, sent_jif;

	if (tx_flags_from_upstream && siw_sq_empty(TX_QP(c_tx)) && !tx_more_wqe(TX_QP(c_tx), wqe))
		msg.msg_flags |= MSG_EOR;

	if (c_tx->state == SIW_SEND_HDR) {
		if (c_tx->use_sendpage) {
			dprint(DBG_ATOMIC, "call siw_tx_ctrl ...\n");
			rv = siw_tx_ctrl(c_tx, s, MSG_DONTWAIT|MSG_MORE);
			if (rv)
				goto done;

			c_tx->state = SIW_SEND_DATA;
		} else {
			iov[0].iov_base =
				(char *)&c_tx->pkt.ctrl + c_tx->ctrl_sent;
			iov[0].iov_len = hdr_len =
				c_tx->ctrl_len - c_tx->ctrl_sent;
			seg = 1;
			siw_dprint_hdr(&c_tx->pkt.hdr, TX_QPID(c_tx),
					"HDR to send: ");
		}
	}

	wqe->processed += data_len;

	while (data_len) { /* walk the list of SGE's */
		unsigned int	sge_len = min(sge->length - sge_off, data_len);
		unsigned int	fp_off = (sge->laddr + sge_off) & ~PAGE_MASK;
		int pbl_idx = 0;

		BUG_ON(!sge_len);

		is_kva = 0;
		if (!(tx_flags(wqe) & SIW_WQE_INLINE)) {
			mr = siw_mem2mr(mem->obj);
			if (!mr->mem_obj)
				is_kva = 1;
		} else
			is_kva = 1;

		if (is_kva && !c_tx->use_sendpage) {
			/*
			 * tx from kernel virtual address: either inline data
			 * or memory region with assigned kernel buffer
			 */
			iov[seg].iov_base = (void *)(sge->laddr + sge_off);
			iov[seg].iov_len = sge_len;

			if (do_crc)
				siw_crc_array(c_tx->mpa_crc_hd,
					      iov[seg].iov_base, sge_len);
			sge_off += sge_len;
			data_len -= sge_len;
			seg++;
			goto sge_done;
		}

		if (is_kva)
			is_kva_vm = is_vmalloc_addr((const void *)sge->laddr);

		while (sge_len) {
			size_t plen = min((int)PAGE_SIZE - fp_off, sge_len);

			BUG_ON(plen <= 0);
			if (!is_kva) {
				struct page *p;
				if (mr->mem.is_pbl)
					p = siw_get_pblpage(mr,
						sge->laddr + sge_off,
						&pbl_idx);
				else
					p = siw_get_upage(mr->umem, sge->laddr
							  + sge_off);
				BUG_ON(!p);
				page_array[seg] = p;

				if (!c_tx->use_sendpage) {
					iov[seg].iov_base = kmap(p) + fp_off;
					iov[seg].iov_len = plen;
				}
				if (do_crc)
					siw_crc_page(c_tx->mpa_crc_hd, p,
						     fp_off, plen);
			} else {
				if (!is_kva_vm) {
					u64 pa = ((sge->laddr + sge_off) & PAGE_MASK);
					page_array[seg] = virt_to_page(pa);
				} else {
					page_array[seg] = vmalloc_to_page((const void *)(sge->laddr + sge_off));
				}
				if (do_crc)
					siw_crc_array(c_tx->mpa_crc_hd,
						(void *)(sge->laddr + sge_off),
						plen);
			}

			sge_len -= plen;
			sge_off += plen;
			data_len -= plen;
			fp_off = 0;

			if (++seg > (int)MAX_ARRAY) {
				dprint(DBG_ON, "(QP%d): Too many fragments\n",
				       TX_QPID(c_tx));
				if (!is_kva && !c_tx->use_sendpage) {
					int i = (hdr_len > 0) ? 1 : 0;
					seg--;
					while (i < seg)
						kunmap(page_array[i++]);
				}
				wqe->processed -= c_tx->bytes_unsent;
				rv = -EMSGSIZE;
				goto done_crc;
			}
		}
sge_done:
		/* Update SGE variables at end of SGE */
		if (sge_off == sge->length &&
		    (data_len != 0 || wqe->processed < wqe->bytes)) {
			sge_idx++;
			sge++;
			mem++;
			sge_off = 0;
		}
	}
	/* trailer */
	if (likely(c_tx->state != SIW_SEND_TRAILER)) {
		iov[seg].iov_base = &c_tx->trailer.pad[4 - c_tx->pad];
		iov[seg].iov_len = trl_len = MAX_TRAILER - (4 - c_tx->pad);
	} else {
		/* [NVMESH-4390] : Make UBSAN happy.
		* iov[seg].iov_base = &c_tx->trailer.pad[c_tx->ctrl_sent];
		*/
		BUG_ON(c_tx->ctrl_sent >= MAX_TRAILER);
		iov[seg].iov_base = ((u8 *)&c_tx->trailer + c_tx->ctrl_sent);
		iov[seg].iov_len = trl_len = MAX_TRAILER - c_tx->ctrl_sent;
	}

	if (c_tx->pad) {
		*(u32 *)c_tx->trailer.pad = 0;
		if (do_crc)
			siw_crc_array(c_tx->mpa_crc_hd,
				      (u8 *)&c_tx->trailer.crc - c_tx->pad,
				      c_tx->pad);
	}
	if (!c_tx->mpa_crc_hd)
		c_tx->trailer.crc = MPA_CRC_OFF_MAGIC;
	else if (do_crc)
		crypto_shash_final(c_tx->mpa_crc_hd,
				   (u8 *)&c_tx->trailer.crc);

	data_len = c_tx->bytes_unsent;

	if (c_tx->tcp_seglen >= (int)MPA_MIN_FRAG && tx_more_wqe(TX_QP(c_tx), wqe)) {
		msg.msg_flags |= MSG_MORE;
		c_tx->new_tcpseg = 0;
	} else
		c_tx->new_tcpseg = 1;

	//omril: finally, the actuall send!!!
	start_send_jif = jiffies;
	if (c_tx->use_sendpage) {
		struct page *trl_pg = NULL;
		unsigned int trl_pg_len = 0;
		if (!c_tx->mpa_crc_hd && (data_len % PAGE_SIZE == 0) && data_len > 0) {
			/* Use the trailer page instead of an additional sendmsg */
			/* Note there is no pad, because data_len is a multiple of the PAGE_SIZE */
			trl_pg = c_tx->trailer_page;
			trl_pg_len = sizeof(c_tx->trailer.crc);
		}
		rv = siw_0copy_tx(s, page_array, first_sge, c_tx->sge_off,
				  data_len, trl_pg, trl_pg_len, c_tx->new_tcpseg);
		if (rv == data_len + trl_pg_len) {
			if (!trl_pg_len) {
				dprint(DBG_TX, "(QP%d): sending %d trailer bytes: %*ph from " dprint_ptr_str() "\n",
				       TX_QPID(c_tx), trl_len, trl_len, iov[seg].iov_base, iov[seg].iov_base);
				rv = kernel_sendmsg(s, &msg, &iov[seg], 1, trl_len);
				if (rv > 0)
					rv += data_len;
				else
					rv = data_len;
			}
		}
		if (rv > data_len) {
			dprint(DBG_TX, "(QP%d): sent %d trailer bytes: %*ph\n",
			       TX_QPID(c_tx), rv - data_len, rv - data_len, c_tx->trailer_page_virt);
		}
	} else {
		rv = kernel_sendmsg(s, &msg, iov, seg + 1,
				    hdr_len + data_len + trl_len);
		if (!is_kva) {
			int i = (hdr_len > 0) ? 1 : 0;
			while (i < seg)
				kunmap(page_array[i++]);
		}
	}
	sent_jif = (jiffies - start_send_jif);
	if (sent_jif > SIW_KERNEL_SENDMSG_LOG_TIMEOUT) {
		dprint(DBG_ON, "(QP%d): kernel_sendmsg took %u ms\n", TX_QPID(c_tx), jiffies_to_msecs(sent_jif));
		SIW_TIMEOUT_WARN_ON_ONCE(sent_jif, SIW_KERNEL_SENDMSG_WARN_TIMEOUT);

	}
	if (rv < (int)hdr_len) {
		/* Not even complete hdr pushed or negative rv */
		wqe->processed -= data_len;
		if (rv >= 0) {
			c_tx->ctrl_sent += rv;
			rv = -EAGAIN;
		}
		goto done_crc;
	}

	rv -= hdr_len;
	
	if (!c_tx->mpa_crc_hd) {
		BUG_ON(*(__be32 *)(iov[seg].iov_base + iov[seg].iov_len - MPA_CRC_SIZE) != MPA_CRC_OFF_MAGIC);
	}

	if (rv >= (int)data_len) {
		/* all user data pushed to TCP or no data to push */
		if (data_len > 0 && wqe->processed < wqe->bytes) {
			/* Save the current state for next tx */
			c_tx->sge_idx = sge_idx;
			c_tx->sge_off = sge_off;
		}
		rv -= data_len;

		if (rv == trl_len) /* all pushed */
			rv = 0;
		else {
			if (c_tx->state != SIW_SEND_TRAILER) {
				c_tx->state = SIW_SEND_TRAILER;
				c_tx->ctrl_len = MAX_TRAILER;
				c_tx->ctrl_sent = rv + 4 - c_tx->pad;
				c_tx->bytes_unsent = 0;
			}
			dprint(DBG_TX, "(QP%d): sent %d bytes of trailer %*ph. siw_qp: " dprint_ptr_str() " tx_ctx: " dprint_ptr_str() " trl_len: %d ctrl_len: %d ctrl_sent: %d pad: %d\n", TX_QPID(c_tx), rv, rv, iov[seg].iov_base, TX_QP(c_tx), c_tx, trl_len, c_tx->ctrl_len, c_tx->ctrl_sent, c_tx->pad);
			rv = -EAGAIN;
		}
	} else if (data_len > 0) {
		/* Maybe some user data pushed to TCP */
		c_tx->state = SIW_SEND_DATA;
		wqe->processed -= data_len - rv;

		if (rv) {
			/*
			 * Some bytes out. Recompute tx state based
			 * on old state and bytes pushed
			 */
			unsigned int sge_unsent;

			c_tx->bytes_unsent -= rv;
			sge = &wqe->sqe.sge[c_tx->sge_idx];
			sge_unsent = sge->length - c_tx->sge_off;

			while (sge_unsent <= rv) {
				rv -= sge_unsent;
				c_tx->sge_idx++;
				c_tx->sge_off = 0;
				sge++;
				sge_unsent = sge->length;
			}
			c_tx->sge_off += rv;
			BUG_ON(c_tx->sge_off >= sge->length);
		}
		rv = -EAGAIN;
	}
done_crc:
	c_tx->do_crc = 0;
done:
	return rv;
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

static void siw_calculate_tcpseg(struct siw_iwarp_tx *c_tx, struct socket *s)
{
	/*
	 * refresh TCP segement len if we start a new segment or
	 * remaining segment len is less than MPA_MIN_FRAG or
	 * the socket send buffer is empty.
	 */
	if (c_tx->new_tcpseg || c_tx->tcp_seglen < (int)MPA_MIN_FRAG ||
	     !tcp_send_head(s->sk))
		c_tx->tcp_seglen = get_tcp_mss(s->sk);
}


/*
 * siw_unseg_txlen()
 *
 * Compute complete tcp payload len if packet would not
 * get fragmented
 */
static inline int siw_unseg_txlen(struct siw_iwarp_tx *c_tx)
{
	int pad = c_tx->bytes_unsent ? -c_tx->bytes_unsent & 0x3 : 0;

	return c_tx->bytes_unsent + c_tx->ctrl_len + pad + MPA_CRC_SIZE;
}


/*
 * siw_prepare_fpdu()
 *
 * Prepares transmit context to send out one FPDU if FPDU will contain
 * user data and user data are not immediate data.
 * Checks and locks involved memory segments of data to be sent.
 * Computes maximum FPDU length to fill up TCP MSS if possible.
 *
 * @qp:		QP from which to transmit
 * @wqe:	Current WQE causing transmission
 *
 * TODO: Take into account real available sendspace on socket
 *       to avoid header misalignment due to send pausing within
 *       fpdu transmission
 */
static int siw_prepare_fpdu(struct siw_qp *qp, struct siw_wqe *wqe)
{
	struct siw_iwarp_tx	*c_tx  = &qp->tx_ctx;
	int rem_max_payload = iwarp_pktinfo[__rdmap_opcode(&c_tx->pkt.ctrl)].max_payload;
	int rv = 0;

	/*
	 * TODO: TCP Fragmentation dynamics needs for further investigation.
	 *	 Resuming SQ processing may start with full-sized packet
	 *	 or short packet which resets MSG_MORE and thus helps
	 *	 to synchronize.
	 *	 This version resumes with short packet.
	 */
	c_tx->ctrl_len = iwarp_pktinfo[__rdmap_opcode(&c_tx->pkt.ctrl)].hdr_len;
	c_tx->ctrl_sent = 0;

	/*
	 * Update target buffer offset if any
	 */
	if (!(c_tx->pkt.ctrl.ddp_rdmap_ctrl & DDP_FLAG_TAGGED))
		/* Untagged message */
		c_tx->pkt.c_untagged.ddp_mo = cpu_to_be32(wqe->processed);
	else	/* Tagged message */
		c_tx->pkt.c_tagged.ddp_to =
		    cpu_to_be64(wqe->sqe.raddr + wqe->processed);

	/* First guess: one big unsegmented DDP segment */
	c_tx->bytes_unsent = wqe->bytes - wqe->processed;
	c_tx->tcp_seglen -= siw_unseg_txlen(c_tx);
	rem_max_payload -= c_tx->bytes_unsent;

	if (likely(c_tx->tcp_seglen >= 0 && rem_max_payload >= 0)) {
		/* Whole DDP segment fits into current TCP segment / Max Payload (Remaining tcp_seglen and rem_max_payload are both >= 0)*/
		c_tx->pkt.ctrl.ddp_rdmap_ctrl |= DDP_FLAG_LAST;
		c_tx->pad = -c_tx->bytes_unsent & 0x3;
	} else {
		/* Trim DDP payload to fit into current TCP segment / Max Payload - (min will return the most negative value) */
		c_tx->bytes_unsent += min(c_tx->tcp_seglen, rem_max_payload);
		c_tx->bytes_unsent &= ~0x3;
		c_tx->pad = 0;
		c_tx->pkt.ctrl.ddp_rdmap_ctrl &= ~DDP_FLAG_LAST;
	}
	c_tx->pkt.ctrl.mpa_len =
		htons(c_tx->ctrl_len + c_tx->bytes_unsent - MPA_HDR_SIZE);

	dprint(DBG_TX, "QP_ID(QP%d): qp: " dprint_ptr_str() ", mpa_len: %d, bytes_unsent: %u, wqe_bytes: %u wqe_processed: %d\n",
		   QP_ID(qp), qp, be16_to_cpu(c_tx->pkt.ctrl.mpa_len), c_tx->bytes_unsent, wqe->bytes, wqe->processed);
	
	/* Check for problems in FPDU sizing calc */
	if (unlikely(c_tx->bytes_unsent < 0 || 
			(c_tx->bytes_unsent > (wqe->bytes - wqe->processed)) ||
			(c_tx->tcp_seglen & 0x3) ||
			((be16_to_cpu(c_tx->pkt.ctrl.mpa_len) + MPA_HDR_SIZE + c_tx->pad) & 0x3))) {
		pr_warn("SIW: QP(%d/" dprint_ptr_str() "): Invalid FPDU sizing calculation - "
		"bytes_unsent: %d wqe_bytes: %u wqe_processed: %u tcp_seglen: %d mpa_len: %d pad: %d\n",
		QP_ID(qp), qp, c_tx->bytes_unsent, wqe->bytes, wqe->processed, c_tx->tcp_seglen,
		be16_to_cpu(c_tx->pkt.ctrl.mpa_len), c_tx->pad);
		WARN_ON_ONCE(1);
		rv = -EINVAL;
		goto out;
	}

#ifdef SIW_TX_FULLSEGS
	c_tx->fpdu_len =
		c_tx->ctrl_len + c_tx->bytes_unsent + c_tx->pad + MPA_CRC_SIZE;
#endif
	/*
	 * Init MPA CRC computation
	 */
	if (c_tx->mpa_crc_hd) {
		siw_crc_txhdr(c_tx);
		c_tx->do_crc = 1;
	}

#ifdef SIW_TX_COMP_WAIT_ACK
	/* fpdu_in_prog must have been pre-allocated by the caller
	 * (siw_qp_sq_proc_tx) before any protocol-state mutation; see
	 * companion comment in siw_qp_prepare_tx().
	 */
	{
		struct socket *s = qp->attrs.llp_stream_handle;
		struct tcp_sock *tp = tcp_sk(s->sk);

		BUG_ON(!c_tx->fpdu_in_prog);
		c_tx->fpdu_in_prog->hdr = c_tx->pkt.hdr;
		c_tx->fpdu_in_prog->wqe = *wqe;
		c_tx->fpdu_in_prog->sge_idx = c_tx->sge_idx;
		c_tx->fpdu_in_prog->sge_off = c_tx->sge_off;
		c_tx->fpdu_in_prog->send_seq = tp->write_seq;
		c_tx->fpdu_in_prog->end_seq = tp->write_seq +
			c_tx->ctrl_len + c_tx->bytes_unsent + c_tx->pad + MPA_CRC_SIZE - 1;
	}
#endif

out:
	return rv;
}

#ifdef SIW_TX_FULLSEGS
static inline int siw_test_wspace(struct socket *s, struct siw_iwarp_tx *c_tx)
{
	struct sock *sk = s->sk;
	int rv = 0;

	lock_sock(sk);
	if (sk_stream_wspace(sk) < (int)c_tx->fpdu_len) {
		set_bit(SOCK_NOSPACE, &s->flags);
		rv = -EAGAIN;
	}
	release_sock(sk);

	return rv;
}
#endif

/*
 * siw_check_sgl_tx()
 *
 * Check permissions for a list of SGE's (SGL)
 *
 * @pd:		Protection Domain SGL should belong to
 * @sge:	List of SGE to be checked
 * @perms:	requested access permissions
 *
 */

static int siw_check_sgl_tx(struct siw_pd *pd, struct siw_wqe *wqe,
		     enum siw_access_flags perms)
{
	struct siw_sge		*sge = &wqe->sqe.sge[0];
	union siw_mem_resolved	*mem = &wqe->mem[0];
	int	num_sge = wqe->sqe.num_sge,
		len = 0;

	dprint(DBG_WR|DBG_TX, "(PD%d): Enter (num_sge=%d)\n", OBJ_ID(pd), num_sge);

	if (unlikely(num_sge > SIW_MAX_SGE))
		return -EINVAL;

	while (num_sge-- > 0) {
		dprint(DBG_WR, "(PD%d): sge=" dprint_ptr_str() ", perms=0x%x, "
			"len=%d, sge->len=%d\n",
			OBJ_ID(pd), sge, perms, len, sge->length);
		/*
		 * rdma verbs: do not check stag for a zero length sge
		 */
		dprint(DBG_OL, "if sge->length=%d calling siw_check_sge ...\n", sge->length);
		if (sge->length &&
		    siw_check_sge(pd, sge, mem, perms, 0, sge->length) != 0) {
			len = -EINVAL;
			break;
		}
		len += sge->length;
		sge++;
		mem++;
	}
	return len;
}

/*
 * siw_qp_sq_proc_tx()
 *
 * Process one WQE which needs transmission on the wire.
 * Return with:
 *	-EAGAIN, if handover to tcp remained incomplete
 *	0,	 if handover to tcp complete
 *	< 0,	 if other errors happend.
 *
 * @qp:		QP to send from
 * @wqe:	WQE causing transmission
 */
static int siw_qp_sq_proc_tx(struct siw_qp *qp, struct siw_wqe *wqe, bool inline_tx)
{
	struct siw_iwarp_tx	*c_tx = &qp->tx_ctx;
	struct socket		*s = qp->attrs.llp_stream_handle;
	int			rv = 0,
				burst_len = qp->tx_ctx.burst;
	unsigned long flags;

	dprint(DBG_TX, "(QP%d): --> opcode %d\n", QP_ID(qp), tx_type(wqe));

	if (unlikely(wqe->wr_status == SR_WR_IDLE)) {
		WARN_ON(1);
		return 0;
	}
	if (!burst_len)
		burst_len = SQ_USER_MAXBURST;

	if (unlikely(wqe->wr_status == SR_WR_WAIT_ORQ)) {
		struct siw_sqe *rreq;

		dprint(DBG_TX, "(QP%d): SQE ID:%llx - SR_WR_WAIT_ORQ\n", QP_ID(qp), wqe->sqe.id);

		lock_orq_rxsave(qp, flags);
		if (qp->tx_ctx.orq_fence) {
			/* Still waiting on ORQ */
			dprint(DBG_TX, "(QP%d): TX: still waiting on ORQ "
				"(orq_put=%d, orq_get=%d, sqe_flags=%04x)\n",
			       QP_ID(qp), qp->orq_put, qp->orq_get, wqe->sqe.flags);
			unlock_orq_rxsave(qp, flags);
			rv = -EAGAIN;
			goto tx_done;
		}
		rreq = orq_get_tail(qp);
		if (unlikely(!rreq)) {
			pr_warn("(QP:%d) siw_qp=" dprint_ptr_str() " orq_fence=0 and ORQ is full\n",
				QP_ID(qp), qp);
			rv = -ENOENT;
			goto tx_done;
		}
		siw_read_to_orq(rreq, &wqe->sqe);
		qp->orq_put++;

		dprint(DBG_OL, "(QP%d): TX: inc orq_put to %d (orq_get=%d), flags=%04x\n",
		       QP_ID(qp), qp->orq_put, qp->orq_get, wqe->sqe.flags);

		unlock_orq_rxsave(qp, flags);
		
		wqe->wr_status = SR_WR_QUEUED;
		dprint(DBG_TX, "(QP%d): SQE ID:%llx -> SR_WR_QUEUED\n", QP_ID(qp), wqe->sqe.id);
	}

	if (wqe->wr_status == SR_WR_QUEUED) {
		dprint(DBG_TX, "SR_WR_QUEUED\n");

#ifdef SIW_TX_COMP_WAIT_ACK
		/* Pre-allocate the per-FPDU WAIT_ACK tracking struct *before*
		 * any protocol-state mutation (siw_check_sgl_tx mem-refs,
		 * wqe->wr_status, siw_qp_prepare_tx's ddp_msn++). GFP_NOWAIT
		 * is allowed to fail under memory pressure; bail with -EAGAIN
		 * so siw_run_sq reschedules the QP for a clean retry. By
		 * pulling the allocation up here we keep the call to
		 * siw_qp_prepare_tx() side-effect-clean on failure, and avoid
		 * the previous footgun where a failed kzalloc inside that
		 * function left c_tx->fpdu_in_prog NULL and oopsed
		 * qp_tx_thread/N in the short-FPDU success branch.
		 */
		BUG_ON(c_tx->fpdu_in_prog);
		c_tx->fpdu_in_prog = kzalloc(sizeof(*c_tx->fpdu_in_prog), GFP_NOWAIT);
		if (unlikely(!c_tx->fpdu_in_prog)) {
			rv = -EAGAIN;
			goto tx_done;
		}
#endif

		if (!(wqe->sqe.flags & SIW_WQE_INLINE)) {
			dprint(DBG_TX, "--- Not INLINE\n");
			if (tx_type(wqe) == SIW_OP_READ_RESPONSE ||
				tx_type(wqe) == SIW_OP_COMP_AND_SWAP_RESPONSE) {
				//omril: isnt num_sge already 1
				//dprint(DBG_ON, "num_sge=%d ...\n", wqe->sqe.num_sge);
				wqe->sqe.num_sge = 1; //???
			} else if (tx_type(wqe) == SIW_OP_WRITE_RESPONSE) {
				wqe->sqe.num_sge = 0;
			}

			if (tx_type(wqe) != SIW_OP_READ &&
			    tx_type(wqe) != SIW_OP_READ_LOCAL_INV &&
				tx_type(wqe) != SIW_OP_COMP_AND_SWAP &&
				tx_type(wqe) != SIW_OP_MASKED_COMP_AND_SWAP &&
				tx_type(wqe) != SIW_OP_WRITE_RESPONSE) {
				/*
				 * Reference memory to be tx'd
				 */
				dprint(DBG_TX, "---> check-sgl, wqe->mem[0].obj=" dprint_ptr_str() "\n", wqe->mem[0].obj);
				rv = siw_check_sgl_tx(qp->pd, wqe,
					tx_type(wqe) != SIW_OP_COMP_AND_SWAP_RESPONSE ?
					SR_MEM_LREAD :  //omril: why only these kind of permissions? what if it is write-op?
					SR_MEM_RATOMIC);
				if (rv < 0) {
					dprint(DBG_TX, "(QP%d):\n", QP_ID(qp));
					goto tx_done;
				}
				dprint(DBG_TX, "<--- check-sgl, wqe->mem[0].obj=" dprint_ptr_str() "\n", wqe->mem[0].obj);

				wqe->bytes = rv;
			} else
				wqe->bytes = 0;
		} else {
			dprint(DBG_TX, "--- INLINE\n");

			wqe->bytes = wqe->sqe.sge[0].length;
			if (!qp->kernel_verbs) {
				if (wqe->bytes > SIW_MAX_INLINE) {
					dprint(DBG_TX, "(QP%d):\n", QP_ID(qp));
					rv = -EINVAL;
					goto tx_done;
				}
				wqe->sqe.sge[0].laddr = (u64)&wqe->sqe.sge[1];
			}
		}
		wqe->wr_status = SR_WR_INPROGRESS;
		wqe->processed = 0;

		siw_calculate_tcpseg(c_tx, s);

		rv = siw_qp_prepare_tx(c_tx);
		if (rv == PKT_FRAGMENTED) {
			c_tx->state = SIW_SEND_HDR;
			rv = siw_prepare_fpdu(qp, wqe);
			if (unlikely(rv < 0))
				goto tx_done;
		} else if (rv == PKT_COMPLETE)
			c_tx->state = SIW_SEND_SHORT_FPDU; //omril: BUG? dont we override data here?
		else {
			if (rv == -EINPROGRESS || rv == -EAGAIN) {
				dprint(DBG_WR|DBG_TX,
					" QP(%d): rv from siw_qp_prepare_tx may mislead caller\n",
					QP_ID(qp));
			}
			goto tx_done;
		}
	}
	else
		dprint(DBG_TX, "wqe->wr_status=%d\n", wqe->wr_status);

next_segment:
	if (--burst_len == 0) {
		rv = -EINPROGRESS;
		goto tx_done;
	}
	dprint(DBG_WR|DBG_TX,
		" QP(%d): WR type %d, state %d, data %u, sent %u, id %llx\n",
		QP_ID(qp), tx_type(wqe), wqe->wr_status, wqe->bytes,
		wqe->processed, wqe->sqe.id);

#ifdef SIW_TX_FULLSEGS
	rv = siw_test_wspace(s, c_tx);
	if (rv < 0)
		goto tx_done;
#endif

	if (c_tx->state == SIW_SEND_SHORT_FPDU) {
		enum siw_opcode tx_type = tx_type(wqe);
		unsigned int msg_flags;

#ifdef SIW_TX_COMP_WAIT_ACK
		if (c_tx->fpdu_in_prog && c_tx->ctrl_sent == 0) {
			/* Set the sequence number from the socket */
			struct tcp_sock *tp = tcp_sk(qp->attrs.llp_stream_handle->sk);
			c_tx->fpdu_in_prog->send_seq = tp->write_seq;
			c_tx->fpdu_in_prog->end_seq = tp->write_seq + c_tx->ctrl_len - 1;
		}
#endif

		/*
		 * Always end current TCP segment (no MSG_MORE flag):
		 * trying to fill segment would result in excessive delay.
		 */
		dprint(DBG_ATOMIC, "call siw_tx_ctrl ...\n");

		if (tx_flags_from_upstream) {
			if ((siw_sq_empty(qp) || burst_len == 1) && !(tx_flags(wqe) & SIW_WQE_MORE_WQES))
					/*
					 * End current TCP segment, if SQ runs empty,
					 * or siw_tcp_nagle is not set, or we bail out
					 * soon due to no burst credit left.
					 */
					msg_flags = MSG_DONTWAIT | (tx_flags_use_eor ? MSG_EOR : 0);
			else
					msg_flags = MSG_DONTWAIT | MSG_MORE;

			rv = siw_tx_ctrl(c_tx, s, msg_flags);
		}
		else {
			rv = siw_tx_ctrl(c_tx, s, MSG_DONTWAIT);
		}

#ifdef SIW_TX_COMP_WAIT_ACK
		/* Not sure if there is a lock to prevent the response happening in parallel, but it is unlikely anyway (this is not RDMA) */
		if (!rv) {
			/* Entire small packet was sent */
			if (c_tx->fpdu_in_prog->wqe.sqe.flags & SIW_WQE_TX_TIMESTAMP) {
				c_tx->fpdu_in_prog->wqe.sqe_md.sent_time = ktime_get();
			}
			c_tx->fpdu_in_prog->wqe.sqe_md.tx_cpu = smp_processor_id();
			if (inline_tx)
				c_tx->fpdu_in_prog->wqe.sqe.flags |= SIW_WQE_INLINE_SEND;

			BUG_ON(c_tx->ctrl_sent != c_tx->ctrl_len);
			if (c_tx->fpdu_in_prog) {
				//struct siw_wqe *wqe_in_prog = &c_tx->fpdu_in_prog->wqe;
#	ifdef SIW_DEBUG_TX_CRC
				struct siw_wqe *wqe_in_prog = &c_tx->fpdu_in_prog->wqe;
				/* Inc mem refcount */
				if (likely((tx_type(wqe_in_prog) == SIW_OP_WRITE ||
							tx_type(wqe_in_prog) == SIW_OP_SEND ||
							tx_type(wqe_in_prog) == SIW_OP_SEND_WITH_IMM ||
							tx_type(wqe_in_prog) == SIW_OP_SEND_REMOTE_INV ||
							tx_type(wqe_in_prog) == SIW_OP_READ_RESPONSE ||
							tx_type(wqe_in_prog) == SIW_OP_COMP_AND_SWAP_RESPONSE) &&
							!(tx_flags(wqe_in_prog) & SIW_WQE_INLINE))) {
					int i;
					for (i = 0; i < c_tx->fpdu_in_prog->wqe.sqe.num_sge; i++) {
						struct siw_sge *sge = &wqe_in_prog->sqe.sge[i];
						struct siw_mem *mem = wqe_in_prog->mem[i].obj;
						BUG_ON(sge->laddr < mem->va || sge->laddr + sge->length > mem->va + mem->len);
						siw_mem_get(wqe_in_prog->mem[i].obj);
					}
				}
#	endif /*SIW_DEBUG_TX_CRC*/
				if (list_empty(&c_tx->sent_fpdus)) {
					union siw_iwarp_tx_sent_fpdu_notify sent_fpdu_notify = {
						.armed = 1,
						.end_seq = c_tx->fpdu_in_prog->end_seq,
					};
					BUG_ON(c_tx->sent_fpdu_notify.armed);
					smp_store_mb(c_tx->sent_fpdu_notify.all, sent_fpdu_notify.all);
				}
				list_add_tail(&c_tx->fpdu_in_prog->link, &c_tx->sent_fpdus);
				c_tx->fpdu_in_prog = NULL;
			}
		}
#endif

		if (!rv && tx_type != SIW_OP_READ &&
		    tx_type != SIW_OP_READ_LOCAL_INV &&
			tx_type != SIW_OP_COMP_AND_SWAP &&
			tx_type != SIW_OP_MASKED_COMP_AND_SWAP) //omril: correct?
			wqe->processed = wqe->bytes;

		goto tx_done;

	} else
		/* Transmit hdr, data and trailer of a single TCP segment ?! */
		rv = siw_tx_hdt(c_tx, s);

	if (!rv) {
		/* Finished sending fpdu in progress - add to sent list */
#ifdef SIW_TX_COMP_WAIT_ACK
		if (c_tx->fpdu_in_prog) {
			/* Update sqe_md */
			if (c_tx->fpdu_in_prog->wqe.sqe.flags & SIW_WQE_TX_TIMESTAMP) {
				c_tx->fpdu_in_prog->wqe.sqe_md.sent_time = ktime_get();
			}
			c_tx->fpdu_in_prog->wqe.sqe_md.tx_cpu = smp_processor_id();
			if (inline_tx)
				c_tx->fpdu_in_prog->wqe.sqe.flags |= SIW_WQE_INLINE_SEND;

			/* Copy updated trailer */
			c_tx->fpdu_in_prog->trailer = c_tx->trailer;

#	if  defined(SIW_DEBUG_TX_CRC) || defined(SIW_DEBUG_TX_CHECK_MEM_ON_ACK)
			/* If non-inline, increase mem refcount so we can check CRC or MEM validity
			 * Because this is not a "short packet", 
			 * we know already that it is a WRITE, SEND, or READ RESPONSE */
			if (!(tx_flags(&c_tx->fpdu_in_prog->wqe) & SIW_WQE_INLINE)) {
				int i;
				for (i = 0; i < c_tx->fpdu_in_prog->wqe.sqe.num_sge; i++) {
					struct siw_sge *sge = &c_tx->fpdu_in_prog->wqe.sqe.sge[i];
					struct siw_mem *mem = c_tx->fpdu_in_prog->wqe.mem[i].obj;
					BUG_ON(sge->laddr < mem->va || sge->laddr + sge->length > mem->va + mem->len);
					siw_mem_get(c_tx->fpdu_in_prog->wqe.mem[i].obj);
					c_tx->fpdu_in_prog->mem_inc_ref = true;
				}
			}
#	endif //defined(SIW_DEBUG_TX_CRC) || defined(SIW_DEBUG_TX_CHECK_MEM_ON_ACK)

			/* Add to sent list */
			if (list_empty(&c_tx->sent_fpdus)) {
				union siw_iwarp_tx_sent_fpdu_notify sent_fpdu_notify = {
					.armed = 1,
					.end_seq = c_tx->fpdu_in_prog->end_seq,
				};
				BUG_ON(c_tx->sent_fpdu_notify.armed);
				smp_store_mb(c_tx->sent_fpdu_notify.all, sent_fpdu_notify.all);
			}
			list_add_tail(&c_tx->fpdu_in_prog->link, &c_tx->sent_fpdus);
			c_tx->fpdu_in_prog = NULL;
		}
#endif
		/* Verbs, 6.4.: Try stopping sending after a full DDP segment
		 * if the connection goes down (== peer halfclose)
		 */
		if (c_tx->pkt.ctrl.ddp_rdmap_ctrl & DDP_FLAG_LAST) {
			/*
			 * One segment sent. Processing completed if last
			 * segment, Do next segment otherwise.
			 */
			dprint(DBG_TX, "(QP%d): WR completed\n", QP_ID(qp));
			goto tx_done;
		}
		c_tx->state = SIW_SEND_HDR; //omril: is this some kind of reset for next_segment (TCP segment)?
		
		lock_sq_rxsave(qp, flags);
		if (unlikely(c_tx->tx_suspend)) {
			/* QP is closing, now and we've finished the FPDU */
			unlock_sq_rxsave(qp, flags);

			dprint(DBG_ON, "(QP%d): SIW_QP_STATE_CLOSING - Last FPDU: opcode %d, wr_id %llx, fpdu_len %d, wqe_bytes: %u, wqe_processed: %u\n", 
				   QP_ID(qp), wqe->sqe.opcode, wqe->sqe.id, c_tx->fpdu_len, wqe->bytes, wqe->processed);
			rv = -ESHUTDOWN;
			goto tx_done;
		}
		unlock_sq_rxsave(qp, flags);

		siw_calculate_tcpseg(c_tx, s);

#ifdef SIW_TX_COMP_WAIT_ACK
		/* Pre-allocate for the *next* fragmented FPDU; the previous
		 * one was just added to sent_fpdus and c_tx->fpdu_in_prog
		 * cleared. No protocol state has been mutated since, so
		 * -EAGAIN here is safe to retry: siw_run_sq will reschedule
		 * the QP and we will re-enter at next_segment with the same
		 * wqe state (SR_WR_INPROGRESS, partial progress).
		 */
		BUG_ON(c_tx->fpdu_in_prog);
		c_tx->fpdu_in_prog = kzalloc(sizeof(*c_tx->fpdu_in_prog), GFP_NOWAIT);
		if (unlikely(!c_tx->fpdu_in_prog)) {
			rv = -EAGAIN;
			goto tx_done;
		}
#endif

		rv = siw_prepare_fpdu(qp, wqe);
		if (unlikely(rv < 0))
			goto tx_done;
		goto next_segment;
	}
tx_done:
	/* NOTE: we deliberately do NOT kfree(c_tx->fpdu_in_prog) here.
	 *
	 * fpdu_in_prog tracks an FPDU that may be only partially sent on
	 * the wire (e.g. siw_tx_ctrl()/siw_tx_hdt() returned -EAGAIN from
	 * the TCP socket, or we hit -EINPROGRESS at next_segment because
	 * the per-QP burst was exhausted). In those cases siw_run_sq will
	 * reschedule this QP and re-enter siw_qp_sq_proc_tx() with
	 * wqe->wr_status == SR_WR_INPROGRESS, skipping Site A and resuming
	 * transmission via c_tx->state (SIW_SEND_SHORT_FPDU / SIW_SEND_HDR
	 * / next_segment). That resume path *needs* the existing
	 * c_tx->fpdu_in_prog: it's the only place where the per-FPDU
	 * TCP seq, hdr/wqe copy and DDP/MPA framing for the in-flight
	 * packet live.
	 *
	 * Cleanup paths that actually need to release fpdu_in_prog:
	 *  - Site A / Site B kzalloc returned NULL: fpdu_in_prog is
	 *    already NULL; nothing to do.
	 *  - Successful FPDU transmission: bookkeeping branches above
	 *    move it onto c_tx->sent_fpdus and clear the pointer.
	 *  - Catastrophic error (rv < 0 and not -EAGAIN/-EINPROGRESS):
	 *    siw_qp_sq_process()'s else-arm at siw_qp_tx.c:2199-2202
	 *    frees fpdu_in_prog as part of QP teardown.
	 */
	qp->tx_ctx.burst = burst_len;
	dprint(DBG_TX, "(QP%d): <--\n", QP_ID(qp));
	return rv;
}

//omril: fast-reg
static int siw_fastreg_mr(struct siw_pd *pd, struct siw_sqe *sqe)
{
	struct siw_mem *mem = siw_mem_id2obj(pd->hdr.sdev, sqe->rkey >> 8);
	struct siw_mr *mr;
	int rv = 0;

	dprint(DBG_MM, ": STag %u (%x) Enter\n", sqe->rkey >> 8, sqe->rkey);

	if (!mem) {
		dprint(DBG_MM, ": STag %u unknown\n", sqe->rkey >> 8);
		return -EINVAL;
	}
	mr = siw_mem2mr(mem);
	if (&mr->ofa_mr != (void *)sqe->ofa_mr) {
		dprint(DBG_MM, ": STag %u: unexpected MR\n", sqe->rkey >> 8);
		rv = -EINVAL;
		goto out;
	}
	if (mr->pd != pd) {
		dprint(DBG_MM, ": PD mismatch: " dprint_ptr_str() " != " dprint_ptr_str() "\n", mr->pd, pd);
		rv = -EINVAL;
		goto out;
	}
	if (mem->stag_valid) {
		dprint(DBG_MM, ": STag already valid: %u\n",
			sqe->rkey >> 8);
		rv = -EINVAL;
		goto out;
	}
	mem->perms = sqe->access;
	mem->stag_valid = 1;
	dprint(DBG_MM, ": STag now valid: %u\n", sqe->rkey >> 8);
out:
	siw_mem_put(mem);
	return rv;
}

static int siw_qp_sq_proc_local(struct siw_qp *qp, struct siw_wqe *wqe)
{
	int rv;

	switch (tx_type(wqe)) {

	case SIW_OP_REG_MR:
		rv = siw_fastreg_mr(qp->pd, &wqe->sqe);
		break;

	case SIW_OP_INVAL_STAG:
		rv = siw_invalidate_stag(qp->pd, wqe->sqe.rkey);
		break;

	default:
		rv = -EINVAL;
	}
	return rv;
}

#ifdef SIW_TX_COMP_WAIT_ACK
int siw_qp_sq_flush_sent_fpdus(struct siw_qp *qp)
{
	int rv = 0;
	struct siw_iwarp_tx *tctx = &qp->tx_ctx;
	struct siw_iwarp_tx_fpdu *sent_fpdu;
	LIST_HEAD(sent_fpdus);
	LIST_HEAD(completed_fpdus);

	dprint(DBG_OL|DBG_ON, "(QP%d): Enter\n", QP_ID(qp));

	list_splice_init(&tctx->sent_fpdus, &sent_fpdus);
	list_splice_tail_init(&tctx->completed_fpdus, &completed_fpdus);
	kfree(tctx->fpdu_in_prog);
	tctx->fpdu_in_prog = NULL;

	dprint(DBG_OL|DBG_ON, "(QP%d): Start Flushing fpdus\n", QP_ID(qp));

	while ((sent_fpdu = list_first_entry_or_null(&sent_fpdus,
		struct siw_iwarp_tx_fpdu, link))) {
		struct siw_wqe *wqe = &sent_fpdu->wqe;
	
		if (sent_fpdu->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_LAST) {
			if (tx_flags(wqe) & SIW_WQE_VALID) {
				/* The rest are completed by ORQ flush */
				if (((tx_type(wqe) == SIW_OP_WRITE &&
					!(sent_fpdu->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_WR_ACK)) ||
					tx_type(wqe) == SIW_OP_SEND ||
					tx_type(wqe) == SIW_OP_SEND_REMOTE_INV ||
					tx_type(wqe) == SIW_OP_SEND_WITH_IMM)) {

					/* Put the ref-count for the WQE MRs */
					siw_wqe_put_mem(&sent_fpdu->wqe, tx_type(&sent_fpdu->wqe));

					dprint(DBG_OL, "(QP%d): TXTX: call siw_sqe_complete, tx_type=%x, with SIW_WC_WR_FLUSH_ERR\n",
						QP_ID(qp), tx_type(wqe));
					
					siw_sqe_complete(qp, &wqe->sqe, NULL, wqe->bytes, SIW_WC_WR_FLUSH_ERR, 0);
					rv++;
				}
			}
		}

		list_del(&sent_fpdu->link);
		kfree(sent_fpdu);
	}
	dprint(DBG_OL|DBG_ON, "(QP%d): Flushed %d sent_fdpus\n", QP_ID(qp), rv);

	/* Completed FPDUs just need to be freed */
	while((sent_fpdu = list_first_entry_or_null(&completed_fpdus,
			struct siw_iwarp_tx_fpdu, link))) {
		list_del(&sent_fpdu->link);
		kfree(sent_fpdu);
	}

	dprint(DBG_OL|DBG_ON, "(QP%d): Exit\n", QP_ID(qp));
	return rv;
}
#else
int siw_qp_sq_flush_sent_fpdus(struct siw_qp *qp)
{
	(void)qp;
	return 0;
}
#endif

#ifdef SIW_DEBUG_TX_CRC
static void check_sent_fpdu_crc(struct siw_qp *qp, struct siw_iwarp_tx_fpdu *sent_fpdu)
{
	u32 calc_crc, pkt_crc;
	int sge_off, data_len, hdr_len, data_in_sge, pad_len, fpdu_len, data_crc, sge_idx;
	struct siw_sge *sge;
	union  siw_mem_resolved *mem;
	SHASH_DESC_ON_STACK(fpdu_hd, qp->tx_ctx.mpa_crc_hd->tfm);
	struct siw_mr *mr;

	fpdu_hd->tfm = qp->tx_ctx.mpa_crc_hd->tfm;
	/* Calc lengths */
	hdr_len = iwarp_pktinfo[__rdmap_opcode(&sent_fpdu->hdr.ctrl)].hdr_len;
	data_len = be16_to_cpu(sent_fpdu->hdr.ctrl.mpa_len) - (hdr_len - MPA_HDR_SIZE);
	pad_len = (-data_len) & 0x3;
	fpdu_len = hdr_len + data_len + pad_len + MPA_CRC_SIZE;
	crypto_shash_init(fpdu_hd);
	if (sent_fpdu->is_short_pkt) {
		/* Entire packet is in context, calc CRC and finalize */
		pkt_crc = *(u32*)(sent_fpdu->short_pkt + fpdu_len - MPA_CRC_SIZE);
		data_crc = fpdu_len - MPA_CRC_SIZE;
		crypto_shash_update(fpdu_hd, sent_fpdu->short_pkt, data_crc);
		goto crc_final;
	}
	/* Recalc CRC - Header */
	pkt_crc = sent_fpdu->trailer.crc;
	crypto_shash_update(fpdu_hd, (u8 *)&sent_fpdu->hdr, hdr_len);
	data_crc = hdr_len;
	if (tx_flags(&sent_fpdu->wqe) & SIW_WQE_INLINE) {
		crypto_shash_update(fpdu_hd, (u8 *)sent_fpdu->wqe.sqe.sge, sent_fpdu->wqe.bytes);
		data_crc += sent_fpdu->wqe.bytes;
		pad_len = (-sent_fpdu->wqe.bytes) & 0x3;
		goto crc_pad;
	}
	sge_off = sent_fpdu->sge_off;
	sge_idx = sent_fpdu->sge_idx;
	sge = &sent_fpdu->wqe.sqe.sge[sent_fpdu->sge_idx];
	mem = &sent_fpdu->wqe.mem[sent_fpdu->sge_idx];
	
	while (data_len > 0) {
		if (!mem->obj) {
			pr_err("NULL mem obj from sge " dprint_ptr_str() " of sent fpdu " dprint_ptr_str() " qp " dprint_ptr_str() " addr %llu\n",
				   sge, sent_fpdu, qp, sge->laddr + sge_off);
			BUG_ON(1);
		}
		mr = siw_mem2mr(mem->obj);
		data_in_sge = min_t(int, data_len, sge->length - sge_off);
		pr_debug("#1 - sent_fpdu " dprint_ptr_str() " data_len %d data_in_sge %d sge_off %d sge " dprint_ptr_str() " sge_idx %d num_sge %d mem " dprint_ptr_str() "\n",
				 sent_fpdu, data_len, data_in_sge, sge_off, sge, sge_idx, sent_fpdu->wqe.sqe.num_sge, mem);
		BUG_ON(data_in_sge <= 0);
		if (!mr || !mr->mem_obj) {
			/* Direct kernal address */
			BUG_ON(sge->laddr + sge_off < mr->mem.va || sge->laddr + sge_off + data_in_sge > mr->mem.va + mr->mem.len);
			crypto_shash_update(fpdu_hd, (void *)sge->laddr + sge_off, data_in_sge);
			data_crc += data_in_sge;
		} else {
			struct page *pg;
			int pg_data_len, pg_off, data_left_in_sge = data_in_sge;
			int index = 0;
			void *pg_data;
			while (data_left_in_sge > 0) {
				/* Get Page from MR */
				if (!mr->mem.is_pbl)
					pg = siw_get_upage(mr->umem, sge->laddr + sge_off);
				else
					pg = siw_get_pblpage(mr, sge->laddr + sge_off, &index);
				if (!pg) {
					pr_err("Failed to get page from sge " dprint_ptr_str() " of sent fpdu " dprint_ptr_str() " qp " dprint_ptr_str() " mr " dprint_ptr_str() " addr %llu index %d is pbl %d\n",
						   sge, sent_fpdu, qp, mr, sge->laddr + sge_off, index, mr->mem.is_pbl);
					BUG_ON(1);
				}
				/* Map Page */
				pg_data = kmap_atomic(pg);
				if (!pg_data) {
					pr_err("Failed to map page " dprint_ptr_str() " from sge " dprint_ptr_str() " of sent fpdu " dprint_ptr_str() " qp " dprint_ptr_str() "\n",
						   pg, sge, sent_fpdu, qp);
					BUG_ON(1);
				}
				/* Calculate offset and data in page */
				pg_off = ((unsigned long)(sge->laddr + sge_off) & ~PAGE_MASK);
				BUG_ON(pg_off < 0 || pg_off >= PAGE_SIZE);
				pg_data_len = min_t(int, PAGE_SIZE - pg_off, data_left_in_sge);
				BUG_ON(pg_data_len < 0 || pg_data_len > PAGE_SIZE);
				BUG_ON(pg_off + pg_data_len > ((unsigned long)pg_data & PAGE_MASK) + PAGE_SIZE);
				pr_debug("#2 - data_len %d data_left_in_sge %d sge_off %d pbl_idx %d page " dprint_ptr_str() " pg_data " dprint_ptr_str() " pg_off %d pg_data_len %d\n",
						 data_len, data_left_in_sge, sge_off, index, pg, pg_data, pg_off, pg_data_len);
				/* CRC Page Data */
				crypto_shash_update(fpdu_hd, pg_data + pg_off, pg_data_len);
				data_crc += pg_data_len;
				/* Unmap page and move to next page */
				kunmap_atomic(pg_data);
				data_left_in_sge -= pg_data_len;
				BUG_ON(data_left_in_sge < 0);
				sge_off += pg_data_len;
				BUG_ON(sge_off > sge->length);
			}
		}
		pr_debug("#3 - sent_fpdu " dprint_ptr_str() " data_len %d data_in_sge %d sge_off %d sge " dprint_ptr_str() " sge_idx %d num_sge %d mem " dprint_ptr_str() "\n",
				 sent_fpdu, data_len, data_in_sge, sge_off, sge, sge_idx, sent_fpdu->wqe.sqe.num_sge, mem);
		/* Move to next SGE */
		data_len -= data_in_sge;
		BUG_ON(data_len < 0);
		sge_off = 0;
		sge++;
		sge_idx++;
		BUG_ON(sge_idx > sent_fpdu->wqe.sqe.num_sge || 
		(sge_idx == sent_fpdu->wqe.sqe.num_sge && data_len > 0));
		mem++;
		pr_debug("#4 - sent_fpdu " dprint_ptr_str() " data_len %d data_in_sge %d sge_off %d sge " dprint_ptr_str() " sge_idx %d num_sge %d mem " dprint_ptr_str() "\n",
				 sent_fpdu, data_len, data_in_sge, sge_off, sge, sge_idx, sent_fpdu->wqe.sqe.num_sge, mem);
	}
crc_pad:
	/* Calc pad crc and finalize */
	crypto_shash_update(fpdu_hd, 
						sent_fpdu->trailer.pad + sizeof(sent_fpdu->trailer.pad) - pad_len, pad_len);
	data_crc += pad_len;
crc_final:
	crypto_shash_final(fpdu_hd, (u8*)&calc_crc);
	/* Phew! Now compare and see if there's been a change */
	if (calc_crc != pkt_crc) {
		/* CRC changed since post send */
		pr_err("SIW_DEBUG_CRC - qp " dprint_ptr_str() " tctx " dprint_ptr_str() " sent_fpdu " dprint_ptr_str() " calc crc %x != mpa crc %x\n",
			   qp, &qp->tx_ctx, sent_fpdu, calc_crc, pkt_crc);
		BUG_ON(1);
	}
	atomic_inc(&qp->tx_ctx.n_completed_fpdus);
}
#else
static void check_sent_fpdu_crc(struct siw_qp *qp, struct siw_iwarp_tx_fpdu *sent_fpdu)
{
	(void)qp;
	(void)sent_fpdu;
}
#endif

#ifdef SIW_TX_COMP_WAIT_ACK
/* Should be called with sq lock held or QP state-lock in write
 * return > 0 if scq notify should be called */
int siw_tx_complete_ack_seq(struct siw_qp *qp)
{
	struct siw_iwarp_tx *tctx = &qp->tx_ctx;
	struct socket *s = qp->attrs.llp_stream_handle;
	struct tcp_sock *tp = tcp_sk(s->sk);
	struct siw_iwarp_tx_fpdu *sent_fpdu, *del_fpdu;
	int notify = 0;
	u32 solicited_flags __attribute__((unused)) = 0;
	u32 snd_una;
	union siw_iwarp_tx_sent_fpdu_notify sent_fpdu_notify = {};

again:
	snd_una = tp->snd_una;

	while ((sent_fpdu = list_first_entry_or_null(&tctx->sent_fpdus,
		struct siw_iwarp_tx_fpdu, link)) && after(snd_una, sent_fpdu->end_seq)) {
		list_del(&sent_fpdu->link);

		if (sent_fpdu->wqe.sqe.flags & SIW_WQE_TX_TIMESTAMP) {
			sent_fpdu->wqe.sqe_md.ack_time = ktime_get();
		}

		if (qp->tx_ctx.mpa_crc_hd)
			check_sent_fpdu_crc(qp, sent_fpdu);

		dprint(DBG_TX_COMP, "(QP%d): TX Comp: FPDU %x Acked, mpa_len=%d, tagged=%d, is_last=%d (tcp_seq %u - %u)\n",
				QP_ID(qp), __rdmap_opcode(&sent_fpdu->hdr.ctrl), 
				be16_to_cpu(sent_fpdu->hdr.ctrl.mpa_len),
				!!(sent_fpdu->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_TAGGED),
				!!(sent_fpdu->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_LAST),
				sent_fpdu->send_seq, sent_fpdu->end_seq);


		if (sent_fpdu->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_LAST) {
			/* Complete WQE */
			if ((tx_type(&sent_fpdu->wqe) == SIW_OP_WRITE ||
				tx_type(&sent_fpdu->wqe) == SIW_OP_SEND ||
				tx_type(&sent_fpdu->wqe) == SIW_OP_SEND_REMOTE_INV ||
				tx_type(&sent_fpdu->wqe) == SIW_OP_SEND_WITH_IMM)) {

				/* Put the ref-count for the WQE MRs */
				siw_wqe_put_mem(&sent_fpdu->wqe, tx_type(&sent_fpdu->wqe));

				if (tx_flags(&sent_fpdu->wqe) & SIW_WQE_SIGNALLED && 
					!(sent_fpdu->hdr.ctrl.ddp_rdmap_ctrl & DDP_FLAG_WR_ACK)) {
					dprint(DBG_TX_COMP, "(QP%d): TXTX: call siw_sqe_complete, tx_type=%x\n",
						QP_ID(qp), tx_type(&sent_fpdu->wqe));

					siw_sqe_complete(qp, &sent_fpdu->wqe.sqe, &sent_fpdu->wqe.sqe_md, 
							 sent_fpdu->wqe.bytes, SIW_WC_SUCCESS, 0);
					notify++;
					/* Aggregate solicited flag */
					solicited_flags |= (sent_fpdu->wqe.sqe.flags & SIW_WQE_SOLICITED);
				}
			}
		}
		if (SIW_TX_COMP_ACK_HISTORY) {
			list_add(&sent_fpdu->link, &tctx->completed_fpdus);
			tctx->n_completed_fpdus_in_list++;
			if (tctx->n_completed_fpdus_in_list > SIW_TX_COMP_ACK_HISTORY) {
				del_fpdu = list_last_entry(&tctx->completed_fpdus, typeof(*sent_fpdu), link);
				BUG_ON(del_fpdu == sent_fpdu);
				list_del(&del_fpdu->link);
				kfree(del_fpdu);
				tctx->n_completed_fpdus_in_list--;
			}
		} else
			kfree(sent_fpdu);
	}

	if (tp->snd_una != snd_una)
		goto again;
	
	if (!sent_fpdu) {
		sent_fpdu_notify.armed = 0;
		sent_fpdu_notify.end_seq = 0;
	} else {
		sent_fpdu_notify.armed = 1;
		sent_fpdu_notify.end_seq = sent_fpdu->end_seq;
	}
	smp_store_mb(qp->tx_ctx.sent_fpdu_notify.all, sent_fpdu_notify.all);

	return notify;
}
#else
int siw_tx_complete_ack_seq(struct siw_qp *qp)
{
	(void)qp;
	return 0;
}
#endif


/*
 * siw_qp_sq_process()
 *
 * Core TX path routine for RDMAP/DDP/MPA using a TCP kernel socket.
 * Sends RDMAP payload for the current SQ WR @wqe of @qp in one or more
 * MPA FPDUs, each containing a DDP segment.
 *
 * SQ processing may occur in user context as a result of posting
 * new WQE's or from siw_sq_work_handler() context. Processing in
 * user context is limited to non-kernel verbs users.
 *
 * SQ processing may get paused anytime, possibly in the middle of a WR
 * or FPDU, if insufficient send space is available. SQ processing
 * gets resumed from siw_sq_work_handler(), if send space becomes
 * available again.
 *
 * Must be called with the QP state read-locked.
 *
 * TODO:
 * To be solved more seriously: an outbound RREQ can be satisfied
 * by the corresponding RRESP _before_ it gets assigned to the ORQ.
 * This happens regularly in RDMA READ via loopback case. Since both
 * outbound RREQ and inbound RRESP can be handled by the same CPU
 * locking the ORQ is dead-lock prone and thus not an option.
 * Tentatively, the RREQ gets assigned to the ORQ _before_ being
 * sent (and pulled back in case of send failure).
 */
int siw_qp_sq_process(struct siw_qp *qp)
{
	struct siw_wqe		*wqe = tx_wqe(qp);
	enum siw_opcode		tx_type;
	unsigned long		flags;
	int			rv = 0;

#if SIW_RESCHED_QP_TX_IN_USE
	dprint(DBG_OL, "(QP%d): TXTX: check if already in_use ...\n",
	       QP_ID(qp));
	
	if (atomic_cmpxchg(&qp->tx_ctx.in_use, 0, SIW_IWARP_TX_IN_USE) != 0) {
		/* Another thread is still in siw_qp_sq_process for this QP */
		dprint(DBG_OL, "(QP%d): TXTX: already in_use (pid: %d) returning -EBUSY to reschedule...\n",
		       QP_ID(qp), qp->tx_ctx.in_use_pid);
		return -EBUSY;
	}
#else
	do {
		int wait_rv;
		dprint(DBG_OL, "(QP%d): TXTX: check if only waiter ...\n",
			QP_ID(qp));

		dprint(DBG_OL, "(QP%d): TXTX: wait event...\n",
			QP_ID(qp));

		wait_rv = wait_event_timeout(qp->tx_ctx.waitq, !atomic_read(&qp->tx_ctx.in_use), HZ);
		if (wait_rv == 0) {
			dprint(DBG_ON, "(QP%d): Timeout waiting for in_use = 0. siw_qp=" dprint_ptr_str() "\n",
				QP_ID(qp), qp);
			pr_err("(QP%d): Timeout waiting for in_use = 0. siw_qp=" dprint_ptr_str() "\n",
				QP_ID(qp), qp);
			BUG();
		}

		if (atomic_inc_return(&qp->tx_ctx.in_use) > 1) {
			/*
			* at least two waiters: that should never happen!
			*/
			//WARN_ON(1); Disable for now, needs to be fixed eventually
			dprint(DBG_OL, "(QP%d): TXTX: already in_use...\n",
				QP_ID(qp));
			atomic_dec(&qp->tx_ctx.in_use);
			return 0;
		}
	} while(0);
#endif

	qp->tx_ctx.in_use_pid = current->pid;

	lock_sq_rxsave(qp, flags);
	if (wqe->wr_status == SR_WR_IDLE || qp->tx_ctx.tx_suspend) {
		if (qp->tx_ctx.tx_suspend)
			rv = -ESHUTDOWN;
		unlock_sq_rxsave(qp, flags);
		goto done;
	}
	unlock_sq_rxsave(qp, flags);

	if (test_bit(SOCK_NOSPACE, &qp->attrs.llp_stream_handle->flags)) {
		/* No space, jump to done to process ACKs */
		goto done;
	}

next_wqe:
	/* Check for completed FPDUs on each new WQE */
	if (qp->scq) {
		dprint(DBG_TX_COMP, "(QP:%d) calling siw_tx_complete_ack_seq\n", QP_ID(qp));
		if (siw_tx_complete_ack_seq(qp)) {
			dprint(DBG_TX_COMP, "(QP:%d) cq notify\n", QP_ID(qp));
			if (qp->notify_on_wq)
				siw_schedule_cq_notify_work(qp, qp->scq);
			else
				siw_cq_notify(qp->scq, 0, true);
		}
	}

	dprint(DBG_OL, "(QP%d): TXTX: start next WQE processing...\n",
		QP_ID(qp));
	
	lock_sq_rxsave(qp, flags);
	if (unlikely(qp->tx_ctx.tx_suspend)) {
		rv = -ESHUTDOWN;
		unlock_sq_rxsave(qp, flags);
		goto done;
	}
	unlock_sq_rxsave(qp, flags);

	tx_type = tx_type(wqe);

	if (tx_type <= SIW_OP_READ_RESPONSE) {
		dprint(DBG_OL, "(QP%d): TXTX: calling siw_qp_sq_proc_tx...\n",
			QP_ID(qp));
		rv = siw_qp_sq_proc_tx(qp, wqe, false);
	} else {
		dprint(DBG_OL, "(QP%d): TXTX: calling siw_qp_sq_proc_local...\n",
			QP_ID(qp));
		rv = siw_qp_sq_proc_local(qp, wqe);
	}

	if (!rv) {
		/*
		 * WQE processing done
		 */
		dprint(DBG_OL, "(QP%d): TXTX: WQE processing OK.\n",
			QP_ID(qp));

		switch (tx_type) {
#ifdef SIW_TX_COMP_WAIT_ACK
		case SIW_OP_SEND: /* Delay these until we get an ack from the receiver */
		case SIW_OP_SEND_REMOTE_INV:
		case SIW_OP_SEND_WITH_IMM:
		case SIW_OP_WRITE:
			break;
#else
		case SIW_OP_SEND: /* omril: siw_qp_sq_process */
		case SIW_OP_SEND_REMOTE_INV:
		case SIW_OP_SEND_WITH_IMM:
		case SIW_OP_WRITE:
			siw_wqe_put_mem(wqe, tx_type);
#endif
		case SIW_OP_INVAL_STAG:
		case SIW_OP_REG_MR:
			if (tx_flags(wqe) & SIW_WQE_SIGNALLED && 
				!(qp->tx_ctx.pkt.ctrl.ddp_rdmap_ctrl & DDP_FLAG_WR_ACK)) {
				dprint(DBG_OL, "(QP%d): TXTX: call siw_sqe_complete, tx_type=%x\n",
					QP_ID(qp), tx_type);

				if (tx_flags(wqe) & SIW_WQE_TX_TIMESTAMP)
					wqe->sqe_md.sent_time = ktime_get();
				wqe->sqe_md.tx_cpu = smp_processor_id();
				siw_sqe_complete(qp, &wqe->sqe, &wqe->sqe_md, wqe->bytes,
						 SIW_WC_SUCCESS, 1);
			}
			break;

		case SIW_OP_READ:
		case SIW_OP_READ_LOCAL_INV:
		case SIW_OP_COMP_AND_SWAP:
		case SIW_OP_MASKED_COMP_AND_SWAP:
			/*
			 * already enqueued to ORQ queue
			 */
			break;

		case SIW_OP_READ_RESPONSE:
		case SIW_OP_COMP_AND_SWAP_RESPONSE:
			//omril: we had sent the data client request to read -> done with the mem-obj
			siw_wqe_put_mem(wqe, tx_type);
			break;
		case SIW_OP_WRITE_RESPONSE:
			break;

		default:
			BUG();
		}

		lock_sq_rxsave(qp, flags);
		wqe->wr_status = SR_WR_IDLE;
		dprint(DBG_OL, "(QP%d): TXTX: call siw_activate_tx...\n",
			QP_ID(qp));
		rv = siw_activate_tx(qp);
		unlock_sq_rxsave(qp, flags);
		/* siw_activate_tx returns < 0 for error, 0 for nothing to tx, 1 for ready for tx */
		if (unlikely(rv <= 0)) {
			goto done;
		}

		goto next_wqe;
	} else if (rv == -EAGAIN) {
		dprint(DBG_OL, "(QP%d): TXTX: WQE processing -EAGAIN\n",
			QP_ID(qp));

		dprint(DBG_WR|DBG_TX,
			"(QP%d): SQ paused: hd/tr %d of %d, data %d\n",
			QP_ID(qp), qp->tx_ctx.ctrl_sent, qp->tx_ctx.ctrl_len,
			qp->tx_ctx.bytes_unsent);
		rv = 0;
		goto done;
	} else if (rv == -EINPROGRESS) {
		dprint(DBG_OL, "(QP%d): TXTX: WQE processing -EINPROGRESS\n",
			QP_ID(qp));

		siw_sq_queue_work(qp, SIW_TX_CTX_PREF_SCQ_VECT);
		rv = 0;
		goto done;
	} else {
		/*
		 * WQE processing failed.
		 * Verbs 8.3.2:
		 * o It turns any WQE into a signalled WQE.
		 * o Local catastrophic error must be surfaced
		 * o QP must be moved into Terminate state: done by code
		 *   doing socket state change processing
		 *
		 * o TODO: Termination message must be sent.
		 * o TODO: Implement more precise work completion errors,
		 *         see enum ib_wc_status in ib_verbs.h
		 */
		dprint(DBG_ON|DBG_OL, " (QP%d): TXTX: WQE type %d processing failed: %d\n",
				QP_ID(qp), tx_type(wqe), rv);

#ifdef SIW_TX_COMP_WAIT_ACK
		if (qp->tx_ctx.fpdu_in_prog) {
			kfree(qp->tx_ctx.fpdu_in_prog);
			qp->tx_ctx.fpdu_in_prog = NULL;
		}
#endif

		lock_sq_rxsave(qp, flags);
		/*
		 * RREQ may have already been completed by inbound RRESP!
		 */
		if (tx_type == SIW_OP_READ ||
		    tx_type == SIW_OP_READ_LOCAL_INV ||
			tx_type == SIW_OP_COMP_AND_SWAP ||
			tx_type == SIW_OP_MASKED_COMP_AND_SWAP) {
			/* Cleanup pending entry in ORQ */
			qp->orq_put--;
			dprint(DBG_OL, "(QP%d): TXTX: inc orq_put to %d (orq_get=%d)\n",
				QP_ID(qp), qp->orq_put, qp->orq_get);
			qp->orq[qp->orq_put % qp->attrs.orq_size].flags = 0;
		}
		unlock_sq_rxsave(qp, flags);
		/*
		 * immediately suspends further TX processing
		 */
		switch (tx_type) {

		case SIW_OP_SEND: /* omril: siw_qp_sq_process */
		case SIW_OP_SEND_REMOTE_INV:
		case SIW_OP_SEND_WITH_IMM:
		case SIW_OP_WRITE:
		case SIW_OP_READ: 			//omril: BUG? run with trace and see when do we inc mem kref?
		case SIW_OP_READ_LOCAL_INV: //omril: BUG?
			siw_wqe_put_mem(wqe, tx_type);
			FALLTHRU;
		case SIW_OP_INVAL_STAG:
		case SIW_OP_REG_MR:
		case SIW_OP_COMP_AND_SWAP: 
		case SIW_OP_MASKED_COMP_AND_SWAP: //omril: unlike READ!!!

			dprint(DBG_OL, "(QP%d): TXTX: call siw_sqe_complete\n",
				QP_ID(qp));

			siw_sqe_complete(qp, &wqe->sqe, NULL, wqe->bytes,
					 SIW_WC_LOC_QP_OP_ERR, 1);

			siw_qp_event(qp, IB_EVENT_QP_FATAL);
			break;

		case SIW_OP_READ_RESPONSE:
		case SIW_OP_COMP_AND_SWAP_RESPONSE:
		case SIW_OP_WRITE_RESPONSE:
			dprint(DBG_WR|DBG_TX|DBG_ON, "(QP%d): "
				   "Processing %sRESPONSE failed with %d\n",
				    QP_ID(qp), tx_type == SIW_OP_READ_RESPONSE ? "R" : "A",
					rv);

			siw_qp_event(qp, IB_EVENT_QP_REQ_ERR);

			siw_wqe_put_mem(wqe, tx_type);

			break;

		default:
			BUG();
		}
		
		lock_sq_rxsave(qp, flags);
		wqe->wr_status = SR_WR_IDLE;
		/* This is actually redundant because we are about to call siw_qp_cm_drop */
		qp->tx_ctx.tx_suspend = 1;
		unlock_sq_rxsave(qp, flags);
	}
done:
	if (!rv) {
		if (qp->scq) {
			dprint(DBG_TX_COMP, "(QP:%d) calling siw_tx_complete_ack_seq\n", QP_ID(qp));
			if (siw_tx_complete_ack_seq(qp)) {
				dprint(DBG_TX_COMP, "(QP:%d) cq notify\n", QP_ID(qp));
				if (qp->notify_on_wq)
					siw_schedule_cq_notify_work(qp, qp->scq);
				else
					siw_cq_notify(qp->scq, 0, true);
			}
		}
	}

	qp->tx_ctx.in_use_pid = 0;

#if SIW_RESCHED_QP_TX_IN_USE
	do {
		enum siw_iwarp_tx_in_use_flags in_use_flags = atomic_xchg(&qp->tx_ctx.in_use, 0);
		/* Sanity Check */
		BUG_ON(!(in_use_flags & SIW_IWARP_TX_IN_USE));
		if (in_use_flags & SIW_IWARP_TX_REQ_RESCHED) {
			/* Another thread has requested that we reschedule - return -EAGAIN */
			dprint(DBG_TX | DBG_ON, "(QP%d): TX: rescheduling due to request by another thread...\n", QP_ID(qp));
			rv = -EAGAIN;
		}
	} while(0);
#else
	atomic_dec(&qp->tx_ctx.in_use);
	wake_up(&qp->tx_ctx.waitq);
#endif

	if (rv < 0) {
		if (rv != -ESHUTDOWN && rv != -EAGAIN) {
			pr_warn("(QP%d): siw_qp_sq_process failed (%d)\n",
					QP_ID(qp), rv);	
		} else if (rv != -EAGAIN) {
			dprint(DBG_TX | DBG_ON, "(QP%d): TX in shutdown\n",
			       QP_ID(qp));
			rv = 0;
		}
	}
	return rv;
}

struct workqueue_struct *siw_sq_wq;

int __init siw_sq_worker_init(void)
{
	siw_sq_wq = alloc_workqueue("siw_sq_wq", WQ_HIGHPRI, 0);
	if (!siw_sq_wq)
		return -ENOMEM;

	dprint(DBG_TX|DBG_OBJ, " Init WQ\n");
	return 0;
}


void siw_sq_worker_exit(void)
{
	dprint(DBG_TX|DBG_OBJ, " Destroy WQ\n");
	if (siw_sq_wq) {
		flush_workqueue(siw_sq_wq);
		destroy_workqueue(siw_sq_wq);
	}
}

#ifndef USE_SQ_KTHREAD
/*
 * siw_sq_work_handler()
 *
 * Scheduled by siw_qp_llp_write_space() socket callback if socket
 * send space became available again. This function resumes SQ
 * processing.
 */
static void siw_sq_work_handler(struct work_struct *w)
{
	struct siw_sq_work	*this_work;
	struct siw_qp		*qp;
	int			rv;
	unsigned long		flags;

	this_work = container_of(w, struct siw_sq_work, work);
	qp = container_of(this_work, struct siw_qp, sq_work);

	dprint(DBG_TX|DBG_OBJ, "(QP%d)\n", QP_ID(qp));

	if (down_read_trylock(&qp->state_lock)) {
		if (likely(qp->attrs.state == SIW_QP_STATE_RTS)) {
			lock_sq_rxsave(qp, flags);
			if (qp->tx_ctx.tx_suspend) {
				unlock_sq_rxsave(qp, flags);
				pr_warn("QP[%d]: tx suspended\n",
					QP_ID(qp));
				goto out;
			}
			if (qp->tx_ctx.orq_fence) {
				unlock_sq_rxsave(qp, flags);
				pr_warn("QP[%d]: work handler in fence\n",
					QP_ID(qp));
				goto out;
			}
			unlock_sq_rxsave(qp, flags);
			dprint(DBG_OL, "(QP%d): TXTX: call siw_qp_sq_process...\n",
				QP_ID(qp));
			rv = siw_qp_sq_process(qp);
			up_read(&qp->state_lock);
			if (rv < 0) {
				if (rv == -EBUSY || rv == -EAGAIN) {
					queue_work(siw_sq_wq, w);
					goto out;
				}
				dprint(DBG_TX, "(QP%d): failed: %d\n",
					QP_ID(qp), rv);

				siw_qp_cm_drop(qp, 0);
			}
		} else {
			dprint(DBG_ON|DBG_TX, "(QP%d): state: %d\n",
				QP_ID(qp), qp->attrs.state);
			up_read(&qp->state_lock);
		}
	} else {
		atomic_inc(&qp->state_lock_failed);
		dprint(DBG_ON|DBG_TX, "(QP%d): QP (" dprint_ptr_str() ") locked\n", QP_ID(qp), qp);
		BUG();
	}
out:
	siw_qp_put(qp);
}
#else

static int siw_sq_run(unsigned long arg)
{
	struct siw_qp *qp = (struct siw_qp *)arg;
	int rv;

	if (down_read_trylock(&qp->state_lock)) {
		if (likely(qp->attrs.state == SIW_QP_STATE_RTS)) {
			unsigned long start_proc_jif = jiffies, proc_jif;
			dprint(DBG_OL, "(QP%d): TXTX: call siw_qp_sq_process...\n",
				QP_ID(qp));
			rv = siw_qp_sq_process(qp);
			if ((proc_jif = (jiffies - start_proc_jif)) > SIW_QP_SQ_PROCESS_LOG_TIMEOUT) {
				dprint(DBG_ON, "(QP%d): siw_qp_sq_process took %u ms",
				       QP_ID(qp), jiffies_to_msecs(proc_jif));
				SIW_TIMEOUT_WARN_ON_ONCE(proc_jif, SIW_QP_SQ_PROCESS_WARN_TIMEOUT);
			}

			if (unlikely(rv < 0)) {
				if (rv != -EBUSY && rv != -EAGAIN) {
					dprint(DBG_ON, "QP[%d]: SQ task failed: %d\n",
						QP_ID(qp), rv);
					siw_qp_cm_drop(qp, 0);
					rv = -ESHUTDOWN;
				}
			}

			up_read(&qp->state_lock);
		} else {
			if (qp->attrs.state == SIW_QP_STATE_ERROR ||
				qp->attrs.state == SIW_QP_STATE_CLOSING) {
				dprint(DBG_ON, "QP(%d) flush sq in state=%s\n",
					QP_ID(qp),
					siw_qp_state_to_string[qp->attrs.state]);
				up_read(&qp->state_lock);
			
				/* Need to hold QP state_lock as write to call siw_sq_flush */
				write_lock_qp(qp);
				siw_sq_flush(qp);
				write_unlock_qp(qp);
			} else
				up_read(&qp->state_lock);
			rv = -ESHUTDOWN;
		}
	} else {
		/* QP is write-locked, check the tx_suspend flag is set otherwise something has gone horribly wrong */
		unsigned long flags;
		lock_sq_rxsave(qp, flags);
		if (!qp->tx_ctx.tx_suspend) {
			unlock_sq_rxsave(qp, flags);
			dprint(DBG_ON, "(QP%d): QP (" dprint_ptr_str() ") write-locked, but tx_suspend = 0\n",
			       QP_ID(qp), qp);
			BUG();
		}
		unlock_sq_rxsave(qp, flags);
		rv = -ESHUTDOWN;
	}

	return rv;
}

struct tx_task_t {
	struct llist_head active;
	wait_queue_head_t waiting;
};

DEFINE_PER_CPU(struct tx_task_t, tx_task_g);

static ulong low_delay_tx_cpu_set = ~(0ULL);
module_param(low_delay_tx_cpu_set, ulong, 0644);
MODULE_PARM_DESC(low_delay_tx_cpu_set, "bitmap of tx-cpus thread in tight loop (ulong).");

bool siw_low_delay_tx_cpu(ulong nr_cpu)
{
	if (!low_delay_tx)
		return low_delay_tx;

	if (nr_cpu >= 64)
		return low_delay_tx;

	return !!((low_delay_tx_cpu_set >> nr_cpu) & 1);
}

extern atomic_t cq_notify_sched_cnt[NR_CPUS];

extern bool cq_notify_tasklet;

int siw_run_sq(void *data);
int siw_run_sq(void *data)
{
	const int nr_cpu = (unsigned int)(long)data;
	struct llist_node *active;
	struct siw_qp *qp;
	unsigned long stoptime = jiffies;
	struct tx_task_t *tx_task = &per_cpu(tx_task_g, nr_cpu);
	bool stop_thread = false;

	init_llist_head(&tx_task->active);
	init_waitqueue_head(&tx_task->waiting);

	dprint(DBG_ON, "Started siw TX thread on CPU %u\n", nr_cpu);

	while (!stop_thread) {
		if (siw_low_delay_tx_cpu(nr_cpu) && (zero_delay_tx || time_before(jiffies, stoptime))) {
			if (kthread_should_stop()) {
				stop_thread = true;
			} else {
				set_current_state(TASK_INTERRUPTIBLE);
				cond_resched();
				__set_current_state(TASK_RUNNING);
			}
		}
		else {
			wait_event_interruptible(tx_task->waiting,
				kthread_should_stop() ||
				!llist_empty(&tx_task->active));

			if (kthread_should_stop())
				stop_thread = true;
		}

		active = llist_del_all(&tx_task->active);
		if (active != NULL) {
			struct llist_node *fifo_list = NULL;

			/*
			 * llist_del_all returns a list with newest entry first.
			 * Re-order list for fairness among QP's.
			 */
			while (active) {
				struct llist_node *tmp = active;

				active = llist_next(active);
				tmp->next = fifo_list;
				fifo_list = tmp;
			}
			while (fifo_list) {
				unsigned long run_jif = jiffies, list_jif;
				struct llist_head *old_tx_list_head;
				int rv;

				/* Pop QP from list */
				qp = container_of(fifo_list, struct siw_qp, tx_list);
				fifo_list = llist_next(fifo_list);
				qp->tx_list.next = NULL;
				/* Check and reset how long QP has been in list (must be before cmpxchg barrier) */
				list_jif = qp->tx_list_jif;
				qp->tx_list_jif = 0;

				/* Remove reschedule barrier and verify */
				if ((old_tx_list_head = cmpxchg(
					&qp->tx_list_head, &tx_task->active, NULL)) != &tx_task->active) {
					pr_warn("SIW QP(%d/" dprint_ptr_str() ") - invalid tx_list_head " dprint_ptr_str() " expected tx_list_head " dprint_ptr_str() "\n",
						QP_ID(qp), qp, old_tx_list_head, &tx_task->active);
					BUG();
				}
				/* Warn if QP has been delayed significantly */
				if (run_jif > list_jif && (run_jif - list_jif) > SIW_RUN_SQ_DELAY_LOG) {
					dprint(DBG_ON, "QP(%d) - siw_sq_run delayed by %u ms\n",
						QP_ID(qp), jiffies_to_msecs(run_jif - list_jif));
					SIW_TIMEOUT_WARN_ON_ONCE(run_jif - list_jif, SIW_RUN_SQ_DELAY_WARN);
				}

				rv = siw_sq_run((unsigned long)qp);
				switch (rv) {
				case -EBUSY:
					/* QP was in-use, tell it's current owner to reschedule when it's done */
					{
						pid_t in_use_pid = qp->tx_ctx.in_use_pid;
						int prev_in_use = atomic_cmpxchg(&qp->tx_ctx.in_use,
										SIW_IWARP_TX_IN_USE, 
										SIW_IWARP_TX_IN_USE | SIW_IWARP_TX_REQ_RESCHED);
						if (prev_in_use != 0) {
							BUG_ON((prev_in_use & ~(SIW_IWARP_TX_IN_USE | SIW_IWARP_TX_REQ_RESCHED)));
							dprint(DBG_ON, "(QP%d): TX: requested (pid: %d) to reschedule...\n",
							       QP_ID(qp), in_use_pid);
							siw_qp_put(qp);
							continue;
						}
						/* Not in-use anymore, move QP back to our tx active list */
						dprint(DBG_ON, "(QP%d): TX: no longer in_use...\n", QP_ID(qp));
					}
					FALLTHRU;
				case -EAGAIN:
					dprint(DBG_ON, "(QP%d): TX: rescheduling..\n", QP_ID(qp));
					if (cmpxchg(&qp->tx_list_head, NULL, &tx_task->active) == NULL) {
						dprint(DBG_ON, "(QP%d): TX: rescheduled...\n",
						       QP_ID(qp));
						qp->tx_list_jif = list_jif;
						llist_add(&qp->tx_list, &tx_task->active);
					} else {
						dprint(DBG_ON, "(QP%d): TX: already rescheduled...\n",
						       QP_ID(qp));
						siw_qp_put(qp);
					}
					break;
				case -ESHUTDOWN:
				case 0:
					siw_qp_put(qp);
					break;
				default:
					BUG();
				}
				if (!cq_notify_tasklet && atomic_read(&cq_notify_sched_cnt[nr_cpu]) > 0) {
					/* Reschedule to allow CQ notify work queue to run */
					set_current_state(TASK_INTERRUPTIBLE);
					cond_resched();
					__set_current_state(TASK_RUNNING);
				}
			}
			stoptime = jiffies + HZ;
		}
	}

	dprint(DBG_ON, "Stopped siw TX thread on CPU %u\n", nr_cpu);
	return 0;
}

static int siw_qp_to_tx(struct siw_qp *qp, enum siw_tx_ctx_pref tx_ctx_pref)
{
	int cpu;
	struct llist_head *tx_list_head;
	struct siw_dev *sdev = qp->hdr.sdev;
	int *vec_cpu = qp_tx_vector_cpu;
	int vec_n = num_tx_vector;
	int i;

	if (sdev && sdev->num_tx_vector > 0) {
		vec_cpu = sdev->tx_vector_cpu;
		vec_n = sdev->num_tx_vector;
	}

	if ((tx_list_head = READ_ONCE(qp->tx_list_head)) != NULL) {
		dprint(DBG_TX, "QP(%d/" dprint_ptr_str() ") - already in tx list " dprint_ptr_str() " (%lu jiffies ago)\n",
		       QP_ID(qp), qp, tx_list_head, jiffies - qp->tx_list_jif);
		return -EALREADY;
	}

	if (tx_ctx_pref == SIW_TX_CTX_PREF_SAME_CPU) {
		qp->cpu = cpu = smp_processor_id();
		goto wake_up;
	} else if (tx_ctx_pref == SIW_TX_CTX_PREF_SCQ_VECT) {
		qp->cpu = cpu = vec_cpu[qp->scq->comp_vector % vec_n];
		goto wake_up;
	} else
		cpu = qp->cpu;

	if (!cpu_online(cpu) || qp_tx_thread[cpu] == NULL)
		cpu = default_tx_cpu;

	if (unlikely(cpu < 0)) {
		WARN_ON(1);
		goto out;
	}
	if (!llist_empty(&per_cpu(tx_task_g, cpu).active)) {
		int new_cpu;

		for (i = 0; i < vec_n; i++) {
			new_cpu = vec_cpu[i];
			if (cpu_online(new_cpu) && qp_tx_thread[new_cpu] != NULL &&
			    llist_empty(&per_cpu(tx_task_g, new_cpu).active)) {
				cpu = new_cpu;
				qp->cpu = new_cpu;
				goto wake_up;
			}
		}
		for_each_online_cpu(new_cpu) {
			if (qp_tx_thread[new_cpu] != NULL &&
			    llist_empty(&per_cpu(tx_task_g, new_cpu).active)) {
				cpu = new_cpu;
				qp->cpu = new_cpu;
				break;
			}
		}
	}

wake_up:
	if ((tx_list_head = 
		cmpxchg(&qp->tx_list_head, NULL, &per_cpu(tx_task_g, cpu).active)) != NULL) {
		dprint(DBG_TX, "(QP%d/" dprint_ptr_str() "): TX: already scheduled on tx_list " dprint_ptr_str() "\n", 
		       QP_ID(qp), qp, tx_list_head);
		goto out;
	}

	siw_qp_get(qp);
	qp->tx_list_jif = jiffies;
	llist_add(&qp->tx_list, &per_cpu(tx_task_g, cpu).active);

	dprint(DBG_OL, "(QP%d): TXTX: wake_up\n",
		QP_ID(qp));

	wake_up(&per_cpu(tx_task_g, cpu).waiting);

out:
	return 0;
}
#endif


int siw_sq_queue_work(struct siw_qp *qp, enum siw_tx_ctx_pref tx_ctx_pref)
{
	dprint(DBG_TX|DBG_OBJ, "(qp%d)\n", QP_ID(qp));

#ifdef USE_SQ_KTHREAD
	return siw_qp_to_tx(qp, tx_ctx_pref);
#else
	siw_qp_get(qp);
	INIT_WORK(&qp->sq_work.work, siw_sq_work_handler);
	return queue_work(siw_sq_wq, &qp->sq_work.work);
#endif
}
