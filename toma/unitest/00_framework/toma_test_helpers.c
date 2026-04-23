/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "nvmeibt_global.h"
#include "utils/nvmeibt_bm.h"

void TEST_init(void)
{
	nvmeibt_global_ctx_alloc();
	nvmeibt_bm_create();
}
