/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_simu_disk.h"
#include "nvmeibc_jam.h"
#include "./uni_framework/bunitest_conf.h"

// The includes below are breaking encapsulation concept (Simulator digs into Blocks code). This is done for debug purpose (verify internal states of Block locks).
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.h"
#include "nvmeibs_main_sim.h"
#include "nvmeibs_nic_dma_atomics.h"
#include "nvmeibc_ib_nordda_channel_sim_shared.h"

/*****************************************************************************/
typedef struct {	// Async callback to call upon RDMA / Admin request completion
	eCPU_thread_internal_params;
	int	which;						// which of the callbacks below to use
	union {
		struct nvmeibc_disk_gen_cmd *gen_cmd;
		struct nvmeibc_d_rdma_comp  *lock_comp;
		struct nvmeibc_nic_rspreq 	*jrr;  // The response from JAM -> Serjio
	};
} t_async_cb_params;

static eCPU_cb_ret_type __async_op_cb(eCPU_cb_param_list) {
	t_async_cb_params *p = eCPU_thread_extract_param(t_async_cb_params);
	eCPU_thread_start_execution(p);
	switch (p->which) {
		case 'g': nvmeibc_disk_gen_cmd_completion(p->gen_cmd, p->gen_cmd->comp_code); break;	// Admin channel gen_cmds
		case 'l': p->lock_comp->callback(p->lock_comp, nvmeibc_d_rdma_comp_tag_make()); break;				// Lock channel RDMA
		default: BUG();
	}
	eCPU_thread_end_execution(p);
}

static u32 ram_disk_get_lock_info(struct ramDiskSimulator* ram, int i) {
	union nvmeib_blkset_info rv;
	rv.bits.dirty = ram->dbits[i].all_bits;
	rv.bits.txid  = ram->TxIDs[i];
	return (u64)rv.all;
};

static void ram_disk_set_lock_info(struct ramDiskSimulator* ram, int i, const u64 val) {
	union nvmeib_blkset_info rv = {.all = (u32)val };
	ram->dbits[i].all_bits	= rv.bits.dirty;
	ram->TxIDs[i]			= rv.bits.txid;
};


static void __debug_verify_owner_lock_correct(u64 addr, struct nvmeibc_d_rdma_comp *dc) {
	const struct nvmeibc_cmd_lock *l = (dc->opr == NVMEIBC_LOCK_CMP_AND_SWAP) ? lock_of_bcomp(dc) : get_d_comp_of_pg(dc)->pigbck_lock;
	BUG_ON(l->address != addr);				// Illegal to take few locks atomically
}

