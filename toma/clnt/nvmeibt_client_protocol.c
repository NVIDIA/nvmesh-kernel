/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "common/kr_incs.h"
#include "common/compat/kr_incs_crc32.h"		/*crc32() */
#include "common/compat/kr_incs_compiler_types.h"

#ifndef USER_SPACE
	#define NCLIENT_PROTO_ASSERT(name, cond, logging...) ({ const bool _rvc = (cond); if (!_rvc) { _NE(name, logging); WARN_ON(1); } _rvc; })
	#include "common/nvmeib_utils.h"	// Kernel client code, For prints
	#include "nvmeibc_trace.h"
#elif defined(TOMA)
	#include "nvmeibt_utils.h"
	#include "nvmeibt_common.h"
	#include <arpa/inet.h>
	#define NCLIENT_PROTO_ASSERT(name, cond, logging...) ({ const bool _rvc = (cond); NTOMA_ASSERT(name, _rvc, logging); _rvc; })
	#ifndef _NT
		#define _NT(name, fmt, ...) N_Tf(name ## _toma, fmt, ##__VA_ARGS__)
		#define _NW(name, fmt, ...) N_Wf(name ## _toma, fmt, ##__VA_ARGS__)
		#define _NE(name, fmt, ...) N_Ef(name ## _toma, fmt, ##__VA_ARGS__)
	#endif
#elif defined(UM_APP)
	#define NCLIENT_PROTO_ASSERT(name, cond, logging...) ({ assert(cond); cond; })
	#if (defined(__GNUC__) && (__GNUC__ >= 9)) || defined(__gcc__)
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
	#endif
#else
	#error "unknown user-space util compilation"
#endif
#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Waddress-of-packed-member"
#endif
#include "common/nvmeib_str.h"
#include "nvmeibt_client_protocol.h"
#include "../autogen/toma/nvmeibt_mcs_stub.h"
#define HEX_LENGTH_OF_ENUM		(20)		// For printing u64: 0x0123456789ABCDEF\0 - 19 bytes

static char	unknown_str[HEX_LENGTH_OF_ENUM];
const char *nvmeibt_protocol_client_msg_str(enum NVMEIBT_CLIENT_MSG_TYPES msg_type)
{
	switch (msg_type) {
	case NVMEIBT_CLIENT_MSG_CT_STALE_LOCK: return "CT_STALE_LOCK";
	case NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK: return "CT_FAILED_LOCK";
	case NVMEIBT_CLIENT_MSG_CT_FAILED_CMD: return "CT_FAILED_CMD";
	case NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED: return "TC_STALE_CLEANED";
	case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE: return "TC_LOCKID_CACHE_PURGE";
	case NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK: return "TC_LOCKID_CACHE_PURGE_ACK";
	case NVMEIBT_CLIENT_MSG_CT_DI_DETECTED: return "CT_DI_DETECTED";
	case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT: return "TR_REGISTRABLE_DISK_SEGMENT";
	case NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT: return "RT_REGISTER_DISK_SEGMENT";
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK: return "TR_REGISTER_DISK_SEGMENT_ACK";
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK: return "TR_REGISTER_DISK_SEGMENT_NACK";
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT: return "TR_UNREGISTER_DISK_SEGMENT";
	case NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT: return "RT_UNREGISTER_DISK_SEGMENT";
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK: return "TR_UNREGISTER_DISK_SEGMENT_ACK";
	case NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY: return "TR_SWITCH_PRAID_TOPOLOGY";
	case NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK: return "RT_SWITCH_PRAID_TOPOLOGY_ACK";
	case NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH: return "TR_VOLUME_MISMATCH";
	case NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY: return "TR_TOMA_NOT_READY";
	case NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID: return "TR_INVALID_DISK_SEGMENT_ID";
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_ANNOUNCE: return "RT_RECOVER_ANNOUNCE";
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_START: return "TR_RECOVER_START";
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT: return "TR_RECOVER_ABORT";
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH: return "RT_RECOVER_FINISH";
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS: return "RT_RECOVER_PROGRESS";
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING: return "TR_RECOVER_PING";
	case NVMEIBT_CLIENT_MSG_RT_CDV_ALLOC_EXTENT: return "RT_CDV_ALLOC_EXTENT";
	case NVMEIBT_CLIENT_MSG_TR_CDV_ALLOC_EXTENT_RSP: return "TR_CDV_ALLOC_EXTENT_RSP";
	case NVMEIBT_CLIENT_MSG_RT_CDV_FREE_EXTENT: return "RT_CDV_FREE_EXTENT";
	case NVMEIBT_CLIENT_MSG_RT_CDV_LIST_EXTENTS: return "RT_CDV_LIST_EXTENTS";
	case NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP: return "TR_CDV_LIST_EXTENTS_RSP";
	default:
		snprintf(unknown_str, sizeof(unknown_str), "0x%x", msg_type);
		return unknown_str;
	}
}

