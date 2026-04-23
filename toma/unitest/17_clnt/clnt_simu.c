/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "clnt_simu.h"
#include "nvmeibt_debug.h"	// Binary tracing
#include "../13_mgmt/mongodb_simu.h"
#include "common/nvmeib_shared.h"
#include "../15_server/sandbox_nvmeibs_toma.h"
#include "clnt/nvmeibt_client_protocol.h"

struct clnt_simu *clnt_simu_create(struct sb_cluster_conf *cfg, int node_idx) {
	struct clnt_simu *C = calloc(1, sizeof(*C));
	C->cfg = cfg;
	cfg->nodes[node_idx].clnt = C;
	C->node_idx = node_idx;
	C->unique_msg_to_toma_counter = (0xC0CU + ((unsigned)node_idx << 4)) << 20;		// Clnt 2 = 0xC2C_____ 2^20 values
	return C;
}

void clnt_simu_destroy(struct sb_cluster_conf *cfg, int node_idx) {
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	C->cfg->nodes[C->node_idx].clnt = NULL;
	free(C);
}

struct clnt_simu *clnt_simu_get_local_clnt(struct sb_cluster_conf *cfg) { return cfg->live->clnt; }

#define BASE_CID 0xc1dc1d00
static u64 __get_clnt_handle_for_seg(const struct clnt_simu *clnt, uint32_t seg_uuid) {	// Emulation of toma_conn_handle_ib_2_proc()
	const u32 cid = BASE_CID | (u32)clnt->node_idx;			// CID (client id) is unique per client
	return ((u64)cid << 32) | seg_uuid;						// Unique per-client-per-segment, Upper 32 bits = CID for lookup by Toma
}

static struct clnt_simu *__get_clnt_and_seg_from_handle(u64 handle, uint32_t *seg_uuid) {
	const uint32_t cid = (handle >> 32);
	const uint32_t node_idx = cid & 0xf;
	*seg_uuid = (uint32_t)(handle & ~0U);
	BUG_ON(cid != (BASE_CID + node_idx));					// Malformed handle received from Toma
	return sb_cluster_get_const_conf()->nodes[node_idx].clnt;
}

static void __subscribe_to_disk(struct clnt_simu *clnt, struct sb_disk_conf *disk, bool do_subscribe) {
	const struct sb_node_conf *node = &clnt->cfg->nodes[clnt->node_idx];
	const u64 handle = __get_clnt_handle_for_seg(clnt, 0);							// Todo: Why seg is 0
	N_Tf(__AUTOID__, "Clnt: subscribe=@BOOL_YN handle=@HANDLE, disk_uuid=@X", do_subscribe, handle, disk->uuid);
	if (do_subscribe) {
		if (++disk->clnts[clnt->node_idx].n_ref == 1) {
			nvmeibs_simu_subscribe_client(handle, node->hostname, disk, true);		// Otherwise, Already subscribed to this disk via another segment
		}
	} else {
		if (--disk->clnts[clnt->node_idx].n_ref == 0)
			nvmeibs_simu_subscribe_client(handle, node->hostname, disk, false);		// Otherwise, Still   subscribed to this disk via another segment
	}
}

static void __subscribe_to_vol_disks_of_live_toma(struct clnt_simu *clnt, const struct sb_volume_conf *vol, bool do_subscribe) {
	const struct sb_cluster_conf *cfg = clnt->cfg;
	topo_declare_iterator(c, r, seg, ci, ri, si);
	topo_for_each_live_toma_seg(vol, c, ci, r, ri, seg, si) {
		struct sb_disk_conf *disk = &cfg->live->disks[sb_cluster_get_disk_idx_from_disk_uuid_n(seg->disk_uuid)];
		__subscribe_to_disk(clnt, disk, do_subscribe);			// Already subscribed to this disk via another segment
	}
}

