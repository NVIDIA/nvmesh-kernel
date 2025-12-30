#include "nvmeibt_common.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_disk_metadata.h"
#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_ds_metadata.h"
#include "clnt/nvmeibt_client_protocol.h"
#include "./interfaces/srvr/nvmeibt_srvr_proc.h"
#include "local/ram/nvmeibt_ds_blkset_entries.h"
#include "nvmeibt_register.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_topo_bin.h"
#include "nvmeibt_kafka.h"
#include "../common/kr_version.h"

/******************************************************************************/

#define SCRUB_PERIODIC_TIMEOUT_SEC		1
#define SCRUB_PERIODIC_RESCAN_SEC		10
#define SCRUB_SECS_IN_DAY				86400	// used for debugging, set it to 60 in order to config the scrub period in minutes
#define SCRUB_RANDOMNESS_SEC			(SCRUBBING_DEFAULT_PERIOD_DAYS_DEFAULT * SCRUB_SECS_IN_DAY / 1024)
#define SCRUB_PRIORITY_SCALE_FACTOR		(1LL << 16)

#define STALE_BLKSET_FMT "(blkset=@LLX, seg=@UUID_8, lockid=@T_LID)"
#define NTODO_N_REPLICA_SUPPORT(name, fmt, x...) N_Tf(name, "TODO_N_REPLICA - " fmt, x)

static int64_t	recovery_client_batch_n_blksets;	// In the client: max_batch_size
static bool is_stale_rebuild_enabled = true;
static bool test_zeroing_fail = false;

int64_t nvmeibt_recovery_n_blksets_per_scrub_iteration = SCRUBBING_N_BLKSETS_PER_ITERATION_DEFAULT;

static int  scrub_default_period_sec = (SCRUBBING_DEFAULT_PERIOD_DAYS_DEFAULT * SCRUB_SECS_IN_DAY);
static bool is_scrub_enabled = SCRUBBING_DEFAULT;
static nvmeib_heap_t          next_scrub_timeout_heap;	// Holds all the local seg_actives. They only move around inside the heap
														// The minimum is taken at O(1), and only this one is
														//  a candidate for scrubbing

static inline struct nvmeibt_disk_segment_topo_ctx *get_applied_seg_topo(struct nvmeibt_seg_active *seg_active)
{
	return &(nvmeibt_seg_active_get_applied_seg_lot(seg_active)->seg_topo);
}

static inline bool nvmeibt_seg_active_is_EC(struct nvmeibt_seg_active *seg_active)
{
	return nvmeibt_praid_is_type_EC(nvmeibt_seg_active_get_praid(seg_active));
}

union nvmeib_lock_blkset_entry *nvmeibt_seg_active_get_locks_tbl_ptr(struct nvmeibt_seg_active *seg_active)
{
	union nvmeib_lock_blkset_entry	*locks_tbl_ptr = NULL;
	unsigned long long				blkset_s;
	struct nvmeibt_local_disk		*local_disk = nvmeibt_seg_active_get_local_disk(seg_active);

	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		goto out;
	}
	if (!(local_disk->mmap_disk_locks_tbl.addr)) {
		N_Ef(hgskjh7, "disk=@STR locks table is NOT mmapped", nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	blkset_s = nvmeibt_seg_active_get_blkset_s(seg_active);
	if (blkset_s != -1ULL) {
		locks_tbl_ptr = (union nvmeib_lock_blkset_entry	*)local_disk->mmap_disk_locks_tbl.addr + blkset_s;
	}
out:
	return locks_tbl_ptr;
}

/******************************************************************************/

void nvmeibt_seg_active_inc_n_active_registrants_on_applied_praid(struct nvmeibt_seg_active *seg_active)
{
	seg_active->n_active_registrants_on_active_praid_version++;
}

bool nvmeibt_seg_active_final_free_if_not_in_use(struct nvmeibt_seg_active *seg_active)
{
	bool							is_freed;

	if (seg_active->ref_count) {
		is_freed = false;
	} else {
		N_Tf(gy2555s, "seg_active=@UUID_8 final free", nvmeibt_seg_active_UUID_8(seg_active));
		NNVMEIBT_BM_FREE(fhyy734, seg_active->persistent_metadata);
		if (pthread_mutex_destroy(&seg_active->stale_locks_hash_mutex))
			N_Ef(ry876bq, "Failed to destroy stale locks mutex (@AUTO_ERRNO)");
		NNVMEIBT_TOMA_FREE(kkooe42, seg_active);
		is_freed = true;
	}
	return is_freed;
}

void nvmeibt_seg_active_free_mem_and_processes(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_registrant_ctx				*reg_ctx;
	struct stale_lock_ctx						*stale_lock;
	struct nvmeibt_seg_active_awaited_lockid	*awaited_lockid;
	struct nvmeibt_wq_entry						*wq_entry;
	struct nvmeibt_local_disk					*local_disk;
	struct nvmeibt_disk_segment					*seg;

	NFIN;
	if (!seg_active) {
		N_Tf(trace_seg_active_nvmeibt_seg_active_free, "seg_active=NULL");
		goto out;
	}
	N_Tf(rdvsyw6, "Deleting seg_active=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));

	if (seg_active->my_next_scrub_timeout_heap_element.idx_in_heap_arr != ILLEGAL_HEAP_ARR_IDX) { // was added to the heap
		nvmeib_heap_remove_element(&next_scrub_timeout_heap, &(seg_active->my_next_scrub_timeout_heap_element));
		NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(zzdja01, n_pending_scrubbing);
	}

	nvmeibt_seg_active_stop_recovery_tasks(seg_active);

	lock_stale_locks_hash(seg_active);
	XHASHTABLE_FOR_EACH_SAFE(stale_lock, &seg_active->stale_locks_hash) {
		N_Tf(txctahq, "Deleting stale_lock " STALE_BLKSET_FMT, stale_lock->seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active), nvmeib_lockid_purify(stale_lock->reg_ctx->reg_lock_id));
		XDLIST_DEL(&(stale_lock->seg_active_link));
		NNVMEIBT_BM_FREE(bnsj29k, stale_lock);
	}
	unlock_stale_locks_hash(seg_active);
	XHASHTABLE_FOR_EACH_SAFE(reg_ctx, &seg_active->longing_registrants_by_cid) {
		free_reg_ctx(reg_ctx);
	}
	XHASHTABLE_FOR_EACH_SAFE(reg_ctx, &seg_active->active_registrants) {
		free_reg_ctx(reg_ctx);
	}
	XHASHTABLE_FOR_EACH_SAFE(reg_ctx, &seg_active->active_registrants_by_cid) {
		free_reg_ctx(reg_ctx);
	}
	XHASHTABLE_FOR_EACH_SAFE(reg_ctx, &seg_active->stale_registrants) {
		free_reg_ctx(reg_ctx);
	}
	XDLIST_FOREACH_SAFE(reg_ctx, &seg_active->registrants_on_timeout) {
		free_reg_ctx(reg_ctx);
	}
	XDLIST_FOREACH_SAFE(wq_entry, &seg_active->owner_lock_ids_to_release) {
		XDLIST_DEL(&wq_entry->link);
		NNVMEIBT_BM_FREE(bfd8ejv, wq_entry);
	}
	XHASHTABLE_FOR_EACH_SAFE(awaited_lockid, &seg_active->awaited_lockids) {
		XDLIST_DEL(&(awaited_lockid->awaited_lockids_link));
		NNVMEIBT_BM_FREE(nvmeibt_seg_active_free_trace_2, awaited_lockid);
	}
	seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	if (seg) {
		seg->seg_follower.seg_active = NULL;
		seg_active->disk_segment = NULL;
	}
	NVMEIBT_SEG_ACTIVE_CLEAR_ARE_POST_UPDATE_ACTIONS_REQUIRED(ww66723, seg_active);

	local_disk = seg_active->local_disk;
	if (local_disk)
		nvmeib_hash_delete_uuid(local_disk->seg_active_hash_by_uuid, nvmeibt_seg_active_UUID(seg_active));

	nvmeibt_seg_active_clear_dirty_rebuild_required(seg_active);
	nvmeibt_seg_active_clear_stale_rebuild_required(seg_active);
	nvmeibt_seg_active_clear_txid_rebuild_required(seg_active);
	nvmeibt_seg_active_clear_cold_recovery_required(seg_active);
	nvmeibt_seg_active_clear_JGC_rebuild_required(seg_active);

	NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(v20a9lk, seg_active, "LOCAL_DISK", -1);
	nvmeibt_seg_active_final_free_if_not_in_use(seg_active);
out:
	NFOUT;
}

/*
 * Scrubbing:
 *
 * Volumes are periodically read in the background to validate data integrity.
 * Each TOMA scrubs its local segments independently of other TOMAs.
 *
 * The "scrubbing period" (in days) is the desired time to complete a full read
 * cycle. This period is configured per volume, or using a default value if not
 * explicitly set in the configuration. A segment is eligible for scrubbing if
 * its period is non-zero and its last scrubbing has completed more than that
 * period ago.
 *
 * TOMA periodically scans all local segments and marks the eligilbe ones. It
 * also updates eligiliblity upon related configuration changes.
 *
 * TOMA treats scrubbing as another type of recovery task: it picks an eligible
 * task with highest prioirty and launches a scrubbing recovery task, until the
 * max number of simultaneous scrubbing tasks is reached. Priority is determined
 * by the (remaining) period and (remaining) blksets to scan. Scrubbing is
 * only done for healthy praids.
 *
 * Each scrubbing task covers a sub-range of blksets in the segment, but not
 * the whole segment (unlike other rebuild tasks). This gives flexibility to
 * allow progress in other segments psedo-concurrently.
 *
 * When scrubbing of a segment completes, TOMA reports to the management, and
 * thereafter will not consider it again until its scrubbing period passes (if
 * not already).
 *
 * The management is expected to collect all the reports on segment scrubbing
 * completions times and collude them to decide when the scrubbing of a whole
 * volume has completed - and present that to the user.
 *
 * The client is expected to:
 * (a) Read the blksets in the range from all redundant sources and verify that
 * the data is readable from disk, consistent among redundant sources, and has
 * valid EDIC if applicable. For example, in R1 the client will read the data
 * from the both local and other segment and compare them.
 * (b) When scanning a blkset range, only read those blkset owns by its node.
 * For example, in R1 each client will read half of the blksets, from both the
 * local and other segments.
 * (c) The client will reports back like in other rebuilds. In case of error it
 * will indicate the reason for the error (disk IO, data inconsistency, or EDIC
 * error). Persistence of progress (retry) is handled by TOMA.
 *
 * Tunealbe parameters:
 *   max_n_simultaneous_scrubbing:  max simultanneous scrub tasks (default 2)
 *   scrubbing_default_period_days: default when not explicit set (default 30)
 *   nvmeibt_recovery_n_blksets_per_scrub_iteration:	(default 16384)
 *
 * Simulate parameters:
 *   scrubbing enable/disable:		switch scrubbing on/off
 */

static int64_t seg_active_scrub_secs_since_start(const struct nvmeibt_seg_active *seg_active)
{
	struct timespec		now;

	getnstimeofday_boot(&now);
	if (now.tv_sec > seg_active->scrubbing_start_time.tv_sec)
		return (now.tv_sec - seg_active->scrubbing_start_time.tv_sec);
	else
		return 0;
}

static int seg_active_scrub_period_sec(const struct nvmeibt_seg_active *seg_active)
{
	if (!seg_active) {
		N_Ef(ybaumxu, "!seg_active");
		nvmeibt_abort(ES_FATAL); // appease compiler
	}
	return scrub_default_period_sec;
}

static inline int64_t calc_tv_sec_of_next_scrub_iteration(struct nvmeibt_seg_active *seg_active)
{
	int64_t			tv_sec;
	int64_t			random_noise;
	int64_t			time_by_percentage_scrubbed_sec;

	if (seg_active->scrubbing_ctx.tid) {
		tv_sec = INT_MAX;	// Remove it from the top of the heap, so that we can take the next seg
							// When done it will be rescheduled
		goto out;
	}
	// The next invocation should be after we passed the point where it is eligiblae
	//  I.e. the calculated priority should be priority>1
	// So basically calculating start_time + period * percentage_scrubbed
	// Add to this a random(0..SCRUB_RANDOMNESS_SEC)
	time_by_percentage_scrubbed_sec = (uint64_t)seg_active_scrub_period_sec(seg_active) * seg_active->persistent_metadata->n_blksets_scrubbed / num_blksets_in_disk_segment(seg_active->disk_segment);
	if (seg_active_scrub_secs_since_start(seg_active) > time_by_percentage_scrubbed_sec + SCRUB_RANDOMNESS_SEC + 100) {
		N_Wf(4bhjasd, "seg=@UUID_8 should have finished scrubbing @ZU>>@ZU", nvmeibt_seg_active_UUID_8(seg_active),
			 seg_active_scrub_secs_since_start(seg_active), time_by_percentage_scrubbed_sec);
		// ORENL_FIX_THIS: log a warning? report to mgmt?
	}
	tv_sec = seg_active->scrubbing_start_time.tv_sec + time_by_percentage_scrubbed_sec;
	if (tv_sec < seg_active->last_failed_scrub_iteration_time.tv_sec + 60) {
		tv_sec = seg_active->last_failed_scrub_iteration_time.tv_sec + 60;	// If failed, do not retry for 1 minute
	} else {
		random_noise = SCRUB_RANDOMNESS_SEC * random() / (1LL << 31) + 1;
		tv_sec += random_noise;
	}
out:
	return (tv_sec);
}

void upd_scrub_timeout_heap_following_a_change(struct nvmeibt_seg_active *seg_active)
{
	unsigned long		new_timestamp;

	NFIN;
	if (seg_active->my_next_scrub_timeout_heap_element.idx_in_heap_arr != ILLEGAL_HEAP_ARR_IDX) {
		new_timestamp = calc_tv_sec_of_next_scrub_iteration(seg_active);
		if (seg_active->my_next_scrub_timeout_heap_element.timestamp != new_timestamp) {
			// N_Tf(mnenus7, "seg=@UUID_8 new_timestamp=@LX", nvmeibt_seg_active_UUID_8(seg_active), new_timestamp);
			seg_active->my_next_scrub_timeout_heap_element.timestamp = new_timestamp;
			nvmeib_heap_relocate_element(&next_scrub_timeout_heap, &(seg_active->my_next_scrub_timeout_heap_element));
		}
	}
	NFOUT;
}

void nvmeibt_seg_active_scrub_upd_failure_status(struct nvmeibt_seg_active *seg_active, bool is_OK)
{
	if (is_OK) {
		seg_active->last_failed_scrub_iteration_time = TIMESPEC_ZERO;
	} else {
		getnstimeofday_boot(&(seg_active->last_failed_scrub_iteration_time));    // In case of failure, do not retry for a while
	}
	upd_scrub_timeout_heap_following_a_change(seg_active);
}

int nvmeibt_seg_active_global_scrubbing_one_time_init(void)
{
	NFIN;
	memset(&next_scrub_timeout_heap, 0, sizeof(next_scrub_timeout_heap));
	NFOUT;
	return 0;
}

static void seg_active_scrub_reset(struct nvmeibt_seg_active *seg_active, bool is_starting_new_period)
{
	int				time_by_percentage_scrubbed_sec;

	// We just read seg_active->persistent_metadata->n_blksets_scrubbed from persistence (or 0 if seg_active was just created)
	// Now we want to continue where we left of, so we immitate the situation as if we started a while ago
	// in the past by correcting seg_active->scrubbing_start_time.tv_sec
	time_by_percentage_scrubbed_sec = (int)((int64_t)seg_active_scrub_period_sec(seg_active) * seg_active->persistent_metadata->n_blksets_scrubbed / num_blksets_in_disk_segment(seg_active->disk_segment));
	if (is_starting_new_period)
		seg_active->scrubbing_start_time.tv_sec += (int64_t)seg_active_scrub_period_sec(seg_active);
	else
		getnstimeofday_boot(&(seg_active->scrubbing_start_time));
	seg_active->scrubbing_start_time.tv_sec -= time_by_percentage_scrubbed_sec;
	nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 1);
}

