/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_dbgdi.h"
#ifdef DBGDI_REMOVED_IN_PRODUCTION
	int dp_dbgdi_get_sizeof_injected_data(void){ return 0; }
	void dp_dbgdi_mark_edic(void *data, enum edic_result pass, u32 read_edic, u32 calc_edic, u64 rlba) { (void)data; (void)pass;(void)read_edic;(void)calc_edic;(void)rlba; }
#else
#include "nvmeibc_block_dp_dbgdi_blk.h"
#include "nvmeibc_block_dp_dbgdi_print.h"
#include "../nvmeibc_block_common.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "../recovery/nvmeibc_block_dp_sync_common.h"
#include "common/nvmeib_str.h"

#ifndef UM_APP
	/* Daniel: do_gettimeofday, is costly to do in datapath, I implemented cached
	version which reads it once and updates using jiffies diff, This gives
	at most the accuracy of jiffies */
	static struct do_gettimeofday_cahced_t {
		struct timeval tv;
		ulong  correspond_jiffies;	// Jiffies which corresponf to the 'time' above
	} time_cahced = {{0, 0}, 0};

	static inline u64 do_gettimeofday_cahced_secs(void)
	{
		struct timeval time;
		if (unlikely(time_cahced.correspond_jiffies == 0)) {// Initialize first time
			do_gettimeofday(&time_cahced.tv);
			time_cahced.correspond_jiffies = jiffies;
		}
		time.tv_sec = time_cahced.tv.tv_sec + (jiffies - time_cahced.correspond_jiffies)/HZ;
		time.tv_usec = time_cahced.tv.tv_usec; // Todo: update it with jiffies
		return (u64)(time.tv_sec - (sys_tz.tz_minuteswest * 60));
	}
#else
	static inline u64 do_gettimeofday_cahced_secs(void) { return (spdk_get_ticks() + nvmeib_get_tsc_offset()) / spdk_get_ticks_hz(); }
#endif

/******************************************************************************/
static void t_db_who_clnt_fill(struct t_db_who_clnt *s)
{
	nvmeib_strlcpy(s->name, nvmeib_get_utsname_nodename(), sizeof(s->name));
	scnprintf(s->version, sizeof(s->version), "%llx", (u64)COMMIT_ID);
	#if 0
	if (in_interrupt()) {
		s->pgid = (u32)task_tgid_nr(current); // Instead of (u32)current->pid;
		nvmeib_strlcpy(s->proc_name, current->comm, sizeof(s->proc_name));
	} else {
		s->pgid = 0;
		s->proc_name[0] = 0;
	}
	#endif
	if (1) { /* Inject time, costly operation */
		s->time_secs = do_gettimeofday_cahced_secs();
	}
}

static void t_db_who_bio_fill(struct t_db_who_bio *s,
							  const struct operation *o)
{
	nvmeib_strlcpy(s->volname, o->nd->name, sizeof(s->volname));
	s->start_vlba = (u32)get_op_start_lba(o);
	s->nlbas =	    (u32)get_op_nlbas(	 o);
	s->op =		    (u8)o->op;
}

static void t_db_who_locks_fill(struct t_db_who_locks *s, const struct nvmeibc_disk_segment *ds) {
	s->owner_si =    (u32)ds->lmap.si[  0];
	s->second_si =   (u32)ds->lmap.si[  1];
	s->third_si =    (u32)ds->lmap.si[  2];
	s->second_type = (u32)ds->lmap.type[1];
	s->third_type =  (u32)ds->lmap.type[2];
	s->reserved = 0;	// Init value
}

static void t_db_who_seg_fill(struct t_db_who_seg *s, const struct nvmeibc_disk_segment *ds)
{
	nvmeib_strlcpy((char*)&s->uuid[0], ds->uuid, sizeof(s->uuid));
	s->sync_safety = ds->sync_safety;
	s->toma_acm =	 ds->toma_acm;
	t_db_who_locks_fill(&s->locks, ds);
}

static void t_db_who_praid_fill(struct t_db_who_praid *s,
								const struct nvmeibc_raid1 *pr)
{
	s->version = 	pr->version;
	s->lock_id = 	pr->lid.all;
	s->n_segs = 	pr->replicas;
	s->slice_size = pr->slice_size;
}

static void t_db_who_topo_fill(struct t_db_who_topo *s,
								const struct nvmeibc_topology *t)
{
	s->my_index = 		 (u32)t->debug_unique_index;
	s->diff_head = 		 ((u32)t->nt->topo_debug_unique_index_generator - s->my_index);
	s->diff_last_freed = (s->my_index - (u32)t->nt->topo_debug_last_freed_version);
	s->num_io_toggles =  (u32)t->nt->dbg_num_enabling_io_toggles;
}

static void t_db_who_rlbalocks_fill(struct t_db_who_locks *s, const struct nvmeibc_raid1 *r1, u64 rlba) {
	const struct nvmeibc_disk_segment *ds = &r1->segments[get_owner_seg_slice_start(r1, rlba)];
	t_db_who_locks_fill(s, ds);
}

