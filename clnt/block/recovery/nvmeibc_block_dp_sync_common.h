#ifndef NVMEIBC_BLOCK_DP_SYNC_COMMON_H
#define NVMEIBC_BLOCK_DP_SYNC_COMMON_H
/* Sync operations are not bio based (in contrast to READ / WRITE / TRIM).
   There is up to MAX_ALLOWED_PARRALLEL_SYNC_OPS sync operations per volume.
   The rest are waiting for their turn. The sync itself does not have a timeout.

   A Sync operation is allocated upon being triggered & freed once the attempt
   has completed, whether successfully or not. this ensures that a sync doesnt
   live for too long thereby preventing topologies from being deleted. the
   caller must therefor deal with the case that the sync has failed & if so it
   might have to re-try the sync.

   There are 4 types of syncs:
	1. Stale special lock recovery (fix up of entire blockset)
    2. Converting stale special to dirty (same as 1 but in degraded mode)
	3. Permanent readfail recovery
    4. Dirtybits recovery

   General Sync steps:
    1. Grab sync slot (0..MAX_ALLOWED_PARRALLEL_SYNC_OPS-1)
    2. Allocate sync buffer. There are a maximum of 10 for starters
    3. SYNC initializes 1 owner lock (or owner + dual). It never uses actives.
	4. Lock the owner lock - presuming it is stale. If this fails or is
	   contended, abort.

    *. The data recovery is done using virtual functions of each data path.
       Refer to specific documentation in each datapath.
       Each datapath has implemetation for 3 types of SYNCS:

    5. Finish / abort - release the lock if taken.
    6. Call on completion a callback (Wake up the original IO).


   This module has the following components:
    1. Throttling of Syncs - not issuing more then predefined amount
    2. Handling Locks for Sync - Stale, stale special, dual locks etc.
    3. Implementation of common (internal) API.
    4. Implementation of external API.
    5. Todo: move out from here: convert stale special to dirty for raid-1
*/

#include "block/recovery/nvmeibc_block_dp_sync_api.h" /* external API */
#include "block/datapath_utils_generic/nvmeibc_block_dp_lock_server.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec.h"
#include "block/nvmeibc_topology.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_async_mode.h"

#define __is_raid1_ec(so)       ((so)->r1->slice_size != 1)
#define __is_raid1_mirror(so) ((so)->r1->slice_size == 1)
#define is_op_sync_stale(op)    ((op) == NVMEIB_BLOCK_IO_OP_RECOVER_STALE)
#define is_op_sync_cold(op)     ((op) == NVMEIB_BLOCK_IO_OP_REC_COLD)
#define is_op_sync_db(op)       ((op) == NVMEIB_BLOCK_IO_OP_RECOVER_DB)
#define is_op_sync_readfail(op) ((op) == NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL)
#define is_op_sync_rollback(op) ((op) == NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK)
#define is_op_sync_hot_jgc(op)  ((op) == NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_HOT_GC)
#define is_op_sync_scrubbing(op) ((op) == NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING)
// EC sync no write hole is both DB sync and RF sync
#define is_op_sync_no_wr_ho(op) (is_op_sync_readfail(op)||is_op_sync_db(op)||is_op_sync_rollback(op)||is_op_sync_scrubbing(op)|| ((op) == NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO))
#define is_op_sync_txid_wrap(op) ((op) == NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP)
#define is_op_sync_maintain(op)	 ((op) & NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO)
#define is_op_sync_commit_binfo(op) ((op) == NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO)
#define is_op_sync_commandless(op)  ((op) == NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON)
#define is_op_sync_read_md_only(op)  (is_op_sync_maintain(op) || is_op_sync_commit_binfo(op))

/* Generic state machine of all sync operations */
enum sync_op_stage_e {
	sync_stage_start = 0,

	/* 1. States of converting stale lock to dirty bits*/
	sync_stage_st_to_db_written_db,
	sync_stage_st_to_db_stale_released,

