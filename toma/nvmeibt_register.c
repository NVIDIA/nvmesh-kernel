/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <time.h>
#include <sys/time.h>
#include "nvmeibt_common.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_register.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_recovery.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_raft.h"
#include "./interfaces/srvr/nvmeibt_srvr_proc.h"
#include "nvmeibt_wq.h"
#include "nvmeibt_ds_metadata.h"
#include "clnt/nvmeibt_client_protocol.h"
#include "clnt/nvmeibt_client.h"
#include "local/ram/nvmeibt_ds_blkset_entries.h"

/******************************************************************************/
/* Stale locks (conversion & use) workflow
   - lockid: bits 0-3 are idx_in_praid, and bits 4-27 are a serial lockid
	 Note that every seg has its own range of lockid (due to idx_in_praid)
   - A client (registrant) receives a legitimate lock_id from all segments (TOMAs),
	 select one of them, and use it for registration on all segs
   
   Objectives:
   - Say a client that registered with lock_id=0x13 unregistered (using a msg/client-cut-off/whatever)
	 and the local_disk is still present (the seg preserved its locks table in mem)
	 - We need to convert all of its lockids 0x13-->0x10000013
	 - The registrant needs to move to seg_active->stale_registrants_hash_by_lockid,
	   since we do not want to suggest this lockid to other clients and
	   Serjio needs to cleanup before the lockid is reused
   - (Unrelated) lockid-allocation
	 - get_fresh_reg_lock_id() for a client (in REGISTER_NACK and REGISTRABLE)
	   scans sequentially from reg_lock_id_cache_last_allocated_lockid looking for
	   an unused value (by active/stale registrants). Usually it is the first attempt.
	   - While at it, the range of lockids is sub-divided into zones, and when trying to
		 allocate from the next zone, we first call
		 lock_id_cache_zone_purge_launch_ask_all_registrants_to_forget_recoverable_lockids_cache_in_zone()
	     which usually does nothing, since even if we reuse a zone, it was used ages ago.
   
   Players:
   - seg_active->stale_registrants_hash_by_lockid
   - reg_ctx->n_stale_locks
   - nvmeibt_seg_active_add_blkset_to_stale_locks_hash()
	 - remove_stale_lock_from_seg_stale_locks_hash()
	   When all the stale_locks of a registrant are recovered, the lockid can be reused
   - seg_active->owner_lock_ids_to_release
   - owner_locks_release_group_wrapper()
   - nvmeibt_seg_active_delete_all_stale_locks_of_registrant()
   - owner_locks_set_to_release() -- called from registrant_disconnect_wrapper()
   - get_fresh_reg_lock_id()
	 - lock_id_cache_alloc()
	   - __lock_id_cache_alloc
   - reg_lock_id_cache_last_allocated_lockid
   
   Client side
   - A client registers with a lockid (that was suggested by one of the praid's TOMAs)
   - A client unregisters (using an UNREGISTER msg / physical-disconnect)
	 - TOMA converts the lock to stale as described in the TOMA workflow below
   - A client with a lockid tries to lock a blkset
	 - If the existing lock is free - no issue
	 - If the existing lock is taken - wait and retry
	   - If the lock is stuck, then report FAILED_LOCK to TOMA that will cut-off the locking client
		 - In TOMA if the locking client is no more an active_registrant (all its locks were already converted to stale), then TOMA sends LOCK_CLEANED
		   - For the client it means that all of the (stale) locks by that lockid can be synched without asking questions
		     - The client will remember that lockid (in a cache of ~10 lockids)
	 - If the existing lock is stale (stale bit was turned on by TOMA)
	   - If already received LOCK_CLEANED for this lockid (from all the praid's segs. I.e. no lock&I/O activity leftovers on this blkset) then
	     - recover (sync) the blkset and only then
		   - lock as usual and continue with the I/O
		   - send msg to TOMA that BLKSET_RECOVERED
		   - for the sync itself, Lock with my-lockid, so that if I unregister before BLKSET_RECOVERED,
			 then TOMA will return the original stale-lock (TOMA is aware of every stale-lock)
	   - If not yet received LOCK_CLEANED for this lockid then
	     - Send STALE_LOCK, and wait for LOCK_CLEANED
   
   stale-lock Workflow:
   - Per TOMA
   - A subscribes with a client_messaging_handle, a 8 bytes number whose 4 MSB bytes
	 are the CID (Client ID), and the LSB is specific to the segment
   - The client registers with a lockID (I will skip the registration workflow & longing)
	 We have a reg_ctx with reg_ctx->reg_lock_id and reg_ctx->client_messaging_handle
   - This reg_ctx is added to seg_active->active_registrants_hash_by_handle,
	 and in parallel to seg_active->active_registrants_hash_by_lockid
   - The registrant is unregistered (We know that it will not perform any lock/IO)
	 For a stale-lock to be relevant, the seg_active must remain functional,
	 hence either the client sent an RT_UNREGISTER, or was unsubscribed from the disk
   
   
   - launch_non_ioable_registrant_removal() // Called after we know that the registrant will not access the local segment or its locks
	 - remove_longing_registrant_on_seg_by_ctx()
	 - launch_existing_active_registrant_removal()	// For a registrant that was active
	   - ...
	   - launch_registrant_removal()	// For a registrant on non-JBOD, that used the locks table
		 - nvmeibt_registrant_disconnect_add_work		// Assign the work to a WQ (registrant_disconnect_wrapper, registrant_disconnect_finalize, ...)
		   - nvmeibt_local_disk_specific_add_work() Adds a WQ entry to the local_disk->wq
		 - (WQ) registrant_disconnect_wrapper
		   - The WQ-entry does not do the work. It is just adds the lockid to seg_active->owner_lock_ids_to_release in the following call-chain
		   - owner_locks_set_to_release()
			 - XDLIST_ADD_TAIL(&seg_active->owner_lock_ids_to_release, &entry->wq_entry);	// The entry from registrant_disconnect_wrapper()
			 - If it is the first entry in seg_active->owner_lock_ids_to_release
			   - Create a WQ task (owner_locks_release_group_wrapper, finalize=NULL , ...)
			   - Add the new task to the same WQ of the original task
			 - If not the first entry in seg_active->owner_lock_ids_to_release
			   - We just already added the lockid_to_release to the XDLIST seg_active->owner_lock_ids_to_release
		 - Now that we have the owner_locks_release_group task in the WQ, we will eventually get to execute owner_locks_release_group_wrapper()
		   - Scan the seg_active locks table, and for every non-0 entry (very few), See if it any of seg_active->owner_lock_ids_to_release
		   - In the following order:
			 - per converted lock: nvmeibt_seg_active_add_blkset_to_stale_locks_hash()
			   - (under mutex) Since we are in a WQ thread, and the main thread also accesses this hash
			 - Convert the lock to stale_lock (Add the stale-bit)
		 - For every entry in XDLIST seg_active->owner_lock_ids_to_release
		   - Call nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *)wq_entry);
			 - Actually, triggering the per registrant registrant_disconnect_finalize()
		 - registrant_disconnect_finalize() (runs in TOMA's main thread)
		   - For awaiting_registrants (clients awaiting for my stuck lock to become stale)
			 - nvmeibt_register_send_msg_to_registrant(awaiting_reg_ctx, NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED, ...)
		   - nvmeibt_register_terminate_registrant
*/

/******************************************************************************/
#define NVMEIB_REG_LOCK_ID_ZONE_BITS        2

#define NVMEIB_REG_LOCK_ID_TO_ZONE(lid)     \
		((lid) >>  (NVMEIB_REG_LOCK_ID_LOCKID_BITS - NVMEIB_REG_LOCK_ID_ZONE_BITS))
#define ZONE_START_COUNTER(zone)     \
		((zone) << (NVMEIB_REG_LOCK_ID_LOCKID_BITS - NVMEIB_REG_LOCK_ID_ZONE_BITS))
#define ZONE_LENGTH()     \
		((1) << (NVMEIB_REG_LOCK_ID_LOCKID_BITS - NVMEIB_REG_LOCK_ID_ZONE_BITS))

// This file contains the code for registrant disk_segment registration related functionality
// #define REGISTRANT_DISCONNECT_QUEUE_DEPTH (512)

static struct timespec next_wait_for_registrant_timeout = TIMESPEC_MAX_C99;

enum REGISTRANT_DISCONNECT_LAUNCH_STATUS {
	REGISTRANT_DISCONNECT_LAUNCH_OK			= 1,
	REGISTRANT_DISCONNECT_LAUNCH_SKIPPED	= 2,
	REGISTRANT_DISCONNECT_LAUNCH_FAILED		= 3,
};

enum REGISTRANT_DISCONNECT_RSP_STATUS {
	REGISTRANT_DISCONNECT_RSP_OK 						= 0,
	REGISTRANT_DISCONNECT_RSP_ERR 						= 1,
};

/**
 * Response message sent by TOMA main thread to registran
 * removal thread, upon completion of unlock request.
 *
 * @author max (11/16/17)
 */
struct registrant_disconnect_rsp_msg {
	enum NVMEIBT_TOPOLOGY_TOMA_MSG_TYPES	msg_type:32;
	enum REGISTRANT_DISCONNECT_RSP_STATUS	status:32;
	union nvmeib_lock_blkset_entry			old_val;
	uint64_t 								blkset_num;
} __attribute__ ((packed));

/**
 * WQ entry for registrant_disconnect thread - contains all the
 * parameters needed in order to perform registrant_disconnect
 * for given segment. All tables that are accessed locally are
 * copies, so no multithreading issues should arise between this
 * thread and TOMA main thread.
 *
 * @author max (11/16/17)
 */
struct registrant_disconnect_wq_entry {
	/* WQ infrastructure */
	struct nvmeibt_wq_entry 			wq_entry;

	/* Input parameters */
	struct nvmeibt_registrant_ctx 		*reg_ctx;	// The registrant can be used inside the thread, since while registrant_disconnect is in progress
													//topology can't change and registrants cannot register/unregister from segment.
	uint64_t 							n_owner_locks_converted_to_stale;	// Turned stale bit on, fixup must be performed by client.
	uint64_t 							n_owner_locks_converted_to_zero;  	// no fixup needed so toma just unlocked them

	/* Output parameters - used by the thread completion*/
	struct nvmeibt_seg_active 			*seg_active;
};

struct owner_locks_release_wq_entry {
	struct nvmeibt_wq_entry 			wq_entry;
	struct nvmeibt_seg_active 			*seg_active;
	struct nvmeibt_local_disk			*local_disk;
};

static bool brute_force_test = false;

static void brute_force_disconnect_registrant_client(
										struct nvmeibt_registrant_ctx *reg_ctx,
										BOOL is_on_timeout);

static void send_registrable_to_all_longing_registrants(struct nvmeibt_seg_active *seg_active);

#define NDUMP_REG_CTX(name, _Tf_OR_If, reg_ctx) do {																\
		struct timespec		__now, time_left;																		\
		if (!reg_ctx) {N ## _Tf_OR_If(name ## _1, "reg_ctx=NULL"); break;}											\
		getnstimeofday_boot(&__now);																				\
		time_left = timespec_sub((reg_ctx)->timeout_time, __now);													\
		N ## _Tf_OR_If(name ## _2, "seg=@UUID_8 lock_id=@T_LID handle=@HANDLE "										\
			"time_left=@LU.@LU is_force_cmd_called=@BOOL disconnect_time=@LU",										\
			nvmeibt_seg_active_UUID_8((reg_ctx)->seg_active),														\
			nvmeib_lockid_purify((reg_ctx)->reg_lock_id), (reg_ctx)->client_messaging_handle,						\
			time_left.tv_sec, time_left.tv_nsec,																	\
			(reg_ctx)->is_force_cmd_called, (reg_ctx)->reg_disconnect_time.tv_sec);									\
} while (0)

static void add_longing_registrant_on_seg(struct nvmeibt_registrant_ctx *input_reg_ctx);

static BOOL is_registrant_on_timeout(const struct nvmeibt_registrant_ctx *reg_ctx)
{
	return (reg_ctx && (reg_ctx->reg_disconnect_time.tv_sec != 0));
}

BOOL nvmeibt_register_is_processing_registrant_removal(const struct nvmeibt_registrant_ctx *reg_ctx)
{
	return (reg_ctx && reg_ctx->is_processing_registrant_removal);
}

static void set_is_processing_registrant_removal(struct nvmeibt_registrant_ctx *reg_ctx, bool val)
{
	if (reg_ctx) {
		reg_ctx->is_processing_registrant_removal = val;
	}
}

static inline struct nvmeibt_registrant_ctx * get_registrant_by_handle_from_active_hash(struct nvmeibt_seg_active *seg_active, struct nvmeibt_registrant_ctx *input_reg_ctx)
{
	struct nvmeibt_registrant_ctx		*candidate_reg_ctx;

	candidate_reg_ctx = (seg_active && seg_active->active_registrants_hash_by_handle ?
						 nvmeib_hash_search_uint64_t(seg_active->active_registrants_hash_by_handle, input_reg_ctx->client_messaging_handle) :
						 NULL);
	return (candidate_reg_ctx);
}

static BOOL upd_registrant_sync_timeout(struct nvmeibt_registrant_ctx *reg_ctx,
										enum REG_TIMEOUT_REASON timeout_reason);

static void registrant_stopped_being_active(struct nvmeibt_seg_active *seg_active, struct nvmeibt_registrant_ctx *reg_ctx, bool is_delete_from_hashs)
{
    int										praid_ver = nvmeibt_seg_active_get_active_praid_version_major(seg_active);

	// We know that it is an active registrant
	seg_active->n_active_registrants_on_active_praid_version -=
		(reg_ctx->praid_version >= praid_ver);	// ">" if the client is more updated. Not an old msg.
	upd_registrant_sync_timeout(reg_ctx, REG_TIMEOUT_REASON_DEL_ME);
	// notify recovery tasks of a registrant removal
	if (reg_ctx->is_recoverer)
		nvmeibt_recovery_handle_client_unregistered(reg_ctx);
	if (is_delete_from_hashs) {
		NVMEIBT_SEG_ACTIVE_REMOVE_ACTIVE_REGISTRANT_FROM_HASHES(seg_active, reg_ctx);
	}
}

static BOOL is_force_cmd_called(const struct nvmeibt_registrant_ctx *reg_ctx)
{
	return (reg_ctx && (reg_ctx->is_force_cmd_called));
}

static void validate_registrants_on_timeout(const struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_registrant_ctx	*reg_ctx;
	int								n = 0;

	XDLIST_FOREACH(reg_ctx, &(seg_active->registrants_on_timeout)) {
		NDUMP_REG_CTX(dhy7652, _Tf, reg_ctx);
		n++;
	}
	N_Tf(trace_1_register_validate_registrants_on_timeout,
		 "n=@N_REGISTRANTS_ON_TIMEOUT", n);

	if (n != seg_active->n_registrants_on_timeout) {
		N_Ef(dgghy34,"n=@N_REGISTRANTS_ON_TIMEOUT seg_active->n_registrants_on_timeout=@N_REGISTRANTS_ON_TIMEOUT",
			 n, seg_active->n_registrants_on_timeout);
		nvmeibt_abort(ES_FATAL);
	}
}

void dump_seg_active_registrants(const struct nvmeibt_seg_active *seg_active, int is_err)
{
	struct nvmeibt_registrant_ctx	*active_registrant;
	struct timespec					now;
	NFIN;

	getnstimeofday_boot(&now);
	NVMEIB_HASH_FOREACH(active_registrant, seg_active->active_registrants_hash_by_lockid) {
		if (is_err && !(active_registrant->is_force_cmd_called) && timespec_lt(active_registrant->timeout_time, now)) {
			NDUMP_REG_CTX(dhy7462, _Ef, active_registrant);
		}
		else {
			NDUMP_REG_CTX(dllo046, _Tf, active_registrant);
		}
	}
	NFOUT;
}

/*
 * Handle lockid alloc/register/unregister
 *
 * Allocation
 * - lock-ids are allocated sequentially, so we must handle wraparound.
 * - clients keep in-memory cache of disconnected (recoverable) lock-ids;
 *   we must invalidate the cache if we wish to reclaim lock-ids.
 * - to invalidate cached lock-ids: divide the lock-id space to zones:
 *   when alloc enters a new zone, purge next zone in client cache.
 * - to purge a zone, send purge request to all existing registrants for
 *   the zone, and wait for ack (or disconnect) from all.
 *
 * Register
 * - verify the registrant provided lock-id isn't already taken. If
 *   it is, then allocate new lock-id and tell the registrant using nack
 *   with reason NVMEIBT_CLIENT_TR_REASON_LOCKID_ALREADY_TAKEN.
 * - in allocating a new lock-id, check if reached new zone; If so then
 *   send all registrants purge request. if the current zone is in-purge,
 *   then nack with reason NVMEIBT_CLIENT_TR_REASON_LOCKID_ALREADY_TAKEN.
 *
 * Unregister
 * - remove the registrant from the list of users of the lock-id, so the
 *   same can be eventually reused by another registrant later.
 */

/*
 * Tracking zone purge state
 *
 * Each seg_active keeps track of
 * - reg_lock_id_cache_counter:     next lock_id to (attempt to) allocate
 * - reg_lock_id_cache_purge_seqno: increasing seq no of purge request
 * - reg_lock_id_cache_purge_zone:  index of current zone going purge (if any)
 * - reg_lock_id_cache_purge_count: number of registrants expected to ack
 *
 * And registrants carry a new flag "is_purging_lock_id_cache" - used for
 * those registrants that are expected to ack a pending purge request.
 */

static BOOL lock_id_cache_is_zone_purging(
				struct nvmeibt_seg_active *seg_active, int zone_no)
{
	/* either purge count is zero (no pending acks) or purge zone is set */
	NTOMA_ASSERT(itu8692,
				 seg_active->reg_lock_id_cache_purge_n_purges_in_fly == 0 || seg_active->reg_lock_id_cache_purge_zone == zone_no,
				 "seg=@UUID_8 purge in lock_id_zone @ZONE_NO but count 0",
				 nvmeibt_seg_active_UUID_8(seg_active), zone_no);
	if (zone_no > NVMEIB_REG_LOCK_ID_LOCKID_BITS_MASK) {
		N_Ef(tv23h48, "zone_no=@INT", zone_no);
		nvmeibt_abort(ES_FATAL);	// A dummy code for unused variable in release mode
	}
	return seg_active->reg_lock_id_cache_purge_n_purges_in_fly > 0;
}

static void lock_id_cache_zone_purge_launch_ask_all_registrants_to_forget_recoverable_lockids_cache_in_zone(
				struct nvmeibt_seg_active *seg_active, int zone_no)
{
	struct nvmeibt_registrant_ctx			*reg_ctx;
	struct nvmeibt_lockid_cache_purge_pl	pl;
	int										n_purges_in_fly = 0;
	int										ret;

	NFIN;

	pl.purge_seqno = ++seg_active->reg_lock_id_cache_purge_seqno;
	pl.start_counter = ZONE_START_COUNTER(zone_no);
	pl.length = ZONE_LENGTH();

	N_Tf(dhyt76w, "zone purge start seg=@UUID_8 lock_id_zone=@LOCK_ID_ZONE count=? seqno=@SEQNO",
		nvmeibt_seg_active_UUID_8(seg_active), zone_no, pl.purge_seqno);

	NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_lockid) {
		N_Tf(lock_id_cache_zone_purge_launch_trace,
			 "send @STR to handle=@HANDLE reg_lock_id=@C_LID",
			nvmeibt_protocol_client_msg_str(NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE),
			reg_ctx->client_messaging_handle, nvmeib_lockid_purify(reg_ctx->reg_lock_id));

		ret = nvmeibt_register_send_msg_to_registrant(
								reg_ctx,
								NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE,
								NVMEIBT_CLIENT_TR_REASON_NONE,
								sizeof(pl), &pl);

		if (ret < 0) {
			/*
			 * just report; most likely the registrant is/will disconnect soon;
			 * if not, and if it remains here when we reach this zone, then we
			 * we will kick it then.
			 */
			N_Tf(lock_id_cache_zone_purge_launch_trace_2,
				 "failed to send to registrant");
		}

		reg_ctx->is_purging_lock_id_cache = 1;
		n_purges_in_fly++;
	}

	if (n_purges_in_fly > 0) {
		seg_active->reg_lock_id_cache_purge_zone = zone_no;
		seg_active->reg_lock_id_cache_purge_n_purges_in_fly = n_purges_in_fly;

		N_Tf(dhhhy73, "zone purge start seg=@UUID_8 lock_id_zone=@LOCK_ID_ZONE count=@COUNT seqno=@SEQNO_LONG",
			nvmeibt_seg_active_UUID_8(seg_active),
			seg_active->reg_lock_id_cache_purge_zone,
			seg_active->reg_lock_id_cache_purge_n_purges_in_fly,
			seg_active->reg_lock_id_cache_purge_seqno);
	}

	NFOUT;
}

static void lock_id_cache_zone_purge_finish(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_registrant_ctx *reg_ctx;

	NFIN;

	N_Tf(dhhuwe5, "zone purge stop seg=@UUID_8 lock_id_zone=@LOCK_ID_ZONE count=@COUNT seqno=@SEQNO_LONG",
		nvmeibt_seg_active_UUID_8(seg_active),
		seg_active->reg_lock_id_cache_purge_zone,
		seg_active->reg_lock_id_cache_purge_n_purges_in_fly,
		seg_active->reg_lock_id_cache_purge_seqno);

	NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_lockid) {
		if (reg_ctx->is_purging_lock_id_cache) {
			N_Tf(dkti994, "drop purging registrant: handle=@HANDLE on seg=@UUID_8 purge_seqno=@LD lock_id_zone=@X",
				 reg_ctx->client_messaging_handle,
				 nvmeibt_seg_active_UUID_8(seg_active),
				 seg_active->reg_lock_id_cache_purge_seqno,
				 seg_active->reg_lock_id_cache_purge_zone);
			brute_force_disconnect_registrant_client(reg_ctx, 0);
		}
	}

	NFOUT;
}

static void lock_id_cache_registrant_purge_done(
				struct nvmeibt_seg_active *seg_active,
				struct nvmeibt_registrant_ctx *reg_ctx)
{
	reg_ctx->is_purging_lock_id_cache = 0;

	seg_active->reg_lock_id_cache_purge_n_purges_in_fly--;

	if (seg_active->reg_lock_id_cache_purge_n_purges_in_fly == 0) {
		N_Tf(trace_register_lock_id_cache_registrant_unpurge, "not more purge waiting");
		send_registrable_to_all_longing_registrants(seg_active);
	}
}

static int lock_id_cache_zone_purge_ack(
				struct nvmeibt_registrant_ctx *reg_ctx, u64 seqno, u64 start_counter)
{
	struct nvmeibt_seg_active	*seg_active;
	int							rv = -1;
	int							zone_no;

	NFIN;
	zone_no = NVMEIB_REG_LOCK_ID_TO_ZONE(start_counter);
	seg_active = reg_ctx->seg_active;

	N_Tf(djjurye, "recv @PROTOCOL_CLIENT_MSG_STR from registrant: handle=@HANDLE on seg=@UUID_8 purge_seqno=@PURGE_SEQNO lock_id_zone=@LOCK_ID_ZONE",
		nvmeibt_protocol_client_msg_str(NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK),
		reg_ctx->client_messaging_handle,
		nvmeibt_seg_active_UUID_8(seg_active), seqno, zone_no);

	if (!reg_ctx->is_purging_lock_id_cache) {
		N_Ef(sloir84, "registrant: handle=@HANDLE not marked as purging", reg_ctx->client_messaging_handle);
		goto out;
	}

	if (seg_active->reg_lock_id_cache_purge_seqno != seqno) {
		N_Ef(jjut843, "registrant: handle=@HANDLE purge_seqno mismatch @REG_LOCK_ID_CACHE_PURGE_SEQNO != @SEQNO",
			reg_ctx->client_messaging_handle, seg_active->reg_lock_id_cache_purge_seqno, seqno);
		goto out;
	}

	if (seg_active->reg_lock_id_cache_purge_zone != zone_no) {
		N_Ef(ajgiut8, "registrant: handle=@HANDLE lock_id_zone mismatch @REG_LOCK_ID_CACHE_PURGE_ZONE != @ZONE_NO",
			reg_ctx->client_messaging_handle, seg_active->reg_lock_id_cache_purge_zone, zone_no);
		goto out;
	}

	if (nvmeibt_register_lookup_active_registrant_by_reg_lock_id(seg_active, reg_ctx->reg_lock_id) != reg_ctx) {
		N_Ef(llo0iy8, "registrant: handle=@HANDLE not really registrered", reg_ctx->client_messaging_handle);
		goto out;
	}

	lock_id_cache_registrant_purge_done(seg_active, reg_ctx);

	rv = 0;

out:
	NFOUT;

	return rv;
}

static int lock_id_cache_registrant_unregistered(struct nvmeibt_registrant_ctx *reg_ctx)
{
	struct nvmeibt_seg_active *seg_active;
	int rv = -1;

	NFIN;

	seg_active = reg_ctx->seg_active;

	if (!reg_ctx->is_purging_lock_id_cache) {
		N_Tf(aasgyr6, "registrant: @HANDLE no new lockid_zone. No need for purging", reg_ctx->client_messaging_handle);
		goto out;
	}

	/* purge count must be positive, purge zone must be set */
	NTOMA_ASSERT(fkitu85,
				 seg_active->reg_lock_id_cache_purge_n_purges_in_fly > 0 && seg_active->reg_lock_id_cache_purge_zone >= 0,
				 "seg=@UUID_8 purge but lock_id_zone @REG_LOCK_ID_CACHE_PURGE_ZONE count @REG_LOCK_ID_CACHE_PURGE_COUNT",
				 nvmeibt_seg_active_UUID_8(seg_active),
				 seg_active->reg_lock_id_cache_purge_zone,
				 seg_active->reg_lock_id_cache_purge_n_purges_in_fly);

	N_Tf(fhruy75, "unregister of registrant: handle=@HANDLE on seg=@UUID_8 purge_count=@PURGE_COUNT purge_seqno=@PURGE_SEQNO_LONG lock_id_zone=@LOCK_ID_ZONE",
		reg_ctx->client_messaging_handle,
		nvmeibt_seg_active_UUID_8(seg_active),
		seg_active->reg_lock_id_cache_purge_n_purges_in_fly,
		seg_active->reg_lock_id_cache_purge_seqno,
		seg_active->reg_lock_id_cache_purge_zone);

	lock_id_cache_registrant_purge_done(seg_active, reg_ctx);

	rv = 0;

out:
	NFOUT;

	return rv;
}

inline static BOOL lock_id_cache_is_lockid_taken(struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id rli)
{
	return ((nvmeibt_register_lookup_active_registrant_by_reg_lock_id(seg_active, rli) != NULL) ||
			(nvmeibt_register_lookup_stale_registrant_by_reg_lock_id(seg_active, rli)));
}

/*
 * Note: the arg "reg_lock_id" is both input and output argument -
 * The caller is assumed to have filled other bit fields, and here we only
 * assign the respective lock_id field and validate the outcome.
 * Return "0" if failed to allocate new lock_id.
 */
static BOOL __lock_id_cache_alloc(
				struct nvmeibt_seg_active *seg_active,
				union nvmeib_lock_id *reg_lock_id)
{
	int		lock_id, start_id;
	int		zone_no, new_zone_no;
	BOOL	is_found = 0;

	start_id = seg_active->reg_lock_id_cache_last_allocated_lockid;
	lock_id = start_id;
	zone_no = NVMEIB_REG_LOCK_ID_TO_ZONE(lock_id);

	do {
		lock_id = ((lock_id + 1) & NVMEIB_REG_LOCK_ID_LOCKID_BITS_MASK);
		/* fail if all lock_ids are currently in use */
		if (lock_id == start_id) {
			N_Tf(dhuu754,"seg=@UUID_8 @LOCKID toma-not-ready due to wraparound",
				nvmeibt_seg_active_UUID_8(seg_active), lock_id);
			break;
		}
		/* lock_id may not be all zero (registrants use 0 to request lock_id */
		if (lock_id == 0)
			lock_id++;

		/* if we enter new zone - purge following (after next) zone */
		new_zone_no = NVMEIB_REG_LOCK_ID_TO_ZONE(lock_id);
		if (new_zone_no != zone_no) {
			lock_id_cache_zone_purge_launch_ask_all_registrants_to_forget_recoverable_lockids_cache_in_zone(seg_active, new_zone_no);
		}
		zone_no = new_zone_no;

		/*
		 * If reached zone in purging, we may not proceed until all registrants
		 * either ack or disconnect - so return error.
		 */
		if (lock_id_cache_is_zone_purging(seg_active, zone_no)) {
			N_Tf(ddhy553, "seg=@UUID_8 lock_id_zone @ZONE_NO toma-not-ready due to purge",
				nvmeibt_seg_active_UUID_8(seg_active), zone_no);
			/* kick in the butt to all registrants that did not ack yet */
			lock_id_cache_zone_purge_finish(seg_active);
			break;
		}

		reg_lock_id->bits.lock_id = lock_id;

		if (lock_id_cache_is_lockid_taken(seg_active, *reg_lock_id)) {
			N_Tf(ubs4nkg, "seg=@UUID_8 lockid=@T_LID reg_lock_id=@C_LID already taken",
				nvmeibt_seg_active_UUID_8(seg_active), lock_id, nvmeib_lockid_purify(*reg_lock_id));
		} else {
			N_Tf(gjiu4336, "seg=@UUID_8 lockid=@T_LID reg_lock_id=@C_LID is good to go",
				nvmeibt_seg_active_UUID_8(seg_active), lock_id, nvmeib_lockid_purify(*reg_lock_id));
			is_found = 1;
			seg_active->reg_lock_id_cache_last_allocated_lockid = lock_id;
			break;
		}
	} while (1);

	return is_found;
}

/* lock_id_cache_alloc(): allocate lock_id for version >> 2.1 */
static BOOL lock_id_cache_alloc(
				struct nvmeibt_seg_active *seg_active,
				union nvmeib_lock_id *reg_lock_id)
{
	return __lock_id_cache_alloc(seg_active, reg_lock_id);
}

static int handle_lock_id_cache_purge_ack_received(struct nvmeibt_register_msg *msg)
{
	struct nvmeibt_registrant_ctx *reg_ctx;
	struct nvmeibt_lockid_cache_purge_pl *pl;
	int rv = -1;

	NFIN;

	pl = (struct nvmeibt_lockid_cache_purge_pl *) msg->msg_data;
	reg_ctx = nvmeibt_register_lookup_active_registrant_by_reg_lock_id(
					msg->registrant_ctx.seg_active,
					msg->registrant_ctx.reg_lock_id);

	if (reg_ctx == NULL) {
		N_Tf(djjru84, "unknown registrant for seg=@UUID_8 reg_lock_id=@C_LID",
			nvmeibt_seg_active_UUID_8(msg->registrant_ctx.seg_active),
			nvmeib_lockid_purify(msg->registrant_ctx.reg_lock_id));
		rv = 0;	// Possibly already removed. No harm anyhow.
		goto out;
	}

	rv = lock_id_cache_zone_purge_ack(reg_ctx, pl->purge_seqno, pl->start_counter);

out:
	NFOUT;
	return rv;
}

static union nvmeib_lock_id get_fresh_reg_lock_id(struct nvmeibt_seg_active *seg_active)
{
	union nvmeib_lock_id	rli = { .all = 0 };

	/*
	 * fill the idx first, so that lock_id_cache_alloc() can test the result
	 * and skip nvmeib_stale_special_* values.
	 */
	rli.bits.idx_in_praid = nvmeibt_disk_segment_idx_in_praid(nvmeibt_seg_active_get_disk_segment(seg_active));	// Fill the idx
	if (!lock_id_cache_alloc(seg_active, &rli)) {	// Fill the lock_id
		rli.all = 0;	// 0 means failure
	}
	N_Tf(trace_register_get_fresh_reg_lock_id, "allocated reg_lock_id=@C_LID", nvmeib_lockid_purify(rli));
	return rli;
}

static void alloc_reg_ctx(struct nvmeibt_registrant_ctx **reg_ctx, struct nvmeibt_registrant_ctx *copied_reg_ctx)
{
	*reg_ctx = NNVMEIBT_TOMA_CALLOC(trace_register_alloc_reg_ctx, 1, sizeof(**reg_ctx));
	**reg_ctx = *copied_reg_ctx;
	XDLIST_INIT_LINK(&((*reg_ctx)->registrant_on_timeout_link), NULL);
	XDLIST_INIT_LINK(&((*reg_ctx)->longing_on_invalid_seg_link), NULL);
	(*reg_ctx)->n_stale_locks = 0;
	nvmeibt_client_reg_ctx_ref_added((*reg_ctx)->client, *reg_ctx);
}

BOOL nvmeibt_register_is_same_registrant(const struct nvmeibt_registrant_ctx *r1, const struct nvmeibt_registrant_ctx *r2)
{
	return (r1 && r2 &&
			((nvmeib_lockid_are_purified_eq(r1->reg_lock_id, r2->reg_lock_id) && (nvmeib_lockid_purify(r1->reg_lock_id) != 0)) ||
			 ((r1->client_messaging_handle == r2->client_messaging_handle) && (r1->client_messaging_handle))));
}

void nvmeibt_register_recalc_seg_active_registrants_align_with_sync_cmd(struct nvmeibt_seg_active *seg_active)
{
	BOOL									are_aligned;
	BOOL									are_all_registrants_aligned_with_praid_version;
	const struct nvmeibt_praid_topo_ctx		*praid_topo_ctx = 0;
	struct nvmeibt_disk_segment				*disk_segment;

	// Examining seg_active properties (registration), but since we need this property
	//  (are_aligned) also in the seg's topos on the leader, we keep it only in seg_topo
	//  so here we update the applied_topo
	if (!seg_active) {
		// Empty (technical) alignment
		are_aligned = 1;
		disk_segment = NULL;
		goto apply_are_aligned;
	}
	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	praid_topo_ctx = nvmeibt_seg_active_get_praid_applied_topo(seg_active);
	are_all_registrants_aligned_with_praid_version =
			(nvmeibt_seg_active_n_active_registrants(seg_active) == nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active));
	if (	nvmeibt_global_get_global()->is_in_shutdown_active_phase ||
			nvmeibt_disk_segment_is_x(nvmeibt_seg_active_get_active_seg_topo(seg_active))) {
		are_aligned = !nvmeibt_register_is_any_registered_on_seg_active(seg_active);
		goto apply_are_aligned;
	}

	switch (praid_topo_ctx->registrants_sync_cmd) {
	case PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN:	// Before getting anything from the leader
	case PRAID_REGISTRANTS_SYNC_CMD_DELETE:
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE_I:
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I:
		// Cannot Delete/initialize if any clients are working
		are_aligned = !nvmeibt_register_is_any_registered_on_seg_active(seg_active);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I:
		// SW_TOPO_I happens when we have live registrants on the owner, and no
		//  registrants on the UNDER_RECOVERY_R
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X:
		are_aligned = (nvmeibt_disk_segment_is_competent_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active)) ?
					   are_all_registrants_aligned_with_praid_version :
					   nvmeibt_seg_active_n_active_registrants(seg_active) == 0);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U:
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R:
		// The switch from SWITCH_TOPO_W/EC_COLD_RECOVERY_I required client sync on the same major version. None use_old_version
		if (!are_all_registrants_aligned_with_praid_version) {
			N_Tf(tuuu853, "seg=@UUID_8 n_active=@N_ACTIVE n_applied=@N_APPLIED",
				nvmeibt_seg_UUID_8(disk_segment),
				nvmeibt_seg_active_n_active_registrants(seg_active),
				nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active));
			dump_seg_active_registrants(seg_active, 1);
			validate_registrants_on_timeout(seg_active);
		}		/* FALLTHROUGH */
#if (__GNUC__ >= 7)
		__attribute__ ((fallthrough)); // Otherwise gcc complains about a nasty fallthrough
#endif
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE:
	case PRAID_REGISTRANTS_SYNC_CMD_UNUSED_0:
		// No sync in the air. Not expecting registrants with old version
	case PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS:
		// RESET_REGISTRANTS resets n_active_registrants_on_active_praid_version
		// We open for registration when n_active_registrants drops to 0
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W:
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D:
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE:
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE:
		are_aligned = are_all_registrants_aligned_with_praid_version;
		break;
	default:
		N_Wf(warn_register_nvmeibt_register_recalc_seg_active_registrants_align_with_sync_cmd, "Unexpected value registrants_sync_cmd=@REGISTRANTS_SYNC_CMD_STR",
			praid_registrants_sync_cmd_str(praid_topo_ctx->registrants_sync_cmd));
		are_aligned = 0;
		break;
	}
apply_are_aligned:
	if (seg_active) {
		N_Tf(fju8745, "@UUID_8 are_aligned=@ARE_ALIGNED n_active=@N_ACTIVE(applied=@APPLIED_INT) n_longing=@N_LONGING sync_cmd=@SYNC_CMD",
			nvmeibt_seg_active_UUID_8(seg_active), are_aligned,
			nvmeibt_seg_active_n_active_registrants(seg_active),
			nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active),
			nvmeibt_seg_active_n_longing_registrants(seg_active),
			praid_registrants_sync_cmd_str(praid_topo_ctx->registrants_sync_cmd));
		if (nvmeibt_seg_active_are_registrants_aligned_with_sync_cmd(seg_active) != are_aligned) {
			nvmeibt_seg_active_set_registrants_aligned_with_sync_cmd(seg_active, are_aligned);
			nvmeibt_disk_segment_active_mark_reserialization_required(disk_segment);
		}
	}
}

