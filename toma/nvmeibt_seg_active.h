#ifndef NVMEIBT_SEG_ACTIVE
#define NVMEIBT_SEG_ACTIVE

#include "../common/nvmeib_heap.h"
#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_wq.h"
#include "../common/nvmeib_shared.h"
#include "clnt/nvmeibt_client_protocol.h"
#include "nvmeibt_register.h"

struct nvmeibt_topology;

/*
 * When a client unregisters (possibly as a side effect of a disconnect), it leaves
 *  unresolved locks. Locks are found in the owner-locks table, some are copy-locks
 * All those locks are converted to stale-locks in the owner-locks table, keeping
 *  the lock-id of the unregistered-client, and while doing so, they are added to
 *  the registrant's reg_ctx->stale_locks_hash.
 * Whenever a (client) recoverer recovers a stale-lock it sends RT_BLKSET_RECOVERED
 *  to all the owners (/active), and those TOMAs remove the relevant blkset_no from
 *  the reg_ctx->stale_locks_hash of the registrant that left the stale lock behind.
 * Once the reg_ctx->stale_locks_hash is empty, we know that the lockid of that
 *  client finished its role, so the registrant can be terminated, and as a result
 *  its lockid can be reused
 */
struct stale_lock_ctx {
	unsigned long long							seg_blkset_no;
	struct nvmeibt_registrant_ctx				*reg_ctx;
	struct xdlist 								seg_active_link;
};

/*
 * For locks that someone asked to be removed, and now that someone waits for them
 * to really be removed.
 */
struct nvmeibt_seg_active_awaited_lockid {
	XDLIST_DECLARE(, struct nvmeibt_registrant_awaiting_lockid, awaiting_lockid_link) awaiting_registrants;
	struct xdlist 								awaited_lockids_link;
	union nvmeib_lock_id						lockid_key;
};

/*
 * Holds context to track ongoing recovery (rebuild)
 */
struct nvmeibt_seg_active_recovery_ctx {	// Exists only for ongoing recoveries
	u64											tid;
	int											praid_version;
	union nvmeib_lock_id						reg_lock_id;
	uint64_t									n_blksets_remaining;
	uint64_t									prev_report_n_blksets_remaining;
};

/*
 * Indicate the required (pending) recovery action
 */
struct seg_active_required_recovery_action {
	int				praid_version_major;
	unsigned int 	stale_rebuild		: 1;
	unsigned int 	txid_rebuild		: 1;
	unsigned int 	cold_recovery		: 1;
	unsigned int 	JGC_rebuild			: 1;
	unsigned int	scrubbing			: 1;
};


enum SERJIO_CLEAN_RANGE_STATE {
	SERJIO_CLEAN_RANGE_STATE_UNINITIALIZED	= 0,
	SERJIO_CLEAN_RANGE_STATE_REQUIRED		= (0x1 << 0),
	SERJIO_CLEAN_RANGE_STATE_NOT_NEEDED		= (0x1 << 1),
	SERJIO_CLEAN_RANGE_STATE_IN_WORK		= (0x1 << 2),
	SERJIO_CLEAN_RANGE_STATE_DONE_AT_INIT	= (0x1 << 3),
	SERJIO_CLEAN_RANGE_STATE_DONE_DELETED	= (0x1 << 4),
};

#define REGISTRANTS_HASH_SIZE (NVMEIBT_MAX_N_CLIENTS_PER_DISK_SEGMENT >> 2)

/*
 * Holds the live state of an active local segments:
 * registrants, ongoing recoveries, FILL MORE HERE
 */
struct nvmeibt_seg_active {
	union nvmeib_uuid							uuid;
	struct nvmeibt_local_disk					*local_disk;	// Might be null if local only in config
	struct nvmeibt_disk_segment					*disk_segment;
	//
	struct nvmeib_hash_table					*longing_registrants_hash_by_handle;
	struct nvmeib_hash_table					*active_registrants_hash_by_lockid;
	struct nvmeib_hash_table					*active_registrants_hash_by_handle;	// Why by cid and not by client_messaging_handle
																					// There should be at most one with the cid. Still need to verify identical client_messaging_handle
	struct nvmeib_hash_table					*stale_registrants_hash_by_purified_lockid;
	XHASHTABLE_DECLARE(stale_locks_hash,           struct stale_lock_ctx,                    seg_active_link,      NVMEIB_XHASHTABLE_N_BITS(REGISTRANTS_HASH_SIZE) + 1);	// EC after unreg, record all registrants' stale-locks
	XHASHTABLE_DECLARE(awaited_lockids_hash_by_lockid,            struct nvmeibt_seg_active_awaited_lockid, awaited_lockids_link, NVMEIB_XHASHTABLE_N_BITS(REGISTRANTS_HASH_SIZE));
	XDLIST_DECLARE(, struct nvmeibt_registrant_ctx, registrant_on_timeout_link) registrants_on_timeout;	// Always add last
	XDLIST_DECLARE(, struct nvmeibt_wq_entry, link) owner_lock_ids_to_release;
	//
	struct xdlist								global_seg_active_post_update_action_link;
//	struct xdlist								disk_seg_active_link;
	//
	struct nvmeibt_disk_segment_topo_ctx		active_seg_topo;
	struct nvmeibt_serialized_seg_active_topo	prev_serialized_topo;
	//
	struct timespec								last_registrant_disconnect_timespec;
	int64_t										JGC_rebuild_required_timeout_sec;
	int											n_active_registrants_on_active_praid_version;
	int											n_registrants_on_timeout;
	//
	struct nvmeibt_seg_active_recovery_ctx		cold_recovery_ctx;
	struct nvmeibt_seg_active_recovery_ctx		dirty_rebuild_ctx;
	struct nvmeibt_seg_active_recovery_ctx		stale_rebuild_ctx;
	struct nvmeibt_seg_active_recovery_ctx		txid_rebuild_ctx;
	struct nvmeibt_seg_active_recovery_ctx		JGC_rebuild_ctx;
	struct nvmeibt_seg_active_recovery_ctx		scrubbing_ctx;
	//
	struct seg_active_required_recovery_action	required_recovery_action;
	//
	int											prev_successful_open_for_use_praid_major;
	int											last_post_update_praid_version_major;
	//
	enum NVMEIBT_ZEROING_STATE					applied_zeroing_state;
	struct timespec								last_zeroing_progress_report_time;
	//
	BOOL										is_expected_to_have_stale_locks; TODO(Follow-up exactly for EC, since we follow-up on all stales);
	int											ref_count;
	BOOL										was_launch_metadata_store_called_during_metadata_store;
	BOOL										is_locktable_on_disk_corrupted;
	BOOL										is_closing_to_reg;	// disk hot-unplug / shutdown / ..., unregistering clients, ... Takes a while
	//
	u64											n_4Kblk_zeroed;
	unsigned long long int						pba_s;
	unsigned long long int						pba_e;
	struct nvmeibt_disk_gpt_partition_entry		*metadata_gpt_entry;
	union nvmeib_lock_blkset_entry				*mmap_locks_tbl;
	//
	unsigned int								reg_lock_id_cache_last_allocated_lockid;
	unsigned long								reg_lock_id_cache_purge_seqno;
	int											reg_lock_id_cache_purge_zone;
	int											reg_lock_id_cache_purge_n_purges_in_fly;
	enum SERJIO_CLEAN_RANGE_STATE				applied_serjio_clean_range_state;

