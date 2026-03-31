/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_DP_DBITS_H_
#define NVMEIBC_BLOCK_DP_DBITS_H_
#include "nvmeib_shared.h"
#include "block/dp_topology_traits.h"

/************************** nvmeibc_dbits_entry API ***************************/
// Below is a set of usefull manipulations on entry (via predefined internal actions)
__attribute__((nonnull(1, 2)))
sgmnts_bmp_t  nvmeibc_dbits_get_turn_on_bmp( const union nvmeibc_dbits_entry *e, struct dp_topology_traits const* topo_traits);	    // Get bitmap of dirty segs
__attribute__((nonnull(1, 2)))
u32  nvmeibc_dbits_get_n_unk(       const union nvmeibc_dbits_entry *e, struct dp_topology_traits const* topo_traits);	    // Get amount of unknown dbits
__attribute__((nonnull(1, 2)))
void nvmeibc_dbits_del_unk(               union nvmeibc_dbits_entry *e, struct dp_topology_traits const* topo_traits);	    // Remove unknowns, used when we resolve unk from md or topology
__attribute__((nonnull(1, 2)))
void nvmeibc_dbits_del_unk_worst_case(               union nvmeibc_dbits_entry *e, struct dp_topology_traits const* topo_traits);	    // Remove unknowns, used when we resolve unk from md or topology
__attribute__((nonnull(1, 2)))
void nvmeibc_dbits_turn_on_convict(       union nvmeibc_dbits_entry *e, struct dp_topology_traits const* topo_traits);

/* Given the dirtybits that read by owner and secondary owner lock, unite them and return the result.all_bits */
__attribute__((nonnull(1, 2, 3)))
u16 nvmeibc_dbits_merge_owners(    const union nvmeibc_dbits_entry *e1, const union nvmeibc_dbits_entry *e2, struct dp_topology_traits const* topo_traits);		// Take worst  problem (unite all problems)
__attribute__((nonnull(1, 2, 3)))
u16 nvmeibc_dbits_intersect_owners(const union nvmeibc_dbits_entry *e1, const union nvmeibc_dbits_entry *e2, struct dp_topology_traits const* topo_traits);		// Take lesser problem (intersect all problems)

/********************** Dirty-bits Transaction actions ************************/
// Below is a generic struct to create any possible actions
struct nvmeibc_dbits_action {		// Bitmaps representation of dbit action, Can represent unpacked 'nvmeibc_dbits_entry' or any transaction (io, sync, etc)
	union {
		struct {
			u16 db_turn_on_bmp;         	// Bit map which segments could not be written  (for dead seg 'i' bit 'i' is turned on).
			u16 db_turn_off_bmp;        	// Bit map which segments are implicitly synced (for W    seg 'i' bit 'i' is turned off).
			u16 db_conv_map;          		// Bit map of which degraded segments are also convicts.
			u16 num_unknowns  :  4;			// Number of unknown dbits: Raid6-{0,1,2}, must be 3 bits for summation (2+2). 4 bits for summation of 2 r1 5 mirror
			u16 num_degraded  :  3;			// Maximum number of valid dbits (We currently support only 2, made for up to 4 degraded in 5 mirror r1)
			u16 has_slice_info:  1;			// true (1) - has information about dirty slices, false - must be represented as global for blockset
			u16 reserved      :  8;
		};
		u64 raw;							// For prints and comparison
	} __attribute__ ((packed));
};

/* IO steps:
	1. Get pre-tx dbits:
		1.1. Read all copies of pre-tx nvmeibc_dbits_entry
    	1.2. merge them to get a single pre-tx result. Merging is converting
    		 entries to actions and merging the actions.
    2. Construct dbits action, due to TX (IO, sync, etc)
	    2.1 Have 2 options: Construct by commands IO cmds or by IO type and rlba
	3. Merge pre-tx with action. Result is post-tx */

struct nvmeibc_dbits_tx {				// Map describing the dirtybits of transaction. Relevant only in degraded mode
	struct nvmeibc_dbits_action action;	// Input: Dirtybits turned on/off as a result of this IO/Sync.
	union nvmeibc_dbits_entry post;		// Output: Final dirty marker to write (in EC or N-replica (N>2) is not calculable before old (pre-io) value is read and merged with action;
} __attribute__ ((packed));
/* Constructors of dbits tx action have a few variants, because we have a few
   IO types (BIO, Sync, ...):
   0. Private constructors: convert nvmeibc_dbits_entry <--> nvmeibc_dbits_action
      Used to access pre-tx as action and export post-tx result as entry
   0. Support actions: clean unknowns / non convicts from dbit marker
   1. Empty constructor, when no action on dbits is performed
   2. Input: topology iterator (during prepare IO, when IO cmds dont exist yet)
    	Used for Raid1s (N-replica, non embedded dirtybits).
		Exists for backwords compatibility. Todo: Remove and use rldr instead.
   3. Input: raid leader cmds (during execute stage). Used for Raid5/6
    	(embedded dirtybits). Cmds of BIO can turn on+off, cmds of dbits sync
		can only turn-off, and others can only turn on.
   4. Input: Specific TxBM (with parities and in seg indices).
    	Example: Sync rolls-fwd a transaction
   5. Input: None, Turn on convicts */
/* WARNING: Using Turn-on and Turn-off in a single action (write) is data
   corruption for N replica! In EC (journal saves us) and 2-Miror cannot
   turn on & off in a single write. */
// Private: void nvmeibc_dbits_action_init_by_entry( struct nvmeibc_dbits_action *act,const union nvmeibc_dbits_entry *e);
// Private: void nvmeibc_dbits_action_to_entry(const struct nvmeibc_dbits_action *act,      union nvmeibc_dbits_entry *e);
__attribute__((nonnull(1, 2)))
void nvmeibc_dbits_tx_init_empty(     struct nvmeibc_dbits_tx*, struct dp_topology_traits const* topo_traits);
__attribute__((nonnull(1, 2)))
void nvmeibc_dbits_tx_init_by_bmp(    struct nvmeibc_dbits_tx*, struct dp_topology_traits const* topo_traits, u32 turn_on_dbit_bmp, u32 turn_off_dbit_bmp, u32 turn_on_conv_bmp);

static inline bool nvmeibc_dbits_tx_has_action(const struct nvmeibc_dbits_tx* tx)
{	// At least 1 segment changed
	return (tx->action.db_turn_on_bmp | tx->action.db_turn_off_bmp);
}

/* tx->post = apply tx->action on pre_tx_dbit. Also return tx->post */
u32 nvmeibc_dbits_tx_apply(const union nvmeibc_dbits_entry *pre_tx_dbit,
									         struct nvmeibc_dbits_tx *tx);
static inline bool nvmeibc_dbits_tx_are_equal(const struct nvmeibc_dbits_tx* a, const struct nvmeibc_dbits_tx* b)
{
	return  (a->post.all_bits == b->post.all_bits) &&
			(a->action.raw    == b->action.raw);
}

#define nvmeibc_dbits_has_turn_on( dbmap) ((dbmap)->action.db_turn_on_bmp)
#define nvmeibc_dbits_has_turn_off(dbmap) ((dbmap)->action.db_turn_off_bmp)
#define nvmeibc_dbits_has_convict( dbmap) ((dbmap)->action.db_conv_map)
#define nvmeibc_dbits_has_unknown( dbmap) ((dbmap)->action.num_unknowns)

#endif /* NVMEIBC_BLOCK_DP_DBITS_H_ */
