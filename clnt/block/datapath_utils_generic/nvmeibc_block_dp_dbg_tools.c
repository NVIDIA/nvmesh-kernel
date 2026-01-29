/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_dbg_tools.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_operation.h"
#include "block/nvmeibc_block_common.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "nvmeib_utils_bin_traces.h"

#ifdef KR_UNDEF_H
#	undef KR_UNDEF_H
#endif
#include "kr_undef.h"

#ifdef DEBUG_UNCOMPLETED
DEFINE_SPINLOCK(uncompleted_list_lock);
LIST_HEAD(uncompleted_list);

void __uncompleted_cmds_list_rmv(struct nvmeibc_disk_io_command *cmds)
{
	unsigned long flags;
	spin_lock_irqsave(&uncompleted_list_lock, flags);
	list_del(&cmds->uncompleted_list_n);
	spin_unlock_irqrestore(&uncompleted_list_lock, flags);
}

void __uncompleted_cmds_list_add(struct nvmeibc_disk_io_command *cmds)
{
	unsigned long flags;
	_ND(__uncompleted_cmds_list_add_d1, "cmds->req_id=0x@_X", cmds->req_id);
	spin_lock_irqsave(&uncompleted_list_lock, flags);
	nvmeibc_block_cmd_status_debug(cmds, NVMEIBC_BLOCK_CMD_INIT);
	list_add(&cmds->uncompleted_list_n, &uncompleted_list);
	spin_unlock_irqrestore(&uncompleted_list_lock, flags);
}

void __uncompleted_cmds_list_send(struct nvmeibc_disk_io_command *cmds)
{
	cmds->iocmd_status = 15; // See __uncompleted_cmds_list_dump values.
}

void __uncompleted_cmds_list_comp(struct nvmeibc_disk_io_command *cmds, int val)
{
	_ND(__uncompleted_cmds_list_comp_d1, "cmds->req_id=0x@_X value=@INT", cmds->req_id, val);
}

void __uncompleted_cmds_list_dump(void)
{
	struct list_head *list_item;
	unsigned long flags;

	spin_lock_irqsave(&uncompleted_list_lock, flags);
	_NT(__uncompleted_cmds_list_dump_t1, "Uncompleted");
	list_for_each(list_item, &uncompleted_list) {
		struct nvmeibc_disk_io_command *cmd =
			container_of(list_item, struct nvmeibc_disk_io_command, uncompleted_list_n);
		struct nvmeibc_block_command *bcmd = container_of(cmd, struct nvmeibc_block_command, iocmd);
		struct nvmeibc_disk_io_command *leader_cmd;
		int nc = bcmd->ncmds, i;
		for (i = 0; i < nc; ++i, ++cmd) {
			if (i && (cmd->iocmd_status == NVMEIBC_BLOCK_CMD_NORDDA_SENT ||
					  cmd->iocmd_status == NVMEIBC_BLOCK_CMD_LOCAL_COMPLETED ||
					  cmd->iocmd_status == NVMEIBC_BLOCK_CMD_COMPLETED)) /* Skip the commands that seemed to have completed OK except for the leader. */
				continue;
			leader_cmd = &bcmd->cmdarr->iocmd;
			_NT(__uncompleted_cmds_list_dump_t2, "cmd=@PTR (@PTR) nlbas=@INT64 req_id=0x@_X (@INT64) op=@INT lba=0x@_X uncompleted=@INT/@INT/@INT state=@STR [sqe=@INT/@INT cqe=@INT/@INT wrap=@INT]",
					bcmd, bcmd->cmdarr, cmd->reqs[0].nlbas, cmd->req_id, leader_cmd->req_id,
					leader_cmd->reqs[0].op, leader_cmd->reqs[0].disk_address,
					i, nvmeibc_atomic_read(&bcmd->cmdarr->n_uncompleted_cmds), bcmd->cmdarr->ncmds,
					nvmeibc_block_cmd_status_to_str(cmd->iocmd_status),
					cmd->sq_entry, cmd->sq_entries, cmd->cq_entry, cmd->cq_entries, cmd->wraparound);
		}
	}
	_NT(__uncompleted_cmds_list_dump_t4, "Uncompleted Done");
	spin_unlock_irqrestore(&uncompleted_list_lock, flags);
}
#endif // DEBUG_UNCOMPLETED

