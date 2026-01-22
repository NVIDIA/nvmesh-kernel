/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_node.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_topology.h"
#include "interfaces/network/network_incs.h"
#include "nvmeibt_global.h"

#define MAX_RX_CONNS_ARRAY 256

void nvmeibt_node_dump(__attribute__((__unused__)) const struct nvmeibt_node *node)
{
#ifdef TOMA_DEBUG
	const struct nvmeibt_node_config *f = &node->from_config;
	N_Tf(yeeu83x, "Config data: id=@UUID_LE version=@VERSION name=@NAME", &f->id, f->version, f->name);
#endif
}

const union nvmeib_uuid *nvmeibt_node_UUID(struct nvmeibt_node *node)
{
	return (node ? &node->from_config.id : &nvmeib_uuid_null_val);
}

char *nvmeibt_node_name(struct nvmeibt_node *node)
{
	return (node ? node->from_config.name : "");
}

static void nvmeibt_node_remove(struct nvmeibt_node *node)
{
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;

	if (node == NULL) {
		goto out;
	}

	N_Tf(jeu2291, "Removing node=@UUID_LE", nvmeibt_node_UUID(node));
	nvmeibt_raft_unlink_member_from_node(NULL, node);

	NNVMEIBT_HASH_DEL_OBJ_new(vts5unc, cur_topo->nodes_hash_by_uuid, node, node);
	NNVMEIBT_BM_FREE(trace_2_node_nvmeibt_node_remove, node);

out:
	NFOUT;
}

enum nvmeibt_add_rv nvmeibt_node_add(struct mm_node_conf *conf, int config_tag)
{
	enum nvmeibt_add_rv			rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_node			*new_node, *node;	// Read into it, maybe use it.
	struct nvmeibt_node_config	*f = NULL;

	NFIN;

	new_node = NNVMEIBT_BM_CALLOC(trace_node_nvmeibt_node_add, sizeof *new_node);

	f = &(new_node->from_config);

	f->id = conf->uuid;
	f->version = conf->version;
	strlcpy(f->name, conf->node_id, sizeof(f->name));

	if (pthread_mutex_init(&new_node->guard, NULL) < 0) {
		N_Ef(error_node_nvmeibt_node_add, "Failed to create node @NODE_NAME guard @AUTO_ERRNO",
			nvmeibt_node_name(new_node));
		rv = NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL;
		goto out;
	}

	rv = NNVMEIBT_HASH_ADD_OBJ_new(4vnfcus,
					nvmeibt_global_get_global()->nodes_hash_by_uuid,
					new_node,
					config_tag,
					NVMEIBT_MAX_N_NODES, node, node);

	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL)
		goto out;

	// In any case, update the following config-driven fields
	// None

	if (rv == NVMEIBT_ADD_NEW) {
		nvmeibt_raft_link_member_to_node(NULL, node, nvmeibt_node_UUID(node));
		nvmeibt_node_reset_IIRs(node, 1);
	}

out:
	if (rv == NVMEIBT_ADD_NEW) {
		/* nothing */ ;
	} else {
		N_Tf(trace_2_node_nvmeibt_node_add, "Freeing unused new node=@NODE", nvmeibt_node_name(new_node));
		NNVMEIBT_BM_FREE(trace_3_node_nvmeibt_node_add, new_node);
	}

	NFOUT;
	return rv;
}

void nvmeibt_node_reset_IIRs(struct nvmeibt_node *node, bool is_resetting_ping)
{
	if (is_resetting_ping) {
		NVMEIB_IIR_RESET(jsijrb4, &(node->peer_statistics.ping_response_time_IIR), NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT / N_PINGS_PER_RAFT_HEARTBEAT, "all ", nvmeibt_node_name(node));
		NVMEIB_IIR_RESET(6bslk03, &(node->peer_statistics.ping_response_time_exceptional_IIR), NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT * 5, "exceptional ", nvmeibt_node_name(node));
	}
	NVMEIB_IIR_RESET(vjhs82l, &(node->peer_statistics.APPEND_ENTRIES_REP_IIR), NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT / 2, "all ", nvmeibt_node_name(node));
	NVMEIB_IIR_RESET(vjhs82l, &(node->peer_statistics.APPEND_ENTRIES_REP_scaling_IIR), NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT, "scaling ", nvmeibt_node_name(node));
	NVMEIB_IIR_RESET(sh8ikpn, &(node->peer_statistics.APPEND_ENTRIES_REP_degrading_IIR), NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT, "degrading ", nvmeibt_node_name(node));
	NVMEIB_IIR_RESET(sh8ikpn, &(node->peer_statistics.APPEND_ENTRIES_REP_exceptional_IIR), NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT * 5, "exceptional ", nvmeibt_node_name(node));
}