static void t_db_cmd_piggyback_fill(struct t_db_cmd_piggyback *s, const struct nvmeibc_block_command *c)
{
	const struct nvmeibc_disk_io_command *iocmd = c->iocmd;
	const struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(iocmd);
	const enum nvmeibc_rdma_intent code = dc->code;
	memset(s, 0, sizeof(*s));
	if (dp_cmds_pigbck_has_any(iocmd) || code) { // Pigyback lock/DB on command or execute it after the command
		const struct nvmeibc_cmd_lock *l = iocmd->comp.pigbck_lock;
		s->op_type = (u8)code; // Daniel: transport layer 'dp_cmds_get_pigbck_comp_dc(iocmd)->opr' is not initialized yet
		s->addr = iocmd->lpb.addr;
		if (l)
			s->retries = l->retries;
		else
			s->val = (u32)dp_cmds_get_piggyback_val(c);
	} else {
		s->op_type = 0xFF;	   // Nothing piggybacked nor executed after command
	}
}

static void t_db_who_cmd_fill(struct t_db_who_cmd *s,
							  const struct nvmeibc_block_command *c)
{
	s->first_rlba = 			c->first_rlba;
	s->req_id = 				c->iocmd->req_id;
	s->disk_address = 			c->iocmd->reqs1.disk_address;
	if (c->nlbas)
		s->nlbas = c->nlbas;
	else
		s->nlbas = c->iocmd->reqs1.ndb->length >> NVMEIBC_SECTOR_SHIFT;
	s->do_not_send = 			c->do_not_send;
	s->is_parity = 				c->is_parity;
	s->nlocks_take_before_cmd = c->nlocks_take_before_cmd;
	s->stage = 					c->my_stage;
	t_db_cmd_piggyback_fill(&s->pigb, c);
}

static void t_db_who_cmd_core_cell(struct t_core_dbgdi *s, const struct nvmeibc_block_command *c) {
	const struct nvmeibc_raid1 *r1 = nvmeibc_disk_segment_get_praid(c->ds);
	const bool is_mirrored = !nvmeibc_raid_is_ec(r1) && !nvmeibc_raid_is_jbod(r1);
	const int cell = is_mirrored ? c->ds->toma_reg->seg % 2 : 0;
	strncpy(s->wr[cell].disk_name, ((c->ds->disk)->base.ops.get_name(&((c->ds->disk))->base)), sizeof(s->wr[cell].disk_name));
}

static void t_db_who_jcmd_and_md_fill(struct t_db_who_writer *s,
					const struct nvmeibc_block_command *jc, const void *jmd)
{
	const struct nvmeibc_disk_client_journal *dj = ((jc->ds->disk)->base.ops.get_journal(&((jc->ds->disk))->base));
	struct nvmeibc_block_command* dcmd = dp_cmd_jour_to_data((void*)jc);
	const void *dmd = (jmd - jc->iocmd->reqs1.md) + dcmd->iocmd->reqs1.md;
	s->jrnl.is_valid = 1;
	s->jrnl.disk_addr = (u32)jc->iocmd->reqs1.disk_address + s->sgl.offset_from_cmd;
	s->jrnl.reserved1 = 0;
	s->jrnl.jri = dj->rng_id;
	s->jrnl.jblock= (s->jrnl.disk_addr + s->sgl.offset_from_cmd - dj->rng_slba);
	s->jrnl.binje_shift = ilog2(dj->rng_binje);
	s->jrnl.action = get_rcookie_ptr(jc->iocmd)->action;
	s->jrnl.jour_md = ((u64*)jmd)[0];   	// Save the first 8 bytes
	s->jrnl.reserved2 = 0;					// Init value
	s->data_md =	  ((u64*)dmd)[0];   	// Save the first 8 bytes
}

static void t_db_who_sgl_fill(struct t_db_who_sgl *s,
							  int i, int j, int cum_len, int n)
{
	s->sg_index = 		i;
	s->sub_index = 		(j >> NVMEIBC_SECTOR_SHIFT);	// Units of blocks
	s->offset_from_cmd= ((cum_len >> NVMEIBC_SECTOR_SHIFT) + s->sub_index);
	s->n_sg_elements =	n;
}

static void t_db_who_sync_fill_generic(struct t_db_who_sync *s,
							   const struct operation *o)
{
	s->magic = DBG_DI_MAGIC_SY;
	s->op =    (u8)o->op;
	t_db_who_clnt_fill(&s->clnt);
	t_db_who_topo_fill(&s->topo , o->topo);
}

static void t_db_who_sync_fill_mirror(struct t_db_who_sync *s,
							   const struct operation *o)
{
	const struct nvmeibc_block_command *orig_rldr = o->cmds; //o->rso->orig_rldr;
	s->reason = 0x7A;				// Todo: fill
	s->source_seg = (u8)o->cmds[0].ds->toma_reg->seg;
	if (orig_rldr) {
		t_db_who_cmd_fill( &s->orig_rldr, orig_rldr); // Read owner
		t_db_who_seg_fill( &s->orig_seg, orig_rldr->ds);
	}
}

static void t_db_who_sync_fill_ec(struct t_db_who_sync *s,
							   const struct nvmeibc_block_command *cmd)
{
	s->reason =		0x7B;				// Todo: fill
	s->source_seg = (u8)cmd->ds->toma_reg->seg;
	t_db_who_cmd_fill(&s->orig_rldr, cmd); // Read owner
	t_db_who_seg_fill(&s->orig_seg, cmd->ds);
}