static void __dump_operation_bio(const struct operation *o, nvmeib_trace_level trace_level){
	int i;
	for (i = 0; i < o->num_bios; ++i) {
		const struct bio_part* bp = o->bios[i];
		const s32 n_ref = nvmeibc_atomic_read(&bp->n_ref);
		_N_dmesg(trace_level, t_05_dp_dbg_tools, "o=@OPERATION bio_part=@BIO bio=@BIO start=@N_BYTES size=@N_BYTES vlba[bytes]=@VLBA_BYTES n_ref=@REFCOUNT ref=@BIO rv=@RV", o, bp, bp->bio, (bp->bio_offst << KERNEL_SECTOR_SHIFT), bp->size, (s64)(bp->start_vlba_s << KERNEL_SECTOR_SHIFT), n_ref, bp->ref, bp->rv);
	}
}

static void __dump_operation_rso(const struct operation *o, nvmeib_trace_level trace_level){
	const struct recovery_sync_op *rso = o->rso;
	_N_dmesg(trace_level,t_07_dp_dbg_tools, "o=@OPERATION", o);
	if (o->rso)
		_N_dmesg(trace_level, t_08_dp_dbg_tools, "self=@RECOVERY_OPERATION rso.o=@OPERATION, sync_request_link{next=@PTR prev=@PTR} @RLBA", rso, rso->o, rso->sync_request_link.next, rso->sync_request_link.prev, rso->rlba);
}

static void __dump_operation_cfg(const struct operation *o, nvmeib_trace_level trace_level){
	const int bio = nvmeibc_operation_is_bio(o) ? 1 : 0;
	const int rso = nvmeibc_operation_is_rso(o) ? 1 : 0;
	const int vro = nvmeibc_operation_is_vro(o) ? 1 : 0;
	const int slo = nvmeibc_operation_is_slo(o) ? 1 : 0;
	WARN_ON(1 != (bio+rso+vro+slo));
	if (bio){
		__dump_operation_bio(o, trace_level);
	} else if (rso){
		__dump_operation_rso(o, trace_level);
	} else if (vro){
		//__dump_operation_vro(o);				// Todo: add this for debugging, upon first need
	} else if (slo){
		//__dump_operation_slo(o);				// Todo: add this for debugging, upon first need
	}
}