void nvmeibt_seg_active_upd_liveliness_according_to_local_disk(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment_topo_ctx		*seg_topo_ctx;
	struct nvmeibt_local_disk					*local_disk;
	struct nvmeibt_disk_segment					*seg;

	NFIN;
	//
	// In any case, we get to this function after parsing a committed(&applied) config (e.g., praid->was_ever_activated)
	// - Either read from the seg persistence
	// - Or from mgmt
	// When reviving a disk, we get here before applying any topo, so the seg_active' dirty_bits_state should be UNKNOWN
	//
	// This function can set the dirty_bits_state to ALIVE/DEAD (or leave as is)
	//
	// - A seg_active comes to life only after its local_disk is fully read and parsed
	// - The state of the seg_active is seg_active->seg_topo->dirty_bits_state
	// - The dirty_bits_state can be one of
	//  - ALIVE_UNSTABLE (if praid was never activated according to config)
	//  - ALIVE_STABLE (if topo was read from the metadata on the local_disk)
	//  - UNKNOWN (if too early)
	//  - DEAD (if the local_disk is known to have an issue)
	//  - Any other state (during the life-cycle)
	//
	// The resulting dirty_bits_state will be used to decide on whether to start raft
	//
	seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	N_Tf(fjsu7xc, "seg=@UUID_8 dirty_bits_state=@DIRTY_BITS_STATE", nvmeibt_seg_active_UUID_8(seg_active), dirty_bits_state_str(seg_topo_ctx->dirty_bits_state));
	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	// If the disk was not read yet, (GPT, seg-metadata, then we cannot make progress)
	if (!nvmeibt_local_disk_is_ready_for_segments(local_disk)) {
		N_Tf(ccccug2, "seg=@UUID_8 disk=@STR !is_ready_for_segments", nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_local_disk_display(local_disk));
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(5gbsdu9, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
		goto out;
	}
	// Added, and revived from local_disk
	// Since inherently, the seg_active might be created before/after
	//  we get its config from mgmt, or we discover it in a GPT, we have
	//  to be able to handle the new information after the creation
	// update metadata header
	if (ARE_UUID_EQ(&(seg_active->persistent_metadata->header.mgmt_db_uuid), &(nvmeib_uuid_null_val))) {
		seg_active->persistent_metadata->header.mgmt_db_uuid = *nvmeibt_global_get_mgmt_DB_uuid();
	}
	// If the seg_active is already revived then we are done
	if (!nvmeibt_disk_segment_is_dirty_bits_state_down(seg_topo_ctx->dirty_bits_state)) {
		N_Tf(fjsu5rb, "seg=@UUID_8 already dirty_bits_state=@DIRTY_BITS_STATE, skipping", nvmeibt_seg_active_UUID_8(seg_active), dirty_bits_state_str(seg_topo_ctx->dirty_bits_state));
		goto out;
	}
	// We already read everything that we could from the disk
	seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	if (!seg) {
		N_Tf(hu87sr4, "seg=@UUID_8 not in config yet, skipping", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;	// The configuration is unknown yet
	}
/* No need for specific was_ever_activated logic here
	if (nvmeibt_praid_lot_is_config_saying_that_was_never_activated(nvmeibt_seg_active_get_applied_praid_lot(seg_active))) {
		N_Tf(fstvewi, "seg=@UUID_8 was_never_activated, resetting", nvmeibt_seg_active_UUID_8(seg_active));
	    nvmeibt_disk_segment_reset_topo_ctx(NULL, NULL, seg_active);
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(va04klw, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE);
		goto out;
	}
*/
/* Excessive code. No need for special logic
	// Was activated in the past already, we revive its active state
	if (!nvmeibt_disk_segment_is_dirty_bits_state_unknown(seg_topo_ctx->dirty_bits_state)) {
		N_Wf(fjsu5rb, "seg=@UUID_8 dirty_bits_state=@DIRTY_BITS_STATE (expected UNKNOWN)", nvmeibt_seg_active_UUID_8(seg_active), dirty_bits_state_str(seg_topo_ctx->dirty_bits_state));
		goto out;
	}
*/
	// All should be good.
	//NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(4bd7ne9, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN);
	//NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(4bd7nb4, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN);
	if (nvmeibt_disk_segment_is_x_zero(&seg->seg_follower.applied_seg_lot.seg_topo)) {
		N_Tf(ncjxsg2, "seg=@UUID_8 is X_ZERO in applied topo, restart zeroing", nvmeibt_seg_UUID_8(seg));
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(hwwu339, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE); // Just to enable update to X_ZERO in the next func
		nvmeibt_seg_active_upd_active_topo_from_applied_topo(seg_active);
		goto out;
	}
	if (seg_active->is_last_shutdown_clean) {
		// Provide the saved persistance as STABLE, the leader will decide whether to use it
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(0cbdgt4, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE);
		//NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(eyr8xi3, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
		//seg_topo_ctx->seg_praid_version_major = persistent_metadata->active_praid_version_major;
		//seg_topo_ctx->seg_praid_version_minor = persistent_metadata->active_praid_version_minor;
		N_Tf(nd83mxe, "seg=@UUID_8 applied_praid_version_major=@PRAID_VERSION applied_praid_version_minor=@PRAID_VERSION",
			 nvmeibt_seg_active_UUID_8(seg_active), seg_topo_ctx->seg_praid_version_major, seg_topo_ctx->seg_praid_version_minor);
	} else if (!nvmeibt_seg_active_is_disk_format_zeroing_done_for_me(seg_active)) {
		N_Tf(ncjd9sg2, "seg=@UUID_8 Awaiting disk zeroing", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	} else {
		N_Tf(mvidjs7, "metadata not yet initialized seg=@UUID_8. Ignore", nvmeibt_seg_active_UUID_8(seg_active));
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(palkysc, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE);
	}
out:
	NFOUT;
}

int nvmeibt_seg_active_upd_metadata_gpt_entry_and_ctrl(struct nvmeibt_seg_active *seg_active, struct nvmeibt_disk_gpt_partition_entry *metadata_gpt_entry,
													   struct nvmeibt_seg_active_metadata_ctrl *metadata_ctrl_fr_persist)
{
	int		rv = -1;

	NFIN;
	seg_active->metadata_gpt_entry = metadata_gpt_entry;
	if (!(seg_active->metadata_gpt_entry)) {
		N_Tf(crwq84n, "seg_active=@UUID_8 metadata_gpt_entry=NULL, skipping", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	if (metadata_ctrl_fr_persist) {
		*(seg_active->persistent_metadata) = *metadata_ctrl_fr_persist;
		if (metadata_ctrl_fr_persist->is_written_on_disk == NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_NOT_ON_DISK) {
			N_Ef(gdta73j, "seg=@UUID_8 found in GPT but metadata was empty/ Leaving initialized as is. Better avoid altogether by writing the metadata before adding to GPT",
				 nvmeibt_seg_active_UUID_8(seg_active));
			goto out;
		}
	} else {
		N_Tf(ksy7cyb, "seg_active=@UUID_8 metadata_ctrl_fr_persist=NULL. Probably just received in config.", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->persistent_metadata->metadata_pbyte_s =	metadata_gpt_entry->pba_s * nvmeibt_local_disk_pblk_size(nvmeibt_seg_active_get_local_disk(seg_active));
		seg_active->persistent_metadata->locks_table_pbyte_s = seg_active->persistent_metadata->metadata_pbyte_s + LOCKS_TABLE_RELATIVE_OFFSET_BYTES;
	}
	if (seg_active->persistent_metadata->metadata_pbyte_s == 0) {
		N_Ef(6dgcmsi, "seg_active=@UUID_8 seg_metadata_pbyte_s=0", nvmeibt_seg_active_UUID_8(seg_active));
		nvmeibt_abort(ES_FATAL);
	}
	if (seg_active->persistent_metadata->locks_table_pbyte_s == 0) {
		N_Ef(fbsi39l, "seg_active=@UUID_8 locks_table_pbyte_s=0", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->persistent_metadata->locks_table_pbyte_s = seg_active->persistent_metadata->metadata_pbyte_s + LOCKS_TABLE_RELATIVE_OFFSET_BYTES;
	}
    // The RM_ver is not part of the topology. take it from the metadata_ctrl
	NVMEIBT_SEG_ACTIVE_SET_COMMITTED_RESERVATION_MODE_VERSION(4hjbwe7, seg_active, seg_active->persistent_metadata->reservation_mode_version);
	// Copy to all reservation_mode fields
	NVMEIBT_SEG_ACTIVE_SET_SUBMITTED_RESERVATION_MODE_VERSION(cnasjkl, seg_active, seg_active->committed_reservation_mode_version);
	NVMEIBT_SEG_ACTIVE_SET_ACTIVE_RESERVATION_MODE_VERSION(mxoiw93, seg_active, seg_active->committed_reservation_mode_version);
	NVMEIBT_SEG_ACTIVE_SET_HIGHEST_RESERVATION_MODE_VERSION(bsus832, seg_active, seg_active->committed_reservation_mode_version);
	seg_active->is_last_shutdown_clean = seg_active->persistent_metadata->is_current_shutdown_clean;
	rv = 0;
out:
	NFOUT;
	return rv;
}

static int regenerate_topo_for_clients(struct nvmeibt_seg_active *seg_active)
{
	int8_t								idx_in_praid;
	int									rv = 0;
	struct nvmeibt_praid_lot			*praid_lot;

	if (seg_active) {
		praid_lot = nvmeibt_seg_active_get_applied_praid_lot(seg_active);
		if (praid_lot->topo_ctx.is_activated) {
			rv = nvmeibt_praid_lot_calc_topo_for_clients(praid_lot, &(seg_active->topo_for_clients), praid_lot->my_praid->praid_follower.applied_io_perms.all, true);
			idx_in_praid = nvmeibt_seg_active_get_applied_seg_lot(seg_active)->from_config.idx_in_praid;
			seg_active->is_seg_registrable = (seg_active->topo_for_clients.s[idx_in_praid].access_mode != NVMEIBTC_DS_MODE_DEAD);
		} else {
			N_Tf(bfyw3j4, "Don't build: seg=@UUID_8 praid is not activated", nvmeibt_seg_active_UUID_8(seg_active));
		}
	}
	return rv;
}

static void nvmeibt_seg_active_topo_reset(struct nvmeibt_seg_active *seg_active)
{
	struct timespec								ts;

	nvmeibt_generic_seg_topo_reset(&(seg_active->active_seg_topo));
	NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(shy7623, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
	NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE( djuy723, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
	NNVMEIBT_SEG_ACTIVE_SET_TXID_INIT_MODE(       dki98se, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
	NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(alo9rt4, seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
	getnstimeofday_real(&ts);
	seg_active->active_seg_topo.active_seg_ser_ver = ((uint64_t)ts.tv_sec << 32) + ts.tv_nsec;	// Hopefully works with disk hot-plug/unplug
}

struct nvmeibt_seg_active *nvmeibt_seg_active_create(const union nvmeib_uuid *uuid, struct nvmeibt_local_disk *local_disk,
													 struct nvmeibt_disk_gpt_partition_entry *metadata_gpt_entry,
													 struct nvmeibt_seg_active_metadata_ctrl *metadata_ctrl_fr_persist)
{
	struct nvmeibt_seg_active			*seg_active;

	NFIN;
	seg_active = NNVMEIBT_TOMA_CALLOC(fwwq99a, 1, sizeof(*seg_active));
	seg_active->uuid = *uuid;
	NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(v20sslk, seg_active, "LOCAL_DISK", 1);
	XHASHTABLE_INIT(&seg_active->longing_registrants_by_cid);
	XHASHTABLE_INIT(&seg_active->active_registrants);
	XHASHTABLE_INIT(&seg_active->active_registrants_by_cid);
	XHASHTABLE_INIT(&seg_active->stale_registrants);
	XHASHTABLE_INIT(&seg_active->stale_locks_hash);
	XHASHTABLE_INIT(&seg_active->awaited_lockids);
	XDLIST_HEAD_INIT(&seg_active->registrants_on_timeout);
	XDLIST_HEAD_INIT(&seg_active->owner_lock_ids_to_release);
	XDLIST_INIT_LINK(&seg_active->global_seg_active_post_update_action_link, NULL);
	seg_active->n_registrants_on_timeout = 0;
	seg_active->n_active_registrants_on_active_praid_version = 0;

	if (pthread_mutex_init(&seg_active->stale_locks_hash_mutex, NULL)) {
		N_Ef(ry876ha, "Failed to create stale locks mutex (@AUTO_ERRNO)");
	    nvmeibt_abort(ES_FATAL);
	}
	nvmeibt_register_move_all_longing_registrants_no_seg_to_seg(seg_active);

	seg_active->dirty_rebuild_ctx.tid = 0;
	seg_active->stale_rebuild_ctx.tid = 0;
	seg_active->cold_recovery_ctx.tid = 0;
	seg_active->JGC_rebuild_ctx.tid = 0;
	seg_active->scrubbing_ctx.tid = 0;

	seg_active->last_zeroing_progress_report_time = TIMESPEC_ZERO;
	seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_UNINITIALIZED;
	//nvmeibt_seg_active_mark_zeroing_required_as_needed(seg_active);

	seg_active->reg_lock_id_cache_last_allocated_lockid = 0;
	seg_active->reg_lock_id_cache_purge_seqno = 0;
	seg_active->reg_lock_id_cache_purge_zone = -1;
	seg_active->reg_lock_id_cache_purge_n_purges_in_fly = 0;

	seg_active->is_closing_to_reg = 0;
	seg_active->persistent_metadata = NNVMEIBT_BM_ALIGNED_CALLOC(752bnsj, PAGE_SIZE, SECTOR_SIZE);
	if (nvmeibt_seg_active_metadata_ctrl_init(seg_active) < 0) {
		N_Ef(5vgsywk, "Failed to init metadata ctrl for seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
	}
	seg_active->local_disk = local_disk;
	nvmeib_hash_add_uuid(local_disk->seg_active_hash_by_uuid, nvmeibt_seg_active_UUID(seg_active), seg_active);
	nvmeibt_seg_active_upd_metadata_gpt_entry_and_ctrl(seg_active, metadata_gpt_entry, metadata_ctrl_fr_persist);

	nvmeibt_seg_active_topo_reset(seg_active);

	seg_active->my_next_scrub_timeout_heap_element.idx_in_heap_arr = ILLEGAL_HEAP_ARR_IDX; // not added to heap yet

	seg_active->topo_for_clients.header.praid_version = PRAID_VERSION_INVALID_VALUE;

	return seg_active;
	NFOUT;
}

void nvmeibt_seg_active_we_got_its_seg(struct nvmeibt_seg_active *seg_active)
{
//	struct nvmeibt_disk_segment					*seg;

	NFIN;
	nvmeibt_seg_active_upd_liveliness_according_to_local_disk(seg_active);
	if (!nvmeibt_seg_active_is_jbod(seg_active)) {
		nvmeib_heap_add_element(&next_scrub_timeout_heap, &(seg_active->my_next_scrub_timeout_heap_element));
		NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(iqosl1a, n_pending_scrubbing);
		seg_active_scrub_reset(seg_active, 0);
	}
	nvmeibt_seg_active_mark_serialize_active_topo_for_leader_required(seg_active);
	NFOUT;
}

BOOL nvmeibt_seg_active_is_closing_to_reg(const struct nvmeibt_seg_active *seg_active)
{
	return (!seg_active || seg_active->is_closing_to_reg);
}

void nvmeibt_seg_active_mark_is_closing_to_reg(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		seg_active->is_closing_to_reg = 1;
	}
}

static void mark_seg_active_x_done_if_all_cleaning_works_are_finished(struct nvmeibt_seg_active *seg_active, bool is_forced)
{

	if (is_forced) {
		seg_active->applied_serjio_clean_range_state = SERJIO_CLEAN_RANGE_STATE_NOT_NEEDED;
		seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_NOT_NEEDED;
	}
	if (nvmeibt_seg_active_is_waiting_for_serjio_clean_range_done(seg_active)) {
		N_Tf(yrnfi23, "Don't mark as X_DONE, wait for serjio cleaning end seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	if (!nvmeibt_seg_active_is_zeroing_state_skipable(seg_active)) {
		N_Tf(yrnfi24, "Don't mark as X_DONE, wait for zeroing end seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(uu889v3, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE);
out:
	;
}
/* stale locks */

/***********************           SERJIO                   ************************/

const char *serjio_clean_range_state_str(enum SERJIO_CLEAN_RANGE_STATE s)
{
	switch (s) {
	case SERJIO_CLEAN_RANGE_STATE_UNINITIALIZED:	return "UNINITIALIZED";
	case SERJIO_CLEAN_RANGE_STATE_REQUIRED: 		return "REQUIRED";
	case SERJIO_CLEAN_RANGE_STATE_NOT_NEEDED:		return "NOT_NEEDED";
	case SERJIO_CLEAN_RANGE_STATE_IN_WORK:			return "IN_WORK";
	case SERJIO_CLEAN_RANGE_STATE_DONE_AT_INIT:		return "DONE_AT_INIT";
	case SERJIO_CLEAN_RANGE_STATE_DONE_DELETED:		return "DONE_DELETED";
	default : {
		static char	unexpected_val_str[] = "unexpected value               ";
		snprintf(unexpected_val_str, sizeof(unexpected_val_str), "unexpected value %x", s);
		return unexpected_val_str;
	}
	}
}

void nvmeibt_seg_active_reset_serjio_clean_range_state_on_new_config_or_topo(struct nvmeibt_seg_active *seg_active)
{
	//FIN;
	if (!seg_active || (seg_active->applied_serjio_clean_range_state == SERJIO_CLEAN_RANGE_STATE_DONE_DELETED))
		goto out;

	if (seg_active->applied_serjio_clean_range_state == SERJIO_CLEAN_RANGE_STATE_IN_WORK) {
		N_Tf(ii3477s, "seg=@UUID_8 serjio cleaning already in progress", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	// Always reset the serjio's CLEAN_RANGE state on new topo/config. we do not trust previous cleanups
	seg_active->applied_serjio_clean_range_state =
		((nvmeibt_seg_active_is_config_EC(seg_active) &&
		  ((nvmeibt_seg_active_is_deleted_in_config(seg_active) && !nvmeibt_disk_segment_is_x_done(nvmeibt_seg_active_get_active_seg_topo(seg_active))) ||
		   nvmeibt_disk_segment_is_under_recovery_I(nvmeibt_seg_active_get_active_seg_topo(seg_active)))) ?
		 SERJIO_CLEAN_RANGE_STATE_REQUIRED : SERJIO_CLEAN_RANGE_STATE_NOT_NEEDED);
	N_Tf(bdhas61, "seg=@UUID_8 clean_range=@CLEAN_RANGE", nvmeibt_seg_active_UUID_8(seg_active), serjio_clean_range_state_str(seg_active->applied_serjio_clean_range_state));
	if (seg_active->applied_serjio_clean_range_state == SERJIO_CLEAN_RANGE_STATE_NOT_NEEDED) {
		if (seg_active->JGC_rebuild_ctx.tid) {
			nvmeibt_recovery_launch_abort_rebuild(seg_active->JGC_rebuild_ctx.tid);
		}
	}
out:
	//FOUT;
	return;
}

int nvmeibt_seg_active_update_serjio_range_cleaned(char *seg_id)
{
	int								rv = -1;
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	seg_active = nvmeibt_global_get_seg_active_through_seg_by_urn_uuid_str(seg_id);
	if (!seg_active) {
		N_Wf(fhuty75, "OOPS, received unknown seg=@SEG from Serjio", seg_id);
		goto out;
	}
	N_Tf(ahuyt75,"seg=@SEG srj_state=@SRJ_STATE",
		 seg_id, serjio_clean_range_state_str(seg_active->applied_serjio_clean_range_state));

	if (nvmeibt_seg_active_is_deleted_in_config(seg_active)) {
		seg_active->applied_serjio_clean_range_state = SERJIO_CLEAN_RANGE_STATE_DONE_DELETED;
	}
	else {
		seg_active->applied_serjio_clean_range_state = SERJIO_CLEAN_RANGE_STATE_DONE_AT_INIT;
		nvmeibt_register_clients_sync_check_and_act_upon(seg_active);
	}
	mark_seg_active_x_done_if_all_cleaning_works_are_finished(seg_active, 0);
	rv = 0;
out:
	NFOUT;
	return rv;
}

static int nvmeibt_seg_active_notify_serjio_clean_range(struct nvmeibt_seg_active *seg_active, bool seg_deleted);
static int nvmeibt_seg_active_notify_serjio_clean_range(struct nvmeibt_seg_active *seg_active, bool seg_deleted)
{
	struct nvmeibs_toma_server_proc_buf buf;
	int									rv = -1;
	const struct nvmeibt_local_disk		*local_disk;
	struct nvmeibs_msg_t2s_clean_journal_for_range		*msg = &buf.clean_journal_msg;

	NFIN;
	if (!seg_active) {
		rv = 0;
		goto out;
	}
	if (seg_active->applied_serjio_clean_range_state != SERJIO_CLEAN_RANGE_STATE_REQUIRED) {
		N_Tf(tbebx7w, "Skipping seg=@UUID_8 state=@STATE_STR", nvmeibt_seg_active_UUID_8(seg_active), serjio_clean_range_state_str(seg_active->applied_serjio_clean_range_state));
		rv = 0;
		goto out;
	}
	if (nvmeibt_register_is_any_registered_on_seg_active(seg_active)) {
		N_Tf(trace_1_seg_active_notify_serjio_clean_range, "Skipping, has registrants");
		rv = 0;
		goto out;
	}
	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(x5cf48q, "disk=@STR is_being_deleted. Skipping", nvmeibt_local_disk_display(local_disk));
		rv = 0;
		goto out;
	}
	//build the req
	ZEROINIT(buf);
	buf.type = NVMEIBS_TOMA_CLEAN_JOURNAL_FOR_DISK_RANGE;
	nvmeibt_strlcpy(msg->disk_id, nvmeibt_local_disk_UUID_str(local_disk), sizeof(msg->disk_id));
	msg->vendor_id = nvmeibt_local_disk_vendor_id(local_disk);
	msg->start_4Klba = nvmeibt_seg_active_get_seg_mgmt(seg_active)->lb_s;
	msg->end_4Klba =   nvmeibt_seg_active_get_seg_mgmt(seg_active)->lb_e;
	msg->seg_deleted = seg_deleted ? 1 : 0;
	nvmeibt_strlcpy(msg->seg_uuid, nvmeibt_seg_active_id_str(seg_active), sizeof(msg->seg_uuid));

	N_Tf(jru8534, "disk=@STR lb_s=@UINT64_TX lb_e=@UINT64_TX seg=@STR", nvmeibt_local_disk_display(local_disk), msg->start_4Klba, msg->end_4Klba, msg->seg_uuid);
	rv = nvmeib_srvr_api_lib_send_block_msg_to_server(nvmeibt_get_srv_comm(), &buf);
	if (rv == EINPROGRESS)
		seg_active->applied_serjio_clean_range_state = SERJIO_CLEAN_RANGE_STATE_IN_WORK;
	rv = 0;	// Meaningless, no one checks this 'rv'.
out:
	NFOUT;
	return rv;
}

void nvmeibt_seg_active_notify_serjio_if_seg_is_being_deleted(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (!seg_active) {
		goto out;
	}
	if (!nvmeibt_seg_active_is_deleted_in_config(seg_active)) {
		N_Tf(idnwkh4, "@UUID_8 Not Deleted, Nothing to do", nvmeibt_seg_active_UUID_8(seg_active));
	}
	else {
		N_Tf(xnkzw5j, "@UUID_8 Deleted, Notifying SERJIO", nvmeibt_seg_active_UUID_8(seg_active));
		nvmeibt_seg_active_notify_serjio_clean_range(seg_active, true);
	}
out:
	NFOUT;
}

/***********************                              ************************/

static BOOL is_mem_tbl_init_mode_applicable(enum NVMEIBT_MEM_TBL_INIT_MODE init_mode)
{
	return (0 == (init_mode &
				  (NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN | NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED |
				   NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE)));
}

void nvmeibt_seg_active_init_locks_table(struct nvmeibt_seg_active *seg_active)
{
	bool			is_stale_rebuild_required;

	NFIN;
	if (!nvmeibt_praid_is_client_sync_cmd_initializing(nvmeibt_seg_active_get_registrants_sync_cmd(seg_active))) {
		goto out;
	}
	N_Tf(7fh3iub, "seg=@UUID_8 sync_cmd=@SYNC_CMD dirty_bits_init_mode=@DIRTY_BITS_INIT_MODE stale_locks_init_mode=@STALE_LOCKS_INIT_MODE txid_init_mode=@TXID_INIT_MODE",
		nvmeibt_seg_active_UUID_8(seg_active), praid_registrants_sync_cmd_str(nvmeibt_seg_active_get_registrants_sync_cmd(seg_active)),
		mem_tbl_init_mode_str(nvmeibt_seg_active_get_active_seg_topo(seg_active)->dirty_bits_init_mode),
		mem_tbl_init_mode_str(nvmeibt_seg_active_get_active_seg_topo(seg_active)->stale_locks_init_mode),
		mem_tbl_init_mode_str(nvmeibt_seg_active_get_active_seg_topo(seg_active)->txid_init_mode));
	if (	!is_mem_tbl_init_mode_applicable(nvmeibt_seg_active_get_active_seg_topo(seg_active)->dirty_bits_init_mode) &&
			!is_mem_tbl_init_mode_applicable(nvmeibt_seg_active_get_active_seg_topo(seg_active)->stale_locks_init_mode)) {
		goto out;
	}
	if (!nvmeibt_seg_active_are_registrants_aligned_with_sync_cmd(seg_active)) {
		N_Tf(vsgwy38, "awaiting_clients_sync seg=@UUID_8 n_active=@N_ACTIVE n_applied=@N_APPLIED",
			nvmeibt_seg_active_UUID_8(seg_active),
			nvmeibt_seg_active_n_active_registrants(seg_active),
			nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active));
		goto out;
	}

	if (nvmeibt_seg_active_is_jbod(seg_active)) {
		is_stale_rebuild_required = 0;
		NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(3bc9adj,	seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
		NNVMEIBT_SEG_ACTIVE_SET_TXID_INIT_MODE(c8a9l0ss,		seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(1m9xim4,	seg_active, NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
	}
	else {
		is_stale_rebuild_required = (nvmeibt_seg_active_get_active_seg_topo(seg_active)->stale_locks_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER);
		if (nvmeibt_seg_active_is_config_EC(seg_active)) {
			nvmeibt_seg_active_notify_serjio_clean_range(seg_active, false);
			is_stale_rebuild_required &= nvmeibt_ds_metadata_init_EC_locks_table(seg_active);
		} else { // RAID1
			is_stale_rebuild_required &= nvmeibt_ds_metadata_init_non_EC_locks_table(seg_active);
		}
	}

	if (is_stale_rebuild_required)
		nvmeibt_seg_active_mark_stale_rebuild_required(seg_active);
out:
	NFOUT;
}

/************************ Stale locks hash map. ********/
static void remove_stale_lock_from_seg_stale_locks_hash(struct stale_lock_ctx *stale_lock)
{
	struct nvmeibt_seg_active		*seg_active;
	struct nvmeibt_registrant_ctx	*reg_ctx;

	NFIN;
	reg_ctx = stale_lock->reg_ctx;
	seg_active = reg_ctx->seg_active;
	N_Tf(t_s1_tslh, "Deleting " STALE_BLKSET_FMT,
		stale_lock->seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active), nvmeib_lockid_purify(reg_ctx->reg_lock_id));
	XHASHTABLE_DEL(&seg_active->stale_locks_hash, &stale_lock->seg_active_link);
	NNVMEIBT_BM_FREE(trace_1_seg_active_remove_stale_lock_from_seg_stale_locks_hash, stale_lock);
	if (--(reg_ctx->n_stale_locks) == 0) {
		nvmeibt_register_terminate_registrant(reg_ctx, 0);
	}
	NFOUT;
}

void nvmeibt_seg_active_delete_all_stale_locks_of_registrant(
	struct nvmeibt_seg_active *seg_active, struct nvmeibt_registrant_ctx *reg_ctx)
{
	struct stale_lock_ctx			*stale_lock;

	NFIN;
	lock_stale_locks_hash(seg_active);
	XHASHTABLE_FOR_EACH_SAFE(stale_lock, &seg_active->stale_locks_hash) {
		if (stale_lock->reg_ctx == reg_ctx) {
			remove_stale_lock_from_seg_stale_locks_hash(stale_lock);
		}
	}
	unlock_stale_locks_hash(seg_active);
	NFOUT;
}

static struct stale_lock_ctx *get_stale_lock_by_blkset_no(
	struct nvmeibt_seg_active *seg_active, unsigned long long seg_blkset_no, union nvmeib_lock_id lockid)
{
	struct stale_lock_ctx			*stale_lock = NULL;
	const union nvmeib_lock_id		pure_recovered_lid = {.all = nvmeib_lockid_purify(lockid) };

	NFIN;
	lock_stale_locks_hash(seg_active);
	XHASHTABLE_FOR_EACH_POSSIBLE_SAFE(stale_lock, &seg_active->stale_locks_hash, seg_blkset_no) {
		if (stale_lock->seg_blkset_no == seg_blkset_no) {
			const union nvmeib_lock_id expected_lid = stale_lock->reg_ctx->reg_lock_id;
			N_Tf(t_02_tstlkrec, "Found " STALE_BLKSET_FMT,
				seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active), pure_recovered_lid.all);
			if (!nvmeib_lockid_are_purified_eq(expected_lid, pure_recovered_lid)) {
				N_Ef(t_03_tstlkrec, "lockid mismatch blkset_lockid=@T_LID stale_lockid=@C_LID",
					expected_lid.all, lockid.all);
				nvmeibt_abort(ES_FATAL);
//				continue;
			}
			goto out;
		}
	}
	stale_lock = NULL;		// not found
	if (lockid.all != 0) {	// Client says there is a stale lock here
		N_Ef(t_11_tstlkrec, "No stale_lock, " STALE_BLKSET_FMT ". Ignoring",
			 seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active), lockid.all);
	}

out:
	unlock_stale_locks_hash(seg_active);
	NFOUT;
	return stale_lock;
}

struct nvmeibt_registrant_ctx *nvmeibt_seg_active_add_blkset_to_stale_locks_hash(
	struct nvmeibt_registrant_ctx *reg_ctx, unsigned long long seg_blkset_no, bool existing_lock_id_bits_is_read)
{
	struct nvmeibt_registrant_ctx	*reg_ctx_to_restore = NULL;
	struct nvmeibt_seg_active		*seg_active;
	struct stale_lock_ctx			*stale_lock;

	NFIN;
	seg_active = reg_ctx->seg_active;
	lock_stale_locks_hash(seg_active);
	if (!nvmeibt_seg_active_is_config_EC(seg_active)) {
		N_Tf(i9i8ub2, "non journalled praid seg=@UUID_8. Skipping.", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	XHASHTABLE_FOR_EACH_POSSIBLE_SAFE(stale_lock, &seg_active->stale_locks_hash, seg_blkset_no) {
		if (stale_lock->seg_blkset_no == seg_blkset_no) {
			if (stale_lock->reg_ctx == reg_ctx) {
				N_Wf(xtvsjw9, "Stale entry already exists " STALE_BLKSET_FMT " client_id=@MY_HOSTNAME",
					stale_lock->seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active),
					nvmeib_lockid_purify(stale_lock->reg_ctx->reg_lock_id),
					stale_lock->reg_ctx->client->net.host_name);
				reg_ctx_to_restore = stale_lock->reg_ctx;
				goto out;
			} else {
				if (existing_lock_id_bits_is_read) {
					N_Tf(x5vcwh3, "Found stale entry " STALE_BLKSET_FMT " in stale_locks_hash. In mem we have an 'is_read (being recovered)' " STALE_BLKSET_FMT " client_id=@MY_HOSTNAME",
						 stale_lock->seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active),
						 nvmeib_lockid_purify(stale_lock->reg_ctx->reg_lock_id),
						 seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active),
						 nvmeib_lockid_purify(reg_ctx->reg_lock_id),
						stale_lock->reg_ctx->client->net.host_name);
					N_Tf(rctyahe, "The value in the locks table will be restored according to stale_locks_hash. The recoverer died");
					reg_ctx_to_restore = stale_lock->reg_ctx;
					goto out;

				} else {
					N_Ef(tcvshwu, "Surprise. Found stale entry " STALE_BLKSET_FMT " different from new " STALE_BLKSET_FMT " client_id=@MY_HOSTNAME",
						 stale_lock->seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active),
						 nvmeib_lockid_purify(stale_lock->reg_ctx->reg_lock_id),
						 seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active),
						 nvmeib_lockid_purify(reg_ctx->reg_lock_id),
						stale_lock->reg_ctx->client->net.host_name);
					N_Tf(omjsy37, "Erasing the old entry, and adding the new one. The old one is definitely wrong");
					remove_stale_lock_from_seg_stale_locks_hash(stale_lock);
				}
			}
			break;
		}
	}
	stale_lock = NNVMEIBT_BM_ALLOC(hf7i30w, sizeof(*stale_lock));
	stale_lock->seg_blkset_no = seg_blkset_no;
	stale_lock->reg_ctx = reg_ctx;
	(reg_ctx->n_stale_locks)++;
	XHASHTABLE_ADD(&seg_active->stale_locks_hash, stale_lock, seg_blkset_no);
	N_Tf(t_s6_tslh, "Adding " STALE_BLKSET_FMT,
		seg_blkset_no, nvmeibt_seg_active_UUID_8(seg_active), nvmeib_lockid_purify(reg_ctx->reg_lock_id));
out:
	unlock_stale_locks_hash(seg_active);
	NFOUT;
	return reg_ctx_to_restore;
}

int nvmeibt_seg_active_handle_blkset_recovered(struct nvmeibs_msg_s2t_blkset_recovered *blkset_recovered_msg)
{
	struct stale_lock_ctx			*stale_lock;
	struct nvmeibt_seg_active		*seg_active;
	union nvmeib_lock_blkset_entry	*pre_recov_lock_ent = (union nvmeib_lock_blkset_entry *)&blkset_recovered_msg->pre_recov_lock_val;
	const union nvmeib_lock_id		recovered_stale_lockid = pre_recov_lock_ent->lock_id;
	int                             rv = 1;

	NFIN;
	seg_active = nvmeibt_global_get_seg_active_through_seg_by_urn_uuid_str(blkset_recovered_msg->disk_segment_urn_uuid_str);
	if (!seg_active) {
		N_Ef(hu8hs03, "Bad blkset_recovered message received, UUID @STR does not point to an active segment!", blkset_recovered_msg->disk_segment_urn_uuid_str);
		goto out;
	}
	stale_lock = get_stale_lock_by_blkset_no(seg_active, blkset_recovered_msg->blkset_no, recovered_stale_lockid);

	if (stale_lock) {
		lock_stale_locks_hash(seg_active);
		remove_stale_lock_from_seg_stale_locks_hash(stale_lock);
		unlock_stale_locks_hash(seg_active);
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

void stale_locks_hash_to_string(printf_fn_t printf_fn, void *printf_ctx, struct nvmeibt_seg_active *seg_active)
{
	struct stale_lock_ctx			*stale_lock = NULL;
	NFIN;
	lock_stale_locks_hash(seg_active);
	XHASHTABLE_FOR_EACH_SAFE(stale_lock, &seg_active->stale_locks_hash) {
		const struct nvmeibt_registrant_ctx *reg_ctx = stale_lock->reg_ctx;
		const union nvmeib_uuid *uuid = &reg_ctx->client->client_provided_uuid;
		(*printf_fn)(printf_ctx, "\t\t\t\t- (blkset=0x%llx, lockid=0x%x cuuid=%016llx-%016llx)\n",	// @UUID_0-@UUID_1
				stale_lock->seg_blkset_no, reg_ctx->reg_lock_id, uuid->ll[0], uuid->ll[1]);
	}
	unlock_stale_locks_hash(seg_active);
	NFOUT;
}

/********************************** rebuild ***********************************/

void nvmeibt_seg_active_mark_cold_recovery_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		if (!nvmeibt_seg_active_is_cold_recovery_required(seg_active)) { NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(eisa910,n_pending_cold_recovery); }
		N_Tf(ji98nko, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->required_recovery_action.cold_recovery = 1;
		//NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(behbs71, seg_active);
	}
}

void nvmeibt_seg_active_clear_cold_recovery_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		N_Tf(f5f4j82, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		if (nvmeibt_seg_active_is_cold_recovery_required(seg_active)) {
			NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(xvnms01, n_pending_cold_recovery);
		}
		seg_active->required_recovery_action.cold_recovery = 0;
	}
}

BOOL nvmeibt_seg_active_is_cold_recovery_required(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->required_recovery_action.cold_recovery : 0);
}

void nvmeibt_seg_active_mark_stale_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		if (!(seg_active->required_recovery_action.stale_rebuild)) {
			NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(a54l0jo, n_pending_stale_rebuild);
			N_Tf(sdf23q9, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
			seg_active->required_recovery_action.stale_rebuild = 1;
			NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(2vvv2y4, seg_active);
			NNVMEIBT_SEG_ACTIVE_SET_IS_EXPECTED_TO_HAVE_STALE_LOCKS(tv393c8, seg_active, 1);
		}
	}
}

void nvmeibt_seg_active_clear_stale_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		N_Tf(lolo0f5, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		if (seg_active->required_recovery_action.stale_rebuild) {
			NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(kslq1i2, n_pending_stale_rebuild);
			seg_active->required_recovery_action.stale_rebuild = 0;
		}
	}
}

BOOL nvmeibt_seg_active_is_stale_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ?
			(seg_active->required_recovery_action.stale_rebuild &&
			 nvmeibt_seg_active_get_is_expected_to_have_stale_locks(seg_active)) :
			0);
}

void nvmeibt_seg_active_mark_txid_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		if (!nvmeibt_seg_active_is_txid_rebuild_required(seg_active)) { NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(b4jty8, n_pending_txid_rebuild); }
		N_Tf(sdf23e3, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->required_recovery_action.txid_rebuild = 1;
		NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(2vvv2k7, seg_active);
	}
}

void nvmeibt_seg_active_clear_txid_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		N_Tf(lolo0y2, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		if (nvmeibt_seg_active_is_txid_rebuild_required(seg_active)) {
			NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(jwwo039, n_pending_txid_rebuild);
		}
		seg_active->required_recovery_action.txid_rebuild = 0;
	}
}

BOOL nvmeibt_seg_active_is_txid_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->required_recovery_action.txid_rebuild : 0);
}

void nvmeibt_seg_active_mark_dirty_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		if (!nvmeibt_seg_active_is_dirty_rebuild_required(seg_active)) { NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(lbt1g8, n_pending_dirty_rebuild); }
		N_Tf(wjug4jd, "seg=@UUID_8, old recovery @T_PRV new @T_PRV",
				nvmeibt_seg_active_UUID_8(seg_active), seg_active->required_recovery_action.praid_version_major, nvmeibt_seg_active_get_active_praid_version_major(seg_active));
		if (seg_active->required_recovery_action.praid_version_major != nvmeibt_seg_active_get_active_praid_version_major(seg_active)) {
			seg_active->required_recovery_action.praid_version_major = nvmeibt_seg_active_get_active_praid_version_major(seg_active);
			seg_active->dirty_rebuild_ctx.n_blksets_remaining = num_blksets_in_disk_segment(nvmeibt_seg_active_get_disk_segment(seg_active));
			seg_active->dirty_rebuild_ctx.prev_report_n_blksets_remaining = U64_MAX;
		}
		//NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(x93m328, seg_active);
	}
}

