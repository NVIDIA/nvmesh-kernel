/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_buffers.h"
#include "nvmeibc_memmgr_metrics.h"

NVMEIBC_MEMMGR_METRIC(dp_data_page_buffers, "component=raid.io.data");

/************************** Pages allocations *********************************/
int nvmeibc_pages_max_alloc = (sizeof(unsigned int) * 8 - 1);
module_param_named(pages_max_alloc, nvmeibc_pages_max_alloc, int, 0644);
MODULE_PARM_DESC(pages_max_alloc, "Maximum order of page allocations allowed.");

static u8 __get_max_contained_order(u32 i)
{
	return (fls64(i) - 1);
}

static inline unsigned int __estimate_num_expected_allocs(unsigned int npages, u8 start_order)
{
	unsigned int nallocs = 0;
	while (npages) {
		u8 order = min(start_order, __get_max_contained_order(npages));
		unsigned int n_pages_in_alloc = 1UL << order;
		unsigned int n_allocs_current = npages / n_pages_in_alloc;
		nallocs += n_allocs_current;
		npages -= n_allocs_current * n_pages_in_alloc;
	}
	return nallocs;
}

static inline int __nvmeibc_pages_resize(struct nvmeibc_pages *nps, unsigned int new_max_nallocs, gfp_t flags)
{
	u8 *new_orders;
	struct page **new_pages = kmalloc(new_max_nallocs * (sizeof(*new_pages) + sizeof(*new_orders)), flags);

	if (unlikely(!new_pages))
		return -ENOMEM;

	new_orders = (void*)&new_pages[new_max_nallocs];
	if (nps->nallocs) {					// Doing a realloc
		memcpy(new_pages,  nps->pages,  nps->nallocs * sizeof(*new_pages));
		memcpy(new_orders, nps->orders, nps->nallocs * sizeof(*new_orders));
		kfree(nps->pages);
	}
	nps->pages = new_pages;
	nps->orders = new_orders;
	return 0;
}

int nvmeibc_pages_alloc(struct nvmeibc_pages *nps, unsigned int n_blocks, const int tcp_mode)
{
	unsigned int npages = NVMEIBC_BLOCKS_TO_PAGES_ALLOC_COUNT(n_blocks);
	const gfp_t flags = (GFP_NOFS | __GFP_NOWARN);
	unsigned int max_nallocs = 0; // size of the currently allocated array
	const u8 min_order = NVMEBC_PAGES_ORDER_BLOCK;
	u8 order = nvmeibc_pages_max_alloc;
	// Check errors: not already alocated, cannot reuse pages if zeroing required, npages should divide by 2^min_order
	WARN(nps->nallocs || (0 == npages) || (flags & __GFP_ZERO) || (npages & ((1UL << min_order) - 1)), "invalid arguments! allocs=%u, npages=%u, flags=%u, min_order=%u\n", nps->nallocs, npages, flags, min_order);
	while (npages &&
			(order = min(order, __get_max_contained_order(npages))) >= min_order) {
		struct page *new_pages = alloc_pages(flags, order);
		nvmesh_memmgr_metric_on_alloc_update(dp_data_page_buffers, (1ull << order) * PAGE_SIZE, new_pages);
		if (!new_pages) {							// Attempt to alocate lower order
			if (0 == order)
				break;
			order--;
			continue;
		}

		if (nps->nallocs == max_nallocs) {			// Resize the descriptor arrays
			max_nallocs += __estimate_num_expected_allocs(npages, order);
			if (__nvmeibc_pages_resize(nps, max_nallocs, flags) < 0) {
				__free_pages(new_pages, order);
				nvmesh_memmgr_metric_on_free_update(dp_data_page_buffers, (1ull << order) * PAGE_SIZE);
				break;
			}
		}

		if (tcp_mode)
			nvmeib_split_page(new_pages, order);

		nps->pages[nps->nallocs] = new_pages;
		nps->orders[nps->nallocs] = order;
		nps->nallocs++;
		BUG_ON(npages < (1U << order));
		npages -= (1U << order);
	}

	nps->n_blks_total = n_blocks;
	if (npages != 0) {
		nvmeibc_pages_free(nps, tcp_mode);
		return -ENOMEM;
	}
	return 0;
}

