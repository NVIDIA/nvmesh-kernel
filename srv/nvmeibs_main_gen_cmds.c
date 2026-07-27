/* nvmeibs_main.c - NVMe IB attached block server driver */
#include "common/kr_incs.h"
#include "../core_unitest/corecomm_injections.h"

#if !defined(BLKDEV_SIMULATOR)
/* Strictly non simulator includes */
#include "nvmeibs_main.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_ib_port.h"
#include "nvmeibs_client.h"
#include "nvmeibs_types.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_disk.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibc_disk.h"
#include "nvmeib_rdma.h"
#include "nvmeib_utils.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib.h"
#include "nvmeib_public.h"
#include "nvmeib_msgloop.h"
#include "nvmeibs_toma.h"
#include "nvmeib_srq.h"
#include "nvmeibs_mcs.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib_public.h"
#include "nvmeibs_serjio.h"
#include "nvmeibs_serjio_gen_cmd_handlers.h"
#include "nvmeibs_um_comm.h"
#include "nvmeibs_nordda_sim_shared.h"
#include "nvmeibs_trace.h"
#include "nvmeibs_async_cookies.h"

void nvmeibs_async_cookie_local_completion(union nvmeibs_async_cookie_ctx *ctx, int rv)
{
	ctx->local.rsp->br.status = rv;
	(*ctx->local.cb)(ctx->local.rsp, rv);
}
#else
	/* Strictly simulator includes */
	#include "nvmeibs_serjio.h"
	#include "nvmeibc_simu_disk.h"
	#include "nvmeibs_serjio_gen_cmd_handlers.h"
	#include "nvmeibs_nordda_sim_shared.h"

	/* Simulator does not deserver cookies */
	struct nvmeibs_async_cookie_params {};
	#define NVMEIBS_INIT_ASYNC_COOKIE_PARAMS(...) ((struct nvmeibs_async_cookie_params){})
	#define nvmeibs_async_cookie_store_get_ch(...) NULL
#endif

int nvmeibs_handle_free_ents_gen_cmd(struct nvmeibs_disk_info *di,
									 const struct nvmeib_gen_cmd_param *p,
									 struct nvmeibs_async_cookie_params *cookie_params)
{
	int rv;
	_NT(trace_s_handle_free_ents_gen_cmd, "NVMEIB_GEN_OP_FREE_JRNL_ENTS (@GEN_CMD_OP)"
		" - SERJIO Boot ID: @SERJIO_BOOT_ID Seg UUID: @SEG_UUID_STR "
		"Recovery SRC: @NVMEIB_RECOV_SRC_STR Num Entries: @N_ENTS "
		"Pass2Toma: @BOOL Lock Entry: @LOCK_ENT_U64 Blkset Num: @BLKSET_NO Blkset LBA: @BLKSET_SLBA",
		NVMEIB_GEN_OP_FREE_JRNL_ENTS,
		p->free_ents.serjio_boot_id, p->free_ents.seg_uuid, nvmeib_recov_src_str(p->free_ents.src),
		p->free_ents.num_ents, p->free_ents.pass2toma, p->free_ents.lock_ent,
		p->free_ents.blkset_num, p->free_ents.blkset_slba);
	if ((rv = nvmeibs_serjio_free_jrnl_ents(di,
			p->free_ents.serjio_boot_id,
			p->free_ents.seg_uuid,
			p->free_ents.src,
			p->free_ents.blkset_slba,
			p->free_ents.num_ents,
			p->free_ents.ents)) < 0)
		goto out;
	if (!p->free_ents.pass2toma)
		goto out;
	if ((rv = nvmeibs_toma_report_event_blkset_recovered(di,
			p->free_ents.seg_uuid,
			p->free_ents.blkset_num,
			p->free_ents.blkset_slba,
			p->free_ents.lock_ent, cookie_params)) < 0)
		goto out;
	rv = NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY; /* Toma will respond asynchronously */
out:
	return rv;
}

int nvmeibs_handle_blkset_recovered_gen_cmd(struct nvmeibs_disk_info *di,
											const struct nvmeib_gen_cmd_param *gen_param,
											union nvmeib_gen_cmd_rsp *gen_rsp, struct nvmeibs_async_cookie_params *cookie_params)
{
	int rv;
	gen_rsp->br.status = -EINVAL; /* Poison */

	_NT(trace_s_handle_blkset_recovered_gen_cmd, "NVMEIB_GEN_OP_BLKSET_RECOVERED (@GEN_CMD_OP)"
		" - Seg UUID: @SEG_UUID_STR Lock Entry: @LOCK_ENT_U64 "
		"Blkset Num: @BLKSET_NO Blkset LBA: @BLKSET_SLBA RangeID: @JRNL_RNG_IDX "
		"Entry: @JRNL_RNG_ENT_IDX Pass2Toma: @BOOL GenID: @JRNL_RNG_GEN_ID",
			NVMEIB_GEN_OP_BLKSET_RECOVERED, gen_param->br.ds_uuid, gen_param->br.lock_ent,
			gen_param->br.blkset_num, gen_param->br.blkset_slba, gen_param->br.rng_id,
			gen_param->br.ent_id, gen_param->br.pass2toma, gen_param->br.gen_id);

	corecomm_inj_code(rv = 0);
	if (corecomm_inj_var(NULL, int, corecomm_inj_serjio_do_send_event, 1))
		rv = nvmeibs_serjio_blkset_recovered(
		    di, gen_param->br.uuid, gen_param->br.rng_id, gen_param->br.ent_id,
		    gen_param->br.blkset_slba, gen_param->br.gen_id);