	u64											submitted_reservation_mode_version;
	u64											committed_reservation_mode_version;
	u64											highest_reservation_mode_version;
	u64											active_reservation_mode_version;

	struct timespec								scrubbing_start_time;
	struct timespec								last_failed_scrub_iteration_time;
	nvmeib_heap_element_t						my_next_scrub_timeout_heap_element;
	//
	uint64_t									dirty_next_unfixed_lock_blkset_no;	// Due to TXID=UNKNOWN. Upd on client's progress. Reset on DIRTY_BITS_INIT
	uint64_t									scrubbing_iter_n_blksets;
	//
	struct nvmeibt_seg_active_metadata_ctrl		*persistent_metadata;			// Actually stores larger buffer for upgrades
	pthread_mutex_t								stale_locks_hash_mutex;
	//
	struct tTopoOfPraid							topo_for_clients;
	bool										is_seg_registrable;
	bool										is_last_shutdown_clean;
};

/* controlled by toma_rpc tool */
extern int64_t nvmeibt_recovery_n_blksets_per_scrub_iteration;

#define NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS(name, I_seg_active, I_dirty_bits_state)	do {			\
	struct nvmeibt_seg_active				*__seg_active = (I_seg_active);							\
	struct nvmeibt_disk_segment_topo_ctx	*__seg_topo = &(__seg_active->active_seg_topo);			\
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	__prev_val;												\
	if (!__seg_active) {																			\
		N_Wf(name ## _5, "seg_active=NULL");														\
		break;																						\
	}																								\
	__prev_val = __seg_topo->dirty_bits_state;														\
	NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS(name,															\
										 nvmeibt_seg_active_UUID(__seg_active),						\
										 nvmeibt_seg_active_get_applied_seg_lot(__seg_active),		\
										 __seg_topo,												\
										 I_dirty_bits_state,										\
										 'A');														\
	if (__seg_topo->dirty_bits_state != __prev_val)													\
		nvmeibt_seg_active_mark_serialize_active_topo_for_leader_required(__seg_active);			\
} while (0)

/******************************************************************************/

#define NNVMEIBT_SEG_ACTIVE_SET_TXID_INIT_MODE(name, I_seg_active, I_txid_init_mode)	do {		\
	struct nvmeibt_seg_active				*__seg_active = (I_seg_active);							\
	struct nvmeibt_disk_segment_topo_ctx	*__seg_topo = &(__seg_active->active_seg_topo);			\
	enum NVMEIBT_MEM_TBL_INIT_MODE			__prev_val;												\
	if (!__seg_active) {																			\
		N_Wf(name ## _5, "seg_active=NULL");														\
		break;																						\
	}																								\
	__prev_val = __seg_topo->txid_init_mode;														\
	NNVMEIBT_SEG_TOPO_SET_TXID_INIT_MODE(name, nvmeibt_seg_active_UUID(__seg_active),				\
			nvmeibt_seg_active_get_applied_seg_lot(__seg_active),									\
			__seg_topo, I_txid_init_mode, 'A');														\
	if (__seg_topo->txid_init_mode != __prev_val)													\
		nvmeibt_seg_active_mark_serialize_active_topo_for_leader_required(__seg_active);			\
} while (0)

#define NNVMEIBT_SEG_ACTIVE_SET_DIRTY_BITS_INIT_MODE(name, I_seg_active, I_dirty_init_mode)	do {	\
	struct nvmeibt_seg_active				*__seg_active = (I_seg_active);							\
	struct nvmeibt_disk_segment_topo_ctx	*__seg_topo = &(__seg_active->active_seg_topo);			\
	enum NVMEIBT_MEM_TBL_INIT_MODE			__prev_val;												\
	if (!__seg_active) {																			\
		N_Wf(name ## _5, "seg_active=NULL");														\
		break;																						\
	}																								\
	__prev_val = __seg_topo->dirty_bits_init_mode;													\
	NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS_INIT_MODE(name, nvmeibt_seg_active_UUID(__seg_active),			\
		nvmeibt_seg_active_get_applied_seg_lot(__seg_active),										\
		__seg_topo, I_dirty_init_mode, 'A');														\
	if (__seg_topo->dirty_bits_init_mode != __prev_val)												\
		nvmeibt_seg_active_mark_serialize_active_topo_for_leader_required(__seg_active);			\
} while (0)

