/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * mgmt_sim.h - Management simulator for Toma sandbox unit tests
 *
 * This module simulates the management server's Kafka message exchanges with Toma.
 * It provides message templates and tracks state for test scenarios.
 */
#ifndef TOMA_UNITEST_MGMT_SIM_H
#define TOMA_UNITEST_MGMT_SIM_H

#include "sandbox_util.h"

/************** Cluster config *******************************/
struct sb_cluster_conf {
	char my_hostname[64];		// Live Toma (non sandbox, real hostname)
	struct sb_node_conf {
		const char *hostname;	// Easily recognizable host name
		char uuid[40];			// Hex as string format (36 characters + NULL terminated string)
		uint64_t uuid16b[2];	// Packed 16[b] format
	} nodes[3], *live, *other;	// Cluster of 3 machines, 1 live followed by 2 simulated other tomas
	int n_nodes;
};
void sb_cluster_conf_create( struct sb_cluster_conf *);
void sb_cluster_conf_destroy(struct sb_cluster_conf *);
int  sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *, const char *host_name);

/*********************************************/

struct mgmt_sim_state;
struct sim_broker_topic;
struct mgmt_sim_state *mgmt_sim_init(struct sb_cluster_conf *initialized_cfg);
void mgmt_sim_send_msg_change_raft_quorum(const int node_idx, bool do_add);
void mgmt_sim_send_msg_assign_to_zone(int zone_idx);
void mgmt_sim_send_msg_latest_hw_config(void);
void mgmt_sim_wakeup_on_incomming_toma_msg(struct sim_broker_topic *);
void mgmt_sim_verify_at_end(void);
void mgmt_sim_do_periodic(void);
void mgmt_sim_destroy(void);

/* Condition-query functions for fiber-based test scenario */
bool mgmt_sim_both_disks_ready_for_format(void);
bool mgmt_sim_both_disks_formatted_ok(void);
bool mgmt_sim_consume_got_report_target(void);
bool mgmt_sim_v_r1_praid_reported(void);
bool mgmt_sim_both_disks_zeroing_done(void);

/* Message-sender functions for fiber-based test scenario */
void mgmt_sim_send_format_drives(void);
void mgmt_sim_send_add_volume_remote1(void);
void mgmt_sim_send_add_volume_r1(void);

#endif /* TOMA_UNITEST_MGMT_SIM_H */
