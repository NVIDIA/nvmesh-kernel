#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "nvmeibc_block_dp_ec_recov_hot.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_reed_solomon.h"
#include "nvmeibc_block_dp_ec_recovery_common.h"
#include "nvmeibc_block_dp_ec_recov_maintenance.h"
#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole.h"
#include "kth/nvmeib_public_kth.h"
#include "nvmeib_kth_events.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "nvmeibc_disk_locks.h"
#include "nvmeibc_icore_ops.h"
#include "../nvmeibc_block_dp_ec.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_hot_dbgdi.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeibc_block_dp_ec_recov_stats.h"
#include "nvmeib_shared.h"
#include "nvmeib_macro_utils.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeibc_io_pet.h"

NVMEIBC_MEMMGR_METRIC(dp_recovery_hot, "component=raid.io.sync.stale_lock");

#define HTR_DEBUG 1

#define HTR_INVALID_SEG_ID 		((int)NVMEIB_EC_INVALID_JOURNAL_ENTRY)
#define HTR_INVALID_JMD_ID 		(u16)(-1)
#define HTR_INVALID_LBA 		(~(0ULL))
#define HTR_INVALID_EDIC_STS	(-1)
#define HTR_INVALID_N_SLICE_JOUR (NVMEIB_EC_JOURNAL_MIN_BLOCKS_PER_ENTRY-1)

/* HTR is per slice, 4KB data per disk */
#define HTR_SG_NENTS (1)
#define HTR_CMD_TIMEOUT (HZ)
#define HTR_REGEN_VEC_LEN (N_MAX_RAID_SLICE_LEN)
#define HTR_MAX_FREE_ENTS (1)

/* printk EC segments bitmap */
#define psbm(_bm_) N_MAX_RAID_SLICE_LEN, &(_bm_)

#define has_rw_parity(h) (!((h->rlba_parities_bm & nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable)) == 0))
#define __is_seg_parity(si, h) ((1 << (si)) & (h)->rlba_parities_bm)

#define NBUG_ON_MSG(name, cond, fmt, ...)	\
	do { if (cond) {  _NE(name, DMESG_PREFIX() ": " fmt, ## __VA_ARGS__); BUG(); } } while (0)

#define Nkfree_nullify(name, _addr_)	do {				\
	if (!_addr_) {											\
		_NE(name, DMESG_PREFIX() ": Trying to free NULL addr " #_addr_ "...\n");	\
	} else {												\
		htr_kfree(_addr_);										\
		_addr_ = NULL;										\
	}														\
} while (0)

#define Nfree_pages_nullify(name, _addr_, _order_) do {		\
	if (!_addr_) {											\
		_NE(name,  DMESG_PREFIX() ": Trying to free NULL pages " #_addr_ "...\n");	\
	} else {												\
		htr_free_pages((unsigned long)_addr_, _order_);			\
		_addr_ = NULL;										\
	}														\
} while (0)

#define Nfree_pages_exact_nullify(name, _virt, _size) do {		\
if (!_virt) {											\
	_NE(name,  DMESG_PREFIX() ": Trying to free NULL pages " #_virt "...\n");	\
} else {												\
	htr_free_pages_exact(_virt, _size);			\
	_virt = NULL;										\
}														\
} while (0)

static void *htr_kzalloc(size_t size, gfp_t flags)
{
	void *ptr = kzalloc(size, flags);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_hot, ptr? ksize(ptr): size, ptr);
	return ptr;
}

static void *htr_kcalloc(size_t n, size_t size, gfp_t flags)
{
	void *ptr = kcalloc(n, size, flags);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_hot, ptr? ksize(ptr): size, ptr);
	return ptr;
}

static void htr_kfree(void *ptr)
{
	if (ptr)
		nvmesh_memmgr_metric_on_free_update(dp_recovery_hot, ksize(ptr));
	kfree(ptr);
}

static void *htr_alloc_pages_exact(size_t size, gfp_t gfp_mask)
{
	void *ptr = alloc_pages_exact(size, gfp_mask);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_hot, size, ptr);
	return ptr;
}

static void htr_free_pages_exact(void *virt, size_t size)
{
	if (virt)
		nvmesh_memmgr_metric_on_free_update(dp_recovery_hot, size);
	free_pages_exact(virt, size);
}

static unsigned long htr__get_free_pages(gfp_t gfp_mask, unsigned int order)
{
	unsigned long addr = __get_free_pages(gfp_mask, order);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_hot, (1 << order) * PAGE_SIZE, addr);
	return addr;
}

static void htr_free_pages(unsigned long addr, unsigned int order)
{
	if (addr)
		nvmesh_memmgr_metric_on_free_update(dp_recovery_hot, (1 << order) * PAGE_SIZE);
	free_pages(addr, order);
}


struct nvmeib_public_kth_obj htr_base;	// Unused dummy u64

/* ===========================
 * Future Support
 * ===========================
 * o Detect Final-Read-Error (preserve comp-code from Transport layer)
 * o Support fixing more than 1 slice (multi pages per cmd, PMD update).
 * o Support kth's stop event (sent from parent).
 * o Change any usage of jblock_md to
 *   nvmeibc_jc_md_base or the other way around.
 *
 * ===========================
 * Integration
 * ===========================
 * o EDIC calc via Regen (gf's encode/decode API)
 * o Updated Dbits format for single degraded (?)
 *
 * ===========================
 * Internal Stuff
 * ===========================
 * o Major:
 *   - KTH parent/manager (use all CPUs)
 * o Minor:
 *   - Dont use ptrs from Daniel's
 *   - Macros for set/clear/test bit in bm of si
 *   - Macro to (reset then) calc  xxx_bm and n_xxx that satisfies some cond
 * o Code cleanup
 *   - Naming: H_/h vs HTR_/htr
 *   - Unite check_params, init_params etc (no need for raid1_for_each_seg)
 *   - Unite htr-params, htr_ctx, etc.
 *
 * ===========================
 * Open Issues
 * ===========================
 * o Endianness - jmdc (txbm), DMD/PMD, lock-ent, other?
 *
 * ===========================
 * Optimization
 * ===========================
 * o Dont send DATA to client when all needed is the MD.
 *   Target to check EDIC and cache the data if requested.
 *   - jblk: # We can later ask Target to do the roll-fwd.
 *           # We may need the jblk data to regen other jblks
 *   - dblk: # We may need the dblk data to regen W segs
 * o Use static kth-event and not kzalloc.
 * o If there are 2 consecutive async-op and possible, dont wait for
 *   first batch to finish before submitting the second one. instead,
 *   let the comp of first one, per seg, trigger the next op on this seg.
 *   E.g. read-jmdc to be followed by scan-jmdc.
 */

enum htr_name_fmt {
	HTR_NAME_FMT_VOL_LEN = NVMEIBC_BD_NAME_LEN,
	HTR_NAME_FMT_BLKSET_LEN = 16,
	HTR_NAME_FMT_TXID_LEN = 6,

	HTR_NAME_FMT_LEN = \
		1+1+NVMEIBC_BD_NAME_LEN+1+\
		3+1+HTR_NAME_FMT_BLKSET_LEN+1+\
		4+1+HTR_NAME_FMT_TXID_LEN+1
};

#define htr_set_name(_h_) (\
 scnprintf(_h_->name, sizeof(_h_->name), "V.%.*s.B.0x%0*llx.Tx.0x%0*x.", \
 HTR_NAME_FMT_VOL_LEN, _h_->so->o->nd->name, \
 HTR_NAME_FMT_BLKSET_LEN, _h_->params.rlba / (_h_->params.raid1->slice_size * LOCKSET_4KS), /* Blockset number in praid */\
 HTR_NAME_FMT_TXID_LEN, _h_->params.recoveree_txid) \
 )

#define NHTR_TRACE(n, LVL, _h_, fmt, ...) \
	_N ## LVL (n, "HTR @HTR_PARAM: " fmt, (_h_ ? _h_->name : "<UNK>"), ## __VA_ARGS__)

#define _NDh(name, _h_, fmt, ...) NHTR_TRACE(name, D, _h_, fmt, ## __VA_ARGS__)
#define _NTh(name, _h_, fmt, ...) NHTR_TRACE(name, T, _h_, fmt, ## __VA_ARGS__)
#define _NEh(name, _h_, fmt, ...) NHTR_TRACE(name, E, _h_, fmt, ## __VA_ARGS__)				// All calls use DMESG_PREFIX()

#define HTR_STATS_INC(name)    ({ atomic_inc(&get_so_fctr((h)->so)->htrs.name); })
#define HTR_STATS_INC_SO(name) ({ atomic_inc(&get_so_fctr(     so)->htrs.name); })
#define HTR_STS_STR_LEN 256

