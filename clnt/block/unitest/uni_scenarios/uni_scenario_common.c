/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_scenario_common.h"
#include "uni_framework/cond_wait_algorithms.h"
#include "uni_framework/range_algorithms.h"
#include "uni_framework/unitest_defs.h"
#include "uni_framework/bunitest_conf.h"
#include "uni_framework/simu_test.h"

#include "../nvmeibc_block_common.h"

const u64 ILLEGAL_DLBA = ~0;
const int ILLEGAL_JRNL_ENTRY = -1;

/******************************************************************************/
static struct __backup_sync_t{// Backup and restore sync/async mode.
	u16 modes;				// Stack for recursive calls of backup and destroy. 16 bits - 16 depth of recursion
	u16 sp;					// Stack pointer
} __backup_sync = {0,0};

static bool __backup_sync_push(bool mode) {
	BUG_ON(__backup_sync.sp >= 16);			// Wrong usage in unitest
	__backup_sync.modes |= ((int)mode << (__backup_sync.sp++));
	return mode;
}

static bool __backup_sync_pop(void) {
	BUG_ON(__backup_sync.sp == 0);			// Wrong usage in unitest
	return !!(__backup_sync.modes & (1 << (--__backup_sync.sp)));
}

/******************************************************************************/

u64 __hash_j2d(u64 key, unsigned int bits) {
	union jblock_md entry = {.raw = key};
	const u64 res = nvmeibc_block_dp_ec_jmd_decode_j2d_only(&entry);
	return res & (((1ULL << bits) << 1) - 1ULL);
}

/**
 The following 2 functions are "guessing" per segment dlba, which will be used for journal allocation
 "guessing" because of the following assumption
 the I/O is write and belongs to a single raid (actually validated, if get_io_traits function is used)
 the topology is perfect (all sgmnts = RW)
 */

void nvmeibc_guess_to_be_written_dlbas_for_slice(struct io_traits io_traits, u16 slice_idx,
												 u64 sgmnt2dlba[N_MAX_RAID_SLICE_LEN]) {
	struct slice_traits *slice = &io_traits.slices[slice_idx];

	u16 idx = slice_idx == 0 ? io_traits.start_first_slice : 0;
	const u16 limit = idx + hweight16(slice->tx_bm);

	for (; idx < limit; ++idx) {
		u16 sgmnt_idx = slice->role2sgmnt[idx];
		sgmnt2dlba[sgmnt_idx] = min(sgmnt2dlba[sgmnt_idx], slice->sgmnt2dlba[sgmnt_idx]);
	}

	for (idx = slice->slice_size; idx < slice->replicas; ++idx) {
		u16 sgmnt_idx = slice->role2sgmnt[idx];
		sgmnt2dlba[sgmnt_idx] = min(sgmnt2dlba[sgmnt_idx], slice->sgmnt2dlba[sgmnt_idx]);
	}
}

void nvmeibc_guess_to_be_written_dlbas_for_io(struct test_context env, struct io_traits io_traits,
											  u64 sgmnt2dlba[N_MAX_RAID_SLICE_LEN]) {
	BUG_ON(io_traits.n_slices == 0);
	BUG_ON(vlba2blockset(env, io_traits.vlba) != vlba2blockset(env, io_traits.vlba + io_traits.n_blocks - 1));
	range_fill(sgmnt2dlba, sgmnt2dlba + N_MAX_RAID_SLICE_LEN, ILLEGAL_DLBA);
	nvmeibc_guess_to_be_written_dlbas_for_slice(io_traits, 0, sgmnt2dlba); // first slice
	if (1 < io_traits.n_slices) {
		nvmeibc_guess_to_be_written_dlbas_for_slice(io_traits, 1, sgmnt2dlba); // second slice
	}
}

u64 vlba2blockset(struct test_context env, u64 vlba) {
	struct slice_traits slc = get_io_slice_traits(env, vlba, 1);
	const u64 blckset = slc.slba / LOCKSET_SLICES;
	BUG_ON(env.dev->size < vlba);
	return blckset;
}

// Matches the logic in dp_locks_get_TxID_dbits()
u32 nvmeibc_get_commited_txid(struct test_context env, struct slice_traits traits) {
	u32 committed_tx_id = 0;
	int li;
	for (li = 0; li < traits.rlmap.n_locks; li++) {
		const u16 si = traits.rlmap.si[li];
		const u16 role = traits.sgmnt2role[si];
		struct disk_range *sgmnt = &env.sraid.cpr[si];
		struct ramDiskSimulator *ssd = &env.sys->servers[sgmnt->node_id].ramDisk;
		u64 txid_address = traits.sgmnt2dlba[si] / LOCKSET_SLICES;
		u32 sgmnt_txid;
		BUG_ON(!nvmeibtc_ds_owner_mode_is_valid(traits.rlmap.type[li]));
		if ((1 << role) & traits.roles_bmps.readable) {
			ramDiskSimulator_read_txid(ssd, &sgmnt_txid, txid_address, 1);
			committed_tx_id = max(committed_tx_id, sgmnt_txid);
		}
	}
	return committed_tx_id;
}

