/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_TYPES_H
#define NVMEIB_TYPES_H

#include "nvmeib_shared.h"

#ifndef LOW_MEM
#define NVMEIB_KEEP_ALIVE_TO (6 * HZ)
#else
#define NVMEIB_KEEP_ALIVE_TO (40 * HZ)
#endif
/* keep alive */
struct nvmeib_keep_alive {
	u8 value;
	u32 lkey;
	u64 raddr;
	u32 size;
	u32 rkey;
};

#define NVMEIB_NUM_OF_GID_SECTIONS 8
#define NVMEIB_SIZE_OF_GID_SEC 04
#define NVMEIB_GID_SECTION_FORMAT "%04x"
#define NVMEIB_SIZE_OF_GID NVMEIB_NUM_OF_GID_SECTIONS * \
		NVMEIB_SIZE_OF_GID_SEC + \
		(NVMEIB_NUM_OF_GID_SECTIONS - 1)

/* I/O command code: IMPORTANT: this is not just enum but bit field!
   Separated to 6 different groups (5-bits), each of up to 8 commands (3 bits)*/
enum nvmeib_block_io_op {
	NVMEIB_BLOCK_IO_OP_NOP = 0, // Initialized, unused value
	NVMEIB_BLOCK_IO_OP_READ = 1, // Don't touch. Must start from 1
	NVMEIB_BLOCK_IO_OP_WRITE = 2,
	NVMEIB_BLOCK_IO_OP_DISCARD = 3, // a.k.a TRIM

	// 0x4 Spare IO operations
	NVMEIB_BLOCK_IO_OP_WRITE_UNCOR = 5, /* Inject NVME error into to block*/
	NVMEIB_BLOCK_IO_OP_DPLIB_PROBLEM = 6, // Represents the problem that sync library has to solve for external user
	NVMEIB_BLOCK_IO_OP_RECOV_PROBLEM = 7, // Represents the problem that recovery has to solve

	/* Sync operations: 0x8-0xF */
	NVMEIB_BLOCK_IO_OP_RECOVER_STALE =        0x0|0x8,                              // 0x8 EC/R1 - Fix stale lock
	NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL =     0x1|NVMEIB_BLOCK_IO_OP_RECOVER_STALE,	// 0x9 EC/R1 - Fix bad sectors (unreadable blocks), acts as scrubbing
	NVMEIB_BLOCK_IO_OP_RECOVER_DB =           0x2|NVMEIB_BLOCK_IO_OP_RECOVER_STALE, // 0xA EC Only, Turn off some dirtybits
	NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK =     0x3|NVMEIB_BLOCK_IO_OP_RECOVER_STALE, // 0xB EC Only, Turn on/off some dbits and regen others
	NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON = 0x4|NVMEIB_BLOCK_IO_OP_RECOVER_STALE, // 0xC EC Only, Mark that we do not trust on disk dbits in parity
	NVMEIB_BLOCK_IO_OP_REC_COLD =             0x5|NVMEIB_BLOCK_IO_OP_RECOVER_STALE, // 0xD EC Only, In R1 Toma does cold recovery
	NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO =     0x6|NVMEIB_BLOCK_IO_OP_RECOVER_STALE, // 0xE Only wrapper to NVMEIB_BLOCK_IO_OP_MAINTAIN_COMMIT_BINFO()
	NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_GC =       0x7|NVMEIB_BLOCK_IO_OP_RECOVER_STALE, // 0xF EC Only, Garbage collection of journals

	NVMEIB_BLOCK_IO_OP_REC_SPARE =            0x0|0x40,                             // 0x40-0x47.. Spare syncs
	NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE =  0x1|NVMEIB_BLOCK_IO_OP_REC_SPARE,     // 0x41 R1 Only, Just copy the stale lock to secondary, wihtout fixing blockset
	NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_HOT_GC =   0x2|NVMEIB_BLOCK_IO_OP_REC_SPARE,     // 0x42 EC Only - Garbage collect journal entries colliding with IO. Like fix stale lock, but with locks already taken.
	NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP =        0x3|NVMEIB_BLOCK_IO_OP_REC_SPARE,     // 0x43 EC Only,
	NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING =    0x4|NVMEIB_BLOCK_IO_OP_REC_SPARE,     // 0x44 - scrub the blockset and if there is a problem in the blockset try to fix it if possible.
	NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO =    0x5|NVMEIB_BLOCK_IO_OP_REC_SPARE,     // 0x45 - After cold recovery (binfo was lost), fix it.
	NVMEIB_BLOCK_IO_OP_LAST_SYNC =            0xF|NVMEIB_BLOCK_IO_OP_REC_SPARE, // 0x4F EC/R1 - Last sync operation, used to detect if there is a new sync operation in the future.
	// If very much needed can use 0x48-0x4F. Total reserved 16 places for new syncs