/* The function below exists regarding of DEBUG_TOPO_CNTRS */
void __dump_operation_unsafe(const struct operation *o, const struct nvmeibc_block_command *_cmds, nvmeib_trace_level trace_level)
{
	const struct nvmeibc_block_command *cmds = (_cmds ? _cmds : o->cmds);	// Taking o->cmds is unsafe!
	const struct nvmeibc_cmd_lock *locksets = cmds? cmds->locksets: NULL;
	const struct nvmeibc_block_command *c;
	int i, nlocks = (locksets ? locksets->nlocks : -1);
	const bool use_stages = cmds && cmds->use_stages;

	__dump_operation_cfg(o, trace_level);
	if (o->mssa && o->cmds)
		nvmeibc_dump_mssa(o->mssa, o->op, nvmeibc_get_raid1_of_seg(o->cmds->ds), trace_level);

	_N_dmesg(trace_level, t_09_dp_dbg_tools,
		 "o=@OPERATION n_commands=@NCMDS locks=@LOCKSETS n_locks=@N_LOCKS, nuncomp=@NUNCOMP", o, cmds ? cmds->ncmds : 0,
		 locksets, nlocks, nvmeibc_atomic_read(&o->n_uncomp_raids));
	for (i = 0, c = cmds; cmds && i < cmds->ncmds; i++, c++) {
		const struct nvmeibc_block_io_req *io_req = &c->iocmd->reqs1;
		const u64 cmd_start = io_req->disk_address;
		const u64 cmd_end = cmd_start + c->nlbas;
		_N_dmesg(trace_level, t_0a_dp_dbg_tools,
			"cmd[@COMMAND_IDX|@RAID_LEADER_IDX] rv=@OPERATION_RV/@COMP_CODE opcode=@BLOCK_IO_OP stage=@MY_STAGE, send=@BOOL_YN, disk=@DISK_NAME:[@DLBA..@DLBA)",
			i, c->my_leader, c->o_rv, c->iocmd->comp.comp_code, io_req->op, c->my_stage, !c->do_not_send,
			c->ds->disk->name, cmd_start, cmd_end);
		if (dp_cmds_pigbck_has_any(c->iocmd)) {
			const struct nvmeibc_cmd_lock *l = c->iocmd->comp.pigbck_lock;
			const u32 val =  l ? (l->retries) : (u32)dp_cmds_get_piggyback_val(c);
			_N_dmesg(trace_level, t_0b1_dp_dbg_tools, "\tPiggyback=@DLBA val=@X", c->iocmd->lpb.addr, val);
		}
		if (use_stages) {
			if (dp_cmd_is_raid_leader(c)) {
				_N_dmesg(trace_level, t_0b2_dp_dbg_tools, "\tlocks_left=@N_LOCKS, nuncomp=@NUNCOMP, stage={@MY_STAGE..@MY_STAGE..@MY_STAGE}, flags{jr_lock=@BOOL_YN, io_append=@BOOL_YN, io_lock=@BOOL_YN}, binfo {pre_@BINFO, post_@BINFO}",
					nvmeibc_atomic_read(&c->nlocks), nvmeibc_atomic_read(&c->n_uncompleted_cmds), c->raid_first_stage, c->raid_cur_stage, c->raid_last_stage,
						  c->use_jr_apend_stage_data_lock, c->use_io_apend_stages, c->use_io_apend_stages_data_lock,
						  c->rld.pre.all, c->rld.post.all);
			} else {
				// Todo: Do we need more info here?
			}
		} else {
			_N_dmesg(trace_level, t_0b4_dp_dbg_tools, "\tlocks_left=@N_LOCKS nuncomp=@NUNCOMP", nvmeibc_atomic_read(&c->nlocks), nvmeibc_atomic_read(&c->n_uncompleted_cmds));
		}
	}
	for (i = 0; i < nlocks; i++) {
		const struct nvmeibc_cmd_lock *l = locksets + i;
		_N_dmesg(trace_level, t_0c_dp_dbg_tools, "lck[@LSI|ow=@LSI]) type=@LOCK_TYPE status=@LOCK_STATUS n_pending=@N_PENDING n_cmds=@NCMDS secondary=@LSI, disk=@DISK_NAME:@DLBA",
			i, l->owner_id, nvmeibc_rdma_intent_to_string(l->type),ncl_status_str(l->status),
			nvmeibc_atomic_read(&l->pending),nvmeibc_atomic_read(&l->ncmds), l->secondary_id,
			l->ds->disk->name, l->address);
	}
	if(cmds)
		nvmeibc_clmat_to_string(cmds);
}

void __dump_operation_report(const struct operation *o, nvmeib_trace_level trace_level)
{
	if(!o) goto out;

	_N_dmesg(T_BUG, __d_o_r_1,
		"o=@OPERATION op=@BLOCK_IO_OP, dbg_id=@O_DBG_ID, nd=@BDEV, @TOPOLOGY jiffies1=@_JIFFIES",
		o, o->op, o->dbg_id, o->nd, o->topo, o->jiffies1);

	__dump_operation_unsafe(o, NULL, trace_level);

out:
	return;
}

#ifdef DEBUG_TOPO_CNTRS

