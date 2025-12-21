#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "clnt/nvmeibt_client.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_global.h"
#include "nvmeibt_register.h"
#include "nvmeibt_global.h"

/**********************                       ***************************/
static char null_str = '\0';

const char *nvmeibt_client_get_urn_uuid_str(struct nvmeibt_client *client)
{
	return (client ? client->client_provided_urn_uuid.str : &null_str);
}

const char *nvmeibt_client_get_hostname(struct nvmeibt_client *client)
{
	return (client ? client->net.host_name : &null_str);
}

#define NVMEIBT_CLIENT_DUMP(__name__, cl) \
	N_Tf(__name__, "uuid=@UUID_LE hostname=@HOSTNAME disk=@STR cid=@CID", &(cl)->client_provided_uuid, (cl)->net.host_name, (cl)->ldisk_id.str, (cl)->cid)

static struct nvmeibt_client *nvmeibt_client_find_by_cid(u32 cid)
{
	struct nvmeibt_client	*client;

	XHASHTABLE_FOR_EACH_POSSIBLE(client, &nvmeibt_global_get_global()->clients_hash, cid) {
		if (client->cid == cid) {
			goto out;
		}
	}

	client = NULL;

out:
	if (client == NULL) {
		N_Tf(nvmeibt_client_find_by_cid_1, "client not found cid=@CID", cid);
	} else {
		N_Tf(nvmeibt_client_find_by_cid_2, "client found cid=@CID client=@PPP", client->cid, client);
		NVMEIBT_CLIENT_DUMP(nvmeibt_client_find_by_cid_3, client);
	}

	return client;
}

static struct nvmeibt_client *nvmeibt_client_add(struct nvmeibs_msg_s2t_subscriber_change *msg)
{
	struct nvmeibt_client	*client;
	struct nvmeibt_topology	*cur_topo = nvmeibt_global_get_global();

	NFIN;

	// Find or create a client object.
	client = nvmeibt_client_find_by_cid(msg->cid);
	if (!client) {
		if (XHASHTABLE_N_ELEMENTS(&cur_topo->clients_hash) >= NVMEIBT_MAX_N_CLIENTS_PER_NODE) {
			N_Wf(nvmeibt_client_add_1, "Reached MAX_N_CLIENTS_PER_NODE=@MAX_N_CLIENTS_PER_NODE", NVMEIBT_MAX_N_CLIENTS_PER_NODE);
		}
		client = NNVMEIBT_TOMA_CALLOC(nvmeibt_client_add_2, 1, sizeof *client);
		XDLIST_INIT_LINK(&client->topo_link, NULL);
		XHASHTABLE_ADD(&cur_topo->clients_hash, client, msg->cid);
		client->client_provided_uuid = msg->client_uuid;
		client->cid = msg->cid;
		client->n_reg_ctx_refs = 0;
		BUILD_BUG_ON(sizeof(client->net.host_name) < sizeof(msg->host_name));				// Will cause cut-off in host name
		nvmeibt_strlcpy(client->net.host_name, msg->host_name, sizeof(client->net.host_name));
		nvmeibt_strlcpy(client->ldisk_id.str , msg->disk_name, sizeof(client->ldisk_id.str));
		client->local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&(client->ldisk_id), &(nvmeibt_global_get_global()->local_disks_hash));
		client->client_provided_urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(client->client_provided_uuid));
		NVMEIBT_CLIENT_DUMP(nvmeibt_client_add_3, client);
	}

	NFOUT;
	return client;
}

static void nvmeibt_client_del_if_not_connected_and_no_reg_ctx_refs(struct nvmeibt_client *client) {
	NFIN;
	if (!client) {
		N_Wf(nvmeibt_client_del_if_not_connected_and_no_reg_ctx_refs_1, "client=NULL");
		goto out;
	}
	NVMEIBT_CLIENT_DUMP(nvmeibt_client_del_if_not_connected_and_no_reg_ctx_refs_4, client);
	if (client->n_reg_ctx_refs || client->is_connected) {
		N_Tf(nvmeibt_client_del_if_not_connected_and_no_reg_ctx_refs_2, "Skipping cid=@CID n_reg_ctx_refs=@N_REGISTRANTS is_connected=@IS_CONNECTED", client->cid, client->n_reg_ctx_refs, client->is_connected);
		goto out;
	}

	XHASHTABLE_DEL(&nvmeibt_global_get_global()->clients_hash, &client->topo_link);
	NNVMEIBT_TOMA_FREE(nvmeibt_client_del_if_not_connected_and_no_reg_ctx_refs_3, client);

out:
	NFOUT;
}

