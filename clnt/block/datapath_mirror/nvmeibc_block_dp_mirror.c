/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_mirror.h"
#include "../nvmeibc_block_common.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeibc_volume.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "common/nvmeib_error_report.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_cmd_lock_link.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_profiling_disk_stages.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.h"

/****************************** Kernel BIO Path ******************************
  Acronym
   1. IO path: Each kernel bio is converted to internal operation consisting of
   locks and commands. Each IO is READ/WRITE/TRIM of contigious N blocks.
   A single block IO may become 2 write commands (raid1), an IO of N blocks may
   be split to many commands if striping is present or if it crosses segments.

   2. Basic IO unit is blockset: 32 blocks. For read/write, each Lock protects 1
   blockset and each command can execute IO to 1 blockset. In raid1: 2 locks are
   taken (one on each disk) and 2 commands are issued.

   2.1 Two typical contigious IO's will share one lock. Example: IO on blocks
   [0..23] needs lock0 and next IO on blocks [24..37] needs lock0 and lock1.
   We employ optimization: prev IO will transfer its last lock serve the next
   IO, thus avoiding free and request its first lock. To prevent lock starvation
   on other clients transferring can occur no more then 256 times in a row.

   3. Write in regular mode:
   Each command is protected by 1 owner and 1 or more copy-owners.
   First Owner is requested and then all copy-owners in parallel
   If owner/copy is contended, we set timer to request it again.
   Upon getting completion of all locks we proceed to executing the command
   If all locks were successfully taken, the command executes and we release
   all locks. If at least one lock was not taken (Example: disk is dead) the
   command is auto-failed (not executed).
   R1: one command succeded and its mirror failed, we abandon all locks.
   After completion of both cmds we try to release the locks. Release may fail.
   If all commands succeded IO is considered to be done. Else, IO is rescheduled
   for resubmition once again.
   Timers timeout on locks are treated as if the disk was dead.

   3.1 Write in degraded mode: For 2 locking, same as above but no need for
   copy owner. We start by requesting owner lock immediately. For N-locking
   This is same as above.

   3.2 Write in dual mode: 1 owner, one secondary owner and 0 or more copy owners.
   We ignore the secondary owner and treat it as the regular case. Once owner
   lock is taken we request its secondary owner dual lock.

   4. Read in regular mode / degraded / dual lock.
   Do not take any lock. Perform the command and piggiback on it a request to
   read the owner lock. If it is unlocked - we are done. If it is locked set
   retry timer to reread the lock, until it becomes free. If timed-out,
   reschedule the IO for resubmition. We must test the lock to avoid uncommited
   writes (value was 1, IO X wrote 2, IO Y read value 2, but IO x abandoned its
   locks and 2 was never written). So IO Y read ficticious corrupted value.

   4.1 Read operation which takes locks: Acts as write.

   5. Trim (discard) in regular mode:
   Trim is like a very long write. Each command can be very long and require
   many (K) consecutive owner locks to protect it. Unlike write, we cannot
   wait for acquisition of all the locks because for very large commands its
   inefficient to wait for lock K while holding K-1 other locks and preventing
   other IO's to those read.

    5.1 We start by requesting all the owner locks of the IO (of all
    the commands). Once all the results returned we examine which locks are taken and which are
    conended. If some are contended we split the commands in such a way that
    each contended lock is moved to separate command. This process is called
    prediscard split.
    Long commands can be executed like regular 1 block command because we
    already hold all the needed owner locks.
    The problematic commands (with contended locks) are of length 1 blockset and
    are executed simillarly to the write.

    5.2 Oowner lock has 'ncmds' field where he stores all the commands which intersect with
    it or with it's copy/dual lock. Only when all the commands that owner
    protects complete, the locks are released.

    5.3 - Delete

    5.4 Trim in degraded mode: For 2-locking:
	 If {r1,r2} raid1 pair are in degraded mode and
     {r3, r4} raid1 pair are normal (total of 4 chunks volume), then prediscards
     locks will be issued on r3 and r4. On r1,r2 there will be regular owner
     locks only.
    5.5 Trim in degraded mode: For N-locking. We ignore secondary owner locks.
    Same as in regular case but each owner has to request its secondary.

   6. Additional Locks/commands are documentation here:
https://docs.google.com/document/d/1lfZ1V-VSHW-wX0XeLQc91IehK5b_pdEZhuGBJsJYnr4
   */

/* All cmds which are protected by ownerlock 'lsi' completed. Only last one runs
   this function so no need for atomic calculations */
static inline void __calc_should_abandon_add_cmd(struct t_abandon *aban, const struct nvmeibc_block_command *c)
{
	if (dp_cmds_was_cmd_not_sent(c->o_rv)) {
		aban->wr_not_issued++;
	} else if (c->o_rv != 0) {
		aban->wr_failed++;
	} else {
		aban->wr_succeeded++;
	}
}

static inline void __calc_should_abandon_set_unlock_value(struct nvmeibc_cmd_lock *lo, const struct t_abandon *aban)
{
	if (aban->wr_not_issued && aban->wr_succeeded) {
		NVMEIBC_LOCK_SET_UNLOCK(lo, RELEASE_LOCK__FORCE_ABANDON, RELEASE_LOCK_REASON__ABANDON_WRITE_PARTIAL_SLICE);
	} else if (aban->wr_failed) {
		NVMEIBC_LOCK_SET_UNLOCK(lo, RELEASE_LOCK__FORCE_ABANDON, RELEASE_LOCK_REASON__ABANDON_WRITE_FAILED);
	}	// EC equivalent uses rldr->was_transaction_abandoned
}

void dp_mirror_calc_should_abandon(struct nvmeibc_block_command *cmds, int olsi)
{
	struct nvmeibc_cmd_lock *lo = cmds->locksets + olsi;
	const ulong *CLmat = cmds->o->CLmat;
	struct t_abandon aban = {0};
	int ci;
	if (unlikely(cmds->o->op == NVMEIB_BLOCK_IO_OP_READ))
		return; // Special topology where read takes locks, never abandoned
	for (ci = 0; ci < cmds->ncmds; ci++) {
		if (nvmeibc_clmat_is_linked(CLmat, ci, olsi, cmds->locksets))
			__calc_should_abandon_add_cmd(&aban, &cmds[ci]);
	}
	__calc_should_abandon_set_unlock_value(lo, &aban);
}

/* RDDA transport error codes - kept for compatibility */
#define NVMEIBC_IB_OVEREAGER 0xdeaddead
#define NVMEIBC_IB_OVEREAGER_MAX_REACHED (NVMEIBC_IB_OVEREAGER + 1)
#define dp_cmds_rv_failed_transport(rv) \
	(((rv)==NVMEIBC_IB_OVEREAGER)||((rv)==NVMEIBC_IB_OVEREAGER_MAX_REACHED))

void dp_mirror_calc_comp_state(const struct nvmeibc_block_command *cmds, int *rv, bool *retry)
{
	const struct operation *o = cmds->o;
	const struct nvmeibc_block_command *c = cmds;
	int i;
	*rv = 0;
	*retry = false;

	for (i = 0; i < cmds->ncmds; i++, c++) {
		if (!c->o_rv)
			continue;

		/* Testing the errors is organized by severity */
		nflog(t_1dmccs, "req_id=@REQ_ID_LLONG cmds[@COMMAND_IDX].o_rv=@RV o=@OPERATION", cmds->iocmd->req_id, i, c->o_rv, o);
		if (dp_cmds_rv_failed_ACID(c) || dp_cmds_rv_failed_non_ACID(c) ||
			 dp_cmds_rv_failed_transport((u32)c->o_rv)) {
			nflog(t_2dmccs, "req_id=@REQ_ID_LLONG. retry", c->iocmd->req_id);
			*retry = true;
			continue;
		} else if (c->o_rv > 0) { /* NVME 14bit error, forwarded by transport */
			if (is_transient_disk_error(c->o_rv)) {
				*retry = true;	// On most of errors we retry the IO
				continue;
			} else { /* Unrecoveraqble NVME error */ }
		} else { /* Negative internal unsupported code (software error) */}

		*rv = c->o_rv;	// operation will definitely fail with this error code
		*retry = nvmeibcb_dp_io_fail_mgr_inspect(cmds, i);
		break;
	}
}

static inline int __calc_max_locks_per_blockset(const int max_replicas, enum nvmeib_block_io_op op, const struct nvmeibc_topology *t) {
	const struct nvmeibc_raid1 *first_pr = t->chunks->raid1s;
	if (!first_pr->use_rdma_locks) { 	// In most cases == (max_replicas <= 1), except for jbod with locks datapath (not fully sopport, experimental for elect)
		return 0;						// Entire volume is non mirrored, no need for locks
	} else if ((op == NVMEIB_BLOCK_IO_OP_READ)&&(t->is_safe_for_view_lock)) {	//  At least one part of volume is mirrored
		return 1;						// View 1 (primary owner) lock only
	} else {	 						// For reads 1 is not enough because we might need to lock on read. Maybe this specific praid supports view lock but the topology as a whole does not and we dont know that
		const int max_n_owners = first_pr->lock_scheme.max_n_owners;
		return min(max_replicas, max_n_owners);
	}
}

