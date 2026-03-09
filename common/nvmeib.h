/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_H
#define NVMEIB_H

#include "kr_incs.h"
#include "kr_version.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_types.h"
#include "ib_incs.h"
#include "nvmeibs_trend_types.h"
#include "nvmeib_consts_shared.h"
#include "nvmeib_msgs_shared.h"
#include "nvmeib_volume_type.h"
#include "nvmeib_nvme_error_codes.h"
#include "nvmeibc_disk_local_dma_pools.h"
#include "keeper/nvmeib_keeper_iface.h"

#ifndef MAX
#	define MAX(a, b) ((a) >= (b) ? (a) : (b))
#endif

#ifndef MAX3
#	define MAX3(x, y, z) MAX(MAX((x), (y)), (z))
#endif

#ifndef MIN
#	define MIN(a, b) ((a) >= (b) ? (b) : (a))
#endif

#ifdef __KERNEL__
#if !KS_HAS_DEL_TIMER_SYNC
#define del_timer_sync		timer_delete_sync
#endif
#if !KS_HAS_HRTIMER_INIT
#define hrtimer_init(timer, clock_id, mode) \
	hrtimer_setup((timer), NULL, (clock_id), (mode))
#endif
/* IOMMU Future-proof: Fail with error if someone tries to use virt_to_phys or page_to_phys
 * If you are sure you know what are you doing, then re-enable them with #pragma pop_macro before use
 */
#pragma push_macro("virt_to_phys")
#undef virt_to_phys
#define virt_to_phys(addr) ({ \
	unsigned long ret = 0; \
	BUILD_BUG_ON_MSG(1, "virt_to_phys is not compatible with iommu. Create a dma mapping");\
	ret;\
})

#pragma push_macro("page_to_phys")
#undef page_to_phys
#define page_to_phys(page) ({ \
	unsigned long ret = 0; \
	BUILD_BUG_ON_MSG(1, "page_to_phys is not compatible with iommu. Create a dma mapping");\
	ret;\
})
#endif


// maximum NVMesh payload attached to CM request
#define NVMEIB_MAX_CM_REQ_PAYLOAD_SIZE 	(IB_CM_REQ_PRIVATE_DATA_SIZE - 36)

// maximum NVMesh payload attached to CM reply
#define NVMEIB_MAX_CM_REP_PAYLOAD_SIZE 	(IB_CM_REP_PRIVATE_DATA_SIZE - 36)

#ifndef NVMEIB_DEBUG_RDMA_CORRUPTION
#define NVMEIB_DEBUG_RDMA_CORRUPTION 0
#endif

// SCQ
#define NVMEIB_PCPU_CQ_DO_SQ_DRAIN_ON_QP_STOP 1
#define NVMEIB_PCPU_CQ_DEFER_RDMA_DESTROY 0
#define NVMEIB_PCPU_CQ_DEFER_QPI_FREE 0

#define NVMEIBC_LOCK_CH_CB_KERNEL_WQ 1



enum {
	NVMEIB_SERVICE_ID_MASK = (~RDMA_IB_IP_PS_MASK)
};
enum {
	NVMEIB_PKEY = 0xffff,
	GUID_SIZE = sizeof("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"),
	NVMEIB_IWARP_PORT_ID = 7915,

	NVMEIB_FMR_SIZE       = 512,
	NVMEIB_FMR_MIN_SIZE	  = 127,
	NVMEIB_FMR_POOL_SIZE  = 5000,

	/* [NVMESH-7594]: Increase max FR Pool Size and 
		Change FR Pool Size to be calculated based on:
		- Number of CPUs, 
		- Max IOs per CPU, 
		- Max Disk Operations per IO (for EC 8 + 2)
		
		Example: For 32 CPUs, 64 Max IOs per CPU, and 10 Max Disk Operations per IO, the FR Pool Size will be: 
		32 CPUs * 64 Max IOs per CPU * 10 Max Disk Operations per IO = 20k MRs

		For 128 CPUs, the FR Pool Size will be: 80k MRs
	*/

#ifndef LOW_MEM
	NVMEIB_MAX_FR_POOL_SIZE  = (1 << 17),
	NVMEIB_BLOCK_DFLT_MAX_IOS_PER_CPU = 64,
	NVMEIB_BLOCK_MAX_DISK_OPS_PER_IO = 10,
#else
	NVMEIB_MAX_FR_POOL_SIZE  = (1 << 13),
	NVMEIB_BLOCK_DFLT_MAX_IOS_PER_CPU = 16,
	NVMEIB_BLOCK_MAX_DISK_OPS_PER_IO = 10,
#endif

	NVMEIB_MAP_ALLOW_FMR  = 0,
	NVMEIB_MAP_NO_FMR     = 1,

	NVMEIBC_BD_UUID_LEN           = 64,			// EC-3152: remove this define, use UUID defines
	NVMEIBC_BD_NAME_LEN           = 32,			// EC-3152: unify with other define

	NVMEIBC_LOGIN_PRIVATE_DATA_MAX_LEN = 256,

	NVMEIB_IB_IO_CLASS   	= 0xabcd,
	NVMEIB_IO_SUBCLASS   	= 0xabcc,
	NVMEIB_PROTOCOL      	= 0xabcb,

	NVMEIB_DEF_SG_PER_WQE      = 4,

	NVMEIB_FAST_REG_WR_ID	= ((u16)0xabab),
	NVMEIB_LOCAL_INV_WR_ID	= ((u16)0xacac),

	NVMEIB_IO_QP_MTU		= IB_MTU_512,

	NVMEIB_WAIT_FOR_IO_STOP		= (120 * HZ),
#ifndef LOW_MEM
	NVMEIB_WAIT_FOR_CONNECTION 	= (3 * (HZ / 4)),
	NVMEIB_WAIT_FOR_EVENT 		= (HZ / 2),
	NVMEIB_WAIT_FOR_MSQ_REP     = (HZ / 10),
#else
	NVMEIB_WAIT_FOR_CONNECTION 	= (30 * (HZ)),
	NVMEIB_WAIT_FOR_EVENT 		= (5 * HZ),
	NVMEIB_WAIT_FOR_MSQ_REP     = (10 * HZ),
#endif
	NVMEIB_WAIT_FOR_ADMIN_SEND_COMP	= (3 * HZ),
	NVMEIB_WAIT_FOR_GET_JMDC_TIMEOUT = 10 * HZ,
	NVMEIB_WAIT_FOR_CM_REP_TIMEOUT          = (2 * HZ),
	NVMEIB_WAIT_FOR_CM_REP_MAX_TIMEOUT      = (32 * HZ),
	NVMEIB_WAIT_BREAK_QP		= (1 * HZ),
	NVMEIB_N_WAIT_BREAK_QP		= 5,
	NVMEIB_WAIT_DREP			= (HZ / 500),
	NVMEIB_WAIT_FOR_RESOURCES_S	= (5 * HZ),
	NVMEIB_WAIT_FOR_RESOURCES_C	= (6 * HZ),
	NVMEIB_WAIT_FOR_LOGOUT_RSP	= (HZ / 2),
	NVMEIB_WAIT_FOR_PENDING_IU_TIMEOUT	= (HZ / 10),
	NVMEIB_WAIT_FOR_TOMA_RSP_TIMEOUT	= (3 * HZ),

	NVMEIB_QP_TIMEOUT = 14, /* 4.096 * 2 ^ timeout usec (14 == 67 ms) */
	NVMEIB_RETRY_CNT = 7, 	/* Number of retries before failing with IB_WC_RETRY_EXC_ERR */
	NVMEIB_MIN_RNR_TIMER = 12,

	NVMEIB_WAIT_DRAIN_SQ 	= HZ,
	NVMEIB_N_WAIT_DRAIN_QP	= 1, //5,
	NVMEIB_WAIT_CM_INV_SEC = 30,

	NVMEIB_DFLT_NUM_CPUS = 32,
	NVMEIB_DFLT_MAX_CPUS = 256,
	NVMEIB_CPU_INVALID = -1,

#if !defined(LOW_MEM)
	NVMEIB_N_2ND_LOCK_CHS = NVMEIB_DFLT_MAX_CPUS, /* clnt connects only X 2nd-lock-chs where
														 X=min(#target-active-cpus, nvmeibc_max_2nd_lock_channels) */
#else
	NVMEIB_N_2ND_LOCK_CHS = 0,
#endif

	NVMEIB_USE_LB_CM = 0, //1,

	NVMEIB_IOCH_KA_TIMEOUT_SEC = 8,

	NVMEIB_IOCH_KA_ONLY_NO_RDDA = 1,

	NVMEIBC_USE_BOTH_ROCE_AND_TCP_LOCK_CH = 0,	/* Future: curently unused */
	NVMEIBC_USE_BOTH_ROCE_AND_TCP_IO_CH = 0, 	/* Future: curently unused */

	NVMEIB_MAX_NR_TCP_CHANNELS_PER_PATH = 16,
	NVMEIB_DEFAULT_ANY_CPU_NRCH = 4,
	NVMEIB_MAX_LOCK_TCP_CHANNELS = 16,

	/* JGC */

	/* Timeout between sending out a JGC request and not getting a BLOCK CLEANED */
	NVMEIB_JGC_NO_RESP_TIMEOUT = 5 * HZ,
	/* Timeout between a range not being reconnected */
	NVMEIB_JGC_RNG_RECONNECT_TIMEOUT = 5 * HZ,
	/* Triggers JGC if available range entries falls below low watermark of total-range-entries * mult / div.
	 * [NVMESH-4322]: Change default to trigger if even 1 entry not available.
	 */
	NVMEIB_JGC_AVAIL_ENT_LOW_WM_MULT = 8,
	NVMEIB_JGC_AVAIL_ENT_LOW_WM_DIV = 8,

	NVMEIB_MAX_RDMA_UI = 256,

	NVMEIB_MAX_GEN_CMD_DATA_SINK = 2,

	NVMEIB_MAX_LOCK_TIME_INC_RETRIES = 5 * HZ,

	NVMEIB_DFLT_MAX_READ_ATOM_ON_WIRE = 4,

	/* Always use RDMA for Local Locks */
	NVMEIB_LOCAL_LOCK_ALWAYS_USE_RDMA = 0,

	/* SIW Flags */
	NVMEIB_SIW_NRCH_WAIT_RLS_ZERO_BEFORE_CB = 0,

	/* -----------   NVMEIB_DEBUG_RDMA_CORRUPTION FLAGS ----------------------- */
	/* 1) see nvmeibc_map_sg_modes */
	/* 2) use an MR for the IO KA */
	NVMEIB_USE_MR_FOR_IO_KA = NVMEIB_DEBUG_RDMA_CORRUPTION,
};

#define NVMEIB_NR_GET_MAX_CHANNELS_PER_PATH(_mparam) (_mparam? : num_possible_cpus() + NVMEIB_DEFAULT_ANY_CPU_NRCH)

/* POISON values */
#define NVMEIBS_NORDDA_PIGGYBACK_READ_POISON 	(0xffff8a8a8a8a8a8a)
#define NVMEIBS_RDDA_BB_POISON_MSBS_U64 		(0xAAEEAAEEULL)
#define NVMEIBC_POISON_AREA_PATTERN_U64 		(0xAACCAACCAACCAACCULL)
#define NVMEIBC_DISK_CMD_PBLR_POISON_CH_SHIFT	(8)
#define NVMEIBC_DISK_CMD_PBLR_POISON_MSBS 		(0xBEBEBEBEBEBEBEULL << NVMEIBC_DISK_CMD_PBLR_POISON_CH_SHIFT)

#define NVMEIB_DEV_USE_LOCAL_LOCK(dev_type)                                    \
	(!NVMEIB_LOCAL_LOCK_ALWAYS_USE_RDMA && (dev_type) == DT_siw)

static const u64 nvmeib_ka_value = 0x4e564d4549424b41; /*NVMEIBKA*/

/* Daniel: utsname() is an unsafe function to call from interrupt context */
const char *nvmeib_get_utsname_nodename(void);

enum nvme_iu_type {
	NVMEIB_IU_POOL,
	NVMEIB_IU_PRIV,
};

enum {
	NVMEIB_NO_DATA_DESC	= 0,
	NVMEIB_DATA_DESC_DIRECT	= 1,
	NVMEIB_DATA_DESC_INDIRECT = 2
};

