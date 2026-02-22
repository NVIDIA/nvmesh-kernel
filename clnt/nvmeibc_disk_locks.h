#ifndef NVMEIBC_DISK_LOCKS_H
#define NVMEIBC_DISK_LOCKS_H

#include "kr_incs.h"
#include "nvmeib.h"
#include "nvmeibc_types.h"
#include "common/nvmeib_cpu_masks.h"
#include "nvmeib_wd.h"

struct nvmeibc_disk;
struct nvmeibc_ib_net;
struct nvmeibc_locks_channel;
struct nvmeibc_disk_io_command;
struct nvmeib_jrnl_ent_md;

/* Each lock is a state machine which transitions between statuses.
   When multiple (N) locks are required, there is a specific order
   at which we take the locks to avoid deadlock. Lock can be taken in 2 ways:
   1. A loop on all the locks [0..N-1] in which requesting them.
   2. A callbock of lock 'i' being taken which enables taking of lock 'j'.
   An extreme care must be taken to not request the same lock twice, or release
   it twice. Moreover, must treat cases when lock acuisition fails.*/
enum nvmeibc_block_lock_status {
	NCL_STATUS_INVALID 			= 0,		// All   locks - As we kzalloc, by default it is bad.
  //NCL_STATUS_WAIT4ACTIVE 		= 1,		// Owner locks - waiting for an active lock to get a slot. Only afterwards it can become not issued
	NCL_STATUS_NOTISSUED 		= 2,		// All   locks - Lock request was not send to transport layer yet.
	NCL_STATUS_ISSUED 			= 3,		// All   locks - Ready to send lock request. Method which changes from not issued to issued will send the request.
	NCL_STATUS_TAKEN 			= 4,		// All   locks - Lock was successfully acquired. For Read (View-Lock) meaning no contention (no one holds the lock)
	NCL_STATUS_CONTENDED 		= 5,		// Owner locks - Could not be taken, because different client or different thread of the same client holding the lock.
	NCL_STATUS_DONE 			= 6,		// All   locks - For debugging only, lock is not needed anymore and will not be used (typically was taken, IO was performed and lock was released).
  //NCL_STATUS_RECYCLED 		= 8,		// Activelocks - Finished recycling. We use 2 states to avoid race condition (contended owner started releasing the active) while retry timer of owner acquired owner lock and tries to un-recycle before the recycling finished.

	/* Lock request failure */
	NCL_STATUS_FAIL_NO_COMP		= 9,		// All   locks - Cannot send lock request (ie disk is dying). No completion callback will be called on the lock request.
	NCL_STATUS_FAIL_COMP 		= 10,		// All   locks - Same as above, but completion will be called (with error status). Important for per_cpu accounting of in_transfers.
	NCL_STATUS_DISKDEAD 		= 11,		// All   locks - Failed to acquire the lock (pausable layer knows that the disk is dead). Otherwise if disk is dying COMP or NO_COMP might be returned
	NCL_STATUS_DISKDEAD_NO_RETRY= 12,		// All   locks - Failed to acquire the lock and should fail the entire IO. Example: lock was stale but sync operation encountered a permanent read fail
  //NCL_STATUS_ISSUED_DISKDEAD	= 13,		// Activelocks - Lock's slot was reserved but could not take it (remote operation failed). A special case when lock is not taken but must be released (to return the slot back)

  //NCL_STATUS_WAIT4OWNER 		= 15,		// Owner locks - Dual locks & copy of owners: secondary.copy owner lock request must wait for owner request to avoid dead lock of 2 clients/2 threads (one holding the owner, the other holding secondary owner)

	/* Lock release failure */
	NCL_STATUS_ABANDONED 		= 16,		// All   locks - I will never release the lock even though I took it. Will generate stale lock on purpose. Used when write was successfull on one disk but faild on its mirror. I corrupted the data so cannot just release locks
	NCL_STATUS_TAKEN_DISKDEAD 	= 17,		// All   locks - I will never release the lock even though I took it, becasue the disk died and I physically can't access it to release the lock

