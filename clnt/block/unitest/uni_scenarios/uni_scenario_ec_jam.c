#include "uni_scenario_ec_jam.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "block/nvmeibc_block_common.h"
#include "uni_framework/simu_test.h"
#include "uni_scenario_common.h"
#include "uni_framework/range_algorithms.h"
#include "core/nvmeibc_core_common.h"
#include "nvmesh_sim.h"

#include "nvmeibc_jam.h"

/**
 * Data structures
 */

struct jam_test_ctx {
	int vol_idx;
	struct test_context env;
	int slice_size;
	int replicas;
	int mem_size;
	u8 *mem;
	u64 magic_pattern;
	struct jam_simu_stats stats[N_MAX_RAID_SLICE_LEN];
};

#define stats_event_occurred(sys, ctx, server, event)                                                                  \
	(nvmeibc_get_jam_simu_stats(sys->servers[server].disk).event > ctx->stats[server].event)

static void refresh_jam_stats(struct jam_test_ctx *ctx) {
	u32 i;
	for (i = 0; i < ctx->env.sraid.cpr->replicas; ++i) {
		ctx->stats[i] = nvmeibc_get_jam_simu_stats(ctx->env.sys->servers[i].disk);
	}
}

/**
 * Initialize jam_test_ctx struct with default values, suitable for most test cases
 */
static inline struct jam_test_ctx *init_jam_test_ctx(struct NVMeshSystem *sys) {
	struct jam_test_ctx *ctx = sim_kzalloc(sizeof(struct jam_test_ctx), GFP_KERNEL);
	ctx->vol_idx = 0;
	ctx->env = init_test_context(sys, ctx->vol_idx, 0, 0, 0);
	ctx->slice_size = ctx->env.sraid.cpr->slice_size;
	ctx->replicas = ctx->env.sraid.cpr->replicas;
	ctx->mem_size = ctx->slice_size * NVMEIBC_SECTOR_SIZE;
	ctx->mem = sim_kmalloc(ctx->mem_size, GFP_KERNEL);
	ctx->magic_pattern = __unitest_fill_blocks_unique_pattern(ctx->mem, ctx->slice_size);

	refresh_jam_stats(ctx);

	return ctx;
}

/**
 * Free ctx resources
 */
static inline void destroy_jam_test_ctx(struct jam_test_ctx *ctx) {
	sim_kfree(ctx->mem);
	sim_kfree(ctx);
}


/**
 * Test scenarios section
 */

TEST_FUNC int unitest_jam_AsyncJAM(struct NVMeshSystem *sys) {
	int rv = 0;
	struct jam_test_ctx *ctx = init_jam_test_ctx(sys);
	int sgmnt2entry0[N_MAX_RAID_SLICE_LEN];
	u64 n_stale_before, n_stale_after;

	NVMeshSystem__verify_all_serjios_are_clean(sys);

	unitest_trace_checkpoint(unitest_AsyncJAM, part_1, "");

	n_stale_before = dp_io_stats_get_counter(&ctx->env.dev->dp.io_stats, DP_IO_STATS_LOCK_STALE_COUNT);
	// allocate & commit journal entries
	nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0xFF);
	nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);
	// allocate once again
	nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0xFF);

	// start the write, it will stalled since the entries are allocated
	rv |= osSimulator_writeArr(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);
	// return the entries, so the I/O can proceed
	nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);

	// Verify all writes have completed
	clientSimulator_wait_for_all_bio_ops(ctx->env.client);

	// Verify that the data was written
	memset(&ctx->mem[0], 0, ctx->mem_size);
	rv |= osSimulator_readArrWait(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(ctx->mem, ctx->slice_size, ctx->magic_pattern,
									false); // Verify that read and write matched.

	if (1) {

		unitest_trace_checkpoint(unitest_AsyncJAM, part_2, "");

		// Take last entry again, this time cause pause on same disk, and verify that stale occurs
		ctx->magic_pattern = __unitest_fill_blocks_unique_pattern(ctx->mem, ctx->slice_size);

		// Adandon the JAM entry again - so it will not be allocated
		nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0xFF);
		nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);
		nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0x1);

		// Write again to both addresses and cause stale on both locks during pause
		rv |= osSimulator_writeArr(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
		REPORT_ERROR(rv);
		{ // Pause disk while waiting for JAM allocation, will cause empty HTR on both addresses but HTR of address
		  // 42 will release all journal entries and allow both writes to complete
			NVMeshSystem__invoke_pause_cont_on_disk(sys, 0);

			clientSimulator_wait_for_all_bio_ops(ctx->env.client);
			memset(&ctx->mem[0], 0, ctx->mem_size);
		}
		// Verify Writes op completed and wrote
		rv |= osSimulator_readArrWait(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
		REPORT_ERROR(rv);
		__unitest_verify_blocks_pattern(ctx->mem, ctx->slice_size, ctx->magic_pattern, false); // Verify that read and write matched.
		nvmeibc_wait_for_all_jam_entires_to_be_free(sys, &ctx->env.sraid, NULL /*PAUSE done above*/);
	}

	n_stale_after = dp_io_stats_get_counter(&ctx->env.dev->dp.io_stats, DP_IO_STATS_LOCK_STALE_COUNT);
	BUG_ON(n_stale_after <= n_stale_before); // Verify that stale count increased

	// Cleanup
	nvmeibc_jam_simu_verify_cleand_all_servers(sys);
	destroy_jam_test_ctx(ctx);
	return rv;
}

