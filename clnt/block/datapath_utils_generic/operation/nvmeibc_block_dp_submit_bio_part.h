/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_SUBMIT_BIO_PART_H
#define NVMEIBC_DP_SUBMIT_BIO_PART_H

#include "kr_incs.h"

/**************************** H interface **********************************/
enum nvmeibc_internal_bio_indicator {
    EXTERNAL_BIO			= 0,		// Any bio not originating outside will have a function pointer in bi_end_io therefore the pointer will be aligned
    CARRIER_BIO				= 1 <<	0,	// Carrier bio masked with nvmeibc_os_api pointer
    WRAPPER_FOR_BIO			= 1 <<	1,	// Wrapper for bio may be created under different conditions:
										// bio is not 4KB aligned
										// bio buffers are muttable
										// 	 write via cache 
										// 	 read unneeded data [0-16KB) & [20KB..32KB) maybe read as [0-32KB), where [16KB-20KB] will be written to some shared throw away place
    MAX_BIO_INDICATOR		= 1 << 31	// All function pointer in actual kernels are 4 bytes aligned
};

static inline bool __check_bio_indicator(struct bio const* bio, enum nvmeibc_internal_bio_indicator indicator)
{
	return ((u64)indicator) == (((u64)(bio)->bi_end_io) & (indicator));
}

static inline bool __is_bio_wrapper_for_bio(struct bio const* bio)
{
	return __check_bio_indicator(bio, WRAPPER_FOR_BIO);
}

static inline bool __is_bio_from_carrier(struct bio const* bio)
{
	return __check_bio_indicator(bio, CARRIER_BIO);
}

// Get the original bio from the wrapper BIO
#define get_original_bio_from_wrapper(wrapper_bio) ((struct bio *)((u64)(wrapper_bio)->bi_end_io & ~WRAPPER_FOR_BIO))
#define store_original_bio_in_wrapper(bio) ((bio_end_io_t*)((u64)bio | WRAPPER_FOR_BIO))

/* A layer between BIO and block operation which allows splitting bio to a list of bios (to make them allign to specific boundaries). Represent part of a bio for inclusion in an operation structure */
struct bio_part {		// Array of such structs for together the full vlba of bio
	struct bio *bio;	/* All bio_parts point to the same original bio */
	u32 bio_offst;		/* Offset into bi_sector in within the bio. unites of 512[bytes] */
	u32 size;			/* length [bytes], within the bio. Sum of all sizes of bio parts is the original bio */
	ulong start_vlba_s;	/* Caching the vlba offset. unites of 512[bytes], Includes sub volumes offset */
	struct bio_part *ref; /*All bio parts point to the first one in the list*/
	nvmeibc_atomic_t n_ref;		/* Only the first elemenet (leader) has a ref count */
	int rv;		/* Only the leader has this value, stores the error of last erronous part*/
}; // Daniel: Todo, remove the above struct and use regular bio. Replace with bio->bi_next for list of bios, bio->__bi_remaining instead of 'n_ref', and use bio_set_flag(bio, BIO_CHAIN)

/* When your bio_exec_fn terminates, call this method to transfer the data back to OS and update internal statistics */
void block_api_os_end_io(struct bio_part *bio, unsigned long start_time, int rv);

/* Assist methods to traverse kernel bio, using iterators */
#if KS_BVEC_ITER
	typedef struct bvec_iter bio_iter_t;

	#define __GET_BI_SECTOR(b) 	((b)->bi_iter.bi_sector)
	#define __GET_BI_SIZE(b) 	((b)->bi_iter.bi_size)
	static inline bio_iter_t __BI_INIT(struct bio_part *part)
	{
		bio_iter_t iter = part->bio->bi_iter;
		bio_advance_iter(part->bio, &iter, (part->bio_offst << KERNEL_SECTOR_SHIFT));
		return iter;
	}
#else
	typedef struct {
		unsigned short bi_idx;
		unsigned int done;
	} bio_iter_t;
	#define __GET_BI_SECTOR(b)	((b)->bi_sector)
	#define __GET_BI_SIZE(b)	((b)->bi_size)
	#define BIO_NO_ADVANCE_ITER_MASK	(REQ_DISCARD|REQ_WRITE_SAME)
	static inline bio_iter_t __BI_INIT(struct bio_part *part)
	{
		bio_iter_t iter = {part->bio->bi_idx, 0 /* done == 0 */};
		unsigned bytes = part->bio_offst << KERNEL_SECTOR_SHIFT;
		if (unlikely(part->bio->bi_rw & BIO_NO_ADVANCE_ITER_MASK)) {
			iter.done = bytes;
			return iter;
		}
		while (bytes > 0) {		// Skip exactly the first 'bytes' of bio
			const unsigned left = part->bio->bi_io_vec[iter.bi_idx].bv_len;
			if (bytes < left) {
				iter.done = bytes;
				break;
			} else {
				bytes -= left;
				++iter.bi_idx;
			}
		}
		return iter;
	}
#endif

#define __GET_BI_SIZE_IN_KERNEL_SECTORS(b) (__GET_BI_SIZE((b)) >> KERNEL_SECTOR_SHIFT)
struct bio_vec __get_bio_vec(struct bio_part *bio, bio_iter_t *bio_i, unsigned bytes);
bool          __skip_bio_vec(struct bio_part *bio, bio_iter_t *bio_i, unsigned bytes);

bool is_io_aligned(		const struct bio_part *);		// Is correctly aligned to blocks. If not this is a kernel BUG
bool is_bio_aligned(	const struct bio *bio);			//
u64  get_start_lba(		const struct bio_part *);		// In blocks
u64  get_nlbas(    		const struct bio_part *);		// In blocks
u64  get_start_b(  		const struct bio_part *);		// In bytes
u64  get_length(   		const struct bio_part *);		// In bytes

/* Convert kernel bio operation type to nvmeibc type */
enum nvmeib_block_io_op __get_bio_op(const struct nvmeibc_os_api* os, const struct bio *bio);
/* Read Modify Write maps from bio_part */
u64 get_valid_wrapper_block_maps(const struct bio_part *);

/**************************** C implement **********************************/

#endif	// H file
