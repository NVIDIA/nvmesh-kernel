/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "nvmeib_nvme.h"
#include "nvmeibc_block_dp_mirror.h"
#include "../nvmeibc_block_common.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.h"

/**
 * For TRIM, we don't traverse the bio contents. The sg buffer and length
 * represent the NVMe DSM command address and bytes, respectively.
 */
int nvmeib_make_discard_ndb(struct nvmeibc_block_command *cmd)
{
	const struct nvmeibc_disk   *d =  cmd->ds->disk;
	struct nvmeibc_block_io_req *ir = &cmd->iocmd->reqs1;
	struct nvmeib_dsm_range     *r =  ir->trim = my_kmalloc(sizeof(*r), GFP_ATOMIC);
	int rv = 0;

	if ((!nvmeib_get_ndb(cmd, 1, GFP_ATOMIC)) || (!r)) {
		_NE(t_01_r1trim, DMESG_PREFIX() ": No memory for cmd=@CMD_PTR, buf=@BUF, ndb=@PTR", cmd, r, ir->ndb);
		rv = -ENOMEM;
		goto out;
	}
	ir->ndb->length = sizeof(*r);
	r->cattr = cpu_to_le32(0); /* Daniel: NVME rfc forces little endian! */
	r->nlb =   cpu_to_le32(cmd->nlbas       << __blk_to_disk_sect_shift(d));
	r->slba =  cpu_to_le64(ir->disk_address << __blk_to_disk_sect_shift(d));
	sg_set_buf(ir->ndb->table.sgl, r, sizeof(*r));
	ir->ndb->table.nents = 1;
out:
	return rv;
}

__attribute__((nonnull(1)))
struct nvmeib_dsm_range nvmeib_get_ndb_discard_range(struct nvmeibc_block_command const* cmd, enum nvmeib_dsm_range_encoding encoding)
{
	struct nvmeibc_block_io_req *ir = &cmd->iocmd->reqs1;
	if (encoding == NVMEIB_DSM_RANGE_ENCODING_LITTLE_ENDIAN) {
		return *ir->trim;
	}
	return (struct nvmeib_dsm_range) {
		.cattr = le32_to_cpu(ir->trim->cattr),
		.nlb = le32_to_cpu(ir->trim->nlb),
		.slba = le64_to_cpu(ir->trim->slba),
	};
}

#define _NDtbuf(name, buf, format, ...) _ND(name, "buffer=(@BUF_CATTR @BUF_SLBA @BUF_NLB), " format, le32_to_cpu((buf)->cattr), le32_to_cpu((buf)->nlb), le64_to_cpu((buf)->slba), ##__VA_ARGS__)
int __concat_discard_op(struct nvmeibc_block_command cmds[], int *pncmds, int nlbas, struct nvmeibc_block_device *nd, bool can_merge_diff_segs)
{
	int n = (*pncmds - 1), rv = 0;
	struct nvmeibc_block_command *cur_c = &cmds[n];
	const struct nvmeibc_disk_segment * const ds_um = cur_c->ds;
	struct nvmeibc_block_io_req *io_req = &cur_c->iocmd->reqs1;
	struct nvmeibc_disk *disk_um = ds_um->disk;
	const u32 shift = __blk_to_disk_sect_shift(disk_um);

	// Check if there is a previous command to merge with
	for (n--; n >= 0; n--) {
		if (cmds[n].ds->disk == disk_um) {				// Compare same disk, not same segment.
			struct nvmeib_dsm_range *range = cmds[n].iocmd->reqs1.trim;
			const u64 last_slba = le64_to_cpu(range->slba) >> shift;
			const u32 len =       le32_to_cpu(range->nlb ) >> shift;
			WARN_ON(len != cmds[n].nlbas);
			if ((last_slba + len == io_req->disk_address) &&
				((cmds[n].ds == ds_um)||(can_merge_diff_segs))) {
				(*pncmds)--;
				range->nlb = cpu_to_le32((len + nlbas) << shift);
				cmds[n].nlbas = len + nlbas;
				_ND(t_02_r1trim, "@DEV_NAME: Concatenated cmds @COMMAND_IDX<-@NCMDS, lenth=@NLBA->@NLBA", nd->name, n, *pncmds + 1, len, len + nlbas);
				goto _out;
			}
			break;
		}
	}

	if ((rv = nvmeib_make_discard_ndb(cur_c)) < 0)
		goto _out;
	_ND(t_03_r1trim, "(@DISK_NAME) shift=@SHIFT len=@NLBA", disk_um->name, nvmeibc_disk_get_sector_shift(disk_um), io_req->ndb->length);
	_NDtbuf(t_04_r1trim, io_req->trim, "");
_out:
	return rv;
}

