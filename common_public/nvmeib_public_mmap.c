/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include <linux/proc_fs.h>
#include <linux/mm.h>

#include "nvmeib_public_mmap.h"
#include "nvmeib_utils.h"
#include "nvmeibp_trace.h"

/*
 * This proc file implements mmap of kernel memory to user space.
 * The mmap() is implemented in a on-demand scheme by overriding the vma's
 * .fault() method. This scheme is used for two reasons:
 * (1) The mmaped memory is not necessarily physically nor virtually contiguous,
 *     thus the resoution of virt to phys must be done per page.
 * (2) The alternative scheme of remap_pfn_range() which mmaps pfn rather than
 *     page-struct does not allow to register this range as IB mr as it sets
 *     VM_PFNMAP flag in vma->vm_flags. This flag is checked to be OFF by
 *     get_user_pages() which called when registering mr to IB.
 */

struct mmap_procfs_ent {
	void *arg;
	nvmeib_public_mmap_table_callback_t *cb;
	struct proc_dir_entry *dir;
	char *name;
	struct proc_dir_entry *ent;

	atomic_t ref_cnt;
	atomic_t dying;
	nvmeib_public_mmap_on_last_t *on_last_cb;
	nvmeib_public_mmap_read_t *read_fn;
	nvmeib_public_mmap_open_t *open_fn;
	nvmeib_public_mmap_release_t *release_fn;
};

#ifndef NVMEIB_MMAP_VM_RESERVED
#	if K_CHECK_VER(2,6,33)
#		define NVMEIB_MMAP_VM_RESERVED	(VM_DONTEXPAND | VM_DONTDUMP)
#	else
#		define NVMEIB_MMAP_VM_RESERVED	(VM_DONTEXPAND)
#	endif
#endif

/* the open method is invoked anytime a process forks
   and creates a new reference to the VMA.  */
