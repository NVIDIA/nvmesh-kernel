/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_B_CP_ABND_SRV_RESOURCE_H
#define NVMEIBC_B_CP_ABND_SRV_RESOURCE_H
/* loser = LOst SErver Resources, manager
Handles lost server resources on datapath errors. Resources are:
   - Locks which were not unlocked (locks siblings for each blockset)
   - Journal entries (grouped by segment). EC only.
This class gathers the lost resources until all IO stops and it is safe to
notify the server about the resources (UNREGISTER message)

Error in datapath causes transaction error. 3 cases:
1. Error during locks aquisition -> Attempt to release locks
2. Error during disk cmds while holding locks
2.1. Caused slice corruption (Some cmds possibly written but not all)
2.1.1 In EC, have journals -> Journals abandoned + Lock abandon
2.1.2 If dont have journals, Abandon only locks
2.2. No slice corruption (No data written, Journal possibly written) -> Attempt to release locks.
3. Error during locks release

If lock was not released due to error: Abandoned or failed release do
nvmeibc_topologies_rereg_seg(), to convert the lock to stale. Unregister msg
will be sent to Toma.

Error flow:
~~~~~~~~~~~
1. IO failure: Optionally abandond various journal entries on some disks,
2. IO marks on protection raid how many total abandoned journal entries exist:
   atomic_add() because few IO's may exist and IO may abandon stuff on a few
   protection raids
2.1 IO asks jams to notify the serjio abount abandoning journal, Each request
   about journal entry rises reference to topology by 1 and each reply from
   JAM decreases the reference by 1. Todo..........Rewrite this
3. IO marks on protection raid how many abandoned locks and failed to release
   locks.
4. Example: IO was on topology T3. It has 2 praids r5 with 5 unreleased locks
and r8 with 8 unreleased locks.
4. Ref-count on topology T3 decreases to zero.
4.1 Now it is guaranteed that all JAM reference to topologies were returned and
   Serjio knows about the abandoned jentries. It is safe to proceed to unregister
5. Unregister message is sent to raid r5 with amount of unreleased locks.
Those counters on r5 are zeroed. Counters on R8 are copied to next topology T4
using atomic_add().
6. T4 was also IOable with 6 abandoned locks. T4 sends unregister to R8 with
   counter of 8+6=14.

Scope:
~~~~~~
This struct has 1 copy for all dups of praids. It is stored in persistent
memory of praid. We might have a few IOable topologies and a few copies of
praid (possibly with different toma versions) due to switch topologies.
However it is guaraneed by Toma that client cannot have more than 1 lockid on
a praid so it is guaranteed that we have only 1 series of IOable topologies:
Example: Lets mark ti as non IOable and Ti as IOable:
    -Topologies list: t2->t3->t4->T5->T6->T7->T8->t9->t10->t11->t12
    -Head is t12. t4 got last register ACK and spawned t5 which is IOable.
     T7 got switch topo and spawend T8.
    -IO is running on T5...T8, and resources are abandoned on all 4 topologies
     but gathered in a single struct. When T8 will be freed it will send UNREG
     so t9 is not IOable.
    -T8 will notify server with the lost resources of topos T5..T8.
    -It is guaranteed that after t9 there is no higher topology which is IOable
     while T8 still exists. Otherwise sharing lost info of T8 and higher topo
     would be a bug coz lost info of higher topo would be notifed by T8.

Upon detach/reboot (when last topo is released), all counters must be 0.
*/

struct nvmeibc_b_cp_loser {					// Lost resources of this praid on disk
	spinlock_t lock;						// Insertion lock. A few IO's on different topos can loose resources in parallel
	atomic_t n_abandon_locks;				// Number of abandoned blocksets (protected by siblings of locks)
	atomic_t n_not_rel_locks;				// Number of fail to release blocksets
	//atomic_t n_abandon_jours;				// Number of abandoned journals for all blksets on all disks of this praid
	struct nvmeibs_lost_srv_resource_payload abandon_jours[N_MAX_RAID_SLICE_LEN];
};

void nvmeibc_b_cp_loser_init(   struct nvmeibc_b_cp_loser *);
void nvmeibc_b_cp_loser_destroy(struct nvmeibc_b_cp_loser *);

/* After notifying the server, clean the lost struct*/
void nvmeibc_b_cp_loser_clean( struct nvmeibc_b_cp_loser *);

/* Note: All function below can get param 'si' (segment index) but instead they
   get 'tr'. This is done for debug reasons (verify that that we LOSER does not
   act on inappropriate segment)*/
struct nvmeibc_subscription_ctx;
#define seg_ind const struct nvmeibc_subscription_ctx *

/* Datapath calls: Must hold spinlock before calling the funcs below.*/
void nvmeibc_b_cp_loser_aband_jour(  struct nvmeibc_b_cp_loser *, seg_ind tr, int jent, u8 jent_gen_id);
struct nvmeibc_cmd_lock;
void nvmeibc_b_cp_loser_aband_blkset(struct nvmeibc_b_cp_loser *, struct nvmeibc_cmd_lock *ow);

/* Debug function to abandon dummy resource 'd' on segment 'si' */
void nvmeibc_b_cp_loser_aband_dummy(struct nvmeibc_b_cp_loser *, seg_ind tr, u32 d);

/* Control path calls: Does not have to hold spinlock, coz only one instance
   is running per segment and no IO possible on this segment.
   Extract report of lost resources for a specific segment. If clean
   returns NULL. Can use this function to check for cleanness */
struct nvmeibs_lost_srv_resource_payload *nvmeibc_b_cp_loser_get_seg_report(struct nvmeibc_b_cp_loser *, seg_ind tr);
void nvmeibc_b_cp_loser_seg_clear(   struct nvmeibc_b_cp_loser *, int si);

/* Control path calls: Must hold spinlock, due to switch topologies. There is no
   IO on a seg but IO maybe enabled for other segs in praid so reporting praid
   info is cuncurent with IO. Todo: This function is not readty yet */
void nvmeibc_b_cp_loser_report_praid(struct nvmeibc_b_cp_loser *, void*dst_buf);
#undef seg_ind

#endif
