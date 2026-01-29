/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_TRANSPORT_API_H
#define NVMEIBC_BLOCK_TRANSPORT_API_H
#include "nvmeibc_disk_locks.h"
#include "nvmeibc_disk.h"

/* Defines the structures of cmds/completion between block layer and transport layer */
/********************** Disk IO Command Status Debug **************************/
#if defined(DEBUG_UNCOMPLETED)
enum nvmeibc_block_cmd_status {
	NVMEIBC_BLOCK_CMD_INIT 					= 0x0,

	/* Local */
	NVMEIBC_BLOCK_CMD_LOCAL					= 0x10,
	NVMEIBC_BLOCK_CMD_LOCAL_MD_TRIM_RD_COMP	= 0x11,
	NVMEIBC_BLOCK_CMD_LOCAL_COMPLETED		= 0x12,

	/* Remote */
	NVMEIBC_BLOCK_CMD_REMOTE				= 0x20,
	NVMEIBC_BLOCK_CMD_REMOTE_GOT_CH			= 0x21,
	NVMEIBC_BLOCK_CMD_REMOTE_ADD_PENDING	= 0x22,
	NVMEIBC_BLOCK_CMD_REMOTE_PENDING_FAILED	= 0x23,
	NVMEIBC_BLOCK_CMD_REMOTE_PENDING_DROP	= 0x24,
	NVMEIBC_BLOCK_CMD_COMPLETED				= 0x25,

	/* No-RDDA */
	NVMEIBC_BLOCK_CMD_NORDDA_EXECUTE 		= 0x30,
	NVMEIBC_BLOCK_CMD_NORDDA_SENT			= 0x31,

	/* RDDA */
	NVMEIBC_BLOCK_CMD_RDDA_EXECUTE			= 0x40,
	NVMEIBC_BLOCK_CMD_RDDA_RECV_COMP		= 0x41,
	NVMEIBC_BLOCK_CMD_RDDA_RECV_COMP_OE		= 0x42,
	NVMEIBC_BLOCK_CMD_RDDA_OE_CONT_WAIT		= 0x43,
	NVMEIBC_BLOCK_CMD_RDDA_OE_FINISH_READ	= 0x44,
};

static inline const char *nvmeibc_block_cmd_status_to_str(
	enum nvmeibc_block_cmd_status sts)
{
	switch (sts) {
	case NVMEIBC_BLOCK_CMD_INIT                  : return "INIT";
	case NVMEIBC_BLOCK_CMD_LOCAL                 : return "LOCAL";
	case NVMEIBC_BLOCK_CMD_LOCAL_MD_TRIM_RD_COMP : return "LOCAL_MD_TRIM_RD_COMP";
	case NVMEIBC_BLOCK_CMD_LOCAL_COMPLETED       : return "LOCAL_COMPLETED";
	case NVMEIBC_BLOCK_CMD_REMOTE                : return "REMOTE";
	case NVMEIBC_BLOCK_CMD_REMOTE_GOT_CH         : return "REMOTE_GOT_CH";
	case NVMEIBC_BLOCK_CMD_REMOTE_ADD_PENDING    : return "REMOTE_ADD_PENDING";
	case NVMEIBC_BLOCK_CMD_REMOTE_PENDING_FAILED : return "REMOTE_PENDING_FAILED";
	case NVMEIBC_BLOCK_CMD_REMOTE_PENDING_DROP   : return "REMOTE_PENDING_DROP";
	case NVMEIBC_BLOCK_CMD_COMPLETED             : return "COMPLETED";
	case NVMEIBC_BLOCK_CMD_NORDDA_EXECUTE        : return "NORDDA_EXECUTE";
	case NVMEIBC_BLOCK_CMD_NORDDA_SENT           : return "NORDDA_SENT";
	case NVMEIBC_BLOCK_CMD_RDDA_EXECUTE          : return "RDDA_EXECUTE";
	case NVMEIBC_BLOCK_CMD_RDDA_RECV_COMP        : return "RDDA_RECV_COMP";
	case NVMEIBC_BLOCK_CMD_RDDA_RECV_COMP_OE     : return "RDDA_RECV_COMP_OE";
	case NVMEIBC_BLOCK_CMD_RDDA_OE_CONT_WAIT     : return "RDDA_OE_CONT_WAIT";
	case NVMEIBC_BLOCK_CMD_RDDA_OE_FINISH_READ   : return "RDDA_OE_FINISH_READ";
	default: return "???";
	}
}

	#define nvmeibc_block_cmd_status_debug(_blk_cmd_, _sts_) ({	\
		nflog("block_cmd @P: update status @INT->@INT\n", 		\
		_blk_cmd_, _blk_cmd_->iocmd_status, _sts_);				\
		_blk_cmd_->iocmd_status = _sts_;						\
	})
#else
	#define nvmeibc_block_cmd_status_debug(_blk_cmd_, _sts_) ({})
#endif

/**************************** Disk IO Command *********************************/
/* completion for a command submitted to a disk inside a controller host. */
struct nvmeibc_d_iocmd_comp {				    // Transport layer completion of command
	struct nvmeibc_block_command *cmd;          // Original command, Todo: Remove, use iocmd->disk_cmd->owner
	struct nvmeibc_cmd_lock *pigbck_lock;       // Reference to locks, which is viewed as piggyblack
	struct nvmeibc_d_rdma_comp pigbck_comp;		// Completion of piggybacked rdma (if applicable). Cmd to disk can fail while piggyback occcured and vice versa
	bool has_piggyback;							// Bool, is piggyback used
	volatile int comp_code;                     // Completion code. O - OK, Negative=driver (transport) error, Positive=NVME code
};

