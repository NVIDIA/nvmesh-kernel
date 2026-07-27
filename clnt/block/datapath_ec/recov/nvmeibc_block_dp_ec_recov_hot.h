#ifndef NVMEIBC_DP_EC_RECOV_HOT_H
#define NVMEIBC_DP_EC_RECOV_HOT_H

#include "kr_incs.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"

/* Hot Recovery of stale-locked blockset.
 *
 * This functionality is called after the client had registered to Toma
 * topology and acquired lock which reflects the real state of Dirty bits.
 *
 * We separate the logics to two cases:
 * 1. HTR's topology include RW parity seg of our blockset.
 *
 *    If Journal WAS committed (full journal on all TxBM segs)
 *  	o Roll forward journal to data (see: New Dbits ahead).
 *  	o Regenerate W segs data       (see: New Dbits ahead).
 *  	o Update Dbits in RW Parity's MD.
 *    In any case, notify (reachable) targets blockset was recovered,
 *    which shall free the corresponding journal. if any.
 *
 * 2. HTR's topology does NOT include RW parity seg of our blockset.
 *
 *    If any of these segs are W in HTR's topology, regenerate their
 *    entire blockset (128KB) from Data segs, slice-by-slice.
 *
 * New Dbits ahead:
 * Whenever HTR writes a parity seg, it also updates the PMD.Dbits such
 * that it clear/set dirty-mark of W/D seg, respectively. For W segs it
 * means that we clear the dirty-mark ahead of actually writing their data.
 * This approach is not expected to lose any information (for new IO,
 * next HTR or Cold Recovery) as long as we dont free the journal before
 * write to all W segs succeeds.
 *
 */

/********************** DP sync stale virtal functions ************************/
void dp_ec_sync_stale_execute_op(struct recovery_sync_op *so);
void dp_ec_sync_stale_cb_stg_end(struct nvmeibc_block_command *cmd); //Daniel: change to so!!!!! Called from Interrupt context, last task (cmd/lock) completed (locks are already taken)

#endif  // NVMEIBC_DP_EC_RECOV_HOT_H


