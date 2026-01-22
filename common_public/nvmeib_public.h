/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_PUBLIC_H
#define NVMEIB_PUBLIC_H

#include "kr_incs.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib.h"
#include "nvmeib_numa.h"
#include "poll/nvmeib_public_intr_poll.h"
#include "kth/nvmeib_public_kth.h"

int nvmeib_public_debug_level(void);
void nvmeib_public_set_debug_level(int (*dlf)(void));
extern int *nvmeib_public_panic_on_warn;

struct ib_pd;
struct ib_mr;

/* example */
#if 0
	{
		struct nvmeib_alloc_n_map mem = {0};

		mem.pd = nis_dev->dev->pd;
		mem.access_flags =
			IB_ACCESS_LOCAL_WRITE |
			IB_ACCESS_REMOTE_READ |
			IB_ACCESS_REMOTE_WRITE|
			IB_ACCESS_REMOTE_ATOMIC;
		mem.ioaddr = 0;
		mem.n_pages = 1000;
		/* mem.pages is not set so we allocate */
		_ND(__AUTOID__, "------> Just before\n");
		if (nvmeib_mem_alloc_n_map(&mem) == 0) {
			_ND(t_02_bpublic, "~~~~~ We made it ~~~~~");
			_ND(t_03_bpublic, "mr->key=@X", mem.mr->rkey);
			nvmeib_mem_unmapn_n_free(&mem);
		}
		else
			_NE(t_01_bpublic, "!!!!! We failed !!!!!");
		_ND(__AUTOID__, "------> Just after\n");
	}
#endif

void nvmeib_mem_vunmap_n_free(struct nvmeib_alloc_n_map *mem,
	void *vaddr);

void * nvmeib_vmap(void **virts, int n);

#define __nvmeib_public_alloc_percpu(size, align) __alloc_percpu(size, align)
#define nvmeib_public_alloc_percpu(type)                                \
		(typeof(type) __percpu *)__alloc_percpu(                        \
											sizeof(type),               \
											__alignof__(type))
#define nvmeib_public_alloc_percpu_cacheline(type)                      \
		(typeof(type) __percpu *)__alloc_percpu(                        \
											sizeof(type),               \
											cache_line_size())
#define nvmeib_public_free_percpu(ptr) free_percpu(ptr)
#define nvmeib_public_zero_percpu(_obj) \
        do { \
            int cpu; \
            for_each_possible_cpu(cpu) { \
                memset(per_cpu_ptr(_obj, cpu), 0, sizeof(*_obj)); \
            } \
        } while(0); \

#define __nvmeib_public_alloc_percpu_zeroed(size, align) __alloc_percpu_gfp(size, align, GFP_KERNEL | __GFP_ZERO)
#define nvmeib_public_alloc_percpu_zeroed(type)                        \
		(typeof(type) __percpu *)__alloc_percpu_gfp(                    \
											sizeof(type),               \
											__alignof__(type),          \
											GFP_KERNEL | __GFP_ZERO)

#define nvmeib_public_alloc_percpu_zeroed_cacheline(type)                      \
		(typeof(type) __percpu *)__alloc_percpu_gfp(                          \
											sizeof(type),               \
											cache_line_size(),          \
											GFP_KERNEL | __GFP_ZERO)
            
void nvmeib_public_uuid_gen(uuid_be *bu);

/**
 * @brief Determines if kernel has a serial console enabled
 */
bool nvmeib_public_serial_console(void);

struct ib_device;
struct ib_device_attr;
struct ib_qp;
struct ib_cq;
struct nvmeib_send_wr;

struct nvmeib_device_public_ops {
	struct module *module;
	/* Workaround max MR size (2MB=512*4K-PAGE) in kernel ib-verbs */
	int (*alloc_n_map)(struct nvmeib_alloc_n_map *mem);
	int (*unmapn_n_free)(struct nvmeib_alloc_n_map *mem);
	int (*map_mr)(struct ib_device *ibdev, struct ib_mr *mr,
				  phys_addr_t *pages, int n_pages);

	int (*query_device)(struct ib_device *ibdev,
			    struct ib_device_attr *props);

	/* Not supported for mlx5 in kernel ib-verbs */
	int (*post_send_atomic)(struct ib_qp *ibqp,
				struct nvmeib_send_wr *wr,
				struct nvmeib_send_wr **bad_wr);
	void (*set_debug_level)(int (*dlf)(void));
	int (*peek_cq)(struct ib_cq *ibcq, int max);
	struct ib_qp *(*create_rdda_qp)(struct ib_pd *ib_pd,
				struct ib_qp_init_attr *qp_init_attr);
	int (*destroy_rdda_qp)(struct ib_qp *ibqp);
};

