/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_PROC_CLI
#define NVMEIBC_PROC_CLI

struct msgloop_procfs_ent;

void *nvmeib_cli_init(void);
void  nvmeib_cli_remove(void *cli);
int   nvmeib_cli_send_proc(void *cli, void *msg, size_t len, struct msgloop_procfs_ent *ent);
#endif