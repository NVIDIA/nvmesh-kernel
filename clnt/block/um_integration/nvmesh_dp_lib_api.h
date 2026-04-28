/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_LIB_API_H
#define NVMEIBC_DP_LIB_API_H

#include "kr_incs.h"
#include "block/recovery/nvmeibc_block_dp_sync_api_manager.h"			// Sync statistics
#include "block/datapath_utils_generic/nvmeibc_block_dp_lock_server.h"	// Topology locking mechanism
#include "clnt/block/platform_services.h"

#define SYMBOL_EXPORT __attribute__((__visibility__("default")))

/* Library is designed to encapsulate datapath, and decouple it from control path (mgmt / toma),
	and other APIs/Components (os_api, throttling mechanisms, kernel interrupts, timers).
	Datapath includes:
		1. all blockset fixup logic (syncs)
		2. Read/Write/Trim IO on protection raid.
	Library can serve multiple block devices, and launch multiple syncs/IO in parallel
*/
struct nvmeibc_disk;
struct nvmeibc_d_rdma_comp;
struct nvmeibc_disk_io_command;
struct nvmeibc_disk_gen_cmd;
struct stale_lock_resolver_t;
struct nvmeibc_disk_jmdc_read_comp;
struct nvmeibt_client_recovery_status_pl;
struct lib_call_api_recov;
struct dp_io_stats;

/* Below is virtual table of transport layer functions
	Markers:
		ACB - Function might asyncronous. Upon finish calls dc->callback()
		SYN - Fucntion must be syncronous
		OPT - Optional. Not mandatory to implement, Unless specifically mark as this, a function is manadatory
		CMD - Asyncronous function, upon completion calls specific predefined callback. See kernel code of the callback function for more info
*/
struct nvmesh_dp_lib_virtual_table {	// Virtual disk-IO / rdma / etc functions which implementent transport layer on top of which the library runs
	void (*run_cmpxchg)(    struct nvmeibc_disk* disk, u64 address, struct nvmeibc_d_rdma_comp* dc);	// ACB
	void (*run_viewlock)(   struct nvmeibc_disk* disk, u64 address, struct nvmeibc_d_rdma_comp* dc);	// ACB
	void (*run_write_binfo)(struct nvmeibc_disk* disk, u64 address, struct nvmeibc_d_rdma_comp* dc);	// ACB
	void (*run_io_blocks)(  struct nvmeibc_disk* disk, struct nvmeibc_disk_io_command* dcmd);			// CMD, completion calls: void nvmeibc_block_completion(dcmd->comp).
	int  (*run_gen)(        struct nvmeibc_disk* disk, struct nvmeibc_disk_gen_cmd* cmd);				// CMD, completion calls: void nvmeibc_disk_gen_cmd_completion(...).

	int (*get_slr_status)(         struct stale_lock_resolver_t *slr, u32 lock_id);						// SYN, Query if safe to take over stale lock. rv = 0, means safe, any other error means unsafe and will abort library sync/io
	int (*get_slr_cuuid_by_lockid)(struct stale_lock_resolver_t *slr, u32 lock_id, uuid_be *cuuid); 	// SYN, Query recoveree clients uuid from stale lock value. See rv exmplanation above

	uuid_be *(*get_cuuid)(void); 																		// SYN, Get this client's UUID

	int (*run_printk)(const char *fmt, va_list args);													// SYN, implements printk textual prints (non binary)

	struct { // Recovery related functions 3 Mandatory, 1 optional. Initialize them only if you want to use recovery, otherwise set all to NULL
		void (*get_problems  )(struct nvmeibc_disk *disk, u64 dlba_start, u64 blocksets_length, struct nvmeibc_d_rdma_comp *dc);	// ACB, Get dbits/stale locks problem from server.
		void (*read_jmdc     )(struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp *dc);									// ACB, Read JMDC for cold recovery.
		void (*reply_for_toma)(enum NVMEIBT_CLIENT_MSG_TYPES msg_type, struct nvmeibt_client_recovery_status_pl *pl);				// SYN, Send msg recov progress/finish with its payload. Used for msgs with prefix NVMEIBT_CLIENT_MSG_RT_RECOVER...
		void (*yield_after_1_sync)(struct lib_call_api_recov*);		// SYN, OPT, After 1 sync, recovery cooperatively call this callback. Inside you can stop recovery upon detach, ping it, set counters, do throttling (yields), etc...
	} recov;
};

// Constructor / destructor of the library.
void SYMBOL_EXPORT nvmesh_dp_lib_create(struct nvmesh_dp_lib_virtual_table vt);			// Call me once before issuing first sync and after last sync.
void SYMBOL_EXPORT nvmesh_dp_lib_destroy(void);			// Call me once after last sync, or else memory will be leaked.

