/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_scenario_ec.h"
#include "../nvmeibc_block_common.h"
#include "./uni_framework/bunitest_conf.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "../../datapath_ec/nvmeibc_block_dp_ec_gf.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeibc_jam.h"
#include "uni_framework/unitest_defs.h"
#include "uni_framework/range_algorithms.h"
#include "uni_framework/cond_wait_algorithms.h"
#include "uni_scenario_tx_history_ec.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_lock_server.h"
#include "nvmeibc_simu_disk.h"
#include "uni_scenario_tx_history_ec.h"
#include "uni_enumerators.h"
#include "uni_recoveries.h"
#include "uni_scenario_common.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recov_stats.h"
#include "nvmeib_jdr.h"

static bool all_permutations = false;

void __fill_block_device_with_unique_data(struct NVMeshSystem *sys, u8 v);

static void __clear_all_journals(struct NVMeshSystem *sys){
	//for the time being works under assumption that all jranges belongs to serjio
	const ulong EXACT_NUM_JENTRIES = (NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE / nvmeibc_jentry_num_blocks);
	for (u16 server_idx = 0; server_idx < sys->nServers; ++server_idx){
		struct serverSimulator *srv = &sys->servers[server_idx];
		const int jri = nvmeibs_find_jri_by_uuid(srv, nvmeibc_get_uuid(&sys->clients[0].p->core));
		if (0 <= jri){
			unsigned long entry_idx = 0;
			const unsigned long abnd_entries_bmp = nvmeibs_nordda_get_abandjour_in_jri(srv, jri);
			for_each_set_bit(entry_idx, &abnd_entries_bmp, EXACT_NUM_JENTRIES) {
				nvmeibs_nordda_jour_entry_do(srv, jri, entry_idx, "Free Entry");
				srv->jmdc[nvmeibs_jmdc_ind_of(jri, entry_idx, nvmeibc_jentry_num_blocks)].raw = nvmeib_jmd_unused_entry_val.raw;
			}
		}
		srv->disk->jrc.jour.rng_gen_id = NVMEIB_EC_INVALID_JOURNAL_GEN_ID;
		bitmap_zero(srv->disk->jrc.free_bitmap, EXACT_NUM_JENTRIES);
		for (u16 entry_idx = 0; entry_idx < ARRAY_SIZE(srv->disk->jrc.ent_md); ++entry_idx) {
			srv->disk->jrc.ent_md[entry_idx].ent_gen_id = 0;
		}
	}
}

//extern struct NVMeshSystem* g_sys;			// Todo: Ugly, remove
TEST_FUNC int unitest_raid_ec_transform(struct NVMeshSystem *sys, const char* action){
	int rv = 0, s, volInd = 0;
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client
	struct volumeDescriptor   *vol 		 = &sys->mdb.vols[volInd];  // Move segment of third volume
	struct disk_range *seg;
	static struct volumeDescriptor backup_vol = {.nChunks = 0};
	u64 expected_size = 0, cur_chunk_start_addr = 0;
	int cur_chunk_index = 0;
	int raid_type = -1, n_parities = 0; 		// Supports: 5 or 6
	_ND(trace_uni_scenario_ec_unitest_raid_ec_transform, "transforming @VOL_I to action=@ACTION_STR", volInd, action);
	if (action[0] == 'r') {
		raid_type = 6;
		n_parities = 2;
		goto _to_raid6_8plus2;
	}

	if (action[0] == 't') {
		raid_type = action[8]-'0';
		n_parities = ((raid_type==5) ? 1 : 2);
		goto _to_raid50_or_60;
	}
	if (action[0] == 'b')
		goto _back_from_raid50_or_60;
	BUG();


_to_raid6_8plus2:	// ------------------------------------ Convert Vol0 to raid 6: 1 chunk 6+2.
	BUG_ON((raid_type!=6));
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	backup_vol = *vol;
	vol->segs = (struct disk_range*)sim_kmalloc(sizeof(*vol->segs)*NVMESH_N_PHYS_DISKS, GFP_KERNEL);
	vol->nSegments = 10;
	vol->nChunks = 1;
	for (s=0, seg = vol->segs; s<vol->nSegments; s++, seg++){
		const int slice_size = vol->nSegments - n_parities;	// Raid5(D+1), Raid6 (D+2);
		disk_range_copy(seg, &backup_vol.segs[s]);
		seg->replicas     = vol->nSegments;
		seg->stripe_width = 1;
		if (seg->stripe_index == 0) {		// New chunk started
			cur_chunk_index = 0;
			cur_chunk_start_addr = 0;
			expected_size += slice_size*seg->length * seg->stripe_width;	// Current chunk size
		}
		seg->stripe_size = slice_size*seg->stripe_size;
		seg->slice_size  = slice_size;
		seg->bd_start    = cur_chunk_start_addr;
		seg->chunk_index = cur_chunk_index;
		seg->volume_index = volInd;
	}
	vol->locks_scheme.maxNOwners   = 1 + n_parities;
	BUG_ON(vol->locks_scheme.type != OWNER_SCHEME_SL_START_DEC_C);

	tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
	mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);
	NVMeshSystem_notify_new_disk_sgmnts(sys);
	send_command_to_vol_attach_or_update(sys, -1, volInd);
	NVMeshSystem_precondition_all_disks_for_EC(sys);
	NVMeshSystem_di_tracking_reconf(sys, true /* for_ec*/);
	goto _out;

_to_raid50_or_60:	// ------------------------------------ Convert Vol0 to raid 50 (first chunk:3+1, Second chunk:2+1 raid 5), stripe width 3
					// ------------------------------------           or to raid 60 (first chunk:2+2, Second chunk:1+2 raid 6), stripe width 3
	BUG_ON((raid_type!=5)&&(raid_type!=6));
	// Ugly temp solution until new configuration format will support EC
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	for (s=0, seg = vol->segs; s<vol->nSegments; s++, seg++){
		const int mir = vol->segs[s].replicas;
		int slice_size = mir-n_parities;	// Raid5(D+1), Raid6 (D+2);
		if (seg->stripe_index == 0) {		// New chunk started
			if (s > 0)
				cur_chunk_index++;
			cur_chunk_start_addr = expected_size;
			expected_size += slice_size*seg->length * seg->stripe_width;	// Current chunk size
		}
		seg->stripe_size = slice_size*seg->stripe_size;
		seg->slice_size = slice_size;
		seg->bd_start = cur_chunk_start_addr;
		seg->chunk_index = cur_chunk_index;
		seg->volume_index = volInd;
	}
	vol->locks_scheme.maxNOwners = 1 + n_parities;
	BUG_ON(vol->locks_scheme.type != OWNER_SCHEME_SL_START_DEC_C);
	tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
	mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);
	NVMeshSystem_notify_new_disk_sgmnts(sys);
	send_command_to_vol_attach_or_update(sys, -1, volInd);
	NVMeshSystem_precondition_all_disks_for_EC(sys);
	NVMeshSystem_di_tracking_reconf(sys, true /* for_ec*/);
	goto _out;

_back_from_raid50_or_60:
	send_command_to_all(sys, -1, volCmds_Detach);
	NVMeshSystem_serialize(sys);
	__clear_all_journals(sys);
	NVMeshSystem_wipe_all_md_of_disks(sys);
	NVMeshSystem_precondition_all_disks_for_R1(sys);
	if (backup_vol.nChunks != 0) {
		sim_kfree(vol->segs);
		*vol = backup_vol;
		backup_vol.nChunks = 0;
		tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
		mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);
	}
	for (s = 0, seg = vol->segs; s < vol->nSegments; s++, seg++) {
		if (seg->stripe_index == 0) {		// New chunk started
			if (s > 0)
				cur_chunk_index++;
			cur_chunk_start_addr = expected_size;
			expected_size += seg->length * seg->stripe_width;	// Current chunk size
		}
		seg->stripe_size /= seg->slice_size;
		seg->bd_start = cur_chunk_start_addr;
		seg->slice_size = 1;
		seg->chunk_index = cur_chunk_index;
		seg->volume_index = volInd;
	}
	vol->locks_scheme.maxNOwners = N_MAX_RAID_LOCKS;			// Back to default N-mirror
	BUG_ON(vol->locks_scheme.type != OWNER_SCHEME_SL_START_DEC_C);
	NVMeshSystem_notify_new_disk_sgmnts(sys);
	send_command_to_all(sys, -1, volCmds_New);
	NVMeshSystem_precondition_all_disks_for_R1(sys);
	NVMeshSystem_di_tracking_reconf(sys, false /* (not) for_ec*/);
	goto _out;

_out:
	BUG_ON(client->devs[volInd]->size != expected_size);
	BUG_ON(rv);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_all_clients_ec_edic(sys, true);
	NVMeshSystem_di_tracking_reset(sys);
	_ND(trace_1_uni_scenario_ec_unitest_raid_ec_transform, "transforming @VOL_I to action=@ACTION_STR - done(@RV)", volInd, action, rv);
	return rv;
}

// No longer used to allow multi-slice EC IO
void __attribute__ ((unused)) set_split_bio_to_slices(struct nvmeibc_block_device *dev, bool enable) {
	static unsigned alignment_sectors = (~0);
	static bool jam_non_ambiguous = false;
	extern bool JAM_NON_AMBIGUOUS;
	if (enable) {
		dev->dp.alignment_sectors.write = alignment_sectors;
		JAM_NON_AMBIGUOUS = jam_non_ambiguous;
	} else {
		alignment_sectors = dev->dp.alignment_sectors.write;
		dev->dp.alignment_sectors.write = 0;

		jam_non_ambiguous = JAM_NON_AMBIGUOUS;
		JAM_NON_AMBIGUOUS = false;
	}
}

// Used to control IO split when slice size changes (new chunk)
static unsigned set_split_bio_to_new_value(struct nvmeibc_block_device *dev, int new_slice_size) {
	const u32 previous_value = dev->dp.alignment_sectors.write >> KERNEL_SECTOR_TO_SECTOR_SHIFT;
	const u32 ms_value = nvmeibc_jentry_num_blocks;
	dev->dp.alignment_sectors.write = ((new_slice_size * ms_value)		 << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	dev->dp.alignment_sectors.read =  ((new_slice_size * LOCKSET_SLICES) << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	if (ms_value != dev->dp.p.binje) {
		BUG_ON(dev->dp.p.binje != LOCKSET_SLICES);
		dev->dp.p.binje = nvmeibc_jentry_num_blocks;
	}
	return previous_value / ms_value;
}

static unsigned set_split_bio_to_blockset(struct nvmeibc_block_device *dev) {
	const u32 previous_value = dev->dp.alignment_sectors.write >> KERNEL_SECTOR_TO_SECTOR_SHIFT;
	const unsigned ms_value = dev->dp.p.binje;
	dev->dp.alignment_sectors.write = ((dev->dp.p.slice_size * LOCKSET_SLICES) << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	BUG_ON(dev->dp.p.binje != nvmeibc_jentry_num_blocks);
	dev->dp.p.binje = LOCKSET_SLICES;
	BUG_ON(dev->dp.alignment_sectors.read != dev->dp.alignment_sectors.write);
	return previous_value / ms_value;
}

// This function will cause a "never written to" parity block to become valid
// Moreover we need to remove all F values that are not valid in MD
static inline void __set_valid_dbits(union nvmeibc_block_dp_ec_data_block_md *md, int db1, int db2)
{
	// Sanity
	BUG_ON(db2 > db1);
	BUG_ON(db1 > N_MAX_RAID_SLICE_LEN);		// Traps for un/initialized MD values
	BUG_ON(db2 > N_MAX_RAID_SLICE_LEN);

	if (nbdpec_md_was_data_never_written(md)) { // Make parity valid for DBits
		nbdpec_md_mark_data_never_written_no_dbits(md, true);
	}
	if (db1 > -1) {								// Set DB1
		md->P.dbits_0 = db1;
	} else if (md->P.dbits_0 == 0xf) {			// Unset unknown from MD (not allowed)
		md->P.dbits_0 = 0;
	}
	if (db2 > -1) {								// Set DB2
		md->P.dbits_1 = db2;
	} else if (md->P.dbits_1 == 0xf) {			// Unset unknown from MD (not allowed)
		md->P.dbits_1 = 0;
	}
}

// This function will cause a "never written to" block to become valid, and set TxID
static inline void __set_valid_tx_id(union nvmeibc_block_dp_ec_data_block_md *md,
									 u32 txid)
{
	if (nbdpec_md_was_data_never_written(md)) { // Make MD valid
		nbdpec_md_mark_valid_version(md);		// Set correct version
		md->jri = -1;							// Set valid JRI
	}
	// Sanity, must have already marked correct version
	BUG_ON(md->D.version == 0x3);
	md->tx_id = txid;							// Set TxID
}

// Verify the metadata on the disks
static void __verify_md_on_disk_after_txid_wraparound(struct NVMeshSystem *sys, const struct disk_range *pr, int io_slice, const u32 expected_TXID, u32 deg_bit_map) {
	union nvmeibc_block_dp_ec_data_block_md *md;
	for (u32 s = 0; s < pr->replicas; s++) {							// Verify the metadata on the disks
		const bool is_parity_seg = (s >= pr->slice_size);						// Correct only for first blockset!!
		struct ramDiskSimulator *ram = &sys->servers[s].ramDisk;
		if (ram->state != ramDisk_running) // Will not have updated all MDs
			continue;
		for (int b = 0; b < LOCKSET_4KS; b++) {                         // In rest of the blocks in blockset -> metadata should be trimmed
			md = ramDiskSimulator_get_metadataptr(ram, pr[s].dlba_start + b);
			if ((is_parity_seg)&&(deg_bit_map))
				BUG_ON(md->P.dbits_0 == 0);								// Verify TxID wraparound turned on dbits in all slices!
			if (b == io_slice) {
				BUG_ON(md->jri   == JRI_MARK_NO_JOURNAL);				// This slice (block on each segment) was written by IO
				BUG_ON(md->tx_id != expected_TXID);
			} else if (!nbdpec_md_was_data_never_written(md)) {
				BUG_ON((!nbdpec_md_is_no_journal(md)) && (md->tx_id != NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS));	// This slice still has wraparound metadata, marking no journal
			}
		}
	}
}

void nvmeibc_raid_verify_tx_id_replica_consistency(struct nvmeibc_raid1 *raid);

/* Test TxID wrap-around fixup & dirty suspect resolve */
static void __test_io_maintanace_syncs(struct test_context env, u8* mem, bool are_writes_disabled) {
	int volInd = 0, rv, s, i;
	struct NVMeshSystem *sys = env.sys;
	struct clientSimulator *client = env.client;				// Current client
	struct nvmeibc_topology *t = nvmeibc_topology_get(&env.dev->topologies);// Put is below (need degraded bit map)
	struct nvmeibc_raid1 *raid = &t->chunks[0].raid1s[0];				// Used for deg map
	struct tTopoOfPraid	*r1 = env.sraid.tpr;
	const u32 deg_bit_map = nvmeibc_raid1_get_inverse_sgmnts_bmp(raid, rw);
	u64 *cur_num_syncs = &env.dev->dp.sync_rsrcs.stats.num_total_maintainance;	// Monitor amount of syncs
	const struct disk_range *seg = env.sraid.cpr;
	const int slice_size = seg->slice_size;
	const int lenBlocks = slice_size;									// Always write to a full slice
	const int n_parities = (seg->replicas - slice_size);
	const u32 expected_TXID = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS + 1;
	const union nvmeibc_dbits_entry expected_dbit = {.bsmod = {.mod_marker = 0, .dead0 = 3, {.dead1 = 1}}};
	const union nvmeibc_dbits_entry lower_dbit    = {.bsmod = {.mod_marker = 0, .dead0 = 2, {.dead1 = 1}}};
	const int rdbs[2][2] = {{23, 27}, {7, 13}};							// Inject dirytbits values into pseudo random 2 slices in metadata on parity seg
	const int rios[2]    = {0, 25};										// psudo random slice that io wants to read when encountering maintenance problem
	const int os = (r1->s[0].access_mode == NVMEIBTC_DS_MODE_RW) ? 0 : (slice_size + n_parities - 1 ); // Owner index, last parity
	const int ps = (r1->s[slice_size].access_mode == NVMEIBTC_DS_MODE_RW) ? slice_size : ((n_parities > 1) ? slice_size + 1 : os); // Parity index
	const int qs = (n_parities > 1) ? slice_size + 1 : ps;
	const bool _os_is_not_eq_parity = (os != ps);
	struct io_traits io_traits = get_io_traits(env, 0, lenBlocks);
	struct ramDiskSimulator *srvOwlock = &(sys->servers[os].ramDisk);
	struct ramDiskSimulator *srvPlock = &(sys->servers[ps].ramDisk);
	struct block_ram_inject_ptrs ram_inj =    serverSimulator_get_block_inject_ptrs(&sys->servers[os], io_traits.slices[0].sgmnt2dlba[os], 0, 0, 0).ram;
	struct block_ram_inject_ptrs ram_inj_ps = serverSimulator_get_block_inject_ptrs(&sys->servers[ps], io_traits.slices[0].sgmnt2dlba[ps], 0, 0, 0).ram;
	struct block_ram_inject_ptrs ram_inj_qs = serverSimulator_get_block_inject_ptrs(&sys->servers[qs], io_traits.slices[0].sgmnt2dlba[qs], 0, 0, 0).ram;
	struct nvmeibc_maintain_sync_stats *maint_stats = &nvmeibc_flow_counters_ref()->main;
	const int num_deg = nvmeibc_praid_get_num_deg_segs(raid);
	BUG_ON((os >= raid->replicas) || (ps >= raid->replicas) || (qs >= raid->replicas));
	nvmeibc_topology_put(t);											// Release t

	//if (deg_bit_map > 1) We already wrote the minimal dirty bits to the md (1,2...) so from now on we will get these as result
	if (r1->s[0].access_mode == NVMEIBTC_DS_MODE_DEAD) { // Cannot be the owner
		if (r1->s[os].access_mode == NVMEIBTC_DS_MODE_DEAD) {// P will never be a single owner until we allow double degraded where D0+Q are degraded
			BUG_ON(are_writes_disabled == false);
			goto _after_writes_test;
		}
	}

	if (r1->s[0].access_mode == NVMEIBTC_DS_MODE_DEAD && r1->s[os].access_mode != NVMEIBTC_DS_MODE_DEAD && r1->s[ps].access_mode != NVMEIBTC_DS_MODE_DEAD && n_parities >= 1) {
		struct toma_recovery_args rcvr_args = { .type = NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA };
		const int startBlock = 0;
		struct block_ram_inject_ptrs ram_inj_os = serverSimulator_get_block_inject_ptrs(&sys->servers[os], io_traits.slices[0].sgmnt2dlba[os], 0, 0, 0).ram;
		NVMeshSystem_wipe_all_dirty_bits(sys);                       // Coz IO in last possible degraded topology left dbits
		atomic_set(&srvOwlock->io_cnt, 0);
		atomic_set(&srvPlock->io_cnt, 0);
		ram_inj_os.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
		*ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
		ram_inj_ps.dbits->slmod.dead0 = 1;
		ram_inj_ps.dbits->slmod.fst_slc = 1;
		ram_inj_ps.dbits->slmod.len_slc = 1;
		ram_inj_ps.dbits->bsmod.dead1 = 1;
		*ram_inj_ps.txid = 100;

		rv = osSimulator_readArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);
		if (_os_is_not_eq_parity) BUG_ON(atomic_read(&srvOwlock->io_cnt) != 0);
		BUG_ON(atomic_read(&srvPlock->io_cnt) != 1);  // 1 cause of read


		NVMeshSystem_set_all_txid_to_1(sys);
		atomic_set(&srvOwlock->io_cnt, 0);
		atomic_set(&srvPlock->io_cnt, 0);
		ram_inj_os.dbits->all_bits = 0;
		*ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
		*ram_inj_ps.txid = 100;
		if (ps != qs) {
			ram_inj_ps.dbits->slmod.dead0 = 1;
			ram_inj_ps.dbits->slmod.fst_slc = 1;
			ram_inj_ps.dbits->slmod.len_slc = 1;
			*ram_inj_qs.txid = INITIAL_LAZY_READ_TXID;
			ram_inj_qs.dbits->all_bits = 0;
		} else {
			ram_inj_ps.dbits->all_bits = 0;
		}

		BUG_ON(tomaSimulator_recoverThing(env.sraid.tpr, env.sraid.cpr + os, rcvr_args) < 0);
		BUG_ON(atomic_read(&srvOwlock->io_cnt) != 0);
		BUG_ON(atomic_read(&srvPlock->io_cnt) != 0);


		atomic_set(&srvOwlock->io_cnt, 0);
		atomic_set(&srvPlock->io_cnt, 0);
		ram_inj_ps.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
		if (ps != qs) {
			ram_inj_qs.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			*ram_inj_qs.txid = INITIAL_LAZY_READ_TXID;
		}
		*ram_inj_ps.txid = INITIAL_LAZY_READ_TXID;
		ram_inj_os.dbits->slmod.dead0 = 1;
		ram_inj_os.dbits->slmod.fst_slc = 1;
		ram_inj_os.dbits->slmod.len_slc = 1;
		*ram_inj_os.txid = 100;
		rv = osSimulator_readArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);
		if (_os_is_not_eq_parity) BUG_ON(atomic_read(&srvOwlock->io_cnt) != 0);
		BUG_ON(atomic_read(&srvPlock->io_cnt) != 1);  // 1 cause of read


		atomic_set(&srvOwlock->io_cnt, 0);
		atomic_set(&srvPlock->io_cnt, 0);
		ram_inj_os.dbits->all_bits = 0;
		*ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
		if (ps != qs) {
			ram_inj_qs.dbits->slmod.dead0 = 1;
			ram_inj_qs.dbits->slmod.fst_slc = 1;
			ram_inj_qs.dbits->slmod.len_slc = 1;
			*ram_inj_ps.txid = INITIAL_LAZY_READ_TXID;
			*ram_inj_qs.txid = 100;
			ram_inj_ps.dbits->all_bits = 0;
		} else {
			*ram_inj_ps.txid = 100;
			ram_inj_ps.dbits->all_bits = 0;
		}

		BUG_ON(tomaSimulator_recoverThing(env.sraid.tpr, env.sraid.cpr + os, rcvr_args) < 0);
		BUG_ON(atomic_read(&srvOwlock->io_cnt) != 0);
		BUG_ON(atomic_read(&srvPlock->io_cnt) != 0);
		atomic_set(&srvOwlock->io_cnt, 0);
		atomic_set(&srvPlock->io_cnt, 0);

		if (1) { // Dbits rebuild resolves unknown tx-dbits
			int recov_status = 0;
			ram_inj_ps.dbits->all_bits = ram_inj_os.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			*ram_inj_ps.txid =           *ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
			BUG_ON(tomaSimulator_recoverThingStatus(env.sraid.tpr, env.sraid.cpr + os, rcvr_args, &recov_status) < 0);
			if (are_writes_disabled) {				// with dead seg amount == parities, ilegal to call dirty bits rebuild.
				BUG_ON(recov_status == 0);
				BUG_ON(*ram_inj_os.txid != INITIAL_LAZY_READ_TXID);
				*ram_inj_ps.txid =           *ram_inj_os.txid = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS;
			} else {
				BUG_ON(atomic_dec_return(&srvOwlock->io_cnt) != 0);  // 1 - Each disks gets IO to read md to resolve unknown
				if (_os_is_not_eq_parity)
					BUG_ON(atomic_dec_return(&srvPlock->io_cnt) != 0);  // 1 - Each disks gets IO to read md to resolve unknown
				BUG_ON(*ram_inj_os.txid == INITIAL_LAZY_READ_TXID);
			}
		}
		if (1) { // Stale-locks rebuild does not resolves unknown tx-dbits
			int recov_status = 0;
			enum NVMEIBT_RECOVERY_TYPE prev_type = rcvr_args.type;
			rcvr_args.type = NVMEIBT_RECOVERY_TYPE_STALE_REBUILD;
			ram_inj_ps.dbits->all_bits = ram_inj_os.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			*ram_inj_ps.txid =           *ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
			BUG_ON(tomaSimulator_recoverThingStatus(env.sraid.tpr, env.sraid.cpr + os, rcvr_args, &recov_status) < 0);
			BUG_ON(recov_status != 0); // Recovery did launch
			BUG_ON((atomic_read(&srvOwlock->io_cnt) + atomic_read(&srvPlock->io_cnt)) != 0);  // 0 - no resolve so no read from disk
			BUG_ON(*ram_inj_os.txid != INITIAL_LAZY_READ_TXID);
			rcvr_args.type = prev_type;
		}
		if (1) { // Dedicated recovery resolves unknown tx-dbits
			u16 unk_dbits = (num_deg > 1) ? nvmeib_dbits_entry_build_unk(-1,-1).all_bits : nvmeib_dbits_entry_single_unk().all_bits;
			int recov_status = 0;
			enum NVMEIBT_RECOVERY_TYPE prev_type = rcvr_args.type;
			rcvr_args.type = NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO;
			ram_inj_ps.dbits->all_bits = ram_inj_os.dbits->all_bits = unk_dbits;
			*ram_inj_ps.txid =           *ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
			BUG_ON(tomaSimulator_recoverThingStatus(env.sraid.tpr, env.sraid.cpr + os, rcvr_args, &recov_status) < 0);
			BUG_ON(recov_status != 0); // Recovery did launch
			BUG_ON(atomic_dec_return(&srvOwlock->io_cnt) != 0);  // 1 - Each disks gets IO to read md to resolve unknown
			if (_os_is_not_eq_parity)
				BUG_ON(atomic_dec_return(&srvPlock->io_cnt) != 0);  // 1 - Each disks gets IO to read md to resolve unknown
			rcvr_args.type = prev_type;
			BUG_ON(*ram_inj_os.txid == INITIAL_LAZY_READ_TXID);
		}
		if (!are_writes_disabled) { // IO (Write) resolves unknown tx-dbits
			ram_inj_ps.dbits->all_bits = ram_inj_os.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			*ram_inj_ps.txid =           *ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
			rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);  // starting the write from lba=1 inorder to not turnon dbits.
			BUG_ON(atomic_read(&srvOwlock->io_cnt) != 3); // resolve unknown + journal write + write
			BUG_ON(atomic_read(&srvPlock->io_cnt) != 3);  // resolve unknown + journal write + write
			BUG_ON(*ram_inj_os.txid == INITIAL_LAZY_READ_TXID);
		} // Writes are disabled

		// clean
		ram_inj_ps.dbits->all_bits = ram_inj_os.dbits->all_bits = 0;
		*ram_inj_ps.txid = *ram_inj_os.txid = INITIAL_LAZY_READ_TXID;
		{
			struct block_ram_inject_ptrs dead_seg_ram_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[0], io_traits.slices[0].sgmnt2dlba[0], 0, 0, 0).ram;
			*dead_seg_ram_inj.txid = INITIAL_LAZY_READ_TXID;
		}
	}

	if (are_writes_disabled)
		goto _after_writes_test;
	for (i = 0; i < 2; i++) { // Three staged test
		const bool are_all_rw = (deg_bit_map == 0);	// Note: We can also use toma topology without accessing internal structures
		// Setup parameters for test with all injection pointers ready
		const int startBlock = rios[i]*lenBlocks;
		struct io_traits db_inj_traits =           get_io_traits(env, rdbs[i][0]*lenBlocks, lenBlocks);
		struct io_traits db_inj_2nd_slice_traits = get_io_traits(env, rdbs[i][1]*lenBlocks, lenBlocks);
		struct block_inject_ptrs db_inj =   serverSimulator_get_block_inject_ptrs(&sys->servers[ps], db_inj_traits.slices[0].sgmnt2dlba[ps],           0, 0, 0);
		struct block_inject_ptrs db_2_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[ps], db_inj_2nd_slice_traits.slices[0].sgmnt2dlba[ps], 0, 0, 0);
		// ----------------------- Inject TxID wraparound + dbits suspect at once
		*ram_inj.txid = NVMEIBC_DP_EC_MD_TX_ID_MAX - 1;         // make a write tx set txid max on blockset.
		rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);		// Full slice IO, writing 1 block on each segment
		clientSimulator_wait_for_all_sync_ops(client);
		*cur_num_syncs = 0;
		BUG_ON(*ram_inj.txid != NVMEIBC_DP_EC_MD_TX_ID_MAX);
		atomic_set(&maint_stats->n_txid_wrap, 0);
		ram_inj.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; // Set DB suspect in lock
		ram_inj_ps.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; // Set DB suspect in lock
		ram_inj_qs.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; // Set DB suspect in lock
		__set_valid_dbits(db_inj.dmd,   expected_dbit.bsmod.dead1, -1);   // Inject dirytbits values into pseudo random 2 slices in metadata on parity seg
		__set_valid_dbits(db_2_inj.dmd, expected_dbit.bsmod.dead0, -1);
		__unitest_fill_blocks_unique_pattern(&mem[0], lenBlocks);
		nvmeibc_debug_ram_binfo = warn_on_too_many_degraded = false;								  // Injected 'expected_dbit' dbit for 2 segments in potentially single degraded mode
		rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);		// Full slice IO, writing 1 block on each segment
		nvmeibc_debug_ram_binfo = warn_on_too_many_degraded = true;
		clientSimulator_wait_for_all_sync_ops(client);
		BUG_ON(*cur_num_syncs != (are_all_rw ? 0 : 1));	// Exactly 1 maintanance operation occured (dirty suspect), in all_rw, IO resolved them automatically
		BUG_ON(atomic_read(&maint_stats->n_txid_wrap) != 1);	// Exactly 1 txid wraparound operation occured (txid wraparound)
		// Verify client cleaned-up the stuff the ram on servers
		BUG_ON(*ram_inj.txid != expected_TXID);
		BUG_ON(*db_inj.ram.txid != expected_TXID);
		if (are_all_rw) {
			BUG_ON(   ram_inj.dbits->all_bits != zero_dbits.all_bits);
			BUG_ON(db_inj.ram.dbits->all_bits != zero_dbits.all_bits);
		} else if (deg_bit_map == 2) {
			BUG_ON(   ram_inj.dbits->all_bits != lower_dbit.all_bits);
			BUG_ON(db_inj.ram.dbits->all_bits != lower_dbit.all_bits);
		} else 				  {
			BUG_ON(   ram_inj.dbits->all_bits != expected_dbit.all_bits);
			BUG_ON(db_inj.ram.dbits->all_bits != expected_dbit.all_bits);
		}
		ram_inj.dbits->all_bits = 0;
		db_inj.ram.dbits->all_bits = 0;
		if (n_parities > 1) {
			for (s = 0; s < n_parities; s++) {
				struct ramDiskSimulator *srvP_lock  = &(sys->servers[slice_size + s].ramDisk);
				if (srvP_lock->state == ramDisk_running && srvOwlock != srvP_lock && srvPlock != srvP_lock) { // We might have already checked the owner which was P or Q or they might be down (When D0 is down Q is owner and P will be srvP)
					struct block_inject_ptrs verify_ram = serverSimulator_get_block_inject_ptrs(&sys->servers[slice_size + s], io_traits.slices[0].sgmnt2dlba[slice_size + s], 0, 0, 0);
					BUG_ON(*verify_ram.ram.txid != expected_TXID);
					if (are_all_rw)            BUG_ON(verify_ram.ram.dbits->all_bits != zero_dbits.all_bits);
					else if (deg_bit_map == 2) BUG_ON(verify_ram.ram.dbits->all_bits != lower_dbit.all_bits);
					else 				       BUG_ON(verify_ram.ram.dbits->all_bits != expected_dbit.all_bits);
					verify_ram.ram.dbits->all_bits = 0;
				}
			}
		}
		__verify_md_on_disk_after_txid_wraparound(sys, seg, rios[i], expected_TXID, deg_bit_map);
		// ----------------------- Inject TxID unresolved + wraparound
		*ram_inj.txid = NVMEIBC_DP_EC_MD_TX_ID_MAX - 1;         // make a write tx set txid max on blockset.
		rv = osSimulator_writeArrWait(&client->OS, volInd, rios[i]*lenBlocks, lenBlocks, mem);	BUG_ON(rv);		// Full slice IO, writing 1 block on each segment
		BUG_ON(rv);
		*cur_num_syncs = 0;
		atomic_set(&maint_stats->n_txid_wrap, 0);
		for (s = 0; s < (int)seg->replicas; s++) {
			struct ramDiskSimulator *ram = &sys->servers[seg[s].node_id].ramDisk;
			memset(ram->TxIDs, INITIAL_LAZY_READ_TXID, sizeof(ram->TxIDs)); // Screw-up the RAM of owner lock, // ramDiskSimulator_reset_txid();
		}
		__unitest_fill_blocks_unique_pattern(&mem[0], lenBlocks);
		rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);		// Full slice IO, writing 1 block on each segment
		clientSimulator_wait_for_all_sync_ops(client);
		BUG_ON(*cur_num_syncs != 1);	// Exactly 1 maintanance operation occured (dirty suspect)
		BUG_ON(atomic_read(&maint_stats->n_txid_wrap) != 1);	// Exactly 1 txid wraparound operation occured (txid wraparound)
		BUG_ON(*ram_inj.txid != expected_TXID);
		BUG_ON(*db_inj.ram.txid != expected_TXID);
		__verify_md_on_disk_after_txid_wraparound(sys, seg, rios[i], expected_TXID, deg_bit_map);
		// ----------------------- Inject TxID unresolved + TxID wraparound + dbits suspect at once
		if (are_all_rw) {		 // Daniel: For fast execution, test once in good topology without degraded modes
			*ram_inj.txid = NVMEIBC_DP_EC_MD_TX_ID_MAX - 1;         // make a write tx set txid max on blockset.
			rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);		// Full slice IO, writing 1 block on each segment
			*cur_num_syncs = 0;
			atomic_set(&maint_stats->n_txid_wrap, 0);
			*ram_inj.txid = INITIAL_LAZY_READ_TXID;
			ram_inj.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			ram_inj_ps.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; // Set DB suspect in lock
			ram_inj_qs.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; // Set DB suspect in lock
			__unitest_fill_blocks_unique_pattern(&mem[0], lenBlocks);
			rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);		// Full slice IO, writing 1 block on each segment
			clientSimulator_wait_for_all_sync_ops(client);
			// txid max is copied from a copy (need to inject all copies for TxID unresolved to occur)
			BUG_ON(*cur_num_syncs != (are_all_rw ? 0 : 1));			// Exactly 1 maintanance operation occured dirty suspect. In perfect topo io resolves them to 0
			BUG_ON(atomic_read(&maint_stats->n_txid_wrap) != 1);	// Exactly 1 txid wraparound operation occured (txid wraparound)
			BUG_ON(*ram_inj.txid != expected_TXID);
			BUG_ON(*db_inj.ram.txid != expected_TXID);
			__verify_md_on_disk_after_txid_wraparound(sys, seg, rios[i], expected_TXID, deg_bit_map);
			ram_inj.dbits->all_bits = 0;
			db_inj.ram.dbits->all_bits = 0;
			if (n_parities > 1) {
				for (s = 0; s < n_parities; s++) {
					struct ramDiskSimulator *srvP_lock  = &(sys->servers[slice_size + s].ramDisk);
					if (srvP_lock->state == ramDisk_running && srvOwlock != srvP_lock && srvPlock != srvP_lock) { // We might have already checked the owner which was P or Q or they might be down (When D0 is down Q is owner and P will be srvP)
						struct block_inject_ptrs verify_ram = serverSimulator_get_block_inject_ptrs(&sys->servers[slice_size + s], io_traits.slices[0].sgmnt2dlba[slice_size + s], 0, 0, 0);
						BUG_ON(*verify_ram.ram.txid != expected_TXID);
						if (are_all_rw)            BUG_ON(verify_ram.ram.dbits->all_bits != zero_dbits.all_bits);
						else if (deg_bit_map == 2) BUG_ON(verify_ram.ram.dbits->all_bits != lower_dbit.all_bits);
						else 				       BUG_ON(verify_ram.ram.dbits->all_bits != expected_dbit.all_bits);
						verify_ram.ram.dbits->all_bits = 0;
					}
				}
			}
		}
		// clean after round
		__set_valid_dbits(db_inj.dmd,   0, 0);
		__set_valid_dbits(db_2_inj.dmd, 0, 0);
		if (slice_size == 8 && ps != slice_size+1) {
			struct serverSimulator *srvQlock = &(sys->servers[slice_size+1]);
			db_inj = serverSimulator_get_block_inject_ptrs(srvQlock, io_traits.slices[0].sgmnt2dlba[slice_size+1], 0, 0, 0);
			db_inj.ram.dbits->all_bits = 0;
		}
		free_io_traits(&db_inj_traits);
		free_io_traits(&db_inj_2nd_slice_traits);
	}
 _after_writes_test:
	free_io_traits(&io_traits);
	nvmeibc_raid_verify_tx_id_replica_consistency(raid);
}

