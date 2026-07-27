/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#ifndef NVMEIB_JDR_PROC_H
#define NVMEIB_JDR_PROC_H

#include "kr_incs.h"
#include "nvmeib_jdr.h"

typedef void    nvmeib_jdr_proc_fill_t(struct jdr *jdr, void *arg);
typedef ssize_t nvmeib_jdr_proc_write_t(void *arg, const char __user *buf,
					size_t count, loff_t *ppos);

struct nvmeib_jdr_procfs_ent;

__attribute__((nonnull(1, 2, 3)))
struct nvmeib_jdr_procfs_ent *nvmeib_jdr_proc_create(const char *name,
						     struct proc_dir_entry *dir,
						     nvmeib_jdr_proc_fill_t *fill,
						     nvmeib_jdr_proc_write_t *write,
						     void *arg);

void nvmeib_jdr_proc_remove(struct nvmeib_jdr_procfs_ent *ent);

typedef void nvmeib_jdr_proc_reset_t(void *arg);

ssize_t nvmeib_jdr_proc_write_reset(nvmeib_jdr_proc_reset_t *reset, void *arg,
				     const char __user *buf, size_t count);

#endif /* NVMEIB_JDR_PROC_H */
