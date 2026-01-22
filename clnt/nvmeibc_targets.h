/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_TARGETS_H
#define NVMEIBC_TARGETS_H

#include "nvmeibc_block.h"
#include "nvmeibc_mcs_stub.h"

// This struct is used to hold both the DxV from disk_id side and all of the nics (both configuration and formatted) on the target
// 1. D - disks, V - volumes VxD connections between them.
// 2. Targets: holds list of VXD for all of his disks D
// 3.          holds list of all of his nics (configuration + pointer of arnic).
// 4.          Back-compatibility: direct list of arnics for transport layer api
struct nvmeibc_target {
	// The list of all targets
	struct list_head link;
	// The uuid of the target for finding it
	char node_id[NVMEIB_HOST_NAME_LEN];
	char node_uuid[NVMEIBC_BD_UUID_LEN];
	// The targets list of nics (updated from MCS either by new_nic, nic_reapper, delete_nic or from new/update volume configuration)
	struct list_head nics;
	// The list of disk_ids currently residing on the target that are used by any of the local volumes
	struct list_head disks;
	// The list of the arnics (hosts) after formatting the configuration
	struct list_head arnics;
	// The management version of the latest set of nics the client is aware
	int mgmt_nics_version;
	int target_update_sequence;
	struct latest_issued_nics_query_info{
		ulong at; //jiffies
		int sent_nics_version;
	} nics_query_cache;
};

static inline struct nvmeibc_target_nics_query nvmeibc_target_get_nics_query(const struct nvmeibc_target* target)
{
	struct nvmeibc_target_nics_query query = {.nicsVersion=target->mgmt_nics_version};
	strlcpy(query.node_id, target->node_id, sizeof(query.node_id));
	BUILD_BUG_ON(sizeof(query.nodeUUID) != sizeof(target->node_uuid));
	strlcpy(query.nodeUUID, target->node_uuid, sizeof(query.nodeUUID));
	return query;
}

// This is for debug, later all will be done with arnics list of nvmeibc_admin_rnic
struct nvmeibc_target_nic {
	struct list_head link;				// The link to all nics
	struct nvmeibc_target* parent;		// The target that hold this nic
	struct nvmeibc_nic_conf data;		// The nic configuration
	struct nvmeibc_admin_rnic host;		// The formatted nic used to create channels
	bool mark_for_deletion;				// Each time we update a target we mark all nics for deletion unless they appear
};

// Used to access a disk_ids list of nics on it's target
static inline struct list_head *nvmeibc_disk_id_to_nics(struct nvmeibc_disk_id *disk_id)
{
	BUG_ON(!disk_id->target);
	return &disk_id->target->arnics;
}

/* Update existing target or create if node does not exist */
struct nvmeibc_cinst_params_main;
bool nvmeibc_target_update_or_create(const struct nvmeibc_cinst_params_main *,
	const struct nvmeibc_target_conf *src, struct nvmeibc_target **target);

void nvmeibc_target_update(const struct nvmeibc_cinst_params_main *,
	const struct nvmeibc_target_conf *src);

/*
	populates nodeUUID and nicsVersion from the target, that query.node_id referes to
	returns false if target was not found 
*/
bool nvmeibc_target_fill_nics_query_by_node_id(const struct nvmeibc_cinst_params_main * cinst, struct nvmeibc_target_nics_query* query);

//if should send - will return non null pointer to the target
struct nvmeibc_target* nvmeibc_target_should_send_nics_query(const struct nvmeibc_cinst_params_main * cinst, struct nvmeibc_target_nics_query* query);
void nvmeibc_target_nics_query_was_sent(struct nvmeibc_target* target);

// Destroy target object
void nvmeibc_target_remove_disk_id(struct nvmeibc_disk_id *disk_id);

void nvmeibc_targets_list_to_string(   const struct nvmeibc_cinst_params_main *);
int  nvmeibc_target_init_arnics_league(const struct nvmeibc_cinst_params_main *);
void nvmeibc_target_free_arnic_league( const struct nvmeibc_cinst_params_main *);
void nvmeibc_target_arnic_new_conn(    struct nvmeibc_admin_rnic *arnic);
void nvmeibc_target_arnic_close_conn(  struct nvmeibc_admin_rnic *arnic);

#endif