/* ioctl codes */
#ifndef CDROM_GET_CAPABILITY
#define CDROM_GET_CAPABILITY  0x5331
#endif

enum nvmeibc_disk_locks_opr {
	NVMEIBC_LOCK_CMP_AND_SWAP = 0,
  //NVMEIBC_LOCK_CMP_AND_SWAP = 1,	// As if 3 values reserved, lock & unlock & lock for trim
	NVMEIBC_LOCK_FORCE_WRITE  = 3,  /* unused: Todo: release owner lock /take copy locks with this function */
	NVMEIBC_LOCK_READ         = 4,
	/* Write Transaction ID + dbits of erasure coding on command */
	NVMEIBC_LOCK_BLKSET_INFO_WRITE = 10,
	NVMEIBC_LOCK_BLKSET_INFO_READ = 12, /* unused by block*/
	NVMEIBC_LOCK_NUM_OPR,
	NVMEIBC_LOCK_LAST_OPR	= 0xff // keep this entry the LAST onet
} __attribute__ ((packed));

enum nvmeibc_rdma_intent {                  // Extention of 'enum nvmeibc_disk_locks_opr'. Todo: Unite them both
	/* Intent for RDMA's of Locks */
	NVMEIBC_CMD_LOCK_UNLOCK = 0,			// Release the lock
	NVMEIBC_CMD_LOCK_OWNER = 1,				// Acquire owner lock (primary or secondary owner)
	NVMEIBC_CMD_PREDISCARD = 2,				// Exactly like NVMEIBC_CMD_LOCK_OWNER, but for trim operation owner lock, before split.
	NVMEIBC_CMD_LOCK_COPY_OWNER = 3,		// Backup of owner, not really locking, just writing the value to another location in RAM. Used in EC
	NVMEIBC_CMD_LOCK_READ_DR = 4,			// Just verify owner lock is unlocked (when reading), read lock directly . Do not acquire the lock.
	NVMEIBC_CMD_LOCK_READ_PB = 5,           // Just verify owner lock is unlocked (when reading), piggybacking on cmd. Do not acquire the lock.
	/* Intent for RDMA of blockset info */
	NVMEIBC_CMD_BLKSET_INFO_WR_DR = 10,		// Same as above but send it directly not as piggyback
	NVMEIBC_CMD_BLKSET_INFO_WR_PB = 11,		// Piggy-back Blkset Info (Dirty Bits and Transaction ID) of erasure coding on command
} __attribute__ ((packed));
const char* nvmeibc_rdma_intent_to_string(const enum nvmeibc_rdma_intent e);

/* opcode encoded in the ib msg's wr_id
   mainly used by sender in send-completion processing
   Important: always bit 31 clear
*/
enum nvmeib_wr_opcode {
	NVMEIB_RECV= 0xAA,
	NVMEIB_SEND_CFG,
	NVMEIB_SEND_IO,
	NVMEIB_REPLY_IO,
	NVMEIB_REPLY_NO_IO,
	NVMEIB_REPLY_GEN,
	NVMEIB_REPLY_IO_W_RDMA,
	NVMEIB_REPLY_GEN_W_RDMA,
	NVMEIB_SEND_ACK,

	NVMEIB_RDMA_QP_CQ_DB,
	NVMEIB_RDMA_RQP_CLEAR_SQ_1,
	NVMEIB_RDMA_RQP_CLEAR_SQ_2,
	NVMEIB_RDMA_DISK_CQ_DB,
	NVMEIB_RDMA_RQP_SQ_1,
	NVMEIB_RDMA_RQP_SQ_2,
	NVMEIB_RDMA_RQP_SQ_3,
	NVMEIB_RDMA_RQP_SQ_4,
	NVMEIB_RDMA_RQP_DB_DESCR,
	NVMEIB_RDMA_SQ_PAGES,
	NVMEIB_RDMA_SQ_CMD,
	NVMEIB_RDMA_SQ_DB,
	NVMEIB_RDMA_MSIX,

	NVMEIB_RDMA_READ_OE_BASE,
	NVMEIB_RDMA_READ_OE_COMP,
	NVMEIB_RDMA_READ_OE_DATA_MID_0,
	/* NVMEIB_RDMA_MID_1 ... NVMEIB_RDMA_MID_255 */
	NVMEIB_RDMA_READ_OE_DATA_MID_MAX = NVMEIB_RDMA_READ_OE_DATA_MID_0 + NVMEIB_MAX_RDMA_UI - 1,
	NVMEIB_RDMA_READ_OE_DATA_LAST,
	NVMEIB_RDMA_READ_OE_LOCK,
	NVMEIB_RDMA_READ_OE_METADATA,
	NVMEIB_RDMA_READ_OE_LAST,

	NVMEIB_TOMA_SEND_REQ,
	NVMEIB_TOMA_SEND_RSP,

	NVMEIB_KEEP_ALIVE_REQ,
	NVMEIB_DRAIN_QUEUE,

	/* EC */
	NVMEIB_RDMA_METADATA,			/* [clnt: IO <--> srv] RDMA Write MD buffer (bi-dir) */
	NVMEIB_RDMA_GET_JMDC_REQ,		/* [clnt: cold recov ] Req srv to RDMA Write entire jmdc */
	NVMEIB_RDMA_GET_JMDC_RSP,		/* [srv-> cold recov ] Rsp clnt after posting RDMA-Write of entire jmdc */
	NVMEIB_RDMA_JMDC_PB,
	NVMEIB_RDMA_GET_JMDC,
	NVMEIB_RDMA_GET_JRANGE_EXT_JMDC,

	/* GEN OPs */
	NVMEIB_WR_GEN_OP_START,
	NVMEIB_WR_GEN_OP_END = NVMEIB_WR_GEN_OP_START + NVMEIB_GEN_OP_MAX - 1,
	NVMEIB_WR_GEN_UUID_RSP_JMDC,
	NVMEIB_WR_GEN_UUID_RSP_ENT_MD,

	/* LOCKS Channnel */
	NVMEIB_DISK_LOCK_OPR,
	NVMEIB_ATOMIC_TEST,
	NVMEIB_MASKED_ATOMIC_TEST,
	NVMEIB_LOCK_KA,

	NVMEIB_WR_DBG_CMD,

	NVMEIB_RDMA_IO_KA,
	NVMEIB_RDMA_WRITE_POISON,

	NVMEIB_RDMA_LAST,
	NVMEIB_RDMA_MID_0,
	/* NVMEIB_RDMA_MID_1 ... NVMEIB_RDMA_MID_255 */
	NVMEIB_RDMA_MID_MAX = NVMEIB_RDMA_MID_0 + NVMEIB_MAX_RDMA_UI - 1,

	NVMEIB_OP_END,
};

static inline const char *nvmeib_wr_opcode_str(enum nvmeib_wr_opcode op)
{
	if (op >= NVMEIB_WR_GEN_OP_START && op <= NVMEIB_WR_GEN_OP_END) {
		enum nvmeib_gen_cmd_op gen_op = op - NVMEIB_WR_GEN_OP_START;
		return nvmeib_gen_op_str(gen_op);
	}
	switch (op) {
	case NVMEIB_RECV: return "NVMEIB_RECV";
	case NVMEIB_SEND_CFG: return "NVMEIB_SEND_CFG";
	case NVMEIB_SEND_IO: return "NVMEIB_SEND_IO";
	case NVMEIB_REPLY_IO: return "NVMEIB_REPLY_IO";
	case NVMEIB_REPLY_NO_IO: return "NVMEIB_REPLY_NO_IO";
	case NVMEIB_REPLY_GEN: return "NVMEIB_REPLY_GEN";
	case NVMEIB_REPLY_IO_W_RDMA: return "NVMEIB_REPLY_IO_W_RDMA";
	case NVMEIB_REPLY_GEN_W_RDMA: return "NVMEIB_REPLY_GEN_W_RDMA";
	case NVMEIB_SEND_ACK: return "NVMEIB_SEND_ACK";
	case NVMEIB_RDMA_MID_0 ... NVMEIB_RDMA_MID_MAX: return "NVMEIB_RDMA_MID";
	case NVMEIB_RDMA_LAST: return "NVMEIB_RDMA_LAST";
	case NVMEIB_RDMA_QP_CQ_DB: return "NVMEIB_RDMA_QP_CQ_DB";
	case NVMEIB_RDMA_RQP_CLEAR_SQ_1: return "NVMEIB_RDMA_RQP_CLEAR_SQ_1";
	case NVMEIB_RDMA_RQP_CLEAR_SQ_2: return "NVMEIB_RDMA_RQP_CLEAR_SQ_2";
	case NVMEIB_RDMA_DISK_CQ_DB: return "NVMEIB_RDMA_DISK_CQ_DB";
	case NVMEIB_RDMA_RQP_SQ_1: return "NVMEIB_RDMA_RQP_SQ_1";
	case NVMEIB_RDMA_RQP_SQ_2: return "NVMEIB_RDMA_RQP_SQ_2";
	case NVMEIB_RDMA_RQP_DB_DESCR: return "NVMEIB_RDMA_RQP_DB_DESCR";
	case NVMEIB_RDMA_SQ_PAGES: return "NVMEIB_RDMA_SQ_PAGES";
	case NVMEIB_RDMA_SQ_CMD: return "NVMEIB_RDMA_SQ_CMD";
	case NVMEIB_RDMA_SQ_DB: return "NVMEIB_RDMA_SQ_DB";
	case NVMEIB_RDMA_MSIX: return "NVMEIB_RDMA_MSIX";
	case NVMEIB_RDMA_READ_OE_COMP: return "NVMEIB_RDMA_READ_OE_COMP";
	case NVMEIB_RDMA_READ_OE_DATA_MID_0 ... NVMEIB_RDMA_READ_OE_DATA_MID_MAX: return "NVMEIB_RDMA_READ_OE_DATA_MID";
	case NVMEIB_RDMA_READ_OE_DATA_LAST: return "NVMEIB_RDMA_READ_OE_DATA_LAST";
	case NVMEIB_RDMA_READ_OE_LOCK: return "NVMEIB_RDMA_READ_OE_LOCK";
	case NVMEIB_RDMA_READ_OE_METADATA: return "NVMEIB_RDMA_READ_OE_METADATA";
	case NVMEIB_TOMA_SEND_REQ: return "NVMEIB_TOMA_SEND_REQ";
	case NVMEIB_TOMA_SEND_RSP: return "NVMEIB_TOMA_SEND_RSP";
	case NVMEIB_KEEP_ALIVE_REQ: return "NVMEIB_KEEP_ALIVE_REQ";
	case NVMEIB_DRAIN_QUEUE: return "NVMEIB_DRAIN_QUEUE";
	/* EC */
	case NVMEIB_RDMA_METADATA: return "NVMEIB_RDMA_METADATA";
	case NVMEIB_RDMA_GET_JMDC_REQ: return "NVMEIB_RDMA_GET_JMDC_REQ";
	case NVMEIB_RDMA_GET_JMDC_RSP: return "NVMEIB_RDMA_GET_JMDC_RSP";
	case NVMEIB_RDMA_JMDC_PB: return "NVMEIB_RDMA_JMDC_PB";
	case NVMEIB_RDMA_GET_JMDC: return "NVMEIB_RDMA_GET_JMDC";
	case NVMEIB_RDMA_GET_JRANGE_EXT_JMDC: return "NVMEIB_RDMA_GET_JRANGE_EXT_JMDC";
	case NVMEIB_WR_GEN_UUID_RSP_JMDC: return "NVMEIB_WR_GEN_UUID_RSP_JMDC";
	case NVMEIB_WR_GEN_UUID_RSP_ENT_MD: return "NVMEIB_WR_GEN_UUID_RSP_ENT_MD";

	case NVMEIB_WR_DBG_CMD: return "NVMEIB_WR_DBG_CMD";

	default: return "???";
	}
}

static inline const char *nvmeib_ib_port_state_t_to_s(enum ib_port_state i)
{
	switch (i) {
	case IB_PORT_NOP: return "NOP";
	case IB_PORT_DOWN: return "DOWN";
	case IB_PORT_INIT: return "INIT";
	case IB_PORT_ARMED: return "ARMED";
	case IB_PORT_ACTIVE: return "ACTIVE";
	case IB_PORT_ACTIVE_DEFER: return "ACTIVE_DEFER";
	default: return "???";
	}
}

