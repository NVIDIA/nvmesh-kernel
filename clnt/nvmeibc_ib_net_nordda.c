/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#define C_IB_NET_NR_C

#include "kr_incs.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_ib_net_nordda.h"
#include "nvmeibc_ib_nordda_channel.h"
#include "nvmeibc_locks_channel.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_main.h"
#include "nvmeibc_jam.h"
#include "flog.h"
#include "vex/nvmeibc_vex_shared.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"

bool nvmeibc_nr_skip_rdma_write = false;
module_param_named(nr_skip_rdma_write, nvmeibc_nr_skip_rdma_write, bool, 0644);
MODULE_PARM_DESC(nr_skip_rdma_write, "Unsafe debug mode: No-rdda skip rdma write in write operation. " \
									  "Will always work on Legacy volumes and on EC " \
  									  "when CRC check is off and bs=slice_length_of_volumes");

bool nvmeibc_nr_store_fr = true;
module_param_named(nr_store_fr, nvmeibc_nr_store_fr, bool, 0644);
MODULE_PARM_DESC(nr_store_fr, "Store fr to work around loc_prot");

#define __FIN FINS(net->base.ioch->name)
#define __FOUT FOUTS(net->base.ioch->name)

u64 nr_bypass_rdma_write = 0;

#define __NFIN NFINS(net->base.ioch->name)
#define __NFOUT NFOUTS(net->base.ioch->name)


int nvmeibc_ib_net_nordda_alloc(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_ib_net_params *params, struct nvmeibc_login_request *lreq)
{
   return nvmeibc_ib_net_alloc(&net->base, params, lreq);
}

void nvmeibc_ib_net_nordda_free(struct nvmeibc_ib_net_nordda *net)
{
	__NFIN;
	nvmeibc_ib_net_free(&net->base);
	atomic_set(&net->base.dying, 0);
	__NFOUT;
}

static inline int nvmeibc_post_io(struct nvmeibc_ib_net_nordda *net,
	struct nvmeib_send_wr *wrh,
	struct nvmeib_send_wr *wrt,
	struct nvmeib_iu *iu,
	struct nvmeibc_volume_req_info *info,
	int wire_len)
{
	int rv;
	struct ib_device *ibdev = P2IB(net->base.port);
	struct ib_sge list;
	IB_DECLARE_BAD_SEND_WR(bad_wr);

	if (wire_len > NVMEIBC_NORDDA_CLIENT_MSG_SIZE) {
		_NW(nvmeibc_post_io_w1, "wire len too big @INT > @INT",
			wire_len, NVMEIBC_NORDDA_CLIENT_MSG_SIZE);
		rv = -1;
		goto out;
	}

	{
		struct volume_client_req *creq = iu->buf;
		struct volume_client_io_req_base *io_req = &creq->io_req.base;
		struct nvmeibc_disk_io_command *bcmd = info->req.bcmd;

		if (dp_dbgdi_should_add_info_core(bcmd)) {
			dp_dbgdi_do_add_info_core_pre(
				&bcmd->reqs[0],
				&NVMEIBC_CORE_DBGDI_PARAM(
					pre, net->nrch->base.disk->name, ct_n_rdda,
					.io_id  = ++net->nrch->base.dbg_di.io_id,
					.ch_ptr = (u64)&net->nrch->base,
					.reuse_bb = info->req.reused_bb,
					.lock_pgbk  = &info->req.lock_pgbk,
					.start_dlba = bcmd->reqs[0].disk_address,
					.magic_data = &net->nrch->base.dbg_di.magic_data));
		}

		NVMEIB_LOG_GOODPATH_CORE_POST(trace_nvmeibc_post_io,
									  net->nrch->base.disk, bcmd,
									  net->nrch, info->idx,
									  be64_to_cpu(io_req->sw_slba),
									  be64_to_cpu(io_req->data_len),
									  (u8)io_req->ind_op,
									  info->req.reused_bb);
	}

	BUG_ON(info->idx != nordda_tag_decode_index(be64_to_cpu(((struct volume_client_req *)iu->buf)->hdr.tag)));
	BUG_ON(nordda_tag_decode_version_decode_verify_reserved(info->version, be64_to_cpu(((struct volume_client_req *)iu->buf)->hdr.tag)));
	BUG_ON(nordda_tag_decode_ch_version_decode_verify_reserved(info->nrch->base.version, nordda_tag_decode_ch_version(be64_to_cpu(((struct volume_client_req *)iu->buf)->hdr.tag))));
	BUG_ON(test_and_set_bit(info->idx, info->nrch->req_in_use));

	/* JH IOMMU: Correct. Sync data with device for local RDMA_SEND */
	ib_dma_sync_single_for_device(ibdev, iu->dma, wire_len, DMA_TO_DEVICE);
	list.addr = iu->dma;
	list.length = wire_len;
	list.lkey = net->base.lkey;
	nvmeib_send_wr_common(*wrt).opcode = IB_WR_SEND;
	/* we don't wait for send-comp of jour-write before we reuse the io-req for
	   data-write. Thus, when we get a send-comp of this @req->idx, we cant rely
	   on @req->reused_bb to tell if this is send-comp of @req->req.reuse_cmd or
	   of @req->req.cmd. Thus, we encode @req.reused_bb wr->wr-id */
	nvmeib_send_wr_common(*wrt).wr_id =
		nordda_wr_id_encode_with_reused(
			info->version, NVMEIB_SEND_IO, iu->index, info->req.reused_bb);
	nvmeib_send_wr_common(*wrt).sg_list = &list;
	nvmeib_send_wr_common(*wrt).num_sge = 1;
	nvmeib_send_wr_common(*wrt).send_flags = IB_SEND_SIGNALED;
	
#if ENABLE_SIW
	if (P2NV(net->base.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(*wrt).send_flags |= SIW_IB_SEND_TX_TIMESTAMP;
		if (net->base.shared_cq)
			nvmeib_send_wr_common(*wrt).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
		else
			nvmeib_send_wr_common(*wrt).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

#if defined(TAKE_STATS) || defined(MGMT_STATS)
	nvmeib_stats_set_start(&info->send.common);
#endif

	//nvmeibc_ib_net_process_send_cq(...);
	nvmeibc_ib_nordda_channel_req_start_wd(info);
	
	if (!net->base.dev_cq && !is_ll_pcpu_nrch(net->nrch))
		nvmeibc_ib_net_req_notify_send_cq(&net->base);

	/* Latency Measurements */
	nvmeibc_nr_lat_meas_init_req(&info->lat_meas, info->req.dcmd->cmd_type);

	rv = nvmeibc_ib_post_send(&net->base, nvmeib_send_wr_to_ib_ptr(*wrh), &bad_wr);
	if (rv < 0) {
		_NE(error_ib_net_nordda_nvmeibc_post_io, "Fail to send io command on nordda channel @RV", rv);
		nvmeibc_ib_nordda_channel_req_stop_wd(info);
		clear_bit(info->idx, info->nrch->req_in_use);
		goto out;
	}

	nvmeibc_nr_lat_meas_sq_post(&info->lat_meas);

out:
	return rv;
}

struct io_read_ctx {
	struct nvmeibc_ib_net_nordda *net;
	struct nvmeibc_disk *disk;
	struct nvmeibc_volume_req_info *info;
	int status;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_read_clnt_base_encode)
{
	struct io_read_ctx *cmd_ctx = arg;
	struct nvmeibc_ib_net_nordda *net = cmd_ctx->net;
	struct nvmeibc_disk *disk = cmd_ctx->disk;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct volume_client_io_req_base *io_req = wire_buf;
	struct nvmeib_direct_buf *buf;
	struct nvmeib_indirect_buf *indirect_hdr;
	int rv = 0, io_len, i;
	int ind_op;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*io_req) > wire_buf_end);

	if (likely(block_cmd->reqs[0].op == NVMEIB_BLOCK_IO_OP_READ)) {
		if (likely(req->ndesc < 2)) {
			/* the whole command read collapsed into a single entry */
			struct nvmeib_direct_buf *src = &req->table_desc;
			if ((rv = vex_encode_alloc_dref((void **)&buf, dref_alloc_ctx, sizeof(*buf),
					NVMEIB_IO_REQ_DIRECT_BUF_MAGIC, &io_req->buf_dref)) < 0)
				goto out;

			io_len = src->len;
			_ND(trace_1_ib_net_nordda_io_read_encode_base, "va=@VA, len=@LEN, key=@KEY_INT", src->va, src->len, src->key);
			buf->va = cpu_to_be64(src->va);
			buf->len = cpu_to_be32(src->len);
			buf->key = cpu_to_be32(src->key);
		}
		else {
			if ((rv = vex_encode_alloc_dref((void **)&indirect_hdr, dref_alloc_ctx,
					sizeof(*indirect_hdr) + sizeof(indirect_hdr->desc_list[0]) * req->ndesc,
					NVMEIB_IO_REQ_INDIRECT_BUF_MAGIC, &io_req->buf_dref)) < 0)
				goto out;

			io_len = indirect_hdr->len = req->total_dma_len;
			buf = indirect_hdr->desc_list;
			/* convert each element of the S/G list */
			for (i = 0; i < req->ndesc; ++i, ++buf) {
					buf->va = cpu_to_be64(req->indirect_desc[i].va);
					buf->len = cpu_to_be32(req->indirect_desc[i].len);
					buf->key = cpu_to_be32(req->indirect_desc[i].key);
			}
			/* and the table descriptor */
			indirect_hdr->table_desc.va = cpu_to_be64(req->table_desc.va);
			indirect_hdr->table_desc.key =
					cpu_to_be32(req->table_desc.key);
			indirect_hdr->table_desc.len =
					cpu_to_be32(req->table_desc.len);
			indirect_hdr->len = cpu_to_be32(indirect_hdr->len);
			/* and the table count */
			indirect_hdr->table_count = cpu_to_be16(req->ndesc);

		}
		ind_op = NVMEIB_IND_OP_IO_READ;
	}
	else {
		io_len = block_cmd->reqs[0].ndb->length;
		ind_op = NVMEIB_IND_OP_MD_READ;
	}
	/* fill the command */
	memcpy(io_req->disk_name, disk->name, sizeof(io_req->disk_name));
	io_req->sw_slba = cpu_to_be64(block_cmd->reqs[0].disk_address);
	io_req->data_len = cpu_to_be64((u64)io_len);
	io_req->ind_op = ind_op;
	if (req->md.has) {
		if (!(rv = nvmeibc_ib_net_map_md(&net->base, req, io_len,
			NVMEIBC_SECTOR_SHIFT, disk->sector_shift))) {
			io_req->md_desc.raddr = cpu_to_be64(req->md.local.addr);
			io_req->md_desc.size = cpu_to_be32(req->md.local.size);
			io_req->md_desc.rkey = cpu_to_be32(req->md.local.rkey);
			io_req->md_desc.sw_sector_shift =
				cpu_to_be32(NVMEIBC_SECTOR_SHIFT);
		}
		else {
			goto out;
		}
	}
	else
		memset(&io_req->md_desc, 0, sizeof(io_req->md_desc));
	/* check if there is a piggyback read */
	if (dp_cmds_pigbck_has_any(block_cmd)) {
		const struct nvmeibc_d_rdma_comp* dcomp = dp_cmds_get_pigbck_comp_dc(block_cmd);
		struct lock_seg_info *lsi = NULL;
		enum nvmeibc_disk_locks_opr opr_type;
		u64 offset;

		opr_type = dcomp->opr;
		if ((rv = nvmeibc_disk_extract_piggyback_lock_info(disk, block_cmd,
			ac_to_iac(net->base.admin_ch)->lsi, opr_type, &lsi, &offset)) < 0) {
			_NE(error_ib_net_nordda_io_read_encode_base, "Fail to extract read lock piggyback info");
			goto out;
		}
		if (lsi) {
			io_req->lmi = lsi->lmi;
			io_req->offset = cpu_to_be64(offset);

			if (opr_type == NVMEIBC_LOCK_READ) {
				_ND(trace_2_ib_net_nordda_io_read_encode_base, "sending piggy back read lock instruction via nordda channel");
				io_req->ind_pg_op = NVMEIB_IND_PG_READ_LOCK;
			} else {
				_NE(error_1_ib_net_nordda_io_read_encode_base, "Invalid piggy-back read opr_type @OPR_TYPE", opr_type);
				io_req->lmi = 0;
			}
		}
		else {
			io_req->lmi = 0;
		}
	} else {
		io_req->lmi = 0;
	}
	rv = sizeof(*io_req);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(fini, static, vex_nrch_io_read_clnt_base_fini)
{
	struct io_read_ctx *cmd_ctx = arg;
	struct nvmeibc_ib_net_nordda *net = cmd_ctx->net;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;

	if (cmd_ctx->status < 0)
		nvmeibc_ib_net_unmap_md(&net->base, req);

	return 0;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_read_clnt_ext1_encode)
{
	struct io_read_ctx *cmd_ctx = arg;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct volume_client_io_req_ext1 *io_req = wire_buf;

	if (block_cmd->reqs->do_512b_sub_block_x) {
		io_req->is_sub_block = 1;
		io_req->sub_block_idx = do_512b_sub_block_x_val(block_cmd->reqs->do_512b_sub_block_x);
	}

	return sizeof(*io_req);
}

VEX_OPS_DECLARE_OP_FN(fini, static, vex_nrch_io_read_clnt_ext1_fini)
{
	return 0;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_read_clnt_ext2_encode)
{
	struct io_read_ctx *cmd_ctx = arg;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct volume_client_io_req_ext2 *io_req = wire_buf;

	io_req->is_recovery = nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig) ? 1 : 0;

	return sizeof(*io_req);
}

