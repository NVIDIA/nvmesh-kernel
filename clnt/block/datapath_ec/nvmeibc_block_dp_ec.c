#include "nvmeibc_block_dp_ec.h"
#include "nvmeib_error_report.h"
#include "nvmeibc_block_dp_ec_journal_common.h"
#include "nvmeibc_block_dp_ec_reed_solomon.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_cmd_lock_link.h"
#include "../datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeibc_jam.h"
#include "recov/nvmeibc_block_dp_ec_recov_maintenance.h"
#include "recov/nvmeibc_block_dp_ec_recovery_common.h"
#include "block/controlpath/nvmeibc_b_cp_blkset_topo.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_profiling_disk_stages.h"
#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.h"

bool qa_ec_stress_debug = false;		 // LKJ: EC-1480 Only for stress testing, remove in product
module_param(qa_ec_stress_debug, bool, 0644);
MODULE_PARM_DESC(qa_ec_stress_debug, "For QA only! Over stress EC datapath");

/*
	TODO: Review documentation and update if required
   Snake Size = 1 for the examples below, extended snake information below
   Erasure coding slice as extension of N-replica
   Erasure coding D+P has D data segments and P parities. It is a natural
   extension of N-mirroring as D=1, P=N-1.
   * Slice size = D. Writing D blocks will fill the entire slice (including
   additional parity blocks).
   * Lock - protects slice size * LOCKSET_SLICES [blocks]
   * N-replica Owner: There were 2 types of owners: owner lock (where do we
	 lock the primary owner lock) and owner-read - from where do we read. The
	 read owner rotates among all the segments and changes when disks are
	 are down (degraded mode).
	 Typically when we use N-locking for N-replica the owner lock and read lock
	 are identical
   * EC Owner: Owner lock is identical to N-replica owner lock. However read
	 owner is now denoted as slice start (first segment in a slice). It does not
	 change when disks go down. Example D+P is 6+2: Slice start is at 0
	 Writing 6 data blocks from offset 0 will fill the entire slice
	 Segments: 0, 1, 2, 3, 4, 5, 6, 7
			   D0,D1,D2,D3,D4,D5,P1,P2
	 Writing 3 blocks from offset 5 will fill 7 blocks
	 Segments: 0, 1, 2, 3, 4, 5, 6, 7
			   __,__,__,__,__,D0,P1,P2
			   D1,D2,__,__,__,__,P3,P4
	 Now the same example where read-owner (slice start was rotated to 4)
	 Writing 6 data blocks from offset 0
	 Segments: 0, 1, 2, 3, 4, 5, 6, 7
			   D4,D5,P1,P2,D0,D1,D2,D3
	 Writing 3 blocks from offset 5 will fill 7 blocks
	 Segments: 0, 1, 2, 3, 4, 5, 6, 7
			   __,D0,P1,P2,__,__,__,__
			   __,__,P3,P4,D1,D2,__,__
	 Note: Withing all vlbas in a lock the ownership of lock and the slice start
		   remain unchanged, they do rotate when advancing to next lock in
		   higher vlbas.

	 WRITE/READ commands in good path, no degraded mode:
	 1. Data commands start from the first block. At most slice size commands
	 2. Pariry commands (for write only) start from first parity

	 READ-Lock Piggyback: Irrelevant to Raid5. When a few blocks are read from
	 a slice the lock should be viewed after the last command (assuming any
	 writehole/stale lock is solved via roll forward by the journal). Moreover
	 if only a single block is read the probability to piggiback a lock on it
	 is very small (1/slice_size). In degraded mode read must always take locks

	Snake Size > 1:
	The data will be consecutive on each segment for snake_size blocks
	Parities are still slice based but the blocks are not consecutive in the volume.
	Example Snake Size = 4, D = 6, P = 2, Entire Snake Size Snake Size
	Segments:  0, 1, 2, 3, 4, 5, 6, 7
			  0 ,4 ,8 ,12,16,20,P1,P2
			  1 ,5 ,9 ,13,17,21,P3,P4
			  2 ,6 ,10,14,18,22,P5,P6
			  3 ,7 ,11,15,19,23,P7,P8
	Snake# 2:
			  24,28,32,36,40,44,P9,Px
			  25,29,33,37,41,45,Px,Px
			  26,30,34,38,42,46,Px,Px
			  27,31,35,39,43,47,Px,P16
	Using the above example with snake:
	 Writing 6 data blocks from offset 0 will not fill an entire slice
	 Segments: 0, 1, 2, 3, 4, 5, 6, 7
			   D0,D4,  ,  ,  ,  ,P1,P2
			   D1,D5,  ,  ,  ,  ,P3,P4
			   D2,  ,  ,  ,  ,  ,P5,P6
			   D3,  ,  ,  ,  ,  ,P7,P8

	For simplicity we split non-snake aligned writes into partial and aligned operations
	There is a limit to the number of snakes that can be written consecutively
	binje is configurable and can work with snake size = 1 as well,
	replacing old multi-slice notion.

	MSSA:
	In order to address the addition of the snake size parameter we added the
	MSSA componenet Multi-Snake-Slice-Analyzer.
	It replaces the cmap/io_addr previously help by the raid leader (and unused by non-rldrs)
	The MSSA fills a 2D map with the recieved BIO, the map will reflect the example above.
	Where D0-D5 bits are set and the rest remain 0.
	If the operation is a non-degraded read, we are almost done.
	For writes, we iterate on the map row by row, adding parities to the map if they are not degraded (Dead)
	Then we decide of the strategy for the slice:
	1. Complement strategy (AKA Full slice) - we read all missing data blocks and calculate parities.
	2. Update strategy - we read the same written blocks, parities included.
	3. Restore strategy - we cannot update nor complement, we need to restore a single segment and calculate full slice with old, recovered, and new blocks

	For the additional reads we add a pre_read map that indicates which reads are required for the operation
	If a read needs to restore part of the data it will add pre-reads as well in the same manner.
	For simplification after setting both maps, read operations add the bio map bits into the pre_read map
	for a single unified map for iterating. We still decern between bio blocks and pre_read with testing the bio_map.

	After this phase we calculate how many columns the operation has, and define
	how many read and write commands are required.
	The MSSA stores short cuts to allow access to all required data during operation.
	It has a 2D map of pointers directly to the data of blocks used in the operation.
	It has an array for translating a colume to the command index by type:
	Read, Write, Jour. If no command exists it will return -1 allowing skip of unrequired commands.
	To reduce calculations during the datapath, preparing the MSSA also caches the
	bitmap results per column and row (TBD).


 MSSA implementation for Snake:

 Pre-read + read hole example:
	D + P = 2 + 2
	Snake size = 3
	D0 is degraded
	First blockset read starts at block 2 for 2 blocks:
	Mssa:
	Bio:
	Sl	|D0|D1|P |Q |
	0	|  |Rd|  |  |
	1	|  |  |  |  |
	2	|Rd|  |  |  |
	Pre-read:
	Sl	|D0|D1|P |Q |
	0	|  |  |  |  |
	1	|  |  |  |  |
	2	|  |PR|PR|  |
	Merged:
	Sl	|D0|D1|P |Q |
	0	|  |Rd|  |  |
	1	|  |  |  |  |
	2	|Rd|PR|PR|  |

	1 Read to D0 do_not_send
	2 separate Reads to D1, first one block from slice 0, second one block from slice 2
	1 Read to P from slice 2

 Write - Parity hole expamples:
	D + P = 2 + 2
	Snake size = 3
	First blockset write starts at block 2 for 2 blocks:
	Mssa:
	Bio:
	Sl	|D0|D1|P |Q |
	0	|  |Wr|Wr|Wr|
	1	|  |  |  |  |
	2	|Wr|  |Wr|Wr|
	Pre-read:
	Sl	|D0|D1|P |Q |
	0	|PR|  |  |  |
	1	|  |  |  |  |
	2	|  |PR|  |  |

	After MSSA:
	(W* no new info must write what was read in R*, do not change MD either)
	Sl	|D0|D1|P |Q |
	0	|  |Wr|Wr|Wr|
	1	|  |  |W*|W*|
	2	|Wr|  |Wr|Wr|
	Pre-read:
	Sl	|D0|D1|P |Q |
	0	|PR|  |  |  |
	1	|  |  |R*|R*|
	2	|  |PR|  |  |

	The above explanation applies also for UPDATE_STRATEGY if D > 4 it's less reads to read DX + P + Q Than 4 remaining Ds:
	D + P = 5 + 2
	Same IO this time to reduce reads we will choose to update the parities:
	Sl	|D0|D1|D2+|P |Q |
	0	|  |Wr|   |Wr|Wr|
	1	|  |  |   |  |  |
	2	|Wr|  |   |Wr|Wr|
	Pre-read:
	Sl	|D0|D1|D2+|P |Q |
	0	|  |PR|   |PR|PR|
	1	|  |  |   |  |  |
	2	|PR|  |   |PR|PR|

	MSSA will set all slices between writes set for write, and for each such slice will set for read
	Sl	|D0|D1|D2+|P |Q |
	0	|  |Wr|   |Wr|Wr|
	1	|  |  |   |W*|W*|
	2	|Wr|  |   |Wr|Wr|
	Pre-read:
	Sl	|D0|D1|D2+|P |Q |
	0	|  |PR|   |PR|PR|
	1	|  |  |   |R*|R*|
	2	|PR|  |   |PR|PR|

	The other option, first slices and last slices update and inbetween full slice (can be the other way around but that is not a problem):
	Write is from lba 1 size 4 blocks
	Sl	|D0|D1|D2|D3|D4|P |Q |
	0	|  |Wr|  |  |  |Wr|Wr|
	1	|Wr|Wr|  |  |  |Wr|Wr|
	2	|Wr|  |  |  |  |Wr|Wr|
	Pre-read:
	Sl	|D0|D1|D2|D3|D4|P |Q |
	0	|  |PR|  |  |  |PR|PR|
	1	|  |  |PR|PR|PR|  |  |
	2	|PR|  |  |  |  |PR|PR|

	MSSA will split reads for parities (** will not be read)
	Pre-read:
	Sl	|D0|D1|D2|D3|D4|P |Q |
	0	|  |PR|  |  |  |PR|PR|
	1	|  |  |PR|PR|PR|**|**|
	2	|PR|  |  |  |  |PR|PR|

	Otherway no problem, no "holes":
	Sl	|D0|D1|D2|D3|D4|P |Q |
	0	|  |Wr|Wr|  |  |Wr|Wr|
	1	|  |Wr|  |  |  |Wr|Wr|
	2	|Wr|Wr|  |  |  |Wr|Wr|
	Pre-read:
	Sl	|D0|D1|D2|D3|D4|P |Q |
	0	|PR|  |  |PR|PR|  |  |
	1	|  |PR|  |  |  |PR|PR|
	2	|  |  |PR|PR|PR|  |  |
*/

int dp_ec_should_ignore_op(const struct operation *o)
{
	if (unlikely(o->op == NVMEIB_BLOCK_IO_OP_DISCARD))
		return 1;		// Ignore with success
	if (unlikely(!is_io_aligned(o->bios[0])))
		return -EINVAL;	// Relevant for non Trims only
	return 0;			// Continue to execution
}

/******************************************************************************/
#define __has_pre_reads(rldr)  (rldr->o->mssa->n_reads > 0)

static int __analyze_binfo_sm_cb_b4w(void *context, int err);

void transition_from_journal_to_write_sm(struct nvmeibc_block_command *rldr, int err) {
	const bool explicit_transition = !__has_pre_reads(rldr);
	dp_cmds_complete_cmd(rldr->cmdarr, rldr->my_leader, NULL);	// As opposed to +1 ref for requesting journal state machine
	if (explicit_transition) { // Pre-reads were not launched in parallel to journal, transition to io state machine or autofail
		const int next_rv = (((err == 0) && dp_ec_journal_alloc_is_success(rldr)) ? 0 : -ENXIO);
		// cannot dump since this is indication for use after free
		BUG_ON(nvmeibc_atomic_read(&rldr->n_uncompleted_cmds) <= 0);	// Sanity: At least one completion needed for the state machine we are transitioning too.
		__analyze_binfo_sm_cb_b4w(rldr, next_rv);
	} else { // pre-reads were launched, cannot handle error here. Journal write stage will discover that journals are not allocated and will fail
	}
}

/******************************************************************************/
// MSSA print adds per-slice data for each segment, if it's part of BIO or pre-read
static const char* __wr_cmd_type_tostring(const struct nvmeibc_block_command *c,
							   bool is_update, bool has_protection)
{
	is_update = is_update && has_protection;
	switch ((enum e_cmds_stage)c->my_stage) {
	case E_CMDS_STAGE_READ_PRE_DATA : return (is_update?"Read Old":"Read Unch");
	case E_CMDS_STAGE_CALC_PARITIES : return "Calc Par";
	case E_CMDS_STAGE_WRITE_JOURNAL : return "Wr Jour";
	case E_CMDS_STAGE_DO_IO_AND_PAR : return (c->is_parity?"Wr Par":"Wr Data");
	case E_CMDS_STAGE_DELETE_JOURNAL: return "Del Jour";
	default: return "Unknown";
	}
}

// We no longer send reads in DO_IO_AND_PAR all are in pre-read
// MSSA print adds per-slice data for each segment, if it's part of BIO or pre-read
// This is just generic to ensure unreadable segments do not go to transport
static const char* __rd_cmd_type_tostring(const struct nvmeibc_block_command *c)
{
	if (c->my_stage != E_CMDS_STAGE_READ_PRE_DATA) {
		return "Unknown";
	}
	return  (c->do_not_send) ? "Unsent Read" : "Sent Read"; // Details in MSSA print
}

// Must have MSSA
static void __calc_and_apply_io_dbits_action(struct nvmeibc_block_command *rldr)
{
	struct nvmeibc_dbits_tx dbmap;
	const union nvmeibc_dbits_entry pre = { .all_bits = rldr->rld.pre.bits.dirty };

	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(rldr->ds);
	sgmnts_bmp_t turn_on_dbit_bmp, turn_off_dbit_bmp;
	turn_on_dbit_bmp = (nvmeibc_mssa_calc_write_bmp(rldr->o->mssa) & nvmeibc_raid1_get_sgmnts_bmp(pr, dbits_on_mask));
	turn_off_dbit_bmp = (nvmeibc_mssa_calc_full_blockset_write_bmp(rldr->o->mssa) & nvmeibc_raid1_get_sgmnts_bmp(pr, dbits_off_mask));
	nvmeibc_dbits_tx_init_by_bmp(&dbmap, nvmeibc_raid1_get_protect_lvl(pr), turn_on_dbit_bmp, turn_off_dbit_bmp, 0 /* turn_on_conv_bmp */);
	rldr->rld.post.bits.dirty = nvmeibc_dbits_tx_apply(&pre, &dbmap);
}

/* Update the TxID&Dbits on all parity members & owner lock */
static void dp_ec_set_tx_id_update_piggyback(struct nvmeibc_block_command *rldr)
{
	// TODO without column to cmd if possible (this is efficient)
	struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	struct nvmeibc_cmd_lock *data_lock;
	const union nvmeib_blkset_info jour_pig = nvmeibc_rldr_get_post_stage_rdma_piggyback(&rldr->rld, E_CMDS_STAGE_POST_JR_RDMA);
	const union nvmeib_blkset_info io_pig =   nvmeibc_rldr_get_post_stage_rdma_piggyback(&rldr->rld, E_CMDS_STAGE_POST_IO_RDMA);
	const bool is_post_io_rdma_and_pig_required = (jour_pig.all != io_pig.all);

	int column;
	WARN(!rldr->o->mssa->no_rw_p && (rldr->rld.post.bits.txid == rldr->rld.pre.bits.txid)
		 , "nvmeibc bug txid=%d\n", rldr->rld.pre.bits.txid);	// txid should be incremented with every write, except the double dead parities case in such case, we don't write journal

	/* Binfo set as piggyback on parity members (Journal & IO stages) */
	for (column = mssa->slice_size; column < mssa->replicas && !mssa->no_jour; column++) {
		int i = mssa->column_to_cmd.jour[column];
		if (i >= 0)
			dp_cmds_piggyback_info_on_write(&rldr[i], jour_pig);
		if (is_post_io_rdma_and_pig_required) { // If JR RDMA is the same as IO RDMA it's redundant
			i = mssa->column_to_cmd.write[column];
			if (i >= 0)
				dp_cmds_piggyback_info_on_write(&rldr[i], io_pig);
		}
	}

	data_lock = __get_data_lock(rldr);	/* Find data_lock if exists */
	if (data_lock == NULL) // No data lock required (must be dead seg)
		goto _out;

	/* Write binfo also to data lock. If cmd to its seg exists: piggyback it, otherwise need separate rdmas */
	if (mssa->column_to_cmd.write[0] >= 0) {			// Can piggyback IO to D0
		WARN_ON(rldr[mssa->column_to_cmd.write[0]].ds != data_lock->ds);	// Todo: remove, just temp debug code
		if (is_post_io_rdma_and_pig_required)
			dp_cmds_piggyback_info_on_write(&rldr[mssa->column_to_cmd.write[0]], io_pig);
		if (!mssa->no_jour) {   							// Can piggiback to D0 cmds both journal and io
			dp_cmds_piggyback_info_on_write(&rldr[mssa->column_to_cmd.jour[0]], jour_pig);
			goto _out;									// Journal + IO piggybacked, we are done
		}
	} else {											// Nothing can be piggybacked
		if (is_post_io_rdma_and_pig_required) {
			rldr->use_io_apend_stages = true;   		// Send in another RDMA
			rldr->use_io_apend_stages_data_lock = true;
		}
	}
	rldr->use_jr_apend_stage_data_lock = true;  		// We have a data_lock that has no journal cmds to it. Send in another RDMA. Regardless if we have jour cmds but none to data-lock or dont have journals at all
_out:;
}

