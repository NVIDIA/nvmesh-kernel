#ifndef NVMEIBT_DISK_SEGMENT
#define NVMEIBT_DISK_SEGMENT

#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_praid_basics.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
#include "nvmeibt_mm_json.h"
#include "nvmeibt_read_config.h"

struct nvmeibt_disk;
struct nvmeibt_praid;
struct nvmeibt_seg_active;

enum SEGMENT_STATUS_FOR_MGMT {
	SEGMENT_STATUS_FOR_MGMT_UNKNOWN = 0x0,
	SEGMENT_STATUS_FOR_MGMT_UNDER_RECOVERY = 0x1 << 0,
	SEGMENT_STATUS_FOR_MGMT_NORMAL = 0x1 << 1,
	SEGMENT_STATUS_FOR_MGMT_DEPRECATED = 0x1 << 2,
	SEGMENT_STATUS_FOR_MGMT_DEAD = 0x1 << 3,
	SEGMENT_STATUS_FOR_MGMT_REPLACEMENT = 0x1 << 4,
	SEGMENT_STATUS_FOR_MGMT_CONF_CORRUPTED = 0x1 << 5,
	SEGMENT_STATUS_FOR_MGMT_BOOTING = 0x1 << 6,
	SEGMENT_STATUS_FOR_MGMT_ZEROING = 0x1 << 7,
	SEGMENT_STATUS_FOR_MGMT_INITIALIZING = 0x01 << 8,
};

enum SEGMENT_VITALITY_FOR_MGMT {
	SEGMENT_VITALITY_UNKNOWN = 0x0,
	SEGMENT_VITALITY_DOWN = 0x1 << 0,
	SEGMENT_VITALITY_UP = 0x1 << 2,
};

struct nvmeibt_urn_uuid;

struct nvmeibt_seg_lot {
	struct nvmeibt_disk_segment_config			from_config;	// A pointer to the object as received from mgmt
	struct nvmeibt_disk_segment					*my_seg;		// Used to access its disk object, possibly more
	struct nvmeibt_praid_lot					*praid_lot;
	struct nvmeibt_disk_segment_topo_ctx		seg_topo;
	struct xdlist 								praid_all_seg_lots_link;
	BOOL										is_replacement;
	bool										is_FIRST_USE_EVER;	// Currently used only by the calculated_seg_lot, harmless for the others
																	// calculated_seg_lot->is_FIRST_USE_EVER is set when {a new seg is added from mgmt / init_mode is FIRST_USE_EVER in baseline}
																	// It is cleared when we see any traces of non-FIRST_USE_EVER in baseline / remote_applied
};

struct nvmeibt_seg_mgmt {
	struct nvmeibt_urn_uuid						urn_uuid;
	struct nvmeibt_disk							*its_disk;
	struct nvmeibt_praid						*its_praid;
	union nvmeib_uuid							praid_id;
	union nvmeib_uuid							disk_id;
	unsigned long long int						pba_s;
	unsigned long long int						pba_e;
	unsigned long long int						lb_s;
	unsigned long long int						lb_e;
	BOOL										is_replacement;
};

struct nvmeibt_seg_leader {
	struct nvmeibt_seg_lot						baseline_seg_lot;
	struct nvmeibt_seg_lot						calculated_seg_lot;
	struct nvmeibt_seg_lot						to_report_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx		remote_seg_topo;
	unsigned long long							last_remote_applied_node_local_serialization_version;
	BOOL										is_removed_from_remote_applied;	// Reported-removed (vs. not-reported-yet)
};

struct nvmeibt_seg_follower {
	struct nvmeibt_seg_lot						applied_seg_lot;
	struct nvmeibt_seg_lot						committed_seg_lot;
	struct nvmeibt_seg_active					*seg_active;
	int											applied_for_clients_topo_praid_version;
	BOOL										is_modified_in_last_config;		// First config ever or update which changed the config
};

struct nvmeibt_disk_segment {
	struct nvmeibt_disk_segment_config			from_config;
	struct nvmeibt_seg_mgmt						seg_mgmt;		
	struct nvmeibt_seg_leader					seg_leader;		
	struct nvmeibt_seg_follower					seg_follower;		
	struct xdlist								topo_link;
	struct xdlist								praid_all_segs_link;
	int											config_tag;
	BOOL										is_conf_corrupted;
	uint8_t										trim_flags;
	BOOL										is_drive_write_error;
};

