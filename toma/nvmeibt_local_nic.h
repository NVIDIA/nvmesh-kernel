#ifndef NVMEIBT_LOCAL_NIC
#define NVMEIBT_LOCAL_NIC

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"

struct nvmeibt_local_nic_config {
	union nvmeib_uuid	id;
	char	device_type[30]; /* Nic name */
	struct nvmeibt_urn_uuid	hw_gid_uuid;
	int 	port;
	char	pkey[11];
	char	link[30];
	char	state[30];
	int		mtu;
	int		max_mtu;
	int		gid_index;
	bool	roce_v2;
	bool	roce_ipv6;
	bool	used;
	char	device_network_name[30];
	struct nvmeibt_urn_uuid	sw_gid_uuid;
};

struct nvmeibt_local_nic {
	struct nvmeibt_local_nic_config	from_config;
	struct nvmeibt_nic				*its_nic;
	struct xdlist					topo_link;
	int								config_tag;
};

#include "nvmeibt_topology.h"

enum nvmeibt_add_rv nvmeibt_local_nic_add(char *config_str, int config_tag);
void nvmeibt_local_nic_trim_unused_entries(int config_tag);
struct nvmeibt_local_nic * nvmeibt_local_nic_nic_to_local_nic(struct nvmeibt_nic *nic);
void nvmeibt_local_nic_free_all_at_exit(void);
#endif // #ifndef NVMEIBT_LOCAL_NIC

