/*
 * Software iWARP device driver for Linux
 *
 * Authors: Animesh Trivedi <atr@zurich.ibm.com>
 *          Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <linux/version.h>
#include <linux/scatterlist.h>
#include <linux/gfp.h>
#include <rdma/ib_verbs.h>
#include <linux/dma-mapping.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,10,0))
#include <linux/dma-map-ops.h>
#endif
#include <linux/slab.h>
#include <linux/pid.h>

#include "siw.h"
#include "siw_debug.h"

#if LINUX_SCHED_MM
#include <linux/sched/mm.h>
#endif

#if KS_HAS_MMAP_LOCK_FUNCTIONS
#include <linux/mmap_lock.h>
#if !KS_HAS_MMAP_WRITE_TRYLOCK
#define mmap_write_trylock(mm) (down_write_trylock(&(mm)->mmap_lock) != 0)
#endif
#else
#define mmap_write_lock(mm) down_write(&mm->mmap_sem)
#define mmap_write_unlock(mm) up_write(&mm->mmap_sem)
#define mmap_write_trylock(mm) down_write_trylock(&mm->mmap_sem)
#endif

/* smartass GCC hacks, operation, atomic or regular depending on operands */
static inline unsigned long ____sub(unsigned long diff, unsigned long *dest) {
	*dest -= diff;
	return *dest;
}

static inline unsigned long ____add(unsigned long diff, unsigned long *dest) {
	*dest += diff;
	return *dest;
}

static inline unsigned long ____deref(unsigned long *dest) {
	return *dest;
}

#define __sub_or_atomic_sub(diff, dest) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), atomic64_t*), \
		atomic64_sub, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), atomic_t*), \
		atomic_sub, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), unsigned long*), \
		____sub \
		,(void)0)))(diff, dest)

#define __add_or_atomic_add(diff, dest) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), atomic64_t*), \
		atomic64_add, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), atomic_t*), \
		atomic_add, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), unsigned long*), \
		____add \
		,(void)0)))(diff, dest)

#define __read_or_atomic_read(dest) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), atomic64_t*), \
		atomic64_read, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), atomic_t*), \
		atomic_read, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(dest), unsigned long*), \
		____deref \
		,(void)0)))(dest)


static void siw_umem_update_stats(struct work_struct *work)
{
	struct siw_umem *umem = container_of(work, struct siw_umem, work);
	struct mm_struct *mm_s = umem->mm_s;

	BUG_ON(!mm_s);

	mmap_write_lock(mm_s);
	__sub_or_atomic_sub(umem->num_pages, &mm_s->pinned_vm);
	mmap_write_unlock(mm_s);

	mmput(mm_s);

	kfree(umem->page_chunk);
	kfree(umem);
}

static void siw_free_plist(struct siw_page_chunk *chunk, int num_pages)
{
	struct page **p = chunk->p;

	while (num_pages--) {
		put_page(*p);
		p++;
	}
}

void siw_umem_release(struct siw_umem *umem)
{
	struct task_struct *task = get_pid_task(umem->pid, PIDTYPE_PID);
	int i, num_pages = umem->num_pages;

	for (i = 0; num_pages; i++) {
		int to_free = min_t(int, PAGES_PER_CHUNK, num_pages);
		siw_free_plist(&umem->page_chunk[i], to_free);
		kfree(umem->page_chunk[i].p);
		num_pages -= to_free;
	}
	put_pid(umem->pid);
	if (task) {
		struct mm_struct *mm_s = get_task_mm(task);
		put_task_struct(task);
		if (mm_s) {
			if (mmap_write_trylock(mm_s)) {
				__sub_or_atomic_sub(umem->num_pages, &mm_s->pinned_vm);
				mmap_write_unlock(mm_s);
				mmput(mm_s);
			} else {
				/*
				 * Schedule delayed accounting if 
				 * mm semaphore not available
				 */
				INIT_WORK(&umem->work, siw_umem_update_stats);
				umem->mm_s = mm_s;
				schedule_work(&umem->work);

				return;
			}
		}
	}
	kfree(umem->page_chunk);
	kfree(umem);
}

void siw_pbl_free(struct siw_pbl *pbl)
{
	kvfree(pbl);
}

static bool use_pbe_fixed_size = false;
module_param(use_pbe_fixed_size, bool, 0644);
MODULE_PARM_DESC(use_pbe_fixed_size, "Use fixed size buffers.");

