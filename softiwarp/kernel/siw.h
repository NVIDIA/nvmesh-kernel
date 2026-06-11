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

#ifndef _SIW_H
#define _SIW_H

#include <linux/idr.h>
#include <rdma/ib_verbs.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/fs.h>
#include <linux/netdevice.h>
#include <crypto/hash.h>
#include <linux/crc32.h>
#include <linux/crc32c.h>
#include <linux/resource.h>	/* MLOCK_LIMIT */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/llist.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/ratelimit.h>

#include <siw_kern_abi.h>
#include <siw_user.h>
#include "iwarp.h"

#ifdef LLVM
#	define crc32c  __crc32c_le
#endif

#if 0
IB_WR_RDMA_READ, IB_WC_RDMA_READ
SIW_OP_READ, SIW_OP_READ_RESPONSE
RDMAP_RDMA_READ_REQ, RDMAP_RDMA_READ_RESP
iwarp_rdma_rreq, iwarp_rdma_rresp
RDMAP_UNTAGGED_QN_RDMA_READ

Check negative case that include read req/resp,
for example: siw_activate_tx() if (wqe->sqe.opcode != SIW_OP_SEND &&

o Where do we read the data in the responder side?
o Shall responder return the cmp and swap values? just for validation against orq?
o Should ATOMIC req/resp use READ req/resp structure?
#endif


#define _load_shared(a)		(*(volatile typeof(a) *)&(a))

enum siw_if_type {
	SIW_IF_OFED = 0,	/* only via standard ofed syscall if */
	SIW_IF_MAPPED = 1	/* private qp and cq mapping */
};

#define DEVICE_ID_SOFTIWARP	0x0815
#define SIW_VENDOR_ID		0x786c726f	/* ascii 'xlro' */
#define SIW_VENDORT_PART_ID	0
#define SIW_MAX_QP		(1024 * 100)
#define SIW_MAX_QP_WR		(1024 * 32)
#define SIW_MAX_ORD		128
#define SIW_MAX_IRD		128
#define SIW_MAX_SGE_PBL		256	/* max num sge's for PBL */
#define SIW_MAX_SGE_RD		1	/* iwarp limitation. we could relax */
#define SIW_MAX_CQ		(1024 * 100)
#define SIW_MAX_CQE		(SIW_MAX_QP_WR * 100)
#define SIW_MAX_MR		(SIW_MAX_QP * 10)
#define SIW_MAX_PD		SIW_MAX_QP
#define SIW_MAX_MW		0	/* to be set if MW's are supported */
#define SIW_MAX_FMR		SIW_MAX_MR
#define SIW_MAX_SRQ		SIW_MAX_QP
#define SIW_MAX_SRQ_WR		(SIW_MAX_QP_WR * 10)
#define SIW_MAX_CONTEXT		SIW_MAX_PD

/*
 * Smallest MR page size SIW knows how to drive. Used both to size the
 * advertised page_size_cap bitmask and to bound per-PBE iteration in the
 * TX path so it works on kernels with PAGE_SIZE > SIW_MR_MIN_PAGE_SIZE.
 */
#define SIW_MR_MIN_PAGE_SIZE	4096

/*
 * Upper bound on the number of fragments siw_tx_hdt() may build for one
 * FPDU: up to one slot per SIW_MR_MIN_PAGE_SIZE-sized chunk of the
 * 64 KiB max payload, plus room for SGE-boundary unsharing and the
 * iWARP header / trailer. Used to size both the per-iteration counter
 * (MAX_ARRAY in siw_qp_tx.c) and the per-QP scratch arrays in
 * struct siw_iwarp_tx (iov_scratch[], page_array_scratch[], etc.).
 *
 * Sizing uses SIW_MR_MIN_PAGE_SIZE rather than PAGE_SIZE so an MR
 * registered with sub-PAGE_SIZE pages (see mr_min_page_4k) on a
 * large-page kernel still fits.
 */
#define SIW_TX_HDT_MAX_FRAGS	((0xffff / SIW_MR_MIN_PAGE_SIZE) + 1 + \
				 (2 * (SIW_MAX_SGE - 1) + 2))

#define SENDPAGE_THRESH		PAGE_SIZE /* min bytes for using sendpage() */
#define SQ_USER_MAXBURST	100 //was 10

#define SIW_TX_COMP_WAIT_ACK		1 /* Delay TX Completion until ACK Received */
#define SIW_TX_COMP_ACK_HISTORY		0 /* Number of completed WQEs to keep in context for debug */
#define SIW_TX_COMP_ACK_ON_WQ		1 /* Do TX Completion ACK work on WQ */

#define SIW_SRQ_EVENT_ON_WQ		1
#define SIW_SRQ_WAIT_LIST		1

/* QPs and CQs are allocated during connect so we don't want to use vmalloc or it might timeout */
#define SIW_NO_VMALLOC_FOR_KVERBS_QP	1
#define SIW_NO_VMALLOC_FOR_KVERBS_CQ	1
/* Not an issue for SRQ */
#define SIW_NO_VMALLOC_FOR_KVERBS_SRQ	0

#define SIW_WR_ACK_MAX_OUTSTANDING	128

/* Ported from SIW Upstream 5.4:
 * Allows listening on multiple IF addresses with single listener
 * Doesn't work for loopback if INET_MATCH does not check both the dest interface (__dif)
 * and source interface (__sdif) parameters.
 */

#define SIW_LISTEN_USE_BOUND_DEV_IF	KS_INET_MATCH_HAS_SDIF | KS_HAS_NEW_INET_MATCH_LOWER | KS_HAS_NEW_INET_MATCH_CAPS

#define SIW_ENABLE_PANIC_REMOTE_ON_RX_ERR	KS_HAS_KERNEL_SENDMSG_LOCKED

/* Needed for when sharing a CQ between multiple QPs or to have SCQ/RCQ completion run on different CPUs */
#define SIW_CQ_NOTIFY_WORK_QP_INDEPENDENT 1

/* Track SRQ RQEs to debug SRQ Empty */
#define SIW_SRQ_RQE_TRACK 1
#define SIW_SRQ_RQE_TRACK_LAST_RET_TIMEOUT	(30 * HZ)

#define SIW_TRACK_QP_WRITE_LOCK		1

#define SIW_RESCHED_QP_TX_IN_USE	1

/* Do qp_get/qp_put when putting QP ptr in CQE - Can get stuck in siw_destroy_qp if CQ is not drained */
#define SIW_CQE_REFCOUNT_QP		0

/* If there are SQEs or RQEs waiting to be flushed because the QP is locked,
 * or the CQ is full. Then schedule (or reschedule) the work with this delay (100ms)
 */
//#define SIW_FLUSH_XQES_WORK_DELAY	(HZ / 10)
/* Update. Delay does not seem to be needed, the rescheduling seems to be enough. */
#define SIW_FLUSH_XQES_WORK_DELAY	(0)

/* Log CEP activity in a ring buffer inside the CEP */
#define SIW_CEP_LOG_RING_BUF_SIZE	8192
#define SIW_CEP_LOG_PRINTF_BUF_SIZE	512

/* Timeouts */
#define SIW_IN_USE_ASSERT_TIMEOUT	(1 * HZ) /* Assert if QP has been in_use for longer than this */
#define SIW_QP_SQ_PROCESS_LOG_TIMEOUT	(HZ / 10)
#define SIW_QP_SQ_PROCESS_WARN_TIMEOUT	(HZ / 10)
#define SIW_KERNEL_SENDMSG_LOG_TIMEOUT	(HZ / 20)
#define SIW_KERNEL_SENDMSG_WARN_TIMEOUT (HZ / 20)
#define SIW_RUN_SQ_DELAY_LOG		(HZ / 5)
#define SIW_RUN_SQ_DELAY_WARN		(HZ / 5)
#define SIW_CQ_NOTIFY_WORK_DELAY_LOG	(HZ / 2)
#define SIW_CQ_NOTIFY_WORK_DELAY_WARN	(HZ / 2)
#define SIW_CQ_NOTIFY_TASK_DELAY_LOG	(HZ / 10)
#define SIW_CQ_NOTIFY_TASK_DELAY_WARN	(HZ)
#define SIW_CQ_HANDLER_TIMEOUT_LOG	(HZ / 5)
#define SIW_CQ_HANDLER_TIMEOUT_WARN	(HZ / 5)