enum nvmeibt_seg_remove_rv {
	NVMEIBT_SEG_STILL_IN_CONFIG,
	NVMEIBT_SEG_REMOVED,
	NVMEIBT_SEG_ZEROING_IN_PROCESS,
	NVMEIBT_SEG_RECOVERY_IN_PROCESS
};

static inline char get_topo_char(struct nvmeibt_seg_lot *seg_lot)
{
	struct nvmeibt_disk_segment			*seg = seg_lot->my_seg;

    return (&seg->seg_leader.calculated_seg_lot == seg_lot ?	'L' :
			&seg->seg_leader.baseline_seg_lot == seg_lot ?		'B' :
			&seg->seg_leader.to_report_seg_lot == seg_lot ?		'M' :
			&seg->seg_follower.committed_seg_lot == seg_lot ?	'C' :
			&seg->seg_follower.applied_seg_lot == seg_lot ?		'P' :
			'U');
}

#define NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS(name, _uuid, p_seg_lot, seg_topo, val, topo_char) do {					\
		struct nvmeibt_seg_lot					*__seg_lot = (p_seg_lot);										\
		struct nvmeibt_disk_segment				*_seg = __seg_lot ?__seg_lot->my_seg : NULL;					\
		struct nvmeibt_disk_segment_topo_ctx	*_topo_ctx = (seg_topo);										\
		enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	_val = (val);													\
		if (_val ==  NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO) {														\
			if (nvmeibt_disk_is_explicitly_out_of_service(nvmeibt_disk_segment_get_disk(_seg)) ) {				\
				N_Tf(name ## _2, "Disk=@STR is_explicitly_out_of_service. Converting X_ZERO-->X_DONE",			\
					 nvmeibt_disk_segment_get_disk_ldisk_id_str(_seg));											\
				_val = NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE;														\
			}																									\
		}																										\
		if (_topo_ctx->dirty_bits_state != _val) {																\
			N_Tf(name, "SET_DIRTY_BITS(@TOPO_CTX_CHAR-@UUID_8: @DIRTY_BITS_STR->@DIRTY_BITS_STR)",				\
				 topo_char,																						\
				 nvmeib_uuid_first_4_bytes(_uuid),																\
				 dirty_bits_state_str(_topo_ctx->dirty_bits_state),												\
				 dirty_bits_state_str(_val));																	\
			_topo_ctx->dirty_bits_state = _val;																	\
		}																										\
	} while (0)

#define NNVMEIBT_SEG_TOPO_SET_TXID_INIT_MODE(name, _uuid, p_seg_lot, seg_topo, in_val, topo_char) do {			\
		struct nvmeibt_seg_lot					*__seg_lot_ = (p_seg_lot);										\
		struct nvmeibt_disk_segment_topo_ctx	*_topo_ctx_ = (seg_topo);										\
		enum NVMEIBT_MEM_TBL_INIT_MODE			_val_ = (in_val);												\
    	if (is_init_mode_irrelevant(__seg_lot_, _val_))															\
			_val_ = NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT;													\
		if (	(_topo_ctx_->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER &&				\
				 _val_ == NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED)) {											\
			_val_ = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER;													\
			N_Tf(name ## _a, "@UUID_8 SET_TXID_INIT_MODE Ignoring FIRST_USE_EVER-->INIT_REQUIRED", nvmeib_uuid_first_4_bytes(_uuid));			\
		}																										\
		if (_topo_ctx_->txid_init_mode != _val_) {																\
			N_Tf(name, "SET_TXID_INIT_MODE(@TOPO_CTX_CHAR-@UUID_8: @DIRTY_BITS_STR->@DIRTY_BITS_STR)",			\
				 topo_char,																						\
				 nvmeib_uuid_first_4_bytes(_uuid),																\
				 mem_tbl_init_mode_str(_topo_ctx_->txid_init_mode),												\
				 mem_tbl_init_mode_str(_val_));																	\
			_topo_ctx_->txid_init_mode = _val_;																	\
		}																										\
	} while (0)