enum nvmeibc_disk_io_cmd_originator {
	NVMEIBC_DISK_IO_CMD_ORIG_NONE		= 0,
	NVMEIBC_DISK_IO_CMD_ORIG_IO			= 0x1,
	NVMEIBC_DISK_IO_CMD_ORIG_IO_READ	= 0x00 | NVMEIBC_DISK_IO_CMD_ORIG_IO,
	NVMEIBC_DISK_IO_CMD_ORIG_IO_WRITE	= 0x10 | NVMEIBC_DISK_IO_CMD_ORIG_IO,
	NVMEIBC_DISK_IO_CMD_ORIG_IO_DISCARD	= 0x20 | NVMEIBC_DISK_IO_CMD_ORIG_IO,

	NVMEIBC_DISK_IO_CMD_ORIG_RECOV		= 0x2,
};

static inline const char *nvmeibc_disk_io_cmd_originator_to_str(enum nvmeibc_disk_io_cmd_originator orig)
{
	switch (orig) {
	case NVMEIBC_DISK_IO_CMD_ORIG_NONE:			return "NONE";
	case NVMEIBC_DISK_IO_CMD_ORIG_IO_READ:		return "IO_READ";
	case NVMEIBC_DISK_IO_CMD_ORIG_IO_WRITE:		return "IO_WRITE";
	case NVMEIBC_DISK_IO_CMD_ORIG_IO_DISCARD:	return "IO_DISCARD";
	case NVMEIBC_DISK_IO_CMD_ORIG_RECOV:		return "RECOV";
	default:									return "???";
	}
}

static inline bool nvmeibc_disk_io_cmd_originator_is_io(enum nvmeibc_disk_io_cmd_originator orig)
{
	return ((orig & 0xf) == NVMEIBC_DISK_IO_CMD_ORIG_IO);
}

static inline bool nvmeibc_disk_io_cmd_originator_is_recov(enum nvmeibc_disk_io_cmd_originator orig)
{
	return ((orig & 0xf) == NVMEIBC_DISK_IO_CMD_ORIG_RECOV);
}

/* IO Command to a disk inside a controller host. Read/Write/Trim. Includes
   sg-list of data if needed, as well as optional piggybacks */
struct nvmeibc_disk_io_command {
	struct nvmeibc_disk_command disk_cmd;		// TBD: move to wrapper structure or remove 'io from this struct's name
	u64 req_id;                                	// Unique id for each request
	enum nvmeibc_disk_io_cmd_originator orig;	// Caller classification: IO/recovery
	struct nvmeibc_block_io_req *reqs;			// Array of IO requests (currently, limited to length=1 in the disk layer). Todo: Remove
	struct nvmeibc_block_io_req reqs1;			// Memory to which 'reqs' points. Effectively making it an array of length 1. n_req <= 1
	struct nvmeibc_disk *disk;					// On which disk this command runs
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	struct nvmeib_io_stats *v_disk_stats;	// Per volume disk stats
#endif
	struct nvmeibc_d_iocmd_comp comp;			// Each command has a completion. Allocate completion inside command and lower level software will fill it, and return it via callback.
	struct nvmeibc_b_rdma_piggyback {			// Piggyback RDMA action on disk access cmd. (View lock/ DB / Write binfo piggyback). Important! If failed can be retried directly not via piggyback and can point to different disk/server than the disk command
		void *handle;							// Transport layer handle of the disk segment on which this command happens (related to lock channel memory region)
		u64 addr;								// Address of the dirtybit / read lock / TxID. Can be different from the physical disk address of command (if command is to a journal, not to data)
	} lpb;
	#if defined(TAKE_STATS) || defined(MGMT_STATS)
		struct nvmeibc_stats io_stat;            // Don't touch, Used by low level layer for collecting IO statistics for a specific physical disk
	#endif

	/**************** DEBUG ********************/
#ifdef DEBUG_UNCOMPLETED
	struct list_head uncompleted_list_n;    // Linked List of uncompleted commands from the array above.
	enum nvmeibc_block_cmd_status iocmd_status;
	u32 sq_entry, sq_entries;
	u32 cq_entry, cq_entries;
	u32 wraparound;
#endif // DEBUG_UNCOMPLETED

#ifdef DEBUG_TRANSFERS
	atomic_t n_cb;							// Amount of callbacks a command received, used to detect double callback completion (leads to mem corurption and system crash)
#endif

};
/* Currently at most 1 type of piggyback is allowed on a command:
   1. Write dirty bit - happens before command is sent to disk
   2. View (read) lock after command data is read
   3. Write Transaction-id TxID before the command */
#define dp_cmds_pigbck_has_any(    iocmd)   ((iocmd)->comp.has_piggyback)
#define dp_cmds_get_pigbck_comp_dc(iocmd) (&((iocmd)->comp.pigbck_comp))
#define dp_cmds_add_generic_piggyback(iocmd) (iocmd)->comp.has_piggyback = true

#define get_rcookie_ptr(bcmd) (&((bcmd)->reqs1.ndb->rcookie))

static inline struct nvmeibc_disk_io_command * disk_to_block(struct nvmeibc_disk_command *d)
{
	return container_of(d, struct nvmeibc_disk_io_command, disk_cmd);
}

#endif
