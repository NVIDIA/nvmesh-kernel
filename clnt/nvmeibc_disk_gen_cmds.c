#include "nvmeibc_disk_gen_cmds.h"
#include "nvmeibc_block.h"
#include "nvmeibc_jam.h"
#include "../core_unitest/corecomm_injections.h"

void corecomm_gen_jentry_erase_cb_(struct nvmeibc_disk_gen_cmd *gen_cmd);

/* Gen-cmd completion interface with disk layer
   It is called both for commands that were issued to ib-net
   and for such that were pending and either timed-out, disk-paused,
   or ended-up in wrong channel type (i.e. local or rdda).*/
void nvmeibc_disk_gen_cmd_completion__(
	struct nvmeibc_disk_gen_cmd *gen_cmd)
{
	struct nvmeibc_disk_io_command *iocmd;
	int comp_code = gen_cmd->comp_code;
	bool local_gen = gen_cmd->local_bypass;
	NFIN;

	on_disk_hook(nvmeibc_disk_gen_cmd_completion, gen_cmd->disk, gen_cmd_completion, gen_cmd);

	if (comp_code) {
		_NW(trace_nvmeibc_disk_gen_cmd_completion_error,
			"@STR Generic cmd @GEN_OP_STR (@GEN_CMD_OP) to disk @DISK_ID_STR (at @HOSTNAME) completed with error @COMP_CODE",
			local_gen ? "Local" : "Remote",
			nvmeib_gen_op_str(gen_cmd->opcode), gen_cmd->opcode, gen_cmd->disk->name,
			gen_cmd->disk->disk_host, comp_code);
		atomic64_inc(&gen_cmd->disk->gen_cmds_cntrs_fail[gen_cmd->opcode]);
	} else {
		_NT(trace_nvmeibc_disk_gen_cmd_completion_ok,
			"@STR Generic cmd @GEN_OP_STR (@GEN_CMD_OP) to disk @DISK_ID_STR (at @HOSTNAME) completed successfully",
			local_gen ? "Local" : "Remote",
			nvmeib_gen_op_str(gen_cmd->opcode), gen_cmd->opcode,
			gen_cmd->disk->name, gen_cmd->disk->disk_host);
		atomic64_inc(&gen_cmd->disk->gen_cmds_cntrs_ok[gen_cmd->opcode]);
	}
	if (local_gen)
		atomic64_inc(&gen_cmd->disk->gen_cmds_cntrs_local[gen_cmd->opcode]);
	else
		atomic64_inc(&gen_cmd->disk->gen_cmds_cntrs_remote[gen_cmd->opcode]);

