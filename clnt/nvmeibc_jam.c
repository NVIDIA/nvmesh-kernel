#define C_JAM_C

#include "nvmeibc_block.h"					// Must be first for simulator
#include "nvmeibc_jam.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_defs.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeib_utils.h"
#include "nvmeibc_icore_ops.h"
#include "nvmeibc_pausable.h"
#include "nvmeibc_main.h"
#include "module/instance/nvmeibc_cinst_params.h"
#include "core/nvmeibc_core_common.h"
#include "nvmeib_public_procfs.h"
#include "nvmeibc_ib_admin_channel.h"
#include "common/proc_epilog.h"
#include "nvmeib_public.h"
#include "nvmeib_pcpu_wq.h"
#include "nvmeib_cpu_masks.h"


/* The Jam is located between the pausable (PD) and transport layer.
 * The Jam APIs are called by the upper block layer (UL) which also
 * located above the PD layer.
 *
 * As such any operation on jam-disk must be PD protected.
 *
 * o For JAM APIs (alloc, free, abandon) called by the UL,
 *    the Jam acquires PD approval (inc refcnt) for disks vector and undo
 *    this (dec refcnt) on exit. The refcnt is not decremented on cmpletion
 *    in order not to prevent disks' pause throughout jam (async) allocation
 *    time.
 *
 *  o For other contexts other than JAM APIs,
 *    (such as send/recv completions, timer, jam-disk work)
 *    it shall be safe to access the jam-disk for processing of the completed
 *    operation but if a sub-sequent operation is needed (e.g resume of partial
 *    allocation, processing work), ctx must go through the PD layer before
 *    triggering this op.
 */

//https://excelero.atlassian.net/browse/EC-2945 - same @tokens for JRI & JRE

#define NJAM_FMT(_disk_, fmt, ...) \
		"JAM @FUNCTION; Disk @DISK_ID_STR (@JAM_DISK); Range @JRNL_RNG_IDX; GenID @JRNL_RNG_GEN_ID: " fmt, \
		__FUNCTION__, \
		(_disk_ ? _disk_->name : "<UNKNOWN>"), \
		_disk_, _disk_->jour.rng_id, _disk_->jour.rng_gen_id, \
		## __VA_ARGS__
#define _NDj(name, _disk_, fmt, ...) _ND(name, NJAM_FMT(_disk_, fmt, ## __VA_ARGS__))
#define _NTj(name, _disk_, fmt, ...) _NT(name, NJAM_FMT(_disk_, fmt, ## __VA_ARGS__))
#define _NIj(name, _disk_, fmt, ...) _NI(name, NJAM_FMT(_disk_, fmt, ## __VA_ARGS__))
#define _NWj(name, _disk_, fmt, ...) _NW(name, NJAM_FMT(_disk_, fmt, ## __VA_ARGS__))
#define _NEj(name, _disk_, fmt, ...) _NE(name, NJAM_FMT(_disk_, fmt, ## __VA_ARGS__))

#define JAM_LOG_METRICS(_name_, _disk_, fmt, ...) \
		NVMEIB_LOG_METRICS( \
				"JAM Disk @DISK_ID_STR (@JAM_DISK); Range @JRNL_RNG_IDX; GenID @JRNL_RNG_GEN_ID: " fmt, _I, tracer_nvmeibc, _name_, \
				(_disk_ ? _disk_->name : "<UNKNOWN>"), \
				_disk_, _disk_->jour.rng_id, _disk_->jour.rng_gen_id, \
				## __VA_ARGS__)

/* TODO:
 *
 * General:
 * ^^^^^^^
 * 1. search for 'omril [jam]:'
 * 2. Reduce JRI size back to 128
 * 3. Shall client send TTFN msg so srv can reset all free JAM entries?
 *    Normally these entries will be cleaned by JGC and Jam-cached free-bitmap.
 *
 * Block Layer:
 * ^^^^^^^^^^^
 * 1. Sort disks once on pRAID create.
 * 2. Generate a sparse matrix to that allow direct access to the disks that
 *    disk D shares pRAID with.
 *
 * Jam:
 * ^^^
 * 1. Replace hash with Bloom Filter.
 * 2. Dont kfree jam-disk on every disk-release, instead just "disable" it
 *    for jam use and remove it only when disk is deleted (last detach).
 *    This will allow getting rid of disk->jrc.
 *
 * Jam (nice-2-have):
 * ^^^^^^^^^^^^^^^^^
 * - Try to get rid of this jam_disk ptr to disk
 * - BUG_ONs: Replace with rv=ERR or JAM_BUG_ON
 * - Seperate file for jam-simu
 * - jidx_event_ to hint what jreq/s were waiting on jidx so we can save
 *   the bind-check on them. Resume is done in fifo order and as these jreq/s
 *   may not (always) be first in line, this may be insignificant improvement.
 */
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	#define NVMEIBC_PENDING_REQ_TIMEOUT (HZ / 1000)
	#define JAM_DEBUG_ABORT_ON_ERROR() 	BUG()
#else
	#define NVMEIBC_PENDING_REQ_TIMEOUT (HZ / 5) /* 200 milliseconds max., in total for all disks for a single request */
	#define JAM_DEBUG_ABORT_ON_ERROR()
#endif

#define DEBUG_JAM 1

ulong nvmeibc_jam_pending_req_timeout_jif = NVMEIBC_PENDING_REQ_TIMEOUT;
module_param_named(jam_pending_req_timeout_jif, nvmeibc_jam_pending_req_timeout_jif, ulong, 0644);
MODULE_PARM_DESC(jam_pending_req_timeout_jif, "Jam timeout for pending allocation request [jiffies], if 0 use system default ");

uint nvmeibc_jam_max_used_entries = 0;
module_param_named(jam_max_used_entries, nvmeibc_jam_max_used_entries, uint, 0644);
MODULE_PARM_DESC(jam_max_used_entries, "Jam max used journal-entries 1 based, if 0 use system default");

ulong nvmeibc_jam_non_free_entry_timeout = 300;
module_param_named(jam_non_free_entry_timeout, nvmeibc_jam_non_free_entry_timeout, ulong, 0644);
MODULE_PARM_DESC(jam_non_free_entry_timeout, "Jam timeout for jentry in non-free state [sec]");

#if DEBUG_JAM
#if defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1)
#define DEFAULT_JAM_LOG_METRICS_PERIOD 0
#else
#define DEFAULT_JAM_LOG_METRICS_PERIOD 2
#endif
ulong nvmeibc_jam_log_metrics_period = DEFAULT_JAM_LOG_METRICS_PERIOD;
module_param_named(jam_log_metrics_period, nvmeibc_jam_log_metrics_period, ulong, 0644);
MODULE_PARM_DESC(jam_log_metrics_period, "Jam periodic metrics logging period [sec]");
#endif

bool nvmeibc_jam_pending_enb = true;
module_param_named(jam_pending_enb, nvmeibc_jam_pending_enb, bool, 0644);
MODULE_PARM_DESC(jam_pending_enb, "Jam pending mode control switch");

bool nvmeibc_jam_use_system_pcpu_wq = false;
module_param_named(jam_use_system_pcpu_wq, nvmeibc_jam_use_system_pcpu_wq, bool, 0444);
MODULE_PARM_DESC(jam_use_system_pcpu_wq, "Jam use system pcpu wq for pending requests");

/*
This IDX will be used when skipping ec locks as we are using the same TXID
So some validations can be skipped
*/
#define JAM_SKIPPED_JIDX 0

struct nvmeibc_jam_disk_percpu_cnts {
	u64 tot_alloc_ok;
};

extern uint nvmeibc_skip_lock_cmds_flags;

/* stats remain valid even after (forced) detch + does not require extra pd access */
struct nvmeibc_jam_percpu_cnts {
	u64 n_ulp_trans_alloc_reqs;
	u64 n_ulp_trans_alloc_ok;
	u64 n_ulp_trans_alloc_err;
	u64 n_ulp_trans_free_reqs;
	u64 n_ulp_trans_abnd_reqs;
	/* record n_disks per ulp's request (pre pd layer) ;
	   e.g. in case ulp ask X allocs but returns <X */
	u64 n_ulp_jents_alloc_ok;
	u64 n_ulp_jents_free;			/* includes jents we decide to erase */
	u64 n_ulp_jents_abnd;

	u64 n_jam_jents_alloc; 			/* updated one disk at a time */
	u64 n_jam_jents_rollback;		/* updated one disk at a time */
	u64 n_jam_jents_alloc_ok;		/* updated on alloc success (+= n_disks) */
	u64 n_jam_jents_free; 			/* updated one disk at a time */
	u64 n_jam_jents_erase;			/* updated one disk at a time */
	u64 n_jam_jents_erase_comp;	 	/* updated one disk at a time */
	u64 n_jam_jents_abnd;			/* updated one disk at a time */
	u64 n_jam_jents_a2f;			/* updated one disk at a time */
	u64 n_jam_jents_cancel_alloced;	/* updated on jam-del */
	u64 n_jam_jents_cancel_pending;	/* updated on jam-del */
};

#define cdisk2cj(__c_disk) __get_c_jam(nvmeibc_cinst_get_core_p(__c_disk))
#define jdisk2cj(__j_disk) cdisk2cj(__j_disk->disk)

#if defined(__KERNEL__)
	#define jam_cnts_add(__j, __name, __val) do {\
		struct nvmeibc_jam_percpu_cnts *__p;\
		unsigned long __f;\
		local_irq_save(__f);\
		__p = this_cpu_ptr(__j->pcpu_cnts); \
		__p->__name += __val; \
		local_irq_restore(__f);\
	} while (0)

	#define jam_disk_cnts_add(__jdisk, __name, __val) do {\
		struct nvmeibc_jam_disk_percpu_cnts *__p;\
		unsigned long __f;\
		local_irq_save(__f);\
		__p = this_cpu_ptr(__jdisk->pcpu_cnts); \
		__p->__name += __val; \
		local_irq_restore(__f);\
	} while (0)
#else
	#define jam_cnts_add(__j, __name, __val) 			((void)__j)
	#define jam_disk_cnts_add(__jdisk, __name, __val) 	((void)__jdisk)
#endif
#define jam_cnts_inc(__j, __name) jam_cnts_add(__j, __name, 1)
#define jam_disk_cnts_inc(__jdisk, __name) jam_disk_cnts_add(__jdisk, __name, 1)

#define	jam_cnts_on_ulp_req_alloc(__j, __n_disks) 			\
do {														\
	if (__n_disks) {										\
		jam_cnts_inc(__j, n_ulp_trans_alloc_reqs); 			\
	} 														\
} while (0)

/* This macro is called under pd protection for ALL disks from lbas-alloc() */
#define jam_cnts_on_alloc_ok(__n_disks, __sorted) 			\
do {														\
	struct nvmeibc_jam *__j = cdisk2cj(__sorted[0].disk);	\
	int __i; 												\
		for (__i = 0; __i < __n_disks; __i++) {				\
			jam_cnts_on_jent_alloc_ok(__sorted[__i].disk->jam_disk);	\
		}													\
		jam_cnts_add(__j, n_jam_jents_alloc_ok, __n_disks);	\
		jam_cnts_add(__j, n_ulp_jents_alloc_ok, __n_disks);	\
		jam_cnts_inc(__j, n_ulp_trans_alloc_ok);			\
} while (0)

#define jam_cnts_on_alloc_err(__j) 							\
do {														\
		jam_cnts_inc(__j, n_ulp_trans_alloc_err);			\
} while (0)

#define	jam_cnts_on_ulp_req_free(__j, __n_disks) 			\
do {														\
	if (__n_disks) {										\
		jam_cnts_inc(__j, n_ulp_trans_free_reqs); 			\
		jam_cnts_add(__j, n_ulp_jents_free, __n_disks); 	\
	} 														\
} while (0)

#define	jam_cnts_on_ulp_req_abnd(__j)						\
do {														\
		jam_cnts_inc(__j, n_ulp_trans_abnd_reqs); 			\
		jam_cnts_inc(__j, n_ulp_jents_abnd); 				\
} while (0)

#define	jam_cnts_on_jent_to_alloc(__jdisk)			 		\
do {														\
		__jdisk->tot_alloc++;								\
		jam_cnts_inc(jdisk2cj(__jdisk), n_jam_jents_alloc);	\
} while (0)

#define	jam_cnts_on_jent_to_rollback(__jdisk)				\
do {														\
		__jdisk->tot_rollback++;							\
		jam_cnts_inc(jdisk2cj(__jdisk), n_jam_jents_rollback); \
} while (0)

#define	jam_cnts_on_jent_to_free(__jdisk)			 		\
do {														\
		__jdisk->tot_free++;								\
		jam_cnts_inc(jdisk2cj(__jdisk), n_jam_jents_free); 	\
} while (0)

#define	jam_cnts_on_jent_alloc_ok(__jdisk)			 		\
do {														\
		jam_disk_cnts_inc(__jdisk, tot_alloc_ok);			\
} while (0)

#define	jam_cnts_on_jent_to_erase(__jdisk)			 		\
do {														\
		__jdisk->tot_erase++;								\
		jam_cnts_inc(jdisk2cj(__jdisk), n_jam_jents_erase); \
} while (0)

#define	jam_cnts_on_jent_to_erase_comp(__jdisk)			 	\
do {														\
		__jdisk->tot_erase_comp++;							\
		jam_cnts_inc(jdisk2cj(__jdisk), n_jam_jents_erase_comp); \
} while (0)

#define	jam_cnts_on_jent_to_abnd(__jdisk)			 		\
do {														\
		__jdisk->tot_abnd++;								\
		jam_cnts_inc(jdisk2cj(__jdisk), n_jam_jents_abnd); 	\
} while (0)

#define	jam_cnts_on_jent_to_a2f(__jdisk)			 		\
do {														\
		__jdisk->tot_a2f++;									\
		jam_cnts_inc(jdisk2cj(__jdisk), n_jam_jents_a2f); 	\
} while (0)

#define	jam_cnts_on_jent_to_canceled(__j, __state, __n)		\
do {														\
		jam_cnts_add(__j, n_jam_jents_cancel_##__state, __n);	\
} while (0)

/* Journal Allocation Manager */
struct nvmeibc_jam {
	spinlock_t lock;

	/* disks list */
	struct list_head disks;
	int n_disks;

	struct nvmeibc_jam_percpu_cnts __percpu *pcpu_cnts;
};

#define __get_c_jam(p) ((struct nvmeibc_jam *)__get_from_params_core_globals_container(p)->c_jam)

#define JAM_ENCODE_JOURNAL_LBA 1

//block layer has multi-slice IO tests, so few entries should be allocated
bool JAM_NON_AMBIGUOUS = 1;

#define NVMEIBC_JAM_CMD_TIMEOUT (3*HZ)
#define NVMEIBC_JAM_MAX_JIDX_RST_ATTEMPTS (8)
#define NVMEIBC_JAM_INVALID_BOUND_IDX (-1)

#define jam_lock(c_jam, flags) \
	do { spin_lock_irqsave(&c_jam->lock, flags); } while (0)
#define jam_unlock(c_jam, flags) \
	do { spin_unlock_irqrestore(&c_jam->lock, flags); } while (0)

#define NVMEIB_EC_JMDC_BITS_J2B (NVMEIB_EC_JMDC_BITS_J2D)  // The number of bits in MD to maintain pointer from the journal block to the data blockset

#define BITS_PER_NIBBLE 4
#define BITS_TO_NIBBLES(nr) DIV_ROUND_UP(nr, BITS_PER_NIBBLE)
#define HKEY_TXID_LEN (BITS_TO_NIBBLES(NVMEIB_EC_JMDC_BITS_TX_ID))
#define HKEY_J2B_LEN (BITS_TO_NIBBLES(NVMEIB_EC_JMDC_BITS_J2B))
#define U64_RAW_LEN (BITS_TO_NIBBLES((int)sizeof(u64)*BITS_PER_BYTE))

/* journal index's state */
enum nvmeibc_jam_jidx_state {
	NVMEIBC_JIDX_STS_FREE = 0xA0,
	NVMEIBC_JIDX_STS_ALLOCED,
	NVMEIBC_JIDX_STS_ERASING,
	NVMEIBC_JIDX_STS_ABANDONED
};


/* JAM unique hash is simmilar to jmdc entry (first 8 bytes on disk and in server memory), the diffrence is that is contains j2b instead of j2d
   The reason j2b is in use is inorder to make htr easier to implement so there will be no journal ambiguity in blockset
   j2b is the dlba[blocksets].*/
typedef union jidx_hkey__t {
	struct {
		u32 j2b 		: NVMEIB_EC_JMDC_BITS_J2B; 			// journal -> data's blockset pointer. Index of blockset on the disk. u32 -> enough for 4G*128KB = 512[TB] disk
		u32 tx_id		: NVMEIB_EC_JMDC_BITS_TX_ID;	  	// transaction id
		u32 reserved	: (32 - NVMEIB_EC_JMDC_BITS_TX_ID);	// transaction bitmap, , i.e.: which segs are written in transaction, excluding parities, relative to slice start (not to raid first segment)
	} __attribute__((packed));
	u64 raw;												// Size of journal entry meta-data (Uses the minimum to support drives that only support 4104 format)
}__attribute__((packed)) jidx_hkey_t;

/* j2b is the blockset lba from the beginning of the disk */
static inline u32 __j2d_to_j2b(const u64 dlba) {
	return ((u32)(dlba >> LOCKSET_SLICES_SHIFT));				// rv in u32 represents dlba of 37bits;
}

/* Value written to JMD/JMDC when a journal entry is in free state */
#define jidx_hkey_invalid         ((const jidx_hkey_t ){ .raw = nvmeib_jmd_unused_entry_val.raw })
/* Value used to indicate JMD/JMDC value is not relevant/available */
#define jidx_hkey_invalid_special ((const jidx_hkey_t ){ .raw = nvmeib_jmd_invalid_special_val.raw })

// JMDC entry not after format or explicitly set to invalid
static inline bool __is_jidx_hkey_io_entry(const jidx_hkey_t jidx_hkey)
{
	return nvmeib_is_jmd_io_entry_by_txid(jidx_hkey.tx_id);
}

struct jidx_hkeys {
	jidx_hkey_t committed;
	jidx_hkey_t transient;
};

#define jidx_hkeys_set_transient(__jidx, __hkey) do {	\
	__jidx->hkeys.transient = __hkey;					\
} while (0)

#define jidx_hkeys_commit_transient(__jidx) do {		\
	__jidx->hkeys.committed = __jidx->hkeys.transient;	\
	__jidx->hkeys.transient = jidx_hkey_invalid;		\
} while (0)

#define jidx_hkeys_reset_both(__jidx) do {				\
	__jidx->hkeys.committed = jidx_hkey_invalid;		\
	__jidx->hkeys.transient = jidx_hkey_invalid;		\
} while (0)

#define jidx_hkeys_reset_committed(__jidx) do {			\
	__jidx->hkeys.committed = jidx_hkey_invalid;		\
} while (0)

#define jidx_hkeys_reset_transient(__jidx) do {			\
	__jidx->hkeys.transient = jidx_hkey_invalid;		\
} while (0)

static inline void __jmdc_to_hkey(const struct jentry_md *jentry, jidx_hkey_t *hkey) {
	const union jblock_md *jmdc = &jentry->md_arr[0];
	hkey->j2b = __j2d_to_j2b(nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmdc));
	hkey->tx_id = jmdc->tx_id;
	hkey->reserved = 0;
}

struct waiting_jreq_record {
	/* @jreq that waits for jidx this struct is embedded in */
	struct nvmeibc_jam_pending_req *jreq;
	/* state jidx was in when @jreq was added to pending list, for debug */
	enum nvmeibc_jam_jidx_state on_add_state;
};