void clnt_simu_vol_attach(struct sb_cluster_conf *cfg, int node_idx, int vol_idx) {
	struct sb_volume_conf *vol = &cfg->vols[vol_idx];
	struct clnt_simu *clnt = cfg->nodes[node_idx].clnt;
	struct sb_attachment_info *attach_info = &vol->clnts[node_idx];
	N_Tf(__AUTOID__, "Clnt[@INT] attach_@DEV_NAME", clnt->node_idx, vol->name);
	BUG_ON(attach_info->attachment_version != 0);					// Already attached!
	__subscribe_to_vol_disks_of_live_toma(clnt, vol, true);
	// Report to mongo-db directly
	attach_info->attachment_version = 15;							// Todo: need to receive this from mgmt
	attach_info->ioEnabled = false;
}

void clnt_simu_vol_detach(struct sb_cluster_conf *cfg, int node_idx, int vol_idx) {
	struct sb_volume_conf *vol = &cfg->vols[vol_idx];
	struct clnt_simu *clnt = cfg->nodes[node_idx].clnt;
	struct sb_attachment_info *attach_info = &vol->clnts[node_idx];
	N_Tf(__AUTOID__, "Clnt[@INT] detach_@DEV_NAME", clnt->node_idx, vol->name);
	BUG_ON(attach_info->attachment_version == 0);					// Not attached!
	__subscribe_to_vol_disks_of_live_toma(clnt, vol, false);
	memset(attach_info, 0, sizeof(*attach_info));					// Report to mongo-db directly
}

bool clnt_simu_vol_is_ioable(int node_idx, int vol_idx) {
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	//struct clnt_simu *clnt = cfg->nodes[node_idx].clnt;
	const struct sb_volume_conf *vol = &cfg->vols[vol_idx];
	topo_declare_iterator(c, r, seg, ci, ri, si);
	topo_for_each_live_toma_seg(vol, c, ci, r, ri, seg, si) {
		const struct clnt_praid_reg_ctx *reg = &vol->clnts[node_idx].chunks[ci].raids[ri];
		if ((reg->is_seg_registered_bmp & (1<<si)) == 0)		// This seg is missing
			return false;
	}
	return true;
}

static void nvmeibc_raid1_clear_new_topo_upon_unreg(struct clnt_praid_reg_ctx *reg) {	// Emulation of clients function with the same name
	BUG_ON(reg->is_seg_registered_bmp);	// Still registered to at least 1 segment!
	reg->conversation_id++;
	reg->version_major = 0;			// Forget the topology, much like real client does
	reg->lock_id = 0;				// All the acquired locks are abandoned and become stale
}

static void __send_register_message(struct clnt_simu *clnt, bool do_reg, unsigned vi, unsigned ci, unsigned ri, unsigned si) {
	struct sb_cluster_conf *cfg = clnt->cfg;
	const struct sb_node_conf *node = &clnt->cfg->nodes[clnt->node_idx];
	const struct sb_volume_conf *vol = &cfg->vols[vi];
	const struct sb_seg_conf *seg = &vol->chunks[ci].raids[ri].segs[si];
	const u64 handle = __get_clnt_handle_for_seg(clnt, seg->uuid);
	struct sb_attachment_info *attach_info = &cfg->vols[vi].clnts[clnt->node_idx];
	struct clnt_praid_reg_ctx *reg = &attach_info->chunks[ci].raids[ri];
	struct nvmeibt_client_msg *m = calloc(1, sizeof(*m));	// Emulate __prepare_toma_thick_msg()
	struct nvmeibt_client_msg_pl *pl = NULL;		// Not supported yet
	const enum NVMEIBT_CLIENT_MSG_TYPES msg_type = (do_reg ? NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT : NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT);
	const int topology_version = 0;					// Unused
	const bool is_recovery_clnt = false;			// Todo: Also support recovery client
	const bool never_reged_on_seg = !(reg->is_seg_registered_bmp & (1<<si));
	char seg_uuid_str[NVMEIB_GID_STR_MAX];
	snprintf(seg_uuid_str, sizeof(seg_uuid_str), UUID_from_U32, seg->uuid);
	N_Tf(__AUTOID__, "Clnt: send @STR seg=@X handle=@HANDLE, lock=@LOCKID, num_stale_locks=@INT", nvmeibt_protocol_client_msg_str(msg_type), seg->uuid, handle, reg->lock_id, reg->n_ios);
	nvmeibt_client_thick_msg_write(m, msg_type, NVMEIBT_CLIENT_RT_REASON_DIRECT,
		node->hostname,	NVMEIBT_CLIENT_PROTO_VERSION, 0x35003500,
		vol->conf_version, topology_version, reg->version_major,
		seg_uuid_str, reg->lock_id, attach_info->reserv.version, reg->conversation_id,
		never_reged_on_seg,	is_recovery_clnt, (pl ? sizeof(*pl) : 0), pl, ++clnt->unique_msg_to_toma_counter);
	nvmeibs_simu_send_clnts_msg(handle, m);
	if (do_reg) {
	} else {
		BUG_ON(reg->lock_id == 0);		// Why is the client unregistering??? it has no lock-id.
		reg->is_seg_registered_bmp &= ~(1 << si);
		nvmeibc_raid1_clear_new_topo_upon_unreg(reg);		// Todo: Call this only after last live seg was unregistered
	}
}