struct nvmeib_public_hwdev {
	struct list_head list;
	enum nvmeib_dev_type type;
#if KS_HAS_MODULE_MUTEX
	struct module *m;
#else
	/* find_module is no longer available, must match by name */
	char m[MODULE_NAME_LEN];
#endif
	struct nvmeib_device_public_ops *ops;
};

struct list_head *nvmeib_public_get_hwdevs(void);
int nvmeib_public_set_pops(enum nvmeib_dev_type type,
			   const char *hwdriver,
			   struct nvmeib_device_public_ops *pops,
			   struct nvmeib_device_public_ops *pops_odp);
int nvmeib_public_clear_pops(enum nvmeib_dev_type type);

int nvmeib_public_generic_post_send_atomic(struct ib_qp *ibqp,
	struct nvmeib_send_wr *wr,
	struct nvmeib_send_wr **bad_wr);

bool nvmeib_mlx_on_demand_paging(void);

#if KS_HAS_DISK_PART_ITER
int nvmeib_public_call_for_each_disk_part(struct gendisk *disk, int (*f)(struct hd_struct *, void *), void *args);
#else
int nvmeib_public_call_for_each_disk_part(struct gendisk *disk, int (*f)(struct block_device *, void *), void *args);
#endif


struct mm_struct * nvmeib_public_get_process_mm(int pid);
void nvmeib_public_put_process_mm(struct mm_struct *mm);
int nvmeib_public_copy_user_pages(
	void *buf, int pid, void *src, int len, int copy_to);

int  nvmeib_public_user_pages_for_io_pin(pid_t pid, ulong userspace_vaddr,			  int n_pages, struct page **pages, int is_write);
void nvmeib_public_user_pages_for_io_unpin(                                           int n_pages, struct page **pages, int copy_to);

void nvmeib_public_save_stack_trace(struct nvmeib_stack_trace *trace);

void nvmeib_public_kgdb_breakpoint(void);

unsigned long nvmeib_kallsyms_lookup_name(const char *name);

bool nvmeib_sym_resolve_kernel_bug_can_happen(void *addr);

/**
 * nvmeib reference counted objects
 *
 * Similar to kernel's kref with the following exceptions:
 * 1. put API does not release the obj.
 * 2. Adding wait-release API which waits for all object's user
 *    to put the object. Object's owner must call it before
 *    releasing the object.
 * 3. Optimization to speed up object release. Prevent new users
 *    (due to existing users) after owner started wait-release.
 */

#define NVMEIB_REF_WAIT_RELEASE (5 * HZ)
struct nvmeib_ref {
	atomic_t cnt;
	struct completion *comp;
	atomic_t dying;
};

#define NVMEIB_REF_INIT() (struct nvmeib_ref){ .cnt = ATOMIC_INIT(1), .comp = NULL, .dying = ATOMIC_INIT(0) }
#define NVMEIB_REF_INIT_DEAD() (struct nvmeib_ref){ .cnt = ATOMIC_INIT(0), .comp = NULL, .dying = ATOMIC_INIT(1) }

void nvmeib_ref_init(struct nvmeib_ref *r);

/**
 * nvmeib_ref_get - increment @r->cnt unless its zero.
 *
 * Returns non-zero if @r->cnt was non-zero, and zero
 * otherwise.
 */
int __must_check nvmeib_ref_get(struct nvmeib_ref *r);

/**
 * nvmeib_ref_put - decrement @r->cnt. If new value is 0,
 * complete @r's completion item.
 *
 * Returns the new value of @r->cnt.
 */
int nvmeib_ref_put(struct nvmeib_ref *r);

/**
 * nvmeib_ref_release_start - inc @r->dying
 *
 * Returns 0 if this is the first call of this function for @r,
 * in which case caller should (may) call release-wait API.
 * Otherwise, return -1.
 *
 * This API lets the caller to first block new users of
 * @r, trigger the release of exiting ones and then call
 * release-wait API.
 *
 * This API allows having multiple contexts triggering the
 * release of but only one waiting (blocking on) till @r is
 * released.
 */
int nvmeib_ref_release_start(struct nvmeib_ref *r);

/**
 * nvmeib_ref_release_wait - wait on @r->comp, for @r->cnt to be
 * 0.
 *
 */
void nvmeib_ref_release_wait_n(struct nvmeib_ref *r, unsigned num_attempts);

#define nvmeib_ref_release_wait(r) nvmeib_ref_release_wait_n(r, ~(unsigned)0)

/**
 * nvmeib_ref_read - return @r->cnt.
 *
 */
static inline int nvmeib_ref_read(struct nvmeib_ref *r)
{
	return atomic_read(&r->cnt);
}

