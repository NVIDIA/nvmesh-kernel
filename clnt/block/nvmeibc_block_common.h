/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_COMMON_H
#define NVMEIBC_BLOCK_COMMON_H
/*
 * Basic compilation flags and utilities for block device and data services layers
 */
#include "kr_incs.h"
#include "flog.h"
#include "nvmeib_volume_type.h"
#include "../nvmeibc_block.h"		/* external API of the block */
#include "nvmeibc_block_api_os.h"
#include "nvmeibc_topology.h"
#include "nvmeibc_common.h"
#include "recovery/nvmeibc_decentralized_unreg.h"
#include "datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "datapath_utils_generic/nvmeibc_block_dp_elevator.h"
#include "controlpath/nvmeibc_b_cp_cpu_masks.h"

struct nvmeibc_api_of_auto_extend {			// API for auto extendable volume. QCow or other simmilar user space app can use it as thin provisioned volume
	bool is_api_enabled;					// By default this API is not active.
	unsigned long allocated_size;			// Stores the actual allocated size of volume (size which OS sees is almost infinity)
	unsigned long max_write_lba;			// Stores the highest address which was written by serial write. When comes close to 'allocated_size' volume should be auto extended by a new chunk
};

// C++ like derrived classes of block-device
#include "derived_apis/nvmeibc_block_api_d_carrier.h"
#include "derived_apis/nvmeibc_block_api_md_carrier.h"

#define HTR_INVALID_COMP_CODE (0xB19B00B5U)

/******************************** Block Device *******************************/
enum nvmeibc_block_status {					// Status of block device during attach/detach, enum and bit field as well
	NCBD_ATTACHING_NO_IO = 0x1, 			// IO was never enable on this volume
	NCBD_ATTACHED        = 0x2, 			// IO were already enabled at some point (may be disabled in future)
	NCBD_DETACHING       = 0x4, 			// Detaching (no new I/O requests), return anything being resubmitted by reducing timer to 100 ms.
	NCBD_DETACHING_UPGRD = 0x8|0x4, // =0xC	// Detaching (no new I/O requests), nvmeiba atom takes control of the block device.
	NCBD_PREEMPTED		 = 0x10,
};
#define nvmeibc_block_status_is_detaching(st) (!!((st)&NCBD_DETACHING))
#define nvmeibc_block_status_is_preempted(st) (!!((st)&NCBD_PREEMPTED))
const char *nvmeibc_block_status_to_string(enum nvmeibc_block_status);

struct nvmeibc_cinst_params_blk;
struct nvmeib_pet_base_controller;
struct nvmeibc_icore_ops;
struct nvmeibc_block_device {   			// TODO: change this to something shorter
	//struct nvmeibc_bdev_base_class {		// TODO: Base class for many block devices
		struct list_head list_n;				// Structure allowing to link block devices in linked list (for higher level app having a few volumes, possible using the same physical disks)
		ulong size; 							// Size is the size of the device (in blocks)
		struct nvmeibc_os_api *os;				// struct to communicate with kernel (receive IO and control API)
		char uuid[NVMEIBC_BD_UUID_LEN];			// Defined by user, Unique in the universe
		char name[NVMEIBC_BD_NAME_LEN];			// Defined by user, how user sees it (C:\, my_disk, etc), might be not unique
		u32 dbg_id;								// Running number, for tracing. Printed instead of dev. Minor in the case of visible attach, negative on hidden attach
		enum nvmeibc_block_status status;		// Stage in which block device is
		ulong max_retry_jiffies; 				// When OS requests IO, we may wait no longer than X[sec] before returning result. If we could not execute the IO during that time (example: disk got disconnected) we will inform OS that IO failed. min 1 second, max 600.
		const struct nvmeibc_cinst_params_blk *cips;
		enum nvmeibc_config_volume_type type;	// Copy from the field of volume
		const struct nvmeibc_volume *volume;	// Access volume reservation info when initialzing / updating the device
		struct nvmeibc_datapath dp;
		struct nvmeibc_topologies topologies;	// Linked list of topologies. Normally, the block device (volume) will have one topology but it may have a few. Whenever a disk access command or lock is built, it refers to the topology that was used to make	it.	When a new topology arrives, the block device should move to it, but it can't throw out the previous topologies until all remaining cmds and locks generated on it have drained out. So there could be multiple active topologies. They are	stored in a linked list.
		struct nvmeibc_profiler *preparation_profiler; // Profile preparation stage of io
		struct nvmeibc_api_of_auto_extend autoext;	// Support autoextendable functionality
		struct nvmeibc_b_cp_volume_cpu_masks cpu_masks;	// A bitmap of CPU mask indexes in the per-instance nvmeibc_b_cp_cpu_masks
	//};

#ifndef DP_LIB									// Rest of the fields are needed for control path only
	struct topo_stats_t topo_stats;				// Gathers cumulative statistics regarding topologies.
	struct nvmeibc_blk_op_elevator merge_op;	// IO scheduler, unify small io's into a big one to improve throughput
	union {										// Extention of block device to up to 1 of the below inherried classes
		struct nvmeibc_api_of_d_carrier c_d_api;// Used for CARRIER_D_VOLUME.
	};