BOOL nvmeibt_register_is_seg_active_registrable_clients_sync_wise(struct nvmeibt_seg_active *seg_active, int *reason)
{
	BOOL									is_registrable;
	BOOL									are_all_registrants_aligned_with_praid_version;
	const struct nvmeibt_praid_topo_ctx		*praid_topo_ctx;
	struct nvmeibt_disk_segment_topo_ctx	*topo_ctx;
	int										tmp_reason = NVMEIBT_CLIENT_TR_REASON_AWAITING_CLIENTS_SYNC;

	TODO(This function pretty much duplicates nvmeibt_register_recalc_seg_active_registrants_align_with_sync_cmd(seg_active), Rethink);
	praid_topo_ctx = nvmeibt_seg_active_get_praid_applied_topo(seg_active);
	if (!(praid_topo_ctx->is_activated)) {
		is_registrable = 0;
		goto out;
	}
	are_all_registrants_aligned_with_praid_version =
		(nvmeibt_seg_active_n_active_registrants(seg_active) ==
		 nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active));

	switch (praid_topo_ctx->registrants_sync_cmd) {
	case PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS:
		// RESET_REGISTRANTS resets n_active_registrants_on_active_praid_version
		// We open for registration when n_active_registrants drops to 0
		is_registrable = (are_all_registrants_aligned_with_praid_version ||
						  !(nvmeibt_seg_active_get_active_seg_topo(seg_active)->is_registrants_synchronizer));
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X:
		TODO(For now, the switch to X RESET_REGISTRANTs. Need to merge with the transition SW_TOPO-->unsefe-->safe-->stable);
		tmp_reason = NVMEIBT_CLIENT_TR_REASON_DELETING_SEG;
		is_registrable = nvmeibt_disk_segment_is_competent_owner(nvmeibt_seg_active_get_active_seg_topo(seg_active));
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I:
		// The owners continue to serve their clients. No interruption
		tmp_reason = NVMEIBT_CLIENT_TR_REASON_INIT;
		topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
		is_registrable = nvmeibt_disk_segment_is_competent_owner(topo_ctx) || nvmeibt_disk_segment_is_under_recovery_R(topo_ctx);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE_I:
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I:
		// No registration during initializations
		// Cannot initialize if any clients are working
		tmp_reason = NVMEIBT_CLIENT_TR_REASON_INIT;
		is_registrable = 0;
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U:
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R:
		// The switch from SWITCH_TOPO_W/EC_COLD_RECOVERY_I required client sync on the same major version. None use_old_version
		if (!are_all_registrants_aligned_with_praid_version) {
			N_Tf(fhu7yw1, "seg=@UUID_8 n_active=@N_ACTIVE n_applied=@N_APPLIED",
				nvmeibt_seg_active_UUID_8(seg_active),
				nvmeibt_seg_active_n_active_registrants(seg_active),
				nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active));
			dump_seg_active_registrants(seg_active, 1);
			validate_registrants_on_timeout(seg_active);
		}
#if (__GNUC__ >= 7)
		__attribute__ ((fallthrough)); // Otherwise gcc complains about a nasty fallthrough
#endif
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE:
		// No sync in the air. Not expecting registrants with old version
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W:
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D:
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE:
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE:
		is_registrable = 1;
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_DELETE:
		N_Wf(trace_zzz_9, "unexpected registrants_sync_cmd=DELETE");
		tmp_reason = NVMEIBT_CLIENT_TR_REASON_INTERNAL_ERR;
		is_registrable = 0;
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN:
	default:
		N_Wf(fjjut85, "unexpected value registrants_sync_cmd=@REGISTRANTS_SYNC_CMD_STR",
			praid_registrants_sync_cmd_str(praid_topo_ctx->registrants_sync_cmd));
		tmp_reason = NVMEIBT_CLIENT_TR_REASON_INTERNAL_ERR;
		is_registrable = 0;
		break;
	}
out:
	N_Tf(aasdju3, "@UUID_8 is_registrable=@IS_REGISTRABLE n_active=@N_ACTIVE(applied=@APPLIED_INT) n_longing=@N_LONGING sync_cmd=@SYNC_CMD",
		nvmeibt_seg_active_UUID_8(seg_active), is_registrable,
		nvmeibt_seg_active_n_active_registrants(seg_active),
		nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active),
		nvmeibt_seg_active_n_longing_registrants(seg_active),
		praid_registrants_sync_cmd_str(praid_topo_ctx->registrants_sync_cmd));
	if (reason && !is_registrable) {
		*reason = tmp_reason;
	}
	return is_registrable;
}

void calc_next_wait_for_registrant_timeout(void)
{
	struct nvmeibt_seg_active	*seg_active;
	struct timespec				min_timespec = TIMESPEC_MAX_C99;
	static struct timespec		prev_min_timespec = TIMESPEC_ZERO;
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	N_Tf(d6ro9u7, "");
	if (!(cur_topo->is_need_calc_next_wait_for_registrant_timeout)) {
		goto out;
	}
	cur_topo->is_need_calc_next_wait_for_registrant_timeout = 0;
	// Go over the local disk_segments find the minimal timeout
	NVMEIB_HASH_FOREACH(local_disk, cur_topo->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			struct nvmeibt_registrant_ctx	*reg_ctx;

			XDLIST_FOREACH(reg_ctx, &(seg_active->registrants_on_timeout)) {
				NDUMP_REG_CTX(djjiru4, _Tf, reg_ctx);
				if (nvmeibt_register_is_processing_registrant_removal(reg_ctx)) {
					N_Ef(djitu84, "processing_unreg_req, should not be in the list");
					nvmeibt_abort(ES_FATAL);
				}
				min_timespec = timespec_min(min_timespec, reg_ctx->timeout_time);
			}
		}
	}
	if (timespec_eq(min_timespec, TIMESPEC_MAX_C99)) {
		// No registrant to wait for
		next_wait_for_registrant_timeout = TIMESPEC_MAX_C99;
		goto out;
	}
	// If got the same timeout, in the past, and the timeout already expired, then give it some time to work
	if (	timespec_eq(prev_min_timespec, min_timespec) &&
			timespec_lt(min_timespec, nvmeibt_global_get_cur_event_start_time()) &&
			timespec_lt(next_wait_for_registrant_timeout, nvmeibt_global_get_cur_event_start_time())) {
		next_wait_for_registrant_timeout = nvmeibt_global_get_cur_event_start_time();
		timespec_update_by_a_few_nsec(&next_wait_for_registrant_timeout, nvmeibt_raft_get_effective_heartbeat_timeout_ns());
	}
	else {
		next_wait_for_registrant_timeout = min_timespec;
	}
	prev_min_timespec = min_timespec;
out:
	;
}

int nvmeibt_register_send_msg_to_registrant(struct nvmeibt_registrant_ctx *registrant_ctx,
									  enum NVMEIBT_CLIENT_MSG_TYPES msg_type, enum NVMEIBT_CLIENT_TR_REASON reason, int data_length, void *data)
{
	int		rv;
	int		praid_version;
	u64		msg_id;

	NFIN;
	msg_id = (u64)nvmeib_public_rdtsc();
	NREGISTER_MSG_DUMP(trace_register_nvmeibt_register_send_msg_to_registrant, msg_type, reason, registrant_ctx, data_length, msg_id);
	if (brute_force_test && ((msg_type == NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT) ||
							 (msg_type == NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY))) {
		N_Tf(t_xx_74, "Under brute force test, don't send the message");
		rv = 0;
		goto out;
	}
	// For UNREG the client needs to UNREG from its registered praid_topo
	// If the leader decided that the seg is dead, but the client is connected and registered toma wise.
	//  The client assumes that it is unregistered from dead, and should send ack to unreg with the
	//  praid_version that is old (from before the dead).
	if (msg_type == NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT) {
		praid_version = registrant_ctx->praid_version;
	} else {
		praid_version = nvmeibt_seg_active_get_active_praid_version_major(registrant_ctx->seg_active);
	}
	if (nvmeibt_register_is_client_delete_in_the_air(registrant_ctx)) {
		N_Tf(wldy6cb, "client cid=@CID is being deleted. Skipping", registrant_ctx->client->cid);
		rv = 0;
		goto out;
	}
	rv = nvmeibt_toma_send_msg_to_client(registrant_ctx, praid_version, msg_type, reason, data_length, data, msg_id);
out:
	NFOUT;
	return rv;
}

BOOL nvmeibt_register_is_any_registered_on_seg_active(const struct nvmeibt_seg_active *seg_active)
{
	NTOMA_ASSERT(error_register_nvmeibt_register_is_any_registered_on_seg_active, seg_active, "OOPS, called with seg_active=NULL");
	return (nvmeibt_seg_active_n_active_registrants(seg_active) != 0);
}

BOOL nvmeibt_register_is_any_registered_on_local_disk(const struct nvmeibt_local_disk *local_disk)
{
	BOOL							is_any = 0;
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	if (!local_disk) {
		goto out;
	}
	NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
		if (nvmeibt_register_is_any_registered_on_seg_active(seg_active)) {
			dump_seg_active_registrants(seg_active, 0);
			is_any = 1;
			goto out;
		}
	}
out:
	NFOUT;
	return is_any;
}

BOOL nvmeibt_register_is_any_registered(void)
{
	struct nvmeibt_local_disk	*local_disk;
	BOOL	rv = 0;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		rv |= nvmeibt_register_is_any_registered_on_local_disk(local_disk);
		if (rv) {
			goto out;
		}
	}
out:
	NFOUT_rv;
	return rv;
}

static struct nvmeibt_registrant_ctx *get_active_registrant_by_client_messaging_handle(
	struct nvmeibt_seg_active		*seg_active,
	struct nvmeibt_registrant_ctx	*input_reg_ctx)
{
	struct nvmeibt_registrant_ctx 	*reg_ctx;

	NFIN;
	reg_ctx = nvmeib_hash_search_uint64_t(seg_active->active_registrants_hash_by_handle, input_reg_ctx->client_messaging_handle);
	if (reg_ctx) {
		N_Tf(v6xjko2, "Found active registrant lockid=@LOCKID messaging_handle=@HANDLE node=@NODE registrant_disconnect_time=@ZU",
			 nvmeib_lockid_purify(reg_ctx->reg_lock_id), reg_ctx->client_messaging_handle,
			 nvmeibt_client_get_hostname(reg_ctx->client), reg_ctx->reg_disconnect_time.tv_sec);
		if (nvmeibt_register_is_processing_registrant_removal(reg_ctx)) {
			N_Tf(bvkospf, "Already is_processing_registrant_removal");
		}
		if (is_registrant_on_timeout(reg_ctx)) {
			N_Tf(nbxjlk2, "Already is_registrant_on_timeout");
		}
	}
	NFOUT;
	return reg_ctx;
}

static bool is_seg_active_reservation_mode_version_registrable(struct nvmeibt_seg_active *seg_active)
{
	// Reject if
	// 1. Not fully committed
	// 2. We have registrants with an old version
	bool    is_rejecting = (is_reservation_mode_version_ahead(seg_active->highest_reservation_mode_version, seg_active->committed_reservation_mode_version)
							||
							(is_reservation_mode_version_ahead(seg_active->highest_reservation_mode_version, seg_active->active_reservation_mode_version) &&
							 nvmeibt_seg_active_n_active_registrants(seg_active) > 0));
	if (is_rejecting) {
		N_Tf(t_xx_301, "seg=@UUID_8 highest_@RES_MOD_VER committed_@RES_MOD_VER active_@RES_MOD_VER n_active=@N_ACTIVE",
			nvmeibt_seg_active_UUID_8(seg_active), seg_active->highest_reservation_mode_version,
			seg_active->committed_reservation_mode_version, seg_active->active_reservation_mode_version,
			nvmeibt_seg_active_n_active_registrants(seg_active));
	}
	return !is_rejecting;
}

bool nvmeibt_register_is_seg_lot_registrable_topo_wise(struct nvmeibt_seg_lot *seg_lot, int *reason)
{
	int										rv = 0;
	struct nvmeibt_praid					*praid = nvmeibt_disk_segment_get_praid(seg_lot->my_seg);
	int										dummy;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo = &seg_lot->seg_topo;
	struct nvmeibt_praid_topo_ctx			*praid_topo = &seg_lot->praid_lot->topo_ctx;

	if (!reason) {
		reason = &dummy;
	}
	// Only based on topology (&config) properties. Not related to identified gaps and issues
	// Every seg that received this topo from the leader will generate the same topo_for_clients
	if (nvmeibt_seg_lot_is_deleted_in_config(seg_lot)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_DELETING_SEG;
		N_Tf(dhhhu83, "seg=@UUID_8 is_being_deleted", nvmeibt_seg_lot_UUID_8(seg_lot));
	} else if (!(praid_topo->is_activated)) {
		 *reason = (praid->was_praid_ever_activated ?
					 NVMEIBT_CLIENT_TR_REASON_PRAID_NOT_ACTIVATED : NVMEIBT_CLIENT_TR_REASON_PRAID_NEVER_ACTIVATED);
		 N_Tf(aasji85, "praid=@UUID_LE not activated", nvmeibt_praid_UUID(praid));
	} else if (!nvmeibt_disk_segment_is_dirty_bits_state_registrable(seg_topo->dirty_bits_state)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_NOT_REGISTRABLE;
		N_Tf(fhhy573, "seg=@UUID_8 NOT_REGISTRABLE, dirty_bits_state=@DIRTY_BITS_STATE",
			 nvmeibt_seg_lot_UUID_8(seg_lot),
			 dirty_bits_state_str(seg_topo->dirty_bits_state));
//	The following is not used, since it is checked in is_seg_active_accepting_registrations(), and should not affect topo_for_clients (R/W / W / DEAD)
//	} else if (!nvmeibt_disk_segment_is_mem_tbl_init_done_fully(seg_topo) ) {
//		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_INITIALIZING;
//		N_Tf(gs74tyd, "seg=@UUID_8 NOT_REGISTRABLE, dirty_bits_state=@DIRTY_BITS_STATE", nvmeibt_seg_lot_UUID_8(seg_lot), dirty_bits_state_str(seg_topo->dirty_bits_state));
	} else {
		*reason = NVMEIBT_CLIENT_TR_REASON_NONE;
		N_Tf(rooik56, "seg=@UUID_8 REGISTRABLE", nvmeibt_seg_lot_UUID_8(seg_lot));
		rv = 1;
	}

	// FOUT_rv;
	return rv;
}

BOOL nvmeibt_register_is_seg_active_accepting_registrations(struct nvmeibt_seg_active *seg_active, int *reason)
{
	int											rv = 0;
	int											dummy = 0;
	struct nvmeibt_disk_segment					*disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	struct nvmeibt_seg_lot						*applied_seg_lot = &disk_segment->seg_follower.applied_seg_lot;
	struct nvmeibt_local_disk					*local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	struct nvmeibt_praid						*praid = nvmeibt_disk_segment_get_praid(disk_segment);
	struct nvmeibt_seg_active_metadata_ctrl		*persistent_metadata = nvmeibt_seg_active_get_persistent_metadata(seg_active);
	struct nvmeibt_disk_segment_topo_ctx		*active_seg_topo = &seg_active->active_seg_topo;

	NFIN;
	if (!reason) {
		reason = &dummy;
	}
	if (!nvmeibt_local_disk_is_ready_for_segments(local_disk)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_NON_LOCAL_DISK;
	} else if (!nvmeibt_local_disk_is_connected_to_disk(local_disk)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_NO_DISK_IN_CONFIG;
	} else if (!praid) {
		*reason = NVMEIBT_CLIENT_TR_REASON_INTERNAL_ERR;
	} else if (!seg_active->is_seg_registrable) {
		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_NOT_REGISTRABLE;
	} else if (!nvmeibt_register_is_seg_lot_registrable_topo_wise(applied_seg_lot, reason)) {
		// *reason was set in callee function above
	} else if (!nvmeibt_disk_segment_is_mem_tbl_init_done_fully(active_seg_topo)) {
		// Depending on the leader (global), not applied that changes
		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_INITIALIZING;
		N_Tf(t_zzz_10, "seg=@UUID_8 NOT_REGISTRABLE in global_topo, dirty_init=@DIRTY_INIT stale_init=@STALE_INIT",
			 nvmeibt_seg_UUID_8(disk_segment),
			 mem_tbl_init_mode_str(active_seg_topo->dirty_bits_init_mode),
			 mem_tbl_init_mode_str(active_seg_topo->stale_locks_init_mode));
	} else if (!nvmeibt_disk_segment_is_config_OK(disk_segment)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_CONF_CORRUPTED;
	} else if (nvmeibt_local_disk_is_being_deleted(local_disk)) {		// disk_segment's local TOMA currently in-work activities
		*reason = NVMEIBT_CLIENT_TR_REASON_NON_LOCAL_DISK;
	} else if (nvmeibt_seg_active_is_closing_to_reg(seg_active)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_NOT_REGISTRABLE;
	} else if (!nvmeibt_register_is_seg_active_registrable_clients_sync_wise(seg_active, reason)) {
		// *reason was set in callee function above
	} else if (!nvmeibt_seg_active_is_disk_format_zeroing_done_for_me(seg_active)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_BLKS_ZEROING;
	} else if (nvmeibt_seg_active_is_waiting_for_serjio_clean_range_done(seg_active)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_WAIT_4_SERJIO;
	} else if (nvmeibt_raft_is_shutdown_triggered()) { // disk_segment's local TOMA currently in-work activities
		*reason = NVMEIBT_CLIENT_TR_REASON_SHUTDOWN;
	} else if (nvmeibt_seg_active_is_during_persistency_store(seg_active)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_STORING;
	} else if (!persistent_metadata || (persistent_metadata->is_written_on_disk != NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_VALID)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_NOT_STORED;
	} else if (seg_active->is_locktable_on_disk_corrupted) {
		*reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_BROKEN;
	} else if (!is_seg_active_reservation_mode_version_registrable(seg_active)) {
		*reason = NVMEIBT_CLIENT_TR_REASON_UPDATING_RM_VERSION;
	} else {
		*reason = NVMEIBT_CLIENT_TR_REASON_NONE;
		rv = 1;
	}

	if (!rv)
		N_Tf(ddfi87e, "seg=@UUID_8, reason=@STRERROR!", nvmeibt_seg_UUID_8(disk_segment), nvmeibt_protocol_client_msg_reason_str(*reason));

	NFOUT_rv;
	return rv;
}

static void remove_longing_registrant_on_seg(struct nvmeibt_seg_active *seg_active, struct nvmeibt_registrant_ctx *reg_ctx)
{
	NFIN;
	N_Tf(kiru834, "Remove the longing_registrant seg=@UUID_8 handle=@HANDLE",
		nvmeibt_seg_active_UUID_8(seg_active), reg_ctx->client_messaging_handle);
	nvmeibt_register_terminate_reg_ctx(reg_ctx, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_LONGING, 0);
	NFOUT;
}

static void send_registrable_to_longing_registrant_if_eligable(
	struct nvmeibt_seg_active *seg_active, struct nvmeibt_registrant_ctx *longing_registrant)
{
	struct nvmeibt_registrant_ctx	*active_reg_ctx;

	NFIN;
	active_reg_ctx = get_active_registrant_by_client_messaging_handle(seg_active, longing_registrant);
	if (active_reg_ctx) {
		goto out;	// Avoid sending registrable if TOMA_NOT_READY for this client
	}
	longing_registrant->reg_lock_id = get_fresh_reg_lock_id(seg_active);
	if (nvmeib_lockid_purify(longing_registrant->reg_lock_id) == 0) {
		goto out;
	}
	nvmeibt_register_send_msg_to_registrant(longing_registrant, NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT,
											NVMEIBT_CLIENT_TR_REASON_NONE,
											(seg_active ? nvmeibt_tTopoOfPraid_len(&(seg_active->topo_for_clients)) : 0),
											(seg_active ? &(seg_active->topo_for_clients) : NULL));
	remove_longing_registrant_on_seg(seg_active, longing_registrant);
out:
	NFOUT;
}

