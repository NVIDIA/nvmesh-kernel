/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_msgloop.h"
#include "nvmeib_event.h"
#include "module/nvmeibc_module_proc_files.inc.c"
#include "module/nvmeibc_module_input_char_device.inc.c"// Todo: Remove
#include "module/nvmeibc_module_ioctls.inc.c"
#include "nvmeibc_capabilities.h"
#include "tpv/nvmeibc_tpv.h"				/* nvmeibc_tpv_proc_destroy_root */

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__  nvmeibc_module_main_inc_c

/************************ Global module variables/methods *********************/
static struct t_main_module_single_instance_globals {
	atomic_t/*enum nvmeibc_mod_state*/ nvmeibc_state;		// The state of the module (Init, Ready, Shuttdown, Exit)
	struct t_md_area {		// Default scratch buffers for metadata when nvme disk has it but block layer does not need it
		void *read;
		void *write;
		void *poison;
		int n_read_pages;
		int n_write_pages;
		int n_poison_pages;
	} md_area;

	struct t_module_clnt_proc_dir {	/* globals: our root @ the procfs */
		const char *root_name;				// Root directory of client module
		struct proc_dir_entry *root;		// Main /proc directory of module
		struct t_module_clnt_proc_files {		// Files in root directory of each instance
			struct nvmeib_public_procfs_ent *version_proc;
			struct nvmeib_public_procfs_ent *cflags_proc;
			struct nvmeib_public_procfs_ent *dict_sign_proc;
			struct nvmeib_public_procfs_ent *isnt_list;
			struct msgloop_procfs_ent *inst_ctls_proc;	// msg loop for ioctls
			struct nvmeib_public_procfs_ent *echo_proc;		// Write only proc to commit text to longterm log
			struct nvmeib_public_procfs_ent *instcaiser;		// Coiser API of module (first instance)
		} files;
	} proc_dir;
	struct t_main_clnt_sched sched;
} mod_globals;

enum nvmeibc_mod_state nvmeibc_get_state(void)
{
	return (enum nvmeibc_mod_state)atomic_read(&mod_globals.nvmeibc_state);
}

void nvmeibc_state_promote(enum nvmeibc_mod_state st)
{
	const enum nvmeibc_mod_state prev_state = nvmeibc_get_state();
	int rv = 0;
	switch (st) {
	case NVMEIBC_MOD_STATE_INITIALIZING: {
		break;
	}
	case NVMEIBC_MOD_STATE_READY:
	case NVMEIBC_MOD_STATE_RM_RDY:
	case NVMEIBC_MOD_STATE_EXITING: {
		rv = (prev_state != (st-1));
		break;
	}
	case NVMEIBC_MOD_STATE_PREP_RM: {
		const bool is_init_error = (prev_state != NVMEIBC_MOD_STATE_READY);	//  Destrucion of partially initialized object (Creation failed).
		rv = (prev_state != (st - (is_init_error ? 2 : 1)));
		break;
	}
	default:
		rv = -EINVAL;
	}
	if (st > prev_state)
		atomic_set(&mod_globals.nvmeibc_state, st);
	WARN(rv, "nvmeibc bug, module wrong flow: state=%d->state=%d, rv=%d", prev_state, st, rv);
	_NI(t_01_mod_state_set, "@EVENT_TAG @NDU module @CURR_STATE->@STATE", EV_MODULE_STATE_CHANGE(), 0, prev_state, st);
}

void *nvmeibc_get_md_read_dummy_area(int *n_pages)
{
	const struct t_md_area *md = &mod_globals.md_area;
	if (n_pages)
		*n_pages = md->n_read_pages;
	return md->read;
}

void *nvmeibc_get_md_write_dummy_area(int *n_pages)
{
	const struct t_md_area *md = &mod_globals.md_area;
	if (n_pages)
		*n_pages = md->n_write_pages;
	return md->write;
}

static int __t_md_area_alloc(struct t_md_area *md)
{
	int rv;
	if ((md->read = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO))) {
		md->n_read_pages = 1;
		rv = 0;
	} else {
		md->n_read_pages = 0;
		rv = -ENOMEM;
	}

	md->write = page_address(ZERO_PAGE(0));
	md->n_write_pages = 1;
	if (rv)
		_NT(error_main_nvmeibc_init, "Failed to allocate MD dummy area");
	return rv;
}