/* journal index */
struct nvmeibc_jam_jidx {
	int idx;
	enum nvmeibc_jam_jidx_state state;
	struct list_head pool_link;
	u8 gen_id;

	/* committed and transient keys */
	struct jidx_hkeys hkeys;
	/* hash-link of committed hkey */
	struct hlist_node uniqj2b_hlink;
	/* hash-link of transient hkey.
	   used in corner case where jlba write
	   completed with error, thus we cant
	   know what value is now committed */
	struct list_head uniqj2b_trans_link;

	/* link to jreq(s) bound to this jidx's
	   committed/transient hkey */
	struct nvmeibc_jam_pending_req *jreq_bound_via_committed;
	struct nvmeibc_jam_pending_req *jreq_bound_via_transient;

	u64 alloc_jif;
	u64 abnd_jif;
	u64 erase_jif;
	u8 n_erase_attempts;

	u64 state_chg_jif;

	/* A flag used for skipping validations when no locks mode */
	bool skipped;
};

/* journal range */
struct nvmeibc_jam_disk {
	int jidx_pool_size;
	int binje_shift;
	struct nvmeibc_jam_jidx *jidx_pool;
	spinlock_t lock;
	int locking_cpu;
	struct list_head free_list;
	bool alloc_enb;

	struct {
		int n_free;
		int n_abandoned;
	} pool_init;

	int n_alloced;
	int n_free;
	int n_abandoned;
	int n_erasing;

	/* mono inc counters */
	struct nvmeibc_jam_disk_percpu_cnts __percpu *pcpu_cnts;
	u64 tot_alloc;
	u64 tot_rollback;
	u64 tot_free;
	u64 tot_erase;
	u64 tot_erase_comp;
	u64 tot_abnd;
	u64 tot_a2f;

	u64 n_alloced_avg_sum;
	u64 n_alloced_avg_cnt;
	u64 jif_alloced_sum;
	u64 jif_alloced_cnt;
	u64 n_erasing_avg_sum;
	u64 n_erasing_avg_cnt;
	u64 jif_erasing_sum;
	u64 jif_erasing_cnt;
	u64 n_free_avg_sum;
	u64 n_free_avg_cnt;
	u64 n_pending_timeout;
	u64 n_pending_reason_empty_cnt;
	u64 n_pending_reason_max_used_cnt;
	u64 n_pending_reason_bound_cnt;
	u64 n_pending_bound_alloced_cnt;
	u64 n_pending_bound_abnded_cnt;
	u64 n_pending_bound_earsing_cnt;
	u64 n_pending_bound_unexp_cnt;
	u64 n_bound_to_transient_cnt;

	/* For APIs that dont have
	   disk as input (e.g. jam status) */
	struct list_head jam_link;

	/* Hash table for having only one jour-entry
	   pointing to the same data-block i.e the
	   {TxID, J2B} pair */
	DECLARE_HASHTABLE(uniqj2b_htable, 8);
	struct list_head uniqj2b_trans_list;

	/* Pending reqs rbtree (ordered by priority) */
	struct rb_root pending_root;
	int n_pending;

	/* wq for resume-work,
	   continues or abort
	   partial allocation */
	struct workq_struct *jwq;

	/* for debug, ensure no work
	   is added after setting this */
	atomic_t dying;

	struct nvmeibc_disk *disk;

	struct nvmeib_public_procfs_ent *proc_ent_status;

	u64 log_metrics_jif;
	u64 scan_non_free_jif;

	atomic_t deferred_jreqs_cnt;
	wait_queue_head_t deferred_jreqs;
};

struct nvmeibc_jam_pending_req {
	int n_disks;
	struct jalloc *sorted;
	u32 txid;
	bool wait_bound_abnd;

	/* result in input's order */
	u64 *res_jlbas;
	void *ctx;

	/* pos in @sorted to which
	   we wait for a free jidx */
	int curr;
	jidx_hkey_t hkey;

	/* Maintain the idx jreq is waiting on, if known,
	   for faster find_jreq_to_resume_()

	   - if pending was due to ambiguity prevention,
	     this is the index of the jidx which bounds @hkey i.e.
	     the jidx with either 'committed' or 'transient' (in
	     case the jidx was in ERASING state) hkey = @heky.

	   - if pending was due to list-empty,
	     set to @jidx_pool_size.

	   - otherwise, unexpected
	 */
	int bound_idx;

	struct rb_node pending_node;

	/* timeout timer */
	TIMER_LIST_INSTANCE(timeout_timer);
	bool timeout_expired;
	void *timeout_ctx;
	unsigned long pend_jif;	/* for debugging only */
	unsigned long deadline_jif;
	unsigned long priority;

	/* defer resume if called from
	   rollback-partial-allocation */
	atomic_t resume_wip;
	struct workqe_struct resume_work;
	
	/* only used for deferred resume */
	struct nvmeibc_jam_disk *jam_disk; 

	/* link to tmp (on-stack) list.
	   instead of overloading @pending_link,
	   (which signals to timeout-timer func
	   if jreq is queued or not) */
	struct list_head tmp_link;

	/* CPU mask for this jreq  - Gurrented to be valid only while jreq is queued */
	const struct nvmeib_cpu_mask_info *cpu_mask_info;
};
//#define JREQ2JD(_jreq_) (_jreq_->sorted[_jreq_->curr].disk->jam_disk)

struct jalloc {
	struct nvmeibc_disk *disk;
	/* position in input @disks[] */
	int orig_pos;
};

static inline const char *jidx_state_to_str(enum nvmeibc_jam_jidx_state evt)
{
	switch (evt) {
	case NVMEIBC_JIDX_STS_FREE: 		return "FREE";
	case NVMEIBC_JIDX_STS_ALLOCED: 		return "ALLOCED";
	case NVMEIBC_JIDX_STS_ERASING: 		return "ERASING";
	case NVMEIBC_JIDX_STS_ABANDONED: 	return "ABANDONED";
	default: return "???";
	}
}

static inline const char *jidx_state_to_str_5(enum nvmeibc_jam_jidx_state evt)
{
	switch (evt) {
	case NVMEIBC_JIDX_STS_FREE: 		return "FREE ";
	case NVMEIBC_JIDX_STS_ALLOCED: 		return "ALLOC";
	case NVMEIBC_JIDX_STS_ERASING: 		return "ERASE";
	case NVMEIBC_JIDX_STS_ABANDONED: 	return "ABAND";
	default: return "???";
	}
}

static inline const char *jidx_event_to_str(enum nvmeibc_jam_jidx_event evt)
{
	switch (evt) {
	case NVMEIBC_JIDX_EVT_END_USE: 			return "END_USE";
	case NVMEIBC_JIDX_EVT_ERASE: 			return "ERASE";
	case NVMEIBC_JIDX_EVT_ERASE_COMP: 		return "ERASE_COMP";
	case NVMEIBC_JIDX_EVT_ERASE_COMP_ERR:	return "ERASE_COMP_ERR";
	case NVMEIBC_JIDX_EVT_ABANDON: 			return "ABANDON";
	case NVMEIBC_JIDX_EVT_FREE_ABND: 		return "FREE_ABND";
	default: return "???";
	}
}

#if DEBUG_JAM
#define debug_alloc_start(_jidx_, _jam_disk_) do { \
	_jam_disk_->n_alloced_avg_sum += _jam_disk_->n_alloced; \
	_jam_disk_->n_alloced_avg_cnt++; \
	_jidx_->alloc_jif = jiffies; \
} while (0)

#define debug_alloc_end(_jidx_, _jam_disk_) do { \
	_jam_disk_->jif_alloced_sum += jiffies - _jidx_->alloc_jif; \
	_jam_disk_->jif_alloced_cnt++; \
	_jidx_->alloc_jif = 0; \
} while (0)

#define debug_alloc_end_unused(_jidx_) do { \
	_jidx_->alloc_jif = 0; \
} while (0)

#define debug_pending_on_empty(_jam_disk_) do { \
	_jam_disk_->n_pending_reason_empty_cnt++; \
} while (0)

#define debug_pending_max_used(_jam_disk_) do { \
	_jam_disk_->n_pending_reason_max_used_cnt++; \
} while (0)

#define debug_pending_on_bound(_jam_disk_, _idx_) do { \
	_jam_disk_->n_pending_reason_bound_cnt++; 		\
	switch (_jam_disk_->jidx_pool[_idx_].state) { 	\
	case NVMEIBC_JIDX_STS_ALLOCED:					\
		_jam_disk_->n_pending_bound_alloced_cnt++;	\
		break;										\
	case NVMEIBC_JIDX_STS_ABANDONED:				\
		_jam_disk_->n_pending_bound_abnded_cnt++;	\
		break;										\
	case NVMEIBC_JIDX_STS_ERASING:					\
		_jam_disk_->n_pending_bound_earsing_cnt++;	\
		break;										\
	default:										\
		_jam_disk_->n_pending_bound_unexp_cnt++;	\
		break;										\
	}												\
} while (0)

#define debug_erase_start(_jidx_,_jam_disk_) do { \
	_jam_disk_->n_erasing_avg_sum += _jam_disk_->n_erasing; \
	_jam_disk_->n_erasing_avg_cnt++; \
	_jidx_->erase_jif = jiffies; \
} while (0)

#define debug_erase_end(_jidx_, _jam_disk_) do { \
	_jam_disk_->jif_erasing_sum += jiffies - _jidx_->erase_jif; \
	_jam_disk_->jif_erasing_cnt++; \
	_jidx_->erase_jif = 0; \
} while (0)

#define debug_free_avg(_jam_disk_) do { \
	_jam_disk_->n_free_avg_sum += _jam_disk_->n_free; \
	_jam_disk_->n_free_avg_cnt++; \
} while (0)

#define debug_pending_timeout(_jam_disk_) do { \
	_jam_disk_->n_pending_timeout++; \
} while (0)

#define debug_pending_reason(_jam_disk_) do { \
	_jam_disk_->n_pending_reason_empty++; \
} while (0)

#define debug_bound_to_transient(_jam_disk_) do { \
	_jam_disk_->n_bound_to_transient_cnt++; \
} while (0)

#else
#define debug_alloc_start(_jidx_)
#define debug_alloc_end(_jidx_, _jam_disk_)
#define debug_alloc_end_unused(_jidx_)
#define debug_pending_on_empty(_jam_disk_)
#define debug_pending_max_used(_jam_disk_)
#define debug_pending_on_bound(_jam_disk_, _idx_)
#define debug_erase_start(_jidx_,_jam_disk_)
#define debug_erase_end(_jidx_, _jam_disk_)
#define debug_free_avg(_jam_disk_)
#endif

/* Please note that jidx-event (funcs) do not acquire PD layer protection.
   Thus any async function called from these funcs must be PD protected.
   For example, erase, resume pending-req using the just-freed jidx, etc. */
static int jidx_event_(struct nvmeibc_disk *disk, int idx, u8 *idx_gen_id,
					   enum nvmeibc_jam_jidx_event evt,
					   bool *put_back, int *n_jreqs);
static int jidx_event_vec(struct nvmeibc_disk *disk, int *idx_arr,
						  u8 *idx_gen_id, int *idx_rv_arr, int n_idx,
						  enum nvmeibc_jam_jidx_event evt);
#define jidx_event(disk, idx, evt)            jidx_event_vec(disk, &idx, NULL, NULL, 1, evt)
#define jidx_event_debug(disk, idx, ctr, evt) jidx_event_vec(disk, &idx, NULL, NULL, 1, evt)

static void jidx_get_(struct nvmeibc_jam_disk *jam_disk,
					  struct nvmeibc_jam_jidx *jidx);
static int jidx_alloc_(struct nvmeibc_jam_disk *jam_disk, jidx_hkey_t hkey,
	int *idx, bool wait_bound_abnd, bool dry_run);

extern void nvmeibc_block_dp_ec_journal_alloc_cb(
	int status, u64 *res_jlbas, void *ctx);

struct lba_enc {
	u64 lba : 56;
	u64 ver :  8;
};

static inline void jam_disk_spin_lock(struct nvmeibc_jam_disk *jam_disk)
{
	spin_lock(&jam_disk->lock);
	jam_disk->locking_cpu = smp_processor_id();
}

static inline void jam_disk_spin_unlock(struct nvmeibc_jam_disk *jam_disk)
{
	jam_disk->locking_cpu = -1;
	spin_unlock(&jam_disk->lock);
}

static inline void jam_disk_spin_lock_irqsave(
	struct nvmeibc_jam_disk *jam_disk, unsigned long *pflags)
{
	spin_lock_irqsave(&jam_disk->lock, *pflags);
	jam_disk->locking_cpu = smp_processor_id();
}

static inline void jam_disk_spin_unlock_irqrestore(
	struct nvmeibc_jam_disk *jam_disk, unsigned long flags)
{
	jam_disk->locking_cpu = -1;
	spin_unlock_irqrestore(&jam_disk->lock, flags);
}

static inline bool jam_disk_already_locked(struct nvmeibc_jam_disk *jam_disk)
{
	/* work assumption: ch is always locked with irqsave */
    return irqs_disabled() && jam_disk->locking_cpu == smp_processor_id();
}

static inline u64 idx_2_lba(struct nvmeibc_disk *disk, int idx)
{
	u64 lba = disk->jour.rng_slba + (idx << disk->jam_disk->binje_shift);
#if JAM_ENCODE_JOURNAL_LBA
	u64 *_lba = &lba;
	struct lba_enc *l = (void *)_lba;

	if (l->ver == 0)
		l->ver = (nvmeibc_disk_version_get(disk) & 0xff);
	else
		_NE(error_jam_idx_2_lba, "Cannot encode version");
#endif

	_NDj(trace_jam_idx_2_lba, disk, "idx=@JRNL_RNG_ENT_IDX --> lba=@LBA_LLONG", idx, lba);
	return lba;
}

static inline int lba_2_idx(struct nvmeibc_disk *disk, u64 lba)
{
	int idx;

#if JAM_ENCODE_JOURNAL_LBA
	u32 disk_version;
	struct lba_enc *l = (void *)&lba;

	disk_version = (nvmeibc_disk_version_get(disk) & 0xff);
	if (l->ver != disk_version) {
		_NE(error_jam_lba_2_idx, "Error: Disk @DISK_NAME (jam_disk=@JAM_DISK), lba from other session @DISK_VERSION vs @DISK_VERSION (disk_dying=@INT)",
		   disk->name, disk->jam_disk, l->ver, disk_version, atomic_read(&disk->dying));
		if (((l->ver + 1) & 0xff) == (disk_version & 0xff)) {
			/* nvmeibc-disk_release first updates disk_version and only then set dying, so PD layer can reach this code */
			return -1;
		} else {
			/* We should never reach here */
			WARN_ON(1);
			return -EFAULT;
		}
	}
	l->ver = 0;
#endif

	idx = (lba - disk->jour.rng_slba) >> disk->jam_disk->binje_shift;

#if	DEBUG_JAM
	if (idx >= disk->jam_disk->jidx_pool_size ||
		!IS_ALIGNED((lba - disk->jour.rng_slba), disk->jour.rng_binje)) {
		_NEj(error_1_jam_lba_2_idx, disk, "Invalid journal idx @JRNL_RNG_ENT_IDX, pool size @JIDX_POOL_SIZE",
			idx, disk->jam_disk->jidx_pool_size);
		idx = -EFAULT;
	}
#endif
	_NDj(trace_jam_lba_2_idx, disk, "@DLBA --> idx=@JRNL_RNG_ENT_IDX", lba, idx);
	return idx;
}

int nvmeibc_jam_lba_2_idx(struct nvmeibc_disk *disk, u64 lba) {
	return lba_2_idx(disk, lba);
}

TIMER_CALLBACK_DECL(pending_req_timeout_timer_fn);

static struct nvmeibc_jam_pending_req *pending_req_alloc(
	int n_disks, struct jalloc *sorted, u64 *onstack_jlbas, u32 txid, bool wait_bound_abnd, unsigned long deadline_jif, unsigned long priority, void *ctx)
{
	struct nvmeibc_jam_pending_req *jreq = NULL;
	u64 *res_jlbas = NULL;
	NFIN;

	if (!(jreq = kzalloc(sizeof(*jreq), GFP_ATOMIC)) ||
		!(res_jlbas = kzalloc(sizeof(*res_jlbas) * n_disks, GFP_ATOMIC))) {
		_NT(trace_jam_pending_req_alloc, "Fail to allocate pending req");
		goto err;
	}

	memcpy(res_jlbas, onstack_jlbas, (sizeof(*res_jlbas) * n_disks));
	jreq->n_disks = n_disks;
	jreq->sorted = sorted;
	jreq->txid = txid;
	jreq->res_jlbas = res_jlbas;
	jreq->wait_bound_abnd = wait_bound_abnd;
	jreq->ctx = ctx;
	atomic_set(&jreq->resume_wip, 0);
	RB_CLEAR_NODE(&jreq->pending_node);
	jreq->curr = -1;
	SETUP_TIMER(&jreq->timeout_timer, pending_req_timeout_timer_fn,
				(unsigned long)jreq, TIMER_IRQSAFE);
	TIMER_SET_DATA(jreq, timeout_timer, (unsigned long) jreq);
	jreq->timeout_expired = false;
	jreq->timeout_ctx = NULL;
	jreq->pend_jif = 0;
	jreq->deadline_jif = deadline_jif;
	jreq->priority = priority;
	goto out;

err:
	kfree(jreq);
	kfree(res_jlbas);
	jreq = NULL;

out:
	NFOUT;
	return jreq;
}

static void pending_req_free(struct nvmeibc_jam_pending_req *jreq)
{
	kfree(jreq->sorted);
	jreq->sorted = NULL;
	kfree(jreq->res_jlbas);
	jreq->res_jlbas = NULL;
	memset(jreq, 0xee, sizeof(*jreq));
	kfree(jreq);
}

/* This func can be called while pause for disk is prevented (i.e. in-transfers
 * is inc'ed) or from comp ctx which gaurentee disk is not freeing (may be paused).
 *
 * Usage:
 * 1. Rollback of partial allocation (jidx-put-unused) - protected by pausable.
 * 2. Send comp ctx of no/rdda channel (remote erase journal entry)
 * 3. Recv comp ctx of admin channel (serjio's free-abandoned msg)
 *
 */
static void pending_req_resume_(struct nvmeibc_jam_pending_req *jreq, int idx)
{
	struct jalloc *sorted = jreq->sorted;
	int curr = jreq->curr;
	u64 lba;
	NFIN;

	lba = idx_2_lba(sorted[curr].disk, idx);
	jreq->res_jlbas[sorted[curr].orig_pos] = lba;
	jreq->curr++;

	NFOUT;
}

static void pending_req_resume_bh(struct nvmeibc_jam_pending_req *jreq);

#define DEC_DEFERRED_JREQS_AND_WAKEUP_WAITER(jam_disk) \
	do { \
		if (atomic_dec_and_test(&(jam_disk)->deferred_jreqs_cnt)) { \
			wake_up(&(jam_disk)->deferred_jreqs); \
		} \
	} while (0)

static void pending_req_resume_work(struct workqe_struct *work)
{
	struct nvmeibc_jam_pending_req *jreq =
		container_of(work, struct nvmeibc_jam_pending_req, resume_work);
	int n;
	struct nvmeibc_jam_disk *jam_disk = jreq->jam_disk;
	NFIN;

	n = atomic_dec_return(&jreq->resume_wip);
	if (n == 0) {
		pending_req_resume_bh(jreq); /* resume from work ctx */
	}
	else {
		_NW(warn_jam_pending_req_resume_work, "Oops, resume work wip err (n=@COUNT)", n);
		WARN_ON_ONCE(1);
	}

	DEC_DEFERRED_JREQS_AND_WAKEUP_WAITER(jam_disk);

	NFOUT;
}

