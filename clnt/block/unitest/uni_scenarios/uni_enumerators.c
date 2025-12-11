/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_enumerators.h"
#include "../nvmeibc_block_common.h"
#include "../uni_framework/unitest_defs.h"
#include "../uni_framework/range_algorithms.h"
#include "nvmeib_macro_magic.h"
#include "nvmeib_jdr.h"

void jdr_write_topology_sgmnts(struct jdr* jdr, char const* name, struct topology_sgmnts_t* topology_sgmnts)
{
	jdr_object_scope(jdr, name);

	jdr_write_fundamental_s_array(jdr, "dgrd_sgmnt", topology_sgmnts->dgrd_sgmnts);
	{
		jdr_array_scope(jdr, STRINGIFY(dgrd_modes));
		array_foreach(mode, topology_sgmnts->dgrd_modes){
			jdr->ops.ascii(jdr, NULL, nvmeibt_client_topo_seg_access_mode_to_str(*mode));
		}
	}
}


bool __topo_enum_exhausted(struct topologies_enumerator* self){
	(void)self;
	BUG(); //if you are here it means you consumed all values and need to reinitialize the enumerator again
	return false;
}

// Just for debugging
bool __topo_enum_move_2_index(struct topologies_enumerator* self, u32 index){
	self->impl.curr_permutation += 1;
	if (self->impl.curr_permutation >= self->impl.n_permutations){
		BUG();
	} else {
		const struct itopo_enum_perm_state state = self->impl.permutations[index];
		struct topology_sgmnts_t curr_topo = {
			.modes = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW},
			.dgrd_sgmnts = {state.desc[0].sgmnt, state.desc[1].sgmnt},
			.dgrd_modes = {state.desc[0].mode, state.desc[1].mode}
		};
		curr_topo.modes[curr_topo.dgrd_sgmnts[0]] = curr_topo.dgrd_modes[0];
		curr_topo.modes[curr_topo.dgrd_sgmnts[1]] = curr_topo.dgrd_modes[1];
		self->curr = curr_topo;
	}
	return true;
}

bool __topo_enum_move_next(struct topologies_enumerator* self){
	self->impl.curr_permutation += 1;
	if (self->impl.curr_permutation >= self->impl.n_permutations){
		self->move_next = __topo_enum_exhausted;
		return false;
	} else {
		const struct itopo_enum_perm_state state = self->impl.permutations[self->impl.curr_permutation];
		struct topology_sgmnts_t curr_topo = {
			.modes = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW},
			.dgrd_sgmnts = {state.desc[0].sgmnt, state.desc[1].sgmnt},
			.dgrd_modes = {state.desc[0].mode, state.desc[1].mode}
		};
		curr_topo.modes[curr_topo.dgrd_sgmnts[0]] = curr_topo.dgrd_modes[0];
		curr_topo.modes[curr_topo.dgrd_sgmnts[1]] = curr_topo.dgrd_modes[1];
		self->curr = curr_topo;
	}
	return true;
}

int topo_enum_tostring(const struct topologies_enumerator*s, char buf[64]) {
	const struct topology_sgmnts_t *c = &s->curr;
	return scnprintf(buf, 64, "{Topo: Seg[%d]=%4s Seg[%d]=%4s}", c->dgrd_sgmnts[0], nvmeibt_client_topo_seg_access_mode_to_str(c->dgrd_modes[0]), c->dgrd_sgmnts[1], nvmeibt_client_topo_seg_access_mode_to_str(c->dgrd_modes[1]));
}

static inline void __init_topologies_enumerator(struct topologies_enumerator *self) {
	*self = (struct topologies_enumerator) {
		 .curr = {
			 .modes = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_INVALID},
			 .dgrd_sgmnts = {-1,-1},
			 .dgrd_modes = {NVMEIBTC_DS_MODE_INVALID, NVMEIBTC_DS_MODE_INVALID}
		 },
		 .impl = {
			 .n_permutations = -1,
			 .curr_permutation = -1
		 },
		 .move_next = __topo_enum_move_next
	};
}

