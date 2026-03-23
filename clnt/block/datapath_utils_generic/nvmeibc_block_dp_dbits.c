/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "block/dp_topology_traits.h"
#include "common/kr_incs.h"
#include "nvmeib_math.h"
#include "nvmeibc_block_dp_dbits.h"

/*************************** nvmeibc_dbits_action *****************************/
static void nvmeibc_dbits_action_init(struct nvmeibc_dbits_action* act, const u16 num_degraded)
{
	BUILD_BUG_ON(sizeof(struct nvmeibc_dbits_action) != 8);		// Usage via ->raw field will be incorrect
	act->raw = 0ULL;
	act->num_degraded = num_degraded;
	// BUG_ON(num_degraded == 0);	// Todo: trap here! the code allows calculations of dbits in perfect topology but traps them later via verify_binfo_is_legal(). Not ideal
	BUG_ON(num_degraded > 3); // 4 Mirror has 3 parities
}

static inline void __action_add(struct nvmeibc_dbits_action *act, int di, int c)
{
	if (di == UNK_DB) {
		act->num_unknowns++;
		WARN(c, "nvmeibc bug, convict with unknown\n");
	} else if (di) {
		act->db_turn_on_bmp |= (1 << (di - 1));
		act->db_conv_map    |= (c << (di - 1));
	} else {
		WARN(c, "nvmeibc bug, convict without dbit\n");
	}
}

#define __should_loose_slice_info(act, is_deg2) \
				(is_deg2 || (act)->db_conv_map || (act)->num_unknowns)

static void __action_calc_has_slice_info(struct nvmeibc_dbits_action *act)
{
	const int n_deg = hweight32(act->db_turn_on_bmp&(~act->db_turn_off_bmp));
	act->has_slice_info = !__should_loose_slice_info(act, (n_deg > 1));
}

static void nvmeibc_dbits_action_init_by_entry(struct nvmeibc_dbits_action *act,
					const union nvmeibc_dbits_entry *e, const int num_degraded)
{
	nvmeibc_dbits_action_init(act, num_degraded);
	if (nvmeibc_dbits_entry_is_global_mode(e) && (e->all_bits != 0)) {
		const int d0 = e->bsmod.dead0, d1 = e->bsmod.dead1;
		WARN((d0 > N_MAX_RAID_SLICE_LEN) && (d0 != UNK_DB), "nvmeibc bug! found illegal dbit_0=%u, ent=%ul (0)", d0, e->all_bits);    // Traps for un/initialized dbits values
		WARN((d1 > N_MAX_RAID_SLICE_LEN) && (d1 != UNK_DB), "nvmeibc bug! found illegal dbit_1=%u, ent=%ul (1)", d1, e->all_bits);
		WARN((d0 < d1), "nvmeibc bug, dbits not sorted 0x%x\n", e->all_bits);
		__action_add(act, d0, e->bsmod.is_d0_convict);
		__action_add(act, d1, e->bsmod.is_d1_convict);
		__action_calc_has_slice_info(act);
	} else {		// Single degraded or no degraded.
		const int d0 = e->slmod.dead0;
		__action_add(act, d0, 0);	// Todo: somehow save slice info?
		act->has_slice_info = true;	// For future
	}
}

