/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_CORE_IBDEV_H
#define NVMEIBC_CORE_IBDEV_H

// Default core layer params for instance are taken from module params.
void nvmeibc_core_ibdev_fill_cinst_params_from_module_params(struct nvmeibc_cinst_params_core* p);

void t_core_clnt_globals_init_ibdev(const struct nvmeibc_cinst_params_core *p);

// Create/Destroy ib connection between client instance and server
static int clnt_start_ib_work_fn(void /*const struct nvmeibc_cinst_params_core*/ * p);	// On main-wq
static void remove_ib(const struct nvmeibc_cinst_params_core *p);

void nvmeibc_core_set_local_server_notification(const struct nvmeibc_cinst_params_core *p, bool is_on);	// Set notiffications on or off

#endif