//omril: @off is 'addr - mr->mem.va;'
u64 siw_pbl_get_buffer(struct siw_pbl *pbl, u64 off, int *len, int *idx)
{
	int i = idx ? *idx : 0;

	if (use_pbe_fixed_size && i == 0 && pbl->pbe_fixed_shift) {
		if (off > pbl->pbe[0].size /* && pbl->num_buf > 1 */) {
			i = ((off - pbl->pbe[0].size) >> pbl->pbe_fixed_shift) + 1;
		}
	}

	while (i < pbl->num_buf) {
		struct siw_pble *pble = &pbl->pbe[i];
		if (pble->pbl_off + pble->size > off) {
			u64 pble_off = off - pble->pbl_off;
			if (len)
				*len = pble->size - pble_off;
			if (idx)
				*idx = i;
			return pble->addr + pble_off;
		}
		i++;
	}
	return 0;
}

struct siw_pbl *siw_pbl_alloc(u32 num_buf)
{
	struct siw_pbl *pbl;
	int buf_size = sizeof(struct siw_pbl);

	if (num_buf == 0)
		return ERR_PTR(-EINVAL);

	buf_size += (num_buf * sizeof(struct siw_pble));

	pbl = kvzalloc(buf_size, GFP_KERNEL);
	if (!pbl) {
		dprint(DBG_MM | DBG_ON, 
			   ": failed alloc pbl of (%u * %d) %d bytes\n",
			   num_buf, (int)sizeof(struct siw_pble), buf_size);
		return ERR_PTR(-ENOMEM);
	}

	dprint(DBG_MM,
		   ": allocated pbl of %d bytes via %smalloc\n",
		   buf_size, is_vmalloc_addr(pbl) ? "v" : "k");

	pbl->max_buf = num_buf;

	return pbl;
}

struct siw_umem *siw_umem_get(u64 start, u64 len)
{
	struct siw_umem *umem;
	u64 first_page_va;
	unsigned long mlock_limit;
	int num_pages, num_chunks, i, rv = 0;

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	if (!len)
		return ERR_PTR(-EINVAL);

	first_page_va = start & PAGE_MASK;
	num_pages = PAGE_ALIGN(start + len - first_page_va) >> PAGE_SHIFT;
	num_chunks = (num_pages >> CHUNK_SHIFT) + 1;

	umem = kzalloc(sizeof *umem, GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);

	umem->pid = get_task_pid(current, PIDTYPE_PID);

	mmap_write_lock(current->mm);

	mlock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	if (num_pages + __read_or_atomic_read(&current->mm->pinned_vm) > mlock_limit) {
		dprint(DBG_ON|DBG_MM,
			": pages req: %d, limit: %lu, pinned: %lu\n",
			num_pages, mlock_limit, (unsigned long)__read_or_atomic_read(&current->mm->pinned_vm));
		rv = -ENOMEM;
		goto out;
	}
	umem->fp_addr = first_page_va;

	umem->page_chunk = kzalloc(num_chunks * sizeof(struct siw_page_chunk),
				   GFP_KERNEL);
	if (!umem->page_chunk) {
		rv = -ENOMEM;
		goto out;
	}
	for (i = 0; num_pages; i++) {
		int got, nents = min_t(int, num_pages, PAGES_PER_CHUNK);
		umem->page_chunk[i].p = kzalloc(nents * sizeof(struct page *),
						GFP_KERNEL);
		if (!umem->page_chunk[i].p) {
			rv = -ENOMEM;
			goto out;
		}
		got = 0;
		while (nents) {
			struct page **plist = &umem->page_chunk[i].p[got];

#if KS_GET_USER_PAGES_HAS_TASK_STRUCT
			rv = get_user_pages(current, current->mm,
                                            first_page_va, nents, 1, 1, plist, NULL);
#else
#if KS_GET_USER_PAGES_HAS_VMAS
			rv = get_user_pages(first_page_va, nents, 0, plist, NULL);
#else
			rv = get_user_pages(first_page_va, nents, 0, plist);
#endif
#endif
			if (rv < 0 )
				goto out;

			umem->num_pages += rv;
			__add_or_atomic_add(rv, &current->mm->pinned_vm);
			first_page_va += rv * PAGE_SIZE;
			nents -= rv;
			got += rv;
		}
		num_pages -= got;
	}
out:
	mmap_write_unlock(current->mm);

	if (rv > 0)
		return umem;

	siw_umem_release(umem);

	return ERR_PTR(rv);
}

/*
 * DMA mapping/address translation functions.
 * Used to populate siw private DMA mapping functions of
 * struct ib_dma_mapping_ops in struct ib_dev - see rdma/ib_verbs.h
 */

static int siw_mapping_error(struct ib_device *dev, u64 dma_addr)
{
	return dma_addr == 0;
}