/*****************************************************************************/
// Generic parameters for Blockset fixup (sync) and IO
struct lib_call_api_params_generic {				// Parameters to launch sync/io operation
	struct bdev_params_t {							// Taken from configuration
		u32 binje;
		bool enable_crc_check;
		bool use_debug_di;
	} bdev;
	struct praid_params_t {
		u32 topology_id;							// Topology serial number, used for tracking topology advertisement
		int praid_version;							// Last applied TOMA pRAID version
		u64 conf_version;							// The last version of the configuration applied to this topology
		u32 n_segments;								// D+P of protection raid
		u32 slice_size;								// D   of protection raid
		union nvmeib_lock_id lid;					// Lock id caller is registered with. Sync/IO acquire locks using this lock-id
		struct stale_lock_resolver_t *slr;			// Stale lock resolver, sync/io will ask if safe to access stale lock / what is recoveree uuid

		struct seg_params_t {						// Descrbies topology of current protection raid
			struct nvmeibc_disk *disk;				// Obscure pointer to transport disk
			u64 dlba_start, dlba_length;			// Units of blocks
			struct lock_ownership_map lmap;			// Describes how to acquire locks/where binfo resides, etc
			enum NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING sync_safety;
			enum NVMEIBTC_DS_MODE toma_acm;
			char uuid[NVMEIB_GID_STR_MAX];
		} segs[N_MAX_RAID_SLICE_LEN];				// Note: Only first 'n_segments' elements should be initialized
		struct locks_params_t {						// Sorted according to topo (lmap) + rlba. Memset to 0. Optimization: Fill only if at least 1 lock is taken.
			bool is_already_taken;					// Is this lock already held by caller. Sync/IO will not touch it. For IO: all relevant locks are taken or none. For sync, subset of locks may be taken
			union nvmeib_blkset_info binfo;			// If 'is_already_taken==true', fill binfo that was read when acquiring lock, otherwise irrelevant (put 0)
		} locks[N_MAX_RAID_LOCKS];					// Starting from owner. Actual amount of locks is defined by lmap/topology. Caller initializes this truct from its locks
		union nvmeib_lock_id offending_stale_lock;	// For Sync: If not all locks were taken by caller due to stale lock that was encountered, put it here, otherwise put zero (meaningless)
	} pr;
	enum nvmeib_block_io_op op;						// Which IO/Sync you want to launch. Note: For IO this should match 'bio->bi_rw', For recovery use NVMEIB_BLOCK_IO_OP_NOP
	u32 dbg_id;										// Will use this dbg_id to print all operation/state machine logs. Makes it easier to debug callers action.

	struct dp_platform_services services; // provides access to the UM platform services
};

// Generic execution counters / statisitcs
struct lib_op_stats_t {							// Various statistics structs which can be updated by Sync / IO
	struct nvmeibc_sync_stats*    sync;			// If not NULL, sync will update this struct (increase relevant counters)
	struct nvmeibc_flow_counters* fctr;			// If not NULL, sync will update this struct (increase relevant counters)
	struct dp_io_stats*           dp_io;		// If not NULL, library copies dp_io_stats here at op finish
};												// Note: IO op may encounter a problem and launch sync which may update the counters

// Generic output result. Represents a sync op, because IO op can call a sync op as well. In case of IO that called a sync, the final result will be represented here
struct output_generic_t {
	int error;									// 0 - means OK, negative is IO transport error / Sync error, positive is nvme error
	union nvmeib_blkset_info binfo;				// Fixed binfo after the IO / Sync runs, example: resolved unknown dbits, wraparound txid, turned of dbit, etc. Always filled
	struct lib_error_description_t {			// Always accompanied with error != 0. More information about the error. Irrelevant when error == 0
		struct {								// Description of which slices were destroyed/fixed, bit per slice. Describes the entire blockset, even if sync request was for subset of slices
			u32 destroyed;						// Bitmap of destroyed slices [0..31].
			u32 fixed;							// Bitmap of fixed     slices [0..31].
		} slice_masks;
		bool must_topo_rereg;					// Sync/IO made even more mess and abandoned locks. Topology should be reregistered as soon as possible
		bool not_even_started;					// IO/Sync could not start.
		bool should_suspend_bdev;				// Sync / IO encountered a data corruption. Requests to immediately stop IO to bdev and investigate
		bool binfo_corrupted;					// Blockset info corrupted. Possibly bug in sync code. May be with/without 'should_suspend_bdev' flag
		bool htr_null_uuid;						// Sync cannot finde recoveree journal to fix the blockset. Impossible to fix blockset. May be with/without 'should_suspend_bdev' flag
		bool was_canceled_by_caller;			// IO / Sync was canceled by caller so it could not properly finish. In rare cases this field can be true, while error == 0 (cancel requrest arrived so late, that Sync/IO already finished)
	} err_info;
	bool sync_was_autonomous;					// Sync was executed as autonomous (executed without holding any lock). IO may call such a sync
	bool please_reread_data;					// R1 - Sync fixed some data. If caller, did read something before sync, it should re-read this, as data may have changed. Irrelevant for IO
	bool is_binfo_field_valid;					// Was 'binfo' field set with real data? In case of error or success of READ IO, it may be unavailable
};

struct __internal_run_time_usage_dont_touch_t {	// Mechanism to reference execution context from parameters struct.
	void *cancellation_context;
};