BOOL nvmeibt_seg_active_is_dirty_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		return (seg_active->required_recovery_action.praid_version_major == nvmeibt_seg_active_get_active_praid_version_major(seg_active) &&
				nvmeibt_praid_is_sync_cmd_run_dirty_rebuild(nvmeibt_seg_active_get_registrants_sync_cmd(seg_active)));
	} else {
		return 0;
	}
}

void nvmeibt_seg_active_clear_dirty_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		N_Tf(guy765r, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		if (nvmeibt_seg_active_is_dirty_rebuild_required(seg_active)) {
			NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(wuej192, n_pending_dirty_rebuild);
		}
		seg_active->required_recovery_action.praid_version_major = 0;
	}
}

void nvmeibt_seg_active_mark_JGC_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active && !seg_active->required_recovery_action.JGC_rebuild) {
		if (!nvmeibt_seg_active_is_JGC_rebuild_required(seg_active)) { NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(aiwk102, n_pending_JGC_rebuild); }
		N_Tf(trace_zzz_40, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->required_recovery_action.JGC_rebuild = 1;
		seg_active->JGC_rebuild_required_timeout_sec = nvmeibt_global_get_cur_event_start_time().tv_sec + 60;
	}
}

void nvmeibt_seg_active_clear_JGC_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		N_Tf(trace_zzz_41, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		if (nvmeibt_seg_active_is_JGC_rebuild_required(seg_active)) {
			NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(salq01k, n_pending_JGC_rebuild);
		}
		seg_active->required_recovery_action.JGC_rebuild = 0;
	}
}

