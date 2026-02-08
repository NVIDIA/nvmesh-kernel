#ifndef NVMEIBC_DP_SUB_BLOCK_UTILS_H
#define NVMEIBC_DP_SUB_BLOCK_UTILS_H
/* Datapath Utils for handling partial blocks. Like Read-modify-Write sub blocks
   Copy partial blocks, etc
*/

#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "clnt/block/nvmeibc_block_common.h"
#include "nvmeibc_block.h"

/**************************** H interface **********************************/
int execute_bio(            struct bio *, ulong now);		// Exec regular bio
int execute_bio_with_wrapper(        struct bio *, ulong now);		// Exec 512 sub-blocks bio in 4K block device
void sub_block_op_bio_endio(struct bio *, int rv);			// End  512 sub-blocks bio
void __copy_sub_block_by_map(unsigned char *dst, unsigned char *src, const ulong map);

/**************************** C implement **********************************/
// This function copies data between two bios, assuming they are in kernel sized (512B) sectors
static void copy_512B_aligned_data(struct bio *src_bio, u32 src_bio_kernel_sector_offset, struct bio *dst_bio, u32 dst_bio_kernel_sector_offset)
{
#if KS_BVEC_ITER
	// Store current bvec iter on stack
	const struct bvec_iter dst_iter = dst_bio->bi_iter;
	const struct bvec_iter src_iter = src_bio->bi_iter;
	// Modify BIO iter in place
	src_bio->bi_iter.bi_bvec_done += src_bio_kernel_sector_offset << KERNEL_SECTOR_SHIFT;
	dst_bio->bi_iter.bi_bvec_done += dst_bio_kernel_sector_offset << KERNEL_SECTOR_SHIFT;
	// Copy from modified BIO iter
	bio_copy_data(dst_bio, src_bio);
	// Restore BIO iter to original values
	src_bio->bi_iter = src_iter;
	dst_bio->bi_iter = dst_iter;
#else
	// Use bio_copy_data but with integrated offsets
	// 2 Assumptions: 	1. All kernel BIOs do not have offsets
	// 					2. Wrapper BIOs have a single bio_vec and can use offset
	nvmeib_bio_copy_data_with_offsets(dst_bio, dst_bio_kernel_sector_offset << KERNEL_SECTOR_SHIFT,
									  src_bio, src_bio_kernel_sector_offset << KERNEL_SECTOR_SHIFT);
#endif
}

void sub_block_op_bio_endio(struct bio *wrapper_bio, int rv)
{
	struct bio *bio = get_original_bio_from_wrapper(wrapper_bio);	// The original sub-block bio
	const ulong lba_bio_start_s = (ulong)__GET_BI_SECTOR(bio);		// Sector offset in the original bio
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	const enum nvmeib_block_io_op op = __get_bio_op(os, bio);	// Wrapper bio might be RMW op type

	if (nvmeib_block_io_op_is_read(op)) { // Copy the result of read into the original bio, at the right offsets
		// Yes, we don't care about rv.
		// Why? Because this is execute_bio behaviour. "fast read" implemented as "read" and "view lock";
		// Even if the lock is contended and user get -EIO as the result, the information from the disk is already presented. 
		// Thus, I expose it here too

		// The difference between the original bio sector offset and the starting sector aligned to NVMESH_SECTOR_SIZE
		// (both in units of 512b) is how much we should skip when copying to the original bio
		const u32 src_bio_kernel_sector_offset = lba_bio_start_s % KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
		copy_512B_aligned_data(wrapper_bio, src_bio_kernel_sector_offset, bio, 0);
	}

	if (!rv) {
		_ND(taai4a_a, "Finished sub-block '@BLOCK_IO_OP' operation for wrapper (@BIO) [@INT64, @INT64), original bio (@BIO) was [@INT64, @INT64)",
			op, wrapper_bio,
			(ulong)__GET_BI_SECTOR(wrapper_bio), (ulong)__GET_BI_SECTOR(wrapper_bio) + bytes_to_kernel_blocks(__GET_BI_SIZE(wrapper_bio)),
			bio, lba_bio_start_s, lba_bio_start_s + bytes_to_kernel_blocks(__GET_BI_SIZE(bio)));

	} else {
		_NE(taai4a_b, "'@BLOCK_IO_OP' operation for wrapper (@BIO) failed with status (@RV) for [@INT64, @INT64), original bio (@BIO) was [@INT64, @INT64). Wrapper bio length (@INT64) with page order(@INT64)",
			op, wrapper_bio, rv,
			(ulong)__GET_BI_SECTOR(wrapper_bio), (ulong)__GET_BI_SECTOR(wrapper_bio) + bytes_to_kernel_blocks(__GET_BI_SIZE(wrapper_bio)),
			bio, lba_bio_start_s, lba_bio_start_s + bytes_to_kernel_blocks(__GET_BI_SIZE(bio)),
			__GET_BI_SIZE(wrapper_bio), get_order(wrapper_bio->bi_io_vec[0].bv_len));
	}
	{	// Free wrapper bio resources
		unsigned short i;
		for (i = 0; i < wrapper_bio->bi_vcnt; i++) {
			struct bio_vec *bv = &wrapper_bio->bi_io_vec[i];
			BUG_ON(!bv->bv_page);
			__free_pages(bv->bv_page, get_order(bv->bv_len));
		}
		wrapper_bio->bi_vcnt = 0;
	}
	kfree(wrapper_bio);
}

