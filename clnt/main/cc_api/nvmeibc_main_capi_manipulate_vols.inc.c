/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "main/cc_api/nvmeibc_main_capi_manipulate_vols.h"
#include "main/cc_api/nvmeibc_main_capi_parse_conf.h"
#include "block/nvmeibc_block_common.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_main_capi_manipulate_vols_inc_c

/* Volume work structure (Attach/Detach/Update Works) */
struct avolume_workq {				/* Volume command work */
	struct workqe_struct work;
	struct nvmeibc_multi_completion on_finish;	// Detach: of 1 or more volumes, completion to wakeup whoever is waiting. Can be used for MTV attach (currently unnecessary)
	const struct nvmeibc_cinst_params_main *p;	// Which client instance performs this work
	union {
		const struct nvmeib_mgmt_to_client_volume_configuration *m;	/* Attach: arrive from MCS message, must be freed after processing of message is done */
		const char *uuids;											/* MTV Attach: storing all volumes UUIDs to verify all are attached before creating the MTV */
	};
	int n_volumes;						// Needed to go over uuids and build the MTV
	union {
		bool update_only;				// Attach/Update Work: if true only update volume configuration
		bool is_upgrade;				// Detach Work: If true - abandon volume for future upgrade
		bool mtv_hidden;				// MTV Attach; If the attach is hidden we don't need to expose the MTV
	};
	int rv;
};

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	#define is_sim() (true)
#else
	#define is_sim() (false)
#endif

#define reply_detach_error(err) nvmeibc_cc_api_reply_vol_cmd_status(p, &volume->hdr, err, NVMEIBC_IO_PERM_USE_CURR_PERMS, send_to_cli, send_to_mcs, 0 /* inc_report_id_if_needed */)
int try_detach_volume_with_multicomplete(struct nvmeibc_volume *volume, const struct nvmeibc_vol_detach_cmd how, struct nvmeibc_multi_completion* on_finish)
{
	const struct nvmeibc_cinst_params_main *p = volume->p;
	const bool send_to_cli = (is_sim() || nvmeibc_block_is_recoverer_or_hidden(&volume->hdr) || nvmeibc_vol_detach_recoverer_or_hidden(&how));
	const bool send_to_mcs = true; // Always update MGMT with detach info, update CLI only if hidden (or simulator, TODO - replace cli parsing with MCS response for simulator)
	int rv = 0;
	_NI(i_00_main_det, "@DEV_NAME: status=@STATUS, how{recovery=@BOOL_YN, hidden=@BOOL_YN, force=@BOOL_YN, abandon=@BOOL_YN} on_finish=@PTR", volume->hdr.devname, volume->status, how.recov, how.hidden, how.force, how.abandon, on_finish);
	// Dont allow to start another detach when already detaching (avoid race with the ongoing detach)
	if (volume->status >= NVS_DETACHING) {
		if (!volume->job_comp) { 			// Detach is running and no-one waiting. We will start waiting
			volume->job_comp = on_finish;
		} else {
			rv = -EMLINK;
			nvmeibc_multi_completion_done(on_finish);
		}
		_NT(t_01_main_det, "@DEV_NAME_FULL: is already dettaching, new detach is ignored, rv=@RV", volume->full_name, rv);
		goto _out;
	}

	rv = nvmeibc_volume_try_detach(volume, how, on_finish);
	if (rv != 0) {
		switch (rv) {
		case -EBUSY:				// Busy, may need to retry
			reply_detach_error(NVMEIB_C_TO_M_VOLUME_ACK_BUSY);
			_NE_to_user(t_03_main_det, DMESG_PREFIX("@DEV_NAME"), "Failed to detach volume as it is busy, retry later after there are no processes holding handles to the volume, more information can be found in /proc/@STR/volumes/@DEV_NAME/@STR. Error code: 1048.", volume->hdr.devname, p->proc_dir_root_name, volume->hdr.devname, "client_processes");
			break;
		case -ECHILD:				// Busy, should not retry
			reply_detach_error(NVMEIB_C_TO_M_VOLUME_ACK_BUSY);
			_NT(t_07_main_det, DMESG_PREFIX("@DEV_NAME: ") "volume has rider, force detach ignored. rv=@RV", volume->hdr.devname, rv);
			break;
		case -EXDEV:				// Busy should not retry
			reply_detach_error(NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED);
			_NT(t_06_main_det, "volume @DEV_NAME is fully attached, hidden detach ignored", volume->hdr.devname);
			break;
		default:
			break;
		} /* switch (rv) */
		nvmeibc_multi_completion_done(on_finish);
	} else { // detach already started. 'volume' gets kfree()
		// Completion of (on_finish) will occur at the end of detach
	}
_out:
	return rv;
}

