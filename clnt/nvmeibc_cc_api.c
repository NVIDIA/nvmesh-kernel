/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*********************** Client - Control API *********************************
 API between this struct and nvmeibc_main.c()
 This module will handle all MCS and CLI requests
 The interface with main will mostly be scheduling on main workqueue
 All fill messages and message logic will be here
 Interface will be giving the handlers and init and destroy and all related
 structures */

#include "nvmeib_event.h"
#include "nvmeib_build_info.h"	/* NVMESH_VERSION/RELEASE (generated; see Makefile GEN_BUILD_INFO) */
#include "nvmeibc_block.h"					// Must be first for simulator
#include "nvmeibc_cc_api.h"
#include "main/nvmeibc_main_common.h"
#include "nvmeibc_main.h"					// To use main-wq
#include "nvmeib_mcs.h"
#include "nvmeib_proc_cli.h"
#include "nvmeib_utils.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_msgloop.h"
#include "nvmeibc_volume.h"					// Todo: Remove
#include "tpv/nvmeibc_tpv.h"				/* nvmeibc_tpv_find_by_uuid, nvmeibc_tpv_detach */
#include "atom/nvmeiba_nvmesh_api.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "nvmeib_public.h"
#include "management_utils_common/nvmeibc_management_capi_parse_conf.h"
#include "autogen/clnt/nvmeibc_mcs_stub.h"

/********************* Communication (MCS/CLI) API ***************************/
       int schedule_handle_cli_msg(void *args, char *buf, size_t len, bool *posted);
static int handle_mcs_msg(         void *args, char *buf, size_t len, bool *posted);
static void nvmeib_mcs_init_protocol(void *params);
static void nvmeib_mcs_done_protocol(void *params);

static const int NVMEIBC_CLI_MAX_MSG = 5000;

static inline bool __is_special_token(const char *t)
{
	return CHECK_RECOVER_MAGIC(t)||CHECK_UPDATE__MAGIC(t)||CHECK_FORCE___MAGIC(t) || CHECK_SHADOW__MAGIC(t) ||CHECK_HIDDEN__MAGIC(t);
}

static void __update_msg(struct get_client_configuration_msg *msg,
									   const char *send_token)
{
	if (__is_special_token(send_token)) {
		strlcpy(msg->cli_unique_id, send_token, sizeof(msg->cli_unique_id));
		if (CHECK_RECOVER_MAGIC(send_token)||CHECK_HIDDEN__MAGIC(send_token)) { // For mgmt to ignore reservation mode if hidden/recoverer
			msg->volumes->is_hidden = RECOVERER_VOLUME;
		}
	}
}

/******************************* Control API *********************************/
static void c_api_proc_remove(struct c_api_proc *p, struct proc_dir_entry *root_dir)
{
	if (p->dir) {
		//WARN_ON(root_dir != p->dir->parent);		// For debug in simulator. Note: Can dereference p->dir only in simulator.
		remove_proc_entry(p->name, root_dir);
		p->dir = NULL;
		p->name= NULL;
	}
}

static int nvmeibc_send_to_mcs(struct nvmeibc_control_api* cc_api, void *msg, const char (*unique_uuid)[64], int opcode)
{

	return msg ? nvmeib_mcs_send_proc(cc_api->mcs.handle, msg,
					  unique_uuid, opcode,
					  cc_api->mcs.msg_loop) :
		     -EINVAL;
}

static int nvmeibc_send_to_cli(struct nvmeibc_control_api* cc_api, char *msg)
{
	_ND(trace_cc_api_nvmeibc_send_to_cli, "@MSG_STR", msg);
	return nvmeib_cli_send_proc(cc_api->cli.handle, msg, strlen(msg),
								cc_api->cli.msg_loop);
}

/* Heartbeat preparation, due to management issues, merged with iostats below,
   keeping for ease of later implementation
static void __cc_api_perrep_heartbeat_work(struct work_struct *w)
{
	struct delayed_work *dw = container_of( w, struct delayed_work, work);
	struct c_api_perrep *pr = container_of(dw, struct c_api_perrep, dwork);
	schedule_delayed_work(&pr->dwork, pr->delay_jiffies);
}
*/

#define __verify_on_main_wq_ccapi(cc_api)    nvmeibc_assert_on_main_wq(__get_cinst_params_from_cc_api(cc_api))

#define REQUEST_ALL_VOLUMES "*"	// Special volume name meaning all volumes
static int __schedule_request_config(struct nvmeibc_control_api *cc_api, const char *name, const char* dbg_reason);

#include "main/cc_api/nvmeibc_main_capi_full_conf.h"
#include "main/cc_api/nvmeibc_main_capi_full_conf.inc.c"			// Todo: Remove this

static int __heartbeat_to_mcs_check(void *param);
static int about_a_second(void)
{
	#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
		return HZ;  // In simulator we don't want to use rand in real code cause we want tests to be deteministic.
	#else
		return (HZ-HZ/10) + get_random_u32() % (2*HZ/10);
	#endif
}
static void __cc_api_perrep_iostats_work(struct work_struct *w)
{
	struct delayed_work *dw = container_of( w, struct delayed_work, work);
	struct c_api_perrep *pr = container_of(dw, struct c_api_perrep, dwork);
	struct nvmeibc_control_api *cc_api = container_of(pr, struct nvmeibc_control_api, heartbeat);
	Nschedule_on_main_wq(trace_e1_ccapi, cc_api, __heartbeat_to_mcs_check, (void*)pr);
	schedule_delayed_work(&pr->dwork, about_a_second());
}

static void __heartbeat_create(struct c_api_perrep* _this)
{
	_NT(t_1_cc_api___heartbeat_create, "Creating keepalive heartbeat");
	_this->delay_jiffies = 1;			// start to count each second
	INIT_DELAYED_WORK(&_this->dwork, __cc_api_perrep_iostats_work);
	schedule_delayed_work(&_this->dwork, 0); //start keepalive immediately
}

static void __cli_on_open(void *cli_handle)
{
	struct c_api_proc *cli = container_of(cli_handle, struct c_api_proc, handle);
	_NT(t_1q_cc_api, "/proc/@STR - open()", cli->name);
}

static void __cli_on_close(void *cli_handle)
{
	struct c_api_proc *cli = container_of(cli_handle, struct c_api_proc, handle);
	_NT(t_1r_cc_api, "/proc/@STR - close()", cli->name);
}

int nvmeibc_cc_api_create(struct nvmeibc_control_api *capi, struct proc_dir_entry *root_proc_dir)
{
	int rv = -ENODEV;
	#define PROCFS_MCS_STR "mcs"
	#define PROCFS_CLI_STR "cli"
	// Create /proc entries: mcs + cli
	capi->mcs.name = PROCFS_MCS_STR;
	capi->proc_dir = root_proc_dir;
	if (!(capi->mcs.dir = proc_mkdir(capi->mcs.name, root_proc_dir)))
		goto _out;
	capi->cli.name = PROCFS_CLI_STR;
	if (!(capi->cli.dir = proc_mkdir(capi->cli.name, root_proc_dir)))
		goto _out;

	// Initialize /proc mechanisms: mcs + cli
	capi->clnt_2_mgmt_report_id = DEFAULT_UPSTREAM_VALUE;
	capi->clnt_2_mgmt_fullconf_token = DEFAULT_UPSTREAM_VALUE;
	capi->clnt_2_mgmt_sequence_id = DEFAULT_UPSTREAM_VALUE;

	capi->latest_attachment_version = DEFAULT_ATTACHMENT_VERSION;
	capi->mcs.handle = NVMEIB_MCS_INIT;
	capi->cli.handle = nvmeib_cli_init();
	if ((!capi->mcs.handle)||(!capi->cli.handle))
		goto _out;
	capi->mcs.msg_loop = nvmeib_msgloop_create(
		capi->mcs.name, capi->mcs.dir, &handle_mcs_msg,
		&nvmeib_mcs_init_protocol, &nvmeib_mcs_done_protocol,
		(void *)(&capi->mcs.handle));
	capi->cli.msg_loop =
		nvmeib_msgloop_create(capi->cli.name, capi->cli.dir,
				      &schedule_handle_cli_msg, &__cli_on_open,
				      &__cli_on_close, &capi->cli.handle);
	if ((!capi->mcs.msg_loop)||(!capi->cli.msg_loop))
		goto _out;
	nvmeib_msgloop_set_ready_cb(capi->mcs.msg_loop, &nvmeib_mcs_init_protocol);
	nvmeib_msgloop_set_max(capi->cli.msg_loop, NVMEIBC_CLI_MAX_MSG);
	nvmeib_msgloop_set_max(capi->mcs.msg_loop, NVMEIBC_CLI_MAX_MSG * 10);
	__heartbeat_create(&capi->heartbeat);
	__full_conf_create(&capi->full_conf);
	rv = 0;
_out:
	if (unlikely(rv < 0))
		_NT(error_cc_api_nvmeibc_cc_api_create, "nvmeibc error while initializing control api @RV", rv);
	return rv;
}

void nvmeibc_cc_api_destroy(struct nvmeibc_control_api *capi)
{
	// Destroy Heart-beat and all delayed works
	cancel_delayed_work_sync(&capi->heartbeat.dwork);
	__full_conf_destroy(&capi->full_conf);

	// Destroy mcs + cli communication mechanisms
	if (capi->cli.handle) {
		nvmeib_cli_remove(capi->cli.handle);
		capi->cli.handle = NULL;
	}
	if (capi->cli.msg_loop) {
		nvmeib_msgloop_remove(capi->cli.msg_loop);
		capi->cli.msg_loop = NULL;
	}
	if (capi->mcs.handle) {
		nvmeib_mcs_remove(capi->mcs.handle);
		capi->mcs.handle = NULL;
	}
	if (capi->mcs.msg_loop) {
		nvmeib_msgloop_remove(capi->mcs.msg_loop);
		capi->mcs.msg_loop = NULL;
	}

	// Remove /proc directories
	c_api_proc_remove(&capi->cli, capi->proc_dir);
	c_api_proc_remove(&capi->mcs, capi->proc_dir);
}

void nvmeibc_cc_api_get_n_msgs(const struct nvmeibc_control_api *capi, int *n_mcs, int *n_cli)
{
	*n_mcs = nvmeib_msgloop_get_count(capi->mcs.msg_loop);
	*n_cli = nvmeib_msgloop_get_count(capi->cli.msg_loop);
}

/*********************** Client - Control API End *****************************/
struct mcs_handler_param {						// Struct for scheduling volume config tasks on mainwq: mcs msg handling, request full config, various reports, ...
	struct nvmeibc_control_api* cc_api;			// Pointer to the object
	union {
		char dev_name[NVMEIBC_BD_NAME_LEN];		// Volume conf related tasks ("*" means all volumes)
		char dev_uuid[NVMEIBC_BD_UUID_LEN];		// Volume conf related tasks ("*" means all volumes)
		unsigned char token[16];
		struct {//for mgmt updates for fields sent upstream
			long long report_id_to_set;
			long long client_token_to_set;
			long long sequence_id_to_set;
			unsigned int keepaliveInterval;
		};
		struct get_client_configuration_msg *m;	// Response message for reservation reply
	};
	unsigned long long attachment_version_per_volume; // During detach, both version fields are needed.
	int attachment_version;						// given attachmentsVersion from mgmt: report, conf and detach tasks
	bool force;									// Use for attach/detach message
	bool upgrade;								// Use for detach for upgrade option
};

static inline bool _is_mcs_version_supported(unsigned int inbound_message_type_version)
{
	return (SUPPORTED_MCS_PROTOCOL_VERSION  == inbound_message_type_version);
}

static inline void nvmeibc_config_volume_init_empty( // Empty constructor
	struct nvmeibc_volume_header*h, const char* uuid, bool is_uuid) {
	memset(h, 0, sizeof(*h));
	h->version = -1;
	h->attachment_version = DEFAULT_ATTACHMENT_VERSION;
	if (is_uuid)
		strlcpy(h->uuid, uuid, sizeof(h->uuid));
	else
		strlcpy(h->devname, uuid, sizeof(h->devname));
}

static int __update_current_attachment_version_from_mgmt(struct nvmeibc_control_api* ccapi, int new_version, const char *dev_name);

static int __update_current_attachment_version_from_mgmt_on_main_wq(void *_param)
{
	struct mcs_handler_param *p = _param;
	struct nvmeibc_control_api* cc_api = p->cc_api;
	__verify_on_main_wq_ccapi(cc_api);
	__update_current_attachment_version_from_mgmt(cc_api, p->attachment_version,p->dev_name);
	kfree(_param);
	return 0;
}

static int __schedule_update_current_attachment_version_from_mgmt(struct nvmeibc_control_api *cc_api, int new_version)
{
	int rv = 0;
	struct mcs_handler_param *p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL) {
		rv = -ENOMEM;
		goto _out;
	}
	if (new_version == -1) {
		_NT(schedule_attachment_version_reject, "Scheduling update of attachment version rejected, version is @INT", new_version);
		kfree(p);
		goto _out;
	}
	p->cc_api = cc_api;
	p->attachment_version = new_version;
	_NT(schedule_attachment_version, "Scheduling update of attachment version to @INT", new_version);
	if ((rv = Nschedule_on_main_wq(trace_e7_ccapi, cc_api, __update_current_attachment_version_from_mgmt_on_main_wq, p)) < 0)
		kfree(p);
_out:
	return rv;
}

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	#define is_sim() (true)
#else
	#define is_sim() (false)
#endif

#include "main/cc_api/nvmeibc_main_capi_parse_conf.h"
#include "main/cc_api/nvmeibc_main_capi_parse_conf.inc.c"			// Todo: Remove this
/* Parse entire volume configuration from MCS message */
static int __parse_single_volume_conf(
	      struct nvmeib_mgmt_to_client_volume_configuration *dst,   // 1 result volume
	const struct nvmeib_mgmt_to_client_volume_configuration  src[], // array from which 'vol_i' is extracted
	const int vol_i)
{
	int i, rv;
	const bool is_hidden =    CHECK_HIDDEN__MAGIC(src->cli_unique_id);
	const bool is_recoverer = CHECK_RECOVER_MAGIC(src->cli_unique_id);
	const bool is_carrier = nvmeibc_block_is_any_carrier(&src->volumes[vol_i]);
	const bool is_shadow = CHECK_SHADOW__MAGIC(src->cli_unique_id);
	bool attach_volume_as_512B = false;

	if ((rv = __copy_nvmeib_mgmt_to_client_volume_config(dst, src, vol_i)) < 0) {
		_NT(t_psvc03, "Copy message failed");
		return -EINVAL;
	}

	if (dst->volumes->reservation.is_512B_IO_allowed == 0) { // AK: TODO - Remove once support management passes the actual value in 'is_512B_IO_allowed' inside the 'nvmeibc_reservation' structure
		_ND(t_psvc04, "Checking if volume 512B attachment override is required: @BOOL_YN, Management supplied value: @BOOL_YN", attach_volume_as_512B, (src->volumes[vol_i].reservation.is_512B_IO_allowed != 0));
		dst->volumes->reservation.is_512B_IO_allowed = attach_volume_as_512B;
	}

	if (is_recoverer) { //Mgmt can give recovery reservation mode, but type will be normal
		/* Real mgmt does not set the type to recoverer properly! */
		_NT(t_psvc05, "making the volume[@VOL_I] attachment recoverer @HDR_TYPE", vol_i, dst->volumes->type);
		dst->volumes->type |= (RECOVERER_VOLUME | HIDDEN_VOLUME); // Slightly different than TYPE_RECOVERER
	} else if (is_hidden) {
		_NT(t_psvc06, "making the volume[@VOL_I] attachment hidden @HDR_TYPE", vol_i, dst->volumes->type);
		dst->volumes->type |= HIDDEN_VOLUME; // Slightly different than RECOVERER_VOLUME
	}
	if (is_carrier) { //Remove hidden flag from carriers as we use the queue to issue extended IOs, keep recoverer flag
		dst->volumes->type &= (~HIDDEN_VOLUME);	// Here recoverer volume becomes visible, mainly for debug
	}
	if (is_shadow) {
		dst->volumes->type |= SHADOW_VOLUME;
	}

	// Format chunk stripe info if not given by mgmt
	for (i=0; i<dst->volumes->n_chunks; i++) {
		struct nvmeibc_chunk_conf *dst_chunk = &dst->volumes->chunks[i];
		int j;
		if (dst_chunk->stripeWidth == 0)
			dst_chunk->stripeWidth = dst->volumes->stripeWidth;
		if (dst_chunk->stripeSize == -1)
			dst_chunk->stripeSize =  dst->volumes->stripeSize;
		for (j=0; j<dst_chunk->n_praids; j++) {
			if (!dst_chunk->praids[j].numberOfMirrors)
				 dst_chunk->praids[j].numberOfMirrors = dst->volumes->numberOfMirrors;
		}
	}
	return 0;
}