	/* Blockset info maintanacne operations - state machine which doesn't change the data so it doesn't need to take locks */
	NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO =  0x10,									      // 0x10 EC Only,
	NVMEIB_BLOCK_IO_OP_MAINTAIN_COMMIT_BINFO =  0x3|NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO, // 0x13 EC/R1, Internally launched by other syncs
	NVMEIB_BLOCK_IO_OP_MAINTAIN_BLKSET_RECOV =  0x4|NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO, // 0x14 EC Only, Send msg to Toma/Serjio that blockset was fixed, Internally launched by other syncs
	NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB =   0x5|NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO, // 0x15 R1 Only, Convert stale lock to dirtybit in degraded mode

	/* Metadata only operations */
	NVMEIB_BLOCK_IO_OP_MD_READ =               0x20,	// Read by server from RAM
	NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR =           0x1|NVMEIB_BLOCK_IO_OP_MD_READ,	// 0x21, On TxID wraparound (Server side no-rdda read-modify-write op)

	NVMEIB_BLOCK_IO_ILLEGAL_DBG	= 0xFF, /* Debug Value */
};

enum {
	NVMEIB_BLOCK_IO_OP_MAX_SYNC_TYPES = NVMEIB_BLOCK_IO_OP_LAST_SYNC - NVMEIB_BLOCK_IO_OP_RECOVER_STALE + 1
};
#define SYNC_TYPE_ID(n) ((n) - NVMEIB_BLOCK_IO_OP_RECOVER_STALE)
#define SYNC_IO_OP(n) ((enum nvmeib_block_io_op)((n) + NVMEIB_BLOCK_IO_OP_RECOVER_STALE))

static inline const char *nvmeib_block_io_op_str(const enum nvmeib_block_io_op op)
{
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_NOP: return "NVMEIB_BLOCK_IO_OP_NOP";
	case NVMEIB_BLOCK_IO_OP_READ: return "NVMEIB_BLOCK_IO_OP_READ";
	case NVMEIB_BLOCK_IO_OP_WRITE: return "NVMEIB_BLOCK_IO_OP_WRITE";
	case NVMEIB_BLOCK_IO_OP_DISCARD: return "NVMEIB_BLOCK_IO_OP_DISCARD";

	case NVMEIB_BLOCK_IO_OP_WRITE_UNCOR: return "NVMEIB_BLOCK_IO_OP_WRITE_UNCOR";
	case NVMEIB_BLOCK_IO_OP_DPLIB_PROBLEM: return "NVMEIB_BLOCK_IO_OP_DPLIB_PROBLEM";
	case NVMEIB_BLOCK_IO_OP_RECOV_PROBLEM: return "NVMEIB_BLOCK_IO_OP_RECOV_PROBLEM";

	case NVMEIB_BLOCK_IO_OP_RECOVER_STALE: return "NVMEIB_BLOCK_IO_OP_RECOVER_STALE";
	case NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL: return "NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL";
	case NVMEIB_BLOCK_IO_OP_RECOVER_DB: return "NVMEIB_BLOCK_IO_OP_RECOVER_DB";
	case NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK: return "NVMEIB_BLOCK_IO_OP_RECOVER_ROLLBACK";
	case NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING: return "NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING";
	case NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON: return "NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON";
	case NVMEIB_BLOCK_IO_OP_REC_COLD: return "NVMEIB_BLOCK_IO_OP_REC_COLD";
	case NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO: return "NVMEIB_BLOCK_IO_OP_REC_COMMIT_BINFO";
	case NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_GC: return "NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_GC";

	case NVMEIB_BLOCK_IO_OP_REC_SPARE: return "NVMEIB_BLOCK_IO_OP_REC_SPARE";
	case NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE: return "NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE";
	case NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_HOT_GC: return "NVMEIB_BLOCK_IO_OP_REC_EC_JOUR_HOT_GC";
	case NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP: return "NVMEIB_BLOCK_IO_OP_REC_TXID_WRAP";
	case NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO: return "NVMEIB_BLOCK_IO_OP_FIX_UNKNOWN_BINFO";

	case NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO: return "NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO";
	case NVMEIB_BLOCK_IO_OP_MAINTAIN_COMMIT_BINFO: return "NVMEIB_BLOCK_IO_OP_MAINTAIN_COMMIT_BINFO";
	case NVMEIB_BLOCK_IO_OP_MAINTAIN_BLKSET_RECOV: return "NVMEIB_BLOCK_IO_OP_MAINTAIN_BLKSET_RECOV";
	case NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB: return "NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB";

	case NVMEIB_BLOCK_IO_OP_MD_READ: return "NVMEIB_BLOCK_IO_OP_MD_READ";
	case NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR: return "NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR";

	case NVMEIB_BLOCK_IO_ILLEGAL_DBG: return "NVMEIB_BLOCK_IO_ILLEGAL_DBG";
	case NVMEIB_BLOCK_IO_OP_LAST_SYNC: return "NVMEIB_BLOCK_IO_OP_LAST_SYNC";
	/**IMPORATNT: Do not use default clase here, must handle ALL possible values**/
	}
	return "???";
}

