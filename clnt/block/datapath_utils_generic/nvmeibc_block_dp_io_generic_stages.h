#ifndef NVMEIBC_DP_IO_GENERIC_STAGES_H
#define NVMEIBC_DP_IO_GENERIC_STAGES_H

#include "block/datapath_utils_generic/profiling/nvmeibc_block_dp_profiling_generic.h"

/* Generic state machine of the IO commands (execution plan).
   Raid5/6 - May use all stages, Reads of single block in non degraded mode
             use much fewer stages
   Raid1   - Excutes stages in between journals
   JBOD    - Has only one stage E_CMDS_STAGE_DO_IO_AND_PAR (No parity) */
enum e_cmds_stage {
	// 0. Not a real values, but special stages for profiler
	E_CMDS_STAGE_WAIT_FOR_LOCK  = -4,	// Used to monitor the time command waited for lock

	// 1. Special Stage for reading journals.
	E_CMDS_STAGE_READ_JOURNAL   = -3,	// Used only in stale lock fixup

	// 1. Stage of execution of Blockset-info resolve
	E_CMDS_STAGE_BLOCKSET_FIXUP = -2,	// Only for EC: Fixup missing or problematic data in the blockset info prior to issuing IO (while holding locks)
										// ^^^^^^^^^^^ not true, also used to measure lock transfer period.

	// 1. Stage for EC only can be run in parallel with the next stage
	E_CMDS_STAGE_JOURNAL_ALLOC  = -1,   // Used only for profiling allocation of journal stages

	// 2. Stages of Execution of IO transaction
	E_CMDS_STAGE_READ_PRE_DATA  = 0,	// Do degraded read or read old data & old parity for calcualtion of parity for write operation, must be 0
	E_CMDS_STAGE_CALC_PARITIES  = 1,	// A stage without real commands, only erasure coding (xor) calculation of data from previous stage. Auto advance to next stage
	E_CMDS_STAGE_WRITE_JOURNAL  = 2,	// Write Journal (data and parity of IO once (not overwriting the old data) to avoid write hole).
	E_CMDS_STAGE_POST_JR_RDMA   = 3,	// A stage without real commands, do RDMA appendix to cmds of previous stage (write blockset-info to owner lock if no command went to that segment (TxID could not be piggybacked))
	E_CMDS_STAGE_DO_IO_AND_PAR  = 4,	// Note: Reads in non degraded mode, do not require lock and start from this stage
	E_CMDS_STAGE_POST_IO_RDMA   = 5,	// A stage without real commands, do RDMA described in cmds of previous stage (write blockset-info or dirtybits or view lock for unlocked reads)
	E_CMDS_STAGE_DELETE_JOURNAL = 6,	// A stage without real commands, only localy free resources of journal (if journal was used).
	E_CMDS_STAGE_CALC_DEG_DATA  = 7,	// Degraded reads can calculate data only after all available data has been read
	//E_CMDS_STAGE_LAST_RAID5     = 7,	// Last stage for raid-56
} __attribute__ ((packed));

static inline const char* nvmeibc_e_cmds_stage_to_str(enum e_cmds_stage stage){
	switch(stage){
		case(E_CMDS_STAGE_WAIT_FOR_LOCK): return "WAIT_FOR_LOCK";
		case(E_CMDS_STAGE_READ_JOURNAL): return "READ_JOURNAL";
		case(E_CMDS_STAGE_BLOCKSET_FIXUP): return "BLOCKSET_FIXUP";
		case(E_CMDS_STAGE_JOURNAL_ALLOC): return "JOURNAL ALLOCATION";
		case(E_CMDS_STAGE_READ_PRE_DATA): return "READ_PRE_DATA";
		case(E_CMDS_STAGE_CALC_PARITIES): return "CALC_PARITIES";
		case(E_CMDS_STAGE_WRITE_JOURNAL): return "WRITE_JOURNAL";
		case(E_CMDS_STAGE_POST_JR_RDMA): return "POST_JR_RDMA";
		case(E_CMDS_STAGE_DO_IO_AND_PAR): return "DO_IO_AND_PAR";
		case(E_CMDS_STAGE_POST_IO_RDMA): return "POST_IO_RDMA";
		case(E_CMDS_STAGE_DELETE_JOURNAL): return "DELETE_JOURNAL";
		case(E_CMDS_STAGE_CALC_DEG_DATA): return "CALC_DEG_DATA";
	}
	return "unknown";
};

static inline u8 nvmeibc_profiling_translate_e_cmds_stage_ec(int /*enum e_cmds_stage*/ cmd_stage) {
	if (unlikely(cmd_stage == PROFILING_GET_N_STAGES)){
		return E_CMDS_STAGE_CALC_DEG_DATA + 5;
	}
	return cmd_stage + 4;
}

static inline const char* nvmeibc_profiling_e_cmds_stage_stage2name_ec(u8 /*enum profiling_cmds_stages*/ prof_stage){
	enum e_cmds_stage stage = (s16)prof_stage - 4;
	return nvmeibc_e_cmds_stage_to_str(stage);
};

static inline u8 nvmeibc_profiling_translate_e_cmds_stage_mirror(int /*enum e_cmds_stage*/ cmd_stage) {
	if (unlikely(cmd_stage == PROFILING_GET_N_STAGES)){
		return 5;
	}
	switch((enum e_cmds_stage)cmd_stage){
	case(E_CMDS_STAGE_READ_PRE_DATA): return 0;
	case(E_CMDS_STAGE_DO_IO_AND_PAR): return 1;
	case(E_CMDS_STAGE_POST_IO_RDMA): return 2;
	case(E_CMDS_STAGE_WAIT_FOR_LOCK): return 3;
	case(E_CMDS_STAGE_BLOCKSET_FIXUP): return 4;
	default: BUG(); return 0;
	}
}

static inline const char* nvmeibc_profiling_e_cmds_stage_stage2name_mirror(u8 stage){
	switch(stage){
		case(0): return nvmeibc_e_cmds_stage_to_str(E_CMDS_STAGE_READ_PRE_DATA);
		case(1): return nvmeibc_e_cmds_stage_to_str(E_CMDS_STAGE_DO_IO_AND_PAR);
		case(2): return nvmeibc_e_cmds_stage_to_str(E_CMDS_STAGE_POST_IO_RDMA);
		case(3): return nvmeibc_e_cmds_stage_to_str(E_CMDS_STAGE_WAIT_FOR_LOCK);
		case(4): return nvmeibc_e_cmds_stage_to_str(E_CMDS_STAGE_BLOCKSET_FIXUP);
		default: BUG(); return "unknown";
	}
};

static inline u8 nvmeibc_profiling_translate_e_cmds_stage_carrier(int /*enum e_cmds_stage*/ cmd_stage) {
	if (unlikely(cmd_stage == PROFILING_GET_N_STAGES)){
		return 1;
	}
	return 0;
}

static inline const char* nvmeibc_profiling_e_cmds_stage_stage2name_carrier(u8 stage){
	(void)stage;
	return "total";
};

static inline const char* nvmeibc_profiling_e_preparation_stage_stage2name(u8 stage){
	(void)stage;
	BUG();
	return "Preparation Stage Total";
}

static inline u8 nvmeibc_profiling_translate_e_preparation_stage(int /*enum e_cmds_stage*/ cmd_stage) {
	BUG_ON(cmd_stage != PROFILING_GET_N_STAGES);
	return 0;
}

#endif  // H beginning

