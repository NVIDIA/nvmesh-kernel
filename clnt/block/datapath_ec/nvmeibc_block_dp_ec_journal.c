#include "nvmeibc_block_dp_ec.h"
#include "nvmeibc_block_dp_ec_journal_common.h"
#include "block/controlpath/nvmeibc_b_cp_lost_srv_resources.h"
#include "block/controlpath/nvmeibc_b_cp_blkset_topo.h"
#include "nvmeibc_jam.h"
#include "block/recovery/nvmeibc_block_dp_sync_api.h"	// JGC sync

#define __is_seg_writable(seg) ((seg)->toma_acm != NVMEIBTC_DS_MODE_DEAD)	// Same as: !cmd->do_not_send
#define jaddr(cmd) ((cmd)->iocmd->reqs1.disk_address)
#define ILLEGAL_JADDR  (0ULL)
static void dp_ec_journal_alloc_commit_comp(struct nvmeibc_block_command *rldr, u64 *res_jlbas)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	struct nvmeibc_block_command *cmd = &rldr[mssa->n_reads];
	int ri, c, n_cmds = mssa->n_writes / 2;
	BUG_ON(mssa->no_jour);
	for (ri = 0, c = 0; c < n_cmds; c++, cmd++) {	/* Parse JAM's answer */
		if (__is_seg_writable(cmd->ds))
			jaddr(cmd) = ((res_jlbas != NULL) ? res_jlbas[ri++] : ILLEGAL_JADDR);
	}
}

bool dp_ec_journal_alloc_is_success(struct nvmeibc_block_command *rldr)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	struct nvmeibc_block_command *cmd = &rldr[mssa->n_reads];
	int i, n_cmds = mssa->n_writes / 2;
	BUG_ON(mssa->no_jour);
	for (i = 0; i < n_cmds; i++, cmd++) {
		if (__is_seg_writable(cmd->ds) && (jaddr(cmd) == ILLEGAL_JADDR))
			return false;
	}
	return true;
}

int dp_ec_journal_alloc_all_areas(struct nvmeibc_block_command *rldr)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	struct nvmeibc_block_command *cmd = &rldr[mssa->n_reads];
	int c, n_cmds = mssa->n_writes/2, rv;
	struct nvmeibc_disk *disks[N_MAX_RAID_SLICE_LEN];
	u64 dlbas[N_MAX_RAID_SLICE_LEN];
	u64 res_jlbas[N_MAX_RAID_SLICE_LEN];
	u32 n_disks = 0;
	const bool should_block_on_abandoned = rldr->was_abandoned_jr_recovered;
	unsigned long deadline_jiffies;
	BUG_ON(mssa->no_jour);

	nvmeibc_profiling_start_take_stats_for_stage(nvmeibc_get_raid_good_path_profile_for_rwt_op(rldr->ds, rldr->o->op), rldr->o, E_CMDS_STAGE_JOURNAL_ALLOC);

	/* Build the query to JAM*/
	for (c = 0; c < n_cmds; c++) {
		if (__is_seg_writable(cmd[c].ds)) {
			disks[n_disks] = cmd[c].ds->disk;
			WARN_ON(         jaddr(&cmd[c       ]) != ILLEGAL_JADDR);// Journal already allocated
			dlbas[n_disks] = jaddr(&cmd[c+n_cmds]);		 // Jam gets an input: addr of data on disk
			n_disks++;
		}
	}
	WARN_ON(n_disks == 0);

	if (!rldr->was_abandoned_jr_recovered)					// +1 ref on jour alloc start, -1 alloc end. This condition tests if +1 was already done, and alloc is retried after jgc completion
		nvmeibc_atomic_inc(&rldr->n_uncompleted_cmds);		// Support async completion of allocation, dec in completion
	rldr->jam_alloc_jif = jiffies;
	deadline_jiffies = nvmeibc_operation_get_expiry_jiffies(rldr->o);
	rv = nvmeibc_jam_lbas_alloc(n_disks, disks, rldr->rld.pre.bits.txid + 1 /* First TxID */, dlbas, res_jlbas,
			should_block_on_abandoned, &rldr->o->cpu_mask_info,
			deadline_jiffies 	/* deadline */,
			deadline_jiffies	/* priority (prioritize by IO expiry) */,
			rldr);

	if (rv != -EINPROGRESS) { // Synchronous answer
		nvmeibc_block_dp_ec_journal_alloc_cb(rv, res_jlbas, rldr); // Simulate as if it was async
	} else { // Async execution for journal is ongoing
		_ND(t_dpec02, "Journal allocation cb will return asyncronously");  // On stack variables like 'res_jlbas' are not used and Jam will allocate memory instead
		BLKCMP_IO_ONLY_IF_PRESERVE_STACK(BUG());	// This flow is not supported in fiber mode execution! Solution: Make cb() for jam which wake-up fiber and callback for block to continue. Fiber sleeps on rv of inprogress
	}
	return rv;
}

