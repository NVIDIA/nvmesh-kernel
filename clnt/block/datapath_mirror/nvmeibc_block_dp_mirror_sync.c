/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_mirror.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole.h"
#include "block/nvmeibc_block_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "nvmeibc_io_pet.h"

static bool __data_could_not_be_read(const struct nvmeibc_block_command *c)
{
	return c->do_not_send || (c->o_rv != 0);	// Note: return false does not mean that data is OK. It could be logical bad sector!
}
#define __data_could_be_read(c)		(!__data_could_not_be_read(c))

static bool dp_sync_cmd_is_valid_read_source(const struct recovery_sync_op *so, const int i)
{	// Note If this segment is not readable (for any reason: topo / dirtybit) then __mark_read_to_dirty_w_seg_as_do_not_send() would make it do not send.
	const struct nvmeibc_block_command *c = &so->cmds[i];
	return ((so->nwhole_exec_plan.invalid_sources & (1 << i)) == 0)	&&
			__data_could_be_read(c);
}

static bool __cant_trust_data(const struct recovery_sync_op *so, const int i)
{
	return !!(so->nwhole_exec_plan.first_write_bmp & (1 << i));	// First_write_bmp bits represent: read-fail, dbits on 'W' segs, force rebuild, etc...
}

static void __inject_debug_di_with_sync_info(struct recovery_sync_op *so, int n_cmds_to_do)
{
	if (so->cmds->o->nd->dp.enable_di_debug_mode) {
		int ir, iw, n_writes = n_cmds_to_do;
		for (iw = n_read_cmds(so); (n_writes>0); iw++) {
			if (so->cmds[iw].do_not_send)
				continue;				// No need to execute write, already identical or segment is dead
			ir = iw - n_read_cmds(so);
			if (dp_sync_cmd_is_valid_read_source(so, ir))
				dp_dbgdi_copy_sync_overwritten(&so->cmds[iw], &so->cmds[ir]);
			else
				dp_dbgdi_clear_sync_overwritten(&so->cmds[iw]);
			--n_writes;
		}
	}
}

static void __set_wr_cmd_ndb_to_read_cmd_ptr(struct nvmeibc_block_command *cmd, const struct nvmeibc_block_command *src_cmd)
{
	extern u32 nvmeibc_calculate_edic_from_data_and_rlba(const u64 rlba, const unsigned char *data, const bool debug_di_enabled);
	BUG_ON(cmd->iocmd->reqs1.ndb && !cmd->is_not_ndb_owner); // Write commands never own the NDBs
	BUG_ON(!src_cmd->iocmd->reqs1.ndb);
	cmd->iocmd->reqs1.ndb = src_cmd->iocmd->reqs1.ndb;
	cmd->is_not_ndb_owner = true;
	if (nvmeibc_is_mirror_md_enabled(cmd)) {
		if (nvmeibc_is_mirror_md_enabled(src_cmd)) {
			nvmeibc_fill_metadata_from_command(cmd, src_cmd);
		} else {	// Only happens when 1 leg of mirror has metadata and the other does not. Much like: __fill_compute_metadata_r1(). We have to calcualte metadata from scratch
			struct nvmeibc_datapath *dp = &cmd->o->nd->dp;
			const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
			struct nvmeibc_block_io_req *req = &cmd->iocmd->reqs1;
			u8* md_raw = (u8*)req->md;
			struct sgl_block_iter sbi = SGL_BLOCK_ITER_INIT(*req->ndb->table.sgl);
			u64 i;
			for (i=0; i < cmd->nlbas; i++) {
				union nvmeibc_block_dp_ec_data_block_md *md = (void*)&md_raw[md_size*i];
				const u64 rlba = cmd->first_rlba + i;
				const unsigned char *blk_data = sgl_block_iter_virt(&sbi);
				const u32 edic = nvmeibc_calculate_edic_from_data_and_rlba(rlba, blk_data, dp->enable_di_debug_mode);
				nvmeibc_block_dp_ec_md_make_r1(md, edic);
				sgl_block_iter_advance(&sbi, 1);
			}
		}
	}
}

/* Found source prepare all needed writes to this read buffer, and set unneeded
   as do_not_send.
   Copy metadata to respective buffers if needed. Metadata has to be actually
   copied and not just pointer set since its size may vary from disk to disk,
   thus influencing the whole buffer alignment.*/
static int __set_write_buffer_to_valid_source(struct recovery_sync_op *so)
{
	int i, count = 0;
	const int write_start = n_read_cmds(so), src = so->R1.valid_read_index;
	const struct nvmeibc_block_command *src_cmd = &so->cmds[src];
	for (i = 0; i < n_write_cmds(so); i++) {
		struct nvmeibc_block_command *dst_cmd = &so->cmds[write_start+i];
		WARN(dst_cmd->iocmd->comp.comp_code, "nvmeibc bug\n");	// Clean on init and cleaned when advancing to next slice
		BUG_ON(!nvmeib_block_io_op_is_write(dst_cmd->iocmd->reqs1.op));		// Same as above
		if (dst_cmd->do_not_send /* Data already OK || SEG == DEAD) */ ||
			(src == i) /* Write is the same as src */) {
			dst_cmd->do_not_send = true;
			continue;
		}
		__set_wr_cmd_ndb_to_read_cmd_ptr(dst_cmd, src_cmd);
		count++;
	}
	_ND(t_00_swbtbs, "Using read source @SEG_DBG_UUID to sync blkset, @INT write cmds", src_cmd->ds->dbg_uuid, count);
	BUG_ON(!count);	// Why was the sync called if it has nothing to write
	return count;
}