enum nvmeib_io_piggyb_opcode {
	NVMEIB_IO_PIGGYB_NONE = 0,
	NVMEIB_IO_PIGGYB_JMDC_WRITE,
	NVMEIB_IO_PIGGYB_LOCK, /* TBD: Switch to this API */
};

struct nvmeib_io_piggyb_cmd {
	enum nvmeib_io_piggyb_opcode opcode;
	union {
		struct {
			u64 rng_gen_id;
			u32 rng_idx;
			u16 ent_idx;
			struct nvmeib_jrnl_ent_md ent_md;
			struct jentry_md_container jmdc_val;
		} jmdc_wr;
		struct {
			enum nvmeibc_disk_locks_opr opr_type;
			void *handle;
			u64 addr;
			u64 val[2];
		} lock;
	};
};

enum nvmeib_data_reuse_buf_enum {
	nvmeib_data_reuse_buf_IGNORE  = 0,
	// Ignore, (defualt: Non EC and EC reads).
	nvmeib_data_reuse_buf_SAVE    = 1,			// Send jour cmd (4K + metadata) and save the buffer
	nvmeib_data_reuse_buf_SEND_REL= 2,			// Send data cmd from existing buffer and release buffer
  //nvmeib_data_reuse_buf_DROP_REL= 3,			// Just release buffer without sending
};

static inline const char *nvmeib_rcookie_action_str(enum nvmeib_data_reuse_buf_enum a)
{
	switch (a) {
		case nvmeib_data_reuse_buf_IGNORE	:  	return "IGNORE";
		case nvmeib_data_reuse_buf_SAVE     :	return "SAVE";
		case nvmeib_data_reuse_buf_SEND_REL	:	return "SEND_REL";
		default: return "???";
	}
}

struct nvmeib_data_reuse_buf_params {	// Journal/Data write the same 4K, so buffer in server memmory can be reused without sending the data twice by client over the network
	u32 action	   	: 4;					// enum nvmeib_data_reuse_buf_enum
	u32 lba_jam_enc		: 1;				// LBA is encoded by JAM
	u32 reserved   		: 11;				// Core team can use it
	u32 req_id	   	: 16;				// Shaul: please full here
	s16 comp_cpu;						// Channel completion CPU - Needed for per-cpu reuse
	u16 padding;
	u64 channel_ver;					// Shaul: please full here. When action is save, stores the cookie here, when action is release, use the cookie
	u64 disk_ver;
	void *channel;
} __attribute__((packed));

#define nvmeib_data_reuse_buf_zero(p)  ({ \
	memset(p, 0, sizeof(*p)); \
})

struct nvmeib_data_buffer {			// IO is transmitted clnt->srvr by this
	struct sg_table table;			// SG list and number of entries
	struct scatterlist sg_single;		// Optimisation for when SG only has one entry
	struct nvmeib_data_reuse_buf_params rcookie;
	u32 length;						// For reads or writes, it's the total bytes in SG list. For TRIM, it's the total bytes of the NVMe DSM command size.
	union {
		struct {
			/* Control Flags */
			u32 core_sgl	: 1;	// SGL is allocated by Core (Read MD local)
			u32 inline_sgl	: 1;	// SGL is inlined in another allocation
			u32 sg_mapped	: 1;	// SGL is already DMA mapped (drive does not need to map it)
			u32 dma_pool	: 1;	// SGL memory is from DMA pool
			u32 md_dma_pool : 1;	// MD memory is from DMA pool
			u32 md_dummy	: 1;	// MD memory is from dummy area
			u32 data_copy	: 1;	// Data pages are copies in DMA pool (fake_4kpi write)
			u32 reserved 	: 25;
		};
		u32 ctrl_flags;
	};
};

struct nvmeib_rdma_iu {
	u64 raddr;
	u32 lkey;
	u32 rkey;
	struct ib_sge *sge;
	u32 sge_cnt;
	size_t len;
};

enum nvmeib_iu_owner {
	NVMEIB_IU_OWNER_HW = 0xaa,
	NVMEIB_IU_OWNER_SW = 0xdd
};

enum nvmeib_iu_md_type {
	NVMEIB_IU_MD_NONE = 0,
	NVMEIB_IU_MD_SIW,
};

/* iu for information unit.  used as a context for send and receive */
struct nvmeib_iu {
	struct list_head free_tx_n;
	u8 opcode;
	int index;
	u16 version;
	enum ib_wc_status io_status;
	void *priv;
	void *owner_ptr;
	/* work attached for the descriptor */
	void (*work_handler)(struct nvmeib_iu *ioctx);
	void *work_payload;
	struct completion *io_done;
	union {
		struct workqe_struct work;   /* custom nvmeib_q work item */
		struct work_struct kwork;    /* kernel workqueue work item */
	};
	u64 dma;
	void *buf;
	size_t size; /* NVMEIBS_MAX_ADMIN_MSG_SIZE or NVMEIBC_DEFAULT_CLIENT_MSG_SIZE */
	enum dma_data_direction	direction;
	u16 max_rdma_iu;
	u16 n_rdma_iu;
	struct nvmeib_rdma_iu *rius;
	bool defer;
	atomic_t owner; /* nvmeib_iu_owner */
	int recv_idx;
	size_t recv_size;
	
	/* For iostats */
	size_t send_size;
	ktime_t send_time;

#define NVMEIB_IU_RESET_RCV_IDX(_iu) \
	do { \
		(_iu)->recv_idx = 0; \
	} while(0)

#if (defined NVMEIBS_NR_LAT_MEAS && (NVMEIBS_NR_LAT_MEAS==1)) || (defined NVMEIBC_NR_LAT_MEAS && (NVMEIBC_NR_LAT_MEAS==1))
	struct {
		bool valid;
		ktime_t recv_time;
		ktime_t poll_time;
		u16 cpu;
		u16 queue;
		u32 skb_hash;
	} rx_md;
#endif

#ifdef DEBUG_SCQ_IU_OWNER_BT
	unsigned long sw2hw_bt[4];
	unsigned long old_sw2hw_bt[4];
#endif
};

/* qp recv_q */
struct nvmeib_recvq {
	struct nvmeib_dev *dev;
	int rq_queue_size;
	int rq_msg_size;
	struct nvmeib_iu **rx_ring;
};

struct nvmeib_rdma_event;
struct nvmeib_rdma_cm;
/* header for any connection manager handler */

struct nvmeib_cm_evt_cnt {
	int rej;
	int rtu;
	int usr_est;
	int dreq;
	int drep;
	int timewt_exit;
	int rep_err;
	int dreq_err;
	int mra_recv;
	int dev_err;
};

#define D_PRINT_CM_EVENT_CNT(cm_evt_cnt)\
	_ND(D_PRINT_CM_EVENT_CNT_d1, "CM Event Count: [REJ]=@INT,[RTU]=@INT,[EST]=@INT,[DREQ]=@INT,[DREP]=@INT," \
	   "[TIME]=@INT,[REP_ERR]=@INT,[DREQ_ERR]=@INT,[MRA]=@INT,[DEVICE_ERROR]=@INT", \
	   cm_evt_cnt.rej, cm_evt_cnt.rtu, cm_evt_cnt.usr_est, cm_evt_cnt.dreq,\
	   cm_evt_cnt.drep, cm_evt_cnt.timewt_exit, cm_evt_cnt.rep_err,\
	   cm_evt_cnt.dreq_err, cm_evt_cnt.mra_recv, cm_evt_cnt.dev_err);

#define D_TRACE_CM_EVENT_CNT(_name_, cm_evt_cnt)\
	_ND(_name_, "CM Event Count: [REJ]=@EVENT_CNT,[RTU]=@EVENT_CNT,[EST]=@EVENT_CNT,[DREQ]=@EVENT_CNT,[DREP]=@EVENT_CNT," \
	   "[TIME]=@EVENT_CNT,[REP_ERR]=@EVENT_CNT,[DREQ_ERR]=@EVENT_CNT,[MRA]=@EVENT_CNT,[DEVICE_ERROR]=@EVENT_CNT", \
	   cm_evt_cnt.rej, cm_evt_cnt.rtu, cm_evt_cnt.usr_est, cm_evt_cnt.dreq,\
	   cm_evt_cnt.drep, cm_evt_cnt.timewt_exit, cm_evt_cnt.rep_err,\
	   cm_evt_cnt.dreq_err, cm_evt_cnt.mra_recv, cm_evt_cnt.dev_err);

struct nvmeib_cm_id {
	/* inc/dec refcnt of this struct's container (s_net) */
	bool (*ref_chg)(struct nvmeib_cm_id *h, bool inc);
    /*  the handler of the event */
    void (*handler)(struct nvmeib_cm_id *h, struct nvmeib_rdma_event *event);
    /* the cm_id in its base form, that is agnotic to connection type */
    struct nvmeib_rdma_cm *cm_id;
    struct hlist_node cmid_link;
    /* counter for CM events */
    struct nvmeib_cm_evt_cnt cm_evt_cnt;
    spinlock_t cm_evt_state_lock;
    bool cm_evt_handle;
};

static inline struct nvmeib_cm_id *h_to_cm_id(struct hlist_node *hlink)
{
	return container_of(hlink, struct nvmeib_cm_id, cmid_link);
}

/**
 * struct nvmeib_fr_desc - fast registration work request
 * arguments
 * @entry: Entry in srp_fr_pool.free_list.
 * @mr:    Memory region.
 * @frpl:  Fast registration page list.
 */
struct nvmeib_fr_desc {
	struct list_head entry;
	struct ib_mr *mr;
#if !IB_NEW_FR
	struct ib_fast_reg_page_list *frpl;
#else
	struct scatterlist *map_sgl;
	int map_sgl_pages;
#endif
	u8 valid;
	u8 bind_err;
	struct list_head used_entry;
	/* NVMEIBC_DEBUG_FR_LEAK - may be invalid if free before us */
	void *owner;
};

struct nvmeib_fr_pool_percpu_cache {
	struct nvmeib_fr_pool *pool;
	struct list_head free_list;
	int n_free;
	struct timer_list idle_timer;
	unsigned long last_get_jif;
	int cpu;

	/* Statistics */
	struct {
		u64 n_get_from_cache_success;
		u64 n_get_from_cache_fail;
		u64 n_get_from_cache_fail_after_refill;
		u64 total_get_n_free;
		u64 n_put_to_cache;
		u64 n_spills_to_excess_list;
		u64 total_spilled_to_excess_list;
		u64 n_refills_from_global_pool;
		u64 total_refilled_from_global_pool;
		u64 n_bind_errors;
		u64 n_rereg_scheduled;
		u64 n_spills_idle_timer;
		u64 total_spills_idle_timer;
		u64 n_rebalance_scheduled;
		u64 n_spills_rebalance;
		u64 total_spilled_rebalance;
		u64 max_n_free;
	} stats;
};

/**
 * struct nvmeib_fr_pool - pool of fast registration descriptors
 *
 * An entry is available for allocation if and only if it occurs in @free_list.
 *
 * @size:      Number of descriptors in this pool.
 * @max_page_list_len: Maximum fast registration work request page list length.
 * @lock:      Protects free_list.
 * @free_list: List of free descriptors.
 * @desc:      Fast registration descriptor pool.
 */
struct nvmeib_fr_pool {
	int size;
	int	max_page_list_len;
	spinlock_t lock;
	struct list_head free_list;
	int n_free;
	struct list_head err_list;
	int n_error;

	struct work_struct rereg_work;

	struct nvmeib_fr_pool_percpu_cache __percpu *percpu_cache;
	struct work_struct rebalance_pcpu_cache_work;
	unsigned long rebalance_pcpu_cache_scheduled_jif;
	/* refill target for per-cpu cache */
	unsigned pcpu_low;
	/* spill threshold for per-cpu cache */
	unsigned pcpu_high;

#ifdef NVMEIBC_DEBUG_FR_LEAK
	struct list_head used_list;
#endif

	struct ib_pd *pd;

	struct nvmeib_public_procfs_ent *proc_ent;

	struct {
		u64 n_get_success;
		u64 n_get_fail;
		u64 total_get_n_free;
		u64 n_puts;
		u64 n_bind_errors;
		u64 n_rereg_scheduled;
		u64 total_rereg_mr_success;
		u64 total_rereg_mr_fail;
		u64 min_n_free;
		u64 max_n_error;
	} stats;
};

#define CQ_POLL_BATCH 64