	NCL_STATUS_TRANSFERRED      = 18,		// Owner locks - Transferred from the previous operation on this CPU, much like TAKEN, acept preious IO requested it and I took it from him locally
}   __attribute__ ((packed));
/* Typical transitions of the state machine: NCL_STATUS_INVALID -> NCL_STATUS_NOTISSUED -> NCL_STATUS_ISSUED -> NCL_STATUS_CONTENDED? -> NCL_STATUS_TAKEN -> NCL_STATUS_DONE */
static inline bool NCL_is_failed_to_release(enum nvmeibc_block_lock_status s){
	return ((s == NCL_STATUS_TAKEN_DISKDEAD)|| /* Tried to release, but failed*/
			(s == NCL_STATUS_ABANDONED)); /* Forbidden to release */
}

static inline bool NCL_is_request_failed(enum nvmeibc_block_lock_status s){
	return (NCL_STATUS_FAIL_NO_COMP == s)
	       || (NCL_STATUS_FAIL_COMP == s)
		   || (NCL_STATUS_DISKDEAD == s)
		   || (NCL_STATUS_DISKDEAD_NO_RETRY == s)
		   || (NCL_STATUS_ABANDONED == s)
		   || (NCL_STATUS_TAKEN_DISKDEAD == s);
}

/* Check if we did our best to acquire a lock but failed (IO also failed) */
static inline bool NCL_is_failed_to_acquire(enum nvmeibc_block_lock_status s){
	return (s == NCL_STATUS_DISKDEAD) || (s == NCL_STATUS_DISKDEAD_NO_RETRY);
}

static inline bool NCL_is_failed_no_retry(enum nvmeibc_block_lock_status s){
	return (s == NCL_STATUS_DISKDEAD_NO_RETRY);
}

/* Have we got a callback as an answer for our request to acquire a lock? */
static inline bool NCL_had_release_callback(enum nvmeibc_block_lock_status s){
	return (s != NCL_STATUS_FAIL_NO_COMP) && (s != NCL_STATUS_TRANSFERRED);
}
static inline bool NCL_had_acquire_callback(enum nvmeibc_block_lock_status s){
	return (!NCL_is_failed_to_acquire(s) && NCL_had_release_callback(s));
}

/* Check if we hold the lock (lock request succeeded) */
static inline bool NCL_do_i_have_lock(enum nvmeibc_block_lock_status s){
	return (s == NCL_STATUS_TAKEN) || (s == NCL_STATUS_TRANSFERRED);
}
static inline bool NCL_do_i_have_owner_lock(enum nvmeibc_block_lock_status s){
	return (s == NCL_STATUS_TAKEN);
}

struct nvmeibc_disk_seg_locks_mem_info {
	/*segment id*/
	int seg_id;
	/*segment start address on the disk*/
	u64 start_addr;
	/*lock set size of this segment [lbas]*/
	u64 lock_set_size;
	/*length of the segment*/
	u64 len;
	/*dirty bit table offset*/
	u64 dirty_bit_offset;
	/*remote memory mapping*/
	u32 rkey;
	/*the local lkey needed for shadow-read*/
	u32 lkey;
	//addr
	u64 addr;
	/*locks channel*/
	struct nvmeibc_locks_channel *locks_channel;
	/*reference to owed disk*/
	struct nvmeibc_disk *disk;
	u64 lock_id;
	/* the pages to access directly the lock buffer */
	struct page **pages;
	/* The lmi pointer from the server (used as a cookie in piggyback lock) */
	u64 lmi;
};

//the used lock segment.
//potentialy a segment can be contacted via one or more nics...
struct nvmeibc_disk_used_lock_segment
{
	struct list_head link;
	/*segment id*/
	int seg_id;
	struct nvmeibc_disk_seg_locks_mem_info *mem_info;
	struct nvmeibc_disk *disk;
};