void nvmeibc_wait_for_all_jam_entires_to_be_free(struct NVMeshSystem *sys, struct TstPRaid *r, struct nvmeibc_topology *t) {
	struct disk_range *rng = r->cpr;
	int s, n_seg = rng->replicas;
	// This function should use normal pause cont. However because unitest holds reference to topology, normal pause cont will never succeed.so alternative JAM specific function is used but it is unsafe becasue IO in air interracts incorrectly with pausable layer
	if (t)
		nvmeibc_topo_wait_for_single_topo_no_io(t->nt, 2);								// Unitest
	for (s = 0; s < n_seg; s++) {
		nvmeibc_jam_drain_disk_ops_block_idle_unsafe(sys->servers[rng[s].node_id].disk); // Drain JAM resets cmds or else abandon will not succeed
		//NVMeshSystem__invoke_pause_cont_on_disk(sys, rng[s].node_id);					// Todo: Use the normal function instead of the above JAM hack
	}
}

void nvmeibc_process_jrnl_entries_for_io(struct test_context env, const int sgmnt2entry[N_MAX_RAID_SLICE_LEN], enum nvmeibc_jam_jidx_event event) {
	for (u16 sgmnt_idx = 0; sgmnt_idx < env.sraid.cpr->replicas; ++sgmnt_idx) {
		if (sgmnt2entry[sgmnt_idx] != ILLEGAL_JRNL_ENTRY) {
			struct disk_range *sgmnt = &env.sraid.cpr[sgmnt_idx];
			struct nvmeibc_disk *cdisk = env.sys->servers[sgmnt->node_id].disk;
			nvmeibc_jam_simu_process_entry_event(cdisk, sgmnt2entry[sgmnt_idx], event);
		}
	}
}

void nvmeibc_alloc_jrnl_entries_for_io(struct test_context env, u64 vlba, u64 nblocks,
									   int sgmnt2entry[N_MAX_RAID_SLICE_LEN], sgmnts_bmp_t sgmnts_bmps) {
	u64 sgmnt2dlba[N_MAX_RAID_SLICE_LEN] = {ILLEGAL_DLBA};
	struct io_traits io_traits = get_io_traits(env, vlba, nblocks);
	u32 committed_tx_id = nvmeibc_get_commited_txid(env, io_traits.slices[0]);

	BUG_ON(vlba2blockset(env, vlba) != vlba2blockset(env, vlba + nblocks));
	range_fill(sgmnt2entry, sgmnt2entry + N_MAX_RAID_SLICE_LEN, ILLEGAL_JRNL_ENTRY);

	nvmeibc_guess_to_be_written_dlbas_for_io(env, io_traits, sgmnt2dlba);
	// ok now we have dlbas to be written & txid
	for (u16 sgmnt_idx = 0; sgmnt_idx < io_traits.slices[0].replicas; ++sgmnt_idx) {
		struct disk_range *sgmnt = &env.sraid.cpr[sgmnt_idx];
		struct serverSimulator *srv = &env.sys->servers[sgmnt->node_id];
		if (sgmnt2dlba[sgmnt_idx] != ILLEGAL_DLBA && (sgmnts_bmps & (1 << sgmnt_idx))) {
			sgmnt2entry[sgmnt_idx] =
				nvmeibc_jam_simu_alloc_entry(srv->disk, sgmnt2dlba[sgmnt_idx], committed_tx_id, false /*not dry run*/);
		}
	}
	free_io_traits(&io_traits);
} // RRRRR: Why all the functions above needed? Why not just asking JAM (dry run allocation?)

void nvmeibc_jam_simu_verify_cleand_all_servers(struct NVMeshSystem *sys) {
	int server, jentry;
	for (server = 0; server < N_MAX_RAID_SLICE_LEN; ++server) {
		for (jentry = 0; jentry < NUM_JENTS_JAM_USES_IN_JRI(sys->servers[server].disk);
			 ++jentry) { // Verify all remaining abandoned entries have been freed
			nvmeibc_jam_simu_verify_cleand(sys->servers[server].disk, jentry);
		}
	}
}

void nvmeibc_inject_jam_hash64(struct test_context env, u64 (*hash64)(u64, unsigned int)) {
	int i;
	#if 0
	if (hash64)
		unitest_trace_note(jam_hash_injected, "");
	else
		unitest_trace_note(jam_hash_restored, "");
	#endif
	for (i = 0; i < N_MAX_RAID_SLICE_LEN; ++i) {
		if (env.sys->servers[i].disk->jam_disk)
			nvmeibc_jam_simu_inject_hash_function(env.sys->servers[i].disk, hash64);
	}
}