int execute_owner_lock(struct ramDiskSimulator* ram, u64 addr, struct nvmeibc_d_rdma_comp *dc) {
	int rv = 0;
	const u64 lock_ind_rel = COMMITTED_ADDR_AS(ram, addr, 4KB, LOCK);
	u32 *curLock = &ram->locks[lock_ind_rel];									// Physical lock we want to manipulate
	struct nvmeibc_d_rdma_comp* action = dc;								// The compare exchange action we want to do.
	u64 action_result;
	BUG_ON(lock_ind_rel>=(u32)ARRAY_SIZE(ram->locks));							// Out of locksets
	__debug_verify_owner_lock_correct(addr, dc);
	action->lockset_id = 0x1117111ULL;										// Daniel: This is a prive transport layer field (slba_blkset). We dont simulate it to prevent block layer from using it.
	spin_lock(&ram->cmpxchg_lock);
	if (ram->state & ramDisk_down) {
		spin_unlock(&ram->cmpxchg_lock);
		action->lock_status = NCL_STATUS_FAIL_NO_COMP;									// There will be no callback
		_NW_dmesg(t0eols, "locks channel paused, request on disk: @RAM_UNIQUEID @DLBA canceled", ram->uniqueID, addr);
		return -EIO;
	}
	BUG_ON(ram->state & ramDisk_no_rdma); 													// Client must never request locks on this disk
	switch (dc->code) {
		case NVMEIBC_CMD_LOCK_UNLOCK:

			union nvmeib_lock_id exchange_lock_id = nvmeibc_d_rdma_comp_get_exchange_lock_id(action);
			BUG_ON(exchange_lock_id.all != 0 && exchange_lock_id.bits.is_stale == 0);
			action_result = cmpxchg(curLock, action->compare, action->exchange);
			if (action_result != action->compare) action->lock_status = NCL_STATUS_CONTENDED; 	// Lock cannot be freed, even though it suppose to belong to the client
			else  								  action->lock_status = NCL_STATUS_TAKEN;
			break;
		case NVMEIBC_CMD_LOCK_OWNER:
			BUG_ON(action->exchange==LS_UNLOCKED);													// Client cannot lock before he got ID from toma
			action_result = cmpxchg(curLock, action->compare, action->exchange);
			if (action_result != action->compare) action->lock_status = NCL_STATUS_CONTENDED; 	// Lock is taken by a different client
			else 			  					  action->lock_status = NCL_STATUS_TAKEN;		// Successfuly acquired the lock
			break;
		case NVMEIBC_CMD_LOCK_COPY_OWNER:
			BUG_NOT_IMPLEMENTED_YET;															// Daniel: Do not enable this until we debug the flow with cmpxchg!!!!
			BUG_ON(action->exchange==LS_UNLOCKED);												// Client cannot lock before he got ID from toma
			action_result = *curLock; *curLock = action->exchange;								// Write, do not compare exchange!
			action->lock_status = NCL_STATUS_TAKEN;												// By definition, cannot be contended
			break;
		case NVMEIBC_CMD_LOCK_READ_PB:
		case NVMEIBC_CMD_LOCK_READ_DR:
			action_result = *curLock;
			action->lock_status = (action_result != LS_UNLOCKED) ? NCL_STATUS_CONTENDED : NCL_STATUS_TAKEN;	// Code of nvmeibc_disk_locks.c, function update_comp_status()
			break;
		default:
			action_result = (~0ULL);
			BUG_NOT_IMPLEMENTED_YET;
			break;
	}
	action->lock.id = action_result;
	action->lock.bi = ram_disk_get_lock_info(ram, lock_ind_rel);
	_ND(t1eols, "Do rdma_op=@RDMA_OP on disk: @RAM_UNIQUEID @DLBA id=@COMPARE-->@EXCHANGE result:@ACTION_RESULT", dc->code, ram->uniqueID, addr, action->compare, action->exchange, action_result);
	spin_unlock(&ram->cmpxchg_lock);
	eCPU_thread_prepare(t_async_cb_params, p);
	p->lock_comp = action; p->which = 'l';
	eCPU_thread_launch(__async_op_cb, p, ut_conf__get_transport()->is_disk_callback_sync);
	return rv;
}


int serverRam_dma_do_db_txid(struct ramDiskSimulator* ram, u64 addr, struct nvmeibc_d_rdma_comp *dc){
	const u64 li = COMMITTED_ADDR_AS(ram, addr, 4KB, LOCK);
	BUG_ON(li>=(int)ARRAY_SIZE(ram->locks));						// Out of locksets

	spin_lock(&ram->cmpxchg_lock);
	if (ram->state & ramDisk_down) {
		spin_unlock(&ram->cmpxchg_lock);
		dc->lock_status = NCL_STATUS_FAIL_NO_COMP;					// There will be no callback
		_NW_dmesg(warn_nic_dma_atomics_serverRam_dma_do_db_txid, "locks channel paused, request on disk: @RAM_UNIQUEID @DLBA canceled", ram->uniqueID, addr);
		return -EIO;
	}
	BUG_ON(ram->state & ramDisk_no_rdma); 							// Client must never request locks on this disk
	switch (dc->opr) {
	case NVMEIBC_LOCK_BLKSET_INFO_WRITE: ram_disk_set_lock_info(ram, li, dc->lock.bi); 		break;
	case NVMEIBC_LOCK_BLKSET_INFO_READ : dc->lock.bi = ram_disk_get_lock_info(ram, li);	BUG();	break;	// Daniel: Even though simulator supports it, it probably should never been used, coz binfo was read when taking lock
	default: BUG_NOT_IMPLEMENTED_YET;
	}
	spin_unlock(&ram->cmpxchg_lock);
	dc->lock_status = NCL_STATUS_TAKEN;								// Alwyas succeeds? Todo: Inject Error
	eCPU_thread_prepare(t_async_cb_params, p);
	p->lock_comp = dc; p->which = 'l';
	eCPU_thread_launch(__async_op_cb, p, ut_conf__get_transport()->is_disk_callback_sync);
	return 0;
}