static void __verify_ec_2block_read(struct clientSimulator *client, int volInd, u8 *mem, int startBlock, int lenBlocks, u64 *magics) { 		// Verify data correctly read
	int rv;
	memset(mem, 0, lenBlocks*NVMEIBC_SECTOR_SIZE);                          // Clear the array
	rv = osSimulator_readArrWait( &client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);
	BUG_ON(*((u64*)&mem[0                  ]) != magics[0]);		// For simplicity test only the first u64
	BUG_ON(*((u64*)&mem[NVMEIBC_SECTOR_SIZE]) != magics[1]);
}
extern void verify_nvmeibc_dlba_to_vlba_translation(struct dp_io_topo_iterator *topo_it, const struct nvmeibc_datapath *dp);

TEST_FUNC int unitest_GoodPathIO_raid50_or_60(bunitest_s *B) {
	struct test_context env = { .sys = B->sys
								, .client = B->sys->clients
								, .dev = B->sys->clients->devs[0]
								, .sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, (struct volume_segment_index){0,0,0,0})};
	struct NVMeshSystem *sys = env.sys;

	int rv = 0, i, k, m, p, l, volInd = 0, max_lenBlocks = clientSimulator_sizeof_bdev(env.client, env.sraid.vsi.volume);
	const int memSize	 = max_lenBlocks*NVMEIBC_SECTOR_SIZE;	// Total array in bytes
	int lenBlocks;
	const struct tTopoOfVolume *vol = &sys->tcf.vols[volInd];
	const struct disk_range* seg = env.sraid.cpr;
	const int segs_in_first_chunk = seg->stripe_width*seg->replicas;
	u64 *cur_num_syncs = &env.dev->dp.sync_rsrcs.stats.num_total, prev_num_syncs = *cur_num_syncs;	// Monitor amount of syncs
	u64 magics[2];										// unique 64b signaturre filling the array
	u64 parity_P[2][2];
	u8        *mem = NULL;
	struct io_traits io_traits;
	mem = sim_kmalloc(memSize, GFP_KERNEL);					// Aligned Array to read/write to disk

	if (0) {		// IOctls to translate addresses, used to debug and build tests
		char cmd[128];
		sprintf(cmd, "#%s|translate_addr=%d\n", env.client->devs[volInd]->name, 17); clientSimulator_send_to_cli(env.client, cmd);
		sprintf(cmd, "#%s|translate_addr=%d\n", env.client->devs[1]->name,7); clientSimulator_send_to_cli(env.client, cmd);
		sprintf(cmd, "#%s|translate_addr=%d\n", env.client->devs[3]->name,13); clientSimulator_send_to_cli(env.client, cmd);
	}
	if (1) {
		NVMeshSystem_all_clients_dbg_di(env.sys, true);
	}
	lenBlocks  = _addr4k(0,2);
	if (1) {		// Test simple 2 blocks IO on the first chunk: Raid5(3+1) or Raid6(2+2)
		const int stripe_width = seg->stripe_width, slice_size = seg->slice_size;
		const int n_parities = seg->replicas - slice_size;			// 1 - Raid5, 2 -Raid6
		const int off_in_lock[3] = { 6             , 11            , 31           };	// Different io offsets - Should we randomize?
		const int owner_change_offset = ((stripe_width * slice_size) <<LOCKSET_SHIFT) / LOCKSET_SLICES;		// Jump N blocks to change the owner segment (change slice start)
		u64 data_P[2][slice_size];
		for (l=0; l<2; l++){										// Test 2 slice starts
			for (i=0; i<3; i++) {									// Each offset within slice
				const int ls_offset = (l << LOCKSET_SHIFT);		    // Test different slice start
				const int startBlock = _addr4k(l*owner_change_offset, off_in_lock[i]);
				const int slice_offset = startBlock % slice_size;  // Offset in slice to first block written
				const int first_slice_blks = min(2, slice_size-slice_offset);  // 2 is I/O size.
				const int last_slice_blks = 2 - first_slice_blks;   // 2 is I/O size;
				io_traits = get_io_traits(env, startBlock, lenBlocks);
				magics[0] = __unitest_fill_blocks_unique_pattern(&mem[0], 1);
				magics[1] = __unitest_fill_blocks_unique_pattern(&mem[NVMEIBC_SECTOR_SIZE], 1);
				// write 2 blocks on different offsets (make random?)
				rv = osSimulator_writeArrWait(&env.client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);

				memset(data_P, 0, sizeof(data_P));                  // Set up for parity calculation(s)
				memset(parity_P, 0, sizeof(parity_P));

				for (k = 0; k < slice_offset; k++) { // Verify data not written in slice before start of operation
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != 0);
				}
				for (k = slice_offset, m=0; k < slice_offset+first_slice_blks; k++, m++) { // Verify data was written prepare data for parity calculation
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != magics[m]);
					data_P[0][k] = magics[m];
				}
				for (k = 0, m=first_slice_blks; k < last_slice_blks; k++, m++) { // Verify second slice if IO spans 2 slices
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[1].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != magics[m]);
					data_P[1][k] = magics[m];
				}
				// Calculate parities without reed solomon
				parity_P[0][0] =                     __calculate_parity_P(slice_size, &(data_P[0][0]));
				if (n_parities > 1) parity_P[0][1] = __calculate_parity_Q(slice_size, &(data_P[0][0]));
				parity_P[1][0] =                     __calculate_parity_P(slice_size, &(data_P[1][0]));
				if (n_parities > 1) parity_P[1][1] = __calculate_parity_Q(slice_size, &(data_P[1][0]));

				for (m = 0, k = slice_size; k < (int)seg->replicas; k++, m++) {// Verify parities
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != parity_P[0][m]);
					if (last_slice_blks) { // Of second slice as well
						inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[1].sgmnt2dlba[seg_index], 0,0,0);
						BUG_ON(*(u64 *)inj.data != parity_P[1][m]);
					}
				}
				rv = osSimulator_trim(&env.client->OS, volInd, startBlock, lenBlocks);			REPORT_ERROR(rv);
				// Trim must do nothing at all
				for (k = 0; k < slice_offset; k++) { // Data must remain (unwritten)
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != 0);
				}
				for (k = slice_offset, m=0; k < slice_offset+first_slice_blks; k++, m++) {// Data must remain
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != magics[m]);
				}
				for (k = 0, m=first_slice_blks; k < last_slice_blks; k++, m++) {// Second slice if necessary
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[1].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != magics[m]);
				}
				for (m = 0, k = slice_size; k < (int)seg->replicas; k++, m++) { // Parities
					const int seg_index = (k+l)%seg->replicas;
					struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
					BUG_ON(*(u64 *)inj.data != parity_P[0][m]);
					if (last_slice_blks) {
						inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[1].sgmnt2dlba[seg_index], 0,0,0);
						BUG_ON(*(u64 *)inj.data != parity_P[1][m]);
					}
				}
				__verify_ec_2block_read(env.client, volInd, mem, startBlock, lenBlocks, magics);// Verify data correctly read from block device

				// Simple Good path Sync operation (checks journal which is fine and fixes lock only)
				if (1) { // More precise tests in HTR tests
					const struct disk_range* own_lock_seg = &seg[l]; //seg_of(i, 0);
					const u64 own_lock_off = (own_lock_seg->dlba_start + ls_offset + (off_in_lock[i]+0)/slice_size);
					struct ramDiskSimulator *ram = &(env.sys->servers[own_lock_seg->node_id].ramDisk);
					for (p = 0; p < n_parities; p++) { // Currupt the parities
						const int seg_index = (slice_size+p+l)%seg->replicas;
						struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
						struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
						*(u64 *)inj.data = (u64)0x17;
					}
					// Trigger stale lock recovery by injecting the lock with stale and reading
					ramDiskSimulator_lockStale(ram, own_lock_off);
					__verify_ec_2block_read(env.client, volInd, mem, startBlock, lenBlocks, magics);// Verify data correctly read
					clientSimulator_wait_for_all_sync_ops(env.client);
					BUG_ON(*cur_num_syncs != ++prev_num_syncs);
					BUG_ON(ramDiskSimulator_lockIsSta(ram, own_lock_off));
					for (p = 0; p < n_parities; p++) { // Verify parities are unchanged on drive and reset their value
						const int seg_index = (slice_size+p+l)%seg->replicas;
						struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
						struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[seg_index], 0,0,0);
						BUG_ON(*(u64 *)inj.data != (u64)0x17);
						*(u64 *)inj.data = parity_P[0][p];
					}

					if (1) {// Test sync logic, TODO: Add test to actually affect the sync (journal coruption and verify data is rolled forward)
						ramDiskSimulator_lockStale(ram, own_lock_off);
						__verify_ec_2block_read(env.client, volInd, mem, startBlock, lenBlocks, magics);// Verify data correctly read
						clientSimulator_wait_for_all_sync_ops(env.client);
						BUG_ON(*cur_num_syncs != ++prev_num_syncs);
						BUG_ON(ramDiskSimulator_lockIsSta(ram, own_lock_off));
					}
				}
				free_io_traits(&io_traits);
			}
		}
		clientSimulator_wait_for_all_sync_ops(env.client);
		//prev_num_syncs += 12; -- No Syncs all are tested in HTR
		BUG_ON(*cur_num_syncs != prev_num_syncs);						// Exactly 0 sync operation occured, in each scenario
	}
	if (vol->nChunks > 1) {	// Test 2-blocks IO on multi-raid IO, multi chunk IO: Raid5(3+1) or Raid6(2+2)
		const int stripe_width = seg->stripe_width, slice_size = seg->slice_size;
		const int n_parities = seg->replicas - slice_size;			// 1 - Raid5, 2 -Raid6
		const int n_blocks_in_stripe = stripe_width*slice_size*LOCKSET_SLICES;
		const int last_block_in_lock = slice_size*LOCKSET_SLICES-1;
		const int last_block_in_chunk= (seg->length*n_blocks_in_stripe)/LOCKSET_SLICES - 1;
		int off_in_vlba[]   = {last_block_in_lock, n_blocks_in_stripe-1, last_block_in_chunk};	    // Last block in lock, last in stripe, last in chunk
		// Dynamic verification -> write on first stripe, last stripe, last stripe for the first slice
		// Dynamic verification -> write on second stripe, first stripe, next chunk for the second slice
		const int d0[3][2]  = { {0, seg->replicas}, {(stripe_width-1)*seg->replicas, 0}, {(stripe_width-1)*seg->replicas, stripe_width*seg->replicas}};
        u64 data_P[2][slice_size];
		for (i = 0; i < (int)ARRAY_SIZE(off_in_vlba); i++) {
            const int startBlock = _addr4k(0, off_in_vlba[i]);
			struct io_traits io_traits1 = get_io_traits(env, startBlock+1, lenBlocks - 1);	// Second chunk
			io_traits = get_io_traits(env, startBlock, lenBlocks - 1);						// First chunk

            magics[0] = __unitest_fill_blocks_unique_pattern(&mem[0], 1);
			magics[1] = __unitest_fill_blocks_unique_pattern(&mem[NVMEIBC_SECTOR_SIZE], 1);
			rv = osSimulator_writeArrWait(&env.client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);

            memset(data_P, 0, sizeof(data_P));                  // Set up for parity calculation(s)
            memset(parity_P, 0, sizeof(parity_P));

			{ // Verify first praid data in last slice (of praid)
				const int r2s = io_traits.slices[0].role2sgmnt[slice_size-1];
				const int si = d0[i][0] + r2s;
				struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[si]);
				u64 dlba = io_traits.slices[0].sgmnt2dlba[io_traits.slices[0].role2sgmnt[slice_size-1]];
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
				BUG_ON(*(u64 *)inj.data != magics[0]);
				data_P[0][1] = magics[0];
				if (seg[si].slice_size > 1) { // Data seg before op start
					rdisk = __get_server_by_seg(sys, &seg[si - 1]);
					dlba = io_traits.slices[0].sgmnt2dlba[io_traits.slices[0].role2sgmnt[slice_size-2]];
					 inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0, 0,0);
					BUG_ON(*(u64 *)inj.data != 0);
				}
			}
            { // Verify second praid data in first slice (of praid)
				const int r2s = io_traits1.slices[0].role2sgmnt[0];
				const int si = d0[i][1] + r2s;
				struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[si]);
				u64 dlba = io_traits1.slices[0].sgmnt2dlba[io_traits1.slices[0].role2sgmnt[0]];
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
				BUG_ON(*(u64 *)inj.data != magics[1]);
                data_P[1][0] = magics[1];
				if (seg[si].slice_size > 1) { // We do a 1 + 2 (so there is no D1, it's P and P == D0 == Q)
					rdisk = __get_server_by_seg(sys, &seg[si + 1]);
					dlba = io_traits1.slices[0].sgmnt2dlba[io_traits1.slices[0].role2sgmnt[1]];
					inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
					BUG_ON(*(u64 *)inj.data != 0);
				}
            }

            parity_P[0][0] =                     __calculate_parity_P(slice_size, &(data_P[0][0]));
            if (n_parities > 1) parity_P[0][1] = __calculate_parity_Q(slice_size, &(data_P[0][0]));
            parity_P[1][0] =                     __calculate_parity_P(slice_size, &(data_P[1][0]));
            if (n_parities > 1) parity_P[1][1] = __calculate_parity_Q(slice_size, &(data_P[1][0]));

			for (m = 0, k = slice_size; k < (int)seg->replicas; k++, m++) {// Verify parities (first chunk)
				const int r2s = io_traits.slices[0].role2sgmnt[k];
				const int si = d0[i][0] + r2s;
				struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[si]);
				u64 dlba = io_traits.slices[0].sgmnt2dlba[io_traits.slices[0].role2sgmnt[k]];
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
				BUG_ON(*(u64 *)inj.data != parity_P[0][m]);

			}
			for (m = 0, k = seg[d0[i][1]].slice_size; k < (int)seg[d0[i][1]].replicas; k++, m++) { // Verify parities (n_parities can differ between chunks)
				const int r2s = io_traits1.slices[0].role2sgmnt[k];
				const int si = d0[i][1] + r2s;
				struct serverSimulator	*rdisk = __get_server_by_seg(sys, &seg[si]);
				u64 dlba = io_traits1.slices[0].sgmnt2dlba[io_traits1.slices[0].role2sgmnt[k]];
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
				BUG_ON(*(u64 *)inj.data != parity_P[1][m]);
			}

			__verify_ec_2block_read(env.client, volInd, mem, startBlock, lenBlocks, magics);// Verify data correctly read
			free_io_traits(&io_traits);
			free_io_traits(&io_traits1);
		}
	}
	if (vol->nChunks > 1)
		seg += segs_in_first_chunk;
	if (vol->nChunks > 1) {		// Test simple 2 blocks IO on second chunk: : Raid5(2+1) or Raid6(1+2)
		const int stripe_width = seg->stripe_width, slice_size = seg->slice_size;
		const int n_parities = seg->replicas - slice_size;			// 1 - Raid5, 2 -Raid6
		int off_in_lock[2]   = { 6          , 11         };		// Different io offsets
		const int owner_change_offset = ((stripe_width * slice_size) <<LOCKSET_SHIFT) / LOCKSET_SLICES;		// Jump N blocks to change the owner segment
		const int blocks_in_chunk= seg->bd_start;
        u64 data_P[2][slice_size];
		unsigned prev_alignment_sectors = set_split_bio_to_new_value(env.dev, slice_size);
		env.sraid.vsi.chunk = 1;
		for (l=0; l<2; l++)										// Each slice start
		for (i=0; i<slice_size; i++) {							// Each offset within slice
			const int startBlock = _addr4k(l*owner_change_offset, off_in_lock[i]) + blocks_in_chunk;
            const int slice_offset = startBlock % slice_size;  // Offset in slice to first block written
            const int first_slice_blks = min(2, slice_size-slice_offset);  // 2 is I/O size.
            const int last_slice_blks = 2 - first_slice_blks;   // 2 is I/O size;
			io_traits = get_io_traits(env, startBlock, lenBlocks);
			magics[0] = __unitest_fill_blocks_unique_pattern(&mem[0], 1);
			magics[1] = __unitest_fill_blocks_unique_pattern(&mem[NVMEIBC_SECTOR_SIZE], 1);
			rv = osSimulator_writeArrWait(&env.client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
            memset(data_P, 0, sizeof(data_P));                  // Set up for parity calculation(s)
            memset(parity_P, 0, sizeof(parity_P));

            for (k = slice_offset, m=0; k < slice_offset+first_slice_blks; k++, m++) { // Verify first slice data
				const int seg_index = (k+l)%seg->replicas;
				struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
				u64 dlba = io_traits.slices[0].sgmnt2dlba[io_traits.slices[0].role2sgmnt[k]];
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
				BUG_ON(*(u64 *)inj.data != magics[m]);
                data_P[0][k] = magics[m];
            }
            for (k = 0, m=first_slice_blks; k < last_slice_blks; k++) { // Verify second slice data
                const int seg_index = (k+l)%seg->replicas;
				struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
				u64 dlba = io_traits.slices[1].sgmnt2dlba[io_traits.slices[1].role2sgmnt[k]];
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
				BUG_ON(*(u64 *)inj.data != magics[m]);
                data_P[1][k] = magics[m];
            }

            parity_P[0][0] =                     __calculate_parity_P(slice_size, &(data_P[0][0]));
            if (n_parities > 1) parity_P[0][1] = __calculate_parity_Q(slice_size, &(data_P[0][0]));
            parity_P[1][0] =                     __calculate_parity_P(slice_size, &(data_P[1][0]));
            if (n_parities > 1) parity_P[1][1] = __calculate_parity_Q(slice_size, &(data_P[1][0]));

			for (m = 0, k = slice_size; k < (int)seg->replicas; k++, m++) {// Verify parities (first slice)
				int seg_index = (slice_size + m + l)%seg->replicas;
				struct serverSimulator *rdisk = __get_server_by_seg(sys,&seg[seg_index]);
				u64 dlba = io_traits.slices[0].sgmnt2dlba[io_traits.slices[0].role2sgmnt[slice_size+m]];
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
				BUG_ON(*(u64 *)inj.data != parity_P[0][m]);
				if (last_slice_blks > 0) {
					dlba = io_traits.slices[1].sgmnt2dlba[io_traits.slices[1].role2sgmnt[slice_size+m]];
					inj = serverSimulator_get_block_inject_ptrs(rdisk, dlba, 0,0,0);
					BUG_ON(*(u64 *)inj.data != parity_P[1][m]);
				}
			}

			__verify_ec_2block_read(env.client, volInd, mem, startBlock, lenBlocks, magics);// Verify data correctly read
			free_io_traits(&io_traits);
		}
		set_split_bio_to_new_value(env.dev, prev_alignment_sectors);
	}
	if (0) {	// Test long IO: ~50 commands and locks
		const int stripe_width = seg->stripe_width, slice_size = (seg->stripe_size%LOCKSET_SLICES)+1;
		const int n_blocks_in_stripe = stripe_width*slice_size*LOCKSET_SLICES;
		const int last_block_in_chunk= (seg->length*n_blocks_in_stripe)/LOCKSET_SLICES - 1;
        const int startBlock = _addr4k(0, last_block_in_chunk - lenBlocks/2);
		lenBlocks  = _addr4k(0,n_blocks_in_stripe*3);				// 3 full stripes
		magics[0] = __unitest_fill_blocks_unique_pattern(&mem[0], lenBlocks);
		rv = osSimulator_writeArrWait(&env.client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
		memset(mem, 0, lenBlocks*NVMEIBC_SECTOR_SIZE);              // Clear the array
		rv = osSimulator_readArrWait( &env.client->OS, volInd, startBlock, lenBlocks, mem);	BUG_ON(rv);
		__unitest_verify_blocks_pattern(mem, lenBlocks, magics[0], false);			// Verify that read and write matched.
	}

	if (1) {				// Test that READ-locked locks do not trigger sync and finish
		struct ramDiskSimulator *ram = &(sys->servers[0].ramDisk);
		union nvmeib_lock_blkset_entry lockid = nvmeib_stale_bit_mask_ec;		// Stale zero, journal-less
		lockid.lock_id.bits.is_read = 1;
		ram->locks[0] = lockid.all;
		rv = osSimulator_readArrWait(&env.client->OS, volInd, 0, 1, mem);	BUG_ON(rv);	// Read with view lock
		BUG_ON(ram->locks[0] != lockid.all);				// Read Succeeded regardless of stale-read-only-lock

		tTopoOfPraid_force_lock_on_read(&sys->tcf.vols[volInd].chunks[0].raids[0], true);
		tomaSimulator_switchTopo_dummy(volInd, 0, SW_TOPO__WAIT_ACK, NULL);
		rv = osSimulator_readArrWait(&env.client->OS, volInd, 0, 1, mem);	BUG_ON(rv);	// Read with view lock
		tTopoOfPraid_force_lock_on_read(&sys->tcf.vols[volInd].chunks[0].raids[0], false);
		tomaSimulator_switchTopo_dummy(volInd, 0, SW_TOPO__WAIT_ACK, NULL);
		BUG_ON(ram->locks[0] != 0);							// Read took locks and cleaned the volume without sync-ops
		BUG_ON(*cur_num_syncs != prev_num_syncs);			// Verify that sync operations never occured
	}

	if (1) { 	// test EC volume conversion of sector between volume & underlying storage (should be made a debug utility)
		struct dp_io_topo_iterator topo_it;
		int vol_ind = 0;
		const struct nvmeibc_block_device* dev = env.dev;
		u64 vol_size = dev->size;
		struct nvmeibc_topology *t = ___get_tail_topo_of_device(sys, vol_ind);
		dp_io_topo_iterator_init(&topo_it, 0, vol_size, t, -1);
		verify_nvmeibc_dlba_to_vlba_translation(&topo_it, &dev->dp);
	}
	// Reset to first chunk
	env.sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, (struct volume_segment_index){0,0,0,0});
	__test_io_maintanace_syncs(env, mem, false);
	clientSimulator_wait_for_all_bio_ops(env.client);			// wait for the sync operations to be free (sync op was completed but object wasnt freed yet)
	NVMeshSystem_all_clients_dbg_di(sys, false);
	NVMeshSystem_wipe_all_dirty_bits(B->sys);			// Coz IO in last possible degraded topology left dbits
	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	sim_kfree(mem);
	return rv;
}

// Disconnect the server and unregister the dead segment
void __degrade_segment(struct clientSimulator *client, struct tTopoOfPraid* r1, struct disk_range *praid, int ind_dead_seg) {
	serverSimulator_disconnect(serverOf(&client->physDiscs[praid[ind_dead_seg].node_id]));
	tomaSimulator_unreg_raid1(r1->header.uuid,ind_dead_seg);
}

// Re-connect the server and set the segment status to W
void __restore_seg_to_write(struct NVMeshSystem *sys, struct clientSimulator *client, struct tTopoOfPraid* r1, struct disk_range *praid, int ind_dead_seg, bool clear_ram) {
	int i, node_ind = praid[ind_dead_seg].node_id;
	enum NVMEIBTC_DS_MODE seg_stats[N_MAX_RAID_SLICE_LEN] = {0};
	for (i=0;i<r1->header.n_segments;i++) seg_stats[i] = r1->s[i].access_mode;
	seg_stats[ind_dead_seg] = NVMEIBTC_DS_MODE_W;
	serverSimulator_re_connect(serverOf(&client->physDiscs[node_ind]));
	NVMeshSystem_serialize(sys);
	tomaSimulator_switchTopoEC(r1->header.uuid, seg_stats, SW_TOPO__WAIT_ACK_DR, NULL);// {DEAD, W, RW, RW}
	if (clear_ram) {
		ramDiskSimulator_wipe_dirty_bits(&sys->servers[node_ind].ramDisk, 0x0);
		ramDiskSimulator_reset_txid(&sys->servers[node_ind].ramDisk);
	}
}

