/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once

#ifdef __KERNEL__
#include <kr_incs.h>
#endif

#include "nvmeibc_core_dbgdi_blk.h"

#ifndef DBGDI_REMOVED_IN_PRODUCTION
#define for_each_stamp_in_magic_area(_iter, _area)                             \
	BUILD_BUG_ON(sizeof(*_iter) != sizeof(*(_area)->a));                       \
	BUILD_BUG_ON(sizeof((_area)->a->raw) != sizeof(*(_area)->a));              \
	for (_iter = (_area)->a; _iter < &(_area)->a[ARRAY_SIZE((_area)->a)];      \
	     _iter += NVMEIBC_CORE_DBG_DI_POSION_AREA_GRANULARITY)

#define for_each_core_writer_area(_iter, _data)                                \
	for (_iter = (_data)->wr; _iter < &(_data)->wr[ARRAY_SIZE((_data)->wr)];   \
	     ++_iter)

void *dp_dbgdi_get_page_start_from_core(void *core_dbgdi_data);
#ifdef __KERNEL__
	static bool __dp_dbgdi_save_last_read_page(void *page) {
		extern atomic_t dp_dbgdi_last_read_page_guard;
		extern char dp_dbgdi_last_read_page[PAGE_SIZE];
		if (atomic_inc_return(&dp_dbgdi_last_read_page_guard) == 1) {
			memcpy(dp_dbgdi_last_read_page, dp_dbgdi_get_page_start_from_core(page), PAGE_SIZE);
			return true;
		}
		return false; /* Else someone else already did it */
	}
	#define BUG_ON_SAVE_LAST_PAGE(cond_, page_, fmt, ...) ({                       \
			if (!!(cond_)) {                                                       \
				__dp_dbgdi_save_last_read_page(page_);                             \
				printk(KERN_ALERT fmt, ##__VA_ARGS__);                             \
				BUG();                                                             \
			}                                                                      \
		})
#else		/* Inside user space parser */
	#define __dp_dbgdi_save_last_read_page(...) 	(false)
	#define BUG_ON_SAVE_LAST_PAGE(cond_, ...) BUG_ON(cond_)
#endif

void* dp_dbgdi_get_core_area_container(const void *c);

static struct t_core_dbgdi_wr *__get_my_side_wr(const char *disk_name,
                                         struct t_core_dbgdi *c, bool pre) {
	size_t i;
	bool was_written = false;
	for (i = 0; i < ARRAY_SIZE(c->wr); ++i) {
		if (pre || c->wr[i].magic == NVMEIBC_CORE_DBG_DI_MAGIC_WR) {
			was_written = true;
			if (!strncmp(disk_name, c->wr[i].disk_name,
			             sizeof(c->wr[i].disk_name))) {
				return &c->wr[i];
			}
		}
	}
	/* If it was written, I expect to find at least one matching cell on either
	   side of the raid. */
	/* Yuri: TODO: Reenable this when/if the issue with per volume
	   default dbg di is resolved. */
	/* BUG_ON_SAVE_LAST_PAGE(was_written, c); */
	(void)was_written;
	return NULL;
}

/* Can't include jam from here. Dependency hell. */
u64 nvmeibc_jam_decode_lba(u64 enc_lba);

void __print_block_debug(const void *s);
/**
 * @param data data buffer
 * @param block_dlba dlba, as block sees it, if it is a journal write, else 0
 * @param block_dlba journal lba if it is a journal write, else 0
 * @param op opcode
 * @param page page number in sg list
 * @param p other arguments from the core layer
 */