const char *nvmeibt_protocol_client_msg_reason_str(enum NVMEIBT_CLIENT_TR_REASON reason)
{
	switch (reason) {
	case NVMEIBT_CLIENT_TR_REASON_UNUSED:						return "-";
	case NVMEIBT_CLIENT_TR_REASON_NONE:							return "NONE";
	case NVMEIBT_CLIENT_TR_REASON_TBD:							return "REASON_TBD";
	case NVMEIBT_CLIENT_TR_REASON_LOCKID_ALREADY_TAKEN:			return "LOCKID_TAKEN";
	case NVMEIBT_CLIENT_TR_REASON_LOCKID_MESS:					return "LOCKID_MESS";
	case NVMEIBT_CLIENT_TR_REASON_LOCKID_CLNT_COMPLAIN:			return "LOCK_TAKEN_TOO_LONG";
	case NVMEIBT_CLIENT_TR_REASON_VOL_VERSION_BEHIND:			return "VOL_VERSION_BEHIND";
	case NVMEIBT_CLIENT_TR_REASON_VOL_VERSION_AHEAD:			return "VOL_VERSION_AHEAD";
	case NVMEIBT_CLIENT_TR_REASON_DELETING_SEG:					return "DELETING_SEG";
	case NVMEIBT_CLIENT_TR_REASON_CONF_CORRUPTED:				return "CONF_CORRUPTED";
	case NVMEIBT_CLIENT_TR_REASON_CONFIG_VERSION_BEHIND:		return "CONFIG_VERSION_BEHIND";
	case NVMEIBT_CLIENT_TR_REASON_CONFIG_VERSION_AHEAD:			return "CONFIG_VERSION_AHEAD";
	case NVMEIBT_CLIENT_TR_REASON_AWAITING_CLIENTS_SYNC:		return "AWAITING_CLIENTS_SYNC";
	case NVMEIBT_CLIENT_TR_REASON_SHUTDOWN:						return "SHUTDOWN";
	case NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH:		return "PROTO_VER_MISMATCH";
	case NVMEIBT_CLIENT_TR_REASON_PRAID_VERSION_BEHIND:			return "PRAID_VERSION_BEHIND";
	case NVMEIBT_CLIENT_TR_REASON_PRAID_VERSION_AHEAD:			return "PRAID_VERSION_AHEAD";
	case NVMEIBT_CLIENT_TR_REASON_UNREGISTER_IN_PROGRESS:		return "UNREGISTER_IN_PROGRESS";
	case NVMEIBT_CLIENT_TR_REASON_INVALID_SEG_ID:				return "INVALID_SEG_ID";
	case NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_BROKEN:			return "SEG_MD_BROKEN";
	case NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_NOT_STORED:		return "SEG_MD_NOT_STORED";
	case NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_STORING:			return "SEG_MD_STORING";
	case NVMEIBT_CLIENT_TR_REASON_SEG_STATE_NOT_REGISTRABLE:	return "SEG_STATE_NOT_REGISTRABLE";
	case NVMEIBT_CLIENT_TR_REASON_NON_LOCAL_DISK:				return "NON_LOCAL_DISK";
	case NVMEIBT_CLIENT_TR_REASON_NO_DISK_IN_CONFIG:			return "NO_DISK_IN_CONFIG";
	case NVMEIBT_CLIENT_TR_REASON_PRAID_NOT_ACTIVATED:			return "PRAID_NOT_ACTIVATED";
	case NVMEIBT_CLIENT_TR_REASON_PRAID_NEVER_ACTIVATED:		return "PRAID_NEVER_ACTIVATED";
	case NVMEIBT_CLIENT_TR_REASON_SEG_STATE_INITIALIZING:		return "SEG_STATE_INITIALIZING";
	case NVMEIBT_CLIENT_TR_REASON_INIT:							return "INIT";
	case NVMEIBT_CLIENT_TR_REASON_SW_TOPO_W:					return "SW_TOPO_W";
	case NVMEIBT_CLIENT_TR_REASON_SW_TOPO_D:					return "SW_TOPO_D";
	case NVMEIBT_CLIENT_TR_REASON_SW_TOPO_STABLE_UNSAFE:		return "SW_TOPO_STABLE_UNSAFE";
	case NVMEIBT_CLIENT_TR_REASON_SW_TOPO_STABLE_SAFE:			return "SW_TOPO_STABLE_SAFE";
	case NVMEIBT_CLIENT_TR_REASON_SW_TOPO_X:					return "SW_TOPO_X";
	case NVMEIBT_CLIENT_TR_REASON_CUT_REQ:						return "CUT_REQ";
	case NVMEIBT_CLIENT_TR_REASON_BLKS_ZEROING:					return "SEG_BLKS_ZEROING";
	case NVMEIBT_CLIENT_TR_REASON_WAIT_4_SERJIO:				return "SEG_WAIT_4_SERJIO";
	case NVMEIBT_CLIENT_TR_REASON_INTERNAL_ERR:					return "REASON_INTERNAL_ERR";
	// -------------- Client --> Toma ---------------------
	case NVMEIBT_CLIENT_RT_REASON_DIRECT:						return "DIRECT";
	case NVMEIBT_CLIENT_RT_REASON_INSTRUCTED_DEAD:				return "INSTRUCTED_DEAD";
	case NVMEIBT_CLIENT_RT_REASON_DELAYED_SW_TOPO_ACK:			return "DELAYED_SW_TOPO_ACK";
	case NVMEIBT_CLIENT_RT_REASON_DELAYED_SW_TOPO_UPD:			return "DELAYED_SW_TOPO_UPD";
	case NVMEIBT_CLIENT_RT_REASON_INLINE_SW_TOPO:				return "INLINE_SW_TOPO";
	case NVMEIBT_CLIENT_RT_REASON_REG_ON_PR_UPDATE:				return "PR_UPDATE";
	case NVMEIBT_CLIENT_RT_REASON_REG_ON_PR_UPDATE_SWTOPO:		return "PR_UPDATE_SW_TOPO";
	case NVMEIBT_CLIENT_RT_REASON_INSTRUCTED_UNREG:				return "INSTRUCTED_UNREG";
	case NVMEIBT_CLIENT_RT_REASON_REREG_ON_IO_FAIL:				return "REREG_ON_IO_FAIL";
	case NVMEIBT_CLIENT_RT_REASON_WARM_UNREGISTER:				return "WARM_UNREGISTER";
	case NVMEIBT_CLIENT_RT_REASON_WARM_REGISTER:				return "WARM_REGISTER";
	case NVMEIBT_CLIENT_RT_REASON_UNREG_DISK_PAUSE:				return "UNREG_DISK_PAUSE";
	case NVMEIBT_CLIENT_RT_REASON_REG_DISK_CONT:				return "REG_DISK_CONT";
	case NVMEIBT_CLIENT_RT_REASON_POISON_RAID:					return "POISON_RAID";
	case NVMEIBT_CLIENT_RT_REASON_REJECT_REG_ACK:				return "REJECT_REG_ACK";
	case NVMEIBT_CLIENT_RT_REASON_UNREG_ALL_XXX:				return "UNREG_ALL_XXX";
	case NVMEIBT_CLIENT_RT_REASON_REG_ALL_XXX:					return "REG_ALL_XXX";
	default:
		snprintf(unknown_str, sizeof(unknown_str), "0x%x", reason);
		return unknown_str;
	}
}

const char *nvmeibt_ib_protocol_signature_to_str(enum NVMEIBT_IB_PROTOCOL_SIGNATURE signature)
{
	switch ((unsigned)signature & 0xffff0000) {
	case NVMEIBT_IB_PROTOCOL_SIGNATURE_RAFT: 				return "RAFT";
	case NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA: 				return "TOMA";
	case NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY: 		return "TOMA_RECOVERY";
	case NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY_LOCK:	return "TOMA_RECOVERY_LOCK";
	case NVMEIBT_PROTOCOL_SIGNATURE_CLIENT: 				return "CLIENT";
	case NVMEIBT_PROTOCOL_SIGNATURE_LOCAL_SERVER: 			return "LOCAL_SERVER";
	case NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR: 			return "REGISTER_TR";
	case NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_RT: 			return "REGISTER_RT";
	case NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD: 			return "TOMA_REBUILD";
	case NVMEIBT_PROTOCOL_SIGNATURE_RAFT_FRAME_ACK:			return "RAFT_FRAME_ACK";
	case NVMEIBT_PROTOCOL_SIGNATURE_RAFT_FINAL_ACK:			return "RAFT_FINAL_ACK";

	default: return "UNKNOWN";
	}
}

const char *nvmeibt_praid_type_str(enum NVMEIBT_PRAID_TYPE praid_type)
{
	switch (praid_type) {
	case NVMEIBT_PRAID_TYPE_JBOD:		return "JBOD";
	case NVMEIBT_PRAID_TYPE_RAID1:		return "RAID1";
	case NVMEIBT_PRAID_TYPE_RAID5:		return "RAID5";
	case NVMEIBT_PRAID_TYPE_RAID6:		return "RAID6";
	case NVMEIBT_PRAID_TYPE_RAID5DP:	return "RAID5DP";
	default:							return "Unknown";
	}
}

#define strcpy_to_msg(dst, src)	  nvmeib_strlcpy(dst, src, sizeof(dst))
#define strcpy_from_msg(dst, src, dst_size) ({		\
	if (&(dst[0]) != &(src[0])) {					\
		nvmeib_strlcpy(dst, src, dst_size);			\
	}												\
})

static void __msg_header_construct(struct nvmeibt_client_msg_header *hdr,
					s32 msg_type, s32 reason, const char *clnt_host_name,
					uint proto_ver, s32 conf_ver, s32 vol_conf_ver, u64 msg_id)
{
	hdr->msg_type              = msg_type;
	hdr->reason                = reason;
	strcpy_to_msg(hdr->clnt_host_name, clnt_host_name);
	hdr->protocol_version      = proto_ver;
	hdr->config_version        = conf_ver;
	hdr->volume_config_version = vol_conf_ver;
	hdr->cookie				   = msg_id;
}