#define NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS_INIT_MODE(name, _uuid, p_seg_lot, seg_topo, in_val, topo_char) do {	\
		struct nvmeibt_seg_lot					*__seg_lot = (p_seg_lot);										\
		struct nvmeibt_disk_segment_topo_ctx	*_topo_ctx = (seg_topo);										\
		enum NVMEIBT_MEM_TBL_INIT_MODE			_val = (in_val);												\
		if (is_init_mode_irrelevant(__seg_lot, _val))															\
			_val = NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT;													\
		if (	(_topo_ctx->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER &&					\
				 _val == NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED)) {											\
			_val = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER;													\
			N_Tf(name ## _b, "@UUID_8 SET_DIRTY_BITS_INIT_MODE Ignoring FIRST_USE_EVER-->INIT_REQUIRED", nvmeib_uuid_first_4_bytes(_uuid));		\
		}																										\
		if (_topo_ctx->dirty_bits_init_mode != _val) {															\
			N_Tf(name, 																							\
				 "SET_DIRTY_BITS_INIT_MODE(@TOPO_CTX_CHAR-@UUID_8: @DIRTY_BITS_STR->@DIRTY_BITS_STR)",			\
				 topo_char,																						\
				 nvmeib_uuid_first_4_bytes(_uuid),																\
				 mem_tbl_init_mode_str(_topo_ctx->dirty_bits_init_mode),										\
                 mem_tbl_init_mode_str(_val));																	\
			_topo_ctx->dirty_bits_init_mode = _val;																\
			if (_val &																							\
				(NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE | NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT)) {			\
				NNVMEIBT_SEG_TOPO_SET_TXID_INIT_MODE(name ## _txid, _uuid, __seg_lot, _topo_ctx,				\
														NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE, topo_char);		\
			}																									\
		}																										\
	} while (0)

#define NNVMEIBT_SEG_TOPO_SET_STALE_LOCKS_INIT_MODE(name, _uuid, p_seg_lot, seg_topo, in_val, topo_char) do {	\
		struct nvmeibt_seg_lot					*__seg_lot = (p_seg_lot);										\
		struct nvmeibt_disk_segment_topo_ctx	*_topo_ctx = (seg_topo);										\
		enum NVMEIBT_MEM_TBL_INIT_MODE			_val = (in_val);												\
		if (is_init_mode_irrelevant(__seg_lot, _val))															\
			_val = NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT;													\
		if (	(_topo_ctx->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER &&					\
				 _val == NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED)) {											\
			_val = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER;													\
			N_Tf(name ## _c, "@UUID_8 SET_STALE_LOCKS_INIT_MODE Ignoring FIRST_USE_EVER-->INIT_REQUIRED", nvmeib_uuid_first_4_bytes(_uuid));	\
		}																										\
		if (_topo_ctx->stale_locks_init_mode != _val) {															\
			N_Tf(name,																							\
				 "SET_STALE_LOCKS_INIT_MODE(@TOPO_CTX_CHAR-@UUID_8: @DIRTY_BITS_STR->@DIRTY_BITS_STR)",			\
				 topo_char,																						\
				 nvmeib_uuid_first_4_bytes(_uuid),																\
				 mem_tbl_init_mode_str((_topo_ctx->stale_locks_init_mode)),										\
				 mem_tbl_init_mode_str(_val));																	\
			_topo_ctx->stale_locks_init_mode = _val;															\
		}																										\
	} while (0)

/**********************************************************************************************/

#define NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(name, p_seg_lot, in_val) do {						\
	struct nvmeibt_seg_lot					*_seg_lot = (p_seg_lot);						\
	char									_topo_char= get_topo_char(_seg_lot);			\
	if (!_seg_lot) {																		\
		N_Wf(name ## _1, "seg_lot=NULL");													\
		break;																				\
	}																						\
	NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS(name, nvmeibt_seg_lot_UUID(_seg_lot),					\
		_seg_lot, &_seg_lot->seg_topo, in_val, _topo_char);									\
} while (0)

#define NNVMEIBT_SEG_LOT_SET_TXID_INIT_MODE(name, p_seg_lot, in_val) do {					\
	struct nvmeibt_seg_lot					*_seg_lot = (p_seg_lot);						\
	char									_topo_char= get_topo_char(_seg_lot);			\
	if (!_seg_lot) {																		\
		N_Wf(name ## _1, "seg_lot=NULL");													\
		break;																				\
	}																						\
	NNVMEIBT_SEG_TOPO_SET_TXID_INIT_MODE(name, nvmeibt_seg_lot_UUID(_seg_lot),				\
		_seg_lot, &_seg_lot->seg_topo, in_val, _topo_char);									\
} while (0)