union extended_rmw_metadata {
	u64 block_maps;
};

static inline const struct nvmeibc_cinst_params_blk * __bio_get_cinst_params_blk(const struct bio* bio)
{
	const struct nvmeibc_block_device* bdev = get_bdev_or_parent_bdev_of_bio(bio);
	return bdev->cips;
}


static inline struct bio *allocate_and_init_4k_aligned_bio(struct bio *bio, const enum nvmeib_block_io_op op, int *rv,
													uint num_4k_blocks, union extended_rmw_metadata *rmw)
{
	struct bio *wrapper_bio = NULL;
	struct nvmeibc_pages pga = nvmeibc_pages_set_empty();
	const gfp_t gdp_f = nvmeibc_dp_get_allow_io_gfp_flags() | __GFP_NOWARN;
	const struct nvmeibc_cinst_params_blk* cinst_params_blk = __bio_get_cinst_params_blk(bio);
	const int tcp_mode = nvmeibc_cinst_params_blk_get_cinst_params_core_tcp_mode(cinst_params_blk);
	const unsigned int total_bytes = (num_4k_blocks << NVMEIBC_SECTOR_SHIFT);
	if (unlikely(*rv != 0))
		goto _out;
	*rv = nvmeibc_pages_alloc(&pga, num_4k_blocks, tcp_mode);
	if (unlikely(*rv != 0)) {
		_NW(taai4a_0, "'@BLOCK_IO_OP' failed - couldn't allocate @INT pages for wrapper bio", op, num_4k_blocks);
		goto _out;
	}
	#if KS_BIO_ALLOC_HAS_BLOCK_DEVICE
		wrapper_bio = bio_kmalloc(pga.nallocs, gdp_f);
	#else
		wrapper_bio = bio_kmalloc(gdp_f, pga.nallocs);
	#endif

	if (unlikely(!wrapper_bio)) {
		_NW(taai4a_1, "'@BLOCK_IO_OP' failed - couldn't allocate wrapper bio; nr_iovecs=@UINT, gfp=@UINT", op, pga.nallocs, gdp_f);
		goto _free_wrapper_bio;
	}
	{
		u32 i;
		// Copy original bio's properties to the 4k aligned bio
		__BIO_CLONE_FAST(wrapper_bio, bio);
		__GET_BI_SECTOR(wrapper_bio) = kernel_block_offset_in_nvmeibc_sector_size_alignment((ulong)__GET_BI_SECTOR(bio));

		{ // Convert nvmeibc_pages to bio_vecs pages
			unsigned int remainig_total_bytes = total_bytes;
			for (i = 0; i < pga.nallocs; i++) {
				const unsigned int max_bv_len = (PAGE_SIZE << pga.orders[i]);
				const unsigned int bv_len = min(remainig_total_bytes, max_bv_len);
				if (bio_add_page(wrapper_bio, pga.pages[i], bv_len, 0) < (int)bv_len) {
					_NW(taai4a_2, "'@BLOCK_IO_OP' failed - couldn't perform bio_add_page, idx=@INT, len=@INT", op, i, bv_len);
					goto _free_wrapper_bio;
				}
				remainig_total_bytes -= bv_len;
			}
		}

		nvmeibc_pages_drop_ownership(&pga);

		// Make sure the size of the wrapper bio reflects the 4k aligned size
		BUG_ON(__GET_BI_SIZE(wrapper_bio) != total_bytes);
		wrapper_bio->bi_private = (void*)rmw->block_maps; // Keep the original bio for completion as private data, Store Read Modify Write maps in bi_private, See get_valid_wrapper_block_maps()
		wrapper_bio->bi_end_io = store_original_bio_in_wrapper(bio); // The bi_end_io field will be used to distinguish wrapper bios and other bios, using the actual function pointer.
		if (nvmeib_block_io_op_is_write(op)) {	// For write bio's, copy the data from the original, non sequentially allocated pages, onto the newly allocated buffer:
			const u32 bio_kernel_sector_offset = (ulong)__GET_BI_SECTOR(bio) % KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
			copy_512B_aligned_data(bio, 0, wrapper_bio, bio_kernel_sector_offset);
			BUG_ON(__GET_BI_SIZE(wrapper_bio) != total_bytes); // BIO after copy must still have the same length
		}
		_ND(taai4a_3, "Transformed sub-block bio (@BIO) (offset @INT64, size @SIZE) to 4K aligned wrapper (@BIO) (offset @INT64, size @SIZE)",
			bio, (ulong)__GET_BI_SECTOR(bio), (u32)__GET_BI_SIZE(bio),
			wrapper_bio, (ulong)__GET_BI_SECTOR(wrapper_bio), (u32)__GET_BI_SIZE(wrapper_bio));
	}
_out:
	return wrapper_bio;

_free_wrapper_bio:
	nvmeibc_pages_free(&pga, tcp_mode);
	*rv = -EPERM;
	if (wrapper_bio)
		kfree(wrapper_bio);
	return NULL;
}