static void dp_dbgdi_data_blk_check_and_init(data_blk *d)
{
	if (!dbgdi_log_initialized(&d->log))
		dbgdi_log_init(&d->log, DBG_DI_INJ_SPACE);
}

void dp_dbgdi_data_blk_check_and_init_v(void *d)
{
	dp_dbgdi_data_blk_check_and_init(d);
}

void dp_dbgdi_mark_edic(void *data, enum edic_result pass, u32 read_edic, u32 calc_edic, u64 rlba) {
	data_blk *d = (data_blk*)data;
	struct t_db_who_edic edic;
	edic.magic = DBG_DI_MAGIC_RD;
	edic.edic_res = (char)pass;
	edic.read_crc = read_edic;
	edic.calc_crc = calc_edic;
	edic.rlba = rlba;
	dp_dbgdi_data_blk_check_and_init(d);
	dbgdi_log_add_rec(&d->log, &edic, DBG_DI_EDIC, DBG_DI_EDIC_REC_SIZE);
	if (pass == EDIC_FAIL) {
		__print_block_debug(d);
	}
}

static void __data_blk_clear_all_history(data_blk *s)
{
	BUILD_BUG_ON(sizeof(data_blk) != NVMEIBC_SECTOR_SIZE);

	dbgdi_log_init(&s->log, DBG_DI_INJ_SPACE);
}

static void __data_blk_poison_before_read_sent(data_blk *s)
{
	s->dont_touch_original.data =	DBG_DI_MAGIC_IV;
	s->dont_touch_original.aux[0] = DBG_DI_MAGIC_IV;
	__data_blk_clear_all_history(s);
}

static void __verify_ec_writer_matches_seg_uuid(data_blk *d, const struct nvmeibc_raid1 *pr, const void* _uuid, const char *reason, const struct nvmeibc_block_command *cmd) {
	const struct t_db_who_writer *s;
	int writer_exists;
	struct t_db_who_writer __s;
	int rc;
	s = &__s;
	rc = dbgdi_log_get_record_by_type(&d->log, DBG_DI_WRITE, (void *)s, DBG_DI_WRITE_REC_SIZE);
	writer_exists = (rc == 0);

	if (writer_exists && nvmeibc_raid_is_ec(pr)) {
		const struct operation *o = cmd->o;
		const int rv = cmd->iocmd->comp.comp_code;
		const u8* read_seg_uuid = _uuid;
		if (memcmp(read_seg_uuid, s->seg.uuid, (sizeof(s->seg.uuid)-1)) != 0) {
			const char *wr_uuid = (const char*)&s->seg.uuid[0];
			__print_block_debug(d);			// Assuming before read we always poison the writer, we are in erronous situation
			if (rv == 0) {
				WARN(true, "nvmeibc bug. DI? o=%p, reason %s, uuid {expected=%s != writer=%.9s}\n", o, reason, read_seg_uuid, wr_uuid);
			} else {
				_NT(t_01dbgdiwv,"EC-6604: Poison-overwritten-by-garbage, upon cmd failure? reason @STR, rv=@RV, cleaning all history including reader injection", reason, rv);
				__data_blk_poison_before_read_sent(d); // Failed read brought wrong writer, clean it by marking as if read was never sent, to avoid future confusion
			}
		}
	}
}

