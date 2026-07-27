#ifndef NVMEIBC_B_CP_SEND_MSG_TO_TOMA_H
#define NVMEIBC_B_CP_SEND_MSG_TO_TOMA_H

/* Send regular volumes message on a 'seg'. 'pl' are optional clnt-to-toma,
   payload and client to server payload.
   Message structure:
	+ struct nvmeibt_client_msg				// Must
    ? struct nvmeibt_client_msg_pl			// Optional
    ? nvmeibs_lost_srv_resource_payload 	// Toma never gets this part
* Toma message is encoded to network format: hton() by Block layer
* Srvr payload in encoded to big endida by transport layer */
int nvmeibc_toma_send_msg(struct nvmeibc_disk_segment *seg,
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type, enum NVMEIBT_CLIENT_TR_REASON reason,
	struct nvmeibt_client_msg_pl             *optional_toma_payload,
	bool never_reged_on_seg /* Default is false */,
	struct nvmeibs_lost_srv_resource_payload *optional_server_payload);

/* A simpler version of the above used to send direct unconditional msg to Toma.
   A msg which does not require draining/scheduling/syncronizitation with other
   mechanisms. Not suitable for UNREG/SWITCHTOPO and such */
int nvmeibc_toma_send_direct_msg(struct nvmeibc_disk_segment *seg,
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type,  struct nvmeibt_client_msg_pl *);

/* Send complains on problems */
int __send_toma_lock_help(const struct nvmeibc_cmd_lock *l       , struct nvmeibc_disk_segment *seg);
int __send_toma_cmd_help( const struct nvmeibc_d_iocmd_comp *comp, const u32 failed, const u32 fixed);
int __send_toma_di_help(  u64 addr_on_disk                       , struct nvmeibc_disk_segment *seg);

#endif