struct nvmeibc_lock_opr_in_progress;

#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)

#define NVMEIBC_DISK_COMMAND_PROBES_N_TRIES 4

struct nvmeibc_disk_command_probes_try_data {
	unsigned long ulp_post_jif;
	unsigned long llp_post_jif;
	unsigned long llp_comp_jif;
	unsigned long ulp_comp_jif;
	unsigned long retry_timer_jif;
	unsigned long retry_work_jif;
	enum nvmeibc_block_lock_status status;
};

struct nvmeibc_disk_command_probes {
	struct nvmeibc_disk_command_probes_try_data tries[NVMEIBC_DISK_COMMAND_PROBES_N_TRIES];
	unsigned int n_tries;
	unsigned long ulp_first_try_jif;
};

static inline struct nvmeibc_disk_command_probes_try_data *nvmeibc_disk_command_probes_current_try(struct nvmeibc_disk_command_probes *probes)
{
	return &probes->tries[probes->n_tries % NVMEIBC_DISK_COMMAND_PROBES_N_TRIES];
}

static inline struct nvmeibc_disk_command_probes_try_data *nvmeibc_disk_command_probes_prev_try(struct nvmeibc_disk_command_probes *probes)
{
	BUG_ON(probes->n_tries == 0);
	return &probes->tries[(probes->n_tries - 1) % NVMEIBC_DISK_COMMAND_PROBES_N_TRIES];
}
#endif /* NVMEIBC_DISK_CMDS_STATS_PROBES */

struct nvmeibc_d_rdma_comp_tag{}; //helps to discover all callbacks
static inline struct nvmeibc_d_rdma_comp_tag nvmeibc_d_rdma_comp_tag_make(void)
{ return (struct nvmeibc_d_rdma_comp_tag){}; }

struct nvmeibc_d_rdma_comp {	/* Todo: Rename to disk_rdma_comp */
	u64 lockset_id;				/* Ownerlock blockset to refer to*/
	union {
		u64 val[2];				// 128[bits] generic encoding (used in transport layer)
		struct nvmeibc_blkst_arr_req {	// Clnts request to get dbits range
			u64 get_dbits   : 1;// True -> bitmap of dbits.
			u64 get_stales  : 1;// True -> bitmap of stale locks. Can combine both
			u64 get_full_val: 1;// Deprecated since V1.3.2: Always true. If true, Get full value otherwise (!!val)
			u64 reserved    : 1;// Removed in V1.3.1
		} dbits_arr_req;
		struct {				// Result of clnts get dbits range request
			u8 *arr;
			u64 size;			// Nelements in the array above (NOT the size of array in bytes)
		} dbits_arr;
		struct {				// Lock value + embdedded metadata (OWNER LOCK request/release/view, write embedded metadata DIRTY BIT / TxID write)
			u64 id;				// The value that is written in remote server lock (lowest 32 bits are used = result of cmpxchng). NVMEIBC_LOCK_CMP_AND_SWAP, NVMEIBC_LOCK_READ, NVMEIBC_LOCK_FORCE_WRITE
			u64 bi;				// The value that is written in remote server blockset info (lowest 32 bits are used). It'
								// is interpreted as union `nvmeib_blkset_info` type.
			// NVMEIBC_LOCK_BLKSET_INFO_WRITE As piggyback: bi=initialized, id=0
			// NVMEIBC_LOCK_BLKSET_INFO_WRITE As RDMA op  : bi=initialized, id=current held lock id
			// NVMEIBC_LOCK_BLKSET_INFO_READ  Currently unused becuase BINFO is auto-read on each access to locks
			// NVMEIBC_LOCK_CMP_AND_SWAP As lock RDMA op  : bi=destination, id=destination result of cpxchng
			// NVMEIBC_LOCK_CMP_AND_SWAP As unlock RDMA op: bi=unknown, id=unknown.
			// NVMEIBC_LOCK_READ As lock RDMA op : bi=0 (destination), id=0 (result of view lock)
			// NVMEIBC_LOCK_FORCE_WRITE - Unused
		} lock;
	};

