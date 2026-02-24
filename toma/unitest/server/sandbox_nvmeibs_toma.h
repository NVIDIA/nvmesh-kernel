#pragma once
/* Emulation of nvmeibs (server) module */

struct nvmeibs_toma_server_proc_buf; struct nvmeibt_host_name;
#include "interfaces/srvr/nvmeibt_srvr_proc.h"
#include "common/nvmeib_shared.h"
#include "srv/nvmeibs_srv_toma_messages.h"		// For nvmeib_nl_uk_comm_msg, nvmeib_disk_info_reply

#include "../os/os_internal.h"
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
};


struct nvmeibs_simulator *sandbox_server_init(struct TSB_netlink_mock *nl);
void sandbox_server_destroy(struct nvmeibs_simulator *s);