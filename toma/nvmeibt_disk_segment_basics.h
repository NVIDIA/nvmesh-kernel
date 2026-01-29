/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_DISK_SEGMENT_BASICS
#define NVMEIBT_DISK_SEGMENT_BASICS

#include "nvmeibt_common.h"
#include "nvmeibt_mm_json.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_str.h"

/****************************  MR Rename later on  ****************************/
// TODO(Rename)
#define nvmeibt_seg_topo_is_x(seg_topo) nvmeibt_disk_segment_is_x(seg_topo)
/************************  End og MR Rename later on  *************************/

enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE {
	NVMEIBT_SEG_DIRTY_BITS_STATE_UNUSED_0 = (0x0),							//
	NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN = (0x1 << 0),						// 0x1   Leader: Never saved to persistency
	NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE = (0x1 << 1),				// 0x2   TOMA: Exists, following surprise shutdown
	NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE = (0x1 << 2),					// 0x4   TOMA: Exists, after clean shutdown - revived as STABLE only if reviving the praid after a crash
	NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD = (0x1 << 5),							// 0x20  Leader:
	NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO = (0x1 << 6),						// 0x40  Leader: start segment deletion - applied will stop registration
	NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE = (0x1 << 7),						// 0x80  Applied: report x_done, after done zeroing - leader permanently removed & dead.
	NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I = (0x1 << 10),			// 0x400 Leader: InActive. Needs to UNREG all and then init
	NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_R = (0x1 << 11),			// 0x800 Leader: Active (locked & written to), but not read from. Client recovery can run
	NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER = (0x1 << 12),				// 0x1000  Leader: owner is recovering an under_recovery.
	// Leader: (note the segment-version-change, owner needs to recover)
	NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE = (0x1 << 13),		// 0x2000  TOMA: Finished the recovery tasks
	NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE = (0x1 << 14),					// 0x4000  Leader:
	NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERED = (0x1 << 15),				// 0x8000  Leader:
	NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER = (0x1 << 18),			// 0x40000 Leader:
	NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE = (0x1 << 19),		// 0x80000 TOMA:
};

enum NVMEIBT_MEM_TBL_INIT_MODE {
	NVMEIBT_MEM_TBL_INIT_MODE_UNUSED_0 = (0x0),								// Invalid value, initialize and should be overriden by calculations to valid value
	NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN = (0x1 << 0),								// Invalid value, initialize and should be overriden by calculations to valid value
	NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED = (0x1 << 1),					// Same as above TODO(remove)
	NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE = (0x1 << 2),						// Required initialization was successfully done
	NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT = (0x1 << 3),					// Fallback to INIT_DONE
	NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON = (0x1 << 4),						// Mark worst case situation
		// Dbits: Mark worst case according to topology: For each degraded seg: If dbits are written in metadata on disk - mark unknowns, If new seg - mark convicts, else mark dbit
		// Locks: EC - not applicable, R1 - mark stale special lock
		// TxID : EC - mark unknown (lazy read), R1 - not applicable
	NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF = (0x1 << 5),					// cleanup memory of recoveree seg, before recovery starts, and possibly cold recovery
		// Dbits: EC/R1 - Mark 0									, no real meaning of Dbit value because it will be overriden by the recovery process
		// Locks: EC/R1 - Mark 0 (unlocked)
		// TxID : EC - mark unknown (lazy read), R1 - not applicable, no real meaning of TxID value because it will be overriden by the recovery process
	NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER = (0x1 << 6),					//  On first activation of praid
		// Dbits: Same as TURN_OFF
		// Locks: Same as TURN_OFF
		// TXID : EC - Special initial value (journal is irrelevant for blockset), R1 - not applicable
	NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST = (0x1 << 8),	// If failing to read from persist, then TURN_ALL_ON.
															// This init mode is not needed if a follower do it automatically in ALIVE_STABLE
};

/*********************   seg_active Serialization   ***************************/

struct nvmeibt_active_seg_flags{
	unsigned int are_praid_registrants_aligned_with_sync_cmd : 1;	// 0,	Did ram survive from revious topo change. I.e. Can client trust the blockset values in ram (lock values,txid,dbits)
	unsigned int did_any_client_report_about_problems : 1;			// 1,	Replacement for an evicted segment
	unsigned int is_drive_write_error : 1;							// 2,	Reflects the error as reported from the drive
	unsigned int reserved : 29;										// 3-31,
} ;

