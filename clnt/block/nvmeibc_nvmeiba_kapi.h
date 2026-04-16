/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

#ifndef NVMEIBC_NVMEIBA_KAPI_H
#define NVMEIBC_NVMEIBA_KAPI_H

#include "atom/nvmeiba_nvmesh_api.h"

/*
 * nvmeibc resolves nvmeiba exports at load time (see nvmeibc_nvmeiba_kapi.c) so
 * nvmeibc.ko does not record undefined symbols against nvmeiba at link time.
 */
struct nvmeiba_kapi {
	void (*os_api_constructor)(struct nvmeiba_atom_os_api *atom);
	void (*os_api_destructor)(struct nvmeiba_atom_os_api *atom);
	int (*os_api_orphan_abandon)(struct nvmeiba_atom_os_api *atom);
	bool (*os_api_is_queue_orphan)(const struct nvmeiba_atom_os_api *atom);
	int (*os_api_set_detaching)(struct nvmeiba_atom_os_api *atom);
	void (*os_api_exec_for_each_atom)(const char *dev_dir,
					  void (*fn)(const struct nvmeiba_atom_os_api *atom, void *ctx),
					  void *ctx);
	struct nvmeiba_atom_os_api *(*os_api_orphan_adopt)(const char *dev_dir, const char *dev_name);
	ssize_t (*atom_users_to_string)(void *_atom, char *buf, size_t len);
	int (*atom_open)(struct BLK_MODE_OPEN_OBJ_T *bdev, const char *name);
	void (*atom_close)(struct gendisk *disk);
	void (*atom_part_add)(struct nvmeiba_atom_os_api *atom);
	void (*atom_part_del)(struct nvmeiba_atom_os_api *atom);
	struct nvmeiba_to_c_handover (*os_do_on_nvmeibc_up)(void);
	void (*os_do_on_nvmeibc_down)(void);
};

extern struct nvmeiba_kapi nvmeiba_kapi;

int nvmeibc_nvmeiba_kapi_init(void);
void nvmeibc_nvmeiba_kapi_fini(void);

#endif