#define NNVMEIBT_SEG_ACTIVE_SET_STALE_LOCKS_INIT_MODE(name, I_seg_active, I_stale_locks_init_mode)	do {	\
	struct nvmeibt_seg_active				*__seg_active = (I_seg_active);									\
	struct nvmeibt_disk_segment_topo_ctx	*__seg_topo = &(__seg_active->active_seg_topo);					\
	enum NVMEIBT_MEM_TBL_INIT_MODE			__prev_val;														\
	if (!__seg_active) {																					\
		N_Wf(name ## _1, "seg_active=NULL");																\
		break;																								\
	}																										\
	__prev_val = __seg_topo->stale_locks_init_mode;															\
	NNVMEIBT_SEG_TOPO_SET_STALE_LOCKS_INIT_MODE(name, nvmeibt_seg_active_UUID(__seg_active),				\
		nvmeibt_seg_active_get_applied_seg_lot(__seg_active),												\
		__seg_topo, I_stale_locks_init_mode, 'A');															\
	if (__seg_topo->stale_locks_init_mode != __prev_val)													\
		nvmeibt_seg_active_mark_serialize_active_topo_for_leader_required(__seg_active);					\
} while (0)

/******************************************************************************/

void nvmeibt_global_add_seg_active_post_update_action(struct nvmeibt_seg_active *seg_active);
#define NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(name, seg_active) do {											\
	struct nvmeibt_seg_active	*__seg_active__ = (seg_active);																	\
	if (__seg_active__ && XDLIST_NULL(&(__seg_active__->global_seg_active_post_update_action_link))) {							\
		N_Tf(name, "seg_active=@UUID_8 MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED", nvmeibt_seg_active_UUID_8(__seg_active__));		\
		nvmeibt_global_add_seg_active_post_update_action(__seg_active__);														\
	}																															\
} while (0)

#define NVMEIBT_SEG_ACTIVE_CLEAR_ARE_POST_UPDATE_ACTIONS_REQUIRED(name, seg_active) do {										\
	struct nvmeibt_seg_active	*__seg_active__ = (seg_active);																	\
	if (__seg_active__ && !XDLIST_NULL(&(__seg_active__->global_seg_active_post_update_action_link))) {							\
		N_Tf(name, "seg_active=@UUID_8 CLEAR_ARE_POST_UPDATE_ACTIONS_REQUIRED", nvmeibt_seg_active_UUID_8(__seg_active__));		\
		XDLIST_DEL(&(__seg_active__->global_seg_active_post_update_action_link));												\
	}																															\
} while (0)

#define NNVMEIBT_SEG_ACTIVE_SET_IS_EXPECTED_TO_HAVE_STALE_LOCKS(name, __seg_active__, __new_value__)	do {	\
	if (__seg_active__) {																						\
		if ((__seg_active__)->is_expected_to_have_stale_locks != (__new_value__)) {								\
			N_Tf(name, "SET_IS_EXPECTED_TO_HAVE_STALE_LOCKS seg=@UUID_8 (@FLAGS_INT-->@FLAGS_INT)",				\
				nvmeibt_seg_active_UUID_8((__seg_active__)),													\
				(__seg_active__)->is_expected_to_have_stale_locks, (__new_value__));							\
			(__seg_active__)->is_expected_to_have_stale_locks = (__new_value__);								\
			if (!(__seg_active__)->is_expected_to_have_stale_locks) {											\
				nvmeibt_seg_active_clear_stale_rebuild_required(__seg_active__);								\
			}																									\
		}																										\
	} else {																									\
		N_Ef(name ## _error, "SET_IS_EXPECTED_TO_HAVE_STALE_LOCKS seg_active=NULL)");							\
	}																											\
} while (0)

#define NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(name, __seg_active__, __user__, __inc_val__) do {					\
	if (__seg_active__) {																						\
		N_Tf(name, "seg_active=@UUID_8 ref_count (@INT->@INT) user=" __user__,									\
			nvmeibt_seg_active_UUID_8((__seg_active__)),														\
			(__seg_active__)->ref_count, (__seg_active__)->ref_count + (__inc_val__));							\
			(__seg_active__)->ref_count += (__inc_val__);														\
	} else {																									\
		N_Ef(name ## _error, "SET_IS_DURING_PERSISTENCY_STORE seg_active=NULL)");								\
	}																											\
} while (0)

#define NVMEIBT_SEG_ACTIVE_SET_COMMITTED_RESERVATION_MODE_VERSION(name, __seg_active__, __new_value__)   do {	\
	if (__seg_active__) {																						\
	   N_Tf(name ## 1, "seg=@UUID_8 SET_COMMITTED_RESERVATION_MODE_VERSION(@RES_MOD_VER-->@RES_MOD_VER)",		\
		   nvmeibt_seg_active_UUID_8((__seg_active__)),															\
		   (__seg_active__)->committed_reservation_mode_version, (__new_value__));								\
	   (__seg_active__)->committed_reservation_mode_version = (__new_value__);									\
	} else {																									\
	   N_Ef(name ## 2, "SET_COMMITTED_RESERVATION_MODE_VERSION seg_active=NULL)");								\
	}																											\
} while (0)