/* Time to wait for QP users to process CQEs after flush in destroy_qp */
#define SIW_QP_SQ_CQ_DRAIN_TIMEOUT	(HZ)
#define SIW_QP_RQ_CQ_DRAIN_TIMEOUT	(HZ)

/* For testing siw_connect failures */
#define SIW_CONNECT_FAIL_TEST		0
#define SIW_CONNECT_FAIL_TEST_N		40

#if defined __NR_rdma_db
#define SIW_DB_SYSCALL
#endif

#if IB_VERBS_PORT_NUM_IS_U32
typedef u32 t_ib_port;
#else
typedef u8 t_ib_port;
#endif
#if KS_HAS_SCHED_SIGNAL_HEADER
#include <linux/sched/signal.h>
#endif
#if KS_HAS_SCHED_TASK_HEADER
#include <linux/sched/task.h>
#endif

extern int *siw_panic_on_warn;

extern unsigned relax_timeouts;

struct siw_devinfo {
	unsigned		device;
	unsigned		version;

	/* close match to ib_device_attr where appropriate */
	u32			vendor_id;
	u32			vendor_part_id;
	u32			sw_version;
	int			max_qp;
	int			max_qp_wr;
	int			max_ord; /* max. outbound read queue depth */
	int			max_ird; /* max. inbound read queue depth */

	enum ib_device_cap_flags	cap_flags;
	int			max_sge;
	int			max_sge_rd;
	int			max_cq;
	int			max_cqe;
	u64			max_mr_size;
	int			max_mr;
	int			max_pd;
	int			max_mw;
	int			max_fmr;
	int			max_srq;
	int			max_srq_wr;
	int			max_srq_sge;
	/* end ib_device_attr */

	enum siw_if_type	iftype;
};

#define USE_SQ_KTHREAD

struct siw_dev {
	struct ib_device	ofa_dev;
	struct list_head	list;
	struct net_device	*netdev;
	struct siw_devinfo	attrs;
	int			is_registered; /* Registered with OFA core */

	/* physical port state (only one port per device) */
	enum ib_port_state	state;

	/* object management */
	struct list_head	cep_list;
	struct list_head	qp_list;
	spinlock_t		idr_lock;
	struct idr		qp_idr;
	struct idr		cq_idr;
	struct idr		pd_idr;
	struct idr		mem_idr;	/* MRs & MWs */
	struct idr		srq_idr;

	/* active objects statistics */
	atomic_t		num_qp;
	atomic_t		num_cq;
	atomic_t		num_pd;
	atomic_t		num_mem;
	atomic_t		num_srq;
	atomic_t		num_cep;
	atomic_t		num_ctx;

	struct dentry		*debugfs;

#ifdef USE_SQ_KTHREAD
	/* NUMA-local TX vector CPUs for this device (when tx_cpus=2); NULL otherwise */
	int			*tx_vector_cpu;
	int			num_tx_vector;
#endif
};

struct siw_objhdr {
	u32			id;	/* for idr based object lookup */
	struct kref		ref;
	struct siw_dev		*sdev;
};

struct siw_uobj {
	struct list_head	list;
	void	*addr;
	u32	size;
	u32	key;
};

struct siw_ucontext {
	struct ib_ucontext	ib_ucontext;
	struct siw_dev		*sdev;
	/* List of user mappable queue objects */
	spinlock_t		uobj_lock;
	struct list_head	uobj_list;
	u32			uobj_key;
};

struct siw_pd {
	/* For the new IB it is important ofa_pd is first member*/
	struct ib_pd		ofa_pd;
	struct siw_objhdr	hdr;
};

enum siw_access_flags {
	SR_MEM_LREAD	= (1<<0),
	SR_MEM_LWRITE	= (1<<1),
	SR_MEM_RREAD	= (1<<2),
	SR_MEM_RWRITE	= (1<<3),
	SR_MEM_RATOMIC	= (1<<4),

	SR_MEM_FLAGS_LOCAL =
		(SR_MEM_LREAD | SR_MEM_LWRITE),
	SR_MEM_FLAGS_REMOTE =
		(SR_MEM_RWRITE | SR_MEM_RREAD)
};

#define SIW_STAG_MAX	0xffffffff

struct siw_mr;

/*
 * siw presentation of user memory registered as source
 * or target of RDMA operations.
 */

struct siw_page_chunk {
	struct page **p;
};

struct siw_umem {
	struct siw_page_chunk	*page_chunk;
	int			num_pages;
	u64			fp_addr;	/* First page base address */
	struct pid		*pid;
	struct mm_struct	*mm_s;
	struct work_struct	work;
};

struct siw_pble {
	u64	addr;		/* Address of assigned user buffer */
	u64	size;		/* Size of this entry */
	u64	pbl_off;	/* Total offset form start of PBL */
};

struct siw_pbl {
	//bool ulp_map;   /* Mapping starts by ULP and finalized upon IB-WR-REG-MR */
	unsigned int	num_buf;
	unsigned int	max_buf;
	unsigned int 	pbe_fixed_shift;
	struct siw_pble	pbe[];
};

/*
 * generic memory representation for registered siw memory.
 * memory lookup always via higher 24 bit of stag (stag index).
 * the stag is stored as part of the siw object header (id).
 * object relates to memory window if embedded mr pointer is valid
 */
struct siw_mem {
	struct siw_objhdr	hdr;

	struct siw_mr	*mr;	/* assoc. MR if MW, NULL if MR */
	u64	va;		/* VA of memory */
	u64	len;		/* amount of memory bytes */

	u32	stag_valid:1,		/* VALID or INVALID */
		is_pbl:1,		/* PBL or user space mem */
		is_zbva:1,		/* zero based virt. addr. */
		mw_bind_enabled:1,	/* check only if MR */
		remote_inval_enabled:1,	/* VALID or INVALID */
		consumer_owns_key:1,	/* key/index split ? */
		rsvd:26;

	enum siw_access_flags	perms;	/* local/remote READ & WRITE */
};

#define SIW_MEM_IS_MW(m)	((m)->mr != NULL)

/*
 * MR and MW definition.
 * Used OFA structs ib_mr/ib_mw holding:
 * lkey, rkey, MW reference count on MR
 */
struct siw_mr {
	struct ib_mr	ofa_mr;
	struct siw_mem	mem;
	struct rcu_head rcu;
	union {
		struct siw_umem	*umem;
		struct siw_pbl	*pbl;
		void *mem_obj;
	};
	struct siw_pd	*pd;
};

struct siw_mw {
	struct ib_mw	ofa_mw;
	struct siw_mem	mem;
	struct rcu_head rcu;
};

//omril: Taken from siw_obj.h so NVMesh can use it
static inline struct siw_mr *siw_mr_ofa2siw(struct ib_mr *ofa_mr)
{
	return container_of(ofa_mr, struct siw_mr, ofa_mr);
}

/********** WR definitions ****************/

enum siw_wr_state {
	SR_WR_IDLE		= 0,
	SR_WR_QUEUED		= 1,	/* processing has not started yet */
	SR_WR_WAIT_ORQ		= 2,	/* waiting for ORQ (either fence or ORQ full) */
	SR_WR_INPROGRESS	= 3	/* initiated processing of the WR */
};

union siw_mem_resolved {
	struct siw_mem	*obj;	/* reference to registered memory */
	char		*buf;	/* linear kernel buffer */
};

struct siw_qp;

struct siw_sqe_md {
	ktime_t		post_send_time;
	ktime_t		sent_time;
	ktime_t		ack_time;
	uint16_t	tx_cpu;
	uint16_t	unused[3];
};

struct siw_rqe_md {
	ktime_t 	first_ddp_recv_time;
	ktime_t 	last_ddp_recv_time;
	uint16_t	rx_cpu;
	uint16_t	rx_queue;
	uint32_t	rx_skb_hash;
};

