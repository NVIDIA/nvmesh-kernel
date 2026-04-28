/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

/*
 * Simulator stand-in for clnt/block/nvmeibc_nvmeiba_kapi.c.
 *
 * In the kernel, nvmeibc.ko and nvmeiba.ko are separate modules and nvmeibc
 * resolves nvmeiba's atom_attach() via __symbol_get()/request_module() so
 * nvmeibc.ko doesn't record undefined symbols against nvmeiba at link time.
 *
 * In the unitest simulator both modules are linked into a single user-space
 * executable, so __symbol_get()/request_module() do not exist and aren't
 * needed: nvmeiba_atom_attach() can be called directly. We still validate
 * the returned ops table the same way the production code does, so a
 * malformed v1/v2 layout is caught here too.
 */

#include "nvmeib_common_all.h"			// First injection point for all simulators

#include "block/nvmeibc_nvmeiba_kapi.h"

struct nvmeiba_atom_ops_v1 nvmeiba_kapi;
struct nvmeiba_atom_ops_v2 nvmeiba_kapi_v2;

static enum nvmeiba_kapi_iface_mode kapi_iface_mode = NVMEIBA_KAPI_IFACE_NONE;

enum nvmeiba_kapi_iface_mode nvmeiba_kapi_iface_mode(void)
{
	return kapi_iface_mode;
}

static bool nvmeiba_atom_ops_v1_complete(const struct nvmeiba_atom_ops_v1 *v)
{
	return v->os_api_constructor && v->os_api_destructor && v->os_api_orphan_abandon &&
	       v->os_api_is_queue_orphan && v->os_api_set_detaching && v->os_api_exec_for_each_atom &&
	       v->os_api_orphan_adopt && v->atom_users_to_string && v->atom_open && v->atom_close &&
	       v->atom_part_add && v->atom_part_del && v->os_do_on_nvmeibc_up && v->os_do_on_nvmeibc_down;
}

static bool nvmeiba_atom_ops_v2_complete(const struct nvmeiba_atom_ops_v2 *v)
{
	return v->module_get_commit_id && v->module_get_nvmesh_version &&
	       v->module_get_nvmesh_release && v->module_get_build_number;
}

static bool nvmeiba_atom_ops_layout_ok(u32 version, u32 size)
{
	switch (version) {
	case NVMEIBA_ATOM_OPS_VERSION_1:
		return size >= offsetof(struct nvmeiba_atom_ops, v2);
	case NVMEIBA_ATOM_OPS_VERSION_2:
		return size >= sizeof(struct nvmeiba_atom_ops);
	default:
		return false;
	}
}

int nvmeibc_nvmeiba_kapi_init(void)
{
	const struct nvmeiba_atom_ops *ops;
	u32 effective_version;

	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
	memset(&nvmeiba_kapi_v2, 0, sizeof(nvmeiba_kapi_v2));
	kapi_iface_mode = NVMEIBA_KAPI_IFACE_NONE;

	ops = nvmeiba_atom_attach();
	if (!ops) {
		_NE_dmesg(err_kapi_sim_atom_attach_returned_null,
			"nvmeiba_atom_attach (sim): returned NULL");
		return -ENOENT;
	}
	if (ops->version < NVMEIBA_ATOM_OPS_VERSION_1 ||
	    ops->version > NVMEIBA_ATOM_OPS_VERSION_MAX_SUPPORTED ||
	    !nvmeiba_atom_ops_layout_ok(ops->version, ops->size)) {
		_NE_dmesg(err_kapi_sim_atom_attach_ops_rejected,
			"nvmeiba_atom_attach (sim): invalid or unsupported ops table (version/size)");
		return -EINVAL;
	}
	if (!nvmeiba_atom_ops_v1_complete(&ops->v1)) {
		_NE_dmesg(err_kapi_sim_atom_attach_v1_incomplete,
			"nvmeiba_atom_attach (sim): v1 ops table has null function pointer(s)");
		return -EINVAL;
	}

	effective_version = ops->version;
	if (effective_version >= NVMEIBA_ATOM_OPS_VERSION_2 &&
	    !nvmeiba_atom_ops_v2_complete(&ops->v2))
		effective_version = NVMEIBA_ATOM_OPS_VERSION_1;

	memcpy(&nvmeiba_kapi, &ops->v1, sizeof(nvmeiba_kapi));
	if (effective_version >= NVMEIBA_ATOM_OPS_VERSION_2)
		memcpy(&nvmeiba_kapi_v2, &ops->v2, sizeof(nvmeiba_kapi_v2));

	kapi_iface_mode = NVMEIBA_KAPI_IFACE_OPS;
	_NT(trace_kapi_sim_init_via_atom_attach,
	    "Successfully resolved nvmeiba via atom_attach (sim). Fn table: @PTR", &nvmeiba_kapi);
	return 0;
}

void nvmeibc_nvmeiba_kapi_fini(void)
{
	_NT(trace_kapi_sim_fini, "Released nvmeiba kapi (sim).");
	kapi_iface_mode = NVMEIBA_KAPI_IFACE_NONE;
	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
	memset(&nvmeiba_kapi_v2, 0, sizeof(nvmeiba_kapi_v2));
}