void serverRam_dma_set_jmdc(struct ramDiskSimulator* D, u64 addr4k, const u64 val) {
	struct serverSimulator *S = serverSimulator_get_srvr_by_nvmedisk(D);
	const u32 jmdci = addr4k - ((D->serjio.jranges_start - D->mem) >> NVMEIBC_SECTOR_SHIFT) - D->committed_addr.block;
	union jblock_md *jmd = &S->jmdc[jmdci];
	BUG_ON(jmdci >= NVMEIB_EC_TOTAL_JOURNAL_BLKS);
	COMMITTED_ADDR(D, addr4k, 4KB);
	if (!nvmeib_is_jmd_io_entry(val)) {		// Clear entry, that was already previously taken by JAM
		if (!nvmeib_is_jmd_io_entry(*jmd)) // Asnc Jam might already reuse the entry so it might not be invalid by the time we get here
			_ND(trace_nic_dma_atomics_serverRam_dma_set_jmdc, "Resetting unused entry: @JMDCI", (int)jmdci);
	}
	jmd->raw = val;
}

/******************* nvmeibc_ib_admin_channel.c functions for JAM *************/
#include "nvmeibc_jam.h"

/* Instead of sending, just return and let caller send the message. */
int nvmeibc_ib_admin_send_rsp(struct nvmeibc_ib_admin_channel *jrr_as_ch, u64 hdr_tag, struct volume_client_rsp *rsp,int rsp_opcode, int wr_opcode, int rsp_len) {
	struct nvmeibc_nic_rspreq *jrr = (void*)jrr_as_ch;
	rsp->hdr.opcode = NVMEIB_RSP;
	rsp->hdr.tag = hdr_tag;
	rsp->opcode = rsp_opcode;
	memcpy(&jrr->rsp, rsp, rsp_len);
	BUG_ON(wr_opcode != NVMEIB_SEND_CFG);
	return 0;
}

/* Simulator: nvmeibc_disk_update_config() is a no-op, so we schedule JAM free-abandoned
 * via nvmeibc_disk_add_work() (disk workqueue) to match real client ordering and avoid
 * eCPU reordering races. */
int nvmeibc_ib_admin_schedule_abnd2free(struct nvmeibc_disk *disk, struct volume_server_req *req)
{
	struct abnd2free *a2f;
	int rv;

	a2f = kzalloc(sizeof(*a2f), GFP_KERNEL);
	if (!a2f) {
		_NE(error_sim_schedule_abnd2free, "OOM: failed to allocate abnd2free");
		return -ENOMEM;
	}
	memcpy(&a2f->jreq, &req->j_req, sizeof(req->j_req));
	a2f->hdr_tag = req->hdr.tag;

	rv = nvmeibc_disk_schedule_abnd2free_work(disk, a2f);
	if (rv)
		kfree(a2f);
	return rv;
}

static eCPU_cb_ret_type __async_serjio_jam_cmd_cb(eCPU_cb_param_list) {		// Todo: Unify with __async_op_cb(), real method is called on admin workqueue
	int rv;
	t_async_cb_params *p = eCPU_thread_extract_param(t_async_cb_params);
	struct volume_server_req *req = &p->jrr->req;
	//struct volume_client_rsp *rsp = &p->jrr->rsp;
	struct serverSimulator *S = p->jrr->S;
	struct nvmeibc_disk* disk = S->disk;
	eCPU_thread_start_execution(p);

	if (atomic_read(&disk->dying) || ((disk)->base.ops.should_pause(&((disk))->base))) // Do not send in case disk is being freed/pausing. This does not prevent a race condition
		goto _out;

	if (req->opcode == NVMEIBS_JAM_ABND2FREE) {
		rv = nvmeibc_jam_process_recv_comp(disk, req); // Send to JAM, Jam does not response to this message
		BUG_ON(rv);
	}

_out:
	sim_kfree(p->jrr);
	eCPU_thread_end_execution(p);
}

