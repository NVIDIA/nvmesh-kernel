#ifndef NVMEIBT_CLIENT
#define NVMEIBT_CLIENT

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"

struct nvmeibt_local_disk;

struct nvmeibt_client {												// Describes connection of client to local disk
	union nvmeib_uuid					client_provided_uuid;		// UUID binary, like: 0x124a1d9f09d21c25eb816ae030a1f98e
	struct nvmeibt_urn_uuid				client_provided_urn_uuid;	// UUID text formatted, like: 251cd209-9f1d-4a12-8ef9-a130e06a81eb
	struct nvmeibt_host_name			net;						// Like: nvme112.acme.com
	struct nvmeibt_ascii_uuid			ldisk_id;					// Like: S3HCNX0JC01988.1
	struct nvmeibt_local_disk			*local_disk;
	u32									cid;						// Small number [0..4K]
	int									n_reg_ctx_refs;				// n segs on this disk that the client registered
	BOOL								is_connected;				// True if server did not try to unsubscribe the client yet
	bool								is_delete_in_the_air;
};

static inline uint32_t client_messaging_handle_to_cid(unsigned long long client_messaging_handle)
{
	return (uint32_t)(client_messaging_handle >> 32);
}

static inline bool nvmeibt_client_is_delete_in_the_air(const struct nvmeibt_client *client)
{
	return (!client || client->is_delete_in_the_air);
}

static inline const char *nvmeibt_client_get_urn_uuid_str(const struct nvmeibt_client *client) { return (client ? client->client_provided_urn_uuid.str : ""); }
static inline const char *nvmeibt_client_get_hostname(    const struct nvmeibt_client *client) { return (client ? client->net.host_name : ""); }
int nvmeibt_client_handle_incoming_message(struct nvmeibs_toma_server_proc_buf *msg_buf, int size);

void handle_subscriber_event(struct nvmeibs_msg_s2t_subscriber_change *msg);
struct nvmeibt_registrant_ctx;
void handle_client_remove(uint32_t cid);
void handle_client_disconnect_event(const struct nvmeibs_msg_s2t_client_disconnect *h);
void nvmeibt_client_reg_ctx_ref_added(struct nvmeibt_client *client, struct nvmeibt_registrant_ctx *reg_ctx_for_logging);
void nvmeibt_client_reg_ctx_ref_removed(struct nvmeibt_client *client, struct nvmeibt_registrant_ctx *reg_ctx_for_logging);

#endif
