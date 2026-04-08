/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_CC_API_H
#define NVMEIBC_CC_API_H

#include "nvmeibc_mcs_stub.h"
#include "common/pet/nvmeib_pet_specification.h"
// configuration/control communication api including MCS/CLI requirements
// Used by both nvmesh and nvmeshum
#define SUPPORTED_MCS_PROTOCOL_VERSION 1
#define DEFAULT_UPSTREAM_VALUE -1
#define DEFAULT_ATTACHMENT_VERSION 0
#define INCREMENT_IF_NOT_DEFAULT(x) if (DEFAULT_UPSTREAM_VALUE != (x)) (x)++

#define CHECK_SINGLE_MAGIC( t, name) (!strncmp(t, name, sizeof(name)))
#define CHECK_UPDATE__MAGIC(t)  CHECK_SINGLE_MAGIC(t,MAGIC_CONFIG_UPDATE_TOKEN)
#define CHECK_FORCE___MAGIC(t)  CHECK_SINGLE_MAGIC(t,MAGIC_CONFIG_FORCE__TOKEN)
#define CHECK_SHADOW__MAGIC(t)  CHECK_SINGLE_MAGIC(t,MAGIC_CONFIG_SHADOW_TOKEN)
#define CHECK_RECOVER_MAGIC(t)  CHECK_SINGLE_MAGIC(t,MAGIC_RECOVR_ATTACH_TOKEN)

static inline int nvmeibc_cc_api_has_volume_status_changed(enum_vol_status status)
{
	switch (status) {
		case NVMEIB_C_TO_M_VOLUME_ACK_BUSY:
		case NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED:
		case NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED_UNKNOWN_VOLUME:
		case NVMEIB_C_TO_M_VOLUME_ACK_ATTACH_FAILED:
		case NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_FAILED:
		case NVMEIB_C_TO_M_VOLUME_ALIAS_CREATE_FAILED:
		case NVMEIB_C_TO_M_VOLUME_ALIAS_DELETE_FAILED:
			return 0;	// Error, did not change anything
		case NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED_NOTHING_TO_UPDATE:
			return 0;	// Nothing was done, Attach/Update request when volume is already in latest state
			break;
		case NVMEIB_C_TO_M_VOLUME_ACK_DETACHED:
		case NVMEIB_C_TO_M_VOLUME_ACK_SHUTDOWN:
		case NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_READY:
		case NVMEIB_C_TO_M_VOLUME_ALIAS_DELETED:
			return 1;	// Successfull detach
			break;
		case NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED:
		case NVMEIB_C_TO_M_VOLUME_ALIAS_CREATED:
			return 1;	// Successfull attach, or update of volume
			break;
		case NVMEIB_C_TO_M_VOLUME_RESERVATION_DENIED:
		case NVMEIB_C_TO_M_VOLUME_RESERVATION_MODE_DENIED:
			return 1;	// Update of volume conf may/may-not occured successfully, but reservation mode/version rejected
		default:
			return -EINVAL;
	}
}

#if !defined(UM_APP)

struct c_api_bdev_ioctls {				// Dummy struct for future use
	int num_executed_ioctls;			// Comulative amount of issued ioctls
};

struct c_api_perrep {					// Periodic report to management
	struct delayed_work dwork;			// Mechanism for periodic rescheduling
	unsigned long delay_jiffies;		// We schedule the report each second on main wq but only send a report if the (delay_jiffies % management_report_frequency) == 0
	spinlock_t lock;					// Ensuring we can access the delay one by one
};

struct c_api_proc {						// Generic /proc interface
	/*const*/ char* name;				// Name of dir under 'proc_dir'
	struct proc_dir_entry *dir;			// Pointer to the directory
	struct msgloop_procfs_ent *msg_loop;// Communication with user-space app
	void *handle;						// Optional handle to send/rcv messages
};

struct nvmeibc_control_api {
	struct c_api_perrep heartbeat;	 	// Keep alive periodic messages
	struct c_api_perrep full_conf;		// Full conf delayed workqueue
	struct c_api_proc mcs;				// Communication with management
	struct c_api_proc cli;				// Command line interface
	struct nvmeib_pet_base_controller* io_pet_controller;
	struct proc_dir_entry *proc_dir;	// Location where ccapi proc files/msg loops reside (typically /proc/nvmeibc/)
	struct c_api_bdev_ioctls ioctls;	// NVMesh specific ioctls