void __find_best_valid_source_for_data(struct recovery_sync_op *so);
void __find_best_valid_source_for_data(struct recovery_sync_op *so)
{
	int i;
	so->R1.was_read_source_calced = true;
	so->R1.valid_read_index = -1;
	for (i = 0; i < n_read_cmds(so); i++) {
		if (dp_sync_cmd_is_valid_read_source(so, i)) {
			so->R1.valid_read_index = i;
			return;
		}	// else cmds[i] data cannot serve as source, but it might be compared to source to avoid write
	}
}
#define __r1_valid_source_verify(so) BUG_ON(!(so->R1.was_read_source_calced && (so->R1.valid_read_index >= 0)))
#define __r1_valid_source_remove(so) ({ so->R1.was_read_source_calced = false;  so->R1.valid_read_index = -1; })

static inline int __find_best_invalid_source_for_data(struct recovery_sync_op *so)
{	// Data is destroyed! But try preserving meaningfull block when writing logical bad sector
	int i;
	for (i = 0; i < n_read_cmds(so); i++) {								// (1) Maybe-old-data (W seg+dirty-suspect) > (2) Old-data (W seg+dbit)
		const struct nvmeibc_block_command *c = &so->cmds[i];
		if (__data_could_be_read(c) && __cant_trust_data(so, i))
			return i;
	}
	for (i = 0; i < n_read_cmds(so); i++) {								// (3) Logical bad sector, WrongCrc > (4) Any-Data+Crc
		const struct nvmeibc_block_command *c = &so->cmds[i];
		if (__data_could_be_read(c) && nvmeibc_is_mirror_md_enabled(c))
			return i;
	}
	for (i = 0; i < n_read_cmds(so); i++) {								// (5) Any-Data-No-Crc, W- Topo, BadSector
		const struct nvmeibc_block_command *c = &so->cmds[i];
		if (__data_could_be_read(c))
			return i;
	}
	return 0;	// (6) Junk (Unitialized-Mem) DGL: Even write uncorrectable command needs an NDB. Data (SGL) does not matter, so any NDB will do, provided it has a correct length field set. Just take from the first read command.
}

bool nvmeibc_raid1_destroy_force_physical_bad_sector_in_sync = false;		// Use logical bad sector if possible
static void __set_write_buffer_to_bad_sector(struct recovery_sync_op *so)
{	// Used only in slice by slice mode, because we dont destroy full data before attempt to fix each slice idependantly
	int i;
	const int src = __find_best_invalid_source_for_data(so);			// All sources are invalid, we are going to destroy the slice, but if any, even wrong block exists, preserve it
	_NTSO(t_01_swbtbs, "No valid source. Detroying slice @SBS_INDEX, by cmd[@INT]", so->slice_by_slice_index, src);
	for (i = 0; i < n_write_cmds(so); ++i ) {
		struct nvmeibc_block_command *dst_cmd = &so->cmds[n_read_cmds(so) + i];
		struct nvmeibc_block_io_req *req = &dst_cmd->iocmd->reqs1;
		WARN(dst_cmd->iocmd->comp.comp_code, "nvmeibc: comp_code clean on init and advancing to next slice %d\n", dst_cmd->iocmd->comp.comp_code);
		WARN(dst_cmd->do_not_send != (dst_cmd->ds->toma_acm == NVMEIBTC_DS_MODE_DEAD), "nvmeibc: do_not_send field should be properly set: %d\n", dst_cmd->do_not_send);
		if (dst_cmd->do_not_send)
			continue;
		WARN_ON(!nvmeib_block_io_op_is_write(req->op));						// Cleanup of previous iteration should have put it as 'write'
		WARN_ON(dst_cmd->nlbas != 1);
		__set_wr_cmd_ndb_to_read_cmd_ptr(dst_cmd, &so->cmds[src]);			// Direct write to use actual data block
		if (nvmeibc_raid1_destroy_force_physical_bad_sector_in_sync || !nvmeibc_is_mirror_md_enabled(dst_cmd)) { // Write physical bad sector as we cant write logical one
			req->op = NVMEIB_BLOCK_IO_OP_WRITE_UNCOR;						// Change from Write to write-Uncorrectable
		} else {
			// Somewhat equivalent to ec __prep_write_perm_read_fail()
			nbdpec_md_mark_data_invalid_for_read(req->md, dst_cmd->is_parity, NVMEIBC_MIRROR_UNUSED_TXID_JRI);
			dp_dbgdi_do_add_restore_info(dst_cmd, true);
			req->op = NVMEIB_BLOCK_IO_OP_WRITE;								// Write logical bad sector
		}
	}
}