#define NNVMEIBT_SEG_LOT_SET_DIRTY_BITS_INIT_MODE(name, p_seg_lot, in_val) do {				\
	struct nvmeibt_seg_lot					*_seg_lot = (p_seg_lot);						\
	char									_topo_char= get_topo_char(_seg_lot);			\
	if (!_seg_lot) {																		\
		N_Wf(name ## _1, "seg_lot=NULL");													\
		break;																				\
	}																						\
	NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS_INIT_MODE(name, nvmeibt_seg_lot_UUID(_seg_lot),		\
		_seg_lot, &_seg_lot->seg_topo, in_val, _topo_char);									\
} while (0)

#define NNVMEIBT_SEG_LOT_SET_STALE_LOCKS_INIT_MODE(name, p_seg_lot, in_val) do {			\
	struct nvmeibt_seg_lot					*_seg_lot = (p_seg_lot);						\
	char									_topo_char= get_topo_char(_seg_lot);			\
	if (!_seg_lot) {																		\
		N_Wf(name ## _1, "seg_lot=NULL");													\
		break;																				\
	}																						\
	NNVMEIBT_SEG_TOPO_SET_STALE_LOCKS_INIT_MODE(name, nvmeibt_seg_lot_UUID(_seg_lot),		\
		_seg_lot, &_seg_lot->seg_topo, in_val, _topo_char);									\
} while (0)

/**********************************************************************************************/

#define NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS(name, p_seg_leader, in_val) do {					\
    struct nvmeibt_seg_leader				*_seg_leader = (p_seg_leader);					\
	struct nvmeibt_seg_lot					*_seg_lot;										\
    if (!_seg_leader) {																		\
    	N_Wf(name ## 1, "seg=NULL");														\
    	break;																				\
	}																						\
	_seg_lot = &_seg_leader->baseline_seg_lot;												\
	NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS(name, nvmeibt_seg_lot_UUID(_seg_lot),					\
		_seg_lot, &_seg_leader->remote_seg_topo, in_val, 'R');								\
} while (0)

#define NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS_INIT_MODE(name, p_seg_leader, in_val) do {		\
    struct nvmeibt_seg_leader				*_seg_leader = (p_seg_leader);					\
	struct nvmeibt_seg_lot					*_seg_lot;										\
    if (!_seg_leader) {																		\
    	N_Wf(name ## 1, "seg=NULL");														\
    	break;																				\
	}																						\
	_seg_lot = &_seg_leader->baseline_seg_lot;												\
	NNVMEIBT_SEG_TOPO_SET_DIRTY_BITS_INIT_MODE(name, nvmeibt_seg_lot_UUID(_seg_lot),		\
		_seg_lot, &_seg_leader->remote_seg_topo, in_val, 'R');								\
} while (0)

#define NNVMEIBT_SEG_REMOTE_SET_STALE_LOCKS_INIT_MODE(name, p_seg_leader, in_val) do {		\
    struct nvmeibt_seg_leader				*_seg_leader = (p_seg_leader);					\
	struct nvmeibt_seg_lot					*_seg_lot;										\
    if (!_seg_leader) {																		\
    	N_Wf(name ## 1, "seg=NULL");														\
    	break;																				\
	}																						\
	_seg_lot = &_seg_leader->baseline_seg_lot;												\
	NNVMEIBT_SEG_TOPO_SET_STALE_LOCKS_INIT_MODE(name, nvmeibt_seg_lot_UUID(_seg_lot),		\
		_seg_lot, &_seg_leader->remote_seg_topo, in_val, 'R');								\
} while (0)

#define NNVMEIBT_SEG_REMOTE_SET_TXID_INIT_MODE(name, p_seg_leader, in_val) do {				\
    struct nvmeibt_seg_leader				*_seg_leader = (p_seg_leader);					\
	struct nvmeibt_seg_lot					*_seg_lot;										\
    if (!_seg_leader) {																		\
    	N_Wf(name ## 1, "seg=NULL");														\
    	break;																				\
	}																						\
	_seg_lot = &_seg_leader->baseline_seg_lot;												\
	NNVMEIBT_SEG_TOPO_SET_TXID_INIT_MODE(name, nvmeibt_seg_lot_UUID(_seg_lot),				\
		_seg_lot, &_seg_leader->remote_seg_topo, in_val, 'R');								\
} while (0)

/********* Declarations of the ".c" functions *********/

