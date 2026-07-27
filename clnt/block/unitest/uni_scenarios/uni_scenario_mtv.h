#ifndef UNI_SCENARIO_MTV_H
#define UNI_SCENARIO_MTV_H
/* Sets of unitest scenarios for MTV volumes */
#include "../bunitest.h"
#include "nvmesh_sim.h"

TEST_FUNC int unitest_mtv_two_volumes(struct NVMeshSystem *sys);

/* Basic unitest for MTV, prepare configuration in mgmt for each MTV
   And test attach functionality */
TEST_FUNC int unitest_mtv_sanity(struct NVMeshSystem *sys);
/* MTV good path IO test */
TEST_FUNC int unitest_mtv_GoodPathIO(struct NVMeshSystem *sys, int mtv_dev_index, int qlc_dev_index, int mdv_dev_index, int wcv_dev_index, u64 *written_magic, u64 *never_written_magic);
/* MTV READ FAIL IO FLOW TEST */
TEST_FUNC int unitest_mtv_ReadFailIO(struct NVMeshSystem *sys, int mtv_dev_index, int qlc_dev_index, int mdv_dev_index, u64 *magic_of_lba0, u64 *magic_of_not_lba0);
TEST_FUNC int unitest_mtv_partial_attach(struct NVMeshSystem *sys);

// Referenced in EC tests
struct block_inject_ptrs *__prepare_block_inject_ptrs_for_qlc(struct NVMeshSystem *sys, const struct disk_range *pr, const u64 slba, const u32 lockset_owner_seg, const int replicas, const int volInd, const int mdv_volInd, struct block_inject_ptrs *bptrs);

#endif
/*****************************************************************************/
// EOF.
