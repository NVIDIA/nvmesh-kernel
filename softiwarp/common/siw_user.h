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

#ifndef _SIW_USER_H
#define _SIW_USER_H

#include <linux/types.h>

/*
 * user commands/command responses must correlate with the siw_abi
 * in user land.
 */
/*Common string that is matched to accept the device by the user library*/
#define SIW_NODE_DESC_COMMON	"Software iWARP stack"

#define SIW_IBDEV_PREFIX 	"siw_"

#define VERSION_ID_SOFTIWARP	2

#if KS_IB_DEVICE_HAS_DRIVER_ID || KS_IB_DEVICE_OPS_HAS_DRIVER_ID
enum {
	RDMA_DRIVER_SIW_XLRO	= 0x786c726f, /* "xlro" */
};
#endif

#define SIW_MAX_SGE		4 //6
#define SIW_MAX_UOBJ_KEY	0xffffff
#define SIW_INVAL_UOBJ_KEY	(SIW_MAX_UOBJ_KEY + 1)
#define SIW_MAX_CPU		64

struct siw_uresp_alloc_pd {
	__u32	pd_id;
	__u32	reserved;
};

struct siw_uresp_create_cq {
	__u32	cq_id;
	__u32	num_cqe;
	__u32	cq_key;
	__u32	reserved;
};

struct siw_uresp_create_qp {
	__u32	qp_id;
	__u32	num_sqe;
	__u32	num_rqe;
	__u32	sq_key;
	__u32	rq_key;
	__u32	reserved;
};

struct siw_ureq_reg_mr {
	__u8	stag_key;
	__u8	reserved[3];
	__u32	reserved1;
};

struct siw_uresp_reg_mr {
	__u32	stag;
	__u32	reserved;
};

struct siw_uresp_create_srq {
	__u32	num_rqe;
	__u32	srq_key;
};

struct siw_uresp_alloc_ctx {
	__u32	dev_id;
	__u32	rdma_db_nr;
};

enum siw_db_type {
	SIW_DB_SQ		= 1
};

enum siw_opcode {
	SIW_OP_WRITE				= 0,
	SIW_OP_READ				= 1,
	SIW_OP_READ_LOCAL_INV			= 2,
	SIW_OP_SEND				= 3,
	SIW_OP_SEND_WITH_IMM			= 4,
	SIW_OP_SEND_REMOTE_INV			= 5,

	/* Unsupported */
	SIW_OP_FETCH_AND_ADD			= 6,
	
	SIW_OP_COMP_AND_SWAP			= 7,
	SIW_OP_MASKED_COMP_AND_SWAP		= 8,

	SIW_OP_RECEIVE				= 9,

	/*
	 * below opcodes valid for
	 * in-kernel clients and internal use only
	 */

	SIW_OP_COMP_AND_SWAP_RESPONSE		= 10,
	SIW_OP_WRITE_RESPONSE			= 11,
	SIW_OP_READ_RESPONSE			= 12,	/* provider internal */
	SIW_OP_INVAL_STAG			= 13,
	SIW_OP_REG_MR				= 14,

	SIW_NUM_OPCODES				= 15,
	SIW_OP_INVALID				= SIW_NUM_OPCODES + 1
};

/* Keep it same as ibv_sge to allow for memcpy */
struct siw_sge {
	__aligned_u64	laddr;
	__u32		length;
	__u32		lkey;
};

/*
 * Inline data are kept within the work request itself occupying
 * the space of sge[1] .. sge[n]. Therefore, inline data cannot be
 * supported if SIW_MAX_SGE is below 2 elements.
 */
#define SIW_MAX_INLINE	(sizeof(struct siw_sge) * (SIW_MAX_SGE - 1))

#if SIW_MAX_SGE < 2
#error "SIW_MAX_SGE must be at least 2"
#endif

enum siw_wqe_flags { 
	SIW_WQE_VALID           = 1,
	SIW_WQE_INLINE          = (1 << 1),
	SIW_WQE_SIGNALLED       = (1 << 2),
	SIW_WQE_SOLICITED       = (1 << 3),
	SIW_WQE_READ_FENCE	= (1 << 4),
	SIW_WQE_COMPLETED       = (1 << 5),
	SIW_WQE_TX_TIMESTAMP	= (1 << 6),
	SIW_WQE_INLINE_SEND	= (1 << 7),
	SIW_WQE_MORE_WQES	= (1 << 8),
};

/* Minimum sized Send Queue Element */
struct siw_sqe {
	__aligned_u64	id;
	__u16	flags;
	__u8		num_sge;
	__u8		opcode; /* Actual enum siw_opcode values */
	__u32	rkey;
	union {
		__aligned_u64	raddr;
		__aligned_u64	ofa_mr; //IB_WR_REG_MR
	};
	union {
		struct siw_sge	sge[SIW_MAX_SGE];
		__u32	access; //IB_WR_REG_MR
	};
	//omril: need compress?
	__aligned_u64	compare_add;
	__aligned_u64	compare_add_mask;
	__aligned_u64	swap;
	__aligned_u64	swap_mask;
};

struct siw_rqe {
	__aligned_u64	id;
	__u16	flags;
	__u16	num_sge;
	__u32	srq_id;
	struct siw_sge	sge[SIW_MAX_SGE];
};

enum siw_notify_flags {
	SIW_NOTIFY_NOT			= (0),
	SIW_NOTIFY_SOLICITED		= (1 << 0),
	SIW_NOTIFY_NEXT_COMPLETION	= (1 << 1),
	SIW_NOTIFY_MISSED_EVENTS	= (1 << 2),
	SIW_NOTIFY_ALL = SIW_NOTIFY_SOLICITED |
			SIW_NOTIFY_NEXT_COMPLETION |
			SIW_NOTIFY_MISSED_EVENTS
};

enum siw_wc_status {
	SIW_WC_SUCCESS		= 0,
	SIW_WC_LOC_LEN_ERR	= 1,
	SIW_WC_LOC_PROT_ERR	= 2,
	SIW_WC_LOC_QP_OP_ERR	= 3,
	SIW_WC_WR_FLUSH_ERR	= 4,
	SIW_WC_BAD_RESP_ERR	= 5,
	SIW_WC_LOC_ACCESS_ERR	= 6,
	SIW_WC_REM_ACCESS_ERR	= 7,
	SIW_WC_REM_INV_REQ_ERR	= 8,
	SIW_WC_GENERAL_ERR	= 9,
	SIW_NUM_WC_STATUS	= 10
};

struct siw_cqe {
	__aligned_u64	id;
	__u8		flags;
	__u8		opcode;
	__u16	status;
	__u32	bytes;
	__u32	imm_data;
	/* Used for SRQ RQE tracking */
	__u32	srq_id;
	/* QP number or QP pointer */
	union { 
		void	*qp;
		__aligned_u64 qp_id;
	};
};

/*
 * Shared structure between user and kernel
 * to control CQ arming.
 */
struct siw_cq_ctrl {
	enum siw_notify_flags	notify;
};

#endif
