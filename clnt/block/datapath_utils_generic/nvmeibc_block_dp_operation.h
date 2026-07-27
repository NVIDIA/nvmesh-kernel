#ifndef NVMEIBC_DP_OPERATION_H
#define NVMEIBC_DP_OPERATION_H
/* IO operations:
   A single IO operation issued by the OS or Recoveries. May translate to
   many IO commands, multiple locks acquisition.
   This volume operation supports alternative execution models with
   other operation acquisition methods (not only kernel bio).
  - Operation is tied to a specific topology.
  - Topologies can be released once no operations refer to them.
  - Operation (in contrast to a pure bio can be retried).
  - It is agnostic to datapath of volume (erasure coding, mirroring, etc)
    and uses virtual functions of volume's datapath to execute itself
 */
#include "nvmeib_stats.h"
#include "nvmeibc_block.h"		/* external API of the block */
#include "nvmeibc_block_dp_io_generic_cmds.h"
#include "nvmeibc_block_dp_buffers.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_async_mode.h"

typedef u32 o_dbg_id_t;

#ifndef DP_LIB
	#define MAX_BIOS_PER_OP 16					/* Elevator can merge up to 16 bio into 1 op */
#else
	#define MAX_BIOS_PER_OP 1					/* No bio merge to operation / no elevator */
#endif

struct operation {
	enum nvmeib_block_io_op op;					// Operation type.
	o_dbg_id_t dbg_id;							// Operation's debug identifier, used in tracing.
	union {
		struct {
			struct bio_part     *bios[MAX_BIOS_PER_OP];			// The bio request from kernel
			int num_bios;
		};
		struct recovery_sync_op *rso;			// Back from degraded mode recovery / Dirty-bits sync / Overcomming permanent read failure
		struct nvmeibc_recovery *vro;			// Volume rebuild master operation: Spawns multiple sync operations (1 for each problematic blockset)
		struct dplib_caller     *slo;			// External caller to sync operation implemented by sync library
	};
	int cpu_id;									// BIO operation: cpu on which the IO started. It may complete on another CPU but will pull the next IO form the list of starting CPU
												// Sync operation: same as above for the IO that issued the sync (if any, otherwise, as in recovery syncs, -1)
	struct nvmeib_cpu_mask_info cpu_mask_info;	// BIO operation: Configured CPU mask that contained the CPU on which the IO started (if any, otherwise a zero mask)
												// Sync operation: Copied from the operation of the IO that issued the sync (if any, otherwise, as in recovery syncs, a zero mask)
	struct nvmeibc_block_device *nd;			// Block device which executes the IO
	struct nvmeibc_topology *topo;				// Topology of bdev on which IO runs.
	unsigned long jiffies1;						// Time stamp when the operation was first issued by OS (regardless whether it was delayed or rescheduled to retry in future)
	union {										// Throtelling & retries of operation, Todo: Wrap in struct
		struct list_head list_paused;			// Field to insert the operation into resubmit queue
		struct list_head per_cpu_wait_list;		// Fields for conencting IO to with list of IO's per cpu to prevent too much IO's beeing in air at once
		struct workqe_struct work_elev;			// Async launch operation after elevator processing
		struct workqe_struct work_skip_recov;	// async skip operation on blockset during recovery
		struct workqe_struct work_throttled;		// Next pulled IO will be executed using this work on a workqueue per cpu
		struct workqe_struct work_resubmitted;	// IOs from resubmitter paused list are executed on a workqueue per cpu
		struct workqe_struct work_copy_to_bio;	// copy data to mutable bioi buffers from a work queue and release the operation
		struct workqe_struct work_rso_execute;	// (Sync operations) execute rso on a workqueue
		struct workqe_struct work_rso_resched;	// (Sync operations) reschedule rso on a workqueue (locks SM only, invoke __handle_locks_o)
		BLKCMP_SO_DEFINE_BLOCKING_CONTEXT;		// Datapath library, sleeping context with fiber stack
	};
	//struct {									// Statistics about this operation
		struct {
			struct nvmeib_stop_watch total;		// Total time [1st execute..last completion] (see struct nvmeib_io_counters :: total_latency)
			struct nvmeib_stop_watch exec;		// Time of all execution attempts until last completion (see struct nvmeib_io_counters :: total_io_exec)
												// dropping the time it waited in wait-list but includes all retries.
		} time;
	//} stats;
	u32 io_stat_length;                      	// IO length[blks], supports up to 16[gb] single IO size
	int    CLmat_elems;							// For debug: Amount of bits in the array above. Todo: remove in final product
	ulong *CLmat;								// Bitfield matrix of size C (cmds) X L (locks) Where A[c,l] is true iff command c is linked to lock l
	struct multi_snake_slice_analyzer *mssa;	// Snake IO will fill this matrix as preparation for datapath execution plan -> replaces ioa and cmap
	struct nvmeibc_block_command *cmds;			// IO commands of the operation, can outlive the operation
	struct nvmeibc_cmd_lock      *locks;		// Locks of the operation, can outlive the operation
	struct nvmeibc_pages pages;					// Extra blocks (non-BIO) for commands' SGLs
	nvmeibc_atomic_t n_uncomp_raids;			// IO is split to praids. Only when all complete IO can be executed
	struct {
		u32 reserved                   : 8;
		u32 is_qlc                     : 8;		// Todo: give 1 bit. Is the block device is EC QLC
		u32 is_allocated_with_bio_part : 1;		// Is operation allocated together with bio_part
		u32 is_clmat_embedded 		   : 1;		// were locks+cmds+ndbs... embedded into operation?
		u32 resubmit_operation_no_free : 1;	// Resubmit embeded JBOD/Raid-0 operaions
		u32 was_bio_part_split         : 1;		// Was bio split to a few operations?
		u32 need_to_copy_bio           : 1;		// If bio changes during read/write operation then we have to copy it for edic/parity calculations to be stable
	} flags;
	union {
		struct {
			u32 n_resubmissions:		8;	// How many times operation was resubmitted
			u32 n_jam_alloc_failed:		4;	// How many times a JAM alloc failed (non-DEADLK failure) at least once?
			u32 n_dcmd_failed:			4;	// How many times a disk command failed (non-DNR (transient) failure) at least once?
			u32 n_lcmd_failed:			4;	// How many times a lock acquire command failed (disk/transport failure) at least once?
			u32 n_write_binfo_failed:	4;	// How many times a write binfo command failed at least once?
			u32 n_topo_phased_out:		4;	// How many times encountered a topology phased out between stages?
		}  __attribute__((packed));
		u64 raw;
	} dbg_cntrs;
	union {
		struct {								// Extended rider->carrier bio
			struct bio_extention *bx;
			void **orig_sgls;					// QLC partial destage can manipulate the sgl and must be freed from original pointer
		} md_op;								// Confusing name, Fix for 'elect op'
		void* user_data; 						// Auxiliary payload (used in recovery->sync operation)
		bool commit_only_owner_binfo;           // Tells commit binfo if it should only take the binfo from owner or merge all binfos.
	};
	struct operation *chained_op; 				// When bio is split to a few operations in the blockset, all operations are chained and transfer locks from first to last. Effectively serializing their execution, and avoiding timer retries on futile acquisition of locks. Pointer is relevant before operation actually starts executing
#ifdef DEBUG_TOPO_CNTRS
	struct debug_topo dbg_topo;
#endif
#ifdef DEBUG_NON_DIRECT_IO
	struct {
		u64 bio0[16];							// Store first 128[b] of bio - debug if bio is changes during execution of oepration
		u64 bio1[16];							// Second copy. In case of a change, helps to decided who changed (copy or bio)
		const u64 *buf;
	} cp;
#endif
};

