#ifndef NVMEIBC_BLOCK_DP_SYNC_TXID_WRAPAROUND_H
#define NVMEIBC_BLOCK_DP_SYNC_TXID_WRAPAROUND_H

struct recovery_sync_op;
void dp_ec_sync_txid_wraparound_cb_stg_end(struct recovery_sync_op *so);
void dp_ec_sync_txid_wraparound_execute_op(struct recovery_sync_op *so);

#endif // NVMEIBC_BLOCK_DP_SYNC_TXID_WRAPAROUND_H
