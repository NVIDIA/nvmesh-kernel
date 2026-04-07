/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_DBGDI_BLK_SCHEME_H
#define NVMEIBC_DP_DBGDI_BLK_SCHEME_H

// Enum for documentation, regardless if debug-di is enabled or not
enum nvmeibc_dp_recovery_hot_roll_fwd_reason {
	NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_READ_JBLK_ERR = 0,
	NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_W_SEGS = 1,
	NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_W_PARITY_SEGS = 2,
	NVMEIBC_DP_RECOVERY_HOT_ROLL_FWD_REASON_ROLL_FWD = 3
};

#ifdef DBGDI_REMOVED_IN_PRODUCTION
	typedef long long int data_blk;
	#define data_blk_get_injection_size()  (0)
#else

/* Defines the injected data block. This header can be compiled for kernel to
   define how IO is injected and to third party user space apps to parse the
   binary block */

#include "nvmeibc_block_dp_dbgdi_log.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"

#define DBG_DI_MAGIC_WR (0x6b6f5f72576b6c42LL) // "BlkWr_ok" Magic value to make sure that dbg_di injects data into writes
#define DBG_DI_MAGIC_WX (0x6f585f72576b6c42LL) // "BlkWr_Xo" Magic value to make sure that dbg_di marks restored    writes
#define DBG_DI_MAGIC_WP (0x766e4972576b6c42LL) // "BlkWrInv" Magic value to make sure that dbg_di marks invalid     writes (poisoned)
#define DBG_DI_MAGIC_WD (0x74734472576b6c42LL) // "BlkWrDst" Magic value to make sure that dbg_di marks Slice destruction
#define DBG_DI_MAGIC_RD (0x6b6f5f64526b6c42LL) // "BlkRd_ok" Magic value to make sure that dbg_di preconditions     reads
#define DBG_DI_MAGIC_RP (0x766e4964526b6c42LL) // "BlkRdInv" Magic value to make sure that dbg_di Detects unsent    reads (poisoned destination buffer for read)
#define DBG_DI_MAGIC_SY (0x5f636e79536b6c42LL) // "BlkSync_" Magic value to make sure that dbg_di injects data into syncs
#define DBG_DI_MAGIC_RF (0x66725f79536b6c42LL) // "BlkSy_rf" Magic value to make sure that dbg_di injects data into read fail sync
#define DBG_DI_MAGIC_SC (0x63735f79536b6c42LL) // "BlkSy_sc" Magic value to make sure that dbg_di injects data into scrubbing sync
#define DBG_DI_MAGIC_DB (0x62645f79536b6c42LL) // "BlkSy_db" Magic value to make sure that dbg_di injects data into dirty bit sync
#define DBG_DI_MAGIC_ST (0x74735f79536b6c42LL) // "BlkSy_st" Magic value to make sure that dbg_di injects data into stale lock recovery
#define DBG_DI_MAGIC_RB (0x62725f79536b6c42LL) // "BlkSy_rb" Magic value to make sure that dbg_di injects data into rollback recovery
#define DBG_DI_MAGIC_WA (0x61775f79536b6c42LL) // "BlkSy_wa" Magic value to make sure that dbg_di injects data into wraparound recovery
#define DBG_DI_MAGIC_MO (0x6b4f76744d6b6c42LL) // "BlkMtvOk" Magic value to make sure that dbg_di injects data into MTV
#define DBG_DI_MAGIC_IV (0x216e6f73696f5021LL) // "!Poison!" Magic value that marks that buffer is poisoned completely
#define NVMEIBC_DP_HOT_RCVR_MAGIC (0x686f7472637672LL)
/****************************** Assist structs ********************************/
struct t_db_who_clnt {
	char name[12];					// "nvme1038.XX" + '\0', who Issueed the io
	char version[12];				// Commit ID of the clients software
	//u32  pgid;					// Process Group id of the IO issuer
	//char proc_name[8];			// Name of the process.
	u64	 time_secs;					// Seconds since 1970 when IO started execution (write/sync) or completed (read)
} __attribute__ ((packed));