VEX_OPS_DECLARE_OP_FN(fini, static, vex_nrch_io_read_clnt_ext2_fini)
{
	return 0;
}

static int execute_io_read(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk *disk, struct nvmeibc_volume_req_info *info)
{
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeib_iu *iu = req->cmd;
	struct volume_client_req *creq = iu->buf;
	struct nvmeib_send_wr wr;
	struct io_read_ctx cmd_ctx = {};
	const struct vex_ops *vex_ops = net->vex_nrio_ops[vex_nrch_io_read];
	size_t wire_len;
	int rv;

	__NFIN;

	cmd_ctx.net = net;
	cmd_ctx.disk = disk;
	cmd_ctx.info = info;

	_ND(debug_execute_io_read, "ch=@CH_PTR index=@INDEX version=@VERSION release_counter=@RELEASE_COUNTER send_counter=@SEND_COUNTER",
		net->base.ioch, info->idx, info->version, info->release_counter, info->send_counter);

	if ((rv = CALL_VEX_OP(encode,
		vex_nrch_io_read_clnt_ops, TWO_EXT,
			base, vex_nrch_io_read_clnt_base_encode,
			ext1, vex_nrch_io_read_clnt_ext1_encode,
		        ext2, vex_nrch_io_read_clnt_ext2_encode,
				vex_ops, NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(creq),
				iu->buf + iu->size, &cmd_ctx)) < 0) {
		goto out;
	}
	wire_len = rv + NVMEIBC_VOLUME_CLIENT_REQ_HDR_SIZE();
	creq->version_tag = cpu_to_be16(vex_ops->vex_ext);

	/* send the message */
	memset(&wr, 0, sizeof(wr));
	if ((rv = nvmeibc_post_io(net, &wr, &wr, iu, info, wire_len) < 0)) {
		info->release_counter = 0;
		goto out;
	}
	info->send_counter++;

out:
	cmd_ctx.status = rv;
	CALL_VEX_OP(fini,
				vex_nrch_io_read_clnt_ops, TWO_EXT,
				base, vex_nrch_io_read_clnt_base_fini,
				ext1, vex_nrch_io_read_clnt_ext1_fini,
				ext2, vex_nrch_io_read_clnt_ext2_fini,
					vex_ops, &cmd_ctx);

	__NFOUT;
	return rv;
}

struct io_other_cmd_ctx {
	struct nvmeibc_ib_net_nordda *net;
	struct nvmeibc_disk *disk;
	struct nvmeibc_volume_req_info *info;
	struct nvmeib_send_wr *wrh;
	struct nvmeib_send_wr *wrt; // WR Head & Tail;
	enum nvmeib_block_io_op op;
	bool wr_op_sep_md;
	bool write_data;
	int io_len;
	int n_wrs;
	int status;
};

VEX_OPS_DECLARE_OP_FN(init, static, vex_nrch_io_other_clnt_base_init)
{
	int rv;
	struct io_other_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_ib_net_nordda *net = cmd_ctx->net;
	struct nvmeibc_disk *disk = cmd_ctx->disk;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct nvmeib_iu *iu = req->reused_bb ? req->reuse_cmd : req->cmd;
	int n_wr_jmdc_pb = 0;

	cmd_ctx->op = block_cmd->reqs[0].op;
	cmd_ctx->wr_op_sep_md = nvmeib_block_io_op_is_write(cmd_ctx->op) &&
				req->md.has && !req->md.has_inline;
	cmd_ctx->write_data = (nvmeib_block_io_op_is_write(cmd_ctx->op) &&
				(!req->reused_bb || (req->md.has && req->md.has_inline))) ||
				(cmd_ctx->op == NVMEIB_BLOCK_IO_OP_DISCARD);

	if ((rv = nvmeibc_ib_net_build_wriu(&net->base, &info->req, iu, cmd_ctx->op, &cmd_ctx->io_len,
		NVMEIBC_SECTOR_SHIFT, disk->sector_shift, info->raddr, info->rkey))) {
		goto out;
	}

	if (req->bcmd && req->md.has) {
		int jmdc_pb_ops = block_cmd->reqs[0].jam_op.n_ops;
		if (jmdc_pb_ops > NVMEIBC_MAX_JAM_RDMA_OPS) {
			_NE(error_ib_net_nordda_io_other_init_base, "Invalid number of jmdc piggy-back rdma ops @JMDC_PB_OPS", jmdc_pb_ops);
			rv = -EINVAL;
			goto out;
		}
		if (NVMEIBC_NR_CH_RDMA_WRITE_JMDC_PB)
			n_wr_jmdc_pb = jmdc_pb_ops;
	}

	/* Init WRs */
	cmd_ctx->n_wrs = (cmd_ctx->write_data ? iu->n_rdma_iu : 0) +
			 (cmd_ctx->wr_op_sep_md ? 1 : 0) + 1 + n_wr_jmdc_pb;

	if (unlikely(cmd_ctx->n_wrs > ARRAY_SIZE(info->wr))) {
		cmd_ctx->wrh = kzalloc(sizeof(*cmd_ctx->wrh) * cmd_ctx->n_wrs, GFP_ATOMIC);
		if (!cmd_ctx->wrh) {
			_NE(error_1_ib_net_nordda_io_other_init_base, "Failed to allocate WQE for send");
			rv = -ENOMEM;
			goto out;
		}
	} else {
		cmd_ctx->wrh = info->wr;
		memset(cmd_ctx->wrh, 0, sizeof(*cmd_ctx->wrh) * cmd_ctx->n_wrs);
	}
	cmd_ctx->wrt = cmd_ctx->wrh;
	rv = 0;

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_other_clnt_base_encode)
{
	struct io_other_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_ib_net_nordda *net = cmd_ctx->net;
	struct nvmeibc_disk *disk = cmd_ctx->disk;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct nvmeib_iu *iu = req->reused_bb ? req->reuse_cmd : req->cmd;
	struct volume_client_io_req_base *io_req = wire_buf;
	u16 jmd_pb = 0;
	struct nvmeib_rdma_iu *riu;
	enum nvmeibc_disk_locks_opr opr_type;
	u64 offset;
	struct lock_seg_info *lsi = NULL;
	int i = 0, rv = 0;

	/* Add WRs of data to write */
	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*io_req) > wire_buf_end);

	/* buf_dref is used for read, so set to empty */
	vex_dref_init_empty(&io_req->buf_dref, ops);

	if (cmd_ctx->write_data) {
		riu = iu->rius;
		BUG_ON(iu->n_rdma_iu > NVMEIB_MAX_RDMA_UI);
		for (i = 0; i < iu->n_rdma_iu; ++i, ++riu) {
			nvmeibc_ib_net_fill_wr(&cmd_ctx->wrh[i],
				nvmeibc_ib_net_ewrop(i == iu->n_rdma_iu - 1 ?
					NVMEIB_RDMA_LAST : NVMEIB_RDMA_MID_0 + i,
					NVMEIB_SEND_IO, iu->index), riu);
			nvmeib_send_wr_set_next(cmd_ctx->wrh[i], &cmd_ctx->wrh[i + 1]);
		}
	}

	/* journal MD piggyback */
	if (req->md.has && nvmeibc_disk_is_bcmd_jour_write(block_cmd->reqs)) {
		if (NVMEIBC_NR_CH_RDMA_WRITE_JMDC_PB) {
			if ((rv = nvmeibc_ib_net_jmdc_pb_fill_wrs(&net->base, req->bcmd, &net->jmdc_rai,
				&info->jmdc_pb, &cmd_ctx->wrh[i])) < 0) {
				_NE(error_ib_net_nordda_io_other_encode_base, "nvmeibc_ib_net_jmdc_pb_fill_wrs failed (@RV)", rv);
				goto out;
			}
			i += rv;
			rv = 0;
		} else {
			if (1) {
				_NE(error_ib_net_nordda_io_other_encode_base_jmd_pb, "Obsolete mode");
				rv = -1;
				goto out;
			}
			jmd_pb = block_cmd->reqs[0].jam_op.jour_idx[0] + 1; /* 1 based idx */
		}
	}

	/* save the SW sector_shift */
	io_req->md_desc.sw_sector_shift = cpu_to_be32(NVMEIBC_SECTOR_SHIFT);

	/* Add WR of metadata to write */
	if (cmd_ctx->wr_op_sep_md) {
		io_req->md_desc.size = cpu_to_be32(req->md.local.size);
		info->md_sg.addr = req->md.local.addr;
		info->md_sg.length = req->md.local.size;
		info->md_sg.lkey = req->md.local.lkey;
		nvmeib_send_wr_common(cmd_ctx->wrh[i]).opcode = IB_WR_RDMA_WRITE;
		nvmeib_send_wr_common(cmd_ctx->wrh[i]).wr_id =
			nordda_wr_id_encode(info->version, NVMEIB_RDMA_METADATA, iu->index);
		nvmeib_send_wr_rdma(cmd_ctx->wrh[i]).remote_addr = req->md.remote.addr;
		nvmeib_send_wr_rdma(cmd_ctx->wrh[i]).rkey = req->md.remote.rkey;
		nvmeib_send_wr_common(cmd_ctx->wrh[i]).num_sge = 1;
		nvmeib_send_wr_common(cmd_ctx->wrh[i]).sg_list = &info->md_sg;
		nvmeib_send_wr_set_next(cmd_ctx->wrh[i], &cmd_ctx->wrh[i + 1]);
		i++;
	}
	else if (cmd_ctx->op == NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR) {
		/* Read-Mod-Write MD, extd or sep */
		io_req->md_desc.size = cpu_to_be32(NVMEIBC_D2MD_LEN(cmd_ctx->io_len, disk));
		io_req->md_desc.rmw_action = cpu_to_be64((*block_cmd->reqs[0].rmw_md_act));
	}
	else {
		/* Write w/o MD, Write w/ Extd-MD or Discard */
		io_req->md_desc.size = 0;
	}

	/* Fill the command */
	memcpy(io_req->disk_name, disk->name, sizeof(io_req->disk_name));
	if (get_rcookie_ptr(block_cmd)->lba_jam_enc)
		io_req->sw_slba = cpu_to_be64(nvmeibc_jam_decode_lba(block_cmd->reqs[0].disk_address));
	else
		io_req->sw_slba = cpu_to_be64(block_cmd->reqs[0].disk_address);
	io_req->data_len = cpu_to_be64(cmd_ctx->io_len);

	if (nvmeib_block_io_op_is_write(cmd_ctx->op)) {
		io_req->ind_op = NVMEIB_IND_OP_IO_WRITE;
		io_req->jmd_pb[0] = (u8)(jmd_pb);
	}
	else if (cmd_ctx->op == NVMEIB_BLOCK_IO_OP_DISCARD) {
		io_req->ind_op = NVMEIB_IND_OP_IO_DSM;
		//WARN_ON(io_len % sizeof(struct nvmeib_dsm_range));
	}
	else if (cmd_ctx->op == NVMEIB_BLOCK_IO_OP_WRITE_UNCOR) {
		io_req->ind_op = NVMEIB_IND_OP_IO_WRITE_UNCOR;
	}
	else if (cmd_ctx->op == NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR) {
		io_req->ind_op = NVMEIB_IND_OP_MD_RD_MOD_WR;
	}

	/* Check for piggy-back read */
	if (dp_cmds_pigbck_has_any(block_cmd)) {
		const struct nvmeibc_d_rdma_comp* dcomp = dp_cmds_get_pigbck_comp_dc(block_cmd);
		opr_type = dcomp->opr;
		if ((rv = nvmeibc_disk_extract_piggyback_lock_info(disk, block_cmd,
			ac_to_iac(net->base.admin_ch)->lsi, opr_type, &lsi, &offset)) < 0) {
			_NE(error_1_ib_net_nordda_io_other_encode_base, "Fail to extract read lock piggyback info");
			goto out;
		}
		if (lsi) {
			io_req->lmi = lsi->lmi;
			io_req->offset = cpu_to_be64(offset);

			if (opr_type == NVMEIBC_LOCK_BLKSET_INFO_WRITE) {
				const u32 binfo = (u32)dcomp->lock.bi;
				_ND(trace_1_ib_net_nordda_io_other_encode_base, "sending piggy-back write set blkset-info instruction via nordda channel. val=@VAL_INT",
				   binfo);
				io_req->ind_pg_op = NVMEIB_IND_PG_WRITE_BLKSET_INFO;
				io_req->blkset_info = cpu_to_be32(binfo);

				nvmeibc_ib_net_vol_req_dbgdi_set(req,
					dcomp->opr, block_cmd->lpb.addr, dcomp->val[0], dcomp->val[1],
					be64_to_cpu(io_req->offset), be32_to_cpu(io_req->blkset_info));

			} else {
				_NE(error_2_ib_net_nordda_io_other_encode_base, "Invalid piggy-back write opr_type @OPR_TYPE", opr_type);
				io_req->lmi = 0;
			}
		}
		else {
			io_req->lmi = 0;
		}
	} else {
		io_req->lmi = 0;
	}

	cmd_ctx->wrt = &cmd_ctx->wrh[i];
	rv = sizeof(*io_req);