#define nvmeibc_mssa_get_row_tx_bm(mssa, row) (GENMASK((mssa)->slice_size - 1, 0) & nvmeibc_mssa_get_row_map((mssa),row,true))

/* prepare the MD objects for cmds writing the journal */
// We cannot access journal MD directly and need to use the command
static void prepare_journal_wr_cmds_md(struct nvmeibc_block_command *rldr)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	const u32 tx_id = rldr->rld.post.bits.txid;
	u32 b;
	int map_i, col, row, ci = 0;
	for_each_column_for_each_snake_while(mssa, mssa->bio_map, map_i, col, row, (ci++ < (mssa->n_writes / 2))) {
		struct nvmeibc_block_command *j_cmd =  &rldr[mssa->column_to_cmd.jour[ col]];
		struct nvmeibc_block_command *io_cmd = &rldr[mssa->column_to_cmd.write[col]];
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(j_cmd->ds);
		for (b=0; b < j_cmd->nlbas; b++) {	// Cannot use mssa->blocks_md, it's the data block MD not journal
			union jblock_md *md = (j_cmd->iocmd->reqs1.md + (b*md_size));
			roles_bmp_t tx_bmp = nvmeibc_mssa_get_row_tx_bm(mssa, row + b);
			bool has_next = (b < j_cmd->nlbas - 1), has_prev = (b > 0);
			nvmeibc_block_dp_ec_jmd_encode(md, io_cmd->iocmd->reqs1.disk_address + b, tx_id, tx_bmp, has_next, has_prev);
		}
	} for_each_column_for_each_snake_while_end;
}

/* prepare the MD objects for cmds writing the data */
static void __prepare_data_wr_cmds_md_with_journal(struct nvmeibc_block_command *rldr)
{
	#define __get_d2j_of_cmd(c)   (u32)(((c)->ds->disk->jour.rng_id == NVMEIB_EC_INVALID_JOURNAL_RANGE ? \
						NVMEIB_EC_INVALID_JOURNAL_ENTRY : \
						((c)->iocmd->reqs1.disk_address - (c)->ds->disk->jour.rng_slba) / (c)->ds->disk->jour.rng_binje))
	#define __get_client_id( c) 										((c)->ds->disk->jour.rng_id)
	union nvmeibc_dbits_entry dbits = { .all_bits = rldr->rld.post.bits.dirty };
	const u32 tx_id = rldr->rld.post.bits.txid;
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	int map_i, col, row, ci = 0;
	u32 b;
	for_each_column_for_each_snake_while(mssa, mssa->bio_map, map_i, col, row, (ci++ < (mssa->n_writes / 2))) {
		struct nvmeibc_block_command *io_cmd = &rldr[mssa->column_to_cmd.write[col]];
		struct nvmeibc_block_command *jrnl_cmd = &rldr[mssa->column_to_cmd.jour[col]];
		const u32 d2j = __get_d2j_of_cmd(jrnl_cmd);			// Journal cmd of this data command
		int map_index_for_column = map_i;
#ifdef DEBUG_SAVE_JENTRY
		if (!io_cmd->do_not_send) {
			/* SANITY - Make sure d2j does not overflow */
			if (unlikely(d2j >= (1 << NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS))) {
				_NE(err_prepare_data_wr_cmds_md_with_journal_d2j_overflow,
					DMESG_PREFIX("@DEV_NAME") ": D2J: @UINT Overflows bitfield - jrnl_cmd @BLOCK_COMMAND disk_address @DISK_ADDRESS rng_slba @SLBA_LONG rng_binje @BINJE io_cmd @BLOCK_COMMAND ",
					rldr->o->nd->name, d2j, jrnl_cmd,
					jrnl_cmd->iocmd->reqs1.disk_address,
					jrnl_cmd->ds->disk->jour.rng_slba,
					jrnl_cmd->ds->disk->jour.rng_binje, io_cmd);
				BUG();
			}
		}
#endif
		for (b=0; b < io_cmd->nlbas; b++) {								// Slice iterator
			union nvmeibc_block_dp_ec_data_block_md *md = mssa->blocks_md[map_index_for_column];
			if (!io_cmd->is_parity)						// Compiler will move this out of the loop
				nvmeibc_block_dp_ec_md_make_d(md, md->D.edic, __get_client_id(io_cmd), tx_id,   	 d2j);
			else { // Todo: clear dbit turn on / off bit on per slice calcualtion (multi-slice 'rld' value is an overkill)
				nvmeibc_block_dp_ec_md_make_p(md, md->P.edic, __get_client_id(io_cmd), tx_id, dbits, d2j);
			}
			advance_rlba_to_next_slice(map_index_for_column, mssa->snake_size, mssa->replicas);
		}
	} for_each_column_for_each_snake_while_end;
}

static void __prepare_data_wr_cmds_md_no_journal(struct nvmeibc_block_command *rldr)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	int map_i, col, row, ci = 0;
	u32 b;
	for_each_column_for_each_snake_while(mssa, mssa->bio_map, map_i, col, row, (ci++ < mssa->n_writes)) {
		struct nvmeibc_block_command *io_cmd = &rldr[mssa->column_to_cmd.write[col]];
		int map_index_for_column = map_i;
		for (b=0; b < io_cmd->nlbas; b++) {								// Slice iterator
			union nvmeibc_block_dp_ec_data_block_md *md = mssa->blocks_md[map_index_for_column];
			if (!io_cmd->is_parity)						// Compiler will move this out of the loop  		  d2j_rng (unused)
				nvmeibc_block_dp_ec_md_make_d(md, md->D.edic, JRI_MARK_NO_JOURNAL,  rldr->rld.post.bits.txid, 0);
			else { // Todo: clear dbit turn on / off bit on per slice calcualtion (multi-slice 'rld' value is an overkill)
				const union nvmeibc_dbits_entry binfo_dbits = {.all_bits = rldr->rld.post.bits.dirty};
				nvmeibc_block_dp_ec_md_set_txid_and_dirty_bits_value_mark_no_journal(md, rldr->rld.post.bits.txid, binfo_dbits);
			}
			advance_rlba_to_next_slice(map_index_for_column, mssa->snake_size, mssa->replicas);
		}
	} for_each_column_for_each_snake_while_end;
}

static void prepare_data_wr_cmds_md(struct nvmeibc_block_command *rldr){
	if (rldr->o->mssa->no_jour)
		__prepare_data_wr_cmds_md_no_journal(rldr);
	else
		__prepare_data_wr_cmds_md_with_journal(rldr);
}
/* Full slice write skipped pre-read. So, need to call the on_stage_end() function for that stage */
static void __do_missed_stages_of_full_slice_write(struct nvmeibc_block_command *rldr, int *rv)
{
	rldr->raid_cur_stage = rldr->raid_first_stage;
	if ((!(*rv)) && rldr->raid_cur_stage == E_CMDS_STAGE_CALC_PARITIES) {
		NVMESH_BUG((!rldr->o->mssa->no_jour && (rldr->raid_cur_stage > E_CMDS_STAGE_WRITE_JOURNAL)), __dump_operation_report, rldr->o,
			   "EC, always must write journals no_jour=%d raid_cur_stage=%u",
			   rldr->o->mssa->no_jour, rldr->raid_cur_stage);
		// EC, always must write journals, could not skip this - of course if the journal is relevant
		// Unwind stage to PRE_READ and then call on stage end (will set journal MD correctly)
		rldr->raid_cur_stage = E_CMDS_STAGE_READ_PRE_DATA;
		while (rldr->raid_cur_stage < rldr->raid_first_stage){
			dp_ec_exec_func_on_stage_end(rldr, rv);
		}
	}
}

/* EC IO sends sync requests while holding locks via this function. Cant fail */
static void nvmeibcbdpec_io_req_sync(struct nvmeibc_block_command *rldr,
		nvmeibc_sync_cb_t done_cb, enum nvmeib_block_io_op op)
{
	int ow_i = nvmeibc_cllink_find_lock_by_cmd(rldr), rv;
	struct nvmeibc_cmd_lock *lo = &rldr->cmdarr->locksets[ow_i];
	rldr->raid_cur_stage = E_CMDS_STAGE_BLOCKSET_FIXUP;
	if (op == NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO) {
		rv = nvmeibc_sync_unknown_binfo_by_io( lo, done_cb, rldr);
	} else if (op == NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP) {
		rv = nvmeibc_sync_txid_wrap(lo, done_cb, rldr);
	} else if (op == NVMEIB_BLOCK_IO_OP_RECOVER_DB) {
		rv = nvmeibc_sync_recover_dirty(lo, done_cb, rldr);
	} else { rv = -1; WARN(true, "nvmeibc bug. op=0x%x\n", op); /* Should never happen! */ }
	if (rv < 0)
		done_cb(rldr, -ENOEXEC);
}

#ifdef BLKCMP_IO_COMPLETION_PRESERVE_STACK
	static int __um_completion_unblock_waiting_stack(void* context, int err)
	{
		struct nvmeibc_block_command *rldr = context;
		struct operation *o = rldr->o;
		// dump with operation
		NVMESH_BUG(rldr->o_rv != 0, __dump_operation_report, rldr->o, "rldr.o_rv=%d", rldr->o_rv);	// If has error, we should have autofailed the io, not launching syncs
		rv_storage_of_sync_error(rldr) = err;
		BLKCMP_IO_ASYNC_RESUME_CMP(o);
		return 0;
	}
	static int __fiber_io_calls_sync(struct nvmeibc_block_command *rldr, enum nvmeib_block_io_op op)
	{
		struct operation *o = rldr->o;
		int sync_rv;
		BLKCMP_IO_ASYNC_AWAIT(nvmeibcbdpec_io_req_sync(rldr, __um_completion_unblock_waiting_stack, op));
		sync_rv = rv_storage_of_sync_error(rldr);	// Wakup after sync and return the error
		rv_storage_of_sync_error(rldr) = 0;
		return sync_rv;
	}
	#define __call_assist_sync(rldr, done_cb, op)    err = __fiber_io_calls_sync(rldr, op);       BLKCMP_SO_ASYNC_RESUME_CUR();
#else
	#define __call_assist_sync(rldr, done_cb, op)    nvmeibcbdpec_io_req_sync(rldr, done_cb, op); return 0;
#endif

/* cb and state machine which fixed blockset info problems, before journal
   allocation starts. Allways returns zero and transitions to
   __analyze_binfo_sm_cb_b4w state machine */
static int __analyze_binfo_sm_cb_b4j(void* context, int err)
{
	struct nvmeibc_block_command *rldr = context;
	struct operation *o = rldr->o;
	const bool has_pre_reads = __has_pre_reads(rldr);	// Must cache on stack!
	const bool has_writable_pari = !o->mssa->no_rw_p;
BLKCMP_IO_ONLY_IF_PRESERVE_STACK(_func_start:)
	if (unlikely(o->topo->phased_out)) {
		err = -EAGAIN;
	}
	if (unlikely(err)) { // Error must skip all TXID/DB phases
		nflog(t_dpec00, "Aborting maintanance steps on topo=@TOPO_DBG_ID, err=@ERR", o->topo->debug_unique_index, err);
		rldr->rld.post = rldr->rld.pre;	// Crucial step! Post might be different from pre like txid+1 but we cannot apply the action that commits post so it is unrolled to pre. Example: Txid wraparoudn failed so post.txid is invalid value which should never be used
		// Note: here prea and post may be both inalid for IO (unknown TxID) because maintanace op failed. We may transfer the lock with Unknown TxID
		__analyze_binfo_sm_cb_b4w(rldr, err); // Continue error flow to start autofail
		return 0;
	}

	if (unlikely(dp_ec_mainten_has_txid_unreslvd(rldr) ||
		         dp_sync_has_unknown_dbits(rldr, nvmeibc_raid1_get_protect_lvl(nvmeibc_disk_segment_get_praid(rldr->ds))))) {
		__call_assist_sync(rldr, __analyze_binfo_sm_cb_b4j, NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO);
	}

	if (unlikely(dp_ec_mainten_next_txid_or_wrap(rldr))) { // Wraparound each few IO's
		__call_assist_sync(rldr, __analyze_binfo_sm_cb_b4j, NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP);
	}

	if (has_writable_pari) {
		err = dp_ec_journal_alloc_all_areas(rldr);
	}

	if (has_pre_reads || (has_writable_pari == false)) {
		// while journal is being allocating we can analyze & fix dbits, and also start executing the stages before write journal (prereads and calc parities)
		__analyze_binfo_sm_cb_b4w(rldr, ((err != -EINPROGRESS) ? err : 0));
	} else { /* WARNING: Here IO could already finish ('rldr' kfree) */}
	return 0;
}

/* cb and state machine which executes the analysis and fix of dirty bits,
   before Write Journal starts. Allways returns zero */
static int __analyze_binfo_sm_cb_b4w(void* context, int err)
{
	struct nvmeibc_block_command *rldr = context;
	struct operation *o = rldr->o;
BLKCMP_IO_ONLY_IF_PRESERVE_STACK(_func_start:)
	if (unlikely(o->topo->phased_out)) {
		err = -EAGAIN;
	}
	if (unlikely(err)) {
		nflog(t_dpec05, "Aborting maintanance steps on topo=@TOPO_DBG_ID err=@ERR", o->topo->debug_unique_index, err);
	} else {
		__calc_and_apply_io_dbits_action(rldr);
		if (unlikely(dp_ec_can_fix_dbits(rldr) && o->nd->dp.turn_off_dbits_before_io)) {
			// pre & post dirty (bits) logic: The segment on which we turn-off must be in write topo. If some seg dead, then we can NOT fix it. If it write, then we should fixed it and in such case __calc_and_apply_io_dbits_action was supposed to reflect this fact
			__call_assist_sync(rldr, __analyze_binfo_sm_cb_b4w, NVMEIB_BLOCK_IO_OP_RECOVER_DB);
		}
		dp_ec_set_tx_id_update_piggyback(rldr);	// No more problems with binfo
	}
	//__dump_operation_unsafe(o, NULL);
	__do_missed_stages_of_full_slice_write(rldr, &err);
	dp_cmds_execute_first_stage(rldr->cmdarr, rldr->my_leader, (err ? -ENXIO : 0));
	return 0;
}

int dp_ec_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int err) {
	struct nvmeibc_profiler *prof = nvmeibc_get_raid_good_path_profile_for_rwt_op(rldr->ds, rldr->o->op);
	struct dp_io_stats *dp_io_stats = &rldr->o->nd->dp.io_stats;

	int rv = err;									// On reads do nothing, just propagate locks error.
	if (unlikely(err)) {
		IO_STATS_INCR(dp_io_stats, DP_IO_STATS_LOCKSET_FAILED);
	}
	nvmeibc_profiling_end_take_stats_for_stage(  prof, rldr->o, E_CMDS_STAGE_WAIT_FOR_LOCK, 0);
	nvmeibc_profiling_start_take_stats_for_stage(prof, rldr->o, rldr->raid_cur_stage);
	if (nvmeib_block_io_op_is_write(rldr->o->op)) {  // Write is always launched via __analyze_binfo_sm_cb_b4w()
		nvmeibc_operation_compressed_op_dump_bio(rldr->o);
		__analyze_binfo_sm_cb_b4j(rldr, err);
		rv = 1;			// Write state mahcine allways executes as if async (even if locks were not taken)
	}
	return rv;
}