// Verifies reservation information is valid for volume configuration
bool nvmeibc_warn_on_mgmt_wrong_msg_logic = true;
static int __verify_reservation_version_correctness(struct nvmeibc_volume_conf *msg, const struct nvmeibc_volume *volume, const struct nvmeibc_cinst_params_main* p, bool *resrv_inc_ignored) {
	const enum nvmeibc_inst_state state = __get_from_params_main_globals_container(p)->priv_sched.state;
	int rv = 0;
	if (state != NVMEIBC_INST_STATE_READY) {
		_NT(t_f1_vvc, "Mgmt command rejected, state=@INT", state);
		rv = -10;
	} else if (volume != NULL) {
		const char *name = volume->hdr.devname;
		const struct nvmeibc_reservation new_vat = msg->reservation;
		struct nvmeibc_volume_attach_t vol_vat;
		nvmeibc_volume_attach_t_copy(&vol_vat, &volume->hdr.vat);
		if (vol_vat.res.mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC) {
			if (vol_vat.res.version != RESERVATION_MODE_IRRELEVANT) {
				_NT(t_f5_vvc, "mgmt bug: reservation mode version should be irrelevant");
			}
		} else if (vol_vat.res.version == new_vat.version) {
			if (vol_vat.res.mode != new_vat.mode) {	// Reservation mode cannot change without version going up
				_NT(nvmeibc_warn_on_mgmt_wrong_msg_logic1, "mgmt bug: reserv ver=@LONGLONGX, clnt_mode=@INT, mgmt_mode=@INT\n", new_vat.version, vol_vat.res.mode, new_vat.mode);
				rv = -1;
			} else if ((new_vat.preempt == NVMEIB_C_TO_M_VOLUME_PREEMPT) && (new_vat.preempt != vol_vat.res.preempt)) {// preempt cannot change without version going up
				_NT(nvmeibc_warn_on_mgmt_wrong_msg_logic2, "mgmt bug: clnt_preempt=@UINT, mgmt_preempt=@UINT, no increase in reserv ver=@LONGLONGX, mgmt_mode=@INT\n", vol_vat.res.preempt, new_vat.preempt, new_vat.version, new_vat.mode);
				rv = -2;
			}
		} else if (vol_vat.res.version > new_vat.version) {
			if (new_vat.mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC) {
				_NT(t_f4_vvc, "@DEV_NAME: NVMESH-276 - Recovery volume configuration arrived, but volume is already attached as visible. Rejecting configuration.", name);
			} else {
				_NT(nvmeibc_warn_on_mgmt_wrong_msg_logic3, "@DEV_NAME: mgmt bug: reserv ver went down @LONGLONGX -> @LONGLONGX\n", name, vol_vat.res.version, new_vat.version);
			}
			rv = -3;
		} else {	// Reservation version went up,
			// Illegal to use reservetion verson an dissue IO. volume will be preempted. Must detach and reattach to keep user-space cache intact, However to allow recoveries, do update configuration
			if ((vol_vat.res.version + 1) == new_vat.version) {	// If we deside to allow remounting of client (change reservation, this must be inside this clase)
				const char *my_host_name = nvmeibc_get_utsname_nodename(p);
				if (!strncmp(new_vat.reservedBy, my_host_name, sizeof(new_vat.reservedBy)) && (new_vat.mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX)) {
					// Change reservation to exclusive, may allow this in future
				}
			}
			*resrv_inc_ignored = true; // rv = -4; This is not an error, do not reject configuration as it is needed for recovery, even though IO will be preempted.
			_NT(t_f3_vvc, "@DEV_NAME: Overriding higher reservation version my_@RES_MOD_VER < new_@RES_MOD_VER", name, vol_vat.res.version, new_vat.version);
			msg->reservation = vol_vat.res; // Override full configuration reservation info, as if new reservation version was never received. Client will get it from Toma
		}
	}
	if (rv) {
		_NT(t_f2_vvc, "Mgmt command rejected, reason=@RV", rv);
	}
	return rv;
}