// Use switch Topo to update segment status from W to RW
void __restore_seg_to_read_write(struct NVMeshSystem *sys, struct tTopoOfPraid* r1, int ind_dead_seg) {
	int i;
	enum NVMEIBTC_DS_MODE seg_stats[N_MAX_RAID_SLICE_LEN] = {0};
	for (i=0;i<r1->header.n_segments;i++) seg_stats[i] = r1->s[i].access_mode;
	seg_stats[ind_dead_seg] = NVMEIBTC_DS_MODE_RW;
	tomaSimulator_switchTopoEC(r1->header.uuid, seg_stats, SW_TOPO__WAIT_ACK_DR, NULL); // {DEAD, RW, RW, RW}
	NVMeshSystem_serialize(sys);
}

static void __verify_all_segments_are_readable(struct tTopoOfPraid* praid){
	for (u8 i = 0; i < praid->header.n_segments; ++i)
		BUG_ON(praid->s[i].access_mode != NVMEIBTC_DS_MODE_RW);
}

// TODO: refactor degraded IO notion to be:
// 1. When generating data, generate P/Q...
// 2. When verifiying data, (additionally to reading) verify P/Q on disk, verify that EDIC is correct, verify MD of all involved data/journal/parity
// 3. Ensure that when we write/read in Degraded mode, we also "change" the degarded mode to ensure correct changes:
//		a. Write with Seg X degraded, and read when it is still degraded.
//		b. Before writing with Seg Y degraded, read again and verify DB sync is correct (data+MD+Journal)
// 		c. Make sure we degrade data segments after degraded Parities since it's very different in behaviour (The DI)
//		d. Ensure that a mix of D/W segments are used and shared with all possible segments.


// Use the IO parameters (startBLock, lenBlocks, memSizeBlocks, lockset_i) to issue a read into cmp_mem and comapre each block read with the expected ls_mem
static void read_and_verify_data(struct clientSimulator *client, int volInd, u8 *cmp_mem, u8 *ls_mem, int start, int startBlock, int lenBlocks, int memSizeBlocks, int lockset_i){
	// Issue degraded read and wait for completion
	int j, rv = osSimulator_readArrWait(&client->OS, volInd, start+startBlock+memSizeBlocks*lockset_i, lenBlocks, cmp_mem); BUG_ON(rv);
	for (j = 0; j < lenBlocks; j++) {// cmp the data read with the original data written
		__unitest_verify_blocks_pattern(&cmp_mem[j*NVMEIBC_SECTOR_SIZE], 1, *(u64*)&ls_mem[(startBlock+memSizeBlocks*lockset_i)*NVMEIBC_SECTOR_SIZE + j*NVMEIBC_SECTOR_SIZE], true);
	}
	memset(cmp_mem, 0, lenBlocks*NVMEIBC_SECTOR_SIZE);	// Reset the read data to ensure the next IO overrides the previous read data
}

static void __generate_data_inplace(u8 *dst, int start, int length)
{
	int j;
	for (j=0;j<length;j++) { // Non-Degraded write will dump all data
		__unitest_fill_blocks_unique_pattern(&dst[start + j*NVMEIBC_SECTOR_SIZE], 1);
	}
}

// Loops over li, startBlock, lenBlocks to issue all possible IOs in a lockset
static int __degraded_ec_io(bunitest_s* B, struct clientSimulator *client, int volInd, u8 *cmp_mem, u8 *ls_mem, int ls_jump, int ind_dead_seg, int next_dead, int memSizeBlocks, int slice_size, const bool is_read, int start)
{
	int li, startBlock, lenBlocks, io_count=0;
	for (li = 0; li < ls_jump; li++) {
		for (startBlock = 0; startBlock < (all_permutations ? 2*slice_size : slice_size); (all_permutations ? startBlock++ : (startBlock+=3))) {   // Keep startBlock to be within the blockset
			for (lenBlocks = 1; (lenBlocks+startBlock<LOCKSET_SLICES)&&(lenBlocks < 3*slice_size); (all_permutations ? lenBlocks++ : (lenBlocks+=3))) { // Issue IOs contained within the blockset, at most 3 slices (no need to test longer slices)
				// Issue degraded io and wait for completion
				if (is_read) {
					read_and_verify_data(client, volInd, cmp_mem, ls_mem, start, startBlock, lenBlocks, memSizeBlocks, li);
				} else { // Generate unique data for each write
					const int data_start_blocks = (startBlock+memSizeBlocks*li);
					const int data_start_bytes = data_start_blocks*NVMEIBC_SECTOR_SIZE;
					__generate_data_inplace(ls_mem, data_start_bytes, lenBlocks);
					osSimulator_writeArrWait(&client->OS, volInd, start+data_start_blocks, lenBlocks, &ls_mem[data_start_bytes]);
					read_and_verify_data(client, volInd, cmp_mem, ls_mem, start, startBlock, lenBlocks, memSizeBlocks, li);
					io_count++;
				}
				io_count++;
			} // for lenBlocks...
		} // for startBlock...

		for (startBlock = (LOCKSET_SLICES - 1)*slice_size - 1; startBlock < LOCKSET_SLICES*slice_size - 1; (all_permutations ? startBlock++ : (startBlock+=3))) {   // Keep startBlock to be within the blockset
			for (lenBlocks = 1; lenBlocks < 3*slice_size; (all_permutations ? lenBlocks++ : (lenBlocks+=3))) { // Issue IOs contained within the blockset, at most 3 slices (no need to test longer slices)
				// Issue degraded io and wait for completion
				if (is_read) {
					read_and_verify_data(client, volInd, cmp_mem, ls_mem, start, startBlock, lenBlocks, memSizeBlocks, li);
				} else { // Generate unique data for each write
					const int data_start_blocks = (startBlock+memSizeBlocks*li);
					const int data_start_bytes = data_start_blocks*NVMEIBC_SECTOR_SIZE;
					__generate_data_inplace(ls_mem, data_start_bytes, lenBlocks);
					osSimulator_writeArrWait(&client->OS, volInd, start+data_start_blocks, lenBlocks, &ls_mem[data_start_bytes]);
					read_and_verify_data(client, volInd, cmp_mem, ls_mem, start, startBlock, lenBlocks, memSizeBlocks, li);
					io_count++;
				}
				io_count++;
			} // for lenBlocks...
		} // for startBlock...
	} // for li...
#if 0 // For timing checks set to 1
	if (next_dead) unitest_print("%s: dead_seg1: %d, dead_seg2: %d, %d IOs took %d[msecs]\n", (is_read) ? "READS" : "WRITES", ind_dead_seg, ind_dead_seg+next_dead, io_count, bunitest_toc(B));
	else unitest_print("%s: dead_seg: %d, %d IOs took %d[msecs]\n", (is_read) ? "READS" : "WRITES", ind_dead_seg, io_count, bunitest_toc(B));
#else
	(void)next_dead;
	(void)ind_dead_seg;
	(void)B;
#endif
	return io_count;
}

static int __fill_locksets_with_data_degraded(bunitest_s* B, struct clientSimulator *client, const int volInd, u8 *cmp_mem, u8 *ls_mem, const int ls_jump, const int memSizeBlocks, const int slice_size, int ind_dead_seg, int next_dead, int start) {
	return __degraded_ec_io(B, client, volInd, cmp_mem, ls_mem, ls_jump, ind_dead_seg, next_dead, memSizeBlocks, slice_size, false, start);
}

static int __fill_locksets_with_data(bunitest_s* B, struct clientSimulator *client, const int volInd, u8 *cmp_mem, u8 *ls_mem, const int ls_jump, const int memSizeBlocks, const bool is_deg_write, const int slice_size, const int ind_dead_seg, const int next_dead, const int start) {
	int j, rv;
	if (is_deg_write) { // Do some unique IO patterns for degraded writes testing
		return __fill_locksets_with_data_degraded(B, client, volInd, cmp_mem, ls_mem, ls_jump, memSizeBlocks, slice_size, ind_dead_seg, next_dead, start);
	}
	// Generate entire locksets of data
	__generate_data_inplace(ls_mem, 0, memSizeBlocks*ls_jump);
	// Write entire locksets where each block has a unique value + block offset
	for (j=0;j<ls_jump;j++) {								// jump ls_jump locksets
		if (j) {
			rv = osSimulator_writeArrWait(&client->OS, volInd, start + j * memSizeBlocks, memSizeBlocks, &ls_mem[j * memSizeBlocks * NVMEIBC_SECTOR_SIZE]); BUG_ON(rv);
		} else { // Split the writes
			int write_start = start, len;
			for (len = 1; (len + write_start - start) < memSizeBlocks; len++) {
				rv = osSimulator_writeArrWait(&client->OS, volInd, write_start, len, &ls_mem[(write_start-start) * NVMEIBC_SECTOR_SIZE]); BUG_ON(rv);
				write_start += len;
				if (len+(write_start-start)+1 >= memSizeBlocks) { // Remainder
					rv = osSimulator_writeArrWait(&client->OS, volInd, write_start, memSizeBlocks-(write_start-start), &ls_mem[(write_start-start) * NVMEIBC_SECTOR_SIZE]); BUG_ON(rv);
				}
			}
		}
	}
	return rv;
}

static void __verify_writes_are_disabled(struct clientSimulator *client, const int v){
	struct nvmeibc_block_device *dev = client->devs[v];
	const u64 n_time_out = dp_io_stats_get_counter(&dev->dp.io_stats, DP_IO_STATS_TIMED_OUT) + 1;
	const ulong prev_jiff = dev->max_retry_jiffies;
	clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs 0", client->devs[v]->name);
	BUG_ON(osSimulator_writeArrWait(&client->OS, v, 0, 1, page_address(ZERO_PAGE(0))) < 0);
	BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, v) 					   != -EIO);
	BUG_ON(n_time_out != dp_io_stats_get_counter(&dev->dp.io_stats, DP_IO_STATS_TIMED_OUT));		// Write Timeed out in resubmition
	dev->max_retry_jiffies = prev_jiff;
	clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", client->devs[v]->name, (int)(prev_jiff/HZ));
}