/* before posting write-op to wire/ldisk */
static inline void
data_blk_fill_for_core_wr_pre(void *_core_dbg_di, u64 block_dlba, u64 jrnl_lba, u8 op, u32 page,
                              struct t_core_dbgdi_params_pre *p) {
	struct t_core_dbgdi *c     = _core_dbg_di;
	struct t_core_dbgdi_wr *wr = __get_my_side_wr(p->disk_name, c, true);
	void *data_4k_block = dp_dbgdi_get_core_area_container(c);

	p->start_dlba = nvmeibc_jam_decode_lba(p->start_dlba); /* safe side */

	c->rd.magic = NVMEIBC_CORE_DBG_DI_MAGIC_INV;

	/* At write - pre stage, there must also be a valid WR filled by the upper
	   layer. */
	BUG_ON_SAVE_LAST_PAGE(!wr, _core_dbg_di,
	                      "PRE-WRITE: WR section is not set by the upper "
	                      "layer, should not happen.");

	wr->magic = NVMEIBC_CORE_DBG_DI_MAGIC_WR;
	wr->op = op;
	wr->ch_type = p->ch_type;
	wr->reuse_bb = p->reuse_bb;
	wr->lock_pgbk = *(p->lock_pgbk);
	wr->container_ptr = (u64)data_4k_block;
	wr->ch_ptr = p->ch_ptr;
	wr->io_id = p->io_id;
	if (jrnl_lba &&
		jrnl_lba != p->start_dlba + page &&
		block_dlba != p->start_dlba + page) {
		__print_block_debug(data_4k_block);
		block_dlba = p->start_dlba + page;
		jrnl_lba = 0;
	}
	if (jrnl_lba) {
		wr->lba.dlba = block_dlba;
		wr->lba.jlba = jrnl_lba;
	} else {
		wr->lba.dlba = p->start_dlba + page;
		wr->lba.jlba = 0;
	}

	/**
	 * Magic area is a LARGE area used to capture partially written BB.
	 * The flow:
	 *  1. Pre write => Generate new stamp, fill magic area with it
	 *  2. Post read => Remember the last stamp. Iterate over the
	 *     magic area, if at least one element is equal to the old
	 *     stamp it means the BB was not fully written.
	 *     After that, update the last known stamp.
	 *  Some cases that are not caught (not common so just ignored):
	 *     - First read ever.
	 *     - Multiple subsequent IOs to the same dlba - only first is checked.
	 *     - Multi sector IO to more then 32 sectors.
	 *     - Read after fail - BB is in unknown state
	 */

	if (p->magic_data && page < NVMEIBC_MAX_CORE_DBGDI_IO_INJECTION) {
		union t_core_magic_area_cell *stamp,
		    *new_stamp = nvmeibc_channel_dbg_di_magic_data_gen_stamp(
		        p->magic_data, page);
		/* We don't know how this op will end */
		p->magic_data->bb_image[page].is_valid = false;
		for_each_stamp_in_magic_area(stamp, &wr->magic_area) {
			*stamp = *new_stamp;
		}
	} else {
		/* Zero the magic area on page */
		union t_core_magic_area_cell *stamp;
		for_each_stamp_in_magic_area(stamp, &wr->magic_area) {
			__INIT_MAGIC_AREA_CELL(stamp);
		}
	}
}

/* after completion of read-op from wire/ldisk */
static inline void
data_blk_fill_for_core_wr_post(void *_core_dbg_di, u64 block_dlba, u64 jrnl_lba, u8 op, u32 page,
                               struct t_core_dbgdi_params_post *p) {
	struct t_core_dbgdi *c = _core_dbg_di;
	struct t_core_dbgdi_wr *wr = __get_my_side_wr(p->disk_name, c, false);

	BUG_ON_SAVE_LAST_PAGE(
	    !wr, _core_dbg_di,
	    "POST-WRITE: WR section is missing in post write. Are running FIO with "
	    "debgu DI on (shame on you if so)?");

	p->start_dlba = nvmeibc_jam_decode_lba(p->start_dlba); /* safe side */

	(void)_core_dbg_di;
	(void)op;
	(void)block_dlba;
	(void)jrnl_lba;

	/* If the write was successful, we should start checking magic on the next
	   operations. */
	if (p->magic_data && page < NVMEIBC_MAX_CORE_DBGDI_IO_INJECTION) {
		BUG_ON_SAVE_LAST_PAGE(
		    p->start_dlba + page != wr->lba.dlba &&
		        p->start_dlba + page != wr->lba.jlba,
		    _core_dbg_di,
		    "POST-WRITE: addresses in WR section changed during write. It is "
		    "either a doomsday bug, or, more likelly, you are running FIO with "
		    "DBGDI on or mmapped a volume with DBGDI on.");
		p->magic_data->bb_image[page].lba.dlba = wr->lba.dlba;
		p->magic_data->bb_image[page].lba.jlba = wr->lba.jlba;
		/* If write ended successfully - we can be sure on remote BB state.
		   If not - it is not valid for ingerity cheks. */
		p->magic_data->bb_image[page].is_valid = !p->comp_code;
		{ /* Last, invalidate the ram to avoid it being written to random regions on disk */
			union t_core_magic_area_cell *stamp;
			wr->magic = NVMEIBC_CORE_DBG_DI_MAGIC_INV;
			for_each_stamp_in_magic_area(stamp, &wr->magic_area) {
				__INIT_MAGIC_AREA_CELL(stamp);
			}
		}
	}
}

/* before posting read-op to wire/ldisk */
static inline int
data_blk_fill_for_core_rd_pre(void *data, u64 block_dlba, u64 jrnl_lba, u8 op, u32 page,
                              struct t_core_dbgdi_params_pre *p) {
	struct t_core_dbgdi *c = data;
	struct t_core_dbgdi_wr *wr;

	p->start_dlba = nvmeibc_jam_decode_lba(p->start_dlba); /* safe side */

	(void)p; (void)op; (void)page; (void)jrnl_lba; (void)block_dlba; (void)jrnl_lba;

	/* Poison all writer areas with WR_INV, we expect it to be overwritten by read */
	for_each_core_writer_area(wr, c) {
		wr->magic = NVMEIBC_CORE_DBG_DI_MAGIC_INV;
	}

	c->rd.magic         = NVMEIBC_CORE_DBG_DI_MAGIC_POISON;
	c->rd.op            = 0xFF;
	c->rd.ch_type       = 0xFF;
	c->rd.comp_code     = 0xFF;
	c->rd.was_overeager = 0;

	return 0;
}

