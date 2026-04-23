/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_B_DP_IOSTATS_H
#define NVMEIBC_B_DP_IOSTATS_H

#include "nvmeib_stats.h"
//#include "kr_incs.h"
#include "common/nvmeib_types.h"
#include "nvmeibc_block.h"		/* external API of the block */
#include "utils/nvmeib_jdr/nvmeib_txt.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

/************************** nvmeibcb_dp_io_fail_mgr ***************************/
/* When critical errors occur blockdevice must return IO error to userspace
   (Example: bad sector in Raid-0). However, most of user space apps dont handle
   well IO erros (not every fread()/fwrite() is tested for failure in the code).
   In particulary file systems. So returning IO error might be disastrous
   (corrupting entire file system) even though this is not always NVMesh's fault.
   As a precausion upon returning maximum of N errors, block device will suspend
   itself and not return errors enymore. IO's will stuck blocking the userspace
   app but at least preserving the existing data and preventing further
   corurptions. Only manual action of support team (ioctl) can revive the block
   device. This is typically done after replacing faulti disk / restoring data
   from external backup, etc.
   Note: Each bio failure reported to user is accompanied by WARN_ON().
   Note: Testing environment may want to disable 'mgr' if it intends to destroy
         data */
struct nvmeibcb_dp_io_fail_mgr {	// Todo: Use per bdev/per-disk. instead of global counter
	u32 n_binfo_errors;				// Seriuos error in binfo of primare owner lock. May be a symtom of data corruption
	u32 n_binfo_copy_owner_error;	// Binfo error in copy lock. Less serios, maybe a result of a bug in the code, can be fixed
	u32 n_htr_null_uuids;
	u32 n_blocked_cont_EC_4571;		// Counter how much times this bug appeared
};
void nvmeibcb_dp_io_fail_mgr_init(          struct nvmeibcb_dp_io_fail_mgr *);
void nvmeibcb_dp_io_fail_mgr_disable(       struct nvmeibcb_dp_io_fail_mgr *);				// Dangerous, disable the mechanism of auto suspension. Called by L1 support/Testing environment
int  nvmeibcb_dp_io_fail_mgr_set_limit(     struct nvmeibcb_dp_io_fail_mgr *, int n_ios);	// Set a new limit (and enable mgr if disabled). Returns the previous limit
void nvmeibcb_dp_io_fail_mgr_reset_limit(   struct nvmeibcb_dp_io_fail_mgr *);				// Reset the counter to default limit (relevant only if mgr is enabled). Used by L1 support. Gives bdev a second chance to retry the failed IO's

/* Upon IO failure (disk cmd with critical error code:
   {unhandled positive NVME error or negative internal error}, let manager act
   returns: true - IO should be retried, false - return Error to userspace */
bool nvmeibcb_dp_io_fail_mgr_inspect(const struct nvmeibc_block_command *cmds, int i);
void nvmeibcb_dp_io_fail_mgr_binfo_err(         struct nvmeibcb_dp_io_fail_mgr *);					// Upon IO, encountered wrong binfo on primary owner
void nvmeibcb_dp_io_fail_mgr_binfo_copy_errfix( struct nvmeibcb_dp_io_fail_mgr *);					// Fixed broken binfo on copy owner
void nvmeibcb_dp_io_fail_mgr_htr_null_uuids_err(struct nvmeibcb_dp_io_fail_mgr *, struct nvmeibc_block_device *dev);	// Upon HTR, encountered null uuid
void nvmeibcb_dp_io_fail_mgr_blocked_cont(      struct nvmeibcb_dp_io_fail_mgr *);										// Upon blocked cont bug

