/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_lock_server.h"
#include "block/nvmeibc_topology.h"
#include "nvmeibc_block_dp_locks_scheme.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "common/nvmeib_str.h"

/*********************** Todo: Move the below into Toma ***********************/

static bool __is_my_owner_lock(struct lock_ownership_map *lmap, int si) {
	return ((lmap->si[0] == (s8)si) || ((lmap->si[1] == (s8)si) &&
						(lmap->type[1] == NVMEIBTC_DS_OWNER_MODE_SECONDARY)));
}

#define __is_not_lockable(r1, si) \
	(((r1)->lock_scheme.type == OWNER_SCHEME_FIRST_L_INC_A)&&((si) >= r1->lock_scheme.max_n_owners))

static void __import_seg_map_from_toma(struct nvmeibc_raid1* r1, int my_si, const struct nvmeibt_client_topo_disk_segment *t_si)
{
	struct lock_ownership_map *lmap = &r1->segments[my_si].lmap;
	struct nvmeibc_disk_segment *seg;
	int si, l;
	lock_ownership_map_poison(lmap);
	/* Step 1: Pack string representation to indices - add valid locks to count */
	for (l = 0; l < N_MAX_RAID_LOCKS; l++) {
		if (t_si->owners[l].seg_uuid[0] == 0) { // break no more valid locks
			lmap->type[l] = NVMEIBTC_DS_OWNER_MODE_NO_OWNER;
			lmap->si[l] = -1;
			break;
		}
		raid1_for_each_seg(r1, seg, si) { // find correlating segment and keep lock type and segment index and increase lock count
			if (ARE_UUIDS_EQ(seg->uuid, t_si->owners[l].seg_uuid)) {
				lmap->type[l] = t_si->owners[l].mode;
				lmap->si[  l] = si;
				lmap->n_locks++;
				break;
			}
		}
		if (unlikely(si == r1->replicas)) {
			_NT(error_dp_lock_server_import_seg_map_from_toma, "TOMA given uuid for unknown segment_uuid=@SEGMENT_UUID, lock_type=@LOCK_TYPE_CHR", t_si->owners[l].seg_uuid, nvmeibtc_ds_owner_mode_to_chr(t_si->owners[l].mode));
			lmap->si[l] = -3;
			lmap->n_locks++;
			break;		// No reason to continue parsing. Locks are broken
		}
	}

	// If this segment is not a lockable segment there should be valid locks, if not (n_locks == 0)
	// If this segment is DEAD it cannot be marked as owner since previous
	// lockable segments also have no valid locks, so topology will not be ioable
	if (__is_not_lockable(r1,my_si)) {
		if ((!lmap->n_locks) && (r1->segments[my_si].toma_acm != NVMEIBTC_DS_MODE_DEAD)) {
			lmap->n_locks = 1;
			lmap->type[0] = NVMEIBTC_DS_OWNER_MODE_PRIMARY;
			lmap->si[  0] = my_si; 	/* Seg is marked as self owner for debug */
		}
	}
}

/***************************** Clnt side code *********************************/
#define __warn_lmap(expression) do {\
	if (unlikely(expression)) { \
		WARN(true, "nvmeibc: Toma bug, disabling IO, seg=%d lock=%d: " __stringify(expression) "\n", si, l); \
		goto _err; \
	} \
} while (0)

#define is_seg_dead(r, i) ((r)->segments[i].toma_acm == NVMEIBTC_DS_MODE_DEAD)