static void __verify_ec_dirty_bits(union nvmeibc_dbits_entry *o_dbits, union nvmeibc_dbits_entry *p_dbits,
								   union nvmeibc_dbits_entry *q_dbits, int ind_dead_seg, int slice_size) {
	switch (ind_dead_seg) {
	case 0:	// Owner (d0) is dead
		BUG_ON(p_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(q_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(o_dbits->all_bits);
		break;
	case 1:	// Dirtybits not merged correctly due to missing copy to owner all segments show the same
		BUG_ON(p_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(q_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
		break;
	case 2: // Merged with previous should hold both 2 and 3 (P is dead so it's unchanged)
		if (slice_size > 2) { // 8+2 P is not dead
			BUG_ON(p_dbits->slmod.dead0 != ind_dead_seg+1);
			BUG_ON(q_dbits->slmod.dead0 != ind_dead_seg+1);
			BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
		} else {
			BUG_ON(p_dbits->all_bits);					// This is P and it's dead
			BUG_ON(q_dbits->slmod.dead0 != ind_dead_seg+1);
			BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
		}
		break;
	case 3:
		if (slice_size > 2) {
			BUG_ON(p_dbits->slmod.dead0 != ind_dead_seg+1);
			BUG_ON(q_dbits->slmod.dead0 != ind_dead_seg+1);
			BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
		} else {
			BUG_ON(q_dbits->all_bits);
			BUG_ON(p_dbits->slmod.dead0 != ind_dead_seg+1);
			BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
		}
		break;
	case 8:
		BUG_ON(p_dbits->all_bits);						// This is P and it's dead
		BUG_ON(q_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
		break;
	case 9:
		BUG_ON(q_dbits->all_bits);						// This is Q and it's dead
		BUG_ON(p_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
		break;
	default:
		BUG_ON(p_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(q_dbits->slmod.dead0 != ind_dead_seg+1);
		BUG_ON(o_dbits->slmod.dead0 != ind_dead_seg+1);
	}
	if (ind_dead_seg != slice_size) {
		BUG_ON(nvmeibc_dbits_entry_is_global_mode(p_dbits));
	}
	if (ind_dead_seg != slice_size + 1) {
		BUG_ON(nvmeibc_dbits_entry_is_global_mode(q_dbits));
	}
	if (ind_dead_seg) { // Zero when seg is dead
		BUG_ON(nvmeibc_dbits_entry_is_global_mode(o_dbits));
	}
}

static void __verify_ec_dirty_bits_no_dead_segs(union nvmeibc_dbits_entry *o_dbits, union nvmeibc_dbits_entry *p_dbits,
												union nvmeibc_dbits_entry *q_dbits) {
	BUG_ON(p_dbits->all_bits | q_dbits->all_bits | o_dbits->all_bits);
}

void __dd_clean_dlba_pointers(struct test_context ctx){
	int i;
	for (i = 0; i < ctx.sraid.tpr->header.n_segments; ++i){
		const struct disk_range* drange = &ctx.sraid.cpr[i];
		struct ramDiskSimulator* ssd = &ctx.sys->servers[drange->node_id].ramDisk;
		ramDiskSimulator_wipe_dirty_bits(ssd, 0x0);
		ramDiskSimulator_wipeMD(ssd, nvmeib_disk_init_md_max);
	}
}

//#define is_read_only_ec_degraded_mode(n_parties, n_dead_segs)      ((n_parities == n_dead_segs))										// We should be read-only
//#define is_not_ioable_ec_full_raid_down(first_dead, last_dead, r1) ((first_dead == 0) && ((last_dead)+1 == r1->header.n_segments    ))	// All data segs are down

// Switch topo and cut of tomas if dead, return value sets params for test execution
struct degraded_test_params __switch_to_new_topo(struct clientSimulator *client, struct topology_sgmnts_t new_topo, struct tTopoOfPraid* r1, struct disk_range *curSeg) {
	struct degraded_test_params rv;
	rv.is_double_degraded = (new_topo.dgrd_modes[0] != NVMEIBTC_DS_MODE_RW && new_topo.dgrd_modes[1] != NVMEIBTC_DS_MODE_RW);
	rv.any_seg_is_write = ((new_topo.dgrd_modes[0] == NVMEIBTC_DS_MODE_W) || (new_topo.dgrd_modes[1] == NVMEIBTC_DS_MODE_W));
	// Set new topo
	tomaSimulator_switchTopoEC(r1->header.uuid, new_topo.modes, SW_TOPO__WAIT_ACK_DR, NULL);
	// Cut down TOMA when segment is dead
	if (new_topo.dgrd_modes[0] == NVMEIBTC_DS_MODE_DEAD) __degrade_segment(client, r1, curSeg, new_topo.dgrd_sgmnts[0]);
	if (new_topo.dgrd_modes[1] == NVMEIBTC_DS_MODE_DEAD) __degrade_segment(client, r1, curSeg, new_topo.dgrd_sgmnts[1]);
	return rv;
}

// If any segments cut of toma re connect them in case they are in W mode next iteration
void __prepare_for_next_iteration(struct clientSimulator *client, struct topology_sgmnts_t new_topo, struct disk_range *curSeg) {
	if (new_topo.dgrd_modes[0] == NVMEIBTC_DS_MODE_DEAD) {
		serverSimulator_re_connect(serverOf(&client->physDiscs[curSeg[new_topo.dgrd_sgmnts[0]].node_id]));
	}
	if (new_topo.dgrd_modes[1] == NVMEIBTC_DS_MODE_DEAD) {
		serverSimulator_re_connect(serverOf(&client->physDiscs[curSeg[new_topo.dgrd_sgmnts[1]].node_id]));
	}
}

static void __verify_tx_id(struct block_inject_ptrs *owner_ram_inj, struct block_inject_ptrs *p_ram_inj, struct block_inject_ptrs *q_ram_inj, const u32 TxID){
	BUG_ON((*owner_ram_inj->ram.txid != *p_ram_inj->ram.txid)||(*owner_ram_inj->ram.txid != *q_ram_inj->ram.txid));
	BUG_ON(q_ram_inj->dmd->tx_id != p_ram_inj->dmd->tx_id);
	BUG_ON(*owner_ram_inj->ram.txid != TxID);
}

static u32 __verify_ram_and_md(struct topology_sgmnts_t topo, struct block_inject_ptrs *owner_ram_inj, struct block_inject_ptrs *p_ram_inj, struct block_inject_ptrs *q_ram_inj, const int slice_size) {
	const int segment_index = topo.dgrd_sgmnts[0];
	BUG_ON(topo.dgrd_modes[1] != NVMEIBTC_DS_MODE_RW);
	if (topo.dgrd_modes[0] == NVMEIBTC_DS_MODE_DEAD) { // Dead seg creates DB
		__verify_ec_dirty_bits(owner_ram_inj->ram.dbits, p_ram_inj->ram.dbits, q_ram_inj->ram.dbits, segment_index, slice_size);
		if (slice_size == segment_index) // We unset Dbits for the MD (not the lock)
			BUG_ON((p_ram_inj->dmd->P.dbits_0 != 0) || (p_ram_inj->dmd->P.dbits_1 != 0));
		else
			BUG_ON((p_ram_inj->dmd->P.dbits_0 != segment_index+1) || (p_ram_inj->dmd->P.dbits_1 != 0));
		if ((slice_size + 1) == segment_index) // We unset Dbits for the MD (not the lock)
			BUG_ON((q_ram_inj->dmd->P.dbits_0 != 0)||(q_ram_inj->dmd->P.dbits_1 != 0));
		else
			BUG_ON((q_ram_inj->dmd->P.dbits_0 != segment_index+1)||(q_ram_inj->dmd->P.dbits_1 != 0));
	} else if (topo.dgrd_modes[0] == NVMEIBTC_DS_MODE_W) { // Cleanup occured
		BUG_ON((p_ram_inj->dmd->P.dbits_0 != 0) || (p_ram_inj->dmd->P.dbits_1 != 0));
		BUG_ON((q_ram_inj->dmd->P.dbits_0 != 0)||(q_ram_inj->dmd->P.dbits_1 != 0));
		__verify_ec_dirty_bits_no_dead_segs(owner_ram_inj->ram.dbits, p_ram_inj->ram.dbits, q_ram_inj->ram.dbits);
		// Verify TxID increments correctly in W mode
		{
			u32 TxID = max(*owner_ram_inj->ram.txid, *p_ram_inj->ram.txid);
			TxID = max(TxID, *q_ram_inj->ram.txid);
			__verify_tx_id(owner_ram_inj, p_ram_inj, q_ram_inj, TxID);
			return TxID;
		}
	}
	return 0;
}

/* Test client's EC raid going in and out of a degraded mode */
TEST_FUNC int unitest_DegradedMode_EC(bunitest_s* B){
	struct volume_segment_index vsi = {0,0,0,0};
	struct test_context env = { .sys = B->sys
								, .client = B->sys->clients
								, .dev = B->sys->clients->devs[0]
								, .sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi)};
	struct NVMeshSystem *sys = B->sys;
	int is_deg_write = 0;				// Volume will be used to test switch topology
	struct clientSimulator *client = env.client;		// Test via the first client
	struct nvmeibc_block_device *dev = client->devs[vsi.volume];
	struct disk_range *curSeg = &sys->mdb.vols[vsi.volume].segs[0];
	const int chunkOffset = _addr4k(0,0);					// IO's will be at this offset from beggining of the first chunk (TODO: multi-chunk)
	int io_count = 0, rv = 0, startBlock, c, seg_in_r1_offset = 0, ls_jump=2;	// Use 2 blocksets first and 3rd
	const int slice = curSeg->slice_size;
	const int width = curSeg->stripe_width;
	const int n_p = curSeg->replicas - slice;
	int memSizeBlocks = 2*LOCKSET_SLICES*slice*width;	// Set a contiguous Lockset array and populate it once and compare all reads later
	const int memSize = memSizeBlocks*NVMEIBC_SECTOR_SIZE*ls_jump;
	u8 *ls_mem = sim_kzalloc(memSize, GFP_KERNEL);								// Array to write to disk and compare with reads later
	u8 *cmp_mem = sim_kzalloc(memSize, GFP_KERNEL);	// Array to read from volume and compare to ls_mem

	u32 TxID = 0;
	__generate_data_inplace(ls_mem, 0, memSizeBlocks*2);
	nvmeibc_io_perm_alert_set_unprotect_period(&dev->dp.io_perm_alert, 0);

	NVMeshSystem_all_clients_dbg_di(sys, true);
	bunitest_tic(B);
	if (B->conf->bunitest.enableEC_exhastiveTests) all_permutations=true; // Set value to all
	#define segs_in_chunk() (curSeg->replicas * curSeg->stripe_width)
	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state

	// Write on 2 complete locksets (0 and 2) - Non degraded writes test
	// Enter single degraded mode (1-2 segs at a time)
	// Write while degraded (for degraded write flavor of RAID-6)
	// Configure reads that vary by:
	// 1. Start block from 0->Lockset_Size-1
	// 2. Len blocks from 1->Lockset_size - start_block
	// 3. Send read and verify data depending on start block and len blocks - the actual action for each run
	for (c = 0; c < sys->tcf.vols[vsi.volume].nChunks; c++, seg_in_r1_offset += segs_in_chunk(), curSeg += segs_in_chunk()) { // TODO add more chunks
		struct tTopoOfPraid* r1 = &sys->tcf.vols[vsi.volume].chunks[c].raids[0];
		struct topologies_enumerator topos;
		const int slice_size = curSeg->slice_size;
		int n_parities = curSeg->replicas - slice_size;
		unsigned prev_alignment_sectors = 0;
		vsi.chunk = c;
		env.sraid = NVMeshSystem_TstPRaid_init_rel(env.sys, vsi);
		startBlock = __from4K(curSeg->bd_start) + chunkOffset;  // TODO (when multichunk use this for indication)
		if (slice_size == 1) { // 1+2 doesn't work for now
			continue;
		}
		if (c) { // TODO when using snake apply consecutive_blocks and verify based on slice_width
			prev_alignment_sectors = set_split_bio_to_new_value(dev, slice_size);
		}
		for (is_deg_write=0; is_deg_write<n_parities; is_deg_write++) { // 1 iteration for RAID-5, RAID-6 also gets another iteration for degraded writes
			topos = create_no_protection_topo_enum_ordered(env.sraid);
			io_count += __fill_locksets_with_data(B, client, vsi.volume, cmp_mem, ls_mem, ls_jump, memSizeBlocks, false, slice_size, 0, 0, startBlock);
			while (topos.move_next(&topos)) {
				struct topology_sgmnts_t topo = topos.curr;
				struct degraded_test_params dtp = __switch_to_new_topo(client, topo, r1, curSeg);
				const bool are_writes_disabled = (dtp.is_double_degraded || n_parities == 1);	// Double degraded writes are forbidden (single degraded with 1 P as well)
				if (are_writes_disabled) {	// Double degraded writes are forbidden (single degraded with 1 P as well)
					__verify_writes_are_disabled(client, vsi.volume);
					if ((slice_size==3) && !dtp.any_seg_is_write) {	// the test fails to verify MD if any segment is in W mode (since we clean it up)
						__test_io_maintanace_syncs(env, ls_mem, true);
					}
				} else {
					if (is_deg_write) { // Single degraded dead or W seg
						struct io_traits io_t = get_io_traits(env, startBlock, slice_size); // Analyze first slice only
						struct block_inject_ptrs o_ram_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[curSeg[io_t.slices[0].role2sgmnt[0]].node_id], io_t.slices[0].sgmnt2dlba[io_t.slices[0].role2sgmnt[0]], 0, 0, 0);
						struct block_inject_ptrs p_ram_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[curSeg[io_t.slices[0].role2sgmnt[slice_size]].node_id], io_t.slices[0].sgmnt2dlba[io_t.slices[0].role2sgmnt[slice_size]], 0, 0, 0);
						struct block_inject_ptrs q_ram_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[curSeg[io_t.slices[0].role2sgmnt[slice_size+1]].node_id], io_t.slices[0].sgmnt2dlba[io_t.slices[0].role2sgmnt[slice_size+1]], 0, 0, 0);
						if (!dtp.any_seg_is_write) {	// the test fails to verify MD if any segment is in W mode (since we clean it up)
							__test_io_maintanace_syncs(env, ls_mem, false);
							for (int i=0; i<sys->nServers; i++){
								ramDiskSimulator_verify_no_locks(      &sys->servers[i].ramDisk);
								ramDiskSimulator_verify_no_bad_sectors(&sys->servers[i].ramDisk);
								tomaSimulator_verify_no_locks(         &sys->servers[i].simToma);
							}
						}
						// Degraded writes and reads
						io_count += __fill_locksets_with_data(B, client, vsi.volume, cmp_mem, ls_mem, ls_jump, memSizeBlocks, true, slice_size, topo.dgrd_sgmnts[0], 0, startBlock);
						if ((TxID = __verify_ram_and_md(topo, &o_ram_inj, &p_ram_inj, &q_ram_inj, slice_size)) != 0){ // Verify current TXID
							// Reproduction of EC-805 bug
							osSimulator_writeArrWait(&client->OS, vsi.volume, startBlock, slice_size, ls_mem);
							__verify_tx_id(&o_ram_inj, &p_ram_inj, &q_ram_inj, TxID+1);
							BUG_ON(o_ram_inj.dmd->tx_id != p_ram_inj.dmd->tx_id);
							BUG_ON(q_ram_inj.dmd->tx_id != p_ram_inj.dmd->tx_id);
							BUG_ON(o_ram_inj.dmd->tx_id != TxID+1);
						}
						free_io_traits(&io_t);
					}
				} // Degraded reads always execute
				io_count += __degraded_ec_io(B, client, vsi.volume, cmp_mem, ls_mem, ls_jump, topo.dgrd_sgmnts[0], topo.dgrd_sgmnts[1] - topo.dgrd_sgmnts[0], memSizeBlocks, slice_size, true, startBlock);
				__prepare_for_next_iteration(client, topo, curSeg);
			}
			{	// Reset all topologies back to RW
				enum NVMEIBTC_DS_MODE reset[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
				tomaSimulator_switchTopoEC(r1->header.uuid, reset, SW_TOPO__WAIT_ACK_DR, NULL);
			}
		}
		if (prev_alignment_sectors)
			set_split_bio_to_new_value(dev, prev_alignment_sectors);

		BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	}
	BUG_ON(rv);
	sim_kfree(ls_mem);
	sim_kfree(cmp_mem);
	NVMeshSystem_all_clients_dbg_di(sys, false);
	nvmeibc_io_perm_alert_set_unprotect_period(&dev->dp.io_perm_alert, 10*60);
	unitest_print("*************** Degraded_EC_IO RAID-%d%s (%d+%d) sent %d IOs in %d[mSec]\n", (n_p == 1) ? 5 : 6, (width > 1) ? "0" : " ", slice, n_p, io_count, bunitest_toc(B));
	return rv;
}

TEST_FUNC int unitest_EC_8127(bunitest_s* B) {
	struct volume_segment_index vsi = {0,0,0,0};
	struct test_context env = { .sys = B->sys
								, .client = B->sys->clients
								, .dev = B->sys->clients->devs[0]
								, .sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi)};
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator *client = env.client;		// Test via the first client
	struct tTopoOfPraid* r1 = &sys->tcf.vols[vsi.volume].chunks[0].raids[0];
	struct disk_range *curSeg = &sys->mdb.vols[vsi.volume].segs[0];
	const int slice = curSeg->slice_size;
	const int width = curSeg->stripe_width;
	const int memSizeBlocks = 1;
	const int memSize = memSizeBlocks * NVMEIBC_SECTOR_SIZE;
	u8 *ls_mem = sim_kzalloc(memSize, GFP_KERNEL);
	int startBlock = slice * width;	// 2nd slice
	struct topology_sgmnts_t topo = {
			.modes = { [0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW },
			.dgrd_sgmnts = { -1, -1 },
			.dgrd_modes = { NVMEIBTC_DS_MODE_INVALID, NVMEIBTC_DS_MODE_INVALID } };

	struct io_traits io_t = get_io_traits(env, startBlock, slice); // Analyze first slice only
	struct block_inject_ptrs o_ram_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[curSeg[io_t.slices[0].role2sgmnt[0]].node_id], io_t.slices[0].sgmnt2dlba[io_t.slices[0].role2sgmnt[0]], 0, 0, 0);
	struct block_inject_ptrs p_ram_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[curSeg[io_t.slices[0].role2sgmnt[slice]].node_id], io_t.slices[0].sgmnt2dlba[io_t.slices[0].role2sgmnt[slice]], 0, 0, 0);
	struct block_inject_ptrs q_ram_inj = serverSimulator_get_block_inject_ptrs(&sys->servers[curSeg[io_t.slices[0].role2sgmnt[slice + 1]].node_id], io_t.slices[0].sgmnt2dlba[io_t.slices[0].role2sgmnt[slice + 1]], 0, 0, 0);
	struct nvmeibc_disk *p_disk = sys->servers[curSeg[io_t.slices[0].role2sgmnt[slice]].node_id].client_disks[client->inst_id];
	struct nvmeibc_disk_hooks disk_hooks;

	NVMeshSystem_all_clients_dbg_di(sys, true);	// Previous test wrote with true, so pre-reads will fail if not set to true or just write full slices

	__generate_data_inplace(ls_mem, 0, memSizeBlocks);

	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state

	// Kill P
	topo.dgrd_sgmnts[0] = slice;
	topo.dgrd_modes[0] = NVMEIBTC_DS_MODE_DEAD;
	topo.modes[topo.dgrd_sgmnts[0]] = topo.dgrd_modes[0];
	__switch_to_new_topo(client, topo, r1, curSeg);
	tomaSimulator_waitProtoEnd(NULL);

	// Set DBits in slice 1 (zero based)
	osSimulator_writeArrWait(&client->OS, vsi.volume, startBlock, memSizeBlocks, ls_mem);

	__prepare_for_next_iteration(client, topo, curSeg);

	// P DEAD->W
	topo.dgrd_modes[0] = NVMEIBTC_DS_MODE_W;
	topo.modes[topo.dgrd_sgmnts[0]] = topo.dgrd_modes[0];
	__switch_to_new_topo(client, topo, r1, curSeg);
	tomaSimulator_waitProtoEnd(NULL);

	// Trigger resolve unknown - will read DBits from slice 1 and call DB sync
	o_ram_inj.ram.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
	p_ram_inj.ram.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
	q_ram_inj.ram.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;

	// Allow one write to P from dbits sync and fail the next one
	// before the fix this would leave unknown Dbits in the caller IO binfo and since Q was fixed with no DBits (for P)
	// but P wasn't fixed, rather had it's own DBits in it's MD, we would consider the blockset fixed and later when P is valid
	// upon the next unknown DB sync we would find these DBits and suspend the volume
	disk_hooks = (struct nvmeibc_disk_hooks){ .args.trerr = {true, true, true, 0, 0, INJECT_TRANSPORT_ERROR_IO, false, 1, 1}, .inject_transport_error = inject_transport_error };
	NVMeshSystem_gen_cmd_hooks_setup_single_disk(p_disk, &disk_hooks);

	// Trigger a write to another slice, the sync will fail, binfo will still be unknown and will call unknown resolve - Q no longer had DBits and the sync would leave DBits on P - the fix prevents this from happening
	osSimulator_writeArrWait(&client->OS, vsi.volume, 0, memSizeBlocks, ls_mem);

	NVMeshSystem_gen_cmd_hooks_setup_single_disk(p_disk, NULL);

	// No dbits after successful sync, moving P to RW, when the bug occurred P would become RW with it's own DBits set
	BUG_ON(o_ram_inj.ram.dbits->all_bits != 0);
	__prepare_for_next_iteration(client, topo, curSeg);
	topo.dgrd_modes[0] = NVMEIBTC_DS_MODE_RW;
	topo.modes[topo.dgrd_sgmnts[0]] = topo.dgrd_modes[0];
	__switch_to_new_topo(client, topo, r1, curSeg);

	// Now trigger resolve dbits sync which will read from P and will (bug) or will not (fix) discover dbits on RW seg.
	o_ram_inj.ram.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
	p_ram_inj.ram.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
	q_ram_inj.ram.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;

	// Write to another slice (0) - trigger reading DBits from parities, and will suspend the volume if illegal DBits are found
	osSimulator_writeArrWait(&client->OS, vsi.volume, 0, memSizeBlocks, ls_mem);

	{	// Reset all topologies back to RW
		enum NVMEIBTC_DS_MODE reset[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
		tomaSimulator_switchTopoEC(r1->header.uuid, reset, SW_TOPO__WAIT_ACK_DR, NULL);
	}

	NVMeshSystem_all_clients_dbg_di(sys, false);
	NVMeshSystem_wipe_all_md_of_disks(B->sys);

	free_io_traits(&io_t);
	sim_kfree(ls_mem);
	return 0;
}

// wipe all MD of blocks used by segments of a raid & journal of raid segments
void nvmeibc_raid_wipe_txid(struct nvmeibc_raid1 *raid) {
	struct nvmeibc_disk_segment *seg;
	int si;
	for (seg = raid->segments, si=0; si < raid->replicas ; seg++, si++) {
		struct ramDiskSimulator *D = &serverOf(seg->disk)->ramDisk;
		ramDiskSimulator_reset_txid(D);
	}
}


static int __get_jour_size(struct nvmeibc_disk_segment *seg) {
	return seg->disk->ops.get_journal(seg->disk)->rng_nlba / nvmeibc_jentry_num_blocks;
}
static void* __get_seg_journal_md_ptr(struct nvmeibc_disk_segment *seg) { return ramDiskSimulator_get_metadataptr(&serverOf(seg->disk)->ramDisk, seg->disk->ops.get_journal(seg->disk)->rng_slba); }

// scan all LOCKSET of raid members & verify the TxID is identical on slice_start & parities.
void nvmeibc_raid_verify_tx_id_replica_consistency(struct nvmeibc_raid1 *raid) {
	const u64 lockets_in_segs = nvmeibc_raid_get_slba_len(raid) / LOCKSET_SLICES;
	u32 owner_tx_id, pari_tx_id;
	int _si;
	u64 li;
	bool compare_parities_only = false;

	for (li=0; li < lockets_in_segs; li++) {
		int owner_index = (li/2)%raid->replicas;
		struct nvmeibc_disk_segment *owner_seg = &raid->segments[owner_index];
		struct ramDiskSimulator	    *owner_ram = &serverOf(owner_seg->disk)->ramDisk;
		ramDiskSimulator_read_txid(owner_ram, &owner_tx_id, owner_seg->first_lba / LOCKSET_SLICES + li, 1);	// read tx_id of owner segment
		if (owner_ram->state != ramDisk_running) compare_parities_only = true;
		for (_si = 0; _si < nvmeibc_raid1_get_protect_lvl(raid); _si++) {
			const int pari_ind = (owner_index + raid->slice_size + _si) % raid->replicas;
			struct nvmeibc_disk_segment *pari_seg = &raid->segments[pari_ind];
			struct ramDiskSimulator	    *pari_ram = &serverOf(pari_seg->disk)->ramDisk;
			if (pari_ram->state == ramDisk_down) {
				continue;
			}
			if (!compare_parities_only) {
				ramDiskSimulator_read_txid(pari_ram, &pari_tx_id, pari_seg->first_lba / LOCKSET_SLICES + li, 1);
				BUG_ON(owner_tx_id != pari_tx_id);
			} else { // Read first parity into owner, compare with second parity
				compare_parities_only = false;
				ramDiskSimulator_read_txid(pari_ram, &owner_tx_id, pari_seg->first_lba / LOCKSET_SLICES + li, 1);
			}
		}
	}
}

/* Debug function: Given 2D metadata array {N slices x N segments }, find
 * next slice (starting from 'slice_ind') that requires REDO
 * slice_ind : slice in journal, [0 .. slices_in_journal)
 * return slice index that requires REDO, -1 otherwise */
static int __journal_find_next(struct nvmeibc_disk_segment *seg, const void*pjmd[], int n_replicas, u32 md_size[], int jour_io_offset[], int slice_ind){
	int j_size = __get_jour_size(seg), si;
	BUG_ON(slice_ind >= j_size);
	for (; slice_ind < j_size; slice_ind++) {
		for (si = 0; si < n_replicas; si++) {
			const int byte_offset = md_size[si]*(jour_io_offset[si]*nvmeibc_jentry_num_blocks + slice_ind);
			union jblock_md *md = (void*)(pjmd[si] + byte_offset);
			if (!nvmeib_is_jmd_unused_entry(md))
				return slice_ind;
		}
	}
	return -1;	// not found
}

/* Old Test: Does multi-slice IO, regardless of binje. Even longer journal than journal entries. Test cleans illegal journals at the end.
 * Test the MD object created for various slices
 * The test needs the IO to write the whole journal but then fail to write all data. for now, we let the IO complete & identify the journal slices that were used.
 * when EC code matures, we'll need to inject an error in the PAR_AND_IO stage of cmds processing.
 * after the IO completed, we use logic based on sync that should find the un-completed journal entries, to verify them
 * once we have the journal entries, we can use the j2d pointer to the data blocks & from there to their MD objects.
 * since cmd's are created per LOCKSET, we test a single LOCKSET with multiple start/end lba */
TEST_FUNC int unitest_GoodPathIO_block_md_illegal_splits(bunitest_s* B) {
	struct test_context env = { .sys = B->sys, .client = B->sys->clients, .dev = B->sys->clients->devs[0], .sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, (struct volume_segment_index){0,0,0,0})};
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client
	const int vol_ind = 0, chunk_ind = 0, raid_ind = 0;
	struct TstPRaid pr = NVMeshSystem_TstPRaid_init_rel(B->sys, (struct volume_segment_index){0,0,0,0});
	//struct test_context env = {.sys = sys, .client = sys->clients,.dev = sys->clients->devs[p->inp.sraid.vsi.volume], .sraid = p->inp.sraid};
	struct nvmeibc_block_device *dev = client->devs[vol_ind];				// Todo: remove this
	struct nvmeibc_topology *t = nvmeibc_topology_get(&dev->topologies);	// Todo: remove this
	struct nvmeibc_raid1 *raid = &t->chunks[chunk_ind].raid1s[raid_ind];	// Todo: remove this
	const struct disk_range* seg_range = pr.cpr;
	const u32 slice_size = seg_range->slice_size, n_segs = seg_range->replicas;
	const u64 blocks_in_lockset = LOCKSET_SLICES * slice_size;
	const int mem_size	 = BYTES_IN_LOCKSET * slice_size;		// Total array in bytes
	u8        *mem 		 = sim_kzalloc(mem_size, GFP_KERNEL);		// Array to read/write to disk
	const u8 tmp_md[DISK_MAX_MD_SIZE_BYTE] = {0};
	u64	slice_start, start_block, n_blocks;
	int n_redo_slices, rv = 0, io_count = 0; u32 i;
	unsigned prev_split_bio_value;
	bunitest_tic(B);
	NVMeshSystem_all_clients_dbg_di(sys, true);
	__fill_block_device_with_unique_data(sys, vol_ind);
	prev_split_bio_value = set_split_bio_to_blockset(dev);
	for (slice_start = 0; slice_start < n_segs; slice_start++) {
		const struct serverSimulator	*owner_lock_server = &sys->servers[slice_start];
		const u64 raid_vlba_start = slice_start * (1<<LOCKSET_SHIFT) * slice_size * seg_range->stripe_width;		// the vlba at which we access the volume to have the slice rotation with the required slice_start
		if (slice_start * ((1<<LOCKSET_SHIFT)/LOCKSET_SLICES) >= (seg_range->length / LOCKSET_SLICES))
			continue;	// segment is too small. reduce LOCKSET_SHIFT to allow larger slice_start
		for (start_block = 0; start_block < blocks_in_lockset; start_block++) {
			struct nvmeibc_disk *disk = sys->servers[seg_range[0].node_id].disk;
			struct nvmeibc_disk_segment *seg = raid->segments;
			const int journal_size = __get_jour_size(seg);
			int                                         jour_io_offset[n_segs];	// On each disk: Where IO - starts
			const void *pjmd[   n_segs];	// allocate journal MD pointer per raid member
			union jblock_md *seg_jmd[n_segs];	// per-segment iterator over journal MD
			u32 md_size[n_segs];
			const u64 max_n_blocks = min((blocks_in_lockset - start_block), (u64)(3*slice_size));
			// wipe MD of disks we use for test.
			for (i=0; i < n_segs; i++) {
				disk = sys->servers[seg_range[i].node_id].disk;
				md_size[i] = nvmeibc_idisk_get_sw_md_size(&disk->base);
				ramDiskSimulator_wipeMD_jour(&serverOf(disk)->ramDisk, nvmeib_jmd_unused_entry_md_max());
				pjmd[i] = __get_seg_journal_md_ptr(&raid->segments[i]);
			}
			nvmeibc_raid_wipe_txid(raid);
			for (n_blocks = 1; n_blocks <= max_n_blocks; n_blocks++) {
				const u64 vlba = raid_vlba_start + start_block;
				struct io_traits io_traits = get_io_traits(env, vlba, n_blocks);
				u64 io_sgmnt2dlba[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = ILLEGAL_DLBA};
				const u32 own_lock_offset = (raid->segments[slice_start].first_lba + slice_start * (1<<LOCKSET_SHIFT)) / LOCKSET_SLICES;
				u32 pre__io_tx_id, post_io_tx_id;
				nvmeibc_guess_to_be_written_dlbas_for_io(env, io_traits, io_sgmnt2dlba);

				io_count++;
				nvmeibc_raid_verify_tx_id_replica_consistency(raid);
				ramDiskSimulator_read_txid(&owner_lock_server->ramDisk, &pre__io_tx_id, own_lock_offset, 1);
				nvmeibc_wait_for_all_jam_entires_to_be_free(sys, &pr, t);
				for (i=0; i < n_segs; i++) {
					jour_io_offset[i] = nvmeibc_jam_simu_alloc_entry(nvmeibc_disk_from_base(raid->segments[i].disk), io_sgmnt2dlba[i], pre__io_tx_id, true /*dry_run*/);
					BUG_ON(jour_io_offset[i] < 0);
					seg_jmd[i] = (void*)((u8*)pjmd[i] + md_size[i]*jour_io_offset[i]*nvmeibc_jentry_num_blocks);
				}

				rv = osSimulator_writeArrWait(&client->OS, vol_ind, vlba, n_blocks, mem);		REPORT_ERROR(rv);   	// IO will execute successfully, but wont release the owner lock
				ramDiskSimulator_read_txid(&owner_lock_server->ramDisk, &post_io_tx_id, own_lock_offset, 1);
				_ND(trace_uni_scenario_ec_unitest_GoodPathIO_block_md, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX @IO_COUNT",io_count);
				if (1) {		// now find the journal slices that need REDO (i.e.: complete the operation)
					int slice_ind, io_slice_ind;
					const int first_slice_in_lockset = start_block / slice_size;
					const int  last_slice_in_lockset = (start_block + n_blocks - 1) / slice_size;
					const int n_io_slices = last_slice_in_lockset - first_slice_in_lockset + 1;
					const u32 slice_start_off = start_block % slice_size;							// offset from slice_start, at which IO begins
					const u32 last_slice_size = ((start_block + n_blocks - 1) % slice_size) + 1;	// number of blocks in last slice.
					u32	exp_bmp;                                                 			// expected slice bmp
					int first_slice_ind = -1;

					BUG_ON(n_io_slices != io_traits.n_slices);
					BUG_ON((u64)first_slice_in_lockset != (io_traits.slices[0].slba % (1 << LOCKSET_SHIFT)));
					BUG_ON(io_traits.n_slices > 1 && last_slice_size != hweight16(io_traits.last_slice->tx_bm));
					BUG_ON(post_io_tx_id != pre__io_tx_id + 1);

					seg = &raid->segments[n_segs - 1];	// pick segment that holds the parity - its written by every IO.
					for (n_redo_slices=0, slice_ind=0; (slice_ind < journal_size) && (slice_ind != -1); slice_ind++) {
						slice_ind = __journal_find_next(seg, pjmd, n_segs, md_size, jour_io_offset, slice_ind);
						if (slice_ind == -1)
							break;
						n_redo_slices++;
						if (first_slice_ind == -1)
							first_slice_ind = slice_ind;						// first slice found
						io_slice_ind = slice_ind - first_slice_ind;				// index of slice within the multi-slice IO
						// decide on expected bitmap & then adjust to parity rotation
						if (     n_io_slices == 1)	              exp_bmp = GENMASK(n_blocks  -1     , 0) << slice_start_off;	// single slice IO
						else if (io_slice_ind == 0)               exp_bmp = GENMASK(slice_size-1     , slice_start_off);		// first slice
						else if (io_slice_ind == n_io_slices - 1) exp_bmp = GENMASK(last_slice_size-1, 0);						// last slice
						else                                      exp_bmp = GENMASK(slice_size-1     , 0);						// middle (full) slice

						if (io_slice_ind < io_traits.n_slices)
							BUG_ON(exp_bmp != io_traits.slices[io_slice_ind].tx_bm);

						for (i=0; i < n_segs; i++) {
							if ((exp_bmp & BIT(i)) || (i >= slice_size)) {			// members that were written (and all parities), should have the same tx_bmp
								const int seg_ind = (slice_start + i) % n_segs;		// adjust for member rotation
								const u32 exp_txid = pre__io_tx_id + 1;
								const u64 data_lba = seg_range[seg_ind].dlba_start + slice_start * (1 << LOCKSET_SHIFT) + first_slice_in_lockset + slice_ind;	// LBA on disk where the IO wrote the data
								union nvmeibc_block_dp_ec_data_block_md *dmd = (void*)tmp_md;		// Might read more than stored in 'dmd'
								struct jblock_md_decompressed md_decomp = nvmeibc_block_dp_ec_jmd_decode(seg_jmd[seg_ind]);
								// verify the journal blocks MD content
								BUG_ON(md_decomp.tx_bmp != exp_bmp);
								BUG_ON(md_decomp.tx_id !=  exp_txid);
								BUG_ON(md_decomp.j2d !=    data_lba);
								// verify the Data blocks MD content
								ramDiskSimulator_MD_read(__get_sdd_by_seg(sys, &seg_range[seg_ind]), data_lba, 1, dmd);
								BUG_ON(dmd->tx_id != exp_txid);
								BUG_ON(dmd->D.version != NVMEIBC_DATA_MD_VERSION);
								*seg_jmd[seg_ind] = nvmeib_jmd_unused_entry_val;		// Delete the journal metadata from disk to avoid confusion of next IO with this IO
								seg_jmd[seg_ind] = (((void*)seg_jmd[seg_ind]) + md_size[seg_ind]);
							}
						}
					}
					if (n_redo_slices != n_io_slices) // When binje >> n_io_slices (TODO: fix test to correctly asses multi-slice journal)
						BUG_ON(n_redo_slices > journal_size);
					nvmeibc_raid_verify_tx_id_replica_consistency(raid);
				}
				free_io_traits(&io_traits);
			}
		}
	}
	for (u32 snake_size = 2; snake_size <= 32; snake_size <<= 1) {
		u32 j;
		int n_parities = n_segs - slice_size;
		struct io_traits io_traits = get_io_traits(env, 0, slice_size); // Since the data is identical (except MD) we can check with old io traits
		u64 slice_magic[snake_size][n_segs];
		dev->dp.p.snake_size = snake_size;
		if (snake_size > 1) { // Vlbas no longer align
			dev->dp.enable_edic_check = false;
		}
		memset(mem, 0, mem_size);
		rv = osSimulator_writeArrWait(&client->OS, vol_ind, 0, blocks_in_lockset, mem);		REPORT_ERROR(rv);

		// First write entire blockset to sync with vlbas
		// write a full snake, calcualte Ps and verify on drive
		slice_magic[0][0] = __unitest_fill_blocks_unique_pattern(mem, snake_size * slice_size);
		for (i = 0; i < slice_size; i++) {
			for (j=0;j<snake_size;j++) {
				slice_magic[j][i] = slice_magic[0][0];
			}
		}
		for (i = 0; i < n_segs; i++) {
			struct serverSimulator *rdisk = __get_server_by_seg(sys, &seg_range[i]);
			struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[i], 0, 0, 0);
			BUG_ON(*(u64 *)inj.data != 0);
		}
		rv = osSimulator_writeArrWait(&client->OS, vol_ind, 0, snake_size*slice_size, mem);		REPORT_ERROR(rv);
		for (j=0;j<snake_size;j++) {
			slice_magic[j][slice_size] = __calculate_parity_P(slice_size, slice_magic[j]);
		}
		if (n_parities > 1) {
			for (j=0;j<snake_size;j++) {
				slice_magic[j][slice_size + 1] = __calculate_parity_Q(slice_size, slice_magic[j]);
			}
		}

		for (i = 0; i < n_segs; i++) {
			struct serverSimulator *rdisk = __get_server_by_seg(sys, &seg_range[i]);
			for (j = 0; j < snake_size; j++) {
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[i]+j, 0, 0, 0);
				BUG_ON(*(u64 *)inj.data != slice_magic[j][i]);
			}
		}

		// Write single column apply to P/Q and verify
		slice_magic[0][0] = __unitest_fill_blocks_unique_pattern(mem, snake_size);
		for (j=0;j<snake_size;j++) {
			slice_magic[j][0] = slice_magic[0][0];
		}
		rv = osSimulator_writeArrWait(&client->OS, vol_ind, 0, snake_size, mem);		REPORT_ERROR(rv);
		for (j=0;j<snake_size;j++) {
			slice_magic[j][slice_size] = __calculate_parity_P(slice_size, slice_magic[j]);
		}
		if (n_parities > 1) {
			for (j=0;j<snake_size;j++) {
				slice_magic[j][slice_size + 1] = __calculate_parity_Q(slice_size, slice_magic[j]);
			}
		}

		for (i = 0; i < n_segs; i++) {
			struct serverSimulator *rdisk = __get_server_by_seg(sys, &seg_range[i]);
			for (j = 0; j < snake_size; j++) {
				struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[i]+j, 0, 0, 0);
				BUG_ON(*(u64 *)inj.data != slice_magic[j][i]);
			}
		}

		// Write middle column single block apply to P/Q and verify that only this slice changed P/Q and Dmiddle
		if (slice_size > 2) {
			u32 slice_index = slice_size - 2;
			slice_magic[0][slice_index] = __unitest_fill_blocks_unique_pattern(&mem[NVMEIBC_SECTOR_SIZE*(slice_index*snake_size)], 1);
			rv = osSimulator_writeArrWait(&client->OS, vol_ind, slice_index*snake_size, 1, &mem[NVMEIBC_SECTOR_SIZE*(slice_index*snake_size)]);		REPORT_ERROR(rv);
			// Update parity for first slice
			slice_magic[0][slice_size] = __calculate_parity_P(slice_size, slice_magic[0]);
			if (n_parities > 1) {
				slice_magic[0][slice_size + 1] = __calculate_parity_Q(slice_size, slice_magic[0]);
			}
			for (i = 0; i < n_segs; i++) {
				struct serverSimulator *rdisk = __get_server_by_seg(sys, &seg_range[i]);
				for (j = 0; j < snake_size; j++) {
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[i]+j, 0, 0, 0);
					BUG_ON(*(u64 *)inj.data != slice_magic[j][i]);
				}
			}
		}
		if (1) {
			// Write from last slice in snake 2 blocks - other slice is first slice slice is last (mix of both is middle)
			slice_magic[snake_size-1][0] = __unitest_fill_blocks_unique_pattern(&mem[(snake_size-1)*NVMEIBC_SECTOR_SIZE], 1);
			slice_magic[0][1] = __unitest_fill_blocks_unique_pattern(&mem[(snake_size)*NVMEIBC_SECTOR_SIZE], 1);
			rv = osSimulator_writeArrWait(&client->OS, vol_ind, snake_size-1, 2, &mem[(snake_size-1)*NVMEIBC_SECTOR_SIZE]);		REPORT_ERROR(rv);
			// Update parities for first and last slices
			slice_magic[           0][slice_size] = __calculate_parity_P(slice_size, slice_magic[           0]);
			slice_magic[snake_size-1][slice_size] = __calculate_parity_P(slice_size, slice_magic[snake_size-1]);
			if (n_parities > 1) {
				slice_magic[           0][slice_size + 1] = __calculate_parity_Q(slice_size, slice_magic[           0]);
				slice_magic[snake_size-1][slice_size + 1] = __calculate_parity_Q(slice_size, slice_magic[snake_size-1]);
			}

			for (i = 0; i < n_segs; i++) {
				struct serverSimulator *rdisk = __get_server_by_seg(sys, &seg_range[i]);
				for (j = 0; j < snake_size; j++) {
					struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(rdisk, io_traits.slices[0].sgmnt2dlba[i]+j, 0, 0, 0);
					BUG_ON(*(u64 *)inj.data != slice_magic[j][i]);
				}
			}
		}

		free_io_traits(&io_traits);
	}
	// Cleaup
	dev->dp.p.snake_size = 1;
	rv = osSimulator_writeArrWait(&client->OS, vol_ind, 0, blocks_in_lockset, mem);		REPORT_ERROR(rv);
	dev->dp.enable_edic_check = true;
	// verify that the topology hasnt changed during the test, bcz it uses the raid/segment/journal of the topology it took at the beggining.
	BUG_ON(t != nvmeibc_topology_get(&dev->topologies));
	nvmeibc_topology_put(t);	// release ref we took for above comparison
	nvmeibc_topology_put(t);	// release ref we took at the beggining of this tes

	clientSimulator_wait_for_all_bio_ops(client);		// wait for the sync operations to be free (sync op was completed but object wasnt freed yet)
	NVMeshSystem_all_clients_dbg_di(sys, false);			// Restore the value
	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	set_split_bio_to_new_value(dev, prev_split_bio_value);
	NVMeshSystem_wipe_all_md_of_disks(sys);					// We did illegal IO (journals longer than binje, so must wipe out jourm-md or else serjio might complain about illegal entries
	sim_kfree(mem);
	_NI_dmesg(trace_1_uni_scenario_ec_unitest_GoodPathIO_block_md, "*************** @FUNCTION sent @IO_COUNT IOs in @BUNITEST_TOC[mSec] *************** end", __FUNCTION__, io_count, bunitest_toc(B));
	return 0;
}

