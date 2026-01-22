/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.h"
#include "server/nvmeibs_main_sim.h"

struct jdr;

/* This frameworks designed for integration testing of io bad path: syncs/recoveries.
   Dedicated other unitests cover simple corner cases while this framework
   covers various complicated flows.
   It implements a time-line of IO which cause a problematic state that must be
   correctly resolved by 'rer'. The timeline consists of the following events:

   TIMELINE:
   0. 'pre' data exists before 'ree' starts its work. System is in good state
   1. 'ree' client made a history of trasnactions to each blockset.Last tx in
      each blockset can fail during execution
   2. Natural disaster occurs: topology changes, bad sectors appear, etc..
   3. 'rer' has a specific state before encountering 'ree's deeds. Example:
      State of JAM, throtelling conditions, etc
   4. 'rer' gets a trigger to enoucnter 'ree' deeds. Recovery msg from toma, bio
      to broken blockset, scrubbing, etc
   5. 'rer' solves intermediate state of 'ree' bring the system to consistent
      state.
   6. 'rer' completes its bio (if it had any)

   FRAMEWORK ACTIONS:
   0. Generate randomly/via iterators possible states of the timelines 0..4 steps
   1. Injecting the snapshot of system appearance at the end of 4. Does not have
      to actually inject all stages, only the final image
   2. Calculate expected outcome of stages [5..6]. And expected flow
   3. Verify both outcome and flow */
/*****************************************************************************/

struct t_slice_all_blks {
	u64                                *blocks[N_MAX_RAID_SLICE_LEN];	// The consistent data that should be written to disk but not neccasrily written yet. (data block pointers, allocated in a single chunk)
	union nvmeibc_block_dp_ec_data_block_md md[N_MAX_RAID_SLICE_LEN];   // Metadata as should be on disk.
};

struct t_slice_data {					// Todo: Rename to 't_multislice_all_blks' and use as array of the above! Misleading name - contains P & Q too + multi-slice data
	u64 *blocks[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY][N_MAX_RAID_SLICE_LEN];	// The consistent data that should be written to disk but not neccasrily written yet. (data block pointers, allocated in a single chunk)
	union nvmeibc_block_dp_ec_data_block_md md[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY][N_MAX_RAID_SLICE_LEN];    // the blocks data metadata as should be on disk.
};
void t_slice_data_extend_slice_inj_to_ms_inject(struct t_slice_data *dst, struct t_slice_all_blks *src);

struct t_slice_tx_ptrs {				// Inject pointers for generic transaction. All arrays index 'i' is i'th block in slice (NOT seg[i]!!!)
	struct t_slice_data pre;			// pre IO data; before the Tx of ree started.
	struct t_slice_data ree;			// slice data state of fully commited ree IO data as if all segs are RW; later the recoverer will try to clean the mess
	struct t_slice_data nat;			// slice data state of fully commited ree IO data as if all segs are RW + bad sectors created by natural disaster. later the recoverer will try to clean the mess
	struct t_slice_data slice_starting_state; // slice data state before executing anything (A  combination of nat, ree and pre).
	struct t_slice_data rer;            // slice data state as rer IO data as if all segs are RW and no bad sectors. rer may discover the mess via dedicated recovery process or via another user IO in case of IO the data is here
	struct t_slice_data post_recov;     // Post_recovery data state, todo: init the blocks fields
	struct t_slice_data exp;			// contains the data which de facto written on segment including the dead segs + dmd of composed rer+ree+pre. different from rer because rer assumes all segs are RW except from parities.
	struct block_inject_ptrs bptrs[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY][N_MAX_RAID_SLICE_LEN];
};

enum ec_tx_abort_stage {				// Scenarios of different stages of transaction commit
	ec_tx_abort_stage_partial_data=0,	// Data is partial, but NOT EMPTY, its piggybacks partially written. Journal is full. roll forward
	ec_tx_abort_stage_success=1,		// Tx completed successfully, nothing to roll forward, only free journals
	ec_tx_abort_stage_partial_journal=2,// No data written, no roll forward, Journal piggybacks partially commited, Possibly empty journal
	ec_tx_abort_stage_nodata=3,         // Jurnal fully written, all its piggybacks written to RAM. no data written, no roll forward
	ec_tx_abort_stage_last=4
};	// If few Tx exist in blockset then only last one can have stage [0..1] (HTR will process it), rest have stages [1..3] (undetectable to HTR) with lower TxID's!
static inline enum ec_tx_abort_stage ec_tx_abort_stage_rand_get_old_tx(   void){ return ec_tx_abort_stage_success      + rand()%(ec_tx_abort_stage_last-ec_tx_abort_stage_success);}
static inline enum ec_tx_abort_stage ec_tx_abort_stage_rand_get_failed_tx(void){ return ec_tx_abort_stage_partial_data + rand()%(ec_tx_abort_stage_success+1);}