static void __copy_sync_read_to_orig_read_io_sgl(const struct recovery_sync_op *so)
{
	const struct nvmeibc_block_command *orig_read = so->orig_rldr;
	BUG_ON(!orig_read  || orig_read->o->op != NVMEIB_BLOCK_IO_OP_READ);
	if (1) {
		const struct nvmeibc_block_io_req *orig_req = &orig_read->iocmd->reqs1;
		const struct nvmeibc_block_command *valid_read = &so->cmds[so->R1.valid_read_index];
		struct nvmeib_data_buffer *orig_ndb = orig_read->iocmd->reqs1.ndb;
		struct nvmeib_data_buffer *valid_ndb = valid_read->iocmd->reqs1.ndb;
		struct sgl_block_iter orig_sbi =  SGL_BLOCK_ITER_INIT(*orig_ndb->table.sgl);
		struct sgl_block_iter valid_sbi = SGL_BLOCK_ITER_INIT(*valid_ndb->table.sgl);
		u64 orig_nlbas = orig_read->nlbas;
		u64 valid_nlbas = valid_read->nlbas;
		u64 copy_nlbas;

		// The following is only correct if r1->slice_size is 1,
		// i.e. we can rely on first_rlba to determine ranges
		BUG_ON(nvmeibc_raid_is_ec(so->r1)); // r1->slice_size is 1
		if (orig_read->first_rlba > valid_read->first_rlba) {
			const u64 diff = orig_read->first_rlba - valid_read->first_rlba;
			if (diff < valid_nlbas) {
				sgl_block_iter_advance(&valid_sbi, diff);
				valid_nlbas -= diff;
			}
			else {
				valid_nlbas = 0;
			}
		} else if (valid_read->first_rlba > orig_read->first_rlba) { // SBS support
			const u64 diff = valid_read->first_rlba - orig_read->first_rlba;
			if (diff < orig_nlbas) {
				WARN_ON(orig_req->do_512b_sub_block_x);	// Here we handle non-1st block of the original read, and subblock ops are only supported for single block reads
				sgl_block_iter_advance(&orig_sbi, diff);
				orig_nlbas -= diff;
			}
			else {
				orig_nlbas = 0;
			}
		}

		copy_nlbas = min(valid_nlbas, orig_nlbas);
		if (copy_nlbas)
			nflog(t_04_r1s, "Sync copies data to Read IO's buffer: @DLBA, nblks=@NBLKS", orig_req->disk_address + (orig_read->nlbas - orig_nlbas), copy_nlbas);

		// Copy the data
		if (orig_req->do_512b_sub_block_x) {	// Subblock read special case
			memcpy(sgl_block_iter_virt(&orig_sbi), sgl_block_iter_virt(&valid_sbi) + (do_512b_sub_block_x_val(orig_req->do_512b_sub_block_x) * 512), 512);
			copy_nlbas = 0;	// Mark as already copied
		}
		while (copy_nlbas) {
			unsigned int nblocks = min(sgl_block_iter_nblocks(&valid_sbi), sgl_block_iter_nblocks(&orig_sbi));
			nblocks = min_t(unsigned int, nblocks, copy_nlbas);
			memcpy(sgl_block_iter_virt(&orig_sbi), sgl_block_iter_virt(&valid_sbi), NVMEIBC_SECTOR2BYTE(nblocks));
			sgl_block_iter_advance(&valid_sbi, nblocks);
			sgl_block_iter_advance(&orig_sbi, nblocks);
			copy_nlbas -= nblocks;
		}
		// TODO(EC-2584): Mark original read as successful - only after sync completes successfully.
		//__cmd_clean_comp_val((struct nvmeibc_block_command *)orig_read);
	}
}

// Compares read cmd's data with valid read's data. @irc - index of read cmd
static inline bool __compare_read_cmds_data_for_mirror_write(struct recovery_sync_op *so, int irc)
{
	const int irc_valid = so->R1.valid_read_index;
	struct nvmeib_data_buffer *ndb0 = so->cmds[irc_valid].iocmd->reqs1.ndb;
	struct nvmeib_data_buffer *ndb1 = so->cmds[irc].iocmd->reqs1.ndb;
	const unsigned int nblocks = NVMEIBC_BYTE2SECTOR(ndb0->length);
	unsigned int nblocks_left = nblocks;
	struct sgl_block_iter sbi0 = SGL_BLOCK_ITER_INIT(*ndb0->table.sgl);
	struct sgl_block_iter sbi1 = SGL_BLOCK_ITER_INIT(*ndb1->table.sgl);

	if (unlikely(ndb0->length != ndb1->length)) {
		WARN(1, "nvmeibc bug!");
		return false;
	}

	if (irc == irc_valid)
		return true;

	while (nblocks_left) {
		unsigned int nblocks_delta = min(sgl_block_iter_nblocks(&sbi0), sgl_block_iter_nblocks(&sbi1));
		u64 *d0 = sgl_block_iter_virt(&sbi0);
		u64 *d1 = sgl_block_iter_virt(&sbi1);
		u64 *d0_end = ((void *)d0) + NVMEIBC_SECTOR2BYTE(nblocks_delta);

		if (unlikely(nblocks_delta > nblocks_left)) {
			WARN(1, "nvmeibc bug! nblocks in sgl is more than blocks in ndb length");
			return false;
		}

		for (; d0 < d0_end; d0++, d1++) {
			if (*d0 != *d1) {
				_ND(tr_02_r1_stale_cmp, "Blocks differ @DLBA offset=@INT data=@LLX valid=@LLX",
						so->locks[0].address,
						((nblocks - nblocks_left) << NVMEIBC_SECTOR_SHIFT) + ((void *)d0 - sgl_block_iter_virt(&sbi0)), *d1, *d0);
				return false;
			}
		}

		nblocks_left -= nblocks_delta;
		sgl_block_iter_advance(&sbi0, nblocks_delta);
		sgl_block_iter_advance(&sbi1, nblocks_delta);
	}
	return nvmeibc_are_metadatas_identical(&so->cmds[irc_valid], &so->cmds[irc]);
}

static int dp_mirror_write_cmds_prepare(struct recovery_sync_op *so)
{
	int ir, n_cmds_to_do = 0;       // Calculate how many writes to do
	__r1_valid_source_verify(so);
	WARN_ON((!is_op_sync_stale(so->o->op) && !is_op_sync_no_wr_ho(so->o->op))||(so->r1->slice_size != 1));
	for (ir = 0; ir < n_read_cmds(so); ir++) {
		const struct nvmeibc_block_command *read_cmd = &so->cmds[ir];
		const int iw = n_read_cmds(so) + ir;
		if (__data_could_not_be_read(read_cmd)) {				// we can still do sync operation as if. this read yielded a different result from valid RW src
			_ND(tr_00_r1_stale_cmp, "Sync: Ignorring read cmd err: rv=@O_RV", read_cmd->o_rv);
		} else if (__cant_trust_data(so, ir)) {
			_ND(tr_01_r1_stale_cmp, "Sync: Ignorring read cmd due to bad sector");
		} else if (__compare_read_cmds_data_for_mirror_write(so, ir)) { // Either data is identical to data in valid read, or this is the valid read
			so->cmds[iw].do_not_send = true; // Skip this write
		}
		if (!so->cmds[iw].do_not_send) {
			so->nwhole_exec_plan.first_write_bmp |= (1 << ir);			// Daniel: Todo, remove the do_not_send booleans and use bitmap, like EC does
			n_cmds_to_do++;
		}
	}
	if (n_cmds_to_do == 0) { /* Entire slice is already synced. */
		WARN(so->nwhole_exec_plan.first_write_bmp != 0, "nvmeibc bug! Need to write bmp=0x%x, but zero cmds will be sent!", so->nwhole_exec_plan.first_write_bmp);
		so->stage = sync_stage_recov_no_write_hole_sbs_loop_end;
	} else {
		int n_cmds_ready = __set_write_buffer_to_valid_source(so);
		WARN_ON(n_cmds_ready != n_cmds_to_do);
		__inject_debug_di_with_sync_info(so, n_cmds_to_do);
	}
	return n_cmds_to_do;
}