static void __calc_alloc_counts_for_prepare_op(enum nvmeib_block_io_op op, u64 nlbas, const struct nvmeibc_topology *t, bool is_trim_continous, int *pmax_locks, int *pmax_cmds, int *pmax_trim_split_cmds)
{
	const int nchunks = __get_topo_num_chunks(t);
	const int max_replicas = nvmeibc_topology_get_max_num_replicas(t);
	const int max_blocksets = (nlbas == 1) ? 1 : (nlbas / LOCKSET_SLICES + 2);
	const int max_locks_per_blockset = __calc_max_locks_per_blockset(max_replicas, op, t);
	*pmax_locks = max_locks_per_blockset * max_blocksets;

	*pmax_trim_split_cmds = 0;
	if (unlikely(op == NVMEIB_BLOCK_IO_OP_DISCARD)) {
		const int max_mirrored_cmds  = max_replicas * max_blocksets;
		const int max_merged_cmds = t->stripe_width * max_replicas * nchunks + 1;
		/* One spare for the pre-merging command at the end */
		if (is_trim_continous)
			*pmax_cmds = min(max_mirrored_cmds, max_merged_cmds);
		else
			*pmax_cmds = max_mirrored_cmds; /* Cannot rely on merge */

		if (max_replicas > 1) /* Another set of commands for prediscard */
			*pmax_trim_split_cmds = max_mirrored_cmds; /* After splitting the merged */
	} else if (nvmeib_block_io_op_is_write(op)) {
		*pmax_cmds = max_blocksets * max_replicas;
	} else {
		*pmax_cmds = max_blocksets;
	}
}

static void __calc_alloc_counts_for_1bmd_op(enum nvmeib_block_io_op op, struct bio_extention *bx, const struct nvmeibc_topology *t, int *pmax_locks, int *pmax_cmds, int *pmax_trim_split_cmds)
{
	const int max_replicas = nvmeibc_topology_get_max_num_replicas(t);
	*pmax_locks = __calc_max_locks_per_blockset(max_replicas, op, t);
	*pmax_trim_split_cmds = 0;
	if (op == NVMEIB_BLOCK_IO_OP_READ)
		*pmax_cmds = 1;
	else
		*pmax_cmds = max_replicas + (bx->exec.force_read_b4_write ? 1 : 0);	// Write + pre-read
}

static int __allocate_commands(int max_commands, struct nvmeibc_block_command **cmds, struct operation *o)
{
	NVMESH_BUG(max_commands <= 0, __dump_operation_report, o, "max_commands=%d", max_commands);
	NVMESH_BUG(*cmds != NULL, __dump_operation_report, o, "cmds should have been memset to 0");
	if (o) { // New jbod attempt to reuse operation allocation for embeding cmds/sgls, requires max commands (max_blocksets) + sgl per nlba (or one sgl per discard command)
		const bool discard_op = (o->op == NVMEIB_BLOCK_IO_OP_DISCARD);
		const u32 nlbas = discard_op ? (u32)max_commands : (u32)get_op_nlbas(o);
		const u32 alloc_size = (u32)dp_cmds_calc_size(max_commands) + sizeof(struct scatterlist)*nlbas;
		*cmds = nvmeibc_operation_alloc_from_sufix(o, alloc_size);
		if (*cmds) { // Successfully embeded commands/sgls from operation allocation
			memset(*cmds, 0, alloc_size);
			*cmds = dp_cmds_placement_new(max_commands, *cmds, true);
			o->flags.is_clmat_embedded = true;	// Mark as if we have locks, but when operation is done release the page
		}
	}
	if (*cmds == NULL) {
		*cmds = dp_cmds_kvzalloc(max_commands);
	}
	return (*cmds ? 0 : -ENOMEM);
}

struct bio_vec vv_bio_inter_get_next_bytes(union vv_bio_inter *vbi, const struct operation *o, const u32 bytes)
{
	struct bio_vec bv = __get_bio_vec(o->bios[vbi->nb], &vbi->bi, bytes);
	_ND(t_dg_bvec, "bv.bv_len=@BV_OFFSET", bv.bv_len);

	NVMESH_BUG(vbi->nb >= o->num_bios, __dump_operation_report, (struct operation *)o, "nvmeibc: wrong input, may crash, o=%p, nbio=%d, num bios=%d",
		   o, vbi->nb, o->num_bios);
	if (!bv.bv_len) {							// Advance to next bio
		vbi->nb++;
		vbi->bi = __BI_INIT(o->bios[vbi->nb]);
		bv = __get_bio_vec(o->bios[vbi->nb], &vbi->bi, bytes);
		_ND(t_dh_bvec, "bv.bv_len=@BV_OFFSET, bio[@INT], num_bios=@INT", bv.bv_len, vbi->nb, o->num_bios);
	}
	NVMESH_BUG(bytes != bv.bv_len, __dump_operation_report, (struct operation *)o, "nvmeibc: wrong input bio will cause mem+data corruption. bytes=0x%x, bv_len=0x%x, offset=0x%x, o=%p nbio=%d/%d. Crashing system to debug it\n", bytes, bv.bv_len, bv.bv_offset,o, vbi->nb, o->num_bios);

	if (vbi->on_op_read_use_private_blocks){
		struct page* page = nps_block_iter_page(&vbi->private_read_blocks_iter);
		NVMESH_BUG(bytes != NVMEIBC_SECTOR_SIZE, __dump_operation_report , (struct operation*)o, "bytes=%u", bytes);
		//yes we replace bv, no - not by mistake
		//we would like to simulatate iteration on bio_vec and at the same time
		//supply private block
		bv = (struct bio_vec){.bv_page = page, .bv_len = NVMEIBC_SECTOR_SIZE, .bv_offset = nps_block_iter_offset(&vbi->private_read_blocks_iter)};
		nps_block_iter_advance(&vbi->private_read_blocks_iter, 1);
	}
	return bv;
}

static void __vv_bio_inter_copy_private_read_block_to_bio(union vv_bio_inter *vbi, const struct operation *o)
{
	struct page* page = nps_block_iter_page(&vbi->private_read_blocks_iter);
	struct bio_vec private_bio_vec = (struct bio_vec){.bv_page = page, .bv_len = NVMEIBC_SECTOR_SIZE, .bv_offset = nps_block_iter_offset(&vbi->private_read_blocks_iter)};

	struct bio_vec kernel_bio_vec = __get_bio_vec(o->bios[vbi->nb], &vbi->bi, NVMEIBC_SECTOR_SIZE);
	if (!kernel_bio_vec.bv_len) {							// Advance to next bio
		vbi->nb++;
		vbi->bi = __BI_INIT(o->bios[vbi->nb]);
		kernel_bio_vec = __get_bio_vec(o->bios[vbi->nb], &vbi->bi, NVMEIBC_SECTOR_SIZE);
		_ND(t_dh_bvec_copy, "bv.bv_len=@BV_OFFSET, bio[@INT], num_bios=@INT", kernel_bio_vec.bv_len, vbi->nb, o->num_bios);
	}

	NVMESH_BUG(kernel_bio_vec.bv_len < private_bio_vec.bv_len, __dump_operation_report, (struct operation *)o,
		   "kernel_bio_vec.bv_len=%u, private_bio_vec.bv_len=%u", kernel_bio_vec.bv_len,
		   private_bio_vec.bv_len);
	memcpy(page_address(kernel_bio_vec.bv_page) + kernel_bio_vec.bv_offset, page_address(private_bio_vec.bv_page) + private_bio_vec.bv_offset, private_bio_vec.bv_len);

	_ND(t_dg_bvec_copy, "bv.bv_len=@BV_OFFSET", kernel_bio_vec.bv_len);
	NVMESH_BUG((vbi->nb >= o->num_bios), __dump_operation_report, (struct operation *)o,
		   "nvmeibc: wrong input, may crash, o=%p, nbio=%d, num bios=%d", o, vbi->nb, o->num_bios);
}

void vv_bio_inter_copy_private_read_blocks_to_bio_if_needed(struct operation* o)
{
	u64 i = 0;
	if (nvmeibc_operation_is_bio_copy_needed_for_read(o)){
		union vv_bio_inter vbi;
		vv_bio_inter_init_thick(&vbi, o);
		for (i = 0; i < get_op_nlbas(o); ++i){
			__vv_bio_inter_copy_private_read_block_to_bio(&vbi, o);
			nps_block_iter_advance(&vbi.private_read_blocks_iter, 1);
		}
	}
}

void vv_bio_inter_set_sg_to_bio_page_maybe_copy(union vv_bio_inter *vbi, struct scatterlist *sg, const struct bio_vec bv, bool should_copy)
{
	if (should_copy) {		// Copy the bio content into our pages allocated then set into sgl
		// do not create since it in creation
		BUG_ON(nps_block_iter_nblocks(&vbi->nbi) < 1);
		sg_set_page(sg, nps_block_iter_page(&vbi->nbi), bv.bv_len, nps_block_iter_offset(&vbi->nbi));
		if (vbi->index_in_stage == 0) {	// Only copy for first cmd in praid, since they all share the same buffers
			memcpy(sg_virt(sg), page_address(bv.bv_page) + bv.bv_offset, bv.bv_len);
		}
		nps_block_iter_advance(&vbi->nbi, 1);
	} else {
		sg_set_page(sg, bv.bv_page              , bv.bv_len, bv.bv_offset);
	}
}

