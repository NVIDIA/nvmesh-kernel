/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmesh_sim.h"
#include "uni_scenario_ec.h"
#include "../nvmeibc_block_common.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "../datapath_ec/nvmeibc_block_dp_ec_reed_solomon.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.h"
#include "../datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.h"
#include "uni_scenario_tx_history_ec.h"
#include "uni_framework/range_algorithms.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_hot.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold.h"
#include "uni_random.h"
#include "uni_enumerators.h"
#include "nvmeibc_jam.h"
#include "../datapath_ec/nvmeibc_block_dp_ec.h"
#include "uni_scenarios/uni_scenario_common.h"
#include "nvmeib_jdr.h"
#include "block/controlpath/nvmeibc_b_cp_blkset_topo.h"

/* Daniel: Remaining stuff to verify
    *. JAM Ambiguity
    *. pre_tx_txid: -5 is voodo, must be less than the first Tx in history to blockset but > then max Tx in prev history
    *. If: ec_tx_init(..., NVMEIBT_RECOVERY_TYPE_INVALID); dont inject stale lock and not HTR will be executed
    *. Advance TxID by 1 for each blockset, not for each tX in idfferent blocksets */
/*****************************************************************************/
static u32 history_txid_gen = 0x1000;
static int jentry_in_jri_seed = 0;						// Pseudo random
struct t_ec_tx_history *_hist;					// Static var for easy access in any function
struct t_ec_recov_tx *_p;								// Same as above

void ec_tx_boomtrah(struct t_ec_tx_history* hist, char const *file, char const* function, int line, char const * condition)
{
	struct charvec buffer = {.base = malloc(4*1024*1024), .len = 4*1024*1024};
	struct jdr jdr = jdr_make(buffer);
	struct charvec json = {0};
	jdr_write_ec_tx_history(&jdr, "history", hist);
	jdr_write_var(&jdr, file, file);
	jdr_write_var(&jdr, function, function);
	jdr_write_var(&jdr, line, line);
	jdr_write_var(&jdr, condition, condition);
	json = jdr_finalize(&jdr);
	printk("%s", json.base);
	free(buffer.base);
}

#define EC_TX_BUG_ON(condition) 														\
({																									\
	if ( (condition) ){																				\
		ec_tx_boomtrah(_hist, __builtin_FILE(), __builtin_FUNCTION(), __builtin_LINE(), #condition);	\
 		BUG();																						\
	}																								\
})

void __reed_solo_fill_missing(const struct test_context env, struct t_slice_data* sdata, const u64 slba, const u16 sidx) {
	u32 _crc[N_MAX_RAID_SLICE_LEN];
	u32 *crc[N_MAX_RAID_SLICE_LEN];
	const struct nvmeibc_datapath *dp = &env.dev->dp;
	const u16 replicas = env.sraid.cpr->replicas;
	const u16 slice_size = env.sraid.cpr->slice_size, snake_size = dp->p.snake_size;

	for (raid_role_t i = 0; i < replicas; ++i)
		crc[i] = &_crc[i];

	EC_TX_BUG_ON(nvmeibc_reed_solomon_fill_missing(replicas, slice_size, snake_size, (void*)sdata->blocks[sidx], NULL, crc,
		GENMASK(slice_size - 1, 0), nvmeibc_raid1_get_parities_bmp(env.sraid.cpr), GENMASK(replicas-1, 0), (slba + sidx) * slice_size, dp->enable_di_debug_mode));

	for (raid_role_t i = 0; i < replicas; ++i)
		nvmeibc_block_dp_ec_md_set_edic(&sdata->md[sidx][i], _crc[i], (i >= slice_size));
}

void allocate_tx_blocks(struct t_ec_recov_tx* p, struct t_slice_data* inj, bool should_zero) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	u8 *blob = sim_kmalloc(NVMEIBC_SECTOR_SIZE * pr->replicas * p->inp.tx_height, GFP_KERNEL | (should_zero ? __GFP_ZERO : 0));
	u32 i, j;
	for (i = 0; i < pr->replicas; i++)
		for (j = 0; j < p->inp.tx_height; j++)
			inj->blocks[j][i] = (void*)&blob[((j * pr->replicas) + i) * NVMEIBC_SECTOR_SIZE];
}

/* Init bio + sratch buffers, Generate data and calc parity */
void __init_ree_slice_blocks(struct test_context env, struct t_ec_recov_tx* p, struct t_slice_data* inj, const int slba){
	u32 i, j, bit;
	const struct disk_range *pr = env.sraid.cpr;

	for (i = 0; i < p->inp.tx_height; i++) {
		if (!p->inp.ree.tx_bm[i]) continue;

		for (j = 0, bit = 1; j < pr->slice_size; j++, bit = (1 << j)) {						// Generate data
			if ((p->inp.ree.tx_bm[i] & bit) && p->rer_bmp.topo.has_rw_pari)
				__unitest_fill_blocks_unique_pattern(inj->blocks[i][j], 1);
			else // data stays the same as after nat
				memcpy(inj->blocks[i][j], p->tpd.pre.blocks[i][j], NVMEIBC_SECTOR_SIZE); // TODO: use copy_blocks
		}
		__reed_solo_fill_missing(env, inj, slba, i);
	}
}

/* Init bio + sratch buffers, Generate data and calc parity */
void __init_rer_slice_blocks(struct test_context env, struct t_ec_recov_tx* p, struct t_slice_data* inj, const int slba) {
	u32 i, j, bit;
	u64 *src;
	const struct disk_range *pr = env.sraid.cpr;

	for (i = 0; i < p->inp.tx_height; i++) {
		for (j = 0, bit = 1; j < pr->slice_size; j++, bit = (1 << j)) {	 // Generate data
			if (p->inp.rer.tx_bm[i] & bit) {
				__unitest_fill_blocks_unique_pattern(inj->blocks[i][j], 1);
			} else {
				if ((p->rer_bmp.dmd_kosher[i]) || (!p->rer_bmp.topo.has_rw_pari))  // roll-forward or  data stays the same as after nat
					src = p->tpd.ree.blocks[i][j];
				else
					src = p->tpd.pre.blocks[i][j];
				memcpy(inj->blocks[i][j], src, NVMEIBC_SECTOR_SIZE);
			}
		}
		__reed_solo_fill_missing(env, inj, slba, i);
	}
}

/* Init bio + sratch buffers, Generate data and calc parity */
void __init_pre_slice_blocks(struct t_ec_recov_tx* p, struct test_context env, struct t_slice_data* inj, const int slba){
	u32 i, j;
	const struct disk_range *pr = env.sraid.cpr;

	for (i = 0; i < p->inp.tx_height; i++) {
		if (p->inp.pre.is_never_written[i]) {  // Slice already zeroized when alloced.
			for (j = 0; j < pr->replicas; j++) {			// Generate data
				u32 never_written_type;
				if (j >= pr->slice_size && p->inp.pre.is_parity_explicitly_marked_neverwritten[i]) {
					never_written_type = 0;
				} else {
					never_written_type = rand() % 3;
				}
				inj->md[i][j].raw = 0; // TODO: (((u64)rand() << 32) | (u64)rand());  // write garbage to md cause we should ignore garbage fields when md never-written.
				switch (never_written_type) {
				case 0: nbdpec_md_mark_data_never_written_no_dbits(&inj->md[i][j], (i >= pr->slice_size)); break;  //our neverwritten.
				case 1: inj->md[i][j].raw = nvmeibc_ec_unwritten_md_entry_val.raw; break;
				case 2: inj->md[i][j].raw = 0; break;
				default: BUG();
				}
			}
			continue;
		}

		for (j = 0; j < pr->slice_size; j++)						// Generate data
			__unitest_fill_blocks_unique_pattern(inj->blocks[i][j], 1);

		__reed_solo_fill_missing(env, inj, slba, i);
	}
}

static void ec_tx_init_bio_ptrs(struct test_context env, struct t_ec_recov_tx* p) {
	const u32 replicas = p->inp.sraid.cpr->replicas;
	const u32 slice_size = p->inp.sraid.cpr->slice_size;
	u32 i, j;
	allocate_tx_blocks(p, &p->tpd.pre, true);
	allocate_tx_blocks(p, &p->tpd.ree, false);
	allocate_tx_blocks(p, &p->tpd.nat, false);
	allocate_tx_blocks(p, &p->tpd.rer, false);
	__init_pre_slice_blocks(p, env, &p->tpd.pre, p->inp.slba);
	__init_ree_slice_blocks(env, p, &p->tpd.ree, p->inp.slba);
	__init_rer_slice_blocks(env, p, &p->tpd.rer, p->inp.slba);
	memcpy(p->tpd.nat.blocks[0][0], p->tpd.ree.blocks[0][0], NVMEIBC_SECTOR_SIZE * replicas * p->inp.tx_height);
	for (i = slice_size; i < replicas; ++i) {
		for (j = 0; j < p->inp.tx_height; j++)
			p->tpd.exp.blocks[j][i] = NULL;
	}
}

void ec_tx_free_bio_ptrs(struct t_ec_recov_tx* p) {
	u32 i;
	sim_kfree(p->tpd.pre.blocks[0][0]);
	sim_kfree(p->tpd.ree.blocks[0][0]);
	sim_kfree(p->tpd.nat.blocks[0][0]);
	sim_kfree(p->tpd.rer.blocks[0][0]);
	for (i = 0; i < p->inp.tx_height; i++) {
		array_fill(p->tpd.pre.blocks[i], NULL);
		array_fill(p->tpd.ree.blocks[i], NULL);
		array_fill(p->tpd.nat.blocks[i], NULL);
		array_fill(p->tpd.rer.blocks[i], NULL);
		array_fill(p->tpd.exp.blocks[i], NULL);
	}
}

static void ec_tx_init_injection_ptrs(struct test_context env, struct t_ec_recov_tx* p){
	const u64 slba = p->inp.slba;
	struct t_slice_tx_ptrs *inj = &p->tpd;
	struct candidate_location* cand = p->jtx.locations;
	const struct disk_range *pr = env.sraid.cpr;
	const u64 lockset_owner_seg = (slba >> LOCKSET_SHIFT) % pr->replicas;
	u32 i, j;
	for (i = 0; i < pr->replicas; i++) {
		const raid_sgmnt_t di = (i + lockset_owner_seg) % pr->replicas;		// Convert role to seg
		struct serverSimulator *srv = &env.sys->servers[di];
		for (j = 0; j < p->inp.tx_height; j++) {  // LKJ ofir: this loop should be changed when transport and jam support MS.
			const u64 dlba = slba + pr[di].dlba_start + j;
			inj->bptrs[j][i] = serverSimulator_get_block_inject_ptrs(srv, dlba, cand ? cand[di].jri : -1, cand ? cand[di].jentry : -1, j);
		}
	}
}

#define __get_slice_start_seg(p) (((p)->inp.slba >> LOCKSET_SHIFT) % (p)->inp.sraid.cpr->replicas)		// Note: (p)->slba == (p)->jtx.b.j2slba

//we are digging in reverse order - starting from now and going into the past
enum ec_tx_last_writer ec_tx_get_last_succeful_writter(const struct t_ec_recov_tx *p, raid_role_t role, u32 h){
	const roles_bmp_t rb = (1U << role);
	const bool old_completed_unchanged = (p->inp.ree.is_old_completed && p->rer_bmp.topo.has_rw_pari);  // Consts, can never be touched
	const bool will_touch_me = (((p->flow.htr_process_me) && (p->rer_bmp.total.slice_data_touched[h] & rb) && (!old_completed_unchanged)) || (p->rer_bmp.tx.syn_blkst_data_change[h] & rb));
	if ((p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE) && (h < p->inp.rer.tx_height) && ((p->inp.rer.tx_bm[h] & (~p->rer_bmp.topo.dead)) & rb)) {    // Did 'rer' managed to overwrite 'ree' + 'pre' ?
		return ec_tx_last_writer_rer;
	} else if ((p->rer_bmp.bad_sec_bmp[h]) & (~p->rer_bmp.tx.fixed_bad_sec[h]) & rb) {
		return ec_tx_last_writer_data_loss;
	} else if (will_touch_me) {
		if (p->rer_bmp.dmd_kosher[h] && (p->inp.ree.tx_bm[h] & rb))
			return ec_tx_last_writer_ree;							// Roll-fwd by rer or already written by ree
		else
			return ec_tx_last_writer_predata;                       // Roll backwards to 'pre' or 'ree' did not write to this block
	} else { // Else: Data remains as injected
		return (p->ree_bmp.dmd_writn[h] & rb) ? ec_tx_last_writer_ree  : ec_tx_last_writer_predata;
	}
}

static inline bool nvmeibc_is_roles_bmps_valid(struct nvmeibc_roles_bmps bmps){
	return bmps.raid.data
		   && ((bmps.rw + bmps.wp + bmps.w + bmps.dead) == (bmps.rw | bmps.wp | bmps.w | bmps.dead))
		   && ((bmps.rw | bmps.wp | bmps.w | bmps.dead) == (bmps.raid.data|bmps.raid.pari));
}

static void ec_tx_gen_rer_and_ree_topo_bmps(struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	struct t_tx_recoveree_bmps *ree_bmp = &p->ree_bmp;
	struct t_tx_recoverer_bmps *rer_bmp = &p->rer_bmp;

	rer_bmp->topo = nvmeibc_sim_init_roles_bmps(pr, p->inp.rer.topo, 0);
	ree_bmp->topo = rer_bmp->topo;				// By default rer & ree topos are identical

	// -- Generate Random Topo for recoveree, by altering rers topo in valid ways
	if (p->inp.ree.is_topo_not_eq_to_rer) {		// Todo: Mutate topology in TXBM segs
		// 1. 'W' rer could be 50% 'D', 50% 'W' in ree (with possible dbits).
		const u32 W2D  = (rer_bmp->topo.w & random_bitmap(pr));

		// 2. 'W+' rer could be 50% 'W', 50% 'W+' in ree.
		const u32 WP2W  = (rer_bmp->topo.wp & random_bitmap(pr));

		// 3. TXBM: 'D' in rer could be 'D/W' in ree (with possible dbit) or 'W+/RW' wihtout dbits. Ouitside of TxBM we dont care
		const u32 D2L  = rer_bmp->topo.dead & random_bitmap(pr);	// Half of rer 'D' become {W/RW} in rer.
		const u32 D2W  = (D2L & random_bitmap(pr));					// Half of the above are 'W' half 'RW/W+'
		const u32 D2RW = (D2L & (~D2W) & random_bitmap(pr));
		const u32 D2WP = (D2L & (~D2W) & (~D2RW));

		// 4. TXBM 'RW' in rer could become 'W/W+' in ree only if amount of non RW does not exceed protection level
		const u32 n_rw_can_kill = hweight32(rer_bmp->topo.rw | D2RW) - pr->slice_size;
		const u32 RW2L_mask = (n_rw_can_kill ? __rand_n_bits_bitmap(n_rw_can_kill, pr->replicas) : 0);	// Todo: Optimize, choose only from RW BMP
		const u32 RW2W = (rer_bmp->topo.rw & RW2L_mask & random_bitmap(pr)); // Half of the above are 'W' half 'W+'
		const u32 RW2WP = (rer_bmp->topo.rw & RW2L_mask & (~RW2W));

		// Fill the topo
		ree_bmp->topo.rw =  (rer_bmp->topo.rw  | D2RW)       & (~RW2W) & (~RW2WP);
		ree_bmp->topo.w =   (rer_bmp->topo.w   | D2W | RW2W | WP2W) & (~W2D);
		ree_bmp->topo.dead =(rer_bmp->topo.dead| W2D)        & (~D2L);
		ree_bmp->topo.wp = (rer_bmp->topo.wp | RW2WP | D2WP) & (~WP2W);
		ree_bmp->topo.readable = ree_bmp->topo.wp | ree_bmp->topo.rw;

		ree_bmp->topo.raid.data = nvmeibc_raid1_get_data_bmp(pr);
		ree_bmp->topo.raid.pari = nvmeibc_raid1_get_parities_bmp(pr);
		ree_bmp->topo.raid.all = nvmeibc_raid1_get_all_bmp(pr);

		nvmeibc_update_roles_bmps_flags(&ree_bmp->topo);

		WARN(!nvmeibc_is_roles_bmps_valid(ree_bmp->topo), "Unitest bug\n");
	}
}

#define is_writable_pari(bit) ((i >= slice_size) && (bit & (~p->rer_bmp.topo.dead)))

static int __find_rw_parity_with_lowest_seg_ind(struct t_ec_recov_tx *p) {	// Much like HTR's algorithm for selecting the pivot
	const struct disk_range *pr = p->inp.sraid.cpr;
	const unsigned long rw_parities = (p->rer_bmp.topo.readable & p->rer_bmp.topo.raid.pari);
	const u32 n_segs = pr->replicas, sstart = __get_slice_start_seg(p);
	if (sstart == 0) {
		return (int)find_first_bit((void*)&rw_parities, n_segs);
	} else {	// Rotate to be relative to praid and not slice start, find and rotate back
		const ulong rw_parities_seg = rol32_width(rw_parities, sstart, n_segs);
		const int res_seg = (int)find_first_bit(&rw_parities_seg, n_segs);
		return ((res_seg + n_segs) - sstart) % n_segs;
	}
}

// generate random bitmap of which data writes failed, such that 'rer' will always see at least 1 data
static u32 __gen_partial_non_empty_data_bmp(struct t_ec_recov_tx *p, u32 h) {
	const u32 rer_rw = (p->ree_bmp.dmd_writn[h] & p->rer_bmp.topo.readable);	// Max of what rer can see
	u32 rnd = random_bitmap(p->inp.sraid.cpr);
	if ((rer_rw & rnd) == 0)			// Illegal bitmap, removes all data, its inverse will leave at least 1 data
		rnd = ~rnd;
	return rnd;
}

static inline bool is_array_all_zeros(u32 arr[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY]) {
	for (u32 i=0; i<NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY; ++i) {
		if (arr[i])
			return false;
	}

	return true;
}

