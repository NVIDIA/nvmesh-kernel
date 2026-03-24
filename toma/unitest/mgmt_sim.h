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
		uint32_t uuid;			// uuid of this node
		struct sb_nics_conf {
			uint32_t uuid;
		} nics[2];				// Each node has exactly 2 nics
		bool ignore_append_entries;			// Emulates infinitely slow local disk response time, does not commit raft leaders topo
	} nodes[3], *live, *other;	// Cluster of 3 machines, 1 live followed by 2 simulated other tomas, presented as nodes n37, n38, n49
	int n_nodes;
	struct sb_volume_conf {							// All volumes configuration
		const char* name;
		uint32_t uuid;								// For simplicity all uuids are u32
		unsigned num_blocks;						// Volume size (num of 4KB blocks)
		unsigned num_chunks;						// Created with ==1
		struct sb_chunk_conf {
			uint32_t uuid;							// My chunk uuid
			unsigned vlba_start;					// Implemented volume blocks range via this chunk
			unsigned vlba_end;
			unsigned n_raids;						// Striping size (Raid-0 implementation)
			struct sb_praid_conf {
				uint32_t uuid;						// My praid uuid
				unsigned D, P;						// (data+parity) protection. R1 = (1+{1..2}), EC = ({2..8}+{1..2})
				struct sb_seg_conf {
					uint32_t disk_uuid;				// Physical disk uuid where disk segment resides, Todo consider changing to u32
					uint32_t uuid;					// My disk segment uuid
					unsigned block_start;			// Disk block address of segment start
					unsigned block_end;				// All disk segments in chunk have identical length
				} segs[4];							// Up to 3+1 EC, for now
			} raids[1];								// For now, each chunk has only 1 praid. Dont support Raid-0
		} chunks[2];								// For now, 2 chunks only, Support for volume extend once
	} vols[4];										// For now, up to 4 volumes
	int n_vols;
	int zone_idx;									// All those volume exist in a specific zone
};

void sb_cluster_conf_create( struct sb_cluster_conf *);
void sb_cluster_conf_destroy(struct sb_cluster_conf *);
int  sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *, const char *host_name);

void sb_cluster_ignore_append_entries_by_node(int node_idx);

/*********************************************/

struct mgmt_sim_state;
struct sim_broker_topic;
struct mgmt_sim_state *mgmt_sim_init(struct sb_cluster_conf *initialized_cfg);
void mgmt_sim_send_msg_change_raft_quorum(const int node_idx, bool do_add);
void mgmt_sim_send_msg_assign_to_zone(int zone_idx);
void mgmt_sim_wakeup_on_incomming_toma_msg(struct sim_broker_topic *);
void mgmt_sim_do_periodic(void);
void mgmt_sim_destroy(bool do_verify_used);

/* Condition-query functions for fiber-based test scenario */
bool mgmt_sim_both_disks_ready_for_format(void);
bool mgmt_sim_drive_format_is_done(const char *drive_name);
bool mgmt_sim_consume_got_report_target(void);
bool mgmt_sim_v_r1_praid_reported(void);
bool mgmt_sim_v_r1_seg_zeroing_seen(void);
bool mgmt_sim_v_r1_praid_deprecated(void);
bool mgmt_sim_v_r1_praid_absent_from_report(void);

int mgmt_sim_get_n_leader_keep_alives_received(void);

/* Message-sender functions for fiber-based test scenario */
void mgmt_sim_send_format_drive(const char *drive_name);
void mgmt_sim_send_leader_keep_alive(void);
void mgmt_sim_send_msg_latest_hw_config(void);
void mgmt_sim_send_add_volume_remote1(void);
void mgmt_sim_send_add_volume_r1(void);
void mgmt_sim_send_delete_volume_r1(void);
void mgmt_sim_send_delete_volume_completed_r1(void);

#endif /* TOMA_UNITEST_MGMT_SIM_H */