#ifdef CQ_DEBUG
#	define CQ_N_COMP_TRACE (100000ULL)
#endif

enum nvmeib_dev_cq_poll_mode {
	NVMEIB_DEV_CQ_POLL_DISABLED = -1,
	NVMEIB_DEV_CQ_INTR_MODE = 0,
	NVMEIB_DEV_CQ_POLL_MODE = 1,
	NVMEIB_DEV_CQ_ENTER_USER_POLL_MODE = 2,
	NVMEIB_DEV_CQ_USER_POLL_MODE = 3,
	NVMEIB_DEV_CQ_EXIT_USER_POLL_MODE = 4,
};

struct nvmeib_dev;
struct nvmeib_srq_info;

struct nvmeib_dev_cq;
struct ib_cq;
struct nvmeib_device_public_ops;
struct nvmeib_dev {
	struct ib_device *ib_dev;
	struct ib_device_attr *dev_attr;
	int dev_type;
	struct ib_pd *pd;
	struct ib_mr *mr;
	u64 mr_page_mask;
	int mr_page_size;
	int mr_max_size;
	int	max_pages_per_mr;
	bool has_fmr;
	bool has_fr;
	bool use_fast_reg;
	int phys_port_cnt;
	struct nvmeib_srqs *srqs;
	/* receive queue buffers */
	struct nvmeib_iu **rx_ring;
	/* fast registration mechanism */
	union {
		struct ib_fmr_pool *fmr_pool;
		struct nvmeib_fr_pool *fr_pool;
	};
	int num_comp_vectors;
	/* Used for FP ptrs to public-ops */
	struct nvmeib_device_public_ops *pops;
	int (*map_mr_f)(struct ib_device *ibdev, struct ib_mr *mr,
					phys_addr_t *pages, int n_pages);
	int (*post_send_atomic_fn)(struct ib_qp *ibqp, struct nvmeib_send_wr *wr,
				struct nvmeib_send_wr **bad_wr);
	int (*peek_cq)(struct ib_cq *ib_cq, int max);
	struct mutex cqs_guard;
	struct nvmeib_dev_cq *cqs;
	int n_cqs;

	const char *inst_name;
	struct proc_dir_entry *proc_dir;

	/* Used for iwarp find-path socket */
	void *iw_find_path_sock_priv;

	/* Stats for iw_cm_id invalidate */
	void *iw_cm_id_inv_stats_priv;
	
	struct nvmeib_keeper_frs_info keeper_frs_info;
	bool init_from_keeper;
	bool save_to_keeper;
};

/* use for fast memory registeration */
struct nvmeib_mr_info {
	union {
		struct ib_fmr_pool *fmr_pool;
		struct nvmeib_fr_pool *fr_pool;
	};
	struct ib_qp *qp;
	bool use_sg;
	union {
		phys_addr_t *pages;
		struct scatterlist *sg; /* Currently unused */
	};
	union {
		int n_pages;
		int count;
	};
	struct nvmeib_iu *iu;
	u32 null_iu_idx;
	u64 io_addr;
	u32 lkey;
	u32 rkey;
	/* the offset and dma_len arer must for 512 byte sectors */
	u32 offset;
	u32 dma_len;
	/* DEBUG - may be outdated if owner clear before us*/
	void *owner;
};

/* use to allocate virtual memory that is dma-able */
struct nvmeib_alloc_info {
	int n;
	struct page **pages;
};

struct nvmeib_alloc_n_map_info {
	struct nvmeib_dev *dev;
	struct nvmeib_alloc_info alloc;
	int size;
	struct nvmeib_mr_info mr;
	void *fmr;
	struct nvmeib_iu *iu;
};

/*
 * interrupt shaper percpu
 */

enum intr_shaper_intr_type {
	INTR_SHAPER_INTR_TYPE_NONE = 0,
	INTR_SHAPER_INTR_TYPE_CLIENT_SCQ = 1,
	INTR_SHAPER_INTR_TYPE_CLIENT_RCQ = 2,
	INTR_SHAPER_INTR_TYPE_SERVER_SCQ = 3,
	INTR_SHAPER_INTR_TYPE_SERVER_RCQ = 4,
	INTR_SHAPER_INTR_TYPE_SERVER_NVME = 5,
	INTR_SHAPER_INTR_TYPE_DEV_CQ = 6,
	MAX_INTR_SHAPER_INTR_TYPE = 7,
};

inline static const char *intr_shaper_intr_type_to_str(enum intr_shaper_intr_type type, bool lower_case)
{
	switch (type) {
	case INTR_SHAPER_INTR_TYPE_NONE: return lower_case ? "none" : "NONE";
	case INTR_SHAPER_INTR_TYPE_CLIENT_SCQ: return lower_case ? "client_scq" : "CLIENT_SCQ";
	case INTR_SHAPER_INTR_TYPE_CLIENT_RCQ: return lower_case ? "client_rcq" : "CLIENT_RCQ";
	case INTR_SHAPER_INTR_TYPE_SERVER_SCQ: return lower_case ? "server_scq" : "SERVER_SCQ";
	case INTR_SHAPER_INTR_TYPE_SERVER_RCQ: return lower_case ? "server_rcq" : "SERVER_RCQ";
	case INTR_SHAPER_INTR_TYPE_SERVER_NVME: return lower_case ? "server_nvme" : "SERVER_NVME";
	case INTR_SHAPER_INTR_TYPE_DEV_CQ: return lower_case ? "dev_cq" : "DEV_CQ";
	default: return lower_case ? "unknown" : "UNKNOWN";
	}
}

struct intr_shaper_percpu_stats {
	u64 n_intrs;
	u64 total_intr_time_ns;
	u64 max_intr_time_ns;
	u64 min_intr_time_ns;
	u64 total_burst_size;
	u32 max_burst_size;
	u32 min_burst_size;
	u64 n_wakeups_burst;
	u64 n_wakeups_cycles;
	u64 n_wakeups_irq_time;
};

struct intr_shaper_percpu {
	/* for EWMA calculation */
	u32 last_burst_size;
	u64 last_update_ns;       /* when we last updated the ewma */
	u64 busy_since_last_ns;   /* accumulated busy time since last update */
	u32 ewma_load_pct_x1000;      /* EWMA of load (% * 1000) */
	int last_result;

	/* Status of current interrupt*/
	enum intr_shaper_intr_type hw_intr_type;
	u64 hw_intr_start_ns;
	int hw_intr_n_polled;

	enum intr_shaper_intr_type sw_intr_type;
	u64 sw_intr_start_ns;
	int sw_intr_n_polled;

	/* Local copy of the shaper parameters */
	unsigned int max_burst_size_local;
	unsigned int max_percent_cpu_local;
	unsigned int max_irq_time_usecs_local;

	/* Statistics*/
	struct intr_shaper_percpu_stats stats_per_intr_type[MAX_INTR_SHAPER_INTR_TYPE];
	u64 total_ewma_percent_cpu_x1000;
	u64 n_calc_ewma_percent_cpu;
	u32 max_ewma_percent_cpu_x1000;
	u32 min_ewma_percent_cpu_x1000;
};

struct nvmeib_intr_shaper {
	size_t percpu_size;
	void *percpu; /* struct intr_shaper_percpu + cachline align */
	/* use shorter frame for 'finer' burst detction */
	u64 frame_size_nsecs;
};

#define NVMEIB_STATE_GUARD_STACK_TRACE_DEPTH 5

#ifdef CONFIG_ARCH_STACKWALK
struct nvmeib_stack_trace {
	unsigned long *entries;
	unsigned int nr_entries;
	unsigned int max_entries;
	unsigned int skip;
};
#else
#define nvmeib_stack_trace stack_trace
#endif

struct nvmeib_state_guard {
	atomic_t state;

#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
	spinlock_t st_chng_lock;
	unsigned long st_chng_stack[NVMEIB_STATE_GUARD_STACK_TRACE_DEPTH];
	struct nvmeib_stack_trace st_chng_stack_trace;
#endif
};

static inline void nvmeib_init_state_guard(struct nvmeib_state_guard *g, int init_state)
{
	atomic_set(&g->state, init_state);

#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
	spin_lock_init(&g->st_chng_lock);
	g->st_chng_stack_trace.entries = g->st_chng_stack;
	g->st_chng_stack_trace.max_entries = NVMEIB_STATE_GUARD_STACK_TRACE_DEPTH;
#endif
}

static inline int nvmeib_set_state_guard(struct nvmeib_state_guard *g,
	int v)
{
	int prev;
#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
	unsigned long flags;
	spin_lock_irqsave(&g->st_chng_lock, flags);
#endif

	prev = atomic_xchg(&g->state, v);

#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
	nvmeib_public_save_stack_trace(&g->st_chng_stack_trace);
	spin_unlock_irqrestore(&g->st_chng_lock, flags);
#endif

	return prev;
}

static inline bool nvmeib_test_n_set_state_guard(struct nvmeib_state_guard *g,
	int v)
{
	int prev = nvmeib_set_state_guard(g, v);
	return prev != v;
}

static inline bool nvmeib_switch_state_guard(struct nvmeib_state_guard *g,
	int o, int n)
{
	int prev;
#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
	unsigned long flags;
	spin_lock_irqsave(&g->st_chng_lock, flags);
#endif
	prev = atomic_cmpxchg(&g->state, o, n);

#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
	if (prev == o)
		nvmeib_public_save_stack_trace(&g->st_chng_stack_trace);
	spin_unlock_irqrestore(&g->st_chng_lock, flags);
#endif

	return (prev == o);
}

static inline int nvmeib_get_state_guard(struct nvmeib_state_guard *g)
{
	return atomic_read(&g->state);
}
#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
	#define nvmeib_state_guard_print_stack_trace(g)\
		do {\
			unsigned long flags;\
			spin_lock_irqsave(&(g)->st_chng_lock, flags);\
			_NI(nvmeib_state_guard_print_stack_trace_i1, "nvmeib_state_guard @INT64_HEX - state: @INT stack_trace: @FN <- @FN <- @FN <- @FN <- @FN",\
			(g), atomic_read(&(g)->state), (void *)(g)->st_chng_stack[0], (void *)(g)->st_chng_stack[1],\
			(void *)(g)->st_chng_stack[2], (void *)(g)->st_chng_stack[3], (void *)(g)->st_chng_stack[4]);\
			spin_unlock_irqrestore(&(g)->st_chng_lock, flags);\
		} while (0)
#else
	#define nvmeib_state_guard_print_stack_trace(g)
#endif

#if 0
static inline u64 nvmeib_encode_wr_id(u32 opcode, u32 idx)
{
	return ((u64)opcode << 32) | idx;
}

static inline u64 nvmeib_encode_gen_wr_id(enum nvmeib_gen_cmd_op gen_op, u32 idx)
{
	return nvmeib_encode_wr_id(NVMEIB_WR_GEN_OP_START + gen_op, idx);
}

static inline u32 nvmeib_opcode_from_wr_id(u64 wr_id)
{
	return (u32)(wr_id >> 32);
}

static inline u32 nvmeib_idx_from_wr_id(u64 wr_id)
{
	return (u32)wr_id;
}

#else
union nordda_wr_id {
	struct {
		u16 version;
		u16 opcode;
		u16 index;
		u16 reused : 1;
		u16 reserved : 15;
	};
	u64 wr_id;
};

#ifdef DEBUG_FIELDSIZE_OFERFLOW
#define BUG_ON_OVERFLOW_FIELD(val , field_sizeof) \
do { \
	u64 max_val;\
	switch (field_sizeof) {\
	case 1: max_val = U8_MAX; break;\
	case 2: max_val = U16_MAX; break;\
	case 4: max_val = U32_MAX; break;\
	case 8: max_val = U64_MAX; break;\
	default: BUG();\
	}\
	if (val > max_val) {\
		printk(KERN_ERR "OOPS, val=%llu > max_val=%llu (field_sizeof=%llu)\n", \
		(u64)val, max_val, (u64)field_sizeof); \
		BUG(); \
	}\
} while (0)
#else
#define BUG_ON_OVERFLOW_FIELD(val , field_sizeof) do {} while (0)
#endif