void nvmeibt_node_locate_my_node(void)
{
	struct nvmeibt_node			*my_node = NULL;
	struct nvmeibt_node			*node;
	struct nvmeibt_node_config	*f;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;

	N_Tf(trace_node_nvmeibt_node_locate_my_node, "n_nodes=@N_ELEMENTS", nvmeib_hash_get_n_elements(cur_topo->nodes_hash_by_uuid));
	if (nvmeib_hash_get_n_elements(cur_topo->nodes_hash_by_uuid) == 0) {
		goto out;
	}
	NVMEIB_HASH_FOREACH(node, cur_topo->nodes_hash_by_uuid) {
		f = &node->from_config;
		N_Tf(nvmeibt_node_locate_my_node_1, "comparing @STR to @STR", f->name, nvmeibt_get_my_hostname());
		if (!strncmp(f->name, nvmeibt_get_my_hostname(), sizeof(f->name))) {
			N_Tf(nvmeibt_node_locate_my_node_2, "Found node @STR", f->name);
			my_node = node;
		} else {
			node->is_my_node = false;
		}
	}
	nvmeibt_global_set_my_node(my_node);	// Also assigns node->is_my_node as needed
	if (!nvmeibt_global_get_my_node()) {
		if (timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), nvmeibt_global_get_startup_timespec()) > SEC_TO_NSEC(30)) {
			N_Wf(a7b3lmh, "The nodes list doesn't include me '@STR'!", nvmeibt_get_my_hostname());
		} else {
			N_Tf(1vys9k2, "The nodes list doesn't include me '@STR'!", nvmeibt_get_my_hostname());
		}
	} else {
		// Send a report target, as the initialization of my_node triggers possibility to
		// updating mgmt_db_uuid  for some local disks, and hence reporting them to the MGMT,
		// no need to wait for some random report_target event in the future for that.
		nvmeibt_toma_mark_is_need_to_update_the_main_select_fds();
		NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(2cxfas8);
	}
out:
	NFOUT;
}

static int lock(struct nvmeibt_node *node)
{
	int rv;

	NFIN;
	if ((rv = pthread_mutex_lock(&node->guard)) != 0) {
		N_Ef(error_node_lock, "Failed to lock node @NODE_NAME guard @AUTO_ERRNO", nvmeibt_node_name(node));
		nvmeibt_abort(ES_FATAL);
	}
	NFOUT;
	return rv;
}

static int unlock(struct nvmeibt_node *node)
{
	int rv;

	NFIN;
	if ((rv = pthread_mutex_unlock(&node->guard)) != 0) {
		N_Ef(error_node_unlock, "Failed to unlock node @NODE_NAME guard @AUTO_ERRNO", nvmeibt_node_name(node));
		nvmeibt_abort(ES_FATAL);
	}
	NFOUT;
	return rv;
}

struct connection_context *nvmeibt_node_get_tx_conn_ctx(struct nvmeibt_node *node)
{
	return (struct connection_context *)(node ? node->conn_ctx : NULL);
}

/**
 * return the first CONNECTED UDP peer
 */
struct udp_peer *nvmeibt_node_get_udp_peer(struct nvmeibt_node *node)
{
	(void) node;
	return NULL;
}

struct nvmeibt_node *nvmeibt_node_get_node_by_id(const union nvmeib_uuid *id)
{
	struct nvmeibt_node *node;

	node =  nvmeib_hash_search_uuid(nvmeibt_global_get_global()->nodes_hash_by_uuid, id);

