#include "common/kr_incs.h"
#include "nvmeib_public_procfs.h"
#include "nvmeibm_trace.h"

#define MAX_BUF_PAGES	1024

struct nvmeib_public_procfs_ent {
	proc_fill_t *fill;
	proc_chng_t *chng;
	void *arg;
	struct proc_dir_entry *dir;
	char *name;
	struct proc_dir_entry *ent;
	char *buf;
	size_t len;
	struct mutex mutex;
	atomic_t num_open;
};

static ssize_t generic_proc_read(struct file *file, char __user *userbuf,
	size_t len, loff_t *offset_p)
{
	char *buf;
	struct nvmeib_public_procfs_ent *p = file_get_priv_data(file);
	loff_t offset = *offset_p;
	size_t bufsize;
	ssize_t copysize;

	if (p->fill == NULL)
		return -ENXIO;

	mutex_lock(&p->mutex);

	/* new read && prev reader didnt read till EOF */
	if (offset == 0 && p->buf != NULL) {
		vfree(p->buf);
		p->buf = NULL;
	}

	/* ensure p->fill has enough buf space to fill ALL its data in one go */
	if (p->buf == NULL) {
		if (offset != 0) {
			/* return EOF (post short-read) */
			copysize = 0;
			goto unlock;
		}
		bufsize = (p->len + PAGE_SIZE + PAGE_SIZE/2) & PAGE_MASK;
		if (bufsize > PAGE_SIZE*MAX_BUF_PAGES)
			bufsize = PAGE_SIZE*MAX_BUF_PAGES;
		for (; bufsize <= PAGE_SIZE*MAX_BUF_PAGES; bufsize <<= 1) {
			if ((buf = vmalloc(bufsize)) == NULL) {
				copysize = -ENOMEM;
				goto unlock;
			}
			p->len = (*p->fill)(p->arg, buf, bufsize);
			if (p->len < bufsize-1) {
				/* consider null terminator (scnprintf's rv=size includes it) */
				break;
			}
			vfree(buf);
			buf = NULL;
		}
		if (buf == NULL) {
			copysize = -EFBIG;
			goto unlock;
		}
		p->buf = buf;
	}
	copysize = p->len - offset;
	if (copysize > (ssize_t)len)
		copysize = (ssize_t)len;
	if (copysize > 0) {
		if (copy_to_user(userbuf, p->buf+offset, copysize))
			copysize = -EFAULT;
	}
	if (copysize > 0)
		*offset_p += copysize;

	/* some apps dont read once more after short-read (i.e. till EOF) so this
	   case apply for both app types */
	if (copysize < (ssize_t)len) {
		vfree(p->buf);
		p->buf = NULL;
	}
unlock:
	mutex_unlock(&p->mutex);
	return copysize;
}

