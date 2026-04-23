/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
/* Implements other Tomas in raft quorum of the alive Toma */
#include "../sandbox_util.h"

struct clnt_simu {
	struct sb_cluster_conf *cfg;						// Global configuration (access to LiveToma's local nvme drives, attach to volumes)
	int node_idx;										// Node on which this client resides
	struct clnt_praid_reg_ctx1 {
		struct sb_praid_conf *ptr;						// The praid client is registering due to volume attach
		u32 praid_version;								//
		// Todo: Save minimal info about Toma topology to know which dbits to write
		u32 lock_id;									// Lock id given by Toma, 0 if unregistered
		u32 n_ios;										// N simulated ios to blocksets that were done
	} regs[8];
};

struct clnt_simu *clnt_simu_create(        struct sb_cluster_conf *, int node_idx);
void              clnt_simu_destroy(       struct sb_cluster_conf *, int node_idx);
struct clnt_simu *clnt_simu_get_local_clnt(struct sb_cluster_conf *);
void clnt_simu_vol_attach(                 struct sb_cluster_conf *, int node_idx, int vol_idx);
void clnt_simu_vol_lock_blockset_v(        struct sb_cluster_conf *, int node_idx, int vol_idx , u32 vlba_blockset);
void clnt_simu_vol_lock_blockset_d(        struct sb_cluster_conf *, int node_idx, int disk_idx, u32 dlba_blockset);
void clnt_simu_vol_unregister(             struct sb_cluster_conf *, int node_idx, int vol_idx /*, int praid_idx*/);		// Possibly leave stale lock
void clnt_simu_vol_detach(                 struct sb_cluster_conf *, int node_idx, int vol_idx);