static int __update_current_attachment_version_from_mgmt(struct nvmeibc_control_api* ccapi, int new_version, const char *dev_name);

/* Parse list of volume configurations from MCS message */
static int __parse_array_volume_conf(const struct nvmeib_mgmt_to_client_volume_configuration *src, struct nvmeibc_control_api *cc_api, bool is_update)
{
	int i, rv;
	const bool update_only = is_update || CHECK_UPDATE__MAGIC(src->cli_unique_id);
	const bool is_recoverer = CHECK_RECOVER_MAGIC(src->cli_unique_id);
	if (!_is_mcs_version_supported(src->messageTypeVersion)) {
		_NE(a93j4l2, "Unsupported messageTypeVersion=@INT", src->messageTypeVersion);
		return -EINVAL;
	}
	// Handle case of no volumes -> will trigger full configuration request
	_NT(trace_cc_api_parse_array_volume_conf, "msg update_type=@RV with @N_VOLUMES volumes", src->updateType, src->n_volumes);
	// Todo: Insert optimization that nics are used for all volumes

	if (is_recoverer) {	// Recoverer attachments are out of bounds and the attachmentsVersion must be ignored
		_NT(t_pavc_01, "Hidden attachment request ignorring attachmentsVersion=@INT", src->attachmentsVersion);
	} else if ((rv = __schedule_update_current_attachment_version_from_mgmt(cc_api, src->attachmentsVersion)) != 0)		// NVMESH-4837: Why schedule work here?
		goto _out;

	for (i = 0; i < src->n_volumes; i++) {	// A single message for each volume released after attach attempt
		struct nvmeib_mgmt_to_client_volume_configuration *dst = kzalloc(sizeof(*dst), GFP_KERNEL);
		if (!dst) {
			_NE(t_pavc_02, DMESG_PREFIX() ": No memory for attachmentsVersion=@INT @VOL_I", src->attachmentsVersion, i);
			rv = -ENOMEM;
			goto _out;
		}
		if ((rv = __parse_single_volume_conf(dst, src, i)) < 0) {
			_NT(t_pavc_03, "Error handling @VOL_I in configuration message", i);
			nvmeibc_cc_api_free_config_msg(dst);
			kfree(dst);
			goto _out;
		} else { // Full/partial configuration - applied to attached volumes
			_NT(t_pavc_04, DMESG_PREFIX("@DEV_NAME: ") "@VOL_I, @HDR_UUID, @SEND_TOKEN_STR @HDR_TYPE", dst->volumes->name, i, dst->volumes->uuid, src->cli_unique_id, dst->volumes->type);
			if ((rv = setup_block_device_generic(__get_cinst_params_from_cc_api(cc_api), dst, update_only)) != 0) {
				_NI(t_pavc_05, DMESG_PREFIX("@DEV_NAME") ": @VOL_I, @HDR_UUID has failed, rv=@RV. Skipping...", dst->volumes->name, i, dst->volumes->uuid, rv);
				nvmeibc_cc_api_free_config_msg(dst);
				kfree(dst);
				dst = NULL;
				goto _out;
			}
		}
	}
	rv = (src->n_volumes) ? 0 : -ENODEV;

_out:
	if (unlikely(rv<0)) { // Todo: Proper Error handling, here or in handle_mcs_message.
	} else if (src->updateType == UPDATETYPE_FULL) { // Reset the counter if a good full conf arrived
		__full_conf_mark_received(cc_api);
	}
	return rv;
}

static int __cache_replayed(struct nvmeibc_control_api *cc_api)
{
	NFIN;
	/* We modify mcs_cache_replayed_completed within the mcs process context, not from the main work queue (wq),
	 * because there is no risk of a race condition with other actions executed on the main wq.
	 * The worst-case scenario is that if a cdisk request targets NICs in parallel with receiving "end_of_cache",
	 * the request will not be processed until the next cdisk target connection cycle.
	 * Note that this is unlikely to occur, as the cache contains all the target NICs, and unless these NICs
         * are modified concurrently, this situation should never arise.
	 */
	cc_api->is_mcs_cache_replayed_completed = true;

	NFOUT;
	return 0;
}

static int __parse_targets_nics_conf(const struct nvmeib_mgmt_to_client_update_targets_nics *src, struct nvmeibc_control_api *cc_api)
{
	int rv;
	struct nvmeib_mgmt_to_client_update_targets_nics* dst = NULL;
	_NT(trace_cc_api_parse_targets_nics_conf, "msg with @INT targets", src->n_targets);

	dst = nvmeib_mgmt_to_client_update_targets_nics_clone(src);
	if (!dst) {
		_NE(t_ptnc_1, "No memory for update target nics");
		rv = -ENOMEM;
		goto _out;
	}

	rv = update_targets_nics_generic(__get_cinst_params_from_cc_api(cc_api), dst);
	if (rv) {
		nvmeibc_cc_api_free_update_targets_nics(dst);
		dst = NULL;
		goto _out;
	}

_out:
	return rv;
}

static void convert_offsets_to_ptrs_in_toma_to_client_volume_configuration_in_place(struct nvmeib_mgmt_to_client_volume_configuration *conf)
{
	int								i, j, k;
	void							*base_ptr = conf;
	struct nvmeibc_volume_conf		*vol;
	struct nvmeibc_chunk_conf		*chunk;
	struct nvmeibc_praid_conf		*praid;
	struct nvmeibc_target_conf		*target;

	NFIN;
	conf->volumes = (struct nvmeibc_volume_conf *)(base_ptr + (uint64_t)(conf->volumes));
	_NT(rvasukw, "local_cl n_volumes=@INT conf->volumes=@PTR", conf->n_volumes, conf->volumes);
	for (i = 0; i < conf->n_volumes; i++) {	// A single message for each volume released after attach attempt
		vol = &(conf->volumes[i]);
		vol->chunks = (struct nvmeibc_chunk_conf *)(base_ptr + (uint64_t)(vol->chunks));
		_NT(6xmig5a, "local_cl vol=@STR n_chunks=@INT vol->chunks=@PTR", vol->name, vol->n_chunks, vol->chunks);
		for (j = 0; j < vol->n_chunks; j++) {
			chunk = &(vol->chunks[j]);
			chunk->praids = (struct nvmeibc_praid_conf *)(base_ptr + (uint64_t)(chunk->praids));
			_NT(vt83k3c, "local_cl chunk=@STR n_praids=@INT chunk->praids=@PTR", chunk->uuid, chunk->n_praids, chunk->praids);
			for (k = 0; k < chunk->n_praids; k++) {
				praid = &(chunk->praids[k]);
				praid->segments = (struct nvmeibc_segment_conf *)(base_ptr + (uint64_t)(praid->segments));
				_NT(vkw02ap3, "local_cl praid=@STR n_segments=@INT praid->segments=@PTR", praid->uuid, praid->n_segments, praid->segments);
			}
		}
	}
	conf->targets = (struct nvmeibc_target_conf *)(base_ptr + (uint64_t)(conf->targets));
	_NT(03j8els, "local_cl n_targets=@INT conf->targets=@PTR", conf->n_targets, conf->targets);
	for (i = 0; i < conf->n_targets; i++) {
		target = &(conf->targets[i]);
		target->disks = (struct nvmeibc_disk_conf *)(base_ptr + (uint64_t)(target->disks));
		target->nics = (struct nvmeibc_nic_conf *)(base_ptr + (uint64_t)(target->nics));
		_NT(unrnsj7, "local_cl target=@STR n_disks=@INT n_nics=@INT target->nics=@PTR target->disks=@PTR", target->node_id, target->n_disks, target->n_nics, target->nics, target->disks);
	}
	NFOUT;
}

/* Translates the enum into a string to send to CLI */
static inline char *__vol_cmd_enum_to_result(enum_vol_status status)
{
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_BUSY           ) return "Busy";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_DETACHED       ) return "Detached";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED  ) return "DetachFailed";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED       ) return "Attached";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED_NOTHING_TO_UPDATE) return "Attached";	// CLI does not care about that difference
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_ATTACH_FAILED  ) return "AttachFailed";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_FAILED  ) return "UpdateFailed";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_READY	  ) return "UpdateReady";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_SHUTDOWN  	  ) return "Shutdown";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_UNKNOWN        ) return "Unknown";
	if (status == NVMEIB_C_TO_M_VOLUME_ALIAS_CREATED      ) return "Created";
	if (status == NVMEIB_C_TO_M_VOLUME_ALIAS_CREATE_FAILED) return "CreateFailed";
	if (status == NVMEIB_C_TO_M_VOLUME_ALIAS_DELETED      ) return "Deleted";
	if (status == NVMEIB_C_TO_M_VOLUME_ALIAS_DELETE_FAILED) return "DeleteFailed";
	if (status == NVMEIB_C_TO_M_VOLUME_RESERVATION_DENIED     ) return "ReservationDenied";
	if (status == NVMEIB_C_TO_M_VOLUME_RESERVATION_MODE_DENIED) return "ReservationModeDenied";
	if (status == NVMEIB_C_TO_M_VOLUME_MT_AUTHORIZATION_DENIED) return "AuthorizationDenied";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED_UNKNOWN_VOLUME) return "DetachFailedUnknownVolume";
	if (status == NVMEIB_C_TO_M_VOLUME_ACK_IRRELEVANT) return NULL;
	BUG();
	return NULL;
}

#define MAX_VOL_INFO_STRING (189)
#define io_perm_is_blocked_no_io(io_perm)  ((io_perm) == NVMEIB_C_TO_M_IO_TYPE_PERMIT_NEVER)
#define is_io_enabled_new(io_perm)         (io_perm == NVMEIB_C_TO_M_IO_TYPE_PERMIT_ALL)
static int __vol_info_to_string(const struct nvmeibc_volume_status_payload *info, char output[MAX_VOL_INFO_STRING])
{
	// Status string max size is 21, max int is 10, is status is either true
	// or false (5), uuid is 64, name is 32 max size of data is:
	// 21+10+5+64+32+20=152 + len("status= version= io= uuid= name= rv= ") = 37
	const char* msg_format="status=%s version=%d io_blocked=%s uuid=%s name=%s rv=%llu";
	const char* io_blocked_depr = io_perm_is_blocked_no_io(info->io_perm) ? "true" : "false";	// Deprecated fields since version 1.2.1+
	int len = snprintf(output, MAX_VOL_INFO_STRING, msg_format,
				       __vol_cmd_enum_to_result(info->vol_status), info->version,
					   io_blocked_depr, info->uuid, info->name, info->reservation.version);
	BUG_ON(len >= MAX_VOL_INFO_STRING);		// May cause stack corruption
	return len;
}

static void __copy_vol_reservation_from_vat(struct nvmeibc_volume_status_payload *vol,
						const struct nvmeibc_volume_attach_t *vat)
{
	struct nvmeibc_volume_attach_t _empty_vat;
	if (vat == NULL) {
		nvmeibc_volume_attach_t_init(&_empty_vat);
		vat = &_empty_vat;
	}
	vol->reservation = vat->res;
}

static void fill_msg_vol_info_empty(struct nvmeibc_volume_status_payload *vol,
									const struct nvmeibc_volume_attach_t *vat,
									const char* uuid, bool is_uuid)
{
	if (is_uuid)
		strlcpy(vol->uuid, uuid, sizeof(vol->uuid));
	else
		strlcpy(vol->name, uuid, sizeof(vol->name));
	vol->version = -1;
	vol->vol_status = NVMEIB_C_TO_M_VOLUME_ACK_DETACHED;
	vol->io_perm = nvmeibc_get_io_perm_for_reporting(NULL);
	vol->ioEnabled = 0;
	__copy_vol_reservation_from_vat(vol, vat);
}

/* Fill msg of volume status */
static void __fill_msg_vol_info(struct nvmeibc_volume_status_payload *vol,
				const struct nvmeibc_volume_header *hdr, enum_vol_status status,
				enum_io_perm io_perm)
{
	strlcpy(vol->uuid, hdr->uuid   , sizeof(vol->uuid));
	strlcpy(vol->name, hdr->devname, sizeof(vol->name));
	vol->version = hdr->version;
	vol->vol_status = (unsigned int)status;
	if (io_perm == NVMEIBC_IO_PERM_USE_CURR_PERMS){
		vol->io_perm = hdr->last_sent_io_perm;
	} else {
		vol->io_perm = io_perm;
	}
	vol->ioEnabled = is_io_enabled_new(vol->io_perm);
	__copy_vol_reservation_from_vat(vol, &hdr->vat);
	vol->is_hidden = (nvmeibc_block_is_recoverer_or_hidden(hdr) || nvmeibc_block_is_shadow(hdr)) ? 1 : 0;
	vol->attachment_version_per_volume = hdr->attachment_version_per_volume;
	vol->n_ref_ids = hdr->ext_blob.n_ref_ids;
	vol->referenceIDs = hdr->ext_blob.referenceIDs;	// No need to copy or lock (ext_blob_modify_guard), we are sending msg from main-wq and volume still exists, so use its memory
}

static inline long long increment_and_get_seq_num(struct nvmeibc_control_api* cc_api)
{
	INCREMENT_IF_NOT_DEFAULT(cc_api->clnt_2_mgmt_sequence_id);
	return cc_api->clnt_2_mgmt_sequence_id;
}

int management_report_frequency = DEFAULT_MANAGEMENT_REPORT_FREQUENCY;
module_param(management_report_frequency, int, 0644);
MODULE_PARM_DESC(management_report_frequency, "Report frequency for management, in seconds");

static unsigned long get_management_report_frequency(void)
{
	return (unsigned long)((management_report_frequency > 0) ?
		management_report_frequency :
		DEFAULT_MANAGEMENT_REPORT_FREQUENCY);
}

//All messages sent upstream from the mcs should set this header
//	Note that message sequence is increased as a side effect (TODO - should we do this explicitly?)
#define __fill_upstream_message_header_with_basic_info(msg, c) ({ 								\
	(msg)->upstream_header.messageTypeVersion	=  SUPPORTED_MCS_PROTOCOL_VERSION;				\
	(msg)->upstream_header.messageSequence	= increment_and_get_seq_num((struct nvmeibc_control_api *)c); \
	(msg)->upstream_header.clientToken	= (c)->clnt_2_mgmt_fullconf_token; 						\
	(msg)->upstream_header.keepaliveInterval	= get_management_report_frequency();		\
})

#define __fill_generic_message_with_minimal_basic_info(msg, c) ({ 	\
	(msg)->client_status = nvmeibc_get_state(); 					\
})

#define __fill_generic_message_with_basic_info(msg, c) ({ 		\
	__fill_generic_message_with_minimal_basic_info((msg), (c)); \
	(msg)->reportID = (c)->clnt_2_mgmt_report_id; 				\
})