static void __binje8_write_read_io(struct test_context *env, u64 startBlock, int lenBlocks, u8 *mem) {
	const u64 magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	int rv;
	rv = osSimulator_writeArrWait(&env->client->OS, env->sraid.vsi.volume, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	memset(mem, 0, lenBlocks*NVMEIBC_SECTOR_SIZE);					// Clear the array
	rv = osSimulator_readArrWait(&env->client->OS, env->sraid.vsi.volume, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(mem, lenBlocks, magic_pattern, false);			// Verify that read and write matched.
}

TEST_FUNC int unitest_EC_GoodPath_binje8(bunitest_s* B) { // Select volume 0 (8+2) for tests
	struct test_context env = { .sys = B->sys, .client = B->sys->clients, .dev = B->sys->clients->devs[0], .sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, (struct volume_segment_index){0,0,0,0})};
	struct nvmeibc_block_device *dev = env.dev;
	int i;
	enum NVMEIBTC_DS_MODE seg_stats[N_MAX_RAID_SLICE_LEN] = {0};
	u8*mem = sim_kmalloc((8*8)*NVMEIBC_SECTOR_SIZE, GFP_KERNEL);				// Array to read/write to disk. : 8 binje, 8 dlovks in slice. Total max 64 blocks IO

	// Backup orig values and set binje 8
	const u32 backup_split = dev->dp.alignment_sectors.write;
	const u32 backup_binje = dev->dp.p.binje;
	dev->dp.alignment_sectors.write = ((dev->dp.p.slice_size * 8) << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	BUG_ON(dev->dp.p.binje != nvmeibc_jentry_num_blocks);
	dev->dp.p.binje = nvmeibc_jentry_num_blocks = 8;
	NVMeshSystem_all_clients_dbg_di(B->sys, true);

	// Select the wanted topology via switch topo
	for (i = 0; i < env.sraid.tpr->header.n_segments; i++)
		seg_stats[i] = NVMEIBTC_DS_MODE_RW;
	seg_stats[2] = NVMEIBTC_DS_MODE_DEAD;
	//seg_stats[8] = NVMEIBTC_DS_MODE_W;
	tomaSimulator_switchTopoEC(env.sraid.tpr->header.uuid, seg_stats, SW_TOPO__WAIT_ACK_DR, NULL);
	__binje8_write_read_io(&env, 0, 8, mem);

	// ------------ Cleanup
	sim_kfree(mem);
	 // Restore topology
	for (i = 0; i < env.sraid.tpr->header.n_segments; i++)
		seg_stats[i] = NVMEIBTC_DS_MODE_RW;
	tomaSimulator_switchTopoEC(env.sraid.tpr->header.uuid, seg_stats, SW_TOPO__WAIT_ACK_DR, NULL);

	// Restore orig values
	dev->dp.alignment_sectors.write = backup_split;
	dev->dp.p.binje = nvmeibc_jentry_num_blocks = backup_binje;
	NVMeshSystem_all_clients_dbg_di(B->sys, false);
	NVMeshSystem_wipe_all_dirty_bits(B->sys);			// Coz IO in last possible degraded topology left dbits
	NVMeshSystem_volume_wipe_MD(B->sys, 0); 			// Make sure to clean md
	BUG_ON(!NVMeshSystem_is_stable(B->sys));			// System must be in a stable state
	return 0;
}

TEST_FUNC int unitest_ec_view_lock(bunitest_s* B) {
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator *client = &sys->clients[0];
	struct ramDiskSimulator* ssd = NULL;
	const int vi = 0;
	const struct disk_range* pr = &sys->mdb.vols[vi].segs[0], *ow = &pr[1]; // Owner will be on first seg
	const u64 vlba_second_lock = (pr->slice_size << LOCKSET_SHIFT);
	const int mem_size = 9*pr->slice_size*NVMEIBC_SECTOR_SIZE;
	const int lock_wipe_size = (int)sizeof(sys->servers[0].ramDisk.locks);	// Lock all locks
	const int vlba[] = {0 /*D0*/, 1 /*D1*/, 6 /*D6D7*/, 7 /*D7D0*/, 7 /*D7+Full+silce*/};
	const int vlen[] = {1       , 1       , 2         , 2         , 9};
	u32 i;
	u8 *mem = sim_kzalloc(mem_size, GFP_KERNEL);		// Array to read/write to disk
	bunitest_tic(B);

	/* All locks except the correct one are locked. Read can finish only if it views the correct lock */
	for (i = 0; i < pr->replicas; i++)
		memset(sys->servers[i].ramDisk.locks, SIMULATOR_OTHER_CLIENT_LOCK_ID, lock_wipe_size);
	ssd = &sys->servers[ow->node_id].ramDisk;
	ssd->locks[COMMITTED_ADDR_AS(ssd, ow->dlba_start, 4KB, LOCK) + 1] = 0;
	for (i = 0; i < ARRAY_SIZE(vlba); i++) {
		BUG_ON(osSimulator_readArrWait(&client->OS, vi, vlba_second_lock + vlba[i], vlen[i], mem));
	}
	for (i = 0; i < pr->replicas; i++)
		memset(sys->servers[i].ramDisk.locks, 0, lock_wipe_size);		// Unset the locks
	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	sim_kfree(mem);
	return 0;
}

/******************************* Async Tests **********************************/
static void __drain_all_serjio_jam_communication(struct NVMeshSystem *sys, const int jri, const struct disk_range *seg) {
	u32 i;
	for (i = 0; i < seg->replicas; i++) {		// Why All? Todo, change only to TxBM
		struct serverSimulator *srv = &sys->servers[i];
		serverSimulator_serjio_drain_wq(srv);
		nvmeibs_nordda_jour_entry_do(srv, jri, -1, "Drain Communication");
	}
}

static const char *get_async_ec_io_thread_name(enum nvmeib_block_io_op op) {
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_WRITE:	return "ut:EC W, 1[s]";	// Single slice
	case NVMEIB_BLOCK_IO_OP_READ:	return "ut:EC R, 1[s]";
	default:						return NULL;
	}
}

int __thread_gen_async_ec_single_slice_io(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct clientSimulator *client = &p->sys->clients[0];
	int rv, round, v = 0;
	const int n_slices = clientSimulator_sizeof_bdev(client, v) / p->nlbas;	// num slices in volume
	u8 *mem = sim_kzalloc(p->nlbas * NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
	for (round=0; !kthread_should_stop(); round++) {
		const int seed = (p->flags & BUNITEST_ASYNC_TEST_RAND_IO_PATTERN) ? rand() : round;
		const int rand_slice = (seed % n_slices) * p->nlbas;
		const int len   = (round % p->nlbas) + 1;						// [1..8] blocks
		const int start = rand_slice + seed % (p->nlbas - len + 1);
		NVMeshSystem_async_io_gate_wait(p->sys);
		if (p->op == NVMEIB_BLOCK_IO_OP_READ){
			rv = osSimulator_readArrWait( &client->OS, v, start, len, mem);
		} else { // To ensure that the data remains stable throughout it's parity/edic cycle we must generate it for each
			__unitest_fill_blocks_unique_pattern_and_lba(mem, start, len);
			rv = osSimulator_writeArrWait(&client->OS, v, start, len, mem);
		}
		BUG_ON(rv != 0);
	}
	sim_kfree(mem);
	p->n_cycles = round;
	return 0;
}

static void create_async_ec_io_thread(t_async_test_params *prm, enum nvmeib_block_io_op op) {
	prm->op = op;
	prm->kthread = kthread_run(__thread_gen_async_ec_single_slice_io, prm, get_async_ec_io_thread_name(op));
	BUG_ON(!prm->kthread);
}

// A stale copy might still be there (disk paused of after owner was released but secondary is stale)
static int __clean_stale_copies(struct NVMeshSystem *sys, const u32 n_segs, struct disk_range *seg) {
	int count = 0;
	// Each segment has locksets between seg->dlba_start -> seg->dlba_start+length (The segment takes ~ 1/2 Disk size)
	const int seg_blocks = seg->length/LOCKSET_4KS;
	for (u32 si = 0; si < n_segs; si++) {
		struct serverSimulator *srvr = &sys->servers[seg[si].node_id];
		const int li = (si >= seg->slice_size) ? 0 : (si+1)*2;
		for (int k = 0; k + li < seg_blocks; k += 2*seg->replicas) { // Each secondary spans 4 locksets (first as Q then P) and repeats replicas*ownershift
			for (int j = 0; j < 4 && (k+li+j) < seg_blocks; j++) {
				const int offset = (j + k + li) * LOCKSET_4KS;
				if (offset >= seg_blocks*LOCKSET_4KS) { // tmp verification
					BUG();
				} else if (tomaSimulator_has_stale_lock(&srvr->simToma, seg[si].dlba_start + offset)) {
					ramDiskSimulator_lockUnSta(         &srvr->ramDisk, seg[si].dlba_start + offset);
					_NT(clean_stale_copies_t, "Stale found in node @INT (seg @INT) lock @INT", seg[si].node_id, si, j + k + li);
					count++;
				}
			}
		}
	}
	return count;
}

static void __async_ec_single_slice_io_cleanup(struct NVMeshSystem *sys) {
	struct tTopoOfPraid *r1 = tTopoOfVolume_getRaid1(&sys->tcf.vols[0], 0);
	struct disk_range *seg = &sys->mdb.vols[0].segs[0];
	const int n_segs = (int)seg->replicas;
	int i, count, recov_status;

	// Quiesce SERJIO-initiated JGC recoveries and prevent starting new ones for the cleanup duration
	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(sys, true);
	clientSimulator_wait_for_all_recoveries_done(&sys->clients[0]);
	tomaSimulator_waitProtoEnd(NULL);

	for (i = 0; i < n_segs; i++) { // Call stale lock recovery for each stale, and then trigger JGC (which will clean all problematic journals)
		//This test fails! Consider the followin scenario
		//1)We take a lock on slice
		//2)pause some disk
		//3)bio completes, but leaves a stale lock on a segment
		//4)the test cancels and drains the resubmittion queue
		//5)runs stale rebuild - in our case the high segment with lock copy will not be touched
		//6)assert
		// Solve via union nvmeib_blkset_problem_report *bi_inj which is set to all stales

		struct ramDiskSimulator *ssd = &sys->servers[seg[i].node_id].ramDisk;
		union nvmeib_blkset_problem_report bi_inj[RAMDISK_DATA_LOCK_SIZE] = {[0 ... RAMDISK_DATA_LOCK_SIZE-1] = (union nvmeib_blkset_problem_report){{.is_stale=true}}};
		ssd->c.bi_inj = bi_inj;
		do {
			BUG_ON(tomaSimulator_recoverThingStatus(r1, seg + i, RCVR_STALE_REBUILD, &recov_status) < 0);
		} while (recov_status);
		ssd->c.bi_inj = NULL;

		do {
			BUG_ON(tomaSimulator_recoverThingStatus(r1, seg + i, RCVR_EC_JOUR_GC, &recov_status) < 0);
		} while (recov_status);
	}

	clientSimulator_wait_for_all_recoveries_done(&sys->clients[0]);
	clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);
	tomaSimulator_waitProtoEnd(NULL);
	NVMeshSystem_serialize(sys);

	// Quick and Dirty cleanup of Stale Copies (only copies), owner stales cleared all secondaries
	count = __clean_stale_copies(sys, n_segs, seg);

	if (1) { // After JGC no abandoned entry are allowed
		unsigned entry;
		int rv = 0;
		const int jri = 1;

		__drain_all_serjio_jam_communication(sys, jri, seg);
		//Internally serjio is using work queue and I think caching, so draining the jam communication is not enough
		//We should wait untill all commands were applied
		for (i = 0; i < n_segs; i++) { // Brute force clear - will send only if ABANDONDED
			struct serverSimulator *S = &sys->servers[i];
			const ulong bmp = nvmeibs_nordda_get_abandjour_in_jri(S, jri);
			rv += nvmeibs_nordda_count_abandjour_in_jri(S, jri);
			for_each_set_bit(entry, &bmp, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE) {
				nvmeibs_nordda_jour_entry_do(S, jri, entry, "Free Entry");
			}
		}
		if (rv) {
			unitest_print("*************** Found  %3d Serjio Abandoned entries, that were not cleared by HTR + JGC\n", rv); rv = 0;
			BUG();
		} else {
			unitest_print("*************** All Serjio entries are free after HTR + JGC as expected\n");
			if (count) { // Verify it was found
				pr_emerg("*************** Found %d Stale copies that were not released\n", count);
			}
		}
		__drain_all_serjio_jam_communication(sys, jri, seg);
	}

	NVMeshSystem_serialize(sys);

	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(sys, false);
}

#define unitest_async_pause_cont_during_ec_io_regu_lock_mode(sys, err)		__async_pause_cont_during_ec_single_slice_io(sys, NULL, err)
#define unitest_async_pause_cont_during_ec_io_dual_lock_mode(sys) 			__async_pause_cont_during_ec_single_slice_io(sys, "Dual locks" , false)
#define unitest_async_pause_cont_during_ec_io_unsafe_lock_mode(sys)		__async_pause_cont_during_ec_single_slice_io(sys, "Unsafe", false)
static int __async_pause_cont_during_ec_single_slice_io(struct NVMeshSystem *sys, const char* lock_modes, const bool with_error){
	t_async_test_params p[32] = {{0}};
	const int n_threads = ARRAY_SIZE(p), n_blocks = sys->mdb.vols->segs->slice_size, disk_id = -sys->mdb.vols->segs->replicas;		// Read/Write 1-8 blocks, in a single slice
	const int n_io_threads = n_threads - 1;
	int v, pc_v = n_io_threads, n_total_ios = 0, n_total_pauses = 0;
	struct nvmeibc_disk_hooks disk_hooks;
	if (lock_modes) __set_different_lock_modes_of_vol(sys, 0, lock_modes);
	if (with_error) {
		struct nvmeibc_disk_hooks disk_hooks_temp = {.args.trerr = {true, true, true, 0, 0, (rand() % INJECT_TRANSPORT_ERROR_CYCLE_SIZE), true, 0, 0}, .inject_transport_error = inject_transport_error};
		disk_hooks = disk_hooks_temp;
		NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, &disk_hooks);
	}
	for (v = 0; v < n_io_threads; v++) {
		t_async_ec_test_params_init(p[v], sys, -1, n_blocks, BUNITEST_ASYNC_TEST_RAND_IO_PATTERN);
		create_async_ec_io_thread(&p[v], (v & 0x1) ? NVMEIB_BLOCK_IO_OP_WRITE : NVMEIB_BLOCK_IO_OP_READ);
	}
	t_async_ec_test_params_init(p[pc_v], sys, disk_id, 0, 0);
	p[pc_v].kthread = kthread_run(__thread_async_pause_cont, &p[pc_v], "ut:async pause");		BUG_ON(!p[pc_v].kthread);

	msleep(1500);												// Let the threads run together

	if (with_error) { // Clear error injection before drain - can cause abandoned entries in JAM
		NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
	}

	for (v = 0; v < n_threads; v++) if (p[v].kthread) kthread_stop(p[v].kthread);
	for (v = 0; v < n_io_threads; v++) n_total_ios += p[v].n_cycles;
	for (v = n_io_threads; v < n_threads; v++) n_total_pauses += p[v].n_cycles;

	if (lock_modes) __set_different_lock_modes_of_vol(sys, 0, "Normal");

	// Remaining problems (No dbits coz no degraded topology). Has stale locks and abandoned/unknown journal
	__async_ec_single_slice_io_cleanup(sys);

	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
	unitest_print("*************** Pauses with%s errors %d, IOs %d, %s\n", ((with_error) ? "   " : "out" ),
				  n_total_pauses, n_total_ios, (lock_modes?lock_modes:""));
	return 0;
}

static int __async_degraded_rebuild_during_ec_single_slice_io(struct NVMeshSystem *sys) {
	t_async_test_params p[32] = {{0}};
	const int n_threads = ARRAY_SIZE(p), n_blocks = sys->mdb.vols->segs->slice_size;		// Read/Write 1-8 blocks, in a single slice
	const int n_io_threads = n_threads - 1;
	int v, deg_v = n_io_threads, n_total_ios = 0, n_total_rebuilds = 0;

	for (v = 0; v < n_io_threads; v++) {
		t_async_ec_test_params_init(p[v], sys, -1, n_blocks, BUNITEST_ASYNC_TEST_RAND_IO_PATTERN);
		create_async_ec_io_thread(&p[v], (v & 0x1) ? NVMEIB_BLOCK_IO_OP_WRITE : NVMEIB_BLOCK_IO_OP_READ);
	}

	t_async_ec_test_params_init(p[deg_v], sys, -1, 0, 0);
	p[deg_v].kthread = kthread_run(__thread_async_degraded_ec_vols, &p[deg_v], "ut:deg_ec");		BUG_ON(!p[deg_v].kthread);

	msleep(1500);												// Let the threads run together
	for (v = 0; v < n_threads; v++) if (p[v].kthread) kthread_stop(p[v].kthread);
	for (v = 0; v < n_io_threads; v++) n_total_ios += p[v].n_cycles;
	for (v = n_io_threads; v < n_threads; v++) n_total_rebuilds += p[v].n_cycles;

	clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);

	// Remaining problems: stale locks and abandoned/unknown journal. Degraded rebuild thread had already recovered dbits.
	__async_ec_single_slice_io_cleanup(sys);

	BUG_ON(!NVMeshSystem_is_stable(sys));
	unitest_print("*************** Rebuilds %d, IOs %d\n", n_total_rebuilds, n_total_ios);
	return 0;
}

bool NVMeshSystem_is_all_jams_ent_free(struct NVMeshSystem *sys) {
	const struct disk_range *pr = sys->mdb.vols[0].segs;
	int i, n_segs = pr->replicas;
	for (i = 0; i < n_segs; ++i) {
		if (nvmeibc_jam_simu_has_abandoned(sys->servers[i].disk))
			return false;
	}
	return true;
}

void NVMeshSystem_check_no_abandoned(struct NVMeshSystem *sys) {
	BUG_ON(!(busy_wait_for(1000000000, 1000, NVMeshSystem_is_all_jams_ent_free(sys))));
}

void NVMeshSystem__verify_all_serjios_are_clean(struct NVMeshSystem *sys) {
	const struct disk_range *pr = sys->mdb.vols[0].segs;
	int i, n_segs = pr->replicas;
	for (i = 0; i < n_segs; i++)		// Implicit assumption volume is
		nvmeibs_nordda_verify_no_abandjour(&sys->servers[pr[i].node_id]);
}


TEST_FUNC int unitest_EC_single_slice_async_pause_disks(struct NVMeshSystem *sys, const bool with_error) {
	NVMeshSystem_all_clients_dbg_di(sys, true);
	NVMeshSystem_all_clients_ec_edic(sys, false);
	NVMeshSystem_di_tracking_enable(sys);
	unitest_async_pause_cont_during_ec_io_regu_lock_mode(sys, with_error);
	NVMeshSystem_di_tracking_disable(sys);
	NVMeshSystem_all_clients_ec_edic(sys, true);
	NVMeshSystem__verify_all_serjios_are_clean(sys);
	NVMeshSystem_precondition_all_disks_for_EC(sys);
	return 0;
}

TEST_FUNC int unitest_EC_async_degraded_mode_rebuild_during_single_slice_io(struct NVMeshSystem *sys) {
	NVMeshSystem_all_clients_dbg_di(sys, true);
	NVMeshSystem_all_clients_ec_edic(sys, false);
	NVMeshSystem_di_tracking_enable(sys);

	__async_degraded_rebuild_during_ec_single_slice_io(sys);

	NVMeshSystem_di_tracking_disable(sys);
	NVMeshSystem_all_clients_ec_edic(sys, true);
	NVMeshSystem__verify_all_serjios_are_clean(sys);
	NVMeshSystem_precondition_all_disks_for_EC(sys);
	return 0;
}

/************************ Permanent Read Fail EC Test ************************/
static short read_fail_error_codes[NUMBER_OF_ERROR_CODES] = { EPERM_READ_FAIL, EPERM_READ_FAIL_NO_RETRY, NVME_SC_DNR  };

static inline short get_random_error(void) { return read_fail_error_codes[rand()%NUMBER_OF_ERROR_CODES]; }

// Depending on the inject/degraded segments we might not hit the injected block
static void __set_injection_not_clear_based_on_segments(const struct io_traits io_traits, const int slice_size,
														const int degraded_seg_index, const int n_inject,
														int *segs, bool *injection_not_cleared)
{
	int i;
	int p_seg = io_traits.slices[0].role2sgmnt[slice_size];
	int q_seg = io_traits.slices[0].role2sgmnt[slice_size+1];
	if (p_seg == degraded_seg_index){
		BUG_ON(n_inject != 1);
		if (q_seg == segs[0]) { // Both parities are unreadable but read will succeed
			injection_not_cleared[0] = true;
		}
	} else if (q_seg == degraded_seg_index) {
		BUG_ON(n_inject != 1);
		if (p_seg == segs[0]) { // Both parities are unreadable but read will succeed
			injection_not_cleared[0] = true;
		}
	} else if (degraded_seg_index != -1) {
		BUG_ON(n_inject != 1);
		if (q_seg == segs[0]) {
			injection_not_cleared[0] = true;
		}
	} else {
		if (n_inject == 1) {
			if (q_seg == segs[0] || p_seg == segs[0]) { // No data is injected read succeeds
				injection_not_cleared[0] = true;
			}
		} else {
			BUG_ON(n_inject != 2);
			for (i = 0; i < n_inject; i++) {
				if (q_seg == segs[i]) { // Q will not be needed if only one more error exists
					if (p_seg == segs[i^1]) { // Both P and Q are injected all data is readable
						injection_not_cleared[i^1] = true;
						injection_not_cleared[i] = true;
					}
				}
			}
		}
	}
}

// Choose 3 random segs (or 2 additonal ones after degraded seg is set)
static void __randomize_segments(const int degraded_seg_index, const int replicas, int *segs)
{
	int random_seg = rand()%replicas;
	segs[2] = degraded_seg_index;
	if (segs[2] < 0) { segs[2] = random_seg; }
	while (random_seg == segs[2]) { random_seg = rand()%replicas; }
	segs[1] = random_seg;
	while (random_seg == segs[2] || random_seg == segs[1]) { random_seg = rand()%replicas; }
	segs[0] = random_seg;
}

/*
u32 __get_max_txid_in_slice(const struct io_traits io_traits, const struct test_context env) {
	int replicas = env.sraid.cpr->replicas;
	u32 max_txid = 0;
	struct block_inject_ptrs bptr;
	for (int i=0; i<replicas; ++i) {
		bptr =  serverSimulator_get_block_inject_ptrs(&env.sys->servers[i], io_traits.slices[0].sgmnt2dlba[i], 0, 0);
		max_txid = max(max_txid, (u32)bptr.dmd->tx_id);
	}

	return max_txid;
}
*/
#include "uni_scenario_mtv.h"
#define OUR_CLIENT_JRI (NVMEIB_EC_MAX_JOURNAL_RANGES - 2)		// 1 Simulated other client, 1 Second instance client
// Writes on expected read failure area, sets up failures for up to 3 segments, and expected outcome for each case
int unitest_PermanentReadError_EC_IO(const struct test_context env, const int degraded_seg_index, u8 *mem, const int is_dead) {
	const int volInd = env.sraid.vsi.volume;
	const struct disk_range *seg = env.sraid.cpr;
	const bool n_slices = 1;
	const int slice_size = seg->slice_size;		// Test on blockset {0,1} where d0 is on seg1, to differentiate between bmp relative to pr and to slice start
	const int memSize	= slice_size*NVMEIBC_SECTOR_SIZE*n_slices;	// Total array in bytes
	u64 magic_pattern;							// unique 64b signaturre filling the array
	int replicas = (int)seg->replicas;
	u64 slice_index = rand()%LOCKSET_SLICES;
	int rv, io_rv, sl, li, i, io_count = 0;
	int segs[3] = {-1,-1,-1};
	int n_max_injections = (degraded_seg_index < 0) ? 3 : 2;
	int n_locksets = seg->length / LOCKSET_SLICES;
	segs[2] = degraded_seg_index;
	for (li = 0; li < n_locksets; li++) {
		for (sl = 0; sl < LOCKSET_SLICES; sl++) {
			// Slice info
			const int tested_slice = (sl + slice_index)%LOCKSET_SLICES;
			const int tested_slice_dlba = tested_slice + li*LOCKSET_SLICES;
			const u64 vlba = (tested_slice_dlba * slice_size);
			struct io_traits io_traits = get_io_traits(env, vlba, slice_size*n_slices);	// QLC
			struct block_inject_ptrs owner_ram[N_MAX_RAID_SLICE_LEN];
			u32 txid;
			u32 degraded_txid = 0, degraded_jri = 0;
			int owner_seg = io_traits.slices[0].role2sgmnt[0];
			// Injection randomization
			__randomize_segments(degraded_seg_index, replicas, segs);

			if (owner_seg == degraded_seg_index) {	// Correct owner RAM (if D0 is_degraded)
				owner_seg = (owner_seg) ? owner_seg - 1 : replicas - 1;
			}
			owner_ram[0] = serverSimulator_get_block_inject_ptrs(&env.sys->servers[owner_seg], io_traits.slices[0].sgmnt2dlba[owner_seg], 0, 0, 0);
			if (degraded_seg_index != -1) { // Degraded segment will not be written to
				struct block_inject_ptrs deg =  serverSimulator_get_block_inject_ptrs(&env.sys->servers[degraded_seg_index], io_traits.slices[0].sgmnt2dlba[degraded_seg_index], 0, 0, 0);
				degraded_txid = deg.dmd->tx_id;
				degraded_jri = deg.dmd->jri;
			}
			for (int n_inject = 1; n_inject <= n_max_injections; n_inject++) { // Up to 3 readfail injections
				bool is_read_ok = (n_inject != n_max_injections);
				bool injection_not_cleared[2] = {false, false};
				if (is_read_ok) { // When injecting only parities or Q and Data, the injection must not trigger
					__set_injection_not_clear_based_on_segments(io_traits, slice_size, degraded_seg_index, n_inject, segs, injection_not_cleared);
				}
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, slice_size*n_slices);  // Set a pattern.
				rv = osSimulator_writeArrWait(&env.client->OS, volInd, vlba, slice_size*n_slices, mem);	REPORT_ERROR(rv);
				txid = *owner_ram->ram.txid; // ram_txid is equal to max_txid in slice so no need to calc max_txid in slice
				for (i = 0; i < n_inject; i++) { // Inject
					const u32 node_id = seg[segs[i]].node_id;
					ramDiskSimulator_do_bad_sector(&env.sys->servers[node_id].ramDisk, seg[segs[i]].dlba_start + tested_slice_dlba, get_random_error());
					BUG_ON(node_id != (u32)segs[i]);
				}
				if (!is_read_ok) { // Prepare TOMAs for report if read fails
					for (i = 0; i < replicas; i++) {
						const u32 node_id = seg[i].node_id;
						if (!is_dead || i != degraded_seg_index)
							tomaSimulator_expectIOFailure(&env.sys->servers[node_id].simToma, EPERM_READ_FAIL_NO_RETRY, (1u << tested_slice), 0);
					}
				}
				// Trigger readfail with read
				memset(mem, 0, memSize);
				rv = osSimulator_readArrWait(&env.client->OS, volInd, vlba, slice_size*n_slices, mem);	REPORT_ERROR(rv);
				io_count++;
				tomaSimulator_waitProtoEnd(NULL);

				io_rv = osSimulator_rv_of_last_io_get(&env.client->OS, volInd);
				if (likely(is_read_ok)) {   // Read IO should succeed verify data
					BUG_ON(io_rv != 0);
					__unitest_verify_blocks_pattern(mem, slice_size*n_slices, magic_pattern, false);
					for (i=0;i<n_inject;i++) { // Verify injected blocks after readfail
						const u32 node_id = seg[segs[i]].node_id;
						struct block_inject_ptrs rf_verify = serverSimulator_get_block_inject_ptrs(&env.sys->servers[node_id], io_traits.slices[0].sgmnt2dlba[segs[i]], 0, 0, 0);
						if (injection_not_cleared[i]) { // If not cleared we need to unset it
							ramDiskSimulator_un_bad_sector(&env.sys->servers[node_id].ramDisk, seg[segs[i]].dlba_start + tested_slice_dlba);
						} else { // When correcting a block the MD jri is MARK_NO_JOURNAL but the txid is updated correctly
							BUG_ON(rf_verify.dmd->jri != JRI_MARK_NO_JOURNAL);
							BUG_ON(rf_verify.dmd->tx_id != txid);
							ramDiskSimulator_verify_no_bad_sectors(&env.sys->servers[node_id].ramDisk);
						}
					}
				} else { // Verify data was not read
					BUG_ON(io_rv != EPERM_READ_FAIL_NO_RETRY);
					for (i=0;i<n_inject;i++) { // The injected segment MD is destroyed
						const u32 node_id = seg[segs[i]].node_id;
						const struct block_inject_ptrs rf_verify = serverSimulator_get_block_inject_ptrs(&env.sys->servers[node_id], io_traits.slices[0].sgmnt2dlba[segs[i]], 0, 0, 0);
						BUG_ON(rf_verify.dmd->tx_id != txid);
						BUG_ON(rf_verify.dmd->jri != JRI_MARK_INVALID_FOR_READ);
					}
				}
				if (degraded_seg_index != -1) { // Degraded segment
					const u32 node_id = seg[degraded_seg_index].node_id;
					const struct block_inject_ptrs rf_verify = serverSimulator_get_block_inject_ptrs(&env.sys->servers[node_id], io_traits.slices[0].sgmnt2dlba[degraded_seg_index], 0, 0, 0);
						if (is_dead) { // Dead - is not affected verify no change
							BUG_ON(rf_verify.dmd->tx_id != degraded_txid);
							BUG_ON(rf_verify.dmd->jri != degraded_jri);
							ramDiskSimulator_verify_no_bad_sectors(&env.sys->servers[degraded_seg_index].ramDisk);
						} else {
							if (is_read_ok) {	// W segment is not written to when fixing readfail (was fixed in DB sync before write)
								BUG_ON(rf_verify.dmd->tx_id != txid);
								BUG_ON(rf_verify.dmd->jri != OUR_CLIENT_JRI);
								ramDiskSimulator_verify_no_bad_sectors(&env.sys->servers[degraded_seg_index].ramDisk);
							} else { // Read failed but this seg is write and therefore untouched
								BUG_ON(rf_verify.dmd->tx_id != txid);
								BUG_ON(rf_verify.dmd->jri != OUR_CLIENT_JRI);
							}
						}
				}
				if (n_inject == n_max_injections) { // Overwrite the uncleaned injections by force TODO add recovery verification (should volume become RO, etc')
					rv = osSimulator_writeArrWait(&env.client->OS, volInd, vlba, slice_size*n_slices, mem);	REPORT_ERROR(rv);
				}
			} // n_inject
			free_io_traits(&io_traits);
		} // sl
	} //li
	return io_count;
}

void unitest_PermanentReadError_EC_Maintenance(const struct test_context env, const int degraded_seg_index, u8 *mem, const int is_dead) {
	const int volInd = env.sraid.vsi.volume;
	struct tTopoOfPraid  *r1 = env.sraid.tpr;
	const struct disk_range *seg = env.sraid.cpr;
	const bool n_slices = 1;
	const int slice_size = seg->slice_size;
	int replicas = (int)seg->replicas;
	int rv, li, i;
	int segs[3] = { -1, -1, -1 };
	int n_max_injections = (degraded_seg_index < 0) ? 3 : 2;
	int n_locksets = seg->length / LOCKSET_SLICES;

	(void)is_dead;
	segs[2] = degraded_seg_index;
	for (li = 0; li < n_locksets; li++) {
		// Slice info
		const int tested_slice = rand() % LOCKSET_SLICES;
		const int tested_slice_dlba = tested_slice + li * LOCKSET_SLICES;
		const u64 vlba = (tested_slice_dlba * slice_size);
		struct io_traits io_traits = get_io_traits(env, vlba, slice_size * n_slices);
		struct block_inject_ptrs owner_ram[N_MAX_RAID_SLICE_LEN] = { 0 };
		int owner_seg = io_traits.slices[0].role2sgmnt[0];
		int actual_owner_seg;
		int copy_i;

		// Injection randomization
		__randomize_segments(degraded_seg_index, replicas, segs);

		actual_owner_seg = -1;
		for (copy_i = 0; copy_i < 3; copy_i++) {
			int cur_owner_seg = (owner_seg + replicas - copy_i) % replicas;
			if (cur_owner_seg == degraded_seg_index)
				continue;
			if (actual_owner_seg == -1)
				actual_owner_seg = cur_owner_seg;
			owner_ram[cur_owner_seg] = serverSimulator_get_block_inject_ptrs(&env.sys->servers[cur_owner_seg], io_traits.slices[0].sgmnt2dlba[cur_owner_seg], 0, 0, 0);
		}

		__unitest_fill_blocks_unique_pattern(mem, slice_size*n_slices);
		rv = osSimulator_writeArrWait(&env.client->OS, volInd, vlba, slice_size * n_slices, mem);	REPORT_ERROR(rv);

		for (int n_inject = 1; n_inject <= n_max_injections; n_inject++) { // Up to 3 readfail injections
			for (i = 0; i < n_inject; i++) { // Inject
				const u32 node_id = seg[segs[i]].node_id;
				ramDiskSimulator_do_bad_sector(&env.sys->servers[node_id].ramDisk, seg[segs[i]].dlba_start + tested_slice_dlba, get_random_error());
				BUG_ON(node_id != (u32)segs[i]);
			}

			// Inject unresolved binfo
			for (copy_i = 0; copy_i < 3; copy_i++) {
				int cur_owner_seg = (owner_seg + replicas - copy_i) % replicas;
				if (cur_owner_seg == degraded_seg_index)
					continue;
				owner_ram[cur_owner_seg].ram.dbits->all_bits = nvmeib_dbits_entry_build_unk(-1, -1).all_bits;
				*owner_ram[cur_owner_seg].ram.txid = INITIAL_LAZY_READ_TXID;
			}

			// Run maintenance sync
			if (1) {
				struct sim_recovery_hooks hooks = sim_recovery_hooks_create(0, 0, 1 /* follow_x_cleanups */, 0); //don't sleep, wait for recovery cleanup
				int recov_status;
				sim_recovery_setup_hooks(&hooks);
				BUG_ON(tomaSimulator_recoverThingStatus(r1, &seg[actual_owner_seg], RCVR_EC_FIX_UNK_BINFO, &recov_status) < 0);
				nvmeibc_multi_completion_wait_for(&hooks.cleaned);
				BUG_ON(recov_status < 0);
				sim_recovery_clean_hooks();
			}

			// Clean up

			// Verify bad sectors were not resolved and clear
			for (i = 0; i < n_inject; i++) {
				const u32 node_id = seg[segs[i]].node_id;
				ramDiskSimulator_un_bad_sector(&env.sys->servers[node_id].ramDisk, seg[segs[i]].dlba_start + tested_slice_dlba);
			}

			// Verify dbits were resolved to worst case by maintenance sync and clear if needed
			for (copy_i = 0; copy_i < 3; copy_i++) {
				int cur_owner_seg = (owner_seg + replicas - copy_i) % replicas;
				if (cur_owner_seg == degraded_seg_index)
					continue;
				if (owner_ram[cur_owner_seg].ram.dbits->all_bits != 0) {
					BUG_ON(degraded_seg_index == -1);
					owner_ram[cur_owner_seg].ram.dbits->all_bits = 0;
				} else {
					BUG_ON(degraded_seg_index != -1);
				}
			}

			if (n_inject == n_max_injections) {
				rv = osSimulator_writeArrWait(&env.client->OS, volInd, vlba, slice_size * n_slices, mem);	REPORT_ERROR(rv);
			}
		} // n_inject
		free_io_traits(&io_traits);
	} //li
}

