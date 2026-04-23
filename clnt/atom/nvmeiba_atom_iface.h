/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBA_ATOM_IFACE_H
#define NVMEIBA_ATOM_IFACE_H

#include "nvmeiba_nvmesh_api.h"

/*
 * ---------------------------------------------------------------------------
 * Extending the nvmeibc <-> nvmeiba dynamic binding (atom_attach ops table)
 * ---------------------------------------------------------------------------
 * 1. Add new function pointers to the **latest** version struct only
 *    (e.g. new fields in nvmeiba_atom_ops_v2, or add nvmeiba_atom_ops_v3).
 * 2. Bump NVMEIBA_ATOM_OPS_VERSION_* and NVMEIBA_ATOM_OPS_VERSION_MAX_SUPPORTED
 *    (in nvmeibc) / NVMEIBA_ATOM_OPS_VERSION_CURRENT (in nvmeiba).
 * 3. Update nvmeiba_atom_ops_layout_ok() in nvmeibc_nvmeiba_kapi.c: each
 *    version has its own minimum ops->size (see switch there).
 * 4. In nvmeibc, copy/use new fields after the v1 memcpy as needed.
 * 5. In nvmeiba, extend the static const struct nvmeiba_atom_ops initializer
 *    (bump .version / .v2 when extending the ops table).
 *
 * ops->version — layout revision implemented by this nvmeiba binary.
 * ops->size    — sizeof(struct nvmeiba_atom_ops) from the nvmeiba build.
 * nvmeibc checks (version, size) before reading v1 / v2. nvmeibc rejects
 * attach if any v1 function pointer is NULL. If nvmeiba advertises version 2
 * but any v2 pointer is NULL, nvmeibc uses only v1 (does not populate v2 kapi).
 * ---------------------------------------------------------------------------
 */

#define NVMEIBA_ATOM_OPS_VERSION_1		1u
#define NVMEIBA_ATOM_OPS_VERSION_2		2u
#define NVMEIBA_ATOM_OPS_VERSION_CURRENT	NVMEIBA_ATOM_OPS_VERSION_2

/* Highest ops version this nvmeibc build knows how to interpret (bump with v2+ logic). */
#define NVMEIBA_ATOM_OPS_VERSION_MAX_SUPPORTED	NVMEIBA_ATOM_OPS_VERSION_2

/*
 * V1: function table used for the nvmeibc global `nvmeiba_kapi` and for
 * legacy per-symbol binding (same layout).
 */
struct nvmeiba_atom_ops_v1 {
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

/*
 * V2: build identity of the loaded nvmeiba module (for nvmeibc attach logging
 * and diagnostics). String getters use scnprintf semantics: NUL-terminated
 * when len > 0; return value is the number of characters that would have been
 * written (excluding trailing NUL), capped at len - 1.
 */
struct nvmeiba_atom_ops_v2 {
	u64 (*module_get_commit_id)(void);
	size_t (*module_get_nvmesh_version)(char *buf, size_t len);
	size_t (*module_get_nvmesh_release)(char *buf, size_t len);
	size_t (*module_get_build_number)(char *buf, size_t len);
};

struct nvmeiba_atom_ops {
	u32 version;
	u32 size;
	struct nvmeiba_atom_ops_v1 v1;
	struct nvmeiba_atom_ops_v2 v2;
};

const struct nvmeiba_atom_ops *nvmeiba_atom_attach(void);

#endif
