/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block.h" // Must be first for simulator
#include "block/nvmeibc_topology.h"
#include "block/nvmeibc_block_common.h"
#include "block/controlpath/nvmeibc_b_cp_topo_common.h"

#define N_MSGS(t)	ARRAY_SIZE((t)->on_free.msgs)

void on_topo_free_message_clear(struct on_topo_free_message *otfm)
{
	otfm->type = NVMEIBT_CLIENT_MSG_ILLEGAL;
	otfm->seg = NULL;
	otfm->reason = NVMEIBT_CLIENT_TR_REASON_UNUSED;
	otfm->append_loser = false;
	otfm->never_reged_on_seg = false;
}

void on_topo_free_cont_preventer_inc(struct nvmeibc_topology *tcp,
										struct nvmeibc_disk *disk, int rv)
{
	const struct nvmeibc_topologies *nt = tcp->nt;
	int n_preventors;			// Just for debug
	BUG_ON(tcp->on_free.paused_disk);
	tcp->on_free.paused_disk = disk;
	n_preventors = atomic_inc_return(&disk->n_cont_preventors);
	BUG_ON(!nvmeibc_disk_should_pause(disk));
	wmb();	// if another thread free this topo right after we put it, it MUST see the 'paused_disk' which is not volatile.
	_NT(t_00_otfcpi, "disk @DISK_NAME: topo(@DEV_NAME:@TOPO_DBG_ID) ++preventors=@PREVENTORS, rv=@RV", nvmeibc_disk_get_name(disk), nt->device_name, tcp->debug_unique_index, n_preventors, rv);
}

static void __reset_proto_version_for_paused_disks(struct nvmeibc_topology *t)
{
	struct nvmeibc_disk *disk = t->on_free.paused_disk;
	struct nvmeibc_chunk *chunk;
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_disk_segment *seg;
	int c, r, si;
	topo_for_each_seg(t, chunk, c, r1, r, seg, si) {
		if ((seg->disk == disk)&&(is_toma_reg_valid(seg->toma_reg)))
			seg->toma_reg->protocol_version = NVMEIBT_CLIENT_PROTO_VERSION;
	}
}
void on_topo_free_cont_preventer_dec(struct nvmeibc_topology *t)
{
	if (t->on_free.paused_disk){  /* Guaranteed: No IO on this topo */
		const u64 t_index = t->debug_unique_index;
		struct nvmeibc_disk *disk = t->on_free.paused_disk;
		const int n_preventors = atomic_dec_return(&disk->n_cont_preventors);
		const bool waited_too_long = disk->n_cont_prevents_waited_too_long;
		_NT(t_01_otfcpd, "disk @DISK_NAME: topo(@DEV_NAME:@TOPO_DBG_ID) io_perm=@IO_PERM, --preventors=@PREVENTORS", nvmeibc_disk_get_name(disk),
		   t->nt->device_name, t_index, t->io_perm, n_preventors);
		if (waited_too_long) {
			unsigned long st_ents[16];
			struct nvmeib_stack_trace st = { .max_entries = ARRAY_SIZE(st_ents), .entries = st_ents, .skip = 0,};
			nvmeib_public_save_stack_trace(&st);
			nvmeibcb_dp_io_fail_mgr_blocked_cont(&nvmeibc_block_t_to_b(t)->dp.io_stats.mgr);
			_NE_TOPO(t_02_otfcpd, t, "I blocked cont for disk @STR, EC-4571 reproduction!", nvmeibc_disk_get_name(disk));
			_NE(     t_03_otfcpd, DMESG_PREFIX("@DEV_NAME") " Stack Trace:\n@STACK_TRACE", t->nt->device_name, &st);
		}
		WARN_TOPO((n_preventors < 0), t, "preventors=%d\n", n_preventors);
		__reset_proto_version_for_paused_disks(t);
	}
}