static unsigned int get_vol_status_from_err_string(const struct vol_config_error * vol_err)
{
	unsigned int vol_status;
	if (vol_err->err[0] == 'C') {		 // More info in array 'mgmt_2_clnt_err_strings'. 3 Error messages, each start with a different char.
		vol_status = NVMEIB_C_TO_M_VOLUME_ACK_UNKNOWN;
	} else if (vol_err->err[0] == 'R') {
		vol_status = NVMEIB_C_TO_M_VOLUME_RESERVATION_MODE_DENIED;
	} else if (vol_err->err[0] == 'T') {
		vol_status = NVMEIB_C_TO_M_VOLUME_RESERVATION_DENIED;
	} else if (vol_err->err[0] == 'U') {
		vol_status = NVMEIB_C_TO_M_VOLUME_MT_AUTHORIZATION_DENIED;
	} else {	// Unsupported err for volume. Treat as if it was unknown
		vol_status = NVMEIB_C_TO_M_VOLUME_ACK_UNKNOWN;
		_NT(error_forward_mcs_error_to_cli, "Unsupported error message from management: @ERR_STR", vol_err->err);
	}
	return vol_status;
}

// Error message from management in case of unknown / reservation issue with volume attach request
// should be replied to CLI
static int __forward_mcs_error_to_cli(struct nvmeib_mcs_error* err,
									  struct nvmeibc_control_api* cc_api)
{
	if (!_is_mcs_version_supported(err->messageTypeVersion)) {
		return -EINVAL;
	} else {
		char cli_reply[MAX_VOL_INFO_STRING];
		int rv = 0;
		int err_idx;
		struct vol_config_error *vol_err;
		struct nvmeib_client_to_mgmt_vol_info *	msg = kzalloc(sizeof(*msg) + (err->n_errors * sizeof(*msg->attachments)), GFP_KERNEL);

		if (!msg) {
			return -ENOMEM;
		}

		__fill_generic_message_with_basic_info(msg, cc_api);
		msg->attachments = (void*)(&msg[1]);
		msg->n_volumes = err->n_errors;

		for (err_idx = 0; err_idx < err->n_errors; err_idx++)
		{
			struct nvmeibc_volume_status_payload *attachment = &msg->attachments[err_idx];
			vol_err = &err->errors[err_idx];

			if (vol_err->entityName[0] || vol_err->entityUUID[0]) {
				if (vol_err->entityName[0] == 0)
					fill_msg_vol_info_empty(attachment, NULL, vol_err->entityUUID, true);
				else
					fill_msg_vol_info_empty(attachment, NULL, vol_err->entityName, false);

				attachment->vol_status = get_vol_status_from_err_string(vol_err);

				__vol_info_to_string(attachment, cli_reply);
				nvmeibc_send_to_cli(cc_api, cli_reply);
			} else {
				_NT(error_1_cc_api_forward_mcs_error_to_cli, "No entity ID given for error message");
				rv |= -ENODEV;
			}
		}

		kfree(msg);
		return rv;
	}
}

static void __fill_msg_all_attached_vol_info(struct nvmeibc_volume_status_payload pl[], const struct nvmeibc_cinst_params_main *i)
{
	struct nvmeibc_volume *volume;
	list_for_each_entry(volume, nvmeibc_get_volumes(i), link) {
		__fill_msg_vol_info(pl, &volume->hdr, NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED, nvmeibc_get_io_perm_for_reporting(volume->block_dev));
		pl++;
	}
	list_for_each_entry(volume, nvmeibc_get_mt_volumes(i), link) {
		__fill_msg_vol_info(pl, &volume->hdr, NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED, nvmeibc_get_io_perm_for_reporting(volume->block_dev));
		pl++;
	}
}

static struct get_client_configuration_msg *get_client_configuration_msg_create(int n_vols)
{
	struct get_client_configuration_msg *msg = kzalloc(sizeof(*msg) + n_vols * sizeof(*msg->volumes), GFP_KERNEL);
	if (msg) {
		msg->volumes = (struct nvmeibc_volume_status_payload*)(&msg[1]);	// Append to message
		msg->n_volumes = n_vols;
		msg->configurationVersion = -1;	// Client does not support this field in this msg, management does not care
	}
	return msg;
}

static int __request_new_config_from_mcs(void *_p)
{
	struct mcs_handler_param *p = (struct mcs_handler_param*)_p;
	struct nvmeibc_control_api* cc_api = p->cc_api;
	const struct nvmeibc_cinst_params_main *i = __get_cinst_params_from_cc_api(cc_api);
	struct get_client_configuration_msg *msg;
	const char* magic_token = (p->force) ? MAGIC_CONFIG_FORCE__TOKEN : MAGIC_CONFIG_UPDATE_TOKEN;
	int rv = -ENOMEM;
	WARN(p->force, "Should never be force");
	__verify_on_main_wq_ccapi(cc_api);
	if (!strcmp(p->dev_name, REQUEST_ALL_VOLUMES)) {	// Full conf request
		int n_vols = is_sim() ? 0 : nvmeibc_get_all_volumes_num(i);		// Client should put n_vols = 0! Mgmt will send full conf according to its view. But due to missing implementation in management, client helps by supplying a list of current volumes. In mgmt simulator everything is implemented
		if (!(msg = get_client_configuration_msg_create(n_vols)))
			goto _out;
		if (n_vols) {
			__fill_msg_all_attached_vol_info(msg->volumes, i);
			_NT(t_rncfm00, "Full conf request, old format of @INT volumes", n_vols);
		}
	} else {								// Single volume conf request
		struct nvmeibc_volume *vol = nvmeibc_volume_get_by_name(i, p->dev_name, UNKNOWN_ILLEGAL);
		if (!vol) {
			_NT(t_rncfm01, "Volume @DEV_NAME, not found not requesting configuration", p->dev_name);
			goto _out;
		}
		if (!(msg = get_client_configuration_msg_create(1)))
			goto _out;
		__fill_msg_vol_info(msg->volumes, &vol->hdr,
							NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED,
							nvmeibc_get_io_perm_for_reporting(vol->block_dev));
	}
	strlcpy(msg->cli_unique_id, magic_token, sizeof(msg->cli_unique_id));	// Magic token, client will allow it
	__fill_upstream_message_header_with_basic_info(msg, cc_api);

	#if defined(__KERNEL__)
		rv = 0; (void)msg;
	#else
		rv = nvmeibc_send_to_mcs(cc_api, msg, NULL, MCS_GET_CLIENT_CONFIGURATION_MSG_MSG);
	#endif
	kfree(msg);
_out:
	if (unlikely(rv < 0))
		_NT(trace_1_cc_api_request_new_config_from_mcs, "Unable to send conf request rv=@RV, vol=@DEV_NAME", rv, p->dev_name);
	kfree(p);
	return rv;
}

static int __schedule_request_config(struct nvmeibc_control_api *cc_api, const char *name, const char* dbg_reason)
{
	const bool is_atomic_context = (dbg_reason[0] == 'T');	// Only when Toma msg arrives - this is atomic context
	const bool is_force = (dbg_reason[0] == 'F');			// Only when rebuild during hot upgrade is requested, this is force
	int rv;
	struct mcs_handler_param *p = kzalloc(sizeof(*p), (is_atomic_context ? GFP_ATOMIC : GFP_KERNEL));
	strlcpy(p->dev_name, name, sizeof(p->dev_name));	// Copy it because volume can get detached while we are scheduling
	p->cc_api = cc_api;
	p->force = is_force;
	_NT(t_10_cc_api, "vol=@DEV_NAME, why? @STR, force=@BOOL_YN", name, dbg_reason, p->force);
	if ((rv = Nschedule_on_main_wq(t_11_cc_api, cc_api, __request_new_config_from_mcs, p)) < 0)
		kfree(p);
	return rv;
}

static int send_unknown_volume_to_cli(struct nvmeibc_control_api* cc_api, const char *v_id, const bool is_uuid)
{
	int rv;
	struct nvmeib_client_to_mgmt_init *msg = NULL;
	char cli_reply[MAX_VOL_INFO_STRING];
	_ND(trace_cc_api_send_unknown_volume_to_cli, "Detach/Delete volume not found replying with unknown status");
	if (!(msg = kzalloc(sizeof(*msg) + sizeof(*msg->volumes), GFP_KERNEL))){
			rv = -ENOMEM;
			goto _out;
		}
	msg->volumes = (void*)(&msg[1]);	// Append to message
	fill_msg_vol_info_empty(msg->volumes, NULL, v_id, is_uuid);
	msg->volumes->vol_status = NVMEIB_C_TO_M_VOLUME_ACK_UNKNOWN;
	__vol_info_to_string(msg->volumes, cli_reply);
	if ((rv = nvmeibc_send_to_cli(cc_api, (char*)cli_reply)) < 0)
		_NE(error_cc_api_send_unknown_volume_to_cli, MAIN_IOCTL_PREFIX ": Unable to send cli message, rv=@RV", rv);
_out:
	kfree(msg);
	return rv;
}

static int __detach_deleted_vol_by_uuid_on_main_wq(void *_p)
{
	struct mcs_handler_param *p = (struct mcs_handler_param*)_p;
	struct nvmeibc_control_api* cc_api = p->cc_api;
	const struct nvmeibc_cinst_params_main *i = __get_cinst_params_from_cc_api(cc_api);
	const char *volume_uuid = p->dev_uuid;
	struct nvmeibc_volume *volume;
	if (!(volume = nvmeibc_volume_get_by_uuid(i, volume_uuid, UNKNOWN_ILLEGAL))) {
		#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
			send_unknown_volume_to_cli(cc_api, volume_uuid, true); // TODO - replace cli parsing with MCS response for simulator
		#endif
		// Should we update mgmt (MCS) with detached status? If the volume is not found (not attached) it is certainly detached - Ask Tom
		_NW(warn_1_cc_api_detach_deleted_vol_by_uuid_on_main_wq, "Incorrect volume @HDR_UUID", volume_uuid);
	} else { // Deleted volume is always force detached
		try_detach_volume_with_multicomplete(volume, nvmeibc_vol_detach_cmd_default(), NULL);
	}
	kfree(p);
	return 0;
}

static int __handle_mcs_ack_on_main_wq(void *_p)
{
	int ret;
	struct mcs_handler_param *p = (struct mcs_handler_param*)_p;
	struct nvmeibc_control_api *cc_api = p->cc_api;
	NFIN;

	ret = nvmeib_mcs_ack_msg(cc_api->mcs.handle, p->token);
	if (ret) {
		_NT(trace_handle_mcs_ack_ignored, "message ack ignored");
	}
	kfree(p);
	NFOUT;

	return ret;
}

static void __nvmeibc_volume_header_create_from_uuid(struct nvmeibc_volume_header *hdr, char const* uuid)
{
	*hdr = (struct nvmeibc_volume_header){0};
	nvmeibc_volume_header_init_0(hdr);
	strlcpy(hdr->uuid, uuid, sizeof(hdr->uuid));
}

static int __detach_vol_by_uuid_on_main_wq(void *_p)
{
	int rv = 0;
	struct mcs_handler_param *p = (struct mcs_handler_param*)_p;
	struct nvmeibc_control_api* cc_api = p->cc_api;
	const struct nvmeibc_cinst_params_main *i = __get_cinst_params_from_cc_api(cc_api);
	const char *volume_uuid = p->dev_uuid;
	struct nvmeibc_volume *volume = nvmeibc_volume_get_by_uuid(i, volume_uuid, UNKNOWN_ILLEGAL);
	if (!volume) {
		struct nvmeibc_volume_header hdr = {0};
		struct nvmeibc_tpv *tpv;
		__nvmeibc_volume_header_create_from_uuid(&hdr, volume_uuid);

		/* TPVs are not tracked in the nvmeibc_volume list; look them up
		 * in the separate nvmeibc_tpv_active_list and detach directly. */
		tpv = nvmeibc_tpv_find_by_uuid(volume_uuid);
		if (tpv) {
			_NI(tpv_mcs_detach, "TPV @HDR_UUID: MCS detach dispatched to nvmeibc_tpv_detach", volume_uuid);
			nvmeibc_tpv_detach(tpv);
			nvmeibc_cc_api_reply_vol_cmd_status(i, &hdr, NVMEIB_C_TO_M_VOLUME_ACK_DETACHED,
				NVMEIB_C_TO_M_IO_TYPE_PERMIT_NEVER, false /*send_to_cli*/, true /*send_to_mcs*/,
				0 /* inc_report_id_if_needed */);
		} else {
			#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
				send_unknown_volume_to_cli(cc_api, volume_uuid, true); // TODO - replace cli parsing with MCS response for simulator
			#endif
			// Should we update mgmt (MCS) with detached status? If the volume is not found (not attached) it is certainly detached - Ask Tom
			_NE(t0dvbuomq, "Incorrect volume @HDR_UUID", volume_uuid);
			nvmeibc_cc_api_reply_vol_cmd_status(i, &hdr, NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED_UNKNOWN_VOLUME,
				NVMEIB_C_TO_M_IO_TYPE_PERMIT_NEVER, false /*send_to_cli*/, true /*send_to_mcs*/,
				0 /* inc_report_id_if_needed */);
		}
	} else {
		const int mcs_av = p->attachment_version;
		const int vol_av = volume->hdr.attachment_version;
		const int version_delta = mcs_av - vol_av;
		_NT(t1dvbuomq, "detach request for volume @HDR_UUID, global_av(mcs=@INT, vol=@INT), per_vol(mcs=@INT64, vol=@INT64)",
			volume_uuid, mcs_av, vol_av, p->attachment_version_per_volume, volume->hdr.attachment_version_per_volume);

		if (version_delta >= 0) {
			const struct nvmeibc_vol_detach_cmd how = (p->upgrade) ? nvmeibc_vol_detach_cmd_upgrade() : ((p->force) ? nvmeibc_vol_detach_cmd_default() : nvmeibc_vol_detach_cmd_nice());
			MAX_WITH(volume->hdr.attachment_version_per_volume, p->attachment_version_per_volume);	// Regardless of success or failure this is the highest value the volume has seen
			try_detach_volume_with_multicomplete(volume, how, NULL);
		} else {
			_NT(trace_3_cc_api_detach_vol_by_uuid_on_main_wq, "Old detach request for volume @HDR_UUID, ignoring", volume_uuid);
			rv = -EINVAL;
		}
	}
	kfree(p);
	return rv;
}

void nvmeibc_cc_api_request_self_recov_detach(/*const char*/ void *_volume_uuid,
									 const struct nvmeibc_cinst_params_main *p)
{
	const char *volume_uuid = (const char *)_volume_uuid;
	struct nvmeibc_volume *volume = nvmeibc_volume_get_by_uuid(p, volume_uuid,
															UNKNOWN_ILLEGAL);
	nvmeibc_assert_on_main_wq(p);
	if (volume) {
		try_detach_volume_with_multicomplete(volume, nvmeibc_vol_detach_cmd_recov(), NULL);
	} else { /* We got lucky, some-one else already detached this volume */ }
}

// Try to detach the volume by uuid given in the deletion event
static int __parse_volume_deletion_message(const struct delete_message *msg, struct nvmeibc_control_api *cc_api)
{
	if (!_is_mcs_version_supported(msg->messageTypeVersion)) {
		return -EINVAL;
	} else {
		int rv = 0;
		struct mcs_handler_param *p = kzalloc(sizeof(*p), GFP_KERNEL);
		const char *uuid = msg->uuid;

		strlcpy(p->dev_uuid, uuid, sizeof(p->dev_uuid));	// Copy it because volume can get detached while we are scheduling
		p->cc_api = cc_api;
		_NI(i_15_cc_api, "detach/delete: uuid @UUID, name @DEV_NAME", uuid, msg->volumeID);
		if ((rv = Nschedule_on_main_wq(t_16_cc_api, cc_api, __detach_deleted_vol_by_uuid_on_main_wq, p)) < 0) {
			kfree(p);
		}
		return rv;
	}
}

static bool __update_clnt_token_from_mgmt(struct nvmeibc_control_api* ccapi, long long new_val);