static void dp_ec_execute_rmw(const struct nvmeibc_block_command *rldr, const u64 op_map)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	const int map_size = mssa->map_size;
	u64 map = __extract_rmw_first_block_map(op_map);
	if (map) { // First bit is first block
		const int map_index = find_first_bit(mssa->bio_map, map_size);
		NVMESH_BUG(!test_bit(map_index, mssa->pre_read), __dump_operation_report, rldr ? rldr->o : NULL, "EC, first block must be pre-read");
		__copy_sub_block_by_map(sg_virt(mssa->gf_blocks.bio[map_index]),
								sg_virt(mssa->gf_blocks.prd[map_index]), map);
	}
	map = __extract_rmw_last_block_map(op_map);
	if (map) {	// Last bit (ignoring parities) is last block
		const int replicas = mssa->replicas, slice_size = mssa->slice_size, snake_size = mssa->snake_size;
		const int map_index = find_last_bit(mssa->bio_map, (map_size - ((replicas-slice_size)*snake_size)));
		NVMESH_BUG(!test_bit(map_index, mssa->pre_read), __dump_operation_report, rldr ? rldr->o : NULL, "EC, first block must be pre-read");
		__copy_sub_block_by_map(sg_virt(mssa->gf_blocks.bio[map_index]),
								sg_virt(mssa->gf_blocks.prd[map_index]), map);
	}
}

/* After we finish stage in execution place execute a callback on the
   commands of the raid */
void dp_ec_exec_func_on_stage_end(struct nvmeibc_block_command *rldr, int *rv) {
	//in EC we don't need to finalize the previous stage commands;
	const bool is_write_op = (nvmeib_block_io_op_is_write(rldr->o->op));

	if (is_write_op) { // Read op does nothing in between stages, POST IO RDMA and CALC deg data occur anyway and are setup in advance
		const bool has_writable_pari = !rldr->o->mssa->no_rw_p;
		const bool has_jour = !rldr->o->mssa->no_jour;
		const bool is_prev_success = !(*rv);
		const enum e_cmds_stage next_stage = ++rldr->raid_cur_stage;	// Finished current stage

		NVMESH_BUG((has_writable_pari == false) && has_jour, __dump_operation_report, rldr->o,
			   "EC, journal must be present if we have writable parity has_jour=%d", has_jour);

		switch (next_stage) { //prepare execution context for the next stage
		case E_CMDS_STAGE_CALC_PARITIES:
			if (is_prev_success) {	// in case of error it will be propogated
				if (has_jour) {	// Cannot be QLC op
					prepare_journal_wr_cmds_md(rldr);
				}
				if (unlikely(__is_bio_wrapper_for_bio(rldr->o->bios[0]->bio))) { // Pre read is done copy according to RMW maps
					const u64 preread_512_maps = get_valid_wrapper_block_maps(rldr->o->bios[0]);
					if (preread_512_maps) {
						// If we only use restore preread action occurs within reed solomon (if we have more than 1 strategy assume we need to take action now - worst case after restore we overwrite area again)
						if (unlikely((rldr->o->mssa->single_strat) && rldr->o->mssa->strategies[0] == RESTORE_STRATEGY)) {
						} else
							dp_ec_execute_rmw(rldr, preread_512_maps);
					}
				}
			}
			break;
		case E_CMDS_STAGE_WRITE_JOURNAL:
			if (is_prev_success) {
				if (has_writable_pari){ // we need journal for this transaction
					if (dp_ec_journal_alloc_is_success(rldr)) {	// Allocation success can only be analyzed here - if we failed to allocate jounrnals abort operation
						prepare_data_wr_cmds_md(rldr);
					} else { // Fail all next stages
						*rv = -EIO;
					}
				} else {
					prepare_data_wr_cmds_md(rldr);
				}
			}
			break;
		case E_CMDS_STAGE_POST_JR_RDMA: // This stage writes binfo to all locks
			break;
		case E_CMDS_STAGE_DO_IO_AND_PAR:
			if (is_prev_success){
				rldr->was_journ_success = true;	// in double dead parities and bio READ, we don't use journal - so we didn't fail to allocate it, in case we tried to allocate journal - then we succeeded
				if (has_jour) {	// Mark cookie reuse
					dp_rldr_set_wr_journal_cookies_to_data(rldr);
				}
			}
			break;
		case E_CMDS_STAGE_POST_IO_RDMA:
			break;
		case E_CMDS_STAGE_CALC_DEG_DATA: // Will never happen it's the last stage. Trap to find memory corruptions
			BUG();
			break;
		case E_CMDS_STAGE_DELETE_JOURNAL:
			if (has_jour) {
				dp_ec_journal_release_areas(rldr);	// regardless of rv
			}
			rldr->raid_cur_stage++;		// Jump to next stage
			break;
		default:
			/* Nothing for other stages */
			break;
		}
	} else { // Read op has 3 stages: PRE_READ (already done), POST_IO_RDMA (if required) and Calculate DEG if required, just set to next stage
		const enum e_cmds_stage current_stage = rldr->raid_cur_stage;	// Finished current stage
		TODO(000,"Fix the atrocity below. stage should advance in ++ like in write operations, never jump forward stage!");
		if (*rv) {
			rldr->raid_cur_stage = rldr->raid_last_stage;				// Fast fail. DoronL: A bit risky as most of commands will have rv == 0, even though they were not executed, Why do that?
			return;
		}
		switch (current_stage) {
		case E_CMDS_STAGE_READ_PRE_DATA:					// All reads of pre_read and E_CMDS_STAGE_DO_IO_AND_PAR are done at this stage
			rldr->raid_cur_stage = E_CMDS_STAGE_POST_IO_RDMA;
			break;
		case E_CMDS_STAGE_POST_IO_RDMA:
			rldr->raid_cur_stage = E_CMDS_STAGE_CALC_DEG_DATA;
			break;
		default:
			BUG();
		}
	}
}

static enum e_cmds_stage __rldr_get_first_stage(struct nvmeibc_block_command *rldr, const enum nvmeib_block_io_op op)
{
	if (op == NVMEIB_BLOCK_IO_OP_READ)
		return rldr->my_stage;
	return __has_pre_reads(rldr) ? E_CMDS_STAGE_READ_PRE_DATA : E_CMDS_STAGE_CALC_PARITIES; // For writes
}

void __prepare_mssa_target_map_for_data_restore(const struct nvmeibc_block_command *rldr);
void __prepare_mssa_target_map_for_data_restore(const struct nvmeibc_block_command *rldr)
{
	struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	const int slice_size = mssa->slice_size;
	u16 target_bmp = 0;
	int col;
	for (col = 0; col < slice_size; col++) {
		const int ci = mssa->column_to_cmd.read[col];
		if (ci >= 0) { // Found valid command
			if (rldr[ci].do_not_send) { // If do not send we need to restore it
				target_bmp |= (1 << col);
			}
		}
	}
	mssa->target_bmp = target_bmp;
}

// Counts how many writable parities in current write op
int __calc_ncmds_calc_parity(const struct nvmeibc_block_command *rldr, int *ncmds)
{
	struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	u16 target_bmp = 0;
	if (nvmeib_block_io_op_is_write(rldr->o->op)) {
		const int slice_size = mssa->slice_size, replicas = mssa->replicas;
		int col;
		u16 curr;
		for (col = slice_size, curr = (1 << slice_size); col < replicas; col++, curr <<= 1) {
			const int ci = mssa->column_to_cmd.write[col];
			if ((ci >= 0) && (!rldr[ci].do_not_send)) {
				NVMESH_BUG(rldr[ci].is_parity == false, __dump_operation_report, rldr[ci].o,
					   "Parity is not writable ci=%d", ci);
				(*ncmds)++;
				target_bmp |= curr;
			}
		}
		if (!(*ncmds) && (rldr->o->mssa->no_rw_p)) { // When double dead parities we only calculate EDIC for data cmds, Once EDIC calculation is complete a single completion will occur
			*ncmds = 1;
		}
	} else {
		*ncmds = 0;
	}
	mssa->target_bmp = target_bmp;
	return rldr->nraid_siblings - 1;		// Return last cmd
}

static void rldr_set_raid_last_stage(struct nvmeibc_block_command *rldr, const struct multi_snake_slice_analyzer *mssa, const enum nvmeib_block_io_op op) {
	if (!mssa->no_jour) {
		rldr->raid_last_stage = E_CMDS_STAGE_DELETE_JOURNAL;
	} else if (op == NVMEIB_BLOCK_IO_OP_READ && (mssa->gf_blocks.bio != NULL)) { // Ensure we calculate the missing data segments
		rldr->raid_last_stage = E_CMDS_STAGE_CALC_DEG_DATA;
	} else if ((nvmeib_block_io_op_is_write(op)) && ((mssa->no_rw_p) && (mssa->column_to_cmd.write[0] == -1))){	// Duplicated logic of rldr->use_io_apend_stages
		NVMESH_BUG(rldr[rldr->nraid_siblings - 1].my_stage != E_CMDS_STAGE_DO_IO_AND_PAR, __dump_operation_report,
			rldr[rldr->nraid_siblings - 1].o, "my_stage=%d", rldr[rldr->nraid_siblings - 1].my_stage); // No disk cmd to D0 to piggyback, we must send dedicated RDMA command to update lock owner blockset info
		rldr->raid_last_stage = E_CMDS_STAGE_POST_IO_RDMA;	// Always exists, at least dbits (turn on). Thats a unique case where this stage is last, coz there is no journal delete stage!
	} else {
		rldr->raid_last_stage = rldr[rldr->nraid_siblings - 1].my_stage;	// fix leader's indication of last stage
	}
}

// Do not count do_not_send commands as uncompleted commands
static void __add_ec_data_cmds_finish_raid(struct operation *o, int first_cmd, int n_cmds)
{
	struct nvmeibc_block_command *rldr = &o->cmds[first_cmd], *c, *end = &rldr[n_cmds];
	int n_cmds_first_stage = 0;
	const enum e_cmds_stage start_stage = __rldr_get_first_stage(rldr, o->op);
	rldr->raid_cur_stage = start_stage;
	rldr_set_raid_last_stage(rldr, o->mssa, o->op);
	for (c = rldr; c < end; c++) {
		c->raid_first_stage = start_stage;	// Copy it to all siblings to save dereference
		c->use_stages = true;
		c->my_leader = first_cmd;
		if (((enum e_cmds_stage)c->my_stage == start_stage) && !c->do_not_send)
			n_cmds_first_stage++;
	}
	if (!n_cmds_first_stage) { // Full slice/snake writes start by calculating parity
		NVMESH_BUG(start_stage != E_CMDS_STAGE_CALC_PARITIES, __dump_operation_report, o, "start_stage=%d", start_stage);
		__calc_ncmds_calc_parity(rldr, &n_cmds_first_stage);
	}
	nvmeibc_atomic_set(&rldr->n_uncompleted_cmds, n_cmds_first_stage);
	NVMESH_WARN(first_cmd, __dump_operation_report, o, "Invalid IO, EC uses a single raid leader. first_cmd=%d\n",
		    first_cmd);
}

#define __get_snake_start_slba(rlba, snake_size, slice_size) (((rlba)/(snake_size*slice_size))*snake_size)

static int __calc_n_cmds_in_column(const ulong col_bmp, int size)
{
	ulong b = col_bmp; //(col_bmp << 1);				// Padd lowest zero, assume autopadding of upper zero. Needed for searching cmd start
	BUILD_BUG_ON(N_MAX_MULTI_SLICE_LEN > 62);			// Assumption: at most 62 bits are used in col_bmp. Coz we need padding zeros around the bitmap
	b = (b & ~(b>>1));									// Marks 1's at every place where cmd ends (pattern 10)
	return bitmap_weight(&b, size);						// Number of cmds = number of cmd ends
}

// Add all EC reads to pre-read stage
// QLC Reads are done in DO_IO_AND_PAR for read (after MD is read is pre-read stage), syncs send reads (and MD) in pre-read stage
static void __fill_pre_read_cmd_from_mssa(struct operation *o, int ci, int column, int row, int nslices, u64 snake_start_offset, const struct nvmeibc_raid1 *pr) {
	struct nvmeibc_block_command *cmd = &o->cmds[ci];
	const int slice_size = o->mssa->slice_size;
	const int si = nvmeibc_raid1_role2seg(pr, o->mssa->owner_seg, column);
	struct nvmeibc_disk_segment *seg = &pr->segments[si];
	NVMESH_BUG(get_owner_seg_slice_start(pr, snake_start_offset * slice_size) != o->mssa->owner_seg,
		   __dump_operation_report, o, "ownwer seg don't match: %d, to %d",
		   get_owner_seg_slice_start(pr, snake_start_offset * slice_size), o->mssa->owner_seg);
	if (o->mssa->column_to_cmd.read[column] < 0)
		o->mssa->column_to_cmd.read[column] = ci; // Set once for split columns
	if (!nvmeibc_is_readable(seg)) { // We still have a command if part of BIO or required for write
		cmd->do_not_send = true;
	}

	cmd->nlbas = nslices;
	dp_cmds_req_fill(o, ci, seg);
	cmd->my_stage = E_CMDS_STAGE_READ_PRE_DATA;
	cmd->iocmd->reqs1.op = NVMEIB_BLOCK_IO_OP_READ; 			// Start_lba == row A on seg X can be < start_lba+1 row A-snake_size on seg X+1
	cmd->iocmd->reqs1.disk_address = seg->first_lba + snake_start_offset + row; // N slices of Snake + first row
	cmd->first_rlba = nvmeibc_datapath_dlba_to_rlba(&o->nd->dp, pr, si, cmd->iocmd->reqs1.disk_address);
	cmd->is_parity = (column >= pr->slice_size);
}

static void __fill_cmd_from_mssa(struct operation *o, int ci, int column, int row, int nslices, u64 snake_start_offset, const struct nvmeibc_raid1 *pr, const bool stage_is_journal, const enum nvmeib_block_io_op op) {
	struct multi_snake_slice_analyzer *mssa = o->mssa;
	struct nvmeibc_block_command *cmd = &o->cmds[ci];
	const int snake_size = mssa->snake_size, slice_size = mssa->slice_size, replicas = pr->replicas;
	const int si = nvmeibc_raid1_role2seg(pr, mssa->owner_seg, column);
	struct nvmeibc_disk_segment *seg = &pr->segments[si];
	u64 disk_address = seg->first_lba + snake_start_offset + row;
	NVMESH_BUG(get_owner_seg_slice_start(pr, snake_start_offset * slice_size) != mssa->owner_seg,
		   __dump_operation_report, o, "ownwer seg don't match: %d, to %d",
		   get_owner_seg_slice_start(pr, snake_start_offset * slice_size), mssa->owner_seg);
	if (!stage_is_journal) { // Only once is required - we can use either journal or data cmds
		NVMESH_BUG(mssa->column_to_cmd.write[column] >= 0, __dump_operation_report, o,
			   "mssa->column_to_cmd.write[column]=%d",
			   mssa->column_to_cmd.write[column]); // 2+ cmds to the same column are not allowed
		mssa->column_to_cmd.write[column] = ci; // No current need to init column_to_command.write for NVMEIB_BLOCK_IO_OP_MD_READ ops, but is simpler
		NVMESH_BUG(!nvmeib_block_io_op_is_write(op) && op != NVMEIB_BLOCK_IO_OP_MD_READ,
			   __dump_operation_report, o, "op=%d ", op);
	} else {
		NVMESH_BUG(mssa->column_to_cmd.jour[column] >= 0, __dump_operation_report, o, "column=%d test:%d",
			   column, mssa->column_to_cmd.jour[column]); // 2+ cmds to the same column are not allowed
		mssa->column_to_cmd.jour[column] = ci;
	}
	if (op == NVMEIB_BLOCK_IO_OP_MD_READ) {			// read cmds of sync - resolve binfo
		const sgmnts_bmp_t readable_bmp = nvmeibc_raid1_get_sgmnts_bmp(pr, readable);
		if (((1 << si)&readable_bmp) == 0)
			cmd->do_not_send = true;
	} else {									  	// BIO write
		if (seg->toma_acm == NVMEIBTC_DS_MODE_DEAD) { // We still have a command if part of BIO or required for write
			NVMESH_BUG(!test_bit(mssa_map_index(), mssa->bio_map), __dump_operation_report, o, "");
			cmd->do_not_send = true;
		}
	}

	cmd->nlbas = nslices;
	dp_cmds_req_fill(o, cmd - o->cmds, seg);
	cmd->my_stage = (stage_is_journal) ? E_CMDS_STAGE_WRITE_JOURNAL : E_CMDS_STAGE_DO_IO_AND_PAR;
	cmd->iocmd->reqs1.op = op;  			// Start_lba == row A on seg X can be < start_lba+1 row A-snake_size on seg X+1
	if (!stage_is_journal) {
		cmd->iocmd->reqs1.disk_address = disk_address;
	}
	cmd->first_rlba = nvmeibc_datapath_dlba_to_rlba(&o->nd->dp, pr, si, disk_address);
	cmd->is_parity = (column >= pr->slice_size);
}

