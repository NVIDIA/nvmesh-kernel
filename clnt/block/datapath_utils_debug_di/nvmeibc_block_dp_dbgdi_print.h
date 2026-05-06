/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_DBGDI_PRINT_H
#define NVMEIBC_DP_DBGDI_PRINT_H

#include "nvmeibc_block_dp_dbgdi_blk.h"

#ifdef DBGDI_REMOVED_IN_PRODUCTION
	static inline int data_blk_cmp(const data_blk *s0, const data_blk *s1) { return (*s0 == *s1) ? 0 : -1; }
	static inline ssize_t data_blk_to_string(const data_blk *s, char *buf, ssize_t len) {
		return scnprintf(buf, len, "User Data: {0x%016llx,0x%016llx}, debug di not supported!\n", s[0], s[1]);
	}
#else

/* Injected block compare and print utilities. This header can be compiled for kernel to
   dump injected blocks to traces and to third party user space apps to parse the
   binary block and output it as string */

#include "../../../clnt/nvmeibc_core_dbgdi.h"
#include "../../block/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_hot_dbgdi.h"
#include "../../block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "nvmeibc_block_dp_dbgdi_log.h"
typedef struct t_mtv_data_blk_analysis {
		bool write_exists;
		bool read_exists;
		bool stage_exists;
		bool destage_exists;
} __attribute__ ((packed)) mtv_data_analysis;

typedef struct t_data_blk_analysis { /* Analysis of the injected data */
	bool wr_exists, read_failed;		// Do we have write information. If Read could not be sent the wr_exists -s false
	bool never_written;					// Was this block never written to
	bool block_restored;				// Did we restore this block from others with Reed Solomon
	bool read_invalid;					// Was this block written with destroyed data (marked in MD jri==2)
	bool sync_overwritten_wr_exists;
	bool sync_overwritten_read_failed_or_dirty;  // If filled the magic with READ it means there was no valid source to take the old writer's data from
	bool sync_overwritten_never_written;	     // Was this block never written to dbg_di filled but with garbage.
	bool r_exists;
	bool sync_exists;
	bool multiple_sync;
	bool core_exists;
	mtv_data_analysis mtv_an;
	char vol_type;						// 'U' unknown, 'E' EC, '1' R1
	u8   D,P;							// D+P of protection raid
	bool is_journal_cmd;				// Is this block was written as journal or as data
	bool diff_segs;						// Data was read not from segment it was written to
	bool diff_clnt;						// Data was read not by client who wrote it
	bool reader_poison;
	bool writer_restore;

	/* for advanced data analysis, content non zero only if the relevant _exists indicator is true */
	struct t_db_who_reader reader;
	struct t_db_who_writer writer;
	struct t_db_who_sync sync;
	struct nvmeibc_dp_recovery_hot_dbgdi hot_recovery;
} __attribute__((packed)) data_analysis;

#define data_blk_does_writer_exists(s) ((s).dbg_di_magic == DBG_DI_MAGIC_WR)
#define data_blk_does_syncer_exists(s) ((s).magic        == DBG_DI_MAGIC_SY)
#define data_blk_does_reader_exists(s) ((s).magic        == DBG_DI_MAGIC_RD)

static void __data_analysis_set_from_praid(data_analysis *a, struct t_db_who_praid *pr)
{
	a->vol_type = ((pr->slice_size > 1) ? 'E' : '1');
	a->D = pr->slice_size;
	a->P = pr->n_segs - pr->slice_size;
}

static void __data_analysis_init(data_analysis* a, struct dbgdi_log_entry *e)
{
	struct t_db_who_writer *wr;
	struct t_db_who_reader *rd;
	struct t_db_who_sync *sync;
	struct nvmeibc_dp_recovery_hot_dbgdi *hot;
	const union nvmeibc_block_dp_ec_data_block_md *md;

	switch (e->type) {
	case DBG_DI_WRITE:
		wr = (struct t_db_who_writer *)e;
		a->wr_exists = true;
		a->read_failed = (wr->dbg_di_magic == DBG_DI_MAGIC_WP); // Before-Read injection exists but buffer was not read from disk at all
		// Detect never-written blocks
		md = (union nvmeibc_block_dp_ec_data_block_md*)&wr->data_md;
		a->never_written = nbdpec_md_was_data_never_written(md);
		a->read_invalid = is_data_invalid_for_read(md);
		__data_analysis_set_from_praid(a, &wr->raid);
		memcpy(&a->writer, wr, sizeof(*wr));
		break;

	case DBG_DI_SYNC_OVERWRITTEN:
		a->sync_overwritten_wr_exists = true;
		break;

	case DBG_DI_WRITE_RESTORED:
		a->writer_restore = true;
		break;

	case DBG_DI_WRITE_DESTROYED:
		break;

	case DBG_DI_READ:
		rd = (struct t_db_who_reader *)e;
		a->reader_poison =  (rd->magic        == DBG_DI_MAGIC_RP);		// No history for read from disk: Either poisoned before read/write, or restored via xors
		a->r_exists = true;
		// Detect never-written blocks
		md = (union nvmeibc_block_dp_ec_data_block_md*)&rd->meta_data;
		a->never_written = nbdpec_md_was_data_never_written(md);
		a->read_invalid = is_data_invalid_for_read(md);
		__data_analysis_set_from_praid(a, &rd->raid);
		memcpy(&a->reader, rd, sizeof(*rd));
		break;

	case DBG_DI_EDIC:
		break;

	case DBG_DI_SYNC:
		sync = (struct t_db_who_sync *)e;
		if (a->sync_exists)
			a->multiple_sync = true;
		else
			a->sync_exists = true;
		__data_analysis_set_from_praid(a, &sync->raid);
		memcpy(&a->sync, sync, sizeof(*sync));
		break;

	case DBG_DI_RECOVER_READ_FAIL:
	case DBG_DI_RECOVER_SCRUBBING:
	case DBG_DI_RECOVER_DIRTY_BITS:
	case DBG_DI_RECOVER_STALE_LOCK:
	case DBG_DI_RECOVER_ROLLBACK:
	case DBG_DI_RECOVER_TXID_WRAPAROUND:
		break;

	case DBG_DI_RECOVER_HOT:
		hot = (struct nvmeibc_dp_recovery_hot_dbgdi *)e;
		memcpy(&a->hot_recovery, hot, sizeof(*hot));
		break;

	case DBG_DI_MTV:
		break;

	default:
		break;
	}

}