static int __handle_mcs_ack(struct nvmeibc_mcs_downstream_ack *msg, struct nvmeibc_control_api *cc_api)
{
	int rv;
	struct mcs_handler_param *p = kzalloc(sizeof(*p), GFP_KERNEL);
	NFIN;

	if(!_is_mcs_version_supported(msg->messageTypeVersion)) {
		rv = -EINVAL;
		goto out;
	}

	_NT(trace_mcs_ack, "mcs message acknowledged");
	if (p == NULL) {
		rv = -ENOMEM;
		_NE(err_handle_mcs_ack_cannot_alloc_param, "failed to allocate mcs_handler_param");
		__update_clnt_token_from_mgmt(cc_api, -1);
		goto out;
	}

	memcpy(p->token, msg->messageToken, sizeof(p->token));
	p->cc_api = cc_api;
	if ((rv = Nschedule_on_main_wq(trace_mcs_ack_on_main, cc_api, __handle_mcs_ack_on_main_wq, p)) < 0) {
		_NE(err_handle_mcs_ack_cannot_send_message, "failed to resched mcs ack message on main wq");
		// if ack could not be delivered, we set client token to -1, and revert the client status,
		// this will cause keep alive client token -1 to be sent, and all cache will be cleared.
		__update_clnt_token_from_mgmt(cc_api, -1);
		kfree(p);
	}

out:
	NFOUT;
	return rv;
}

static void __detach_message_dump(const struct detach_message *msg)
{
	int i;
	_NT(detach_message_dump, "Detach message - attachmentsVersion: @INT, n_volumes: @LEN", msg->attachmentsVersion, msg->n_volumes);
	for (i = 0; i < msg->n_volumes; i++) {
		_NT(detach_message_dump_vol, "volume index: @LEN, UUID: @UUID, Name: @DEV_NAME, Force: @RV",
			i, msg->volumes[i].uuid, msg->volumes[i].name, msg->volumes[i].force);
	}
}

static int __parse_volume_detach_message(const struct detach_message *msg, struct nvmeibc_control_api *cc_api)
{
	int rv = 0, i;
	if (!_is_mcs_version_supported(msg->messageTypeVersion)) {
		return -EINVAL;
	}
	__detach_message_dump(msg);
	if ((rv = __schedule_update_current_attachment_version_from_mgmt(cc_api, msg->attachmentsVersion)) != 0)
		goto _out;
	for (i = 0; i < msg->n_volumes; i++) {
		struct mcs_handler_param *p = kzalloc(sizeof(*p), GFP_KERNEL);
		const char *uuid = msg->volumes[i].uuid;
		if (p == NULL) {
			rv = -ENOMEM;
			goto _out;
		}
		strlcpy(p->dev_uuid, uuid, sizeof(p->dev_uuid));	// Copy it because volume can get detached while we are scheduling
		p->cc_api = cc_api;
		p->force =   msg->volumes[i].force;
		p->upgrade = msg->volumes[i].upgrade;
		p->attachment_version = msg->attachmentsVersion;
		p->attachment_version_per_volume = msg->volumes[i].attachment_version_per_volume;
		_NI(i_detach_volu, "detach: uuid @UUID, name @DEV_NAME", uuid, msg->volumes[i].name);
		if ((rv = Nschedule_on_main_wq(t_18_cc_api, cc_api, __detach_vol_by_uuid_on_main_wq, p)) < 0) {
			kfree(p);
			goto _out;
		}
	}
	rv = (msg->n_volumes) ? rv : -ENODEV;
_out:
	return rv;
}

static int __client_update_upstream_params(void *param);
static int __schedule_upstream_params_update(struct update_client_token_message *msg, struct nvmeibc_control_api* cc_api)
{
	if(!_is_mcs_version_supported(msg->messageTypeVersion)) {
		return -EINVAL;
	} else {
		struct mcs_handler_param *p = kzalloc(sizeof(*p), GFP_KERNEL);
		int rv;
		p->sequence_id_to_set = msg->messageSequence;
		p->report_id_to_set = msg->reportID;
		p->client_token_to_set = msg->clientToken;
		p->keepaliveInterval = msg->keepaliveInterval;
		p->attachment_version = msg->attachmentsVersion;
		p->cc_api = cc_api;
		_NT(t_update_upstream_params_cc_api, "Management updated upstream params clientToken=@COUNTS, reportID=@COUNTS, messageSequence=@COUNTS",
			msg->clientToken, msg->reportID, msg->messageSequence);
		if ((rv = Nschedule_on_main_wq(trace_e8_update_upstream_params_ccapi, cc_api, __client_update_upstream_params, p)) < 0)
			kfree(p);
		return rv;
	}
}

#include "../toma/clnt/nvmeibt_client_protocol.h"
static int nvmeibc_cc_api_handle_toma_to_local_clnt_msg_process(struct nvmeibc_control_api* cc_api, void *__p)
{
	int													rv;
	struct nvmeibt_toma_to_local_client_msg 			*msg = (struct nvmeibt_toma_to_local_client_msg *)(__p);
	struct nvmeibt_toma_to_local_client_attach_params	*attach_params;
	struct nvmeib_mgmt_to_client_volume_configuration	*toma_to_client_volume_configuration;
	struct detach_message								*detach_msg;

	NFIN;
	attach_params = &(msg->payload.attach_params);
	switch (attach_params->msg_type) {
	case TOMA_TO_LOCAL_CLNT_MSG_TYPE_ATTACH:
		toma_to_client_volume_configuration = (struct nvmeib_mgmt_to_client_volume_configuration *)(msg->data);
		_NT(muxb52b, "local_cl attach_params: msg=@PTR attach_params=@PTR data=@PTR attach_params->msg_type=@INT attach_params->vol_name=@STR toma_to_client_volume_configuration=@PTR",
			msg, attach_params, toma_to_client_volume_configuration, attach_params->msg_type, attach_params->vol_name, toma_to_client_volume_configuration->volumes[0].name);
		convert_offsets_to_ptrs_in_toma_to_client_volume_configuration_in_place(toma_to_client_volume_configuration);
		// Fixups
		toma_to_client_volume_configuration->messageTypeVersion = SUPPORTED_MCS_PROTOCOL_VERSION;
		//
		toma_to_client_volume_configuration->attachmentsVersion = cc_api->latest_attachment_version;	// Artificial, for verification, since it should come from mgmt
		//
		rv = __parse_array_volume_conf(toma_to_client_volume_configuration, cc_api, false);
		break;
	case TOMA_TO_LOCAL_CLNT_MSG_TYPE_DETACH:
		detach_msg = (struct detach_message *)(msg->data);
		detach_msg->volumes = (struct volume_detach_payload *)((void *)detach_msg + (uint64_t)detach_msg->volumes);
		// Fixups
		detach_msg->messageTypeVersion = SUPPORTED_MCS_PROTOCOL_VERSION;
		//
		detach_msg->attachmentsVersion = cc_api->latest_attachment_version;	// Artificial, for verification, since it should come from mgmt
		//
		rv = __parse_volume_detach_message(detach_msg, cc_api);
		break;
	default:
		rv = -1;
		_NT(r90l24k, "Unknown option @INT", attach_params->msg_type);
	}
	kfree(__p);
	NFOUT;
	return rv;
}

struct t_main_clnt_globals	*toma_local_clnt_globals;
void set_toma_local_clnt_globals(struct t_main_clnt_globals *p)
{
	toma_local_clnt_globals = p;
}
int nvmeibc_cc_api_handle_toma_to_local_clnt_msg(struct nvmeib_local_client_params *p)
{
	int										rv = -1;
	NFIN;
	if (!toma_local_clnt_globals) {
		_NE(23k0skc, "toma_local_clnt_globals=NULL");
		goto out;
	}
	// Already on main wq.
	_NT(i8jsikl, "toma_local_clnt_globals=@PTR msg_len=@INT msg=@PTR", toma_local_clnt_globals, p->msg_len, p->msg);
	rv = nvmeibc_cc_api_handle_toma_to_local_clnt_msg_process(&toma_local_clnt_globals->cc_api, p->msg);
out:
	NFOUT;
	return rv;
}

/* Return value: Amount of bytes processed (0..len-1 are errors, len is OK) */
static int handle_mcs_msg(void *mcs_handle, char *buf, size_t len, bool *posted)
{
	struct c_api_proc *mcs = container_of(mcs_handle, struct c_api_proc, handle);
	struct nvmeibc_control_api *cc_api = container_of(mcs, struct nvmeibc_control_api, mcs);
	int rv = -EINVAL, opcode;
	void *msg;
	NFIN;

	(void)posted;
	// First sizeof integer is the length of the message, afterwards is starts
	msg = nvmeib_mcs_get_msg((*(void**)mcs_handle), buf + sizeof(int), len - sizeof(int));
	if (unlikely(msg == NULL)) {
		_NT(error_cc_api_handle_mcs_msg, "nvmeibc out of memory");
		rv = -ENOMEM;
		goto _out;
	}
	opcode = nvmeib_mcs_get_opcode(msg);
	_NI(info_cc_api_handle_mcs_msg, "opcode=@OPCODE", opcode);
	_ND(debug_cc_api_handle_mcs_msg, "buf=@BUF, msg=@MSG, len=@LEN", buf, msg, (int)len);
	switch (opcode) //vcfg@"mcs entry point"
	{
	case MCS_ATTACH_VOLUMES_MESSAGE_MSG:
		rv = __parse_array_volume_conf(msg, cc_api, false);
		break;
	case MCS_UPDATE_VOLUMES_MESSAGE_MSG:
		rv = __parse_array_volume_conf(msg, cc_api, /*is_update*/true);
		break;
	case MCS_VOLUME_DETACH_MESSAGE_MSG:
		rv = __parse_volume_detach_message(msg, cc_api);
		break;
	case MCS_VOLUME_DELETION_MESSAGE_MSG:
		rv = __parse_volume_deletion_message(msg, cc_api);
		break;
	case MCS_ERROR_RESPONSE_MSG:
		rv = __forward_mcs_error_to_cli(msg, cc_api);
		break;
	case MCS_UPDATE_CLIENT_TOKEN_MSG:
		rv = __schedule_upstream_params_update(msg, cc_api);
		break;
	case MCS_UPDATE_TARGETS_NICS_MSG:
		rv = __parse_targets_nics_conf(msg, cc_api);
		break;
	case MCS_CACHE_REPLAY_END_MSG:
		rv = __cache_replayed(cc_api);
		break;
	case MCS_CONFIRMED_DELIVERY_MSG:
		rv = __handle_mcs_ack(msg, cc_api);
		break;
	default:
		_NT(error_1_cc_api_handle_mcs_msg, "nvmeibc unsupported mcs opcode=@OPCODE", opcode);
		//rv = -EINVAL
		break;
	}

	if (unlikely((rv != 0)&&(rv != -ENOENT))) { /* ! (OK or canceled) */
		_NT(trace_2_cc_api_handle_mcs_msg, "Invalid msg, requesting full config, op=@OPCODE, rv=@RV", opcode, rv);
		__full_conf_request_on_err(cc_api);
	}
	rv = (int)len;		// Tell msgloop how many bytes were used
_out:
	NFOUT;
	return rv;
}

void update_processing_multi_vol_cmd(const struct nvmeibc_cinst_params_main *p,
				     bool val)
{
	struct nvmeibc_control_api *ccapi =
		&__get_from_params_main_globals_container(p)->cc_api;

	_NT(trace_update_processing_multi_vol_cmd, "setting multi vol indicator to @BOOL", val);

	ccapi->processing_multi_vol_cmd = val;
}

/*********************** Client - Management communication *******************/
int nvmeibc_volume_get_max_global_attach_version_ever_seen(const struct nvmeibc_cinst_params_main *p)
{
	return __get_from_params_main_globals_container(p)->cc_api.latest_attachment_version;		// NVMESH-4837: verify with Mgmt team if/where needed
}

int nvmeibc_cc_api_reply_vol_cmd_status(const struct nvmeibc_cinst_params_main* p,
		/*const*/ struct nvmeibc_volume_header *hdr,
		enum_vol_status status, u32 io_perm, bool send_to_cli, bool send_to_mcs,
		int inc_report_id_if_needed)
{
	struct nvmeibc_control_api* ccapi = &__get_from_params_main_globals_container(p)->cc_api;
	int rv = 0;
	const int did_status_change = nvmeibc_cc_api_has_volume_status_changed(status);
	bool did_something_change;
	char cli_reply[MAX_VOL_INFO_STRING];
	struct nvmeib_client_to_mgmt_vol_info *msg;
	bool filter_status_message = false;

// shortcut, the kernel simulator does not yet set client token, need to be fixed in the future
#if defined(__KERNEL__)
	if (ccapi->clnt_2_mgmt_fullconf_token == DEFAULT_UPSTREAM_VALUE) {
		_NT(e_reply_status_a1, "@DEV_NAME: client token not yet received, ignoring status update", hdr->devname);
		filter_status_message = true;
	}
#endif

	msg = kzalloc(sizeof(*msg) + sizeof(*msg->attachments), GFP_KERNEL);
	if (!msg) {
		rv = -ENOMEM;
		goto _out;
	}

	if (inc_report_id_if_needed < 0) {
		_NE(e_reply_status_a0, DMESG_PREFIX() ": Wrong usage of func @INT", status);
	}
	did_something_change = inc_report_id_if_needed && !!did_status_change;

	if (NVMEIBC_IO_PERM_USE_CURR_PERMS == io_perm)
		io_perm = hdr->last_sent_io_perm;

	msg->attachments = (void*)(&msg[1]);	// Append to message
	msg->n_volumes = 1;
	__fill_generic_message_with_basic_info(msg, ccapi);
	__fill_msg_vol_info(msg->attachments, hdr, status, io_perm);
	__vol_info_to_string(msg->attachments, (char *)cli_reply);
	if (send_to_mcs) {
		_NT(t_carvcs00, "Potentially incrementing report_id. Current counters: clientToken=@COUNTS, reportID=@COUNTS, messageSequence=@COUNTS", ccapi->clnt_2_mgmt_fullconf_token, ccapi->clnt_2_mgmt_report_id, ccapi->clnt_2_mgmt_sequence_id);
		hdr->last_sent_io_perm = io_perm;
		if (!filter_status_message) {
			if (did_something_change) {
				//Increment the report ID and update the message
				INCREMENT_IF_NOT_DEFAULT(ccapi->clnt_2_mgmt_report_id);
				msg->reportID = ccapi->clnt_2_mgmt_report_id;
			}
			__fill_upstream_message_header_with_basic_info(msg, ccapi);
			_NT(t_carvcs05, "@DEV_NAME: About to send an updated volume status to MCS with reportID=@INT, by status=@INT, increase=@BOOL_YN, io_perm=@INT, n_ref_ids=@INT, avpv=@INT64", hdr->devname, msg->reportID, status, did_something_change, io_perm, msg->attachments->n_ref_ids, msg->attachments->version);
			if ((rv = nvmeibc_send_to_mcs(ccapi, msg, &msg->attachments[0].uuid, MCS_VOLUME_STATUS_MESSAGE_MSG)) < 0)
				_NE(t_carvcs02, DMESG_PREFIX() ": Unable to send mcs message, rv=@RV", rv);
			_NT(t_carvcs03, "Sent updated volume status to MCS. Current counters: clientToken=@COUNTS, reportID=@COUNTS, messageSequence=@COUNTS", ccapi->clnt_2_mgmt_fullconf_token, ccapi->clnt_2_mgmt_report_id, ccapi->clnt_2_mgmt_sequence_id);
		} else {
			rv = 0;
		}
	}
	if (send_to_cli) { // When updating MGMT on IO En/Disabled no need to send to cli
		if ((rv = nvmeibc_send_to_cli(ccapi, (char*)cli_reply)) < 0)
			_NE(t_carvcs04, DMESG_PREFIX() ": Unable to send cli message, rv=@RV", rv);
	}
	kfree(msg);
_out:
	return rv;
}