static void t_db_who_reader_fill(data_blk *d, const void *md,
	const struct nvmeibc_block_command *cmd, int sgi, int j_in_sgi, int cum_len)
{
	struct t_db_who_reader s; // All fields are set no need for init
	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(cmd->ds);
	s.magic = DBG_DI_MAGIC_RD;
	t_db_who_clnt_fill(&s.clnt);
	t_db_who_cmd_fill(  &s.cmd	   , cmd);
	t_db_who_sgl_fill(  &s.sgl	   , sgi, j_in_sgi, cum_len,
									 cmd->iocmd->reqs1.ndb->table.nents);
	t_db_who_bio_fill(&s.who_bio  , cmd->o);
	t_db_who_seg_fill(  &s.seg	   , cmd->ds);
	t_db_who_praid_fill(&s.raid   , pr);
	t_db_who_topo_fill( &s.topo   , cmd->ds->chunk->topology);
	t_db_who_rlbalocks_fill(&s.rlbalocks, pr, cmd->first_rlba + sgi + j_in_sgi);
	s.comp_code = cmd->iocmd->comp.comp_code;
	if (md)
		s.meta_data = ((u64 *)md)[0];   	// Save the first 8 bytes
	else
		s.meta_data = 0;				// Mark Virgin and not checked

	/* avoid adding twice the same read record */
	if (d->log.header.last_rec_type != DBG_DI_READ) {
		dbgdi_log_add_rec(&d->log, &s, DBG_DI_READ, DBG_DI_READ_REC_SIZE);
		__verify_ec_writer_matches_seg_uuid(d, pr, cmd->ds->uuid, "writer_vs_reader", cmd);
	}
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-larger-than"
#endif
void __print_block_debug(const void *_s)
{	/* Uncomment to debug the injection mechanism (print the injected data) */
	#define size_of_data_blk_to_string (4096)
	const data_blk *s = _s;
	static char print_block[size_of_data_blk_to_string];
	static DEFINE_SPINLOCK(dbgdi_l);
	unsigned long flags;
	spin_lock_irqsave(&dbgdi_l, flags);
	data_blk_to_string(s, print_block, size_of_data_blk_to_string);
	print_block[4000] = '\0'; /* Because tracer currently cannot deal with such sizes */
	_NT(t_xa_dp_dbg_tools, "@STR", print_block);
	memset(print_block, 0, sizeof(print_block));
	spin_unlock_irqrestore(&dbgdi_l, flags);
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#if 0
void __generate_magics(void) {
	union { u64 k; char s[8]; } v;
	memcpy(v.s, "BlkWr_ok",8);
	pr_alert("#define XXXX (0x%llxLL) ; \"%-.8s\" Magic\n", v.k, &v.s[0]); // dp_dbg_tools
	exit(1);
}
#endif

static void recovery_fill_for_write(struct t_db_who_recovery *rec, const struct nvmeibc_raid1 *pr,
									const struct nvmeibc_block_command *cmd)
{
	t_db_who_clnt_fill( &rec->clnt);
	t_db_who_seg_fill(  &rec->seg, cmd->ds);
	t_db_who_praid_fill(&rec->raid, pr);
	t_db_who_topo_fill( &rec->topo, cmd->ds->chunk->topology);
	t_db_who_cmd_fill(  &rec->cmd, cmd);
}

void dp_dbgdi_clear_sync_overwritten(struct nvmeibc_block_command *cmd)
{
	struct sgl_block_iter sbi = SGL_BLOCK_ITER_INIT(nvmeibc_block_command_get_sgl(cmd));
	unsigned int nlbas = cmd->nlbas;
	struct t_db_who_mark mark = {.dbg_di_magic = 0}; // init required for dbg_di_magic value, log entry e is set in add_rec

	BUG_ON(!nvmeib_block_io_op_is_write(cmd->iocmd->reqs1.op));

	if (unlikely(sizeof(data_blk) > NVMEIBC_SECTOR_SIZE))
		return;			// No space within block for injection

	for (; nlbas; nlbas--, sgl_block_iter_advance(&sbi, 1)) {
		data_blk *dblk = (data_blk *)sgl_block_iter_virt(&sbi);

		dp_dbgdi_data_blk_check_and_init(dblk);
		dbgdi_log_add_rec(&dblk->log, (void *)&mark, DBG_DI_SYNC_OVERWRITTEN_CLEARED, DBG_DI_MARK_REC_SIZE);
	}
}

void dp_dbgdi_copy_sync_overwritten(struct nvmeibc_block_command *dst, const struct nvmeibc_block_command *src)
{
	struct sgl_block_iter sbi_src = SGL_BLOCK_ITER_INIT(nvmeibc_block_command_get_sgl(src));
	struct sgl_block_iter sbi_dst = SGL_BLOCK_ITER_INIT(nvmeibc_block_command_get_sgl(dst));
	unsigned int nlbas = src->nlbas;
	struct t_db_who_writer w; // Used to init all structs to 0, however, w is copied from other block, init is not required
	int rc;

	BUG_ON((!nvmeib_block_io_op_is_write(dst->iocmd->reqs1.op)) || (src->iocmd->reqs1.op != NVMEIB_BLOCK_IO_OP_READ));
	BUG_ON(dst->nlbas != src->nlbas);
	BUG_ON(!nvmeib_block_io_op_is_rw_op(dst->iocmd->reqs1.op) || !nvmeib_block_io_op_is_rw_op(src->iocmd->reqs1.op));

	if (unlikely(sizeof(data_blk) > NVMEIBC_SECTOR_SIZE))
		return;			// No space within block for injection

	for (; nlbas; nlbas--, sgl_block_iter_advance(&sbi_src, 1), sgl_block_iter_advance(&sbi_dst, 1)) {
		data_blk *dblk_src = (data_blk *)sgl_block_iter_virt(&sbi_src);
		data_blk *dblk_dst = (data_blk *)sgl_block_iter_virt(&sbi_dst);

		/* copy write record from src to dst */
		rc = dbgdi_log_get_record_by_type(&dblk_src->log, DBG_DI_WRITE, (void *)&w, DBG_DI_WRITE_REC_SIZE);
		if (!rc) {
			dp_dbgdi_data_blk_check_and_init(dblk_dst);
			dbgdi_log_add_rec(&dblk_dst->log, (void *)&w, DBG_DI_SYNC_OVERWRITTEN, DBG_DI_WRITE_REC_SIZE);
		}
	}
}

/* Used by reed solomon to erase history from recovered/calculated blocks */
static void __clear_restored_block_history(void *_d, bool is_destroyed_blk) {
	data_blk *d = (data_blk*)_d;
	struct t_db_who_mark mark; // Init not required all fields are set in both paths
	if (is_destroyed_blk) {
		mark.dbg_di_magic = DBG_DI_MAGIC_WD;   // Writer Magic is destroyed
		dbgdi_log_add_rec(&d->log, &mark, DBG_DI_WRITE_DESTROYED, DBG_DI_MARK_REC_SIZE);
	} else {
		__data_blk_clear_all_history(d);
		mark.dbg_di_magic = DBG_DI_MAGIC_WX;	// Writer Magic is restored
		dbgdi_log_add_rec(&d->log, &mark, DBG_DI_WRITE_RESTORED, DBG_DI_MARK_REC_SIZE);
	}
}

static void data_blk_fill_for_sync(data_blk *d, const void *md,
	const struct nvmeibc_block_command *cmd)
{
	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(cmd->ds);
	const struct operation *o = cmd->o;
	const enum nvmeib_block_io_op op = o->op;
	struct t_db_who_recovery r = {.magic = 0}; // Init required even if all fields are filled - TODO determine why fails on unitest_ECDbitsOnDisk
	struct t_db_who_sync sync = {.magic = 0}; // Must be init, in mirror if orig_rldr doesn't exist orig_seg/orig_rldr are not set TODO - fix

	t_db_who_cmd_core_cell(&d->core, cmd);

	dp_dbgdi_data_blk_check_and_init(d);

	/* add generic sync record */
	t_db_who_sync_fill_generic(&sync, o);
	t_db_who_praid_fill(&sync.raid   , pr);
	if (nvmeibc_raid_is_ec(pr)) {
		t_db_who_sync_fill_ec(&sync, cmd);
		__verify_ec_writer_matches_seg_uuid(d, pr, sync.orig_seg.uuid, "writer_vs_syncer", cmd);
	} else
		t_db_who_sync_fill_mirror(&sync, o);
	if (md)
		sync.meta_data = ((u64*)md)[0];		// Save the first 8 bytes
	else
		sync.meta_data = 0;
	dbgdi_log_add_rec(&d->log, &sync, DBG_DI_SYNC, DBG_DI_SYNC_REC_SIZE);

	/* add sync specific record */
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_RECOVER_STALE:
		r.magic = DBG_DI_MAGIC_ST;
		recovery_fill_for_write(&r, pr, cmd);
		dbgdi_log_add_rec(&d->log, &r, DBG_DI_RECOVER_STALE_LOCK, DBG_DI_RECOVER_REC_SIZE);
		break;

	case NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL:
		r.magic = DBG_DI_MAGIC_RF;
		recovery_fill_for_write(&r, pr, cmd);
		dbgdi_log_add_rec(&d->log, &r, DBG_DI_RECOVER_READ_FAIL, DBG_DI_RECOVER_REC_SIZE);
		break;

	case NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING:
		r.magic = DBG_DI_MAGIC_SC;
		recovery_fill_for_write(&r, pr, cmd);
		dbgdi_log_add_rec(&d->log, &r, DBG_DI_RECOVER_SCRUBBING, DBG_DI_RECOVER_REC_SIZE);
		break;

	case NVMEIB_BLOCK_IO_OP_RECOVER_DB:
		r.magic = DBG_DI_MAGIC_DB;
		recovery_fill_for_write(&r, pr, cmd);
		dbgdi_log_add_rec(&d->log, &r, DBG_DI_RECOVER_DIRTY_BITS, DBG_DI_RECOVER_REC_SIZE);
		break;

	case NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK: {
		struct t_db_who_rb_recovery rb; // Init not required as all fields are set
		struct recovery_sync_op *so = o->rso;
		rb.rec.magic = DBG_DI_MAGIC_RB;
		recovery_fill_for_write(&rb.rec, pr, cmd);
		rb.nwhole_params = so->nwhole_params.raw;
		dbgdi_log_add_rec(&d->log, &rb, DBG_DI_RECOVER_ROLLBACK, DBG_DI_RECOVER_RB_REC_SIZE);
		break;
	}

	case NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP:
		r.magic = DBG_DI_MAGIC_WA;
		recovery_fill_for_write(&r, pr, cmd);
		dbgdi_log_add_rec(&d->log, &r, DBG_DI_RECOVER_TXID_WRAPAROUND, DBG_DI_RECOVER_REC_SIZE);
		break;

	default:	// Read / Trims / Various syncs which dont write data, maintanace op's etc
		WARN(true, "nvmeibc bug, op=0x%x cannot inject data\n", op);
		break;
	}
}

#define __get_tbl(cmd) ((cmd)->iocmd->reqs1.ndb->table)
/* Replace most of the blocks data with internal structs, for write commands */
static void __data_blk_fill_for_write(data_blk *d, const void *md,
	const struct nvmeibc_block_command *cmd, int sgi, int j_in_sgi, int cum_len)
{
	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(cmd->ds);
	struct nvmeibc_block_command *rldr = dp_cmd_get_raid_leader((void *)cmd);
	const struct operation *o = cmd->o;
	struct t_db_who_writer s = {.dbg_di_magic = 0};	// Non-journal EC write doesn't set data_md so init is required - TODO fix, jrnl might not be full set
	const bool is_mirrored = !nvmeibc_raid_is_ec(pr) && !nvmeibc_raid_is_jbod(pr);

	/* NVMESH-4505 - for mirror, the buffer is shared between the per-segment commands --> FIFO MD corruption
	 * may take place if one command fills the writer record while another command is DMA-ed. To address that
	 * we only inject writer record for the raid leader. */
	if (is_mirrored && cmd != &rldr[dp_cmds_get_first_cmd_of_stage(rldr, E_CMDS_STAGE_DO_IO_AND_PAR)]) {
		return;
	}

	__data_blk_clear_all_history(d);

	s.dbg_di_magic = DBG_DI_MAGIC_WR;
	t_db_who_clnt_fill( &s.clnt);
	t_db_who_bio_fill(  &s.who_bio, o);
	t_db_who_seg_fill(  &s.seg	   , cmd->ds);
	t_db_who_praid_fill(&s.raid   , pr);
	t_db_who_topo_fill( &s.topo   , cmd->ds->chunk->topology);
	t_db_who_cmd_fill(  &s.cmd	   , cmd);
	t_db_who_sgl_fill(  &s.sgl	   , sgi, j_in_sgi, cum_len, __get_tbl(cmd).nents);
	t_db_who_rlbalocks_fill(&s.rlbalocks, pr, cmd->first_rlba + sgi + j_in_sgi);
	if (md) {
		if (cmd->my_stage == E_CMDS_STAGE_WRITE_JOURNAL) {
			t_db_who_jcmd_and_md_fill(&s, cmd, md);
		} else if (cmd->my_stage == E_CMDS_STAGE_DO_IO_AND_PAR) {
			s.jrnl.is_valid = 0;
			if (!nvmeibc_raid_is_ec(pr))
				s.data_md = ((u64*)md)[0];	// For R1 there is no journal stage
		}
		/* Actually Added both for journal and data coz they share sglist */
	} else {	// Mark invalid
		s.jrnl.is_valid = 0;
		s.jrnl.jour_md = 0;
		s.data_md = 0;
	}

	dbgdi_log_add_rec(&d->log, &s, DBG_DI_WRITE, DBG_DI_WRITE_REC_SIZE);
}

static void data_blk_fill_for_write(data_blk *d, const void *md,
	const struct nvmeibc_block_command *cmd, int sgi, int j_in_sgi, int cum_len)
{
	const struct operation *o = cmd->o;
	const enum nvmeib_block_io_op op = o->op;
	t_db_who_cmd_core_cell(&d->core, cmd);

	switch (op) {
	case NVMEIB_BLOCK_IO_OP_WRITE:
		__data_blk_fill_for_write(d, md, cmd, sgi, j_in_sgi, cum_len);
		break;

	case NVMEIB_BLOCK_IO_OP_RECOVER_STALE:
	case NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL:
	case NVMEIB_BLOCK_IO_OP_RECOVER_DB:
	case NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK:
	case NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING:
	case NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP:
		data_blk_fill_for_sync(d, md, cmd);
		break;

	default:	// Read / Trims / Various syncs which dont write data, maintanace op's etc
		WARN(true, "nvmeibc bug, op=%d cannot inject data\n", op);
		break;
	}
}

#define dbg_di_enabled(o) ((o)->nd->dp.enable_di_debug_mode)
bool dp_dbgdi_should_add_info_core(struct nvmeibc_disk_io_command *iocmd)
{
	const struct nvmeibc_block_command *cmd = iocmd->comp.cmd;
	return !iocmd->reqs1.do_512b_sub_block_x && unlikely(dbg_di_enabled(cmd->o) && (iocmd->reqs1.op <= NVMEIB_BLOCK_IO_OP_WRITE));
}

static void __dbgdi_do_add_info(struct nvmeibc_block_command *cmd)
{
	struct sg_table *sgtbl = &__get_tbl(cmd);
	void *md = cmd->iocmd->reqs1.md;
	struct scatterlist *curSG = NULL;
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	int i, j, nSGelements = sgtbl->nents, cum_length = 0;
	const enum nvmeib_block_io_op op = cmd->iocmd->reqs1.op;
	if (unlikely(sizeof(data_blk) > NVMEIBC_SECTOR_SIZE))
		return;			// No space within block for injection
	for_each_sg(sgtbl->sgl, curSG, nSGelements, i) {
		const int length = curSG->length;
		u8 *buf			 = (u8*)sg_virt(curSG);
		if (length < (int)sizeof(data_blk))
			goto _injection_done; // Cannot fill this entry, too short :-(
		for (j = 0; j < length; j += NVMEIBC_SECTOR_SIZE) {
			data_blk* dblk = (data_blk*)&buf[j];
			switch (op) { /* != cmd->o->op */
			case NVMEIB_BLOCK_IO_OP_READ:
				__data_blk_poison_before_read_sent(dblk);
				break;
			case NVMEIB_BLOCK_IO_OP_WRITE:
				data_blk_fill_for_write(dblk, md, cmd, i, j, cum_length);
				if (md)
					md += md_size;
				break;
			default:;
				BUG(); /* Do nothing on trim's and other unknown commands */
			}
		}
		_injection_done:
			cum_length += length;	// Offset of curSG from cmd[n] start
	}
}

void dp_dbgdi_do_add_info(struct nvmeibc_block_command *cmd, bool should_execute)
{
	if (unlikely(dbg_di_enabled(cmd->o))) {
		const enum nvmeib_block_io_op op = cmd->iocmd->reqs1.op;
		if ((op == NVMEIB_BLOCK_IO_OP_READ) ||							// Read -> Always poison read operation, even if cmd was not sent to disk, or else it will have incorrect garbage writer info
			((nvmeib_block_io_op_is_write(op)) && should_execute))		// Writer -> Fill writer only if cmd is actually sent.
			__dbgdi_do_add_info(cmd);
	}
}

void dp_dbgdi_do_add_restore_info(struct nvmeibc_block_command *cmd, bool is_destroyed_blk)
{
	if (unlikely(dbg_di_enabled(cmd->o))) {
		struct sg_table *sgtbl = &__get_tbl(cmd);
		struct scatterlist *curSG = NULL;
		unsigned i;
		for_each_sg(sgtbl->sgl, curSG, sgtbl->nents, i) {
			u8 *buf	= (u8*)sg_virt(curSG), *end = &buf[curSG->length];
			for (; buf < end; buf += NVMEIBC_SECTOR_SIZE)
				__clear_restored_block_history((void *)buf, is_destroyed_blk);
		}
	}
}

/* Helper function for dp_dbgdi_foreach_sector, If journal available - fill in both jlba and dlba, Else - both will be 0 */
static void __dp_dbgdi_get_dlba_and_jlba(const void *data, u64 *dlba, u64 *jlba) {
	const struct t_db_who_writer *writer;
	const data_blk *p = data;
	bool wr_exists;
	struct t_db_who_writer __writer;
	int rc;
	writer = &__writer;
	rc = dbgdi_log_get_record_by_type(&p->log, DBG_DI_WRITE, (void *)writer, DBG_DI_WRITE_REC_SIZE);
	wr_exists = (rc == 0);
	*dlba = *jlba = 0;
	if (wr_exists) {
		if (writer->jrnl.is_valid ) {
			*dlba = nvmeibc_block_dp_ec_jmd_decode_j2d_only(((const union jblock_md *)&writer->jrnl.jour_md));
			*jlba = writer->jrnl.disk_addr;
		}
	}
}

#define dp_dbgdi_foreach_sector(req, when, ...) ({								\
	struct sg_table *sg_tbl = &req->ndb->table; 								\
	struct scatterlist *sgl = sg_tbl->sgl;  									\
	struct scatterlist *sg_i;   												\
	int n = sg_tbl->nents, i, j, page = 0, rv = 0;								\
																				\
	for_each_sg(sgl, sg_i, n, i) {  											\
		const int length = sg_i->length;										\
		void *buf = sg_virt(sg_i);  											\
		_ND(t_3d_dp_dbg_tools_##when, "op=@INT, sg_i @INT of @INT, buf=@PTR",	\
			req->op, i, n, buf); 												\
		for (j = 0; j < length; j += NVMEIBC_SECTOR_SIZE) { 					\
			void *p_ = buf + j; 												\
			u64 jlba, dlba;														\
			__dp_dbgdi_get_dlba_and_jlba(p_, &dlba, &jlba);						\
			_ND(t_3e_dp_dbg_tools_##when,										\
				"op=@INT, sg_i @INT of @INT, (@INT, @PTR)",						\
				req->op, i, n, j, p_);											\
			switch (req->op) {													\
			case NVMEIB_BLOCK_IO_OP_WRITE:  									\
				data_blk_fill_for_core_wr_##when(								\
					dp_dbgdi_get_core_area(p_), dlba, jlba,						\
					req->op, page, ## __VA_ARGS__);								\
				break;  														\
			case NVMEIB_BLOCK_IO_OP_READ:   									\
				rv |= data_blk_fill_for_core_rd_##when(							\
					dp_dbgdi_get_core_area(p_), dlba, jlba,						\
					req->op, page, ## __VA_ARGS__); 							\
				break;															\
			default:															\
				WARN_ON_ONCE(1);												\
				_NE(t_3f_dp_dbg_tools_##when, "unsupported op @INT", req->op);	\
				break;															\
			}																	\
			page++;																\
		}																		\
	}   																		\
	rv;																			\
})

