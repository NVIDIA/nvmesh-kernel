/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MODULE_MAIN_H
#define NVMEIBC_MODULE_MAIN_H
/************************ Global module variables/methods *********************/
int  main_module_single_instance_globals_init(   void);
void main_module_single_instance_globals_destroy(void);

enum nvmeibc_mod_state nvmeibc_get_state(void);
void nvmeibc_state_promote(enum nvmeibc_mod_state to);
const char *           nvmeibc_get_module_proc_dir_name( void);
struct proc_dir_entry *nvmeibc_get_module_proc_dir_entry(void);
struct t_main_clnt_sched * nvmeibc_get_module_sched(void);

#define nvmeibc_assert_on_module_wq()  ({ BUG_ON(!t_main_clnt_sched_on_main_wq(nvmeibc_get_module_sched(), true)); })
void *nvmeibc_get_md_read_dummy_area( int *n_pages);
void *nvmeibc_get_md_write_dummy_area(int *n_pages);

/*************************** Add/Remove Clnt instance *************************/
struct nvmeibc_cinst_params;
void nvmeibc_instance_init_module_params(const struct nvmeibc_cinst_params *p);
void nvmeibc_instance_free_module_params(const struct nvmeibc_cinst_params *p);
int  nvmeibc_instance_create_on_modwq( const struct nvmeibc_cinst_params *p);		// Actual execution of add/del on module main_wq
int  nvmeibc_instance_destroy_on_modwq(const struct nvmeibc_cinst_params *p);
int  nvmeibc_instance_clean_all_vols(  const struct nvmeibc_cinst_params *p, struct nvmeibc_multi_completion *on_finish);

enum module_work_types {					// Enum and bit field
	mw_illegal =    0,
	mw_is_blocking_flag = 0x10,				// If flag is on, this work is blocking
	mw_is_all_inst_flag = 0x20,				// If flag is on, this work is done for all instances
	mw_inst_ioctl =                      1,	// Instance ioctl: add/remove/configure isntance work by ioctl string.
	mw_inst_ioctl_blocking = mw_inst_ioctl|mw_is_blocking_flag,	// Same as above, but blocks user space caller until completion
	mw_inst_add_blocking =               2|mw_is_blocking_flag,	// Instance add/remove work by blocking call
	mw_inst_del_blocking =               3|mw_is_blocking_flag,	// Instance add/remove work by blocking call. If volumes are attach, also, has to wait for them to detach
	mw_clean_all_vols_of_all_inst =      4|mw_is_blocking_flag|mw_is_all_inst_flag,	// Optimization of shutdown: DEtach all volumes of all instances in parallel. Prevent attaches of volumes.  Instances removal is serialized but volumes detach of all instances can be in parallel
	mw_inst_del_all_blocking =           5|mw_is_blocking_flag|mw_is_all_inst_flag, // Remove all instances
};
int nvmeibc_instance_do_blocking(const struct nvmeibc_cinst_params *p, enum module_work_types t, bool is_upgrade);

#endif