struct nvmeibt_local_disk *nvmeibt_disk_segment_get_local_disk(struct nvmeibt_disk_segment *disk_segment);
void nvmeibt_seg_lot_set_all_init_modes(struct nvmeibt_seg_lot *seg_lot, enum NVMEIBT_MEM_TBL_INIT_MODE init_mode);
void nvmeibt_disk_segment_mark_is_newly_added_seg_in_all_topos(struct nvmeibt_disk_segment *seg);
void nvmeibt_generic_seg_topo_reset(struct nvmeibt_disk_segment_topo_ctx *seg_topo);
void nvmeibt_seg_remote_reset(struct nvmeibt_disk_segment_topo_ctx *seg_remote_topo, struct nvmeibt_disk_segment *seg);

bool nvmeibt_disk_segment_is_node_leader_valid(struct nvmeibt_seg_lot *seg_lot);
bool nvmeibt_disk_segment_leader_is_init_due_now(struct nvmeibt_seg_lot *seg_lot);
bool nvmeibt_disk_segment_leader_is_waiting_for_dirty_bits_init(const struct nvmeibt_disk_segment *disk_segment);
bool nvmeibt_disk_segment_leader_is_waiting_for_cold_init_done(const struct nvmeibt_disk_segment *disk_segment);
enum nvmeibt_add_rv nvmeibt_disk_segment_add(struct mm_segment_conf *conf,
											 struct mm_praid_conf *praid,
											 struct mm_vol_conf *vol,
											 bool is_updating_leader,
											 struct nvmeibt_disk_segment **seg_out,
											 int config_tag);
void nvmeibt_seg_update_committed_lot_config(struct mm_segment_conf *conf,
											 struct mm_praid_conf *praid_conf,
											 struct mm_vol_conf *vol,
											 struct nvmeibt_praid *praid,
											 struct nvmeibt_disk_segment **seg_out);
void nvmeibt_seg_lot_leader_convert_unusable_to_dead(struct nvmeibt_seg_lot *seg_lot);
void nvmeibt_disk_segment_leader_sync_with_remote_applied(struct nvmeibt_seg_lot *seg_lot);
enum nvmeibt_add_rv nvmeibt_disk_segment_leader_upd_from_peer_applied(struct nvmeibt_serialized_seg_active_topo *seg_topo_ptr,
																	  unsigned long long local_serialization_version,
																	  struct nvmeibt_raft_member *remote_member);
enum nvmeibt_add_rv nvmeibt_seg_follower_upd_committed_seg_topo(struct nvmeibt_serialized_seg_leader_topo *seg_topo_ptr);
struct nvmeibt_disk_segment *nvmeibt_disk_segment_get_disk_segment_by_id(const union nvmeib_uuid *disk_segment_id);

void nvmeibt_disk_segment_garbage_collect_old_segments(bool *is_any_garbage_collected, bool *is_all_garbage_collected);

enum nvmeibt_seg_remove_rv nvmeibt_disk_segment_remove(struct nvmeibt_disk_segment *disk_segment);
void nvmeibt_seg_lot_mark_conf_corrupted(struct nvmeibt_seg_lot *seg_lot);
void nvmeibt_disk_segment_mark_conf_corrupted(struct nvmeibt_disk_segment *seg);
void nvmeibt_disk_segment_trim_specific_seg(struct nvmeibt_disk_segment *seg, uint8_t trim_flag);
void nvmeibt_disk_segment_trim_unused_entries(int config_tag, uint8_t trim_flag);

bool nvmeibt_disk_segment_leader_is_usable(const struct nvmeibt_disk_segment *disk_segment);
bool nvmeibt_disk_segment_leader_is_new_dead(struct nvmeibt_disk_segment *disk_segment);
void nvmeibt_disk_segment_active_mark_reserialization_required(struct nvmeibt_disk_segment *seg);
bool nvmeibt_disk_segment_are_topos_actionably_different(const union nvmeib_uuid *uuid, struct nvmeibt_disk_segment_topo_ctx *new_t, struct nvmeibt_disk_segment_topo_ctx *old_t);
bool nvmeibt_seg_lot_is_owner_of_any_seg_in_baseline_topo(struct nvmeibt_seg_lot *seg_lot);
void nvmeibt_disk_segment_dump(const struct nvmeibt_disk_segment *disk_segment);

/******* Static inline forward declarations ********/