static union vv_bio_inter __fill_sg_req_of_cmd_bio(struct nvmeibc_block_io_req *io_req, u64 nlbas, const union vv_bio_inter *first_vbi, struct operation *o)
{
	struct nvmeib_data_buffer *ndb = io_req->ndb;
	struct scatterlist *sg = ndb->table.sgl;
	u64 i;
	int len_left = ndb->length = (nlbas << NVMEIBC_SECTOR_SHIFT);	// sg has 1 4KB block as each entry, even if pages are larger
	union vv_bio_inter rv = *first_vbi;
	const bool should_copy = nvmeibc_operation_is_bio_copy_needed_for_write(o);
	for_each_sg(sg, sg, nlbas, i) {
		const unsigned bytes = min((unsigned)NVMEIBC_SECTOR_SIZE, (unsigned)len_left);
		struct bio_vec bv = vv_bio_inter_get_next_bytes(&rv, o, bytes);
		vv_bio_inter_set_sg_to_bio_page_maybe_copy(&rv, sg, bv, should_copy);
		len_left -= bytes;
		if (len_left <= 0)
			break; /* Stop iteration when nlbas was depleeted. sg list may not be filled entirely if bv_len>block size */
	}
	NVMESH_BUG(len_left || (ndb->table.nents != i + 1), __dump_operation_report, o,
		"nvmeibc: wrong input. len_left=0x%x, nents=0x%x, i=0x%lx, nlbas=0x%lx. Crashing system to debug it\n",
		len_left, ndb->table.nents, (long)i, (long)nlbas);
	return rv;
}

static bool __prepare_mirror_binfo_for_write(struct nvmeibc_block_command *rldr, const enum nvmeib_block_io_op op, int n_cmds, int npreread, const struct nvmeibc_raid1 *r1, u64 nlbas)
{
	struct nvmeibc_dbits_tx raid_d;
	struct nvmeibc_block_command *cur_c, *end = &rldr[n_cmds];
	const bool implicit_sync = (nlbas == LOCKSET_SLICES);

	NVMESH_BUG(!nvmeib_block_io_op_is_write(op), __dump_operation_report, rldr->o, "opearation is not writei: %d", op);

	// Write can only turn on or off (unlikely), will change existing unknown to exact Dbits
	nvmeibc_dbits_tx_init_by_bmp(&raid_d, nvmeibc_raid1_get_protect_lvl(r1),
			nvmeibc_raid1_get_sgmnts_bmp(r1, dbits_on_mask) /* turn_on_dbit_bmp */,
			(implicit_sync ? nvmeibc_raid1_get_sgmnts_bmp(r1, dbits_off_mask) : 0)/* turn_off_dbit_bmp */,
			0 /* turn_on_conv_bmp */);

	if (true/* EC: 1484, Todo Remove and use dp.exec_func_on_locks_tkn as in EC to merge dbits! */) {
		/* Only 1 dirty seg possible. Perform Direct calculation without the
		   need to read old values */
		nvmeibc_dbits_tx_apply(&zero_dbits, &raid_d);
	}

	if (nvmeibc_dbits_tx_has_action(&raid_d)) {		// At least 1 dirty marker in raid,  Piggyback dirtybits if needed
		for (cur_c = &rldr[npreread]; cur_c < end; cur_c++)		// Daniel: This is incorrect !!! Write only to subset of commands?
			dp_cmds_piggyback_dbR1_on_write(cur_c, &raid_d, op);
	}
	return (unlikely(nvmeibc_dbits_has_turn_off(&raid_d)));
}

/* For Read/Writes Finalize the execution plan of this raid */
static void __add_mirr_data_cmds_finish_raid(struct operation *o, int first_cmd, int n_cmds, int npreread, const struct nvmeibc_raid1 *r1, u64 nlbas)
{
	struct nvmeibc_block_command *rldr = &o->cmds[first_cmd], *cur_c, *end = &rldr[n_cmds];
	const enum nvmeib_block_io_op op = o->op;

	rldr->raid_cur_stage = (npreread ? E_CMDS_STAGE_READ_PRE_DATA : E_CMDS_STAGE_DO_IO_AND_PAR);
	rldr->raid_last_stage = E_CMDS_STAGE_DO_IO_AND_PAR;		// Default
	if (op != NVMEIB_BLOCK_IO_OP_READ && (r1->replicas > 1)) { // JBOD doesn't apply here
		const bool has_turn_off = __prepare_mirror_binfo_for_write(rldr, op, n_cmds, npreread, r1, nlbas);		// EC-3043, EC-5969: Todo, change like EC, move this code to be after locks taken. This way we can preserve debug values in TxID for R1 and Actually not create data corruption on N-mirrored
		if (unlikely(has_turn_off)) {
			rldr->raid_last_stage = E_CMDS_STAGE_POST_IO_RDMA;
			rldr->use_io_apend_stages = true;
		}
	}
	rldr->nraid_siblings = n_cmds;
	for (cur_c = rldr; cur_c < end; cur_c++) {
		cur_c->use_stages = true;
		cur_c->my_leader = first_cmd;
		cur_c->raid_first_stage = rldr->raid_cur_stage;	// Copy it to all siblings to save dereference
		if ((cur_c - rldr) >= npreread)
			cur_c->my_stage = E_CMDS_STAGE_DO_IO_AND_PAR;
		else
			cur_c->my_stage = E_CMDS_STAGE_READ_PRE_DATA;
	}
	if (!npreread) {
		nvmeibc_atomic_set(&rldr->n_uncompleted_cmds, n_cmds);	// Default All mirror commands are sent in parallel
	} else {
		nvmeibc_atomic_set(&rldr->n_uncompleted_cmds, npreread);// Start with preread
	}
}

extern u32 nvmeibc_calculate_edic_from_data_and_rlba(const u64 rlba, const unsigned char *data, const bool debug_di_enabled);

static void __fill_compute_metadata_r1(struct nvmeibc_block_command *cmd) {
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	struct nvmeibc_block_io_req *req = &cmd->iocmd->reqs1;
	struct nvmeib_data_buffer *ndb = req->ndb;
	int i, nents = (int)ndb->table.nents;
	struct scatterlist *curr_sg = NULL;
	union nvmeibc_block_dp_ec_data_block_md *md = (union nvmeibc_block_dp_ec_data_block_md *)req->md;
	const u32 mask = NVMEIBC_DP_EC_MD_EDIC_MASK(true); // In mirror we use parity metadata data structures
	u64 rlba = cmd->first_rlba;
	struct nvmeibc_datapath *dp = &cmd->o->nd->dp;

	for_each_sg(ndb->table.sgl, curr_sg, nents, i) {
		int sector_i;
		for (sector_i = 0; sector_i < (int)curr_sg->length; sector_i += NVMEIBC_SECTOR_SIZE) {
			u8 *blk_data = &((u8*)sg_virt(curr_sg))[sector_i];
			const u32 edic = (dp->enable_edic_check) ? (nvmeibc_calculate_edic_from_data_and_rlba(rlba, blk_data, dp->enable_di_debug_mode) & mask) : 0;
			nvmeibc_block_dp_ec_md_make_r1(md, edic);
			rlba += 1;	// Advance to next rlba. (slice_size == 1)
			md = (union nvmeibc_block_dp_ec_data_block_md *)((u8*)md + md_size);
		}
	}
}

static void* __metadata_for_cmd_alloc(struct nvmeibc_block_command *cmd)
{
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	struct nvmeibc_block_io_req *req = &cmd->iocmd->reqs1;
	cmd->is_parity = NVMEIBC_DATA_MD_MIRROR_IS_PARITY;
	req->md = (md_size > 0) ? nvmeibc_alloc_md(cmd->nlbas, md_size) : NULL;			// EC-5473, here imporve
	return req->md;
}

static void __metadata_for_cmd_fill_vals(struct nvmeibc_block_command *cmd, int index_in_stage)
{
	if ((index_in_stage > 0) && nvmeibc_is_mirror_md_enabled(&cmd[-1])) {
		nvmeibc_fill_metadata_from_command(cmd, &cmd[-1]);						// Copy from previous command (much faster than calculating edic from scratch
	} else {
		__fill_compute_metadata_r1(cmd);
	}
	// EC-2038: Add dirty bits to metadata, as a separate stage after write is completed.
}

static bool __is_local_read_optimization_allowed(const struct nvmeibc_raid1 *r1, const struct nvmeibc_block_device *nd)
{
	return (nd->dp.enable_local_read_optimization &&
			nvmeibc_raid1_get_inverse_sgmnts_bmp(r1, rw) == 0);	// perfect topology
}

/* Get the role to read from */
static int __get_role_of_read(const struct nvmeibc_raid1 *r1, const struct nvmeibc_block_device *nd, const u64 rlba, const int slice_start_si)
{
	const int read_si = get_owner_seg_of_read(r1, rlba);
	const int read_role = nvmeibc_raid1_seg2role(r1, slice_start_si, read_si);

	if (__is_local_read_optimization_allowed(r1, nd)) {
		const roles_bmp_t local_access_roles_bmp = nvmeibc_raid1_get_roles_bmp(r1, slice_start_si, local_access);
		if (local_access_roles_bmp != 0) {
			const int local_role = __ffs(local_access_roles_bmp);
			if (read_role != local_role)
				nflog(flog_dp_mirror_mirror_cmds_add_for_raid_local_read,
						"Applying local read optimization: replacing read_role=@ROLE_INT with local_role=@ROLE_INT",
						read_role, local_role);
			return local_role;
		}
	}

	return read_role;
}

