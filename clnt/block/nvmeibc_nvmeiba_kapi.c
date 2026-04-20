/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

/*
 * Resolve nvmeiba exports at nvmeibc module load. The kernel does not export
 * find_symbol() to modules; __symbol_get() performs the same lookup for
 * exported symbols and pins the owner module until symbol_put_addr().
 * Note: symbol_put() is a macro for static symbol names; dynamic pointers
 * from __symbol_get() must use symbol_put_addr().
 *
 * Two modes:
 *  - OPS: nvmeiba_atom_attach() returns a versioned ops struct (single symbol ref).
 *  - LEGACY: bind each exported function (one ref per symbol).
 */

#include "nvmeibc_nvmeiba_kapi.h"

#include "kr_incs.h"
#include "nvmeibc_trace.h"

struct nvmeiba_atom_ops_v1 nvmeiba_kapi;

#define NVMEIBA_SYM_MAX DIV_ROUND_UP(sizeof(struct nvmeiba_atom_ops_v1), sizeof(void *))

static void *nvmeiba_sym_refs[NVMEIBA_SYM_MAX];
static unsigned int nvmeiba_sym_nrefs;

static enum nvmeiba_kapi_iface_mode kapi_iface_mode = NVMEIBA_KAPI_IFACE_NONE;

enum nvmeiba_kapi_iface_mode nvmeiba_kapi_iface_mode(void)
{
	return kapi_iface_mode;
}

static void nvmeiba_release_symbols(void)
{
	while (nvmeiba_sym_nrefs)
		symbol_put_addr(nvmeiba_sym_refs[--nvmeiba_sym_nrefs]);
}

static int nvmeiba_bind_symbol(const char *name, void **dest)
{
	void *p = __symbol_get(name);

	if (!p) {
		_NE_dmesg(err_nvmeiba_bind_symbol_not_found,
			"nvmeiba symbol: @SYMBOL_NAME not found (is nvmeiba loaded?)", name);
		return -ENOENT;
	}
	if (nvmeiba_sym_nrefs >= NVMEIBA_SYM_MAX) {
		symbol_put_addr(p);
		_NE_dmesg(err_nvmeiba_bind_symbol_overflow,
			"nvmeiba internal symbol table overflow binding: @SYMBOL_NAME", name);
		return -EINVAL;
	}
	nvmeiba_sym_refs[nvmeiba_sym_nrefs++] = p;
	*dest = p;
	return 0;
}

typedef const struct nvmeiba_atom_ops *(*nvmeiba_atom_attach_fn)(void);

/*
 * Minimum ops->size (bytes) for each ops->version — must cover the fields
 * nvmeibc reads for that version. Keep in sync with nvmeiba_atom_iface.h.
 */
static bool nvmeiba_atom_ops_layout_ok(u32 version, u32 size)
{
	switch (version) {
	case NVMEIBA_ATOM_OPS_VERSION_1:
		/* Header + v1 table (everything before optional v2). */
		return size >= offsetof(struct nvmeiba_atom_ops, v2);
	case NVMEIBA_ATOM_OPS_VERSION_2:
		/* v1 + v2; grows when nvmeiba_atom_ops_v2 gains members. */
		return size >= sizeof(struct nvmeiba_atom_ops);
	default:
		return false;
	}
}

static int nvmeiba_try_attach_ops(void)
{
	void *attach_sym;
	nvmeiba_atom_attach_fn attach_fn;
	const struct nvmeiba_atom_ops *ops;

	attach_sym = __symbol_get("nvmeiba_atom_attach");
	if (!attach_sym)
		return -ENOENT;

	attach_fn = (nvmeiba_atom_attach_fn)attach_sym;
	ops = attach_fn();
	if (!ops || ops->version < NVMEIBA_ATOM_OPS_VERSION_1 ||
	    ops->version > NVMEIBA_ATOM_OPS_VERSION_MAX_SUPPORTED ||
	    !nvmeiba_atom_ops_layout_ok(ops->version, ops->size)) {
		symbol_put_addr(attach_sym);
		_NE_dmesg(err_nvmeiba_kapi_atom_attach_ops_rejected,
			"nvmeiba_atom_attach: invalid or unsupported ops table (version/size)");
		return -EINVAL;
	}

	memcpy(&nvmeiba_kapi, &ops->v1, sizeof(nvmeiba_kapi));
	if (nvmeiba_sym_nrefs >= NVMEIBA_SYM_MAX) {
		symbol_put_addr(attach_sym);
		return -EINVAL;
	}
	nvmeiba_sym_refs[nvmeiba_sym_nrefs++] = attach_sym;
	kapi_iface_mode = NVMEIBA_KAPI_IFACE_OPS;
	_NT(trace_nvmeiba_kapi_init_via_atom_attach,
	    "Successfully resolved nvmeiba via atom_attach. Fn table: @PTR", &nvmeiba_kapi);
	return 0;
}

static int nvmeiba_bind_legacy(void)
{
	int rv;

#define BIND(n, field) \
	do { \
		rv = nvmeiba_bind_symbol(#n, (void **)&nvmeiba_kapi.field); \
		if (rv) \
			goto err; \
	} while (0)

	BIND(nvmeiba_os_api_constructor, os_api_constructor);
	BIND(nvmeiba_os_api_destructor, os_api_destructor);
	BIND(nvmeiba_os_api_orphan_abandon, os_api_orphan_abandon);
	BIND(nvmeiba_os_api_is_queue_orphan, os_api_is_queue_orphan);
	BIND(nvmeiba_os_api_set_detaching, os_api_set_detaching);
	BIND(nvmeiba_os_api_exec_for_each_atom, os_api_exec_for_each_atom);
	BIND(nvmeiba_os_api_orphan_adopt, os_api_orphan_adopt);
	BIND(nvmeiba_atom_users_to_string, atom_users_to_string);
	BIND(nvmeiba_atom_open, atom_open);
	BIND(nvmeiba_atom_close, atom_close);
	BIND(nvmeiba_atom_part_add, atom_part_add);
	BIND(nvmeiba_atom_part_del, atom_part_del);
	BIND(nvmeiba_os_do_on_nvmeibc_up, os_do_on_nvmeibc_up);
	BIND(nvmeiba_os_do_on_nvmeibc_down, os_do_on_nvmeibc_down);
#undef BIND

	kapi_iface_mode = NVMEIBA_KAPI_IFACE_LEGACY;
	_NT(trace_nvmeiba_kapi_init_via_legacy_bind,
	    "Successfully resolved all nvmeiba symbols (legacy bind). Fn table: @PTR", &nvmeiba_kapi);
	return 0;
err:
	return rv;
}

int nvmeibc_nvmeiba_kapi_init(void)
{
	int rv;

	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
	kapi_iface_mode = NVMEIBA_KAPI_IFACE_NONE;
	request_module("nvmeiba");

	rv = nvmeiba_try_attach_ops();
	if (!rv)
		return 0;

	rv = nvmeiba_bind_legacy();
	if (!rv)
		return 0;

	nvmeiba_release_symbols();
	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
	kapi_iface_mode = NVMEIBA_KAPI_IFACE_NONE;
	return rv;
}

void nvmeibc_nvmeiba_kapi_fini(void)
{
	nvmeiba_release_symbols();
	_NT(trace_nvmeibc_nvmeiba_kapi_fini,
	    "Successfully released all nvmeiba symbols. nvmeiba can now be unloaded.");
	kapi_iface_mode = NVMEIBA_KAPI_IFACE_NONE;
	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
}