/* after completion of read-op from wire/ldisk */
static inline int
data_blk_fill_for_core_rd_post(void *data, u64 block_dlba, u64 jrnl_lba, u8 op, u32 page,
                               struct t_core_dbgdi_params_post *p) {
	struct t_core_dbgdi *c = data;
	struct t_core_dbgdi_wr *wr = __get_my_side_wr(p->disk_name, c, false);

	(void)block_dlba; (void)jrnl_lba;

	p->start_dlba = nvmeibc_jam_decode_lba(p->start_dlba); /* safe side */

	if (!p->comp_code) {
		BUG_ON_SAVE_LAST_PAGE(
		    c->rd.magic == NVMEIBC_CORE_DBG_DI_MAGIC_POISON, data,
		    "POST-READ: Have read poison from disk. Should never happen.");
		c->rd.magic = NVMEIBC_CORE_DBG_DI_MAGIC_RD;
	} else {
		c->rd.magic = NVMEIBC_CORE_DBG_DI_MAGIC_ERR;
	}
	c->rd.op            = op;
	c->rd.ch_type       = p->ch_type;
	c->rd.was_overeager = p->was_overeager;
	c->rd.comp_code     = p->comp_code;
	c->rd.container_ptr = (u64)dp_dbgdi_get_core_area_container(c);

	if (p->magic_data && page < NVMEIBC_MAX_CORE_DBGDI_IO_INJECTION) {
		bool hit = false; /* for statistics */
		if (!p->comp_code && wr) {
			/* We have read, successfully, something valid,
			   that was previously written */
			int problem_rv = 0;

			/* All the sanity checks are only relevant if read lba != magic lba
			 */
			if (p->magic_data->bb_image[page].lba.dlba != p->start_dlba + page &&
			    p->magic_data->bb_image[page].lba.jlba != p->start_dlba + page) {
				/* if curr and last IO coincide stamp should have been zeroed */
				if (p->magic_data->bb_image[page].is_valid) {
					union t_core_magic_area_cell *stamp,
					    *magic_stamp = &p->magic_data->bb_image[page].stamp;
					hit = true;
					/* First, check if we have read something we are never
					   supposed to read. */
					for_each_stamp_in_magic_area(stamp, &wr->magic_area) {
						if (nvmeibc_magic_stamp_eq(stamp, magic_stamp)) {
							problem_rv = (1<<0);
							break;
						}
					}
				}

				if (!problem_rv) {
					/* If all OK, check also the magic area is consistent, i.e
					   contains only one value. */
					union t_core_magic_area_cell *stamp, *prev_stamp = NULL;
					for_each_stamp_in_magic_area(stamp, &wr->magic_area) {
						if (prev_stamp &&
						    nvmeibc_magic_stamp_eq(stamp, prev_stamp)) {
							problem_rv = (1<<1);
							break;
						}
					}
				}
			}

			if (!problem_rv && (wr->lba.dlba != p->start_dlba + page) &&
			    (wr->lba.jlba != p->start_dlba + page)) {
				problem_rv = (1<<2);
			}

			if (problem_rv) {
				/* So we have a problem. */
				if (__dp_dbgdi_save_last_read_page(data))
					return problem_rv;
				/* Else don't care, we are going to crash in a moment */
			}

			/* Update the last dlba */
			p->magic_data->bb_image[page].lba.dlba = wr->lba.dlba;
			p->magic_data->bb_image[page].lba.jlba = wr->lba.jlba;

			/* Update the new stamp */
			if (!__IS_MAGIC_AREA_CELL_ZERO(&wr->magic_area.a[0])) {
				p->magic_data->bb_image[page].stamp    = wr->magic_area.a[0];
				p->magic_data->bb_image[page].is_valid = true;
			} else {
				/* Not much to do with zero area */
				p->magic_data->bb_image[page].is_valid = false;
			}
		} else {
			/* Problem, don't know what is in BB - cell invalid to use */
			p->magic_data->bb_image[page].is_valid = false;
		}
		if (hit) p->stats.hits++;
		else p->stats.misses++;
	}
	return 0;
}

/* TBD:
 * Commit comp-code of write-ops too.
 * Wrapper to any injected data - Simple to add more
 * Add injected info:
   - nvme: op, lba, len
   - direct/pending
   - piggyback - local and read-op
   - rdma: #wrs (mapping result)
   - disk-ver number
 * Read poison - Apply to No-RDDA too.
 * Change dp_dbgdi_should_add_info_core and dp_dbgdi_should_rdr_info_core
   such that we inject for internal disk-writes like gen-cmds (jerase) and
   NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR
 * Enable adding post-rd info from each caller of nvmeibc_block_completion()
   that reached the disk (i.e. not from abort-pending etc.)
 * Do we need to invalidate writer's info if post-failed?
 * add NVMEIBC_CORE_DBG_DI_MAGIC_ERR to wr-post
 * NVMEIBC_MAX_CORE_DBGDI_IO_INJECTION - derive this from BB size... or ndb's or disk->max_rdda_bb
**/
#endif	// DBGDI_REMOVED_IN_PRODUCTION

