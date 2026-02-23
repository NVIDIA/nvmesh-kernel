/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_COMMON_H
#define NVMEIBC_DP_COMMON_H
/*
 * Managment of locks for protected IO. Acquiring locks, releasing them after
 * IO, dealing with contended locks, retrying, abandoning locks, etc
 */
#include "kr_incs.h"
#include "nvmeibc_block.h"		/* external API of the block */
#include "nvmeibc_block_dp_cmd_lock_link.h"
#include "nvmeibc_block_dp_operation.h"
#include "nvmeibc_block_dp_io_perm_alert.h"
#include "profiling/nvmeibc_block_dp_profiling_generic.h"
#include "nvmeibc_block_dp_cpu_masks.h"
#include "nvmeib_pcpu_wq.h"

/****************************** IO statistics Mechanism ***********************
  There are 4 structs for IO statistics:
  1. struct nvmeibc_stats - set of counters and timers
  2. struct nvmeib_io_stats - Uses nvmeibc_stats to measure each IO and stores
    	cumulative statistics. Those statistics are written to /proc for each
    	volume, and for each disk. /proc entries are read by
    	management agent and displayed in a beautiful gui.
  3. Kernel internal IO statistics, are gathered independently of our code
  4. struct t_failed_io_stats - Each volume gathers cumulative statistics of
		failed IO's and writes them occasionally to dmesg. Used for debugging

  There are 5 IO statistic mechanisms:
  1. In block.c : block_stats - for debug only, enable by #ifdef TAKE_STATS.
    	A set of counters to debug specific parts in data path. Default=Disabled
  2. Each block device has in its OS API a field 'stats' which gethers stats and
    	writes them to /proc/nvmeibc/volumes/vol_name/io_stats. This field is
    	active for regular volumes, and carriers. The statstics
		are gathered only for successful IO's (ignorring: timed-out, failed)
  3. Each IO operation has a field 'dev_stat'. A set of counters used for
    	updating mechanism 2 (blockdev->os_api.stats).
  4. nvmeibc_disk->stats : Same as 2 but gathers IO stats per physical disk.
	    appears in /proc/nvmeibc/disks/disk_name
  5. Each command in each IO operation has a field 'io_stat'. A set of counters
    	used for updating mechanism 4, like 3 is used for 2. This field must be
		per command and not per operation. */

/************************* Configuration related methods **********************/
/* All functions named check or validate return 0 on success or negative error*/
int __check_layout(u64 nlbas, u64 start_lba, const struct nvmeibc_topology *t,
				   bool allow_thin_writes);

/* Address translation steps: We have 3 steps
   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   Given - contiguous range of blocks in vlba (start, length). Length can be 1.
   Step 1: vlba {u64 start, u64 len} --> multiple clba {u64 start, u64 len}
     Convert vlba into chunk lba. If range is large it can span accross many
     chunks. This conversion is trivial - just offsets in vlba address space
     (trivial chunk iterator).
   Step 2: clba {u64 start, u64 len} -> multiple rlba {raid, u64 start, u64 end}
	 rlba range is denoted by struct dp_io_topo_iterator_res. rlba is a range
     inside a protection raid (regardless of datapath type). If chunk range is
     long and it is raid+0 then a multiple rlbas will be generated (bounded by
     stripe width). This translation step deals with striping and includes
     stripe iterator. Note: range in protection raid is address of offset from
	 beginning of a protection raid and length.
   Step 3: rlba --> multiple dlba {physical disk, u64 start, u64 length}
     dlba or physical lba is a location on the physical disk and a single rlba
     can be converted to 2 dlbas for raid1 and much more for R5. This step has
     2 variants:
    3.1 Convert rlba of a LOCK ADDRESS to dlba: Can be done without exact block
       address calculation (assuming locks are aligned to 128K). This
       translation can be done regardless of raid type. This translation is used
       when allocating locks for IO.
    3.2 Convert rlba of a BLOCK ADDRESS to dlba: This cannot be implemented
       generically, but rather a virtual function implemented by each datapath.
       This translation is used when allocating commands for IO (during prepare
       operation). This translation deals with slicing (uses slice iterator) and
	   offset of each raid segment on physical disk.
       Ror mirroring (slice size 1) the iterator is so trivial that it is
	   ommited.

    Inverse translation has also those 3 steps. This translation always yields
    1 result (unlike 1-to-many previous translation)
    dlba->rlba->clba->vlba.
    Summary:
       vlba range {vol   ptr, u64 start, u64 len}
       clba range {chunk ptr, u64 start, u64 len}
       rlba range {raid  ptr, u64 start, u64 len}
	   dlba range {disk  ptr, u64 start, u64 len}
*/

