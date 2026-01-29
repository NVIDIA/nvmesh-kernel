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

#ifndef _SIW_KERN_ABI_H
#define _SIW_KERN_ABI_H

#include <linux/types.h>

enum siw_ofa_wc_flags {
	SIW_IB_WC_WITH_SIW_MD = (1 << 16), /* wc, has pointer to siw_ib_wc_md in wr_id field */
};

enum siw_wc_md_flags {
	SIW_WC_MD_TX_TIME = (1 << 0), /* Contains TX Timestamps */
	SIW_WC_MD_RX_TIME = (1 << 1), /* Contains RX Timestamps */
	SIW_WC_MD_INLINE_SEND = (1 << 2), /* Was Inline Send */
};

struct siw_wc_md {
	union {
		struct ib_cqe ofa_cqe; /* Must be first! This makes the CQE CB mechanism still work */
		u64 wr_id;
	};
	uint16_t	flags;
	union {
		struct {
			ktime_t		post_send_time;
			ktime_t 	sent_time;
			ktime_t 	ack_time;
			u16		tx_cpu;
		} tx_timestamp;
		struct {
			ktime_t 	first_ddp_recv_time;
			ktime_t 	last_ddp_recv_time;
			ktime_t		reap_time;
			u16		rx_cpu;
			u16		rx_queue;
			u32		rx_skb_hash;
		} rx_timestamp;
	};
};

enum siw_ofa_wr_flags {
	SIW_IB_SEND_TX_TIMESTAMP = IB_SEND_RESERVED_START,
	SIW_IB_SEND_MORE_WQES = (IB_SEND_RESERVED_START << 1),
	SIW_IB_SEND_TX_CTX_PREF_SAME_CPU = (IB_SEND_RESERVED_START << 2),
	SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT = (IB_SEND_RESERVED_START << 3),
};
	
#endif