#define OPERATION_DBG_CNTR_INC(_o, _cntr) \
	do { \
		(_o)->dbg_cntrs._cntr++; \
		if (unlikely(!(_o)->dbg_cntrs._cntr)) { \
			/* Undo wraparound */ \
			(_o)->dbg_cntrs._cntr--; \
		} \
	} while (0)


#define for_each_op_in_chain(iter, start) for ((iter)=(start);(iter);(iter)=(iter)->chained_op)

#define _NTO(n, o, format, ...) _NT(n, "{@O_DBG_ID}(@BLOCK_IO_OP,@VOL_ID:@TOPO_DBG_ID,@VLBA[@NLBAS]): " format, o->dbg_id, o->op, nvmeibc_volume_short_id(o->nd), o->topo->debug_unique_index, get_op_start_lba(o), (u64)o->io_stat_length, ##__VA_ARGS__)
void nvmeibc_operation_compressed_op_dump_bio(const struct operation *o);

/* Get/Put reference count on operation to prevent it from being free */
void nvmeibc_operation_get(struct operation *o, int n_refs);
void nvmeibc_operation_put(struct operation *o, int n_refs);

#define nvmeibc_operation_has_bio_extention(o) ((o)->md_op.bx != NULL)
bool nvmeibc_operation_is_valid_carrier_op(struct operation *o);

#define nvmeibc_operation_is_exclusive(o) (nvmeibc_block_get_res_vat((o)->nd)->res.mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX)

static inline bool nvmeibc_operation_is_bio(const struct operation *o){
	return nvmeib_block_io_op_is_bio_op(o->op);
}

static inline bool nvmeibc_operation_is_rso(const struct operation *o){
	return !!(o->op & (NVMEIB_BLOCK_IO_OP_RECOVER_STALE|NVMEIB_BLOCK_IO_OP_REC_SPARE|NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO));
}

static inline bool nvmeibc_operation_is_vro(const struct operation *o){
	return NVMEIB_BLOCK_IO_OP_RECOV_PROBLEM == o->op;
}

static inline bool nvmeibc_operation_is_slo(const struct operation *o){
	return NVMEIB_BLOCK_IO_OP_DPLIB_PROBLEM == o->op;
}

static inline bool nvmeibc_operation_is_bio_copy_needed_for_read(const struct operation* o){
	return o->flags.need_to_copy_bio && o->op == NVMEIB_BLOCK_IO_OP_READ;
}