static int __copy_blk_cmd(struct nvmeibc_block_command *cmds, int ci,
		struct nvmeibc_block_command *new_cmds, int nci, u64 cmd_s, u64 cmd_e)
{
	struct nvmeibc_block_command *dst = &new_cmds[nci], *src = &cmds[ci];
	struct nvmeibc_disk_io_command *iocmd = dst->iocmd;	// Transport layer command
	struct nvmeib_data_buffer *ndb = dst->iocmd->reqs1.ndb;
	struct nvmeibc_block_io_req *dst_req = &(iocmd->reqs1);
	const struct nvmeibc_block_io_req *src_req = &(src->iocmd->reqs1);
	const size_t dst_mem_allocated_size = dst->mem_allocated_size;
	int rv = 0;

	NFIN;
	WARN((!src_req->trim) || src->gen_cmd, "nvmeibc bug: %p\n", src->gen_cmd);
	/* Deep Copy down-top*/
	memcpy(dst->iocmd, src->iocmd, sizeof(*dst->iocmd));
	memcpy(dst,        src,        sizeof(*dst));
	dst->mem_allocated_size = dst_mem_allocated_size; // restore allocated size; needed for correct memmgr metric accounting
	/* restore pointers */
	dst->cmdarr = new_cmds;			// Restore dst, A bit like what dp_cmds_req_fill() does
	dst->ncmds = 0;  /* Valid only for nci==0. Set it in the caller. */
	dst->iocmd = iocmd;				// restore dst->iocmd
	dst->iocmd->reqs = &dst->iocmd->reqs1;
	dst->iocmd->comp.cmd = dst;
	iocmd->disk_cmd.owner = dst;
	dst->iocmd->reqs1.ndb = ndb;	// restore ndb
	if (dp_cmds_pigbck_has_any(src->iocmd))
		dp_cmds_add_generic_piggyback(dst->iocmd);
	dst->iocmd->req_id = dp_cmds_req_alloc_unique_id();
	dst_req->disk_address = cmd_s;
#ifdef DEBUG_UNCOMPLETED
	dst_req->nlbas = cmd_e - cmd_s;
#endif
	dst->nlbas = cmd_e - cmd_s;
	if ((rv = nvmeib_make_discard_ndb(&new_cmds[nci])) < 0)
		goto out;
	_ND(t_05_r1trim, "new_cmds=@NEW_CMDS[@NCI] req_id=@REQ_ID_LLONG", new_cmds, nci, dst->iocmd->req_id);
	_ND(t_06_r1trim, "cmds[@CI]=[@DISK_ADDRESS-@NLBAS) new_cmds[@NCI]=[@DISK_ADDRESS-@NLBAS)",
		ci , src_req->disk_address, src_req->disk_address + src->nlbas,
		nci, dst_req->disk_address, dst_req->disk_address + dst->nlbas);
out:
	NFOUT;
	return rv;
}

/* Split TRIM commands in such a way that memory on which we could not acquire
   lock, is seprated into a different command. 'ls' is locksets */