enum dp_iostats_names {
	DP_IO_STATS_CRITICAL_FAIL = 0,		// Amount of io's since the start of the report that could not be executed due to critical error (no memory, wrong address, etc)
	DP_IO_STATS_TIMED_OUT,			// Amount of io's since the start of the report that failed due to time out
	DP_IO_STATS_SUSPED_FAIL,		// Amount of io's since the start of the report that auto-failed due to device being suspended/unsafely detached
	DP_IO_STATS_IGNORED_ERR,		// Amount of ignored io's with failure
	DP_IO_STATS_ILLEGAL_TRIMS,		// Amount of disobedient discard operations
	DP_IO_STATS_RESUBMITTED,		// Amount of io's which have been resubmitted and may succeed in future
	DP_IO_STATS_RESUBMITTED_STARTED,	// Amount of io's scheduled for resubmition
	DP_IO_STATS_DNR_BAD_SECTORS,	        // Amount of bad sectors that are reported by hardware.
	DP_IO_STATS_SOFTWARE_READ_FAIL_COUNT,   // Amount of read IO's that failed (retriable errors)
	DP_IO_STATS_MD_MARKED_INVALID_ERRORS,   // Metadata errors, like edic problem etc
        DP_IO_STATS_MD_EDIC_CHECK_ERRORS, // amount of cases where EDIC discrepancy between block edic and the calculated edic.
	DP_IO_STATS_EDIC_ERRORS,        // Amount of EDIC errors, like EDIC not found, etc
	DP_IO_STATS_LOCK_OP_FAILED,     // Num of io failed Lock operations.
	DP_IO_STATS_LOCKSET_FAILED,     // Num of failed lockset wait
	DP_IO_STATS_LOCK_TRANSFER_ACCEPTED_COUNT,
	DP_IO_STATS_LOCK_TRANSFER_REJECTED_COUNT,
	DP_IO_STATS_LOCK_CONTENDED_COUNT,
	DP_IO_STATS_LOCK_STALE_COUNT,
	DP_IO_STATS_WAIT_FOR_LOCKSET_CANCELED_COUNT,
	DP_IO_STATS_SYNC_STALE_TO_DIRTY_COUNT,
	DP_IO_STATS_SYNC_SUCCESS_COUNT,
	DP_IO_STATS_OTHER				// 'o' Other Failed io not following within any of the above groups
};

/************************** t_failed_io_stats *********************************/
/*
 * Structure to collect statistics on failed IO due to various reasons.
 *
 * NOTE: Counter values should not be used directly.
 * Each counter stores its value in bits 2 to 64. The least significant bit (LSB)
 * indicates whether the counter has changed and needs to be reported in the next
 * reporting cycle.
 *
 * */
struct dp_io_stats_cntrs {
	u64 countrs[DP_IO_STATS_OTHER + 1]; // Array of counters, one for each enum dp_iostats_names

	// note: the sync types inside sync_counters are cummulative, meaning that it counts the total number of sync operations from block creation.
	u64 sync_counters[NVMEIB_BLOCK_IO_OP_MAX_SYNC_TYPES]; // Sync counters, one for each sync type in "enum nvmeib_block_io_op"
};

struct dp_io_stats {
	spinlock_t lock; // Protect the counters below. Alternatively can do them all percpu.
	struct dp_io_stats_cntrs n;
	struct nvmeibcb_dp_io_fail_mgr mgr; // Actions that must be taken when IO fails
};

void dp_io_stats_init(		struct dp_io_stats *t);
void dp_io_stats_clear(		struct dp_io_stats *t);
void dp_io_stats_tostring(	const struct dp_io_stats *t, struct nvmeib_txt *txt);
void dp_io_stats_tojson(	const struct dp_io_stats *t, struct jdr *jdr);
													//
u64 dp_io_stats_get_counter(const struct dp_io_stats *t, enum dp_iostats_names name);
void dp_io_stats_clear_counter(struct dp_io_stats *t, enum dp_iostats_names name);

u64 dp_io_stats_get_sync_counter(const struct dp_io_stats *t, enum nvmeib_block_io_op sync_type);
													//
/* output all traces of a volume to metrics channel*/
void dp_io_stats_trace(u32 blk_dev_id, struct dp_io_stats *t, bool changed_only);

void dp_io_stats_add(struct dp_io_stats *t, u64 *counter, int count);

#define IO_STATS_ADD(t, counter, count)                                              \
	do {                                                                         \
		if ((counter) <= DP_IO_STATS_OTHER)                                  \
			dp_io_stats_add((t), &((t)->n.countrs[(counter)]), (count)); \
	} while (0)

#define IO_STATS_INCR(t, counter) IO_STATS_ADD((t), (counter), 1)
#define IO_STATS_DEC(t, counter) IO_STATS_ADD((t), (counter), -1)

#define IO_STATS_SYNC_ADD(t, sync_type, count)                                                          \
	do {                                                                                            \
		int sync_id = SYNC_TYPE_ID(sync_type); \
		if (sync_id >= 0 && sync_id < NVMEIB_BLOCK_IO_OP_MAX_SYNC_TYPES)				     \
			dp_io_stats_add((t), &((t)->n.sync_counters[sync_id]), (count)); \
	} while (0)

#define IO_STATS_SYNC_INCR(t, sync_type) IO_STATS_SYNC_ADD((t), (sync_type), 1)
#define IO_STATS_SYNC_DEC(t, sync_type) IO_STATS_SYNC_ADD((t), (sync_type), -1)

#endif  // ifdef NVMEIBC_B_DP_IOSTATS_H