/* Stores the last read page contents IN CASE OF DATA CORRUPTION */
char dp_dbgdi_last_read_page[PAGE_SIZE];
/* Acts as a lock for dp_dbgdi_last_read_page. Not using spinlock to avoid the initialization hussle. */
atomic_t dp_dbgdi_last_read_page_guard = ATOMIC_INIT(0);

void *dp_dbgdi_get_page_start_from_core(void *core_dbgdi_data)
{
	BUILD_BUG_ON(sizeof(struct t_data_blk) > NVMEIBC_SECTOR_SIZE);
	BUILD_BUG_ON(sizeof(struct t_dont_touch_original) != DEBUG_DI_SIZE);
	return container_of(core_dbgdi_data, struct t_data_blk, core);
}

void dp_dbgdi_do_add_info_core_pre(struct nvmeibc_block_io_req *req, struct t_core_dbgdi_params_pre *p)
{
	/* data_blk_fill_for_core_wr_pre
	   data_blk_fill_for_core_rd_pre */
	dp_dbgdi_foreach_sector(req, pre, p);
}

int dp_dbgdi_do_add_info_core_post(struct nvmeibc_block_io_req *req, struct t_core_dbgdi_params_post *p)
{
	/* data_blk_fill_for_core_wr_post
	   data_blk_fill_for_core_rd_post */
	return dp_dbgdi_foreach_sector(req, post, p);
}

