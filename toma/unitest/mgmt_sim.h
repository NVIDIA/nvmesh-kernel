/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/* Management simulator for Toma sandbox unit tests */
#include "13_mgmt/mongodb_simu.h"

struct mgmt_sim_state;
struct sim_broker_topic;
struct mgmt_sim_state *mgmt_sim_init(struct sb_cluster_conf *initialized_cfg);
void mgmt_sim_send_msg_change_raft_quorum(const int node_idx, bool do_add);
void mgmt_sim_send_msg_assign_to_zone(int zone_idx);
void mgmt_sim_wakeup_on_incomming_toma_msg(struct sim_broker_topic *);
void mgmt_sim_destroy(bool do_verify_used);

/* Condition-query functions for fiber-based test scenario */
bool mgmt_sim_both_disks_ready_for_format(void);
bool mgmt_sim_drive_format_is_done(int disk_idx_on_node);
bool mgmt_sim_consume_got_report_target(void);
bool mgmt_sim_v_r1_praid_reported(void);
bool mgmt_sim_v_r1_seg_zeroing_seen(void);
bool mgmt_sim_v_r1_praid_deprecated(void);
bool mgmt_sim_v_r1_praid_absent_from_report(void);

int mgmt_sim_get_n_leader_keep_alives_received(void);

/* Message-sender functions for fiber-based test scenario */
void mgmt_sim_send_format_drive(int disk_idx_on_node);
void mgmt_sim_send_leader_keep_alive(void);
void mgmt_sim_send_msg_latest_hw_config(void);
void mgmt_sim_send_add_volume_remote1(void);
void mgmt_sim_send_add_volume_r1(void);
void mgmt_sim_send_delete_volume_r1(void);
void mgmt_sim_send_delete_volume_completed_r1(void);
void mgmt_sim_send_praid_report_req(const u32 praid_uuid);
