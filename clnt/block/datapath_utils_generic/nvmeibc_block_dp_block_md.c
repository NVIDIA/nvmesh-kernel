/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "common/kr_incs.h"
#if defined(UM_APP)
	#include "nvmesh/nvmeib_common_includes.h"
	#include "block/nvmeibc_topology.h"
	#include "nvmeibc_block_dp_io_generic_cmds.h"
	#include "nvmeibc_block_dp_operation.h"
	#include "nvmeibc_block_dp_common.h"
#endif
#include "block/nvmeibc_block_common.h"
#include "nvmeibc_block_dp_block_md.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec.h"
#include "nvmeibc_memmgr_metrics.h"

#ifndef UM_APP
bool nvmeibc_warn_on_edic_verification_failure = true;				// Default true
module_param(nvmeibc_warn_on_edic_verification_failure, bool, 0644);
MODULE_PARM_DESC(nvmeibc_warn_on_edic_verification_failure, "Issue kernel warning upon CRC-based read block verification failure. Useful for detecting data that has been correct on drives.");

uint nvmeibc_jmd_wr_version = NVMEIBC_JOURNAL_MD_VERSION_PACKED;				// Default, backwards compatibale, Todo: Consider using mgmt based trigger, not module param
module_param(nvmeibc_jmd_wr_version, uint, 0644);
MODULE_PARM_DESC(nvmeibc_jmd_wr_version, "Version of JMD (journal metadata) to use to facilitate backwards compatibility: 0 = packed, 1 = unpacked.");	// Dont touch this! This is part of EC version, in future, make this module param read only! it was needed to upgrade versions.

#else // UM_APP
	#include "module_params.h"
	bool nvmeibc_warn_on_edic_verification_failure = true;				// Default true
	MPARAM(bool, nvmeibc_warn_on_edic_verification_failure, "nvmeibc_warn_on_edic_verification_failure");

	uint32_t nvmeibc_jmd_wr_version = NVMEIBC_JOURNAL_MD_VERSION_PACKED;
	MPARAM(uint32_t, nvmeibc_jmd_wr_version, "Version of jmd to write. 0-unpacked, 1-packed");
#endif


extern void dp_dbgdi_mark_edic(void *data, enum edic_result pass, u32 read_edic, u32 calc_edic, u64 rlba);
extern u32 nvmeibc_calculate_edic_from_data_and_rlba(const u64 rlba, const unsigned char *data, const bool debug_di_enabled);

NVMEIBC_MEMMGR_METRIC(dp_data_metadata, "component=raid.io.data_metadata");

// Can return resume -> will check EDIC for this block, continue -> Skip this block mark unchecked
// bad_sector -> return EPERM_READ_FAIL_NO_RETRY or DATA VIRGIN -> mark our type of neverwritten in the MD
static enum metadata_action nvmeibc_data_written_state_to_action(enum nvmeibc_data_written_state state)
{
	enum metadata_action rv = RESUME;
	if (     state == DATA_VIRGIN)                         rv = MARK_NEVER_WRITTEN;
	else if (state == DATA_EXPLICITLY_MARKED_INVALID)      rv = BAD_SECTOR;
	else if (state == DATA_EXPLICITLY_MARKED_NEVERWRITTEN) rv = SKIP_EDIC_CHECK;
	return rv;
}