struct nvmeibc_raid1;
/* Iterator that splits contiguous IO address range into chunks/raids/locks,
   and iterates from start to end. Used in different data pathes to traverse
   locks and commands. Iterator does the above addr translation steps 1 and 2 */
struct dp_io_topo_iterator {			// Contigous IO
	const struct nvmeibc_topology *t;	// Topology of IO
	u64 vlba;							// Current address[blocks]
	u64 nlbas;							// Total remaining blocks of IO[blocks]
	u64 nlbas_chunk;					// Remaining blocks in current chunk
	int ci;								// Current chunk index in topology.
	struct dp_io_topo_iterator_res {	// Result variable (rlba):
		const struct nvmeibc_raid1 *r;	// 	 Protection raid
		u64 rlba;						// 	 Offset from raids beginnng
		u64 nlbas; 						// 	 Num of blocks in raid
	} res;
};

/* Initialize the iterator with vlba range on topology t->chunks[ci]*/
void dp_io_topo_iterator_init(struct dp_io_topo_iterator *it,
				u64 start_lba, u64 nlbas, const struct nvmeibc_topology *t, int ci);

/* Updates it->res to store the next iteration. User can consume it as is or
   decrease 'nlbas_raid' if it consumes only prefix of the iteration
   due to restrictions (like: small disk DMA size, short scatter gather lists).
   Next call to this function will take into account the decreased values.
   'what' - 'l' = iterate to next locks (jump by blocksets)
          - 'r' = iterate to raid       (jump by raids)
   Returns: true if next batch (iteration exists) false if done.
   Performs address conversion of step 1. */
bool dp_io_topo_iterator_next(struct dp_io_topo_iterator *it, const char what);

/* Convert rlba in raid to physical lock address. step 3.1 address translation.*/
u64 dp_io_topo_iterator_conv_rlba_to_phys_lock_addr(const struct nvmeibc_raid1* r1, int si, u64 rlba);

/* Inverse of the above 3.1 step: translate, physical "lock address" to raid
   offset and length. It has 2 api's choose whichever suits you (give the lock
   or give l->ds and locks offset on segment explicitly */
void dp_io_topo_iterator_conv_phys_lock_addr_to_raid_ofst(
	struct dp_io_topo_iterator_res *res, const struct nvmeibc_cmd_lock *l);
void dp_io_topo_iterator_conv_seg_lock_addr_to_raid_ofst(
	struct dp_io_topo_iterator_res *res, const struct nvmeibc_disk_segment *ds,
	const u64 phys_offset_in_seg);

/* Generic dlba->vlba (inverse of all 3 steps of translation) for as single
   block. Uses virtual func of datapath */
struct nvmeibc_datapath;
u64 nvmeibc_datapath_dlba_to_vlba(const struct nvmeibc_datapath *dp, const struct nvmeibc_raid1* r1, const int si, const u64 dlba);
u64 nvmeibc_datapath_dlba_to_rlba(const struct nvmeibc_datapath *dp, const struct nvmeibc_raid1* r1, const int si, const u64 dlba);
/* Get segment index from disk segment */
raid_sgmnt_t nvmeibc_dp_get_sgmnt_idx_from_ds(const struct nvmeibc_disk_segment *ds);
/****************************** Mallocs **************************************/
static inline void* my_kmalloc(size_t size, gfp_t flags){
    void *k = kmalloc(size, flags);
    return k;
}