// Add all read commands to the operation, get each set column, count number of commands
// if 2 commands split otherwise create the command.
static int add_ec_read_cmds_for_raid(const struct dp_io_topo_iterator_res* it, struct operation *o, int ci)
{
	struct multi_snake_slice_analyzer *mssa = o->mssa;
	const struct nvmeibc_raid1 *pr = it->r;
	const int snake_size = mssa->snake_size, slice_size = mssa->slice_size, replicas = pr->replicas;
	int col, row, map_i;
	const u64 snake_start_offset = __get_snake_start_slba(it->rlba, snake_size, slice_size);
	for_each_column_for_each_snake_while(mssa, mssa->pre_read, map_i, col, row, (ci < mssa->n_reads)) { // We need to correcly set the order of cmds - will break once all commands are set, some commands can appear only in second snake
		if (mssa->column_to_cmd.read[col] < 0) { // Set once for command - attempt to remove column_to_cmd mechanism
			int nslices;
			ulong col_mask = nvmeibc_mssa_get_column_map(mssa, col, false);
			nslices = hweight32(col_mask);
			if (nslices) { // check if read is split (add command per split, up to one more)
				int rows = 0;
				if (__calc_n_cmds_in_column(col_mask, mssa->map_size/replicas) > 1) { // We HAVE a split
					int row_i = find_next_zero_bit(&col_mask, mssa->map_size / replicas, row);
					rows = row_i - row;
					NVMESH_BUG((rows == nslices) || (rows == 0), __dump_operation_report, o,
						   "rows=%d nslices=%d", rows, nslices);
					__fill_pre_read_cmd_from_mssa(o, ci++, col, row, rows, snake_start_offset, pr);
					row_i = find_next_bit(&col_mask, mssa->map_size / replicas, row_i);
					NVMESH_BUG(row == row_i, __dump_operation_report, o, "row=%d row_i=%d", row, row_i); // Same row?
					row = row_i;
				}
				__fill_pre_read_cmd_from_mssa(o, ci++, col, row, nslices - rows, snake_start_offset, pr);
			}
		}
	for_each_column_for_each_snake_while_end }
	NVMESH_BUG(ci != mssa->n_reads, __dump_operation_report, o, "ci=%d mssa->nreads=%d", ci, mssa->n_reads);
	return ci;
}

// Sync commands fill the pre-read and then the write (no real need for bio_map/pre_read, only n_reads/writes
static int add_ec_sync_cmds_for_raid(const struct dp_io_topo_iterator_res* it, struct operation *o, int ci, const enum nvmeib_block_io_op op)
{
	const u64 snake_start_offset = __get_snake_start_slba(it->rlba, o->mssa->snake_size, o->mssa->slice_size);
	int col;
	if (!bitmap_empty(o->mssa->pre_read, o->mssa->map_size)) {
		for (col = 0; col < it->r->replicas; col++, ci++)
			__fill_pre_read_cmd_from_mssa(o, ci, col, 0, LOCKSET_SLICES, snake_start_offset, it->r);
	}
	if (!bitmap_empty(o->mssa->bio_map, o->mssa->map_size)) {
		for (col = 0; col < it->r->replicas; col++, ci++)
			__fill_cmd_from_mssa(   	  o, ci, col, 0, LOCKSET_SLICES, snake_start_offset, it->r, false, op);
	}
	return ci;
}

/* add write member(s) command:
 * go over the mssa bio_map column by column
 * Fill a command by amount of slices used, (it) is used for it's start raid and pr, refactor this later
 */
static int add_ec_write_cmds_for_raid(const struct dp_io_topo_iterator_res* it, struct operation *o, const bool stage_is_journal, int ci)
{
	int column, map_i, row;
	struct multi_snake_slice_analyzer *mssa = o->mssa;
	const int snake_size = mssa->snake_size, slice_size = mssa->slice_size;

	const u64 snake_start_offset = __get_snake_start_slba(it->rlba, snake_size, slice_size);
	const int limit = (stage_is_journal) ? (mssa->n_reads + (mssa->n_writes / 2)) : (mssa->n_reads + mssa->n_writes);
	for_each_column_for_each_snake_while(mssa, mssa->bio_map, map_i, column, row, (ci < limit)) {
		unsigned long column_map = nvmeibc_mssa_get_column_map(mssa, column, true);
		int nslices = hweight32(column_map);
		NVMESH_BUG(nslices == 0, __dump_operation_report, o, "EC, write must have at least one slice");
		if (nslices) { // Currently writes can not be split, once we can split partial destage writes (maybe never), allow multiple writes to same segment
			__fill_cmd_from_mssa(o, ci++, column, row, nslices, snake_start_offset, it->r, stage_is_journal, NVMEIB_BLOCK_IO_OP_WRITE);
		} else BUG(); // we go over all the columns that are set, must have one slice at least
		for_each_column_for_each_snake_while_end }
	return ci;
}

static inline void __alloc_md_for_cmd(struct nvmeibc_block_command *cmd)
{
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	cmd->iocmd->reqs1.md = ((md_size > 0) ? nvmeibc_alloc_md(cmd->nlbas, md_size) : NULL);
}

static int __ec_cmds_alloc_ndb_and_md(struct multi_snake_slice_analyzer *mssa, struct nvmeibc_block_command *rldr, struct scatterlist **rsgs) {
	struct nvmeibc_block_io_req *req;
	int ncmds = mssa->n_reads + mssa->n_writes, c, r = 0;
	for (c = 0; c < ncmds; c++) {
		req = &rldr[c].iocmd->reqs1;
		__alloc_md_for_cmd(&rldr[c]);   	  // EC-5473, here imporve, and search for other calls to this func
		if (req->md == NULL)
			goto _enomem;
		if (rldr[c].my_stage == E_CMDS_STAGE_WRITE_JOURNAL)
			continue;		// Allocate journals after the rest
		if (!nvmeib_get_ndb(&rldr[c], rldr[c].nlbas, GFP_NOFS))
			goto _enomem;
		req->ndb->length = 0;
		rsgs[r++] = req->ndb->table.sgl;
	}
	if (mssa->n_writes && !mssa->no_jour)	// Journal has identical blocks as data cmd, so reuse ndb
		dp_rldr_set_wr_journal_ndb_from_data(rldr, mssa->n_writes/2, mssa->n_reads);
	return 0;
_enomem:
	_NW(warn_dp_ec_ec_cmds_alloc_ndb_and_md, DMESG_PREFIX("@DEV_NAME") ": Out of memory nlbas=@NLBAS", rldr->o->nd->name, rldr[c].nlbas);	// Todo: Add cleanup?
	return -ENOMEM;
}

bool nvmeibc_copy_bio_buffers = true;
module_param(nvmeibc_copy_bio_buffers, bool, 0644);
MODULE_PARM_DESC(nvmeibc_copy_bio_buffers, "Copy bio buffers in writes. Set to True when using page cache");

static int __append_bio_block_to_cmd(int _i, struct scatterlist **sg, struct nvmeibc_block_command *cmds, struct operation *o, union vv_bio_inter *vbi, const int mi, const bool is_read)
{
	struct bio_vec bv = vv_bio_inter_get_next_bytes(vbi, o, NVMEIBC_SECTOR_SIZE);
	vv_bio_inter_set_sg_to_bio_page_maybe_copy(vbi, (sg)[_i], bv, false); // Use BIO, for "write" copy will happen in reed solomon, for "read" here
	if (is_read && o->mssa->gf_blocks.prd) {
		o->mssa->gf_blocks.prd[mi] = (sg)[_i];
	} else if (o->mssa->gf_blocks.bio) {
		o->mssa->gf_blocks.bio[mi] = (sg)[_i];
	}
	(sg)[_i] = sg_next((sg)[_i]);
	(cmds)[_i].iocmd->reqs1.ndb->length += NVMEIBC_SECTOR_SIZE;
	/*cmds[_i].iocmd->reqs1.ndb->table.nents++; // Already set */
	return 0;
}

// No need to check gf_blocks
static int __append_pages_block_to_cmd(int _i, struct scatterlist **sg, struct nvmeibc_block_command *rldr, struct operation *o, struct nps_block_iter *nbi, const int mi, const bool is_read)
{
	if (unlikely(nps_block_iter_nblocks(nbi) < 1)) {
		WARN(true, "nvmeibc bug! not enough blocks in allocated pages");
		return -ENOMEM; // Not really...
	}

	sg_set_page((sg)[_i], nps_block_iter_page(nbi), NVMEIBC_SECTOR_SIZE, nps_block_iter_offset(nbi));
	if (is_read && o->mssa->gf_blocks.prd != NULL)
		o->mssa->gf_blocks.prd[mi] = (sg)[_i];
	else
		o->mssa->gf_blocks.bio[mi] = (sg)[_i];
	(sg)[_i] = sg_next((sg)[_i]);
	(rldr)[_i].iocmd->reqs1.ndb->length += NVMEIBC_SECTOR_SIZE;
	/*rldr[_i].iocmd.reqs1.ndb->table.nents++; // Already set */

	nps_block_iter_advance(nbi, 1);
	return 0;
}

static int __append_parity_pages_block_to_cmds(int wi, struct scatterlist **sg, struct nvmeibc_block_command *rldr, struct operation *o, struct nps_block_iter *nbi, const int mi, const bool is_read, int ri)
{
	if (unlikely(nps_block_iter_nblocks(nbi) < 1)) {
		WARN(true, "nvmeibc bug! not enough blocks in allocated pages");
		return -ENOMEM; // Not really...
	}
	{
		struct page *p = nps_block_iter_page(nbi);
		const size_t offset = nps_block_iter_offset(nbi);
		sg_set_page((sg)[wi], p, NVMEIBC_SECTOR_SIZE, offset);
		o->mssa->gf_blocks.bio[mi] = (sg)[wi];
		(sg)[wi] = sg_next((sg)[wi]);
		(rldr)[wi].iocmd->reqs1.ndb->length += NVMEIBC_SECTOR_SIZE;
		if (is_read){
			sg_set_page((sg)[ri], p, NVMEIBC_SECTOR_SIZE, offset);
			if (o->mssa->gf_blocks.prd != NULL) { // Only for restore and update
				o->mssa->gf_blocks.prd[mi] = (sg)[ri];
			}
			(sg)[ri] = sg_next((sg)[ri]);
			(rldr)[ri].iocmd->reqs1.ndb->length += NVMEIBC_SECTOR_SIZE;
		}
	}
	nps_block_iter_advance(nbi, 1);
	return 0;
}

static void __set_data_metadata_in_mssa(struct multi_snake_slice_analyzer *mssa, int mi, struct nvmeibc_block_command *cmd, int *md_index)
{
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	mssa->blocks_md[mi] = cmd->iocmd->reqs1.md + md_size*((*md_index)++);
}

static int map_bio_cmds_io_buffers(struct operation *o, union vv_bio_inter *vbi, int first_raid_cmd) {
	int rv = -ENOMEM;
	struct nvmeibc_block_command *rldr = o->cmds + first_raid_cmd;
	struct multi_snake_slice_analyzer *mssa = o->mssa;
	const int ncmds = rldr->nraid_siblings, snake_size = mssa->snake_size, slice_size = mssa->slice_size, replicas = mssa->replicas;
	const bool is_read = (o->op == NVMEIB_BLOCK_IO_OP_READ);
	struct scatterlist *rsgs[4*N_MAX_RAID_SLICE_LEN] = {0};	// current sg to fill per each cmd in raid. In bio of almost 3 full slices in double degraded mode we have (D+P) prereads for first slice, for last slice, journals and data commands. Hence the x4 multiplier
	int i, ci = 0;
	int prev_column = -1, column = 0, prev_row = 0,row = 0, map_index = 0;
	struct nps_block_iter *nbi = &vbi->nbi;
	NVMESH_BUG((int)ARRAY_SIZE(rsgs) < ncmds, __dump_operation_report, o,
		   "Not enough rsgs, available: %lu, needed: %d", ARRAY_SIZE(rsgs), ncmds);
	if (__ec_cmds_alloc_ndb_and_md(mssa, rldr, rsgs) < 0)	// Keep for now, since cmds know how many blocks they require
		goto out;
	else {	// Handle all reads first
		int last_row[14] = {[0 ... 14-1] = -1};	// Note 14 >= N_MAX_RAID_SLICE_LEN. Enough bits to support up to 14+2
		for_each_set_bit(map_index, mssa->pre_read, mssa->map_size) { // In order of rlbas
			column = mssa_map_index_to_column(map_index ,snake_size, replicas);
			row =    mssa_map_index_to_row(   map_index ,snake_size, replicas);
			ci = mssa->column_to_cmd.read[column];
			if ((last_row[column] == -1) || ((last_row[column]+1) == row)) { // No hole use ci
				last_row[column] = row;
			} else { // Hole in read cmd use next value, we no longer update last row since we have at most one hole
				ci++;
			}
			if (is_read && test_bit(map_index, mssa->bio_map)) { // Add bio page -> will be stored in mssa->gf_blocks[map_index] - replication is done because if this
				    __append_bio_block_to_cmd(  ci, rsgs, rldr, o, vbi, map_index, true);
			} else if ((column >= slice_size) && (!is_read)) {
				if (!test_bit(map_index, mssa->bio_map)) { // Dummy dead parity read for update strategy, not written cannot share any buffer with write
					__append_pages_block_to_cmd(ci, rsgs, rldr, o, nbi,	map_index, true);
				}
				// Parity read buffers are added with the write buffers
			} else
				    __append_pages_block_to_cmd(ci, rsgs, rldr, o, nbi, map_index, true);
			prev_column = column;
			prev_row = row;
		}
	}
	if (!is_read) {
		const bool no_jour = mssa->no_jour;
		int md_index[N_MAX_RAID_SLICE_LEN] = {0}, di;
		prev_column = prev_row = -1;
		// If no pre-reads do not increment commands
		#define get_rsgs_index(column, no_jour, mssa) ((no_jour) ? ((mssa)->column_to_cmd.write[column]) : ((mssa)->column_to_cmd.jour[column]))
		for_each_set_bit(map_index, mssa->bio_map, mssa->map_size) { // Journal / Data cmds (setting on will affect the other as well.
			column = mssa_map_index_to_column(map_index ,snake_size, replicas);
			row = mssa_map_index_to_row(map_index ,snake_size, replicas);	// Required for QLC MD offset
			di = mssa->column_to_cmd.write[column];								// Write cmd metadata differs from journal MD
			ci = get_rsgs_index(column, no_jour, mssa);						// ci - can be either the journal CMD index or the data CMD index (we access the sgl by the relative index from last read - since journal CMDs are before write CMDs we use that index,	when there are no journals it is the same as data index)
			if (prev_column >= 0) { // When we switch coloumns or if we have a read hole
				if (column == prev_column)
					NVMESH_BUG(prev_row + 1 != row, __dump_operation_report, o,
						   "Hole in write cmd, prev_row: %d, row: %d", prev_row,
						   row); // TODO - for QLC allow write holes (use ci++, di++)
			} else { // No pre-reads
				// BUG_ON(no_jour &&(ci != 0)); No longer true, since we can have no_jour and pre-reads for read-modify-write of any seg
			}
			if (column < slice_size) { // Data block
				__append_bio_block_to_cmd(  ci, rsgs, rldr, o, vbi, map_index, false);
			} else { // Read and write parity buffers are shared
				ulong p_cmds_start = mssa->col_masks.prd[column] & ~(mssa->col_masks.prd[column] >> 1);
				int read_ci = mssa->column_to_cmd.read[column];
				const int n_slices = mssa->map_size / replicas;
				if (hweight32(p_cmds_start) > 1) { // We have split parities reads use correct command index
					int slice;
					// Consider using this for setting multiple-reads - and QLC fragmented writes
					for_each_set_bit(slice, &p_cmds_start, n_slices) {
						if (row <= slice) {
							break;
						}
						read_ci++;
					}
				}
				__append_parity_pages_block_to_cmds(ci, rsgs, rldr, o, nbi, map_index, test_bit(map_index, mssa->pre_read), read_ci);
			}
			__set_data_metadata_in_mssa(mssa, map_index, &rldr[di], &md_index[column]);
			prev_column = column;
			prev_row = row;
		}
	}

	for (i = 0; i < ncmds; i++)
		NVMESH_BUG(rldr[i].nlbas != (rldr[i].iocmd->reqs1.ndb->length >> NVMEIBC_SECTOR_SHIFT),
			   __dump_operation_report, rldr[i].o,
			   "rldr[%d].nlbas (%llu) != (rldr[%d].iocmd->reqs1.ndb->length (%u) >> NVMEIBC_SECTOR_SHIFT",
			   i, rldr[i].nlbas, i,
			   rldr[i].iocmd->reqs1.ndb->length); // Daniel: Todo remove, just for debugging

	if (o->flags.need_to_copy_bio) { // When copying BIO we must have spare pages
		NVMESH_BUG(o->op == NVMEIB_BLOCK_IO_OP_WRITE && nps_block_iter_empty(nbi), __dump_operation_report, o, "empty block iter");		// Spare pages allocated
		o->pages.nused_blks = nbi->n_blocks_used;
	} else {
		NVMESH_BUG(!nps_block_iter_empty(nbi), __dump_operation_report, o, "empty block iter");		// Iterator exhausted all pages
	}
	rv = 0;
out:
	return rv;
}