#ifdef SIW_DEBUG_SRQ
struct siw_srqe_md {
	unsigned long post_bt[5];
	int post_pid;
	int unused;
};
#endif

struct siw_wqe {
	union {
		struct siw_sqe	sqe;
		struct siw_rqe	rqe;
	};
	union {
		struct siw_sqe_md sqe_md;
		struct siw_rqe_md rqe_md;
	};
	union siw_mem_resolved	mem[SIW_MAX_SGE]; /* per sge's resolved mem */

	enum siw_wr_state	wr_status;
	enum siw_wc_status	wc_status;
	u32			bytes;		/* total bytes to process */
	u32			processed;	/* bytes processed */
	int			error;
};

struct siw_cqe_md {
	union {
		struct {
			ktime_t 	post_send_time;
			ktime_t 	sent_time;
			ktime_t 	ack_time;
			uint16_t	tx_cpu;
			uint16_t	tx_unused0;
			uint32_t	tx_unused1;
		} tx_md;
		struct {
			ktime_t 	first_ddp_recv_time;
			ktime_t 	last_ddp_recv_time;
			uint16_t 	rx_cpu;
			uint16_t 	rx_queue;
			uint32_t 	rx_skb_hash;
		} rx_md;
	};
};

struct siw_cq {
	struct ib_cq		ofa_cq;
	struct siw_objhdr	hdr;
	enum siw_notify_flags	*notify;
	spinlock_t		lock;
	struct siw_cqe		*queue;
	struct siw_cqe_md	*cqe_md; /* completion queue element MD for kernel users */
	u32			cq_put;
	u32			cq_get;
	u32			num_cqe;
	int			kernel_verbs;
	int			comp_vector;

	bool notify_on_wq;

	struct tasklet_struct 	notify_task;

	struct work_struct notify_work;

	atomic_t dying;
#if !KS_IB_CREATE_CQ_HAS_IB_DEVICE
	struct completion free_comp;
#endif
	unsigned long notify_sched_jif;
};

enum siw_qp_state {
	SIW_QP_STATE_IDLE	= 0,
	SIW_QP_STATE_RTR	= 1,
	SIW_QP_STATE_RTS	= 2,
	SIW_QP_STATE_CLOSING	= 3,
	SIW_QP_STATE_TERMINATE	= 4,
	SIW_QP_STATE_ERROR	= 5,
	SIW_QP_STATE_MORIBUND	= 6, /* destroy called but still referenced */
	SIW_QP_STATE_UNDEF	= 7,
	SIW_QP_STATE_COUNT	= 8
};

enum siw_qp_flags {
	SIW_RDMA_BIND_ENABLED	= (1 << 0),
	SIW_RDMA_WRITE_ENABLED	= (1 << 1),
	SIW_RDMA_READ_ENABLED	= (1 << 2),
	SIW_SIGNAL_ALL_WR	= (1 << 3),
	/*
	 * QP currently being destroyed
	 */
	SIW_QP_IN_DESTROY	= (1 << 8)
};

enum siw_qp_attr_mask {
	SIW_QP_ATTR_STATE		= (1 << 0),
	SIW_QP_ATTR_ACCESS_FLAGS	= (1 << 1),
	SIW_QP_ATTR_LLP_HANDLE		= (1 << 2),
	SIW_QP_ATTR_ORD			= (1 << 3),
	SIW_QP_ATTR_IRD			= (1 << 4),
	SIW_QP_ATTR_SQ_SIZE		= (1 << 5),
	SIW_QP_ATTR_RQ_SIZE		= (1 << 6),
	SIW_QP_ATTR_MPA			= (1 << 7)
};

struct siw_mpa_attrs {
	__u8	marker_rcv; /* always 0 */
	__u8	marker_snd; /* always 0, consider support */
	__u8	crc;
	__u8	wr_ack;
};

struct siw_sk_upcalls {
	void	(*sk_state_change)(struct sock *sk);
	void	(*sk_data_ready)(struct sock *sk, int bytes);
	void	(*sk_write_space)(struct sock *sk);
	void	(*sk_error_report)(struct sock *sk);
};

struct siw_sq_work {
	struct work_struct	work;
};

struct siw_srq {
	struct ib_srq		ofa_srq;
	struct siw_objhdr	hdr;
	struct siw_pd		*pd;
	atomic_t		rq_index;
	spinlock_t		lock;
	u32			max_sge;
	atomic_t		space;	/* current space for posting wqe's */
	u32			limit;	/* low watermark for async event */
	struct siw_rqe		*recvq;
	u32			rq_put;
	u32			rq_get;
	u32			num_rqe;	/* max # of wqe's allowed */
	char			armed;	/* inform user if limit hit */
	char			kernel_verbs; /* '1' if kernel client */
	struct completion	free_comp;

	/* Used to schedule the SRQ event on WQ
	 * It only supports 1 pending */
	struct work_struct event_work;
	enum ib_event_type event_work_type;
	atomic_t			event_work_sched;

#if SIW_SRQ_RQE_TRACK
	struct {
		/* Tracking RQEs */
		unsigned long n_q;
		unsigned long n_recv;
		unsigned long n_rsvd;
		unsigned long n_cq;
		unsigned long n_ulp;
		unsigned long max_q;
		unsigned long n_qp;

		/* List of QPs with reserved RQEs */
		struct list_head rsvd_qp_head;
		/* List of QPs with RQEs in RX ctx */
		struct list_head recv_qp_head;

		/* Jiffies of last time an RQE was returned to the SRQ */
		unsigned long last_ret_jif;
	} rqe_track;
#endif

#if SIW_SRQ_WAIT_LIST
	/* List of QPs waiting for RQEs */
	struct list_head wait_qp_head;
#endif

#ifdef SIW_DEBUG_SRQ
	struct siw_srqe_md *srqe_md;
#endif
};

struct siw_qp_attrs {
	enum siw_qp_state	state;
	char			terminate_buffer[52];
	u32			terminate_msg_length;
	u32			ddp_rdmap_version; /* 0 or 1 */
	char			*stream_msg_buf;
	u32			stream_msg_buf_length;
	u32			rq_hiwat;
	u32			sq_size;
	u32			rq_size;
	u32			orq_size;
	u32			irq_size;
	u32			sq_max_sges;
	u32			sq_max_sges_rdmaw;
	u32			rq_max_sges;
	struct siw_mpa_attrs	mpa;
	enum siw_qp_flags	flags;

	struct socket		*llp_stream_handle;
};

enum siw_tx_ctx {
	SIW_SEND_HDR = 0,	/* start or continue sending HDR */
	SIW_SEND_DATA = 1,	/* start or continue sending DDP payload */
	SIW_SEND_TRAILER = 2,	/* start or continue sending TRAILER */
	SIW_SEND_SHORT_FPDU = 3 /* send whole FPDU hdr|data|trailer at once */
};

enum siw_rx_state {
	SIW_GET_HDR = 0,	/* await new hdr or within hdr */
	SIW_GET_DATA_START = 1,	/* start of inbound DDP payload */
	SIW_GET_DATA_MORE = 2,	/* continuation of (misaligned) DDP payload */
	SIW_GET_TRAILER	= 3,	/* await new trailer or within trailer */
	SIW_WAIT_RQE = 4, /* (S)RQ is currently empty, wait for entry */
};

struct siw_xqe_flush {
	struct list_head link;
	u64 id;
	u8 opcode;
};

struct siw_iwarp_rx {
	struct sk_buff		*skb;
	union iwarp_hdrs	hdr;
	struct mpa_trailer	trailer;
	/*
	 * local destination memory of inbound iwarp operation.
	 * valid, according to wqe->wr_status
	 */
	struct siw_wqe		wqe_active;

	struct shash_desc	*mpa_crc_hd;
	/*
	 * Next expected DDP MSN for each QN +
	 * expected steering tag +
	 * expected DDP tagget offset (all HBO)
	 */
	u32			ddp_msn[RDMAP_UNTAGGED_QN_COUNT];
	u32			ddp_stag;
	u64			ddp_to;
	u64			first_ddp_to;