out:
	__NFOUT;
	return rv;
}

VEX_OPS_DECLARE_OP_FN(fini, static, vex_nrch_io_other_clnt_base_fini)
{
	struct io_other_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_ib_net_nordda *net = cmd_ctx->net;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;

	if (unlikely(cmd_ctx->wrh != info->wr))
			kfree(cmd_ctx->wrh);

	if (cmd_ctx->status < 0)
		nvmeibc_ib_net_unmap_md(&net->base, req);

	return 0;
}

VEX_OPS_DECLARE_OP_FN(init, static, vex_nrch_io_other_clnt_ext1_init)
{
	return 0;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_other_clnt_ext1_encode)
{
	struct io_other_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct volume_client_io_req_ext1 *io_req = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*io_req) > wire_buf_end);

	if (block_cmd->reqs->do_512b_sub_block_x) {
		io_req->is_sub_block = 1;
		io_req->sub_block_idx = do_512b_sub_block_x_val(block_cmd->reqs->do_512b_sub_block_x);
	}
	return sizeof(*io_req);
}

VEX_OPS_DECLARE_OP_FN(fini, static, vex_nrch_io_other_clnt_ext1_fini)
{
	return 0;
}

VEX_OPS_DECLARE_OP_FN(init, static, vex_nrch_io_other_clnt_ext2_init)
{
	return 0;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_other_clnt_ext2_encode)
{
	struct io_other_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_volume_req_info *info = cmd_ctx->info;
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_io_command *block_cmd = req->bcmd;
	struct volume_client_io_req_ext2 *io_req = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*io_req) > wire_buf_end);

	io_req->is_recovery = nvmeibc_disk_io_cmd_originator_is_recov(block_cmd->orig) ? 1 : 0;

	return sizeof(*io_req);
}

VEX_OPS_DECLARE_OP_FN(fini, static, vex_nrch_io_other_clnt_ext2_fini)
{
	return 0;
}

static int execute_io_other(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk *disk, struct nvmeibc_volume_req_info *info)
{
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeib_iu *iu = req->reused_bb ? req->reuse_cmd : req->cmd;
	const struct vex_ops *vex_ops = net->vex_nrio_ops[vex_nrch_io_other];
	struct io_other_cmd_ctx cmd_ctx = {};
	struct volume_client_req *creq = iu->buf;
	const void *buf_end = iu->buf + iu->size;
	size_t wire_len;
	struct nvmeib_send_wr *cmds_head;

	_ND(debug_execute_io_other, "ch=@CH_PTR index=@INDEX version=@VERSION release_counter=@RELEASE_COUNTER send_counter=@SEND_COUNTER bcmd=@BCMD reuse action=@ACTION_INT",
		net->base.ioch, info->idx, info->version, info->release_counter, info->send_counter, info->req.bcmd,
		get_rcookie_ptr(info->req.bcmd)->action);

	cmd_ctx.net = net;
	cmd_ctx.disk = disk;
	cmd_ctx.info = info;

	cmd_ctx.status = CALL_VEX_OP(init,
								vex_nrch_io_other_clnt_ops, TWO_EXT,
									base, vex_nrch_io_other_clnt_base_init,
									ext1, vex_nrch_io_other_clnt_ext1_init,
									ext2, vex_nrch_io_other_clnt_ext2_init,
										vex_ops, &cmd_ctx);
	if (cmd_ctx.status < 0)
		goto out;

	cmd_ctx.status = CALL_VEX_OP(encode,
								vex_nrch_io_other_clnt_ops, TWO_EXT,
									base, vex_nrch_io_other_clnt_base_encode,
									ext1, vex_nrch_io_other_clnt_ext1_encode,
									ext2, vex_nrch_io_other_clnt_ext2_encode,
										vex_ops, NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(creq), buf_end, &cmd_ctx);
	if (cmd_ctx.status < 0)
		goto out;

	cmds_head = cmd_ctx.wrh;
	if ((cmd_ctx.wrt - cmds_head + 1) != cmd_ctx.n_wrs) {
		_NW(warn_execute_io_other, "n_wrs=@INT vs. pre-calc=@INT", cmd_ctx.wrt - cmds_head +1, cmd_ctx.n_wrs);
		WARN_ON(1);
	}

	wire_len = cmd_ctx.status + NVMEIBC_VOLUME_CLIENT_REQ_HDR_SIZE();
	creq->version_tag = cpu_to_be16(vex_ops->vex_ext);


#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibc_nr_skip_rdma_write)) {
		/* will make nvmeibc_post_io send only the io request(the tail) */
		cmds_head = cmd_ctx.wrt;
	}
#endif

	if ((cmd_ctx.status = nvmeibc_post_io(net, cmds_head, cmd_ctx.wrt, iu, info, wire_len)) < 0)
		goto out;
	info->send_counter++;

out:
	CALL_VEX_OP(fini,
				vex_nrch_io_other_clnt_ops, TWO_EXT,
				base, vex_nrch_io_other_clnt_base_fini,
				ext1, vex_nrch_io_other_clnt_ext1_fini,
				ext2, vex_nrch_io_other_clnt_ext2_fini,
				vex_ops, &cmd_ctx);

	return cmd_ctx.status;
}

static int execute_io(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk *disk, struct nvmeibc_volume_req_info *info,
	struct nvmeibc_disk_io_command *block_cmd)
{
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeib_iu *iu = req->reused_bb ? req->reuse_cmd : req->cmd;
	struct volume_client_req *cmd = iu->buf;
	int dma_len = NVMEIBC_NORDDA_CLIENT_IO_REQ_MIN_SIZE;
	struct nvmeib_data_buffer *ndb = block_cmd->reqs[0].ndb;
	const enum nvmeib_block_io_op op = block_cmd->reqs[0].op;
	int rv = -1;

	__NFIN;

	/* check if counter was set properly by ULP
	   specifically for reuse of jour-cmd by data-cmd */
	DEBUG_TRANSFERS_is_init_cb_counter(&block_cmd->comp);

	_ND(trace_1_ib_net_nordda_execute_io, "Start block_command=@BLOCK_COMMAND id=@REQ_ID_LLONG", block_cmd, block_cmd->req_id);
	/* Sanity - check that the bytes we need to IO fit in the BB */
	if ((ndb->length > net->nrch->max_io_sz) || !ndb->length) {
		_NE(error_ib_net_nordda_execute_io, "Invalid ndb length (@LENGTH_INT, BB-size @MAX_NORDDA_BB)",
			ndb->length, disk->max_nordda_bb);
		WARN_ON_ONCE(1);
		goto out;
	}
	if (req->bcmd) {
		_NE(error_execute_io_0,
			"net=@PTR (req=@PTR) still linked to cmd=@PTR",
			net, req, req->bcmd);
		BUG();
	}
	if (!req->fr_list && !req->reused_bb) {
		_NE_dmesg(error_execute_io_1, "net=@PTR (req=@PTR) has null fr_list and reused_bb=0", net, req);
		BUG();
	}

	nvmeibc_disk_cmd_piggyback_lock_read_poison_inject(
		block_cmd, NVMEIBC_DISK_CMD_PBLR_POISON_NORDDA);

	/* set dma-mapping direction - used by data and metadata */
	/* JH IOMMU: DMA_FROM_DEVICE is correct for read ops, used as a sink for Remote RDMA_WRITE 
	 * DMA_TO_DEVICE is correct for write ops, used as a source for Local RDMA_WRITE */
	req->dma_dir =
		(op == NVMEIB_BLOCK_IO_OP_READ ||
		 op == NVMEIB_BLOCK_IO_OP_MD_READ) ? DMA_FROM_DEVICE :
		DMA_TO_DEVICE;

