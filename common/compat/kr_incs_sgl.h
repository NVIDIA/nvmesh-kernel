/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_SGL_H
#define KR_INCS_SGL_H
#ifndef __KERNEL__
	// Kernel already has those functions. Define as compatibility for user-space
	#include "kr_incs_malloc.h"
	// /asm/io.h
	static inline ulong virt_to_phys(void* address){ return (ulong)((u64)address + 0x0F00000000000000LL); }
	static inline void* phys_to_virt(ulong address){ return (void*)((u64)address - 0x0F00000000000000LL); }

	// /linux/scatterlist.h, /linux/asm-generic/scatterlist.h, Scatter-gather list implementation without virtual pages. API described at https://lwn.net/Articles/256368/
	struct scatterlist {
		unsigned long	page_link;
		unsigned int	offset, length;	 // For reads or writes, `length` is the total bytes in SG list. For TRIM, it's the total bytes of the NVMe DSM command size.
		#if defined(BLKDEV_SIMULATOR)
			dma_addr_t		dma_address;		// Needed only for emulation of kernel to kernel drives
		#endif
	};
	#define sg_dma_address(sg)	((sg)->dma_address)
	#define sg_dma_len(sg)		((sg)->length)

	static inline void sg_assign_page(struct scatterlist *sg, struct page *page){ sg->page_link = (unsigned long)page; }
	static inline void sg_set_page(   struct scatterlist *sg, struct page *page, unsigned int len, unsigned int offset) {
		sg_assign_page(sg, page);
		sg->offset = offset;
		sg->length = len;
	}
	void sg_set_buf(    struct scatterlist *sg, const void  *buf,  unsigned int len);
	void sg_init_table( struct scatterlist *sg, 				   unsigned int len);
	static inline void sg_init_one(  struct scatterlist *sg, const void *buf, unsigned int buflen) { sg_set_buf(sg, buf, buflen); }
	static inline void sg_mark_end(  struct scatterlist *sg) { (void)sg; /* NOOP in simulator */ }
	static inline void sg_unmark_end(struct scatterlist *sg) { (void)sg; /* NOOP in simulator */ }
	static inline struct page *sg_page(struct scatterlist *sg) { return (struct page *)((sg)->page_link & ~0x3); }
	static inline void *sg_virt(struct scatterlist *sg) {
		struct page *page = sg_page(sg);
		return ((sg->offset&0x1) ? (void*)page /* Buf, not page */: (void*)((page->mapped_vaddr)+(sg->offset&(~0x1))));
	}
	static inline dma_addr_t sg_phys(	struct scatterlist *sg) { return (dma_addr_t)virt_to_phys(sg_virt(sg)); }

	#define SG_CHAIN	0x01UL
	#define SG_END		0x02UL
	#define sg_is_chain(sg)		((sg)->page_link & SG_CHAIN)
	#define sg_is_last(sg)		((sg)->page_link & SG_END)
	#define sg_chain_ptr(sg)	((struct scatterlist *) ((sg)->page_link & ~(SG_CHAIN | SG_END)))

	size_t sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen);
	size_t sg_copy_to_buffer(  struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen);
	size_t sg_copy_buffer(     struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen, long int skip, bool to_buffer);

	struct sg_table {
		struct scatterlist *sgl;												// Aray of sg lists
		unsigned int nents, orig_nents;
	};
	int  sg_alloc_table(struct sg_table *t, unsigned int n, gfp_t flags);		// Allocate array of scatterlists
	void sg_free_table( struct sg_table *t);
	struct scatterlist *sg_next(struct scatterlist *sg);						// Get next element in the 'sgl' list
	struct scatterlist *sg_last(struct scatterlist *sg, unsigned int nents);    // Get the last/nent element in the 'sgl' list
	#define for_each_sg(sglist, sg, nr, __i) for (__i = 0, sg = (sglist); __i < (nr); __i++, sg = sg_next(sg))		// Iterator over all the elements of 'sgl' array

#endif // __KERNEL__
#endif // KR_INCS_SGL_H
