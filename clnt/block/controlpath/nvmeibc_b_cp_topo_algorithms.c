/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block.h"					// Must be first for simulator
#include "block/nvmeibc_topology.h"
#include "block/controlpath/nvmeibc_b_cp_topo_common.h"
#include "block/dp_topology_traits.h"
#include "block/nvmeibc_block_common.h"
#include "nvmeibc_io_pet.h"
/* This c file implements algorithmic components of topology
	Started separating the specific implementation logic of topologies transition
	(atomic counters, list, locks) from the algorithmic functions which are
	required by users of topology (IO/Syncs/...)
*/

enum nvmeib_io_type_permission nvmeibc_raid1_get_io_perm(const struct nvmeibc_topology *t, int c, int r)
{
	return nvmeibc_raid1_get_io_perm_2(__get_r1_by_t(t, c, r));
}
bool nvmeibc_should_use_view_lock(const enum nvmeib_block_io_op op, const struct nvmeibc_disk_segment *seg)
{
	return ((op == NVMEIB_BLOCK_IO_OP_READ)&&(seg->is_safe_for_view_lock));
}

struct nvmeibc_raid1 const* __nvmeibc_disk_segment_get_praid_impl(struct nvmeibc_disk_segment const *seg)
{
	const struct nvmeibc_subscription_ctx *tr;
	if (seg) {
		tr = seg->toma_reg;
		return &seg->chunk->raid1s[tr->r1];
	}
	return NULL;
}

union nvmeibc_raid1_io_pet_status
nvmeibc_raid1_io_pet_describe_state(struct nvmeibc_raid1 const* raid)
{
	u8 idx = 0;
	enum NVMEIBTC_DS_MODE acms[2] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW};
	u8 sgmnts[2] = {0, 0};
	int info_idx = -1;
	u8 const n_segments = numeric_downcast(u8, raid->replicas);
	for (idx = 0; idx < n_segments; ++idx){
		enum NVMEIBTC_DS_MODE acm = raid->segments[idx].toma_acm;
		if (unlikely( acm != NVMEIBTC_DS_MODE_RW)){
			info_idx += 1;
			acms[info_idx] = acm;
			sgmnts[info_idx] = idx;
			if (info_idx == 1){
				break;
			}
		}
	}
	return (union nvmeibc_raid1_io_pet_status){
		.info = {
			.n_sgmnts = n_segments,
			.dgrd_sgmnts = {{sgmnts[0], acms[0]}, {sgmnts[1], acms[1]}}
		}
	};
}

struct nvmeib_lock_entry_constants _default_lock_consts;		// Constants for lock (mask, stale_bit_mask, tx_id shift/mask, dirty-bits shift/mask)

void __praid_init_lock_consts(struct nvmeibc_raid1 *pr);
void __praid_init_lock_consts(struct nvmeibc_raid1 *pr)
{
	struct nvmeib_lock_entry_constants *lc = (void*)nvmeibc_raid1_get_lock_consts(pr);
	lc->unlocked_val = LS_UNLOCKED;
	/* Uses parts of the lock for Transaction ID and Dirty-bits */
	lc->w_blkset_info =           true;
	lc->blkset_info_txid_shift =  NVMEIB_BLKSET_INFO_TXID_SHIFT;
	lc->blkset_info_txid_mask =   NVMEIB_BLKSET_INFO_TXID_MASK;
	lc->blkset_info_dbits_shift = NVMEIB_EC_JMDC_BITS_TX_ID+NVMEIB_BLKSET_INFO_TXID_SHIFT;
	lc->blkset_info_dbits_mask =  NVMEIB_BLKSET_INFO_DIRTY_MASK;
	lc->stale_bit_mask =          nvmeib_stale_bit_mask_ec.all;
}

/*************************** protection raid bitmaps *************************/
void nvmeibc_raid1_acm2bmp(enum NVMEIBTC_DS_MODE acm, int si, struct nvmeibc_roles_bmps *bmp)
{
	switch (acm) {
		case NVMEIBTC_DS_MODE_RW:         bmp->rw |=   (1 << si); break;
		case NVMEIBTC_DS_MODE_W_NO_DIRTY: bmp->wp |=   (1 << si); break;
		case NVMEIBTC_DS_MODE_W_IS_DIRTY: bmp->wm |=   (1 << si); FALLTHRU; /*break;*/
		case NVMEIBTC_DS_MODE_W:          bmp->w |=    (1 << si); break;
		case NVMEIBTC_DS_MODE_DEAD:       bmp->dead |= (1 << si); break;
		default: break;
	}
}

