/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_API_OS_SUB_VOLS_H
#define NVMEIBC_BLOCK_API_OS_SUB_VOLS_H
/* Internal API on top of regular os_apis, which implements a sub volume functionality.
   A bit like what https://en.wikipedia.org/wiki/Device_mapper does */

static int __exec_for_each_sub_vol(        struct nvmeiba_atom_os_api *car, int (*fn)(struct nvmeiba_atom_os_api *sub));
static int __exec_for_carrier_and_sub_vols(struct nvmeiba_atom_os_api *car, int (*fn)(struct nvmeiba_atom_os_api *sub));

static int  nvmeibc_atom_part_add(struct nvmeiba_atom_os_api *atom, struct nvmeiba_atom_os_api *car, ulong start_lba, const char *dir_lsblk, const char* dev_name, union nvmeiba_part_flags f);
static void nvmeibc_atom_part_del(struct nvmeiba_atom_os_api *atom);

// Extention of block_api_os_destroy()
static int block_api_os_destroy_sub_vol(struct nvmeiba_atom_os_api *atom);

static int __revalidate_sub_vols_of_carrier(struct nvmeiba_atom_os_api *atom);

#endif  // H beginning

