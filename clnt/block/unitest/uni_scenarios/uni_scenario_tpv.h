/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef UNI_SCENARIO_TPV_H
#define UNI_SCENARIO_TPV_H

/*
 * uni_scenario_tpv.h — TPV/CDV unit-test scenario declarations.
 *
 * Tests are self-contained: each creates its own CDV simulator, attaches a
 * TPV, exercises specific behaviour, and tears everything down.  No
 * NVMeshSystem is required — TPV logic is decoupled from the full cluster
 * simulator.
 */

#include "../bunitest.h"

struct NVMeshSystem;

/*
 * unitest_tpv_alloc_free — basic alloc / free round-trip.
 *
 * Verifies:
 *   • nvmeibc_tpv_alloc_extent() returns 0 and populates *out for each of
 *     the available virtual extent indices.
 *   • stat_tpv_alloc_ok increments correctly.
 *   • After filling the map, alloc_extent returns -EAGAIN (pool empty).
 *   • nvmeibc_tpv_free_extent() returns 0 and decrements allocated_count.
 *   • stat_tpv_free_ok increments correctly.
 */
TEST_FUNC int unitest_tpv_alloc_free(struct NVMeshSystem *sys);

/*
 * unitest_tpv_persist — flush-state / load-state round-trip.
 *
 * Verifies:
 *   • nvmeibc_tpv_flush_state() serialises the xarray to the CDV RAM buffer.
 *   • After detach, loading the same CDV RAM buffer via a fresh TPV attach
 *     reconstructs the identical xarray (same virtual-extent → phys-offset
 *     mappings).
 */
TEST_FUNC int unitest_tpv_persist(struct NVMeshSystem *sys);

/*
 * unitest_tpv_recovery — orphan extent adoption.
 *
 * Verifies:
 *   • When the CDV simulator has extents allocated to a TPV UUID but those
 *     extents are absent from the on-disk L1 tree, nvmeibc_tpv_recovery()
 *     adopts them and adds their slots to the free pool.
 */
TEST_FUNC int unitest_tpv_recovery(struct NVMeshSystem *sys);

/*
 * unitest_tpv_cdv_full — behaviour when TOMA reports CDV_FULL.
 *
 * Verifies:
 *   • stat_cdv_alloc_full increments when the TOMA sim returns CDV_FULL.
 *   • The allocator continues to serve IOs from the existing free pool.
 *   • cdv_alloc_work does not reschedule itself after CDV_FULL.
 */
TEST_FUNC int unitest_tpv_cdv_full(struct NVMeshSystem *sys);

/*
 * unitest_tpv_wrong_gen — behaviour when TOMA reports WRONG_GEN.
 *
 * Verifies:
 *   • stat_cdv_alloc_wgen increments.
 *   • After calling nvmeibc_tpv_update_allocator_id() with a new generation,
 *     the next cdv_alloc_work invocation succeeds.
 */
TEST_FUNC int unitest_tpv_wrong_gen(struct NVMeshSystem *sys);

/*
 * unitest_tpv_stat_reset — stats proc write resets all counters to zero.
 */
TEST_FUNC int unitest_tpv_stat_reset(struct NVMeshSystem *sys);

/*
 * unitest_tpv_pool_exhaustion — allocate all slots, verify -EAGAIN.
 *
 * Verifies:
 *   • After allocating all TPV_SIMU_VIRT_EXTENTS virtual extents (exhausting
 *     the free pool), the next nvmeibc_tpv_alloc_extent() returns -EAGAIN.
 *   • stat_tpv_alloc_eagain increments.
 *   • After freeing one extent, a subsequent alloc succeeds again.
 */
TEST_FUNC int unitest_tpv_pool_exhaustion(struct NVMeshSystem *sys);

/*
 * unitest_tpv_double_free — free an extent twice, verify -ENOENT.
 *
 * Verifies:
 *   • The first nvmeibc_tpv_free_extent() returns 0.
 *   • The second nvmeibc_tpv_free_extent() on the same virt_idx returns
 *     -ENOENT (the xarray entry was already erased).
 */
TEST_FUNC int unitest_tpv_double_free(struct NVMeshSystem *sys);

/*
 * unitest_tpv_cdv_full_sustained — verify cdv_alloc_work handles repeated
 * CDV_FULL responses correctly: stat increments once per call, pool stays
 * empty, cdv_alloc_pending clears each time (no busy-loop).
 */
TEST_FUNC int unitest_tpv_cdv_full_sustained(struct NVMeshSystem *sys);

/*
 * unitest_tpv_alloc_eagain_under_cdv_full — end-to-end: with pool empty and
 * the CDV exhausted, nvmeibc_tpv_alloc_extent returns -EAGAIN, schedules
 * cdv_alloc_work, and the subsequent work run increments stat_cdv_alloc_full
 * without replenishing the pool.
 */
TEST_FUNC int unitest_tpv_alloc_eagain_under_cdv_full(struct NVMeshSystem *sys);

/*
 * unitest_tpv_cdv_full_then_recovery — capacity-return: CDV full →
 * stat_cdv_alloc_full increments; an admin-side extent release followed by
 * a new cdv_alloc_work run refills the pool (stat_cdv_alloc_ok increments,
 * free_tpv_extent_count > 0).
 */
TEST_FUNC int unitest_tpv_cdv_full_then_recovery(struct NVMeshSystem *sys);

/*
 * unitest_tpv_attach_under_cdv_full — attach a TPV against a pre-exhausted
 * CDV.  Verify attach reaches TPV_ATTACHED state, initial cdv_alloc_work
 * returns CDV_FULL without crashing, and subsequent alloc calls return
 * -EAGAIN (degraded mode).
 */
TEST_FUNC int unitest_tpv_attach_under_cdv_full(struct NVMeshSystem *sys);

/*
 * unitest_tpv_AllTests — convenience wrapper that runs all TPV sub-tests.
 * This is the entry point registered in bunitest.c.
 */
TEST_FUNC int unitest_tpv_AllTests(struct NVMeshSystem *sys);

#endif /* UNI_SCENARIO_TPV_H */
