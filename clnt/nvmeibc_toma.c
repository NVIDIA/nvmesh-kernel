#define C_TOMA_C

/* Toma client side interface - extension of IB admin channel */
#include "nvmeibc_toma.h"
#include "nvmeibc_disk.h"
#include "nvmeib_utils.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_main.h"
#include "nvmeib_srq.h"
#include "nvmeibc_trace.h"
#include "kr_undef.h"
#include "vex/nvmeibc_vex_shared.h"
#include "nvmeibc_memmgr_metrics.h"

#define __NFIN _ND(__AUTOID__, "--> @BASE_NAME:@RANIC_GUID\n", ch->base.base.name, ch->ranic_guid)
#define __NFOUT _ND(__AUTOID__, "<-- @BASE_NAME:@RANIC_GUID\n", ch->base.base.name, ch->ranic_guid)
#define __NFIND NFINS(disk->name)
#define __NFOUTD NFOUTS(disk->name)

#define NVMEIBC_TOMA_DEBUG	0

NVMEIBC_MEMMGR_METRIC(toma_recv_msg, "component=client.toma.recv_msg");

/* Client --> Server (--> Toma) */

struct prp_toma_req_info {
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_disk_toma_cmd *toma_cmd;
};

/* WAS: encode_toma_req_base */
VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_clnt_toma_req_clnt_base_encode)
{
	struct volume_client_toma_req_base *toma_req = wire_buf;
	struct nvmeibc_disk_toma_cmd *toma_cmd = arg;
	ssize_t rv;

	BUG_ON((const void *)(toma_req + 1) > wire_buf_end);

	toma_req->cmd_type = toma_cmd->type;
	toma_req->handle = cpu_to_be64(toma_cmd->handle);

	/* toma req - cmd specific */
	if (toma_cmd->type == NVMEIB_TOMA_CMD_SEND &&
			toma_cmd->send_params->buf && toma_cmd->send_params->len_toma) {
		nvmeib_container_raw_payload_init(&toma_req->data_ctnr,
			NVMEIB_TOMA_REQ_DATA_MAGIC, ops->vex_ext, toma_cmd->send_params->len_toma);
		vex_memcpy(NVMEIB_CONT_PAYLOAD(&toma_req->data_ctnr), toma_cmd->send_params->buf,
				toma_cmd->send_params->len_toma);
		_NT( vex_ach_clnt_toma_req_clnt_base_encode_t1,
            "Encoding TOMA request data, len @INT bytes:",
			toma_cmd->send_params->len_toma);

#if NVMEIBC_TOMA_DEBUG
		/* Dumping only send_params->len since the whole req was zeroed. */
		_NI(vex_ach_clnt_toma_req_clnt_base_encode_i1,
            "Dump send toma send cmd, len @INT bytes:",
			toma_cmd->send_params->len_toma);
		_Dbuf(toma_req->data, toma_cmd->send_params->len_toma);
#endif
		rv = sizeof(*toma_req) + toma_cmd->send_params->len_toma;
		goto out;
	}

	nvmeib_container_empty_init(&toma_req->data_ctnr);

	rv = sizeof(*toma_req);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_clnt_toma_req_clnt_ext1_encode)
{
	struct volume_client_toma_req_ext1 *toma_req = wire_buf;
	struct nvmeibc_disk_toma_cmd *toma_cmd = arg;
	ssize_t rv;
	size_t pb_len;

	if (toma_cmd->type == NVMEIB_TOMA_CMD_SEND &&
		toma_cmd->send_params->buf &&
		(pb_len = toma_cmd->send_params->len_srvr - toma_cmd->send_params->len_toma) > 0) {
		nvmeib_container_raw_payload_init(&toma_req->srv_pb_ctnr,
			NVMEIB_TOMA_REQ_PB_MAGIC, ops->vex_ext, pb_len);
		vex_memcpy(NVMEIB_CONT_PAYLOAD(&toma_req->srv_pb_ctnr),
			   toma_cmd->send_params->buf + toma_cmd->send_params->len_toma,
				pb_len);
		_NT(vex_ach_clnt_toma_req_clnt_ext1_encode_t1,
            "Encoding TOMA Piggyback data, len @ZU bytes:", pb_len);
		rv = sizeof(*toma_req) + pb_len;
		goto out;
	}
	nvmeib_container_empty_init(&toma_req->srv_pb_ctnr);
	rv = sizeof(*toma_req);

out:
	return rv;
}