#define NHTR_STS(name, _h_, fmt, ...) do { \
	if (_h_->sts_str[0] == '\0') {                                                                                 \
		scnprintf(_h_->sts_str, HTR_STS_STR_LEN, N_FORMAT_STRING_FOR_PRINT(__fine_##name, ##__VA_ARGS__));         \
		_NTh(name, _h_, fmt, ##__VA_ARGS__);                                                                       \
		_NF(__fine_##name, fmt, ##__VA_ARGS__);                                                                    \
	} else {                                                                                                       \
		_NEh(__warn_##name, _h_, DMESG_PREFIX() ": status already @HTR_STATUS (ignore " fmt ")", _h_->sts_str, ##__VA_ARGS__);      \
		WARN_ON_ONCE(1);                                                                                           \
	}                                                                                                              \
} while (0)

struct htr_ctx;
typedef int (*h_op_f)(struct htr_ctx *h, int si);
typedef int (*h_op_comp_f)(struct htr_ctx *h, int si);

enum h_seg_op_status {
	H_SEG_OP_STS_IDLE = 0,
	H_SEG_OP_STS_INPROGRESS,
	/* results */
	H_SEG_OP_STS_OK,
	H_SEG_OP_STS_ERR,
};

struct seg_info {
	int si;
	enum h_seg_op_status op_sts;

	/* For disk IO command */
	struct nvmeibc_block_command cmd;
	struct nvmeib_data_buffer ndb;			// Todo: Remove me

	/* IO 4KB data of Data/Journal block */
	struct {
		u8 valid[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // 1 == valid
		void *data_addr[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // TODO use cmd's buffer like in nwhole
		union {
			union jblock_md jmd;
			union nvmeibc_block_dp_ec_data_block_md dmd;
			u8 raw[DISK_MAX_MD_SIZE_BYTE];	// Allocate maximal possible size
		} *md[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];   // TODO use cmd's buffer like in nwhole
		int edic_sts;
		u32 edic_res;
	} dblk;

	/* Journal-info of 'IO-Client',
	   used only by Topo's RW segs */
	struct cl_jour {
		bool valid;
		struct nvmeib_disk_client_journal_extended desc;
		/* For receiving the JMDC and Entry Metadata */
		union jblock_md *jmdc;
		size_t jmdc_size;
		u64 rng_gen_id;
		struct nvmeib_jrnl_ent_md *ent_md;
		size_t ent_md_sz;
	} clj;

	/* Free entries command */
	struct nvmeibc_disk_free_jrnl_ents_comp free_ents_comp;
};

struct state_bm {	// All bitmaps represent segments
	sgmnts_bmp_t all;
	sgmnts_bmp_t rw;
	sgmnts_bmp_t w;
	sgmnts_bmp_t d;
};

enum h_op {
	h_op_invalid = 0,

	h_op_read_jmdc,
	h_op_read_dblk,
	h_op_read_jblk,
	h_op_jblk_edic_calc,
	h_op_write_dblk,
	h_op_send_recovered,

	h_op_last,
	h_op_extern_sm,	// Execute external state machine, not part of the execution plan above
};

struct h_op_func {
	h_op_f f;
	h_op_comp_f comp_f;
};

static int read_jmdc      (struct htr_ctx *h, int si);
static int scan_jmdc(struct htr_ctx *h, int si, bool is_pivot);
static int read_dblk      (struct htr_ctx *h, int si);
static int read_jblk      (struct htr_ctx *h, int si);
static int jblk_edic_calc (struct htr_ctx *h, int si);
static int write_dblk     (struct htr_ctx *h, int si);
static int send_recovered (struct htr_ctx *h, int si);

static int read_jmdc_comp (struct htr_ctx *h, int si);
static int read_dblk_comp (struct htr_ctx *h, int si);
static int read_jblk_comp (struct htr_ctx *h, int si);
static int write_dblk_comp(struct htr_ctx *h, int si);
static int send_recovered_comp(struct htr_ctx *h, int si);

static int op_func_trap   (struct htr_ctx *h, int si);

struct h_op_func h_ops_funcs[] = {
	[h_op_invalid		 ] = {(h_op_f)(~(0ULL)), (h_op_comp_f)(~(0ULL))},

/*  [h_op_##<func>       ] = {<func>         , <func>##_comp  } */
	[h_op_read_jmdc      ] = {read_jmdc      , read_jmdc_comp 		},
	[h_op_read_dblk      ] = {read_dblk      , read_dblk_comp 		},
	[h_op_read_jblk      ] = {read_jblk      , read_jblk_comp 		},
	[h_op_jblk_edic_calc ] = {jblk_edic_calc , NULL 				},
	[h_op_write_dblk     ] = {write_dblk     , write_dblk_comp		},
	[h_op_send_recovered ] = {send_recovered , send_recovered_comp  },
	[h_op_last           ] = {op_func_trap   , op_func_trap         },
	[h_op_extern_sm      ] = {op_func_trap   , op_func_trap         } //better warning then segmentation fault
};

struct nvmeibc_block_dp_ec_recov_hot_params {
	/* uuid of the client stale-locking the blockset,
	   used for retrieving journal from target(s) */
	uuid_be recoveree_cuuid;
	struct nvmeibc_raid1 *raid1;
	u64 rlba; /* First rlba in the lockset */
	union nvmeib_lock_blkset_entry lock_ent;
	/* TxID of IO (expected in jour and data MD) */
	u32 recoveree_txid;
	/* For cancelling the request */
	uuid_be caller_uuid;
	bool is_hot_jgc;
	bool is_cold;
};

struct htr_ctx {
	struct nvmeib_public_kth_id htr_kth;
	struct recovery_sync_op *so;

	char name[HTR_NAME_FMT_LEN];
	struct nvmeibc_block_dp_ec_recov_hot_params params; //omril: do we still need this now that we have so?

	int n_segs;
	struct seg_info *seg_info;
	struct jent_md_decompressed tx_jentries[N_MAX_RAID_SLICE_LEN];
	u16 binje;
	u16 snake_size;

	/* rlba's (native) owner seg */
	u32 owner_si;
	/* rlba's parity segs bitmap (bit i --> segment i) */
	sgmnts_bmp_t rlba_parities_bm;

	/* TxBM from pivot-jmd after adding parities and
	   << so bit0 (right-most) is r1->segments[0] */
	struct state_bm txbm_topo;

	/* In progress */
	enum h_op op;
	u32 wip_bm;

	/* 1st (RW) seg we found that has jmd entry of our TxID */
	int jour_pivot;
	sgmnts_bmp_t jour_committed_bm;  // Bitmap: of location where journal is commited.
	/* True if found @jour_pivot && *ALL* TxRW segs have
	   jour-MD entry with TxID & Rlba matching to lockset */
	bool jour_committed;
	/* Transaction's absolute slba (=slice) offset from the start of pRAID
	   where all journals point to. Valid only if @jour_committed = true */
	u64 tx_slba;
	u16 tx_height; // Number of slices involved in transaction, can be extracted from pivot by its jentry chain length.
	u16 cur_slice;

	struct per_slice_info{ // These fields are reused by per-slice loop, and should be cleaned at the beginning of each iteration.
		sgmnts_bmp_t data_written_bm;	 // True if (@jour_committed = true) && at least one TxRW seg's MD point back to its journal
		bool data_committed_on_rw_seg; //omril: change such that if all DATAs were written, we dont call roll-fwd
		bool found_real_tx;	// HTR cannot exclude existance of TX (Either found a real one or cant prove it does not exist), so an operations on slice and binfo are needed.
		bool is_rollback;  // Are we rolling back the TX (regen w segs and turnon dbits on dead in the TX's txbm) can occur only when the journal commited but no data committed on readable seg.
		bool is_neverwritten_slice;
		u32 txid_for_regen;    // If we are in rollback mode we would like to assign to the blocks we write to max_txid in slice, else in roll-fwd use the locks txid.

		struct {	/* pre slice IO Tx Dbits - needed for post IO dbits calculations */
			bool is_initialized;					// Calc'ed once
			union nvmeibc_dbits_entry dbits;
		} pre_slice_dbits;

		struct t_dbits {	/* post IO Tx Dbits  */
			bool is_initialized;					// Calc'ed once
			struct nvmeibc_dbits_tx slice_after_turnon;		    // dbits of fixed slice after turnon. Written to metadata
			struct nvmeibc_dbits_tx slice_after_turnoff;		// dbits of fixed slice after turnon and turnoff. Written to metadata
			struct nvmeibc_dbits_tx binfo;		// dbits for blockset info after turnon (HTR doesn't turnoff by himself).
		} new_dbits;
	} cur_slice_info;

	struct {	// Todo: Sub class for no-writehole-fix
		/* vector for regen of blocks.
		   dynamic alloc for future support of HW acc offload */
		unsigned char **regen_vec;

		/* vector for edic prepartion of regen
		   up to n_parities segs */
		u32 **crc_vec;
	};

	/* status and debug */
	char sts_str[HTR_STS_STR_LEN];
	int n_ext_sm;
};

static int op_func_trap   (struct htr_ctx *h, int si)
{
	_NTh(warn_dp_ec_recov_hot_call_op_trap_func, h, "@SI (op=@OP) - illegal function call", si, h->op); //cant use si's cmd
	WARN_ON_ONCE(1);
	return -1;
}

/* @txbm right-most bit correspond to seg_slice_start of rlba. Identical to nvmeibcbdpec_bmp_ss2fs_with_pari()
   we add parities and shift left so right-most bit is r1->segments[0] */
static u32 txbm_2_topo(struct htr_ctx *h, roles_bmp_t txbm)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	u32 shift = h->owner_si;
	txbm |= nvmeibc_raid1_get_parities_bmp(r1);
	return rol32_width(txbm, shift, r1->replicas);
}

static bool filter_htr_event(struct nvmeib_public_kth_event *e)
{
	return ((e->type == htr_op_comp_event) || (e->type == nke_timeout));
}

struct htr_op_comp_event {				// HTR completion of send cmd
	struct nvmeib_public_kth_event e;
	struct nvmeibc_block_command *cmd;
};

static inline struct htr_op_comp_event * e_to_he(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct htr_op_comp_event, e);
}

static void free_he(struct nvmeib_public_kth_event *e)
{
	htr_kfree(e_to_he(e));
}

/* Important it is impossible to know if cmd is so->cmds[i] or h->seg_info[i].cmd */
static void add_htr_op_comp_event(struct htr_ctx *h, struct nvmeibc_block_command *cmd)
{
	struct htr_op_comp_event *e;
	int rv = 0;

	NFIN;
	if ((e = htr_kzalloc(sizeof(*e), GFP_ATOMIC))) {
		e->cmd = cmd;
		e->e.type = htr_op_comp_event; /* calls handle_htr_op_comp_event */
		e->e.id.ptr = h->htr_kth.ptr;
		e->e.free = free_he;
		e->e.no_recipient = NULL;
		if (unlikely(nvmeib_public_kth_add_event(&e->e))) {
			rv = -EINVAL;
			htr_kfree(e);
		}
	} else {
		rv = -ENOMEM;
	}
	e = NULL; 			// Note: 'e' does not exist anymore in every flow.
	if (unlikely(rv)) {
		_NE(t_01_ahoce, DMESG_PREFIX() ": htr-event sched failed, rv=@RV, IO will stuck: @NAME", rv, nvmeib_public_kth_get_name());
	}
	NFOUT;
}

static inline void handle_htr_cmd_comp_event(struct htr_ctx *h,
									 struct htr_op_comp_event *he)
{
	struct nvmeibc_block_command *cmd = he->cmd;
	struct seg_info *s = container_of(cmd, struct seg_info, cmd);
	int si = s->si;
	h_op_comp_f comp_f;
	int rv;
	NFIN;

	_NTh(trace_dp_ec_recov_hot_handle_htr_cmd_comp_event, h, "Handle comp event si @SI (op=@OP)", si, h->op);

	if (h->op <= h_op_invalid || h->op >= h_op_last) {
		_NTh(trace_1_dp_ec_recov_hot_handle_htr_cmd_comp_event, h, "No op inprogress (@OP)", h->op);
		goto out;
	}
	if (si < 0 || si > h->n_segs) {
		_NTh(trace_2_dp_ec_recov_hot_handle_htr_cmd_comp_event, h, "Invalid si @SI", si);
		goto out;
	}
	if (s->op_sts != H_SEG_OP_STS_INPROGRESS ||
		!(h->wip_bm & (1 << si))) {
		_NTh(trace_3_dp_ec_recov_hot_handle_htr_cmd_comp_event, h, "seg-op not INPROGRESS (@OP_STS) for si @SI",
			s->op_sts, si);
		goto out;
	}
	if (!(comp_f = h_ops_funcs[h->op].comp_f)) {
		_NTh(trace_4_dp_ec_recov_hot_handle_htr_cmd_comp_event, h, "No comp-func for op @OP", h->op);
		goto out;
	}

	//omril: what if this is not the event TYPE that we are expecting???
	//E.g. we issued read nad got write comp ?!?!?

	rv = comp_f(h, si);
	h->seg_info[si].op_sts = !rv ? H_SEG_OP_STS_OK : H_SEG_OP_STS_ERR;
	h->wip_bm &= ~(1 << si);

out:
	NFOUT;
}

static void handle_htr_op_comp_event(struct htr_ctx *h,
									 struct htr_op_comp_event *he)
{
	if (likely(h->op != h_op_extern_sm))
		handle_htr_cmd_comp_event(h, he);
	else {
		_NTh(trace_dp_ec_recov_hot_handle_htr_op_comp_event, h, "Handle comp event from extern-sm");
		h->wip_bm = 0;
	}
}


int debug_bail_op_wait = 0; /* Future use */

static int wait_events_timeout(struct nvmeib_public_kth_event **e, long timeout)
{
	int rv = nvmeib_public_kth_wait_events_timeout(filter_htr_event, false, e, timeout);
	if (rv == 0) {
		if ((*e)->type == nke_timeout) {
			nvmeib_public_kth_event_free(*e);
			*e = NULL;
			rv = 0;
		}
		else if ((*e)->type == htr_op_comp_event)
			rv = 1;
		else {
			_NE(error_dp_ec_recov_hot_wait_events_timeout, DMESG_PREFIX() ": Unexpcted event @NVMEIB_KTH_EVENT_TO_STR arrived", nvmeib_kth_event_to_str((*e)->type));
			nvmeib_public_kth_event_free(*e);
			*e = NULL;
			rv = -1;
		}
	} else
		rv = -1;
	return rv;
}

#define H_WAIT_TIMEOUT (5*HZ)
static int wait_all_wip(struct htr_ctx *h)
{
	struct nvmeib_public_kth_event *e;
	int n_init = hweight32(h->wip_bm), n_timeouts = 0;
	int rv;
	NFIN;

	while (h->wip_bm) {
		e = NULL;
		if ((rv = wait_events_timeout(&e, H_WAIT_TIMEOUT)) <= 0) {
			_NTh(error_dp_ec_recov_hot_wait_all_wip, h, "Failed to wait for event '@CALLBACK' (rv=@RV), remaining [@BITMAP]", h->op < h_op_last ? h_ops_funcs[h->op].f : NULL, rv, h->wip_bm);
			//omril: only if rv=0 it's timeout
			//otherwise, can we bail w/o receving the comp or RDMA?!
			n_timeouts++;
		}
		else {
			if (e->type == nke_timeout) {
				_NTh(error_1_dp_ec_recov_hot_wait_all_wip, h, "Failed to wait for event '@CALLBACK' (rv=@RV), remaining [@BITMAP]", h_ops_funcs[h->op].f, rv, h->wip_bm);
				//omril: only if rv=0 it's timeout
				//otherwise, can we bail w/o receving the comp or RDMA?!
				n_timeouts++;
			} else {
				handle_htr_op_comp_event(h, e_to_he(e));
			}
			nvmeib_public_kth_event_free(e);

		}

		if (n_timeouts == n_init && debug_bail_op_wait) {
			_NTh(error_2_dp_ec_recov_hot_wait_all_wip, h, "Timeout, remaining [@BITMAP] - bail op-wait", h->wip_bm);
			h->wip_bm = 0;
			break;
		}
	}

	NFOUT;
	return (h->wip_bm == 0) ? 0 : -1;
}


static int call_op_func(struct htr_ctx *h, int si, h_op_f func)
{
	int rv = -1;
	NFIN;

	if (h->seg_info[si].op_sts != H_SEG_OP_STS_IDLE ||
		h->wip_bm & (1 << si)) {
		_NTh(warn_dp_ec_recov_hot_call_op_func, h, "seg-op not IDLE (@OP_STS) for si @SI", h->seg_info[si].op_sts, si); //cant use si's cmd
		WARN_ON_ONCE(1);
		goto out;
	}

	memset(h->seg_info[si].cmd.iocmd, 0, sizeof(*h->seg_info[si].cmd.iocmd));
	memset(h->seg_info[si].cmd.gen_cmd, 0, sizeof(*h->seg_info[si].cmd.gen_cmd));

	/* init op to 'in-progress' */
	h->seg_info[si].cmd.iocmd->comp.comp_code = HTR_INVALID_COMP_CODE;
	h->seg_info[si].cmd.gen_cmd->comp_code = HTR_INVALID_COMP_CODE;
	h->seg_info[si].op_sts = H_SEG_OP_STS_INPROGRESS;

	rv = func(h, si);
	if (rv != -EINPROGRESS)
		h->seg_info[si].op_sts = !rv ? H_SEG_OP_STS_OK : H_SEG_OP_STS_ERR;
out:
	NFOUT;
	return rv;
}

enum h_on_err_mode {
	H_ON_ERR_BREAK = 0x2A,
	H_ON_ERR_CONT
};
/*
 * Calls @func for every segment corresponding to set bit in @bitmap.
 *
 * @func expected behavior:
 *  	(*) if submits asynchronous op, return -EINPROGRESS
 *
 * Return a bitmap of rv corresponding to @bitmap where a rv is 0
 * iff the call to @func for the corresponding segment was issued
 * and completed successfully.
 */
static u32 _call_for_bitmap(struct htr_ctx *h, const ulong bitmap, enum h_op op, enum h_on_err_mode on_err)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	int si, n_all = hweight32(bitmap), r1_size = r1->replicas, f_rv;
	ulong called_bm = 0, rv_bm = bitmap; /* init all rv to err */
	u32 wip_bm = 0;
	h_op_f func;
	NFIN;

	/* checks */
	if (op <= h_op_invalid || op >= h_op_last) {
		_NTh(warn_dp_ec_recov_hot_call_for_bitmap, h, "Invalid op @OP", op);
		goto out;
	}
	if (bitmap & ~((1 << r1_size) - 1)) {
		_NTh(warn_1_dp_ec_recov_hot_call_for_bitmap, h, "Invalid bitmap @BITMAP", (u32)bitmap);
		goto out;
	}
	if (h->op || h->wip_bm) {
		_NTh(warn_2_dp_ec_recov_hot_call_for_bitmap, h, "Op in progress (@OP ('@CALLBACK'), wip:[@BITMAP]), bail", h->op, h_ops_funcs[h->op].f, h->wip_bm);
		WARN_ON_ONCE(1);
		goto out;
	}
	if (unlikely(h->so->o->topo->phased_out)) {
		_NTh(trace_6_dp_ec_recov_hot_call_for_bitmap, h, "topo phased out");
		goto out;
	}

	/* init */
	h->op = op;
	func = h_ops_funcs[op].f;
	_NTh(trace_dp_ec_recov_hot_call_for_bitmap, h, "Call '@FUNC' for segs-bitmap=[@BITMAP] (from '@FUNC')", func, (u32)bitmap, __builtin_return_address(0));

	/* call per relevant seg */
	for_each_set_bit(si, &bitmap, r1_size) {
		f_rv = call_op_func(h, si, func);
		called_bm |= (1 << si);
		if (f_rv == -EINPROGRESS)
			wip_bm |= (1 << si);
		else if (f_rv != 0) {
			_NTh(trace_1_dp_ec_recov_hot_call_for_bitmap, h, "Failed '@FUNC' si @SI (@F_RV)",
				func, si, f_rv);
			if (on_err == H_ON_ERR_BREAK)
				break;
		}
	}

	/* wait for successfull in-progress calls */
	if (wip_bm) {
		h->wip_bm = wip_bm;
		wait_all_wip(h);
		NBUG_ON_MSG(error_dp_ec_recov_hot_call_for_bitmap, h->wip_bm != 0, "OOPS, not all '@FUNC' done (@INT, @INT, @INT), comps may corrupt mem, boom!", func, hweight32(wip_bm), hweight32(h->wip_bm), n_all);
	}
	else _NTh(trace_2_dp_ec_recov_hot_call_for_bitmap, h, "No func-calls to wait for\n");

	/* collect status from segs we've called and reset op-sts for those completed normally */
	for_each_set_bit(si, &called_bm, r1_size) {
		if (h->seg_info[si].op_sts == H_SEG_OP_STS_OK)
			rv_bm &= ~(1 << si);
		else if (h->seg_info[si].op_sts == H_SEG_OP_STS_ERR)
			_NTh(trace_3_dp_ec_recov_hot_call_for_bitmap, h, "si @SI comp err", si);
		else {
			_NTh(warn_3_dp_ec_recov_hot_call_for_bitmap, h, "si @SI unexp op-sts (@OP_STS)", si, h->seg_info[si].op_sts);
			WARN_ON_ONCE(1);
			continue;
		}
		h->seg_info[si].op_sts = H_SEG_OP_STS_IDLE;
	}
	h->op = h_op_invalid;

	if (rv_bm) {
		int i = 0;
		_NTh(trace_4_dp_ec_recov_hot_call_for_bitmap, h, "Failed '@FUNC'(op=@HOT_RECOVERY_OP) for @INT[@BITMAP] of @INT[@BITMAP] segs:", func, op, hweight32(rv_bm), (u32)rv_bm, n_all, bitmap);
		for_each_set_bit(si, &rv_bm, r1_size) {
			_NTh(trace_5_dp_ec_recov_hot_call_for_bitmap, h, "[@INDEX] si=@SI, disk=@DISK_NAME seg=@SEGMENT_UUID", i++, si,
				h->params.raid1->segments[si].disk->full_name,
				h->params.raid1->segments[si].uuid);
		}
	}

out:
	NFOUT;
	return (u32)rv_bm;
}

#define call_for_bitmap(_h_, _bitmap_, _func_, _on_err_) \
	_call_for_bitmap(_h_, _bitmap_, h_op_##_func_, _on_err_)

static void init_block_cmd(struct nvmeibc_block_command *cmd,
						   struct operation *o, struct nvmeibc_disk_segment *ds)
{	//Daniel: Todo, init all cmds via dp_ec_sync_prepare_op()/dp_sync_cmd_fill() NOTE -> CMAP.DEG is not set!!! when prep-runs
	struct nvmeibc_disk_io_command *iocmd = cmd->iocmd;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd->gen_cmd;

	BUG_ON(iocmd->comp.comp_code != (int)HTR_INVALID_COMP_CODE);
	BUG_ON(gen_cmd->comp_code != (int)HTR_INVALID_COMP_CODE);

	memset(cmd    , 0, sizeof(*cmd));

	cmd->iocmd = iocmd;
	cmd->gen_cmd = gen_cmd;
	iocmd->disk_cmd.owner = cmd;
	iocmd->reqs1.cpu_mask_info = &o->cpu_mask_info;
	gen_cmd->disk_cmd.owner = iocmd;
	gen_cmd->cpu_mask_info = o->cpu_mask_info;
	iocmd->orig = NVMEIBC_DISK_IO_CMD_ORIG_RECOV;

	cmd->cmdarr = cmd;
	cmd->ncmds = 1;
	nvmeibc_atomic_set(&cmd->n_uncompleted_cmds, 1);
	DEBUG_TRANSFERS_init_cb_counter(cmd);
	cmd->o = o; /* used by comp-handler to extract htr scheduler */
	cmd->ds = ds;
	cmd->iocmd->comp.cmd = cmd;
}


/* Prepare and send iocmd similar to dp_cmds_req_fill and dp_sync_cmd_fill.
 * We use @so (or @o) just so not to add more if statements in
 * nvmeibc-block_completion()
 * @ disk_address: block-dev LBA address
 */
static int submit_iocmd(struct htr_ctx *h, int si, enum nvmeib_block_io_op io_op,
						u64 disk_address, void *buf, size_t len, void *md,
						enum e_cmds_stage stage)
{
	struct nvmeibc_block_command *cmd = &h->seg_info[si].cmd;
	struct nvmeib_data_buffer *ndb = &h->seg_info[si].ndb;
	struct nvmeibc_disk_segment *ds = &h->params.raid1->segments[si];
	int rv = -1;
	NFIN;

	if (len != NVMEIBC_SECTOR_SIZE) {
		_NE(error_dp_ec_recov_hot_submit_iocmd, DMESG_PREFIX() ": Support only 4K block disk-iocmd");
		goto out;
	}
	if (io_op != NVMEIB_BLOCK_IO_OP_READ && !nvmeib_block_io_op_is_write(io_op)) {
		_NE(error_1_dp_ec_recov_hot_submit_iocmd, DMESG_PREFIX() ": Invalid io-op @BLOCK_IO_OP", io_op);
		goto out;
	}

	init_block_cmd(cmd, h->so->o, ds);
	/* init ndb */
	sg_set_buf(ndb->table.sgl, buf, len);
	ndb->length = len;
	/* init iocmd's io-req */
	cmd->iocmd->reqs1.op = io_op;
	cmd->iocmd->reqs1.disk_address = disk_address;
	cmd->first_rlba = nvmeibc_datapath_dlba_to_rlba(&h->so->o->nd->dp, h->params.raid1, si, disk_address);
	cmd->iocmd->reqs1.ndb = ndb;			// This is HTR hack due to zeroing the cmd, should in fact be done only once
	cmd->iocmd->reqs1.md = md;
	/* init iocmd */
	cmd->iocmd->reqs = &cmd->iocmd->reqs1;
	cmd->my_stage = stage;
	cmd->is_parity = !(!__is_seg_parity(si, h));

#if HTR_DEBUG
	if (io_op == NVMEIB_BLOCK_IO_OP_READ) {
		memset(buf, 0xcc, len);
		memset(md, 0xcc, nvmeibc_sgmnt_sw_md_size(ds));
	}
#endif
	rv = dp_cmds_execute_cmd(cmd, 0);
out:
	NFOUT;
	return rv;
}

static inline u64 dblk_disk_address(struct htr_ctx *h, int si)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	u64 dlba = r1->segments[si].first_lba + h->tx_slba + h->cur_slice;
	_NTh(trace_dp_ec_recov_hot_dblk_disk_address, h, "si @SI, @DLBA", si, dlba);
	return dlba;
}

static inline u64 dblk_rlba_address(struct htr_ctx *h, const int si)
{
	u64 rlba = nvmeibc_datapath_dlba_to_rlba(&h->so->o->nd->dp, h->params.raid1, si, dblk_disk_address(h, si));
	_NTh(trace_dp_ec_recov_hot_dblk_rlba_address, h, "si @SI, @RLBA", si, rlba);
	return rlba;
}

#define owner_rlba_address(h) (dblk_rlba_address((h), (h)->owner_si))

/*
 * Usage: Read MD of TxRW segs.
 *        Data is also read and may be used for regen of TxW segs
 */
static inline int read_dblk(struct htr_ctx *h, int si)
{
	int rv = -1;
	NFIN;

	if (has_rw_parity(h) && (!h->jour_committed)) {
		_NTh(trace_dp_ec_recov_hot_read_dblk, h, "Odd, reading data but jour "
			   "NOT committed, si @SI", si);
		goto out;
	}
	if (h->tx_slba == HTR_INVALID_LBA) {
		_NTh(trace_1_dp_ec_recov_hot_read_dblk, h, "Invalid Tx-SLBA");
		goto out;
	}
	if (!!h->seg_info[si].dblk.valid[h->cur_slice]) {
		_NTh(trace_2_dp_ec_recov_hot_read_dblk, h, "Already read dblk si @SI, unexpected!", si);
		goto out;
	}
	rv = (submit_iocmd(h, si, NVMEIB_BLOCK_IO_OP_READ,
					   dblk_disk_address(h, si),
					   h->seg_info[si].dblk.data_addr[h->cur_slice], NVMEIBC_SECTOR_SIZE,
					   (u8 *)(&h->seg_info[si].dblk.md[h->cur_slice]->dmd),
					   E_CMDS_STAGE_READ_PRE_DATA) == 0) ? -EINPROGRESS : -1;
out:
	NFOUT;
	return rv;
}

/*
 * Usage:
 *  (1) Roll-fwd RW segs
 *  (2) Write regenerated W segs
 *  (3) Write parity segs' MD.Dbits
 */
static inline int write_dblk(struct htr_ctx *h, int si)
{
	int rv = -1;
	NFIN;

	if (has_rw_parity(h) && (!h->jour_committed)) {
		_NTh(trace_dp_ec_recov_hot_write_dblk, h, "Odd, writing data but jour "
			   "NOT committed, si @SI", si);
		goto out;
	}
	if (h->tx_slba == HTR_INVALID_LBA) {
		_NTh(trace_1_dp_ec_recov_hot_write_dblk, h, "Invalid Tx-SLBA");
		goto out;
	}
	if (!h->seg_info[si].dblk.valid[h->cur_slice]) {
		_NTh(trace_2_dp_ec_recov_hot_write_dblk, h, "Invalid dblk si @SI", si);
		goto out;
	}
	rv = (submit_iocmd(h, si, NVMEIB_BLOCK_IO_OP_WRITE,
					   dblk_disk_address(h, si),
					   h->seg_info[si].dblk.data_addr[h->cur_slice], NVMEIBC_SECTOR_SIZE,
					   (u8 *)(&h->seg_info[si].dblk.md[h->cur_slice]->dmd),
					   E_CMDS_STAGE_DO_IO_AND_PAR) == 0) ? -EINPROGRESS : -1;
out:
	NFOUT;
	return rv;
}

// Returns the jblock index from the beginning of the journal area in units of blocks.
/* Used as D2J prior to version 2.7 */
static inline u64 get_jidx_rblk(struct htr_ctx *h, int si)
{
	struct jent_md_decompressed *jent = &h->tx_jentries[si];
	if (!jent->is_valid)
		return HTR_INVALID_LBA;
	return jent->jent_idx * h->binje + h->cur_slice - jent->offset;
}

/* Return the index of the journal entry
 * Used as D2J for version 2.7 going forward */
static inline u64 get_jidx_idx(struct htr_ctx *h, int si)
{
	struct jent_md_decompressed *jent = &h->tx_jentries[si];
	if (!jent->is_valid)
		return NVMEIB_EC_INVALID_JOURNAL_ENTRY;
	return jent->jent_idx;
}

static inline u64 jblk_disk_address(struct htr_ctx *h, int si)
{
	return h->seg_info[si].clj.desc.rng_slba + get_jidx_rblk(h, si);
}

static inline int read_jblk(struct htr_ctx *h, int si)
{
	int rv = -1;
	NFIN;

	if (!h->jour_committed) {
		_NTh(trace_dp_ec_recov_hot_read_jblk, h, "Odd, reading jour but jour NOT committed, si @SI", si);
		goto out;
	}
	if (h->seg_info[si].dblk.valid[h->cur_slice]) {
		_NTh(trace_1_dp_ec_recov_hot_read_jblk, h, "Reading jblk while dblk valid, si @SI, unexpected!", si);
		goto out;
	}
	if (!h->tx_jentries[si].is_valid) {
		_NTh(trace_2_dp_ec_recov_hot_read_jblk, h, "Invalid jentry si @SI", si);
		goto out;
	}

	rv = (submit_iocmd(h, si, NVMEIB_BLOCK_IO_OP_READ,
					   jblk_disk_address(h, si),
					   h->seg_info[si].dblk.data_addr[h->cur_slice], NVMEIBC_SECTOR_SIZE,
					   (u8 *)(&h->seg_info[si].dblk.md[h->cur_slice]->jmd),
					   E_CMDS_STAGE_READ_JOURNAL) == 0) ? -EINPROGRESS : -1;
out:
	NFOUT;
	return rv;
}

static inline int comp_check(struct htr_ctx *h, int comp_code)
{
	int rv = 0;
	NFIN;

	if ((unsigned)comp_code == HTR_INVALID_COMP_CODE) {
		WARN(true, "comp-code not assigned by lower layer");
		rv = -1;
	}
	else if (comp_code != 0) {
		_NTh(error_dp_ec_recov_hot_comp_check, h, "op completed with error @COMP_CODE, skip comp-func", comp_code);
		rv = -1;
	}

	NFOUT;
	return rv;
}

#define iocmd_comp_check(_h_, _si_) ({ 										\
	int _rv_ = comp_check(_h_, _h_->seg_info[_si_].cmd.iocmd->comp.comp_code);	\
	_rv_; 																	\
})
#define gen_cmd_comp_check(_h_, _si_) ({ 									\
	int _rv_ = comp_check(_h_, _h_->seg_info[_si_].cmd.gen_cmd->comp_code);	    \
	_rv_;																	\
})