void __dump_operation(struct operation *o)
{
	const struct nvmeibc_block_command *cmds;
	int i;

	if (o->op >= NVMEIB_BLOCK_IO_OP_RECOVER_STALE) {
		__dump_operation_cfg(o, false);
		_NI_dmesg(t_0e_dp_dbg_tools, "o=@OPERATION, op=@BLOCK_IO_OP, no code for dumping yet", o, o->op);
		goto out;
	}
	for (i = 0; i < o->num_bios; ++i)
		_NI_dmesg(t_0f_dp_dbg_tools, "o=@OPERATION, op=@BLOCK_IO_OP bio=@BIO @TOPOLOGY d_jiffies=@D_JIFFIES commands=@COMMAND", o, o->op, o->bios[i], o->topo, jiffies - o->jiffies1, o->dbg_topo.cmds);
	if (!o->topo || !o->dbg_topo.cmds) {
		goto out;
	}

	if ((o->dbg_topo.new_cmds)&&(o->dbg_topo.new_cmds->cmdarr))
		cmds = o->dbg_topo.new_cmds;
	else
		cmds = o->dbg_topo.cmds;
	__dump_operation_unsafe(o, cmds, false);
out:;
}
#else
void __dump_operation(struct operation *o)
{
	(void)o;
}

#endif

#ifdef DEBUG_TOPO_CNTRS

void __debug_topo_print_uncompleted_op(const struct nvmeibc_cinst_params_blk *p, bool analyze_current_topo)
{
	struct t_block_clnt_globals *b = __get_from_params_blok_globals_container(p);
	struct nvmeibc_block_device *dev;
	struct nvmeibc_topology *t;
	struct debug_topo *dt;
	struct operation *o;
	struct nvmeibc_cmd_lock *l;
	struct list_head *t_item, *o_item, *t_o, *t_t;
	unsigned long flags;
	_NI_dmesg(t_10_dp_dbg_tools, "Printing stuck IO's / Locks");
	spin_lock_irqsave(&b->block_devices_sl, flags);
	list_for_each_entry(dev, &b->block_devices, list_n) {
		_NI_dmesg(t_11_dp_dbg_tools, "@DEV_NAME", dev->name);
		spin_lock(&dev->topologies.lock);
		list_for_each_safe(t_item, t_t, &dev->topologies.topologies) {
			t = container_of(t_item, struct nvmeibc_topology, list_n);
			if ((!t->newer) && (!analyze_current_topo)) {
				_NI_dmesg(t_12_dp_dbg_tools, "Skipping @TOPOLOGY (is_newest=@BOOL_YN, analyze_current_topo=@BOOL_YN)", t, !t->newer, analyze_current_topo);
				continue;	/* Do not analyze current topoolgy */
			}
			spin_lock(&t->dbg_tcntrs_lck);
			list_for_each_safe(o_item, t_o, &t->dbg_tcntrs) {
				dt = container_of(o_item, struct debug_topo, list);
				if (!dt->is_operation) {
					l = container_of(dt, struct nvmeibc_cmd_lock, dbg_topo);
					_NI_dmesg(t_13_dp_dbg_tools, "@DEV_NAME: @TOPOLOGY phased_out=@PHASED_OUT locks=@LOCKSETS n_uncompleted_locks=@N_UNCOMPLETED_LOCKS lock_type={@LOCK_TYPE_INT,@LOCK_TYPE_INT} lock_status={@LOCK_STATUS,@LOCK_STATUS}",
						dev->name, t, t->phased_out, l, nvmeibc_atomic_read(&l->n_uncompleted_locks),
						l[0].type, l[1].type, ncl_status_str(l[0].status), ncl_status_str(l[1].status));
					continue;
				}
				o = container_of(dt, struct operation, dbg_topo);
				_NI_dmesg(t_14_dp_dbg_tools, "@DEV_NAME: @TOPOLOGY o=@OPERATION", dev->name, t, o);
				__dump_operation(o);
			}
			spin_unlock(&t->dbg_tcntrs_lck);
		}
		spin_unlock(&dev->topologies.lock);

		if (0) { // print waiting IO's in per_cpu lists
			int cpu_id;
			for_each_allocated_cpu(cpu_id) {
				struct topo_percore_shared *tps = dev->topologies.percore_shared + cpu_id;
				spin_lock(&tps->list_access);
				list_for_each(o_item, &tps->io_wait_list) {
					o = container_of(o_item, struct operation, per_cpu_wait_list);
					_NI_dmesg(t_15_dp_dbg_tools, "@DEV_NAME: @CPU o=@OPERATION", dev->name, cpu_id, o);
					__dump_operation(o);
				}
				spin_unlock(&tps->list_access);
			}
		}

		if (analyze_current_topo) {
			int is_resubmit_empty = list_empty(&dev->dp.resub.list_paused_ops);
			spin_lock(&dev->dp.resub.lock);
			//TODO: resubmitter - no dmesg under spin lock
			if (!is_resubmit_empty)
				_NI_dmesg(t_16_dp_dbg_tools, "!list_empty(list_paused_ops)");
			o = list_first_entry_or_null(&dev->dp.resub.list_paused_ops, struct operation, list_paused);	//The next operation to execute in round-robin manner
			if (o) {
				__dump_operation(o);
			}
   			spin_unlock(&dev->dp.resub.lock);
		}
	}
	spin_unlock_irqrestore(&b->block_devices_sl, flags);
	_NI_dmesg(t_19_dp_dbg_tools, "Printing stuck IO's / Locks .. Done");
}