	// struct { /* Safe reboot/Shutdown/nvmeibc upgrade mechanism */
		struct list_head reboot_ops;			// A queue of reboot operations. The first one is currently executing, the rest are queued.
		bool ignore_all_toma_msgs;				// If true will ignore toma and force toma to brutally unregister this block device
		bool ignore_all_recov_requests;			// Debug only: If true will ignore all toma request for recoveries
	// };
	bool allow_external_io_on_carrier;			// Allow other nvmesh volume to transfer BIO to this volume (treat it as disk). This allows volumes stacking
	struct nvmeibc_trace_stats_scheduling trace_stats;	// Scheduling state of block stats tracing for block watchdog
#endif
	bool ignore_all_recov_toma_speed_req;		// Ignore requests from toma to change recovery speed. Used when manually setting those values
	struct nvmeib_pet_base_controller* io_pet_controller;
};
#define nvmeibc_volume_short_id(nd) ((nd)->dbg_id)
#define assert_dev_on_mainwq(dev) nvmeibc_assert_on_main_wq(nvmeibc_isnt_params_blk2main(nvmeibc_cinst_get_blok_p(dev)))	// Attach/Detach actions must be done serialized on main-wq

static inline int nvmeibc_idisk_get_block_to_disk_sector_shift(struct nvmeibc_idisk const* disk) // Translation beetween volume block size and NVME disk formatted block size. Daniel: Todo, make this per volume
{
	return NVMEIBC_SECTOR_SHIFT - disk->ops.get_sector_shift(disk);
}


/* Volume that did not export to kernel an api of issuing bio, can be upgraded
   using this function (to enable bio). It also creates /dev entry
   sets max rw bytes according to slice_size*transport limit */
int nvmeibc_block_upgrade_os_to_ioable(struct nvmeibc_block_device *dev, const int slice_size, u64 header_size);

/* Reason 'D' - detach, 'U' - detach for nvmeibc upgrade, 'A' - attach,
   'I' - Bio & Syncs enabled, 'S' - only sync enabled, 'P' - preempted by other client. Updates the status
   field and max_retry_jiffies fields. Returns true if block device has to be
   revalidated by the OS */
bool nvmeibc_block_update_status(struct nvmeibc_block_device* dev, char reason);

#define NVMEIBC_NUM_PREV_OWNERS 8

struct nvmeibc_topo_percpu_entry {
	struct nvmeibc_cmd_lock *prev_owner;	// Last owner lock of the last IO issued on this topo (which can potentially be transferred, to next lockset in the future)
	int counter;							// Once every 256 IO's do not allow a lock to be transfered.
	unsigned long jiffies1;
};

struct nvmeibc_topo_percpu {
	volatile int t_users; 					// # entities currently using topology
	spinlock_t lock;						// Protects the below fields
	struct nvmeibc_topo_percpu_entry array[NVMEIBC_NUM_PREV_OWNERS];	/* Optimization: Locks elevator for sequential writes. Reduces contention on subsequent operations to the same blockset */
} ____cacheline_aligned;

static inline struct nvmeibc_block_device *
nvmeibc_block_nt_to_b(struct nvmeibc_topologies *nt)
{
	return container_of(nt, struct nvmeibc_block_device, topologies);
}

static inline struct nvmeibc_block_device *
nvmeibc_block_t_to_b(const struct nvmeibc_topology *t)
{
	return nvmeibc_block_nt_to_b(t->nt);
}

#include "main/utils/nvmeibc_main_block_gen_work_sched.h"

int nvmeibc_block_send_mgmt_allert(const struct nvmeibc_block_device* dev, char *msg);