int dp_ec_cmds_add_for_raid(struct operation *o, const struct dp_io_topo_iterator_res* it, int first_raid_cmd)
{
	struct multi_snake_slice_analyzer *mssa = o->mssa;
	int ncmds = mssa->n_reads + mssa->n_writes;
	int c = first_raid_cmd;
	struct nvmeibc_block_command *rldr = &o->cmds[first_raid_cmd];
	NVMESH_BUG(first_raid_cmd || (!o->cmds), __dump_operation_report, o, "first_raid_cmd=%d, cmds=%p",
		   first_raid_cmd, o->cmds); // Single praid io in valid cmds
	rldr->nraid_siblings = ncmds;
	if (nvmeib_block_io_op_is_rw_op(o->op)){
		if (mssa->n_reads)
			c = add_ec_read_cmds_for_raid(it, o, first_raid_cmd);
		if (nvmeib_block_io_op_is_write(o->op)) { // Add journal and data cmds
			if (!mssa->no_jour)
				c = add_ec_write_cmds_for_raid(it, o, true,  c);
			c = 	add_ec_write_cmds_for_raid(it, o, false, c);
		}
	} else { // Sync is simpler
		enum nvmeib_block_io_op io_op = (is_op_sync_read_md_only(o->op) ? NVMEIB_BLOCK_IO_OP_MD_READ : NVMEIB_BLOCK_IO_OP_WRITE);  // sync that only reads only needs read commands.
		c = add_ec_sync_cmds_for_raid(it, o, c, io_op);
	}
	NVMESH_BUG(c != (mssa->n_reads + mssa->n_writes), __dump_operation_report, 0,
		   "c (%d) != (mssa->n_reads + mssa->n_writes) (%d)", c, mssa->n_reads + mssa->n_writes);

	__add_ec_data_cmds_finish_raid(o, first_raid_cmd, ncmds);
	/* Important! Unlike __mirror_cmds_add_for_raid(), dont treat dbits
	   explicitly. Because they are embedded in binfo and binfo is written
	   anyways */
	return ncmds;
}

static int dp_ec_operation_page_manager(struct operation *o)
{
	union vv_bio_inter vbi;
	vv_bio_inter_init_thick(&vbi, o);
	return map_bio_cmds_io_buffers(o, &vbi, 0);
}

/* Given array of commands (possibly partially filled), append the cmds which
   protect range [start_lba,start_lba+nlbas) */
static int __prepare_ec_cmds(struct operation *o, u64 nlbas, u64 start_lba, int c_i, struct nvmeibc_topology *t, const int ncmdsalloced)
{
	int rv = 0, ncmds = 0;
	struct dp_io_topo_iterator it;
	dp_io_topo_iterator_init(&it, start_lba, nlbas, t, c_i);
	BUILD_BUG_ON(!DEBUG_MD_EXTD && LOCKSET_SLICES > NVMEIBS_MAX_IO_CHANNEL_MSGS);   	// Erasue coding works in units of locks, unlike jbods which works in units of raids
	while (dp_io_topo_iterator_next(&it, 'l')) {
		rv = dp_ec_cmds_add_for_raid(o, &it.res, ncmds);
		if (rv < 0)
			goto _out;
		ncmds += rv;
		NVMESH_BUG(ncmds > ncmdsalloced, __dump_operation_report, o, "ncmds (%d) > ncmdsalloced (%d)", ncmds,
			   ncmdsalloced);
	}
	if (nvmeib_block_io_op_is_rw_op(o->op)) {
		rv = dp_ec_operation_page_manager(o);
		if (rv < 0)
			goto _out;
	}
	NVMESH_BUG(!o->cmds, __dump_operation_report, o, "null o->cmds");
	rv = ncmds;
	o->cmds->ncmds = ncmds;
	nvmeibc_atomic_set(&o->n_uncomp_raids, ncmds);
_out:	// Daniel, TODO: cleanup on negative rv
	return rv;
}

struct _prepare_alloc_counts {
	int nlocks;
	int ncmds;
	int n_extra_blocks;
	int mssa_bio_size;
	int mssa_prd_size;
	int mssa_md_block_size;
	bool has_dgrd_topology;
};

u32 nvmeibc_mssa_get_column_map_or_set(const struct multi_snake_slice_analyzer *_mssa, const int column, const bool is_bio, const bool set)
{
	if (set) {
		struct multi_snake_slice_analyzer *mssa = (void*)_mssa;
		u32 *rv = ((is_bio ? mssa->col_masks.bio : mssa->col_masks.prd) + column);
		const ulong *map2D = (is_bio ? mssa->bio_map : mssa->pre_read);
		const int snake_size = mssa->snake_size, replicas = mssa->replicas, n_rows = (mssa->map_size / replicas);
		int row;
		u32 column_mask = 0, bit;
		for (bit = 1, row = 0; row < n_rows; row++, bit <<= 1) {
			if (test_bit(mssa_map_index(), map2D))
				column_mask |= bit;
		}
		*rv = column_mask;
		return *rv;
	} else {
		if (is_bio)
			return _mssa->col_masks.bio[column];
		else
			return _mssa->col_masks.prd[column];
	}
}

roles_bmp_t nvmeibc_mssa_get_row_map_or_set(const struct multi_snake_slice_analyzer *_mssa, const int row, const bool is_bio, const bool set)
{
	const int replicas = _mssa->replicas;
	const ulong *map2D = (is_bio ? _mssa->bio_map : _mssa->pre_read);
	if (_mssa->map_size == replicas) {		// Single slice IO, 2D bitmap already is exactly the row
		return (roles_bmp_t)map2D[0];
	} else if (set) {								// Partial single snake IO (snake_size > 1)
		struct multi_snake_slice_analyzer *mssa = (void*)_mssa;
		u32 *rv = ((is_bio ? mssa->row_masks.bio : mssa->row_masks.prd) + row);
		const int snake_size = mssa->snake_size;
		int column;
		u32 row_mask = 0, bit;
		for (bit = 1, column = 0; column < replicas; column++, bit <<= 1) {
			if (test_bit(mssa_map_index(), map2D))
				row_mask |= bit;
		}
		*rv = row_mask;
		return row_mask;
	} else {
		if (is_bio)
			return _mssa->row_masks.bio[row];
		else
			return _mssa->row_masks.prd[row];
	}
}

static void nvmeibc_mssa_set_all_row_maps(struct multi_snake_slice_analyzer *mssa)
{
	int row;
	int n_slices = mssa->map_size / mssa->replicas;
	struct mssa_dump_vars vars = {.mssa = mssa, .op = 0, .pr = NULL};

	NVMESH_BUG(n_slices <= 1, __dump_mssa, &vars, "n_slices=%d", n_slices); // No need for single slice
	for (row = 0; row < n_slices; row++) { // Fully aligned writes - no pre-read
		NVMESH_BUG(mssa->row_masks.bio[row] != 0, __dump_mssa, &vars,
			   "row_masks.bio[%d] != 0", row);
		NVMESH_BUG(mssa->row_masks.prd[row] != 0, __dump_mssa, &vars,
			   "row_masks.prd[%d] != 0", row);
		mssa->row_masks.bio[row] = (1 << mssa->replicas) - 1;
	}
}

static void nvmeibc_mssa_row_map_add_bit(struct multi_snake_slice_analyzer *mssa, const int row, const u32 member, const bool is_bio)
{
	if (is_bio)
		mssa->row_masks.bio[row] |= member;
	else
		mssa->row_masks.prd[row] |= member;
}

static void __count_gf_blocks_and_md_for_bio(struct multi_snake_slice_analyzer *mssa, enum nvmeib_block_io_op op, struct _prepare_alloc_counts *pac)
{
	if (mssa->strategies[0] != INVALID_STRATEGY) { // Other wise it's a non-degraded read or no-parity write
		int i;
		bool update_strategy = false;
		//_NT(t_09_pmssa, "mssa, strategy = @INT, is_read=@BOOL_YN, bio_map =@LLX", (int)mssa->strategies[0], (op == NVMEIB_BLOCK_IO_OP_READ), (u64)(*mssa->bio_map));
		if (op != NVMEIB_BLOCK_IO_OP_READ) { // Check if we require twice the amount of buffer pointers
			for (i = 0; i < N_MAX_STRATEGIES; i++) {
				if (mssa->strategies[i] == INVALID_STRATEGY) { // No more strategies
					break;
				}
				if ((mssa->strategies[i] != COMPLEMENT_STRATEGY) && (mssa->strategies[i] != NO_ACTION_STRATEGY)) { // UPDATE/RESTORE
					update_strategy = true; // We do
					break;
				}
			}
		}
		{	// Allocate pointers to all data blocks (Read + Write) and all MDs (Write)
			pac->mssa_bio_size = sizeof(*mssa->gf_blocks.bio) * mssa->map_size;
			if (update_strategy) {
				pac->mssa_prd_size = sizeof(*mssa->gf_blocks.prd) * mssa->map_size;
			}
			if (op != NVMEIB_BLOCK_IO_OP_READ) {
				pac->mssa_md_block_size = sizeof(*mssa->blocks_md) * mssa->map_size;
			}
		}
	}
}

// Count commands by the set columns and store the value, reads (and writes for QLC) can be split
// We allocate gf_blocks and blocks_md if required by operation
// If update or restore strategy we also allocate pre-read mssa buffers
// Count number of split read and writes -  BIO commands,
static int __mssa_count_number_of_required_bio_cmds(struct multi_snake_slice_analyzer *mssa, enum nvmeib_block_io_op op) {
	const int replicas = mssa->replicas, max_slices = mssa->map_size / replicas;
	int column, n_writes = 0, n_reads = 0;
	struct mssa_dump_vars vars = {.mssa = mssa, .op = op, .pr = NULL};
	for (column = 0; column < replicas; column++) { // Mark the columns one by one
			  ulong column_mask = nvmeibc_mssa_get_column_map_or_set(mssa, column, false, true); // Pre-read
		const ulong    bio_mask = nvmeibc_mssa_get_column_map_or_set(mssa, column, true,  true); // BIO map
		if (nvmeib_block_io_op_is_write(op)) { // QLC can have split writes
			if (bio_mask)
				n_writes += __calc_n_cmds_in_column(bio_mask, max_slices);
		}
		n_reads += __calc_n_cmds_in_column(column_mask, max_slices);
	}
	NVMESH_BUG((n_reads + n_writes) == 0, __dump_mssa, &vars, "n_reads=%d n_writes=%d", n_reads, n_writes);
	mssa->n_reads = n_reads;
	mssa->n_writes = n_writes * (1 + (int)!mssa->no_jour);
	return mssa->n_reads + mssa->n_writes;
}

// Pre reads are added into the read_map, degraded reads as well (in addtion to bio reads). Parity writes are added to write_map in addtion to bio writes
// This is used as a test function and has been removed
static int __mssa_count_additional_blocks(struct multi_snake_slice_analyzer *mssa, const int nlbas, const enum nvmeib_block_io_op op) {
	const int pre_read_total_count = bitmap_weight(mssa->pre_read, mssa->map_size);		// All pre-reads require a new block (will not set pre-read if part of bio read)
	const int bio_total_count = 	 bitmap_weight(mssa->bio_map , mssa->map_size);
	int rv = pre_read_total_count - nlbas;
	struct mssa_dump_vars vars = {.mssa = mssa, .op = op, .pr = NULL};
	if (op != NVMEIB_BLOCK_IO_OP_READ) { // Bio reads merged into preR
		const int replicas = mssa->replicas, snake_size = mssa->snake_size, slice_size = mssa->slice_size;
		int row, column;
		rv += bio_total_count;
		// Remove all read parity blocks they use the write buffer
		for (row = 0; row < mssa->map_size / replicas; row++) {
			for (column = slice_size; column < replicas; column++) {
				if (test_bit(mssa_map_index(), mssa->pre_read) &&
					test_bit(mssa_map_index(), mssa->bio_map)) { // Remove parity read count - can be dummy dead parity for update strategy
					rv--;
				}
			}
		}
	}
	NVMESH_WARN(bio_total_count && bio_total_count < nlbas, __dump_mssa, &vars, "bio_total_count=%d nlbas=%d",
		    bio_total_count, nlbas); // Always write parities.

	return rv; // The total map without the bio size
}

static void __prepare_alloc_counts_mssa(struct _prepare_alloc_counts *pac, struct multi_snake_slice_analyzer *mssa, const u64 nlbas, const enum nvmeib_block_io_op op)
{
	struct mssa_dump_vars vars = {.mssa = mssa, .op = op, .pr = NULL};

	if (0) // Test function only - mssa_init calculates exact amount of extra blocks
		BUG_ON(pac->n_extra_blocks != __mssa_count_additional_blocks(mssa, nlbas, op));
	pac->ncmds = __mssa_count_number_of_required_bio_cmds(mssa, op);
	NVMESH_BUG(pac->ncmds == 0, __dump_mssa, &vars, "op=%d pac->ncmds=%d", op, pac->ncmds);
	__count_gf_blocks_and_md_for_bio(mssa, op, pac);
}