static inline bool nvmeibc_operation_is_bio_copy_needed_for_write(const struct operation* o){
	return o->flags.need_to_copy_bio && o->op == NVMEIB_BLOCK_IO_OP_WRITE;
}

#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_part.h"
static inline u64 get_op_start_lba(const struct operation *o)
{
	BUG_ON(!o->num_bios);
	return get_start_lba(o->bios[0]);
}

static inline u64 get_op_nlbas(const struct operation *o)
{
	u64 nlbas = 0;
	int i;
	for (i = 0; i < o->num_bios; ++i)
		nlbas += get_nlbas(o->bios[i]);
	return nlbas;
}

static inline u64 get_op_length(const struct operation *o)
{
	u64 length = 0;
	int i;
	for (i = 0; i < o->num_bios; ++i)
		length += get_length(o->bios[i]);
	return length;
}

union vv_bio_inter {			// Elevated bio iterator. Iterates on array of bios. Todo: Rename this struct, it is too ugly.
	struct {						// Regular BIO
		struct nps_block_iter nbi;	// Optional: In case we have to copy BIO pages: Iterator of spare blocks
		bio_iter_t bi;				// Orig bio iterator
		int nb;						// Exists due to elevater which unifies bios, index of current bio
		int index_in_stage;			// BIO: Index of cmd in stage
		// if bio page, we read into, is shared between different bio, then we cannot use it to restore "missing" data in degraded topology 
		// thus we need to be an exclusive page owners
		int on_op_read_use_private_blocks;
		struct nps_block_iter private_read_blocks_iter;
	};
};

void           vv_bio_inter_init_thick(         union vv_bio_inter *vbi, const struct operation *o);
struct bio_vec vv_bio_inter_get_next_bytes(     union vv_bio_inter *vbi, const struct operation *o, const u32 bytes);	// Get bvec with next 'bytes'
void           vv_bio_inter_copy_private_read_blocks_to_bio_if_needed(struct operation *o);
void vv_bio_inter_set_sg_to_bio_page_maybe_copy(union vv_bio_inter *vbi, struct scatterlist *sg, const struct bio_vec bv, bool should_copy); // Consider unifying this function and above, into 1 function

bool nvmeibc_operation_does_expire_at(struct operation *o, ulong future);
unsigned long nvmeibc_operation_get_expiry_jiffies(const struct operation *o);

/* Is it legal to execute operation in current topology?
   'tnt' is either 'topo' for execution or 'nt' for resubmition */
#define nvmeibc_operation_can_execute_in_topo(tnt, o) \
	(nvmeibc_topo_is_io_ok(tnt) ||	/* All IOs OK */ \
		(((tnt)->io_perm == NVMEIB_IO_TYPE_PERMIT_RDONLY) && \
		 ((o)->op == NVMEIB_BLOCK_IO_OP_READ)))	// R in Read only */

// Relationship between bio_part and operation, when applicable
static inline struct operation * nvmeibc_operation_of_bio_part(   struct bio_part *b) { return (void*)&b[1]; }				// Memory => {bio_part, operation, derrived class}
void                             nvmeibc_operation_start_bio_part(struct bio_part *b, int n_refs);
void                             nvmeibc_operation_add_bio_part(  struct bio_part *b);

/* Execute the operation: when all commands finished we have to complete its execution (maybe retry it if needed). Once we decided that operation should be finished (execution cannot start / it succeeded / timed-out / etc) we destroy it and return answer to kernel. If execution was started it must be completed before destroying operation*/
/* Source of operation: 1. User space, 2. Throttled op pulled by finishing op, 3. resubmitter, 4. mtv-destager, 5. carrier bio, 6. vio */
struct operation * nvmeibc_operation_create_with_biopart(u32 op_size);
struct operation * nvmeibc_operation_create_atomic(const struct operation *o);
void* nvmeibc_operation_alloc_from_sufix(const struct operation *o, u32 alloc_size);
void nvmeibc_operation_execute(       struct operation *o, bool is_from_user_space);		// is_from_user_space=false if 'o' submitted internally, not from user_space
void nvmeibc_operation_execute_chain( struct operation *o);
void nvmeibc_operation_move_mem_to_locks(struct operation *o);								// Upon completion, move memory management to locks
void nvmeibc_operation_complete(struct operation *o, bool do_retry, int o_rv);				// If prepare_op() suuceeded, use complete(), else use destroy.
void nvmeibc_operation_destroy( struct operation *o,                int o_rv);

void nvmeibc_operation_alloc_dbg_id(struct operation *o);

/* Operations IO throttleing */
extern unsigned int max_ios_per_cpu; // Module params: Max in air IO's per cpu
bool nvmeibc_operation_throttling_check_should_execute(struct operation *o);

/* Get core param from operation's block device for page allocation */
int nvmeibc_operation_get_cinst_params_core_tcp_mode(const struct operation *o);

#define get_tcp_mode_of_operation(o) nvmeibc_operation_get_cinst_params_core_tcp_mode((o))

void nvmeibc_operation_free_bio_part(struct bio_part *b);

/* to be used by callers of nvmeibc_operation_create_atomic (sync) */
void nvmeibc_operation_free(struct operation *o);

#endif  // H beginning