/* If resume is invoked from rollback-partial-allocation we defer it
   in order to avoid unknown number of calls to rollback/resume/rollback
   (particularly on a intr-ctx stack),
   Note that timeout timer must be off when adding work ! */
static inline bool schedule_resume_work(struct nvmeibc_jam_disk *jam_disk,
					 struct nvmeibc_jam_pending_req *jreq)
{
	static unsigned int pcpu_cntr = 0; /* this is racy between threads/cpus but only used for randomization */
	bool rv = false;
	unsigned int resched_cpu;

	WQ_INIT_WORK(&jreq->resume_work, pending_req_resume_work);
	jreq->jam_disk = jam_disk;

	atomic_inc(&jam_disk->deferred_jreqs_cnt);
	if (NVMEIBC_DISK_SHOULD_DEFER_TO_PCPU_WQ(nvmeibc_jam_use_system_pcpu_wq && !NVMEIB_CPU_MASK_IS_EMPTY(jreq->cpu_mask_info->mask), jreq->cpu_mask_info->mask)) {
		resched_cpu = NVMEIBC_DISK_GET_RESCHED_CPU(jreq->cpu_mask_info->mask.cpus, pcpu_cntr);
		rv = nvmeib_pcpu_wq_add_work_on_core(nvmeib_get_system_wq(), resched_cpu, &jreq->resume_work);
	} else {
		rv = wq_add_work(jam_disk->jwq, &jreq->resume_work);
	}
	if (!rv) {
		WARN_ON_ONCE(1);
		_NE(fail_resume_work_add, "Failed to add resume work");
		DEC_DEFERRED_JREQS_AND_WAKEUP_WAITER(jam_disk);
	}

	return rv;
}

static void pending_req_resume_work_add(struct nvmeibc_jam_disk *jam_disk,
	struct nvmeibc_jam_pending_req *jreq)
{
	int n;
	bool rv = false;
	NFIN;

	BUG_ON(timer_pending(&jreq->timeout_timer));

	n = atomic_inc_return(&jreq->resume_wip);
	if (n == 1) {
		rv = schedule_resume_work(jam_disk, jreq);
		if (!rv) {
			_NE(error_jam_pending_req_resume_work_add, "Failed to add resume work, from '@__BUILTIN_RETURN_ADDRESS'",
				__builtin_return_address(0));
		}
	}
	else {
		_NW(warn_jam_pending_req_resume_work_add, "Oops, resume work (member) already used (n=@COUNT), from '@__BUILTIN_RETURN_ADDRESS'",
			n, __builtin_return_address(0));
		WARN_ON_ONCE(1);
	}

	NFOUT;
}

static void link_jreq_jidx(struct nvmeibc_jam_disk *jam_disk,
					struct nvmeibc_jam_pending_req *jreq, int bound_idx)
{
	struct nvmeibc_jam_jidx *jidx;
	NFIN;

	/* jreq --> jidx */
	jreq->bound_idx = bound_idx;

	if (bound_idx < jam_disk->jidx_pool_size &&
		bound_idx != NVMEIBC_JAM_INVALID_BOUND_IDX) {

		/* jidx --> jreq */
		jidx = &jam_disk->jidx_pool[jreq->bound_idx];
		if (jreq->hkey.raw == jidx->hkeys.committed.raw) {
			BUG_ON(jidx->jreq_bound_via_committed);
			jidx->jreq_bound_via_committed = jreq;
		}
		else if (jreq->hkey.raw == jidx->hkeys.transient.raw) {
			BUG_ON(jidx->jreq_bound_via_transient);
			jidx->jreq_bound_via_transient = jreq;
		}
		else {
			BUG();
		}
	}

	NFOUT;
}

static void unlink_jreq_jidx(struct nvmeibc_jam_disk *jam_disk,
					  struct nvmeibc_jam_pending_req *jreq)
{
	struct nvmeibc_jam_jidx *jidx;
	NFIN;

	if (jreq->bound_idx < jam_disk->jidx_pool_size &&
		jreq->bound_idx != NVMEIBC_JAM_INVALID_BOUND_IDX) {

		/* jidx -X-> jreq */
		jidx = &jam_disk->jidx_pool[jreq->bound_idx];
		if (jreq == jidx->jreq_bound_via_committed) {
			jidx->jreq_bound_via_committed = NULL;
		}
		else if (jreq == jidx->jreq_bound_via_transient) {
			jidx->jreq_bound_via_transient = NULL;
		}
		else {
			BUG();
		}

		/* jreq -X-> jidx */
		jreq->bound_idx = NVMEIBC_JAM_INVALID_BOUND_IDX;
	}

	NFOUT;
}

static inline int unlink_jidx_from_any_jreq(struct nvmeibc_jam_jidx *jidx)
{
	struct nvmeibc_jam_pending_req *jreq;
	int n_jreqs = 0;
	NFIN;

	jreq = jidx->jreq_bound_via_committed;
	if (jreq) {
		BUG_ON(jreq->bound_idx != jidx->idx);
		BUG_ON(RB_EMPTY_NODE(&jreq->pending_node));
		BUG_ON(jreq->hkey.raw != jidx->hkeys.committed.raw);
		jreq->bound_idx = NVMEIBC_JAM_INVALID_BOUND_IDX;
		jidx->jreq_bound_via_committed = NULL;
		n_jreqs++;
	}

	jreq = jidx->jreq_bound_via_transient;
	if (jreq) {
		BUG_ON(jreq->bound_idx != jidx->idx);
		BUG_ON(RB_EMPTY_NODE(&jreq->pending_node));
		BUG_ON(jreq->hkey.raw != jidx->hkeys.transient.raw);
		BUG_ON(jidx->state != NVMEIBC_JIDX_STS_ERASING);
		jreq->bound_idx = NVMEIBC_JAM_INVALID_BOUND_IDX;
		jidx->jreq_bound_via_transient = NULL;
		n_jreqs++;
	}

	NFOUT;
	return n_jreqs;
}

static void pending_req_tree_insert(struct nvmeibc_jam_disk *jam_disk, struct nvmeibc_jam_pending_req *jreq)
{
	struct rb_root *root = &jam_disk->pending_root;
	struct rb_node **node = &(root->rb_node), *parent = NULL;
	while (*node) {				// Figure out where to put new node
		struct nvmeibc_jam_pending_req *cur = container_of(*node, struct nvmeibc_jam_pending_req, pending_node);
		parent = *node;
		if (jreq->priority < cur->priority)
			node = &((*node)->rb_left);
		else
			node = &((*node)->rb_right);
	}

	rb_link_node(&jreq->pending_node, parent, node);
	rb_insert_color(&jreq->pending_node, root);
}

/* Function must be PD protected */
static void pending_req_list_add_(struct nvmeibc_jam_pending_req *jreq)
{
	struct nvmeibc_jam_disk *jam_disk;
	NFIN;

	if (jreq->curr >= jreq->n_disks) {
		_NW(warn_jam_pending_req_list_add, "jreq: curr > ndisks (@CURR_INT, @N_DISKS)",
			jreq->curr, jreq->n_disks);
		WARN_ON_ONCE(1);
		goto out;
	}
	if (!RB_EMPTY_NODE(&jreq->pending_node)) {
		_NW(warn_1_jam_pending_req_list_add, "jreq already linked");
		WARN_ON_ONCE(1);
		goto out;
	}

	jam_disk = jreq->sorted[jreq->curr].disk->jam_disk;
	pending_req_tree_insert(jam_disk, jreq);
	jam_disk->n_pending++;
	jreq->timeout_ctx = jam_disk;
	jreq->pend_jif = jiffies;

	{
		static ulong __jam_pending_req_timeout_jif = 0;
		if (__jam_pending_req_timeout_jif != nvmeibc_jam_pending_req_timeout_jif) {
			_NI(__AUTOID__, "Disk @DISK_NAME, Jam pending-req timeout: @LU -> @LU",
					  jam_disk->disk->name, __jam_pending_req_timeout_jif, nvmeibc_jam_pending_req_timeout_jif);
			__jam_pending_req_timeout_jif = nvmeibc_jam_pending_req_timeout_jif;
		}
	}

	mod_timer(&jreq->timeout_timer, jreq->deadline_jif);

out:
	NFOUT;
}

/* After calling this function and unlocking jam-disk,
   one must call del-timer-sync (except from timer-fn) */
static void pending_req_list_del_(struct nvmeibc_jam_pending_req *jreq,
	struct nvmeibc_jam_disk *jam_disk)
{
	NFIN;

	BUG_ON(jreq->sorted[jreq->curr].disk->jam_disk != jam_disk);
	rb_erase(&jreq->pending_node, &jam_disk->pending_root);
	RB_CLEAR_NODE(&jreq->pending_node);
	jam_disk->n_pending--;

	NFOUT;
}

TIMER_CALLBACK(pending_req_timeout_timer_fn, struct nvmeibc_jam_pending_req, timeout_timer, struct nvmeibc_jam_pending_req, jreq)
	struct nvmeibc_jam_disk *jam_disk = jreq->timeout_ctx;
	ulong flags;
	NFIN;

	if (unlikely(jam_disk == NULL)) {				// DHS: I observed this crash in simulator 'unitest_AsyncJAM - part_1'. Race condition between del_sync and timer?
		WARN(jam_disk == NULL, "Timeout: with NULL jam_disk. Ignorring timer");
		goto _out;
	}
	jam_disk_spin_lock_irqsave(jam_disk, &flags);
	if (!RB_EMPTY_NODE(&jreq->pending_node)) {
		/* see n_pending.* counters for details */
		_NDj(t_01_jttofn, jam_disk->disk, "Timeout: jrep @JREQ (c=@CURR_INT, b=@BOUND_IDX) dequeue and resume for abort",
			jreq, jreq->curr, jreq->bound_idx);
		debug_pending_timeout(jam_disk);
		unlink_jreq_jidx(jam_disk, jreq);
		pending_req_list_del_(jreq, jam_disk);
		/* Cannot free jreq here as timer (ctx) is part of it, defer */
		jreq->timeout_expired = true;
		pending_req_resume_work_add(jam_disk, jreq);
	}
	else
		_NT(trace_1_jam_pending_req_timeout_timer_fn, "Already dequeued, exit");
	jam_disk_spin_unlock_irqrestore(jam_disk, flags);
_out:
	NFOUT;
}

/* alloc --> free
   alloc --> abnd */
static inline void uniqj2b_commit_transient_(struct nvmeibc_jam_disk *jam_disk,
											 struct nvmeibc_jam_jidx *jidx)
{
	if (jidx->hkeys.committed.raw != jidx_hkey_invalid.raw) {
		BUG_ON(!__is_jidx_hkey_io_entry(jidx->hkeys.committed));
		BUG_ON(hlist_unhashed(&jidx->uniqj2b_hlink));
		hash_del(&jidx->uniqj2b_hlink);
	}
	jidx_hkeys_commit_transient(jidx);

#if DEBUG_JAM
	{
	struct nvmeibc_jam_jidx *t;
	u64 new_heky = jidx->hkeys.committed.raw;
	hash_for_each_possible(jam_disk->uniqj2b_htable, t, uniqj2b_hlink, new_heky) {
		if (t->hkeys.committed.raw == new_heky) {
			_NE(error_jam_uniqj2b_commit_transient, "OOPS, exact hkey=@HKEY is already hashed "
			   "(jam=@JAM, jidx=@JIDX_PTR(idx=@JRNL_RNG_ENT_IDX), t=@PTR(idx=@JRNL_RNG_ENT_IDX) tx_id:@INT",
			   new_heky, jam_disk, jidx, jidx->idx, t, t->idx, t->hkeys.committed.tx_id);
			BUG();
		}
	}
	}
#endif

	hash_add(jam_disk->uniqj2b_htable, &jidx->uniqj2b_hlink,
			 jidx->hkeys.committed.raw);
}

/* abnd --> free */
static inline void uniqj2b_hdel_committed_(struct nvmeibc_jam_disk *jam_disk,
										   struct nvmeibc_jam_jidx *jidx)
{
	(void)jam_disk;
	if (jidx->hkeys.committed.raw == jidx_hkey_invalid_special.raw) {
		_NW(uniqj2b_hdel_committed__w17,
			"abnd->free for invalid-special (jam=@PTR, jidx=@PTR(idx=@INT))",
		   jam_disk, jidx, jidx->idx);
	} else {
		BUG_ON(hlist_unhashed(&jidx->uniqj2b_hlink));
		hash_del(&jidx->uniqj2b_hlink);
		BUG_ON(!list_empty(&jidx->uniqj2b_trans_link) ||
			__is_jidx_hkey_io_entry(jidx->hkeys.transient));
		jidx_hkeys_reset_committed(jidx);
	}
}

/* alloc --> erase */
static inline void uniqj2b_hadd_transient_(struct nvmeibc_jam_disk *jam_disk,
										   struct nvmeibc_jam_jidx *jidx)
{
	BUG_ON(__is_jidx_hkey_io_entry(jidx->hkeys.committed) &&
		   hlist_unhashed(&jidx->uniqj2b_hlink));
	BUG_ON(!list_empty(&jidx->uniqj2b_trans_link));

	if (__is_jidx_hkey_io_entry(jidx->hkeys.transient)) {
		list_add_tail(&jidx->uniqj2b_trans_link, &jam_disk->uniqj2b_trans_list);
	}
	else {
		_NE(error_jam_uniqj2b_hadd_transient, "jidx @JIDX_PTR: already transient @RAW",
		   jidx, jidx->hkeys.transient.raw);
	}
}

/* erase --> free */
static inline void uniqj2b_hdel_both(struct nvmeibc_jam_disk *jam_disk,
									struct nvmeibc_jam_jidx *jidx)
{
	NFIN;
	(void)jam_disk;
	//committed
	if (__is_jidx_hkey_io_entry(jidx->hkeys.committed)) {
		BUG_ON(hlist_unhashed(&jidx->uniqj2b_hlink));
		hash_del(&jidx->uniqj2b_hlink);
	}

	//transient
	if (__is_jidx_hkey_io_entry(jidx->hkeys.transient)) {
		BUG_ON(list_empty(&jidx->uniqj2b_trans_link));
		list_del_init(&jidx->uniqj2b_trans_link);
	}
	else {
		BUG_ON(!list_empty(&jidx->uniqj2b_trans_link));
	}

	//reset
	jidx_hkeys_reset_both(jidx);

	NFOUT;
}

enum hkey_bind_rv {
	HKEY_NOT_BOUND = 0x33,	/* @hkey is not NOT bound, use any free jidx */
	HKEY_BOUND_REUSE,		/* @hkey is bound to @bound_idx, re-use it */
	HKEY_BOUND_WAIT,		/* @hkey is bound to @bound_idx, wait in pending-list */
	HKEY_BOUND_ABANDONED,	/* @hkey is bound to @bound_idx, but the jidx state is ABANDONED;
							 * decide if to wait in pending-list or to fail the allocation */
	HKEY_ERROR,				/* unexpected state, rollback allocation */
};

enum hkey_bind_check {
	HKEY_BIND_CHECK_BOTH = 0,
	HKEY_BIND_CHECK_ONLY_COMMITTED
};

static int uniqj2b_bind_check(struct nvmeibc_jam_disk *jam_disk,
				jidx_hkey_t hkey, int *bound_idx, enum hkey_bind_check bind_check)
{
	struct nvmeibc_jam_jidx *jidx;
	bool found = false;
	int rv;

	/* init to not-bound */
	*bound_idx = NVMEIBC_JAM_INVALID_BOUND_IDX;

	/* Check if @hkey is bound to 'committed' */
	hash_for_each_possible(jam_disk->uniqj2b_htable, jidx, uniqj2b_hlink, hkey.raw) {
		if (jidx->hkeys.committed.raw == hkey.raw) {
			_NDj(trace_jam_uniqj2b_bind_check, jam_disk->disk, "found jidx @JRNL_RNG_ENT_IDX (@JIDX_PTR) state=@STATE_STR, committed hkey=@HKEY",
			   jidx->idx, jidx, jidx_state_to_str(jidx->state), hkey.raw);

			switch (jidx->state) {
			case NVMEIBC_JIDX_STS_FREE:
				*bound_idx = jidx->idx;
				BUG_ON(jidx->hkeys.transient.raw != jidx_hkey_invalid.raw);
				rv = HKEY_BOUND_REUSE;
				break;

			case NVMEIBC_JIDX_STS_ALLOCED:
			case NVMEIBC_JIDX_STS_ABANDONED:
			case NVMEIBC_JIDX_STS_ERASING:
				if (jidx->jreq_bound_via_committed) {
					_NE(error_jam_uniqj2b_bind_check, "@JIDX_PTR", jidx);
					BUG();
				}
//				if (jidx->jreq_bound_via_committed) { _NE(uniqj2b_bind_check_e1, "@PTR", jidx); BUG(); }
				*bound_idx = jidx->idx;
				rv = (jidx->state == NVMEIBC_JIDX_STS_ABANDONED ? HKEY_BOUND_ABANDONED : HKEY_BOUND_WAIT);
				break;

			default:
				_NW(warn_1_jam_uniqj2b_bind_check, "jidx @JRNL_RNG_ENT_IDX(@JIDX_PTR), unknown state=@STATE_STR",
				   jidx->idx, jidx, jidx_state_to_str(jidx->state));
				WARN_ON_ONCE(1);
				rv = HKEY_ERROR;
				goto out;
			}

			found = true;
			break;
		}
	}

	if (bind_check == HKEY_BIND_CHECK_ONLY_COMMITTED) {
		goto skip_trans_check;
	}

	/* Check if @hkey is bound to 'transient' */
	if (!found) {
		list_for_each_entry(jidx, &jam_disk->uniqj2b_trans_list, uniqj2b_trans_link) {
			if (jidx->hkeys.transient.raw == hkey.raw) {
				_NT(trace_1_jam_uniqj2b_bind_check, "Found jidx @JRNL_RNG_ENT_IDX (@JIDX_PTR) state=@STATE_STR, transient hkey=@HKEY",
				   jidx->idx, jidx, jidx_state_to_str(jidx->state), hkey.raw);

				debug_bound_to_transient(jam_disk);

				if (unlikely(jidx->state != NVMEIBC_JIDX_STS_ERASING)) {
					_NW(warn_2_jam_uniqj2b_bind_check, "Disk @DISK_NAME(@DISK), jidx @JRNL_RNG_ENT_IDX (@JIDX_PTR), unexpected state @JIDX_STATE_TO_STR",
					   jam_disk->disk->name, jam_disk->disk, jidx->idx, jidx, jidx_state_to_str(jidx->state));
					WARN_ON_ONCE(1);
					rv = HKEY_ERROR;
					goto out;
				}

				if (jidx->jreq_bound_via_transient) { _NE(error_1_jam_uniqj2b_bind_check, "@JIDX_PTR", jidx); BUG(); }
//				if (jidx->jreq_bound_via_transient) { _NE(jam_uniqj2b_bind_check_e200, "@PTR", jidx); BUG(); }
				*bound_idx = jidx->idx;
				rv = HKEY_BOUND_WAIT;
				found = true;
				break;
			}
		}
	}

skip_trans_check:
	/* Free as a bird */
	if (!found) {
		rv = HKEY_NOT_BOUND;
	}

out:
	return rv;
}

/* Called after jidx's committed-hkey (and transient-hkey in case of erase-comp)
   was/were flushed/swapped and unhashed even if jidx was returned to free-pool.
   If jreq/s was/were pending because of this jidx, its bound_idx is no longer
   pointing on jidx->idx. */
static struct nvmeibc_jam_pending_req *find_jreq_to_resume_(
	struct nvmeibc_jam_disk *jam_disk)
{
	struct nvmeibc_jam_pending_req *jreq;
	struct nvmeibc_jam_jidx *jidx = NULL;
	struct rb_node *node;
	int idx;
	int rv;
	NFIN;