/**
 * @brief Suspend the entire block device. All IO's will fail. Will call
 *   on_suspend_finish_cb(susped_context) when last IO finishes. Use revive to
 *   reactivate suspended bdev. Revive is a syncronous call so it does not have
 *   callback
 * @param dev pointer to block device
 */
int nvmeibc_block_suspend(struct nvmeibc_block_device *dev,
				void *susped_context, blk2blk_gen_work_t on_suspend_finish_cb);
int nvmeibc_block_revive( struct nvmeibc_block_device *dev);

int nvmeibc_block_call_for_all_disks(struct nvmeibc_block_device *dev, int (*call_fn)(struct nvmeibc_disk *disk, void *ctx), void *ctx);

/******************************* Error codes *********************************/
#define is_software_error(err) ((err) < 0) /* Software error */
#define is_non_retriable_error(err) ((err) > 0 && (nvmeib_error_code_has_dnr(err) || /* ! Hardware error */ \
				  ((err) == EPERM_READ_FAIL) || ((err) == EPERM_WRITE_FAIL)))
#define is_transient_disk_error(err) (is_software_error(err) ||  \
				(!(nvmeib_error_code_has_dnr(err) || /* ! Hardware error */ \
				  ((err) == EPERM_READ_FAIL) || ((err) == EPERM_WRITE_FAIL))))
#define is_readfail_error(err) (!is_software_error(err) && (nvmeib_error_code_has_dnr(err)||(err & EPERM_READ_FAIL)) // Includes any positive err where EPERM_READ_FAIL is on (can be with or without DNR bit)
/********************** Protection Raid persistency ***************************/
#include "block/controlpath/nvmeibc_b_cp_lost_srv_resources.h"

/* Struct which stores all configuration properties of protection raid, not
   copied with topologies */
struct nvmeibc_raid_topo_persistent {
	atomic_t refcount;				// How many copies of topoloy use this raid
	struct stale_lock_resolver_t slr;
	struct nvmeibc_recovery* recoveries[NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES]; /*holds information about currently processed recovery*/
	struct nvmeibc_b_cp_loser loser;	// Info about server lost resources on this praid
	struct nvmeibc_elect_chunk_cpr *cpr;	// Only in WCV, on first raid of each chunk
	struct nvmeibc_profiler *sync_profile; // Replaces sync_profile from earlier version
	struct nvmeibc_profiler *good_path_profile[VERB_RW_NUM]; //we may even rename it to good_data_path_profiler, and add locks_profiler to simplify the stages definition
	struct nvmeibc_clients_cut_mgr {		// Manager of procees of cutting other clients from this praid
		struct list_head jobs_list;		// List of clients that are under cutting proccess, guarded by the nvmeibc_topologies lock
	} ccut_mgr;
};

static inline struct nvmeibc_profiler *nvmeibc_get_raid_good_path_profile_for_rwt_op(struct nvmeibc_disk_segment *ds, enum nvmeib_block_io_op op)
{
	const unsigned verb = good_path_profiler_io_op_to_rwt_verb(op);
	return nvmeibc_disk_segment_get_praid(ds)->hdr->good_path_profile[verb];
}

struct nvmeibc_raid_topo_persistent* nvmeibc_raid_topo_persistent_create(void);
//bdev->dp path variable is initialized after topology (thus all raid1 instances already created), so we need second phase
void nvmeibc_raid_topo_persistent_init_profilers(struct nvmeibc_raid_topo_persistent* self, struct nvmeibc_block_device* bdev, const int chunk, const int raid);
void nvmeibc_raid_topo_persistent_clear_profilers(struct nvmeibc_raid_topo_persistent* self);
void nvmeibc_raid_topo_persistent_clear_preparation_profiler(struct nvmeibc_block_device* bdev);
void nvmeibc_seg_topo_persistent_init_profilers(struct nvmeibc_disk_segment *seg, struct nvmeibc_block_device* bdev, const int chunk, const int raid, const int segment);
void nvmeibc_seg_topo_persistent_clear_profilers(struct nvmeibc_disk_segment *seg);
void nvmeibc_raid_topo_persistent_put_ref(struct nvmeibc_raid_topo_persistent*);
void nvmeibc_raid_topo_persistent_add_ref(struct nvmeibc_raid_topo_persistent*);
struct stale_lock_resolver_t * nvmeibc_raid_topo_persistent_get_slr(struct nvmeibc_raid_topo_persistent*);

