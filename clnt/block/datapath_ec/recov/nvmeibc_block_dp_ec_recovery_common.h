#ifndef NVMEIBC_DP_EC_RECOV_COMMON_H
#define NVMEIBC_DP_EC_RECOV_COMMON_H
/* Generic utils for various syncs of EC */
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "block/nvmeibc_block_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"

/***************** Common for READ+WRITE sync's (i.e: No whole, HTR, cold, txid-wraparound)  *********************/
bool dp_sync_is_slice_neverwritten( const struct recovery_sync_op *so, u64 lba, const roles_bmp_t readable_segs, char debug_reason); // debug_reason = 'D' - destroy slice, 'R' - Reconstruct, 'W' - txid wraparound
int  dp_sync_get_any_non_readfail_errors( struct recovery_sync_op *so);
u32  dp_sync_gen_read_fail_bit_mask(      struct recovery_sync_op *so);
void dp_sync_notify_toma_on_write_failure(struct recovery_sync_op *so);
bool dp_sync_verify_binfo_is_legal(          struct recovery_sync_op *so, const union nvmeib_blkset_info binfo);

/* Prepares binfo DB values for dbits turnon */
static inline void dp_sync_calc_new_binfo_dbits_turnon(struct recovery_sync_op *so, const sgmnts_bmp_t dbits_turnon_bmp) {
	struct nvmeibc_raid_leader_cmd_ctx *rld = &so->cmds->rld;
	struct nvmeibc_dbits_tx tx = {.action.db_turn_on_bmp = dbits_turnon_bmp, .action.num_parities = nvmeibc_raid1_get_protect_lvl(so->r1)};
	union nvmeibc_dbits_entry pre = {.all_bits = rld->pre.bits.dirty};
	BUG_ON(__is_raid1_mirror(so));  // Currently not being used in mirror
	rld->post.bits.dirty = nvmeibc_dbits_tx_apply(&pre, &tx);
	if (rld->post.bits.dirty != rld->pre.bits.dirty) {
		union nvmeibc_dbits_entry post;
		mark_blockset_info_not_written(so);
		post.all_bits = rld->post.bits.dirty;
		_NTSO(trace_01_calc_new_binfo_dbits_turnon, "pre_ram_dbits=[@DBITS], ram_dbits_after_turnon=[@DBITS]", pre.all_bits, post.all_bits); (void)post;
	} else if (rld->post.all != rld->pre.all)
		_NTSO(trace_02_calc_new_binfo_dbits_turnon, "pre_ram_dbits do not need update however TXID differs pre_ram_txid=[@TXID], post_ram_txid=[@TXID]", rld->pre.bits.txid, rld->post.bits.txid);
}

/***************** EC No Write Hole *******************************************/
void dp_ec_sync_no_write_hole_execute_op(struct recovery_sync_op *so);

enum dp_ec_dirty_bits_operation {
	DBITS_OP_NOP = 0,
	DBITS_OP_TURN_ON,
	DBITS_OP_TURN_OFF,
};

struct dp_ec_restore_plan { // For ease of use
	const ulong parity_target_mask;				// Parities that have to be updated
	const u32   invalid_sources;				// All non-readable segments (both writable and DEAD)
	const roles_bmp_t md_to_calc;				// Which mds should be calced because the block is really going to be written and not just need it's data for parities calculation.
	const bool  parity_rebuild_required;		// Parity is part of target mask
	const bool  invalid_data_rebuild_required;	// Data is part of invalid sources
	const enum  dp_ec_dirty_bits_operation dbits_op;	// Dirty bits are being changed? and in which way?
};

void prepare_parity_md(struct recovery_sync_op *so, const struct dp_ec_restore_plan plan);

/***************** EC No Write Hole - END *******************************************/
enum cand_state {
	NOT_A_CANDIDATE = 0,      //For sure not the last TX, Throw away candidate
	PARTIALLY_COMMITED,       //Roll Forward
	MIGHT_BE_FULLY_COMMITED,  //Could not fully analyze TXBM (may need to update dbit) and need to check max_data_txid + 1 TXs for dbits update
	FULLY_COMMITED 		   	  //Transaction was completed, need to check max_data_txid + 1 TXs for dbits update
};

enum cand_type {
	UNRESOLVED = 0,    // Candidate type not resolved yet.
	ROLL_FWD,		   // Candidate should be roll forward.
	ROLL_BWD           // Candidate should be roll backward.
};

/* Send all commands of current stage of sync*/
void nvmeibc_sync_send_cur_stage_cmds(  struct recovery_sync_op *so);
/* Get rv of current stage of sync */
int  nvmeibcbdpec_get_rv_cur_stage_cmds(struct recovery_sync_op *so);

/********************* General Cold/Hot Common functions *******************/
u32 nvmeibcbdpec_calc_max_txid_in_data_md(const struct recovery_sync_op *so);
/* Merge all dbits of all slices from all sources (RAM, MD, UNK, Convicts) */
union nvmeibc_dbits_entry
	nvmeibcbdpec_calc_max_dbit_in_ram_md( const struct recovery_sync_op *so);
union nvmeibc_dbits_entry
	nvmeibcbdpec_calc_worst_case_dbits(const struct recovery_sync_op *so);

bool __is_journal_committed(struct nvmeibc_raid1 *r1, struct jent_md_decompressed jent_mds[], sgmnts_bmp_t analyzed_segs);

