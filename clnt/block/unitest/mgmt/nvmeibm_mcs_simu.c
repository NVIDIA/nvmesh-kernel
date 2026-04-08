/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/* Communication simulator includes 3 main parts:
   1. MCS simulator - recieveing and sending MCS messages from and to client
   2. Management simulator - recieveing and sending msgs to client */
#include "nvmeibm_mcs_simu.h"
#include "nvmeib_mcs_header.h"
#include "nvmeibc_cc_api.h"
#include "nvmeibc_mcs_stub.h"
#include "uni_framework/range_algorithms.h"
#include "autogen/clnt/nvmeibc_mcs_stub.h"

/* mgmt receives a request from client (get vol conf) and replies with conf of this specific volume */
typedef struct {             		// This will allow Management to reply to Client's requests asynchronously
	eCPU_thread_internal_params;
	struct mcs_simu *mcs;
	int opcode;
	const struct volumeDescriptor *vol;
	char cli_unique_id[16];
	int preempt;
} t_async_mgmt_params;

static eCPU_cb_ret_type __async_generate_mcs_msg(eCPU_cb_param_list) {
	t_async_mgmt_params *p = eCPU_thread_extract_param(t_async_mgmt_params);
	eCPU_thread_start_execution(p);
	generate_mcs_attach_message_for_volume(p->mcs, p->vol, p->opcode, p->preempt, &p->cli_unique_id[0], NVMEIB_MCS_MSG_WITH_NO_ERROR);
	eCPU_thread_end_execution(p);
}

static inline void _fill_message_type_version(unsigned int *message_type_version, const enum NVMEIB_MCS_MSG_ERROR_TYPES error) {
	*message_type_version = (NVMEIB_MCS_MSG_WITH_BAD_MESSAGE_VER == error)?0:SUPPORTED_MCS_PROTOCOL_VERSION;
}

static int mcs_gather(struct mcs_simu *mcs, struct mcs_info *info, void *msg, int opcode, const enum NVMEIB_MCS_MSG_ERROR_TYPES error);