// If txid == NVMEIBC_DP_EC_MD_TX_ID_MAX + 1 it means the caller just wanted to clear all entries so the the alloced entries won't be bounded to any journal emtry.
int *nvmeibc_drain_free_jrnls(struct test_context env, u32 max_free_list, u32 txid) {
	// In order to drain the free list, we will emulate @max_free_list full slice ios,
	// under controlled hash function.
	// The algorithm is greedy. We may actually allocate more entries then we need to
	// drain free list, and it is OK.

	// This array is used to store allocations info, to be able to release it later
	// Make sure it is big enough to hold max_free_list full slice ios info
	int *io2sgmnt2entry = sim_kmalloc(N_MAX_RAID_SLICE_LEN * max_free_list * sizeof(int), GFP_KERNEL);

	// Initial value is ILLEGAL_JRNL_ENTRY, meaning entry is not allocated
	range_fill(io2sgmnt2entry, io2sgmnt2entry + N_MAX_RAID_SLICE_LEN * max_free_list, ILLEGAL_JRNL_ENTRY);

	// We use __hash_j2d, which will return a monotonically growing hash for each dlba. This way it is
	// guaranteed that after @max_free_list ios to the same disk, we will drain the free list completelly,
	// at least once.
	nvmeibc_inject_jam_hash64(env, __hash_j2d);

	// Emulate multiple full slice IOs
	for (u32 io = 0; io < (max_free_list/nvmeibc_jentry_num_blocks); ++io) {
		// We don't really care about the dlba since we just want to draint all entries (the dlba affect the entry which will be allocated).
		const u32 dlba = (io+1) * LOCKSET_SLICES;

		// Now allocate journals
		for (u16 sgmnt_idx = 0; sgmnt_idx < env.sraid.cpr->replicas; ++sgmnt_idx) {
			struct disk_range *sgmnt = &env.sraid.cpr[sgmnt_idx];
			struct serverSimulator *srv = &env.sys->servers[sgmnt->node_id];
			BUG_ON(ILLEGAL_JRNL_ENTRY != io2sgmnt2entry[N_MAX_RAID_SLICE_LEN * io + sgmnt_idx]);
			io2sgmnt2entry[N_MAX_RAID_SLICE_LEN * io + sgmnt_idx] =
				nvmeibc_jam_simu_alloc_entry(srv->disk, dlba, txid, false /*not dry run*/);
		}
	}
	return io2sgmnt2entry;
}

void nvmeibc_release_drained_jrnls(struct test_context env, const int *io2sgmnt2entry, u32 max_free_list, enum nvmeibc_jam_jidx_event jam_event) {
	for (u32 io = 0; io < max_free_list; ++io) {
		nvmeibc_process_jrnl_entries_for_io(env, io2sgmnt2entry + N_MAX_RAID_SLICE_LEN * io, jam_event);
	}
	nvmeibc_inject_jam_hash64(env, NULL); // Reset hash function
	sim_kfree(io2sgmnt2entry);				  // Release resources
}

/**
 * This function ensures that ater it is done, no jentry is bound to anything
 */
void nvmeibc_ensure_jam_has_nothing_bound(struct test_context env) {
	int *io2sgmnt2entry = nvmeibc_drain_free_jrnls(env, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, 0); /*Allocate everything*/
	nvmeibc_release_drained_jrnls(env, io2sgmnt2entry, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, NVMEIBC_JIDX_EVT_ERASE);
	NVMeshSystem_serialize(env.sys);
}

void nvmeibc_backup_switch_sync_mode(bool sync) {
	const bool prev_mode = __backup_sync_push(ut_conf__platform_io_sync_get());
	if (prev_mode != sync) {
		unitest_trace_note(sync_mode_changed, "@BOOL", sync);
		ut_conf__platform_io_sync_set(sync);
	}
}

void nvmeibc_restore_sync_mode(void) {
	const bool prev_mode = __backup_sync_pop();
	if (prev_mode != ut_conf__platform_io_sync_get()) {
		unitest_trace_note(sync_mode_restored, "@BOOL", prev_mode);
		ut_conf__platform_io_sync_set(prev_mode);
	}
}

struct nvmeibc_roles_bmps nvmeibc_sim_init_roles_bmps(const struct disk_range *pr, enum NVMEIBTC_DS_MODE topo[N_MAX_RAID_SLICE_LEN], u32 slice_start_seg){
	u32 i;
	struct nvmeibc_roles_bmps bmps;
	bmps.data_sgmnts = nvmeibc_raid1_get_data_bmp(pr);
	bmps.pari_sgmnts = nvmeibc_raid1_get_parities_bmp(pr);

	bmps.raid.data = nvmeibc_raid1_get_data_bmp(pr);
	bmps.raid.pari = nvmeibc_raid1_get_parities_bmp(pr);
	bmps.raid.all = nvmeibc_raid1_get_all_bmp(pr);

	bmps.wm = bmps.wp = bmps.rw = bmps.w = bmps.dead = 0;
	for (i = 0; i < pr->replicas; i++) {
		nvmeibc_raid1_acm2bmp(topo[(i + slice_start_seg) % pr->replicas], i, &bmps);
	}
	bmps.readable = bmps.wp | bmps.rw;
	bmps.dbits_off_mask = bmps.w;
	bmps.dbits_on_mask = bmps.dead;
	nvmeibc_update_roles_bmps_flags(&bmps);
	return bmps;
}
