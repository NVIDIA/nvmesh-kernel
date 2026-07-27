#include "nvmeiba_infra.h"

/******************** Tiny read only proc files *******************************/
struct nvmeiba_proc_file {
	nvmeiba_proc_read_cb *fill;				// Read function
	void *arg;								// Context
	struct proc_dir_entry *dir, *ent;		// Parent dir and this entry
	char *name;								// Needed for removal
};

static ssize_t __proc_read(struct file *file, char __user *userbuf, size_t len, loff_t *offset_p)
{
	char *buf;
	const loff_t offset = *offset_p;
	size_t req_buf_size = len + offset + PAGE_SIZE;			// Daniel! Extremely important to add 1 page (or something) on top of len + offset or else read of proc files might be truncated, See commit message
	size_t bufsize = min((size_t)round_up(req_buf_size, PAGE_SIZE), (size_t)(32*PAGE_SIZE));
	ssize_t copysize;
	struct nvmeiba_proc_file *p = file_get_priv_data(file);

	if (p->fill == NULL)
		return -ENXIO;

	if ((buf = vmalloc(bufsize)) == NULL)
		return -ENOMEM;

	copysize = (*p->fill)(p->arg, buf, bufsize);
	if ((copysize > 0) && (offset > 0))
		copysize = (offset >= copysize) ? 0 : (copysize - offset);
	if (copysize > (ssize_t)len)
		copysize = (ssize_t)len;
	if (copysize > 0) {
		if (copy_to_user(userbuf, buf+offset, copysize))
			copysize = -EFAULT;
	}
	if (copysize > 0)
		*offset_p += copysize;
	vfree(buf);
	return copysize;
}

//static ssize_t __proc_write(struct file *file, const char __user *userbuf, size_t len, loff_t *offset_p){(void)file; (void)userbuf; (void)len; (void)offset_p;return -ENXIO;		/* Not suported yet*/}

void *nvmeiba_proc_create(char *name, struct proc_dir_entry *dir, nvmeiba_proc_read_cb *fill, void *arg)
{
	#if !KS_HAS_PROC_FS
		static struct file_operations fops = {.read = __proc_read};//, .write = __proc_write};
	#else
		static struct proc_ops fops = {.proc_read = __proc_read, .proc_lseek = default_llseek};
	#endif
	const int mode = 0444;		// Readonly
	struct nvmeiba_proc_file *p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (p) {
		p->name = kstrdup(name, GFP_KERNEL);
		p->ent = proc_create_data(name, mode, dir, &fops, p);
		if (!p->ent) {
			kfree(p->name);
			kfree(p);
		}
		p->fill = fill;
		p->arg = arg;
		p->dir = dir;
	}
	return p;
}

void nvmeiba_proc_remove(void *p)
{
	struct nvmeiba_proc_file *_p = p;
	remove_proc_entry(_p->name, _p->dir);
	kfree(_p->name);
	kfree(_p);
}

static bool verbose_debug = !NVMESH_IS_PRODUCTION_COMPILATION;
module_param(verbose_debug, bool, 0644);
MODULE_PARM_DESC(verbose_debug, "affects logging level");

bool is_verbose_mode(void)
{
	return verbose_debug;
}