TEST_FUNC int unitest_jam_PendingResume(struct NVMeshSystem *sys) {
	int rv = 0;
	struct jam_test_ctx *ctx = init_jam_test_ctx(sys);
	int sgmnt2entry0[N_MAX_RAID_SLICE_LEN];

	NVMeshSystem__verify_all_serjios_are_clean(sys);

	// allocate & commit journal entries
	nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0xFF);
	nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);
	// allocate once again
	nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0xFF);

	// Refetch statistics
	refresh_jam_stats(ctx);

	// start the write, it will stalled since the entries are allocated
	rv |= osSimulator_writeArr(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);

	// wait for timeout to occur (wait for sufficienty longer than minimal timeout)
	msleep(jiffies_to_msecs(nvmeibc_jam_pending_req_timeout_jif) * 2);

	// Verify we had timeout
	BUG_ON(!stats_event_occurred(sys, ctx, 0, n_pending_timeout));

	// return the entries, so the I/O can proceed
	nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);

	// Verify all writes have completed
	clientSimulator_wait_for_all_bio_ops(ctx->env.client);

	// Verify that the data was written
	memset(&ctx->mem[0], 0, ctx->mem_size);
	rv |= osSimulator_readArrWait(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(ctx->mem, ctx->slice_size, ctx->magic_pattern,
									false); // Verify that read and write matched.

	// Cleanup
	nvmeibc_jam_simu_verify_cleand_all_servers(sys);
	destroy_jam_test_ctx(ctx);
	return rv;
}

enum rollback_flow {
	JAM_UNITEST_ROLLBACK_FIRST,
	JAM_UNITEST_ROLLBACK_TIMEOUT,
	JAM_UNITEST_ROLLBACK_PAUSE,
	JAM_UNITEST_ROLLBACK_LAST
};