#define NVMEIBT_SEG_ACTIVE_SET_ACTIVE_RESERVATION_MODE_VERSION(name, __seg_active__, __new_value__)  do {		\
	if (__seg_active__) {																						\
	   N_Tf(name ## 1, "seg=@UUID_8 SET_ACTIVE_RESERVATION_MODE_VERSION(@RES_MOD_VER-->@RES_MOD_VER)",			\
		   nvmeibt_seg_active_UUID_8((__seg_active__)),															\
		   (__seg_active__)->active_reservation_mode_version, (__new_value__));									\
	   (__seg_active__)->active_reservation_mode_version = (__new_value__);										\
	} else {																									\
	   N_Ef(name ## 2, "SET_ACTIVE_RESERVATION_MODE_VERSION seg_active=NULL)");									\
	}																											\
} while (0)

#define NVMEIBT_SEG_ACTIVE_SET_SUBMITTED_RESERVATION_MODE_VERSION(name, __seg_active__, __new_value__)   do {	\
	if (__seg_active__) {																						\
	   N_Tf(name ## 1, "seg=@UUID_8 SET_SUBMITTED_RESERVATION_MODE_VERSION(@RES_MOD_VER-->@RES_MOD_VER)",		\
		   nvmeibt_seg_active_UUID_8((__seg_active__)),															\
		   (__seg_active__)->submitted_reservation_mode_version, (__new_value__));								\
	   (__seg_active__)->submitted_reservation_mode_version = (__new_value__);									\
	} else {																									\
	   N_Ef(name ## 2, "SET_SUBMITTED_RESERVATION_MODE_VERSION seg_active=NULL)");								\
	}																											\
} while (0)

#define NVMEIBT_SEG_ACTIVE_SET_HIGHEST_RESERVATION_MODE_VERSION(name, __seg_active__, __new_value__) do {		\
	if (__seg_active__) {																						\
	   N_Tf(name ## 1, "seg=@UUID_8 SET_HIGHEST_RESERVATION_MODE_VERSION(@RES_MOD_VER-->@RES_MOD_VER)",			\
		   nvmeibt_seg_active_UUID_8((__seg_active__)),															\
		   (__seg_active__)->highest_reservation_mode_version, (__new_value__));								\
	   (__seg_active__)->highest_reservation_mode_version = (__new_value__);									\
	} else { 																									\
	   N_Ef(name ## 2, "SET_HIGHEST_RESERVATION_MODE_VERSION seg_active=NULL)");								\
	}																											\
} while (0)

/* alloc/free */
// #define NVMEIBT_SEG_ACTIVE_ALLOC(seg)	({ nvmeibt_seg_active_alloc(seg); })
#define NVMEIBT_SEG_ACTIVE_FREE_MEM_AND_PROCESSES(seg_active)		do { nvmeibt_seg_active_free_mem_and_processes(seg_active); seg_active = NULL; } while (0)

#define NVMEIBT_SEG_ACTIVE_REMOVE_ACTIVE_REGISTRANT_FROM_HASHES(__seg_active__, __reg_ctx__)	do {														\
	if ((__seg_active__) && (__reg_ctx__)) {																												\
		nvmeib_hash_delete_uint32_t(__seg_active__->active_registrants_hash_by_lockid, nvmeib_lockid_purify(__reg_ctx__->reg_lock_id));						\
		nvmeib_hash_delete_uint64_t(__seg_active__->active_registrants_hash_by_handle, __reg_ctx__->client_messaging_handle);								\
	}																																						\
} while (0)

#define NVMEIBT_SEG_ACTIVE_ADD_ACTIVE_REGISTRANT_TO_HASHES(__seg_active__, __reg_ctx__)	do {																			\
	if ((__seg_active__) && (__reg_ctx__)) {																															\
		__seg_active__->active_reservation_mode_version = __seg_active__->committed_reservation_mode_version;															\
		nvmeib_hash_add_uint32_t(__seg_active__->active_registrants_hash_by_lockid, nvmeib_lockid_purify(__reg_ctx__->reg_lock_id), (__reg_ctx__));						\
		nvmeib_hash_add_uint64_t(__seg_active__->active_registrants_hash_by_handle, __reg_ctx__->client_messaging_handle, (__reg_ctx__));								\
	}																																									\
} while (0)

#define NDUMP_N_ACTIVE_REGISTRANTS(name, seg_active) ({										\
	if (seg_active) {																		\
		N_Tf(name, "seg=@UUID_8 n_active=@N_REGISTRANTS(A=@N_REGISTRANTS)",					\
			nvmeibt_seg_active_UUID_8(seg_active),											\
			nvmeibt_seg_active_n_active_registrants(seg_active),							\
			nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(seg_active));	\
	}																						\
})

/******* Declarations of the ".c" functions ********/

int nvmeibt_seg_active_global_scrubbing_one_time_init(void);

void nvmeibt_seg_active_reset_serjio_clean_range_state_on_new_config_or_topo(struct nvmeibt_seg_active *seg_active);
int nvmeibt_seg_active_update_serjio_range_cleaned(char *seg_id);
//bool nvmeibt_seg_active_is_different_topo_for_leader(struct nvmeibt_disk_segment_topo_ctx *new_t, struct nvmeibt_disk_segment_topo_ctx *old_t);
void nvmeibt_seg_active_notify_serjio_if_seg_is_being_deleted(struct nvmeibt_seg_active *seg_active);

void nvmeibt_seg_active_init_locks_table(struct nvmeibt_seg_active *seg_active);