static void ec_tx_sync_apply_abrt_stage_to_txbm(struct t_ec_recov_tx *p) {
	u32 i, h;
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 locks_bmp = 1 | p->ree_bmp.topo.raid.pari; //hard coding D0, P & Q; the real code uses lock map
	struct t_tx_recoveree_bmps *ree_bmp = &p->ree_bmp;
	struct t_tx_recoverer_bmps *rer_bmp = &p->rer_bmp;

	// ------------- Calculate recoveree bmps -----------------
	ree_bmp->is_jour_committed = true;
	ree_bmp->jmd_writn_union = 0;
	for (i = 0; i < p->inp.tx_height; i++) {
		const u32 txbmp_written = ((~ree_bmp->topo.dead) & p->inp.ree.tx_bm[i]);	// Default: Transcation succeeded, and is fully commited
		ree_bmp->jmd_writn[i] = ree_bmp->dmd_writn[i] = txbmp_written;

		switch (p->inp.ree.aband_stg) {
			case ec_tx_abort_stage_success:			break;
			case ec_tx_abort_stage_partial_data:	ree_bmp->dmd_writn[i] &= __gen_partial_non_empty_data_bmp(p, i); EC_TX_BUG_ON(ree_bmp->dmd_writn == 0); break;
			case ec_tx_abort_stage_partial_journal:	ree_bmp->jmd_writn[i] &= (rand()%10 < 8) ? all_segs_bm(pr) : random_bitmap(pr);	FALLTHRU;	// Same as _nodata
			case ec_tx_abort_stage_nodata:			ree_bmp->dmd_writn[i] = 0;					break;
			default:								BUG();								break;
		}
		WARN(!__is_bmp_included_in(ree_bmp->jmd_writn[i], txbmp_written     ), "Unitest bug, Not all journal included in TxBM\n");
		WARN(!__is_bmp_included_in(ree_bmp->dmd_writn[i], ree_bmp->jmd_writn[i]), "Unitest bug, Not all data included in journal\n");
		ree_bmp->is_jour_committed &= (txbmp_written == ree_bmp->jmd_writn[i]);
		ree_bmp->jmd_writn_union |= ree_bmp->jmd_writn[i];
	}


	// If Some piggybacks of journal/data cmds failed, randomly select which ones failed
	p->ree_bmp.ram_txid_no_wr = 0;
	p->ree_bmp.ram_dbits_no_wr = 0;
	if (!p->ree_bmp.is_jour_committed) {        // We ommit the case when all jmd written and txid piggybacks not because it's not interseting enough
		p->ree_bmp.ram_txid_no_wr = (locks_bmp & random_bitmap(pr));	// Piggybacks of journal failure bmp
		p->ree_bmp.ram_dbits_no_wr = locks_bmp;
	} else if (p->inp.ree.aband_stg != ec_tx_abort_stage_success) {
		p->ree_bmp.ram_dbits_no_wr = (locks_bmp & random_bitmap(pr));    // Piggybacks of data failure bmp
	}

	ree_bmp->jmd_with_valid_chain = all_segs_bm(pr);
	ree_bmp->jmd_with_fake_chain = 0;
	for (h = 0; h < p->inp.tx_height; h++) {
		if (p->inp.nat.is_journal_overriden) {
			ree_bmp->jmd_with_valid_chain = 0;
		} else {
			ree_bmp->jmd_with_valid_chain &= (ree_bmp->jmd_writn[h] | (ree_bmp->jmd_writn_union & (~p->inp.ree.tx_bm[h])));
			if (h == (p->inp.tx_height - 1)) {
				for (i = 0; i < pr->replicas; i++) {
					if ((p->jour_offset[i] + 1) == (int)p->inp.tx_height) {
						ree_bmp->jmd_with_valid_chain |= (ree_bmp->jmd_writn[p->jour_offset[i]]) & (1 << i); // if only the first block is written than it also going to form a valid chain
					}
					ree_bmp->jmd_with_fake_chain |= (random_bitmap(pr) & (~ree_bmp->jmd_writn_union) & (~ree_bmp->jmd_with_valid_chain));  // add fake chains where there is no jmd writn.
					ree_bmp->jmd_with_valid_chain |= ree_bmp->jmd_with_fake_chain;
				}
			}
		}
	}

	// -------------- Old journal pre transaction, random missleading values, sometimes correct j2d somtimes not, Regardless of topology
	ree_bmp->abandoned_jent = 0;
	ree_bmp->jmd_with_correct_j2d_union = 0;
	for (h = 0; h < p->inp.tx_height; h++) {
		if (p->inp.nat.is_journal_overriden) {
			ree_bmp->jmd_with_correct_j2d[h] = 0; /* LKJ: should be random_bitmap(pr), but for now we can't have journal with j2d to the praid and not abandoned
	because cold don't know which entries are dirty (just jris) and then he will tell serjio to clean the not abandoned but with j2d, which will lead to a bug_on in JAM */
			ree_bmp->abandoned_jent = 0;
		} else {
			ree_bmp->jmd_with_correct_j2d[h] = ree_bmp->jmd_with_valid_chain | ree_bmp->jmd_writn[h];
			if ((p->jour_offset[i] == -1) && (h > 0)) {  // Only where chain is valid we want that there will be a candidate
				ree_bmp->jmd_with_correct_j2d[h] |= random_bitmap(pr) & (~ree_bmp->jmd_writn_union);
			}
			ree_bmp->abandoned_jent |= ree_bmp->jmd_with_correct_j2d[h];
		}

		ree_bmp->jmd_with_correct_j2d_union |= ree_bmp->jmd_with_correct_j2d[h];
	}

	for (i = 0; i < pr->replicas; i++) {
		if (!(ree_bmp->jmd_with_valid_chain & (1<<i))) {
			ree_bmp->chain_height[i] = 0;
			continue;
		}

		for (h = 0; h < p->inp.tx_height; h++) {
			if (!!(ree_bmp->jmd_writn[h] & (1<<i))) {
				int h_minus_offset = h;
				if (p->jour_offset[i] != -1 && (h >= (u32)(p->jour_offset[i]))) {
					h_minus_offset = h - p->jour_offset[i];
				} // else h_minus_offset = h
				ree_bmp->chain_height[i] = h_minus_offset;
			}
		}
	}


	// ------------- Calculate recoverers bmps -----------------
	rer_bmp->jmd_ready_union = 0;
	rer_bmp->any_dmd_kosher = false;
	rer_bmp->is_jour_committed = true;
	rer_bmp->jmd_kosher_union = 0;
	for (i = 0; i < p->inp.tx_height; i++) {
		if (!p->inp.nat.is_journal_overriden) {
			rer_bmp->jmd_ready[i] =  ree_bmp->jmd_writn[i] & (~rer_bmp->topo.dead);		// Accessible journals to recoveree
			rer_bmp->jmd_kosher[i] = ree_bmp->jmd_writn[i] & (rer_bmp->topo.readable);			// Trustowrty journals to recoveree
			rer_bmp->is_jour_committed &= (rer_bmp->jmd_kosher[i] == (p->inp.ree.tx_bm[i] & rer_bmp->topo.readable));
			rer_bmp->dmd_kosher[i] = ree_bmp->dmd_writn[i] & (rer_bmp->topo.readable);			// Trustowrty data to recoveree
			rer_bmp->any_dmd_kosher = rer_bmp->any_dmd_kosher || !!(rer_bmp->dmd_kosher[i]);
			if (p->inp.rer.io_perm.bits.is_cold_recovery && (!p->rer_bmp.topo.has_rw_pari)) { // starting from verion 1 we decided not to handle candidates when no rw parity since resolve dbits will either way will fix the write hole.
				rer_bmp->is_jour_committed = false;
			} else {
				WARN(ree_bmp->is_jour_committed > rer_bmp->is_jour_committed, "Unitest bug\n");	// If 'ree' did not fully commit but 'rer' cannot read uncommited segs then 'rer' thinks it is commited, but not the opposite!
				WARN(ree_bmp->dmd_writn[i]  && (!rer_bmp->is_jour_committed), "Unitest bug\n");	// Ree could not proceed to writing data before it is sure that rer will be aable to access journals
				WARN(rer_bmp->dmd_kosher[i] && (!rer_bmp->is_jour_committed), "Unitest bug\n");	// Rer could not see data if it does not see entire journal
			}
		} else {
			rer_bmp->jmd_ready[i] = rer_bmp->jmd_kosher[i] = 0;
			rer_bmp->is_jour_committed = false;
			rer_bmp->dmd_kosher[i] = ree_bmp->dmd_writn[i] & (rer_bmp->topo.readable);
			rer_bmp->any_dmd_kosher = rer_bmp->any_dmd_kosher || !!(rer_bmp->dmd_kosher[i]);
			WARN(ree_bmp->is_jour_committed < rer_bmp->is_jour_committed, "Unitest bug\n");
			WARN(ree_bmp->dmd_writn[i]  && (!ree_bmp->is_jour_committed), "Unitest bug\n");	// Ree could not proceed to writing data before it is sure that rer will be aable to access journals
			WARN(rer_bmp->dmd_kosher[i] && (!ree_bmp->is_jour_committed), "Unitest bug\n");	// Rer could not see data if it does not see entire journal
		}

		rer_bmp->jmd_ready_union |= rer_bmp->jmd_ready[i];
		rer_bmp->jmd_kosher_union |= rer_bmp->jmd_kosher[i];
	}

	rer_bmp->jmd_kosher_intersect = (~((u32)0)) & nvmeibc_raid1_get_all_bmp(pr);
	for (i = 0; i < p->inp.tx_height; i++) {
		rer_bmp->jmd_kosher_intersect &= rer_bmp->jmd_kosher_union & (~(rer_bmp->jmd_kosher[i] ^ p->inp.ree.tx_bm[i])) & nvmeibc_raid1_get_all_bmp(pr);
	}

	// can_see_ree_txid_in_ram ?
	if (p->inp.rer.io_perm.bits.is_cold_recovery) {  // In cold the RAM txid will be calculated from the dmd's txid
		p->rer_bmp.can_see_ree_txid_in_ram = (!is_array_all_zeros(rer_bmp->dmd_kosher));		// Rer can access at least 1 block of ree with max_txid so max accross entire blockset is ok
	}
	else if ((p->inp.rer.io_perm.bits.is_hot_recovery) && (p->ree_bmp.ram_txid_no_wr)) {   // If some piggybacks failed, check if all are inaccessible to 'rer'
		const u32 ree_failed = (p->ree_bmp.topo.dead|p->ree_bmp.ram_txid_no_wr);
		const u32 rer_cant_see = (ree_failed|((~p->rer_bmp.topo.readable) & p->rer_bmp.topo.raid.all));			// 'ree' did not write or 'rer' cant lock there
		p->rer_bmp.can_see_ree_txid_in_ram = !(__is_bmp_included_in(locks_bmp, rer_cant_see));		// 'ree' did not manage to write txid to any copy of lock
	} else { //HTR and Txid fully written to RAM or JGC
		p->rer_bmp.can_see_ree_txid_in_ram = true;
	}

	// can_see_ree_dbits_in_slice ?
	rer_bmp->can_see_all_ree_dbits_in_slice = true;
	rer_bmp->can_see_any_ree_dbits_in_slice = false;
	for (i = 0; i < p->inp.tx_height; i++) {
		rer_bmp->can_see_ree_dbits_in_slice[i] = (((rer_bmp->dmd_kosher[i]) & nvmeibc_raid1_get_parities_bmp(pr)) != 0);
		rer_bmp->can_see_all_ree_dbits_in_slice &= rer_bmp->can_see_ree_dbits_in_slice[i];
		rer_bmp->can_see_any_ree_dbits_in_slice |= rer_bmp->can_see_ree_dbits_in_slice[i];
	}


	if (p->inp.rer.io_perm.bits.is_cold_recovery) {  // In cold the RAM dbits will be calculated from the dmd dbits
		p->rer_bmp.can_see_any_ree_dbits_in_ram = false;
		p->rer_bmp.can_see_all_ree_dbits_in_ram = true;
		for (i = 0; i < p->inp.tx_height; i++) {
			p->rer_bmp.can_see_any_ree_dbits_in_ram |= rer_bmp->can_see_ree_dbits_in_slice[i];
			p->rer_bmp.can_see_all_ree_dbits_in_ram &= rer_bmp->can_see_ree_dbits_in_slice[i];
		}
	}
	else if (p->ree_bmp.ram_dbits_no_wr) {  // If some piggybacks failed, check if all are inaccessible to 'rer'
		const u32 rer_missing_dbits_bmp = ~(p->ree_bmp.topo.dead|p->rer_bmp.topo.dead|p->ree_bmp.ram_dbits_no_wr);
		p->rer_bmp.can_see_all_ree_dbits_in_ram = p->rer_bmp.can_see_any_ree_dbits_in_ram = ((locks_bmp & rer_missing_dbits_bmp) == (locks_bmp & p->rer_bmp.topo.readable));
	} else {  //HTR and dbit fully written to RAM or JGC
		p->rer_bmp.can_see_all_ree_dbits_in_ram = p->rer_bmp.can_see_any_ree_dbits_in_ram = true;
	}

	// does_know_txbm ?
	p->rer_bmp.does_know_txbm = false;
	if (p->rer_bmp.topo.has_rw_pari) {
		const u32 rer_pivot =  __find_rw_parity_with_lowest_seg_ind(p);	// Possible pivots for HTR to choose from, All RW parities must be valid regardless which is chosen as pivot and which is cross checked to match the pivot
		const u32 pivot_found =   (p->rer_bmp.jmd_kosher_intersect & (1<<rer_pivot));
		p->rer_bmp.does_know_txbm = (p->rer_bmp.can_see_ree_txid_in_ram && pivot_found);	// If pivot found, 'rer' knows the txbm, otherwise it does not!
	}

	rer_bmp->whole.any_regen = false;
	// Calculate Regen mask
	if (p->inp.nat.is_journal_overriden) {						// 'rer' will not find anything related to this candidate so wont fix
		EC_TX_BUG_ON(p->rer_bmp.does_know_txbm == true);					// No way we know TxBM, coz journal txbm was overwritte,
		memset(rer_bmp->whole.regen, 0, sizeof(rer_bmp->whole.regen));
	} else if (!rer_bmp->topo.has_rw_pari) {									// In no RW parities, regen parities
		EC_TX_BUG_ON(p->rer_bmp.does_know_txbm == true);						// No way we know TxBM, or else we would use it
		if (p->inp.rer.io_perm.bits.is_hot_recovery)
			for (i = 0; i < p->inp.tx_height; i++) {
				rer_bmp->whole.regen[i] = (rer_bmp->topo.w & rer_bmp->topo.raid.pari);  // Regen full blockset 'W' parities
				rer_bmp->whole.any_regen = rer_bmp->whole.any_regen || rer_bmp->whole.regen[i];
			}
		else
			memset(rer_bmp->whole.regen, 0, sizeof(rer_bmp->whole.regen));  // when no rw parities cold can only turn on RAM dbits
	} else if (p->rer_bmp.is_jour_committed && p->rer_bmp.can_see_ree_txid_in_ram) {
			EC_TX_BUG_ON(p->rer_bmp.does_know_txbm == false);					// We surrely know TxBM (from journal) and going to use it
			for (i = 0; i < p->inp.tx_height; i++) {
				rer_bmp->whole.regen[i] = (rer_bmp->topo.w & p->inp.ree.tx_bm[i]);		// 'rer' acknowledge that tx exists. Regen All 'W' segs for single slice
				rer_bmp->whole.any_regen = rer_bmp->whole.any_regen || rer_bmp->whole.regen[i];
			}
	} else { // Dont rebuild anything, coz surrely data was not written!
			for (i = 0; i < p->inp.tx_height; i++) {
				EC_TX_BUG_ON(ree_bmp->dmd_writn[i]);
			}
			EC_TX_BUG_ON((p->inp.rer.io_perm.bits.is_hot_recovery ) && (p->inp.ree.aband_stg != ec_tx_abort_stage_partial_journal));	// Else journal is full and htr would use it
			EC_TX_BUG_ON((p->inp.rer.io_perm.bits.is_cold_recovery) && (p->inp.ree.aband_stg  < ec_tx_abort_stage_partial_journal));	// Journal is partial or full but cold recovery cant use it for regen or roll fwd, only roll backwards because data does not point to this jour candidate
			memset(rer_bmp->whole.regen, 0, sizeof(rer_bmp->whole.regen)); // p->rer_bmp.does_know_txbm can be either false or true. Examples:
			// True : 'rer' knows TxBM + it knows data was not written
			// False: 'rer' does not know TxBM coz there wasn't any transaction
	}

	// Calculate Roll-fwd/Bck-wrd masks
	memset(rer_bmp->roll_fwd, 0, sizeof(rer_bmp->roll_fwd));   // By default, nothing
	memset(rer_bmp->roll_fwd_by_dbits_turnon, 0, sizeof(rer_bmp->roll_fwd_by_dbits_turnon)); 	// By default, nothing
	memset(rer_bmp->regen_fwd, 0, sizeof(rer_bmp->regen_fwd));
	memset(rer_bmp->whole.roll_bkw, 0, sizeof(rer_bmp->whole.roll_bkw));    // By default, nothing
	memset(rer_bmp->whole.regen_bkw, 0, sizeof(rer_bmp->whole.regen_bkw));
	memset(rer_bmp->whole.will_rollfwd, 0, sizeof(rer_bmp->whole.will_rollfwd));
	p->rer_bmp.whole.any_roll_bkw = false;
	p->rer_bmp.any_roll_fwd = false;

	if (rer_bmp->topo.has_rw_pari && p->rer_bmp.is_jour_committed) {
		for (i = 0; i < p->inp.tx_height; i++) {
			if (rer_bmp->dmd_kosher[i]) {					// Transaction will be fully commited after sync
				rer_bmp->roll_fwd[i] = rer_bmp->jmd_ready[i] & (~ree_bmp->dmd_writn[i]) & (~rer_bmp->whole.regen[i]);
				p->rer_bmp.any_roll_fwd = p->rer_bmp.any_roll_fwd || (!!rer_bmp->roll_fwd[i]);
				rer_bmp->regen_fwd[i] = rer_bmp->whole.regen[i];
				rer_bmp->roll_fwd_by_dbits_turnon[i] = p->inp.ree.tx_bm[i] & p->rer_bmp.topo.dead & (~(p->pre.slice_dbits[i]));  // Today we turnon dbits although it already turnedon in RAM but on parities not
				rer_bmp->whole.will_rollfwd[i] = true;
			} else if (p->rer_bmp.is_jour_committed && p->rer_bmp.does_know_txbm) { // Rer, sees jounral but no data. Maybe new data on non readable segs, must roll backwards it (via regen or dbits turn on)
				rer_bmp->whole.roll_bkw[i] = p->inp.ree.tx_bm[i] & (nvmeibc_get_nonrw_roles(p->rer_bmp.topo)) & (~rer_bmp->whole.regen[i]);
				p->rer_bmp.whole.any_roll_bkw = p->rer_bmp.whole.any_roll_bkw || (!!rer_bmp->whole.roll_bkw[i]);
				rer_bmp->whole.regen_bkw[i] = rer_bmp->whole.regen[i];
			}
			WARN(rer_bmp->roll_fwd[i] && rer_bmp->whole.roll_bkw[i], "Unitest bug\n");
		}
	}

	rer_bmp->nwhole.roll_bkw = 0;                                                  // By default, nothing
	rer_bmp->nwhole.regen_bkw = 0;
	if (p->inp.rer.io_perm.bits.is_hot_recovery && (!rer_bmp->topo.has_rw_pari) && (!p->inp.nat.is_journal_overriden)) {
		rer_bmp->nwhole.roll_bkw = rer_bmp->topo.raid.pari & (rer_bmp->topo.dead);
		rer_bmp->nwhole.regen_bkw = rer_bmp->topo.raid.pari & (rer_bmp->topo.w);
	}

	// TODO: remove can_change_ram_dbits and switch with can_turnon_ram_dbits || can_turnof_ram_dbits
	rer_bmp->can_change_ram_dbits = (p->inp.rer.io_perm.bits.is_hot_recovery && !p->inp.nat.is_journal_overriden) || ((!p->inp.rer.io_perm.bits.is_hot_recovery) && ((!rer_bmp->topo.has_rw_pari)||((p->rer_bmp.is_jour_committed) && (rer_bmp->any_dmd_kosher))||(rer_bmp->whole.any_roll_bkw)||(rer_bmp->whole.any_regen_bkw))); // Not 'should!' but rather 'can?'
	rer_bmp->can_turnon_ram_dbits = (((!rer_bmp->topo.has_rw_pari) && (!((p->inp.rer.io_perm.bits.is_hot_recovery) && (p->inp.nat.is_journal_overriden))))||((p->rer_bmp.is_jour_committed) && (rer_bmp->any_dmd_kosher))||(rer_bmp->whole.any_roll_bkw)||(rer_bmp->whole.any_regen_bkw));	// Not 'should!' but rather 'can?' Roll-fwd or regen //(((!p->inp.rer.io_perm.bits.is_hot_recovery) && (!rer_bmp->topo.has_rw_pari))||((p->rer_bmp.is_jour_committed) && (rer_bmp->dmd_kosher))||(rer_bmp->whole.roll_bkw)||(rer_bmp->regen_bkw));	// Not 'should!' but rather 'can?' Roll-fwd or regen
	rer_bmp->can_turnof_ram_dbits = (p->inp.rer.io_perm.bits.is_hot_recovery && !p->inp.nat.is_journal_overriden);   // Cold recovery does not do dirty rebuild to save time
	rer_bmp->can_turnon_ram_dbist_on_w_segs = ((p->inp.rer.io_perm.bits.is_cold_recovery) && (!rer_bmp->topo.has_rw_pari));


	rer_bmp->ree_ram_dbits_turnon_and_can_be_turnof_by_rer = 0;
	if (p->rer_bmp.can_see_any_ree_dbits_in_ram) {
		for (i = 0; i < p->inp.tx_height; i++) {
			rer_bmp->ree_ram_dbits_turnon_and_can_be_turnof_by_rer |= ((p->inp.ree.tx_bm[i] & p->ree_bmp.topo.dead) & rer_bmp->topo.w);
		}
	}

	rer_bmp->nwhole.ram_dbits_turnof = 0;
	rer_bmp->nwhole.ram_dbits_turnon_on_turnoff = 0;
	rer_bmp->nwhole.regen = 0;
	rer_bmp->nwhole.will_call_nwhole_sync = false;
	if (rer_bmp->can_turnof_ram_dbits || (rer_bmp->nwhole.roll_bkw || rer_bmp->nwhole.regen_bkw)) {		// Todo: Unify with __calc_dead_topo_for_dbits()
		bool will_do_dbits_rebuild_because_of_ree = false;
		bool will_do_dbits_rebuild_because_of_pre_ree = false;
		if (p->rer_bmp.can_see_any_ree_dbits_in_ram)
			will_do_dbits_rebuild_because_of_ree = !!(rer_bmp->ree_ram_dbits_turnon_and_can_be_turnof_by_rer);
		will_do_dbits_rebuild_because_of_pre_ree = !!((rer_bmp->topo.w) & (p->pre.ram_dbits | p->pre.ram_dconv));
		rer_bmp->nwhole.will_call_nwhole_sync = (will_do_dbits_rebuild_because_of_ree || will_do_dbits_rebuild_because_of_pre_ree) || (rer_bmp->nwhole.roll_bkw || rer_bmp->nwhole.regen_bkw);
		if (rer_bmp->nwhole.will_call_nwhole_sync) {
			rer_bmp->nwhole.ram_dbits_turnof = (rer_bmp->topo.w) & (rer_bmp->ree_ram_dbits_turnon_and_can_be_turnof_by_rer | p->pre.ram_dbits | p->pre.ram_dconv);
			if (rer_bmp->nwhole.ram_dbits_turnof)
				rer_bmp->nwhole.ram_dbits_turnon_on_turnoff = rer_bmp->topo.raid.pari & rer_bmp->topo.dead; /* Turn on dead parities if we have turnoff */
			rer_bmp->nwhole.regen = rer_bmp->nwhole.ram_dbits_turnof; // If Found transaction and can clean dbits, then data changed and dbits rebuild/ rollback (double deg parities) is needed for parity segs and data fixed segs for full blockset
		}
	}

	for (i = 0; i < p->inp.tx_height; i++) {
		rer_bmp->ree_dbits_turnon_on_slice[i] = 0;
		if (rer_bmp->can_see_ree_dbits_in_slice[i]) {
			rer_bmp->ree_dbits_turnon_on_slice[i] = (p->inp.ree.tx_bm[i] & p->ree_bmp.topo.dead);
		}
	}

	memset(rer_bmp->whole.slice_dbits_turnon, 0, sizeof(rer_bmp->whole.slice_dbits_turnon));
	memset(rer_bmp->whole.slice_dbits_rebuild, 0, sizeof(rer_bmp->whole.slice_dbits_rebuild));
	for (i = 0; i < p->inp.tx_height; i++) {
		rer_bmp->whole.slice_dbits_turnon[i] = rer_bmp->whole.roll_bkw[i] | rer_bmp->roll_fwd_by_dbits_turnon[i];
		if (p->rer_bmp.is_jour_committed && (p->inp.rer.io_perm.bits.is_hot_recovery || p->rer_bmp.does_know_txbm)) {
			rer_bmp->whole.slice_dbits_rebuild[i] = ((p->inp.ree.tx_bm[i] & p->rer_bmp.topo.w) & (p->pre.slice_dbits[i] | rer_bmp->ree_dbits_turnon_on_slice[i]));
		}

		rer_bmp->whole.slice_dbits_change[i] = rer_bmp->whole.slice_dbits_turnon[i] | rer_bmp->whole.slice_dbits_rebuild[i];
	}

	for (i = 0; i < p->inp.tx_height; i++) {
		rer_bmp->total.slice_dbits_turnon[i] = 0;
		if (p->inp.rer.io_perm.bits.is_hot_recovery) {
			if (!rer_bmp->topo.has_rw_pari) {
				rer_bmp->total.slice_dbits_turnon[i] = (rer_bmp->topo.dead) & rer_bmp->topo.raid.pari;
			} else {
				rer_bmp->total.slice_dbits_turnon[i] = rer_bmp->whole.roll_bkw[i] | rer_bmp->roll_fwd_by_dbits_turnon[i];
			}
		} else if ((p->rer_bmp.is_jour_committed) && (p->rer_bmp.does_know_txbm)) { // cold or jgc
			rer_bmp->total.slice_dbits_turnon[i] = (p->inp.ree.tx_bm[i] & p->rer_bmp.topo.dead);
		}
	}

	for (i = 0; i < p->inp.tx_height; i++) {
		rer_bmp->total.slice_dbits_rebuild[i] = 0;
		if (p->inp.rer.io_perm.bits.is_hot_recovery) {
			if (!p->inp.nat.is_journal_overriden) // If journal overriden no stale lock thus no dbits rebuild
				rer_bmp->total.slice_dbits_rebuild[i] = (p->rer_bmp.topo.w) & (p->pre.slice_dbits[i] | rer_bmp->ree_dbits_turnon_on_slice[i]);
		} else if ((p->rer_bmp.is_jour_committed) && (p->rer_bmp.does_know_txbm)) { // cold or jgc
			rer_bmp->total.slice_dbits_rebuild[i] = ((p->inp.ree.tx_bm[i] & p->rer_bmp.topo.w) & (p->pre.slice_dbits[i] | rer_bmp->ree_dbits_turnon_on_slice[i]));
		}

		rer_bmp->total.slice_dbits_change[i] = rer_bmp->total.slice_dbits_rebuild[i] | rer_bmp->total.slice_dbits_turnon[i];
	}


	rer_bmp->nwhole.syn_blkst_data_change = rer_bmp->nwhole.regen | rer_bmp->nwhole.regen_bkw;
	rer_bmp->nwhole.syn_blkst_dmd_change = rer_bmp->nwhole.syn_blkst_data_change;
	if (rer_bmp->nwhole.ram_dbits_turnof || rer_bmp->nwhole.roll_bkw) // This is correct because we don't inject bad sectors when there is nwhole called by htr
		rer_bmp->nwhole.syn_blkst_dmd_change |= (nvmeibc_raid1_get_parities_bmp(pr) & (~p->rer_bmp.topo.dead)); // Paritie's md allways changes if there is dbits turnon/off
	for (i = 0; i < p->inp.tx_height; i++) {
		rer_bmp->total.slice_data_touched[i] = (rer_bmp->roll_fwd[i] | rer_bmp->whole.regen[i]);
		WARN(!__is_bmp_included_in(rer_bmp->total.slice_data_touched[i], p->inp.ree.tx_bm[i]), "Unitest bug, Sync touches unrelated blocks in Tx slice\n");
		rer_bmp->total.slice_data_touched[i] |= rer_bmp->nwhole.syn_blkst_data_change;
	}

	for (i = 0; i < p->inp.tx_height; i++) {
		p->rer_bmp.whole.is_neverwritten_readable_parity[i] = p->inp.pre.is_never_written[i] && ((p->rer_bmp.topo.raid.pari) & (p->rer_bmp.topo.readable) & (~p->ree_bmp.dmd_writn[i]));
		rer_bmp->whole.is_neverwritten_source_parity_for_regen[i] = p->rer_bmp.whole.is_neverwritten_readable_parity[i] && (!p->rer_bmp.whole.will_rollfwd[i]);
		rer_bmp->nwhole.is_neverwritten_source_parity_for_regen[i] = rer_bmp->whole.is_neverwritten_source_parity_for_regen[i];
		p->rer_bmp.whole.is_neverwritten_slice_by_data_blocks[i] = p->inp.pre.is_never_written[i] && (((p->rer_bmp.topo.raid.data) & p->rer_bmp.topo.readable & (~p->ree_bmp.dmd_writn[i])) == (p->rer_bmp.topo.raid.data));
		p->rer_bmp.whole.is_neverwritten_slice[i] = p->rer_bmp.whole.is_neverwritten_readable_parity[i] || p->rer_bmp.whole.is_neverwritten_slice_by_data_blocks[i];
		{
			bool is_neverwritten_slice_by_data_blocks = p->rer_bmp.whole.is_neverwritten_slice_by_data_blocks[i] && (!p->rer_bmp.whole.will_rollfwd[i]);
			rer_bmp->nwhole.is_neverwritten_slice[i] = rer_bmp->nwhole.is_neverwritten_source_parity_for_regen[i] || is_neverwritten_slice_by_data_blocks;
		}
	}
}

static void __drain_all_prev_serjio_communication_with_jam(struct NVMeshSystem *sys, struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const int lockset_owner_seg = __get_slice_start_seg(p);
	u32 i;
	for (i = 0; i < pr->replicas; i++) {		// Why All? Todo, change only to TxBM
		const int di = (i + lockset_owner_seg) % pr->replicas;
		nvmeibs_nordda_jour_entry_do(&sys->servers[i], p->jtx.locations[di].jri, p->jtx.locations[di].jentry, "Drain Communication");
	}
}