struct nvmeibt_serialized_seg_active_topo {
	char									eyecatcher[4];
	int										res_1;
	union nvmeib_uuid						uuid;
    unsigned long long						active_seg_ser_ver;
	int										active_praid_version_major;
	int										active_praid_version_minor;
	union {
		struct nvmeibt_active_seg_flags		active_seg_flags;
		int									active_seg_flags_int;
	};
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	dirty_bits_state;
	enum NVMEIBT_MEM_TBL_INIT_MODE			dirty_bits_init_mode;
	enum NVMEIBT_MEM_TBL_INIT_MODE			stale_locks_init_mode;
    short									res_2;
    short									res_3;
} __attribute__((packed));

#define NVMEIBT_DISK_SEGMENT_DUMP_ACTIVE_TOPO(name, _topo) N_Tf(name, 													\
	"\n@STR seg=@UUID_8 active_praid_ver=@X.@X ser_ver=@X flags=@X "													\
	"dirty_state=@STR dirty_init=@STR stale_init=@STR",																	\
	(_topo).eyecatcher, nvmeib_uuid_first_4_bytes(&((_topo).uuid)),														\
	(_topo).active_praid_version_major, (_topo).active_praid_version_minor,												\
	(_topo).active_seg_ser_ver, *(int *)&((_topo).active_seg_flags), dirty_bits_state_str((_topo).dirty_bits_state),	\
	mem_tbl_init_mode_str((_topo).dirty_bits_init_mode), mem_tbl_init_mode_str((_topo).stale_locks_init_mode))

/***********************   Leader's Serialization   ***************************/

struct nvmeibt_leader_seg_flags {
	unsigned int has_ram_survived : 1;					// 0, 	Did ram survive from revious topo change. I.e. Can client trust the blockset values in ram (lock values,txid,dbits)
	unsigned int is_newly_added_seg : 1;				// 1,	Replacement for an evicted segment
	unsigned int is_owner_ram_recoverable_stable : 1;	// 2,	Will INIT_FROM_PERSIST recover the RAM properly
	unsigned int is_drive_write_error : 1;				// 3,	Reflects the error as reported from the drive
	unsigned int reserved : 28;							// 4-31,
};

struct nvmeibt_serialized_seg_leader_topo {
	char									eyecatcher[4];
	int			 							res_1;
	union nvmeib_uuid						uuid;
	unsigned long long						res_2;
	//union nvmeib_uuid						owner_seg_uuid;
	//union nvmeib_uuid						secondary_owner_seg_uuid;
	int										praid_version_major;
	int										praid_version_minor;
    // int										active_seg_ser_ver;	// Removed in disk_segment.h
	union {
		struct nvmeibt_leader_seg_flags		leader_seg_flags;
		int									leader_seg_flags_int;	
	};
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	dirty_bits_state:32;
	enum NVMEIBT_MEM_TBL_INIT_MODE			dirty_bits_init_mode:32;
	enum NVMEIBT_MEM_TBL_INIT_MODE			stale_locks_init_mode:32;
	enum NVMEIBT_MEM_TBL_INIT_MODE			txid_init_mode:32;
	int8_t									seg_idx;
	int8_t									owner_idx;
	int8_t									secondary_owner_idx;
	BOOL									is_registrants_synchronizer;
} __attribute__((packed));

const char *dirty_bits_state_str(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE s);
const char *mem_tbl_init_mode_str(enum NVMEIBT_MEM_TBL_INIT_MODE m);

/*******************    ?    **********************/

struct nvmeibt_seg_lot;

struct nvmeibt_disk_segment_config {
	union nvmeib_uuid			id;
	int							version;
	int8_t						idx_in_praid;
	char						deprecation_flag;
};