static inline bool nvmeib_block_io_op_is_read(enum nvmeib_block_io_op op){
	return op == NVMEIB_BLOCK_IO_OP_READ || op == NVMEIB_BLOCK_IO_OP_MD_READ;
}

static inline bool nvmeib_block_io_op_is_write(enum nvmeib_block_io_op op){
	return op == NVMEIB_BLOCK_IO_OP_WRITE;
}

#define  nvmeib_block_io_op_is_rw_op(op) ((op == NVMEIB_BLOCK_IO_OP_READ) || (nvmeib_block_io_op_is_write(op)))
#define  nvmeib_block_io_op_is_bio_op(op) (nvmeib_block_io_op_is_rw_op(op) || (op == NVMEIB_BLOCK_IO_OP_DISCARD))

static inline bool nvmeib_do_i_care_about_broken_binfo(enum nvmeib_block_io_op op)
{
	return !((op == NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON) ||		// I add even more problems, dont have to solve anything
		     (op == NVMEIB_BLOCK_IO_OP_REC_COLD) ||					// I expect it to be broken, binfo is irrelevant to me, I am initializing it
			 (op == NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO)); // I am solving this specific problem, dont care about other unrelated
			//op == NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE			// Also doesnt care, but it is in R1 only and this func is not used by R1
}

enum channel_type {
	ct_base = 0,
	ct_admin,
	ct_local,
	ct_rdda,
	ct_n_rdda,
	ct_m_rdda,
	ct_lock,
	ct_lock_2nd,

	ct_other,
	ct_end
};

static inline const char * ch_type_to_str(enum channel_type ct)
{
	switch (ct) {
		case ct_base: return "B";
		case ct_admin: return "A";
		case ct_local: return "C"; /* converged, L was taken :) */
		case ct_rdda: return "R";
		case ct_n_rdda: return "N";
		case ct_m_rdda: return "M";
		case ct_lock: return "L";
		case ct_lock_2nd: return "2";
		case ct_other: return "O";
		default: return "???";
	};
}

static inline const char * ch_type_to_str_long(enum channel_type ct)
{
	switch (ct) {
		case ct_base: return "Base";
		case ct_admin: return "Admin";
		case ct_local: return "Local";
		case ct_rdda: return "RDDA";
		case ct_n_rdda: return "NoRDDA";
		case ct_m_rdda: return "M";
		case ct_lock: return "Lock";
		case ct_lock_2nd: return "Lock2nd";
		case ct_other: return "Other";
		default: return "???";
	};
}

struct nvmeib_msg_to_process {
	/* the pid of the user mode process */
	int pid;
	/* the data to pass to the process - it will be copied */
	void *buf;
	/* the data len */
	int buf_len;
	/* optional callback to be called after the actual send to process */
	void (*on_send_comp)(void *ctx, int error);
	/* the context to pass to the completion callback */
	void *ctx;
	/* optional a callback to call to free the data buffer */
	void (*free_buf)(void *buf);
};
typedef int nvmeib_send_msg_to_process_t(struct nvmeib_msg_to_process *msg);

#endif //_NVMEIB_TYPES_