static void __dp_dbgdi_do_rdr_info_of_read_op(struct nvmeibc_block_command *cmd)
{
	struct sg_table *sgtbl = &__get_tbl(cmd);
	void*md = cmd->iocmd->reqs1.md;
	struct scatterlist *curSG = NULL;
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(cmd->ds);
	int i, j, nSGelements = sgtbl->nents, cum_length = 0;
	for_each_sg(sgtbl->sgl, curSG, nSGelements, i) {
		const int length = curSG->length;
		u8 *buf =		   (u8*)sg_virt(curSG);
		if (length < (int)sizeof(data_blk))
			goto _injection_done; // Cannot fill this entry, too short :-(
		for (j = 0; j < length; j += NVMEIBC_SECTOR_SIZE) {
			data_blk* dblk = (data_blk*)&buf[j];
			dp_dbgdi_data_blk_check_and_init(dblk);
			t_db_who_reader_fill(dblk, md, cmd, i, j, cum_length);
			if (md)
				md += md_size;
		}
		_injection_done:
			cum_length += length;	// Offset of curSG from cmd[n] start
	}
}

static void __dp_dbgdi_do_rdr_info_for_sync(struct nvmeibc_block_command *cmd)
{
	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(cmd->ds);
	struct sg_table *sgtbl = &__get_tbl(cmd);
	struct scatterlist *curSG = NULL;
	data_blk* dblk;
	unsigned i;
	for_each_sg(sgtbl->sgl, curSG, sgtbl->nents, i) {
		u8 *buf	= (u8*)sg_virt(curSG), *end = &buf[curSG->length];
		for (; buf < end; buf += NVMEIBC_SECTOR_SIZE) {
			dblk = (data_blk*)buf;
			dp_dbgdi_data_blk_check_and_init(dblk);
			__verify_ec_writer_matches_seg_uuid((void*)dblk, pr, cmd->ds->uuid, "writer_vs_sync_reader", cmd);
		}
	}
}