	/* 2. Acquire all needed locks for this blockset (for all sync op) */
	sync_stage_recov_lo_try_lock,
	sync_stage_recov_lo_try_lock_cb,
	sync_stage_recov_lo_next,
	sync_stage_recov_lo_all_taken,		// All the above states will fail upon error to lock release (2.3)

	/* 3. Once locks are taken, analyze binfo, call maintanace ops to fix it*/
	sync_stage_recov_analyze_binfo,

	/* 3.1 Change 'so' according to the state of fixed binfo and locks */
	sync_stage_mutate_done,				// Mutation is finished (or error)

	/* 4. Do the commands of sync (varies according to data path type).
	      on error all states will fail to cmds_done.
	      Daniel: Todo: Use so->cmds->raid_cur_stage for this state-machine */
	sync_stage_recov_read_cmds_sent,	// Read the data
	sync_stage_recov_restore_complete,

	/* 4.0 Stages of maintenance sync */
	sync_stage_recov_mainten_sbs_loop_end,	// If encountered readfail, read slice-by-slice
	sync_stage_recov_mainten_resolve_binfo,	// All reads complete, resolve unknown parts of binfo from on-disk MD

	/* 4.1. States of recovering stale-special locks and dirtybyte */
	sync_stage_recov_do_sync_stale,
	sync_stage_recov_commit_binfo_before_rollback,
	sync_stage_recov_do_rollback,

	/* 4.2 - No write hole stages */
	sync_stage_recov_no_write_hole_turnon_ram_dbits,
	sync_stage_recov_no_write_hole_turnon_ram_dbits_done,
	sync_stage_recov_no_write_hole_read_done,
	sync_stage_recov_no_write_hole_restore_complete,
	sync_stage_recov_no_write_hole_turnon_ram_dbits_before_turnoff,
	sync_stage_recov_no_write_hole_sent_restore_data_and_turnon_parity_md_dbits_done,
	sync_stage_recov_no_write_hole_turoff_parity_md_dbits_done,
	sync_stage_recov_no_write_hole_sbs_loop_end,

	/* 4.3 - Txid wraparound stages */
	sync_stage_recov_txid_wrap_read_done,
	sync_stage_recov_txid_wrap_turnon_ram_dbits,
	sync_stage_recov_txid_wrap_turnon_ram_dbits_done,
	sync_stage_recov_txid_wrap_sent_maxtxid_and_turnon_dbits_done,
	sync_stage_recov_txid_wrap_sent_txid_no_journal_done,


	/* 4.2.2 Sending write commands for all types of syncs */
	sync_stage_recov_write_cmds_sent,	// Send the write commands - unused stage LKJ: to remove.
	sync_stage_recov_write_cmds_done,	// Lockset is receovered or sync failed

	/* 4.3. If Sync must send blockset recovered to TOMAs of all locks,
	   will continue to write binfo if needed */
	sync_stage_recov_send_recovered,
	sync_stage_recov_send_recovered_thread,	// Rescheduled on resubmit thread context, can use sleeping allocs

	/* 4.4. If Sync must write blockset info back to all owner locks (fixed dirty
	   bits, resolved dirty suspects, etc) do the states below */
	sync_stage_recov_write_binfo, 			// Write binfo, from various state machines (clean dirty bit, txid, to all copies of locksm etc)
	sync_stage_recov_write_binfo_done,

	/* 5. Release locks */
	sync_stage_recov_owner_ulock_sm,		// Start lock release state machine, All other state machines finished.
	sync_stage_recov_un_lock_release, 		// Errors during those states are ignored
	sync_stage_recov_un_lock_release_cb, 	// Errors during those states are ignored
	sync_stage_recov_un_lock_next_lock, 	// Errors during those states are ignored

	/* 6. Bad path */
	sync_stage_recov_rereg_on_error,		// Reregister in case of errors on sync completion

	/* 7. Notify caller IO about sync termination */
	sync_stage_done,
};