static void __nvmeibc_cc_api_notify_vol_io_changed(struct nvmeibc_volume *volume,
						   const struct nvmeibc_cinst_params_main *p)
{
	enum_io_perm io_perm;
	bool send_to_mcs = true; // Update mgmt
	bool send_to_cli = !volume->hdr.first_io_enabled_was_sent_to_cli && !nvmeibc_block_is_recoverer_or_hidden(&volume->hdr); // Send only on first time and not hidden
	BUG_ON(volume == NULL);

	io_perm = nvmeibc_get_io_perm_for_reporting(volume->block_dev);
	if (volume->hdr.last_sent_io_perm == io_perm) { // We are sending the exact same IO permission from WD context, probably a race
		_ND(nvmeibc_cc_api_notify_io_changed, "@DEV_NAME_FULL: Volume @HDR_UUID sending the previously sent IO Perm @IO_PERM to mgmt", volume->full_name, volume->hdr.uuid, io_perm);
	}
	nvmeibc_cc_api_reply_vol_cmd_status(p, &volume->hdr, NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED, io_perm, send_to_cli, send_to_mcs, 1 /* inc_report_id_if_needed */);
	// Only upon first IO enabled send to CLI - if hidden attached and changing to fully attached will update cli only once (mgmt still gets every update)
	volume->hdr.first_io_enabled_was_sent_to_cli = volume->hdr.first_io_enabled_was_sent_to_cli || (!io_perm_is_blocked_no_io(io_perm) && !nvmeibc_block_is_recoverer_or_hidden(&volume->hdr));
}


void nvmeibc_cc_api_notify_io_changed(void *_volume_uuid, const struct nvmeibc_cinst_params_main *p)
{
	const char *volume_uuid = (const char *)_volume_uuid;
	struct nvmeibc_volume *volume = nvmeibc_volume_get_by_uuid(p, volume_uuid,
															UNKNOWN_ILLEGAL);
	nvmeibc_assert_on_main_wq(p);
	if (volume){
		__nvmeibc_cc_api_notify_vol_io_changed(volume, p);
	} else {
		//actually this should never happen - work queue will ensure this
	}
}

void nvmeibc_cc_api_notify_detach_completion(/*const*/ struct nvmeibc_volume *volume, u32 /*enum_vol_status*/ status)
{
	const bool send_to_cli = (is_sim() || nvmeibc_block_is_recoverer_or_hidden(&volume->hdr)), send_to_mcs = true; // Always update MGMT (update CLI if recoverer), Simulator TODO - replace cli parsing with MCS response for simulator, then remove is_sim() condition
	nvmeibc_cc_api_reply_vol_cmd_status(volume->p, &volume->hdr, status, nvmeibc_get_io_perm_for_reporting(NULL), send_to_cli, send_to_mcs, 1 /* inc_report_id_if_needed */);
}

// Notify the script of the request status
int nvmeibc_cc_api_sub_vol_notification(    /*const*/ struct nvmeibc_volume *volume, u32 /*enum_vol_status*/ status)
{
	int rv;
	const bool send_to_cli = true; // Always update CLI (sub volume can only come from CLI)
	const bool send_to_mcs = false; // Never update MGMT about sub_volumes
	unsigned int io_perm = nvmeibc_get_io_perm_for_reporting(volume->block_dev);
	rv = nvmeibc_cc_api_reply_vol_cmd_status(volume->p, &volume->hdr, status, io_perm, send_to_cli, send_to_mcs, 1 /* inc_report_id_if_needed */);
	return rv;
}

// Notify the script that the volume is unknown
int nvmeibc_cc_api_sub_vol_unknown(const struct nvmeibc_cinst_params_main *p, const char *vol_name, const bool is_uuid)
{
	return send_unknown_volume_to_cli(&__get_from_params_main_globals_container(p)->cc_api, vol_name, is_uuid);
}

#include "main/nvmeibc_main_ioctls.h"
#include "main/nvmeibc_main_ioctls.inc.c"		// Todo: Remove me

/*************************** Client - MGMT allerts ****************************/
static void __fill_mgmt_allert(struct nvmeibc_control_api* cc_api, struct nvmeib_mcs_log *msg, const char *level, const char *header, const char *message, u64 m_id)
{
	//We assume both messages are actually the same structure
	BUG_ON(sizeof(struct nvmeib_mcs_log) != sizeof(struct nvmeib_mcs_update_log));
	__fill_upstream_message_header_with_basic_info(msg, cc_api);
	strlcpy(msg->header , header , sizeof(msg->header));
	strlcpy(msg->message, message, sizeof(msg->message));
	strlcpy(msg->level  , level  , sizeof(msg->level));
	msg->id = m_id;
}

static void	__fill_mgmt_ack(struct nvmeibc_control_api* cc_api, struct nvmeib_mcs_ack_log *msg, const char *header, u64 m_id)
{
	__fill_upstream_message_header_with_basic_info(msg, cc_api);
	strlcpy(msg->header, header, sizeof(msg->header));
	msg->id = m_id;
}

static int __send_mgmt_allert(struct nvmeibc_control_api* cc_api, const char *level, const char *header, const char *message, u64 m_id, const int opcode)
{
	int rv = -EINVAL;
	void *msg;
	if (strlen(header) > 96 || strlen(message) > 128) {
		_NT(trace_cc_api_send_mgmt_allert, "Msg exceeds permitted size, truncating");
	}
	if (opcode == MCS_ACK_MANAGEMENT_LOG_MESSAGE_MSG) {
		if (!(msg = kzalloc(sizeof(struct nvmeib_mcs_ack_log), GFP_KERNEL)))
			goto _no_mem;
		_ND(trace_1_cc_api_send_mgmt_allert, "ack to MGMT: @HEADER (@M_ID)", header, m_id);
		__fill_mgmt_ack(cc_api, msg, header, m_id);
	} else {  /* Update or New message */
		if (!(msg = kzalloc(sizeof(struct nvmeib_mcs_log), GFP_KERNEL)))
			goto _no_mem;
		_ND(trace_2_cc_api_send_mgmt_allert, "to MGMT: @LEVEL:@HEADER:@STR (@M_ID)", level, header, message, m_id);
		__fill_mgmt_allert(cc_api, msg, level, header, message, m_id);
	}

	rv = nvmeibc_send_to_mcs(cc_api, msg, NULL, opcode);
_out:
	kfree(msg);
	return rv;

_no_mem:
	rv = -ENOMEM;
	goto _out;
}

static char *log_level_to_str(char level)
{
	switch (level) {
	case 'D': case 'd':	return "DEBUG";
	case 'I': case 'i':	return "INFO";
	case 'W': case 'w':	return "WARNING";
	case 'E': case 'e':	return "ERROR";
	}
	return NULL;	// Illegal level
}

struct mgmt_allert {
	struct nvmeibc_control_api *cc_api;	// communication channel
	char *msg;				// Will be relased if schedule fails or after sending
	u64 msg_id;				// Used to update a previous allert or ack it
	bool is_update;			// If this message is an update
	bool is_ack;			// If the message is an ack
};
#define mgmt_allert_init(m_al, _cc_api, _buf, _is_upd, _is_ack, _msg_id) ({ \
	(m_al)->cc_api    = _cc_api; \
	(m_al)->msg       = _buf;   \
	(m_al)->is_ack    = _is_ack; \
	(m_al)->is_update = _is_upd; \
	(m_al)->msg_id    = _msg_id; \
})

static int __create_mgmt_allert_from_cmd(struct mgmt_allert* m_al)
{
	int rv = 0, opcode;
	char *header = NULL, *message = NULL;
	const char *level = log_level_to_str(m_al->msg[0]);
	if (!level) {
		_NI(trace_cc_api_create_mgmt_allert_from_cmd, MAIN_IOCTL_PREFIX ": Unrecognized log_level=@LOG_LEVEL", m_al->msg[0]);
		rv = -EINVAL;
	} else {
		header  = (char*)&m_al->msg[1]; 	// Skip the the level character
		message = strchr(m_al->msg, '@');
		if (!message) {
			_NI(trace_1_cc_api_create_mgmt_allert_from_cmd, MAIN_IOCTL_PREFIX ": Sending mgmt allert without msg body");
			message = "";
		} else {
			(*message++) = '\0';		// Split header and message.
		}
		WARN_ON((m_al->is_update) && (m_al->is_ack)); // Cannot happen
		if (m_al->is_update) {
			opcode = MCS_UPDATE_MANAGEMENT_LOG_MESSAGE_MSG;
		} else if (m_al->is_ack) { // Ack only contains header and id
			opcode = MCS_ACK_MANAGEMENT_LOG_MESSAGE_MSG;
		} else
			opcode = MCS_MANAGEMENT_LOG_MESSAGE_MSG;
		rv = __send_mgmt_allert(m_al->cc_api, level, header, message, m_al->msg_id, opcode);
	}
	return rv;
}

/************************* Client - CLI communication *************************/
/* Outputs to cli:
	1. Volume status, generated by __vol_info_to_string()
	2. Error, generated by send_cli_error_reply()
	3. sprintf(cli_reply, ":%s: Token cancelled" , send_token);
*/

// Fills a cli error reply with extra info if needed
static void send_cli_error_reply(struct nvmeibc_control_api* cc_api,
					enum NVMEIBC_CLI_ERROR_TYPES e, const char *extra_info)
{
	char cli_reply[MAX_VOL_INFO_STRING];
	char *format = "ERR: -%d, %s", *info;
	int rv;
	switch (e) {
	case CANCEL_FORMAT_ERROR:	// Only a missing space is a formatting error
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "No space in cancel command");
		goto _send_to_cli;
	case CANCEL_MISSING_TOKEN:	// Cancel command without a token
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "No send token in cancel command");
		goto _send_to_cli;
	case ATTACH_MISSING_TOKEN:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "Attach command missing send token");
		goto _send_to_cli;
	case MISSING_RESERVATION_MODE:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "Attach command missing reservation mode");
		goto _send_to_cli;
	case MISSING_RESERVATION_VERSION:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "Attach command missing reservation version");
		goto _send_to_cli;
	case INVALID_IOCTL_FORMAT:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "Invalid IOCTL format");
		goto _send_to_cli;
	case INVALID_IOCTL_COMMAND:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "Invalid IOCTL command");
		goto _send_to_cli;
	case INVALID_CLI_FORMAT:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "Invalid CLI format");
		goto _send_to_cli;
	case INVALID_CLI_COMMAND:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, "Invalid CLI command");
		goto _send_to_cli;
	case CLIENT_SHUTTING_DOWN:
		info = "Ignored, Shutting Down";
		break;
	case CANCEL_INVALID_TOKEN:	// Cancel command with an invalid token
		info = "Cancel token is invalid";
		break;
	case CANCEL_TOKEN_NOT_FOUND:
		info = "Token not found cancel failed";
		break;
	case ATTACH_INVALID_TOKEN:
		info = "Attach command invalid token";
		break;
	case ATTACH_TOKEN_ALREADY_USED:
		info = "Attach command token already used";
		break;
	case INVALID_SEND_TOKEN:
		info = "Invalid send token";
		break;
	case INVALID_RESERVATION_MODE:
		info = "Invalid Reservation mode";
		break;
	case INVALID_RESERVATION_VERSION:
		info = "Invalid reservation mode";
		break;
	case INVALID_OPTIONAL_FLAGS:
		info = "Invalid optional flags";
		break;
	case DETACH_INVALID_FORMAT:
		info = "Detach invalid format, contains token";
		break;
	case CLI_INVALID_MODIFIER:
		format = "ERR: -%d, %s :%c:";
		info = "Bad cli modifier, expected u/U/v/V got";
		snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, info, extra_info);
		goto _send_to_cli;
	case MCS_CLOSE_RETRY:
		snprintf(cli_reply, MAX_VOL_INFO_STRING, "RETRY");
		goto _send_to_cli;
	default:
		_NE(error_cc_api_send_cli_error_reply, DMESG_PREFIX() ": Unknown cli error @X", e);
		return;
	}
	format = "ERR: -%d, %s: %s";
	snprintf(cli_reply, MAX_VOL_INFO_STRING, format, e, extra_info, info);
_send_to_cli:
	rv = nvmeibc_send_to_cli(cc_api, cli_reply);
	_NT(error_1_cc_api_send_cli_error_reply, DMESG_PREFIX("CLI Error: ") "replying with @CLI_REPLY, send_rv=@RV", cli_reply, rv);
}
// block device nvmesh proprietry ioctl
#define __cli_msg_is_ioctl(buf) ((buf)[0] == '#' || \
								 (buf)[0] == '@' || \
								 (buf)[0] == MAIN_IOCTL_MARKER)

static int __handle_ioctl(struct nvmeibc_control_api* cc_api, char *buf, size_t len)
{
	const struct nvmeibc_cinst_params_main *p = __get_cinst_params_from_cc_api(cc_api);
	int rv;
	(void)len;
	if (strncmp(buf, "@help",4) == 0) {		// Grand Help to all sub ioctls
		_NI_dmesg(t_c0_dp_dbg_tools, "--------- # ioctls:\n");
		nvmeibc_volume_ioctl_config(p, "|help");
		_NI_dmesg(t_c1_dp_dbg_tools, "--------- %% ioctls:\n");
		nvmeibc_main_ioctl(cc_api, "help");
		_NI_dmesg(t_c2_dp_dbg_tools, "---------@<> ioctls:\n");
		_NI_dmesg(t_c3_dp_dbg_tools, "@<D/I/W/E><Header string>@<Body string>\n");
		_NI_dmesg(t_c31_dp_dbg_tools, "---------/nvmeibc/echo cmds:\n");
		_NI_dmesg(t_c32_dp_dbg_tools, "Any string is saved to long term bin logs, @string - saved as warning");
		_NI_dmesg(t_c33_dp_dbg_tools, "#[b/u/l]0x<ptr>=0x<val> - set value in memory: l0x7fffde7fba50=0x00001");
		rv = 0;
	} else if (buf[0] == '#') // Volume IOCTL - will update volume cli_params
		rv = nvmeibc_volume_ioctl_config(p, buf+1 /*, len-1*/); // Skip the '#' prefix
	// Management logging pattern is "@(debug level - single char)(header -
	// up to 96 bytes)@(message up to 128 bytes)"
	else if (buf[0] == '@') {
		struct mgmt_allert m_al;
		mgmt_allert_init(&m_al, cc_api, buf+1, false, false, 0); // Skip the '@' prefix
		rv = __create_mgmt_allert_from_cmd(&m_al);
	} else if (buf[0] == MAIN_IOCTL_MARKER) { // Dump volume status information
		rv = nvmeibc_main_ioctl(cc_api, buf+1);	// Skip the MAIN_IOCTL_MARKER prefix
	} else // If the IOCTL is invalid
		rv = -EINVAL;
	if (!rv) {
		cc_api->ioctls.num_executed_ioctls++;
	} else {
		_NI(trace_cc_api_handle_ioctl99, "@EVENT_TAG " MAIN_IOCTL_PREFIX ": Invalid IOCTL command, rv=@RV", EV_IOCTL(), rv);
		send_cli_error_reply(cc_api, INVALID_IOCTL_COMMAND, NULL);
	}
	return rv;
}

static int __handle_cancel_cli(struct nvmeibc_control_api* cc_api, char *buf, size_t len)
{
	int i, rv = -EINVAL;
	char *send_token = NULL;
	for (i = 6;i < (int)len;i++) {
		if (buf[i] != ' ') {
			send_token = &buf[i];
			break;
		}
	}
	if (i == (int)len || !send_token) {
		if (i == (int)len)
			send_cli_error_reply(cc_api, CANCEL_FORMAT_ERROR, NULL);
		else
			send_cli_error_reply(cc_api, CANCEL_MISSING_TOKEN, NULL);
	} else {
		char cli_reply[MAX_VOL_INFO_STRING] = {0};
		snprintf(cli_reply, MAX_VOL_INFO_STRING, ":%s: Token cancelled" , send_token);
		if ((rv = nvmeibc_send_to_cli(cc_api, cli_reply)) < 0)
			_NE(error_cc_api_handle_cancel_cli, DMESG_PREFIX("CLI Error") ": Could not reply");
	}
	return rv;
}