static inline u64 nordda_wr_id_encode_with_reused(u16 version, u16 opcode, u16 index, bool reused)
{
	union nordda_wr_id wrid = {};
	BUG_ON_OVERFLOW_FIELD(index,  sizeof_field(union nordda_wr_id, index));
	BUG_ON_OVERFLOW_FIELD(opcode, sizeof_field(union nordda_wr_id, opcode));
	wrid.index = index;
	wrid.reused = reused ? 1 : 0;
	wrid.opcode = opcode;
	wrid.version = version;
	return wrid.wr_id;
}

static inline u64 nordda_wr_id_encode(u16 version, u16 opcode, u16 index)
{
	return nordda_wr_id_encode_with_reused(version, opcode, index, false);
}

static inline u64 nordda_wr_id_encode_gen(u16 version, enum nvmeib_gen_cmd_op gen_op, u32 index)
{
	union nordda_wr_id wrid = {};
	BUG_ON(gen_op <= NVMEIB_GEN_OP_UNUSED ||
		   gen_op >= NVMEIB_GEN_OP_MAX);
	wrid.opcode = (unsigned)gen_op + (unsigned)NVMEIB_WR_GEN_OP_START;
	wrid.index = index;
	wrid.version = version;
	return wrid.wr_id;
}

static inline u16 nordda_wr_id_decode_version(u64 wr_id)
{
	union nordda_wr_id wrid = {.wr_id = wr_id};
	return wrid.version;
}

static inline u16 nordda_wr_id_decode_opcode(u64 wr_id)
{
	union nordda_wr_id wrid = {.wr_id = wr_id};
	return wrid.opcode;
}

static inline bool nordda_wr_opcode_is_gen_cmd(u16 opcode)
{
	return (opcode >= NVMEIB_WR_GEN_OP_START && opcode <= NVMEIB_WR_GEN_OP_END);
}

static inline enum nvmeib_gen_cmd_op nordda_wr_opcode_get_gen_op(u16 opcode)
{
	BUG_ON(opcode < NVMEIB_WR_GEN_OP_START || opcode > NVMEIB_WR_GEN_OP_END);
	return opcode - NVMEIB_WR_GEN_OP_START;
}

static inline u16 nordda_wr_id_decode_index(u64 wr_id)
{
	union nordda_wr_id wrid = {.wr_id = wr_id};
	return wrid.index;
}

static inline bool nordda_wr_id_decode_reused(u64 wr_id)
{
	union nordda_wr_id wrid = {.wr_id = wr_id};
	return !!wrid.reused;
}

//------------------------------------------------------------------------------
#define NON_NR_VERSION (0xdddd)
static inline u64 nvmeib_encode_wr_id(u16 opcode, u32 idx)
{
	BUG_ON_OVERFLOW_FIELD(idx,    sizeof_field(union nordda_wr_id, index));
	BUG_ON_OVERFLOW_FIELD(opcode, sizeof_field(union nordda_wr_id, opcode));
	return nordda_wr_id_encode(NON_NR_VERSION, opcode, idx);
}

static inline u64 nvmeib_encode_gen_wr_id(enum nvmeib_gen_cmd_op gen_op, u32 idx)
{
	return nordda_wr_id_encode_gen(NON_NR_VERSION, (unsigned)NVMEIB_WR_GEN_OP_START + (unsigned)gen_op, idx);
}

static inline u32 nvmeib_opcode_from_wr_id(u64 wr_id)
{
	return nordda_wr_id_decode_opcode(wr_id);
}

static inline u32 nvmeib_idx_from_wr_id(u64 wr_id)
{
	return nordda_wr_id_decode_index(wr_id);
}

#endif

/* Ideally, we would only include this nvmeib_rdma.c and use an accessor function, but we want to prevent an extra function call in IO-path */
#if ENABLE_SIW
#include "../softiwarp/common/siw_kern_abi.h"
#endif

static inline u64 nvmeib_wr_id_from_wc(const struct ib_wc *wc)
{
#if ENABLE_SIW
	if (unlikely((wc->wc_flags & SIW_IB_WC_WITH_SIW_MD)))
		return ((const struct siw_wc_md *)wc->wr_id)->wr_id;
#endif
	return wc->wr_id;
}

static inline u32 nvmeib_opcode_from_wc(const struct ib_wc *wc)
{
	return nvmeib_opcode_from_wr_id(nvmeib_wr_id_from_wc(wc));
}

static inline u32 nvmeib_idx_from_wc(const struct ib_wc *wc)
{
	return nvmeib_idx_from_wr_id(nvmeib_wr_id_from_wc(wc));
}

int nvmeib_debug_level(void);
void nvmeib_set_debug_level(int (*dlf)(void));
struct nvmeib_dev *nvmeib_init(struct ib_device *device,
			       const char *inst_name,
			       bool init_cqs,
			       bool create_poll_cq_proc);
void nvmeib_free(struct nvmeib_dev *dev);
const char *nvmeib_device_name(struct nvmeib_dev *dev);
int nvmeib_get_dev_numa_node(struct nvmeib_dev *dev);
int nvmeib_init_fast_reg(struct nvmeib_dev *dev);



/**
 * prepares a pool of descriptors that later will be used
 * note that either fmr_pool or fr_pool will be null
 *
 * @param dev the device to which the pull belongs to
 * @param fmr_pool
 * @param fr_pool
 *
 * @return int
 */
int nvmeib_alloc_fast_reg_pool(struct nvmeib_dev *dev,
	struct ib_fmr_pool **fmr_pool, struct nvmeib_fr_pool **fr_pool,
	int pool_size, void *memmgr_metrics_ctx);
void nvmeib_destroy_fast_reg_pool(struct nvmeib_fr_pool *pool, struct ib_mr **keep_mrs_arr, int *keep_mrs_arr_sz, void *memmgr_metrics_ctx, int pool_size);
struct nvmeib_fr_desc *nvmeib_fast_reg_pool_get(struct nvmeib_fr_pool *pool);
void nvmeib_fast_reg_pool_put(struct nvmeib_fr_pool *pool,
	struct nvmeib_fr_desc **desc, int n);
int nvmeib_fast_reg_pool_handle_bind_err(struct nvmeib_fr_desc **desc, int n, u32 rkey);
void nvmeib_fast_reg_pool_trace(struct nvmeib_fr_pool *pool);

void *nvmeib_map_fr(struct nvmeib_dev *nvdev, struct nvmeib_mr_info *info);
void *nvmeib_map_fmr(struct nvmeib_mr_info *info);

/**
 * map memory to device
 *
 *
 * @param nvdev
 * @param info
 *
 * @return void*
 */
void *nvmeib_map_mr(struct nvmeib_dev *nvdev, struct nvmeib_mr_info *info);
void nvmeib_free_mr(struct nvmeib_dev *nvdev, struct ib_qp *qp,
	struct nvmeib_fr_pool *fr_pool, void *fmr, struct nvmeib_iu *iu);

struct nvmeib_numa_iter;

struct nvmeib_iu *nvmeib_alloc_ioctx(struct ib_device *dev,
	int ioctx_size, int dma_size, enum dma_data_direction dir,
	struct nvmeib_numa_iter *iter); /*iter can be NULL*/
struct nvmeib_iu **nvmeib_alloc_ioctx_ring(struct ib_device *dev,
	int ring_size, int ioctx_size, int dma_size, enum dma_data_direction dir,
	struct list_head *free_tx, void *priv);
void nvmeib_free_ioctx(struct ib_device *dev,
	struct nvmeib_iu *ioctx, int dma_size, enum dma_data_direction dir);
void nvmeib_free_ioctx_ring(struct nvmeib_iu **ioctx_ring,
	struct ib_device *dev, int ring_size, int dma_size,
	enum dma_data_direction dir, struct list_head *free_tx);
struct nvmesh_memmgr_metrics;
/* the memmgr metric mem_audit param is optional */
void *nvmeib_alloc(struct nvmeib_alloc_info *ai, u32 size, struct nvmesh_memmgr_metrics *mem_audit);
void nvmeib_release(struct nvmeib_alloc_info *ai,
	void *vaddr, struct nvmesh_memmgr_metrics *mem_audit);
void nvmeib_dump_page(void *page);
void nvmeib_dump_buf(const void *buf, int len);

enum nvmeib_cq_vector_get_type {
	NVMEIB_CQ_VECTOR_GET_TYPE_ADMIN = 0,
	NVMEIB_CQ_VECTOR_GET_TYPE_LOCK,
	NVMEIB_CQ_VECTOR_GET_TYPE_IO,
	NVMEIB_CQ_VECTOR_GET_TYPE_NORDDA,
	NVMEIB_CQ_VECTOR_GET_TYPE_DEVCQ,
	MAX_NVMEIB_CQ_VECTOR_GET_TYPE,
};

void nvmeib_cq_vector_get(struct nvmeib_dev *dev, const char *ch_name, enum nvmeib_cq_vector_get_type type,unsigned index, int *scq_vector, int *rcq_vector);
void *nvmeib_alloc_n_map(struct nvmeib_alloc_n_map_info *info);
void nvmeib_free_n_unmap(void *vaddr, struct nvmeib_alloc_n_map_info *info);
/* RQ Routines */
static inline struct nvmeib_iu *nvmeib_rq_rtrv_recv(
	struct nvmeib_recvq *rq, u32 index)
{
	BUG_ON(index >= (u32)rq->rq_queue_size);
	return rq->rx_ring[index];
}
int nvmeib_init_recvq(struct nvmeib_recvq *rq, struct nvmeib_dev *dev,
		      int rq_queue_size, int rq_msg_size);
int nvmeib_fill_recvq(struct nvmeib_recvq *rq, struct ib_qp *qp, int *n_posted);
int nvmeib_post_recvq(struct nvmeib_recvq *rq,
			struct ib_qp *qp, struct nvmeib_iu *iu);
void nvmeib_free_recvq(struct nvmeib_recvq *rq);
int nvmeib_post_sq_drain(struct ib_qp *qp);
int nvmeib_post_rq_drain(struct ib_qp *qp);

struct nvmeib_intr_shaper *nvmeib_intr_shaper_create(u64 frame_size_usecs);
void nvmeib_intr_shaper_destroy(struct nvmeib_intr_shaper *shaper);
unsigned int nvmeib_intr_shaper_get_max_burst(struct nvmeib_intr_shaper *shaper);

enum nvmeib_intr_shaper_calc_ret {
	NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP = 0,
	NVMEIB_INTR_SHAPER_RET_WAKE_UP_BURST = 1,
	NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES = 2,
	NVMEIB_INTR_SHAPER_RET_WAKE_UP_IRQ_TIME = 3,
};

inline static const char *nvmeib_intr_shaper_calc_ret_to_str(int ret)
{
	switch (ret) {
	case NVMEIB_INTR_SHAPER_RET_DONT_WAKE_UP: return "DONT_WAKE_UP";
	case NVMEIB_INTR_SHAPER_RET_WAKE_UP_BURST: return "WAKE_UP_BURST";
	case NVMEIB_INTR_SHAPER_RET_WAKE_UP_CYCLES: return "WAKE_UP_CYCLES";
	case NVMEIB_INTR_SHAPER_RET_WAKE_UP_IRQ_TIME: return "WAKE_UP_IRQ_TIME";
	default: return "UNKNOWN";
	}
}

#define NVMEIB_INTR_SHAPER_OVERLOAD_PCT_MARGIN 3

struct nvmeib_intr_shaper *nvmeib_get_intr_shaper(void);
void nvmeib_intr_shaper_intr_enter(struct nvmeib_intr_shaper *shaper, enum intr_shaper_intr_type intr_type);
void nvmeib_intr_shaper_intr_exit(struct nvmeib_intr_shaper *shaper);
void nvmeib_intr_shaper_intr_polled(struct nvmeib_intr_shaper *shaper, int n_polled);
bool nvmeib_intr_shaper_intr_should_wake_up_reason(struct nvmeib_intr_shaper *shaper, enum nvmeib_intr_shaper_calc_ret *wake_up_reason);
#define nvmeib_intr_shaper_intr_should_wake_up(shaper) nvmeib_intr_shaper_intr_should_wake_up_reason(shaper, NULL)

bool nvmeib_intr_shaper_should_continue_polling(struct nvmeib_intr_shaper *shaper, int n_polled, u64 busy_ns);