	if (list_empty(&jam_disk->free_list)) {
		jreq = NULL;
		goto out;
	}

	/* search for pending jreq, in the pending rbtree order, i.e. by priority */
	for (node = rb_first(&jam_disk->pending_root); node; node = rb_next(node)) {
		jreq = container_of(node, struct nvmeibc_jam_pending_req, pending_node);
		if (jreq->bound_idx == NVMEIBC_JAM_INVALID_BOUND_IDX) {
			/* This jreq was either added while free-pool was empty, or;
			   It was bound but then unbound (end-use/abnd/a2f/erase-comp) */
			rv = jidx_alloc_(jam_disk, jreq->hkey, &idx, jreq->wait_bound_abnd, false /*dry_run*/);
			if (!rv) {
				jidx = &jam_disk->jidx_pool[idx];
				break;
			}
			else if (rv == -EBUSY) {
				_NT(trace_jam_find_jreq_to_resume, "Updating pending jreq @JREQ (curr=@CURR_INT) bound_idx=@BOUND_IDX",
					jreq, jreq->curr, idx);
				link_jreq_jidx(jam_disk, jreq, idx);
			}
		}
		else {
			/* These jreqs are bound, skipping */
			BUG_ON(jreq->bound_idx >= jam_disk->jidx_pool_size);
		}
	}
	if (!jidx) {
		jreq = NULL;
		goto out;
	}

	/* jidx was dequeued and its transient-hkey was set, resume @jreq */
	pending_req_list_del_(jreq, jam_disk);
	pending_req_resume_(jreq, jidx->idx);

out:
	NFOUT;
	return jreq;
}

void pd_rollback_partial_allocation(struct nvmeibc_jam_pending_req *jreq);

/* Do not hold any jreq's jam-disk lock while calling this function,
   the same lock is required for returning the unused jidx to the free-pool */
static void pending_req_cancel(struct nvmeibc_jam_pending_req *jreq)
{
	NFIN;

	_NT(trace_jam_pending_req_cancel, "Cancel req pending on disk @DISK_NAME", jreq->sorted[jreq->curr].disk->name);
	pd_rollback_partial_allocation(jreq);
	jam_cnts_on_alloc_err(cdisk2cj(jreq->sorted[0].disk));
	nvmeibc_block_dp_ec_journal_alloc_cb(-ENOTCONN, NULL, jreq->ctx);
	pending_req_free(jreq);

	NFOUT;
}

/*
 * rv = 0 		-->  @idx=jidx->idx
 * rv == -EBUSY -->  if hkey is bound @idx=@bound_idx
 *    				 else, free jidx pool is empty and @idx=pool-size
 * rv = -1      -->  error, rollback allocation
 */
static int jidx_alloc_(struct nvmeibc_jam_disk *jam_disk, jidx_hkey_t hkey,
	int *idx, bool wait_bound_abnd, bool dry_run)
{
	struct nvmeibc_jam_jidx *jidx = NULL;
	int bound_idx;
	// The print below needs a value for bind_rv if the list is empty
	int bind_rv = -1, rv = -1;
	NFIN;

	/* First check if list-empty as otherwise we might do extra bind-check.
	   [1] if free-pool is empty, @hkey may not be bound now, but by the time
	   jreq is dequeued, older jreqs may have caused hkey bound to some jidx.
	   [2] when jidx is done with its hkey/s it also unlink any pending jreq
	   from it. So at least for the case that list is empty we potentially
	   save one bind-check.
	*/

	if (list_empty(&jam_disk->free_list)) {
		debug_pending_on_empty(jam_disk);

		*idx = NVMEIBC_JAM_INVALID_BOUND_IDX;
		rv = -EBUSY;
		if (!wait_bound_abnd &&
			(bind_rv = uniqj2b_bind_check(
				jam_disk, hkey, &bound_idx, HKEY_BIND_CHECK_ONLY_COMMITTED)) == HKEY_BOUND_ABANDONED)
		{
			_NTj(trace_jam_jidx_alloc_deadlk, jam_disk->disk,
			     "entry @JRNL_RNG_ENT_IDX ABANDONED with matching hkey=@HKEY  - returning EDEADLK", bound_idx, hkey.raw);
			*idx = bound_idx;
			rv = -EDEADLK;
		}
		goto out;
	}

	if (unlikely(nvmeibc_jam_max_used_entries &&
				 (uint)jam_disk->n_alloced >= (nvmeibc_jam_max_used_entries - 1)))
	{
		debug_pending_max_used(jam_disk);

		*idx = NVMEIBC_JAM_INVALID_BOUND_IDX;
		rv = -EBUSY;
		if (!wait_bound_abnd &&
			(bind_rv = uniqj2b_bind_check(
				jam_disk, hkey, &bound_idx, HKEY_BIND_CHECK_ONLY_COMMITTED)) == HKEY_BOUND_ABANDONED)
		{
			_NTj(trace_jam_jidx_alloc_deadlk_2, jam_disk->disk,
			     "entry @JRNL_RNG_ENT_IDX ABANDONED with matching hkey=@HKEY  - returning EDEADLK", bound_idx, hkey.raw);
			*idx = bound_idx;
			rv = -EDEADLK;
		}
		goto out;
	}

	bind_rv = uniqj2b_bind_check(jam_disk, hkey, &bound_idx, HKEY_BIND_CHECK_BOTH);
	switch (bind_rv) {
	case HKEY_NOT_BOUND:
		jidx = list_first_entry(&jam_disk->free_list,
			struct nvmeibc_jam_jidx, pool_link);
		break;

	case HKEY_BOUND_REUSE:
		jidx = &jam_disk->jidx_pool[bound_idx];
		break;

	case HKEY_BOUND_ABANDONED:
		if (!wait_bound_abnd) {
			_NTj(trace_jam_jidx_alloc_deadlk_3, jam_disk->disk,
			     "entry @JRNL_RNG_ENT_IDX ABANDONED with matching hkey=@HKEY  - returning EDEADLK", bound_idx, hkey.raw);
			rv = -EDEADLK;
			break;
		}
		FALLTHRU;
		/* no break */
	case HKEY_BOUND_WAIT:
		debug_pending_on_bound(jam_disk, bound_idx);
		*idx = bound_idx;
		rv = -EBUSY;
		break;

	case HKEY_ERROR:
	default:
		_NE(error_jam_jidx_alloc, "Failed bind-check (@BIND_RV)", bind_rv);
		rv = -1;
		break;
	}

	if (jidx) {
		if (likely(!dry_run)){
			jidx_get_(jam_disk, jidx);
			jidx_hkeys_set_transient(jidx, hkey);
		}
		*idx = jidx->idx;
		rv = 0;
	}

out:
	_NDj(trace_jam_jidx_alloc, jam_disk->disk, "txid=@TXID j2b=@J2B idx=@JRNL_RNG_ENT_IDX bind_rv=@BIND_RV rv=@RV dry_run=@DRY_RUN",
		hkey.tx_id, hkey.j2b, *idx, bind_rv, rv, (int)dry_run);
	NFOUT;
	return rv;
}

/********************** Debug API: Used for unitesting ************************/
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
int   nvmeibc_jam_simu_alloc_entry(const struct nvmeibc_disk *disk, u64 dlba, u32 pre_txid, bool dry_run)
{
	/*const*/ struct nvmeibc_jam_disk *jam_disk = (void*)disk->jam_disk;
	ulong flags;
	int rv = -1, idx = -1;
	jidx_hkey_t hkey = {.j2b=__j2d_to_j2b(dlba), .tx_id = pre_txid + 1, .reserved=0};
	jam_disk_spin_lock_irqsave(jam_disk, &flags);
	rv = jidx_alloc_(jam_disk, hkey, &idx, true /* wait_bound_abnd */, dry_run);
	BUG_ON(rv != 0);
	jam_disk_spin_unlock_irqrestore(jam_disk, flags);
	return idx;
}

void nvmeibc_jam_simu_process_entry_event(const struct nvmeibc_disk *disk, int idx, enum nvmeibc_jam_jidx_event event){
	int rv = jidx_event((struct nvmeibc_disk *)disk, idx, event);
	BUG_ON(rv != 0);
}

struct jam_simu_stats nvmeibc_get_jam_simu_stats(const struct nvmeibc_disk *disk) {
	return (struct jam_simu_stats){.n_bound_to_transient = disk->jam_disk->n_bound_to_transient_cnt,
								   .n_pending_timeout = disk->jam_disk->n_pending_timeout,
								   .n_pending_on_empty = disk->jam_disk->n_pending_reason_empty_cnt};
}

void nvmeibc_jam_simu_inject_hash_function(const struct nvmeibc_disk *disk, u64 (*hash64)(u64, unsigned int)) {
	disk->jam_disk->uniqj2b_htable.hash64 = hash64;
}

static void __all_jam_inair_ops_drained_cb(void *_comp)
{
	complete((struct completion *)_comp);
}

void nvmeibc_jam_drain_disk_ops_block_idle_unsafe(struct nvmeibc_disk *disk)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	disk->should_pause = true;	// Simulate as if we are in pause
	nvmeibc_pd_pause(disk, __all_jam_inair_ops_drained_cb, &comp); // Drain JAM as if pause arrive
	wait_for_completion(&comp); // Wait for rA exexution (# of pending ios == 2)
	nvmeibc_pd_cont(disk);		// Invert the cont results
}

void nvmeibc_jam_simu_verify_cleand(struct nvmeibc_disk *disk, u32 e) {
	struct nvmeibc_jam_disk *jd = (void*)disk->jam_disk;
	struct nvmeibc_jam_jidx *jidx = NULL;
	ulong flags;
	spin_lock_irqsave(&jd->lock, flags);
	jidx = &jd->jidx_pool[e];
	_NDj(trace_jam_nvmeibc_jam_simu_verify_cleand, disk, "idx=@JRNL_RNG_ENT_IDX state=@STATE_STR", e, jidx_state_to_str(jidx->state));
	BUG_ON(jidx->state != NVMEIBC_JIDX_STS_FREE);   // Unitest requested illegal entry
	spin_unlock_irqrestore(&jd->lock, flags);
}

bool nvmeibc_jam_simu_has_abandoned(struct nvmeibc_disk *disk) {
	return ((disk->jam_disk->n_free != disk->jam_disk->jidx_pool_size) || (disk->jam_disk->n_abandoned != 0));
}

#endif //SIMULATOR

/******************************************************************************/
static void jidx_get_(struct nvmeibc_jam_disk *jam_disk,
					  struct nvmeibc_jam_jidx *jidx)
{
	NFIN;

	list_del_init(&jidx->pool_link);
	jidx->state = NVMEIBC_JIDX_STS_ALLOCED;
	jam_disk->n_alloced++;
	jam_disk->n_free--;
	debug_alloc_start(jidx, jam_disk);
	jam_cnts_on_jent_to_alloc(jam_disk);

	NFOUT;
}

/* Called only if there are no pending reqs on @jam_disk */
static inline void jidx_put_(struct nvmeibc_jam_disk *jam_disk,
							 struct nvmeibc_jam_jidx *jidx)
{
	NFIN;
	jam_disk->n_free++;
	debug_free_avg(jam_disk);
	jam_cnts_on_jent_to_free(jam_disk);
	list_add_tail(&jidx->pool_link, &jam_disk->free_list);
	NFOUT;
}

/* Expected to be called from only from rollback-partial-allocation flow */
static void jidx_put_unused(struct nvmeibc_jam_disk *jam_disk, int idx)
{
	struct nvmeibc_jam_jidx *jidx = &jam_disk->jidx_pool[idx];
	struct nvmeibc_jam_pending_req *jreq = NULL;
	ulong flags;
	NFIN;

	BUG_ON(idx < 0);
	jam_disk_spin_lock_irqsave(jam_disk, &flags);
	BUG_ON(jidx->state != NVMEIBC_JIDX_STS_ALLOCED);
	jam_disk->n_alloced--;
	debug_alloc_end_unused(jidx/*, jam_disk*/);
	jam_cnts_on_jent_to_rollback(jam_disk);
	jidx->state = NVMEIBC_JIDX_STS_FREE;
	jidx_hkeys_reset_transient(jidx);
	jidx_put_(jam_disk, jidx);
	jreq = find_jreq_to_resume_(jam_disk);
	jam_disk_spin_unlock_irqrestore(jam_disk, flags);
	if (jreq) {
		del_timer_sync(&jreq->timeout_timer);
		jreq->timeout_ctx = NULL;
		pending_req_resume_work_add(jam_disk, jreq);
	}

	NFOUT;
}

int nvmeibc_jam_jmd_set(struct nvmeibc_disk *disk, u64 lba,
						struct jentry_md *jentry,
						int *ret_idx, bool *lba_enc, u8 *ret_ent_gen_id)
{
	struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
	int idx = lba_2_idx(disk, lba);
	struct nvmeibc_jam_jidx *jidx;
	enum nvmeibc_jam_jidx_state state;
	jidx_hkey_t t;
	int rv = -1;
	NFIN;

	_ND(trace_jam_nvmeibc_jam_jmd_set, "Disk @DISK_NAME(@DISK): set jmdc idx @JRNL_RNG_ENT_IDX", disk->name, disk, idx);
	if (idx < 0 || !jentry || !jentry->md_arr)
		goto out;

	jidx = &jam_disk->jidx_pool[idx];
	#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(jidx->skipped)) {
		WARN_ON(idx != JAM_SKIPPED_JIDX);
		goto done;
	}
	#endif

	state = jidx->state;
	if (state != NVMEIBC_JIDX_STS_ALLOCED) {
		_NWj(warn_jam_nvmeibc_jam_jmd_set, disk, "jmd @JRNL_RNG_ENT_IDX, set but not alloced (state: @JIDX_STATE_TO_STR(@STATE))",
			idx, jidx_state_to_str(state), state);
		JAM_DEBUG_ABORT_ON_ERROR();
		goto out;
	}

	__jmdc_to_hkey(jentry, &t);			// Execute cmd from block layer - jmdc (and its j2d is 100% valid)
	if (likely(JAM_NON_AMBIGUOUS) && t.raw != jidx->hkeys.transient.raw) {
		_NWj(warn_1_jam_nvmeibc_jam_jmd_set, disk, "jmd @JRNL_RNG_ENT_IDX, set to diff value than declared on alloc, @RAW vs hkey=@HKEY",
			idx, t.raw, jidx->hkeys.transient.raw);
		JAM_DEBUG_ABORT_ON_ERROR();
		goto out;
	}

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
done:
#endif
	if (ret_idx)
		*ret_idx = idx;
	if (ret_ent_gen_id)
		*ret_ent_gen_id = jidx->gen_id;
#if JAM_ENCODE_JOURNAL_LBA
	*lba_enc = true;
#else
	*lba_enc = false;
#endif
	rv = 0;

out:
	NFOUT;
	return rv;
}

/* erase remote journal entry (Data + MD + JDM-Cache)
 *
 * This operation is triggered by the UL calling jam's LBAs-free API with
 * write-status=ERR. The UL first acquire pausable layer's approval (inc refcnt)
 * but undo this (dec refcnt) on exit. Thus, in order to ensure pasue is not
 * acked until this async op completes we must inc PD's refcnt too.
 * This is not really needed as there is no operation waiting in UL for this
 * op's completion. Still, future code might issue this op from a ctx that
 * may not be PD protected.
 */
#define NVMEIBC_JAM_MAX_ERASE_ATTEMPTS (3)
static int jentry_erase(struct nvmeibc_disk *disk, struct nvmeibc_jam_jidx *jidx)
{
	int idx = jidx->idx;
	u8 jidx_gen_id = jidx->gen_id;
	struct nvmeibc_disk_gen_cmd *gen_cmd = NULL;
	int rv = -1;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	NFIN;

	if (jidx->n_erase_attempts < NVMEIBC_JAM_MAX_ERASE_ATTEMPTS) {
		jidx->n_erase_attempts++;
	}
	else {
		_NEj(error_0_jam_jentry_erase, disk, "Max-attempts exceeded");
		goto out;
	}

	if (!(gen_cmd = kzalloc(sizeof(*gen_cmd), GFP_ATOMIC))) {
		_NEj(error_1_jam_jentry_erase, disk, "Fail to alloc gen-cmd");
		goto out;
	}

	gen_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_GEN;
	gen_cmd->disk_cmd.server_side_only = true;
	gen_cmd->opcode = NVMEIB_GEN_OP_JENTRY_ERASE;
	gen_cmd->param.je.rng_gen_id = disk->jour.rng_gen_id;
	gen_cmd->param.je.rng_idx = disk->jour.rng_id;
	gen_cmd->param.je.rng_binje = disk->jour.rng_binje;
	gen_cmd->param.je.ent_erase.ent_idx = idx;
	gen_cmd->param.je.ent_erase.ent_md.ent_gen_id = jidx_gen_id;
	gen_cmd->param.je.ent_erase.ent_swlba =
		nvmeibc_jam_decode_lba(idx_2_lba(disk, idx));
	gen_cmd->timeout = NVMEIBC_JAM_CMD_TIMEOUT;
	gen_cmd->disk = disk;

	_NTj(trace_jam_jentry_erase, disk,
		"NVMEIB_GEN_OP_JENTRY_ERASE (@GEN_CMD_OP), "
		"Range: @JRNL_RNG_IDX, "
		"Entry: @JRNL_RNG_ENT_IDX, "
		"GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID, "
		"LBA: @SW_LBA, n_attempts=@INT",
		NVMEIB_GEN_OP_JENTRY_ERASE,
		gen_cmd->param.je.rng_idx, gen_cmd->param.je.ent_erase.ent_idx,
		gen_cmd->param.je.rng_gen_id, gen_cmd->param.je.ent_erase.ent_idx,
		gen_cmd->param.je.ent_erase.ent_swlba, jidx->n_erase_attempts);

	rv = icore_ops->execute_gen(icore_ops, disk, gen_cmd);

out:
	if (rv) {
		_NEj(error_2_jam_jentry_erase, disk,
			 "Fail to issue jentry erase idx=@JRNL_RNG_ENT_IDX, "
			 "n_erase_attempts=@INT, rv=@RV", idx, jidx->n_erase_attempts, rv);
		kfree(gen_cmd);
		nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_JOURNAL_ENTRY_ERASE_FAILURE);
	}

	NFOUT;
	return rv;
}

int nvmeibc_jam_jentry_erase_comp(struct nvmeibc_disk_gen_cmd *gen_cmd)
{
	struct nvmeibc_disk *disk = gen_cmd->disk;
	int idx;
	int rv = -1;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	NFIN;

	idx = gen_cmd->param.je.ent_erase.ent_idx;
	if (!gen_cmd->comp_code) {
		rv = jidx_event(disk, idx, NVMEIBC_JIDX_EVT_ERASE_COMP);
	}
	else {
		_NEj(error_jam_nvmeibc_jam_jentry_erase_comp, disk, "Jentry erase idx=@JRNL_RNG_ENT_IDX completed with error @COMP_CODE",
		   idx, gen_cmd->comp_code);
		rv = jidx_event(disk, idx, NVMEIBC_JIDX_EVT_ERASE_COMP_ERR);
	}

	// For block simulation above code must run before dec_transfers (otherwise
	// jam_disk can be released)
	icore_ops->cb_called_cmd(icore_ops, disk, &gen_cmd->disk_cmd);

	kfree(gen_cmd);
	NFOUT;
	return rv;
}

/*
 * Returns 0 if event triggered legal state transition
 */
