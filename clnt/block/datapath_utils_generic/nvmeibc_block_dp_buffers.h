#ifndef NVMEIBC_DP_BUFFERS_H
#define NVMEIBC_DP_BUFFERS_H
/*
 * Buffers, pages allocations etc.
 */

#include "nvmeib_shared.h"
#include "nvmeibc_block.h"

/************************** Pages allocations *********************************/
struct nvmeibc_pages {
	struct page **pages;	// Allocated pages pointers. Example Requested 11 pages: { ptr_to_8pages, ptr_to_2pages, ptr_to_1pages }
	u8 *orders;				// Corresponding orders                                  {      3              1              0        }
	u32 nallocs;			// Length of the pages array							 3
	u32 n_blks_total;		// Total amount of blocks in all pages above
	u32 nused_blks;			// How many blocks have we used so far, we set pages for parity and pre-reads and then might use more pages to copy the data for EC
} __attribute__((packed));
#define nvmeibc_pages_set_empty() ((struct nvmeibc_pages){0})

// Convenience, for block allocations
#define NVMEBC_PAGES_NBLOCKS_IN_PAGE_ORDER(order)     (1U << ((order) + PAGE_SHIFT - NVMEIBC_SECTOR_SHIFT))
#if (NVMEIBC_SECTOR_SHIFT >= PAGE_SHIFT)
	#define NVMEBC_PAGES_ORDER_BLOCK (NVMEIBC_SECTOR_SHIFT - PAGE_SHIFT)		// Block must be contigous in memory
	#define NVMEIBC_BLOCKS_TO_PAGES_ALLOC_COUNT(_blocks)      ((_blocks) << NVMEBC_PAGES_ORDER_BLOCK)
#else
	#define NVMEBC_PAGES_ORDER_BLOCK (0)
	#define NVMEIBC_BLOCKS_TO_PAGES_ALLOC_COUNT(_blocks)      DIV_ROUND_UP((_blocks), NVMEBC_PAGES_NBLOCKS_IN_PAGE_ORDER(0))
#endif

int  nvmeibc_pages_alloc(   struct nvmeibc_pages *nps, unsigned int n_blocks, const int tcp_mode);
void nvmeibc_pages_free(    struct nvmeibc_pages *nps,                        const int tcp_mode);
void nvmeibc_pages_drop_ownership(struct nvmeibc_pages *nps);

static inline bool nvmeibc_pages_is_empty(const struct nvmeibc_pages *nps) { return nps->nallocs == 0; }	// Alternative can check for n_blks_total == 0, same result
static inline void nvmeibc_pages_clean_for_reuse(struct nvmeibc_pages *nps) { nps->nused_blks = 0; }

/* Iterator over nvmeibc_pages allocated with min_order >= NVMEBC_PAGES_ORDER_BLOCK */
struct nps_block_iter {
	struct page **page;			// pointer to the current entry in the pages array
	u8 *order;					// pointer to the current entry in the orders array
	u32 n_blks_total;			// Const after initialization, taken from nvmeibc_pages
	u32 block_offset;			// offset (in blocks) in current entry
	u32 entries_left;			// count-down
	u32 n_blocks_used;			// Number of total blocks Consumed/Skipped by the iterator so far
};

#define NPS_BLOCK_ITER_INIT(_nps) (struct nps_block_iter){.page = (_nps).pages, .order = (_nps).orders, .n_blks_total = (_nps).n_blks_total, .entries_left = (_nps).nallocs, .block_offset = 0, .n_blocks_used = 0 }

bool nps_block_iter_advance(struct nps_block_iter *nbi, unsigned int nblocks);

static inline struct page* nps_block_iter_page(const struct nps_block_iter *const nbi)
{
	return *nbi->page;
}

static inline size_t nps_block_iter_offset(const struct nps_block_iter *const nbi)
{
	return NVMEIBC_SECTOR2BYTE(nbi->block_offset);
}

static inline unsigned int nps_block_iter_nblocks(const struct nps_block_iter *const nbi)
{
	return NVMEBC_PAGES_NBLOCKS_IN_PAGE_ORDER(*nbi->order) - nbi->block_offset;
}

void dump_block_iter(const struct nps_block_iter *nbi, nvmeib_trace_level trace_level);
// static inline size_t nps_block_iter_len(const struct nps_block_iter *const nbi) { return NVMEIBC_SECTOR2BYTE(nps_block_iter_nblocks(nbi)); }
// static inline void *nps_block_iter_virt(const struct nps_block_iter *const nbi) { return page_address(*nbi->page) + (nbi->block_offset << NVMEIBC_SECTOR_SHIFT); }

static inline bool nps_block_iter_empty(const struct nps_block_iter *const nbi)
{
	return (!nbi->entries_left) || (nbi->n_blocks_used == nbi->n_blks_total);
}

/************************** Buffers *******************************************/

void nvmeibc_fill_ndb(struct nvmeib_data_buffer *ndb, unsigned int nlbas , struct nps_block_iter *nbi);

/*
 * Block-by-block iteration over SGL. SGL entries are assumed to be
 * multiples of block size in length.
 */
struct sgl_block_iter {
	struct scatterlist *sg;		// current entry
	unsigned int block_offset;	// offset (in blocks) in sg
};

#define SGL_BLOCK_ITER_INIT(_sgl) { .sg = &(_sgl), .block_offset = 0 }

static inline void *sgl_block_iter_virt(const struct sgl_block_iter *const sbi)
{
	return sg_virt(sbi->sg) + (sbi->block_offset << NVMEIBC_SECTOR_SHIFT);
}

static inline unsigned int sgl_block_iter_nblocks(const struct sgl_block_iter *const sbi)
{
	return NVMEIBC_BYTE2SECTOR(sbi->sg->length) - sbi->block_offset;
}

//static inline size_t sgl_block_iter_length(const struct sgl_block_iter *const sbi) { return sgl_block_iter_nblocks(sbi) << NVMEIBC_SECTOR_SHIFT; }

bool sgl_block_iter_advance(struct sgl_block_iter *sbi, unsigned int nblocks);

void nvmeibc_pages_alloc_stats_clear(void);
void nvmeibc_pages_alloc_stats_to_txt(struct nvmeib_txt *txt);

#endif  // H beginning
