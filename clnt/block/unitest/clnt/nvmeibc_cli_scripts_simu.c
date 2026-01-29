/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

// For documentation, see Header in H file
/*****************************************************************************/
// Includes
#include "nvmeibc_cli_scripts_simu.h"

/******************************* CLI expectations *****************************/
void cli_status_ver_init(struct cli_status_verification* cli) {
	(*cli) = (struct cli_status_verification){0};
	nvmeibc_multi_completion_init(&cli->comp);
	nvmeibc_multi_completion_done(&cli->comp);// Init in state=done
	spin_lock_init(&cli->lock);
	cli->err = 0;
}

static inline bool is_done(struct cli_status_verification *csv) {
	return completion_done(&csv->comp.done);
};

void cli_status_ver_reset(struct cli_status_verification *csv)
{
	if (!is_done(csv)) {
		_NE_dmesg(error_cli_scripts_simu_cli_status_ver_reset, "CLI status verification: resetting before completion is done ignoring");
		return;
	}
	nvmeibc_multi_completion_init(&csv->comp);
}

void cli_status_ver_set_expector(struct cli_status_verification *csv, const char *compare_string, const int vol_index, bool bug_on_mismatch)
{
	spin_lock(&csv->lock);
	// If the expector is already set ignore this request (will happen every time)
	if (csv->compare_strings[vol_index][0] != '\0') {
		_ND(trace_cli_scripts_simu_cli_status_ver_set_expector, "Expector already set (@COMPARE_STRING) and has not yet been met, cannot set another expector (@COMPARE_STRING) for volume index @VOL_I", csv->compare_strings[vol_index], compare_string, vol_index);
		sim_kfree(compare_string);
		spin_unlock(&csv->lock);
		return;
	}
	// Set the expector
	strcpy(csv->compare_strings[vol_index], compare_string);
	csv->bug_on_mismatch = bug_on_mismatch;
	_ND(trace_1_cli_scripts_simu_cli_status_ver_set_expector, "CLI status verification: Added expected status string @COMPARE_STRING", compare_string);
	sim_kfree(compare_string);
	BUG_ON(is_done(csv));
	nvmeibc_multi_completion_add_aux_jobs(&csv->comp, 1);
	spin_unlock(&csv->lock);
}

void cli_status_ver_match_expector(struct cli_status_verification *csv, const char *compare_string)
{
	//While debugging, one can peek here at the expectors set vs. the actual string
	if (compare_string) { // Search for the string among the expectors
		int i;
		for (i=0;i<MAX_VOLUMES_IN_NVMESH;i++) {
			if (!strcmp(compare_string, csv->compare_strings[i])) {
				_ND(trace_cli_scripts_simu_cli_status_ver_match_expector, "CLI status verification: Found expected string @COMPARE_STRING, removing expector", compare_string);
				memset(csv->compare_strings[i], 0, sizeof(csv->compare_strings[i]));
				break;
			}
		}
		if (i == MAX_VOLUMES_IN_NVMESH) { // Not found do not reduce counter
			//AK: Enable the following to get (a lot of) prints of the expector strings
			if (0) { 					  // Enable to see comparison strings
				for (i = 0; i < MAX_VOLUMES_IN_NVMESH; i++) {
					pr_emerg("Compare string %s, not found among %d: %s\n", compare_string, i, csv->compare_strings[i]);
					_ND(trace_1_cli_scripts_simu_cli_status_ver_match_expector, "Compare string @COMPARE_STRING, not found among @COMPARE_STRING", compare_string, csv->compare_strings[i]);
				}
			}
			BUG_ON(csv->bug_on_mismatch);
			return;
		}
	} else
		_ND(trace_2_cli_scripts_simu_cli_status_ver_match_expector, "CLI status verification: Reducing counter without string comparison");
	BUG_ON(is_done(csv));
	_ND(trace_3_cli_scripts_simu_cli_status_ver_match_expector, "CLI status verification: reduce counter from @COUNTER", atomic_read(&csv->comp.counter));
	nvmeibc_multi_completion_done(&csv->comp);
}

void cli_status_ver_set_err(struct cli_status_verification *csv, const enum NVMEIBC_CLI_ERROR_TYPES err)
{
	if (csv->err != 0) { // If the expector is already set ignore this request (will happen every time)
		_ND(trace_cli_scripts_simu_cli_status_ver_set_err, "ERR Expector already set (@ERR) and has not yet been met, cannot set another expector (@ERR)", csv->err, err);
		return;
	}
	// Set the expector
	csv->err = err;
	_ND(trace_1_cli_scripts_simu_cli_status_ver_set_err, "CLI ERR status verification: Added expected status err @ERR", err);
	BUG_ON(is_done(csv));
	_ND(trace_2_cli_scripts_simu_cli_status_ver_set_err, "CLI ERR status verification: increse counter from @COUNTER", csv->comp.counter.c);
	nvmeibc_multi_completion_add_aux_jobs(&csv->comp, 1);
}