void __copy_sub_block_by_map(unsigned char *dst, unsigned char *src, const ulong map)
{
	int i, pre = 0;
	if (hweight_long(map) == 1) {
		i = find_first_bit(&map, KERNEL_SECTORS_IN_NVMEIBC_SECTOR);
		if (i > 0) {
			const int offset_s_b = i << KERNEL_SECTOR_SHIFT;
			memcpy(dst, src, offset_s_b);
		}
		if (i < KERNEL_SECTORS_IN_NVMEIBC_SECTOR - 1) {
			const int offset_s_b = (i + 1) << KERNEL_SECTOR_SHIFT;
			memcpy(&dst[offset_s_b], &src[offset_s_b], NVMEIBC_SECTOR_SIZE - offset_s_b);
		}
	} else {
		for_each_set_bit(i, &map, KERNEL_SECTORS_IN_NVMEIBC_SECTOR) {
			if (i == 0) {
				pre++;
				continue;
			} else {
				const int offset_s_b = (i + pre) << KERNEL_SECTOR_SHIFT;
				if (pre == 0) {
					memcpy(dst, src, offset_s_b);
					pre++;
				} else if ((pre == 1) && (offset_s_b < NVMEIBC_SECTOR_SIZE)) {
					memcpy(&dst[offset_s_b], &src[offset_s_b], NVMEIBC_SECTOR_SIZE - offset_s_b);
					pre++;
				} else if (pre > 1) BUG();
			}
		}
	}
}

enum bio_wrap_reason
{
	BIO_WRAPPER_REASON_NO_NEED,
	BIO_WRAPPER_REASON_UNALIGNED,
};

static inline enum bio_wrap_reason __bio_is_wrapper_required(const enum nvmeib_block_io_op op, const struct bio *bio)
{
	const struct nvmeibc_block_device* bdev = get_bdev_or_parent_bdev_of_bio(bio);
	const bool unaligned_io_allowed = nvmeibc_block_is_kernel_sector_io_allowed(bdev);
	const ulong lba_bio_start_s =  (ulong)__GET_BI_SECTOR(bio);
	const long total_size_b = __GET_BI_SIZE(bio);

	const bool is_aligned_io =
		(0 == (lba_bio_start_s & (KERNEL_SECTORS_IN_NVMEIBC_SECTOR - 1))) && // Either Kernel sector offset is not aligned to KERNEL_SECTORS_IN_NVMEIBC_SECTOR
		(0 == (total_size_b & (KERNEL_SECTORS_IN_NVMEIBC_SECTOR - 1))) &&   // Or byte length is not aligned to NVMEIBC_SECTOR_SIZE
		is_bio_aligned(bio);                                                 // Or the bio buffers are not aligned to pages (traverses BIO according to kernel version)

	if (unlikely(op == NVMEIB_BLOCK_IO_OP_DISCARD)) {					// Ignore all discard/trims will be completed without doing anything
		return BIO_WRAPPER_REASON_NO_NEED;
	}

	if (unaligned_io_allowed && false == is_aligned_io){
		return BIO_WRAPPER_REASON_UNALIGNED;
	}

	return BIO_WRAPPER_REASON_NO_NEED;
}