static void data_analysis_init(data_analysis* a, const data_blk *s)
{
	const struct dbgdi_log *log = &s->log;
	int rc, loc = dbgdi_log_iter_init(log);
	u8 rec_buf[DBG_DI_MAX_REC_SIZE];
	struct dbgdi_log_entry *e;

	memset(a, 0, sizeof(*a));
	a->vol_type = 'U';		// No injections exist, not much we can do...

	if (loc == -1) {
		return;
	}

	do {
		rc = dbgdi_log_get_rec(log, loc, rec_buf, DBG_DI_MAX_REC_SIZE);
		if (rc != -1) {
			e = db_entry(&rec_buf[0]);
			__data_analysis_init(a, e);
		}
		loc = dbgdi_log_iter_next(log, loc);
		if (loc < 0)
			break;
	} while ((u32)loc != log->header.head);

	a->block_restored = (a->writer_restore && (a->r_exists || a->reader_poison));	// Injected in memory or Restored from NVMe Drive

	// Detect never-written blocks
	if (!a->wr_exists && !a->r_exists) {		// Both are zero -> zero block
		a->never_written = true;
	} else if (a->block_restored) {
		// Todo: How to know that restored block was never written? a->never_written = true;
	}

	if (a->sync_overwritten_wr_exists && !a->wr_exists && !a->read_failed)
		a->sync_overwritten_never_written = true;  // Might also be corrupted but need to know the overriden md - for now we don't have md in mirror.
}

/**************************** ToString function ******************************/
#define BUF_ADD(...) cnt += scnprintf(buf+cnt, len-cnt, __VA_ARGS__)
static inline ssize_t t_core_dbgdi_lock_piggyback_to_string(const struct t_core_dbgdi_lock_piggyback *pb, data_analysis *an, char*buf, ssize_t len) {
	ssize_t cnt = 0;
	(void)an;
	BUF_ADD("Piggiback %s\n", pb->valid ? "VALID" : "NOT VALID");
	BUF_ADD("LLP(off=0x%016llx, val=0x%016llx)\n", pb->llp.off, pb->llp.val);
	BUF_ADD("ULP(type=0x%lld, addr=0x%016llx, val0=0x%016llx, val1=0x%016llx)\n",
	        pb->ulp.type, pb->ulp.addr, pb->ulp.val0, pb->ulp.val1);

	return cnt;
}

static inline ssize_t t_core_dbgdi_to_string(const struct t_core_dbgdi *c, data_analysis *an, char*buf, ssize_t len)
{
	ssize_t cnt = 0;
	an->core_exists = (c->rd.magic != 0);			// At least some core info exists
	{
		const struct t_core_dbgdi_wr *wr;
			for_each_core_writer_area(wr, c) {
				an->core_exists |= wr->magic;
			}
	}
	if (an->core_exists) {
		const struct t_core_dbgdi_wr *wr;
		bool core_r_exists = (c->rd.magic == NVMEIBC_CORE_DBG_DI_MAGIC_RD);
		const size_t rd_section_offset = (size_t)&c->rd - (size_t)container_of(c, struct t_data_blk, core);
		for_each_core_writer_area(wr, c) {
			const size_t section_offset = (size_t)wr - (size_t)container_of(c, struct t_data_blk, core);
			bool core_w_exists = (wr->magic == NVMEIBC_CORE_DBG_DI_MAGIC_WR);
			BUF_ADD("------- CoreW (off=0x%lx) %s ---------\n", section_offset, (core_w_exists ? "OK" : "NOT DONE"));
			BUF_ADD("Disk %.40s\n", wr->disk_name);
			BUF_ADD("op       :%d, ch=%d, reuse=%d, container=0x%016llx, "
			        "ch_ptr=0x%016llx, io_id=%llu, dlba=%llu(0x%llx) jlba=%llu(0x%llx)\n",
			        wr->op, wr->ch_type, wr->reuse_bb, wr->container_ptr,
			        wr->ch_ptr, wr->io_id, wr->lba.dlba, wr->lba.dlba,
			        wr->lba.jlba, wr->lba.jlba);
			cnt += t_core_dbgdi_lock_piggyback_to_string(&wr->lock_pgbk, an, buf + cnt, len - cnt);
		}
		BUF_ADD("------- CoreR (off=0x%lx) %s ---------\n", rd_section_offset, (core_r_exists ? "OK" : "IS CORRUPTED"));
		BUF_ADD("op       :%d, ch=%d, rv=%d, overeager=%s, container=0x%16llx\n",
				c->rd.op,
				c->rd.ch_type, c->rd.comp_code,
				c->rd.was_overeager ? "true" : "false",
				c->rd.container_ptr);
		for_each_core_writer_area(wr, c) {
			const union t_core_magic_area_cell *stamp;
			BUF_ADD("MagicArea:\n");
			for_each_stamp_in_magic_area(stamp, &wr->magic_area)
				BUF_ADD("0x%016llx %016llx %016llx %016llx\n", stamp->raw[0], stamp->raw[1], stamp->raw[2], stamp->raw[3]);
			BUF_ADD("\n");
		}
	}
	return cnt;
}