static ssize_t prp_toma_req(void *p, void *buf, const void *buf_end)
{
	int rv;
	struct prp_toma_req_info *info = p;
	struct nvmeibc_ib_admin_channel *ch = info->ch;
	struct volume_client_req *req;
	const struct vex_ops *toma_req_ops = ch->base.vex_ops[vex_ach_clnt_toma_req];

	__NFIN;
	/* init the command */
	req = buf;
	memset(req, 0, sizeof(*req));
	req->hdr.opcode = NVMEIB_TOMA_REQ;
	req->hdr.tag = atomic_inc_return(&ch->base.base.tag);
	req->version_tag = cpu_to_be16(toma_req_ops->vex_ext);
	_ND(trace_toma_prp_toma_req, "req tag @TAG", req->hdr.tag);

	rv = CALL_VEX_OP(encode,
				vex_ach_clnt_toma_req_clnt_ops, ONE_EXT,
					base, vex_ach_clnt_toma_req_clnt_base_encode,
					ext1, vex_ach_clnt_toma_req_clnt_ext1_encode,
						toma_req_ops,
						NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(req), buf_end, info->toma_cmd);
	if (rv < 0)
		goto out;

	rv += NVMEIBC_VOLUME_CLIENT_REQ_HDR_SIZE();

out:
	__NFOUT;
	return rv;
}

int nvmeibc_toma_send_req(struct nvmeibc_ib_admin_channel *ch,
						  struct nvmeibc_disk_toma_cmd *toma_cmd)
{
	struct volume_client_req *req;
	struct volume_server_rsp *rsp;
	struct nvmeib_iu *recv_ioctx = NULL;
	u8 cmd_type = toma_cmd->type;
	struct prp_toma_req_info info = {ch, toma_cmd};
	int rv = -ENOENT;
	struct nvmeib_iu *send_ioctx_toma = NULL;

	__NFIN;

	_ND(trace_toma_nvmeibc_toma_send_req, "--- Sending NVMEIB_TOMA_REQ message (cmd @NVMEIB_TOMA_CMD_STR) to server @BASE_NAME",
		nvmeib_toma_cmd_str(cmd_type), ch->base.base.name);

	if ((cmd_type == NVMEIB_TOMA_CMD_SEND) &&
		(toma_cmd->send_params->len_srvr > NVMEIB_TOMA_REQ_MAX_LEN)) {
		_NE(error_toma_nvmeibc_toma_send_req, "toma request too long (@LEN_SRVR)", toma_cmd->send_params->len_srvr);
		goto out;
	}

	if (!(send_ioctx_toma = nvmeibc_ib_admin_channel_get_tx_iu(ch))) {
		_NE(error_1_toma_nvmeibc_toma_send_req, "No free messages for administrator to use");
		goto out;
	}

	req = send_ioctx_toma->buf;

	/* send and wait for send completion */
	if ((rv = nvmeibc_ib_admin_channel_prepare_n_send_msg(
		ch, prp_toma_req, &info, NVMEIB_TOMA_SEND_REQ, send_ioctx_toma)) < 0) {
		_NT(trace_1_toma_nvmeibc_toma_send_req, "Failed sending TOMA request to server");
		goto free_iu;
	}

	/* invoke send-completion callback */
	if (cmd_type == NVMEIB_TOMA_CMD_SEND) {
		if (toma_cmd->send_params->send_comp_cb) {
			toma_cmd->send_params->send_comp_cb(toma_cmd->send_params->arg);
		}
	}

	/* wait for the response */
	if ((recv_ioctx = nvmeibc_ib_admin_channel_pending_iu(ch, req->hdr.tag))) {
		rsp = recv_ioctx->buf;
		if (rsp->hdr.tag != req->hdr.tag ||
			rsp->opcode != NVMEIBS_RSP_TOMA_OPCODE_OK) {
			_NE(error_2_toma_nvmeibc_toma_send_req, "Toma response error, tag rsp @TAG vs. req @TAG, rsp->opcode = "
			   "@OPCODE", rsp->hdr.tag, req->hdr.tag, rsp->opcode);
			rv = -EINVAL;
			goto post_recv;
		}
	}
	else {
		_NT(trace_2_toma_nvmeibc_toma_send_req, "Failed to wait for response");
		rv = -ETIMEDOUT;
		goto free_iu;
	}

	_ND(trace_3_toma_nvmeibc_toma_send_req, "--- Received NVMEIB_TOMA_RSP message "
		"from host @BASE_NAME was: @OPT_NOT ok", ch->base.base.name, rv ? "not " : "");

