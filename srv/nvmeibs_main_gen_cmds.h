/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_MAIN_GEN_CMDS_H
#define NVMEIBS_MAIN_GEN_CMDS_H

struct nvmeibs_disk_info;
struct nvmeib_gen_cmd_param;
union nvmeib_gen_cmd_rsp;
struct nvmeib_local_disk;
struct nvmeibs_async_cookie_params;

int nvmeibs_handle_free_ents_gen_cmd(struct nvmeibs_disk_info *di,
									 const struct nvmeib_gen_cmd_param *p,
									 struct nvmeibs_async_cookie_params *cookie_params);

int nvmeibs_handle_blkset_recovered_gen_cmd(struct nvmeibs_disk_info *di, 
											const struct nvmeib_gen_cmd_param *gen_param, 
											union nvmeib_gen_cmd_rsp *gen_rsp,
											struct nvmeibs_async_cookie_params *cookie_params);

int nvmeibs_handle_gen_cmd(struct nvmeib_local_disk *disk, enum nvmeib_gen_cmd_op opcode, 
						   const struct nvmeib_gen_cmd_param *p, union nvmeib_gen_cmd_rsp *rsp, u32 cid);

#endif /* NVMEIBS_MAIN_CMDS_H */