#define PACKED_LBA_BITS_VCR (32)	// Support volume of up to 4G x NVMEIBC_SECTOR_SIZE[bytes], vlba,clba,rlba
#define PACKED_LBA_BITS_D   (40)	// dlba on disk support up to 2^40 x NVMEIBC_SECTOR_SIZE[bytes]
struct t_db_who_bio {
	char volname[16];					// 15 letters + \'0' Of device name
	u64 start_vlba : PACKED_LBA_BITS_VCR;
	u64 nlbas : 24;						// Length of IO
	u64 op : 8;							// Type of the operation. packed enum nvmeib_block_io_op
} __attribute__ ((packed));

struct t_db_who_edic {
	struct dbgdi_log_entry e;
	u64 magic;
	u64 rlba : PACKED_LBA_BITS_VCR;
	u32 read_crc;
	u32 calc_crc;
	char edic_res;						// == edic check result of last read OP: == enum edic_result
} __attribute__ ((packed));

struct t_db_who_locks {  // description of lock scheme mapping of segment.
	u32 owner_si : 4;    // Index of owner segment. Always exists
	u32 second_si : 4;   // Index of second owner or -1 if does not exists
	u32 second_type : 4; // enum NVMEIBTC_DS_OWNER_MODE, Type of second lock (Secondary owner / copy-own / active, no owner)
	u32 third_si : 4;    // Index of third owner or -1 if does not exist
	u32 third_type : 4;
	u32 reserved : 4;
} __attribute__((packed));

struct t_db_who_seg {				// 12[bytes]
	u8 uuid[9];						// 8 first bytes of UUID + \x0
	u8 unused      : 3;				//
	u8 sync_safety     : 2;			// enum NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING
	u8 toma_acm        : 3;    		// enum NVMEIBTC_DS_MODE
	struct t_db_who_locks locks;	// description of lock scheme mapping of segment.
} __attribute__ ((packed));

struct t_db_who_praid {
	u32 lock_id;             		// lock-id (generated by toma) - written into locks (toma can know which client locked the memory). 0=unlocked value. Unique for each raid1 (in each block device in each client).
	u32 version    : 24;			// Unique id for multiple segments which mirror each other. Starts from 1 and grows to infinity
	u32 n_segs     : 4;				// Supports N replicas, D+P raid
	u32 slice_size : 4;				// Size in blocks of a single slice in protection raid, For raid1 it is 1, for raid5 - the amount of D segments. We write exactly one block in each segment in a slice to minimize the amount of parities written
} __attribute__ ((packed));

struct t_db_who_topo {
	u32 my_index       : 32;		// Current topology index
	u32 diff_head      :  8;		// Head-my_index: Every topology gets a monotonically increasing index.
	u32 diff_last_freed: 12;		// my_index-last_freed. Ever increasing index (above) that was already freed. (-1 ..-N) from the head topology.
	u32 num_io_toggles : 12;		// Amount of times io was disabled and enabled
} __attribute__ ((packed));

struct t_db_who_cmd {
	u64 req_id;                     // Unique id for each request
	u64 first_rlba  : PACKED_LBA_BITS_VCR;	// The rlba of the operation
	u64 disk_address: PACKED_LBA_BITS_D; // The remote disk address (in units of 4K blocks)
	u64 nlbas       : 24;			// Length of cmd in volume blocks
	u32 stage		: 8;			// The stage of my execution: == enum e_cmds_stage
	u32 do_not_send : 1;			// Command which is not send to remote disk but. Degraded read - data is calculated from parity. In degraded write, cmd might not be sent to disk but is needed for calculation of parity.
	u32 is_parity   : 1;			// Relevant for EC only, Is this command R/W parity (crucial coz, parity metadata is different from data md).
	u32 nlocks_take_before_cmd :3;	// Number of locks that had to be taken before this command could be executed [0..4]
	struct t_db_cmd_piggyback {
		u64 addr : PACKED_LBA_BITS_D; // Physical address on disk
		u8  op_type;				// enum nvmeibc_rdma_intent, 0xFF if irrelevant
		union {
			u32 val;				// Payload of the piggyback to write (dirtybit/ TxID)
			u32 retries;			// If op was a lock read, store the amount of retries that we did
		};
	} __attribute__ ((packed)) pigb;
} __attribute__ ((packed));

