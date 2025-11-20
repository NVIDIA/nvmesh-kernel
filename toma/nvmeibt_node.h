#ifndef NVMEIBT_NODE
#define NVMEIBT_NODE

#include <pthread.h>
#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
#include "interfaces/network/network_incs.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_mm_json.h"

struct nvmeibt_node_config {
	union nvmeib_uuid			id;
	int							version;
	char						name[NVMEIB_HOST_NAME_LEN];
};

struct nvmeibt_nic;
struct nvmeibt_disk;
struct nvmeibt_topology;
struct nvmeibt_raft_member;

#include "../common/nvmeib_iir.h"
struct nvmeibt_peer_statistics {
	struct nvmeib_iir		ping_response_time_IIR;
	struct nvmeib_iir		ping_response_time_exceptional_IIR;
	int						ping_response_logging_counter;
	struct timespec			last_ping_response_timespec;
	//
	struct nvmeib_iir		APPEND_ENTRIES_REP_IIR;
	struct nvmeib_iir		APPEND_ENTRIES_REP_exceptional_IIR;
	struct nvmeib_iir		APPEND_ENTRIES_REP_scaling_IIR;
	struct nvmeib_iir		APPEND_ENTRIES_REP_degrading_IIR;
	bool					is_awaiting_REP_first_APPEND_ENTRIES_with_cur_committed_and_applied_topo_timespec;
};

struct nvmeibt_node {
	struct nvmeibt_node_config		 	from_config;
	struct nvmeibt_disk					*disks_config[NVMEIBT_MAX_N_DISKS];
	int									n_disks_config;
	void								*conn_ctx;	// Used by ib to send messages to this node
	struct nvmeibt_nic					*nics[NVMEIBT_MAX_N_NICS_PER_NODE];
	int									n_nics;
	int									n_disks_needed_for_vol;
	bool 								is_my_node;
	bool								append_entries_rep_was_not_sent;
	int									config_tag;
	int									serialization_signature;
	struct nvmeibt_raft_member			*raft_member;
	struct nvmeibt_peer_statistics		peer_statistics;

	pthread_mutex_t guard;
};

struct connection_context;
typedef int (*conn_state_switch_t)(struct nvmeibt_node *node, void *arg,
								   NVMEIBT_CONN_STATE_T state, int n_conns,
								   struct connection_context *conn_ctx);

static inline bool nvmeibt_node_is_my_node(struct nvmeibt_node *node)
{
	return (node && node->is_my_node);
}

static inline struct nvmeibt_raft_member *nvmeibt_node_get_raft_member(struct nvmeibt_node *node)
{
	return (node ? node->raft_member : NULL);
}

const union nvmeib_uuid *nvmeibt_node_UUID(struct nvmeibt_node *node);
char *nvmeibt_node_name(struct nvmeibt_node *node);
enum nvmeibt_add_rv nvmeibt_node_add(struct mm_node_conf *conf, int config_tag);
void nvmeibt_node_reset_IIRs(struct nvmeibt_node *node, bool is_resetting_ping);
struct connection_context *nvmeibt_node_get_tx_conn_ctx(struct nvmeibt_node *node);
struct udp_peer *nvmeibt_node_get_udp_peer(struct nvmeibt_node *node);
void nvmeibt_node_locate_my_node(void);
struct nvmeibt_node *nvmeibt_node_get_node_by_id(const union nvmeib_uuid *id);

/* the below function set/unset (NULL callback) the switch state callback.
   to set a new value we first must to unset the value.
   the return value is 0 is we set a new value or -1 if we failed
*/
void nvmeibt_node_trim_unused_entries(int n_used);
void nvmeibt_node_free_all_at_exit(void);
struct nvmeibt_msg_request;
void nvmeibt_node_cancel_send(struct nvmeibt_node *node);
int nvmeibt_node_send(struct nvmeibt_node *node, struct nvmeibt_msg_request *req);

void nvmeibt_node_remove_nic(struct nvmeibt_node *node, struct nvmeibt_nic *nic);
#endif // #ifndef NVMEIBT_NODE