bool warn_on_too_many_degraded = true; 	// Warn on more than 2 dirty bits
static void nvmeibc_dbits_action_to_entry(const struct nvmeibc_dbits_action *act, union nvmeibc_dbits_entry *dst)
{
	u32 db_map = act->db_turn_on_bmp;
	int segs[2] = {0,0}, conv[2] = {0,0}, i = 0, n_deg = 0;
	if ((act->num_degraded == 0) && (warn_on_too_many_degraded)) {
		WARN(act->db_turn_on_bmp|act->db_turn_off_bmp|act->db_conv_map, "nvmeibc bug! illegal non degraded dbit_action=0x%llx", (long long unsigned int)(act->raw));
	}

	WARN(db_map >= (1<<N_MAX_RAID_SLICE_LEN), "nvmeibc bug! found illegal dbit=0x%x, action=0x%llx", db_map, (long long unsigned int)(act->raw));
	for (i = 0; db_map; i++, db_map >>= 1) { // Traverse bits lowest to highest
		if (db_map & 0x1) { // Found set bit
			if (unlikely(n_deg >= 2)) { // we already have 2 degraded, todo incorporate 3-4 degradedness representation
				WARN(warn_on_too_many_degraded, "nvmeibc bug, 3+ dirty bits: 0x%x\n", act->db_turn_on_bmp);
				break;
			}
			segs[n_deg] = i + 1;
			conv[n_deg] = ((act->db_conv_map >> i)&0x1);
			n_deg++;
		}
	}
	if (likely(warn_on_too_many_degraded)) {
		WARN(((n_deg + act->num_unknowns) > act->num_degraded), "nvmeibc bug, action:0x%llx, dbits=%u, unkn=%u, n_deg_topo=%u\n", (long long unsigned int)(act->raw), n_deg, act->num_unknowns, act->num_degraded);
	}

	if (segs[0] < segs[1]) {	// Swap inorder to keep order of seg[0] > seg[1]
		swap(segs[0], segs[1]);
		swap(conv[0], conv[1]);
	}

	dst->all_bits = 0;
	if (!act->has_slice_info) {
		if (act->num_unknowns == 0) {
			dst->bsmod.is_d0_convict = conv[0];
			dst->bsmod.is_d1_convict = conv[1];
			dst->bsmod.dead0 = segs[0];
			dst->bsmod.dead1 = segs[1];
		} else if (act->num_unknowns == 1) { // UNKNOWN is always first
			dst->bsmod.dead0 = UNK_DB;					 // Append unknown markers
			dst->bsmod.dead1 = segs[0];
			dst->bsmod.is_d1_convict = conv[0];
		} else {  // Double unknown, order is irrelevant
			dst->bsmod.dead0 = dst->bsmod.dead1 = UNK_DB;
		}
		if (dst->bsmod.mod_marker)
			dst->bsmod.mod_marker += 0xc;    // All convicts are encoded as 0xC+
	} else { 						// Single degraded
		dst->slmod.dead0 = segs[0];
		WARN(n_deg > 1 || (act->num_unknowns), "nvmeibc bug! found illegal action action=0x%llx", (long long unsigned int)(act->raw));
		// Todo: Here encode slice information
	}
}

enum merge_option { MO_INTERSECT = 0, MO_UNIFY = 1, MO_DIFF = 2 };
static struct nvmeibc_dbits_action nvmeibc_dbits_action_merge(const struct nvmeibc_dbits_action *old, const struct nvmeibc_dbits_action *New, enum merge_option mo)
{
	struct nvmeibc_dbits_action rv;
	const int num_degraded = old->num_degraded;
	BUG_ON(num_degraded != New->num_degraded);
	nvmeibc_dbits_action_init(&rv, num_degraded);
	BUG_ON(old->db_turn_off_bmp);		// Just for debug, old action already turned off what it needed