#define set_if_zero_dblk_valid(name, _h_, _si_) ({							\
	int _rv_ = 0;															\
	if (!_h_->seg_info[_si_].dblk.valid[(_h_)->cur_slice]) { 									\
		 _h_->seg_info[_si_].dblk.valid[(_h_)->cur_slice] = true; 							\
	} else {																\
		_NTh(name, _h_, "si @SI, dblk already valid!", (int)(_si_));		\
		_rv_ = -1; 															\
	}																		\
	_rv_;																	\
})

#define Nset_if_zero_dblk_valid(name, _h_, _si_) ({								\
	int _rv_ = 0;															\
	if (!_h_->seg_info[_si_].dblk.valid[(_h_)->cur_slice]) { 									\
		 _h_->seg_info[_si_].dblk.valid[(_h_)->cur_slice] = true; 							\
	} else {																\
		_NTh(name, _h_, "si @SI, dblk already valid!\n", (int)(_si_));						\
		_rv_ = -1; 															\
	}																		\
	_rv_;																	\
})

// Only called for journal edic calculation since we do not have it in MD
static int jblk_edic_calc(struct htr_ctx *h, int si)
{
	u64 rlba = dblk_rlba_address(h, si);
	u32 crc = nvmeibc_calculate_edic_from_data_and_rlba(rlba, (u8*)h->seg_info[si].dblk.data_addr[h->cur_slice],
														h->so->o->nd->dp.enable_di_debug_mode);

	NFIN;

#if HTR_DEBUG
	if (!h->seg_info[si].dblk.valid[h->cur_slice]) {
		_NTh(trace_dp_ec_recov_hot_jblk_edic_calc, h, "Invalid dblk si @SI", si);
		return -1;
	}
	if (!h->seg_info[si].dblk.edic_sts) {
		_NTh(trace_1_dp_ec_recov_hot_jblk_edic_calc, h, "EDIC already valid si @SI", si);
		return -1;
	}
#endif
	h->seg_info[si].dblk.edic_sts = 0;
	h->seg_info[si].dblk.edic_res = crc;

	NFOUT;
	return 0;
}

static int read_xblk_comp(struct htr_ctx *h, int si)
{
	int rv;

	if ((rv = iocmd_comp_check(h, si)))
		goto out;

	if ((rv = Nset_if_zero_dblk_valid(error_dp_ec_recov_hot_read_xblk_comp, h, si)))
		goto out;

out:
	return rv;
}

static int read_dblk_comp(struct htr_ctx *h, int si)
{
	int rv;

	if (!(rv = read_xblk_comp(h, si))) { // Store edic from MD of read command
		h->seg_info[si].dblk.edic_res = (h->seg_info[si].cmd.is_parity) ?
			h->seg_info[si].dblk.md[h->cur_slice]->dmd.P.edic : h->seg_info[si].dblk.md[h->cur_slice]->dmd.D.edic;
		h->seg_info[si].dblk.edic_sts = 0; // Validate EDIC
	}
	return rv;
}

static int write_dblk_comp(struct htr_ctx *h, int si)
{
	return iocmd_comp_check(h, si);
}

static int read_jblk_comp (struct htr_ctx *h, int si)
{
	int rv;
	if (!(rv = read_xblk_comp(h, si))) {
		rv = jblk_edic_calc(h, si);
	}
	return rv;
}

static void __nvmeibc_read_jmdc_request_pet_describe(struct htr_ctx *h, int si, uuid_be *client_uuid)
{
	struct nvmeibc_block_command *cmd = &h->seg_info[si].cmd;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd->gen_cmd;

	NVMEIBC_IO_PET_MSG_NORM(&h->so->o->journal,
		"read_jmdc.request(sgmnt=%hhu, opcode=%hhu<enum nvmeib_gen_cmd_op>, client_uuid_first_8b=0x%llx)",
		numeric_downcast(u8, si), numeric_downcast(u8, gen_cmd->opcode), ((union nvmeib_uuid*)client_uuid)->ll[0]);
}

static void __nvmeibc_read_jmdc_response_pet_describe(struct htr_ctx *h, int si, int rv, struct cl_jour *clj, u32 binje)
{
	unsigned i;

	if (rv == 0) {
		NVMEIBC_IO_PET_MSG_NORM(&h->so->o->journal, "read_jmdc.response(si=%hhu) = %d", numeric_downcast(u8, si), rv);
		if (nvmeib_pet_journal_is_verbose(&h->so->o->journal)) {
			for (i = 0; i < clj->desc.n_ents; i++) {
				NVMEIBC_IO_PET_MSG_NORM(&h->so->o->journal,
					"client_jnl(idx=%hhu ent_gen_id=%hhu jmdc=0x%llx<union jblock_md>)",
					numeric_downcast(u8, i), clj->ent_md[i].ent_gen_id, clj->jmdc[i * binje].raw);
			}
		}
	} else {
		NVMEIBC_IO_PET_MSG_WARN(&h->so->o->journal, "read_jmdc.response(si=%hhu) = %d", numeric_downcast(u8, si), rv);
	}
}

static void __nvmeibc_free_jrnl_ents_request_pet_describe(struct htr_ctx *h, int si, struct nvmeibc_disk_free_jrnl_ents_comp *free_ents_comp)
{
	NVMEIBC_IO_PET_MSG_NORM(
		&h->so->o->journal,
		"free_jrnl_ents.request(sgmnt_idx=%hhu, blkset_slba=%llu, blkset_num=%llu, pass2toma=%hhu, lock_id=0x%llx<union nvmeib_lock_id>)",
		numeric_downcast(u8, si), free_ents_comp->blkset_slba, free_ents_comp->blkset_num,
		free_ents_comp->pass2toma, free_ents_comp->lock_ent);
}

static void __nvmeibc_send_recovered_response_pet_describe(struct htr_ctx *h, int si, int rv)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd = h->seg_info[si].cmd.gen_cmd;

	if (h->tx_jentries[si].is_valid) {
		NVMEIBC_IO_PET_MSG_NORM(
			&h->so->o->journal,
			"free_jrnl_ents.response(sgmnt_idx=%hhu, status=%hhu) = %d",
			numeric_downcast(u8, si), gen_cmd->rsp.br.status, rv);
	} else {
		NVMEIBC_IO_PET_MSG_NORM(
			&h->so->o->journal,
			"send_recovered_blkset.response(sgmnt_idx=%hhu, status=%hhu) = %d",
			numeric_downcast(u8, si), gen_cmd->rsp.br.status, rv);
	}
}

static int read_jmdc(struct htr_ctx *h, int si)
{
	struct nvmeibc_block_command *cmd = &h->seg_info[si].cmd;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd->gen_cmd;
	int rv = -1;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	NFIN;

	if (h->seg_info[si].clj.valid) {
		_NTh(trace_dp_ec_recov_hot_read_jmdc, h, "Already read jmdc si @SI, unexpected!", si);
		goto out;
	}

	init_block_cmd(cmd, h->so->o, &h->params.raid1->segments[si]);
	/* init gen-cmd common part */
	gen_cmd->jiffies_start = jiffies;
	gen_cmd->timeout = HTR_CMD_TIMEOUT;
	gen_cmd->ctx = h->htr_kth.ptr;
	/* init gen-cmd specific part */
	gen_cmd->opcode = NVMEIB_GEN_OP_GET_UUID_JOUR;
	gen_cmd->param.uj.client_uuid = h->params.recoveree_cuuid;
	/* FUTURE PROOF: Get N from TOMA when recovering clients that switch N on the fly */
	gen_cmd->param.uj.binje = NVMEIB_EC_INVALID_JOURNAL_BINJE;
	BUILD_BUG_ON(ARRAY_SIZE(gen_cmd->param.uj.sgmnt_uuid) != ARRAY_SIZE(h->params.raid1->segments[si].uuid));
	memcpy(gen_cmd->param.uj.sgmnt_uuid, h->params.raid1->segments[si].uuid, ARRAY_MEM_SIZE(gen_cmd->param.uj.sgmnt_uuid));
	nvmeib_buffer_init_one(&gen_cmd->param.uj.jmdc_dest.local, h->seg_info[si].clj.jmdc, h->seg_info[si].clj.jmdc_size);
	nvmeib_buffer_init_one(&gen_cmd->param.uj.ent_md_dest.local, h->seg_info[si].clj.ent_md, h->seg_info[si].clj.ent_md_sz);

	gen_cmd->n_data_sink = 2;
	gen_cmd->data_sink[0] = &gen_cmd->param.uj.jmdc_dest;
	gen_cmd->data_sink[1] = &gen_cmd->param.uj.ent_md_dest;

	__nvmeibc_read_jmdc_request_pet_describe(h, si, &gen_cmd->param.uj.client_uuid);
	rv = (icore_ops->execute_gen(icore_ops, cmd->ds->disk, gen_cmd) == 0) ? -EINPROGRESS : -1;

out:
	NFOUT;
	return rv;
}

static inline u32 __get_n_jblks_in_jentry_from_disk_cmd(const struct nvmeib_disk_client_journal_extended *desc) {
	const u32 rv = desc->binje;
	WARN(rv==0 || rv > NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY, "nvmeibc bug!, invalid binje=%u\n", rv);
	return rv;
}

static int read_jmdc_comp(struct htr_ctx *h, int si)
{
	struct nvmeibc_block_command *cmd = &h->seg_info[si].cmd;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd->gen_cmd;
	struct cl_jour *clj = &h->seg_info[si].clj;
	struct nvmeibc_disk *disk = h->params.raid1->segments[si].disk;
	int rv = -1;
	unsigned i;
	u32 binje = HTR_INVALID_N_SLICE_JOUR;
	NFIN;

	if (gen_cmd_comp_check(h, si) < 0)
		goto out;

	clj->desc = gen_cmd->rsp.uj.jour;
	binje = __get_n_jblks_in_jentry_from_disk_cmd(&clj->desc);

	for (i = 0; i < clj->desc.n_ents; i++) {
		_NDh(trace_5_dp_ec_recov_hot_read_jmdc_comp, h,
			"[@JRNL_RNG_ENT_IDX/@JRNL_ENT_GEN_ID]: @JMDC_ENT",
			i, clj->ent_md[i].ent_gen_id, clj->jmdc[i * binje].raw);
	}

	if (clj->desc.rng_id == NVMEIB_EC_INVALID_JOURNAL_RANGE) {
		_NTh(trace_dp_ec_recov_hot_read_jmdc_comp, h, "No jour found at target (uuid prob had empty jmdc)");
		rv = 0;
		goto out;
	}

	/* jmdc was RDMA'ed and rsp's uuid was validated by nrch receiver */
	if ((clj->desc.rng_nlba * sizeof(*(clj->jmdc))) > clj->jmdc_size) {
		_NTh(trace_1_dp_ec_recov_hot_read_jmdc_comp, h, "OOPS, Target probably overflow jmdc sink buffer?! (#jour-entries=@ENTRIES)", clj->desc.rng_nlba);
		BUG();
		goto out;
	}
	if (clj->desc.rng_blk > disk->jour.max_rng_blk) {
		_NTh(trace_2_dp_ec_recov_hot_read_jmdc_comp, h, "Version Incompatiblity, potential rdma overflow?!, (#jour-entries=@ENTRIES)", clj->desc.rng_nlba);
		goto out;
	}

	/* check binje validity and consistency over all JMDCs */
	if (binje == HTR_INVALID_N_SLICE_JOUR) {
		_NTh(trace_3_dp_ec_recov_hot_read_jmdc_comp, h, "Received invalid binje for jmdc(@BINJE).", binje);
		goto out;
	}
	if (h->binje == HTR_INVALID_N_SLICE_JOUR)
		h->binje = binje;
	else if (h->binje != binje) {
		_NTh(trace_4_dp_ec_recov_hot_read_jmdc_comp, h, "Inconsistent binje: jmdc(@BINJE) != h(@BINJE)", binje, h->binje);
		goto out;
	}

	/* jmdc is valid  */
	h->tx_jentries[si].is_valid = false;
	h->seg_info[si].clj.valid = true;
	rv = 0;


out:
	__nvmeibc_read_jmdc_response_pet_describe(h, si, rv, clj, binje);
	NFOUT;
	return rv;
}

/* Translate j2d to slba and first-rlba. Note that HTR's rlba is the first block in the lockset.
 * Result:
 * @slba - slice offset from the start of pRAID
 * @rlba - first block in lockset j2d (slba) belongs to. */
static inline void j2d_translate(struct htr_ctx *h, struct nvmeibc_raid1 *r1, int si, u64 j2d, u64 *slba, u64 *rlba)
{
	*slba = j2d - r1->segments[si].first_lba;
	*rlba = (*slba & LOCKSET_4KS_MASK) * (u64)r1->slice_size;
	_NTh(trace_dp_ec_recov_hot_j2d_translate, h, "j2d=@J2D, first_@DLBA --> @SLBA, @RLBA", j2d, r1->segments[si].first_lba, *slba, *rlba);
}

/* Returns 0 iff found *single* matching jentry_md */
static int scan_jmdc(struct htr_ctx *h, int si, bool is_pivot)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	union jblock_md *jmdc;
	struct jentry_md cand_jent;
	u64 slba, rlba, cand_slba = 0;
	u32 i, n_found, binje = h->binje;
	u16 cand = HTR_INVALID_JMD_ID;
	int rv = -EINVAL;
	bool is_hot_jgc = h->params.is_hot_jgc;
	NFIN;

	/* checks */
	if (h->seg_info[si].clj.desc.rng_id == NVMEIB_EC_INVALID_JOURNAL_RANGE) {
		_NTh(t_01_ehsj, h, "No jmdc for uuid, si=@SI; not scanning jmdc's", si);
		HTR_STATS_INC(n_uuid_no_jour);
		rv = 0;
		goto out;
	}

	if (!nvmeibc_is_readable(&r1->segments[si])) {
		_NDh(t_02_ehsj, h, "Scanning non RW (@INT) si @SI in JGC HTR", si, r1->segments[si].toma_acm);
		if (unlikely(!h->params.is_hot_jgc)) { // Only allowed in hot JGC
			_NE_to_user(t_04_ehsj, DMESG_PD_PREFIX("@HTR_PARAM"), "Unexpected internal error, crashing the operating system to prevent data corruption, contact Excelero support. Error code: 1015. Internal info {@SI @INT}.", h->name, si, r1->segments[si].toma_acm);
			BUG();
		}
	}

	if (binje == HTR_INVALID_N_SLICE_JOUR) {
		_NTh(trace_10_dp_ec_recov_hot_scan_jmdc, h, "Invalid binje (@BINJE), cannot scan jmdc", binje);
		goto out;
	}

	if (h->tx_jentries[si].is_valid) {
		_NTh(trace_1_dp_ec_recov_hot_scan_jmdc, h, "si @SI already has jent_idx (@JENT_IDX)",si, h->tx_jentries[si].jent_idx);
		goto out;

	}
	jmdc = h->seg_info[si].clj.jmdc;

	/* scan jmdc */
	n_found = 0;
	for_each_set_bit(i, h->seg_info[si].clj.desc.dirty_ents_bitmap, h->seg_info[si].clj.desc.n_ents)
	{
		union jblock_md *jentry_first = &jmdc[i * binje];
		const u64 j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(jentry_first);
		cand_jent.md_arr = jentry_first;

		if (jentry_first->tx_id != h->params.recoveree_txid) {
			_NDh(trace_2_dp_ec_recov_hot_scan_jmdc, h, "si @SI (i=@INDEX): diff TxID (@TXID,@RECOVEREE_TXID)", si, i, jentry_first->tx_id, h->params.recoveree_txid);
			continue;
		}

		j2d_translate(h, r1, si, j2d, &slba, &rlba);
		if (rlba != h->params.rlba) {
			_NDh(trace_3_dp_ec_recov_hot_scan_jmdc, h, "si @SI (i=@INDEX): diff Rlab (@RLBA(@J2D),param_@RLBA))", si, i, rlba, j2d, h->params.rlba);
			continue;
		}

		// In hot_jgc we are not looking for journal candidate but to clean all journal which point to the blockset
		if (is_hot_jgc) {
			h->tx_slba = slba;
		} else {
			if (!nvmeib_jentry_md_is_valid(&cand_jent, binje)) {
				_NDh(trace_4_dp_ec_recov_hot_scan_jmdc, h, "si @SI (i=@INDEX): jentry chain is invalid", si, i);
				continue;
			}
			if (is_pivot) {
			   WARN(h->tx_slba != HTR_INVALID_LBA, "nvmeibc bug\n"); // Sanity
			   h->tx_slba = slba;
			} else if (h->tx_slba == HTR_INVALID_LBA) { // Didn't find pivot candidate just need to find the journal to send_recovered on it.
				h->tx_slba = slba;
			}  /* else if (h->binje == 1 && slba != h->tx_slba) From uniqeness of JAM there can be only one valid jentry that point to the blockset and we would like to send_blkset_recovered also for garbage entries. Later there is another validation see is_journal_comitted func.*/
		}

		_NTh(trace_6_dp_ec_recov_hot_scan_jmdc, h, "Found matching jmd in si @SI.[@INDEX]", si, i);
		n_found++;
		if (n_found == 1) {
			cand = i;
			cand_slba = slba;
		}
		else
			_NTh(error_dp_ec_recov_hot_scan_jmdc, h, "Oops, @N_FOUND jmd are matching (@SI.@INDEX)", n_found, si, i);
		/* check for other matching entries */
	}

	if (n_found == 1) {
		u16 offset = (cand_slba - h->tx_slba);
		h->tx_jentries[si].jent_idx = cand;
		h->tx_jentries[si].is_valid = true;
		h->tx_jentries[si].offset = offset;
		_NTh(trace_7_dp_ec_recov_hot_scan_jmdc, h, "Found single match, jent_idx=@JENT_IDX, offset=@RV", cand, offset);
		cand_jent.md_arr = &jmdc[cand * binje];
		nvmeibc_block_dp_ec_md_decode_jentry(r1->segments[si].first_lba, &h->tx_jentries[si], &cand_jent);
		rv = 0;
	}
	else if (n_found == 0) {
		_NTh(trace_8_dp_ec_recov_hot_scan_jmdc, h, "No matching jmd in si @SI", si);
		rv = -ENOENT;
	}
	else {
		NHTR_STS(trace_9_dp_ec_recov_hot_scan_jmdc, h, "Found more than one (@N_FOUND) matching jmd, FATAL", n_found);
		rv = -ETOOMANYREFS;
	}

out:
	NFOUT;
	return rv;
}

/* Used to translate the callback of free_jrnl_ents to the standard gen command one.
 * TBD: Make free_jrnl_ents also use the standard one */
static void __free_jrnl_ents_cb(struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	struct seg_info *seg_info = container_of(comp, struct seg_info, free_ents_comp);
	struct nvmeibc_block_command *bcmd = &seg_info->cmd;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();

	if (NCL_had_acquire_callback(comp->status))
		icore_ops->cb_called_free_jrnl_ents(icore_ops, comp->disk, comp);
	dp_ec_sync_stale_cb_stg_end(bcmd);
}


// If no lock on seg returns NULL
static inline const struct nvmeibc_cmd_lock *__get_lock_on_seg(const struct htr_ctx *h, int si)
{
	int l;
	const struct recovery_sync_op *so = h->so;
	const int nlocks = so->locks->nlocks;
	for (l = 0; l < nlocks; l++) {
		const struct nvmeibc_cmd_lock *lock = &so->locks[l];
		const int lock_si = (lock->ds - so->r1->segments);
		if (lock_si == si) {
			return lock;
		}
	}

	return NULL;
}