static inline int handle_bio_with_wrapper(struct bio *bio, unsigned long now, enum bio_wrap_reason wrap_reason)
{
	int rv = 0;
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	const enum nvmeib_block_io_op op = __get_bio_op(os, bio);
	const ulong lba_bio_start_s =  (ulong)__GET_BI_SECTOR(bio);
	const long total_size_b = __GET_BI_SIZE(  bio);

		struct bio *wrapper_bio = NULL;
		union extended_rmw_metadata maps = {0};
		u64 start_4k_block, last_4k_block;
		// The number of NVMEIBC_SECTOR_SIZE sized blocks required by the wrapper bio
		const uint num_4k_blocks = get_numer_of_required_4k_blocks_from_512B_IO(lba_bio_start_s, total_size_b, &start_4k_block, &last_4k_block);
	if (nvmeib_block_io_op_is_write(op) && (wrap_reason == BIO_WRAPPER_REASON_UNALIGNED)) {  // Check if read before write required
			const u64 first_block_rmw = lba_bio_start_s % KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
			const u64 last_block_rmw = (lba_bio_start_s + bytes_to_kernel_blocks(total_size_b)) % KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
			if (first_block_rmw) {	// Mark first sub-block in first block
				maps.block_maps = (first_block_rmw) ? (1ULL << first_block_rmw) : 0;
			}
			if (num_4k_blocks == 1) {	// Single block
				if (last_block_rmw) {	// Single block mark last required sub-block
					if (maps.block_maps == 0) {	// First sub-block is required mark it
						maps.block_maps = 1ULL;
					}
					maps.block_maps |= (1ULL << (last_block_rmw - 1));
				} else if (maps.block_maps) {	// Single block mark last possible sub-block as required
					maps.block_maps |= (1ULL << (KERNEL_SECTORS_IN_NVMEIBC_SECTOR - 1));
				}	// else fully aligned single block
			} else {	// First block was maped (or clear), add last block map if required
				if (first_block_rmw) {	// Mark last sub-block of first block
					maps.block_maps |= (1ULL << (KERNEL_SECTORS_IN_NVMEIBC_SECTOR - 1));
				}
				if (last_block_rmw) {   // Last block requires rmw, mark first and last sub-blocks required in last block
					maps.block_maps |= (1ULL << KERNEL_SECTORS_IN_NVMEIBC_SECTOR);
					maps.block_maps |= (1ULL << (last_block_rmw - 1)) << KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
				}
			}
		}

		_ND(t_exec_sub_bio_1,
		"Trasforming bio '@BLOCK_IO_OP', block size (@INT64) from [@INT64, @INT64) to 4K aligned io [@INT64, @INT64]; reason=@HEX_1B",
			op, get_dev_logical_block_size_from_bio(bio),
			lba_bio_start_s, lba_bio_start_s + bytes_to_kernel_blocks(total_size_b),
		nvmeibc_block_to_kernel_block(start_4k_block), nvmeibc_block_to_kernel_block(last_4k_block),
	 	(u8)wrap_reason);

		wrapper_bio = allocate_and_init_4k_aligned_bio(bio, op, &rv, num_4k_blocks, &maps);

		if (likely(wrapper_bio)) {	// Issued all the wrapper BIOs
			rv = execute_bio(wrapper_bio, now);
		} else {
			struct nvmeibc_block_device *bdev = get_bdev_or_parent_bdev_of_bio(bio); 
			_NE(e_exec_sub_bio, "Failed allocating a wrapper BIO");
			rv = -ENOMEM;
			IO_STATS_INCR(&bdev->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL);
	}
	return rv;
}



int execute_bio_with_wrapper(struct bio *bio, unsigned long now)
{
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	const enum nvmeib_block_io_op op = __get_bio_op(os, bio);
	const enum bio_wrap_reason wrap_reason = __bio_is_wrapper_required(op, bio);

	BUG_ON(__is_bio_wrapper_for_bio(bio)); // Should only be generated here
	if (wrap_reason == BIO_WRAPPER_REASON_NO_NEED) {	// If BIO is aligned to 4k or is discard no need to add a wrapper and BIO can execute
		return execute_bio(bio, now); // Valid sub block operation execute it as is
	} else {
		const int rv = block_api_os_verify_bio_geometry(bio);
		if (unlikely(rv)){
			struct nvmeibc_block_device *bdev = get_bdev_or_parent_bdev_of_bio(bio); 
			IO_STATS_INCR(&bdev->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL);
			return rv;
		}
		return handle_bio_with_wrapper(bio, now, wrap_reason);
	}
}

#endif	// H file
