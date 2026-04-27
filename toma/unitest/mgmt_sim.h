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
bool mgmt_sim_v_r1_praid_reported(void);
bool mgmt_sim_v_r1_seg_zeroing_seen(void);
bool mgmt_sim_v_r1_praid_deprecated(void);
bool mgmt_sim_v_r1_praid_absent_from_report(void);

/* Message-sender functions for fiber-based test scenario */
void mgmt_sim_send_format_drive(int disk_idx_on_node);
void mgmt_sim_send_leader_keep_alive(void);
void mgmt_sim_send_msg_latest_hw_config(void);
void mgmt_sim_send_add_volume(int vol_idx);
void mgmt_sim_send_delete_volume_r1(void);
void mgmt_sim_send_delete_volume_completed_r1(void);

struct mgmt_sim_vol_seg_update {
	int seg_idx;			// Index into disk segments array (sb_praid_conf.segs[])
	int praid_idx;			// Location inside raid {D0,D1,...DN,P,Q}. Example seg[seg_idx=D+P] can replace D5 (praid_dx=5).
	const char *status;		// e.g. "normal" | "markedForRebuild" | "markedForRebuild_old"
};

/** Send updateVolume with an explicit per-segment status list and an
 *  explicit payload version / volume status / volume action.  The scenario
 *  uses this to emit the v2 (replacement added) and v3 (deprecated removed)
 *  updates that drive eviction. */
void mgmt_sim_send_volume_update(int vol_idx, const char *vol_status, const char *vol_action, const struct mgmt_sim_vol_seg_update *segs, int n_segs);

/** Clear cached condition flags derived from V_R1 pRaidReports so a
 *  subsequent scenario phase starts from a known baseline. */
void mgmt_sim_reset_v_r1_report_state(void);

/** Per-segment snapshot extracted from the latest updatePRaidReport for V_R1. */
struct mgmt_sim_praid_report_seg {
	u32  uuid;
	enum seg_topo_state status1;
};

struct mgmt_sim_praid_report_snapshot {
	int n_segments;
	struct mgmt_sim_praid_report_seg segs[12];		// NVMEIBT_MAX_N_SEGMENTS_IN_PRAID
	bool was_under_recovery_witnessed;				// latches on first observation; cleared only by reset
};

/** Live view of the latest V_R1 pRaidReport.  Each new report overwrites
 *  the per-segment array, except for was_under_recovery_witnessed which
 *  latches so scenarios can assert on transient states. */
const struct mgmt_sim_praid_report_snapshot *mgmt_sim_get_v_r1_report(void);
void mgmt_sim_send_praid_report_req(const u32 praid_uuid);
void mgmt_sim_send_volume_exclusive_attach_notify(const u32 volume_uuid);
void mgmt_sim_send_disk_report_req(const u32 disk_idx);