static int send_recovered(struct htr_ctx *h, int si)
{
	struct nvmeibc_block_command *cmd = &h->seg_info[si].cmd;
	u32 range_id, entry_id;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	bool pass2toma = h->params.is_hot_jgc ?
			false :
			((si == (int)h->owner_si) || __is_seg_parity(si, h));	// Todo: not ideal, explicit assumption on the location of locks
	struct nvmeibc_disk_segment *ds = &h->params.raid1->segments[si];
	int rv;
	const struct nvmeibc_cmd_lock *lock = __get_lock_on_seg(h, si);
	union nvmeib_lock_blkset_entry lock_entry;

	NFIN;

	init_block_cmd(cmd, h->so->o, ds);

	if (lock) {
		lock_entry = nvmeibc_d_rdma_comp_get_lock_blkset_entry(&lock->comp);
	} else {
		lock_entry.all = h->params.lock_ent.all;
		pass2toma = false;
	}

	/* if seg has valid jentry, req includes it
	   and must complete successfully.
	   Otherwise may complete with -ENOENT. */
	if (h->tx_jentries[si].is_valid) {
		const u64 rng_gen_id = h->seg_info[si].clj.desc.rng_gen_id;
		const u8 ent_gen_id = h->seg_info[si].clj.ent_md[h->tx_jentries[si].jent_idx].ent_gen_id;

		struct nvmeibc_disk_free_jrnl_ents_comp *free_ents_comp = &h->seg_info[si].free_ents_comp;
		/* If we have a valid jentry, send the free ents command instead */
		free_ents_comp->disk = h->params.raid1->segments[si].disk;
		memcpy(free_ents_comp->seg_uuid, ds->uuid, NVMEIB_GID_STR_MAX);
		free_ents_comp->recov_src = NVMEIB_RECOV_SRC_HTR;
		free_ents_comp->blkset_slba = ds->first_lba + (h->so->rlba / h->so->r1->slice_size);
		free_ents_comp->pass2toma = pass2toma;
		free_ents_comp->blkset_num = h->so->rlba / (LOCKSET_SLICES * h->so->r1->slice_size);
		free_ents_comp->lock_ent = lock_entry.all;
		memcpy(free_ents_comp->serjio_boot_id,
			   h->seg_info[si].clj.desc.serjio_boot_id, NVMEIB_GID_STR_MAX);
		free_ents_comp->ents[0].rng_idx = h->seg_info[si].clj.desc.rng_id;
		free_ents_comp->ents[0].rng_gen_id = h->seg_info[si].clj.desc.rng_gen_id;
		free_ents_comp->ents[0].ent_idx = h->tx_jentries[si].jent_idx;
		free_ents_comp->ents[0].ent_md = h->seg_info[si].clj.ent_md[h->tx_jentries[si].jent_idx];
		free_ents_comp->ents[0].rng_binje = h->binje;
		free_ents_comp->num_ents = 1;
		free_ents_comp->gen_cmd = cmd->gen_cmd;
		free_ents_comp->callback = __free_jrnl_ents_cb;

		_NTh(trace_dp_ec_recov_hot_send_recovered_entries, h,
			"Send free-ents, disk @DISK_NAME, jri=@JRI, jent_idx=@JENT_IDX gen_id=@JRNL_RNG_GEN:@JRNL_RNG_ENT_GEN, pass2toma=@BOOL lock_id=@LOCK_ENT_U64",
			ds->disk->name, h->seg_info[si].clj.desc.rng_id, h->tx_jentries[si].jent_idx, rng_gen_id, ent_gen_id, pass2toma, lock_entry.all);
		__nvmeibc_free_jrnl_ents_request_pet_describe(h, si, free_ents_comp);
		rv = icore_ops->free_jrnl_ents(icore_ops, ds->disk, free_ents_comp);
		if (rv) {
			_NTh(trace_dp_ec_recov_hot_send_recovered_entries_failed, h,
				"Send free-ents failed, disk @DISK_NAME, jri=@JRI, jent_idx=@JENT_IDX gen_id=@JRNL_RNG_GEN:@JRNL_RNG_ENT_GEN rv=@RV",
				ds->disk->name, h->seg_info[si].clj.desc.rng_id, h->tx_jentries[si].jent_idx, rng_gen_id, ent_gen_id, rv);
			/*
			 * Ideally we (the block team) would like to execute the following two lines;
			 * (This is the way block code implements failure treatment)
			 * But in HOT recovery, all events are handled via "a private work-queue (kth context)
			 * thus, it is possible to manage in a single place a bitmap of all successful launches (-EINPROGRESS)
			 * and handle waits and failures in a single place (call_for_bitmap & wip_bm)
			free_ents_comp->status = NCL_STATUS_DISKDEAD;	// Same as NCL_STATUS_FAIL_COMP
			free_ents_comp->callback(free_ents_comp);
			*/
		}
	}
	else {
		range_id = NVMEIB_EC_INVALID_JOURNAL_RANGE;
		entry_id = NVMEIB_EC_INVALID_JOURNAL_ENTRY;

		nvmeibcbdpec_fill_blockset_recovered_info(cmd, HTR_CMD_TIMEOUT, h->htr_kth.ptr,
			lock_entry.all, h->so, range_id, entry_id, pass2toma);

		_NTh(trace_dp_ec_recov_hot_send_recovered, h, "Send blkset-recovered, disk @DISK_NAME, jri=@JRI, jent_idx=@JENT_IDX, pass2toma=@BOOL lock_id=@LOCK_ENT_U64",
		   ds->disk->name, range_id, entry_id, pass2toma, lock_entry.all);
		nvmeibc_send_recovered_blkset_request_pet_describe(h->so, cmd, lock_entry.all, si);
		rv = icore_ops->execute_gen(icore_ops, ds->disk, cmd->gen_cmd);
		if (rv) {
			_NTh(trace_dp_ec_recov_hot_send_recovered_failed, h, "Send blkset-recovered failed, disk @DISK_NAME, jri=@JRI, jent_idx=@JENT_IDX rv=@RV",
			   ds->disk->name, range_id, entry_id, rv);
			if (cmd->gen_cmd)
				cmd->gen_cmd->comp_code = rv; // Simulate completion
			/* See the comment above dp_ec_sync_cmd_cb(cmd); */
		}
	}

	NFOUT;
	return (rv == 0) ? -EINPROGRESS : -1;
}

static int send_recovered_comp(struct htr_ctx *h, int si)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd = h->seg_info[si].cmd.gen_cmd;
	int rv;

	if ((rv = gen_cmd_comp_check(h, si)))
		goto out;

	if ((int)gen_cmd->rsp.br.status == -ENOENT) {
		if (!h->tx_jentries[si].is_valid) {
			_NTh(error_dp_ec_recov_hot_send_recovered_comp, h, "No jour range/entry found at target although jentry is valid (si=@SI)", si);
			rv = -1;
		}
	} else if (gen_cmd->rsp.br.status) {
		_NTh(error_1_dp_ec_recov_hot_send_recovered_comp, h, "Unexpected blkset-recovered status=@STATUS (si=@SI)", gen_cmd->rsp.br.status, si);
		rv = -1;
	}

out:
	__nvmeibc_send_recovered_response_pet_describe(h, si, rv);
	return rv;
}

/* Before Toma moves a segment back to RW state, it must resolve all
   stale-locks (and dirty bits of course), across all pRAID, that were
   generated while the segment was dying (was still RW in topo) or while
   it was dead (in D state). This means that the RW parity column (if any),
   that HTR may use as a base, must have been writable when stale-lock was
   created and thus must have valid journal to rely on. */
static int get_jmd_pivot_toporw_segs(struct htr_ctx *h)
{
	u64 rw_parities = h->rlba_parities_bm & nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable);
	int pivot = 0;
	int rv = -1;
	NFIN;

	BUG_ON(false == has_rw_parity(h));

	/* paranoid */
	if (h->jour_pivot != HTR_INVALID_SEG_ID)
		goto out;
	if (rw_parities == 0)
		goto out;

	//TODO: consider to check every parity segment to find pivot - it may give us access to txbm
	pivot = find_first_bit((ulong *)&rw_parities, h->n_segs);
	_NTh(trace_dp_ec_recov_hot_get_jmd_pivot_toporw_segs, h, "scan jmdc of pivot RW parity column (@PIVOT)", pivot);

	if (h->seg_info[pivot].clj.desc.rng_id == NVMEIB_EC_INVALID_JOURNAL_RANGE) {
		_NTh(trace_2_dp_ec_recov_hot_get_jmd_pivot_toporw_segs, h, "No jmdc for uuid, pivot RW parity (si=@PIVOT), Done", pivot);
		HTR_STATS_INC(n_uuid_no_jour);
		rv = 0;
		goto out;
	}

	if ((rv = scan_jmdc(h, pivot, true)) < 0) {
		if (rv == -ENOENT) {
			_NTh(trace_3_dp_ec_recov_hot_get_jmd_pivot_toporw_segs, h, "Tx not journalled in RW parity seg, Done");
			rv = 0;
		}
		else
			NHTR_STS(trace_4_dp_ec_recov_hot_get_jmd_pivot_toporw_segs, h, "Fail scan-jmdc from pivot RW parity column, FATAL");
		goto out;
	}

	_NTh(trace_5_dp_ec_recov_hot_get_jmd_pivot_toporw_segs, h, "Found Tx's jmd in RW parity column (si=@PIVOT, jent_idx=@JENT_IDX)",
		pivot, h->tx_jentries[pivot].jent_idx);
	h->jour_pivot = pivot;
out:
	NFOUT;
	return rv;
}

static void init_state_bm_from_extern_txbm(struct htr_ctx *h, struct state_bm *state_bm, roles_bmp_t tx_bm)
{
	state_bm->all = txbm_2_topo(h, tx_bm);
	state_bm->rw = state_bm->all & nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable);
	state_bm->w = state_bm->all & nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, w);
	state_bm->d = state_bm->all & nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, dead);
}

static void init_txbm_topo_from_extern_txbm(struct htr_ctx *h, roles_bmp_t tx_bm)
{
	init_state_bm_from_extern_txbm(h, &h->txbm_topo, tx_bm);
}

static int init_txbm_topo_from_pivot(struct htr_ctx *h)
{
	struct seg_info *s;
	struct jent_md_decompressed *pivot_jent;
	struct state_bm slice_txbm;
	int rv = -1, i;
	NFIN;

	if (h->jour_pivot == HTR_INVALID_SEG_ID) {
		_NTh(trace_dp_ec_recov_hot_init_txbm_topo_from_pivot, h, "No jour-pivot segment yet");
		goto out;
	}
	s = &h->seg_info[h->jour_pivot];
	pivot_jent = &h->tx_jentries[h->jour_pivot];
	if (!s->clj.jmdc) {
		_NTh(trace_1_dp_ec_recov_hot_init_txbm_topo_from_pivot, h, "jour-pivot has no jmdc");
		goto out;
	}
	if (!pivot_jent->is_valid) {
		_NTh(trace_2_dp_ec_recov_hot_init_txbm_topo_from_pivot, h, "jour-pivot has no valid jentry");
		goto out;
	}

	h->tx_height = pivot_jent->len;
	for (i = 0; i < h->tx_height; i++) {
		init_state_bm_from_extern_txbm(h, &slice_txbm, pivot_jent->md_arr[i].tx_bmp);
		if (!__is_bmp_included_in((u32)(1 << h->jour_pivot), slice_txbm.rw)) {
			_NTh(error_dp_ec_recov_hot_init_txbm_topo_from_pivot, h, "Odd, pivot seg is not in txbm_topo.rw");
			goto out;
		}
		h->txbm_topo.all |= slice_txbm.all;
		h->txbm_topo.rw |= slice_txbm.rw;
		h->txbm_topo.w |= slice_txbm.w;
		h->txbm_topo.d |= slice_txbm.d;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}

static int check_jmdc_non_pivot_segs_committed(struct htr_ctx *h)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	int si, rv = -1;
	ulong other_bm = 0, topo_rw = nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable);
	NFIN;

	if (h->jour_pivot == HTR_INVALID_SEG_ID) {
		other_bm = topo_rw;                  // We don't know the txbm scan all readable jmdcs
	} else {
		other_bm = (h->txbm_topo.rw & (~(1 << h->jour_pivot)));
	}

	for_each_set_bit(si, &other_bm, r1->replicas) {
		/* Scan all other TxRW/RW segs for ~identical jmd-entry */
		if ((rv = scan_jmdc(h, si, false)) < 0) {
			if (rv == -ENOENT) {
				_NDh(trace_5_dp_ec_recov_hot_check_jmdc_non_pivot_segs_committed, h, "Tx not journalled in RW parity seg, si=@SI", si);
			} else {
				NHTR_STS(trace_2_dp_ec_recov_hot_check_jmdc_non_pivot_segs_committed, h, "Fail to scan-jmdc of TxRW/RW segs, FATAL");
				goto out;
			}
		}
	}

	if (h->jour_pivot != HTR_INVALID_SEG_ID)
		h->jour_committed = __is_journal_committed(r1, h->tx_jentries, topo_rw);

	rv = 0;

out:
	NFOUT;
	return rv;
}

/* We need to read all jmcds even if journal_no_committed in order to know which jentries to free*/
static int read_all_rw_jmdcs(struct htr_ctx *h) {
	int rv = -1;
	sgmnts_bmp_t topo_rw = nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable);
	NFIN;

	_NTh(trace_dp_ec_recov_hot_read_all_rw_jmdcs, h, "Read jmdc of all RW segs");

	if (call_for_bitmap(h, topo_rw, read_jmdc, H_ON_ERR_BREAK)) {
		NHTR_STS(trace_1_dp_ec_recov_hot_read_all_rw_jmdcs, h, "Failed to read one of the jmdcs on RW seg, FATAL");
		goto out;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}

/* If both parities are dead there can still be a journal commited cause TX could have been executed on different topology */
static int calc_jour_committed_bmp(struct htr_ctx *h)
{
	int rv = -1;
	NFIN;

	/* TxID first valid is 0 and as write-flow incs it after
	   jour stage is done, jour is committed iff TxID > 0
	   (&& TxID!=Inv-value) */
	if (nvmeib_txid_no_journal(h->params.lock_ent.blkset_info.bits.txid)) {
		_NTh(trace_dp_ec_recov_hot_sat_jour_committed, h, "TxID indicates Journal NOT committed");
		rv = 0;
		goto out;
	}

	if (read_all_rw_jmdcs(h) < 0)
		goto out;

	if (has_rw_parity(h)) {
		/* Get jmd-pivot from a Topo's RW seg */
		if (get_jmd_pivot_toporw_segs(h) < 0) {
			_NTh(trace_1_dp_ec_recov_hot_calc_jour_committed_bmp, h, "Fail getting jmd-pivot");
			goto out;
		}

		if (h->jour_pivot == HTR_INVALID_SEG_ID) {
			_NTh(trace_2_dp_ec_recov_hot_calc_jour_committed_bmp, h, "No jmd-pivot, Journal NOT committed");  // Still need to scan all other jmdcs to know which jentries to free.
		} else {
			_NTh(trace_3_dp_ec_recov_hot_calc_jour_committed_bmp, h, "Found jmd-pivot (si=@JOUR_PIVOT, jent_idx=@JENT_IDX)",
				 h->jour_pivot, h->tx_jentries[h->jour_pivot].jent_idx);
			if (init_txbm_topo_from_pivot(h) < 0) {
				NHTR_STS(trace_4_dp_ec_recov_hot_calc_jour_committed_bmp, h, "Fail init TxBM & Topo, FATAL");
				goto out;
			}
		}
	}

	/* Important
	   1. calc if jour_committed
	   2. If jmd-pivot commited compare to all TxRW segs
		  i.e TxBm & Topo's RW seg(s), else compare them to each other */
	if (check_jmdc_non_pivot_segs_committed(h) < 0) {
		_NTh(trace_5_dp_ec_recov_hot_calc_jour_committed_bmp, h, "Fail checking non-pivot jmd's commited state");
		goto out;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}

static bool does_dmd_match(struct htr_ctx *h, int si)
{
	union nvmeibc_block_dp_ec_data_block_md *dmd;
	u32 ver;
	bool match = false;
	BUILD_BUG_ON(((BITS_PER_BYTE * (sizeof(ver))) <
				  NVMEIBC_DP_EC_DMD_BITS_VERSION));
	NFIN;

	dmd = &h->seg_info[si].dblk.md[h->cur_slice]->dmd;
	if (dmd->tx_id != h->params.recoveree_txid) {
		_NTh(trace_dp_ec_recov_hot_does_dmd_match, h, "si @SI: diff TxID (@TXID,@RECOVEREE_TXID)",
			si, dmd->tx_id, h->params.recoveree_txid);
		goto out;
	}

	if (dmd->jri != h->seg_info[si].clj.desc.rng_id) {
		_NTh(trace_1_dp_ec_recov_hot_does_dmd_match, h, "si @SI: diff JRI (@JRI,@RNG_ID) all=@ALL_LLONG",
			si, dmd->jri, h->seg_info[si].clj.desc.rng_id, *(u64 *)dmd);
		goto out;
	}

	ver = dmd->D.version; //(__is_seg_parity(si, h) ? dmd->P.version: dmd->D.version);
	if (ver != NVMEIBC_DATA_MD_VERSION) {
		_NTh(trace_2_dp_ec_recov_hot_does_dmd_match, h, "si @SI: diff VER (@MD_VER,@MD_VER)", si, ver, NVMEIBC_DATA_MD_VERSION);
		goto out;
	}

	#ifdef DEBUG_SAVE_JENTRY
	{
		const u32 d2j_rng = dmd->D.d2j_rng;
		WARN(d2j_rng != get_jidx_rblk(h, si) && d2j_rng != get_jidx_idx(h, si),
		     "HTR %px jidx for seg %d jentry is not the same as the one in MD (expected rblk=%d or jidx=%d, md d2j=%d)\n",
		     h, si, (u32)get_jidx_rblk(h, si), (u32)get_jidx_idx(h, si), d2j_rng);\
	}
	#endif


	match = true;

out:
	NFOUT;
	return match;
}

static int calc_pre_tx_dbits(struct htr_ctx *h, const sgmnts_bmp_t read_ok_bm)
{
	const ulong txrw_p_ok_bm = (h->txbm_topo.rw & h->rlba_parities_bm & read_ok_bm);
	raid_sgmnt_t db_source_parity;
	int rv = -1;

	if (txrw_p_ok_bm) { // get pre dbits from RW pari which been read succefully
		db_source_parity = find_first_bit(&txrw_p_ok_bm, h->n_segs);
	} else {
		const int n_txrw = hweight32(h->txbm_topo.rw);
		const int n_read_ok = hweight32(read_ok_bm);
		NHTR_STS(error_dp_ec_recov_hot_calc_pre_tx_dbits, h, "Error, Read only @N_READ_OK [@BITMAP] of @N_TXRW[@BITMAP] TxRW segs, "
		   "and no parity to calculate pre Tx dbits from, FATAL",
		n_read_ok, read_ok_bm, n_txrw, h->txbm_topo.rw);
		goto out;
	}

	h->cur_slice_info.pre_slice_dbits.dbits = fill_nvmeibc_dbits_entry_from_md(&h->seg_info[db_source_parity].dblk.md[h->cur_slice]->dmd);
	h->cur_slice_info.pre_slice_dbits.is_initialized = true;

	rv = 0;

out:
	return rv;
}

static inline void calc_new_dbits(struct htr_ctx *h)
{
	union nvmeibc_dbits_entry pre_slice, pre_blkset = {.all_bits = h->so->cmds->rld.pre.bits.dirty};
	const int num_parities = nvmeibc_raid1_get_protect_lvl(h->params.raid1);

	/* Use lockset's Dbits as base, They might be pre or post TX depending on when failure occured */
	WARN(nvmeibc_dbits_get_n_unk(&pre_blkset, num_parities) != 0, "nvmeibc bug, dbits=0x%x\n", pre_blkset.all_bits);	// Should have resolved them earlier

	WARN(h->cur_slice_info.pre_slice_dbits.is_initialized == false, "nvmeibc bug\n"); // Sanity
	pre_slice = h->cur_slice_info.pre_slice_dbits.dbits;

	{
		nvmeibc_dbits_tx_init_by_bmp(&h->cur_slice_info.new_dbits.slice_after_turnon, num_parities, h->txbm_topo.d, 0, 0); // Turnon bits of D segs on binfo and slice, Use Topo/r1 order (not owner-lock-first).
		nvmeibc_dbits_tx_apply(&pre_slice, &h->cur_slice_info.new_dbits.slice_after_turnon);
	}

	{
		nvmeibc_dbits_tx_init_by_bmp(&h->cur_slice_info.new_dbits.slice_after_turnoff, num_parities, h->txbm_topo.d, h->txbm_topo.w, 0); // Turnon bits of D segs on binfo and slice, Use Topo/r1 order (not owner-lock-first).
		nvmeibc_dbits_tx_apply(&pre_slice, &h->cur_slice_info.new_dbits.slice_after_turnoff);
	}

	{
		nvmeibc_dbits_tx_init_by_bmp(&h->cur_slice_info.new_dbits.binfo, num_parities, h->txbm_topo.d, 0, 0); // Clean bit of W segs in slice only
		nvmeibc_dbits_tx_apply(&pre_blkset, &h->cur_slice_info.new_dbits.binfo);
	}

	_NTh(trace_dp_ec_recov_hot_calc_new_dbits, h, "Dbits: old=[@DBITS] --> {slice_after_turnon=[@DBITS],slice_final=[@DBITS],binfo=[@DBITS]",
		pre_blkset.all_bits, h->cur_slice_info.new_dbits.slice_after_turnon.post.all_bits, h->cur_slice_info.new_dbits.slice_after_turnoff.post.all_bits, h->cur_slice_info.new_dbits.binfo.post.all_bits);
	WARN(h->cur_slice_info.new_dbits.binfo.action.db_conv_map, "nvmeibc bug\n");
}

static int check_data_committed_and_calc_dbtis(struct htr_ctx *h)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	u32 txrw_bm = h->txbm_topo.rw;
	ulong read_ok_bm = 0;
	int n_txrw = hweight32(txrw_bm), n_read_ok, n_match;
	int si;
	u32 rv_bm;
	int rv = -1;
	NFIN;

	/* Sanity */
	if (!h->jour_committed) {
		NHTR_STS(trace_3_dp_ec_recov_hot_check_data_committed_and_calc_dbtis, h, "Odd, checking if data commited while journal is not fully commited.");
		goto out;
	}

	if (h->cur_slice_info.data_written_bm) {
		_NTh(trace_dp_ec_recov_hot_check_data_committed_and_calc_dbtis, h, "Oops, nz");
		goto out;
	}

	/* Read (Data and) MD of AMAP TxRW segs */
	rv_bm = call_for_bitmap(h, txrw_bm, read_dblk, H_ON_ERR_CONT);
	if (rv_bm == txrw_bm) {
		NHTR_STS(trace_1_dp_ec_recov_hot_check_data_committed_and_calc_dbtis, h, "Fail read-dblk of ALL TxRW segs, FATAL");
		goto out;
	}
	read_ok_bm = (txrw_bm & (~rv_bm));
	n_read_ok = hweight32(read_ok_bm);

	/* Per RW seg we managed to read, check if Data/Parity MD
	   match Lockset-Entry (TxID) && Journal-MD (JRI) and has
	   valid EDIC */
	for_each_set_bit(si, &read_ok_bm, r1->replicas) {
		if (does_dmd_match(h, si)) {
			h->cur_slice_info.data_written_bm |= (1 << si);
		}
#if HTR_DEBUG
		else {
			h->seg_info[si].dblk.valid[h->cur_slice] = false;
			h->seg_info[si].dblk.edic_sts = HTR_INVALID_EDIC_STS;
		}
#endif
	}

	/* Decision */
	n_match = hweight32(h->cur_slice_info.data_written_bm);
	_NTh(trace_2_dp_ec_recov_hot_check_data_committed_and_calc_dbtis, h, "Out of @N_TXRW TxRW segs, read=@READ, match={n=@N_MATCH, bm=[@BITMAP]}",
		n_txrw, n_read_ok, n_match, h->cur_slice_info.data_written_bm);
	if (n_match > 0) {
		h->cur_slice_info.data_committed_on_rw_seg = true;
	}
	else if (n_txrw == n_read_ok) {
		/* only if we managed to read all TxBM's dblks,
		   can we say that data was NOT committed on RW segs */
		h->cur_slice_info.data_committed_on_rw_seg = false;
	} else {
		NHTR_STS(error_dp_ec_recov_hot_check_data_committed_and_calc_dbtis, h, "Error, Read only @N_READ_OK [@BITMAP] of @N_TXRW[@BITMAP] TxRW segs "
			   "but no DMD match, FATAL",
			n_read_ok, (u32)read_ok_bm, n_txrw, h->txbm_topo.rw);
		goto out;
	}

	if (calc_pre_tx_dbits(h, read_ok_bm) < 0)
		goto out;
	calc_new_dbits(h);

	rv = 0;

out:
	NFOUT;
	return rv;
}

