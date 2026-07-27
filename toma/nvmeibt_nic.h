#ifndef NVMEIBT_NIC
#define NVMEIBT_NIC

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_mm_json.h"

struct nvmeibt_nic_config {
	union nvmeib_uuid		id;
	int						version;
	union nvmeib_uuid		its_node_id;
	char					nicID[35];
	char					guid_str[35];
	int						partition_key;
	char					protocol[20];
};

struct nvmeibt_node;
struct nvmeibt_topology;

struct nvmeibt_nic {
	struct nvmeibt_nic_config		from_config;
	struct nvmeibt_node				*its_node;
	void							*tx_conn_ctx;	// Used by ib to send messages to this nic
	enum nvmeib_rdma_transport		transport;
	struct xdlist					topo_link;
	int								config_tag;
};

const union nvmeib_uuid *nvmeibt_nic_UUID(struct nvmeibt_nic *nic);
char *nvmeibt_nic_get_node_name(struct nvmeibt_nic *nic);
void nvmeibt_nic_dump(struct nvmeibt_nic *nic);
enum nvmeibt_add_rv nvmeibt_nic_add(struct mm_nic_conf *conf, struct mm_node_conf *node, int config_tag);
#define nvmeibt_nic_get_tx_conn_ctx(nic) ((struct connection_context *)((nic)->tx_conn_ctx))
void nvmeibt_nic_detach_from_node(struct nvmeibt_nic *nic);
void nvmeibt_nic_trim_unused_entries(int n_used);
bool nvmeibt_nic_is_roce(enum nvmeib_rdma_transport transport);

#endif