BOOL nvmeibt_seg_active_is_closing_to_reg(const struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_mark_is_closing_to_reg(struct nvmeibt_seg_active *seg_active);

bool nvmeibt_seg_active_final_free_if_not_in_use(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_free_mem_and_processes(struct nvmeibt_seg_active *seg_active);
struct nvmeibt_seg_active *nvmeibt_seg_active_alloc(struct nvmeibt_disk_segment *disk_segment);
void nvmeibt_seg_active_upd_liveliness_according_to_local_disk(struct nvmeibt_seg_active *seg_active);
int nvmeibt_seg_active_upd_metadata_gpt_entry_and_ctrl(struct nvmeibt_seg_active *seg_active, struct nvmeibt_disk_gpt_partition_entry *metadata_gpt_entry,
													   struct nvmeibt_seg_active_metadata_ctrl *metadata_ctrl);
struct nvmeibt_seg_active *nvmeibt_seg_active_create(const union nvmeib_uuid *uuid, struct nvmeibt_local_disk *local_disk,
													 struct nvmeibt_disk_gpt_partition_entry *metadata_gpt_entry,
													 struct nvmeibt_seg_active_metadata_ctrl *metadata_ctrl_fr_persist);
void nvmeibt_seg_active_we_got_its_seg(struct nvmeibt_seg_active *seg_active);

int nvmeibt_seg_active_record_registrant_disconnect(struct nvmeibt_seg_active *seg_active, struct nvmeibt_registrant_ctx *reg_ctx);
void nvmeibt_seg_active_remove_registrant_disconnect_record_from_seg(struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id reg_lock_id, bool is_force);

/* stale locks */
void nvmeibt_seg_active_delete_all_stale_locks_of_registrant(
	struct nvmeibt_seg_active *seg_active, struct nvmeibt_registrant_ctx *reg_ctx);
struct nvmeibt_registrant_ctx *nvmeibt_seg_active_add_blkset_to_stale_locks_hash(
	struct nvmeibt_registrant_ctx *reg_ctx, unsigned long long seg_blkset_no, bool existing_lock_id_bits_is_read);

/* recovery/rebuild */
bool nvmeibt_seg_active_mark_cold_recovery_required_if_needed(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_clear_cold_recovery_required(struct nvmeibt_seg_active *seg_active);
BOOL nvmeibt_seg_active_is_cold_recovery_required(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_mark_stale_rebuild_required(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_clear_stale_rebuild_required(struct nvmeibt_seg_active *seg_active);
BOOL nvmeibt_seg_active_is_stale_rebuild_required(struct nvmeibt_seg_active *seg_active);
bool nvmeibt_seg_active_mark_txid_rebuild_required_if_needed(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_clear_txid_rebuild_required(struct nvmeibt_seg_active *seg_active);
BOOL nvmeibt_seg_active_is_txid_rebuild_required(struct nvmeibt_seg_active *seg_active);
bool nvmeibt_seg_active_mark_dirty_rebuild_required_if_needed(struct nvmeibt_seg_active *seg_active);
BOOL nvmeibt_seg_active_is_dirty_rebuild_required(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_clear_dirty_rebuild_required(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_mark_JGC_rebuild_required(struct nvmeibt_seg_active *seg_active);
BOOL nvmeibt_seg_active_is_JGC_rebuild_required(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_clear_JGC_rebuild_required(struct nvmeibt_seg_active *seg_active);
//
void nvmeibt_seg_active_set_n_dirty_bits_remaining(struct nvmeibt_seg_active *, uint64_t n_remaining);
void nvmeibt_seg_active_set_n_stale_locks_remaining(struct nvmeibt_seg_active *, uint64_t n_remaining);
static inline uint64_t nvmeibt_seg_active_get_n_dirty_bits_remaining(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->dirty_rebuild_ctx.n_blksets_remaining : 0);
}
static inline uint64_t nvmeibt_seg_active_get_n_stale_locks_remaining(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->stale_rebuild_ctx.n_blksets_remaining : 0);
}
static inline uint64_t nvmeibt_seg_active_get_n_txid_remaining(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->txid_rebuild_ctx.n_blksets_remaining : 0);
}
static inline uint64_t nvmeibt_seg_active_get_n_scrubbing_remaining(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->scrubbing_ctx.n_blksets_remaining : 0);
}
void nvmeibt_seg_active_set_n_txid_remaining(struct nvmeibt_seg_active *, uint64_t n_remaining);
//
void nvmeibt_seg_active_stop_recovery_tasks(struct nvmeibt_seg_active *seg_active);

/* miscelaneous */
int nvmeibt_seg_active_handle_blkset_recovered(struct nvmeibs_msg_s2t_blkset_recovered *blkset_recovered_msg);

void nvmeibt_recovery_set_scrub_default_period_days(int64_t period_days);
int64_t nvmeibt_recovery_get_scrub_default_period_days(void);
void nvmeibt_recovery_set_n_blksets_per_scrub_iteration(int64_t val);
int64_t nvmeibt_recovery_get_n_blksets_per_scrub_iteration(void);

void nvmeibt_seg_active_mark_zeroing_required_as_needed(struct nvmeibt_seg_active *seg_active);
void nvmeibt_recovery_set_is_stale_rebuild_enabled(bool disable_stale_rebuild);
int64_t nvmeibt_recovery_client_batch_n_blksets_get(void);
void nvmeibt_recovery_client_batch_n_blksets_set(int64_t batch_size);
void nvmeibt_recovery_set_is_scrub_enabled(int64_t is_enabled);
int64_t nvmeibt_recovery_get_is_scrub_enabled(void);
void nvmeibt_seg_active_set_zeroing_test(bool zeroing_fail);
void nvmeibt_seg_active_stop_all_recoveries_and_registrations(struct nvmeibt_seg_active *seg_active, bool is_brute_force_disconnect_required);
bool nvmeibt_seg_active_is_conf_corrupted(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_stop_all_recoveries_and_registrations_on_deleted_segs(void);
void nvmeibt_seg_active_handle_post_update_actions(struct nvmeibt_seg_active *seg_active);

BOOL nvmeibt_seg_active_is_disk_format_zeroing_done_for_me(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_mark_as_fully_zeroed(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_zero_if_is_being_deleted_and_unused(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_inc_n_active_registrants_on_applied_praid(struct nvmeibt_seg_active *seg_active);

void nvmeibt_seg_active_get_recovery_praid_version(
	enum NVMEIBT_RECOVERY_TYPE recovery_type, struct nvmeibt_seg_active *seg_active, int *praid_version);
void nvmeibt_seg_active_get_recovery_blkset_range(
	enum NVMEIBT_RECOVERY_TYPE recovery_type, struct nvmeibt_seg_active *seg_active, u64 *blkset_start, u64 *blkset_count);

typedef int (*printf_fn_t)(void *ctx, const char *fmt, ...);
BOOL nvmeibt_seg_active_is_any_seg_active_during_metadata_store(void);
void nvmeibt_seg_active_launch_store_of_all_seg_actives_metadata(void);
int nvmeibt_seg_active_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_seg_active *seg_active, bool is_full_info_needed);
int nvmeibt_seg_active_print_all_seg_actives_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
void stale_locks_hash_to_string(printf_fn_t printf_fn, void *printf_ctx, struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_scan_all(void);

enum NVMEIBT_RECOVERY_TYPE;
enum NVMEIBT_RECOVERY_STATUS;
void nvmeibt_seg_active_recovery_done(
	u64 tid, struct nvmeibt_seg_active *seg_active,
	enum NVMEIBT_RECOVERY_TYPE recovery_type,
	enum NVMEIBT_RECOVERY_STATUS recovery_status);

// The following have recovery-NAME and reside in seg_active. Need to do something about it
void nvmeibt_recovery_execute_dirty_rebuilds_as_needed(void);
void nvmeibt_recovery_execute_cold_recoveries_as_needed(void);
void nvmeibt_recovery_execute_stale_and_txid_rebuilds_as_needed(void);
void nvmeibt_recovery_execute_JGC_rebuilds_as_needed(void);
void nvmeibt_recovery_trigger_local_seg_JGC(char *disk_segment_urn_uuid_str, char *ldisk_id_str);
void nvmeibt_seg_active_upd_active_topo_from_applied_topo(struct nvmeibt_seg_active *seg_active);
struct nvmeibt_seg_active *nvmeibt_find_seg_active_of_specific_local_disk_by_uuid(struct nvmeibt_local_disk *local_disk, const union nvmeib_uuid *seg_uuid);
struct nvmeibt_seg_active *nvmeibt_find_seg_active_on_all_local_disks_by_uuid(const union nvmeib_uuid *seg_uuid);
bool nvmeibt_seg_active_send_one_seg_rebuild_progress_report_to_mgmt(struct nvmeibt_seg_active *seg_active, struct nvmeibt_Str *json_payload);
union nvmeib_lock_blkset_entry *nvmeibt_seg_active_get_locks_tbl_ptr(struct nvmeibt_seg_active *seg_active);
void nvmeibt_recovery_execute_scrubbing_as_needed(void);

/******* Static inline forward declarations ********/

static inline struct nvmeibt_praid *nvmeibt_seg_active_get_praid(struct nvmeibt_seg_active *seg_active);
static inline struct nvmeibt_block_device *nvmeibt_seg_active_get_blkdev(struct nvmeibt_seg_active *seg_active);
static inline const char *nvmeibt_seg_active_blkdev_name(struct nvmeibt_seg_active *seg_active);
static inline struct nvmeibt_praid_lot *nvmeibt_seg_active_get_applied_praid_lot(struct nvmeibt_seg_active *seg_active);
static inline struct nvmeibt_praid_lot *nvmeibt_seg_active_get_committed_praid_lot(struct nvmeibt_seg_active *seg_active);
static inline struct nvmeibt_seg_lot *nvmeibt_seg_active_get_applied_seg_lot(struct nvmeibt_seg_active *seg_active);
static inline struct nvmeibt_seg_lot *nvmeibt_seg_active_get_committed_seg_lot(struct nvmeibt_seg_active *seg_active);
static inline bool nvmeibt_seg_active_is_deprecated_in_config(struct nvmeibt_seg_active *seg_active);
static inline bool nvmeibt_seg_active_is_zeroing_explicitly_required_according_to_config(struct nvmeibt_seg_active *seg_active);
static inline bool nvmeibt_seg_active_is_deleted_in_config(struct nvmeibt_seg_active *seg_active);
static inline bool nvmeibt_seg_active_is_jbod(struct nvmeibt_seg_active *seg_active);
static inline unsigned long long nvmeibt_seg_active_get_blkset_s(struct nvmeibt_seg_active *seg_active);
static inline struct nvmeibt_disk *nvmeibt_seg_active_get_disk(struct nvmeibt_seg_active *seg_active);
static inline enum PRAID_REGISTRANTS_SYNC_CMD nvmeibt_seg_active_get_registrants_sync_cmd(struct nvmeibt_seg_active *seg_active);

/******* Static inline with no external dependencies ********/

static inline const union nvmeib_uuid *nvmeibt_seg_active_UUID(const struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? &(seg_active->uuid) : NULL);
}

static inline unsigned int nvmeibt_seg_active_UUID_8(const struct nvmeibt_seg_active *seg_active)
{
	return nvmeib_uuid_first_4_bytes(nvmeibt_seg_active_UUID(seg_active));
}

static inline BOOL nvmeibt_seg_active_is_waiting_for_serjio_clean_range_done(const struct nvmeibt_seg_active *seg_active)
{
	return (seg_active && (seg_active->applied_serjio_clean_range_state & (SERJIO_CLEAN_RANGE_STATE_IN_WORK | SERJIO_CLEAN_RANGE_STATE_REQUIRED)));
}

static inline bool nvmeibt_seg_active_get_is_expected_to_have_stale_locks(const struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->is_expected_to_have_stale_locks : 0);
}

/* misc helpers */
TODO(Remove the following function);
static inline struct nvmeibt_disk_segment *nvmeibt_seg_active_get_disk_segment(const struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->disk_segment : NULL);
}

static inline struct nvmeibt_seg_active_metadata_ctrl *nvmeibt_seg_active_get_persistent_metadata(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->persistent_metadata : NULL);
}

/* BOOL helpers */
static inline BOOL nvmeibt_seg_active_is_zeroing_state_launchable(const struct nvmeibt_seg_active *seg_active)
	{ return (seg_active && seg_active->applied_zeroing_state & (NVMEIBT_ZEROING_STATE_IN_WORK | NVMEIBT_ZEROING_STATE_REQUIRED)); }

static inline BOOL nvmeibt_seg_active_is_zeroing_state_WQ_task_in_work(const struct nvmeibt_seg_active *seg_active)
	{ return (seg_active &&
			  (seg_active->applied_zeroing_state & (NVMEIBT_ZEROING_STATE_IN_WORK | NVMEIBT_ZEROING_STATE_IN_WORK_CANCELLING))); }

static inline bool nvmeibt_seg_active_is_zeroing_state_skipable(const struct nvmeibt_seg_active *seg_active)
{
	return (seg_active &&
			(seg_active->applied_zeroing_state & NVMEIBT_ZEROING_STATE_DONE ||
			 seg_active->applied_zeroing_state & NVMEIBT_ZEROING_STATE_NOT_NEEDED));
}

/* registrants */
static inline int nvmeibt_seg_active_n_active_registrants(const struct nvmeibt_seg_active *seg_active)
	{ return (seg_active ? nvmeib_hash_get_n_elements(seg_active->active_registrants_hash_by_lockid) : 0); }

static inline int nvmeibt_seg_active_n_active_registrants_on_applied_praid_version(const struct nvmeibt_seg_active *seg_active)
	{ return (seg_active ? seg_active->n_active_registrants_on_active_praid_version : 0); }

static inline int nvmeibt_seg_active_n_longing_registrants(const struct nvmeibt_seg_active *seg_active)
	{ return (seg_active ? nvmeib_hash_get_n_elements(seg_active->longing_registrants_hash_by_handle) : 0); }

static inline int nvmeibt_seg_active_n_awaited_lockids(const struct nvmeibt_seg_active *seg_active)
	{ return (seg_active ? XHASHTABLE_N_ELEMENTS(&seg_active->awaited_lockids_hash_by_lockid) : 0); }

static inline BOOL nvmeibt_seg_active_is_any_recovery_in_the_air(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active &&
			(seg_active->cold_recovery_ctx.tid ||
			 seg_active->dirty_rebuild_ctx.tid ||
			 seg_active->stale_rebuild_ctx.tid ||
			 seg_active->JGC_rebuild_ctx.tid ||
			 seg_active->scrubbing_ctx.tid));
}

static inline struct nvmeibt_disk_segment_topo_ctx *nvmeibt_seg_active_get_active_seg_topo(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? &(seg_active->active_seg_topo) : NULL);
}

static inline bool nvmeibt_seg_active_is_during_persistency_store(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active && (seg_active->ref_count > 1));
}

static inline int nvmeibt_seg_active_get_active_praid_version_major(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->active_seg_topo.seg_praid_version_major : PRAID_VERSION_INVALID_VALUE);
}

static inline int nvmeibt_seg_active_get_active_praid_version_minor(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->active_seg_topo.seg_praid_version_minor : PRAID_VERSION_INVALID_VALUE);
}