struct nvmeibt_disk_segment_topo_ctx {
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	dirty_bits_state;
	// unsigned long long						active_seg_ser_ver;	// Moved to seg_active
	int										seg_praid_version_major;
	int										seg_praid_version_minor;
	struct nvmeibt_seg_lot					*owner_seg_lot;
	struct nvmeibt_seg_lot					*secondary_owner_seg_lot;
	enum NVMEIBT_MEM_TBL_INIT_MODE			dirty_bits_init_mode;
	enum NVMEIBT_MEM_TBL_INIT_MODE			stale_locks_init_mode;
	enum NVMEIBT_MEM_TBL_INIT_MODE			txid_init_mode;	// Just a val. Its state is piggibacked on dirty_bits_init_mode
	struct nvmeibt_active_seg_flags			active_seg_flags;
	struct nvmeibt_leader_seg_flags			leader_seg_flags;
	unsigned long long						active_seg_ser_ver;	// The follower increases on every serialization
	BOOL									is_registrants_synchronizer;
};

/******* Declarations of the ".c" functions ********/

/******* Static inline forward declarations ********/


/******* Static inline with no external dependencies ********/

static inline bool nvmeibt_disk_segment_is_de_facto_owner(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != ((topo_ctx->dirty_bits_state) &
			(NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE | NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE |
			 NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER |
			 NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER | NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE)));
}

static inline bool nvmeibt_disk_segment_is_owner_recovered(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERED));
}

static inline bool nvmeibt_disk_segment_is_competent_owner(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	// A competent owner has the correct data & dirty bits.
	// Stale-bits are there too, but some are still in the active-table, so they
	//  will be in-place only after all the clients are disconnected
	// A competent owner will become a de_facto_owner after the transfer the ownership
	return (nvmeibt_disk_segment_is_de_facto_owner(topo_ctx) || nvmeibt_disk_segment_is_owner_recovered(topo_ctx));
}

static inline BOOL nvmeibt_disk_segment_is_any_hot_recoverer(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != ((topo_ctx->dirty_bits_state) &
			(NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER | NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE)));
}

static inline bool nvmeibt_disk_segment_is_reported_as_under_recovery(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != ((topo_ctx->dirty_bits_state) &
			(NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERED |
			 NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I | NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_R)));
}

static inline bool nvmeibt_disk_segment_is_owner_recoverer(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER));
}

static inline bool nvmeibt_disk_segment_is_owner_recoverer_done(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE));
}

static inline bool nvmeibt_disk_segment_is_ec_cold_recoverer(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER));
}

static inline bool nvmeibt_disk_segment_is_ec_cold_recoverer_done(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE));
}

static inline bool nvmeibt_disk_segment_is_any_ec_cold_recoverer(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER | NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE)));
}

static inline bool nvmeibt_disk_segment_is_any_recoverer(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER | NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE|
												NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER | NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE)));
}

static inline bool nvmeibt_disk_segment_is_under_recovery_I(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I)));
}

static inline bool nvmeibt_disk_segment_is_under_recovery_R(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_R)));
}

static inline bool nvmeibt_disk_segment_is_alive_unstable(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE)));
}

static inline bool nvmeibt_disk_segment_is_alive_stable(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE)));
}

static inline BOOL nvmeibt_disk_segment_is_calculated_dead(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state &
				  (NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD |
				   NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO |
				   NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE)));
}

static inline bool nvmeibt_disk_segment_leader_is_state_progressible(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 == (topo_ctx->dirty_bits_state &
								  (NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN |
								   NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD |
								   NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE)));
}

static inline bool nvmeibt_disk_segment_is_dirty_bits_state_unknown(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dirty_bits_state)
{
	return (dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
}

static inline bool nvmeibt_disk_segment_is_x_done(struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE)));
}

static inline bool nvmeibt_disk_segment_is_x_zero(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO)));
}

static inline bool nvmeibt_disk_segment_is_x(struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return (topo_ctx &&
			0 != (topo_ctx->dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO |
												NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE)));
}

static inline bool nvmeibt_disk_segment_is_dirty_bits_state_down(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dirty_bits_state)
{
	return ((dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_UNUSED_0) ||
			(dirty_bits_state & (NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN | NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD)));
}

static inline bool nvmeibt_disk_segment_is_dirty_bits_state_calculated(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dirty_bits_state)
{
	return (0 != (dirty_bits_state) &&
			(0 == (dirty_bits_state &
				   (NVMEIBT_SEG_DIRTY_BITS_STATE_UNUSED_0 | NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN |
					NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE | NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE))));
}