	/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer is used as source for local RDMA_SEND */
	ib_dma_sync_single_for_cpu(P2IB(net->base.port), iu->dma, dma_len,
		DMA_TO_DEVICE);
	memset(cmd, 0, dma_len);

	/* link req and iocmd */
	req->bcmd = block_cmd;
	req->md.has = !!disk->md_size;
	if (req->md.has) {
		req->md.is_dummy = false;
		req->md.has_inline = disk->md_extd;
		req->md.entry_size = disk->md_size;
		req->md.data = req->bcmd->reqs[0].md;
		req->md.local.addr = 0;
		req->md.local.size = 0;
		req->md.local.lkey = net->base.lkey;
		req->md.local.rkey = net->base.rkey;
	}
	req->bb_pages = info->rn_pages;

	/* set command parameters */
	cmd->hdr.opcode = NVMEIB_CMD;
	cmd->client_op = NVMEIBC_CR_IO;
	/* set the io-ka piggyback */
	cmd->io_req.base.io_ka_val = cpu_to_be64(jiffies);

	/* map the block's SG list to the IB device and place the
	   description of the result @req->cmd->buf->io_req.byte */
	if (op == NVMEIB_BLOCK_IO_OP_READ ||
		(nvmeib_block_io_op_is_write(op) &&
		(!req->reused_bb || (req->md.has && req->md.has_inline))) ||
		op == NVMEIB_BLOCK_IO_OP_DISCARD) {
		if ((rv = nvmeibc_ib_net_map_data(&net->base, req, 0)) < 0) {
			_NE(error_1_ib_net_nordda_execute_io, "Failed to map data, @RV", rv);
			goto out;
		}
	}

	/* we must set the version before we set the release counter */
	NVMEIB_INC_TAG_VERSION(info->version);
	/* we set the release counter to be 2 since release includes two events:
	   1. get the send completion on the request
	   2. getting the reply
	   we must wait for the send completion otherwise we may encounter
	   a full completion buffer
	*/
	info->release_counter = 2;

	cmd->hdr.tag = cpu_to_be64(nordda_tag_encode(&disk->last_tgt_ver, (u32)info->nrch->base.version, info->version, req->index));

	nflog(flog_ib_net_nordda_execute_io, "NORDDA - io-op: @BLOCK_IO_OP length: @LENGTH_INT", op, ndb->length);

	switch (op) {
	case NVMEIB_BLOCK_IO_OP_READ:
	case NVMEIB_BLOCK_IO_OP_MD_READ:
		if (req->md.has && !req->md.data) {
			if (op == NVMEIB_BLOCK_IO_OP_MD_READ) {
				_NW(warn_c_net_nordda_execute_io_local_cmd_io_md_rd_no_md,
					"@DISK_STR - NVMEIB_BLOCK_IO_OP_MD_READ (@OP) with NULL metadata",
					disk->name, NVMEIB_BLOCK_IO_OP_MD_READ);
			}
			req->md.data = info->nrch->lionic->dummy_md_read_ptr;
			req->md.local.addr = info->nrch->lionic->dummy_md_read_addr;
			req->md.is_dummy = true;
		}
		rv = execute_io_read(net, disk, info);
		break;
	case NVMEIB_BLOCK_IO_OP_WRITE:
		if (req->md.has && !req->md.data) {
			req->md.data = info->nrch->lionic->dummy_md_write_ptr;
			req->md.local.addr = info->nrch->lionic->dummy_md_write_addr;
			req->md.is_dummy = true;
		}
		FALLTHRU;
	case NVMEIB_BLOCK_IO_OP_WRITE_UNCOR:
	case NVMEIB_BLOCK_IO_OP_DISCARD:
	case NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR:
		rv = execute_io_other(net, disk, info);
		break;
	default:
		_NE(error_2_ib_net_nordda_execute_io, "nvmeibc unknown NVME op @BLOCK_IO_OP", op);
		BUG();
	}

	if (rv < 0) {
		_NE(error_3_ib_net_nordda_execute_io, "I/O execution failed");
		goto err_unmap;
	}
	else
		_ND(trace_2_ib_net_nordda_execute_io, "I/O execution success");
	nvmeibc_block_cmd_status_debug(block_cmd, NVMEIBC_BLOCK_CMD_NORDDA_SENT);
	nvmeibc_disk_cmd_status_debug(&block_cmd->disk_cmd,
			NVMEIBC_DISK_CMD_NORDDA_SENT);
	goto out;

err_unmap:
	nvmeibc_ib_net_unmap_md(&net->base, req);
	nvmeibc_ib_net_free_req(&net->base, req);
	nvmeibc_ib_net_complete_iocmd_sg(&net->base, req);
	info->release_counter = 0;

out:
	__NFOUT;
	return rv;
}

static int execute_gen_send_request_signaled(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_req_info *info, u16 version_tag, bool has_sink)
{
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeibc_disk_gen_cmd *gen_cmd = disk_to_gen(req->dcmd);
	struct nvmeib_iu *iu = req->cmd;
	struct volume_client_req *cmd = iu->buf;
	int dma_len = NVMEIBC_NORDDA_CLIENT_GEN_REQ_SIZE;
	struct ib_sge list, *wr_sges = NULL, wr_siw_sge;
	struct nvmeib_send_wr *wr = info->wr, *first_wr = info->wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv = -1, nents = 0, i;
	struct scatterlist *sg;
	u64 bb_raddr;
	off_t sg_off = 0;

	__NFIN;
	if (has_sink) {
//		if (nvmeibc_ib_net_map_sink(&net->base, req))
//			goto out;
////		cmd->gen_req.rai.raddr = cpu_to_be64(req->sink.dma_addr);
////		cmd->gen_req.rai.len = cpu_to_be32(req->sink.size);
////		cmd->gen_req.rai.rkey = cpu_to_be32(net->base.rkey);
	}

	/* wait only for both send and recv comp */
	NVMEIB_INC_TAG_VERSION(info->version);
	info->release_counter = 2;

	if (gen_cmd->src_ndb.table.sgl) {
		if (P2NV(net->base.port)->dev_type == DT_siw) {
			/* For SIW, we can just use the virtual address */
			memset(wr, 0, sizeof(*wr));
			nvmeib_send_wr_common(*wr).opcode = IB_WR_RDMA_WRITE;
			nvmeib_send_wr_common(*wr).wr_id =
				nordda_wr_id_encode(info->version, NVMEIB_RDMA_LAST, iu->index);
			nvmeib_send_wr_common(*wr).sg_list = &wr_siw_sge;
			nvmeib_send_wr_common(*wr).num_sge = 1;
			nvmeib_send_wr_set_next(*wr, (wr + 1));
			nvmeib_send_wr_rdma(*wr).rkey = info->rkey;
			nvmeib_send_wr_rdma(*wr).remote_addr = info->raddr;
			wr_siw_sge.addr = (u64)gen_cmd->src + gen_cmd->src_off;
			wr_siw_sge.length = gen_cmd->src_len;
			wr_siw_sge.lkey = nvmeib_get_lkey(P2NV(net->base.port));
			wr++;
		} else {
			/* Map bounce-buffer source for DMA */
			/* JH IOMMU: DMA_TO_DEVICE is correct. Only used as source for Local RDMA Write (Write Gen Cmd Input into remote BB)  */
			nents = ib_dma_map_sg(P2IB(net->base.port), gen_cmd->src_ndb.table.sgl,
								gen_cmd->src_ndb.table.nents, DMA_TO_DEVICE);
			if (!nents) {
				_NE(error_ib_net_nordda_execute_gen_send_request_signaled, "DMA mapping failed for sgcount");
				goto out;
			}
			nvmeib_public_ib_dma_sync_sg_for_device(P2IB(net->base.port), 
								gen_cmd->src_ndb.table.sgl, nents, DMA_TO_DEVICE);
			if (nents > ARRAY_SIZE(info->wr) - 1) {
				/* Dynamically allocate more wrs */
				if (!(wr = kzalloc((nents + 1) * sizeof(*wr), GFP_ATOMIC))) {
					_NE(error_1_ib_net_nordda_execute_gen_send_request_signaled, "Memory allocation error");
					goto out;
				}
				first_wr = wr;
			}
			if (!(wr_sges = kzalloc(nents * sizeof(*wr_sges), GFP_ATOMIC))) {
				_NE(error_2_ib_net_nordda_execute_gen_send_request_signaled, "Memory allocation error");
				goto out;
			}
			bb_raddr = info->raddr;
			/* Fill wrs for transfer */
			for_each_sg(gen_cmd->src_ndb.table.sgl, sg, nents, i) {
				if (sg_off + sg_dma_len(sg) <= gen_cmd->src_off) {
					sg_off += sg_dma_len(sg);
					continue;
				}
				wr_sges[i].addr = sg_dma_address(sg);
				wr_sges[i].length = sg_dma_len(sg);
				wr_sges[i].lkey = nvmeib_get_lkey(P2NV(net->base.port));

				if (sg_off < gen_cmd->src_off) {
					u32 sg_delta = gen_cmd->src_off - sg_off;
					wr_sges[i].addr += sg_delta;
					wr_sges[i].length -= sg_delta;
				}
				if (gen_cmd->src_off + gen_cmd->src_len < sg_off + wr_sges[i].length)
					wr_sges[i].length = gen_cmd->src_off + gen_cmd->src_len - sg_off;

				nvmeib_send_wr_common(*wr).opcode = IB_WR_RDMA_WRITE;
				nvmeib_send_wr_common(*wr).wr_id =
					nordda_wr_id_encode(info->version,
											i == ((nents - 1) ? NVMEIB_RDMA_LAST : NVMEIB_RDMA_MID_0 + i), iu->index);
				nvmeib_send_wr_common(*wr).sg_list = &wr_sges[i];
				nvmeib_send_wr_common(*wr).num_sge = 1;
				nvmeib_send_wr_set_next(*wr, (wr + 1));
				nvmeib_send_wr_rdma(*wr).rkey = info->rkey;
				nvmeib_send_wr_rdma(*wr).remote_addr = bb_raddr;
				bb_raddr += wr_sges[i].length;
				sg_off += wr_sges[i].length;
				wr++;
				if (sg_off >= gen_cmd->src_off + gen_cmd->src_len)
					break;
			}
		}
	}

	cmd->hdr.opcode = NVMEIB_GEN_CMD;
	cmd->version_tag = cpu_to_be16(version_tag);
	/* JH IOMMU: Correct. Syncs data prepared by CPU to NIC as source for Local RDMA Send */
	ib_dma_sync_single_for_device(P2IB(net->base.port), iu->dma, dma_len,
		DMA_TO_DEVICE);

	_ND(debug_execute_gen_send_request_signaled_siw, "ch=@CH_PTR index=@INDEX release_counter=@RELEASE_COUNTER send_counter=@SEND_COUNTER",
		net->base.ioch, info->idx, info->release_counter, info->send_counter);

	cmd->hdr.tag = cpu_to_be64(nordda_tag_encode(&net->base.ioch->disk->last_tgt_ver, (u32)info->nrch->base.version, info->version, req->index));
	BUG_ON(test_and_set_bit(info->idx, info->nrch->req_in_use));

	list.addr = iu->dma;
	list.length = NVMEIBC_NORDDA_CLIENT_GEN_REQ_SIZE;
	list.lkey = net->base.lkey;
	memset(wr, 0, sizeof(*wr));
	nvmeib_send_wr_common(*wr).opcode = IB_WR_SEND;
	nvmeib_send_wr_common(*wr).wr_id =
		nordda_wr_id_encode_gen(info->version, gen_cmd->opcode, iu->index);
	nvmeib_send_wr_common(*wr).sg_list = &list;
	nvmeib_send_wr_common(*wr).num_sge = 1;
	nvmeib_send_wr_common(*wr).send_flags = IB_SEND_SIGNALED;

	nvmeib_send_wr_clear_next(*wr);

#if ENABLE_SIW
	if (P2NV(net->base.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(*wr).send_flags |= SIW_IB_SEND_TX_TIMESTAMP;
		if (net->base.shared_cq)
			nvmeib_send_wr_common(*wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SAME_CPU;
		else
			nvmeib_send_wr_common(*wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

	/* start WD and post send */
	nvmeibc_ib_nordda_channel_req_start_wd(info);
	if (!net->base.dev_cq)
		nvmeibc_ib_net_req_notify_send_cq(&net->base);
	gen_cmd->send_time = ktime_get();
	if ((rv = nvmeibc_ib_post_send(&net->base,
			nvmeib_send_wr_to_ib_ptr(*first_wr), &bad_wr)) < 0) {
		_NE(error_3_ib_net_nordda_execute_gen_send_request_signaled, "Fail to send gen command on nordda channel @RV", rv);
		nvmeibc_ib_nordda_channel_req_stop_wd(info);
		info->release_counter = 0;
		clear_bit(info->idx, info->nrch->req_in_use);
		goto out;
//		if (has_sink)
//			nvmeibc_ib_net_unmap_sink(&net->base, req);
	}
	info->send_counter++;

out:
	kfree(wr_sges);
	if (first_wr != info->wr)
		kfree(first_wr);
	__NFOUT;
	return rv;
}

#define execute_gen_rdma_get_signaled(_n_, _i_, _v_)	({		\
	int _rv_ = 											\
	execute_gen_send_request_signaled(_n_, _i_, _v_, true);	\
	_rv_; 												\
})

#define execute_gen_send_req_signaled(_n_, _i_, _v_)	({		\
	int _rv_ = 											\
	execute_gen_send_request_signaled(_n_, _i_, _v_, false);	\
	_rv_; 												\
})



