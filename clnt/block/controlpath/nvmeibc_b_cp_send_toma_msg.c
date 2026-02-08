#include "block/nvmeibc_block_common.h"
#include "block/nvmeibc_topology.h"
#include "nvmeibc_icore_ops.h"
//#include "nvmeibc_b_cp_send_toma_msg.h"

/* Fill global fields with sensible values */
static inline void __prepare_toma_thick_msg(struct nvmeibc_disk_segment *seg,
		struct nvmeibc_raid1* r1, struct nvmeibt_client_msg *msg,
		enum NVMEIBT_CLIENT_MSG_TYPES msg_type,
		enum NVMEIBT_CLIENT_TR_REASON reason, bool never_reged_on_seg,
		struct nvmeibt_client_msg_pl *pl)
{
	const struct nvmeibc_topology *t = seg->chunk->topology;
	const int praid_version = nvmeibc_seg_on_active_get_version_send(r1, seg, msg_type);
	const u32 pl_size = (pl) ? sizeof(*pl) : 0;
	nvmeibt_client_thick_msg_write(msg, msg_type, reason,
		nvmeib_get_utsname_nodename(), seg->toma_reg->protocol_version, 0x35003500, /*hdr*/
		t->configuration_version, t->topology_version, praid_version, /*thick*/
		seg->uuid, r1->lid.all, t->nt->reservation_version_max_seen, r1->toma.conversation_ind, /*thick*/
		never_reged_on_seg, nvmeibc_block_is_hidden(t->nt->nd), pl_size, pl, nvmeib_get_guid());
}

#define TOMA_HDR_SIZE sizeof(struct nvmeibt_client_msg)
static int __send_combined_msg(struct nvmeibc_disk_segment *seg,
	enum NVMEIBT_CLIENT_MSG_TYPES mtype, bool never_reged_on_seg,
	enum NVMEIBT_CLIENT_TR_REASON reason, struct nvmeibt_client_msg_pl *tpl,
	struct nvmeibs_lost_srv_resource_payload *spl)
{
	const u16 toma_payload_size = (tpl ? sizeof(*tpl) : 0);
	const u16 srvr_payload_size = (spl ? sizeof(*spl) : 0);
	const u32 max_size = TOMA_HDR_SIZE + sizeof(*tpl) + sizeof(*spl);
	struct nvmeibc_disk_toma_send_params env = { .buf = kmalloc(max_size, GFP_ATOMIC),
		.len_toma = TOMA_HDR_SIZE + toma_payload_size,
		.len_srvr = TOMA_HDR_SIZE + toma_payload_size + srvr_payload_size };
	struct nvmeibt_client_msg *msg = NULL;
	const struct nvmeibc_subscription_ctx *tr = seg->toma_reg;
	struct nvmeibc_raid1* r1;
	struct nvmeibc_topologies *nt;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	int rv;

	BUILD_BUG_ON(((TOMA_HDR_SIZE&0x7)!=4)||(max_size > NVMEIB_TOMA_REQ_MAX_LEN));

	if (!env.buf) {
		_NW(trace_0_b_cp_send_toma_msg_send_combined_msg, DMESG_PREFIX() ": Memory allocation failure");
		rv = -ENOMEM;
		goto out;
	}
	msg = (void*)env.buf;

	if (!is_toma_reg_valid(tr)) {
		rv = 0;	/* Probably we are closing all conenctions right now */
		goto out;
	}
	memset(env.buf, 0, env.len_srvr);	// Memset only the needed size
	r1 = nvmeibc_disk_segment_get_praid(seg);
	nt = tr->nt;						// Also can take nvmeibc_disk_seg_to_bdev(seg)->nt which is the same
	{
		__prepare_toma_thick_msg(seg, r1, msg, mtype, reason, never_reged_on_seg, tpl);
		if (srvr_payload_size) {
			memcpy(&env.buf[env.len_toma], spl, srvr_payload_size);
		}
	}
	msg->hdr.cookie = nvmeib_get_guid();
	rv = icore_ops->toma_send(icore_ops, seg->disk, seg->toma_reg->handle, &env);
	_NITR(t1_c2t_send, "sent=@BOOL_YN, @PROTOCOL_CLIENT_MSG_STR(@PROTOCOL_CLIENT_MSG_REASON_STR), uuid=@SEG_DBG_UUID, @C_PRV cookie=@COOKIE, "
					   "c_lid=@C_LID, cnt_@RES_MOD_VER, conv_id=@CLNT_TOMA_PR_CONVER_IND len={t=@X/s=@X} rv=@RV",
	   (rv == 0), nvmeibt_protocol_client_msg_str(mtype), nvmeibt_protocol_client_msg_reason_str(reason),
	   seg->dbg_uuid, r1->version, msg->hdr.cookie,
	   r1->lid.all, nt->reservation_version_max_seen, r1->toma.conversation_ind, env.len_toma, env.len_srvr, rv);
out:
	kfree(msg);
	return rv;
}

int nvmeibc_toma_send_msg(struct nvmeibc_disk_segment *seg, enum NVMEIBT_CLIENT_MSG_TYPES msg_type, enum NVMEIBT_CLIENT_TR_REASON reason, struct nvmeibt_client_msg_pl *tpl, bool never_reged_on_seg, struct nvmeibs_lost_srv_resource_payload *spl)
{
	return __send_combined_msg(seg, msg_type, never_reged_on_seg, reason, tpl, spl);
}