	u64 compare;				/*comparand for nvmeibc_disk_interlocked_cmp_exchange*/
	u64 exchange;				/*value to exchange the lock value, not relevant for nvmeibc_disk_read_lock*/
	/* lock id constants */
	const struct nvmeib_lock_entry_constants *lock_cnsts;
	/*keep a pointer to lock channel when the comp is in process*/
	struct nvmeibc_disk_seg_locks_mem_info *mem_info;
	/*callback to be used after operation*/
	int (*callback)(struct nvmeibc_d_rdma_comp*, struct nvmeibc_d_rdma_comp_tag tag);

	enum nvmeibc_disk_locks_opr opr;		/*keep operation, todo: unite with field below and block layer lock->type. 3 fields that mean the same thing */
	volatile enum nvmeibc_rdma_intent code;     // Intention, Same as lock->type but changes to unlock upon release. For piggybacked dirtybits this field is a must coz we dont have lock
	enum nvmeibc_block_lock_status lock_status;					/*status of locking operation*/
	bool mark;
	struct nvmeib_cpu_mask_info cpu_mask_info;
	/*identification*/
	unsigned send_id;

	union {											// Union to save space, only one retry mechanism is used at any time
		struct {									// Transport Layer Retry mechanisms
			uint64_t n_retries_cmpxcng;				// retry count (in case of contention of cmp_xchng)
			unsigned long deferred_jif;				// Timout implementation: jiffies when the comp was posted to the deferred queue. Used to implement timeout
			union {									// link into defered operations
				struct list_head link_deferred;		// link into defered operations
			#ifdef NVMEIBC_LOCK_CH_CB_KERNEL_WQ
				struct work_struct cb_work_transport;
			#else
				struct workqe_struct cb_work_transport;
			#endif
				struct workqe_struct cb_workqe_transport;
			};
		};
		struct list_head resubmit_link;				// Block Layer: When ow-lock with its siblings must be resubmitted use this field to link into locks-resubmit list
		struct workqe_struct transfer_work;			// Block Layer: Execution of locks transfer between operations on a workqueue
		TIMER_LIST_INSTANCE(retry_timer);			// Block Layer: A timer to retry the lock operation. includes a callback method and argument of type nvmeibc_d_iocmd_comp
		struct workqe_struct retry_work_post_timer;	// Block Layer: Work object that executes contended lock retry, scheduled from within the timer
	};

#ifdef DEBUG_D_RDMA_COMP
	struct nvmeibc_lock_opr_in_progress *in_prog_data;
	void *debug_ptr;
	int callback_line;
#endif

#ifdef DEBUG_TRANSFERS
	struct nvmeibc_transfer_reason reason;
	bool in_flight;
#endif // DEBUG_TRANSFERS

	/* Non-Masked Cmp&Swap with (req to read) Blkset-Info */
	struct {
		u32 n_attempts;
		u32 last_value;
	} nmcs_bi;

#if defined(NVMEIBC_DISK_CMDS_STATS) && (NVMEIBC_DISK_CMDS_STATS==1)
	struct nvmeibc_disk_command_stats_only lock_cmd;
#endif

#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && (NVMEIBC_DISK_CMDS_STATS_PROBES==1)
	struct nvmeibc_disk_command_probes probes; //TBD: move into @nvmeibc_disk_command
#endif
};

// Getter helper functions
__attribute__((nonnull (1)))
static inline union nvmeib_blkset_info nvmeibc_d_rdma_comp_get_bi(const struct nvmeibc_d_rdma_comp *self)
{
	return (union nvmeib_blkset_info){ .all = (u32)self->lock.bi };
}

__attribute__((nonnull (1)))
static inline union nvmeib_lock_id nvmeibc_d_rdma_comp_get_compare_lock_id(const struct nvmeibc_d_rdma_comp *self) {
    return (union nvmeib_lock_id){ .all = (u32)self->compare };
}