static void __msg_header_conv(struct nvmeibt_client_msg_header *hdr, bool do_enc)
{
	if (do_enc) {
		hdr->msg_type               = nvmeib_htonl(hdr->msg_type);
		hdr->reason                 = nvmeib_htonl(hdr->reason);
		hdr->protocol_version       = nvmeib_htonl(hdr->protocol_version);
		hdr->config_version         = nvmeib_htonl(hdr->config_version);
		hdr->volume_config_version  = nvmeib_htonl(hdr->volume_config_version);
	} else {
		hdr->msg_type     			= nvmeib_ntohl(hdr->msg_type);
		hdr->reason       			= nvmeib_ntohl(hdr->reason);
		hdr->protocol_version  	 	= nvmeib_ntohl(hdr->protocol_version);
		hdr->config_version			= nvmeib_ntohl(hdr->config_version);
		hdr->volume_config_version	= nvmeib_ntohl(hdr->volume_config_version);
	}
}

static void __failed_cmd_pl_conv(struct nvmeibt_client_failed_cmd_pl *p, bool do_enc)
{
	if (do_enc) {
		p->error_code = nvmeib_htons( p->error_code);
		p->offset     = NVMEIB_HTONLL(p->offset);
		p->op         = nvmeib_htonl( p->op);
	} else {
		p->error_code = nvmeib_ntohs( p->error_code);
		p->offset     = NVMEIB_NTONLL(p->offset);
		p->op         = nvmeib_ntohl( p->op);
	}
}

static void __failed_lock_pl_conv(struct nvmeibt_client_failed_lock_pl *p, bool do_enc)
{
	if (do_enc) {
		p->lock_op			= nvmeib_htonl( p->lock_op);
		p->status			= nvmeib_htonl( p->status);
		p->is_problem_here	= nvmeib_htonl( p->is_problem_here);
		p->disk_blkno_4k  	= NVMEIB_HTONLL(p->disk_blkno_4k);
		p->curr    			= NVMEIB_HTONLL(p->curr);
		p->comp    			= NVMEIB_HTONLL(p->comp);
		p->xchg    			= NVMEIB_HTONLL(p->xchg);
	} else {
		p->lock_op 			= nvmeib_ntohl( p->lock_op);
		p->status  			= nvmeib_ntohl( p->status);
		p->is_problem_here	= nvmeib_ntohl( p->is_problem_here);
		p->disk_blkno_4k	= NVMEIB_NTONLL(p->disk_blkno_4k);
		p->curr    			= NVMEIB_NTONLL(p->curr);
		p->comp    			= NVMEIB_NTONLL(p->comp);
		p->xchg    			= NVMEIB_NTONLL(p->xchg);
	}
}

static __attribute__((unused)) void _cleaned_stalock_convert(struct nvmeibt_cleaned_stalock_info *r, bool do_enc)
{
	if (do_enc)	{
		r->lock_id = nvmeib_htonl( r->lock_id);
	} else {
		r->lock_id = nvmeib_ntohl( r->lock_id);
	}
}

static __attribute__((unused)) void __lockid_cache_purge_pl_convert(struct nvmeibt_lockid_cache_purge_pl *pl, bool do_enc)
{
	if (do_enc) {
		pl->purge_seqno		= NVMEIB_HTONLL(pl->purge_seqno);
		pl->start_counter	= NVMEIB_HTONLL(pl->start_counter);
		pl->length			= NVMEIB_HTONLL(pl->length);
	} else {
		pl->purge_seqno		= NVMEIB_NTONLL(pl->purge_seqno);
		pl->start_counter	= NVMEIB_NTONLL(pl->start_counter);
		pl->length			= NVMEIB_NTONLL(pl->length);
	}
}

static void __recovery_hdr_convert(struct nvmeibt_client_recovery_generic_header *r, bool do_enc)
{
	if (do_enc)	{ r->id = NVMEIB_HTONLL(r->id); r->type = nvmeib_htonl(r->type); r->max_batch_size = nvmeib_htonl(r->max_batch_size);
	} else {      r->id = NVMEIB_NTONLL(r->id); r->type = nvmeib_ntohl(r->type); r->max_batch_size = nvmeib_ntohl(r->max_batch_size);}
}

static __attribute__((unused)) void __attr_no_alignment_sanity __recovery_start_pl_convert(struct nvmeibt_client_recovery_start_pl *r, bool do_enc)
{
	if (do_enc)	{
		const enum NVMEIBT_RECOVERY_TYPE r_type = r->task.type;
		__recovery_hdr_convert(&r->task, do_enc);
		r->is_mandatory  = nvmeib_htonl( r->is_mandatory);
		r->do_only_owners= nvmeib_htonl( r->do_only_owners);
		r->start_lock    = NVMEIB_HTONLL(r->start_lock);
		r->num_locks     = NVMEIB_HTONLL(r->num_locks);
		if (r_type == NVMEIBT_RECOVERY_TYPE_EC_COLD)
			r->cold.surviving_ram_bmp = nvmeib_htonl(r->cold.surviving_ram_bmp);
	} else {
		__recovery_hdr_convert(&r->task, do_enc);
		r->is_mandatory  = nvmeib_ntohl( r->is_mandatory);
		r->do_only_owners= nvmeib_ntohl( r->do_only_owners);
		r->start_lock    = NVMEIB_NTONLL(r->start_lock);
		r->num_locks     = NVMEIB_NTONLL(r->num_locks);
		if (r->task.type == NVMEIBT_RECOVERY_TYPE_EC_COLD)
			r->cold.surviving_ram_bmp = nvmeib_ntohl(r->cold.surviving_ram_bmp);
	}
}

static inline void __recovery_taskid_pl_convert(struct nvmeibt_client_recovery_taskid_pl *r, bool do_enc) { __recovery_hdr_convert(&r->task, do_enc); }
static void __recovery_status_pl_convert(struct nvmeibt_client_recovery_status_pl *r, bool do_enc)
{
	if (do_enc)	{
		__recovery_hdr_convert(&r->task, do_enc);
		r->ret_code          = nvmeib_htonl( r->ret_code);
		r->praid_version     = nvmeib_htonl( r->praid_version);
		r->next_unfixed_lock = NVMEIB_HTONLL(r->next_unfixed_lock);
		r->num_locks_left    = NVMEIB_HTONLL(r->num_locks_left);
	} else {
		__recovery_hdr_convert(&r->task, do_enc);
		r->ret_code          = nvmeib_ntohl( r->ret_code);
		r->praid_version     = nvmeib_ntohl( r->praid_version);
		r->next_unfixed_lock = NVMEIB_NTONLL(r->next_unfixed_lock);
		r->num_locks_left    = NVMEIB_NTONLL(r->num_locks_left);
	}
}

__attribute__((unused)) static inline void nvmeib_ecc_load_t_convert(struct nvmeib_ecc_load_t *pl, bool do_enc)
{
	if (do_enc) {
		pl->load_level =    nvmeib_htonl(pl->load_level);
		pl->n_blocks_have = nvmeib_htonl(pl->n_blocks_have);
	} else {
		pl->load_level =    nvmeib_ntohl(pl->load_level);
		pl->n_blocks_have = nvmeib_ntohl(pl->n_blocks_have);
	}
}