	if (mo == MO_DIFF) {				// Calculate diff via XOR
		rv.db_turn_on_bmp =  (old->db_turn_on_bmp  ^ New->db_turn_on_bmp);
		rv.db_turn_off_bmp = (old->db_turn_off_bmp ^ New->db_turn_off_bmp);
		rv.db_conv_map =     (old->db_conv_map     ^ New->db_conv_map);
		rv.num_unknowns =    (old->num_unknowns    ^ New->num_unknowns);
		return rv;
	}
	/* Step 1: basic merge, yields incorrect results */
	if (mo == MO_UNIFY) {
		rv.db_turn_on_bmp =  (old->db_turn_on_bmp  | New->db_turn_on_bmp);	// union of dbits
		rv.db_turn_off_bmp = (old->db_turn_off_bmp | New->db_turn_off_bmp);
		rv.db_conv_map =     (old->db_conv_map     | New->db_conv_map);
		rv.num_unknowns =    (old->num_unknowns    + New->num_unknowns);
	} else {										// MO_INTERSECT
		WARN(!!old->db_turn_off_bmp || !!New->db_turn_off_bmp, "nvmeibc bug! should never be true, turn_off1=%u, turn_off2=%u\n", old->db_turn_off_bmp, New->db_turn_off_bmp); // sanity
		rv.num_unknowns = min((u32)old->num_unknowns, (u32)New->num_unknowns);
		if        (((u32)New->num_unknowns > 0) && ((u32)old->num_unknowns == 0)) {
			rv.db_turn_on_bmp = old->db_turn_on_bmp;
			rv.db_conv_map =    old->db_conv_map;
		} else if (((u32)old->num_unknowns > 0) && ((u32)New->num_unknowns == 0)) {
			rv.db_turn_on_bmp = New->db_turn_on_bmp;
			rv.db_conv_map =    New->db_conv_map;
		} else if (((u32)old->num_unknowns > 0) && ((u32)New->num_unknowns >  0)) {  // Both entries have unkowns
			rv.db_turn_on_bmp =  (old->db_turn_on_bmp  | New->db_turn_on_bmp);  // union of dbits, because of unknowns, this might give slightly worst results than possible
			rv.db_conv_map =     (old->db_conv_map     | New->db_conv_map);
		} else {  // Both entries don't have unknowns
			rv.db_turn_on_bmp =  (old->db_turn_on_bmp  & New->db_turn_on_bmp);  // intersection of dbits
			rv.db_conv_map =     (old->db_conv_map     & New->db_conv_map);
		}
	}
	{	/* Step 2: Fixups */
		const int n_deg = (int)hweight32((u32)rv.db_turn_on_bmp);	// Before turn off!
		if (unlikely(n_deg > num_degraded)) {
			WARN(warn_on_too_many_degraded, "nvmeibc bug, too much dirty bits: turn_on_bmp=0x%x > %u\n", rv.db_turn_on_bmp, num_degraded);
		}
		{	// Fix unknowns: Daniel, this is not the full algorithm. Should take into account the topology. Otherwise we end up with too much unknowns
			const u16 resolved_bits = (rv.db_turn_on_bmp | rv.db_turn_off_bmp);			// Bits we know for sure are not unknowns because we touched them
			int n_max_unk = (num_degraded - hweight16(resolved_bits));					// Some unknowns might be resolved via merge of turn ons / turnoffs
			if (n_max_unk < 0)
				n_max_unk = 0;                // n_max_unk Can be negative
			if ((int)rv.num_unknowns > n_max_unk)
				rv.num_unknowns = (u16)n_max_unk;
		}
		rv.db_turn_on_bmp &= (~rv.db_turn_off_bmp);
		rv.db_conv_map    &=   rv.db_turn_on_bmp;
		__action_calc_has_slice_info(&rv);
	}

	/* Step 3: Todo, merge slice info */
	if (rv.has_slice_info) {
	}
	return rv;
}

sgmnts_bmp_t nvmeibc_dbits_get_bm(const union nvmeibc_dbits_entry *e, const int np)
{
	struct nvmeibc_dbits_action a;
	nvmeibc_dbits_action_init_by_entry(&a, e, np);
	return a.db_turn_on_bmp;
}

u32 nvmeibc_dbits_get_n_unk(const union nvmeibc_dbits_entry *e, const int np)
{
	struct nvmeibc_dbits_action a;
	nvmeibc_dbits_action_init_by_entry(&a, e, np);
	return a.num_unknowns;
}