static inline void* my_kzalloc(size_t size, gfp_t flags){
    void *k = kzalloc(size, flags);
    return k;
}

static inline void* my_vzalloc(size_t size){
	void *v = vzalloc(size);
	return v;
}

extern bool nvmeibc_dp_alloc_allow_io;

static inline gfp_t nvmeibc_dp_get_allow_io_gfp_flags(void)
{
	return (nvmeibc_dp_alloc_allow_io ? GFP_NOFS : GFP_NOIO);
}

/* Try kzalloc, if fails vzalloc. The only permitted flags are as in vzalloc.
 * Do not use KMALLOC_MAX_SIZE becasue it is large ~32[mb]. This method is used
 * to allocate rdma locks and disk commands */
static inline void* my_kvzalloc(size_t size, gfp_t flags){
    #define __KV_LEGAL_FLAGS (GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO)
	const size_t MAX_KM_SIZE = (PAGE_SIZE << 3);	// 32[KB]
	void *kv = NULL;
	WARN(flags & (~(__KV_LEGAL_FLAGS)), "nvmeibc bug! f=%u KV=%u\n", flags, __KV_LEGAL_FLAGS);
	if (size <= MAX_KM_SIZE && (kv = my_kzalloc(size, flags))) { // Kmem
	} else {
		kv = my_vzalloc(max(size, sizeof(struct work_struct)));// Vmem
	}
	return kv;
    #undef __KV_LEGAL_FLAGS
}

static inline void my_kfree(const void *p){
	kfree(p);
}

static inline void my_vfree_work(struct work_struct *work) {
	vfree(work);
}

/* vfree can deadlock if called from process contect w/ irq-disabled, defer */
static inline void my_vfree_deferred(void *addr) {
	struct work_struct *work = addr;
	INIT_WORK(work, my_vfree_work);
	BLKCMP_ANY_schedule_work(work);
}

static inline void my_kvfree(void *addr){
	// kvfree(p); YR: kvfree does not exist in older kernels clean up for new kernels.
	if (is_vmalloc_addr(addr))
		my_vfree_deferred(addr);
	else
		kfree(addr);
}

/************************** Operation rescheduling ****************************/
inline static bool dp_block_schedule_work(int cpu_id, struct work_struct *work)
{
	//it looks like in some cases/platforms the schedule_work_on is defined in a such way that cpu_id is not in use.
	//sonar is complaining.
	(void)cpu_id; 
	return schedule_work_on(cpu_id, work);
}

inline static bool dp_block_schedule_operation_work(const struct operation *o, struct work_struct *work)
{
	BUILD_BUG_ON(NR_CPUS != WORK_CPU_UNBOUND);	// Linux defines WORK_CPU_UNBOUND as NR_CPUS; non-IO operations define their cpu_id as NR_CPUS as a "don't care" value; work queues expect WORK_CPU_UNBOUND as a "don't care" value; make sure these are indeed the same
	return dp_block_schedule_work(!NVMEIB_CPU_MASK_INFO_IS_EMPTY(o->cpu_mask_info) ? o->cpu_id : WORK_CPU_UNBOUND, work);
}

/************************** Datapath Virtual functions ************************/
#include "nvmeibc_block_dp_io_resubmitter.h"

#define LARGE_DEBUG_VALUE (0x1000000)                  // We have many counters that go up and down (like amount of uncompleted task. We add this big value to the counter and remove at the end to debug, whether the counters don't go negative

/* Describes translation of a  vlba block according to datapth, used for
   debug and translation ioctls */
