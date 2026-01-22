/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_CORE_COMMON_H
#define NVMEIBC_CORE_COMMON_H

struct t_core_clnt_globals {
	struct wd_obj *wd_commands;		// client watch dog
	struct wd_obj **pcpu_wds;		// watchdogs for per-cpu chs
	struct nvmeib_intr_shaper *intr_shaper;
	const char *clnt_inst_name;		// Just for prints and name of ib_dev
	struct nvmeib_local_server *local_server;		// the local server if any
	struct t_core_clnt_nvmeof_inst {
		struct nvmeib_public_procfs_ent *proc;
		struct list_head disks;
		struct mutex lock;
	} nvme_of;
	struct t_core_clnt_ib {
		struct ib_sa_client sa_cli;
		struct ib_client cli;
		bool sa_registered;
		bool registered;
		bool was_notification_set;
		enum rdma_link_layer selected_link_layer;
	} ib;
	struct t_core_clnt_devs_lists { /* Stores the list of devices and ports to use */
		struct list_head all;				// List of dev currently detected
		struct list_head used;				// Stores the list of devices and ports to use
		struct list_head unused;
	} devs_lists;
	struct nvmeibc_jam  *c_jam;		/* Manager of jams of all disks*/
	struct disk_globals *dg;		/* Globals of nvmeibc_disk.c*/
};

struct nvmeibc_cinst_params_core;
int  t_core_clnt_globals_create( const struct nvmeibc_cinst_params_core *p, const char *clnt_inst_name);
void t_core_clnt_globals_destroy(const struct nvmeibc_cinst_params_core *p);
int  t_core_clnt_globals_params_update(struct nvmeibc_cinst_params_core *p, const char *ioctl);	// Respond to ioctl which updates params
void t_core_clnt_globals_params_free(struct nvmeibc_cinst_params_core *p);

struct t_core_clnt_globals * __get_from_params_core_globals_container(
				const struct nvmeibc_cinst_params_core *p);

void nvmeibc_cinst_prep_upgrade_shutdown(const struct nvmeibc_cinst_params_core *p);

#define nvmeibc_cinst_get_core_p(obj) ((obj)->cips)
#define nvmeibc_cinst_get_core_g(obj) __get_from_params_core_globals_container(nvmeibc_cinst_get_core_p(obj))
#define nvmeibc_cinst_get_core_m(obj) nvmeibc_isnt_params_core2main(nvmeibc_cinst_get_core_p(obj))

#endif