static void __msg_summary_convert(struct nvmeibt_client_msg_summary *r, bool do_enc)
{
	if (do_enc) {
		r->protocol_version = nvmeib_htonl(r->protocol_version);
		r->msg_type         = nvmeib_htonl(r->msg_type);
		r->reason           = nvmeib_htonl(r->reason);
		r->data_length 		= nvmeib_htonl(r->data_length);
	} else {
		r->protocol_version = nvmeib_ntohl(r->protocol_version);
		r->msg_type     	= nvmeib_ntohl(r->msg_type);
		r->reason       	= nvmeib_ntohl(r->reason);
		r->data_length		= nvmeib_ntohl(r->data_length);
	}
}

static void __thick_vol_conv(struct nvmeibt_client_thick_volume *v, bool do_enc)
{
	if (do_enc) {
		v->topology_version = NVMEIB_HTONLL(v->topology_version);
		v->praid_version    = nvmeib_htonl( v->praid_version);
		v->lock_id          = nvmeib_htonl( v->lock_id);
		v->reservation_mode_version    = NVMEIB_HTONLL(v->reservation_mode_version);
		v->conversation_ind = NVMEIB_HTONLL(v->conversation_ind);
		v->data_length      = nvmeib_htonl( v->data_length);
	} else {
		v->topology_version = NVMEIB_NTONLL(v->topology_version);
		v->praid_version    = nvmeib_ntohl( v->praid_version);
		v->lock_id          = nvmeib_ntohl( v->lock_id);
		v->reservation_mode_version    = NVMEIB_NTONLL(v->reservation_mode_version);
		v->conversation_ind = NVMEIB_NTONLL(v->conversation_ind);
		v->data_length      = nvmeib_ntohl( v->data_length);
	}
}

static void __topo_raid_conv(struct nvmeibt_client_topo_praid *pr, bool do_enc)
{
	if (do_enc) {
		pr->praid_version =			nvmeib_htonl(pr->praid_version);
		pr->topo_checksum =			nvmeib_htonl(pr->topo_checksum);
		pr->blkset_sync_safety =	pr->blkset_sync_safety;
		pr->n_segments = 			nvmeib_ntohb(pr->n_segments);
	} else {
		pr->praid_version =			nvmeib_ntohl(pr->praid_version);
		pr->topo_checksum =			nvmeib_ntohl(pr->topo_checksum);
		pr->blkset_sync_safety =	pr->blkset_sync_safety;
		pr->n_segments =			nvmeib_ntohb(pr->n_segments);
	}
}

static void __topo_seg_conv(struct nvmeibt_client_topo_disk_segment *s, bool do_enc)
{
	if (do_enc) {
		s->access_mode = nvmeib_htonl(s->access_mode);
	} else {
		s->access_mode = nvmeib_ntohl(s->access_mode);
	}
}

static inline bool nvmeibt_protocol_client_msg_is_valid(enum NVMEIBT_CLIENT_MSG_TYPES msg_type)
{
	switch (msg_type) {
	case NVMEIBT_CLIENT_MSG_CT_STALE_LOCK:
	case NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK:
	case NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED:
	case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE:
	case NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK:
	case NVMEIBT_CLIENT_MSG_CT_FAILED_CMD:
	case NVMEIBT_CLIENT_MSG_CT_DI_DETECTED:
	case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK:
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK:
	case NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY:
	case NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK:
	case NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH:
	case NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY:
	case NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_START:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT:
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH:
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING:
	/* CDV thin-provisioning messages */
	case NVMEIBT_CLIENT_MSG_RT_CDV_ALLOC_EXTENT:
	case NVMEIBT_CLIENT_MSG_TR_CDV_ALLOC_EXTENT_RSP:
	case NVMEIBT_CLIENT_MSG_RT_CDV_FREE_EXTENT:
	case NVMEIBT_CLIENT_MSG_RT_CDV_LIST_EXTENTS:
	case NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP:
	case NVMEIBT_CLIENT_MSG_TR_CDV_ALLOCATOR_UPDATE:
		return true;
	default:
		return false;
	}
}

/* Process only msg without payload */
static void __client_msg_convert_no_payload(struct nvmeibt_client_msg* msg, bool do_enc)
{
	const struct nvmeibt_client_msg_header *h = &msg->hdr;
	const s32 type = (do_enc ? h->msg_type : (s32)nvmeib_ntohl(h->msg_type));
	NCLIENT_PROTO_ASSERT(error_client_protocol_client_msg_convert_no_payload,
			nvmeibt_protocol_client_msg_is_valid(type),"type=@MSG_TYPE", type);
	__msg_header_conv(&msg->hdr, do_enc);

	if (nvmeibt_ib_protocol_signature(type) == NVMEIBT_PROTOCOL_SIGNATURE_LOCAL_SERVER) {
		/* NOP */
	} else {
		__thick_vol_conv(&msg->thick, do_enc);
	}
}

/* Todo: Fix Toma's external econding of topology and unify with nvmeibt_client_encode()*/
void nvmeibt_client_thick_msg_write(struct nvmeibt_client_msg *m,
									s32			msg_type,
									s32			reason,
									const char	*clnt_host_name,
									u32			protocol_version,
									s32			config_version,
									s32			volume_config_version,
									u64			topology_version,
									s32			praid_version,
									const char	*disk_segment_uuid,
									u32			lock_id,
									u64			reservation_mode_version,
									u64  		conversation_ind,
									u8			rt_never_reged_on_seg,
									u8			is_REGISTER_for_recovery,
									u32			data_length,
									void		*data,
									u64			msg_id)
{
	struct nvmeibt_client_thick_volume *thick = &m->thick;
	__msg_header_construct(&m->hdr, msg_type, reason, clnt_host_name, protocol_version, config_version, volume_config_version, msg_id);
	thick->topology_version = topology_version;		// Todo: Move the lines below to constructor of struct
	thick->praid_version    = praid_version;
	strcpy_to_msg(thick->disk_segment_uuid, disk_segment_uuid);
	thick->lock_id          = lock_id;
	thick->reservation_mode_version    = reservation_mode_version;
	thick->conversation_ind = conversation_ind;
	thick->rt_never_reged_on_seg = rt_never_reged_on_seg;
	thick->is_REGISTER_for_recovery = is_REGISTER_for_recovery;
	thick->data_length      = data_length;
	if (data){
		unsafe_memcpy(thick->data, data, data_length, "TOMA Protocol");
	}

	nvmeibt_client_encode(m);
}

enum NVMEIBT_CLIENT_MSG_DECODE_RES
nvmeibt_client_thick_msg_read(u8 *buf, s32 len,
							  enum NVMEIBT_CLIENT_MSG_TYPES	*msg_type,
							  s32							*reason,
							  char							*clnt_host_name,
							  size_t						clnt_host_name_size,
							  u32							*proto_ver,
							  s32							*conf_ver,
							  s32							*vol_conf_ver,
 							  u64							*msg_id,
							  u64							*topology_version,
							  s32							*praid_version,
							  char							*disk_segment_uuid,
							  size_t						disk_segment_uuid_size,
							  u32							*lock_id,
							  u64							*reservation_mode_version,
							  u64          				 	*conversation_ind,
							  u8			   				*rt_never_reged_on_seg,
							  u8			   				*is_REGISTER_for_recovery,
							  u32							*data_length,
							  void							**data,
							  struct nvmeibt_client_msg		**pcl)
{
	struct nvmeibt_client_msg				*m;
	enum NVMEIBT_CLIENT_MSG_DECODE_RES		rv;
	struct nvmeibt_client_thick_volume		*thick;
	struct nvmeibt_client_msg_header 		*hdr;

