/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_MALLOC_H
#define KR_INCS_MALLOC_H
#ifndef __KERNEL__
	// Kernel already has those functions. Define as compatibility for user-space
	/*********** Page emulation: gfp.h, page_types.h, pgtable.h page.h ************/
	#include "kr_incs_types.h"
	#if !defined(PAGE_SIZE)
		#define PAGE_SIZE (1UL << PAGE_SHIFT)
	#endif
	#define PAGE_MASK (~(PAGE_SIZE-1))

	struct page {
		u8 *mapped_vaddr;					// Pointer to 4KB block of memory to which we can read/write (array in RAM). Mapped virtual address which kernel code can access directly
		union {
			struct {
				u32 _head     : 16;			// Split pages all point to the first one
				u32 n_refs    : 16;			// ... and take a reference on it. Warning: not atomic, so no support for concurrent freeing of split pages!
				u32 order     : 16;			// For freeing later, only on the head page.
				u32 is_pinned :  1;			// Is pinned to prevent swap out
				u32 is_split  :  1;
			} split;
			u64 all_flags;
		};
	};
	#define virt_to_head_page(x) virt_to_page(x)				// This is incorrect, however good for now (virt / phys)
	#define page_address(page)      ((void*)(page)->mapped_vaddr)

	#define virt_to_head_page(x) virt_to_page(x)
	static inline bool PageSlab(struct page *page) { return (page == NULL); }		// page == NULL means: Allocation not via page_alloc, but via kmalloc()
	static inline bool PageWriteback(struct page *page) { (void)page; return false; }
	static inline bool PageDirty(struct page *page) { (void)page; return false; }
	#define nth_page(page, n)      (page + n)
	struct page *virt_to_page(const void* vaddr);
	#define page_to_phys(page)      virt_to_phys(page_address(page))
	#define page_address(page)      ((void*)(page)->mapped_vaddr)
	#define	virt_addr_valid(kaddr)	(kaddr != NULL)			// Daniel: not implemented yet
	struct page *ZERO_PAGE(u64 vaddr);

	// Allocation supported flags
	enum { GFP_NOWAIT = 0, GFP_KERNEL = 0, __GFP_HIGHMEM = 0, GFP_ATOMIC = 0, GFP_NOFS = 0, GFP_NOIO = 0, __GFP_NOWARN = 0, __GFP_ZERO = 0x01000000};
	#define KMALLOC_MAX_SIZE	(0x7FFFFFFF) 		// Big enough number

	// Memory allocations emulation: slub_def.h, gfp.h, page_types.h, pgtable.h
	int is_vmalloc_addr(const void *);
	void *kmalloc(size_t size, gfp_t flags);
	#define kzalloc_node(s, f, n) kzalloc(s, f)
	void *kzalloc(size_t size, gfp_t flags);
	void *vzalloc(size_t size);
	void *vmalloc(size_t size);
	void *krealloc(const void *, size_t, gfp_t);
	void *kcalloc(size_t n, size_t size, gfp_t flags);
	void vfree(  const void *addr);
	void kfree(  const void *addr);
	size_t ksize(const void *addr);

	#define __get_free_page(flags) __get_free_pages(flags, 0)
	#define free_page(addr)        free_pages(addr, 0)
	unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
	void free_pages(unsigned long addr, unsigned int order);
	struct page *alloc_pages(gfp_t gfp_mask, unsigned int order);
	#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)
	void nvmeib_split_page(struct page *page, unsigned int order);
	void __free_pages(struct page *page, unsigned int order);
	void __free_page(struct page *page);
	void put_page(struct page *page);

	static inline void *alloc_pages_exact(size_t size, gfp_t gfp_mask) { return kmalloc(size, gfp_mask); }
	static inline void free_pages_exact(void *virt, size_t size) { (void)size; kfree(virt); }
#endif // __KERNEL__
#endif // KR_INCS_MALLOC_H
