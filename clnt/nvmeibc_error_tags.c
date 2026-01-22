/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_error_tags.h"

ssize_t nvmeibc_error_tags_info(void *_ctx, char *buffer, size_t len)
{
	bool const dump_all_cpus = true;
	struct charvec const jdr_buffer = {.base = buffer, .len = len};
	ssize_t const count = nvmesh_error_tags_json_serialize(jdr_buffer, dump_all_cpus, __start_nvmeibc_error_tags, __stop_nvmeibc_error_tags);
	(void)(_ctx);
	return count;
}