/*****************************************************************************/
struct lib_call_api_sync {							// Parameters to launch sync
	// ----------------------------- Input const Params (not changed by the library)
	struct lib_call_api_params_generic gen_params;	// Generic parameters
	// ----------------------------- Output: Generic format
	struct __internal_run_time_usage_dont_touch_t exec;
	struct lib_op_stats_t stats;					// Various statistics structs which can be updated
	struct output_generic_t out;					// Error code with explanations
	// ----------------------------- Input const Params (not changed by the library)
	struct lib_call_api_params_sync {				// Sync op specific params
		u64 rlba;									// Rlba of blockset start in units of blocks (Divisable by slice_size * num slices in blockset)
		u16 start_slice, n_slices;					// Optional: can request sub range of slices within blockset. Used fo fixing stale locks
	} sync_params;
};

void SYMBOL_EXPORT nvmesh_dp_lib_do_sync_op(   struct lib_call_api_sync*);	// Function returns void. Always look at out.error
void SYMBOL_EXPORT nvmesh_dp_lib_do_sync_abort(struct lib_call_api_sync*);	// Function returns void. Always look at out.error

/*************************** IO to 1 blockset ********************************/
struct lib_call_api_io {							// Parameters to launch sync
	// ----------------------------- Input const Params (not changed by the library)
	struct lib_call_api_params_generic gen_params;	// Generic parameters
	// ----------------------------- Output: Generic format
	struct __internal_run_time_usage_dont_touch_t exec;
	struct lib_op_stats_t stats;					// Various statistics structs which can be updated
	struct output_generic_t out;					// Error code with explanations
	// ----------------------------- Input const Params (not changed by the library)
	struct lib_call_api_params_io {					// IO specific param
		struct bio *bio;							// Pointer to RW bio. Put RLBA*8 in 'bi_sector'. No bio_vec for Discard/Trim/NVMEIB_BLOCK_IO_OP_WRITE_UNCOR
		bool enable_local_read_optimization;		// R1 only: Allow reading from local disk instead of primary owner
	} io_params;
};

void SYMBOL_EXPORT nvmesh_dp_lib_do_rwt_op(   struct lib_call_api_io*);	// Read/Write/Trim Function returns void. Always look at out.error
void SYMBOL_EXPORT nvmesh_dp_lib_do_rwt_abort(struct lib_call_api_io*);	// Optional: Abort as fast as possible. Up to caller to prevent race condition

/*************************** Trim to multiple blocksets ********************************/
void SYMBOL_EXPORT nvmesh_dp_lib_do_trim_op( struct lib_call_api_io*);	// Not supported yet

/*************************** Recovery of praid range ********************************/
struct lib_call_api_recov {							// Parameters to launch sync
	// ----------------------------- Input const Params (not changed by the library)
	struct lib_call_api_params_generic gen_params;	// Generic parameters
	// ----------------------------- Output: Generic format
	struct __internal_run_time_usage_dont_touch_t exec;
	struct lib_op_stats_t stats;					// Various statistics structs which can be updated
	struct output_generic_t out;					// Error code with explanations
	// ----------------------------- Input const Params (not changed by the library)
	struct lib_call_api_params_recov {				// Recovery specific params, based on struct nvmeibt_client_recovery_start_pl
		u64 id;										// Recovery task identifier (typically given by toma). Used for prints and pinging running recovery task by toma using this unique key. Must be unique for each call!
		u8 type;									// == enum NVMEIBT_RECOVERY_TYPE
		u8 effort_percents;							// 1-100. Client should do the rebuild with this relative speed. 100% is max, 1% is slowest. 0 - means field is irrelevant
		u8 seg_id;									// Segment index in praid on which this recoveyr is launched (it is Toma owner of the task)
		bool is_mandatory;							// =1, client must successfully clean every required blockset and retry failed sync until success
		bool do_only_owners;						// =1, Process only blockset that the segment written in the message is their owner (barries the primary lock), =0,
		u64 start_lock;								// Start of blkset locks range for recovery. Units: RLBA blocksets. 0 for raid start
		u64 num_locks;								// Number of blkset locks for recovery. -1ULL for ALL
		u32 surviving_ram_bmp_cold_recov;			// Cold recovery only: bitmap for segments, which segs maintained RAM and which lost their RAM. if all lost their ram then this is true cold recovery of 100% of praid
	} recov_params;
};

void SYMBOL_EXPORT nvmesh_dp_lib_do_recovery_op(   struct lib_call_api_recov*);	// Launch any recovery task on Praid. Blocking function, returns when recovery terminates. Always check out.error
void SYMBOL_EXPORT nvmesh_dp_lib_do_recovery_abort(struct lib_call_api_recov*);	// Optional: Abort as fast as possible. Up to caller to prevent race condition, Call upon detach / Instruction from Toma. Can call from virtual func on sync completion
void SYMBOL_EXPORT nvmesh_dp_lib_do_recovery_ping( struct lib_call_api_recov*);	// Optional: Ping recovery (ask it to send status to toma) and optionally update 'effort_percents'. Up to caller to prevent race condition, Call upon Instruction from Toma.

#endif // .h end