#define WARN_WRONG_SKIP_CHECK(condition, lock_val) WARN(condition, "so=" PRI_SO_NAME ",  holder=0x%x, topo=%llu rlba=0x%llx, ow_lock_seg=%08x\n", PRI_SO_NAME_ARGS(so), lock_val, so->o->topo->debug_unique_index, so->rlba, so->locks->ds->dbg_uuid);

static union nvmeib_lock_id __get_worst_stale_possible(struct recovery_sync_op *so)		// R1 equivalent to EC __is_write_hole_possible()
{
	union nvmeib_lock_id holder = {.all = 0};
	int i;
	for (i = 0; i < so->locks->n_siblings; i++) {				// Analyze all onwer locks, crucial for dual locks topology
		const struct nvmeibc_cmd_lock *l = &so->locks[i];
		BUG_ON(!((l->type == NVMEIBC_CMD_LOCK_OWNER) || (l->type == NVMEIBC_CMD_LOCK_COPY_OWNER)));
		holder = nvmeibc_d_rdma_comp_get_contending_id(&l->comp);
		if (!((holder.all == 0ULL) || did_caller_of_so_took_this_lock(l))) { // is_read bit is irrelevant for R1: || (holder.bits.is_read))
			WARN_WRONG_SKIP_CHECK(holder.bits.is_stale == 0, holder.all);
			return holder;		// Found a stale lock
		} // Else: No problem with this lock
	}
	return holder;					// Not stale but result of last owner. Crucial for dual locks topology
}

static void __mark_read_to_dirty_w_seg_as_do_not_send(struct recovery_sync_op *so, const union nvmeibc_dbits_entry pre)
{	// Optimization, reads to W seg with dbit for them will be discarded as invalid sources, so just avoid the read. W seg with unknown dirtybits will be read
	u16 pre_dirty_bmp = nvmeibc_dbits_get_turn_on_bmp(&pre, &so->r1->calculated_data.topo_traits); // Use DBits from pre transaction to prevent reads
	if (pre_dirty_bmp) { 																		 // If any dirty bits are set (including convicts) we should not read them
		const int slice_start = so_get_owner_seg(so);
		if (slice_start) // Convert from seg index in praid to roles within slice
			pre_dirty_bmp = ror32_width(pre_dirty_bmp, slice_start, so->r1->replicas);
		// Todo: Use __get_dirty_roles_bmp_pre_sync()
		nvmeibc_sync_set_cmds_only_do_not_send_by_bmp(so, 0, n_read_cmds(so)-1, pre_dirty_bmp);		// Only set do not send to dirty segments (it could already be set if acm is NVMEIBTC_DS_MODE_W_IS_DIRTY
	}
}

static u32 __gen_mirror_txid_sync(const struct recovery_sync_op *so)
{
	return (u32)so->o->op;		// Daniel: Tmp debug code (coz those bits are not used)
}

static void __dump_bug_NVMESH3032(const struct recovery_sync_op *so, const char *why) {
	const struct nvmeibc_raid_leader_cmd_ctx *rld = &so->cmds->rld;
	_NTSO(t_01_nvmesh3032, "NVMESH-3032 fix=@CHAR: op=@OP_CODE {pre=@BINFO, post=@BINFO, n_locks=@INT, locks_pre[@BINFO,@BINFO]}",
			why[0], so->o->op, rld->pre.all, rld->post.all, so->locks->n_siblings, nvmeibc_cmd_lock_get_bi(&so->locks[0]).all, nvmeibc_cmd_lock_get_bi(&so->locks[1]).all);
}