// Unused
#define __db_update_required(rld) ((rld).post.bits.dirty != (rld).pre.bits.dirty)
// Should be part of Operation page manager
void nvmeibc_fill_ndb_for_ec(struct multi_snake_slice_analyzer *mssa, int map_start, struct nvmeib_data_buffer *ndb, unsigned int nlbas, struct nps_block_iter *nbi);

/********************* Journal candidate for IO transaction *******************/
// Common to Hot stale lock recovery and cold recovery (Write hole solution)
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"

struct nvmibc_blockset_candidates {
	struct list_head can_list;			// List of 'struct nvmibc_tx_candidate'
	u32 blkset_lba : 30;				// J2D in units of blksets. Enough for up to (2^30*BYTES_IN_LOCKSET) disk size: 128[TB]
	u32 status     :  2;				// Holds at least 3 values: {todo, in process, aready fixed}
};
void nvmibc_blockset_candidates_kfree_deleted(struct nvmibc_blockset_candidates *);

struct candidate_tx_params {
	u64 j2slba;							 // Instead of using j2d we use the first j2slba because it is identical to all disks in the transcation. No need to store array of j2d's
	u32 tx_id;							 // The txid of the candidate.
	//u32 version_unused;                // Internal candidate should always be on latest version. No need to support versions here
	roles_bmp_t tx_bmp[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // The txbm on each slice of the candidate (taken from pivot pari).
	int len;  							 // The number of slice in the candidate (the len of tx_bm array).
};

struct nvmibc_tx_candidate {
	struct candidate_tx_params b; 		// Base journal metadata for all segs of transaction (taken from pivot pari): j2d is slba (identical to all segs in tx_bmp)
	struct candidate_location {			// Array of journal entries location for segment. Todo: Make u32
		ulong is_data_commited[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY, sizeof(ulong)*8)]; // Boolen, on this segment+offset_in_pivot_jent data was written and points back to journal
		u16 jri;					// Which JRI
		u16 jentry;					// Which entry in JRI
		bool is_jour_commited;		// Boolen, on this segment jour was written and points to data via j2d
		bool is_garbage;		// Boolen, Garbage collection decided this journal entry is unneeded, marked on first element in locations array
		u64 rng_gen_id;
		u64 rng_start_lba;
		binje_t rng_binje;
		unsigned rng_size_lba;
		unsigned rng_num_ents;
		u16 binje_offset;			// The first slice in transaction contains this jentry in its txbm (For parities and in binje=1 == 0). Possible value 0 or 1. For Raid 8+2 io to vlba [7..9] will yield 2 journal blocks on parity and offset 0 for segs 7,8,9, offset 1 for seg 0,1
		u8 jentry_gen_id;
		u8 binje_len;     // the length of the chain in the jetnry
	} locations[N_MAX_RAID_SLICE_LEN];	// arr[i] for seg[i]. Note: array not aligned to Tx_bmp!!!!
	struct list_head next;			// Linked list of such candidates, all sharing identical 'blockset_lba'
	uuid_be cuuid;
	enum cand_type type; 		    // The type of operation that should be execute on the candidate.
};

static inline bool nvmibc_is_tx_candidate_invalid(struct nvmibc_tx_candidate *cand) {
	return cand->b.len == -1;
}

static inline void nvmibc_tx_candidate_invalidate(struct nvmibc_tx_candidate *cand) { // Mark candidate for deletion (Illegal TxBM)
	cand->b.len = -1;
}

struct data_slice_d2j_info {		// Data candidate to verify journal candidate correctness
	u32 max_txid;					// Max txid in data-md
	u32 jri;						// The jri on max-txid segs in blkset.
	roles_bmp_t txbm[LOCKSET_SLICES];	// Bitmap of roles in the slice with maximal txid
	int len;								// len of txbm array (number of slices that the max_txid appears on.)
	bool is_txid_wraparound_or_write_called_it_failed;  // is txid wraparound failed or the write cmd which called it failed.
};

/* Given Journ candidate find the read-data-command and offset of the data block
   which is relevant to the journal.
   Returns ci = cmd index and
           ofset_rv = [0..LOCKSET_SLICES-1] */
u32 nvmeibc_tx_get_dblk_ofset_in_cmd(const struct recovery_sync_op *so,
				int si, const struct nvmibc_tx_candidate *cand, int i, u32* ofst_rv);

/* Test whether data back references correctly to journal candidate.
   Return: Negative on error, 0 if data not commited, 1 if data commited & roll
		forward required (also fills data_commited field to relevant segs). */
enum cand_state nvmeibc_tx_does_d2j_j2d_match(const struct recovery_sync_op *so,
		struct nvmibc_tx_candidate *jour, const struct data_slice_d2j_info *data);	 	//  EC-1475: Unify with HTR: sat_data_committed

/* Do we have precalculated list of journal candidates (at least 1) */
bool nvmeibcbdpec_has_rollfwd_jour_candidate(const struct recovery_sync_op *so);
const struct nvmibc_tx_candidate*
     nvmeibcbdpec_get_rollfwd_jour_candidate(const struct recovery_sync_op *so);
bool __attribute__((__unused__)) nvmeibcbdpec_has_jour_candidate(const struct recovery_sync_op *so);
const struct nvmibc_tx_candidate* __attribute__((__unused__))
     nvmeibcbdpec_get_jour_candidate(const struct recovery_sync_op *so);

/* Get access to block #ofst of command 'ci'. */
void nvmeibc_tx_get_ptrs_to_blk_in_cmd(const struct recovery_sync_op *so, u32 ci, u32 ofst,
		union nvmeibc_block_dp_ec_data_block_md **dmd, void**buf);

#endif  // H beginning

