/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_KOBJ_H
#define NVMEIB_KOBJ_H

#include "kr_incs.h"

struct nvmeib_kobj_file {
	const char *name;
	ssize_t (*show)(void *priv, char *buf);
	ssize_t (*store)(void *priv, const char *buf, size_t count);
};

struct nvmeib_kobj_params {
   struct kobject *parent;
   const char *dir_name;
   struct nvmeib_kobj_file *files;
   int n_files;
   void *priv;
};

struct nvmeib_kobj;
struct nvmeib_kobj *nvmeib_kobj_create(struct nvmeib_kobj_params *params);
void nvmeib_kobj_free(struct nvmeib_kobj *obj);

#endif

