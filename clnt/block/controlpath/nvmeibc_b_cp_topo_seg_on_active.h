/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_B_CP_TOPO_SEG_ON_ACTIVE_H
#define NVMEIBC_B_CP_TOPO_SEG_ON_ACTIVE_H

/* Manages the on_active layer of topology

   1. Block layer has a list of topologies consisting of 4 topology types:
   old - list of old topologies upon which no new io/ nor toma messages arrive
   head- the head of the list. Latest topology of client (used for IO)
   calc- short term calculation. Taking head, applying changes and setting as
    	 new head
   on_active- Topology of segment which is held outside of the list. Used to
    		  register to toma and apply the topo in delayed fashion
    		  Example: R1: {RW,D} - > {RW,W} transition cannot occur before W
    		  seg is registered, or else client looses ability to issue IO

  2. on_active topology, unlike all other is stored as toma message (efectively
  delaying the processing of this message. Only specific types of messages
  can be delayed (only those which bear new topology in them).

  3. Each seg can point only to a single 'on_active' msg and this msg is shared
  between a few topologies in the list.

  Illustration of topologies list:
	  | .. old topos ....| Head |
	  t3 --> t4 --> t5 --> t6
			  \_____|______/
					|
    				\_on_active
  Now Head receives register ACK and 't7' is calculated:
    t7 = t6 + topo of on_active + info in REG_ACK msg

  4. In topology on ever protection raid at most (pr->n_segs - pr->slice_size)

  5. Note: You have to understand 'on_topo_free_message' before understanding
	how on_active is used.

  5. When client requests registration it uses the praid version of on_active
	topo instead of the 'head' topo. Hint: Not doing so would result in endless
    REG-NACK loop.
    A few mechanisms in control path have a 2 steps scheme (msg sent on topo ti
    is talking about ti+1).
    This 2 steps scheme reseble a bit the 'on_topo_free_message' but is
	radically different.
    on active: REG ack sent on t5 which talks about 'on_active' (which is even
    			more advanced than t7).
    on_free:   msg sent on ti, talks about ti or ti+1 (depending on msg type)

  Todos:
  1. The sharing of on_active is done without protection and there is no
     protection against usage after free in function:
     nvmeibc_seg_on_active_get_version_send().
     The flow of the protocol makes it very unlikely so this issue was never handled.
	 Rarely reproducable in the simulator
     Example: Thus after on_active was processed by t6 and deleted, t5
	 accesses it when sending acks before free.
  2. The solution to the above is either use additional refcount on on_active
     structure or extend 'enum nvmeibc_segment_shared_memo' mechanism (better
     solution). Thus on_active will not be freed after processing but will be
     freed by last topology which uses it (when t6 is freed, not during calc of
     t7).
   */

/* Struct to hold toma message for delayed processing. (When seg becomes active)
   when msg != NULL, we have a msg pending processng */
struct nvmeibc_segment_on_active {
	int pr_version;						// Cached version for access by tail topos (non head)
	struct nvmeibt_client_msg	*msg;	// A TOMA message to incorporate into the topology upon activation of the segment. Used only by head topo, Tood: Set to NULL in tail topos
	const struct nvmeibc_subscription_ctx 		*tr;	// the segment on which we've receivd the msg on and will send ack to
};

struct nvmeibc_disk_segment;
void nvmeibc_seg_on_active_init(struct nvmeibc_disk_segment *seg);

/*************** Methods called only by latest segment (head topo) *************/
/* Schedule 'msg' send on channel 'tr' to process when segment 'seg' becomes active */
int nvmeibc_seg_on_active_schedule(const struct nvmeibc_subscription_ctx *tr,
	const struct nvmeibt_client_msg *msg, int len, struct nvmeibc_disk_segment *seg);

/* Get version of on_active for processing of the message (calculating topo),
   If nothing scheduled, return 0 (illegal version). Else positive version */
#define nvmeibc_seg_on_active_get_version_calc(_dseg) \
	((_dseg)->on_active.msg ? (_dseg)->on_active.msg->thick.praid_version : 0)
#define nvmeibc_seg_on_active_get_msg(_dseg) (_dseg)->on_active.msg

/* Once segment 'seg' of raid 'pr' on new topo becomes active, and on_active msg
   was incorporated by head topo. schedule ack to the on active message msg.
   This ack will be sent upon free of 't_old' topo (topo which received
   the register ack) */
void nvmeibc_seg_on_active_sched_ack(const struct nvmeibc_subscription_ctx *tr, struct nvmeibc_raid1 *pr, struct nvmeibc_disk_segment *seg, struct nvmeibc_topology *t_old);

/* Note: Only HEAD of topology can access the msg itself because only it might
   need to apply the message. Tail topos cannot access the msg because head
   might be freeing it while in parallel
   need it to apply */
void nvmeibc_seg_on_active_free(struct nvmeibc_disk_segment *seg);

/*************** Methods called only by old (non head) topo *************/
/* Get version of on_active for sending msg to toma. Called by non head topo
   (by old topo being freed) */
int nvmeibc_seg_on_active_get_version_send(struct nvmeibc_raid1 *pr, struct nvmeibc_disk_segment *seg, enum NVMEIBT_CLIENT_MSG_TYPES msg_type);

#endif
