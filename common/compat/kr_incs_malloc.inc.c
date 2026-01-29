/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs_malloc.h"
#include <stdlib.h>	// free

struct allocs_manager {
	int64_t num_active_allocs;
} __aloc_mgr = {0};
int64_t get_num_memory_allocs(void);	// For debugging
int64_t get_num_memory_allocs(void) { return __aloc_mgr.num_active_allocs; }

struct __kmem_prefix {							// Additional 8 bytes allocated before user data
	u32 alloc_size;								// User requested allocation size
	union t_malloc_flags {
		struct {
			u32 reserved                    : 24; // Kernel flags are here
			u32 do_zero                     : 1;  // 24	== Exactly the offset of __GFP_ZERO bit
			u32 is_virtual_mem              : 1;  // 25
			u32 is_alloced_with_page_struct : 1;  // 26
			u32 was_already_freed           : 1;  // 27
			u32 page_order                  : 4;  // 28 - 31 - 4 bits in flags are reserved for page alloc order (16, more  then enouth, kernel limit is 5 anyway). For regular malloc  * the order is 0. For get_free_page it is 1. For  get_free_pages(x) it is x
		};
		u32 all;
	} flags;
};
static struct __kmem_prefix *__get_alloc_prefix(const void* user_addr) {
	BUILD_BUG_ON(sizeof(struct __kmem_prefix) != 8UL);
	BUILD_BUG_ON(sizeof(struct page) != 16UL);
	return (struct __kmem_prefix*)(user_addr - sizeof(struct __kmem_prefix));
}

static size_t __kmem_calc_alignment(size_t size) {
	return ((size & PAGE_MASK) == size) ? PAGE_SIZE : 64; // Align to 64 bytes, required by GF asm functions for AVX2, AVX1 needed 32.
}

static size_t __kmem_prefix_size(size_t size) {
	return round_up(sizeof(struct __kmem_prefix), __kmem_calc_alignment(size));
}

void *kmalloc(size_t size, gfp_t _gfp_mask) {
	void* raw_alloc = NULL;
	const size_t alignment = __kmem_calc_alignment(size);
	const size_t prefix_size = __kmem_prefix_size(size);
	const union t_malloc_flags f = {.all = _gfp_mask};
	int rv = posix_memalign(&raw_alloc, alignment, (prefix_size + size));
	BUG_ON((size==0) || !raw_alloc || rv != 0);
	{
		void* user_addr = raw_alloc + prefix_size;
		struct __kmem_prefix* prefix = __get_alloc_prefix(user_addr);
		if (f.do_zero)
			memset(user_addr, 0, size);
		prefix->alloc_size = size;
		prefix->flags.all = f.all;
		__aloc_mgr.num_active_allocs++;

		if (prefix->flags.is_alloced_with_page_struct) {
			int i, n_pages = (1 << (prefix->flags.page_order));
			struct page *pages = (void*)prefix - (sizeof(struct page)*n_pages);
			BUG_ON((void*)pages < raw_alloc);					// Inline array of pages before the prefix
			for (i = 0; i < n_pages; i++) {
				pages[i].mapped_vaddr = user_addr + (i << PAGE_SHIFT);
				pages[i].all_flags = 0UL;					// Zero all flags
			}
		}
		return user_addr;
	}
}

struct page *virt_to_page(const void* user_addr) {
	const struct __kmem_prefix* prefix = __get_alloc_prefix(user_addr);
	const int n_pages = (1 << (prefix->flags.page_order));
	struct page *pages = (void*)prefix - (sizeof(struct page)*n_pages);
	return (prefix->flags.is_alloced_with_page_struct) ? pages : NULL; // Page not mapped. Allocated with kmalloc()
}

static void _kvfree(void *user_addr, gfp_t _f_flags) {
	if (user_addr != NULL) {
		const union t_malloc_flags f_flags = {.all = _f_flags};
		struct __kmem_prefix* prefix = __get_alloc_prefix(user_addr);
		BUG_ON(prefix->flags.was_already_freed);
		BUG_ON(prefix->flags.page_order     != f_flags.page_order);
		BUG_ON(prefix->flags.is_virtual_mem != f_flags.is_virtual_mem);	// kmalloc+vfree or vmalloc+kfree
		prefix->flags.was_already_freed = true;
		__aloc_mgr.num_active_allocs--;
		free(user_addr - __kmem_prefix_size(prefix->alloc_size));
	} else {/* It is legal to call kernel free on NULL. don't warn on that*/}
}

inline unsigned long __get_free_pages(gfp_t _gfp_mask, unsigned int order) {
	union t_malloc_flags f = {.all = _gfp_mask};
	f.page_order = order;
	f.is_alloced_with_page_struct = 1;
	if (order >= 8)
		return 0ULL; // 256 pages at once is the allowed maximum. In real kernel limit is even smaller
	return (u64)kmalloc(PAGE_SIZE*(1<<order), f.all);
}

void free_pages(unsigned long addr, unsigned int order) {
	union t_malloc_flags f = {.all = 0};
	f.page_order = order;
	_kvfree((void*)addr, f.all);
}

struct page *alloc_pages(gfp_t flags, unsigned int order) {
	u64 user_addr = __get_free_pages(flags, order);
	return (user_addr) ? virt_to_page((void*)user_addr) : NULL;
}

void nvmeib_split_page(struct page *page, unsigned int order) {
	u32 i, n_pages = (1 << order);
	u8 *mem;
	for (i = 0, mem = page->mapped_vaddr; i < n_pages; i++, mem += PAGE_SIZE) {
		page[i].split._head = i+10000;
		page[i].mapped_vaddr = mem;
	}
	page->split.is_split = 1;
	page->split.n_refs = n_pages;
	page->split.order = order;
}

void put_page(struct page *page) {
	BUG_ON(page->split.is_pinned != true);
	page->split.is_pinned = false;
}

void __free_pages(struct page *page, unsigned int order) {
	free_pages((unsigned long)page->mapped_vaddr, order);
}

void __free_page(struct page *page) {
	if (page->split._head) {
		struct page *head = &page[-page->split._head+10000];
		BUG_ON((head->split.n_refs <= 0)||(!head->split.is_split));
		if (--head->split.n_refs == 0)
			__free_pages(head, head->split.order);
	} else {
		__free_pages(page, 0);
	}
}

void *kzalloc(size_t size, gfp_t flags) {
	return kmalloc(size, flags | __GFP_ZERO);
}

void *kcalloc(size_t n, size_t size, gfp_t flags) {
	return kmalloc((n*size), flags | __GFP_ZERO);
}

void *vzalloc(size_t size) {
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = f.do_zero = 1;
	return kmalloc(size, f.all);
}
void *vmalloc(size_t size) {
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = 1;
	return kmalloc(size, f.all);
}

void kfree(const void *addr) { _kvfree((void*)addr, 0); }
void vfree(const void *addr) {
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = 1;
	_kvfree((void*)addr, f.all);
}

size_t ksize(const void *addr){
	const struct __kmem_prefix* prefix = addr - sizeof(struct __kmem_prefix);
	BUG_ON(!addr);
	return prefix->alloc_size;
}

int is_vmalloc_addr(const void *user_addr) {
	return user_addr ?	__get_alloc_prefix(user_addr)->flags.is_virtual_mem : false;
}