static inline ssize_t t_db_cmd_piggyback_to_string(const struct t_db_cmd_piggyback *s,
									 char*buf, ssize_t len)
{	// Note: This function is not equivalent of nvmeibc_rdma_intent_to_string()!
	ssize_t cnt = 0;
	bool is_lock;
	BUF_ADD("piggyback:addr=%llu, %d=", (u64)s->addr, s->op_type);
	if (s->op_type == 0xFF) {
		BUF_ADD("None\n");
		goto _out;
	}
	switch (s->op_type) { /* enum nvmeibc_rdma_intent */
	case  5: BUF_ADD("LOCK_read pigback,"); is_lock = true;  break;
	case  4: BUF_ADD("LOCK_read direct," ); is_lock = true;  break;
	case 11: BUF_ADD("BInf pigback,"     ); is_lock = false; break;
	case 10: BUF_ADD("Binf post cmd,"    ); is_lock = false; break;
	default: BUF_ADD("Unknown! Error,"   ); is_lock = false; break;
	}
	if (is_lock) BUF_ADD("lock(retries=%d)\n", s->val);
	else         BUF_ADD("write(val=0x%x)\n" , s->val);
_out:
	return cnt;
}

static inline ssize_t t_db_who_clnt_to_string(const struct t_db_who_clnt  *cl,
								char*buf, ssize_t len, const char* time_type)
{
	ssize_t cnt = 0;
	struct rtc_time t;
	rtc_time_to_tm(cl->time_secs, &t);
	BUF_ADD("clnt     :%s, git commit=%s\n", cl->name, cl->version);
	//BUF_ADD("Process  :%s, pgid=%u\n", cl->proc_name, cl->pgid);
	BUF_ADD("Time     :%04d-%02d-%02d %02d:%02d:%02d %s\n",
			(int)t.tm_year+1900, (int)t.tm_mon+1, (int)t.tm_mday,
			(int)t.tm_hour     , (int)t.tm_min  , (int)t.tm_sec,
			time_type);
	return cnt;
}

static inline ssize_t t_db_who_topo_to_string(const struct t_db_who_topo *t,
								char*buf, ssize_t len)
{
	ssize_t cnt = 0;
	BUF_ADD("topo     :[%llu.._%llu_..%llu), io_toggles=%lld\n",
		 (u64)(t->my_index + t->diff_head), (u64)t->my_index, (u64)(t->my_index - t->diff_last_freed),
		 (u64)t->num_io_toggles);
	return cnt;
}

static inline char __nvmeibtc_ds_owner_mode_to_chr(u32 t)
{ 	// t is enum NVMEIBTC_DS_OWNER_MODE, (!!!NOT enum nvmeibc_rdma_intent!!!)
	switch (t) {
	case 2:	return 'O';
	case 3:	return 'A';
	case 4: return 'S';
	case 5: return 'C';
	default:return '?';
	}
}

static inline char *__nvemibc_cmd_stage_to_string(const u32 stage) {
	switch (stage) {		// This is enum e_cmds_stage
		case 0: return "Pre Read";
		case 2: return "Write J";
		case 4: return "Do IO";
		default: return "Other";
	}

}

#define __cmd_dp_type(c)  ((c)->is_parity ? "Parity" : "Data")
#define __cmd_jpd_type(is_jour, c) (is_jour ? "Journal" : __cmd_dp_type(c))
#define bool_to_YN(b) ((b) ? "Yes" : "No")
static inline ssize_t t_db_who_cmd_to_string(const struct t_db_who_cmd  *c,
									char*buf, ssize_t len, const char *header)
{
	ssize_t cnt = 0;
	BUF_ADD("%-9s:cpu=%u, req=0x%llx dlba=%llu, len=%u, locks=%d\n",
			header, (u32)(c->req_id>>48), c->req_id,
			(u64)c->disk_address, (u32)c->nlbas, c->nlocks_take_before_cmd);

	BUF_ADD("%-9s:DorP?=%s, sent_to_disk=%s, first_rlba=%llu, stage=%s (%d)\n",
			"", __cmd_dp_type(c), bool_to_YN(!c->do_not_send),
			(u64)c->first_rlba, __nvemibc_cmd_stage_to_string(c->stage),
			(int)c->stage);

	return cnt;
}