static int jidx_event_(struct nvmeibc_disk *disk, int idx,
					   u8 *idx_gen_id,
					   enum nvmeibc_jam_jidx_event evt,
					   bool *put_back, int *n_jreqs)
{
	struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
	struct nvmeibc_jam_jidx *jidx = &jam_disk->jidx_pool[idx];
	enum nvmeibc_jam_jidx_state state = jidx->state;
	int rv = -EBADE;
	bool goodpath = false;
	NFIN;

	BUG_ON(!put_back);
	BUG_ON(!n_jreqs);
	*put_back = false;
	*n_jreqs = 0;

	_NDj(trace_jam_jidx_event, disk, "journal idx @JRNL_RNG_ENT_IDX, state: @JIDX_STATE_TO_STR(@STATE) event=@EVENT_STR(@EVT) committed=(txid=@TXID j2b=@J2B) transient=(txid=@TXID j2d=@J2D)",
		idx, jidx_state_to_str(jidx->state), jidx->state,
		jidx_event_to_str(evt), evt,
		jidx->hkeys.committed.tx_id, jidx->hkeys.committed.j2b,
		jidx->hkeys.transient.tx_id, jidx->hkeys.transient.j2b);

	switch (evt) {
	case NVMEIBC_JIDX_EVT_END_USE:
		if (state == NVMEIBC_JIDX_STS_ALLOCED) {
			jam_disk->n_alloced--;
			debug_alloc_end(jidx, jam_disk);
			*n_jreqs = unlink_jidx_from_any_jreq(jidx);
			uniqj2b_commit_transient_(jam_disk, jidx);

			/* new state */
			jidx->state = NVMEIBC_JIDX_STS_FREE;
			*put_back = true;
			goodpath = true;
			rv = 0;
		}
		break;

	case NVMEIBC_JIDX_EVT_ERASE:
		if (state == NVMEIBC_JIDX_STS_ALLOCED) {
			jam_disk->n_alloced--;
			debug_alloc_end(jidx, jam_disk);
			uniqj2b_hadd_transient_(jam_disk, jidx);

			/* new state */
			jidx->state = NVMEIBC_JIDX_STS_ERASING;
			jam_disk->n_erasing++;
			debug_erase_start(jidx, jam_disk);
			jam_cnts_on_jent_to_erase(jam_disk);
			jidx->n_erase_attempts = 0;

			/* This will acquire disk's lock while holding jam-disk's lock,
			   we allow it as the reversed order does not exists (yet!)  */
			rv = jentry_erase(disk, jidx);
		}
		break;

	case NVMEIBC_JIDX_EVT_ERASE_COMP:
		if (state == NVMEIBC_JIDX_STS_ERASING) {
			jam_disk->n_erasing--;
			debug_erase_end(jidx, jam_disk);
			*n_jreqs = unlink_jidx_from_any_jreq(jidx);
			uniqj2b_hdel_both(jam_disk, jidx);
			jam_cnts_on_jent_to_erase_comp(jam_disk);

			/* new state */
			jidx->state = NVMEIBC_JIDX_STS_FREE;
			*put_back = true;
			rv = 0;
		}
		break;

	case NVMEIBC_JIDX_EVT_ERASE_COMP_ERR:
		if (state == NVMEIBC_JIDX_STS_ERASING) {
			_NTj(trace_1_jam_jidx_event_jerase_err, disk,
				 "Fail comp jentry erase idx=@JRNL_RNG_ENT_IDX", idx);
			rv = jentry_erase(disk, jidx);
		}
		break;

	case NVMEIBC_JIDX_EVT_ABANDON:
		if (state == NVMEIBC_JIDX_STS_ALLOCED) {
			jam_disk->n_alloced--;
			debug_alloc_end(jidx, jam_disk);
			*n_jreqs = unlink_jidx_from_any_jreq(jidx);
			uniqj2b_commit_transient_(jam_disk, jidx);

			/* new state */
			jidx->state = NVMEIBC_JIDX_STS_ABANDONED;
			jidx->abnd_jif = jiffies;
			jam_disk->n_abandoned++;
			jam_cnts_on_jent_to_abnd(jam_disk);
			if (idx_gen_id) {
				*idx_gen_id = jidx->gen_id;
				BUG_ON(jidx->gen_id < nvmeib_jrnl_ent_gen_id_min ||
						jidx->gen_id > nvmeib_jrnl_ent_gen_id_max);
			}
			rv = 0;
			_NDj(trace_1_jam_jidx_event, disk, "ABANDON entry @JRNL_RNG_ENT_IDX", idx);
		}
		break;

	case NVMEIBC_JIDX_EVT_FREE_ABND:
		if (state == NVMEIBC_JIDX_STS_ABANDONED) {
			jam_disk->n_abandoned--;
			*n_jreqs = unlink_jidx_from_any_jreq(jidx);
			uniqj2b_hdel_committed_(jam_disk, jidx);
			jam_cnts_on_jent_to_a2f(jam_disk);

			/* new state */
			jidx->state = NVMEIBC_JIDX_STS_FREE;
			BUG_ON(!idx_gen_id);
			if (idx_gen_id) {
				BUG_ON(*idx_gen_id < nvmeib_jrnl_ent_gen_id_min ||
						*idx_gen_id > nvmeib_jrnl_ent_gen_id_max);
				jidx->gen_id = *idx_gen_id;
			}
			*put_back = true;
			rv = 0;
		}
		/* else, caution!!!
		   if state is ERASING, do nothing (return err) as comp may still arrive */
		break;

	default:
		_NTj(trace_2_jam_jidx_event, disk, "unknown event @EVENT", evt);
		break;
	}

	if (rv >= 0) {
		BUG_ON(rv > 0);
		if (jidx->state != state) {
			jidx->state_chg_jif = jiffies;
		}
		if (!goodpath) {
			_NTj(trace_3_jam_jidx_event, disk, "journal idx @JRNL_RNG_ENT_IDX, event @JIDX_EVENT_TO_STR(@EVT), rv: @RV, state: @JIDX_STATE_TO_STR(@STATE) --> @JIDX_STATE_TO_STR(@STATE)",
			idx, jidx_event_to_str(evt), evt, rv, jidx_state_to_str(state), state,
			jidx_state_to_str(jidx->state), jidx->state);
		}
	}
	else {
		_NWj(warn_jam_jidx_event, disk, "journal idx @JRNL_RNG_ENT_IDX, failed (rv=@RV) to handle event @JIDX_EVENT_TO_STR(@EVT), "
				  "state=@STATE_STR(@STATE)",
		   idx, rv, jidx_event_to_str(evt), evt,
		   jidx_state_to_str(state), state);
	}

	NFOUT;
	return rv;
}

static int jidx_event_vec(struct nvmeibc_disk *disk,
	int *idx_arr, u8 *idx_gen_id_arr, int *idx_rv_arr, int n_idx,
	enum nvmeibc_jam_jidx_event evt)
{
	struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
	struct nvmeibc_jam_jidx *jidx;
	struct nvmeibc_jam_pending_req *jreq;
	bool put_back;
	int n_jreqs, j;
	ulong flags = 0;
	int rv = 0, i, idx, *idx_rv_ptr, idx_rv = 0;
	LIST_HEAD(resume_list);
	const bool already_locked = jam_disk_already_locked(jam_disk);
	u8 idx_gen_id = NVMEIB_EC_INVALID_JOURNAL_ENT_GEN_ID;
	NFIN;

	if (!already_locked)
		jam_disk_spin_lock_irqsave(jam_disk, &flags);

	for (i = 0; i < n_idx; i++) {
		idx = idx_arr[i];
		if (idx_gen_id_arr)
			idx_gen_id = idx_gen_id_arr[i];
		idx_rv_ptr = idx_rv_arr ? &idx_rv_arr[i] : &idx_rv;
		_NDj(trace_jam_jidx_event_vec, disk, "journal idx @JRNL_RNG_ENT_IDX, event: @JIDX_EVENT_TO_STR(@EVT) (from '@__BUILTIN_RETURN_ADDRESS')",
			idx, jidx_event_to_str(evt), evt, __builtin_return_address(0));

		/* checks */
		if (unlikely(idx < 0 || idx >= jam_disk->jidx_pool_size)) {
			_NWj(warn_jam_jidx_event_vec, disk, "invalid journal idx @JRNL_RNG_ENT_IDX, pool size @JIDX_POOL_SIZE",
				idx, jam_disk->jidx_pool_size);
			WARN_ON_ONCE(1);
			*idx_rv_ptr = -EINVAL;
			rv |= *idx_rv_ptr;
			continue;
		}
		jidx = &jam_disk->jidx_pool[idx];
		if (unlikely(!list_empty(&jidx->pool_link))) {
			_NWj(warn_1_jam_jidx_event_vec, disk, "journal idx @JRNL_RNG_ENT_IDX, event: @JIDX_EVENT_TO_STR(@EVT) while in free-pool",
				idx, jidx_event_to_str(evt), evt);
			*idx_rv_ptr = -EINVAL;
			rv |= *idx_rv_ptr;
			continue;
		}

		/* process event */
		*idx_rv_ptr = jidx_event_(disk, idx, &idx_gen_id, evt, &put_back, &n_jreqs);
		rv |= *idx_rv_ptr;

		if (!(*idx_rv_ptr)) {
			if (put_back) {
				jidx_put_(jam_disk, jidx);
			}
			/* We have n_jreqs(0-2) due to collision and 1 for the case there wasn't a free jentry */
			for (j = 0; j < n_jreqs + 1; j++) {
				if (!RB_EMPTY_ROOT(&jam_disk->pending_root) &&
					(jreq = find_jreq_to_resume_(jam_disk))) {
					list_add_tail(&jreq->tmp_link, &resume_list);
				}
			}
			if (idx_gen_id_arr)
				idx_gen_id_arr[i] = idx_gen_id;
		}
	}

	if (!already_locked)
		jam_disk_spin_unlock_irqrestore(jam_disk, flags);

	while (!list_empty(&resume_list)) {
		jreq = list_first_entry(&resume_list,
		struct nvmeibc_jam_pending_req, tmp_link);
		list_del_init(&jreq->tmp_link);
		del_timer_sync(&jreq->timeout_timer); //omril: why can we do it from non-process ctx
		jreq->timeout_ctx = NULL;
		/* we assume to be run under preempt_disable() or pinned kthread (ipoller) */
		if (NVMEIBC_DISK_SHOULD_DEFER_TO_PCPU_WQ(nvmeibc_jam_use_system_pcpu_wq && !NVMEIBC_DISK_SAFE_TEST_CURRENT_CPU_IN_BITMAP(&jreq->cpu_mask_info->mask), jreq->cpu_mask_info->mask)) {
			pending_req_resume_work_add(jam_disk, jreq);
		} else {
			pending_req_resume_bh(jreq);
		}
		/* @jreq is not valid here */
	}

	NFOUT;
	return rv;
}

static void jam_cnts_sum(struct nvmeibc_jam *c_jam, struct nvmeibc_jam_percpu_cnts *sum);

#if DEBUG_JAM
static void nvmeibc_jam_log_metrics(struct nvmeibc_disk *disk)
{
	struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
	struct nvmeibc_jam *c_jam = cdisk2cj(disk);
	struct nvmeibc_jam_percpu_cnts sum;
	ulong flags;
	NFIN;

	jam_lock(c_jam, flags);
	jam_cnts_sum(c_jam, &sum);
	jam_unlock(c_jam, flags);

	JAM_LOG_METRICS(jam_metrics_periodic, disk,
			"Avg. free(sum/count)=(@UINT64_TD/@UINT64_TD), "
			"Avg. alloced(sum/count)=(@UINT64_TD/@UINT64_TD), "
			"Avg. erasing(sum/count)=(@UINT64_TD/@UINT64_TD), "
			"Alloced (count/jifs)=(@UINT64_TD/@UINT64_TD), "
			"Erasing (count/jifs)=(@UINT64_TD/@UINT64_TD), "
			"Pending timeouts=@UINT64_TD, "
			"Pending reason (empty/max_used/bound)=(@UINT64_TD/@UINT64_TD/@UINT64_TD), "
			"Pending bound (alloced/abandoned/erasing/unexpected)=(@UINT64_TD/@UINT64_TD/@UINT64_TD/@UINT64_TD), "
			"Bound to transient=@UINT64_TD",
			jam_disk->n_free_avg_sum, jam_disk->n_free_avg_cnt,
			jam_disk->n_alloced_avg_sum, jam_disk->n_alloced_avg_cnt,
			jam_disk->n_erasing_avg_sum, jam_disk->n_erasing_avg_cnt,
			jam_disk->jif_alloced_cnt, jam_disk->jif_alloced_sum,
			jam_disk->jif_erasing_cnt, jam_disk->jif_erasing_sum,
			jam_disk->n_pending_timeout,
			jam_disk->n_pending_reason_empty_cnt, jam_disk->n_pending_reason_max_used_cnt, jam_disk->n_pending_reason_bound_cnt,
			jam_disk->n_pending_bound_alloced_cnt, jam_disk->n_pending_bound_abnded_cnt, jam_disk->n_pending_bound_earsing_cnt, jam_disk->n_pending_bound_unexp_cnt,
			jam_disk->n_bound_to_transient_cnt);

	JAM_LOG_METRICS(jam_metrics_periodic_per_cpu, disk,
			"n_ulp_trans_alloc_reqs=@UINT64_TD,"
			"n_ulp_trans_alloc_err=@UINT64_TD,"
			"n_ulp_trans_abnd_reqs=@UINT64_TD,"
			"n_ulp_jents_abnd=@UINT64_TD,"
			"jam_jents_rollback=@UINT64_TD,"
			"n_jam_jents_abnd=@UINT64_TD,"
			"n_jam_jents_cancel_alloced=@UINT64_TD,"
			"n_jam_jents_cancel_pending=@UINT64_TD",
			sum.n_ulp_trans_alloc_reqs, sum.n_ulp_trans_alloc_err, sum.n_ulp_trans_abnd_reqs,
			sum.n_ulp_jents_abnd, sum.n_jam_jents_rollback, sum.n_jam_jents_abnd,
			sum.n_jam_jents_cancel_alloced, sum.n_jam_jents_cancel_pending);

	NFOUT;
}
#endif

#define JAM_DISABLE_NON_FREE_TIMEOUT_SCAN 1

void nvmeibc_jam_on_periodic(struct nvmeibc_disk *disk)
{
	struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
	u64 non_free_timeout_jif = nvmeibc_jam_non_free_entry_timeout * HZ;
	u64 now = jiffies;
	u64 dt;
	int i;
	NFIN;

	/* checks */
	if (!on_wq_pid(disk->main_ach_wq_pid)) {
		_NTj(t0_jam_on_periodic, disk, "wrong wq");
		goto out;
	}
	if (disk->md_size == 0) {
		goto out;
	}
	if (!jam_disk) {
		_NTj(t1_jam_on_periodic, disk, "no jam-disk");
		goto out;
	}

#if DEBUG_JAM
	if (nvmeibc_jam_log_metrics_period) {
		u64 log_metrics_time_jif = nvmeibc_jam_log_metrics_period * HZ;

		if ((now - jam_disk->log_metrics_jif) > log_metrics_time_jif) {
			jam_disk->log_metrics_jif = now;
			nvmeibc_jam_log_metrics(disk);
		}
	}
#endif

	if (JAM_DISABLE_NON_FREE_TIMEOUT_SCAN)
		goto out;

	/* scan either non-free entries every fixed period or if any abandoned */
	dt = now - jam_disk->scan_non_free_jif;
	if (dt > non_free_timeout_jif) {
		jam_disk->scan_non_free_jif = now;
		if (jam_disk->n_free != jam_disk->jidx_pool_size)
			goto scan;
	}

 	if (jam_disk->n_abandoned == 0)
 		goto out;

scan:
	_NTj(t2_jam_on_periodic, disk,
		 "scan @INT non-free (alloced/abandoned=(@INT/@INT) entries (of @INT)",
		 jam_disk->jidx_pool_size - jam_disk->n_free,
		 jam_disk->n_alloced, jam_disk->n_abandoned, jam_disk->jidx_pool_size);

	/* scan w/o lock */
	for (i = 0; i < jam_disk->jidx_pool_size; i++) {
		struct nvmeibc_jam_jidx *jidx = &jam_disk->jidx_pool[i];
		if (jidx->state != NVMEIBC_JIDX_STS_FREE) {
			dt = now - jidx->state_chg_jif;
			if (dt > non_free_timeout_jif) {
				_NTj(t3_jam_on_periodic, disk,
					 "Found entry @INT in state @JIDX_STATE_TO_STR(@STATE) for "
					 "@LD(@LD) [jif(sec)] (timeout="
					 "@LD(@LD) [jif(sec)])",
					 i, jidx_state_to_str(jidx->state), jidx->state,
					 dt, dt / HZ, non_free_timeout_jif, non_free_timeout_jif / HZ);
				nvmeibc_disk_counters_inc(disk, n_err_jam_non_free_entry_timeout);
				nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_DISK_ABANDONED_JENTRY);
				break;
			}
		}
	}

out:
	NFOUT;
}

int nvmeibc_jam_abandon_lba(struct nvmeibc_disk *disk, u64 jlba, u8 *gen_id)
{
	int idx, rv;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	NFIN;

	jam_cnts_on_ulp_req_abnd(cdisk2cj(disk));

	if (!icore_ops->jam_get(icore_ops, disk)) {
		idx = lba_2_idx(disk, jlba);
		_NT(trace_jam_nvmeibc_jam_abandon_lba, "Disk @DISK_NAME(@DISK): abandon journal idx @IDX, jlba=@JLBA",
		   disk->name, disk, idx, (u32)jlba);
		if (unlikely(idx < 0)) {
			rv = idx;
		} else {
		#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
			if (unlikely(disk->jam_disk->jidx_pool[idx].skipped)) {
				WARN_ON(idx != JAM_SKIPPED_JIDX);
				rv = idx;
				*gen_id = nvmeib_jrnl_ent_gen_id_min;
			} else
		#endif
			if ((rv = jidx_event_vec(disk, &idx, gen_id, NULL, 1, NVMEIBC_JIDX_EVT_ABANDON)) < 0)
				_NW(nvmeibc_jam_abandon_lba_w23, "Disk @STR(@PTR): abandon "
					"journal idx @INT, jlba=@INT64, failed: @INT",
					disk->name, disk, idx, jlba, rv);
			else {
				rv = idx;
			}
		}
		icore_ops->jam_put(icore_ops, disk);
	}
	else {
		rv = -1;
		_NT(trace_1_jam_nvmeibc_jam_abandon_lba, "Fail to get pausable approval for disk @DISK", disk);
	}

	NFOUT;
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_abnd_free_base_decode)
{
	const struct volume_server_cmd_jmd_free_abnd_base *base = wire_buf;
	struct abnd_free_decode_ctx *ctx = arg;
	struct nvmeibc_disk *disk = ctx->disk;
	int i, rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	ctx->rng_num = be32_to_cpu(base->rng_num);
	ctx->rng_gen_id = be64_to_cpu(base->rng_gen_id);

	for (i = 0; i < NUM_JENTS_JAM_USES_IN_JRI(disk); i++) {
		if (nvmeib_test_bit_be32(i, base->abnd_free_bitmap)) {
			if (i >= disk->jam_disk->jidx_pool_size) {
				_NE(error_2_vex_ach_abnd_free_base_decode,
					"Invalid a2f-bitmap=" NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE
					"out-of-range found in bit @INT", base->abnd_free_bitmap, i);
				WARN_ON(1);
				rv = -EINVAL;
				goto out;
			}
			ctx->free_idx_gen_id[ctx->abnd_free_idx_n] = base->free_ent_md[i].ent_gen_id;
			ctx->abnd_free_idx_arr[ctx->abnd_free_idx_n++] = i;
		}
	}
	rv = sizeof(*base);
out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_abnd_free_ext1_decode)
{
	const struct volume_server_cmd_jmd_free_abnd_ext1 *ext1 = wire_buf;
	struct abnd_free_decode_ctx *ctx = arg;

	if (!wire_buf) {
		/* Extension not present, set defaults */
		ctx->binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		return 0;
	} else {
		/* Decode */
		BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);
		ctx->binje = binje_from_be(ext1->binje);
	}
	return sizeof(*ext1);
}

