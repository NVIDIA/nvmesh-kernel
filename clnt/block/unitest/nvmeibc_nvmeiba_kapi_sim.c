/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

/*
 * Simulator-friendly counterpart of clnt/block/nvmeibc_nvmeiba_kapi.c.
 *
 * In the kernel, nvmeibc resolves nvmeiba exports at module load via
 * __symbol_get()/symbol_put_addr() so nvmeibc.ko does not record undefined
 * symbols against nvmeiba at link time. Those primitives do not exist in
 * user space, and the unitest links nvmeiba sources directly into the
 * simulator binary, so we just statically wire the function pointers to the
 * already-linked nvmeiba_* implementations.
 */

#include "block/nvmeibc_nvmeiba_kapi.h"

struct nvmeiba_kapi nvmeiba_kapi;

int nvmeibc_nvmeiba_kapi_init(void)
{
	nvmeiba_kapi = (struct nvmeiba_kapi){
		.os_api_constructor      = nvmeiba_os_api_constructor,
		.os_api_destructor       = nvmeiba_os_api_destructor,
		.os_api_orphan_abandon   = nvmeiba_os_api_orphan_abandon,
		.os_api_is_queue_orphan  = nvmeiba_os_api_is_queue_orphan,
		.os_api_set_detaching    = nvmeiba_os_api_set_detaching,
		.os_api_exec_for_each_atom = nvmeiba_os_api_exec_for_each_atom,
		.os_api_orphan_adopt     = nvmeiba_os_api_orphan_adopt,
		.atom_users_to_string    = nvmeiba_atom_users_to_string,
		.atom_open               = nvmeiba_atom_open,
		.atom_close              = nvmeiba_atom_close,
		.atom_part_add           = nvmeiba_atom_part_add,
		.atom_part_del           = nvmeiba_atom_part_del,
		.os_do_on_nvmeibc_up     = nvmeiba_os_do_on_nvmeibc_up,
		.os_do_on_nvmeibc_down   = nvmeiba_os_do_on_nvmeibc_down,
	};
	return 0;
}

void nvmeibc_nvmeiba_kapi_fini(void)
{
	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
}