static int __jour_hot_gc_cb(void *context, int err) {
	struct nvmeibc_block_command *rldr = context;
	if (unlikely(rldr->o->topo->phased_out))
		err = -EAGAIN;
	if (unlikely(err)) {
		transition_from_journal_to_write_sm(rldr, err);
	} else {									// Retry journal allocation, but this time tell JAM to wait for A2F from Serjio.
		rldr->was_abandoned_jr_recovered = true;
		err = dp_ec_journal_alloc_all_areas(rldr);
		(void)err;	// Handled via nvmeibc_block_dp_ec_journal_alloc_cb(). Note, if pre-read stage is running, then next stage will wait for the callback. Upon journals allocation failure - Journal write stage will discover this
	}
	return 0;
}

// Journal allocation call back
void nvmeibc_block_dp_ec_journal_alloc_cb(int status, u64 *res_jlbas, void *ctx)
{
	struct nvmeibc_block_command *rldr = ctx;
	const u32 alloc_msecs = jiffies_to_msecs(jiffies - rldr->jam_alloc_jif);
	BUG_ON(status == -EINPROGRESS);

	if (alloc_msecs > 1000) {
		struct operation *o = rldr->o;
		const u32 vol_id = nvmeibc_volume_short_id(o->nd);
		_NW(t_dpec03, "Slow JAM Allocation  (@MILISECONDS ms) - rv=@RV "
		"Volume: @DEV_NAME - {@O_DBG_ID} OPCODE: @OP_CODE, VOL_MINOR: @VOL_ID, TOPOLOGY: @TOPO_DBG_ID",
		    alloc_msecs, status, o->nd->name, o->dbg_id, o->op, vol_id, o->topo->debug_unique_index);
	}
	dp_ec_journal_alloc_commit_comp(rldr, (status == 0) ? res_jlbas : NULL);

	if (status == -EDEADLK) {
		BUG_ON(rldr->was_abandoned_jr_recovered);
		BUG_ON(rldr->cmdarr != rldr);		// The code below will not work otherwise, correct solution: use clmat to find primaty owner lock of rldr
		nvmeibc_sync_jour_hot_gc(rldr->locksets, __jour_hot_gc_cb, rldr);
		return; // Do not complete journal alloc "cmd" yet - will return here on retry completion or completed on sync failure.
	}

	nvmeibc_profiling_end_take_stats_for_stage(nvmeibc_get_raid_good_path_profile_for_rwt_op(rldr->ds, rldr->o->op), rldr->o, E_CMDS_STAGE_JOURNAL_ALLOC, status);
	if (status != 0)
		OPERATION_DBG_CNTR_INC(rldr->o, n_jam_alloc_failed);
	transition_from_journal_to_write_sm(rldr, 0 /* Success*/);
}

static void __calc_is_roll_fwd_guaranteed(struct nvmeibc_block_command *rldr, u16 wr_succeeded, bool did_data_cmd_failed)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	const struct nvmeibc_raid1* pr = nvmeibc_disk_segment_get_praid(rldr->ds);
	const struct nvmeibc_roles_bmps *bmp = &pr->calculated_data.roles_bmps[mssa->owner_seg];
	const bool can_recoverer_skip_rollfwd = __is_bmp_included_in(bmp->raid.data, bmp->readable);	// If All datas OK -> can loose all parities -> no rollfwds are performed
	const bool has_journal = (!mssa->no_jour);
	const bool can_data_corruption_occur = (did_data_cmd_failed && can_recoverer_skip_rollfwd);

	if (has_journal && (!can_data_corruption_occur)) {
		const u16 max_can_die = hweight16(bmp->readable) - pr->slice_size;
		rldr->is_roll_fwd_guaranteed = (wr_succeeded > max_can_die);
	}
}

/* Note: This function has ~simmilar condition to dp_ec_calc_should_abandon for
   owner locks, coz journal and locks are abandoned together for roll forward */
static bool __ec_should_abandon_journal(struct nvmeibc_block_command *rldr)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	struct t_abandon aban = {0};
	struct nvmeibc_block_command *data_cmds = &rldr[mssa_get_data_start_index(mssa)];
	int ndp_writes = mssa_get_write_count(mssa), ci;
	bool rv = false, did_rdma_appendix_failed = false, did_data_cmd_failed = false;
	if (!rldr->was_journ_success)
		goto _out;					// No roll forward on transaction. Retry IO
	for (ci = 0; ci < ndp_writes; ci++) {
		if (unlikely(data_cmds[ci].do_not_send)) {
			continue;					// This command cannot corrupt data on disks
		} else if (dp_cmds_was_cmd_not_sent(data_cmds[ci].o_rv)) {
			if (!data_cmds[ci].is_parity) did_data_cmd_failed = true;
			aban.wr_not_issued++;
		} else if (data_cmds[ci].o_rv != 0) {
			if (!data_cmds[ci].is_parity) did_data_cmd_failed = true;
			aban.wr_failed++;
		} else {
			aban.wr_succeeded++;
		}
	}
	if (rv_storage_of_binfo_write(rldr)) {		// Regardless if it is do_not_send or not! Because if journal succeeded then we test only relevant o_rv
		did_rdma_appendix_failed = true;				//
	}
	rv = ((aban.wr_not_issued && aban.wr_succeeded) ||  // Possible roll forward
		  (aban.wr_failed) ||
		  (did_rdma_appendix_failed));					// Possible slice corruption. Daniel: Need to re-think this. Maybe an overkill