void nvmeibt_client_reg_ctx_ref_added(struct nvmeibt_client *client, struct nvmeibt_registrant_ctx *reg_ctx_for_logging)
{
	client->n_reg_ctx_refs++;
	N_Tf(trace_client_nvmeibt_client_reg_ctx_ref_added, "cid=@CID reg_ctx=@PTR n_reg_ctx_refs=@N_REGISTRANTS", client->cid, reg_ctx_for_logging, client->n_reg_ctx_refs);
}

void nvmeibt_client_reg_ctx_ref_removed(struct nvmeibt_client *client, struct nvmeibt_registrant_ctx *reg_ctx_for_logging)
{
	--(client->n_reg_ctx_refs);
	N_Tf(tsv84kl, "cid=@CID reg_ctx=@PTR n_reg_ctx_refs=@N_REGISTRANTS", client->cid, reg_ctx_for_logging, client->n_reg_ctx_refs);
	nvmeibt_client_del_if_not_connected_and_no_reg_ctx_refs(client);
}

int nvmeibt_client_handle_incoming_message(struct nvmeibs_toma_server_proc_buf *msg_buf, int size)
{
	int									rv;
	struct nvmeibt_register_msg			registrant_msg;
	u64									topology_version = 0;
	u32									reg_lock_id_raw = 0;
	struct nvmeibt_client_msg			*pcl;
	struct nvmeibt_host_name			msg_hdr_net;
	struct nvmeibt_urn_uuid				seg_urn_uuid;
	enum NVMEIBT_CLIENT_MSG_DECODE_RES	decode_res;

	const int min_size = sizeof(msg_buf->handle) + sizeof(*pcl);

	NFIN;

	ZEROINIT(registrant_msg);
	memset(&msg_hdr_net, 0, sizeof(msg_hdr_net));

	if (size < min_size) {
		N_Ef(trace_00_nvmeibt_client_handle_inmsg, "Failed read too short @SIZE", size);
		rv = -1;
		goto out;
	}

	registrant_msg.registrant_ctx.client_messaging_handle = msg_buf->handle;
	size -= sizeof(msg_buf->handle);

	decode_res = nvmeibt_client_thick_msg_read((u8*)msg_buf->buf, size,
			&(registrant_msg.msg_type), &(registrant_msg.reason),
			msg_hdr_net.host_name /* deprecated */, sizeof(msg_hdr_net.host_name),
			&(registrant_msg.registrant_ctx.client_protocol_version),
			&(registrant_msg.registrant_ctx.config_version),
			&(registrant_msg.registrant_ctx.volume_config_version),
			&(registrant_msg.cookie),
			&(topology_version),
			&(registrant_msg.registrant_ctx.praid_version),
			seg_urn_uuid.str, sizeof(seg_urn_uuid.str),
			&(reg_lock_id_raw),
			&(registrant_msg.registrant_ctx.reservation_mode_version),
			&(registrant_msg.registrant_ctx.client_conversation_index),
			&(registrant_msg.registrant_ctx.is_client_warrant_safe_to_rereg),
			(u8 *)&(registrant_msg.registrant_ctx.is_recoverer),
			&(registrant_msg.data_length),
			(void **)&(registrant_msg.msg_data),
			&pcl);
	nvmeibt_urn_uuid_str_to_union_uuid(&(registrant_msg.registrant_ctx.seg_uuid), seg_urn_uuid.str);
	if (decode_res != NVMEIBT_CLIENT_MSG_DECODE_OK) {
		struct nvmeibt_client_msg_summary msg_smr = nvmeibt_client_decode_msg_summary((u8*)msg_buf->buf, size);
		N_Wf(ghuy7t5, "Failed to read the message, msg_type=@MSG_TYPE reason=@REASON proto_ver=@TOMA_CLIENT_PROTOCOL_VERSION data_len=@DATA_LEN cookie=@COOKIE",
			 msg_smr.msg_type, msg_smr.reason, msg_smr.protocol_version, msg_smr.data_length, registrant_msg.cookie);
		registrant_msg.registrant_ctx.client = nvmeibt_client_find_by_cid((u32)client_messaging_handle_to_cid(registrant_msg.registrant_ctx.client_messaging_handle));
		registrant_msg.registrant_ctx.client_protocol_version = msg_smr.protocol_version;
		if (!registrant_msg.registrant_ctx.client) {
			N_Wf(skiur56, "bad msg_type=@MSG_TYPE reason=@REASON from a non-connected clnt=@HOSTNAME handle=@HANDLE cookie=@COOKIE. Ignoring",
				 registrant_msg.msg_type, registrant_msg.reason,
				msg_hdr_net.host_name, registrant_msg.registrant_ctx.client_messaging_handle, registrant_msg.cookie);
		} else if (decode_res == NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_MISMATCH) {
			const enum NVMEIBT_CLIENT_TR_REASON mismatch_reason = NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH;
			nvmeibt_register_send_toma_not_ready(&registrant_msg.registrant_ctx, mismatch_reason, &msg_smr);
		}
		rv = 0;
		goto out;
	}
	NTOMA_ASSERT(glo090t, nvmeibt_ib_protocol_signature(registrant_msg.msg_type) != NVMEIBT_PROTOCOL_SIGNATURE_LOCAL_SERVER, "wrong callback");

	registrant_msg.topology_version = topology_version;
	registrant_msg.registrant_ctx.reg_lock_id.all = reg_lock_id_raw;

	registrant_msg.registrant_ctx.client = nvmeibt_client_find_by_cid((u32)client_messaging_handle_to_cid(registrant_msg.registrant_ctx.client_messaging_handle));
	if (!registrant_msg.registrant_ctx.client) {
		N_Wf(ddjiir5, "msg_type=@MSG_TYPE reason=@REASON from a non-connected clnt=@HOSTNAME handle=@HANDLE cookie=@COOKIE. Ignoring",
			 registrant_msg.msg_type, registrant_msg.reason,
			msg_hdr_net.host_name, registrant_msg.registrant_ctx.client_messaging_handle, registrant_msg.cookie);
		rv = 0;
		goto out;
	}
	if (nvmeibt_client_is_delete_in_the_air(registrant_msg.registrant_ctx.client)) {
		N_Tf(uznlweo, "Ignoring msg from removed local_disk");
		rv = 0;
		goto out;
	}

	N_Tf(6fgwcon, "handle=@HANDLE msg_type=@MSG_TYPE reason=@REASON clnt=@HOSTNAME proto_ver=@TOMA_CLIENT_PROTOCOL_VERSION config=@CONFIG vol_ver=@VOL_VER topo=@TOPO_LLONG praid_ver=@PRAID_VERSION seg=@UUID_8 lock_id=@C_LID @RES_MOD_VER cci=@CCI data_len=@DATA_LEN cookie=@COOKIE",
		msg_buf->handle,
		registrant_msg.msg_type, registrant_msg.reason,
		nvmeibt_client_get_hostname(registrant_msg.registrant_ctx.client),
		registrant_msg.registrant_ctx.client_protocol_version,
		registrant_msg.registrant_ctx.config_version,
		registrant_msg.registrant_ctx.volume_config_version,
		registrant_msg.topology_version,
		registrant_msg.registrant_ctx.praid_version,
		nvmeib_uuid_first_4_bytes(&(registrant_msg.registrant_ctx.seg_uuid)),
		registrant_msg.registrant_ctx.reg_lock_id.all,
		(long long)registrant_msg.registrant_ctx.reservation_mode_version,
		(unsigned long long)registrant_msg.registrant_ctx.client_conversation_index,
		registrant_msg.data_length,
		registrant_msg.cookie);

	registrant_msg.registrant_ctx.seg_active = nvmeibt_find_seg_active_on_all_local_disks_by_uuid(&registrant_msg.registrant_ctx.seg_uuid);
	// Dispatch the msg
	switch (registrant_msg.msg_type & 0xffff0000) {
	case NVMEIBT_PROTOCOL_SIGNATURE_CLIENT:
	case NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_RT:
	case NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR:
		rv = nvmeibt_register_handle_incoming_message(&registrant_msg);
		break;
	case NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD:
		rv = nvmeibt_recovery_handle_incoming_msg(&registrant_msg);
		break;
	default:
		rv = -1;
		N_Ef(trace_05_nvmeibt_client_handle_inmsg, "Unexpected msg_type=@MSG_TYPE", registrant_msg.msg_type);
		break;
	}

out:
	NFOUT;
	return rv;
}