struct recovery_sync_stack {		/* Support nested syncs: Example, stale lock syncs calls read-fail sync */
	void (*ret[3])(struct recovery_sync_op *);		// Supports up to 4 nested sync ops (3 inside original), return address
	enum sync_op_stage_e stage[3];					// Stage of sync operation. Same as registered, Saved/Restored via push()/ret()
	enum nvmeib_block_io_op op[3];					// Type of sync operation
	u32 sp;											// Stack pointer. When 0, means no recursive syncs. Range [0..2]
	void (*cleanup[3])(struct recovery_sync_op *);  // Optional: When 'so' finishes it cleans up tmp variables before returning to caller so
};

/******** NO WRTIE HOLE PARAMS related stuff ****************/
union no_writehole_params {
	struct {
		roles_bmp_t dbits_turnon_bmp  : 16;           /* Bitmap: On which roles should nwhole sy turnon dbits. */
		roles_bmp_t force_rebuild_bmp : 16;			  /* Bitmap. Default Empty. Which segments should be written to disk. Block which is written to disk might be read from disk, recalculated, generated in some way. For each non dead segment, actual write hits the disk, for dead segment dbit is turned on  */
		bool destroy_full_slice       : 1;            /* Boolean. Default False. Can be set to True in addition and only if force-rebuild covers all segments - then write bad sectors to the entire slice (fix it by destroying it entirely). Read more about this in appendix */
		bool must_fix_bad_sectors     : 1;            /* Boolean. Default is false. If false, bad sectors can exists and might be solved by this sync, but caller does not need them to be solved. Default is false and is typically set to true only if caller indeed encountered bad sectors */
		bool must_turn_off_dbits      : 1;            /* Boolean. Default is true. If true and according to topology an existing dbit can be turned off - then, no-writehole will auto detect it & do all necessary tasks to turn it off. Default is true and never set to false (except manual ioctls), to speed up Toma topologies transitions. */
		bool must_scrub               : 1;            /* Boolean: Default is false. If true need to scrub the blockset. */
		u32 reserved                  : 28;            /* Padding */
	};
	u64 raw;
} __attribute__((packed));

static const union no_writehole_params no_writehole_params_default = {
	.dbits_turnon_bmp = 0, .force_rebuild_bmp = 0,
	.destroy_full_slice = false, .must_fix_bad_sectors = false,
	.must_turn_off_dbits = true,
	.reserved = 0};

static const union no_writehole_params no_writehole_params_empty = {.raw = 0};

struct no_writehole_execution_plan {
	roles_bmp_t first_write_bmp;								/* Which blocks should be written to disk in the first stage write. */
	roles_bmp_t second_write_bmp;								/* Which blocks should be written to disk in the 2nd stage write - currently just the writable parities for dbits turnoff. */
	roles_bmp_t invalid_sources;								/* Which blocks are not readable */
	// struct ram_dbits_struct {
		union nvmeibc_dbits_entry ram_dbits_after_first_turnon;	/* If should_write_ram_dbits_first - these are the dirty bits we write */
		union nvmeibc_dbits_entry ram_dbits_after_turnoff;		/* The ram dbits state after turnoff. */
		u16 should_write_ram_dbits_first : 1;					/* If true - make surte to write dbits to ram as a barrier just before turnoff */
		u16 should_turnoff_dbits : 1;
		u16 encountered_bad_sectors : 1;						// During reads, have seen at least 1 bad sector
		u16 was_in_sbs_mode : 1;								// True, after no writehole slice by slice processing finished. For flow documentation and prints
		u16 is_blkset_corrupted : 1;							// Encountered unexpected corrupted state in a blockset slice.
	// };
};

/******* End of NO WRTIE HOLE PARAMS related stuff ***********/