static ssize_t generic_proc_write(struct file *file, const char __user *userbuf,
	size_t len, loff_t *offset_p)
{
	char *buf;
	struct nvmeib_public_procfs_ent *p = file_get_priv_data(file);
	int rv;

	if (p->chng == NULL)
		return -ENXIO;

	if (len >= 128*1024)
		return E2BIG;

	if ((buf = kmalloc(len+1, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	if (copy_from_user(buf, userbuf, len)) {
		rv = -EFAULT;
	} else {
		buf[len] = '\0';
		rv = (*p->chng)(p->arg, buf, len);
	}

	if (rv > 0)
		*offset_p += rv;
	kfree(buf);
	return rv;
}

static int generic_proc_open(struct inode *inode, struct file *file) {
	struct nvmeib_public_procfs_ent *p = file_get_priv_data(file);

	(void)inode;
	atomic_add(1, &p->num_open);
	return 0;
}

static int generic_proc_release(struct inode *inode, struct file *file) {
	struct nvmeib_public_procfs_ent *p = file_get_priv_data(file);

	(void)inode;
	atomic_sub(1, &p->num_open);
	return 0;
}

#if !KS_HAS_PROC_FS
	static struct file_operations proc_fops = {
		.read = generic_proc_read,
		.write = generic_proc_write,
		.open		= generic_proc_open,
		.release	= generic_proc_release
	};
#else
	static struct proc_ops proc_fops = {
		.proc_read = generic_proc_read,
		.proc_write = generic_proc_write,
		.proc_open		= generic_proc_open,
		.proc_release	= generic_proc_release,
		.proc_lseek = default_llseek,
	};
#endif

struct nvmeib_public_procfs_ent *nvmeib_public_proc_create(const char *name,
	struct proc_dir_entry *dir, proc_fill_t *fill, proc_chng_t *chng, void *arg)
{
	struct nvmeib_public_procfs_ent *p;
	int mode = 0;

	if (fill != NULL)
		mode |= 0444;
	if (chng != NULL)
		mode |= 0200;
	if (mode == 0)
		return NULL;
	if ((p = kzalloc(sizeof *p, GFP_KERNEL)) == NULL)
		return NULL;
	mutex_init(&p->mutex);
	p->fill = fill;
	p->chng = chng;
	p->arg = arg;
	p->dir = dir;
	p->name = kstrdup(name, GFP_KERNEL);
	p->buf = NULL;
	p->len = 1;
	if (!p->name)
		goto err_free_ent;

	p->ent = proc_create_data(name, mode, dir, &proc_fops, p);
	if (!p->ent)
		goto err_free_name;

	return p;

err_free_name:
	kfree(p->name);
err_free_ent:
	kfree(p);

	return NULL;
}
EXPORT_SYMBOL(nvmeib_public_proc_create);

void nvmeib_public_proc_remove(struct nvmeib_public_procfs_ent *p)
{
	if (p == NULL)
		return;
	remove_proc_entry(p->name, p->dir);
	kfree(p->name);
	kfree(p);
}
EXPORT_SYMBOL(nvmeib_public_proc_remove);

struct nvmeib_public_procfs_seq_ent {
	void *data;
	struct proc_dir_entry *dir;
	const char *name;
	struct proc_dir_entry *ent;
	const struct seq_operations *seq_ops;
};

static int proc_seq_open(struct inode *inode, struct file *file)
{
	int rv;
	struct nvmeib_public_procfs_seq_ent *seq_ent = file_get_priv_data(file);
	(void)inode;
	if (!(rv = seq_open(file, seq_ent->seq_ops))) {
		struct seq_file *seq_file = file->private_data;
		seq_file->private = seq_ent->data;
	}
	return rv;
}

static int proc_seq_release(struct inode *inode, struct file *file)
{
	struct nvmeib_public_procfs_seq_ent *seq_ent = file_get_priv_data(file);
	(void)seq_ent;
	(void)inode;
	return seq_release(inode, file);
}

#if !KS_HAS_PROC_FS
	static const struct file_operations proc_seq_fops = {
		.open		= proc_seq_open,
		.read		= seq_read,
		.llseek		= seq_lseek,
		.release	= proc_seq_release,
	};
#else
	static const struct proc_ops proc_seq_fops = {
		.proc_open		= proc_seq_open,
		.proc_read		= seq_read,
		.proc_lseek		= seq_lseek,
		.proc_release	= proc_seq_release,
	};
#endif

struct nvmeib_public_procfs_seq_ent *nvmeib_public_proc_create_seq_data(const char *name,
											  struct proc_dir_entry *dir,
											  const struct seq_operations *seq_ops,
											  void *data)
{
	struct nvmeib_public_procfs_seq_ent *seq_ent = NULL;

	if (!(seq_ent = kzalloc(sizeof(*seq_ent), GFP_KERNEL))) {
		goto out;
	}
	if (!(seq_ent->name = kstrdup(name, GFP_KERNEL)))
		goto err;

	seq_ent->data = data;
	seq_ent->seq_ops = seq_ops;
	seq_ent->dir = dir;
	seq_ent->ent = proc_create_data(name, 0444, dir, &proc_seq_fops, seq_ent);
	if (!seq_ent->ent)
		goto err;
	goto out;

err:
	kfree(seq_ent->name);
	kfree(seq_ent);

out:
	return seq_ent;
}
EXPORT_SYMBOL(nvmeib_public_proc_create_seq_data);

void nvmeib_public_proc_seq_remove(struct nvmeib_public_procfs_seq_ent *seq_ent)
{
	if (seq_ent == NULL)
		return;
	remove_proc_entry(seq_ent->name, seq_ent->dir);
	kfree(seq_ent->name);
	kfree(seq_ent);
}
EXPORT_SYMBOL(nvmeib_public_proc_seq_remove);