static inline u8 __init_relevant_sgmnts_mapping(const struct TstPRaid sraid, u8 sgmnts[N_MAX_RAID_SLICE_LEN]) {
	const u32 n_pari = sraid.cpr->replicas - sraid.cpr->slice_size;
	const u32 n_data = min(3U, sraid.cpr->slice_size); //lock_porter + any data sgmnt; data sgmnt1 + data sgmnt2
	const u8 n_relevant_sgmnts = n_data + n_pari;
	const u8 slice_size = sraid.cpr->slice_size;

	range_fill(sgmnts, sgmnts+N_MAX_RAID_SLICE_LEN, -1);

	for (u8 d = 0; d < n_data; ++d)
		sgmnts[d] = d;

	for (u8 p = 0; p < n_pari; ++p)
		sgmnts[n_data+p] = slice_size + p;

	return n_relevant_sgmnts;
}

static inline struct itopo_enum_perm_state create_itopo_enum_perm_state(u8 sx, const enum NVMEIBTC_DS_MODE mode_x, u8 sy, const enum NVMEIBTC_DS_MODE mode_y) {
	return (struct itopo_enum_perm_state){.desc={{.sgmnt=sx, .mode=mode_x}, {.sgmnt=sy, .mode=mode_y}}};
}

struct topologies_enumerator __impl_create_no_protection_topo_enum_single_protection(struct TstPRaid sraid){ //
	u32 curr_i = 0;
	u8 sgmnts[N_MAX_RAID_SLICE_LEN];
	const u8 n_relevant_sgs = __init_relevant_sgmnts_mapping(sraid, sgmnts);
	struct topologies_enumerator ienum;
	__init_topologies_enumerator(&ienum);

	for (u8 x = 0; x < n_relevant_sgs; ++x){
		const u8 sx = sgmnts[x];
		const u8 sy = sx ? 0 : 1; //always in RW state

		const struct itopo_enum_perm_state x_dead
			= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_DEAD}, {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_RW}}};

		const struct itopo_enum_perm_state x_writable
			= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_W}, {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_RW}}};

		ienum.impl.permutations[curr_i++] = x_dead;
		ienum.impl.permutations[curr_i++] = x_writable;
	}
	ienum.impl.n_permutations = curr_i;
	BUG_ON(ienum.impl.n_permutations != 2*n_relevant_sgs);

	return ienum;
};

struct topologies_enumerator __impl_create_no_protection_topo_enum_double_protection(struct TstPRaid sraid){ //
	u32 curr_i = 0;
	u8 sgmnts[N_MAX_RAID_SLICE_LEN];
	const u8 n_relevant_sgs = __init_relevant_sgmnts_mapping(sraid, sgmnts);
	struct topologies_enumerator ienum;
	__init_topologies_enumerator(&ienum);

	for (u8 x = 0; x < n_relevant_sgs; ++x){
		for (u8 y = x+1; y < n_relevant_sgs; ++y){
			const u8 sx = sgmnts[x];
			const u8 sy = sgmnts[y];

			const struct itopo_enum_perm_state both_dead
				= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_DEAD}, {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_DEAD}}};

			const struct itopo_enum_perm_state x_dead
				= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_DEAD}, {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_W}}};

			const struct itopo_enum_perm_state y_dead
				= {.desc={{.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_DEAD}, {.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_W}}};

			const struct itopo_enum_perm_state both_w
				= {.desc={{.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_W}, {.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_W}}};

			ienum.impl.permutations[curr_i++] = both_dead;
			ienum.impl.permutations[curr_i++] = x_dead;
			ienum.impl.permutations[curr_i++] = y_dead;
			ienum.impl.permutations[curr_i++] = both_w;
		}
	}
	ienum.impl.n_permutations = curr_i;
	//just simple combinatorics:
	//* C(n,2) == n*(n-1)/2, for EC 8+2 we have 5 interesting sgmnts C(5, 2) == 10 (without repetions and order is not important)
	//depends on at_most_one_dead we have 3 or 4 sub cases
	BUG_ON(ienum.impl.n_permutations != 4*n_relevant_sgs*(n_relevant_sgs-1)/2);

	return ienum;
};