void nvmeibc_nic_send_jam_free_abandoned(struct serverSimulator *S, struct nvmeibc_nic_rspreq *rr) {
	eCPU_thread_prepare(t_async_cb_params, p);
	rr->S = S;
	p->jrr = rr;
	eCPU_thread_launch(__async_serjio_jam_cmd_cb, p, ut_conf__get_transport()->is_disk_callback_sync);
}

void serverRam_dma_do_pigback(struct ramDiskSimulator* ram, struct nvmeibc_block_command *bcmd) {
	struct nvmeibc_disk_io_command *cmd = bcmd->iocmd;
	const u64 lengthDisk = cmd->reqs1.ndb->length;
	struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(cmd);
	const u64 i	= COMMITTED_ADDR_AS(ram, cmd->lpb.addr, 4KB, LOCK);
	if (bcmd->my_stage != E_CMDS_STAGE_WRITE_JOURNAL) {	// Addresses of journal block do not match data blocks
		const u64 start_Disk = COMMITTED_ADDR_AS(ram, cmd->reqs1.disk_address, SECTOR, BYTE);
		const u32 i_first =  start_Disk				 /BYTES_IN_LOCKSET;
		const u32 i_last  = (start_Disk+lengthDisk-1)/BYTES_IN_LOCKSET;
		BUG_ON((i_last!=i_first)||(i!=i_first));      	// We piggyback a single blockset only regardless of piggyback type
	}
	switch (dc->opr) {
	case NVMEIBC_LOCK_BLKSET_INFO_WRITE : ram_disk_set_lock_info(ram, i, dc->lock.bi);   break;
	case NVMEIBC_LOCK_READ              : dc->lock.id = ram->locks[i]; dc->lock.bi = ram_disk_get_lock_info(ram, i); break; // No need to cmpxchg. Atomically read the value
	default: BUG_NOT_IMPLEMENTED_YET;
	}
	_ND(trace_nic_dma_atomics_serverRam_dma_do_pigback, "disk[@RAMDISK_UNIQUEID].blockset[@LBA], opr=@OPR_TYPE, val-{@LLX,@LLX}", ram->uniqueID, i, dc->opr, dc->val[0], dc->val[1]);
}

/* Get blockset problems in the given range as bit array: Implementation for nvmeibs:encode_problem_report() */
static u32 __getBInfoProblemsArr(struct ramDiskSimulator *_this, u64 lockset_start, u64 length, u64 *actual_length,
                                 char *res, char get_dbits, char get_stales, char get_full_val) {
	u64 istart  = COMMITTED_ADDR(_this, lockset_start, LOCK), ilength = length, end;
	unsigned rv = 0;
	u64 i,j, maxLockSets = 3;						// Simulate casewhere the core level does not return the whole dirty bits array in one shot. In that case, the upper level will call the core more than one time
	const u64 is_stale = nvmeib_stale_bit_mask_ec.all;

	if (ilength > maxLockSets)
		ilength = maxLockSets;

	if (get_dbits) rv = (ilength*sizeof(union nvmeib_blkset_problem_report));
	else           rv = (ilength*sizeof(union nvmeib_blkset_sparse_report));
	if (res) {
		memset(res, 0, rv);
		spin_lock(&_this->cmpxchg_lock);
		BUG_ON(!get_full_val);
		if (get_dbits) {
			end = (istart + ilength);
			for (i = istart, j = 0; i < end; ++i, ++j) {	// Pack dbits & stale locks int array of u16's
				union nvmeib_blkset_problem_report *p = &((union nvmeib_blkset_problem_report*)res)[j];
				if (!_this->c.bi_inj) {						// Todo: 2 lines below reimplement nvmeib_blkset_problem_report_fill_new()
					p->dbits    = (u16)                     _this->dbits[i].all_bits;
					p->is_stale = (u16)(get_stales && (_this->locks[i] != 0));

				} else {
					*p = _this->c.bi_inj[i];				// External injection
				}
			}
		} else if (get_stales) {						// Pack stale locks only into a sparse array of u64 {blockset addr, lock id}
			end = istart + (int)length;
			for (i = istart, j = 0; i < end && j < ilength; ++i) {
				union nvmeib_blkset_sparse_report *p = &((union nvmeib_blkset_sparse_report*)res)[j];
				if (!_this->c.bi_inj){
					if (_this->locks[i]&is_stale) {
						p->ind = i + _this->committed_addr.lock;
						p->val = _this->locks[i];
						j++;
					}
				} else {
					if (_this->c.bi_inj[i].is_stale) {
						union nvmeib_lock_id other_stale_lock = {.all = SIMULATOR_CLNT_ALLIEN_LOCK_NO_J};
						other_stale_lock.bits.is_stale = 1;
						p->ind = i + _this->committed_addr.lock;
						p->val = other_stale_lock.all;
						j++;
					}
				}
			}
			ilength = j;
		}
		spin_unlock(&_this->cmpxchg_lock);
	}
	*actual_length = ilength;
	return rv;
}