	if (node == NULL) {
		N_Tf(jiy9336, "Node not found id=@UUID_LE", id);
	}

	return node;
}

int conn_state_switch_cb_wrapper(struct nvmeibt_node *node, void *arg,
								 conn_state_switch_t cb,
								 va_list args)
{
	int state;
	int n_conns;
	struct connection_context *conn_ctx;
	int rv = 0;

	NFIN;

	if (cb) {
		state = va_arg(args, int);
		n_conns = va_arg(args, int);
		conn_ctx = va_arg(args, struct connection_context *);

		N_Tf(trace_node_conn_state_switch_cb_wrapper, "(va_arg[0]) state     = @STATE", state);
		N_Tf(trace_1_node_conn_state_switch_cb_wrapper, "(va_arg[1]) n_conns   = @N_CONNS", n_conns);
		N_Tf(trace_2_node_conn_state_switch_cb_wrapper, "(va_arg[2]) conn_ctx  = @CONN_CTX", conn_ctx);
		rv = cb(node, arg, (NVMEIBT_CONN_STATE_T)state, n_conns, conn_ctx);
	}

	NFOUT;
	return rv;
}

void nvmeibt_node_trim_unused_entries(int config_tag)
{
	struct nvmeibt_node		*node;

	NFIN;
	NVMEIB_HASH_FOREACH(node, nvmeibt_global_get_global()->nodes_hash_by_uuid) {
		if (NVMEIBT_OBJ_IS_OLDER(node, config_tag)) {
			N_Tf(skqo227, "drop node: @UUID_LE with config tag @INT<@INT", nvmeibt_node_UUID(node), node->config_tag, config_tag);
			/*
			 * close tx_conn and all rx_conns:
			 * - unregister conn's srm-users (under node lock)
			 * - call registered ncs-cb with ncs_conn_down state (w/o node lock)
			 */
			nvmeibt_nm_del_remote_node(nvmeibt_get_nw_node(), node);
			/* from here on, no one uses this node ... */
			if (pthread_mutex_destroy(&node->guard)) {
				N_Ef(xx_33, "Failed to destroy node guard @AUTO_ERRNO");
			}
			nvmeibt_topology_leader_detach_all_disks_from_raft_member(nvmeibt_node_get_raft_member(node));
			nvmeibt_topology_detach_all_disks_from_node(node);
			nvmeibt_topology_detach_all_nics_from_node(node);
			//			nvmeibt_raft_node_was_removed(node);
			if (nvmeibt_global_get_my_node() == node) {
				node->is_my_node = 0;
				nvmeibt_global_set_my_node(NULL);
			}
			NVMEIBT_OBJ_MARK_OUTDATED(nvmeibt_node_trim_unused_entries_trace, node, node);
			nvmeibt_node_remove(node);
		}
	}
	NFOUT;
}

void nvmeibt_node_free_all_at_exit(void)
{
	struct nvmeibt_node		*node;
	NVMEIB_HASH_FOREACH(node, nvmeibt_global_get_global()->nodes_hash_by_uuid) {
		NNVMEIBT_BM_FREE(wixjby2, node);
	}
}

void nvmeibt_node_cancel_send(struct nvmeibt_node *node)
{
	NFIN;
	lock(node);
	nvmeibt_nm_cancel_req_node(nvmeibt_get_nw_node(), node);
	unlock(node);
	NFOUT;
}

int nvmeibt_node_send(struct nvmeibt_node *node, struct nvmeibt_msg_request *req)
{
	int rv = -1;

	NFIN;
	lock(node);
	rv = nvmeibt_nm_queue_srm_req(nvmeibt_get_nw_node(), node, req);
	unlock(node);
	NFOUT;
	return rv;
}

void nvmeibt_node_remove_nic(struct nvmeibt_node *node, struct nvmeibt_nic *nic)
{
	int i;

	NFIN;
	if (node) {
		for (i = node->n_nics; i--;) {
			if (node->nics[i] == nic) {
				node->nics[i] = node->nics[--node->n_nics];
				nic->its_node = NULL;
				break;
			}
		}
	}
	NFOUT;
}