static void send_registrable_to_all_longing_registrants(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	N_Tf(fjurty3, "seg=@UUID_8 committed_@RES_MOD_VER",
		 nvmeibt_seg_active_UUID_8(seg_active), seg_active->committed_reservation_mode_version);
	if (nvmeibt_register_is_seg_active_accepting_registrations(seg_active, NULL)) {
		struct nvmeibt_registrant_ctx	*reg_ctx;
		// If clients tried to register and received TOMA_NOT_READY, tell them that they can register now
		NVMEIB_HASH_FOREACH(reg_ctx, seg_active->longing_registrants_hash_by_handle) {
			send_registrable_to_longing_registrant_if_eligable(seg_active, reg_ctx);
		}
	}
	NFOUT;
}

// Should be called on every sync_cmd change, client unreg completion, and sw_topo_ack
void nvmeibt_register_clients_sync_check_and_act_upon(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment		*disk_segment;
	struct nvmeibt_praid			*praid;
	bool							is_in_active_life_cycle = 0;

	NFIN;
	if (!seg_active) {
		N_Tf(fhuine4, "seg_active=NULL. Skipping");
		goto out;
	}
	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	praid = nvmeibt_seg_active_get_praid(seg_active);
	if (!nvmeibt_disk_segment_is_config_OK(disk_segment)) {
		NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(gdhy769, seg_active, NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
	}
	else if (	nvmeibt_disk_segment_is_deprecated_in_config(disk_segment) &&
				!nvmeibt_register_is_any_registered_on_seg_active(seg_active) &&
				nvmeibt_seg_active_is_zeroing_state_skipable(seg_active) &&
				nvmeibt_disk_segment_is_x(&(nvmeibt_seg_active_get_applied_seg_lot(seg_active)->seg_topo))) {
		// If already X_DONE, should be caught here.
		nvmeibt_seg_active_mark_as_fully_zeroed(seg_active);
	}
	else if (nvmeibt_raft_is_shutdown_triggered()) {
		N_Tf(gjut822, "Shutdown triggered");
	}
	else if (!(praid->praid_follower.applied_praid_lot.topo_ctx.is_activated)) {
		N_Tf(rri98t5, "praid not is_activated");
	}
	else if (!nvmeibt_disk_segment_is_in_active_life_cycle(nvmeibt_seg_active_dirty_bits_state(seg_active))) {
		N_Tf(aaskir4, "Not in life cycle, dirty_bits_state=@DIRTY_BITS_STATE.", nvmeibt_seg_active_dirty_bits_state_str(seg_active));
	}
	else if (!nvmeibt_local_disk_is_serjio_ready(nvmeibt_seg_active_get_local_disk(seg_active))) {
		N_Tf(ee47j43, "serjio not ready yet.");
	}
	else {
		is_in_active_life_cycle = 1;
	}
	nvmeibt_register_recalc_seg_active_registrants_align_with_sync_cmd(seg_active);
	if (is_in_active_life_cycle) {
		// If we were waiting with the initializations, then now is the time
		nvmeibt_seg_active_init_locks_table(seg_active);
		N_Tf(gkitjre, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		send_registrable_to_all_longing_registrants(seg_active);
		if (nvmeibt_praid_is_type_EC(praid)) {
			nvmeibt_seg_active_notify_serjio_if_seg_is_being_deleted(seg_active);
		}
	}

	nvmeibt_seg_active_zero_if_is_being_deleted_and_unused(seg_active);
	nvmeibt_local_disk_munmap_and_rm_if_should_be_removed_and_unused(nvmeibt_seg_active_get_local_disk(seg_active));
out:
	NFOUT;
}

static BOOL upd_registrant_sync_timeout(struct nvmeibt_registrant_ctx *reg_ctx,
										enum REG_TIMEOUT_REASON timeout_reason)
{
	struct nvmeibt_seg_active	*seg_active;
	int							was_timeout_active;
	int							is_activating_timeout;

	NDUMP_REG_CTX(dhyr763, _Tf, reg_ctx);
	seg_active = reg_ctx->seg_active;
	is_activating_timeout = (timeout_reason != REG_TIMEOUT_REASON_DEL_ME);
	was_timeout_active = !XDLIST_NULL(&(reg_ctx->registrant_on_timeout_link));
	if (is_activating_timeout) {
		if (reg_ctx->timeout_reason && (reg_ctx->timeout_reason != timeout_reason)) {
			N_Tf(fkit983, "Modifying timeout_reason from @TIMEOUT_REASON to @TIMEOUT_REASON", reg_ctx->timeout_reason, timeout_reason);
		}
		reg_ctx->timeout_reason = timeout_reason;
		// If already set, then ignore
		if (was_timeout_active) {
			N_Tf(ww39kr4, "Timeout already active, ignoring");
			goto out_no_change;
		}
	} else {	// Deleting the timeout
		if (!was_timeout_active) {
			N_Tf(ggki943, "Timeout already inactive, ignoring");
			goto out_no_change;
		}
		N_Tf(dkiury5, "Removing from registrants_on_timeout");
		reg_ctx->is_force_cmd_called = 0;	// In case of bug, where force_cmd was lost. Bring it back to normal life
		XDLIST_DEL(&(reg_ctx->registrant_on_timeout_link));
		--seg_active->n_registrants_on_timeout;
		reg_ctx->timeout_time = TIMESPEC_MAX_C99;
		goto out_changed;
	}
	getnstimeofday_boot(&(reg_ctx->timeout_time));
	timespec_update_by_a_few_nsec(&(reg_ctx->timeout_time), MAX_WAIT_FOR_CLIENT_REGISTRANT_TIMEOUT_NSEC);
	// Always add it last, assuming that it is added at time now+MAX_WAIT_FOR_REGISTRANT_TIMEOUT_NSEC
	N_Tf(ski9ry3, "Adding last");
	NXDLIST_ADD_TAIL_CHECK(error_register_upd_registrant_sync_timeout, &seg_active->registrants_on_timeout, reg_ctx);
	seg_active->n_registrants_on_timeout++;
out_changed:
	nvmeibt_global_get_global()->is_need_calc_next_wait_for_registrant_timeout = 1;
out_no_change:
	return was_timeout_active;
}

/**
 * Add registrant to list of those awaiting disconnection of the given
 * stale_lock_id, on the disk segment.
 *
 * @author max (11/20/17)
 *
 * @param seg_active
 * @param stale_lock_id
 * @param registrant_ctx
 */
void add_awaiting_lockid_recipient(
	struct nvmeibt_seg_active *seg_active,
	union nvmeib_lock_id stale_lock_id,
	struct nvmeibt_registrant_ctx *registrant_ctx)
{
	struct nvmeibt_seg_active_awaited_lockid	*awaited_lockid = NULL;
	struct nvmeibt_registrant_awaiting_lockid	*awaiting_registrant_wrapper = NULL;

	NFIN;

	N_Tf(trace_register_add_awaiting_lockid_recipient, "lockid=@T_LID adding handle=@HANDLE to it's list, reg_ctx=@REG_CTX_PTR",
		nvmeib_lockid_purify(stale_lock_id), registrant_ctx->client_messaging_handle, registrant_ctx);

	// Check if we have this lockid in the hash, if so add this reigstrant as ANOTHER recipient in the list.
	awaited_lockid = nvmeib_hash_search_uint32_t(seg_active->awaited_lockids_hash_by_lockid, nvmeib_lockid_purify(stale_lock_id));
	if (awaited_lockid) {
		N_Tf(fjui8t2, "Found @SEG_ACTIVE_N_AWAITED_LOCKIDS recipients awaiting lockid=@T_LID already, adding handle=@HANDLE to it's list",
			nvmeibt_seg_active_n_awaited_lockids(seg_active),
			nvmeib_lockid_purify(stale_lock_id),
			registrant_ctx->client_messaging_handle);
	}

	// If this is the first recipient to currently wait for this lock_id, we need to initialize the recipients list.
	if (!awaited_lockid) {
		awaited_lockid = NNVMEIBT_BM_ALLOC(trace_2_register_add_awaiting_lockid_recipient, sizeof(*awaited_lockid));
		awaited_lockid->lockid_key = stale_lock_id;
		XDLIST_HEAD_INIT(&awaited_lockid->awaiting_registrants);
		nvmeib_hash_add_uint32_t(seg_active->awaited_lockids_hash_by_lockid, nvmeib_lockid_purify(stale_lock_id), awaited_lockid);
		N_Tf(trace_3_register_add_awaiting_lockid_recipient, "init recipients list for lock_id=@T_LID", nvmeib_lockid_purify(stale_lock_id));
	}

	// Allocate the registrant wrapper, and tie it to both the list inside the registrant and the list of those awaiting lock_id X.
	// We need this wrapper as a given registrant can potentially await multiple lockid disconnections, hence cannot have a single
	// link inside the registrant to different lists of the same type (as they will be corrupted in this manner) so we hold
	// a list of link wrapper inside the registrants and the link wrappers are each UNIQUE in a sense that they map the registrant
	// to a given lockid that it awaits to be dropped.
	awaiting_registrant_wrapper = NNVMEIBT_BM_CALLOC(trace_4_register_add_awaiting_lockid_recipient, sizeof(struct nvmeibt_registrant_awaiting_lockid));
	awaiting_registrant_wrapper->reg_ctx = registrant_ctx;
	awaiting_registrant_wrapper->stale_lockid_provided_by_registrant = stale_lock_id;
	XDLIST_INIT_LINK(&(awaiting_registrant_wrapper->awaiting_lockid_link), NULL);
	XDLIST_INIT_LINK(&(awaiting_registrant_wrapper->registrant_link), NULL);
	// Add the wrapper to the list of those awaiting some lock disconnection.
	XDLIST_ADD_TAIL(&(awaited_lockid->awaiting_registrants), awaiting_registrant_wrapper);

	NFOUT;
}

static void search_stale_lock_hash_and_fill_response_cuuid(
		struct nvmeibt_seg_active				*seg_active,
		struct nvmeibt_cleaned_stalock_info		*response_payload,
		union nvmeib_lock_id					search_lockid)
{
	struct stale_lock_ctx			*stale_lock;
	struct nvmeibt_registrant_ctx	*reg_ctx = NULL;

	NFIN;
	// If the client disconnected (not active), left a stale lock behind, and the stale lock is not fully cleaned yet
	// For now, we simply scan all the stale_locks of a seg. If needed, we can add a hash by lockid for this
	lock_stale_locks_hash(seg_active);
	TODO(Add seg_active->stale_locks_hash_by_seg_blkset_no and search directly);
	NVMEIB_HASH_FOREACH(stale_lock, seg_active->stale_locks_hash_by_seg_blkset_no) {
		N_Tf(gkit954, "seg=@UUID_8 comparing @X with @X",
			nvmeibt_seg_active_UUID_8(seg_active),
			nvmeib_lockid_purify(stale_lock->reg_ctx->reg_lock_id),
			nvmeib_lockid_purify(search_lockid));
		if (nvmeib_lockid_are_purified_eq(stale_lock->reg_ctx->reg_lock_id, search_lockid)) {
			reg_ctx = stale_lock->reg_ctx;
			break;
		}
	}
	if (reg_ctx) {
		N_Tf(fhuryew, "Found stale_lock seg=@UUID_8 client_uuid=@UUID_LE lockid=@T_LID",
			nvmeibt_seg_active_UUID_8(seg_active),
			nvmeibt_client_get_urn_uuid_str(stale_lock->reg_ctx->client),
			nvmeib_lockid_purify(reg_ctx->reg_lock_id));
		memcpy(&(response_payload->cuuid), &(stale_lock->reg_ctx->client->client_provided_uuid.bytes), sizeof(response_payload->cuuid));
	} else {
		N_Tf(trace_1_register_search_stale_lock_hash_and_fill_response_cuuid, "No stale_lock lockid=@T_LID", nvmeib_lockid_purify(search_lockid));
		memset(&(response_payload->cuuid), 0, sizeof(response_payload->cuuid));
	}
	unlock_stale_locks_hash(seg_active);
	NFOUT;
}

/* A registrant reports to the toma that holds the stale lock
 *
 */
static int handle_stale_lock_report(struct nvmeibt_register_msg *msg)
{
	int										rv = 0;
	struct nvmeibt_registrant_ctx 			*reporting_registrant_ctx;
	struct nvmeibt_registrant_ctx			*active_registrant_ctx;
	struct nvmeibt_seg_active				*seg_active = msg->registrant_ctx.seg_active;
	int										SEG_UUID_8 = nvmeibt_seg_active_UUID_8(seg_active);
	struct nvmeibt_client_failed_lock_pl	*pl = (struct nvmeibt_client_failed_lock_pl *)(msg->msg_data);
	union nvmeib_lock_id					problematic_lock_id;

	NFIN;

	problematic_lock_id.all = (u32)pl->curr;
	{											// Todo: move to msg payload to_string function
		const union nvmeib_lock_blkset_entry comp_unused = {.all = pl->comp};
		const union nvmeib_lock_blkset_entry xchg_unused = {.all = pl->xchg};
		N_Tf(fjju84e, "seg=@UUID_8, problematic:{seg=@STR, addr=@ADDR, lock_op=@LOCK_OP status=@STATUS @LOCKID cmp=@LOCK_ENT_U64 xchg=@LOCK_ENT_U64}",
			SEG_UUID_8,
			pl->problematic_seg_uuid_str, pl->disk_blkno_4k, pl->lock_op, pl->status,
			problematic_lock_id.all, comp_unused.all, xchg_unused.all);
	}
	reporting_registrant_ctx = nvmeibt_register_lookup_active_registrant_by_reg_lock_id(
					seg_active, msg->registrant_ctx.reg_lock_id);
	if (!reporting_registrant_ctx) {
		N_Wf(djur855, "seg=@UUID_8 reporting_registrant reg_lock_id=@T_LID not found",
			 SEG_UUID_8, msg->registrant_ctx.reg_lock_id.all);
		goto out;
	}
	active_registrant_ctx = nvmeibt_register_lookup_active_registrant_by_reg_lock_id(
					seg_active, problematic_lock_id);

	// Check if an active registrant still exists (maybe it got disconnected before we got the complaint from the reporting_registrant)
	if (!active_registrant_ctx) {
		struct nvmeibt_cleaned_stalock_info		response_payload;
		response_payload.lock_id = problematic_lock_id.all;
		if (problematic_lock_id.bits.is_stale) {
			// Clearly the stale_lockid does not belong to any active registrant, but
			//  it also does not have any traces of stale-locks in the lock_table.
			// We do not remove the active_registrant as long as it has stale locks
			N_Tf(asdr10m, "No active registrant ctx with lockid=@T_LID, seg=@UUID_8",
				nvmeib_lockid_purify(problematic_lock_id), SEG_UUID_8);
			// Create a response payload - containing the original lock_id that the registrant complained about. (with stale bits on)
			search_stale_lock_hash_and_fill_response_cuuid(seg_active, &response_payload, problematic_lock_id);
			//RH: Check if this lockid has stale-locked blksets. Also can compare by segment id of the problematic
			N_Tf(djury74, "No stale. Send NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED to registrant: handle=@HANDLE on seg=@UUID_8 lock_id=@T_LID",
				reporting_registrant_ctx->client_messaging_handle, SEG_UUID_8, response_payload.lock_id);
			rv = nvmeibt_register_send_msg_to_registrant(
							reporting_registrant_ctx,
							NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED,
							NVMEIBT_CLIENT_TR_REASON_NONE,
							sizeof(response_payload),
							&response_payload);
		} else {	// Lock is not stale but registrant does not exist.
			// EC-4987: Minor race, probably the complaint was sent / has arrived too late:
			// lock is already released and its client already disconnected.
			N_Wf(vvgrt66, "seg=@UUID_8, no active registrant ctx for non-stale lock: {seg=@STR, addr=@ADDR, problem=@LOCKID}",
					SEG_UUID_8, pl->problematic_seg_uuid_str, pl->disk_blkno_4k, problematic_lock_id.all);
		}
	} else {
		N_Tf(kkori54, "adding handle=@HANDLE with lockid=@T_LID to awaiting_lockids for seg=@UUID_8",
			reporting_registrant_ctx->client_messaging_handle, nvmeib_lockid_purify(problematic_lock_id), SEG_UUID_8);
		add_awaiting_lockid_recipient(seg_active, problematic_lock_id, reporting_registrant_ctx);

		if (!nvmeibt_register_is_processing_registrant_removal(active_registrant_ctx) && !is_registrant_on_timeout(active_registrant_ctx)) {
			//send unregister to this registrant to get things going
			if (nvmeibt_register_send_msg_to_registrant(
									active_registrant_ctx,
									NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT,
									NVMEIBT_CLIENT_TR_REASON_LOCKID_CLNT_COMPLAIN,
									nvmeibt_tTopoOfPraid_len(&(seg_active->topo_for_clients)),
									&(seg_active->topo_for_clients)) == 0) {
				N_Tf(ddkit95, "sent unregister TR to client reg_lock_id=@C_LID", nvmeib_lockid_purify(problematic_lock_id));
				add_longing_registrant_on_seg(active_registrant_ctx); // So that we will send REGISTRABLE in the future.
			} else {
				N_Wf(fkitu54, "Fail to send unregister TR to client reg_lock_id=@C_LID, wait for client disconnect event",
					nvmeib_lockid_purify(problematic_lock_id));
			}
			upd_registrant_sync_timeout(active_registrant_ctx, REG_TIMEOUT_REASON_UNREG);
		}
	}

out:
	NFOUT;
	return rv;
}

static int handle_failed_cmd_report(struct nvmeibt_register_msg *msg)
{
	int								rv = 0;
	struct nvmeibt_seg_active		*seg_active;

	N_Tf(sjur843, "Client reports about problems seg=@UUID_8", nvmeib_uuid_first_4_bytes(&msg->registrant_ctx.seg_uuid));
	seg_active = nvmeibt_global_get_seg_active_through_seg_by_uuid(&msg->registrant_ctx.seg_uuid);
	if (seg_active)
		seg_active->active_seg_topo.active_seg_flags.did_any_client_report_about_problems = 1;
	else
		N_Wf(oo99m53, "Seg not found");
	return rv;
}

static int handle_data_integrity_issue_report(struct nvmeibt_register_msg *msg)
{
	int										rv = 0;
	struct nvmeibt_registrant_ctx 			*reporting_registrant_ctx = NULL;
	struct nvmeibt_seg_active				*seg_active = msg->registrant_ctx.seg_active;

	NFIN;
	if (seg_active)
		reporting_registrant_ctx = nvmeibt_register_lookup_active_registrant_by_reg_lock_id(
					seg_active, msg->registrant_ctx.reg_lock_id);
	N_ETf(error_register_handle_data_integrity_issue_report, "Received Data Integrity msg from client @MY_HOSTNAME. Aborting.",
			(reporting_registrant_ctx && reporting_registrant_ctx->client) ? reporting_registrant_ctx->client->net.host_name : "(unknown)");
	nvmeibt_abort(ES_FATAL);
	NFOUT;
	return rv;
}

/*
 * - The reg_ctx object is one of active/active+longing/stale/longing/longing_on_invalid_seg
 *   - Here we handle only the specific reg_ctx. Specifically, if it is longing then we do not handle the active registrant that we found
 * - In any case, we will end-up freeing it except for
 *   - active_registrant (finalize) that is moved to stale (the object) whose reg_ctx->n_stale_locks > 0
 *   - stale_registrant that still has locks (reg_ctx->n_stale_locks > 0)
 */
void nvmeibt_register_terminate_reg_ctx(struct nvmeibt_registrant_ctx *reg_ctx, bool is_deleting_seg_active,
										enum NVMEIBT_REGISTER_REGISTRANT_TYPE reg_ctx_type, bool is_move_from_active_reg_hash_to_stale_reg_hash)
{
	struct nvmeibt_seg_active			*seg_active;
	struct nvmeibt_registrant_ctx		*existing_active_registrant = 0;
	bool								is_change_to_active_registrants = 0;

	NFIN;
	NDUMP_REG_CTX(gkitu74, _Tf, reg_ctx);
	seg_active = reg_ctx->seg_active;
	//
	switch (reg_ctx_type) {
	case NVMEIBT_REGISTER_REGISTRANT_TYPE_LONGING:
		if (!is_deleting_seg_active) {
			nvmeib_hash_delete_uint64_t(seg_active->longing_registrants_hash_by_handle, reg_ctx->client_messaging_handle);
		}
		break;
	case NVMEIBT_REGISTER_REGISTRANT_TYPE_LONGING_ON_INVALID_SEG:
		XDLIST_DEL(&(reg_ctx->longing_on_invalid_seg_link));
		break;
	case NVMEIBT_REGISTER_REGISTRANT_TYPE_STALE:
		if (reg_ctx->n_stale_locks > 0) { // If stale_registrant (might be that recently added) and the seg_active is alive then skip it, stale-recovery will retry
			if (!is_deleting_seg_active) {
				N_Tf(djur843, "Skipping. lockid=@T_LID seg=@UUID_8 still has stale_locks n_stale_locks=@INT",
					nvmeib_lockid_purify(reg_ctx->reg_lock_id), nvmeibt_seg_active_UUID_8(seg_active), reg_ctx->n_stale_locks);
				goto out;
			}
			N_Ef(dkitu43, "reg_ctx=@PTR lockid=@T_LID seg=@UUID_8 has stale_locks, is_deleting_seg_active=1",
				 reg_ctx, nvmeib_lockid_purify(reg_ctx->reg_lock_id), nvmeibt_seg_active_UUID_8(seg_active));
			nvmeibt_abort(ES_FATAL);
		}
		nvmeib_hash_delete_uint32_t(seg_active->stale_registrants_hash_by_purified_lockid, nvmeib_lockid_purify(reg_ctx->reg_lock_id));
		break;
	case NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE:
		existing_active_registrant = get_registrant_by_handle_from_active_hash(seg_active, reg_ctx);
		if (!nvmeibt_register_is_processing_registrant_removal(existing_active_registrant) && (existing_active_registrant != reg_ctx)) {
			N_Ef(rcs8l30, "!nvmeibt_register_is_processing_registrant_removal. existing_active_registrant=@PTR != reg_ctx=@PTR ", existing_active_registrant, reg_ctx);
			nvmeibt_abort(ES_FATAL);
		}
		registrant_stopped_being_active(seg_active, reg_ctx, 1);
		is_change_to_active_registrants = 1;
		if (is_move_from_active_reg_hash_to_stale_reg_hash) {	// Called only from registrant_disconnect_finalize(), after all the locks were converted
			if (reg_ctx->n_stale_locks > 0) {
				// Add to stale_registrants. Do not add a stale-registrant that didn't leave stale-locks, since we will never get the drop to 0
				// Stale locks might be freed in the main thread while the WQ is still scanning the locks-table in order to convert to stale
				N_Tf(dkiruu4, "moving active_registrant to stale. seg=@UUID_8 reg_ctx=@PTR lock=@LOCKID n_locks=@INT)",
					 nvmeibt_seg_active_UUID_8(seg_active), reg_ctx, nvmeib_lockid_purify(reg_ctx->reg_lock_id), reg_ctx->n_stale_locks);
				// Actually, move the reg_ctx object from active to stale
				nvmeib_hash_add_uint32_t(seg_active->stale_registrants_hash_by_purified_lockid, nvmeib_lockid_purify(reg_ctx->reg_lock_id), reg_ctx);
				NVMEIBT_SEG_ACTIVE_REMOVE_ACTIVE_REGISTRANT_FROM_HASHES(seg_active, reg_ctx);
				NDUMP_N_ACTIVE_REGISTRANTS(ianwq8i, seg_active);
				goto out;
			} else {
				// We do not need a stale registrant with no stale locks
				// Since it was never added as a stale_registrants, free it as if it is active
			}
		}
		break;
	default:
		N_Wf(favewhw, "Unexpected reg_ctx_type=@INT", reg_ctx_type);
		break;
	}
	nvmeibt_client_reg_ctx_ref_removed(reg_ctx->client, reg_ctx);
	NNVMEIBT_TOMA_FREE(vgbsauy, reg_ctx);
out:
	if (is_change_to_active_registrants) {
		nvmeibt_register_clients_sync_check_and_act_upon(seg_active); // may destroy seg_active
	}
	NFOUT;
}

static void remove_disconnected_client_new_active_registrant_by_lockid(struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id reg_lock_id)
{
	struct nvmeibt_registrant_ctx	*active_reg_ctx;

	NFIN;

	active_reg_ctx = nvmeib_hash_search_uint32_t(seg_active->active_registrants_hash_by_lockid, nvmeib_lockid_purify(reg_lock_id));
	if (active_reg_ctx) {
		nvmeibt_register_terminate_reg_ctx(active_reg_ctx, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE, 0); // The registrant never registrated (it is new)
	} else {
		N_Tf(trace_register_remove_client_active_registrant_by_lockid, "Did not find active registrant reg_lock_id=@C_LID", nvmeib_lockid_purify(reg_lock_id));
	}
	NFOUT;
}

void nvmeibt_register_eliminate_all_active_registrants_and_stales_of_seg_due_to_locks_table_reset(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_registrant_ctx	*reg_ctx;

	NFIN;
	// Remove leftovers of stale_locks. This will also remove the stale_registrants
	NVMEIB_HASH_FOREACH(reg_ctx, seg_active->stale_registrants_hash_by_purified_lockid) {
	    nvmeibt_seg_active_delete_all_stale_locks_of_registrant(seg_active, reg_ctx);
	}
	NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_lockid) {
		nvmeibt_register_terminate_reg_ctx(reg_ctx, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE, 0);
	}
	NFOUT;
}

struct nvmeibt_registrant_ctx *nvmeibt_register_lookup_stale_registrant_by_reg_lock_id(
	struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id reg_lock_id)
{
	struct nvmeibt_registrant_ctx	*reg_ctx;

	reg_ctx = nvmeib_hash_search_uint32_t(seg_active->stale_registrants_hash_by_purified_lockid, nvmeib_lockid_purify(reg_lock_id));
	if (reg_ctx) {
		N_Tf(floti94, "Found stale registrant seg=@UUID_8 reg_lock_id=@C_LID",
			 nvmeibt_seg_active_UUID_8(seg_active), nvmeib_lockid_purify(reg_ctx->reg_lock_id));
	}
	return reg_ctx;
}

struct nvmeibt_registrant_ctx *nvmeibt_register_lookup_active_registrant_by_reg_lock_id(
	struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id reg_lock_id)
{
	struct nvmeibt_registrant_ctx	*reg_ctx;

	reg_ctx = nvmeib_hash_search_uint32_t(seg_active->active_registrants_hash_by_lockid, nvmeib_lockid_purify(reg_lock_id));
	if (reg_ctx) {
		N_Tf(fjiut75, "Found active registrant seg=@UUID_8 reg_lock_id=@C_LID client_disconnext_time=@LLD",
			 nvmeibt_seg_active_UUID_8(seg_active), nvmeib_lockid_purify(reg_ctx->reg_lock_id), reg_ctx->reg_disconnect_time.tv_sec);
	}
	return reg_ctx;
}

static void add_longing_registrant_on_seg(struct nvmeibt_registrant_ctx *input_reg_ctx)
{
	struct nvmeibt_seg_active		*seg_active = input_reg_ctx->seg_active;
	struct nvmeibt_registrant_ctx	*reg_ctx;
	struct nvmeibt_registrant_ctx	*old_reg_ctx;

	NFIN;
	if (nvmeibt_toma_is_in_shutdown()) {
		N_Tf(t98ksoc, "Skipping. toma_is_in_shutdown");
		goto out;
	}
	if (nvmeibt_seg_active_n_longing_registrants(seg_active) >= NVMEIBT_MAX_N_CLIENTS_PER_DISK_SEGMENT) {
		N_Wf(fkiuree,"seg_active->n_longing_registrants=@N_LONGING_REGISTRANTS > @MAX_N_CLIENTS_PER_NODE",
			 nvmeibt_seg_active_n_longing_registrants(seg_active),
			 NVMEIBT_MAX_N_CLIENTS_PER_DISK_SEGMENT);
	}
	alloc_reg_ctx(&reg_ctx, input_reg_ctx);
	old_reg_ctx = nvmeib_hash_add_uint64_t(seg_active->longing_registrants_hash_by_handle, reg_ctx->client_messaging_handle, reg_ctx);
	if (old_reg_ctx) {
		N_Tf(dkiru43, "Already exists seg=@UUID_8 longing=(@LLX,@X)", nvmeibt_seg_active_UUID_8(seg_active),
			 old_reg_ctx->client_messaging_handle, nvmeib_lockid_purify(old_reg_ctx->reg_lock_id));
	}
	NDUMP_REG_CTX(fjiur74, _Tf, reg_ctx);
out:
	NFOUT;
}

void remove_unsubscribed_specific_longing_registrant_on_invalid_seg(struct nvmeibt_registrant_ctx *longing_registrant)
{
	// Delete longing registrant from the list on invalid seg.
	N_Tf(ju87cwe, "Found longing registrant handle=@HANDLE seg=@UUID_8",
		 longing_registrant->client_messaging_handle, nvmeib_uuid_first_4_bytes(&longing_registrant->seg_uuid));
	nvmeibt_register_terminate_reg_ctx(longing_registrant, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_LONGING_ON_INVALID_SEG, 0);
}

bool remove_longing_registrant_on_seg_by_ctx(struct nvmeibt_registrant_ctx *input_reg_ctx)
{
	struct nvmeibt_registrant_ctx	*longing_registrant;

	longing_registrant = nvmeib_hash_search_uint64_t(input_reg_ctx->seg_active->longing_registrants_hash_by_handle, input_reg_ctx->client_messaging_handle);
	if (longing_registrant) {
		remove_longing_registrant_on_seg(input_reg_ctx->seg_active, longing_registrant);
	}
	return 0;
}

static void add_longing_registrant_on_invalid_seg(struct nvmeibt_registrant_ctx *input_reg_ctx)
{
	struct nvmeibt_registrant_ctx	*longing_registrant;

	NFIN;
	XDLIST_FOREACH(longing_registrant, &(nvmeibt_global_get_global()->longing_on_invalid_seg_list_by_handle)) {
		if (longing_registrant->client_messaging_handle == input_reg_ctx->client_messaging_handle) {
			if (!ARE_UUID_EQ(&(longing_registrant->seg_uuid), &(input_reg_ctx->seg_uuid))) {
				N_Wf(bd6qu2m, "handle=@LLX seg:(longing=@UUID_8 != input=@UUID_8", input_reg_ctx->client_messaging_handle,
					 nvmeib_uuid_first_4_bytes(&(longing_registrant->seg_uuid)), nvmeib_uuid_first_4_bytes(&(input_reg_ctx->seg_uuid)));
			}
			N_Tf(dkiru84, "Already exists");
			goto out;
		}
	}
	alloc_reg_ctx(&longing_registrant, input_reg_ctx);
	XDLIST_ADD_TAIL(&(nvmeibt_global_get_global()->longing_on_invalid_seg_list_by_handle), longing_registrant);
out:
	NFOUT;
}

void nvmeibt_register_move_all_my_longing_registrants_on_invalid_seg_to_my_longing(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_registrant_ctx	*longing_registrant;

	NFIN;
	XDLIST_FOREACH_SAFE(longing_registrant, &(nvmeibt_global_get_global()->longing_on_invalid_seg_list_by_handle)) {
		if (ARE_UUID_EQ(&(longing_registrant->seg_uuid), nvmeibt_seg_active_UUID(seg_active))) {
			longing_registrant->seg_active = seg_active;
			// Delete longing registrant from the list on invalid seg.
			// Add registrant as longing on current segment.
			add_longing_registrant_on_seg(longing_registrant);
			// Free the reg_ctx (when adding registrant as longing to segment new reg_ctx is allocated.
			longing_registrant->seg_active = NULL;
			nvmeibt_register_terminate_reg_ctx(longing_registrant, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_LONGING_ON_INVALID_SEG, 0);
		}
	}
	NFOUT;
}

void nvmeibt_register_remove_unsubscribed_longing_registrant_on_invalid_seg(unsigned long long closed_messaging_handle, bool is_complete_removal_from_all_segs)
{
	struct nvmeibt_registrant_ctx	*longing_registrant;

	NFIN;
	XDLIST_FOREACH_SAFE(longing_registrant, &(nvmeibt_global_get_global()->longing_on_invalid_seg_list_by_handle)) {
		if (client_messaging_handle_to_cid(longing_registrant->client_messaging_handle) == client_messaging_handle_to_cid(closed_messaging_handle)) {
			remove_unsubscribed_specific_longing_registrant_on_invalid_seg(longing_registrant);
			if (is_complete_removal_from_all_segs) {
				continue;
			} else if (longing_registrant->client_messaging_handle == closed_messaging_handle) {
				break;	// There should be only one that matches
			}
		}
	}
	NFOUT;
}

static void owner_locks_release_group_free(struct nvmeibt_wq_entry *wq_entry)
{
	struct owner_locks_release_wq_entry *entry;

	entry = container_of(wq_entry, struct owner_locks_release_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_owner_locks_release_group_free, entry);
}


static void owner_locks_release_group_wrapper(struct nvmeibt_wq_entry *wq_entry);
void owner_locks_set_to_release(struct registrant_disconnect_wq_entry *entry)
{
	struct owner_locks_release_wq_entry *owner_locks_release_group_task;
	struct nvmeibt_seg_active *seg_active = entry->seg_active;

	XDLIST_ADD_TAIL(&seg_active->owner_lock_ids_to_release, &entry->wq_entry);
	if (XDLIST_N_ELEMNTS(&seg_active->owner_lock_ids_to_release) == 1) {
		owner_locks_release_group_task = NNVMEIBT_BM_CALLOC(trace_owner_locks_set_to_release, sizeof *owner_locks_release_group_task);
		owner_locks_release_group_task->wq_entry.type = "OWNER_LOCKS_RELEASE_GROUP";
		owner_locks_release_group_task->wq_entry.execute = owner_locks_release_group_wrapper;
		// This owner_locks_release_group_task is not created in TOMA's main thread like
		//  all the other WQ tasks. It also does not relate to a specific TOMA object
		//  that has anything to do on finalize.
		// We do specify its free(), although will probably be called differently.
		owner_locks_release_group_task->wq_entry.free = owner_locks_release_group_free;	// Triggered by the nvmeibt_toma_trigger_wakeup() when done
		owner_locks_release_group_task->wq_entry.finalize = NULL;	// It is just a wrapper for the group, no effects on toma objects' workflow and state
		owner_locks_release_group_task->seg_active = seg_active;
		owner_locks_release_group_task->local_disk = nvmeibt_seg_active_get_local_disk(seg_active);

		if (nvmeibt_wq_addw(entry->wq_entry.wq, &owner_locks_release_group_task->wq_entry) < 0) {
			N_Ef(djjiu87, "failed to add release group wq_entry");
			nvmeibt_abort(ES_FATAL);
		}
	}
}

#define nvmeib_lock_id_set_is_stale(lid) (lid)->bits.is_stale = 1
static void owner_locks_release_group_wrapper(struct nvmeibt_wq_entry *owner_locks_release_group_task_wq_entry)
{
	struct nvmeibt_wq_entry						*wq_entry;
	struct nvmeibt_seg_active					*seg_active;
	uint64_t									n_blksets;
	uint64_t									ii;
	union nvmeib_lock_id						write_lock_id;
	union nvmeib_lock_blkset_entry				*mmapped_locks_table;
	struct stale_lock_ctx						*already_existing_stale_lock;
	struct nvmeibt_disk_segment					*disk_segment;
	struct {
		union nvmeib_lock_id						stale_lock_id;
		u32											input_lock_id;
		struct registrant_disconnect_wq_entry		*entry;
	} 											*released_lock_ids = 0;
	int											n_released_lock_ids, n = 0;
	struct registrant_disconnect_wq_entry		*reg_disconnect_entry;
	u32											map_lock_id_purified;
	struct nvmeibt_local_disk					*local_disk;

	NFIN;
	seg_active = container_of(owner_locks_release_group_task_wq_entry, struct owner_locks_release_wq_entry, wq_entry)->seg_active;
	local_disk = container_of(owner_locks_release_group_task_wq_entry, struct owner_locks_release_wq_entry, wq_entry)->local_disk;
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(twr59ih, "local_disk=@STR is_being_deleted. No need to add/modify entries (of stale) in memory", nvmeibt_local_disk_display(local_disk));
		goto done_with_wq_entry;
	}

	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	if (!disk_segment) {
		N_Ef(t_3j_toma_reg, "No seg for seg_active");
		nvmeibt_abort(ES_FATAL);
	}

	mmapped_locks_table = nvmeibt_seg_active_get_locks_tbl_ptr(seg_active);

	if (!mmapped_locks_table) {
		N_Wf(dkki987, "seg=@UUID_8 locks table is NOT mmapped, disk was probably removed", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	n_released_lock_ids = XDLIST_N_ELEMNTS(&seg_active->owner_lock_ids_to_release);
	N_Tf(skiure2, "n_locks_to_release=@X seg=@UUID_8", n_released_lock_ids, nvmeibt_seg_active_UUID_8(seg_active));
	if (n_released_lock_ids <= 0)
		goto out;

	released_lock_ids = NNVMEIBT_TOMA_MALLOC(trace_1_owner_locks_release_group, n_released_lock_ids * sizeof(*released_lock_ids));

	XDLIST_FOREACH(wq_entry, &seg_active->owner_lock_ids_to_release) {
		reg_disconnect_entry = container_of(wq_entry, struct registrant_disconnect_wq_entry, wq_entry);
		released_lock_ids[n].input_lock_id = nvmeib_lockid_purify(reg_disconnect_entry->reg_ctx->reg_lock_id);
		released_lock_ids[n].stale_lock_id = reg_disconnect_entry->reg_ctx->reg_lock_id;
		released_lock_ids[n].entry = reg_disconnect_entry;
		nvmeib_lock_id_set_is_stale(&released_lock_ids[n].stale_lock_id);
		if (released_lock_ids[n].input_lock_id == 0) {
			N_Wf(t_3m_toma_reg, "Turned on stale with lockid==0");
		}
		N_Tf(dkiu876, "Converting (@INT) all @X-->@X seg=@UUID_8",
			 n,
			 released_lock_ids[n].input_lock_id,
			 released_lock_ids[n].stale_lock_id.all,
			 nvmeibt_seg_active_UUID_8(seg_active));
		n++;
	}

	// YR: TODO: add in qsort once regular search is debugged
	// qsort();

//	populate_am_I_owner_of_seg_idx_tbl(disk_segment, am_I_owner_of_seg_idx_tbl);
	n_blksets = num_blksets_in_disk_segment(disk_segment);
	for (ii = 0; ii < n_blksets; ii++) {
		const union nvmeib_lock_id existing_lock_id = mmapped_locks_table[ii].lock_id;
		if ((existing_lock_id.all == 0) || (existing_lock_id.all == nvmeib_stale_special_raid1.lock_id.all)) {// Free (the dominant case) or already stale special (post R1 cold-recovery)
			continue;
		}
#if 0	// Todo Enable, once we are sure that clients behaviour NVMESH-7407 is correct and remove the related Error print below, grep this ticket id
		if (existing_lock_id.bits.is_stale)
			continue;							// Toma has nothing to do with this lock. No need to remove .is_read bit
#endif

		map_lock_id_purified = nvmeib_lockid_purify(existing_lock_id);
		// YR: TODO: change with binary search once regular search is debugged
		for (n = 0; n < n_released_lock_ids; ++n) {
			if (map_lock_id_purified != released_lock_ids[n].input_lock_id)
				continue;

			// Since only this registrant_disconnect can update this entry, do it directly
			//  Do not worry about RDMA read caches. We find it hard to believe that they exist (== disable polling over RDMA)
			already_existing_stale_lock = nvmeibt_seg_active_add_blkset_to_stale_locks_hash(released_lock_ids[n].entry->reg_ctx, ii, existing_lock_id);
			if (!already_existing_stale_lock) {
				write_lock_id = released_lock_ids[n].stale_lock_id;
				// write_lock_id.bits.is_read = existing_lock_id.bits.is_read; // Daniel: Impossible, client will not send blockset recovered on this blckset and Toma Hash will explode
			} else {
				// A recoverer locked and unregistered. We need to restore the value that it tried to fix (recoveree clients lock)
				write_lock_id = already_existing_stale_lock->lockid_that_was_left_behind;
				nvmeib_lock_id_set_is_stale(&write_lock_id);
				// A client that does the sync (recoverer)
				// - locks with is_read=1
				// - sync
				// - Send BLKSET_RECOVERED
				// - gets confirmation
				// - unlock
				if (!(existing_lock_id.bits.is_read)) {		// The last thing that happened is A non-recoverer locked for Write
					write_lock_id = released_lock_ids[n].stale_lock_id;	// The better option is not to restore to the value from the hash
						// Remove it from the hash
					N_Ef(dki98u7, "Probably missed blockset recovered msg! seg=@UUID_8 blkset=@LLX lockConvert @X-->@X",
						 nvmeibt_seg_active_UUID_8(seg_active), (unsigned long long)ii, existing_lock_id.all, write_lock_id.all);
				}
				if (existing_lock_id.bits.is_stale) {		// This is a data corruption situation. Recoverer unlocked to its stale lock. Should have abandoned or unlock to recoveree lock.
					write_lock_id = released_lock_ids[n].stale_lock_id;	// The better option is not to restore to the value from the hash
					N_Ef(dki98u8, "Client bug: NVMESH-7407, unlocked to invalid stale lock, possible DI issue! seg=@UUID_8 blkset=@LLX lockConvert @X-->@X",
						 nvmeibt_seg_active_UUID_8(seg_active), (unsigned long long)ii, existing_lock_id.all, write_lock_id.all);
				}
			}
			N_Tf(awee8ju, "seg=@UUID_8 seg_blkset=@LLX lockConvert @X-->@X",
				nvmeibt_seg_active_UUID_8(seg_active), (unsigned long long)ii, existing_lock_id.all, write_lock_id.all);
			mmapped_locks_table[ii].lock_id = write_lock_id;
			if (write_lock_id.all) {
				released_lock_ids[n].entry->n_owner_locks_converted_to_stale++;
			} else {
				released_lock_ids[n].entry->n_owner_locks_converted_to_zero++;
			}
			break;
		}
	}
	NNVMEIBT_TOMA_FREE(trace_13_owner_locks_release_group, released_lock_ids);

done_with_wq_entry:
	XDLIST_FOREACH_SAFE(wq_entry, &seg_active->owner_lock_ids_to_release) {
		reg_disconnect_entry = container_of(wq_entry, struct registrant_disconnect_wq_entry, wq_entry);
		N_Tf(djji87e, "registrant_disconnect success seg=@UUID_8 client messaging_handle=@HANDLE stale=@LOCKID",
			nvmeibt_seg_active_UUID_8(seg_active),
			reg_disconnect_entry->reg_ctx->client_messaging_handle,
			nvmeib_lockid_purify(reg_disconnect_entry->reg_ctx->reg_lock_id));
		XDLIST_DEL(&wq_entry->link);
		nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *)wq_entry);
	}

out:
	if (owner_locks_release_group_task_wq_entry->finalize) {
		N_Ef(dkii982, "Group release should not have a finalize()");
		nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) owner_locks_release_group_task_wq_entry);
	} else {
		// Since this task was created in this wq thread, we also want to free it here directly, and not in the main thread.
		// Not calling: nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) owner_locks_release_group_task_wq_entry);
		// This will avoid races.
		owner_locks_release_group_task_wq_entry->free(owner_locks_release_group_task_wq_entry);
	}

	NFOUT;
}