	/*
	 * For each FPDU, main RX loop runs through 3 stages:
	 * Receiving protocol headers, placing DDP payload and receiving
	 * trailer information (CRC + eventual padding).
	 * Next two variables keep state on receive status of the
	 * current FPDU part (hdr, data, trailer).
	 */
	int			fpdu_part_rcvd;/* bytes in pkt part copied */
	int			fpdu_part_rem; /* bytes in pkt part not seen */

	int			skb_new;      /* pending unread bytes in skb */
	int			skb_offset;   /* offset in skb */
	int			skb_copied;   /* processed bytes in skb */

	int			sge_idx;	/* current sge in rx */
	unsigned int		sge_off;	/* already rcvd in curr. sge */

	enum siw_rx_state	state;

	u32			inval_stag;

	union {
		u32		locked_flags;
		struct {
			u32 	rx_suspend:1,	   /* stop rcv DDP segs. */
				locked_bits:30;
		};
	};

	union {
		u8		rx_ctx_flags;
		struct {
			u8	first_ddp_seg:1,   /* this is first DDP seg */
				more_ddp_segs:1,   /* more DDP segs expected */
				unused:2,
				prev_rdmap_opcode:4; /* opcode of prev msg */
		};
	};
	char			pad;		/* # of pad bytes expected */

	struct iwarp_ctrl prev_hdr_ctrl; /* previous header ctrl part */

	struct delayed_work	rx_work;
	int n_retries;

	struct list_head flush_rqes;

	atomic_t rcq_qp_ref_cnt;

	struct completion rcq_qp_comp;

#ifdef SIW_DEBUG_RX_CRC
#define SIW_DEBUG_RX_CRC_MAX_FPDU 65536
#define SIW_DEBUG_RX_CRC_TRACE_LOG_SZ 65536
	void *curr_fpdu;
	size_t curr_fpdu_len, curr_fpdu_bytes, curr_fpdu_pad, curr_fpdu_crc_bytes, curr_fpdu_crc_bytes_rem;
	size_t curr_fpdu_skb_copied, curr_fpdu_skb_offset;
	u32 curr_fpdu_seq;
	struct crypto_shash *curr_fpdu_shash;
	struct shash_desc	*curr_fpdu_shash_desc;
	void *prev_fpdu;
	size_t prev_fpdu_len, prev_fpdu_pad;
	u32 prev_fpdu_seq;
	char *crc_trace_log;
	u16 crc_trace_prod;
	unsigned crc_trace_line_prod;
#endif
};

#define siw_rx_data(qp, rctx)	\
	(iwarp_pktinfo[__rdmap_opcode(&rctx->hdr.ctrl)].proc_data(qp, rctx))

/*
 * Shorthands for short packets w/o payload
 * to be transmitted more efficient.
 */
struct siw_send_pkt {
	struct iwarp_send	send;
	__be32			crc;
};

struct siw_write_pkt {
	struct iwarp_rdma_write	write;
	__be32			crc;
};

struct siw_rreq_pkt {
	struct iwarp_rdma_rreq	rreq;
	__be32			crc;
};

struct siw_rresp_pkt {
	struct iwarp_rdma_rresp	rresp;
	__be32			crc;
};

struct siw_areq_pkt {
	struct iwarp_rdma_areq	areq;
	__be32			crc;
};

struct siw_aresp_pkt {
	struct iwarp_rdma_aresp	aresp;
	__be32			crc;
};

struct siw_iwarp_tx_fpdu;
int siw_qp_sq_flush_sent_fpdus(struct siw_qp *qp);
int siw_tx_complete_ack_seq(struct siw_qp *qp);

void siw_cq_notify(struct siw_cq *cq, u32 flags, bool force);
bool siw_schedule_cq_notify_work(struct siw_qp *qp, struct siw_cq *cq);

enum siw_orq_fence {
	SIW_ORQ_FENCE_NONE = 0, 	/* No ORQ Fence */
	SIW_ORQ_FENCE_FULL,		/* ORQ is Full */
	SIW_ORQ_FENCE_WR,		/* WR Fence */
};

union siw_iwarp_tx_sent_fpdu_notify {
	struct {
		u32 end_seq;
		u32 armed;
	};
	u64 all;
};

enum siw_iwarp_tx_in_use_flags {
	SIW_IWARP_TX_IN_USE = (1 << 0),
	SIW_IWARP_TX_REQ_RESCHED = (1 << 1),
};

/*
 * Per-QP debug ring buffer that records the state of every siw_tx_hdt()
 * inner-loop iteration. We use it to triage zero-copy TX bugs where
 * page_array[]/page_len[] disagree with what siw_tcp_sendpages() expects
 * (e.g. NULL page_array[i] entries, page_len[i] > PAGE_SIZE, etc.).
 *
 * The trace is inspected post-mortem from a crash dump -- walk back from
 * tx_ctx.hdt_trace_head to see the last SIW_TX_HDT_TRACE_ENTRIES iterations,
 * then cross-check page_array[]/page_len[] on the stack of siw_tx_hdt /
 * siw_tcp_sendpages.
 *
 * Costs ~SIW_TX_HDT_TRACE_ENTRIES * sizeof(siw_tx_hdt_dbg_entry) bytes per
 * QP, so it's compile-time gated by SIW_TX_HDT_TRACE -- comment out the
 * #define below to disable.
 */
#define SIW_TX_HDT_TRACE 1

#ifdef SIW_TX_HDT_TRACE

#define SIW_TX_HDT_TRACE_ENTRIES 64u

#define SIW_TX_HDT_TRACE_FL_MERGED		(1u << 0)
#define SIW_TX_HDT_TRACE_FL_IS_KVA		(1u << 1)
#define SIW_TX_HDT_TRACE_FL_IS_KVA_VM		(1u << 2)
#define SIW_TX_HDT_TRACE_FL_IS_PBL		(1u << 3)
#define SIW_TX_HDT_TRACE_FL_USE_SENDPAGE	(1u << 4)
#define SIW_TX_HDT_TRACE_FL_INTRA_OFF_OK	(1u << 5)  /* intra_off == prev_page_off + decode(prev page_len) */
#define SIW_TX_HDT_TRACE_FL_SAME_PAGE		(1u << 6)  /* p == page_array[seg-1] */

struct siw_tx_hdt_dbg_entry {
	u64	sge_laddr;	/* sge->laddr for this iter's SGE */
	u64	p;		/* struct page * returned by lookup (cast) */
	u64	prev_page;	/* page_array[seg-1] at decision time, 0 if N/A */

	u32	sge_off;	/* sge_off BEFORE this iter consumed plen */
	u32	sge_len;	/* sge_len BEFORE this iter consumed plen */
	u32	bytes_unsent;	/* c_tx->bytes_unsent at iter start */
	u32	data_len;	/* siw_tx_hdt's data_len at iter start */

	u32	jif;		/* (u32)jiffies, for timestamp */
	u32	pbl_idx;	/* PBL index hint after siw_pbl_get_paddr() */
	u32	pbe_remaining;	/* bytes remaining in current PBE */

	u16	intra_off;	/* paddr & ~PAGE_MASK for PBL, virt for kva/umem */
	u16	plen;		/* bytes this iter contributes */
	u16	seg_before;	/* page_array[] index BEFORE the merge/new action */
	u16	seg_at_sge_start; /* seg captured at the start of this SGE */
	u16	prev_page_len;	/* page_len[seg-1] BEFORE the action (raw u16) */
	u16	page_len_after;	/* page_len[updated_idx] AFTER the action (raw u16) */
	u16	prev_page_off;	/* page_off[seg-1] BEFORE the action (raw u16) */
	u16	page_off_after;	/* page_off[updated_idx] AFTER the action (raw u16) */

	u8	sge_idx;	/* SGE index within the WQE */
	u8	flags;		/* SIW_TX_HDT_TRACE_FL_* */
	u8	pad[2];
};

#endif /* SIW_TX_HDT_TRACE */

struct siw_iwarp_tx {
	union {
		union iwarp_hdrs		hdr;

		/* Generic part of FPDU header */
		struct iwarp_ctrl		ctrl;
		struct iwarp_ctrl_untagged	c_untagged;
		struct iwarp_ctrl_tagged	c_tagged;