static void mmap_open(struct vm_area_struct *vma)
{
	struct mmap_procfs_ent *p = (struct mmap_procfs_ent *)vma->vm_private_data;

	if (!atomic_inc_not_zero_hint(&p->ref_cnt, 1)) {
		_NE(error_nvmeib_mmap_mmap_open, "OOPS: Fail to create new reference (fork) to vma, mmap dev @P_NAME, "
		   "file being removed", p->name);
		WARN_ON(1);
	}

	_NT(trace_nvmeib_mmap_mmap_open, "VMA open @P_NAME (refcnt=@REFCNT), virt @VM_START, phys @VM_START", p->name,
	   atomic_read(&p->ref_cnt), vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

/* When an area is destroyed, the kernel calls its close operation.
   The area is opened and closed exactly once by each process that uses it */
static void mmap_close(struct vm_area_struct *vma)
{
	struct mmap_procfs_ent *p = (struct mmap_procfs_ent *)vma->vm_private_data;
	int ref_cnt;

	if (!(ref_cnt = atomic_dec_return(&p->ref_cnt)))
		p->on_last_cb(p->arg);

	_NT(trace_nvmeib_mmap_mmap_close, "VMA close @P_NAME (refcnt=@REFCNT)", p->name, atomic_read(&p->ref_cnt));

	BUG_ON(ref_cnt < 0);
}

/* fault (nopage) is called the first time a memory area
 * is accessed which is not in memory i.e. not mapped
 */
#if !KS_HAS_VM_FAULT_T
typedef int vm_fault_t;
#endif

#if !KS_FAULT_EXPECTS_VM_AREA
static vm_fault_t mmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#else
static vm_fault_t mmap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
#endif
	struct mmap_procfs_ent *p = (struct mmap_procfs_ent *)vma->vm_private_data;
	struct page *pg = NULL;

	/*_ND(mmap_fault_d1, "VMA fault operation, virt-start @INT64_HEX, phys @INT64_HEX, "
	   "vmf->virtual_addres = @PTR",
	   vma->vm_start, vmf->pgoff << PAGE_SHIFT, vmf->virtual_address);*/

	//nvmeibs_disk_mmap_fault()
	if (p->cb(p->arg, vmf->pgoff, &pg) < 0) {
		_NE(error_nvmeib_mmap_mmap_fault, "Failed VMA fault op @P_NAME: page offset @PGOFF", p->name, vmf->pgoff);
		return VM_FAULT_SIGBUS;
	}

	/*_ND(mmap_fault_d2, "VMA fault operation, virt-start @INT64_HEX, phys @INT64_HEX, "
	   "vmf->virtual_addres = @PTR, k_vaddr = @INT64_HEX",
	   vma->vm_start, vmf->pgoff << PAGE_SHIFT, vmf->virtual_address, k_vaddr);
	 */

	//increment the reference count of this page
	get_page(pg);

	//satisfy the fault
	vmf->page = pg;

	return 0;
}

struct vm_operations_struct mmap_vm_ops = {
	.open =     mmap_open,
	.close =    mmap_close,
	.fault =    mmap_fault,
};

static inline bool nvmeib_is_cow_mapping(KS_VM_FLAGS_T flags)
{
	return (flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE;
}

static int nvmeib_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mmap_procfs_ent *p = file_get_priv_data(file);

	unsigned long vsize;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long end_pg_offset;
	struct page *page = NULL;
	int rv;

    if (atomic_read(&p->dying) || !atomic_inc_not_zero_hint(&p->ref_cnt, 1)) {
		_NE(error_nvmeib_mmap_nvmeib_mmap, "Fail to mmap dev @P_NAME, file being removed", p->name);
        return -ENOENT;
	}

	/* requested mapped area size in bytes, rounded up to page size.
	   vm_start and vm_end are already page aligned hence, vsize is
	   'length' argument in mmap() sys-call, rounded-up to page size */
	vsize  = vma->vm_end - vma->vm_start;

	_ND(trace_nvmeib_mmap_nvmeib_mmap, "VMA .mmap operation, "
	   "virt start @VM_START, virt end @VM_END, virt size @VSIZE [bytes],\n"
	   "page offset in phys mem @VM_PGOFF, byte offset = @OFFSET_LONG,\n "
	   "flags @VM_FLAGS",
	   vma->vm_start, vma->vm_end, vsize,
	   vma->vm_pgoff, offset,
	   vma->vm_flags);

	if (nvmeib_is_cow_mapping(vma->vm_flags)) {
		_NE(error_1_nvmeib_mmap_nvmeib_mmap, "Mapping is private (copy-on-write) but must be shared; "
		   "set mmap() 'flags' argument to MAP_SHARED");
		rv = -EINVAL;
		goto err;
	}

	//validate requested mmap length
	//(ab)use the callback with the offset to the last requested page
	end_pg_offset = vma->vm_pgoff + (vsize >> PAGE_SHIFT) - 1;
	//nvmeibs_disk_mmap_fault()
	if (p->cb(p->arg, end_pg_offset, &page) < 0) {
		_NE(error_2_nvmeib_mmap_nvmeib_mmap, "Failed VMA fault op @P_NAME: "
		   "Requested mmap area {offset=@OFFSET_LONG [bytes], length=@LENGTH_LONG [bytes]} "
		   "spans out memory region",
			p->name, vma->vm_pgoff << PAGE_SHIFT, vsize);
		rv = VM_FAULT_SIGBUS;
		goto err;
	}

	vma->vm_ops = &mmap_vm_ops;
#if KS_HAS_VM_FLAGS_SET
	vm_flags_set(vma, vma->vm_flags | NVMEIB_MMAP_VM_RESERVED);
#else // KS_HAS_VM_FLAGS_SET
	vma->vm_flags |= NVMEIB_MMAP_VM_RESERVED;
#endif // KS_HAS_VM_FLAGS_SET
	vma->vm_private_data = p;

	/* Since the open method is not invoked on the initial mmap,
	   we must call it explicitly if we want it to run. */
	//mmap_open(vma);
	_NT(trace_1_nvmeib_mmap_nvmeib_mmap, "VMA open @P_NAME (refcnt=@REFCNT), virt @VM_START, phys @VM_START", p->name,
	   atomic_read(&p->ref_cnt), vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);

	rv = 0;
	goto out;

err:
	if (!atomic_dec_return(&p->ref_cnt))
		p->on_last_cb(p->arg);

out:
	return rv;
}

static int mmap_proc_open(struct inode *inode, struct file *file)
{
	struct mmap_procfs_ent *p = file_get_priv_data(file);
	int rv = 0;

    if (atomic_read(&p->dying) || !atomic_inc_not_zero_hint(&p->ref_cnt, 1)) {
		_NE(error_nvmeib_mmap_mmap_proc_open, "Fail to open @P_NAME, file being removed", p->name);
        rv = -ENOENT;
	}
	else {		
		// If default open succeeded and user function is provided, call it
		if (p->open_fn) {
			if((rv = p->open_fn(inode, file, p->arg)) < 0) {
				_NE(error_nvmeib_mmap_mmap_proc_open2, "Fail to open @P_NAME, file being removed", p->name);
				goto out;
			}
		}
	}

out:
	return rv;
}

static int mmap_proc_release(struct inode *inode, struct file *file)
{
	struct mmap_procfs_ent *p = file_get_priv_data(file);
	int ref_cnt;
	int rv = 0;

	// Call user function first if provided
	if (p->release_fn) {
		rv = p->release_fn(inode, file);
	}

	// Then call default release logic
	ref_cnt = atomic_dec_return(&p->ref_cnt);
	_NT(trace_nvmeib_mmap_mmap_proc_release, "closed file @P_NAME (refcnt=@REFCNT)", p->name, ref_cnt);
	if (!ref_cnt)
		p->on_last_cb(p->arg);

	return rv;
}

static ssize_t nvmeib_mmap_read(struct file *file, char __user *userbuf,
			 size_t len, loff_t *offset_p)
{
	struct mmap_procfs_ent *p = file_get_priv_data(file);
	if (!p || !p->read_fn) {
		return -ENODATA;
	} else
		return (*p->read_fn)(p->arg, userbuf, len, offset_p, file);
}

#if !KS_HAS_PROC_FS
	static struct file_operations proc_fops = {
		.open = mmap_proc_open,
		.release = mmap_proc_release,
		.mmap = nvmeib_mmap
	};

	static struct file_operations proc_fops_read = {
		.open = mmap_proc_open,
		.release = mmap_proc_release,
		.mmap = nvmeib_mmap,
		.read = nvmeib_mmap_read,
	};
#else
	static struct proc_ops proc_fops = {
		.proc_open = mmap_proc_open,
		.proc_release = mmap_proc_release,
		.proc_mmap = nvmeib_mmap,
		.proc_lseek = default_llseek,
	};

	static struct proc_ops proc_fops_read = {
		.proc_open = mmap_proc_open,
		.proc_release = mmap_proc_release,
		.proc_mmap = nvmeib_mmap,
		.proc_read = nvmeib_mmap_read,
		.proc_lseek = default_llseek,
	};
#endif 

struct mmap_procfs_ent *nvmeib_public_mmap_create(char *name,
	struct proc_dir_entry *dir,
	nvmeib_public_mmap_table_callback_t *cb, void *arg,
	nvmeib_public_mmap_on_last_t *on_last_cb,
	nvmeib_public_mmap_read_t *read_fn,
	nvmeib_public_mmap_open_t *open_fn,
	nvmeib_public_mmap_release_t *release_fn,
	int mode)
{
	struct mmap_procfs_ent *p;
	void *fops = &proc_fops;

	if (!on_last_cb)
		return NULL;
	if (cb != NULL)
		mode |= 0200;
	p = kzalloc(sizeof *p, GFP_KERNEL);
	if (!p)
		return NULL;

	p->cb = cb;
	p->arg = arg;
	p->dir = dir;
	p->name = kstrdup(name, GFP_KERNEL);
	if (!p->name)
		goto err_free_proc;

	p->on_last_cb = on_last_cb;
	p->open_fn = open_fn;
	p->release_fn = release_fn;
	if (read_fn) {
		fops = &proc_fops_read;
		p->read_fn = read_fn;
		p->release_fn = release_fn;
	}
	atomic_set(&p->dying, 0);
	atomic_set(&p->ref_cnt, 1);
	p->ent = proc_create_data(name, mode, dir, fops, p);
	if (!p->ent)
		goto err_free_name;

	return p;

err_free_name:
	kfree(p->name);
err_free_proc:
	kfree(p);

	return NULL;
}
EXPORT_SYMBOL(nvmeib_public_mmap_create);

void nvmeib_public_mmap_release(struct mmap_procfs_ent *p)
{
	int dying;
	int ref_cnt;

	if (p == NULL)
		return;

	if ((dying = atomic_inc_return(&p->dying)) == 1) {
		ref_cnt = atomic_dec_return(&p->ref_cnt);
		_NT(trace_nvmeib_mmap_nvmeib_public_mmap_release, "mmap release @P_NAME (refcnt=@REFCNT)", p->name, ref_cnt);
		if (!ref_cnt)
			p->on_last_cb(p->arg);
	}
	else
		_NT(trace_1_nvmeib_mmap_nvmeib_public_mmap_release, "@P_NAME already dying (@DYING)", p->name, dying);
}
EXPORT_SYMBOL(nvmeib_public_mmap_release);

void nvmeib_public_mmap_remove(struct mmap_procfs_ent *p)
{
	if (p == NULL)
		return;
	if (p->ent) {
		_NT(trace_nvmeib_mmap_nvmeib_public_mmap_remove, "mmap remove /proc entry @P_NAME", p->name);
		remove_proc_entry(p->name, p->dir);
	}
	kfree(p->name);
	kfree(p);
}
EXPORT_SYMBOL(nvmeib_public_mmap_remove);

void nvmeib_public_mmap_remove_ent(struct mmap_procfs_ent *p)
{
	if (p == NULL)
		return;
	if (p->ent) {
		_NT(trace_nvmeib_mmap_nvmeib_public_mmap_remove_ent, "mmap remove /proc entry @P_NAME", p->name);
		remove_proc_entry(p->name, p->dir);
		p->ent = NULL;
	}
}
EXPORT_SYMBOL(nvmeib_public_mmap_remove_ent);
