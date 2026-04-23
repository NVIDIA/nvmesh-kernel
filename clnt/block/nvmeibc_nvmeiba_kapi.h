/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

#ifndef NVMEIBC_NVMEIBA_KAPI_H
#define NVMEIBC_NVMEIBA_KAPI_H

#include "atom/nvmeiba_atom_iface.h"

/*
 * nvmeibc resolves nvmeiba at load time (see nvmeibc_nvmeiba_kapi.c) so
 * nvmeibc.ko does not record undefined symbols against nvmeiba at link time.
 *
 * Preferred: nvmeiba_atom_attach() returns a versioned ops table.
 * Fallback:  per-symbol __symbol_get() for legacy nvmeiba without attach().
 *
 * Global function table (layout matches struct nvmeiba_atom_ops_v1 / attach .v1).
 * Populated from attach .v2 when nvmeiba advertises version 2 and all v2 pointers
 * are non-null; otherwise v2 stays zeroed (see nvmeibc_nvmeiba_kapi.c).
 */
extern struct nvmeiba_atom_ops_v1 nvmeiba_kapi;
extern struct nvmeiba_atom_ops_v2 nvmeiba_kapi_v2;

enum nvmeiba_kapi_iface_mode {
	NVMEIBA_KAPI_IFACE_NONE = 0,
	NVMEIBA_KAPI_IFACE_OPS,
	NVMEIBA_KAPI_IFACE_LEGACY,
};

enum nvmeiba_kapi_iface_mode nvmeiba_kapi_iface_mode(void);

int nvmeibc_nvmeiba_kapi_init(void);
void nvmeibc_nvmeiba_kapi_fini(void);

#endif