/* cpu version of volume_client_config_jrange_cache */
struct nvmeib_jrange_cache
{
	/* range id 0xff... for invalid */
	u32 rng_id;
	/* generation ID for JGC/Cold Recovery */
	u64 gen_id;
	/* known free entries */
	DECLARE_BITMAP(free_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	/* known entries metadata */
	struct nvmeib_jrnl_ent_md ent_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	/* SERJIO Boot ID */
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
	/* number of 'blocks in jentry' of this jrange */
	binje_t rng_binje;
};

struct nvmeib_jrange_rsp
{
	bool valid;
	u32 rng_idx;
	u64 rng_slba;
	u32 rng_nlba;
	u32 rng_binje;
	u64 gen_id;
	DECLARE_BITMAP(free_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	union jblock_md *jmdc;
	struct nvmeib_jrnl_ent_md ent_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	/* SERJIO Boot ID */
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
	/* Number of entries in the range */
	u32 n_ents;
	/* Number of blocks in the range */
	u32 rng_nblk;
	/* Max Journal Blocks in a Range (Used by Recovery) */
	u32 max_rng_blk;
	/* Total number of Journal ranges (Used by Recovery) */
	u32 tot_n_rng;
};



struct nvmeib_local_disk {
	/* pointer to srv's struct nvmeibs_disk_info */
	void *p;
	/* disk-info */
	int sector_shift;
	int  md_size;
	bool md_extd;
	int max_request_size;
	bool external; /* External (SATA/NVMf) Drive */
	/* disk-locks-info */
	int n_ldl;
	struct nvmeib_local_disk_locks *ldl_a;
	struct list_head ldl_list;
	/* journal range */
	struct nvmeib_jrange_rsp jrnl;
	void *jrange_handle;
	/* Using PRPL instead of SGL for NVME Requests */
	bool local_io_use_prpl;

	struct nvmeib_dma_percpu_pools __percpu *dma_pools;
	
	size_t max_prpl_sz;
	dma_addr_t prpl_eof_marker;

	/* Use pool of pages for read-metadata operations */
	bool local_io_use_rd_md_pool;

	/* Use pool of pages for dummy-metadata operations */
	bool local_io_use_md_dma_pool;

	/* Copy write data to DMA pool to avoid CRC race with application buffers */
	bool local_io_use_data_copy;

	size_t md_dma_pool_entry_sz;

	/* Dummy MD areas DMA mapped to drive */
	struct device *dummy_md_dma_dev;
	void *dummy_md_read_ptr;
	dma_addr_t dummy_md_read_addr;
	void *dummy_md_write_ptr;
	dma_addr_t dummy_md_write_addr;
};

struct nvmeib_local_disk_locks {
	/*link to used in containig list*/
	struct list_head link;
	/*lock id*/
	u64 lock_id;
	/*segment id*/
	u32 seg_id;
	/*segment start address*/
	u64 start_addr;
	/*lock set size*/
	u64 lock_set_size;
	/*length of the segments (i.e. number of 4KB disk blocks).*/
	u64 len;
	/*local key that access the segment*/
	u32 rkey;
	u64 addr;
	/* the virt table to access directly the lock buffer */
	struct page **pages;
};

struct nvmeibs_nvme_req;

typedef bool nvmeib_is_local_disk_t(char *name, int *md_size, bool *md_extd);
typedef int nvmeib_cl_register_t(u64 cid, const char *disk_name,
	struct nvmeib_local_disk *ldisk);
typedef int nvmeib_cl_unregister_t(u64 cid, struct nvmeib_local_disk *ldisk);
typedef int nvmeib_cl_alloc_locks_t(u64 cid, struct nvmeib_local_disk *ldisk);
typedef int nvmeib_cl_alloc_jrnl_rng_t(u64 cid, struct nvmeib_local_disk *ldisk,
	const struct nvmeib_jrange_cache *jrc, binje_t binje_req);

/* local server API and related stuff */
typedef int nvmeib_local_cmd_t(struct nvmeib_local_disk *disk,
	struct nvmeibs_nvme_req *req);
struct device_data;
typedef struct device *nvmeib_dma_device_t(struct nvmeib_local_disk *disk);

typedef int nvmeib_gen_cmd_t(struct nvmeib_local_disk *disk,
							 enum nvmeib_gen_cmd_op opcode,
							 const struct nvmeib_gen_cmd_param *p,
							 union nvmeib_gen_cmd_rsp *rsp,
							 u32 cid);

typedef int nvmeib_io_piggyb_cmd_t(struct nvmeib_local_disk *disk,
								   struct nvmeib_io_piggyb_cmd *piggyb_cmd);

typedef void nvmeib_bail_local_async_cookie_channel_t(u32 cid);

struct nvmeib_local_server {
	nvmeib_is_local_disk_t *is_local_disk;
	nvmeib_cl_register_t *cl_register;
	nvmeib_cl_unregister_t *cl_unregister;
	nvmeib_cl_alloc_locks_t *cl_alloc_locks;
	nvmeib_cl_alloc_jrnl_rng_t *cl_alloc_jrnl_rng;
	nvmeib_local_cmd_t *local_cmd;
	nvmeib_dma_device_t *dma_device;
	nvmeib_gen_cmd_t *gen_cmd;
	nvmeib_io_piggyb_cmd_t *io_piggyb_cmd;
	nvmeib_bail_local_async_cookie_channel_t *bail_async_cookie_ch;
	nvmeib_send_msg_to_process_t *send_msg_to_process;
};

bool nvmeib_local_server_up(void);
void nvmeib_register_local_server(struct nvmeib_local_server *s);

/**
 * sets callbacks to be used on server special events
 *
 *
 * @param cb called when local server goes up
 * @param arg context to be passed be he callback
 * @param close_cb called when the server goes down
 */
int nvmeib_set_local_server_notification_calbacks(
	void (*cb)(struct nvmeib_local_server *s, void *arg), void *arg,
	void (*close_cb)(void *arg));
bool nvmeib_local_server_close_client(void);

/* local client API and related stuff */
struct nvmeib_local_client_params {
	void *msg;
	int msg_len;
	bool copy;
};

typedef int nvmeib_toma_request_t(struct nvmeib_local_client_params *p);
struct nvmeib_local_client {
	nvmeib_toma_request_t	*toma_request_f;
};

bool nvmeib_local_client_up(void);
void nvmeib_register_local_client(struct nvmeib_local_client *c);
int nvmeib_set_local_client_notification_calbacks(
	void (*cb)(struct nvmeib_local_client *c, void *arg), void *arg,
	void (*close_cb)(void *arg));
bool nvmeib_local_client_close_server(void);

/**
 * Mechanism for waiting until a all the jobs complete.
 * When launching N jobs, each gets a pointer to this struct
 * which is allocated in one place. If task 'j' wants to launch
 * 'j0' new sub tasks it updates the multi completion
 * accordingly and now we wait for N+j0 tasks to complete
 */
struct nvmeibc_multi_completion {		// Wait for a few tasks to finish
	atomic_t counter;					// Each job which completes, does dec
	struct completion done;				// Last job invokes completion
	//struct workqe_struct work;		// Can schedule completion on wq ?
	//int rv;
};
#define nvmeibc_multi_completion_init_empty() { ATOMIC_INIT(1), {0} }
static inline void nvmeibc_multi_completion_init(
					struct nvmeibc_multi_completion *mc){
	atomic_set(&mc->counter, 1);
	init_completion(&mc->done);
}

static inline void nvmeibc_multi_completion_done(
					struct nvmeibc_multi_completion *mc)
{
	if (mc) {
		const int counter = atomic_dec_return(&mc->counter);
		if (counter == 0 /*&& (mc->done)*/)
			complete(&mc->done);
		/* Danger: Here 'mc' is already invalid. Father job was unblocked and
		   might already free() 'mc' */
		WARN(counter < 0, "nvmeib, mc_counter=%d\n", counter);
	}
}

static inline void nvmeibc_multi_completion_wait_for(
					struct nvmeibc_multi_completion *mc)
{
	wait_for_completion(&mc->done);
}

static inline void nvmeibc_multi_completion_add_aux_jobs(
					struct nvmeibc_multi_completion *mc, int n)
{
	BUG_ON(!mc);		/* Completion must exist*/
	atomic_add(n, &mc->counter);
}

#define do_512b_sub_block_x_val(x)  ((x)^0x20)

/* Given an IO to a sub block, return the actual hw sector it shall be
 * written to. */
static inline u64 nvmeib_translate_sw_addr_to_subblock_addr(u64 sw_addr,
                                                            int sw_sector_shift,
                                                            int hw_sector_shift,
                                                            int subblock) {
	return (sw_addr << (sw_sector_shift - hw_sector_shift)) + subblock;
}

int nvmeib_ibdr_dev_init (bool paging_enabled);
void nvmeib_ibdr_dev_cleanup (void);

/* Common EC functions to client and server filled as function pointer */
void nvmeib_block_dp_ec_dmd_read_mod_wr(void *dmd_ptr, u64 param);
void nvmeib_set_block_dp_ec_funcs(void (*read_mod_wr_dmd)(void*, u64));

/* kth stuff*/
struct nvmeib_public_kth_ft;
const struct nvmeib_public_kth_ft * nvmeib_kth_ft(void);
#if defined(IO_POLL_THREAD) && IO_POLL_THREAD
struct nvmeib_intr_pollers_ft;
void nvmeib_intr_poller_set_ft(struct nvmeib_intr_pollers_ft *ft);
void nvmeib_intr_poller_unset_ft(void);
#endif

#define DEV_CQ_PROCESS_FUNC(name) void name(struct ib_wc *wcs, int n_wcs, unsigned long *wcs_mask, void *ctx)

struct nvmeib_dev_cq * nvmeib_cq_get(struct nvmeib_dev *dev,
	enum channel_type type, DEV_CQ_PROCESS_FUNC((*process)), bool is_mostly_idle, int comp_cpu);
void nvmeib_cq_put(struct nvmeib_dev *dev, struct nvmeib_dev_cq *cq,
	enum channel_type type);

struct nvmeib_srq_info *nvmeib_cq_get_srq(struct nvmeib_dev_cq *cq);
struct ib_cq *nvmeib_cq_get_cq(struct nvmeib_dev_cq *cq);
int nvmeib_cq_get_cpu(struct nvmeib_dev_cq *cq);
int nvmeib_cq_get_intr(struct nvmeib_dev_cq *cq);

int nvmeib_cq_qp_add(struct nvmeib_dev_cq *cq, u64 qp_key, void *qp_ctx);
void nvmeib_cq_qp_stop(struct nvmeib_dev_cq *cq, u64 qp_key);
void nvmeib_cq_qp_del(struct nvmeib_dev_cq *cq, u64 qp_key,
					  struct list_head *qp_action_list);
void nvmeib_dev_drain_cqs(struct nvmeib_dev *dev);

struct nvmeib_cq_qp_action {
	void (*f)(struct nvmeib_cq_qp_action *arg);
	struct list_head action_link;
};

struct nvmeib_cq_qp_destroy_action {
	struct nvmeib_cq_qp_action action;
	struct ib_qp *qp;
	struct nvmeib_rdma_cm *cm_id;
	struct nvmeib_rdma_evt_ctx *rdma_e_ctx;
};
void nvmeib_cq_qp_destroy_action_f(struct nvmeib_cq_qp_action *action);
int nvmeib_dev_cq_stat_hdr(char *buffer, size_t len);
int nvmeib_dev_cq_stat(
		struct nvmeib_dev *dev, char *buffer, size_t len);
int nvmeib_dev_cq_stat_reset(struct nvmeib_dev *dev);

bool nvmeib_support_srq(struct nvmeib_dev *dev);
int nvmeib_create_cq_srq(struct nvmeib_dev *dev, int msg_size, void *memmgr_metrics_ctx);



#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
void nvmeib_c_tree_add(unsigned long key, void *val);
void * nvmeib_c_tree_del(unsigned long key);
void * nvmeib_c_tree_lookup(unsigned long key);
void nvmeib_s_tree_add(unsigned long key, void *val);
void * nvmeib_s_tree_del(unsigned long key);
void * nvmeib_s_tree_lookup(unsigned long key);
#endif

#if !defined(UM_APP)
	#if KS_NEW_TIMER_API
		#if KS_HAS___INIT_TIMER
			#define INIT_TIMER(x) __init_timer((x), 0, 0)
			#define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
						do {                                                    \
								__init_timer((_timer), (_fn), (_flags));        \
								(_timer)->function = (_fn);                     \
						} while (0)
		#else
			#define INIT_TIMER(x) timer_setup((x), NULL, 0)
			#define SETUP_TIMER(_timer, _fn, _data, _flags) \
						timer_setup((_timer), (_fn), (_flags))
		#endif