static inline struct nvmeibt_praid *nvmeibt_disk_segment_get_praid(const struct nvmeibt_disk_segment *disk_segment);
static inline const char *nvmeibt_disk_segment_get_praid_type_str(const struct nvmeibt_disk_segment *disk_segment);
static inline struct nvmeibt_praid_topo_ctx *nvmeibt_disk_segment_get_praid_applied_topo(struct nvmeibt_disk_segment *disk_segment);
static inline struct nvmeibt_block_device *nvmeibt_disk_segment_get_blkdev(const struct nvmeibt_disk_segment *disk_segment);
static inline const char *nvmeibt_disk_segment_blkdev_name(const struct nvmeibt_disk_segment *disk_segment);
static inline struct nvmeibt_disk *nvmeibt_disk_segment_get_disk(const struct nvmeibt_disk_segment *disk_segment);
static inline struct nvmeibt_node *nvmeibt_disk_segment_get_node(const struct nvmeibt_disk_segment *disk_segment);
static inline const char *nvmeibt_disk_segment_get_node_name(const struct nvmeibt_disk_segment *disk_segment);

/******* Static inline with no external dependencies ********/

static inline const struct nvmeibt_disk_segment_config *nvmeibt_disk_segment_get_config(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? &(disk_segment->from_config) : NULL);
}

static inline BOOL nvmeibt_disk_segment_is_self_owner(struct nvmeibt_seg_lot *seg_lot)
{
	return (seg_lot && (seg_lot == seg_lot->seg_topo.owner_seg_lot));
}

static inline BOOL nvmeibt_disk_segment_is_X_in_config(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment &&
			(disk_segment->from_config.deprecation_flag == 'X'));
}

static inline BOOL nvmeibt_disk_segment_is_Replaced_in_config(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment && (disk_segment->from_config.deprecation_flag == 'R'));
}

static inline bool nvmeibt_disk_segment_is_missing_in_config(const struct nvmeibt_disk_segment *disk_segment)
{
	return NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(disk_segment);
}

static inline char *nvmeibt_disk_segment_id_str(struct nvmeibt_disk_segment *seg)
{
	return (seg ? seg->seg_mgmt.urn_uuid.str : "");
}

static inline int8_t nvmeibt_disk_segment_idx_in_praid(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? disk_segment->from_config.idx_in_praid : -1);
}

/******* Includes needed for static inline functions ********/

#include "nvmeibt_praid.h"
// #include "nvmeibt_disk.h"

/******* Static inline functions that depend on other functions ******/

static inline struct nvmeibt_praid *nvmeibt_disk_segment_get_praid(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? disk_segment->seg_mgmt.its_praid : NULL);
}

static inline const char *nvmeibt_disk_segment_get_praid_type_str(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? nvmeibt_praid_get_type_str(disk_segment->seg_mgmt.its_praid) : "???");
}

static inline struct nvmeibt_praid_topo_ctx *nvmeibt_disk_segment_get_praid_applied_topo(struct nvmeibt_disk_segment *disk_segment)
{
	return nvmeibt_praid_get_applied_topo(nvmeibt_disk_segment_get_praid(disk_segment));
}

static inline struct nvmeibt_disk *nvmeibt_disk_segment_get_disk(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? disk_segment->seg_mgmt.its_disk : NULL);
}

const char *nvmeibt_disk_get_ldisk_id_str(const struct nvmeibt_disk *disk);
static inline const char *nvmeibt_disk_segment_get_disk_ldisk_id_str(struct nvmeibt_disk_segment *seg)
{
	// Might be used for a non-local seg. We still need its ldisk_id
	return (nvmeibt_disk_get_ldisk_id_str(nvmeibt_disk_segment_get_disk(seg)));
}

static inline bool seg_is_explicitly_deleted_in_config(const struct nvmeibt_disk_segment_config *f)
{
	return (f->deprecation_flag == 'X');
}

static inline bool nvmeibt_seg_lot_is_explicitly_deleted_in_config(const struct nvmeibt_seg_lot *seg_lot)
{
	return seg_is_explicitly_deleted_in_config(&seg_lot->from_config);
}

static inline BOOL nvmeibt_seg_lot_is_deleted_in_config(const struct nvmeibt_seg_lot *seg_lot)
{
	return (seg_lot && (nvmeibt_disk_segment_is_missing_in_config(seg_lot->my_seg) || nvmeibt_seg_lot_is_explicitly_deleted_in_config(seg_lot)));
}

static inline struct nvmeibt_block_device *nvmeibt_disk_segment_get_blkdev(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? nvmeibt_praid_get_blkdev(disk_segment->seg_mgmt.its_praid) : NULL);
}

static inline const char *nvmeibt_disk_segment_blkdev_name(const struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? nvmeibt_praid_get_blkdev_name(disk_segment->seg_mgmt.its_praid) : "");
}