// Does this raid requires to only view lock for read (TODO: Degraded EC might "think" it needs but practically it might not - read a single block to a drive that is not degraded in a degraded p-raid)
static bool __calc_should_use_view_lock_on_read(const struct nvmeibc_raid1 *pr)
{
	if (pr->segments[0].sync_safety != NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO) {
		return false;
	} else if (!nvmeibc_raid_is_ec(pr)) {
		return true;
	} else {
		return nvmeibc_praid_are_all_readable(pr);
	}
}

static void __calc_rotated_bmps(struct nvmeibc_raid1 *pr, int shift) {
	struct nvmeibc_raid1_calculated_data *cd = &pr->calculated_data;
	struct nvmeibc_roles_bmps *bmps = &cd->roles_bmps[shift];
	const struct nvmeibc_roles_bmps *base = &cd->roles_bmps[0]; // Roles for shift 0 are initially calculated. All other roles are calculated based on them

	// Those are actually always the same, they are generated here for sake of performance
	// as later this whole structure may be used as a whole.
	bmps->raid.data = nvmeibc_raid1_get_data_bmp(pr);
	bmps->raid.pari = nvmeibc_raid1_get_parities_bmp(pr);
	bmps->raid.all = nvmeibc_raid1_get_all_bmp(pr);

	if (shift) {
		// Convert segments to roles
		bmps->rw =   ror32_width(base->rw,   shift, pr->replicas);
		bmps->w =    ror32_width(base->w,    shift, pr->replicas);
		bmps->wp =   ror32_width(base->wp,   shift, pr->replicas);
		bmps->dead = ror32_width(base->dead, shift, pr->replicas);

		bmps->readable =       ror32_width(base->readable,       shift, pr->replicas);
		bmps->readable_sync =  ror32_width(base->readable_sync,  shift, pr->replicas);
		bmps->dbits_on_mask =  ror32_width(base->dbits_on_mask,  shift, pr->replicas);
		bmps->dbits_off_mask = ror32_width(base->dbits_off_mask, shift, pr->replicas);
		bmps->local_access =   ror32_width(base->local_access,   shift, pr->replicas);

		// Those are the inverse: they have to be converted from roles to segments
		bmps->data_sgmnts = rol32_width(base->data_sgmnts, shift, pr->replicas);
		bmps->pari_sgmnts = rol32_width(base->pari_sgmnts, shift, pr->replicas);
	}

	// Calculate flags based on roles
	nvmeibc_update_roles_bmps_flags(bmps);
}

static void nvmeibc_raid1_fill_calculated_data(struct nvmeibc_raid1 *r1)
{
	// Deduce bitmaps based on corresponding segments access mode on each segment level
	struct nvmeibc_disk_segment *seg;
	struct dp_topology_traits *topo_traits = &r1->calculated_data.topo_traits;
	struct nvmeibc_roles_bmps *base = &r1->calculated_data.roles_bmps[0]; // Roles for shift 0 are initially calculated. All other roles are calculated based on them
	int si;
	memset(&r1->calculated_data, 0, sizeof(r1->calculated_data));
	raid1_for_each_seg(r1, seg, si) {
		nvmeibc_raid1_acm2bmp(seg->toma_acm, si, base);
		if (nvmeibc_disk_is_access_local(seg->disk))
			base->local_access |= (1 << si);
	}

	base->readable = base->wp | base->rw; // Readable ==> RW or W+
	base->readable_sync = base->readable | (base->w & ~base->wm); // == ~(D or W-)
	base->dbits_on_mask = base->dead; // Access mode dead yields potential turn on (in case of write attempt)
	base->dbits_off_mask = base->w;   // Access mode write only yields potential turn off (in case of full overwrite)

	base->data_sgmnts = nvmeibc_raid1_get_data_bmp(r1);
	base->pari_sgmnts = nvmeibc_raid1_get_parities_bmp(r1);

	topo_traits->n_parities = r1->replicas - r1->slice_size;

	// Create a variation of bitmaps for each possible role shift to avoid those calculations later
	for (si = 0; si < r1->replicas; ++si) {
		__calc_rotated_bmps(r1, si);
	}
	{
		const bool can_view_lock = __calc_should_use_view_lock_on_read(r1);
		for (si = 0; si < r1->replicas; ++si)
			r1->segments[si].is_safe_for_view_lock = can_view_lock;
	}
}

void nvmeibc_topology_fill_all_topo_raids_calculated_data(struct nvmeibc_topology *t)
{
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	int c, r;
	t->max_n_segs_in_praid = 0;
	t->is_safe_for_view_lock = true;
	topo_for_each_raid1(t, chunk, c, r1, r) {
		nvmeibc_raid1_fill_calculated_data(r1);
		MAX_WITH(t->max_n_segs_in_praid, r1->replicas); // In actual product all praid has the same structure, so this code can be simplified to jsut copy the value from prev topology or take it from first praid
		t->is_safe_for_view_lock &= r1->segments[0].is_safe_for_view_lock;
	}
}

