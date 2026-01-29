/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/* Sets of unitest utils for enumeration of topologies */
#include "../bunitest.h"
#include "nvmesh_sim.h"

struct jdr;

//describes raid topology
struct topology_sgmnts_t{
	enum NVMEIBTC_DS_MODE modes[N_MAX_RAID_SLICE_LEN];
	//in the worst case we have 2 degraded segments in a topology, so instead of searching for them
	//a shortcuts are provided; If there is no dgrd sgmnts, the variables content is undefined
	//if there is a dead sgmnt it will be in the first element, the second one may be also dead
	raid_sgmnt_t dgrd_sgmnts[2];
	enum NVMEIBTC_DS_MODE dgrd_modes[2];
};

void jdr_write_topology_sgmnts(struct jdr* jdr, char const* name, struct topology_sgmnts_t* topology_sgmnts);

#define topo_enum_fmt "{Topo: Seg[@SI]=@STR Seg[@SI]=@STR}"
#define topo_enum_args(s)                                                                                              \
	(s)->curr.dgrd_sgmnts[0], nvmeibt_client_topo_seg_access_mode_to_str((s)->curr.dgrd_modes[0]),                    \
	(s)->curr.dgrd_sgmnts[1], nvmeibt_client_topo_seg_access_mode_to_str((s)->curr.dgrd_modes[1])

static const struct topology_sgmnts_t perfect_topo_sgmnts = {
	.modes = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW},
	.dgrd_sgmnts = {0,1},
	.dgrd_modes = {NVMEIBTC_DS_MODE_RW,NVMEIBTC_DS_MODE_RW}
};

//switching topology is an expensive operation
//so we should prefer to switch raid topology and run maximum amount of tests under it
//the tests themself prefer to consume the topology by roles
struct topology_roles_t{
	enum NVMEIBTC_DS_MODE modes[N_MAX_RAID_SLICE_LEN];
	//if there is a dead sgmnt it will be in the first element, the second one may be also dead
	raid_role_t dgrd_roles[2];
	enum NVMEIBTC_DS_MODE dgrd_modes[2];
};

static inline struct topology_roles_t
sim_convert_topo_presentations(u32 d0_sgmnt_id, u32 n_replicas, struct topology_sgmnts_t topo){
	struct topology_roles_t res;
	for (raid_sgmnt_t i = 0; i < n_replicas; ++i){
		res.modes[i] = topo.modes[(i + d0_sgmnt_id) % n_replicas];
	}
	for (u32 j = 0; j < ARRAY_SIZE(topo.dgrd_sgmnts); ++j){
		res.dgrd_modes[j] = topo.dgrd_modes[j];
		res.dgrd_roles[j] = (topo.dgrd_sgmnts[j] + d0_sgmnt_id) % n_replicas;
	}
	return res;
}


struct itopo_enum_perm_state {
	struct {					// By default all segs are RW except for up to 2.
		u8 sgmnt;				// Choose a segment from praid
		enum NVMEIBTC_DS_MODE mode;
	} desc[2];
};

//given a raid, enumerates all "interesting" topologies; the usage example is comming
struct topologies_enumerator{
	struct topology_sgmnts_t curr;
	bool (*move_next)(struct topologies_enumerator* /*self*/);

	struct {						// this is the enumerator private state
		u16 n_permutations;			// Total amount of permitations that this exhaustive iterator will do
		u16 curr_permutation;		// currrent index
		struct itopo_enum_perm_state permutations[4*N_MAX_RAID_SLICE_LEN*(N_MAX_RAID_SLICE_LEN-1)];	// since N_MAX_RAID_SLICE_LEN is a small number we may keep all the state in a single array but this may be changed in future
	} impl;
};

// Randomly Enumerates all possible non/single/double degraded topologies
struct topologies_enumerator create_all_topo_random_enum(struct TstPRaid sraid, u16 n_perms, bool all_topos);

// Enumerates all possible non/single/double degraded topologies
struct topologies_enumerator create_all_topo_enum(struct TstPRaid sraid);

// Enumerates single/double degraded topologies
struct topologies_enumerator create_no_protection_topo_enum(struct TstPRaid sraid);
struct topologies_enumerator create_no_protection_topo_enum_ordered(struct TstPRaid sraid);
int topo_enum_tostring(const struct topologies_enumerator*s, char buf[64]);

//usage example:
//
//struct itopology_enumerator itopos = create_no_protection_topo_enum(ctx.env.sraid, true);
//while(itopos.move_next(&itopos)){
//    const struct topology_sgmnts_t topo = itopos.curr;
//    ...
//}
//if move_next returned "false" the "curr" member variable contains garbage and should not be used
//if needed it is possible to add "reset" functionality to the enumerator, to aoiv costly initialization


//enumerates all possible IO to "interesting" slices
struct io_geometry_enumerator{
	struct io_geometry{
		struct slice_traits traits;
		u64 vlba  : 32; 			// vlba the IO is going into
		u16 bytes; 					// the bio length in bytes
		u8 length: 4; 				// the bio length in blocks
		u8 offset: 4; 				// from the slice start
	} curr;

	bool (*move_next)(struct io_geometry_enumerator * /*self*/);

	struct {						// Internal state of the iterator
		struct test_context env;
		u64 vlba_blkst : 32;		// Vlba of blockset where io occurs
		u8 offset;
		u8 length;
	} impl;
};

void jdr_write_io_geometry(struct jdr* jdr, char const* name, struct io_geometry* iog);

//creates enumerator, which produces all possible IO permutations to slices (full + partial with different offset and length);
//the slices are cherry picked - only those, where lock ownership is changed - slice1(D0, P, Q) != slice2(D0, P, Q)
struct io_geometry_enumerator create_lock_owner_shift_slices_enum(struct test_context env);
struct io_geometry_enumerator create_blocksets_enum(struct test_context env);
struct io_geometry_enumerator create_full_slice_blocksets_enum(struct test_context env);
int io_geometry_enum_tostring(const struct io_geometry_enumerator*, char buf[64]);


// For debugging: move to topology at index
bool __topo_enum_move_2_index(struct topologies_enumerator* self, u32 index);


struct mgmt_sgmnts_enumerator{
	struct test_context curr;

	bool (*move_next)(struct mgmt_sgmnts_enumerator*);
	void (*reset)(struct mgmt_sgmnts_enumerator*);

	struct {
		struct test_context sgmnts[NVMESH_N_PHYS_DISKS];
		s16 index;
		s16 size;
	} impl;
};

struct mgmt_sgmnts_enumerator create_mirror_volumes_sgmnts_enum(struct NVMeshSystem *sys);



