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
 */

#include "nvmeibc_nvmeiba_kapi.h"

#include "kr_incs.h"
#include "nvmeibc_trace.h"

struct nvmeiba_kapi nvmeiba_kapi;

#define NVMEIBA_SYM_MAX DIV_ROUND_UP(sizeof(struct nvmeiba_kapi), sizeof(void *))

static void *nvmeiba_sym_refs[NVMEIBA_SYM_MAX];
static unsigned int nvmeiba_sym_nrefs;

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

int nvmeibc_nvmeiba_kapi_init(void)
{
	int rv;

	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
	request_module("nvmeiba");

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

	_NT(trace_nvmeibc_nvmeiba_kapi_init, "Succesfully resolved all nvmeiba symbols. Fn table: @PTR", &nvmeiba_kapi);

	return 0;
err:
	nvmeiba_release_symbols();
	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
	return rv;
}

void nvmeibc_nvmeiba_kapi_fini(void)
{
	nvmeiba_release_symbols();
	_NT(trace_nvmeibc_nvmeiba_kapi_fini, "Successfully released all nvmeiba symbols. nvmeiba can now be unloaded.");
	memset(&nvmeiba_kapi, 0, sizeof(nvmeiba_kapi));
}