void dp_dbgdi_do_rdr_info(struct nvmeibc_block_command *cmd)
{
	const struct operation *o = cmd->o;
	if (!dbg_di_enabled(o)) {
		return; // do nothing
	} if (unlikely(cmd->iocmd->reqs1.ndb == NULL)) {
		return; // This is irrelevant gen cmd, not even io cmd
	} else if (cmd->iocmd->reqs1.op != NVMEIB_BLOCK_IO_OP_READ) {
		return; // Write cmd, do nothing
	} else if (o->op == NVMEIB_BLOCK_IO_OP_READ) {
		__dp_dbgdi_do_rdr_info_of_read_op(cmd);		// Inject read info upon read completion
	} else {
		__dp_dbgdi_do_rdr_info_for_sync(cmd);		// In sync dont inject anything, jsut verify correctness of writer
	}
}

void dp_dbgdi_do_add_info_unitest(void *_d, const char *seg_uuid)
{
	struct t_data_blk *d = _d;
	struct t_db_who_writer w = {.dbg_di_magic = 0}; // Init required for injection - not all fields are filled
	__clear_restored_block_history(d, false);
	w.dbg_di_magic = DBG_DI_MAGIC_WR;				// Simplified version of 'data_blk_fill_for_write()'
	nvmeib_strlcpy((char*)&w.seg.uuid[0], seg_uuid, sizeof(w.seg.uuid));

	dbgdi_log_add_rec(&d->log, &w, DBG_DI_WRITE, DBG_DI_WRITE_REC_SIZE);
}