void nvmeibt_disk_segment_active_mark_reserialization_required(struct nvmeibt_disk_segment *seg);
static inline void nvmeibt_seg_active_mark_serialize_active_topo_for_leader_required(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	if (seg) {
		nvmeibt_disk_segment_active_mark_reserialization_required(seg);
		// NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(brjhg8d, seg_active);
	}
}

static inline struct nvmeibt_disk_gpt_partition_entry *nvmeibt_seg_active_get_metadata_gpt_entry(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->metadata_gpt_entry : NULL);
}

static inline bool nvmeibt_seg_active_are_registrants_aligned_with_sync_cmd(const struct nvmeibt_seg_active *seg_active)
{
	return (!seg_active || seg_active->active_seg_topo.active_seg_flags.are_praid_registrants_aligned_with_sync_cmd);
}

static inline void nvmeibt_seg_active_set_registrants_aligned_with_sync_cmd(struct nvmeibt_seg_active *seg_active, bool val)
{
	if (seg_active) {
		seg_active->active_seg_topo.active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = val;
	}
}

static inline struct nvmeibt_local_disk *nvmeibt_seg_active_get_local_disk(const struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->local_disk : NULL);
}

static inline void lock_stale_locks_hash(struct nvmeibt_seg_active *seg_active)
{
	if (pthread_mutex_lock(&seg_active->stale_locks_hash_mutex)) {
		N_Ef(ry876n2, "Failed to lock stale locks mutex (@AUTO_ERRNO)");
	    nvmeibt_abort(ES_FATAL);
	}
}
static inline void unlock_stale_locks_hash(struct nvmeibt_seg_active *seg_active)
{
	if (pthread_mutex_unlock(&seg_active->stale_locks_hash_mutex)) {
		N_Ef(ry876i3, "Failed to unlock stale locks mutex (@AUTO_ERRNO)");
	    nvmeibt_abort(ES_FATAL);
	}
}