int process_jmd_free_abnd_decoded(struct nvmeibc_disk *disk,
	       struct abnd_free_decode_ctx *decode_ctx)
{
	int rv;
	if (decode_ctx->rng_num != disk->jour.rng_id) {
		_NE(error_jam_process_jmd_free_abandoned,
			"Invalid journal range number @RNG_NUM", decode_ctx->rng_num);
		rv = -EINVAL;
		goto out;
	}

	if (decode_ctx->binje != disk->jour.rng_binje) {
		_NE(error_jam_process_jmd_free_abnd_binje_mismatch,
				"Invalid N @BINJE for range number @RNG_NUM",
				decode_ctx->binje, disk->jour.rng_binje);
		BUG_ON(1);
		rv = -EINVAL;
		goto out;
	}

	if (decode_ctx->rng_gen_id != disk->jour.rng_gen_id) {
		struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
		ulong flags = 0;
		/* Should only increase */
		BUG_ON(decode_ctx->rng_gen_id < disk->jour.rng_gen_id);
		jam_disk_spin_lock_irqsave(jam_disk, &flags);
		_NT(trace_jam_process_jmd_free_abandoned_gen_id_inc,
			"GenID @JRNL_RNG_GEN_ID increased for range @JRNL_RNG_IDX",
			decode_ctx->rng_gen_id, disk->jour.rng_id);
		disk->jour.rng_gen_id = decode_ctx->rng_gen_id;
		jam_disk_spin_unlock_irqrestore(jam_disk, flags);
	}

	rv = jidx_event_vec(disk, decode_ctx->abnd_free_idx_arr, decode_ctx->free_idx_gen_id, NULL, decode_ctx->abnd_free_idx_n,
			NVMEIBC_JIDX_EVT_FREE_ABND) ? 1 : 0;
out:
	return rv;
}

/* This function is safe to process the request as it is don in recv-comp
   of nordda channel (disk not paused yet). But, any sub-sequent operation
   it may trigger must get pausable layer approval */
int process_jmd_free_abandoned(struct nvmeibc_disk *disk,
			       struct nvmeibc_ib_admin_channel *ch,
			       struct abnd2free *a2f)
{
	struct abnd_free_decode_ctx *decode_ctx = NULL;

	struct volume_server_cmd_jmd_free_abnd *j_req = &a2f->jreq;
	int rv = 0;
	NFIN;

	if (!(decode_ctx = kzalloc(sizeof(*decode_ctx), GFP_NOWAIT))) {
		_NE(error_1_jam_process_jmd_free_abandoned, "Memory allocation error");
		rv = -ENOMEM;
		goto out;
	}
	decode_ctx->disk = disk;

	if ((rv = CALL_VEX_OP(decode,
			vex_ach_abnd_free_clnt_ops, ONE_EXT,
				base, vex_ach_abnd_free_base_decode,
				ext1, vex_ach_abnd_free_ext1_decode,
					ch->base.vex_ops[vex_ach_abnd_free], j_req, j_req + 1, decode_ctx)) < 0) {
		goto out;
	}

	rv = process_jmd_free_abnd_decoded(disk, decode_ctx);

	if (!disk->jam_disk->alloc_enb && disk->jam_disk->n_abandoned == 0) {
		_NTj(trace_0_jam_process_jmd_free_abandoned, disk,
			 "Freed last anabdoned entry (prev_n_abandoned=@INT), "
			 "rediscover disk to get range with requested binje(@BINJE/@BINJE)",
			 disk->jam_disk->n_abandoned, disk->binje_ulp, disk->jour.rng_binje);
		nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_GET_REQUESTED_BINJE);
	}

out:
	kfree(decode_ctx);

	NFOUT;
	return rv;
}

/* This function is called from interrupt ctx */
int nvmeibc_jam_process_recv_comp(struct nvmeibc_disk *disk,
				  struct volume_server_req *req)
{
	int rv;
	NFIN;

	if (req->opcode == NVMEIBS_JAM_ABND2FREE) {
		rv = nvmeibc_ib_admin_schedule_abnd2free(disk, req);
	}
	else {
		_NWj(warn_jam_nvmeibc_jam_process_recv_comp, disk, "unknown jam recv-comp opcode @OPCODE",
		   req->hdr.opcode);
		rv = -1;
	}

	NFOUT;
	return rv;
}

static int disks_ptr_cmp_func(const void *p1, const void *p2)
{
	const struct jalloc *ja1 = p1;
	const struct jalloc *ja2 = p2;

	if (ja1->disk > ja2->disk)
		return 1;
	if (ja1->disk < ja2->disk)
		return -1;
	return 0;
}

static struct jalloc *sort_disks(int n_disks, struct nvmeibc_disk *disks[])
{
	struct jalloc *sorted = NULL;
	int i;
	NFIN;

	if (!(sorted = kzalloc(n_disks * sizeof(*sorted), GFP_ATOMIC))) {
		_NT(trace_jam_sort_disks, "Fail to dup input");
		goto out;
	}
	for (i = 0; i < n_disks; i++) {
		sorted[i].disk = disks[i];
		sorted[i].orig_pos = i;
		_NF(trace_1_jam_sort_disks, "&sorted[@JRNL_RNG_ENT_IDX]=@PTR: disk=@DISK, orig_pos=@ORIG_POS",
			i, &sorted[i], sorted[i].disk, sorted[i].orig_pos);
	}
	sort(sorted, n_disks, sizeof(*sorted), disks_ptr_cmp_func, NULL);

#if	DEBUG_JAM
	for (i = 0; i < n_disks; i++) {
		_NF(trace_2_jam_sort_disks, "&sorted[@JRNL_RNG_ENT_IDX]=@PTR: disk=@DISK, orig_pos=@ORIG_POS",
			i, &sorted[i], sorted[i].disk, sorted[i].orig_pos);

		if (i && sorted[i].disk < sorted[i - 1].disk) {
			_NE(error_jam_sort_disks, "OOPS, bad sort ([@JRNL_RNG_ENT_IDX]=@DISK < [@JRNL_RNG_ENT_IDX]=@DISK)",
				i, sorted[i].disk, i - 1, sorted[i - 1].disk);
			kfree(sorted);
			sorted = NULL;
			goto out;
		}
	}
#endif

out:
	NFOUT;
	return sorted;
}

static void jlba_put_unused(struct nvmeibc_disk *disk, u64 jlba)
{
	int idx;
	NFIN;

	idx = lba_2_idx(disk, jlba);
	if (idx < 0) {
		_NTj(trace_jam_jlba_put_unused, disk, "skip put-back of UNUSED");
	}
	else {
		_NDj(trace_1_jam_jlba_put_unused, disk, "put unused idx @JRNL_RNG_ENT_IDX", idx);
		jidx_put_unused(disk->jam_disk, idx);
	}

	NFOUT;
}
/* Rollback partial allocation can potentialy take time in case we
   each disk in this jreq has its own pending-req, it will try to
   resume allocation frim this ctx. moreover if it fails at some
   point it will also rollback it own (resumed) partial allocation ...
   Consider deferring resume operation to work.
*/
/* This func can be called after passing pausable for ALL disks in @sorted */
static void rollback_partial_allocation(struct jalloc *sorted, u64 res_jlbas[], int n)
{
	int i = n;
	u64 lba;
	NFIN;

	_ND(trace_jam_rollback_partial_allocation, "Rollback alloc of first @N_DISKS disks", n);
	while (--i >= 0) {
		lba = res_jlbas[sorted[i].orig_pos];
		jlba_put_unused(sorted[i].disk, lba);
	}

	NFOUT;
}

void pd_rollback_partial_allocation(struct nvmeibc_jam_pending_req *jreq)
{
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	struct jalloc *sorted = jreq->sorted;
	u64 *res_jlbas = jreq->res_jlbas;
	int n = jreq->curr;
	int i = n;
	u64 lba;
	NFIN;

	while (--i >= 0) {
		if (!icore_ops->jam_get(icore_ops, sorted[i].disk)) {
			lba = res_jlbas[sorted[i].orig_pos];
			jlba_put_unused(sorted[i].disk, lba);
			icore_ops->jam_put(icore_ops, sorted[i].disk);
		}
		else
			_ND(trace_jam_pd_rollback_partial_allocation, "Pausable reject for disk @DISK", sorted[i].disk);
	}

	NFOUT;
}

/* This func can be called after passing pausable for ALL disks in @sorted */
static int lbas_alloc(int n_disks, struct jalloc *sorted, u64 res_jlbas[],
	u32 txid, bool wait_bound_abnd, void *ctx, struct nvmeibc_jam_pending_req *jreq, const struct nvmeib_cpu_mask_info *cpu_mask_info, unsigned long deadline_jif, unsigned long priority)
{
	struct nvmeibc_jam_disk *jam_disk;
	ulong flags;
	int orig_pos;
	jidx_hkey_t hkey = { .raw = 0 };
	int idx;
	u64 lba;
	int i;
	int rv = -EINVAL;
#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	bool skipped = (unlikely(!!nvmeibc_skip_lock_cmds_flags));
#endif
	NFIN;

	hkey.tx_id = txid;
	i = jreq ? jreq->curr : 0; /* jreq->curr is the disk AFTER the one we were pending for, otherwise we wouldnt have resume */
	WARN_ON_ONCE(i == n_disks);

	_ND(trace_jam_lbas_alloc, "Alloc start from disk @DISK_IDX (of @N_DISKS)", i, n_disks);
	for (; i < n_disks; i++) {
		rv = -EINVAL;
		orig_pos = sorted[i].orig_pos;
#if	0 //DEBUG_JAM
		if (res_jlbas[orig_pos] != ~(0ULL)) {
			_NWj(lbas_alloc_w7, sorted[i].disk, "res_jlbas[.] already set");
			WARN_ON_ONCE(1);
			rollback_partial_allocation(sorted, res_jlbas, i);
			rv = -EINVAL;
			break;
		}
#endif
		hkey.j2b = __j2d_to_j2b(res_jlbas[orig_pos]);
		BUG_ON(!__is_jidx_hkey_io_entry(hkey));
		jam_disk = sorted[i].disk->jam_disk;
		if (!jam_disk) {
			_NE(err_c_jam_lbas_alloc_null_jam_disk, "Disk @DISK_NAME(@DISK): JAM not initialized", sorted[i].disk->name, sorted[i].disk);
			rv = -EINVAL;
			goto rollback;
		}
		if (!jam_disk->alloc_enb) {
			_NE(err_c_jam_lbas_alloc_disb_jam_disk, "Disk @DISK_NAME(@DISK): alloc disabled", sorted[i].disk->name, sorted[i].disk);
			WARN_ON(1);
			rv = -ENOMEM;
			goto rollback;
		}
		jam_disk_spin_lock_irqsave(jam_disk, &flags);
	#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
		if(unlikely(skipped)) {
			jam_disk->jidx_pool[JAM_SKIPPED_JIDX].skipped = true;
			idx = JAM_SKIPPED_JIDX;
			rv = 0;
		} else
	#endif
		{
			rv = jidx_alloc_(jam_disk, hkey, &idx, wait_bound_abnd, false /*dry_run*/);
			jam_disk->jidx_pool[JAM_SKIPPED_JIDX].skipped = false;
		}
		if (!rv) {
			lba = idx_2_lba(sorted[i].disk, idx);
			res_jlbas[orig_pos] = lba;
			_ND(trace_1_jam_lbas_alloc, "Disk @DISK_NAME(@DISK): allocated journal idx @JRNL_RNG_ENT_IDX, jlba=@JLBA",
			   sorted[i].disk->name, sorted[i].disk, idx, lba);
		}
		else if (rv == -EBUSY) {
			if (!nvmeibc_jam_pending_enb) {
				static u32 pending_disbled = 0;
				if ((pending_disbled & 0xFFFFF) == 0) {
					_NI_dmesg(trace_4_jam_lbas_alloc,
						"Pending disabled (@INT), rollback alloc...", pending_disbled);
				}
				pending_disbled++;
				rv = -ENOMEM;
			}
			else if (!jreq && !(jreq = pending_req_alloc(
				n_disks, sorted, res_jlbas, txid, wait_bound_abnd, deadline_jif, priority, ctx))) {
				_NT(trace_2_jam_lbas_alloc, "Fail to alloc pending req, rollback alloc...");
				rv = -ENOMEM;
			}
			else {
				/* see n_pending.* counters for details */
				_NDj(trace_3_jam_lbas_alloc, jam_disk->disk,
					 "Adding pending req @JREQ (curr=@CURR_INT, bound_idx=@BOUND_IDX)",
					jreq, i, idx);
				jreq->curr = i;
				jreq->hkey = hkey;
				jreq->cpu_mask_info = cpu_mask_info;
				link_jreq_jidx(jam_disk, jreq, idx);
				pending_req_list_add_(jreq);
				rv = -EINPROGRESS;
			}
		}
		jam_disk_spin_unlock_irqrestore(jam_disk, flags);

		if (rv == -EINPROGRESS) {
			break;
		}

rollback:
		if (rv < 0) {
			rollback_partial_allocation(sorted, res_jlbas, i);
			break;
		}
	}

	if (!rv) {
		/* update both disks & trans stats */
		jam_cnts_on_alloc_ok(n_disks, sorted);
	}

	NFOUT;
	return rv;
}

/* bottom-half of the resume operation.
   continue or abort allocation */
static void pending_req_resume_bh(struct nvmeibc_jam_pending_req *jreq)
{
	int n_disks = jreq->n_disks;
	struct nvmeibc_disk **disks = 0;
	struct jalloc *sorted = jreq->sorted;
	u64 *res_jlbas = jreq->res_jlbas;
	int i;
	int rv = -1;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	NFIN;

	/* on any of these assertions, we leak invalid/zombie jreq its already
	   allocated jentries, if any */
	if (n_disks <= 0) {
		_NW(w0_jam_pending_req_resume_bh, "n_disks: @N_DISKS", n_disks);
		WARN_ON_ONCE(1);
		goto out;
	}
	if (!RB_EMPTY_NODE(&jreq->pending_node)) {
		_NW(warn_jam_pending_req_resume_bh, "oops, jreq still linked");
		WARN_ON_ONCE(1);
		goto out;
	}
	if (jreq->curr > n_disks) {
		_NW(warn_1_jam_pending_req_resume_bh, "jreq: curr > ndisks (@CURR_INT, @N_DISKS)",
			jreq->curr, n_disks);
		WARN_ON_ONCE(1);
		goto out;
	}

	/* timeout */
	if (jreq->timeout_expired) {
		/* see n_pending.* counters for details */
		_ND(warn_2_jam_pending_req_resume_bh,
			"Expired @ @EXPIRES, Pending @ @PEND_JIF, Now: @_JIFFIES, abort resume, bound_idx=@BOUND_IDX",
			jreq->timeout_timer.expires, (u64)jreq->pend_jif, jiffies, jreq->bound_idx);
		pd_rollback_partial_allocation(jreq);
		goto done;
	}

	if (!(disks = kmalloc(sizeof(*disks) * n_disks, GFP_ATOMIC))) {
		_NW(warn_0_jam_pending_req_resume_bh,
			"OOM (n_disks: @N_DISKS)", n_disks);
		WARN_ON_ONCE(1);
		pd_rollback_partial_allocation(jreq);
		goto done;
	}

	/* First check that ALL disks are still not paused/ing */
	for (i = 0; i < n_disks; i++)
		disks[i] = sorted[i].disk;
	if (icore_ops->jam_get_all(icore_ops, n_disks, disks)) {
		_NT(trace_jam_pending_req_resume_bh, "Fail to get pausable approval for all disks, "
			"rollback alloc of pausable disks only");
		pd_rollback_partial_allocation(jreq);
		goto done;
	}
	//TBD: merge this if into lbas-alloc
	if (jreq->curr == n_disks) {
		jam_cnts_on_alloc_ok(n_disks, sorted);
	}
	rv = (jreq->curr < n_disks) ?
		lbas_alloc(n_disks, sorted, res_jlbas, jreq->txid, jreq->wait_bound_abnd, jreq->ctx, jreq, jreq->cpu_mask_info, jreq->deadline_jif, jreq->priority) : 0;
	icore_ops->jam_put_all(icore_ops, n_disks, disks);

	if (rv == -EINPROGRESS)
		goto out;

done:
	/* here we should have already rolled-back the allocation... */
	if (likely(!rv)) {
		/* both disks & trans stats were updated */
		nvmeibc_block_dp_ec_journal_alloc_cb(rv, res_jlbas, jreq->ctx);
	}
	else {
		/* only per disk stats were updated via rollback-partial-allocation logic */
		jam_cnts_on_alloc_err(cdisk2cj(jreq->sorted[0].disk));
		nvmeibc_block_dp_ec_journal_alloc_cb(rv, NULL, jreq->ctx);
	}
	pending_req_free(jreq);

out:
	if (disks)
		kfree(disks);
	NFOUT;
	return;
}

int nvmeibc_jam_lbas_alloc(int n_disks, struct nvmeibc_disk *disks[], u32 txid,
	u64 dlbas[], u64 res_jlbas[], bool wait_bound_abnd, const struct nvmeib_cpu_mask_info *cpu_mask_info, unsigned long deadline_jif, unsigned long priority, void *ctx)
{
	struct nvmeibc_jam *c_jam = cdisk2cj(disks[0]);
	struct jalloc *sorted = NULL;
	const unsigned long max_timeout_jif = (nvmeibc_jam_pending_req_timeout_jif ? : NVMEIBC_PENDING_REQ_TIMEOUT);
	const unsigned long now_jif = jiffies;
	const unsigned long capped_deadline_jif = (deadline_jif > now_jif + max_timeout_jif) ? now_jif + max_timeout_jif : deadline_jif;
	int rv = -1;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	NFIN;

	jam_cnts_on_ulp_req_alloc(c_jam, n_disks);

	if (!(sorted = sort_disks(n_disks, disks)))
		goto out;

#if	0 //DEBUG_JAM
	memset(res_jlbas, 0xff, sizeof(res_jlbas[0]) * n_disks);
#endif
	/* initialize with j2d, used for hkey */
	memcpy(res_jlbas, dlbas, sizeof(res_jlbas[0]) * n_disks);

	if (icore_ops->jam_get_all(icore_ops, n_disks, disks)) {
		_NT(trace_jam_nvmeibc_jam_lbas_alloc, "Fail to get pausable approval for all disks");
		goto done;
	}

	rv = lbas_alloc(n_disks, sorted, res_jlbas, txid, wait_bound_abnd, ctx, NULL, cpu_mask_info, capped_deadline_jif, priority);
	icore_ops->jam_put_all(icore_ops, n_disks, disks);

	if (rv == -EINPROGRESS)
		goto out;

done:
	if (unlikely(rv)) {
		/* only per disk stats were updated via rollback-partial-allocation logic */
		jam_cnts_on_alloc_err(c_jam);
	}

	kfree(sorted);

out:
	NFOUT;
	return rv;
}