#define CLI_FLAG_FORCE "--force"		// Force detach
#define CLI_FLAG_ABAND "--upgrade"		// Force detach the volume for software upgrade
#define CLI_FLAG_HIDDN "--hidden"		// will detach only if the volume is hidden attached (will ignore recoverers)
#define CLI_FLAG_RECOV "--recov"		// will detach only if the volume is recivery or hidden attached
#define is_upgrading_to_normal(vol, send_token)   (nvmeibc_block_is_recoverer_or_hidden(&(vol)->hdr) && !CHECK_HIDDEN__MAGIC(send_token) && !CHECK_RECOVER_MAGIC(send_token))
#define is_upgrading_to_recovery(vol, send_token) (nvmeibc_block_is_hidden(&(vol)->hdr)      && !nvmeibc_block_is_recoverer(&(vol)->hdr) &&  CHECK_RECOVER_MAGIC(send_token))

bool cli_attach_check_if_already_attached = true;
module_param(cli_attach_check_if_already_attached, bool, 0644);
MODULE_PARM_DESC(cli_attach_check_if_already_attached, "Debug only: use to simulate and test race conditions of cli attach");

// Handling attach command logic, if volume is found return status to CLI only
// Otherwise send a registerToEvents message
static int __handle_cli_attach(struct nvmeibc_control_api* cc_api, const char *token, struct nvmeibc_volume_attach_t *vat,
 							   const char *v_id, struct nvmeibc_volume *volume, const bool is_uuid)
{
	const enum nvmeibc_inst_state state = __get_from_params_main_globals_container(__get_cinst_params_from_cc_api(cc_api))->priv_sched.state;
	int rv = -EINVAL;
	struct get_client_configuration_msg *msg = NULL;
	char cli_reply[MAX_VOL_INFO_STRING];
	if (state != NVMEIBC_INST_STATE_READY) {	// Optimization for fast fail: Even if attach request would be sent to mgmt, upon configuration arrival we would fail it becasue state is not ready
		_NI(trace_cc_api_handle_cli_attach, DMESG_PREFIX("@V_ID") ": Attach request is rejected. state=@STATE", v_id, state);
		send_cli_error_reply(cc_api, CLIENT_SHUTTING_DOWN, v_id);
		goto _out;
	}

	WARN_ON(!token); // sanity
	_NT(trace_2_cc_api_handle_cli_attach, "@HDR_UUID: attach_status=@ATTACH_STATUS", v_id, (volume ? "already attached" : "attaching"));
	if (!(msg = get_client_configuration_msg_create(1))) {
		rv = -ENOMEM;
		goto _out;
	}
	if (volume && cli_attach_check_if_already_attached) {	// We are already attached
		const bool need_update = is_upgrading_to_normal(volume, token) || is_upgrading_to_recovery(volume, token);	// updating from hidden/recoverer -> visible or from hidden->recoverer
		if (!need_update) {	// Just report success and do nothing
			__fill_msg_vol_info(msg->volumes, &volume->hdr, NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED, nvmeibc_get_io_perm_for_reporting(volume->block_dev));
			__vol_info_to_string(msg->volumes, cli_reply);
			if ((rv = nvmeibc_send_to_cli(cc_api, cli_reply)) < 0 )
				_NE(error_cc_api_handle_cli_attach1, DMESG_PREFIX("CLI Error") ": Could not reply");
			goto _out;		// Already attached - nothing to do
		}
	}
	// Send registerToEvents for this volume to initiate attach - request reservation mode if upgrading recoverer
	fill_msg_vol_info_empty(msg->volumes, vat, v_id, is_uuid);
	__vol_info_to_string(msg->volumes, cli_reply);
	_ND(trace_3_cc_api_handle_cli_attach, "Sending message=@STR", &cli_reply[0]);
	__update_msg(msg, token);
	//update the upstream counter fields according to the current values
	__fill_upstream_message_header_with_basic_info(msg, cc_api);
	#if defined(__KERNEL__)
		rv = 0;(void)msg;
	#else
		rv = nvmeibc_send_to_mcs(cc_api, msg, NULL, MCS_GET_CLIENT_CONFIGURATION_MSG_MSG);
	#endif

	_NT(trace_4_cc_api_handle_cli_attach, "Sending attach request message to mcs, for @V_ID, rv=@RV", v_id, rv);

_out:
	kfree(msg);
	return rv;
}

static int __handle_cli_detach(struct nvmeibc_control_api* cc_api, struct nvmeibc_vol_detach_cmd *how, const char *v_id,
							 struct nvmeibc_volume *volume, const bool is_uuid)
{
	const enum nvmeibc_inst_state state = __get_from_params_main_globals_container(__get_cinst_params_from_cc_api(cc_api))->priv_sched.state;
	int rv = 0;
	_NT(trace_cc_api_handle_cli_detach, "handling detach, flags: hidden=@BOOL recov=@BOOL force=@BOOL aband=@BOOL, vol=@VOL is_uuid=@BOOL_YN", how->hidden, how->recov, how->force, how->abandon, !volume ? "?" : volume->full_name, is_uuid);
	WARN_ON(!how); //sanity
	if (!volume) {
		rv = send_unknown_volume_to_cli(cc_api, v_id, is_uuid);
	} else { // Call detach immediately since we are on main WQ
		if (unlikely(state != NVMEIBC_INST_STATE_READY)) {
			_NI(trace_3_cc_api_handle_cli_detach, DMESG_PREFIX("@V_ID") ": Detach request is rejected. state=@STATE", v_id, state);
			send_cli_error_reply(cc_api, CLIENT_SHUTTING_DOWN, v_id);
		} else {
			rv = try_detach_volume_with_multicomplete(volume, *how, NULL);
		}
	}

	return rv;
}

#define is_modifier_uuid(c)     ((c == 'u') || (c == 'U'))
#define is_modifier_name(c)     ((c == 'v') || (c == 'V'))
#define is_command_attach(c)    ((c == 'a') || (c == 'A'))
#define is_command_detach(c)    ((c == 'd') || (c == 'D'))
#define is_command_cancel(c)    ((c == 'c') || (c == 'C'))
#define is_command_upgrade(c)   ((c == 'u') || (c == 'U'))
#define is_command_ecaser(c)    ((c == 'e') || (c == 'E'))
#define is_command_shut_down(c) ((c == 's') || (c == 'S') || is_command_upgrade(c))
#define START_OF_VOLUME_STRING (8)						// len("attachu ") == len("detachv ") == len("attachv ") == len("detachu ")
#define UUID_NAME_MODIFIER_VOLUME_STRING_LOCATION (6)

struct cli_handler_param {	// Struct for scheduling cli msg handling on mainwq
	struct nvmeibc_control_api* cc_api;	// Pointer to cli object
	char *buf;				// string of the command (null terminated)
	size_t len;				// == strlen(buf)
};

static int __cli_msg_verify_size(struct cli_handler_param *p)
{
	if ((p->len<3) || ((p->buf[p->len-1] != 0) && (p->buf[p->len] != 0))){
		if (p->len != 0) // Invalid format too few chars
			send_cli_error_reply(p->cc_api, INVALID_CLI_FORMAT, NULL);
		//} else {/* Empty CLI command, probably just '\n' at the end, can happend when using echo */}
		return -EINVAL;		// Prevent buffer overflow attack
	}
	if (__cli_msg_is_ioctl(p->buf)) {
		if ((int)p->len > 0xFF) {
			send_cli_error_reply(p->cc_api, INVALID_IOCTL_FORMAT, NULL);
			return -EINVAL;	// Max ioctl is of length 256
		}
	} else {				// Attach/Detach commands
		if (((int)p->len <= START_OF_VOLUME_STRING)||((int)p->len > 0xFF)) {
			send_cli_error_reply(p->cc_api, INVALID_CLI_FORMAT, NULL);
			return -EINVAL;	// Illegal volume identifier (too short or too long)
		}
	}
	_NT(trace_cc_api_cli_msg_verify_size, "CLI:|@BUF_STR|", p->buf); // Safe to print it
	return p->len;
}

static char *__get_params_string(struct cli_handler_param *p)
{
	char *params_string = &p->buf[START_OF_VOLUME_STRING];
	char *param = strsep(&params_string, " ");
	if (param != NULL)
		_ND(debug_cc_api_parse_params, "Found volume ID:@DEV_NAME", param);
	return params_string;
}



#define PREEMPT_FLAG "--preempt"
#define IS_SUB_BLOCK_IO_ALLOWED_FLAG "--512"
static bool __parse_optional_attach_arguments(struct cli_handler_param *p, struct nvmeibc_volume_attach_t *vat, char *params_string)
{
	unsigned int optional_param_count = 0;
	// Set the optional arguments to default values
	vat->res.preempt = NVMEIB_C_TO_M_VOLUME_NO_PREEMPT;
	vat->res.is_512B_IO_allowed = false;

	while (params_string) {
		char *param = strsep(&params_string, " ");
		if (!param) {
			break;
		} else if (!strcmp(param, PREEMPT_FLAG)) {
			vat->res.preempt = NVMEIB_C_TO_M_VOLUME_PREEMPT;
		} else if (!strcmp(param, IS_SUB_BLOCK_IO_ALLOWED_FLAG)) {
			_NT(t_01__parse_optional_attach_arguments, "Volume will allow kernel sector alignment (512b)");
			vat->res.is_512B_IO_allowed = true;
		} else {
			_NE(t_02__parse_optional_attach_arguments, "Invalid optional attach argument flag=@STR", param);
			send_cli_error_reply(p->cc_api, INVALID_OPTIONAL_FLAGS, param);
			return false;
		}
		optional_param_count++;
	}

	if (!optional_param_count) {
		_NT(t_03__parse_optional_attach_arguments, "No optional flags set");
	}
	return true;
}

// Assumption START_OF_VOLUME_STRING - 1 is the only other ' ' char in the
// buffer so we can't replace it in this loop
// cli command format:
// <cmd name><u/v> <volume id no spaces> <attach flags>
// Attach:
// attach<u/v> <vol id> <send token> <res mode> <res ver> optional: --preempt --512
// First param is volume ID unknown length, caller function needs us to put '\0' with strsep into v_id
// cli_unique_token aka send_token is the second param, can be recoverer (MAGIC_RECOVR_ATTACH_TOKEN)
// it is made of 15 digits in string format, we verify the length and store it (recoverer is done here)
// next we verify the reservation mode (one of --RO/--RW/--EX) and store it
// next we read the reservation version into our attach_t
// if we have another parameter it must be either --preempt, --512 or both otherwise we fail the command
//
// Examples:
// attachv volume 123412341234123 --RW 0 --preempt --512
// attachu 123e4567-e89b-12d3... AAAAAAAAAAAAAAA		(**)
// attachv volume 123412341234123 --EX 10
// Recovery attach (hidden) has a unique token and does not require reservation info see (**)
static bool __parse_attach_token(char **token, struct cli_handler_param *p, struct nvmeibc_volume_attach_t *vat)
{
	char *params_string = __get_params_string(p);
	char *param = strsep(&params_string, " ");
	int rv; // For kstrtoull

	// --------------- Extract Token
	if (!param) {
		_NT(t_00_attp, "Missing send token");
		send_cli_error_reply(p->cc_api, ATTACH_MISSING_TOKEN, "NO TOKEN");
		return false;
	} else if (strlen(param) != 15) { // Verify token is used correctly
		_NT(t_01_attp, "Wrong send token @SEND_TOKEN_STR", param);
		send_cli_error_reply(p->cc_api, INVALID_SEND_TOKEN, param);
		return false;
	} else {
		*token = param;
	}

	if (CHECK_RECOVER_MAGIC(*token) || CHECK_HIDDEN__MAGIC(*token))  // Hidden volumes have no reservation info
		return true;

	// --------------- Extract Reservation mode
	param = strsep(&params_string, " ");
	if (!param) {
		_NT(t_03_attp, "Missing Reservation mode rm=@STR", params_string);
		send_cli_error_reply(p->cc_api, MISSING_RESERVATION_MODE, params_string);
		return false;
	} else if (strlen(param) != 4) { // Verify RM is used correctly (--RO,--RW,--EX)
		_NT(t_04_attp, "Wrong Reservation mode rm=@STR", param);
		send_cli_error_reply(p->cc_api, INVALID_RESERVATION_MODE, param);
		return false;
	}
	if (       param[3] == 'O') {
		vat->res.mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO;
	} else if (param[3] == 'X') {
		vat->res.mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX;
	} else if (param[3] == 'W') {
		vat->res.mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RW;
	} else {
		_NT(t_05_attp, "Wrong Reservation mode rm=@STR", param);
		send_cli_error_reply(p->cc_api, INVALID_RESERVATION_MODE, param);
		return false;
	}

	// --------------- Extract Reservation version
	param = strsep(&params_string, " ");
	if (!param) {
		_NT(t_06_attp, "Missing Reservation Version");
		send_cli_error_reply(p->cc_api, MISSING_RESERVATION_VERSION, param);
		return false;
	} else if ((rv = kstrtoull(param, 10, &vat->res.version)) < 0) {
		_NT(t_07_attp, "Invalid Reservation Version rm=@STR, rv=@RV", param, rv);
		send_cli_error_reply(p->cc_api, INVALID_RESERVATION_VERSION, param);
		return false;
	}

	// --------------- Parse and extract optional attach flags
	return __parse_optional_attach_arguments(p, vat, params_string);
}

// Assumption START_OF_VOLUME_STRING - 1 is the only other ' ' char in the, buffer so we can't replace it in this loop
// cli command format:
// <cmd name><u/v> <volume id no spaces> <detach flags>
// Detach:
// detach<v/u> <vol id> optionals: --force/--hidden/--recov/--upgrade
//
// Examples:
// detachv volume --force
// detachu 123e4567-e89b-12d3...
// detachv volume --hidden
// detachv volume --recov
// detachv volume --hidden --force --upgrade
static bool __parse_detach_flags(struct nvmeibc_vol_detach_cmd *detach_cmd, struct cli_handler_param *p)
{
	int len = p->len;
	char *param, *params_string = __get_params_string(p);
	if (!params_string)
		return true;

	param = strsep(&params_string, " ");
	while (param) {
		if (!strcmp(param, CLI_FLAG_FORCE)) {
			detach_cmd->force = true;
		} else if (!strcmp(param, CLI_FLAG_ABAND)) {
			detach_cmd->force = true;
			detach_cmd->abandon = true;
		} else if (!strcmp(param, CLI_FLAG_HIDDN)) {
			detach_cmd->hidden = true;
		} else if (!strcmp(param, CLI_FLAG_RECOV)) {
			detach_cmd->recov = true;
		} else {
			_NT(error_cc_api_parse_detach_flags, "nvmeibc bug: Invalid flag buf=@BUF_STR, len=@LEN, @SEND_TOKEN_STR", p->buf, len, param);
			send_cli_error_reply(p->cc_api, DETACH_INVALID_FORMAT, param);
			return false;
		}
		param = strsep(&params_string, " ");
	}
	return true;
}

static bool __get_volume(struct nvmeibc_volume **volume, struct cli_handler_param *p, bool *is_uuid) {
	const struct nvmeibc_cinst_params_main *i = __get_cinst_params_from_cc_api(p->cc_api);
	char *buf = p->buf;
	char uuid_modif;
	const char *v_id = &buf[START_OF_VOLUME_STRING];

	// Verify we get a valid modifier (u/U for uuid and v/V for name)
	uuid_modif = buf[UUID_NAME_MODIFIER_VOLUME_STRING_LOCATION];
	if (is_modifier_uuid(uuid_modif)) { 	// UUID modifier
		*volume = nvmeibc_volume_get_by_uuid(i, v_id, UNKNOWN_ILLEGAL);
		*is_uuid = true;
	} else if (is_modifier_name(uuid_modif)) { // Name modifier
		*volume = nvmeibc_volume_get_by_name(i, v_id, UNKNOWN_ILLEGAL);
		*is_uuid = false;
	} else {									// Unknown modifier
		send_cli_error_reply(p->cc_api, CLI_INVALID_MODIFIER, &uuid_modif);
		return false;
	}

	return true;
}

