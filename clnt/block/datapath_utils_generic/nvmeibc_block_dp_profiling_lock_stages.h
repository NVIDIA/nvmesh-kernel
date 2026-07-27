#ifndef NVMEIBC_BLOCK_DP_PROFILING_LOCK_STAGES_H
#define NVMEIBC_BLOCK_DP_PROFILING_LOCK_STAGES_H

#include "nvmeibc_block.h"
#include "block/datapath_utils_generic/profiling/nvmeibc_block_dp_profiling_generic.h"

static inline u8 nvmeibc_profiling_translate_rdma_intent(int /*enum nvmeibc_rdma_intent*/ intent)
{
	if (unlikely(intent == PROFILING_GET_N_STAGES))	// Allocation/Initialization
		return NVMEIBC_CMD_LOCK_READ_PB + 4;
	/* Unlock value post operation for profiler */
	#define NVMEIBC_CMD_UNLOCK_OWNER           (8)
	#define NVMEIBC_CMD_UNLOCK_COPY_OWNER      (9)

	// There are sparse values in enum nvmeibc_rdma_intent, we don't want to allocate holes in the profiler stats (it's an array), so the +X is just to make it aligned and consecutive.
	switch((enum nvmeibc_rdma_intent)intent) {
		case NVMEIBC_CMD_LOCK_UNLOCK: return NVMEIBC_CMD_LOCK_UNLOCK;
		case NVMEIBC_CMD_LOCK_OWNER: return NVMEIBC_CMD_LOCK_OWNER;
		case NVMEIBC_CMD_PREDISCARD: return NVMEIBC_CMD_PREDISCARD;
		case NVMEIBC_CMD_LOCK_COPY_OWNER: return NVMEIBC_CMD_LOCK_COPY_OWNER;
		case NVMEIBC_CMD_LOCK_READ_DR: return NVMEIBC_CMD_LOCK_READ_DR;
		case NVMEIBC_CMD_LOCK_READ_PB: return NVMEIBC_CMD_LOCK_READ_PB;
		case NVMEIBC_CMD_BLKSET_INFO_WR_DR: return NVMEIBC_CMD_LOCK_READ_PB + 1; // 6
		case NVMEIBC_CMD_BLKSET_INFO_WR_PB: return NVMEIBC_CMD_LOCK_READ_PB + 2; // 7
	}
	switch (intent) {
		case NVMEIBC_CMD_UNLOCK_OWNER: return NVMEIBC_CMD_LOCK_READ_PB + 3;
		case NVMEIBC_CMD_UNLOCK_COPY_OWNER: return NVMEIBC_CMD_LOCK_READ_PB + 4;
	}
	BUG();
	return 0;
}

static inline const char *nvmeibc_profiling_lock_stage2name(u8 stage)
{
	switch (stage) {
	case 0:	return "UNLOCK";
	case 1: return "LOCK_OWNER";
	case 2: return "PREDISCARD";
	case 3: return "LOCK_COPY_OWNER";
	case 4: return "LOCK_READ_DR";
	case 5: return "LOCK_READ_PB";
	case 6: return "BLKSET_INFO_WR_DR";
	case 7: return "BLKSET_INFO_WR_PB";
	case NVMEIBC_CMD_UNLOCK_OWNER: return "UNLOCK_OWNER";
	case NVMEIBC_CMD_UNLOCK_COPY_OWNER: return "UNLOCK_COPY_OWNER";
	default: BUG(); return "ERROR";
	}
}

/*
enum nvmeibc_rdma_intent __get_unlock_type(const enum nvmeibc_rdma_intent type)
{
	switch (type) {
	case NVMEIBC_CMD_LOCK_UNLOCK: return NVMEIBC_CMD_LOCK_UNLOCK;
	case NVMEIBC_CMD_LOCK_OWNER : return NVMEIBC_CMD_UNLOCK_OWNER;
	case NVMEIBC_CMD_LOCK_COPY_OWNER: return NVMEIBC_CMD_UNLOCK_COPY_OWNER;
	default: BUG();
	}
	return type;
}*/

#undef NVMEIBC_CMD_UNLOCK_OWNER
#undef NVMEIBC_CMD_UNLOCK_COPY_OWNER

#endif  // H beginning

