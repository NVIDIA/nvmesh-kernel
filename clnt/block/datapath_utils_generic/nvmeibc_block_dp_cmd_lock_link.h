/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_CMD_LOCK_LINK_H
#define NVMEIBC_DP_CMD_LOCK_LINK_H
/* Commands - Locks linkage. Has 3 components
   1. for each lock -> amount of commands it protects
   2. for each command -> amount of locks it needs to be executed
   3. CLmat: 2D bit matrix describing wether cmd ci and lock li are linked */
#include "nvmeibc_block.h"		/* external API of the block */
#include "nvmeibc_block_dp_io_generic_cmds.h"
#include "nvmeibc_block_dp_operation.h"
#include "nvmeibc_block_dp_io_req_rel_locks.h"

/* Scan each L * C combination of {lock, cmd} to check for intersection.
 * Example: tripple mirroring write of 2 locksets on segments S0,S1,S2
 *        L0 (Ownr) on S0 | L1 (Act) on S1 | L2 (Own) on S1 | L3 (Act) on S0
 * -------------------------------------------------------------------------
 * c0(S0)         X       |      X         |                |
 * c1(S1)         X       |      X         |                |
 * c2(S2)         X       |      X         |                |
 * c3(S1)                 |                |     X          |     X
 * c4(S2)                 |                |     X          |     X
 * c5(S0)                 |                |     X          |     X
 * Command and Lock are intersecting if and only if:
 *  1. They belong to the same protection raid
 *  2. The range of lock/cmd relative to beggining of its segment intersect
 *     Example: L2 intersects with C3,C4,C5 even though C4, C5 not on S1
 *
 * Count sum of rows and sum of columns. ie:
 *  1. # of locks required for each command before it can be issued. Relevant
 *     only for first stage commands. Typically all siblings of owner lock are
 *     needed for each command, but long trims may need many other locks.
 *  2. # of commands dependent on each lock, so we know when a lock can be
 *     released. In our example we have 3 cmds C0,C1,C2 which need locks L0, L1
 *     So L0,L1 may be released after 3 commands finish. We sum all counters on
 *     owner lock so its value will be initialized to 6 (3+3). In multi stage
 *     commands, only the raid leader command will do atomic_sub(6) on the owner
 *     lock once raid leader execution finishes. If stages are not used each
 *     command will do, atomic_dec() on the owner for each lock (efectively,
 *     atomic_sub(2))
 * locks are released immediately when the commands they were protecting are
 * completed.
 *
 * Clmat:
 *  1. For multi-stage commands - maps link between raidleader and its locks.
 *     Rest of the commands just copy the linkage, for faster access in datapath
 *     even though it can be removed and all the siblings will probe the link
 *     via the raid leader.
 *  2. If stages are not used, each command is linked independently vs locks.
 *
 * Note: It is enough for 1 lock from all siblings protects a command to link
 *   all siblings of that command (even if sibling cmd is ec-journal which is
 *   written to a different disk area).
 * Non continous trim example: Trimming 2 blocks {3,37}. There are 4 commands
 *   and 4 locks where commands c2,c3 are linked to L0 even though they are
 *   not intersected. But they do with L3 which is merged to L1 which is sibling
 *	 of L0
 *        L0 (Ownr) on S0 | L1 (Act) on S1 | L2 (Own) on S0 | L3 (Act) on S0
 * -------------------------------------------------------------------------
 * c0(S0, Block 3)  X     |      X         |                |
 * c1(S1, Block 3)  X     |      X         |                |
 * c2(S0, Block37)  X     |      X         |       X        |       X
 * c3(S1, Block37)  X     |      X         |       X        |       X
 */

/****************************** CLmat methods *********************************/
int  nvmeibc_clmat_allocate(struct operation *o, bool use_kv_alloc, int n_locks, int n_cmds, int n_additional_bytes);		// Allocate Locks + CLmat + additional bytes for cmds
void nvmeibc_clmat_free(    struct operation *o);
void nvmeibc_clmat_free_dangling_locks(                                 struct nvmeibc_cmd_lock *locksets);	// Free Locks + CLmat, Todo: Refactor this. Locks get free after operation. Locks should always be allocated with clmat
bool nvmeibc_clmat_is_linked(const ulong *CLmat, int ci, int lsi, const struct nvmeibc_cmd_lock *locksets);
void nvmeibc_clmat_set_link(       ulong *CLmat, int ci, int lsi, const struct nvmeibc_cmd_lock *locksets);

/* Dump matrix to log: each command has a row, each lock has a column */
void nvmeibc_clmat_to_string(const struct nvmeibc_block_command *cmds);

/**************** Cllink = CLmat + cmds->nlocks + locks->ncmds ****************/
/* Find the first command (owner RW seg) which needs the given lock or find the
   first (owner) lock which protects this command. On error returns -1/NULL */
struct nvmeibc_block_command *
	nvmeibc_cllink_find_cmd_by_lock(struct nvmeibc_cmd_lock *locksets, int lsi);
int nvmeibc_cllink_find_lock_by_cmd(struct nvmeibc_block_command *cmd);

/* Do the full linkage of cmds (and optional trim new_cmds) to locks */
void nvmeibc_cllink_cmds_locksets(struct nvmeibc_block_command *cmds,
	struct nvmeibc_cmd_lock *locksets, struct nvmeibc_block_command *new_cmds);

/* When blockset was locked by 'locksets[ow_i]', trasnistion to cmds state machine. Command may need new mutliple blockset to be locked. In multi stage commands state machine, lauches the first stage */
void dp_transition_to_locked_cmds_sm(      struct nvmeibc_cmd_lock *locksets,          int ow_i);
/* When cmds[ci] state machine finished, transition to unlock state machine of locksets[ow_i]*/
void dp_transition_to_unlocked_blockset_sm(struct nvmeibc_block_command *cmds, int ci, int ow_i);

/************** Code for relinking commands after trim split *****************/
/* split_rv: negative - split failed, 1 - split not needed, 0 - split done */
void relink_trim_split_cmds(struct nvmeibc_cmd_lock *ls, int split_rv);

#define __cmd_start(c)   ((c).iocmd->reqs1.disk_address            )
#define __cmd_end(c)     ((c).iocmd->reqs1.disk_address + (c).nlbas)
#define __lock_start(l)  __from4K((l).address             )
#define __lock_end(l)    __from4K((l).address + __to4K(LOCKSET_SLICES))
#define __offset_from_seg(c)          ((c).ds->first_lba)

#endif  // H beginning