/* Abandon the entries in serjios+Jam according to TxMB */
static void ec_tx_for_txbm_jrnl_do(struct NVMeshSystem *sys, struct t_ec_recov_tx *p, char *action) {
	const int lockset_owner_seg = __get_slice_start_seg(p);
	const struct disk_range *pr = p->inp.sraid.cpr;
	ulong i, all_abandoned = p->ree_bmp.abandoned_jent;

	if (action[0] == 'A') {	/* Abandoon the entries in serjios+Jam according to TxMB */
		//TODO: RRR inject valid transaction for JGC and verified it is not cleaned
		for_each_set_bit(i, &all_abandoned, pr->replicas) {   // Abandond even serjios which recovery cant access.
			const int di = (i + lockset_owner_seg) % pr->replicas;
			const char* entry_action = p->inp.rer.io_perm.bits.is_hot_recovery ? "Abandon Entry" : "UnknownEntry";
			nvmeibs_nordda_jour_entry_do(&sys->servers[di], p->jtx.locations[di].jri, p->jtx.locations[di].jentry, entry_action);
		}
	} else if (action[0] == 'C') {	/* Clean Leacked abandoned journals LKJ: when abandoned_jent can be different from jmd_with_correct_j2d clean them too*/
		ulong leaked_bmp;
		ulong jam_leaked_bmp = (~all_abandoned)&(p->rer_bmp.topo.raid.all);
		if (p->inp.rer.io_perm.bits.is_cold_recovery) {
			u32 w_j2d = ( all_abandoned & p->rer_bmp.topo.w);			//Todo: 'W' segments serjio is not notified by cold recovery and serjio currently cannot clean them in the simulator
			leaked_bmp = (all_abandoned ^ p->rer_bmp.jmd_ready_union);		// Default for cold recovery (all j2d except for what rer seen and cleaned in HTR)
			leaked_bmp &= nvmeibc_get_nonrw_roles(p->rer_bmp.topo);	// All RW segs are cleaned by cold, regardless of slices and candidates
			leaked_bmp |= w_j2d;
			leaked_bmp |= (p->ree_bmp.jmd_with_correct_j2d_union | p->rer_bmp.jmd_ready_union) & (~p->ree_bmp.jmd_with_valid_chain); // All the entries which are not formed valid chain but abandoned and there is md with valid j2d aren't going to be cleaned
		} else if (p->inp.rer.io_perm.bits.is_jgc_recovery) {			// JGC, everyhing not stale was cleaned
			leaked_bmp =  (p->flow.jgc_free_me ? 0 : (all_abandoned & (p->rer_bmp.topo.readable)));
			leaked_bmp |= (p->ree_bmp.jmd_with_correct_j2d_union | p->rer_bmp.jmd_ready_union) & (~p->ree_bmp.jmd_with_valid_chain); // All the entries which are not formed valid chain but abandoned and there is md with valid j2d aren't going to be cleaned
		} else {														// HTR
			if (p->inp.ree.inject_allien_lock_id) {
				leaked_bmp = all_abandoned;								// Serjio does not know this client so it will not clean any abandoned entries
			} else {
					leaked_bmp = all_abandoned&(~p->rer_bmp.send_blkst_recov);
			}
		}
		for_each_set_bit(i, &leaked_bmp, pr->replicas) {
			const int di = (i + lockset_owner_seg) % pr->replicas;
			WARN(!nvmeib_is_jmd_io_entry(*p->tpd.bptrs[0][i].ram.jmdc), "Could not access this JRI so could not clean the entry!\n");
			nvmeibs_nordda_jour_entry_do(&sys->servers[di], p->jtx.locations[di].jri, p->jtx.locations[di].jentry, "Free");
		}
		for_each_set_bit(i, &jam_leaked_bmp, pr->replicas) { // Free in JAM the unabandoned allocated entries which we didn't abandon on serjio
			const int di = (i + lockset_owner_seg) % pr->replicas;
			if (!nvmeibs_is_other_client(*p->inp.ree.uuid)) {
					nvmeibc_jam_simu_free_entry(sys->servers[di].disk, p->jtx.locations[di].jentry);
			}
		}
	} else {BUG();}
}

static inline struct jblock_md_decompressed_for_seg  __convert_cand_to_jmd_decomp(const struct nvmibc_tx_candidate *jtx, int h) {
	return (struct jblock_md_decompressed_for_seg){ .j2slba = jtx->b.j2slba + h, .tx_id = jtx->b.tx_id, .tx_bmp = jtx->b.tx_bmp[h], .has_next = 0, .version = 0 /*jtx->b.version_unused*/ };
}

static u64 J2D_OUTSIDE_OF_ALL_SEGS = 0;
void __calc_j2d_ouside_of_all_segs(const struct TstPRaid *pr) {
	u32 i;
	for (i = 0; i < pr->cpr->replicas; i++)
		MAX_WITH(J2D_OUTSIDE_OF_ALL_SEGS, pr->cpr[i].dlba_start);
	J2D_OUTSIDE_OF_ALL_SEGS += pr->cpr->length;
}

void ec_tx_for_jmdc_do(struct NVMeshSystem *sys, struct t_ec_recov_tx *p, char *action) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	u32 i, bit, h = 0;

	if (action[0] == 'I') {	/* Inject same values as in JMDD */
		for (h = 0; h < p->inp.tx_height; h++) {
			for (i = 0; i < pr->replicas; i++) {
				*p->tpd.bptrs[h][i].ram.jmdc = *p->tpd.bptrs[h][i].jmd;
			}
		}
	} else if (action[0] == 'V') {	/* Verify cleaned */
		//in case described below; serjio will execute JGC; lets wait for it. obviously not perfect solution; races are possible
		struct jblock_md_decompressed_for_seg model_jmd = { .j2slba = p->jtx.b.j2slba + h, .tx_id = p->jtx.b.tx_id, .tx_bmp = p->jtx.b.tx_bmp[0], .has_next = 0, .version = 0 /*p->jtx.b.version_unused*/ };
		u32 seg_of_sstart = __get_slice_start_seg(p);
		clientSimulator_wait_for_all_recoveries_done(sys->clients);
		for (i = 0, bit = 1; i < pr->replicas; i++, bit = (1 << i)) {
			const u32 di = (i + seg_of_sstart) % pr->replicas;
			union jblock_md *jmd = p->tpd.bptrs[h][i].ram.jmdc;
			const u64 j2d_expected = model_jmd.j2slba + (pr[di].dlba_start);		// Update j2d from relative to disk specific
			const u64 j2d_existing = __dp_ec_jmd_get_u64_j2d(jmd);
			if (       p->rer_bmp.jmd_ready_union &			   bit) {
				WARN(!nvmeib_is_jmd_unused_entry(jmd), "sgmnt=%d jri=%d jentry=%d was not freed by serjio as expected\n", di, p->jtx.locations[di].jri, p->jtx.locations[di].jentry);
			} else if (p->ree_bmp.jmd_with_correct_j2d_union & bit) { // Jounral that was inaccessible to 'rer', Once on D->W seg, Toma simu will tell serjio to cleanup, remove this line cleanup.
				if (   p->ree_bmp.abandoned_jent &			   bit) {
					WARN(!nvmeib_is_jmd_unused_entry(jmd), "sstart+%d, leaked-jentry not freed by serjio as expected!\n", i);
				} else {
					BUG(); // For now abandoned_jent == jmd_with_correct_j2d until cold can inform serjio on just the abandoned entries and not just by j2d to praid.
					WARN(j2d_existing != j2d_expected,  "sstart+%d, incorrect injection by unitest!\n", i);  // Non abandoned entires shouldn't be touched.
				}
			} else {
				WARN(j2d_existing != J2D_OUTSIDE_OF_ALL_SEGS,  "sstart+%d, incorrect injection by unitest!\n", i); // Daniel: Dont clean up old unrelated transactions in JMDC.
			}
		}
	} else {BUG();}
}

static inline roles_bmp_t __rotate_txbm(const roles_bmp_t txbm, const int slice_size) {
	int shift = 1;
	roles_bmp_t mask = (1 << slice_size) - 1;
	roles_bmp_t res = (txbm << shift) & mask;
	if (res == 0) {
		res = (txbm >> shift) & mask;
	}
	return res;
}

// turnon at list 2 unique data bits (one is not enough cause on double degraded we might get a candidate with only one journal seg) in txbm so a candidate will never be formed. (TXBM still valid)
static inline roles_bmp_t _get_next_pseudo_rand_txbm(u32 *upper_bit, u32 *lower_bit) {
	roles_bmp_t res;
	if ((*lower_bit) >= (*upper_bit)) { // creating an random txbm which is different from other replicas so I don't get candidate by mistake.
		(*upper_bit)--;
		(*lower_bit) = rand()%(*upper_bit);
	}

	res = GENMASK((*upper_bit), (*lower_bit));
	(*lower_bit)++;
	return res;
}

#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
void __inject_blk_to_disk(void *dst, const void *src, const struct disk_range *seg) {
	memcpy(dst, src, NVMEIBC_SECTOR_SIZE);
	if (seg)
		dp_dbgdi_do_add_info_unitest(dst, (char*)&seg->ruuid[0]);		// Inject debug di as if real writer wrote it
}

static void ec_tx_inject_transaction_to_ssds(struct t_ec_recov_tx *p, bool inject_debug_di_of_seg) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 seg_of_sstart = __get_slice_start_seg(p);
	u32 i, bit, h;
	for (h = 0; h < p->inp.tx_height; h++) {
		for (i = 0, bit = 1; i < pr->replicas; i++, bit = (1 << i)) {
			const u32 di = (i + seg_of_sstart) % pr->replicas;
			const struct disk_range *seg = inject_debug_di_of_seg ? &pr[di] : NULL;
			int h_minus_offset = h;
			/* Inject pre transaction garbage to disks and journlas with correct edic */

			/* dont care  = */   __unitest_fill_blocks_unique_pattern(p->tpd.bptrs[h][i].jrnl, 1);

			/* Inject transaction into data of Journal and Data LBAs  */
			if (p->jour_offset[i] != -1 && (h >= (u32)(p->jour_offset[i]))) {
				h_minus_offset = h - p->jour_offset[i];
			} // else h_minus_offset = h
			if ((!p->inp.nat.is_journal_overriden) && (p->ree_bmp.jmd_writn[h] & bit))
					__inject_blk_to_disk(p->tpd.bptrs[h_minus_offset][i].jrnl, p->tpd.ree.blocks[h][i], seg); // Set valid jour as Roll-fwd is expected

			/* Fill Data/Parity md  */
			if (p->rer_bmp.bad_sec_bmp[h] & bit) {
				__inject_blk_to_disk(p->tpd.bptrs[h][i].data, p->tpd.nat.blocks[h][i], seg);
				*p->tpd.bptrs[h][i].dmd = p->tpd.nat.md[h][i];
				p->tpd.slice_starting_state.blocks[h][i] = p->tpd.nat.blocks[h][i];
			} else if (p->ree_bmp.dmd_writn[h] & bit) {
				__inject_blk_to_disk(p->tpd.bptrs[h][i].data, p->tpd.ree.blocks[h][i], seg);
				*p->tpd.bptrs[h][i].dmd = p->tpd.ree.md[h][i];
				p->tpd.slice_starting_state.blocks[h][i] = p->tpd.ree.blocks[h][i];
			} else {
				__inject_blk_to_disk(p->tpd.bptrs[h][i].data, p->tpd.pre.blocks[h][i], seg);
				*p->tpd.bptrs[h][i].dmd = p->tpd.pre.md[h][i];
				p->tpd.slice_starting_state.blocks[h][i] = p->tpd.pre.blocks[h][i];
			}

			p->tpd.slice_starting_state.md[h][i] = *p->tpd.bptrs[h][i].dmd;
		}
	}

	{ /* Fill journal md into JMDD */
		for (h = 0; h < p->inp.tx_height; h++) {
			u32 upper_bit = pr->slice_size - 1; // LKJ: add rand
			u32 lower_bit = 0;
			struct jblock_md_decompressed_for_seg model_jmd = __convert_cand_to_jmd_decomp(&p->jtx, h);
			model_jmd.tx_bmp &= (~nvmeibc_raid1_get_parities_bmp(pr));				// Remove the parities, 'real' journal md entries include only data disks
			for (i = 0; i < pr->replicas; i++) {
				const u32 di = (i + seg_of_sstart) % pr->replicas;
				const u64 j2d_correct = (model_jmd.j2slba + pr[di].dlba_start);		// Update j2d from relative to disk specific
			    u64 j2d_visible;
				u32 tx_id_visible =  model_jmd.tx_id;
				roles_bmp_t txbm = model_jmd.tx_bmp;
				bool has_next = false;
				int h_minus_offset = h;
				if ((!p->inp.nat.is_journal_overriden) && (p->ree_bmp.jmd_writn[h] & (1 << i))) {
					has_next = ((h < (p->inp.tx_height - 1)) && (!!(p->jtx.b.tx_bmp[h] & p->jtx.b.tx_bmp[h+1] & (1 << i))));
					j2d_visible = j2d_correct;									// Must be correct J2D of commited journal
				} else if (p->ree_bmp.jmd_with_correct_j2d[h] & (1 << i)) {
					if (p->ree_bmp.jmd_with_valid_chain & (1<<i)) {
						if (p->ree_bmp.chain_height[i] > (h+1))
							has_next = true;
						if (p->ree_bmp.jmd_with_fake_chain & (1<<i))
							tx_id_visible = p->lid.pre.blkset_info.bits.txid;		// Old TxID
					} else {
						tx_id_visible = p->lid.pre.blkset_info.bits.txid;		// Old TxID
					}
					j2d_visible = j2d_correct;									// Old journal pre transaction, inject pseudo random missleading values, sometimes correct j2d somtimes not
					if (p->inp.rer.io_perm.bits.is_hot_recovery) {
						if (j2d_correct&0x4)
							txbm = __rotate_txbm(txbm, pr->slice_size); // 50% chance old TxBM == current Tx, 50% it is different (swap nibbles to generate valid TxBM)
					} else { // in cold or JGC we don't want to create by mistake a candidate to the blockset which will lead to unexpected behavior - Ofir: it can form a candidate once crac will inspect abandoned jentires.
						txbm = _get_next_pseudo_rand_txbm(&upper_bit, &lower_bit);
					}
				} else {
					j2d_visible = J2D_OUTSIDE_OF_ALL_SEGS;				// No transaction here that points to any valid segment
					// For maximal confusion keep TxID and TxBM identical to current transaction
				}

				if (p->jour_offset[i] != -1 && (h >= (u32)(p->jour_offset[i]))) {
					h_minus_offset = h - p->jour_offset[i];
				} // else h_minus_offset = h
				nvmeibc_block_dp_ec_jmd_encode(p->tpd.bptrs[h_minus_offset][i].jmd, j2d_visible, tx_id_visible, txbm, has_next, 0);
			}
		}
	}
}

static void __unitest_verify_dmd(struct t_ec_recov_tx *p, union nvmeibc_block_dp_ec_data_block_md* dmd, union nvmeibc_block_dp_ec_data_block_md exp, bool is_p, bool is_trans_err_injected) {
	bool is_never_written_explicitly_marked = nbdpec_md_was_data_explicitly_marked_never_written(&exp);
	if (p->inp.rer.io_perm.bits.is_jgc_recovery)
		return;

	EC_TX_BUG_ON(dmd->D.version != exp.D.version);
	if (is_p) {
		EC_TX_BUG_ON(dmd->P.dbits_0 != exp.P.dbits_0);
		EC_TX_BUG_ON(dmd->P.dbits_1 != exp.P.dbits_1);
		if (!is_never_written_explicitly_marked)  // those fileds are don't care upon neverwritten explicitly marked.
			EC_TX_BUG_ON(dmd->P.edic != exp.P.edic);
	} else {
		if (!is_never_written_explicitly_marked) // those fileds are don't care upon neverwritten explicitly marked.
			EC_TX_BUG_ON(dmd->D.edic != exp.D.edic);
	}

	if (is_trans_err_injected && p->inp.rer.io_perm.bits.is_hot_recovery) {
		EC_TX_BUG_ON(dmd->tx_id < exp.tx_id);
	} else {
		EC_TX_BUG_ON(dmd->tx_id != exp.tx_id);
	}
	if (p->inp.rer.bio_type != NVMEIB_BLOCK_IO_OP_WRITE) {   // If rer writes we can't predict his jri
		EC_TX_BUG_ON(dmd->jri != exp.jri);
	}
}

void ec_tx_verify_post_tx_ssd_disks_roll_fwd_dbits_fixup(struct t_ec_recov_tx *p, bool is_trans_err_injected) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	u32 i, h;
	const u32 slice_size = p->inp.sraid.cpr->slice_size;
	for (h = 0; h < p->inp.tx_height; h++) {
		for (i = 0; i < pr->replicas; ++i) {
			const u64 exp_data = *((u64*)p->tpd.exp.blocks[h][i]);
			__unitest_verify_blocks_pattern(p->tpd.bptrs[h][i].data, 1, exp_data, false);
			__unitest_verify_dmd(p, p->tpd.bptrs[h][i].dmd, p->tpd.exp.md[h][i], (i >= slice_size), is_trans_err_injected);
		}
	}
}

static u32 __calc_post_recov_dead_topo_for_ram_tx_dbits(struct t_ec_recov_tx *p) {
	const struct t_tx_recoverer_bmps *rer = &p->rer_bmp;
	u32 topo_dead = (rer->can_see_any_ree_dbits_in_ram) ? p->ree_bmp.topo.dead : 0;	// 'rer' may or may not see 'ree' dead segments. pre_dbits are not treated in this function
	u32 h;
	if (rer->can_see_any_ree_dbits_in_ram && !rer->can_see_all_ree_dbits_in_ram) {
		topo_dead = 0;
		for (h = 0; h < p->inp.tx_height; h++) {
			topo_dead |= p->rer_bmp.can_see_ree_dbits_in_slice[h] ? (p->ree_bmp.topo.dead &  p->inp.ree.tx_bm[h]) : 0;
		}
	}
	if (!rer->can_change_ram_dbits) {			// HTR will not touch dbit, default: ree dbits or 0 if ree did not write any or rer cant see any
		return topo_dead;
	} else if (!rer->can_turnof_ram_dbits) {   // Can only turn on: ree|rer.
		if (rer->can_turnon_ram_dbits)
			topo_dead |= p->rer_bmp.topo.dead;
		if (p->rer_bmp.can_turnon_ram_dbist_on_w_segs)
			topo_dead |= p->rer_bmp.topo.w;              // In this case always the W segs will be on txbm.
	} else {  //  Can turn off but might not turnon.
		if (rer->can_turnon_ram_dbits)
			topo_dead = p->rer_bmp.topo.dead;
		else   // turnoff what ree left
			topo_dead &= p->rer_bmp.topo.dead;
	}
	return topo_dead;
}

int ec_tx_calc_topo_ree_num_deg_segs(const struct t_ec_recov_tx *p) {
	int i, n_segs = p->inp.sraid.cpr->replicas;
	int num_deg = 0;
	for (i = 0; i < n_segs; i++) {
		const struct nvmeibc_disk_segment ds = {.toma_acm = p->inp.rer.topo[i] };	// Dummy segment
		if (!nvmeibc_is_readable(&ds))
			num_deg++;
	}
	// Daniel: Note, p->rer_bmp.topo.raid.all migt be uninitialized and rer topo may be not degraded the same as ree topo
	// BUG_ON(num_deg != (int)hweight32((~p->rer_bmp.topo.readable) & p->rer_bmp.topo.raid.all));
	return num_deg;
}

static u32 __gen_pre_tx_ram_dbits(struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 pre_dbits = p->pre.ram_dbits;
	const u32 pre_tx_dconv = p->pre.ram_dconv;
	const u32 dbits_pr = rol32_width(pre_dbits, __get_slice_start_seg(p), pr->replicas);
	const u32 dconv_pr = rol32_width(pre_tx_dconv, __get_slice_start_seg(p), pr->replicas);
	struct nvmeibc_dbits_tx db_tx;
	union nvmeibc_dbits_entry empty_dbits = { .all_bits = 0 };
	const struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};

	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dbits_pr, 0x0 /* No trun-off*/, dconv_pr);
	return nvmeibc_dbits_tx_apply(&empty_dbits, &db_tx);
}

static u32 __gen_ree_ram_dbits_from_dead_txbm(struct t_ec_recov_tx *p) {
	const u32 topo_dead = p->ree_bmp.topo.dead;
	const u32 txbm = p->inp.ree.tx_bm_union;
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 dbit_bm = (topo_dead & txbm) | p->pre.ram_dbits;
	const u32 pre_tx_dconv = p->pre.ram_dconv;
	const u32 dbits_pr = rol32_width(dbit_bm, __get_slice_start_seg(p), pr->replicas);
	const u32 dconv_pr = rol32_width(pre_tx_dconv, __get_slice_start_seg(p), pr->replicas);
	struct nvmeibc_dbits_tx db_tx;
	union nvmeibc_dbits_entry empty_dbits = { .all_bits = 0 };
	const struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};

	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dbits_pr, 0x0 /* No trun-off*/, dconv_pr);
	return nvmeibc_dbits_tx_apply(&empty_dbits, &db_tx);
}

static u32 __gen_post_recov_ram_dbits_from_dead_txbm(struct t_ec_recov_tx *p) {
	const u32 topo_dead = __calc_post_recov_dead_topo_for_ram_tx_dbits(p);
	const u32 txbm = p->inp.ree.tx_bm_union;
	const struct disk_range *pr = p->inp.sraid.cpr;
	const struct t_tx_recoverer_bmps *rer = &p->rer_bmp;
	const struct t_pre_tx_params *pre = &p->pre;
	const u32 non_readable = rer->topo.raid.all & (~rer->topo.readable);
	const u32 after_resolve_dbits = p->blkset->resolve_dbits.turnon_all_deg_segs ? (pre->ram_dbits | non_readable) : pre->ram_dbits;
	const u32 pre_dbits = (rer->can_change_ram_dbits && rer->can_turnof_ram_dbits) ? after_resolve_dbits & rer->topo.dead : after_resolve_dbits;
	bool is_cold = p->inp.rer.io_perm.bits.is_cold_recovery;
	const u32 dbit_bm = ((topo_dead & txbm) | pre_dbits);
	const u32 dconv_after_resolve_or_pre = pre->ram_dconv | (is_cold ? p->rer_bmp.topo.wm : 0); // In resolve dbits if seg is wm then turnon dconv on it.
	const u32 dconv_bm = (rer->can_change_ram_dbits && rer->can_turnof_ram_dbits) ? dconv_after_resolve_or_pre & rer->topo.dead : dconv_after_resolve_or_pre;
	const u32 dbits_pr = rol32_width(dbit_bm, __get_slice_start_seg(p), pr->replicas);
	const u32 dconv_pr = rol32_width(dconv_bm, __get_slice_start_seg(p), pr->replicas);
	struct nvmeibc_dbits_tx db_tx;
	union nvmeibc_dbits_entry empty_dbits = { .all_bits = 0 };
	const struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};
	p->blkset->resolve_dbits.after_resolve_dbits = after_resolve_dbits;
	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dbits_pr, 0x0 /* No trun-off*/, dconv_pr);
	return nvmeibc_dbits_tx_apply(&empty_dbits, &db_tx);
}

static union nvmeib_blkset_info __gen_toma_lockset_info_for_cold(struct t_ec_recov_tx *p) {
	union nvmeib_blkset_info blkset_info;
	const u32 n_deg = hweight32((~p->rer_bmp.topo.readable) & p->rer_bmp.topo.raid.all);
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 dconv_bm = p->pre.ram_dconv;
	const u32 dconv_pr = rol32_width(dconv_bm, __get_slice_start_seg(p), pr->replicas);
	struct nvmeibc_dbits_tx db_tx;
	union nvmeibc_dbits_entry dbits = {.all_bits = 0};
	const struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};
	if (n_deg)  // Toma don't inject unkowns if no degraded segs
		dbits.all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; // puts 2 unknowns
	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, 0x0 /*No dbits*/, 0x0 /* No trun-off*/, dconv_pr);
	blkset_info.bits.dirty = dbits.all_bits; //nvmeibc_dbits_tx_apply(&dbits, &db_tx);   // Add the convicts at the expanse of unkonws
	blkset_info.bits.txid = INITIAL_LAZY_READ_TXID;
	return blkset_info;
}

static u32 calc_dbit_rebuild_bm(struct t_ec_recov_tx *p, union nvmeibc_dbits_entry ram_dbits, struct nvmeibc_roles_bmps *topo_bmp)
{
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 topo_w_pr = rol32_width(topo_bmp->w, __get_slice_start_seg(p), pr->replicas);

	struct dp_topology_traits const topo_traits = {
		.n_degraded = ec_tx_calc_topo_ree_num_deg_segs(p),
	};

	sgmnts_bmp_t ram_dbits_bm = nvmeibc_dbits_get_turn_on_bmp(&ram_dbits, &topo_traits);
	return ((ram_dbits_bm) & (topo_w_pr));
}

static u32 __gen_rer_ram_dbits(struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	u32 h;
	u32 dbit_turnon_bm = 0;
	u32 dbits_turnon_pr = 0;
	struct nvmeibc_dbits_tx db_tx;
	u32 dbits_turn_off_bm_pr = 0;
	const union nvmeibc_dbits_entry post_recov_dbits = { .all_bits = p->lid.post_recov.blkset_info.bits.dirty };
	const struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};
	if (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE) {
		for (h = 0; h < p->inp.tx_height; h++) {
			dbit_turnon_bm |= ((p->inp.rer.tx_bm[h]) & (p->rer_bmp.topo.dead));
		}
		dbits_turnon_pr = rol32_width(dbit_turnon_bm, __get_slice_start_seg(p), pr->replicas);
	}

	if (p->rer_bmp.tx.will_call_nwhole_sync)
		dbits_turn_off_bm_pr = calc_dbit_rebuild_bm(p, post_recov_dbits, &p->rer_bmp.topo);

	if (p->blkset->last_tx.is_wraparound)
		dbits_turnon_pr |= rol32_width(p->rer_bmp.topo.dead, __get_slice_start_seg(p), pr->replicas);

	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dbits_turnon_pr, dbits_turn_off_bm_pr, 0);
	return nvmeibc_dbits_tx_apply(&post_recov_dbits, &db_tx);
}