void __mirror_sync_calc_post_binfo(struct recovery_sync_op *so, struct nvmeibc_raid_leader_cmd_ctx *rld, bool has_stale_lock);
void __mirror_sync_calc_post_binfo(struct recovery_sync_op *so, struct nvmeibc_raid_leader_cmd_ctx *rld, bool has_stale_lock)		// Equivalent to EC function: __mutate_op_according_to_binfo()
{
	const union nvmeibc_dbits_entry pre = {.all_bits = rld->pre.bits.dirty};
	struct dp_topology_traits const *topo_traits = &so->r1->calculated_data.topo_traits;
	const u16 dbits_on_topo_bmp = nvmeibc_raid1_get_sgmnts_bmp(so->r1, dbits_on_mask);
	struct nvmeibc_dbits_tx tx;
	const bool has_unknown_dbits = (nvmeibc_dbits_get_n_unk(&pre, topo_traits) != 0);
	const bool enable_store_dirty_bits_in_peristent_md = so->o->nd->dp.enable_store_dirty_bits_in_peristent_md;
	if (so->o->op == NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB) {
		BUG_ON(!has_stale_lock);	// Miss-use of the function. This is illegal because we never took the lock to know if it is stale or not
		BUG_ON(enable_store_dirty_bits_in_peristent_md);	// Cannot use this sync as has to commit dbit to metadata
		if (has_unknown_dbits) {			// If unknown exists, fill the rest with unknowns. Likely that data on R1 legs is identical, Optimization for cold recovery of R1, Toma turns on stale + unknown
			const u16 n_dead = hweight16(dbits_on_topo_bmp);
			nvmeibc_dbits_tx_init_by_bmp(&tx, topo_traits, 0                , 0                              , 0);
			tx.action.num_unknowns = n_dead; // Fill Every possible dead with optional unknown (unless it already has dbit)
			BUG_ON(n_dead != topo_traits->n_degraded);	 // Trap: Otherwise this sync is called wrongly and creates data corruption.
		} else {													// If {Real dbit exists or nothing} + stale lock, fill with real dbits. Likely that data on R1 legs differs.
			nvmeibc_dbits_tx_init_by_bmp(&tx, topo_traits, dbits_on_topo_bmp, 0                              , 0);
		}
	} else if (so->o->op == NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE) {
		WARN_ON(topo_traits->n_parities <= 1);	// In 2-mirror pre and post dbits are always 0, no reason to call this function
		BUG_ON(has_unknown_dbits);				// Dbits for 'W' seg cant exists, so naturally unknowns cannot exist as well
		if (has_stale_lock)		// Copy stale lock to all writable ram segs, turn dbits on for all dead. Extended version of stale2dirty sync
			nvmeibc_dbits_tx_init_by_bmp(&tx, topo_traits, dbits_on_topo_bmp, 0, 0);
		else {}					// Sync executed when stale lock exists, but other client already solved it. Just do nothing
	} else {	// Note: We dont care about type of sync, only the situation after locks taken
		const bool should_db_turn_on =  has_stale_lock;										// Stale lock has to turn on dbit for dead segments coz cant access them, Dbit/Read-fail syncs do not introduce new info so can never turn dbits on
		const bool should_db_turn_off = (so->n_slices == LOCKSET_SLICES);					// Only if we fix all slices
		const u32 turn_off_topo_bmp = (nvmeibc_raid1_get_sgmnts_bmp(so->r1, dbits_off_mask));
		const u32 turn_off_inv_bmp = (~dbits_on_topo_bmp);										// Used in case simulator injected invalid dbits, and we want to clean them as well
		const u32 turn_off_bmp = (should_db_turn_off ? (turn_off_topo_bmp | turn_off_inv_bmp) : 0);
		const u32 turn_on_bmp =  (should_db_turn_on  ?  dbits_on_topo_bmp                     : 0);
		nvmeibc_dbits_tx_init_by_bmp(&tx, topo_traits, turn_on_bmp, turn_off_bmp, 0);
		if (has_unknown_dbits)
			so->R1.is_dirty_suspect = true;					// Note here: all syncs (stale/db/bad/read-fail) will run identically. Do all possible reads, compare data and turn off unknown dbits if possible

		__mark_read_to_dirty_w_seg_as_do_not_send(so, pre);	// unknown dbits may exist in pre, we did not resolve them to send read 'W' seg anyways to potentially avoid writes!
	}
	nvmeibc_dbits_tx_apply(&pre, &tx);
	if ((so->o->op != NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB) && (so->n_slices == LOCKSET_SLICES)) {
		BUG_ON(nvmeibc_dbits_get_n_unk(&tx.post, topo_traits) != 0);	// All other full syncs resolve unknown in to 'post'
	}
	rld->post.bits.dirty = tx.post.all_bits;
	BUG_ON(enable_store_dirty_bits_in_peristent_md && (rld->post.bits.dirty != 0)); // First turn on dbits (much like done in HTR for ec), then do writes then turn off.
	rld->post.bits.txid = __gen_mirror_txid_sync(so);
	if (rld->pre.all != rld->post.all)
		mark_blockset_info_not_written(so);

	// Same as EC: 'sync_stage_recov_analyze_binfo' stage
	if (!should_blockset_info_commit(so) && !dp_sync_common_are_all_binfo_equal(so)) { // Solve binfo descrepancy between ram copies
		mark_blockset_info_not_written(so); // Explicit mark_blockset_info_not_written
	}
	if ((so->n_slices != LOCKSET_SLICES) && should_blockset_info_commit(so) && (topo_traits->n_parities == 1)) {
		if (rld->post.bits.dirty) {			// EC-5969: For 3 mirror, do a more elaborate analysis. dbits == 0 is overkill. We want to verify no dbits for 'W' segs are written to 'W' seg. But dbit for 'D' segs can be written on 'W' seg binfo
			mark_blockset_info_written(so);	// Writing dirtybits is illegal in R1 with 2 mirror {RW,W}! This sync cannot clean dbits, but also cannot propagate them to 'W' seg. Legal with 3 mirror and above. Example {RW,W,D} with Dbit for Seg2, Need to be copied from RW to W, to transition to {RW,RW,D} topo.
			__dump_bug_NVMESH3032(so, "reason");
		}
	}
}

static bool __mirror_check_if_has_something_to_do_with_disks(struct recovery_sync_op *so, struct nvmeibc_raid_leader_cmd_ctx *rld, bool has_stale_lock)
{
	return (has_stale_lock || (rld->pre.bits.dirty != rld->post.bits.dirty) || is_op_sync_readfail(so->o->op) || is_op_sync_scrubbing(so->o->op));
}