/* check lock-map object is valid (Toma lock calculation of topo was correct) */
static void lock_ownership_map_assert_valid(struct nvmeibc_raid1 *r1, int si)
{
	struct nvmeibc_disk_segment *seg = &r1->segments[si];
	struct lock_ownership_map *lmap = &seg->lmap;
	int l = 0;
	if (lmap->n_locks < 1) { /* Degraded N-replica: All lockable segs are down*/
		_NT(t_0lomav, "Seg @SI: Data available, No owner lock (@N_LOCKS)", si, lmap->n_locks);
		goto _err;
	}
	/* Owner lock must exist in sorted array */
	__warn_lmap(lmap->type[0] != NVMEIBTC_DS_OWNER_MODE_PRIMARY);
	__warn_lmap(r1->segments[lmap->si[0]].toma_acm != NVMEIBTC_DS_MODE_RW);
	__warn_lmap(lmap->n_locks > r1->replicas);
	for (l = 0; l < lmap->n_locks; l++) {	// Verify that lock segments are OK
		__warn_lmap((lmap->si[l] < 0)||(lmap->si[l] >= r1->replicas));
		//__warn_lmap(lmap->si[l] >= lock_scheme->max_n_owners); - Non lockable segment
		__warn_lmap(is_seg_dead(r1, lmap->si[l]));
		if (l > 1)
			__warn_lmap(lmap->type[l] != NVMEIBTC_DS_OWNER_MODE_COPY_OWNER);
	}
	/* Verify dual lock */
	if (lmap->n_locks > 1) {
		if (lmap->type[1] == NVMEIBTC_DS_OWNER_MODE_SECONDARY) { 	// At most 1 secondary owner exists on W+
			__warn_lmap(r1->segments[lmap->si[1]].toma_acm != NVMEIBTC_DS_MODE_W_NO_DIRTY);
		} else
			__warn_lmap(lmap->type[1] != NVMEIBTC_DS_OWNER_MODE_COPY_OWNER);
	}
	__warn_lmap(lmap->n_locks > r1->lock_scheme.max_n_owners);
	return;
_err:
	lmap->si[0] = -0xD;		// No owner, will disable IO for this raid
}

void lock_ownership_update_seg_map(struct nvmeibc_raid1* r1, int my_si,
					const struct nvmeibt_client_topo_disk_segment *t_si)
{
	__import_seg_map_from_toma(r1, my_si, t_si);
	BUG_ON(r1->lock_scheme.type != OWNER_SCHEME_SL_START_DEC_C);	// Only this one is used by mgmt, The rest were deprecated for now
	lock_ownership_map_assert_valid(r1, my_si);
}

void lock_ownership_map_to_string(const struct lock_ownership_map *lm,
								  char res[LOCK_OWNERSHIP_MAP_STRING_LEN])
{
	int i, pos = 0, len = LOCK_OWNERSHIP_MAP_STRING_LEN;
	for (i = 0; i< lm->n_locks; i++)
		pos += scnprintf(res + pos, len-pos, "%c%d,",
						nvmeibtc_ds_owner_mode_to_chr(lm->type[i]), lm->si[i]);
	if (likely(pos)) {
		res[pos - 1] = 0; // Delete the trailing ','
	} else {
		res[0] = '?'; res[1] = 0;	// Broken topology of praid, print "?"
	}
}

int get_owner_seg_slice_start(const struct nvmeibc_raid1 *r1, u64 rlba)
{
	return nvmeibc_locks_scheme_find_slice_start_seg(rlba/r1->slice_size, r1->replicas);
}

// get the default slice member for the lock when accessing a given LBA
static inline int __find_initial_owner_seg(u64 offset_in_blks, const struct nvmeibc_raid1*r1)
{
	return nvmeibc_locks_scheme_find_initial_owner_seg(&r1->lock_scheme, offset_in_blks, r1->replicas);
}

int get_owner_seg_of_lock(const struct nvmeibc_raid1 *r1, u64 rlba)
{
	const int o_cand = __find_initial_owner_seg(rlba/r1->slice_size, r1);
	return r1->segments[o_cand].lmap.si[0];
}

int get_owner_seg_of_read(const struct nvmeibc_raid1 *r1, u64 rlba)
{
	if (r1->replicas <= r1->lock_scheme.max_n_owners) {
		return get_owner_seg_of_lock(r1, rlba);	// Symetric locking
	} else { /* N-replica, Find readable seg. For now, its the first readable segment in
				slice (starting from slice start segment) */
		const int slice_start_si = get_owner_seg_slice_start(r1, rlba);
		const roles_bmp_t readable_roles_bmp = nvmeibc_raid1_get_roles_bmp(r1, slice_start_si, readable);
		const int readable_role = __ffs(readable_roles_bmp);

		return nvmeibc_raid1_role2seg(r1, slice_start_si, readable_role);
	}
}

void lock_ownership_build_raid_map(const struct nvmeibc_raid1 *r1, u64 rlba,
	enum nvmeib_block_io_op op, struct lock_ownership_map *raid_l)
{	/* Reduce Max amount of needed locks (to take/verify) for 'op' */
	if (!r1->use_rdma_locks) {
		raid_l->n_locks = 0; 	/* No need to lock this protection raid */
	} else {	// Note: Lock owner != Slice start
		const int o = __find_initial_owner_seg(rlba/r1->slice_size, r1);
		*raid_l = r1->segments[o].lmap;
		if (nvmeibc_should_use_view_lock(op, r1->segments))
			raid_l->n_locks = 1; /* View only owner lock (don't take it)*/
	}
}