	/* invoke receive response callback */
	if (cmd_type == NVMEIB_TOMA_CMD_SEND) {
		if (toma_cmd->send_params->recv_rsp_cb)
			toma_cmd->send_params->recv_rsp_cb(toma_cmd->send_params->arg, NULL);
	}

post_recv:
	nvmeibc_ib_admin_channel_put_rx_iu(ch, recv_ioctx);

free_iu:
	nvmeibc_ib_admin_channel_put_tx_iu(ch, send_ioctx_toma);

out:

__NFOUT;
	return rv;
}

/*
 * Client <-- Server (<-- Toma)
 */
/**
 * Process toma request ONLY once its have been fully assembled
 * into the admin channel's toma_recv_msg.
 */
static void toma_process_req(struct nvmeibc_ib_admin_channel *ch)
{
#if NVMEIBC_TOMA_DEBUG
	_NI( toma_process_req_i1,
		"Dump recv toma message of length @INT bytes:", ch->toma.recv_msg.len);
	_Dbuf(ch->toma.recv_msg.buf, ch->toma.recv_msg.len);
#endif

	/* forward to ib admin channel towards disk */
	nvmeibc_ib_admin_recv_toma_cmd(ch);

	_NT(trace_toma_toma_process_req, "Toma recv request processing done (returned from recv callback)");
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_toma_clnt_req_clnt_base_decode)
{
	ssize_t rv = -1;
	struct nvmeibc_toma_recv_msg *toma_recv_msg = arg;
	const struct volume_server_toma_req_base *toma_req = wire_buf;
	u16 req_len;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*toma_req) > wire_buf_end);

	if (!toma_recv_msg->buf) {
		_NW(vex_ach_toma_clnt_req_clnt_base_decode_t1,
			"No reassemble buffer");
		rv = -ENOMEM;
		goto out;
	}

	/* checks */
	if ((toma_req->position == NVMEIB_TOMA_REQ_FIRST) ||
		(toma_req->position == NVMEIB_TOMA_REQ_STANDALONE)) {
		if (toma_recv_msg->len != 0) {
			struct nvmeibc_ib_admin_channel *ch = container_of(toma_recv_msg, struct nvmeibc_ib_admin_channel, toma.recv_msg);
			_NE(error_toma_toma_recv_req_base, "@BASE_NAME (@CH_PTR) - Received @POSITION_STR toma req chunk while toma accumulation buf, reset buf",
			   ch->base.base.name, ch, (toma_req->position == NVMEIB_TOMA_REQ_FIRST) ? "first" : "standalone");
		}
		/* initialize toma recv msg */
		toma_recv_msg->handle = be64_to_cpu(toma_req->handle);
		toma_recv_msg->len = 0;
	}
	else if ((toma_req->position == NVMEIB_TOMA_REQ_MIDDLE) ||
			   (toma_req->position == NVMEIB_TOMA_REQ_LAST)) {
		if (toma_recv_msg->len == 0) {
			_NE(error_1_toma_toma_recv_req_base, "Received @POSITION_STR toma req chunk w/o first - ignored",
			   (toma_req->position == NVMEIB_TOMA_REQ_MIDDLE) ? "middle" : "last");
			goto out;
		}
		if (toma_recv_msg->handle != be64_to_cpu(toma_req->handle)) {
			_NE(error_2_toma_toma_recv_req_base, "Received @POSITION_STR toma req chunk witn inconsistent handle @HANDLE (expecting @HANDLE) - ignored",
			   (toma_req->position == NVMEIB_TOMA_REQ_MIDDLE) ? "middle" : "last",
			   be64_to_cpu(toma_req->handle), toma_recv_msg->handle);
			goto out;
		}
	}
	else {
		_NE(error_3_toma_toma_recv_req_base,
			"Invalid toma req position (@POSITION)", toma_req->position);
		goto out;
	}

	//copy req to acc buf
	if ((rv = nvmeib_container_validate(
		&toma_req->data_ctnr, NVMEIB_TOMA_REQ_DATA_MAGIC, ops->vex_ext)) < 0)
		goto out;
	req_len = nvmeib_container_payload_size(&toma_req->data_ctnr);
	if (toma_recv_msg->len + req_len > NVMEIB_TOMA_BUF_MAX_LEN) {
		_NE(vex_ach_toma_clnt_req_clnt_base_decode_e1,
		   "toma accumulation buf overflow");
		/* Clear accumulation */
		toma_recv_msg->handle = 0;
		toma_recv_msg->len = 0;
		rv = -ENOSPC;
		goto out;
	}
	vex_memcpy(toma_recv_msg->buf + toma_recv_msg->len,
		   NVMEIB_CONT_PAYLOAD(&toma_req->data_ctnr), req_len);
	toma_recv_msg->len += req_len;

	if (toma_req->position & NVMEIB_TOMA_REQ_LAST)
		toma_recv_msg->complete = true;

	rv = sizeof(*toma_req) + req_len;