static void ec_tx_calc_blkset_entries(struct t_ec_recov_tx *p) {
	const u32 ree_dbits = __gen_ree_ram_dbits_from_dead_txbm(p);
	const u32 post_recov_dbits = __gen_post_recov_ram_dbits_from_dead_txbm(p);
	const u32 pre_dbits = __gen_pre_tx_ram_dbits(p);
	u32 inj_lock_id = p->inp.ree.inject_allien_lock_id ? SIMULATOR_CLNT_ALLIEN_LOCK_NO_J : SIMULATOR_OTHER_CLIENT_LOCK_ID;
	union nvmeib_lock_blkset_entry lid_pre = {{
	 .lock_id =     {.all = 0 },
	 .blkset_info = {.bits = {.txid = p->inp.pre.txid, .dirty = pre_dbits,} },}};
	union nvmeib_lock_blkset_entry lid_ree = {{
		.lock_id =     {.bits = {.idx_in_praid = 0, .lock_id = inj_lock_id, .is_stale = 1, .is_read = 0, .reserved = 0,} },
		.blkset_info = {.bits = {.txid = p->inp.ree.txid, .dirty = ree_dbits,} },}};
	union nvmeib_lock_blkset_entry post_recov = {{
		.lock_id =     {.all = 0 },
		.blkset_info = {.bits = {.txid = p->inp.ree.txid, .dirty = post_recov_dbits,} },}};
	p->lid.pre = lid_pre;
	p->lid.ree = lid_ree;
	if (p->inp.rer.io_perm.bits.is_cold_recovery) {
		p->lid.nat.blkset_info = __gen_toma_lockset_info_for_cold(p);
		p->lid.nat.lock_id.bits.lock_id = 0;
	} else {
		p->lid.nat = lid_ree;
		if (!p->rer_bmp.can_see_any_ree_dbits_in_ram)
			p->lid.nat.blkset_info.bits.dirty = lid_pre.blkset_info.bits.dirty;
		if (p->inp.nat.is_journal_overriden) {
			p->lid.nat.lock_id.bits.lock_id = 0;
			p->lid.nat.lock_id.bits.is_stale = 0; // When journal overriden the lock is no longer stale.
		}
	}

	if (!p->rer_bmp.can_see_ree_txid_in_ram)
		post_recov.blkset_info.bits.txid--;				/* Tx doesnt exist for 'rer'. It will restore to:
				HTR:  Maximum of RAM TxID's it sees, COLD: Maximum of TxID's in accessible metadata (handled outside of this function (in the merge function)) */

	p->lid.post_recov = post_recov; // Not final value, TxID can change if 1. HTR - journal was not commited! 2. Cold- Data was not commited at all
	if (!p->inp.ree.is_old_completed) {
		p->blkset->lid.post_recov = p->lid.post_recov;
	} else {
		memset(&p->blkset->lid.post_recov, 0, sizeof(p->blkset->lid.post_recov));
	}
}

static void ec_tx_calc_expected_blkset_entries(struct t_ec_recov_tx *p) {
	p->lid.rer = p->lid.post_recov;
	p->lid.rer.blkset_info.bits.dirty = __gen_rer_ram_dbits(p);

	if (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE) {
		if (p->blkset->last_tx.is_wraparound)
			p->lid.rer.blkset_info.bits.txid = 2;               // After wraparound and write the txid should be 2.
		else
			p->lid.rer.blkset_info.bits.txid += 1;				// Doing additional IO after fixing 'ree' stuff
	}
}

static u32 __gen_pre_tx_slice_dbits(struct t_ec_recov_tx *p, int slice) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 dbits_pr = rol32_width(p->pre.slice_dbits[slice], __get_slice_start_seg(p), pr->replicas);
	struct nvmeibc_dbits_tx db_tx;
	union nvmeibc_dbits_entry empty_dbits = { .all_bits = 0 };
	const struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};
	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dbits_pr, 0x0 /* No trun-off*/, 0x0);
	return nvmeibc_dbits_tx_apply(&empty_dbits, &db_tx);
}

static u32 __gen_ree_slice_dbits(struct t_ec_recov_tx *p, int slice) {
	const u32 topo_dead = p->ree_bmp.topo.dead;
	const u32 txbm = p->inp.ree.tx_bm[slice];
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 dbit_bm = (topo_dead & txbm) | (p->pre.slice_dbits[slice]); // Assumes the ree doesn't turnoff slice dbits outside of txbm inorder to cover more cases and make the test easier.
	const u32 dbits_pr = rol32_width(dbit_bm, __get_slice_start_seg(p), pr->replicas);
	struct nvmeibc_dbits_tx db_tx;
	union nvmeibc_dbits_entry empty_dbits = { .all_bits = 0 };
	const struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};
	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dbits_pr, 0x0 /* No trun-off*/, 0x0);
	return nvmeibc_dbits_tx_apply(&empty_dbits, &db_tx);
}

static void ec_tx_calc_dmd(struct t_ec_recov_tx *p, const struct test_context *env) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 slice_size = p->inp.sraid.cpr->slice_size;
	u32 i, bit, h;
	const int lockset_owner_seg = __get_slice_start_seg(p);
	const u32 binje = nvmeibc_cinst_get_blok_p(env->dev)->binje;

	// Calc pre and ree dmd
	for (h = 0; h < p->inp.tx_height; h++) {
		for (i = 0, bit = 1; i < pr->replicas; i++, bit = (1 << i)) {
			const int di = (i + lockset_owner_seg) % pr->replicas;
			u32 jri = p->jtx.locations[di].jri, jentry = p->jtx.locations[di].jentry;

			union nvmeibc_block_dp_ec_data_block_md pre_dmd = p->tpd.pre.md[h][i];
			union nvmeibc_block_dp_ec_data_block_md ree_dmd = {.raw = 0};
			int h_minus_offset = h;
			if (p->jour_offset[i] != -1 && (h >= (u32)(p->jour_offset[i]))) {
				h_minus_offset = h - p->jour_offset[i];
			} // else h_minus_offset = h
			// -- Calc pre dmd
			if (i < slice_size) {
				if (!p->inp.pre.is_never_written[h]) {
					nbdpec_md_mark_valid_version(&pre_dmd);
					pre_dmd.D.d2j_rng = jentry * binje + h_minus_offset;    // Points to the correct jentry (don't want it to collide with other entry from the Tx).
					pre_dmd.D.edic = p->tpd.pre.md[h][i].D.edic;
				}
			} else {
				const union nvmeibc_dbits_entry dbits = {.all_bits = __gen_pre_tx_slice_dbits(p, h)};
				if (!p->inp.pre.is_never_written[h]) {
					nbdpec_md_mark_valid_version(&pre_dmd);
					nvmeibc_block_dp_ec_md_fill_p_with_dbits(&pre_dmd, dbits);
					pre_dmd.D.d2j_rng = jentry * binje + h_minus_offset;  // Points to the correct jentry (don't want it to collide with other entry from the Tx).
					pre_dmd.P.edic = p->tpd.pre.md[h][i].P.edic;
				} else if (p->inp.pre.is_parity_explicitly_marked_neverwritten[h]) {
					nvmeibc_block_dp_ec_md_fill_p_with_dbits(&pre_dmd, dbits);
				}
			}

			if (!p->inp.pre.is_never_written[h]) {
				if (p->inp.ree.tx_bm[h] & bit) {   // Non commited data due to transaction abort, No match expected (when we will have non umbigous we will be able inject misleading values)
					pre_dmd.tx_id = p->lid.pre.blkset_info.bits.txid;
					pre_dmd.jri = JRI_MARK_NO_JOURNAL;   // can use same code as above. Deniel: separated the if from else for debug only
				} else {// Inject missleading values to old data non in txbm,
					pre_dmd.tx_id = p->lid.pre.blkset_info.bits.txid; // Do not inject latest txid coz on wraparound it is cleaned. Injecting latest TxID is bad to HTR (may confuse his is_journal_commited decision), and devastating to cold recovery
					pre_dmd.jri = jri + ((p->jtx.b.j2slba + h)&0x1);  // Pseudo randomly jri points back to the correct or incorrect jri
				}
			}

			// -- Calc ree dmd
			nbdpec_md_mark_valid_version(&ree_dmd);
			ree_dmd.D.d2j_rng = jentry * binje + h_minus_offset;
			if (i < slice_size) {
				ree_dmd.D.edic = p->tpd.ree.md[h][i].D.edic;
			} else {
				const union nvmeibc_dbits_entry dbits = {.all_bits = __gen_ree_slice_dbits(p, h)};
				nvmeibc_block_dp_ec_md_fill_p_with_dbits(&ree_dmd, dbits);
				ree_dmd.P.edic = p->tpd.ree.md[h][i].P.edic;
			}
			ree_dmd.tx_id = p->jtx.b.tx_id;
			ree_dmd.jri =   jri;


			p->tpd.pre.md[h][i] = pre_dmd;
			p->tpd.ree.md[h][i] = ree_dmd;


			// -- Calc rer dmd
			nbdpec_md_mark_valid_version(&p->tpd.rer.md[h][i]);
			p->tpd.rer.md[h][i].tx_id = p->lid.rer.blkset_info.bits.txid;
		}
	}


}

/* Inject into RAMs */
static void ec_tx_inject_blkset_entries(struct t_ec_recov_tx *p) {
	const ulong lock_porters = 1 | p->ree_bmp.topo.raid.pari; //hard coding D0, P & Q; the real code uses lock map
	ulong l;

	if (p->inp.ree.is_old_completed) {
		return;
	} else if (p->inp.rer.io_perm.bits.is_cold_recovery) { // CLD - Cold recovery has inject into RAMs only dirty convicts
		for_each_set_bit(l, &lock_porters, p->inp.sraid.cpr->replicas) {
			struct block_ram_inject_ptrs* ir = &p->tpd.bptrs[0][l].ram; /* Mark locks (all/partially) as stale and inject blockset info*/
			ir->dbits->all_bits = p->lid.nat.blkset_info.bits.dirty;
			*ir->txid = p->lid.nat.blkset_info.bits.txid;
			*ir->lock = p->lid.nat.lock_id.bits.lock_id;
		}
	} else { // HTR or JGC
		for_each_set_bit(l, &lock_porters, p->inp.sraid.cpr->replicas){
			const u32 bit = (1<<l);
			struct block_ram_inject_ptrs* ir = &p->tpd.bptrs[0][l].ram; /* Mark locks (all/partially) as stale and inject blockset info*/
			union nvmeib_lock_id *toma_stale = p->tpd.bptrs[0][l].toma_stale_lock;
			const bool is_lock_dead_on_ree = (p->ree_bmp.topo.dead & bit);
			const bool is_lock_dead_on_rer = (p->rer_bmp.topo.dead & bit);
			const bool is_lock_zero_init_by_toma = (is_lock_dead_on_ree && ((!p->inp.nat.is_journal_overriden) || (is_lock_dead_on_rer))); // If journal overriden then there is no stale_lock and TOMA will copy locks from others.
			union nvmeib_lock_blkset_entry lid = ((is_lock_zero_init_by_toma) ? nvmeib_lock_init_value : p->lid.nat); // This ram was dead in 'ree' and initialized by tomas to 0 or copied from other locks in case there is no stale lock, or Valid transaction
			if ((!is_lock_dead_on_ree) && (!p->inp.nat.is_journal_overriden)) {									// Injections of 'ree' failure to write something
				if (p->ree_bmp.ram_txid_no_wr  & bit)				// Some txids were not updated by 'ree' before it died and include previous tx value
					lid.blkset_info.bits.txid--;                    // Assumption: TxID >= 2, In HTR we need the RAM to be valid but pre has voodoo different txid.
				if (p->ree_bmp.ram_dbits_no_wr & bit)				// Some dbits were not updated by 'ree' before it died and include previous tx value
					lid.blkset_info.bits.dirty = p->lid.pre.blkset_info.bits.dirty;
				if (p->inp.ree.is_garbage_for_jgc)
					lid.lock_id.all = 0;							// Garbage journal must have unlocked lock in blockset
			} 														// else this is the only copy of dbits, dont destroy it or else recoverer will not be able to access dbits and at all

			*ir->lock =          lid.lock_id.all;               // Todo: Inject partially (some locks)
			*ir->txid =          lid.blkset_info.bits.txid;
			 ir->dbits->all_bits =lid.blkset_info.bits.dirty;
			toma_stale->all = *ir->lock;

			p->blkset->is_unknown_txid_injected_to_ram = false;
			if (p->inp.rer.io_perm.bits.is_hot_recovery) {
			   u32 max_txid_in_blkset = (!!p->rer_bmp.any_dmd_kosher) ? p->inp.ree.txid : (p->blkset->tx[0].inp.ree.txid);
			   if ((!p->blkset->nwhole.bad_sec_bmp) && (max_txid_in_blkset == lid.blkset_info.bits.txid) &&
				  ((!p->inp.nat.is_journal_overriden) || p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE)) {

				   p->blkset->is_unknown_txid_injected_to_ram = rand()%2;
				   if (p->blkset->is_unknown_txid_injected_to_ram) {
					   *ir->txid = 0;
				   }
			   }
			}

			if (p->inp.rer.io_perm.bits.is_hot_recovery && p->blkset->is_unknown_dbits_injected_to_ram) {
				ir->dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; // puts 2 unknowns
			}


			 if (p->inp.rer.io_perm.bits.is_jgc_recovery) {
				 p->lid.rer.lock_id.all |= *ir->lock;			// Journal GC will encounter ree's locks and will not fix anything in ram so rer lock == ree lock
				 p->lid.post_recov.lock_id.all = p->lid.rer.lock_id.all;
			 }
		}
	}
}

static void __verify_ram_txid_equals_rers_dmd(struct t_ec_recov_tx *p, const u32 ram_txid) {
	ulong role;
	const u32 replicas = p->inp.sraid.cpr->replicas;
	u32 h;
	for (h = 0; h < p->inp.tx_height; h++) {
		const ulong tx_writable = p->inp.rer.tx_bm[h] & ((~p->rer_bmp.topo.dead) & (p->rer_bmp.topo.raid.all));

		EC_TX_BUG_ON(p->inp.ree.is_old_completed);
		EC_TX_BUG_ON(p->inp.rer.bio_type != NVMEIB_BLOCK_IO_OP_WRITE);

		for_each_set_bit(role, &tx_writable, replicas) {
			EC_TX_BUG_ON(p->tpd.bptrs[h][role].dmd->tx_id != ram_txid);
		}
	}
}

static void __verify_post_tx_lock_blkset_entry(struct t_ec_recov_tx *p, bool is_trans_err_injected) {
	union nvmeib_blkset_info ebi = p->lid.rer.blkset_info;				// Expected blockset info in all copies of locks
	union nvmeibc_dbits_entry ebid = {.all_bits = ebi.bits.dirty};
	if (!p->inp.ree.is_old_completed){
		const ulong lock_porters = 1 | p->ree_bmp.topo.raid.pari; //hard coding D0, P & Q; the real code uses lock map
		ulong l;
		for_each_set_bit(l, &lock_porters, p->inp.sraid.cpr->replicas){
			struct block_ram_inject_ptrs* ir = &p->tpd.bptrs[0][l].ram; //Injection to ram
			union nvmeib_lock_id *toma_lock = p->tpd.bptrs[0][l].toma_stale_lock;
			if ((p->inp.rer.topo[l] != NVMEIBTC_DS_MODE_DEAD)&&(!p->inp.rer.io_perm.bits.is_jgc_recovery)) {
				EC_TX_BUG_ON(*ir->lock            != p->lid.rer.lock_id.all);	// All stale locks were cleaned by HTR or untouched by JGC
				EC_TX_BUG_ON(toma_lock->all != *ir->lock);
				if (is_trans_err_injected && p->inp.rer.io_perm.bits.is_hot_recovery) { // All TxID's are set to post transaction regardless of roll forward or abort
					EC_TX_BUG_ON(*ir->txid < ebi.bits.txid);    // if there is a retry of the op the txid can inc more than once..
					if ((!p->rer_bmp.tx.will_call_nwhole_sync)) {  // Because HTR is mandatory no_whole_sync called by it must finish correctly, so we only have to consider HTR's called by rer's TX.
						EC_TX_BUG_ON(ir->dbits->all_bits != ebid.all_bits);
					} else {
						EC_TX_BUG_ON((ir->dbits->all_bits != ebid.all_bits) && (ir->dbits->all_bits != p->lid.post_recov.blkset_info.bits.dirty));            // Verify Cleaned Dbits on on 'W' segments and Turn on on 'D' or that maybe there was a transport error while commiting non-owner binfo.
					}
					if (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE) {
						__verify_ram_txid_equals_rers_dmd(p, *(ir->txid));
					}
				} else {
					EC_TX_BUG_ON( ir->dbits->all_bits != ebid.all_bits);			// Verify Cleaned Dbits on on 'W' segments and Turn on on 'D'
					EC_TX_BUG_ON(*ir->txid != ebi.bits.txid);
				}
			}
			*ir->lock =           0;
			toma_lock->all = 0;
			 ir->dbits->all_bits = 0;									// Clean for next unitest scenario
		}
	}
}

static void ec_tx_init_jrnl_candidates(struct test_context env, struct t_ec_recov_tx* p){
	struct t_input_params *inp = &p->inp;
	struct NVMeshSystem *sys = env.sys;
	struct nvmibc_tx_candidate *c = &p->jtx;
	const struct disk_range *pr = p->inp.sraid.cpr;
	u32 h;
	for (u32 i = 0; i < inp->sraid.cpr->replicas; i++) {
		const u64 dlba = inp->slba + pr[i].dlba_start;
		const u32 txid = inp->ree.txid;
		c->locations[i].jri = nvmeibs_get_jri_by_uuid(&sys->servers[i], inp->ree.uuid);
		if (nvmeibs_is_other_client(*inp->ree.uuid)){
			c->locations[i].jentry = (i + jentry_in_jri_seed) % NUM_JENTS_JAM_USES_IN_JRI(sys->servers[i].disk);
		} else{
			c->locations[i].jentry = nvmeibc_jam_simu_alloc_entry(sys->servers[i].disk, dlba, txid - 1, false);
		}

		p->jour_offset[i] = -1;
		for (h = 0; h < p->inp.tx_height; h++) {
			if (!!((1 << i) & inp->ree.tx_bm[h])) {
				p->jour_offset[i] = h;
				break;
			}
		}
	}
	jentry_in_jri_seed++;

	c->b.j2slba = inp->slba;			// Slice is important for CRC
	c->b.tx_id = inp->ree.txid;
	for (h = 0; h < p->inp.tx_height; h++) {
		c->b.tx_bmp[h] = inp->ree.tx_bm[h];   // txbm with parities - will be excluded when set in jmdc/jmdd
	}
	c->b.len = 1;
	//c->b.version_unused = nvmeibc_jmd_wr_version;
}

void ec_tx_set_and_switch_rer_topo(struct NVMeshSystem *sys, struct t_ec_recov_tx *p, bool set) {
	struct TstPRaid *pra = &p->inp.sraid;
	u32 i, n_segs = p->inp.sraid.cpr->replicas, s_start = __get_slice_start_seg(p);
	enum NVMEIBTC_DS_MODE acms[n_segs];
	if (set) {
		pra->tpr->header.io_perms = p->inp.rer.io_perm.all;		// Specific IO permissions for recovery
		for (i = 0; i < n_segs; i++) {
			acms[((i + s_start) % n_segs)] = p->inp.rer.topo[i];
		}
	} else {		// Undo segment access mode (All == RW)
		pra->tpr->header.io_perms = ~0;			// All is permitted
		for (i = 0; i < n_segs; i++)
			p->inp.rer.topo[i] = acms[i] = NVMEIBTC_DS_MODE_RW;
	}
	tomaSimulator_switchTopoEC(r1uuid(pra->tpr), acms, SW_TOPO__WAIT_ACK_DR, NULL);
	(void)sys;													// Daniel, dont remove, for future use
}

static void ec_tx_calc_expected_blkst_recov_msgs(struct t_ec_recov_tx *p) {			// Which serjios will be notified to clean the abandoned jentries for current blockset
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 txbm_ow_lock = (p->inp.ree.tx_bm_union | (1 /* Owner lock*/));		// TxBM + Owner lock

	if (p->inp.rer.io_perm.bits.is_hot_recovery) {
		p->rer_bmp.send_blkst_recov = p->rer_bmp.can_see_ree_txid_in_ram ? p->rer_bmp.jmd_kosher_intersect : 0;
	} else {
		u32 brecov_try = (p->rer_bmp.does_know_txbm ? txbm_ow_lock : all_segs_bm(pr));    // Attempt to send those blockset recovered
		p->rer_bmp.send_blkst_recov = (brecov_try & (~p->rer_bmp.topo.dead));       // Only those will reach serjios
	}
}

static void ec_tx_calc_expected_flow_counters(struct t_ec_recov_tx *p) {
	if (p->inp.rer.io_perm.bits.is_hot_recovery) {
		p->flow.htr_process_me = p->lid.nat.lock_id.bits.is_stale;
		p->flow.jgc_free_me = false;
	} else if (p->inp.rer.io_perm.bits.is_jgc_recovery) {
		p->flow.htr_process_me = false;						// Garbage collection never involves HTR
		p->flow.jgc_free_me = (p->lid.rer.lock_id.all == 0);// Only if unlocked
	} else {
		u32 h;
		p->flow.htr_process_me = false;
		for (h = 0; h < p->inp.tx_height; h++) {
			p->flow.htr_process_me |= (!p->inp.nat.is_journal_overriden) && (p->inp.ree.tx_bm[h] != p->rer_bmp.dmd_kosher[h]);	// Cold: Htr will process only if transaction wasnt fully commited and journal wan't overriten.
		}
		if (p->inp.rer.io_perm.bits.is_cold_recovery && (!p->rer_bmp.topo.has_rw_pari)) { // starting from verion 1 we decided not to handle candidates when no rw parity since resolve dbits will either way will fix the write hole.
			p->flow.htr_process_me = false;
		}
		p->flow.jgc_free_me = false;
	}
}

static void ec_tx_gen_pre_tx_dbits_bmps(struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	struct t_tx_recoveree_bmps *ree_bmp = &p->ree_bmp;
	struct t_tx_recoverer_bmps *rer_bmp = &p->rer_bmp;
	struct t_pre_tx_params *pre = &p->pre;
	u32 i;

    u16 can_dbit_exist_in_topo_bmp = (ree_bmp->topo.w & nvmeibc_get_nonrw_roles(rer_bmp->topo)) | ree_bmp->topo.dead; // The topology of seg rer can't be rw if in ree was dbit over there
    pre->ram_dbits = can_dbit_exist_in_topo_bmp & random_bitmap(pr);     // Generate random dirty bits in valid positions.
	if (p->inp.rer.io_perm.bits.is_cold_recovery && p->inp.pre.is_never_written_union && (!p->inp.pre.is_parity_explicitly_marked_neverwritten_union)) {  // If the slice is neverwriten there are no dbits in slice therefor in cold recovery ram dbits correspond to this slice will be ignored.
		pre->ram_dbits = 0;
		pre->ram_dconv = 0;
	} else if (!p->inp.ree.is_old_completed) {
		if (p->inp.rer.io_perm.bits.is_hot_recovery) {
			pre->ram_dconv = pre->ram_dbits & ree_bmp->topo.wm & random_bitmap(pr);         // Change some dbits to Convict.
		} else {
			pre->ram_dconv = ree_bmp->topo.wm;         // Change some dbits to Convict.
		}
		pre->ram_dbits &= (~pre->ram_dconv);
	} else { // On old tx no dconvicts because they don't check any new functionality.
		pre->ram_dconv = 0;
	}

	pre->slice_dbits_union = 0;
	for (i = 0; i < p->inp.tx_height; i++) {
		if (p->inp.pre.is_never_written[i] && (!p->inp.pre.is_parity_explicitly_marked_neverwritten[i])) {
			pre->slice_dbits[i] = 0;
		} else if (p->inp.rer.io_perm.bits.is_cold_recovery) {
			pre->slice_dbits[i] = (pre->ram_dbits | pre->ram_dconv);  // In cold recovery only the slice dbits matters cause nat disaster erase the RAM.
		} else {
			pre->slice_dbits[i] = (pre->ram_dbits | pre->ram_dconv) & random_bitmap(pr); // pass some ram dbits to slice
		}
		pre->slice_dbits_union |= pre->slice_dbits[i];
	}

	pre->ram_dbits |= p->inp.pre.history_ram_dbits; // Add dbits of history to considaration.
}

// Calculates the dbits of writable parities after recovry finish (not including rer's bio)
static u32 __gen_post_recov_slice_dbits(struct t_ec_recov_tx *p, int h) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	u32 dbits_pr;
	struct nvmeibc_dbits_tx db_tx;
	union nvmeibc_dbits_entry empty_dbits = { .all_bits = 0 };
	u32 ree_dbits_bm = (p->rer_bmp.can_see_ree_dbits_in_slice[h]) ? (p->ree_bmp.topo.dead & p->inp.ree.tx_bm[h]) : 0;	// 'rer' may or may not see 'ree' dbits.
	u32 dbit_bm = 0;
	const roles_bmp_t bad_sectors_affect_on_nwhole_sync_regen = p->blkset->nwhole.is_sl_by_sl ? p->rer_bmp.bad_sec_bmp[h] : p->blkset->nwhole.bad_sec_bmp;

	struct dp_topology_traits const topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
		.n_degraded = ec_tx_calc_topo_ree_num_deg_segs(p),
	};
	bool is_valid_parity = !!((p->rer_bmp.topo.raid.pari) & (p->rer_bmp.topo.readable) & (~bad_sectors_affect_on_nwhole_sync_regen));
	if (!is_valid_parity) {  // when no RW parities there are no source parities so iserting worst case to slice
		const union nvmeibc_dbits_entry e = { .all_bits = p->lid.post_recov.blkset_info.bits.dirty };
		dbits_pr = nvmeibc_dbits_get_turn_on_bmp(&e, &topo_traits);
	} else {
		dbit_bm = (((p->pre.slice_dbits[h] | ree_dbits_bm) & (~p->rer_bmp.total.slice_dbits_rebuild[h]) & (~p->rer_bmp.nwhole.regen)) | (p->rer_bmp.roll_fwd_by_dbits_turnon[h]) | (p->rer_bmp.whole.roll_bkw[h]) | (p->rer_bmp.nwhole.ram_dbits_turnon_on_turnoff));
		dbits_pr = rol32_width(dbit_bm, __get_slice_start_seg(p), pr->replicas);
	}

	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dbits_pr, 0x0 /* No trun-off*/, 0);
	return nvmeibc_dbits_tx_apply(&empty_dbits, &db_tx);
}