static void __fill_sg_req_of_preread(struct nvmeibc_block_io_req *io_req, union vv_bio_inter *first_vbi)
{
	struct nvmeib_data_buffer *ndb = io_req->ndb;
	struct scatterlist *sg = ndb->table.sgl;
	int len_left = ndb->length = NVMEIBC_SECTOR_SIZE;
	NVMESH_BUG(nps_block_iter_nblocks(&first_vbi->nbi) < 1, dump_block_iter, &first_vbi->nbi, "illegal number of blocks");
	sg_set_page(sg, nps_block_iter_page(&first_vbi->nbi), len_left, nps_block_iter_offset(&first_vbi->nbi));
	nps_block_iter_advance(&first_vbi->nbi, 1);
	BUG_ON(ndb->table.nents != 1);
}

/* Add pre-read for read-modify-write flow */
#define __mirror_pre_read_add_for_raid(cur_c, req, seg, ncmds, o, vbi, rlba) ({								\
	(cur_c)->nlbas = 1; dp_cmds_req_fill((o), ncmds, (seg));												\
	(req)->op = NVMEIB_BLOCK_IO_OP_READ;																	\
	if (!nvmeib_get_ndb((cur_c), 1, gfp)) goto _enomem;												\
	__fill_sg_req_of_preread((req), (vbi));																	\
	(cur_c)->first_rlba = rlba;																				\
	(req)->disk_address = (seg)->first_lba + rlba;															\
	if (nvmeibc_is_mirror_md_enabled((cur_c))) if (__metadata_for_cmd_alloc((cur_c)) == NULL) goto _enomem;	\
})

/* Add commands to the given raid. Returns the total amount of commands, updates nlbas and bio iterator
   !use_stages means No split by dma, No dirtybits, nor multi-stage commands*/
static int __mirror_cmds_add_for_raid(const struct nvmeibc_raid1 *r1, u64 rlba, u64 *nlbas, union vv_bio_inter *vbi, struct operation *o, int *pncmds, bool use_stages, int nprereads, const bool first_iter, bool last_iter)
{
	int ncmds = *pncmds;
	u64 nlbas_at_start = *nlbas;
	struct nvmeibc_block_command *cmds = o->cmds;
	const enum nvmeib_block_io_op  op = o->op;
	const bool is_non_mirrored = (r1->replicas == 1);
	const int slice_start_si = get_owner_seg_slice_start(r1, rlba);
	const ulong cmds_roles_bmp =
			(op == NVMEIB_BLOCK_IO_OP_READ) ?
					(1 << __get_role_of_read(r1, o->nd, rlba, slice_start_si)) :			// Read from a single segment
					nvmeibc_raid1_get_inverse_roles_bmp(r1, slice_start_si, dead);	// Write/trim to all non-dead segments
	union vv_bio_inter first_vbi;	// Store the iterator, coz ech cmd of write has to restart from the same point.
	const int first_cmd = ncmds;	// First command in protection raid
	int role, rv = 0;
	const gfp_t gfp = nvmeibc_dp_get_allow_io_gfp_flags();

	if (use_stages) {				// Limit nlbas by dma_size of segments of the R/W. Trim and Cow operations are already split well
		for_each_set_bit(role, &cmds_roles_bmp, r1->replicas) {
			const int si = nvmeibc_raid1_role2seg(r1, slice_start_si, role);
			const struct nvmeibc_disk_segment *seg = &r1->segments[si];
			u64 nlbas_128k;

			*nlbas = min(*nlbas, (u64)seg->max_dma_size);
			*nlbas = min(*nlbas, (u64)NVMEIBS_MAX_IO_CHANNEL_MSGS);
			BUILD_BUG_ON(!DEBUG_MD_EXTD && LOCKSET_SLICES > NVMEIBS_MAX_IO_CHANNEL_MSGS);

			if (seg->max_dma_size != LOCKSET_SLICES && is_non_mirrored)	// YR: TODO: make this happen only for Intel disks!  Today, decision is is_intel_drive = (max_dma_size == 128k)
				continue;

			nlbas_128k = LOCKSET_SLICES - ((seg->first_lba + rlba) & (LOCKSET_SLICES - 1));	// For intel disk limit the command to not cross boundaries or else is suffers great latancy penalty.
			*nlbas = min(*nlbas, nlbas_128k);
		}
	}

	if (likely(vbi)) {
		first_vbi = *vbi; 		// Save iterators for mirrored segs to restart them
	} else {					// Address translation only ioctl, no bio.
		memset(&first_vbi, 0, sizeof(first_vbi));	// Iterator is irrelevant
		NVMESH_WARN(o->num_bios, __dump_operation_report, o, "bios in operation: %d", o->num_bios);
	}

	last_iter &= (nlbas_at_start == *nlbas);	// YR: If this was reduced in the code above, we are not yet at the last iter
	if (nprereads && (first_iter|last_iter)) {	// We need pre-reads at first and/or last blocks
		const u64 preread_maps = get_valid_wrapper_block_maps(o->bios[0]);
		const ulong read_roles_bmp = (1 << __get_role_of_read(r1, o->nd, rlba, slice_start_si));
		const int pre_read_role = find_first_bit(&read_roles_bmp, r1->replicas);
		const int si = nvmeibc_raid1_role2seg(r1, slice_start_si, pre_read_role);
		struct nvmeibc_disk_segment *seg = &r1->segments[si];
		struct nvmeibc_block_command *cur_c = &cmds[ncmds];
		struct nvmeibc_block_io_req *req = &cur_c->iocmd->reqs1;
		nprereads = 0;	// Only first and last rldr have pre-reads all middle rldrs do not
		if (first_iter && __extract_rmw_first_block_map(preread_maps)) {
			__mirror_pre_read_add_for_raid(cur_c, req, seg, ncmds, o, ((vbi != NULL) ? (&first_vbi) : NULL), rlba);
			cur_c++;
			ncmds++;
			nprereads++;
		}
		if (last_iter && __extract_rmw_last_block_map(preread_maps)) {
			req = &cur_c->iocmd->reqs1;
			__mirror_pre_read_add_for_raid(cur_c, req, seg, ncmds, o, ((vbi != NULL) ? (&first_vbi) : NULL), rlba + *nlbas - 1);
			ncmds++;
			nprereads++;
		}
	} else nprereads = 0; // Not relevant for this rldr

	for_each_set_bit(role, &cmds_roles_bmp, r1->replicas) {
		const int si = nvmeibc_raid1_role2seg(r1, slice_start_si, role);
		struct nvmeibc_disk_segment *seg = &r1->segments[si];
		struct nvmeibc_block_command *cur_c = &cmds[ncmds];
		struct nvmeibc_block_io_req *req = &cur_c->iocmd->reqs1;
		cur_c->nlbas = *nlbas;
		cur_c->first_rlba = rlba;
		dp_cmds_req_fill(o, ncmds, seg);
		ncmds++;
		req->op = op;
		req->disk_address = seg->first_lba + rlba;

		if (likely(op != NVMEIB_BLOCK_IO_OP_DISCARD)) {
			const bool is_op_cmp_xcnhg = (!use_stages);						// A bit ugly: !use_stages of Write means cmpxchng write.
			const int index_in_stage = (ncmds - first_cmd - nprereads - 1);	// Technically, only boolean is enough: Is it first cmd or not.
			/* nlbas may be more than needed, we could try and optimize by merging adjacent pages..., especially for striped with small stripe! */
			if (!nvmeib_get_ndb(cur_c, *nlbas, gfp))					// Note: Even though R1 cmds are identical - they cannot share ndb/sgl because both cmds are sent together and sg_dma_address() writes inside 'struct scatterlist'
				goto _enomem;
			if (likely(vbi)) {
				if (use_stages) {					// Mirror Read/Write
					first_vbi.index_in_stage = index_in_stage;
					*vbi = __fill_sg_req_of_cmd_bio(req, *nlbas, &first_vbi,  o);		// Advance vbi to next
				} else {										// Mirror Compare-and-write, Elect project
					NVMESH_BUG(o->flags.need_to_copy_bio, __dump_operation_report, o, "Wrong logic, cannot copy bio aside");
					__fill_sg_req_of_cmd_bio(req, *nlbas, &first_vbi,  o);		// Do not advance vbi, because read/write all use the same first_vib
				}
			}

			if (nvmeibc_is_mirror_md_enabled(cur_c)) {
				if (__metadata_for_cmd_alloc(cur_c) == NULL)
					goto _enomem;
				if ((nvmeib_block_io_op_is_write(op)) && (!is_op_cmp_xcnhg))
					__metadata_for_cmd_fill_vals(cur_c, index_in_stage);	// Else: For read, dont fill anything and for cmpxchng fill after we know the final value to write
			}
		} else {
			rv = __concat_discard_op(cmds, &ncmds, *nlbas, o->nd, is_non_mirrored);
			if (rv < 0)
				goto _out;
		}
		nflog(trace_dp_mirror_mirror_cmds_add_for_raid, "cmds[@COMMAND_IDX]: disk=@DISK nlbas=@NLBAS o=@OPERATION", (int)(cur_c - cmds), nvmeibc_disk_from_base(cur_c->ds->disk), cur_c->nlbas, o);
	}

	if (use_stages) { // Not DISCARD
		__add_mirr_data_cmds_finish_raid(o, first_cmd, (ncmds - first_cmd), nprereads, r1, *nlbas);
	}
_out:
	*pncmds = ncmds;
	return rv;

_enomem:
	_NW(warn_dp_mirror_mirror_cmds_add_for_raid, DMESG_PREFIX("@DEV_NAME") ": Out of memory", o->nd->name);
	rv = -ENOMEM;
	goto _out;
}