#define NNVMEIBT_DISK_SEGMENT_SET_OWNER(name, seg, topo_ctx, new_owner) do {										\
	(topo_ctx)->owner_seg = (new_owner);																			\
	N_Tf(name, "seg=@UUID_8 SET_OWNER(@TOPO_CTX_CHAR)<--@UUID_8", nvmeibt_seg_UUID_8(seg),							\
		nvmeibt_disk_segment_topo_ctx_char((seg), (topo_ctx)), nvmeibt_seg_UUID_8((topo_ctx)->owner_seg));			\
} while (0)

struct nvmeibt_node *nvmeibt_disk_get_node(const struct nvmeibt_disk *disk);
static inline struct nvmeibt_node *nvmeibt_disk_segment_get_node(const struct nvmeibt_disk_segment *disk_segment)
{
	return nvmeibt_disk_get_node(nvmeibt_disk_segment_get_disk(disk_segment));
}

char *nvmeibt_disk_get_node_name(const struct nvmeibt_disk *disk);
static inline const char *nvmeibt_disk_segment_get_node_name(const struct nvmeibt_disk_segment *disk_segment)
{
	return nvmeibt_disk_get_node_name(nvmeibt_disk_segment_get_disk(disk_segment));
}

static inline bool nvmeibt_is_seg_topos_praid_version_matching(struct nvmeibt_disk_segment_topo_ctx *seg_topo_1, struct nvmeibt_disk_segment_topo_ctx *seg_topo_2)
{
	return ((seg_topo_1->seg_praid_version_major == seg_topo_2->seg_praid_version_major) &&
			(seg_topo_1->seg_praid_version_minor == seg_topo_2->seg_praid_version_minor));
}

static inline bool nvmeibt_seg_lot_is_remote_active_fully_synched(struct nvmeibt_disk_segment_topo_ctx *calculated_topo, struct nvmeibt_disk_segment_topo_ctx *remote_topo)
{
	return (nvmeibt_is_seg_topos_praid_version_matching(calculated_topo, remote_topo) &&
			(!(calculated_topo->is_registrants_synchronizer) || remote_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd));
}

static inline struct nvmeibt_seg_active *nvmeibt_disk_segment_get_seg_active(struct nvmeibt_disk_segment *disk_segment)
{
	return (disk_segment ? disk_segment->seg_follower.seg_active : NULL);
}

static inline bool nvmeibt_disk_segment_is_conf_corrupted(struct nvmeibt_disk_segment *seg)
{
	return (!seg || seg->is_conf_corrupted);
}

static inline bool nvmeibt_disk_segment_is_config_OK(struct nvmeibt_disk_segment *seg)
{
	return (!nvmeibt_disk_segment_is_conf_corrupted(seg) && seg->seg_mgmt.its_disk);
}

static inline bool nvmeibt_seg_lot_is_config_OK(const struct nvmeibt_seg_lot *seg_lot)
{
	return nvmeibt_disk_segment_is_config_OK(seg_lot->my_seg);
}

static inline const union nvmeib_uuid *nvmeibt_seg_UUID(struct nvmeibt_disk_segment *seg)
{
	return (seg ? &seg->from_config.id : &nvmeib_uuid_null_val);
}

static inline unsigned int nvmeibt_seg_UUID_8(struct nvmeibt_disk_segment *seg)
{
	return nvmeib_uuid_first_4_bytes(nvmeibt_seg_UUID(seg));
}

static inline const union nvmeib_uuid *nvmeibt_seg_lot_UUID(struct nvmeibt_seg_lot *seg_lot)
{
	return nvmeibt_seg_UUID(seg_lot ? seg_lot->my_seg : NULL);
}

static inline unsigned int nvmeibt_seg_lot_UUID_8(struct nvmeibt_seg_lot *seg_lot)
{
	return nvmeibt_seg_UUID_8(seg_lot ? seg_lot->my_seg : NULL);
}

static inline bool seg_is_deprecated_in_config(const struct nvmeibt_disk_segment_config *f)
{
	return ((f->deprecation_flag != 'N') && (f->deprecation_flag != 'S'));
}

static inline bool nvmeibt_disk_segment_is_deprecated_in_config(struct nvmeibt_disk_segment *seg)
{
	return seg_is_deprecated_in_config(&seg->from_config);
}