	rv = nvmeibt_client_decode_new(buf, len, &m);
	switch (rv) {
	case NVMEIBT_CLIENT_MSG_DECODE_OK:
		thick = &m->thick;
		hdr = &m->hdr;

		*msg_type     = hdr->msg_type;
		*reason       = hdr->reason;
		strcpy_from_msg(clnt_host_name, hdr->clnt_host_name, clnt_host_name_size);
		*proto_ver    = hdr->protocol_version;
		*conf_ver     = hdr->config_version;
		*vol_conf_ver = hdr->volume_config_version;
		*msg_id		  = hdr->cookie;

		*topology_version = thick->topology_version;
		*praid_version    = thick->praid_version;
		strcpy_from_msg(disk_segment_uuid, thick->disk_segment_uuid, disk_segment_uuid_size);
		*lock_id          = thick->lock_id;
		*reservation_mode_version    = thick->reservation_mode_version;
		*conversation_ind = thick->conversation_ind;
		*rt_never_reged_on_seg = thick->rt_never_reged_on_seg;
		*is_REGISTER_for_recovery = thick->is_REGISTER_for_recovery;
		*data_length      = thick->data_length;
		if (data && *data_length) {
			*data = (void*)thick->data;
		}
		*pcl = m;
		break;
	case NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_MISMATCH:
	case NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_ERROR:
		*pcl = NULL;
		break;
	}
	return rv;
}

static u32 __calc_topo_checksum(const struct tTopoOfPraid *T)
{
	u32			rv = ~0;
	int8_t		i, n_segs = T->header.n_segments;
	rv = crc32(rv, &T->header, sizeof(T->header));
	for (i = 0; i < n_segs; i++)
		rv = crc32(rv, &T->s[i], sizeof(T->s[i]));
	return rv;
}

/********************** ENCODE / DECODE / back compatibility ******************/
#define err_msg   "len=@LEN size=@LEN msg_type=@MSG_TYPE"
#define VERIFY_NO_BUF_OVERRUN(name, p, buf, len)    if (!NCLIENT_PROTO_ASSERT(name, (int)(((u8*)p) - buf) < len, err_msg, (int)(((u8*)p) - buf), len, msg_type)) goto _decode_err;
#define VERIFY_LEN_EQUAL(     name, len1, len2 )	if (!NCLIENT_PROTO_ASSERT(name, (len1) == (int)(len2)      , err_msg, len1,            (int)len2, msg_type)) goto _decode_err;
#define VERIFY_ENCODE_BUF(  name, pl_len, size_of)       NCLIENT_PROTO_ASSERT(name, pl_len >= (u32)(size_of)   , err_msg, pl_len,       (u32)size_of, msg_type)

void nvmeibt_client_encode(struct nvmeibt_client_msg* msg)
{
	const enum NVMEIBT_CLIENT_MSG_TYPES msg_type = msg->hdr.msg_type;
	const u32 pl_len = msg->thick.data_length;
	const bool has_payload  = (msg->thick.data_length > 0);
	struct nvmeibt_client_msg_pl *pl = (void*)msg->thick.data;
	const bool encode = true;
	const bool bc_v20 = (NVMEIBT_CLIENT_PROTO_VERSION_202 == msg->hdr.protocol_version);	// Backward compatible to V2.0.2
	const bool bc_v2_8_0 = (NVMEIBT_CLIENT_PROTO_VERSION_2_8_0 == msg->hdr.protocol_version);   // Backward compatible to V2.8.0

	if (!bc_v20 && !bc_v2_8_0) {
		NCLIENT_PROTO_ASSERT(t_08_clnt_toma_proto_enc, false, "unknown protocol_version=@TOMA_CLIENT_PROTOCOL_VERSION", msg->hdr.protocol_version);
		goto _out;
	}

	__client_msg_convert_no_payload(msg, encode);
	if (!has_payload)
		goto _out;

	/******** Encode clients payloads *******/
	switch (msg_type) {
	case NVMEIBT_CLIENT_MSG_CT_FAILED_CMD:
	case NVMEIBT_CLIENT_MSG_CT_DI_DETECTED:
		VERIFY_ENCODE_BUF(t_00_clnt_toma_proto_enc, pl_len, sizeof(pl->failed_cmd));
		__failed_cmd_pl_conv(&pl->failed_cmd, encode);
		break;

	case NVMEIBT_CLIENT_MSG_CT_STALE_LOCK:
	case NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK:
		VERIFY_ENCODE_BUF(t_01_clnt_toma_proto_enc, pl_len, sizeof(pl->failed_lock));
		__failed_lock_pl_conv(&pl->failed_lock, encode);
		break;

	case NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH:
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS:
		VERIFY_ENCODE_BUF(t_02_clnt_toma_proto_enc, pl_len, sizeof(pl->recov_status));
		__recovery_status_pl_convert(&pl->recov_status, encode);
		break;

	/******** Encode Tomas payloads *******/
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_START:
		VERIFY_ENCODE_BUF(t_03_clnt_toma_proto_enc, pl_len, sizeof(pl->recov_start));
		__recovery_start_pl_convert(&pl->recov_start, encode);
		break;

	case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT:
		VERIFY_ENCODE_BUF(t_04_clnt_toma_proto_enc, pl_len, sizeof(pl->recov));
		__recovery_taskid_pl_convert(&pl->recov, encode);
		break;

	case NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED:
		VERIFY_ENCODE_BUF(t_05_clnt_toma_proto_enc, pl_len, sizeof(pl->stalock_info));
		_cleaned_stalock_convert(&pl->stalock_info, encode);
		break;

	case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE:
	case NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK:
		VERIFY_ENCODE_BUF(t_06_clnt_toma_proto_enc, pl_len, sizeof(pl->lockid_cache_purge));
		__lockid_cache_purge_pl_convert(&pl->lockid_cache_purge, encode);
		break;

	case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK:
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY: {
		/* Payload praid topo */
		struct tTopoOfPraid *T = (void*)pl;
		int8_t		i, n_segs = T->header.n_segments;
		T->header.topo_checksum = __calc_topo_checksum(T);
		__topo_raid_conv(&T->header, encode);
		for (i = 0; i < n_segs; i++) {
			__topo_seg_conv(&T->s[i], encode);
		}
		break;
	}
	case NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY:{
		struct nvmeibt_client_msg_summary* m = (typeof(m))pl;
		VERIFY_ENCODE_BUF(t_07_clnt_toma_proto_enc, pl_len, sizeof(*m));
		__msg_summary_convert(m, encode);
		break;
	}
	default:
		/* No payload */
		break;
	} /* switch (msg_type) */
_out:;
}

struct nvmeibt_client_msg *nvmeibt_client_decode(u8 *buf, s32 len)
{
	struct nvmeibt_client_msg  *msg = (struct nvmeibt_client_msg*)buf;
	struct nvmeibt_client_msg_pl *pl;
	const u32 proto_ver = nvmeib_ntohl(msg->hdr.protocol_version);
	const bool decode = false;
	const bool bc_v20 = (NVMEIBT_CLIENT_PROTO_VERSION_202 == proto_ver);	// Backward compatible to V2.0.2
	const bool bc_v2_8_0 = (NVMEIBT_CLIENT_PROTO_VERSION_2_8_0 == proto_ver);   // Backward compatible to V2.0.2
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type;