/* offset_in_slice = 0 for parities */
static u32 __calc_edic_for_block(struct t_ec_recov_tx *p, const struct test_context *env, const unsigned char *data, const int column, const int snake_size, const int h) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u64 snrlba = ((p->inp.slba + h) / snake_size) * pr->slice_size * snake_size;
	const u64 snake_row = (p->inp.slba + h) % snake_size;
	const u64 rlba = (snrlba + (column * snake_size + snake_row));
	return nvmeibc_calculate_edic_from_data_and_rlba(rlba, data, env->dev->dp.enable_di_debug_mode);
}

void ec_tx_calc_dmd_blkset_sync_results(struct t_ec_recov_tx *p, u32 i, const union nvmeibc_dbits_entry dbits, const struct test_context *env, bool is_neverwritten_slice, int h) {
	const u32 slice_size = p->inp.sraid.cpr->slice_size;
	u32 edic;
	if (i < slice_size) {
		edic = p->tpd.exp.md[h][i].D.edic;
		if (is_neverwritten_slice) {
			nbdpec_md_mark_data_never_written_no_dbits(&p->tpd.exp.md[h][i], false);
		} else {
			if (nbdpec_md_was_data_never_written(&p->tpd.exp.md[h][i]))
				edic = __calc_edic_for_block(p, env, (unsigned char *)p->tpd.pre.blocks[h][i], i, 1/*get snake_size or add to cpr*/, h);
			nvmeibc_block_dp_ec_md_make_d(&p->tpd.exp.md[h][i], edic, JRI_MARK_NO_JOURNAL, p->rer_bmp.total.max_txid_in_slice[h], 0);
		}
	} else {
		edic = p->tpd.exp.md[h][i].P.edic;
		if (is_neverwritten_slice) { // turnof or regen - neverwritten will turn to mark_neverwritten
				nbdpec_md_mark_data_never_written_with_dbits(&p->tpd.exp.md[h][i], true, dbits);
		} else {
			if (nbdpec_md_was_data_never_written(&p->tpd.exp.md[h][i]))  // There might be neverwritten on non readable segs.
				edic = __calc_edic_for_block(p, env, (unsigned char *)p->tpd.pre.blocks[h][i], 0 /* slice offset of parities in rlba is 0 for parity calculations*/, 1/*get snake_size or add to cpr*/, h);
			nvmeibc_block_dp_ec_md_make_p(&p->tpd.exp.md[h][i], edic, JRI_MARK_NO_JOURNAL, p->rer_bmp.total.max_txid_in_slice[h], dbits, 0);
		}
	}
}

void ec_tx_calc_dmd_blkset_wraparound_results(struct t_ec_recov_tx *p, u32 i, const union nvmeibc_dbits_entry dbits, bool is_neverwritten_slice, int h) {
	const u32 slice_size = p->inp.sraid.cpr->slice_size;
	u32 edic;
	if (i < slice_size) {
		edic = p->tpd.exp.md[h][i].D.edic;
		if (is_neverwritten_slice || nbdpec_md_was_data_never_written(&p->tpd.exp.md[h][i])) {
			nbdpec_md_mark_data_never_written_no_dbits(&p->tpd.exp.md[h][i], false);
		} else {
			nvmeibc_block_dp_ec_md_make_d(&p->tpd.exp.md[h][i], edic, JRI_MARK_NO_JOURNAL, 1, 0);
		}
	} else {
		edic = p->tpd.exp.md[h][i].P.edic;
		if (is_neverwritten_slice) { // turnof or regen - neverwritten will turn to mark_neverwritten
				nbdpec_md_mark_data_never_written_with_dbits(&p->tpd.exp.md[h][i], true, dbits);
		} else {
			nvmeibc_block_dp_ec_md_make_p(&p->tpd.exp.md[h][i], edic, JRI_MARK_NO_JOURNAL, 1, dbits, 0);
		}
	}
}

static u32 ec_tx_calc_rer_slice_dbits_after_nwhole(struct t_ec_recov_tx *p, u32 post_recov_slice_dbits) {
	const union nvmeibc_dbits_entry post_recov_slice_dbits_entry = { .all_bits = post_recov_slice_dbits };
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 topo_w_pr = rol32_width(p->rer_bmp.topo.w, __get_slice_start_seg(p), pr->replicas);

	struct dp_topology_traits const topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
		.n_degraded = ec_tx_calc_topo_ree_num_deg_segs(p),
	};

	const sgmnts_bmp_t post_recov_dbits_bmp_pr = nvmeibc_dbits_get_turn_on_bmp(&post_recov_slice_dbits_entry, &topo_traits);
	const u32 turnoff_bmp_pr = p->rer_bmp.tx.will_call_nwhole_sync ? (post_recov_dbits_bmp_pr & topo_w_pr) : 0;
	struct nvmeibc_dbits_tx db_tx;

	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, 0, turnoff_bmp_pr, 0x0);
	return nvmeibc_dbits_tx_apply(&post_recov_slice_dbits_entry, &db_tx);
}

static u32 ec_tx_calc_rer_slice_dbits_after_wraparound(struct t_ec_recov_tx *p, const union nvmeibc_dbits_entry post_nwhole_dbits, int h) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 topo_d_pr = rol32_width(p->rer_bmp.topo.dead, __get_slice_start_seg(p), pr->replicas);
	struct nvmeibc_dbits_tx db_tx;
	struct dp_topology_traits topo_traits = {
		.n_parities = __disk_range_get_num_parities(pr),
	};

	if (p->rer_bmp.tx.is_neverwritten_slice[h])  // if slice neverwritten no dbits will be turned on on it.
		return post_nwhole_dbits.all_bits;

	nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, topo_d_pr, 0, 0x0);
	return nvmeibc_dbits_tx_apply(&post_nwhole_dbits, &db_tx);
}

static void ec_tx_calc_expected_ssds_state(struct t_ec_recov_tx *p, const struct test_context *env) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 slice_size = p->inp.sraid.cpr->slice_size;
	const int lockset_owner_seg = __get_slice_start_seg(p);
	u32 i, bit, _edic, h;
	const u32 binje = nvmeibc_cinst_get_blok_p(env->dev)->binje;

	for (h = 0; h < p->inp.tx_height; h++) {
		u32 post_recov_slice_dbits = __gen_post_recov_slice_dbits(p, h);
		const union nvmeibc_dbits_entry rer_slice_dbits_after_nwhole = { .all_bits = ec_tx_calc_rer_slice_dbits_after_nwhole(p, post_recov_slice_dbits)};
		const union nvmeibc_dbits_entry rer_slice_dbits_after_wrap = { .all_bits = ec_tx_calc_rer_slice_dbits_after_wraparound(p, rer_slice_dbits_after_nwhole, h)};

		p->rer_bmp.total.max_txid_in_slice[h] = p->rer_bmp.dmd_kosher[h] ? p->inp.ree.txid : p->inp.pre.txid;

		for (i = 0, bit = 1; i < pr->replicas; i++, bit = (1 << i)) {
			const int di = (i + lockset_owner_seg) % pr->replicas;
			u32 jri = p->jtx.locations[di].jri, jentry = p->jtx.locations[di].jentry;

			// -- Calc expected data and dmd's edic
			switch (ec_tx_get_last_succeful_writter(p, i, h)) {
			case ec_tx_last_writer_rer:
				p->tpd.exp.blocks[h][i] = p->tpd.rer.blocks[h][i];
				p->tpd.exp.md[h][i] = p->tpd.rer.md[h][i];
				break;
			case ec_tx_last_writer_data_loss:
				p->tpd.exp.blocks[h][i] = p->tpd.nat.blocks[h][i];
				p->tpd.exp.md[h][i] = p->tpd.nat.md[h][i];
				break;
			case ec_tx_last_writer_ree:
				p->tpd.exp.blocks[h][i] = p->tpd.ree.blocks[h][i];
				p->tpd.exp.md[h][i] = p->tpd.ree.md[h][i];
				break;
			case ec_tx_last_writer_predata:
				p->tpd.exp.blocks[h][i] = p->tpd.pre.blocks[h][i];
				p->tpd.exp.md[h][i] = p->tpd.pre.md[h][i];
				break;
			default:	BUG();
			}

			// -- Calc expected md
			_edic = (i < slice_size) ? p->tpd.exp.md[h][i].D.edic : p->tpd.exp.md[h][i].P.edic; // Edic already calculate here.
			if (!p->inp.nat.is_journal_overriden) {  // when journal_overriden we take the state ree left.
				const union nvmeibc_dbits_entry post_recov_dbits = {.all_bits = is_writable_pari(bit) ? post_recov_slice_dbits : 0};  // 0 - we dont's care (dbits stay the same as before natural disaster)

				if (p->inp.ree.is_old_completed)
					continue;			// Not relevant to HTR cause we don't have history, only for cold.

				if (p->rer_bmp.roll_fwd[h] & bit) { // Calc roll-forward results
					int h_minus_offset = h;
					if (p->jour_offset[i] != -1 && (h >= (u32)(p->jour_offset[i]))) {
						h_minus_offset = h - p->jour_offset[i];
					} // else h_minus_offset = h
					p->tpd.exp.md[h][i].D.d2j_rng = jentry * binje + h_minus_offset;
					if (i < slice_size) {
						nbdpec_md_mark_valid_version(&p->tpd.exp.md[h][i]);
					} else {
						nvmeibc_block_dp_ec_md_set_txid_and_dirty_bits_value_mark_no_journal(&p->tpd.exp.md[h][i], p->rer_bmp.total.max_txid_in_slice[h], post_recov_dbits);
					}
					p->tpd.exp.md[h][i].tx_id = p->jtx.b.tx_id;
					p->tpd.exp.md[h][i].jri = jri;
				} else if (p->rer_bmp.whole.regen[h] & bit) { // Calc regen results - when there are no rw parities the dmd will be changed by nwhole sync.
					u32 _jri = JRI_MARK_NO_JOURNAL;
					if (i < slice_size) {
						if (p->rer_bmp.whole.is_neverwritten_source_parity_for_regen[h]) { // needs to mark neverwritten.
							nbdpec_md_mark_data_never_written_no_dbits(&p->tpd.exp.md[h][i], false);
						} else {
							if (nbdpec_md_was_data_never_written(&p->tpd.exp.md[h][i]))  // needs to calc the edic cause it never calced before.
								_edic = __calc_edic_for_block(p, env, (unsigned char *)p->tpd.pre.blocks[h][i], i /* slice offset of parities in rlba is 0 for parity calculations*/, 1/*get snake_size or add to cpr*/, h);
							nvmeibc_block_dp_ec_md_make_d(&p->tpd.exp.md[h][i], _edic, _jri, p->rer_bmp.total.max_txid_in_slice[h], 0);
						}
					} else {
						if (p->rer_bmp.whole.is_neverwritten_source_parity_for_regen[h]) { // needs to mark neverwritten.
							nbdpec_md_mark_data_never_written_with_dbits(&p->tpd.exp.md[h][i], true, post_recov_dbits);
						} else {
							if (nbdpec_md_was_data_never_written(&p->tpd.exp.md[h][i]))  // needs to calc the edic cause it never calced before.
								_edic = __calc_edic_for_block(p, env, (unsigned char *)p->tpd.pre.blocks[h][i], 0 /* slice offset of parities in rlba is 0 for parity calculations*/, 1/*get snake_size or add to cpr*/, h);
							nvmeibc_block_dp_ec_md_make_p(&p->tpd.exp.md[h][i], _edic, _jri, p->rer_bmp.total.max_txid_in_slice[h], post_recov_dbits, 0);
						}
					}
				}

				// Calc parity dbits state change cause of slice dbits turn on or off in whole stage
				if (p->rer_bmp.whole.slice_dbits_change[h] && is_writable_pari(bit)) {
					if (p->rer_bmp.whole.is_neverwritten_source_parity_for_regen[h]) { // needs to mark neverwritten.
						nbdpec_md_mark_data_never_written_with_dbits(&p->tpd.exp.md[h][i], true, post_recov_dbits);
					} else if (nbdpec_md_was_data_never_written(&p->tpd.exp.md[h][i])) {
						_edic = __calc_edic_for_block(p, env, (unsigned char *)p->tpd.pre.blocks[h][i], 0 /* slice offset of parities in rlba is 0 for parity calculations*/, 1/*get snake_size or add to cpr*/, h);
						nvmeibc_block_dp_ec_md_make_p(&p->tpd.exp.md[h][i], _edic, JRI_MARK_NO_JOURNAL, p->rer_bmp.total.max_txid_in_slice[h], post_recov_dbits, 0 /* don't care - not checking jentry anyway*/);
					} else {
						nvmeibc_block_dp_ec_md_fill_p_with_dbits(&p->tpd.exp.md[h][i], post_recov_dbits);
					}
				}

				// Calc blockset sync results
				if (p->rer_bmp.nwhole.syn_blkst_dmd_change & bit)
					ec_tx_calc_dmd_blkset_sync_results(p, i, post_recov_dbits, env, p->rer_bmp.nwhole.is_neverwritten_slice[h], h);
			}

			p->tpd.post_recov.md[h][i] = p->tpd.exp.md[h][i];  // Save post recov md state.

			// Calc rer tx results
			{
				// Calc blockset sync results
				if (p->rer_bmp.tx.syn_blkst_dmd_change[h] & bit)
					ec_tx_calc_dmd_blkset_sync_results(p, i, rer_slice_dbits_after_nwhole, env, p->rer_bmp.tx.is_neverwritten_slice[h], h);  // The dbits here might be the ones that will after rer write but it's

				if (p->blkset->last_tx.is_wraparound && ((~p->rer_bmp.topo.dead) & bit))
					ec_tx_calc_dmd_blkset_wraparound_results(p, i, rer_slice_dbits_after_wrap, p->rer_bmp.tx.is_neverwritten_slice[h], h);

				// Calc write results
				if ((p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE) && ((p->inp.rer.tx_bm[h]) & (~p->rer_bmp.topo.dead) & bit)) {
					u32 dont_care = 0;  // We can't know here the chosen jentry or jri.
					if (i < slice_size) {
						nvmeibc_block_dp_ec_md_make_d(&p->tpd.exp.md[h][i], p->tpd.rer.md[h][i].D.edic, dont_care, p->lid.rer.blkset_info.bits.txid, dont_care);
					} else {
						const union nvmeibc_dbits_entry rer_binfo_dbits = {.all_bits = p->lid.rer.blkset_info.bits.dirty};
						nvmeibc_block_dp_ec_md_make_p(&p->tpd.exp.md[h][i], p->tpd.rer.md[h][i].P.edic, dont_care, p->lid.rer.blkset_info.bits.txid, rer_binfo_dbits, dont_care); // In HTR with rer bio, the final dmd' dbits equal to ram dbits, because no dbits in other slices.
					}
				}
			}
		}
	}
}

// Alternative function for strategy calculation

//since GENMASK from 0 to 0 is actually 1 we must return 0 for it
static inline u32 genmask_or_zero(const u32 count, const u32 start)
{
	if (count) { // start at "start" and set all bits until count+start-1
		return GENMASK(start + count - 1, start);
	} else return 0;
}

enum nvmeibc_pari_calc_strategy {
	NVMEIBC_STRAT_FULL_SLICE = 1,    // Genrerate P & Q from all data segs.
	NVMEIBC_STRAT_UPDATE,        // Pnew = Pold - Dold + Dnew
	NVMEIBC_STRAT_RESTORE,	     // only when no redundancy, Restore degraded data segs from the rest and then the same as NVMEIBC_STRAT_UPDATE.
	NVMEIBC_STRAT_NO_PARI_CALC,  // For now only on double degraded parities.
};

// EC-5585 - revamp to Snake or replace entirely
static enum nvmeibc_pari_calc_strategy TEST_ONLY_calc_pari_calc_strategy_t(const int slice_size, const u32 n_pari,
												 const struct nvmeibc_roles_bmps bmps,
												 const bool has_writable_pari,
												 const bool dgrd_sgmnt_in_bio,
												 const int first_cmd, const int n_bio_cmds) {

	const int n_bio_and_parity_cmds = n_pari + n_bio_cmds;
	const int n_complement_cmds = slice_size - n_bio_cmds;
	const bool has_dgrd_parity = 0 != (nvmeibc_get_nonrw_roles(bmps) & bmps.raid.pari);

	if (likely(bmps.readable == bmps.raid.all)) {
		if (n_complement_cmds <= n_bio_and_parity_cmds)
			return NVMEIBC_STRAT_FULL_SLICE;
		else
			return NVMEIBC_STRAT_UPDATE;
	} else if (bmps.has_protection) {	// 1-degraded raid-6 (1 redundancy exists)
		if (unlikely(has_dgrd_parity)) { // Single parity degraded
			if (bmps.w) { // Cannot update must full slice
				return NVMEIBC_STRAT_FULL_SLICE;
			// EC-5585:
			// Our assembly code for updating reed solomon parities requires both parities as inputs, we cannot supply a null value and only update one
			// Until our MSSA implmentation in order to cope with the problem, the strategy was inefficiently NVMEIBC_STRAT_NO_PARI_CALC read all available segments and calculate a single new Parity
			// With MSSA, we add a single fictitious block for the dead parity and update the remaining one with the minimal amount of reads
			} else if (bmps.dead) { // We can now choose the most efficient strategy to minimize reads
				if (n_complement_cmds <= n_bio_and_parity_cmds - 1)
					return NVMEIBC_STRAT_FULL_SLICE;
				else
					return NVMEIBC_STRAT_UPDATE;
			} else {
				BUG();
				return NVMEIBC_STRAT_NO_PARI_CALC;
			}
		} else { //have degraded data
			if (likely(dgrd_sgmnt_in_bio)) //cannot use (P - BIOold + BIOnew) && also avoiding 2 step parity calculations
				return NVMEIBC_STRAT_FULL_SLICE;
			else
				return NVMEIBC_STRAT_UPDATE;
		}
	} else { // No redundancy (1-deg raid5 or 2-deg raid-6)
		const bool all_parities_dgrd = (nvmeibc_get_nonrw_roles(bmps) & bmps.raid.pari) == bmps.raid.pari;
		if (all_parities_dgrd){
			if (has_writable_pari) {
				return NVMEIBC_STRAT_FULL_SLICE;
			}
			else {
				return NVMEIBC_STRAT_NO_PARI_CALC;//no journal; no need to deal with parities; no preread
			}
		} else if (likely(has_dgrd_parity)){//dgrd=(data, parity) - there is journal
			const bool parity_is_dead = bmps.raid.pari & bmps.dead;
			const u32 bio_mask = genmask_or_zero(n_bio_cmds, first_cmd);
			const bool dgrd_bio_mask = (nvmeibc_get_nonrw_roles(bmps) & bio_mask);
			//const bool data_is_dead = ioa->dead_roles_map & GENMASK(slice_size-1,0);
			if (dgrd_bio_mask) { // Cannot update regardless if parity is writable
				if (n_complement_cmds <= n_bio_and_parity_cmds-1)
					return NVMEIBC_STRAT_FULL_SLICE;
				else
					return NVMEIBC_STRAT_FULL_SLICE;//(parity_is_dead) NVMEIBC_STRAT_RESTORE; // __GF_UPDATE_STRATEGY__;
			} else {
				if (parity_is_dead) { // We don't need to update this parity and can update the other one
					return NVMEIBC_STRAT_UPDATE;
				}
				return NVMEIBC_STRAT_RESTORE;
			}
		} else {//only data is degraded
			const u32 bio_mask = genmask_or_zero(n_bio_cmds, first_cmd);
			const bool dgrd_bio_mask = (nvmeibc_get_nonrw_roles(bmps) & bio_mask);
			if (!dgrd_bio_mask){//bio does not need degraded segments
				EC_TX_BUG_ON(dgrd_sgmnt_in_bio);
				return NVMEIBC_STRAT_UPDATE;
			} else if (hweight32(dgrd_bio_mask) == 1 && hweight32(nvmeibc_get_nonrw_roles(bmps)) > 1) { //degraded sgmnt is written and the other one is not and we don't have its data
				EC_TX_BUG_ON(!dgrd_sgmnt_in_bio);
				return NVMEIBC_STRAT_RESTORE;
			} else {//both data are dead and in bio
				EC_TX_BUG_ON(!dgrd_sgmnt_in_bio);
				return NVMEIBC_STRAT_FULL_SLICE;
			}
		}
	}
}