		/* FPDU headers */
		struct iwarp_rdma_write		rwrite;
		struct iwarp_rdma_rreq		rreq;
		struct iwarp_rdma_rresp		rresp;
		struct iwarp_rdma_areq		areq;
		struct iwarp_rdma_aresp		aresp;
		struct iwarp_terminate		terminate;
		struct iwarp_send		send;
		struct iwarp_send_inv		send_inv;
//omril: what the different btw these 2? cna atomic save bytes by using the short version?
		/* complete short FPDUs */
		struct siw_send_pkt		send_pkt;
		struct siw_write_pkt		write_pkt;
		struct siw_rreq_pkt		rreq_pkt;
		struct siw_rresp_pkt		rresp_pkt;
		struct siw_areq_pkt		areq_pkt;
		struct siw_aresp_pkt		aresp_pkt;
	} pkt;

	struct mpa_trailer			trailer;
	/* DDP MSN for untagged messages */
	u32			ddp_msn[RDMAP_UNTAGGED_QN_COUNT];

	enum siw_tx_ctx		state;
	wait_queue_head_t	waitq;
	u16			ctrl_len;	/* ddp+rdmap hdr */
	u16			ctrl_sent;
	int			burst;

	int			bytes_unsent;	/* ddp payload bytes */

	struct shash_desc	*mpa_crc_hd;

	atomic_t		in_use;		/* tx currently under way */
	pid_t			in_use_pid;

	/* Flags in use by tx context only (tx thread or WQ) */
	union {
		u8 		tx_ctx_flags;
		struct {
			u8		do_crc:1;	/* do crc for segment */
			u8		use_sendpage:1;	/* send w/o copy */
			u8		new_tcpseg:1;	/* start new tcp segment */
			u8		unused_tx_ctx_bits:5;
		};
	};

	u8			pad;		/* # pad in current fpdu */
	u16			fpdu_len;	/* len of FPDU to tx */

	/* Flags that communicate between tx ctx and other ctx's (SQ/ORQ spinlock or QP state-lock must be write acquired) */
	union {
		u32		sq_locked_flags;
		struct {
			u32		tx_suspend:1;	/* stop sending DDP segs. */
			u32		sq_flushed:1;   /* SQ has been flushed */
			u32		unused_sq_locked_bits:30;
		};
	};

	union {
		u32		orq_locked_flags;
		struct {
			u32		orq_fence:2;	/* enum siw_tx_orq_fence */
			u32		unused_locked_bits:30;
		};
	};

	int			tcp_seglen;	/* remaining tcp seg space */

	struct siw_wqe		wqe_active;

	int			sge_idx;	/* current sge in tx */
	u32			sge_off;	/* already sent in curr. sge */
	int			in_syscall;	/* TX out of user context */

	void		*trailer_page_virt; /* When doing zcopy_tx, allow trailer to be sent using send_pages */
	struct page *trailer_page;

	bool ack_signal_wr;	/* Request responder to acknowledge signalled RDMA WRITE */

	struct list_head flush_sqes;

	atomic_t scq_qp_ref_cnt;

	struct completion scq_qp_comp;

#ifdef SIW_TX_COMP_WAIT_ACK
	struct siw_iwarp_tx_fpdu *fpdu_in_prog;
	struct list_head sent_fpdus; // struct siw_iwarp_tx_fdpu
	struct list_head completed_fpdus; // struct siw_iwarp_tx_fdpu
	union siw_iwarp_tx_sent_fpdu_notify sent_fpdu_notify;
	atomic_t n_completed_fpdus;
	int n_completed_fpdus_in_list;
	/* Set when the previous FPDU finished and we committed to building
	 * the next one, but siw_prepare_fpdu() hasn't completed yet (e.g.
	 * Site B kzalloc returned NULL). Resume guard before next_segment
	 * in siw_qp_sq_proc_tx() retries the prep until it succeeds.
	 * See NVMESH-8981.
	 */
	bool fpdu_needs_prepare;
#endif

	/*
	 * Per-FPDU scratch built up by siw_tx_hdt()'s inner loop. Hoisted
	 * out of the function's stack frame to:
	 *   (a) keep the frame within the kernel's 1 KiB
	 *       -Wframe-larger-than budget without #pragma suppression;
	 *   (b) save ~800 B on top of the deep kernel_sendmsg ->
	 *       tcp_sendmsg_locked -> ip_xmit call chain;
	 *   (c) make the *last-written* TX fragment layout inspectable
	 *       post-mortem from a saved siw_qp even when
	 *       SIW_TX_HDT_TRACE is compiled out (the trace below
	 *       records the *history* of mutations -- these fields
	 *       preserve the most recent value of what was mutated).
	 *
	 * Producer-only: valid only while tx_ctx.in_use != 0 (gated by
	 * the atomic_cmpxchg in siw_qp_sq_process()). The local `seg`
	 * counter on the siw_tx_hdt() stack bounds the live region:
	 * entries [0, seg) reflect the FPDU currently being assembled;
	 * entries [seg, SIW_TX_HDT_MAX_FRAGS) are stale leftovers from a
	 * previous call. siw_tx_hdt() always writes a slot before it
	 * reads it within the loop, so no zero-init is needed.
	 *
	 * page_off / page_len use the u16 PAGE_SIZE-as-0 sentinel
	 * encoding (see siw_encode_page_len()) so 64 KiB-page kernels
	 * still fit; page_off is in [0, PAGE_SIZE) and fits u16
	 * directly.
	 */
	struct kvec	iov_scratch[SIW_TX_HDT_MAX_FRAGS];
	struct page	*page_array_scratch[SIW_TX_HDT_MAX_FRAGS];
	u16		page_off_scratch[SIW_TX_HDT_MAX_FRAGS];
	u16		page_len_scratch[SIW_TX_HDT_MAX_FRAGS];

#ifdef SIW_TX_HDT_TRACE
	/*
	 * Circular log of siw_tx_hdt() inner-loop iterations, see the
	 * struct siw_tx_hdt_dbg_entry comment above for details.
	 *
	 * Producer-only (the TX path holds the qp tx-in-use flag), so no
	 * locking needed. Readers (i.e. someone poking around in a crash
	 * dump) should treat hdt_trace_head as the *next* slot to write,
	 * i.e. the most recent entry is hdt_trace[(head-1) & (N-1)].
	 */
	struct siw_tx_hdt_dbg_entry	hdt_trace[SIW_TX_HDT_TRACE_ENTRIES];
	u32				hdt_trace_head;
#endif
};

#if defined(SIW_DEBUG_TX_CRC) && !defined(SIW_TX_COMP_WAIT_ACK)
#error "SIW_DEBUG_TX_CRC requires SIW_TX_COMP_WAIT_ACK"
#endif

struct siw_qp {
	struct ib_qp		ofa_qp;
	struct siw_objhdr	hdr;
	struct list_head	devq;
	int			cpu;
	int			kernel_verbs;
	struct siw_iwarp_rx	rx_ctx;
	struct siw_iwarp_tx	tx_ctx;

	struct siw_cep		*cep;
	struct rw_semaphore	state_lock;
	atomic_t state_lock_failed;

#ifdef	SIW_TRACK_QP_WRITE_LOCK
	int state_lock_pid;
	int state_lock_line;
	const char *state_lock_fn;
	const char *state_lock_file;
#endif

	struct siw_pd		*pd;
	struct siw_cq		*scq;
	struct siw_cq		*rcq;
	struct siw_srq		*srq;

	struct siw_qp_attrs	attrs;

	struct siw_sqe		*sendq;	/* send queue element array */
	struct siw_sqe_md	*sendq_kern_md; /* send queue element MD for kernel users */
	uint32_t		sq_get;	/* consumer index into sq array */
	uint32_t		sq_put;	/* kernel prod. index into sq array */
#ifdef USE_SQ_KTHREAD
	struct llist_node	tx_list;
	struct llist_head	*tx_list_head;
	unsigned long		tx_list_jif;
#endif

	struct siw_sqe		*irq;	/* inbound read queue element array */
	uint32_t		irq_get;/* consumer index into irq array */
	uint32_t		irq_put;/* producer index into irq array */