		#define TIMER_CALLBACK_DECL(func_name) \
			void func_name(struct timer_list* _tl);

		#define TIMER_CALLBACK(func_name, container_type, container_field, data_type, instance)	\
			TIMER_CALLBACK_DECL(func_name) \
			void func_name(struct timer_list* _tl)				\
			{								\
			data_type * instance = (data_type*)container_of(_tl, container_type, container_field)->container_field ## _data;

		#define TIMER_LIST_INSTANCE(container_field)			\
			struct {							\
				struct timer_list	container_field;		\
				unsigned long		container_field ## _data;	\
			};

		#define TIMER_SET_DATA(container, container_field, data)	\
			do { 								\
				container->container_field ## _data = data;		\
			} while (0)

	#else //KS_NEW_TIMER_API

		#define INIT_TIMER(x) init_timer(x)

		#define TIMER_CALLBACK_DECL(func_name) \
		void func_name(unsigned long _data);

		#define TIMER_CALLBACK(func_name, container_type, container_field, data_type, instance)	\
		TIMER_CALLBACK_DECL(func_name) \
		void func_name(unsigned long _data)				\
		{								\
		data_type * instance = (data_type*)_data;

		#define TIMER_LIST_INSTANCE(container_field)	\
		struct timer_list	container_field;

		#define TIMER_SET_DATA(container, container_field, _data)	\
		do { 								\
			container->container_field.data = _data;		\
		} while (0)

		#if KS_INIT_TIMER_KEY_HAS_FLAGS
			#ifdef CONFIG_LOCKDEP
				#define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
					do {                                                    \
							static struct lock_class_key __key;\
							init_timer_key((_timer), (_flags), #_timer, &__key); \
							(_timer)->function = (_fn);                     \
							(_timer)->data = (_data);                       \
					} while (0)
			#else
				#define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
					do {                                                    \
							init_timer_key((_timer), (_flags), NULL, NULL); \
							(_timer)->function = (_fn);                     \
							(_timer)->data = (_data);                       \
					} while (0)
			#endif
		#else
			#ifdef CONFIG_LOCKDEP
				#define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
					do {                                                    \
							static struct lock_class_key __key;\
							init_timer_key((_timer), #_timer, &__key); 	\
							(_timer)->function = (_fn);                     \
							(_timer)->data = (_data);                       \
					} while (0)
			#else
				#define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
					do {                                                    \
							init_timer_key((_timer), NULL, NULL); 			\
							(_timer)->function = (_fn);                     \
							(_timer)->data = (_data);                       \
					} while (0)
			#endif
		#endif
	#endif
#else //UM_APP
	#define TIMER_LIST_INSTANCE(container_field) struct { struct timer_list	container_field; unsigned long container_field ## _data; };
	#define TIMER_CALLBACK_DECL(func_name) void func_name(unsigned long _data);
	#define TIMER_CALLBACK(func_name, container_type, container_field, data_type, instance)	\
			TIMER_CALLBACK_DECL(func_name) \
			void func_name(unsigned long _data) { data_type * instance = (data_type*)_data; BUG();
	#define INIT_TIMER(...)
	#define TIMER_SET_DATA(...)
	#define SETUP_TIMER(...)

#endif //UM_APP

struct nvmeib_alloc_n_map;
bool nvmeib_same_physical_dev(struct nvmeib_dev *d1, struct nvmeib_dev *d2);
bool nvmeib_is_same_physical_ib_dev(struct ib_device *d1, struct ib_device *d2);

int nvmeib_mem_unmapn_n_free(struct nvmeib_alloc_n_map *mem);

void nvmeib_mem_vunmap_n_free(struct nvmeib_alloc_n_map *mem,
									 void *vaddr);

int nvmeib_mem_alloc_n_map(struct nvmeib_alloc_n_map *mem);

#define NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN		(~(size_t)0)

void nvmeib_mem_sync_map_for_device(struct nvmeib_alloc_n_map *mem, off_t offset, size_t len);
void nvmeib_mem_sync_map_for_cpu(struct nvmeib_alloc_n_map *mem, off_t offset, size_t len);

int nvmeib_peek_cq(struct ib_cq *cq, int max);


int nvmeib_query_device(struct ib_device *device,
							   struct ib_device_attr *device_attr);

int nvmeib_post_send_atomic(struct ib_qp *qp,
								   struct nvmeib_send_wr *send_wr,
								   struct nvmeib_send_wr **bad_send_wr);

void* nvmeib_mem_alloc_n_vmap(struct nvmeib_alloc_n_map *mem);

void* nvmeib_vmap(void **virts, int n);

#ifdef NVMEIB_WORKQ
	#define on_wq(_wq) (!in_interrupt() && current->pid == wq_pid(_wq))
	#define on_wq_pid(_wq_pid) (!in_interrupt() && current->pid == _wq_pid)
#else
	#if defined(__KERNEL__)
		#warning "Cannot determine wq-pid of GPL WQ, YAOYO"
	#endif
	#define on_wq(_wq) (!in_interrupt())
	#define on_wq_pid(_wq_pid) (!in_interrupt())
#endif

#ifndef per_cpu_var
#	define per_cpu_var(x)	(x)
#endif

#if (KS_GET_BDEV_BY_PATH_EXISTS)
	/* Already defined in new kernels */
#else
static inline struct block_device *blkdev_get_by_path(const char *path, fmode_t mode, void *holder)
{
	struct block_device *bdev = lookup_bdev(path);
	(void)holder;				// Unused, we dont do exclusive
	if (!IS_ERR(bdev)) {
		const int err = blkdev_get(bdev, mode);
		if (err)
			return ERR_PTR(err);
		if ((mode & FMODE_WRITE) && bdev_read_only(bdev)) {
			blkdev_put(bdev, mode);
			return ERR_PTR(-EACCES);
		}
	}
	return bdev; // Note: This funtion does open() on our os.
}
#endif

enum nvmeib_cnt_mem_type {
	NVMEIB_CNT_MEM_KMEM = 0,
	NVMEIB_CNT_MEM_DMA,
	NVMEIB_CNT_MEM_PAGES,
	NVMEIB_CNT_MEM_VIRT,
	/* Same as above, but specifically allocated by the IB layer */
	NVMEIB_CNT_MEM_IB_KMEM,
	NVMEIB_CNT_MEM_IB_DMA,
	NVMEIB_CNT_MEM_IB_PAGES,
	NVMEIB_CNT_MEM_IB_VIRT,
	NVMEIB_CNT_MEM_MAX,
};

#if defined(NVMEIB_COUNT_MEM_USAGE)

#pragma GCC diagnostic ignored "-Wframe-address"

enum nvmeib_alloc_type {
	NVMEIB_ALLOC_KMALLOC = 0,
	NVMEIB_ALLOC_KMALLOC_ARRAY,
	NVMEIB_ALLOC_KMALLOC_NODE,
	NVMEIB_ALLOC_KSTRDUP,
	NVMEIB_ALLOC_KMEMDUP,
	NVMEIB_ALLOC_DMA_ALLOC,
	NVMEIB_ALLOC_IB_DMA_ALLOC,
	NVMEIB_ALLOC_GET_PAGES,
	NVMEIB_ALLOC_PAGES,
	NVMEIB_ALLOC_PAGES_NODE,
	NVMEIB_ALLOC_VMALLOC,
	NVMEIB_ALLOC_IB_VERBS_KMEM,
	NVMEIB_ALLOC_IB_VERBS_DMA,
	NVMEIB_ALLOC_IB_VERBS_PAGES,
	NVMEIB_ALLOC_IB_VERBS_VIRT,
	NVMEIB_ALLOC_MAX,
};

enum nvmeib_free_type {
	NVMEIB_FREE_KFREE = 0,
	NVMEIB_FREE_DMA_FREE,
	NVMEIB_FREE_IB_DMA_FREE,
	NVMEIB_FREE_PAGES,
	NVMEIB_FREE_VFREE,
	NVMEIB_FREE_IB_VERBS_KMEM,
	NVMEIB_FREE_IB_VERBS_DMA,
	NVMEIB_FREE_IB_VERBS_PAGES,
	NVMEIB_FREE_IB_VERBS_VFREE,
};

#ifdef NVMEIB_COUNT_MEM_USAGE_BACKTRACE
#define NVMEIB_CNT_ALLOC_LOC_ID (((((u64)__FILE__ & ~(u32)0) << 32) | __COUNTER__) ^ ((u64)__builtin_return_address(0) ^ (u64)__builtin_return_address(1) ^ (u64)__builtin_return_address(2)))
#define NVMEIB_CNT_ALLOC_LOC_PARAMS __FILE__, __LINE__, __FUNCTION__, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2), NVMEIB_CNT_ALLOC_LOC_ID
#define NVMEIB_CNT_FREE_LOC_PARAMS	__FILE__, __LINE__, __FUNCTION__
#else
#define NVMEIB_CNT_ALLOC_LOC_ID ((((u64)__FILE__ & ~(u32)0) << 32) | __COUNTER__)
#define NVMEIB_CNT_ALLOC_LOC_PARAMS __FILE__, __LINE__, __FUNCTION__, NULL, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_ID
#define NVMEIB_CNT_FREE_LOC_PARAMS	__FILE__, __LINE__, __FUNCTION__
#endif

void *nvmeib_cnt_alloc(enum nvmeib_alloc_type alloc_type, size_t size, gfp_t flags,
						/* kmalloc_array - size_t n */
						/* kmalloc_node - int n */
						size_t p1,
						/* dma_alloc_coherent - (struct device *) */
						/* ib_dma_alloc_coherent - (struct ib_device *) */
						const void *p2,
						/* dma_alloc_coherent - (dma_addr_t *) */
						/* ib_dma_alloc_coherent - (dma_addr_t *) */
						const void *p3,
						const char *file, int line, const char *fn,
						const void *bt0, const void *bt1, const void *bt2, u64 loc_id);

void nvmeib_cnt_free(enum nvmeib_free_type free_type, const void *ptr,
					/* dma_free_coherent - (struct device *) */
					/* ib_dma_free_coherent - (struct ib_device *) */
					const void *p1,
					/* dma_free_coherent - (size_t size) */
					/* ib_dma_free_coherent - (size_t size) */
					size_t p2,
					/* dma_free_coherent - (dma_addr_t dma_handle) */
					/* ib_dma_free_coherent - (dma_addr_t dma_handle) */
					dma_addr_t p3,
					const char *file, int line, const char *fn);

ssize_t nvmeib_cnt_print(enum nvmeib_cnt_mem_type mem_type, char *buffer, size_t len);

#if !defined(__NVMEIB_C__) && !defined(NVMEIB_NO_MEM_CNT)

/* kXalloc functions */
#define kmalloc(s, f)						nvmeib_cnt_alloc(NVMEIB_ALLOC_KMALLOC, s, f, 0, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kmalloc_array(n, s, f)				nvmeib_cnt_alloc(NVMEIB_ALLOC_KMALLOC_ARRAY, s, f, n, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kmalloc_node(s, f, n)				nvmeib_cnt_alloc(NVMEIB_ALLOC_KMALLOC_NODE, s, f, n, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kzalloc_node(s, f, n)				nvmeib_cnt_alloc(NVMEIB_ALLOC_KMALLOC_NODE, s, f | __GFP_ZERO, n, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kzalloc(s, f)						nvmeib_cnt_alloc(NVMEIB_ALLOC_KMALLOC, s, f | __GFP_ZERO, 0, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kcalloc(n, s, f) 					nvmeib_cnt_alloc(NVMEIB_ALLOC_KMALLOC_ARRAY, s, f | __GFP_ZERO, n, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kstrdup(str, f)						nvmeib_cnt_alloc(NVMEIB_ALLOC_KSTRDUP, strlen(str) + 1, f, 0, str, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kmemdup(p, s, f)					nvmeib_cnt_alloc(NVMEIB_ALLOC_KMEMDUP, s, f, 0, p, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define kfree(p)							do {if (p) nvmeib_cnt_free(NVMEIB_FREE_KFREE, p, NULL, 0, 0, NVMEIB_CNT_FREE_LOC_PARAMS);} while(0)

/* dma alloc functions */
#pragma push_macro("dma_alloc_coherent")
#undef dma_alloc_coherent

#pragma push_macro("dma_free_coherent")
#undef dma_free_coherent

#define dma_alloc_coherent(d, s, h, f)		nvmeib_cnt_alloc(NVMEIB_ALLOC_DMA_ALLOC, s, f, 0, (void *)d, (void *)h, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define dma_free_coherent(d, s, p, h)		nvmeib_cnt_free(NVMEIB_FREE_DMA_FREE, p, (void *)d, s, h, NVMEIB_CNT_FREE_LOC_PARAMS)
#define ib_dma_alloc_coherent(d, s, h, f)	nvmeib_cnt_alloc(NVMEIB_ALLOC_IB_DMA_ALLOC, s, f, 0, (void *)d, (void *)h, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define ib_dma_free_coherent(d, s, p, h)	nvmeib_cnt_free(NVMEIB_FREE_IB_DMA_FREE, p, (void *)d, s, h, NVMEIB_CNT_FREE_LOC_PARAMS)

/* get page functions */

#undef __get_free_page
#undef free_page
#undef __free_pages

#define __get_free_pages(f, o)				(unsigned long)nvmeib_cnt_alloc(NVMEIB_ALLOC_GET_PAGES, (PAGE_SIZE << o), f, o, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define __get_free_page(f)					(unsigned long)__get_free_pages(f, 0)
#define get_zeroed_page(f)					(unsigned long)nvmeib_cnt_alloc(NVMEIB_ALLOC_GET_PAGES, PAGE_SIZE, f | __GFP_ZERO, 0, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)

#define alloc_pages(f, o)			nvmeib_cnt_alloc(NVMEIB_ALLOC_PAGES, (PAGE_SIZE << o), f, o, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define alloc_pages_node(n, f, o)	\
({\
	unsigned long ul_node = n;\
	struct page *pgs = nvmeib_cnt_alloc(NVMEIB_ALLOC_PAGES_NODE, (PAGE_SIZE << o), f, o, (void*)ul_node, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS);\
	pgs;\
})

#define free_page(p)						nvmeib_cnt_free(NVMEIB_FREE_PAGES, (void *)p, NULL, 0, 0, NVMEIB_CNT_FREE_LOC_PARAMS)
#define free_pages(p, o)					nvmeib_cnt_free(NVMEIB_FREE_PAGES, (void *)p, NULL, o, 0, NVMEIB_CNT_FREE_LOC_PARAMS)

#define __free_pages(p, o)					do { if (p) nvmeib_cnt_free(NVMEIB_FREE_PAGES, page_address(p), NULL, o, 0, NVMEIB_CNT_FREE_LOC_PARAMS); } while(0)

/* vmalloc functions */
#define vmalloc(s)							nvmeib_cnt_alloc(NVMEIB_ALLOC_VMALLOC, s, 0, 0, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define vzalloc(s)							nvmeib_cnt_alloc(NVMEIB_ALLOC_VMALLOC, s, __GFP_ZERO, 0, NULL, NULL, NVMEIB_CNT_ALLOC_LOC_PARAMS)
#define vfree(p)							do {if (p) nvmeib_cnt_free(NVMEIB_FREE_VFREE, p, NULL, 0, 0, NVMEIB_CNT_FREE_LOC_PARAMS);} while(0)

/* ib functions */

#endif /* !defined(__NVMEIB_C__) && !defined(NVMEIB_NO_MEM_CNT) */

#endif /* defined(NVMEIB_COUNT_MEM_USAGE) */
u64 nvmeib_get_guid(void);
struct nvmeib_dcmanager * nvmeib_get_dcmanager(void);

ssize_t nvmeib_add_proc_status_footer(char *buffer, size_t len, const char *title);

enum nvmeib_numa_alloc_policy_type {
	NVMEIB_NUMA_POLICY_THIS_CPU_NODE = 0, /* Default behaviour, allocate on the node that happened to run the allocation. */
	NVMEIB_NUMA_POLICY_RR_ALL_NODES = 1, /* Round robin on all numas. */
	NVMEIB_NUMA_POLICY_RR_DEV_SOCKET_NODES = 2, /* Round robin on all numas within the socket of a certain device. */
};

#define GFP_ALLOC_N_MAP GFP_KERNEL | __GFP_ZERO
struct nvmeib_alloc_n_map {
	/* the protection domain of the device we would like
	   to register the memory with
	*/
	struct ib_pd *pd;
	/* the device virtual address we would like to use */
	u64 ioaddr;
	/* the access flags e.g.
		IB_ACCESS_LOCAL_WRITE |
		IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE|
	    IB_ACCESS_REMOTE_ATOMIC|
	*/
	int access_flags;
	/* indicate if we allocated the memory */
	bool allocated;
	/* When allocating memory, allocate in groups of 2^x (default is 0 for groups of 1) */
	unsigned alloc_order_n;
	/* dma direction to use for mapping (either DMA_FROM_DEVICE, DMA_TO_DEVICE or DMA_BIDIRECTIONAL) */
	enum dma_data_direction dma_dir;
	/* the allocated memory - whether caller or we allocated it */
	union {
		struct page **pages;
		dma_addr_t *dma_pages;
	};
	unsigned n_pages;
	bool use_dma_pages;
	/* SGL for DMA Mapping */
	struct sg_table mem_table;
	unsigned int map_sg_nents;
	/* the resulting memory region - the keys */
	struct ib_mr *mr;
	/* the local & remote keys */
	u32 lkey;
	u32 rkey;

	enum nvmeib_numa_alloc_policy_type alloc_policy; /* Will be overriden by the value of nvmeib_numa_alloc_policy*/
};

/* -------------------------------------------------------------------------- */
/* QP statistics                                                              */
/* -------------------------------------------------------------------------- */

/* per qp pcpu-stats both for nic and disk devices */
struct nvmeib_qp_stats_pcpu_per_context {
	u64 n_executions;  			/* number time interrupt fired or offload-thread iterations */
	u64 n_post_sends;			/* number of post-sends */
	u64 n_poll_cq; 				/* number of times CQ was polled */
	u64 n_poll_cq_send_cqes;	/* number of send  CQEs polled on last poll-cq -> calc CQEs to Interrupt/Offload-iteration ratio */
	u64 n_poll_cq_recv_cqes;	/* number of recv  CQEs polled on last poll-cq -> calc CQEs to Interrupt/Offload-iteration ratio */
	u64 n_poll_cq_mixed_cqes;	/* number of mixed CQEs polled on last poll-cq -> calc CQEs to Interrupt/Offload-iteration ratio */
	u64 n_poll_cq_empty;		/* number of poll-cq found 0 entries */
	u64 n_offth_sched;			/* number of times offload thread was scheduled*/
	u64 n_rearm_irqs;			/* [unused] number of times interrupts were rearmed */
};

struct nvmeib_qp_stats_pcpu {
	struct nvmeib_qp_stats_pcpu_per_context hi;
	struct nvmeib_qp_stats_pcpu_per_context si;
	struct nvmeib_qp_stats_pcpu_per_context sy;
};

#define QPS_STATS_PAD_BLANKS_LEN_LINE_NUM	(6)	//"[%03d] "
#define QPS_STATS_PAD_BLANKS_LEN_NAME		(64)
#define QPS_STATS_PAD_BLANKS_LEN_CNT 		(8)

struct nvmeib_qp_stats_pcpu *nvmeib_qp_stats_alloc(void);
void nvmeib_qp_stats_free(struct nvmeib_qp_stats_pcpu *s);

struct nvmeib_pool_percpu_counts __percpu *nvmeib_alloc_percpu_pool_counts(void);

#if defined (NVMEIB_QP_STATS) && (NVMEIB_QP_STATS==1)

#define nvmeib_qp_stats_this_cpu_ptr(__s) 						\
	((struct nvmeib_qp_stats_pcpu *)this_cpu_ptr(__s))

#define nvmeib_qp_stats_this_cpu_ptr_context(__s) ({						\
	struct nvmeib_qp_stats_pcpu_per_context *__c; 							\
	if 		(in_irq())		__c = &(nvmeib_qp_stats_this_cpu_ptr(__s)->hi);	\
	else if (in_softirq())  __c = &(nvmeib_qp_stats_this_cpu_ptr(__s)->si); \
	else					__c = &(nvmeib_qp_stats_this_cpu_ptr(__s)->sy); \
	__c;																	\
})

#define nvmeib_qp_stats_on_post_send(__s) do {					\
	nvmeib_qp_stats_this_cpu_ptr_context(__s)->n_post_sends++;	\
} while (0)

#define nvmeib_qp_stats_on_post_send_add(__s, __n) do {				\
	nvmeib_qp_stats_this_cpu_ptr_context(__s)->n_post_sends += __n;\
} while (0)

#define nvmeib_qp_stats_on_poll_cq(__s, __n, __type) do {		\
	struct nvmeib_qp_stats_pcpu_per_context *__c =				\
	nvmeib_qp_stats_this_cpu_ptr_context(__s);					\
	__c->n_poll_cq++; 											\
	__c->n_poll_cq_##__type##_cqes += n;						\
} while (0)

#define nvmeib_qp_stats_on_update_cqes(__s, __n_send, __n_recv) do {	\
	struct nvmeib_qp_stats_pcpu_per_context *__c =				\
	nvmeib_qp_stats_this_cpu_ptr_context(__s);					\
	__c->n_poll_cq_send_cqes += __n_send;						\
	__c->n_poll_cq_recv_cqes += __n_recv;						\
} while (0)

#define nvmeib_qp_stats_on_interrupt(__s) do {					\
	struct nvmeib_qp_stats_pcpu_per_context *__c =				\
	nvmeib_qp_stats_this_cpu_ptr_context(__s);					\
	__c->n_executions++; 										\
} while (0)

