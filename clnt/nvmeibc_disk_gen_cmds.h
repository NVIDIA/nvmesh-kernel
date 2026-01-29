/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DISK_GEN_CMD_HANDLERS_H
#define NVMEIBC_DISK_GEN_CMD_HANDLERS_H

#include "nvmeibc_disk.h"

struct free_jrnl_ents_info {
	struct nvmeibc_disk_gen_cmd *gen_cmd;
	struct nvmeibc_disk_free_jrnl_ents_comp *comp;
	struct nvmeibc_disk *disk;
	unsigned start_ent;
	unsigned total_num_ents;
	unsigned max_ents;
};

struct get_dirty_bits_ec_info {
	struct nvmeibc_disk_gen_cmd gen_cmd;
	u32 seg_id;
	u64 start_addr;
	u64 len;
	struct nvmeibc_d_rdma_comp *comp;
	struct nvmeibc_disk *disk;
	struct list_head link;
	int rv;
};

void nvmeibc_disk_gen_cmd_completion(struct nvmeibc_disk_gen_cmd *gen_cmd, int comp_code);
void nvmeibc_disk_gen_cmd_completion__(struct nvmeibc_disk_gen_cmd *gen_cmd);
int nvmeibc_disk_fill_free_jrnl_ents_info(struct nvmeibc_disk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp,
                                           struct free_jrnl_ents_info *info);
int nvmeibc_disk_fill_gen_op_get_jmdc(struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp *comp,
                                       struct nvmeibc_disk_gen_cmd *gen_cmd);
void nvmeibc_get_jmdc_deserialize_rng_data(void *rng_data, size_t rng_data_sz,
	    struct nvmeibc_disk_jmdc_read_comp *comp,
	    struct nvmeibc_ib_admin_channel *ch);

#endif /* NVMEIBC_DISK_GEN_CMD_HANDLERS_H */