TEST_FUNC int unitest_jam_RollbackPartial(struct NVMeshSystem *sys) {
	int rv = 0;
	const int alloc_segment = 1;
	struct jam_test_ctx *ctx = init_jam_test_ctx(sys);
	int sgmnt2entry0[N_MAX_RAID_SLICE_LEN];

	// There are two possible flows to cause rollback: timeout and pause. We want to test both.
	enum rollback_flow test_flow;

	NVMeshSystem__verify_all_serjios_are_clean(sys);

	for (test_flow = JAM_UNITEST_ROLLBACK_FIRST + 1; test_flow < JAM_UNITEST_ROLLBACK_LAST; ++test_flow) {

		if (test_flow == JAM_UNITEST_ROLLBACK_TIMEOUT)
			unitest_trace_checkpoint(unitest_jam_RollbackPartial, Timeout_flow, "");
		if (test_flow == JAM_UNITEST_ROLLBACK_PAUSE)
			unitest_trace_checkpoint(unitest_jam_RollbackPartial, Pause_flow, "");

		// allocate & commit journal entries
		nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0xFF);
		nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);
		// allocate once again, only second segment
		nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 1 << alloc_segment);

		// Refetch statistics
		refresh_jam_stats(ctx);

		// start the write, it will successfully allocate first segment,
		// then stalled since the second one is allocated
		rv |= osSimulator_writeArr(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
		REPORT_ERROR(rv);

		if (test_flow == JAM_UNITEST_ROLLBACK_TIMEOUT) {
			// wait for timeout to occur (wait for sufficienty than minimal timeout)
			msleep(jiffies_to_msecs(nvmeibc_jam_pending_req_timeout_jif) * 2);
			// return the entries, so the I/O can proceed
			nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);
			// Verify all writes have completed
			clientSimulator_wait_for_all_bio_ops(ctx->env.client);

			// Verify we had timeout
			BUG_ON(!stats_event_occurred(sys, ctx, alloc_segment, n_pending_timeout));
		}

		if (test_flow == JAM_UNITEST_ROLLBACK_PAUSE) {
			// Pause / cont on disk we did allocate, this will cause rollback
			NVMeshSystem__invoke_pause_cont_on_disk(sys, 0);
			// Verify we did not have timeouts
			BUG_ON(stats_event_occurred(sys, ctx, alloc_segment, n_pending_timeout));
			// Release entries that are blocking the IO
			nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);
			// Verify all writes have completed
			clientSimulator_wait_for_all_bio_ops(ctx->env.client);

			// RRRR: Currently JAM does not behave in an optimal way. Instead of rollback in case of pause
			// it will abandon partial journal allocations. Fix this in the future in JAM code, then
			// remove this cleanup.
			BUG_ON(tomaSimulator_recoverThing(ctx->env.sraid.tpr, ctx->env.sraid.cpr, RCVR_EC_JOUR_GC) < 0);
		}

		// Verify that the data was written
		memset(&ctx->mem[0], 0, ctx->mem_size);
		rv |= osSimulator_readArrWait(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
		REPORT_ERROR(rv);
		__unitest_verify_blocks_pattern(ctx->mem, ctx->slice_size, ctx->magic_pattern, false); // Verify that read and write matched.
		nvmeibc_wait_for_all_jam_entires_to_be_free(sys, &ctx->env.sraid, NULL /*PAUSE done above*/);
	}

	// Cleanup
	nvmeibc_jam_simu_verify_cleand_all_servers(sys);
	destroy_jam_test_ctx(ctx);
	return rv;
}

