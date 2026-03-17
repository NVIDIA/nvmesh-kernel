/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/* Emulation of nvmeibs (server) module */

#include "srv/nvmeibs_srv_toma_messages.h"		// For nvmeib_nl_uk_comm_msg, nvmeib_disk_info_reply
#include "interfaces/srvr/nvmeibt_srvr_proc.h"
#include "common/nvmeib_shared.h"
#include "../os/os_internal.h"					// Other side of netlink and file descriptors

struct TSB_server_toma_status_req_simu {		// Mechanism for server to request Toma to fill status proc files
	int n_srvr_msg_idx;							// Ever increasing number
	int n_toma_replies_received;
	int expecting_reply_cookie;					// If sent a message to toma and expecting a reply, store it
	int max_reply_length_bytes;
	int n_msgs_to_registrants;
	enum nvmeibs_toma_server_msg_type msg_q[16];
};

struct nvmeibs_simulator {
	struct TSB_fd_otherside com_srvr2toma_o, com_toma2srvr_o, com_toma2clnt_o;		// Toma 3 extern communication fd's via server
	struct TSB_netlink_mock *nl;													// Other side of netlink communication
	struct TSB_server_toma_status_req_simu s_req_simu;
	const struct sandbox_nvme_device *pending_disk_adds[3];								// Pending disk ADD events to be sent to toma. Supports concurrent format operations on different disks.
	int n_pending_disk_adds;
};

// API towards unitest environment
struct nvmeibs_simulator *nvmeibs_simu_init(struct TSB_netlink_mock *nl);
void nvmeibs_simu_destroy(struct nvmeibs_simulator *s, bool do_verify_used);
void nvmeibs_simu_do_periodic(void);
void nvmeibs_simu_send_extended_msg(const char *something);