__attribute__((nonnull (1)))
static inline union nvmeib_lock_id nvmeibc_d_rdma_comp_get_exchange_lock_id(const struct nvmeibc_d_rdma_comp *self) {
    return (union nvmeib_lock_id){ .all = (u32)self->exchange };
}

__attribute__((nonnull (1)))
static inline union nvmeib_lock_id nvmeibc_d_rdma_comp_get_lock_id(const struct nvmeibc_d_rdma_comp *self)
{
	return (union nvmeib_lock_id){ .all = (u32)self->lock.id };
}

__attribute__((nonnull (1)))
static inline union nvmeib_lock_blkset_entry nvmeibc_d_rdma_comp_get_lock_blkset_entry(const struct nvmeibc_d_rdma_comp *self)
{
	return (union nvmeib_lock_blkset_entry){
		.lock_id = nvmeibc_d_rdma_comp_get_lock_id(self), 
		.blkset_info=nvmeibc_d_rdma_comp_get_bi(self)
	};
}

__attribute__((nonnull (1)))
static inline union nvmeib_lock_id nvmeibc_d_rdma_comp_get_contending_id(const struct nvmeibc_d_rdma_comp *self)
{
	return nvmeibc_d_rdma_comp_get_lock_id(self);
}

__attribute__((nonnull (1)))
static inline void nvmeibc_d_rdma_comp_set_lock_id(struct nvmeibc_d_rdma_comp *self, const union nvmeib_lock_id lock_id)
{
	self->lock.id = lock_id.all;
}

#define NVMEIBC_MAX_JAM_RDMA_OPS		1

struct nvmeibc_jam_rdma_op {			// Piggybacked on journal IO command, or sent directly inside dedicated completion struct
	int n_ops;				// If 0, do not execute this operation
	u64 rng_gen_id;
	u32 rng_idx;
	u16 jour_idx[NVMEIBC_MAX_JAM_RDMA_OPS];
	struct nvmeib_jrnl_ent_md ent_md[NVMEIBC_MAX_JAM_RDMA_OPS];
	struct jentry_md jmdc_ent[NVMEIBC_MAX_JAM_RDMA_OPS]; /* not using @jentry_md_container to reduce mem alloc&zero size */
};

/**
 * holds information about all segments in a disk
 *
 * @param
 */
struct nvmeibc_disk_segments_locks {
	/*number of segments*/
	u32 num_of_segments;
	/*array of segment locks*/
	struct nvmeibc_disk_seg_locks_mem_info *locks;
	/*connection channel*/
	struct nvmeibc_locks_channel *lock_ch;
	/* local locks */
	bool is_local;
	/* rw_semaphore guard */
	struct rw_semaphore guard;
	struct nvmeibc_disk *disk;
};

/*management*/

/**
 * return a disk lock mem info struct
 *
 * @author yaron (5/25/2015)
 *
 * @param disk - disk of the lock
 * @param seg_id - segment id
 *
 * @return nvmeibc_disk_seg_locks_mem_info* lock record.
 *         null if the lock was not found
 */
void * nvmeibc_disk_locks_seg_locks_mem_info(
	struct nvmeibc_disk *disk, int seg_id);

/**
 * free disk lock mem info struct
 *
 * @author yaron (11/3/2015)
 *
 * @param handle - handle to free
 */
void nvmeibc_disk_locks_free_mem_info(void *handle);

/*lockin' api*/

/**
 * Performs an atomic compare-and-exchange on disk lock resources.
 * The function compares the value in the disk lock entry with
 * a given value and exchanges with the given value based on the
 * outcome of the comparison.
 * When using this Api the user must compare "initial" to "compare" to know if
 * the compare was success
 *
 *
 * @param record - record corresponds to a segment in a disk.
 * @param addr - addr .
 * configured to that segment
 * @param compare - comperand
 * @param exchange - the value to exchange
 * @param initial - lockset original value
 *
 * @return int 0 upon sucess. (user must compare the initi
 */