struct dp_block_translation_unit {
	struct t_dp_block_trans_input {			// Input to translation mechanism
		struct nvmeibc_block_device *nd;	// Which volume
		u64 vlba;							// Range of vlbas
		u32 nlbas;
        bool translate_by_cfg;              // If true, the addresses will be translated by configuration, instead by topology
		enum nvmeib_block_io_op op;			// Which operation. Default is write
		bool translate_locks;				// Add locks translation as well
	} input;
	struct t_dp_block_trans_output { 		// Output of translation
		const struct nvmeibc_idisk* disks[32]; // Array of disks (need raid->n_segs*2)
		u64                         offs[32]; // Phys offsets
		const char                *descr[32]; // Type of the command
		u16 io_perm;						// enum nvmeib_io_type_permission, of current topology (May prevent translation if 'op' is illegal)
		u16 ci, ri;			// Chunk index and raid index of protection raid
		u16 n_cmds;			// Num elements in the arrays above
		// ---------------------------
		char 					*mssa_output; // Not mandatory, will be allocated and filled if required
		// ---------------------------
		const struct nvmeibc_idisk* ldisks[N_MAX_RAID_LOCKS]; // Array of disks of locks
		const char                *ldescr[N_MAX_RAID_LOCKS]; // Type of the lock
		u16 n_locks;										// Num elements in the arrays above
		struct {
			struct nvmeibc_block_device *nds[2];
			u64                         lbas[2];		// can be ~0ULL if data is not at that device
			const char                *descr[2];
			u16 n_nds;
		} d_carriers;
		struct {
			struct nvmeibc_block_device *nd;		// NULL if no MD carrier
			u64                         lba;
		} md_carrier;
	} output;
};
/* Generic, unrelated to a specific datapath */
void dp_block_translation_unit_calc_locks(struct dp_block_translation_unit *tu, struct dp_io_topo_iterator *it);

#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"

/*********************** Recoveries draining component ************************/
// Daniel: Consider moving to dedicated file
struct nvmeibc_recoveries_drainer {
	spinlock_t lock;
	uint /*struct kref*/ num_running;		// Number of running recoveries on all praids of volume, including weak
	uint num_running_weak;					// Weak recovery reference is not counted towards the total recoveries number and does not prevent self hidden detach
	uint num_finished;						// Amount of finished recoveries
	ulong last_finished_jiff;				// When last recovery finished
	void (*__on_last_recovery_finish_cb)(void *_ctx);
	void *_ctx;
};

void  nvmeibc_recovs_drainer_init(    struct nvmeibc_recoveries_drainer *d, void (*release_fn)(void *), void *_ctx);
void  nvmeibc_recovs_drainer_reinit(  struct nvmeibc_recoveries_drainer *d);
void  nvmeibc_recovs_drainer_inc(     struct nvmeibc_recoveries_drainer *d);
void  nvmeibc_recovs_drainer_inc_weak(struct nvmeibc_recoveries_drainer *d);
int   nvmeibc_recovs_drainer_dec(     struct nvmeibc_recoveries_drainer *d);
int   nvmeibc_recovs_drainer_dec_weak(struct nvmeibc_recoveries_drainer *d);
int   nvmeibc_recovs_drainer_get_num( struct nvmeibc_recoveries_drainer *d, ulong *get_time, uint *num_finished, uint *num_running_weak);

/***************************** Data-path object *******************************/
enum nvmeibc_data_path_type {
	NVMEIBC_DATA_PATH_ILLEGAL		= 0x0,			// Illegal/Unknown value, IO will be auto disabled
	NVMEIBC_DATA_PATH_JBODS			= 0x8,			// JBOD/R0 (Without RDMA locks)
	NVMEIBC_DATA_PATH_MIR_BIO		= 0x2,			// R1/R10, Or Locked-JBOD/R0 (with RDMA locks)
	NVMEIBC_DATA_PATH_MIR_MDBLK_BIO	= 0x7,			// Mirroring metadata carrier, Mostly R1 datapath
	NVMEIBC_DATA_PATH_MIR_D_CARRIER	= 0x9,			// Same as above but supports being data carrier (WCV)
	NVMEIBC_DATA_PATH_EC_R6			= 0x4,			// D+P Erasure coding. Supports R5/R6/R50/R60
};