static inline bool change_incomming_msg_and_find(const struct nvmeibc_volume *volume, struct nvmeibc_volume_conf *new_hdr, bool update_only)
{
	if (update_only && volume) {
		_NT(t_cimaf01, "Update only forces type: @HDR_TYPE <-- @HDR_TYPE", new_hdr->type, volume->hdr.type);
		new_hdr->type = volume->hdr.type;		// Type change is not allowed!
	}
	if (nvmeibc_block_is_recoverer_or_hidden(new_hdr)) { 	// Both hidden and recoverer do not use any reservation info
		_NT(t_cimaf02, DMESG_PREFIX("@DEV_NAME: ") "Removing attach_t info of hidden volume", new_hdr->name);
		if (volume && !nvmeibc_block_is_recoverer_or_hidden(&volume->hdr)) { // Issue NVMESH-276
			// Race condition: Mgmt/Cli wanted to hidden attach (volume) did not exist then, but meanwhile it was attached. This is not a bug. Just return success
			// WARN, "Invalid volume type transition %x <-- %x\n", new_hdr->type, volume->hdr.type);	// Cannot convert visible to hidden
		}
		nvmeibc_volume_attach_t_init((struct nvmeibc_volume_attach_t *)&new_hdr->reservation);
	}
	return (volume != NULL);
}

static inline const char* __action(const bool found) { return (found) ? "update" : "attach"; }
static enum_vol_status _calc_reply_on_attach(const char *vol_name, const struct nvmeibc_volume *volume, const int rv, const bool resrv_inc_ignored, struct nvmeibc_volume_header *reply_hdr)
{
	const bool found = (volume != NULL);
	if (found) { // Volume existed before attach request. Merge its lates information to into the reply
		nvmeibc_volume_attach_t_copy(&reply_hdr->vat, &volume->hdr.vat);
		reply_hdr->last_sent_io_perm = volume->hdr.last_sent_io_perm;
	}
	if (resrv_inc_ignored)
		return NVMEIB_C_TO_M_VOLUME_RESERVATION_DENIED;
	if (rv < 0) {
		if (rv == -ERROR_VOL_UPDATE_ALREADY_LATEST) {
			_NT(t_tsbd04, DMESG_PREFIX("@DEV_NAME") ": command @STR succeded (no-op), already existing config", vol_name, __action(found));
			return NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED; //NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED_NOTHING_TO_UPDATE;
		}
		_NE(t_tsbd03, DMESG_PREFIX("@DEV_NAME") ": command @STR failed, rv=@RV", vol_name, __action(found), rv);
		return found ? NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_FAILED : NVMEIB_C_TO_M_VOLUME_ACK_ATTACH_FAILED;
	} else {
		return NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED;
	}
}

struct update_targets_nics_args {
	const struct nvmeibc_cinst_params_main *cinst;
	struct nvmeib_mgmt_to_client_update_targets_nics* conf;
};

