#ifndef NVMEIBC_PAUSABLE_H
#define NVMEIBC_PAUSABLE_H

#include "nvmeibc_block.h"
#include "nvmeibc_disk_locks.h"

/*
 * This file is an interim layer between the disk layer and the block layer.
 * Generally, this layer provides a wrapper for the disk operations that
 * enables pausing them safely.
 *
 * nvmeibc_pd_ is short for nvmeibc_pausable_disk_
 * This module, maintain counters of how many request are being transferred to
 * the disk.
 *
 * Flow:
 * 1. PAUSE interrupt arrives. Calls nvmeibc_pd_pause()
 * 2. disk->should_pause becomes true, meaning we must avoid from issuing new
 * io requests (locks, commands, etc). Each such request prevents the PAUSE.
 * 3. wait_for_pausing(), - wait for all the pause preventors to drop to zero
 * (each request was either imediately declined or is being transfered to the
 * target disk).
 * set disk->pausing = 1
 * 4. The above steps happen very fast in interrupt context.
 * Now we wait for all transfers to terminate (may have hundreds of them)
 * 5. Another call to nvmeibc_pd_pause() arrives not from interrupt context with
 * a callback we should call when all transfers complete.
 * 6. We schedule the callback to a list. Last transfer that terminates
 * (dec_transferring counter==0) notifies the transport layer via callbacks
 */

/***************************** Owner lock operations **************************/
int nvmeibc_pd_cmpxchg(struct nvmeibc_disk *disk, void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp);

int nvmeibc_pd_read_lock(struct nvmeibc_disk *disk, void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp);

/******************************** JMDC operations *****************************/
/* Cold EC Recovery: Read the entire jmdc of disk (all 2GB jri's). (jri = -1)
   (512K entries, each of 8B). 4[MB]+ unpacked, but packed to take less.
   Hot Recovery: Read specific chunk (index jri). 128*8B = 1[KB] of data ( */
int nvmeibc_pd_jmdc_read(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_jmdc_read_comp *comp);

/* Cold EC Recovery: Send bitmap of journal entries to be free'd to SERJIO */
int nvmeibc_pd_free_jrnl_ents(struct nvmeibc_disk *disk,
							  struct nvmeibc_disk_free_jrnl_ents_comp *comp);

/* DBG admin cmd used to immediately kill the remote side with coredump */
int nvmeibc_pd_dbg_please_kill_yourself(struct nvmeibc_disk *disk,
                                          void (*cb)(void*), void *ctx, int rsc_id, u64 dlba);

/********************* Transaction ID & Dbits operations **********************/
int nvmeibc_pd_write_blkset_info(struct nvmeibc_disk *disk, void *handle,
		u64 addr, struct nvmeibc_d_rdma_comp *comp);
//int nvmeibc_pd_read_blkset_info( struct nvmeibc_disk *disk, void *handle, u64 addr, struct nvmeibc_d_rdma_comp *comp);
int nvmeibc_pd_get_blkset_problems(struct nvmeibc_disk *disk, void *handle, u64 start,
	u64 length, struct nvmeibc_d_rdma_comp *dc);
/*************** IO cmds Read/Write/Trim (optional piggybacks) ****************/
/* Operation type set in @cmd->reqs[0].op, its valid values are:

   NVMEIB_BLOCK_IO_OP_READ/WRITE/DISCARD   :
        Send IO blocks (with optional metadata of EC)

   NVMEIB_BLOCK_IO_OP_WRITE_UNCOR
    	Destroy specific block on disk (make it bad sector)

   NVMEIB_BLOCK_IO_OP_MD_READ              :
        Send Request to read metadata of n contiguous disk-sectors such
        that total data len <= 128KB, to metadata buffer @cmd->reqs[0].md.
        Server read data+metadata but sends back only the metadata

   NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR         :
    	Send Request to Read-modify-write metadata of n contiguous disk-sectors
		such that total data len is within 1 blockset <= 128KB.
*/
int nvmeibc_pd_execute_io_blocks(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *cmd);

/* IO to journal blocks - Sets the potential journal-md piggyback info
   and calls nvmeibc_pd_execute_io_blocks() */
int nvmeibc_pd_execute_io_jour_blocks(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *cmd);

/* Excecute a generic command (non-IO) at Target over No-RDDA channel,
   see nvmeibc_disk_gen_cmd() */
int nvmeibc_pd_execute_gen(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_gen_cmd *cmd);

/********************** Async Events: Transport->Volume ***********************/
int nvmeibc_pd_pause(struct nvmeibc_disk *disk,
	void (*cb)(void *cntx), void *cntx);

/* Inverse of the above. Will never be called before disk actually paused */
int nvmeibc_pd_cont(struct nvmeibc_disk *disk);

/* Daniel: ugly, should think of better solution, must be called by block layer
   when completion arrives from transport (on rdma or command), to keep the
   count of in_transfers */
void nvmeibc_pd_cb_called_comp(struct nvmeibc_disk *disk,
	struct nvmeibc_d_rdma_comp *comp);

void nvmeibc_pd_cb_called_cmd(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_command *disk_cmd);

void nvmeibc_pd_cb_called_jmdc(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_jmdc_read_comp *comp);

void nvmeibc_pd_cb_called_free_jrnl_ents(struct nvmeibc_disk *disk,
					 struct nvmeibc_disk_free_jrnl_ents_comp *comp);

/*********************************** Debug ************************************/
void nvmeibc_pd_dump_transfers(struct nvmeibc_disk *disk);

int nvmeibc_pd_tostring(const struct nvmeibc_disk *disk,
	char *buf, int buf_len);

/*********************** JAM - transactional pausable *************************/
int  nvmeibc_pd_jam_get(struct nvmeibc_disk *disk);
void nvmeibc_pd_jam_put(struct nvmeibc_disk *disk);
int  nvmeibc_pd_jam_get_all(int n_disks, struct nvmeibc_disk *disks[]);
void nvmeibc_pd_jam_put_all(int n_disks, struct nvmeibc_disk *disks[]);

/************************** TOMA msgs & unsuscribe ****************************/
int nvmeibc_pd_toma_send(struct nvmeibc_disk *disk, u64 handle,
						 struct nvmeibc_disk_toma_send_params *params);
int nvmeibc_pd_toma_unsubscribe(struct nvmeibc_disk *disk, u64 handle);

/************************** Release transport-cookies *************************/
int nvmeibc_pd_reused_bb_release(struct nvmeibc_disk *disk,
								 struct nvmeib_data_reuse_buf_params *p);

#endif // NVMEIBC_PAUSABLE_H