// Remove expector, this function will compare a string with the err expector
// If found it will decrease the multicompletion object by one, if not found
// will not affect the counter.
// Allowed with empty string to reduce the initial value and allow waiting
static void _match_cli_error_expector(struct cli_status_verification *csv, const char *compare_string)
{
	if (compare_string) { // Search for the string among the expectors
		char expecting[16];
		int err_len = snprintf(expecting, 15, "ERR: -%d", csv->err);
		if (!strncmp(compare_string, expecting, err_len)) {
			_ND(error_cli_scripts_simu_match_cli_error_expector, "CLI Error verification: Found expected error @EXPECTING, removing expector", expecting);
			csv->err = 0;
		} else {
			if (csv->err != 0)
				_ND(trace_cli_scripts_simu_match_cli_error_expector, "Comparing err @EXPECTING, not found in @COMPARE_STRING", expecting, compare_string);
			return;
		}
	} else
		_ND(trace_1_cli_scripts_simu_match_cli_error_expector, "CLI ERR status verification: Reducing counter without string comparison");
	BUG_ON(is_done(csv));
	_ND(trace_2_cli_scripts_simu_match_cli_error_expector, "CLI ERR status verification: reduce counter from @COUNTER", csv->comp.counter.c);
	nvmeibc_multi_completion_done(&csv->comp);
}

void cli_status_ver_wait_for(struct cli_status_verification *csv)
{
	nvmeibc_multi_completion_wait_for(&csv->comp);
}

int cli_status_ver_incoming_msg_from_clnt_cb(void* _csv, const char *buf, size_t len) {
	struct cli_status_verification *csv = _csv;
	(void)len;
	spin_lock(&csv->lock);
	if (!is_done(csv)) {			// Ignore these messages completely
		if (strncmp(buf, "ERR:", 4)) { 		// NOT Error expector
			cli_status_ver_match_expector(csv, buf);
		} else {
			_match_cli_error_expector(csv, buf);
		}
		WARN((atomic_read(&csv->comp.counter) < 0), "CLI status verification: gone negative\n");
	}
	spin_unlock(&csv->lock);
	return 0;
}

void cli_send_command_to_clnt(struct cli_status_verification *csv, char *cmd){
	csv->cli->dir->fops->write((void*)csv->cli->msg_loop, cmd, strlen(cmd), NULL);
}

/**************** Assist methods to prepare cli expected strings **************/
#include "../toma/clnt/nvmeibt_client_protocol.h"
u64 calc_expected_reserv_ver(const struct volumeDescriptor *vol) {
	return (vol->vat.res.version > RESERVATION_MODE_IRRELEVANT) ? 1 : RESERVATION_MODE_IRRELEVANT;		// Version is not updated after attach.
}

char *cli_generic_string(const struct volumeDescriptor *vol, const char* status, bool is_hidden) {
	char output[MAX_VOL_INFO_STRING];
	const char* msg_format = CLI_MSG_FORMAT;
	int len = snprintf(output, MAX_VOL_INFO_STRING, msg_format, status, vol->info.version, "false", vol->info.uuid, vol->info.devname, (is_hidden) ? RESERVATION_MODE_IRRELEVANT : vol->vat.res.version);
	BUG_ON(len >= MAX_VOL_INFO_STRING);		// May cause stack corruption
	return sim_kstrdup(output, GFP_KERNEL);
}

char *cli_unknown_string(const char *ident, const bool by_uuid)
{
	char output[MAX_VOL_INFO_STRING];
	const char* msg_format = CLI_MSG_FORMAT;
	const char* uuid_st =  (by_uuid) ? ident : "";
	const char* name_st = (!by_uuid) ? ident : "";
	int len = snprintf(output, MAX_VOL_INFO_STRING, msg_format, CLI_UNKNOWN, -1, "false", uuid_st, name_st, RESERVATION_MODE_IRRELEVANT);
	BUG_ON(len >= MAX_VOL_INFO_STRING);		// May cause stack corruption
	return sim_kstrdup(output, GFP_KERNEL);
}

// A detach command for a not attached volume returns as unknown and depends if uuid or name was used
static char *__failed_string(const struct volumeDescriptor *vol, const char* status, const bool by_uuid)
{
	char output[MAX_VOL_INFO_STRING];
	const char* msg_format = CLI_MSG_FORMAT;
	const char *uuid_st = ( by_uuid) ? vol->info.uuid    : "";
	const char *name_st = (!by_uuid) ? vol->info.devname : "";
	int len = snprintf(output, MAX_VOL_INFO_STRING, msg_format, status, -1, "false", uuid_st, name_st, RESERVATION_MODE_IRRELEVANT);
	BUG_ON(len >= MAX_VOL_INFO_STRING);		// May cause stack corruption
	return sim_kstrdup(output, GFP_KERNEL);
}

