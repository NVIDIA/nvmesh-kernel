#ifndef NVMEIBC_DP_IO_REQ_REL_LOCKS_H
#define NVMEIBC_DP_IO_REQ_REL_LOCKS_H
/* Generic mechanism for requesting/Releasing and viewing locks, regardless of
   datapth, protection raid type, etc */
#include "../../nvmeibc_block.h"		/* external API of the block */

// Todo: Move the below to somewhere else
#define LOCKSET_MASK (~(long long)(LOCKSET_SLICES-1))     // Zero the lowest X bits which represent blocks within a lockset.

/******************************** Lock ID API *********************************/
struct nvmeibc_raid1;
u64     get_lockid_for_cmpxchg(const struct nvmeibc_raid1 *r1, enum nvmeib_block_io_op op);

/* result of tranport layer failed rdma cpxchng (id that holds lock) */
#define get_contending_id(lock_comp)  ((lock_comp)->lock.id)

/************************************ Locks ***********************************/
// indicate the value we should put in the lock when we release it.
enum release_lock_value {
	RELEASE_LOCK__UNLOCKED = 0,			// write it as unlocked (must be 0 for kzalloc)
	RELEASE_LOCK__STALE_SPECIAL = 1,	// write stale-special instead of the lock (In R1 only)
	RELEASE_LOCK__FORCE_ABANDON = 2,	// Deliberately do not release this lock
} __attribute__ ((packed));
#define R1_STALE_SPECIAL_BINFO_VAL (nvmeib_stale_special_raid1.all)			//u64 -- Todo: Remove this. No need for lock operations to know binfo
#define R1_STALE_SPECIAL_LOCK_VAL  (nvmeib_stale_special_raid1.lock_id.all) //u32

// For debugging: indicate the reason why we decided to release lock to a value other than UNLOCKED or abandone
enum release_lock_reason {
	RELEASE_LOCK_REASON__NONE 				= 0,				// 0 must be used by the default (regular unlock to 0). can also be used to eliminate a previously set reaso in case the unlock is canceled until further decided.
	RELEASE_LOCK_REASON__IO_WITHOUT_SYNC,						// IO to sub LOCKSET size decided to skip sync
	RELEASE_LOCK_REASON__ABANDON_WRITE_FAILED,					// Write got negative completion. Maybe it wrote to disk, maybe not (possible slice corruption)
	RELEASE_LOCK_REASON__ABANDON_WRITE_PARTIAL_SLICE,			// At least one write succeeded and one not sent (guaranteed slice corruption)
} __attribute__ ((packed));

// set the unlock value & the reason associated with it (i.e.: why it is this value during unlock).
#define NVMEIBC_LOCK_SET_UNLOCK(_lock, _unlock_val, _unlock_reason) ({\
	BUG_ON((_unlock_reason < RELEASE_LOCK_REASON__NONE) || (_unlock_reason > RELEASE_LOCK_REASON__ABANDON_WRITE_PARTIAL_SLICE)); \
	(_lock)->unlock_val = _unlock_val;								\
	(_lock)->unlock_reason = _unlock_reason;						\
})

/* This structure represents a single remote lock access action. A few instances
 * might be needed to perform a single IO on a volume (Owner, copy). All
 * instances that protect a single blockset are called siblings.
 * A few commands might need the same lock. Once we acquire it, all commands can execute.
 * Thus we save time on locking/unlocking for each command
 */
