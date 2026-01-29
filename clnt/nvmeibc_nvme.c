/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_nvme.h"
#include "nvmeibc_disk.h"
#include "nvmeib_utils.h"
#include "nvmeibc_trace.h"

int nvmeibc_fill_dsq(struct nvmeibc_disk_channel_rsc *info,
	u32 command_id, int entry, u64 disk_sector, int len_bytes, int *prpl_len)
{
	struct nvmeib_dsm_cmd *p =
		&((struct nvmeib_dsm_cmd*)info->sq_shadow)[entry];
	memset(p, 0, sizeof *p);
	p->opcode = nvme_cmd_dsm;
	p->command_id = command_id;
	p->nsid = info->info->nsid;
	p->nr = 0;
	p->attributes = cpu_to_le32(NVME_DSMGMT_AD);
	_ND(trace_nvme_nvmeibc_fill_dsq, "opcode=@OPCODE, cmdid=@CMDID, nsid=@NSID, nr=@NR, attributes=@ATTRIBUTES",
		p->opcode,
		p->command_id,
		p->nsid,
		p->nr,
		p->attributes);
	*prpl_len = 0;
	p->prp1 = info->bb_raddr_nvme[0];
	return 0;
}

int nvmeibc_fill_rwsq(u8 nvme_op, struct nvmeibc_disk_channel_rsc *info,
	u32 command_id, int entry, u64 disk_sector, int len_bytes, int *prpl_len,
	u8 do_512b_sub_block_x)
{
	struct nvme_rw_command *p =
		&((struct nvme_rw_command*)info->sq_shadow)[entry];
	int npages = DIV_ROUND_UP(len_bytes, info->info->cntr_page_size);
	int prp_per_page;

	memset(p, 0, sizeof *p);
	p->opcode = nvme_op;
	p->command_id = command_id;
	p->nsid = info->info->nsid;
	if (do_512b_sub_block_x) {
		p->slba = nvmeib_translate_sw_addr_to_subblock_addr(
		    disk_sector, NVMEIBC_SECTOR_SHIFT, info->info->sector_shift,
		    do_512b_sub_block_x_val(do_512b_sub_block_x));
	} else {
		p->slba = disk_sector << (NVMEIBC_SECTOR_SHIFT-info->info->sector_shift);
	}

	p->length = (len_bytes >> info->info->sector_shift) - 1;

	_ND(trace_nvme_nvmeibc_fill_rwsq, "cmd=@CMD_PTR, opcode=@OPCODE, cmdid=@CMDID, nsid=@NSID, slba=@SLBA_LLONG, "
	   "length=@LENGTH_INT, nsid=@NSID, len_bytes=@LEN",
		NULL, /* RDDA removed */
		p->opcode,
		p->command_id,
		p->nsid,
		p->slba,
		p->length,
		info->info->nsid,
		len_bytes);
	*prpl_len = 0;

	if (nvme_op == nvme_cmd_write_uncor)
		return 0;	/* No data */

	if (info->info->disk->md_size) {
		if (info->info->disk->md_extd)
			++npages;
		else
			p->metadata = info->bb_raddr_nvme[2];
	}

	p->dptr_prp1 = info->bb_raddr_nvme[0];
	if (npages == 1)
		return 0;
	if (info->bb_raddr_nvme[0] & (info->info->cntr_page_size-1))
		return -EINVAL;
	if (npages == 2) {
		if (info->bb_raddr_nvme[1] & (info->info->cntr_page_size-1))
			return -EINVAL;
		p->dptr_prp2 = info->bb_raddr_nvme[1];
		return 0;
	}

	prp_per_page = info->info->cntr_page_size / sizeof(u64);
	if (npages <= prp_per_page) {
		if (info->prp1_raddr & (info->info->cntr_page_size-1))
			return -EINVAL;
		p->dptr_prp2 = info->prp1_raddr;
		return 0;
	}

	/* we never send prp list because  we always use the the disk
	   (a small) bounce buffer and thus the pages are known in advance
	*/
	_NE(error_nvme_nvmeibc_fill_rwsq, "OOPS: should have never reached here");

	BUG_ON(true);
	return 0;
}