static int generate_mcs_error_message_from_payload(struct mcs_simu *mcs, struct nvmeibc_volume_status_payload *req, const char *cli_unique_id, const char *err)
{
	void *new_info;
	int rv = -ENOMEM;
	struct nvmeib_mcs_error *msg = sim_kzalloc(sizeof(*msg) + sizeof(*msg->errors), GFP_KERNEL);
	if (!msg) {
		return rv;
	}
	_fill_message_type_version(&msg->messageTypeVersion, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	msg->n_errors = 1;
	msg->errors = (void*)(&msg[1]);

	strlcpy(msg->errors[0].cli_unique_id, cli_unique_id, sizeof(msg->errors[0].cli_unique_id));
	strlcpy(msg->errors[0].entityName, req->name, sizeof(msg->errors[0].entityName));
	strlcpy(msg->errors[0].entityUUID, req->uuid, sizeof(msg->errors[0].entityUUID));
	strlcpy(msg->errors[0].err, err, sizeof(msg->errors[0].err));
	new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
	rv = mcs_gather(mcs, new_info, msg, MCS_ERROR_RESPONSE_MSG, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	sim_kfree(new_info);
	sim_kfree(msg);
	return rv;
}

const char *mgmt_2_clnt_err_strings[] = {
	"Request cannot be fulfill, the requested RM doesn't comply with the volume RM, please use the preempt flag to transition.",
	"The reservation version is outdated, current reservation version is:",
	"Could not find the volume",
};

static void __mgmt_reply_with_vol_conf_on_clnts_request(struct mcs_simu *mcs, struct nvmeibc_volume_status_payload *req, const char *cli_unique_id) {
	const struct mongo_db_simu *mdb = mcs_simu_get_mdb(mcs);
	bool find_by_uuid = (req->uuid[0] != 0);
	int opcode = -1 /* Trap */, v;
	for (v=0; v<mdb->nVols; v++) {
		const struct volumeDescriptor *vol = &mdb->vols[v];
		int rv;
		if (((!find_by_uuid) && (!strcmp(req->name, vol->info.devname))) ||
		    (( find_by_uuid) && (!strcmp(req->uuid, vol->info.uuid)))) {
			switch (vol->nextCmd) {
			case volCmds_AttachReadOnly:
			case volCmds_AttachExclusive:
			case volCmds_ShadowAttach:
			case volCmds_RecoveryAttach:
			case volCmds_Update:
			case volCmds_New:    opcode = MCS_ATTACH_VOLUMES_MESSAGE_MSG; break;
			case volCmds_Illegal: continue;		/* Client requested but */break; /* unitest env forbids mgmt from giving it to client */
			default: BUG();
			}

			if ((rv = nvmeibc_volume_attach_request_update_reservation_info((struct volumeDescriptor *)vol, &req->reservation.preempt, req->reservation.mode, req->reservation.version, req->reservation.is_512B_IO_allowed)) < 0) {
				if (rv == -EACCES)      generate_mcs_error_message_from_payload(mcs, req, cli_unique_id, mgmt_2_clnt_err_strings[0]);
				else if (rv == -EPERM)  generate_mcs_error_message_from_payload(mcs, req, cli_unique_id, mgmt_2_clnt_err_strings[1]);
				else if (rv == -EINVAL) generate_mcs_attach_message_for_volume_with_res_version(mcs, (struct volumeDescriptor *)vol, MCS_ATTACH_VOLUMES_MESSAGE_MSG, req->reservation.preempt, cli_unique_id, NVMEIB_MCS_MSG_WITH_NO_ERROR, req->reservation.version);
			} else {
				eCPU_thread_prepare(t_async_mgmt_params, p);
				p->opcode = opcode;
				p->vol = vol;
				p->mcs = mcs;
				p->preempt = req->reservation.preempt;
				strlcpy(p->cli_unique_id, cli_unique_id, sizeof(p->cli_unique_id));
				eCPU_thread_launch(__async_generate_mcs_msg, p, false);
			}
			break;
		}
	}
	if (v == mdb->nVols) { // generate an mcs error volume unknown reply
		generate_mcs_error_message_from_payload(mcs, req, cli_unique_id, mgmt_2_clnt_err_strings[2]);
		return;
	}
}

// Required since we started using the lower level nvmeib_msgloop_sendl and must replace the pointer
static void __mcs_message_deserialize_pointers_tree_bfs(void *msg, const int opcode, size_t len) {
	char* buf;
	if (opcode == MCS_CLIENT_REGISTRATION_MESSAGE_MSG) {
		struct nvmeib_client_to_mgmt_init *m = msg;
		m->volumes = (struct nvmeibc_volume_status_payload*)&m[1];
		BUG_ON(len != (sizeof(*m) + sizeof(*m->volumes)*m->n_volumes));
	} else if (opcode == MCS_VOLUME_STATUS_MESSAGE_MSG) {
		struct nvmeib_client_to_mgmt_vol_info *m = msg;
		int i;
		m->attachments = (struct nvmeibc_volume_status_payload*)&m[1];
		buf = (char*)&m->attachments[m->n_volumes];
		for (i=0; i < m->n_volumes; i++) {
			const int n_ref_ids = m->attachments[i].n_ref_ids;
			m->attachments[i].referenceIDs = NULL;
			if (n_ref_ids) {
				m->attachments[i].referenceIDs = (struct nvmeibc_reference_id *)buf;
				buf += n_ref_ids * sizeof(*m->attachments->referenceIDs);
			}
		}
		BUG_ON((size_t)(buf-(char*)msg) != len );
	} else if (opcode == MCS_GET_CLIENT_CONFIGURATION_MSG_MSG) {
		struct get_client_configuration_msg *m = msg;
		m->volumes = (struct nvmeibc_volume_status_payload*)&m[1];
		buf = (char*)&m->volumes[m->n_volumes];
		for (int i=0; i < m->n_volumes; i++) {
			const int n_ref_ids = m->volumes[i].n_ref_ids;
			m->volumes[i].referenceIDs = NULL;
			if (n_ref_ids) {
				m->volumes[i].referenceIDs = (struct nvmeibc_reference_id *)buf;
				buf += n_ref_ids * sizeof(*m->volumes->referenceIDs);
			}
		}
		BUG_ON((size_t)(buf-(char*)msg) != len );
	} else if (opcode == MCS_GET_TARGETS_NICS_MSG) {
		struct nvmeib_client_to_mgmt_get_target_nics *m = msg;
		m->targets = (struct nvmeibc_target_nics_query*)&m[1];
		BUG_ON(len != (sizeof(*m) + sizeof(*m->targets)*m->n_targets));
	} else if (opcode == MCS_CLIENT_KEEP_ALIVE_MSG) {
		struct nvmeibc_client_keep_alive_message *m = msg;
		m->attachmentsUUIDHash = NULL;
		if (m->n_vols_id)
			m->attachmentsUUIDHash = (struct volume_version_id *)&m[1];
		BUG_ON(len != (sizeof(*m) + sizeof(*m->attachmentsUUIDHash)*m->n_vols_id));
	} else if (opcode == MCS_MANAGEMENT_LOG_MESSAGE_MSG) {
		struct nvmeib_mcs_log *m = msg;
		(void)m;						// Real mgmt stores this into its logs journal. Todo: simulate this
		BUG_ON(len != sizeof(*m));
	} else {
		BUG();		// Unsupported version
	}
}

static int __mgmt_reply_with_full_conf_on_clnts_request(struct mcs_simu *mgmt, const char *token);
#define enum_vol_status_is_detached(s)  ((s == NVMEIB_C_TO_M_VOLUME_ACK_DETACHED) || (s == NVMEIB_C_TO_M_VOLUME_ACK_ATTACH_FAILED) || (s == NVMEIB_C_TO_M_VOLUME_ACK_SHUTDOWN) || (s == NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_READY))
#define enum_vol_status_is_attached(s)  ((s == NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED))
#define enum_vol_status_is_no_change(s) ((s == NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_FAILED) || (s == NVMEIB_C_TO_M_VOLUME_ACK_BUSY) || (s == NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED) || (s == NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED_UNKNOWN_VOLUME) || (s == NVMEIB_C_TO_M_VOLUME_RESERVATION_DENIED))
static void increment_expected_sequence_num(struct mcs_simu *mcs)
{
	if (mcs->expected_sequence_num >= 0) {
		mcs->expected_sequence_num++;
	}
}

static bool mcs_is_message_seq_valid(struct mcs_simu *mcs, const long long val) {
	bool valid = (val == mcs->expected_sequence_num);
	return valid;
}

static bool mcs_is_client_token_valid(struct mcs_simu *mcs, const long long val) {
	bool valid = (val == mcs->expected_client_token);
	return valid;
}

static bool mcs_is_message_type_version_valid(struct mcs_simu *mcs, const u32 val) {
	return mcs->expected_messageTypeVersion == val;
}

void mcs_set_expected_counters(struct mcs_simu *mcs, long long expected_sequence_num,
	long long expected_reportID, long long expected_client_token, unsigned long expected_keepalive_interval)
{
	if (expected_sequence_num)
		mcs->expected_sequence_num 	= expected_sequence_num;
	if (expected_reportID)
		mcs->expected_reportID 		= expected_reportID;
	if (expected_client_token && (expected_client_token > mcs->expected_client_token))
		mcs->expected_client_token 	= expected_client_token;
	if (expected_keepalive_interval)
		mcs->expected_keepalive_interval = expected_keepalive_interval;
}

static void fill_actual_counters_from_kafka_hdr(const struct kafka_header_s *hdr, long long *actual_sequence_number, long long *actual_client_token,
	unsigned long *actual_keepalive_interval, u32 *actual_message_type_id) {
		*actual_sequence_number = hdr->messageSequence;
		*actual_client_token = hdr->clientToken;
		*actual_keepalive_interval = hdr->keepaliveInterval;
		*actual_message_type_id = hdr->messageTypeVersion;
}

// simulate mcs ack reply to client
static void send_ack_reply(struct mcs_simu *mcs, struct header *upstream_header)
{
	void *new_info = NULL;
	struct nvmeibc_mcs_downstream_ack *msg =
		(struct nvmeibc_mcs_downstream_ack *)sim_kzalloc(sizeof(*msg), GFP_KERNEL);
	BUG_ON(!msg);
	msg->messageTypeVersion = SUPPORTED_MCS_PROTOCOL_VERSION;
	new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
	BUG_ON(sizeof(msg->messageToken) != sizeof(upstream_header->token));
	memcpy(msg->messageToken, upstream_header->token, sizeof(msg->messageToken));
	mcs_gather(mcs, new_info, msg, MCS_CONFIRMED_DELIVERY_MSG,
		   NVMEIB_MCS_MSG_WITH_NO_ERROR);
	sim_kfree(new_info);
	sim_kfree(msg);
}

int mgmt_incoming_msg_from_clnt_cb(void* _mcs, const char *buf, size_t len) {
	struct mcs_simu *mcs = _mcs;
	struct mcs_message *_msg = (struct mcs_message *)((char *)buf + sizeof(int));
	void *msg = &_msg[1];		// Skip the message header and go the payload
	int rv = 0;
	const int opcode = nvmeib_mcs_get_opcode(msg);
	long long actual_sequence_number = -1;
	long long actual_client_token = -1;
	unsigned long actual_keepalive_interval = 0;
	u32 actual_message_type_id = 0;

	BUG_ON(msg != &_msg->msg);					// Just make sure there is no allignemnt hole between header and payload
	__mcs_message_deserialize_pointers_tree_bfs(msg, opcode, (&buf[len]-(const char*)msg));	//	Length without header

	_NI(trace_mcs_simu_mgmt_incoming_msg_from_clnt_cb_0, "message @INT was received", opcode);
	switch (opcode) {
		case MCS_CLIENT_KEEP_ALIVE_MSG: {
			const struct nvmeibc_client_keep_alive_message *inm = msg;
			fill_actual_counters_from_kafka_hdr(&inm->upstream_header,
				&actual_sequence_number, &actual_client_token, &actual_keepalive_interval, &actual_message_type_id);
			_NT(trace_mcs_simu_mgmt_incoming_msg_from_clnt_cb, "Client keep alive periodic message received");
			if (mcs->on_ka_cb) {
				mcs->on_ka_cb(mcs->ka_cb_ctx);
			}
			break;
		}
		case MCS_VOLUME_STATUS_MESSAGE_MSG: {
			const struct nvmeib_client_to_mgmt_vol_info *inm = (const struct nvmeib_client_to_mgmt_vol_info *)msg;
			struct nvmeibc_volume_status_payload *pl = inm->attachments;
			const enum_vol_status status = (enum_vol_status)pl->vol_status;
			const uint reportID_diff = (inm->reportID - mcs->expected_reportID);
			fill_actual_counters_from_kafka_hdr(&inm->upstream_header,
				&actual_sequence_number, &actual_client_token, &actual_keepalive_interval, &actual_message_type_id);
			if (       enum_vol_status_is_detached(status)) {
				mongo_db_simu_update_clnt_vol_attachment((void*)mcs_simu_get_mdb(mcs), mcs->inst_id, pl, false);
			} else if (enum_vol_status_is_attached(status)) {
				mongo_db_simu_update_clnt_vol_attachment((void*)mcs_simu_get_mdb(mcs), mcs->inst_id, pl, true );
			} else if (enum_vol_status_is_no_change(status)) {
				mongo_db_simu_update_clnt_vol_attachment((void*)mcs_simu_get_mdb(mcs), mcs->inst_id, pl, true );
			} else {
				BUG(); // Unsupported volume status
			}
			BUG_ON(reportID_diff > 1);					// Diff == {0,1}
			mcs->expected_reportID = inm->reportID;
			send_ack_reply(mcs, &_msg->header);
			break;
		}
		case MCS_MANAGEMENT_LOG_MESSAGE_MSG: {
			const struct nvmeib_mcs_log *inm = msg;
			fill_actual_counters_from_kafka_hdr(&inm->upstream_header,
				&actual_sequence_number, &actual_client_token, &actual_keepalive_interval, &actual_message_type_id);
			if (mcs->s.expector_set)
				mgmt_alerts_log_simu_verify_msg(&mcs->s, msg);
			break;
		}
		case MCS_GET_CLIENT_CONFIGURATION_MSG_MSG: { // Keep old logic correct by passing cli_unique_id, as client set's MAGIC_CONFIG_UPDATE_TOKEN when required
			const struct get_client_configuration_msg *inm = msg;
			fill_actual_counters_from_kafka_hdr(&inm->upstream_header,
				&actual_sequence_number, &actual_client_token, &actual_keepalive_interval, &actual_message_type_id);
			if (inm->n_volumes)	// Specific volume configuration requested
				__mgmt_reply_with_vol_conf_on_clnts_request( mcs, inm->volumes, inm->cli_unique_id);
			else 				// Full volume configuration requested
				__mgmt_reply_with_full_conf_on_clnts_request(mcs,               inm->cli_unique_id);
			break;
		}
		case MCS_GET_TARGETS_NICS_MSG: {
			const struct nvmeib_client_to_mgmt_get_target_nics *inm = msg;
			fill_actual_counters_from_kafka_hdr(&inm->upstream_header,
				&actual_sequence_number, &actual_client_token, &actual_keepalive_interval, &actual_message_type_id);
			for (int idx=0; idx < inm->n_targets; ++idx){
				__auto_type query = &inm->targets[idx];
				_NT(trace_mcs_simu_mgmt_incoming_msg_from_clnt_cb1_get_targets_nics, "query; target=@NODE_ID_STR, nicsVersion=@INT", query->node_id, query->nicsVersion);
			}
			send_ack_reply(mcs, &_msg->header);
			break;
		}
		default: BUG();
	}

	//this is just an incorrect. we may decide to modify keepalive interval and even set it.
	//BUT, we need to wait until this interval will be propogated back to the mcs.
	//Only after the propogation, we may assert on the correct value
	//
	//BUG_ON(actual_keepalive_interval != mcs->expected_keepalive_interval);
	_NI(trace_mcs_simu_mgmt_incoming_msg_from_clnt_cb_type_version, "message type version @INT was received, expected @INT", actual_message_type_id, mcs->expected_messageTypeVersion);
	BUG_ON(!mcs_is_message_type_version_valid(mcs, actual_message_type_id));
	BUG_ON(!mcs_is_message_seq_valid(mcs, actual_sequence_number));
	increment_expected_sequence_num(mcs);
	BUG_ON(!mcs_is_client_token_valid(mcs, actual_client_token));
	return rv;
}

/******************************* mcs.c Simulator ******************************/
#include "nvmeib_mcs.h"
#include "nvmeibc_cc_api.h"
#include "nvmeibc_simu_disk.h"

static int fill_targets_from_discriptor(const struct mongo_db_simu *mdb, struct nvmeibc_target_conf *targets, u64 uniqueNodes[], int n_targets, const enum NVMEIB_MCS_MSG_ERROR_TYPES err) {
	int i, rv;
	struct nvmeibc_nic_conf   *dst_nic;
	struct nvmeibc_disk_conf   *dst_disk;
	struct nvmeibc_target_conf *dst_targ;
	BUG_ON(n_targets > NVMESH_N_PHYS_DISKS);
	for (i = 0; i < n_targets; i++) {
		int node_i = uniqueNodes[i];
		BUG_ON(node_i<0);
		strcpy(targets[i].node_id, mdb->srvrs[node_i].node_id);
	}

	// One nic per target, optionally skip the first nic
	for (i = (err == NVMEIB_MCS_MSG_DROP_NIC); i < n_targets; i++) {
		dst_targ	  = &targets[i];
		dst_targ->nics= sim_kzalloc(sizeof(struct nvmeibc_nic_conf), GFP_KERNEL);   BUG_ON(!targets[i].nics);
		dst_nic 	  = &dst_targ->nics[dst_targ->n_nics++];
		dst_nic->protocol = PROTOCOL_INFINIBAND;	/* Constructor of nic */
		dst_nic->pkey     = 23456;
		strcpy(dst_nic->nicID, mdb->srvrs[uniqueNodes[i]].rnic_guid);
	}

	// One disk per target, optionally skip the first disk
	for (i = (err == NVMEIB_MCS_MSG_DROP_DISK); i < n_targets; i++) {
		const struct mdb_target_conf* src = &mdb->srvrs[uniqueNodes[i]];
		dst_targ	    = &targets[i];
		dst_targ->disks = sim_kzalloc(sizeof(struct nvmeibc_disk_conf) , GFP_KERNEL); BUG_ON(!targets[i].disks);
		dst_disk	    = &dst_targ->disks[dst_targ->n_disks++];
		strcpy(dst_disk->uuid  , src->disk_name); 	/* Constructor of disk */
		strcpy(dst_disk->diskID, src->disk_name);
		dst_disk->blocks       = ((u64)-1);// src->ramDisk.disk_size / NVMEIBC_SECTOR_SIZE;	// Used by Real Toma Only, not client
	}
	rv = n_targets;
	return rv;
}

static void disk_range_2mcs_range(const struct mongo_db_simu *mdb, struct nvmeibc_segment_conf *dst, struct disk_range *src, const char*status, unsigned int pRaidIndex, unsigned int pRaidTypeIndex) {
	const struct mdb_target_conf* srvr = &mdb->srvrs[src->node_id];
	dst->lbs = src->dlba_start;
	dst->lbe = src->length + src->dlba_start - 1;
	strcpy(dst->uuid, src->ruuid);
	strcpy(dst->diskUUID, srvr->disk_name);
	strcpy(dst->diskID  , srvr->disk_name);
	strcpy(dst->nodeUUID, "0");
	strcpy(dst->node_id, srvr->node_id);
	dst->type = TYPE_DATA;
	strcpy(dst->status, status);		// The values as defined by Toma in: _mm_seg_from_json() and filtered by MCS: CMSocket.py
	dst->allocationIndex = src->stripe_index;
	dst->pRaidIndex = pRaidIndex;
	dst->pRaidTypeIndex = pRaidTypeIndex;
}

static inline u64 __find_next_lba_of_empty_chunk(const struct volumeDescriptor *vol, const u64 lba) {
	struct disk_range *s, *end;
	u64 rv = ~0ULL;
	for (s = vol->segs      , end = s + vol->nSegments     ; s < end; s++) {
		if ((s->bd_start > lba) && (s->bd_start < rv))
			rv = s->bd_start;
	}
	for (s = vol->depre_segs, end = s + vol->nDepreSegments; s < end; s++) {
		if ((s->bd_start > lba) && (s->bd_start < rv))
			rv = s->bd_start;
	}
	return (rv == ~0ULL) ? lba : rv;			// If lba does not exist, return current lba. Doron L: Why? what is this algorithm?
}

// This function takes a volumeDescriptor and creates the expected MCS message to comply with the configuration (allows errors in the descriptor)
static int fill_volume_from_discriptor(struct mgmt_simu *mgmt, struct nvmeibc_volume_conf *dst, const struct volumeDescriptor *vol, u64 uniqueDisks[])
{
	const int v = (vol - mgmt->mdb->vols);					// Todo: This should be the actual param to the function instead of 'vol'
	int total_segs = vol->nSegments + vol->nDepreSegments, num_disks = 0, c, i, s_ind;
	int calculated_chunks = 0;
	int segs_per_chunk = (vol->nChunks) ? vol->nSegments/vol->nChunks : 0;
	u32 r;
	u64 next_bd_start = 0;
	if (!segs_per_chunk) {
		_ND(t01msfvfd, "Creating Invalid config");
	}
	if (total_segs != vol->nSegments) { // depricated segs should not be in the message (as is now)
		_ND(t02msfvfd, "Cannot use DepreSegments with MCS");
	}
	if (vol->nChunks && vol->nSegments%vol->nChunks) {
		_ND(t03msfvfd, "Either missing a segment or have an additional one");
	} // Similar to the way configuration is parsed in the attach process
	for (i=0;i<vol->nSegments;i++) {
		if (vol->segs[i].stripe_index == 0) {
			calculated_chunks++;
		}
	}
	if (calculated_chunks != vol->nChunks) {
		_ND(t04msfvfd, "Creating Invalid config");
	}
	if (vol->nChunks)
		dst->chunks = sim_kzalloc(sizeof(*dst->chunks) * vol->nChunks, GFP_KERNEL);
	else
		dst->chunks = NULL;
	dst->n_chunks = vol->nChunks;
	BUG_ON(vol->nChunks && !dst->chunks);
	for (c=0;c<vol->nChunks;c++) {
		struct nvmeibc_chunk_conf *cur_chunk = &dst->chunks[c];
		const struct disk_range *first_seg_in_chunk = NULL;
		// Allows using an empty praid or a praid with a different amount of segs
		if (vol->segs && vol->segs->stripe_width)
			cur_chunk->praids = sim_kzalloc(sizeof(*cur_chunk->praids) * vol->segs->stripe_width /* seg.stripe_width */, GFP_KERNEL);
		cur_chunk->n_praids = vol->segs->stripe_width;
		for (r=0;r<vol->segs->stripe_width;r++) {
			struct nvmeibc_praid_conf *cur_pr = &cur_chunk->praids[r];
			int segs_in_raid = 0, dep_segs_in_raid = 0;
			struct nvmeibc_locks_scheme_conf *lock_scheme = &cur_pr->lockServer;
			lock_scheme->type 		  = vol->locks_scheme.type;
			lock_scheme->maxNOwners   = vol->locks_scheme.maxNOwners;
			lock_scheme->locksetShift = vol->locks_scheme.locksetShift;
			for (i=0;i<vol->nSegments;i++) { // count active segs
				if ((vol->segs[i].bd_start == next_bd_start) && ((vol->segs[i].stripe_index / vol->segs[i].replicas == r) || (vol->segs[i].stripe_index / vol->segs[i].replicas >= vol->segs->stripe_width))) {
					segs_in_raid++;
					if (!first_seg_in_chunk)
						first_seg_in_chunk = &vol->segs[i];
				}
			}
			for (i=0;i<vol->nDepreSegments;i++) { // count depricated segs
				if (vol->depre_segs[i].bd_start == next_bd_start) {
					dep_segs_in_raid++;
					if (!first_seg_in_chunk)
						first_seg_in_chunk = &vol->depre_segs[i];
				}
			}
			if (segs_in_raid + dep_segs_in_raid)
				cur_pr->segments = sim_kzalloc(sizeof(*cur_pr->segments) * (segs_in_raid + dep_segs_in_raid), GFP_KERNEL);
			cur_pr->n_segments = segs_in_raid + dep_segs_in_raid;
			cur_pr->stripeIndex = r;
			for (i = 0, s_ind = 0; i<vol->nSegments;i++) { // Add normal segments first
				if ((vol->segs[i].bd_start == next_bd_start) && ((vol->segs[i].stripe_index / vol->segs[i].replicas == r) || (vol->segs[i].stripe_index / vol->segs[i].replicas >= vol->segs[i].stripe_width))) {
					// TODO add an index for the raftonly segments
					unsigned int pRaidTypeIndex = vol->segs[i].stripe_index - vol->segs[i].replicas * r;
					if (s_ind == 0)
						disk_range_format_praid_uuid(&vol->segs[i], cur_pr->uuid);
					disk_range_2mcs_range(mgmt->mdb, &cur_pr->segments[s_ind++], &vol->segs[i], "norm", r, pRaidTypeIndex);
					if (mgmt->mdb->discs[v][vol->segs[i].node_id].n_ranges != 0) {
						for (int j = 0; j < NVMESH_N_PHYS_DISKS; j++) {
							if (uniqueDisks[j] == vol->segs[i].node_id)
								break;
							if (uniqueDisks[j] == (u64)-1) {
								uniqueDisks[j] = vol->segs[i].node_id;
								num_disks++;
								break;
							}
						}
					}
					if (!cur_pr->dataBlocks) {
						cur_pr->dataBlocks = vol->segs[i].slice_size;
						if (cur_pr->dataBlocks > 1) {
							cur_pr->parityBlocks = vol->segs[i].replicas - vol->segs[i].slice_size;
							cur_pr->numberOfMirrors = 0;
						} else {
							cur_pr->numberOfMirrors = vol->segs[i].replicas - vol->segs[i].slice_size;
							cur_pr->parityBlocks = 0;
						}
					}
					BUG_ON((u32)cur_pr->dataBlocks != vol->segs[i].slice_size);
					segs_in_raid--;
				}
			}
			for (i = 0; i<vol->nDepreSegments;i++) { // Add deprecated segments
				unsigned int pRaidTypeIndex = vol->depre_segs[i].stripe_index - vol->depre_segs[i].replicas * r;
				if ((vol->depre_segs[i].bd_start != next_bd_start) /*&& vol->depre_segs[i].stripe_index / vol->segs[i].replicas != r*/) {
					continue;
				} // TODO add an index for the raftonly segments
				disk_range_2mcs_range(mgmt->mdb, &cur_pr->segments[s_ind++], &vol->depre_segs[i], "depr", r, pRaidTypeIndex);
				if (mgmt->mdb->discs[v][vol->depre_segs[i].node_id].n_ranges != 0) {
					for (int j = 0; j < NVMESH_N_PHYS_DISKS; j++) {
						if (uniqueDisks[j] == vol->depre_segs[i].node_id)
							break;
						if (uniqueDisks[j] == (u64)-1) {
							uniqueDisks[j] = vol->depre_segs[i].node_id;
							num_disks++;
							break;
						}
					}
				}
				if (!cur_pr->dataBlocks) {
					cur_pr->dataBlocks = vol->depre_segs[i].slice_size;
					if (cur_pr->dataBlocks > 1) {
						cur_pr->parityBlocks = vol->depre_segs[i].replicas - vol->depre_segs[i].slice_size;
						cur_pr->numberOfMirrors = 0;
					} else {
						cur_pr->numberOfMirrors = vol->depre_segs[i].replicas - vol->depre_segs[i].slice_size;
						cur_pr->parityBlocks = 0;
					}
				}
				BUG_ON((u32)cur_pr->dataBlocks != vol->segs[i].slice_size);
				dep_segs_in_raid--;
			}
			BUG_ON(dep_segs_in_raid || segs_in_raid);
		}
		if (first_seg_in_chunk == NULL) { // Handle empty chunk
			cur_chunk->vlbs = next_bd_start;
			cur_chunk->vlbe = __find_next_lba_of_empty_chunk(vol, next_bd_start) - 1;
		} else { // Fill chunk info from the first segment on the chunk
			const struct disk_range *seg = first_seg_in_chunk;
			cur_chunk->vlbs = seg->bd_start;
			cur_chunk->vlbe = seg->bd_start + (seg->length * seg->slice_size) * seg->stripe_width - 1;
			cur_chunk->stripeSize = seg->stripe_size;
			cur_chunk->stripeWidth = seg->stripe_width;
			strcpy(cur_chunk->uuid, "");
		}
		next_bd_start = cur_chunk->vlbe + 1;
	}
	BUG_ON(num_disks > vol->nSegments+vol->nDepreSegments);
	dst->sliceWidth = vol->snake_size;
	// Init volume header
	if (total_segs) {
		if (vol->segs[0].slice_size > 1) {
			dst->numberOfMirrors = 0;
			dst->parityBlocks = (int)vol->segs[0].replicas - (int)vol->segs[0].slice_size;
			dst->dataBlocks = (int)vol->segs[0].slice_size;
		} else {	// Init multi-slice and snake (set in volume descriptor)
			dst->numberOfMirrors = (int)vol->segs[0].replicas - (int)vol->segs[0].slice_size;
			dst->dataBlocks = (int)vol->segs[0].slice_size;
			dst->parityBlocks = 0;
			BUG_ON(dst->sliceWidth != 1);
		}
		dst->stripeWidth = (unsigned short)vol->segs[0].stripe_width;
		dst->stripeSize  = (int)vol->segs[0].stripe_size;
	}

	// Init reservation info
	dst->reservation.mode = vol->vat.res.mode;
	dst->reservation.version = vol->vat.res.version;
	dst->reservation.is_512B_IO_allowed = vol->vat.res.is_512B_IO_allowed;
	strcpy(dst->reservation.reservedBy, "??? todo");
	dst->enableCrcCheck = vol->enable_crc_check;
	dst->enableLocalReadOptimization = vol->enable_local_read_optimization;
	dst->use_debug_di = vol->use_debug_di;

	dst->type            = (int)vol->info.type;
	strcpy(dst->name, vol->info.devname);
	dst->version         = vol->info.version;
	strcpy(dst->uuid, vol->info.uuid);
	dst->n_chunks        = vol->nChunks;
	if (dst->n_chunks) { // Some updates have no chunks
		dst->blocks          = dst->chunks[dst->n_chunks - 1].vlbe + 1;
	}
	if (dst->numberOfMirrors > 0)   dst->RAIDLevel = ((dst->stripeWidth > 1) ? RAIDLEVEL_STRIPED_MIRRORED_RAID_10 : RAIDLEVEL_MIRRORED_RAID_1);
	else							dst->RAIDLevel = ((dst->stripeWidth > 1) ? RAIDLEVEL_STRIPED_RAID_0           : RAIDLEVEL_CONCATENATED);
	if (dst->dataBlocks > 2) 		dst->RAIDLevel = RAIDLEVEL_STRIPED_RAID_6;
	if (total_segs == 0) { // MTV
		dst->RAIDLevel = RAIDLEVEL_ELECT;
	}

	// Add latest reference id's (no copy, just const reference)
	BUG_ON(vol->attachmentVersion <= 0);	// This field is copied to attachment message header
	dst->attachment.version = vol->attachmentVersion + 10;
	dst->attachment.n_ref_ids = vol->n_ref_ids;
	dst->attachment.referenceIDs = (void*)&vol->referenceIDs[0];
	return num_disks;
}

static void free_configuraion(struct nvmeib_mgmt_to_client_volume_configuration *msg)
{
	if (!msg)
		return;
	for (int i = 0; i < msg->n_volumes; ++i) {
		for (int j = 0; j < msg->volumes[i].n_chunks; ++j) {
			for (int k = 0; k < msg->volumes[i].chunks[j].n_praids; k++) {
				if (msg->volumes[i].chunks[j].praids[k].n_segments) {
					sim_kfree(msg->volumes[i].chunks[j].praids[k].segments);
				}
			}
			if (msg->volumes[i].chunks[j].n_praids) {
				sim_kfree(msg->volumes[i].chunks[j].praids);
			}
		}
		if (msg->volumes[i].n_chunks)
			sim_kfree(msg->volumes[i].chunks);
		//sim_kfree(msg->volumes[i].referenceIDs);
	}
	if (msg->n_volumes)
		sim_kfree(msg->volumes);
	for (int i = 0; i < msg->n_targets; ++i) {
		if (msg->targets[i].n_nics)
			sim_kfree(msg->targets[i].nics);
		if (msg->targets[i].n_disks)
			sim_kfree(msg->targets[i].disks);
	}
	if (msg->n_targets)
		sim_kfree(msg->targets);
	sim_kfree(msg);
}

static int nvmeib_fill_attach_message_from_volume_descriptor(struct mgmt_simu *mgmt, struct nvmeib_mgmt_to_client_volume_configuration *attach_msg, const struct volumeDescriptor *vol, const int vol_index, const enum NVMEIB_MCS_MSG_ERROR_TYPES err)
{
	int rv = 0, num_disks = 0;
	u64 uniqueDisks[NVMESH_N_PHYS_DISKS];// = {[ 0 ... (NVMESH_N_PHYS_DISKS - 1) ] = ~0ULL};
	array_fill(uniqueDisks, ~0ULL);
	attach_msg->volumes = sim_kzalloc(sizeof(*attach_msg->volumes), GFP_KERNEL);
	BUG_ON(!attach_msg->volumes);
	_fill_message_type_version(&attach_msg->messageTypeVersion, err);
	// Each seg will add it's target disk to uniqueDisks thus allowing adding only the relevant disks and nics
	num_disks += fill_volume_from_discriptor(mgmt, attach_msg->volumes, (vol->nVols == 1) ? vol : vol->links[vol_index], uniqueDisks);

	if (err == NVMEIB_MCS_MSG_DROP_TARGET) {
		if (num_disks > 1) {
			num_disks--;
		}
	}
	if (num_disks)
		attach_msg->targets = sim_kzalloc(sizeof(*attach_msg->targets) * num_disks , GFP_KERNEL);
	else
		attach_msg->targets = NULL;
	BUG_ON(num_disks && !attach_msg->targets);
	attach_msg->n_targets = num_disks;
	rv = fill_targets_from_discriptor(mgmt->mdb, attach_msg->targets, uniqueDisks, num_disks, err);
	BUG_ON(rv != num_disks);
	rv = 0;
	attach_msg->n_volumes = 1;	// We send each volume separetly
	attach_msg->attachmentsVersion = vol->attachmentVersion;
	return rv;
}

static int __calculate_attachmet_version_hack(const struct mongo_db_simu *_mdb) {
	struct mongo_db_simu *mdb = (struct mongo_db_simu *)_mdb;	// Remove const, becasue this function is a hack
	// There should be a proper logic of when attachment version is increased (much like in real mgmt)
	// I (Daniel) dont know this logic, so this function is a temporal hack which always takes the max. Why?
	// Because attachmentVersion should be per volume but it is given on full conf message, 1 per all volumes.
	// I dont know how real mgmt treats this. So the hack is to increase all attachment versions of all volumes to
	// some maximum. This is legit as attachment version can grow anyways due to mgmt decision.
	int v, rv = 0;
	for (v=0; v<mdb->nVols; v++) {
		MAX_WITH(rv, mdb->vols[v].attachmentVersion);
	}
	for (v=0; v<mdb->nVols; v++) {
		mdb->vols[v].attachmentVersion = rv;		// Simulate that attachment version of each volume possibly increases
	}
	return rv;
}

static int __mgmt_reply_with_full_conf_on_clnts_request(struct mcs_simu *mcs, const char *token) {
	// TODO: Create a message and update multicompletion accordingly (by the expected amount of volumes)
	struct mgmt_simu *mgmt = mcs_simu_get_mgmt(mcs);
	const struct mongo_db_simu *mdb = mcs_simu_get_mdb(mcs);
	int rv = 0, num_disks = 0, v;
	u64 uniqueDisks[NVMESH_N_PHYS_DISKS];// = {[ 0 ... (NVMESH_N_PHYS_DISKS - 1) ] = ~0ULL};
	void *new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
	struct nvmeib_mgmt_to_client_volume_configuration *msg = (struct nvmeib_mgmt_to_client_volume_configuration *)sim_kzalloc(sizeof(*msg), GFP_KERNEL);
	msg->volumes = sim_kzalloc(sizeof(*msg->volumes)*mdb->nVols, GFP_KERNEL);
	array_fill(uniqueDisks, ~0ULL);
	BUG_ON(!msg->volumes);
	msg->n_volumes = mdb->nVols; // Here we assume that all volumes should be attached.
	msg->attachmentsVersion = __calculate_attachmet_version_hack(mdb);
	for (v=0; v<mdb->nVols; v++) {
		const struct volumeDescriptor *vol = &mdb->vols[v];
		// Each seg will add it's target disk to uniqueDisks thus allowing adding only the relevant disks and nics
		num_disks += fill_volume_from_discriptor(mgmt, &msg->volumes[v], vol, uniqueDisks);
	}
	if (num_disks)
		msg->targets = sim_kzalloc(sizeof(*msg->targets) * num_disks , GFP_KERNEL);
	else
		msg->targets = NULL;
	BUG_ON(num_disks && !msg->targets);
	msg->n_targets = num_disks;
	rv = fill_targets_from_discriptor(mdb, msg->targets, uniqueDisks, num_disks, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	BUG_ON(rv != num_disks);
	msg->updateType = UPDATETYPE_FULL;
	strcpy(msg->cli_unique_id, token);
	_fill_message_type_version(&msg->messageTypeVersion, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	rv = mcs_gather(mcs, new_info, msg, MCS_ATTACH_VOLUMES_MESSAGE_MSG, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	sim_kfree(new_info);
	free_configuraion(msg);
	return rv;
}

//helper to build the message
struct build_info {
	/*the size calculated by accumulating the incoming messages*/
	unsigned total_len;
	/*the size of message calculated by the mcs level*/
	unsigned declared_size;
	char *buf;
	char *itr;
};

static int send_tool(void *context, void *buf,  int len) {
	struct build_info *info = context;
	if (len == 0)
		goto out;

	if (!info->buf) {
		BUG_ON(len != sizeof(int));
		info->declared_size = *(int *)buf;
		info->buf = (char *)sim_kzalloc(info->declared_size + len, GFP_KERNEL);
		BUG_ON(!info->buf);
		info->total_len = info->declared_size + len;
		info->itr = info->buf;
	}

	BUG_ON(!info->itr);
	BUG_ON(info->itr < info->buf);
	BUG_ON((info->itr + len) > (info->buf + info->total_len));
	memcpy(info->itr, buf, len);
	info->itr += len;
out:
	return len;
}

int mcs_send_raw_msg_buf_unsafe(const struct c_api_proc *mcs, char *msg, unsigned len) {
	int rv = mcs->dir->fops->write((void*)mcs->msg_loop,msg, len, NULL);
	if ((unsigned)rv != len) {
		rv = -EIO;
		_NE_dmesg(error_mcs_simu_mcs_send_raw_msg_buf_unsafe, "send mcs msg to client - failed");
	}
	return rv;
}

static int mcs_gather(struct mcs_simu *mcs, struct mcs_info *info, void *msg, int opcode,  const enum NVMEIB_MCS_MSG_ERROR_TYPES error) {
	struct build_info b_info = {0,0,(char *)NULL, (char *)NULL};
	struct mcs_message* mcsmsg;
    int rv = 0;
	//static DEFINE_SPINLOCK(func_lock);		// Send only one message at a time???
    NFIN;
	//spin_lock(&func_lock);
	if ((opcode > info->mcs_max_msg_id)||(opcode < 0)) {
		WARN(true, "Incorrect opcode=%d\n", opcode);
		goto _out;
	}
	_NT(trace_mcs_simu_mcs_gather, "Sending to MCS opcode @OPCODE", opcode);

	if (msg) {
		rv = nvmeib_mcs_build_msg(info, opcode, msg, NULL, send_tool, &b_info);
		if (rv < 0)
			goto _out;

		mcsmsg = ((struct mcs_message*)(((int*)b_info.buf)+1));				// Skip sizeof(int)
		switch (error) {
			case NVMEIB_MCS_MSG_WITH_BAD_HEADER:
				mcsmsg->header.header_version++;
				break;
			case NVMEIB_MCS_MSG_WITH_BAD_SCHEME:
				mcsmsg->header.scheme_version++;
				break;
			case NVMEIB_MCS_MSG_WITH_BUFFER_OVERFLOW:
				mcsmsg->header.msg_len.msg_len--;
				break;
			case NVMEIB_MCS_MSG_WITH_BAD_OPCODE:
				mcsmsg->header.opcode = INT_MAX;
				break;
			default:;
		}
		if (mcs_message_queue_try_insert(&mcs->mq, b_info.buf, b_info.total_len))
			goto _out; 		// No need to send it, it was enqueued
		rv = mcs_send_raw_msg_buf_unsafe(mcs->mq.mcs, b_info.buf, b_info.total_len);
	}
_out:
	sim_kfree(b_info.buf);
	//spin_unlock(&func_lock);
	//if (!rv) _T("Msg rejected by client\n");
	NFOUT;
	return rv;
}

// This function is called for each MCS message generation, currently ATTACH/UPDATE volume and allows injection of error into the scheme header
int generate_mcs_attach_message_for_volume(struct mcs_simu *mcs, const struct volumeDescriptor *vol, int opcode, int preempt, const char *token, const enum NVMEIB_MCS_MSG_ERROR_TYPES error) {
	int rv = 0, i;
	for (i = 0; i < vol->nVols; i++) {	// For each related volume send one configuration at a time (in required order)
		struct nvmeib_mgmt_to_client_volume_configuration *msg = (struct nvmeib_mgmt_to_client_volume_configuration *)sim_kzalloc(sizeof(*msg), GFP_KERNEL);
		strlcpy(msg->cli_unique_id, token, sizeof(msg->cli_unique_id));
		_fill_message_type_version(&msg->messageTypeVersion, error);
		if ((rv = nvmeib_fill_attach_message_from_volume_descriptor(mcs_simu_get_mgmt(mcs), msg, vol, i, error)) < 0)
			_NE_dmesg(error_mcs_simu_generate_mcs_attach_message_for_volume, "Fill attach message failed");
		else {
			void *new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
			msg->volumes[0].reservation.preempt = preempt;
			rv = mcs_gather(mcs, new_info, msg, opcode, error);
			sim_kfree(new_info);
		}
		free_configuraion(msg);
	}
	return rv;
}

int generate_mcs_attach_message_for_volume_with_res_version(
	struct mcs_simu *mcs, const struct volumeDescriptor *vol, int opcode, int preempt,
	const char *token, const enum NVMEIB_MCS_MSG_ERROR_TYPES error, unsigned long long version) {
	int rv = 0, i;
	for (i = 0; i < vol->nVols; i++) {	// For each related volume send one configuration at a time (in required order)
		struct nvmeib_mgmt_to_client_volume_configuration *msg = (struct nvmeib_mgmt_to_client_volume_configuration *)sim_kzalloc(sizeof(*msg), GFP_KERNEL);
		strlcpy(msg->cli_unique_id, token, sizeof(msg->cli_unique_id));
		_fill_message_type_version(&msg->messageTypeVersion, error);
		if ((rv = nvmeib_fill_attach_message_from_volume_descriptor(mcs_simu_get_mgmt(mcs), msg, vol, i, error)) < 0)
			_NE_dmesg(error_mcs_simu_generate_mcs_attach_message_for_volume_with_res_version, "Fill attach message failed");
		else {
			void *new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
			msg->volumes[0].reservation.preempt = preempt;
			msg->volumes[0].reservation.version = max(vol->vat.res.version, version);
			rv = mcs_gather(mcs, new_info, msg, opcode, error);
			sim_kfree(new_info);
		}
		free_configuraion(msg);
	}
	return rv;
}

// Called to set client counters
int generate_mcs_update_token(struct mcs_simu *mcs,
	long long messageSequence, long long reportID, long long clientToken,
	unsigned int keepalive_interval, const enum NVMEIB_MCS_MSG_ERROR_TYPES error) {
	int rv = 0;
	void *new_info = NULL;
	struct update_client_token_message *msg = (struct update_client_token_message *)sim_kzalloc(sizeof(*msg), GFP_KERNEL);
	BUG_ON(!msg);
	msg->messageSequence = messageSequence;
	msg->reportID = reportID;
	msg->clientToken = clientToken;
	msg->keepaliveInterval = keepalive_interval;
	_fill_message_type_version(&msg->messageTypeVersion, error);

	new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
	BUG_ON(!new_info);
	rv = mcs_gather(mcs, new_info, msg, MCS_UPDATE_CLIENT_TOKEN_MSG, error);
	sim_kfree(new_info);
	sim_kfree(msg);

	return rv;
}

int generate_mcs_fake_update_targets_nics(struct mcs_simu *mcs) {
	int rv = 0;
	void *new_info = NULL;

	struct nvmeib_mgmt_to_client_update_targets_nics *msg = (struct nvmeib_mgmt_to_client_update_targets_nics*)sim_kzalloc(sizeof(*msg) + sizeof(msg->targets[0]), GFP_KERNEL);
	BUG_ON(!msg);

	msg->n_targets=1;
	msg->targets = (struct nvmeibc_target_conf*)(&(msg[1]));

	new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
	BUG_ON(!new_info);
	rv = mcs_gather(mcs, new_info, msg, MCS_UPDATE_TARGETS_NICS_MSG, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	sim_kfree(new_info);
	sim_kfree(msg);

	return rv;
}

//Generate a message with an invalid mcs opcode to be sent to the client
int generate_mcs_invalid_operation(struct mcs_simu *mcs) {
	int rv = 0;
	void *new_info = NULL;
	struct invalid_mcs_opcode_message *msg = (struct invalid_mcs_opcode_message *)sim_kzalloc(sizeof(*msg), GFP_KERNEL);
	BUG_ON(!msg);
	_fill_message_type_version(&msg->messageTypeVersion, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
	BUG_ON(!new_info);
	rv = mcs_gather(mcs, new_info, msg, MCS_INVALID_MCS_OPCODE_MSG, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	sim_kfree(new_info);
	sim_kfree(msg);

	return rv;
}

static int nvmeib_fill_delete_message_from_volume_descriptor(struct delete_message *event_msg, const struct volumeDescriptor *vol)
{
	strcpy(event_msg->health, "healthy");
	strcpy(event_msg->volumeID, vol->info.devname);
	strcpy(event_msg->uuid, vol->info.uuid);

	//if (unlikely(rv)) _E("Fill delete message failed\n"); // Cannot fail currently
	return 0;
}

int generate_mcs_delete_message_for_volume(struct mcs_simu *mcs, const struct volumeDescriptor *vol, const int opcode)
{
	int rv;
	void *new_info = replace_upstream_with_downstream(mcs->mq.mcs->handle);
	struct delete_message *msg = sim_kzalloc(sizeof(*msg), GFP_KERNEL);
	_fill_message_type_version(&msg->messageTypeVersion, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	rv = nvmeib_fill_delete_message_from_volume_descriptor(msg, vol);
	rv = mcs_gather(mcs, new_info, msg, opcode, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	sim_kfree(new_info);
	sim_kfree(msg);
	return rv;
}
/*****************************************************************************/
// EOF.