void on_topo_free_send_shceduled(struct nvmeibc_topology *t)
{
	//const struct nvmeibc_block_device* dev = nvmeibc_block_t_to_b(t);
	/* Todo: can't use nvmeibc_block_t_to_b(t) because reconf topologies is
	allocated on stack instead of inside nd */
	int si, end = N_MSGS(t);
	struct on_topo_free_message *otfm = t->on_free.msgs;
	struct nvmeibc_raid_topo_persistent *hdr = NULL;
	//_NT(on_topo_free_send_shceduled_t1, "t=@PTR of devices @STR", t, dev->name);
	for (si = 0; si < end; si++, otfm++) {
		if (otfm->seg && is_toma_reg_valid(otfm->seg->toma_reg)) {
			const struct nvmeibc_subscription_ctx *tr = otfm->seg->toma_reg;
			struct nvmeibs_lost_srv_resource_payload *lp = NULL;				// Loser payload
			struct nvmeibt_client_msg_pl *cp = NULL;						// Caser payload
			if (!hdr) 	// All segs have same hdr, cache it once
				hdr = tr->hdr;
			if (otfm->append_loser)
				lp = nvmeibc_b_cp_loser_get_seg_report(&hdr->loser, tr /*si*/);
			nvmeibc_toma_send_msg(otfm->seg, otfm->type, otfm->reason, cp, otfm->never_reged_on_seg, lp);
			if (lp) {	/* lp cannot exist for REG, cause it was reported on UNREG/SWITCH_TOPO */
				WARN_TOPO((otfm->type == NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT), t, "nvmeibc bug (%d,%d,%d) loser corrupted\n", tr->ch, tr->r1, tr->seg);
				nvmeibc_b_cp_loser_seg_clear(&hdr->loser, si);
			}
		}
	}
	on_topo_free_cont_preventer_dec(t);
}

static void __verify_no_scheduled_msgs(const struct nvmeibc_topology *t, int si)
{
	const struct on_topo_free_message *msg = &t->on_free.msgs[si];
	if (unlikely(msg->seg)) {
		const struct nvmeibc_subscription_ctx *tr = msg->seg->toma_reg;
		WARN_TOPO(1, t, "si=%d, msg=0x%x (%d,%d,%d)\n", si, msg->type, tr->ch, tr->r1, tr->seg);
	}
}

void on_topo_free_schedule_praid_ack(const struct nvmeibc_subscription_ctx *tr,
	struct nvmeibc_topology *t_new, struct nvmeibc_topology *t_old,
	const enum NVMEIBT_CLIENT_MSG_TYPES msg, enum NVMEIBT_CLIENT_TR_REASON reason)
{
	const int max_num_acks = N_MSGS(t_old);
	int si;
	struct nvmeibc_raid1 *r1_old = __get_r1_by_tr(t_old, tr);
	struct nvmeibc_disk_segment *seg_old = NULL;

	BUG_ON(!spin_is_locked(&t_old->nt->lock));
	WARN_TOPO(r1_old->replicas > max_num_acks, t_old, "no space for msgs, nseg=%d>acks=%d\n", r1_old->replicas, max_num_acks);
	raid1_for_each_seg(r1_old, seg_old, si) {
		__verify_no_scheduled_msgs(t_old, si);
	}
	if (msg == NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT) {
		raid1_for_each_seg(r1_old, seg_old, si) {
			struct on_topo_free_message *otfm = &t_old->on_free.msgs[si];
			const bool has_lockid = (r1_old->lid.all != LS_UNLOCKED);
			const bool was_active = is_seg_active(*seg_old);
			const char active_lockid_status = (has_lockid ? (was_active ? 'V' : '?') : 'X');		// V - Unregged, X - not unregged, ? - Oportunistic unreg
			/* Note: (has_lockid && !was_active) means clnt never
			   registered to this seg but maybe toma think it did and REG_ACK
			   is yet to arrive. Maybe clnt did register to other Tomas.
			   For assertness - send UNREG anyways with this flag to notify Toma
			   That this lock ID was never used */
			if (has_lockid) {
				_NT(t_06_otfcpd, "si=@SI unreg, append_loser=@BOOL_YN->@BOOL_YN", si, otfm->append_loser, was_active);
				otfm->type =     msg;
				otfm->seg =      seg_old;
				otfm->reason =   reason;
				otfm->append_loser = was_active;	// Else, loser is empty
				otfm->never_reged_on_seg = (!was_active);
			}
			_NT_TOPO(t_07_otfcpd, t_old, "UNR @ACT_CHR, seg=" SEGMENT_FMT " reason=@REASON", active_lockid_status, tr->ch, tr->r1, si, (u32)reason);
			nvmeibc_segment_clear_old_topo_upon_unreg(seg_old);
		}
		nvmeibc_raid1_clear_old_topo(r1_old /*, tr*/);
		nvmeibc_raid1_clear_new_topo_upon_unreg(__get_r1_by_tr(t_new, tr)); /* For future rereg */
	} else if (msg == NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT) {
		/* Todo: It is legal that r1_old->lid.all is not zero. May happen
		   if other segment was registered while we were processing this
		   segment and updated the ID of both. Clean this*/
		raid1_for_each_seg(r1_old, seg_old, si) {
			struct on_topo_free_message *otfm = &t_old->on_free.msgs[si];
			const bool was_active = is_seg_active(*seg_old);
			const bool disk_dying = nvmeibc_disk_should_pause(seg_old->disk);	// Msg will probably not reach Toma anyways
			char disk_dying_status;
			if ((!disk_dying)&&(!was_active)) {
				otfm->type =     msg;
				otfm->seg =      seg_old;
				otfm->reason =   reason;
			}
			if (0) { /* NO! dont call it, scheduled segs dont require cleaning
			           needed stuff was cleaned when transitioning from
					   old->new topos. If something left, it should be there */
				nvmeibc_segment_clear_b4_reg(__get_seg_by_tr(t_new, tr), __FUNCTION__);
			}
			disk_dying_status = ((!disk_dying) ? (was_active ? 'X' : 'V') : 'D');
			_NT_TOPO(trace_1_b_cp_topo_on_free_on_topo_free_schedule_praid_ack, t_old, "REG @DISK_DYING_STATUS, seg=" SEGMENT_FMT " reason=@REASON",
			   disk_dying_status, tr->ch, tr->r1, si, (u32)reason);
		}
	} else {
		WARN_TOPO(true, t_old, "unknown msg=0x%x\n", msg);
	}
}