/* Return value: Amount of bytes processed (0..len-1 are errors, len is OK) */
static int handle_cli_msg_on_mainwq(void *param)
{
	struct nvmeibc_volume *volume;
	struct cli_handler_param *cli_params = param;
	struct nvmeibc_control_api* cc_api = cli_params->cc_api;
	char *buf = cli_params->buf;
	size_t len = cli_params->len;
	int rv = 0;
	bool is_uuid = false;
	const char *v_id = &buf[START_OF_VOLUME_STRING];
	__verify_on_main_wq_ccapi(cc_api);

	// Verify CLI command is OK
	if (__cli_msg_verify_size(cli_params) < 0) {
		_NT(trace_cc_api_handle_cli_msg_on_mainwq, "CLI got illegal command=@PTR len=@LEN", buf, (int)len);
		goto _out;	// Illegal volume identifier (command too short or too long)
	}

	// Handle IOCTL commands
	if (__cli_msg_is_ioctl(buf)) {
		rv = __handle_ioctl(cc_api, buf, len);
		goto _out;
	}

	//	Handle cancel operation "Cancel 000000000000001"
	if (is_command_cancel(buf[0])) {
		rv = __handle_cancel_cli(cc_api, buf, len);
		goto _out;
	}
	if (is_command_shut_down(buf[0])) {
		WARN(1, "nvmeibc: illegal cmd=%16s", buf);	// Forbidden to handle them on mainwq
		goto _out;
	}

	// Handle attach/detach commands
	if (is_command_attach(buf[0])) {  // Attach
		char *token;
		struct nvmeibc_volume_attach_t vat;
		nvmeibc_volume_attach_t_init(&vat);
		if (!__parse_attach_token(&token, cli_params, &vat) || !__get_volume(&volume, cli_params, &is_uuid))
			goto _out;
		rv = __handle_cli_attach(cc_api, token, &vat, v_id, volume, is_uuid);
	} else if (is_command_detach(buf[0])) {		// Detach
		struct nvmeibc_vol_detach_cmd detach_cmd = nvmeibc_vol_detach_cmd_nice();
		if (!__parse_detach_flags(&detach_cmd, cli_params) || !__get_volume(&volume, cli_params, &is_uuid))
			goto _out;
		rv = __handle_cli_detach(cc_api, &detach_cmd, v_id, volume, is_uuid);
	} else { // Unknown command
		send_cli_error_reply(cc_api, INVALID_CLI_COMMAND, NULL);
	}
_out:
	kfree(cli_params->buf);
	kfree(cli_params);
	return rv;
}

static void __on_atom_detaching_increment_counter(const struct nvmeiba_atom_os_api *atom, void* ctx)
{
	int *n_detaching_volumes = (int *)ctx;

	if (atom->status == nvmeiba_status_detaching)
		(*n_detaching_volumes)++;
}

/* in case we want to also catch in-flight detaches (e.g. currently processed in WQ and
 * such) a practice similar to how inflight read partitions are counted can be used. */

static int __count_detaching_atoms(void)
{
	int n_detaching_volumes = 0;

	nvmeiba_os_api_exec_for_each_atom(NULL, __on_atom_detaching_increment_counter, (void *)&n_detaching_volumes);

	return n_detaching_volumes;
}

static int __count_shutdown_preventors(struct nvmeibc_control_api *cc_api)
{
	extern int nvmeibc_os_api_layer_get_num_read_part(const struct nvmeibc_cinst_params_blk *p, bool *is_on);
	const struct nvmeibc_cinst_params_main *main_p = __get_cinst_params_from_cc_api(cc_api);
	const struct nvmeibc_cinst_params_blk  *blk_p = nvmeibc_isnt_params_main2blk(main_p);
	int n_shutdown_preventers = 0;
	bool is_read_part_on = false;

	n_shutdown_preventers = __count_detaching_atoms();
	if (n_shutdown_preventers)
		goto out;

	n_shutdown_preventers = nvmeibc_os_api_layer_get_num_read_part(blk_p, &is_read_part_on);	// Todo: Here take into account also other instances and mtv
	if (!is_read_part_on) {
		_NW(t_w3_cc_api, DMESG_MOD_PREFIX ": dangerous flow, hot upgrade launched without preparation");
	}

out:
	return n_shutdown_preventers;
}

static int __handle_shutdown_instruction(struct nvmeibc_control_api *cc_api, struct cli_handler_param *param)
{
	const bool is_upgrade = is_command_upgrade(param->buf[0]);
	int n_shutdown_preventers = 0;
	int rv = 0;
	_NI(t_w2_cc_api, DMESG_MOD_PREFIX ": service @BUF_STR requested", param->buf);

	if (is_upgrade) {
		n_shutdown_preventers = __count_shutdown_preventors(cc_api);
	}

	if (n_shutdown_preventers == 0) {
		if (is_upgrade) {
			const struct nvmeibc_cinst_params_main *main_p = __get_cinst_params_from_cc_api(cc_api);
			const struct nvmeibc_cinst_params_core *core_p = nvmeibc_isnt_params_main2core(main_p);
			nvmeibc_cinst_prep_upgrade_shutdown(core_p);
		}
		nvmeibc_instance_do_blocking(NULL, mw_clean_all_vols_of_all_inst, is_upgrade);
	} else {
		_NW(t_w4_cc_api, DMESG_MOD_PREFIX ": rejecting shutdown request. @INT preventers", n_shutdown_preventers);
		rv = -1;
	}
	kfree(param->buf);
	kfree(param);
	return rv;
}
/* Core unitest - unload hook */
#ifdef CORE_UNITEST
void destroy_corecomm(void);
#else
#define destroy_corecomm(...)
#endif

/* Input buf-string of N characters + '\0'. len = N*/
static int __handle_multiple_cli_lines(struct nvmeibc_control_api *cc_api, char *buf, size_t remain_len)
{
	int		n_lines = 0;
	size_t cur_len;
	struct cli_handler_param *param;
	char *next_line;
	int rv = -EIO;

	if (buf[remain_len] != 0) { // Daniel: Todo, remove in the final product
		const char *l = &buf[remain_len-1];
		WARN(true, DMESG_PREFIX(": ") "currupted buffer = {%c,%c,%c}", l[0], l[1], l[2]);
		goto _out;
	}
	do { // Single line CLI handling
		next_line = strchr(buf, '\n');
		cur_len   = (next_line ? (size_t)(next_line - buf) : remain_len);
		if (!(param = kzalloc(sizeof(*param), GFP_KERNEL)))
			goto _out;
		param->cc_api = cc_api;
		param->len = cur_len + 1;	// Ensuring a '\0' at the end
		if (!(param->buf = kzalloc(param->len, GFP_KERNEL))) {
			goto _err;
		}
		memcpy(param->buf, buf, cur_len);
		buf        += param->len;	// skip cur_line and ('\n' or '\0')
		remain_len -= param->len;
		param->len = nvmeib_remove_unsafe_symbols(param->buf, param->buf);
		if (unlikely(is_command_shut_down(param->buf[0]))) {
			if (__handle_shutdown_instruction(cc_api, param)) {
				rv = -EBUSY; /* marking we can't do shutdown now */
				goto _out; /* param->buf/param are freed on the callee */
			}
			destroy_corecomm();
		} else if (Nschedule_on_main_wq(trace_e3_ccapi, cc_api, handle_cli_msg_on_mainwq, param)<0) {
			goto _err;
		} // Note: here 'param' is already free(). Do not access it
		n_lines++;
		// When exiting, remaining length is -1 because we used the last '\0'
	} while (next_line);
	rv = n_lines; /* success */
	goto _out;
_err:
	kfree(param->buf);
	kfree(param);
_out:
	return rv;
}

/* Return value: Amount of bytes processed (0..len-1 are errors, len is OK) */
int schedule_handle_cli_msg(void *args, char *buf, size_t len, bool *posted)
{
	struct c_api_proc *cli = container_of(args, struct c_api_proc, handle);
	struct nvmeibc_control_api *cc_api = container_of(cli, struct nvmeibc_control_api, cli);
	int rv;
	(void)posted;		// We do not allocate any memory
	if (len == 0)
		return -EINVAL;	// Returning 0 may put user mode app in infinte loop
	rv = __handle_multiple_cli_lines(cc_api, buf, len);
	if (rv >= 0) {
		_ND(trace_cc_api_schedule_handle_cli_msg, "Scheduled @RV cli commands on main workqueue", rv);
		rv = len;
	}
	return rv;
}

static int __cli_retry(void *cli_handle) {
	struct c_api_proc *cli = container_of(cli_handle, struct c_api_proc, handle);
	struct nvmeibc_control_api *cc_api = container_of(cli, struct nvmeibc_control_api, cli);
	_NT(t_cli_retry, "Requesting retry after MCS close");
	send_cli_error_reply(cc_api, MCS_CLOSE_RETRY, NULL);
	return 0;
}

static void __fill_version(char* version, size_t maxsize)
{
	int const rv = snprintf(version, maxsize, "%s-%s.%s", __stringify(NVMESH_VERSION), __stringify(NVMESH_RELEASE), __stringify(BUILD_NUMBER));
	BUG_ON(rv < 0); //this means we have encoding problem - should fail immediatelly in the QA tests
	if (rv > 0) {
		BUG_ON(maxsize < (size_t)rv); //this means that we don't have enough space to print the version
	}
}


static int __heartbeat_to_mcs(struct nvmeibc_control_api* cc_api)
{
	struct nvmeibc_client_keep_alive_message *msg = NULL;
	const struct nvmeibc_cinst_params_main * cinst = __get_cinst_params_from_cc_api(cc_api);
	int rv;
	int vol_idx = 0;
	const int n_vols = nvmeibc_get_volumes_num(cinst, NORMAL_VOLUME);
	const int n_vols_under_recovery = nvmeibc_get_masked_volumes_num(cinst, RECOVERER_VOLUME);
	const int n_shadow_vols = nvmeibc_get_masked_volumes_num(cinst, SHADOW_VOLUME);
	const size_t attch_uuid_size = n_vols * sizeof(msg->attachmentsUUIDHash[0]);
	const size_t msg_size = sizeof(*msg) + attch_uuid_size;
	struct nvmeibc_volume *curr_volume = NULL;

	__verify_on_main_wq_ccapi(cc_api);
	if (!(msg = kzalloc(msg_size, GFP_KERNEL)))
		return -ENOMEM;
	
	__fill_version(msg->version, sizeof(msg->version));
	__fill_upstream_message_header_with_basic_info(msg, cc_api);
	__fill_generic_message_with_minimal_basic_info(msg, cc_api);

	msg->configProfile = cinst->cfg_profile;
	msg->attachmentsUUIDHash = (void*)(&msg[1]);
	msg->attachmentsVersion = cc_api->latest_attachment_version;
	msg->hasWIPOperations = (cc_api->processing_multi_vol_cmd ||
				 !cc_api->is_mcs_cache_replayed_completed) ? 1 : 0;
	msg->volumeVersionsSum = 0;
	msg->nHiddenVolumes = n_vols_under_recovery + n_shadow_vols;

	if (n_vols) { // Some volumes have new io_perms add them
		list_for_each_entry(curr_volume, nvmeibc_get_volumes(cinst), link) {
			struct volume_version_id *curr_vol_id = msg->attachmentsUUIDHash + vol_idx;

			if (curr_volume->hdr.type != NORMAL_VOLUME){
				_NT(t_2___heartbeat_to_mcs4, DMESG_PREFIX("@DEV_NAME: ") "@HDR_UUID, @HDR_TYPE - skipped from keepalive",
					curr_volume->hdr.devname, curr_volume->hdr.uuid, curr_volume->hdr.type);
				continue;
			}

			if (curr_volume->status != NVS_ATTACHED)
				msg->hasWIPOperations = 1;

			BUILD_BUG_ON(sizeof(curr_vol_id->val) < sizeof(curr_volume->hdr.uuid));
			strlcpy(curr_vol_id->val, curr_volume->hdr.uuid, sizeof(curr_vol_id->val));

			msg->volumeVersionsSum += curr_volume->hdr.version;

			vol_idx += 1;
		}
		BUG_ON(vol_idx != n_vols);
		msg->n_vols_id = n_vols;
	}

	_NT(t_1___heartbeat_to_mcs, "Sending a keepalive msg with: messageSequence=@COUNTS, clientToken=@COUNTS, keepaliveInterval=@UINT, attachmentsVersion=@INT. Current client reportID=@COUNTS",
		msg->upstream_header.messageSequence, msg->upstream_header.clientToken,
		msg->upstream_header.keepaliveInterval, cc_api->latest_attachment_version, cc_api->clnt_2_mgmt_report_id);
	rv = nvmeibc_send_to_mcs(cc_api, msg, NULL, MCS_CLIENT_KEEP_ALIVE_MSG);
	kfree(msg);
	return rv;
}

static int __heartbeat_to_mcs_check(void *_p)
{
	struct c_api_perrep *param = _p;
	struct nvmeibc_control_api* cc_api = container_of(_p, struct nvmeibc_control_api, heartbeat);
	// Get the current counter value
	unsigned long counter = param->delay_jiffies++;
	if (counter % get_management_report_frequency()) { // When frequency is reached send report
		return 0; // otherwise schedule delayed wq for another second
	}
	_NT(t_1__heartbeat_to_mcs_check, "It's time to send a keepalive - jiffies=@_JIFFIES", counter);
	return __heartbeat_to_mcs(cc_api);
}

static bool __update_clnt_upstream_arg_from_mgmt(long long *var, long long new_val)
{
	//Use the maximum between the current value and that received from mgmt
	if (*var > new_val && *var >= 0) {
		_NT(__update_clnt_upstream_arg_from_mgmt, "Updated value was smaller than the current counter value, ignoring it");
	} else if (*var < new_val) {
		*var = new_val;
		return true;
	}
	return false;
}

static bool __update_clnt_token_from_mgmt(struct nvmeibc_control_api* ccapi, long long new_val)
{
	bool clnt_token_updated = false;
	const long long current_val = ccapi->clnt_2_mgmt_fullconf_token;
	if(__update_clnt_upstream_arg_from_mgmt(&ccapi->clnt_2_mgmt_fullconf_token, new_val)){
		_NT(t_1a_update_clnt_token_cc_api, "updated client token - new token num=@COUNTS, previous=@COUNTS", new_val, current_val);
		clnt_token_updated = true;
	} else {
		_NT(t_1b_update_clnt_token_cc_api, "no need to update client token - current token=@COUNTS, new_val=@COUNTS", current_val, new_val);
	}
	return clnt_token_updated;
}

static void __update_clnt_sequence_num_from_mgmt(struct nvmeibc_control_api* ccapi, long long new_val)
{
	long long current_val = ccapi->clnt_2_mgmt_sequence_id;
	if(__update_clnt_upstream_arg_from_mgmt(&ccapi->clnt_2_mgmt_sequence_id, new_val)){
		_NT(t_1a_update_clnt_sequence_num_cc_api, "updated client sequence number - new sequence num=@COUNTS, previous=@COUNTS", new_val, current_val);
	} else {
		_NT(t_1b_update_clnt_sequence_num_cc_api, "no need to update client sequence number - current sequence num=@COUNTS, new_val=@COUNTS", current_val, new_val);
	}
}

