/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_IB_NORDDA_CHANNEL_SIM_SHARED
#define NVMEIBC_IB_NORDDA_CHANNEL_SIM_SHARED

struct nvmeibc_disk_gen_cmd;
struct volume_server_rsp;
struct nvmeibc_ib_net_nordda;

/**
 * Read gen rsp and and fill out gen_cmd's rsp accordingly.
 */
int nvmeibc_ib_net_nordda_decode_gen_rsp(struct nvmeibc_ib_net_nordda *net,
										 struct nvmeibc_disk_gen_cmd *g, struct volume_server_rsp *rsp);


#endif /*NVMEIBC_IB_NORDDA_CHANNEL_SIM_SHARED*/