/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_PRAID_BASICS
#define NVMEIBT_PRAID_BASICS

#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_str.h"
#include "nvmeibt_mm_json.h"
#include "../autogen/clnt/nvmeibc_mcs_stub.h"
#include "clnt/nvmeibt_client_protocol.h"

enum PRAID_REGISTRANTS_SYNC_CMD {
	PRAID_REGISTRANTS_SYNC_CMD_UNUSED_0 =				(0x0),
	PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN =				(0x1 << 1),
	PRAID_REGISTRANTS_SYNC_CMD_STABLE_I =				(0x1 << 2),
	PRAID_REGISTRANTS_SYNC_CMD_STABLE =					(0x1 << 3),		// Does not require clients_sync
	PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS =		(0x1 << 6),		// An owner died
	PRAID_REGISTRANTS_SYNC_CMD_DELETE =					(0x1 << 8),		// Permanent delete. survives reboot
	//
	PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I =			(0x1 << 9),		// Init (of back from Dead)
	PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W =			(0x1 << 10),	// Write-only
	PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U =			(0x1 << 11),	// Under recovery
	PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D =			(0x1 << 12),	// Dual-owner (!!! NOT DEGRADED !!!), next is SWITCH_TOPO_N
	PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE =	(0x1 << 13),	// Stable - requires client-sync. blkset_sync_safety=off
	PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE =	(0x1 << 14),	// Stable - requires client-sync. blkset_sync_safety=OK
	//
	PRAID_REGISTRANTS_SYNC_CMD_DEPRECATED_SWITCH_TOPO_DX =			(0x1 << 16),	// Dual-owner, next is SWITCH_TOPO_X
	PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X =			(0x1 << 17),	// A non-owner died/going-down
	//
	PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I =		(0x1 << 20),	// Init
	PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R =		(0x1 << 21),	// Run. No under-recovery. Recovering journal entries all over
};

struct nvmeibt_praid_topo_ctx {
	BOOL									is_activated;			// Calculated in the Leader's logic. The new state DOES NOT depend on global_topo->is_activated
																	// Used in order to optimize:
																	// - The active_seg
																	//   Applied/active could use the (last-valid==global) topo as-is,
																	//   and let the clients fail. Not elegant though.
																	// - report_to_mgmt
																	// - Early stop of leader's calculation
	int										praid_version_major;	// Increased upon every praid change that affects clients
	int										praid_version_minor;	// Increased upon every praid change
	enum PRAID_REGISTRANTS_SYNC_CMD			registrants_sync_cmd;
	int										leader_did_all_segs_sync_registrants;	// Used only ib the leader's context
};

struct nvmeibt_praid_config {
	union nvmeib_uuid			id;
	int							version;
	int 						redundancy;
	lock_server_type_e 			lock_scheme_type;
	BOOL						was_ever_activated;
};

struct nvmeibt_praid_serialized_topo {
	char									eyecatcher[4];							// 4
    int										res_1;									// 8
	union nvmeib_uuid						uuid;									// 24
	int										praid_version_major;					// 28
	int										praid_version_minor;					// 32
	int										leader_did_all_segs_sync_registrants;	// 36
	enum PRAID_REGISTRANTS_SYNC_CMD			registrants_sync_cmd:32;				// 40
    int										res_2;									// 44
    short									res_3;									// 48
	BOOL									is_activated;							// 49
    int8_t									segs_num;								// 50
	struct nvmeibt_serialized_seg_leader_topo		segs[0] __attribute__((aligned(8)));	// 56
} __attribute__((packed, aligned(8)));

#define NVMEIBT_PRAID_TOPO_DUMP(name, _uuid, _which_str, _topo) do {					\
	if (_topo) {																		\
		N_Tf(name, "praid=@UUID_LE @STR "												\
			"sync_cmd=@STR(are_synced=@X) "												\
			"is_activated=@BOOL "														\
			"praid_version=@X:@X",														\
			_uuid, _which_str,															\
			praid_registrants_sync_cmd_str(_topo->registrants_sync_cmd),				\
			_topo->leader_did_all_segs_sync_registrants,								\
			_topo->is_activated,														\
			_topo->praid_version_major, _topo->praid_version_minor);					\
	} 																					\
} while (0)