#endif

/* If locks are corrupted, data corruption will surrely occur. Must prevent */
void __invoke_crash_on_lock_corruption(
	struct nvmeibc_cmd_lock *locksets, int lock_i, const char* cmd, int action)
{
#ifdef DEBUG_LOCKS_CORRUPTION
	struct nvmeibc_cmd_lock *l = &locksets[lock_i];
	const int i_callbs = nvmeibc_atomic_add_return(action, &l->n_callbs);
	bool has_bug = 0;
	switch (cmd[0]) {
		case 't': case 'T': /* Take */
				if (action > 0)
					has_bug = (i_callbs > 1);	// Double callback
				else if (action < 0)// -1 = sync trying to retake secondary lock
					has_bug = (i_callbs != 0);	// Not taken
				else
					has_bug = true;
				break;
		case 'r': case 'R': /* Release */
				if (action>0)
				has_bug = ((action>0) && (i_callbs!=2));
				break;
		case 'f': case 'F': /* Free */
				has_bug = ((action>0) && (i_callbs!=2));
				break;
		case 'v': case 'V': /* View lock: Read operation can only view lock without taking it */
				has_bug = (i_callbs!=1);	// Viewing lock occures only after the last command
				break;
		case 'm': case 'M': /* Marker of transfer of locks between IO's */
				if (cmd[1] == 's' || cmd[1] == 'S') { // Start logs transfer
					has_bug = (i_callbs!=1);	// Only taken ,not released
				} else if (cmd[1] == 'e' || cmd[1] == 'E') {
					has_bug = (i_callbs!=0);	// Before lock is taken
				}
				break;
		case 'i': case 'I': {/* Init */
			int i;
			for (i=0; i<locksets[0].nlocks; i++)
				nvmeibc_atomic_set(&locksets[i].n_callbs, 0);
			break;
		}
		default:
			_NE_dmesg(error_dp_dbg_tools_invoke_crash_on_lock_corruption, "nvmeibc bug: @CMD_STR", cmd); BUG();
	}
	if (has_bug) {
		_NE_dmesg(error_1_dp_dbg_tools_invoke_crash_on_lock_corruption, "Serious problem. @LOCKSETS[@LSI],@ACTION_INT, @CMD_STR, @N_CALLBS",
		   locksets, lock_i, action, cmd, i_callbs);
		WARN_ON(true);
		nvmeibc_block_suspend(nvmeibc_disk_seg_to_bdev(locksets->ds), NULL, NULL);
	}
#else
	/* prevent gcc unused warning */
	(void)locksets; (void)lock_i; (void)cmd; (void)action;
#endif
}