struct topologies_enumerator __impl_create_no_protection_topo_enum_double_protection_ordered(struct TstPRaid sraid){ //
	u32 curr_i = 0;
	u8 sgmnts[N_MAX_RAID_SLICE_LEN];

	const u8 n_relevant_sgs = __init_relevant_sgmnts_mapping(sraid, sgmnts);
	struct topologies_enumerator ienum;
	__init_topologies_enumerator(&ienum);

	for (u8 x = 0; x < n_relevant_sgs; ++x){
		//const struct itopo_enum_perm_state clean
			//= {.desc={{.sgmnt=sgmnts[x], .mode=NVMEIBTC_DS_MODE_RW}, {.sgmnt=sgmnts[x], .mode=NVMEIBTC_DS_MODE_RW}}};
		for (u8 y = x+1; y < n_relevant_sgs; ++y){
			const u8 sx = sgmnts[x];
			const u8 sy = sgmnts[y];

			const struct itopo_enum_perm_state x_dead
				= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_DEAD},
						  {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_RW}}};

			const struct itopo_enum_perm_state both_dead
				= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_DEAD},
						  {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_DEAD}}};

			const struct itopo_enum_perm_state y_dead_x_w
				= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_W},
						  {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_DEAD}}};

			const struct itopo_enum_perm_state both_w
				= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_W},
						  {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_W}}};

			const struct itopo_enum_perm_state x_w
				= {.desc={{.sgmnt=sx, .mode=NVMEIBTC_DS_MODE_W},
						  {.sgmnt=sy, .mode=NVMEIBTC_DS_MODE_RW}}};


			ienum.impl.permutations[curr_i++] = x_dead;
			ienum.impl.permutations[curr_i++] = both_dead;
			ienum.impl.permutations[curr_i++] = y_dead_x_w;
			ienum.impl.permutations[curr_i++] = both_w;
			ienum.impl.permutations[curr_i++] = x_w;
		}	// Y is RW need to change X from W ->RW since X++ will be DEAD
	}
	ienum.impl.n_permutations = curr_i;
	//just simple combinatorics:
	//* C(n,2) == n*(n-1)/2, for EC 8+2 we have 5 interesting sgmnts C(5, 2) == 10 (without repetions and order is not important)
	//depends on at_most_one_dead we have 3 or 4 sub cases
	//BUG_ON(ienum.impl.n_permutations != 4*n_relevant_sgs*(n_relevant_sgs-1)/2);

	return ienum;
};

struct topologies_enumerator create_no_protection_topo_enum_ordered(struct TstPRaid sraid){
	if (2 == (sraid.cpr->replicas - sraid.cpr->slice_size)){
		return __impl_create_no_protection_topo_enum_double_protection_ordered(sraid);
	} else{
		return __impl_create_no_protection_topo_enum_single_protection(sraid);
	}
}

struct topologies_enumerator create_no_protection_topo_enum(struct TstPRaid sraid){
	BUG_ON((sraid.vsi.chunk != 0) && (sraid.vsi.raid != 0));
	if (2 == (sraid.cpr->replicas - sraid.cpr->slice_size)){
		return __impl_create_no_protection_topo_enum_double_protection(sraid);
	} else{
		return __impl_create_no_protection_topo_enum_single_protection(sraid);
	}
}

const enum NVMEIBTC_DS_MODE nvmeibtc_dgdrd_modes[] = {NVMEIBTC_DS_MODE_W, NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_W_NO_DIRTY, NVMEIBTC_DS_MODE_W_IS_DIRTY};