void nvmeibc_jam_lbas_free(int n_disks, struct nvmeibc_disk *disks[], u64 jlbas[], u32 wr_sts_bm)
{
	struct jalloc *sorted = NULL;
	int i, orig_pos, idx;
	u64 lba;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	NFIN;

	jam_cnts_on_ulp_req_free(cdisk2cj(disks[0]), n_disks);

	BUG_ON(n_disks > (int)(sizeof(wr_sts_bm) * BITS_PER_BYTE));

	if ((sorted = sort_disks(n_disks, disks))) {
		for (i = 0; i < n_disks; i++) {
			if (!icore_ops->jam_get(icore_ops, sorted[i].disk)) {
				orig_pos = sorted[i].orig_pos;
				lba = jlbas[orig_pos];
				idx = lba_2_idx(sorted[i].disk, lba);
				if (likely(idx >= 0)) {
				#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
					if (likely(!(sorted[i].disk->jam_disk->jidx_pool[idx].skipped)))
				#endif
					{
						_ND(trace_jam_nvmeibc_jam_lbas_free, "Disk @DISK_NAME(@DISK): free journal idx @JRNL_RNG_ENT_IDX, jlba=@JLBA",
					sorted[i].disk->name, sorted[i].disk, idx, lba);
						jidx_event(sorted[i].disk, idx, !(wr_sts_bm & (1 << orig_pos)) ?
							NVMEIBC_JIDX_EVT_END_USE : NVMEIBC_JIDX_EVT_ERASE);
					}
				}
				icore_ops->jam_put(icore_ops, sorted[i].disk);
			}
			else
				_ND(trace_1_jam_nvmeibc_jam_lbas_free, "Pausable reject for disk @DISK", sorted[i].disk);
		}
		kfree(sorted);
	}

	NFOUT;
}

static void jam_disk_cnts_sum(struct nvmeibc_jam_disk *jam_disk,
							  struct nvmeibc_jam_disk_percpu_cnts *sum);

#define CORE_CLIENT_JAM_DISK_PROC_FRMT_VER 1
static int __jam_disk_locked_to_string(const struct nvmeibc_jam_disk *J, int d, char* buf, int len, bool all)
{
	#define BUF_ADD(...) cnt += scnprintf(buf+cnt, len-cnt, __VA_ARGS__)
	int cnt = 0, e = 0;
	int n_pending = 0;
	struct nvmeibc_jam_jidx *jidx = NULL;
	struct nvmeibc_jam_pending_req *jreq;
	struct rb_node *node;
	struct nvmeibc_disk *cdisk = J->disk;
	struct nvmeibc_jam_disk_percpu_cnts sum = {0};
	u64 alloced_avg = J->n_alloced_avg_cnt ?
		J->n_alloced_avg_sum / J->n_alloced_avg_cnt : 0;
	u64 erasing_avg = J->n_erasing_avg_cnt ?
		J->n_erasing_avg_sum / J->n_erasing_avg_cnt : 0;
	u64 free_avg = J->n_free_avg_cnt ?
		J->n_free_avg_sum / J->n_free_avg_cnt : 0;
	u64 alloced_avg_jif = J->jif_alloced_cnt ?
		J->jif_alloced_sum / J->jif_alloced_cnt : 0;
	u64 erasing_avg_jif = J->jif_erasing_cnt ?
		J->jif_erasing_sum / J->jif_erasing_cnt : 0;
	BUF_ADD("\\-+-%3d) %s(%p), RngID=%04u Binje=%u(%u), Free/Abnd Init = %d/%d, "
			"Alloced/Abandoned/Erasing/Free[Total] = %d/%d/%d/%d[%d], "
			"Alloced/Erasing/Free Avg: %llu/%llu/%llu, "
			"Alloced/Erasing Avg Jif: %llu/%llu, "
			"Pending: %d\n",
			d, cdisk->name, cdisk, cdisk->jour.rng_id, cdisk->jour.rng_binje, cdisk->binje_ulp,
			J->pool_init.n_free, J->pool_init.n_abandoned,
			J->n_alloced, J->n_abandoned, J->n_erasing, J->n_free, J->jidx_pool_size,
			alloced_avg, erasing_avg, free_avg,
			alloced_avg_jif, erasing_avg_jif,
			J->n_pending);

	if (all) {
		BUF_ADD(" idx | state | committed-hkey... | transient-hkey... | since state [jif, sec] \n");
		for (e = 0, jidx = J->jidx_pool; e < J->jidx_pool_size; e++, jidx++) {
			if ((e & 0x1f) == 0) {
				BUF_ADD(" %-3s | %-5s | %-*s, %-*s, %-*s, %-*s | %-*s, %-*s, %-*s, %-*s | %-*s\n",
						"idx", "state",
						2+HKEY_TXID_LEN, "TxID", 2+HKEY_J2B_LEN, "J2B", U64_RAW_LEN, "raw", U64_RAW_LEN, "jreq",
						2+HKEY_TXID_LEN, "TxID", 2+HKEY_J2B_LEN, "J2B", U64_RAW_LEN, "raw", U64_RAW_LEN, "jreq",
						U64_RAW_LEN+2+U64_RAW_LEN, "since state [jif, sec]");
			}
			BUF_ADD(" %03d | %5s | 0x%0*x, 0x%0*x, %016llx, %p | 0x%0*x, 0x%0*x, %016llx, %p | %016llu, %016llu\n",
					jidx->idx, jidx_state_to_str_5(jidx->state),
					HKEY_TXID_LEN, jidx->hkeys.committed.tx_id,
					HKEY_J2B_LEN, jidx->hkeys.committed.j2b,
					jidx->hkeys.committed.raw, jidx->jreq_bound_via_committed,
					HKEY_TXID_LEN, jidx->hkeys.transient.tx_id,
					HKEY_J2B_LEN, jidx->hkeys.transient.j2b,
					jidx->hkeys.transient.raw, jidx->jreq_bound_via_transient,
					jiffies - jidx->state_chg_jif, (jiffies - jidx->state_chg_jif)/HZ);
			/* count pending */
			n_pending += !!(jidx->jreq_bound_via_committed);
			n_pending += !!(jidx->jreq_bound_via_transient);
		}
		if (n_pending != J->n_pending) {
			BUF_ADD("ERR: Found %d pending, Exp %d\n", n_pending, J->n_pending);
		}

		jam_disk_cnts_sum((void *)J, &sum);
		BUF_ADD("calcs: diff1 = alloced - rollback; diff2 = alloc_ulp - diff1\n");
		BUF_ADD("-\n! "
				"Now={n_free=%d, n_alloc=%d, n_abnd=%d, n_erasing=%d}, "
				"Total={{alloc_ulp=%llu, {alloced=%llu, rollback=%llu, diff1=%lld}, diff2=%lld}, free=%llu, erase=%llu, erase_comp=%llu, abnd=%llu, a2f=%llu}\n",
				J->n_free, J->n_alloced, J->n_abandoned, J->n_erasing,
				sum.tot_alloc_ok, J->tot_alloc, J->tot_rollback, J->tot_alloc - J->tot_rollback,
				sum.tot_alloc_ok - (J->tot_alloc - J->tot_rollback), J->tot_free,
				J->tot_erase, J->tot_erase_comp, J->tot_abnd, J->tot_a2f);

		BUF_ADD("-\n! Pending: Count=%d, Timeouts=%llu, Reason={empty=%llu, bound=%llu, max-used(mod-param)=%llu}, "
				"Bound_idx's state={alloced=%llu, abnded=%llu, erasing=%llu, unexp=%llu}\n",
				J->n_pending, J->n_pending_timeout,
				J->n_pending_reason_empty_cnt, J->n_pending_reason_bound_cnt,
				J->n_pending_reason_max_used_cnt,
				J->n_pending_bound_alloced_cnt,
				J->n_pending_bound_abnded_cnt,
				J->n_pending_bound_earsing_cnt,
				J->n_pending_bound_unexp_cnt);

		e = 0;
		for (node = rb_first(&J->pending_root); node; node = rb_next(node)) {
			jreq = container_of(node, struct nvmeibc_jam_pending_req, pending_node);
			BUF_ADD("[%03d] jreq %p : {hkey=%llx, bound_idx=%03d}\n", e, jreq, jreq->hkey.raw, jreq->bound_idx);
			if (e++ == J->n_pending) {
				BUF_ADD("ERR: Too many elements in pending-list, Exp %d\n", J->n_pending);
				break;
			}
		}
	}

	#if 0
	list_for_each_entry(jidx, &J->free_list, pool_link) {
		BUF_ADD("  \\-%3d) %03d=%5s\n", e, jidx->idx, jidx_state_to_str_5(jidx->state));
		e++;
	}
	#else


	#endif

	cnt += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_JAM_DISK_PROC_FRMT_VER, buf+cnt, len-cnt);

	return cnt;
}

#define JAM_CNTS_SUM_PCPU(_sum, _pcpu, _name) \
do { _sum->_name += _pcpu->_name; } while (0)

static void jam_disk_cnts_sum(struct nvmeibc_jam_disk *jam_disk,
							  struct nvmeibc_jam_disk_percpu_cnts *sum)
{
	struct nvmeibc_jam_disk_percpu_cnts *pcpu;
	int cpu;

	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(jam_disk->pcpu_cnts, cpu);
		JAM_CNTS_SUM_PCPU(sum, pcpu, tot_alloc_ok);
	}
}

static void jam_cnts_sum(struct nvmeibc_jam * c_jam,
				  struct nvmeibc_jam_percpu_cnts *sum)
{
	struct nvmeibc_jam_percpu_cnts *pcpu;
	int cpu;

	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(c_jam->pcpu_cnts, cpu);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_trans_alloc_reqs);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_trans_alloc_ok);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_trans_alloc_err);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_trans_free_reqs);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_trans_abnd_reqs);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_jents_alloc_ok);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_jents_free);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_ulp_jents_abnd);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_alloc);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_rollback);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_alloc_ok);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_free);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_erase);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_erase_comp);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_abnd);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_a2f);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_cancel_alloced);
		JAM_CNTS_SUM_PCPU(sum, pcpu, n_jam_jents_cancel_pending);
	}
}