struct t_ec_recov_blkset;

//RRRR: PAY ATTENTION: the framework doesn't work with debug DI turned-on!!!


/*************** Representation of Single Slice IO transaction ****************/
/* Bitmaps: all include parities and i'th elem is NOT seg[i], but relative to slice start! */
/* LockBitmaps: include at most 3 tunred on bits according to lock scheme (0, p, q) */
struct t_ec_recov_tx {			// Parameters for a single IO trasnaction injected for recoveries to solv
	struct t_ec_recov_blkset *blkset;			// Backpointer to blockset
	struct t_input_params {						// ------------ Input params
		struct TstPRaid  sraid;					// Protection raid, where 'ree' & 'rer' operate
		u32 tx_height;							// Number of slices in transaction.
		s32 slba;								// A slice in which transaction occurs
		struct t_recovepre_params {				// Params of the IO transaction which is recovered
			u32 txid;							// The txid which will be written to slice data where ree havn't writteb too or been overriden. Cause we can't have same txid in different slice on blockset so need a never used txid, so we can't used an history txid or txid that have been used in other tests or iterations. in htr the pre txid should be latest txid -2 because if no txid has been written we don't want htr to consider the pre tx and roll it forward or backward.
			u32 history_ram_dbits;				// Bitmap: Ram dbits which was genrerated by history TXs and should be take to considartion by latest.
			bool is_never_written[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		 	// bool bmp of slices: is the data of pre is never-written.
			bool is_never_written_union;		// bool is any of is_never_written[] true
			bool is_parity_explicitly_marked_neverwritten[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]; // bool bmp of slices: If we eant to generate dbits then parities should always be marked_neverwritten
			bool is_parity_explicitly_marked_neverwritten_union;   // bool is any of is_parity_explicitly_marked_neverwritten[] true
		} pre;									//
		struct t_recoveree_params {				// Params of the IO transaction which is recovered
			const uuid_be *uuid;				// UUID of the 'ree' client (used to locate its JRI)
			enum ec_tx_abort_stage aband_stg;	// At which stage tx was aborted
			u32 tx_bm[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];	// TXBM including parities, of what ree intended to write
			u32 tx_bm_union;					// Bitmap: union of tx_bm
			u32 txid;							// TXID value post transaction (What 'ree' wanted to write to RAM).
			u32 is_topo_not_eq_to_rer : 1;		// Randomally alter topology of 'rer' in valid ways to simulate 'ree' & 'rer' in different topos
			u32 inject_allien_lock_id : 1;		// If 'true' - JAM/Serjio/Sync will not know this client and sync will not clean any journals
			u32 is_old_completed : 1;			// Tx was completed long time ago and irrelevant for hot recovery, only for cold. It be partial coz it was overwritten by newer transactions
			u32 is_garbage_for_jgc : 1;			// Relevant only for jgc recovery: Consider this transaction as garbage for garbage collector
		} ree;
		struct t_natural_disaster {				// After 'ree' did its IO, did natural disaster occured before 'rer' has chance to fix the problem
			u32 allow_data_loss : 1;			// Can enough bad sectors occur that will force a permanent data loss?
			u32 allow_rer_network_fail : 1;		// Can random tranisent network failures occure to 'rer'? (Transport layer errors
			u32 is_journal_overriden   : 1;     // Is journal been cleaned/Overriden after TX finnished successfully (making it not candidate for future recoveries)? In HTR the lock is also unlocked because if the lock staying locked it's a special case of ec_tx_abort_stage_partial_journal when there is no journal written at all.
			u32 allow_bad_sectors : 1;			// Can bad sectors occur?
			u32 destory_slice : 1;              // Should bad_sectors_destro_slice
		} nat;
		struct t_recoverer_params {				// Params of recovererer which accesses failed transaction
			enum nvmeib_block_io_op bio_type;  	// If 'rer' does write, read or nothing IO after recovering 'ree'
			u32 tx_height;							// Number of slices in transaction.
			u32 tx_bm[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];							// If 'rer' does write or read IO after recovering 'ree', TXBM including parities, of what rer is going to write or read
			enum NVMEIBTC_DS_MODE topo[N_MAX_RAID_SLICE_LEN];
			union io_perms_bitfield io_perm;	// IO permissions to the protection raid according to current topology
		} rer;
	} inp;
	struct t_pre_tx_params {                // --------- Pre TX params
		u32 ram_dbits     : 16; 			// Bitmap: Which segs has dirty dbit turned on in RAM.
		u32 ram_dconv     : 16; 			// Bitmap: Which segs has dirty convit turned on.
		u32 slice_dbits[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];             // Bitmap: Which segs has dirty bit turned on (can be a dirty convict that passed to slice).
		u32 slice_dbits_union;             // Bitmap: union of slice_dbits.
	} pre;