static void __t_md_area_free(struct t_md_area *md)
{

	if (md->n_read_pages) {
		free_page((unsigned long)md->read);
		md->n_read_pages = 0;
	}
	if (md->n_write_pages) {
		md->n_write_pages = 0;
	}
}

#define INST_CTLS "instctls"

static ssize_t get_cflags(void *dummy, char *buffer, size_t len)
{
	(void)dummy;
	return nvmeibc_get_compile_flags(buffer, len);
}

static int nvmeibc_module_procs_create(struct t_main_module_single_instance_globals *_mg)
{
	int rv = -1;
	if (!(_mg->proc_dir.root = proc_mkdir(_mg->proc_dir.root_name, NULL)))
		goto destroy;

	rv = (_mg->proc_dir.files.inst_ctls_proc = nvmeib_msgloop_create(INST_CTLS, _mg->proc_dir.root, &handle_module_cli_input, NULL, NULL, _mg)) ? 0 : -1 ;
	PROC_FILE_CREATE(         _mg, _mg->proc_dir.files.version_proc  , "version"       , fill_version_json);
	PROC_FILE_CREATE(         _mg, _mg->proc_dir.files.cflags_proc   , "cflags"        , get_cflags);
	PROC_FILE_CREATE(         _mg, _mg->proc_dir.files.dict_sign_proc, "dict_sign"     , fill_dict_sign);
	PROC_FILE_CREATE(         _mg, _mg->proc_dir.files.isnt_list     , "inst_list.json", fill_isntances_info);
	PROC_FILE_CREATE_WRITABLE(_mg, _mg->proc_dir.files.echo_proc     , "echo"          , __echo_msg_to_longterm_log);
destroy:
	if (rv) {
	}
	return rv;
}

static void nvmeibc_module_procs_destroy(struct t_main_module_single_instance_globals *_mg)
{
	NFIN;

	if (_mg->proc_dir.files.inst_ctls_proc) {
		nvmeib_msgloop_remove(_mg->proc_dir.files.inst_ctls_proc);
		_mg->proc_dir.files.inst_ctls_proc = NULL;
	}
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.version_proc);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.cflags_proc);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.dict_sign_proc);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.isnt_list);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.echo_proc);
	/* Remove /proc/nvmeibc/tpv/ before removing its parent /proc/nvmeibc/.
	 * The per-TPV subdirs are gone by now (nvmeibc_tpv_detach_all_for_inst
	 * was called during volume shutdown, which invoked proc_deregister). */
	nvmeibc_tpv_io_exit();
	nvmeibc_tpv_proc_destroy_root();
	remove_proc_entry(_mg->proc_dir.root_name, NULL);
	NFOUT;
}

int main_module_single_instance_globals_init(void)
{
	int rv = 0;
	struct t_md_area *md = &mod_globals.md_area;
	memset(&mod_globals, 0 , sizeof(mod_globals));
	mod_globals.proc_dir.root_name = "nvmeibc";		 // Daniel: Consider using (THIS_MODULE->name)
	nvmeibc_state_promote(NVMEIBC_MOD_STATE_INITIALIZING);
	rv = t_main_clnt_sched_create(&mod_globals.sched, "mod", 0);
	if (!rv)
		rv = nvmeibc_module_procs_create(&mod_globals);
	if (!rv)
		rv = __t_md_area_alloc(md);
	if (!rv)
		rv = nvmeibc_tpv_io_init();
	nvmeibc_cinst_array_init();
	return rv;
}

void main_module_single_instance_globals_destroy(void)
{
	struct t_md_area *md = &mod_globals.md_area;
	t_main_clnt_sched_disable(&mod_globals.sched);		// Prevent new tasks from incomming via the /proc
	nvmeibc_module_procs_destroy(&mod_globals);
	__t_md_area_free(md);
	nvmeibc_cinst_array_destroy();
	t_main_clnt_sched_destroy(&mod_globals.sched);
}

const char * nvmeibc_get_module_proc_dir_name(void)
{
	return mod_globals.proc_dir.root_name;
}

struct proc_dir_entry *nvmeibc_get_module_proc_dir_entry(void)
{
	return mod_globals.proc_dir.root;
}

struct t_main_clnt_sched * nvmeibc_get_module_sched(void)
{
	return &mod_globals.sched;
}

#pragma pop_macro("__FILE_LITERAL__")