/**
 * nvmeib_ref_read - return @r->cnt.
 *
 */
static inline bool nvmeib_ref_is_dying(struct nvmeib_ref *r)
{
	return !!atomic_read(&r->dying);
}

#if 0
/**
 * nvmeib_ref_release - initiate release, if first to do so,
 * wait for @r->cnt to be 0.
 *
 * This API shall be used only if there is only one context that
 * may trigger the release.
 */
static inline void nvmeib_ref_release_wait_if_first(struct nvmeib_ref *r)
{
	if (nvmeib_ref_release_start(r) == 0)
		nvmeib_ref_release_wait(r);
}
#endif

void nvmeib_public_kth_fill_ft(struct nvmeib_public_kth_ft *ft);
void nvmeib_public_intr_poller_fill_ft(struct nvmeib_intr_pollers_ft *ift);

static inline bool is_siw_ib_dev(struct ib_device *ib_dev)
{
	static const char *siw_prefix = "siw_";

	return strncmp(ib_dev->name, siw_prefix, strlen(siw_prefix)) == 0;
}

/* New kernels claim to be safe for NULL dma_device but this functions are not */
static inline void *nvmeib_public_ib_dma_alloc_coherent(struct ib_device *dev,
					   size_t size,
					   dma_addr_t *dma_handle,
					   gfp_t flag)
{
#if KS_HAS_VIRT_DMA_SUPPORT
	void *kva = NULL;
	if (ib_uses_virt_dma(dev)) {
		kva = alloc_pages_exact(size, flag);

		if (dma_handle)
			*dma_handle = (u64)kva;

		return kva;
	}
#endif
	if (is_siw_ib_dev(dev)) {
		void *kva = alloc_pages_exact(size, flag);
		if (kva && dma_handle)
			*dma_handle = (dma_addr_t)(unsigned long)kva;
		return kva;
	}
	return ib_dma_alloc_coherent(dev, size, dma_handle, flag);
}

static inline void nvmeib_public_ib_dma_free_coherent(struct ib_device *dev,
                    size_t size, void *cpu_addr,
                    dma_addr_t dma_handle)
{
#if KS_HAS_VIRT_DMA_SUPPORT
	if (ib_uses_virt_dma(dev)) {
		free_pages_exact(cpu_addr, size);
		return;
	}
#endif
	if (is_siw_ib_dev(dev)) {
		free_pages_exact(cpu_addr, size);
		return;
	}
	ib_dma_free_coherent(dev, size, cpu_addr, dma_handle);
}

static inline void nvmeib_public_ib_dma_sync_sg_for_device(struct ib_device *dev, struct scatterlist *sg,
							int nelems, enum dma_data_direction dir)
{
#if KS_HAS_VIRT_DMA_SUPPORT
	if (ib_uses_virt_dma(dev)) {
		return;
	}
#endif
	if (is_siw_ib_dev(dev)) {
		return;
	}
	dma_sync_sg_for_device(dev->dma_device, sg, nelems, dir);
}

static inline void nvmeib_public_ib_dma_sync_sg_for_cpu(struct ib_device *dev, struct scatterlist *sg,
							   int nelems, enum dma_data_direction dir)
{
#if KS_HAS_VIRT_DMA_SUPPORT
	if (ib_uses_virt_dma(dev)) {
		return;
	}
#endif
	if (is_siw_ib_dev(dev)) {
		return;
	}
	dma_sync_sg_for_cpu(dev->dma_device, sg, nelems, dir);
}

static inline void nvmeib_public_ib_dma_sync_single_for_cpu(struct ib_device *dev,
							      u64 addr,
							      size_t size,
							      enum dma_data_direction dir)
{
#if KS_HAS_VIRT_DMA_SUPPORT
	if (ib_uses_virt_dma(dev)) {
		return;
	}
#endif
	if (is_siw_ib_dev(dev)) {
		return;
	}
	dma_sync_single_for_cpu(dev->dma_device, addr, size, dir);
}

static inline void nvmeib_public_ib_dma_sync_single_for_device(struct ib_device *dev,
								 u64 addr,
								 size_t size,
								 enum dma_data_direction dir)
{
#if KS_HAS_VIRT_DMA_SUPPORT
	if (ib_uses_virt_dma(dev)) {
		return;
	}
#endif
	if (is_siw_ib_dev(dev)) {
		return;
	}
	dma_sync_single_for_device(dev->dma_device, addr, size, dir);
}