	// ------------ Intermediate structs generated from the input and pre params
	struct t_tx_recoveree_bmps {			// See remark about bmps in struct header
		//the following bitmaps instructs what state (on disk/memory) should be achieved
		struct nvmeibc_roles_bmps topo;		// Bitmap: Which roles have access mode according to 'ree's topology
		u32 jmd_writn[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]; // Bitmap: Which journals were written by ree
		u32 jmd_writn_union;                 // Union of jmd_writn
		u32 dmd_writn[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];			// Bitmap: Which datas    were written by ree (if @jmd_writn is entire txbm)
		u32 jmd_with_correct_j2d[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		// Bitmap: At least 'jmd_writn' but can have more toggled bits, due to old previous transactions
		u32 jmd_with_correct_j2d_union : 16;		// Bitmap: At least 'jmd_writn' but can have more toggled bits, due to old previous transactions
		u32 jmd_with_valid_chain : 16;      // Bitmap: At least 'jmd_writn' but can have more toggled bits, due to old previous transactions
		u32 jmd_with_fake_chain : 16;      // Bitmap: included in jmd_with_valid_chain will conain old txid for not creating a candidate.
		u32 chain_height[N_MAX_RAID_SLICE_LEN];  // the height of the chain on each role, 0 if no chain at all.
		u32 abandoned_jent	: 16;			// Bitmap: Which journal entires were abandoned in nat.
		u32 is_jour_committed : 1;			// Boolean: Did 'ree' fully commit his journal (According to TxBM, his topo and abort stage)?
		u32 ram_txid_no_wr  : 16;			// LockBitmap: which txid  piggybacked did not occur (blocks are OK but RAM is not). If ree->journal not commited (not fully written), Then TxID might not be written to all locks, coz piggybacks on journal commands never occured
		u32 ram_dbits_no_wr : 16;			// LockBitmap: which dbits piggybacked did not occur (piggyback failed or didn't sent yet).
	} ree_bmp;
	struct t_tx_recoverer_bmps {			// See remark about bmps in struct header
		//the following bitmaps helps to verify that the desired action happened
		struct nvmeibc_roles_bmps topo;		// Bitmap: Which roles have access mode according to 'rer's topology
		u32 bad_sec_bmp[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];           // Bad sectors on ree tx's slice
		u32 jmd_ready[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]; // Bitmap: Which journals are accessible   to 'rer' (subset of jmd_writn)
		u32 jmd_ready_union;					// union of jmd_ready
		u32 jmd_kosher[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];			// Bitmap: Which journals are trust worthy to 'rer' (subset of jmd_ready) and can be used for roll forward
		u32 jmd_kosher_intersect;          // intersection of jmd_kosher
		u32 jmd_kosher_union;              // union of jmd_kosher
		u32 dmd_kosher[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]; 			// Bitmap: Which datas    are accessible & trust worthy to 'rer' (subset of dmd_writn)
		u32 any_dmd_kosher : 1;				// Bool: is any of the dmd's kosher
		u32 roll_fwd[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];	// Bitmap: Which roles should be rolled forward by sync for a Tx slice only (Journal blk copied to Data)
		u32 any_roll_fwd : 1;               // Bool: is any roll forward by HTR
		u32 roll_fwd_by_dbits_turnon[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // Bitmap: Which roles should be lazy rolled forward by dbits turnon by sync for a Tx slice only (Journal blk not copied to Data)
		u32 regen_fwd[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];			// Bitmap: Which roles should be rolled forward by regen by sync for a Tx slice only (Journal blk not copied copied to Data, intead regen from other blocks in slice)
		u32 can_change_slice_dbits : 16; 	// Boolean: Can 'rer' change the dbits on slice (on dmd of parities in slice). Not 'should!' but rather 'can?'
		u32 send_blkst_recov: 16;			// Bitmap: Which segs receive blkset recovered message (JMDC cleaned + Abandoned entries in Serjio cleaned)
		u32 is_jour_committed : 1;			// Boolean: Does 'rer' thinks that 'ree' fully commited his journal? 'ree' may think that it did not commit but rer may think it did, but not the opposite!
		u32 does_know_txbm : 1;				// Boolean: Can 'rer' understand what was 'ree' txbm or not (decided by HTR which is launched by cold/stale lock). For JGC and other scenarios this will be false
		u32 can_change_ram_dbits: 1;		// Boolean: Not 'should!' but rather 'can?'
		u32 can_turnof_ram_dbits: 1;		// Boolean: Not 'should!' but rather 'can?'
		u32 can_turnon_ram_dbits: 1;		// Boolean: Not 'should!' but rather 'can?' Roll-fwd or regen
		u32 can_turnon_ram_dbist_on_w_segs: 1; // Boolean: Not 'should!' but rather 'can?'. Cold recovery with both parities degrade - assumes entire blockset is dirty
		u32 can_see_ree_dbits_in_slice[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // Boolean: Can 'rer' see on slice parities the dbits that 'ree' wrote. If no then ....
		u32 can_see_all_ree_dbits_in_slice : 1;              // Boolean: Can 'rer' see on all slice parities the dbits that 'ree' wrote. If no then ....
		u32 can_see_any_ree_dbits_in_slice : 1;               // Boolean: Can 'rer' see on any of slice parities the dbits that 'ree' wrote. If no then ....
		u32 ree_dbits_turnon_on_slice[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];	// The slice dbits that 'ree' turned on
		u32 can_see_ree_txid_in_ram : 1;	// Boolean: Can 'rer' see in RAM the txid  that 'ree' wrote. If no then 'rer' would not be able to analyze 'jmd_kosher' even though they are accessible, coz they have wrong TxID form 'rer' prespective, relevant for HTR only, not cold.
		u32 can_see_any_ree_dbits_in_ram : 1;	// Boolean: Can 'rer' see in RAM some of the dbits that 'ree' wrote. If no then ....
		u32 can_see_all_ree_dbits_in_ram : 1;   // Boolean: Can 'rer' see in RAM all the dbits that 'ree' wrote. If no then ....
		u32 can_see_ree_dbits : 1;	 		// LKJ: to remove Boolean: Can 'rer' see in RAM or slice the dbits that 'ree' wrote. If no then ....
		u32 ree_ram_dbits_turnon_and_can_be_turnof_by_rer : 16;      // Bitmap: on which roles there are dbits turned on cause of ree as seen by rer and can be turned off by rer.
		struct t_tx_whole {
			u32 regen_bkw[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];			// Bitmap: Which roles should be rolled backward (by regen) for a Tx slice only.
			u32 any_regen_bkw : 1;					// Bool: any of the regen_bkw[] != 0
			u32 roll_bkw[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];			// Bitmap: Which roles should be rolled backward (by dbit turn on) for a Tx slice only (Turn on dbit for dead segments to erase possible new data upon future dbits-rebuild)
			u32 any_roll_bkw : 1;					// Bool: any of the roll_bkw[] != 0
			u32 regen[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		// Bitmap: Which roles (Topology W) will be recalculated via reed-solomon a Tx slice only by HTR (not by no_w_hole_sync)
			u32 any_regen : 1;                      // Bool: is any regen by HTR
			u32 will_rollfwd[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		// Will roll-forward ree's tx by any means (dbits turnon/ regen/ roll-fwd journals).
			u32 is_neverwritten_slice_by_data_blocks[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]; // Is all data blocks is neverwritten.
			u32 is_neverwritten_readable_parity[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // Is there a readable neverwritten parity which haven't overriden by ree/nat.
			u32 is_neverwritten_slice[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];       // is_neverwritten_slice_by_data_blocks || is_neverwritten_readable_parity
			bool is_neverwritten_source_parity_for_regen[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // Boolean: is there a readable parity which is neverwritten for potential regen.
			u32 slice_dbits_rebuild[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		// Bitmap: Which roles (Topology W) will be rebuild-dbits-turn-off on Tx slice
			u32 slice_dbits_turnon[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];        // Bitmap: Which roles will be dbits turn on on Tx slice
			u32 slice_dbits_change[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]; 		// Bitmap: Which roles will be dbits turn off or on Tx slice
		} whole;
		struct t_tx_nowhole {
			u32 will_call_nwhole_sync : 1;      // Bool: will nwhole sync will be called at the end of recovery.
			u32 ram_dbits_turnof : 16; //Bitmap: Which roles (Topology W) will be dbits turnoff by no_write_hole sync (Full blockset, not only Tx slice)
			u32 ram_dbits_turnon_on_turnoff: 16; //Bitmap: Which roles will be dbits turnon by no_write_hole sync (Dead parities in case of turnoff)
			u32 regen : 16;			// (not including regen_bkw) Bitmap: Which roles (Topology W) will be regened by no_write_hole sync (Full blockset, not only Tx slice).
			u32 roll_bkw;			// Bitmap: Which roles should be rolled backward (by dbit turn on) by nwhole_sync for a Tx slice only (Turn on dbit for dead segments to erase possible new data upon future dbits-rebuild)
			u32 regen_bkw;			// Bitmap: Which roles should be rolled backward (by regen) by a sync.
			u32 syn_blkst_data_change: 16;		// Bitmap: Which data should be touched/changed due to 'sync' for all slice (Full blockset)
			u32 syn_blkst_dmd_change: 16;		// Bitmap: Which dmd should be touched/changed due to 'sync' for all slice (Full blockset)
			bool is_neverwritten_source_parity_for_regen[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  // Boolean: is there a readable parity which is neverwritten for potential regen.
			u32 is_neverwritten_slice[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  	// Boolean: is there a readable parity which is neverwritten and if no readable parity are all data blocks are neverwritten.
		} nwhole;
		struct t_tx_total_recov {
			u32 slice_dbits_rebuild[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		// Bitmap: Which roles (Topology W) will be rebuild-dbits-turn-off on Tx slice
			u32 slice_dbits_turnon[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];        // Bitmap: Which roles will be dbits turn on on Tx slice
			u32 slice_dbits_change[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]; 		// Bitmap: Which roles will be dbits turn off or on Tx slice
			u32 slice_data_touched[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		// Bitmap: Which data should be touched/changed due to for this slice
			u32 max_txid_in_slice[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];          // The max_txid readable on slice (if ree wrote to a readable seg then max_txid = p->inp.ree.txid else max_txid = p->inp.pre.txid)
		} total;  // whole + nwhole
		struct t_post_recov_tx {
			u32 will_call_nwhole_sync : 1; 		                        // Bool: will rer fix dbits or RF.
			u32 syn_blkst_data_change[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];                             // Bitmap: Which data should be touched/changed due to 'rer's Tx' for all slice (Full blockset)
			u32 syn_blkst_dmd_change[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];		                        // Bitmap: Which dmd should be touched/changed due to 'rer's Tx' for all slice (Full blockset)
			u32 fixed_bad_sec[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];				                        // Bitmap: Which bad sectors will be fixed - by write or by a nwhole sync.
			u32 is_neverwritten_readable_parity[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  					// Is there a readable neverwritten parity which haven't overriden by ree or nat.
			u32 is_neverwritten_source_parity_for_regen[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  			// Boolean: is there a readable parity which is neverwritten for potential regen.
			u32 is_neverwritten_slice[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];  							// Boolean: is there a readable parity which is neverwritten and if no readable parity are all data blocks are neverwritten.
		} tx;
	} rer_bmp;
	struct t_tx_bentry { 					// Lock injection data.
		union nvmeib_lock_blkset_entry pre; // Values 'pre' the state of the blockset before 'ree' changed the servers RAMs (blockset info + locks). Previous TxID that existed before 'ree's history (TX). Can be inp.ree.txid-1 or any lower number
		union nvmeib_lock_blkset_entry ree;	// Values 'ree' wanted to write servers RAMs (blockset info + locks) according to his topo. It could fail to write those value
		union nvmeib_lock_blkset_entry nat; // Values 'natural disaster' changed the servers RAMs (blockset info + locks) too which rer will see first time - i.e. in cold it's the Blockset info which TOMA injected.
		union nvmeib_lock_blkset_entry post_recov; // State of RAM after the recovery.
		union nvmeib_lock_blkset_entry rer;	// Values 'rer' will write into servers RAMs (blockset info + locks) according to his topo.
	} lid;
	int jour_offset[N_MAX_RAID_SLICE_LEN];  // Journal start offset from the beginnig of the entry -1 for no txbm on this entry,  i -> role i (relevant for MS)
	struct nvmibc_tx_candidate jtx;			// Transaction's journal candidate. It is relative to seg[i] in praid and NOT from slice start
	struct t_slice_tx_ptrs tpd;				// Pointers to inject transaction and verify data was fixed

	// ------------ Testing the flow of code (not data correctness)
	struct t_tx_flow {
		u32 any_j2d;					// Bitmap, Which roles in this blockset has at least 1 journal candidate with j2d
		u32 htr_process_me : 1;				// Boolean: Cold recovery only, will cold find this tx as candidate and execute HTR on it
		u32 jgc_free_me    : 1;				// Boolean: JGC recovery only, will it free me
	} flow;
};

void jdr_write_ec_recov_tx(struct jdr* jdr, char const * name, struct t_ec_recov_tx const* rtx);

/*************** Representation of Single Blockset with only staff that mutual to all TXs in Blockset ****************/
/* Bitmaps: all include parities and i'th elem is NOT seg[i], but relative to slice start! */
/* LockBitmaps: include at most 3 tunred on bits according to lock scheme (0, p, q) */
struct t_ec_recov_blkset {			// Parameters for a single IO trasnaction injected for recoveries to solve
	struct t_ec_recov_tx tx[2];						  // At most N prev tx candidates
	u32 will_htr_sync_called_on_blockset : 1;         // Bool: htr will be called on blockset iff journal is not overriden and there is no stale lock in binfo.
	u32 will_nwhole_sync_called_on_blockset : 1;      // Bool: Old Txs might be affected by last tx nwhole sync which affects all blockset.
	struct t_blkset_resolv {
		u32 turnon_all_deg_segs : 1;				  // Bool
		u32 after_resolve_dbits : 16;                 // Bitmap
	} resolve_dbits;
	struct t_blkset_nwhole {
		u32 turnoff_dbits_bmp : 16; 				  // Bitmap: which roles nwhole sync will fix because of turnoff dbits in blockset granularity.
		u32 will_turnoff_dbits : 1; 				  // Bool: will nwhole sync turnoff dbits in blockset granularity.
		u32 will_turnon_dbits : 1;					  // Bool: will nwhole sync turnon dbits in blockset granularity.
		u32 regen_bkw_bmp : 16;                       // Bitmap : which roles nwhole fix because of regen backwards.
		u32 is_sl_by_sl : 1;                          // Bool: will the nwhole operate in slice by slice mode or full blockset?l
		u32 bad_sec_bmp;        // Bitmap: on which segs there are bad sectors in blockset granularity.
		u32 ram_dbits_turnon_on_turnoff: 16; //Bitmap: Which roles will be dbits turnon by no_write_hole sync (Dead parities in case of turnoff)
	} nwhole;
	struct t_blkset_bentry {
		union nvmeib_lock_blkset_entry post_recov; 		  // State of RAM after the recovery.
	} lid;
	struct t_blkset_tx {
		bool is_wraparound;							// Is the write tx cause an wraparound (can be true only in HTR tests)
	} last_tx;
	u32 is_unknown_txid_injected_to_ram : 1;
	u32 is_unknown_dbits_injected_to_ram : 1;
	u32 is_double_deg_and_deg_parity : 1;
};

void jdr_write_ec_recov_blkset(struct jdr* jdr, char const * name, struct t_ec_recov_blkset const* rblkset);

struct t_ec_tx_history {					// History of transactions, Todo, make it larger
	u32 msn;
	struct t_ec_recov_iteration {                      // For easy reproduction and debugging of problems: the iteration in which the test is.
		u64 curr_permutation;                   // Index of the topo enumerator.
		u64 iner_iteration;                     // For each topo in which inner iteration we are.
		u64 n_recov_retries;                    // Relevant only for Cold and JGC test - indicates the number of recovery retries until success when transport errors injected..
	} iter;
	struct nvmeibc_raid_topo_persistent *hdr; // persistent state of raid per client.
	struct t_ec_recov_properties {
		bool is_trans_errors_inject_enabled;	// Is transport error injections enabled?
		u32 n_tx_in_test : 4;				    // Num of transactions injected in test, up to 8 (2 per blockset)
		u32 n_blksets :	4;					    // Num of blocksets participate in test each with the same amount of txs
		u32 n_tx_in_blkset :	4;				// Num of TXs for each blockset.
		struct {								// Type of the recovery to run on the history
			enum NVMEIBT_RECOVERY_TYPE type;	// The recovery which is executed on the history
			u32 seg_start : 4;					// Launch recovery on range of segments [start..end)
			u32 seg_end   : 4;					// Default is: All segments for JGC, single segment in cold/HTR
		} rec;
	} prop;
	struct t_ec_recov_blkset blkst[8];		// At most N blocksets

	struct post_recovery_conclusions {   			// Conclusions figured only after the recovery is finished, because they can't be concluded before cause of the rendomization caused by err_injection.
		bool is_unexpected_htr_executed;			// Is unexpected htr was being executed - Currently can only be in HTR test cause of write operation which failed and left stale-lock so her resub will call htr on it.
	} prc;
	struct nvmeibc_disk_hooks disk_hooks;
};

void jdr_write_ec_tx_history(struct jdr* jdr, char const *name, struct t_ec_tx_history const* hist);

void ec_tx_boomtrah(struct t_ec_tx_history* hist, char const *file, char const* function, int line, char const * condition);


enum ec_tx_last_writer {					// What was the source of the data we expect to find on disk (what user will receive upon read)
	ec_tx_last_writer_predata = 1,			// Existed before 'ree' & 'rer'
	ec_tx_last_writer_ree = 2,				// Created by 'ree'. Either written explicitly by 'ree' or fixed by 'rer' to represent what 'ree' wanted to write
	ec_tx_last_writer_data_loss = 3,		// This data was permanently loss as a result of natural disaster + 'rer's attempts to fix the slice
	ec_tx_last_writer_rer = 4				// Created by 'rer'. IO that 'rer' did after finishing recovery
};

enum ec_tx_last_writer
ec_tx_get_last_succeful_writter(const struct t_ec_recov_tx *p, raid_role_t role, u32 h);

union io_perms_bitfield ec_tx_convert_recov_type_to_io_perms(const enum NVMEIBT_RECOVERY_TYPE type);
//partially initializes t_ec_recov_tx.inp struct; INVALID recovery types means that developer is going to READ/WRITE data
void ec_tx_init(struct TstPRaid sraid, struct t_ec_recov_blkset *containing_blkset, struct t_ec_recov_tx* p, enum NVMEIBT_RECOVERY_TYPE type);
void ec_tx_free_bio_ptrs(struct t_ec_recov_tx* p);
/* Initialize pointers and references for current transaction server side only
 * candidate_location may be null; if passed should have "replicas" items
 */
void ex_tx_inject_tx(struct NVMeshSystem *sys, struct t_ec_recov_tx *p);
void ec_tx_set_and_switch_rer_topo(struct NVMeshSystem *sys, struct t_ec_recov_tx *p, bool set);
void ec_tx_verify_post_tx_ssd_disks_roll_fwd_dbits_fixup(struct t_ec_recov_tx *p, bool is_trans_errors_inject_enabled);
void ec_tx_verify_post_sync_tx(struct NVMeshSystem *sys, struct t_ec_recov_tx *p, bool is_trans_errors_inject_enabled, bool could_unexpected_htr_executed);
void ec_tx_init_blkset(struct t_ec_recov_blkset *blkset);
void ec_tx_for_jmdc_do(struct NVMeshSystem *sys, struct t_ec_recov_tx *p, char *action);
u32 ec_tx_gen_next_txid(void);