static bool __verify_pre_schedule_sw_topo_ack(struct nvmeibc_disk_segment *seg_new,
	struct nvmeibc_topology *t_old)
{
	const struct nvmeibc_subscription_ctx *tr = seg_new->toma_reg;
	struct nvmeibc_disk_segment *seg_old = __get_seg_by_tr(t_old, tr);
	int si = tr->seg, do_schedule = true;
	struct on_topo_free_message *otf = &t_old->on_free.msgs[si];
	/* on May 17, 2017, we discussed with Ronen the case we get SWITCH_TOPO msg
	 * from a segment which becomes DEAD. we then need to choose the reply
	 * msg, either UNREGISTER (bcz it becomes DEAD) or SWITCH_TOPO_ACK (bcz
	 * we accept the request). Ronen requested we return the SW_TOPO_ACK reply*/
	if (unlikely(is_seg_active(*seg_old) &&
				 seg_old->toma_acm != NVMEIBTC_DS_MODE_DEAD &&
				 seg_new->toma_acm == NVMEIBTC_DS_MODE_DEAD)) {
		if (otf->seg) {	// Replace existing unregister with switch topo
			const bool notU = (otf->type != NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT);
			const bool notS = (otf->seg != seg_old);	// Corruption in scheduling mechanism
			_NT(trace_b_cp_topo_on_free_verify_pre_schedule_sw_topo_ack, "SW_TOPO_ACK replaces UNREG to DEAD seg=@SEG", seg_old->uuid);
			WARN_TOPO((notU||notS), t_old, "msg=0x%x(0x%x) otf_seg(%d)!=%d\n", otf->type, otf->reason, otf->seg->toma_reg->seg, si); //
			on_topo_free_message_clear(otf);  // delete msg and send SWITH_TOPO_ACK instead
		}
	} else if (unlikely(!is_seg_active(*seg_old))) {
		/* Issue of Sending SW_TOPO_ACK on unregistered segment. It is weired to
		   do so + clnt might already shceduled a REG msg to this seg.
		   on 04/12/2018 Daniel and Ronen agreed. SWITCH TOPO arrived from seg
		   on which client is not registered. Examples:
		   1. Delayed switch topo: while waiting for REG on D->W seg, clnt unreged
		   from the source RW 'seg'.
		   2. Inline switch topo: Clnt UNREG while Toma was sending SWITCH_TOPO.
		   Now after udpating the topo clnt has to send ACK on unregistered seg
		   Solution: If REG msg scheduled, clnt will send it (coz UNREG is
		   on the way at it will serve as SW_TOPO_ACK, else, send SW_TOPO_ACK
		   anyways even though we are unreged. REG msg is scheduled on a
		   different topo */
		if (otf->seg) {	// Reg MSG scheduled
			const bool notR = (otf->type != NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT);
			const bool notS = (otf->seg != seg_new);	// Corruption in scheduling mechanism
			_NT(trace_1_b_cp_topo_on_free_verify_pre_schedule_sw_topo_ack, "REG replaces SW_TOPO_ACK to seg=@SEG", seg_old->uuid);
			WARN_TOPO((notR||notS), t_old, "msg=0x%x(0x%x) otf_seg(%d)!=%d\n", otf->type, otf->reason, otf->seg->toma_reg->seg, si);
			otf->reason = NVMEIBT_CLIENT_RT_REASON_REG_ON_PR_UPDATE_SWTOPO;
			do_schedule = false;
		}
	} else {	// Likely: Just is_seg_active(*seg_old)
		__verify_no_scheduled_msgs(t_old, si);
	}
	return do_schedule;
}