#define __print_lock(l) (((l) != 0xF) ? (int)l : -1)

static inline ssize_t t_db_who_seg_to_string(const struct t_db_who_seg  *s,
								char*buf, ssize_t len, const char *header)
{
	const struct t_db_who_locks *l = &s->locks;
	ssize_t cnt = 0;
	const char l2_type = __nvmeibtc_ds_owner_mode_to_chr(l->second_type);
	const char l3_type = __nvmeibtc_ds_owner_mode_to_chr(l->third_type);
	BUF_ADD("%-9s:%s, acm=%d, sy=%d ", header, s->uuid, s->toma_acm, s->sync_safety);
	BUF_ADD("locks(O=%d,%c=%d,%c=%d)\n", l->owner_si, l2_type,
			__print_lock(l->second_si), l3_type, __print_lock(l->third_si));
	return cnt;
}

static inline ssize_t t_db_who_rlbalocks_to_string(const struct t_db_who_locks  *l,
								char*buf, ssize_t len, const char *header)
{
	ssize_t cnt = 0;
	const char l2_type = __nvmeibtc_ds_owner_mode_to_chr(l->second_type);
	const char l3_type = __nvmeibtc_ds_owner_mode_to_chr(l->third_type);
	BUF_ADD("%-9s:", header);
	BUF_ADD("locks(O=%d,%c=%d,%c=%d)\n", l->owner_si, l2_type,
			__print_lock(l->second_si), l3_type, __print_lock(l->third_si));
	return cnt;
}

static inline ssize_t t_db_who_praid_to_string(const struct t_db_who_praid *pr,
								char*buf, ssize_t len)
{
	ssize_t cnt = 0;
	BUF_ADD("raid     :toma_ver=0x%x, {D+P=%d+%d}, lock_id=0x%x\n",
			pr->version, pr->slice_size, pr->n_segs-pr->slice_size, pr->lock_id);
	return cnt;
}

static inline ssize_t t_db_who_hot_rec_to_string(const struct nvmeibc_dp_recovery_hot_dbgdi *hot,
												 char*buf, ssize_t len) {
	ssize_t cnt = 0;
	if (hot->magic == NVMEIBC_DP_HOT_RCVR_MAGIC) { // Valid hot recovery
		BUF_ADD("Hot Recovery Reason %s (%d)", nvmeibc_dp_recovery_hot_roll_fwd_reason_to_string(hot->reason), hot->reason);
	} else {
		/* NO HTR */
	}
	return cnt;
}

static inline ssize_t t_db_edic_check_to_string(const struct t_db_who_edic *edic,
												char*buf, ssize_t len)
{
	ssize_t cnt = 0;
	if (edic->edic_res == 0) {
		BUF_ADD("Edic check status was not filled\n");
	} else if (edic->edic_res == 'N') {
		BUF_ADD("Edic was not checked for rlba=%llu\n", (u64)edic->rlba);
		if (edic->read_crc != edic->calc_crc) {
			BUF_ADD("Data was never written at edic check time\n");
		} else {
			BUF_ADD("Edic check is disabled for this Client\n");
		}
	} else {	// 'P'assed, 'F'ailed
		BUF_ADD("Edic was checked: rlba=%llu, read_edic=0x%x, calc_edic=0x%x\n", (u64)edic->rlba, edic->read_crc, edic->calc_crc);
		BUF_ADD("Final result edic=%c\n", edic->edic_res);
	}
	return cnt;
}

static inline ssize_t t_db_metadata_to_string(const void *meta_data, char*buf, ssize_t len, const char* cmd_type)
{	// Reimplementation of nbdpec_md_to_string()
	const union nvmeibc_block_dp_ec_data_block_md *md = meta_data;
	ssize_t cnt = 0;
	BUF_ADD("metdata-%c:raw=0x%llx, ", cmd_type[0], md->raw);
	if (cmd_type[0] == 'J') {
		struct jblock_md_decompressed jmd = nvmeibc_block_dp_ec_jmd_decode((const union jblock_md *)meta_data);
		BUF_ADD("TxID=0x%x, V=0x%x, J2D=0x%llx=%llu, TxBM=0x%x, has_next=0x%x, has_prev=0x%x\n", jmd.tx_id, jmd.version, jmd.j2d, jmd.j2d, jmd.tx_bmp, jmd.has_next, jblock_md_prev_link(&jmd));
	} else {
		BUF_ADD("version=%d, ", md->D.version);
		#ifdef DEBUG_SAVE_JENTRY
		BUF_ADD("d2j_rng=%d, ", md->D.d2j_rng);
		#endif
		if (cmd_type[0] == 'D')
			BUF_ADD("edic=0x%08x, ", md->D.edic);
		else
			BUF_ADD("edic=0x%08x, dbits=(%d, %d), ", md->P.edic, md->P.dbits_0, md->P.dbits_1);
		BUF_ADD("TxID=0x%x, JRI=0x%x - State=%s\n", md->tx_id, md->jri,
				nvmeibc_data_written_state_tostring(nbdpec_md_get_data_written_state(md)));
	}
	return cnt;
}