struct recovery_sync_op {
	struct operation *o;				/* C++ style of inheritance from 'o', by including it */
	struct list_head sync_request_link;	/* Todo: remove, and use the one in 'o' Connect to resubmit thread list */
	struct nvmeibc_cmd_lock locks[N_MAX_RAID_LOCKS]; /* Optimization: lock only owners */
	struct nvmeibc_pages pages;			/* Sync stale: Space for N_MAX_RAID_SLICE_LEN full locksets to read, Readfail: 1 read */
	struct nvmeibc_block_command *cmds;	/* Array of commands, using the buffers above, todo: remove, use o.cmds */
	struct nvmeibc_raid1 *r1;			/* Protection raid slice we are fixing, units of blocks */
	u64 rlba; 							/* First rlba of the blockset (lock) we are fixing */
	const struct nvmeibc_block_command *orig_rldr;	/* The orignial cmd of caller that needed help, and spawned sync op. Even though it is const, its sg-buffers can be filled by sync*/
	union /* Run time claculations */ {
		struct t_sync_mirror_extension {/* RAID-1: */
			bool is_dirty_suspect;		/* Mark dirty suspect recoveries for accounting */
			int valid_read_index: 8;	/* Index of the segment from which we actually obtained a valid copy of the data (RW with no bad sector, W with no dbits, etc)*/
		} R1;
		struct /*t_sync_ec_extension*/ {/* RAID-5: */
			uuid_be recoveree_cuuid;	/* For EC recovery - the client UUID of the client under recovery */
		} /* ec */;
		struct resource_reuse_container {		// When sync acts as container for resource reuse. This happ
			int tcp_mode;
			bool should_trasnfer_resources_for_another_sync;
		} rr_container;
	};

	union no_writehole_params nwhole_params;
	struct no_writehole_execution_plan nwhole_exec_plan;

	struct {/* Sync op command manager */
		int n_cmds      /*  : 8*/;      /* # cmds in cur stage */
		int last_cmd    /*  : 8*/;      /* Last cmd of current stage */
	};

	struct /* sl_by_sl */{				/* State machine for single slice or partial (not full blockset) sync. Also used in slice by slice mode */
		u32 start_slice			: 9;	/* The first slice from which to start the sync (within the LOCKSET_SLICES) */
		u32 n_slices			: 9;	/* The number of slices to sync (within the LOCKSET_SLICES) also represents the number of blocks in a single disk */
		u32 slice_by_slice_index: 9;	/* Set to start slice+n_slices and reduced each time either we can sync or set uncorrectable */
		u32 is_sbs_mode			: 1;	/* Are we fixing stuff in slice by slice mode or full blockset mode. Default is full blkset */
		u32 reserved			: 4;
		u32 write_unco_mask;			/* Todo: Support for 512[b] blocks! The mask of slices where we write uncorrectable in slice by slice mode */
		u32 write_fix_mask;				/* Todo: Support for 512[b] blocks! The mask of slices which we fixed in slice by slice mode */
	};