struct topologies_enumerator create_topo_random_enum(struct TstPRaid sraid, u16 max_non_deg, u16 max_1_deg, u16 max_2_deg)
{
	struct topologies_enumerator topo_enum;
	__init_topologies_enumerator(&topo_enum);
	topo_enum.impl.n_permutations = 0;
	BUG_ON((size_t)(max_non_deg + max_1_deg + max_2_deg) > ARRAY_SIZE(topo_enum.impl.permutations));

	for (int n_deg = 0; n_deg < 3; n_deg++) {
		u16 max_deg_perms = n_deg == 0 ? max_non_deg : (n_deg == 1 ? max_1_deg : max_2_deg);
		u16 n_deg_perms;
		struct topologies_enumerator deg_topo_enum;
		if (!max_deg_perms)
			continue;
		deg_topo_enum = create_all_topo_enum(sraid, n_deg);
		n_deg_perms = min(deg_topo_enum.impl.n_permutations, max_deg_perms);
		if (!n_deg_perms)
			continue;
		range_shuffle(deg_topo_enum.impl.permutations, deg_topo_enum.impl.permutations + deg_topo_enum.impl.n_permutations);
		range_copy(&deg_topo_enum.impl.permutations[0], &deg_topo_enum.impl.permutations[n_deg_perms], (&topo_enum.impl.permutations[topo_enum.impl.n_permutations]));
		topo_enum.impl.n_permutations += n_deg_perms;
	}

	range_shuffle(topo_enum.impl.permutations, topo_enum.impl.permutations + topo_enum.impl.n_permutations);
	return topo_enum;
};

struct topologies_enumerator create_all_topo_enum(struct TstPRaid sraid, u16 n_deg) {
	u8 sgmnts[N_MAX_RAID_SLICE_LEN];
	const u8 n_relevant_sgs = __init_relevant_sgmnts_mapping(sraid, sgmnts);
	struct topologies_enumerator ienum;

	BUG_ON((sraid.vsi.chunk != 0) && (sraid.vsi.raid != 0));

	__init_topologies_enumerator(&ienum);
	ienum.impl.n_permutations = 0;

	switch (n_deg) {
	case 0:
		// Add non degraded topo
		ienum.impl.permutations[ienum.impl.n_permutations++] = create_itopo_enum_perm_state(0, NVMEIBTC_DS_MODE_RW, 1, NVMEIBTC_DS_MODE_RW);
		break;
	case 1:
		// Generate all single degraded possibilities
		for (u8 x = 0; x < n_relevant_sgs; ++x) {
			const u8 sx = sgmnts[x];
			const u8 sy = sgmnts[(sx + 1) % n_relevant_sgs]; // sy should be different than sx
			array_foreach(m, nvmeibtc_dgdrd_modes)
				ienum.impl.permutations[ienum.impl.n_permutations++] = create_itopo_enum_perm_state(sx, *m, sy, NVMEIBTC_DS_MODE_RW);
		}
		break;
	case 2:
		// Generate all double degraded (including w+) possibilities
		for (u8 x = 0; x < n_relevant_sgs; ++x) {
			for (u8 y = x+1; y < n_relevant_sgs; ++y){
				const u8 sx = sgmnts[x];
				const u8 sy = sgmnts[y];
				array_foreach(mx, nvmeibtc_dgdrd_modes) {
					range_foreach(my, mx, nvmeibtc_dgdrd_modes + ARRAY_SIZE(nvmeibtc_dgdrd_modes)) {
						ienum.impl.permutations[ienum.impl.n_permutations++] = create_itopo_enum_perm_state(sx, *mx, sy, *my);
						if (*mx==*my)
							continue;
						ienum.impl.permutations[ienum.impl.n_permutations++] = create_itopo_enum_perm_state(sx, *my, sy, *mx);
					}
				}
			}
		}
		break;
	default:
		BUG();
	}

