/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_NIC_DMA_ATOMICS_H
#define NVMEIBS_NIC_DMA_ATOMICS_H
/* Simulates the DMA operations that server side nic does. This API is called
 * by clients pausable layer when nvmeibc_disk sends the RDMA operations to
 * server.
 * Supported operations: cmpxchng 64 bits, read 64/32 bits, write 64/32bits.
 * 1. Activated via clients lock channel: {Locks, Dirtybits, TxID}
 * 2. Activated via piggybacks of RDMA operations on io cmds via io channel
 */

/* Non locks RDMA operations, on lock channel */
int serverRam_dma_do_db_txid( struct ramDiskSimulator* D, u64 addr  , struct nvmeibc_d_rdma_comp *dc);
/* RDMA operation over IO channel (could also be implemented on lock channel)*/
/* Same as 2 above but done from piggyback context (io channel). Cannot fail, no completion from transport */
void serverRam_dma_do_pigback(struct ramDiskSimulator* D,             struct nvmeibc_block_command *bcmd);
void serverRam_dma_set_jmdc(  struct ramDiskSimulator* D, u64 addr4k, const u64 val);

/* Locks */
int execute_owner_lock(       struct ramDiskSimulator* D, u64 addr,   struct nvmeibc_d_rdma_comp *dc);

/* Messages which are passed to server side no-rdda operations (including gen_cmds) */
struct nvmeibc_nic_rspreq {			// Response and request together for easier debugability
	struct volume_client_rsp rsp;
	struct volume_server_req req;
	struct serverSimulator*  S;		// On which server, stored for debug
};
void nvmeibc_nic_send_jam_free_abandoned(struct serverSimulator *S, struct nvmeibc_nic_rspreq   *rr);
int  nvmeibc_nic_send_jentry_erase(      struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gen_cmd);
int  nvmeibc_nic_get_jrng_by_uuid(       struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gen_cmd); // Pass gen command to server

#endif  // H beginning