	long long clnt_2_mgmt_report_id;		// Initialized to DEFAULT_UPSTREAM_VALUE (-1). Set by mgmt with MCS_UPDATE_CLIENT_TOKEN_MSG. Increments whenever volume: is attached/detached/IO enabled for the first time
	long long clnt_2_mgmt_fullconf_token;	// Initialized to DEFAULT_UPSTREAM_VALUE (-1). Set by mgmt with MCS_UPDATE_CLIENT_TOKEN_MSG. Token with which mgmt requests client to report its full configuration
	long long clnt_2_mgmt_sequence_id;		// Initialized to DEFAULT_UPSTREAM_VALUE (-1). Set by mgmt with MCS_UPDATE_CLIENT_TOKEN_MSG. A mono-inc counter for each outbound msg from Client.
	bool processing_multi_vol_cmd; /* true means that a command (only attach & update) with multiple volumes is being processed and not yet finished. */

	bool is_mcs_cache_replayed_completed;    // non zero value means that mcs cache was transmitted.

	int latest_attachment_version;		// Value given by mgmt, client stores max value for each new attachment request, and returns with each message
};
int  nvmeibc_cc_api_create( struct nvmeibc_control_api *capi, struct proc_dir_entry *root_proc_dir);
void nvmeibc_cc_api_destroy(struct nvmeibc_control_api *capi);
void nvmeibc_cc_api_get_n_msgs(const struct nvmeibc_control_api *capi, int *n_mcs, int *n_cli);


int nvmeibc_cc_api_handle_toma_to_local_clnt_msg(struct nvmeib_local_client_params *p);

/* CLI Error types */
enum NVMEIBC_CLI_ERROR_TYPES {	// Possible error codes
	CANCEL_FORMAT_ERROR = 0,    // An error in the cancel format
	CANCEL_MISSING_TOKEN,       // No token given in cancel command
	CANCEL_INVALID_TOKEN,       // Invalid token in cancel command
	CANCEL_TOKEN_NOT_FOUND,     // Cancel token not found
	ATTACH_MISSING_TOKEN,       // Attach command missing token
	ATTACH_INVALID_TOKEN,       // Attach command invalid token
	ATTACH_TOKEN_ALREADY_USED,  // Attach command non unique token
	DETACH_INVALID_FORMAT,      // Can only occur if a send token is found
	INVALID_SEND_TOKEN,         // Generic send token error
	INVALID_IOCTL_FORMAT,       // IOCTL bad format
	INVALID_IOCTL_COMMAND,      // IOCTL bad command
	INVALID_CLI_FORMAT,         // CLI command bad format
	INVALID_CLI_COMMAND,        // Unknown CLI command
	CLI_INVALID_MODIFIER,       // Invalid modifier (not u/U/v/V)
	CLIENT_SHUTTING_DOWN,		// Refuse to accept requests from CLI
	MISSING_RESERVATION_MODE,	// Missing reservation mode
	INVALID_RESERVATION_MODE,	// Reservation mode is invalid
	INVALID_OPTIONAL_FLAGS,		// Invalid optional parameters - can only be either '--preempt' or '--512'
	MISSING_RESERVATION_VERSION,// We require a reservation version in case the client restarted otherwise it's 0
	INVALID_RESERVATION_VERSION,// We require a reservation version to be only digits
	MCS_CLOSE_RETRY,			// The MCS was closed while we were waiting for a reply, resend the attach information
};

#define Nschedule_on_main_wq(name, cc_api, func, params) ({									\
	int		_my_rv_;																		\
	const struct nvmeibc_cinst_params_main *_p = __get_cinst_params_from_cc_api(cc_api);	\
	if ((_my_rv_ = nvmeibc_run_on_main_wq1(_p, func, params)) < 0)							\
		_NT(name, "@STR - Error scheduling @STR on mainwq rv=@RV", __FUNCTION__, #func, _my_rv_);	\
	_my_rv_;																				\
})

#endif /*!defined(UM_APP)*/

struct t_main_clnt_globals;
void set_toma_local_clnt_globals(struct t_main_clnt_globals *p);

#endif /* NVMEIBC_CC_API_H */