// Test 2 async alloc phases totaling less (half) of the JAM timeout
// JAM timeout flow is already covered in unitest_jam_RollbackPartial()
TEST_FUNC int unitest_jam_NoTimeout(struct NVMeshSystem *sys) {
	int rv = 0;
	struct jam_test_ctx *ctx = init_jam_test_ctx(sys);
	int alloc_segments[2];
	int sgmnt2entry0[N_MAX_RAID_SLICE_LEN];
	int i;

	alloc_segments[0] = rand() % ctx->replicas;
	alloc_segments[1] = (alloc_segments[0] + 1 + (rand() % (ctx->replicas - 1))) % ctx->replicas;
	BUG_ON(alloc_segments[0] == alloc_segments[1]);

	NVMeshSystem__verify_all_serjios_are_clean(sys);

	// We do not know what is the JAM alloc order of the segments' disks, so try both:
	for (int segs_order = 0; segs_order < 2; segs_order++) {

		if (segs_order)
			swap(alloc_segments[0], alloc_segments[1]);

		// allocate & commit journal entries
		nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, ~(sgmnts_bmp_t)0);
		nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_END_USE);
		// allocate once again, only the selected segments
		nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, (1 << alloc_segments[0]) | (1 << alloc_segments[1]));

		// Refetch statistics
		refresh_jam_stats(ctx);

		// start the write, it will stall on allocating one of the 2 already allocated segments
		rv |= osSimulator_writeArr(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
		REPORT_ERROR(rv);

		for (i = 0; i < (int)ARRAY_SIZE(alloc_segments); i++) {
			int sgmnt2entry_i[N_MAX_RAID_SLICE_LEN];
			// wait before returning each entry, for a total of half the JAM timeout
			msleep(jiffies_to_msecs(nvmeibc_jam_pending_req_timeout_jif / 4));
			// return the entry
			array_fill(sgmnt2entry_i, ILLEGAL_JRNL_ENTRY);
			sgmnt2entry_i[alloc_segments[i]] = sgmnt2entry0[alloc_segments[i]];
			nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry_i, NVMEIBC_JIDX_EVT_END_USE);
		}

		// All entries were returned, I/O can now proceed
		clientSimulator_wait_for_all_bio_ops(ctx->env.client);

		// Verify we had no timeout
		for (i = 0; i < (int)ARRAY_SIZE(alloc_segments); i++) {
			BUG_ON(stats_event_occurred(sys, ctx, alloc_segments[i], n_pending_timeout));
		}

		// Verify that the data was written
		memset(&ctx->mem[0], 0, ctx->mem_size);
		rv |= osSimulator_readArrWait(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
		REPORT_ERROR(rv);
		__unitest_verify_blocks_pattern(ctx->mem, ctx->slice_size, ctx->magic_pattern, false); // Verify that read and write matched.
		nvmeibc_wait_for_all_jam_entires_to_be_free(sys, &ctx->env.sraid, NULL);
	}

	// Cleanup
	nvmeibc_jam_simu_verify_cleand_all_servers(sys);
	destroy_jam_test_ctx(ctx);
	return rv;
}

int gen_cmd_completion(struct nvmeibc_disk_hook_args *args, struct nvmeibc_disk_gen_cmd *gen_cmd) {
	if (gen_cmd->opcode == NVMEIB_GEN_OP_JENTRY_ERASE) {
		wait_for_completion(&args->wait_event);
	}
	return 0; /*Continue execution*/
}