/* Base test for checking behaviour of Read failures in EC  */
TEST_FUNC int unitest_PermanentReadError_EC(bunitest_s *B) {					// On EC volumes permanent failure should succeed to restore data if up to n_parities are down/non-readable
	struct test_context env = { .sys = B->sys
								, .client = B->sys->clients
								, .dev = B->sys->clients->devs[0]
								, .sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, (struct volume_segment_index){0,0,0,0})};
	u64 io_count = 0, total_count = 0;
	const int slice_size = env.sraid.cpr->slice_size;
	const int memSize = slice_size * NVMEIBC_SECTOR_SIZE;		// Total array in bytes full slice
	const u32 rand_seed = (u32)(jiffies);					// Store it to be able to reproduce random sequence
	int degraded_seg = 0, i;
	u8  *mem;
	unsigned long timer;
	bunitest_tic(B);
	NVMeshSystem_all_clients_dbg_di(env.sys, true);
	mem = sim_kmalloc(memSize, GFP_KERNEL);	// Aligned Array to read/write to disk
	srand(rand_seed);
	// To ensure we test at least one degraded data segment and one of each partiy we can use the first lockset to force it
	for (i = 0; i < 4; i++) {
		timer = jiffies;
		// i == 0 degraded_seg == 0 Owner
		if (i==1) { // Random data seg
			while (!degraded_seg) {
				degraded_seg = rand()%slice_size;
			}
		} else if (i == 2) { // P
			degraded_seg = slice_size;
		} else if (i == 3) { // Q
			degraded_seg++;
		}
		unitest_print("PermanentReadFail Test Degraded Segment is %d, seed = %x\n", degraded_seg, rand_seed);
		__degrade_segment(env.client, env.sraid.tpr, env.sraid.cpr, degraded_seg);

		// Test IO with degraded seg + 1-2 RF sectors per slice
		io_count += unitest_PermanentReadError_EC_IO(env, degraded_seg, mem, true);
		unitest_PermanentReadError_EC_Maintenance(env, degraded_seg, mem, true);

		__restore_seg_to_write(env.sys, env.client, env.sraid.tpr, env.sraid.cpr, degraded_seg, false);

		// Test IO with write only seg + 1-2 RF sectors per slice
		io_count += unitest_PermanentReadError_EC_IO(env, degraded_seg, mem, false);
		unitest_PermanentReadError_EC_Maintenance(env, degraded_seg, mem, false);

		__restore_seg_to_read_write(env.sys, env.sraid.tpr, degraded_seg);	// Todo: Use tomaSimulator_switchSegmentTopo() instead
		unitest_print("PermanentReadFail Test Degraded Segment is %u - seed=%d sent %llu Readfailed IOs result => PASS took %d[mSec]\n", degraded_seg, rand_seed, io_count, (int)(((jiffies-timer)*1000)/HZ));
		total_count += io_count;
		io_count = 0;
	}
	// Test IO with perfect topo, 1-3 RF sectors per slice
	timer = jiffies;
	unitest_print("PermanentReadFail Test No Degraded Segments\n");
	io_count += unitest_PermanentReadError_EC_IO(env, -1, mem, false);
	unitest_PermanentReadError_EC_Maintenance(env, -1, mem, false);
	unitest_print("PermanentReadFail Test No Degraded Segments - sent %llu Readfailed IOs result => PASS took %d[mSec]\n", io_count, (int)(((jiffies-timer)*1000)/HZ));
	BUG_ON(!NVMeshSystem_is_stable(env.sys));	// Extremely rarelly reports unreal bug when prev topo did not have time to delete.
	NVMeshSystem_all_clients_dbg_di(env.sys, false);
	sim_kfree(mem);
	unitest_print("*************** EC Permanent read fail (8+2) after %d[mSec] randseed=0x%x sent %llu IOs\n", bunitest_toc(B), rand_seed, total_count);
	return 0;
}

/* Verify dirty bits recovery also copies blockset info when there is no dbit */
static void __EC_sync_commit_blockset(bunitest_s* B) {
	struct NVMeshSystem *sys = B->sys;
	int i, j, volIndx = 0, deg_i = 0;
	struct disk_range    *pr = &sys->mdb.vols[volIndx].segs[0];
	struct tTopoOfPraid  *r1 = tTopoOfVolume_getRaid1(&sys->tcf.vols[volIndx], 0);
	char praidUUID[40];

// Description for each blockset how blockset info is reconstructed (From Primary owner to both copies)
//                   RW         RW      D/W      RW
//         Segs      S8         S9       S0      S1
// Blocksets|
// ---------+---------------------------------------
//    0     |        C1        Primary   C0
//    1     |        C1        Primary   C0
//    2     |                   C2       C1      Primary
//    3     |                   C2       C1      Primary
	struct ramDiskSimulator* ssds[4];									// 4 disks/segs participating in the party (8,9,0,1). Woo hoo!
	u64                      slba_rel[4];									// Corresponding Offset of segment on disk in units of locks
	u64                      slba_abs[4];									// Corresponding Offset of segment on disk in units of locks
	struct tomaSimulator    *T;
	enum NVMEIBTC_DS_MODE topo[N_MAX_RAID_SLICE_LEN] = {[0 ... 9] = NVMEIBTC_DS_MODE_RW};
	const int n_seq_locks = (1<<LOCKSET_SHIFT)/LOCKSET_4KS;

	disk_range_format_praid_uuid(pr, praidUUID);

	if (1) {															// Move seg 'deg_i' to 'W' and clean TxID's
		topo[deg_i] = NVMEIBTC_DS_MODE_W;
		tomaSimulator_switchTopoEC(praidUUID, topo, SW_TOPO__WAIT_ACK_DR, NULL);
		for (i = 0; i < 4; i++) {
			const int di = (pr->replicas + (deg_i-2) +i ) % pr->replicas;
			ssds[i] = &sys->servers[di].ramDisk;
			slba_abs[i] = (pr[di].dlba_start/LOCKSET_4KS);
			slba_rel[i] = COMMITTED_ADDR(ssds[i], slba_abs[i], LOCK);
			ramDiskSimulator_reset_txid(ssds[i]);
		}
	}
	for (i = 0; i < n_seq_locks; i++) {						// Inject problems To blocksets 0,1: blockset info descrepancies
		ssds[2]->TxIDs[slba_rel[2] + i] = INITIAL_LAZY_READ_TXID;// S0 - TxID was lost
		ssds[1]->TxIDs[slba_rel[1] + i] = 0xaba0+i;				// S9 Owner lock of dead seg s0 is on s9. This value will be committed
		ssds[0]->TxIDs[slba_rel[0] + i] = 2;					// S8 - TxID of s1 should be OK but inject some value which will be overwritten
	}
	for (i = n_seq_locks; i < 2*n_seq_locks; i++) {			// Inject problems To blocksets 0,1: blockset info descrepancies
		ssds[3]->TxIDs[slba_rel[3] + i] = 0xbba0+i;				// S1 - Owner lock of dead seg s0 is on s1. This value will be committed
		ssds[2]->TxIDs[slba_rel[2] + i] = INITIAL_LAZY_READ_TXID;// S0 - TxID was lost
		ssds[1]->TxIDs[slba_rel[1] + i] = 2;					// S9 - TxID of s9 should be OK but inject some value which will be overwritten
	}

	// Launch recovery on seg s1, and verify that only blocksets 2,3 (where it is owner) were fixed
	BUG_ON(tomaSimulator_recoverThing(r1, &pr[ssds[3]->uniqueID], RCVR_DIRTY_REBUILD) < 0);
	for (i = 0; i < n_seq_locks; i++)
		BUG_ON(ssds[2]->TxIDs[slba_rel[2] + i] != INITIAL_LAZY_READ_TXID);			// Verify Blocksets 0,1 were not fixed in 'W' segment
	for (i = n_seq_locks; i < 2*n_seq_locks; i++) {			// Verify Blocksets 2,3 were fixed (on disks ssds[3,2,1])
		for (j = 1; j < 4; j++)
			BUG_ON(ssds[j]->TxIDs[slba_rel[j] + i] != (u32)(0xbba0+i));
	}

	// Launch recovery on seg s9, and verify that its blockset were cleaned
	BUG_ON(tomaSimulator_recoverThing(r1, &pr[ssds[1]->uniqueID], RCVR_DIRTY_REBUILD) < 0);
	for (i = 0; i < n_seq_locks; i++) {						// Verify Blocksets 0,1 were fixed (on disks ssds[2,1,0])
		for (j = 0; j < 3; j++)
			BUG_ON(ssds[j]->TxIDs[slba_rel[j] + i] != (u32)(0xaba0+i));
	}

	if (1) { // ----------------------- Verify that Dbits recovery, handles well stale locks
		union nvmeib_lock_blkset_entry lid = {.all = 0ULL };
		union nvmeib_blkset_problem_report bi_inj[RAMDISK_DATA_LOCK_SIZE];
		memset(bi_inj, 0, sizeof(bi_inj));
		lid.lock_id.bits.lock_id =  SIMULATOR_OTHER_CLIENT_LOCK_ID;
		lid.lock_id.bits.is_stale = 1;
		ssds[1]->locks[slba_rel[1]] = lid.lock_id.all;
		for (i = 0; i < n_seq_locks; i++) {						// Blockset 0 has stale lock and uncommited binfo.
			ssds[1]->TxIDs[slba_rel[1] + i] = (u32)(0xcba0+i);		// Blockset 1 has only           uncommited binfo (as previous tests)
			ssds[1]->dbits[slba_rel[1] + i].all_bits = 0;	// No dirty bits
		}
		BUG_ON(tomaSimulator_recoverThing(r1, &pr[ssds[1]->uniqueID], RCVR_DIRTY_REBUILD) < 0);
		// Verify Blocksets 0,1 were fixed (on disks ssds[2,1,0]), blockset info was commited by either stale lock sync or binfo_commit sync
		for (i = 0; i < n_seq_locks; i++) {
			for (j = 0; j < 3; j++)
				BUG_ON(ssds[j]->TxIDs[slba_rel[j] + i] != (u32)(0xcba0+i));
		}
		BUG_ON(ssds[1]->locks[slba_rel[1]] != 0);					// Stale locks recovery cleaned this lock

		// ----------------------- Dbits recovery, encounters unexpected stale locks (verify mutations to stale. when needed)
		for (i = 0; i < n_seq_locks; i++)
			ssds[1]->locks[slba_rel[1] + i] = lid.lock_id.all;		// RAM has 2 stale locks, binfo query returns {dbit,binfo_commit} wihtout stales
		bi_inj[slba_rel[1] + 0].dbits = 0x1;						// 1st blockset: Dbits sync mutates to stale lock syncs and solves the staleness
		bi_inj[slba_rel[1] + 1].binfo_not_commited = 0x1;			// 2nd blockset: binfo_commit sync mutate to commit binfo but release lock back to stale
		ssds[1]->c.bi_inj = bi_inj;
		BUG_ON(tomaSimulator_recoverThing(r1, &pr[ssds[1]->uniqueID], RCVR_DIRTY_REBUILD) < 0);
		for (i = 0; i < n_seq_locks; i++) {
			if (i == 1) {	// had stale
				// binfo_commit sync did not mutate to stale locks and released locks to their prior values
				for (j = 0; j < 3; j++) {
					if (j == 1)
						ramDiskSimulator_lockUnSta(ssds[j], (slba_abs[j] + i)*LOCKSET_4KS);
					else
						BUG_ON(ssds[j]->locks[slba_rel[j] + i] != 0);
				}
			}
			else {	// i == 0, in particular (dbits sync)
				// Dbits mutated to stale lock sync and solved the problem
				BUG_ON(ssds[j]->locks[slba_rel[j] + i] != 0);
			}
		}

		// ----------------------- Dbits recovery, encounters unexpected stale locks in a copy, but not in owner
		T = serverSimulator_get_toma_by_ram(ssds[0]);
		for (i = 0; i < n_seq_locks; i++) {
			ssds[0]->locks[slba_rel[0] + i] =     lid.lock_id.all;		// RAM has 2 copies of stale locks, binfo query returns {dbit,binfo_commit} wihtout stales
			T->stale_locks[slba_rel[0] + i].all = lid.lock_id.all;
		}
		bi_inj[slba_rel[1] + 0].dbits = 0x1;						// 1st blockset: Dbits sync        ignores stale lock and unlocks to 0, sends         blkset recovered msg
		bi_inj[slba_rel[1] + 1].binfo_not_commited = 0x1;			// 2nd blockset: binfo_commit sync releases lock back to stale, 		does not send blkset recovered msg coz dont have cmds in sync
		BUG_ON(tomaSimulator_recoverThing(r1, &pr[ssds[1]->uniqueID], RCVR_DIRTY_REBUILD) < 0);
		for (i = 0; i < n_seq_locks; i++) {
			if (i == 1) {
				// binfo_commit sync did not mutate to stale locks and released locks to their prior values
				for (j = 0; j < 3; j++) {
					if (j == 0) {	// had stale
						BUG_ON(T->stale_locks[slba_rel[j] + i].all != lid.lock_id.all);	// Blkset recovered msg not sent
						ramDiskSimulator_lockUnSta(ssds[j], (slba_abs[j] + i)*LOCKSET_4KS);
					} else {
						BUG_ON(ssds[j]->locks[slba_rel[j] + i] != 0);		// ramDiskSimulator_is_locked()
					}
				}
			}
			else {	// i == 0, in particular (dbits sync)
				// Dbits saw stale lock and solved the problem
				BUG_ON(ssds[j]->locks[slba_rel[j] + i] != 0);
				BUG_ON(T->stale_locks[slba_rel[j] + i].all != 0);	// Blkset recovered msg sent
			}
		}

		// ----------------------- Dbits recovery, encounters solved stale locks (verify mutations from stale. when needed). As if other client solved some stales before recovery reached those blocksets
		for (i = 0; i < n_seq_locks; i++) {
			ssds[1]->locks[slba_rel[1] + i] = 0;					// RAM has no stale locks, binfo query returns stales
			bi_inj[        slba_rel[1] + i].dbits    = i;			// First blockset has dbits, second does not
			bi_inj[        slba_rel[1] + i].is_stale = 0x1;			// Both blocksets have stale lock
		}
		BUG_ON(tomaSimulator_recoverThing(r1, &pr[ssds[1]->uniqueID], RCVR_DIRTY_REBUILD) < 0);
		for (j = 0; j < 3; j++)
			for (i = 0; i < n_seq_locks; i++)
				BUG_ON(ssds[j]->locks[slba_rel[j] + 1] != 0);     	// Verify No stale locks suddenly appeared

		// ----------------------- Dbits recovery, decides to commit a broken binfo
		memset(bi_inj, 0, sizeof(bi_inj));						// Server reports as if there is no problem with any blockset, but
		ssds[1]->dbits[slba_rel[1] + 0].all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;	// Blockset 0 has dirty suspect
		ssds[1]->TxIDs[slba_rel[1] + 1]          = INITIAL_LAZY_READ_TXID;	// Blockset 1 has unresolved TxID
		BUG_ON(tomaSimulator_recoverThing(r1, &pr[ssds[1]->uniqueID], RCVR_DIRTY_REBUILD) < 0);
		for (j = 0; j < 3; j++) {
			BUG_ON(ssds[j]->dbits[slba_rel[j] + 0].all_bits == nvmeib_dbits_entry_build_unk(-1,-1).all_bits);     	// Verify dbits resolved.
			BUG_ON(ssds[j]->TxIDs[slba_rel[j] + 0] == INITIAL_LAZY_READ_TXID);     	                   // Verify TXID resloved.
			ssds[j]->dbits[slba_rel[j] + 0].all_bits = 0;
		}
		ssds[1]->c.bi_inj = NULL;
	}

	topo[deg_i] = NVMEIBTC_DS_MODE_RW;
	tomaSimulator_switchTopoEC(praidUUID, topo, SW_TOPO__WAIT_ACK_DR, NULL);
	NVMeshSystem_serialize(sys);
}

static void __verify_no_remaining_locks_ec(struct NVMeshSystem *sys, int volIndx) {
	struct disk_range *pr = &sys->mdb.vols[volIndx].segs[0];
	int i;
	for (i = 0; i < (int)pr->replicas; i++) {// Verify stale lock was indeed cleared
		ramDiskSimulator_verify_no_locks(&sys->servers[pr[i].node_id].ramDisk);
		tomaSimulator_verify_no_locks(&sys->servers[pr[i].node_id].simToma);
	}
}

static void __verify_no_dirty_bits(struct NVMeshSystem *sys, struct TstPRaid praid){
	for (u8 si = 0; si < praid.cpr->replicas; ++si){
		ramDiskSimulator_verify_no_dirty_bits(&sys->servers[praid.cpr[si].node_id].ramDisk);
	}
}

#define n_segs_in_chunk() (seg->replicas * seg->stripe_width)
void __fill_block_device_with_unique_data(struct NVMeshSystem *sys, u8 v)
{
	const int _4ksize = 4096;
	const u64 blks_in4k = _4ksize / NVMEIBC_SECTOR_SIZE;
	struct clientSimulator *client = &sys->clients[0];
	struct nvmeibc_block_device *dev = client->devs[v];
	const int n_4ks_in_bdev = (dev->size * NVMEIBC_SECTOR_SIZE)/_4ksize;
	int c, s, i;
	const bool saved_enable_edic_check = dev->dp.enable_edic_check;
	u8 *mem = sim_kmalloc(n_4ks_in_bdev*_4ksize, GFP_KERNEL);
	const int snake_size = dev->dp.p.snake_size;
	BUG_ON(snake_size != 1);
	dev->dp.enable_edic_check = false;

	{//rewrite whole data
		struct disk_range *seg = &sys->mdb.vols[v].segs[0];
		for (i = 0; i < n_4ks_in_bdev; ++i) { // Fill unique data for each block then write
			__unitest_fill_blocks_unique_pattern(&mem[i*_4ksize], 1);
		}
		for (c = 0; c < sys->tcf.vols[v].nChunks; c++, seg += n_segs_in_chunk()) { // split by chunks to update bio split values
			const int chunk_start = seg->bd_start;
			const int chunk_end = ((c == sys->tcf.vols[v].nChunks - 1) ? dev->size : seg[n_segs_in_chunk()].bd_start);
			const int chunk_size = chunk_end - chunk_start;
			const int data_snake_size = seg->slice_size*snake_size;
			const int prev_slice_size_value = set_split_bio_to_new_value(dev, data_snake_size);
			for (s = 0; s < chunk_size / data_snake_size; s++) {// Split writes into full snakes
				BUG_ON(chunk_start + (s+1) * data_snake_size > n_4ks_in_bdev);
				BUG_ON(osSimulator_writeArrWait(&client->OS, v, (chunk_start + s * data_snake_size)*blks_in4k, data_snake_size*blks_in4k, &mem[_4ksize * (chunk_start + s * data_snake_size)]) < 0);
			}
			set_split_bio_to_new_value(dev, prev_slice_size_value);
		}
	}
	{//read whole data
		struct disk_range *seg = &sys->mdb.vols[v].segs[0];
		memset(mem, 0x0, n_4ks_in_bdev*_4ksize);
		for (c = 0; c < sys->tcf.vols[v].nChunks; c++, seg += n_segs_in_chunk()) {
			const int chunk_start = seg->bd_start;
			const int chunk_end = ((c == sys->tcf.vols[v].nChunks - 1) ? dev->size : seg[n_segs_in_chunk()].bd_start);
			const int chunk_size = chunk_end - chunk_start;
			const int data_snake_size = seg->slice_size*snake_size;
			const int prev_slice_size_value = set_split_bio_to_new_value(dev, data_snake_size);
			// Read entire chunk at once
			BUG_ON(osSimulator_readArrWait(&client->OS, v, chunk_start*blks_in4k, chunk_size*blks_in4k, mem) < 0);
			for (s = 0; s < chunk_size; s++) { // Read each block on it's own
				BUG_ON(osSimulator_readArrWait(&client->OS, v, chunk_start*blks_in4k + s, 1, mem) < 0);
			}
			set_split_bio_to_new_value(dev, prev_slice_size_value);
		}
		sim_kfree(mem);
	}
	dev->dp.enable_edic_check = saved_enable_edic_check;
}


void __verify_dbits_identical(union nvmeibc_dbits_entry dbits, union nvmeibc_dbits_entry expected, int num_parities)
{
	if (dbits.all_bits != expected.all_bits) {
		char buf0[32], buf1[32];
		nvmeibc_dbits_entry_to_str(buf0, sizeof(buf0), dbits.all_bits);
		nvmeibc_dbits_entry_to_str(buf1, sizeof(buf1), expected.all_bits);
		WARN(true, "actual{%s=0x%x} != expected{%s=0x%x}, n_par=%u\n", buf0, dbits.all_bits, buf1, expected.all_bits, num_parities);
	}
}

static inline void __verify_ram_dbits_valid(const struct NVMeshSystem *sys, const struct TstPRaid* praid, const u32 (*conv_segs)[3], const u32 conv_nblk[3], union nvmeibc_dbits_entry expected)
{
	u8 i, si;
	const union nvmeibc_dbits_entry clean_dbit = { .all_bits = 0x0000 };
	const int num_par = __disk_range_get_num_parities(praid->cpr);
	for (si = 0; si < ARRAY_SIZE(*conv_segs); si++) {
		const struct ramDiskSimulator *ssd = &sys->servers[(*conv_segs)[si]].ramDisk;
		const u64 ram_blkst_offset = COMMITTED_ADDR_AS(ssd, praid->cpr[(*conv_segs)[si]].dlba_start, 4KB, LOCK);
		for (i = 0; i < conv_nblk[si]; i++)
			__verify_dbits_identical(ssd->dbits[ram_blkst_offset + i], expected,   num_par);
		if ((ram_blkst_offset + i) < RAMDISK_DATA_LOCK_SIZE)
			__verify_dbits_identical(ssd->dbits[ram_blkst_offset + i], clean_dbit, num_par);
	}
}

#define __is_slmod(d0, is_d0_convict, d1) !(d0 == 0xf || (d0 && d1) || is_d0_convict)

/* d0/d1 is dead0/dead1 value [ range 1 to 0xf which is unkown] , is_d0_convict = 1 if convict OW 0 */
static inline union nvmeibc_dbits_entry _db_entry(int d0, int is_d0_convict, int d1, int is_d1_convict)
{
	union nvmeibc_dbits_entry db;
	db.all_bits = 0;
	if (!__is_slmod(d0, is_d0_convict, d1)) {
		db.bsmod.dead0 = d0;
		db.bsmod.is_d0_convict = is_d0_convict;
		db.bsmod.dead1 = d1;
		db.bsmod.is_d1_convict = is_d1_convict;
		if (db.bsmod.mod_marker)
			db.bsmod.mod_marker += 0xc;
	}
	else {
		db.slmod.dead0 = d0;
	}
	return db;
}

struct tst_nvmeibc_dbits_entry
{
	union nvmeibc_dbits_entry db;
	int conv_seg_ind;   // -1 if no dbit
	int dirty_seg_ind;  // -1 if no dbit
};

/* d0/d1 is dead0/dead1 value [ range 1 to 0xf which is unkown] , is_d0_convict = 1 if convict OW 0,
   convict_ind - the seg ind for the following convict turn on */
static inline struct tst_nvmeibc_dbits_entry tst_db_entry(int d0, int is_d0_convict, int d1, int is_d1_convict, int convict_ind)
{
	struct tst_nvmeibc_dbits_entry db;
	db.db = _db_entry(d0, is_d0_convict, d1, is_d1_convict);
	db.conv_seg_ind = convict_ind;
	if (d0 == 0xf)
		db.dirty_seg_ind = (d1 == 0xf) ? -1 : d1 - 1;
	else if (d0 == convict_ind + 1)
		db.dirty_seg_ind = d1 - 1; // d0 is dirty in the index of the convict taking d1
	else
		db.dirty_seg_ind = d0 - 1;
	return db;
}

static inline void __verify_disk_dbits_valid(const struct TstPRaid *praid, const struct NVMeshSystem *sys, int p_ind, int q_ind, int d0, int d1)
{
	union nvmeibc_block_dp_ec_data_block_md *md;
	u64 dlba = praid->cpr[p_ind].dlba_start;
	md = ramDiskSimulator_get_metadataptr(&sys->servers[p_ind].ramDisk, dlba);
	BUG_ON(md->P.dbits_0 != d0);
	BUG_ON(md->P.dbits_1 != d1);
	dlba = praid->cpr[q_ind].dlba_start;
	md = ramDiskSimulator_get_metadataptr(&sys->servers[q_ind].ramDisk, dlba);
	BUG_ON(md->P.dbits_0 != d0);
	BUG_ON(md->P.dbits_1 != d1);
}

TEST_FUNC int __test_dirty_convict(bunitest_s* btest)
{
	struct NVMeshSystem *sys = btest->sys;
	struct clientSimulator *client = &sys->clients[0];
	const struct volume_segment_index vsi = { .volume=0, .chunk=0, .raid=0, .segment=0};  // Offset of convict
	struct TstPRaid praid = NVMeshSystem_TstPRaid_init_rel(sys, vsi);

	const int lenBlocks = 1;
	const int memSize = lenBlocks * NVMEIBC_SECTOR_SIZE;	// Total array in bytes
	u8 *mem = sim_kzalloc(memSize, GFP_KERNEL);	// Aligned Array to read/write to disk
	u8 i, si, itopo_other;
	enum NVMEIBTC_DS_MODE seg_mode[N_MAX_RAID_SLICE_LEN];

	//                   RW         RW       RW      D/W
	//         Segs      S8         S9       S0      S1
	// Blocksets|
	// ---------+---------------------------------------
	//    0     |        C1         C0		Primary
	//    1     |        C1         C0		Primary
	//    2     |                   C1      Primary   C0
	//    3     |                   C1      Primary   C0
	const u32 conv_segs[] = {8,9,0};	        // Verfy convict turned on 4 blocksets seg0, 4 on seg9 and 2 on seg8
	const u32 conv_nblk[] = {2,4,4};			// Todo: use praid.cpr[si].length/LOCKSET_4KS

	// pre(The initial state of dbits), expected state after convict turn on, expected state after rebuilding just the convict.
	const struct tst_nvmeibc_dbits_entry dbits_pre[] =    { tst_db_entry(2,0,0,0,3), tst_db_entry(2,1,0,0,3), tst_db_entry(4,0,2,1,3), tst_db_entry(4,0,2,0,3), tst_db_entry(0xf,0,2,1,3), tst_db_entry(0xf,0,2,0,3), tst_db_entry(4,0,0,0,1), tst_db_entry(4,0,2,0,1), tst_db_entry(4,1,2,0,1), tst_db_entry(4,1,0,0,1), tst_db_entry(0xf,0,4,0,1), tst_db_entry(0xf,0,4,1,1), tst_db_entry(0xf,0,0,0,1), tst_db_entry(0xf,0,0xf,0,1), tst_db_entry(0,0,0,0,1) };
	const union nvmeibc_dbits_entry dbits_post[] =        {    _db_entry(4,1,2,0),      _db_entry(4,1,2,1),      _db_entry(4,1,2,1),      _db_entry(4,1,2,0),      _db_entry(  4,1,2,1),        _db_entry(4,1,2,0),      _db_entry(4,0,2,1),      _db_entry(4,0,2,1),      _db_entry(4,1,2,1),      _db_entry(4,1,2,1),      _db_entry(  4,0,2,1),        _db_entry(4,1,2,1),       _db_entry(2,1,0,0),        _db_entry(2,1,0,0),        _db_entry(2,1,0,0)   };
	const union nvmeibc_dbits_entry dbits_post_rebuild[] ={    _db_entry(2,0,0,0),      _db_entry(2,1,0,0),      _db_entry(2,1,0,0),      _db_entry(2,0,0,0),      _db_entry(  2,1,0,0),        _db_entry(2,0,0,0),      _db_entry(4,0,0,0),      _db_entry(4,0,0,0),      _db_entry(4,1,0,0),      _db_entry(4,1,0,0),      _db_entry(  4,0,0,0),        _db_entry(4,1,0,0),       _db_entry(0,0,0,0),        _db_entry(0,0,0,0),        _db_entry(0,0,0,0)   };
	struct nvmeibc_maintain_sync_stats *maint_stats = &nvmeibc_flow_counters_ref()->main;

	for (i = 0; i < N_MAX_RAID_SLICE_LEN; ++i)
		seg_mode[i] = NVMEIBTC_DS_MODE_RW;

	for (itopo_other = 0; itopo_other < 2; ++itopo_other) {
		for (u32 ti = 0; ti < ARRAY_SIZE(dbits_pre); ++ti) {
			struct toma_recovery_args rcvr_args = {.type=NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON, .cmd=NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA};
			const int conv_seg_ind =  dbits_pre[ti].conv_seg_ind;
			const int dirty_seg_ind = dbits_pre[ti].dirty_seg_ind;
			const bool has_dirty_seg = (dirty_seg_ind != -1);
			const u64 dlba_stale = (praid.cpr[conv_segs[0]].dlba_start + 0);	// Put in first blockset, todo, try other blocksets
			rcvr_args.ext_args.is_mandatory.override = rcvr_args.ext_args.is_mandatory.value = true;

			// -- prepare RAM to pre state dbits.
			for (si = 0; si < ARRAY_SIZE(conv_segs); si++) {
				struct ramDiskSimulator *ssd = &sys->servers[conv_segs[si]].ramDisk;
				const u64 dlba_start =         praid.cpr[conv_segs[si]].dlba_start;
				const u64 ram_blkst_offset = COMMITTED_ADDR_AS(ssd, dlba_start, 4KB, LOCK);
				for (i = 0; i < conv_nblk[si]; i++)
					ssd->dbits[ram_blkst_offset + i].all_bits = dbits_pre[ti].db.all_bits;	// ramDiskSimulator_setDirty(ssd, dlba_start + i * LOCKSET_4KS)
			}
			if (1) { // -- prepare RAM to pre state stale-lock. Test dirty convict which encounters stale locks
				struct ramDiskSimulator *ssd = &sys->servers[conv_segs[0]].ramDisk;	// Put in copy owner, todo, inject randomly to other locks as well
				if (ti % 2)
					ramDiskSimulator_lockStaleRO_EC(ssd, dlba_stale);		// Read-Stale lock in copy owner
				else
					ramDiskSimulator_lockStale(     ssd, dlba_stale);		// Stale lock in copy owner
			}

			// -- Turn on dirty convict in ram
			if (has_dirty_seg) // if should turn on dbit.
				seg_mode[dirty_seg_ind] = NVMEIBTC_DS_MODE_DEAD;
			seg_mode[conv_seg_ind] = NVMEIBTC_DS_MODE_W_IS_DIRTY;
			tomaSimulator_switchTopoEC(praid.tpr->header.uuid, seg_mode, SW_TOPO__WAIT_ACK_DR, NULL);
			atomic_set(&maint_stats->n_dconvict_turnon, 0);
			BUG_ON(tomaSimulator_recoverThing(praid.tpr, praid.cpr, rcvr_args) < 0);     // ask segment to turn on dconvict on the blocksets it owns
			__verify_ram_dbits_valid(sys, &praid, &conv_segs, conv_nblk, dbits_post[ti]); // Verify convict turned on properly
			ramDiskSimulator_lockUnSta(&sys->servers[conv_segs[0]].ramDisk, dlba_stale);		// Verify stale lock remained and remove it
			BUG_ON(atomic_read(&maint_stats->n_dconvict_turnon) != 4);	// All 4 blocksets were handled properly

			if (!has_dirty_seg) {	// -- Todo: extend the test to verify convict and dirty bit, not only convicts
				BUG_ON(osSimulator_writeArrWait(&client->OS, vsi.volume, praid.cpr[0].bd_start, lenBlocks, mem));
				if (seg_mode[conv_seg_ind] == NVMEIBTC_DS_MODE_DEAD) {
					__verify_disk_dbits_valid(&praid, sys, 8, 9, conv_seg_ind + 1, 0);		// V 1.3, dirty convict turn on on DEAD topo. Verify the covict written to metadata of P,Q
				} else {
					__verify_disk_dbits_valid(&praid, sys, 8, 9, 0, 0);						// V 2.0, dirty convict turn on on W- topo so Write op will do no writehole sync
				}
			}

			// -- Rebuild convicts
			if (has_dirty_seg)
				seg_mode[dirty_seg_ind] = (itopo_other) ? NVMEIBTC_DS_MODE_W : NVMEIBTC_DS_MODE_DEAD;
			tomaSimulator_switchTopoEC(praid.tpr->header.uuid, seg_mode, SW_TOPO__WAIT_ACK_DR, NULL);
			BUG_ON(tomaSimulator_recoverThing(praid.tpr, praid.cpr, RCVR_DIRTY_REBUILD) < 0); // Turn off conv

			if (has_dirty_seg && seg_mode[dirty_seg_ind] == NVMEIBTC_DS_MODE_DEAD) {
				// Verify convict turned off properly
				__verify_ram_dbits_valid(sys, &praid, &conv_segs, conv_nblk, dbits_post_rebuild[ti]);
			}

			// -- Rebuild possible dirty bit.
			clientSimulator_wait_for_all_sync_ops(client);
			if (has_dirty_seg)
				seg_mode[dirty_seg_ind] = NVMEIBTC_DS_MODE_W;
			seg_mode[conv_seg_ind] = NVMEIBTC_DS_MODE_RW;
			tomaSimulator_switchTopoEC(praid.tpr->header.uuid, seg_mode, SW_TOPO__WAIT_ACK_DR, NULL);
			BUG_ON(tomaSimulator_recoverThing(praid.tpr, praid.cpr, RCVR_DIRTY_REBUILD) < 0);
			if (seg_mode[1] == NVMEIBTC_DS_MODE_RW) // In some blocksets this seg might be primary
				BUG_ON(tomaSimulator_recoverThing(praid.tpr, praid.cpr + 1, RCVR_DIRTY_REBUILD) < 0);
			clientSimulator_wait_for_all_sync_ops(client);
			if (has_dirty_seg)
				seg_mode[dirty_seg_ind] = NVMEIBTC_DS_MODE_RW;
			seg_mode[conv_seg_ind] = NVMEIBTC_DS_MODE_RW;
			tomaSimulator_switchTopoEC(praid.tpr->header.uuid, seg_mode, SW_TOPO__WAIT_ACK_DR, NULL);
			__verify_no_dirty_bits(sys, praid);
			__verify_disk_dbits_valid(&praid, sys, 8, 9, 0, 0);		// Verify the covict cleaned from metadata
		}  // end of ti(test index)
	}
	NVMeshSystem_serialize(btest->sys);
	BUG_ON(!NVMeshSystem_is_stable(btest->sys));
	sim_kfree(mem);
	return 0;
}


