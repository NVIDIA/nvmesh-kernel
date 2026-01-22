/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_common.h"
#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_praid_basics.h"
#include "nvmeibt_mm_json.h"

const char *praid_registrants_sync_cmd_str(enum PRAID_REGISTRANTS_SYNC_CMD c)
{
	static char	unknown_val[sizeof(int) * 2 + 1];
	switch (c) {
	case PRAID_REGISTRANTS_SYNC_CMD_UNUSED_0:				return "UNUSED";
	case PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN:				return "UNKNOWN";
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE_I:				return "STABLE_I";
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE:					return "STABLE";
	case PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS:		return "RESET_REGISTRANTS";
	case PRAID_REGISTRANTS_SYNC_CMD_DELETE:					return "DELETE";
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I:			return "SWITCH_TOPO_I";
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W:			return "SWITCH_TOPO_W";
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U:			return "SWITCH_TOPO_U";
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D:			return "SWITCH_TOPO_D";
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE:	return "SW_TOPO_STABLE_UNSAFE";
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE:	return "SW_TOPO_STABLE_SAFE";
	case PRAID_REGISTRANTS_SYNC_CMD_DEPRECATED_SWITCH_TOPO_DX:			return "DEPRECATED_SWITCH_TOPO_DX";
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X:			return "SWITCH_TOPO_X";
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I:		return "EC_COLD_RECOVERY_I";
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R:		return "EC_COLD_RECOVERY_R";
	default: {
		snprintf(unknown_val, sizeof(unknown_val), "%x", c);
		return unknown_val;
		}
	}
}

enum NVMEIBT_PRAID_TYPE nvmeibt_praid_type_str_to_type(const char *type_str)
{
	if (!strcmp(type_str, "JB"))		return NVMEIBT_PRAID_TYPE_JBOD;
	if (!strcmp(type_str, "R1"))		return NVMEIBT_PRAID_TYPE_RAID1;
	if (!strcmp(type_str, "R5"))		return NVMEIBT_PRAID_TYPE_RAID5;
	if (!strcmp(type_str, "R6"))		return NVMEIBT_PRAID_TYPE_RAID6;
	if (!strcmp(type_str, "5DP"))		return NVMEIBT_PRAID_TYPE_RAID5DP;
	return NVMEIBT_PRAID_TYPE_UNKNOWN;
}

char *nvmeibt_praid_type_to_str(const enum NVMEIBT_PRAID_TYPE praid_type)
{
	switch (praid_type) {
	case	NVMEIBT_PRAID_TYPE_JBOD:		return "JB";
	case	NVMEIBT_PRAID_TYPE_RAID1:		return "R1";
	case	NVMEIBT_PRAID_TYPE_RAID5:		return "R5";
	case	NVMEIBT_PRAID_TYPE_RAID6:		return "R6";
	case	NVMEIBT_PRAID_TYPE_RAID5DP:		return "5DP";
	default:								return "UNKNOWN";
	}
}