TEST_FUNC int unitest_jam_BoundToTransient(struct NVMeshSystem *sys) {
	int rv = 0;

	struct jam_test_ctx *ctx = init_jam_test_ctx(sys);
	int sgmnt2entry0[N_MAX_RAID_SLICE_LEN];

	struct nvmeibc_disk_hooks disk_hooks = {.gen_cmd_completion = gen_cmd_completion};

	init_completion(&disk_hooks.args.wait_event);

	NVMeshSystem__verify_all_serjios_are_clean(sys);

	// allocate some entries, making sure to use FUTURE txid
	nvmeibc_alloc_jrnl_entries_for_io(ctx->env, 0, ctx->slice_size, sgmnt2entry0, 0xFF);

	// Setup hooks to delay NVMEIBC_JIDX_EVT_ERASE completion on *some* disk
	nvmeibc_disk_hooks_setup(sys->servers[0].disk, &disk_hooks);

	// now erase entries, making them bound by transient, so the I/O can proceed
	nvmeibc_process_jrnl_entries_for_io(ctx->env, sgmnt2entry0, NVMEIBC_JIDX_EVT_ERASE);

	refresh_jam_stats(ctx);

	// This write must be sync. We must ensure an attempt to allocate journal is done before
	// erase completion, else jam choses other flow, and the flow we want to test is not tested.
	nvmeibc_backup_switch_sync_mode(true);

	// start the write, it will stalled since the entries are allocated
	rv |= osSimulator_writeArr(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);

	nvmeibc_restore_sync_mode(); // End of critical section

	// Now we can release the delayed NVMEIBC_JIDX_EVT_ERASE completion
	complete(&disk_hooks.args.wait_event);

	// We should have passed via bound to transient flow
	BUG_ON(!stats_event_occurred(sys, ctx, 0, n_bound_to_transient));

	// Hooks no longer needed
	nvmeibc_disk_hooks_clean(sys->servers[0].disk);

	// Verify all writes have completed
	clientSimulator_wait_for_all_bio_ops(ctx->env.client);

	// Verify that the data was written
	memset(&ctx->mem[0], 0, ctx->mem_size);
	rv |= osSimulator_readArrWait(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(ctx->mem, ctx->slice_size, ctx->magic_pattern,
									false); // Verify that read and write matched.

	// Cleanup
	nvmeibc_jam_simu_verify_cleand_all_servers(sys);
	destroy_jam_test_ctx(ctx);
	return rv;
}

TEST_FUNC int unitest_jam_EmptyFreeList(struct NVMeshSystem *sys) {
	int rv = 0;
	const u32 txid = (rand() % (NVMEIBC_DP_EC_MD_TX_ID_MAX-1)) + 2; // [0,1] are reserved. Random value between [2..NVMEIBC_DP_EC_MD_TX_ID_MAX]
	struct jam_test_ctx *ctx = init_jam_test_ctx(sys);
	int *io2sgmnt2entry;

	NVMeshSystem__verify_all_serjios_are_clean(sys);

	// This is used to validate we actually entered pending on empty flow. Save previous value.
	refresh_jam_stats(ctx);
	io2sgmnt2entry = nvmeibc_drain_free_jrnls(ctx->env, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, txid);		// Fully drain the free list

	// This is critical for test case coverage that the first write arrives while free list is
	// still drained, so this is crictical section that must be sync.
	nvmeibc_backup_switch_sync_mode(true);

	// start the write, it will stalled since the entries are allocated
	rv |= osSimulator_writeArr(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);

	nvmeibc_restore_sync_mode(); // End of critical section

	nvmeibc_release_drained_jrnls(ctx->env, io2sgmnt2entry, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, NVMEIBC_JIDX_EVT_END_USE);	// Release free list
	clientSimulator_wait_for_all_bio_ops(ctx->env.client);	// Verify all writes have completed

	// Verify that pending on empty occurred
	BUG_ON(!stats_event_occurred(sys, ctx, 0, n_pending_on_empty));

	// Verify that the data was written
	memset(&ctx->mem[0], 0, ctx->mem_size);
	rv |= osSimulator_readArrWait(&ctx->env.client->OS, ctx->vol_idx, 0, ctx->slice_size, ctx->mem);
	REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(ctx->mem, ctx->slice_size, ctx->magic_pattern,
									false); // Verify that read and write matched.

	// Cleanup
	nvmeibc_jam_simu_verify_cleand_all_servers(sys);
	destroy_jam_test_ctx(ctx);

	return rv;
}

TEST_FUNC int unitest_jam_Prints(struct NVMeshSystem *sys) {
	struct nvmeibc_disk* d0 = sys->servers[0].disk;
	int rv = 0;
	char buf[PAGE_SIZE];
	nvmeibc_jam_fill_status(nvmeibc_cinst_get_core_p(d0), buf, sizeof(buf));
	nvmeibc_jam_fill_disk_status(d0, buf, sizeof(buf));
	return rv;
}


/**
 * When adding new test scenarios, add their invocation here
 */

int unitest_jam_AllTests(struct NVMeshSystem *sys) {
	int rv = 0;

	NVMeshSystem_all_clients_dbg_di(sys, true);

	rv |= SIMU_RUN_TEST(unitest_jam_AsyncJAM, sys);
	rv |= SIMU_RUN_TEST(unitest_jam_PendingResume, sys);
	rv |= SIMU_RUN_TEST(unitest_jam_RollbackPartial, sys);
	rv |= SIMU_RUN_TEST(unitest_jam_NoTimeout, sys);
	rv |= SIMU_RUN_TEST(unitest_jam_BoundToTransient, sys);
	rv |= SIMU_RUN_TEST(unitest_jam_EmptyFreeList, sys);
	rv |= SIMU_RUN_TEST(unitest_jam_Prints, sys);

	nvmeibc_jam_simu_verify_cleand_all_servers(sys);

	return rv;
}