struct nvmeibc_gen_cmd_ctx {
	struct nvmeibc_ib_net_nordda *net;
	struct nvmeibc_disk *disk;
	struct nvmeibc_disk_gen_cmd *gen_cmd;
	struct nvmeibc_volume_request *req;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_br_req_clnt_base_encode)
{
	struct nvmeibc_gen_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd_ctx->gen_cmd;
	struct volume_client_gen_req_blkset_recovered_base *br = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*br) > wire_buf_end);

	br->uuid = gen_cmd->param.br.uuid;
	memcpy(br->ds_uuid, gen_cmd->param.br.ds_uuid, sizeof(br->ds_uuid));
	br->blkset_num = cpu_to_be64(gen_cmd->param.br.blkset_num);
	br->blkset_slba = cpu_to_be64(gen_cmd->param.br.blkset_slba);
	br->lock_ent = cpu_to_be64(gen_cmd->param.br.lock_ent);
	br->rng_id = cpu_to_be32(gen_cmd->param.br.rng_id);
	br->ent_id = cpu_to_be32(gen_cmd->param.br.ent_id);
	br->pass2toma = gen_cmd->param.br.pass2toma;

	return sizeof(*br);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_uj_req_clnt_base_encode)
{
	struct nvmeibc_gen_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd_ctx->gen_cmd;
	struct volume_client_gen_req_uuid_jour_base *uj = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*uj) > wire_buf_end);
	uj->client_uuid = gen_cmd->param.uj.client_uuid;

	BUILD_BUG_ON(ARRAY_SIZE(uj->sgmnt_uuid) != ARRAY_SIZE(gen_cmd->param.uj.sgmnt_uuid));
	memcpy(uj->sgmnt_uuid, gen_cmd->param.uj.sgmnt_uuid, ARRAY_MEM_SIZE(gen_cmd->param.uj.sgmnt_uuid));

	uj->jmdc_rai.raddr = cpu_to_be64(gen_cmd->param.uj.jmdc_dest.remote.raddr);
	uj->jmdc_rai.len = cpu_to_be32(gen_cmd->param.uj.jmdc_dest.remote.len);
	uj->jmdc_rai.rkey = cpu_to_be32(gen_cmd->param.uj.jmdc_dest.remote.rkey);

	uj->ent_md_rai.raddr = cpu_to_be64(
		gen_cmd->param.uj.ent_md_dest.remote.raddr);
	uj->ent_md_rai.len = cpu_to_be32(gen_cmd->param.uj.ent_md_dest.remote.len);
	uj->ent_md_rai.rkey = cpu_to_be32(gen_cmd->param.uj.ent_md_dest.remote.rkey);
	return sizeof(*uj);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_uj_req_clnt_ext1_encode)
{
	struct nvmeibc_gen_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd_ctx->gen_cmd;
	struct volume_client_gen_req_uuid_jour_ext1 *uj = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*uj) > wire_buf_end);
	uj->binje = cpu_to_be32(gen_cmd->param.uj.binje);

	return sizeof(*uj);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_db_req_clnt_base_encode)
{
	struct nvmeibc_gen_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd_ctx->gen_cmd;
	struct disk_req_get_ec_dirty_bits_base *db = wire_buf;
	struct nvmeibc_disk_dirty_bits_mapping* nic_mapping;
	int found_port = 0;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*db) > wire_buf_end);
	db->lba = cpu_to_be64((u64)gen_cmd->param.db.lba);
	db->sectors = cpu_to_be32(gen_cmd->param.db.n_lba);
	db->get_dbits = gen_cmd->param.db.get_dbits;
	db->get_stales = gen_cmd->param.db.get_stales;
	db->get_full_val = gen_cmd->param.db.get_full_val;
	db->reserved = gen_cmd->param.db.reserved;

	list_for_each_entry(nic_mapping, &cmd_ctx->disk->db.dirty_bits_mappings, link) {
		if (nic_mapping->nic_dev == cmd_ctx->net->base.port->nic_dev) {
			_NT(vex_nrch_gen_db_clnt_base_encode_t1234,
				"found disk's mapping for ec dirty bits memory "
				"@PTR @PTR rkey=@INT32_HEX",
				cmd_ctx->disk, nic_mapping, nic_mapping->map.rkey);
			found_port = 1;
			break;
		}
	}

	if (!found_port) {
		_NT(vex_nrch_gen_db_clnt_base_encode_t1235,
			"couldn't find port's gid on disk's list");
		rv = -ENOENT;
		goto out;
	}

	db->rai.raddr = cpu_to_be64(nic_mapping->ioaddr);
	db->rai.len = cpu_to_be32(nic_mapping->length);
	db->rai.rkey = cpu_to_be32(nic_mapping->rkey);

		_NT(trace_ib_net_nordda_genc_op_get_ec_db_base, "lba=@LBA_LLONG get_dbits=@GET_DBITS get_stales=@GET_STALES get_full_val=@GET_FULL_VAL",
	   db->lba, db->get_dbits, db->get_stales, db->get_full_val);

	rv = sizeof(*db);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_fje_ent_clnt_base_encode)
{
	struct nvmeib_free_ents_data *ents_data = arg;
	struct wire_free_ents_entry_base *base = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	base->rng_idx = cpu_to_be16(ents_data->rng_idx);
	base->rng_gen_id = cpu_to_be64(ents_data->rng_gen_id);
	base->ent_idx = cpu_to_be16(ents_data->ent_idx);
	base->ent_md_gen_id = ents_data->ent_md.ent_gen_id;

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_fje_ent_clnt_ext1_encode)
{
	struct nvmeib_free_ents_data *ents_data = arg;
	struct wire_free_ents_entry_ext1 *ext1 = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	ext1->binje = binje_to_be(ents_data->rng_binje);

	return sizeof(*ext1);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_fje_clnt_base_encode)
{
	struct nvmeibc_gen_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd_ctx->gen_cmd;
	struct volume_client_gen_req_free_ents_base *free_ents_req = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*free_ents_req) > wire_buf_end);

	memcpy(free_ents_req->seg_uuid, gen_cmd->param.free_ents.seg_uuid, NVMEIB_GID_STR_MAX);
	free_ents_req->src = gen_cmd->param.free_ents.src;
	free_ents_req->num_ents = cpu_to_be16(gen_cmd->param.free_ents.num_ents);
	free_ents_req->pass2toma = gen_cmd->param.free_ents.pass2toma;
	free_ents_req->blkset_num = cpu_to_be64(gen_cmd->param.free_ents.blkset_num);
	free_ents_req->blkset_slba = cpu_to_be64(gen_cmd->param.free_ents.blkset_slba);
	free_ents_req->lock_ent = cpu_to_be64(gen_cmd->param.free_ents.lock_ent);
	memcpy(free_ents_req->serjio_boot_id,
		   gen_cmd->param.free_ents.serjio_boot_id, NVMEIB_GID_STR_MAX);

	return sizeof(*free_ents_req);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_je_clnt_base_encode)
{
	struct nvmeibc_gen_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd_ctx->gen_cmd;
	struct volume_client_gen_req_jentry_erase_base *je = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*je) > wire_buf_end);

	if (ops->vex_ext == link_ext) {
		_NT(trace_vex_je_encode, "Encode Base NVMEIB_GEN_OP_JENTRY_ERASE (@GEN_CMD_OP)  - Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID, LBA: @SW_LBA", NVMEIB_GEN_OP_JENTRY_ERASE,
			gen_cmd->param.je.rng_idx, gen_cmd->param.je.ent_erase.ent_idx,
		 gen_cmd->param.je.rng_gen_id, gen_cmd->param.je.ent_erase.ent_idx,
		 gen_cmd->param.je.ent_erase.ent_swlba);
	}

	je->rng_gen_id = cpu_to_be64(gen_cmd->param.je.rng_gen_id);
	je->rng_id = cpu_to_be32(gen_cmd->param.je.rng_idx);
	je->ent_id = cpu_to_be32(gen_cmd->param.je.ent_erase.ent_idx);
	je->ent_md = gen_cmd->param.je.ent_erase.ent_md;
	je->sw_jlba = cpu_to_be64(gen_cmd->param.je.ent_erase.ent_swlba);

	return sizeof(*je);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_je_clnt_ext1_encode)
{
	struct nvmeibc_gen_cmd_ctx *cmd_ctx = arg;
	struct nvmeibc_disk_gen_cmd *gen_cmd = cmd_ctx->gen_cmd;
	struct volume_client_gen_req_jentry_erase_ext1 *je = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*je) > wire_buf_end);

	if (ops->vex_ext == link_ext) {
		_NT(trace_vex_je_encode_ext1, "Encode Ext1 NVMEIB_GEN_OP_JENTRY_ERASE (@GEN_CMD_OP)  - Range: @JRNL_RNG_IDX N: @BINJE Entry: @JRNL_RNG_ENT_IDX GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID, LBA: @SW_LBA", NVMEIB_GEN_OP_JENTRY_ERASE,
			gen_cmd->param.je.rng_idx, gen_cmd->param.je.rng_binje, gen_cmd->param.je.ent_erase.ent_idx,
		 gen_cmd->param.je.rng_gen_id, gen_cmd->param.je.ent_erase.ent_idx,
		 gen_cmd->param.je.ent_erase.ent_swlba);
	}

	je->rng_binje = binje_to_be(gen_cmd->param.je.rng_binje);

	return sizeof(*je);
}