	struct /* rv */ {					/* Output struct of the sync */
		int error;						/* Cumulative error of operation. Todo: Extend to 2 ret values (Did sync succeeed? Was data of all slices fixed?)  */
		bool should_abandon_on_error;	/* Indicates the sync has successfully reached release locks or blockset recovered stage, without an error. If true, and we have an error - we must abandon locks to ensure we leave a stale lock. */
		struct {
			u32 n_4ks_read		 : 8;	// Number of units of 4K's actually read/written to disks
			u32 n_4ks_writ		 : 8;
			u32 other_client_did : 1;	/* Sync succeeded by doing nothing because other client already fixed this blockset */
		} stats;
	};
	struct /* exec_flow */ {			// Inner state machine controlling the execution flow of 'so'
		bool assume_caller_holds_locks;		/* Don't take/nor release locks. Assume caller already holds the needed locks: Todo: Remove */
		bool is_autonomous;					/* Very fast, unthrottled sync, which doesn't need locks to run, nor access disks, undocumented in resubmition thread */
		bool should_send_msg_blckst_recovrd;/* Should client notify TOMA's that this sync blockset has been recovered (Stale lock rec completed) */
		bool should_write_binfo;			/* Does sync has to update binfo in RAMs of servers, before releasing the locks*/
		enum sync_op_stage_e stage;			/* Curent stage we are in */
		struct recovery_sync_stack stack;	/* Support nested syncs: Exmple, stale lock syncs calls read-fail sync */
	};
	nvmeibc_sync_cb_t when_done_cb;		/* Call cb upon completion of the upmost 'so'. Nested 'so's Will use stack return */
	void* when_done_context;			/* Context for the above callback */
	void* user_ptr;						/* Generic way to extend 'so' (c++ inheritance)*/
};
typedef struct recovery_sync_op recovery_sync_op_resources_reuse;
#define _NTSO(n, format, ...) _NT(n, "{@O_DBG_ID}: " format, so->o->dbg_id, ##__VA_ARGS__)
#define _NDSO(n, format, ...) _ND(n, "{@O_DBG_ID}: " format, so->o->dbg_id, ##__VA_ARGS__)

#define get_so_blkset(so_) ((so_)->rlba / ((so_)->r1->slice_size * LOCKSET_SLICES))
#define get_so_ow_seg(so_) ((so_)->locks->ds - (so_)->r1->segments)
#define get_so_fctr(  so_) ((so_)->o->nd->dp.sync_rsrcs.fcntr)

#define PRI_SO_NAME "V%.24s.B0x%0x.OPx%x.Ow%x"
#define PRI_SO_NAME_ARGS(so_) (so_)->o->nd->name, (u32)get_so_blkset(so_), so->o->op, (u32)get_so_ow_seg(so_)

// Uses the slice_size of the relevant raid to calculate total blocks of sync operation (across multiple disks)
#define get_so_blocks(so) ((u64)(so->n_slices*so->r1->slice_size))

/* Initialize sync command (which segment, where and op type) */
#define dp_sync_init_cmd(cmds, i, _ds, _addr, _op) ({\
	(cmds)[i].ds = _ds; \
	(cmds)[i].iocmd->reqs1.disk_address = _addr; /* units of blocks */\
	(cmds)[i].iocmd->reqs1.op = _op; \
	(cmds)[i].iocmd->orig = NVMEIBC_DISK_IO_CMD_ORIG_RECOV; \
})

#define did_caller_of_so_took_this_lock(l)  ((l)->status == NCL_STATUS_DONE)

/*********************** Slice by Slice (SBS) ********************************/
u32  nvmeibc_sync_sl_by_sl_is_get_current_slice_index(const struct recovery_sync_op *so);
bool nvmeibc_sync_sl_by_sl_is_current_slice_destroyed(const struct recovery_sync_op *so);

// SO command infomation
#define so_get_owner_seg(so) (((so)->o->mssa) ? (so)->o->mssa->owner_seg : get_owner_seg_slice_start(so->r1, so->rlba))
#define n_read_cmds(so)  ((so)->o->mssa ? (so)->o->mssa->n_reads : (so)->r1->replicas)	// We can probably use (so)->r1->replicas everytime - check for a while with BUG_ON
#define n_write_cmds(so) ((so)->o->mssa ? (so)->o->mssa->n_writes : (so)->r1->replicas) // Check if journal or data or sync // We can probably use (so)->r1->replicas everytime - check for a while with BUG_ON
#define last_cmd(so)     (n_write_cmds((so))+n_read_cmds((so)))
#define CMD_BYTES		NVMEIBC_SECTOR2BYTE(so->n_slices)