static int __mirror_sync_data_fill_cmds(struct recovery_sync_op *so)
{
	struct nvmeibc_block_command *c = so->cmds;
	struct nps_block_iter nbi = NPS_BLOCK_ITER_INIT(so->pages);
	const gfp_t gfp = nvmeibc_dp_get_allow_io_gfp_flags();
	int i;
	for (i = 0; i < c[0].ncmds; i++) {
		const bool is_read = (i < n_read_cmds(so));
		dp_sync_cmd_init(so, i);
		if (is_read) {
			struct nvmeib_data_buffer *ndb = nvmeib_get_ndb(&c[i], so->n_slices /* nentries */, gfp);
			if (unlikely(!ndb))
				return -ENOMEM;

			nvmeibc_fill_ndb(ndb, so->n_slices /* nlbas */, &nbi);
			BUG_ON(c[i].is_not_ndb_owner);
		} else {
			c[i].iocmd->reqs1.ndb = NULL; // All write cmds use NULL NDBs (set a trap). Later write cmds will point to read cmd ndb to commit the correct data
		}
	}
	return 0;
}

static int __mirror_sync_data_prepare_op(struct recovery_sync_op *so)
{
	const struct nvmeibc_cmd_lock *l = so->locks;
	struct nvmeibc_block_command *c;
	const u64 lock_dlba = __lock_start(*l) + so->start_slice;				// dlba in segment where primary owner resides[blks]
	const u64 seg_lba = (lock_dlba - l->ds->first_lba);						// Blocks
	int i, rv = -ENOMEM;
	const int n_segs = so->r1->replicas, n_reads = n_segs, n_writes = n_segs;
	u64 first_rlba = 0;

	NFIN;
	if (!(so->cmds = dp_cmds_kvzalloc((n_reads + n_writes))))
		goto _out;
	if ((rv = nvmeibc_sync_alloc_mem_resources(so)) < 0)
		goto _out;

	c = so->cmds;

	// Calcualte first rlba using lock segment
	first_rlba = nvmeibc_datapath_dlba_to_rlba(&so->o->nd->dp, so->r1, (l->ds-so->r1->segments), lock_dlba);

	for (i = 0; i < n_segs; i++) { // Mimik EC
		const int w = n_reads + i, first_seg = so_get_owner_seg(so);
		const u32 si = (first_seg+ i)% n_segs;
		struct nvmeibc_disk_segment *ds = &(so)->r1->segments[si];
		const u64 disk_lba = ds->first_lba + seg_lba;
		dp_sync_init_cmd(c, w, ds, disk_lba, NVMEIB_BLOCK_IO_OP_WRITE);
		dp_sync_init_cmd(c, i, ds, disk_lba, NVMEIB_BLOCK_IO_OP_READ);
		c[w].first_rlba = c[i].first_rlba = first_rlba;
		// If we use metadata in mirror, we will also have to allocate memory for it
		// Memory is allocated for read commands only, each subsequent write command
		// reuses the memory space of respective read command only then releasing it
		if ((nvmeibc_sgmnt_sw_md_size(ds) > 0) && so->o->nd->dp.enable_edic_check) {
			if (!(c[i].iocmd->reqs1.md = nvmeibc_alloc_md(so->n_slices, nvmeibc_sgmnt_sw_md_size(c[i].ds)))) {
				rv = -ENOMEM;
				goto _out;
			}
			c[w].iocmd->reqs1.md = c[i].iocmd->reqs1.md;					// write cmd shares MD with read cmds
			c[w].do_not_realease_md = true;
			c[w].is_parity = c[i].is_parity = NVMEIBC_DATA_MD_MIRROR_IS_PARITY;
		}
		if (ds->toma_acm == NVMEIBTC_DS_MODE_W_IS_DIRTY) { // Do not send reads to convicts and dead segs. Note: To 'W' and above we will send, unless existing dbit explicitly states that it is illegal to use
			c[i].do_not_send = true;
		}
		if (ds->toma_acm == NVMEIBTC_DS_MODE_DEAD) {	// Do not send write
			c[w].do_not_send = true;
			c[i].do_not_send = true;
		}
	}
	c[0].ncmds = (n_reads + n_writes);	/* Init generic command headers */

	if ((rv = __mirror_sync_data_fill_cmds(so)) < 0)
		goto _out;

	rv = 0;
_out:
	NFOUT;
	return rv;
}

int dp_mirror_sync_prepare_op(struct recovery_sync_op *so)
{
	const enum nvmeib_block_io_op op = so->o->op;
	if (is_op_sync_stale(op)||is_op_sync_no_wr_ho(op)) {
		WARN_WRONG_SKIP_CHECK(so->assume_caller_holds_locks && (!is_op_sync_readfail(op)), 0);	// Only read-fail is the single 'sync' that can be triggered by IO when it holds locks
		return __mirror_sync_data_prepare_op(so);
	} else if (is_op_sync_commandless(op) || is_op_sync_commit_binfo(so->o->op)) {
		// Unlike EC: Mirror syncs cannot mutate to one of the above, and we dont prepare commadns for them
		// Note: so->n_slices may still be full blockset in special cases of 2 mirror, where we can solve full blockset without disk commands
		return dp_sync_cmd_alloc_fill_rldr_only(so);
	} else { BUG(); }
	return 0;
}

#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_maintenance.h"			// LKJ: Move maintanances to be in common directory, not EC only

