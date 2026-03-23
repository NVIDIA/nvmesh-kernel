/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBR_DISK_METADATA_H
#define NVMEIBR_DISK_METADATA_H
/* Simulator of toma/nvmeibt_disk_metadata.h, Manages GPT header+entries
   for ssd disk. GPT is used by serjio, and is part of API between
   Toma-to-Serjio which describes configuration of segments and persistency */

#include "nvmeib_common_all.h"
#include "nvmeibs_serjio_gpt.h"

/******************************************************************************/
struct serverSimulator;

/* Generate gpt header for disk on a given server (according to mgmt configuration) */
void nvmeibr_disk_metadata_store_gpt(struct serverSimulator *srv, struct gpt_header *gpt, bool primary);

/* Total GPT partition entry slots for this disk (fixed layout used by store_gpt / serjio). */
unsigned nvmeibr_disk_metadata_num_gpt_entries(struct serverSimulator *srv);

/* Generate GPT entries [start_ent, start_ent + n_ents) into @ent when non-NULL (packed). @max_ent
 * is the full table size on disk; indices must stay within [0, max_ent). At least one of @ent or
 * @ent_crc must be non-NULL; if @ent is set, n_ents must be non-zero. When @ent_crc is non-NULL,
 * if start_ent == 0 then *ent_crc is reset (~0L) before accumulating; otherwise the existing
 * *ent_crc is continued (CRC is taken over the encoded entry bytes, same as written to @ent when
 * both are used). After the slice that ends the table (start_ent + n_ents == max_ent), *ent_crc
 * is finalized (^ ~0L) for the GPT header. */
void nvmeibr_disk_metadata_store_entries(struct serverSimulator *srv, struct gpt_entry *ent,
					 unsigned start_ent, unsigned n_ents, unsigned max_ent, u32 *ent_crc);

#endif // NVMEIBR_DISK_METADATA_H