static void ec_tx_calc_rer_tx_bmps(struct t_ec_recov_tx *p)
{
	const struct disk_range *pr = p->inp.sraid.cpr;
	struct t_tx_recoverer_bmps *rer_bmp = &p->rer_bmp;
	bool will_call_nwhole_sync_because_of_dbits;
	bool will_call_nwhole_sync_because_of_readfail = false;
	bool will_turnoff_dbits;
	roles_bmp_t turn_off_dbits_bmp;
	roles_bmp_t regen_bkw_bmp;
	u32 h;
	if (p->inp.ree.is_old_completed) {  // can be affected from recovery calling to nwhole sync or from rer's tx calling it.
		will_turnoff_dbits = p->blkset->nwhole.will_turnoff_dbits;
		turn_off_dbits_bmp = p->blkset->nwhole.turnoff_dbits_bmp;
		regen_bkw_bmp = p->blkset->nwhole.regen_bkw_bmp;
	} else {  // in last Tx needs to really calculate here just the results of nwhole called by rer's TX.
		const union nvmeibc_dbits_entry ram_dbits = {.all_bits= p->lid.post_recov.blkset_info.bits.dirty};
		const u32 post_recovery_rebuild_bmp_pr = calc_dbit_rebuild_bm(p, ram_dbits, &p->rer_bmp.topo);
		const roles_bmp_t post_recovery_rebuild_bmp = rol32_width(post_recovery_rebuild_bmp_pr, pr->replicas - __get_slice_start_seg(p), pr->replicas);
		p->blkset->nwhole.turnoff_dbits_bmp = post_recovery_rebuild_bmp | p->rer_bmp.nwhole.ram_dbits_turnof;
		regen_bkw_bmp = 0;  // No regen bkw possible after the recovery.
		turn_off_dbits_bmp = post_recovery_rebuild_bmp;
		will_turnoff_dbits = !!post_recovery_rebuild_bmp;
		p->blkset->nwhole.will_turnoff_dbits = will_turnoff_dbits || p->rer_bmp.nwhole.ram_dbits_turnof;
		p->blkset->nwhole.ram_dbits_turnon_on_turnoff = 0;
		if (will_turnoff_dbits) {
			p->blkset->nwhole.ram_dbits_turnon_on_turnoff |= rer_bmp->topo.raid.pari & rer_bmp->topo.dead; /* Turn on dead parities if we have turnoff */
		}
		p->blkset->nwhole.ram_dbits_turnon_on_turnoff |= p->rer_bmp.nwhole.ram_dbits_turnon_on_turnoff;
		p->blkset->nwhole.will_turnon_dbits = (!!p->rer_bmp.nwhole.roll_bkw) || (!!p->blkset->nwhole.ram_dbits_turnon_on_turnoff);
		p->blkset->nwhole.regen_bkw_bmp = p->rer_bmp.nwhole.regen_bkw;
	}

	if (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE) {
		will_call_nwhole_sync_because_of_dbits = will_turnoff_dbits;
		if (1) {
			for (h = 0; h < p->inp.rer.tx_height; h++) {
				bool dgrd_sgmnt_in_bio = p->inp.rer.tx_bm[h] & (~p->rer_bmp.topo.readable);
				bool has_bad_sec_in_txbm = (p->inp.rer.tx_bm[h]) & (p->rer_bmp.bad_sec_bmp[h]);
				bool has_bad_sec_outside_txbm = (~p->inp.rer.tx_bm[h]) & (p->rer_bmp.bad_sec_bmp[h]);
				const unsigned long rer_tx_bm = p->inp.rer.tx_bm[h];
				enum nvmeibc_pari_calc_strategy strategy = TEST_ONLY_calc_pari_calc_strategy_t(pr->slice_size, hweight32(p->rer_bmp.topo.raid.pari),
															p->rer_bmp.topo,
															p->rer_bmp.topo.has_writable_pari,
															dgrd_sgmnt_in_bio,
															(u32)find_first_bit(&rer_tx_bm, pr->replicas), hweight32((p->inp.rer.tx_bm[h]) & (p->rer_bmp.topo.raid.data)));

				switch (strategy) {
				case NVMEIBC_STRAT_FULL_SLICE:
					will_call_nwhole_sync_because_of_readfail |= has_bad_sec_outside_txbm;
					break;
				case NVMEIBC_STRAT_RESTORE:
					will_call_nwhole_sync_because_of_readfail |= true;  // restore happens when there is no redudency so destroy slice will happen when calling dbits rebuild.
					break;
				case NVMEIBC_STRAT_UPDATE:
					will_call_nwhole_sync_because_of_readfail |= has_bad_sec_in_txbm;
					break;
				case NVMEIBC_STRAT_NO_PARI_CALC:
					will_call_nwhole_sync_because_of_readfail |= false;
					break;
				default: BUG();
				}
			}
		}
	} else if (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_READ) {
		roles_bmp_t read_segs_bmp;
		for (h = 0; h < p->inp.rer.tx_height; h++) {
			bool is_restoraion_need = !!((p->inp.rer.tx_bm[h] & rer_bmp->topo.raid.data) & (~rer_bmp->topo.readable));
			if (is_restoraion_need) {  // If trying to read from a degraded seg it will try to read all of the other segs and restore it.
				u32 n_non_rw = hweight32((~rer_bmp->topo.readable) & p->rer_bmp.topo.raid.all);
				u32 readen_parity_mask = GENMASK(pr->slice_size, pr->slice_size + n_non_rw - 1);
				read_segs_bmp = rer_bmp->topo.raid.data | readen_parity_mask;
			} else {
				read_segs_bmp = p->inp.rer.tx_bm[h] & p->rer_bmp.topo.raid.data;
			}
			will_call_nwhole_sync_because_of_readfail = will_call_nwhole_sync_because_of_readfail || (!!(read_segs_bmp & rer_bmp->bad_sec_bmp[h]));
		}
		will_call_nwhole_sync_because_of_dbits = false;
	} else {  // p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_NOP
		will_call_nwhole_sync_because_of_dbits = false;
		will_call_nwhole_sync_because_of_readfail = false;
	}

	if (p->blkset->last_tx.is_wraparound && p->blkset->nwhole.bad_sec_bmp)
		will_call_nwhole_sync_because_of_readfail = true;
	rer_bmp->tx.will_call_nwhole_sync = will_call_nwhole_sync_because_of_dbits || will_call_nwhole_sync_because_of_readfail || p->blkset->will_nwhole_sync_called_on_blockset;

	for (h = 0; h < p->inp.tx_height; h++) {
		rer_bmp->tx.fixed_bad_sec[h] = 0;
		if (rer_bmp->tx.will_call_nwhole_sync)
			rer_bmp->tx.fixed_bad_sec[h] = rer_bmp->bad_sec_bmp[h];
		else if (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE && h < p->inp.rer.tx_height)
			rer_bmp->tx.fixed_bad_sec[h] = rer_bmp->bad_sec_bmp[h] & p->inp.rer.tx_bm[h];
	}

	for (h = 0; h < p->inp.tx_height; h++) {
		rer_bmp->tx.syn_blkst_data_change[h] = 0;
		rer_bmp->tx.syn_blkst_dmd_change[h] = 0;
		if (rer_bmp->tx.will_call_nwhole_sync) {
			rer_bmp->tx.syn_blkst_data_change[h] = turn_off_dbits_bmp | rer_bmp->bad_sec_bmp[h] | regen_bkw_bmp;
			rer_bmp->tx.syn_blkst_dmd_change[h] = rer_bmp->tx.syn_blkst_data_change[h];
			p->blkset->nwhole.is_sl_by_sl = (hweight32(p->blkset->nwhole.bad_sec_bmp | (~p->rer_bmp.topo.readable & p->rer_bmp.topo.raid.all)) > hweight32(p->rer_bmp.topo.raid.pari));
			if (!p->blkset->nwhole.is_sl_by_sl)
				rer_bmp->tx.syn_blkst_dmd_change[h] |= p->blkset->nwhole.bad_sec_bmp; // if not in sbs mode than dmd will be changed in blockset's segs granularity.
			if (will_turnoff_dbits || (p->inp.ree.is_old_completed && p->blkset->nwhole.will_turnon_dbits)) {
				EC_TX_BUG_ON(!p->inp.ree.is_old_completed && !rer_bmp->tx.syn_blkst_dmd_change[h]);
				rer_bmp->tx.syn_blkst_dmd_change[h] |= (nvmeibc_raid1_get_parities_bmp(pr) & (~p->rer_bmp.topo.dead));
			}
		}
	}

	for (h = 0; h < p->inp.tx_height; h++) {
		const roles_bmp_t bad_sectors_affect_on_nwhole_sync_regen = p->blkset->nwhole.is_sl_by_sl ? p->rer_bmp.bad_sec_bmp[h] : p->blkset->nwhole.bad_sec_bmp;
		bool is_valid_parity = ((p->rer_bmp.topo.raid.pari) & (p->rer_bmp.topo.readable) & (~p->ree_bmp.dmd_writn[h]) & (~bad_sectors_affect_on_nwhole_sync_regen));
		p->rer_bmp.tx.is_neverwritten_readable_parity[h] = p->inp.pre.is_never_written[h] && is_valid_parity;
		rer_bmp->tx.is_neverwritten_source_parity_for_regen[h] = p->rer_bmp.tx.is_neverwritten_readable_parity[h] && rer_bmp->whole.is_neverwritten_source_parity_for_regen[h] && rer_bmp->nwhole.is_neverwritten_source_parity_for_regen[h];
		{
			bool is_neverwritten_slice_by_data_blocks = p->inp.pre.is_never_written[h] && (((p->rer_bmp.topo.raid.data) & p->rer_bmp.topo.readable & (~p->ree_bmp.dmd_writn[h]) & (~bad_sectors_affect_on_nwhole_sync_regen)) == (p->rer_bmp.topo.raid.data)) && (!p->rer_bmp.whole.will_rollfwd[h]);
			rer_bmp->tx.is_neverwritten_slice[h] = rer_bmp->tx.is_neverwritten_source_parity_for_regen[h]  || is_neverwritten_slice_by_data_blocks;
		}
	}
}

static short tested_error_codes[NUMBER_OF_ERROR_CODES] = { EPERM_READ_FAIL, EPERM_READ_FAIL_NO_RETRY, NVME_SC_DNR };

static void ec_tx_generate_bad_sectors(struct test_context env, struct t_ec_recov_tx *p) {
	const struct disk_range *pr = env.sraid.cpr;
	u32 max_bad_sec_in_slice_without_destory = (pr->replicas - pr->slice_size) - hweight32(~p->rer_bmp.topo.readable & p->rer_bmp.topo.raid.all);
	u32 i, bit, h;

	for (h = 0; h < p->inp.tx_height; h++) {

		p->rer_bmp.bad_sec_bmp[h] = 0;

		if (!p->inp.nat.allow_bad_sectors)
			continue;

		if (max_bad_sec_in_slice_without_destory != 0)
			p->rer_bmp.bad_sec_bmp[h] = __rand_n_bits_bitmap(max_bad_sec_in_slice_without_destory, pr->replicas) & (p->rer_bmp.topo.readable);

		for (i = 0, bit = 1; i < pr->replicas; i++, bit = (1 << i)) {
			if (p->rer_bmp.bad_sec_bmp[h] & bit)
				ramDiskSimulator_do_bad_sector_with_ptr((u64*)p->tpd.nat.blocks[h][i], (u64*)(&p->tpd.nat.md[h][i]), tested_error_codes[rand() % NUMBER_OF_ERROR_CODES]);
		}

		p->blkset->nwhole.bad_sec_bmp |= p->rer_bmp.bad_sec_bmp[h];
	}
}

void ec_tx_calc_should_inject_unknown_dbits_to_ram(struct t_ec_recov_tx *p) {
	if (!p->inp.ree.is_old_completed) {
		p->blkset->is_unknown_txid_injected_to_ram = false;
		p->blkset->is_unknown_dbits_injected_to_ram = false;
		if (p->inp.rer.io_perm.bits.is_hot_recovery) {
			if ((!p->blkset->nwhole.bad_sec_bmp) && ((!p->inp.nat.is_journal_overriden) || p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE)) {
				bool is_dbits_in_binfo_equal_to_dmd = (((p->rer_bmp.can_see_all_ree_dbits_in_slice && p->rer_bmp.can_see_all_ree_dbits_in_ram) || ((!p->rer_bmp.can_see_any_ree_dbits_in_slice) && (!p->rer_bmp.can_see_any_ree_dbits_in_ram))) && (p->pre.ram_dbits == p->pre.slice_dbits_union));
				/*
				 (!p->pre.ram_dconv): because in simulator there are dconv on non W- segs so resolve dbits will not set dconv in ram.
				!(p->rer_bmp.topo.wm & (~p->pre.ram_dconv)): because reolve dbits will automatically add dconv to wm seg   		   .
				(p->inp.nat.is_journal_overriden || (p->rer_bmp.topo.has_rw_pari)): because if not rw parity resolve will automatically turnon dbits on dbits in this case. 																												   .
				*/
				if (is_dbits_in_binfo_equal_to_dmd && (!p->pre.ram_dconv) && (!(p->rer_bmp.topo.wm & (~p->pre.ram_dconv))) && (p->inp.nat.is_journal_overriden || (p->rer_bmp.topo.has_rw_pari))) {
					p->blkset->is_unknown_dbits_injected_to_ram = rand()%2;
				}
			}
		}
	}

	p->blkset->is_double_deg_and_deg_parity = (hweight16(p->rer_bmp.topo.raid.all & (~p->rer_bmp.topo.readable)) > 1) && (p->rer_bmp.topo.raid.pari & (~p->rer_bmp.topo.readable));
	p->blkset->resolve_dbits.turnon_all_deg_segs = false;
	if ((p->inp.rer.io_perm.bits.is_cold_recovery || p->blkset->is_unknown_dbits_injected_to_ram) && p->blkset->is_double_deg_and_deg_parity) {
		p->blkset->resolve_dbits.turnon_all_deg_segs = true;
	}

}

void ec_tx_update_nwhole_bmps_after_dbits_unkown_injection(struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	u32 h;
	if (p->blkset->resolve_dbits.turnon_all_deg_segs && p->rer_bmp.can_turnof_ram_dbits) {
		// Need to update which blocks will be regened by nwhole sm.
		p->rer_bmp.nwhole.ram_dbits_turnof |= (p->rer_bmp.topo.w);
		p->rer_bmp.nwhole.regen |= p->rer_bmp.nwhole.ram_dbits_turnof; // If Found transaction and can clean dbits, then data changed and dbits rebuild/ rollback (double deg parities) is needed for parity segs and data fixed segs for full blockset
		p->rer_bmp.nwhole.syn_blkst_data_change |= p->rer_bmp.nwhole.regen;
		p->rer_bmp.nwhole.syn_blkst_dmd_change |= p->rer_bmp.nwhole.syn_blkst_data_change;
		if (p->rer_bmp.nwhole.ram_dbits_turnof) { // This is correct because we don't inject bad sectors when there is nwhole called by htr
			p->rer_bmp.nwhole.ram_dbits_turnon_on_turnoff = p->rer_bmp.topo.raid.pari & p->rer_bmp.topo.dead; /* Turn on dead parities if we have turnoff */
			p->rer_bmp.nwhole.syn_blkst_dmd_change |= (nvmeibc_raid1_get_parities_bmp(pr) & (~p->rer_bmp.topo.dead)); // Paritie's md allways changes if there is dbits turnon/off
		}
		for (h = 0; h < p->inp.tx_height; h++) {
			p->rer_bmp.total.slice_data_touched[h] |= p->rer_bmp.nwhole.syn_blkst_data_change;
		}
		p->rer_bmp.nwhole.will_call_nwhole_sync |= (!!p->rer_bmp.topo.w);
	}
}

void ex_tx_inject_tx(struct NVMeshSystem *sys, struct t_ec_recov_tx *p) {
	struct test_context env = {.sys = sys, .client = sys->clients,.dev = sys->clients->devs[p->inp.sraid.vsi.volume], .sraid = p->inp.sraid};
	ec_tx_gen_rer_and_ree_topo_bmps(p);
	ec_tx_init_jrnl_candidates(env, p);
	ec_tx_init_injection_ptrs(env, p);
	ec_tx_gen_pre_tx_dbits_bmps(p);
	ec_tx_sync_apply_abrt_stage_to_txbm(p); 				// Adjust bitmaps per scenario
	ec_tx_init_bio_ptrs(env, p);
	ec_tx_for_txbm_jrnl_do(sys, p, "Abandon");
	ec_tx_generate_bad_sectors(env, p);
	ec_tx_calc_should_inject_unknown_dbits_to_ram(p);
	ec_tx_update_nwhole_bmps_after_dbits_unkown_injection(p);
	ec_tx_calc_blkset_entries(p);						// Calc RAM injection.
	ec_tx_calc_rer_tx_bmps(p);
	ec_tx_calc_expected_blkset_entries(p);						// Calc RAM expected results.
	ec_tx_calc_dmd(p, &env);
	ec_tx_calc_expected_blkst_recov_msgs(p);
	//ec_tx_notify_toma_on_fixed_bad_sec(env, p); //LKJ: On full-blockset Toma doesn't notify on bad sectors currently
	ec_tx_inject_blkset_entries(p);
	ec_tx_inject_transaction_to_ssds(p, env.dev->dp.enable_di_debug_mode);			// Must be after __calc_blkset_entry_of_tx() to know which dbits inject into parity metadata
	ec_tx_for_jmdc_do(sys, p, "Inject vals from JMDD");
	__drain_all_prev_serjio_communication_with_jam(sys, p);	// Prevent contention with 'sync'
	ec_tx_calc_expected_flow_counters(p);
	ec_tx_calc_expected_ssds_state(p, &env);
}

static void __verify_single_tx_in_blkset_correct_flow(struct t_ec_recov_tx *p, bool is_unexpected_htr_executed) {
	if (p->inp.rer.io_perm.bits.is_hot_recovery && (!p->inp.ree.is_old_completed)){ // In htr the counters only counted for the last Tx.
		struct htr_stats st;
		int n_total_regen;
		u32 h;
		nvmeibc_htr_stats_get(&st);
		n_total_regen = atomic_read(&st.n_regen_bkw) + atomic_read(&st.n_regen_fwd) + atomic_read(&st.n_regen_bkw_no_pari);
		if (p->inp.rer.io_perm.bits.is_hot_recovery) {
			int n_expecred_calls = p->flow.htr_process_me ? 1 : 0;
			EC_TX_BUG_ON(!is_unexpected_htr_executed && (atomic_read(&st.n_calls) != n_expecred_calls));
		}
		EC_TX_BUG_ON(!!atomic_read(&st.n_dbits_rebuild)     != !!p->rer_bmp.nwhole.ram_dbits_turnof);
		for (h = 0; h < p->inp.tx_height; h++) {
			if (is_unexpected_htr_executed) {
				EC_TX_BUG_ON(p->rer_bmp.roll_fwd[h] && (!atomic_read(&st.n_roll_fwd)));
				EC_TX_BUG_ON(p->rer_bmp.whole.roll_bkw[h] && (!atomic_read(&st.n_roll_bkw_by_dbits_turnon)));
				EC_TX_BUG_ON(p->rer_bmp.whole.regen[h] && (!n_total_regen));
			} else {
				EC_TX_BUG_ON(!!atomic_read(&st.n_roll_fwd)  != p->rer_bmp.any_roll_fwd);
				EC_TX_BUG_ON(!!atomic_read(&st.n_roll_bkw_by_dbits_turnon)  != p->rer_bmp.whole.any_roll_bkw);
				EC_TX_BUG_ON(!!n_total_regen  != p->rer_bmp.whole.any_regen);
			}
		}
		nvmeibc_htr_stats_reset();
	}
}

static void __verify_jour_history_correct_flow(struct t_ec_tx_history *hist, bool is_trans_err_injected) {
	struct nvmeibc_cold_stats st;
	struct t_ec_recov_tx *p;
	u32 blkset_ind;
	const bool is_cold = (hist->prop.rec.type == NVMEIBT_RECOVERY_TYPE_EC_COLD), is_jgc = (hist->prop.rec.type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC);
	int i, n_expected_htrs = 0, n_expected_jgcs = 0, n_expected_syncs = 0;
	const u32 mask = (((1<<hist->prop.rec.seg_end)-1) ^ ((1<<hist->prop.rec.seg_start)-1));
	const u32 n_segs = hist->blkst->tx->inp.sraid.cpr->replicas;
	for (blkset_ind = 0; blkset_ind < hist->prop.n_blksets; blkset_ind++) {
		for (i = 0, p = &hist->blkst[blkset_ind].tx[i]; i < hist->prop.n_tx_in_blkset; i++, p++) {
			if (!p->inp.ree.is_old_completed) {		// Only 1 last transaction in each blockset
				if (p->flow.htr_process_me)
					n_expected_htrs++;
				if (is_cold) {		// Cold recovery if there is a journal candidate will always processes, JGC only if lock is unlocked
					struct t_ec_recov_tx *old_tx = &hist->blkst[blkset_ind].tx[i - 1];
					bool has_candidate_for_blkset = ((p->rer_bmp.is_jour_committed && __is_bmp_included_in(p->ree_bmp.jmd_writn_union, p->ree_bmp.abandoned_jent)) || (old_tx->rer_bmp.is_jour_committed && __is_bmp_included_in(old_tx->ree_bmp.jmd_writn_union, old_tx->ree_bmp.abandoned_jent)));
					if (has_candidate_for_blkset)
						n_expected_syncs++;
				} else if (p->lid.rer.lock_id.all == 0) { 		// JGC only if lock is unlocked
					const u32 rwj2d = p->flow.any_j2d & p->rer_bmp.topo.readable;	// Sync will test stale locks only on rw segs, only the first jmd block in the entry determines if jgc will free the entry
					const u32 sync = (rol32_width(rwj2d, __get_slice_start_seg(p), n_segs) & mask); // Bitmap where sync will be activated
					n_expected_syncs += hweight32(sync);
				}
			}
			if (is_jgc) {
				const u32 j2d = p->ree_bmp.jmd_with_valid_chain;   // jgc will free the entry iff it as valid chain on it.
				const u32 j2d_garbage = (p->flow.jgc_free_me) ? j2d : (j2d & (nvmeibc_get_nonrw_roles(p->rer_bmp.topo)));	// All non RW are always freed
				const u32 j2d_freed = (rol32_width(j2d_garbage, __get_slice_start_seg(p), n_segs) & mask);
				n_expected_jgcs += hweight32(j2d_freed);
			}
		}
	}
	nvmeibc_cold_stats_get(&st);
	 // < and not != because: When there are errors in the locking state machine (i.e write binfo) the sync will be called again on the blockset which will inc twice the counter
	if (1) {
		const int n_syncs =     ((!is_jgc) ? atomic_read(&st.n_syncs) : atomic_read(&st.n_jgc_blksets));
		const int n_htr_call =  atomic_read(&st.n_htr_call);
		const int n_jgc_freed = atomic_read(&st.n_jgc_freed);
		if (is_trans_err_injected) {
			EC_TX_BUG_ON(n_syncs      < n_expected_syncs);
			EC_TX_BUG_ON(n_htr_call   < n_expected_htrs);
			EC_TX_BUG_ON(n_jgc_freed  < n_expected_jgcs);
		} else {
			EC_TX_BUG_ON(n_syncs     != n_expected_syncs);
			EC_TX_BUG_ON(n_htr_call  != n_expected_htrs);
			EC_TX_BUG_ON(n_jgc_freed != n_expected_jgcs);
		}
	}

	nvmeibc_cold_stats_reset();
}

void clean_unfixed_bad_sectors(struct t_ec_recov_tx *p) {
	u32 h;
	for (h = 0; h < p->inp.tx_height; h++) {
		ulong i, unfixed_bad_sec = p->rer_bmp.bad_sec_bmp[h] & (~p->rer_bmp.tx.fixed_bad_sec[h]);
		const struct disk_range *pr = p->inp.sraid.cpr;

		for_each_set_bit(i, &unfixed_bad_sec, p->inp.sraid.cpr->replicas) {
			u64 *src_data = p->tpd.ree.blocks[h][i]; // where the us unfixed bad sec the data will always be ree's
			union nvmeibc_block_dp_ec_data_block_md *src_md = &p->tpd.ree.md[h][i];
			memcpy(p->tpd.bptrs[h][i].data, src_data, NVMEIBC_SECTOR_SIZE);
			memcpy(p->tpd.bptrs[h][i].dmd, src_md, sizeof(union nvmeibc_block_dp_ec_data_block_md));
		}

		for (i = 0; i < pr->replicas; i++)
			EC_TX_BUG_ON(ramDiskSimulator_is_block_bad_sector((u64 *)p->tpd.bptrs[h][i].data));
	}
}

/*
static void __verify_toma_not_expecting_bad_sec_fixes(struct NVMeshSystem *sys, struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 slba = p->inp.slba;
	const u32 lockset_owner_seg = (slba >> LOCKSET_SHIFT) % pr->replicas;

	for (u32 i = 0; i < pr->replicas; i++) {
		const raid_sgmnt_t di = (i + lockset_owner_seg) % pr->replicas;     // Convert role to seg
		struct serverSimulator *srv = &sys->servers[di];
		struct tomaSimulator *simToma = &srv->simToma;
		EC_TX_BUG_ON(simToma->expect_io_failure);
		EC_TX_BUG_ON(simToma->fail_mask);
		EC_TX_BUG_ON(simToma->fix_mask);
	}
}
*/

void ec_tx_verify_post_sync_tx(struct NVMeshSystem *sys, struct t_ec_recov_tx *p, bool is_trans_err_injected, bool is_unexpected_htr_executed) {
	__verify_single_tx_in_blkset_correct_flow(p, is_unexpected_htr_executed);		// Coz in cold, all HTR stats are summed and dont reflect a single Tx
	ec_tx_verify_post_tx_ssd_disks_roll_fwd_dbits_fixup(p, is_trans_err_injected);
	//__verify_toma_not_expecting_bad_sec_fixes(sys, p);  //LKJ: On full-blockset Toma doesn't notify on bad sectors currently
	__verify_post_tx_lock_blkset_entry(p, is_trans_err_injected);				// Binfo will be affected only by the latest Tx in blockset (not by old completed transactions
	clean_unfixed_bad_sectors(p);
	//__drain_all_prev_serjio_communication_with_jam(sys, p);	// Prevent contention with 'sync'
	ec_tx_for_txbm_jrnl_do(sys, p, "Clean leaked");				// This is due to the bug with journals that are in TxBM but inaccessible in rer's topology
	//__drain_all_prev_serjio_communication_with_jam(sys, p);	// Prevent contention with 'sync'
}

/* For cold recovery, we dont execute dirty rebuild thorugh valid topologies
   changes so dbits must be deleted from disks at the end of each scenario
   For htr, dbits might be left on dead segs and need to be cleaned*/
static void __invalidate_used_disks_md(struct t_ec_recov_tx* p, const struct disk_range *pr, struct t_slice_tx_ptrs *tpd) {
	u32 h, i;
	for (h = 0; h < p->inp.tx_height; h++) {
		bool is_there_never_written_parity_in_slice = false;
		for (i = pr->slice_size; i < pr->replicas; i++) {
			if (nbdpec_md_was_data_never_written(tpd->bptrs[h][i].dmd))
				is_there_never_written_parity_in_slice = true;
		}
		for (i = 0; i < pr->replicas; i++) {
			if (i < pr->slice_size) {
				if (is_there_never_written_parity_in_slice)
					nbdpec_md_mark_data_never_written_no_dbits(tpd->bptrs[h][i].dmd, false);
			} else if (is_there_never_written_parity_in_slice) {
				nbdpec_md_mark_data_never_written_no_dbits(tpd->bptrs[h][i].dmd, true);
			} else {
				nvmeibc_block_dp_ec_md_set_txid_and_dirty_bits_value_mark_no_journal(tpd->bptrs[h][i].dmd, p->blkset->last_tx.is_wraparound ? 1 : p->rer_bmp.total.max_txid_in_slice[h], zero_dbits);
			}
		}
	}
}

/* Journal recoveries only: A few transactions exist within the same blockset, so
   restored txid is the max of accessible to rer of them all
   HTR: Needs only to figure out if dbits rebuild will be slice by slice or full-blockset*/
