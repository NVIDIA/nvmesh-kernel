/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
#include "../13_mgmt/mongodb_simu.h"

struct clnt_simu {
	struct sb_cluster_conf *cfg;						// Global configuration (access to LiveToma's local nvme drives, attach to volumes)
	int node_idx;										// Node on which this client resides
	unsigned unique_msg_to_toma_counter;
};

// API towards unitest environment
struct clnt_simu *clnt_simu_create(        struct sb_cluster_conf *, int node_idx);
void              clnt_simu_destroy(       struct sb_cluster_conf *, int node_idx);
struct clnt_simu *clnt_simu_get_local_clnt(struct sb_cluster_conf *);
void clnt_simu_vol_attach(                 struct sb_cluster_conf *, int node_idx, int vol_idx);
void clnt_simu_vol_register(               struct sb_cluster_conf *, int node_idx, int vol_idx, bool do_reg /*, int praid_idx*/);
bool clnt_simu_vol_is_ioable(                                        int node_idx, int vol_idx);
void clnt_simu_vol_lock_blockset_v(        struct sb_cluster_conf *, int node_idx, int vol_idx , u32 vlba_blockset);
void clnt_simu_vol_lock_blockset_d(        struct sb_cluster_conf *, int node_idx, int disk_idx, u32 dlba_blockset);
void clnt_simu_vol_detach(                 struct sb_cluster_conf *, int node_idx, int vol_idx);

// API towards server simulator
struct nvmeibs_toma_client_proc_buf;
void clnt_simu_receive_msg_from_toma(const struct nvmeibs_toma_client_proc_buf *msg, int len);