	NCLIENT_PROTO_ASSERT(t_17_clnt_toma_proto_decode, (bc_v20 || bc_v2_8_0), "unknown protocol_version=@TOMA_CLIENT_PROTOCOL_VERSION", proto_ver);
	NCLIENT_PROTO_ASSERT(t_00_clnt_toma_proto_decode,		// Cant be valid before decode because of bit/little endianess
		(len >= (s32)sizeof(*msg)) &&
		!nvmeibt_protocol_client_msg_is_valid(msg->hdr.msg_type), err_msg, len, (int)sizeof(*msg), msg->hdr.msg_type);

	__client_msg_convert_no_payload(msg, decode);
	pl = (struct nvmeibt_client_msg_pl*)msg->thick.data;

	msg_type = msg->hdr.msg_type;
	switch (msg_type) {

	/* Messages without payload */
	case NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK:
	case NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID:
	case NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH:
	case NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK:
		VERIFY_LEN_EQUAL(t_02_clnt_toma_proto_decode, len, sizeof(*msg)); // Ensure we decoded the whole msg
		break;

	/* Messages with mandatory payload */
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_START:
		VERIFY_NO_BUF_OVERRUN(t_03_clnt_toma_proto_decode, pl, buf, len);
		__recovery_start_pl_convert(&pl->recov_start, decode);
		break;

	case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT:
		VERIFY_NO_BUF_OVERRUN(t_04_clnt_toma_proto_decode, pl, buf, len);
		__recovery_taskid_pl_convert(&pl->recov, decode);
		break;

	case NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS:
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH:
		VERIFY_NO_BUF_OVERRUN(t_05_clnt_toma_proto_decode, &pl->recov_status, buf, len);
		__recovery_status_pl_convert(&pl->recov_status, decode);
		break;

	case NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED: {
		struct nvmeibt_cleaned_stalock_info *rpl = &pl->stalock_info;
		VERIFY_NO_BUF_OVERRUN(t_06_clnt_toma_proto_decode, rpl, buf, len);
		_cleaned_stalock_convert(rpl, decode);
		break;
	}

	case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE:
	case NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK:
		VERIFY_NO_BUF_OVERRUN(t_07_clnt_toma_proto_decode, &pl->lockid_cache_purge, buf, len);
		__lockid_cache_purge_pl_convert(&pl->lockid_cache_purge, decode);
		break;

	case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK:
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY: {
		struct nvmeibt_client_topo_praid *pr1 = (struct nvmeibt_client_topo_praid*)pl;
		struct nvmeibt_client_topo_disk_segment *seg;
		int8_t		i;
		VERIFY_NO_BUF_OVERRUN(t_08_clnt_toma_proto_decode, pr1, buf, len);
		__topo_raid_conv(pr1, decode);
		seg = (struct nvmeibt_client_topo_disk_segment*)(pr1+1);
		for (i=0; i < pr1->n_segments; i++, seg++) {
			VERIFY_NO_BUF_OVERRUN(t_09_clnt_toma_proto_decode, seg, buf, len);
			__topo_seg_conv(seg, decode);
		}
		VERIFY_LEN_EQUAL(t_10_clnt_toma_proto_decode, len, ((u8*)seg - buf));
		break;
	}

	case NVMEIBT_CLIENT_MSG_CT_FAILED_CMD:
	case NVMEIBT_CLIENT_MSG_CT_DI_DETECTED:
		__failed_cmd_pl_conv(&pl->failed_cmd, decode);
		VERIFY_LEN_EQUAL(t_11_clnt_toma_proto_decode, len, ((u8*)&pl[1] - buf));
		break;

	case NVMEIBT_CLIENT_MSG_CT_STALE_LOCK:
	case NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK:
		__failed_lock_pl_conv(&pl->failed_lock, decode);
		VERIFY_LEN_EQUAL(t_12_clnt_toma_proto_decode, len, ((u8*)&pl[1] - buf));
		break;

	case NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY: {
		const enum NVMEIBT_CLIENT_TR_REASON reason = msg->hdr.reason;
		if (reason == NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH){
			__msg_summary_convert(&pl->prior_msg_summary, decode);
			VERIFY_LEN_EQUAL(t_13_clnt_toma_proto_decode, len, ((u8*)pl + sizeof(pl->prior_msg_summary) - buf));
		} else {
			VERIFY_LEN_EQUAL(t_14_clnt_toma_proto_decode, len, sizeof(*msg));
		}
		break;
	}
	/* CDV thin-provisioning messages: payloads are raw structs, no field conversion */
	case NVMEIBT_CLIENT_MSG_RT_CDV_ALLOC_EXTENT:
	case NVMEIBT_CLIENT_MSG_TR_CDV_ALLOC_EXTENT_RSP:
	case NVMEIBT_CLIENT_MSG_RT_CDV_FREE_EXTENT:
	case NVMEIBT_CLIENT_MSG_RT_CDV_LIST_EXTENTS:
	case NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP:
	case NVMEIBT_CLIENT_MSG_TR_CDV_ALLOCATOR_UPDATE:
		break;
	default:
		NCLIENT_PROTO_ASSERT(t_15_clnt_toma_proto_decode, false, err_msg, len, (int)sizeof(*msg), msg->hdr.msg_type);
		break;
	}
	return (struct nvmeibt_client_msg*)buf;

_decode_err:
	return NULL;
}

enum NVMEIBT_CLIENT_MSG_DECODE_RES nvmeibt_client_decode_new(u8 *buf, s32 len, struct nvmeibt_client_msg **decoded_msg)
{
	struct nvmeibt_client_msg  *msg = (struct nvmeibt_client_msg*)buf;
	struct nvmeibt_client_msg_pl *pl;
	const u32 proto_ver = nvmeib_ntohl(msg->hdr.protocol_version);
	const bool decode = false;
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type;

	if ((proto_ver != NVMEIBT_CLIENT_PROTO_VERSION) && (proto_ver != NVMEIBT_CLIENT_PROTO_VERSION_PREV))
		return NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_MISMATCH;

	NCLIENT_PROTO_ASSERT(u87yhg5,		// Cant be valid before decode because of bit/little endianess
		(len >= (s32)sizeof(*msg)) &&
		!nvmeibt_protocol_client_msg_is_valid(msg->hdr.msg_type), err_msg, len, (int)sizeof(*msg), msg->hdr.msg_type);
	__client_msg_convert_no_payload(msg, decode);
	pl = (struct nvmeibt_client_msg_pl*)msg->thick.data;