static void __merge_rer_binfo_of_recovered_tx_with_completed_tx(struct t_ec_recov_tx *d, struct t_ec_recov_tx *s, struct NVMeshSystem *sys) {
	const struct test_context env = {.sys = sys, .client = sys->clients,.dev = sys->clients->devs[s->inp.sraid.vsi.volume], .sraid = s->inp.sraid};
	union nvmeibc_dbits_entry _d = {.all_bits = d->lid.rer.blkset_info.bits.dirty};
	union nvmeibc_dbits_entry _s = {.all_bits = s->lid.rer.blkset_info.bits.dirty};
	struct dp_topology_traits const topo_traits = {
		.n_parities = __disk_range_get_num_parities(env.sraid.cpr),
	};
	bool has_candidate_for_blkset;
	if (!d->inp.rer.io_perm.bits.is_hot_recovery) {
		has_candidate_for_blkset = (d->rer_bmp.is_jour_committed || s->rer_bmp.is_jour_committed);
		if (!has_candidate_for_blkset){ // No candidate pointing to the blockset, the blockset will stay the same as after the natural disaster
			_d.all_bits = d->lid.nat.blkset_info.bits.dirty;
			_s.all_bits = s->lid.nat.blkset_info.bits.dirty;
			d->lid.rer.blkset_info.bits.txid = d->lid.nat.blkset_info.bits.txid;
		}

		d->lid.rer.blkset_info.bits.dirty = nvmeibc_dbits_merge_owners(&_d, &_s, &topo_traits);
	}


	if (d->inp.rer.io_perm.bits.is_hot_recovery) { // Needs to recalc expected ssd state of old_tx because if last tx called nwhole it's being affected from it (the binfo is being verified only to last TX so no need to update it).
			d->blkset->will_nwhole_sync_called_on_blockset = (d->rer_bmp.tx.will_call_nwhole_sync || d->rer_bmp.nwhole.will_call_nwhole_sync);
			if (d->blkset->will_nwhole_sync_called_on_blockset) {
				ec_tx_calc_rer_tx_bmps(s);
				ec_tx_calc_expected_ssds_state(s, &env);
			}
	}

	if (d->inp.rer.io_perm.bits.is_jgc_recovery) {
		s->lid.rer.lock_id.all = d->lid.rer.lock_id.all;			// For JGC all Tx's in a given blockset has the same fate
		s->flow.jgc_free_me = d->flow.jgc_free_me;
		d->flow.any_j2d = s->ree_bmp.jmd_with_valid_chain | d->ree_bmp.jmd_with_valid_chain;
	}
}

/*
static void ec_tx_notify_toma_on_fixed_bad_sec(struct test_context env, struct t_ec_recov_tx *p) {
	const struct disk_range *pr = p->inp.sraid.cpr;
	const u32 slba = p->inp.slba;
	const u32 slice_index_bmp = (1 << (slba % LOCKSET_SLICES));
	const u32 lockset_owner_seg = (slba >> LOCKSET_SHIFT) % pr->replicas;

	if (p->rer_bmp.tx.will_call_nwhole_sync && p->rer_bmp.bad_sec_bmp) {
		for (u32 i = 0; i < pr->replicas; i++) {
			const raid_sgmnt_t di = (i + lockset_owner_seg) % pr->replicas;     // Convert role to seg
			struct serverSimulator *srv = &env.sys->servers[di];
			tomaSimulator_expectIOFailure(&srv->simToma, *((short *)p->tpd.nat.blocks[i]), 0, slice_index_bmp);
		}
	}
}
*/


static void __update_tomas_to_expectIOFailure(struct NVMeshSystem *sys, struct t_ec_recov_blkset *blkst) {
	const struct disk_range *pr = blkst->tx->inp.sraid.cpr;
	u32 role, replicas = pr->replicas;
	const u32 lockset_owner_seg = (blkst->tx->inp.slba >> LOCKSET_SHIFT) % pr->replicas;
	const ulong non_dead = (blkst->tx->rer_bmp.topo.raid.all) & (~blkst->tx->rer_bmp.topo.dead);
	u32 fixed_slices_bmp = 0;
	//const ulong bad_sector = blkst->nwhole.bad_sec_bmp;

	for (ulong i = 0; i < sizeof(blkst->tx)/sizeof(blkst->tx[0]); ++i) {
		struct t_ec_recov_tx *p = &blkst->tx[i];
		const u32 slba = p->inp.slba;
		u32 h;
		for (h = 0; h < p->inp.tx_height; h++) {
			if (p->rer_bmp.bad_sec_bmp[h]) {
				fixed_slices_bmp |= (1u << ((slba + h) % LOCKSET_SLICES));
			}
		}
	}

	for_each_set_bit(role, &non_dead, replicas) {
		const raid_sgmnt_t seg = (role + lockset_owner_seg) % replicas;     // Convert role to seg
		tomaSimulator_expectIOFailure(&sys->servers[seg].simToma, EPERM_READ_FAIL, 0, fixed_slices_bmp);
	}
}

static void __inject_full_recov_history(struct NVMeshSystem *sys, struct t_ec_tx_history *hist) {
	int i, b;
	for (b = 0; b < hist->prop.n_blksets; b++) {
		for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
			struct t_ec_recov_tx *p = &hist->blkst[b].tx[i];
			if (!p->inp.ree.is_old_completed && p->inp.rer.io_perm.bits.is_hot_recovery) {
				const struct disk_range *pr = p->inp.sraid.cpr;
				struct t_ec_recov_tx *old_tx = p-1;
				union nvmeibc_dbits_entry e = { .all_bits = old_tx->lid.post_recov.blkset_info.bits.dirty };
				struct dp_topology_traits const topo_traits = {
					.n_degraded = ec_tx_calc_topo_ree_num_deg_segs(p),
				};

				p->inp.pre.history_ram_dbits = rol32_width(nvmeibc_dbits_get_turn_on_bmp(&e, &topo_traits), pr->replicas - __get_slice_start_seg(p), pr->replicas);
			} else {
				p->inp.pre.history_ram_dbits = 0;
			}
			ex_tx_inject_tx(sys, p);
			if (!p->inp.ree.is_old_completed)
				__merge_rer_binfo_of_recovered_tx_with_completed_tx(p, p-1, sys);	// Daniel: Ugly, todo, make this generic for any amount of Tx's, not just 2 in blockset
		}

		if (hist->blkst[b].nwhole.is_sl_by_sl) {
			__update_tomas_to_expectIOFailure(sys, &hist->blkst[b]);
		}
	}
}

static void __update_expectors_according_to_err_injection(struct t_ec_tx_history *hist) {
	int i, b;
	u32 h;
	u32 role;

	for (b = 0; b < hist->prop.n_blksets; b++) {
		for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
			struct t_ec_recov_tx *p = &hist->blkst[b].tx[i];
			struct disk_range *pr = p->inp.sraid.cpr;
			bool all_parities_non_readable_and_wrtable_exists = (!hist->blkst->tx->rer_bmp.topo.has_rw_pari && hist->blkst->tx->rer_bmp.topo.has_writable_pari);
			if (p->inp.rer.io_perm.bits.is_hot_recovery
				&& p->inp.ree.is_old_completed
				&& hist->prc.is_unexpected_htr_executed
				&& all_parities_non_readable_and_wrtable_exists) {  // nwhole will be called in order to regen w parities and turnon dbits on for d parities.
				ulong *writable_pari = sim_kmalloc(sizeof(ulong), GFP_KERNEL);  // malloced because compiler in release mode has bug so he complains about possible array out of bound which never occurs.
				roles_bmp_t dead_pari = p->rer_bmp.topo.raid.pari & (p->rer_bmp.topo.dead);
				sgmnts_bmp_t dead_pari_pr = rol32_width(dead_pari, __get_slice_start_seg(p), pr->replicas);
				struct nvmeibc_dbits_tx db_tx;
				union nvmeibc_dbits_entry empty_dbits = { .all_bits = 0 };  // All parities degraded at the end of nwhole sync onnly on the dead parities there will be a dbit.
				union nvmeibc_dbits_entry post_dbits;
				struct dp_topology_traits topo_traits = {
					.n_parities = __disk_range_get_num_parities(pr),
				};
				*writable_pari = p->rer_bmp.topo.raid.pari & (~p->rer_bmp.topo.dead);

				nvmeibc_dbits_tx_init_by_bmp(&db_tx, &topo_traits, dead_pari_pr, 0x0 /* No trun-off*/, 0);
				post_dbits.all_bits = nvmeibc_dbits_tx_apply(&empty_dbits, &db_tx);

				for (h = 0; h < p->inp.tx_height; h++) {
					for_each_set_bit(role, writable_pari, pr->replicas) {
						nvmeibc_block_dp_ec_md_set_txid_and_dirty_bits_value_mark_no_journal(&p->tpd.exp.md[h][role], p->tpd.exp.md[h][role].tx_id, post_dbits); // We don't know if last tx write failed before incrementing the txid or not so taking the lowest possible one.
					}
				}
				sim_kfree(writable_pari);
			}
		}
	}
}

static struct t_ec_recov_tx *get_latest_tx_in_blkset(struct t_ec_recov_blkset *blkset) {
	const u32 n_txs_in_blkset = sizeof(blkset->tx)/ sizeof(blkset->tx[0]);
	EC_TX_BUG_ON(n_txs_in_blkset != 2);
	EC_TX_BUG_ON(blkset->tx[n_txs_in_blkset - 1].inp.ree.is_old_completed);
	return &blkset->tx[n_txs_in_blkset - 1];
}

static void __calc_post_recovery_conclusions(struct t_ec_tx_history *hist, bool is_trans_err_injected) {
	bool is_htr = hist->prop.rec.type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD;
	hist->prc.is_unexpected_htr_executed = false;
	if (is_htr) {
		struct htr_stats st;
		struct t_ec_recov_tx *last_tx_in_blkset = get_latest_tx_in_blkset(hist->blkst);  // In htr there is only one blkset tested.
		int n_expected_calls = (!last_tx_in_blkset->inp.nat.is_journal_overriden) ? 1 : 0;  // The only time when htr won't be called is when journal is overriden and this is because in that case we don't inject stale lock.
		int n_calls;
		nvmeibc_htr_stats_get(&st);
		n_calls = atomic_read(&st.n_calls);
		EC_TX_BUG_ON((!is_trans_err_injected) && (n_calls != n_expected_calls));
		hist->prc.is_unexpected_htr_executed = (n_calls != n_expected_calls);
	}
}


static inline u32  __get_protect_lvl(struct t_ec_recov_tx *p)
{return p->inp.sraid.cpr->replicas - p->inp.sraid.cpr->slice_size; }


static inline bool __should_validate_nwhole_barrier(struct t_ec_tx_history *hist) {
	struct t_ec_recov_tx *last_tx = &hist->blkst[0].tx[hist->prop.n_tx_in_blkset - 1];  // in HTR we are using just 1 blockset.

	if (last_tx->inp.rer.io_perm.bits.is_hot_recovery && last_tx->inp.nat.is_journal_overriden && last_tx->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE && (!hist->blkst->nwhole.ram_dbits_turnon_on_turnoff) && (!last_tx->rer_bmp.nwhole.ram_dbits_turnon_on_turnoff)) { // Only in journal overwrriden and rer is writing nwhole sync is the first thing that being called (cause the is no stale lock injected).)
		return true;
	}

	return false;
}

bool is_time_for_check_nwhole_pmd_dbits_barrier(struct nvmeibc_block_command *cmd) {
	struct nvmeibc_block_command *rldr = cmd->cmdarr;
	struct recovery_sync_op *so = rldr->o->rso;

	if (so->stage == sync_stage_recov_no_write_hole_sent_restore_data_and_turnon_parity_md_dbits_done &&
		so->nwhole_exec_plan.should_turnoff_dbits) {
		WARN_ON(nvmeibc_atomic_read(&rldr->n_uncompleted_cmds) != 0);	// only called when all stage cmds complete
		return true;
	}

	return false;
}

void ec_tx_verify_nwhole_barrier(struct t_ec_recov_tx *p) {
	struct disk_range *pr = p->inp.sraid.cpr;
	ulong *readable_parities_bmp = sim_kmalloc(sizeof(ulong), GFP_KERNEL);  // malloced because compiler in release mode has bug so he complains about possible array out of bound which never occurs.
	u32 role, h;
	for (h = 0; h < p->inp.tx_height; h++) {
		ulong bad_sector_cmds_on_nwhole_read = p->blkset->nwhole.is_sl_by_sl ? p->rer_bmp.bad_sec_bmp[h] : p->blkset->nwhole.bad_sec_bmp;
		*readable_parities_bmp = p->rer_bmp.topo.raid.pari & p->rer_bmp.topo.readable & (~bad_sector_cmds_on_nwhole_read);
		for_each_set_bit(role, readable_parities_bmp, pr->replicas) {  // By design we allow to turnoff non-readable parities dbits - so don't verify them.
			EC_TX_BUG_ON(p->tpd.bptrs[h][role].dmd->P.dbits_0 != p->tpd.slice_starting_state.md[h][role].P.dbits_0);
			EC_TX_BUG_ON(p->tpd.bptrs[h][role].dmd->P.dbits_1 != p->tpd.slice_starting_state.md[h][role].P.dbits_1);
		}
	}
	sim_kfree(readable_parities_bmp);
}

int check_nwhole_pmd_dbits_barrier(struct nvmeibc_disk_hook_args *hook_args, struct nvmeibc_block_command *cmd) {
	struct t_ec_recov_tx *p;
	struct t_ec_tx_history *__hist = hook_args->iocmd_comp_args.hist;
	struct t_ec_recov_blkset *blkst = __hist->blkst;  // In htr we are using only the first blkst
	u32 i;
	(void)hook_args;

	if (!is_time_for_check_nwhole_pmd_dbits_barrier(cmd))
		return 0; /*Continue execution*/

	for (i = 0; i < __hist->prop.n_tx_in_blkset; i++) {
		p = &blkst->tx[i];
		ec_tx_verify_nwhole_barrier(p);
	}

	__hist->disk_hooks.on_sync_cb_stage_end = NULL;  // Unset this hook. because nwhole might be called again as a result of error injection.

	return 0; /*Continue execution*/
}


static void __unitest_history_txid_gen_to_max_dmd_plus_1(struct t_ec_tx_history *hist) {
	struct t_ec_recov_tx *p = &hist->blkst[0].tx[1];
	const struct disk_range *pr = p->inp.sraid.cpr;
	u32 i, h;
	u32 max_txid = 0;
	u32 max1;
	for (int b = 0; b < hist->prop.n_blksets; b++) {
		for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
			for (h = 0; h < p->inp.tx_height; h++) {
				for (u32 j = 0; j < pr->replicas; ++j) {
					u32 txid;
					p = &hist->blkst[b].tx[i];
					txid = p->tpd.bptrs[h][j].dmd->tx_id;
					if (txid < NVMEIBC_DP_EC_MD_TX_ID_MAX) {
						max_txid = max(max_txid, txid);
					}
				}
			}
		}
	}

	max1 = max((u32)2, max_txid + 1);
	history_txid_gen = max(max1, history_txid_gen);
}

static u32 __get_len_of_tx(const struct disk_range *pr, const u32 tx_bm[], u32 tx_height) {
	u32 h;
	int res = 0;
	for (h = 0; h < tx_height; h++) {
		res +=  hweight32(tx_bm[h] & nvmeibc_raid1_get_data_bmp(pr));
	}

	return res;
}

static void __unitest_ECHotRecovery(struct NVMeshSystem *sys, struct t_ec_tx_history *hist) {
	int i, b;
	struct t_ec_recov_tx *p = &hist->blkst[0].tx[1];
	const u32 replicas = p->inp.sraid.cpr->replicas;
	const struct disk_range *pr = p->inp.sraid.cpr;
	const unsigned long rer_tx_bm = p->inp.rer.tx_bm[0];
	u32 len = __get_len_of_tx(pr, p->inp.rer.tx_bm, p->inp.rer.tx_height);
	u32 first_role = (u32)find_first_bit(&rer_tx_bm, pr->replicas);
	u8 *read_blk = NULL;
	bool should_clean_all_dbits_in_blockset = false;
	struct test_context env = {
		.sys = sys,
		.client = sys->clients,
		.dev = sys->clients->devs[p->inp.sraid.vsi.volume],
		.sraid = p->inp.sraid
	};
	u32 n_errs_inj_prev = NVMeshSystem_get_n_trans_err_injected(sys);
	bool is_trans_err_injected, is_write_resubmitted;
	u64 n_resubmitted;
	const u64 n_resubmitted_before = dp_io_stats_get_counter(&env.dev->dp.io_stats, DP_IO_STATS_RESUBMITTED);
	__inject_full_recov_history(sys, hist);
	if (__should_validate_nwhole_barrier(hist))
		hist->disk_hooks.on_sync_cb_stage_end = check_nwhole_pmd_dbits_barrier;

	if (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE) { // Caluclate the vlba range from txbm and the array to write
		u8 *write_buf = (u8 *)sim_kmalloc(NVMEIBC_SECTOR_SIZE * len, GFP_KERNEL);
		u32 h ,count, ri;
		for (h = count = 0, ri=first_role; count<len; ri++, count++) {
			if (ri == p->inp.sraid.cpr->slice_size) {
				ri = 0;
				h++;
			}
			memcpy(write_buf + NVMEIBC_SECTOR_SIZE*count, (u8*)p->tpd.rer.blocks[h][ri], NVMEIBC_SECTOR_SIZE);
		}
		EC_TX_BUG_ON(osSimulator_writeArrWait(&sys->clients->OS, 0, (p->inp.slba * pr->slice_size) + first_role, len, write_buf)); // Read IO which will encounter the stale lock and trigger synce
		if (write_buf) sim_kfree(write_buf);
	} else {
		read_blk = sim_kmalloc(NVMEIBC_SECTOR_SIZE * len, GFP_KERNEL);
		__unitest_fill_blocks_unique_pattern(read_blk, len);		// Garbage
		EC_TX_BUG_ON(osSimulator_readArrWait(&sys->clients->OS, 0, (p->inp.slba * pr->slice_size) + first_role, len, read_blk)); // Read IO which will encounter the stale lock and trigger synce
	}

	NVMeshSystem_serialize(sys);

	is_trans_err_injected = (NVMeshSystem_get_n_trans_err_injected(sys) == n_errs_inj_prev) ? false : true;
	n_resubmitted = dp_io_stats_get_counter(&env.dev->dp.io_stats, DP_IO_STATS_RESUBMITTED);
	is_write_resubmitted = is_trans_err_injected && (n_resubmitted_before != n_resubmitted) && (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE);
	__calc_post_recovery_conclusions(hist, is_trans_err_injected);

	__update_expectors_according_to_err_injection(hist);

	for (b = 0; b < hist->prop.n_blksets; b++) {
		for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
			bool no_parity_source_for_dbits_in_nwhole_in_other_slices = ((p->blkset->nwhole.bad_sec_bmp | ((~p->rer_bmp.topo.readable) & (p->rer_bmp.topo.raid.all))) == p->rer_bmp.topo.raid.pari);
			 p = &hist->blkst[b].tx[i];
			 _p = p;
			 ec_tx_verify_post_sync_tx(sys, p, is_trans_err_injected, hist->prc.is_unexpected_htr_executed);
			 if (p->inp.rer.io_perm.bits.is_hot_recovery && (!p->rer_bmp.topo.has_rw_pari || no_parity_source_for_dbits_in_nwhole_in_other_slices || hist->blkst->last_tx.is_wraparound || p->blkset->nwhole.ram_dbits_turnon_on_turnoff)) {
				 /* If no source parity in no_whole sync:
				 1. the dbits could be turned on for all slices in blockset so it's needs to be cleaned
				 2. If the parities are neverwritten and one of them is in W topology, it will become written while the other neverwritten so we need to set all the paritie's mds in blockset to neverwritten.
				 3. In cold(EC) no write-hole sync will not be called on double degraded parities so no need to clean all blockset dbits.
				 4. In case nwhole sync has to turn off dbits while there is a dead parity, it may turn on new dbits on those parities.
				 This cleaning of md's is neccassary because the TX franework makes invalid topology changes and with out syncs between them.
				 */
				 should_clean_all_dbits_in_blockset = true;
			 }
		}
	}

	// If rer's TX failed it might left abandodned entries on w segs becasue we disabled JGC execution by serjio.
	if (is_write_resubmitted) {
		struct toma_recovery_args rcvr_args = { .type = NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA };
		u32 seg_of_sstart = __get_slice_start_seg(&hist->blkst->tx[1]);
		roles_bmp_t role;
		struct nvmeibc_disk_hooks *disk_hooks = sys->servers[0].disk->disk_hooks;   // All the hooks in all disks reference to the same disk_hooks.
		ulong topo_w = p->rer_bmp.topo.w;

		NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);

		for_each_set_bit(role, &topo_w, replicas) {
			sgmnts_bmp_t w_seg = (seg_of_sstart + role) % replicas;
			EC_TX_BUG_ON(tomaSimulator_recoverThing(hist->blkst->tx->inp.sraid.tpr, hist->blkst->tx->inp.sraid.cpr + w_seg, rcvr_args) < 0);
		}
		NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, disk_hooks);
	}

	NVMeshSystem_check_no_abandoned(sys);

	for (b = 0; b < hist->prop.n_blksets; b++) {
		for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
			p = &hist->blkst[b].tx[i];
			_p = p;
			ec_tx_for_jmdc_do(sys, p, "Verify cleaned");
			ec_tx_free_bio_ptrs(p);
			__invalidate_used_disks_md(p, p->inp.sraid.cpr, &p->tpd);	// On topology change cannot have old dbits
		}
	}

	if (hist->blkst->last_tx.is_wraparound)
		history_txid_gen = 3;

	if (is_write_resubmitted) {
		__unitest_history_txid_gen_to_max_dmd_plus_1(hist);
	}

	if (should_clean_all_dbits_in_blockset)
		__dd_clean_dlba_pointers(env);   // Need only to clean the dbits in the md, but it's easier to erase all the dmd (TODO: clean only dmd's dbits).

	if (read_blk)
		sim_kfree(read_blk);
	hist->disk_hooks.on_sync_cb_stage_end = NULL;  // Unset this hook.
	NVMeshSystem__verify_all_serjios_are_clean(sys);				// verify journal abandoned entries were freed
	NVMeshSystem_check_no_abandoned(sys);
	for (i=0; i<sys->nServers; i++){
		 ramDiskSimulator_verify_no_locks(      &sys->servers[i].ramDisk);
		 ramDiskSimulator_verify_no_dirty_bits( &sys->servers[i].ramDisk);
		 ramDiskSimulator_verify_no_bad_sectors(&sys->servers[i].ramDisk);
		 tomaSimulator_verify_no_locks(         &sys->servers[i].simToma);
	}
}


static void __run_wrong_hot_scenarious(struct NVMeshSystem *sys, struct t_ec_tx_history *hist) {
	struct t_ec_recov_tx *p = &hist->blkst[0].tx[1];
	p->inp.ree.inject_allien_lock_id = true;		// ----------- Alien client, sync cannot read jmdc at all
	p->inp.tx_height = 1;
	p->inp.ree.tx_bm[0] = 0x3ff;
	p->inp.ree.aband_stg = ec_tx_abort_stage_success;
	p->inp.nat.is_journal_overriden = false;
	p->inp.nat.allow_bad_sectors = false;
	p->inp.nat.destory_slice = false;
	p->inp.rer.tx_height = 1;
	p->inp.rer.tx_bm[0] = 0x3ff;
	p->inp.rer.bio_type = NVMEIB_BLOCK_IO_OP_READ;
	__unitest_ECHotRecovery(sys, hist);
}

union io_perms_bitfield ec_tx_convert_recov_type_to_io_perms(const enum NVMEIBT_RECOVERY_TYPE type){
	switch (type) {
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
		return (union io_perms_bitfield){.bits={.is_io_R = 0, .is_io_W = 0, .is_cold_recovery = 1, .is_hot_recovery = 0, .is_jgc_recovery = 0}};
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
		return (union io_perms_bitfield){.bits={.is_io_R = 1, .is_io_W = 1, .is_cold_recovery = 0, .is_hot_recovery = 0, .is_jgc_recovery = 1}};
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:
		return (union io_perms_bitfield){.bits={.is_io_R = 1, .is_io_W = 1, .is_cold_recovery = 0, .is_hot_recovery = 1, .is_jgc_recovery = 0}};
	case NVMEIBT_RECOVERY_TYPE_INVALID:	// 'Rer' Does not intend to do recovery, it does 'Write IO', this write, however might trigger HOT/Dirty-bits/Maintanance syncs
		return (union io_perms_bitfield){.bits={.is_io_R = 1, .is_io_W = 1, .is_cold_recovery = 0, .is_hot_recovery = 1, .is_jgc_recovery = 0}};
	default: BUG();
	}
	return     (union io_perms_bitfield){.bits={.is_io_R = 0, .is_io_W = 0, .is_cold_recovery = 0, .is_hot_recovery = 0, .is_jgc_recovery = 0}}; //just to please compiler
}

void ec_tx_init(struct TstPRaid sraid, struct t_ec_recov_blkset *containing_blkset, struct t_ec_recov_tx* p, enum NVMEIBT_RECOVERY_TYPE type) {
	memset(p, 0, sizeof(*p));
	memset(containing_blkset, 0, sizeof(*containing_blkset));
	p->inp.sraid = sraid;
	array_fill(p->inp.rer.topo, NVMEIBTC_DS_MODE_RW);
	p->inp.rer.io_perm = ec_tx_convert_recov_type_to_io_perms(type);
	p->inp.pre.txid = 15; //we take space of 2 txid so that pre want be considered by hot
	p->inp.ree.txid = 17;										// Start from random 17 and increase
	p->inp.ree.is_topo_not_eq_to_rer = true;					// Allow 'rer' and 'ree' topos to differ
	p->inp.ree.uuid = SIMULATOR_OTHER_CLIENT__get_uuid();		// By default, other client made this problem
	p->inp.ree.is_old_completed = false;						// By default, this tx has to be analyzed and is not old
	p->blkset = containing_blkset;
}