static int __prepare_commands(u64 nlbas, u64 start_lba, int c_i, struct nvmeibc_topology *t, struct operation *o, const int ncmdsalloced, const int nprereads)
{
	int ncmds, rv = 0;
	bool first_iter = true;
	struct dp_io_topo_iterator it;
	struct nvmeibc_block_command *cmds = o->cmds;
	const bool inline_sgl = cmds->used_placement_sgl;
	const bool discard = (o->op == NVMEIB_BLOCK_IO_OP_DISCARD);
	union vv_bio_inter vv_bio_ptr;
	vv_bio_inter_init_thick(&vv_bio_ptr, o);
	ncmds = cmds[0].ncmds;
	dp_io_topo_iterator_init(&it, start_lba, nlbas, t, c_i);
	while (dp_io_topo_iterator_next(&it, 'r')) {
		bool last_iter = (it.nlbas == 0) && (it.nlbas_chunk == it.res.nlbas);
		rv = __mirror_cmds_add_for_raid(it.res.r, it.res.rlba, &it.res.nlbas, &vv_bio_ptr, o, &ncmds, (o->op != NVMEIB_BLOCK_IO_OP_DISCARD), nprereads, first_iter, last_iter);
		last_iter = last_iter && (it.res.nlbas == it.nlbas_chunk); // Can vary since we might limit a single command to aligned 128K, even if the raid can take it
		if (unlikely(inline_sgl && !discard && !last_iter && (nlbas > 1))) {// Embeded JBOD IO that has more than 1 non-discard command
			// set next SGL to last used in array (all sgls are in an array, previous command sgl address + number of sgls used by it points to next available sgl)
			cmds[ncmds].iocmd->reqs1.ndb->table.sgl = &cmds[ncmds - 1].iocmd->reqs1.ndb->table.sgl[it.res.nlbas];
		}
		NVMESH_BUG(ncmds > ncmdsalloced, __dump_operation_report, o, "ncmds=%d, ncmdsalloced=%d", ncmds, ncmdsalloced);
		if (rv < 0)
			goto _out;
		first_iter = false;
	}
	//_ND(t_0_cpcmds, ----------- bio @PTR(@INT), cmds=@INT ------",	cmds[0].o->bio, cmds[0].o->bio->bi_vcnt, ncmds);
	nvmeibc_atomic_set(&o->n_uncomp_raids, ncmds);
	if (o->op == NVMEIB_BLOCK_IO_OP_DISCARD) { // All cmds are siblings
		cmds[0].nraid_siblings = ncmds;
		nvmeibc_atomic_set(&cmds[0].n_uncompleted_cmds, ncmds);
	}

_out:
	cmds[0].ncmds = ncmds; // Needed in any case, for dp_cmds_free_all().
	return rv;
}

// Much like __prepare_commands() but for 1[block] md commands (read/write/cmpxchng)
static int __prepare_1bmd_cmds(u64 nlbas, u64 start_lba, int c_i, struct nvmeibc_topology *t, struct operation *o, struct bio_extention *bx)
{
	int ncmds = 0, rv = 0;
	struct dp_io_topo_iterator it;
	struct nvmeibc_block_command *cmds = o->cmds;
	union vv_bio_inter vv_bio_ptr;
	vv_bio_inter_init_thick(&vv_bio_ptr, o);
	dp_io_topo_iterator_init(&it, start_lba, nlbas, t, c_i);
	dp_io_topo_iterator_next(&it, 'r');
	NVMESH_WARN((o->op == NVMEIB_BLOCK_IO_OP_DISCARD) || (nlbas != 1), __dump_operation_report, o, "Unexpected op=%d, nlbas=%llu", o->op, nlbas);
	if (bx->exec.force_read_b4_write) {
		o->op = NVMEIB_BLOCK_IO_OP_READ;
		if ((rv = __mirror_cmds_add_for_raid(it.res.r, it.res.rlba, &it.res.nlbas, &vv_bio_ptr, o, &ncmds, false, 0, 0, 0)) < 0)
			goto _out;
		o->op = NVMEIB_BLOCK_IO_OP_WRITE;	// Note: Here 'vv_bio_ptr' was not advanced by reads so it is till initialized to start
		if ((rv = __mirror_cmds_add_for_raid(it.res.r, it.res.rlba, &it.res.nlbas, &vv_bio_ptr, o, &ncmds, false, 0, 0, 0)) < 0)
			goto _out;
		__add_mirr_data_cmds_finish_raid(o, 0, ncmds, 1, it.res.r, it.res.nlbas);
	} else {
		if ((rv = __mirror_cmds_add_for_raid(it.res.r, it.res.rlba, &it.res.nlbas, &vv_bio_ptr, o, &ncmds, (o->op != NVMEIB_BLOCK_IO_OP_DISCARD), 0, 0, 0)) < 0)
			goto _out;
	}
	nvmeibc_atomic_set(&o->n_uncomp_raids, ncmds);
_out:
	cmds[0].ncmds = ncmds; // Needed in any case, for dp_cmds_free_all().
	if (bx->exec.do_512b_sub_block_x) {
		struct nvmeibc_block_command *c = cmds, *end = &cmds[cmds->ncmds];
		for ( ; c < end; c++) {					// For loop to cover compare exchange (read+write)
			if (nvmeibc_idisk_is_512b_sub_block_x_supported(c->ds->disk)) {
				struct nvmeibc_block_io_req *req = &c->iocmd->reqs1;
				struct scatterlist *sgl = req->ndb->table.sgl;
				req->do_512b_sub_block_x = bx->exec.do_512b_sub_block_x;
				sgl->offset += (do_512b_sub_block_x_val(req->do_512b_sub_block_x)*512);	// Mark sgl that it is sub block
				req->ndb->length = sgl->length = 512;
			}
		}
	}
	return rv;
}

/* split_rv: negative - split failed, 1 - split not needed, 0 - split done */
void relink_trim_split_cmds(struct nvmeibc_cmd_lock *ls, int split_rv)
{
	struct nvmeibc_block_command *new_cmds = ls->new_cmds, *old_cmds = ls->cmds;
	struct operation *o = old_cmds->o;
	if (split_rv != 0) {
		if (unlikely(split_rv < 0))
			_NW_to_user(trace_dp_mirror_relink_trim_split_cmds,  DMESG_PREFIX("@DEV_NAME"), "Volume got trim operation in parallel to write operation to identical lbas. Trim will be much slower. Error code: 1056.", old_cmds->o->nd->name);
		if (new_cmds) {	/* Anyways, get rid of new commands asap */
			dp_cmds_free_all(new_cmds);
			ls->new_cmds = NULL;
			DEBUG_TOPO_CNTRS_op_update_trim(o, ls);
		}
		return /*false*/;
	}
	/* Here all prediscards split completed */
	_ND(trace_1_dp_mirror_relink_trim_split_cmds, "__prediscard_split() done. new_cmds=@NEW_CMDS", new_cmds);
	nvmeibc_cllink_cmds_locksets(new_cmds, ls, NULL);
	_ND(trace_2_dp_mirror_relink_trim_split_cmds, "ls=@LS cmds=@CMDS[@NCMDS] new_cmds=@NEW_CMDS[@NCMDS]", ls,
	   old_cmds, old_cmds->ncmds, new_cmds, new_cmds->ncmds);
	__uncompleted_cmds_list_add(new_cmds->iocmd);
	o->cmds = new_cmds;

	/* Unlink old cmds from operation, get rid of them asap */
	old_cmds->o = NULL;
	old_cmds->locksets = NULL;
	DEBUG_TOPO_CNTRS_op_update_trim(o, ls);
	dp_cmds_free_split(old_cmds);
	DEBUG_TRANSFERS_init_cb_counters(new_cmds->ncmds, new_cmds);
	return /*true*/;
}

static void __send_unprotected_cmds(struct nvmeibc_block_command *c)
{
	struct nvmeibc_block_command *c0 = c;
	const bool isread = (c->o->op == NVMEIB_BLOCK_IO_OP_READ);
	int rv, ci;
	const int ncmds = c->ncmds; /* Copy as it could be destructed in the loop */

	for (ci = 0; ci < ncmds; ci++, c++) {
		if (c->nlocks_take_before_cmd != 0)
			continue; /* Skip locked and uninitialized commands */

		if (!isread) {
			if ((rv = dp_cmds_execute_cmd(c0, ci)) == 0)
				continue;
			if (rv == -EDEAD)
				rv = -EAGAIN; /* If disk dead, we will retry on write/trim */
		} else {
			if ((c0->locksets)&&(dp_cmd_is_raid_leader(c)))	// Only for rldr of N-replica (not jbod). 'c == rldr'!
				dp_cmds_add_readlock_to_rldr(c);	// Todo: Move from execute_op to prepare_op
			if ((rv = dp_cmds_execute_cmd(c0, ci)) == 0)
				continue;
			rv = -EAGAIN; /* Read - overcome any error by retrying the BIO */
		}
		c->iocmd->comp.comp_code = rv;
		dp_cmds_analyze_rv_and_complete(&c->iocmd->comp);
	}
}