	msg_type = msg->hdr.msg_type;
	switch (msg_type) {

	/* Messages without payload */
	case NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK:
	case NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID:
	case NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH:
	case NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK:
		VERIFY_LEN_EQUAL(st673nu, len, sizeof(*msg)); // Ensure we decoded the whole msg
		break;

	/* Messages with mandatory payload */
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_START:
		VERIFY_NO_BUF_OVERRUN(bsh38qn, pl, buf, len);
		__recovery_start_pl_convert(&pl->recov_start, decode);
		break;

	case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT:
		VERIFY_NO_BUF_OVERRUN(cki93ba, pl, buf, len);
		__recovery_taskid_pl_convert(&pl->recov, decode);
		break;

	case NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS:
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH:
		VERIFY_NO_BUF_OVERRUN(l09ijse, &pl->recov_status, buf, len);
		__recovery_status_pl_convert(&pl->recov_status, decode);
		break;

	case NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED: {
		struct nvmeibt_cleaned_stalock_info *rpl = &pl->stalock_info;
		VERIFY_NO_BUF_OVERRUN(dy76au2, rpl, buf, len);
		_cleaned_stalock_convert(rpl, decode);
		break;
	}

	case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE:
	case NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK:
		VERIFY_NO_BUF_OVERRUN(u87sw3e, &pl->lockid_cache_purge, buf, len);
		__lockid_cache_purge_pl_convert(&pl->lockid_cache_purge, decode);
		break;

	case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK:
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY: {
		struct nvmeibt_client_topo_praid *pr1 = (struct nvmeibt_client_topo_praid*)pl;
		struct nvmeibt_client_topo_disk_segment *seg;
		int8_t		i;
		VERIFY_NO_BUF_OVERRUN(asdgt66, pr1, buf, len);
		__topo_raid_conv(pr1, decode);
		seg = (struct nvmeibt_client_topo_disk_segment*)(pr1+1);
		for (i=0; i < pr1->n_segments; i++, seg++) {
			VERIFY_NO_BUF_OVERRUN(ki8ujy6, seg, buf, len);
			__topo_seg_conv(seg, decode);
		}
		VERIFY_LEN_EQUAL(sye7445, len, ((u8*)seg - buf));
		break;
	}

	case NVMEIBT_CLIENT_MSG_CT_FAILED_CMD:
	case NVMEIBT_CLIENT_MSG_CT_DI_DETECTED:
		__failed_cmd_pl_conv(&pl->failed_cmd, decode);
		VERIFY_LEN_EQUAL(dhy7776, len, ((u8*)&pl[1] - buf));
		break;

	case NVMEIBT_CLIENT_MSG_CT_STALE_LOCK:
	case NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK:
		__failed_lock_pl_conv(&pl->failed_lock, decode);
		VERIFY_LEN_EQUAL(wy76yww, len, ((u8*)&pl[1] - buf));
		break;

	case NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY: {
		const enum NVMEIBT_CLIENT_TR_REASON reason = msg->hdr.reason;
		if (reason == NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH){
			__msg_summary_convert(&pl->prior_msg_summary, decode);
			VERIFY_LEN_EQUAL(ffy7644, len, ((u8*)pl + sizeof(pl->prior_msg_summary) - buf));
		} else {
			VERIFY_LEN_EQUAL(fji8nhy, len, sizeof(*msg));
		}
		break;
	}
	/* CDV thin-provisioning messages: payloads are raw structs, no field conversion */
	case NVMEIBT_CLIENT_MSG_RT_CDV_ALLOC_EXTENT:
	case NVMEIBT_CLIENT_MSG_TR_CDV_ALLOC_EXTENT_RSP:
	case NVMEIBT_CLIENT_MSG_RT_CDV_FREE_EXTENT:
	case NVMEIBT_CLIENT_MSG_RT_CDV_LIST_EXTENTS:
	case NVMEIBT_CLIENT_MSG_TR_CDV_LIST_EXTENTS_RSP:
	case NVMEIBT_CLIENT_MSG_TR_CDV_ALLOCATOR_UPDATE:
		break;
	default:
		NCLIENT_PROTO_ASSERT(du87uhy, false, err_msg, len, (int)sizeof(*msg), msg->hdr.msg_type);
		break;
	}
	*decoded_msg = msg;
	return NVMEIBT_CLIENT_MSG_DECODE_OK;

_decode_err:
	return NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_ERROR;
}

struct nvmeibt_client_msg_summary nvmeibt_client_decode_msg_summary(u8 *buf, s32 len){
	struct nvmeibt_client_msg const * const msg = (typeof(msg))buf;
	struct nvmeibt_client_msg_header hdr = msg->hdr;

	NCLIENT_PROTO_ASSERT(t_16_clnt_toma_proto_decode,
						(u32)len >= sizeof(hdr), err_msg, (u32)len, (int)sizeof(hdr), msg->hdr.msg_type);
	__msg_header_conv(&hdr, false);
	return (struct nvmeibt_client_msg_summary) {
		.protocol_version = hdr.protocol_version,
		.msg_type = hdr.msg_type,
		.reason = hdr.reason,
		.data_length = 0
	};
}

/********************* Todo: Delete the functions below ***********************/
void nvmeibt_client_topo_praid_write(struct nvmeibt_client_topo_praid	*pr,
									 const char							*uuid,
									 s32								praid_version,
									 u8									blkset_sync_safety,
									 u8									io_perms,
									 int8_t								n_segments
									)
{
	strcpy_to_msg(pr->uuid,		  uuid);
	pr->praid_version           = praid_version;
	pr->blkset_sync_safety      = blkset_sync_safety;
	pr->reserved_3[0] = 0;		// For compatibility to 1.2
	pr->reserved_3[1] = 0;		// For compatibility to 1.2
	pr->reserved_3[2] = 0;		// For compatibility to 1.2
	pr->io_perms			    = io_perms;
	pr->n_segments              = n_segments;
}

void nvmeibt_client_topo_praid_read(const struct nvmeibt_client_topo_praid 	*pr,
		char *uuid, size_t uuid_size, s32 *praid_version, u8 *blkset_sync_safety,
		u8 *io_perms, int8_t *n_segments)
{
	strcpy_from_msg(uuid,		  pr->uuid, uuid_size);
	*praid_version				= pr->praid_version;
	*blkset_sync_safety			= pr->blkset_sync_safety;
	*io_perms					= pr->io_perms;
	*n_segments					= pr->n_segments;
}

#define is_valid_owner_index(diff_between_owner_and_seg_idx, n_segments, max_n_owners) ((((n_segments) + (diff_between_owner_and_seg_idx)) % (n_segments)) < (max_n_owners))
#define UUID_LEN  (sizeof(((struct nvmeibt_client_topo_disk_segment*)0)->uuid))	// Todo: Remove it, use centralized define
#define ARE_UUIDS_EQ(u1, u2) (!strncmp(u1, u2, UUID_LEN))

void nvmeibt_client_reset_seg_topo_owners(struct nvmeibt_client_topo_disk_segment *seg_topo) {
	memset(seg_topo->owners, 0, sizeof(seg_topo->owners));
}

