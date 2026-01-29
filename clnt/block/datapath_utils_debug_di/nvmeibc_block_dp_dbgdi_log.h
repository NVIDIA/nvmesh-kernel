/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef __DB_LOG_H__
#define __DB_LOG_H__

#include "../../../clnt/nvmeibc_core_dbgdi_blk.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_defs.h"

struct t_dont_touch_original {
	u64 data;						// Unique data of each block given by user space app.
	u64 aux[63];					// Additional place holder for user app (for future usage)
};

struct dbgdi_log_header {
	u32 magic;
	u32 head;
	u32 tail;
	u32 size;
	u16 full:1;
	u16 over:1;
	u16 reserved:14;
	u16 last_rec_type;
};

#ifdef DBGDI_REMOVED_IN_PRODUCTION
//no special meaning, just allow the compilation in this mode; the feature is anyway compiled out
#define DBG_DI_INJ_SPACE (NVMEIBC_SECTOR_SIZE - \
	(sizeof(struct t_dont_touch_original) + sizeof(struct dbgdi_log_header)))
#else
#define DBG_DI_INJ_SPACE (NVMEIBC_SECTOR_SIZE - \
	(sizeof(struct t_dont_touch_original) + sizeof(struct dbgdi_log_header) + sizeof(struct t_core_dbgdi)))
#endif

struct dbgdi_log {
	struct dbgdi_log_header header;
/* in our use-case, the log object will be always contained in the block and
 * the buffer over which records are written comes just after the log object
 */
	u8 buf[DBG_DI_INJ_SPACE];
};

/* every record in the log has this header and then their data */
struct dbgdi_log_entry {
	u16 type;
	u16 size;
};

#define db_entry(p) (struct dbgdi_log_entry *)p

/* initialize the log object using the provided buffer/size */
void dbgdi_log_init(struct dbgdi_log *log, int size);

/* returns true if the log object is initialized */
bool dbgdi_log_initialized(const struct dbgdi_log *log);

/* reset the internal pointers of the log object such that it's considered empty */
void dbgdi_log_reset(struct dbgdi_log *log);

/* adds a record at the head of the log using the provided params to set
 * a <type, size> header at the begining of the provided buffer.
 * The space needed for header should be accounted in the size param --
 * hence the mininal size is four.
 *
 * If there currently there is not enough room in the log buffer for this record, older
 * records are deleted to make room.
 *
 * If the buffer is bigger than the internal log buffer or too small
 * to contain the <type, value> header error (-1) is returned, otherwize success (0).
 *
 * --- to be used by debug DI data path code ---
 */
int dbgdi_log_add_rec(struct dbgdi_log *log, void *buf, u16 type, u16 size);

/* copies a record from the log residing in the given location to the
 * provided buffer. If the input buffer is too small to contain the record,
 * error (-1) is returned otherwise success (0).
 *
 * --- to be used by debug DI parsing code ---
 */
int dbgdi_log_get_rec(const struct dbgdi_log *log, int loc, void *buf, int buf_size);

/* returns how much of the log is occupied  */
int dbgdi_log_occupancy(const struct dbgdi_log *log);

/* returns how much free space the log has */
int dbgdi_log_free_space(const struct dbgdi_log *log);

/* returns iterator (location) to the log tail, if the log is empty -1 is returned */
int dbgdi_log_iter_init(const struct dbgdi_log *log);

/* returns iterator (location) to the record next to the current iterator */
int dbgdi_log_iter_next(const struct dbgdi_log *log, int iter);

/* looks for a record by the provided type, return a copy in buf if the provided size is big enough */
int dbgdi_log_get_record_by_type(const struct dbgdi_log *log, int type, void *buf, int buf_size);
#endif