static __attribute__ ((unused))
u64 siw_dma_map_single(struct ib_device *dev, void *kva, size_t size,
			       enum dma_data_direction dir)
{
	/* siw uses kernel virtual addresses for data transfer */
	//dprint(DBG_MM, "krping: siw uses kernel virtual addresses for data transfer\n");

	return (u64) kva;
}

static __attribute__ ((unused))
void siw_dma_unmap_single(struct ib_device *dev,
				 u64 addr, size_t size,
				 enum dma_data_direction dir)
{
	/* NOP */
}

static u64 siw_dma_map_page(struct ib_device *dev, struct page *page,
			    unsigned long offset, size_t size,
			    enum dma_data_direction dir)
{
	u64 kva = 0;

	BUG_ON(!valid_dma_direction(dir));

	/* XXX Allow for multiple pages to be mapped */
	if (1 || offset + size <= PAGE_SIZE) {
		kva = (u64) page_address(page);
		if (kva)
			kva += offset;
	}
	dprint(DBG_MM, "krping: page=" dprint_ptr_str() ", kva=%llx\n", page, kva);

	return kva;
}

static void siw_dma_unmap_page(struct ib_device *dev,
			       u64 addr, size_t size,
			       enum dma_data_direction dir)
{
	/* NOP */
}

static int siw_dma_map_sg(struct ib_device *dev, struct scatterlist *sgl,
			  int n_sge, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	BUG_ON(!valid_dma_direction(dir));

	for_each_sg(sgl, sg, n_sge, i) {
		/* This is just a validity check */
		if (unlikely(page_address(sg_page(sg)) == NULL)) {
			n_sge = 0;
			break;
		}
		sg->dma_address =
			(dma_addr_t) (page_address(sg_page(sg)) + sg->offset);
		sg_dma_len(sg) = sg->length;
	}
	return n_sge;
}

static void siw_dma_unmap_sg(struct ib_device *dev, struct scatterlist *sgl,
			     int n_sge, enum dma_data_direction dir)
{
	/* NOP */
}

static void siw_sync_single_for_cpu(struct ib_device *dev, u64 addr,
				    size_t size, enum dma_data_direction dir)
{
	/* NOP */
}

static void siw_sync_single_for_device(struct ib_device *dev, u64 addr,
				       size_t size,
				       enum dma_data_direction dir)
{
	/* NOP */
}

/*
 * [Gregory] do not allocate high-order page for TCP 0-copy TX 
 */
static void *siw_dma_alloc_coherent(struct ib_device *dev, size_t size,
				    u64 *dma_addr, gfp_t flag)
{
	void *kva = alloc_pages_exact(size, flag);

	if (dma_addr)
		*dma_addr = (u64)kva;

	return kva;
}

static void siw_dma_free_coherent(struct ib_device *dev, size_t size,
				  void *kva, u64 dma_addr)
{
	free_pages_exact(kva, size);
}

#if KS_IB_VERBS_HAS_DMA_MAPPING_OPS
struct ib_dma_mapping_ops siw_dma_mapping_ops = {
	.mapping_error		= siw_mapping_error,
	.map_single		= siw_dma_map_single,
	.unmap_single		= siw_dma_unmap_single,
	.map_page		= siw_dma_map_page,
	.unmap_page		= siw_dma_unmap_page,
	.map_sg			= siw_dma_map_sg,
	.unmap_sg		= siw_dma_unmap_sg,
	.sync_single_for_cpu	= siw_sync_single_for_cpu,
	.sync_single_for_device	= siw_sync_single_for_device,
	.alloc_coherent		= siw_dma_alloc_coherent,
	.free_coherent		= siw_dma_free_coherent
};
#endif 

#if KS_DMA_MAP_OPS_HAS_DMA_ATTR
static void *siw_dma_generic_alloc(struct device *dev, size_t size,
				   dma_addr_t *dma_handle, gfp_t gfp,
				   struct dma_attrs *attrs)
#else
static void *siw_dma_generic_alloc(struct device *dev, size_t size,
				   dma_addr_t *dma_handle, gfp_t gfp,
				   unsigned long attrs)
#endif
{
	return siw_dma_alloc_coherent(NULL, size, dma_handle, gfp);
}

#if KS_DMA_MAP_OPS_HAS_DMA_ATTR
static void siw_dma_generic_free(struct device *dev, size_t size,
				 void *vaddr, dma_addr_t dma_handle,
				 struct dma_attrs *attrs)
#else
static void siw_dma_generic_free(struct device *dev, size_t size,
				 void *vaddr, dma_addr_t dma_handle,
				 unsigned long attrs)
#endif
{
	siw_dma_free_coherent(NULL, size, vaddr, dma_handle);
}

