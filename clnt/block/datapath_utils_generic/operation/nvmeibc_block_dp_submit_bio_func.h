/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_SUBMIT_BIO_FUNC_H
#define NVMEIBC_DP_SUBMIT_BIO_FUNC_H
/* Datapath Utils function for BIO execution. Used for submitting BIO into krnel request
	queue, IO from rider volume or externally via other mechanism.
*/

#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeibc_block.h"
#include "nvmeibc_io_pet.h"

#ifndef __KERNEL__
	#include "nvmeib_math.h"	/* For round_up/round_down in user-space */
#endif

/**************************** H interface **********************************/
int execute_bio(struct bio *bio, ulong now);

/**************************** C implement **********************************/
#include "block/nvmeibc_block_api_os.h"					// Get bdev + sub_vol offset from bio

static inline bool __ops_in_same_blockset(struct bio_part *o1, struct bio_part *o2, u64 slice_size) {
	const u64 num_kern_sectors_in_blockset = ((slice_size * LOCKSET_SLICES) << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	return (o1->start_vlba_s / num_kern_sectors_in_blockset) == (o2->start_vlba_s / num_kern_sectors_in_blockset);
}

static inline void __warn_on_bug_EXC_1794_disobedient_kernel(long *total_size_b, struct nvmeibc_block_device *dev)
{
	const u64 nlbas = (*total_size_b >> NVMEIBC_SECTOR_SHIFT);
	const u64 max_nlbas_split = block_api_os_get_max_supported_trim_blks(dev->os);
	if (unlikely(nlbas > max_nlbas_split)) {
		const u64 max_nlbas = 256 * max_nlbas_split; // Roughly a few gigabytes. bio will be split to 256 parts
		IO_STATS_INCR(&dev->dp.io_stats, DP_IO_STATS_ILLEGAL_TRIMS);
		/* This should never happen - kernel (blkdev_issue_discard) looks at
		 * max_discard_sectors and breaks long trims into short BIOs accordingly
		 * (except in specific versions where it does not, hence the define),
		 * but there is still some race condition where discard processing in kernel
		 * apparently does not see the updated value of max_discard_sectors.
		 * When this happens, we print a warning and continue (split to BIO parts).
		 */
		if (nlbas > max_nlbas) {
			*total_size_b = (max_nlbas << NVMEIBC_SECTOR_SHIFT);		// Shorten the bio, because even after split it will be enormous
			_NW_to_user(t_01_1794, DMESG_PREFIX("@DEV_NAME"), "Kernel known bug (typically mkfs) issued discard operation exceeding max IO size (@NLBAS > @NLBAS). Discard operation will be partially executed. Error code: 1050.", dev->name, nlbas, max_nlbas_split);
		} else {
			_NW_to_user(t_02_1794, DMESG_PREFIX("@DEV_NAME"), "Kernel known bug (typically mkfs) issued discard operation exceeding max IO size (@NLBAS > @NLBAS). Discard operation will be executed but at slower rate. Error code: 1051.", dev->name, nlbas, max_nlbas_split);
		}
	}
}

static inline u32 __get_split(const struct nvmeibc_block_device *nd, const enum nvmeib_block_io_op op)
{
	if (nvmeib_block_io_op_is_write(op))
		return nd->dp.alignment_sectors.write;
	if (op == NVMEIB_BLOCK_IO_OP_DISCARD)
		return block_api_os_get_max_supported_trim_blks(nd->os) << KERNEL_SECTOR_TO_SECTOR_SHIFT;
	return nd->dp.alignment_sectors.read;
}

/**
 * __is_multiple_blocksets - Check if an I/O spans multiple blocksets
 * @lba_bio_start_s: Starting LBA in kernel sectors (512 bytes)
 * @size: Size in bytes
 * @slice_size: Number of blocks (4KB) per slice (e.g., 128 for EC 4+2, 256 for EC 8+2)
 *
 * A blockset in VLBA (volume) space is LOCKSET_SLICES (32 blocks = 128KB on single disk)
 * times the slice_size (number of data disks). For example:
 *  - EC 4+2: blockset = 32 * 128 = 4096 blocks = 512KB
 *  - EC 8+2: blockset = 32 * 256 = 8192 blocks = 1MB
 *  - Mirror/JBOD: blockset = 32 * 1 = 32 blocks = 128KB (BYTES_IN_LOCKSET)
 *
 * Note: BYTES_IN_LOCKSET (128KB) represents the lock granularity on a single disk segment.
 * The actual blockset size in volume space depends on the RAID configuration.
 *
 * Returns: true if I/O spans multiple blocksets, false otherwise
 */
static inline bool __is_multiple_blocksets(ulong lba_bio_start_s, long size, u32 slice_size)
{
	ulong bio_start_b;
	ulong blockset_size_b;
	ulong align_down_start;
	ulong align_up_end;

	BUG_ON(size <= 0);
	BUG_ON(slice_size == 0);

	/* Convert start from sectors to bytes */
	bio_start_b = lba_bio_start_s << KERNEL_SECTOR_SHIFT;

	/* Calculate blockset size in bytes for VLBA space:
	 * blockset = LOCKSET_SLICES (32 blocks) * slice_size * block_size (4KB)
	 * LOCKSET_SLICES is already in blocks, so multiply by slice_size and block size */
	blockset_size_b = ((ulong)LOCKSET_SLICES * slice_size) << NVMEIBC_SECTOR_SHIFT;

	/* Align down the start address to blockset boundary */
	align_down_start = round_down(bio_start_b, blockset_size_b);

	/* Align up the end address to blockset boundary */
	align_up_end = round_up(bio_start_b + size, blockset_size_b);

	/* If the span is larger than one blockset, return true */
	return (align_up_end - align_down_start) > blockset_size_b;
}

__attribute__((nonnull(1)))
static inline void __pet_nvmeibc_log_operation_create(struct operation *o, u32 short_volume_id)
{
	u32 bio_part_ofst_s = o->bios[0]->bio_offst;

	NVMEIBC_IO_PET_MSG_NORM(&o->journal,
		"operation.create(short_volume_id=%d, o=%p, type=%d<enum nvmeib_block_io_op>, vlba_start_s=0x%llx, nlbas=%llu, bio_part_ofst_s=0x%x)",
		short_volume_id, o, o->op, get_op_start_lba(o), get_op_nlbas(o), bio_part_ofst_s);
}

#define BIO_LEADER_REF  0x40000000
int execute_bio(struct bio *bio, unsigned long now)
{
	int rv = 0;
	struct operation *o;
	struct operation *chain_head = NULL, *chain_prev = NULL;
	ulong sub_offset = 0, sub_len = ~0UL;							// len - Irrelevant for testing of size (volume can be auto extandable)
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	struct nvmeibc_block_device *nd = block_api_os_get_base_bdev(os, &sub_offset, &sub_len);	// Important: os != nd->os
	struct bio_part *ldr = NULL;													// Leader part of bio
	const enum nvmeib_block_io_op op = __get_bio_op(os, bio);						//Even if sub-read is done, there will be no change to the operation
	const u32 split_s = __get_split(nd, op);
	const ulong lba_bio_start_s = (ulong)__GET_BI_SECTOR(bio);
	long total_size_b = __GET_BI_SIZE(bio);
	long bio_part_ofst_s = 0UL, bio_part_size_b;
	struct nvmeib_cpu_mask_info cpu_mask_info;
	struct nvmeib_pet_base_controller* io_pet_controller;

	/* Subset of out of bound tests taken from operation code. Needed to avoid wrong split */
	rv = block_api_os_verify_bio_geometry(bio);
	if (rv){
		goto _out;
	}

	if (op == NVMEIB_BLOCK_IO_OP_DISCARD)
		__warn_on_bug_EXC_1794_disobedient_kernel(&total_size_b, nd);

	nvmeibc_b_dp_cpu_masks_get_on_cpu(&nd->dp.cpu_masks, &cpu_mask_info);

	#define __n_bytes_remaining() (total_size_b - (bio_part_ofst_s << KERNEL_SECTOR_SHIFT))
	for (bio_part_size_b = __n_bytes_remaining(); bio_part_size_b > 0; bio_part_ofst_s += (bio_part_size_b >> KERNEL_SECTOR_SHIFT), bio_part_size_b = __n_bytes_remaining()) {
		const long cur_start_s = lba_bio_start_s + bio_part_ofst_s;
		if (split_s) {
			long max_bytes = (split_s - (cur_start_s % split_s)) << KERNEL_SECTOR_SHIFT;
			if (bio_part_size_b > max_bytes)
				bio_part_size_b = max_bytes;
		}
		io_pet_controller = __is_multiple_blocksets(cur_start_s, bio_part_size_b, nd->dp.p.slice_size) ? NULL : nd->io_pet_controller;
		o = nvmeibc_operation_create_with_biopart(nd->dp.sizeof_operation, io_pet_controller);
		if (unlikely(!o)) {
			rv = -ENOMEM;
			goto _out;
		}
		if (ldr == NULL) {
			ldr = o->bios[0];
		} else {
			if (!nvmeibc_operation_of_bio_part(ldr)->flags.was_bio_part_split) {
				nvmeibc_operation_start_bio_part(ldr, 1 + BIO_LEADER_REF);	// Initialize rldr: 1 + fictitious self reference on leader to prevent it from being freed before splitting is finished
			}
			nvmeibc_operation_add_bio_part(ldr);
		}
		o->bios[0]->ref = ldr;
		o->bios[0]->bio = bio;
		o->bios[0]->bio_offst = bio_part_ofst_s;
		o->bios[0]->size = bio_part_size_b;
		o->bios[0]->start_vlba_s = sub_offset + (ulong)cur_start_s;	// Sub vol offset + bio start offset + bio_part offset
		o->num_bios = 1;
		o->op = op;
		o->jiffies1 = now;
		#ifdef DEBUG_NON_DIRECT_IO
		{
			extern void __non_direct_init_verification(struct operation *o);
			__non_direct_init_verification(o);
		}
		#endif
		BLKCMP_SO_BLOCKING_CONTEXT_ALLOC(o);
		o->nd = nd;
		o->cpu_mask_info = cpu_mask_info; /* STRUCT ASSIGNMENT */
		if (o->op < NVMEIB_BLOCK_IO_OP_DISCARD) {
			const u64 nlbas = get_op_nlbas(o);
			nvmeib_io_stats_operation_start(o->nd->stats, io_op_to_verb(o->op, false), nlbas << NVMEIBC_SECTOR_SHIFT);
		}

		__pet_nvmeibc_log_operation_create(o, nvmeibc_volume_short_id(nd));

		if (!chain_head) /* Starting new chain */
			chain_head = o;

		if (chain_prev) { /* Lets see if we can continue the chain */
			if ((split_s != 0) && __ops_in_same_blockset(o->bios[0], chain_prev->bios[0], nd->dp.p.slice_size)) {	// No split - no chains, who cares
				chain_prev->chained_op = o;
			} else { /* Release the last chain and start the new one */
				//dp_start_take_stats(bio);
				if (nvmeibc_operation_throttling_check_should_execute(chain_head))
					nvmeibc_operation_execute_chain(chain_head);
				chain_head = o; /* This op will start the new chain */
			}
		}

		chain_prev = o;
	}

_out:
	if (ldr) {
		if (unlikely(rv)) {		// Wait for already sent BIO parts, but remember the error.
			ldr->rv = rv;
			rv = 0;
		}

		// Execute the residual chain, error or not
		BUG_ON(!chain_head);
		if (nvmeibc_operation_of_bio_part(ldr)->flags.was_bio_part_split) {
			WARN_ON(nvmeibc_atomic_sub_return(BIO_LEADER_REF, &ldr->n_ref) == 0);  // How come? This chain still holds refs...
		}
		if (nvmeibc_operation_throttling_check_should_execute(chain_head))
			nvmeibc_operation_execute_chain(chain_head);
	} else {
		WARN_ON(chain_head);
		WARN_ON(!rv);	// and therefore no need to sub leader ref
		IO_STATS_INCR(&nd->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL);
	}
	return rv;
}

#endif	// H file