static void __update_clnt_report_id_from_mgmt(struct nvmeibc_control_api* ccapi, long long new_val)
{
	long long const current_val = ccapi->clnt_2_mgmt_report_id;
	//In case mgmt know of a smaller report id, we need to update them that we have a newer version
	if(__update_clnt_upstream_arg_from_mgmt(&ccapi->clnt_2_mgmt_report_id, new_val)){
		_NT(t_1a_update_clnt_report_id_cc_api, "updated client report ID: new=@COUNTS, previous=@COUNTS", new_val, current_val);
	} else {
		_NT(t_1b_update_clnt_report_id_cc_api, "no need to update client report ID - current report ID num=@COUNTS, new_val=@COUNTS", current_val, new_val);
	}
}

static int __update_current_attachment_version_from_mgmt(struct nvmeibc_control_api* ccapi, int new_version, const char *dev_name) {
	const int current_version = ccapi->latest_attachment_version;

	_NT(t_12l_cc_api, "@STR: new_version=@INT was old_version=@INT", dev_name, new_version, current_version);
	ccapi->latest_attachment_version = max(new_version, current_version);

	return 0;
}

static inline void __set_keepalive_interval(struct nvmeibc_control_api* cc_api, unsigned int keepaliveInterval)
{
	if (!keepaliveInterval) {
		_NT(w_1__set_keepalive_interval, "keepaliveInterval to set was zero. Resting KA interval to default (@INT)", DEFAULT_MANAGEMENT_REPORT_FREQUENCY);
		management_report_frequency = DEFAULT_MANAGEMENT_REPORT_FREQUENCY;
	} else if (management_report_frequency != (int)keepaliveInterval) {
		_NT(t_1__set_keepalive_interval,
		    "Client updating KA interval: @INT-->@INT",
		    management_report_frequency, keepaliveInterval);
		management_report_frequency = keepaliveInterval;
	} else {
		_NT(t_3__set_keepalive_interval, "No change in KA interval (@INT)", management_report_frequency);
	}
	//reset the delay to 1 to ensure that the management will not be flooded with keepalives
	cc_api->heartbeat.delay_jiffies = 1;
}

static void __send_all_volumes_status_upstream(struct nvmeibc_control_api *cc_api)
{
	const struct nvmeibc_cinst_params_main *cinst = __get_cinst_params_from_cc_api(cc_api);
	struct nvmeibc_volume *volume = NULL;
	int n_vols_total = 0, n_vols_skipped = 0;
	NFIN;

	list_for_each_entry(volume, nvmeibc_get_volumes(cinst), link) {
		++n_vols_total;
		if (false == nvmeibc_block_is_during_attach_stabilization_period(volume->block_dev)){
			__nvmeibc_cc_api_notify_vol_io_changed(volume, cinst);
		} else {
			++n_vols_skipped;
		}

	}
	_NT(t_1__send_volume_status_upstream, "Sent status of @INT volumes upstream after setting client token; @INT volumes were skipped", n_vols_total, n_vols_skipped);

	NFOUT;
}

static int __client_update_upstream_params(void *_p) {
	struct mcs_handler_param *p = (struct mcs_handler_param*)_p;
	struct nvmeibc_control_api* cc_api = p->cc_api;
	int rv = 0;
	bool was_client_token_updated;
	const long long last_token_val = cc_api->clnt_2_mgmt_fullconf_token;

	__verify_on_main_wq_ccapi(cc_api);

	was_client_token_updated = __update_clnt_token_from_mgmt(cc_api, p->client_token_to_set);
	//client token
	if(was_client_token_updated) {
		/*If mgmt set a new client token, they assume the client is new. We need to set the attachment version if that ver is bigger than the current one */
		_NT(t_1__client_update_upstream_params,
		    "Client token updated, need to test if last attachment version should be updated from (@INT) to @INT",
		    cc_api->latest_attachment_version, p->attachment_version);
		if (p->attachment_version > cc_api->latest_attachment_version) {
			_NT(t_1a__client_update_upstream_params, "updated attachment version to @INT", p->attachment_version);
			cc_api->latest_attachment_version = p->attachment_version;
		}
	}

	//sequence number
	__update_clnt_sequence_num_from_mgmt(cc_api, p->sequence_id_to_set);

	//report id
	__update_clnt_report_id_from_mgmt(cc_api, p->report_id_to_set);

	__set_keepalive_interval(cc_api, p->keepaliveInterval);

	_NT(t_2_client_update_upstream_params_cc_api, "Client updated upstream params: clientToken=@COUNTS, reportID=@COUNTS, messageSequence=@COUNTS, attachmentsVersion=@INT", cc_api->clnt_2_mgmt_fullconf_token, cc_api->clnt_2_mgmt_report_id, cc_api->clnt_2_mgmt_sequence_id, cc_api->latest_attachment_version);
	if (was_client_token_updated && last_token_val == DEFAULT_UPSTREAM_VALUE) { //resend all volume status if after setting token.
		__send_all_volumes_status_upstream(cc_api);
	}
	kfree(p);
	return rv;
}

void nvmeibc_cc_api_request_volume_config_in_atomic_context(const struct nvmeibc_cinst_params_main *p, const char *vol_name)
{
	struct nvmeibc_control_api *cc_api = &__get_from_params_main_globals_container(p)->cc_api;
	(void)__schedule_request_config(cc_api, vol_name, "Toma request");
}

static int __create_mgmt_allert_from_cmd_and_free(void *param) {
	struct mgmt_allert *m_al = param;
	__create_mgmt_allert_from_cmd(m_al);
	kfree(m_al->msg);
	kfree(m_al);
	return 0;
}

int nvmeibc_cc_api_send_mgmt_allert(const struct nvmeibc_cinst_params_main *p, char *msg, unsigned long msg_id, bool is_update)
{
	struct t_main_clnt_globals * _mg = __get_from_params_main_globals_container(p);
	struct nvmeibc_control_api *cc_api = &_mg->cc_api;
	int rv = 0;
	struct mgmt_allert *m_al = kzalloc(sizeof(*m_al), GFP_ATOMIC);
	if (!m_al) {
		rv = -ENOMEM;
		goto _out;
	}
	mgmt_allert_init(m_al, cc_api, msg, is_update, false, msg_id);
	_NT(trace_cc_api_nvmeibc_cc_api_send_mgmt_allert, "@MSG_STR", msg);
	rv = Nschedule_on_main_wq(trace_e4_ccapi, cc_api,  __create_mgmt_allert_from_cmd_and_free, (void*)m_al);
_out:
	if (rv < 0) {
		kfree(msg);
		kfree(m_al);
	}
	return rv;

}

//Whenever someone connects to the procfs, immediately sends a keepalive
static int __mcs_init(void *mcs_handle)
{
	struct c_api_proc *mcs = container_of(mcs_handle, struct c_api_proc, handle);
	struct nvmeibc_control_api *cc_api = container_of(mcs, struct nvmeibc_control_api, mcs);
	__verify_on_main_wq_ccapi(cc_api);
	cc_api->processing_multi_vol_cmd = false;
	// after connection, if there are cached messages send them
	if (cc_api->clnt_2_mgmt_fullconf_token >= 0) {// i.e mcs went down and came back
		size_t n_erased_msgs = 0;
		_NT(trace_cc_api_mcs_reinit,
		    "Someone re-connected to procfs. Sending a cache to mgmt via MCS num_messages=@INT",
		    nvmeib_mcs_cache_size(cc_api->mcs.handle));

		n_erased_msgs = nvmeib_msgloop_flush(cc_api->mcs.msg_loop);
		if (n_erased_msgs){
			_NW(trace_cc_api_mcs_erased_msgs, "mcs msgloop erased @COUNTS messages", n_erased_msgs);
		}

		/* It's safe to disregard errors from nvmeib_mcs_send_cached_msg.
		 * This function can only fail if the message loop is in an error state.
		 * However, it's guaranteed to be called again once the message loop exits this error state.
		 */
		if (nvmeib_mcs_send_cached_msg(cc_api->mcs.handle, cc_api->mcs.msg_loop)) {
			_NW(trace_cc_api_mcs_failed_to_send_cache, "Failed to send cache in mcs_init");
		}
	} else {
		_NT(trace_cc_api_mcs_init,
		    "Someone connected to procfs. Sending a Keepalive to mgmt via MCS");
		__heartbeat_to_mcs(cc_api);
	}
	//We just sent a keepalive, make sure to resent the next one according to schedule
	cc_api->heartbeat.delay_jiffies = 1;
	return 0;
}

//AK: TODO - since mgmt transition to Apache Kafka the following is not longer correct.
// The client does nothing during start up and will get configuration from management
// Need to update/remove after integration

// Client init MCS protocol with MGMT
//		MGMT(r) -- A -- CM(l) -- B -- Client(l) r=remote, l=local
// On first boot has no meaning (no volumes are attached)
// If it's a client restart the persistency part of the CLI will attach volumes
// one by one (client detached the volumes before going down on good shutdown)
// Otherwise we have Multiple events registration (Since A remains the same and
// no one "detached" the volumes in order)
// If a socket is re-opened (B went down):
// 		1. (A went down -> Does this cause B to go down as well? TBD)
// 		   Mgmt went down long live the new mgmt (either CM will send all
// 		   resend_on_failure messages) or we send the init with a list of volumes
// 		   (or both)
//		2. (B only went down)Client is disconnected from CM only (both are alive)
// 			- We send init message to MCS causing Multiple event registration
// 		3. (A and B go down)CM went down (mgmt unregisters this client from all
// 			messages) - Once it's back we MUST send the init message
// 		4. (B goes down but client has no attached volumes thus mimiking restart)
// 			If Client went down see Client restart above.
//
static void nvmeib_mcs_init_protocol(void *mcs_handle)
{
	struct c_api_proc *mcs = container_of(mcs_handle, struct c_api_proc, handle);
	struct nvmeibc_control_api *cc_api = container_of(mcs, struct nvmeibc_control_api, mcs);
	_NT(t_1o_cc_api, "/proc/@STR - open()", mcs->name);
	Nschedule_on_main_wq(trace_e5_ccapi, cc_api, __mcs_init, mcs_handle);
}

static void nvmeib_mcs_done_protocol(void *mcs_handle)
{
	struct c_api_proc *mcs = container_of(mcs_handle, struct c_api_proc, handle);
	struct nvmeibc_control_api *cc_api = container_of(mcs, struct nvmeibc_control_api, mcs);
	_NT(t_1p_cc_api, "/proc/@STR - close()", mcs->name);
	// this is required in case TOMA uses the attach script and we lose the mgmt connection, this way the cli will retry the request and won't time out.
	Nschedule_on_main_wq(trace_e6_ccapi, cc_api, __cli_retry, &cc_api->cli.handle);
}

struct query_target_nics_args {
	const struct nvmeibc_cinst_params_main *cinst;
	struct nvmeibc_target_nics_query query;
};

static const int SPECIAL_NIC_VERSION_QUERY_BY_NAME = INT_MIN;

struct nvmeibc_target;
extern bool nvmeibc_target_fill_nics_query_by_node_id(const struct nvmeibc_cinst_params_main * cinst, struct nvmeibc_target_nics_query* query);
extern struct nvmeibc_target* nvmeibc_target_should_send_nics_query(const struct nvmeibc_cinst_params_main * cinst, struct nvmeibc_target_nics_query* query);
extern void nvmeibc_target_nics_query_was_sent(struct nvmeibc_target* target);

static bool is_get_target_nics_query_allowed(struct nvmeibc_control_api *cc_api)
{
	bool rv = cc_api->is_mcs_cache_replayed_completed;
	if (!rv) {
		_NT(query_target_nics_dropped, "dropping get target nics message, mcs didn't finish playing cache");
	}
	return rv;
}

static int __query_target_nics(void* args_)
{
	struct query_target_nics_args* args = args_;
	struct t_main_clnt_globals * _mg = __get_from_params_main_globals_container(args->cinst);
	struct nvmeibc_control_api *cc_api = &_mg->cc_api;
	struct nvmeib_client_to_mgmt_get_target_nics* msg = NULL;
	struct nvmeibc_target* target = NULL;

	int rv;

	__verify_on_main_wq_ccapi(cc_api);

	// if cache is not fully replayed, we assume that target_nics will arrive.
	if (!is_get_target_nics_query_allowed(cc_api)) {
		rv = 0;
		goto _out;
	}

	if (args->query.nicsVersion == SPECIAL_NIC_VERSION_QUERY_BY_NAME) {
		bool const query_was_updated = nvmeibc_target_fill_nics_query_by_node_id(args->cinst, &args->query);
		if (false == query_was_updated){
			_NT(t_0___query_target_nics, "dropping get target nics request - target(@NODE_ID_STR) was not found", args->query.node_id);
			rv = 0;
			goto _out;
		}
	}

	target = nvmeibc_target_should_send_nics_query(args->cinst, &args->query);
	if (!target){
		_NT(t_1___query_target_nics, "dropping get target(@NODE_ID_STR) nics request - the relevant request was send less then 10 seconds ago", args->query.node_id);
		rv = 0;
		goto _out;
	}

	if (!(msg = kzalloc(sizeof(*msg) + sizeof(msg->targets[0]), GFP_KERNEL))){
		_NT(t_2___query_target_nics, "dropping get target(@NODE_ID_STR) nics request - ENOMEM", args->query.node_id);
		rv = -ENOMEM;
		goto _out;
	}

	__fill_version(msg->version, sizeof(msg->version));
	__fill_upstream_message_header_with_basic_info(msg, cc_api);
	__fill_generic_message_with_minimal_basic_info(msg, cc_api);

	msg->n_targets = 1;
	msg->targets = (void*)(&msg[1]);
	memcpy(msg->targets, &(args->query), sizeof(msg->targets[0]));
	//msg->targets[0] = args->query;

	_NI(t_3___query_target_nics,
	    "@EVENT_TAG Sending get target(@NODE_ID_STR) nics(version=@INT) msg with: messageSequence=@COUNTS, clientToken=@COUNTS, keepaliveInterval=@UINT, attachmentsVersion=@INT. Current client reportID=@COUNTS",
	    EV_GET_TGT_NICS(), args->query.node_id, args->query.nicsVersion,
	    msg->upstream_header.messageSequence, msg->upstream_header.clientToken,
	    msg->upstream_header.keepaliveInterval, cc_api->latest_attachment_version, cc_api->clnt_2_mgmt_report_id);
	rv = nvmeibc_send_to_mcs(cc_api, msg, &msg->targets[0].nodeUUID, MCS_GET_TARGETS_NICS_MSG);
	if (rv == 0){
		nvmeibc_target_nics_query_was_sent(target);
	}
_out:
	kfree(msg);
	kfree(args);
	return rv;
}

int nvmeibc_cc_api_query_target_nics(const struct nvmeibc_cinst_params_main *cinst, struct nvmeibc_target_nics_query query)
{
	int rv = 0;
	struct query_target_nics_args *args = NULL;

	_NT(query_target_nics_0, "target @NODE_ID_STR, nics version @INT", query.node_id, query.nicsVersion);

	args = kzalloc(sizeof(*args), GFP_KERNEL);
	if (!args){
		_NE(query_target_nics_1, "target @NODE_ID_STR, nics version @INT - failed ENOMEM", query.node_id, query.nicsVersion);
		return -ENOMEM;
	}
	args->cinst = cinst;
	args->query = query;

	rv = nvmeibc_run_on_main_wq1(cinst, __query_target_nics, args);
	if (rv) {
		_NE(query_target_nics_2, "target @NODE_ID_STR, nics version @INT - failed workqueue(@RV)", query.node_id, query.nicsVersion, rv);
		kfree(args);
		return rv;
	}
	return 0;
}


int nvmeibc_cc_api_query_target_nics_by_node_id(const struct nvmeibc_cinst_params_main *cinst, const char* target_node_id)
{
	struct nvmeibc_target_nics_query query = {.nicsVersion=SPECIAL_NIC_VERSION_QUERY_BY_NAME};
	strlcpy(query.node_id, target_node_id, sizeof(query.node_id));
	_NT(query_target_nics_by_node_id, "query target @NODE_ID_STR, nics by node_id @NODE_ID_STR", target_node_id, query.node_id);
	return nvmeibc_cc_api_query_target_nics(cinst, query);
}