void dp_mirror_sync_execute_op(struct recovery_sync_op *so)
{
	struct nvmeibc_block_command *rldr = so->cmds;
	const union nvmeib_blkset_info binfo = dp_locks_get_TxID_dbits(so->locks, 0);
	const int num_p = so->r1->calculated_data.topo_traits.n_parities;
	const union nvmeib_lock_id holder = __get_worst_stale_possible(so);
	const bool had_stale_lock = (holder.bits.is_stale);
	const enum nvmeib_block_io_op op = so->o->op;
	__ndump_operation(mirror_sync_execute, &so->o);
	WARN(so->stage != sync_stage_recov_lo_all_taken, "so=" PRI_SO_NAME ", stage=%d\n", PRI_SO_NAME_ARGS(so), so->stage);
	rldr->rld.post.all = rldr->rld.pre.all = binfo.all;	// Commit possibly broken binfo to rldr
	rldr->rld.post.bits.txid = __gen_mirror_txid_sync(so);
	if (unlikely(nvmeibc_raid_is_jbod(so->r1)))
		return nvmeibcbdpec_return_to_caller_sm(so);
	if (!dp_sync_verify_binfo_is_legal(so, binfo))
		return nvmeibcbdpec_return_to_caller_sm(so);
	// Note: we dont test nvmeib_do_i_care_about_broken_binfo(so) like EC because conditions below already split the cases
	if (is_op_sync_stale(op)||is_op_sync_no_wr_ho(op)) {
		enum sync_op_stage_e next_stage = sync_stage_recov_no_write_hole_read_done;
		__mirror_sync_calc_post_binfo(so, &rldr->rld, had_stale_lock);
		if (!__mirror_check_if_has_something_to_do_with_disks(so, &rldr->rld, had_stale_lock)) {
			_NTSO(t00dpmseo, "Sync skip no_whole algorithm, op=@BLOCK_IO_OP, pre_dbits=[@DBITS], post_dbits=[@DBITS], commit_binfo=@BOOL_YN", op, rldr->rld.pre.bits.dirty, rldr->rld.post.bits.dirty, should_blockset_info_commit(so));
			nvmeibc_sync_set_cmds_only_do_not_send_by_bmp(so, 0, rldr->ncmds - 1, (~0));        // Do not send any commands, Not reads and not writes
			next_stage = sync_stage_recov_write_cmds_done;
		}
		nvmeibc_sync_send_all_read_cmds(so, NULL, next_stage);
		BLKCMP_SO_ASYNC_RESUME_SND(dp_mirror_sync_resume_op(so));
	} else if (op == NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE) {
		WARN_ON(so->n_slices != 0);				// we dont fix any slice
		WARN_ON(so->locks->n_siblings == 1);	// with single primary owner, nowhere to copy it! Bug in design, should have called stale-2-dirty sync
		if (unlikely(rldr->rld.pre.bits.dirty)) {
			const int num_deg =  nvmeibc_praid_get_num_deg_segs(so->r1);
			// This situation is illegal for single-degrade mode (always illegal for 2 mirror)
			//      Topo {RW,D}: Should have called stale-2 dirty
			//      Topo {RW,RW,D}: Mathematically this is legal, but there is no such flow! We prefer solve stale locks asap (Remove stale lock and mark dbit for dead seg on all RW seg)
			//      Topo {RW,RW,W/W-}: Must call dbits turn off sync.
			if (num_deg <= 1) {
					WARN_ONCE(num_deg <= 1, "nvmeibc bug! so=" PRI_SO_NAME " {%u} Sync must turn-off dbits! Abort to prevent data corruption! pre=0x%x, locks_pre[0x%x,0x%x]\n", PRI_SO_NAME_ARGS(so), so->o->dbg_id, rldr->rld.pre.all, (u32)nvmeibc_cmd_lock_get_bi(&so->locks[0]).all, (u32)nvmeibc_cmd_lock_get_bi(&so->locks[1]).all);
					nvmeibc_block_suspend(so->o->nd, NULL, NULL); /* Critical error. Any further action will cause corruption. Fail recovery, suspend the device. We require user intervention to exit this state. Should not happen normally. */
					so->error = -10029;
			}
			// In multi degraded this sync might be legal (3 mirror and above)
			//      Topo {RW,W,D} with Dbit for Seg1 or DirtySuspect: Illegal! Must call dbits turn off sync to fix 'W' seg.
			//      Topo {RW,W,D,D} with Dbit for Seg2, Need to copy stale lock from RW to W seg to transition to {RW,RW,D,D} asap and solve stale lock later in new topology
			//              Do nothing with disk. However we do have to turn on dbit for all 'D' seg
			//              Note: Verification that no dbits for seg1 exist will be done when binfo is commited with stale lock
			__mirror_sync_calc_post_binfo(so, &rldr->rld, true);
		} else if (unlikely(dp_sync_common_has_dbits_anywhere(so))) {
			// Copy locks have incorrect binfo (dbit, while primary owner is clean). This should not happen! Caused by a bug somewhere else in the code. but we can recover from that by commiting clean owner binfo
			BUG_ON(rldr->rld.post.bits.dirty);	// Primary owner supposed to be clean of locks!
			nvmeibcb_dp_io_fail_mgr_binfo_copy_errfix(&so->o->nd->dp.io_stats.mgr);
			mark_blockset_info_not_written(so); // Explicit mark_blockset_info_not_written to commit empty dirty bits and fix the problem in binfo
			__dump_bug_NVMESH3032(so, "clean");
		}
		_NTSO(t01dpmseo, "COMMIT_STALE: has_stale=@BOOL_YN, has_unknowns=@BOOL_YN, pre_dbits=[@DBITS], post_dbits=[@DBITS], commit_binfo=@BOOL_YN", had_stale_lock, so->R1.is_dirty_suspect, rldr->rld.pre.bits.dirty, rldr->rld.post.bits.dirty, should_blockset_info_commit(so));
		return nvmeibcbdpec_return_to_caller_sm(so);	// Data is OK, nothing to do
	} else if (is_op_sync_commit_binfo(op)) {
		so->n_slices = 0;				// We never fix any slice (Used in 3+ mirror). Unlike EC where this sync can mutate and actually solve stale locks/bad sectors/etc
		if (had_stale_lock) {			// If there is a stale lock, I cannot fix it, mutate and commit stale lock
			so->o->op = NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE;
			_NTSO(t02dpmseo, "mutated @BLOCK_IO_OP-->@BLOCK_IO_OP: has_stale=@BOOL_YN, has_unknowns=@BOOL_YN, pre_dbits=[@DBITS], post_dbits=[@DBITS], commit_binfo=@BOOL_YN", op, so->o->op, had_stale_lock, so->R1.is_dirty_suspect, rldr->rld.pre.bits.dirty, rldr->rld.post.bits.dirty, should_blockset_info_commit(so));
			return dp_mirror_sync_execute_op(so);
		} else {
			mark_blockset_info_not_written(so);
			_NTSO(t03dpmseo, "commit pre_dbits=[@DBITS], post_dbits=[@DBITS]", rldr->rld.pre.bits.dirty, rldr->rld.post.bits.dirty);
			return nvmeibcbdpec_return_to_caller_sm(so);
		}
	} else if (op == NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON) {
		if (had_stale_lock) {
			if (num_p > 1)
				so->n_slices = 0;	// For 2-mirror, dconvict turn on actually solves the stale lock. Think why? Not for 3+ Mirror. Turn on convict and leave stale lock
		}
		return dp_maintenance_execute_op(so);
	} else {
		WARN(true, "nvmeibc bug! so=" PRI_SO_NAME " unsupported\n", PRI_SO_NAME_ARGS(so));
	}
}