	switch (gen_cmd->opcode) {
	case NVMEIB_GEN_OP_JENTRY_ERASE:
		nvmeibc_disk_cmd_status_debug(&gen_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
		/* Ugly but no other way to inject custom completion for now (TODO: introduce ftrace) */
		corecomm_inj_code_else(corecomm_gen_jentry_erase_cb_(gen_cmd), nvmeibc_jam_jentry_erase_comp(gen_cmd));
		break;
	case NVMEIB_GEN_OP_GET_UUID_JOUR:
		iocmd = gen_cmd->disk_cmd.owner;
		nvmeibc_block_comp_gencmd(&iocmd->comp); //uuid-jour
		break;
	case NVMEIB_GEN_OP_BLKSET_RECOVERED:
		iocmd = gen_cmd->disk_cmd.owner;
		nvmeibc_block_comp_gencmd(&iocmd->comp); //blkset-recovered
		break;
	case NVMEIB_GEN_OP_GET_EC_DB:
		nvmeibc_disk_get_ec_db_comp(gen_cmd, comp_code);
		break;
	case NVMEIB_GEN_OP_FREE_JRNL_ENTS:
		nvmeibc_disk_free_jrnl_ents_info_comp(gen_cmd, comp_code);
		break;
	case NVMEIB_GEN_OP_GET_JMDC:
	{	// RRR: move this chunk of code into a separate function
		struct nvmeibc_disk_jmdc_read_comp *comp = gen_cmd->ctx;
		*comp->rsp.read_jrnl_data = gen_cmd->rsp.jmdc_get.jrnl_data;
		*comp->rsp.rng_data_len = gen_cmd->rsp.jmdc_get.hdr_len;
		*comp->rsp.jmdc_ent_len = gen_cmd->rsp.jmdc_get.ents_len;
		nvmeibc_disk_cmd_status_debug(&gen_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
		nvmeib_buffer_free_sgl(&gen_cmd->param.jmdc_get.jmdc_sink.local);
		kfree(gen_cmd);
		comp->rsp.status = (!comp_code) ? NCL_STATUS_TAKEN : NCL_STATUS_FAIL_COMP;
		comp->callback(comp);
		break;
	}
	default:
		_NE(nvmeibc_disk_gen_cmd_completion_e1,
			"nvmeibc. Unknown cmd_type @INT32_HEX", gen_cmd->opcode);
		break;
	}

	NFOUT;
}

void nvmeibc_disk_gen_cmd_completion(
	struct nvmeibc_disk_gen_cmd *gen_cmd, int comp_code)
{
	gen_cmd->comp_code = comp_code;
	nvmeibc_disk_gen_cmd_completion__(gen_cmd);
}

int nvmeibc_disk_fill_free_jrnl_ents_info(struct nvmeibc_disk *disk,
										   struct nvmeibc_disk_free_jrnl_ents_comp *comp,
                                           struct free_jrnl_ents_info *info)
{
	unsigned int rv;

	NFIN;
	info->disk = disk;
	info->comp = comp;
	info->total_num_ents = comp->num_ents;

	if (!disk->access_local) {
		if (comp->ents_enc_buf_sz > disk->max_gen_cmd_bb) {
			comp->ents_enc_buf_sz = disk->max_gen_cmd_bb;
			BUG_NON_PRODUCTION(5138);
		}
		info->max_ents = (unsigned)(comp->ents_enc_buf_sz / sizeof(struct wire_free_ents_entry));
		if (info->max_ents == 0) {
			_NE(nvmeibc_disk_fill_free_jrnl_ents_info_e1,
				"disk @STR has zero min_gen_cmd_bb value", disk->name);
			rv = -EINVAL;
			goto out;
		}
	} else {
		info->max_ents = ~(unsigned)0;
	}

	BUG_ON(!comp->gen_cmd);
	info->gen_cmd = comp->gen_cmd;

	info->gen_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_GEN;
	info->gen_cmd->disk_cmd.server_side_only = true;

	info->gen_cmd->jiffies_start = jiffies;
	info->gen_cmd->timeout = 2 * HZ;
	info->gen_cmd->ctx = info;

	info->gen_cmd->opcode = NVMEIB_GEN_OP_FREE_JRNL_ENTS;
	memcpy(info->gen_cmd->param.free_ents.serjio_boot_id, comp->serjio_boot_id, NVMEIB_GID_STR_MAX);
	memcpy(info->gen_cmd->param.free_ents.seg_uuid, comp->seg_uuid, NVMEIB_GID_STR_MAX);
	info->gen_cmd->param.free_ents.src = comp->recov_src;
	info->gen_cmd->param.free_ents.blkset_num = comp->blkset_num;
	info->gen_cmd->param.free_ents.blkset_slba = comp->blkset_slba;
	info->gen_cmd->param.free_ents.lock_ent = comp->lock_ent;

	info->gen_cmd->param.free_ents.num_ents = min(comp->num_ents, info->max_ents);

	if (info->gen_cmd->param.free_ents.num_ents == comp->num_ents)
		info->gen_cmd->param.free_ents.pass2toma = comp->pass2toma;

	info->gen_cmd->param.free_ents.ents = comp->ents;
	info->gen_cmd->src = comp->ents_enc_buf;
	info->gen_cmd->src_len = comp->ents_enc_ai.n << PAGE_SHIFT;
	rv = 0;

out:
	NFOUT;
	return rv;
}

void nvmeibc_disk_free_jrnl_ents_info_comp(struct nvmeibc_disk_gen_cmd* gen_cmd, int comp_code) {
	struct free_jrnl_ents_info *info = gen_cmd->ctx; // Daniel: very ugly, rewrite this API!
	unsigned num_ents = gen_cmd->param.free_ents.num_ents;
	BUG_ON(info->start_ent + num_ents > info->total_num_ents);
	if (!comp_code && info->start_ent + num_ents < info->total_num_ents) {
		/* Send next batch */
		info->start_ent += num_ents;
		info->gen_cmd->param.free_ents.ents += num_ents;
		num_ents = min(num_ents, info->total_num_ents - info->start_ent);
		if (info->start_ent + num_ents == info->total_num_ents)
			gen_cmd->param.free_ents.pass2toma = info->comp->pass2toma;
		gen_cmd->param.free_ents.num_ents = num_ents;
		gen_cmd->jiffies_start = jiffies;
		gen_cmd->timeout = 2 * HZ;

		if (!(comp_code = nvmeibc_disk_execute_gen(info->disk, gen_cmd)))
			return;
	}
	nvmeibc_disk_cmd_status_debug(&gen_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
	info->comp->status = (!comp_code) ? NCL_STATUS_TAKEN : NCL_STATUS_FAIL_COMP;

	if (gen_cmd->src_ndb.table.sgl)
		sg_free_table(&gen_cmd->src_ndb.table);

	info->comp->callback(info->comp);
	/* From this point - don't touch gen_cmd, it may no longer exist */
	kfree(info);
}

static void get_db_process_cb(struct get_dirty_bits_ec_info *info, int comp_code)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd = &info->gen_cmd;
	_NT(get_db_process_cb_t1,
		"calling callback  info @PTR   comp @PTR", info, info->comp);
	if (info->comp && info->comp->callback) {
		if (comp_code) {
			_NT(get_db_process_cb_t2,
				"calling ec dirty bits callback  info @PTR   comp @PTR   "
				"comp_code @INT val[0]=0 val[1]=@INT    NCL_STATUS_FAIL_COMP"
				, info, info->comp, comp_code, comp_code);
			info->comp->lock_status = NCL_STATUS_FAIL_COMP;
			info->comp->val[0] = 0;
			info->comp->val[1] = comp_code;
		} else {
			void *addr = gen_cmd->disk->db.virt;
			u32 size = gen_cmd->rsp.db.nlba;
			_NT(get_db_process_cb_t3,
				"calling ec dirty bits callback  info @PTR   comp @PTR   "
				"comp_code @INT array=@PTR size=@UINT   NCL_STATUS_TAKEN"
				,info, info->comp, comp_code, addr, size);
			info->comp->lock_status = NCL_STATUS_TAKEN;
			info->comp->dbits_arr.arr = addr;
			info->comp->dbits_arr.size = size;
		}
		nvmeibc_disk_cmd_status_debug(&gen_cmd->disk_cmd, NVMEIBC_DISK_CMD_COMPLETED);
		info->comp->callback(info->comp);
		_NT(get_db_process_cb_t4,
			"done with dirty bits callback  info @PTR   comp @PTR   "
			"comp_code @INT", info, info->comp, comp_code);
	}
}

static void get_db_process_pending(struct nvmeibc_disk *disk)
{
	struct get_dirty_bits_ec_info *info;
	int rv;
	ulong flags;

	do {
		spin_lock_irqsave(&disk->db.dirty_bits_spinlock, flags);
		disk->db.dirty_bits_already_running = 0;
		if (!(info = list_first_entry_or_null(
			&disk->db.dirty_bits_pending_reqs, struct get_dirty_bits_ec_info, link))) {
			spin_unlock_irqrestore(&disk->db.dirty_bits_spinlock, flags);
			break;
		}

		list_del(&info->link);

		_NT(get_db_process_pending_t1,
			"executing deferred dirty bits request @PTR @PTR", info, disk);
		disk->db.dirty_bits_already_running = 1;
		spin_unlock_irqrestore(&disk->db.dirty_bits_spinlock, flags);
		if ((rv = nvmeibc_disk_execute_gen(disk, &info->gen_cmd)) < 0) {
			_NT(get_db_process_pending_t2, "rv=@INT", rv);
			get_db_process_cb(info, rv);
			kfree(info);
		}
	} while (rv < 0);
}

int nvmeibc_disk_get_db_ec_rl(void *handle, u64 start_addr, u64 len,
	struct nvmeibc_d_rdma_comp *comp)
{
	struct nvmeibc_disk_gen_cmd *gen_cmd;
	struct get_dirty_bits_ec_info *info;
	struct nvmeibc_disk_used_lock_segment *uls = handle;
	struct nvmeibc_disk *disk = uls->disk;
	int rv;
	ulong flags;

	NFIN;
	info = kzalloc(sizeof(*info), GFP_ATOMIC);
	if (!info) {
		_NT(nvmeibc_disk_get_db_ec_rl_t1, "failed allocating info");
		rv = -ENOMEM;
		goto out;
	}

	info->seg_id = uls->seg_id;
	info->start_addr = start_addr;
	info->len = len;
	info->comp = comp;
	info->disk = disk;

	gen_cmd = &info->gen_cmd;
	gen_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_GEN;
	/* init gen-cmd common part */
	gen_cmd->jiffies_start = jiffies;
	gen_cmd->timeout = HZ;
	gen_cmd->ctx = info;

	gen_cmd->param.db.lba = start_addr;
	gen_cmd->param.db.n_lba = len;
	gen_cmd->param.db.get_dbits = comp->dbits_arr_req.get_dbits;
	gen_cmd->param.db.get_stales = comp->dbits_arr_req.get_stales;
	gen_cmd->param.db.get_full_val = comp->dbits_arr_req.get_full_val;
	gen_cmd->param.db.reserved = comp->dbits_arr_req.reserved;
	nvmeib_buffer_init_sgl(&gen_cmd->param.db.db_dest.local,
				disk->db.dirty_bits_mem.sgt.sgl,
				disk->db.dirty_bits_mem.sgt.nents,
				disk->db.len, 0);
	/* init gen-cmd specific part */
	gen_cmd->opcode = NVMEIB_GEN_OP_GET_EC_DB;
	gen_cmd->cpu_mask_info = comp->cpu_mask_info;
	spin_lock_irqsave(&disk->db.dirty_bits_spinlock, flags);
	if (disk->db.dirty_bits_stopping) {
		_NT(nvmeibc_disk_get_db_ec_rl_t20,
			"channel is being stopped");
		rv = -EBUSY;
		goto unlock;
	}
	if (disk->db.dirty_bits_already_running) {
		_NT(nvmeibc_disk_get_db_ec_rl_t30,
			"disk has an inflight dirty bits request - deferring for now @PTR", info);
		list_add_tail(&info->link, &disk->db.dirty_bits_pending_reqs);
		rv = 0;
		goto unlock;
	}

	disk->db.dirty_bits_already_running = 1;
	spin_unlock_irqrestore(&disk->db.dirty_bits_spinlock, flags);
	rv = nvmeibc_disk_execute_gen(disk, gen_cmd);
	if (rv < 0)
		get_db_process_pending(disk);
	goto out;
unlock:
	spin_unlock_irqrestore(&disk->db.dirty_bits_spinlock, flags);
out:
	if (rv < 0) {
		_NT(nvmeibc_disk_get_db_ec_rl_t32, "rv=@INT", rv);
		kfree(info);
	}
	NFOUT;
	return rv;
}

void nvmeibc_disk_get_ec_db_comp(struct nvmeibc_disk_gen_cmd* gen_cmd, int comp_code)
{
	struct get_dirty_bits_ec_info *info;

	NFIN;
	info = (struct get_dirty_bits_ec_info*)gen_cmd->ctx;

	get_db_process_cb(info, comp_code);
	get_db_process_pending(info->disk);
	kfree(info);

	NFOUT;
}

int nvmeibc_disk_fill_gen_op_get_jmdc(struct nvmeibc_disk *disk,
									   struct nvmeibc_disk_jmdc_read_comp *comp,
									   struct nvmeibc_disk_gen_cmd *gen_cmd) {
	int rv;

	/* Init gen command */
	gen_cmd->disk_cmd.cmd_type = NVMEIBC_DISK_CMD_GEN;
	gen_cmd->disk_cmd.server_side_only = true;
	gen_cmd->disk_cmd.cpu_mask_info = &gen_cmd->cpu_mask_info;

	gen_cmd->opcode = NVMEIB_GEN_OP_GET_JMDC;

	gen_cmd->ctx = comp;
	gen_cmd->param.jmdc_get.start_rng = comp->start_rng;
	gen_cmd->param.jmdc_get.num_rng = comp->num_rng;
	gen_cmd->param.jmdc_get.get_dirty_only = comp->dirty_only;
	gen_cmd->param.jmdc_get.get_by_client_uuid = comp->client_uuid_only;
	gen_cmd->param.jmdc_get.client_uuid = comp->client_uuid;

	gen_cmd->param.jmdc_get.rng_data = comp->rsp.rng_data;
	gen_cmd->param.jmdc_get.rng_data_len = *comp->rsp.rng_data_len;
	gen_cmd->param.jmdc_get.ent_md = comp->rsp.ent_md;
	gen_cmd->param.jmdc_get.ent_md_len = *comp->rsp.ent_md_len;

	if (!comp->rsp.jmdc_ent || !comp->rsp.rng_data || !comp->rsp.ent_md)
		gen_cmd->param.jmdc_get.get_len_only = true;

	if ((rv = nvmeib_buffer_alloc_sgl_from_pages(&gen_cmd->param.jmdc_get.jmdc_sink.local,
							comp->jmdc_ent_ai->pages, comp->jmdc_ent_ai->n,
							comp->jmdc_ent_ai->n << PAGE_SHIFT, 0, GFP_ATOMIC)) < 0)
		goto out;

	gen_cmd->data_sink[0] = &gen_cmd->param.jmdc_get.jmdc_sink;
	gen_cmd->n_data_sink = 1;
	gen_cmd->disk = disk;

	rv = 0;
out:
	return rv;
}