/* Analyze special markers in metadata that mark invalidity of data and, calculate EDIC from block data. Note: Take care of multi-block commands */
int nvmeibc_check_metadata_read_cmd(struct nvmeibc_block_command *cmd, u64 rlba, const u32 mask, void *md, const u32 md_size, const int slice_size, const int snake_size, const int prev_rv)
{
	int i, j, rv = prev_rv;
	const struct nvmeibc_datapath *dp = &cmd->o->nd->dp;
	const bool is_parity = cmd->is_parity;
	const bool enable_edic_check = dp->enable_edic_check;
	const bool enable_di_debug_mode = dp->enable_di_debug_mode;
	const struct nvmeib_data_buffer *ndb = cmd->iocmd->reqs1.ndb;
	struct scatterlist *curSG = NULL;
	const int nSGelements = ndb->table.nents;
	struct dp_io_stats *dp_io_stats = &cmd->o->nd->dp.io_stats;
	for_each_sg(ndb->table.sgl, curSG, nSGelements, i) { //
		const int length  = curSG->length;
		for (j = 0; j < length; j += NVMEIBC_SECTOR_SIZE,		// Advance to next slice
					md = (void*)((u8*)md + md_size),
					advance_rlba_to_next_slice(rlba, snake_size, slice_size)) {
			u8 *blk_data = &((u8*)sg_virt(curSG))[j];
			/* Special meta state of data */
			enum metadata_action act = nvmeibc_data_written_state_to_action(nbdpec_md_get_data_written_state(md));
			switch (act) {
				case BAD_SECTOR:
					IO_STATS_INCR(dp_io_stats, DP_IO_STATS_MD_MARKED_INVALID_ERRORS);
					return EPERM_READ_FAIL_NO_RETRY;
				case MARK_NEVER_WRITTEN:
					nbdpec_md_mark_data_never_written_no_dbits(md, cmd->is_parity);
					FALLTHRU;
				case SKIP_EDIC_CHECK:
					if (unlikely(enable_di_debug_mode))
						dp_dbgdi_mark_edic(blk_data, EDIC_NOT_CHECKED, 0, -1, rlba);
					continue;
			default: break;
			}

			if (enable_edic_check) { // If we want to check EDIC on each read
				const u32 read_edic = nvmeibc_block_dp_ec_md_get_edic(md, is_parity);
				const u32 exp_edic  = nvmeibc_calculate_edic_from_data_and_rlba(rlba, blk_data, dp->enable_di_debug_mode) & mask;
				if (read_edic != exp_edic) {
					if (unlikely(enable_di_debug_mode)) {
						dp_dbgdi_mark_edic(blk_data, EDIC_FAIL, read_edic, exp_edic, rlba);
					}
					WARN(nvmeibc_warn_on_edic_verification_failure, "EC-7676 - %s: op=%u, Disk %s, Seg %x, stg=%d edic fail: rlba:%llu, P=%d, read_edic=0x%08x, calc_edic=0x%08x, slice_ofst=%u, blk_data=0x%016llx\n", cmd->o->nd->name, cmd->o->op, nvmeibc_disk_get_name(cmd->ds->disk), cmd->ds->dbg_uuid, cmd->my_stage, rlba, cmd->is_parity, read_edic, exp_edic, (j / NVMEIBC_SECTOR_SIZE), *(u64*)blk_data);
					IO_STATS_INCR(dp_io_stats, DP_IO_STATS_MD_EDIC_CHECK_ERRORS);
					return EPERM_READ_FAIL;
				} else if (unlikely(enable_di_debug_mode)) {
					dp_dbgdi_mark_edic(blk_data, EDIC_PASS, read_edic, exp_edic, rlba);
				}
			} else if (unlikely(enable_di_debug_mode)) {
				dp_dbgdi_mark_edic(blk_data, EDIC_NOT_CHECKED, 0, 0, rlba);
			}
		}
	}
	if (!rv) cmd->is_valid_for_reuse = true;
	return rv;
}

static int __check_metadata_cmd_generic(struct nvmeibc_block_command *cmd, int prev_rv)
{
	struct nvmeibc_block_io_req *req = cmd->iocmd->reqs;
	if ((!req) || 		// Gen commands (not IO)
		(req->op != NVMEIB_BLOCK_IO_OP_READ) ||
		(prev_rv != 0) || // Some error already exists do not check CRC
		(cmd->my_stage == E_CMDS_STAGE_READ_JOURNAL) ||
		(cmd->do_not_send) ||	// Journal has no edic, nor meta state in metadata
		(req->do_512b_sub_block_x)) {			// Read of partial block cannot verify edic
	} else {
		const struct multi_snake_slice_analyzer *mssa = cmd->cmdarr[cmd->my_leader].o->mssa;
		const bool is_raid1_mirror = (mssa == NULL);
		const u32 mask = NVMEIBC_DP_EC_MD_EDIC_MASK(cmd->is_parity);
		const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
		u64 rlba = cmd->first_rlba;
		const int slice_size = (!is_raid1_mirror) ? mssa->slice_size : 1;
		const int snake_size = (!is_raid1_mirror) ? mssa->snake_size : 1;
		prev_rv = nvmeibc_check_metadata_read_cmd(cmd, rlba, mask, req->md, md_size, slice_size, snake_size, prev_rv);
	}
	return prev_rv;
}

void nvmeibc_block_dp_ec_jmd_encode(union jblock_md *md, u64 j2d, u32 tx_id, roles_bmp_t tx_bmp, bool has_next, bool has_prev)
{
	if (nvmeibc_jmd_wr_version) {
		md->j2d_0 = (u32)j2d;
		md->tx_id = tx_id;
		md->tx_bmp = nvmeibc_block_dp_ec_md_txbm_compress(tx_bmp);
		md->v1.has_next = has_next;
		(void)has_prev; //md->v1.has_prev = has_prev;
		md->v1.j2d_extended = (j2d >> 32);
		md->version = NVMEIBC_JOURNAL_MD_VERSION_PACKED;
	} else {
		jblock_md_jmd_encode_v0(md, (u32)j2d, tx_id, tx_bmp);
	}
}

