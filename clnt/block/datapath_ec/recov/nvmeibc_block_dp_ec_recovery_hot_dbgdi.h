#pragma once

#include "../../datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
#include "../../datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.h"

#ifdef DBGDI_REMOVED_IN_PRODUCTION
	#define data_blk_fill_for_hot_rec(...)
#else
static inline const char* nvmeibc_dp_recovery_hot_roll_fwd_reason_to_string(u8 value)
{
	static const char* values[] = {"READ_JBLK_ERR", "W_SEGS", "W_PARITY_SEGS", "ROLL_FWD", "<unknown>"};
	int index = (int)value < 5 ? value : 4;
	return values[index];
}

static inline void __data_blk_fill_for_hot_rec(struct nvmeibc_dp_recovery_hot_dbgdi *hot,
											 enum nvmeibc_dp_recovery_hot_roll_fwd_reason reason,
											 const bool is_cold)
{
	if (hot->magic == NVMEIBC_DP_HOT_RCVR_MAGIC) {
		// Already wrote HTR info? should overwrite?
	}
	hot->magic = NVMEIBC_DP_HOT_RCVR_MAGIC;
	hot->reason = reason;
	hot->is_cold = is_cold;
	hot->reserved = 0; // Attempt to ensure we don't access this partial data without init
}

void *dp_dbgdi_get_recovery_hot_area(void *data);
void dp_dbgdi_data_blk_check_and_init_v(void *d);

static inline void data_blk_fill_for_hot_rec(void *data, enum nvmeibc_dp_recovery_hot_roll_fwd_reason reason, const bool is_cold)
{
	struct nvmeibc_dp_recovery_hot_dbgdi *hot;
	struct nvmeibc_dp_recovery_hot_dbgdi __hot;
	struct dbgdi_log *log;

	hot = &__hot;
	__data_blk_fill_for_hot_rec(hot, reason, is_cold);
	log = (struct dbgdi_log *)dp_dbgdi_get_recovery_hot_area(data);
	dp_dbgdi_data_blk_check_and_init_v((void *)data);
	dbgdi_log_add_rec(log, hot, DBG_DI_RECOVER_HOT, DBG_DI_RECOVER_HOT_REC_SIZE);
}
#endif	// DBGDI_REMOVED_IN_PRODUCTION