void on_topo_free_schedule_seg_msg(struct nvmeibc_disk_segment *seg_new,
	struct nvmeibc_topology *t_old,
	const enum NVMEIBT_CLIENT_MSG_TYPES msg, enum NVMEIBT_CLIENT_TR_REASON reason)
{
	const int si = seg_new->toma_reg->seg;
	bool do_schedule = true;
	struct on_topo_free_message *otf = &t_old->on_free.msgs[si];
	if (msg == NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK) {
		do_schedule = __verify_pre_schedule_sw_topo_ack(seg_new, t_old);
		if (do_schedule) {
			const bool became_inactive = !is_seg_active(*seg_new); 	// Switch Topo may acts as unregister
			otf->seg = seg_new;
			_NT(t_01_otf_ssm, "si=@SI switch_topo, append_loser=@BOOL_YN->@BOOL_YN", si, otf->append_loser, became_inactive);
			otf->append_loser = became_inactive;
		}
	} else if (msg == NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT) {
		__verify_no_scheduled_msgs(t_old, si);
		otf->seg = __get_seg_by_tr(t_old, seg_new->toma_reg);
		_NT(t_02_otf_ssm, "si=@SI unreg, old append_loser=@BOOL_YN", si, otf->append_loser);
		otf->append_loser = true;	// Always requires loser info
		nvmeibc_segment_clear_old_topo_upon_unreg(otf->seg); /* seg_old*/
		nvmeibc_segment_clear_new_topo_upon_unreg(seg_new);
	} else if (msg == NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT) {
		__verify_no_scheduled_msgs(t_old, si);
		otf->seg = seg_new;
		nvmeibc_segment_clear_b4_reg(seg_new, NULL); // Must be called here!
	} else {
		WARN_TOPO(true, t_old, "unknown msg=0x%x\n", msg);
		do_schedule = false;
	}
	if (do_schedule) {
		otf->type = msg;
		otf->reason = reason;
	}
}

void on_topo_free_schedule_seg_msg_no_io(struct nvmeibc_disk_segment *seg,
	const enum NVMEIBT_CLIENT_MSG_TYPES msg, enum NVMEIBT_CLIENT_TR_REASON reason)
{
	struct nvmeibc_subscription_ctx *tr = seg->toma_reg;
	if (is_toma_reg_valid(tr)) {			// Initalization erro handling
		nvmeibc_segment_clear_old_topo_upon_unreg(seg);			// here seg acts as seg_old when scheduling msg
		if (is_seg_active(*seg)) {
			struct nvmeibc_raid_topo_persistent *hdr = tr->hdr;
			struct nvmeibs_lost_srv_resource_payload *lp = nvmeibc_b_cp_loser_get_seg_report(&hdr->loser, tr);
			nvmeibc_toma_send_msg(seg, msg, reason, NULL, false, lp);	// here seg acts as seg_old when old topo is freed
			if (lp)
				nvmeibc_b_cp_loser_seg_clear(&hdr->loser, tr->seg);
		}
	}
}
