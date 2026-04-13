/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * uni_scenario_tpv.c — TPV/CDV unit-test scenarios.
 *
 * Each test is self-contained:
 *   1. tpv_test_setup()   — allocate CDV sim + mock CDV volume + attach TPV
 *   2. body               — exercise the feature under test
 *   3. tpv_test_teardown()— detach TPV + destroy CDV vol + destroy CDV sim
 *
 * No NVMeshSystem is required.  The sys argument is accepted (and ignored)
 * so that SIMU_RUN_TEST() can call these functions uniformly.
 *
 * Test geometry (from nvmeibc_tpv_simu.h):
 *   T = 64 KiB, E = 1 MiB, A = 0, virtual = 4 MiB (64 extents)
 *   4 data CDV extents (indices 1..4), 16 slots each → 64 slots total
 */

#include "common/kr_incs.h"
#include "clnt/tpv/nvmeibc_tpv.h"
#include "tpv/nvmeibc_tpv_simu.h"
#include "uni_scenarios/uni_scenario_tpv.h"
#include "nvmesh_sim.h"

/* ── Internal assertion helper ───────────────────────────────────────────── */

#define TPV_CHECK(cond, fmt, ...)					\
	do {								\
		if (!(cond)) {						\
			pr_err("TPV_TEST FAIL [%s:%d]: " fmt "\n",	\
			       __func__, __LINE__, ##__VA_ARGS__);	\
			rv = -1;					\
		}							\
	} while (0)

/* ── Per-test context ────────────────────────────────────────────────────── */

struct tpv_test_ctx {
	struct tpv_cdv_sim    *sim;
	struct nvmeibc_volume *cdv;
	struct nvmeibc_tpv    *tpv;
};

/*
 * tpv_test_setup — create CDV simulator, mock CDV volume, and attach a fresh
 * TPV.  Does NOT set the allocator TOMA ID (so cdv_alloc_work defers); call
 * tpv_simu_set_toma_id() and tpv_simu_fill_pool() when needed.
 */
static int tpv_test_setup(struct tpv_test_ctx *ctx)
{
	ctx->sim = tpv_cdv_sim_create();
	if (!ctx->sim) {
		pr_err("TPV_TEST: tpv_cdv_sim_create failed\n");
		return -ENOMEM;
	}

	ctx->cdv = tpv_cdv_vol_create();
	if (!ctx->cdv) {
		pr_err("TPV_TEST: tpv_cdv_vol_create failed\n");
		tpv_cdv_sim_destroy();
		return -ENOMEM;
	}

	ctx->tpv = nvmeibc_tpv_attach(
		ctx->cdv,
		TPV_SIMU_TPV_NAME,
		TPV_SIMU_TPV_UUID,
		TPV_SIMU_VIRTUAL_MB * 1024ULL * 1024ULL,	/* virtual_size_bytes */
		(u32)TPV_SIMU_TPV_EXTENT_KB,
		(u32)TPV_SIMU_CDV_EXTENT_MB,
		(u64)TPV_SIMU_ALLOC_GB);
	if (!ctx->tpv) {
		pr_err("TPV_TEST: nvmeibc_tpv_attach failed\n");
		tpv_cdv_vol_destroy(ctx->cdv);
		tpv_cdv_sim_destroy();
		return -EINVAL;
	}

	/*
	 * Drain the cdv_alloc_work that attach schedules (pool empty).
	 * Without a TOMA ID it exits immediately, clearing cdv_alloc_pending.
	 */
	flush_workqueue(system_wq);
	return 0;
}

static void tpv_test_teardown(struct tpv_test_ctx *ctx)
{
	if (ctx->tpv) {
		nvmeibc_tpv_detach(ctx->tpv);
		/* tpv is freed by detach */
		ctx->tpv = NULL;
	}
	tpv_cdv_vol_destroy(ctx->cdv);
	ctx->cdv = NULL;
	tpv_cdv_sim_destroy();
	ctx->sim = NULL;
}

/* ── Helper: count mapped extents in xarray ─────────────────────────────── */

static u64 tpv_test_count_mapped(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator    *alloc = &tpv->allocator;
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;
	u64 count = 0;

	xa_for_each(&alloc->extent_map, idx, entry)
		count++;
	return count;
}

/* ── unitest_tpv_alloc_free ─────────────────────────────────────────────── */

/*
 * Basic alloc / free round-trip.
 *
 * 1. After fill_pool, free_tpv_extent_count == N_DATA_EXT × n_slots.
 * 2. Allocating N extents decrements the pool by N.
 * 3. stat_tpv_alloc_ok == N after N successful allocations.
 * 4. The phys_offset of each allocation falls within a valid CDV data extent.
 * 5. Freeing all N restores stat_tpv_free_ok.
 */
TEST_FUNC int unitest_tpv_alloc_free(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_allocator *alloc;
	struct nvmeibc_tpv_extent_entry *entry;
	u64 n_alloc = 8;
	u64 i;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);
	tpv_simu_fill_pool(ctx.tpv);

	alloc = &ctx.tpv->allocator;

	/* ── 1. Verify pool size after fill ── */
	TPV_CHECK(alloc->free_tpv_extent_count ==
		  (u64)TPV_SIMU_N_DATA_EXT * TPV_SIMU_N_SLOTS,
		  "expected %llu free slots, got %llu",
		  (u64)TPV_SIMU_N_DATA_EXT * TPV_SIMU_N_SLOTS,
		  alloc->free_tpv_extent_count);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_cdv_alloc_ok) ==
		  (s64)TPV_SIMU_N_DATA_EXT,
		  "expected %d CDV alloc_ok, got %lld",
		  TPV_SIMU_N_DATA_EXT,
		  (s64)atomic64_read(&alloc->stat_cdv_alloc_ok));

	/* ── 2. Allocate n_alloc virtual extents ── */
	for (i = 0; i < n_alloc; i++) {
		int arc = nvmeibc_tpv_alloc_extent(ctx.tpv, i, &entry);

		TPV_CHECK(arc == 0, "alloc_extent(%llu) returned %d", i, arc);
		if (arc != 0)
			continue;

		/* phys_offset must lie within a data CDV extent */
		TPV_CHECK(entry->phys_offset >= TPV_SIMU_E_BYTES,
			  "virt=%llu phys=0x%llx below first data extent",
			  i, entry->phys_offset);
		TPV_CHECK(entry->phys_offset < TPV_SIMU_CDV_BYTES,
			  "virt=%llu phys=0x%llx beyond CDV end",
			  i, entry->phys_offset);
		TPV_CHECK(entry->cdv_extent_index >= 1 &&
			  entry->cdv_extent_index <= (u64)TPV_SIMU_N_DATA_EXT,
			  "virt=%llu bad cdv_extent_index=%llu",
			  i, entry->cdv_extent_index);
	}

	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_alloc_ok) == (s64)n_alloc,
		  "stat_tpv_alloc_ok: expected %llu got %lld",
		  n_alloc, (s64)atomic64_read(&alloc->stat_tpv_alloc_ok));

	TPV_CHECK(tpv_test_count_mapped(ctx.tpv) == n_alloc,
		  "xarray has %llu entries, expected %llu",
		  tpv_test_count_mapped(ctx.tpv), n_alloc);

	TPV_CHECK(alloc->free_tpv_extent_count ==
		  (u64)TPV_SIMU_N_DATA_EXT * TPV_SIMU_N_SLOTS - n_alloc,
		  "pool after %llu allocs: expected %llu, got %llu",
		  n_alloc,
		  (u64)TPV_SIMU_N_DATA_EXT * TPV_SIMU_N_SLOTS - n_alloc,
		  alloc->free_tpv_extent_count);

	/* ── 3. Free all allocated extents ── */
	for (i = 0; i < n_alloc; i++) {
		int frc = nvmeibc_tpv_free_extent(ctx.tpv, i);

		TPV_CHECK(frc == 0, "free_extent(%llu) returned %d", i, frc);
	}

	/* Drain persist_work triggered by free_extent */
	flush_workqueue(system_wq);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_free_ok) == (s64)n_alloc,
		  "stat_tpv_free_ok: expected %llu got %lld",
		  n_alloc, (s64)atomic64_read(&alloc->stat_tpv_free_ok));

	TPV_CHECK(tpv_test_count_mapped(ctx.tpv) == 0,
		  "xarray not empty after freeing all: %llu entries",
		  tpv_test_count_mapped(ctx.tpv));

	/* ── 4. Free of unmapped extent returns -ENOENT ── */
	TPV_CHECK(nvmeibc_tpv_free_extent(ctx.tpv, 999) == -ENOENT,
		  "free_extent of unmapped index should return -ENOENT");

	unitest_print("*** tpv_alloc_free: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_persist ────────────────────────────────────────────────── */

/*
 * Flush-state / load-state round-trip.
 *
 * 1. Allocate N extents, flush to CDV RAM.
 * 2. Detach the TPV (which also flushes).
 * 3. Re-attach a fresh TPV against the same CDV RAM.
 * 4. Verify the xarray is reconstructed identically.
 */
TEST_FUNC int unitest_tpv_persist(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_extent_entry *entry;
	u64 saved_phys[4];
	u64 saved_cdv_idx[4];
	u64 n_alloc = 4;
	u64 i;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);
	tpv_simu_fill_pool(ctx.tpv);

	/* Allocate n_alloc virtual extents and record their physical mappings. */
	for (i = 0; i < n_alloc; i++) {
		int arc = nvmeibc_tpv_alloc_extent(ctx.tpv, i, &entry);

		TPV_CHECK(arc == 0, "alloc_extent(%llu) failed: %d", i, arc);
		if (arc == 0) {
			saved_phys[i]    = entry->phys_offset;
			saved_cdv_idx[i] = entry->cdv_extent_index;
		}
	}

	/* Explicit flush so the CDV RAM reflects the current xarray. */
	{
		int frc = nvmeibc_tpv_flush_state(ctx.tpv);

		TPV_CHECK(frc == 0, "flush_state returned %d", frc);
	}

	/*
	 * Detach: this also flushes dirty state.  Mark not-dirty first so that
	 * detach's conditional flush is skipped (we already flushed above).
	 */
	spin_lock(&ctx.tpv->persist_lock);
	ctx.tpv->dirty = false;
	spin_unlock(&ctx.tpv->persist_lock);

	nvmeibc_tpv_detach(ctx.tpv);
	ctx.tpv = NULL;

	/*
	 * Re-attach a fresh TPV against the same CDV vol and CDV RAM.
	 * The TOMA extent table still has extents 1..N_DATA_EXT allocated with
	 * the TPV UUID, so recovery (if called) would re-adopt them.  But since
	 * load_state will restore the xarray from the flushed L1, recovery is
	 * a no-op (all extents are already known).
	 */
	ctx.tpv = nvmeibc_tpv_attach(
		ctx.cdv,
		TPV_SIMU_TPV_NAME,
		TPV_SIMU_TPV_UUID,
		TPV_SIMU_VIRTUAL_MB * 1024ULL * 1024ULL,
		(u32)TPV_SIMU_TPV_EXTENT_KB,
		(u32)TPV_SIMU_CDV_EXTENT_MB,
		(u64)TPV_SIMU_ALLOC_GB);
	TPV_CHECK(ctx.tpv != NULL, "re-attach failed");
	if (!ctx.tpv) {
		rv = -1;
		goto out;
	}

	/* Drain any pending work from re-attach (pool replenished by load_state). */
	flush_workqueue(system_wq);

	/* Verify the xarray was restored from the L1 tree. */
	TPV_CHECK(tpv_test_count_mapped(ctx.tpv) == n_alloc,
		  "after reload: xarray has %llu entries, expected %llu",
		  tpv_test_count_mapped(ctx.tpv), n_alloc);

	for (i = 0; i < n_alloc; i++) {
		struct nvmeibc_tpv_extent_entry *e =
			xa_load(&ctx.tpv->allocator.extent_map, i);

		TPV_CHECK(e != NULL, "virt=%llu not in xarray after reload", i);
		if (!e)
			continue;
		TPV_CHECK(e->phys_offset == saved_phys[i],
			  "virt=%llu phys mismatch: expected 0x%llx got 0x%llx",
			  i, saved_phys[i], e->phys_offset);
		TPV_CHECK(e->cdv_extent_index == saved_cdv_idx[i],
			  "virt=%llu cdv_idx mismatch: expected %llu got %llu",
			  i, saved_cdv_idx[i], e->cdv_extent_index);
	}

out:
	unitest_print("*** tpv_persist: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_recovery ───────────────────────────────────────────────── */

/*
 * Orphan extent adoption.
 *
 * Simulates a crash that left CDV extents allocated in TOMA (visible via
 * cdv_list_extents) but absent from the L1 tree (CDV RAM is zeroed).
 *
 * Recovery is called manually after setting the TOMA ID so it can run
 * the TOMA query.  Adopted extents must appear in the free pool.
 */
TEST_FUNC int unitest_tpv_recovery(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_allocator *alloc;
	u64 n_orphan = 2;
	u64 expected_slots;
	u64 i;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	/*
	 * Pre-populate the TOMA extent table: mark extents 1..n_orphan as
	 * allocated with the TPV UUID, simulating a crash after CDV_ALLOC_OK
	 * but before any flush of virtual-extent mappings.
	 */
	for (i = 1; i <= n_orphan; i++) {
		ctx.sim->extents[i].allocated = true;
		strncpy(ctx.sim->extents[i].owner_uuid, TPV_SIMU_TPV_UUID,
			sizeof(ctx.sim->extents[i].owner_uuid) - 1);
	}

	/*
	 * The CDV RAM buffer is zeroed → load_state sees an empty L1 tree →
	 * no extents are restored to the allocator.
	 * The cdv_alloc_work that attach schedules will be a no-op (no TOMA ID).
	 */
	alloc = &ctx.tpv->allocator;

	TPV_CHECK(alloc->free_tpv_extent_count == 0,
		  "expected 0 free slots before recovery, got %llu",
		  alloc->free_tpv_extent_count);
	TPV_CHECK(alloc->cdv_extents_count == 0,
		  "expected 0 CDV extents before recovery, got %llu",
		  alloc->cdv_extents_count);

	/* Set TOMA ID and run recovery manually. */
	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);
	{
		int rrc = nvmeibc_tpv_recovery(ctx.tpv);

		TPV_CHECK(rrc == 0, "nvmeibc_tpv_recovery returned %d", rrc);
	}

	/* After recovery, the n_orphan CDV extents must be in the free pool. */
	expected_slots = n_orphan * TPV_SIMU_N_SLOTS;
	TPV_CHECK(alloc->free_tpv_extent_count == expected_slots,
		  "after recovery: expected %llu free slots, got %llu",
		  expected_slots, alloc->free_tpv_extent_count);
	TPV_CHECK(alloc->cdv_extents_count == n_orphan,
		  "after recovery: expected %llu CDV extents, got %llu",
		  n_orphan, alloc->cdv_extents_count);

	/* Verify we can allocate from the recovered pool. */
	{
		struct nvmeibc_tpv_extent_entry *e;
		int arc = nvmeibc_tpv_alloc_extent(ctx.tpv, 0, &e);

		TPV_CHECK(arc == 0,
			  "alloc_extent from recovered pool returned %d", arc);
	}

	unitest_print("*** tpv_recovery: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_cdv_full ───────────────────────────────────────────────── */

/*
 * CDV_FULL injection: verify the stat counter increments and the allocator
 * does not crash.
 */
TEST_FUNC int unitest_tpv_cdv_full(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_allocator *alloc;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	alloc = &ctx.tpv->allocator;

	/* Inject CDV_FULL on the next alloc_extent call. */
	ctx.sim->inject_full = true;

	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);

	/* Trigger cdv_alloc_work once. */
	if (!atomic_xchg(&ctx.tpv->cdv_alloc_pending, 1))
		schedule_work(&ctx.tpv->cdv_alloc_work);
	flush_workqueue(system_wq);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_cdv_alloc_full) == 1,
		  "stat_cdv_alloc_full: expected 1, got %lld",
		  (s64)atomic64_read(&alloc->stat_cdv_alloc_full));

	/* Pool should still be empty (no extent was allocated). */
	TPV_CHECK(alloc->free_tpv_extent_count == 0,
		  "expected 0 free slots after CDV_FULL, got %llu",
		  alloc->free_tpv_extent_count);

	/* cdv_alloc_pending is cleared even after CDV_FULL. */
	TPV_CHECK(atomic_read(&ctx.tpv->cdv_alloc_pending) == 0,
		  "cdv_alloc_pending should be 0 after work completes");

	unitest_print("*** tpv_cdv_full: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_wrong_gen ──────────────────────────────────────────────── */

/*
 * WRONG_GEN injection: verify the stat counter increments, then verify that
 * updating the allocator ID and re-triggering work succeeds.
 */
TEST_FUNC int unitest_tpv_wrong_gen(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_allocator *alloc;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	alloc = &ctx.tpv->allocator;

	/* Inject WRONG_GEN on the next alloc_extent call. */
	ctx.sim->inject_wrong_gen     = true;
	ctx.sim->inject_wrong_gen_val = 99ULL;	/* new generation from TOMA */

	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);

	/* Trigger cdv_alloc_work — should get WRONG_GEN. */
	if (!atomic_xchg(&ctx.tpv->cdv_alloc_pending, 1))
		schedule_work(&ctx.tpv->cdv_alloc_work);
	flush_workqueue(system_wq);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_cdv_alloc_wgen) == 1,
		  "stat_cdv_alloc_wgen: expected 1, got %lld",
		  (s64)atomic64_read(&alloc->stat_cdv_alloc_wgen));
	TPV_CHECK(alloc->free_tpv_extent_count == 0,
		  "expected 0 free slots after WRONG_GEN, got %llu",
		  alloc->free_tpv_extent_count);

	/* Update the allocator ID to the new generation. */
	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, 99ULL);

	/* Now trigger work again — should succeed. */
	if (!atomic_xchg(&ctx.tpv->cdv_alloc_pending, 1))
		schedule_work(&ctx.tpv->cdv_alloc_work);
	flush_workqueue(system_wq);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_cdv_alloc_ok) >= 1,
		  "stat_cdv_alloc_ok: expected >= 1 after gen update, got %lld",
		  (s64)atomic64_read(&alloc->stat_cdv_alloc_ok));
	TPV_CHECK(alloc->free_tpv_extent_count > 0,
		  "expected > 0 free slots after gen update, got %llu",
		  alloc->free_tpv_extent_count);

	unitest_print("*** tpv_wrong_gen: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_stat_reset ─────────────────────────────────────────────── */

/*
 * Verify that all atomic stat counters can be zeroed atomically and that
 * allocation activity is reflected correctly in the counters.
 */
TEST_FUNC int unitest_tpv_stat_reset(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_allocator *alloc;
	struct nvmeibc_tpv_extent_entry *entry;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);
	tpv_simu_fill_pool(ctx.tpv);

	alloc = &ctx.tpv->allocator;

	/* Generate some activity. */
	nvmeibc_tpv_alloc_extent(ctx.tpv, 0, &entry);
	nvmeibc_tpv_alloc_extent(ctx.tpv, 1, &entry);
	nvmeibc_tpv_free_extent(ctx.tpv,  0);
	flush_workqueue(system_wq);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_alloc_ok) >= 2,
		  "stat_tpv_alloc_ok should be >= 2 before reset, got %lld",
		  (s64)atomic64_read(&alloc->stat_tpv_alloc_ok));
	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_free_ok) >= 1,
		  "stat_tpv_free_ok should be >= 1 before reset, got %lld",
		  (s64)atomic64_read(&alloc->stat_tpv_free_ok));

	/* Reset all counters (mirrors tpv_proc_stats_reset logic). */
	atomic64_set(&alloc->stat_tpv_alloc_ok,    0);
	atomic64_set(&alloc->stat_tpv_alloc_eagain, 0);
	atomic64_set(&alloc->stat_tpv_alloc_enomem, 0);
	atomic64_set(&alloc->stat_tpv_free_ok,      0);
	atomic64_set(&alloc->stat_cdv_alloc_ok,     0);
	atomic64_set(&alloc->stat_cdv_alloc_full,   0);
	atomic64_set(&alloc->stat_cdv_alloc_wgen,   0);
	atomic64_set(&alloc->stat_cdv_alloc_err,    0);
	atomic64_set(&alloc->stat_cdv_free_ok,      0);
	atomic64_set(&alloc->stat_cdv_alloc_ns,     0);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_alloc_ok) == 0,
		  "stat_tpv_alloc_ok not zero after reset");
	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_free_ok) == 0,
		  "stat_tpv_free_ok not zero after reset");
	TPV_CHECK((s64)atomic64_read(&alloc->stat_cdv_alloc_ok) == 0,
		  "stat_cdv_alloc_ok not zero after reset");

	/* Counters increment again after reset. */
	nvmeibc_tpv_alloc_extent(ctx.tpv, 2, &entry);
	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_alloc_ok) == 1,
		  "stat_tpv_alloc_ok should be 1 after post-reset alloc, got %lld",
		  (s64)atomic64_read(&alloc->stat_tpv_alloc_ok));

	unitest_print("*** tpv_stat_reset: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_pool_exhaustion ────────────────────────────────────────── */

/*
 * Allocate every available virtual extent slot, verify -EAGAIN when the pool
 * is empty, then free one and verify the allocator recovers.
 */
TEST_FUNC int unitest_tpv_pool_exhaustion(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_allocator *alloc;
	struct nvmeibc_tpv_extent_entry *entry;
	u64 total_slots = (u64)TPV_SIMU_N_DATA_EXT * TPV_SIMU_N_SLOTS;
	u64 i;
	int rc;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);
	tpv_simu_fill_pool(ctx.tpv);

	alloc = &ctx.tpv->allocator;

	/* Allocate every slot. */
	for (i = 0; i < total_slots; i++) {
		rc = nvmeibc_tpv_alloc_extent(ctx.tpv, i, &entry);
		TPV_CHECK(rc == 0, "alloc_extent(%llu) returned %d", i, rc);
	}

	TPV_CHECK(alloc->free_tpv_extent_count == 0,
		  "pool should be empty, got %llu free",
		  alloc->free_tpv_extent_count);

	/* Next alloc must return -EAGAIN. */
	rc = nvmeibc_tpv_alloc_extent(ctx.tpv, total_slots, &entry);
	TPV_CHECK(rc == -EAGAIN,
		  "expected -EAGAIN (%d) when pool empty, got %d", -EAGAIN, rc);

	TPV_CHECK((s64)atomic64_read(&alloc->stat_tpv_alloc_eagain) >= 1,
		  "stat_tpv_alloc_eagain should be >= 1, got %lld",
		  (s64)atomic64_read(&alloc->stat_tpv_alloc_eagain));

	/* Free one slot, drain persist_work, then alloc again — should succeed. */
	rc = nvmeibc_tpv_free_extent(ctx.tpv, 0);
	TPV_CHECK(rc == 0, "free_extent(0) returned %d", rc);
	flush_workqueue(system_wq);

	rc = nvmeibc_tpv_alloc_extent(ctx.tpv, 0, &entry);
	TPV_CHECK(rc == 0,
		  "alloc_extent after freeing one slot returned %d", rc);

	unitest_print("*** tpv_pool_exhaustion: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_double_free ───────────────────────────────────────────── */

/*
 * Free an allocated extent, then free the same virt_idx again.
 * The second free must return -ENOENT.
 */
TEST_FUNC int unitest_tpv_double_free(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	struct tpv_test_ctx ctx = {0};
	struct nvmeibc_tpv_extent_entry *entry;
	int rc;
	int rv = 0;

	if (tpv_test_setup(&ctx))
		return -1;

	tpv_simu_set_toma_id(ctx.tpv, TPV_SIMU_TOMA_ID, TPV_SIMU_TOMA_GEN);
	tpv_simu_fill_pool(ctx.tpv);

	/* Allocate virt_idx 0. */
	rc = nvmeibc_tpv_alloc_extent(ctx.tpv, 0, &entry);
	TPV_CHECK(rc == 0, "alloc_extent(0) returned %d", rc);

	/* First free — should succeed. */
	rc = nvmeibc_tpv_free_extent(ctx.tpv, 0);
	TPV_CHECK(rc == 0, "first free_extent(0) returned %d", rc);
	flush_workqueue(system_wq);

	/* Second free of the same index — should return -ENOENT. */
	rc = nvmeibc_tpv_free_extent(ctx.tpv, 0);
	TPV_CHECK(rc == -ENOENT,
		  "expected -ENOENT (%d) on double free, got %d", -ENOENT, rc);

	unitest_print("*** tpv_double_free: %s\n", rv ? "FAIL" : "PASS");
	tpv_test_teardown(&ctx);
	return rv;
}

/* ── unitest_tpv_AllTests ───────────────────────────────────────────────── */

TEST_FUNC int unitest_tpv_AllTests(
	__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	int rv = 0;

	unitest_print("=== TPV unit tests begin ===\n");

	rv |= unitest_tpv_alloc_free(sys);
	rv |= unitest_tpv_persist(sys);
	rv |= unitest_tpv_recovery(sys);
	rv |= unitest_tpv_cdv_full(sys);
	rv |= unitest_tpv_wrong_gen(sys);
	rv |= unitest_tpv_stat_reset(sys);
	rv |= unitest_tpv_pool_exhaustion(sys);
	rv |= unitest_tpv_double_free(sys);

	unitest_print("=== TPV unit tests end: %s ===\n",
		      rv ? "FAIL" : "PASS");
	return rv;
}