void handle_client_remove(int cid)
{
	struct nvmeibt_client		*client;

	NFIN;

	client = nvmeibt_client_find_by_cid(cid);

	if (client) {
		NVMEIBT_CLIENT_DUMP(handle_client_remove_1, client);
	} else {
		N_Wf(handle_client_remove_2, "OOPS. No client object for cid=@CID", cid); // There are a few concurent flows which call nvmeibt_client_del(). Typically, Not an error
		goto out;
	}

	// launch client-disconnect thread per client's registrant on disk's segments
	// (will also remove the client from longing_registrants)
	if (nvmeibt_register_launch_disconnected_client_removal_from_all_segments(cid, NULL) < 0) {
		N_Ef(handle_client_remove_3, "Fail to handle client disconnect event cid=@CID", cid);
		goto out;
	}

	if (client) {
		client->is_connected = 0;   // Only now, so that the client is not deleted by earlier calls to nvmeibt_client_del()
		nvmeibt_client_del_if_not_connected_and_no_reg_ctx_refs(client);
	}

out:
	NFOUT;
}

void handle_client_disconnect_event(int srv_events_fd, const struct nvmeibs_msg_s2t_client_disconnect *h)
{
	(void)srv_events_fd;
	if (h->payload_len) {
		N_Tf(ur8473w, "client disconnect cid=@CID with payload @X[bytes]", h->cid, h->payload_len);	// Deprecated
	} else {
		N_Tf(ur847cb, "client disconnect cid=@CID without payload", h->cid);
	}
	handle_client_remove(h->cid);
}