static int __prediscard_split(struct nvmeibc_cmd_lock *ls)
{
	struct nvmeibc_block_command *cmds = ls->cmds, *new_cmds = ls->new_cmds;
	u64 ls_start, ls_end, trans_offset;
	int lsi, ci, nci, rv = 0;

	NFIN;
	_ND(t_01_r1pds, "starting: bio=@BIO, ncmds=@NCMDS, ls=@LS cmds=@CMDS, new_cmds=@NEW_CMDS", ls->cmds->o->bios[0], cmds->ncmds, ls, cmds, new_cmds);

	/* For CONTENDED locks, split the commands. Assuming
	   1. lock protects 1 or 2 non mirrored commands only and the locks
	   2. commands are ordered by disk address for each segment.
	   IMPORTANT: must convert the addresses by offset of the commands */
	for (ci = 0, nci = 0; ci < cmds->ncmds; ci++) {
		const u64 cmd_offset = __offset_from_seg(cmds[ci]);
			  u64 cmd_start = __cmd_start(cmds[ci]);
		const u64 cmd_end =   __cmd_end(  cmds[ci]);
		_ND(t_02_r1pds, "cmds[@CI].pending=@PENDING_INT", ci, nvmeibc_atomic_read(&cmds[ci].nlocks));

		for_each_primary_owner(lsi, ls) {
			if ((ls[lsi].status != NCL_STATUS_CONTENDED) || (!nvmeibc_clmat_is_linked(cmds->o->CLmat, ci, lsi, ls))) {
				continue; /* Locks will split different cmd. Not ci */
			} // else, ls[lsi] is a contended owner which invokes cmds split

			trans_offset = cmd_offset - __offset_from_seg(ls[lsi]);
			ls_start = __lock_start(ls[lsi]) + trans_offset;
			ls_end   = __lock_end(  ls[lsi]) + trans_offset;

			if (cmd_start < ls_start) {
				/* create prefix cmd, trunc the range at the contended lock. */
				rv = __copy_blk_cmd(cmds, ci, new_cmds, nci, cmd_start, ls_start);
				nci++;
				if (rv < 0)
					goto out;
			}

			{	/* create command for intersection with the failed lock */
				const u64 int_s = max(cmd_start	, ls_start);
				const u64 int_e = min(cmd_end	, ls_end);
				rv = __copy_blk_cmd(cmds, ci, new_cmds, nci, int_s, int_e);
				nci++;
				if (rv < 0)
					goto out;
				cmd_start = int_e;
			}
			if (cmd_start==cmd_end)
				break;	/* Done splitting cmds[ci]. Other locks are irrelevant*/
		} /* for (lsi < locksets[0].nlocks) */

		if (cmd_start < cmd_end) {
			/* create suffix cmd if needed for any tail-end leftover. */
			rv = __copy_blk_cmd(cmds, ci, new_cmds, nci, cmd_start, cmd_end);
			nci++;
			if (rv < 0)
				goto out;
		}
	} /* for (ci < cmds->ncmds) */

	WARN_ON_ONCE(nci < cmds->ncmds);
	nvmeibc_atomic_set(&new_cmds[0].n_uncompleted_cmds, nci);

out:
	new_cmds[0].ncmds = nci; // Needed in any case, for dp_cmds_free_all().
	_ND(t_03_r1pds, "done: bio=@BIO, ncmds=@NCMDS, locksets=@LOCKSETS cmds=@CMDS new_cmds=@NEW_CMDS ncmds=@NCMDS, rv=@RV",
		 ls->cmds->o->bios[0], cmds->ncmds, ls, ls->cmds, new_cmds, new_cmds[0].ncmds, rv);
	NFOUT;
	return rv;
}

static inline bool __is_trim_cmds_split_needed(struct nvmeibc_cmd_lock *ls)
{
	int i;
	for_each_primary_owner(i, ls) {
		if (NCL_is_failed_to_acquire(ls[i].status)) {
			_ND(t_10_r1trim, "@LOCKSETS[@LSI] dead", ls, i);
			return false;	// dead means no-split, just retry in different topo
		}
	}
	for_each_primary_owner(i, ls) {
		if (ls[i].status == NCL_STATUS_CONTENDED) {
			_ND(t_11_r1trim, "@LOCKSETS[@LSI] contended", ls, i);
			return true;	// Contended means split
		}
	}
	return false;			// No problems
}

int nvmeibc_do_trim_split_if_needed(struct nvmeibc_cmd_lock *ls)
{
	bool problem = __is_trim_cmds_split_needed(ls);
	if (problem) {
		_ND(t_12_r1trim, "ls=@LS cmds=@CMDS uncompleted=@UNCOMPLETED/@NCMDS new_cmds=@NEW_CMDS", ls, ls->cmds, nvmeibc_atomic_read(&ls->cmds->n_uncompleted_cmds), ls->cmds->ncmds, ls->new_cmds);
		return __prediscard_split(ls); /* <0 on split fail, 0 on split OK */
	}
	return 1; /* split not needed */
}
