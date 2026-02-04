#include "nvmeibt_toma.h"
#include <ctype.h>

#include "nvmeibt_common.h"
#include "nvmeibt_nic.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_node.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "interfaces/network/network_incs.h"

const union nvmeib_uuid *nvmeibt_nic_UUID(const struct nvmeibt_nic *nic)
{
	return (nic ? &nic->from_config.id : &nvmeib_uuid_null_val);
}

void nvmeibt_nic_dump(__attribute__((__unused__)) const struct nvmeibt_nic *nic)
{
#ifdef TOMA_DEBUG
	const struct nvmeibt_nic_config *f = &nic->from_config;
	N_Tf(__AUTOID__, "Config data: uuid=@UUID_LE version=@VERSION its_node_uuid=@UUID_LE hw_gid_str=@STR sw_gid_str=@STR partition_key=@INT protocol=@STR",
		nvmeibt_nic_UUID(nic), f->version, &(f->its_node_id), f->hw_gid_str, f->sw_gid_str, f->partition_key, f->protocol);
#endif
}

static void nic_remove(struct nvmeibt_nic *nic)
{

	NFIN;

	if (nic == NULL) {
		goto out;
	}

	N_Tf(djuiry5, "Removing nic=@UUID_LE", nvmeibt_nic_UUID(nic));

	NNVMEIBT_HASH_DEL_OBJ_new(yaioqle, nvmeibt_global_get_global()->nics_hash_by_uuid, nic, nic);
	NNVMEIBT_TOMA_FREE(gjiuy86, nic);

out:
	NFOUT;
}

enum nvmeibt_add_rv nvmeibt_nic_add(struct mm_nic_conf *conf, struct mm_node_conf *node, int config_tag)
{
	enum nvmeibt_add_rv			rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_nic			*new_nic, *nic;	// Read into it, maybe use it.
	struct nvmeibt_nic_config	*f = NULL;
	char 						prot_c;

	NFIN;

	new_nic = NNVMEIBT_TOMA_CALLOC(trace_1_nic_nvmeibt_nic_add, 1, sizeof *new_nic);

	// "uuid","version","node_uuid","guid","partition_key","protocol"
	// fe800000-0000-0000-e41d-2d03001f9341,77777,44444444-4444-4444-4444-444444444444,0xfe80000000000000e41d2d03001f9341,65535,Infiniband
	f = &(new_nic->from_config);

	f = &(new_nic->from_config);
	f->id = conf->uuid;
	f->version = conf->version;
	nvmeibt_strlcpy(f->sw_gid_str, conf->sw_gid_str+2, sizeof(f->sw_gid_str)-1);
	nvmeibt_strlcpy(f->hw_gid_str, conf->hw_gid_str, sizeof(f->hw_gid_str));
	f->its_node_id = node->uuid;
	f->partition_key = conf->pkey;
	switch(conf->protocol) {
	case 1: nvmeibt_strlcpy(f->protocol, "ROCE", sizeof(f->protocol)); break;
	case 2: nvmeibt_strlcpy(f->protocol, "IB", sizeof(f->protocol));   break;
	case 3: nvmeibt_strlcpy(f->protocol, "TCP", sizeof(f->protocol));  break;
	case 4: nvmeibt_strlcpy(f->protocol, "MULTI", sizeof(f->protocol));  break;
	default: nvmeibt_strlcpy(f->protocol, "Unknown", sizeof(f->protocol));  break;
	}

	rv = NNVMEIBT_HASH_ADD_OBJ_new(fkiut86,
					nvmeibt_global_get_global()->nics_hash_by_uuid,
					new_nic,
					config_tag,
					NVMEIBT_MAX_N_NICS, nic, nic);

	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL)
		goto out;

	// In any case, update the following config-driven fields
	// None

	if (rv == NVMEIBT_ADD_NEW) {
		// Init the topology-related fields
		prot_c = toupper(new_nic->from_config.protocol[0]);
		new_nic->transport = nvmeib_transport_cton(prot_c);
		N_Tf(hu83jsw, "added new nic @UUID_LE type @PROT_C:@TRANSPORT_INT", nvmeibt_nic_UUID(new_nic), prot_c, new_nic->transport);
	}

out:
	if (rv == NVMEIBT_ADD_NEW) {
		/* nothing */ ;
	} else {
		N_Tf(oo0044d, "Freeing unused new nic=@UUID_LE", nvmeibt_nic_UUID(new_nic));
		NNVMEIBT_TOMA_FREE(trace_5_nic_nvmeibt_nic_add, new_nic);
	}

	NFOUT;
	return rv;
}

char *nvmeibt_nic_get_node_name(struct nvmeibt_nic *nic)
{
	return (nic ? nvmeibt_node_name(nic->its_node) : "");
}

void nvmeibt_nic_detach_from_node(struct nvmeibt_nic *nic)
{
	nvmeibt_nm_del_remote_nic(nvmeibt_get_nw_node(), nic);
}

void nvmeibt_nic_trim_unused_entries(int config_tag)
{
	struct nvmeibt_nic		*nic;

	NVMEIB_HASH_FOREACH(nic, nvmeibt_global_get_global()->nics_hash_by_uuid) {
		if (NVMEIBT_OBJ_IS_OLDER(nic, config_tag)) {
			N_Tf(tt922bx, "drop nic: @UUID_LE with config tag @INT<@INT", nvmeibt_nic_UUID(nic), nic->config_tag, config_tag);
			NVMEIBT_OBJ_MARK_OUTDATED(fhy76r5, nic, nic);
			nvmeibt_nic_detach_from_node(nic);
			nic_remove(nic);
		}
	}
}

bool nvmeibt_nic_is_roce(enum nvmeib_rdma_transport transport)
{
	return ((transport == rtr_roce) || (transport == rtr_multi));
}

void nvmeibt_nic_free_all_at_exit(void)
{
	struct nvmeibt_nic		*nic;
	NVMEIB_HASH_FOREACH(nic, nvmeibt_global_get_global()->nics_hash_by_uuid) {
		NNVMEIBT_TOMA_FREE(ka90kxq, nic);
	}
}