void __test_concurrent_and_chain_recoveries(bunitest_s* B) {
	const struct volume_segment_index vsi = {0,0,0,0};

	struct test_context env = {
			.sys = B->sys,
			.client = B->sys->clients,
			.dev = B->sys->clients->devs[vsi.volume],
			.sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi)

	};

	struct disk_range *sgmnt = &env.sraid.cpr[vsi.segment];
	struct ramDiskSimulator *ssd = &env.sys->servers[sgmnt->node_id].ramDisk;
	enum NVMEIBTC_DS_MODE acm[N_MAX_RAID_SLICE_LEN] = {[0 ...N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};

	char rebuild_cmd[256] = {'\0'};
	char rec_jgc_cmd[256] = {'\0'};
	char single_blockset_fixup_cmd[256] = {'\0'};
	const char *model = "#%s|recov_launch sgmnt=(0,0,%d) type=%u is_mandatory=%d do_only_owners=%d blocksets=%s jgc_cookie=0";
	int n_expected_ioctls = clientSimulator_get_num_executed_ioctls(env.client);

	unitest_print("*** %s - started\n", __FUNCTION__);

	sprintf(rebuild_cmd, model, env.dev->name, 0, NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, 1, 1, "[0, -1)");
	sprintf(rec_jgc_cmd, model, env.dev->name, 0, NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC,    0, 0, "[0, -1)");
	sprintf(single_blockset_fixup_cmd, model, env.dev->name, 0, NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, 1, 0, "[0, 1)");
	acm[1] = NVMEIBTC_DS_MODE_W_IS_DIRTY;
	tomaSimulator_switchTopoEC(env.sraid.tpr->header.uuid, acm, SW_TOPO__WAIT_ACK_DR, NULL);

	if (1) {									// Test chain recoveries. Dbits rebuild calls to dirty convict turn on + JGC running in parallel
		const int n_recoveries = 3;			 	// Don't sleep, wait for 3 recoveries
		struct sim_recovery_hooks hooks = sim_recovery_hooks_create(0, n_recoveries, n_recoveries, 0);
		sim_recovery_setup_hooks(&hooks);

		//take a lock; now both recoveries will spin on it (they will recover other blocksets)
		ramDiskSimulator_set_lock(ssd, sgmnt->dlba_start, SIMULATOR_OTHER_CLIENT_LOCK_ID);
		//apparently, it is not enough to take lock, DIRTY_REBUILD does even queries whether the blockset is locked or not
		//TODO: I think this is a bug. DIRTY_REBUILD should "wait" untill lock will be freed and then verify the dirty was rebuilt
		//otherwise it assumes other client WILL NOT turn on dirty bit under any condition
		ramDiskSimulator_setDirty(ssd, sgmnt->dlba_start, nvmeib_dbits_entry_build_unk(-1, -1).all_bits );
		clientSimulator_send_to_cli(env.client, rebuild_cmd); n_expected_ioctls++;	if (clientSimulator_get_num_executed_ioctls(env.client) != n_expected_ioctls) _Emerg("Simulator may stuck. Recovry not sent...!!!\n");
		clientSimulator_send_to_cli(env.client, rec_jgc_cmd); n_expected_ioctls++;	if (clientSimulator_get_num_executed_ioctls(env.client) != n_expected_ioctls) _Emerg("Simulator may stuck. Recovry not sent...!!!\n");

		nvmeibc_multi_completion_wait_for(&hooks.launched);
		ramDiskSimulator_set_unlock(ssd, sgmnt->dlba_start);
		nvmeibc_multi_completion_wait_for(&hooks.cleaned);
		sim_recovery_clean_hooks();

		NVMeshSystem_serialize(env.sys);
	}
	acm[1] = NVMEIBTC_DS_MODE_W; // Without convict
	tomaSimulator_switchTopoEC(env.sraid.tpr->header.uuid, acm, SW_TOPO__WAIT_ACK_DR, NULL);
	if (1) { // Test single blockset fixup with do_only_owners = false
		// Inject unknown DBs to two blocksets but fix only one!!!
		u64 *cur_num_main = &env.dev->dp.sync_rsrcs.stats.num_total_maintainance;	// Monitor amount of syncs
		u64 *cur_num_sync = &env.dev->dp.sync_rsrcs.stats.num_total;	// Monitor amount of syncs

		*cur_num_main = *cur_num_sync = 0;

		ramDiskSimulator_setDirty(ssd, sgmnt->dlba_start, nvmeib_dbits_entry_build_unk(-1, -1).all_bits);
		ramDiskSimulator_setDirty(ssd, sgmnt->dlba_start+LOCKSET_SLICES, nvmeib_dbits_entry_build_unk(-1, -1).all_bits);
		ssd->TxIDs[1] = INITIAL_LAZY_READ_TXID;
		ssd->TxIDs[2] = INITIAL_LAZY_READ_TXID;
		// Call single blockset sync
		clientSimulator_send_to_cli(env.client, single_blockset_fixup_cmd); n_expected_ioctls++; if (clientSimulator_get_num_executed_ioctls(env.client) != n_expected_ioctls) _Emerg("Simulator may stuck. Recovry not sent...!!!\n");

		clientSimulator_wait_for_all_recoveries_done(env.client);
		clientSimulator_wait_for_all_sync_ops(env.client);
		// One DB unknown Resolution - not logged?
		// BUG_ON(*cur_num_main != 1);
		// One DB sync
		BUG_ON(*cur_num_sync != 1);

		*cur_num_main = *cur_num_sync = 0;

		BUG_ON(ssd->dbits[0].all_bits);
		BUG_ON(ssd->dbits[1].all_bits != nvmeib_dbits_entry_build_unk(-1, -1).all_bits);
		BUG_ON(ssd->TxIDs[1] != INITIAL_LAZY_READ_TXID);
		// Add both locksets now and re-verify when running from non-owner (1 is in W mode and will WARN if we ask it to recover)
		sprintf(single_blockset_fixup_cmd, model, env.dev->name, 2, NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, 1, 0, "[0, 2)");
		clientSimulator_send_to_cli(env.client, single_blockset_fixup_cmd); n_expected_ioctls++; if (clientSimulator_get_num_executed_ioctls(env.client) != n_expected_ioctls) _Emerg("Simulator may stuck. Recovry not sent...!!!\n");

		clientSimulator_wait_for_all_recoveries_done(env.client);
		clientSimulator_wait_for_all_sync_ops(env.client);
		// One DB unknown Resolution and one TX_ID - not logged
		// BUG_ON(*cur_num_main != 2);
		// Two DB sync
		BUG_ON(*cur_num_sync != 2);
		BUG_ON(ssd->TxIDs[1] == INITIAL_LAZY_READ_TXID);

		*cur_num_main = *cur_num_sync = 0;

		// Use offset and fix only unknown TXID
		sprintf(single_blockset_fixup_cmd, model, env.dev->name, 3, NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, 1, 0, "[2, 3)");
		clientSimulator_send_to_cli(env.client, single_blockset_fixup_cmd); n_expected_ioctls++; if (clientSimulator_get_num_executed_ioctls(env.client) != n_expected_ioctls) _Emerg("Simulator may stuck. Recovry not sent...!!!\n");

		clientSimulator_wait_for_all_recoveries_done(env.client);
		clientSimulator_wait_for_all_sync_ops(env.client);

		BUG_ON(ssd->TxIDs[2] == INITIAL_LAZY_READ_TXID);
		BUG_ON(*cur_num_sync != 1);

		// No stales no dirty
		ramDiskSimulator_verify_no_locks(ssd);
		ramDiskSimulator_verify_no_dirty_bits(ssd);
	}
	//back to normal
	acm[1] = NVMEIBTC_DS_MODE_RW;
	tomaSimulator_switchTopoEC(env.sraid.tpr->header.uuid, acm, SW_TOPO__WAIT_ACK_DR, NULL);
	NVMeshSystem_wipe_all_dirty_bits(env.sys);

	NVMeshSystem_serialize(env.sys);
	BUG_ON(!NVMeshSystem_is_stable(env.sys));

	unitest_print("*** %s - done\n", __FUNCTION__);
}

/****************************** Scrubbing *************************************/
static void allocate_slice_blocks_zero(u32 n_segs, struct t_slice_data* inj) {
	u8 *blob = sim_kzalloc(NVMEIBC_SECTOR_SIZE * n_segs, GFP_KERNEL);
	u32 i;
	for (i = 0; i < n_segs; i++)
		inj->blocks[0][i] = (void*)&blob[i * NVMEIBC_SECTOR_SIZE];
}

void t_slice_data_extend_slice_inj_to_ms_inject(struct t_slice_data *msl, struct t_slice_all_blks *sl) {
	memset(msl, 0, sizeof(*msl));
	memcpy(msl->blocks[0], sl->blocks, sizeof(sl->blocks));
	memcpy(msl->md[0], sl->md, sizeof(sl->md));
}

extern void __reed_solo_fill_missing(const struct test_context env, struct t_slice_data* sdata, const u64 slba, const u16 sidx);
struct t_corrupted_block {
	u8 *block;
	union nvmeibc_block_dp_ec_data_block_md md;
	u32 ind;					// Index of corrupted block in a slice
	bool is_data;				// is corrutped block a data or parity
	enum corruption_type {	CORRUPT_BAD_SECT = 'B',			// Logical bad sector
							CORRUPT_WRONG_EDIC = 'E',		// Not supported yet, just change edic of D or P so it will be wrong. Causes a WARN upon read
							CORRUPT_SCRUB = 'S',			// Edic is correct for each column (D/P) but P does not match to D so parities will be recalculated.
							CORRUPT_1P_NOT_WRITTEN = 'P' }	// P is never written while entire slice + q is written
							type;
};
static void t_corrupted_block_reinit_random(struct t_corrupted_block *c, const struct disk_range *pr, const int rand_seed) {
	const enum corruption_type errs[] = {CORRUPT_BAD_SECT, CORRUPT_WRONG_EDIC, CORRUPT_SCRUB};
	const int n_errors = ARRAY_SIZE(errs);
	if (c->block == NULL)
		c->block = sim_kmalloc(NVMEIBC_SECTOR_SIZE, GFP_KERNEL);	// Allocate only on first iteration
	c->ind = ((rand_seed / n_errors) % pr->replicas);
	c->is_data = (c->ind < pr->slice_size);
	c->type = errs[rand_seed % n_errors];
	if ((c->type == CORRUPT_SCRUB) && (!c->is_data) && (rand_seed < (n_errors*(int)pr->replicas)))
		c->type = CORRUPT_1P_NOT_WRITTEN;
}

static void t_corrupted_block_copy_inj(struct t_corrupted_block *c, const char *direction, struct t_slice_data *inj) {
	if (direction[0] == '<') {			// '>' Inialize corrupted data
		c->md.raw =      inj->md[0][c->ind].raw;
		memcpy(c->block, inj->blocks[0][c->ind], NVMEIBC_SECTOR_SIZE);
	} else if (direction[0] == '>') {	// Inject corrupted data
		inj->md[0][c->ind].raw = c->md.raw;
		if (c->type == CORRUPT_WRONG_EDIC)
			inj->md[0][c->ind].P.edic ^= 0x2;	// Alter 1 bit of edic
		memcpy(inj->blocks[0][c->ind], c->block, NVMEIBC_SECTOR_SIZE);
	}
}

static void t_corrupted_block_destroy(struct t_corrupted_block *c) {
	sim_kfree(c->block);
	c->block = NULL;
}

struct t_scrub_tester {
	struct t_corrupted_block corrupt;
	struct block_inject_ptrs bptrs[N_MAX_RAID_SLICE_LEN];
	struct t_slice_data inj;						// Blocks for injection
	const struct disk_range *pr;					// praid in which the test runs
	u32 slba;										// slba where of the corrupted slice
	u32 lockset_owner_seg;							// Which disk is primary owner of corrutped slice
	struct {
		u8 *block;
		union nvmeibc_block_dp_ec_data_block_md md;
	} restored;
	int multi_slice_ind; 			// = 0; // No multi slice in this test, generate 1 slice only
	int txid;
	int n_total_syncs, n_expected_scrub_nop, n_expected_scrubs, n_bad_sectors, n_di_fix; // Expected stats
};

static void t_scrub_tester_init(struct t_scrub_tester *st) {
	memset(st, 0, sizeof(*st));
	st->txid = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS;
}

static void t_scrub_tester_set_inject_ptrs(struct t_scrub_tester *st, struct NVMeshSystem *sys, const struct disk_range *pr, u32 slba) {
	int i, n_segs = pr->replicas;
	st->pr = pr;
	st->slba = slba;
	st->lockset_owner_seg = (st->slba >> LOCKSET_SHIFT) % n_segs;
	for (i = 0; i < n_segs; i++) {
		const raid_sgmnt_t di = (i + st->lockset_owner_seg) % n_segs;		// Convert role to seg
		const u64 dlba = st->slba + pr[di].dlba_start;
		st->bptrs[i] = serverSimulator_get_block_inject_ptrs(&sys->servers[di], dlba, -1, -1, st->multi_slice_ind);	// -1 Means not using journals
	}
}

static void t_scrub_tester_reinit_random(struct t_scrub_tester *st, struct NVMeshSystem *sys, const struct disk_range *pr, u32 slba, const int rand_seed) {
	st->txid++;
	t_scrub_tester_set_inject_ptrs(st, sys, pr, slba);
	t_corrupted_block_reinit_random(&st->corrupt, pr, rand_seed);
}

static void t_scrub_tester_generate_valid_slice(struct t_scrub_tester *st, const struct test_context *env) { // Generate full slice of data + parities + all edics
	const int slice_size = env->sraid.cpr->slice_size;
	for (int i = 0; i < slice_size; i++)
		__unitest_fill_blocks_unique_pattern(st->inj.blocks[st->multi_slice_ind][i], 1);
	__reed_solo_fill_missing(*env, &st->inj, st->slba, st->multi_slice_ind);
}

static void t_scrub_tester_inject_valid_slice(struct t_scrub_tester *st) {
	int i, n_segs = st->pr->replicas, slice_size = st->pr->slice_size;
	for (i = 0; i < n_segs; i++) {
		struct block_inject_ptrs *bp = &st->bptrs[i];
		memcpy(bp->data, st->inj.blocks[st->multi_slice_ind][i], NVMEIBC_SECTOR_SIZE);
		nvmeibc_block_dp_ec_md_set_externally_written_by_nvck_ec(bp->dmd, st->txid, (i >= slice_size), st->inj.md[0][i].D.edic, NULL);
		*bp->ram.txid = bp->dmd->tx_id;
	}
	if ((st->corrupt.type == CORRUPT_1P_NOT_WRITTEN) && (!st->corrupt.is_data)) {
		st->bptrs[st->corrupt.ind].dmd->raw = nvmeibc_ec_unwritten_md_entry_val.raw;
	}
}

static void t_scrub_tester_inject_bad_sector(struct t_scrub_tester *st, int seg_i) {
	struct block_inject_ptrs *bp = &st->bptrs[seg_i];
	bp->dmd->raw = nvmeibc_ec_unwritten_md_entry_val.raw;
	nbdpec_md_mark_data_invalid_for_read(bp->dmd, ((u32)seg_i >= st->pr->slice_size), st->txid);
}

static void t_scrub_tester_insert_corrupt_block_into_slice(struct t_scrub_tester *st, const struct test_context *env) {
	const int i = (st->corrupt.is_data) ? st->corrupt.ind : 0;					// If corrupted block was data - change it, if it was parity change D0 to create a different block. Note: If you change more than 1 block then first parity might not change
	union nvmeibc_block_dp_ec_data_block_md *md = &st->inj.md[st->multi_slice_ind][st->corrupt.ind];
	do { // Generate new 4K with edic not identical to the corrupted block
		__unitest_fill_blocks_unique_pattern(st->inj.blocks[st->multi_slice_ind][i], 1);
		__reed_solo_fill_missing(*env, &st->inj, st->slba, st->multi_slice_ind);
	} while (md->raw == st->corrupt.md.raw);

	// Save aside original value (restored) and inject corrupted value
	st->restored.md.raw = md->raw;
	memcpy(st->restored.block, st->inj.blocks[st->multi_slice_ind][st->corrupt.ind], NVMEIBC_SECTOR_SIZE);
	t_corrupted_block_copy_inj(&st->corrupt, ">>", &st->inj);			// Corrupt the block
}

static void t_scrub_tester_restore_slice(struct t_scrub_tester *st) {
	const int i = st->corrupt.ind;
	memcpy(st->bptrs[i].data, st->restored.block, NVMEIBC_SECTOR_SIZE);
	if (st->corrupt.is_data)
		st->bptrs[i].dmd->D.edic = st->restored.md.D.edic;
	else
		st->bptrs[i].dmd->P.edic = st->restored.md.P.edic;
}

static void t_scrub_tester_set_expectors(struct t_scrub_tester *st) {
	const int n_blocksets_to_recover = (1<<LOCK_CHANGE_STRIDE_SHIFT);	// This many syncs in 1 recovery
	st->n_total_syncs += n_blocksets_to_recover;
	st->n_expected_scrub_nop += (n_blocksets_to_recover - 1);			// We inject problem in 1 blockset only, the rest are nothing to do
	st->n_expected_scrubs += (st->corrupt.type == CORRUPT_1P_NOT_WRITTEN);	// Fix the problem, instead of warning
	st->n_di_fix          += (st->corrupt.type == CORRUPT_1P_NOT_WRITTEN);	// Fix the problem, instead of warning
	st->n_expected_scrubs += (st->corrupt.type == CORRUPT_SCRUB);
	st->n_bad_sectors += (st->corrupt.type == CORRUPT_BAD_SECT) || (st->corrupt.type == CORRUPT_WRONG_EDIC);
}

#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole.h"
int unitest_scrubRecovery_ec(bunitest_s* B) {
	struct NVMeshSystem *sys = B->sys;
	struct TstPRaid sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, (const struct volume_segment_index){0,0,0,0});
	int i, rep;
	struct toma_recovery_args rcvr_args = { .type = NVMEIBT_RECOVERY_TYPE_SCRUBBING, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA };
	struct t_scrub_tester st;
	struct test_context env = {.sys = sys, .client = sys->clients, .dev = sys->clients->devs[sraid.vsi.volume], .sraid = sraid};
	const struct disk_range *pr = env.sraid.cpr;
	const int n_segs = pr->replicas, rand_seed = (u32)(jiffies);	// Store it to be able to reproduce random sequence
	struct nvmeibc_nowhole_stats stats;
	const union nvmeibc_block_dp_ec_data_block_md* dmd;
	srand(rand_seed);
	t_scrub_tester_init(&st);
	__dd_clean_dlba_pointers(env);
	allocate_slice_blocks_zero(n_segs, &st.inj);
	NVMeshSystem_all_clients_dbg_di(sys, true);
	st.restored.block = sim_kzalloc(NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
	nvmeibc_nowhole_stats_reset();
	nvmeibc_warn_on_edic_verification_failure = false;							// Unitest cases edic failure
	nvmeibc_warn_on_parities_sync_missmatch = false;							// Unitest can inject 1 parity as written, the other as not written
	for (rep = 0; rep < 75; rep++) {
		const u64 n_blksets_in_praid = sraid.cpr->length/LOCKSET_SLICES;
		const u64 blockset = ((rep*3) % n_blksets_in_praid);
		const u32 slice =    ((rep*7) % LOCKSET_SLICES);
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), rep);
		t_scrub_tester_set_expectors(&st);
		t_scrub_tester_generate_valid_slice(&st, &env);		// Generate full slice of data + parities + all edics
		t_corrupted_block_copy_inj(&st.corrupt, "<<", &st.inj);
		t_scrub_tester_insert_corrupt_block_into_slice(&st, &env);
		t_scrub_tester_inject_valid_slice(&st); // Inject corrupted slice to ssd disks
		if (st.corrupt.type == CORRUPT_BAD_SECT)
			t_scrub_tester_inject_bad_sector(&st, st.corrupt.ind);
		// } if (1) {	// Todo: Instead of many iterations, inject a few problems to single blockset
		// unitest_print("-----------------------Corrupt: slice=%d, slba=%u, ind=0x%x, rep=%u, err=%c\n", slice, st.slba, st.corrupt.ind, rep, st.corrupt.type);
		BUG_ON(tomaSimulator_recoverThing(sraid.tpr, sraid.cpr + st.lockset_owner_seg, rcvr_args) < 0);
		dmd = st.bptrs[st.corrupt.ind].dmd;
		if ((st.corrupt.type == CORRUPT_BAD_SECT) || (st.corrupt.type == CORRUPT_WRONG_EDIC)) { // Bad sector in data fixed and correct edic was recalculated
			BUG_ON((dmd->D.edic != st.restored.md.D.edic) || is_data_invalid_for_read(dmd));
		} else if ((st.corrupt.type == CORRUPT_SCRUB) && (!st.corrupt.is_data)) {
			BUG_ON(dmd->D.edic != st.restored.md.D.edic);										// Edi fixed, data was fixd
		} else if (st.corrupt.type == CORRUPT_1P_NOT_WRITTEN) { // Should be fixed to written
			BUG_ON(nbdpec_md_was_data_never_written(dmd));
		}
		// Verify correct flow
		nvmeibc_nowhole_stats_get(&stats);
		BUG_ON(atomic_read(&stats.n_scrub_fix) != st.n_expected_scrubs);
		BUG_ON(atomic_read(&stats.n_bdsec_fix) != st.n_bad_sectors);
		BUG_ON(atomic_read(&stats.n_other_fix) != st.n_expected_scrub_nop);
		BUG_ON(atomic_read(&stats.n_di_fix)    != st.n_di_fix);
		if (0) t_scrub_tester_restore_slice(&st); // Scrubber fixed all Parities when EDIC fails - no need to changes blocks back to make slice consistent, EDIC and parities are compatible with slice
	}
	nvmeibc_nowhole_stats_reset();

	// --------------------------------------------- Slice by slice mode with multiple failures in blockset
	rcvr_args.ext_args.lock_range.override = true;					// Segments have 8 blocksets, for now run recovery on only 1 blockset
	rcvr_args.ext_args.lock_range.start = 0;
	rcvr_args.ext_args.lock_range.count = 1;	// Working only on 1 blockset
	{
		const u32 blockset = 0;					// Use first blockset
		u32 failed_slices = 0, fixed_slices = 0;	// Bitmpas of expected failed / fixed slices.

		// Slice 31 OK (or Never written)
		u32 slice = 30;	// Slice {BAD_SECT in D0}
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		fixed_slices |= (1 << slice);

		slice = 29;	// Slice {BAD_SECT in D5}
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 5);
		fixed_slices |= (1 << slice);

		slice = 28;	// Slice {BAD_SECT in D3}
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 3);
		fixed_slices |= (1 << slice);

		slice = 26;	// Never written slice with 3 bad sectors, will be destroyed.
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_inject_bad_sector(&st, 0);
		t_scrub_tester_inject_bad_sector(&st, 1);
		t_scrub_tester_inject_bad_sector(&st, 2);
		failed_slices |= (1 << slice);

		slice = 25;	// Never written slice with 3 bad sectors and no parity, will be destroyed.
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_inject_bad_sector(&st, n_segs-3);
		t_scrub_tester_inject_bad_sector(&st, n_segs-2);
		t_scrub_tester_inject_bad_sector(&st, n_segs-1);
		failed_slices |= (1 << slice);

		slice = 24;	// Written slice with 3 bad sectors, will be destroyed, Max Txid taken from parity
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		t_scrub_tester_inject_bad_sector(&st, 1);
		t_scrub_tester_inject_bad_sector(&st, 2);
		failed_slices |= (1 << slice);

		slice = 23;	// Written slice with 3 bad sectors, covering all parities, will be destroyed, Max Txid taken from parity
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, n_segs-3);
		t_scrub_tester_inject_bad_sector(&st, n_segs-2);
		t_scrub_tester_inject_bad_sector(&st, n_segs-1);
		failed_slices |= (1 << slice);

		slice = 22;	// Never written slice with 3 bad sectors, some parities are available, but not all. wWll be destroyed.
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_inject_bad_sector(&st, 0);
		t_scrub_tester_inject_bad_sector(&st, 1);
		t_scrub_tester_inject_bad_sector(&st, n_segs-1);
		failed_slices |= (1 << slice);

		slice = 21;	// Written slice with 3 bad sector, some parities are available, but not all. Will be destroyed. Max Txid taken from parity
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		t_scrub_tester_inject_bad_sector(&st, 1);
		t_scrub_tester_inject_bad_sector(&st, n_segs-1);
		failed_slices |= (1 << slice);

		slice = 20;	// Slice {Wrong D4}
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		st.corrupt.ind = 4; st.corrupt.is_data = 1;
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_corrupted_block_copy_inj(&st.corrupt, "<<", &st.inj);
		__unitest_fill_blocks_unique_pattern(st.inj.blocks[0][st.corrupt.ind], 1);
		__reed_solo_fill_missing(env, &st.inj, st.slba, 0);
		t_corrupted_block_copy_inj(&st.corrupt, ">>", &st.inj);			// Corrupt the block
		t_scrub_tester_inject_valid_slice(&st);
		fixed_slices |= (1 << slice);

		slice = 18;	// Slice {Wrong D2}
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		st.corrupt.ind = 2; st.corrupt.is_data = 1;
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_corrupted_block_copy_inj(&st.corrupt, "<<", &st.inj);
		__unitest_fill_blocks_unique_pattern(st.inj.blocks[0][st.corrupt.ind], 1);
		__reed_solo_fill_missing(env, &st.inj, st.slba, 0);
		t_corrupted_block_copy_inj(&st.corrupt, ">>", &st.inj);			// Corrupt the block
		t_scrub_tester_inject_valid_slice(&st);
		fixed_slices |= (1 << slice);

		slice = 17;	// Slice {D2, Wrong Edic}
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), 0);
		st.corrupt.ind = 2; st.corrupt.is_data = 1; st.corrupt.type = CORRUPT_WRONG_EDIC;
		t_scrub_tester_generate_valid_slice(&st, &env);
		t_corrupted_block_copy_inj(&st.corrupt, "<<", &st.inj);
		t_corrupted_block_copy_inj(&st.corrupt, ">>", &st.inj);			// Corrupt the block
		t_scrub_tester_inject_valid_slice(&st);
		fixed_slices |= (1 << slice);

		for (i = 0; i < n_segs; i++) { // Set expectors
			tomaSimulator_expectIOFailure(&sys->servers[pr[i].node_id].simToma, EPERM_READ_FAIL_NO_RETRY, failed_slices, fixed_slices);
		}
		// unitest_print("-----------------------Corrupt: MultiSlice\n");
		BUG_ON(tomaSimulator_recoverThing(sraid.tpr, sraid.cpr + st.lockset_owner_seg, rcvr_args) < 0);
		// Verify correct flow
		nvmeibc_nowhole_stats_get(&stats);
		BUG_ON(atomic_read(&stats.n_destoyed)  != 1);	// Some slices were destroyed in 1 blockset
		BUG_ON(atomic_read(&stats.n_bdsec_fix) != 1);	// Some slices were fixed     in 1 blockset
		nvmeibc_nowhole_stats_reset();

		// unitest_print("-----------------------Corrupt: MultiSlice\n");
		for (i = 0; i < n_segs; i++) { // Set expectors
			tomaSimulator_expectIOFailure(&sys->servers[pr[i].node_id].simToma, EPERM_READ_FAIL_NO_RETRY, failed_slices, 0);
		}
		BUG_ON(tomaSimulator_recoverThing(sraid.tpr, sraid.cpr + st.lockset_owner_seg, rcvr_args) < 0);
		// Verify correct flow
		nvmeibc_nowhole_stats_get(&stats);
		BUG_ON(atomic_read(&stats.n_destoyed)  != 1);	// Some slices were destroyed in 1 blockset
		BUG_ON(atomic_read(&stats.n_bdsec_fix) != 0);	// Zero slices were fixed     in 1 blockset
		nvmeibc_nowhole_stats_reset();
	}
	t_corrupted_block_destroy(&st.corrupt);
	sim_kfree(st.inj.blocks[0][0]);
	sim_kfree(st.restored.block);
	clientSimulator_wait_for_all_sync_ops(env.client);
	nvmeibc_warn_on_edic_verification_failure = true;							// Unitest cases edic failure
	nvmeibc_warn_on_parities_sync_missmatch = true;
	NVMeshSystem_all_clients_dbg_di(sys, false);
	NVMeshSystem_serialize(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	return 0;
}
/******************************************************************************/