	struct siw_rqe		*recvq;	/* recv queue element array */
	uint32_t		rq_get;	/* consumer index into rq array */
	uint32_t		rq_put;	/* kernel prod. index into rq array */

	struct siw_sqe		*orq; /* outbound read queue element array */
	uint32_t		orq_get;/* consumer index into orq array */
	uint32_t		orq_put;/* shared producer index for ORQ */

	spinlock_t		sq_lock;
	spinlock_t		rq_lock;
	spinlock_t		orq_lock;

	struct siw_sq_work	sq_work;

	bool notify_on_wq;
	struct work_struct cq_notify_work;
	unsigned long cq_notify_sched_jif;

	struct list_head srq_wait_link;

#if SIW_SRQ_RQE_TRACK
	struct list_head srq_rqe_link;
	enum {
		SRQ_RQE_IDLE = 0,
		SRQ_RQE_RSVD,
		SRQ_RQE_RECV,
	} srq_rqe_state;
#endif

	struct siw_rqe srq_rqe_ready;
	u32 srq_rqe_ready_count;

	struct delayed_work flush_work;

#if KS_IB_DEVICE_OPS_HAS_QP_SIZE
	struct completion free_comp;
#endif
};

#define lock_sq(qp)	spin_lock(&qp->sq_lock)
#define unlock_sq(qp)	spin_unlock(&qp->sq_lock)

#ifdef LOCK_WO_FLAG
#define lock_sq_rxsave(qp, flags) spin_lock_bh(&qp->sq_lock)
#define unlock_sq_rxsave(qp, flags) spin_unlock_bh(&qp->sq_lock)
#else
#define lock_sq_rxsave(qp, flags) spin_lock_irqsave(&qp->sq_lock, flags)
#define unlock_sq_rxsave(qp, flags) spin_unlock_irqrestore(&qp->sq_lock, flags)
#endif

#define lock_rq(qp)	spin_lock(&qp->rq_lock)
#define unlock_rq(qp)	spin_unlock(&qp->rq_lock)

#define lock_rq_rxsave(qp, flags) spin_lock_irqsave(&qp->rq_lock, flags)
#define unlock_rq_rxsave(qp, flags) spin_unlock_irqrestore(&qp->rq_lock, flags)

#define lock_srq(srq)	spin_lock(&srq->lock)
#define unlock_srq(srq)	spin_unlock(&srq->lock)

#define lock_srq_rxsave(srq, flags) spin_lock_irqsave(&srq->lock, flags)
#define unlock_srq_rxsave(srq, flags) spin_unlock_irqrestore(&srq->lock, flags)

#define lock_cq(cq) spin_lock(&cq->lock)
#define unlock_cq(cq)	spin_unlock(&cq->lock)

#ifdef SIW_TRACK_QP_WRITE_LOCK
#define write_lock_qp(qp) do {\
	down_write(&qp->state_lock);\
	qp->state_lock_pid = current->pid;\
	qp->state_lock_file = __FILE__;\
	qp->state_lock_fn = __func__;\
	qp->state_lock_line = __LINE__;\
} while(0)

#define try_write_lock_qp(qp) ({\
	int __rv = down_write_trylock(&qp->state_lock);\
	if (__rv) {\
		qp->state_lock_pid = current->pid;\
		qp->state_lock_file = __FILE__;\
		qp->state_lock_fn = __func__;\
		qp->state_lock_line = __LINE__;\
	}\
	__rv;\
})

#define write_unlock_qp(qp) do {\
	qp->state_lock_pid = -1;\
	qp->state_lock_file = NULL;\
	qp->state_lock_fn = NULL;\
	qp->state_lock_line = -1;\
	up_write(&qp->state_lock);\
} while(0)
#else
#define write_lock_qp(qp) down_write(&qp->state_lock)
#define try_write_lock_qp(qp) down_write_trylock(&qp->state_lock)
#define write_unlock_qp(qp) up_write(&qp->state_lock)
#endif

#define dprint2(fmt, args...)					\
	do {								\
		if (1) {				\
			if (!in_interrupt())				\
				pr_info("(%5d/%1d) %s (%s/%d)" fmt,		\
					current->pid,			\
					current_thread_info()->cpu,	\
					__func__, FILENAME, __LINE__, ## args);		\
			else						\
				pr_info("( irq /%1d) %s (%s/%d)" fmt,		\
					current_thread_info()->cpu,	\
					__func__, FILENAME, __LINE__, ## args);		\
		}							\
	} while (0)

#if 0
#define lock_cq_rxsave(cq, flags) do { \
 dprint2("cq " dprint_ptr_str() " locking...\n", cq); \
 spin_lock_irqsave(&cq->lock, flags); \
 dprint2("cq " dprint_ptr_str() " locked!\n", cq); \
} while (0)

#define unlock_cq_rxsave(cq, flags) do { \
 dprint2("cq " dprint_ptr_str() " unlocking\n", cq); \
 spin_unlock_irqrestore(&cq->lock, flags); \
} while (0)

#else
#define lock_cq_rxsave(cq, flags) do { \
 spin_lock_irqsave(&cq->lock, flags); \
} while (0)

#define unlock_cq_rxsave(cq, flags) do { \
 spin_unlock_irqrestore(&cq->lock, flags); \
} while (0)

#endif

#define lock_orq(qp)	spin_lock(&qp->orq_lock)
#define unlock_orq(qp)	spin_unlock(&qp->orq_lock)

#ifdef LOCK_WO_FLAG
#define lock_orq_rxsave(qp, flags)	spin_lock_bh(&qp->orq_lock)
#define unlock_orq_rxsave(qp, flags)	spin_unlock_bh(&qp->orq_lock)
#else
#define lock_orq_rxsave(qp, flags)	spin_lock_irqsave(&qp->orq_lock, flags)
#define unlock_orq_rxsave(qp, flags)\
	spin_unlock_irqrestore(&qp->orq_lock, flags)
#endif

#define RX_QP(rx)		container_of(rx, struct siw_qp, rx_ctx)
#define TX_QP(tx)		container_of(tx, struct siw_qp, tx_ctx)
#define QP_ID(qp)		((qp)->hdr.id)
#define OBJ_ID(obj)		((obj)->hdr.id)
#define RX_QPID(rx)		QP_ID(RX_QP(rx))
#define TX_QPID(tx)		QP_ID(TX_QP(tx))

/* helper macros */
#define tx_wqe(qp)		(&(qp)->tx_ctx.wqe_active)
#define rx_wqe(qp)		(&(qp)->rx_ctx.wqe_active)
#define rx_mem(qp)		((qp)->rx_ctx.wqe_active.mem[0].obj)
#define tx_type(wqe)		((wqe)->sqe.opcode)
#define rx_type(wqe)		((wqe)->rqe.opcode)
#define tx_flags(wqe)		((wqe)->sqe.flags)
#define rx_flags(wqe)		((wqe)->rqe.flags)
#define list_entry_wqe(pos)	list_entry(pos, struct siw_wqe, list)
#define list_first_wqe(pos)	list_first_entry(pos, struct siw_wqe, list)

#define TX_ACTIVE(qp)		(tx_wqe(qp).status != SIW_WR_IDLE)
#define TX_ACTIVE_RRESP(qp)	(TX_ACTIVE(qp) &&\
			tx_type(tx_wqe(qp)) == SIW_OP_READ_RESP)

#define TX_IDLE(qp)		(!TX_ACTIVE(qp) && SQ_EMPTY(qp) && \
				IRQ_EMPTY(qp) && ORQ_EMPTY(qp))


struct iwarp_msg_info {
	int			hdr_len;
	struct iwarp_ctrl	ctrl;
	int (*proc_data)	(struct siw_qp *, struct siw_iwarp_rx *);
	int			max_payload;
};

extern struct iwarp_msg_info iwarp_pktinfo[RDMAP_TERMINATE + 1];
//extern struct siw_dev *siw;


/* QP general functions */
int siw_qp_modify(struct siw_qp *, struct siw_qp_attrs *,
		  enum siw_qp_attr_mask);

void siw_qp_llp_close(struct siw_qp *);
void siw_qp_cm_drop(struct siw_qp *, int);

