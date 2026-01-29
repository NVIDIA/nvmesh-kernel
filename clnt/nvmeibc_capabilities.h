/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_CAPABILITIES_H
#define NVMEIBC_CAPABILITIES_H

#include <linux/types.h>
const char* nvmeibc_get_capabilities(void);
ssize_t nvmeibc_get_compile_flags(char *buffer, size_t len);

#endif //NVMEIBC_CAPABILITIES_H