void nvmeibc_block_dp_ec_md_decode_jentry(u64 seg_start_dlba, struct jent_md_decompressed *res, const struct jentry_md *ent) {
	const union jblock_md *src = ent->md_arr;
	struct jblock_md_decompressed_for_seg *dst = res->md_arr;
	bool has_next;
	int len = 0;
	BUILD_BUG_ON(sizeof(struct jblock_md_decompressed) != sizeof(struct jblock_md_decompressed_for_seg));
	for (has_next = true; has_next; src++, dst++, len++ ) {
		*((struct jblock_md_decompressed*)((void*)dst)) = nvmeibc_block_dp_ec_jmd_decode(src);
		dst->j2slba -= seg_start_dlba;	// Convert to slice in segment, thus ignorring different offset of each segment on disk
		has_next = dst->has_next;
	}
	res->len = len;
}


void nvmeibc_fill_metadata_from_command(struct nvmeibc_block_command *cmd, const struct nvmeibc_block_command *precalculated_cmd)
{	// Just copy metadata from one command to another, Keep in mind that md_size may be different
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	const u32 pmd_size = nvmeibc_sgmnt_sw_md_size(precalculated_cmd->ds);
	u64 block;
	for (block=0; block < cmd->nlbas; block++) {
		u64 *raw =  (void*)((u8*)              cmd->iocmd->reqs1.md + md_size *block);
		u64 *praw = (void*)((u8*)precalculated_cmd->iocmd->reqs1.md + pmd_size*block);
		*raw = *praw;
	}
}

bool nvmeibc_are_metadatas_identical(const struct nvmeibc_block_command *c0, const struct nvmeibc_block_command *c1)
{
	const u32 c0_md_size = nvmeibc_sgmnt_sw_md_size(c0->ds), c1_md_size = nvmeibc_sgmnt_sw_md_size(c1->ds);
	if ((nvmeibc_is_mirror_md_enabled(c0)) && (nvmeibc_is_mirror_md_enabled(c1))) {
		u64 i;
		for (i=0; i < c0->nlbas; i++) {
			const u64 *c0_raw = (void*)((u8*)c0->iocmd->reqs1.md + c0_md_size*i);
			const u64 *c1_raw = (void*)((u8*)c1->iocmd->reqs1.md + c1_md_size*i);
			if (*c0_raw != *c1_raw)
				return false;
		}
	}
	return true;	// Daniel, Todo, maybe analyze case where 1 cmd has metadata and other not
}

void nvmeibc_check_metadata_actions(struct nvmeibc_d_iocmd_comp *comp)
{
	comp->comp_code = __check_metadata_cmd_generic(dp_cmds_get_cmd_from_comp(comp), comp->comp_code);
}

void *nvmeibc_alloc_md(const u64 nlbas, const u32 mdsize) {
	void *md = NULL;
	bool page_crossed = false;
	const u64 totalsize = nlbas * mdsize;
	const gfp_t gfp = nvmeibc_dp_get_allow_io_gfp_flags();

	/* First try allocating with kzalloc */
	if (!(md = kzalloc(totalsize, gfp))) {
		goto out;
	}
	BUG_ON(!PageSlab(virt_to_head_page(md)));

	/* Check alignment - TBD: Check is local bypass */
	if (((u64)md & PAGE_MASK) != (((u64)md + (totalsize - 1)) & PAGE_MASK)) {
		/* Crosses a page - Free and allocate a whole page */
		page_crossed = true;
		kfree(md);
		md = NULL;

		if (totalsize > PAGE_SIZE) {	// Daniel is this covered by a BUILD_BUG_ON?
			WARN(true, "nvmeibc bug: nlbas=0x%llx, mdsize=0x%x", nlbas, mdsize);					/* Why this should not happen? On trim operations we do not allocate metadata. On other opertions, we nlbas is limited by blockset size (32). In addition, mdsize is deduced by block size on server vs client. At worst case, it is 4K on client 512b on server, resulting in mdsize = 8*(4096/512) = 64.Hence, we are limited by total mdsize*nlbas = 2048 < 4096. */
			goto out;
		}
		md = (void *)__get_free_page(gfp);
		BUG_ON(PageSlab(virt_to_head_page(md)));
		memset(md, 0, totalsize);
	}

	nvmesh_memmgr_metric_on_alloc_update(dp_data_metadata, page_crossed? PAGE_SIZE: ksize(md), md);

out:
	return md;
}

void nvmeibc_free_md(void *md) {
	if (md) {
		struct page *pg_head = virt_to_head_page(md);
		u32 alloc_size;

		if (unlikely(!PageSlab(pg_head))) {
			alloc_size = PAGE_SIZE;
			free_page((unsigned long)md);
		} else {
			alloc_size = ksize(md);
			kfree(md);
		}

		nvmesh_memmgr_metric_on_free_update(dp_data_metadata, alloc_size);
	}
}