static void mssa_init(struct multi_snake_slice_analyzer *mssa, struct _prepare_alloc_counts *pac, int start_lba, const int nlbas, bool read_op, int binje, int snake_size, u64 preread_512_map, const struct dp_io_topo_iterator_res *it) {
	const struct nvmeibc_raid1 *pr = it->r;
	const int owner_seg = get_owner_seg_slice_start(pr, it->rlba);
	const int replicas = pr->replicas;
	const int slice_size = pr->slice_size;
	const unsigned long no_read_map = nvmeibc_raid1_get_inverse_roles_bmp(pr, owner_seg, readable);
	const unsigned long no_write_map = nvmeibc_raid1_get_roles_bmp(pr, owner_seg, dead);
	const bool has_rw_parity = nvmeibc_raid1_get_roles_bmp(pr, owner_seg, has_writable_pari);
	const int n_slices = (read_op) ? LOCKSET_SLICES : binje;
	const int one_snake = replicas * snake_size;
	const int data_snake = slice_size * snake_size;
	const int snake_start = start_lba % data_snake;
	const int first_column = snake_start / snake_size;
	const int first_row = snake_start % snake_size;
	unsigned long *map = mssa->bio_map;
	const roles_bmp_t data_roles_mask = GENMASK(slice_size-1,0);
	int row = first_row, column = first_column, last_snake_index = 0, n_lbas = nlbas, strategy_index = 0, txbm_index = 0, last_row = -1;
	struct mssa_dump_vars vars = {.mssa = mssa, .op = 0, .pr = NULL};
	mssa->map_size = replicas * n_slices;
	mssa->replicas = replicas;
	mssa->snake_size = snake_size;
	mssa->slice_size = slice_size;
	mssa->owner_seg = owner_seg;
	mssa->no_rw_p = !has_rw_parity;
	mssa->no_jour = mssa->no_rw_p || read_op;
	NVMESH_WARN(nlbas > mssa->map_size, NO_REPORT, NULL, "replicas=%d, n_slices=%d, nlbas=%d, map_size=%d", replicas,
		    n_slices, nlbas, mssa->map_size);
	do {	// Set the BIO blocks required in the 2D matrix
		const int start = column * snake_size + row;
		const int max_nlbas = min(n_lbas, data_snake - start);
		NVMESH_BUG(start >= data_snake, NO_REPORT, NULL, "start=%d, data_snake=%d, snake_size=%d, row=%d, column=%d",
			   start, data_snake, snake_size, row, column);
		NVMESH_BUG(last_snake_index * snake_size > n_slices, NO_REPORT, NULL,
			   "last_snake_index=%d, snake_size=%d, n_slices=%d", last_snake_index, snake_size, n_slices);
		bitmap_set(map, one_snake * last_snake_index + start, max_nlbas);
		n_lbas -= max_nlbas;
		last_snake_index++;
		row = 0;
		column = 0;
	} while (n_lbas);
	mssa->map_size = one_snake * last_snake_index;
	if (unlikely(preread_512_map)) { // Read Modify Write flow needs the first and last block, we go by row so we need to know which row is the last (can be == first_row on another column)
		last_row =  mssa_map_index_to_row(find_last_bit(map, mssa->map_size), snake_size, replicas);
	}
	n_lbas = nlbas;
	if ((!read_op) && (snake_start == 0) && ((nlbas % data_snake) == 0) && (!preread_512_map)) { // Fully aligned IO up to N/S snakes - optimized
		int snake, n_parities = replicas - slice_size;  // Default value
		if (unlikely(no_write_map)) { // Mark all non dead parities for write
			u32 parity_map = GENMASK(replicas-1, slice_size);
			n_parities = n_parities - hweight32(parity_map & no_write_map);
		}
		column = slice_size - 1;
		mssa->strategies[strategy_index] = COMPLEMENT_STRATEGY; // Full slice
		mssa->tx_bms[0] = ((1 << slice_size) -1); // All are written
		while (n_parities--) {
			column++;
			while ((1<<column) & no_write_map)
				column++; // Skip dead Ps
			for (snake = 0; snake < last_snake_index; snake ++) {
				bitmap_set(map, snake * one_snake + column * snake_size, snake_size);
				pac->n_extra_blocks += snake_size;
			}
		}
		if ((mssa->map_size / replicas) > 1) { // Single slice IO access the row from the map itself
			nvmeibc_mssa_set_all_row_maps(mssa);
		}
		BUG_ON(column >= replicas);
	} else { // Partial single snake operation
		if (!read_op || no_read_map) { // Write op or Read in degraded mode. Fill in pre_read per slice
			for (row = 0; row < mssa->map_size / replicas; row++) {
				const roles_bmp_t slice_map = nvmeibc_mssa_get_row_map_or_set(mssa, row, true, true);
				if (slice_map == 0) {
					if ((txbm_index) && (mssa->tx_bms[txbm_index-1] != slice_map)) {
						mssa->tx_bms[txbm_index++] = slice_map;
					}

					NVMESH_BUG((strategy_index >= 3), __dump_mssa, &vars, "strategy_index=%d", strategy_index);  // Any other IO - TODO remove after developement completes
					if (strategy_index) { // Found a hole! Otherwise we haven't reached the first slice IO
						if (mssa->strategies[strategy_index-1] != NO_ACTION_STRATEGY) {
							mssa->slice_change_strategy[strategy_index - 1] = row;
							mssa->strategies[strategy_index++] = NO_ACTION_STRATEGY;
						}
					}
					continue;
				} else {
					int n_blocks_in_row = hweight16(slice_map);
					if (!read_op) { // Set write parities and preread based on strategy (including degraded)
						bool must_complement = false, must_update = false, read_all = false; // Slice specific variables
						const roles_bmp_t slice_txbm = slice_map & data_roles_mask;
						NVMESH_BUG(slice_map != slice_txbm, __dump_mssa, &vars, "slice_map=%hu slice_txbm=%hu", slice_map, slice_txbm); // Since we have yet to add parities
						if ((txbm_index == 0) || (mssa->tx_bms[txbm_index-1] != slice_txbm)) {
							mssa->tx_bms[txbm_index++] = slice_txbm;
						}
						if (has_rw_parity) {
							for (column = slice_size; column < replicas; column++) { // Set write parities column < replicas
								if (((1<<column) & no_write_map) == 0) { // Skip dead Ps
									set_bit(mssa_map_index(), map);
									nvmeibc_mssa_row_map_add_bit(mssa, row, (1 << column), true);
									pac->n_extra_blocks++;
								}
							}
							if (unlikely(preread_512_map)) {	// Check if read-modify-write flow forces a strategy on this slice
								NVMESH_BUG(last_row < 0, __dump_mssa, &vars, "last_row=%d", last_row);	// TODO: Remove only for sanity
								if ((row == first_row) && __extract_rmw_first_block_map(preread_512_map)) {
									must_update = true;
									// Only check first row once -> remove the map we used next rows will not stop here again
									preread_512_map &= ((1UL << KERNEL_SECTORS_IN_NVMEIBC_SECTOR) - 1) << KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
								}
								if ((row == last_row) && __extract_rmw_last_block_map(preread_512_map)) { // Check if we are in the last slice, if so mark must_update and unmark the map
									must_update = true;
									// Only check last row once
									preread_512_map &= (1UL << KERNEL_SECTORS_IN_NVMEIBC_SECTOR) - 1;
								}
							}
						}
						if (unlikely(no_read_map)) { // Check if degraded forces a strategy on this slice
							for_each_set_bit(column, &no_read_map, replicas) { // Degraded parities (not written to) should not force a restore strategy, but if written to could cause restore
								if (column < slice_size)
									// Can be if (1 << column) & slice_map
									if (test_bit(mssa_map_index(), map)) { // We write to a degraded segment we cannot update the parities and must read complementary data blocks
										must_complement = true;
									} else  							// We cannot read the full slice (a missing data segment cannot be read) - so we must update the parities
										must_update = true;
								else { // Not written to parity (DEAD) no need to force update
									if (!((1 << column) & no_write_map)) { // Writable not readable parity - must complemet
										must_complement = true;
									}
								}
							}
						}
						if (unlikely(must_complement && must_update)) {
							// Double degraded write, One block written is dead and one block not-written is dead as well.
							// We read all available blocks + P/Q.
							// We restore the not-written block from above sources (read all avilable D and P+Q).
							// We calculate new P/Q from the "final" slice blocks previous slice including restored data, with BIO on top.
							// We issue writes only BIO and P/Q that are not dead
							// Simple example 4+2 (D2 and D1 are dead) this slice writes to D0+D1 (and P/Q)
							// We read D0 + D3 + P + Q -> Calculate D2 (we don't need old D1) -> Calculate new P/Q from D0 (new) + D1 (new) + D2 (Calc) + D3 (pre-read)
							// Write D0 + P + Q
							// This case is why we must still allocate pre-reads for a dead segment (as a temp block)

							// New 512B options
							// update strategy is true since we read a written to block (first or last)
							// but the block itself is degraded and we must restore (any single other D or P can be degraded)
							// or any P is writable but not readable (so we cannot update the P)
							read_all = true;
						}
						if (has_rw_parity) {   // Finally set pre-reads according to slice stratgy (Must skip this if double degraded parities since we don't read at all, nor calculate Parities)
							const int protect_level = replicas - slice_size;
							const int n_unreadable_parities = hweight16(no_read_map & nvmeibc_raid1_get_parities_bmp(pr));
							const int n_bio_and_parity_cmds = protect_level + n_blocks_in_row - n_unreadable_parities; // Number of reads for update -> if P is unreadable reduce this count
							const int n_complement_cmds = slice_size - n_blocks_in_row; 	   // Number of reads for full slice calculation
							const bool complement = ((n_complement_cmds <= n_bio_and_parity_cmds) && !must_update) || must_complement; // We prefer to override P/Q rather than update
							const enum slice_strategy strat = (read_all) ? RESTORE_STRATEGY : (complement) ? COMPLEMENT_STRATEGY : UPDATE_STRATEGY;
							for (column = 0; column < replicas; column++) {
								if (likely(!read_all)) { // Skip dead segments
									if ((1 << column) & no_write_map) {
										if (column >= slice_size && !complement) { // Dead parity, if we chose to update, we need the parity place holder
											set_bit(mssa_map_index(), mssa->pre_read);
											BUG_ON(test_bit(mssa_map_index(), map));
											nvmeibc_mssa_row_map_add_bit(mssa, row, (1 << column), false);
											pac->n_extra_blocks++; // A single block for this parity
										}
										continue;
									}
								}
								{ // If compliment we read the segments we do not write - otherwise we only read the segments we do write (including P/Q)
									const int has_write = test_bit(mssa_map_index(), map);	// Note: The result is not boolean! False == 0 but True is {1, -1, 2^bit index, or other value}
									const bool read_action = (has_write != 0) ^ complement;		// Boolean 'xor' on first bit
									if (unlikely(read_all) || read_action) {	// Add this block to the pre_read map and update the mssa row bits (data blocks require an addtional buffer for reads)
										if (unlikely((column >= slice_size) && ((1 << column) & no_write_map) && read_all)) {	// Dead P + read all means read-modify-write requires pre-read for block
											// Do not mark irrelevant P for read or write just skip it
											continue;
										}
										set_bit(mssa_map_index(), mssa->pre_read);
										nvmeibc_mssa_row_map_add_bit(mssa, row, (1 << column), false);
										if (column < slice_size) { // Parities share read and write blocks
											pac->n_extra_blocks++;
										}
									} else {
										// We don't need to clear the pre-read it wasn't set
									}
								}
							}
							// Mark unique strategies (up to 3 if Snake size > 2, changes with tx_bm)
							if (strategy_index && (mssa->strategies[strategy_index-1] == strat)) { // Same strategy don't update
							} else {
								if (strategy_index)
									mssa->slice_change_strategy[strategy_index-1] = row;
								mssa->strategies[strategy_index++] = strat; // New strategy compared to previous slice
							}
						} else {	// No read/write parities
							enum slice_strategy default_strategy = COMPLEMENT_STRATEGY;	// Unless 512B forces a read - doesn't affect anything other than allocating pre_read gf_blocks
							if (unlikely(preread_512_map)) {	// We force pre-read on the first and last block by using the first and last indexes
								if (__extract_rmw_first_block_map(preread_512_map)) {
									const long unsigned int first_block_index = map_rlba_offset(first_column, snake_size, first_row, replicas);
									BUG_ON(first_block_index != find_first_bit(map, mssa->map_size));
									set_bit(first_block_index, mssa->pre_read);
									pac->n_extra_blocks++;
									// Unmark the first block map, no need to do it for each slice
									preread_512_map &= ((1UL << KERNEL_SECTORS_IN_NVMEIBC_SECTOR) - 1) << KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
									default_strategy = UPDATE_STRATEGY; // Mark update because we read the block first (we only calculate EDIC so it is fine)
								}
								if (__extract_rmw_last_block_map(preread_512_map)) {
									const int map_size_no_parities = mssa->map_size - (replicas-slice_size)*snake_size;
									const int last_block_index = find_last_bit(map, map_size_no_parities);
									set_bit(last_block_index, mssa->pre_read);
									pac->n_extra_blocks++;
									// Unmark the last block map, no need to do it for each slice
									preread_512_map &= (1UL << KERNEL_SECTORS_IN_NVMEIBC_SECTOR) - 1;
									default_strategy = UPDATE_STRATEGY; // Mark update because we read the block first (we only calculate EDIC so it is fine)
								}
							}
							if (strategy_index && (mssa->strategies[strategy_index-1] == default_strategy)) { // Same strategy don't update
							} else {	// When parities are not writable we usually have "complement" strategy but we only calcualte EDIC without calculating Parities (if not complement, we need to read for read-modify-write subblock flow, see details above)
								// The strategies doesn't matter other than debug, since we do both the first and last block into the maps, and reed solomon only calcualtes edic since we don't have parities.
								// The read blocks are modified into the write blocks on stage end
								mssa->strategies[strategy_index++] = default_strategy;
							}
						}
					} else { // Degraded read - if row requires restore pre_read row map will have bits set (can only be parity if full slice read)
						int n_parities = 0, n_unavailable_segs = 0;
						for_each_set_bit(column, &no_read_map, slice_size) {
							// Can be if (1 << column) & slice_map
							if (test_bit(mssa_map_index(), map)) { // We read from a degraded segment add another parity read to this row
								// Mark first and last slice parity reads
								n_parities++;
							} else n_unavailable_segs++; // Another missing data segment that we will need to read but not for bio (only relevant if we also read from a unreable data seg)
						}
						if (n_parities) { // Must read entire slice + N parities
							n_parities += n_unavailable_segs;
							for (column = 0; column < slice_size; column++) { // Mark all non-degraded segs to be read from (if part of BIO already marked)
								if ((1 << column) & no_read_map)	continue;
								if (test_bit(mssa_map_index(), map)) continue; // Already marked for bio
								set_bit(mssa_map_index(), mssa->pre_read);
								nvmeibc_mssa_row_map_add_bit(mssa, row, (1 << column), false); // All read masks are bio masks (they include pre-reads)
								pac->n_extra_blocks++;
							}
							// Used to check how many parities are required to restore the bio part of the read
							if (strategy_index && mssa->n_parities[strategy_index-1] == n_parities) { // Same strategy don't update (how many required Parities we have)
							} else {
								if (strategy_index)
									mssa->slice_change_strategy[strategy_index-1] = row;
								mssa->n_parities[strategy_index++] = n_parities; // New strategy compared to previous slice,
							}
							do {	// Read the first N lowest accessible parities. Example: If we have 2 parities but single degraded D5, read only P, not Q
								while ((1 << column) & no_read_map) column++; // Skip read from degraded Parities (P > 2)
								set_bit(mssa_map_index(), mssa->pre_read);
								pac->n_extra_blocks++;
								nvmeibc_mssa_row_map_add_bit(mssa, row, (1 << column), false); // All read masks are bio masks (they include pre-reads)
								column++;
							} while (--n_parities);
						}
					} // Degraded read
				} // Slice is not empty
				if (nlbas < snake_size) { // We can finish early
					// At least part of this slice is written to (From the bottom we might skip a few rows) or in between
					if (--n_lbas == 0) // Final slice
						break; // Matters when nlbas << Snake size
					NVMESH_WARN(n_lbas < 0, NO_REPORT, NULL, "nlbas=%d", n_lbas); // Should not happen
				}
			} // For row ...
			if (!read_op && (nlbas < snake_size) && (nlbas > 1) && (strategy_index == 3)) { // Check for write parity holes add both read and write
				int first_slice = mssa->slice_change_strategy[0];
				int last_slice = mssa->slice_change_strategy[1];
				NVMESH_BUG(mssa->strategies[0] == INVALID_STRATEGY || mssa->strategies[2] == INVALID_STRATEGY || mssa->strategies[1] != NO_ACTION_STRATEGY,
					   __dump_mssa, &vars,
					   "mssa->strategies[0]=%d, mssa->strategies[1]=%d, mssa->strategies[2]=%d",
					   mssa->strategies[0], mssa->strategies[1], mssa->strategies[2]);
				NVMESH_BUG((first_slice == 0) || (last_slice == 0), __dump_mssa, &vars,
					   "first_slice=%d, last_slice=%d", first_slice, last_slice);
				NVMESH_BUG(first_slice == last_slice, __dump_mssa, &vars,
					   "first_slice=%d, last_slice=%d", first_slice, last_slice);
				NVMESH_BUG(mssa->tx_bms[1] != 0, __dump_mssa, &vars, "mssa->tx_bms[1]=%hu",
					   mssa->tx_bms[1]);
				for (column = slice_size; column < replicas; column++) {
					if (no_write_map & (1 << column)) continue;
					for (row = first_slice; row < last_slice; row++ ) {
						const int map_index = mssa_map_index();
						if (!test_bit(map_index, map)) { // Added write - one by one for accounting of required blocks
							set_bit(map_index, map);
							pac->n_extra_blocks++;
							NVMESH_BUG(test_bit(map_index, mssa->pre_read), __dump_mssa, &vars, "map_index=%d", map_index);
							set_bit(map_index, mssa->pre_read);
						} else BUG();
					}
				}
			}
		} else { // Regular (non degraded) read. No pre-reads, nothing to do
		}
	} // Full/Partial Snake handling complete
	if (read_op) { // Set bio into pre_read for unified map, and bio map to desern
		bitmap_or(mssa->pre_read, mssa->pre_read, map, mssa->map_size);
	}
	memset(mssa->column_to_cmd.read, -1, replicas);
	if (!read_op) {
		memset(mssa->column_to_cmd.write, -1, replicas);
		if (!mssa->no_jour) {
			memset(mssa->column_to_cmd.jour, -1, replicas);
		}
	}
	memset(&mssa->col_masks, -1, sizeof(mssa->col_masks));  // Initialize to u32(-1)
	// 0 if non-degraded read / double degraded parity write
	if (strategy_index <= 1) {
		NVMESH_BUG(mssa->slice_change_strategy[0] || mssa->slice_change_strategy[1], __dump_mssa, &vars, "slice strategy not consistent");
		mssa->single_strat = true;
	}
	NVMESH_BUG(strategy_index > 3, NO_REPORT, NULL, "strategy_index=%d", strategy_index);
	if (0) { // Prints all three "maps" Degraded / (Pre+)Read / Write
		//nvmeibc_multi_snake_slice_analyzer_debug_print(mssa, replicas, slice_size, deg_map, first_snake_index, last_snake_index, snake_size);
	}
}

// Calculate the set of all the segments we write to -> writes are consecutive so up to 5 different txbms
// QLC partial destage has no logic to it, except each snake is unique for the entire snake
sgmnts_bmp_t nvmeibc_mssa_calc_write_bmp(const struct multi_snake_slice_analyzer *mssa)
{
	roles_bmp_t write_bmp = GENMASK(mssa->replicas - 1, mssa->slice_size); // Access bmp is first computed as roles bmp, but then converted to segments.
	write_bmp |= mssa->tx_bms[0] | mssa->tx_bms[1] | mssa->tx_bms[2] | mssa->tx_bms[3] | mssa->tx_bms[4];
	// Now convert roles into segments
	return rol32_width(write_bmp, mssa->owner_seg, mssa->replicas);
}

// Calculate the set of all the segments that fully overwrites the blockset
sgmnts_bmp_t nvmeibc_mssa_calc_full_blockset_write_bmp(const struct multi_snake_slice_analyzer *mssa)
{
	roles_bmp_t full_blockset_write_bmp = 0;					  // Reperesents a set of all segments in the given blockset that we fully overwrite, Parities are always full
	if (mssa->map_size < mssa->replicas * LOCKSET_SLICES) // If the write spans on less then LOCKSET_SLICES slices - no chance there is a single full segment
		return full_blockset_write_bmp;
	else { // AND first and last slice then add parities
		full_blockset_write_bmp  = nvmeibc_mssa_get_row_tx_bm(mssa,                  0);
		full_blockset_write_bmp &= nvmeibc_mssa_get_row_tx_bm(mssa, LOCKSET_SLICES - 1);
		full_blockset_write_bmp |= GENMASK(mssa->replicas - 1, mssa->slice_size);
		return rol32_width(full_blockset_write_bmp, mssa->owner_seg, mssa->replicas);
	}
}