BOOL nvmeibt_seg_active_is_JGC_rebuild_required(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->required_recovery_action.JGC_rebuild : 0);
}

void nvmeibt_seg_active_set_n_dirty_bits_remaining(struct nvmeibt_seg_active *seg_active, uint64_t n_blksets_remaining)
{
	N_Tf(srt54e9, "seg=@UUID_8 n=@N_DBITS", nvmeibt_seg_active_UUID_8(seg_active), n_blksets_remaining);
	seg_active->dirty_rebuild_ctx.n_blksets_remaining = n_blksets_remaining;
	nvmeibt_global_mark_is_any_rebuild_progress_report_to_mgmt_due();
}

void nvmeibt_seg_active_set_n_stale_locks_remaining(struct nvmeibt_seg_active *seg_active, uint64_t n_blksets_remaining)
{
	N_Tf(lopkjh8, "seg=@UUID_8 n=@UINT64_TX", nvmeibt_seg_active_UUID_8(seg_active), n_blksets_remaining);
	seg_active->stale_rebuild_ctx.n_blksets_remaining = n_blksets_remaining;
}

void nvmeibt_seg_active_set_n_txid_remaining(struct nvmeibt_seg_active *seg_active, uint64_t n_blksets_remaining)
{
	N_Tf(lopkja5, "seg=@UUID_8 n=@UINT64_TX", nvmeibt_seg_active_UUID_8(seg_active), n_blksets_remaining);
	seg_active->txid_rebuild_ctx.n_blksets_remaining = n_blksets_remaining;
}

static struct nvmeibt_seg_active_recovery_ctx *seg_active_get_recovery_ctx(
	enum NVMEIBT_RECOVERY_TYPE recovery_type, struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_seg_active_recovery_ctx *recovery_ctx = NULL;

	switch (recovery_type) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:
		recovery_ctx = &seg_active->dirty_rebuild_ctx;
		break;
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:
		recovery_ctx = &seg_active->stale_rebuild_ctx;
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:
		recovery_ctx = &seg_active->txid_rebuild_ctx;
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
		recovery_ctx = &seg_active->JGC_rebuild_ctx;
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
		recovery_ctx = &seg_active->cold_recovery_ctx;
		break;
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:
		recovery_ctx = &seg_active->scrubbing_ctx;
		break;
	default:
		N_Ef(e1_seg_active_get_recovery_ctx,
			"invalid task type=@STR", nvmeibt_recovery_type_to_str(recovery_type));
		nvmeibt_abort(ES_FATAL);
	}

	return recovery_ctx;
}

void nvmeibt_seg_active_get_recovery_praid_version(
	enum NVMEIBT_RECOVERY_TYPE recovery_type, struct nvmeibt_seg_active *seg_active, int *praid_version)
{
	struct nvmeibt_seg_active_recovery_ctx *recovery_ctx;
	recovery_ctx = seg_active_get_recovery_ctx(recovery_type, seg_active);
	*praid_version = recovery_ctx->praid_version;
}

void nvmeibt_seg_active_get_recovery_blkset_range(
	enum NVMEIBT_RECOVERY_TYPE recovery_type, struct nvmeibt_seg_active *seg_active, u64 *blkset_s, u64 *n_blksets)
{
	struct nvmeibt_disk_segment					*seg;

	seg = nvmeibt_seg_active_get_disk_segment(seg_active);

	switch (recovery_type) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:
		*blkset_s = seg_active->dirty_next_unfixed_lock_blkset_no;
		*n_blksets = num_blksets_in_disk_segment(seg) - *blkset_s;
		break;
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
		*blkset_s = 0;
		*n_blksets = num_blksets_in_disk_segment(seg);
		break;
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:
		*blkset_s = seg_active->persistent_metadata->n_blksets_scrubbed;
		*n_blksets = min((uint64_t)nvmeibt_recovery_n_blksets_per_scrub_iteration, num_blksets_in_disk_segment(seg) - *blkset_s);
		seg_active->scrubbing_iter_n_blksets = *n_blksets;
		break;
	default:
		N_Ef(asej439, "Unknown recovery_type=@RECOVERY_TYPE", recovery_type);
		nvmeibt_abort(ES_FATAL);
	}
}

static void stop_txid_rebuild(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (seg_active && seg_active->txid_rebuild_ctx.tid) {
		nvmeibt_recovery_launch_abort_rebuild(seg_active->txid_rebuild_ctx.tid);
	}
	NFOUT;
}

static void stop_stale_rebuild(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (seg_active && seg_active->stale_rebuild_ctx.tid) {
		nvmeibt_recovery_launch_abort_rebuild(seg_active->stale_rebuild_ctx.tid);
	}
	NFOUT;
}

void stop_all_stale_and_txid_rebuild_tasks(void)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_seg_active	*seg_active;

	if (!nvmeibt_topology_is_HW_config_functional() || (NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_stale_rebuild) + NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_txid_rebuild) == 0))
		goto out;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			stop_stale_rebuild(seg_active);
			stop_txid_rebuild(seg_active);
		}
	}

out:
	NFOUT;
}

static void stop_JGC_rebuild(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (seg_active && seg_active->JGC_rebuild_ctx.tid) {
		nvmeibt_recovery_launch_abort_rebuild(seg_active->JGC_rebuild_ctx.tid);
	}
	NFOUT;
}

void stop_all_JGC_rebuild_tasks(void)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_seg_active	*seg_active;

	if (!nvmeibt_topology_is_HW_config_functional() || (nvmeibt_global_get_global()->n_running_JGC_rebuild == 0))
		goto out;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			stop_JGC_rebuild(seg_active);
		}
	}

out:
	NFOUT;
}

static void stop_cold_recovery(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (seg_active && seg_active->cold_recovery_ctx.tid) {
		// Stop the running thread
		N_Tf(tyb7bvf, "stopping tid=@TID", seg_active->cold_recovery_ctx.tid);
		seg_active->cold_recovery_ctx.praid_version = 0;
		nvmeibt_recovery_launch_abort_rebuild(seg_active->cold_recovery_ctx.tid);
	}
	NFOUT;
}

static void stop_dirty_rebuild(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (seg_active && seg_active->dirty_rebuild_ctx.tid) {
		// Stop the running thread
		N_Tf(dr5tty6, "stopping tid=@TID", seg_active->dirty_rebuild_ctx.tid);
		seg_active->dirty_rebuild_ctx.praid_version = 0;
		nvmeibt_recovery_launch_abort_rebuild(seg_active->dirty_rebuild_ctx.tid);
	}
	NFOUT;
}

static void stop_scrubbing(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (seg_active && seg_active->scrubbing_ctx.tid) {
		// Stop the running thread
		N_Tf(ii9iu34, "stopping tid=@TID", seg_active->scrubbing_ctx.tid);
		seg_active->scrubbing_ctx.praid_version = 0;
		nvmeibt_recovery_launch_abort_rebuild(seg_active->scrubbing_ctx.tid);
	}
	NFOUT;
}

static void stop_all_scrubbing_tasks(void)
{
	struct nvmeibt_seg_active	*seg_active;
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;

	if (!nvmeibt_topology_is_HW_config_functional() || (cur_topo->n_running_scrubbing == 0))
		goto out;

	NVMEIB_HASH_FOREACH(local_disk, cur_topo->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			stop_scrubbing(seg_active);
		}
	}

out:
	NFOUT;
}

void nvmeibt_seg_active_stop_recovery_tasks(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	stop_JGC_rebuild(seg_active);
	stop_dirty_rebuild(seg_active);
	stop_stale_rebuild(seg_active);
	stop_txid_rebuild(seg_active);
	stop_cold_recovery(seg_active);
	stop_scrubbing(seg_active);
	// Do not stop registrant_disconnects, as it will leave unhandled stale locks.
	// The registrant_disconnect must finish either successfully, or due to topo-change
	NFOUT;
}

static void nvmeibt_seg_active_dirty_rebuild(struct nvmeibt_seg_active *seg_active)
{
	uint64_t								tid;

	NFIN;

	if (!seg_active) {
		N_Ef(error_seg_active_nvmeibt_seg_active_dirty_rebuild, "seg_active==NULL");
		goto out;
	}
	if (	!(nvmeibt_seg_active_dirty_bits_state(seg_active) & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER) ||
			(nvmeibt_seg_active_get_active_praid_version_major(seg_active) == seg_active->dirty_rebuild_ctx.praid_version)) {
		goto out;
	}

	N_Tf(7vg3mk0, "seg=@UUID_8 dirty-rebuild is required", nvmeibt_seg_active_UUID_8(seg_active));

	nvmeibt_seg_active_stop_recovery_tasks(seg_active);

	if (seg_active->dirty_rebuild_ctx.tid || seg_active->stale_rebuild_ctx.tid) {
		N_Wf(huy7t65, "old tasks still running, retry later. dirty_rebuild_ctx.tid=@TID stale_rebuild_ctx.tid=@TID",
			seg_active->dirty_rebuild_ctx.tid, seg_active->stale_rebuild_ctx.tid);
		goto out;
	}

	// All is well, launch the rebuild
	tid = nvmeibt_recovery_start_rebuild(NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, seg_active);
	if (tid == -1ULL) {
		goto out;
	}

	seg_active->dirty_rebuild_ctx.praid_version = nvmeibt_seg_active_get_active_praid_version_major(seg_active);
	nvmeibt_seg_active_clear_dirty_rebuild_required(seg_active);
	NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(bdhd7eh, n_running_dirty_rebuild);
	seg_active->dirty_rebuild_ctx.tid = tid;

	N_Tf(drf56t3, "Launched dirty_rebuild seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
out:
	NFOUT;
}

void nvmeibt_recovery_execute_dirty_rebuilds_as_needed(void)
{
	struct nvmeibt_seg_active		*seg_active;
	struct nvmeibt_local_disk		*local_disk;
	int								i, max_n_simultaneous_dirty_rebuild;

	NFIN;
	if (!nvmeibt_topology_is_HW_config_functional())
		goto out;

	for (i = 0; i < 2; i++) {
		// Enable double amount of RAID1 rebuilds
		max_n_simultaneous_dirty_rebuild = nvmeibt_recovery_get_max_n_simultaneous_dirty_rebuild() * (2 - i);
		NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
			NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
				// The following is inside the loop since N_RUNNING_TASKS might change.
				if (NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_dirty_rebuild) >= max_n_simultaneous_dirty_rebuild) {
					N_Tf(ddu8760, "Skipping dirty rebuilds, n_running_dirty_rebuild=@INT", NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_dirty_rebuild));
					goto out;
				}
				if (!nvmeibt_seg_active_is_dirty_rebuild_required(seg_active) ||
					(nvmeibt_seg_active_is_EC(seg_active) ^ i)) {  // During the first loop launch only RAID1 rebuilds, during the second - only EC
					continue;
				}
				// Run recovery thread per this PRAID segment, that resides on this node, and is owner_recoverer
				if (nvmeibt_disk_segment_is_de_facto_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
					nvmeibt_seg_active_dirty_rebuild(seg_active);
				} else {
					N_Tf(6hdoueb, "Skipping seg=@UUID_8 @STR", nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_seg_active_dirty_bits_state_str(seg_active));
				}
			}
		}
	}

out:
	NFOUT;
}

static void nvmeibt_seg_active_cold_recovery(struct nvmeibt_seg_active *seg_active)
{
	uint64_t								tid;

	NFIN;

	if (!seg_active) {
		N_Ef(kito97m, "seg_active==NULL");
		goto out;
	}
	if (	!nvmeibt_disk_segment_is_ec_cold_recoverer(nvmeibt_seg_active_get_active_seg_topo(seg_active)) ||
			(nvmeibt_seg_active_get_active_praid_version_major(seg_active) == seg_active->cold_recovery_ctx.praid_version)) {
		goto out;
	}

	N_Tf(5bfhsch, "seg=@UUID_8 cold_recovery is required", nvmeibt_seg_active_UUID_8(seg_active));

	stop_all_stale_and_txid_rebuild_tasks();
	stop_all_scrubbing_tasks();
	nvmeibt_seg_active_stop_recovery_tasks(seg_active);

	if (	seg_active->cold_recovery_ctx.tid || seg_active->dirty_rebuild_ctx.tid || seg_active->stale_rebuild_ctx.tid ||
			seg_active->JGC_rebuild_ctx.tid) {
		N_Wf(warn_seg_active_nvmeibt_disk_segment_cold_recovery, "seg=@UUID_8 old tasks still running, retry later. "
			 "cold_rebuild_ctx.tid=@TID dirty_rebuild_ctx.tid=@TID stale_rebuild_ctx.tid=@TID JGC_rebuild_ctx.tid=@TID",
			 nvmeibt_seg_active_UUID_8(seg_active),
			 seg_active->cold_recovery_ctx.tid,
			 seg_active->dirty_rebuild_ctx.tid,
			 seg_active->stale_rebuild_ctx.tid,
			 seg_active->JGC_rebuild_ctx.tid);
		goto out;
	}

	// All is well, launch the rebuild

	tid = nvmeibt_recovery_start_rebuild(NVMEIBT_RECOVERY_TYPE_EC_COLD, seg_active);
	if (tid == -1ULL) {
		goto out;
	}

	seg_active->cold_recovery_ctx.praid_version = nvmeibt_seg_active_get_active_praid_version_major(seg_active);
	nvmeibt_seg_active_clear_cold_recovery_required(seg_active);
	NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(sksowa0, n_running_cold_recovery);
	seg_active->cold_recovery_ctx.tid = tid;

	N_Tf(ee4r762, "Launched cold_recovery seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
out:;
	NFOUT;
}

void nvmeibt_recovery_execute_cold_recoveries_as_needed(void)
{
	struct nvmeibt_seg_active		*seg_active;
	struct nvmeibt_local_disk		*local_disk;

	NFIN;
	if (!nvmeibt_topology_is_HW_config_functional())
		goto out;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			if (!nvmeibt_seg_active_is_cold_recovery_required(seg_active)) {
				continue;
			}
			// The following is inside the loop since N_RUNNING_TASKS might change.
			if (NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_cold_recovery) >= MAX_N_RUNNING_COLD_RECOVERY_PER_NODE) {
				N_Tf(ki982nd, "Skipping, n_running_cold_recovery=@INT", NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_cold_recovery));
				goto out;
			}
			// Run recovery thread per this PRAID segment, that resides on this node, and is owner_recoverer
			if (nvmeibt_disk_segment_is_de_facto_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
				nvmeibt_seg_active_cold_recovery(seg_active);
			} else {
				N_Tf(u867cn3, "Skipping seg=@UUID_8 @STR", nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_seg_active_dirty_bits_state_str(seg_active));
			}
		}
	}

out:
	NFOUT;
}

static void nvmeibt_seg_active_txid_rebuild(struct nvmeibt_seg_active *seg_active)
{
	u64								tid;

	NFIN;

	// Run unless already running or terminating an old run
	if (seg_active->txid_rebuild_ctx.tid) {
		N_Tf(ybsowve, "tx_rebuild seg=@UUID_8 already running", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	// We should not get here if dirty-bits rebuild is in the air
	if (seg_active->dirty_rebuild_ctx.tid) {
		N_Ef(sfty78w, "dirty-rebuild seg=@UUID_8 already running", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	// Launch a txid-rebuild
	tid = nvmeibt_recovery_start_rebuild(NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO, seg_active);
	if (tid == -1ULL) {
		goto out;
	}

	nvmeibt_seg_active_clear_txid_rebuild_required(seg_active);
	NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(xqoalh2, n_running_txid_rebuild);
	seg_active->txid_rebuild_ctx.tid = tid;

	N_Tf(iu8uyr9, "Launch tx_rebuild seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
out:
	NFOUT;
	return;
}

static void nvmeibt_seg_active_stale_rebuild(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_praid			*praid;
	u64								tid;
	int64_t							nsec_since_last_registrant_disconnect;

	NFIN;

	if (!seg_active) {
		N_Ef(e459832, "seg_active==NULL");
		goto out;
	}
	// Run unless already running or terminating an old run
	if (seg_active->stale_rebuild_ctx.tid) {
		N_Tf(ybsow8c, "stale_rebuild seg=@UUID_8 already running", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	praid = nvmeibt_seg_active_get_praid(seg_active);
	if (!nvmeibt_praid_applied_is_qualify_for_sync_stale(praid)) {
		// Stopping running instance
		if (seg_active->stale_rebuild_ctx.tid) {
			N_Ef(error_1_seg_active_nvmeibt_seg_active_stale_rebuild, "All running stale_rebuild should have been terminated by now");
		}
		goto out;
	}
	// We should not get here if dirty-bits rebuild is in the air
	if (seg_active->dirty_rebuild_ctx.tid) {
		N_Ef(sfty7yz, "dirty-rebuild seg=@UUID_8 already running", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	nsec_since_last_registrant_disconnect =
			timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), seg_active->last_registrant_disconnect_timespec);
	if (nsec_since_last_registrant_disconnect < SEC_TO_NSEC(1)) {
		N_Tf(opo09s3, "nsec_since_last_registrant_disconnect=@LLD, Waiting some more",
			nsec_since_last_registrant_disconnect);
		nvmeibt_seg_active_mark_stale_rebuild_required(seg_active);
		goto out;
	}
	// Launch a stale-rebuild
	NTODO_N_REPLICA_SUPPORT(yrvf8sk,
		"compute this.Loop on all @N_TOPO_SEGS replicas that "
		"are needed for recovery, and not just for n=2", nvmeibt_seg_active_get_applied_praid_lot(seg_active)->n_topo_seg_lots);

	NNVMEIBT_SEG_ACTIVE_SET_IS_EXPECTED_TO_HAVE_STALE_LOCKS(pqbx3nf, seg_active, 0);	// Before launching an async rebuild
	tid = nvmeibt_recovery_start_rebuild(NVMEIBT_RECOVERY_TYPE_STALE_REBUILD, seg_active);
	if (tid == -1ULL) {
		NNVMEIBT_SEG_ACTIVE_SET_IS_EXPECTED_TO_HAVE_STALE_LOCKS(dy7ner2, seg_active, 1);	// Failed to launch the rebuild. Probably not accepting registrations reason=SEG_MD_STORING
		goto out;
	}

	nvmeibt_seg_active_clear_stale_rebuild_required(seg_active);
	NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(xqoalk9, n_running_stale_rebuild);
	seg_active->stale_rebuild_ctx.tid = tid;

	N_Tf(iu8uy76, "Launch stale_rebuild seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
out:
	NFOUT;
	return;
}

void nvmeibt_recovery_execute_stale_and_txid_rebuilds_as_needed(void)
{
	struct nvmeibt_seg_active		*seg_active;
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_praid_topo_ctx	*applied_praid_topo;

	NFIN;
	if (!is_stale_rebuild_enabled) /* For debugging only. Use toma_trace.config to set it */
		goto out;
	if (!nvmeibt_topology_is_HW_config_functional())
		goto out;
	if (NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_dirty_rebuild)) {
		N_Tf(oi98ne5, "Skipping, n_running_dirty_rebuilds=@INT", NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_dirty_rebuild));
		goto out;
	}

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			if (!nvmeibt_seg_active_is_stale_rebuild_required(seg_active) &&
				!nvmeibt_seg_active_is_txid_rebuild_required(seg_active)) {
				continue;	// Not needed or already running properly
			}
			applied_praid_topo = nvmeibt_seg_active_get_praid_applied_topo(seg_active);
			if (!applied_praid_topo->is_activated ||
				!nvmeibt_praid_is_client_sync_cmd_stable(applied_praid_topo->registrants_sync_cmd)) {
				N_Tf(gttt12q, "Skipping seg=@UUID_8, praid cannot perform stale and txid rebuilds now", nvmeibt_seg_active_UUID_8(seg_active));
				continue;
			}

			// The following is inside the loop since N_RUNNING_TASKS might change.
			if (NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_stale_rebuild) + NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_txid_rebuild) >= (int)nvmeibt_recovery_get_max_n_simultaneous_stale_and_txid_rebuild()) {
				N_Tf(guy690f, "Skipping, n_running_stale_rebuild=@INT n_running_txid_rebuild=@INT", NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_stale_rebuild), NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_txid_rebuild));
				goto out;
			}
			// Run recovery thread per this PRAID segment, that resides on this node, and is owner_recoverer
			if (nvmeibt_disk_segment_is_de_facto_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
				if (nvmeibt_seg_active_is_stale_rebuild_required(seg_active))
					nvmeibt_seg_active_stale_rebuild(seg_active);
				else
					nvmeibt_seg_active_txid_rebuild(seg_active);
			} else {
				N_Tf(u784n22, "Skipping seg=@UUID_8 @STR", nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_seg_active_dirty_bits_state_str(seg_active));
			}
		}
	}

out:
	NFOUT;
}

