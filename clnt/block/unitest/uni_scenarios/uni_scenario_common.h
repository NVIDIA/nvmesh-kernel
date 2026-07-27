#ifndef UNI_SCENARIO_COMMON_H
#define UNI_SCENARIO_COMMON_H

/**
 * Common utility functions shared between multiple scenarios
 */

#include "../bunitest.h"
#include "nvmeibc_jam.h"
#include "nvmesh_sim.h"

#define init_test_context(sys, volume, chunk, raid, segment)                                                           \
	(struct test_context) {                                                                                            \
		.sys = sys, .client = sys->clients, .dev = sys->clients->devs[volume],                                         \
		.sraid = NVMeshSystem_TstPRaid_init_rel(sys, (struct volume_segment_index){volume, chunk, raid, segment})      \
	}

void nvmeibc_ensure_jam_has_nothing_bound(struct test_context env);

extern const u64 ILLEGAL_DLBA;
extern const int ILLEGAL_JRNL_ENTRY;

void nvmeibc_guess_to_be_written_dlbas_for_slice(struct io_traits io_traits, u16 slice_idx,
												 u64 sgmnt2dlba[N_MAX_RAID_SLICE_LEN]);
void nvmeibc_guess_to_be_written_dlbas_for_io(struct test_context env, struct io_traits io_traits,
											  u64 sgmnt2dlba[N_MAX_RAID_SLICE_LEN]);
u64 vlba2blockset(struct test_context env, u64 vlba);
u32 nvmeibc_get_commited_txid(struct test_context env, struct slice_traits slice_traits);
void nvmeibc_wait_for_all_jam_entires_to_be_free(struct NVMeshSystem *sys, struct TstPRaid *r, struct nvmeibc_topology *t);
void nvmeibc_process_jrnl_entries_for_io(struct test_context env, const int sgmnt2entry[N_MAX_RAID_SLICE_LEN],
										 enum nvmeibc_jam_jidx_event event);
void nvmeibc_alloc_jrnl_entries_for_io(struct test_context env, u64 vlba, u64 nblocks,
									   int sgmnt2entry[N_MAX_RAID_SLICE_LEN], sgmnts_bmp_t sgmnts_bmps);

/**
 * Verify jam is cleaned on all servers
 */
void nvmeibc_jam_simu_verify_cleand_all_servers(struct NVMeshSystem *sys);

/**
 * Inject the given hash function into any valid jam disk
 *
 * Pay attention that it may be dangerous playing around with hash. It may result in 2 jidx allocated
 * for same hkey. It is OK as long as it is RAM only (alloc / release), but may become a problem if
 * actually writing to the disk, as it may confuse recovery. Use with caution.
 *
 */
void nvmeibc_inject_jam_hash64(struct test_context env, u64 (*hash64)(u64, unsigned int));

/**
 * At the end of this function, JAM free_list will be empty.
 * The flow:
 * 1. Replace hash_function on all disks with predictable function
 * 2. Allocate @max_free_list entries
 * 3. Return @io2sgmnt2entry array
 *
 * If txid passed is valid, use it. If not, use the last committed txid for each IO
 *
 */
int *nvmeibc_drain_free_jrnls(struct test_context env, u32 max_free_list, u32 txid);

/**
 * The inverse of nvmeibc_drain_free_jrnls
 * The flow:
 * 1. Process `end of usage` event for each drained entry
 * 2. Return previous hash function
 * 3. Release resources
 */
void nvmeibc_release_drained_jrnls(struct test_context env, const int *io2sgmnt2entry, u32 max_free_list, enum nvmeibc_jam_jidx_event jam_event);

/* Backup current simulator syn mode, and switch to the new one (if required). Can be used for critical sections in tests that must run synchronously. */
void nvmeibc_backup_switch_sync_mode(bool sync);

/* Restore previously backed up sync mode. */
void nvmeibc_restore_sync_mode(void);

/****************************
 * Resusable hash functions *
 ****************************/

/**
 * Use j2d % table_size as hash.
 */
u64 __hash_j2d(u64 key, unsigned int bits);

struct nvmeibc_roles_bmps nvmeibc_sim_init_roles_bmps(const struct disk_range *pr, enum NVMEIBTC_DS_MODE topo[N_MAX_RAID_SLICE_LEN], u32 slice_start_seg);

#endif /*UNI_SCENARIO_COMMON_H*/