struct ib_qp *siw_get_ofaqp(struct ib_device *, int);
void siw_qp_get_ref(struct ib_qp *);
void siw_qp_put_ref(struct ib_qp *);

enum siw_qp_state siw_map_ibstate(enum ib_qp_state);

int siw_check_mem(struct siw_pd *, struct siw_mem *, u64,
		  enum siw_access_flags, int);
int siw_check_sge(struct siw_pd *, struct siw_sge *, union siw_mem_resolved *,
		  enum siw_access_flags, u32, int);
int siw_check_sgl(struct siw_pd *, struct siw_wqe *,
		  enum siw_access_flags);

void siw_read_to_orq(struct siw_sqe *, struct siw_sqe *);

int siw_sqe_complete(struct siw_qp *, struct siw_sqe *, struct siw_sqe_md *, u32,
		     enum siw_wc_status, int notify);
int siw_rqe_complete(struct siw_qp *, struct siw_rqe *, struct siw_rqe_md *, u32,
		     enum siw_wc_status, int notify);

int siw_sq_flush_wr(struct siw_qp* qp, const struct ib_send_wr* wr, const struct ib_send_wr** bad_wr, bool flush_imm);
int siw_rq_flush_wr(struct siw_qp *qp, const struct ib_recv_wr *wr,
			   const struct ib_recv_wr **bad_wr, bool flush_imm);
void siw_qp_notify_work(struct work_struct *work);
void siw_cq_notify_work(struct work_struct *work);

#if KS_HAS_TASKLET_SETUP
void siw_cq_notify_task(struct tasklet_struct *task);
#else
void siw_cq_notify_task(unsigned long data);
#endif

#if 0
void siw_retry_get_rqe_work(struct work_struct *work);
#endif

int siw_return_reserved_rqe(struct siw_srq *srq, struct siw_rqe *ret_rqe, bool already_locked);

void siw_rx_queue_work(struct siw_qp *qp, unsigned long delay);
void siw_rx_cancel_work(struct siw_qp *qp);
void siw_rx_work_handler(struct work_struct *work);
int siw_do_rx_work(struct siw_qp *qp);

#ifdef SIW_TX_COMP_WAIT_ACK
#define SIW_TCP_CONG_CTRL_NAME "siw_tcp_ca"

struct siw_tcp_cong_priv_data {
	struct siw_qp *qp;
};

void siw_tcp_cong_ack_event(struct sock *sk, u32 ack_flags);
bool siw_low_delay_tx_cpu(ulong nr_cpu);
#endif


/* SIW user memory management */

#define CHUNK_SHIFT	9	/* sets number of pages per chunk */
#define PAGES_PER_CHUNK	(_AC(1, UL) << CHUNK_SHIFT)
#define CHUNK_MASK	(~(PAGES_PER_CHUNK - 1))
#define PAGE_CHUNK_SIZE	(PAGES_PER_CHUNK * sizeof(struct page *))

/*
 * siw_get_upage()
 *
 * Get page pointer for address on given umem.
 *
 * @umem: two dimensional list of page pointers
 * @addr: user virtual address
 */
static inline struct page *siw_get_upage(struct siw_umem *umem, u64 addr)
{
	unsigned int	page_idx	= (addr - umem->fp_addr) >> PAGE_SHIFT,
			chunk_idx	= page_idx >> CHUNK_SHIFT,
			page_in_chunk	= page_idx & ~CHUNK_MASK;

	if (likely(page_idx < umem->num_pages))
		return umem->page_chunk[chunk_idx].p[page_in_chunk];

	return NULL;
}

extern struct siw_umem *siw_umem_get(u64, u64);
extern struct siw_umem *siw_umem_alloc(u32);
extern void siw_umem_release(struct siw_umem *);
extern struct siw_pbl *siw_pbl_alloc(u32);
extern u64 siw_pbl_get_buffer(struct siw_pbl *, u64, int *, int *);
extern void siw_pbl_free(struct siw_pbl *);

/* QP TX path functions */
enum siw_tx_ctx_pref {
	SIW_TX_CTX_PREF_RR = 0,
	SIW_TX_CTX_PREF_SAME_CPU = 1,
	SIW_TX_CTX_PREF_SCQ_VECT = 2,
	SIW_TX_CTX_PREF_MAX,
};

extern int siw_qp_sq_process(struct siw_qp *);
extern int siw_sq_worker_init(void);
extern void siw_sq_worker_exit(void);
extern int siw_sq_queue_work(struct siw_qp *qp, enum siw_tx_ctx_pref tx_ctx_pref);
extern int siw_activate_tx(struct siw_qp *);
extern int siw_activate_tx_atomic(struct siw_qp *qp, u64 compare_add, u64 compare_add_mask,
								  u64 swap, u64 swap_mask);

/* QP RX path functions */
extern int siw_proc_send(struct siw_qp *, struct siw_iwarp_rx *);
extern int siw_proc_rreq(struct siw_qp *, struct siw_iwarp_rx *);
extern int siw_proc_areq(struct siw_qp *, struct siw_iwarp_rx *);
extern int siw_proc_rresp(struct siw_qp *, struct siw_iwarp_rx *);
extern int siw_proc_write(struct siw_qp *, struct siw_iwarp_rx *);
extern int siw_proc_terminate(struct siw_qp*, struct siw_iwarp_rx *);
extern int siw_proc_unsupp(struct siw_qp *, struct siw_iwarp_rx *);

extern int siw_tcp_rx_data(read_descriptor_t *rd_desc, struct sk_buff *skb,
			   unsigned int off, size_t len);

/* MPA utilities */
static inline int siw_crc_array(struct shash_desc *desc, u8 *start,
				size_t len)
{
	return crypto_shash_update(desc, start, len);
}

static inline int siw_crc_page(struct shash_desc *desc, struct page *p,
			       int off, int len)
{
	return crypto_shash_update(desc, page_address(p) + off, len);
}


/* Varia */
extern void siw_cq_flush(struct siw_cq *);
extern void siw_sq_flush(struct siw_qp *);
extern void siw_rq_flush(struct siw_qp *);
extern int siw_reap_cqe(struct siw_cq *, struct ib_wc *);
extern void siw_flush_xqes_work(struct work_struct *work);

/* RDMA core event dipatching */
extern void siw_qp_event(struct siw_qp *, enum ib_event_type);
extern void siw_cq_event(struct siw_cq *, enum ib_event_type);
extern void siw_srq_event(struct siw_srq *, enum ib_event_type, bool schedule);
extern void siw_srq_event_work(struct work_struct *work);
extern void siw_port_event(struct siw_dev *, u8, enum ib_event_type);

/* External use */
extern struct ib_mr *siw_mr_alloc(struct ib_pd *ofa_pd, u32 max_sge);
extern int siw_mr_enable(struct ib_mr *ofa_mr, u32 access);
extern int siw_mr_free(struct ib_mr *ofa_mr);

/* External use for redirecting SIW debug traces */
typedef void (*t_siw_dprint_fn)(u32 dbgcat, int pid, int cpu, int in_interrupt,
				const char *task_name,
				const char *func,
				const char *file, int line,
				const char *fmt, ...);
int siw_set_dprint_fn(t_siw_dprint_fn dprint_fn);

enum ib_qp_state siw_2_ib_qp_state(enum siw_qp_state siw_state);

static inline struct siw_qp *siw_qp_ofa2siw(struct ib_qp *ofa_qp)
{
	return container_of(ofa_qp, struct siw_qp, ofa_qp);
}

static inline int siw_sq_empty(struct siw_qp *qp)
{
	return qp->sendq[qp->sq_get % qp->attrs.sq_size].flags == 0;
}

static inline struct siw_sqe *sq_get_next(struct siw_qp *qp, struct siw_sqe_md **sqe_md)
{
	struct siw_sqe *sqe = &qp->sendq[qp->sq_get % qp->attrs.sq_size];
	if (sqe->flags & SIW_WQE_VALID) {
		if (sqe_md && qp->sendq_kern_md)
			*sqe_md = &qp->sendq_kern_md[qp->sq_get % qp->attrs.sq_size];
		return sqe;
	}
	return NULL;
}

