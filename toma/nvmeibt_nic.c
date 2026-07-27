#include "nvmeibt_toma.h"
#include <ctype.h>

#include "nvmeibt_common.h"
#include "nvmeibt_bm.h"
#include "nvmeibt_nic.h"
#include "nvmeibt_debug.h"
#include "nvmeibt_node.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "interfaces/network/network_incs.h"

const union nvmeib_uuid *nvmeibt_nic_UUID(struct nvmeibt_nic *nic)
{
	return (nic ? &nic->from_config.id : &nvmeib_uuid_null_val);
}

#ifdef TOMA_DEBUG
void nvmeibt_nic_dump(struct nvmeibt_nic *nic)
{
	struct nvmeibt_nic_config *f = &nic->from_config;

	N_Tf(trace_nic_nvmeibt_nic_dump, "Config data: uuid=@UUID_LE version=@VERSION its_node_uuid=@UUID_LE nicID=@STR guid=@STR partition_key=@INT protocol=@STR",
		nvmeibt_nic_UUID(nic), f->version, &(f->its_node_id), f->nicID, f->guid_str, f->partition_key, f->protocol);
}
#else	// #ifdef TOMA_DEBUG
void nvmeibt_nic_dump(__attribute__((__unused__)) struct nvmeibt_nic *nic) {}
#endif	// #ifdef TOMA_DEBUG

static void nic_remove(struct nvmeibt_nic *nic)
{

	NFIN;

	if (nic == NULL) {
		goto out;
	}

	N_Tf(djuiry5, "Removing nic=@UUID_LE", nvmeibt_nic_UUID(nic));

	NNVMEIBT_HASH_DEL_OBJ(yaioqle, &nvmeibt_global_get_global()->nics_hash, nic, nic);
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
	XDLIST_INIT_LINK(&new_nic->topo_link, NULL);

	// "uuid","version","node_uuid","guid","partition_key","protocol"
	// fe800000-0000-0000-e41d-2d03001f9341,77777,44444444-4444-4444-4444-444444444444,0xfe80000000000000e41d2d03001f9341,65535,Infiniband
	f = &(new_nic->from_config);

	f = &(new_nic->from_config);
	f->id = conf->uuid;
	f->version = conf->version;
	nvmeibt_strlcpy(f->guid_str, conf->guid+2, sizeof(f->guid_str)-1);
	nvmeibt_strlcpy(f->nicID, conf->nicID, sizeof(f->nicID));
	f->its_node_id = node->uuid;
	f->partition_key = conf->pkey;
	switch(conf->protocol) {
	case 1: nvmeibt_strlcpy(f->protocol, "ROCE", sizeof(f->protocol)); break;
	case 2: nvmeibt_strlcpy(f->protocol, "IB", sizeof(f->protocol));   break;
	case 3: nvmeibt_strlcpy(f->protocol, "TCP", sizeof(f->protocol));  break;
	case 4: nvmeibt_strlcpy(f->protocol, "MULTI", sizeof(f->protocol));  break;
	default: nvmeibt_strlcpy(f->protocol, "Unknown", sizeof(f->protocol));  break;
	}

	rv = NNVMEIBT_HASH_ADD_OBJ(fkiut86,
					&nvmeibt_global_get_global()->nics_hash,
					new_nic,
					config_tag,
					NVMEIBT_MAX_N_NICS, nic, NULL, nic);

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

	XHASHTABLE_FOR_EACH_SAFE(nic, &nvmeibt_global_get_global()->nics_hash) {
		if (NVMEIBT_HASH_IS_OLDER_OBJ(nic, config_tag)) {
			N_Tf(tt922bx, "drop nic: @UUID_LE with config tag @INT<@INT", nvmeibt_nic_UUID(nic), nic->config_tag, config_tag);
			NVMEIBT_HASH_MARK_OBJ_OUTDATED(fhy76r5, nic, nic);
			nvmeibt_nic_detach_from_node(nic);
			nic_remove(nic);
		}
	}
}

bool nvmeibt_nic_is_roce(enum nvmeib_rdma_transport transport)
{
	return ((transport == rtr_roce) || (transport == rtr_multi));
}