struct nvmeibc_cmd_lock {
	// Todo: Move to header struct. Fields, only of the first lock in the array
	int nlocks;                                 // There is array of locks (array of this structs). The first lock stores the length of the array
	nvmeibc_atomic_t n_uncompleted_locks;       // Amount of locks that are used by the currently executed comamnds or finished commands that hasn't released the lock yet. When drops to zero, this struct can be free.
	struct nvmeibc_block_command *cmds;         // Array of commands that use locks (use this or other locks, from the array of locks). In C++ facion: this[i] might be used by this->cmd[j]
	struct nvmeibc_block_command *new_cmds;		// DISCARD ops copy for lock contendedd cmds (split)
	struct nvmeibc_topology *topo;				// WARNING: Usually is NULL. Used to temporarily store the topology reference between operation release and lockset release
	#ifdef DEBUG_TOPO_CNTRS
		struct debug_topo dbg_topo;				// For debugging, each lockset and operation connect to topology they are launched on
	#endif
	//struct {// Todo: Move to header_trigger struct. Fields, only of the first lock in the array
		struct nvmeibc_cmd_lock *asker;			// Pointer to lockset to whom I shall transfer my last owner lock instead of releasing it. A lock that has requested to be transferred once this lock is done
		struct nvmeibc_cmd_lock *giver;			// Inverse pointer of the above. The lockset from which this lockset has request a transfer (pointer to the lockset whos last owner lock will be transferred to us replacing our first lock)
		union {
			struct nvmeibc_topo_percpu *last_ls;	// Points to the last IO request running on the same topology (and same CPU) as this locksets. I can request from this IO to transferr some of its locks to me.
			void *user_data;						// Auxiliary payload for that lock. Todo: Union with entire triggere struct, not with this specicif field
			bool commit_only_owner_binfo;			// Tells commit binfo if it should only take the binfo from owner or merge all binfos.
		};
	//};
	// Generic fields of all type of locks locks
	u64 address;								// Range of addresses that this lock protects, start address is given, length is constant size 1 blockset
	//u64 length; == __to4K(LOCKSET_SLICES);
	struct nvmeibc_disk_segment *ds;            // Reference to disk segment on which this lock guards a short range of addresses
	struct nvmeibc_d_rdma_comp comp;          	// Completion structure (Given via callbacks from transport layer and include its fields)
	enum nvmeibc_rdma_intent type;              // Todo: Remove this, use the 'comp.code' field. value What we want to do with the lock, can be equal to only the lock values of that enum
	enum nvmeibc_block_lock_status status;      // Status of the lock (taken, contended).
	enum release_lock_value	 unlock_val;		// what should we write to the lock when we decide to release
	enum release_lock_reason unlock_reason;		// the reason we have a special unlock (for debugging)
	#ifdef DEBUG_LOCKS_CORRUPTION
		nvmeibc_atomic_t n_callbs;                      // Amount of callbacks the lock received. Should become 1. 0 - Lock was not processed yet. IO is stuck. 2+ means vicious corruption
	#endif
	int owner_idx;                               // In protection raid we lock only on 1 owner (to avoid deadlocks). Index of the owner (index in the array of locks). Owner and the rest of its mirrored locks are called siblings
	int n_siblings;                             // Todo: Move to be field of only primary owner lock.
	int lockset_idx;                           //Index of this "lock command" within the lockset array - needed to refer to the header;
	u32 mem_allocated_size;                    // set for the first lock when operation and locks don't share memory, used by memmgr metrics on-free accounting

#ifdef DEBUG_CONTENDED_LOCKS
	/* For debug of contended locks */
	u32 first_try_contender_id;
	u32 first_try_txid;
	u32 curr_contender_id;
	u32 curr_txid;
	unsigned long lock_taken_jif;
#endif

	struct /* all owner locks */{
		union {
			nvmeibc_atomic_t ncmds;		// Amount of commands still using the this lock (currently need it). Instead of each sibling holding his own 'ncmds', owner holds the sum for all
			int      ncmds_non_atomic;	// Tmp assist variable for calculating 'ncmds' in a loop (Avoid expensive atomic operations on ncmds).
		};
		int secondary_id;				// 0, normally, index of sec+1 in dual lock topology. -1 when copy of owners is used
		struct  /* Primary owner only */{
			nvmeibc_atomic_t pending;		// The amount of locks that we still need to acquire access to our protected raid. If we have 2 replications (raid1) pending starts from 2 and when we have both locks drops to 0. No we can continue with IO and then release locks
			nvmeibc_atomic_t prediscards;	// For TRIM (discard) operations, first we want to receive all the results of owner locks requests, and later process the other locks. Stores the amount of needed owners.
		};
		union {										// To retry a contended work, a timer is scheduled with a random delay. From the timer, the actual work to do the resubmission is done, in order not to do the whole thing from within the timer context, which may cause soft lockup.
			struct {								// Struct for retrying acquring contended owner lock
				u32 retries;                        // Amount of time we are going to retry to acquire a lock until we say that IO failed
				bool need_stale_sync;
				unsigned long first_try_time;       // The time(jiffies) in which the first lock request launched.
				unsigned long last_retry_report_time; // The time(jiffies) in which the last problematic locking time reported.
			};
			struct {								// Upon release
				void *pg;							// When operation and locks share memory: Absuce pointer to operation, in case
				bool resubmit_operation_dont_free;	// When operation and locks share memory: After unlock resubmit operation or free the memory
			};
		};
	};
} __attribute__((aligned(sizeof(long))));			// Multiple of 64 bits, for nice allignement

__attribute__((nonnull (1)))
static inline union nvmeib_blkset_info nvmeibc_cmd_lock_get_bi(const struct nvmeibc_cmd_lock *self)
{
	return nvmeibc_d_rdma_comp_get_bi(&self->comp);
}