struct t_db_who_sgl {				// Offset of the current block in the command
	u32 sg_index        : 8;		// This block is in 'i'th entry in sgl <32 for 4K blocks, <256 for 512B blocks
	u32 sub_index       : 8;/*0..31*/	//   in ofset of 'sub_index' blocks
	u32 n_sg_elements   : 8;		// Number of elements in the sg list, <=32 for 4K blocks, <=256 for 512B blocks
	u32 offset_from_cmd : 8;		// Offset[blocks] of current sg entry from the start of the command
} __attribute__ ((packed));

struct t_journal_who {				// For erasure coding: Describes the journal of data blocks
	u64 disk_addr : PACKED_LBA_BITS_D;	// Disk lba of journal block which describes this data block. In units of 4K
	u64 reserved1 : 64-PACKED_LBA_BITS_D;
	u32 jri : NVMEIBC_DP_EC_DMD_BITS_JCI;					// Can be deduced from disk_addr
	u32 jblock: NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT;					//
	u32 action : 4;					// Cookies action can be 0 or SAVE
	u32 is_valid : 1;				// Valid
	u32 binje_shift: 3; // binje can be 1,2,4,8,16 so binje_shift can be 0,1,2,3,4 (3 bits)
	u32 reserved2: 32-NVMEIBC_DP_EC_DMD_BITS_JCI-NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT-5-3;
	u64 jour_md;					// 8 bytes: Metadata of journal block
} __attribute__ ((packed));

struct t_db_who_recovery {
	struct dbgdi_log_entry e;
	u64 magic;						// Know whether sync occured
	struct t_db_who_clnt clnt;		// Client which does the sync
	struct t_db_who_seg seg;
	struct t_db_who_praid raid;
	struct t_db_who_topo  topo;
	struct t_db_who_cmd   cmd;
} __attribute__ ((packed));

struct t_db_who_rb_recovery {
	struct t_db_who_recovery rec;
	u64 nwhole_params; 				// == union no_writehole_params
};

struct t_db_who_mtv_writer {
	u64 magic;
	struct t_db_who_clnt clnt;
	struct t_db_who_bio bio;
} __attribute__ ((packed));

struct t_db_who_mtv_reader {
	u64 magic;
	struct t_db_who_clnt clnt;
	struct t_db_who_bio bio;
} __attribute__ ((packed));

struct t_db_who_mtv_stage {
	u64 magic;
	struct t_db_who_clnt clnt;
} __attribute__ ((packed));

struct t_db_who_mtv_destage {
	u64 magic;
	struct t_db_who_clnt clnt;
} __attribute__ ((packed));

struct t_db_who_mtv {
	struct dbgdi_log_entry e;
	u64 magic;
	struct t_db_who_mtv_writer writer;
	struct t_db_who_mtv_reader reader;
	struct t_db_who_mtv_stage stager;
	struct t_db_who_mtv_destage destager;
} __attribute__ ((packed));

struct t_db_who_writer {
	struct dbgdi_log_entry e;
	u64 dbg_di_magic;				// Know whether data was injected
	struct t_db_who_clnt clnt;
	struct t_db_who_bio who_bio;
	struct t_db_who_seg seg;
	struct t_db_who_praid raid;
	struct t_db_who_topo  topo;
	struct t_db_who_cmd   cmd;
	struct t_db_who_sgl   sgl;
	struct t_journal_who jrnl;
	struct t_db_who_locks rlbalocks; // Locks info for given topology at given address
	u64 data_md;					// 8 bytes: Metadata of D/P block
} __attribute__ ((packed));

struct t_db_who_mark {
	struct dbgdi_log_entry e;
	u64 dbg_di_magic;
} __attribute__ ((packed));

struct t_db_who_reader {
	struct dbgdi_log_entry e;
	u64 magic;                        // Know whether injection of read occured
	struct t_db_who_clnt clnt;        // Client which does the sync
	struct t_db_who_bio who_bio;      // Read BIO information
	struct t_db_who_seg seg;          // Segment of the original command
	struct t_db_who_praid raid;       // Raid info
	struct t_db_who_topo topo;        // Topology on which sync arrived
	struct t_db_who_locks rlbalocks; // Locks info for given topology at given address
	struct t_db_who_cmd cmd;          // Original IO command which initiated the sync
	s32 comp_code : 16;               // Completion code of the command
	struct t_db_who_sgl sgl;          // Offset of the current block in the command
	u64 meta_data;                    // 8 bytes: Metadata of block
} __attribute__ ((packed));