void nvmeibc_pages_free(struct nvmeibc_pages *nps, const int tcp_mode)
{
	u32 i, j;
	if (nps->nallocs)
		BUG_ON(nps->n_blks_total == 0);				// Sanity Trap
	for (i = 0; i < nps->nallocs; i++) {
		struct page *pages = nps->pages[i];
		const u32 n_pages = (1 << nps->orders[i]);
		if (!tcp_mode)
			__free_pages(nps->pages[i], nps->orders[i]);
		else {
			for (j = 0; j < n_pages; j++)
				__free_page(&pages[j]);						// Pages were split for TCP, so need to free them one by one
		}
		nvmesh_memmgr_metric_on_free_update(dp_data_page_buffers, n_pages*PAGE_SIZE);
	}

	kfree(nps->pages);
	*nps = nvmeibc_pages_set_empty();
}

void nvmeibc_pages_drop_ownership(struct nvmeibc_pages *nps)
{
	u32 i;

	if (nps->nallocs)
		BUG_ON(nps->n_blks_total == 0);                         // Sanity Trap

	for (i = 0; i < nps->nallocs; i++) {
		const u32 n_pages = (1 << nps->orders[i]);
		nvmesh_memmgr_metric_on_free_update(dp_data_page_buffers, n_pages*PAGE_SIZE);
	}

	kfree(nps->pages);
	*nps = nvmeibc_pages_set_empty();
}

bool nps_block_iter_advance(struct nps_block_iter *nbi, unsigned int nblocks)
{
	while (nblocks) {
		const unsigned int n_blocks_in_entry = NVMEBC_PAGES_NBLOCKS_IN_PAGE_ORDER(*nbi->order);
		const unsigned int n_blocks_delta = min(nblocks, n_blocks_in_entry - nbi->block_offset);
		BUG_ON(0 == nbi->entries_left);
		nbi->block_offset += n_blocks_delta;
		nbi->n_blocks_used += n_blocks_delta;
		nblocks -= n_blocks_delta;

		if (nbi->block_offset == n_blocks_in_entry) {
			nbi->block_offset = 0;
			nbi->page++;
			nbi->order++;
			nbi->entries_left--;
		}
	}

	return (nbi->entries_left != 0);
}

/************************** Buffers *******************************************/
void nvmeibc_fill_ndb(struct nvmeib_data_buffer *ndb, unsigned int nlbas, struct nps_block_iter *nbi)
{
	struct scatterlist *sg = ndb->table.sgl;

	ndb->table.nents = 0;
	ndb->length = NVMEIBC_SECTOR2BYTE(nlbas);

	while (nlbas) {
		unsigned int n_sg_blocks = min((unsigned int)nps_block_iter_nblocks(nbi), nlbas);

		sg_set_page(
				sg, nps_block_iter_page(nbi),
				NVMEIBC_SECTOR2BYTE(n_sg_blocks) /* length */,
				nps_block_iter_offset(nbi) /* offset */);

		nps_block_iter_advance(nbi, n_sg_blocks);
		ndb->table.nents++;
		nlbas -= n_sg_blocks;

		if (!nlbas)
			sg_mark_end(sg);
		else
			sg_unmark_end(sg);

		sg = sg_next(sg);
	}
}

bool sgl_block_iter_advance(struct sgl_block_iter *sbi, unsigned int nblocks)
{
	while (nblocks) {
		unsigned int n_blocks_in_sg, n_blocks_delta;
		BUG_ON(!sbi->sg);	// nblocks crossed SGL end
		WARN_ON(sbi->sg->length & ((1U << NVMEIBC_SECTOR_SHIFT) - 1));	// sg length is not a block multiple

		n_blocks_in_sg = NVMEIBC_BYTE2SECTOR(sbi->sg->length);
		n_blocks_delta = min(nblocks, n_blocks_in_sg - sbi->block_offset);
		sbi->block_offset += n_blocks_delta;
		nblocks -= n_blocks_delta;

		if (sbi->block_offset == n_blocks_in_sg) {
			sbi->block_offset = 0;
			sbi->sg = sg_next(sbi->sg);
		}
	}

	return !!sbi->sg;
}

void dump_block_iter(const struct nps_block_iter *nbi, nvmeib_trace_level trace_level)
{
	if (!nbi)
		return;

	_N_dmesg(trace_level, t_dump_block_iter,
		"block iter dump n_blocks=@UINT, order=@INT32_HEX, n_blks_total=@UINT, entries_left=@UINT, block_offset=@UINT, n_blocks_used=@UINT",
		nps_block_iter_nblocks(nbi), (nbi->order ? 0 : *nbi->order), nbi->n_blks_total, nbi->entries_left,
		nbi->block_offset, nbi->n_blocks_used);
}

