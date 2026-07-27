#include "kr_incs.h"
#include "kr_version.h"
#include "u.h"

struct dc_proc_file {
	dc_proc_chng_cb *chng;
	void *arg;
	struct proc_dir_entry *dir;
	struct proc_dir_entry *ent;
	char *name;
};

#if KS_PDE_DATA
	#define get_priv_data(file) PDE_DATA(file_inode(file))
#else
	#define get_priv_data(file) PDE(file->f_path.dentry->d_inode)->data
#endif

static ssize_t proc_write(struct file *file, const char __user *userbuf,
	size_t len, loff_t *offset_p)
{
	char *buf;
	struct dc_proc_file *p = get_priv_data(file);
	int rv;

	if (p->chng == NULL)
		return -ENXIO;

	if (len >= 128 * 1024)
		return E2BIG;

	if ((buf = kmalloc(len + 1, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	if (copy_from_user(buf, userbuf, len)) {
		rv = -EFAULT;
	}
	else {
		buf[len] = '\0';
		rv = (*p->chng)(p->arg, buf, len);
	}

	if (rv > 0)
		*offset_p += rv;
	kfree(buf);
	return rv;
}

void * dc_proc_create(char *name, struct proc_dir_entry *dir,
	dc_proc_chng_cb *chng, void *arg)
{
	static struct file_operations fops = {.write = proc_write};
	const int mode = 0200; 
	struct dc_proc_file *p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (p) {
		p->name = kstrdup(name, GFP_KERNEL);
		p->ent = proc_create_data(name, mode, dir, &fops, p);
		if (!p->ent) {
			kfree(p->name);
			kfree(p);
		}
		p->chng = chng;
		p->arg = arg;
		p->dir = dir;
	}
	return p;
}

void dc_proc_remove(void *p)
{
	struct dc_proc_file *q = p;
	remove_proc_entry(q->name, q->dir);
	kfree(q->name);
	kfree(q);
}

int start_server_thread(struct workqueue_struct **wq, const char *name)
{
	int rv;

	FIN;
	trace("starting %s IP thread...\n", name);
	*wq = alloc_ordered_workqueue("dc_ip_net", WQ_MEM_RECLAIM);
	if (*wq == NULL) {
		trace("unable to launch %s IP thread\n", name);
		rv = -ENOMEM;
	}
	else
		rv = 0;
	FOUT;
	return rv;
}