// Pre allocated with the operation at end of o
void dp_ec_set_allocated_mssa_into_operation(struct operation *o)
{
	o->mssa = (struct multi_snake_slice_analyzer *)&o[1];    // Pre allocated with the operation
}

static void __prepare_alloc_counts_calc(struct _prepare_alloc_counts *pac,
		struct operation *o, u64 nlbas, u64 start_lba, int c_i, struct nvmeibc_topology *t)
{
	struct dp_io_topo_iterator it;
	u64 alignment_sectors;
	NVMESH_BUG(o->mssa, __dump_operation_report, o, "o->mssa=%p", o->mssa);
	dp_io_topo_iterator_init(&it, start_lba, nlbas, t, c_i);
	while (dp_io_topo_iterator_next(&it, 'l')) {
		const struct nvmeibc_raid1* praid = it.res.r;
		struct lock_ownership_map rlmap;
		u64 preread_512_map = 0;
		if (unlikely(__is_bio_wrapper_for_bio(o->bios[0]->bio) && nvmeib_block_io_op_is_write(o->op))) {
			preread_512_map = get_valid_wrapper_block_maps(o->bios[0]);
		}
		lock_ownership_build_raid_map(it.res.r, it.res.rlba, o->op, &rlmap);
		pac->nlocks += rlmap.n_locks;

		alignment_sectors = (u64)(o->nd->dp.p.snake_size * it.res.r->slice_size); // 1 full snake
		dp_ec_set_allocated_mssa_into_operation(o);
		NVMESH_BUG(o->mssa == NULL, __dump_operation_report, o, "o->mssa=%p", o->mssa);
		mssa_init(o->mssa, pac, it.res.rlba % alignment_sectors, nlbas, (o->op == NVMEIB_BLOCK_IO_OP_READ),
						 o->nd->dp.p.binje, o->nd->dp.p.snake_size, preread_512_map, &it.res);

		__prepare_alloc_counts_mssa(pac, o->mssa, nlbas, o->op);

		pac->has_dgrd_topology |= (!nvmeibc_praid_are_all_readable(praid));
	}
}

// For adding into buf
static int nvmeibc_multi_snake_slice_analyzer_tostring(const struct multi_snake_slice_analyzer *mssa, char *buf, size_t len, enum nvmeib_block_io_op op, const struct nvmeibc_raid1* pr) {
	#define BUF_ADD(...) pos += scnprintf(buf+pos, len-pos, __VA_ARGS__)
	const int slice_size = mssa->slice_size, snake_size = mssa->snake_size, replicas = mssa->replicas;
	const int one_snake = snake_size * replicas;
	int pos = 0, column, row, snake, srow;
	const bool is_read = (op == NVMEIB_BLOCK_IO_OP_READ);
	BUF_ADD("%s, array=%dx%d, snake=%d, D0 on S%d\n", nvmeib_block_io_op_str(op), mssa->replicas, mssa->map_size/mssa->replicas, snake_size, mssa->owner_seg);
	{	// Add TxBMs
		roles_bmp_t pari_bmp = GENMASK(mssa->replicas - 1, mssa->slice_size), i;
		BUF_ADD("TxBM= {");
		for (i = 0; mssa->tx_bms[i] != 0; i++) {
			BUF_ADD("0x%x,", mssa->tx_bms[i]); // | pari_bmp);
		}
		pos--; BUF_ADD("} | 0x%x\n", pari_bmp);
	}
	{	// Add axis names of roles
		char Par = 'P';
		for (column = 0; column < slice_size; column++)	BUF_ADD("%02d|", column);
		for (          ; column < replicas  ; column++)	BUF_ADD("%c |", Par++);
		BUF_ADD("\n");
	}
	if (pr) {
		roles_bmp_t non_read = nvmeibc_raid1_get_inverse_roles_bmp(pr, mssa->owner_seg, readable);
		roles_bmp_t non_writ = nvmeibc_raid1_get_roles_bmp(        pr, mssa->owner_seg, dead    );
		for (column = 0; column < replicas  ; column++, non_read >>= 1, non_writ >>= 1)
			BUF_ADD("%c |", ((non_read & 1) ? ((non_writ & 1) ? 'D' : 'W'): ' '));
		BUF_ADD("\t\tDegraded.\n");//.  row_mask=0x%04x\n", (u32)deg_map);
	}
	if (is_read) {
		for (snake = 0; snake*one_snake < mssa->map_size; snake++) {
			for (srow=0;srow<snake_size;srow++) {
				row = srow + snake*snake_size;
				for (column = 0; column < replicas; column++) {
					BUF_ADD("%s|", ((test_bit(mssa_map_index(), mssa->bio_map)) ? "BI" : (test_bit(mssa_map_index(), mssa->pre_read)) ? "PR" : "  " ));
				}
				BUF_ADD("\t\tRead\n");
			}
		}
		return pos;
	}
	// For write
	if (!bitmap_empty(mssa->pre_read, mssa->map_size)) {
		for (snake = 0; snake*one_snake < mssa->map_size; snake++) {
			for (srow=0;srow<snake_size;srow++) {
				row = srow + snake*snake_size;
				for (column = 0; column < replicas; column++) {
					BUF_ADD("%s|", ((test_bit(mssa_map_index(), mssa->pre_read)) ? "PR" : "  " ));
				}
				BUF_ADD("\t\tPre-Read.  row_mask=0x%04x\n", mssa->row_masks.prd[row]);
			}
		}
	}
	for (snake = 0; snake*one_snake < mssa->map_size; snake++) {
		for (srow=0;srow<snake_size;srow++) {
			row = srow + snake*snake_size;
			for (column = 0; column < replicas; column++) {
				BUF_ADD("%s|", test_bit(mssa_map_index(), mssa->bio_map) ? "BI" : "  ");
			}
			BUF_ADD("\t\tWrite-Map. row_mask=0x%04x\n", nvmeibc_mssa_get_row_map((mssa),row,true));
		}
	}

	BUF_ADD("col_masks_prd {");
	for (column = 0; column < replicas; column++) {
		BUF_ADD("0x%04x ", mssa->col_masks.prd[column]);
	}
	BUF_ADD("}\ncol_masks_bio {");
	for (column = 0; column < replicas; column++) {
		BUF_ADD("0x%04x ", mssa->col_masks.bio[column]);
	}
	BUF_ADD("}");
	#undef BUF_ADD
	return pos;
}

#define __get_mssa_print_size(...)	(PAGE_SIZE)
void nvmeibc_dump_mssa(const struct multi_snake_slice_analyzer *mssa, enum nvmeib_block_io_op op,
		       const struct nvmeibc_raid1 *pr, nvmeib_trace_level trace_level)
{
	size_t len = __get_mssa_print_size(mssa, op);
	char *buf = kzalloc(len, GFP_ATOMIC);					// Allow dumping in interrupt context
	if (buf == NULL) {
		_N_dmesg(trace_level, t_00_mssa_dp_dbg_tools, "No Mem for MSSA dump");
	} else {
		nvmeibc_multi_snake_slice_analyzer_tostring(mssa, &buf[0], len, op, pr);
		_N_dmesg(trace_level, t_01_mssa_dp_dbg_tools, "Dump MSSA: @STR", buf);
		kfree(buf);
	}
}

void __dump_mssa(struct mssa_dump_vars *vars, nvmeib_trace_level trace_level)
{
	nvmeibc_dump_mssa(vars->mssa, vars->op, vars->pr, trace_level);
}

static bool __dp_ec_prepare_op_calc_need_to_copy_bio(struct operation *o, const struct _prepare_alloc_counts* pac)
{
	const enum nvmeib_block_io_op op = o->op;

	if (__is_bio_wrapper_for_bio(o->bios[0]->bio)) {
		return false; //we alreay copied the buffers
	}

	//the following check was not in EC datapath. Should I enable it?
	//if (nvmeibc_operation_has_bio_extention(o)){
	//	return false; //extended rider to carrier bio - rider was supposed to handle this
	//}
	
	if (nvmeib_block_io_op_is_write(op)) {
		return nvmeibc_copy_bio_buffers;
	} 

	if (nvmeib_block_io_op_is_read(op)) {
		const int read_has_mutable_bio_buffers = o->nd->dp.read_has_mutable_bio_buffers;
		if( read_has_mutable_bio_buffers == 2){
			return true; //always copy the buffers
		}

		if (read_has_mutable_bio_buffers == 1){
			//has_dgrd_topology - we need to restore data from non-readable segments, so cannot allow buffers to modified by an external application
			//enable_edic_check - if we need to verify that we read data correctly - we also use private buffer
			return pac->has_dgrd_topology || o->nd->dp.enable_edic_check;
		}
	}

	return false;
}

int dp_ec_prepare_op(struct operation *o)
{
	struct nvmeibc_topology *t = o->topo;
	const enum nvmeib_block_io_op  op = o->op;
	const u64 start_lba = get_op_start_lba(o), nlbas = get_op_nlbas(o);

	// E_PREP_VERIFY - consider declaring rv and later check layout
	int pos, rv = __check_layout(nlbas, start_lba, t, (nvmeib_block_io_op_is_write(op)));
	struct _prepare_alloc_counts pac = {0};

	_ND(dp_ec_prepare_op_length, "op=@OP start=@VLBA nlbas=@NLBAS", op, start_lba, nlbas);
	if (unlikely(rv)) {
		goto _out;
	}

	nvmeibc_profiling_start_take_stats(o->nd->preparation_profiler, o);
	pos = nvmeibc_get_chunk_ind_of_lba(start_lba, t);

	//E_PREP_COUNT_CMDS_AND_LOCKS
	__prepare_alloc_counts_calc(&pac, o, nlbas, start_lba, pos, t);
	if ((pac.nlocks < 0)||(pac.ncmds < 0)) {
		_NT(trace_dp_ec_dp_ec_prepare_op, "Wrong configuration for IO @N_LOCKS,@NCMDS", pac.nlocks, pac.ncmds);
		rv = -ENOMEM;
		goto _out;
	}
	{	//E_PREP_ALLOC													// Attempt to allocate Array {locks,cmds,sgls,mssa maps}
		const int n_bytes_for_cmds = dp_cmds_calc_size(pac.ncmds);
		const int n_bytes_for_sgls = 0;
		const int n_bytes_for_mssa = pac.mssa_bio_size + pac.mssa_md_block_size + pac.mssa_prd_size;
		rv = nvmeibc_clmat_allocate(o, false, pac.nlocks, pac.ncmds, (n_bytes_for_cmds + n_bytes_for_sgls + n_bytes_for_mssa));
		if (rv < 0) {
			_NW(warn_dp_ec_dp_ec_prepare_op, DMESG_PREFIX("@DEV_NAME") ": No memory for IO num_locks=@N_LOCKS, num_cmds=@NCMDS", o->nd->name, pac.nlocks, pac.ncmds);
			rv = -ENOMEM; 		/* Critical failure */
			goto _out;
		}
		o->cmds = dp_cmds_placement_new(pac.ncmds, ((void *)o->locks) + (rv - n_bytes_for_cmds - n_bytes_for_sgls - n_bytes_for_mssa), false);
		o->md_op.orig_sgls = NULL;
		if (n_bytes_for_mssa) {	// At the end of the sgls
			int current_offset = n_bytes_for_cmds + n_bytes_for_sgls;
			if (pac.mssa_bio_size) {
				o->mssa->gf_blocks.bio = ((void *)o->cmds) + current_offset;
				current_offset += pac.mssa_bio_size;
			}
			if (pac.mssa_md_block_size) {
				o->mssa->blocks_md = ((void *)o->cmds) + current_offset;
				current_offset += pac.mssa_md_block_size;
			}
			if (pac.mssa_prd_size) {
				o->mssa->gf_blocks.prd = ((void *)o->cmds) + current_offset;
				current_offset += pac.mssa_prd_size;
			}
			BUG_ON(current_offset != n_bytes_for_mssa + n_bytes_for_cmds + n_bytes_for_sgls);
		}
	}

	o->flags.need_to_copy_bio = __dp_ec_prepare_op_calc_need_to_copy_bio(o, &pac);
	if (o->flags.need_to_copy_bio) {
		pac.n_extra_blocks += nlbas;
	}

	_ND(dp_ec_prepare_op_integrity_checks, "op=@OP start=@VLBA nlbas=@NLBAS extra_blocks=@NLBAS need_to_copy_bio=@BOOL", 
	 										op, start_lba, nlbas, pac.n_extra_blocks, o->flags.need_to_copy_bio);

	if (pac.n_extra_blocks &&
			(rv = nvmeibc_pages_alloc(&o->pages, pac.n_extra_blocks, get_tcp_mode_of_operation(o))) < 0) {
		_NW(warn_dp_ec_dp_ec_prepare_op_1, DMESG_PREFIX("@DEV_NAME") ": No memory for IO @INT extra blocks", o->nd->name, pac.n_extra_blocks);
		goto _out;
	}

	o->cmds[0].o = o;
	o->cmds[0].locksets = NULL;
	DEBUG_TOPO_CNTRS_op_constructor(o, o->cmds, NULL);
	o->io_stat_length = (u32)nlbas;
	dp_fill_locks_for_io(op, nlbas, start_lba, pos, t, o->locks, pac.nlocks, &o->cpu_mask_info);
	rv = __prepare_ec_cmds(o,nlbas, start_lba, pos, t, pac.ncmds);
	nflog(flog_1_dp_ec_dp_ec_prepare_op, "done allocating resources: o=@OPERATION, pac.nlocks=@N_LOCKS, rv=@RV, o->locks=@LOCKS, o->cmds=@CMDS", o, pac.nlocks, rv, o->locks, o->cmds);
	if (rv < 0) {
		_NW(warn_1_dp_ec_dp_ec_prepare_op, DMESG_PREFIX("@DEV_NAME") ": Error preparing commands=@RV", o->nd->name, rv);
		goto _out;
	} else {
		rv = 0;
	}
	if (o->locks) {
		nvmeibc_cllink_cmds_locksets(o->cmds, o->locks, NULL);
		nvmeibc_atomic_set(&o->locks->n_uncompleted_locks, o->locks->nlocks + LARGE_DEBUG_VALUE);
	}
	rv = 0;

_out:
	nvmeibc_profiling_end_take_stats(o->nd->preparation_profiler, o, rv);
	if (unlikely(rv))
		_NW(warn_dp_ec_dp_ec_prepare_op_2, DMESG_PREFIX("@DEV_NAME") ": io prepare finished with rv=@RV\n", o->nd->name, rv);
	return rv;
}

static void __send_unprotected_read_1rldr_cmds(struct nvmeibc_block_command *cmds)
{
	int ci, ncmds = cmds->ncmds;						// Copy as it could be destructed in the loop
	for (ci = 0; ci < ncmds; ci++) {
		if (unlikely(dp_cmds_execute_cmd(cmds, ci) != 0)) {
			struct nvmeibc_d_iocmd_comp *cmp = &cmds[ci].iocmd->comp;
			cmp->comp_code = -EAGAIN; // we can overcome practically any error by retrying BIO
			dp_cmds_analyze_rv_and_complete(cmp);
		}
	}
}