void nvmeibc_sync_prepare_so_for_read( struct recovery_sync_op *so, const roles_bmp_t *read_bmp, const enum sync_op_stage_e next_stage);
void nvmeibc_sync_send_all_read_cmds(  struct recovery_sync_op *so, const roles_bmp_t *read_bmp, const enum sync_op_stage_e next_stage);
void nvmeibc_sync_prepare_so_for_write(struct recovery_sync_op *so, const roles_bmp_t write_bmp, const enum sync_op_stage_e next_stage);
void nvmeibc_sync_send_all_write_cmds( struct recovery_sync_op *so, const roles_bmp_t write_bmp, const enum sync_op_stage_e next_stage);

/************************ API with resubmitter thread ************************/
void nvmeibcbdp_sync_reschedule(struct recovery_sync_op *so);

int nvmeibc_sync_alloc_mem_resources(struct recovery_sync_op* so);

/*****************************************************************************/
/* Initialize cmd i of sync, sans NDB. */
void dp_sync_cmd_init(struct recovery_sync_op *so, int i);
int  dp_sync_cmd_alloc_fill_rldr_only(struct recovery_sync_op *so); // for some flows we need the raid leader part of the command only,

/************ Commiting blockset info of primary owner to all copies **********/
/* Daniel: Todo, this is evolution of dbits (which were the only binfo in R1.
   Rewrite those methods to be generalized well to EC. Now this is a hack on
   dbits framework, which is ugly */

bool dp_sync_has_unknown_dbits(const struct nvmeibc_block_command *rldr, const int num_parities);
bool dp_sync_does_see_clean_ram_dbits(const struct recovery_sync_op *so);
bool dp_sync_common_are_all_binfo_equal(const struct recovery_sync_op *so);
bool dp_sync_common_has_dbits_anywhere( const struct recovery_sync_op *so);

#define mark_blockset_info_written(so)     ({(so)->should_write_binfo = false;})
#define mark_blockset_info_not_written(so) ({(so)->should_write_binfo = true ;})
#define should_blockset_info_commit(so)    ((so)->should_write_binfo)
void dp_sync_write_all_blocksets_info_op(struct recovery_sync_op *so);						// State machine to commit blockset info

/* prepare Blockset recovered gen_cmd for updating Serjio (and possibly) TOMA*/
void nvmeibcbdpec_fill_blockset_recovered_info(struct nvmeibc_block_command *cmd,
		const int timeout, void *ctx, const u64 lock_entry,
		struct recovery_sync_op *so, const u32 jri, const u32 jentry,
		const bool pass2toma);

/**************** Mapping operations to sync functions ************************/
void /*func ptr*/*nvmeibcbdpec_sync_get_fn_by_rtype(enum NVMEIBT_RECOVERY_TYPE);

/******************* API for nesting calling of so ***************************/
#define is_so_nested(so) ((so)->stack.sp > 0)

/* Return to caller state machine (After all data was fixed) */
void nvmeibcbdpec_return_to_caller_sm(struct recovery_sync_op *so);

/* Caller state machine pushes its state before nested 'so' start */
void nvmeibcbdpec_push_sm_to_stack(struct recovery_sync_op *so,
								   void (*ret)(struct recovery_sync_op *));

/* Nested state machine pushes its cleanup function to stack */
void nvmeibcbdpec_push_clean_to_stack(struct recovery_sync_op *so,
								void (*cln)(struct recovery_sync_op *));

bool nvmeibcbd_sync_stack_is_htr_asking_for_extern_sm(const struct recovery_sync_op *so);

/* Debug Stack: Dump it to a preallocated buffer. */
void recovery_sync_stack_to_string(const struct recovery_sync_stack *st, char buf[32]);