void nvmeibc_dbits_del_unk(union nvmeibc_dbits_entry *e, const int np)
{
	struct nvmeibc_dbits_action a;
	nvmeibc_dbits_action_init_by_entry(&a, e, np);
	if (a.num_unknowns) {
		a.num_unknowns = 0;
		__action_calc_has_slice_info(&a);
	}
	nvmeibc_dbits_action_to_entry(&a, e);
}

static u16 __nvmeibc_dbits_merge_by_strategy(const union nvmeibc_dbits_entry *e1, const union nvmeibc_dbits_entry *e2, enum merge_option mo, struct dp_topology_traits const* topo_traits)
{
	union nvmeibc_dbits_entry rv;
	if (mo != MO_DIFF) {							// Calc quick cases when one of them is zero. Unify takes worst, intersect takes best (0 dbits)
		if (        e1->all_bits == 0) {
			return ((mo == MO_UNIFY) ? e2->all_bits : 0);
		} else if ( e2->all_bits == 0) {
			return ((mo == MO_UNIFY) ? e1->all_bits : 0);
		}
	}
	{
		struct nvmeibc_dbits_action a1, a2, a_rv;
		nvmeibc_dbits_action_init_by_entry(&a1, e1, topo_traits->n_parities);
		nvmeibc_dbits_action_init_by_entry(&a2, e2, topo_traits->n_parities);
		a_rv = nvmeibc_dbits_action_merge(&a1, &a2, mo);
		nvmeibc_dbits_action_to_entry(&a_rv, &rv);
	}
	return rv.all_bits;
}

u16 nvmeibc_dbits_intersect_owners(const union nvmeibc_dbits_entry *e1, const union nvmeibc_dbits_entry *e2, struct dp_topology_traits const* topo_traits)
{
	return __nvmeibc_dbits_merge_by_strategy(e1, e2, MO_INTERSECT, topo_traits);
}

u16 nvmeibc_dbits_merge_owners(const union nvmeibc_dbits_entry *e1, const union nvmeibc_dbits_entry *e2, struct dp_topology_traits const* topo_traits)
{
	return __nvmeibc_dbits_merge_by_strategy(e1, e2, MO_UNIFY, topo_traits);
}

u32 nvmeibc_dbits_tx_apply(const union nvmeibc_dbits_entry *e_pre,
									 struct nvmeibc_dbits_tx *tx)
{
	struct nvmeibc_dbits_action a_pre, a_post;
	nvmeibc_dbits_action_init_by_entry(&a_pre, e_pre, tx->action.num_degraded);
	a_post = nvmeibc_dbits_action_merge(&a_pre, &tx->action, MO_UNIFY);
	nvmeibc_dbits_action_to_entry(&a_post, &tx->post);
	return tx->post.all_bits;
}

/********************** Dirty-bits Raid Policy Info **************************/

void nvmeibc_dbits_tx_init_empty(struct nvmeibc_dbits_tx *tx, const int num_degraded)
{
	nvmeibc_dbits_action_init(&tx->action, num_degraded);
}

void nvmeibc_dbits_tx_init_by_bmp(struct nvmeibc_dbits_tx* tx, const int num_degraded,
								  u32 turn_on_dbit_bmp, u32 turn_off_dbit_bmp, u32 turn_on_conv_bmp)
{
	nvmeibc_dbits_tx_init_empty(tx, num_degraded);
	tx->action.db_turn_on_bmp = turn_on_dbit_bmp | turn_on_conv_bmp;
	tx->action.db_turn_off_bmp = turn_off_dbit_bmp;
	tx->action.db_conv_map = turn_on_conv_bmp;
	__action_calc_has_slice_info(&tx->action);
}

void nvmeibc_dbits_tx_init_only_dconv(struct nvmeibc_dbits_tx *tx, const int num_degraded, u32 turn_on_conv_bmp)
{
	nvmeibc_dbits_tx_init_by_bmp(tx, num_degraded, 0 /* turn_on_dbit_bmp */, 0 /* turn_off_dbit_bmp */, turn_on_conv_bmp);
}