// Using the owner scheme we decide if the owner is valid or set it to no-owner
static bool is_owner_valid(struct nvmeibt_client_topo_disk_segment praid_segs[], int8_t idx_in_praid,
							  int8_t n_segments, s32 type, int8_t max_n_owners,
							  const char *uuid_of_primary_owner_segment) {
	int8_t										i;
	struct nvmeibt_client_topo_disk_segment		*seg_topo = &(praid_segs[idx_in_praid]);

	for (i = 0; i < n_segments; i++) {
		if (ARE_UUIDS_EQ(praid_segs[i].uuid, uuid_of_primary_owner_segment)) {
			// Found the index of the candidate. Now verify that it is one of the ligitimate owners
			switch (type) {
			case OWNER_SCHEME_FIRST_L_INC_A:
				if (i >= max_n_owners) { // Owner is not valid ignoring all owners
					_NE(error_client_protocol_is_owner_valid,
						"Invalid owner i=@INT > @MAX_N_OWNERS", i, max_n_owners);
					goto set_no_owner;
				}
				break;
			case OWNER_SCHEME_SL_START_INC_A:
				if (!is_valid_owner_index(i - idx_in_praid, n_segments, max_n_owners)) {
					_NE(error_1_client_protocol_is_owner_valid,
						"Invalid owner for idx_in_praid=@IDX_IN_PRAID i=@INT",
						idx_in_praid, i);
					goto set_no_owner;
				}
				break;
			case OWNER_SCHEME_SL_START_DEC_A:
			case OWNER_SCHEME_SL_START_DEC_C:
				if (!is_valid_owner_index(idx_in_praid - i, n_segments, max_n_owners)) {
					_NE(error_2_client_protocol_is_owner_valid,
						"Invalid owner for idx_in_praid=@IDX_IN_PRAID i=@INT "
						"n_segs=@N_SEGS max_n_owners=@MAX_N_OWNERS",
						idx_in_praid, i, n_segments, max_n_owners);
					goto set_no_owner;
				}
				break;
			}
			return true;
		}
	}
set_no_owner:
	seg_topo->owners[0].mode = (char)NVMEIBTC_DS_OWNER_MODE_NO_OWNER;
	seg_topo->owners[0].seg_uuid[0] = 0;
	return false;
}

int8_t get_owner_idx_by_owner_scheme_type(int8_t owner_no, s32 type, int8_t idx_in_praid, int8_t n_segments) {
	switch (type) {
	case OWNER_SCHEME_FIRST_L_INC_A:   /* First L segs can be locked */
		return owner_no;
	case OWNER_SCHEME_SL_START_INC_A:	/* Cyclic i++ after me */
		return (n_segments + idx_in_praid + owner_no) % n_segments;
	case OWNER_SCHEME_SL_START_DEC_A:	/* Cyclic i-- before me */
	case OWNER_SCHEME_SL_START_DEC_C:	/* Cyclic i-- before me */
		return (n_segments + idx_in_praid - owner_no) % n_segments;
	}
	NCLIENT_PROTO_ASSERT(t_00_clnt_toma_proto_get_lock, false, "lock_type=@RV", type);
	return -1;
}

static bool __is_owner_already_set(struct nvmeibt_client_topo_disk_seg_owner *owner,
								  int8_t max_n_owners, const char *uuid) {
	s32 i;
	for (i=0;i<max_n_owners;i++) {
		if (owner[i].seg_uuid[0] == 0) { // Unused entry
			return false;
		}
		if (ARE_UUIDS_EQ(owner[i].seg_uuid, uuid)) {
			return true;
		}
	}
	return false;
}

#define uuid_str_is_valid(_uuid_str) (_uuid_str[0] != '\0')

void nvmeibt_client_topo_disk_segment_upd_owners(struct nvmeibt_client_topo_disk_segment praid_segs[],
												 int8_t n_segments,
												 const char *uuid_of_primary_owner_segment,
												 const char *uuid_of_secondary_owner_segment,
												 s32 type, int8_t max_n_owners, int8_t idx_in_praid)
{
	int8_t										i, n_used_owners = 0;
	int8_t										n_owners = min(max_n_owners, n_segments);
	struct nvmeibt_client_topo_disk_segment     *seg_topo = &(praid_segs[idx_in_praid]);
	const char 									filler_owner_mode = (char)NVMEIBTC_DS_OWNER_MODE_COPY_OWNER;
	NCLIENT_PROTO_ASSERT(t_01_nal, (type == OWNER_SCHEME_SL_START_DEC_C) ,"type=@INT", type); // This is the only supported mode
	max_n_owners = n_owners;
	nvmeibt_client_reset_seg_topo_owners(seg_topo);
	if (uuid_str_is_valid(uuid_of_primary_owner_segment)) {
		if (is_owner_valid(praid_segs, idx_in_praid, n_segments, type, max_n_owners, uuid_of_primary_owner_segment)) {
			strcpy_to_msg(seg_topo->owners[0].seg_uuid, uuid_of_primary_owner_segment);
			seg_topo->owners[0].mode = (char)NVMEIBTC_DS_OWNER_MODE_PRIMARY;
			n_used_owners++;
		} else { // Owner is invalid already set to NO owner
			return;
		}
	} else { // No Owner given, reset ownerss made all ownerss invalid
		return;
	}

	if (uuid_str_is_valid(uuid_of_secondary_owner_segment)) { // Set secondary if given
		strcpy_to_msg(seg_topo->owners[1].seg_uuid, uuid_of_secondary_owner_segment);
		              seg_topo->owners[1].mode = (char)NVMEIBTC_DS_OWNER_MODE_SECONDARY;
		n_used_owners++;
	}

	// Set non primary owners for up to max_n_owners on non-dead segments!
	for (i = 0; (i < n_owners) && (n_used_owners < n_owners); i++) {
		const int8_t owner_idx = get_owner_idx_by_owner_scheme_type(i, type, idx_in_praid, n_segments);
		struct nvmeibt_client_topo_disk_segment *owner_topo = praid_segs + owner_idx;
		struct nvmeibt_client_topo_disk_seg_owner *o = &seg_topo->owners[n_used_owners];
		if (__is_owner_already_set(&seg_topo->owners[0], n_owners, owner_topo->uuid))
			continue;

		if (owner_topo->access_mode == NVMEIBTC_DS_MODE_DEAD) {
			o->mode = (char)NVMEIBTC_DS_OWNER_MODE_NO_OWNER;
			o->seg_uuid[0] = 0;
			max_n_owners--;
		} else {
			o->mode = filler_owner_mode;
			strcpy_to_msg(o->seg_uuid, owner_topo->uuid);
			n_used_owners++;
		}
	}
	NCLIENT_PROTO_ASSERT(djur857, n_used_owners == max_n_owners,
						"n_used_owners=@N_USED_OWNERS max_n_owners=@MAX_N_OWNERS",
						n_used_owners, max_n_owners);
}

void nvmeibt_client_topo_disk_segment_write(struct nvmeibt_client_topo_disk_segment *d,
											const char								*uuid,
											s32										access_mode)
{
	strcpy_to_msg(d->uuid, uuid);
	d->access_mode = access_mode;
}

void nvmeibt_client_topo_disk_segment_read(struct nvmeibt_client_topo_disk_segment *d,
										   char										*uuid,
										   size_t									uuid_size,
										   s32										*access_mode,
										   char										*uuid_of_primary_owner_segment,
										   size_t									uuid_of_primary_owner_segment_size,
										   char										*uuid_of_secondary_owner_segment,
										   size_t									uuid_of_secondary_owner_segment_size
										  )
{
	*access_mode = d->access_mode;
	strcpy_from_msg(uuid,                  d->uuid, uuid_size);
	strcpy_from_msg(uuid_of_primary_owner_segment, d->owners[0].seg_uuid, uuid_of_primary_owner_segment_size);
	if (d->owners[1].mode == NVMEIBTC_DS_OWNER_MODE_SECONDARY) {
		strcpy_from_msg(uuid_of_secondary_owner_segment, d->owners[1].seg_uuid, uuid_of_secondary_owner_segment_size);
	} else {
		strcpy_from_msg(uuid_of_secondary_owner_segment, "", uuid_of_secondary_owner_segment_size);
	}
}

#ifdef UM_APP
	#if (defined(__GNUC__) && (__GNUC__ >= 9)) || defined(__gcc__)
		#pragma GCC diagnostic pop
	#endif
#endif
