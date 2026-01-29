/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_MIRROR_H
#define NVMEIBC_DP_MIRROR_H
/* API of mirroring Raid1/Jbod + optional Raid0 datapath:
   Includes Components:
    1. Internal API for dp_mirror (including trim)
    2. External API (virtual functions of data path)
 */
#include "../datapath_utils_generic/nvmeibc_block_dp_common.h"

/******************** Internal Mirrored Trim functionality ********************/
/* Merge discard operations. When long trim is split due to striping it can be reconcatenated into number of commands <= stripe width.
   can_merge_diff_segs - true means 1 command can span accross many segments as long as they are adjacent on the same disk. */
int __concat_discard_op(struct nvmeibc_block_command cmds[], int *pncmds, int nlbas, struct nvmeibc_block_device *nd, bool can_merge_diff_segs);

/* After all owner locks are taken attempt to split trim commands.
   retrurns: negative - split needed but failed, 1 - split not needed,
   0 - split needed and done OK */
int nvmeibc_do_trim_split_if_needed(struct nvmeibc_cmd_lock *ls);

/************************ DP IO virtual functions *****************************/
int  dp_mirror_should_ignore_op(const struct operation *o); /* Ignore nothing */
int  dp_mirror_prepare_op(struct operation *o);
int  dp_mirror_execute_op(struct operation *o);
void dp_mirror_block_completion(struct nvmeibc_d_iocmd_comp *comp);
int  dp_mirror_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int err);
void dp_mirror_exec_func_on_stage_end(struct nvmeibc_block_command *rldr, int *rv);
void dp_mirror_calc_should_abandon(struct nvmeibc_block_command *cmds, int lsi);
void dp_mirror_calc_comp_state(const struct nvmeibc_block_command *cmds, int *rv, bool *retry);
void dp_mirror_complete_locks_debug_val(struct nvmeibc_cmd_lock *locksets);
int dp_mirror_translate_addr(struct dp_block_translation_unit *tu);

/************************ DP Sync Prep/Execute functions ***************************/
int  dp_mirror_sync_prepare_op(struct recovery_sync_op *so);
void dp_mirror_sync_execute_op(struct recovery_sync_op *so);	// When locks are taken
void dp_mirror_sync_cmd_cb(struct nvmeibc_block_command *cmd);	// Callback on disk command
void dp_mirror_sync_resume_op( struct recovery_sync_op *so);	// Resume the execution of state machine after async disk command completion

/************************ DP Sync virtual functions ***************************/
u32                                  dp_mirror_calc_scrub_writes(struct recovery_sync_op *so);  // Returns a bitmap of which slices are incorrect and should to be rewritten
enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE dp_mirror_no_write_hole_fix(struct recovery_sync_op *so);
void                             dp_mirror_no_write_hole_destroy(struct recovery_sync_op *so);
void                         dp_mirror_no_write_hole_sbs_cleanup(struct recovery_sync_op *so);

#endif  // H beginning