int nvmeibs_disk_locks_get_ec_dirty_bytes(struct nvmeibs_disk_private_data *disk_pd, int seg_id, u64 start, u64 length,
                                          struct nvmeib_buffer *buf, char get_dbits, char get_stales,
                                          char get_full_val, char reserved, u32 *nlbas) {
	//const u64 start = gen_cmd->param.db.addr, length = gen_cmd->param.db.length;
	struct nvmeibc_disk_gen_cmd *gen_cmd = container_of(nlbas, struct nvmeibc_disk_gen_cmd, rsp.db.nlba);
	struct nvmeibc_d_rdma_comp *dc = gen_cmd->ctx;
	struct ramDiskSimulator *ram = container_of(disk_pd, struct ramDiskSimulator, server_disk.pd);
	u64 actual_len = 0, needed_len = 0;
	(void)seg_id; (void)reserved;

	needed_len = __getBInfoProblemsArr(ram, start, length, &actual_len, NULL, get_dbits, get_stales, get_full_val);
	BUG_ON(needed_len > buf->size);

	__getBInfoProblemsArr(ram, start, length, &actual_len, nvmeib_buffer_get_virt(buf, NULL), get_dbits, get_stales, get_full_val);

	*nlbas = (u32)actual_len;
	/* Set special invalid value here to validate it is overriden later */
	dc->dbits_arr.arr = (void *)0xbabababababababa;
	return 0;
}

int nvmeibs_pass_gen_to_nordda_channel(struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gen_cmd) {
	switch (gen_cmd->opcode) {
		case NVMEIB_GEN_OP_GET_UUID_JOUR: gen_cmd->comp_code = nvmeibs_nordda_get_jrng_by_uuid(S, gen_cmd); break;
		case NVMEIB_GEN_OP_JENTRY_ERASE:  gen_cmd->comp_code = nvmeibs_nordda_jentry_erase(    S, gen_cmd); break;
		default: BUG(); break;
	}

	// Callback
	gen_cmd->disk = S->disk;
	eCPU_thread_prepare(t_async_cb_params, p);
	p->gen_cmd = gen_cmd; p->which = 'g';
	eCPU_thread_launch(__async_op_cb, p, gen_cmd->opcode != NVMEIB_GEN_OP_JENTRY_ERASE && ut_conf__get_transport()->is_disk_callback_sync);
	return 0;
}

int nvmeibs_pass_gen_to_server(struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gen_cmd) {
	struct nvmeib_local_disk __disk = {.p = &S->ramDisk.server_disk.di};
	gen_cmd->comp_code = nvmeibs_handle_gen_cmd(&__disk, gen_cmd->opcode, &gen_cmd->param, &gen_cmd->rsp, 0);
	if (gen_cmd->comp_code == (int)NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY)
		gen_cmd->comp_code = 0;
	// Callback
	gen_cmd->disk = S->disk;
	eCPU_thread_prepare(t_async_cb_params, p);
	p->gen_cmd = gen_cmd; p->which = 'g';
	eCPU_thread_launch(__async_op_cb, p, ut_conf__get_transport()->is_disk_callback_sync);
	return 0;
}

int nvmeibs_disk_locks_gen_op_lock(struct nvmeib_local_disk *disk,
	const struct nvmeib_gen_cmd_lock_param *lock_param,
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp)
{
	(void)disk;
	(void)lock_param;
	(void)lock_rsp;
	BUG();
	return -ENOTSUPP;
}

/*****************************************************************************/
// EOF.