out:
	return rv;
}

static void toma_send_rsp(struct nvmeibc_ib_admin_channel *ch,
		   struct nvmeibc_toma_recv_msg *toma_recv_msg,
		   u64 req_tag, u8 rsp_opcode)
{
	struct ib_sge sge;
	struct nvmeib_send_wr wr = {};
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	struct volume_client_rsp *rsp = toma_recv_msg->rsp;
	int rv;

	rsp->hdr.opcode = NVMEIB_RSP;
	rsp->hdr.tag = req_tag;
	rsp->opcode = rsp_opcode;

	sge.addr = toma_recv_msg->rsp_dma_addr;
	sge.length = toma_recv_msg->rsp_sz;
	sge.lkey = ch->net.base.lkey;

	nvmeib_send_wr_common(wr).opcode = IB_WR_SEND;
	nvmeib_send_wr_common(wr).wr_id = nvmeib_encode_wr_id(NVMEIB_TOMA_SEND_RSP, (req_tag & 0xffff));	nvmeib_send_wr_common(wr).send_flags = IB_SEND_SIGNALED;
	nvmeib_send_wr_common(wr).sg_list = &sge;
	nvmeib_send_wr_common(wr).num_sge = 1;
	ib_dma_sync_single_for_device(P2IB(ch->net.base.port), toma_recv_msg->rsp_dma_addr,
				      toma_recv_msg->rsp_sz, DMA_TO_DEVICE);
#if ENABLE_SIW
	if (P2NV(ch->net.base.port)->dev_type == DT_siw) {
		if (ch->net.base.shared_cq)
			nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
		else
			nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif
	if ((rv = nvmeibc_ib_post_send(&ch->net.base, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr)) < 0) {
		_NT(nvmeibc_toma_recv_req_e4, "nvmeibc_ib_post_send failed (@RV)", rv);
	}
}

/**
 * Buffer a received Toma req(s) from server. When done
 * buffering process the complete request
 *
 * This function is called from a work queue to serialize toma
 * request fragments of same message.
 *
 * The server, tx side, ensures that toma /proc file write
 * operations (e.g. from different toma threads) are mutually
 * exclusive messages. Thus, the client, rx side, dosen't need
 * to worry about their fragments mixing.
 *
 */
void nvmeibc_toma_recv_req(struct nvmeibc_ib_admin_channel *ch,
						   struct volume_server_req *req,
						   const void *e)
{
	struct volume_server_toma_req *toma_req = &req->toma_req;
	struct nvmeibc_toma_recv_msg *toma_recv_msg = &ch->toma.recv_msg;
	ssize_t rv;

	__NFIN;

	rv = CALL_VEX_OP(decode,
					vex_ach_toma_clnt_req_clnt_ops, BASE_ONLY,
						base, vex_ach_toma_clnt_req_clnt_base_decode,
							ch->base.vex_ops[vex_ach_toma_clnt_req],
							toma_req, e, toma_recv_msg);
	if (rv < 0) {
		_NE(nvmeibc_toma_recv_req_e1,
			"toma accumulation buf overflow");
		goto send_err_rsp;
	}

	if (toma_recv_msg->complete) {
		_ND(nvmeibc_toma_recv_req_t1, "@BASE_NAME (@CH_PTR) - TOMA msg @TAG complete (len @LENGTH)",
		    ch->base.base.name, ch, req->hdr.tag, toma_recv_msg->len);

		/* [NVMESH-1153: Don't use the admin WQ anymore */

		/* Process the request (call the callback) */
		toma_process_req(ch);

		//reset the acc buf
		toma_recv_msg->len = 0;
		toma_recv_msg->complete = false;

		/* Send the response without waiting for send completion (We are in interrupt context) */
		_ND(nvmeibc_toma_recv_req_t2,
			"@BASE_NAME - Sending response @TAG to server", ch->base.base.name, req->hdr.tag);
		toma_send_rsp(ch, toma_recv_msg, req->hdr.tag, NVMEIBC_RSP_TOMA_OPCODE_OK);
	}
	goto out;

send_err_rsp:
	_NT(trace_nvmeibc_toma_recv_req, 
	    "@BASE_NAME - Sending error response for @TAG to server", 
		ch->base.base.name, req->hdr.tag);
	toma_send_rsp(ch, toma_recv_msg, req->hdr.tag, NVMEIBC_RSP_TOMA_OPCODE_ERR);

out:
	__NFOUT;
}

static inline size_t calc_toma_recv_msg_alloc_sz(struct nvmeibc_ib_admin_channel *ch)
{
	return NVMEIB_TOMA_BUF_MAX_LEN + ch->toma.recv_msg.rsp_sz;
}

/**
 * Allocate resources to the disk's main admin channel for
 * communication with Toma via server and send initial
 * introduction message.
 *
 */
int nvmeibc_toma_create(struct nvmeibc_ib_admin_channel *ch,
						nvmeibc_disk_unsubscribe_toma_comp_callback_t *unsub_cb,
						nvmeibc_disk_async_subscribe_toma_comp_callback *async_sub_cb)
{
	int rv = -1;

	NFIN;

	if (ch->toma.recv_msg.buf) {
		_NE(error_toma_nvmeibc_toma_create, "client toma recv buffer already allocated");
		goto out;
	}

	if (!(ch->toma.recv_msg.buf = vzalloc(NVMEIB_TOMA_BUF_MAX_LEN))) {
		_NE(error_1_toma_nvmeibc_toma_create, "Fail to allocate client toma recv buffer");
		goto out;
	}

	ch->toma.recv_msg.rsp_sz = offsetof(struct volume_client_rsp, bytes);
	if (!(ch->toma.recv_msg.rsp = kzalloc(ch->toma.recv_msg.rsp_sz, GFP_KERNEL))) {
		_NE(error_2_toma_nvmeibc_toma_create, "Fail to allocate response buffer");
		goto out;
	}
	ch->toma.recv_msg.rsp_dma_addr = nvmeib_public_ib_dma_map_single(P2IB(ch->net.base.port),
									 ch->toma.recv_msg.rsp, ch->toma.recv_msg.rsp_sz,
									 DMA_TO_DEVICE);
	if (nvmeib_public_ib_dma_mapping_error(P2IB(ch->net.base.port), ch->toma.recv_msg.rsp_dma_addr)) {
		_NE(error_3_toma_nvmeibc_toma_create, "Fail to map response buffer");
		ch->toma.recv_msg.rsp_dma_addr = 0;
		rv = -EFAULT;
		goto out;
	}

	ch->toma.recv_msg.len = 0;
	ch->toma.unsubscribe_comp_cb = unsub_cb;
	ch->toma.async_subscribe_comp_cb = async_sub_cb;
	ch->toma.valid = true;
	rv = 0;

out:
	nvmesh_memmgr_metric_on_alloc_update(toma_recv_msg, calc_toma_recv_msg_alloc_sz(ch), rv == 0 /* success */);
	
	NFOUT;
	return rv;
}


/**
 * Free resources of the disk's main admin channel for
 * communication with Toma via server.
 *
 * Function is called after nvmeibc_ib_net_admin_free() to
 * ensure there's no ongoing processing of toma request.
 *
 */
void nvmeibc_toma_free(struct nvmeibc_ib_admin_channel *ch)
{
	int allocated_sz = 0;
	NFIN;
	ch->toma.valid = false;
	
	if (ch->toma.recv_msg.rsp_dma_addr) {
		nvmeib_public_ib_dma_unmap_single(P2IB(ch->net.base.port), 
						ch->toma.recv_msg.rsp_dma_addr, 
						ch->toma.recv_msg.rsp_sz, DMA_TO_DEVICE);
		ch->toma.recv_msg.rsp_dma_addr = 0;
	}
	if (ch->toma.recv_msg.rsp) {
		allocated_sz += ch->toma.recv_msg.rsp_sz;
		kfree(ch->toma.recv_msg.rsp);
		ch->toma.recv_msg.rsp = NULL;
	}
	if (ch->toma.recv_msg.buf) {
		allocated_sz += NVMEIB_TOMA_BUF_MAX_LEN;
		vfree(ch->toma.recv_msg.buf);
		ch->toma.recv_msg.buf = NULL;
	}
	if (allocated_sz > 0) {
		nvmesh_memmgr_metric_on_free_update(toma_recv_msg, allocated_sz);
	}
	NFOUT;
}

