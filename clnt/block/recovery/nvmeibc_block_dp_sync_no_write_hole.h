/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_DP_SYNC_NO_WRITE_HOLE_H
#define NVMEIBC_BLOCK_DP_SYNC_NO_WRITE_HOLE_H
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.h"


/* EC No Write Hole Sync:
   Copy design document from here: https://docs.google.com/document/d/1v1uk-8fW8y8aEqZXmOrTj1cKvfYwAp_8NtwCe25RI8w/edit#
   Permanent read-failure recovery: Occures when read from nvme disk fails with
   permanent read fail (no retry) error. In this case we have to recosntruct the
   data from redundant disks, if possible.

   Unlike stale lock recovery, permanent read
   failure recovery is executed when locks are already taken. There are N types
   of permanent read failure recoveries
   1. Read fail of Data block from disk. IO holds locks and tried to read at
      least 1 data block that failed.
      Solution: Attempt to read the entire blockset, reconstruct the missing
      data from redundancy and write it back to disks. Give callback to original
      IO, which will decide if it must be retried or not.
   2. Same as above but block could not be recosntructed due to having at least
      P+1 missing slice members.
      Solution: Set the block to zeros, and write in its metadata that it is
      broken (a special bit). Fix the parities appropriately. Every read IO will
      test this bit and return an error to kernel appropriately.

LKJ: Do we take into account the topology? If P segments are dead, may be refusing the read or write could be a better idea.
     Let's disscusst this with Daniel, I think writes will not be refused (they can succeed), reads will end up with READ_FAILURE until fixed


   3. Read fail of Journal block. We had a write hole, Hot recovery started and
      is unable to read at least one of the journals
      Solution: Read the entire slice of data + journals and reconstruct the
      missing journal entry (block without metadata). Fill this block into the
      original sync's sglist and let the original sync continue its state
      machine

LKJ TODO(EC-400): This is not clear. It is possible only in few cases; Consider D2 & P writes, while Q is dead; What we should do in such case?
              All journal read fails are not exatcly documented, but we have a placeholder ticket for these


   4. Same as above, but journal block could not be reconstructed.
      Solution, much like case 2: Returns a zero block + metadata bit that it
	  is broken!

LKJ: Same as above do we take topology into account?


   5. Dirty-bits rebuild - In topology transition {Dead->Write} dirtybits are
      turned off and data is copied (exactly as if dead segment got read-fail).
      The difference is that Dirty-bits recovery has to acquire locks while
      read-fail assumes locks are already taken by caller
   6. Dbits-rebuild gets read-fail on source data and cannot reconstruct
      original-data.
	  Solution: Exactly like 2
   */

#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole_stats.h"

/*********************** No Write Hole SM ************************************/
void dp_sync_no_write_hole_cb_stg_end(	  struct recovery_sync_op *so);				// Called from Interrupt context, last task (cmd/lock) completed (locks are already taken)

// RV enumeration for read stage or datapath virtual functions:
enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE {
	GOTO_NEXT_STAGE = 0,					// Go to next stage (synchronously). Example: in Raid1 blocks were compared and writes of wrong blocks ware prepared
	NEXT_STAGE_SENT = 1,					// Next stage sent (asynchronously). Example: reed-solomon calculation offloading on hardware
	START_SLICE_BY_SLICE_MODE = 2,	// Slice by slice required (not enough sources to fix entire blockset)
	NO_WRITE_HOLE_DONE = 3				// Blockset is fully correct, nothing to fix. In EC-QLC: Carrier volume cannot access data to complete write hole, end recovery
};


/* PET trace declarations */
void pet_trace_binfo_commit_unexpected(const struct recovery_sync_op *so);
void pet_trace_binfo_commit_missing(const struct recovery_sync_op *so);
void pet_trace_binfo_entry_mismatch(const struct recovery_sync_op *so);
void pet_trace_nwhole_param_err(const struct recovery_sync_op *so);

#endif // NVMEIBC_BLOCK_DP_SYNC_NO_WRITE_HOLE_H