static bool __dp_mirror_prepare_op_calc_need_to_copy_bio(struct operation *o, int max_locks)
{
	const enum nvmeib_block_io_op op = o->op;

	if (__is_bio_wrapper_for_bio(o->bios[0]->bio)) {
		return false; //we alreay copied the buffers
	}

	if (nvmeibc_operation_has_bio_extention(o)){
		return false; //extended rider to carrier bio - rider was supposed to handle this
	}

	if (nvmeib_block_io_op_is_write(op)) {
		extern bool nvmeibc_copy_bio_buffers;
		if (!nvmeibc_copy_bio_buffers){
			return false; //no one is changing our buffers
		}
		return max_locks > 0 || o->nd->dp.enable_edic_check; //we need a lock or to maintain the edic
	}

	if (nvmeib_block_io_op_is_read(op)) {
		const int read_has_mutable_bio_buffers = o->nd->dp.read_has_mutable_bio_buffers;
		if( read_has_mutable_bio_buffers == 2){
			return true; //always copy the buffers
		}

		if (read_has_mutable_bio_buffers == 1){
			return o->nd->dp.enable_edic_check;
		}
	}

	return false;
}

/************************** DP virtual functions ******************************/
int dp_mirror_prepare_op(struct operation *o)
{
	struct nvmeibc_topology *t = o->topo;
	const u64 start_lba = get_op_start_lba(o), nlbas = get_op_nlbas(o);
	const char *dev_name = o->nd->name;
	const enum nvmeib_block_io_op op = o->op;
	int pos, i;
	int max_locks, max_cmds, max_new_cmds;
	struct nvmeibc_block_command *new_cmds = NULL;
	//E_PREP_VERIFY
	const bool is_op_write = (nvmeib_block_io_op_is_write(op)), has_bio_ext = nvmeibc_operation_has_bio_extention(o), is_wrapper = __is_bio_wrapper_for_bio(o->bios[0]->bio);
	int rv = __check_layout(nlbas, start_lba, t, is_op_write), nprereads = 0;
	nvmeibc_profiling_start_take_stats(o->nd->preparation_profiler, o);
	if (unlikely(rv)) {
		goto _out;
	}

	for (i = 0; i < o->num_bios; ++i)
		nflog(t_01_r1prp, "start allocating resources: bio=@BIO, op=@BLOCK_IO_OP, s_lba=@VLBA, nlbas=@NLBAS, bio-vec-len=@LEN", o->bios[i], o->op, start_lba, nlbas, o->bios[i]->bio->bi_vcnt);
	//E_PREP_COUNT_CMDS_AND_LOCKS
	if (has_bio_ext && (nlbas == 1)) {
		__calc_alloc_counts_for_1bmd_op(op, o->md_op.bx, t, &max_locks, &max_cmds, &max_new_cmds);
	} else {
		__calc_alloc_counts_for_prepare_op(op, nlbas, t, true /* is_trim_continous */, &max_locks, &max_cmds, &max_new_cmds);
	}
	if (is_wrapper && is_op_write) {	// Check if we require pre-read for read modify write flow
		const u64 preread_maps = get_valid_wrapper_block_maps(o->bios[0]);
		if (unlikely(preread_maps)) {
			if (__extract_rmw_first_block_map(preread_maps)) nprereads++;
			if (__extract_rmw_last_block_map(preread_maps))  nprereads++;
		}
	}
	max_cmds += nprereads;

	//E_PREP_ALLOC
	if (max_new_cmds) {										// Trim which can be potentially split: Has locks, cmds, split_cmds
		const bool use_kv_alloc = true;						// Many cmds, might not fit in kmalloc
		rv |= nvmeibc_clmat_allocate(o, use_kv_alloc, max_locks, max_new_cmds, 0);
		rv |= __allocate_commands(max_cmds    , &o->cmds,  NULL);
		rv |= __allocate_commands(max_new_cmds, &new_cmds, NULL);
	} else if (max_locks) {
		const int n_bytes_for_cmds = dp_cmds_calc_size(max_cmds);		// Attempt to allocate cmds and locks together
		const int n_bytes_for_sgls = (nlbas == 1) ? (int)(max_cmds * nlbas * sizeof(struct scatterlist)) : 0;	// For short ios allocate sgl as well with cmds
		const int n_bytes_total_cmds = n_bytes_for_cmds + n_bytes_for_sgls;
		const bool use_kv_alloc = false;								// Few cmds, no need to vmalloc(), moreover this is not good for sgls
		rv = nvmeibc_clmat_allocate(o, use_kv_alloc, max_locks, max_cmds, n_bytes_total_cmds);
		if (o->locks) {
			o->cmds = dp_cmds_placement_new(max_cmds, ((void *)o->locks) + (rv - n_bytes_total_cmds), (n_bytes_for_sgls != 0));        // Last 'n_bytes_total_cmds' were allocated for cmds
		}
	} else {	// Optimize jbod to reuse operation left over memory
		rv = __allocate_commands(max_cmds, &o->cmds, o);					// Locks dont exist, allocate cmds directly
	}
	if (rv < 0) {
		_NW(t_02_r1prp, DMESG_PREFIX("@DEV_NAME") ": No memory @N_LOCKS locks, @NCMDS cmds, @NCMDS trim cmds", dev_name, max_locks, max_cmds, max_new_cmds);
		goto _out;
	}

	{	// Estimate if bio can change during write and create non atomic effects
		o->flags.need_to_copy_bio = __dp_mirror_prepare_op_calc_need_to_copy_bio(o, max_locks);
		if (o->flags.need_to_copy_bio) {
			rv = nvmeibc_pages_alloc(&o->pages, nlbas, get_tcp_mode_of_operation(o));
			if (rv < 0) {
				_NW(t_05_r1prp, DMESG_PREFIX("@DEV_NAME") ": No memory @N_LOCKS locks, @NCMDS cmds, @NCMDS trim cmds", dev_name, max_locks, max_cmds, max_new_cmds);
				goto _out;
			}
		} else if (nprereads){
			rv = nvmeibc_pages_alloc(&o->pages, nprereads, get_tcp_mode_of_operation(o));
			if (rv < 0) {
				_NW(t_06_r1prp, DMESG_PREFIX("@DEV_NAME") ": No memory @NCMDS pre-read cmds", dev_name, nprereads);
				goto _out;
			}
		}
	}

	o->cmds[0].o = o;
	o->cmds[0].locksets = NULL;
	DEBUG_TOPO_CNTRS_op_constructor(o, o->cmds, new_cmds);
	o->io_stat_length = (u32)nlbas;

	pos = nvmeibc_get_chunk_ind_of_lba(start_lba, t);
	dp_fill_locks_for_io(op, nlbas, start_lba, pos, t, o->locks, max_locks, &o->cpu_mask_info);
	if (has_bio_ext && (nlbas == 1)) {
		rv = __prepare_1bmd_cmds(nlbas, start_lba, pos, t, o, o->md_op.bx);
	} else {
		rv = __prepare_commands( nlbas, start_lba, pos, t, o, max_cmds, nprereads);
	}
	nflog(t_03_r1prp, "done allocating resources: o=@OPERATION, max_locks=@N_LOCKS, rv=@RV, o->locks=@LOCKS, o->cmds=@CMDS", o, max_locks, rv, o->locks, o->cmds);
	if (rv < 0) {
		_NW(t_04_r1prp, DMESG_PREFIX("@DEV_NAME") ": Error preparing commands=@RV", dev_name, rv);
		goto _err_free_new_cmds;
	}
	if (o->locks) {
		nvmeibc_cllink_cmds_locksets(o->cmds, o->locks, new_cmds);
		nvmeibc_atomic_set(&o->locks->n_uncompleted_locks, o->locks->nlocks + LARGE_DEBUG_VALUE);
	}

_out:
	nvmeibc_profiling_end_take_stats(o->nd->preparation_profiler, o, rv);
	return rv;

_err_free_new_cmds:
	dp_cmds_free_all(new_cmds);
	new_cmds = NULL;
	goto _out;
}

int dp_mirror_execute_op(struct operation *o)
{
	struct nvmeibc_block_command *cmds = o->cmds;
	struct nvmeibc_cmd_lock *locksets = o->locks;
	const enum nvmeib_block_io_op op = o->op;
	struct nvmeibc_profiler *prof = nvmeibc_get_raid_good_path_profile_for_rwt_op(cmds->ds, op);
	int i, ncmds = cmds->ncmds, n_unprotected_cmds = 0;	/* Calc amount of unlocked commands */

	for (i=0; i<ncmds; i++) {
		if ((cmds[i].nlocks_take_before_cmd == 0) ||
			nvmeibc_should_use_view_lock(op, cmds[i].ds)){
			cmds[i].nlocks_take_before_cmd = 0; // Send without requesting lock
			n_unprotected_cmds++;
		} // else, Must take lock, coz: cmds[i].nlocks_take_before_cmd > 0
	}

	{
		const bool must_acquire_locks = (n_unprotected_cmds < ncmds);
		const bool profiler_started = nvmeibc_profiling_start_take_stats(prof, o);
		if (locksets) {			// At least 1-Praid with use_rdma_locks==true
			if (op == NVMEIB_BLOCK_IO_OP_READ) { // For simplicity read's might not take locks, but view it so don't pass them
				nvmeibc_operation_compressed_op_dump_bio(o);
				__IO_LT_refuse_transfer(locksets);
			} else { BUG_ON(!must_acquire_locks); }	// Just sanity
		} else {
			BUG_ON(must_acquire_locks);		// Just sanity
			nvmeibc_operation_compressed_op_dump_bio(o);	// All Praids with use_rdma_locks==false, Read/Write/Triem
		}
		if (n_unprotected_cmds) { //  At least 1-Praid with use_rdma_locks==false, or any reads with view lock
			if (profiler_started && !must_acquire_locks) /* op spans two praids, locking profiling gets precedence on unprotected cmds profiling */
				nvmeibc_profiling_start_take_stats_for_stage(prof, o, cmds->raid_cur_stage);
			BLKCMP_IO_ASYNC_AWAIT(__send_unprotected_cmds(cmds));
		}
		if (must_acquire_locks) {
			if (profiler_started)
				nvmeibc_profiling_start_take_stats_for_stage(prof, o, E_CMDS_STAGE_WAIT_FOR_LOCK);
			__uncompleted_cmds_list_send(&cmds->iocmd);
			BLKCMP_IO_ASYNC_AWAIT(dp_locks_send_all(locksets));	// Mirror Write/Trim commands or Reads from unsafe r1's
			nflog(t_1dpr1, "Done sending protected cmds: o=@OPERATION, @N_UNPRO, ncmds=@NCMDS", o, n_unprotected_cmds, ncmds);
		} // Note: here in TRIM: 'cmds' can already be free() if they were split, and relinked
	} // Here: Operation could kfree(), dont access it
	BLKCMP_IO_ASYNC_RESUME_SND(dp_cmds_fiber_execute_1_blockset_state_machine(o, !n_unprotected_cmds)); // Here: Operation could kfree(), dont access it
	return 0; // Meaningless
}