static inline bool nvmeibt_seg_lot_is_deprecated_in_config(struct nvmeibt_seg_lot *seg_lot)
{
	return (seg_lot && seg_is_deprecated_in_config(&seg_lot->from_config));
}

static inline bool seg_is_replaced_in_config(const struct nvmeibt_disk_segment_config *f)
{
	return (f->deprecation_flag == 'R');
}

static inline bool seg_is_substitution_in_config(const struct nvmeibt_disk_segment_config *f)
{
	return (f->deprecation_flag == 'S');
}

static inline bool nvmeibt_seg_lot_is_substitution_in_config(struct nvmeibt_seg_lot *seg_lot)
{
	return seg_is_substitution_in_config(&seg_lot->from_config);
}

static inline bool nvmeibt_seg_lot_is_replaced_in_config(struct nvmeibt_seg_lot *seg_lot)
{
	return seg_is_replaced_in_config(&seg_lot->from_config);
}

static inline bool nvmeibt_seg_lot_is_X_in_config(struct nvmeibt_seg_lot *seg_lot)
{
	return (seg_lot && (seg_lot->from_config.deprecation_flag == 'X'));
}

static inline bool nvmeibt_seg_lot_is_zeroing_explicitly_required_according_to_config(struct nvmeibt_seg_lot *seg_lot)
{
	return nvmeibt_seg_lot_is_deprecated_in_config(seg_lot);
}

static inline enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE nvmeibt_seg_topo_dirty_bits_state(const struct nvmeibt_disk_segment_topo_ctx *seg_topo)
{
	return (seg_topo ? seg_topo->dirty_bits_state : NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
}

static inline const char *nvmeibt_seg_topo_dirty_bits_state_str(struct nvmeibt_disk_segment_topo_ctx *seg_topo)
{
	return dirty_bits_state_str(nvmeibt_seg_topo_dirty_bits_state(seg_topo));
}

static inline bool is_init_mode_irrelevant(struct nvmeibt_seg_lot *seg_lot, enum NVMEIBT_MEM_TBL_INIT_MODE init_mode)
{
	// JBOD lifecycle starts with INIT_MODE_FIRST_USE_EVER, and later switches to INIT_MODE_INIT_IRRELEVANT
	// - INIT_MODE_FIRST_USE_EVER was added here, just to eliminate erroneous TOMAwarns in leader_remove_node_disks_whose_segs_are_not_in_remote_applied
	return ((init_mode != NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER) &&
			seg_lot &&
			(nvmeibt_seg_lot_is_X_in_config(seg_lot) ||
			 nvmeibt_disk_segment_is_x(&seg_lot->seg_topo) ||
			 (seg_lot->praid_lot && !nvmeibt_praid_is_segments_dirty_bit_relevant(seg_lot->praid_lot->my_praid))));
}

static inline struct nvmeibt_disk_segment_topo_ctx *seg_lot_get_remote_seg_topo(struct nvmeibt_seg_lot *seg_lot)
{
	return &(seg_lot->my_seg->seg_leader.remote_seg_topo);
}

static inline struct nvmeibt_seg_lot *seg_lot_get_baseline_seg_lot(struct nvmeibt_seg_lot *seg_lot)
{
	return &(seg_lot->my_seg->seg_leader.baseline_seg_lot);
}

static inline struct nvmeibt_disk_segment_topo_ctx *seg_lot_get_baseline_seg_topo(struct nvmeibt_seg_lot *seg_lot)
{
	return &(seg_lot_get_baseline_seg_lot(seg_lot)->seg_topo);
}

static inline struct nvmeibt_disk *nvmeibt_seg_lot_get_disk(const struct nvmeibt_seg_lot *seg_lot)
{
	return seg_lot->my_seg->seg_mgmt.its_disk;
}

static inline uint64_t num_blksets_in_disk_segment(struct nvmeibt_disk_segment *seg)
{
	//assume #blocks in disk-segment is multiple of NUM_BLKS_IN_BLKSET
	return (seg ? ((seg->seg_mgmt.lb_e - seg->seg_mgmt.lb_s + 1 + (NUM_4KBLKS_IN_BLKSET - 1)) / NUM_4KBLKS_IN_BLKSET) : -1ULL);
}

static inline char *nvmeibt_seg_lot_id_str(struct nvmeibt_seg_lot *seg_lot)
{
	return (seg_lot ? seg_lot->my_seg->seg_mgmt.urn_uuid.str : "");
}

#endif	// #ifdef NVMEIBT_DISK_SEGMENT

