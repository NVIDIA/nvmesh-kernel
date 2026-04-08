/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_CLI_SCRIPTS_SIMU_H
#define NVMEIBC_CLI_SCRIPTS_SIMU_H
/* Implementation of various client side cli scripts (attach/detach/cli ioctls): */
#include "./nvmeib_common_all.h"						// Must be first include
#include "./../mgmt/nvmeibm_conf_db.h"
#include "nvmeibc_cc_api.h"

/* This object will get expected CLI responses from clnt, up to 1 per volume
   and expect the exact status. For example we can wait for IO enabled,
   which is a second CLI response on attach.
   It can only be initialized when the counter reaches 0 so we can't override
   previous expectations
   Once an exact status is found a multi completion counter is decreased, and
   when reaches zero the wait function will continue */
#define MAX_VOL_INFO_STRING (158)
struct cli_status_verification {
	struct nvmeibc_multi_completion comp;
	char compare_strings[MAX_VOLUMES_IN_NVMESH][MAX_VOL_INFO_STRING];
	bool bug_on_mismatch;
	spinlock_t lock;
	int err;
	const struct c_api_proc *cli;					// Link to the /proc/nvmeibc/cli/cli of client to send msgs to client
};
void cli_status_ver_init(          struct cli_status_verification *csv);
void cli_status_ver_reset(         struct cli_status_verification *csv);				// Reset between each time we set the multicompletion. Since we must be exact we will not reset if we try to reset before the counter is exactly zero Allowing to set an expectation that is not "normal"
void cli_status_ver_set_err(       struct cli_status_verification *csv, enum NVMEIBC_CLI_ERROR_TYPES err);	// Set an error expector msg from client
void cli_status_ver_set_expector(  struct cli_status_verification *csv, const char *clnt_msg, const int vol_index, bool bug_on_mismatch); // Set a single expector (allocated string) for each volume
void cli_status_ver_match_expector(struct cli_status_verification *csv, const char *clnt_msg);	// Remove expector (compare incomming clnt msg with expectors of all volumes). If found - dec multicompletion. Allowed with empty string to reduce the initial value and allow waiting
void cli_status_ver_wait_for(      struct cli_status_verification *csv);	// After resetting the cli status object needs to be called to ensure all expectors have been met

int  cli_status_ver_incoming_msg_from_clnt_cb(void* csv, const char *buf, size_t len);	// Accepts all strings sent to cli by clnt and enforce expctations. Will centralize the expected CLI replies and allow blk_unitest run to wait until all async calls are done (volume reached expcted status)
void cli_send_command_to_clnt(struct cli_status_verification *csv, char *cmd);

/**************** Assist methods to prepare cli expected strings **************/
#define CLI_BUSY			  "Busy"          // Returned on safe detach request (when volume is still in use). Detach not started. Volume functions normally
#define CLI_DETACHED		  "Detached"      // Was attached, detach completed (safe or unsafe)
#define CLI_SHUTDOWN		  "Shutdown"      // Was attached, detach completed due to shutdown, next client start will request it's configuration from persistency
#define CLI_DETACH_FAILED 	  "DetachFailed"  // Detach was started (safe or unsafe) but failed due to some problem. Very rare (typically result of a bug). Volume is in problematic undefined state (IO might be disabled and even failed immediately).
#define CLI_ATTACHED		  "Attached"      // Volume is OK
#define CLI_ATTACH_FAILED 	  "AttachFailed"  // Attempt to attach failed. All traces of the attempted attach are deleted. Client forgets this volume
#define CLI_UPDATE_FAILED 	  "UpdateFailed"  // Attached volume could not be updated with latest configuration. Volume is OK but is stuck in previous configuration) - might eventually lead to IO disable, when Tomas move to new configuration
#define CLI_UPDATE_READY 	  "UpdateReady"   // Attached volume has been detached and passed to ATOM to be re-taken after upgrade
#define CLI_RV_DENIED	      "ReservationDenied"		// Attach volume request reservation version is older than in DB
#define CLI_RV_MODE_DENIED    "ReservationModeDenied"	// Attach volume request reservation mode is not allowed
#define CLI_UNKNOWN			  "Unknown"       // Request to detach a volume which is not attached. In this case client does not even now anything about this volume (including its UUID).

#define CLI_MSG_FORMAT "status=%s version=%d io_blocked=%s uuid=%s name=%s rv=%llu"
char *cli_generic_string(  const struct volumeDescriptor *vol, const char* status, bool is_io_blocked);
char *detach_string(       const struct volumeDescriptor *vol, const bool uuid, const bool attached); // Called from get_volumes_configuration
char *shutdown_string(     const struct volumeDescriptor *vol, const bool uuid, const bool attached, const bool is_io_blocked); // When shutting down we notify mgmt
char *update_ready_string( const struct volumeDescriptor *vol, const bool uuid, const bool attached, const bool is_io_blocked); // When we do detach --upgrade or --shutdown --upgrade
char *invalid_token_status(const struct volumeDescriptor *vol); // Reply with a detached status on invalid tokens
char *failed_attach_string(const struct volumeDescriptor *vol, const char *status);
char *failed_update_non_exis_vol(const struct volumeDescriptor *vol, const char *status);
char *fail_recovery_attach_string(const struct volumeDescriptor *vol);
char *attach_string(       const struct volumeDescriptor *vol);
char *update_string(       const struct volumeDescriptor *vol);
char *update_string_reject_reserv(const struct volumeDescriptor *vol);


char *attach_string_no_io( const struct volumeDescriptor *vol);
char *detach_recov_string(const struct volumeDescriptor *vol, const bool recoverer_attached);
char *recovery_attach_string(const struct volumeDescriptor *vol, const bool already_attached);
char *cli_unknown_string(  const char *idnet, const bool by_uuid);
char *attach_string_rv(              const struct volumeDescriptor *vol, const char* status, const u64 reservation_version);
char *reservation_mode_denied_string(const struct volumeDescriptor *vol, const bool is_uuid);
char *reservation_denied_string(     const struct volumeDescriptor *vol, const bool is_uuid);

#endif // NVMEIBC_CLI_SCRIPTS_SIMU_H