static void registrant_disconnect_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct registrant_disconnect_wq_entry 	*entry;
	struct nvmeibt_seg_active				*seg_active;

	NFIN;
	entry = container_of(wq_entry, struct registrant_disconnect_wq_entry, wq_entry);
	seg_active = entry->seg_active;
	if (nvmeibt_local_disk_is_being_deleted(nvmeibt_seg_active_get_local_disk(seg_active))) {
		N_Tf(04kalf8, "No local_disk. Skipping stale_locks conversion. Still need to complete the registrant_disconnect");
		nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *)wq_entry);
		goto out;
	}

	N_Tf(dkki98q, "start registrant_disconnect stale-locks Release seg=@UUID_8 handle=@HANDLE reg=@LOCKID",
		nvmeibt_seg_active_UUID_8(seg_active),
		entry->reg_ctx->client_messaging_handle,
		nvmeib_lockid_purify(entry->reg_ctx->reg_lock_id));
	owner_locks_set_to_release(entry);
out:
	NFOUT;
}

static void registrant_disconnect_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct registrant_disconnect_wq_entry *entry;

	entry = container_of(wq_entry, struct registrant_disconnect_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(jji9883, entry);
}

static void registrant_disconnect_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct nvmeibt_seg_active_awaited_lockid	*awaited_lockid;
	struct registrant_disconnect_wq_entry		*entry;
	struct nvmeibt_seg_active					*seg_active;
	struct nvmeibt_disk_segment					*disk_segment;
	struct nvmeibt_registrant_ctx				*active_reg_ctx;
	struct nvmeibt_registrant_awaiting_lockid	*awaiting_registrant_wrapper;

	NFIN;

	entry = container_of(wq_entry, struct registrant_disconnect_wq_entry, wq_entry);

	if (wq_entry->is_canceled) {
		// Nothing in this case
		// OL: WRONG, FIX THIS?
	}

	seg_active = entry->seg_active;
	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	active_reg_ctx = entry->reg_ctx;

	if (entry->n_owner_locks_converted_to_stale) {
		N_Tf(vvnnh86, "seg=@UUID_8 marking stale rebuild required, n_owner_locks_converted_to_stale=@N_OWNER_LOCKS_CONVERTED_TO_STALE",
			nvmeibt_seg_active_UUID_8(seg_active), entry->n_owner_locks_converted_to_stale);
		nvmeibt_seg_active_mark_stale_rebuild_required(seg_active);
	}
	N_Tf(djiur85, "registrant_disconnect succeeded on seg=@UUID_8 n_owner_locks_converted_to_stale=@N_OWNER_LOCKS_CONVERTED_TO_STALE client handle=@HANDLE reg_lock_id=@C_LID n_awaited_lockids=@N_AWAITED_LOCKIDS",
		nvmeibt_seg_active_UUID_8(seg_active),
		entry->n_owner_locks_converted_to_stale,
		active_reg_ctx->client_messaging_handle,
		nvmeib_lockid_purify(active_reg_ctx->reg_lock_id),
		nvmeibt_seg_active_n_awaited_lockids(seg_active));

	awaited_lockid = nvmeib_hash_search_uint32_t(seg_active->awaited_lockids_hash_by_lockid, nvmeib_lockid_purify(active_reg_ctx->reg_lock_id));
	if (awaited_lockid) {
		N_Tf(bvhht73, "client handle=@HANDLE reg_lock_id=@C_LID pending list size=@SIZE",
			 active_reg_ctx->client_messaging_handle, nvmeib_lockid_purify(active_reg_ctx->reg_lock_id), XDLIST_N_ELEMNTS(&(awaited_lockid->awaiting_registrants)));

		// Go over the list of all the recipients that are expected to know that this given lockid was dropped.
		XDLIST_FOREACH_SAFE(awaiting_registrant_wrapper, &(awaited_lockid->awaiting_registrants)) {
			struct nvmeibt_registrant_ctx *awaiting_reg_ctx = awaiting_registrant_wrapper->reg_ctx;

			// Create a response payload - containing the original lock_id that the registrant complained about. (with stale bits on)
			struct nvmeibt_cleaned_stalock_info response_payload;
			response_payload.lock_id = awaiting_registrant_wrapper->stale_lockid_provided_by_registrant.all;
			search_stale_lock_hash_and_fill_response_cuuid(seg_active, &response_payload, active_reg_ctx->reg_lock_id);

			N_Tf(ddhhuu5, "Sending NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED to handle=@HANDLE on seg=@UUID_8 lock_id=@T_LID",
				 awaiting_reg_ctx->client_messaging_handle, nvmeibt_seg_active_UUID_8(seg_active), response_payload.lock_id);
			TODO(actually, we can send the LOCK_CLEANED right after we know the client is unregistered. But make sure that the client only work with stale-locks);
			if (nvmeibt_register_send_msg_to_registrant(awaiting_reg_ctx,
														NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED,
														NVMEIBT_CLIENT_TR_REASON_NONE,
														sizeof(response_payload),
														&response_payload) < 0) {
				N_Wf(cc667qs, "Unable to send lockid_dropped for @LOCKID on seg=@UUID_8 clnt_host=@STR",
					 nvmeib_lockid_purify(awaiting_reg_ctx->reg_lock_id), nvmeibt_seg_active_UUID_8(seg_active),
					 awaiting_reg_ctx->client->net.host_name);
			}
			// Delete the wrapper from both lists in the registrant and in the list of those awaiting lockid X removal.
			XDLIST_DEL(&awaiting_registrant_wrapper->awaiting_lockid_link);
			XDLIST_DEL(&awaiting_registrant_wrapper->registrant_link);
			// Free the wrapper struct as it is no longer needed.
			NNVMEIBT_BM_FREE(trace_4_register_registrant_disconnect_finalize, awaiting_registrant_wrapper);
		}
		nvmeib_hash_delete_uint32_t(seg_active->awaited_lockids_hash_by_lockid, nvmeib_lockid_purify(active_reg_ctx->reg_lock_id));
		NNVMEIBT_BM_FREE(trace_5_register_registrant_disconnect_finalize, awaited_lockid);
	}

	// go over all the hash and all the lists inside and delete disconnected registrant from
	// all the waiting lists - since it is now disconnected and won't be able to get any reply - EVER!
	awaited_lockid = NULL;
	NVMEIB_HASH_FOREACH(awaited_lockid, seg_active->awaited_lockids_hash_by_lockid) {
		N_Tf(registrant_disconnect_finalize_1, "Clearing disconnected registrants for lockid=@T_LID num_recipients=@INT",
			nvmeib_lockid_purify(awaited_lockid->lockid_key), XDLIST_N_ELEMNTS(&(awaited_lockid->awaiting_registrants)));
		TODO(Convert XDLIST awaited_lockid->awaiting_registrants to awaited_lockid->awaiting_registrants_hash_by_client_messaging_handle. Well, usually there are very few);
		XDLIST_FOREACH_SAFE(awaiting_registrant_wrapper, &(awaited_lockid->awaiting_registrants)) {
			struct nvmeibt_registrant_ctx *awaiting_reg_ctx = awaiting_registrant_wrapper->reg_ctx;
			if (nvmeibt_register_is_same_registrant(awaiting_reg_ctx, active_reg_ctx)) {
				NDUMP_REG_CTX(oiujf87, _Tf, awaiting_reg_ctx);
				// Remove this recipient from the list of those pending response - since the recipient itslef was just disconnected.
				XDLIST_DEL(&awaiting_registrant_wrapper->awaiting_lockid_link);
				// Remove also from the list of locks the registrant is awaiting.
				XDLIST_DEL(&awaiting_registrant_wrapper->registrant_link);
				// Free the wrapper struct as it is no longer needed.
				NNVMEIBT_BM_FREE(registrant_disconnect_finalize_3, awaiting_registrant_wrapper);
			}
		}
		// If this was the last recipient for some lockid - we remove its entry from the hash.
		if (XDLIST_EMPTY(&(awaited_lockid->awaiting_registrants))) {
			nvmeib_hash_delete_uint32_t(seg_active->awaited_lockids_hash_by_lockid, nvmeib_lockid_purify(awaited_lockid->lockid_key));
		}
	}

	if (active_reg_ctx->is_client_waiting_for_ack) {
		if (nvmeibt_register_send_msg_to_registrant(active_reg_ctx, NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK,
													NVMEIBT_CLIENT_TR_REASON_NONE, 0, NULL) < 0) {
			N_Tf(trace_zzz_12, "Fail to send unregister-ACK to (reg_lock_id=@C_LID)", nvmeib_lockid_purify(active_reg_ctx->reg_lock_id));
		}
	}

	if (nvmeibt_disk_segment_get_seg_active(disk_segment))
		seg_active->last_registrant_disconnect_timespec = nvmeibt_global_get_cur_event_start_time();
	set_is_processing_registrant_removal(active_reg_ctx, 0);
	nvmeibt_register_terminate_reg_ctx(active_reg_ctx, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE, 1);   // The registrant was active, its stale-locks were converted. Need to convert to stale

	NFOUT;
}

