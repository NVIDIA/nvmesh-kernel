/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_API_MD_CARRIER_H
#define NVMEIBC_BLOCK_API_MD_CARRIER_H

// BIO to Read/Write 1 metadata block. Todo: In future extend to sub block and cow functionalities
struct md_carrier_base_block_io {	// Todo, move from here and use in mtv as well
	struct bio bio;					// Read/Write MD block
	struct bio_extention bext;
	struct bio_vec table[1];		// Your actually data is in table[0].
	struct page *page;
};

/* rw = {READ or WRITE} */
int   md_carrier_base_block_io_create(  struct md_carrier_base_block_io *md, const u64 vlba, int rw);
void  md_carrier_base_block_io_reinit(  struct md_carrier_base_block_io *md,                 int rw);			// Used to alter read/write. Usefull for reading md, changing and writing back
void  md_carrier_base_block_io_set_cb(  struct md_carrier_base_block_io *md, rider_bio_cb_t fn, void *ctx);
void  md_carrier_base_block_io_destroy( struct md_carrier_base_block_io *md);
void  md_carrier_base_block_io_execute( struct md_carrier_base_block_io *md, struct nvmeibc_block_device *dev);		// Send to carrier
void* md_carrier_base_block_io_get_data(struct md_carrier_base_block_io *md);	// For simplicity, direct access to the block

#endif  // H beginning