#include "block/recovery/nvmeibc_block_dp_sync_api_manager.h"
struct nvmeibc_datapath {
	struct nvmeibc_io_resubmitter resub;
	struct dp_io_stats   io_stats;	// Gathers cumulative statistics regarding failures and abnormal behaviour of IO's
	struct slow_io_stats_t     io_slow;		// Gathers cumulative statistics regarding successfull but slow IO's
	struct nvmeibc_io_perm_alert io_perm_alert; // Used to inform mgmt of ongoing IO problems (disabled, readonly, etc)
	struct nvmeibc_datapath_syncs_resources sync_rsrcs; //protected by resubmitter lock
	struct nvmeibc_recoveries_drainer running_recovs;	// Mechanism to drain running recoveries
	struct nvmeibc_b_dp_cpu_masks cpu_masks;	// Per-CPU CPU masks

	/****** DP IO virtual functions ******/
	int (*should_ignore_op)(const struct operation *o);	// >0: Yes with success, <0: Yes with failure, 0: No, just execute it
	int  (*prepare_op)( struct operation *o);// Allocate/Prepare locks, commands, etc
	int  (*execute_op)( struct operation *o);
	void (*cmd_comp_cb)(struct nvmeibc_d_iocmd_comp *comp, struct nvmeibc_d_iocmd_comp_tag tag);	// Callback for completion of specific command
	int  (*exec_func_on_locks_tkn)(struct nvmeibc_block_command *rldr, int rv);			// This function is called after locks are taken/broken and before first stage is launched (transition of locks state machine to cmd leader state machine). Returns Error code <0, or positive code, if execution should be aborted and callback will return
	void (*exec_func_on_stage_end)(struct nvmeibc_block_command *rldr, int *rv);	// This function is called for each stage the raid leader finishes
	void (*calc_should_abandon)(struct nvmeibc_block_command *cmds, int li);							// After all commands that need lock 'li' calculate if should abandon this lock or not
	void (*calc_comp_state)(const struct nvmeibc_block_command *cmds, int *o_rv, bool *retry_required);	// Calcualte the completion state of the operation from its commands
	void (*allow_locks_rel_debug)(struct nvmeibc_cmd_lock *locksets);									// When all commands finished we allow locks release, but not kfree(). Only after this method is called locks can get kfree and put their topo reference

	/****** DP utils *******/
	int (*dbg_trans_addr)(struct dp_block_translation_unit* tu);		// Debug function for translating vlba to dlba (up to 16 segments) and the raid where this address resides
	u64 (*dbg_trans_dlba_to_clba)(const struct nvmeibc_datapath *dp, const struct nvmeibc_raid1* r1, int si, u64 dlba); // convert the LBA used for the physical segment to the matching LBA within the chunk

	/****** DP Sync virtual functions ******/
	int  (*sync_prepare_op)(struct recovery_sync_op *so);		// Allocate the needed commands
	void (*sync_execute_op)(struct recovery_sync_op *so);		// Launch Syncs commands state machine (when locks are taken). On failure must complete the execution with error (deliberatly no return value)

	struct nvmeibc_dp_params {
		u32 slice_size;
		u32 protect_level;
		u32 snake_size;							// Used by EC volumes to allow inner blockset stripping, write snake_size blocks to a segment, then the next segment continues for snake_size and so forth
		u32 binje;								// Amount of journal blocks in journal entry. Maximal amount of slices of journalled write operation. Must divide snake_size
	} p;
	struct {									// If != 0 bio's will be split to parts. Units of 512[bytes]
		u32 write;								// Split Writes: Each 'write' bytes (like slice size) is executed in a single operation. // Todo: Remove the following commentSlight problem: in case the volume contains two or more EC RAIDS with different slices; the split should be done based on the real geometry and not by max/min slice size JAM/Serjio does not support multi-slice I/O
		u32 read;								// Split Reads:  Ensure signle raid leader per operation (always == slice_size * LOCKSET_SLICES)
	} alignment_sectors;
	struct {									// If 0: Elevator will not work. Units of blocks
		u32 max_elev_write;						// Elevator: unites writes. Naturally alignment_sectors.write divides this field
	} elevator;
	u32 sizeof_operation;