int nvmeibc_toma_send_direct_msg(struct nvmeibc_disk_segment *seg, enum NVMEIBT_CLIENT_MSG_TYPES msg_type, struct nvmeibt_client_msg_pl *tpl)
{
	return __send_combined_msg(seg, msg_type, false /* Unused */, NVMEIBT_CLIENT_RT_REASON_DIRECT, tpl, NULL);
}

#define DECLARE_PAYLOAD_ONSTACK(pl) struct nvmeibt_client_msg_pl pl = {{{0}}}

/***** Complaining to Toma about commands (failed IO completions to disk ******/
int __send_toma_cmd_help(const struct nvmeibc_d_iocmd_comp *comp,
						 const u32 failed, const u32 fixed)
{
	DECLARE_PAYLOAD_ONSTACK(pl);
	int rv;
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(comp);
	struct nvmeibc_disk_segment *seg = cmd->ds;
	BUG_ON(fixed & failed);
	pl.failed_cmd.error_code = comp->comp_code;
	pl.failed_cmd.offset     = cmd->iocmd->reqs1.disk_address;
	pl.failed_cmd.op         = cmd->o->op;
	pl.failed_cmd.fix_map    = fixed;
	pl.failed_cmd.fail_map   = failed;
	rv = nvmeibc_toma_send_direct_msg(seg, NVMEIBT_CLIENT_MSG_CT_FAILED_CMD,&pl);
	return rv;
}

int __send_toma_di_help(u64 addr_on_disk, struct nvmeibc_disk_segment *seg)
{
	DECLARE_PAYLOAD_ONSTACK(pl);
	pl.failed_cmd.error_code = (u16)(~0);
	pl.failed_cmd.offset     = addr_on_disk;
	pl.failed_cmd.op         = 0;	// Todo: use
	return nvmeibc_toma_send_direct_msg(seg, NVMEIBT_CLIENT_MSG_CT_DI_DETECTED, &pl);
}

/******************* Complaining to Toma about locks **************************/
static inline enum NVMEIBT_CLIENT_LOCK_OP __lock_op_translate_to_toma(enum nvmeibc_disk_locks_opr comp_opr)
{
	enum NVMEIBT_CLIENT_LOCK_OP rv = NVMEIBT_CLIENT_LOCK_ILLEGAL;
	switch (comp_opr) {
	case NVMEIBC_LOCK_CMP_AND_SWAP:		 rv = NVMEIBT_CLIENT_LOCK_XCHG; break;
	case NVMEIBC_LOCK_READ:				 rv = NVMEIBT_CLIENT_LOCK_RD;   break;
	case NVMEIBC_LOCK_BLKSET_INFO_READ:  rv = NVMEIBT_CLIENT_BIF_R;	    break;
	case NVMEIBC_LOCK_BLKSET_INFO_WRITE: rv = NVMEIBT_CLIENT_BIF_W;	    break;
	default:
		_NE_to_user(t_00_lottt, DMESG_PREFIX(), "Internal error in communication with the TOMA that is not expected to happen, contact Excelero support. Error code: 1001. Lock operation: @COMP_OPR.", comp_opr); break;
	}
	return rv;
}

static inline enum NVMEIBT_CLIENT_MSG_TYPES __lock_op_get_msg_type(const union nvmeib_lock_blkset_entry *lid, const struct nvmeibc_cmd_lock *l)
{
	return (nvmeibc_sync_is_stale(l, lid->all)) ? NVMEIBT_CLIENT_MSG_CT_STALE_LOCK : NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK;
}

int __send_toma_lock_help(const struct nvmeibc_cmd_lock *l, struct nvmeibc_disk_segment *seg)
{
	DECLARE_PAYLOAD_ONSTACK(pl);
	int rv;
	const struct nvmeibc_d_rdma_comp *dc = &l->comp;
	const union nvmeib_lock_blkset_entry *lid = (void*)&get_contending_id(dc);
	enum NVMEIBT_CLIENT_LOCK_OP lock_op = __lock_op_translate_to_toma(dc->opr);
	enum NVMEIBT_CLIENT_MSG_TYPES msg_type = __lock_op_get_msg_type(lid, l);
	struct nvmeibt_client_failed_lock_pl *fl = &pl.failed_lock;
	BUG_ON(!lid->all); /* EC-4937: Toma help with problem = 0 shall never happen */
	fl->lock_op =         lock_op;
	fl->status =          l->status;
	fl->is_problem_here = (seg == l->ds); // can send: l->ds->toma_reg->handle
	fl->disk_blkno_4k =   l->address;
	fl->curr =            (u64)lid->all;
	fl->comp =            dc->compare;
	fl->xchg =            dc->exchange;
	fl->num_retries =     l->retries;
	strlcpy(fl->problematic_seg_uuid_str, l->ds->uuid, sizeof(fl->problematic_seg_uuid_str));
	_NT(trace_b_cp_send_toma_msg_send_toma_lock_help, "Help request {seg=@SEGMENT_UUID, @DLBA, problem=@LOCK_ENT_U64}, send to seg=@SEGMENT_UUID, problem=@LOCKID", l->ds->uuid, l->address, lid->all, seg->uuid, lid->lock_id.all);
	rv = nvmeibc_toma_send_direct_msg(seg, msg_type, &pl);
	return rv;
}