static inline bool nvmeibt_disk_segment_is_dirty_bits_state_registrable(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dirty_bits_state)
{
	return (0 != (dirty_bits_state) &&
			(0 == (dirty_bits_state &
				   (NVMEIBT_SEG_DIRTY_BITS_STATE_UNUSED_0 | NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN |
					NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE | NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE |
					NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD | NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO |
					NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE | NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I))));
}

static inline bool nvmeibt_disk_segment_is_in_active_life_cycle(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dirty_bits_state)
{
	return (0 != (dirty_bits_state) &&
			(0 == (dirty_bits_state &	// None of the below
				   (NVMEIBT_SEG_DIRTY_BITS_STATE_UNUSED_0 | NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN |
					NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE | NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE |
					NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD | NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE))));
}

static inline bool nvmeibt_disk_segment_is_mem_tbl_init_FIRST_USE_EVER(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	return !!((topo_ctx->dirty_bits_init_mode | topo_ctx->stale_locks_init_mode | topo_ctx->txid_init_mode ) & NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER);
}

static inline bool nvmeibt_disk_segment_is_mem_tbl_init_beyond_FIRST_USE_EVER(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	uint32_t	init_mode = (topo_ctx ? (topo_ctx->dirty_bits_init_mode | topo_ctx->stale_locks_init_mode | topo_ctx->txid_init_mode) : 0);
	return !!(init_mode & ~(NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER | NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN | NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED));
}

static inline bool nvmeibt_disk_segment_leader_is_remote_active_mem_tbl_init_command(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	unsigned int	actionable = (NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON |
								  NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF |
								  NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST |
								  NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER);
	return !!((topo_ctx->dirty_bits_init_mode | topo_ctx->stale_locks_init_mode | topo_ctx->txid_init_mode ) &
			  actionable);
}

static inline bool nvmeibt_seg_active_is_mem_tbl_init_meaningful(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	unsigned int	actionable = (NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON |
								  NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF |
								  NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER |
								  NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST |
								  NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
	return !!((topo_ctx->dirty_bits_init_mode | topo_ctx->stale_locks_init_mode | topo_ctx->txid_init_mode ) &
			  actionable);
}

static inline bool nvmeibt_disk_segment_is_mem_tbl_init_done_fully(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx)
{
	const unsigned int done = (NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE | NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
	return (topo_ctx &&
			(topo_ctx->dirty_bits_init_mode &  done) &&
			(topo_ctx->stale_locks_init_mode & done) &&
			(topo_ctx->txid_init_mode &		   done));
}

static inline bool nvmeibt_disk_segment_is_init_mode_turning_off(const struct nvmeibt_disk_segment_topo_ctx *topo_ctx, BOOL is_txid_relevant)
{
	// Explicitly stating which INIT_MODEs are OK, so newly added INIT_MODEs
	//  modes will fail this test
	unsigned int preserving_mask = (NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE |
									NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT |
									NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON);
	return (topo_ctx &&
			( !(topo_ctx->dirty_bits_init_mode  & preserving_mask) ||
			  !(topo_ctx->stale_locks_init_mode & preserving_mask) ||
			  (!(topo_ctx->txid_init_mode		   & preserving_mask) && is_txid_relevant)));
}

#define NVMEIBT_SEG_TOPO_SET_OWNER(name, uuid_8, seg_topo, __owner_seg_lot) do {												\
	(seg_topo)->owner_seg_lot = (__owner_seg_lot);																				\
	N_Tf(name, "SET_OWNER(seg=@UUID_8)<--@UUID_8", (uuid_8), nvmeibt_seg_lot_UUID_8(__owner_seg_lot));	\
} while (0)

#define NNVMEIBT_SEG_LOT_SET_OWNER(name, _seg_lot, _owner_seg_lot) \
	 NVMEIBT_SEG_TOPO_SET_OWNER(name, nvmeibt_seg_lot_UUID_8(_seg_lot), &(_seg_lot->seg_topo), _owner_seg_lot)

static inline bool nvmeibt_seg_topo_is_newly_added(const struct nvmeibt_disk_segment_topo_ctx *seg_topo)
{
	return (bool)(seg_topo->leader_seg_flags.is_newly_added_seg);
}

/******* Includes needed for static inline functions ********/

/******* Static inline functions that depend on other functions ******/

#endif	// #ifndef NVMEIBT_DISK_SEGMENT_BASICS
