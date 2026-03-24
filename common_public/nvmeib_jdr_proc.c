/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

#include "common/kr_incs.h"
#include "nvmeib_jdr_proc.h"

struct nvmeib_jdr_procfs_ent {
	nvmeib_jdr_proc_fill_t  *fill;
	nvmeib_jdr_proc_write_t *write;
	void			*arg;
	struct proc_dir_entry   *ent;
};

#ifdef __KERNEL__
#include "nvmeibm_trace.h"
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static int jdr_proc_show(struct seq_file *m, void *v)
{
	struct nvmeib_jdr_procfs_ent *p = m->private;
	struct jdr jdr = jdr_make_seq(m);

	p->fill(&jdr, p->arg);
	jdr_finalize(&jdr);
	return 0;
}

static int jdr_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, jdr_proc_show, file_get_priv_data(file));
}

static ssize_t jdr_proc_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct nvmeib_jdr_procfs_ent *p = file_get_priv_data(file);

	return p->write(p->arg, buf, count, ppos);
}

#if !KS_HAS_PROC_FS
static const struct file_operations jdr_proc_fops = {
	.open    = jdr_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write   = jdr_proc_write,
};
#else
static const struct proc_ops jdr_proc_fops = {
	.proc_open    = jdr_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
	.proc_write   = jdr_proc_write,
};
#endif

struct nvmeib_jdr_procfs_ent *nvmeib_jdr_proc_create(const char *name,
						     struct proc_dir_entry *dir,
						     nvmeib_jdr_proc_fill_t *fill,
						     nvmeib_jdr_proc_write_t *write,
						     void *arg)
{
	struct nvmeib_jdr_procfs_ent *p;
	int mode = 0444 | (write ? 0200 : 0);

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return NULL;

	p->fill  = fill;
	p->write = write;
	p->arg   = arg;
	p->ent   = proc_create_data(name, mode, dir, &jdr_proc_fops, p);
	if (!p->ent) {
		kfree(p);
		return NULL;
	}
	return p;
}
EXPORT_SYMBOL(nvmeib_jdr_proc_create);

void nvmeib_jdr_proc_remove(struct nvmeib_jdr_procfs_ent *p)
{
	if (!p)
		return;
	proc_remove(p->ent);
	kfree(p);
}
EXPORT_SYMBOL(nvmeib_jdr_proc_remove);

ssize_t nvmeib_jdr_proc_write_reset(nvmeib_jdr_proc_reset_t *reset, void *arg,
				     const char __user *buf,
				     size_t count)
{
	char kbuf[16];
	int val;

	if (count == 0 || count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (kstrtoint(kbuf, 10, &val) || val != 0)
		return -EINVAL;

	reset(arg);
	return count;
}
EXPORT_SYMBOL(nvmeib_jdr_proc_write_reset);

#else /* !__KERNEL__ — for now only to support simulation */

static struct nvmeib_jdr_procfs_ent nvmeib_jdr_proc_dummy;

struct nvmeib_jdr_procfs_ent *nvmeib_jdr_proc_create(const char *name,
						     struct proc_dir_entry *dir,
						     nvmeib_jdr_proc_fill_t *fill,
						     nvmeib_jdr_proc_write_t *write,
						     void *arg)
{
	(void)name;
	(void)dir;
	(void)fill;
	(void)write;
	(void)arg;
	return &nvmeib_jdr_proc_dummy;
}

void nvmeib_jdr_proc_remove(struct nvmeib_jdr_procfs_ent *ent)
{
	(void)ent;
}

ssize_t nvmeib_jdr_proc_write_reset(nvmeib_jdr_proc_reset_t *reset, void *arg,
				     const char __user *buf, size_t count)
{
	(void)reset;
	(void)arg;
	(void)buf;
	return (ssize_t)count;
}

#endif /* __KERNEL__ */