struct t_db_who_sync {
	struct dbgdi_log_entry e;
	u64 magic;						// Know whether sync occured
	u8 op;							// Type of the sync. packed enum nvmeib_block_io_op
	u8 reason;						// Must not be zero!
	u8 source_seg;					// R1: Data was read from this segment. R5 - Segment of sync write cmd
	struct t_db_who_clnt clnt;		// Client which does the sync
	struct t_db_who_topo topo;		// Topology on which sync arrived
	struct t_db_who_praid raid;
	struct t_db_who_seg orig_seg;   // R1: Segment of the original command, R6 - Segment of sync write cmd
	struct t_db_who_cmd orig_rldr;	// R1: Original IO command which initiated the sync,  R6 - Sync write cmd
	u64 meta_data;					// 8 bytes: Metadata of block
} __attribute__ ((packed));

struct nvmeibc_dp_recovery_hot_dbgdi {		//preserves hot recovery slice roll fwd reasons;
	struct dbgdi_log_entry e;
	u64 magic;						//inidicator for hot recovery
	enum nvmeibc_dp_recovery_hot_roll_fwd_reason reason: 2;			//
	bool is_cold: 1;
	u8 reserved: 5;
} __attribute__ ((packed));


enum {
	DBG_DI_NONE = 0,
	DBG_DI_WRITE,
	DBG_DI_SYNC_OVERWRITTEN,
	DBG_DI_SYNC_OVERWRITTEN_CLEARED,
	DBG_DI_WRITE_RESTORED,
	DBG_DI_WRITE_DESTROYED,
	DBG_DI_READ,
	DBG_DI_EDIC,
	DBG_DI_SYNC,
	DBG_DI_RECOVER_READ_FAIL,
	DBG_DI_RECOVER_SCRUBBING,
	DBG_DI_RECOVER_DIRTY_BITS,
	DBG_DI_RECOVER_STALE_LOCK,
	DBG_DI_RECOVER_ROLLBACK,
	DBG_DI_RECOVER_TXID_WRAPAROUND,
	DBG_DI_MTV,
	DBG_DI_RECOVER_HOT,
};

// All structs are under 200 bytes, execpt MTV which more than 400
enum {
	DBG_DI_WRITE_REC_SIZE = sizeof(struct t_db_who_writer),
	DBG_DI_MARK_REC_SIZE = sizeof(struct t_db_who_mark),
	DBG_DI_READ_REC_SIZE = sizeof(struct t_db_who_reader),
	DBG_DI_EDIC_REC_SIZE = sizeof(struct t_db_who_edic),
	DBG_DI_SYNC_REC_SIZE = sizeof(struct t_db_who_sync),
	DBG_DI_RECOVER_REC_SIZE = sizeof(struct t_db_who_recovery),
	DBG_DI_RECOVER_RB_REC_SIZE = sizeof(struct t_db_who_rb_recovery),
	DBG_DI_RECOVER_HOT_REC_SIZE = sizeof(struct nvmeibc_dp_recovery_hot_dbgdi),
	DBG_DI_MTV_REC_SIZE = sizeof(struct t_db_who_mtv),
	DBG_DI_MAX_REC_SIZE = 512
};

/* Single nvmeblock structure as injected with debug data */
typedef struct t_data_blk { /* Single block, first 128 bytes are not changed */
	struct t_dont_touch_original dont_touch_original;
	struct dbgdi_log log;
	struct t_core_dbgdi core;
} data_blk;

static inline void nvmeibc_verify_data_blk(void){
	BUILD_BUG_ON(sizeof(data_blk) != NVMEIBC_SECTOR_SIZE);
	BUILD_BUG_ON(sizeof(struct t_dont_touch_original) % 8 != 0);
	BUILD_BUG_ON(sizeof(struct dbgdi_log) % 8 != 0);
}

#define data_blk_get_injection_size() (int)(DBG_DI_INJ_SPACE + sizeof(struct t_core_dbgdi))

#endif  // DBGDI_REMOVED_IN_PRODUCTION

#endif  // H beginning