static char *__attach_string_status(const struct volumeDescriptor *vol, const char *status, const bool io_blocked, const u64 reservation_version)
{
	char output[MAX_VOL_INFO_STRING];
	char *block_string = (io_blocked) ? "true" : "false";
	const char* msg_format = CLI_MSG_FORMAT;
	int len = snprintf(output, MAX_VOL_INFO_STRING, msg_format, status, vol->info.version, block_string, vol->info.uuid, vol->info.devname, reservation_version);
	BUG_ON(len >= MAX_VOL_INFO_STRING);		// May cause stack corruption
	return sim_kstrdup(output, GFP_KERNEL);
}

char *hidden_attach_string(const struct volumeDescriptor *vol, const bool already_attached)
{
	char output[MAX_VOL_INFO_STRING];
	const char* msg_format = CLI_MSG_FORMAT;
	int len = snprintf(output, MAX_VOL_INFO_STRING, msg_format, CLI_ATTACHED, vol->info.version, (already_attached) ? "false" : "true", vol->info.uuid, vol->info.devname, RESERVATION_MODE_IRRELEVANT);
	BUG_ON(len >= MAX_VOL_INFO_STRING);		// May cause stack corruption
	return sim_kstrdup(output, GFP_KERNEL);
}

static char *__detach_string_failed_status(const struct volumeDescriptor *vol) {
	return __attach_string_status(vol, CLI_DETACH_FAILED, false, (vol->vat.res.mode ? 1 : 0));
}

char *detach_string(const struct volumeDescriptor *vol, const bool uuid, const bool attached) {
	if (!attached) // The detached status doesn't have the entire volume info
		return __failed_string(vol, CLI_UNKNOWN, uuid);
	return cli_generic_string(vol, CLI_DETACHED, false);
}

char *update_ready_string(const struct volumeDescriptor *vol, const bool uuid, const bool attached, const bool is_hidden) {
	if (!attached) // The detached status doesn't have the entire volume info
		return __failed_string(vol, CLI_UNKNOWN, uuid);
	return cli_generic_string(vol, CLI_UPDATE_READY, is_hidden);
}

char *shutdown_string(const struct volumeDescriptor *vol, const bool uuid, const bool attached, const bool is_hidden) {
	if (!attached) // The detached status doesn't have the entire volume info
		return __failed_string(vol, CLI_UNKNOWN, uuid);
	return cli_generic_string(vol, CLI_SHUTDOWN, is_hidden);
}

char *invalid_token_status(const struct volumeDescriptor *vol) {
	return __failed_string(vol, CLI_DETACHED, true);
}

char *failed_attach_string(const struct volumeDescriptor *vol, const char *status) {
	return __attach_string_status(vol, status, (strcmp(status,CLI_UPDATE_FAILED)), calc_expected_reserv_ver(vol));
}

char *failed_update_non_exis_vol(const struct volumeDescriptor *vol, const char *status) {
	return __attach_string_status(vol, status, true, calc_expected_reserv_ver(vol));
}

char *fail_hidattch_string(const struct volumeDescriptor *vol) {
	return __attach_string_status(vol, CLI_ATTACH_FAILED, true, 0);
}

char *attach_string(const struct volumeDescriptor *vol) {
	return __attach_string_status(vol, CLI_ATTACHED, false, calc_expected_reserv_ver(vol));
}

char *detach_hidden_string(const struct volumeDescriptor *vol, const bool hidden_attached, const bool recoverer_attached) {
	(void)recoverer_attached; //note that if the volume can be attached by user and therefore hidden_attached will be false
	if (!hidden_attached)
		return __detach_string_failed_status(vol);
	return cli_generic_string(vol, CLI_DETACHED, true);
}

char *detach_recov_string(const struct volumeDescriptor *vol, const bool hidden_attached, const bool recoverer_attached) {
	(void)hidden_attached;
	if (!recoverer_attached)
		return __detach_string_failed_status(vol);
	return cli_generic_string(vol, CLI_DETACHED, true);
}

char *update_string(const struct volumeDescriptor *vol) {
	return attach_string(vol);
}

char *update_string_reject_reserv(const struct volumeDescriptor *vol) {
	return __attach_string_status(vol, CLI_RV_DENIED, false, calc_expected_reserv_ver(vol));
}

char *attach_string_no_io(const struct volumeDescriptor *vol) {
	return __attach_string_status(vol, CLI_ATTACHED, true, calc_expected_reserv_ver(vol));
}

// When we know we are changing the RV in mgmt we need the current rv+1
// For reservation testing, when the simulator is operating we only use
// RV 1 and mode = RW
char *attach_string_rv(const struct volumeDescriptor *vol, const char* status, const u64 reservation_version) {
	BUG_ON(vol->vat.res.version > reservation_version);
	return __attach_string_status(vol, status, false, reservation_version);
}

char *reservation_denied_string(const struct volumeDescriptor *vol, const bool is_uuid) {
	return __failed_string(vol, CLI_RV_DENIED, is_uuid);
}

char *reservation_mode_denied_string(const struct volumeDescriptor *vol, const bool is_uuid) {
	return __failed_string(vol, CLI_RV_MODE_DENIED, is_uuid);
}

/*****************************************************************************/
// EOF.
