#include "nvmeibt_common.h"
#include "nvmeibt_global.h"
#include "nvmeibt_read_config.h"
#include "nvmeibt_local_nic.h"

void nvmeibt_local_nic_dump(struct nvmeibt_local_nic *local_nic)
{
	const struct nvmeibt_local_nic_config *f = &local_nic->from_config;
	N_Tf(trace_local_nic_nvmeibt_local_nic_dump, "Config data: device_type=@STR hw_gid_str=@GID_STR port=@PORT pkey=@STR link=@STR state=@STR mtu=@MTU max_mtu=@MTU sw_gid_str=@GID_STR",
		f->device_type, f->hw_gid_uuid.str, f->port, f->pkey, f->link, f->state, f->mtu, f->max_mtu, f->sw_gid_uuid.str);
}

const union nvmeib_uuid *nvmeibt_local_nic_UUID(const struct nvmeibt_local_nic *local_nic)
{
	return &local_nic->from_config.id;
}

enum nvmeibt_add_rv nvmeibt_local_nic_add(char *config_str, int config_tag)
{
	enum nvmeibt_add_rv					rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_local_nic			*new_local_nic = NULL, *local_nic = NULL;
	struct nvmeibt_local_nic_config		*f = NULL;
	int									r;

	NFIN;
	new_local_nic = NNVMEIBT_TOMA_CALLOC(trace_local_nic_nvmeibt_local_nic_add, 1, sizeof(*new_local_nic));
	XDLIST_INIT_LINK(&new_local_nic->topo_link, NULL);

	// device,hw_gid,port,pkey,transport,state,mtu,max_mtu,gid_index,roce_v2,roce_ipv6,used,ndev_name,sw_gid
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

	if (nvmeibt_urn_uuid_to_union_uuid(&f->id, &f->hw_gid_uuid)) {
		N_Ef(hru8720, "Illegal nic uuid=@STR, ignoring", f->hw_gid_uuid.str);
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}
	rv = NNVMEIBT_HASH_ADD_OBJ(ji987ys,
					&nvmeibt_global_get_global()->local_nics_hash,
					new_local_nic,
					config_tag,
					NVMEIBT_MAX_N_NICS_PER_NODE, local_nic, local_nic);

out:
	if (rv != NVMEIBT_ADD_NEW) {
		N_Tf(njko098, "free unused new local nic: @UUID_LE", nvmeibt_local_nic_UUID(new_local_nic));
		NNVMEIBT_TOMA_FREE(ddt654w, new_local_nic);
	}
	NFOUT;
	return rv;
}
static void __local_nic_remove(struct nvmeibt_local_nic *local_nic)
{
	NNVMEIBT_HASH_DEL_OBJ(ajji8e3, &nvmeibt_global_get_global()->local_nics_hash, local_nic, local_nic);
	NNVMEIBT_TOMA_FREE(ddii98w, local_nic);
}
void nvmeibt_local_nic_trim_unused_entries(int config_tag)
{
	struct nvmeibt_local_nic	*local_nic;
	NFIN;
	XHASHTABLE_FOR_EACH_SAFE(local_nic, &nvmeibt_global_get_global()->local_nics_hash) {
		if (NVMEIBT_HASH_IS_OLDER_OBJ(local_nic, config_tag)) {
			N_Tf(inn87x, "Removing local nic: @UUID_LE with config tag @INT<@INT",
				nvmeibt_local_nic_UUID(local_nic), local_nic->config_tag, config_tag);
			NVMEIBT_HASH_MARK_OBJ_OUTDATED(fjuu873, local_nic, local_nic);
			__local_nic_remove(local_nic);
		}
	}
	NFOUT;
}

struct nvmeibt_local_nic * nvmeibt_local_nic_nic_to_local_nic(struct nvmeibt_nic *nic) {
	struct nvmeibt_local_nic *check;
	XHASHTABLE_FOR_EACH_SAFE(check, &nvmeibt_global_get_global()->local_nics_hash) {
		if (!strncmp(nic->from_config.guid_str, check->from_config.sw_gid_uuid.str, URN_UUID_STR_LENGTH)) {
			return check;
		}
	}
	return NULL;
}

void nvmeibt_local_nic_free_all_at_exit(void)
{
	struct nvmeibt_local_nic	*local_nic;
	XHASHTABLE_FOR_EACH_SAFE(local_nic, &nvmeibt_global_get_global()->local_nics_hash) {
		__local_nic_remove(local_nic);
	}
}