/* This function may launch async actions. When all of them complete the completion will be set*/
static int try_setup_block_device(const struct nvmeibc_cinst_params_main* p, const struct nvmeib_mgmt_to_client_volume_configuration *msg, const bool update_only)
{
	struct nvmeibc_volume_header reply_hdr = {0};
	const struct nvmeibc_volume_conf *hdr = &msg->volumes[0];
	const struct nvmeibc_volume *volume = nvmeibc_volume_get_by_uuid(p, hdr->uuid, UNKNOWN_ILLEGAL);
	const bool found = change_incomming_msg_and_find(volume, &msg->volumes[0], update_only);
	const bool is_explicit_hidden_attach = (nvmeibc_block_is_recoverer_or_hidden(hdr) && update_only);	// If Toma requests hidden attach via cli, it must get a reply via cli
	const bool send_to_cli = (is_sim() || is_explicit_hidden_attach), send_to_mcs = true; // Update CLI except when full config (block simulator requires CLI updates, until parsing MCS replaces CLI parse)
	bool resrv_inc_ignored = false;
	enum_vol_status res;
	struct nvmeibc_control_api* ccapi = &__get_from_params_main_globals_container(p)->cc_api;
	
	int rv = 0;

	NFIN;
	// NVMESH-4837: msg->attachmentsVersion -can be updated here and tested here
		update_processing_multi_vol_cmd(
			p,
			msg->expect_more_cmd_with_identical_av ? true : false);

	if (!update_only) {
		const bool is_recoverer = nvmeibc_block_is_recoverer(hdr);
		if (!is_recoverer && volume) {	// Recoverer configuration attachmentsVersion is not valid, do not check
			const int version_delta = msg->attachmentsVersion - volume->hdr.attachment_version;

			_NT(t_tsbd00, DMESG_PREFIX("@DEV_NAME") ": Setting up / updating block device attachment versions(mcs=@INT, volume=@INT)", hdr->name, msg->attachmentsVersion, volume->hdr.attachment_version);
			if (version_delta < 0) {    // Old message, ignore, do not reply
				_NW(t_tsbd01, DMESG_PREFIX("@DEV_NAME") ": Setting up / updating block device with old attachment version @INT (current @INT), ignoring", hdr->name, msg->attachmentsVersion, volume->hdr.attachment_version);
				rv = -EINVAL;
				goto _out;
			}
		}
	}

	nvmeibc_volume_header_create_from_msg(&reply_hdr, hdr, msg->attachmentsVersion, false);	// Reply info is taken from the request, no need to print
	{	// Print the incomming cmd - reservation info now in reply_hdr
		char vat_str[128];
		struct nvmeib_txt txt = nvmeib_txt_make((struct charvec){.base = vat_str, .len = sizeof(vat_str)});
		nvmeibc_volume_attach_t_tostring(&reply_hdr.vat, &txt);
		_NI(i_tsbd02, "volume @DEV_NAME @HDR_UUID @C_VOL_VER - got command @STR. @STR.", hdr->name, hdr->uuid, hdr->version, __action(found), vat_str);
		_NT(t_tsbd08, "referenceIDs=@INT", hdr->attachment.n_ref_ids);
	}
	if ((msg->updateType != UPDATETYPE_TOMA_VOL_CONFIG_TO_LOCAL_CLNT &&
	     __verify_reservation_version_correctness(&msg->volumes[0], volume, p, &resrv_inc_ignored))) { // Verify reservation info, if fails send current volume information
		res = _calc_reply_on_attach(hdr->name, volume, -1, resrv_inc_ignored, &reply_hdr);
	} else if (update_only && !found) {
		_NE(t_tsbd05, DMESG_PREFIX("@DEV_NAME") ": not found in full-configuration message, skipping update", hdr->name);
                nvmeibc_cc_api_reply_vol_cmd_status(p, &reply_hdr, NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_FAILED, NVMEIBC_IO_PERM_USE_CURR_PERMS, send_to_cli, send_to_mcs, 0);
		goto _out;
	} else {
		rv = nvmeibc_volume_attach(p, msg, ccapi->io_pet_controller);
		res = _calc_reply_on_attach(hdr->name, volume, rv, resrv_inc_ignored, &reply_hdr);
		if (rv == -ERROR_VOL_UPDATE_ALREADY_LATEST)
			rv = 0;
	}
	if (volume) {	// This was actually an instruction to update volume, We cannot take the latest info from volume. As reply must be based on the latest config
		// Except for attachment section. Yet another hack of the protocol: NVMESH-4837. Here we must take the volume values as maybe volume did not agree to update ref_ids
		nvmeibc_volume_header_destroy(&reply_hdr, NVMEIBC_VOLUME_HEADER_DESTROY_REF_IDS);
		reply_hdr.ext_blob = volume->hdr.ext_blob;
		nvmeibc_cc_api_reply_vol_cmd_status(p, &reply_hdr, res, NVMEIBC_IO_PERM_USE_CURR_PERMS, send_to_cli, send_to_mcs, 1 /* inc_report_id_if_needed */);
		nvmeibc_volume_header_init_0(&reply_hdr);
	} else {
		// Hack  NVMESH-4837. Assumption that on successfull attach volume took the same ref_ids as on stack reply_hdr
		// Consider instead of the above if, just do: volume = nvmeibc_volume_get_by_uuid(p, hdr->uuid, UNKNOWN_ILLEGAL); and use it reagrless of update/attach
		nvmeibc_cc_api_reply_vol_cmd_status(p, &reply_hdr, res, NVMEIBC_IO_PERM_USE_CURR_PERMS, send_to_cli, send_to_mcs, 1 /* inc_report_id_if_needed */);
	}
_out:
	if (rv != -EBUSY) { // Volume update failed due to previous update running, reschedule again on mainwq
		nvmeibc_cc_api_free_config_msg((void *)msg);
	}
	nvmeibc_volume_header_destroy(&reply_hdr, NVMEIBC_VOLUME_HEADER_DESTROY_TOTAL);
	NFOUT;
	return rv;
}