/*
 * Note: we assume that that registrant (client) will not access the local
 * segment anymore. See launch_non_ioable_registrant_removal() for details.
 */
static enum REGISTRANT_DISCONNECT_LAUNCH_STATUS launch_active_registrant_removal(struct nvmeibt_registrant_ctx *active_reg_ctx)
{
	struct registrant_disconnect_wq_entry		*registrant_disconnect_task = NULL;
	enum REGISTRANT_DISCONNECT_LAUNCH_STATUS	rv = REGISTRANT_DISCONNECT_LAUNCH_FAILED;
	struct nvmeibt_seg_active					*seg_active = active_reg_ctx->seg_active;
	struct nvmeibt_local_disk					*its_local_disk = nvmeibt_seg_active_get_local_disk(seg_active);

	NFIN;

	N_Tf(dkiut65, "handle=@HANDLE seg=@UUID_8",
		 active_reg_ctx->client_messaging_handle, nvmeibt_seg_active_UUID_8(seg_active));

	NTOMA_ASSERT(dki98sa, seg_active != NULL, "seg_active===NULL");

	if (!nvmeibt_seg_active_get_locks_tbl_ptr(seg_active) || nvmeibt_local_disk_is_being_deleted(its_local_disk)) {
		N_Tf(jju8771, "seg=@UUID_8 locks_table_mmap=@PTR local_disk_is_being_deleted=@BOOL", nvmeibt_seg_active_UUID_8(seg_active),
			 nvmeibt_seg_active_get_locks_tbl_ptr(seg_active), nvmeibt_local_disk_is_being_deleted(its_local_disk));
		rv = REGISTRANT_DISCONNECT_LAUNCH_SKIPPED;
		nvmeibt_register_terminate_reg_ctx(active_reg_ctx, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE, 0);	// We will probably be is_deleting_seg_active, but not right away
		goto free_resources;
	}

	// all required params a ready, launch the task
	N_Tf(hhus662, "Handle client reg=@LOCKID seg=@UUID_8",
		nvmeib_lockid_purify(active_reg_ctx->reg_lock_id), nvmeibt_seg_active_UUID_8(seg_active));

	registrant_disconnect_task = NNVMEIBT_BM_CALLOC(karytx3, sizeof(*registrant_disconnect_task));

	registrant_disconnect_task->wq_entry.type = "REGISTRANT_DISCONNECT";
	registrant_disconnect_task->wq_entry.execute = registrant_disconnect_wrapper;
	registrant_disconnect_task->wq_entry.finalize = registrant_disconnect_finalize;
	registrant_disconnect_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	registrant_disconnect_task->wq_entry.free = registrant_disconnect_freer;
	registrant_disconnect_task->reg_ctx = active_reg_ctx;
	registrant_disconnect_task->seg_active = seg_active;
	registrant_disconnect_task->n_owner_locks_converted_to_stale = 0;
	registrant_disconnect_task->n_owner_locks_converted_to_zero =  0;

	TODO(This code should optimally go away from a WQ thread and run in the TOMA main thread. \
		 For that we need to refactor the flow so that the send lock requests are all sent and the IB \
		 callback gathers all responses and continues disconnect flow once this done.)
	if (nvmeibt_registrant_disconnect_add_work(its_local_disk, &(registrant_disconnect_task->wq_entry)) != 0) {
		N_Ef(dki9y76, "Unable to add registrant_disconnect offload task to WQ!");
		rv = REGISTRANT_DISCONNECT_LAUNCH_FAILED;
		goto free_resources;
	}

	rv = REGISTRANT_DISCONNECT_LAUNCH_OK;
	goto out;

free_resources:
	NNVMEIBT_BM_FREE(du8667h, registrant_disconnect_task);

out:
	NFOUT;
	return rv;
}

static enum UNREGISTER_RV launch_existing_active_registrant_removal(struct nvmeibt_registrant_ctx *active_reg_ctx)
{
	enum REGISTRANT_DISCONNECT_LAUNCH_STATUS	rvu;
	enum UNREGISTER_RV				rv = UNREGISTER_RV_IN_WORK;
	struct nvmeibt_seg_active		*seg_active = active_reg_ctx->seg_active;

	NFIN;
	N_Tf(rcsko3b, "handle=@HANDLE reg_lock_id=@C_LID", active_reg_ctx->client_messaging_handle, nvmeib_lockid_purify(active_reg_ctx->reg_lock_id));
	if (!XDLIST_NULL(&(active_reg_ctx->registrant_on_timeout_link))) {
		upd_registrant_sync_timeout(active_reg_ctx, REG_TIMEOUT_REASON_DEL_ME);	// Not waiting for RT_UNREG
	}
	if (!nvmeibt_praid_is_jbod(nvmeibt_seg_active_get_praid(seg_active))) {
		rvu = launch_active_registrant_removal(active_reg_ctx);

		if (rvu == REGISTRANT_DISCONNECT_LAUNCH_FAILED) {
			rv = UNREGISTER_RV_FAILED;
		} else if (rvu == REGISTRANT_DISCONNECT_LAUNCH_SKIPPED) {
			rv = UNREGISTER_RV_SKIPPED;
		} else {
			rv = UNREGISTER_RV_IN_WORK;
		}

		if (rv == UNREGISTER_RV_IN_WORK) {
			set_is_processing_registrant_removal(active_reg_ctx, 1);
		}
	}
	else {
		//non-praid
		TODO(Why not cut the networking);
		if (active_reg_ctx->is_client_waiting_for_ack) {
			if (nvmeibt_register_send_msg_to_registrant(active_reg_ctx, NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK,
														NVMEIBT_CLIENT_TR_REASON_NONE, 0, NULL) < 0) {
				N_Tf(trace_zzz_11, "Fail to send unregister-ACK to (reg_lock_id=@C_LID)", nvmeib_lockid_purify(active_reg_ctx->reg_lock_id));
			}
		}
		nvmeibt_register_terminate_reg_ctx(active_reg_ctx, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE, 0);	// JBOD should not have stale-locks
		rv = UNREGISTER_RV_OK;
	}
	if (rv == UNREGISTER_RV_IN_WORK) {
		getnstimeofday_boot(&(active_reg_ctx->reg_disconnect_time));
	}
	NFOUT;
	return rv;
}