int nvmeibc_disk_locks_interlocked_cmp_exchange(void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp);

/**
 * force the value of a lock record
 *
 * @param record segment parameters
 * @param addr lock disk address
 * @param val value to force
 * @param comp returned values
 *
 * @return int 0 upon sucess.
 */
int nvmeibc_disk_locks_write_lock(void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp);

/**
 * read the value of a lock record
 *
 * @param record segment parameters
 * @param addr lock disk address
 * @param comp returned values
 *
 * @return int 0 upon sucess.
 */
int nvmeibc_disk_locks_read_lock(void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp);

/* Same as above but read a range of problems in blocksets. Returns a bitmap */
int nvmeibc_disk_get_db_ec_rl(     void *handle, u64 start, u64 length,
	struct nvmeibc_d_rdma_comp *comp);
struct nvmeibc_disk_gen_cmd;
void nvmeibc_disk_get_ec_db_comp(struct nvmeibc_disk_gen_cmd*, int comp_code);

/** Write / Read transaction-id (to/from comp->val[0])
 * @return int 0 upon success.
 */
int nvmeibc_disk_locks_write_blkset_info(void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp);

/**read the value of transaction-id entry of a given address to
*  comp->val.
*/
int nvmeibc_disk_locks_read_blkset_info(void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp);

/**
 * called when a lock trasaction is completed
 */
int nvmeibc_disk_locks_on_completion(struct nvmeibc_locks_channel *ch,
									 struct nvmeibc_ib_net *net, struct ib_wc *wc, bool last_wc_in_series);

/**
 * called upon completion of compare-exchange call
 *
 * @param wc
 */
void nvmeibc_disk_locks_on_cmp_exchange( int id,
	struct nvmeibc_d_rdma_comp *lock_comp,
	struct nvmeibc_ib_net *net);

/**
 * handles watchdog event
 *
 * @author yaron (6/16/2015)
 *
 * @param comp
 * @param time_passed
 *
 * @return int
 */
int nvmeibc_disk_locks_handle_wd_event(
	struct nvmeibc_lock_opr_in_progress *opr_ip, unsigned long time_passed);

/**
 * attach used segments
 *
 * @author yaron (7/26/2015)
 *
 * @param disk
 */
void nvmeibc_disk_locks_attach_used_segments(struct nvmeibc_disk *disk,
											 struct nvmeibc_disk_segments_locks *disk_seg_locks);

/**
 * return used segment lock id
 *
 * @author yaron (7/26/2015)
 *
 * @param handle
 *
 * @return U64
 */
u64 nvmeibc_disk_lock_get_lock_id(void * handle);

int nvmeibc_disk_locks_extract_info(void *handle, u64 addr, int *segid,
	enum nvmeibc_disk_locks_opr opr_type, u64 *offset);

/**
 * drain all locks operation that are not being processed at the time
 * of the call. Another version is a safe method for higher
 * level software to call, which validates existance of lock channel
 *
 * @author yaron (3/7/2016)
 *
 * @param ch
 */
void nvmeibc_disk_locks_drain_defered(struct nvmeibc_locks_channel *ch, bool only_expired, bool this_ch_only);
void nvmeibc_disk_locks_abort_in_progress_oprs(struct nvmeibc_locks_channel* ch, bool this_ch_only);
void nvmeibc_disk_locks_abort_all_oprs(struct nvmeibc_locks_channel *ch);

int nvmeibc_disk_locks_server_side_post_send_atomic(struct ib_qp *qp,
	struct nvmeib_send_wr *send_wr, struct nvmeib_send_wr **bad_send_wr);

void nvmeibc_disk_locks_process_deferred(struct nvmeibc_locks_channel *ch);

#endif //NVMEIBC_DISK_LOCKS_H