static void setup_block_device_work(struct workqe_struct *_w)
{
	struct avolume_workq *w = container_of(_w, struct avolume_workq, work);
	NFIN;
	if (w->m) {
		w->rv = try_setup_block_device(w->p, w->m, w->update_only);
		WARN_ON(w->rv == -EBUSY);	// Can no longer happen
		kfree(w->m);
	} else {
		_NE(error_main_setup_block_device_work, DMESG_PREFIX() ": missing config argument. Attach will fail");
		kfree(w->m); // This is not necessary
		w->rv = -EINVAL;
	}
	kfree(w);
	w = NULL; /* Waring: w is already invalid here, got free(). Don't use it */
	NFOUT;
}

static int update_targets_nics_work(void* args_)
{
	int i = 0;
	struct update_targets_nics_args* args = args_;
	struct nvmeib_mgmt_to_client_update_targets_nics* conf = args->conf;

	NFIN;
	for(i = 0; i < conf-> n_targets; ++i){
		nvmeibc_target_update(args->cinst, &conf->targets[i]);
	}

	nvmeibc_cc_api_free_update_targets_nics(args->conf);
	kfree(args);
	NFOUT;
	return 0;
}

int setup_block_device_generic(
	const struct nvmeibc_cinst_params_main *p,
	const struct nvmeib_mgmt_to_client_volume_configuration *m,
	const bool update_only)
{
	struct avolume_workq *work = kzalloc(sizeof(*work), GFP_KERNEL);
	struct t_main_clnt_globals *mg =__get_from_params_main_globals_container(p);
	int rv = -1;

	if (!work) {
		_NE(t_52_cmain, DMESG_PREFIX() ": Fail to add work - no memory");
		return rv;
	}

	if (t_main_clnt_priv_sched_bdev_manipulation_start(&mg->priv_sched) < 0){
		kfree(work);
		goto out;
	}

	WQ_INIT_WORK(&work->work, setup_block_device_work);
	work->p = p;
	work->m = m;
	work->update_only = update_only;
	work->rv = 0;
	if ((rv = nvmeibc_add_work(p, &work->work)) < 0) {
		_NE(t_51_cmain, DMESG_PREFIX() ": Fail to add work rv=@RV", rv);
		kfree(work);
	}
out:
	t_main_clnt_priv_sched_bdev_manipulation_end(&mg->priv_sched);
	return rv;
}