/*
 * Every unregister gets here after we know that the registrant will not
 * access the local segment:
 *
 * - When a registrant sends RT_UNREGISTER (client/recovery)
 * - If the client network was disconnected (client)
 * - If the client was forced to disconnect (client)
 *
 * Now, we start a registrant_disconnect procedure, that includes converting locks &
 * to stale_locks
 * */
static enum UNREGISTER_RV launch_non_ioable_active_registrant_removal(
										struct nvmeibt_registrant_ctx *input_registrant_ctx)
{
	enum UNREGISTER_RV				rv = UNREGISTER_RV_OK;
	struct nvmeibt_seg_active		*seg_active = input_registrant_ctx->seg_active;
	struct nvmeibt_registrant_ctx	*reg_ctx;

	if (!seg_active) {
        N_Ef(uty7532, "No disk_segment");
        rv = UNREGISTER_RV_FAILED;
        goto out;
    }

TODO(Move all of the removal. longing, ... to the finalize, The problem is that registrants removal might have a side effect of seg_active and local_disk removel that compicates nvmeibt_register_launch_disconnected_client_removal_from_all_segments());

	reg_ctx = nvmeib_hash_search_uint64_t(seg_active->active_registrants_hash_by_handle, input_registrant_ctx->client_messaging_handle);
	if (reg_ctx) {
		reg_ctx->client_conversation_index = input_registrant_ctx->client_conversation_index;	// So that a reply would match
		if (nvmeibt_register_is_processing_registrant_removal(reg_ctx)) {
		   goto out;
		}
		rv = launch_existing_active_registrant_removal(reg_ctx);
	}
out:
	return rv;
}

int nvmeibt_register_launch_disconnected_client_removal_from_all_segments(int cid, struct nvmeibt_node *registrant_node)
{
	int								rv = 0;
	int								n_local_disk;
	int								n_seg_active;
	struct nvmeibt_registrant_ctx	*unregistering_reg_ctx;
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Ef(error_register_nvmeibt_register_launch_disconnected_client_removal_from_all_segments, "!nvmeibt_topology_is_HW_config_functional()");
		rv = -1;
		goto out;
	}
	N_Tf(heu44ns, "cid=@X node=@UUID_LE", cid, nvmeibt_node_UUID(registrant_node));
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		n_local_disk = nvmeib_hash_get_n_elements(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			// Go over all the segments active registrants and notify recovery about those that match by client handle + type.
			// We can't notify only with the tmp registrant as it has no lock id, and we need the lockid for for the recovery.
			n_seg_active = nvmeib_hash_get_n_elements(local_disk->seg_active_hash_by_uuid);
			NVMEIB_HASH_FOREACH(unregistering_reg_ctx, seg_active->active_registrants_hash_by_handle) {
				if (client_messaging_handle_to_cid(unregistering_reg_ctx->client_messaging_handle) != (uint32_t)cid) {
					continue;
				}
				lock_id_cache_registrant_unregistered(unregistering_reg_ctx);
				if (nvmeibt_register_launch_unsubscribed_active_registrant_removal(unregistering_reg_ctx) == UNREGISTER_RV_FAILED)
					rv = -1;
				if (n_local_disk != nvmeib_hash_get_n_elements(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str)) {
					N_Tf(cfcreyu, "local_disk removed while looping on registrants");
					break;
				}
				if (n_seg_active != nvmeib_hash_get_n_elements(local_disk->seg_active_hash_by_uuid)) {
					N_Tf(anijr2b, "seg_active removed");
					break;
				}
			}
			if (n_local_disk != nvmeib_hash_get_n_elements(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str)) {
				N_Tf(fst6623, "local_disk removed");
				break;
			}
		}
	}
	nvmeibt_register_remove_unsubscribed_longing_registrant_on_invalid_seg(((unsigned long long)cid) << 32, 1);
out:
	NFOUT;
	return rv;
}

static void brute_force_disconnect_client_request(struct nvmeibt_registrant_ctx *reg_ctx)
{																								// Brutally cut the QP for RDMA clients
	const uint32_t cid = client_messaging_handle_to_cid(reg_ctx->client_messaging_handle);		// identifier of client's connection with server per specific disk, Command server to force disconnect clinet (cid) from disk
	struct nvmeibs_toma_server_proc_buf buf;

	N_Tf(do3by0a, "handle=@HANDLE, cid=@CID", reg_ctx->client_messaging_handle, cid);
	ZEROINIT(buf);
	buf.type = NVMEIBS_TOMA_CLIENT_DISCONNECT_FORCE_CMD;
	buf.client_disconnect_force_cmd.cid = cid;
	if (nvmeib_srvr_api_lib_send_block_msg_to_server(&buf) < 0) {		// If client does not exist, server returns 0, see srvr code.
		N_Ef(do3by0b, "cid=@CID failed req (@AUTO_ERRNO)", cid);
	}
	N_Tf(do3by0c, "req sent");	// Not erasing the registrant. We only asked now asyncronously, Will unreg when the force succeeds
}

static void brute_force_disconnect_registrant_client(struct nvmeibt_registrant_ctx *reg_ctx,
											  BOOL is_on_timeout)
{
	BOOL was_timeout_active;

	NFIN;

	if (!is_force_cmd_called(reg_ctx)) {
		reg_ctx->is_force_cmd_called = 1;
		// Beware, might change the list
		was_timeout_active = upd_registrant_sync_timeout(reg_ctx, REG_TIMEOUT_REASON_DEL_ME);
		if (is_on_timeout && !was_timeout_active) {
			N_Ef(oqjwm4v, "is_on_timeout && !was_timeout_active");
			nvmeibt_abort(ES_FATAL);
		}
		brute_force_disconnect_client_request(reg_ctx);
	}

	NFOUT;
}

void nvmeibt_register_send_toma_not_ready(struct nvmeibt_registrant_ctx *reg_ctx,
										  enum NVMEIBT_CLIENT_TR_REASON reason,
										  struct nvmeibt_client_msg_summary* prior_msg_smr /*may be null*/)
{
	int data_length = prior_msg_smr ? sizeof(*prior_msg_smr) : 0;
	NFIN;
	N_Tf(tiu8nf5, "Sending toma_not_ready reason=@REASON_STR", nvmeibt_protocol_client_msg_reason_str((enum NVMEIBT_CLIENT_TR_REASON)reason));
	if (nvmeibt_register_send_msg_to_registrant(
							reg_ctx, NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY,
							reason, data_length, prior_msg_smr) == 0) {
		if (is_reason_recoverable_within_client_subscription(reason)) {
			add_longing_registrant_on_seg(reg_ctx);
		} else {
			N_Tf(iyu95rf, "Will not send registrable");
		}
	} else {
		N_Tf(gloo0d4, "Send failed, will not send registrable, handle is lost forever");
	}
	NFOUT;
}

void send_invalid_disk_segment(struct nvmeibt_registrant_ctx *reg_ctx, enum NVMEIBT_CLIENT_TR_REASON reason)
{
	NFIN;
	nvmeibt_register_send_msg_to_registrant(reg_ctx, NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID, reason, 0, NULL);
	if (reg_ctx->seg_active) {
		N_Tf(dloti96, "OOPS! invalid seg=@UUID_8", nvmeibt_seg_active_UUID_8(reg_ctx->seg_active));
		goto out;
	}
	add_longing_registrant_on_invalid_seg(reg_ctx);
out:
	NFOUT;
}

static BOOL is_ready_to_receive_registrant_msg(struct nvmeibt_register_msg *msg)
{
	int								 rv = 0;
	struct nvmeibt_registrant_ctx	*reg_ctx = &(msg->registrant_ctx);

	NFIN;
	if (	((msg->msg_type & 0xffff0000) != NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR) &&
			((msg->msg_type & 0xffff0000) != NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_RT) &&
			((msg->msg_type & 0xffff0000) != NVMEIBT_PROTOCOL_SIGNATURE_CLIENT)	) {
		N_Ef(error_register_is_ready_to_receive_registrant_msg, "OOPS msg->msg_type=@MSG_TYPE", msg->msg_type);
		goto out;
	}
	if (reg_ctx->client_protocol_version != NVMEIBT_CLIENT_PROTO_VERSION) {
		N_Wf(warn_register_is_ready_to_receive_registrant_msg,
			 "Software version mismatch, '@TOMA_CLIENT_PROTOCOL_VERSION' != "
			 "'@TOMA_CLIENT_PROTOCOL_VERSION'",
			 reg_ctx->client_protocol_version, NVMEIBT_CLIENT_PROTO_VERSION);
	}
	rv = 1;
	goto out;
out:
	NFOUT;
	return rv;
}

static void upd_seg_and_registrant_on_reregister_or_sw_topo_ack(
				struct nvmeibt_registrant_ctx *reg_ctx,
				struct nvmeibt_registrant_ctx *new_reg_ctx)
{
	struct nvmeibt_seg_active		*seg_active;
	int								applied_praid_ver;

	NFIN;
	if (nvmeibt_register_is_processing_registrant_removal(reg_ctx)) {
		N_Tf(trace_register_upd_seg_and_registrant_on_reregister_or_sw_topo_ack, "is_processing_registrant_removal. Ignoring");
		goto out;
	}
	seg_active = reg_ctx->seg_active;
	applied_praid_ver = nvmeibt_seg_active_get_praid_applied_topo(seg_active)->praid_version_major;
	if ((reg_ctx->praid_version < new_reg_ctx->praid_version) &&
		(applied_praid_ver >= new_reg_ctx->praid_version)) {
		N_Tf(fki9ir5, "Updating ctx praid_version:@PRAID_VERSION(@PRAID_VERSION)", new_reg_ctx->praid_version, reg_ctx->praid_version);
		reg_ctx->praid_version = new_reg_ctx->praid_version;
		if (new_reg_ctx->praid_version == applied_praid_ver) {
			nvmeibt_seg_active_inc_n_active_registrants_on_applied_praid(seg_active);
			// Stop the timer only if it is the right answer.
			// If we first sent SW_topo, and then UNREG, received SW_ACK after a long time,
			//  then we might force client not allowing it enough time to UNREG. Be it.
			if (reg_ctx->timeout_reason <= REG_TIMEOUT_REASON_SW_TOPO) {
				upd_registrant_sync_timeout(reg_ctx, REG_TIMEOUT_REASON_DEL_ME);
			}
		}
		NDUMP_N_ACTIVE_REGISTRANTS(slo0ir5, seg_active);
		if (nvmeibt_praid_is_client_sync_cmd_req_client_ack(nvmeibt_seg_active_get_praid_applied_topo(seg_active)->registrants_sync_cmd)) {
			nvmeibt_register_clients_sync_check_and_act_upon(seg_active);
		}
	}
out:
	NFOUT;
}

static struct nvmeibt_registrant_ctx *register_on_disk_segment(
				struct nvmeibt_registrant_ctx *incoming_reg_ctx,
				struct nvmeibt_registrant_ctx *existing_reg_ctx)
{
	struct nvmeibt_seg_active		*seg_active = incoming_reg_ctx->seg_active;

	NFIN;

	if (existing_reg_ctx) {
		// Already registered with the same lock_id and handle. If not the same then this abnormality was handled earlier.
		upd_seg_and_registrant_on_reregister_or_sw_topo_ack(existing_reg_ctx, incoming_reg_ctx);
		goto out;
	}
	// Add a new registrant
	if (nvmeibt_seg_active_n_active_registrants(seg_active) >= NVMEIBT_MAX_N_CLIENTS_PER_DISK_SEGMENT) {
		N_Ef(gkoi965, "seg=@UUID_8. n_registrants=@N_REGISTRANTS",
			nvmeibt_seg_active_UUID_8(seg_active),
			nvmeibt_seg_active_n_active_registrants(seg_active));
	}
	NTOMA_ASSERT(akmnn56, incoming_reg_ctx->reg_lock_id.all != 0, "new registrant with reg_lock_id=0");
	alloc_reg_ctx(&existing_reg_ctx, incoming_reg_ctx);
	N_Tf(fjiut86, "seg=@UUID_8 reg_ctx(@PTR/@LOCKID)",
		nvmeibt_seg_active_UUID_8(seg_active), existing_reg_ctx, nvmeib_lockid_purify(existing_reg_ctx->reg_lock_id));
	NVMEIBT_SEG_ACTIVE_ADD_ACTIVE_REGISTRANT_TO_HASHES(seg_active, existing_reg_ctx);

	if (incoming_reg_ctx->praid_version == nvmeibt_seg_active_get_praid_applied_topo(seg_active)->praid_version_major)
		nvmeibt_seg_active_inc_n_active_registrants_on_applied_praid(seg_active);
out:
	NDUMP_N_ACTIVE_REGISTRANTS(trace_register_register_on_disk_segment, seg_active);
	NFOUT;
	return existing_reg_ctx;
}

static BOOL is_valid_register_req(struct nvmeibt_registrant_ctx *incoming_reg_ctx, struct nvmeibt_registrant_ctx *existing_active_reg_ctx)
{
	int								rv = 0;
	struct nvmeibt_seg_active		*seg_active = incoming_reg_ctx->seg_active;
	int								seg_version = nvmeibt_seg_active_active_config_version(seg_active);
	int								refusal_reason;
	int								praid_ver_for_clients;

	NFIN;
	if (incoming_reg_ctx->volume_config_version < seg_version) {
		N_Tf(mkhjiy6, "Volume config versions registrant=@REGISTRANT_INT < @SEG_VERSION",
			incoming_reg_ctx->volume_config_version, seg_version);
		refusal_reason = NVMEIBT_CLIENT_TR_REASON_VOL_VERSION_BEHIND;
		goto volume_mismatch;
	}
	if (incoming_reg_ctx->volume_config_version > seg_version) {
		N_Tf(akiugj6, "Volume config versions registrant=@REGISTRANT_INT > @SEG_VERSION",
			incoming_reg_ctx->volume_config_version, seg_version);
		refusal_reason = NVMEIBT_CLIENT_TR_REASON_VOL_VERSION_AHEAD;
		goto toma_not_ready;
	}
	if (!nvmeibt_register_is_seg_active_accepting_registrations(seg_active, &refusal_reason)) {
		N_Tf(tu87tfe, "seg=@UUID_8 is not accepting registrations.", nvmeibt_seg_active_UUID_8(seg_active));
		goto toma_not_ready;
	}

	praid_ver_for_clients = seg_active->topo_for_clients.header.praid_version;
	if ((incoming_reg_ctx->praid_version < praid_ver_for_clients) || !(incoming_reg_ctx->reg_lock_id.all)) {
		refusal_reason = NVMEIBT_CLIENT_TR_REASON_PRAID_VERSION_BEHIND;
		goto nack;
	}
	if (incoming_reg_ctx->praid_version > praid_ver_for_clients) {
		N_Tf(akiunf5, "praid_version registrant=@REGISTRANT_INT > @PRAID_VERSION",
			incoming_reg_ctx->praid_version, praid_ver_for_clients);
		refusal_reason = (praid_ver_for_clients == -1) ? NVMEIBT_CLIENT_TR_REASON_SEG_STATE_NOT_REGISTRABLE : NVMEIBT_CLIENT_TR_REASON_PRAID_VERSION_AHEAD;
		goto toma_not_ready;
	}

	if (nvmeibt_register_is_processing_registrant_removal(existing_active_reg_ctx)) {
		refusal_reason = NVMEIBT_CLIENT_TR_REASON_UNREGISTER_IN_PROGRESS;
		goto toma_not_ready;
	}
	if (!existing_active_reg_ctx && lock_id_cache_is_lockid_taken(seg_active, incoming_reg_ctx->reg_lock_id)) {
		// No active registrant, but its journal entries were not fully cleaned yet
		refusal_reason = NVMEIBT_CLIENT_TR_REASON_LOCKID_ALREADY_TAKEN;
		goto nack;
	}
	if (existing_active_reg_ctx && !nvmeibt_register_is_same_registrant(existing_active_reg_ctx, incoming_reg_ctx)) {
		if (incoming_reg_ctx->rt_never_reged_on_seg) {
			existing_active_reg_ctx->rt_never_reged_on_seg = 1;	// Not used, transfer the safeness to the existing_reg_ctx
			nvmeibt_register_terminate_reg_ctx(existing_active_reg_ctx, 0, NVMEIBT_REGISTER_REGISTRANT_TYPE_ACTIVE, 0);	// Client wise, never properly registered, no I/O --> no stale_locks
		} else {
			N_Tf(t_fg_tomareg, "We have a mess, there is an existing registrant, but with a different handle or lock id: "
				"@NODE,@HANDLE,reg_@LOCKID  &  @NODE,@HANDLE,reg_@LOCKID",
				existing_active_reg_ctx->registrant_node_id.str, existing_active_reg_ctx->client_messaging_handle,
				nvmeib_lockid_purify(existing_active_reg_ctx->reg_lock_id),
				incoming_reg_ctx->registrant_node_id.str, incoming_reg_ctx->client_messaging_handle,
				nvmeib_lockid_purify(incoming_reg_ctx->reg_lock_id));
			// We have a mess, possibly on the client side too
			refusal_reason = NVMEIBT_CLIENT_TR_REASON_LOCKID_MESS;
			goto toma_not_ready;
		}
	}
	// Not validating the client for cold_recovery/IO. The client obeys the topology.
	rv = 1;
	goto out;
toma_not_ready:
	nvmeibt_register_send_toma_not_ready(incoming_reg_ctx, refusal_reason, NULL);
	if (refusal_reason == NVMEIBT_CLIENT_TR_REASON_LOCKID_MESS) {
		if (!nvmeibt_register_is_processing_registrant_removal(existing_active_reg_ctx) && !is_force_cmd_called(existing_active_reg_ctx)) {
			N_Wf(jiut853, "Surprise REGISTER from a registered registrant lockid=@T_LID(@LOCKID) handle=@HANDLE(@HANDLE)",
				nvmeib_lockid_purify(incoming_reg_ctx->reg_lock_id), nvmeib_lockid_purify(existing_active_reg_ctx->reg_lock_id),
				incoming_reg_ctx->client_messaging_handle, existing_active_reg_ctx->client_messaging_handle);
			brute_force_disconnect_registrant_client(incoming_reg_ctx, 0);
			brute_force_disconnect_registrant_client(existing_active_reg_ctx, 0);
		}
	}
	goto out;
nack:
	/* if lock_id allocation fails, the field reg_lock_id will be zeroed out */
	incoming_reg_ctx->reg_lock_id = get_fresh_reg_lock_id(seg_active);
	if (nvmeibt_tTopoOfPraid_len(&(seg_active->topo_for_clients)) == 0) {
		N_Ef(error_register_is_valid_register_req, "seg_active->topo_for_clients length=0");
	}
	nvmeibt_register_send_msg_to_registrant(
					incoming_reg_ctx, NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK,
					refusal_reason, nvmeibt_tTopoOfPraid_len(&(seg_active->topo_for_clients)), &(seg_active->topo_for_clients));
	goto out;
volume_mismatch:
	nvmeibt_register_send_msg_to_registrant(
					incoming_reg_ctx, NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH,
					refusal_reason, 0, NULL);
	goto out;
out:
	NFOUT;
	return rv;
}