// Basic recovery driven by toma - set stale locks and trigger sync verify all were cleared
// 								 - set Dirty bits and trigger sync verify all were cleared
//								 - set Dirty suspect and trigger sync verity all were cleared
// TODO: Add data verification
TEST_FUNC int unitest_EC_recovery_Basic(bunitest_s* B) {
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator *client = &sys->clients[0];		// Test via the first client
	u32 i, do_deg;
	int os, l, k;											// Owner seg index, number of copies, lock index
	int volIndx = 0;
	u64 magic_pattern = 0;
	struct disk_range *seg  = &sys->mdb.vols[volIndx].segs[0];
	struct tTopoOfVolume *cfv    = &sys->tcf.vols[volIndx];
	struct tTopoOfPraid  *r1     = tTopoOfVolume_getRaid1(cfv, 0);
	const int slice_size = seg->slice_size, n_segs = seg->replicas;
	const int max_copies = 3; //== ___get_tail_topo_of_device(sys, volIndx)->chunks[0].raid1s[0].lock_scheme.max_n_owners;
	const int full_ZIGZAG_step_slices = (1<<LOCKSET_SHIFT)*n_segs;
	u8 *mem = sim_kmalloc(slice_size * NVMEIBC_SECTOR_SIZE, GFP_KERNEL);	// Aligned Array to read/write to disk
	const struct volume_segment_index vsi = {0,0,0,0};
	struct TstPRaid sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi);
	 struct test_context env = {
		 .sys = sys,
		 .client = sys->clients,
		 .dev = sys->clients->devs[sraid.vsi.volume],
		 .sraid = sraid
	 };

	NVMeshSystem_all_clients_dbg_di(sys, true);

	__test_concurrent_and_chain_recoveries(B);
	__dd_clean_dlba_pointers(env);

	BUG_ON(tomaSimulator_recoverThing(r1, &seg[0], RCVR_STALE_REBUILD));		// Verify Empty stale recovery works
	#define __move_to_prev_seg(si)  ({ si--; if (si < 0) si += n_segs; })
	for (l = 1; l <= max_copies; l++) {
		for (os = 0; os < n_segs; os++) { // ------------------------------------- Stale locks recovery: msg sent by 1 toma to owner
			for (i = os*(1<<LOCKSET_SHIFT); i < seg->length; i += full_ZIGZAG_step_slices) { // Iterate on all (half) blocksets of owner seg
				int si = os; // si changes and must be reset -> sets owner then q then p
				for (k = 0; k < l; k++) {		// Put stale starting from owner up to 'l' copies
					ramDiskSimulator_lockStale(&sys->servers[seg[si].node_id].ramDisk, seg[si].dlba_start + i);
					__move_to_prev_seg(si);
				}
			}
			BUG_ON(tomaSimulator_recoverThing(r1, &seg[os], RCVR_STALE_REBUILD) < 0);
			BUG_ON(tomaSimulator_recoverThing(r1, &seg[os], RCVR_STALE_REBUILD_PING) < 0);           // Test, ping

			clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);
			tomaSimulator_waitProtoEnd(NULL);
			__verify_no_remaining_locks_ec(sys, volIndx); // Verify all stale locks (owners and copies)
		}
	}

	for (l = 2; l <= max_copies; l++) {
		for (os = 0; os < n_segs; os++) { // ------------------------------------- Stale locks recovery: stale lock one/two copy/ies and verify cleaned. Owner is always clean
			for (i = os*(1<<LOCKSET_SHIFT); i < seg->length; i += full_ZIGZAG_step_slices) { // Iterate on all (half) blocksets of owner seg
				int si = os;
				for (k = 0; k < (l-1); k++) {		// All/Some copies
					__move_to_prev_seg(si);
					ramDiskSimulator_lockStale(&sys->servers[seg[si].node_id].ramDisk, seg[si].dlba_start + i);
					// TODO: inject ancestor lock as well to the same index, verify cleaned once we send Blockset recovered to TOMA
				}
			}
			// In order to trigger the stale lock recovery we need to write once on each blockset in order to find the stale copies
			for (i = os*(1<<LOCKSET_SHIFT); i < seg->length; i += full_ZIGZAG_step_slices) { // Iterate on all (half) blocksets of owner seg
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, slice_size);  // Set a pattern.
				BUG_ON(magic_pattern == 0);
				REPORT_ERROR(osSimulator_writeArr(&client->OS, volIndx, __from4K(i)*slice_size+os, slice_size, mem));
				clientSimulator_wait_for_all_bio_ops(client);
			}

			clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);
			tomaSimulator_waitProtoEnd(NULL);
			__verify_no_remaining_locks_ec(sys, volIndx); // Verify stale lock was indeed cleared
		}
	}

	for (os = 0; os < n_segs; os++) { // ------------------------------------- Dbits recovery
		int si = 4; // This will be the degraded segment
		if (os == 4) {
			si = 5;
		}
		__degrade_segment(          &sys->clients[0], r1, seg, si);
		__restore_seg_to_write(sys, &sys->clients[0], r1, seg, si, true);

		for (i = os*(1<<LOCKSET_SHIFT); i < seg->length; i += full_ZIGZAG_step_slices) {
			if (os%3 == 0) { // Set dirty suspect and ensure that MD shows the degraded seg and a regular degraded blockset
				const int odd_blkset_slice = i+LOCKSET_SLICES+1;
				const int p_index = (slice_size + os)%n_segs;
				union nvmeibc_block_dp_ec_data_block_md *md_p = physSegMDIdxPtr_off(&seg[p_index], odd_blkset_slice);
				ramDiskSimulator_setDirty(&sys->servers[seg[os].node_id].ramDisk, seg[os].dlba_start + odd_blkset_slice, nvmeib_dbits_entry_build_unk(-1,-1).all_bits);
				ramDiskSimulator_setDirty(&sys->servers[seg[os].node_id].ramDisk, seg[os].dlba_start + i, si+1 /* index of W seg encoded */);
				__set_valid_dbits(md_p, si+1, 0);
			} else { // Regular DB setting
				ramDiskSimulator_setDirty(&sys->servers[seg[os].node_id].ramDisk, seg[os].dlba_start + i, si+1 /* index of W seg encoded */);
			}
		}

		BUG_ON(tomaSimulator_recoverThing(r1, &seg[os], RCVR_DIRTY_REBUILD) < 0);
		BUG_ON(tomaSimulator_recoverThing(r1, &seg[os], RCVR_DIRTY_REBUILD) < 0); // No dirtybits, call recovery which will do nothing
		BUG_ON(tomaSimulator_recoverThing(r1, &seg[os], RCVR_INVALID) < 0);                         	     // Illegal type of recovery
		__restore_seg_to_read_write(sys, r1, si);

		NVMeshSystem_serialize(sys);
		BUG_ON(!NVMeshSystem_is_stable(sys));
	}

	// ------------------------------------- Dbits suspect within DB sync
	for (do_deg = 0; do_deg <= 1; do_deg++)	{ // 2 loops, 1 in perfect topology, 1 with single degraded 'W' but not D0,P,Q

	for (os = 0; os < n_segs; os++) { 	// Advance in vlba to select D0 as segment 'os'
		int ps = (os + slice_size)%n_segs, qs = (os + slice_size + 1)%n_segs;
		const int p_index = (slice_size + os)%n_segs;
		const int slice_index = os*(1<<LOCKSET_SHIFT);
		const long unsigned int vlba = slice_index*slice_size + 3;
		if (vlba > env.dev->size) {
			break;
		} else {
			struct ramDiskSimulator *srvOwlock =   &(sys->servers[seg[os].node_id].ramDisk);
			struct ramDiskSimulator *srvOwlock_p = &(sys->servers[seg[ps].node_id].ramDisk);
			struct ramDiskSimulator *srvOwlock_q = &(sys->servers[seg[qs].node_id].ramDisk);

			const u64 di =   COMMITTED_ADDR_AS(&sys->servers[seg[os].node_id].ramDisk, seg[os].dlba_start + slice_index, 4KB, LOCK);
			const u64 di_p = COMMITTED_ADDR_AS(&sys->servers[seg[ps].node_id].ramDisk, seg[ps].dlba_start + slice_index, 4KB, LOCK);
			const u64 di_q = COMMITTED_ADDR_AS(&sys->servers[seg[qs].node_id].ramDisk, seg[qs].dlba_start + slice_index, 4KB, LOCK);

			union nvmeibc_block_dp_ec_data_block_md *md_p = physSegMDIdxPtr_off(&seg[p_index], slice_index + 3);
			union nvmeibc_dbits_entry dbs = {.all_bits = 0};
			int si = os;
			BUG_ON(slice_index+3 > (int)seg[p_index].length);

			if (do_deg) {
				__degrade_segment(&sys->clients[0], r1, seg, os+1);	// D1
				__restore_seg_to_write(sys, &sys->clients[0], r1, seg, os+1, true);
			}
			srvOwlock->TxIDs[di] = NVMEIBC_DP_EC_MD_TX_ID_MAX;         // Screw-up the RAM of owner lock -> should not be affect by DB/DS sync
			srvOwlock->dbits[di].all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			srvOwlock_p->dbits[di_p].all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			srvOwlock_q->dbits[di_q].all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			dbs.slmod.dead0 = 1;
			__set_valid_dbits(md_p, 1, -1);
			nvmeibc_debug_ram_binfo = false;	// We do not allow setting DBs that are in W+ or better
			BUG_ON(tomaSimulator_recoverThing(r1, &seg[os], RCVR_DIRTY_REBUILD) < 0);
			clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);
			nvmeibc_debug_ram_binfo = true;
			for (k = 0; k < max_copies; k++) {
				struct ramDiskSimulator* si_ssd = &sys->servers[seg[si].node_id].ramDisk;
				const u64 db_entry_offset = COMMITTED_ADDR_AS(si_ssd, seg[si].dlba_start + slice_index, 4KB, LOCK);
				union nvmeibc_dbits_entry *dbit_in_ram = &si_ssd->dbits[db_entry_offset];
				if (do_deg) {
					BUG_ON(dbit_in_ram->all_bits != dbs.all_bits);			// Dirty suspect was resolve to 1 from disk metadata (even if it's not turn on/off)
					dbit_in_ram->all_bits = 0;
				} else {
					BUG_ON(dbit_in_ram->all_bits != zero_dbits.all_bits);	// Because unknown was resolved automatically to 0
				}
				__move_to_prev_seg(si);
			}
			__set_valid_dbits(md_p, 0, -1);

			if (do_deg)
				__restore_seg_to_read_write(sys, r1, os+1);

			NVMeshSystem_serialize(sys);
			BUG_ON(!NVMeshSystem_is_stable(sys));
		}
	}
	}
	__EC_sync_commit_blockset(B);

	if (1) {					// Just test address translation
		char cmd[128], *vname = client->devs[0]->name;
		snprintf(cmd, sizeof(cmd), "#%s|translate_addr=17"        ,vname);	clientSimulator_send_to_cli(client, cmd);
		snprintf(cmd, sizeof(cmd), "#%s|translate_addr=17,5"      ,vname);	clientSimulator_send_to_cli(client, cmd);
		snprintf(cmd, sizeof(cmd), "#%s|translate_addr=17,2,R"    ,vname);	clientSimulator_send_to_cli(client, cmd);
		snprintf(cmd, sizeof(cmd), "#%s|translate_addr=513,3,L"   ,vname);	clientSimulator_send_to_cli(client, cmd);
		snprintf(cmd, sizeof(cmd), "#%s|translate_addr=515,1,C,29",vname);	clientSimulator_send_to_cli(client, cmd);
	}

	sim_kfree(mem);
	NVMeshSystem_wipe_all_dirty_bits(B->sys);
	NVMeshSystem_volume_wipe_MD(sys, 0); // Make sure to clean md
	__test_dirty_convict(B);
	NVMeshSystem_all_clients_dbg_di(sys, false);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	return 0;
}

void __verify_no_dbits(struct test_context ctx){
	int sgmnt_i = 0;
	for (sgmnt_i = 0; sgmnt_i < ctx.sraid.tpr->header.n_segments; ++sgmnt_i){ // verify ram dbits
		const struct disk_range* drange = &ctx.sraid.cpr[sgmnt_i];
		struct ramDiskSimulator* ssd = &ctx.sys->servers[drange->node_id].ramDisk;
		union nvmeibc_dbits_entry *dbits;
		dbits = ssd->dbits;
		for (u32 i = 0; i < RAMDISK_DATA_LOCK_SIZE; ++i) {
			BUG_ON(dbits[i].all_bits != 0);
		}

	}

	{ // verify dmd dbits on seg 9
		u32 seg = 9;
		const struct disk_range* drange = &ctx.sraid.cpr[seg];
		struct ramDiskSimulator* ssd = &ctx.sys->servers[drange->node_id].ramDisk;
		u32 size = 128; //ramDiskSimulator_n_metadatas(ssd);

		for (u32 i = 0; i < size; i++) {
			union nvmeibc_block_dp_ec_data_block_md *md = (union nvmeibc_block_dp_ec_data_block_md *)__ptr_to_ith_md(ssd, drange->dlba_start + i);
			BUG_ON(md->P.dbits_0 != 0 && md->P.dbits_0 != 0xf);
			BUG_ON(md->P.dbits_1 != 0 && md->P.dbits_1 != 0xf);
		}
	}
}

void __dd_restore_segments_and_wipe_db_md(struct test_context env)
{
	enum NVMEIBTC_DS_MODE acm[N_MAX_RAID_SLICE_LEN] = {[0 ...N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
	struct tTopoOfPraid *tpr = env.sraid.tpr;
	tomaSimulator_switchTopoEC(tpr->header.uuid, acm, SW_TOPO__WAIT_ACK_DR, NULL);
	__dd_clean_dlba_pointers(env);
}

TEST_FUNC int unitest_DoubleDegradedMode_EC(bunitest_s *B) {
	const struct volume_segment_index vsi = {0,0,0,0};
	struct test_context env = {
			.sys = B->sys,
			.client = B->sys->clients,
			.dev = B->sys->clients->devs[vsi.volume],
			.sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi)
	};
	char iter_descript[64], *itr_txt_cur;
	u8* rbuffer = sim_kmalloc(env.sraid.cpr->slice_size*NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
	struct topologies_enumerator topos = create_no_protection_topo_enum(env.sraid);
	struct t_ec_recov_blkset blkset;
	//bool orig_ver = nvmeibc_jmd_wr_version;
	//nvmeibc_jmd_wr_version = rand()%2;				// 50% NVMEIBC_JOURNAL_MD_VERSION_PACKED, NVMEIBC_JOURNAL_MD_VERSION_UNPACKED
	//((struct nvmeibc_cinst_params_blk *)(env.dev->cips))->jentry_num_blocks = 1; // LKJ: (ofir) uncomment when transport and jam will be able to handle MS: nvmeibc_jmd_wr_version ? 1 : rand()%9;
	bunitest_tic(B);
	nvmeibc_htr_stats_reset();					// Dont care about counters of prev tests
	BUG_ON(env.dev->dp.io_perm_alert.config.unprotected_write_period == 0);

	__dd_restore_segments_and_wipe_db_md(env);
	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(B->sys, true);

	while (topos.move_next(&topos)) {
		struct topology_sgmnts_t topo = topos.curr;
		struct t_ec_recov_tx tx;
		struct io_geometry_enumerator ios = create_lock_owner_shift_slices_enum(env);
		itr_txt_cur = &iter_descript[topo_enum_tostring(&topos, iter_descript)];
		while (ios.move_next(&ios)){
			struct io_geometry io = ios.curr;
			struct topology_roles_t roles_topo = sim_convert_topo_presentations(io.traits.role2sgmnt[0] , io.traits.replicas, topo);
			u8* wbuffer = NULL; //TODO waste - adding iovec API to osSimulator_write* function will avoid memory allocation and copying

			if ((io.vlba - io.offset) != (io.traits.slba * io.traits.slice_size)){
				break;	// LKJ: Roma for Ofir, Remove this line to discover BUG in edic!
			}

			ec_tx_init_blkset(&blkset);
			bzero(&tx, sizeof(tx));
			tx.blkset = &blkset;
			if (0){//prints topo & bio; usefull for debugging enumerators
				io_geometry_enum_tostring(&ios, itr_txt_cur);
				unitest_print("%s\n", (u8*)&iter_descript[0]);
			}

			ec_tx_init(env.sraid, &blkset, &tx, NVMEIBT_RECOVERY_TYPE_STALE_REBUILD);
			tx.inp.tx_height = 1; // LKJ: uncomment: rand() % (hist->msn - tx_chain_offset) + 1;
			tx.inp.ree.aband_stg = ec_tx_abort_stage_success;
			tx.inp.nat.is_journal_overriden = true;
			tx.inp.ree.is_topo_not_eq_to_rer = false;

			tx.inp.ree.tx_bm[0] = GENMASK(tx.inp.sraid.cpr->replicas-1,0);
			tx.inp.ree.tx_bm_union = tx.inp.ree.tx_bm[0];
			array_copy(tx.inp.rer.topo, roles_topo.modes);

			if (io.vlba == 0){
				ec_tx_set_and_switch_rer_topo(env.sys, &tx, true);
			}

			tx.inp.slba = io.traits.slba;
			tx.inp.rer.tx_bm[0] = io.traits.tx_bm | nvmeibc_raid1_get_parities_bmp(env.sraid.cpr);
			tx.inp.rer.tx_height = tx.inp.tx_height; // LKJ: TODO: randomize in the future.
			tx.inp.rer.bio_type = NVMEIB_BLOCK_IO_OP_WRITE;
			tx.inp.nat.allow_bad_sectors = false;  // LKJ: to be added

			ex_tx_inject_tx(B->sys, &tx);

			wbuffer = (void*)tx.tpd.rer.blocks[0][io.offset];
			BUG_ON(osSimulator_writeArrWait(&env.client->OS, env.sraid.vsi.volume, io.vlba, io.length, wbuffer) < 0);
			BUG_ON(osSimulator_readArrWait(&env.client->OS, env.sraid.vsi.volume, io.vlba, io.length, rbuffer) < 0);
			BUG_ON(memcmp(wbuffer, rbuffer, ((env.dev->dp.enable_di_debug_mode) ? 8 : io.bytes)));		// Todo: Test first 8-byte of each block using __unitest_verify_blocks_pattern_data()

			ec_tx_verify_post_sync_tx(env.sys, &tx, false, false);
			NVMeshSystem_check_no_abandoned(env.sys);
			ec_tx_for_jmdc_do(B->sys, &tx, "Verify cleaned");
			ec_tx_free_bio_ptrs(&tx);
		}
		ec_tx_set_and_switch_rer_topo(env.sys, &tx, false);
	}

	sim_kfree(rbuffer);
	__dd_restore_segments_and_wipe_db_md(env);
	__verify_all_segments_are_readable(env.sraid.tpr);
	__fill_block_device_with_unique_data(env.sys, env.sraid.vsi.volume);
	NVMeshSystem_precondition_all_disks_for_EC(B->sys);		//not sure why we need this, the whole device is re-written with data
	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(env.sys, false);
	//nvmeibc_jmd_wr_version = orig_ver;
	unitest_print("*************** %s - done, %d[mSec]\n", __FUNCTION__, bunitest_toc(B));
	return 0;
}

TEST_FUNC int unitest_ECHotJgcRecovery(bunitest_s *B) {
	const struct volume_segment_index vsi = {0, 0, 0, 0};
	struct test_context env = {
			.sys = B->sys,
			.client = B->sys->clients,
			.dev = B->sys->clients->devs[vsi.volume],
			.sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi)
	};
	const uuid_be *my_uuid = nvmeibc_get_uuid(&env.client->p->core);
	char iter_descript[64], *itr_txt_cur;
	u8* mem = sim_kmalloc(env.sraid.cpr->slice_size * NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
	u64 magic_pattern;
	struct topologies_enumerator topos = create_topo_random_enum(env.sraid, 1, 10, 10);
	bunitest_tic(B);
	nvmeibc_htr_stats_reset();

	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(B->sys, true);

	__dd_restore_segments_and_wipe_db_md(env);
	NVMeshSystem_precondition_all_disks_for_EC(B->sys);

	while(topos.move_next(&topos)) {
		struct topology_sgmnts_t topo = topos.curr;
		struct io_geometry_enumerator ios = create_lock_owner_shift_slices_enum(env);
		tomaSimulator_switchTopoEC(r1uuid(env.sraid.tpr), topo.modes, SW_TOPO__WAIT_ACK_DR, NULL);
		NVMeshSystem_wipe_all_dirty_bits(env.sys);
		itr_txt_cur = &iter_descript[topo_enum_tostring(&topos, iter_descript)];
		while(ios.move_next(&ios)) {
			struct io_geometry io = ios.curr;
			const struct disk_range *pr = env.sraid.cpr;

			if (!io.traits.roles_bmps.has_writable_pari)
				continue; // Journal will not be used

			if (0) {
				io_geometry_enum_tostring(&ios, itr_txt_cur);
				unitest_print("%s\n", (u8*)&iter_descript[0]);
			}
			if (0){
				struct charvec buffer = {.base = malloc(1024*1024), .len = 1024*1024};
				struct jdr jdr = jdr_make(buffer);
				BUG_ON(buffer.base == NULL);
				jdr_write_topology_sgmnts(&jdr, "topo", &topo);
				jdr_write_io_geometry(&jdr, "io", &io);
				unitest_print("%s\n", jdr_finalize(&jdr).base);
				free(buffer.base);
			}

			{
				u32 committed_tx_id = nvmeibc_get_commited_txid(env, io.traits);
				ulong tx_bm = io.traits.tx_bm | nvmeibc_raid1_get_parities_bmp(pr);

				NVMeshSystem_check_no_abandoned(env.sys);

				for (raid_sgmnt_t di = 0; di < pr->replicas; di++) {
					if (topo.modes[di] != NVMEIBTC_DS_MODE_RW ||
							!test_bit(io.traits.sgmnt2role[di], &tx_bm)) {
						continue;
					}
					else {
						const u64 dlba = io.traits.sgmnt2dlba[di];
						struct serverSimulator *srv = &env.sys->servers[pr[di].node_id];
						const int jri = nvmeibs_get_jri_by_uuid(srv, my_uuid);
						const int jentry = nvmeibc_jam_simu_alloc_entry(srv->disk, dlba, committed_tx_id, false);

						// Inject JMD+JMDC TxID+J2D
						struct block_inject_ptrs inj = serverSimulator_get_block_inject_ptrs(srv, dlba, jri, jentry, 0);
						nvmeibc_block_dp_ec_jmd_encode(inj.jmd, dlba, (committed_tx_id + 1), tx_bm, 0, 0);
						inj.ram.jmdc->raw = inj.jmd->raw;
						nvmeibs_nordda_jour_entry_do(srv, jri, jentry, "Abandon Entry");
					}
				}
			}

			magic_pattern = __unitest_fill_blocks_unique_pattern(mem, io.length);
			BUG_ON(osSimulator_writeArrWait(&env.client->OS, env.sraid.vsi.volume, io.vlba, io.length, mem) < 0);
			clientSimulator_wait_for_all_sync_ops(env.client);
			NVMeshSystem_check_no_abandoned(env.sys);
			BUG_ON(osSimulator_readArrWait(&env.client->OS, env.sraid.vsi.volume, io.vlba, io.length, mem) < 0);
			__unitest_verify_blocks_pattern(mem, io.length, magic_pattern, false);
		}
	}


	__dd_restore_segments_and_wipe_db_md(env);
	__verify_all_segments_are_readable(env.sraid.tpr);
	__fill_block_device_with_unique_data(env.sys, env.sraid.vsi.volume);
	NVMeshSystem_precondition_all_disks_for_EC(B->sys);

	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(env.sys, false);

	sim_kfree(mem);
	unitest_print("*************** %s - done, %d[mSec]\n", __FUNCTION__, bunitest_toc(B));
	return 0;
}

/**
 * Tests the scenarion described in EC-4697
 * Assume we have 2 segments D and P. Lets assume P is parity.
 *    D is DEAD.
 *    Write on D. D becomes dirty. On P, it is written in metadata that D is dirty.
 *    P is DEAD. No writes. Only D is dirty.
 *    D is ALIVE. P still DEAD.
 *    Dirty bits recovery. D is recovered. Dirty bits are off in RAM and all all segments on disk. Except P because it is DEAD.
 *    D becomes RW, P is alive.
 *    Another dirty bits recovery.
 *       Expected: dirty bits are cleared from D on P's disk metadata
 *       Bad: All segments are clean in RAM, but on disk P still thinks D is dirty
 */
TEST_FUNC int unitest_ECDbitsOnDisk(bunitest_s* B) {
	raid_sgmnt_t Di = 0, Pi = 8, Qi = 9; /* Indices of D and P + Q(second parity) */
	int vol_idx = 0; /* Use volume 0. Because why not. */
	struct test_context env = {.sys    = B->sys,
	                           .client = B->sys->clients,
	                           .dev    = B->sys->clients->devs[0],
	                           .sraid =
	                               NVMeshSystem_TstPRaid_init_rel(B->sys, (struct volume_segment_index){vol_idx, 0, 0, 0})};
	struct tTopoOfPraid *r1 = env.sraid.tpr;
	struct disk_range *dr = env.sys->mdb.vols[vol_idx].segs;

	/* This is for IOs */
	const size_t mem_size = NVMEIBC_SECTOR_SIZE; /* All IOs in this test will be 1 block exactly */
	u8 *mem = sim_kmalloc(mem_size, GFP_KERNEL); /* Allocate mem */
	u64 magic = __unitest_fill_blocks_unique_pattern(mem, 1); /* Fill magic pattern */


	/* Injection ptrs and all that stuff */
	struct io_traits io_t = get_io_traits(env, 0, env.sraid.cpr->slice_size);
	struct block_inject_ptrs D_ptrs = serverSimulator_get_block_inject_ptrs(&env.sys->servers[Di], io_t.slices->sgmnt2dlba[Di], 0, 0, 0);
	struct block_inject_ptrs P_ptrs = serverSimulator_get_block_inject_ptrs(&env.sys->servers[Pi], io_t.slices->sgmnt2dlba[Pi], 0, 0, 0);
	struct block_inject_ptrs Q_ptrs = serverSimulator_get_block_inject_ptrs(&env.sys->servers[Qi], io_t.slices->sgmnt2dlba[Qi], 0, 0, 0);

	/* Kill D */
	__degrade_segment(&env.sys->clients[0], r1, dr, Di);

	/* Do 1 write to D and verify */
	REPORT_ERROR(osSimulator_writeArrWait(&env.client->OS, vol_idx, 0, 1, mem));
	memset(mem, 0, mem_size);
	REPORT_ERROR(osSimulator_readArrWait(&env.client->OS, vol_idx, 0, 1, mem));
	__unitest_verify_blocks_pattern(mem, 1, magic, false);

	/* Kill P */
	__degrade_segment(&env.sys->clients[0], r1, dr, Pi);

	/* Revive D */
	__restore_seg_to_write(env.sys, &env.sys->clients[0], r1, dr, Di, true);

	/* Run recovery on D */
	BUG_ON(tomaSimulator_recoverThing(r1, &dr[Qi], RCVR_DIRTY_REBUILD) < 0);
	__restore_seg_to_read_write(env.sys, r1, Di);

	/* Revive P */
	__restore_seg_to_write(env.sys, &env.sys->clients[0], r1, dr, Pi, true);

	/* Run second recovery */
	BUG_ON(P_ptrs.dmd->P.dbits_0 != Di + 1); /* Expect dirty bits on D in P metadata */
	BUG_ON(D_ptrs.ram.dbits->slmod.dead0 != Pi + 1); /* Expect dirty bits on P in D ram */
	BUG_ON(Q_ptrs.dmd->P.dbits_0 != Pi + 1); /* Expect dirty bits on P in Q metadata */
	BUG_ON(tomaSimulator_recoverThing(r1, &dr[Di], RCVR_DIRTY_REBUILD) < 0);

	/* Verify expected */
	BUG_ON(P_ptrs.dmd->P.dbits_0 || P_ptrs.dmd->P.dbits_1); /* Not dirty bits on P in metadata */
	BUG_ON(Q_ptrs.dmd->P.dbits_0 || Q_ptrs.dmd->P.dbits_1); /* Not dirty bits on Q in metadata */
	BUG_ON(D_ptrs.ram.dbits->all_bits); /* No dirty bits in ram */

	/* Cleanup */
	__restore_seg_to_read_write(env.sys, r1, Pi);
	free_io_traits(&io_t);
	sim_kfree(mem);

	return 0;
}