/************************** Toma messaging API *******************************/
/* Each segment in a topology is NORMAL and can send/recive toma messages. When
 * it is not needed anymore we may not be able to just remove it due to incomming toma messages.
 * Typical life of segment: kzalloc()->subscribe_tr->NORMAL->DEAD->segment free()->unsubscribe_tr */
enum nvmeibc_subscription_status {
	NVMEIBC_SUBSCRIPTION_STATUS_NORMAL 		= 0,		// Normal segment, value of kzalloc()
	NVMEIBC_SUBSCRIPTION_STATUS_DEAD		= 2,		// Segment is getting free(). Can't send/accept toma messages or do anything.
};

/* Struct which stores all configuration properties of the segment.
   1. Dispatches a received toma message to its segment.
   Toma has a unique ID for each segment which using hash table is converted to
   a pointer to the actual segment in the volume.
   2. Stores information of the memory of disk locks (transport layer mem) */
struct nvmeibc_subscription_ctx {
	struct nvmeibc_topologies *nt;				// Block device
	struct nvmeibc_raid_topo_persistent *hdr;	// praid configuration: fast access in datapath
	int ch, r1, seg;		// A unique id of the segment: volume->chunk[ch].raid1s[r1].segment[seg]. Don't use UUID to avoid string search (speed consideration)
	u64 first_lba, length;	// Same fields as of topology segment. Duality for fast IO

#ifndef DP_LIB				// Rest of the fields are needed for control path only
	struct rb_node rb;		// node to be inserted nto hash table
	struct kref use_count;	/* Ref held by each copy of seg + incomming msg */
	u64 handle;				/* Unique ID, key of the hash table  */
	void *mem_handle;		/* Opaque referenence to low level disk-locks */
	enum nvmeibc_subscription_status status;
	spinlock_t death_lock;	  /* Atomic write access to live status of this TR, or read access + taking a drainer ref */
	struct nvmeibc_disk *disk;	// Physical disk on which segment resided
	#ifdef DEBUG_TOPO_CNTRS
		atomic_t n_registers; /* Allert if segment registered twice */
	#endif
	//during upgrade, it is possible to stuck for an undefined period of time
	//in situation, when client was upgraded, but toma is not, so instead of
	//doing analysis what client wanted to send and resend it using previous version of protocol
	//we will keep the version here.
	//on first message it will be downgraded if needed,
	//on "pause" it will be restored to the current
	u32 protocol_version;
	#define handle_of_subscription_ctx(tr) ((tr)->mem_handle)
#else
	#define handle_of_subscription_ctx(tr) (NULL)	/* Not used in DP_LIB */
#endif
};
#define is_toma_reg_valid(tr)			(tr)
#define is_toma_reg_already_dead(tr)	((tr)->status != NVMEIBC_SUBSCRIPTION_STATUS_NORMAL)

#include "block/controlpath/nvmeibc_b_cp_send_toma_msg.h"

/***************************** Debugging & Utils*******************************/
u64 __read_ull_addr(const char** str);			// Parse long unsigned integer, 10 or 16 base. Return ~0ULL on error. Advance string ptr by number of consumed character. Skips prefix

/*************************** Block per instance object ************************/
struct t_block_clnt_globals {
	struct list_head block_devices;			// List of block devices, Add/Remove to list done only from main-wq
	spinlock_t block_devices_sl;			// Lock used only on Write access to list on main-wq and read access from other contexts. Read acces son main-wq does not need the lock
	struct {
		struct task_struct *thread;
		wait_queue_head_t sleep_q;
		int thread_counter;
	} watchdog;
	struct nvmeibc_trace_stats_scheduling metrics_trace_stats;
	struct nvmeibc_os_apis_container *osc;
	struct nvmeibc_b_cp_cpu_masks *cpu_masks;
	const struct nvmeibc_cinst_params_blk *cips;
};
struct t_block_clnt_globals * __get_from_params_blok_globals_container(const struct nvmeibc_cinst_params_blk *p);

#define nvmeibc_cinst_get_blok_p(obj) ((obj)->cips)
#define nvmeibc_cinst_get_blok_g(obj) __get_from_params_blok_globals_container(nvmeibc_cinst_get_blok_p(obj))
#define nvmeibc_cinst_get_blok_m(obj) nvmeibc_isnt_params_blk2main(nvmeibc_cinst_get_blok_p(obj))

int  t_block_clnt_globals_add_bdev(struct nvmeibc_block_device *dev);
void t_block_clnt_globals_del_bdev(struct nvmeibc_block_device *dev);

#endif  // H beginning