void lock_ownership_update_raid_map(const struct nvmeibc_raid1 *r1, int si,
									char action)
{
	struct nvmeibc_disk_segment *seg;
	int i, l;
	if (action == 'd') { /* downgrade, remove segment si */
		raid1_for_each_seg(r1, seg, i) {
			struct lock_ownership_map *lmap= &seg->lmap;
			if (i != si) {
				BUG_ON(__is_my_owner_lock(lmap, si));	// Other segment points on this one as owner. How is it removed then ???
			}
			for (l = 0; l < lmap->n_locks; l++)
				if (lmap->si[l] > si)	lmap->si[l]--;
		}
	} else { /* u - upgrade, insert segment si */
		BUG_ON(!is_seg_dead(r1, si)); // New unused, unregistered seg cannot have lock map!
		raid1_for_each_seg(r1, seg, i) {
			struct lock_ownership_map *lmap= &seg->lmap;
			for (l = 0; l < lmap->n_locks; l++)
				if (lmap->si[l] >= si)	lmap->si[l]++;
		}
	}
	// Todo: Verify correctness like for each seg, like in lock_ownership_update_seg_map
}

#include "block/datapath_ec/nvmeibc_block_dp_ec.h"

sgmnts_bmp_t nvmeibc_calc_db_on_parities_segs(const struct nvmeibc_block_command *rldr) {
	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(rldr->ds);
	const int slice_start = rldr->o->mssa->owner_seg;
	/* Parities are rotated based on slice. Dead stay dead. Think about it... */
	return nvmeibc_raid1_get_roles_bmp(pr, slice_start, pari_sgmnts) & nvmeibc_raid1_get_sgmnts_bmp(pr, dbits_on_mask);
}

roles_bmp_t nvmeibc_calc_db_on_parities_roles(const struct nvmeibc_block_command *rldr) {
	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(rldr->ds);
	const int slice_start = rldr->o->mssa->owner_seg;
	/* Parities are rotated based on slice. Dead stay dead. Think about it... */
	return nvmeibc_raid1_get_roles_bmp(pr, slice_start, raid.pari) & nvmeibc_raid1_get_roles_bmp(pr, slice_start, dbits_on_mask);
}

bool dp_ec_can_fix_dbits(struct nvmeibc_block_command *rldr)
{
	union nvmeibc_dbits_entry dbits_pre = {.all_bits = rldr->rld.pre.bits.dirty};
	struct nvmeibc_raid1* pr = nvmeibc_disk_segment_get_praid(rldr->ds);
	const sgmnts_bmp_t db_turn_off = nvmeibc_raid1_get_sgmnts_bmp(pr, dbits_off_mask);
	int ind[2], i;
	nvmeibc_dbits_entry_get_ind(&dbits_pre, &ind[0], &ind[1]);
	for (i = 0; i < 2; i++) {
		// dirty bit encoding is 1 based
		if ((ind[i] != 0) && ((1 << (ind[i] - 1)) & db_turn_off)) {
			return true;  // We can sync!
		}
	}
	return false;
}

void nvmeibc_dbits_turn_on_convict(union nvmeibc_dbits_entry *e, struct dp_topology_traits const* topo_traits)
{
	struct nvmeibc_dbits_tx dbits;
	nvmeibc_dbits_tx_init_by_bmp(&dbits, topo_traits->n_parities, 0, 0, topo_traits->wm);
	e->all_bits = nvmeibc_dbits_tx_apply(e, &dbits);
}

#if 0
#include "block/controlpath/nvmeibc_b_cp_blkset_topo.h"
union nvmeibc_dbits_entry
nvmeibc_dbits_turn_on_by_tx(u16 d0_sgmnt_id, const struct nvmeibc_roles_bmps* bmps, roles_bmp_t tx_bm){
	union nvmeibc_dbits_entry res;
	const u32 n_replicas = hweight16(bmps->data|bmps->pari);
	const roles_bmp_t dead_roles = nvmeibc_get_tx_dead_roles(*bmps, tx_bm);
	const u32 dead_sgmnts = rol32_width(dead_roles, d0_sgmnt_id, n_replicas);
	struct nvmeibc_dbits_tx dbmap;
	union nvmeibc_dbits_entry old_dbits = { .all_bits = 0 };
	nvmeibc_dbits_tx_init_by_bmp(&dbmap, dead_sgmnts, 0x0 /* No trun-off*/, 0, 0);
	res.all_bits = nvmeibc_dbits_tx_apply(&old_dbits, &dbmap);
	return res;
}
#endif