__attribute__((nonnull (1)))
static inline void nvmeibc_cmd_lock_set_bi(struct nvmeibc_cmd_lock *self, union nvmeib_blkset_info binfo)
{
	self->comp.lock.bi = binfo.all;
}

static inline void nvmeibc_copy_blockset_info(struct nvmeibc_cmd_lock *dst, const struct nvmeibc_cmd_lock *src)
{
	dst->comp.lock.bi = src->comp.lock.bi;
}

struct t_abandon {				// Assist struct for calculations whether blockset locks should be abondoned or not.
	u8 wr_not_issued;			// Raid1: Abandon the locks unless all commands were successful or none of the commands were issued
	u8 wr_failed;				// Raid5: Uses more subtile algorithm decision.
	u8 wr_succeeded;			// Each datapath has a virtual function which uses this struct to decide if should abandon or not
	u8 unused;
};

__attribute__((nonnull (1)))
raid_sgmnt_t dp_locks_get_sgmnt_idx_of_lock(const struct nvmeibc_cmd_lock *self);

/* Given an empty array of locks, append the locks which
   protect range [start_lba,start_lba+nlbas). Returns the amount of locks. The
   array stores concatenated arrays of locks of raids covered by the io */
int dp_fill_locks_for_io(enum nvmeib_block_io_op op, u64 nlbas, u64 vlba,
	int c_i, struct nvmeibc_topology *t, struct nvmeibc_cmd_lock *locksets,
	const int locksets_len, const struct nvmeib_cpu_mask_info *cpu_mask_info);

/* Same as above but do this for a specific offset in raid */
int dp_fill_locks_for_raid(const struct nvmeibc_raid1 *r1,
	enum nvmeib_block_io_op op, u64 rlba, struct nvmeibc_cmd_lock *locksets);

/* Request all the needed locks in the input array. Acquiring lock reads binfo (viewing lock - does not) */
void dp_locks_send_all(					struct nvmeibc_cmd_lock *locksets);

/* Same as above but request locks for a single raid (lock transfer failed) */
void dp_locks_resend_raid_locks(		struct nvmeibc_cmd_lock *locksets);

/* Get RV of blockset locks acquisition state machine */
int  dp_locks_calc_blockset_rv(const	struct nvmeibc_cmd_lock *owner_lock);

/* Get binfo (TxID & Dbits) of blockset from locks state machine */
union nvmeib_blkset_info dp_locks_get_TxID_dbits(const struct nvmeibc_cmd_lock *locksets, int owner_i, bool only_owner);

/* Inverse of the above, after binfo was altered by maintanance op's (like TxID
   wraparound), inject it back into the locks */
void dp_locks_put_TxID_dbits(struct nvmeibc_cmd_lock *locksets, int owner_i,
							 union nvmeib_blkset_info binfo, bool can_put_unknown_txid);

/* @return non-zero if binfo is valid, 0 otherwise */
bool __attribute__((warn_unused_result)) verify_binfo_is_legal(struct nvmeibc_disk_segment *ds, const union nvmeib_blkset_info binfo, const u64 blockset_dlba, const char action);

/* For EC / QLC recoveries and IO stages we update all lock binfos with a new value */
void dp_locks_write_all_blocksets_info_op(struct nvmeibc_cmd_lock *ow_l, const union nvmeib_blkset_info *binfo, int (*callback)(struct nvmeibc_d_rdma_comp*, struct nvmeibc_d_rdma_comp_tag), int prev_rv);

/********************* Stale Locks During lock acquisition ********************/
sgmnts_bmp_t dp_locks_stale_get_locks_bitmap_in_blockset(struct nvmeibc_cmd_lock *owl);
int  dp_locks_stale_call_sync_blockset(      struct nvmeibc_cmd_lock *owl,                     ulong retry_locks_bmp);
void dp_locks_stale_after_sync_retry_acquire(struct nvmeibc_cmd_lock *owl, int sync_rv, sgmnts_bmp_t retry_locks_bmp);

/***************************** Locks Release **********************************/
/* Release/Abandon/Transfer owner lock and all its siblings (in correct order)*/
void dp_locks_release_locks_sibs(struct nvmeibc_cmd_lock *locksets, int owner_i);

/* Callback of the above function (allways called, on each of the siblings). */
int  dp_locks_release_cb(   struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag);	// Callback for e
void dp_locks_complete_lock(struct nvmeibc_cmd_lock *locks, int nrefs, int lsi, const bool release);	// Complete state machine for this lock. No more work has to be done
void dp_locks_free_all(     struct nvmeibc_cmd_lock *locks);	// Actually kfree the locks array (all owner locks with their siblings)