_out:
	rldr->was_transaction_abandoned = rv;				// Save for locks abandoning
	if (unlikely(rv)) {
		extern bool qa_ec_stress_debug;
		if (qa_ec_stress_debug)
			__calc_is_roll_fwd_guaranteed(rldr, aban.wr_succeeded, did_data_cmd_failed);																		/// For now, later print entire txbm per slice
		_NTO(trace_should_aband, rldr->o, "Abandon jour: txbms={@TXBM,@TXBM}, not_is=@RV, succ=@RV, failed=@RV apndx=@BOOL_YN, is_rfwd=@BOOL_YN, data_cmd_failed=@BOOL_YN", mssa->tx_bms[0], mssa->tx_bms[1], aban.wr_not_issued, aban.wr_succeeded, aban.wr_failed, did_rdma_appendix_failed, rldr->is_roll_fwd_guaranteed, did_data_cmd_failed);
	}
	return rv;
}

static void __journals_abandon(struct nvmeibc_block_command *cmd, u32 n_cmds)
{
	struct nvmeibc_b_cp_loser *loser = &cmd->ds->toma_reg->hdr->loser;
	ulong flags;
	s16 jentry[N_MAX_RAID_SLICE_LEN] = {0};	//Daniel: Save space on stack
	u8 jentry_gen_id[N_MAX_RAID_SLICE_LEN] = {0};
	u32 c;
	for (c = 0; c < n_cmds; c++) {
		if (__is_seg_writable(cmd[c].ds)) {
			u64 *j_addr = &jaddr(&cmd[c]);
			jentry[c] = (s16)nvmeibc_jam_abandon_lba(
				cmd[c].ds->disk, *j_addr, &jentry_gen_id[c]);
			if (jentry[c] >= 0) {
				BUG_ON(jentry_gen_id[c] < nvmeib_jrnl_ent_gen_id_min ||
					jentry_gen_id[c] > nvmeib_jrnl_ent_gen_id_max);
			}
			*j_addr = ILLEGAL_JADDR;	// Mark that blocks were abandoned to jam
		}
	}

	// Add the abondoned journals into praids loser
	spin_lock_irqsave(&loser->lock, flags);	// Other IO's may also abandon
	for (c = 0; c < n_cmds; c++) {
		struct nvmeibc_disk_segment *ds = cmd[c].ds;
		if (__is_seg_writable(ds) && (jentry[c] >= (s16)0)) {
			nvmeibc_b_cp_loser_aband_jour(loser, ds->toma_reg, jentry[c], jentry_gen_id[c]);
		}	// if jentry == -1, JAM could not abandon, PAUSE will arrive
	}
	spin_unlock_irqrestore(&loser->lock, flags);

}

static void __journals_free(struct nvmeibc_block_command *cmd, u32 n_cmds)
{
	struct nvmeibc_disk *disks[  N_MAX_RAID_SLICE_LEN];
	u64                 res_addr[N_MAX_RAID_SLICE_LEN] = {ILLEGAL_JADDR};
	u32 jrnl_state_unkn = 0;
	u32 n_disks = 0, c;

	for (c = 0; c < n_cmds; c++) {	/* Build the query to JAM*/
		if (__is_seg_writable(cmd[c].ds)) {
			u64 *j_addr = &jaddr(&cmd[c]);
			disks[   n_disks] = cmd[c].ds->disk;
			res_addr[n_disks] = *j_addr;
			if (cmd[c].o_rv)
				jrnl_state_unkn |= (1<<n_disks);	// Write succeeded, previous journal was overwritten
			n_disks++;
			*j_addr = ILLEGAL_JADDR;	// Mark that blocks were returned to jam
		}
	}

	WARN((n_disks == 0), "nvmeibc bug! ndisks==0\n");
	if (unlikely(res_addr[0] == ILLEGAL_JADDR)) {	// Don't relase, JAM take failed
		for (c = 0; c < n_disks; c++) {	// Verify not partialy allocated
			WARN_ON(res_addr[c] != ILLEGAL_JADDR);
		}
	} else {
		nvmeibc_jam_lbas_free(n_disks, disks, res_addr, jrnl_state_unkn);
	}
}

void dp_ec_journal_release_areas(struct nvmeibc_block_command *rldr)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	struct nvmeibc_block_command *cmd = &rldr[mssa->n_reads];
	const u32 n_cmds = mssa->n_writes / 2;
	const bool should_abandon_jour = __ec_should_abandon_journal(rldr);
	BUG_ON(mssa->no_jour);
	if (!should_abandon_jour)
		__journals_free(cmd, n_cmds);
	else {
		__journals_abandon(cmd, n_cmds);
	}
}

bool dp_cmds_does_require_jam(const struct nvmeibc_block_command *cmd)
{
	return (cmd->my_stage == E_CMDS_STAGE_WRITE_JOURNAL);
}

