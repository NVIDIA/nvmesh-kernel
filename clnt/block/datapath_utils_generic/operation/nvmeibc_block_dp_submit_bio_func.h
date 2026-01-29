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
		o = nvmeibc_operation_create_with_biopart(nd->dp.sizeof_operation);
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
			nvmeib_io_stats_operation_start(o->nd->os->stats, io_op_to_verb(o->op, false), nlbas << NVMEIBC_SECTOR_SHIFT);
		}

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
