/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_MMAP_H
#define NVMEIB_MMAP_H

struct mmap_procfs_ent;
typedef int nvmeib_public_mmap_table_callback_t(void *arg, unsigned long pg_offset,
										 struct page **page);
typedef void nvmeib_public_mmap_on_last_t(void *arg);

typedef ssize_t nvmeib_public_mmap_read_t(void *arg, char __user *, size_t, loff_t *, struct file *);
typedef int nvmeib_public_mmap_open_t(struct inode *, struct file *, void *);
typedef int nvmeib_public_mmap_release_t(struct inode *, struct file *);

struct mmap_procfs_ent *nvmeib_public_mmap_create(char *name,
	struct proc_dir_entry *dir, 
	nvmeib_public_mmap_table_callback_t *cb, void *arg,
	nvmeib_public_mmap_on_last_t *on_last_cb,
	nvmeib_public_mmap_read_t *read_fn,
	nvmeib_public_mmap_open_t *open_fn,
	nvmeib_public_mmap_release_t *release_fn,
	int mode);
void nvmeib_public_mmap_release(struct mmap_procfs_ent *p);
void nvmeib_public_mmap_remove(struct mmap_procfs_ent *p);
void nvmeib_public_mmap_remove_ent(struct mmap_procfs_ent *p);

#endif /* NVMEIB_MMAP_H */