static u32 __pre_recovery_test_init(bunitest_s* B, const char *test_name) {
	const u32 rand_seed = (u32)(jiffies);					// Store it to be able to reproduce random sequence
	srand(rand_seed);
	bunitest_tic(B);
	_ND(t_simuav31, "*************** EC-@STR-Sync (8+2), rseed=@X start", test_name, rand_seed);
	NVMeshSystem_all_clients_dbg_di(B->sys, true);
	return rand_seed;
}

static void __post_recovery_test_cleanup(bunitest_s* B) {
	NVMeshSystem_all_clients_dbg_di(B->sys, false);
	NVMeshSystem_wipe_all_dirty_bits(B->sys);			// Coz IO in last possible degraded topology left dbits
	NVMeshSystem_wipe_all_md_of_disks(B->sys);			// Dont confuse Cold/Hot recovery tests with partial transactions. Clean JMDD+JMDC
	EC_TX_BUG_ON(!NVMeshSystem_is_stable(B->sys));
}

u32 ec_tx_gen_next_txid(void) {
	const u32 rv = (history_txid_gen++) % (NVMEIBC_DP_EC_MD_TX_ID_MAX + 1);	// Daniel, for easier debugging give each blockset unique TxID, even though this is not a must
	history_txid_gen = (history_txid_gen + rand()%2) % (NVMEIBC_DP_EC_MD_TX_ID_MAX + 1);			// Randomally add another one
	WARN(history_txid_gen > NVMEIBC_DP_EC_MD_TX_ID_MAX, "Invalid Txid value.\n"); // Sanity
	return rv;
}

void ec_tx_init_blkset(struct t_ec_recov_blkset *blkset) {
	blkset->nwhole.turnoff_dbits_bmp = 0;
	blkset->nwhole.will_turnoff_dbits = false;
	blkset->will_nwhole_sync_called_on_blockset = false;
	blkset->nwhole.is_sl_by_sl = false;
	blkset->nwhole.bad_sec_bmp = 0;
	blkset->nwhole.will_turnon_dbits = false;
	blkset->nwhole.regen_bkw_bmp = 0;
	blkset->will_htr_sync_called_on_blockset = false;
}

static bool __is_wraparound_needed(struct t_ec_tx_history *hist) {
	const u32 txid_after_2_iterations_worst_case = history_txid_gen + 2*hist->prop.n_blksets*hist->prop.n_tx_in_blkset*2*2; // last *2 is to make sure it worst case might be not mandatory.
	if (txid_after_2_iterations_worst_case > NVMEIBC_DP_EC_MD_TX_ID_MAX) {
		return true;
	} else {
		return (rand()%6 == 0);  // make a wraparound even if not a must.
	}
}

static void __clear_all_journals(struct NVMeshSystem *sys, struct test_context env) {
	int *io2sgmnt2entry;

	nvmeibc_ensure_jam_has_nothing_bound(env);
	io2sgmnt2entry = nvmeibc_drain_free_jrnls(env, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, 2);	// Fully drain the free list
	nvmeibc_release_drained_jrnls(env, io2sgmnt2entry, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, NVMEIBC_JIDX_EVT_ERASE);	// Release free list
	NVMeshSystem_serialize(sys);
}

static void __simulate_wraparound(struct test_context env, struct NVMeshSystem *sys) {
	__dd_clean_dlba_pointers(env);  // Clean max_txid in blockset.
	__clear_all_journals(sys, env);
}

static void __make_sure_no_wraparound_happens(struct test_context env, struct t_ec_tx_history *hist, struct NVMeshSystem *sys) {
	const u32 txid_after_2_iterations_worst_case = history_txid_gen + 2*hist->prop.n_blksets*hist->prop.n_tx_in_blkset*2*2; // last *2 is to make sure it worst case might be not mandatory.
	if (txid_after_2_iterations_worst_case > NVMEIBC_DP_EC_MD_TX_ID_MAX) {  // Simulate wraparound
		history_txid_gen = rand() % txid_after_2_iterations_worst_case; // if disable_wrap make sure there will be no wraparound
		__simulate_wraparound(env, sys);
	}
}

static void __gen_recov_cand_history(struct test_context env, struct NVMeshSystem *sys, struct t_ec_tx_history *hist, bool is_htr, bool rand_blockset, bool disable_wrap) {
	const struct nvmeibc_cinst_params_core *p_core = &sys->clients[0].p->core;
	const uuid_be *uuids[] = {nvmeibc_get_uuid(p_core), SIMULATOR_OTHER_CLIENT__get_uuid()};	// Possible reoceveree clients
	struct t_ec_recov_tx *p;
	int n_tx_in_blkset = 2;  // 2 transactions in each blockset, todo, extend
	const u32 n_blksets_in_praid = min((u32)(NUM_JENTS_JAM_USES_IN_JRI(sys->servers[0].disk)/n_tx_in_blkset) /*each tx has it's own entry*/, (u32)hist->blkst[0].tx->inp.sraid.cpr->length/LOCKSET_SLICES);
	const u32 chosen_blkset = rand_blockset ? (rand() % n_blksets_in_praid) : ((u32)hist->blkst[0].tx[0].inp.slba)/LOCKSET_SLICES; // Only for HTR
	u32 slbas[LOCKSET_SLICES];
	int i, b;
	u32 h, j;
	bool is_wrap;
	bool is_bio_type_write = false;
	int num_jentries = LOCKSET_SLICES / hist->msn;

	__my_rand_perm_multi_slice(slbas, num_jentries, hist->msn);

	hist->prop.n_tx_in_blkset = n_tx_in_blkset;
	hist->prop.n_blksets = is_htr ? 1 : n_blksets_in_praid;
	hist->prop.n_tx_in_test = hist->prop.n_blksets * hist->prop.n_tx_in_blkset;
	for (b = 0; b < hist->prop.n_blksets; b++) {
		ec_tx_init_blkset(&hist->blkst[b]);
		for (i = 0, p = &hist->blkst[b].tx[i]; i < hist->prop.n_tx_in_blkset; i++, p++) {
			p->inp = hist->blkst[0].tx[0].inp;	// Copy Default input from blkst[0].tx[0]
			p->blkset = &hist->blkst[b]; // Fix backponter to blockset
		}
	}

	if (disable_wrap) {
		is_wrap = false;
		__make_sure_no_wraparound_happens(env, hist, sys);
	} else {
		is_wrap = __is_wraparound_needed(hist);
	}

	if (is_wrap) {  // Change starting txid so that last ree's txid will be NVMEIBC_DP_EC_MD_TX_ID_MAX
		history_txid_gen = (NVMEIBC_DP_EC_MD_TX_ID_MAX+1) - hist->prop.n_blksets*hist->prop.n_tx_in_blkset*2;
	}

	for (b = 0; b < hist->prop.n_blksets; b++) {
		struct t_ec_recov_blkset *blkset = &hist->blkst[b];
		const u32 first_pre_txid_in_blkset = history_txid_gen;
		history_txid_gen = ((history_txid_gen + hist->prop.n_tx_in_blkset) % (NVMEIBC_DP_EC_MD_TX_ID_MAX + 1)); // reserve free txids for pre of each TX in blockset.
		blkset->last_tx.is_wraparound = is_htr ? is_wrap : false;   // If not htr we do want to check the state when txid = NVMEIBC_DP_EC_MD_TX_ID_MAX but don't have any tx to make a wraparound.
		for (i = 0, p = &blkset->tx[i]; i < hist->prop.n_tx_in_blkset; i++, p++) {
			u32 tx_chain_offset;
			p->inp.tx_height = ((hist->msn == 1) ? 1 : 2); TODO(LKJ, make rand() % (hist->msn - tx_chain_offset) + 1);
			tx_chain_offset = rand() % (hist->msn - p->inp.tx_height + 1);
			if (is_htr)
				p->inp.slba = slbas[i] + tx_chain_offset + chosen_blkset * LOCKSET_SLICES;
			else
				p->inp.slba = slbas[i] + tx_chain_offset + b * LOCKSET_SLICES;   // Allow different blocksets point to the same slice [0..31]
			p->inp.ree.is_old_completed = (i < (hist->prop.n_tx_in_blkset - 1));    // Only 1 candidate per blockset can be not completed
			rand_valid_txbm_multi_slice(p->inp.sraid, true, p->inp.ree.tx_bm, p->inp.tx_height, false);
			p->inp.ree.tx_bm_union = 0;
			for (h = 0; h < p->inp.tx_height; h++) {
				p->inp.ree.tx_bm_union |= p->inp.ree.tx_bm[h];
			}
			p->inp.ree.txid = is_wrap ? history_txid_gen++ : ec_tx_gen_next_txid();
			p->inp.pre.txid = first_pre_txid_in_blkset + i;
			WARN_ON(p->inp.ree.txid > NVMEIBC_DP_EC_MD_TX_ID_MAX || p->inp.pre.txid > NVMEIBC_DP_EC_MD_TX_ID_MAX);
			p->inp.pre.is_parity_explicitly_marked_neverwritten_union = false;
			p->inp.pre.is_never_written_union = false;
			for (j = 0; j < p->inp.tx_height; j++) {
				p->inp.pre.is_never_written[j] = rand() % 2;
				p->inp.pre.is_never_written_union |= p->inp.pre.is_never_written[j];
				if (p->inp.pre.is_never_written[j]) {
					p->inp.pre.is_parity_explicitly_marked_neverwritten[j] = rand()%2;
				} else {
					p->inp.pre.is_parity_explicitly_marked_neverwritten[j] = false;
				}
				p->inp.pre.is_parity_explicitly_marked_neverwritten_union |= p->inp.pre.is_parity_explicitly_marked_neverwritten[j];
			}
			if (!is_htr) {
				for (j = 0; j < p->inp.tx_height; j++)
					p->inp.rer.tx_bm[j] = 0;
				p->inp.rer.bio_type = NVMEIB_BLOCK_IO_OP_NOP;
			} else if (p->inp.ree.is_old_completed) {           // If old comleted and htr we don't want it to cause a nwhole sync so only read (which will not take place) assures this.
				p->inp.rer.bio_type = NVMEIB_BLOCK_IO_OP_NOP;
				for (j = 0; j < p->inp.tx_height; j++)
					p->inp.rer.tx_bm[j] = 0;
			} else {
				p->inp.rer.tx_height = p->inp.tx_height; // LKJ: TODO: randomize in the future.
				rand_valid_txbm_multi_slice(p->inp.sraid, true, p->inp.rer.tx_bm, p->inp.rer.tx_height, false);
				if (is_wrap) {
					p->inp.rer.bio_type = NVMEIB_BLOCK_IO_OP_WRITE; // In order to cause a wraparound the tx must be write.
				} else {
					p->inp.rer.bio_type = (rand() & 0x1) ? NVMEIB_BLOCK_IO_OP_READ : NVMEIB_BLOCK_IO_OP_WRITE;
				}
				is_bio_type_write = (p->inp.rer.bio_type == NVMEIB_BLOCK_IO_OP_WRITE);
			}
			if (p->inp.ree.is_old_completed) {
				p->inp.ree.is_topo_not_eq_to_rer = false;				// Not accurate, but prevent 2 transactions in 2 different mutually excluding ree topos in the same blockset
				p->inp.ree.aband_stg = is_htr ? ec_tx_abort_stage_success : ec_tx_abort_stage_rand_get_old_tx();
			} else {
				p->inp.ree.is_topo_not_eq_to_rer = true;
				if (is_wrap && is_htr) {
					p->inp.ree.aband_stg = (enum ec_tx_abort_stage)(rand()%ec_tx_abort_stage_partial_journal); // In order to cause a wraparound the rer must see ree's txid (cause NVMEIBC_DP_EC_MD_TX_ID_MAX must be written to dmd).
				} else {
					p->inp.ree.aband_stg = is_htr ? (enum ec_tx_abort_stage)(rand()%ec_tx_abort_stage_last) : ec_tx_abort_stage_rand_get_failed_tx();
				}
			}
			if (p->inp.ree.aband_stg == ec_tx_abort_stage_success)
				p->inp.nat.is_journal_overriden = (is_htr && p->inp.ree.is_old_completed) ? true : rand() % 2;
			else
				p->inp.nat.is_journal_overriden = false;
			if (is_htr && !p->inp.ree.is_old_completed && !p->inp.nat.is_journal_overriden)
				blkset->will_htr_sync_called_on_blockset = true; // Default is false
			p->inp.nat.destory_slice = false;  // LKJ: enable destroy slice
			p->inp.ree.uuid = is_htr ? SIMULATOR_OTHER_CLIENT__get_uuid() : uuids[rand() % 2];     // LKJ: Currently only one client have lock id known by toma so in HTR using it. // Randomly assign 50% tx to each client
			p->inp.ree.is_garbage_for_jgc = ((p->inp.rer.io_perm.bits.is_jgc_recovery) ? (rand()%2) : 0);
		}

		// After genreating all Txs in blkset need to generate more bmps upon last generations results.
		for (i = 0, p = &blkset->tx[i]; i < hist->prop.n_tx_in_blkset; i++, p++) {
			if (is_htr && (!blkset->will_htr_sync_called_on_blockset)) {  // Currently we allow bad sectors only on htr when the htr itself won't be called.
				p->inp.nat.allow_bad_sectors = rand() % 2;
			} else {
				p->inp.nat.allow_bad_sectors = false;
			}
		}
	}


	if (!is_htr) { // In Htr we only have one blockset so the copy above is sufiicient.
		for (b = 0; b < hist->prop.n_blksets; b++) {
			for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
				u32 si, replicas, s_start;
				p = &hist->blkst[b].tx[i];
				replicas = p->inp.sraid.cpr->replicas;
				s_start = __get_slice_start_seg(p);
				if (s_start) {
					for (si = 0; si < replicas; si++)					// Topology is relative to slice start so rotate it such that entire history has identical rer's topology
						p->inp.rer.topo[si] = hist->blkst[0].tx[0].inp.rer.topo[(si + s_start)%replicas];
				}
			}
		}
	}


	if (is_bio_type_write)
		history_txid_gen++;  // Write is going to inc txid in blockset so need to inc history_txid_gen too.

	WARN((hist->blkst[0].tx[0].inp.slba >= LOCKSET_SLICES) && (!is_htr), "Unitest BUG, First candidate must be in first blockset (except from htr), or else, setting topology will not work\n");
}


static void __init_rams_before_cold_recovery(struct NVMeshSystem *sys, struct t_ec_recov_tx *p) {
	struct TstPRaid *pra = &p->inp.sraid;
	if (p->inp.rer.io_perm.bits.is_cold_recovery) {	// Ask All relevant Tomas to initialize servers ram (locks, binfo) before cold recovery
		for (pra->vsi.segment = 0; pra->vsi.segment < (int)pra->cpr->replicas; pra->vsi.segment++)
			nvmeibr_ds_metadata_init_EC_lock_and_dirty(&sys->servers[pra->cpr[pra->vsi.segment].node_id].simToma, pra);
	}
}

static void __verify_stats_incremented(u64 before, u64 after, u64 amount) {
	EC_TX_BUG_ON(after != before + amount);
}

//the function returns true if retry is needed, false otherwise
static bool __run_rcvry_on_sgmnt(struct t_ec_tx_history *hist, int sgmnt_id){
	struct toma_recovery_args rcvr_args = {.type = hist->prop.rec.type, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA};
	const u64 n_total_recovery_success_before = hist->hdr->recoveries[hist->prop.rec.type]->stats.n_success;

	EC_TX_BUG_ON(tomaSimulator_recoverThing(hist->blkst->tx->inp.sraid.tpr, hist->blkst->tx->inp.sraid.cpr + sgmnt_id, rcvr_args) < 0);
	hist->iter.n_recov_retries++;

	if (hist->hdr->recoveries[hist->prop.rec.type]->stats.n_success >= (n_total_recovery_success_before + 1)) {
		__verify_stats_incremented(n_total_recovery_success_before, hist->hdr->recoveries[hist->prop.rec.type]->stats.n_success, 1);
		return false;
	} else {
		return true;
	}
}

static int __unitest_EC_jour_recovery(struct NVMeshSystem *sys, struct t_ec_tx_history *hist) {
	int i, b;
	u32 seg;
	u32 n_errs_inj_prev = NVMeshSystem_get_n_trans_err_injected(sys);
	bool is_trans_err_injected;
	struct test_context env = {
	 .sys = sys,
	 .client = sys->clients,
	 .dev = sys->clients->devs[hist->blkst->tx->inp.sraid.vsi.volume],
	 .sraid = hist->blkst->tx->inp.sraid
	};

	__init_rams_before_cold_recovery(sys, &hist->blkst[0].tx[0]);
	__inject_full_recov_history(sys, hist);
	nvmeibc_cold_stats_reset();

	for (seg = hist->prop.rec.seg_start; seg < hist->prop.rec.seg_end; seg++) { // Call Recovery on some segments
		u32 n_tries = hist->prop.is_trans_errors_inject_enabled ? 20 : 1;
		const u64 n_total_recoveries_before = hist->hdr->recoveries[hist->prop.rec.type]->stats.n_total_tasks;
		hist->iter.n_recov_retries = 0;

		while (n_tries && __run_rcvry_on_sgmnt(hist, seg)) {
			--n_tries;
		}
		__verify_stats_incremented(n_total_recoveries_before, hist->hdr->recoveries[hist->prop.rec.type]->stats.n_total_tasks, hist->iter.n_recov_retries);
	}

	is_trans_err_injected = (NVMeshSystem_get_n_trans_err_injected(sys) == n_errs_inj_prev) ? false : true;
	__verify_jour_history_correct_flow(hist, is_trans_err_injected);

	for (b = 0; b < hist->prop.n_blksets; b++) {
		for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
			struct t_ec_recov_tx *p = &hist->blkst[b].tx[i];
			_p = p;
			ec_tx_verify_post_sync_tx(sys, p, is_trans_err_injected, false);
		}
	}

	NVMeshSystem_check_no_abandoned(sys);

	for (b = 0; b < hist->prop.n_blksets; b++) {
		for (i = 0; i < hist->prop.n_tx_in_blkset; i++) {
			struct t_ec_recov_tx *p = &hist->blkst[b].tx[i];
			_p = p;
			ec_tx_for_jmdc_do(sys, p, "Verify cleaned");
			ec_tx_free_bio_ptrs(p);
			__invalidate_used_disks_md(p, p->inp.sraid.cpr, &p->tpd);	// On topology change cannot have old dbits
		}
	}

	if (history_txid_gen >= NVMEIBC_DP_EC_MD_TX_ID_MAX) {  // Simulate wraparound
		history_txid_gen = 2;
		__simulate_wraparound(env, sys);
	}

	NVMeshSystem__verify_all_serjios_are_clean(sys);
	NVMeshSystem_check_no_abandoned(sys);
	return 0;
}

void __gen_recov_info(struct t_ec_tx_history *hist, enum NVMEIBT_RECOVERY_TYPE type) {
	hist->prop.rec.type = type;
	switch (type) {
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:    { /*Send Via 1st seg */ hist->prop.rec.seg_start = 0; hist->prop.rec.seg_end = 1; break; }
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC: { /*Send Via all seg */ hist->prop.rec.seg_start = 0; hist->prop.rec.seg_end = hist->blkst->tx->inp.sraid.cpr->replicas; break; }
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD: { hist->prop.rec.seg_start = 0; hist->prop.rec.seg_end = 0; break;}
	default: BUG();
	}
}

static int __unitest_EC_Recovery(bunitest_s* B, const enum NVMEIBT_RECOVERY_TYPE type) {
	struct NVMeshSystem *sys = B->sys;
	struct t_ec_tx_history hist;
	const char* name = nvmeibt_recov_type_to_3str(type);
	char topo_str[64] = {0};
	const struct volume_segment_index vsi = {0,0,0,0};
	struct TstPRaid sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi);
	u32 s, first_txid, rand_seed = __pre_recovery_test_init(B, name);
	struct topologies_enumerator topos;
	bool orig_ver = nvmeibc_jmd_wr_version;
	struct test_context env = {
		.sys = sys,
		.client = sys->clients,
		.dev = sys->clients->devs[sraid.vsi.volume],
		.sraid = sraid
	};
	u32 *binje_ptr = (void*)&nvmeibc_cinst_get_blok_p(env.dev)->binje, binje_orig = *binje_ptr;
	__calc_j2d_ouside_of_all_segs(&sraid);

	__clear_all_journals(sys, env);
	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(sys, true);
	//rand_seed = 302251; srand(rand_seed);
	nvmeibc_jmd_wr_version = NVMEIBC_JOURNAL_MD_VERSION_PACKED; //rand()%2;				// 50% NVMEIBC_JOURNAL_MD_VERSION_PACKED, NVMEIBC_JOURNAL_MD_VERSION_UNPACKED
	memset(&hist, 0, sizeof(hist));
	(type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD) ? nvmeibc_htr_stats_reset() : nvmeibc_cold_stats_reset(); // Reset any values that other tests have changed
	ec_tx_init(sraid, &hist.blkst[0], &hist.blkst[0].tx[0], type);
	__gen_recov_info(&hist, type);
	hist.hdr = ___get_tail_topo_of_device(env.sys, vsi.volume)->chunks[vsi.chunk].raid1s[vsi.raid].hdr;
	first_txid = history_txid_gen; //hist.tx[0].inp.ree.txid;
	hist.prop.is_trans_errors_inject_enabled = true;
	*binje_ptr = ((nvmeibc_jmd_wr_version == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED) ? 1 : nvmeibc_jentry_num_blocks);
	hist.msn = *binje_ptr;
	if (hist.prop.is_trans_errors_inject_enabled) {
		struct nvmeibc_disk_hooks disk_hooks_temp = {.args.trerr = {false, false, false, 0, 0, (rand() % INJECT_TRANSPORT_ERROR_CYCLE_SIZE), true, 0, 0}, .args.iocmd_comp_args.hist = &hist,
		.inject_transport_error = inject_transport_error };
		hist.disk_hooks = disk_hooks_temp;
		NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, &hist.disk_hooks);
	}
	_hist = &hist;
	topos = create_topo_random_enum(sraid, 1, 10, 10);
	__dd_clean_dlba_pointers(env);  /* Todo: remove after dbits rebuild wont turn on dbits on all blockset slices  */

	while (topos.move_next(&topos)) {  // iterate on random topologies - all topologies are possible //__topo_enum_move_2_index(&topos, 19)) {
		struct topology_sgmnts_t topo = topos.curr;
		struct topology_roles_t topo_roles = sim_convert_topo_presentations(__get_slice_start_seg(&hist.blkst[0].tx[0]), sraid.cpr->replicas, topo); // Assumes the the first tx is on the first blockset.
		for (s = 0; s < 2; s++)
			hist.blkst[0].tx[0].inp.rer.topo[topo_roles.dgrd_roles[s]] = topo_roles.dgrd_modes[s];	// Set topo
		topo_enum_tostring(&topos, topo_str);
		unitest_print("%s: ============= %s rand_seed=%d txid_gen=%d\n", name, topo_str, rand_seed, history_txid_gen);
		__gen_recov_cand_history(env, sys, &hist, type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD, true, true);
		ec_tx_set_and_switch_rer_topo(sys, &hist.blkst[0].tx[0], true);
		if (1) {
			int u;
			for (u = 0; u < 10; u++) {							// Do X iterations for better random coverage
				bool is_last_iteration = (u == 9);
				//if (u == 2 && topos.impl.curr_permutation == 8) {
				if (u == 0 && topos.impl.curr_permutation == 0) {
					int vv =0;
					vv++;
				}
				hist.iter.curr_permutation = topos.impl.curr_permutation;
				hist.iter.iner_iteration = u;
				if (type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD) {
					__unitest_ECHotRecovery(sys, &hist);
				} else {
					__unitest_EC_jour_recovery(sys, &hist);
				}
				if (!is_last_iteration)
					__gen_recov_cand_history(env, sys, &hist, type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD, false, true);
				_ND(t_simuav32, "*************** EC-@STR-Sync (8+2), rseed=@X itr=@INT txid_gen=@INT", name, rand_seed, u, history_txid_gen);
			}
		}
		ec_tx_set_and_switch_rer_topo(sys, &hist.blkst[0].tx[0], false);
	}
	if (hist.prop.is_trans_errors_inject_enabled) {
		NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
	}
	if (type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD) {
		__gen_recov_cand_history(env, sys, &hist, true, true, true);
		__run_wrong_hot_scenarious(sys, &hist);
	}
	NVMeshSystem_serialize(sys);
	__post_recovery_test_cleanup(B);
	EC_TX_BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(sys, false);
	_ND(t_simuav33, "*************** EC-@STR-Sync (8+2), rseed=@X, sent @INT Syncs in @INT[mSec]", name, rand_seed, (history_txid_gen-first_txid)/8, bunitest_toc(B));
	nvmeibc_jmd_wr_version = orig_ver;
	*binje_ptr = binje_orig;
	return 0;
}

TEST_FUNC int unitest_ECHotRecovery( bunitest_s* B) { return __unitest_EC_Recovery(B, NVMEIBT_RECOVERY_TYPE_STALE_REBUILD);}
TEST_FUNC int unitest_ECJgcRecovery( bunitest_s* B) { return __unitest_EC_Recovery(B, NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC);}
TEST_FUNC int unitest_ECColdRecovery(bunitest_s* B) { return __unitest_EC_Recovery(B, NVMEIBT_RECOVERY_TYPE_EC_COLD);}