int update_targets_nics_generic(const struct nvmeibc_cinst_params_main *cinst,
		struct nvmeib_mgmt_to_client_update_targets_nics *msg)
{
	int rv = 0;
	struct update_targets_nics_args* args = kzalloc(sizeof(*args), GFP_KERNEL);
	if (!args){
		_NE(update_targets_nics_1, "update targets nics failed - ENOMEM");
		return -ENOMEM;
	}

	args->cinst = cinst;
	args->conf = msg;

	rv = nvmeibc_run_on_main_wq1(cinst, update_targets_nics_work, args);
	if (rv) {
		_NE(update_targets_nics_generic, "update_targets_nics_generic - failed workqueue(@RV)", rv);
		kfree(args);
	}
	return rv;
}

static void __detach_all_volumes_of_inst_work(struct workqe_struct *_w)
{
	struct avolume_workq *w = container_of(_w, struct avolume_workq, work);
	int num_devs = nvmeibc_get_all_volumes_num(w->p);
	bool should_retry = false;
	//enum nvmeibc_mod_state state = nvmeibc_get_state();
	// Detach all either for upgrade or just shutdown (shutdown is not reported to mgmt)
	const struct nvmeibc_vol_detach_cmd how = (w->is_upgrade) ? nvmeibc_vol_detach_cmd_upgrade() : nvmeibc_vol_detach_cmd_shutdown();
	struct nvmeibc_volume *volume, *tvolume;
	nvmeibc_assert_on_main_wq(w->p);
	nvmeibc_multi_completion_add_aux_jobs(&w->on_finish, num_devs);
	_NI(i_01_main_davw, "Instance @STR, Starting to detach all: @INT volumes", w->p->proc_dir_root_name, num_devs);
	list_for_each_entry_safe(volume, tvolume, nvmeibc_get_mt_volumes(w->p), link) {
		if (try_detach_volume_with_multicomplete(volume, how, &w->on_finish) == -EBUSY)
			should_retry = true;
	}
	list_for_each_entry_safe(volume, tvolume, nvmeibc_get_volumes(w->p), link) {
		if (try_detach_volume_with_multicomplete(volume, how, &w->on_finish) == -EBUSY)
			should_retry = true;
	}
	if (should_retry) {
		/*if ((rv = nvmeibc_add_work(p, _w)) < 0) */ {
			WARN(1, "nvmeibc bug: unhandled error!\n"); 		// Daniel, todo: enable this 'if' (do it only after we have a unitests which reproduces the problem)
		} // else goto _out;
	}
	nvmeibc_multi_completion_done(&w->on_finish);
}