static inline void gen_md(struct htr_ctx *h, int si, u32 txid, u32 jri, bool should_turnoff_slice_dbits)
{
	union nvmeibc_block_dp_ec_data_block_md *md = &h->seg_info[si].dblk.md[h->cur_slice]->dmd;
	u32 edic = h->seg_info[si].dblk.edic_res;
	NFIN;

	BUG_ON(!h->seg_info[si].dblk.valid[h->cur_slice]);
	BUG_ON(h->seg_info[si].dblk.edic_sts == HTR_INVALID_EDIC_STS);
	//omril: check inputs are within bitfield

	if (__is_seg_parity(si, h)) {
		nvmeibc_block_dp_ec_md_make_p(md, edic, jri, txid, should_turnoff_slice_dbits ? h->cur_slice_info.new_dbits.slice_after_turnoff.post : h->cur_slice_info.new_dbits.slice_after_turnon.post, get_jidx_idx(h, si));
	} else
		nvmeibc_block_dp_ec_md_make_d(md, edic, jri, txid, get_jidx_idx(h, si));

	NFOUT;
}

static inline void gen_md_rw(struct htr_ctx *h, int si, bool should_turnoff_slice_dbits)
{
	NFIN;
	BUG_ON(((1 << si) & h->txbm_topo.rw) == 0);
	gen_md(h, si, h->params.recoveree_txid, h->seg_info[si].clj.desc.rng_id, should_turnoff_slice_dbits);
	NFOUT;
}

static inline void gen_md_w(struct htr_ctx *h, int si, bool should_turnoff_slice_dbits)
{
	bool is_parity = ((1 << si) & h->rlba_parities_bm);
	NFIN;
	BUG_ON(has_rw_parity(h) && ((1 << si) & h->txbm_topo.w) == 0);
	BUG_ON(!has_rw_parity(h) && ((1 << si) & nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, w)) == 0);

	if (h->cur_slice_info.is_neverwritten_slice) { // If slice is neverwritten all regened blocks needs to be neverwritten.
		union nvmeibc_dbits_entry dbits = {.all_bits = 0};
		if (is_parity)
			dbits.all_bits = should_turnoff_slice_dbits ? h->cur_slice_info.new_dbits.slice_after_turnoff.post.all_bits : h->cur_slice_info.new_dbits.slice_after_turnon.post.all_bits;
		BUG_ON(!h->cur_slice_info.is_rollback); // After roll-forward there can't be neverwritten parity.
		nbdpec_md_mark_data_never_written_with_dbits(&h->seg_info[si].dblk.md[h->cur_slice]->dmd, is_parity, dbits);  // We are going to write this block so set it to our version's nefverwritten without dbits cause dbits will be written in next stage
	} else {
		gen_md(h, si, h->cur_slice_info.txid_for_regen, JRI_MARK_NO_JOURNAL, should_turnoff_slice_dbits); /* We might want to put here in the future JRI_MARK_NO_JOURNAL_HTR instead, because if we regen on nonm readable seg and we haven't regen other one and crashed and the next time cold will run and this seg will be readable (readfail the fixed) we might think we don't need to roll_fwd and regen the other seg. If cold decide to make on it roll-fwd he might mistakenly call htr on tx with overrriden journal. */
	}

	NFOUT;
}

#include "../../datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"

/* rotate right all inputs to lockset owner seg and issue regen */
static int regen_slice(struct htr_ctx *h, u32 in_bm, u32 out_bm, __attribute__ ((unused)) enum nvmeibc_dp_recovery_hot_roll_fwd_reason roll_fwd_reason)
{
	const struct nvmeibc_block_device *nd = h->so->o->nd;
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	const u64 first_rlba = owner_rlba_address(h);
	int shift = h->owner_si;
	int si;
	int rv = -1;
	NFIN;

	if (!h->snake_size) { // Validate it
		h->snake_size = nd->dp.p.snake_size;
	}

	/* checks */
	BUG_ON(r1->replicas > HTR_REGEN_VEC_LEN);
	if (in_bm & out_bm) {
		_NTh(trace_dp_ec_recov_hot_regen_slice, h, "bitmaps overlap");
		goto out;
	}
	if ((in_bm | out_bm) & ~((1 << r1->replicas) - 1)) {
		_NTh(trace_1_dp_ec_recov_hot_regen_slice, h, "Invalid bitmap @BITMAP", (in_bm | out_bm));
		goto out;
	}
	if (hweight32(in_bm) < (u32)r1->slice_size) {
		_NTh(trace_2_dp_ec_recov_hot_regen_slice, h, "Insufficient input blocks");
		goto out;
	}
#if HTR_DEBUG
	for (si = 0; si < r1->replicas; si++)
		BUG_ON(h->regen_vec[si]);
#endif

	_NTh(trace_3_dp_ec_recov_hot_regen_slice, h, "Regenerate [@BITMAP] from [@BITMAP], @RLBA",
		out_bm, in_bm, first_rlba);
	/* fill regen-vector in ror order */
	rv = 0;
	in_bm  = ror32_width(in_bm , shift, r1->replicas);			        // Convert from seg index in praid to offset within slice
	out_bm = ror32_width(out_bm, shift, r1->replicas);
	for (si = 0; si < r1->replicas; si++) {
		struct seg_info *segi = &h->seg_info[(si+shift)%r1->replicas];	// Convert from seg index in praid to offset within slice
		h->regen_vec[si] = segi->dblk.data_addr[h->cur_slice];
		if (        in_bm & (1 << si)) {
			if (!segi->dblk.valid[h->cur_slice]) {
				_NTh(trace_4_dp_ec_recov_hot_regen_slice, h, "Invalid dblk si @SI", si);
				rv = -1;
				goto out;
			}
			h->crc_vec[si] = 0;
		} else if (out_bm & (1 << si)){
			h->crc_vec[si] = &segi->dblk.edic_res;
			rv |= Nset_if_zero_dblk_valid(error_dp_ec_recov_hot_regen_slice, h, (int)(segi - h->seg_info));	// XXX: a non-zero rv is later zeroed anyway, so why collect it?
			segi->dblk.edic_sts = 0; // Validate edic
		}
	}
	rv = nvmeibc_reed_solomon_fill_missing(r1->replicas, r1->slice_size, h->snake_size, h->regen_vec, NULL, h->crc_vec,
						 in_bm, out_bm, out_bm, first_rlba, nd->dp.enable_di_debug_mode);

	if (unlikely(nd->dp.enable_di_debug_mode)) {
		for (si = 0; si < r1->replicas; si++) {
			if (out_bm & (1 << si)) {
				__attribute__ ((unused)) struct seg_info *segi = &h->seg_info[(si+shift)%r1->replicas];	// Convert from seg index in praid to offset within slice
				data_blk_fill_for_hot_rec(segi->dblk.data_addr[h->cur_slice], roll_fwd_reason, h->params.is_cold);
			}
		}
	}

	if (rv)
		goto out;

	/* reset */
	for (si = 0; si < r1->replicas; si++)
		h->regen_vec[si] = NULL;

out:
	NFOUT;
	return rv;
}

/* We call this function after we know the:
   1. TxRW segs which completed data-write stage dring IO
   2. TxRW segs which we've managed to read their journal */
static int handle_read_jblk_err(struct htr_ctx *h)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	const ulong txbm_topo_rw = h->txbm_topo.rw;
	u32 regen_bm = 0, readable_bm, read_bm, input_bm;
	int n_regen = 0, n_readable, n_input;
	int si;
	u32 rv_bm;
	int rv = -1;
	NFIN;

	/* How many of TxRW segs require regeneration */
	for_each_set_bit(si, &txbm_topo_rw, r1->replicas) {
		if (!h->seg_info[si].dblk.valid[h->cur_slice]) {
			regen_bm |= (1 << si);
			n_regen++;
		}
	}

	/* Do we have enough readable RW segs for input to regen */
	readable_bm = nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable) & ~regen_bm;
	n_readable = hweight32(readable_bm);
	if (n_readable < r1->slice_size) {
		NHTR_STS(trace_dp_ec_recov_hot_handle_read_jblk_err, h, "Insufficient readable RW segs (@N_READABLE) for regen @N_REGEN segs, FATAL",
			n_readable, n_regen);
		goto out;
	}

	/* Read Data of RW segs we haven't already managed/failed to read their
	   data/jour, respectively, i.e. all RW segs that are not in TxBM */
	read_bm = nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable) & (~(h->txbm_topo.rw));
	rv_bm = call_for_bitmap(h, read_bm, read_dblk, H_ON_ERR_CONT);
	if (rv_bm == read_bm) {
		NHTR_STS(trace_1_dp_ec_recov_hot_handle_read_jblk_err, h, "Fail read-dblk of ALL RW segs not in TxBM, FATAL");
		goto out;
	}

	input_bm = nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable) & (~(rv_bm | regen_bm));
	n_input = hweight32(input_bm);
	if (n_input < r1->slice_size) {
		NHTR_STS(trace_2_dp_ec_recov_hot_handle_read_jblk_err, h, "Insufficient RW segs (@N_INPUT) for regen input, FATAL",
			n_input);
		goto out;
	}

	rv = regen_slice(h, input_bm, regen_bm, NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_READ_JBLK_ERR);
	if (rv) NHTR_STS(trace_3_dp_ec_recov_hot_handle_read_jblk_err, h, "Fail regen unreadble jour segs, FATAL");

out:
	NFOUT;
	return rv;
}

static int roll_fwd(struct htr_ctx *h)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	ulong roll_bm = 0, rv_bm;
	int si, rv = -1;
	NFIN;

	roll_bm = (h->txbm_topo.rw & (~h->cur_slice_info.data_written_bm));
	if (roll_bm == 0) {
		_NTh(trace_dp_ec_recov_hot_roll_fwd, h, "No seg to roll-fwd, Done");
		rv = 0;
		goto out;
	}

	/* Read Journal (and MD) of all TxRW segs that require roll-fwd */
	rv_bm = call_for_bitmap(h, roll_bm, read_jblk, H_ON_ERR_CONT);
	if (rv_bm == roll_bm) {
		NHTR_STS(trace_1_dp_ec_recov_hot_roll_fwd, h, "Fail to read-jblk of ALL TxRW segs require roll-fwd, FATAL");
		goto out;
	}

	if (rv_bm && (handle_read_jblk_err(h) < 0)) {
		_NTh(trace_2_dp_ec_recov_hot_roll_fwd, h, "Fail to handle read-jour err");
		goto out;
	}

	if (1) {/* sanity: jmdc[jidx] == disk-MD */
		const ulong tmp_bm = (roll_bm & ~rv_bm);
		for_each_set_bit(si, &tmp_bm, r1->replicas) {
			const union jblock_md *jmdd = &h->seg_info[si].dblk.md[h->cur_slice]->jmd;
			const union jblock_md *jmdc = &h->seg_info[si].clj.jmdc[get_jidx_rblk(h, si)];
			bool are_equal;
			if (jmdc->version == jmdd->version) {
				are_equal = (jmdc->raw == jmdd->raw);			// Raw comparison is fastest
			} else {											// IO was issued in previous software version, roll forward occurs in new software version. Serjio changed the version while loading jmdd to jmdc
				// Daniel: I am too lazy to implement a future proof comparator. Not sure this is even possible. Just verify J2D and txid only as if it was version 0. This is a debug code anyways
				are_equal = ((jmdc->j2d_0 == jmdd->j2d_0) && (jmdc->tx_id == jmdd->tx_id));
			}
			if (!are_equal) {
				NHTR_STS(trace_3_dp_ec_recov_hot_roll_fwd, h, "OOPS, si=@SI jmdc(@RAW) != jmdd(@RAW), FATAL", si, jmdc->raw, jmdd->raw);
				WARN_ON_ONCE(1);
				goto out;
			}
		}
	}

	/* Prep Data and MD for write */
	for_each_set_bit(si, &roll_bm, r1->replicas) {
		gen_md_rw(h, si, false);  // No turnoff dbits just turnon (turnon barrier)
	}

	if (unlikely(h->so->o->nd->dp.enable_di_debug_mode)) {
		for_each_set_bit(si, &roll_bm, r1->replicas) {
			__attribute__ ((unused)) const struct seg_info *segi = &h->seg_info[si];
			data_blk_fill_for_hot_rec(segi->dblk.data_addr[h->cur_slice],
									  NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_ROLL_FWD,
									  h->params.is_cold);
		}
	}

	/* Write rolled Data (and MD) to disks */
	if (call_for_bitmap(h, roll_bm, write_dblk, H_ON_ERR_BREAK)) {
		NHTR_STS(trace_4_dp_ec_recov_hot_roll_fwd, h, "Fail to write TxRW segs require roll-fwd, FATAL");
		goto out;
	}

	HTR_STATS_INC(n_roll_fwd);
	_NTh(trace_5_dp_ec_recov_hot_roll_fwd, h, "Roll fwd @HWEIGHT32 of @HWEIGHT32 TxRW segs",
	   hweight32(roll_bm), hweight32(h->txbm_topo.rw));
	rv = 0;

out:
	NFOUT;
	return rv;
}

static void __import_cold_candidate_data_blks_of_cur_slice(struct htr_ctx *h)
{
	const struct recovery_sync_op *so = h->so;
	const struct nvmibc_tx_candidate* cand = nvmeibcbdpec_get_rollfwd_jour_candidate(so);
	int si;
	ulong txbm_topo_rw;

	h->cur_slice_info.data_written_bm = 0;
	h->cur_slice_info.data_committed_on_rw_seg = true;

	init_txbm_topo_from_extern_txbm(h, cand->b.tx_bmp[h->cur_slice]); // TODO: can we skip this step?
	txbm_topo_rw = h->txbm_topo.rw;
	_NTh(t_01_echtr, h, "Importing candidate to HTR for cur_slice=@INT, j2slba=@J2D, txid=@TXID, txbm=@TXBM", h->cur_slice, cand->b.j2slba, cand->b.tx_id, txbm_topo_rw);

	for_each_set_bit(si, &txbm_topo_rw, h->n_segs) {
		struct seg_info *sinfo = &h->seg_info[si];
		const struct candidate_location *loc = &cand->locations[si];

		if (1) {// ---------------- Fill 'sinfo' generic
			h->cur_slice_info.data_written_bm |= (test_bit(h->cur_slice, loc->is_data_commited) ? (1 << si) : 0);
		}

		if (1) { // ---------------- Fill dblk: as if read_dblk() was called on its: reading all txbm's data and dmd.
			u32 ofst, ci = nvmeibc_tx_get_dblk_ofset_in_cmd(so, si, cand, h->cur_slice, &ofst);
			struct nvmeibc_block_command *cmd = &so->cmds[ci];
			void* src_blk;
			union nvmeibc_block_dp_ec_data_block_md *dmd;
			nvmeibc_tx_get_ptrs_to_blk_in_cmd(so, ci, ofst, &dmd, &src_blk);
			memcpy(sinfo->dblk.data_addr[h->cur_slice], src_blk, NVMEIBC_SECTOR_SIZE);
			sinfo->dblk.md[h->cur_slice]->dmd = *dmd;
			WARN(cmd->o_rv != 0, "nvmeibc bug rv=%d\n", cmd->o_rv);	// If cmd failed, how did we know to set .is_data_commited = 1?
			if (test_bit(h->cur_slice, loc->is_data_commited)) {
				sinfo->dblk.valid[h->cur_slice] = true;
				sinfo->dblk.edic_sts = 0;
			}
#if HTR_DEBUG
			else {   // In regular htr flow non segs will be setted as follow:
				sinfo->dblk.valid[h->cur_slice] = false;
				sinfo->dblk.edic_sts = HTR_INVALID_EDIC_STS;
			}
#endif
		}
	}

	// -- Calc pre tx dbits
	calc_pre_tx_dbits(h, txbm_topo_rw); // TODO: For now cold won't run until all readable blocks being read successfully and txbm_topo_rw == read_ok.
	calc_new_dbits(h);
}

static void __simulate_read_jmdc_descriptor_by_recoverer_descriptor(struct nvmeib_disk_client_journal_extended *desc,
								    const struct nvmeibc_disk_client_journal *recoverer_desc,
								    const struct candidate_location *recoveree_loc)
{
	memcpy(desc->serjio_boot_id, recoverer_desc->serjio_boot_id, NVMEIB_GID_STR_MAX);
	desc->rng_id = recoveree_loc->jri;
	desc->rng_slba = recoveree_loc->rng_start_lba;
	desc->rng_nlba = recoveree_loc->rng_size_lba;
	desc->rng_gen_id = recoveree_loc->rng_gen_id;
	desc->binje = recoveree_loc->rng_binje;
	desc->n_ents = recoveree_loc->rng_num_ents;
	desc->rng_blk = desc->n_ents * desc->binje;
}

static void __simulate_read_jmdc_array(struct htr_ctx *h, struct cl_jour *clj, u64 seg_start_dlba, const struct nvmibc_tx_candidate* cand, int si)
{
	const struct candidate_location *loc = &cand->locations[si];
	u16 binje_offset = loc->binje_offset;
	const u16 jent_idx = loc->jentry;
	//clj->jmdc_size = ...;	// Dont touch, as if read JMDC with a single entry
	struct jentry_md jent = {.md_arr = &clj->jmdc[jent_idx * h->binje] };
	int i;
	WARN(!loc->is_jour_commited, "nvmeibc cold recov passed wrong param\n");
	clj->valid = true;

	for (i = 0; i < loc->binje_len; i++) {
		bool has_next = (i < loc->binje_len - 1), has_prev = (i > 0);
		nvmeibc_block_dp_ec_jmd_encode(&jent.md_arr[i], (cand->b.j2slba + seg_start_dlba + binje_offset + i), cand->b.tx_id, cand->b.tx_bmp[i + binje_offset], has_next, has_prev); // Daniel: cand->b.j2slba has still a valid u32 value without overflow
	}

	nvmeibc_block_dp_ec_md_decode_jentry(seg_start_dlba, &h->tx_jentries[si], &jent);
	h->tx_jentries[si].jent_idx = jent_idx;
	h->tx_jentries[si].is_valid = true;
	h->tx_jentries[si].offset = binje_offset;
}