int dp_mirror_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int err) {
	struct nvmeibc_profiler *prof = nvmeibc_get_raid_good_path_profile_for_rwt_op(rldr->ds, rldr->o->op);
	struct dp_io_stats *dp_io_stats = &rldr->o->nd->dp.io_stats;

	if (err != 0) {
		IO_STATS_INCR(dp_io_stats, DP_IO_STATS_LOCKSET_FAILED);
	}
	if (nvmeibc_profiling_end_take_stats_for_stage(prof, rldr->o, E_CMDS_STAGE_WAIT_FOR_LOCK, err))
		nvmeibc_profiling_start_take_stats_for_stage(prof, rldr->o, rldr->raid_cur_stage);
	if (nvmeib_block_io_op_is_write(rldr->o->op)) {
		const struct nvmeibc_datapath *dp = &rldr->o->nd->dp;
		nvmeibc_operation_compressed_op_dump_bio(rldr->o);
		BUG_ON(dp->turn_off_dbits_before_io || dp->enable_care_about_txid);	// Todo: refactor common code with ec of __analyze_binfo_sm_cb_b4j()
	}
	return err;
}

/******************************************************************************/
void dp_mirror_block_completion(struct nvmeibc_d_iocmd_comp *comp, struct nvmeibc_d_iocmd_comp_tag tag)
{
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(comp);
	struct operation *o = cmd->o;
	
	(void)tag;

	if (nvmeibc_is_mirror_md_enabled(cmd))
		nvmeibc_check_metadata_actions(comp);	// If metadata is supported - check it
	if (cmd->iocmd->reqs1.op <= NVMEIB_BLOCK_IO_OP_DISCARD && io_op_is_rwt(o->op)) {
		nvmeibc_profiling_end_take_cmd_stats_for_op(nvmeibc_get_raid_good_path_profile_for_rwt_op(cmd->ds, o->op), cmd->ds->disk_operation_profiler, o, nvmeibc_profiling_get_cmd_stage(cmd->iocmd), cmd, comp->comp_code);
	}
	if (o->op <= NVMEIB_BLOCK_IO_OP_DISCARD) { // Regular: Read / Write / Trim
		dp_cmds_analyze_rv_and_complete(comp);
	} else {
		dp_mirror_sync_cmd_cb(cmd);				// Syncs
	}
}

static void __copy_the_preread_into_the_dst_by_map(struct nvmeibc_block_command *dst_cmd, struct nvmeibc_block_command *src_cmd,
												   const ulong map, const bool is_first_block, const bool enable_edic_check, const bool enable_di_debug_mode)
{
	struct scatterlist *dst_sg = dst_cmd->iocmd->reqs1.ndb->table.sgl;
	u8 *dst_data;
	u8 *src_data = sg_virt(src_cmd->iocmd->reqs1.ndb->table.sgl);

	if (!is_first_block) { // Go to last sg
		int nlbas = dst_cmd->nlbas;
		while (--nlbas) {	// Get the last sg
			dst_sg = sg_next(dst_sg);
		}
	}

	dst_data = sg_virt(dst_sg);
	__copy_sub_block_by_map(dst_data, src_data, map);

	if nvmeibc_is_mirror_md_enabled(dst_cmd) {	// Now calculate EDIC for this block if required
		union nvmeibc_block_dp_ec_data_block_md *dst_md = (union nvmeibc_block_dp_ec_data_block_md *)dst_cmd->iocmd->reqs1.md;
		if (!is_first_block) {
			const u32 dst_md_size = nvmeibc_sgmnt_sw_md_size(dst_cmd->ds);
			dst_md = (union nvmeibc_block_dp_ec_data_block_md *)((u8*)dst_md + dst_md_size*(dst_cmd->nlbas-1));
		}

		if (enable_edic_check) {
			const u32 mask = NVMEIBC_DP_EC_MD_EDIC_MASK(true); // In mirror we use parity metadata data structures
			const u32 edic = (nvmeibc_calculate_edic_from_data_and_rlba(src_cmd->first_rlba, dst_data, enable_di_debug_mode) & mask);
			dst_md->P.edic = edic;
		} else {
			dst_md->P.edic = 0;
		}

	} else {
		// get to operation and dump
		NVMESH_BUG(enable_edic_check, __dump_operation_report, dst_cmd->o, "nvmeibc: mirror disabled, but we have edic check enabled.");
	}
}

static void __copy_block_and_edic(struct nvmeibc_block_command *dst_cmd, struct nvmeibc_block_command *src_cmd, bool first_block, int nprereads, const bool enable_edic_check)
{
	struct scatterlist *dst_sg = dst_cmd->iocmd->reqs1.ndb->table.sgl;
	struct scatterlist *src_sg = src_cmd->iocmd->reqs1.ndb->table.sgl;
	const bool md_required = nvmeibc_is_mirror_md_enabled(src_cmd);
	const bool both = (nprereads > 1);
	//get to operation and dump
	NVMESH_BUG(md_required != nvmeibc_is_mirror_md_enabled(dst_cmd), __dump_operation_report, dst_cmd->o, \
		   "source md required (%d) differ from ds md required (%d)", md_required, \
		   nvmeibc_is_mirror_md_enabled(dst_cmd));
	if (both || first_block) {
		u8 *dst_data = sg_virt(dst_sg);
		u8 *src_data = sg_virt(src_sg);
		memcpy(dst_data, src_data, NVMEIBC_SECTOR_SIZE);
		if (md_required) {
			union nvmeibc_block_dp_ec_data_block_md *src_md = (union nvmeibc_block_dp_ec_data_block_md *)src_cmd->iocmd->reqs1.md;
			union nvmeibc_block_dp_ec_data_block_md *dst_md = (union nvmeibc_block_dp_ec_data_block_md *)dst_cmd->iocmd->reqs1.md;
			dst_md->P.edic = src_md->P.edic;
			if (!enable_edic_check) {
				NVMESH_BUG(dst_md->P.edic | src_md->P.edic, __dump_operation_report, dst_cmd->o,
					   "EDIC mismatch dst edic=%x src edic=%x", dst_md->P.edic,
					   src_md->P.edic);
			}
		}
	}
	if (both || !first_block) {
		const int nlbas = dst_cmd->nlbas-1;
		int i = 0;
		while (i++ < nlbas) {
			dst_sg = sg_next(dst_sg);
			src_sg = sg_next(src_sg);
		}
		{
			u8 *dst_data = sg_virt(dst_sg);
			u8 *src_data = sg_virt(src_sg);
			memcpy(dst_data, src_data, NVMEIBC_SECTOR_SIZE);
			if (md_required) {
				const u32 src_md_size = nvmeibc_sgmnt_sw_md_size(src_cmd->ds);
				const u32 dst_md_size = nvmeibc_sgmnt_sw_md_size(dst_cmd->ds);
				union nvmeibc_block_dp_ec_data_block_md *src_md = (union nvmeibc_block_dp_ec_data_block_md *)((u8*)src_cmd->iocmd->reqs1.md + src_md_size*nlbas);
				union nvmeibc_block_dp_ec_data_block_md *dst_md = (union nvmeibc_block_dp_ec_data_block_md *)((u8*)dst_cmd->iocmd->reqs1.md + dst_md_size*nlbas);
				dst_md->P.edic = src_md->P.edic;
				if (!enable_edic_check) {
					NVMESH_BUG(dst_md->P.edic | src_md->P.edic, __dump_operation_report, dst_cmd->o,
						   "EDIC mismatch dst edic=%x src edic=%x",
						   dst_md->P.edic, src_md->P.edic);
				}
			}
		}
	}
}