void clnt_simu_vol_register(struct sb_cluster_conf *cfg, int node_idx, int vi, bool do_reg) {
	struct clnt_simu *clnt = cfg->nodes[node_idx].clnt;
	const struct sb_volume_conf *vol = &cfg->vols[vi];
	topo_declare_iterator(c, r, seg, ci, ri, si);
	topo_for_each_live_toma_seg(vol, c, ci, r, ri, seg, si) {
		__send_register_message(clnt, do_reg, vi, ci, ri, si);
	}
}

#include "../10_local_hw/nvme_disk_simu.h"
static void __simulate_io_to_disk(struct clnt_simu *C, struct sandbox_nvme_device *disk, u32 dlba_blockset) {
	struct sb_attachment_info *attach_info = &C->cfg->vols[0].clnts[C->node_idx];
	union nvmeib_lock_blkset_entry *ptr = &((union nvmeib_lock_blkset_entry *)disk->ram.addr)[dlba_blockset];
	struct clnt_praid_reg_ctx *reg = &attach_info->chunks[0].raids[0];		// Todo: properly extract from registered client
	BUG_ON((disk->ram.addr == NULL) || (disk->ram.len <= dlba_blockset));
	ptr->lock_id.all = reg->lock_id;
	ptr->blkset_info.bits.dirty = 0;		// Todo: inject dbits
	ptr->blkset_info.bits.txid++;
	reg->n_ios++;
}

void clnt_simu_vol_lock_blockset_v(struct sb_cluster_conf *cfg, int node_idx, int vol_idx, u32 vlba_blockset) {
	const struct sb_attachment_info *A = &cfg->vols[vol_idx].clnts[node_idx];
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	struct sandbox_nvme_device *disk = NULL;	// Todo: calculate from config
	const u32 dlba_blockset = vlba_blockset+5;	// Todo: calculate vlba->dlba from config
	BUG_ON(!A->ioEnabled);
	__simulate_io_to_disk(C, disk, dlba_blockset);
}

void clnt_simu_vol_lock_blockset_d(struct sb_cluster_conf *cfg, int node_idx, int disk_idx, u32 dlba_blockset) {
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	struct sandbox_nvme_device *disk = cfg->live->disks[disk_idx].local_nvme;
	__simulate_io_to_disk(C, disk, dlba_blockset);
}

static void __toma_segment_register_succeed(struct clnt_simu *clnt, uint32_t seg_uuid) {
	unsigned vi, ci, ri, si;
	struct clnt_praid_reg_ctx *reg;
	sb_cluster_get_seg_idx_from_uuid_n(seg_uuid, &vi, &ci, &ri, &si);
	reg = &clnt->cfg->vols[vi].clnts[clnt->node_idx].chunks[ci].raids[ri];
	reg->is_seg_registered_bmp |= (1 << si);
	BUG_ON(reg->lock_id == 0);		// Invalid lock id
}