static int __import_cold_candidate_to_htr(struct htr_ctx *h)
{
	int rv = -1;
	const struct recovery_sync_op *so = h->so;
	const struct nvmeibc_raid1 *r1 = so->r1;
	const struct nvmibc_tx_candidate* cand = nvmeibcbdpec_get_rollfwd_jour_candidate(so);
	ulong txbm_topo_rw = 0;
	int si, i;
	u32 binje;

	HTR_STATS_INC(n_colds);
	h->jour_committed = true;
	h->jour_pivot = HTR_INVALID_SEG_ID;
	h->tx_slba = cand->b.j2slba;
	h->tx_height = cand->b.len;

	for (i = 0; i < h->tx_height; i++)
		txbm_topo_rw |= cand->b.tx_bmp[i];
	_NTh(t_02_echtr, h, "Importing candidate to HTR, j2slba=@J2D, txid=@TXID, len=@INT, txbm=@TXBM", cand->b.j2slba, cand->b.tx_id, cand->b.len, txbm_topo_rw);
	init_txbm_topo_from_extern_txbm(h, txbm_topo_rw);
	txbm_topo_rw = h->txbm_topo.rw;

	for_each_set_bit(si, &txbm_topo_rw, h->n_segs) {
		struct cl_jour *clj = &h->seg_info[si].clj;
		const struct candidate_location *loc = &cand->locations[si];
		__simulate_read_jmdc_descriptor_by_recoverer_descriptor(&clj->desc, &r1->segments[si].disk->jour, loc);

		binje = __get_n_jblks_in_jentry_from_disk_cmd(&clj->desc);										   // Enough to do it only once, becuase cold recovery already verified that all the values are equal.
		if (h->binje == HTR_INVALID_N_SLICE_JOUR) {
			h->binje = binje;
		} else if (h->binje != binje) {
			_NTh(trace_3_dp_ec_recov_hot__import_cold_candidate_to_htr, h, "Inconsistent binje: jmdc(@BINJE) != h(@BINJE)", binje, h->binje);
			goto out;
		}
		__simulate_read_jmdc_array(h, clj, r1->segments[si].first_lba, cand, si);									   // Fill 'clj'
	}

	rv = 0;
out:
	return rv;
}

static int htr_check_journal_committed(struct htr_ctx *h)
{
	int rv = -1;
	NFIN;

	/* For Topo-RW segs within TxBM,
	   chcek all have this Tx journalled */
	if (calc_jour_committed_bmp(h) < 0)
		goto out;

	if (!h->jour_committed) {
		rv = 0;
		goto out;
	}
	HTR_STATS_INC(n_jour_cmtd);
	_NTh(trace_1_dp_ec_recov_hot_check_journal_committed, h, "Journal Committed");
	rv = 0;

out:
	NFOUT;
	return rv;
}

static int htr_check_data_committed(struct htr_ctx *h)
{
	int rv = -1;
	bool roll_or_regen_backward = false;
	NFIN;

	if (h->params.is_cold) {
		__import_cold_candidate_data_blks_of_cur_slice(h);
		goto done;
	}

	/* Topo-RW segs within TxBM,
	   check at least one points back to its journal */
	h->cur_slice_info.data_committed_on_rw_seg = false;
	if (check_data_committed_and_calc_dbtis(h) < 0)
		goto out;

done:
	if (!h->cur_slice_info.data_committed_on_rw_seg) {
		h->cur_slice_info.is_rollback = true;    // (h->jour_committed) && (!h->cur_slice_info.data_committed_on_rw_seg)
	} else {
		HTR_STATS_INC(n_data_cmtd);
		_NTh(trace_1_dp_ec_recov_hot_htr_check_data_committed, h, "Data Committed [@BITMAP]",
			 h->cur_slice_info.data_written_bm);
	}

	if (h->cur_slice_info.is_rollback) // Full journal & no data, possible roll backwards (new data exists but not accessible to us)
		roll_or_regen_backward = (h->txbm_topo.rw != h->txbm_topo.all); // important: txbm_topo will be valid only if h->jour_committed
	h->cur_slice_info.found_real_tx = ((has_rw_parity(h)) && (h->cur_slice_info.data_committed_on_rw_seg || roll_or_regen_backward)); /* If true:
-       1. HTR found real TXBM (rollfwd, or tx was already fully commited, etc)
- 		2. HTR cannot disprove Tx existance so roll/regen backwards
		No parities case: RAM dbits Already treated*/
	rv = 0;

out:
	NFOUT;
	return rv;
}

static void htr_calc_is_slice_neverwritten(struct htr_ctx *h, u32 valid_bm)
{
	const ulong rw_p_bm = (h->txbm_topo.rw & h->rlba_parities_bm) & valid_bm;
	u32 src_parity_index = find_first_bit(&rw_p_bm, h->n_segs);

	BUG_ON(!rw_p_bm);  // This function called only when there are readable parities.

	if (h->cur_slice_info.is_rollback && nbdpec_md_was_data_never_written(&h->seg_info[src_parity_index].dblk.md[h->cur_slice]->dmd)) {
		h->cur_slice_info.is_neverwritten_slice = true;
	} else {  // if we rollfrwd or rollbackward but parity isn't neverwwritten
		h->cur_slice_info.is_neverwritten_slice = false;
	}

	// Sainty
	if (h->cur_slice_info.is_rollback) {
		int bit;
		struct nvmeibc_raid1 *r1 = h->params.raid1;

		for_each_set_bit(bit, &rw_p_bm, r1->replicas) {
			WARN(h->cur_slice_info.is_neverwritten_slice && (!nbdpec_md_was_data_never_written(&h->seg_info[bit].dblk.md[h->cur_slice]->dmd)), "One parity neverwritten while other is not\n");
		}
	}

	_NTh(trace_1_htr_calc_is_slice_neverwritten, h, "is_neverwritten_slice=@BOOL", h->cur_slice_info.is_neverwritten_slice);
	return;
}

static void htr_calc_txid_for_regen(struct htr_ctx *h, u32 valid_bm)
{
	const ulong rw_p_bm = (h->txbm_topo.rw & h->rlba_parities_bm) & valid_bm;
	u32 src_parity_index = find_first_bit(&rw_p_bm, h->n_segs);

	BUG_ON(!rw_p_bm);  // This function called only when there are readable parities.


	if (h->cur_slice_info.is_rollback) {
		h->cur_slice_info.txid_for_regen = h->seg_info[src_parity_index].dblk.md[h->cur_slice]->dmd.tx_id;  // The max_txid_id in slice is on one of the readable parities.
		_NTh(trace_1_htr_calc_txid_for_regen, h, "Rolling-back with max_txid_in_slice=@TXID", h->cur_slice_info.txid_for_regen);

		// Sanity
		{
			int bit;
			struct nvmeibc_raid1 *r1 = h->params.raid1;
			for_each_set_bit(bit, &rw_p_bm, r1->replicas) {
				WARN(h->cur_slice_info.txid_for_regen != h->seg_info[bit].dblk.md[h->cur_slice]->dmd.tx_id, "Readable parities have different txids.\n");
			}
		}

	} else {  // In roll-fwd we are taking the binfo's txid.
		h->cur_slice_info.txid_for_regen = h->params.recoveree_txid;
		_NTh(trace_2_htr_calc_txid_for_regen, h, "Regenning-fwd with binfo's_txid=@TXID", h->cur_slice_info.txid_for_regen);
	}

	return;
}

static int htr_w_segs(struct htr_ctx *h)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	const ulong topo_bm_rw = nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable);
	const ulong topo_bm_w =  nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, w);
	const ulong txbm_topo_w = h->txbm_topo.w;
	u32 rd_bm = 0, tmp_bm;
	int si, n_rd = 0;
	int n_w, n_tmp;
	int rv = -1;
	NFIN;

	BUG_ON(!h->jour_committed);
	n_w = hweight32(h->txbm_topo.w);
	if (n_w == 0) {
		_NDh(trace_dp_ec_recov_hot_htr_w_segs, h, "No TxW segs to regen");
		rv = 0;
		goto out;
	}

	/* sanity */
	for_each_set_bit(si, &topo_bm_w, r1->replicas) {
		if (h->seg_info[si].dblk.valid[h->cur_slice]) {
			NHTR_STS(trace_1_dp_ec_recov_hot_htr_w_segs, h, "Odd, W seg @SI data was already read, FATAL", si);
			goto out;
		}
	}

	/* Bitmap of RW segs we still haven't read */
	for_each_set_bit(si, &topo_bm_rw, r1->replicas) {
		if (!h->seg_info[si].dblk.valid[h->cur_slice]) {
			rd_bm |= (1 << si);
			n_rd++;
		}
	}
	/* sanity */
	tmp_bm = h->txbm_topo.rw & rd_bm;
	if (h->cur_slice_info.data_committed_on_rw_seg && tmp_bm) {
		NHTR_STS(trace_2_dp_ec_recov_hot_htr_w_segs, h, "Odd, Data-committed and we're past Roll-Fwd stage but "
			   "have not read all TxRW segs Data+MD [@BITMAP], FATAL",
			tmp_bm);
		goto out;
	}
	if (n_rd) {
		/* Read AMAP missing RW segs */
		_NTh(trace_3_dp_ec_recov_hot_htr_w_segs, h, "Read extra @N_RD RW segs required for regen", n_rd);
		call_for_bitmap(h, rd_bm, read_dblk, H_ON_ERR_CONT);
	}

	/* How many RW segs were read in total */
	n_tmp = 0;
	tmp_bm = 0;
	for_each_set_bit(si, &topo_bm_rw, r1->replicas) {
		if (h->seg_info[si].dblk.valid[h->cur_slice]) {
			tmp_bm |= (1 << si);
			n_tmp++;
		}
	}
	if (n_tmp < r1->slice_size) {
		NHTR_STS(trace_4_dp_ec_recov_hot_htr_w_segs, h, "Fail to read sufficient #RW segs for regenerate "
			   "W segs (@N_RD,@N_TMP,@SLICE_SIZE), FATAL", n_rd, n_tmp, r1->slice_size);
		goto out;
	}

	if (!(tmp_bm & (h->txbm_topo.rw & h->rlba_parities_bm))) {
		NHTR_STS(trace_4a_dp_ec_recov_hot_htr_w_segs, h, "Fail to read any RW TXBM-parity segs for regenerate "
			   "W segs ([@BITMAP] : [@BITMAP]), FATAL", tmp_bm, h->txbm_topo.rw & h->rlba_parities_bm);
		goto out;
	}

	htr_calc_is_slice_neverwritten(h, tmp_bm);
	if (!h->cur_slice_info.is_neverwritten_slice)
		htr_calc_txid_for_regen(h, tmp_bm);

	/* Regenerate Data/Parity W segs' Data and EDIC */
	if (regen_slice(h, tmp_bm, h->txbm_topo.w, NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_W_SEGS) < 0) {
		NHTR_STS(trace_5_dp_ec_recov_hot_htr_w_segs, h, "Fail regen TxW segs, FATAL");
		goto out;
	}

	/* Generate Data/Parity W segs' MD */
	for_each_set_bit(si, &txbm_topo_w, r1->replicas) {
		gen_md_w(h, si, false); // No turnoff dbits just turnon (turnon barrier)
	}

	/* Write W segs */
	if (call_for_bitmap(h, h->txbm_topo.w, write_dblk, H_ON_ERR_BREAK)) {
		NHTR_STS(trace_6_dp_ec_recov_hot_htr_w_segs, h, "Fail to write-dblk of TxW segs, FATAL");
		goto out;
	}

	if (h->cur_slice_info.is_rollback) {
		HTR_STATS_INC(n_regen_bkw);
	} else {
		HTR_STATS_INC(n_regen_fwd);
	}
	_NTh(trace_7_dp_ec_recov_hot_htr_w_segs, h, "Regenerated @N_W TxW segs", n_w);
	rv = 0;

out:
	NFOUT;
	return rv;
}

/* In case journal was committed (roll-fwd or drop) but Dbits in RW parity seg
 * are different than how HTR resolved the stale-lock, update them. Meaning...
 *  - Dbit shall be set   for any D seg in Topo & TxBM as HTR didnt write it.
 *  - Dbit shall be clear for any W seg in Topo & TxBM as HTR regenerated it.
 *
 * In case journal WAS committed and Dbits in parity segs, that are RW in HTR's
 * Topo, does NOT coinside with how HTR resolved the stale-lock, update them.
 *
 * In case journal WAS committed and Dbits in parity segs, that are RW in HTR's
 * Topo, does NOT coinside with how HTR resolved the stale-lock, update them.
 * This can happen regardless to whether data was committed or not (wrt to HTR's
 * Topo).
 *
 * Expected Dbits value:
 *  - Dbit shall be set   for any D seg in Topo & TxBM as HTR didnt write it.
 *  - Dbit shall be clear for any W seg in Topo & TxBM as HTR regenerated it.
 *
 * Note that for parity seg(s) are W in Topo (& By definition in TxBM too),
 * their Dbits were updated in prev-stage where we wrote their regenerated
 * data to disk.
 *
 * Scenario #1:
 * 1. During IO:  Si is RW (and in TxBM), Data write to RW-parities completes
 *  			  OK but does not complete on Si.
 * 2. During HTR: Si is D, Jour-Committed=Y, Data-Committed=Y, roll-fwd RW segs.
 *                But PMD.Dbits does NOT say that Si is dirty although it is.
 *
 * Scenario #2:
 * 1. During IO:  Si is RW (and in TxBM), Data write succeeds only to to Si
 *
 * 2. During HTR: Si is D, Jour-Committed=Y, Data-Committed=N (as the only
 *  			  witness to that is Dead now), drop Tx ...
 *                But PMD.Dbits does NOT say that Si is dirty although it is.
 *
 * When Si is W during HTR we have to complmentary scenarios where Si may be
 * marked dirty in PMD.Dbits although HTR had regenerated it. This can happen
 * if during IO, Si was D or W. This is less worse problem than the above as
 * here the penalty is longer recovery time and not data corruption.
 */
static int htr_update_parity_segs_md(struct htr_ctx *h)
{
	struct nvmeibc_raid1 *r1 = h->params.raid1;
	const int num_parities = nvmeibc_raid1_get_protect_lvl(r1);
	const ulong writable_p_bm = ((h->txbm_topo.w | h->txbm_topo.rw) & h->rlba_parities_bm);
	const ulong rw_p_bm = (h->txbm_topo.rw & h->rlba_parities_bm);
	u32 update_dbits_bm = 0;
	sgmnts_bmp_t new_dbit_bm;
	int si;
	int rv = -1;
	NFIN;

	BUG_ON(!h->jour_committed);
	if ((h->txbm_topo.d == 0) && (h->txbm_topo.w == 0)) {
		_NTh(trace_dp_ec_recov_hot_htr_update_parity_segs_md, h, "No TxBM seg are W or D in Topo, No need to update Dbits");
		rv = 0;
		goto out;
	}

	/* Convert new Dbits to bitmap */
	new_dbit_bm = nvmeibc_dbits_get_bm(&h->cur_slice_info.new_dbits.slice_after_turnoff.post, num_parities);

	/* Read RW parity seg, if not done yet */
	if (!h->cur_slice_info.data_committed_on_rw_seg && h->txbm_topo.w == 0) {
		// jour committed, data NOT committed, DEAD seg in TxBM --> We need to explicitily update PMD.Dbits but we must first read parities dblk we haven't read yet.
		// if h->txbm_topo.w than we already read all readbale blocks.
		if (call_for_bitmap(h, rw_p_bm, read_dblk, H_ON_ERR_BREAK)) {
			NHTR_STS(trace_1_dp_ec_recov_htr_update_parity_segs_md, h, "Fail to read TxRW parity seg, FATAL");
			goto out;
		}
	}

	/* Compare each parity seg PMD.Dbits to new Dbits */
	for_each_set_bit(si, &writable_p_bm, r1->replicas) {
		union nvmeibc_block_dp_ec_data_block_md *pmd;
		union nvmeibc_dbits_entry pmd_db;
		sgmnts_bmp_t pmd_dbit_bm = 0;
		bool is_parity = ((1 << si) & h->rlba_parities_bm);

		if (!h->seg_info[si].dblk.valid[h->cur_slice]) {
			NHTR_STS(trace_2_dp_ec_recov_hot_htr_update_parity_segs_md, h, "Invalid dblk for TxRW parity seg, si @SI, FATAL", si);
			goto out;
		}

		pmd = &h->seg_info[si].dblk.md[h->cur_slice]->dmd;

		if (nbdpec_md_get_data_written_state(pmd) == DATA_VIRGIN)
			nbdpec_md_mark_data_never_written_no_dbits(&h->seg_info[si].dblk.md[h->cur_slice]->dmd, is_parity);  // We are going to write this block so set it to our version's nefverwritten without dbits cause dbits will be written in next stage

		pmd_db = fill_nvmeibc_dbits_entry_from_md(pmd);
		pmd_dbit_bm = nvmeibc_dbits_get_bm(&pmd_db, num_parities);

		if (pmd_dbit_bm == new_dbit_bm)
			continue;

		_NTh(trace_3_dp_ec_recov_hot_htr_update_parity_segs_md, h, "Updating TxRW parity seg @SI Dbits ([@BITMAP] : [@BITMAP])", si, pmd_dbit_bm, new_dbit_bm);

		//omril: Do we need to check EDIC?

		/* Update seg's PMD.Dbits (locally) */
		nvmeibc_block_dp_ec_md_fill_p_with_dbits(pmd, h->cur_slice_info.new_dbits.slice_after_turnoff.post);

		update_dbits_bm |= (1 << si);
	}
	if (update_dbits_bm == 0) {
		_NDh(trace_4_dp_ec_recov_hot_htr_update_parity_segs_md, h, "No need to update TxRW parity segs Dbits");
		rv = 0;
		goto out;
	}

	/* Persist updated seg's PMD.Dbits */
	if (call_for_bitmap(h, update_dbits_bm, write_dblk, H_ON_ERR_BREAK)) {
		NHTR_STS(trace_6_dp_ec_recov_hot_htr_update_parity_segs_md, h, "Fail to update Dbits of TxRW parity segs, FATAL");
		goto out;
	}
	HTR_STATS_INC(n_update_parity_dbits);
	rv = 0;

out:
	NFOUT;
	return rv;
}


static int htr_send_recovered(struct htr_ctx *h)
{
	//RRRR: right now, HOT recovery will free entries, which it thinks "belong" to original transaction.
	//But, while reconstructing the original transaction, it may discover entries, which belongs to the same blockset or even slice.
	//(HOT recovery knows that those entries are problematic (ABND or UNKNOWN))
	//For a time being it skips them; In future, it could be nice to free them too.
	const u32 send_bm = nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, raid.all) & ~nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, dead);
	u32 rv_bm = 0;
	int rv = 0;

	/* We are freeing the owner lock last so that if client will fail in send_blockset recovered next time he will use the same lock_id in HTR.
	Moreover it will reduce the chance of leaked journal entries cause if we have err on non owner_lock in current topo
	there will be still a stale lock exists.
	This implementation also makes testing the code easier.
	*/
	const struct recovery_sync_op *so = h->so;
	const struct nvmeibc_cmd_lock *o_lock = &so->locks[0];
	sgmnts_bmp_t o_lock_bmp = (1 << (o_lock->ds - so->r1->segments));
	NFIN;

	_ND(trace_dp_ec_recov_hot_htr_send_recovered, "send blkset-recovered bm=[@BITMAP]", send_bm);
	/* First send on segs without lock cause if we got an problem on that segs we would like to leave the stale locks */
	rv_bm = call_for_bitmap(h, (send_bm & (~o_lock_bmp)), send_recovered, H_ON_ERR_BREAK);
	if (rv_bm) {
		NHTR_STS(trace_1_dp_ec_recov_hot_htr_send_recovered, h, "Fail to blkset-recovered, FATAL");
		rv = -1;
		goto out;
	}

	rv_bm = call_for_bitmap(h, (send_bm & o_lock_bmp), send_recovered, H_ON_ERR_BREAK);
	if (rv_bm) {
		NHTR_STS(trace_1_dp_ec_recov_hot_htr_send_recovered_3, h, "Fail to blkset-recovered, FATAL");
		rv = -1;
		goto out;
	}

	if (send_bm)
		HTR_STATS_INC(n_send_recovered);

out:
	NFOUT;
	return rv;
}

static void* __nvmeib_get_ndb_for_si(struct seg_info *seg_info)
{
	const gfp_t alloc_flags = nvmeibc_dp_get_allow_io_gfp_flags();
	seg_info->cmd.iocmd->reqs1.ndb = &seg_info->ndb;		// Simulate as if ndb was already allocated
	return nvmeib_get_ndb(&seg_info->cmd, HTR_SG_NENTS, alloc_flags);
}

#define __free_pages_array(arr, size, order)                                   \
	({                                                                         \
	u32 iii;                                                                   \
	for(iii=0; iii<size; iii++) {                                              \
		htr_free_pages((unsigned long)(arr[iii]), order);                            \
	}                                                                          \
	})