int dp_ec_execute_op(struct operation *o)
{
	struct nvmeibc_block_command *cmds = o->cmds;
	struct nvmeibc_cmd_lock *locksets = o->locks;
	const enum nvmeib_block_io_op op = o->op;
	struct nvmeibc_profiler *prof = nvmeibc_get_raid_good_path_profile_for_rwt_op(cmds->ds, op);
	int i, ncmds = cmds->ncmds, n_unprotected_cmds = 0;	/* Calc amount of unlocked commands */

	for (i = 0; i < o->num_bios; ++i)
		nflog(flog_dp_ec_dp_ec_execute_op, "start o=@OPERATION(@UNCOMP_RAID), bio=@BIO, cmds=@CMDS, ncmds=@NCMDS", o, nvmeibc_atomic_read(&o->n_uncomp_raids), o->bios[i], o->cmds, ncmds);

	if (o->op == NVMEIB_BLOCK_IO_OP_READ) {
		nvmeibc_operation_compressed_op_dump_bio(o);
		if (nvmeibc_should_use_view_lock(op, cmds->ds)) {			// All cmds are within a single praid (blockset), check this condition only once
			for (i = 0; i < ncmds; i++)
				cmds[i].nlocks_take_before_cmd = 0;					// Send without requesting lock. Note: Here 'cmds[i].nlocks_take_before_cmd != 0' because EC always uses locks, unlike jbod
			n_unprotected_cmds = ncmds;
		}
		__IO_LT_refuse_transfer(locksets);  						// read's might not take locks so don't pass them
	}

	if (n_unprotected_cmds) {   			// reads which view locks
		dp_cmds_add_readlock_to_rldr(cmds);					// Todo: Move from execute_op to prepare_op.
		if (nvmeibc_profiling_start_take_stats(prof, o))
			nvmeibc_profiling_start_take_stats_for_stage(prof, o, cmds->raid_cur_stage);
		BLKCMP_IO_ASYNC_AWAIT(__send_unprotected_read_1rldr_cmds(cmds));
	} else {
		if (nvmeibc_profiling_start_take_stats(prof, o))
			nvmeibc_profiling_start_take_stats_for_stage(prof, o, E_CMDS_STAGE_WAIT_FOR_LOCK);
		__uncompleted_cmds_list_send(&cmds->iocmd);
		BLKCMP_IO_ASYNC_AWAIT(dp_locks_send_all(locksets));	// Writes or Degraded/Unsafe raid Reads
	} // Here: Operation could kfree(), dont access it
	nflog(flog_2_dp_ec_dp_ec_execute_op, "Sent cmds: @N_UNPRO/@NCMDS", n_unprotected_cmds, ncmds);
	BLKCMP_IO_ASYNC_RESUME_SND(dp_cmds_fiber_execute_1_blockset_state_machine(o, !n_unprotected_cmds));
	return 0;
}

void dp_ec_calc_comp_state(const struct nvmeibc_block_command *cmds, int *rv, bool *retry)
{
	extern void dp_mirror_calc_comp_state(const struct nvmeibc_block_command *, int *, bool *);
	if (unlikely(cmds->is_roll_fwd_guaranteed)) {		// LKJ: Thats a hack
		WARN(cmds->ncmds != cmds->nraid_siblings, "nvmeibc wrong assumption of single rldr in op\n");
		*rv = 0; *retry = false;			// Transaction aborted but safe to assume it finished OK
	} else {
		dp_mirror_calc_comp_state(cmds, rv, retry);
	}
	if (*rv) {
		// Todo, in future, test which command failed, if roll forward of write is guaranteed then can return OK.
		// last_needed_stage = (op == NVMEIB_BLOCK_IO_OP_READ) ? E_CMDS_STAGE_DO_IO_AND_PAR : E_CMDS_STAGE_WRITE_JOURNAL; // All reads must succeed
	}
}

void dp_ec_block_completion(struct nvmeibc_d_iocmd_comp *comp, struct nvmeibc_d_iocmd_comp_tag tag)
{
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(comp);
	struct operation *o = cmd->o;

	(void)tag;

	nvmeibc_check_metadata_actions(comp);
	if (cmd->iocmd->reqs1.op <= NVMEIB_BLOCK_IO_OP_DISCARD && io_op_is_rwt(o->op)) {
		nvmeibc_profiling_end_take_cmd_stats_for_op(nvmeibc_get_raid_good_path_profile_for_rwt_op(cmd->ds, o->op), cmd->ds->disk_operation_profiler, o, nvmeibc_profiling_get_cmd_stage(cmd->iocmd), cmd, comp->comp_code);
	}
	if (o->op <= NVMEIB_BLOCK_IO_OP_DISCARD) { // Regular: Read / Write / Trim
		dp_cmds_analyze_rv_and_complete(comp);
	} else {
		dp_ec_sync_cmd_cb(cmd);					// Syncs
	}
}

void dp_ec_calc_should_abandon(struct nvmeibc_block_command *cmds, int lsi)
{
	struct nvmeibc_cmd_lock *locksets = cmds->locksets;
	struct nvmeibc_cmd_lock *lo = locksets + locksets[lsi].owner_idx;
	int ci;

	if (unlikely(cmds->o->op == NVMEIB_BLOCK_IO_OP_READ)) // Just a hack!
		goto _out; // Special topology where read takes locks
	for (ci = 0; ci < cmds->ncmds; ci+= cmds[ci].nraid_siblings) {	// Make macro: for_each_rldr
		const struct nvmeibc_block_command *rldr = &cmds[ci];
		if (!nvmeibc_clmat_is_linked(cmds->o->CLmat, ci, lsi, locksets))
			continue;		// In V2.0 - Only one rldr is connected to lock leader so the loop is for future use
		if (rldr->was_transaction_abandoned) {
			NVMEIBC_LOCK_SET_UNLOCK(lo, RELEASE_LOCK__FORCE_ABANDON, RELEASE_LOCK_REASON__ABANDON_WRITE_PARTIAL_SLICE);
		} /* Enough: Corruption af 1 raid-leader to abandon locks */
		{	// Copy binfo of last rldr (for locks transfered to another IO). It is guaranteed that 'post' is initialized even if raidldr executino failed
			const union nvmeib_blkset_info bi = { .all = rldr->rld.post.all };
			dp_locks_put_TxID_dbits(locksets, lsi, bi, true);
		}
	}
_out:;
}

static const char* __data_segment_index_as_str(u8 i, bool is_vlba_segment){
	const char* names[] =      {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "Da", "Db", "Dc", "Dd"}; // up to 14+2 (N_MAX_RAID_SLICE_LEN)
	const char* vlba_names[] = {"D0[*]", "D1[*]", "D2[*]", "D3[*]", "D4[*]", "D5[*]", "D6[*]", "D7[*]", "D8[*]", "D9[*]", "Da[*]", "Db[*]", "Dc[*]", "Dd[*]"};
	if (is_vlba_segment)
		return vlba_names[i];
	else
		return names[i];
}

static int __translate_addr_by_cfg(struct dp_block_translation_unit *tu)
{
	int rv = 0;
	u64 slba = 0;
	u64 start_first_slice = 0;
	struct nvmeibc_topology *t;
	const struct nvmeibc_raid1 *r;
	struct dp_io_topo_iterator it;
	struct t_dp_block_trans_input  *_inp = &tu->input;
	struct t_dp_block_trans_output *_out = &tu->output;
	u8 i;
	int d_own_seg;

	NFIN;
	t = nvmeibc_topology_get(&_inp->nd->topologies);
	if (unlikely(__check_layout(_inp->nlbas, _inp->vlba, t, false))) {
		rv = -ENXIO;
		goto _out;
	}

	_out->ci = nvmeibc_get_chunk_ind_of_lba(_inp->vlba, t);
	dp_io_topo_iterator_init(&it, _inp->vlba, _inp->nlbas, t, _out->ci);
	dp_io_topo_iterator_next(&it, 'l');
	_out->ri = (int)(it.res.r - t->chunks[_out->ci].raid1s);
	_out->io_perm = nvmeibc_raid1_get_io_perm(t, _out->ci, _out->ri);
	_out->n_cmds = it.res.r->replicas;
	_out->n_locks = 0;

	r = it.res.r;
	slba =  			(it.res.rlba / r->slice_size);
	start_first_slice = (it.res.rlba % r->slice_size);
	d_own_seg = get_owner_seg_slice_start(r, it.res.rlba);
	for (i = 0; i < r->replicas; ++i){
		struct nvmeibc_disk_segment *segment = &r->segments[i];
		const u64 dlba = slba + segment->first_lba;
		const u8 index = nvmeibc_raid1_seg2role(r, d_own_seg, i);
		_out->disks[i] = segment->disk;
		_out->offs[i] = dlba;
		if (index < r->slice_size){
			_out->descr[i] = __data_segment_index_as_str(index, index==start_first_slice);
		} else if (index == r->slice_size){
			_out->descr[i] = "P";
		} else {
			_out->descr[i] = "Q";
		}
	}
_out:
	nvmeibc_topology_put(t);
	NFOUT;
	return rv;
}

static int __translate_addr_by_topology(struct dp_block_translation_unit *tu)
{
	int rv = 0;
	u64 alignment_sectors;
	struct nvmeibc_topology *t;
	struct dp_io_topo_iterator it;
	int i, is_update, ncmds, has_protection;
	struct operation o = {0};
	struct multi_snake_slice_analyzer *mssa = my_kvzalloc(sizeof(*mssa), GFP_NOFS);
	struct nvmeibc_block_command *cmd, *end;
	struct t_dp_block_trans_input  *_inp = &tu->input;
	struct t_dp_block_trans_output *_out = &tu->output;
	struct _prepare_alloc_counts pac;

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
	dp_io_topo_iterator_next(&it, 'l');
	_out->ri = (int)(it.res.r - t->chunks[_out->ci].raid1s);

	_out->io_perm = nvmeibc_raid1_get_io_perm(t, _out->ci, _out->ri);
	if (nvmeibc_topo_is_invtopo_state(_out)) {
		rv = -EIO;
		goto _out;		// Warning, translation is hazardous without a valid topo, may crash the system
	}
	alignment_sectors = (u64)(o.nd->dp.p.snake_size * it.res.r->slice_size); // 1 full snake
	o.mssa = mssa;
	{	// Check if operation translation is possible (valid IO is in single operation)
		const u32 n_max_slices = (nvmeib_block_io_op_is_write(_inp->op)) ? o.nd->dp.p.binje : LOCKSET_SLICES;
		if (_inp->nlbas > n_max_slices * it.res.r->slice_size) { // Invalid size for single operation
			rv = -EIO;
		} else {	// Check if IO is in bound of single operation
			const u32 offset = _inp->vlba % (n_max_slices * it.res.r->slice_size);
			if ((offset + _inp->nlbas) > (n_max_slices * it.res.r->slice_size)) {
				rv = -EIO;
			}
		}
		if (rv) goto _out;
	}
	mssa_init(o.mssa, &pac, _inp->vlba % alignment_sectors, _inp->nlbas, _inp->op == NVMEIB_BLOCK_IO_OP_READ, o.nd->dp.p.binje, o.nd->dp.p.snake_size, 0, &it.res);
	o.mssa->no_jour = true;
	ncmds = __mssa_count_number_of_required_bio_cmds(o.mssa, _inp->op);
	if (ncmds > (s16)ARRAY_SIZE(_out->disks)) {  // Todo: Dont count write_journal commands
		rv = -EINVAL;
		goto _out;		// Warning, translation mechanism is incorrect, something went wrong...
	}

	_out->n_cmds = ncmds;

	/* Generate commands that will not be sent */
	o.cmds = dp_cmds_kvzalloc(ncmds);
	dp_ec_cmds_add_for_raid(&o, &it.res, 0);
	// TODO IMPORTANT
	// Replace with mssa (print multi_slice)
	is_update = (o.mssa->strategies[0] != COMPLEMENT_STRATEGY);
	has_protection = !o.mssa->no_rw_p;
	for (cmd = o.cmds, end = cmd + _out->n_cmds, i = 0; cmd < end; cmd++) {
		NVMESH_BUG(cmd->my_stage == E_CMDS_STAGE_WRITE_JOURNAL, __dump_operation_report, &o, "cmd->my_stage=%d", cmd->my_stage); // Skip jour cmds as they dont convey any info
		_out->disks[i] = cmd->ds->disk;
		_out->offs[ i] = cmd->iocmd->reqs1.disk_address;
		if (nvmeib_block_io_op_is_write(o.op))
			_out->descr[i] = __wr_cmd_type_tostring(cmd, is_update, has_protection);
		else
			_out->descr[i] = __rd_cmd_type_tostring(cmd);
		i++;
	}
	if (1) {
		const size_t len = __get_mssa_print_size(o.mssa, _inp->op);
		_out->mssa_output = kzalloc(len, GFP_NOFS);
		if (_out->mssa_output != NULL) {
			nvmeibc_multi_snake_slice_analyzer_tostring(o.mssa, _out->mssa_output, len, _inp->op, it.res.r);
		}
	}
	_out->n_cmds = i;   		// Array was decreased in size
	dp_cmds_free_all(o.cmds);
	if (tu->input.translate_locks)	/* Generate locks (if needed) */
		dp_block_translation_unit_calc_locks(tu, &it);
_out:
	my_kfree(mssa);
	nvmeibc_topology_put(t);
	NFOUT;
	return rv;
}

int dp_ec_translate_addr(struct dp_block_translation_unit *tu)
{
	int rv = 0;
	if (tu->input.translate_by_cfg){
		rv = __translate_addr_by_cfg(tu);
	} else {
		rv = __translate_addr_by_topology(tu);
	}
	return rv;
}

void nbdpec_md_to_string(void* md_arr, int md_size, int len, char type)
{
	int i;
	_NT(t00ecmdts, "type=@TYPE_CHR", type);
	if (	   type == 'D') {
		for (i = 0; i < len; i++) {
			union nvmeibc_block_dp_ec_data_block_md *md = (md_arr+(md_size*i));
			_NT(t01ecmdts, "@IND) {ver=@MD_VER, d2j_rng=@X edic=@EDIC, txid=@TXID, jri=@JRI}", i, md->D.version, md->D.d2j_rng, md->D.edic, md->tx_id, md->jri);
		}
	} else if (type == 'P') {
		for (i = 0; i < len; i++) {
			union nvmeibc_block_dp_ec_data_block_md *md = (md_arr+(md_size*i));
			_NT(t02ecmdts, "@IND) {ver=@MD_VER, d2j_rng=@X edic=@EDIC, db=@DBITS@DBITS,txid=@TXID, jri=@JRI}", i, md->D.version, md->D.d2j_rng, md->P.edic, md->P.dbits_0, md->P.dbits_1, md->tx_id, md->jri);
		}
	} else if (type == 'J') {
		for (i = 0; i < len; i++) {
			union jblock_md *md = (md_arr+(md_size*i));
			struct jblock_md_decompressed jmd = nvmeibc_block_dp_ec_jmd_decode(md);
			_NT(t03ecmdts, "@IND) {j2d=@J2D, txid=@TXID, version=@VERSION, tx_bmp=@TXBM, has_next=@BOOL}", i, jmd.j2d, jmd.tx_id, jmd.version, jmd.tx_bmp, jmd.has_next);
		}
	} else {
		WARN(true, "nvmeibc: wrong usage of function, type=%c\n", type);
	}
}

void nvmeibc_block_dp_ec_copy_md_dbits(const union nvmeibc_block_dp_ec_data_block_md *src, union nvmeibc_block_dp_ec_data_block_md *dst) {
	dst->P.dbits_0 = src->P.dbits_0;
	dst->P.dbits_1 = src->P.dbits_1;
	WARN((dst->P.dbits_0 > N_MAX_RAID_SLICE_LEN)||(dst->P.dbits_1 > N_MAX_RAID_SLICE_LEN), "nvmeibc bug! illegal dbit={0x%x,0x%x} md=0x%llx", dst->P.dbits_0, dst->P.dbits_1, src->raw);		// Traps for un/initialized MD values
}

/* Restore data from parities also restores metadata using this copy function */
void nvmeibc_block_dp_ec_copy_md(const union nvmeibc_block_dp_ec_data_block_md *src, union nvmeibc_block_dp_ec_data_block_md *dst)
{
	const u32 dst_edic = dst->P.edic;
	*dst = *src;
	WARN((dst->P.dbits_0 > N_MAX_RAID_SLICE_LEN)||(dst->P.dbits_1 > N_MAX_RAID_SLICE_LEN), "nvmeibc bug! illegal dbit={0x%x,0x%x} md=0x%llx", dst->P.dbits_0, dst->P.dbits_1, src->raw);		// Traps for un/initialized MD values
	dst->P.edic = dst_edic;
	if (!nbdpec_md_was_data_never_written(src))
		nbdpec_md_mark_no_journal(dst, dst->tx_id, true);
}

/****************** No RDDA, read-modify-write metadata actions ***************/
union nvmeibc_b_dp_ec_rmwmd_action {					// u64 action struct
	struct {
		enum nvmeib_block_io_op op : 8;					// Daniel: Too much bits
		union {											// First 32 bits
			/*
			struct {									// Params for tx id on wraparound trim action
				u8 is_parity       : 1;
				u8 reserved        : 7;
				u16	dbits_turn_on  : 16;					//
				u32 reserved1      : 32;
			} __attribute__((packed)) tx_id_wrap;
			*/  // TXID wraparound is done on client side for now.
		} __attribute__((packed));
	};
	u64 all;
} __attribute__((packed));

void nvmeibc_block_dp_ec_dmd_read_mod_wr(void*_md_ptr, u64 params);
void nvmeibc_block_dp_ec_dmd_read_mod_wr(void*_md_ptr, u64 params)
{
	union nvmeibc_block_dp_ec_data_block_md *p = _md_ptr;
	const union nvmeibc_b_dp_ec_rmwmd_action act = {.all = params};
	(void)p;
	WARN(true, "nvmbeib unsuported param=0x%llx, op=%d (currently there are no read_mod_write actions.)\n", params, act.op);
	BUILD_BUG_ON(sizeof(union nvmeibc_b_dp_ec_rmwmd_action) != sizeof(u64));
}