	struct no_writehole_functions {
		enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE (*restore_function)    (struct recovery_sync_op *);
		void                                 (*destroy_function)(struct recovery_sync_op *);
		void                                 (*sbs_cleanup)     (struct recovery_sync_op *);
		int                                  (*get_restore_rv)  (struct recovery_sync_op *);
	} nwhole_funcs;

	/* Datapath generic capabilities (traits): See design doc https://docs.google.com/document/d/1Lo_Vx_25Zm3lWvTr-5pFlnSPk9laE1kKoUWKCXpFlHQ */
	//struct datapath_capabilities {
		bool enable_di_debug_mode;						// Special (debug only mode) where datapath writes block internal info instead of source data. Used in data corruption debugging. Can be switched on/off in run-time. Supported by all datapathes
		bool enable_edic_check;							// Enable checking edic for each read (compare CRC of block data to edic in metadata)
		bool enable_local_read_optimization;			// Mirror only: In perfect topology, prefer reading from segments residing on disks which are local to the client.
		int read_has_mutable_bio_buffers;	// In case, read buffers are shared between multiple bios, we must allocate our own buffers
		bool enable_store_dirty_bits_in_peristent_md;	// Optimization: dbits are stored to metadata of {p,q} segments so cold recovery in degraded mode does not have to rewrite the entire 'W' seg.
		bool enable_multi_degraded_writes;				// Dbits can be turned on and turned off in a single transaction, requires barriers between remote rdma/io-to-disk commands
		bool turn_off_dbits_before_io;					// Optimization, simplification of good path. IO will not deal with dbits turn off, just call blockset fixup to do so
		bool enable_care_about_txid;					// Do we care about generation of blockset? Used for Journaling and for future snappshotting
		bool avoid_store_dbits_in_ram_of_W_segs;		// storing dbits = Pros: all copies of ram binfo become identical. Con: During dbits rebuild of seg W->RW need to fix blockset info of all blocksets, not only dirty blocksets
		// bool enable_partial_slice_write;				// Moved to be per praid. Whether this dapaath allows parital slices. False if D=1, but even for D>1 may not be false if external mechanism guarantees full slices
		// bool use_rdma_locks;							// Moved to be per praid. Locks must be taken if D+P > 1, or block-rmw(read-modify-write) supported. rmw is used for sub block IO or compare-exchange on block data.
	//} traits;

	struct nvmeibc_profiler_configuration good_path_io_profiling_defs;

	struct nvmeibc_profiler_configuration preparation_profiling_defs;
	struct nvmeibc_profiler_configuration sync_profiling_defs;
	struct nvmeibc_profiler_configuration lock_profiling_defs;
	union {	// Used to profile MTV destage (MTV has no disks or locks)
		struct nvmeibc_profiler_configuration disk_profiling_defs;
		struct nvmeibc_profiler_configuration destage_profiling_defs;
	};
};

void nvmeibc_datapath_init(struct nvmeibc_datapath *dp, const char* name, enum nvmeibc_data_path_type e, bool enable_edic_check, bool use_debug_di, bool enable_local_read_optimization, int read_has_mutable_bio_buffers, int slice_size_split, struct nvmeibc_dp_params par);
void nvmeibc_datapath_destroy(struct nvmeibc_datapath *dp);

/************************* Todo: Sort this out ********************************/
/* Each implementation of dp->cmd_comp_cb, must call this command, or if sending
   command failed it is called syncronously.
   Func does: Analyzes rv + frees locks + completes command */
void dp_cmds_analyze_rv_and_complete(struct nvmeibc_d_iocmd_comp *comp);

/* Used for debug di only, to mark EDIC check in EC */
enum edic_result {
	EDIC_UNINITIALIZED = 0,		// Must be 0, not filled
	EDIC_NOT_CHECKED = 'N',
	EDIC_PASS = 'P',
	EDIC_FAIL = 'F'
};

#endif  // H beginning