static int seg_info_alloc(struct seg_info *seg_info, int jmdc_size, int ent_md_sz, int num_free_ents)
{
	size_t ents_enc_buf_sz = num_free_ents * sizeof(struct wire_free_ents_entry);
	int rv = 0;
	u32 i;
	const gfp_t gfp = nvmeibc_dp_get_allow_io_gfp_flags();
	BUG_ON(seg_info->dblk.data_addr[0] || seg_info->clj.jmdc || seg_info->clj.ent_md);

	for(i=0; i<NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY; i++) {
		if (!(seg_info->dblk.data_addr[i] = (void *)htr__get_free_pages(gfp | __GFP_ZERO, NVMEBC_PAGES_ORDER_BLOCK)) ||
			!(seg_info->dblk.md[i] =  (void *)htr__get_free_pages(gfp | __GFP_ZERO, get_order(DISK_MAX_MD_SIZE_BYTE)))) {
			__free_pages_array(seg_info->dblk.data_addr, NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY, NVMEBC_PAGES_ORDER_BLOCK);
			__free_pages_array(seg_info->dblk.md, NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY,  get_order(DISK_MAX_MD_SIZE_BYTE));
		}
	}

	/* All these buffer are used for RDMA, must by phys contiguous */
	if (!(seg_info->clj.jmdc = htr_alloc_pages_exact(jmdc_size, gfp | __GFP_ZERO)) ||
		!(seg_info->clj.ent_md = htr_alloc_pages_exact(ent_md_sz, gfp | __GFP_ZERO)) ||
		!(seg_info->cmd.iocmd = htr_kzalloc(sizeof(*seg_info->cmd.iocmd), gfp)) ||
		!(__nvmeib_get_ndb_for_si(seg_info)) ||
		!(seg_info->cmd.gen_cmd = htr_kzalloc(sizeof(*seg_info->cmd.gen_cmd), gfp)) ||
		!(seg_info->free_ents_comp.ents = htr_kzalloc(sizeof(*seg_info->free_ents_comp.ents) * num_free_ents, gfp)) ||
		!(seg_info->free_ents_comp.ents_enc_buf = nvmeib_alloc(
			&seg_info->free_ents_comp.ents_enc_ai, ents_enc_buf_sz, dp_recovery_hot)))
	{
		if (seg_info->cmd.iocmd)
			__nvmeib_put_ndb(seg_info->cmd.iocmd->reqs1.ndb);
		Nfree_pages_exact_nullify(err_seg_info_alloc_jmdc_free, seg_info->clj.jmdc, jmdc_size);
		Nfree_pages_exact_nullify(err_seg_info_alloc_ent_md_free, seg_info->clj.ent_md, ent_md_sz);
		htr_kfree(seg_info->cmd.iocmd);
		htr_kfree(seg_info->free_ents_comp.ents);
		if (seg_info->free_ents_comp.ents_enc_buf)
			nvmeib_release(&seg_info->free_ents_comp.ents_enc_ai, seg_info->free_ents_comp.ents_enc_buf, dp_recovery_hot);
		rv = -1;
	} else {
		seg_info->free_ents_comp.ents_enc_buf_sz = seg_info->free_ents_comp.ents_enc_ai.n << PAGE_SHIFT;
		seg_info->clj.jmdc_size = jmdc_size;
		seg_info->clj.ent_md_sz = ent_md_sz;
		seg_info->cmd.iocmd->disk_cmd.owner = &seg_info->cmd;
		seg_info->cmd.gen_cmd->disk_cmd.owner = seg_info->cmd.iocmd;
		seg_info->cmd.iocmd->orig = NVMEIBC_DISK_IO_CMD_ORIG_RECOV;
	}
	return rv;
}

static void seg_info_free(struct seg_info *seg_info)
{
	u32 i;
	NFIN;
	__nvmeib_put_ndb(&seg_info->ndb); // Todo: use seg_info->cmd.iocmd->reqs1.ndb
	for(i=0; i<NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY; i++) {
		Nfree_pages_nullify(error_dp_ec_recov_hot_seg_info_free, seg_info->dblk.data_addr[i], NVMEBC_PAGES_ORDER_BLOCK);
		Nfree_pages_nullify(error_5_dp_ec_recov_hot_seg_info_free, seg_info->dblk.md[i], get_order(DISK_MAX_MD_SIZE_BYTE));
	}
	Nfree_pages_exact_nullify(error_1_dp_ec_recov_hot_seg_info_free, seg_info->clj.jmdc, seg_info->clj.jmdc_size);
	Nfree_pages_exact_nullify(error_4_dp_ec_recov_hot_seg_info_free, seg_info->clj.ent_md, seg_info->clj.ent_md_sz);
	Nkfree_nullify(error_2_dp_ec_recov_hot_seg_info_free, seg_info->cmd.iocmd);
	Nkfree_nullify(error_3_dp_ec_recov_hot_seg_info_free, seg_info->cmd.gen_cmd);
	Nkfree_nullify(error_6_dp_ec_recov_hot_seg_info_free, seg_info->free_ents_comp.ents);
	if (seg_info->free_ents_comp.ents_enc_buf) {
		nvmeib_release(&seg_info->free_ents_comp.ents_enc_ai, seg_info->free_ents_comp.ents_enc_buf, dp_recovery_hot);
		seg_info->free_ents_comp.ents_enc_buf = NULL;
	}
	NFOUT;
}

static void htr_build_checks(void)
{
	BUILD_BUG_ON(
		sizeof_field(struct volume_client_gen_req_blkset_recovered_base, ds_uuid) !=
		sizeof_field(struct nvmeibc_disk_segment, uuid));
}

/* Should be called from htr_init and before handling each slice */
static void htr_init_per_slice_info(struct per_slice_info *info)
{
	info->data_committed_on_rw_seg = false;
	info->data_written_bm = 0;
	info->is_rollback = false;
	info->is_neverwritten_slice = false;
	info->txid_for_regen = 0;
	info->found_real_tx = false;
	info->pre_slice_dbits.is_initialized = false;
	info->new_dbits.is_initialized = false;
}

static int htr_init(struct htr_ctx *h)
{
	struct seg_info dummy = {0};
	const struct nvmeibc_raid1 *r1 = h->params.raid1;
	int i, n_rw, rv = -ENOMEM;
	const gfp_t gfp = nvmeibc_dp_get_allow_io_gfp_flags();
	NFIN;

	htr_build_checks();
	//if not using kth per htr, alloc task obj

	h->op = h_op_invalid;
	h->n_segs = r1->replicas;
	h->tx_slba = HTR_INVALID_LBA;
	h->owner_si = get_owner_seg_slice_start(r1, h->params.rlba);
	h->rlba_parities_bm = nvmeibc_raid1_get_roles_bmp(r1, h->owner_si, pari_sgmnts);
	htr_init_per_slice_info(&h->cur_slice_info);
	h->sts_str[0] = '\0';
	n_rw = hweight32(nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable));
	for (i = 0; i < N_MAX_RAID_SLICE_LEN; i++)
		h->tx_jentries[i].offset = JENT_UNINITIALIZED_OFFSET;
	h->jour_committed = false;
	h->jour_pivot = HTR_INVALID_SEG_ID;
	h->binje = HTR_INVALID_N_SLICE_JOUR;
	h->snake_size = HTR_INVALID_N_SLICE_JOUR;
	h->cur_slice = 0;

	/* paranoid */
	if (n_rw < r1->slice_size) {
		_NTh(trace_dp_ec_recov_hot_htr_init, h, "Insufficient RW segs (@N_RW of @SLICE_SIZE)", n_rw, r1->slice_size);
		goto out;
	}

	if (!(h->seg_info = htr_kcalloc(h->n_segs, sizeof(*h->seg_info), gfp))) {
		_NTh(trace_1_dp_ec_recov_hot_htr_init, h, "Fail to alloc");
		goto out;
	}

	for (i = 0; i < h->n_segs; i++) {
		const int jmdc_size = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE * sizeof(*dummy.clj.jmdc); 	//4096 * 8B -> 32KB
		const int ent_md_size = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE * sizeof(*dummy.clj.ent_md);	//4096 * 1B -> 4KB
		if (seg_info_alloc(&h->seg_info[i], jmdc_size, ent_md_size, HTR_MAX_FREE_ENTS)) {
			_NEh(error_dp_ec_recov_hot_htr_init, h, DMESG_PREFIX() ": Failed to alloc seg_info=@SI", i);
			goto err;
		}
		/* init */
		h->seg_info[i].si = i;
		h->tx_jentries[i].is_valid = false;
		h->seg_info[i].dblk.edic_sts = HTR_INVALID_EDIC_STS;
	}
	if (!(h->regen_vec = htr_kcalloc(h->n_segs, sizeof(*h->regen_vec), gfp))) {
		_NTh(trace_2_dp_ec_recov_hot_htr_init, h, "Fail to alloc");
		goto err;
	}

	if (!(h->crc_vec = htr_kcalloc(h->n_segs, sizeof(*h->crc_vec), gfp))) {
		_NTh(trace_3_dp_ec_recov_hot_htr_init, h, "Fail to alloc");
		goto err;
	}

	//if not using kth per htr, add self to kth tasks list

	rv = 0;
	goto out;

err:
	NHTR_STS(trace_4_dp_ec_recov_hot_htr_init, h, "Fail htr-init, FATAL");
	htr_kfree(h->regen_vec);
	h->regen_vec = NULL;
	while (--i >= 0)
		seg_info_free(&h->seg_info[i]);
	htr_kfree(h->seg_info);
	h->seg_info = NULL;

out:
	NFOUT;
	return rv;
}

static void htr_exit(struct htr_ctx *h)
{
	int i;
	NFIN;

	//if not using kth per htr, remove self from kth tasks list
	//(ignore future zombie completions)

	//remove kth

	if (h->seg_info) {
		for (i = 0; i < h->n_segs; i++)
			seg_info_free(&h->seg_info[i]);
		htr_kfree(h->seg_info);
		h->seg_info = NULL;
	}
	if (h->regen_vec) {
		htr_kfree(h->regen_vec);
		h->regen_vec = NULL;
	}

	if (h->crc_vec) {
		htr_kfree(h->crc_vec);
		h->crc_vec = NULL;
	}

	//if not using kth per htr, free task obj

	NFOUT;
}

static void __extern_sm_finished(struct recovery_sync_op *so)
{	// This function is identical to dp_ec_sync_stale_cb_stg_end(), Todo: unite them both
	struct htr_ctx *h = so->user_ptr;
	add_htr_op_comp_event(h, so->cmds);//NULL);	/* NULL coz: so->cmds[i] != h->seg_info[i].cmd */
}

static int __extern_sm_execute_and_wait(struct htr_ctx *h, enum nvmeib_block_io_op op)
{
	struct recovery_sync_op *so = h->so;
	int rv;
	h->op = h_op_extern_sm;
	h->wip_bm = 0x7;			// As if waiting for something
	nvmeibcbdpec_push_sm_to_stack(so, __extern_sm_finished);
	so->o->op = op;
	so->stage = sync_stage_mutate_done;	// Why? Because all pre mutation stuff was allready executed by the caller.
	HTR_STATS_INC(n_ext_sm);
	h->n_ext_sm++;
	BLKCMP_SO_ASYNC_RESUME_CMP(dp_ec_sync_execute_op(so));
	if (unlikely(wait_all_wip(h) < 0)) {
		WARN(true, "nvmeibc bug, mem-corruption bmp=0x%x\n", h->wip_bm);
	}
	h->op = h_op_invalid;
	rv = so->error;
	so->error = 0;			// HTR will decide which error to propagate
	return rv;
}

// Regenerate non-readable segs and turnon on D segs (Rollback all non-readable segs)
static int htr_rollback_deg_segs(struct htr_ctx *h)
{
	roles_bmp_t d_p_bm = ~0;
	roles_bmp_t w_p_bm = ~0;
	int rv = -1;
	union nvmeibc_dbits_entry blkset_dbits;
	NFIN;

	/* sanity */
	if (has_rw_parity(h)) {
		_NTh(warn_dp_ec_recov_hot_htr_rollback_deg_segs, h, "Oops, here but have RW parity");
		WARN_ON_ONCE(1);
		goto out;
	}

	d_p_bm = (nvmeibc_raid1_get_parities_bmp(h->params.raid1) & nvmeibc_raid1_get_roles_bmp(h->params.raid1, h->owner_si, dead));
	w_p_bm = (nvmeibc_raid1_get_parities_bmp(h->params.raid1) & nvmeibc_raid1_get_roles_bmp(h->params.raid1, h->owner_si, w));

	blkset_dbits.all_bits = h->params.lock_ent.blkset_info.bits.dirty;
	WARN(nvmeibc_dbits_get_n_unk(&blkset_dbits, nvmeibc_raid1_get_protect_lvl(h->params.raid1)) != 0,
		 "nvmeibc bug, dbits=0x%x\n", blkset_dbits.all_bits);	// Should have resolved them earlier
	h->so->nwhole_params = no_writehole_params_default;
	h->so->nwhole_params.dbits_turnon_bmp = d_p_bm;
	h->so->nwhole_params.force_rebuild_bmp = w_p_bm;

	if (dp_ec_can_fix_dbits(h->so->cmds))
		HTR_STATS_INC(n_dbits_rebuild);

	rv = __extern_sm_execute_and_wait(h, NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK);
	if (rv < 0)
		goto out;

	if (w_p_bm)
		HTR_STATS_INC(n_regen_bkw_no_pari);

	if (d_p_bm)
		HTR_STATS_INC(n_dbits_turnon_no_pari);


	rv = 0;
out:
	NFOUT;
	return rv;
}

/* Binfo steps:
 0. TxID is always OK, Caller will syncronize it to all locks copies
 1. HTR calced dbits turn on + off, possibly wrote into slice md
 2. If we have dbits turn on, must commit them (treat dbits as data in roll-forward)
 3. If can turn off dbit need to call dbits sync

 Stale recovery makes a decision based on all locks involved; If the stale lock is copy and will be dead on next topology & IO, we may not call hot recovery, thus dirty bits in RAM will reflect better state then actually.
*/
static int __htr_turn_on_ram_dbits(struct htr_ctx *h)
{
	struct recovery_sync_op *so = h->so;
	int rv = 0;
	if ((h->cur_slice_info.found_real_tx) && (nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable) != nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, raid.all))) {
		so->cmds->rld.post.bits.dirty = h->cur_slice_info.new_dbits.binfo.post.all_bits;
		if (nvmeibc_dbits_has_turn_on(&h->cur_slice_info.new_dbits.binfo)) {	// Commit only turn on, before calling to dbits rebuild that will turn something off
			mark_blockset_info_not_written(so);
			rv = __extern_sm_execute_and_wait(h, NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO);
			if (rv)
				_NTh(trace_dp_ec_recov_hot_htr_turn_on_ram_dbits, h, "Commit dbit turn-on (in binfo) failed");
			else // Must not be set after commit binfo completes successfully
				WARN_ON(should_blockset_info_commit(so));
		}
		if (rv == 0)
			nvmeibcbdpec_inject_binfo_back_to_caller(so);
	}
	return rv;
}

/* For now just doing only Dbits rebuild and continue scrubbing (scrubber that found stale must return to scrub the blockset) */
static int __solve_no_writehole_problem(struct htr_ctx *h)
{
	struct recovery_sync_op *so = h->so;
	const bool has_degraded_seg = (nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable) != nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, raid.all));
	const bool should_postpone = (h->params.is_cold || h->params.is_hot_jgc); 	// In cold recovery, postpone the dbits fix to enable IO faster
	const bool can_fix_dbits = (has_degraded_seg && dp_ec_can_fix_dbits(so->cmds));
	enum nvmeib_block_io_op op = NVMEIB_BLOCK_IO_OP_NOP;						// Default, do nothing
	int rv = 0;

	if (so->nwhole_params.must_scrub) {
		op = NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING;
		WARN_ON(has_degraded_seg);					// Todo: Currently scrubbing does nto support degrade mode.
	} else if (can_fix_dbits && !should_postpone) {
		op = NVMEIB_BLOCK_IO_OP_RECOVER_DB;
	}

	if (op != NVMEIB_BLOCK_IO_OP_NOP) {
		WARN(!has_rw_parity(h), "nvmeibc bug - no RW parity and calling no_w_hole sync op=%d\n", op); // when no RW pari we rollback in different flow
		HTR_STATS_INC(n_dbits_rebuild);
		rv = __extern_sm_execute_and_wait(h, op);
		if ((can_fix_dbits)&&(rv == 0))
			WARN_ON(!should_blockset_info_commit(so)); // Successfull DB sync must set commit binfo if we could clear DBs
	}
	return rv;
}

static int hot_jgc_read_jmdcs(struct htr_ctx *h)
{
	int rv = -1;
	const ulong topo_nondead = nvmeibc_raid1_get_inverse_sgmnts_bmp(h->params.raid1, dead);
	NFIN;

	_NTh(trace_dp_ec_recov_hot_hot_jgc_read_jmdcs, h, "Read jmdc of all non-D segs");

	if (call_for_bitmap(h, topo_nondead, read_jmdc, H_ON_ERR_BREAK)) {
		NHTR_STS(trace_1_dp_ec_recov_hot_hot_jgc_read_jmdcs, h, "Failed to read one of the jmdcs for hot JGC, FATAL");
		goto out;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}

static int hot_jgc_scan_jmdcs(struct htr_ctx *h)
{
	int si, rv = -1;
	const ulong topo_nondead = nvmeibc_raid1_get_inverse_sgmnts_bmp(h->params.raid1, dead);
	NFIN;

	for_each_set_bit(si, &topo_nondead, h->params.raid1->replicas) {
		if ((rv = scan_jmdc(h, si, false)) < 0) {
			if (rv == -ENOENT) {
				_NDh(trace_dp_ec_recov_hot_hot_jgc_scan_jmdcs, h, "Tx not journalled in seg, si=@SI", si);
			} else {
				NHTR_STS(trace_2_dp_ec_recov_hot_hot_jgc_scan_jmdcs, h, "Fail to scan jmdc for hot JGC, FATAL");
				goto out;
			}

		}
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}

static inline void htr_status_per_slice(struct htr_ctx *h)
{
	_NTh(t_01_htr_ps, h, "(@HTR_CTX, cuuid=@CLIENT_UUID) - END: sts=@STS, is_cold=@BOOL_YN, is_jgc=@BOOL_YN", h, &h->params.recoveree_cuuid, h->sts_str, h->params.is_cold, h->params.is_hot_jgc);
	_NTh(t_02_htr_ps, h, "cur_slice=@INT, rlba: owner_si=@OWNER_SI, parities=[@BITMAP], rw-pari=@BOOL_YN, pre_db: v=@BOOL_YN, val={s=@DBITS}, ",
	h->cur_slice, h->owner_si, h->rlba_parities_bm, has_rw_parity(h),
	h->cur_slice_info.pre_slice_dbits.is_initialized,
	h->cur_slice_info.pre_slice_dbits.dbits.all_bits);

	_NTh(t_03_htr_ps, h, "pivot: si=@SI, txbm(ror)=[@TXBM], tx_slice=@TX_SLICE, jour: committed=@BOOL_YN, txbm&topo: RW=[@BITMAP], W=[@BITMAP], D=[@BITMAP]",
		h->jour_pivot, h->tx_jentries[h->jour_pivot == HTR_INVALID_SEG_ID ? 0 : h->jour_pivot].md_arr[h->cur_slice].tx_bmp, h->tx_slba & (~LOCKSET_4KS_MASK),
		!!h->jour_committed, h->txbm_topo.rw, h->txbm_topo.w, h->txbm_topo.d);

	_NTh(t_04_htr_ps, h, "data: committed=@BOOL_YN, match-bm=[@BITMAP], found_tx=@BOOL_YN pmd: v=@BOOL_YN, val={s=@DBITS,b=@DBITS}, n_ext_sm=@N_EXT_SM",
		!!h->cur_slice_info.data_committed_on_rw_seg, h->cur_slice_info.data_written_bm, !!h->cur_slice_info.found_real_tx,
		h->cur_slice_info.new_dbits.is_initialized, h->cur_slice_info.new_dbits.slice_after_turnoff.post.all_bits, h->cur_slice_info.new_dbits.binfo.post.all_bits,
		h->n_ext_sm);
}

static inline void htr_status(struct htr_ctx *h)
{
	_NTh(t_01_htr, h, "(@HTR_CTX, cuuid=@CLIENT_UUID) - END: sts=@STS, ", h, &h->params.recoveree_cuuid, h->sts_str);
	_NTh(t_02_htr, h, "topo: RW=[@BITMAP], W=[@BITMAP], D=[@BITMAP], lock_ent: @CALCULATED_DATA_LLONG",
		nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, readable),
		nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, w),
		nvmeibc_raid1_get_sgmnts_bmp(h->params.raid1, dead),
		h->params.lock_ent.all);

	_NTh(t_03_htr, h, "is_cold=@BOOL_YN, is_hot_jgc=@BOOL_YN",
		h->params.is_cold, h->params.is_hot_jgc);
}

