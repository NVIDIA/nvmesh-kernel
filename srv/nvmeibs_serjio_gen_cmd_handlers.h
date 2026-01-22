/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_SERJIO_GEN_CMD_HANDLERS_H
#define NVMEIBS_SERJIO_GEN_CMD_HANDLERS_H

#include "common/kr_incs.h"

struct nvmeibs_disk_info;
struct nvmeib_local_disk;
struct nvmeib_gen_cmd_param;
union nvmeib_gen_cmd_rsp;

int nvmeibs_serjio_gen_cmd_handle_get_jmdc_op(struct nvmeibs_disk_info *di,
				   const struct nvmeib_gen_cmd_param *p, union nvmeib_gen_cmd_rsp *rsp);

#endif//NVMEIBS_SERJIO_GEN_CMD_HANDLERS_H