void handle_subscriber_event(struct nvmeibs_msg_s2t_subscriber_change *msg)
{
	struct nvmeibt_client		*client;
	int							n_local_disk;
	uint64_t 					calculated_hash_val = xhash_str_to_32_bits(msg->disk_name);

	NFIN;
	N_Tf(trace_client_handle_subscriber_event, "cid=@CID handle=@HANDLE host_name=@HOSTNAME disk=@STR",
			msg->cid,
			msg->toma_conn_proc_handle,
			msg->host_name,
			msg->disk_name);

	if (msg->is_subscribe) {
		client = nvmeibt_client_add(msg);		// 25.06.2019 - This function is a preliminary code, not 100% implemented, not well debugged
		if (client) {
			client->is_connected = 1;
			client->is_delete_in_the_air = 0;
		}
	} else {
		// handle_client_remove(msg->cid);
		struct nvmeibt_registrant_ctx	tmp_reg_ctx;
		struct nvmeibt_local_disk		*local_disk;
		struct nvmeibt_seg_active		*seg_active;

		memset(&tmp_reg_ctx, 0, sizeof(tmp_reg_ctx));
		tmp_reg_ctx.client_messaging_handle = msg->toma_conn_proc_handle;
		tmp_reg_ctx.registrant_node = NULL;
		tmp_reg_ctx.is_client_waiting_for_ack = 0;
		XHASHTABLE_FOR_EACH_POSSIBLE_SAFE(local_disk, &nvmeibt_global_get_global()->local_disks_hash, calculated_hash_val) {
			if (strcmp(msg->disk_name, nvmeibt_local_disk_UUID_str(local_disk)) != 0)
				continue;
			n_local_disk = XHASHTABLE_N_ELEMENTS(&nvmeibt_global_get_global()->local_disks_hash);
			XHASHTABLE_FOR_EACH_SAFE(seg_active, &(local_disk->seg_active_hash)) {
				// TODO: need to get the segment UUID from the client somehow, and use a hash-table to go
				// directly to that segment. For now, we're doing a simple search on all segments.
				tmp_reg_ctx.seg_active = seg_active;
				// implicitly unregister the registrant and remove it from longing registrants list
				nvmeibt_register_totally_remove_registrant(&tmp_reg_ctx);
				if (n_local_disk != XHASHTABLE_N_ELEMENTS(&nvmeibt_global_get_global()->local_disks_hash)) {
					N_Tf(fst6645, "Local disk removed");
					break;
				}
			}
		}
		nvmeibt_remove_longing_registrant_on_invalid_seg(msg->toma_conn_proc_handle);
	}
	NFOUT;
}

