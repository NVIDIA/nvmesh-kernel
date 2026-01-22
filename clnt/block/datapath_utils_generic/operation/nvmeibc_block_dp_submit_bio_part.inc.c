/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_submit_bio_part.h"

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_block_dp_submit_bio_part_inc_c

u64 get_start_lba(const struct bio_part *bio)
{
	return bio->start_vlba_s >> KERNEL_SECTOR_TO_SECTOR_SHIFT;
}

u64 get_start_b(const struct bio_part *bio)
{
	return bio->start_vlba_s << KERNEL_SECTOR_SHIFT;
}

u64 get_nlbas(const struct bio_part *bio)
{
	return bio->size >> NVMEIBC_SECTOR_SHIFT;
}

static u64 get_nlbas_s(const struct bio_part *bio)
{
	return bio->size >> KERNEL_SECTOR_SHIFT;
}

u64 get_length(const struct bio_part *bio)
{
	return bio->size;
}

static bool is_last_bio_part(const struct bio_part *biop)
{
	const struct bio *bio = biop->bio;
	const u64 biop_size_s = get_nlbas_s(biop);
	const u64 bio_size_s = __GET_BI_SIZE_IN_KERNEL_SECTORS(bio);
	if (bio_size_s == biop_size_s) {	// Not split - always last
		return true;
	} else {	// Check if this is the last sector
		const u64 bio_start_s = (u64)__GET_BI_SECTOR(bio);
		if ((bio_start_s + bio_size_s) == (biop->start_vlba_s + biop_size_s)) {
			return true;
		}
	}
	return false;
}

#define get_wrapper_first_block_map(map, biop) (((biop)->bio_offst) ? 0 : __extract_rmw_first_block_map(map))
#define get_wrapper_last_block_map( map, biop) ((is_last_bio_part((biop))) ? (__extract_rmw_last_block_map(map) << KERNEL_SECTORS_IN_NVMEIBC_SECTOR): 0)
u64 get_valid_wrapper_block_maps(const struct bio_part *bio)
{
	u64 rv = 0;
	const u64 block_maps = (u64)(bio->bio->bi_private);
	if (block_maps) {
		rv = get_wrapper_first_block_map(block_maps, bio);
		rv |= get_wrapper_last_block_map(block_maps, bio);
	}
	return rv;
}

static inline bool is_bvec_aligned(const struct bio_vec *bvec)
{
	return !((bvec->bv_offset | bvec->bv_len) & ~PAGE_MASK); // bio_vec is not aligned to pages
}

/* bio_for_each_segment API:
   up to 3.14 (!KS_BVEC_ITER) - (struct bio_vec*, struct bio*, int i)
   after (KS_BVEC_ITER)       - (struct bio_vec , struct bio*, struct bvec_iter)
   TODO - use bio_iter_t as a generic iterator same as in union vv_bio_inter)

   returns false if any bio_vec offset or length is not a full page	*/
bool is_bio_aligned(const struct bio *bio)
{
	struct bio_vec bvec;
#if KS_BVEC_ITER
	struct bvec_iter iter;
	bio_for_each_segment(bvec, (struct bio *)(bio), iter) {
#else
	int bv_ind;
	for (bv_ind = 0; bv_ind < bio->bi_vcnt; bv_ind++) {
		bvec = bio->bi_io_vec[bv_ind];
#endif
		if (!is_bvec_aligned(&bvec))
			return false;
	}
	return true;
}

struct bio_vec __get_bio_vec(struct bio_part *bio_part, bio_iter_t *bio_i, unsigned bytes)
{
	struct bio *bio = bio_part->bio;
	struct bio_vec bv;
#if KS_BVEC_ITER
	if (!bio_i->bi_size)
		goto at_the_end;
	bv = bio_iter_iovec(bio, *bio_i);
	bio_advance_iter(bio, bio_i, bytes);
#else
	if (bio_i->bi_idx >= bio->bi_vcnt) // In elevated bio (list of bio's) once bio is finished we go to the next one
		goto at_the_end;
	bv = bio->bi_io_vec[bio_i->bi_idx];
	bv.bv_offset += bio_i->done;
	bv.bv_len -= bio_i->done;		// Up to here equivalent of bio_iter_iovec()
	if (bytes == bv.bv_len) {		// Below: Simplified reimplementation of bio_advance_iter()
		(bio_i->bi_idx)++;
		bio_i->done = 0;
	} else
		bio_i->done += bytes;
#endif
	bv.bv_len = min(bytes, bv.bv_len);	// We advanced iterators and consumed only 'bytes'[b] from total fo bv_len
	return bv;
at_the_end:
	bv.bv_offset = 0;
	bv.bv_len = 0;
	return bv;
}

bool __skip_bio_vec(struct bio_part *bio_part, bio_iter_t *bio_i, unsigned bytes)
{
	while (bytes) {
		struct bio_vec bv = __get_bio_vec(bio_part, bio_i, min(bytes, (unsigned)NVMEIBC_SECTOR_SIZE));	// TODO: this is inefficient, __get_bio_vec() doesn't support requests for size larger than sector size
		if (unlikely(bv.bv_len == 0))
			return false;
		bytes -= bv.bv_len;
	}
	return true;
}

bool is_io_aligned(const struct bio_part *bio_part)
{
	const u64 block_mask = ((1 << NVMEIBC_SECTOR_SHIFT) - 1);
	const u64 bio_bytes =  get_start_b(bio_part);
	const u64 bio_len =    bio_part->size;

	if (((bio_bytes|bio_len) & block_mask) == 0) {			// Range is aligned to blks
		if (likely(NVMEIBC_SECTOR_SHIFT == PAGE_SHIFT)) {	// Verify we are 4k aligned in each bi_io_vec
			return is_bio_aligned((const struct bio*)(bio_part->bio));
		}
		return true;
	}
	return false;
}

#pragma pop_macro("__FILE_LITERAL__")