/* Execute Generic Commnads from upper layer.
 * Supported types are:
 *
 * 1. NVMEIB_RDMA_X
 * 		RDMA-Write X to pre-mapped area @Target.
 * 		Callback upper layer on send-comp; No Rsp.
 *
 * 2. NVMEIB_SEND_X
 * 		Send Req X to Target to execute operation.
 * 		Callback upper layer on recv-comp of Rsp.
 *
 * 3. NVMEIB_RDMA_GET_X
 * 		Send Req to Target to RDMA-Write X to clnt's sink buffer.
 *  	Callback upper layer on recv-comp of Rsp.
 */

bool nvmeibc_debug_fail_uj_encode = false; //true;
module_param_named(debug_fail_uj_encode, nvmeibc_debug_fail_uj_encode, bool, 0644);
MODULE_PARM_DESC(debug_fail_uj_encode, "...");

static int execute_gen(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk *disk, struct nvmeibc_volume_req_info *info,
	struct nvmeibc_disk_gen_cmd *gen_cmd)
{
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeib_iu *iu = req->cmd;
	struct volume_client_req *cmd = iu->buf;
	const void *req_buf_end = iu->buf + iu->size;
	const struct vex_ops *gen_ops = NULL;
	struct nvmeibc_gen_cmd_ctx cmd_ctx;
	int rv = -1;

	__NFIN;

	if (gen_cmd->disk_cmd.owner) {
		DEBUG_TRANSFERS_is_init_cb_counter(
			&((const struct nvmeibc_disk_io_command *)gen_cmd->disk_cmd.owner)->comp);
	}
	cmd_ctx.net = net;
	cmd_ctx.req = req;
	cmd_ctx.gen_cmd = gen_cmd;
	cmd_ctx.disk = disk;
	memset(cmd, 0, NVMEIBC_NORDDA_CLIENT_GEN_REQ_SIZE);

	cmd->gen_req.op = gen_cmd->opcode;
	req->dcmd = &gen_cmd->disk_cmd;

	switch (gen_cmd->opcode) {
	case NVMEIB_GEN_OP_JENTRY_ERASE:
		if (gen_cmd->param.je.rng_gen_id != disk->jour.rng_gen_id) {
			_NW(warn_c_net_nordda_je_gen_id_wrong,
				"@GEN_CMD_OP - Invalid Range GenID: @JRNL_RNG_GEN_ID (not @JRNL_RNG_GEN_ID) - "
				"Disk: @DISK_NAME, JRI: @JRNL_RNG_IDX", NVMEIB_GEN_OP_JENTRY_ERASE,
				gen_cmd->param.je.rng_gen_id, disk->jour.rng_gen_id, disk->name, disk->jour.rng_id);
			rv = -EINVAL;
			goto out;
		}
		gen_ops = net->vex_nrio_ops[vex_nrch_gen_je];
		if ((rv = CALL_VEX_OP(encode,
				vex_nrch_gen_je_clnt_ops, ONE_EXT,
					base, vex_nrch_gen_je_clnt_base_encode,
					ext1, vex_nrch_gen_je_clnt_ext1_encode,
						gen_ops, NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(cmd),
						req_buf_end, &cmd_ctx)) < 0)
			goto out;
		rv = execute_gen_send_req_signaled(net, info, gen_ops->vex_ext);
		break;

	case NVMEIB_GEN_OP_BLKSET_RECOVERED:
		gen_ops = net->vex_nrio_ops[vex_nrch_gen_br_req];
		if ((rv = CALL_VEX_OP(encode,
				vex_nrch_gen_br_req_clnt_ops, BASE_ONLY,
					base, vex_nrch_gen_br_req_clnt_base_encode,
						gen_ops, NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(cmd),
						req_buf_end, &cmd_ctx)) < 0)
			goto out;
		rv = execute_gen_send_req_signaled(net, info, gen_ops->vex_ext);
		break;

	case NVMEIB_GEN_OP_GET_UUID_JOUR:
		gen_ops = net->vex_nrio_ops[vex_nrch_gen_uj_req];
		if ((rv = nvmeibc_ib_net_map_gen_data(&net->base, req)) < 0)
			goto out;

		if (nvmeibc_debug_fail_uj_encode ||
			(rv = CALL_VEX_OP(encode,
				vex_nrch_gen_uj_req_clnt_ops, ONE_EXT,
					base, vex_nrch_gen_uj_req_clnt_base_encode,
					ext1, vex_nrch_gen_uj_req_clnt_ext1_encode,
						gen_ops, NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(cmd),
						req_buf_end, &cmd_ctx)) < 0) {
			_NE_dmesg(__AUTOID__, "Fail get-uud-jour ecode --> unmap w/o sending");
			nvmeibc_ib_net_unmap_gen_data(&net->base, req);
			goto out;
		}
		rv = execute_gen_rdma_get_signaled(net, info, gen_ops->vex_ext);
		break;

	case NVMEIB_GEN_OP_GET_EC_DB:
		gen_ops = net->vex_nrio_ops[vex_nrch_gen_db_req];
		if ((rv = CALL_VEX_OP(encode,
					vex_nrch_gen_db_req_clnt_ops, BASE_ONLY,
						base, vex_nrch_gen_db_req_clnt_base_encode,
							gen_ops, NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(cmd),
							req_buf_end, &cmd_ctx)) < 0)
			goto out;
		rv = execute_gen_rdma_get_signaled(net, info, gen_ops->vex_ext); //omril: this guy doesn't use the right func
		break;

	case NVMEIB_GEN_OP_FREE_JRNL_ENTS:
	{
		int i;
		void *enc_ptr = gen_cmd->src;
		void *enc_end = gen_cmd->src + gen_cmd->src_len;

		/* Encode the entry data */
		gen_ops = net->vex_nrio_ops[vex_nrch_gen_fje_ent];
		for (i = 0; i < gen_cmd->param.free_ents.num_ents; i++) {
			if ((rv = CALL_VEX_OP(encode,
								vex_nrch_gen_fje_ent_clnt_ops, ONE_EXT,
									base, vex_nrch_gen_fje_ent_clnt_base_encode,
									ext1, vex_nrch_gen_fje_ent_clnt_ext1_encode,
										gen_ops, enc_ptr,
										enc_end, &gen_cmd->param.free_ents.ents[i])) < 0)
				goto out;
			enc_ptr += rv;
		}
		/* Encode the request */
		gen_ops = net->vex_nrio_ops[vex_nrch_gen_fje];
		if ((rv = CALL_VEX_OP(encode,
					vex_nrch_gen_fje_clnt_ops, BASE_ONLY,
						base, vex_nrch_gen_fje_clnt_base_encode,
							gen_ops, NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(cmd),
							req_buf_end, &cmd_ctx)) < 0)
			goto out;
		rv = execute_gen_send_req_signaled(net, info, gen_ops->vex_ext);
		break;
	}
	default:
		_NE(error_ib_net_nordda_execute_gen, "Unknown cmd_type @OPCODE", gen_cmd->opcode);
		break;
	}

out:
	if (rv < 0)
		_NE(error_1_ib_net_nordda_execute_gen, "Gen execution @OPCODE failed", gen_cmd->opcode);
	else {
		_ND(trace_ib_net_nordda_execute_gen, "Gen execution @OPCODE success", gen_cmd->opcode);
#ifdef DEBUG_UNCOMPLETED
		if (gen_cmd->disk_cmd.owner)
			nvmeibc_block_cmd_status_debug(
				((struct nvmeibc_disk_io_command *)gen_cmd->disk_cmd.owner),
				NVMEIBC_BLOCK_CMD_NORDDA_SENT);
#endif
		nvmeibc_disk_cmd_status_debug(&gen_cmd->disk_cmd,
				NVMEIBC_DISK_CMD_NORDDA_SENT);
	}

	__NFOUT;
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_srv_lock_req_clnt_base_encode)
{
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd = arg;
	struct nvmeibc_locks_channel *ch = disk_lock_cmd->ch;
	struct nvmeibc_disk *disk = ch->base.disk;
	struct nvmeib_gen_cmd_lock_param *lock_param = &disk_lock_cmd->lock_param;
	struct volume_client_lock_req_base *lock_req = wire_buf;
	struct lock_seg_info *lsi;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*lock_req) > wire_buf_end);

	if (!(lsi = nvmeibc_disk_get_lock_seg_info(disk,
		ac_to_iac(ch->net.admin_ch)->lsi, lock_param->seg_id))) {
		_NT(vex_nrch_io_srv_lock_req_clnt_base_encode_t11,
			"fail to find the lock_segment_id @INT_ULLONG on the server",
			lock_param->seg_id);
		rv = -ENOENT;
		goto out;
	}

	lock_req->op = lock_param->op;
	lock_req->offset = cpu_to_be64(lock_param->offset);
	lock_req->atomic.compare_add = cpu_to_be64(lock_param->atomic.compare_add);
	lock_req->atomic.swap = cpu_to_be64(lock_param->atomic.swap);
	lock_req->atomic.compare_add_mask = cpu_to_be64(lock_param->atomic.compare_add_mask);
	lock_req->atomic.swap_mask = cpu_to_be64(lock_param->atomic.swap_mask);
	if (link_ext == vex_base) {
		/* If the extension >= vex_ext1 then we use the seg_id instead */
		lock_req->lmi = lsi->lmi;
		if (lock_param->op >= NVMEIB_LOCK_RDMA_READ) {
			_NE(vex_nrch_io_srv_lock_req_clnt_base_encode_t12,
				"unsupported lock op @INT for base extension", lock_param->op);
			rv = -ENOTSUPP;
			goto out;
		}
	}
	rv = sizeof(*lock_req);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_srv_lock_req_clnt_ext1_encode)
{
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd = arg;
	struct nvmeib_gen_cmd_lock_param *lock_param = &disk_lock_cmd->lock_param;
	struct volume_client_lock_req_ext1 *lock_req = wire_buf;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*lock_req) > wire_buf_end);

	if (lock_param->rdma.len > sizeof(lock_req->rdma.data)) {
		_NE(vex_nrch_io_srv_lock_req_clnt_ext1_encode_e1,
			"Invalid rdma size @UINT", lock_param->rdma.len);
		rv = -EPROTO;
		goto out;
	}

	lock_req->seg_id = cpu_to_be64(lock_param->seg_id);
	lock_req->rdma.len = cpu_to_be32(lock_param->rdma.len);
	memcpy(lock_req->rdma.data, lock_param->rdma.data, lock_param->rdma.len);
	lock_req->rdma.table_type = lock_param->rdma.table_type;

	rv = sizeof(*lock_req);

out:
	return rv;
}

