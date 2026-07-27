#ifndef NVMEIBC_DP_EC_RECOV_MAINTENANCE_H
#define NVMEIBC_DP_EC_RECOV_MAINTENANCE_H
/* Mintenance routins on wrap-around of counters
   Is called by IO when already holding all the necessary locks:
   1. Clean-up wrap-around of TxID
      Solution: Todo: Implement and describe
   2. Fixing TxID == 0 in RAM:
      Solution: Calculate maximum among all metadata TxID's
   3. Dbits-suspect: Occurs only during/after cold recovery
 */
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "block/nvmeibc_block_common.h"

/* Test if maintainance needed by IO op, and invokes it */
bool    dp_ec_mainten_has_txid_unreslvd(const struct nvmeibc_block_command *rldr);

extern bool qa_ec_stress_debug;

// Advance txid by increment. Return true if that results in wraparound
static inline bool dp_ec_mainten_next_txid_or_wrap(struct nvmeibc_block_command *rldr) {
	bool is_wraparound_needed = false;
	WARN(nvmeib_txid_never_write_to_disk(rldr->rld.pre.bits.txid), "Bug: pre_txid=0x%x\n", rldr->rld.pre.bits.txid);
	rldr->rld.post.bits.txid = rldr->rld.pre.bits.txid + 1; // For wraparound to be valid should only increment txid in 1 for each bio,  It's expected that all slices in multi-slice io will have the same txid.
	if (rldr->rld.post.bits.txid > NVMEIBC_DP_EC_MD_TX_ID_MAX) {
		is_wraparound_needed = true;
	} else if (qa_ec_stress_debug && rldr->rld.post.bits.txid > 0xff) {
		rldr->rld.post.bits.txid = NVMEIBC_DP_EC_MD_TX_ID_MAX; // In next write an wraparound will occur.
		rldr->rld.pre.bits.txid = rldr->rld.post.bits.txid - 1;
	}

	return is_wraparound_needed;
}

/*********************** DP maintain virtal functions *************************/
void dp_ec_mainten_reinit(    struct recovery_sync_op *so, enum nvmeib_block_io_op new_op);  // Same as prepare but convert existing 'so' to requested maintanace
void dp_maintenance_execute_op(struct recovery_sync_op *so);   // Called from thread context
void dp_ec_mainten_cb_stg_end(struct recovery_sync_op *so);   // Called from Interrupt context, last task (cmd/lock) completed (locks are already taken)

/*********** Ram-only-syncs (do not fix SSD disks, just fix RAM) ***************/
void dp_ec_mainten_blkset_recov_cb_stg_end(struct recovery_sync_op *so); // Notify Tomas/Serjios that blockset was recovered

#endif  // H beginning