/*
 * When SERJIO runs out of entries, it needs the local client to run
 * garbage-collection on the entries of a given segment. This will only
 * free the clearly free entries that mistakenly seem to be taken.
 * TOMA is involved since currently we use the standard mechanism to
 * ask the local client to run a recovery job (recovery-attach, wait for
 * registrant, and send a RECOVER_START msg.)
 * TOMA does not even require that it works. It makes one attempt and
 * thats it. SERJIO will re-request if things do not work-out well
 */
static void nvmeibt_seg_active_JGC_rebuild(struct nvmeibt_seg_active *seg_active)
{
	u64								tid;
	struct nvmeibt_praid			*praid = nvmeibt_seg_active_get_praid(seg_active);
	struct nvmeibt_disk_segment		*seg;

	//NFIN;

	if (praid && !nvmeibt_praid_is_type_EC(praid)) {
		nvmeibt_seg_active_clear_JGC_rebuild_required(seg_active);
		N_Tf(hya87re, "seg=@UUID_8 is not a part of EC praid, JGC not needed", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	if (seg_active->JGC_rebuild_required_timeout_sec < nvmeibt_global_get_cur_event_start_time().tv_sec) {
		N_Wf(hym7j3e, "seg=@UUID_8 cannot start JGC for 60 sec", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->JGC_rebuild_required_timeout_sec = nvmeibt_global_get_cur_event_start_time().tv_sec + 60;
	}

	seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	if (seg && (seg->seg_follower.applied_seg_lot.seg_topo.dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD) &&
		(timespec_lt(seg_active->local_disk->last_add_time, nvmeibt_global_get_global()->last_apply_time))) {
		nvmeibt_seg_active_clear_JGC_rebuild_required(seg_active);
		if (seg_active->applied_serjio_clean_range_state != SERJIO_CLEAN_RANGE_STATE_IN_WORK) {
			N_Tf(hym23re, "seg=@UUID_8 is DEAD. Serjio may clean it", nvmeibt_seg_active_UUID_8(seg_active));
			seg_active->applied_serjio_clean_range_state = SERJIO_CLEAN_RANGE_STATE_REQUIRED;
			nvmeibt_seg_active_notify_serjio_clean_range(seg_active, false);
		}
		goto out;
	}

	if (!nvmeibt_praid_applied_is_qualify_for_JGC(praid)) {
		N_Tf(hy76tre, "seg=@UUID_8. Skipping JGC", nvmeibt_seg_active_UUID_8(seg_active));
		// Stopping running instance
		if (seg_active->JGC_rebuild_ctx.tid && !nvmeibt_recovery_is_aborting_rebuild(seg_active->JGC_rebuild_ctx.tid)) {
			N_Ef(ws43209, "All running JGC should have been terminated by now seg=@UUID_8 tid=@TID", nvmeibt_seg_active_UUID_8(seg_active), seg_active->JGC_rebuild_ctx.tid);
		}
		goto out;
	}
	// Run unless already running or terminating an old run
	if (seg_active->JGC_rebuild_ctx.tid) {
		N_Tf(bfhs7hs, "JGC_rebuild seg=@UUID_8 already running", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	// Launch a JGC-rebuild

	tid = nvmeibt_recovery_start_rebuild(NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC, seg_active);
	if (tid == -1ULL) {
		goto out;
	}

	nvmeibt_seg_active_clear_JGC_rebuild_required(seg_active);
	NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(omwuias, n_running_JGC_rebuild);
	seg_active->JGC_rebuild_ctx.tid = tid;

	N_Tf(i987y62, "Launch JGC_rebuild seg=@UUID_8 tid=@TID", nvmeibt_seg_active_UUID_8(seg_active), seg_active->JGC_rebuild_ctx.tid);
out:
	//NFOUT;
	return;
}

void nvmeibt_recovery_trigger_local_seg_JGC(char *disk_segment_urn_uuid_str, char *ldisk_id_str)
{
	struct nvmeibt_seg_active 		*seg_active;
	union nvmeib_uuid				seg_uuid;
	struct nvmeibt_ascii_uuid		ldisk_id;
	struct nvmeibt_local_disk		*local_disk;

	NFIN;
	nvmeibt_strlcpy(ldisk_id.str, ldisk_id_str, sizeof(ldisk_id.str));
	nvmeibt_urn_uuid_str_to_union_uuid(&seg_uuid, disk_segment_urn_uuid_str);
	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ldisk_id, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
	seg_active = nvmeibt_find_seg_active_of_specific_local_disk_by_uuid(local_disk, &seg_uuid);
	if (!seg_active) {
		if (!nvmeibt_local_disk_is_ready_for_segments(local_disk)) {
			N_Tf(6gsu39k, "seg=@STR disk=@STR not not is_ready_for_segments", disk_segment_urn_uuid_str, nvmeibt_local_disk_display(local_disk));
		} else {
			N_Wf(g7hksdo, "seg=@STR not found on local_disk=@STR", disk_segment_urn_uuid_str, nvmeibt_local_disk_display(local_disk));
		}
		goto out;
	}
	nvmeibt_seg_active_mark_JGC_rebuild_required(seg_active);
out:
	NFOUT;
}

void nvmeibt_recovery_execute_JGC_rebuilds_as_needed(void)
{
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	if (!nvmeibt_topology_is_HW_config_functional())
		goto out;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		if (!nvmeibt_local_disk_is_ready_for_segments(local_disk) || !nvmeibt_local_disk_is_connected_to_disk(local_disk))
			continue;
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			if (!nvmeibt_seg_active_is_JGC_rebuild_required(seg_active)) {
				continue;	// Not needed or already running properly
			}
			nvmeibt_seg_active_JGC_rebuild(seg_active);
		}
	}

out:
	NFOUT;
}

static int nvmeibt_seg_active_scrubbing(struct nvmeibt_seg_active *seg_active)
{
	u64								tid;
	int								rv = 0;

	NFIN;

	// Launch a background scrub
	tid = nvmeibt_recovery_start_rebuild(NVMEIBT_RECOVERY_TYPE_SCRUBBING, seg_active);
	if (tid == -1ULL) {
		rv = -1;
		goto out;
	}

	NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(fhjitu8, n_running_scrubbing);
	seg_active->scrubbing_ctx.tid = tid;

	N_Tf(7dskm6b, "seg=@UUID_8 tid=@TID scrubbing launched", nvmeibt_seg_active_UUID_8(seg_active), tid);
	upd_scrub_timeout_heap_following_a_change(seg_active);	// Remove the running one from the top of the heap

out:
	NFOUT;
	return rv;
}

void nvmeibt_recovery_execute_scrubbing_as_needed(void)
{
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_seg_active		*seg_active;
	static struct timespec			prev_call_timespec = TIMESPEC_ZERO;
	struct timespec					now;
	struct nvmeibt_praid			*praid;
	struct nvmeibt_praid_lot		*applied_praid_lot;
	struct nvmeibt_seg_lot			*applied_seg_lot;

	NFIN;

	if (!nvmeibt_topology_is_HW_config_functional()) {
		goto out;
	}

	if (!is_scrub_enabled || nvmeibt_recovery_get_max_n_simultaneous_scrubbing() == 0) {
		N_Tf(4gx7ajk, "is_scrub_enabled=@BOOL nax_n_simultaneous_scrubbing=@INT", is_scrub_enabled, (int)nvmeibt_recovery_get_max_n_simultaneous_scrubbing());
		goto out;
	}

	if ((cur_topo->n_running_dirty_rebuild > 0) || (cur_topo->n_running_cold_recovery > 0)) {
		N_Tf(fty7621, "skip n_running_dirty_rebuild=@INT n_running_cold_recovery=@INT",
			 cur_topo->n_running_dirty_rebuild, cur_topo->n_running_cold_recovery);
		goto out;
	}

	getnstimeofday_boot(&now);
	// Instead of using a timeout mechanism, we use the idle_time_activities, and avoid frequent scans
	if (now.tv_sec - prev_call_timespec.tv_sec < SCRUB_PERIODIC_TIMEOUT_SEC) {
		N_Tf(msjf43h, "Too soon after prev call");
		goto out;
	}
	prev_call_timespec.tv_sec = now.tv_sec;

	while (1) { // Try to start scrubbings
		if (cur_topo->n_running_scrubbing >= (int) nvmeibt_recovery_get_max_n_simultaneous_scrubbing()) {
			N_Tf(hyu7tr4, "skip new scrubbing (in progress=@NUM)", cur_topo->n_running_scrubbing);
			break;
		}
again:
		seg_active = (next_scrub_timeout_heap.n_elements ? container_of(nvmeib_heap_peek_top(&next_scrub_timeout_heap), struct nvmeibt_seg_active, my_next_scrub_timeout_heap_element) : NULL);
		if (!seg_active) {
			break;
		}
		if (seg_active->my_next_scrub_timeout_heap_element.timestamp > (unsigned long)now.tv_sec) {
			N_Tf(yfdbsj4, "Not scrubbing, seg=@UUID_8 timestamp @LLD>@LLD", nvmeibt_seg_active_UUID_8(seg_active), seg_active->my_next_scrub_timeout_heap_element.timestamp, now.tv_sec);
			break;
		}
		praid = nvmeibt_disk_segment_get_praid(seg_active->disk_segment);
		if (seg_active->scrubbing_ctx.tid) {
			N_Wf(4cgsyg8, "OOPS scrubbing already running praid=@UUID_LE seg=@UUID_8 (tid=@TID)",
				nvmeibt_praid_UUID(praid), nvmeibt_seg_active_UUID_8(seg_active), seg_active->scrubbing_ctx.tid);
			break;
		}
		applied_praid_lot = nvmeibt_praid_get_applied_praid_lot(praid);
		if (!praid || !(applied_praid_lot->topo_ctx.is_activated)) {
			// N_Tf(5vwys1n, "not activated praid=@PRAID seg=@UUID_8", nvmeibt_praid_UUID(praid), nvmeibt_seg_active_UUID_8(seg_active));
			nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 0);
			goto again;
		}

		if (!nvmeibt_disk_segment_is_de_facto_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
			N_Tf(xbdiemj, "skipping (not topo/owner) seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
			nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 0);
			goto again;
		}

		// Check that the pRAID is nice and stable
		if (!nvmeibt_praid_is_client_sync_cmd_stable(applied_praid_lot->topo_ctx.registrants_sync_cmd)) {
			// N_Tf(chs7wk3, "praid=@STR is_client_sync_cmd_stable", nvmeibt_praid_UUID(praid));
			nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 0);	// In case of failure, do not retry for a while
			goto again;
		} else {	// Scan all its segs
			XDLIST_FOREACH(applied_seg_lot, &applied_praid_lot->all_seg_lot_list) {
				if (!nvmeibt_disk_segment_is_competent_owner(&applied_seg_lot->seg_topo)) {
					// N_Tf(iwjdnxy, "seg=@UUID_8 dirty_bits_state=@STR",
					// 		nvmeibt_seg_UUID_8(disk_segment), dirty_bits_state_str(seg_loop->seg_follower.applied_seg_lot.seg_topo.dirty_bits_state));
					nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 0);	// In case of failure, do not retry for a while
					goto again;
				}
			}
		}
		if (nvmeibt_seg_active_scrubbing(seg_active)) {
			nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 0);
			goto again;
		}
	}

out:
	NFOUT;
}

static void seg_active_done_dirty_rebuild(
				u64 tid, struct nvmeibt_seg_active *seg_active,
				enum NVMEIBT_RECOVERY_STATUS recovery_status)
{
	NFIN;

	if (tid != seg_active->dirty_rebuild_ctx.tid) {
		N_Ef(error_seg_active_nvmeibt_seg_active_recovery_done, "tid=@TID != @TID=dirty_rebuild_ctx.tid", tid, seg_active->dirty_rebuild_ctx.tid);
	}