/******* Declarations of the ".c" functions ********/

const char *praid_registrants_sync_cmd_str(enum PRAID_REGISTRANTS_SYNC_CMD c);
enum NVMEIBT_PRAID_TYPE nvmeibt_praid_type_str_to_type(const char *type_str);
char *nvmeibt_praid_type_to_str(const enum NVMEIBT_PRAID_TYPE praid_type);

/******* Static inline forward declarations ********/


/******* Static inline with no external dependencies ********/

static inline BOOL nvmeibt_praid_topo_is_client_sync_cmd_io_able(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_STABLE | PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D |
					 PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE | PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X));
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_req_client_ack(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_DELETE |
					 PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D |
					 PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE | PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U | PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R |
					 PRAID_REGISTRANTS_SYNC_CMD_STABLE |
					 PRAID_REGISTRANTS_SYNC_CMD_STABLE_I | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I |
					 PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I));
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_stable(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & PRAID_REGISTRANTS_SYNC_CMD_STABLE);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_switch_topo(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D | PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE | PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE |
					 PRAID_REGISTRANTS_SYNC_CMD_DEPRECATED_SWITCH_TOPO_DX | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X)	);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_switch_topo_I(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_switch_topo_X(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_reset_registrants(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_switch_topo_under_recovery(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I |
					 PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U |
					 PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I | PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R)	);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_capable_to_INIT_TURN_OFF_on_owners(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	// Those sync_cmd are the only ones that for an owner seg might TURN_ALL_OFF
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_STABLE_I | PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I)	);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_initializing(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_STABLE_I | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I | PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I)	);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_req_unregister(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_DELETE |
					 PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS |
					 PRAID_REGISTRANTS_SYNC_CMD_STABLE_I | PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I)	);
}

static inline BOOL nvmeibt_praid_is_sync_cmd_run_dirty_rebuild(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_unknown(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN);
}

static inline BOOL nvmeibt_praid_is_client_sync_cmd_delete(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_DELETE)	);
}

static inline BOOL nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery_r(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R);
}

static inline BOOL nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return !!(cmd & (PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I | PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R));
}

static inline enum NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING nvmeibt_praid_client_sync_blkset_sync_safety(enum PRAID_REGISTRANTS_SYNC_CMD cmd)
{
	return (cmd & (PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D | PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE) ?
			 NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_YES : NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO);
}

static inline unsigned long long praid_versions_to_last_log_index(int major, int minor)
{
	union last_LOG_index_union_praid_version	ret;
	ret.praid_version.major = major;
	ret.praid_version.minor = minor;
	return ret.ll;
}

static inline unsigned long long praid_topo_to_last_log_index(struct nvmeibt_praid_topo_ctx *praid_topo)
{
	return praid_versions_to_last_log_index(praid_topo->praid_version_major, praid_topo->praid_version_minor);
}

static inline enum PRAID_REGISTRANTS_SYNC_CMD nvmeibt_praid_topo_get_registrants_sync_cmd(const struct nvmeibt_praid_topo_ctx *praid_topo)
{
	return (praid_topo ? praid_topo->registrants_sync_cmd : PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN);
}

static inline const char *nvmeibt_praid_topo_get_registrants_sync_cmd_str(const struct nvmeibt_praid_topo_ctx *praid_topo)
{
	return praid_registrants_sync_cmd_str(nvmeibt_praid_topo_get_registrants_sync_cmd(praid_topo));
}

#endif	// #ifndef NVMEIBT_PRAID_BASICS

/******* Includes needed for static inline functions ********/

/******* Static inline functions that depend on other functions ******/


