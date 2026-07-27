#ifndef NVMEIBC_DP_EC_REED_SOLO_H
#define NVMEIBC_DP_EC_REED_SOLO_H

#include "../datapath_utils_generic/nvmeibc_block_dp_common.h"

/******************************** EC Algorithms *******************************/
void restore_degraded_data_for_read(   const struct operation *o, u32 dgrd_segment_bmp, int prev_rv, const bool is_crc_required);
u32 apply_gf_calculation_for_operation(const struct operation *o, u32 dgrd_segmnts_bmp, int prev_rv);
int reed_solo_restore_degraded_data_for_read(struct nvmeibc_block_command *rldr, int prev_rv);
int reed_solo_restore_degraded_data_for_sync(struct nvmeibc_block_command *rldr, int prev_rv, const u32 invalid_source_bmp);

#endif  // H beginning

