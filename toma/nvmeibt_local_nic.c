/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_common.h"
#include "nvmeibt_global.h"
#include "nvmeibt_read_config.h"
#include "nvmeibt_local_nic.h"

int nvmeibt_ib_common_device_uuid_str_to_raw(
	union ibv_gid *ibv_gid, const char *device_uuid_str);

void nvmeibt_local_nic_dump(struct nvmeibt_local_nic *local_nic)
{
	const struct nvmeibt_local_nic_config *f = &local_nic->from_config;
	N_Tf(trace_local_nic_nvmeibt_local_nic_dump, "Config data: device_type=@STR hw_gid_str=@GID_STR port=@PORT pkey=@STR link=@STR state=@STR mtu=@MTU max_mtu=@MTU sw_gid_str=@GID_STR",
		f->device_type, f->hw_gid_uuid.str, f->port, f->pkey, f->link, f->state, f->mtu, f->max_mtu, f->sw_gid_uuid.str);
}

const struct nvmeibt_ascii_uuid *nvmeibt_local_nic_UUID(const struct nvmeibt_local_nic *local_nic)
{
	return &local_nic->from_config.sw_gid_uuid;
}

enum nvmeibt_add_rv nvmeibt_local_nic_add(char *config_str, int config_tag)
{
	enum nvmeibt_add_rv					rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_local_nic			*new_local_nic = NULL, *local_nic = NULL;
	struct nvmeibt_local_nic_config		*f = NULL;
	int									r;
	union ibv_gid						uuid_just_for_validation;

	NFIN;
	new_local_nic = NNVMEIBT_TOMA_CALLOC(trace_local_nic_nvmeibt_local_nic_add, 1, sizeof(*new_local_nic));
	// NVMEIBS_NICS_CSV_HEADER
	// mlx4_0,0xfe80000000000000f4521403007984e1,1,0xffff,I,ACTIVE,4096,4096,0xfe80000000000000f4521403007984e1
	f = &(new_local_nic->from_config);
	r = nvmeibt_sscanf_csv_line(config_str,
								's', sizeof(f->device_type), &(f->device_type),
								'0', &(f->hw_gid_uuid.str),
								'd', &(f->port),
								's', sizeof(f->pkey), &(f->pkey),
								's', sizeof(f->link), &(f->link),
								's', sizeof(f->state), &(f->state),
								'd', &(f->mtu),
								'd', &(f->max_mtu),
								'd', &(f->gid_index),
								'b', &(f->roce_v2),
								'b', &(f->roce_ipv6),
								'b', &(f->used),
								's', sizeof(f->device_network_name), &(f->device_network_name),
								'0', &(f->sw_gid_uuid.str),
								'\0');
	if (r <= 0) {
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}

	if (nvmeibt_ib_common_device_uuid_str_to_raw(&uuid_just_for_validation, f->hw_gid_uuid.str)) {
		N_Ef(hru8720, "Illegal nic uuid=@STR, ignoring", f->hw_gid_uuid.str);
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}
	rv = NNVMEIBT_HASH_ADD_OBJ_ASCII_new(ji987ys,
					nvmeibt_global_get_global()->local_nics_hash_by_sw_gid_str,
					new_local_nic,
					config_tag,
					NVMEIBT_MAX_N_NICS_PER_NODE, local_nic, local_nic);

out:
	if (rv != NVMEIBT_ADD_NEW) {
		N_Tf(njko098, "free unused new local nic: @STR", nvmeibt_local_nic_UUID(new_local_nic)->str);
		NNVMEIBT_TOMA_FREE(ddt654w, new_local_nic);
	}
	NFOUT;
	return rv;
}
static void __local_nic_remove(struct nvmeibt_local_nic *local_nic)
{
	NNVMEIBT_HASH_DEL_OBJ_ASCII_new(ajji8e3, nvmeibt_global_get_global()->local_nics_hash_by_sw_gid_str, local_nic, local_nic);
	NNVMEIBT_TOMA_FREE(ddii98w, local_nic);
}

TODO(local_nic does not have a add/remove event like local_disk, hence we have a trim and NVMEIBT_OBJ_MARK_OUTDATED_ASCII());

void nvmeibt_local_nic_trim_unused_entries(int config_tag)
{
	struct nvmeibt_local_nic	*local_nic;
	NFIN;
	NVMEIB_HASH_FOREACH(local_nic, nvmeibt_global_get_global()->local_nics_hash_by_sw_gid_str) {
		if (NVMEIBT_OBJ_IS_OLDER(local_nic, config_tag)) {
			N_Tf(inn87x, "Removing local nic: @STR with config tag @INT<@INT",
				 nvmeibt_local_nic_UUID(local_nic)->str, local_nic->config_tag, config_tag);
			NVMEIBT_OBJ_MARK_OUTDATED_ASCII(fjuu873, local_nic, local_nic);
			__local_nic_remove(local_nic);
		}
	}
	NFOUT;
}

void nvmeibt_local_nic_free_all_at_exit(void)
{
	struct nvmeibt_local_nic	*local_nic;
	NVMEIB_HASH_FOREACH(local_nic, nvmeibt_global_get_global()->local_nics_hash_by_sw_gid_str) {
		NNVMEIBT_TOMA_FREE(vgs8k30, local_nic);
	}
}