static int execute_lock(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk *disk, struct nvmeibc_volume_req_info *info,
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd)
{
	struct nvmeibc_volume_request *req = &info->req;
	struct nvmeib_iu *iu = req->cmd;
	struct volume_client_req *cmd = iu->buf;
	const void *req_buf_end = iu->buf + iu->size;
	struct ib_sge list;
	struct nvmeib_send_wr *wr = info->wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv = -1;
	size_t wire_size;
	const struct vex_ops *vex_ops = net->vex_nrio_ops[vex_nrch_io_srv_lock_req];

	__NFIN;
	memset(cmd, 0, sizeof(*cmd));
	memset(wr, 0, sizeof(*wr));
	req->dcmd = &disk_lock_cmd->disk_cmd;

	rv = CALL_VEX_OP(encode,
				vex_nrch_io_srv_lock_req_clnt_ops, ONE_EXT,
					base, vex_nrch_io_srv_lock_req_clnt_base_encode,
					ext1, vex_nrch_io_srv_lock_req_clnt_ext1_encode,
						vex_ops, NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(cmd),
						req_buf_end, disk_lock_cmd);
	if (rv < 0)
		goto out;

	wire_size = NVMEIBC_VOLUME_CLIENT_REQ_HDR_SIZE() + rv;

	/* JH IOMMU: Changed to "sync for device". Data prepared by CPU for Device to use as source for local RDMA_SEND */
	ib_dma_sync_single_for_device(P2IB(net->base.port), iu->dma, wire_size,
		DMA_TO_DEVICE);

	NVMEIB_INC_TAG_VERSION(info->version);
	info->release_counter = 2;

	_ND(debug_execute_lock, "ch=@CH_PTR index=@INDEX version=@VERSION release_counter=@RELEASE_COUNTER send_counter=@SEND_COUNTER",
		net->base.ioch, info->idx, info->version, info->release_counter, info->send_counter);

	/* set command parameters */
	cmd->hdr.opcode = NVMEIB_CMD;
	cmd->hdr.tag = cpu_to_be64(nordda_tag_encode(&disk->last_tgt_ver, (u32)info->nrch->base.version, info->version, req->index));
	cmd->version_tag = cpu_to_be16(vex_ops->vex_ext);

	BUG_ON(test_and_set_bit(info->idx, info->nrch->req_in_use));

	cmd->client_op = NVMEIBC_CR_LOCK;

	/* send the message */
	list.addr = iu->dma;
	list.length = wire_size;
	list.lkey = net->base.lkey;
	nvmeib_send_wr_common(*wr).opcode = IB_WR_SEND;
	nvmeib_send_wr_common(*wr).wr_id =
		nordda_wr_id_encode(info->version, NVMEIB_SEND_IO, iu->index);
	nvmeib_send_wr_common(*wr).sg_list = &list;
	nvmeib_send_wr_common(*wr).num_sge = 1;
	nvmeib_send_wr_common(*wr).send_flags = IB_SEND_SIGNALED;

#if defined(TAKE_STATS) || defined(MGMT_STATS)
	nvmeib_stats_set_start(&info->send.common);
#endif

	nvmeibc_ib_nordda_channel_req_start_wd(info);
	if (!net->base.dev_cq)
		nvmeibc_ib_net_req_notify_send_cq(&net->base);
	NVMEIBC_LOCK_GUARD_SWITCH_CHECK(locks_free_opr_e7,
		&disk_to_opr(disk_lock_cmd)->bypass_state, LOCK_OPR_USING_BYPASS,
		LOCK_OPR_BYPASS_POSTED);
	if ((rv = nvmeibc_ib_post_send(&net->base, nvmeib_send_wr_to_ib_ptr(*wr),
						   &bad_wr)) < 0) {
		_NE(error_1_ib_net_nordda_execute_lock, "Fail to send io command on nordda channel @RV", rv);
		nvmeibc_ib_nordda_channel_req_stop_wd(info);
		info->release_counter = 0;
		clear_bit(info->idx, info->nrch->req_in_use);
		goto out;
	}
	info->send_counter++;
	nvmeibc_disk_cmd_status_debug(&disk_lock_cmd->disk_cmd,
					NVMEIBC_DISK_CMD_NORDDA_SENT);
#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES) && NVMEIBC_DISK_CMDS_STATS_PROBES==1
	do {
		struct nvmeibc_lock_opr_in_progress *opr_ip = disk_to_opr(disk_lock_cmd);
		struct nvmeibc_d_rdma_comp *comp = disk_to_opr(disk_lock_cmd)->comp;
		unsigned long dt;
		struct nvmeibc_disk_command_probes_try_data *current_try =
			nvmeibc_disk_command_probes_current_try(&comp->probes);
		current_try->llp_post_jif = jiffies;

		dt = current_try->llp_post_jif - current_try->ulp_post_jif;
		if (dt > 1*HZ) {
			_NW(warn_net_nordda_execute_lock_long_pend,
				"@DISK_NAME, NRCH @CH_NAME (@NRCH), LOCK_CH @CH_NAME (@LOCK_CH), opr=@OPR_IP pending for too long @LD",
				disk->name, net->nrch->base.name, net->nrch, opr_ip->ch->base.name, opr_ip->ch, opr_ip, dt);
		}
	} while(0);
#endif

out:
	__NFOUT;
	return rv;
}

int nvmeibc_ib_net_nordda_execute_io(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk *disk, struct nvmeibc_volume_req_info *info,
	struct nvmeibc_disk_command *disk_cmd)
{
	struct nvmeibc_volume_request *req = &info->req;
	int rv = -1;
	bool already_locked = ri_already_locked(info);
	unsigned long flags = 0;
	__NFIN;

	if (already_locked)
		BUG_ON(!req->reused_bb);
	else
		ri_spin_lock_irqsave(info, flags);

	BUG_ON(info->release_counter != 0);

	if (req->dcmd || req->sgcount || req->cmd->n_rdma_iu ||
		req->nmdesc || req->md.local.size) {
		_NE(error_ib_net_nordda_nvmeibc_ib_net_nordda_execute_io, "Prev usage of req @REQ incomplete (@DCMD, @SGCOUNT, @N_RDMA_IU, @NMDESC, @SIZE)",
			req, req->dcmd, req->sgcount, req->cmd->n_rdma_iu,
			req->nmdesc, req->md.local.size);
		BUG_ON(1);
		goto out;
	}
	BUILD_BUG_ON_MSG(sizeof(enum nvmeibc_indirect_op) != 1, "Endianness");

	nvmeibc_ib_net_vol_req_dbgdi_reset((&info->req));
	info->comp_code = NVMEIBC_DISK_CMD_COMP_CODE_INVALID;
	info->recv_comp_arrived = false;

	switch (disk_cmd->cmd_type) {
	case NVMEIBC_DISK_CMD_IO:
		rv = execute_io(net, disk, info, disk_to_block(disk_cmd));
		break;
	case NVMEIBC_DISK_CMD_GEN:
		rv = execute_gen(net, disk, info, disk_to_gen(disk_cmd));
		break;
	case NVMEIBC_DISK_CMD_LOCK:
		rv = execute_lock(net, disk, info, disk_to_lock(disk_cmd));
		break;
	default:
		rv = -1;
		break;
	}
	if (rv < 0) {
		nvmeibc_disk_cmd_status_debug(disk_cmd, NVMEIBC_DISK_CMD_NORDDA_FAILED_SEND);
		_NE(error_1_ib_net_nordda_nvmeibc_ib_net_nordda_execute_io, "Execution failed");
		goto err;
	}
	else {
		disk_cmd->post_jif = jiffies;
		_ND(trace_ib_net_nordda_nvmeibc_ib_net_nordda_execute_io, "Execution success");
	}
	goto out;

err:
	req->dcmd = NULL;

out:
	if (!already_locked)
		ri_spin_unlock_irqrestore(info, flags);
	__NFOUT;
	return rv;
}

/* ------------------------------ gen-cmd ----------------------------------- */

void nvmeibc_ib_net_nordda_unmap_and_unlink_gcmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req, int comp_code)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd = disk_to_gen(req->dcmd);
	NFIN;

	if (gen_cmd->src_ndb.table.sgl) {
		/* JH IOMMU: DMA_TO_DEVICE is correct. Used as source for Local RDMA Write (Input for Gen Cmd to Remote BB) */
		ib_dma_unmap_sg(P2IB(net->base.port), gen_cmd->src_ndb.table.sgl,
						gen_cmd->src_ndb.table.nents, DMA_TO_DEVICE);
	}
	nvmeibc_ib_net_unmap_gen_data(&net->base, req);

	req->dcmd = NULL; /* unlink req from iocmd */
	gen_cmd->comp_code = comp_code;

	NFOUT;
}

void nvmeibc_ib_net_nordda_complete_gcmd(struct nvmeibc_disk_command *dcmd, struct nvmeibc_dev *local_dev)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd = disk_to_gen(dcmd);
	NFIN;
	
	if (local_dev && !gen_cmd->comp_code) {
		/* TBD: Break-down latency into send/recv time using client/target timestamps */
		u64 lat = ktime_after(gen_cmd->recv_comp_time, gen_cmd->send_time) ? 
				ktime_to_ns(ktime_sub(gen_cmd->recv_comp_time, gen_cmd->send_time)) : 0;

		nvmeib_io_stats_adjust_and_update(local_dev->stats, NULL, IO_STAT_VERB_GEN_TX,
				NVMEIBC_NORDDA_CLIENT_GEN_REQ_SIZE + gen_cmd->src_len, lat, false);

		nvmeib_io_stats_adjust_and_update(local_dev->stats, NULL, IO_STAT_VERB_GEN_RX, gen_cmd->recv_sz, lat, false);
	}

	nvmeibc_block_cmd_status_debug(dcmd, NVMEIBC_BLOCK_CMD_COMPLETED);
	nvmeibc_disk_cmd_status_debug(dcmd, NVMEIBC_DISK_CMD_COMPLETED);
	nvmeibc_disk_cmds_stats_llp_complete(gen_cmd->disk, dcmd, STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_GEN_CMD_NO_RDDA, gen_cmd->comp_code);
	nvmeibc_disk_gen_cmd_completion__(gen_cmd); //No-RDDA: send/recv-comp + free-vol-reqs (remove-work)

	NFOUT;
}

void nvmeibc_ib_net_nordda_complete_gen_cmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req, int comp_code)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd = disk_to_gen(req->dcmd);
	NFIN;

	nvmeibc_in_net_warn_on_remote_cmd_wip((&net->base), comp_code);
	nvmeibc_ib_net_nordda_unmap_and_unlink_gcmd(net, req, comp_code);

	if (NVMEIBC_NRCH_DEFER_COMPLETE_IOCMD && gen_cmd->disk_cmd.defer_cb) {
		gen_cmd->disk_cmd.ulp_cb = nvmeibc_ib_net_nordda_complete_gcmd;
	} else {
		nvmeibc_ib_net_nordda_complete_gcmd(&gen_cmd->disk_cmd, net->base.port->nic_dev);
	}

	NFOUT;
}

/* ------------------------------ lock-cmd ---------------------------------- */

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_io_srv_lock_rsp_clnt_base_decode)
{
	const struct volume_server_lock_rsp_base *wire_lock_rsp = wire_buf;
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*lock_rsp) > wire_buf_end);

	lock_rsp->cmp_swap_val = be64_to_cpu(wire_lock_rsp->cmp_swap_val);
	lock_rsp->comp_code = be32_to_cpu(wire_lock_rsp->comp_code);

	return sizeof(*wire_lock_rsp);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_io_srv_lock_rsp_clnt_ext1_decode)
{
	const struct volume_server_lock_rsp_ext1 *wire_lock_rsp = wire_buf;
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*lock_rsp) > wire_buf_end);

	lock_rsp->read_data = wire_lock_rsp->read_data;
	lock_rsp->read_len = be32_to_cpu(wire_lock_rsp->read_len);
	BUG_ON(lock_rsp->comp_code == 0 && lock_rsp->read_len > sizeof(wire_lock_rsp->read_data));

	return sizeof(*wire_lock_rsp);
}