bool dp_dbgdi_should_add_rider_info(const struct operation *o) { (void)o; return false; }
void dp_dbgdi_do_add_rider_info(struct d_carrier_base_block_io *d, const struct nvmeibc_block_device *car, const struct operation *o) { (void)d; (void)car; (void)o; }
void dp_dbgdi_do_add_rider_rdr_info(const struct bio_extention *bext, const struct nvmeibc_block_device *car) { (void)bext; (void)car; }

bool dp_dbgdi_should_add_rider_rdr_info(const struct bio_extention *bext)
{
	const struct operation *rider_o = bext->rider.o;
	return (rider_o && dbg_di_enabled(rider_o) && rider_o->op == NVMEIB_BLOCK_IO_OP_READ);
}

int dp_dbgdi_get_sizeof_injected_data(void)
{
	return data_blk_get_injection_size();
}


void* dp_dbgdi_get_recovery_hot_area(void* data)
{
	data_blk* dblk = (data_blk*)data;
	return (void *)&dblk->log;
}

void* dp_dbgdi_get_core_area(void *data)
{
	data_blk *dblk = (data_blk *)data;
	return (void*)&dblk->core;
}

void* dp_dbgdi_get_core_area_container(const void *c)
{
	return container_of(c, data_blk, core);
}
#endif	// DBGDI_REMOVED_IN_PRODUCTION