	return ienum;
};

/******************************************************************************/
bool __lock_owner_shift_slices_enum_exhausted(struct io_geometry_enumerator* self){
	(void)self;
	BUG(); //if you are here it means you consumed all values and need to reinitialize the enumerator again
	return false;
}

static bool __geometry_enum_move_next_any_raid(struct io_geometry_enumerator* self, u32 n_slices, u8 n_blocks_to_incr) {
	struct test_context *env = &self->impl.env;
	const u8 n_max_slices = nvmeibc_cinst_get_blok_p(env->dev)->binje;
	const u8 slice_size = env->sraid.cpr->slice_size;
	const u8 aligned_size = n_max_slices * slice_size;
	BUG_ON(n_max_slices != env->dev->dp.p.binje);
	(void) aligned_size; // TODO - replace slice_size below with aligned_size. Currently there is a bug in expectors preventing MS

	self->impl.length += n_blocks_to_incr;	// Advance length within slice
	if (slice_size < (self->impl.offset + self->impl.length)) {	// Advance offset withing slice
		self->impl.length = 1;
		self->impl.offset += 1;
		if (self->impl.offset == slice_size) {	// Advance to next blockset
			self->impl.offset = 0;
			self->impl.vlba_blkst += slice_size * n_slices;
		}
	}
	if ((self->impl.vlba_blkst + slice_size) <= env->dev->size) {
		struct io_geometry *g = &self->curr;
		g->offset = self->impl.offset;
		g->length = self->impl.length;
		g->bytes = NVMEIBC_SECTOR2BYTE(self->impl.length);
		g->vlba = self->impl.vlba_blkst + self->impl.offset;
		g->traits = get_io_slice_traits(*env, g->vlba, g->length);
		BUG_ON(self->curr.traits.tx_bm == 0);
		//unitest_print("ios_itr: vlba=%lld, [%d..%d), txbm=0x%x\n", g->vlba, g->offset, g->offset + g->length, g->traits.tx_bm);

		return true;
	}

	self->move_next = __lock_owner_shift_slices_enum_exhausted;
	return false;
}

static bool __geometry_enum_is_io_within_raid(struct io_geometry_enumerator* self){
	struct test_context *env = &self->impl.env;
	return (self->curr.traits.chunk_idx == env->sraid.vsi.chunk)
		   && (self->curr.traits.raid_idx == env->sraid.vsi.raid);
}

static bool __geometry_enum_move_next(struct io_geometry_enumerator* self, u32 n_slices, u8 n_blocks_to_incr) {
	bool result = false;
	do{
		result = __geometry_enum_move_next_any_raid(self, n_slices, n_blocks_to_incr);
	} while (result && !__geometry_enum_is_io_within_raid(self));
	return result;
}

static bool __lock_owner_shift_slices_enum_move_next(struct io_geometry_enumerator* self){
	const u8 n_blocks_to_incr = 1;
	return __geometry_enum_move_next(self, 1 << LOCKSET_SHIFT, n_blocks_to_incr);
}

static bool __blocksets_enum_move_next(struct io_geometry_enumerator* self){
	const u8 n_blocks_to_incr = 1;
	return __geometry_enum_move_next(self, LOCKSET_SLICES, n_blocks_to_incr);
}

static bool __blocksets_enum_move_next_slice(struct io_geometry_enumerator* self){
	return __geometry_enum_move_next(self, 1, self->impl.env.sraid.cpr->slice_size);
}

int io_geometry_enum_tostring(const struct io_geometry_enumerator*s, char buf[64]) {
	const struct io_geometry *g = &s->curr;
	return scnprintf((void*)buf, 64, "{ios:vlba=%d, [%d..%d)}" /*, txbm=0x%x}"*/, g->vlba, g->offset, g->offset + g->length /*, g->traits.tx_bm*/);
}