static void __toma_update_raid1(struct clnt_simu *clnt, uint32_t seg_uuid, const struct nvmeibt_client_msg *pl) {	// Emulation of clients function
	unsigned vi, ci, ri, si;
	const struct nvmeibt_client_topo_praid *praid_t = (typeof(praid_t))&pl->thick.data;
	//const struct nvmeibt_client_topo_disk_segment *segs_t =  (typeof(segs_t ))praid_t->segs;
	sb_cluster_get_seg_idx_from_uuid_n(seg_uuid, &vi, &ci, &ri, &si);
	{	// Update 'reg' context from Toma info
		const struct sb_praid_conf *priad_conf = &clnt->cfg->vols[vi].chunks[ci].raids[ri];
		struct sb_attachment_info *attach_info = &clnt->cfg->vols[vi].clnts[clnt->node_idx];
		struct clnt_praid_reg_ctx *reg = &attach_info->chunks[ci].raids[ri];
		const union io_perms_bitfield io_perm = { .all = praid_t->io_perms };
		reg->lock_id = pl->thick.lock_id;
		reg->version_major = praid_t->praid_version;
		BUG_ON((priad_conf->D + priad_conf->P) != (unsigned)praid_t->n_segments);
		BUG_ON(!io_perm.bits.is_io_R);					// Todo: Support degraded mode that this segment is not readable/writable and hidden recovery
		// for (int i = 0; i < praid_t->n_segments; i++) {	segs_t[i].access_mode; }	// No need to parse access mode and such because we already know it
	}
	__send_register_message(clnt, true, vi, ci, ri, si);
}

static void __block_toma_msg_handler(u64 handle, u8 *buf, int len) {		// Emulates real function with the same name
	uint32_t seg_uuid;
	struct clnt_simu *clnt = __get_clnt_and_seg_from_handle(handle, &seg_uuid);
	struct nvmeibt_client_msg *pl;
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type;
	BUG_ON(nvmeibt_client_decode_new(buf, len, &pl) != NVMEIBT_CLIENT_MSG_DECODE_OK);
	BUG_ON(pl->hdr.protocol_version != NVMEIBT_CLIENT_PROTO_VERSION);
	msg_type = pl->hdr.msg_type;
	N_Tf(__AUTOID__, "Clnt: msg=@STR(@STR), cookie=@COOKIE, h=@HANDLE buf=@BUF len=@LEN @T_PRV toma_lid=@T_LID toma_@RES_MOD_VER, cfg=@CFG, conv=@CLNT_TOMA_PR_CONVER_IND", nvmeibt_protocol_client_msg_str(msg_type), nvmeibt_protocol_client_msg_reason_str(pl->hdr.reason), pl->hdr.cookie, handle, buf, len, pl->thick.praid_version, pl->thick.lock_id, pl->thick.reservation_mode_version, pl->hdr.volume_config_version, pl->thick.conversation_ind);
	switch (msg_type) {
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK:  __toma_segment_register_succeed(clnt, seg_uuid); return;
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK: { BUG_ON(true); return; /* Unsupportd yet */	}
	case NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT:
	case NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK: __toma_update_raid1(clnt, seg_uuid, pl); return;
	case NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT:		{ BUG_ON(true); return; /* Unsupportd yet */	}
	case NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY:		{ BUG_ON(true); return; /* Unsupportd yet */	}
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_START:
	case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE:			{ BUG_ON(true); return; /* Unsupportd yet */	}
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT:
	case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING:				{ BUG_ON(true); return; /* Unsupportd yet */	}
	case NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH:				{ BUG_ON(true); return; /* Unsupportd yet */	}
	case NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY:								return; 	// Do nothing, consider recording it and if Toma does not answer later with REGISTRABLE fire a bug on
	case NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID:
	case NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED:
	default:	/* Unsupported messages */					{ BUG_ON(true); return; /* Unsupportd yet */	}
	}
}

void clnt_simu_receive_msg_from_toma(const struct nvmeibs_toma_client_proc_buf *msg, int len) {
	__block_toma_msg_handler(msg->handle, (u8*)&msg->data, len - sizeof(msg->handle));
}