static inline void __print_execution_plan(struct htr_ctx *h) {
	_NTh(t_06_htr, h, "(@HTR_CTX, jour_committed=@BOOL_YN, data_committed_on_rw_seg=@BOOL_YN, found_real_tx=@BOOL_YN is_rollback=@BOOL_YN",
		h, h->jour_committed, h->cur_slice_info.data_committed_on_rw_seg, h->cur_slice_info.found_real_tx, h->cur_slice_info.is_rollback);
}

static int htr_main_per_slice(struct htr_ctx *h)
{
	int rv = -1;
	NFIN;

	/* Check data committed on current slice */
	if (htr_check_data_committed(h) < 0)
		goto out;

	__print_execution_plan(h);

	/* Turn on RAM Dbits */
	if (__htr_turn_on_ram_dbits(h) < 0)
		goto out;

	if (h->cur_slice_info.data_committed_on_rw_seg)
		if (roll_fwd(h) < 0) // roll-fwd R/W segs.
			goto out;

	/* For Topo-W segs within TxBM, regenerate Data and MD on disk */
	if (htr_w_segs(h) < 0)
		goto out;

	/* For parity segs within TxBM, update Parity-MD. Dbits turnoff on disk (turnoff barrier). Today it also turn on dirty bits on roll-backwards (jour commited and data not) there should be a turn on barrier in the future*/
	if (htr_update_parity_segs_md(h) < 0)
		goto out;

	rv = 0;
out:
	NFOUT;
	return rv;
}

static int htr_main(struct htr_ctx *h)
{
	int rv = -1, i;
	//const struct nvmeibc_subscription_ctx *tr;
	NFIN;

	if (htr_init(h) < 0)
		goto out;

	_NTh(trace_dp_ec_recov_hot_htr_main, h, "(@HTR_CTX, cuuid=@CLIENT_UUID, lock_ent: @CALCULATED_DATA_LLONG, is_cold=@BOOL_YN, is_jgc=@BOOL_YN) - START",
		h, &h->params.recoveree_cuuid, h->params.lock_ent.all, h->params.is_cold, h->params.is_hot_jgc);

	if (h->params.is_hot_jgc) {
		if (hot_jgc_read_jmdcs(h) < 0)
			goto out;

		if (hot_jgc_scan_jmdcs(h) < 0)
			goto out;

		goto done;
	}

	//tr = h->so->r1->segments->toma_reg;

	/* If no RW parity seg, slice is consistent.
	   Regenerate non-readable segs and turnon on D segs (Rollback all non-readable segs) and Drop Tx */
	if (!has_rw_parity(h)) {
		// Didn't search for TxBM so assume worst (a full slice)
		init_txbm_topo_from_extern_txbm(h, (1 << h->params.raid1->slice_size) - 1);

		if (!h->params.is_cold) {  // In cold the rolback will be done only by turning on dbits on RAM
			if (calc_jour_committed_bmp(h) < 0)
				goto out;

			if (htr_rollback_deg_segs(h) < 0)
				goto out;
		}

		goto done;
	}

	if (!h->params.is_cold) {
		/* For Topo-RW segs within TxBM, Roll-Fwd or Drop transaction */
		if (htr_check_journal_committed(h) < 0)
			goto out;

		/* If journal stage had NOT completed, IO had no affect on slice. */
		if (!h->jour_committed)
			goto done;
	} else {
		__import_cold_candidate_to_htr(h);
	}

	for (i = 0; i < h->tx_height; i++) {
		_NTh(trace_2_dp_ec_recov_hot_htr_main, h, "Handling current slice (@INT)", i);

		h->cur_slice = i;
		if (h->jour_pivot != HTR_INVALID_SEG_ID)
			init_txbm_topo_from_extern_txbm(h, h->tx_jentries[h->jour_pivot].md_arr[i].tx_bmp);
		htr_init_per_slice_info(&h->cur_slice_info);

		if (htr_main_per_slice(h) < 0)
			goto out;

		if (!!h->txbm_topo.d) {
			if (h->cur_slice_info.is_rollback) {   // Full journal & no data, possible roll backwards (new data exists but not accessible to us)
				HTR_STATS_INC(n_roll_bkw_by_dbits_turnon); // Lazy roll-bkw via dirtybit turn on
			} else if (h->cur_slice_info.data_committed_on_rw_seg) {
				HTR_STATS_INC(n_roll_fwd_by_dbits_turnon); // Lazy roll-fwd via dirtybit turn on
			}
		}

		htr_status_per_slice(h);
	}

done:
	if (__solve_no_writehole_problem(h) < 0)
		goto out;

	h->so->should_send_msg_blckst_recovrd = false; // HTR will do that
	if (!h->params.is_cold) {	// Cold-recov doesnt notify Toma. Serjios get different API
		// TODO: Send blkset recovered can be called during unlock SM, use TxBM as it's input (instead of bool), also JIDX is missing in the following case
		// if (h->seg_info[si].jidx != HTR_INVALID_JMD_ID) we must call it now, otherwise mark TxBM as input to unlock SM and will send once binfo is committed
		h->so->should_abandon_on_error = true;
		if (htr_send_recovered(h) < 0) {
			goto out;
		}
	}

	NHTR_STS(trace_1_dp_ec_recov_hot_htr_main, h, "OK");
	rv = 0;

out:
	_NTh(trace_3_dp_ec_recov_hot_htr_main, h, "(cuuid=@CLIENT_UUID) - END: rv=@RV, j=@JOUR_COMMITTED_INT, d=@DATA_COMMITTED_ON_RW_SEG",
		&h->params.recoveree_cuuid, rv, !!h->jour_committed,
		!!h->cur_slice_info.data_committed_on_rw_seg);
	htr_status(h);
	htr_exit(h);

	NFOUT;
	return rv;
}
/* -------------------------------------------------------------------------- */
/*                                 htr-kth                                    */
/* -------------------------------------------------------------------------- */
/* Block layer triggers hot-recovery as follows:
 *
 *  1. Add htr-start event to volume-kth.
 *     This is done by EC's dp->sync_execute_op(), which may run in intr-ctx.
 *
 *  2. The volume's htr-start event handler:
 *     allocate htr container, starts its kth and link it to volume.
 *
 *  3. The htr-kth's run() func execute the hot-recovery procedure.
 *
 *  4. The htr-kth's done() func:
 *     - Callback the block-layer completion in order to resume the EC sync SM.
 *     - Add htr-done event to volume-kth.
 *
 *  5. The volume's htr-done event handler:
 *     unlink htr-kth from volume, free the kth and the htr container.
 */

struct nvmeibc_block_dp_ec_recov_hot {
	struct nvmeib_public_kth_obj base;
	struct recovery_sync_op *so;
	uuid_be uuid;
	/* parent kth */
	struct nvmeib_public_kth_id volume_kth;
	/* wait start */
	struct completion start;
	const struct nvmeibc_cinst_params_main *pmain_kth_hack;	// Todo: Remove me. Exists due to kth on_done hack
};

struct volume_htr_start_event {
	union {
		struct nvmeib_public_kth_event _e;					// Launch via kth
		struct work_struct             work;				// Launch via wq
	};
	struct recovery_sync_op *so;
	uuid_be uuid;
};

struct volume_htr_done_event {
	union {
		struct nvmeib_public_kth_event _e;					// Launch via kth
		struct work_struct             work;				// Launch via wq
	};
	struct nvmeibc_block_dp_ec_recov_hot *htr;
};

#define USE_HTR_ON_SYSTEM_WQ (1)
#if USE_HTR_ON_SYSTEM_WQ
	#define __HTR_KTH_ADAPTOR_RV_TYPE void
	#define __HTR_KTH_ADAPTOR_RETURN  return
#else
	#define __HTR_KTH_ADAPTOR_RV_TYPE int
	#define __HTR_KTH_ADAPTOR_RETURN  return 0
#endif

static int nvmeibc_block_dp_ec_recov_hot_start_kth(struct nvmeibc_block_dp_ec_recov_hot *htr);
static void call_so_callback(struct recovery_sync_op *so, int status)
{
	HTR_STATS_INC_SO(n_comps);
	so->error = status;
	if (so->error != 0) {
		HTR_STATS_INC_SO(n_comps_err);
	}
	BLKCMP_SO_ASYNC_RESUME_CMP(nvmeibcbdpec_return_to_caller_sm(so));
}

static __HTR_KTH_ADAPTOR_RV_TYPE htr_start_from_thread_context(struct work_struct *work)
{
	struct volume_htr_start_event *e = container_of(work, struct volume_htr_start_event, work);
	struct nvmeibc_block_dp_ec_recov_hot *htr;
	if (!(htr = htr_kzalloc(sizeof(*htr), GFP_NOFS))) {
		_NE(error_dp_ec_recov_hot_handle_ve_htr_start, DMESG_PREFIX("@DEV_NAME") ": Fail to alloc htr", e->so->o->nd->name);
		goto err;
	}
	htr->so = e->so;
	htr->uuid = e->uuid;
	htr->volume_kth = htr_base.kth;
	nvmeib_public_kth_obj_init(&htr->base);
	if (nvmeibc_block_dp_ec_recov_hot_start_kth(htr) < 0) {
		htr_kfree(htr);
		goto err;
	}
	goto out;
err:
	call_so_callback(e->so, -1);
out:
	htr_kfree(e);
	__HTR_KTH_ADAPTOR_RETURN;
}

static __HTR_KTH_ADAPTOR_RV_TYPE htr_hack_terminate(struct work_struct *work)
{
	struct volume_htr_done_event *e = container_of(work, struct volume_htr_done_event, work);
	struct nvmeibc_block_dp_ec_recov_hot *htr = e->htr;
	_NT(trace_dp_ec_recov_hot_handle_ve_htr_done, "Free htr-kth");
	nvmeib_public_kth_free(htr->base.kth);
	htr_kfree(htr);
	htr_kfree(e);
	__HTR_KTH_ADAPTOR_RETURN;
}

#if HTR_DEBUG
static int check_params(struct nvmeibc_block_dp_ec_recov_hot_params const * const params, struct recovery_sync_op *so)
{
	struct nvmeibc_raid1*r1 = params->raid1;
	u32 lock_txid;
	int non_rw, rv = -1;
	NFIN;

	if (!r1 || r1->replicas > HTR_REGEN_VEC_LEN) {
		_NT(trace_dp_ec_recov_hot_check_params, "Invalid inputs");
		goto out;
	}
	if (!nvmeib_uuid_cmp(params->recoveree_cuuid, NULL_UUID_BE)) {
		nvmeibcb_dp_io_fail_mgr_htr_null_uuids_err(&so->o->nd->dp.io_stats.mgr, so->o->nd);
		_NT(trace_1_dp_ec_recov_hot_check_params, "Null recoveree_cuuid");
		goto out;
	}

	non_rw = hweight_long(nvmeibc_raid1_get_inverse_sgmnts_bmp(r1, readable));
	if (non_rw > nvmeibc_raid1_get_protect_lvl(r1)) {
		_NT(trace_3_dp_ec_recov_hot_check_params, "#Non-RW segs > Protection-Level @RV", non_rw);
		goto out;
	}

	lock_txid = params->lock_ent.blkset_info.bits.txid;
	if (lock_txid == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS) {
		_NT(trace_4_dp_ec_recov_hot_check_params, "Invalid TxID @TXID", lock_txid);
		goto out;
	}

	//redundant as long we have the lockset-aligned check
	if (params->rlba % r1->slice_size) {
		_NT(trace_5_dp_ec_recov_hot_check_params, "@RLBA not slice-aligned (@SLICE_SIZE)", params->rlba, r1->slice_size);
		goto out;
	}

	if (params->rlba % (LOCKSET_4KS * r1->slice_size)) {
		_NT(trace_6_dp_ec_recov_hot_check_params, "@RLBA not lockset-aligned", params->rlba);
		goto out;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}
#endif

static inline void h_init_params_common(struct nvmeibc_block_dp_ec_recov_hot_params *hp, struct recovery_sync_op *so)
{
	WARN(so->start_slice, "nvmeibc bug: slice should be unknown. Not %d\n", so->start_slice);
	hp->raid1 = so->r1;
	hp->rlba = so->rlba;
}

static inline void h_init_params_hot(struct nvmeibc_block_dp_ec_recov_hot_params *hp, struct recovery_sync_op *so)
{
	struct nvmeibc_cmd_lock *l = &so->locks[0];
	struct nvmeibc_d_rdma_comp *dc = &l->comp;

	h_init_params_common(hp, so);

	hp->lock_ent.blkset_info.all = so->cmds->rld.pre.all;
	hp->lock_ent.lock_id = nvmeibc_d_rdma_comp_get_contending_id(dc);	// Daniel: This is true only coz there is no dual lock. The generic case should be not lock[0] but first lock that sync actually took (status == NCL_STATUS_TAKEN)
	hp->recoveree_cuuid = so->recoveree_cuuid;
	hp->recoveree_txid = hp->lock_ent.blkset_info.bits.txid;
	hp->is_cold = nvmeibcbdpec_has_rollfwd_jour_candidate(so);
}

extern uuid_be *nvmeibc_get_uuid(const struct nvmeibc_cinst_params_core *p); 	// Dont include entire nvmeibc_main.h api
static inline void h_init_params_hot_jgc(struct nvmeibc_block_dp_ec_recov_hot_params *hp, struct recovery_sync_op *so)
{
	const struct nvmeibc_cinst_params_core *p_core = nvmeibc_isnt_params_blk2core(nvmeibc_cinst_get_blok_p(so->o->nd));
	h_init_params_common(hp, so);
	hp->is_hot_jgc = true;
	hp->recoveree_cuuid = *nvmeibc_get_uuid(p_core);	// Recover self journals
	hp->recoveree_txid = so->orig_rldr->rld.pre.bits.txid + 1;	// The JAM collision to resolve (see dp_ec_journal_alloc_all_areas)
}

static inline int h_init_params(struct nvmeibc_block_dp_ec_recov_hot_params *hp, struct recovery_sync_op *so)
{
	NFIN;

	if (is_op_sync_hot_jgc(so->o->op))
		h_init_params_hot_jgc(hp, so);
	else
		h_init_params_hot(hp, so);

	NFOUT;
	return check_params(hp, so);
}


static int htr_run(void *arg)
{
	struct nvmeibc_block_dp_ec_recov_hot *htr = arg;
	int rv = -1;
	struct htr_ctx *h = NULL;
	const struct nvmeibc_block_device *nd = htr->so->o->nd;
	const gfp_t gfp = nvmeibc_dp_get_allow_io_gfp_flags();
	NFIN;

	//Todo: Once KTH is in, replace with kth-resume/resume-wait
	wait_for_completion(&htr->start);

	nvmeib_public_kth_switch_current_eq(&htr->base.events);
	if ((h = htr_kzalloc(sizeof(*h), gfp))) { //omril: integration pains
		h->htr_kth = htr->base.kth;
		h->so = htr->so;
		htr->so->user_ptr = h; 	// Needed for h->htr_kth.ptr
		if (!h_init_params(&h->params, htr->so)) {
			htr_set_name(h);
			rv = htr_main(h); // Here: entire sync runs
		}
	} else {
		_NE(error_dp_ec_recov_hot_htr_run, DMESG_PREFIX("@DEV_NAME") ": Fail to alloc htr", nd->name);
	}
	htr_kfree(h);
	htr->pmain_kth_hack = nvmeibc_cinst_get_blok_m(nd);
	call_so_callback(htr->so, rv);
	htr->so = NULL;						// For deubg only, prevent use after free
	NFOUT;
	return -1; //dont run again
}

static int htr_done(void *arg)
{
	struct nvmeibc_block_dp_ec_recov_hot *htr = arg;
	struct volume_htr_done_event *e;
	int rv = 0;
	if (!(e = htr_kzalloc(sizeof(*e), GFP_ATOMIC))) {
		rv = -1;
	} else {
		e->htr = htr;
		#if USE_HTR_ON_SYSTEM_WQ
			INIT_WORK(&e->work, htr_hack_terminate);
			schedule_work(&e->work);		// Not BLKCMP_ANY_schedule_work()! as cannot be called directly. IT has to wait for kth current thread to finish and free htr kth context
		#else
			_ND(trace_dp_ec_recov_hot_htr_done, "HTR: run on main wq");
			rv = nvmeibc_run_on_main_wq1(htr->pmain_kth_hack, htr_hack_terminate, (void*)&e->work);
		#endif
	}
	WARN(rv, "nvmeibc: out of memory, io will stuck");
	return 0;
}

/* Called from parent-kth's ctx */
static int nvmeibc_block_dp_ec_recov_hot_start_kth(
	struct nvmeibc_block_dp_ec_recov_hot *htr)
{
	char name[64];
	proc_name_t pname;
	struct nvmeib_public_kth_params params = {
		.name = name,
		.short_name = pname,
		.cpu = htr->so->o->cpu_id, // the original IO's CPU, or NR_CPUS when launched by a recovery
		.started = false,
		.is_main = true,
		.run = htr_run,
		.done = htr_done,
		.run_arg = htr,
		.done_arg = htr
	};
	struct nvmeib_public_kth_id kth;
	int rv;

	NFIN;
	clnt_proc_name_format(pname, 'C', "HR", "htr", nvmeibc_cinst_get_blok_inst_num(nvmeibc_cinst_get_blok_p(htr->so->o->nd)));
	init_completion(&htr->start);
	snprintf(name, sizeof(name), "HTR.%.*s", (int)NVMEIBC_BD_NAME_LEN, htr->so->o->nd->name);
	if (nvmeib_public_kth_is_err(
		(kth = nvmeib_public_kth_create(&params)))) {
		_NE(error_dp_ec_recov_hot_nvmeibc_block_dp_ec_recov_hot_start_kth, DMESG_PREFIX() ": Fail to create thread for htr @NAME", name);
		rv = -1;
	} else {
		htr->base.kth = kth;
		complete(&htr->start);	// allow htr_start() to run
		rv = 0;
	}
	NFOUT;
	return rv;
}

/********************** DP sync stale virtal functions ************************/
static void __dp_ec_sync_stale_execute_op(struct recovery_sync_op *so)
{
	int rv = -1;
	struct volume_htr_start_event *e;
	const struct nvmeibc_block_device *nd = so->o->nd;

	HTR_STATS_INC_SO(n_calls);
	if (!(e = htr_kzalloc(sizeof(*e), GFP_ATOMIC))) {
		_NE(error_dp_ec_recov_hot_dp_ec_sync_stale_execute_op, DMESG_PREFIX("@DEV_NAME") ": Fail to alloc event", nd->name);
	} else {
		e->so = so;
		nvmeib_public_uuid_gen(&e->uuid);
		#if USE_HTR_ON_SYSTEM_WQ
			INIT_WORK(&e->work, htr_start_from_thread_context);
			BLKCMP_ANY_schedule_work(&e->work);
			rv = 0;
		#else
			rv = nvmeibc_run_on_main_wq1(nvmeibc_cinst_get_blok_m(nd), htr_start_from_thread_context, (void*)&e->work);
			//if (rv) htr_kfree(e);
		#endif
	}

	if (unlikely(rv))
		call_so_callback(so, -ENOEXEC);
}

#ifdef BLKCMP_SO_COMPLETION_PRESERVE_STACK
	void dp_ec_sync_stale_execute_op(struct recovery_sync_op *so)		// Called from 'so' context
	{
		BLKCMP_SO_ASYNC_AWAIT(__dp_ec_sync_stale_execute_op(so));	// launches HTR in new fiber, and suspend current fiber
		while (nvmeibcbd_sync_stack_is_htr_asking_for_extern_sm(so)) {	// While loop, because HTR may ask help a few times
			struct recovery_sync_stack *st = &so->stack;				// 'so' woke up, HTR asked it to do its EC state machine
			BUG_ON((st->ret[st->sp-1] != __extern_sm_finished) || (so->stage != sync_stage_mutate_done));	// Sanity, HTR indeed asks for something
			dp_ec_sync_execute_op(so);									// Do the task
			BLKCMP_SO_ASYNC_AWAIT(__extern_sm_finished(so));		// Fiber sleep an wakeup HTR, to let it run
		} // else { Woke up because HTR finished }
		return BLKCMP_SO_ASYNC_RESUME_SND(nvmeibcbdpec_return_to_caller_sm(so));	// Note: Caller may be 'so' which is stale lock sync or cold recovery (so->stack may be non empty)
	}
#else
void dp_ec_sync_stale_execute_op(struct recovery_sync_op *so) { __dp_ec_sync_stale_execute_op(so); }
#endif

/* This is the only code in comp-ctx (hopefully) */
void dp_ec_sync_stale_cb_stg_end(struct nvmeibc_block_command *cmd)
{
	struct htr_ctx *h = cmd->o->rso->user_ptr;
	_NTh(trace_dp_ec_recov_hot_dp_ec_sync_stale_cb_stg_end, h, "Comp of cmd=@CMD_PTR, h=@HTR_CTX, ctx(kth)=@HTR_KTH_PTR", cmd, h, h->htr_kth.ptr);
	add_htr_op_comp_event(h, cmd);	// this is cmd: h->seg_info[i].cmd
}