/**************** No Write Hole Virtual Funcs *********************************/
u32 dp_mirror_calc_scrub_writes(struct recovery_sync_op *so)
{
	const int n_writes = dp_mirror_write_cmds_prepare(so);
	const u32 wrong_slices_mask = (so->is_sbs_mode) ? (1U << nvmeibc_sync_sl_by_sl_is_get_current_slice_index(so)) : ~0U; // Todo: EC properly writes only problematic slices. R1 for simplicity writes the entire scatter gather so all slices are considered as bad
	return n_writes ? wrong_slices_mask : 0;
}

// Virtual function called from so->restore_function: Always Syncronous
enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE dp_mirror_no_write_hole_fix(struct recovery_sync_op *so){	// Use this
	const enum nvmeib_block_io_op caller_op = so->orig_rldr->o->op;
	int n_writes;
	BUG_ON(nvmeibc_raid_is_ec(so->r1));
	// When we have the data read correctly we must copy to original IO if it was a read.
	// This is not an optimization! Not updating the caller's read data might
	// result in a DI violation ("read uncommitted").
	if (caller_op == NVMEIB_BLOCK_IO_OP_READ)
		__copy_sync_read_to_orig_read_io_sgl(so);
	n_writes = dp_mirror_write_cmds_prepare(so);
	if ((caller_op == NVMEIB_BLOCK_IO_OP_DPLIB_PROBLEM) && (n_writes > 0)) {
		nvmeibc_mark_sync_wants_to_inject_caller_sgl(so->orig_rldr);	// Dont know for now if this is read or write, so mark this flag for caller
	}
	return GOTO_NEXT_STAGE;
}

// Virtual function called from so->destroy_function
void dp_mirror_no_write_hole_destroy(struct recovery_sync_op *so) {
	const bool should_destroy = (so->R1.valid_read_index < 0) || ((int)hweight32(so->nwhole_exec_plan.invalid_sources) != n_read_cmds(so));
	WARN(!should_destroy, "nvmeibc bug: no need to destroy slice. Source=%d, invalid_bmp=0x%x, n_segs=%d\n", so->R1.valid_read_index, so->nwhole_exec_plan.invalid_sources, n_read_cmds(so));
	__set_write_buffer_to_bad_sector(so);
}

// Virtual function called from so->cleanup_function
void dp_mirror_no_write_hole_sbs_cleanup(struct recovery_sync_op *so)
{
	int i = n_read_cmds(so);
	for (; i < so->cmds->ncmds; i++) { // Check all write CMDS and replace uncorrectable with WRITE
		if (unlikely(so->cmds[i].ds->toma_acm == NVMEIBTC_DS_MODE_DEAD)) { // MUST BE 3 way mirroring or above
			BUG_ON(!so->cmds[i].do_not_send);
		} else { // Any writable seg should be ready for writing the next required slice
			struct nvmeibc_block_io_req *req = &so->cmds[i].iocmd->reqs1;
			if (so->cmds[i].do_not_send) { // Was the previous source segment
				so->cmds[i].do_not_send = false;
				// Clear write's NDB, needs to come from source
				WARN_ON(req->ndb && !so->cmds[i].is_not_ndb_owner); // Going to leak NDB or wrong ownership
				req->ndb = NULL;
			}
			req->op = NVMEIB_BLOCK_IO_OP_WRITE;	// In case we destroyed the previous slice, revert to default write
		}
	}
	__r1_valid_source_remove(so);				// Be extra carefull, protect from caller algorithm forgetting to calculate valid source of next slice
}

/**************** Mirror No Write Hole CB ************************************/
void dp_mirror_sync_cmd_cb(struct nvmeibc_block_command *cmd)
{
	struct recovery_sync_op *so;
	if (dp_sync_cmd_generic_cb(cmd) != 0)
		return;	// Waiting for at least 1 remaining read or write
	so = cmd->cmdarr->o->rso;
	BLKCMP_SO_ASYNC_RESUME_CMP(dp_mirror_sync_resume_op(so));
}

void dp_mirror_sync_resume_op(struct recovery_sync_op *so)
{	/* This state machine starts when all locks are taken and terminates in request to release locks */
	//__dump_operation(so->o);
	switch (so->o->op) {
	case NVMEIB_BLOCK_IO_OP_RECOVER_STALE:
	case NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL:
	case NVMEIB_BLOCK_IO_OP_RECOVER_DB:
	case NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING:
		dp_sync_no_write_hole_cb_stg_end(so);
		break;
	default:
		WARN(1, "nvmeibc bug! so=" PRI_SO_NAME ", stage=%d, op=0x%x\n", PRI_SO_NAME_ARGS(so), so->stage, so->o->op);
	}
}