#define vl_type_to_str(vt) ((vt == 'E') ? "EC" : ((vt == '1') ? "R1": "??"))
static inline ssize_t advanced_analysis_to_string(char*buf, ssize_t len, data_analysis *an)
{
	ssize_t cnt = 0;

	BUF_ADD("------- Auto Analysis ---------\n");
	BUF_ADD("Volume is %s {D+P=%d+%d}\n", vl_type_to_str(an->vol_type),
			an->D, an->P);

	if ((an->wr_exists)&&(an->r_exists)) {	// Compare Write and Read
		an->diff_segs = strcmp((char *)an->writer.seg.uuid, (char *)an->reader.seg.uuid);
		an->diff_clnt = strcmp((char*)an->writer.clnt.name, (char*)an->reader.clnt.name);
		if (an->diff_segs) {
			BUF_ADD("Read seg %s and Write Seg %s differ\n", an->writer.seg.uuid, an->reader.seg.uuid);
		}
		{
			u64 wr_addr;
			const u64 rd_addr = an->reader.cmd.disk_address + an->reader.sgl.offset_from_cmd;
			const u64 wr_vlba = an->writer.who_bio.start_vlba;
			const u32 wr_nlbas = an->writer.who_bio.nlbas;
			const u64 rd_vlba = an->reader.who_bio.start_vlba;
			const u32 rd_nlbas = an->reader.who_bio.nlbas;

			if (!an->is_journal_cmd)
				wr_addr = an->writer.cmd.disk_address + an->writer.sgl.offset_from_cmd;
			else
				wr_addr = nvmeibc_block_dp_ec_jmd_decode_j2d_only(((const union jblock_md *)&an->writer.jrnl.jour_md));

			if (wr_vlba != rd_vlba) {
				BUF_ADD("Read vlba (%llu) is not same as Write vlba (%llu)", rd_vlba, wr_vlba);
				if (wr_vlba > rd_vlba+rd_nlbas) {
					BUF_ADD(" Write start is greater than the Read operation range [%llu->%llu]\n",
							rd_vlba, rd_vlba + rd_nlbas - 1);
				} else if ((wr_vlba + wr_nlbas) < rd_vlba) {
					BUF_ADD(" Read start is greater than the Write operation range [%llu->%llu]\n",
							wr_vlba, wr_vlba + wr_nlbas - 1);
				} else {
					const u64 delta = (rd_vlba > wr_vlba) ? rd_vlba - wr_vlba : wr_vlba - rd_vlba;
					const u32 _32_slices = (an->D * 32);
					if (delta >= _32_slices) {
						BUF_ADD(" the difference between start of read and write (%llu) is greater than a full blkset (%d)\n",
								delta, _32_slices);
					} else {
						BUF_ADD("\n");
					}
				}
			}
			if (wr_addr != rd_addr) {
				const s64 diff = ((s64)wr_addr - (s64)rd_addr);
				const u64 absdiff =  (u64)((wr_addr > rd_addr) ? diff : -diff);
				BUF_ADD("Error: Block addresses corruption w(%llu) != r(%llu)!!!"
						" diff=%llu=0x%llx\n",
						wr_addr, rd_addr, absdiff, absdiff);
			}
		}
	}

	if (an->sync_exists) {
		if (an->multiple_sync)
			BUF_ADD("few sync histories found, please take notice");
		if (an->vol_type == 'E') {
			if (an->sync.op == NVMEIB_BLOCK_IO_OP_RECOVER_DB) {
				BUF_ADD("DB Recovery on block dlba=%llu\n", (u64)an->sync.orig_rldr.disk_address);
			} else if (an->sync.op == NVMEIB_BLOCK_IO_OP_RECOVER_STALE) {
				if (an->is_journal_cmd){
					if (an->hot_recovery.magic != NVMEIBC_DP_HOT_RCVR_MAGIC) {
						BUF_ADD("Sync op is HTR but magic is missing\n");
					} else if (an->hot_recovery.reason == NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_ROLL_FWD)
						BUF_ADD("Roll Fwd-Occured on block dlba=%llu\n", (u64)an->sync.orig_rldr.disk_address);
				} else if (an->sync.orig_seg.toma_acm == 1 /* RW */) {
					if (an->sync.orig_rldr.is_parity) {
						BUF_ADD("HTR: Updated Parity MD dbits\n");
					} else  {
						BUF_ADD("????\n");
					}
				} else {
					BUF_ADD("HTR: Regenerated %s block using reed-solomon\n",
							((!an->sync.orig_rldr.is_parity) ? "Data" : "Parity"));
				}
			} else if (an->sync.op == NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL) {
					BUF_ADD("Read Fail: Regenerated %s block using reed-solomon\n",
							((!an->sync.orig_rldr.is_parity) ? "Data" : "Parity"));
			} else {
			}
			/* Todo: Analyze TxID illegal values */
		}
	}
#ifdef ADVANCED_PLUS_DATA_ANALYSIS_SUPPORTED
	if (an->sync.op == NVMEIB_BLOCK_IO_OP_RECOVER_STALE)
		BUF_ADD("--- Stale Sync Overriden Data %s%s ---\n", (an->sync_overwritten_never_written ? "NEVER WRITTEN (might also be corrupted)" :
										(an->sync_overwritten_wr_exists ? "OK" : "IS CORRUPTED")),
#endif
	return cnt;
}

static inline ssize_t __t_db_recovery_to_string(const struct t_db_who_recovery *rec,
												char*buf, ssize_t len)
{
	ssize_t cnt = 0;
	switch (rec->magic) {
	case DBG_DI_MAGIC_RF: BUF_ADD("------- Read Fail  ---------\n"); break;
	case DBG_DI_MAGIC_DB: BUF_ADD("------- Dirty Bit  ---------\n"); break;
	case DBG_DI_MAGIC_ST: BUF_ADD("------- Stale      ---------\n"); break;
	case DBG_DI_MAGIC_RB: BUF_ADD("------- Rollback   ---------\n"); break;
	case DBG_DI_MAGIC_SC: BUF_ADD("------- Scrubbing  ---------\n"); break;
	case DBG_DI_MAGIC_WA: BUF_ADD("------- TXID wrap  ---------\n"); break;
	default: return cnt;
	}
	cnt += t_db_who_clnt_to_string(&rec->clnt, buf + cnt, len - cnt, "Sync wrote");
	cnt += t_db_who_seg_to_string(&rec->seg, buf + cnt, len - cnt, "segment");
	cnt += t_db_who_praid_to_string(&rec->raid, buf + cnt, len - cnt);
	cnt += t_db_who_topo_to_string(&rec->topo, buf + cnt, len - cnt);
	cnt += t_db_who_cmd_to_string(&rec->cmd, buf + cnt, len - cnt, "cmd");
	return cnt;
}

static inline const char* __nvmeib_data_reuse_to_string(/*enum nvmeib_data_reuse_buf_enum*/u8 value)
{
	static const char* values[] = {"ignore", "save", "send_release", "<unknown>"};
	const int index = (value < 4) ? value : 3;
	return values[index];
}

static inline ssize_t t_db_who_bio_to_string(const struct t_db_who_bio *bio, char* buf, ssize_t len)
{
	ssize_t cnt = 0;
	BUF_ADD("bio(%2d)  :volume=%s, vlba=%u, len=%u\n", bio->op, bio->volname, bio->start_vlba, bio->nlbas);
	return cnt;
}

static inline ssize_t t_db_who_writer_to_string(const struct t_db_who_writer *writer, char* buf, ssize_t len, data_analysis *an) {
	ssize_t cnt = 0;
	const struct t_db_who_clnt  *cl = &writer->clnt;
	const struct t_db_who_seg  *seg = &writer->seg;
	const struct t_db_who_locks *locks = &writer->rlbalocks;
	const struct t_db_who_praid *pr = &writer->raid;
	const struct t_db_who_topo  *t  = &writer->topo;
	const struct t_db_who_cmd   *c  = &writer->cmd;
	const struct t_db_who_sgl  *sgl = &writer->sgl;
	const struct t_db_who_bio  *bio = &writer->who_bio;
	u64 blk_dlba;

	cnt += t_db_who_clnt_to_string(cl, buf + cnt, len - cnt, "write started");
	cnt += t_db_who_bio_to_string(bio, buf + cnt, len - cnt);
	cnt += t_db_who_seg_to_string(seg, buf + cnt, len - cnt, "segment");
	cnt += t_db_who_rlbalocks_to_string(locks, buf + cnt, len - cnt, "rlbalocks");
	cnt += t_db_who_praid_to_string(pr, buf + cnt, len - cnt);
	cnt += t_db_who_topo_to_string(t, buf + cnt, len - cnt);
	cnt += t_db_who_cmd_to_string( c, buf + cnt, len - cnt, "cmd");
	blk_dlba = (c->disk_address + sgl->offset_from_cmd);		// Command addr + offset due to sgl
	BUF_ADD("sgl      :%d.%d of %d, disk_addr=%llu\n",
			sgl->sg_index, sgl->sub_index, sgl->n_sg_elements, blk_dlba);
	cnt += t_db_cmd_piggyback_to_string(&c->pigb, buf + cnt, len - cnt);

	if (writer->jrnl.is_valid) {
		an->is_journal_cmd = (writer->jrnl.disk_addr == c->disk_address);
		if (!an->is_journal_cmd) {
			BUF_ADD("Written into journal, but addresses are not aligned: jrnl (%llu), data (%llu)\n",
					(u64)writer->jrnl.disk_addr, (u64)c->disk_address);
		}
		blk_dlba = (writer->jrnl.disk_addr + sgl->offset_from_cmd); // Command addr + offset due to sgl
		BUF_ADD("journal  :disk_addr=%llu, Jri=%u, Jblock=%u, Binje=%u, Jentry=%u, action=%s(%u) I am %s block\n",
			blk_dlba, writer->jrnl.jri, writer->jrnl.jblock, 1 << writer->jrnl.binje_shift, writer->jrnl.jblock >> writer->jrnl.binje_shift,
			__nvmeib_data_reuse_to_string(writer->jrnl.action), writer->jrnl.action,
			__cmd_jpd_type(an->is_journal_cmd, c));
		cnt += t_db_metadata_to_string(&writer->jrnl.jour_md, buf + cnt, len - cnt,
										__cmd_jpd_type(true, c));
	}

	cnt += t_db_metadata_to_string(&writer->data_md, buf + cnt, len - cnt,
								   __cmd_dp_type(c));

	return cnt;
}

static inline ssize_t t_db_who_reader_to_string(const struct t_db_who_reader *reader, char* buf, ssize_t len,  __attribute__ ((unused))data_analysis *an)
{
	ssize_t cnt = 0;
	const struct t_db_who_clnt  *cl = &reader->clnt;
	const struct t_db_who_seg  *seg = &reader->seg;
	const struct t_db_who_locks *locks = &reader->rlbalocks;
	const struct t_db_who_praid *pr = &reader->raid;
	const struct t_db_who_topo  *t  = &reader->topo;
	const struct t_db_who_cmd   *c  = &reader->cmd;
	const struct t_db_who_sgl  *sgl = &reader->sgl;
	const struct t_db_who_bio  *bio = &reader->who_bio;

	BUF_ADD("comp_code: %d\n", reader->comp_code);
	cnt += t_db_who_clnt_to_string(cl, buf + cnt, len - cnt, "read completed");
	cnt += t_db_who_bio_to_string(bio, buf + cnt, len - cnt);
	cnt += t_db_who_seg_to_string(seg, buf + cnt, len - cnt, "segment");
	cnt += t_db_who_rlbalocks_to_string(locks, buf + cnt, len - cnt, "rlbalocks");
	cnt += t_db_who_praid_to_string(pr, buf + cnt, len - cnt);
	cnt += t_db_who_topo_to_string(t, buf + cnt, len - cnt);
	cnt += t_db_who_cmd_to_string( c, buf + cnt, len - cnt, "cmd");
	BUF_ADD("sgl      :%d.%d of %d, disk_addr=%llu\n",
			sgl->sg_index, sgl->sub_index, sgl->n_sg_elements,
			(u64)c->disk_address + sgl->offset_from_cmd);
	cnt += t_db_cmd_piggyback_to_string(&c->pigb, buf + cnt, len - cnt);
	cnt += t_db_metadata_to_string(&reader->meta_data, buf + cnt, len - cnt,
								   __cmd_jpd_type(false, c));

	return cnt;
}

static inline ssize_t t_db_who_sync_to_string(const struct t_db_who_sync *sync, char* buf, ssize_t len, data_analysis *an)
{
	ssize_t cnt = 0;
	const struct t_db_who_clnt  *cl = &sync->clnt;
	const struct t_db_who_cmd   *c  = &sync->orig_rldr;
	const struct t_db_who_seg  *seg = &sync->orig_seg;
	const struct t_db_who_topo  *t  = &sync->topo;
	const struct t_db_who_praid *pr = &sync->raid;

	BUF_ADD("Syn(0x%2x):seg=%d, reason=%x\n", sync->op,
			sync->source_seg, sync->reason);
	cnt += t_db_who_clnt_to_string(cl, buf + cnt, len - cnt, "sync-wr started");
	cnt += t_db_who_seg_to_string(seg, buf + cnt, len - cnt, ((an->vol_type == 'E') ? "WriteSeg" : "OwnerSeg"));
	cnt += t_db_who_praid_to_string(pr, buf + cnt, len - cnt);
	cnt += t_db_who_topo_to_string(t, buf + cnt, len - cnt);
	cnt += t_db_who_cmd_to_string( c, buf + cnt, len - cnt, ((an->vol_type == 'E') ? "SyncCmd" : "ReadCmd"));
	cnt += t_db_cmd_piggyback_to_string(&c->pigb, buf + cnt, len - cnt);
	cnt += t_db_metadata_to_string(&sync->meta_data, buf + cnt, len - cnt, __cmd_dp_type(c));

	return cnt;
}


static inline ssize_t t_db_who_mtv_to_string(const struct t_db_who_mtv *mtv, mtv_data_analysis *an, char* buf, ssize_t len)
{
	ssize_t cnt = 0;
	const struct t_db_who_mtv_writer *writer = &mtv->writer;
	const struct t_db_who_mtv_reader *reader = &mtv->reader;
	bool wr_exists, r_exists;

	(void)an;
	(void)r_exists;

	if (mtv->magic != DBG_DI_MAGIC_MO)
		goto _out;

	wr_exists = (writer->magic == DBG_DI_MAGIC_WR);
	if (wr_exists) {
		cnt += t_db_who_clnt_to_string(&writer->clnt, buf + cnt, len - cnt, "write started");
		cnt += t_db_who_bio_to_string(&writer->bio, buf + cnt, len - cnt);
	}

	r_exists = data_blk_does_reader_exists(*reader);
	if (r_exists) {
		cnt += t_db_who_clnt_to_string(&reader->clnt, buf + cnt, len - cnt, "write started");
		cnt += t_db_who_bio_to_string(&reader->bio, buf + cnt, len - cnt);
	}

_out:
	return cnt;
}

static inline void dump_dbgdi_log_record(struct dbgdi_log_entry *e, data_analysis *an, char *buf, ssize_t len, ssize_t *__cnt)
{
	ssize_t cnt = *__cnt;
	data_analysis dummy_an = {0};

	switch (e->type) {
	case DBG_DI_WRITE:
#ifdef ADVANCED_PLUS_DATA_ANALYSIS_SUPPORTED
		BUF_ADD("--- Written Data %s%s ---\n",  (an->read_invalid ? "BLOCK MARKED INVALID IN MD" :
									(an->block_restored ? "IS RESTORED" :
									(an->never_written ? "NEVER WRITTEN" :
									(an->wr_exists ? "OK" : "IS CORRUPTED")))),
									(an->read_failed ? ", READ wasnt sent" : ""));
#else
		BUF_ADD("------- Writer ---------\n");
#endif
		cnt += t_db_who_writer_to_string((struct t_db_who_writer *)e, buf + cnt, len - cnt, an);
		break;

	case DBG_DI_SYNC_OVERWRITTEN:
		BUF_ADD("------- Sync Over Written ---------\n");
		cnt += t_db_who_writer_to_string((struct t_db_who_writer *)e, buf + cnt, len - cnt, &dummy_an);
		break;

	case DBG_DI_WRITE_RESTORED:
		BUF_ADD("------- Write restored ---------\n");
		break;

	case DBG_DI_WRITE_DESTROYED:
		BUF_ADD("------- Write destroyed ---------\n");
		break;

	case DBG_DI_READ:
		BUF_ADD("------- Reader ---------\n");
		cnt += t_db_who_reader_to_string((struct t_db_who_reader *)e, buf + cnt, len - cnt, an);
		break;

	case DBG_DI_EDIC:
		BUF_ADD("------- EDIC ---------\n");
		cnt += t_db_edic_check_to_string((struct t_db_who_edic *)e, buf + cnt, len - cnt);
		break;

	case DBG_DI_SYNC:
		BUF_ADD("------- Sync ---------\n");
		cnt += t_db_who_sync_to_string((struct t_db_who_sync *)e, buf + cnt, len - cnt, an);
		break;

	case DBG_DI_RECOVER_READ_FAIL:
	case DBG_DI_RECOVER_SCRUBBING:
	case DBG_DI_RECOVER_DIRTY_BITS:
	case DBG_DI_RECOVER_STALE_LOCK:
	case DBG_DI_RECOVER_ROLLBACK:
	case DBG_DI_RECOVER_TXID_WRAPAROUND:
		cnt += __t_db_recovery_to_string((struct t_db_who_recovery *)e, buf + cnt, len - cnt);
		break;

	case DBG_DI_RECOVER_HOT:
		cnt += t_db_who_hot_rec_to_string((struct nvmeibc_dp_recovery_hot_dbgdi *)e, buf + cnt, len - cnt);
		break;

	case DBG_DI_MTV:
		cnt += t_db_who_mtv_to_string((struct t_db_who_mtv *)e, &an->mtv_an, buf + cnt, len - cnt);
		break;

	default:
		break;
	}

	*__cnt = cnt;
}

static inline void print_dbgdi_log_records(const struct dbgdi_log *log, data_analysis *an, char *buf, ssize_t len, ssize_t *cnt)
{
	int rc, loc = dbgdi_log_iter_init(log);
	u8 rec_buf[DBG_DI_MAX_REC_SIZE];
	struct dbgdi_log_entry *e;

	if (loc == -1) {
		return;
	}

	do {
		rc = dbgdi_log_get_rec(log, loc, rec_buf, DBG_DI_MAX_REC_SIZE);
		if (rc != -1) {
			e = db_entry(&rec_buf[0]);
			dump_dbgdi_log_record(e, an, buf, len, cnt);
		}
		loc = dbgdi_log_iter_next(log, loc);
		if (loc < 0)
			break;
	} while ((u32)loc != log->header.head);
}

static inline ssize_t data_blk_to_string(const data_blk *s, char *buf, ssize_t len)
{
	data_analysis an = {0};
	ssize_t cnt = 0;

	BUF_ADD("User Data: {0x%016llx,0x%016llx} inject size:%d[b]\n",
			s->dont_touch_original.data, s->dont_touch_original.aux[0],
			data_blk_get_injection_size());

	data_analysis_init(&an, s);

	print_dbgdi_log_records(&s->log, &an, buf, len, &cnt);

	cnt += t_core_dbgdi_to_string(&s->core, &an, buf + cnt, len - cnt);

	cnt += advanced_analysis_to_string(buf + cnt, len - cnt, &an);

	BUG_ON(cnt >= len);
	return cnt;
};

static inline int data_blk_cmp(const data_blk *s0, const data_blk *s1)
{
	if (s0->dont_touch_original.data != s1->dont_touch_original.data)
		return -1;
	if (s0->dont_touch_original.aux[0] != s1->dont_touch_original.aux[0])
		return -2;
	return 0;
}

#endif  // DBGDI_REMOVED_IN_PRODUCTION
#endif  // H beginning