#if KS_DMA_MAP_OPS_HAS_DMA_ATTR
static dma_addr_t siw_dma_generic_map_page(struct device *dev,
					   struct page *page,
					   unsigned long offset,
					   size_t size,
					   enum dma_data_direction dir,
					   struct dma_attrs *attrs)
#else
static dma_addr_t siw_dma_generic_map_page(struct device *dev,
					   struct page *page,
					   unsigned long offset,
					   size_t size,
					   enum dma_data_direction dir,
					   unsigned long attrs)
#endif
{
	return siw_dma_map_page(NULL, page, offset, size, dir);
}

#if KS_DMA_MAP_OPS_HAS_DMA_ATTR
static void siw_dma_generic_unmap_page(struct device *dev,
				       dma_addr_t handle,
				       size_t size,
				       enum dma_data_direction dir,
					   struct dma_attrs *attrs)
#else
static void siw_dma_generic_unmap_page(struct device *dev,
				       dma_addr_t handle,
				       size_t size,
				       enum dma_data_direction dir,
				       unsigned long attrs)
#endif
{
	siw_dma_unmap_page(NULL, handle, size, dir);
}

#if KS_DMA_MAP_OPS_HAS_DMA_ATTR
static int siw_dma_generic_map_sg(struct device *dev, struct scatterlist *sg,
				  int nents, enum dma_data_direction dir,
				  struct dma_attrs *attrs)
#else
static int siw_dma_generic_map_sg(struct device *dev, struct scatterlist *sg,
				  int nents, enum dma_data_direction dir,
				  unsigned long attrs)
#endif
{
	return siw_dma_map_sg(NULL, sg, nents, dir);
}

#if KS_DMA_MAP_OPS_HAS_DMA_ATTR
static void siw_dma_generic_unmap_sg(struct device *dev,
				    struct scatterlist *sg,
				    int nents,
				    enum dma_data_direction dir,
					struct dma_attrs *attrs)
#else
static void siw_dma_generic_unmap_sg(struct device *dev,
				    struct scatterlist *sg,
				    int nents,
				    enum dma_data_direction dir,
				    unsigned long attrs)
#endif
{
	siw_dma_unmap_sg(NULL, sg, nents, dir);
}

static void siw_generic_sync_single_for_cpu(struct device *dev,
					    dma_addr_t dma_handle,
					    size_t size,
					    enum dma_data_direction dir)
{
	siw_sync_single_for_cpu(NULL, dma_handle, size, dir);
}


static void siw_generic_sync_single_for_device(struct device *dev,
					       dma_addr_t dma_handle,
					       size_t size,
					       enum dma_data_direction dir)
{
	siw_sync_single_for_device(NULL, dma_handle, size, dir);
}

static void siw_generic_sync_sg_for_cpu(struct device *dev,
					struct scatterlist *sg,
					int nents,
					enum dma_data_direction dir)
{
	/* NOP */
}

static void siw_generic_sync_sg_for_device(struct device *dev,
					   struct scatterlist *sg,
					   int nents,
					   enum dma_data_direction dir)
{
	/* NOP */
}

static __attribute__ ((unused))
int siw_dma_generic_mapping_error(struct device *dev,
					 dma_addr_t dma_addr)
{
	return siw_mapping_error(NULL, dma_addr);
}

static int siw_dma_generic_supported(struct device *dev, u64 mask)
{
	return 1;
}

#if 0
static __attribute__ ((unused))
int siw_dma_generic_set_mask(struct device *dev, u64 mask)
{
	if (!dev->dma_mask || !dma_supported(dev, mask))
		return -EIO;

	*dev->dma_mask = mask;

	return 0;
}
#endif

struct dma_map_ops siw_dma_generic_ops = {
	.alloc			= siw_dma_generic_alloc,
	.free			= siw_dma_generic_free,
	.map_page		= siw_dma_generic_map_page,
	.unmap_page		= siw_dma_generic_unmap_page,
	.map_sg			= siw_dma_generic_map_sg,
	.unmap_sg		= siw_dma_generic_unmap_sg,
	.sync_single_for_cpu	= siw_generic_sync_single_for_cpu,
	.sync_single_for_device	= siw_generic_sync_single_for_device,
	.sync_sg_for_cpu	= siw_generic_sync_sg_for_cpu,
	.sync_sg_for_device	= siw_generic_sync_sg_for_device,
	.dma_supported		= siw_dma_generic_supported,
#if 0
	.mapping_error		= siw_dma_generic_mapping_error,
#endif
#if 0
	.set_dma_mask		= siw_dma_gneric_set_mask,
#endif
#if 0
	.is_phys		= 1
#endif
};