void nvmeibt_register_handle_new_reservation_mode(struct nvmeibt_seg_active *seg_active, u64 reservation_mode_version)
{
	if (seg_active && is_reservation_mode_version_ahead(reservation_mode_version, seg_active->highest_reservation_mode_version)) {
		// Here we just handle the new highest. Later, we will handle the refusal as needed
		N_Tf(6gwyson, "New highest - incoming_@RES_MOD_VER > highest_@RES_MOD_VER",
			reservation_mode_version, seg_active->highest_reservation_mode_version);
		NVMEIBT_SEG_ACTIVE_SET_HIGHEST_RESERVATION_MODE_VERSION(of9fmk6, seg_active, reservation_mode_version);
		if (seg_active->persistent_metadata)
			seg_active->persistent_metadata->reservation_mode_version = seg_active->highest_reservation_mode_version; // Not part of the any logic anyhow.
		nvmeibt_register_launch_seg_metadata_ctrl_save(seg_active);	// Store the seg_active->highest_reservation_mode_version if needed
		nvmeibt_register_make_all_seg_active_registrants_sync_praid_topology(seg_active);
	}
}

static int handle_register_registrant_on_disk_segment(struct nvmeibt_registrant_ctx *incoming_reg_ctx)
{
	int									rv = 0;
	BOOL								is_send_OK;
	struct nvmeibt_registrant_ctx		*new_ctx;
	struct nvmeibt_registrant_ctx		*existing_active_reg_ctx;
	struct nvmeibt_seg_active           *seg_active = incoming_reg_ctx->seg_active;

	NFIN;
	existing_active_reg_ctx = get_active_registrant_by_client_messaging_handle(seg_active, incoming_reg_ctx);
	// Longing is for clients that hold their registration attempt. Remove it, and possibly add again during processing
	remove_longing_registrant_on_seg_by_ctx(incoming_reg_ctx);

	nvmeibt_register_handle_new_reservation_mode(seg_active, incoming_reg_ctx->reservation_mode_version);

	if (!is_valid_register_req(incoming_reg_ctx, existing_active_reg_ctx)) {
		goto out;
	}

	new_ctx = register_on_disk_segment(incoming_reg_ctx, existing_active_reg_ctx);
	if (!new_ctx) {
		N_Tf(trace_register_handle_register_registrant_on_disk_segment, "Failed to register");
		goto out;
	}
	// Prapare the registration specific fields
	is_send_OK = nvmeibt_register_send_msg_to_registrant(new_ctx,
														 NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK, NVMEIBT_CLIENT_TR_REASON_NONE,
														 0, NULL);
	if (is_send_OK < 0) {
		if (!existing_active_reg_ctx) {
			// The client is not aware of this registration. Remove it.
			// It solves the problem of TR_REGISTER that is handled after the registrant_disconnect. Failing to send ACK
			remove_disconnected_client_new_active_registrant_by_lockid(seg_active, new_ctx->reg_lock_id);
		}
		goto out;
	}
	if (new_ctx->is_recoverer) {    // Attach for recovery per TOMA req (of interest only for the local_clnt)
		// notify recovery tasks of a new registrant
		nvmeibt_recovery_handle_client_registered(new_ctx);
	}

out:
	NFOUT;
	return rv;
}

static int handle_unregister_registrant_from_disk_segment(struct nvmeibt_registrant_ctx *input_registrant_ctx)
{
	int									rv = 0;

	NFIN;

	input_registrant_ctx->is_client_waiting_for_ack = 1;
	if (launch_non_ioable_active_registrant_removal(input_registrant_ctx) == UNREGISTER_RV_FAILED)
		rv = -1;

	/* notify lock_id: don't wait for (lost registrant's) ack for purges */
	lock_id_cache_registrant_unregistered(input_registrant_ctx);

	NFOUT;
	return rv;
}

enum UNREGISTER_RV nvmeibt_register_launch_unsubscribed_active_registrant_removal(struct nvmeibt_registrant_ctx *input_registrant_ctx)
{
	return launch_non_ioable_active_registrant_removal(input_registrant_ctx);
}

/******************          Register      ***********************/

void nvmeibt_register_make_all_seg_active_registrants_sync_praid_topology(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment			*disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	struct nvmeibt_praid				*praid = nvmeibt_disk_segment_get_praid(disk_segment);
	struct nvmeibt_praid_topo_ctx		*praid_topo;
	bool								is_accepting_reg;
	int									send_rv;
	struct nvmeibt_registrant_ctx		*reg_ctx;

	NFIN;
	if (!seg_active) {
		N_Tf(djur854, "seg_active=NULL. Skipping");
		goto out;
	}
	if (!praid) {
		N_Tf(alo0983, "seg=@UUID_8 No praid, ignoring", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	praid_topo = &praid->praid_follower.applied_praid_lot.topo_ctx;
	is_accepting_reg = nvmeibt_register_is_seg_active_accepting_registrations(seg_active, NULL);

	if (is_accepting_reg) {
		if (!(praid_topo->is_activated))
		{
			N_Tf(fkoit96, "seg=@UUID_8 praid not activated, ignoring", nvmeibt_seg_active_UUID_8(seg_active));
			// Note that a client that still has an old topo will continue using it,
			//  but probably, it is not usable due to the same reasons that drove
			//  the !is_activated
			goto out;
		}
	}

	if (!nvmeibt_disk_is_local(disk_segment->seg_mgmt.its_disk)) {
		N_Tf(loitkvd, "Non local disk_segment");
		goto out;
	}
	nvmeibt_register_clients_sync_check_and_act_upon(seg_active);
	if (!nvmeibt_disk_segment_get_seg_active(disk_segment)) {
		seg_active = NULL;
		N_Tf(t_xx_302, "The seg_active was released");
		goto out;
	}
	if (nvmeibt_seg_active_are_registrants_aligned_with_sync_cmd(seg_active) && is_seg_active_reservation_mode_version_registrable(seg_active)) {
		goto out;
	}
	// Registrants are not aligned, i.e., either
	// - We are in the middle of a switch-topo, where some are still behind
	// or
	// - All registered registrants have to unregister
	if (is_accepting_reg && !nvmeibt_praid_is_client_sync_cmd_req_client_ack(praid_topo->registrants_sync_cmd)) {
		goto out;
	}
	// Send all the registrants a request to voluntarily switch_praid_topology/unregister
	NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_lockid) {
		if (nvmeibt_register_is_processing_registrant_removal(reg_ctx) || is_registrant_on_timeout(reg_ctx) || is_force_cmd_called(reg_ctx)) {
			continue;
		}
		if (is_accepting_reg) {
			if (reg_ctx->praid_version == praid_topo->praid_version_major) {
				continue;	// This registrant is already using the current praid_version
			}
			if (reg_ctx->praid_version == seg_active->topo_for_clients.header.praid_version) {
				N_Tf(jitu865, "New topo is not yet build, old=@PRAID_VERSION new=@PRAID_VERSION",
					 reg_ctx->praid_version, praid_topo->praid_version_major);
				continue;
			}
			if (reg_ctx->praid_version > seg_active->topo_for_clients.header.praid_version)
			{
				N_Wf(skiutj6, "Switch topo BUG, versions: old=@PRAID_VERSION new=@PRAID_VERSION",
					 reg_ctx->praid_version, praid_topo->praid_version_major);
			}
		}
		send_rv = nvmeibt_register_send_msg_to_registrant(reg_ctx,
														  (is_accepting_reg ? NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY :
														   NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT),
														  (is_seg_active_reservation_mode_version_registrable(seg_active) ?
														   nvmeibt_praid_applied_sync_cmd_reason(praid) : NVMEIBT_CLIENT_TR_REASON_UPDATING_RM_VERSION),
														   nvmeibt_tTopoOfPraid_len(&(seg_active->topo_for_clients)),
														  &(seg_active->topo_for_clients)	);
		if (send_rv == 0) {
			upd_registrant_sync_timeout(reg_ctx, (is_accepting_reg ? REG_TIMEOUT_REASON_SW_TOPO : REG_TIMEOUT_REASON_UNREG));
			if (!is_accepting_reg) {
				add_longing_registrant_on_seg(reg_ctx); // So that we will send REGISTRABLE in the future.
			}
		}
	}
	// 4) Kill non-responsive registrants
out:
	NFOUT;
}

void nvmeibt_register_close_seg_active_for_registration(struct nvmeibt_seg_active *seg_active, bool is_brute_force_disconnect_required)
{
	struct nvmeibt_registrant_ctx	*reg_ctx;
	struct nvmeibt_disk_segment		*disk_segment;

	NFIN;
	if (!seg_active)
		goto out;
	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	nvmeibt_seg_active_mark_is_closing_to_reg(seg_active);
	seg_active->active_seg_topo.is_registrants_synchronizer = 1;
	nvmeibt_register_make_all_seg_active_registrants_sync_praid_topology(seg_active);

	if (!nvmeibt_disk_segment_get_seg_active(disk_segment)) // might have been freed inside previous function
		goto out;

	if (is_brute_force_disconnect_required) {
		// Now go and brute force disconnect them
		NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_lockid) {
			if (nvmeibt_register_is_processing_registrant_removal(reg_ctx) || is_registrant_on_timeout(reg_ctx)) {
				continue;
			}
			brute_force_disconnect_registrant_client(reg_ctx, 0);
		}
	}
out:
	NFOUT;
}

void nvmeibt_register_close_all_seg_actives_for_registration(void)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_disk			*disk;
	struct nvmeibt_seg_active	*seg_active;

	NFIN;
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		disk = local_disk->its_disk;
		if (!disk)
			continue;
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			nvmeibt_seg_active_stop_all_recoveries_and_registrations(seg_active, 0, 1);
			// The previous func can remove the local disk
			if (nvmeibt_disk_get_local_disk(disk) == NULL) {
				N_Tf(nh112bb, "local_disk=@STR was removed", nvmeibt_disk_get_ldisk_id_str(disk));
				break;
			}
		}
	}
	NFOUT;
}

void nvmeibt_register_open_disk_eligible_seg_actives_for_use(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_seg_active	*seg_active;

	if (!nvmeibt_local_disk_is_ready_for_segments(local_disk) || !nvmeibt_local_disk_is_connected_to_disk(local_disk)) {
		return;
	}
	NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
		if (seg_active->prev_successful_open_for_use_praid_major == nvmeibt_seg_active_get_active_praid_version_major(seg_active)) {
			continue;
		}
		if (nvmeibt_disk_segment_is_deprecated_in_config(nvmeibt_seg_active_get_disk_segment(seg_active)) || !nvmeibt_register_is_seg_active_accepting_registrations(seg_active, NULL)) {
			continue;
		}
		send_registrable_to_all_longing_registrants(seg_active);
		if (nvmeibt_seg_active_n_longing_registrants(seg_active) == 0) {
			seg_active->prev_successful_open_for_use_praid_major = nvmeibt_seg_active_get_active_praid_version_major(seg_active);
		}
	}
}

void nvmeibt_register_open_all_eligible_seg_actives_for_use(void)
{
	struct nvmeibt_local_disk	*local_disk;

	NFIN;
	if (nvmeibt_raft_is_shutdown_triggered()) {
		goto out;
	}
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		nvmeibt_register_open_disk_eligible_seg_actives_for_use(local_disk);
	}
out:
	NFOUT;
}

static int registrant_sw_topo_ack_received(struct nvmeibt_registrant_ctx *input_reg_ctx)
{
	int								rv = -1;
	struct nvmeibt_seg_active		*seg_active = input_reg_ctx->seg_active;
	struct nvmeibt_registrant_ctx	*reg_ctx;

	NFIN;
	if (seg_active == NULL) {
		N_Ef(yuii984, "No seg=@UUID_8", nvmeib_uuid_first_4_bytes(&input_reg_ctx->seg_uuid));
		goto out;
	}
	reg_ctx = get_active_registrant_by_client_messaging_handle(seg_active, input_reg_ctx);
	if (reg_ctx) {
		upd_seg_and_registrant_on_reregister_or_sw_topo_ack(reg_ctx, input_reg_ctx);
	}
	else {
		N_Tf(dki94r8, "Cannot find entry seg=@UUID_8 registrant=@HOSTNAME",
			nvmeib_uuid_first_4_bytes(&input_reg_ctx->seg_uuid), input_reg_ctx->client->net.host_name);
	}

	rv = 0;
out:
	NFOUT;
	return rv;
}

struct nvmeibt_registrant_ctx *nvmeibt_register_get_out_reg_ctx_by_in_msg(struct nvmeibt_register_msg *msg)
{
	struct nvmeibt_registrant_ctx			*input_reg_ctx;

	input_reg_ctx = &msg->registrant_ctx;
	return get_active_registrant_by_client_messaging_handle(input_reg_ctx->seg_active, input_reg_ctx);
}

int nvmeibt_register_timeout_occurred(void)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_seg_active	*seg_active;
	int							rv = 0;
	struct timespec				now;

	NFIN;
	getnstimeofday_boot(&now);
	// Go over the local disk_segments, and locate the expired registrants' timeout
	TODO(Although this function is called rarely, maybe better add a heap for it);
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			struct nvmeibt_registrant_ctx	*reg_ctx;
			XDLIST_FOREACH_SAFE(reg_ctx, &(seg_active->registrants_on_timeout)) {
				if (!(reg_ctx->is_force_cmd_called) && timespec_lt(reg_ctx->timeout_time, now)) {
					// Send yet another TR_UNREG.
					nvmeibt_register_send_msg_to_registrant(reg_ctx, NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK,
															NVMEIBT_CLIENT_TR_REASON_AWAITING_CLIENTS_SYNC, 0, NULL);
					// Ask to brutally disconnect registrants that failed to unregister voluntarily
					brute_force_disconnect_registrant_client(reg_ctx, 1);
				}
			}
		}
	}
	NFOUT;
	return rv;
}

struct timespec nvmeibt_register_get_next_timeout_timespec(void)
{
	return next_wait_for_registrant_timeout;
}

int nvmeibt_register_handle_incoming_message(struct nvmeibt_register_msg *msg)
{
	int								rv = 0;
	enum NVMEIBT_CLIENT_TR_REASON	reason;

	NFIN;
	XDLIST_INIT_LINK(&(msg->registrant_ctx.registrant_on_timeout_link), NULL);	// Make certain that it is not used
	NREGISTER_MSG_DUMP(trace_register_nvmeibt_register_handle_incoming_message, msg->msg_type, msg->reason, &(msg->registrant_ctx), msg->data_length, msg->cookie);

	if (!msg->registrant_ctx.seg_active) {
		reason = NVMEIBT_CLIENT_TR_REASON_INVALID_SEG_ID;
		send_invalid_disk_segment(&(msg->registrant_ctx), reason);
		goto out;
	}
	if (!is_ready_to_receive_registrant_msg(msg)) {
		goto out;
	}
	switch (msg->msg_type) {
	case NVMEIBT_CLIENT_MSG_CT_STALE_LOCK:
	case NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK:
		rv = handle_stale_lock_report(msg);		// Differentiate msg types by the value of stale bit
		break;
	case NVMEIBT_CLIENT_MSG_CT_FAILED_CMD:
		rv = handle_failed_cmd_report(msg);
		break;
	case NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK:
		rv = handle_lock_id_cache_purge_ack_received(msg);
		break;
	case NVMEIBT_CLIENT_MSG_CT_DI_DETECTED:
		rv = handle_data_integrity_issue_report(msg);
		break;
	case NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT:
		rv = handle_register_registrant_on_disk_segment(&(msg->registrant_ctx));
		break;
	case NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT:
		rv = handle_unregister_registrant_from_disk_segment(&(msg->registrant_ctx));
		break;
	case NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK:
		rv = registrant_sw_topo_ack_received(&(msg->registrant_ctx));
		break;
	default:
		N_Ef(error_register_nvmeibt_register_handle_incoming_message, "msg_type=@MSG_TYPE", msg->msg_type);
		rv = -1;
		break;
	}
out:
	NFOUT;
	return rv;
}

static void dump_reg_ctx_to_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx,
								   struct nvmeibt_registrant_ctx *reg_ctx, BOOL is_with_seg)
{
	struct timespec	time_to_cutoff;
	char	seg_str[50] = "";

	if (is_with_seg) {
		sprintf(seg_str, " seg=%08x", nvmeib_uuid_first_4_bytes(&(reg_ctx->seg_uuid)));
	}
	time_to_cutoff = timespec_sub(reg_ctx->timeout_time, nvmeibt_global_get_cur_event_start_time());
	(*printf_fn)(printf_ctx, "\t\t\t\t- %s lock_id=%llx client_uuid=%s handle=%llx node=%s praid_ver=%x is_force_cmd_called=%x time_to_cutoff=%lld.%09lld is_on_pending_list=%d\n",
			seg_str,
			reg_ctx->reg_lock_id, nvmeibt_client_get_urn_uuid_str(reg_ctx->client),
			reg_ctx->client_messaging_handle, nvmeibt_client_get_hostname(reg_ctx->client),
			reg_ctx->praid_version,
			reg_ctx->is_force_cmd_called, time_to_cutoff.tv_sec, time_to_cutoff.tv_nsec,
			!XDLIST_NULL(&(reg_ctx->registrant_on_timeout_link)));
}

int nvmeibt_register_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_registrant_ctx	*reg_ctx;

	//FIN;
	if (seg_active) {
		(*printf_fn)(printf_ctx, "\t\t\t- Active registrants\n");
		NVMEIB_HASH_FOREACH(reg_ctx, seg_active->active_registrants_hash_by_lockid) {
			dump_reg_ctx_to_status(printf_fn, printf_ctx, reg_ctx, 0);
		}
		(*printf_fn)(printf_ctx, "\t\t\t- Longing registrants\n");
		NVMEIB_HASH_FOREACH(reg_ctx, seg_active->longing_registrants_hash_by_handle) {
			dump_reg_ctx_to_status(printf_fn, printf_ctx, reg_ctx, 0);
		}
		(*printf_fn)(printf_ctx, "\t\t\t- Stale registrants\n");
		NVMEIB_HASH_FOREACH(reg_ctx, seg_active->stale_registrants_hash_by_purified_lockid) {
			dump_reg_ctx_to_status(printf_fn, printf_ctx, reg_ctx, 0);
		}
		(*printf_fn)(printf_ctx, "\t\t\t- Stale Locks Hash:\n");
		stale_locks_hash_to_string(printf_fn, printf_ctx, seg_active);
	}
	else {
		(*printf_fn)(printf_ctx, "\t- Longing registrants on invalid seg\n");
		XDLIST_FOREACH(reg_ctx, &(nvmeibt_global_get_global()->longing_on_invalid_seg_list_by_handle)) {
			dump_reg_ctx_to_status(printf_fn, printf_ctx, reg_ctx, 1);
		}
	}
	//FOUT;
	return 0;
}

void nvmeibt_register_set_brute_force_test(bool is_tested)
{
	if (brute_force_test != is_tested) {
		if (is_tested)
			N_Tf(t_xx_75, "Start brute force test");
		else
			N_Tf(t_xx_76, "Stop brute force test");
		brute_force_test = is_tested;
	}
}