#define nvmeib_qp_stats_on_offth_iter(__s) do {					\
	nvmeib_qp_stats_this_cpu_ptr(__s)->sy.n_executions++;		\
} while (0)

#define nvmeib_qp_stats_on_offload_sched(__s) do {				\
	nvmeib_qp_stats_this_cpu_ptr_context(__s)->n_offth_sched++; \
} while (0)

#define nvmeib_qp_stats_on_poll_cq_empty(__s) do {				\
	struct nvmeib_qp_stats_pcpu_per_context *__c =				\
	nvmeib_qp_stats_this_cpu_ptr_context(__s);					\
	__c->n_poll_cq_empty++;										\
} while (0)

#define nvmeib_qp_stats_on_rearm(__s)

ssize_t nvmeib_qp_stats_header_fill(void *arg, char *buf, size_t len);
ssize_t nvmeib_qp_stats_fill(struct nvmeib_qp_stats_pcpu *s, char *buf, size_t len);
#define nvmeib_qp_stats_reset(_s) nvmeib_public_zero_percpu(_s);

#else /* NVMEIB_QP_STATS */

#define nvmeib_qp_stats_this_cpu_ptr(__s)
#define nvmeib_qp_stats_this_cpu_ptr_context(__s)
#define nvmeib_qp_stats_on_post_send(__s)
#define nvmeib_qp_stats_on_post_send_add(__s, __n)
#define nvmeib_qp_stats_on_poll_cq(__s, __n, __type)
#define nvmeib_qp_stats_on_update_cqes(__s, __n_send, __n_recv)
#define nvmeib_qp_stats_on_interrupt(__s)
#define nvmeib_qp_stats_on_offth_iter(__s)
#define nvmeib_qp_stats_on_offload_sched(__s)
#define nvmeib_qp_stats_on_poll_cq_empty(__s)
#define nvmeib_qp_stats_on_rearm(__s)
#define nvmeib_qp_stats_header_fill(arg, buf, len) (0)
#define nvmeib_qp_stats_fill(s, buf, len) (0)
#define nvmeib_qp_stats_reset(_s) (0)

#endif /* NVMEIB_QP_STATS */

unsigned int nvmeib_get_tcp_base_port_id(void);
unsigned int nvmeib_get_tcp_num_ports(struct nvmeib_dev *dev);
bool nvmeib_is_dev_in_blacklist(struct ib_device *ib_dev);
bool nvmeib_dev_use_keeper(struct nvmeib_dev *dev);

/* Forward declaration for nvmeib_pcpu_wq */
struct nvmeib_pcpu_wq;
extern struct nvmeib_pcpu_wq *nvmeib_system_wq;

#define nvmeib_get_system_wq() (nvmeib_system_wq)

#endif /* NVMEIB_H */