static inline int nvmeib_public_ib_dma_map_sg(struct ib_device *dev,
				struct scatterlist *sg, int nents,
				enum dma_data_direction direction)
{
#if !KS_HAS_VIRT_DMA_SUPPORT
	if (is_siw_ib_dev(dev)) {
		/* Replicate the functionality of ib_dma_virt_map_sg:
		 * - Copy the virtual address and length of each entry to the dma address and length
		 * */
		struct scatterlist *s;
		int i;
		for_each_sg(sg, s, nents, i) {
			sg_dma_address(s) = (uintptr_t)sg_virt(s);
			sg_dma_len(s) = s->length;
		}
		return nents;
	}
#endif
	return ib_dma_map_sg(dev, sg, nents, direction);
}

static inline void nvmeib_public_ib_dma_unmap_sg(struct ib_device *dev,
				   struct scatterlist *sg, int nents,
				   enum dma_data_direction direction)
{
#if !KS_HAS_VIRT_DMA_SUPPORT
	if (is_siw_ib_dev(dev)) {
		/* Nothing to do here */
		return;
	}
#endif
	return ib_dma_unmap_sg(dev, sg, nents, direction);
}

static inline u64 nvmeib_public_ib_dma_map_single(struct ib_device *dev, void *kva, size_t size,
                                           enum dma_data_direction dir)
{
#if KS_HAS_VIRT_DMA_SUPPORT
       if (ib_uses_virt_dma(dev)) {
               return (dma_addr_t)kva;
       }
#endif
       if (is_siw_ib_dev(dev)) {
               return (dma_addr_t)kva;
       }
       return ib_dma_map_single(dev, kva, size, dir);
}

static inline void nvmeib_public_ib_dma_unmap_single(struct ib_device *dev,
                         u64 addr, size_t size,
                         enum dma_data_direction dir)
{
#if KS_HAS_VIRT_DMA_SUPPORT
       if (ib_uses_virt_dma(dev)) {
               return;
       }
#endif
       if (is_siw_ib_dev(dev)) {
               return;
       }
       ib_dma_unmap_single(dev, addr, size, dir);
}

static inline bool nvmeib_public_ib_dma_mapping_error(struct ib_device *dev, u64 addr)
{
#if KS_HAS_VIRT_DMA_SUPPORT
       if (ib_uses_virt_dma(dev)) {
               return false;
       }
#endif
       if (is_siw_ib_dev(dev)) {
               return false;
       }
       return ib_dma_mapping_error(dev, addr);
}

#if KS_HAS_VIRT_DMA_SUPPORT
#define IBDEV2DMADEV(_ib_dev) (ib_uses_virt_dma(_ib_dev)? (&_ib_dev->dev) : _ib_dev->dma_device)
#else
#define IBDEV2DMADEV(_ib_dev) (_ib_dev)->dma_device
#endif

#define IBDEV2PCIDEV(_ib_dev) to_pci_dev(_ib_dev->dma_device)
static inline struct pci_dev * NVMEIBDEV2PCIDEV(struct nvmeib_dev *dev) {
	if(dev->dev_type == DT_siw) {
		WARN_ON(true);
		return NULL;
	}
	return IBDEV2PCIDEV(dev->ib_dev);
}

#if	defined(NVMEIB_STATE_GUARD_STACK_TRACE) && !defined(NVMEIB_COMMON)
void nvmeib_public_save_stack_trace(struct nvmeib_stack_trace *trace);
#endif

#include "../common/compat/kr_incs_time_rdtsc.h"

struct task_struct *nvmeib_kthread_create_on_cpu(
	int (*threadfn)(void *data), void *data, unsigned int cpu, const char *namefmt);

/* Used for finding memory corruption with KASAN */
enum nvmeib_public_kasan_poison_type {
	NVMEIB_PUBLIC_KASAN_POISON_TYPE_KMALLOC = 0,
	NVMEIB_PUBLIC_KASAN_POISON_TYPE_PAGES,
	NVMEIB_PUBLIC_KASAN_POISON_TYPE_VMALLOC
};

int nvmeib_public_kasan_poison(const void *addr, size_t size, enum nvmeib_public_kasan_poison_type poison_type);
int nvmeib_public_kasan_unpoison(const void *addr, size_t size);
int nvmeib_public_kasan_test(void);


#define __NVMEIB_PUBLIC_SYMBOL_STR(x) #x
#define NVMEIB_PUBLIC_SYMBOL_STR(x) __NVMEIB_PUBLIC_SYMBOL_STR(x)

/* Convenience macros for symbol_get/put with type casting */
#define nvmeib_public_symbol_get(x) ((typeof(&x))(__symbol_get(NVMEIB_PUBLIC_SYMBOL_STR(x))))
#define nvmeib_public_symbol_put(x) __symbol_put(NVMEIB_PUBLIC_SYMBOL_STR(x))

#endif
