/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_MIRROR_MD_H
#define NVMEIBC_DP_MIRROR_MD_H
/* API Metadata carrier volume. Mirroring Raid1/Jbod:
	Supports sub block operations and better control of executino flow */

int   dp_md_mirror_should_ignore_op(const struct operation *o);
void  dp_md_mirror_exec_func_on_stage_end(struct nvmeibc_block_command *rldr, int *rv);
int   dp_md_mirror_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int  rv);
void  dp_md_mirror_calc_should_abandon(struct nvmeibc_block_command *rldr, __attribute__ ((unused)) int lsi);
void  dp_md_calc_comp_state(const struct nvmeibc_block_command *cmds, int *rv, bool *retry);

/* WCV specific functions */
int   dp_wcv_mirror_should_ignore_op(const struct operation *o);
int   dp_wcv_mirror_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int rv);
u64   dp_wcv_mirror_get_wcv2mdv_ptr_from_locks_binfo(const struct operation *o);


/* Data carrier API */
int  dp_dc_mirror_should_ignore_op(const struct operation *o);

#endif