void dp_mirror_exec_func_on_stage_end(struct nvmeibc_block_command *rldr, int *rv)
{
	// Note: rldr->raid_cur_stage == E_CMDS_STAGE_DO_IO_AND_PAR)
	if (*rv) rldr->o_rv = *rv;

	if (rldr->raid_cur_stage == E_CMDS_STAGE_READ_PRE_DATA) {	// For sub-block write we have pre-reads
		// Copy the pre-read data for the first block and the last block
		const u64 preread_maps = get_valid_wrapper_block_maps(rldr->o->bios[0]);
		int nprereads = 0;
		// go for operation

		NVMESH_BUG(!__is_bio_wrapper_for_bio(rldr->o->bios[0]->bio), __dump_operation_report, rldr->o,
			   "bio not wrapped o=%p, num bios=%d", rldr->o, rldr->o->num_bios);
		NVMESH_BUG(!nvmeib_block_io_op_is_write(rldr->o->op), __dump_operation_report, rldr->o,
			   "bio not for write o=%p, num bios=%d op=%d", rldr->o, rldr->o->num_bios, rldr->o->op);
		NVMESH_BUG(!preread_maps, __dump_operation_report, rldr->o, "preread false");
		if (preread_maps) {	// First update one dst_cmd, then copy to the remaining
			const bool enable_di_debug_mode = rldr->o->nd->dp.enable_di_debug_mode;
			const bool enable_edic_check = rldr->o->nd->dp.enable_edic_check;
			struct nvmeibc_block_command *src_cmd = NULL;
			struct nvmeibc_block_command *dst_cmd = &rldr[dp_cmds_get_first_cmd_of_stage(rldr, E_CMDS_STAGE_DO_IO_AND_PAR)];
			ulong map = __extract_rmw_first_block_map(preread_maps);
			bool first_block = true;	// Assume first block, otherwise override
			NVMESH_BUG(enable_di_debug_mode, __dump_operation_report, rldr->o, "DI debug mode enabled");	// TODO - Need to check if 512B is enough for debug DI payload, will also cause changes to storing them for these volumes
			if (map && (rldr->my_leader == 0)) {
				src_cmd = &rldr[nprereads++];
				__copy_the_preread_into_the_dst_by_map(dst_cmd, src_cmd, map, first_block, enable_edic_check, enable_di_debug_mode);
			}
			map = __extract_rmw_last_block_map(preread_maps);
			if (map && ((&rldr->cmdarr[rldr->cmdarr->ncmds] - rldr) == rldr->nraid_siblings)) {
				src_cmd = &rldr[nprereads++];
				first_block = false;
				__copy_the_preread_into_the_dst_by_map(dst_cmd, src_cmd, map, first_block, enable_edic_check, enable_di_debug_mode);
			}
			if (nprereads) { // We might be in a rldr without pre-reads
				src_cmd = dst_cmd;
				for (dst_cmd++; dst_cmd < &rldr[rldr->nraid_siblings]; dst_cmd++) {
					__copy_block_and_edic(dst_cmd, src_cmd, first_block, nprereads, enable_edic_check);
				}
			}
		}
		rldr->raid_cur_stage = E_CMDS_STAGE_DO_IO_AND_PAR; // Move to DO_IO now that the writes are ready to be sent
	} else
		rldr->raid_cur_stage++; // Just advance to next stage
}

int dp_mirror_should_ignore_op(const struct operation *o)
{
	if (o->op == NVMEIB_BLOCK_IO_OP_DISCARD) {	// Ignore discards for 512B device (we can't issue them with or without the wrapper)
		if (unlikely(nvmeibc_block_is_kernel_sector_io_allowed(o->nd)))
			return 1;
	} else if (unlikely(!is_io_aligned(o->bios[0])))	// Read/Write BIOs that are unaligned are invalid
		return -EINVAL;
	return false;	/* Nothing is ignorred */
}

void dp_mirror_complete_locks_debug_val(struct nvmeibc_cmd_lock *locksets)
{
	#ifdef BLKCMP_IO_COMPLETION_PRESERVE_STACK
		dp_locks_free_all(locksets);	// No need for atomics to wait for last usage. Now is the last one.
	#else
		dp_locks_complete_lock(locksets, LARGE_DEBUG_VALUE, -1, true);
	#endif
}

static int __translate_addr_by_cfg(struct dp_block_translation_unit *tu)
{
	int rv = 0;
	struct nvmeibc_topology *t;
	struct dp_io_topo_iterator it;
	const struct nvmeibc_raid1 *pr;
	struct t_dp_block_trans_input  *_inp = &tu->input;
	struct t_dp_block_trans_output *_out = &tu->output;
	int i, slice_start_si;

	NFIN;
	t = nvmeibc_topology_get(&_inp->nd->topologies);
	if (unlikely(__check_layout(_inp->nlbas, _inp->vlba, t, false) || (_inp->nlbas > 1))) {
		rv = -ENXIO;
		goto _out;		// Todo: Support vlba range
	}
	_out->ci = nvmeibc_get_chunk_ind_of_lba(_inp->vlba, t);
	dp_io_topo_iterator_init(&it, _inp->vlba, _inp->nlbas, t, _out->ci);
	dp_io_topo_iterator_next(&it, 'r');
	pr = it.res.r;
	_out->ri = (int)(pr - t->chunks[_out->ci].raid1s);
	_out->io_perm = nvmeibc_raid1_get_io_perm(t, _out->ci, _out->ri);
	if (pr->replicas > (int)ARRAY_SIZE(_out->disks)) {
		rv = -EINVAL;
		goto _out;
	}
	slice_start_si = get_owner_seg_slice_start(pr, it.res.rlba);
	_out->n_cmds = pr->replicas;
	for (i = 0; i < pr->replicas; i++) {
		const int role = nvmeibc_raid1_seg2role(pr, slice_start_si, i);
		struct nvmeibc_disk_segment *seg = &pr->segments[i];
		_out->disks[i] = seg->disk;
		_out->offs[ i] = seg->first_lba + it.res.rlba;
		_out->descr[i] = ((role == 0) ? "Data" : "Mirror");
	}

_out:
	nvmeibc_topology_put(t);
	NFOUT;
	return rv;
}

static int __translate_addr_by_topology(struct dp_block_translation_unit *tu)
{
	int rv = 0;
	struct nvmeibc_topology *t;
	struct dp_io_topo_iterator it;
	const struct nvmeibc_raid1 *pr;
	int i, ncmds, slice_start_si;
	int max_locks, max_cmds, max_new_cmds;
	struct operation o = {0};
	struct t_dp_block_trans_input  *_inp = &tu->input;
	struct t_dp_block_trans_output *_out = &tu->output;

	NFIN;
	t = nvmeibc_topology_get(&_inp->nd->topologies);
	if (unlikely(__check_layout(_inp->nlbas, _inp->vlba, t, false))) {
		rv = -ENXIO;
		goto _out;
	}

	o.op = _inp->op;
	o.nd = _inp->nd;

	_out->ci = nvmeibc_get_chunk_ind_of_lba(_inp->vlba, t);
	dp_io_topo_iterator_init(&it, _inp->vlba, _inp->nlbas, t, _out->ci);
	dp_io_topo_iterator_next(&it, 'r');
	pr = it.res.r;
	slice_start_si = get_owner_seg_slice_start(pr, it.res.rlba);
	_out->ri = (int)(pr - t->chunks[_out->ci].raid1s);

	_out->io_perm = nvmeibc_raid1_get_io_perm(t, _out->ci, _out->ri);
	if (nvmeibc_topo_is_invtopo_state(_out)) {
		rv = -EIO;
		goto _out;		// Warning, translation is hazardous without a valid topo, may crash the system
	}

	/* Generate commands that will not be sent */
	__calc_alloc_counts_for_prepare_op(o.op, it.res.nlbas, t, true /* is_trim_continous, whatever */, &max_locks, &max_cmds, &max_new_cmds);
	if (__allocate_commands(max_cmds ,&o.cmds, NULL) < 0) {
		rv = -ENOMEM;
		goto _out;
	}

	ncmds = 0;
	if (__mirror_cmds_add_for_raid(pr, it.res.rlba, &it.res.nlbas, NULL /* vbi */, &o, &ncmds, (o.op != NVMEIB_BLOCK_IO_OP_DISCARD), 0, 0, 0) < 0) {
		rv = -EIO;
		goto _out;
	}
	o.cmds[0].ncmds = ncmds;	/* for dp_cmds_free_all */
	if (ncmds > (s16)ARRAY_SIZE(_out->disks)) {
		rv = -EIO;
		goto _out;
	}

	_out->n_cmds = ncmds;

	for (i = 0; i < _out->n_cmds; i++) {
		struct nvmeibc_block_command *cmd = &o.cmds[i];
		bool is_owner = (cmd->ds == &it.res.r->segments[slice_start_si]);
		_out->disks[i] = cmd->ds->disk;
		_out->offs[ i] = cmd->iocmd->reqs1.disk_address;
		_out->descr[i] = (nvmeib_block_io_op_is_write(o.op)) ?
				(is_owner ? "Wr Data" : "Wr Mirror") :
				(is_owner ? "Read Data" : "Read Mirror");
	}

	if (tu->input.translate_locks)	/* Generate locks (if needed) */
		dp_block_translation_unit_calc_locks(tu, &it);

_out:
	dp_cmds_free_all(o.cmds);
	nvmeibc_topology_put(t);
	NFOUT;
	return rv;
}

int dp_mirror_translate_addr(struct dp_block_translation_unit *tu)
{
	int rv = 0;
	if (tu->input.translate_by_cfg) {
		rv = __translate_addr_by_cfg(tu);
	} else {
		rv = __translate_addr_by_topology(tu);
	}
	return rv;
}