/******* Includes needed for static inline functions ********/

#include "nvmeibt_disk_segment.h"
#include "nvmeibt_praid.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_recovery.h"
#include "nvmeibt_local_disk.h"

/******* Static inline functions that depend on other functions ******/

struct nvmeibt_seg_lot;
struct nvmeibt_praid_lot;

static inline struct nvmeibt_praid *nvmeibt_seg_active_get_praid(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	return (seg ? seg->seg_mgmt.its_praid : NULL);
}

static inline struct nvmeibt_praid_topo_ctx *nvmeibt_seg_active_get_praid_applied_topo(struct nvmeibt_seg_active *seg_active)
{
	return nvmeibt_disk_segment_get_praid_applied_topo(nvmeibt_seg_active_get_disk_segment(seg_active));
}

static inline struct nvmeibt_block_device *nvmeibt_seg_active_get_blkdev(struct nvmeibt_seg_active *seg_active)
	{ return nvmeibt_disk_segment_get_blkdev(nvmeibt_seg_active_get_disk_segment(seg_active)); }

static inline const char *nvmeibt_seg_active_blkdev_name(struct nvmeibt_seg_active *seg_active)
	{ return nvmeibt_blkdev_name(nvmeibt_seg_active_get_blkdev(seg_active)); }

static inline struct nvmeibt_praid_lot *nvmeibt_seg_active_get_applied_praid_lot(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_praid *praid = nvmeibt_seg_active_get_praid(seg_active);
	return (praid ? &praid->praid_follower.applied_praid_lot : NULL);
}