/* Must be called in each datapath callback of sync command completion */
static inline int dp_sync_cmd_generic_cb(struct nvmeibc_block_command *cmd)
{
	struct nvmeibc_block_command *c0 = cmd->cmdarr;
	int rv;
	cmd->o_rv = cmd->iocmd->comp.comp_code;			// Note: Works for iocmd, irrelevant for cmd->gen_cmd
	WARN_ON_ONCE((u32)(cmd-c0) >= (u32)c0->ncmds); /* Not: c0 <= cmd < ncmds */
	/* WARNING: Do the dec only after we update status of cmd and all OTHER
	   thats other commands may access! */
	rv = nvmeibc_atomic_dec_return(&c0->n_uncompleted_cmds);
	WARN_ON(rv < 0);
	#if defined(DEBUG_TRANSFERS) && defined(DEBUG_TRANSFERS_DETECT_FAIL_TO_UNMAP)
	if (rv == 0) {
		int i;
		for (i = 0; i < c0->nraid_siblings; i++) {
			if (c0[i].iocmd && c0[i].iocmd->disk_cmd.in_flight) {
				_NE_dmesg(t_0p_dp_dbg_tools, "cmd %px iocmd %px - still in flight!", &c0[i], c0[i].iocmd);
				BUG();
			}
			#ifdef DEBUG_TRANSFERS_CHECK_NDB_MAPPED
			if (c0[i].iocmd && c0[i].iocmd->disk_cmd.cmd_type == NVMEIBC_DISK_CMD_IO && c0[i].iocmd->reqs->ndb_mapped) {
				_NE_dmesg(t_0q_dp_dbg_tools, "cmd %px iocmd %px - still mapped!", &c0[i], c0[i].iocmd);
				BUG();
			}
			#endif
		}
	}
	#endif
	return rv;
}

static inline void nvmeibc_sync_set_uncompleted_cmds(struct recovery_sync_op *so, int n_cmds)
{
	nvmeibc_atomic_set(&so->cmds->n_uncompleted_cmds, n_cmds);
}

// Set do_not_send to false for bitmap and true for inverse
static inline void nvmeibc_sync_set_cmds_do_not_send_by_bmp(struct recovery_sync_op *so, int starting_cmd, int last_cmd, const roles_bmp_t send_bmp)
{
	int i, bit;
	for (i = starting_cmd, bit = 1; i <= last_cmd; i++, bit <<= 1) {
		struct nvmeibc_block_command *c = &so->cmds[i];
		c->do_not_send = !(send_bmp & bit);
		DEBUG_TRANSFERS_init_cb_counter(c);
	}
}

// Only sets do not send for bitmap
static inline void nvmeibc_sync_set_cmds_only_do_not_send_by_bmp(struct recovery_sync_op *so, int starting_cmd, int last_cmd, const roles_bmp_t do_not_send)
{
	int i, bit;
	for (i = starting_cmd, bit = 1; i <= last_cmd; i++, bit <<= 1) {
		struct nvmeibc_block_command *c = &so->cmds[i];
		if (do_not_send & bit) {
			c->do_not_send = true;
		}
	}
}

/* Remove rv and comp_code of do_not_send cmds of current stage. */
static inline void nvmeibc_erase_rv_and_comp_codes_of_cur_stage_cmds(struct recovery_sync_op *so, bool do_not_send_vals_only) {
	struct nvmeibc_block_command *rldr = so->cmds;
	int i, last_cmd  = so->last_cmd;
	for (i = (last_cmd + 1 - so->n_cmds); (i <= last_cmd); i++) {
		if (!do_not_send_vals_only || rldr[i].do_not_send) {
			__cmd_set_comp_err(&rldr[i], 0);
		}
	}
}

static inline void nvmeibc_restore_read_cmds_do_not_send_vals(struct recovery_sync_op *so) {
	const int slice_start = so_get_owner_seg(so);
	const roles_bmp_t send_bmp = nvmeibc_raid1_get_roles_bmp(so->r1, slice_start, readable);
	if (__is_raid1_ec(so))
		nvmeibc_sync_set_cmds_do_not_send_by_bmp(so, 0, n_read_cmds(so) - 1, send_bmp);
}

/* Fixup original IO 'pre' and Caller SO 'pre' with our 'post' */
void nvmeibcbdpec_inject_binfo_back_to_caller(struct recovery_sync_op *so);

#endif // NVMEIBC_BLOCK_DP_SYNC_COMMON_H