	if (rv) {
		if (rv == -ENOENT &&
			gen_param->br.ent_id == NVMEIB_EC_INVALID_JOURNAL_ENTRY) {
			/* request was opportunistic, hence entry may not be found */
			rv = 0;
		}
		else {
			_NT(trace_nordda_handle_blkset_recovered_base, "Fail to update jmdc, cuuid=@CLIENT_UUID (rv=@RV), skip toma-event",
			   &gen_param->br.uuid, rv);
			rv = NVMEIBS_IO_RSP_ERR_SERJIO;
			goto out;
		}
	}

	//DEBUG - at this point all data shall be written
	gen_rsp->br.uuid = gen_param->br.uuid;
	gen_rsp->br.rng_id = gen_param->br.rng_id;
	gen_rsp->br.ent_id = gen_param->br.ent_id;

	/* From this point - NOT ALLOWED TO TOUCH gen_rsp */

	if (gen_param->br.pass2toma) {
		rv = nvmeibs_toma_report_event_blkset_recovered(
			di, gen_param->br.ds_uuid, gen_param->br.blkset_num,
			gen_param->br.blkset_slba, gen_param->br.lock_ent, cookie_params);
		if (rv && rv != (int)NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY) {
			rv = NVMEIBS_IO_RSP_ERR_TOMA_EVT;
			goto out;
		}
	}

out:
	if (rv != (int)NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY)
		gen_rsp->br.status = rv; /* Safe to assign as cookie was never sent */
	return rv;
}

int nvmeibs_handle_gen_cmd(struct nvmeib_local_disk *disk, enum nvmeib_gen_cmd_op opcode,
				   const struct nvmeib_gen_cmd_param *p, union nvmeib_gen_cmd_rsp *rsp, u32 cid)
{
	struct nvmeibs_disk_info *di = disk->p;
	int rv;
	(void)cid; /*To shut up the simulator*/
	NFIN;

	if (di->dying) {
		_NT(trace_s_handle_gen_cmd, "Disk @DISK_NAME is dying - failing (@GEN_CMD_OP) FOR CID=@CID",
			di->disk_id, opcode, cid);
		rv = NVMEIBS_IO_RSP_ERR_NO_DISK;
		goto out;
	}

	switch (opcode) {
	case NVMEIB_GEN_OP_JENTRY_ERASE:
		_NT(trace_s_gen_cmd_jentry_erase, "NVMEIB_GEN_OP_JENTRY_ERASE (@GEN_CMD_OP) "
		" - Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID, LBA: @SW_LBA",
			NVMEIB_GEN_OP_JENTRY_ERASE,
			p->je.rng_idx, p->je.ent_erase.ent_idx,
			p->je.rng_gen_id, p->je.ent_erase.ent_idx,
			p->je.ent_erase.ent_swlba);
		rv = nvmeibs_serjio_erase_jam_ent(disk->jrange_handle, p->je.rng_binje, p->je.rng_gen_id,
										  1, &p->je.ent_erase, JAM_ENT_ERASE_REASON_FAILED_WRITE);
		break;
	case NVMEIB_GEN_OP_BLKSET_RECOVERED: {
		/* At this stage do not init cookie channel - it will be init later under lock */
		BUG_ON(!p->async_cb);
		rv = nvmeibs_handle_blkset_recovered_gen_cmd(
			di, p, rsp,
			&NVMEIBS_INIT_ASYNC_COOKIE_PARAMS(cid, true /* local */, NULL,
											.ctx.local.rsp = rsp,
											.ctx.local.cb = p->async_cb,
 											));
	} break;
	case NVMEIB_GEN_OP_GET_UUID_JOUR:
	{
		struct handle_get_uuid_jour_cb_param param = {
			.gen_param = p,
			.gen_rsp = rsp,
		};
		rv = nvmeibs_gen_cmd_handle_get_uuid_jour(di, &param);
	}
	break;
	case NVMEIB_GEN_OP_GET_EC_DB:
		rv = nvmeibs_disk_locks_get_ec_dirty_bytes(di->priv, 0, p->db.lba, p->db.n_lba,
										(struct nvmeib_buffer *)&p->db.db_dest.local,
										p->db.get_dbits, p->db.get_stales,
										p->db.get_full_val, p->db.reserved, &rsp->db.nlba);
		break;
	case NVMEIB_GEN_OP_FREE_JRNL_ENTS: {
		/* At this stage do not init cookie channel - it will be init later under lock */
		BUG_ON(!p->async_cb);
		rv = nvmeibs_handle_free_ents_gen_cmd(
			di, p,
			&NVMEIBS_INIT_ASYNC_COOKIE_PARAMS(cid, true /* local */, NULL,
											.ctx.local.rsp = rsp,
											.ctx.local.cb = p->async_cb,
 											));
	} break;
	case NVMEIB_GEN_OP_LOCK:
		rv = nvmeibs_disk_locks_gen_op_lock(disk, &p->lock_param, &rsp->lock_rsp);
		break;
	case NVMEIB_GEN_OP_GET_JMDC:
		rv = nvmeibs_serjio_gen_cmd_handle_get_jmdc_op(di, p, rsp);
		break;
	default:
		_NE(nvmeibs_handle_gen_cmd_e2, "Unsupported gen command @INT", opcode);
		rv = NVMEIBS_IO_RSP_ERR_INV_OP;
	};
out:
	NFOUT;
	return rv;
}
