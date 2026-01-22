/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MAIN_IOCTLS_H
#define NVMEIBC_MAIN_IOCTLS_H
/* Ioctls to main module */
#define MAIN_IOCTL_MARKER '%'			// Each ioctl starts with this specific marker
int nvmeibc_main_ioctl(struct nvmeibc_control_api *cc_api, const char *cmd);
int nvmeibc_module_ioctl(const char *cmd);	// Given throuhg module cli, not through instance cli

#endif