/******************************* Locks View ***********************************/
/* State machine for viewing lock value instead of acquire/release lock.
	State-machine is activated when:
		1. Read disk command finished and piggibacked a view lock.
		2. Read disk command failed and piggyback is not needed/irrelevant
		3. Read disk command finished and view lock has to be explicitly sent (methematically it cant be piggybacked)
		4. Piggyback/ Explicit-view-lock failed, need to retry it.
   After read command completed, read piggibacked lock value or rdma read the
   owner lock value (do not compare exchange it) */
int  dp_locks_view_lock_sm(       struct nvmeibc_d_rdma_comp *read_comp, struct nvmeibc_d_rdma_comp_tag tag);	// State machine for blockset (access of 1 owner lock) and also a callback
//void dp_locks_send_read_lock(struct nvmeibc_d_iocmd_comp *cmp);		// No declaration as it is internally called
void dp_locks_read_complete(struct nvmeibc_cmd_lock *locksets, int lsi, enum nvmeibc_block_lock_status status);	// Termination of owner lock view state machine

/*****************************************************************************/
// Todo: move this to .c file
#define __change_lock_status_to(l, s) ({ (l)->comp.lock_status = (l)->status = (s); })

__attribute__((nonnull(1)))
static inline struct nvmeibc_cmd_lock const* __dp_locks_get_blockset_owner_lock_impl(struct nvmeibc_cmd_lock const* self)
{
	size_t const distance = self->lockset_idx - self->owner_idx;
	return self - distance;
}

//working correctly with constness
#define dp_locks_get_blockset_owner_lock(self) \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof__(self), const struct nvmeibc_cmd_lock*),	\
		__dp_locks_get_blockset_owner_lock_impl(self),														\
		(struct nvmeibc_cmd_lock*)__dp_locks_get_blockset_owner_lock_impl(self)) 							\

__attribute__((nonnull(1)))
static inline struct nvmeibc_cmd_lock const* __dp_locks_get_locks_header_impl(struct nvmeibc_cmd_lock const* self)
{
	return self - self->lockset_idx;
}

//working correctly with constness
#define dp_locks_get_locks_header(self) \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof__(self), const struct nvmeibc_cmd_lock*),	\
		__dp_locks_get_locks_header_impl(self),																\
		(struct nvmeibc_cmd_lock*)__dp_locks_get_locks_header_impl(self)) 									\


#define nvmeibc_b_rdma_comp_init(comp, lsi, locksarr) ({ locksarr[lsi].lockset_idx = lsi; })

#define                                    lock_of_bcomp(                              dc)  container_of(dc, struct nvmeibc_cmd_lock, comp)
static inline struct nvmeibc_d_iocmd_comp *get_d_comp_of_pg(struct nvmeibc_d_rdma_comp *d) { return container_of(d, struct nvmeibc_d_iocmd_comp, pigbck_comp);}

/* backoff time for lock retry */
ulong dp_locks_get_retry_time(struct nvmeibc_cmd_lock *l);

// Iterator on primary owners (blocksets), when lcoks cannot be freed
#define for_each_primary_owner(lsi, locksets) \
	for (lsi = 0; lsi < locksets->nlocks; lsi += locksets[lsi].n_siblings)

// Iterator in unsafe context (like requesting locks) where they can get free before we reach the loop end
#define for_each_primary_owner_safe_loop_start(i, locksets, start, end) { \
	int _end_i = (end), _n_siblings; \
	for (lsi = start; lsi < _end_i; lsi += _n_siblings) { \
		_n_siblings = locksets[lsi].n_siblings;

#define for_each_primary_owner_safe_loop_end() \
	} \
}

#define dp_locks_activate_timer(dc, func, arg, retry_time) ({ \
	SETUP_TIMER(&(dc)->retry_timer, (func), (unsigned long)arg, TIMER_PINNED); \
	TIMER_SET_DATA(dc, retry_timer, (unsigned long)arg); \
	mod_timer(&(dc)->retry_timer, retry_time); \
})

/***************************** Locks Tracing **********************************/
struct operation;
void dp_locks_trace_lock_comp(   const struct operation *o, const struct nvmeibc_cmd_lock *l, const struct nvmeibc_d_rdma_comp *lock_comp);
void dp_locks_trace_lock_release(const struct operation *o, const struct nvmeibc_cmd_lock *l);

/***************************** PET APIs **********************************/
void nvmeibc_cmd_lock_request_io_pet_describe(struct operation const* o, struct nvmeibc_cmd_lock const* lock);
void nvmeibc_cmd_lock_response_io_pet_describe(struct operation const* o, struct nvmeibc_cmd_lock const* lock);

#endif  // H beginning