static inline struct siw_sqe *orq_get_current(struct siw_qp *qp)
{
	return &qp->orq[qp->orq_get % qp->attrs.orq_size];
}

static inline struct siw_sqe *orq_get_tail(struct siw_qp *qp)
{
	if (likely(qp->attrs.orq_size))
		return &qp->orq[qp->orq_put % qp->attrs.orq_size];

	pr_warn("QP[%d]: ORQ has zero length", QP_ID(qp));
	return NULL;
}

static inline struct siw_sqe *orq_get_free(struct siw_qp *qp)
{
	struct siw_sqe *orq_e = orq_get_tail(qp);

	if (orq_e && orq_e->flags == 0)
		return orq_e;

	//dprint2(DBG_ATOMIC "qp " dprint_ptr_str() ": orq_put=%d, orq_get=%d\n", qp, (int)qp->orq_put, (int)qp->orq_get);
	//dprint2("qp " dprint_ptr_str() "\n", qp);
	return NULL;
}

static inline int siw_orq_empty(struct siw_qp *qp)
{
	return qp->orq[qp->orq_get % qp->attrs.orq_size].flags == 0 ? 1 : 0;
}

static inline struct siw_sqe *irq_get_free(struct siw_qp *qp)
{
	struct siw_sqe *irq_e = &qp->irq[qp->irq_put % qp->attrs.irq_size];
	if (irq_e->flags == 0)
		return irq_e;
	return NULL;
}

static inline int siw_irq_empty(struct siw_qp *qp)
{
	return qp->irq[qp->irq_get % qp->attrs.irq_size].flags == 0;
}

#if KS_HAS_SKB_CHECKSUM_OPS
static inline __wsum siw_csum_update(const void *buff, int len, __wsum sum)
{
	return (__force __wsum)crc32c((__force __u32)sum, buff, len);
}

static inline __wsum siw_csum_combine(__wsum csum, __wsum csum2, int offset,
									  int len)
{
	return (__force __wsum)__crc32c_le_combine((__force __u32)csum,
											   (__force __u32)csum2, len);
}

static inline void siw_crc_skb(struct siw_iwarp_rx *rctx, unsigned int len)
{
	const struct skb_checksum_ops siw_cs_ops = {
		.update = siw_csum_update,
		.combine = siw_csum_combine,
	};
	__wsum crc = *(u32 *)shash_desc_ctx(rctx->mpa_crc_hd);

	crc = __skb_checksum(rctx->skb, rctx->skb_offset, len, crc,
						 &siw_cs_ops);
	*(u32 *)shash_desc_ctx(rctx->mpa_crc_hd) = crc;
}
#else
static inline void siw_crc_skb(struct siw_iwarp_rx *rctx, unsigned int len)
{
	u32 crc = *(u32 *)shash_desc_ctx(rctx->mpa_crc_hd);
	unsigned int done = 0;

	while (done < len) {
		u8 buf[256];
		unsigned int chunk = min_t(unsigned int, len - done, sizeof(buf));

		if (skb_copy_bits(rctx->skb, rctx->skb_offset + done, buf, chunk))
			break;
		crc = crc32c(crc, buf, chunk);
		done += chunk;
	}
	*(u32 *)shash_desc_ctx(rctx->mpa_crc_hd) = crc;
}
#endif

#define tx_more_wqe(qp, curr_wqe)	(!siw_sq_empty(qp) || (tx_flags(curr_wqe) & SIW_WQE_MORE_WQES) || !siw_irq_empty(qp))


static inline struct siw_mr *siw_mem2mr(struct siw_mem *m)
{
	if (!SIW_MEM_IS_MW(m))
		return container_of(m, struct siw_mr, mem);
	return m->mr;
}

#if !KS_HAS_SMP_STORE_MB
#ifndef smp_store_mb
#define smp_store_mb(var, value)  do { WRITE_ONCE(var, value); barrier(); } while (0)
#endif
#endif

#define EC_BUG_PREFIX  "#EC-"
#define EC_BUG_PREFIX_FMT EC_BUG_PREFIX "%d"
#define NVMESH_BUG_PREFIX  "#NVMESH-"
#define NVMESH_BUG_PREFIX_FMT NVMESH_BUG_PREFIX "%d"

#define SIW_WARN_KNOWN_COMMON(op, cond, fmt, bug_num) ({ \
	int __rv = 0; \
	if (!siw_panic_on_warn || !*siw_panic_on_warn) \
		__rv = op(cond, fmt, bug_num); \
	__rv; }) \

#define SIW_WARN_KNOWN_EC(cond, bug_num) SIW_WARN_KNOWN_COMMON(WARN, cond, EC_BUG_PREFIX_FMT, bug_num)
#define SIW_WARN_KNOWN_EC_ONCE(cond, bug_num) SIW_WARN_KNOWN_COMMON(WARN_ONCE, cond, EC_BUG_PREFIX_FMT, bug_num)
#define SIW_WARN_KNOWN(cond, bug_num) SIW_WARN_KNOWN_COMMON(WARN, cond, NVMESH_BUG_PREFIX_FMT, bug_num)
#define SIW_WARN_KNOWN_ONCE(cond, bug_num) SIW_WARN_KNOWN_COMMON(WARN_ONCE, cond, NVMESH_BUG_PREFIX_FMT, bug_num)

#define SIW_TIMEOUT_WARN_ON_ONCE(__delay, __timeout)			\
	do {								\
		unsigned long timeout = __timeout;			\
		timeout *= clamp(relax_timeouts, 1U, 10000U);		\
		WARN_ON_ONCE(__delay > timeout);			\
	} while (0)

#define SIW_TIMEOUT_WARN_ON_KNOWN_ONCE(__delay, __timeout, __bug)	\
	do {								\
		unsigned long timeout = __timeout;			\
		timeout *= clamp(relax_timeouts, 1U, 10000U);		\
		SIW_WARN_KNOWN_ONCE(__delay > timeout, __bug);		\
	} while (0)

#define SIW_TIMEOUT_WARN_KNOWN(__delay, __timeout, __bug)	\
	do {								\
		unsigned long timeout = __timeout;			\
		timeout *= clamp(relax_timeouts, 1U, 10000U);		\
		SIW_WARN_KNOWN(__delay > timeout, __bug);		\
	} while (0)

#define SIW_TIMEOUT_WARN_KNOWN_THROTTLE_INTERVAL	(30 * HZ)

#define SIW_TIMEOUT_WARN_KNOWN_THROTTLED(__delay, __timeout, __bug)	\
	do {								\
		unsigned long timeout = __timeout;			\
		timeout *= clamp(relax_timeouts, 1U, 10000U);		\
		if (__delay > timeout) {				\
			static DEFINE_RATELIMIT_STATE(			\
				_siw_to_warn_rl_##__LINE__,		\
				SIW_TIMEOUT_WARN_KNOWN_THROTTLE_INTERVAL, 1); \
			if (__ratelimit(&_siw_to_warn_rl_##__LINE__))	\
				SIW_WARN_KNOWN(1, __bug);		\
		}							\
	} while (0)

#if defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1)
	#define SIW_BUG_NON_PRODUCTION(bug_num) WARN(1, NVMESH_BUG_PREFIX_FMT, bug_num)
	#define SIW_BUG_ON_NON_PRODUCTION(cond, bug_num) WARN_ON(cond, NVMESH_BUG_PREFIX_FMT, bug_num)
#else
	#define SIW_BUG_NON_PRODUCTION(bug_num) do { \
		pr_err("KERNEL WARNING TRIGGERED AT %s:%d - Bug Number: %d, BUG_ON for debug", __FILE__, __LINE__, bug_num); \
		BUG();\
	} while (0)

	#define SIW_BUG_ON_NON_PRODUCTION(cond, bug_num) do { \
		if (cond) { \
			pr_err("KERNEL WARNING TRIGGERED AT %s:%d - Bug Number: %d, BUG_ON for debug", __FILE__, __LINE__, bug_num); \
			BUG(); \
		} \
	} while (0)
#endif

#include "../../common/compat/kr_incs_types.h"

#endif