	nvmeibt_recovery_launch_abort_rebuild(tid);  /* tell recovery to delete the task */
	seg_active->dirty_rebuild_ctx.tid = 0;
	NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(rbsuyg3, n_running_dirty_rebuild);
	if (recovery_status == NVMEIBT_RECOVERY_STATUS_SUCCESS) {
		N_Tf(tyr6542, "Dirty rebuild succeeded seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		if (nvmeibt_seg_active_dirty_bits_state(seg_active) != NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER) {
			N_Wf(sdu7276, "seg=@UUID_8 Surprise RECOVERY_SUCCESS of dirty_bits for @DIRTY_BITS_STATE_STR",
				nvmeibt_seg_active_UUID_8(seg_active),
				nvmeibt_seg_active_dirty_bits_state_str(seg_active));
		}
		if (nvmeibt_seg_active_get_active_praid_version_major(seg_active) == seg_active->dirty_rebuild_ctx.praid_version) {
			seg_active->dirty_rebuild_ctx.praid_version = 0;
			// If indeed this round succeeded then mark as done
			NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(3chsir9, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE);
		}
		nvmeibt_seg_active_clear_txid_rebuild_required(seg_active);
	} else {
		N_Tf(fgy6207, "Dirty rebuild failed seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->dirty_rebuild_ctx.praid_version = 0;
		if (nvmeibt_disk_segment_is_owner_recoverer(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
			N_Tf(dkw0gv3, "status=@STATUS but still a recoverer, marking for retry.", recovery_status);
			nvmeibt_seg_active_mark_dirty_rebuild_required(seg_active);
			NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(x93m328, seg_active);
		}
	}
	NFOUT;
}

static void seg_active_done_txid_rebuild(
				u64 tid, struct nvmeibt_seg_active *seg_active,
				enum NVMEIBT_RECOVERY_STATUS recovery_status)
{
	NFIN;
	if (tid != seg_active->txid_rebuild_ctx.tid) {
		N_Ef(5dpufd7, "tid=@TID != @TID=txid_rebuild_ctx.tid", tid, seg_active->txid_rebuild_ctx.tid);
	}
	nvmeibt_recovery_launch_abort_rebuild(tid);  /* tell recovery to delete the task */
	seg_active->txid_rebuild_ctx.tid = 0;
	NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(jaha7q7, n_running_txid_rebuild);
	if (recovery_status == NVMEIBT_RECOVERY_STATUS_SUCCESS) {
		N_Tf(jdielb0, "txid rebuild succeeded seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
	} else {
		N_Tf(vn5koy2, "txid rebuild failed seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		//seg_active->txid_rebuild_ctx.praid_version = 0;
		nvmeibt_seg_active_mark_txid_rebuild_required(seg_active);
	}
	NFOUT;
}

static void seg_active_done_stale_rebuild(
				u64 tid, struct nvmeibt_seg_active *seg_active,
				enum NVMEIBT_RECOVERY_STATUS recovery_status)
{
	NFIN;
	if (tid != seg_active->stale_rebuild_ctx.tid) {
		N_Ef(5dpuf2m, "tid=@TID != @TID=stale_rebuild_ctx.tid", tid, seg_active->stale_rebuild_ctx.tid);
	}
	nvmeibt_recovery_launch_abort_rebuild(tid);  /* tell recovery to delete the task */
	seg_active->stale_rebuild_ctx.tid = 0;
	NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(jaha72g, n_running_stale_rebuild);
	if (recovery_status == NVMEIBT_RECOVERY_STATUS_SUCCESS) {
		N_Tf(jdiellp, "Stale rebuild succeeded seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
	} else {
		N_Tf(vn5kof4, "Stale rebuild failed seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->stale_rebuild_ctx.praid_version = 0;
		nvmeibt_seg_active_mark_stale_rebuild_required(seg_active);
		NNVMEIBT_SEG_ACTIVE_SET_IS_EXPECTED_TO_HAVE_STALE_LOCKS(6bud83j, seg_active, 1);
	}
	NFOUT;
}

static void seg_active_done_active_JGC_rebuild(
				u64 tid, struct nvmeibt_seg_active *seg_active,
				enum NVMEIBT_RECOVERY_STATUS recovery_status)
{
	NFIN;
	if (tid != seg_active->JGC_rebuild_ctx.tid) {
		N_Ef(9udnwdu, "tid=@TID != @TID=JGC_rebuild_ctx.tid", tid, seg_active->JGC_rebuild_ctx.tid);
	}
	nvmeibt_recovery_launch_abort_rebuild(tid);  /* tell recovery to delete the task */
	seg_active->JGC_rebuild_ctx.tid = 0;
	NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(meixy5x, n_running_JGC_rebuild);
	if (recovery_status == NVMEIBT_RECOVERY_STATUS_SUCCESS) {
		N_Tf(vxoie6n, "JGC rebuild succeeded seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
	} else {
		// no need to rerun. If Serjio is still short of entries it will re-trigger
		N_Tf(ebxjeo5, "JGC rebuild failed seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->JGC_rebuild_ctx.praid_version = 0;
		nvmeibt_seg_active_mark_JGC_rebuild_required(seg_active);
	}
	NFOUT;
}

static void seg_active_done_cold_recovery(
				u64 tid, struct nvmeibt_seg_active *seg_active,
				enum NVMEIBT_RECOVERY_STATUS recovery_status)
{
	NFIN;
	if (tid != seg_active->cold_recovery_ctx.tid) {
		N_Ef(idnngo9, "tid=@TID != @TID=cold_recovery_ctx.tid", tid, seg_active->cold_recovery_ctx.tid);
	}
	nvmeibt_recovery_launch_abort_rebuild(tid);  /* tell recovery to delete the task */
	seg_active->cold_recovery_ctx.tid = 0;
	NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(bjsu474, n_running_cold_recovery);
	if (recovery_status == NVMEIBT_RECOVERY_STATUS_SUCCESS) {
		N_Tf(djyoe4b, "Cold recovery succeeded seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		if (!nvmeibt_disk_segment_is_ec_cold_recoverer(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
			N_Wf(uvs7fom, "seg=@UUID_8 Surprise RECOVERY_SUCCESS of COLD for @DIRTY_BITS_STATE_STR",
				nvmeibt_seg_active_UUID_8(seg_active),
				nvmeibt_seg_active_dirty_bits_state_str(seg_active));
		}
		if (nvmeibt_seg_active_get_active_praid_version_major(seg_active) == seg_active->cold_recovery_ctx.praid_version) {
			seg_active->cold_recovery_ctx.praid_version = 0;
			// If indeed this round succeeded then mark as done
			NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(bxj830m, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE);
		}
	} else {
		N_Tf(jdikr4h, "Cold_recovery failed seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->cold_recovery_ctx.praid_version = 0;
		if (nvmeibt_disk_segment_is_ec_cold_recoverer(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
			N_Tf(uej6m4k, "status=@STATUS but still an ec_cold_recoverer, marking for retry.", recovery_status);
			nvmeibt_seg_active_mark_cold_recovery_required(seg_active);
			NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(behbs71, seg_active);
		}
	}
	NFOUT;
}

static void seg_active_done_scrubbing(
				u64 tid, struct nvmeibt_seg_active *seg_active,
				enum NVMEIBT_RECOVERY_STATUS recovery_status)
{
	NFIN;
	if (tid != seg_active->scrubbing_ctx.tid) {
		N_Ef(e1_seg_active_done_scrubbing,
			"tid=@TID != @TID=scrubbing_ctx.tid", tid, seg_active->scrubbing_ctx.tid);
	}

	nvmeibt_recovery_launch_abort_rebuild(tid);  /* tell recovery to delete the task */
	seg_active->scrubbing_ctx.tid = 0;

	NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(fhuy733, n_running_scrubbing);

	if (recovery_status == NVMEIBT_RECOVERY_STATUS_SUCCESS) {
		seg_active->persistent_metadata->n_blksets_scrubbed += seg_active->scrubbing_iter_n_blksets;
		N_Tf(cgur5w3, "Scrubbing succeeded seg=@UUID_8 blksets:(n=@ZX total=@ZX)",
			nvmeibt_seg_active_UUID_8(seg_active),
			seg_active->scrubbing_iter_n_blksets,
			seg_active->persistent_metadata->n_blksets_scrubbed);

		if (seg_active->persistent_metadata->n_blksets_scrubbed >= num_blksets_in_disk_segment(seg_active->disk_segment)) {
			N_Tf(gyue763, "scrubbing seg=@UUID_8 finished in @ZU days)",
				nvmeibt_seg_active_UUID_8(seg_active), seg_active_scrub_secs_since_start(seg_active) / SCRUB_SECS_IN_DAY);
			seg_active->persistent_metadata->n_blksets_scrubbed = 0; // for the next period
			seg_active_scrub_reset(seg_active, 1);
			// TODO: report to mgmt that we have completed scrubbing cycle
		} else {
			nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 1);
		}

		nvmeibt_register_launch_seg_metadata_ctrl_save(seg_active);	// No logic, if it fails then we lost one checkpoint for the next TOMA restart
	} else {
		N_Tf(po0iw83, "Scrubbing failed seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		nvmeibt_seg_active_scrub_upd_failure_status(seg_active, 0);
	}

	NFOUT;
}

void nvmeibt_seg_active_recovery_done(
				u64 tid, struct nvmeibt_seg_active *seg_active,
				enum NVMEIBT_RECOVERY_TYPE recovery_type,
				enum NVMEIBT_RECOVERY_STATUS recovery_status)
{
	NFIN;
	N_Tf(hugf620, "recovery task done tid=@TID type=@TYPE_STR status=@STATUS_STR seg=@UUID_8", tid,
		nvmeibt_recovery_type_to_str(recovery_type),
		nvmeibt_recovery_status_to_str(recovery_status),
		nvmeibt_seg_active_UUID_8(seg_active));

	switch (recovery_type) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:
		seg_active_done_dirty_rebuild(tid, seg_active, recovery_status);
		break;
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:
		seg_active_done_stale_rebuild(tid, seg_active, recovery_status);
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:
		seg_active_done_txid_rebuild(tid, seg_active, recovery_status);
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
		seg_active_done_active_JGC_rebuild(tid, seg_active, recovery_status);
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
		seg_active_done_cold_recovery(tid, seg_active, recovery_status);
		break;
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:
		seg_active_done_scrubbing(tid, seg_active, recovery_status);
		break;
	default:
		N_Ef(jwicstq, "Unknown recovery_type=@RECOVERY_TYPE", recovery_type);
		nvmeibt_abort(ES_FATAL);
	}

	switch (recovery_status) {
	case NVMEIBT_RECOVERY_STATUS_SUCCESS:
	case NVMEIBT_RECOVERY_STATUS_ABORT:
	case NVMEIBT_RECOVERY_STATUS_ERROR:
		/* sanity check only */
		break;
	default:
		N_Ef(srqpbdu, "Unknown recovery_status=@RECOVERY_STATUS", recovery_status);
		nvmeibt_abort(ES_FATAL);
	}
	NFOUT;
}

/* client disconnect */

void nvmeibt_recovery_set_is_stale_rebuild_enabled(bool is_enabled)
{
	if (is_stale_rebuild_enabled != is_enabled) {
		N_Tf(sei7b9v, "is_stale_rebuild_enabled: @BOOL-->@BOOL", is_stale_rebuild_enabled, is_enabled);
		is_stale_rebuild_enabled = is_enabled;
	}
}

int64_t nvmeibt_recovery_client_batch_n_blksets_get(void)
{
	return (recovery_client_batch_n_blksets);
}

void nvmeibt_recovery_client_batch_n_blksets_set(int64_t batch_size)
{
	if (batch_size != recovery_client_batch_n_blksets) {
		N_Tf(cvsumwl, "recovery_client_batch_n_blksets: @INT64_TD-->@INT64_TD", recovery_client_batch_n_blksets, batch_size);
		recovery_client_batch_n_blksets = batch_size;
		nvmeibt_recovery_notify_all_dirty_rebuilds_of_params_change();
	}
}

void adjust_next_scrub_timeout_heap_following_params_change(void)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_seg_active	*seg_active;

	NFIN;
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			seg_active_scrub_reset(seg_active, 0);
		}
	}
	NFOUT;
}

void nvmeibt_recovery_set_is_scrub_enabled(int64_t is_enabled)
{
	if (is_scrub_enabled != !!is_enabled) {
		N_Tf(v28suzq, "is_scrub_enabled: @BOOL-->@BOOL", is_scrub_enabled, !!is_enabled);
		is_scrub_enabled = !!is_enabled;
		if (is_scrub_enabled)
			adjust_next_scrub_timeout_heap_following_params_change();
	}
}

int64_t nvmeibt_recovery_get_is_scrub_enabled(void)
{
	return is_scrub_enabled;
}

void nvmeibt_recovery_set_scrub_default_period_days(int64_t period_days)
{
	int64_t		new_period_sec = period_days * SCRUB_SECS_IN_DAY;

	if (new_period_sec > INT_MAX) {
		N_Wf(5vshzmq, "invalid large period=@LONG (ignoring)", period_days);
	} else if ((int)new_period_sec != scrub_default_period_sec) {
		N_Tf(5vwhgab, "scrubbing_default_period_days=@LLD", (int) period_days);
		scrub_default_period_sec = new_period_sec;
		adjust_next_scrub_timeout_heap_following_params_change();
	}
	// no need for the below: will be called shortly by idle_time_activities()
	// nvmeibt_recovery_execute_scrubbing_as_needed();
}

int64_t nvmeibt_recovery_get_scrub_default_period_days(void)
{
	return (scrub_default_period_sec / SCRUB_SECS_IN_DAY);
}

void nvmeibt_recovery_set_n_blksets_per_scrub_iteration(int64_t val)
{
	if (val) {
		nvmeibt_recovery_n_blksets_per_scrub_iteration = val;
	} else {
		N_Tf(lwnx8n4, "Cannot set n_blksets_per_scrub_iteration=0");
	}
}

int64_t nvmeibt_recovery_get_n_blksets_per_scrub_iteration(void)
{
	return nvmeibt_recovery_n_blksets_per_scrub_iteration;
}

void nvmeibt_seg_active_set_zeroing_test(bool zeroing_fail)
{
	if (test_zeroing_fail != zeroing_fail) {
		if (zeroing_fail)
			N_Tf(t_xx_71,"Start fail zeroing");
		else
			N_Tf(t_xx_72,"Stop fail zeroing");

		test_zeroing_fail = zeroing_fail;
	}
}

void nvmeibt_seg_active_mark_zeroing_required_as_needed(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (!seg_active) {
		goto out;
	}
	if (!nvmeibt_seg_lot_is_zeroing_explicitly_required_according_to_config(nvmeibt_seg_active_get_applied_seg_lot(seg_active))) {
		// We mark that zeroing is required only if it says so in the mgmt config
		// Just the fact that a seg is missing in config does not mark it for zeroing
		goto out;	// No need for zeroing
	}
	if (nvmeibt_seg_active_is_zeroing_state_skipable(seg_active)) {
		// Already Done / Not_needed (might be due to evicted disk)
		goto out;
	}
	if (!nvmeibt_seg_active_get_disk(seg_active)) {
		// We are not sure about the disk->is_out_of_service, defer the decision
		goto out;
	}
	if (nvmeibt_disk_is_explicitly_out_of_service(nvmeibt_seg_active_get_disk(seg_active))) {
		seg_active->applied_zeroing_state = (nvmeibt_seg_active_is_zeroing_state_WQ_task_in_work(seg_active) ?
											 NVMEIBT_ZEROING_STATE_IN_WORK_CANCELLING : NVMEIBT_ZEROING_STATE_NOT_NEEDED);
		goto out;
	}
	if (!nvmeibt_seg_active_get_metadata_gpt_entry(seg_active)) {
		N_Wf(brus93j, "seg=@UUID_8 not in GPT, no need to zero. Marking as NOT_NEEDED", nvmeibt_seg_active_UUID_8(seg_active));
		mark_seg_active_x_done_if_all_cleaning_works_are_finished(seg_active, 1);
		goto out;
	}
	if (seg_active->applied_zeroing_state == NVMEIBT_ZEROING_STATE_UNINITIALIZED) {
		seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_REQUIRED;
		N_Tf(ahv73hu, "seg=@UUID_8 zeroing_state=@ZEROING_STATE",
			 nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_zeroing_state_str(seg_active->applied_zeroing_state));
	}
out:
	NFOUT;
}

void nvmeibt_seg_active_stop_all_recoveries_and_registrations(struct nvmeibt_seg_active *seg_active, bool is_brute_force_disconnect_required)
{
	struct nvmeibt_disk_segment				*seg;

	NFIN;
	if (!seg_active) {
		goto out;
	}
	seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	N_Tf(04k2ns7, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
	nvmeibt_register_close_seg_active_for_registration(seg_active, is_brute_force_disconnect_required);
	if (nvmeibt_disk_segment_get_seg_active(seg))
		nvmeibt_seg_active_stop_recovery_tasks(seg_active);
out:
	NFOUT;
}

bool nvmeibt_seg_active_is_conf_corrupted(struct nvmeibt_seg_active *seg_active)
{
	return (!nvmeibt_seg_lot_is_config_OK(nvmeibt_seg_active_get_applied_seg_lot(seg_active)) ||
			nvmeibt_praid_is_conf_corrupted(nvmeibt_seg_active_get_praid(seg_active)));
}

void nvmeibt_seg_active_stop_all_recoveries_and_registrations_on_deleted_segs(void)
{
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_disk				*disk;
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	TODO(Work on global->seg_active_post_update_action_list);
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		disk = local_disk->its_disk;
		if (!disk)
			continue;
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			if (nvmeibt_seg_lot_is_X_in_config(nvmeibt_seg_active_get_applied_seg_lot(seg_active))) {
				nvmeibt_seg_active_stop_all_recoveries_and_registrations(seg_active, 1);
				// The previous func can remove the local disk
				if (nvmeibt_disk_get_local_disk(disk) == NULL) {
					N_Tf(nh112cc, "local_disk=@STR was removed", nvmeibt_disk_get_ldisk_id_str(disk));
					break;
				}
			}
		}
	}
	NFOUT;
}

void nvmeibt_seg_active_handle_post_update_actions(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (!seg_active) {
		N_Tf(bhsduye, "seg_active=NULL");
		goto out;
	}
	// Also handle post-update for replaced segs. No harm
	//  What a bug we had (2150) with missing n_active_registrants_on_active_praid_version=0 :)
	if (XDLIST_NULL(&(seg_active->global_seg_active_post_update_action_link))) {
		N_Tf(nd854le, "seg=@UUID_8 no post_update_action_link", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	NVMEIBT_SEG_ACTIVE_CLEAR_ARE_POST_UPDATE_ACTIONS_REQUIRED(rri9003, seg_active);
	N_Tf(gyt65e0, "seg=@UUID_8 dirty_bits_state=@DIRTY_BITS_STATE", nvmeibt_seg_active_UUID_8(seg_active),
		 nvmeibt_seg_active_dirty_bits_state_str(seg_active));
	if (seg_active->persistent_metadata->is_written_on_disk == NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_NOT_ON_DISK) {
		N_Tf(6cbslpl, "seg=@UUID_8 CTRL_BLK_STATUS_NOT_ON_DISK", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	NDUMP_N_ACTIVE_REGISTRANTS(ghu87r5, seg_active);
	// New raid_version_major. Reset registrants counter.
	// Follower accepted RECOVERER
	if (	nvmeibt_disk_segment_is_owner_recoverer(nvmeibt_seg_active_get_active_seg_topo(seg_active)) &&
			nvmeibt_praid_is_sync_cmd_run_dirty_rebuild(nvmeibt_seg_active_get_registrants_sync_cmd(seg_active)) &&
			seg_active->dirty_rebuild_ctx.praid_version < nvmeibt_seg_active_get_active_praid_version_major(seg_active)) {
		N_Tf(sr4530y, "New praid version. Restarting dirty recovery");
		nvmeibt_seg_active_mark_dirty_rebuild_required(seg_active);
	}
	else if (	nvmeibt_disk_segment_is_ec_cold_recoverer(nvmeibt_seg_active_get_active_seg_topo(seg_active)) &&
				nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery_r(nvmeibt_seg_active_get_registrants_sync_cmd(seg_active)) &&
				seg_active->cold_recovery_ctx.praid_version < nvmeibt_seg_active_get_active_praid_version_major(seg_active)) {
		N_Tf(ar5tue3, "New praid version. Restarting cold recovery");
		nvmeibt_seg_active_mark_cold_recovery_required(seg_active);
	}
	N_Tf(t_d1_tomaseg, "seg=@UUID_8 prev_applied_major=@PRAID_VERSION segp_major=@PRAID_VERSION segp_minor=@PRAID_VERSION",
		nvmeibt_seg_active_UUID_8(seg_active),
		seg_active->last_post_update_praid_version_major,
		nvmeibt_seg_active_get_active_praid_version_major(seg_active),
		nvmeibt_seg_active_get_active_praid_version_minor(seg_active));

	nvmeibt_seg_active_mark_zeroing_required_as_needed(seg_active);
	if (seg_active->last_post_update_praid_version_major == nvmeibt_seg_active_get_active_praid_version_major(seg_active)) {
		goto praid_version_minor_actions;
	}
	seg_active->last_post_update_praid_version_major = nvmeibt_seg_active_get_active_praid_version_major(seg_active);
	nvmeibt_seg_active_stop_recovery_tasks(seg_active);
	nvmeibt_seg_active_notify_serjio_if_seg_is_being_deleted(seg_active);

praid_version_minor_actions:
	// If synchronizer
	if (nvmeibt_seg_active_get_active_seg_topo(seg_active)->is_registrants_synchronizer ||
		!nvmeibt_register_is_seg_active_accepting_registrations(seg_active, NULL)) {
		// Not checking for same praid_version_major, since synchronized should be provided by the leader only for the same major
		nvmeibt_register_make_all_seg_active_registrants_sync_praid_topology(seg_active);
		// Just called nvmeibt_register_clients_sync_check_and_act_upon()
	} else {
		nvmeibt_register_clients_sync_check_and_act_upon(seg_active);
	}
out:
	NFOUT;
}


void nvmeibt_seg_active_mark_stale_rebuild_needed_as_needed(struct nvmeibt_seg_active *seg_active, struct nvmeibt_disk_segment_topo_ctx *prev_active_topo)
{
	// Any segment that changes goes through here, and marks its praid as requires_stale_rebuild
	if (	nvmeibt_praid_applied_is_qualify_for_sync_stale(nvmeibt_seg_active_get_praid(seg_active)) &&
			nvmeibt_disk_segment_are_topos_actionably_different(nvmeibt_seg_active_UUID(seg_active), &(seg_active->active_seg_topo), prev_active_topo)) {
		nvmeibt_seg_active_mark_stale_rebuild_required(seg_active);
	}
}

/********************************************************************************************/
/****************                         Zeroing                          ******************/
/********************************************************************************************/

#define SEG_ZERO_REPORT_TO_MGMT_FREQUENCY_SEC 			10

struct zero_seg_active_wq_entry {
	/* Input */
	struct nvmeibt_wq_entry 				wq_entry;
	int										fd;
	struct netlink_io_context				*nl_ctx;
	struct nvmeibt_ldisk_id_for_srvr_cmd	disk;
	struct nvmeibt_seg_active				*seg_active;
	unsigned int							pblk_size;
	uint64_t 								n_4Kblk_zeroed;	/* In/Out */
	int										rv;				/* Output */
};

BOOL nvmeibt_seg_active_is_disk_format_zeroing_done_for_me(struct nvmeibt_seg_active *seg_active)
{
	const struct nvmeibt_local_disk		*its_local_disk;
	const struct nvmeibt_block_device	*its_blkdev;
	BOOL								rv = false;

	// FIN;
	if (!seg_active) {
		goto out;
	}
	if (seg_active->persistent_metadata && seg_active->persistent_metadata->active_praid_version_major > PRAID_VERSION_INVALID_VALUE) {
		// Avoid TOMAerr before parsing the config. active_praid_version_major is a sign that it was already zeroed.
		TODO(Probably doesnt work for JBOD currently, because not saved);
		rv = true;
		goto out;
	}
	its_local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	its_blkdev = nvmeibt_seg_active_get_blkdev(seg_active);

	if (its_blkdev) {
		uint64_t disk_recorded_last_byte_zeroed = nvmeibt_local_disk_get_last_byte_zeroed(its_local_disk);
		uint64_t seg_last_byte = (nvmeibt_seg_active_get_seg_mgmt(seg_active)->lb_e + 1) * its_blkdev->from_config.blk_size_bytes - 1;

		N_Tf(djdju82, "seg=@UUID_8 disk=@STR disk_recorded_last_byte_zeroed=@DISK_LAST_BYTE_ZEROED seg_last_byte=@SEG_LAST_BYTE",
			nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_local_disk_display(its_local_disk), disk_recorded_last_byte_zeroed, seg_last_byte);
		if (disk_recorded_last_byte_zeroed >= seg_last_byte) {
			rv = true;
		}
	}
	else {
		N_Wf(ee5t184, "seg=@UUID_8 no blkdev", nvmeibt_seg_active_UUID_8(seg_active));
	}
out:
	N_Tf(we43772, "seg=@UUID_8 are_blks_zeroed=@ARE_BLKS_ZEROED", nvmeibt_seg_active_UUID_8(seg_active), rv);
	// FOUT;
	return rv;
}

static int zero_pblk_range_of_seg_active(struct nvmeibt_seg_active *seg_active, struct zero_seg_active_wq_entry *entry,
										 uint64_t pba_s, uint64_t n_pblk)
{
	int 		rv = 0;

	NFIN;
	NTOMA_ASSERT(vshgsdy, (long long int)n_pblk > 0, "seg=@UUID_8 n_pblk=@ZX", nvmeibt_seg_active_UUID_8(seg_active), n_pblk);
	rv = nvmeibt_zero_disk_pblks(&entry->disk, pba_s, n_pblk, false);
	if (rv < 0) {
		N_Ef(5sye9nw, "Unable to zero seg=@UUID_8 pba_s=@ZX n_blks=@ZX",
			 nvmeibt_seg_active_UUID_8(seg_active), pba_s, n_pblk);
	}
	NFOUT;
	return rv;
}

static uint64_t calc_n_4Kblks_to_zero(struct nvmeibt_seg_active *seg_active, struct nvmeibt_ldisk_id_for_srvr_cmd *disk)
{
	struct nvmeibt_seg_mgmt						*seg_mgmt = nvmeibt_seg_active_get_seg_mgmt(seg_active);
	uint64_t									seg_n_4Kblks;
	uint64_t									max_n_4Kblks_to_zero;
	uint64_t									n_4Kblks_to_zero;
	uint64_t									start_4Kblk_to_zero;
	uint64_t    								iteration_ratio;
	const struct nvmeibt_disk_flow_params_t 	*p;

	start_4Kblk_to_zero = (uint64_t)(seg_mgmt->lb_s) + seg_active->n_4Kblk_zeroed;
	seg_n_4Kblks = seg_mgmt->lb_e - seg_mgmt->lb_s + 1;
	p = nvmeibt_disk_flow_params_get(disk->Model, true);
	iteration_ratio = (p->is_zeroing_mandatory) ? RATIO_TO_ZERO_PER_ITERATION : RATIO_TO_TRIM_PER_ITERATION;
	max_n_4Kblks_to_zero = max((seg_n_4Kblks / iteration_ratio), (uint64_t)(MIN_N_BYTES_TO_ZERO_PER_ITERATION / 4096));
	n_4Kblks_to_zero = min(max_n_4Kblks_to_zero, (uint64_t)(seg_mgmt->lb_e) - start_4Kblk_to_zero + 1);
	n_4Kblks_to_zero = nvmeibt_align_n_blks_to_write_to_blkset(start_4Kblk_to_zero, n_4Kblks_to_zero, 4096);

	//N_Tf(ahu7j31, "seg=@UUID_8 zeroing n_4Kblks=@UINT64_TX total 4Kblks=@UINT64_TX", nvmeibt_seg_active_UUID_8(seg_active), n_4Kblks_to_zero, seg_n_4Kblks);

	return n_4Kblks_to_zero;
}

static void seg_active_zeroing_finalize(struct nvmeibt_wq_entry *wq_entry);
static void seg_active_zeroing_freer(struct nvmeibt_wq_entry *wq_entry);

static void seg_active_zeroing_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct zero_seg_active_wq_entry		*entry;
	struct nvmeibt_seg_active			*seg_active = NULL;
	struct nvmeibt_seg_mgmt				*seg_mgmt;
	int 		entry_rv = 0;
	uint64_t	seg_n_4Kblks;
	uint64_t	n_4Kblks_to_zero;
	uint64_t	start_4Kblk_to_zero;
	uint64_t	pba_s;
	uint64_t	n_pblk;

	NFIN;

	entry = container_of(wq_entry, struct zero_seg_active_wq_entry, wq_entry);

	seg_active = entry->seg_active;
	if (seg_active == NULL) {
		N_Ef(sr65489, "seg_active=NULL");
		entry_rv = -1;
		goto out;
	}
	seg_mgmt = nvmeibt_seg_active_get_seg_mgmt(seg_active);

	// Perform segment zeroing, from last checkpoint onwards. (In case of TOMA failover, disk segment will start from scratch..)
	start_4Kblk_to_zero = (uint64_t)(seg_mgmt->lb_s) + entry->n_4Kblk_zeroed;
	seg_n_4Kblks = seg_mgmt->lb_e - seg_mgmt->lb_s + 1;
	n_4Kblks_to_zero = calc_n_4Kblks_to_zero(seg_active, &entry->disk);
	N_Tf(ahu7231, "seg=@UUID_8 zeroing lba from=@UINT64_TX n_blks=@UINT64_TX total seg 4Kblks=@UINT64_TX",
		 nvmeibt_seg_active_UUID_8(seg_active), start_4Kblk_to_zero, n_4Kblks_to_zero, seg_n_4Kblks);

	// We need to convert the 4KB segment logical blocks to disk physical blocks in order to issue the zero range command.
	pba_s = start_4Kblk_to_zero * 4096 / entry->pblk_size;
	n_pblk = n_4Kblks_to_zero * 4096 / entry->pblk_size;
	entry_rv = zero_pblk_range_of_seg_active(seg_active, entry, pba_s, n_pblk);
	if (entry_rv < 0) {
		N_Ef(kri58aq, "Unable to zero seg=@UUID_8 pblks from=@UINT64_TX n_blks=@UINT64_TX total seg 4Kblks=@UINT64_TX",
			 nvmeibt_seg_active_UUID_8(seg_active), start_4Kblk_to_zero, n_4Kblks_to_zero, seg_n_4Kblks);
		goto out;
	}

	entry->n_4Kblk_zeroed += n_4Kblks_to_zero;

	// When done, also zero the metadata, except for the first blk (the control block)
	if (entry->n_4Kblk_zeroed >= seg_n_4Kblks) {
		N_Tf(vcghsat, "seg=@UUID_8 Done zeroing of data. Zeroing metadata (leaving metadata CTRL untouched)", nvmeibt_seg_active_UUID_8(seg_active));
		N_Tf(rxc2nxa, "metadata_gpt_entry->pba_s=@UINT64_TX sizeof=@SIZE_T pblk_size=@UINT pba_s=@UINT64_TX", seg_active->metadata_gpt_entry->pba_s, sizeof(*(seg_active->persistent_metadata)), entry->pblk_size, pba_s);
		pba_s = seg_active->metadata_gpt_entry->pba_s + (sizeof(*(seg_active->persistent_metadata)) / entry->pblk_size);	// Skip the metadata 4K
		entry_rv = zero_pblk_range_of_seg_active(seg_active, entry,
												 pba_s,
												 seg_active->metadata_gpt_entry->pba_e - pba_s + 1);
	}

out:
	entry->rv = entry_rv;
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void seg_active_zeroing_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct zero_seg_active_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct zero_seg_active_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_seg_active_zeroing_freer, entry);

	NFOUT;
}

static void launch_seg_active_zero_task(struct nvmeibt_seg_active *seg_active, uint64_t n_4Kblk_zeroed, struct netlink_io_context *nl_ctx, int fd, int last_CHANGE_no)
{
	struct zero_seg_active_wq_entry		*seg_zero_task;
	struct nvmeibt_local_disk			*its_local_disk;

	NFIN;
	nvmeibt_seg_active_mark_zeroing_required_as_needed(seg_active);	// Recheck everything that might have changed
	its_local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	if (nvmeibt_local_disk_is_being_deleted(its_local_disk)) {
		N_Tf(ey75y23, "local_disk=@STR is_being_deleted. Skipping", nvmeibt_local_disk_display(its_local_disk));
		goto out;
	}
	if (!nvmeibt_seg_active_is_zeroing_state_launchable(seg_active)) {
		goto out;
	}
	// Temporary statuses, that should change soon
	if (!nvmeibt_local_disk_is_ready_for_segments(its_local_disk)) {
		N_Wf(vw6y3h4, "seg=@UUID_8 disk !is_ready_for_segments, not launching zeroing task to avoid data corruption", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	if (nvmeibt_register_is_any_registered_on_seg_active(seg_active)) {
		N_Tf(vertshg, "seg=@UUID_8 Skipping, has registrants", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	//
	N_Tf(qw43er7, "seg=@UUID_8 next zeroing batch - Already zeroed @UINT64_TX", nvmeibt_seg_active_UUID_8(seg_active), n_4Kblk_zeroed);
	seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_IN_WORK;

	// Allocate task to offload writing leader_name to a thread.
	seg_zero_task = NNVMEIBT_BM_CALLOC(ty78u22, sizeof(*seg_zero_task));

	NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(uy777sd, seg_active, "ZEROING", 1);
	seg_zero_task->wq_entry.type = "ZERO_DISK_SEGMENT";
	seg_zero_task->wq_entry.execute = seg_active_zeroing_wrapper;
	seg_zero_task->wq_entry.finalize = seg_active_zeroing_finalize;
	seg_zero_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	seg_zero_task->wq_entry.free = seg_active_zeroing_freer;
	seg_zero_task->wq_entry.last_CHANGE_no = last_CHANGE_no;
	seg_zero_task->seg_active = seg_active;
	if (fd) {
		seg_zero_task->fd = fd;
		seg_zero_task->nl_ctx = nl_ctx;
	}
	else {
		seg_zero_task->fd = nvmeibt_local_disk_dev_file_fd(its_local_disk);
		seg_zero_task->nl_ctx = nvmeibt_local_disk_dev_nl_ctx(its_local_disk);
	}
	seg_zero_task->disk = nvmeibt_local_disk_config_to_srvr_cmd_disk(&its_local_disk->from_config);
	seg_zero_task->n_4Kblk_zeroed = n_4Kblk_zeroed;
	seg_zero_task->pblk_size = nvmeibt_local_disk_pblk_size(its_local_disk);
	// Copy the node name to the ofloading task, in case for some reason the leader node will be deleted, when we are doing the actual write.

	if (nvmeibt_toma_segment_zeroing_add_work(its_local_disk, &(seg_zero_task->wq_entry)) != 0) {
		N_Ef(dji85h3, "Unable to add segment zeroing offload task to WQ for seg=@UUID_8!", nvmeibt_seg_active_UUID_8(seg_active));
		NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(uy788sd, seg_active, "ZEROING", -1);
		NNVMEIBT_BM_FREE(i9i983d, seg_zero_task);
		seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_REQUIRED;
	}
out:
	NFOUT;
}

static void seg_active_zeroing_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct zero_seg_active_wq_entry				*entry;
	struct nvmeibt_seg_mgmt						*seg_mgmt;
	struct nvmeibt_seg_active					*seg_active;
	BOOL										is_done_zeroing;
	struct timespec								now;
	struct timespec								diff_timeout;
	uint64_t									n_4Kblk_zeroed;
	struct nvmeibt_Str							*json_payload = NULL;
	char 										unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN] = "seg_zeroing";

	NFIN;

	entry = container_of(wq_entry, struct zero_seg_active_wq_entry, wq_entry);
	seg_active = entry->seg_active;

	/* if wq_entry was canceled, set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		N_Tf(dju85y3, "canceled zeroing seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		entry->rv = -1;
	}

	if (!seg_active) {
		N_Ef(ajiw43e, "seg_active=NULL");
		nvmeibt_abort(ES_FATAL);
	}
	NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(uy799sd, seg_active, "ZEROING", -1);
	if (nvmeibt_seg_active_final_free_if_not_in_use(seg_active))
		goto out;

	if (nvmeibt_seg_active_is_zeroing_state_skipable(seg_active)) {
		N_Tf(cvdgey8, "seg=@UUID_8 zeroing is canceled", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_NOT_NEEDED;
		goto out;
	}

	seg_mgmt = nvmeibt_seg_active_get_seg_mgmt(seg_active);
	if (entry->rv < 0 || test_zeroing_fail) {
		if (test_zeroing_fail) {
			N_Tf(dju87e4, "Simulate zeroing fail seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		}
		else {
			N_Ef(aki93ge, "Error zeroing seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		}
		seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_REQUIRED;
		goto out;
	}

	if (!nvmeibt_seg_active_get_local_disk(seg_active)) {
		N_Wf(xbhyet4, "seg=@UUID_8 has no local_disk, probably is during advanced delete stages already", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	n_4Kblk_zeroed = entry->n_4Kblk_zeroed;
	is_done_zeroing = (seg_mgmt->lb_s + n_4Kblk_zeroed >= seg_mgmt->lb_e + 1);
	if (is_done_zeroing) {
		N_Tf(dju86u4, "Done zeroing seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		nvmeibt_seg_active_mark_as_fully_zeroed(seg_active);
	}
	seg_active->n_4Kblk_zeroed = n_4Kblk_zeroed; // for status reports only

	getnstimeofday_boot(&now);
	diff_timeout = timespec_sub(now, seg_active->last_zeroing_progress_report_time);

	if (is_done_zeroing || (diff_timeout.tv_sec > SEG_ZERO_REPORT_TO_MGMT_FREQUENCY_SEC)) {
		N_Tf(ko9eu65, "Should send zeroing progress update for seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		nvmeibt_strlcpy(unique_key + strlen(unique_key), nvmeibt_seg_active_id_str(seg_active), sizeof(unique_key) - strlen(unique_key));
		json_payload = NNVMEIBT_STR_ALLOC(vb659sl);
		nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT "\"payload\": {\"praidVersion\": \"%d.%d\", \"segmentUUID\": \"%s\", \"pRaidUUID\": \"%s\", \"nZeroedBlks\": %llu}}",
				KAFKA_PRODUCER_MSG_HEADER_VAR("segmentZeroingProgress", 1),
							nvmeibt_seg_active_get_active_praid_version_major(seg_active),
							nvmeibt_seg_active_get_active_praid_version_minor(seg_active),
							nvmeibt_seg_active_id_str(seg_active),
							nvmeibt_seg_active_praid_id_str(seg_active),
							n_4Kblk_zeroed);
		N_Tf(5msp64f, "@STR", nvmeibt_Str_str(json_payload));
		nvmeibt_kafka_outgoing_msgs_queue_add(unique_key, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
		NNVMEIBT_STR_FREE(bfhbeu3, json_payload);
		seg_active->last_zeroing_progress_report_time = now;
	}
	else {
		N_Tf(aki9h45, "Not sending zeroing progress for seg=@UUID_8, seconds from last update=@LLD", nvmeibt_seg_active_UUID_8(seg_active), diff_timeout.tv_sec);
	}

	if (seg_mgmt->lb_s + n_4Kblk_zeroed > seg_mgmt->lb_e + 1) {
		N_Ef(error_3_seg_active_zeroing_finalize, "seg=@UUID_8 Zeroed too much zeroed_till=@ZEROED_TILL lb_e=@LB_E", nvmeibt_seg_active_UUID_8(seg_active),
			seg_mgmt->lb_s + n_4Kblk_zeroed, seg_mgmt->lb_e);
		nvmeibt_abort(ES_FATAL);
	}

	if (!is_done_zeroing) { // Zeroing not completed.. start the next itteration.
		if (	nvmeibt_disk_segment_is_x_zero(nvmeibt_seg_active_get_active_seg_topo(seg_active)) ||
				seg_active->applied_zeroing_state == NVMEIBT_ZEROING_STATE_IN_WORK) {	// Ignore bogus states. Complete the work.
			launch_seg_active_zero_task(seg_active, n_4Kblk_zeroed, entry->nl_ctx, entry->fd, wq_entry->last_CHANGE_no);
		}
	}

out:
	NFOUT;
}

void nvmeibt_seg_active_mark_as_fully_zeroed(struct nvmeibt_seg_active *seg_active)
{
	if (seg_active) {
		seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_DONE;
		mark_seg_active_x_done_if_all_cleaning_works_are_finished(seg_active, 0);
	}
}

void nvmeibt_seg_active_zero_if_is_being_deleted_and_unused(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_local_disk		*local_disk;

	NFIN;
	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(vgsye6g, "disk=@STR is_being_deleted. Skipping", nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	if (!nvmeibt_disk_segment_is_x_zero(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
		N_Tf(6bw73la, "seg=@UUID_8 state=@STATE_STR not X_ZERO, Nothing to do",
			nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_seg_active_dirty_bits_state_str(seg_active));
		goto out;
	}

	if (seg_active->applied_zeroing_state == NVMEIBT_ZEROING_STATE_REQUIRED) {
		if (nvmeibt_disk_metadata_deprecate_entry_in_mem_gpt(&local_disk->main_gpt, nvmeibt_seg_active_UUID(seg_active)) == 0) {
			NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(hus3bj1, local_disk);
		}
		launch_seg_active_zero_task(seg_active, 0, NULL, 0, 0);
	}

out:
	NFOUT;
}

/********************************************************************************************/
/****************      Functions that go over (all/local) seg_actives      ******************/
/********************************************************************************************/

BOOL nvmeibt_seg_active_is_any_seg_active_during_metadata_store(void)
{
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_seg_active		*seg_active;
	bool							is_during_metadata_store = 0;

	NFIN;
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			if (nvmeibt_seg_active_is_during_persistency_store(seg_active)) {
				is_during_metadata_store = 1;
				goto out;
			}
		}
	}

out:
	NFOUT;
	return is_during_metadata_store;
}

void nvmeibt_seg_active_launch_store_of_all_seg_actives_metadata(void)
{
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			if (!nvmeibt_disk_segment_is_competent_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
				N_Tf(5v6djiw, "seg=@UUID_8 Skipping metadata_store - not a competent owner", nvmeibt_seg_active_UUID_8(seg_active));
			} else if (!nvmeibt_disk_segment_is_mem_tbl_init_done_fully(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
				N_Tf(sbomfh3, "seg=@UUID_8 Skipping metadata_store (for this shutdown) dirty_init_mode=@STR",
					 nvmeibt_seg_active_UUID_8(seg_active), mem_tbl_init_mode_str(nvmeibt_seg_active_get_active_seg_topo(seg_active)->dirty_bits_init_mode));
			} else {
				nvmeibt_ds_metadata_store_on_shutdown(seg_active);
			}
		}
	}
	NFOUT;
}

void nvmeibt_seg_active_generate_all_segs_topo_for_clients(void)
{
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	XDLIST_FOREACH_SAFE(seg_active, &(nvmeibt_global_get_global()->seg_active_post_update_action_list)) {
		regenerate_topo_for_clients(seg_active);
	}
	NFOUT;
}

/******************************************************************************/

void nvmeibt_seg_active_upd_active_topo_from_applied_topo(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment_topo_ctx		prev_active_topo;
	struct nvmeibt_disk_segment_topo_ctx		*active_topo;
	struct nvmeibt_disk_segment_topo_ctx		*applied_topo;
	struct nvmeibt_praid_topo_ctx				*praid_topo;
	struct nvmeibt_seg_lot						*seg_lot;
	struct nvmeibt_local_disk					*local_disk;
	bool										is_accepting_registrations;

	NFIN;
	praid_topo = &(nvmeibt_seg_active_get_praid(seg_active)->praid_follower.applied_praid_lot.topo_ctx);
	active_topo = &(seg_active->active_seg_topo);
	applied_topo = get_applied_seg_topo(seg_active);
	seg_lot = nvmeibt_seg_active_get_applied_seg_lot(seg_active);
	prev_active_topo = *active_topo;
	NTOMA_ASSERT(gd0n2rz, applied_topo && active_topo, "applied_topo=@PTR active_topo=@PTR", applied_topo, active_topo);
	if (	active_topo->seg_praid_version_major == applied_topo->seg_praid_version_major &&
			active_topo->seg_praid_version_minor == applied_topo->seg_praid_version_minor) {
		N_Tf(yutu875, "The same praid_ver=(@PRAID_VERSION,@PRAID_VERSION), ignoring", applied_topo->seg_praid_version_major, applied_topo->seg_praid_version_minor);
		goto out;
	}
	// Always update the received praid_version - we got it, and are going to accept as much as we can
	// - Either we really accept all the other topo values which make sense
	// - Or we reject the topo, but still "goto mark_post_update_actions_required", and have to report to the leader with the latest praid_version
	// - Or we goto out, so we do not care
	if (active_topo->seg_praid_version_major != applied_topo->seg_praid_version_major)
		seg_active->n_active_registrants_on_active_praid_version = 0;
	active_topo->seg_praid_version_major = applied_topo->seg_praid_version_major;
	active_topo->seg_praid_version_minor = applied_topo->seg_praid_version_minor;
	//
	seg_active->dirty_next_unfixed_lock_blkset_no = 0;	// On every topo change, forget the progress in dirty_rebuild

	if (!nvmeibt_disk_segment_leader_is_state_progressible(active_topo) && nvmeibt_disk_segment_leader_is_state_progressible(applied_topo)) {
		N_Tf(d9k32bf, "seg=@UUID_8 active=@STR not updating to applied=@STR",
			 nvmeibt_seg_active_UUID_8(seg_active),
			 dirty_bits_state_str(active_topo->dirty_bits_state),
			 dirty_bits_state_str(applied_topo->dirty_bits_state));
		goto mark_applied_post_update_actions_required;
	}
	if (!(praid_topo->is_activated)) {
		// Since is_new_global_topology_received, this is a new !is_activated. Need to disconnect registrants
		// The topology was distributed just to followers' persistency.
		// This praid's topo is not applicable. Leave the current applied, even if not usable
		N_Tf(j5smbx9, "praid not activated. Leaving active topo as is");
		goto mark_applied_post_update_actions_required;	//  Need to disconnect registrants for evict
	}
	if (nvmeibt_disk_segment_is_x(active_topo)) {
		// If the active is already X_ZERO or X_DONE then do not go backwards due to leader decision.
		// The issue is that we might read the X from config that the leader didn't yet get
		TODO(Do not make topo changes, such as switch to X_ZERO based on config. wait for leader decision);
		if (nvmeibt_disk_segment_is_x_done(applied_topo)) {
			// In practice if already X, then only accept X_DONE (the mgmt. already received X_DONE from us in the past)
			mark_seg_active_x_done_if_all_cleaning_works_are_finished(seg_active, 1);
		}
		else {
			N_Tf(ekc03bc, "Ignoring @UUID_8 @DIRTY_BITS_STATE_STR",
				 nvmeibt_seg_active_UUID_8(seg_active), dirty_bits_state_str(applied_topo->dirty_bits_state));
		}
		goto mark_applied_post_update_actions_required;
	}

	if (!(applied_topo->owner_seg_lot)) {
		if (	nvmeibt_praid_is_client_sync_cmd_delete(nvmeibt_seg_active_get_registrants_sync_cmd(seg_active)) ||
				nvmeibt_seg_lot_is_deleted_in_config(seg_lot) ||	// Plain deletion
				(nvmeibt_disk_segment_is_x(applied_topo) && nvmeibt_seg_lot_is_replaced_in_config(seg_lot)) ||     // Started deprecation
				(!nvmeibt_disk_segment_is_dirty_bits_state_calculated(applied_topo->dirty_bits_state) && nvmeibt_seg_lot_is_substitution_in_config(seg_lot))) {	// Not yet involved
			// OK. No real need for an owner
		} else {
			N_Ef(f4b2ljs, "@UUID_8 - no owner", nvmeibt_seg_active_UUID_8(seg_active));
		}
	}
	nvmeibt_disk_segment_dump(seg_lot->my_seg);
	// Always update the owner(s)
	NVMEIBT_SEG_TOPO_SET_OWNER(u5be8xb, nvmeibt_seg_lot_UUID_8(seg_lot), active_topo, applied_topo->owner_seg_lot);
	active_topo->secondary_owner_seg_lot = applied_topo->secondary_owner_seg_lot;
	active_topo->is_registrants_synchronizer = applied_topo->is_registrants_synchronizer;
	// Always update the dirty_bits/stale_locks init_mode
	if (	(applied_topo->dirty_bits_init_mode != active_topo->dirty_bits_init_mode || applied_topo->stale_locks_init_mode != active_topo->stale_locks_init_mode) &&
			nvmeibt_seg_active_is_mem_tbl_init_meaningful(applied_topo)) {	// seg_active only takes commands etc.. Ignore INIT_DONE
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(t0b4ks, seg_active, applied_topo->dirty_bits_init_mode);
		NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(on406xe, seg_active, applied_topo->stale_locks_init_mode);
		NNVMEIBT_SEG_ACTIVE_SET_TXID_INIT_MODE(xmo031h, seg_active, applied_topo->txid_init_mode);
	} else {
		N_Tf(rbshx5m, "Ignoring dirty_init=@STR(was @STR) stale_init=@STR",
			 mem_tbl_init_mode_str(applied_topo->dirty_bits_init_mode), mem_tbl_init_mode_str(active_topo->dirty_bits_init_mode), mem_tbl_init_mode_str(applied_topo->stale_locks_init_mode));
	}
	// A local seg (according to config) might be dead
	//  when we failed to read smart counters.
	// Take care of the cases, where the active_seg_topo is more knowledgable
	// Refuse to kill a locally live disk
	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	if (	local_disk->was_last_read_of_smart_counters_successful &&
			(applied_topo->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD | NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN))) {
		// Probably just started following a degraded-mode without me
		N_Tf(rguc7m4, "@DIRTY_BITS_STATE_STR-->@DIRTY_BITS_STATE_STR for a local seg. Moving applied to ALIVE_UNSTABLE",
			dirty_bits_state_str(active_topo->dirty_bits_state), dirty_bits_state_str(applied_topo->dirty_bits_state));
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(bd3gs7q, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE);
		goto mark_applied_post_update_actions_required;
	}

	if (active_topo->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED) {
		N_Tf(6fbdkdw, "Ignoring dirty_bits_state=@STR-->@STR since active_init_mode=@STR",
			 dirty_bits_state_str(active_topo->dirty_bits_state), dirty_bits_state_str(applied_topo->dirty_bits_state), mem_tbl_init_mode_str(active_topo->dirty_bits_init_mode));
		goto mark_applied_post_update_actions_required;
	}
#if 1
	if ((nvmeibt_disk_segment_is_owner_recoverer_done(active_topo) ||
		 nvmeibt_disk_segment_is_ec_cold_recoverer_done(active_topo)) &&
		(prev_active_topo.seg_praid_version_major == praid_topo->praid_version_major)) {
      // Do not override the follower's all kinds of "RECOVERER_DONE" with the same major version
		goto mark_applied_post_update_actions_required;
	}
#endif
	NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(w0c2nh4, seg_active, applied_topo->dirty_bits_state);
	nvmeibt_seg_active_mark_stale_rebuild_needed_as_needed(seg_active, &prev_active_topo);

mark_applied_post_update_actions_required:
	nvmeibt_seg_active_reset_serjio_clean_range_state_on_new_config_or_topo(seg_active);
	NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(x9w9w83, seg_active);
	// Set the are_praid_registrants_synced, for non-synchronizer, where all is good
	is_accepting_registrations = nvmeibt_register_is_seg_active_accepting_registrations(seg_active, NULL);
	TODO(Is the following set_registrants_aligned_with_sync_cmd needed? If needed then why not check for real?);
	nvmeibt_seg_active_set_registrants_aligned_with_sync_cmd(seg_active, (!(applied_topo->is_registrants_synchronizer) ||
																		  is_accepting_registrations));	// Not registrable --> Need to UNREG all registrants, for evict, and regardless
	if (nvmeibt_disk_segment_is_competent_owner(&prev_active_topo) && nvmeibt_disk_segment_is_competent_owner(active_topo)) {
		// I was owner in applied, and owner now. I.e., Keeping the old stale & dirty in mem.
		// Validate that the init_mode does not erase stale or dirty bits from memory
		if (nvmeibt_disk_segment_is_init_mode_turning_off(active_topo, nvmeibt_seg_active_is_EC(seg_active))) {
			if (	!is_accepting_registrations &&
				   /*nvmeibt_praid_is_client_sync_cmd_capable_to_INIT_TURN_OFF_on_owners(praid_topo_ctx->registrants_sync_cmd) &&*/
					(nvmeibt_disk_segment_is_ec_cold_recoverer(active_topo) ||
					 nvmeibt_disk_segment_is_init_mode_turning_off(applied_topo, nvmeibt_seg_active_is_EC(seg_active)))) {
				// We are good. either EC_cold_recoverer (that should turn off), or
				//  the prev topo was turning_off (implying no I/O since), so we do not mind turning off again
			} else {
				N_Ef(u4h6gsk, "seg=@UUID_8 dirty_bits_state=@DIRTY_BITS_STATE-->@DIRTY_BITS_STATE_STR "
					"praid_version=@PRAID_VERSION.@PRAID_VERSION "
					"dirty_bits_init_mode=@DIRTY_BITS_INIT_MODE stale_locks_init_mode=@STALE_LOCKS_INIT_MODE txid_init_mode=@TXID_INIT_MODE",
					nvmeibt_seg_active_UUID_8(seg_active),
					dirty_bits_state_str(prev_active_topo.dirty_bits_state), dirty_bits_state_str(active_topo->dirty_bits_state),
					active_topo->seg_praid_version_major, active_topo->seg_praid_version_minor,
					mem_tbl_init_mode_str(active_topo->dirty_bits_init_mode), mem_tbl_init_mode_str(active_topo->stale_locks_init_mode),
					mem_tbl_init_mode_str(active_topo->txid_init_mode));
				nvmeibt_abort(ES_FATAL);
			}
		}
	}
	nvmeibt_seg_active_mark_serialize_active_topo_for_leader_required(seg_active);

out:
	NFOUT;
}

struct nvmeibt_seg_active *nvmeibt_find_seg_active_of_specific_local_disk_by_uuid(struct nvmeibt_local_disk *local_disk, const union nvmeib_uuid *seg_uuid)
{
	return (local_disk ? nvmeib_hash_search_uuid(local_disk->seg_active_hash_by_uuid, seg_uuid) : NULL);
}

struct nvmeibt_seg_active *nvmeibt_find_seg_active_on_all_local_disks_by_uuid(const union nvmeib_uuid *seg_uuid)
{
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_seg_active		*seg_active = NULL;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		seg_active = nvmeibt_find_seg_active_of_specific_local_disk_by_uuid(local_disk, seg_uuid);
		if (seg_active) {
			break;
		}
	}
	return seg_active;
}

bool nvmeibt_seg_active_send_one_seg_rebuild_progress_report_to_mgmt(struct nvmeibt_seg_active *seg_active, struct nvmeibt_Str *json_payload)
{
	bool									is_any_written = 0;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx;
	char									unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN] = "Dirty_progress_";

	seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	if (nvmeibt_disk_segment_is_any_ec_cold_recoverer(seg_topo_ctx)) {
		N_IMf(74v46dc, "------- Need to add cold_recovery reports -------");
	}
	if (	(nvmeibt_disk_segment_is_any_hot_recoverer(seg_topo_ctx)) &&
			 seg_active->dirty_rebuild_ctx.prev_report_n_blksets_remaining != seg_active->dirty_rebuild_ctx.n_blksets_remaining) {
		seg_active->dirty_rebuild_ctx.prev_report_n_blksets_remaining = seg_active->dirty_rebuild_ctx.n_blksets_remaining;
		nvmeibt_Str_reuse(json_payload);
		nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT
							"\"payload\": {\"segmentsDirtyBitsUpdate\": [{"
							"\"pRaidMinorVersion\": %d, \"pRaidMajorVersion\": %d, \"segmentID\": \"%s\", \"pRaidUUID\": \"%s\", \"remainingDirtyBits\": %lld, \"reappearingCounter\": %lld"
							"}]}"
							"}",
							KAFKA_PRODUCER_MSG_HEADER_VAR("updateDiskSegmentsDirtyBits", 1),
							nvmeibt_seg_active_get_active_praid_version_minor(seg_active),
							nvmeibt_seg_active_get_active_praid_version_major(seg_active),
							nvmeibt_seg_active_id_str(seg_active),
							nvmeibt_seg_active_praid_id_str(seg_active),
							seg_active->dirty_rebuild_ctx.prev_report_n_blksets_remaining,
							nvmeibt_seg_active_get_local_disk(seg_active)->reappearing_counter);
		N_Tf(c8as76s, "@STR", nvmeibt_Str_str(json_payload));
		nvmeibt_strlcpy(unique_key + strlen(unique_key), nvmeibt_seg_active_id_str(seg_active), sizeof(unique_key) - strlen(unique_key));
		nvmeibt_kafka_outgoing_msgs_queue_add(unique_key, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_LOW);
		is_any_written = 1;
	}
	return is_any_written;
}

/**************************         Status           **************************/

int nvmeibt_seg_active_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_seg_active *seg_active, bool is_full_info_needed)
{
	char									progress_str[127] = "";
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo;

	NFIN;
	if (!seg_active) {
		N_Ef(t_d9_tomaseg, "seg_active=NULL");
		goto out;
	}
	seg_topo = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	if (nvmeibt_disk_segment_is_any_hot_recoverer(seg_topo)) {
		sprintf(progress_str, "(remaining dirty bits=%jx, remaining stale locks=%jx, remaning txid=%jx%s) ",
				seg_active->dirty_rebuild_ctx.n_blksets_remaining,
				seg_active->stale_rebuild_ctx.n_blksets_remaining,
				seg_active->txid_rebuild_ctx.n_blksets_remaining,
				(seg_active->JGC_rebuild_ctx.tid ? ", JGC in_work" : ""));
	}
	if (is_full_info_needed) {
		(*printf_fn)(printf_ctx, "\t\t- seg=%08x vol=%s ",
				nvmeibt_seg_active_UUID_8(seg_active),
				nvmeibt_seg_active_blkdev_name(seg_active));
	} else {
		(*printf_fn)(printf_ctx, "\t\t- seg_active ");
	}
	(*printf_fn)(printf_ctx, "praid_ver=%x.%x state=%s%s is_synchronizer=%d are_reg_sync=%d init_mode(dirty=%s stale=%s txid=%s) RM_ver=%zu\n",
			seg_topo->seg_praid_version_major, seg_topo->seg_praid_version_minor,
			dirty_bits_state_str(seg_topo->dirty_bits_state), progress_str,
			seg_topo->is_registrants_synchronizer,
			nvmeibt_seg_active_are_registrants_aligned_with_sync_cmd(seg_active),
			mem_tbl_init_mode_str(seg_topo->dirty_bits_init_mode), mem_tbl_init_mode_str(seg_topo->stale_locks_init_mode),
			mem_tbl_init_mode_str(seg_topo->txid_init_mode),
			seg_active->active_reservation_mode_version);
	nvmeibt_praid_dump_praid_status_line(printf_fn, printf_ctx, nvmeibt_seg_active_get_praid(seg_active), 0, is_full_info_needed);
	nvmeibt_register_print_status(printf_fn, printf_ctx, seg_active);
out:
	NFOUT;
	return 0;
}

int nvmeibt_seg_active_print_all_seg_actives_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx) {
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_seg_active	*seg_active;
	NFIN;
	(*printf_fn)(printf_ctx, "APPLIED_DISK_SEGMENTS\n");
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		nvmeibt_disk_print_status_line(printf_fn, printf_ctx, NNVMEIBT_LOCAL_DISK_GET_DISK(iersp4m, local_disk), 1);
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			nvmeibt_seg_active_print_status(printf_fn, printf_ctx, seg_active, 1);
		}
		nvmeibt_register_print_status(printf_fn, printf_ctx, NULL);
	}
	NFOUT;
	return 0;
}