static int jam_cnts_fill(struct nvmeibc_jam * c_jam, char* buf, int len)
{
	struct nvmeibc_jam_percpu_cnts sum = {0};
	int cnt = 0;

	jam_cnts_sum(c_jam, &sum);

#define LINE_ADD(__counter) \
		BUF_ADD("%-*s: %llu\n", \
		30, #__counter, sum.__counter)

	LINE_ADD(n_ulp_trans_alloc_reqs);
	LINE_ADD(n_ulp_trans_alloc_ok);
	LINE_ADD(n_ulp_trans_alloc_err);
	LINE_ADD(n_ulp_trans_free_reqs);
	LINE_ADD(n_ulp_trans_abnd_reqs);
	BUF_ADD("-\n");
	LINE_ADD(n_ulp_jents_alloc_ok);
	LINE_ADD(n_ulp_jents_free);
	LINE_ADD(n_ulp_jents_abnd);
	BUF_ADD("-\n");
	LINE_ADD(n_jam_jents_alloc);
	LINE_ADD(n_jam_jents_rollback);
	LINE_ADD(n_jam_jents_alloc_ok);
	LINE_ADD(n_jam_jents_free);
	LINE_ADD(n_jam_jents_erase);
	LINE_ADD(n_jam_jents_erase_comp);
	LINE_ADD(n_jam_jents_abnd);
	LINE_ADD(n_jam_jents_a2f);
	LINE_ADD(n_jam_jents_cancel_alloced);
	LINE_ADD(n_jam_jents_cancel_pending);

	BUF_ADD("-\n");

	BUF_ADD("Jam uncompleted jouranl alloc transactions : %lld\n",
			sum.n_ulp_trans_alloc_reqs - (sum.n_ulp_trans_alloc_ok + sum.n_ulp_trans_alloc_err));

	BUF_ADD("Jam non-rolled-back journal entries allocs : %lld\n",
		 sum.n_jam_jents_alloc_ok -		(sum.n_jam_jents_alloc -
										 sum.n_jam_jents_rollback));

	BUF_ADD("Ulp non-returned journal entries [pre pd ] : %lld\n",
			sum.n_ulp_jents_alloc_ok - (sum.n_ulp_jents_free + sum.n_ulp_jents_abnd));

	BUF_ADD("Ulp non-returned journal entries [jam pov] : %lld\n",
			sum.n_jam_jents_alloc_ok -	(sum.n_jam_jents_free -
										 (sum.n_jam_jents_a2f +
										  sum.n_jam_jents_rollback +
										  sum.n_jam_jents_erase_comp) +
										 sum.n_jam_jents_erase +
										 sum.n_jam_jents_abnd +
										 sum.n_jam_jents_cancel_alloced +
										 sum.n_jam_jents_cancel_pending));
#undef LINE_ADD

	return cnt;
}

static inline int __all_jam_disks_locked_to_string(struct nvmeibc_jam *c_jam, char* buf, int len)
{
	ulong flags;
	int cnt = 0, d = 0;
	struct nvmeibc_jam_disk *jdisk;

	list_for_each_entry(jdisk, &c_jam->disks, jam_link) {
		jam_disk_spin_lock_irqsave(jdisk, &flags);
		cnt += __jam_disk_locked_to_string(jdisk, d, buf+cnt, len-cnt, false);
		d++;
		jam_disk_spin_unlock_irqrestore(jdisk, flags);
	}
	return cnt;
}

int nvmeibc_jam_fill_status(const struct nvmeibc_cinst_params_core *p, char* buf, int len)
{
	struct nvmeibc_jam *c_jam = __get_c_jam(p);
	ulong flags;
	int cnt = 0;

	BUF_ADD("\nJAM\n");
	BUF_ADD("*\n");
	jam_lock(c_jam, flags);
	BUF_ADD("n_disks=%d\n", c_jam->n_disks);
	cnt += __all_jam_disks_locked_to_string(c_jam, buf+cnt, len-cnt);
	BUF_ADD("*\n");
	cnt += jam_cnts_fill(c_jam, buf+cnt, len-cnt);
	jam_unlock(c_jam, flags);
	BUF_ADD("\n");
	return cnt;
}

void nvmeibc_jam_disk_cache_init(struct nvmeibc_disk *disk)
{
	NFIN;
	nvmeibc_disk_client_journal_mark_as_no_journal(&disk->jrc.jour);
	disk->jrc.n_jents = 0;
	bitmap_zero(disk->jrc.free_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	memset(disk->jrc.ent_md, NVMEIB_EC_INVALID_JOURNAL_ENT_GEN_ID,
		   sizeof(disk->jrc.ent_md));
	NFOUT;
}

static void jam_disk_cache_fill(struct nvmeibc_disk *disk)
{
	struct nvmeibc_jam_jidx *jidx_pool = disk->jam_disk->jidx_pool;
	int jidx_pool_size = disk->jam_disk->jidx_pool_size;
	int i;
	NFIN;

	disk->jrc.jour = disk->jour;
	disk->jrc.n_jents = jidx_pool_size;
	bitmap_zero(disk->jrc.free_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	for (i = 0; i < jidx_pool_size; i++) {
		if (jidx_pool[i].state == NVMEIBC_JIDX_STS_FREE) {
			set_bit(i, disk->jrc.free_bitmap);
		}
		disk->jrc.ent_md[i].ent_gen_id = jidx_pool[i].gen_id;
	}

	NFOUT;
}

void nvmeibc_jam_disk_cache_hton(struct nvmeibc_disk *disk,
	struct volume_client_config_jrange_cache *clnt_jrc)
{
	int n_jents = disk->jrc.n_jents;
	int i;

	NFIN;
	clnt_jrc->rng_id = cpu_to_be32(disk->jrc.jour.rng_id);
	clnt_jrc->rng_genid = cpu_to_be64(disk->jrc.jour.rng_gen_id);
	memset(clnt_jrc->free_bitmap, 0, sizeof(clnt_jrc->free_bitmap));
	nvmeib_bitmap_to_be32(clnt_jrc->free_bitmap, disk->jrc.free_bitmap, n_jents);
	for (i = 0; i < n_jents; i++)
		clnt_jrc->ent_md[i].ent_gen_id = disk->jrc.ent_md[i].ent_gen_id;
	memcpy(clnt_jrc->serjio_boot_id, disk->jrc.jour.serjio_boot_id, NVMEIB_GID_STR_MAX);
	/* jrange_req->binje_req is filled @ vex_ach_get_jrange_req_clnt_ext2_encode */
	NFOUT;
}

void nvmeibc_jam_disk_cache_local(struct nvmeibc_disk *disk,
	struct nvmeib_jrange_cache *clnt_jrc)
{
	int n_jents = disk->jrc.n_jents;
	int i;

	NFIN;
	clnt_jrc->rng_id = disk->jrc.jour.rng_id;
	clnt_jrc->gen_id = disk->jrc.jour.rng_gen_id;
	bitmap_zero(clnt_jrc->free_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	bitmap_copy(clnt_jrc->free_bmp, disk->jrc.free_bitmap, n_jents);
	for (i = 0; i < n_jents; i++)
		clnt_jrc->ent_md[i].ent_gen_id = disk->jrc.ent_md[i].ent_gen_id;
	memcpy(clnt_jrc->serjio_boot_id, disk->jrc.jour.serjio_boot_id, NVMEIB_GID_STR_MAX);
	clnt_jrc->rng_binje = disk->jrc.jour.rng_binje;
	NFOUT;
}

static ssize_t fill_jam_disk_status(void *priv, char *buf, size_t len)
{
	#define JAM_BUF_ADD(...) cnt += scnprintf(buf+cnt, len-cnt, __VA_ARGS__)
	struct nvmeibc_jam_disk *jam_disk = priv;
	struct nvmeibc_disk *disk;
	ssize_t cnt = 0;
	ulong flags;
	NFIN;

	if (!jam_disk) {
		JAM_BUF_ADD("Invalid jam-disk\n");
		goto out;
	}
	if (!(disk = jam_disk->disk)) {
		JAM_BUF_ADD("Invalid disk\n");
		goto out;
	}

	jam_disk_spin_lock_irqsave(jam_disk, &flags);
	cnt += __jam_disk_locked_to_string(jam_disk, 0, buf+cnt, len-cnt, true);
	jam_disk_spin_unlock_irqrestore(jam_disk, flags);

out:
	NFOUT;
	return cnt;
}

static inline int jam_disk_proc_create(struct nvmeibc_jam_disk *jam_disk,
									   struct nvmeibc_disk *disk)
{
	struct proc_dir_entry *dir = nvmeibc_get_proc_dir_jam(nvmeibc_cinst_get_core_p(disk));
	int rv = -1;
	NFIN;

	if (!dir) {
		_NE(error_jam_jam_disk_proc_create, "Oops, nvmeibc_jam_proc_dir NULL");
		goto out;
	}
	if (!jam_disk) {
		_NE(error_1_jam_jam_disk_proc_create, "Oops, jam-disk NULL");
		goto out;
	}

	if (!(jam_disk->proc_ent_status = nvmeib_public_proc_create(disk->name,
		dir, fill_jam_disk_status, NULL, jam_disk))) {
		_NE(error_2_jam_jam_disk_proc_create, "Fail to jam-disk proc entry: @STR", disk->name);
		goto out;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}

static inline void jam_disk_proc_destroy(struct nvmeibc_jam_disk *jam_disk)
{
	NFIN;

	if (jam_disk->proc_ent_status) {
		nvmeib_public_proc_remove(jam_disk->proc_ent_status);
		jam_disk->proc_ent_status = NULL;
	}

	NFOUT;
}

/*
 * @free_ents_bitmap 	- bitmap of size NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE/disk->jour.rng_binje
 * @jmdc_tbl			- array  of size NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE
 * @ent_md           	- array  of size NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE/disk->jour.rng_binje
 */
int nvmeibc_jam_disk_add(struct nvmeibc_disk *disk,
						 unsigned long *free_ents_bitmap,
						 union jblock_md *jmdc_tbl,
						 struct nvmeib_jrnl_ent_md *ent_md)
{
	struct nvmeibc_jam *c_jam = cdisk2cj(disk);
	unsigned jidx_pool_size;
	struct nvmeibc_jam_disk *jam_disk = NULL;
	struct nvmeibc_jam_jidx *jidx_pool = NULL;
	struct workq_struct *jwq = NULL;
	struct nvmeibc_jam_disk_percpu_cnts *pcpu = NULL;
	struct nvmeibc_jam_disk *jdisk;
	unsigned i;
	ulong flags;
	int rv = -1;
	proc_name_t pname;
	struct jentry_md jmdc_ent;

	NFIN;

	if (disk->jour.rng_id == NVMEIB_EC_INVALID_JOURNAL_RANGE ||
		disk->jour.rng_nblk > NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE ||
		disk->jour.rng_binje > NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY ||
		disk->jour.rng_binje > disk->jour.rng_nblk ||
		!is_power_of_2(disk->jour.rng_nblk) ||
		!is_power_of_2(disk->jour.rng_binje) ||
		!free_ents_bitmap) {
		_NTj(trace_jam_nvmeibc_jam_disk_add, disk, "invalid journal-info");
		goto out;
	} else {
		extern bool qa_ec_stress_debug;
		if (qa_ec_stress_debug) {
			MIN_WITH(disk->jour.n_ents, 8);		// Use only first few journals of each JRI
		}
	}
	_NTj(trace_1_jam_nvmeibc_jam_disk_add, disk,
		 "Allocate jam-disk from journal-info: "
		 "rng: id=@RNG_ID, n_ents: @N_ENTS, slba=@SLBA_LLONG, nlba=@NLBA, nblk=@NBLOCKS, binje=@BINJE, "
		 "Prev-rng={id=@RNG_ID, gid=@GID_LLONG}",
		 disk->jour.rng_id, disk->jour.n_ents, disk->jour.rng_slba, disk->jour.rng_nlba, disk->jour.rng_nblk,
		disk->jour.rng_binje, disk->jrc.jour.rng_id, disk->jrc.jour.rng_gen_id);

	//alloc
	jidx_pool_size = disk->jour.n_ents;
	BUG_ON(disk->jour.n_ents * disk->jour.rng_binje > disk->jour.rng_nblk);
	clnt_proc_name_format(pname, 'C', "WQ", "jd", nvmeibc_cinst_get_core_inst_num(nvmeibc_cinst_get_core_p(disk)));
	if (!(jam_disk = kzalloc(sizeof(*jam_disk), GFP_KERNEL)) ||
		!(jidx_pool = kzalloc(sizeof(*jidx_pool) * jidx_pool_size, GFP_KERNEL)) ||
		!(jwq = wq_create(pname)) ||
		!(pcpu = nvmeib_public_alloc_percpu_cacheline(struct nvmeibc_jam_disk_percpu_cnts))) {
		_NTj(trace_2_jam_nvmeibc_jam_disk_add, disk, "Fail to allocate jam-disk");
		goto err;
	}

	//init
	jam_disk->jidx_pool_size = jidx_pool_size;
	jam_disk->binje_shift = ilog2(disk->jour.rng_binje);
	jam_disk->jidx_pool = jidx_pool;
	jam_disk->jwq = jwq;
	jam_disk->pcpu_cnts = pcpu;
	spin_lock_init(&jam_disk->lock);
	INIT_LIST_HEAD(&jam_disk->free_list);
	jam_disk->pending_root = RB_ROOT;

	hash_init(jam_disk->uniqj2b_htable);
	INIT_LIST_HEAD(&jam_disk->uniqj2b_trans_list);
	init_waitqueue_head(&jam_disk->deferred_jreqs);
	atomic_set(&jam_disk->deferred_jreqs_cnt, 0);
	jam_disk->n_pending = 0;
	jam_disk->n_alloced = 0;
	jam_disk->n_free = 0;
	jam_disk->n_abandoned = 0;
	jam_disk->n_erasing = 0;
	atomic_set(&jam_disk->dying, 0);
	jam_disk->scan_non_free_jif = jiffies;

	for (i = 0; i < jidx_pool_size; i++) {
		jidx_pool[i].idx = i;
		jidx_pool[i].gen_id = ent_md[i].ent_gen_id;
		INIT_LIST_HEAD(&jidx_pool[i].pool_link);
		INIT_LIST_HEAD(&jidx_pool[i].uniqj2b_trans_link);
		jidx_pool[i].state_chg_jif = jiffies;

		/* init jidx's hkeys and add to hash if needed */
		jmdc_ent.md_arr = &jmdc_tbl[i * disk->jour.rng_binje];
		jidx_pool[i].hkeys.transient = jidx_hkey_invalid;
		if (nvmeib_jentry_md_is_unused_fast(&jmdc_ent, disk->jour.rng_binje)) {
			BUG_ON(!test_bit(i, free_ents_bitmap));
			jidx_pool[i].hkeys.committed = jidx_hkey_invalid;
		}
		else {
			if (nvmeib_jentry_md_is_valid(&jmdc_ent, disk->jour.rng_binje)) {
				__jmdc_to_hkey(&jmdc_ent, &jidx_pool[i].hkeys.committed);  // jmdc (and its j2d is 100% valid)
				hash_add(jam_disk->uniqj2b_htable, &jidx_pool[i].uniqj2b_hlink,
						jidx_pool[i].hkeys.committed.raw);
			} else {
				BUG_ON(test_bit(i, free_ents_bitmap));
				BUG_ON(!nvmeib_jentry_md_is_invalid_special(&jmdc_ent, disk->jour.rng_binje));
				_NW(nvmeibc_jam_disk_add_w1,
					"invalid-special jidx: @INT (jam @PTR)", i, jam_disk);
				jidx_pool[i].hkeys.committed = jidx_hkey_invalid_special;
			}
		}

		/* init jidx's state and add to free-pool if needed */
		if (test_bit(i, free_ents_bitmap)) {
			BUG_ON(ent_md[i].ent_gen_id < nvmeib_jrnl_ent_gen_id_min ||
				ent_md[i].ent_gen_id > nvmeib_jrnl_ent_gen_id_max);
			jidx_pool[i].state = NVMEIBC_JIDX_STS_FREE;
			list_add_tail(&jidx_pool[i].pool_link, &jam_disk->free_list);
			jam_disk->n_free++;
		}
		else {
			BUG_ON(jidx_pool[i].gen_id != nvmeib_jrnl_ent_gen_id_invalid);
			jidx_pool[i].state = NVMEIBC_JIDX_STS_ABANDONED;
			jidx_pool[i].abnd_jif = jiffies;
			jam_disk->n_abandoned++;
		}

		_NTj(trace_3_jam_nvmeibc_jam_disk_add, disk, "Init jidx @JRNL_RNG_ENT_IDX: state=@STATE_STR, "
				  "committed-hkey={TxID=@TXID, J2B=@J2B}",
			i, jidx_state_to_str(jidx_pool[i].state),
			jidx_pool[i].hkeys.committed.tx_id,
			jidx_pool[i].hkeys.committed.j2b);
	}

	/* sanity */
	for ( ; i < jidx_pool_size; i++) {
		if (test_bit(i, free_ents_bitmap)) {
			WARN_ON(1);
			_NE(trace_6_jam_nvmeibc_jam_disk_add,
				"Invalid free-bitmap=" NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE
				"out-of-range found in bit @INT", free_ents_bitmap, i);
			goto err;
		}
	}

	if (disk->binje_ulp != disk->jour.rng_binje) {
		if (jam_disk->n_abandoned == 0) {
			WARN_ON(1);
			_NE(trace_7_jam_nvmeibc_jam_disk_add,
				"Did not receive requested binje (req/rsp:@BINJE/@BINJE) although "
				"there are no abandoned entries",
				disk->binje_ulp, disk->jour.rng_binje);
			goto err;
		}
		/* only handle abnd2feee and when abnd reaches 0, redicover disk */
		jam_disk->alloc_enb = false;
	}
	else {
		jam_disk->alloc_enb = true;
	}

	jam_disk->pool_init.n_free = jam_disk->n_free;
	jam_disk->pool_init.n_abandoned = jam_disk->n_abandoned;

	if (jam_disk_proc_create(jam_disk, disk) < 0) {
		goto err;
	}

	//add
	jam_lock(c_jam, flags);
	if (disk->jam_disk == NULL) {
		disk->jam_disk = jam_disk;
		jam_disk->disk = disk;

		//list-add-sorted
		_NT(t0_jam_disk_add, "add disk @PTR (n=@INT)", disk, c_jam->n_disks);
		INIT_LIST_HEAD(&disk->jam_disk->jam_link);
		list_for_each_entry(jdisk, &c_jam->disks, jam_link) {
			if (jdisk->disk > disk) {
				list_add_tail(&disk->jam_disk->jam_link, &jdisk->jam_link);
				break;
			}
		}
		//first&last cases
		if (list_empty(&disk->jam_disk->jam_link)) {
			list_add_tail(&disk->jam_disk->jam_link, &c_jam->disks);
		}

		c_jam->n_disks++;
		rv = 0;
		_NTj(trace_4_jam_nvmeibc_jam_disk_add, disk, "added to jam (n_free=@N_FREE, n_abnd=@N_ABND, jwq=@JWQ), n_disks=@N_DISKS",
			jam_disk->n_free, jam_disk->n_abandoned, wq_pid(jam_disk->jwq),
			c_jam->n_disks);
	}
	else {
		_NTj(trace_5_jam_nvmeibc_jam_disk_add, disk, "already in jam");
	}
	jam_unlock(c_jam, flags);

	if (!rv)
		goto out;

err:
	if (pcpu)
		nvmeib_public_free_percpu(pcpu);
	jam_disk_proc_destroy(jam_disk);
	if (jwq) /* no drain */
		wq_destroy(jwq);
	kfree(jam_disk);
	kfree(jidx_pool);

out:
	NFOUT;
	return rv;
}

/* When this function is called (we expext that):
 *
 * 1. Upper layer (UL) had already acked disk-pause i.e.
 *    No new calls for this disk/jam-disk can pass pausable layer.
 *
 * 2. Not all jidx have been returned by UL.
 *    UL may try to return jidx after call disk-pasue returned to Transport
 *    layer and thus will be rejected by pausable layer --> Not reaching JAM.
 *
 * 3. Admin channel and All nr-channels were disconnected.
 *    No more ib-comps (e.g. jmd-rst (nrch), abnd2free-from-serjio (admin))
 *    that may try to process jidx (put-back or reuse).
 *
 * 4. This jam-disk may be part-of pending-reqs waiting for free jidx of this
 *    disk or other disks.
 *
 *
 * Description:
 * 1. Dequeue every pending-req that @disk is part-of (into a local list).
 *    o If such req is waiting for other disk's free-jidx and it is dequeued
 *      before we manage to do so, it must first get PD approval for all disks
 *      and thus will have to rollback as PD of this @disk will reject it.
 *
 *    o No new jam-alloc requests that include @disk can pass pausable layer,
 *      let alone, generate new pending-req, that involve @disk, in other disks.
 *
 *  2. Cancel all dequeued pending-req that @disk was part-of
 * 	   o Delete pending timeout-timer
 *     o Rollback allocation of disks that are not pausing - may
 *       trigger resume of other pending-req @disk is not part of.
 *     o Callback block completion (so IO can dec topology refcnt)
 *     o Free pending-req memory.
 *
 *  3. Drain WQ from resume-work(s) added by:
 *     o Expired timer (done under jam-disk lock).
 *     	  Any pending-req that its timer expired && managed to dequeue jreq
 *	      before we did, had also added a resume-work to abort jreq (under lock)
 *	      (BEFORE we started dequeuing).
 *     o Rollbacks occured before disk was paused.
 */
void nvmeibc_jam_disk_del(struct nvmeibc_disk *disk)
{
	struct nvmeibc_jam *c_jam = cdisk2cj(disk);
	struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
	struct nvmeibc_jam_disk *jdisk;
	struct nvmeibc_jam_pending_req *jreq;
	struct nvmeibc_jam_disk_percpu_cnts sum = {0};
	LIST_HEAD(cancel_list);
	int i, n_cancel = 0;
	ulong flags;
	NFIN;

	if (!jam_disk) {
		_NWj(warn_jam_nvmeibc_jam_disk_del, disk, "not in jam");
		goto out;
	}
	jam_disk_proc_destroy(jam_disk);

	jam_lock(c_jam, flags);

#if DEBUG_JAM
	atomic_inc(&jam_disk->dying);
#endif

	/* Dequeue every pending-req that @disk is part-of (into a local list) */
	list_for_each_entry(jdisk, &c_jam->disks, jam_link) {
		LIST_HEAD(del_list);
		struct rb_node *node;
		jam_disk_spin_lock(jdisk);
		for (node = rb_first(&jdisk->pending_root); node; node = rb_next(node)) {
			jreq = container_of(node, struct nvmeibc_jam_pending_req, pending_node);
			for (i = 0; i < jreq->n_disks; i++) {
				if (jreq->sorted[i].disk == disk) {
					list_add_tail(&jreq->tmp_link, &del_list);
					break;
				}
			}
		}
		list_for_each_entry(jreq, &del_list, tmp_link) {
			unlink_jreq_jidx(jdisk, jreq);
			pending_req_list_del_(jreq, jdisk);
			n_cancel++;
		}
		jam_disk_spin_unlock(jdisk);
		list_splice(&del_list, &cancel_list);
	}
	_NT(trace_jam_nvmeibc_jam_disk_del,
		"Found @N_CANCEL pending-reqs disk @DISK_NAME is part of "
		"(n_alloced=@N_ALLOCED)",
		n_cancel, disk->name, jam_disk->n_alloced);

	jam_cnts_on_jent_to_canceled(c_jam, alloced, jam_disk->n_alloced);
	jam_cnts_on_jent_to_canceled(c_jam, pending, n_cancel);

	/* reomve @disk from jam */
	c_jam->n_disks--;
	list_del(&disk->jam_disk->jam_link); /* c_jam.disks list */
	jam_unlock(c_jam, flags);

	/* Cancel all dequeued pending-req that @disk was part-of.
	   (rollback, callback and free) */
	while (!list_empty(&cancel_list)) {
		jreq = list_first_entry(&cancel_list,
		struct nvmeibc_jam_pending_req, tmp_link);
		list_del_init(&jreq->tmp_link);
		del_timer_sync(&jreq->timeout_timer);
		pending_req_cancel(jreq);
	}

	_NT(trace_jam_nvmeibc_jam_disk_del_wait_deferred_jreqs, "Waiting for deferred_jreqs_cnt to be 0 count: @INT", atomic_read(&jam_disk->deferred_jreqs_cnt));
	wait_event(jam_disk->deferred_jreqs, atomic_read(&jam_disk->deferred_jreqs_cnt) == 0);
	/* Wait for all resume-works (to ...
	   rollback allocs of other disks, cb and free jreq) */
	wq_drain(jam_disk->jwq);

	/* Now that we have dec n_alloced for any pending-req lets see (for debug)
	   without lock, how many reqs was the block unable or forgot to return */
	if (jam_disk->n_alloced)
		_NTj(trace_1_jam_nvmeibc_jam_disk_del, disk, "@DISK_NAME @INT idxs not returned from block (or leaked)",
		   disk->name, jam_disk->n_alloced);

	jam_disk_cnts_sum(jam_disk, &sum);
	_NTj(trace_2_jam_nvmeibc_jam_disk_del, disk,
		 "del from jam, "
		 "Now={n_free=@N_FREE, n_alloc=@N_ALLOC, n_abnd=@N_ABND, n_erasing=@N_ERASING, n_cancel=@N_CANCEL}, "
		 "Total={alloc_ok=@LLU, {alloced=@LLU, rollback=@LLU, diff=@LLD}, free=@LLU, erase=@LLU, erase_comp=@LLU, abnd=@LLU, a2f=@LLU}",
		 jam_disk->n_free, jam_disk->n_alloced, jam_disk->n_abandoned, jam_disk->n_erasing, n_cancel,
		 sum.tot_alloc_ok, jam_disk->tot_alloc, jam_disk->tot_rollback, jam_disk->tot_alloc - jam_disk->tot_rollback, jam_disk->tot_free,
		 jam_disk->tot_erase, jam_disk->tot_erase_comp, jam_disk->tot_abnd, jam_disk->tot_a2f);

	jam_disk_cache_fill(disk);

	nvmeib_public_free_percpu(jam_disk->pcpu_cnts);
	kfree(jam_disk->jidx_pool);
	wq_destroy(jam_disk->jwq);
	kfree(jam_disk);
	disk->jam_disk = NULL;
	_NDj(trace_3_jam_nvmeibc_jam_disk_del, disk, "removed from jam (n_disks=@N_DISKS)", c_jam->n_disks);
out:
	NFOUT;
}

size_t nvmeibc_jam_fill_disk_status(struct nvmeibc_disk *disk, char *buf, size_t len)
{
	struct nvmeibc_jam_disk *jam_disk = disk->jam_disk;
	unsigned long flags;
	int cnt = 0;

	if (jam_disk) {
		jam_disk_spin_lock_irqsave(jam_disk, &flags);
		BUF_ADD("\t- Journal Range: %u (LBA: %llu - %llu) GenID (%llx) SERJIO Boot ID: %s\n",
				jam_disk->disk->jour.rng_id, jam_disk->disk->jour.rng_slba,
				jam_disk->disk->jour.rng_slba + jam_disk->disk->jour.rng_nlba - 1,
				jam_disk->disk->jour.rng_gen_id, disk->jour.serjio_boot_id);
		BUF_ADD("\t- Total Entries: %d, Abandoned: %d, Free: %d\n",
				jam_disk->jidx_pool_size, jam_disk->n_abandoned, jam_disk->n_free);
		BUF_ADD("\t- Entries Allocated Avg: %llu\n", jam_disk->n_alloced_avg_cnt ?
			jam_disk->n_alloced_avg_sum / jam_disk->n_alloced_avg_cnt : 0);
		BUF_ADD("\t- Time Entries Allocated Avg: %llu jiffies\n", jam_disk->jif_alloced_cnt ?
			jam_disk->jif_alloced_sum / jam_disk->jif_alloced_cnt : 0);
		jam_disk_spin_unlock_irqrestore(jam_disk, flags);
	}

	return (size_t)cnt;
}

void* nvmeibc_jam_init(const struct nvmeibc_cinst_params_core *p)
{
	struct nvmeibc_jam *c_jam;
	NFIN;
	if ((c_jam = kzalloc(sizeof(*c_jam), GFP_KERNEL))) {
		if ((c_jam->pcpu_cnts = nvmeib_public_alloc_percpu_cacheline(
			struct nvmeibc_jam_percpu_cnts))) {
			spin_lock_init(&c_jam->lock);
			INIT_LIST_HEAD(&c_jam->disks);
			c_jam->n_disks = 0;
		}
		else {
			kfree(c_jam);
			c_jam = NULL;
		}
	}
	(void)p;							// Currently JAM does not need any parameter
	NFOUT;
	return c_jam;
}

void nvmeibc_jam_exit(const struct nvmeibc_cinst_params_core *p)
{
	struct nvmeibc_jam *c_jam = __get_c_jam(p);

	NFIN;
	if (c_jam) {
		if (!list_empty(&c_jam->disks))
			_NE(error_jam_nvmeibc_jam_exit, "OOPS, jam disks list not empty, n_disks=@INT", c_jam->n_disks);
		if (c_jam->pcpu_cnts)
			nvmeib_public_free_percpu(c_jam->pcpu_cnts);
		kfree(c_jam);
	}
	NFOUT;
}

u64 nvmeibc_jam_decode_lba(u64 enc_lba)
{
#if JAM_ENCODE_JOURNAL_LBA
	struct lba_enc *l = (void *)&enc_lba;
	return l->lba;
#else
	return enc_lba;
#endif
}