void shut_down_detach_all_remainig_volumes_of_inst(const struct nvmeibc_cinst_params_main *p, bool is_nvmeibc_upgrade)
{
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(p);
	struct avolume_workq work;
	int rv;

	memset(&work, 0, (sizeof(work)));
	work.p = p;
	work.is_upgrade = is_nvmeibc_upgrade;
	nvmeibc_assert_on_module_wq();
	if (_mg->priv_sched.state == NVMEIBC_INST_STATE_RM_RDY) {
		_NT(t_12_main_davw, "skipped already done work");
		goto _out;
	}
	t_main_clnt_priv_sched_set(_mg, NVMEIBC_INST_STATE_PREP_RM);   // Prevent new attaches
	// Block until all volumes are detached
	WQ_INIT_WORK(&work.work, __detach_all_volumes_of_inst_work);
	nvmeibc_multi_completion_init(&work.on_finish);
	if ((rv = nvmeibc_add_work(p, &work.work)) < 0) {
		_NE(t_11_main_davw, DMESG_PREFIX() ": Fail to add work rv=@RV", rv);
		goto _out;
	}
	nvmeibc_multi_completion_wait_for(&work.on_finish);
	t_main_clnt_priv_sched_set(_mg, NVMEIBC_INST_STATE_RM_RDY); 	// No more volumes exist
_out:;
}


/************************** Attach flags **************************************/
void nvmeibc_volume_attach_t_init(struct nvmeibc_volume_attach_t *vat) {
	vat->res.version = RESERVATION_MODE_IRRELEVANT;		// Special reserved version for recoverer
	vat->res.mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC;
	vat->res.preempt = NVMEIB_C_TO_M_VOLUME_PREEMPT_UNKNOWN;
	vat->res.is_512B_IO_allowed = false;
	memset(vat->res.reservedBy, 0, sizeof(vat->res.reservedBy));
}

void nvmeibc_volume_attach_t_init_from_conf(struct nvmeibc_volume_attach_t *vat, const struct nvmeibc_volume_conf *conf)
{
	vat->res = conf->reservation;
	// vat->res.reservedBy[0] = 0;	// DHSH Todo: consider ignorring this as once attached, volume does not care anymore about this field
}

void nvmeibc_volume_attach_t_copy(struct nvmeibc_volume_attach_t *vat, const struct nvmeibc_volume_attach_t *src)
{
	if (src)
		*vat = *src;
	else
		nvmeibc_volume_attach_t_init(vat);
}

bool nvmeibc_volume_attach_t_are_equal(const struct nvmeibc_volume_attach_t *v1, const struct nvmeibc_volume_attach_t *v2)
{
	return (memcmp(v1, v2, sizeof(*v1)) == 0);
}

void nvmeibc_volume_attach_t_tostring(const struct nvmeibc_volume_attach_t *_vat, struct nvmeib_txt *txt) {
	struct nvmeibc_volume_attach_t vat;
	nvmeibc_volume_attach_t_copy(&vat, _vat);		// Deal with cases of vat == NULL;
	{
		const char* mode_str = nvmeibc_volume_attach_t_mode_to_string(vat.res.mode);
		const char* preempt_str = nvmeibc_volume_attach_t_preempt_to_string(vat.res.preempt);
		nvmeib_txt_append(txt, "Reservation Mode: {%s, version=0x%llx, preempt=%s, rby=%.*s}", mode_str, vat.res.version, preempt_str, (int)sizeof(vat.res.reservedBy), vat.res.reservedBy);
	}
}

char *nvmeibc_volume_attach_t_mode_to_string(enum_reservation_mode mode) {
	switch (mode) {
	case NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO: return "SHARED_READ_ONLY";
	case NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RW: return "SHARED_READ_WRITE";
	case NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX: return "EXCLUSIVE_READ_WRITE";
	case NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC: return "Recovery Only";
	default:									 return "Unknown";
	}
}

char *nvmeibc_volume_attach_t_preempt_to_string(enum_preempt_status preempt) {
	switch (preempt) {
	case NVMEIB_C_TO_M_VOLUME_NO_PREEMPT:	   return "No";
	case NVMEIB_C_TO_M_VOLUME_WEAK_PREEMPT:	   return "Weak";
	case NVMEIB_C_TO_M_VOLUME_PREEMPT:		   return "Yes";
	case NVMEIB_C_TO_M_VOLUME_PREEMPT_UNKNOWN: return "Unknown";
	default:								   return "";
	}
}

#pragma pop_macro("__FILE_LITERAL__")