void nvmeibc_ib_net_nordda_unlink_lcmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req)
{
	NFIN;

	(void)(net);
	req->dcmd = NULL; /* unlink req from iocmd */

	NFOUT;
}

void nvmeibc_ib_net_nordda_complete_lcmd(
	struct nvmeibc_disk_lock_cmd *lock_cmd, int comp_code)
{
	struct nvmeibc_disk_command *dcmd = &lock_cmd->disk_cmd;
	NFIN;

	nvmeibc_block_cmd_status_debug(dcmd, NVMEIBC_BLOCK_CMD_COMPLETED);
	nvmeibc_disk_cmd_status_debug(dcmd, NVMEIBC_DISK_CMD_COMPLETED);
	nvmeibc_disk_cmds_stats_llp_complete(lock_cmd->ch->base.disk, dcmd, STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_LOCK_CMD_NO_RDDA, comp_code);
	nvmeibc_locks_channel_lock_cmd_completion(lock_cmd, comp_code, LOCK_OPR_BYPASS_POSTED); //No-RDDA: recv-comp + free-vol-reqs (remove-work)

	NFOUT;
}

void nvmeibc_ib_net_nordda_complete_lock_cmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req, int comp_code)
{
	struct nvmeibc_disk_lock_cmd *lock_cmd = disk_to_lock(req->dcmd);
	NFIN;

	nvmeibc_in_net_warn_on_remote_cmd_wip((&net->base), comp_code);
	nvmeibc_ib_net_nordda_unlink_lcmd(net, req);
	nvmeibc_ib_net_nordda_complete_lcmd(lock_cmd, comp_code);

	NFOUT;
}

int nvmeibc_ib_net_nordda_decode_lock_rsp(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req, struct volume_server_rsp *rsp)
{
	struct nvmeibc_disk_lock_cmd *lock_cmd = disk_to_lock(req->dcmd);
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp = &lock_cmd->lock_rsp;
	const struct vex_ops *vex_ops = net->vex_nrio_ops[vex_nrch_io_srv_lock_rsp];
	int comp_code;
	ssize_t rv;

	NFIN;
	BUG_ON(lock_cmd->lock_param.op >= NVMEIB_LOCK_RDMA_READ && vex_ops->vex_ext == vex_base);

	rv = CALL_VEX_OP(decode,
			vex_nrch_io_srv_lock_rsp_clnt_ops, ONE_EXT,
				base, vex_nrch_io_srv_lock_rsp_clnt_base_decode,
				ext1, vex_nrch_io_srv_lock_rsp_clnt_ext1_decode,
				vex_ops, &rsp->lock_rsp, (const void *)(rsp + 1), lock_rsp);
	if (rv < 0)
		comp_code = rv;
	else
		comp_code = lock_rsp->comp_code;

	NFOUT;
	return comp_code;
}

/* Simulator - server shared code */

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_uj_rsp_clnt_base_decode)
{
	const struct volume_server_gen_rsp_uuid_jour_base *wire_gen_rsp = wire_buf;
	struct nvmeibc_disk_gen_cmd *gen_cmd = arg;

	BUG_ON(wire_buf + sizeof(*wire_gen_rsp) > wire_buf_end);

	gen_cmd->rsp.uj.jour.client_uuid = wire_gen_rsp->uuid;
	gen_cmd->rsp.uj.jour.rng_id = be32_to_cpu(wire_gen_rsp->rng_id);

	if (gen_cmd->rsp.uj.jour.rng_id == NVMEIB_EC_INVALID_JOURNAL_RANGE)
		goto out;

	gen_cmd->rsp.uj.jour.rng_slba = be64_to_cpu(wire_gen_rsp->rng_slba);
	gen_cmd->rsp.uj.jour.rng_nlba = be32_to_cpu(wire_gen_rsp->rng_nlba);
	gen_cmd->rsp.uj.jour.rng_gen_id = be64_to_cpu(wire_gen_rsp->rng_gen_id);

	memcpy(gen_cmd->rsp.uj.jour.serjio_boot_id, wire_gen_rsp->serjio_boot_id, NVMEIB_GID_STR_MAX);
	gen_cmd->rsp.uj.jmdc_len = be32_to_cpu(wire_gen_rsp->jmdc_len);
	gen_cmd->rsp.uj.ent_md_len = be32_to_cpu(wire_gen_rsp->ent_md_len);

	nvmeib_bitmap_from_be32(gen_cmd->rsp.uj.jour.dirty_ents_bitmap,
		NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE,
		wire_gen_rsp->dirty_ents_bitmap);

	nvmeib_bitmap_from_be32(gen_cmd->rsp.uj.jour.abnd_ents_bitmap,
		NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE,
		wire_gen_rsp->abnd_ents_bitmap);

out:
	return sizeof(*wire_gen_rsp);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_uj_rsp_clnt_ext1_decode)
{
	const struct volume_server_gen_rsp_uuid_jour_ext1 *wire_gen_rsp = wire_buf;
	struct nvmeibc_disk_gen_cmd *gen_cmd = arg;

	if (!wire_buf) {
		gen_cmd->rsp.uj.jour.binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY; /* No multi slice */
	} else {
		BUG_ON(wire_buf + sizeof(*wire_gen_rsp) > wire_buf_end);
		gen_cmd->rsp.uj.jour.binje = be32_to_cpu(wire_gen_rsp->binje);
	}

	return sizeof(*wire_gen_rsp);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_uj_rsp_clnt_ext2_decode)
{
	const struct volume_server_gen_rsp_uuid_jour_ext2 *wire_gen_rsp = wire_buf;
	struct nvmeibc_disk_gen_cmd *gen_cmd = arg;

	if (!wire_buf) {
		gen_cmd->rsp.uj.jour.rng_blk = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3;
		gen_cmd->rsp.uj.jour.n_ents = nvmeib_ec_journal_entries_in_range_binje(
			gen_cmd->rsp.uj.jour.binje, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3);
		goto out;
	}

	BUG_ON(wire_buf + sizeof(*wire_gen_rsp) > wire_buf_end);
	gen_cmd->rsp.uj.jour.rng_blk = be32_to_cpu(wire_gen_rsp->rng_blk);
	gen_cmd->rsp.uj.jour.n_ents = be32_to_cpu(wire_gen_rsp->n_ents);

out:
	return sizeof(*wire_gen_rsp);
}


/**
 * Read gen rsp and and fill out gen_cmd's rsp accordingly.
 */
int nvmeibc_ib_net_nordda_decode_gen_rsp(struct nvmeibc_ib_net_nordda *net,
										 struct nvmeibc_disk_gen_cmd *g, struct volume_server_rsp *rsp)
{
	int comp_code;
	const struct vex_ops *vex_ops = net->vex_nrio_ops[vex_nrch_gen_uj_rsp];

	NFIN;
	comp_code = be32_to_cpu(rsp->gen_rsp.comp_code);

	_NT(trace_1_nvmeibc_ib_net_nordda_decode_gen_rsp,
		"@GEN_OP_STR(@GEN_CMD_OP) to @HOSTNAME responded with code @COMP_CODE",
		nvmeib_gen_op_str(g->opcode), g->opcode,
		net->base.ioch->rhost_name, comp_code);
	if (g->opcode == NVMEIB_GEN_OP_GET_UUID_JOUR) {
		if ((CALL_VEX_OP(decode,
					vex_nrch_gen_uj_rsp_clnt_ops, TWO_EXT,
					base, vex_nrch_gen_uj_rsp_clnt_base_decode,
					ext1, vex_nrch_gen_uj_rsp_clnt_ext1_decode,
					ext2, vex_nrch_gen_uj_rsp_clnt_ext2_decode,
					vex_ops, rsp->gen_rsp.payload, rsp + 1, g)) < 0) {
			_NT(trace_nvmeibc_ib_net_nordda_decode_gen_rsp_uj_rsp,
				"Decode failed, revoke comp_code");
			comp_code = -EIO;
		} else {
			// DEBUG:
			if (g->rsp.uj.jour.rng_id != NVMEIB_EC_INVALID_JOURNAL_RANGE &&
				nvmeib_uuid_cmp(g->param.uj.client_uuid, g->rsp.uj.jour.client_uuid) != 0) {
				_NE(error_nvmeibc_ib_net_nordda_decode_gen_rsp_uuid_mismatch,
					"GET_UUID_JOUR Rsp != Req, revoke comp_code! "
				   "(@CLIENT_UUID;@CLIENT_UUID)\n",
				   &g->param.uj.client_uuid, &g->rsp.uj.jour.client_uuid);
				comp_code = -EIO;
			}
		}
	} else if (g->opcode == NVMEIB_GEN_OP_BLKSET_RECOVERED) {
		struct volume_server_gen_rsp_blkset_recovered_base *a = &rsp->gen_rsp.blkset_recovered_rsp;
		g->rsp.br.uuid = a->uuid;
		g->rsp.br.rng_id = be32_to_cpu(a->rng_id);
		g->rsp.br.ent_id = be32_to_cpu(a->ent_id);
		// DEBUG:
		if (memcmp(&g->param.br.uuid, &g->rsp.br.uuid, sizeof(g->rsp.br.uuid)) ||
		    g->param.br.rng_id != g->rsp.br.rng_id || g->param.br.ent_id != g->rsp.br.ent_id) {
			_NE(error_nvmeibc_ib_net_nordda_decode_gen_rsp_uuid_br_failed,
				"BLKSET_RECOVERED Rsp != Req, revoke comp_code! "
			   "(UUID: @CLIENT_UUID;@CLIENT_UUID, rng_id: "
			   "@JRNL_RNG_IDX;@JRNL_RNG_IDX, ent_id: @JRNL_RNG_ENT_IDX;@JRNL_RNG_ENT_IDX\n",
			   &g->param.br.uuid, &g->rsp.br.uuid, g->param.br.rng_id, g->rsp.br.rng_id, g->param.br.ent_id,
			   g->rsp.br.ent_id);
			comp_code = -EIO;
		}
	} else if (g->opcode == NVMEIB_GEN_OP_GET_EC_DB) {
		struct volume_server_gen_rsp_get_dirty_bits_base *a = &rsp->gen_rsp.get_db_rsp;
		g->rsp.db.nlba = be32_to_cpu(a->nlbas);
		g->recv_sz += min_t(size_t, (net->nrch->reqs[0].rn_pages << PAGE_SHIFT), g->param.db.db_dest.local.size);
	} else if (g->opcode == NVMEIB_GEN_OP_FREE_JRNL_ENTS) {
		_NT(trace_nvmeibc_ib_net_nordda_decode_gen_rsp_fje_rsp,
			"Got response @COMP_CODE to Free Journal Entries Command\n", comp_code);
	} else if (g->opcode == NVMEIB_GEN_OP_JENTRY_ERASE) {
		_NT(trace_nvmeibc_ib_net_nordda_decode_gen_rsp_je_rsp,
			"Got response @COMP_CODE to Journal Entry Erase Command\n", comp_code);
	} else {
		_NE(error_nvmeibc_ib_net_nordda_decode_gen_rsp_unknown_rsp,
			"Unexp opcode @GEN_CMD_OP, revoke comp_code!\n", g->opcode);
		comp_code = -EIO;
	}

	NFOUT;

    return comp_code;
}
