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

/* Generate array of gpt entries for disk on a given server (according to mgmt configuration) */
void nvmeibr_disk_metadata_store_entries(struct serverSimulator *srv, struct gpt_entry *ent, unsigned max_ent, u32 *ent_crc);

#endif // NVMEIBR_DISK_METADATA_H


