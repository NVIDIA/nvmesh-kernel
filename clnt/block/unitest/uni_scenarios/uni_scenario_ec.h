/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef UNI_SCENARIO_EC_VOLS_H
#define UNI_SCENARIO_EC_VOLS_H
/* Sets of unitest scenarios for EC datapath */
#include "../bunitest.h"
#include "nvmesh_sim.h"
#include "uni_enumerators.h"

/* Convert volume 0 to erasure coded volume */
TEST_FUNC int unitest_raid_ec_transform(         struct NVMeshSystem *sys, const char* action);

TEST_FUNC int unitest_GoodPathIO_raid50_or_60(bunitest_s* B);
TEST_FUNC int unitest_ec_journal_different_sizes(struct NVMeshSystem *sys);
TEST_FUNC int unitest_GoodPathIO_block_md_illegal_splits(bunitest_s* B);
TEST_FUNC int unitest_EC_GoodPath_binje8(bunitest_s* B);
TEST_FUNC int unitest_EC_single_slice_async_pause_disks(struct NVMeshSystem *sys, const bool with_error);
TEST_FUNC int unitest_EC_async_degraded_mode_rebuild_during_single_slice_io(struct NVMeshSystem *sys);
TEST_FUNC int unitest_TestAsyncJAM(struct NVMeshSystem *sys);

TEST_FUNC int unitest_ec_view_lock(bunitest_s* B);
TEST_FUNC int unitest_DegradedMode_EC(bunitest_s* B);
TEST_FUNC int unitest_EC_8127(bunitest_s* B);
TEST_FUNC int unitest_PermanentReadError_EC(bunitest_s* B);
// test IO's that fail to allocate journal space
TEST_FUNC int unitest_ec_journal_alloc_fail(struct NVMeshSystem *sys);

TEST_FUNC int unitest_scrubRecovery_ec(bunitest_s* B);
TEST_FUNC int unitest_ECColdRecovery(bunitest_s* B);
TEST_FUNC int unitest_ECJgcRecovery( bunitest_s* B);
TEST_FUNC int unitest_ECHotRecovery( bunitest_s* B);
TEST_FUNC int unitest_EC_recovery_Basic(bunitest_s* B);

TEST_FUNC int unitest_DoubleDegradedMode_EC(bunitest_s* B);
TEST_FUNC int unitest_ECHotJgcRecovery(bunitest_s* B);

TEST_FUNC int unitest_ECDbitsOnDisk(bunitest_s* B);

// Referenced in MTV/QLC tests as well
int unitest_PermanentReadError_EC_IO(const struct test_context env, const int degraded_seg_index, u8 *mem, const int is_dead);
void unitest_PermanentReadError_EC_Maintenance(const struct test_context env, const int degraded_seg_index, u8 *mem, const int is_dead);
void __degrade_segment(struct clientSimulator *client, struct tTopoOfPraid* r1, struct disk_range *praid, int ind_dead_seg);
void __restore_seg_to_write(struct NVMeshSystem *sys, struct clientSimulator *client, struct tTopoOfPraid* r1, struct disk_range *praid, int ind_dead_seg, bool clear_ram);
void __restore_seg_to_read_write(struct NVMeshSystem *sys, struct tTopoOfPraid* r1, int ind_dead_seg);

struct degraded_test_params {
	bool is_double_degraded;
	bool any_seg_is_write;
};

struct degraded_test_params __switch_to_new_topo(struct clientSimulator *client, struct topology_sgmnts_t new_topo, struct tTopoOfPraid* r1, struct disk_range *curSeg);
void __prepare_for_next_iteration(struct clientSimulator *client, struct topology_sgmnts_t new_topo, struct disk_range *curSeg);
#endif
/*****************************************************************************/
// EOF.
