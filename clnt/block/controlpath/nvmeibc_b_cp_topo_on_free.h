/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_B_CP_TOPO_ON_FREE_H
#define NVMEIBC_B_CP_TOPO_ON_FREE_H

/* Serializes various resources / states of datapath with topologies.
   It is used each time when topology is freed because then we know that
   specific datapath users were drained.
   Responsible for:
    1. Notifying Server about lost resources only when they are not used
    2. Preventing CONT on disk, before IO's pre PAUSE were drained
    3. Scheduling msgs to specific seg/entire praid
    	3.1 Register message to all segs which are needed and not registered
    	3.2 Unregister message to entire praid
    	3.2 Switch topology to live segment
    	3.3 Switch topology to dead segment (acting as unregister) */

struct on_topo_free_message {									// Messages to send when topo is freed
	struct nvmeibc_disk_segment *seg;							// Pointer to the segment in this topology on which the message us sent
	enum NVMEIBT_CLIENT_MSG_TYPES type;							// Schedule message to toma. Will be sent when are done using the topology. Scheduled messages to Unregister/Register to/from both raid1 segments
	enum NVMEIBT_CLIENT_TR_REASON reason;						// For debug: Reason for sending the message to Toma
	bool never_reged_on_seg;									// UREG msg for seg that was never registered is sent with this flag to help Toma assert correctness
	bool append_loser;											// Should append lost server resource message
};

void on_topo_free_message_clear(struct on_topo_free_message *);

/******************************************************************************/
struct on_topo_free {											// Actions on a single praid, to take when topology is freed
	struct on_topo_free_message msgs[N_MAX_RAID_SLICE_LEN];		// Send message to each Toma in the protection raid
	struct nvmeibc_idisk *paused_disk;							// If topology was outdated via disk PAUSE, it must finish it's IO before allowing CONTINUE on that disk
};

/* Set cont preventer topology (on disk pause) and remove cont preventer when
   This topology is freed */
void on_topo_free_cont_preventer_inc(struct nvmeibc_topology *tcp,
										struct nvmeibc_idisk *disk, int rv);
void on_topo_free_cont_preventer_dec(struct nvmeibc_topology *tcp);

/* Must be called when topology is not used anymore by datapath and is about to
   get kfree(). sends all scheduled messages of a praid in 't'
   Assumes t is a valid topology with user_count > 0 */
void on_topo_free_send_shceduled(struct nvmeibc_topology *t);

/* Schedule UNREG/REG msgs of raid in on_free of t_old. 'tr' defines the raid.
   This function schedules msgs to all segments of praid (upon need). We either
   want to unregister or reregister the entire praid */
void on_topo_free_schedule_praid_ack(const struct nvmeibc_subscription_ctx *tr,
	struct nvmeibc_topology *t_new, struct nvmeibc_topology *t_old,
	const enum NVMEIBT_CLIENT_MSG_TYPES msg, enum NVMEIBT_CLIENT_TR_REASON reason);

/* Given a segment on NEW! and an old topology, schedule a single msg for a seg.
   Msg will be sent when old topo is released.
   1. UNREG msg must be sent on seg/r1 of old topology (because it
      exmplains with what clnt finished working.
   2. SWITCH topo ACK, sent on seg on new topology (because it explains with
      what clnt starts working.
   3. REG is sent on seg on new topology (request to work with new one)
   Summary: for topos list {t_old->t_new} messages 'want_to_use_t_new' and
   'done_using_t_old' are both sent when 't_old' is freeing but the difference
   is, about which topology the message talks. on_active complicates this even
   more. Read about it in dedicated .h file */
void on_topo_free_schedule_seg_msg(struct nvmeibc_disk_segment *seg_new,
	struct nvmeibc_topology *t_old,
	const enum NVMEIBT_CLIENT_MSG_TYPES msg, enum NVMEIBT_CLIENT_TR_REASON reason);

/* Unnecessary optimization, of the above, (Todo: remove), When detach (no IO)
   there is no reason to wait for topo free. Directly send the UNREG msg */
void on_topo_free_schedule_seg_msg_no_io(struct nvmeibc_disk_segment *seg,
	const enum NVMEIBT_CLIENT_MSG_TYPES msg, enum NVMEIBT_CLIENT_TR_REASON reason);

#endif