static inline struct nvmeibt_praid_lot *nvmeibt_seg_active_get_committed_praid_lot(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_praid *praid = nvmeibt_seg_active_get_praid(seg_active);
	return (praid ? &praid->praid_follower.committed_praid_lot : NULL);
}

static inline struct nvmeibt_seg_lot *nvmeibt_seg_active_get_applied_seg_lot(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	return (seg ? &seg->seg_follower.applied_seg_lot : NULL);
}

static inline struct nvmeibt_seg_lot *nvmeibt_seg_active_get_committed_seg_lot(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	return (seg ? &seg->seg_follower.committed_seg_lot : NULL);
}

static inline bool nvmeibt_seg_active_is_deprecated_in_config(struct nvmeibt_seg_active *seg_active)
{
	return nvmeibt_seg_lot_is_deprecated_in_config(nvmeibt_seg_active_get_applied_seg_lot(seg_active));
}

static inline bool nvmeibt_seg_active_is_zeroing_explicitly_required_according_to_config(struct nvmeibt_seg_active *seg_active)
{
	return nvmeibt_seg_lot_is_zeroing_explicitly_required_according_to_config(nvmeibt_seg_active_get_applied_seg_lot(seg_active));
}

static inline bool nvmeibt_seg_active_is_deleted_in_config(struct nvmeibt_seg_active *seg_active)
{
	return nvmeibt_seg_lot_is_deleted_in_config(nvmeibt_seg_active_get_applied_seg_lot(seg_active));
}

static inline struct nvmeibt_seg_mgmt *nvmeibt_seg_active_get_seg_mgmt(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	return (seg ? &seg->seg_mgmt : NULL);
}

static inline const struct nvmeibt_disk_segment_config *nvmeibt_seg_active_get_committed_seg_lot_config(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_seg_lot *seg_lot = nvmeibt_seg_active_get_committed_seg_lot(seg_active);
	return (seg_lot ? &seg_lot->from_config : NULL);
}

static inline bool nvmeibt_seg_active_is_jbod(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	return (seg && nvmeibt_praid_is_jbod(seg->seg_mgmt.its_praid));
}

static inline unsigned long long nvmeibt_seg_active_get_blkset_s(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	return (seg ? seg->seg_mgmt.lb_s / NUM_4KBLKS_IN_BLKSET : -1ULL);
}

static inline enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE nvmeibt_seg_active_dirty_bits_state(struct nvmeibt_seg_active *seg_active)
{
	return (seg_active ? seg_active->active_seg_topo.dirty_bits_state : NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
}

static inline const char *nvmeibt_seg_active_dirty_bits_state_str(struct nvmeibt_seg_active *seg_active)
{
	return dirty_bits_state_str(seg_active ? seg_active->active_seg_topo.dirty_bits_state : NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
}

static inline int nvmeibt_seg_active_active_config_version(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_seg_lot *seg_lot = nvmeibt_seg_active_get_applied_seg_lot(seg_active);
	return (seg_lot ? seg_lot->from_config.version : -1);
}

static inline struct nvmeibt_disk *nvmeibt_seg_active_get_disk(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment *seg = nvmeibt_seg_active_get_disk_segment(seg_active);
	return (seg ? seg->seg_mgmt.its_disk : NULL);
}

static inline bool nvmeibt_seg_active_is_config_EC(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_praid *praid = nvmeibt_seg_active_get_praid(seg_active);
	return (praid ? nvmeibt_praid_is_type_EC(praid) : 0);
}

static inline enum PRAID_REGISTRANTS_SYNC_CMD nvmeibt_seg_active_get_registrants_sync_cmd(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_praid_lot *praid_lot = nvmeibt_seg_active_get_applied_praid_lot(seg_active);
	return (praid_lot ? nvmeibt_praid_topo_get_registrants_sync_cmd(&praid_lot->topo_ctx) : PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN);
}

static inline char *nvmeibt_disk_segment_id_str(struct nvmeibt_disk_segment *disk_segment);
static inline char *nvmeibt_seg_active_id_str(struct nvmeibt_seg_active *seg_active)
	{ return nvmeibt_disk_segment_id_str(nvmeibt_seg_active_get_disk_segment(seg_active)); }

static inline char *nvmeibt_seg_active_praid_id_str(struct nvmeibt_seg_active *seg_active)
	{ return nvmeibt_praid_id_str(nvmeibt_seg_active_get_praid(seg_active)); }

#endif	// #ifdef NVMEIBT_SEG_ACTIVE