void jdr_write_io_geometry(struct jdr* jdr, char const* name, struct io_geometry* iog)
{
	jdr_object_scope(jdr, name);
	jdr_write_slice_traits(jdr, "traits", &iog->traits);
	jdr_write_bitfield(jdr, iog, vlba);
	jdr_write(jdr, iog, bytes);
	jdr_write_bitfield(jdr, iog, length);
	jdr_write_bitfield(jdr, iog, offset);
}

struct io_geometry_enumerator
create_lock_owner_shift_slices_enum(struct test_context env){
	return (struct io_geometry_enumerator){
		.curr = {{0},0,0,0,0},
		.move_next = __lock_owner_shift_slices_enum_move_next,
		.impl = {.env=env, .vlba_blkst=0, .offset=0, .length=0}
	};
}

struct io_geometry_enumerator create_blocksets_enum(struct test_context env){
	return (struct io_geometry_enumerator){
		.curr = {{0},0,0,0,0},
		.move_next = __blocksets_enum_move_next,
		.impl = {.env=env, .vlba_blkst=0, .offset=0, .length=0}
	};
}


struct io_geometry_enumerator create_full_slice_blocksets_enum(struct test_context env){
	return (struct io_geometry_enumerator){
		.curr = {{0},0,0,0,0},
		.move_next = __blocksets_enum_move_next_slice,
		.impl = {.env=env, .vlba_blkst=0, .offset=0, .length=0}
	};
}


bool __volume_sgmnts_move_next_exhausted(struct mgmt_sgmnts_enumerator* self){
	(void)self;
	BUG();
	return false;
}

bool __volume_sgmnts_move_next(struct mgmt_sgmnts_enumerator* self){
	self->impl.index += 1;
	if (self->impl.index < self->impl.size){
		self->curr = self->impl.sgmnts[self->impl.index];
		return true;
	} else {
		self->move_next = __volume_sgmnts_move_next_exhausted;
		return false;
	}
}

void __volume_sgmnts_reset(struct mgmt_sgmnts_enumerator* self){
	self->impl.index = -1;
	self->move_next = __volume_sgmnts_move_next;
}

struct mgmt_sgmnts_enumerator
create_mirror_volumes_sgmnts_enum(struct NVMeshSystem *sys){
	struct mgmt_sgmnts_enumerator result = {
		.curr = {0},
		.move_next = __volume_sgmnts_move_next,
		.reset = __volume_sgmnts_reset,
		.impl = {.index=-1, .size=-1}
	};

	u16 index = 0;
	struct tTopoOfNVMesh *cf = &sys->tcf;
	for (int vi = 0; vi < cf->nVolumes; vi++) {
		struct tTopoOfVolume *vol = &cf->vols[vi];
		struct disk_range* raid_disk_ranges = sys->mdb.vols[vi].segs;
		if (!tTopoOfVolume_isMirrored(vol))
			continue;
		for (int ci = 0; ci < vol->nChunks; ci++){
			const struct tTopoOfRaid0Chunk *chunk = &vol->chunks[ci];
			for (int ri = 0; ri < chunk->stripeWidth; ri++) {
				struct tTopoOfPraid *sraid = &chunk->raids[ri];
				for (int si = 0; si < sraid->header.n_segments; si++) {
					struct volume_segment_index vsi = {.volume=vi, .chunk=ci, .raid = ri, .segment = si};
					struct test_context env = {
						.sys = sys,
						.client = sys->clients,
						.dev = sys->clients->devs[vi],
						.sraid = {.cpr = raid_disk_ranges, .tpr = sraid, .vsi = vsi}
					};
					result.impl.sgmnts[index++] = env;
					BUG_ON(index >= ARRAY_SIZE(result.impl.sgmnts));
				}
				raid_disk_ranges += raid_disk_ranges->replicas;
			}
		}
	}
	result.impl.size = index;
	return result;
}



